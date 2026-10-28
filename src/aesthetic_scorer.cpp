#include "aesthetic_scorer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <windows.h>

#include "json.hpp"
#include "nsfw_classifier.h" // DecodeImageFileRGBA

#define ORT_API_MANUAL_INIT
#include "onnxruntime_cxx_api.h"
#undef ORT_API_MANUAL_INIT

namespace fs = std::filesystem;

namespace {

struct OrtDmlApiPrefix {
    OrtStatus*(ORT_API_CALL* SessionOptionsAppendExecutionProvider_DML)(OrtSessionOptions* options, int device_id);
};

fs::path ExeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}

fs::path FindModel(const wchar_t* name) {
    const fs::path exeDir = ExeDir();
    for (const fs::path& dir : {exeDir / L"models", exeDir.parent_path().parent_path() / L"models"}) {
        std::error_code ec;
        const fs::path p = dir / name;
        if (fs::is_regular_file(p, ec)) return p;
    }
    return {};
}

float HalfToFloat(unsigned short h) {
    const unsigned sign = (h >> 15) & 1, exp = (h >> 10) & 31, man = h & 1023;
    float v;
    if (exp == 0) v = std::ldexp(static_cast<float>(man), -24);
    else if (exp == 31) v = man ? NAN : INFINITY;
    else v = std::ldexp(static_cast<float>(man + 1024), static_cast<int>(exp) - 25);
    return sign ? -v : v;
}

// Average of the source window [sx, sx+sw) x [sy, sy+sh) for every destination pixel (a box filter: shrinking big AI
// images by plain bilinear aliases), written as RGB planes in 0..1 into `out` (a dstW x dstH x 3 buffer, NCHW) at
// destination offset (ox, oy) of a canvas that is canvasW wide.
void SampleWindow(const unsigned char* rgba, int W, int H, float sx, float sy, float sw, float sh, int dstW, int dstH, float* out,
                  int canvasW, int canvasH, int ox, int oy) {
    const size_t plane = static_cast<size_t>(canvasW) * canvasH;
    for (int y = 0; y < dstH; ++y) {
        const float y0 = sy + y * sh / dstH, y1 = sy + (y + 1) * sh / dstH;
        const int iy0 = std::clamp(static_cast<int>(std::floor(y0)), 0, H - 1);
        const int iy1 = std::clamp(std::max(iy0 + 1, static_cast<int>(std::ceil(y1))), 1, H);
        for (int x = 0; x < dstW; ++x) {
            const float x0 = sx + x * sw / dstW, x1 = sx + (x + 1) * sw / dstW;
            const int ix0 = std::clamp(static_cast<int>(std::floor(x0)), 0, W - 1);
            const int ix1 = std::clamp(std::max(ix0 + 1, static_cast<int>(std::ceil(x1))), 1, W);
            float sum[3] = {0, 0, 0};
            for (int yy = iy0; yy < iy1; ++yy) {
                const unsigned char* row = rgba + (static_cast<size_t>(yy) * W + ix0) * 4;
                for (int xx = ix0; xx < ix1; ++xx, row += 4) {
                    sum[0] += row[0];
                    sum[1] += row[1];
                    sum[2] += row[2];
                }
            }
            const float n = static_cast<float>((ix1 - ix0) * (iy1 - iy0)) * 255.0f;
            const size_t pos = static_cast<size_t>(oy + y) * canvasW + (ox + x);
            for (int c = 0; c < 3; ++c) out[c * plane + pos] = sum[c] / n;
        }
    }
}

} // namespace

struct AestheticScorer::Impl {
    std::mutex loadMutex, runMutex;
    bool attempted = false, loaded = false;
    std::string error, device;

    std::unique_ptr<Ort::Env> env;
    std::unique_ptr<Ort::Session> clip, anime;
    std::string clipIn, clipOut, animeIn, animeOut;

    // from aesthetic.json
    std::vector<float> headW, animeVec, photoVec;
    std::vector<std::vector<float>> kindVecs; // one per style (may be empty: styles are then not reported)
    float headB = 0, logitScale = 100;
    int clipSize = 224, animeSize = 768;
    float mean[3] = {0.48145466f, 0.4578275f, 0.40821073f}, stdv[3] = {0.26862954f, 0.26130258f, 0.27577711f};

    bool Load();
    bool RunClip(std::vector<float>& input, std::vector<float>& embedding);
    bool RunAnime(std::vector<float>& input, float& score);
};

bool AestheticScorer::Impl::Load() {
    const fs::path clipPath = FindModel(L"aesthetic_clip.onnx"), animePath = FindModel(L"aesthetic_anime.onnx"), jsonPath = FindModel(L"aesthetic.json");
    if (clipPath.empty() || animePath.empty() || jsonPath.empty()) {
        error = "Quality models not found: put aesthetic_clip.onnx, aesthetic_anime.onnx and aesthetic.json in the models folder next to Try2Create.exe";
        return false;
    }
    {
        std::ifstream f(jsonPath, std::ios::binary);
        nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
        if (!j.is_object() || !j.contains("head_w") || !j.contains("anime_vec") || !j.contains("photo_vec")) {
            error = "aesthetic.json is not valid";
            return false;
        }
        headW = j["head_w"].get<std::vector<float>>();
        animeVec = j["anime_vec"].get<std::vector<float>>();
        photoVec = j["photo_vec"].get<std::vector<float>>();
        if (j.contains("kind_vecs") && j["kind_vecs"].is_array() && static_cast<int>(j["kind_vecs"].size()) == kStyleCount) {
            for (const auto& v : j["kind_vecs"]) kindVecs.push_back(v.get<std::vector<float>>());
            for (const auto& v : kindVecs) {
                if (v.size() != headW.size()) kindVecs.clear();
                if (kindVecs.empty()) break;
            }
        }
        headB = j.value("head_b", 0.0f);
        logitScale = j.value("logit_scale", 100.0f);
        clipSize = j.value("clip_size", 224);
        animeSize = j.value("anime_size", 768);
        if (j.contains("clip_mean") && j["clip_mean"].size() == 3) for (int i = 0; i < 3; ++i) mean[i] = j["clip_mean"][i].get<float>();
        if (j.contains("clip_std") && j["clip_std"].size() == 3) for (int i = 0; i < 3; ++i) stdv[i] = j["clip_std"][i].get<float>();
        if (headW.size() != animeVec.size() || headW.size() != photoVec.size() || headW.empty()) {
            error = "aesthetic.json has vectors of different sizes";
            return false;
        }
    }
    HMODULE ort = LoadLibraryExW((ExeDir() / L"onnxruntime.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!ort) {
        error = "onnxruntime.dll not found next to Try2Create.exe";
        return false;
    }
    using GetApiBaseFn = const OrtApiBase*(ORT_API_CALL*)();
    auto getApiBase = reinterpret_cast<GetApiBaseFn>(GetProcAddress(ort, "OrtGetApiBase"));
    const OrtApi* api = getApiBase ? getApiBase()->GetApi(ORT_API_VERSION) : nullptr;
    if (!api) {
        error = "onnxruntime.dll is an incompatible version";
        return false;
    }
    Ort::InitApi(api);
    try {
        env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_ERROR, "Try2CreateAesthetic");
        auto make = [&](const fs::path& path, bool dml, const std::vector<std::pair<std::string, int64_t>>& dims) {
            Ort::SessionOptions opts;
            opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            if (dml) {
                opts.DisableMemPattern();
                opts.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
                for (const auto& [name, value] : dims) opts.AddFreeDimensionOverrideByName(name.c_str(), value);
                const OrtDmlApiPrefix* d = nullptr;
                Ort::ThrowOnError(Ort::GetApi().GetExecutionProviderApi("DML", ORT_API_VERSION, reinterpret_cast<const void**>(&d)));
                if (!d) throw Ort::Exception("DirectML provider unavailable", ORT_FAIL);
                Ort::ThrowOnError(d->SessionOptionsAppendExecutionProvider_DML(opts, 0));
            } else {
                opts.SetIntraOpNumThreads(std::max(1u, std::thread::hardware_concurrency() / 2));
            }
            return std::make_unique<Ort::Session>(*env, path.c_str(), opts);
        };
        // symbolic dims of the CLIP model ("batch_size", "num_channels", ...) are pinned: DirectML needs fixed shapes
        const std::vector<std::pair<std::string, int64_t>> clipDims = {{"batch_size", 1}, {"num_channels", 3}, {"height", clipSize}, {"width", clipSize}};
        auto names = [&](Ort::Session& s, std::string& in, std::string& out) {
            Ort::AllocatorWithDefaultOptions alloc;
            in = s.GetInputNameAllocated(0, alloc).get();
            out = s.GetOutputNameAllocated(0, alloc).get();
        };
        bool dml = true;
        try {
            clip = make(clipPath, true, clipDims);
            anime = make(animePath, true, {});
            names(*clip, clipIn, clipOut);
            names(*anime, animeIn, animeOut);
            // some graphs load on DirectML and only fail on the first Run: find out now
            std::vector<float> w1(static_cast<size_t>(clipSize) * clipSize * 3, 0.0f), emb;
            if (!RunClip(w1, emb)) throw std::runtime_error("clip test run failed");
            std::vector<float> w2(static_cast<size_t>(animeSize) * animeSize * 3, 0.5f);
            float s;
            if (!RunAnime(w2, s)) throw std::runtime_error("anime test run failed");
        } catch (const std::exception&) {
            dml = false;
            clip = make(clipPath, false, {});
            anime = make(animePath, false, {});
            names(*clip, clipIn, clipOut);
            names(*anime, animeIn, animeOut);
        }
        device = dml ? "DirectML (GPU)" : "CPU";
    } catch (const std::exception& ex) {
        clip.reset();
        anime.reset();
        error = std::string("Failed to load the quality models: ") + ex.what();
        return false;
    }
    return true;
}

bool AestheticScorer::Impl::RunClip(std::vector<float>& input, std::vector<float>& embedding) {
    try {
        const std::vector<int64_t> shape = {1, 3, clipSize, clipSize};
        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value tensor = Ort::Value::CreateTensor<float>(mem, input.data(), input.size(), shape.data(), shape.size());
        const char* in[] = {clipIn.c_str()};
        const char* out[] = {clipOut.c_str()};
        std::vector<Ort::Value> outs;
        {
            std::lock_guard<std::mutex> lock(runMutex);
            outs = clip->Run(Ort::RunOptions{nullptr}, in, &tensor, 1, out, 1);
        }
        auto info = outs[0].GetTensorTypeAndShapeInfo();
        const size_t n = info.GetElementCount();
        embedding.resize(n);
        if (info.GetElementType() == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16) {
            const unsigned short* p = reinterpret_cast<const unsigned short*>(outs[0].GetTensorData<Ort::Float16_t>());
            for (size_t i = 0; i < n; ++i) embedding[i] = HalfToFloat(p[i]);
        } else {
            const float* p = outs[0].GetTensorData<float>();
            std::copy(p, p + n, embedding.begin());
        }
        return n == headW.size();
    } catch (const std::exception&) {
        return false;
    }
}

bool AestheticScorer::Impl::RunAnime(std::vector<float>& input, float& score) {
    try {
        const std::vector<int64_t> shape = {1, 3, animeSize, animeSize};
        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value tensor = Ort::Value::CreateTensor<float>(mem, input.data(), input.size(), shape.data(), shape.size());
        const char* in[] = {animeIn.c_str()};
        const char* out[] = {animeOut.c_str()};
        std::vector<Ort::Value> outs;
        {
            std::lock_guard<std::mutex> lock(runMutex);
            outs = anime->Run(Ort::RunOptions{nullptr}, in, &tensor, 1, out, 1);
        }
        score = outs[0].GetTensorData<float>()[0];
        return std::isfinite(score);
    } catch (const std::exception&) {
        return false;
    }
}

AestheticScorer::AestheticScorer() : impl_(std::make_unique<Impl>()) {}
AestheticScorer::~AestheticScorer() = default;

bool AestheticScorer::ModelsPresent() {
    return !FindModel(L"aesthetic_clip.onnx").empty() && !FindModel(L"aesthetic_anime.onnx").empty() && !FindModel(L"aesthetic.json").empty();
}

std::string AestheticScorer::ModelId() {
    std::string id;
    for (const wchar_t* name : {L"aesthetic_clip.onnx", L"aesthetic_anime.onnx", L"aesthetic.json"}) {
        const fs::path p = FindModel(name);
        std::error_code ec;
        id += std::to_string(p.empty() ? 0 : fs::file_size(p, ec)) + ":";
    }
    return id;
}

bool AestheticScorer::EnsureLoaded(std::string* error) {
    std::lock_guard<std::mutex> lock(impl_->loadMutex);
    if (!impl_->attempted) {
        impl_->attempted = true;
        impl_->loaded = impl_->Load();
    }
    if (!impl_->loaded && error) *error = impl_->error;
    return impl_->loaded;
}

bool AestheticScorer::IsLoaded() const {
    std::lock_guard<std::mutex> lock(impl_->loadMutex);
    return impl_->loaded;
}

std::string AestheticScorer::DeviceName() const {
    std::lock_guard<std::mutex> lock(impl_->loadMutex);
    return impl_->device;
}

bool AestheticScorer::ScoreRGBA(const unsigned char* rgba, int W, int H, Scores& out, std::string* error) {
    if (!IsLoaded() || W < 1 || H < 1) {
        if (error) *error = "Quality models not loaded";
        return false;
    }
    Impl& m = *impl_;
    out = Scores();

    // ---- CLIP: centre square, resized to 224, normalised ----
    {
        const int S = m.clipSize;
        std::vector<float> in(static_cast<size_t>(S) * S * 3);
        const float side = static_cast<float>(std::min(W, H));
        SampleWindow(rgba, W, H, (W - side) * 0.5f, (H - side) * 0.5f, side, side, S, S, in.data(), S, S, 0, 0);
        const size_t plane = static_cast<size_t>(S) * S;
        for (int c = 0; c < 3; ++c) {
            for (size_t i = 0; i < plane; ++i) in[c * plane + i] = (in[c * plane + i] - m.mean[c]) / m.stdv[c];
        }
        std::vector<float> e;
        if (!m.RunClip(in, e)) {
            if (error) *error = "The general model failed";
            return false;
        }
        double norm = 0;
        for (float v : e) norm += static_cast<double>(v) * v;
        norm = std::sqrt(norm);
        if (norm <= 0) return false;
        double general = m.headB, ca = 0, cp = 0;
        for (size_t i = 0; i < e.size(); ++i) {
            const double v = e[i] / norm;
            general += v * m.headW[i];
            ca += v * m.animeVec[i];
            cp += v * m.photoVec[i];
        }
        out.general = static_cast<float>(std::clamp(general, 0.0, 10.0));
        out.animeProb = static_cast<float>(1.0 / (1.0 + std::exp(-m.logitScale * (ca - cp))));
        if (!m.kindVecs.empty()) {
            double logits[kStyleCount], best = -1e30;
            int bestIdx = 0;
            for (int k = 0; k < kStyleCount; ++k) {
                double d = 0;
                for (size_t i = 0; i < e.size(); ++i) d += (e[i] / norm) * m.kindVecs[k][i];
                logits[k] = m.logitScale * d;
                if (logits[k] > best) {
                    best = logits[k];
                    bestIdx = k;
                }
            }
            double sum = 0;
            for (int k = 0; k < kStyleCount; ++k) sum += std::exp(logits[k] - best);
            out.style = bestIdx;
            out.styleConf = static_cast<float>(1.0 / sum);
        }
    }

    // ---- anime model: whole picture, fitted into 768 x 768 on a grey canvas ----
    {
        const int S = m.animeSize;
        int nw = S, nh = S;
        if (H > W) nw = std::max(1, static_cast<int>(static_cast<long long>(S) * W / H));
        else nh = std::max(1, static_cast<int>(static_cast<long long>(S) * H / W));
        std::vector<float> in(static_cast<size_t>(S) * S * 3, 0.5f);
        SampleWindow(rgba, W, H, 0, 0, static_cast<float>(W), static_cast<float>(H), nw, nh, in.data(), S, S, (S - nw) / 2, (S - nh) / 2);
        float s = 0;
        if (m.RunAnime(in, s)) out.anime = std::clamp(s, 0.0f, 1.0f) * 10.0f;
    }
    return out.general >= 0;
}

bool AestheticScorer::ScoreFile(const std::wstring& path, Scores& out, std::string* error) {
    std::vector<unsigned char> rgba;
    int w = 0, h = 0;
    if (!DecodeImageFileRGBA(path, rgba, w, h)) {
        if (error) *error = "Could not decode the picture";
        return false;
    }
    return ScoreRGBA(rgba.data(), w, h, out, error);
}
