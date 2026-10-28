#include "person_detector.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>
#include <windows.h>

#include "json.hpp"
#include "nsfw_classifier.h" // DecodeImageFileRGBA

// See nsfw_classifier.cpp: manual init lets us LoadLibrary onnxruntime.dll
// ourselves so a missing DLL is a soft failure.
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

fs::path FindModelPath() {
    fs::path exeDir = ExeDir();
    for (const fs::path& dir : {exeDir / L"models", exeDir.parent_path().parent_path() / L"models"}) {
        std::error_code ec;
        fs::path p = dir / L"people.onnx";
        if (fs::is_regular_file(p, ec)) return p;
    }
    return {};
}

struct Box {
    float x0, y0, x1, y1, score;
};

float IoU(const Box& a, const Box& b) {
    float ix = std::max(0.0f, std::min(a.x1, b.x1) - std::max(a.x0, b.x0));
    float iy = std::max(0.0f, std::min(a.y1, b.y1) - std::max(a.y0, b.y0));
    float inter = ix * iy;
    float uni = (a.x1 - a.x0) * (a.y1 - a.y0) + (b.x1 - b.x0) * (b.y1 - b.y0) - inter;
    return uni > 0.0f ? inter / uni : 0.0f;
}

// Letterboxes RGBA8 into an S x S planar float image (RGB, 0..1) padded with grey,
// area-averaging when shrinking and bilinear when enlarging. Returns the scale
// and padding used so boxes could be mapped back (only counts are needed here).
void Letterbox(const unsigned char* src, int srcW, int srcH, int S, std::vector<float>& out) {
    const float scale = std::min(static_cast<float>(S) / srcW, static_cast<float>(S) / srcH);
    const int newW = std::max(1, static_cast<int>(std::round(srcW * scale)));
    const int newH = std::max(1, static_cast<int>(std::round(srcH * scale)));
    const int padX = (S - newW) / 2, padY = (S - newH) / 2;
    const size_t plane = static_cast<size_t>(S) * S;
    out.assign(plane * 3, 114.0f / 255.0f);

    const bool shrink = srcW >= newW && srcH >= newH;
    for (int y = 0; y < newH; ++y) {
        for (int x = 0; x < newW; ++x) {
            float rgb[3] = {0, 0, 0};
            if (shrink) {
                int x0 = static_cast<int>(static_cast<long long>(x) * srcW / newW);
                int x1 = std::max(x0 + 1, static_cast<int>(static_cast<long long>(x + 1) * srcW / newW));
                int y0 = static_cast<int>(static_cast<long long>(y) * srcH / newH);
                int y1 = std::max(y0 + 1, static_cast<int>(static_cast<long long>(y + 1) * srcH / newH));
                float sum[3] = {0, 0, 0};
                for (int sy = y0; sy < y1; ++sy) {
                    const unsigned char* row = src + (static_cast<size_t>(sy) * srcW + x0) * 4;
                    for (int sx = x0; sx < x1; ++sx, row += 4) {
                        sum[0] += row[0]; sum[1] += row[1]; sum[2] += row[2];
                    }
                }
                float n = static_cast<float>((x1 - x0) * (y1 - y0));
                for (int c = 0; c < 3; ++c) rgb[c] = sum[c] / n;
            } else {
                float fx = std::clamp((x + 0.5f) / scale - 0.5f, 0.0f, static_cast<float>(srcW - 1));
                float fy = std::clamp((y + 0.5f) / scale - 0.5f, 0.0f, static_cast<float>(srcH - 1));
                int ix = static_cast<int>(fx), iy = static_cast<int>(fy);
                int ix1 = std::min(ix + 1, srcW - 1), iy1 = std::min(iy + 1, srcH - 1);
                float ax = fx - ix, ay = fy - iy;
                auto px = [&](int sx, int sy, int c) { return static_cast<float>(src[(static_cast<size_t>(sy) * srcW + sx) * 4 + c]); };
                for (int c = 0; c < 3; ++c) {
                    float top = px(ix, iy, c) * (1 - ax) + px(ix1, iy, c) * ax;
                    float bot = px(ix, iy1, c) * (1 - ax) + px(ix1, iy1, c) * ax;
                    rgb[c] = top * (1 - ay) + bot * ay;
                }
            }
            const size_t o = static_cast<size_t>(y + padY) * S + (x + padX);
            for (int c = 0; c < 3; ++c) out[c * plane + o] = rgb[c] / 255.0f;
        }
    }
}

} // namespace

struct PersonDetector::Impl {
    std::mutex loadMutex;
    bool loadAttempted = false;
    bool loaded = false;
    std::string loadError;
    std::string deviceName;

    std::unique_ptr<Ort::Env> env;
    std::unique_ptr<Ort::Session> session;
    std::mutex runMutex;
    std::string inputName, outputName;

    int inputSize = 640;
    float threshold = 0.324f;
    float nmsIou = 0.5f;
    int personClass = 0;

    bool Load(std::string* error);
    void ReadConfig(const fs::path& jsonPath);
    int Detect(std::vector<float>& input); // throws on failure
};

void PersonDetector::Impl::ReadConfig(const fs::path& jsonPath) {
    std::ifstream f(jsonPath, std::ios::binary);
    if (!f) return;
    nlohmann::json j = nlohmann::json::parse(f, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return;
    if (j.contains("input_size") && j["input_size"].is_number_integer()) inputSize = std::clamp(j["input_size"].get<int>(), 64, 2048);
    if (j.contains("threshold") && j["threshold"].is_number()) threshold = std::clamp(j["threshold"].get<float>(), 0.01f, 0.99f);
    if (j.contains("nms_iou") && j["nms_iou"].is_number()) nmsIou = std::clamp(j["nms_iou"].get<float>(), 0.1f, 0.95f);
    if (j.contains("person_class") && j["person_class"].is_number_integer()) personClass = std::max(0, j["person_class"].get<int>());
}

int PersonDetector::Impl::Detect(std::vector<float>& input) {
    const int64_t shape[4] = {1, 3, inputSize, inputSize};
    Ort::MemoryInfo memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value tensor = Ort::Value::CreateTensor<float>(memInfo, input.data(), input.size(), shape, 4);
    const char* inNames[] = {inputName.c_str()};
    const char* outNames[] = {outputName.c_str()};
    std::vector<Ort::Value> outputs;
    {
        std::lock_guard<std::mutex> lock(runMutex);
        outputs = session->Run(Ort::RunOptions{nullptr}, inNames, &tensor, 1, outNames, 1);
    }

    // YOLOv8 head: [1, 4 + classes, anchors] (or transposed), boxes as cx,cy,w,h in input pixels.
    std::vector<int64_t> dims = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
    if (dims.size() != 3) throw std::runtime_error("unexpected model output shape");
    const bool channelsFirst = dims[1] < dims[2];
    const int64_t channels = channelsFirst ? dims[1] : dims[2];
    const int64_t anchors = channelsFirst ? dims[2] : dims[1];
    if (channels < 5) throw std::runtime_error("unexpected model output shape");
    const float* data = outputs[0].GetTensorData<float>();
    auto at = [&](int64_t c, int64_t i) { return channelsFirst ? data[c * anchors + i] : data[i * channels + c]; };
    const int64_t scoreChannel = 4 + std::min<int64_t>(personClass, channels - 5);

    const float minSide = inputSize * 0.025f; // ignore specks
    std::vector<Box> boxes;
    for (int64_t i = 0; i < anchors; ++i) {
        float score = at(scoreChannel, i);
        if (score < threshold) continue;
        float cx = at(0, i), cy = at(1, i), w = at(2, i), h = at(3, i);
        if (w < minSide || h < minSide) continue;
        boxes.push_back({cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f, score});
    }
    std::sort(boxes.begin(), boxes.end(), [](const Box& a, const Box& b) { return a.score > b.score; });
    std::vector<Box> kept;
    for (const Box& b : boxes) {
        bool suppressed = false;
        for (const Box& k : kept) {
            if (IoU(b, k) > nmsIou) { suppressed = true; break; }
        }
        if (!suppressed) kept.push_back(b);
    }
    return static_cast<int>(kept.size());
}

bool PersonDetector::Impl::Load(std::string* error) {
    fs::path modelPath = FindModelPath();
    if (modelPath.empty()) {
        *error = "Model not found: put a YOLOv8 person-detection ONNX model at models\\people.onnx next to Try2Create.exe";
        return false;
    }
    fs::path dllPath = ExeDir() / L"onnxruntime.dll";
    HMODULE ort = LoadLibraryExW(dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!ort) {
        *error = "onnxruntime.dll not found next to Try2Create.exe";
        return false;
    }
    using GetApiBaseFn = const OrtApiBase*(ORT_API_CALL*)();
    auto getApiBase = reinterpret_cast<GetApiBaseFn>(GetProcAddress(ort, "OrtGetApiBase"));
    const OrtApi* api = getApiBase ? getApiBase()->GetApi(ORT_API_VERSION) : nullptr;
    if (!api) {
        *error = "onnxruntime.dll is an incompatible version";
        return false;
    }
    Ort::InitApi(api);
    ReadConfig(fs::path(modelPath).replace_extension(L".json"));

    try {
        env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_ERROR, "Try2CreatePeople");

        auto makeSession = [&](bool useDml, const std::vector<std::pair<std::string, int64_t>>& overrides) {
            Ort::SessionOptions opts;
            opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            if (useDml) {
                opts.DisableMemPattern();
                opts.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
                for (auto& [name, value] : overrides) opts.AddFreeDimensionOverrideByName(name.c_str(), value);
                const OrtDmlApiPrefix* dml = nullptr;
                Ort::ThrowOnError(Ort::GetApi().GetExecutionProviderApi("DML", ORT_API_VERSION, reinterpret_cast<const void**>(&dml)));
                if (!dml) throw Ort::Exception("DirectML provider unavailable", ORT_FAIL);
                Ort::ThrowOnError(dml->SessionOptionsAppendExecutionProvider_DML(opts, 0));
            } else {
                opts.SetIntraOpNumThreads(std::max(1u, std::thread::hardware_concurrency() / 2));
            }
            return std::make_unique<Ort::Session>(*env, modelPath.c_str(), opts);
        };

        // Names plus any symbolic input dims that must be pinned for DirectML.
        auto inspect = [&]() {
            Ort::AllocatorWithDefaultOptions alloc;
            inputName = session->GetInputNameAllocated(0, alloc).get();
            outputName = session->GetOutputNameAllocated(0, alloc).get();
            std::vector<std::pair<std::string, int64_t>> overrides;
            Ort::TypeInfo typeInfo = session->GetInputTypeInfo(0);
            auto info = typeInfo.GetTensorTypeAndShapeInfo();
            std::vector<int64_t> shape = info.GetShape();
            if (shape.size() == 4) {
                if (shape[2] > 0) inputSize = static_cast<int>(shape[2]); // fixed-size export wins over the config
                std::vector<const char*> symbolic(shape.size(), nullptr);
                info.GetSymbolicDimensions(symbolic.data(), symbolic.size());
                const int64_t fixed[4] = {1, 3, inputSize, inputSize};
                for (size_t i = 0; i < 4; ++i) {
                    if (shape[i] <= 0 && symbolic[i] && *symbolic[i]) overrides.push_back({symbolic[i], fixed[i]});
                }
            }
            return overrides;
        };

        try {
            session = makeSession(true, {});
            auto overrides = inspect();
            if (!overrides.empty()) {
                session = makeSession(true, overrides);
                inspect();
            }
            // Catch models that load on DirectML but fail at the first Run.
            std::vector<float> warmup(static_cast<size_t>(inputSize) * inputSize * 3, 0.5f);
            Detect(warmup);
            deviceName = "DirectML (GPU)";
        } catch (const std::exception&) {
            session = makeSession(false, {});
            inspect();
            deviceName = "CPU";
        }
    } catch (const std::exception& ex) {
        session.reset();
        *error = std::string("Failed to load model: ") + ex.what();
        return false;
    }
    return true;
}

PersonDetector::PersonDetector() : impl_(std::make_unique<Impl>()) {}
PersonDetector::~PersonDetector() = default;

bool PersonDetector::EnsureLoaded(std::string* error) {
    std::lock_guard<std::mutex> lock(impl_->loadMutex);
    if (!impl_->loadAttempted) {
        impl_->loadAttempted = true;
        impl_->loaded = impl_->Load(&impl_->loadError);
    }
    if (!impl_->loaded && error) *error = impl_->loadError;
    return impl_->loaded;
}

bool PersonDetector::IsLoaded() const {
    std::lock_guard<std::mutex> lock(impl_->loadMutex);
    return impl_->loaded;
}

std::string PersonDetector::DeviceName() const {
    std::lock_guard<std::mutex> lock(impl_->loadMutex);
    return impl_->deviceName;
}

std::string PersonDetector::ModelId() {
    fs::path modelPath = FindModelPath();
    if (modelPath.empty()) return {};
    std::error_code ec;
    return "people.onnx:" + std::to_string(fs::file_size(modelPath, ec));
}

int PersonDetector::CountPeopleRGBA(const unsigned char* rgba, int width, int height, std::string* error) {
    if (!IsLoaded()) {
        if (error) *error = "Detector not loaded";
        return -1;
    }
    Impl& m = *impl_;
    std::vector<float> input;
    Letterbox(rgba, width, height, m.inputSize, input);
    try {
        return m.Detect(input);
    } catch (const std::exception& ex) {
        if (error) *error = std::string("Inference failed: ") + ex.what();
        return -1;
    }
}

int PersonDetector::CountPeopleFile(const std::wstring& path, std::string* error) {
    if (!IsLoaded()) {
        if (error) *error = "Detector not loaded";
        return -1;
    }
    std::vector<unsigned char> pixels;
    int w = 0, h = 0;
    if (!DecodeImageFileRGBA(path, pixels, w, h)) {
        if (error) *error = "Could not decode image";
        return -1;
    }
    return CountPeopleRGBA(pixels.data(), w, h, error);
}
