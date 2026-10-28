#include "nsfw_classifier.h"
#include "video_util.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>
#include <windows.h>
#include <wincodec.h>

#include "stb_image.h"
#include "json.hpp"

// The ORT C++ wrapper normally binds to onnxruntime.dll via its import
// library at startup; manual init lets us LoadLibrary it ourselves and hand
// the API table over, so a missing DLL is a soft failure instead of the exe
// refusing to launch.
#define ORT_API_MANUAL_INIT
#include "onnxruntime_cxx_api.h"
#undef ORT_API_MANUAL_INIT

namespace fs = std::filesystem;

namespace {

// Leading slice of OrtDmlApi (dml_provider_factory.h). Declared here so we
// don't drag d3d12.h/DirectML.h into the build for one function pointer.
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
        fs::path p = dir / L"nsfw.onnx";
        if (fs::is_regular_file(p, ec)) return p;
    }
    return {};
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(len > 0 ? len - 1 : 0, '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

// Windows Imaging Component decode to RGBA8; covers WebP (and anything else
// with an installed codec) that stb_image can't read.
bool DecodeWithWic(const std::wstring& path, std::vector<unsigned char>& rgba, int& w, int& h) {
    HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = false;
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapSource* converted = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                     WICDecodeMetadataCacheOnDemand, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) &&
        SUCCEEDED(WICConvertBitmapSource(GUID_WICPixelFormat32bppRGBA, frame, &converted))) {
        UINT uw = 0, uh = 0;
        if (SUCCEEDED(converted->GetSize(&uw, &uh)) && uw > 0 && uh > 0) {
            rgba.resize(static_cast<size_t>(uw) * uh * 4);
            if (SUCCEEDED(converted->CopyPixels(nullptr, uw * 4, static_cast<UINT>(rgba.size()), rgba.data()))) {
                w = static_cast<int>(uw);
                h = static_cast<int>(uh);
                ok = true;
            }
        }
    }
    if (converted) converted->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (factory) factory->Release();
    if (SUCCEEDED(init)) CoUninitialize();
    return ok;
}

std::wstring FindOnPath(const wchar_t* exeName) {
    wchar_t buf[MAX_PATH];
    DWORD n = SearchPathW(nullptr, exeName, nullptr, MAX_PATH, buf, nullptr);
    return (n > 0 && n < MAX_PATH) ? std::wstring(buf) : std::wstring();
}

// Runs a console program without a window and returns everything it wrote
// to stdout (stderr is discarded). Returns false if it couldn't be started
// or exited with a non-zero code.
bool RunCaptureStdout(const std::wstring& commandLine, std::string& out) {
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return false;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nul;
    si.hStdOutput = writePipe;
    si.hStdError = nul;
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = commandLine; // CreateProcessW may modify its buffer
    BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                                  &si, &pi);
    CloseHandle(writePipe);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!started) {
        CloseHandle(readPipe);
        return false;
    }

    char buf[65536];
    DWORD got = 0;
    while (ReadFile(readPipe, buf, sizeof(buf), &got, nullptr) && got > 0) out.append(buf, got);
    CloseHandle(readPipe);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return exitCode == 0;
}

// Resizes RGBA8 `src` to dstW x dstH and writes normalized floats into `out`
// in the model's layout. Area-averages when shrinking (AI images are
// typically 3-6x the model's input size, where plain bilinear aliases badly)
// and bilinear-interpolates when enlarging.
void ResizeNormalize(const unsigned char* src, int srcW, int srcH, int dstW, int dstH, const float mean[3],
                     const float stdv[3], bool nchw, float* out) {
    const size_t plane = static_cast<size_t>(dstW) * dstH;
    auto store = [&](int x, int y, const float rgb[3]) {
        for (int c = 0; c < 3; ++c) {
            float v = (rgb[c] / 255.0f - mean[c]) / stdv[c];
            if (nchw) out[c * plane + static_cast<size_t>(y) * dstW + x] = v;
            else out[(static_cast<size_t>(y) * dstW + x) * 3 + c] = v;
        }
    };

    const bool shrink = srcW >= dstW && srcH >= dstH;
    for (int y = 0; y < dstH; ++y) {
        for (int x = 0; x < dstW; ++x) {
            float rgb[3] = {0, 0, 0};
            if (shrink) {
                int x0 = x * srcW / dstW, x1 = std::max(x0 + 1, (x + 1) * srcW / dstW);
                int y0 = y * srcH / dstH, y1 = std::max(y0 + 1, (y + 1) * srcH / dstH);
                float sum[3] = {0, 0, 0};
                for (int sy = y0; sy < y1; ++sy) {
                    const unsigned char* row = src + (static_cast<size_t>(sy) * srcW + x0) * 4;
                    for (int sx = x0; sx < x1; ++sx, row += 4) {
                        sum[0] += row[0];
                        sum[1] += row[1];
                        sum[2] += row[2];
                    }
                }
                float n = static_cast<float>((x1 - x0) * (y1 - y0));
                for (int c = 0; c < 3; ++c) rgb[c] = sum[c] / n;
            } else {
                float fx = std::clamp((x + 0.5f) * srcW / dstW - 0.5f, 0.0f, static_cast<float>(srcW - 1));
                float fy = std::clamp((y + 0.5f) * srcH / dstH - 0.5f, 0.0f, static_cast<float>(srcH - 1));
                int ix = static_cast<int>(fx), iy = static_cast<int>(fy);
                int ix1 = std::min(ix + 1, srcW - 1), iy1 = std::min(iy + 1, srcH - 1);
                float ax = fx - ix, ay = fy - iy;
                auto px = [&](int sx, int sy, int c) {
                    return static_cast<float>(src[(static_cast<size_t>(sy) * srcW + sx) * 4 + c]);
                };
                for (int c = 0; c < 3; ++c) {
                    float top = px(ix, iy, c) * (1 - ax) + px(ix1, iy, c) * ax;
                    float bot = px(ix, iy1, c) * (1 - ax) + px(ix1, iy1, c) * ax;
                    rgb[c] = top * (1 - ay) + bot * ay;
                }
            }
            store(x, y, rgb);
        }
    }
}

} // namespace

struct NsfwClassifier::Impl {
    std::mutex loadMutex;
    bool loadAttempted = false;
    bool loaded = false;
    std::string loadError;
    std::string deviceName;

    std::unique_ptr<Ort::Env> env;
    std::unique_ptr<Ort::Session> session;
    std::mutex runMutex;

    std::string inputName;
    std::string outputName;
    int inputW = 224;
    int inputH = 224;
    bool nchw = true;
    float mean[3] = {0.5f, 0.5f, 0.5f};
    float stdv[3] = {0.5f, 0.5f, 0.5f};
    int nsfwIndex = 1;
    bool softmax = true;

    bool Load(std::string* error);
    void ReadConfig(const fs::path& jsonPath);
    // Runs the model on one preprocessed image; returns the NSFW probability.
    // Throws on failure.
    float Infer(std::vector<float>& input);
};

float NsfwClassifier::Impl::Infer(std::vector<float>& input) {
    std::vector<int64_t> shape = nchw ? std::vector<int64_t>{1, 3, inputH, inputW}
                                      : std::vector<int64_t>{1, inputH, inputW, 3};
    Ort::MemoryInfo memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value tensor = Ort::Value::CreateTensor<float>(memInfo, input.data(), input.size(), shape.data(), shape.size());
    const char* inNames[] = {inputName.c_str()};
    const char* outNames[] = {outputName.c_str()};

    std::vector<Ort::Value> outputs;
    {
        std::lock_guard<std::mutex> lock(runMutex);
        outputs = session->Run(Ort::RunOptions{nullptr}, inNames, &tensor, 1, outNames, 1);
    }
    const float* logits = outputs[0].GetTensorData<float>();
    size_t n = outputs[0].GetTensorTypeAndShapeInfo().GetElementCount();
    if (n == 0) throw std::runtime_error("empty model output");

    if (n == 1) { // single-logit binary classifier
        return softmax ? 1.0f / (1.0f + std::exp(-logits[0])) : std::clamp(logits[0], 0.0f, 1.0f);
    }
    size_t idx = std::min(static_cast<size_t>(std::max(0, nsfwIndex)), n - 1);
    if (!softmax) return std::clamp(logits[idx], 0.0f, 1.0f);
    float maxLogit = *std::max_element(logits, logits + n);
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) sum += std::exp(logits[i] - maxLogit);
    return static_cast<float>(std::exp(logits[idx] - maxLogit) / sum);
}

void NsfwClassifier::Impl::ReadConfig(const fs::path& jsonPath) {
    std::ifstream f(jsonPath, std::ios::binary);
    if (!f) return;
    nlohmann::json j = nlohmann::json::parse(f, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return;
    if (j.contains("input_size") && j["input_size"].is_number_integer()) {
        inputW = inputH = j["input_size"].get<int>();
    }
    auto readTriple = [&](const char* key, float* dst) {
        if (j.contains(key) && j[key].is_array() && j[key].size() == 3) {
            for (int i = 0; i < 3; ++i) dst[i] = j[key][i].get<float>();
        }
    };
    readTriple("mean", mean);
    readTriple("std", stdv);
    if (j.contains("nsfw_index") && j["nsfw_index"].is_number_integer()) nsfwIndex = j["nsfw_index"].get<int>();
    if (j.contains("softmax") && j["softmax"].is_boolean()) softmax = j["softmax"].get<bool>();
}

bool NsfwClassifier::Impl::Load(std::string* error) {
    fs::path modelPath = FindModelPath();
    if (modelPath.empty()) {
        *error = "Model not found: put an ONNX model at models\\nsfw.onnx next to Try2Create.exe";
        return false;
    }

    fs::path dllPath = ExeDir() / L"onnxruntime.dll";
    // Altered search path makes onnxruntime.dll find our DirectML.dll beside
    // it rather than the older copy Windows ships in System32.
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
        env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_ERROR, "Try2Create");

        // dimOverrides pins symbolic input dims (e.g. "batch_size") to fixed
        // values: DirectML fails on some ops (ViT's embedding Concat) when
        // the batch dimension is left dynamic.
        auto makeSession = [&](bool useDml, const std::vector<std::pair<std::string, int64_t>>& dimOverrides) {
            Ort::SessionOptions opts;
            opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            if (useDml) {
                // Required by the DirectML provider.
                opts.DisableMemPattern();
                opts.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
                for (auto& [name, value] : dimOverrides) opts.AddFreeDimensionOverrideByName(name.c_str(), value);
                const OrtDmlApiPrefix* dml = nullptr;
                Ort::ThrowOnError(Ort::GetApi().GetExecutionProviderApi(
                    "DML", ORT_API_VERSION, reinterpret_cast<const void**>(&dml)));
                if (!dml) throw Ort::Exception("DirectML provider unavailable", ORT_FAIL);
                Ort::ThrowOnError(dml->SessionOptionsAppendExecutionProvider_DML(opts, 0));
            } else {
                opts.SetIntraOpNumThreads(std::max(1u, std::thread::hardware_concurrency() / 2));
            }
            return std::make_unique<Ort::Session>(*env, modelPath.c_str(), opts);
        };

        // Reads input/output names and the input layout from the session.
        // Returns the symbolic input dims that need pinning for DirectML.
        auto inspect = [&]() {
            Ort::AllocatorWithDefaultOptions alloc;
            inputName = session->GetInputNameAllocated(0, alloc).get();
            outputName = session->GetOutputNameAllocated(0, alloc).get();

            // Prefer the model's own fixed input shape over config/defaults, and
            // detect channels-last models (e.g. Keras exports) by where the 3 is.
            Ort::TypeInfo typeInfo = session->GetInputTypeInfo(0); // owns what `info` views
            auto info = typeInfo.GetTensorTypeAndShapeInfo();
            std::vector<int64_t> shape = info.GetShape();
            if (shape.size() == 4) {
                if (shape[1] == 3) {
                    nchw = true;
                    if (shape[2] > 0) inputH = static_cast<int>(shape[2]);
                    if (shape[3] > 0) inputW = static_cast<int>(shape[3]);
                } else if (shape[3] == 3) {
                    nchw = false;
                    if (shape[1] > 0) inputH = static_cast<int>(shape[1]);
                    if (shape[2] > 0) inputW = static_cast<int>(shape[2]);
                }
            }
            std::vector<std::pair<std::string, int64_t>> overrides;
            if (shape.size() != 4) return overrides;
            std::vector<const char*> symbolic(shape.size(), nullptr);
            info.GetSymbolicDimensions(symbolic.data(), symbolic.size());
            const int64_t fixed[4] = {1, nchw ? 3 : inputH, nchw ? inputH : inputW, nchw ? inputW : 3};
            for (size_t i = 0; i < 4; ++i) {
                if (shape[i] <= 0 && symbolic[i] && *symbolic[i]) overrides.push_back({symbolic[i], fixed[i]});
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
            // Some models load fine on DirectML but fail at the first Run;
            // catch that here rather than on every image.
            std::vector<float> warmup(static_cast<size_t>(inputW) * inputH * 3, 0.0f);
            Infer(warmup);
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

NsfwClassifier::NsfwClassifier() : impl_(std::make_unique<Impl>()) {}
NsfwClassifier::~NsfwClassifier() = default;

bool NsfwClassifier::EnsureLoaded(std::string* error) {
    std::lock_guard<std::mutex> lock(impl_->loadMutex);
    if (!impl_->loadAttempted) {
        impl_->loadAttempted = true;
        impl_->loaded = impl_->Load(&impl_->loadError);
    }
    if (!impl_->loaded && error) *error = impl_->loadError;
    return impl_->loaded;
}

bool NsfwClassifier::IsLoaded() const {
    std::lock_guard<std::mutex> lock(impl_->loadMutex);
    return impl_->loaded;
}

std::string NsfwClassifier::DeviceName() const {
    std::lock_guard<std::mutex> lock(impl_->loadMutex);
    return impl_->deviceName;
}

std::string NsfwClassifier::ModelId() {
    fs::path modelPath = FindModelPath();
    if (modelPath.empty()) return {};
    std::error_code ec;
    return WideToUtf8(modelPath.filename().wstring()) + ":" + std::to_string(fs::file_size(modelPath, ec));
}

float NsfwClassifier::ScoreRGBA(const unsigned char* rgba, int width, int height, std::string* error) {
    if (!IsLoaded()) {
        if (error) *error = "Classifier not loaded";
        return -1.0f;
    }
    Impl& m = *impl_;
    std::vector<float> input(static_cast<size_t>(m.inputW) * m.inputH * 3);
    ResizeNormalize(rgba, width, height, m.inputW, m.inputH, m.mean, m.stdv, m.nchw, input.data());
    try {
        return m.Infer(input);
    } catch (const std::exception& ex) {
        if (error) *error = std::string("Inference failed: ") + ex.what();
        return -1.0f;
    }
}

float NsfwClassifier::ScoreFile(const std::wstring& path, std::string* error) {
    if (!IsLoaded()) {
        if (error) *error = "Classifier not loaded";
        return -1.0f;
    }
    std::vector<unsigned char> pixels;
    int w = 0, h = 0;
    if (DecodeImageFileRGBA(path, pixels, w, h)) return ScoreRGBA(pixels.data(), w, h, error);
    if (error) *error = "Could not decode image";
    return -1.0f;
}

bool DecodeImageFileRGBA(const std::wstring& path, std::vector<unsigned char>& rgba, int& width, int& height) {
    if (IsVideoPath(path)) return DecodeVideoFrameRGBA(path, rgba, width, height); // a poster frame
    std::ifstream file(path, std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    int comp = 0;
    unsigned char* pixels =
        bytes.empty() ? nullptr
                      : stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &comp, 4);
    if (pixels) {
        rgba.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
        stbi_image_free(pixels);
        return true;
    }
    return DecodeWithWic(path, rgba, width, height);
}

bool NsfwClassifier::FfmpegAvailable() {
    return !FindOnPath(L"ffmpeg.exe").empty();
}

float NsfwClassifier::ScoreVideo(const std::wstring& path, std::string* error) {
    if (!IsLoaded()) {
        if (error) *error = "Classifier not loaded";
        return -1.0f;
    }
    std::wstring ffmpeg = FindOnPath(L"ffmpeg.exe");
    if (ffmpeg.empty()) {
        if (error) *error = "ffmpeg.exe not found on PATH";
        return -1.0f;
    }

    // Duration via ffprobe (ships alongside ffmpeg) so frames can be spread
    // over the whole clip; without it, fall back to one frame per second.
    constexpr int kFrames = 6;
    double duration = 0.0;
    std::wstring ffprobe = (fs::path(ffmpeg).parent_path() / L"ffprobe.exe").wstring();
    std::string probeOut;
    if (RunCaptureStdout(L"\"" + ffprobe + L"\" -v error -show_entries format=duration -of default=nw=1:nk=1 \"" +
                             path + L"\"",
                         probeOut)) {
        try {
            duration = std::stod(probeOut);
        } catch (...) {
        }
    }

    const int side = 384; // frames are squashed to a square; ScoreRGBA resizes to the model's input anyway
    wchar_t filter[128];
    std::wstring seek;
    if (duration > 0.5) {
        // One frame from the middle of each of kFrames equal slices.
        swprintf(filter, 128, L"fps=%.6f,scale=%d:%d", kFrames / duration, side, side);
        wchar_t ss[32];
        swprintf(ss, 32, L"-ss %.3f ", duration / (2.0 * kFrames));
        seek = ss;
    } else {
        swprintf(filter, 128, L"fps=1,scale=%d:%d", side, side);
    }
    std::wstring cmd = L"\"" + ffmpeg + L"\" -v error -nostdin " + seek + L"-i \"" + path + L"\" -an -vf " + filter +
                       L" -frames:v " + std::to_wstring(kFrames) + L" -f rawvideo -pix_fmt rgba -";
    std::string raw;
    RunCaptureStdout(cmd, raw); // a non-zero exit can still have produced usable frames
    const size_t frameBytes = static_cast<size_t>(side) * side * 4;
    if (raw.size() < frameBytes) {
        if (error) *error = "ffmpeg could not extract frames";
        return -1.0f;
    }
    float best = -1.0f;
    for (size_t off = 0; off + frameBytes <= raw.size(); off += frameBytes) {
        best = std::max(best, ScoreRGBA(reinterpret_cast<const unsigned char*>(raw.data()) + off, side, side, error));
    }
    return best;
}

// ---------------------------------------------------------------------------
// Prompt keyword scoring
// ---------------------------------------------------------------------------
namespace {

// Lowercases and reduces everything that isn't a letter/digit to single
// spaces, padded with a space on each side, so "(Nude:1.2)", "rating:explicit"
// and "see-through" become " nude 1 2 ", " rating explicit ", " see through "
// and keywords can be matched as whole words with a plain substring search.
std::string NormalizeWords(const std::string& s) {
    std::string out = " ";
    for (unsigned char c : s) {
        if (std::isalnum(c) || c >= 0x80) {
            out += static_cast<char>(std::tolower(c));
        } else if (out.back() != ' ') {
            out += ' ';
        }
    }
    if (out.back() != ' ') out += ' ';
    return out;
}

struct KeywordWeight {
    const char* phrase;
    float weight;
};

// Tag-style (Danbooru) and natural-language terms. Only the positive prompt
// is scored: "nsfw" in a negative prompt means the opposite.
const KeywordWeight kKeywords[] = {
    // Explicit
    {"nsfw", 1.0f}, {"rating explicit", 1.0f}, {"explicit", 0.9f}, {"nude", 1.0f}, {"nudity", 1.0f},
    {"naked", 1.0f}, {"topless", 1.0f}, {"bottomless", 1.0f}, {"nipples", 1.0f}, {"nipple", 1.0f},
    {"areola", 1.0f}, {"areolae", 1.0f}, {"pussy", 1.0f}, {"vagina", 1.0f}, {"vulva", 1.0f}, {"penis", 1.0f},
    {"cock", 1.0f}, {"erection", 1.0f}, {"testicles", 1.0f}, {"genitals", 1.0f}, {"sex", 1.0f},
    {"vaginal", 1.0f}, {"anal", 1.0f}, {"oral", 0.8f}, {"fellatio", 1.0f}, {"cunnilingus", 1.0f},
    {"blowjob", 1.0f}, {"handjob", 1.0f}, {"paizuri", 1.0f}, {"titjob", 1.0f}, {"cum", 1.0f},
    {"cumshot", 1.0f}, {"ejaculation", 1.0f}, {"creampie", 1.0f}, {"masturbation", 1.0f}, {"fingering", 1.0f},
    {"orgasm", 1.0f}, {"porn", 1.0f}, {"pornographic", 1.0f}, {"hentai", 1.0f}, {"uncensored", 1.0f},
    {"pubic hair", 1.0f}, {"spread pussy", 1.0f}, {"futanari", 1.0f}, {"completely nude", 1.0f},
    {"breasts out", 1.0f}, {"bare breasts", 1.0f}, {"exposed breasts", 1.0f}, {"rape", 1.0f},
    {"bdsm", 0.9f}, {"bondage", 0.8f}, {"partially nude", 0.9f}, {"censored", 0.9f}, {"mosaic censoring", 1.0f},
    // Suggestive
    {"rating questionable", 0.7f}, {"rating sensitive", 0.5f}, {"questionable", 0.6f}, {"lewd", 0.8f},
    {"ecchi", 0.8f}, {"erotic", 0.8f}, {"seductive", 0.6f}, {"sexy", 0.6f}, {"sensual", 0.6f},
    {"suggestive", 0.6f}, {"lingerie", 0.6f}, {"underwear", 0.6f}, {"panties", 0.6f}, {"bra", 0.5f},
    {"thong", 0.6f}, {"g string", 0.6f}, {"cleavage", 0.5f}, {"see through", 0.5f}, {"sideboob", 0.6f},
    {"underboob", 0.6f}, {"pantyshot", 0.7f}, {"upskirt", 0.7f}, {"micro bikini", 0.7f}, {"ass", 0.5f},
    {"butt", 0.5f}, {"bikini", 0.4f}, {"swimsuit", 0.3f}, {"cameltoe", 0.8f}, {"covering breasts", 0.7f},
};

} // namespace

float PromptNsfwScore(const std::string& positivePrompt) {
    if (positivePrompt.empty()) return 0.0f;
    const std::string text = NormalizeWords(positivePrompt);
    float best = 0.0f;
    for (const auto& kw : kKeywords) {
        if (kw.weight <= best) continue;
        if (text.find(" " + std::string(kw.phrase) + " ") != std::string::npos) best = kw.weight;
    }
    return best;
}
