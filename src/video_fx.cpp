// Video tools, under the Edits > Videos player (2026-10-28, from Freefield's apps - neuregex/freefield, MIT). Lightricks' LTX-2.5 IC-LoRAs that
// redo a clip: a new shape with the edges painted in (Reframe), new words in the person's mouth (Dub), the people taken out (Clean plate), night
// (Day to night), water (Add water), its camera move on a picture (Copy the camera move); a picture made to talk or sing to a sound file (LTX-2.5
// audio-to-video); and two with MiniMax H3 (video_ui.cpp builds them): Extend (more of the clip from its last frame) and New angle (the clip from
// another viewpoint: MoGe depth + CrossView warp + Cseti's CrossView LoRA).
// The LTX ones need Lightricks' ComfyUI-LTXVideo nodes (the IC-LoRA loader, its guides, the inpaint blend) and LTX-2.5's own files. Lightricks'
// LoRA repos are gated: they download with this PC's Hugging Face login (huggingface-cli login) once their licence is accepted on the page.
// (Freefield's talking-head AV LoRA is left out: it is one person's voice and face, trained in.)
#include "video_fx.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <thread>
#include <vector>
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include "app_state.h"
#include "comfy.h"
#include "imgui.h"
#include "notify.h"
#include "ui_extra.h"
#include "ui_theme.h"
#include "video_util.h"
#include "win_dialogs.h"

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::string U8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring W(const char* s) { return std::wstring(s, s + strlen(s)); }

std::wstring DataFolder() {
    wchar_t buf[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    return std::wstring(buf) + L"\\Try2Create";
}

// ---- the files
struct ToolFile {
    const char* sub;    // models\<sub>
    const char* folder; // the LoRA sorter's folder for it (it goes there when that folder is there), or nullptr
    const char* name;
    const char* repo; // huggingface.co/<repo>/resolve/main/<name>
    double gb;
    bool gated;
};
const ToolFile kInpaint = {"loras", "LTX Video", "ltx-2.3-22b-ic-lora-in-outpainting-0.9.safetensors", "Lightricks/LTX-2.3-22b-IC-LoRA-In-Outpainting", 1.31, true};
const ToolFile kDubIt = {"loras", "LTX Video", "ltx-2.3-22b-ic-lora-dubit-0.9.safetensors", "Lightricks/LTX-2.3-22b-IC-LoRA-DubIt", 2.47, true};
const ToolFile kCleanPlate = {"loras", "LTX Video", "ltx-2.5-22b-ic-lora-clean-plate-1.0.safetensors", "Lightricks/LTX-2.5-22b-IC-LoRA-Clean-Plate", 0.33, true};
const ToolFile kDayNight = {"loras", "LTX Video", "ltx-2.5-22b-ic-lora-day-to-night-0.9.safetensors", "Lightricks/LTX-2.5-22b-IC-LoRA-Day-To-Night", 0.33, true};
const ToolFile kWaterSim = {"loras", "LTX Video", "ltx-2.5-22b-ic-lora-water-simulation-0.9.safetensors", "Lightricks/LTX-2.5-22b-IC-LoRA-Water-Simulation", 0.91, true};
const ToolFile kCameraman = {"loras", "LTX Video", "LTX2.3-22B_IC-LoRA-Cameraman_v2_14000.safetensors", "Cseti/LTX2.3-22B_IC-LoRA-Cameraman_v2", 0.65, false};
const ToolFile kMoge = {"geometry_estimation", nullptr, "moge_2_vitl_normal_fp16.safetensors", "Comfy-Org/MoGe", 0.66, false};
const ToolFile kCrossView = {"loras", "MiniMax H3", "MiniMax-H3_Ref2VA-LoRA-CrossView-Warp_v1_3500.safetensors", "Cseti/MiniMax-H3_Ref2VA-LoRA-CrossView-Warp_v1", 0.6, false};
const ToolFile kRefTurbo = {"loras", "MiniMax H3", "minimax_h3_ref2v_turbo_8step_v1.0_768p_comfyui_bf16.safetensors", "lightx2v/Minimax-h3-Turbo", 1.96, false};

std::string RepoPath(const ToolFile& f) { // (MoGe's file sits in a folder of its repo)
    return std::string(f.repo) + "/resolve/main/" + (f.sub == std::string("geometry_estimation") ? "geometry_estimation/" : "") + f.name;
}

fs::path FilePlace(const ToolFile& f, bool forDownload) {
    const fs::path base = fs::path(ComfyModelsDir()) / W(f.sub);
    std::error_code ec;
    if (f.folder && (forDownload ? fs::is_directory(base / W(f.folder), ec) : fs::exists(base / W(f.folder) / W(f.name), ec))) return base / W(f.folder) / W(f.name);
    return base / W(f.name);
}
bool FileThere(const ToolFile& f) {
    if (ComfyModelsDir().empty()) return false;
    std::error_code ec;
    return fs::exists(FilePlace(f, false), ec);
}

bool NodePack(const wchar_t* folder) {
    const std::wstring install = ComfyInstallDir();
    std::error_code ec;
    return !install.empty() && fs::exists(fs::path(ComfyAppDir()) / L"custom_nodes" / folder / L"__init__.py", ec);
}

// this PC's Hugging Face login: HF_TOKEN, HF_HOME\token, or %USERPROFILE%\.cache\huggingface\token
std::string HfToken() {
    char buf[512] = {};
    if (GetEnvironmentVariableA("HF_TOKEN", buf, sizeof(buf)) && buf[0]) return buf;
    std::vector<fs::path> places;
    wchar_t w[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"HF_HOME", w, MAX_PATH)) places.push_back(fs::path(w) / L"token");
    if (GetEnvironmentVariableW(L"USERPROFILE", w, MAX_PATH)) places.push_back(fs::path(w) / L".cache\\huggingface\\token");
    for (const fs::path& p : places) {
        std::ifstream f(p);
        std::string t;
        if (f && std::getline(f, t)) {
            while (!t.empty() && (t.back() == '\r' || t.back() == ' ' || t.back() == '\n')) t.pop_back();
            if (!t.empty()) return t;
        }
    }
    return {};
}

struct Download {
    std::atomic<bool> running{false}, finished{false};
    std::atomic<int> done{0}, total{0};
    std::mutex m;
    std::string error, what, gatedRepo; // gatedRepo: the one Hugging Face said no to
};
Download g_dl;

void StartDownload(std::vector<const ToolFile*> files) {
    if (g_dl.running || files.empty() || ComfyModelsDir().empty()) return;
    g_dl.running = true, g_dl.finished = false, g_dl.done = 0, g_dl.total = static_cast<int>(files.size());
    {
        std::lock_guard<std::mutex> lock(g_dl.m);
        g_dl.error.clear(), g_dl.gatedRepo.clear();
    }
    std::thread([files]() {
        std::error_code ec;
        const fs::path tmp = fs::path(DataFolder()) / L"setup_tmp";
        fs::create_directories(tmp, ec);
        // the login goes to curl in a header file (not on its command line, where other programs can read it), removed straight after
        const std::string token = HfToken();
        const fs::path header = tmp / (L"hf_" + std::to_wstring(GetCurrentThreadId()) + L".txt");
        if (!token.empty()) std::ofstream(header) << "Authorization: Bearer " << token << "\n";
        std::string error, gated;
        for (const ToolFile* f : files) {
            {
                std::lock_guard<std::mutex> lock(g_dl.m);
                g_dl.what = f->name;
            }
            const fs::path dest = FilePlace(*f, true);
            fs::create_directories(dest.parent_path(), ec);
            if (!fs::exists(dest, ec)) {
                const std::wstring part = dest.wstring() + L".part", url = L"https://huggingface.co/" + W(RepoPath(*f).c_str());
                std::wstring cmd = L"curl.exe -L --fail --retry 3 --retry-delay 10 -C - --no-progress-meter";
                if (f->gated && !token.empty()) cmd += L" -H @\"" + header.wstring() + L"\"";
                std::string out;
                RunCaptureHidden(cmd + L" -o \"" + part + L"\" \"" + url + L"\"", out, true);
                const bool good = fs::exists(part, ec) && fs::file_size(part, ec) > static_cast<uintmax_t>(f->gb * 0.9 * 1073741824.0);
                if (good) fs::rename(part, dest, ec);
                if (!good || ec) {
                    if (out.find("401") != std::string::npos || out.find("403") != std::string::npos) {
                        gated = f->repo;
                        error = token.empty() ? "Hugging Face wants a login for it" : "Hugging Face said no: its licence is not accepted on your account yet";
                        fs::remove(part, ec);
                    } else {
                        error = "it did not download (" + out.substr(0, 160) + ")";
                    }
                    break;
                }
            }
            ++g_dl.done;
        }
        fs::remove(header, ec);
        {
            std::lock_guard<std::mutex> lock(g_dl.m);
            g_dl.error = error, g_dl.gatedRepo = gated;
        }
        g_dl.running = false;
        g_dl.finished = true;
    }).detach();
}

// ---- the tools
enum Tool { kReframe, kDub, kClean, kNight, kWater, kCamera, kTalk, kExtend, kReshoot, kToolCount };
struct ToolInfo {
    const char* name;
    const char* about;
    std::vector<const ToolFile*> files;
    const char* prompt;   // what it starts with
    const char* negative; // (LTX)
    const char* promptLabel;
    bool ltx, needsVideo;
};
const ToolInfo kTools[] = {
    {"Reframe", "A new shape for the clip (9:16 for a phone, 16:9, square...): what lies beyond its edges is painted in, frame by frame, in two "
                "passes (Lightricks' In-Outpainting IC-LoRA). About 3.5 min for 5 s to 9:16 (1088 x 1920) on a 24 GB card.",
     {&kInpaint}, "", "", "What lies beyond the frame (optional)", true, true},
    {"Dub", "The person in the clip says new words, in any language: a new voice and new lip movement from your text (Lightricks' DubIt "
            "IC-LoRA). Best on a clip of one person talking. About 1.5 min for 5 s.",
     {&kDubIt}, "A woman talks to the camera, speaking in Spanish, saying: \"Hola, bienvenidos. Esta es una prueba de doblaje.\"",
     "blurry, distorted face, warped mouth", "Who speaks, how, and the words in quotes", true, true},
    {"Clean plate", "People and moving things taken out of the shot, the background rebuilt behind them (Lightricks' Clean-Plate IC-LoRA). Made for wide shots with people passing through (a close-up "
                    "of one person stays as it was); parked cars can go too. About 1.5 min for 5 s.",
     {&kCleanPlate}, "An empty street with parked cars and shop fronts, no people anywhere in the frame, natural daylight.",
     "people, pedestrians, person, crowd", "Describe the result", true, true},
    {"Day to night", "The same shot at night: the same framing, camera and motion, only the light changes (Lightricks' Day-To-Night IC-LoRA). About 1.5 min for 5 s (2 from a cold start).",
     {&kDayNight}, "A realistic nighttime scene, photorealistic moonlight, deep natural shadows. Only the lighting changes from day to night; "
                   "identical composition, framing, camera movement and motion.",
     "daytime, bright sunlight, blue sky, overexposed, worst quality, inconsistent motion, blurry, jittery, distorted", "Describe the result", true, true},
    {"Add water", "Water simulated into the shot, following its ground and walls: a flood, a puddle, a wave (Lightricks' Water-Simulation IC-LoRA). Say where the water is: on a street it floods "
                  "the street. About 1.5 min for 5 s.",
     {&kWaterSim}, "The street slowly floods with clear rippling water that reflects the buildings, realistic fluid motion.", "blurry, distorted, low quality",
     "Describe the water", true, true},
    {"Copy the camera move", "The clip's camera move (a push-in, a pan, an orbit, a crane) made again starting from a picture you pick (Cseti's "
                             "Cameraman IC-LoRA: research and non-commercial use only). About 1.5 min for 5 s.",
     {&kCameraman}, "The camera slowly pushes in toward her face, soft studio light, shallow depth of field", "static camera, frozen frame, blurry, distorted",
     "Describe the shot", true, true},
    {"Make a picture talk", "A picture speaks or sings to a sound file you pick: its lips, face and head move with it (LTX-2.5 audio-to-video, in "
                            "two passes). Use pictures of yourself, made-up characters, or people who agreed to it. About 3.5 min for 5 s (the long side 1920).",
     {}, "She speaks warmly to the camera with natural head movement and expressive eyes, soft studio light", "blurry, distorted face, warped mouth",
     "Describe the performance", true, false},
    {"Extend", "More of the clip: MiniMax H3 carries on from its last frame (picture and sound) at the Video page's quality, and the new part is "
               "joined after it in one video. About 2.5 min for 3 s more at Balanced.",
     {}, "The scene continues naturally: the same people, place, light and camera style.", "", "What happens next", false, true},
    {"New angle", "The same clip seen from another angle: MoGe reads its depth, the frames are turned to the new viewpoint, and MiniMax H3 "
                  "(CrossView LoRA, 8 steps) paints in what the turn shows. About 6.5 min for 5 s (960 x 544).",
     {&kMoge, &kCrossView, &kRefTurbo}, "", "", "Extra description (optional)", false, true},
};

struct Aspect {
    const char* name;
    int w, h;
};
const Aspect kAspects[] = {{"9:16", 1088, 1920}, {"16:9", 1920, 1088}, {"1:1", 1408, 1408}, {"4:5", 1216, 1536}, {"4:3", 1472, 1088}, {"21:9", 2048, 896}};
const int kSizes[] = {448, 544, 576, 720}; // the short side the LTX IC tools work at

struct Fx {
    int tool = kReframe;
    std::string prompt[kToolCount], negative[kToolCount];
    bool filled = false;
    int aspect = 0, size = 1;
    float strength = 1.0f, hold = 0.6f;
    std::wstring picture, audio;
    float audioStart = 0.0f, talkSeconds = 5.0f, extendSeconds = 3.0f, reshootSeconds = 5.0f;
    float azimuth = -30.0f, elevation = 20.0f, distance = 1.0f;
    bool keepSound = true;
    // the clip, probed on a worker
    std::wstring probed;
    VideoInfo info;
    std::future<VideoInfo> probeFut;
    std::wstring probing;
    std::string note;
};
Fx g_fx;

// ---- LTX-2.5's files (the same as the Video page's LTX-2.5 quality)
const char* const kLtxModel = "ltx-2.5-22b-distilled-transformer-comfy-int8-convrot.safetensors";
const char* const kLtxText = "gemma4-12b-with-proj-ltx-2.5-comfy-int8-convrot.safetensors";
const char* const kLtxVae = "ltx-2.5-video-vae-bf16.safetensors";
const char* const kLtxAudioVae = "ltx-2.5-audio-vae-bf16.safetensors";
const char* const kLtxUpscaler = "ltx-2.5-latent-spatial-upscaler-x2-bf16-1.0.safetensors";
const char* const kSigmas8 = "1.0, 0.99375, 0.9875, 0.98125, 0.975, 0.909375, 0.725, 0.421875, 0.0"; // (the distilled model's)

struct Graph {
    json wf;
    void node(const std::string& id, const char* cls, json inputs) { wf[id] = {{"class_type", cls}, {"inputs", std::move(inputs)}, {"_meta", {{"title", cls}}}}; }
    static json L(const std::string& id, int k = 0) { return json::array({id, k}); }
    void loaders(const char* lora, double strength) { // 1 model (2 with the IC-LoRA), 3 text, 4 video VAE, 5 audio VAE
        node("1", "UNETLoader", {{"unet_name", kLtxModel}, {"weight_dtype", "default"}});
        if (lora) node("2", "LTXICLoRALoaderModelOnly", {{"model", L("1")}, {"lora_name", lora}, {"strength_model", strength}});
        node("3", "CLIPLoader", {{"clip_name", kLtxText}, {"type", "ltxv"}, {"device", "default"}});
        node("4", "VAELoader", {{"vae_name", kLtxVae}});
        node("5", "VAELoader", {{"vae_name", kLtxAudioVae}});
    }
    void text(const std::string& prompt, const std::string& negative, const json& fps) { // 23: the conditioning
        node("21", "CLIPTextEncode", {{"clip", L("3")}, {"text", prompt}});
        node("22", "CLIPTextEncode", {{"clip", L("3")}, {"text", negative}});
        node("23", "LTXVConditioning", {{"positive", L("21")}, {"negative", L("22")}, {"frame_rate", fps}});
    }
    void clip(int frames) { // 10 the clip, 11 its frames / sound / fps, 12 the first `frames` frames; 13 its sound as long as they are
        node("10", "LoadVideo", {{"file", "@clip"}});
        node("11", "GetVideoComponents", {{"video", L("10")}});
        node("12", "ImageFromBatch", {{"image", L("11")}, {"batch_index", 0}, {"length", frames}});
    }
    void sound(double seconds) { // 13: the clip's sound, silence added under it (a clip without sound still works), cut to the frames' length
        node("14", "EmptyAudio", {{"duration", seconds}, {"sample_rate", 44100}, {"channels", 2}});
        node("15", "AudioMerge", {{"audio1", L("11", 1)}, {"audio2", L("14")}, {"merge_method", "add"}});
        node("13", "TrimAudioDuration", {{"audio", L("15")}, {"start_index", 0.0}, {"duration", seconds}});
    }
};

// The IC-LoRA tools (Freefield's dub / vfx-* / camera-copy): the clip at a small size as the IC guide over all its frames, its sound as the
// voice reference; one pass of the distilled model's 8 sigmas. Camera copy also starts the latent from the picture ("@first").
json BuildIc(const ToolFile& lora, double strength, const std::string& prompt, const std::string& negative, int shortSide, int frames, double fps,
             bool picture, double hold, bool newVoice, bool keepSound, const char* prefix) {
    Graph g;
    g.loaders(lora.name, strength);
    g.clip(frames);
    g.sound(frames / fps);
    g.text(prompt, negative, Graph::L("11", 2));
    g.node("16", "ResizeImageMaskNode", {{"input", Graph::L("12")}, {"resize_type", "scale shorter dimension"}, {"resize_type.shorter_size", shortSide}, {"scale_method", "lanczos"}});
    g.node("17", "GetImageSize", {{"image", Graph::L("16")}});
    g.node("30", "EmptyLTXVLatentVideo", {{"width", Graph::L("17", 0)}, {"height", Graph::L("17", 1)}, {"length", Graph::L("17", 2)}, {"batch_size", 1}});
    json latent = Graph::L("30");
    if (picture) {
        g.node("6", "LoadImage", {{"image", "@first"}});
        g.node("7", "ResizeImageMaskNode", {{"input", Graph::L("6")}, {"resize_type", "scale longer dimension"}, {"resize_type.longer_size", 1536}, {"scale_method", "lanczos"}});
        g.node("31", "LTXVImgToVideoInplace", {{"vae", Graph::L("4")}, {"image", Graph::L("7")}, {"latent", latent}, {"strength", hold}, {"bypass", false}});
        latent = Graph::L("31");
    }
    g.node("32", "LTXAddVideoICLoRAGuide", {{"positive", Graph::L("23", 0)}, {"negative", Graph::L("23", 1)}, {"vae", Graph::L("4")}, {"latent", latent}, {"image", Graph::L("16")},
                                            {"latent_downscale_factor", Graph::L("2", 1)}, {"frame_idx", 0}, {"strength", 1.0}, {"crop", "disabled"}, {"use_tiled_encode", false},
                                            {"tile_size", 256}, {"tile_overlap", 64}, {"use_streaming", false}, {"tile_frames", 97}});
    g.node("33", "VAEEncodeAudio", {{"audio", Graph::L("13")}, {"vae", Graph::L("5")}});
    g.node("34", "LTXVSetAudioRefTokens", {{"positive", Graph::L("32", 0)}, {"negative", Graph::L("32", 1)}, {"audio_latent", Graph::L("33")}});
    json audioLatent = Graph::L("34", 2), pos = Graph::L("32", 0), neg = Graph::L("32", 1);
    if (newVoice) { // Dub: a new sound track, the clip's voice only as the reference
        g.node("35", "LTXVEmptyLatentAudio", {{"audio_vae", Graph::L("5")}, {"frames_number", Graph::L("17", 2)}, {"frame_rate", Graph::L("11", 2)}, {"batch_size", 1}});
        audioLatent = Graph::L("35"), pos = Graph::L("34", 0), neg = Graph::L("34", 1);
    }
    g.node("36", "LTXVConcatAVLatent", {{"video_latent", Graph::L("32", 2)}, {"audio_latent", audioLatent}});
    g.node("40", "CFGGuider", {{"model", Graph::L("2")}, {"positive", pos}, {"negative", neg}, {"cfg", 1.0}});
    g.node("41", "KSamplerSelect", {{"sampler_name", "euler_ancestral"}});
    g.node("42", "RandomNoise", {{"noise_seed", 42}});
    g.node("43", "ManualSigmas", {{"sigmas", kSigmas8}});
    g.node("44", "SamplerCustomAdvanced", {{"noise", Graph::L("42")}, {"guider", Graph::L("40")}, {"sampler", Graph::L("41")}, {"sigmas", Graph::L("43")}, {"latent_image", Graph::L("36")}});
    g.node("45", "LTXVSeparateAVLatent", {{"av_latent", Graph::L("44")}});
    g.node("46", "LTXVCropGuides", {{"positive", Graph::L("32", 0)}, {"negative", Graph::L("32", 1)}, {"latent", Graph::L("45", 0)}});
    g.node("47", "VAEDecodeTiled", {{"samples", Graph::L("46", 2)}, {"vae", Graph::L("4")}, {"tile_size", 512}, {"overlap", 64}, {"temporal_size", 64}, {"temporal_overlap", 8}});
    g.node("48", "LTXVAudioVAEDecode", {{"samples", Graph::L("45", 1)}, {"audio_vae", Graph::L("5")}});
    g.node("49", "CreateVideo", {{"images", Graph::L("47")}, {"audio", keepSound ? Graph::L("13") : Graph::L("48")}, {"fps", Graph::L("11", 2)}});
    g.node("50", "SaveVideo", {{"video", Graph::L("49")}, {"filename_prefix", prefix}, {"format", "auto"}, {"format.codec", "auto"}});
    return g.wf;
}

// Reframe (Freefield's reframe, from Lightricks' in/outpainting example): the clip fitted into the new shape (tw x th) and padded, the padding as
// the mask; a first pass at half size guided by the padded clip, blended back over it, doubled and finished in 3 sigmas, blended again at full
// size so the clip's own pixels stay as they were.
json BuildReframe(double strength, const std::string& prompt, int frames, double fps, int srcW, int srcH, int tw, int th) {
    Graph g;
    auto L = [](const std::string& id, int k = 0) { return Graph::L(id, k); };
    g.loaders(kInpaint.name, strength);
    g.clip(frames);
    g.sound(frames / fps);
    g.text(prompt, "", L("11", 2));
    const double scale = std::min(static_cast<double>(tw) / std::max(srcW, 1), static_cast<double>(th) / std::max(srcH, 1));
    const int fw = std::min(tw, std::max(8, static_cast<int>(srcW * scale) / 2 * 2)), fh = std::min(th, std::max(8, static_cast<int>(srcH * scale) / 2 * 2));
    const int left = (tw - fw) / 2, top = (th - fh) / 2;
    g.node("16", "ImageScale", {{"image", L("12")}, {"upscale_method", "lanczos"}, {"width", fw}, {"height", fh}, {"crop", "disabled"}});
    g.node("17", "ImagePadForOutpaint", {{"image", L("16")}, {"left", left}, {"top", top}, {"right", tw - fw - left}, {"bottom", th - fh - top}, {"feathering", 0}});
    g.node("18", "MaskToImage", {{"mask", L("17", 1)}});
    g.node("19", "RepeatImageBatch", {{"image", L("18")}, {"amount", frames}});
    g.node("20", "ImageToMask", {{"image", L("19")}, {"channel", "red"}}); // the mask, every frame, full size
    g.node("24", "ResizeImageMaskNode", {{"input", L("20")}, {"resize_type", "scale by multiplier"}, {"resize_type.multiplier", 0.5}, {"scale_method", "area"}});
    g.node("25", "ImageScale", {{"image", L("17")}, {"upscale_method", "area"}, {"width", tw / 2}, {"height", th / 2}, {"crop", "disabled"}});
    g.node("26", "LTXVInpaintPreprocess", {{"images", L("25")}, {"mask", L("24")}});
    g.node("27", "GetImageSize", {{"image", L("26")}});
    g.node("30", "EmptyLTXVLatentVideo", {{"width", L("27", 0)}, {"height", L("27", 1)}, {"length", L("27", 2)}, {"batch_size", 1}});
    g.node("32", "LTXAddVideoICLoRAGuideAdvanced", {{"positive", L("23", 0)}, {"negative", L("23", 1)}, {"vae", L("4")}, {"latent", L("30")}, {"image", L("26")}, {"frame_idx", 0},
                                                    {"strength", 1.0}, {"latent_downscale_factor", 1}, {"crop", "disabled"}, {"use_tiled_encode", false}, {"tile_size", 256},
                                                    {"tile_overlap", 64}, {"attention_strength", 1.0}, {"use_streaming", false}, {"tile_frames", 97}});
    g.node("33", "VAEEncodeAudio", {{"audio", L("13")}, {"vae", L("5")}});
    g.node("34", "LTXVSetAudioRefTokens", {{"positive", L("32", 0)}, {"negative", L("32", 1)}, {"audio_latent", L("33")}});
    g.node("36", "LTXVConcatAVLatent", {{"video_latent", L("32", 2)}, {"audio_latent", L("34", 2)}});
    g.node("40", "CFGGuider", {{"model", L("2")}, {"positive", L("32", 0)}, {"negative", L("32", 1)}, {"cfg", 1.0}});
    g.node("41", "KSamplerSelect", {{"sampler_name", "euler_ancestral"}});
    g.node("42", "RandomNoise", {{"noise_seed", 43}});
    g.node("43", "ManualSigmas", {{"sigmas", kSigmas8}});
    g.node("44", "SamplerCustomAdvanced", {{"noise", L("42")}, {"guider", L("40")}, {"sampler", L("41")}, {"sigmas", L("43")}, {"latent_image", L("36")}});
    g.node("45", "LTXVSeparateAVLatent", {{"av_latent", L("44", 1)}});
    g.node("46", "LTXVCropGuides", {{"positive", L("32", 0)}, {"negative", L("32", 1)}, {"latent", L("45", 0)}});
    g.node("47", "VAEDecodeTiled", {{"samples", L("46", 2)}, {"vae", L("4")}, {"tile_size", 512}, {"overlap", 64}, {"temporal_size", 128}, {"temporal_overlap", 32}});
    g.node("48", "LTXVLaplacianPyramidBlend", {{"image_a", L("47")}, {"image_b", L("26")}, {"mask", L("24")}, {"trim_to_shortest", true}, {"mask_low_res_dilation", 5}});
    // the second pass, at full size
    g.node("51", "ResizeImageMaskNode", {{"input", L("48")}, {"resize_type", "scale by multiplier"}, {"resize_type.multiplier", 2.0}, {"scale_method", "lanczos"}});
    g.node("52", "VAEEncodeTiled", {{"pixels", L("51")}, {"vae", L("4")}, {"tile_size", 512}, {"overlap", 64}, {"temporal_size", 500}, {"temporal_overlap", 64}});
    g.node("53", "LTXVSetAudioRefTokens", {{"positive", L("46", 0)}, {"negative", L("46", 1)}, {"audio_latent", L("45", 1)}});
    g.node("54", "LTXVConcatAVLatent", {{"video_latent", L("52")}, {"audio_latent", L("53", 2)}});
    g.node("55", "CFGGuider", {{"model", L("2")}, {"positive", L("46", 0)}, {"negative", L("46", 1)}, {"cfg", 1.0}});
    g.node("56", "RandomNoise", {{"noise_seed", 42}});
    g.node("57", "ManualSigmas", {{"sigmas", "0.7250, 0.4219, 0.0"}});
    g.node("58", "KSamplerSelect", {{"sampler_name", "euler"}});
    g.node("59", "SamplerCustomAdvanced", {{"noise", L("56")}, {"guider", L("55")}, {"sampler", L("58")}, {"sigmas", L("57")}, {"latent_image", L("54")}});
    g.node("60", "LTXVSeparateAVLatent", {{"av_latent", L("59")}});
    g.node("61", "VAEDecodeTiled", {{"samples", L("60", 0)}, {"vae", L("4")}, {"tile_size", 512}, {"overlap", 64}, {"temporal_size", 128}, {"temporal_overlap", 32}});
    g.node("62", "LTXVInpaintPreprocess", {{"images", L("17")}, {"mask", L("20")}});
    g.node("63", "LTXVLaplacianPyramidBlend", {{"image_a", L("61")}, {"image_b", L("62")}, {"mask", L("20")}, {"trim_to_shortest", true}, {"mask_low_res_dilation", 6}});
    g.node("64", "LTXVAudioVAEDecode", {{"samples", L("60", 1)}, {"audio_vae", L("5")}});
    g.node("65", "CreateVideo", {{"images", L("63")}, {"audio", L("13")}, {"fps", L("11", 2)}}); // (the clip's own sound)
    g.node("66", "SaveVideo", {{"video", L("65")}, {"filename_prefix", "video/Reframe"}, {"format", "auto"}, {"format.codec", "auto"}});
    return g.wf;
}

// Make a picture talk (Freefield's talking): the picture ("@first") as the first frame, the sound file ("@audio") kept as it is (its latent
// masked so it is not redrawn) while the picture moves to it; half size (the long side 960) then doubled and finished, as LTX-2.5's i2v.
json BuildTalk(const std::string& prompt, const std::string& negative, double start, double seconds) {
    Graph g;
    auto L = [](const std::string& id, int k = 0) { return Graph::L(id, k); };
    g.loaders(nullptr, 0.0);
    g.node("6", "LatentUpscaleModelLoader", {{"model_name", kLtxUpscaler}});
    g.text(prompt, negative, 24.0);
    const int frames = 1 + static_cast<int>(std::floor(24.0 * seconds / 8.0)) * 8;
    g.node("7", "LoadImage", {{"image", "@first"}});
    g.node("8", "ResizeImageMaskNode", {{"input", L("7")}, {"resize_type", "scale longer dimension"}, {"resize_type.longer_size", 1536}, {"scale_method", "lanczos"}});
    g.node("9", "LTXVPreprocess", {{"image", L("8")}, {"img_compression", 18}});
    g.node("10", "ResizeImageMaskNode", {{"input", L("7")}, {"resize_type", "scale longer dimension"}, {"resize_type.longer_size", 960}, {"scale_method", "lanczos"}});
    g.node("11", "ResizeImageMaskNode", {{"input", L("10")}, {"resize_type", "scale to multiple"}, {"resize_type.multiple", 32}, {"scale_method", "lanczos"}});
    g.node("12", "GetImageSize", {{"image", L("11")}});
    g.node("30", "EmptyLTXVLatentVideo", {{"width", L("12", 0)}, {"height", L("12", 1)}, {"length", frames}, {"batch_size", 1}});
    g.node("31", "LTXVImgToVideoInplace", {{"vae", L("4")}, {"image", L("9")}, {"latent", L("30")}, {"strength", 0.7}, {"bypass", false}});
    g.node("14", "LoadAudio", {{"audio", "@audio"}});
    g.node("13", "TrimAudioDuration", {{"audio", L("14")}, {"start_index", start}, {"duration", seconds}});
    g.node("15", "LTXVAudioVAEEncode", {{"audio", L("13")}, {"audio_vae", L("5")}});
    g.node("16", "SolidMask", {{"value", 0.0}, {"width", 1024}, {"height", 1024}});
    g.node("17", "SetLatentNoiseMask", {{"samples", L("15")}, {"mask", L("16")}}); // (the sound is kept: nothing of it is redrawn)
    g.node("36", "LTXVConcatAVLatent", {{"video_latent", L("31")}, {"audio_latent", L("17")}});
    g.node("40", "CFGGuider", {{"model", L("1")}, {"positive", L("23", 0)}, {"negative", L("23", 1)}, {"cfg", 1.0}});
    g.node("41", "KSamplerSelect", {{"sampler_name", "euler_ancestral"}});
    g.node("42", "RandomNoise", {{"noise_seed", 43}});
    g.node("43", "ManualSigmas", {{"sigmas", kSigmas8}});
    g.node("44", "SamplerCustomAdvanced", {{"noise", L("42")}, {"guider", L("40")}, {"sampler", L("41")}, {"sigmas", L("43")}, {"latent_image", L("36")}});
    g.node("45", "LTXVSeparateAVLatent", {{"av_latent", L("44")}});
    g.node("50", "LTXVLatentUpsampler", {{"samples", L("45", 0)}, {"upscale_model", L("6")}, {"vae", L("4")}});
    g.node("51", "LTXVImgToVideoInplace", {{"vae", L("4")}, {"image", L("8")}, {"latent", L("50")}, {"strength", 1.0}, {"bypass", false}});
    g.node("52", "LTXVConcatAVLatent", {{"video_latent", L("51")}, {"audio_latent", L("17")}});
    g.node("53", "RandomNoise", {{"noise_seed", 42}});
    g.node("54", "ManualSigmas", {{"sigmas", "0.909375, 0.725, 0.421875, 0.0"}});
    g.node("55", "SamplerCustomAdvanced", {{"noise", L("53")}, {"guider", L("40")}, {"sampler", L("41")}, {"sigmas", L("54")}, {"latent_image", L("52")}});
    g.node("56", "LTXVSeparateAVLatent", {{"av_latent", L("55")}});
    g.node("57", "VAEDecodeTiled", {{"samples", L("56", 0)}, {"vae", L("4")}, {"tile_size", 512}, {"overlap", 64}, {"temporal_size", 64}, {"temporal_overlap", 8}});
    g.node("58", "CreateVideo", {{"images", L("57")}, {"audio", L("13")}, {"fps", 24.0}});
    g.node("59", "SaveVideo", {{"video", L("58")}, {"filename_prefix", "video/Talking"}, {"format", "auto"}, {"format.codec", "auto"}});
    return g.wf;
}

// ---- what a tool still needs ("" = ready)
std::string Missing(int t, std::vector<const ToolFile*>* files) {
    const ToolInfo& ti = kTools[t];
    for (const ToolFile* f : ti.files) {
        if (!FileThere(*f) && files) files->push_back(f);
    }
    if (ComfyModelsDir().empty()) return "ComfyUI's models folder was not found";
    if (ti.ltx) {
        if (!VideoLtxReady()) return "LTX-2.5's own files (the Video page's LTX-2.5 quality sets them up)";
        if (!NodePack(L"ComfyUI-LTXVideo")) return "Lightricks' ComfyUI-LTXVideo nodes in ComfyUI's custom_nodes";
    } else {
        if (t == kExtend && VideoH3ExtendModel().empty()) return "a MiniMax H3 model (the Video page sets it up)";
        if (t == kReshoot && !NodePack(L"ComfyUI-CrossViewWarp")) return "the ComfyUI-CrossViewWarp nodes (comfyrob) in ComfyUI's custom_nodes";
    }
    for (const ToolFile* f : ti.files) {
        if (!FileThere(*f)) return "files";
    }
    return {};
}

std::wstring PickFile(const wchar_t* filter, const wchar_t* title) {
    wchar_t buf[MAX_PATH * 2] = {};
    OPENFILENAMEW ofn = {sizeof(ofn)};
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&ofn) ? std::wstring(buf) : std::wstring();
}

bool Chip(const char* label, bool on) {
    ImGui::PushStyleColor(ImGuiCol_Button, on ? ui::ChipActiveColor() : ui::ChipColor());
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::ChipHoverColor());
    const bool hit = ImGui::SmallButton(label);
    ImGui::PopStyleColor(2);
    return hit;
}

void Make(AppState& app, const std::wstring& video) {
    const int t = g_fx.tool;
    const ToolInfo& ti = kTools[t];
    g_fx.note.clear();
    if (ti.needsVideo && (video.empty() || g_fx.probed != video || g_fx.info.width <= 0)) {
        g_fx.note = video.empty() ? "Pick a video on the right first" : "Still reading the video: try again in a moment";
        return;
    }
    const char* node = ti.ltx ? "LTXICLoRALoaderModelOnly" : t == kReshoot ? "CrossViewWarp" : nullptr;
    if (node && t != kTalk && ComfyHasNode(app.comfy.url, node) == 0) {
        g_fx.note = "ComfyUI has not loaded the new nodes yet: restart ComfyUI once, then Make it";
        return;
    }
    const std::string prompt = PromptText(g_fx.prompt[t]), negative = PromptText(g_fx.negative[t]);
    const double fps = g_fx.info.fps > 1.0 ? g_fx.info.fps : 24.0;
    // the LTX tools take up to 10 s of the clip (8k + 1 frames)
    const int all = std::max(9, static_cast<int>(std::lround(g_fx.info.seconds * fps)));
    const int frames = (std::min(all, static_cast<int>(10.0 * fps)) - 1) / 8 * 8 + 1;
    ComfyJob job;
    job.times = 1;
    const std::string name = U8(fs::path(video).filename().wstring());
    switch (t) {
    case kReframe: {
        const Aspect& a = kAspects[std::clamp(g_fx.aspect, 0, 5)];
        job.apiWorkflow = BuildReframe(g_fx.strength, prompt, frames, fps, g_fx.info.width, g_fx.info.height, a.w, a.h).dump();
        job.label = std::string("Reframe (") + a.name + "): " + name;
        break;
    }
    case kDub: case kClean: case kNight: case kWater: case kCamera: {
        if (t == kCamera && g_fx.picture.empty()) {
            g_fx.note = "Pick the picture to start from first";
            return;
        }
        if (prompt.empty()) {
            g_fx.note = "Say what it should be first";
            return;
        }
        static const char* const kPrefix[] = {"", "video/Dub", "video/CleanPlate", "video/Night", "video/Water", "video/CameraCopy"};
        job.apiWorkflow = BuildIc(*ti.files[0], g_fx.strength, prompt, negative, kSizes[std::clamp(g_fx.size, 0, 3)], frames, fps, t == kCamera, g_fx.hold, t == kDub,
                                  t == kClean || t == kNight, kPrefix[t]).dump();
        if (t == kCamera) job.rawUploads.push_back({"@first", g_fx.picture});
        job.label = std::string(ti.name) + ": " + name;
        break;
    }
    case kTalk: {
        if (g_fx.picture.empty() || g_fx.audio.empty()) {
            g_fx.note = "Pick the picture and the sound file first";
            return;
        }
        job.apiWorkflow = BuildTalk(prompt.empty() ? std::string(ti.prompt) : prompt, negative, g_fx.audioStart, g_fx.talkSeconds).dump();
        job.rawUploads = {{"@first", g_fx.picture}, {"@audio", g_fx.audio}};
        job.label = "Make a picture talk: " + U8(fs::path(g_fx.picture).filename().wstring());
        break;
    }
    case kExtend:
        job.apiWorkflow = BuildExtendVideo(prompt.empty() ? std::string(ti.prompt) : prompt, g_fx.extendSeconds, g_fx.info.width, g_fx.info.height).dump();
        job.label = "Extend (+" + std::to_string(static_cast<int>(g_fx.extendSeconds)) + " s): " + name;
        break;
    case kReshoot: {
        auto there = [](const ToolFile& f) {
            const fs::path p = FilePlace(f, false);
            return f.folder && p.parent_path().filename() == W(f.folder) ? std::string(f.folder) + "\\" + f.name : std::string(f.name);
        };
        job.apiWorkflow = BuildReshootVideo(prompt, std::min<double>(g_fx.reshootSeconds, g_fx.info.seconds), g_fx.azimuth, g_fx.elevation, g_fx.distance, g_fx.keepSound,
                                            there(kCrossView), there(kRefTurbo)).dump();
        char b[64];
        snprintf(b, sizeof(b), "New angle (%+.0f, %+.0f): ", g_fx.azimuth, g_fx.elevation);
        job.label = b + name;
        break;
    }
    default:
        return;
    }
    if (ti.needsVideo) job.rawUploads.insert(job.rawUploads.begin(), {"@clip", video});
    if (SendComfyJob(app, std::move(job))) {
        g_fx.note = std::string("Sent: ") + ti.name + " (it shows in the status bar; the new video comes up in the list on the right)";
        Toast(ToastKind::Info, ti.name, std::string("Sent to ComfyUI") + (ti.needsVideo ? ": " + name : std::string()), {}, {}, "videofx");
    }
}

} // namespace

// Under the Edits > Videos player: the tools, the one picked, what it needs, Make
void DrawVideoFx(AppState& app, const std::wstring& videoShown) {
    ImGui::PushID("videofx");
    if (!g_fx.filled) {
        g_fx.filled = true;
        for (int t = 0; t < kToolCount; ++t) g_fx.prompt[t] = kTools[t].prompt, g_fx.negative[t] = kTools[t].negative;
    }
    // tests: MV_VIDEO_FX="tool|video|picture|audio|setting=value,..." makes that tool once on those files (whatever is on disk: its files may be
    // in a test ComfyUI's own folders); settings: aspect, size, az, el, secs, prompt
    static struct {
        bool read = false, sent = false;
        int tool = -1;
        std::wstring video;
    } hook;
    if (!hook.read) {
        hook.read = true;
        wchar_t buf[4096] = {};
        if (GetEnvironmentVariableW(L"MV_VIDEO_FX", buf, 4096)) {
            std::vector<std::wstring> f;
            std::wstring cur;
            for (const wchar_t* c = buf;; ++c) {
                if (*c == L'|' || !*c) f.push_back(cur), cur.clear();
                else cur += *c;
                if (!*c) break;
            }
            f.resize(5);
            hook.tool = std::clamp(_wtoi(f[0].c_str()), 0, kToolCount - 1);
            g_fx.tool = hook.tool;
            hook.video = f[1], g_fx.picture = f[2], g_fx.audio = f[3];
            std::wstring kv = f[4] + L",";
            for (size_t a = 0, b; (b = kv.find(L',', a)) != std::wstring::npos; a = b + 1) {
                const std::wstring s = kv.substr(a, b - a);
                const size_t eq = s.find(L'=');
                if (eq == std::wstring::npos) continue;
                const std::wstring k = s.substr(0, eq), v = s.substr(eq + 1);
                if (k == L"aspect") g_fx.aspect = _wtoi(v.c_str());
                else if (k == L"size") g_fx.size = _wtoi(v.c_str());
                else if (k == L"az") g_fx.azimuth = static_cast<float>(_wtof(v.c_str()));
                else if (k == L"el") g_fx.elevation = static_cast<float>(_wtof(v.c_str()));
                else if (k == L"secs") g_fx.extendSeconds = g_fx.talkSeconds = g_fx.reshootSeconds = static_cast<float>(_wtof(v.c_str()));
                else if (k == L"prompt") g_fx.prompt[hook.tool] = U8(v);
            }
        }
    }
    const std::wstring video = hook.tool >= 0 && !hook.video.empty() ? hook.video : videoShown;
    if (hook.tool >= 0 && !hook.sent && (!kTools[hook.tool].needsVideo || (g_fx.probed == video && g_fx.info.width > 0))) {
        hook.sent = true;
        Make(app, video);
    }
    // the clip's size, frame rate and length (ffmpeg, on a worker)
    if (g_fx.probeFut.valid() && g_fx.probeFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) g_fx.info = g_fx.probeFut.get(), g_fx.probed = g_fx.probing;
    if (!video.empty() && video != g_fx.probed && video != g_fx.probing && !g_fx.probeFut.valid()) {
        g_fx.probing = video;
        g_fx.info = VideoInfo{};
        g_fx.probeFut = std::async(std::launch::async, [video]() { return ProbeVideoFfmpeg(video); });
    }
    if (g_dl.finished.exchange(false)) {
        std::lock_guard<std::mutex> lock(g_dl.m);
        g_fx.note = g_dl.error.empty() ? "Downloaded: ready to Make" : "Could not download: " + g_dl.error;
    }
    ui::SectionHeading("VIDEO TOOLS");
    for (int t = 0; t < kToolCount; ++t) {
        if (t) ui::SameLineIfFits(kTools[t].name);
        if (Chip(kTools[t].name, g_fx.tool == t)) g_fx.tool = t, g_fx.note.clear();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", kTools[t].about);
    }
    const int t = g_fx.tool;
    const ToolInfo& ti = kTools[t];
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", ti.about);
    ImGui::PopTextWrapPos();
    // what it needs
    std::vector<const ToolFile*> files;
    const std::string missing = Missing(t, &files);
    if (!missing.empty() && missing != "files") {
        ImGui::TextColored(ui::kWarn, "Needs %s.", missing.c_str());
    } else if (!files.empty()) {
        double gb = 0;
        bool gated = false;
        for (const ToolFile* f : files) gb += f->gb, gated |= f->gated;
        if (g_dl.running) {
            std::lock_guard<std::mutex> lock(g_dl.m);
            ImGui::TextColored(ui::kAccentHover, "Downloading %d of %d: %s...", g_dl.done.load() + 1, g_dl.total.load(), g_dl.what.c_str());
        } else {
            char label[96];
            snprintf(label, sizeof(label), "Set up %s (%.1f GB)", ti.name, gb);
            if (ImGui::SmallButton(label)) StartDownload(files);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
                std::string list;
                for (const ToolFile* f : files) list += std::string("\n") + f->name + "  (" + f->repo + (f->gated ? ", needs your Hugging Face login" : "") + ")";
                ImGui::SetTooltip("Downloads into ComfyUI's models folder:%s", list.c_str());
            }
            std::string gatedRepo;
            {
                std::lock_guard<std::mutex> lock(g_dl.m);
                gatedRepo = g_dl.gatedRepo;
            }
            if (gated || !gatedRepo.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("(Lightricks' repo: log in on huggingface.co and accept its licence first)");
                const char* repo = !gatedRepo.empty() ? gatedRepo.c_str() : nullptr;
                for (const ToolFile* f : files) repo = repo ? repo : f->gated ? f->repo : nullptr;
                if (repo) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Open its page")) {
                        const std::wstring url = L"https://huggingface.co/" + W(repo);
                        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    }
                }
            }
        }
    }
    // its settings
    const float w = ImGui::GetContentRegionAvail().x;
    if (t == kReframe) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("New shape:");
        for (int a = 0; a < 6; ++a) {
            ImGui::SameLine(0, ui::Dp(4.0f));
            char l[32];
            snprintf(l, sizeof(l), "%s##asp", kAspects[a].name);
            if (Chip(l, g_fx.aspect == a)) g_fx.aspect = a;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%d x %d", kAspects[a].w, kAspects[a].h);
        }
    }
    if (t == kCamera || t == kTalk) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled(t == kCamera ? "Start from this picture:" : "The picture:");
        if (!g_fx.picture.empty()) {
            ImGui::SameLine();
            const AppState::LibThumb lt = app.LibraryThumb(g_fx.picture);
            const float th = ui::Dp(40.0f);
            if (lt.srv) ImGui::Image((ImTextureID)(intptr_t)lt.srv, ImVec2(th, th));
            else ImGui::Button("?##pic", ImVec2(th, th));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nClick: take it out", U8(g_fx.picture).c_str());
            if (ImGui::IsItemClicked()) g_fx.picture.clear();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Gallery's picture")) {
            if (app.selectedIndex >= 0 && app.selectedIndex < static_cast<int>(app.entries.size())) g_fx.picture = app.entries[app.selectedIndex].path;
            else g_fx.note = "Pick a picture in the gallery first";
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("File...##pic")) {
            const std::wstring f = OpenImageFileDialog(GetActiveWindow());
            if (!f.empty()) g_fx.picture = f;
        }
        if (t == kCamera) ImGui::TextDisabled("The video on the right is the camera move it copies.");
    }
    if (t == kTalk) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("The sound:");
        ImGui::SameLine();
        ImGui::TextUnformatted(g_fx.audio.empty() ? "none yet" : U8(fs::path(g_fx.audio).filename().wstring()).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("File...##audio")) {
            const std::wstring f = PickFile(L"Sound\0*.wav;*.mp3;*.flac;*.ogg;*.m4a\0All files\0*.*\0", L"A voice or a song");
            if (!f.empty()) g_fx.audio = f;
        }
        ImGui::SetNextItemWidth(ui::Dp(120.0f));
        ImGui::DragFloat("from (s)", &g_fx.audioStart, 0.1f, 0.0f, 600.0f, "%.1f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ui::Dp(140.0f));
        ImGui::SliderFloat("seconds##talk", &g_fx.talkSeconds, 2.0f, 10.0f, "%.0f s");
    }
    if (t == kExtend) {
        ImGui::SetNextItemWidth(ui::Dp(160.0f));
        ImGui::SliderFloat("seconds to add", &g_fx.extendSeconds, 3.0f, 10.0f, "%.0f s");
        ImGui::SameLine();
        ImGui::TextDisabled("(at the Video page's quality and size)");
    }
    if (t == kReshoot) {
        ImGui::SetNextItemWidth(ui::Dp(150.0f));
        ImGui::SliderFloat("around (deg)", &g_fx.azimuth, -90.0f, 90.0f, "%+.0f");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("How far the camera moves round the subject: minus to the left, plus to the right");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ui::Dp(150.0f));
        ImGui::SliderFloat("height (deg)", &g_fx.elevation, -30.0f, 60.0f, "%+.0f");
        ImGui::SetNextItemWidth(ui::Dp(150.0f));
        ImGui::SliderFloat("distance", &g_fx.distance, 0.5f, 2.0f, "%.2f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ui::Dp(150.0f));
        ImGui::SliderFloat("seconds##reshoot", &g_fx.reshootSeconds, 2.0f, 10.0f, "%.0f s");
        ImGui::Checkbox("Keep the clip's sound", &g_fx.keepSound);
    }
    if (ti.ltx && t != kTalk && t != kReframe) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Size:");
        for (int s = 0; s < 4; ++s) {
            ImGui::SameLine(0, ui::Dp(4.0f));
            char l[24];
            snprintf(l, sizeof(l), "%dp##sz", kSizes[s]);
            if (Chip(l, g_fx.size == s)) g_fx.size = s;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("The short side it is made at (544p is what these LoRAs were shown at; bigger is slower)");
    }
    if (ti.ltx && t != kTalk) {
        ImGui::SetNextItemWidth(ui::Dp(150.0f));
        ImGui::SliderFloat("strength", &g_fx.strength, 0.5f, 1.3f, "%.2f");
        if (t == kCamera) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ui::Dp(150.0f));
            ImGui::SliderFloat("stick to the picture", &g_fx.hold, 0.3f, 1.0f, "%.2f");
        }
    }
    // the words
    ImGui::TextDisabled("%s", ti.promptLabel);
    PromptBox("##fxprompt", g_fx.prompt[t], ImVec2(w, ui::Dp(52.0f)));
    if (ti.ltx && t != kReframe) {
        ImGui::TextDisabled("Must not appear");
        PromptBox("##fxneg", g_fx.negative[t], ImVec2(w, ui::Dp(30.0f)));
    }
    if (ti.needsVideo && g_fx.probed == video && g_fx.info.seconds > 0) {
        if (ti.ltx && g_fx.info.seconds > 10.0) ImGui::TextColored(ui::kWarn, "Only its first 10 s are used (this one is %.0f s).", g_fx.info.seconds);
        else ImGui::TextDisabled("%d x %d, %.1f s at %.0f fps", g_fx.info.width, g_fx.info.height, g_fx.info.seconds, g_fx.info.fps);
    }
    ImGui::BeginDisabled(!missing.empty() || (ti.needsVideo && video.empty()));
    if (ui::PrimaryButton((std::string("Make: ") + ti.name).c_str())) Make(app, video);
    ImGui::EndDisabled();
    if (!g_fx.note.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ui::kAccent, "%s", g_fx.note.c_str());
    }
    ImGui::PopID();
}
