// The Video page: videos with sound from MiniMax H3 (ComfyUI's own nodes). A start picture (or none: from the prompt alone), an optional end picture,
// the prompt (what happens, and what it sounds like), quality (Wuli Turbo 4 steps / the 8-step turbo LoRA / the model alone at 20 steps), resolution,
// shape, length, and what it made. Every run gets its own API workflow, written here and sent with SendH3Workflow (comfy_ui.cpp).
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <windows.h>
#include <d3d11.h>
#include <commdlg.h>
#include <shellapi.h>

#include <atomic>
#include <set>
#include <deque>
#include <random>
#include <map>
#include <sstream>
#include <thread>
#include <mutex>

#include "app_state.h"
#include "board.h"
#include "clipboard_image.h"
#include "clef.h"
#include "comfy.h"
#include "video_player.h"
#include "video_util.h"
#include "vision_prompt.h"
#include "image_texture.h"
#include "win_dialogs.h"
#include "imgui.h"
#include "imgui_internal.h" // RenderTextEllipsis
#include "json.hpp"
#include "music_lyrics.h" // ReplaceSongFile
#include "notify.h"
#include "ui_extra.h"
#include "ui_theme.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

int ApplyLoras(json& graph, const std::vector<LoraEntry>& loras, bool replaceOwn); // (comfy.cpp)

namespace {

const char* const kUnet = "minimax_h3_fl2va_pruned_int8_convrot.safetensors";
// Animate a picture (and long videos) on WarmBloodAban's Singularity, a fusion fine-tune of H3's ref, fl and base checkpoints, pruned to 4-bit
// weights (w4a8, native in ComfyUI 0.42) when it is in diffusion_models; the same workflow and LoRAs. Tested 2026-10-22 (RTX 3090, 640p, 5 s,
// Fast + Veda, a fresh ComfyUI each): about 90 s against 120 s, ComfyUI's RAM 27 GB against 32 GB at peak, the same picture held and timeline
// followed (a touch calmer motion). The 360-degree orbit keeps kUnet (its LoRA was trained on it).
const char* const kSingularity = "Minimax-h3_Singularity_ref2va_v1.3_Pruned_w4a8.safetensors";
// Since 2026-10-26 the default ref2va: OliviaRossi's PRISM merge of Singularity ref2va v2 with TenStrip's 10Eros-Max
// 8-step turbo (about 55% of the turbo blended in), as its w6a8 file (15.9 GB: 6-bit weights, native in ComfyUI 0.42 - between the 4-bit
// Singularity's 11.8 GB and int8's 19.3 GB). Tested (RTX 3090, 1152 x 640, 5 s, Veda, the rainy-street timeline, same seed): with the Fast
// LoRA the same 11.3 s a step as Singularity, and clearly sharper (Singularity smeared a girl's arm; PRISM kept faces, hands and raindrops
// crisp), the puddle jump and the dog shaking off happened, and the sound came in order (laughing, a bark, "Again!"; Singularity shouted a
// stray "Help!"). Alone at 8 steps (its own turbo) it was as good, at twice the time.
const char* const kPrism = "Minimax-h3_Singularity_x_10Eros-Max_PRISM_w6a8.safetensors";
const char* const kPrismUrl = "https://huggingface.co/OliviaRossi/Minimax-h3_Singularity-x-10Eros-Max_PRISM/resolve/main/Minimax-h3_Singularity_x_10Eros-Max_PRISM_w6a8.safetensors";
const char* const kPrismSha = "8960bca3a44e633e327562de72f17cf56850920374da497cde8dab23bcb1bca1";
bool HavePrism() {
    static double asked = -1000.0;
    static bool have = false;
    if (ImGui::GetTime() - asked > 20.0) {
        asked = ImGui::GetTime();
        const std::wstring models = ComfyModelsDir();
        std::error_code ec;
        have = !models.empty() && fs::exists(fs::path(models) / L"diffusion_models" / kPrism, ec);
    }
    return have;
}
std::string H3Unet() {
    static double asked = -1000.0;
    static bool have = false;
    if (ImGui::GetTime() - asked > 20.0) {
        asked = ImGui::GetTime();
        const std::wstring models = ComfyModelsDir();
        std::error_code ec;
        have = !models.empty() && fs::exists(fs::path(models) / L"diffusion_models" / kSingularity, ec);
    }
    return HavePrism() ? kPrism : have ? kSingularity : kUnet;
}
const char* const kTextEncoder = "qwen3vl_32b_minimax_h3_nvfp4_awq.safetensors";
const char* const kVideoVae = "minimax_h3_video_vae_int8_convrot.safetensors";
const char* const kAudioVae = "minimax_h3_audio_vae_fp32.safetensors";
// The reference model's turbo LoRAs, the first there is used. lightx2v's 4-step v1.0 (768p) first: tested 2026-10-20 on the reference model
// against Comfy-Org's 4-step v0.1, natural skin where v0.1 over-contrasts, and 54 s against 88 s for the 8-step one.
const char* const kRefLoras[] = {"minimax_h3_ref2v_turbo_4step_v1.0_768p_comfyui_bf16.safetensors", "minimax_h3_ref2v_turbo_8step_v1.0_768p_comfyui_bf16.safetensors",
                                 "minimax_h3_ref2v_turbo_4step_v0.1_comfyui_bf16.safetensors"};
// Recreate's Fast (Star in a video > Recreate the clip with them): FastVideo's FastH3-Omni, the reference model distilled to 8 steps (cfg 1, shift 12/3) - its own model, not a LoRA.
// Tested 2026-10-21 (RTX 3090, 480p, 5 s): with Veda as fast as the reference model with the 8-step LoRA and Veda (66 against 64 s; 180 against
// 194 s with a dance clip as <Video 1>), the same likeness, and closer to the prompt (the reference model turned "a handheld push-in" into
// a selfie arm like the picture's). Needs ComfyUI 0.42 or newer (it stacks 32 output heads). Swaps keep the reference model (the Character
// Swap LoRA was trained on it).
const char* const kOmniModel = "fasth3_omniref_8step_pruned_int8_convrot.safetensors";
const char* const kOmniUrl = "https://huggingface.co/FastVideo/FastH3-Omni-Comfy/resolve/main/diffusion_models/fasth3_omniref_8step_pruned_int8_convrot.safetensors";
const char* const kOmniSha = "851b2dc9670ed864b7f1e951ac9da047d83b1c04ded9bfcc1a9e5bba0ee33f80";

// ---- Swap the person: Alissonerdx's BFS Character Swap LoRA for ref2va. The clip goes in as a GUIDE (MiniMaxH3AddGuide at frame 0: its
// motion, camera, place and timing are anchored in the latent) and the picture as <Picture 1> (who); the prompt starts "body_swap:" and must
// describe the new person's look. The plain reference route (ref_videos) blends the two people. Speed as in wangweihong's comparison
// (comfyui-ecosystem, BFS-Character-Swap, 15 s at 1 MP on a 5090): the ref2v 4-step turbo LoRA with Veda's R2VA predictor took 353 s,
// against 632 s turbo alone and 993 s for 16 steps with Veda.
const char* const kSwapLora = "minimax_h3_character_swap_v1.0_ref2va_r64_gl.safetensors";
const char* const kSwapLoraUrl = "https://huggingface.co/Alissonerdx/Minimax-H3-ComfyUI/resolve/main/loras/minimax_h3_character_swap_v1.0_ref2va_r64_gl.safetensors";
const char* const kSwapLoraSha = "643a3fe63646fee5aebb181a7e1c9f8a1feb5f472709f37e22eeec290cbb75e3";
const char* const kSwapTurbo = "minimax_h3_ref2v_turbo_4step_v1.0_768p_comfyui_bf16.safetensors"; // lightx2v's (see kRefLoras)
const char* const kSwapTurboUrl = "https://huggingface.co/lightx2v/Minimax-h3-Turbo/resolve/main/minimax_h3_ref2v_turbo_4step_v1.0_768p_comfyui_bf16.safetensors";
const char* const kSwapTurboSha = "b4da466e43588004097dfbcde245ccc439c34a26bc997cb76a0b53a1d549ea0d";
const char* const kFastLora = "minimax_h3_turbo_4step_wuli_v1.0_comfyui.safetensors";     // tools\h3turbo converts it
const char* const kBalancedLora = "MiniMax-H3-FL2VA-Acc-8Step_pruned_comfy.safetensors"; // Alibaba's accelerator, Kijai/MiniMax-H3-experimental (loras)
const char* const kPresenterLora = "taomate_h3_fl2va_step300_comfyui.safetensors"; // TaoMate-H3-FL2VA, tools\h3turbo\setup_taomate.ps1 converts it
const char* const kCrispLora = "open_h3_regenerate_2k_comfyui.safetensors"; // Make it crisp (Open-H3-Regenerate; tools\h3turbo converts it)
const char* const kOrbitLora = "minimax_h3_360_orbit_flf2v_v1.safetensors"; // a character's 360-degree view (Characters tab; see MakeCharacterOrbit)
const char* const kBalancedUrl = "https://huggingface.co/Kijai/MiniMax-H3-experimental/resolve/main/loras/MiniMax-H3-FL2VA-Acc-8Step_pruned_comfy.safetensors";
// Veda sparse attention (github.com/veda-sparse/Veda-on-ComfyUI): a learned predictor picks the ~10% of attention that matters; tools\veda sets it up
const char* const kVedaPredictor = "minimax_h3_t2va_veda_8nfe_600step_preview_fp8.safetensors"; // text and picture to video (T2VA / FL2VA)
const char* const kVedaStarPredictor = "minimax_h3_r2va_veda_preview_fp8.safetensors";             // reference to video (Star in a video)
const double kVedaTime = 0.4; // the sampling time with Veda, against without: measured 15.5 against 42.9 s a step (RTX 3090, 768p, 5 s, Balanced)

// Veda's node: the API format needs its advanced inputs too (these are its defaults: 32 key tiles kept per query tile)
json VedaNode(const json& model, const char* predictor) {
    return {{"model", model}, {"predictor", predictor}, {"generated_sparsity", "32"}, {"reference_sparsity", "32"},
            {"full_attention_layers", ""}, {"full_attention_steps", ""}, {"verbose", false}};
}

struct Quality {
    const char* name;
    const char* about;
    const char* lora; // nullptr = the model alone
    int steps;
    const char* sampler;
    double shiftVideo, shiftAudio;
};
const Quality kQuality[] = {
    {"Fast", "Wuli Turbo: 4 steps (fine detail and text can drift)", kFastLora, 4, "euler", 6.0, 3.0},
    {"Balanced", "Alibaba's 8-step accelerator: steadier than Fast, about twice its time", kBalancedLora, 8, "euler", 12.0, 3.0},
    {"Best", "the model alone, no LoRA: 20 steps (12 on PRISM, which has a turbo in it), about 3-5x slower", nullptr, 20, "res_multistep", 12.0, 3.0},
    // tested against Fast (same frames and seed, 480p, 5 s): a presenter talking to the camera came out as good in 46 s against 56; a
    // two-person walk drifted (the light and the place changed) and lost a line of dialogue
    {"Presenter", "TaoMate 3 steps: for a person talking to the camera, about 15% quicker than Fast; other scenes can drift", kPresenterLora, 3, "euler", 12.0, 3.0},
    // not H3: Lightricks' LTX-2.5 (BuildLtx). Tested 2026-10-23 against Singularity + Fast (the rainy-street picture, 640p, RTX 3090): 89 s for 5 s
    // from a cold start against 93 s, 93 s for 10 s with it loaded (about 3x quicker), 31 GB of RAM against 27; cinematic and its own sound, but
    // it skipped parts of a busy timeline (the dog left after a second, nobody jumped in the puddle)
    {"LTX-2.5", "Lightricks' LTX-2.5: the quickest, cinematic, its own sound; its prompts are written its own way (one flowing paragraph). "
                "768p and up show it at its best (it was made for 1024 x 1536)", nullptr, 8, "euler_ancestral", 0.0, 0.0},
};
constexpr int kLtxQuality = 4;
constexpr int kQualityCount = static_cast<int>(sizeof(kQuality) / sizeof(kQuality[0]));
// The settings a quality makes with on the model there is. PRISM's Best: 12 steps at shift 10/3, as its turbo half (10Eros-Max, "an 8-12
// step model") asks - tested 2026-10-26 against 20 steps at 12/3 (same picture and seed): the same faces, splashes and motion in 153 s
// against 250 s. The other qualities' LoRAs work on it as they are (Balanced: 110 s, as steady as the model's own 8 steps).
Quality QualityFor(int quality) {
    Quality q = kQuality[std::clamp(quality, 0, kQualityCount - 1)];
    if (quality == 2 && HavePrism()) q.steps = 12, q.shiftVideo = 10.0;
    return q;
}

struct Res {
    const char* name;
    int w, h; // landscape
};
const Res kRes[] = {{"480p", 864, 480}, {"640p", 1152, 640}, {"768p", 1344, 768}, {"1080p", 1920, 1088}};
const char* const kShapes[] = {"Like the picture", "Landscape 16:9", "Portrait 9:16", "Square"};

int Snap32(double v) { return std::max(32, static_cast<int>(std::lround(v / 32.0)) * 32); }

// The video's size for these settings (srcW/H: the start picture, 0 = none)
void H3Size(const H3Options& o, int srcW, int srcH, int& w, int& h) {
    const Res& r = kRes[std::clamp(o.res, 0, 3)];
    const double px = static_cast<double>(r.w) * r.h;
    int shape = o.shape;
    if (shape == 0 && (srcW <= 0 || srcH <= 0)) shape = 1;
    if (shape == 1) w = r.w, h = r.h;
    else if (shape == 2) w = r.h, h = r.w;
    else if (shape == 3) w = h = Snap32(std::sqrt(px));
    else {
        const double ar = static_cast<double>(srcW) / srcH;
        w = Snap32(std::sqrt(px * ar));
        h = Snap32(std::sqrt(px / ar));
    }
}

// A long video (over 15 s, H3 Continuum): how many parts, and how long each
constexpr double kLongMax = 60.0;
void LongPlan(double seconds, int& chunks, double& chunkSeconds) {
    chunks = std::clamp(static_cast<int>(std::ceil(seconds / 10.0 - 1e-6)), 2, 16); // parts of 10 s at most (5-15 s is what it was made for)
    chunkSeconds = std::max(4.0, seconds / chunks);
}

int FramesFor(double seconds) { // what the node snaps to: 17k + 5 frames at 24 fps
    const int f = std::max(5, static_cast<int>(std::lround(std::clamp(seconds, 3.0, 15.0) * 24.0)));
    return f + (5 - f % 17 + 17) % 17;
}

// Rough time on this kind of PC: measured 205 s for 1344 x 768 (picture-shaped), 124 frames, 4 steps, with loading.
double EstimateSeconds(const H3Options& o, int w, int h) {
    if (o.seconds > 15.0 + 1e-6 || o.parts >= 2) { // a long video: its parts, a little quicker together than one by one (measured 365 s against 3 x 155 estimated)
        int chunks = 2;
        double chunkSeconds = 8.0;
        LongPlan(o.seconds, chunks, chunkSeconds);
        if (o.parts >= 2) chunks = o.parts, chunkSeconds = o.partSeconds > 0 ? o.partSeconds : o.seconds / o.parts;
        H3Options part = o;
        part.seconds = chunkSeconds;
        part.parts = 0;
        part.times = 1;
        return (35.0 + (EstimateSeconds(part, w, h) - 35.0) * chunks * 0.85) * std::max(1, o.times);
    }
    const double tokens = (static_cast<double>(w) * h / (1344.0 * 768.0)) * (FramesFor(o.seconds) / 124.0);
    if (o.quality == kLtxQuality) // (measured 2026-10-26, warm: 5 s at 640 x 960 in 28 s, at 1024 x 1536 in 78 s; a cold start adds about a minute)
        return (6.0 + 52.0 * (static_cast<double>(w) * h / 1e6) * (std::min(o.seconds, 15.0) / 5.0)) * std::max(1, o.times) + 20.0;
    return 35.0 + 170.0 * std::pow(tokens, 1.25) * (QualityFor(o.quality).steps / 4.0) * (o.veda ? kVedaTime : 1.0) * std::max(1, o.times);
}

std::wstring DataDir() {
    wchar_t buf[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    return std::wstring(buf) + L"\\Try2Create";
}

std::wstring PickPicture(const wchar_t* title) {
    wchar_t buf[MAX_PATH * 2] = {};
    OPENFILENAMEW ofn = {sizeof(ofn)};
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = L"Pictures\0*.png;*.jpg;*.jpeg;*.webp;*.bmp\0All files\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&ofn) ? std::wstring(buf) : std::wstring();
}

// The workflow of one run (API format)
bool FizgigOn();                   // (below: they read the page state)
json FizgigNode(const json& model);

json BuildH3(const H3Options& o, bool first, bool last, int w, int h) {
    const Quality q = QualityFor(o.quality);
    json wf;
    auto node = [&](const char* id, const char* cls, json inputs) { wf[id] = {{"class_type", cls}, {"inputs", std::move(inputs)}, {"_meta", {{"title", cls}}}}; };
    node("1", "UNETLoader", {{"unet_name", H3Unet()}, {"weight_dtype", "default"}});
    json model = json::array({"1", 0});
    if (q.lora) {
        node("2", "LoraLoaderModelOnly", {{"model", model}, {"lora_name", q.lora}, {"strength_model", 1.0}}); // the turbo heads only work at exactly 1.0
        model = json::array({"2", 0});
    }
    if (FizgigOn()) {
        node("22", "FizgigH3Tweaks", FizgigNode(model));
        model = json::array({"22", 0});
    }
    node("3", "MiniMaxH3SigmaShift", {{"model", model}, {"shift_video", q.shiftVideo}, {"shift_audio", q.shiftAudio}});
    model = json::array({"3", 0});
    if (o.veda) { // an attention override: after the LoRA, last before the sampler
        node("21", "VedaSparseAttention", VedaNode(model, kVedaPredictor));
        model = json::array({"21", 0});
    }
    node("4", "CLIPLoader", {{"clip_name", kTextEncoder}, {"type", "minimax"}, {"device", "default"}});
    node("5", "VAELoader", {{"vae_name", kVideoVae}});
    node("6", "VAELoader", {{"vae_name", kAudioVae}});
    json i2v = {{"clip", json::array({"4", 0})}, {"vae", json::array({"5", 0})}, {"prompt", ""}, {"width", w}, {"height", h}, {"length", FramesFor(o.seconds)}};
    if (first) {
        node("7", "LoadImage", {{"image", "example.png"}});
        if (o.shape == 0) { // its own shape at the chosen size
            const Res& r = kRes[std::clamp(o.res, 0, 3)];
            node("9", "ImageScaleToTotalPixels", {{"image", json::array({"7", 0})}, {"upscale_method", "lanczos"}, {"megapixels", r.w * r.h / 1.0e6}, {"resolution_steps", 32}});
            node("10", "GetImageSize", {{"image", json::array({"9", 0})}});
            i2v["width"] = json::array({"10", 0});
            i2v["height"] = json::array({"10", 1});
        } else { // another shape: cut to it (the model would stretch it)
            node("9", "ImageScale", {{"image", json::array({"7", 0})}, {"upscale_method", "lanczos"}, {"width", w}, {"height", h}, {"crop", "center"}});
        }
        i2v["first_frame"] = json::array({"9", 0});
    }
    if (last) {
        node("20", "LoadImage", {{"image", "example.png"}}); // the node crops it to the video's shape itself
        i2v["last_frame"] = json::array({"20", 0});
    }
    node("8", "MiniMaxH3ImageToVideo", i2v);
    node("11", "RandomNoise", {{"noise_seed", 42}});
    node("12", "KSamplerSelect", {{"sampler_name", q.sampler}});
    node("13", "BasicScheduler", {{"model", model}, {"scheduler", "simple"}, {"steps", q.steps}, {"denoise", 1.0}});
    node("14", "BasicGuider", {{"model", model}, {"conditioning", json::array({"8", 0})}});
    node("15", "SamplerCustomAdvanced", {{"noise", json::array({"11", 0})}, {"guider", json::array({"14", 0})}, {"sampler", json::array({"12", 0})},
                                         {"sigmas", json::array({"13", 0})}, {"latent_image", json::array({"8", 1})}});
    node("16", "VAEDecode", {{"samples", json::array({"15", 0})}, {"vae", json::array({"5", 0})}});
    node("17", "VAEDecodeAudio", {{"samples", json::array({"15", 0})}, {"vae", json::array({"6", 0})}});
    node("18", "CreateVideo", {{"images", json::array({"16", 0})}, {"audio", json::array({"17", 0})}, {"fps", 24.0}});
    node("19", "SaveVideo", {{"video", json::array({"18", 0})}, {"filename_prefix", std::string("video/H3_") + q.name}, {"format", "auto"}, {"format.codec", "auto"}});
    return wf;
}

// ---- LTX-2.5 (Lightricks, LTX-2.x community licence; a quality since 2026-10-23): Comfy-Org's video_ltx2_5_i2v template -
// the distilled 22B model (int8), a first pass at half size (8 sigmas), the latent doubled by its spatial upscaler, 3 more sigmas, the sound
// made with the picture. With an end picture, its FLF2V template instead: one pass at full size, the first and last frames as guides.
// The repo is gated: its files are fetched with a Hugging Face login (they cannot be one-click downloads here).
const char* const kLtxModel = "ltx-2.5-22b-distilled-transformer-comfy-int8-convrot.safetensors";
const char* const kLtxTextEncoder = "gemma4-12b-with-proj-ltx-2.5-comfy-int8-convrot.safetensors";
const char* const kLtxVideoVae = "ltx-2.5-video-vae-bf16.safetensors";          // (the diffusion decoder)
const char* const kLtxConvVae = "ltx-2.5-video-vae-conv-bf16.safetensors";       // the conv decoder: used when it is there
bool g_ltxConv = false;                                                          // (found by LtxReady)
const char* const kLtxAudioVae = "ltx-2.5-audio-vae-bf16.safetensors";
const char* const kLtxUpscaler = "ltx-2.5-latent-spatial-upscaler-x2-bf16-1.0.safetensors";
bool LtxReady() {
    static double at = -1000.0;
    static bool ok = false;
    if (ImGui::GetTime() - at > 10.0) {
        at = ImGui::GetTime();
        const std::wstring models = ComfyModelsDir();
        std::error_code ec;
        auto there = [&](const wchar_t* sub, const char* name) { return fs::exists(fs::path(models) / sub / name, ec); };
        g_ltxConv = !models.empty() && there(L"vae", kLtxConvVae);
        ok = !models.empty() && there(L"diffusion_models", kLtxModel) && there(L"text_encoders", kLtxTextEncoder) && (g_ltxConv || there(L"vae", kLtxVideoVae)) &&
             there(L"vae", kLtxAudioVae) && there(L"latent_upscale_models", kLtxUpscaler);
    }
    return ok;
}

// "@first" / "@last" are the uploaded pictures; frames 8n + 1 at 24 fps; w and h multiples of 64 (the first pass is half of them)
json BuildLtx(const std::string& prompt, double seconds, bool first, bool last, int w, int h) {
    json wf;
    auto node = [&](const std::string& id, const char* cls, json inputs) { wf[id] = {{"class_type", cls}, {"inputs", std::move(inputs)}, {"_meta", {{"title", cls}}}}; };
    auto L = [](const std::string& id, int k = 0) { return json::array({id, k}); };
    const int frames = 8 * static_cast<int>(std::lround(std::clamp(seconds, 1.0, 15.0) * 24.0 / 8.0)) + 1;
    node("1", "UNETLoader", {{"unet_name", kLtxModel}, {"weight_dtype", "default"}});
    node("2", "CLIPLoader", {{"clip_name", kLtxTextEncoder}, {"type", "ltxv"}, {"device", "default"}});
    node("3", "VAELoader", {{"vae_name", g_ltxConv ? kLtxConvVae : kLtxVideoVae}});
    node("4", "VAELoader", {{"vae_name", kLtxAudioVae}});
    node("20", "CLIPTextEncode", {{"clip", L("2")}, {"text", prompt}});
    node("21", "CLIPTextEncode", {{"clip", L("2")}, {"text", "pc game, console game, video game, cartoon, childish, ugly"}}); // (the template's)
    node("22", "LTXVConditioning", {{"positive", L("20")}, {"negative", L("21")}, {"frame_rate", 24.0}});
    auto picture = [&](const std::string& id, const char* file) { // the picture at the video's size, as the model wants it
        node(id, "LoadImage", {{"image", file}});
        node(id + "1", "ImageScale", {{"image", L(id)}, {"upscale_method", "lanczos"}, {"width", w}, {"height", h}, {"crop", "center"}});
        node(id + "2", "LTXVPreprocess", {{"image", L(id + "1")}, {"img_compression", 18}});
        return L(id + "2");
    };
    node("32", "LTXVEmptyLatentAudio", {{"frames_number", frames}, {"frame_rate", 24}, {"batch_size", 1}, {"audio_vae", L("4")}});
    node("34", "RandomNoise", {{"noise_seed", 42}});
    node("35", "KSamplerSelect", {{"sampler_name", "euler_ancestral"}});
    node("36", "ManualSigmas", {{"sigmas", "1.0, 0.99375, 0.9875, 0.98125, 0.975, 0.909375, 0.725, 0.421875, 0.0"}});
    json videoOut, audioOut;
    if (last) { // the FLF2V way: one pass at full size, guided at the first and the last frame
        node("30", "EmptyLTXVLatentVideo", {{"width", w}, {"height", h}, {"length", frames}, {"batch_size", 1}});
        json pos = L("22", 0), neg = L("22", 1), lat = L("30");
        if (first) {
            node("60", "LTXVAddGuide", {{"positive", pos}, {"negative", neg}, {"vae", L("3")}, {"latent", lat}, {"image", picture("10", "@first")}, {"frame_idx", 0}, {"strength", 0.7}});
            pos = L("60", 0), neg = L("60", 1), lat = L("60", 2);
        }
        node("61", "LTXVAddGuide", {{"positive", pos}, {"negative", neg}, {"vae", L("3")}, {"latent", lat}, {"image", picture("15", "@last")}, {"frame_idx", -1}, {"strength", 0.7}});
        node("33", "LTXVConcatAVLatent", {{"video_latent", L("61", 2)}, {"audio_latent", L("32")}});
        node("37", "LTXVDualCFGGuider", {{"model", L("1")}, {"positive", L("61", 0)}, {"negative", L("61", 1)}, {"video_cfg", 1.0}, {"audio_cfg", 1.0}});
        node("38", "SamplerCustomAdvanced", {{"noise", L("34")}, {"guider", L("37")}, {"sampler", L("35")}, {"sigmas", L("36")}, {"latent_image", L("33")}});
        node("39", "LTXVSeparateAVLatent", {{"av_latent", L("38")}});
        node("62", "LTXVCropGuides", {{"positive", L("61", 0)}, {"negative", L("61", 1)}, {"latent", L("39", 0)}}); // (the guide frames out again)
        videoOut = L("62", 2), audioOut = L("39", 1);
    } else { // the I2V way: half size first, then the latent doubled and finished
        node("5", "LatentUpscaleModelLoader", {{"model_name", kLtxUpscaler}});
        node("30", "EmptyLTXVLatentVideo", {{"width", w / 2}, {"height", h / 2}, {"length", frames}, {"batch_size", 1}});
        json lat = L("30");
        json pic;
        if (first) {
            pic = picture("10", "@first");
            node("31", "LTXVImgToVideoInplace", {{"vae", L("3")}, {"image", pic}, {"latent", lat}, {"strength", 0.7}, {"bypass", false}});
            lat = L("31");
        }
        node("33", "LTXVConcatAVLatent", {{"video_latent", lat}, {"audio_latent", L("32")}});
        node("37", "LTXVDualCFGGuider", {{"model", L("1")}, {"positive", L("22", 0)}, {"negative", L("22", 1)}, {"video_cfg", 1.0}, {"audio_cfg", 1.0}});
        node("38", "SamplerCustomAdvanced", {{"noise", L("34")}, {"guider", L("37")}, {"sampler", L("35")}, {"sigmas", L("36")}, {"latent_image", L("33")}});
        node("39", "LTXVSeparateAVLatent", {{"av_latent", L("38")}});
        node("40", "LTXVLatentUpsampler", {{"samples", L("39", 0)}, {"upscale_model", L("5")}, {"vae", L("3")}});
        json up = L("40");
        if (first) {
            node("41", "LTXVImgToVideoInplace", {{"vae", L("3")}, {"image", pic}, {"latent", up}, {"strength", 1.0}, {"bypass", false}});
            up = L("41");
        }
        node("42", "LTXVConcatAVLatent", {{"video_latent", up}, {"audio_latent", L("39", 1)}});
        node("43", "RandomNoise", {{"noise_seed", 42}});
        node("44", "ManualSigmas", {{"sigmas", "0.909375, 0.725, 0.421875, 0.0"}}); // (Lightricks' STAGE_2_DISTILLED_SIGMA_VALUES for 2.5; 0.85 was an older template's)
        node("47", "SamplerCustomAdvanced", {{"noise", L("43")}, {"guider", L("37")}, {"sampler", L("35")}, {"sigmas", L("44")}, {"latent_image", L("42")}});
        node("48", "LTXVSeparateAVLatent", {{"av_latent", L("47")}});
        videoOut = L("48", 0), audioOut = L("48", 1);
    }
    node("49", "VAEDecodeTiled", {{"samples", videoOut}, {"vae", L("3")}, {"tile_size", 512}, {"overlap", 64}, {"temporal_size", 64}, {"temporal_overlap", 16}});
    node("50", "LTXVAudioVAEDecode", {{"samples", audioOut}, {"audio_vae", L("4")}});
    node("51", "CreateVideo", {{"images", L("49")}, {"audio", L("50")}, {"fps", 24.0}});
    node("52", "SaveVideo", {{"video", L("51")}, {"filename_prefix", "video/LTX"}, {"format", "auto"}, {"format.codec", "auto"}});
    return wf;
}

// ---- long videos (over 15 s): H3 Continuum (github.com/ukr8b3g-cmyk/ComfyUI-H3-Continuum, MIT) makes them in parts that continue each other,
// picture and sound. Tested 2026-10-16 (3 x 6 s, 640 x 960, Fast): its joins were as smooth as the frames around them and the sound ran
// straight through (at most 1 dB between 50 ms windows there), where three clips each started on the last one's final frame jumped 13-20 dB
// at every join; about 19% slower than those three clips (365 s against 307 s).

json BuildH3Long(const H3Options& o, bool first, bool last, int w, int h) {
    const Quality q = QualityFor(o.quality);
    int chunks = 2;
    double chunkSeconds = 8.0;
    LongPlan(o.seconds, chunks, chunkSeconds);
    if (o.parts >= 2) chunks = std::min(o.parts, 16), chunkSeconds = std::clamp(o.partSeconds > 0 ? o.partSeconds : o.seconds / o.parts, 4.0, 15.0); // (a story's scenes)
    json wf;
    auto node = [&](const char* id, const char* cls, json inputs) { wf[id] = {{"class_type", cls}, {"inputs", std::move(inputs)}, {"_meta", {{"title", cls}}}}; };
    node("1", "UNETLoader", {{"unet_name", H3Unet()}, {"weight_dtype", "default"}});
    json model = json::array({"1", 0});
    if (q.lora) {
        node("2", "LoraLoaderModelOnly", {{"model", model}, {"lora_name", q.lora}, {"strength_model", 1.0}});
        model = json::array({"2", 0});
    }
    node("3", "MiniMaxH3SigmaShift", {{"model", model}, {"shift_video", q.shiftVideo}, {"shift_audio", q.shiftAudio}});
    model = json::array({"3", 0});
    // Veda inside Continuum: tested 2026-10-16 (3 x 6 s, 640 x 960, Fast, RTX 3090): 205-218 s against 365-417 s with full attention, the joins
    // as smooth (1.2-1.3x the frames' own motion at the first join either way). From a picture in its place it stayed one clean shot; from a
    // white-studio portrait both full attention (1 of 2 seeds) and Veda (3 of 3) drifted back to the studio in the last part. (No Fizgig here:
    // not tested with Continuum.)
    if (o.veda) {
        node("21", "VedaSparseAttention", VedaNode(model, kVedaPredictor));
        model = json::array({"21", 0});
    }
    node("4", "CLIPLoader", {{"clip_name", kTextEncoder}, {"type", "minimax"}, {"device", "default"}});
    node("5", "VAELoader", {{"vae_name", kVideoVae}});
    node("6", "VAELoader", {{"vae_name", kAudioVae}});
    node("8", "KSamplerSelect", {{"sampler_name", q.sampler}});
    node("13", "BasicScheduler", {{"model", model}, {"scheduler", "simple"}, {"steps", q.steps}, {"denoise", 1.0}});
    node("10", "PrimitiveStringMultiline", {{"value", ""}}); // the prompt: parts separated by "---" lines
    json s = {{"model", model}, {"clip", json::array({"4", 0})}, {"video_vae", json::array({"5", 0})}, {"sampler", json::array({"8", 0})},
              {"sigmas", json::array({"13", 0})}, {"sequence_prompt", json::array({"10", 0})}, {"prompt_mode", "Auto"}, {"chunks", chunks},
              {"chunk_seconds", std::round(chunkSeconds * 10.0) / 10.0}, {"aspect", "Auto from First Image"}, {"preset", "Custom"}, {"custom_mp", 0.6},
              {"continuity", "Balanced \xE2\x80\x94 22 frames"}, {"base_seed", 42}, {"audio_continuity", true}, {"diagnostics", "Basic"},
              {"reroll_from_chunk", "Auto"}, {"reroll_nonce", 0}, {"strict_compatibility", true}, {"debug", false}, {"show_preview", true},
              {"run_storage", "Off"}, {"run_name", ""}, {"reference_size", "Match Output"}, {"project_id", ""}, {"video_reference_size", "Efficient - 0.4 MP"},
              {"continuation_backend", "Standard"}, {"generation_mode", "Full Run"}, {"review_action", "Continue / Next"}, {"take_group", 0},
              {"take_revision_id", ""}, {"take_action", "Automatic"}, {"size_source", "Manual"}, {"width", w}, {"height", h}};
    if (first) {
        node("7", "LoadImage", {{"image", "example.png"}});
        node("9", "ImageScale", {{"image", json::array({"7", 0})}, {"upscale_method", "lanczos"}, {"width", w}, {"height", h}, {"crop", "center"}});
        s["first_frame"] = json::array({"9", 0});
    }
    if (last) {
        node("20", "LoadImage", {{"image", "example.png"}});
        node("23", "ImageScale", {{"image", json::array({"20", 0})}, {"upscale_method", "lanczos"}, {"width", w}, {"height", h}, {"crop", "center"}});
        s["last_frame"] = json::array({"23", 0});
    }
    node("11", "H3ContinuumSamplerV39", s);
    node("12", "H3DecodeCacheHelper", {{"cache_mode", "Auto"}, {"ram_budget_mb", 256}, {"disk_budget_gb", 8}, {"reset_token", 0}, {"video_samples", json::array({"11", 0})},
                                       {"video_vae", json::array({"5", 0})}, {"audio_samples", json::array({"11", 1})}, {"audio_vae", json::array({"6", 0})}});
    node("14", "H3ContinuumAssembleSeamV35", {{"images", json::array({"12", 0})}, {"audio", json::array({"12", 1})}, {"assembly_plan", json::array({"11", 2})},
                                              {"exact_total_duration", true}, {"audio_seam", "Auto"}, {"video_seam", "Auto"}, {"buffer_backend", "Auto"},
                                              {"diagnostics", "Basic"}, {"driving_audio", json::array({"11", 4})}});
    node("18", "CreateVideo", {{"images", json::array({"14", 0})}, {"audio", json::array({"14", 1})}, {"fps", 24.0}});
    node("19", "SaveVideo", {{"video", json::array({"18", 0})}, {"filename_prefix", std::string("video/H3_Long_") + q.name}, {"format", "auto"}, {"format.codec", "auto"}});
    return wf;
}

// ---- the video prompt, written for the picture
// A picture's own prompt describes a still frame: sent as it is, H3 makes a still image with small movements. The prompt writer (the local LLM,
// looking at the start picture and the end picture) turns it into what happens: action beats over the clip's length, the camera, and the sound.
// The camera, for every writer: seen 2026-10-24 - an Orbit left on from a preset card went into a first-person shooter picture ("the camera
// begins its smooth half-circle orbit", then "the orbit accelerates"): H3 swung the corridor around the player's eyes and the picture melted
#define kCameraFitRule \
    "A camera move the user asks for is a wish: use it only if it can work for this picture, else the closest one that can. In a first-person " \
    "view (the camera is someone's eyes: their hands, a weapon or a steering wheel in the foreground) the camera moves only as that person does - " \
    "walking forward, turning, looking around, a recoil - never an orbit, a crane or a dolly around them. Never speed the camera up or add more " \
    "camera moves later in the clip. Nobody reaches toward, points into, taps or touches the camera or the lens, and nobody leans into it: hands " \
    "and gestures stay in front of the body (a hand coming at the lens smears the picture). In a selfie or vlog shot (the person holds the camera "     "themselves) the camera stays in their hand the whole time: they walk and talk with it, may turn it to show the street for a moment, but never "     "walk past it, turn their back on it, put it down or look at its screen. "

// LTX-2.5's prompt (2026-10-26): its guide asks for one flowing present-tense paragraph - the shot, the action start to end, the camera, the
// sound, dialogue in quotes - and warns against prompts written for other models (shot lists, numbered beats); for image-to-video the
// advice is motion, not the look again (describing the frame again makes it drift from the picture). Tested on the same picture and
// seed: the H3 timeline form and this both spoke every line; with this the actions were followed more fully.
const char* const kLtxSystem =
    "You write prompts for LTX-2.5, an image-to-video model that makes video WITH sound. The first attached picture is the FIRST frame of the "
    "video.{LAST} Write what HAPPENS over {SECONDS} seconds as ONE flowing paragraph of 4 to 7 sentences in the present tense, the way a director "
    "describes a shot to a cinematographer: no lists, no headings, no timestamps, no brackets, no notes. In this order: the shot (its framing, e.g. "
    "a handheld medium close-up); the action from beginning to end with clear physical verbs (people move, turn, gesture, react, talk; hair, "
    "clothes, water, smoke and light move), building naturally and ending calmly; the camera (at most one gentle move: a slow push-in, a slow "
    "pan, a handheld sway, or a still camera); last, the sound (ambience, effects, music). Do NOT describe how the people, clothes or place look "
    "- the picture already shows them, and describing them again makes the video drift from it; refer to people simply (the woman, the man in "
    "the denim jacket). One continuous take, one or two people acting, calm and readable motion: no frantic action, no crowds appearing, no "
    "objects that are not there. Spoken words go in double quotes with who says them and how (she says in a warm, cheerful voice, \"...\"): at "
    "most {SPEECH} words of speech in all, short natural sentences, in the language of the user's idea. No text or captions on screen. "
    kCameraFitRule "Reply with ONLY the paragraph.";

const char* const kVideoSystem =
    "You write prompts for MiniMax H3, an image-to-video model that makes video WITH sound. The first attached picture is the FIRST frame of the video "
    "(call it <Picture 1>).{LAST} Your job: make it come alive. Describe what HAPPENS, not what the picture looks like: clear, visible, physical action "
    "(people move, turn, walk, gesture, react, talk; hair, clothes, objects, water, smoke, weather and light move), with a natural build-up over the clip. "
    "A still picture with tiny movements is a failure.\n"
    "Write it in this form:\n"
    "1. One line: the look of <Picture 1> (medium, lighting, mood) and the place, ending with \"The scene opens exactly on image 1.\"\n"
    "2. \"Timeline:\" then {BEATS} beats, each on its own line as [Xs-Ys], together covering 0 to {SECONDS} seconds. Each beat: who does what (strong verbs), "
    "what moves, and the camera (slow push-in, pan, orbit, handheld follow, tilt, static). One continuous shot unless the idea asks for cuts.\n"
    "3. If someone would speak, give the words in quotes and who says them (short: they must fit the time).\n"
    "4. \"Audio:\" one line: voices, sound effects matching the actions, ambience, music.\n"
    // (a concrete example here gets copied word for word by small writers: it once put a watch into a love story)
    "5. \"Keep:\" one line naming what must not change from image 1, exactly: the faces, the objects' shapes, any text, logos, numbers and patterns, "
    "using ONLY things you can see in image 1, then \"exactly as in image 1; nothing morphs.\"\n"
    "Use at most one or two gentle camera moves for the whole clip (a slow push-in OR an orbit, not both and a tilt): the faster and more the camera "
    "moves, the more the model reinvents details. Keep the same people, faces, clothes, style and place as the picture. Add no new characters "
    "unless the idea asks for them. " kCameraFitRule
    "Reply with ONLY the prompt (under 170 words): no preface, no notes, no markdown.";

// A long video (H3 Continuum): one section per part, the later ones written as continuations (its own examples' wording: "Seamless continuation
// of the immediately preceding clip ... No cut or reset"). Tested: one prompt for every part made each part start the action over (a pose jumped
// 0.6 s after a join); a section per part continued it.
const char* const kLongVideoSystem =
    "You write prompts for MiniMax H3, an image-to-video model that makes video WITH sound, run through H3 Continuum: one long video made as "
    "{CHUNKS} parts of {CHUNK} seconds that continue each other seamlessly ({SECONDS} seconds in all, one continuous shot). The first attached "
    "picture is the FIRST frame of the video (call it <Picture 1>).{LAST} Make it come alive: describe what HAPPENS (people move, turn, walk, "
    "gesture, react, talk; hair, clothes, water, smoke, light move), building over the whole length. A still picture with tiny movements is a failure.\n"
    "Write exactly {CHUNKS} sections, separated by a line with only --- on it.\n"
    "Section 1: one line with the look of <Picture 1> (medium, lighting, mood) and the place, ending with \"The scene opens exactly on image 1.\"; "
    "then what happens in this part (strong verbs) and the camera (one gentle move at most); then \"One continuous shot, no cuts.\"; then "
    "\"Audio:\" one line.\n"
    "Every later section starts with \"Seamless continuation of the immediately preceding clip:\" then what happens next in that part, carrying on "
    "from the part before (never start an action over or repeat one), then \"Same people, clothes, place and light. One continuous shot: no cut "
    "or reset.\", then \"Audio:\" one line.\n"
    "The last section brings the action to a natural end. If someone speaks, give the words in quotes and who says them, short enough to fit "
    "their part. Keep the same people, faces, clothes, style and place as the picture; add no new characters unless the idea asks. "
    "Each section under 90 words. Reply with ONLY the sections: no preface, no part numbers, no notes, no markdown.";

// A long video, planned whole: the writer gives ONE timeline over the full length (beats that never cross a part's border), and the app cuts
// it into the parts (SplitLongPlan), each a complete prompt in the short form with its beats on its own clock. Why: written part by part
// (kLongVideoSystem) a 30 s video came back as "he does this, then this" per part - no timing, no detail - and the parts did not add up to
// one story (seen 2026-10-20). H3 Continuum's own guide says the same: invariants in every part, and each part its current
// state, a concrete next action and the end state the next part takes over.
const char* const kLongPlanSystem =
    "You write prompts for MiniMax H3, an image-to-video model that makes video WITH sound. This video is {SECONDS} seconds long, one continuous "
    "shot, made in {CHUNKS} parts of {CHUNK} seconds that continue each other (parts start at {BOUNDS}). The first attached picture is the FIRST "
    "frame of the video (call it <Picture 1>).{LAST} Your job: make it come alive over the whole length. Describe what HAPPENS, not what the "
    "picture looks like: clear, visible, physical action (people move, turn, walk, gesture, react, talk; hair, clothes, objects, water, smoke, "
    "weather and light move) that builds like a little story: a beginning, a development and an ending in the last part. A still picture with "
    "tiny movements is a failure, and so is the same action over and over.\n"
    "Write it in this form:\n"
    "1. One line: the look of <Picture 1> (medium, lighting, mood), the place, and who is in it with their clothes, as seen.\n"
    "2. \"Timeline:\" then {BEATS} beats, each on its own line as [Xs-Ys], in order, together covering 0 to {SECONDS} seconds with no gaps. "
    "A beat never crosses a part's start ({BOUNDS}): a new part starts a new beat. Each beat 15 to 35 words: who does what (strong verbs), "
    "what moves, how it ends, and the camera (one gentle move at a time: slow push-in, pan, handheld follow, or static). Every beat carries on "
    "from the one before: never start an action over or repeat one.\n"
    "3. If someone speaks, put the words in quotes inside the beat where they are said, with who says them (short: they must fit that beat).\n"
    "4. \"Audio:\" one line: voices, sound effects matching the actions, ambience, music.\n"
    "5. \"Keep:\" one line naming what must not change, exactly: the faces, the clothes, the objects' shapes, any text, logos and patterns, using "
    "ONLY things you can see in image 1, then \"exactly as in image 1; nothing morphs.\"\n"
    "Keep the same people, faces, clothes, style and place as the picture; add no new characters unless the idea asks for them. " kCameraFitRule
    "Reply with ONLY the prompt (under {WORDS} words): no preface, no notes, no markdown.";

// Presenter (TaoMate's LoRA) was trained on livestream presenters with prompts in one fixed form: written freely, it brings the livestreams'
// habits along (subtitles of the speech, leaves and flowers drifting over the frame, a ghosted start). This is that form, filled for the picture.
const char* const kPresenterSystem =
    "You write prompts for a talking-presenter video model (MiniMax H3 with a presenter LoRA) that makes video WITH sound from a start picture "
    "(<Picture 1>).{LAST} It only works well with prompts in exactly this form, so copy the labels and the fixed sentences word for word:\n"
    "Summary: <one sentence: who presents what, where>. One continuous <language> presentation.\n"
    "Narration 1:\n"
    "<Vertical portrait | Horizontal landscape> composition. Static <seated | standing> <medium | close-up> shot retaining <what is in the frame: "
    "tabletop, hands, headroom...> in the source image. <The place in one or two sentences: what is behind and around the person, the light>. "
    "The camera, lighting, person identity, clothes and background remain unchanged. One presenter only. No cuts, zooms, scene changes or "
    "additional people. Preserve existing printed details and background objects; add no captions, subtitles, text, overlays, plants, logos or "
    "watermarks.\n"
    "Speaker_1's Appearance: <age, hair, clothes, as seen>. Preserve the face, hairstyle, clothing and proportions in the input image.\n"
    "Speaker_1's Actions: <what the person does over the clip: posture, one or two small gestures, a nod; nothing big>. Motions are continuous, "
    "moderate and anatomically natural.\n"
    "Speaker_1's Facial Expression: <expression>, with natural blinking and gaze toward the camera.\n"
    "Speaker_1's Held Objects:\n"
    "<None. Her/His hands remain empty; background objects stay in place. | the object, which stays the same>\n"
    "Speech Attribution:\n"
    "Speaker_1 says: \"<what they say: about {WORDS} words, in the language of the idea>\"\n"
    "Speaker_1's Emotion: <emotion>.\n"
    "Speaker_1's Voice Description: <an adult female/male voice, its tone and delivery, the language>.\n"
    "Sound-Visual Alignment: Clear dry speech with no music. Natural speech pace, synchronized lip and facial motion.\n"
    "Describe only what you can see in the picture; the speech follows the user's idea. Write the speech as natural talk to one person, as in a "
    "conversation: no channel greetings (\"welcome back to my channel\", \"hey everyone\"), no numbered or listed points (\"first... second...\", "
    "\"three tips\"), no headline phrases; if the idea is a list, say its points in flowing sentences. (Livestream-style speech makes the model "
    "write the words over the picture.) Reply with ONLY the prompt: no preface, no notes, no markdown.";

// Text over the picture, in a finished Presenter take. A presenter shot holds still, so words laid over it show as sharp new edges in the bands
// above the head and over the table, against the clip's first frames. Measured on 11 takes judged by eye: with text the top band's edges rose
// 3.4x to 13.6x, clean ones 1.02x-1.06x (2.55x for one where a small sprig grew on a shelf). Returns the larger band's rise (0 = not measured).
double OverlayRise(const std::wstring& video) {
    const std::wstring ff = FfmpegPath();
    if (ff.empty()) return 0;
    double worst = 0;
    for (const wchar_t* band : {L"crop=iw:ih*0.24:0:0", L"crop=iw:ih*0.22:0:ih*0.78"}) {
        std::string out;
        RunCaptureIdle(L"\"" + ff + L"\" -hide_banner -threads 2 -i \"" + video + L"\" -filter_threads 2 -vf \"" + band +
                             L",scale=384:-1,format=gray,edgedetect=low=0.08:high=0.2,signalstats,metadata=print:key=lavfi.signalstats.YAVG:file=-\" -an -f null -",
                         out, false);
        std::vector<double> v;
        for (size_t at = out.find("YAVG="); at != std::string::npos; at = out.find("YAVG=", at + 5)) v.push_back(std::atof(out.c_str() + at + 5));
        if (v.size() < 5) continue;
        const double base = (v[0] + v[1] + v[2]) / 3.0;
        std::sort(v.begin(), v.end(), std::greater<double>());
        const size_t top = v.size() / 10 + 1;
        double peak = 0;
        for (size_t k = 0; k < top; ++k) peak += v[k];
        worst = std::max(worst, peak / top / std::max(0.5, base));
    }
    return worst;
}
constexpr double kOverlayRise = 3.0;

// How much a swap is still the clip that guided it: the PSNR of their brightness, small, frame by frame (dB; 0 = not measured). Measured:
// swaps that worked 14.1 and 17.6, two that gave back the clip unchanged (the same girl, a woman for a woman with no look written) 26.8.
double LikenessToClip(const std::wstring& video, const std::wstring& clip) {
    const std::wstring ff = FfmpegPath();
    if (ff.empty()) return 0;
    std::string out;
    RunCaptureIdle(L"\"" + ff + L"\" -hide_banner -threads 2 -i \"" + video + L"\" -i \"" + clip +
                       L"\" -lavfi \"[0:v]scale=96:168,format=gray,setpts=PTS-STARTPTS[a];[1:v]fps=24,scale=96:168,format=gray,setpts=PTS-STARTPTS[b];[a][b]psnr=shortest=1\" -f null -",
                   out, true);
    const size_t at = out.rfind("average:");
    return at == std::string::npos ? 0 : std::atof(out.c_str() + at + 8);
}
constexpr double kSwapUnchanged = 23.0;

// ComfyUI decodes H3 video in windows that each keep 17 frames and cross-fade 5 more into the next; the previous window's 5 fade frames come
// out too bright and too contrasty, so a flash starts exactly on a seam (frame 17k: +25 luma, then +11, +7, +4, +2). Seen on busy, bright
// pictures at every quality (two people in maid outfits: frame 85 and 51). Measured on 20 clips: flashed seams +6.5 to +25 against their
// neighbours, every other frame (2748) within 1.2. A seam more than 3 brighter is repaired: its 5 fade frames get the brightness and contrast
// of a line between the frame before the seam and the one after the fade (motion kept), and the seam frame itself the average of its neighbours.
// The video is re-encoded only then (audio copied, the workflow tag kept). Returns the seams repaired, or -1 when it could not be checked.
// With `readyTmp`, the repaired file is left at *readyTmp for the caller to put in place (the UI thread, where a player may have it open).
int RepairDecoderSeams(const std::wstring& video, std::wstring* readyTmp = nullptr) {
    const std::wstring ff = FfmpegPath();
    if (ff.empty()) return -1;
    std::string out;
    // (idle priority and few threads, here and below: it runs while the next video generates, and must not make the PC lag)
    RunCaptureIdle(L"\"" + ff + L"\" -hide_banner -threads 2 -i \"" + video + L"\" -filter_threads 2 -vf \"signalstats,metadata=print:file=-\" -an -f null -", out, false);
    struct Stat {
        double avg = 0, low = 0, high = 0;
    };
    std::vector<Stat> st;
    for (size_t at = out.find("frame:"); at != std::string::npos;) {
        const size_t next = out.find("frame:", at + 6);
        const std::string fr = out.substr(at, next == std::string::npos ? std::string::npos : next - at);
        auto val = [&](const char* key) {
            const size_t p = fr.find(key);
            return p == std::string::npos ? -1.0 : std::atof(fr.c_str() + p + strlen(key));
        };
        st.push_back({val("YAVG="), val("YLOW="), val("YHIGH=")});
        at = next;
    }
    if (st.size() < 30) return -1;
    const double fps = 24.0;
    std::string chain, seamsEnable;
    int repaired = 0;
    auto at = [&](int f) {
        char b[96];
        snprintf(b, sizeof(b), "between(t\\,%.4f\\,%.4f)", (f - 0.4) / fps, (f + 0.4) / fps);
        return std::string(b);
    };
    for (int k = 17; k + 5 < static_cast<int>(st.size()); k += 17) {
        if (st[k].avg - (st[k - 1].avg + st[k + 1].avg) / 2.0 <= 3.0) continue;
        ++repaired;
        const Stat& p = st[k - 1];
        const Stat& n = st[k + 5];
        for (int i = 0; i <= 4; ++i) {
            const double w = (i + 1) / 6.0;
            const double tAvg = p.avg * (1 - w) + n.avg * w, tLow = p.low * (1 - w) + n.low * w, tHigh = p.high * (1 - w) + n.high * w;
            const Stat& c = st[k + i];
            const double con = (tHigh - tLow) / std::max(1.0, c.high - c.low);
            const double bri = (tAvg / 255.0 - 0.5) - (c.avg / 255.0 - 0.5) * con;
            char f[200];
            snprintf(f, sizeof(f), "%seq=contrast=%.4f:brightness=%.4f:enable='%s'", chain.empty() ? "" : ",", con, bri, at(k + i).c_str());
            chain += f;
        }
        seamsEnable += (seamsEnable.empty() ? "" : "+") + at(k);
    }
    if (repaired == 0) return 0;
    const std::string graph = "[0:v]" + chain + ",split[a][b];[b]tmix=frames=3:weights='1 0 1',setpts=PTS-1/(24*TB)[c];[a][c]overlay=eof_action=pass:enable='" +
                              seamsEnable + "',format=yuv420p[v]";
    const std::wstring tmp = video + L".seams.tmp";
    std::error_code ec;
    fs::remove(tmp, ec);
    std::string log;
    RunCaptureIdle(L"\"" + ff + L"\" -y -hide_banner -loglevel error -threads 2 -i \"" + video + L"\" -filter_complex_threads 2 -filter_complex \"" + std::wstring(graph.begin(), graph.end()) + /* (ASCII) */
                         L"\" -map \"[v]\" -map 0:a? -c:v libx264 -crf 14 -preset medium -threads 4 -c:a copy -map_metadata 0 -movflags +use_metadata_tags -f mp4 \"" + tmp + L"\"",
                     log, true);
    if (!fs::exists(tmp, ec) || fs::file_size(tmp, ec) < 10000) {
        fs::remove(tmp, ec);
        return -1;
    }
    if (readyTmp) {
        *readyTmp = tmp;
        return repaired;
    }
    return ReplaceSongFile(tmp, video) ? repaired : -1;
}

// what any Presenter prompt gets when it lacks the form's fixed sentences (a prompt typed by hand, the simple one)
const char* const kPresenterGuard =
    "The camera, lighting, person identity, clothes and background remain unchanged. One continuous shot: no cuts, zooms, scene changes or "
    "additional people. Preserve existing printed details and background objects; add no captions, subtitles, text, overlays, plants, logos or "
    "watermarks. Clear dry speech with no music, synchronized lip and facial motion.";

std::string PresenterReady(const std::string& prompt) {
    if (prompt.find("add no captions") != std::string::npos) return prompt;
    return prompt + (prompt.empty() || prompt.back() == '\n' ? "" : "\n") + kPresenterGuard;
}

std::string Trimmed(std::string s) {
    const size_t a = s.find_first_not_of(" \t\r\n\"");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n\"");
    return s.substr(a, b - a + 1);
}

int BeatsFor(double seconds) { return seconds <= 4 ? 2 : seconds <= 7 ? 3 : seconds <= 11 ? 4 : 5; }

// The writer sometimes copies the form it was given: "1. "Timeline: ..."" - the numbers and the quotes around whole lines go
std::string CleanWritten(const std::string& text) {
    std::string out;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        size_t k = 0;
        while (k < line.size() && line[k] == ' ') ++k;
        size_t d = k;
        while (d < line.size() && std::isdigit(static_cast<unsigned char>(line[d]))) ++d;
        if (d > k && d + 1 < line.size() && line[d] == '.' && line[d + 1] == ' ') line = line.substr(d + 2); // "1. "
        while (!line.empty() && (line.front() == '"' || line.front() == ' ')) line.erase(line.begin());
        while (!line.empty() && (line.back() == '"' || line.back() == ' ' || line.back() == '\r')) line.pop_back();
        if (line.rfind("**", 0) == 0) { // markdown bold labels
            line.erase(0, 2);
            const size_t e = line.find("**");
            if (e != std::string::npos) line.erase(e, 2);
        }
        if (!line.empty() || (!out.empty() && out.back() != '\n')) out += line + "\n";
        pos = nl + 1;
    }
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

std::string ReplaceAll(std::string s, const std::string& from, const std::string& to) {
    for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size())) s.replace(at, from.size(), to);
    return s;
}

// When the writer cannot run: the same shape, filled from what is known
std::string FallbackVideoPrompt(const std::string& idea, const std::string& context, double seconds, bool last) {
    const int s = static_cast<int>(std::lround(seconds));
    std::string out = (context.empty() ? std::string("Cinematic, natural light.") : Trimmed(context.substr(0, 300))) + " The scene opens exactly on image 1.\n";
    out += "Timeline:\n[0s-" + std::to_string(s / 2) + "s] " +
           (idea.empty() ? std::string("The subject comes to life: turns the head, shifts the weight, and starts to move; hair and clothes move with it, the camera slowly pushes in.")
                         : Trimmed(idea) + "; the camera slowly pushes in.") + "\n";
    out += "[" + std::to_string(s / 2) + "s-" + std::to_string(s) + "s] " +
           (last ? std::string("The action carries through to the final pose of image 2.") : std::string("The movement grows and resolves; the background stays alive.")) + "\n";
    out += "Audio: natural ambience, sounds matching the movement, a soft score.";
    return out;
}

std::string SecondsText(double s) {
    char b[16];
    if (std::fabs(s - std::round(s)) < 0.05) snprintf(b, sizeof(b), "%d", static_cast<int>(std::lround(s)));
    else snprintf(b, sizeof(b), "%.1f", s);
    return b;
}

// The long plan cut into H3 Continuum's parts ("---" between them): each part the look line, the beats that fall in it on its own clock
// ([0s-...]), the audio and the keep line; a continuing part says where the one before ended. "" = the plan could not be read (too few beats).
std::string SplitLongPlan(const std::string& plan, int chunks, double chunkSeconds, bool hasLast) {
    struct Beat {
        double a, b;
        std::string text;
    };
    std::vector<Beat> beats;
    std::string look, audio, keep;
    std::string* label = nullptr; // an "Audio:" / "Keep:" line written alone: the lines after it are its words
    std::istringstream in(plan);
    for (std::string line; std::getline(in, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back(); // (Trimmed would take a closing quote)
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.erase(line.begin());
        if (line.empty()) continue;
        std::string low = line;
        for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (low.rfind("timeline", 0) == 0) {
            label = nullptr;
            continue;
        }
        if (low.rfind("audio:", 0) == 0) {
            audio = line;
            label = &audio;
            continue;
        }
        if (low.rfind("keep:", 0) == 0) {
            keep = line;
            label = &keep;
            continue;
        }
        if (line[0] == '[') { // [3s-7s] or [3-7s] or [3.5s - 7s]
            const size_t close = line.find(']');
            double a = -1, b = -1;
            if (close != std::string::npos) {
                std::string inside = line.substr(1, close - 1);
                for (char& c : inside) {
                    if (c == 's' || c == 'S') c = ' ';
                    if (static_cast<unsigned char>(c) >= 0x80) c = '-'; // (an en dash)
                }
                const size_t dash = inside.find('-');
                if (dash != std::string::npos) a = atof(inside.substr(0, dash).c_str()), b = atof(inside.substr(dash + 1).c_str());
            }
            if (a >= 0 && b > a) {
                beats.push_back({a, b, Trimmed(line.substr(close + 1))});
                label = nullptr;
                continue;
            }
            if (close != std::string::npos) line = line.substr(close + 1); // a lone time ("[30s]"): its words go on, the time does not
            while (!line.empty() && line.front() == ' ') line.erase(line.begin());
            if (line.empty()) continue;
        }
        if (label) { // (the words of an Audio: / Keep: written on its own line)
            *label += (label->back() == ':' ? " " : " ") + line;
            continue;
        }
        if (beats.empty() && look.empty()) look = line; // the look line
        else if (!beats.empty()) beats.back().text += " " + line; // (speech written under its beat)
        else look += " " + line;
    }
    if (beats.size() < 2) return {};
    for (Beat& bt : beats) { // speech whose closing quote was lost (CleanWritten takes quotes off line ends)
        if (std::count(bt.text.begin(), bt.text.end(), '"') % 2) bt.text += "\"";
    }
    for (size_t i = 0; i + 1 < beats.size(); ++i) { // small gaps the writer left ([0s-4s], [5s-9s]): each beat runs on to the next
        if (beats[i + 1].a > beats[i].b && beats[i + 1].a - beats[i].b <= 2.0) beats[i].b = beats[i + 1].a;
    }
    look = ReplaceAll(look, "The scene opens exactly on image 1.", "");
    look = Trimmed(look);
    std::string out;
    std::string before; // how the part before ended
    for (int k = 0; k < chunks; ++k) {
        const double s0 = k * chunkSeconds, s1 = (k + 1) * chunkSeconds;
        std::string lines;
        std::string last;
        for (size_t bi = 0; bi < beats.size(); ++bi) {
            const Beat& bt = beats[bi];
            const bool finalBeat = bi + 1 == beats.size() && k == chunks - 1; // (the plan's last beat runs to the end of the video)
            const double a = std::max(bt.a, s0), b = std::min(finalBeat ? std::max(bt.b, s1) : bt.b, s1);
            if (b - a < 0.5) continue; // (a beat that only touches this part)
            lines += "[" + SecondsText(a - s0) + "s-" + SecondsText(b - s0) + "s] " + (bt.a < s0 - 0.5 ? "(continuing) " : "") + bt.text + "\n";
            last = bt.text;
        }
        if (lines.empty()) { // the plan stopped short: this part carries the last beat on to its end
            lines = "[0s-" + SecondsText(chunkSeconds) + "s] (continuing) " + beats.back().text + "\n";
            last = beats.back().text;
        }
        std::string part = look.empty() ? std::string() : look + " ";
        if (k == 0) {
            part += "The scene opens exactly on image 1.\n";
        } else {
            std::string end = before.size() > 220 ? before.substr(0, before.find_last_of(' ', 220)) + "..." : before;
            part += "\nSeamless continuation of the immediately preceding clip, which ended: " + end + "\n";
        }
        part += "Timeline:\n" + lines;
        if (k == chunks - 1 && hasLast) part += "The last beat ends exactly on image 2.\n";
        part += k == 0 ? "One continuous shot, no cuts." : "Same people, clothes, place and light. One continuous shot: no cut or reset.";
        if (!audio.empty()) part += "\n" + audio;
        if (!keep.empty()) part += "\n" + keep;
        out += (k ? "\n---\n" : "") + part;
        before = last;
    }
    return out;
}

// H3 Continuum's prompt mode for this text, set explicitly: left on Auto, a part written with "[0s-3s]" beats made Continuum decide the whole
// text was one malformed timeline and send ALL of it as every part's prompt (its parser, v2/prompts.py: a line starting "[3s-" before any
// header means Fixed). Parts split by "---" lines are List; anything else is one prompt for every part.
std::string ContinuumPromptMode(const std::string& prompt) {
    std::istringstream in(prompt);
    for (std::string line; std::getline(in, line);) {
        const std::string t = Trimmed(line);
        if (t.size() >= 3 && t.find_first_not_of('-') == std::string::npos) return "List";
    }
    return "Fixed";
}

struct PeopleScan; // (below: the people of a clip part, found for the page)
struct WriteJob {
    std::wstring first, last;
    std::string idea, context, label, key;
    H3Options opt;
    bool sendAfter = false, toPage = false;
    int kind = 0;                  // 0 animate a picture, 1 star in a video
    bool write = true;             // write the prompt (false: `prompt` is used as it is)
    std::string prompt;
    std::vector<std::wstring> who; // star: the pictures (<Picture 1>...)
    std::wstring refVideo;         // star: the clip
    double refStart = 0;
    bool keepSound = true;
    int starQuality = 0;
    int starMethod = 0;            // star: 0 swap the person (Character Swap, the clip as a guide), 1 recreate the clip (reference)
    std::vector<std::string> names, replaces; // star: who[k]'s name, and whom they replace in the clip ("" = from the idea and the clip)
    std::vector<std::string> said;            // star: whom the user's words say who[k] replaces ("the girl"), read by ReplacesFromIdea
    int pass = -1;                            // swap, one person at a time: whose pass this is (who[pass] only); -1 = everyone at once
    std::wstring origClip;                    // swap, one person at a time: the clip itself (a later pass gets the earlier pass's video)
    std::string emphasis;                     // swap, one person at a time: said first in the prompt (a second take)
    std::shared_ptr<const PeopleScan> scan;   // swap: the people found in the clip part (who is who was picked on the page)
    std::vector<std::vector<std::vector<int>>> picks; // ...per shot, per new person: the SAM 3 objects they replace (empty: the writer decides)
    int longId = 0, longPart = -1;            // swap: a part of a long swap (joined when every part is kept)
    std::string sam3;                         // swap: SAM 3's checkpoint (the pictures' people are cut out onto white with it), "" = not there
};
struct WriteResult {
    std::string text;
    std::wstring clip; // star: the prepared reference clip
    std::string note;  // why something fell back
    std::vector<std::wstring> who; // swap: the pictures as they go in (each person cut out onto white), "" = as they were
};
std::deque<WriteJob> g_writeQueue;
std::future<WriteResult> g_writeFut;
WriteJob g_writing;
std::mutex g_phaseMutex;
std::string g_phase;
std::string g_writeNote; // why the last prompt is the simple one (the writer could not run), "" = it was written

// What a video's making did and when, one line each (DataDir\video_steps.log, the last ~2000 lines): seen 2026-10-22, a swap sat 18 minutes
// between finding the people and writing its prompt with nothing to say where the time went.
void StepLog(const std::string& what) {
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    const std::wstring path = DataDir() + L"\\video_steps.log";
    std::error_code ec;
    if (fs::exists(path, ec) && fs::file_size(path, ec) > 400000) { // keep the newer half
        std::ifstream in(path, std::ios::binary);
        std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        const size_t cut = all.find('\n', all.size() / 2);
        std::ofstream(path, std::ios::binary | std::ios::trunc) << (cut == std::string::npos ? std::string() : all.substr(cut + 1));
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[32];
    snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d:%02d ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::ofstream(path, std::ios::binary | std::ios::app) << stamp << what << "\n";
}

void SetWritePhase(const std::string& p) {
    if (!p.empty()) StepLog("writer: " + p);
    std::lock_guard<std::mutex> lock(g_phaseMutex);
    g_phase = p;
}

// An action without its camera words ("..., the camera pans across the water, then tilts down to her face" -> its clauses with a camera
// word taken out): the beat's camera is said once, from the gentle moves allowed
std::string DropCameraClauses(const std::string& text) {
    std::vector<std::string> parts;
    std::string cur;
    for (size_t i = 0; i <= text.size(); ++i) {
        const char c = i < text.size() ? text[i] : ',';
        if (c == ',' || c == ';' || c == '.') {
            if (!Trimmed(cur).empty()) parts.push_back(Trimmed(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    std::string out;
    for (const std::string& p : parts) {
        std::string low = p;
        for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        bool camera = false;
        for (const char* w : {"camera", " pans", "pan ", "tilts", "tilt ", "zoom", "the shot", "the frame ", "whirs", "orbits"}) camera = camera || low.find(w) != std::string::npos;
        if (camera) continue;
        std::string q = p;
        if (low.rfind("then ", 0) == 0 && out.empty()) q = q.substr(5);
        out += (out.empty() ? "" : ", ") + q;
    }
    if (out.empty()) return text;
    out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0]))); // (its first clause gone: "smiles wider" -> "Smiles wider")
    return out;
}

// At most `maxWords` words, ended at a sentence's (or a clause's) end when there is one
std::string FitWords(const std::string& words, int maxWords) {
    maxWords = std::max(4, maxWords);
    std::istringstream in(words);
    std::vector<std::string> w;
    for (std::string x; in >> x;) w.push_back(x);
    if (static_cast<int>(w.size()) <= maxWords) return words;
    int cut = maxWords;
    for (int k = maxWords; k >= maxWords / 2; --k) { // the last sentence or clause end within the limit
        const char e = w[k - 1].back();
        if (e == '.' || e == '!' || e == '?' || e == ',') {
            cut = k;
            break;
        }
    }
    std::string out;
    for (int k = 0; k < cut; ++k) out += (k ? " " : "") + w[k];
    while (!out.empty() && (out.back() == ',' || out.back() == ';')) out.pop_back();
    return out;
}

std::string WriteVideoPrompt(WriteJob j, std::string comfyUrl) {
    auto note = [](const std::string& n) {
        std::lock_guard<std::mutex> lock(g_phaseMutex);
        g_writeNote = n;
    };
    note("");
    if (!WriterConfigured()) {
        note("the prompt writer is not set up (Clef mode > Prompt writer)");
        return FallbackVideoPrompt(j.idea, j.context, j.opt.seconds, !j.last.empty());
    }
    const ClefSession session = WriterAcquire(comfyUrl, SetWritePhase);
    std::string out;
    if (session.ok) {
        SetWritePhase("Writing the video prompt...");
        const bool see = WriterCanSee();
        std::vector<std::string> images;
        if (see) {
            for (const std::wstring* p : {&j.first, &j.last}) {
                if (p->empty()) continue;
                const std::string url = ImageDataUrl(*p, 768);
                if (!url.empty()) images.push_back(url);
            }
        }
        const bool presenter = j.opt.quality == 3;
        const bool longVideo = j.opt.seconds > 15.0 + 1e-6 && !presenter;
        { // a camera wish and a picture: is it a first-person view? (the camera is someone's eyes: it cannot orbit them)
            const std::string tag = "Camera wish (only if it suits this picture): ";
            const size_t at = j.idea.find(tag);
            if (at != std::string::npos && !images.empty()) {
                const size_t end = j.idea.find('.', at + tag.size());
                const std::string wish = j.idea.substr(at + tag.size(), end == std::string::npos ? std::string::npos : end - at - tag.size());
                const bool fits = wish.find("static") != std::string::npos || wish.find("push-in") != std::string::npos || wish.find("handheld") != std::string::npos;
                std::string js;
                const json schema = {{"type", "object"}, {"properties", {{"first_person_view", {{"type", "boolean"}}}}}, {"required", {"first_person_view"}}};
                if (!fits && WriterChatJson("Look at the picture. Is it a first-person view: the camera is a person's own eyes, with their hands, a weapon, a steering "
                                            "wheel or handlebars in the foreground (a video game shooter, a POV shot)? Reply as JSON: first_person_view.",
                                            "Is this picture a first-person view?", {images.front()}, schema.dump(), 40, 0.0, js, nullptr)) {
                    const json a = json::parse(js, nullptr, false);
                    if (a.is_object() && a.value("first_person_view", false)) {
                        j.idea.replace(at, (end == std::string::npos ? j.idea.size() : end + 1) - at,
                                       "Camera: first-person - it moves only as the person does (walking forward, turning, looking around), smooth and steady.");
                        StepLog("writer: first-person picture - the camera wish \"" + wish + "\" was swapped for the person's own movement");
                    }
                }
            }
        }
        const bool ltx = j.opt.quality == kLtxQuality && !longVideo; // (its own form: one paragraph)
        std::string system = presenter ? kPresenterSystem : longVideo ? kLongPlanSystem : ltx ? kLtxSystem : kVideoSystem;
        system = ReplaceAll(system, "{SPEECH}", std::to_string(static_cast<int>(std::lround(j.opt.seconds * 2.3))));
        int chunks = 2;
        double chunkSeconds = 8.0;
        int longBeats = 0;
        if (longVideo) {
            LongPlan(j.opt.seconds, chunks, chunkSeconds);
            system = ReplaceAll(system, "{CHUNKS}", std::to_string(chunks));
            system = ReplaceAll(system, "{CHUNK}", SecondsText(chunkSeconds));
            std::string bounds;
            for (int k = 0; k < chunks; ++k) bounds += (k ? (k == chunks - 1 ? " and " : ", ") : "") + SecondsText(k * chunkSeconds) + " s";
            system = ReplaceAll(system, "{BOUNDS}", bounds);
            longBeats = std::clamp(static_cast<int>(std::lround(j.opt.seconds / 3.0)), 6, 20); // about one beat every 3 s, as a short clip has
            system = ReplaceAll(system, "{BEATS}", std::to_string(longBeats));
            system = ReplaceAll(system, "{WORDS}", std::to_string(80 + longBeats * 32));
        }
        system = ReplaceAll(system, "{LAST}", j.last.empty() ? std::string()
                                              : presenter ? std::string(" The second attached picture is the LAST frame (<Picture 2>): the actions end in its pose.")
                                                          : std::string(" The second attached picture is the LAST frame (<Picture 2>): the action must lead there, and "
                                                                        "the last beat ends exactly on image 2."));
        system = ReplaceAll(system, "{BEATS}", std::to_string(BeatsFor(j.opt.seconds)));
        system = ReplaceAll(system, "{SECONDS}", std::to_string(static_cast<int>(std::lround(j.opt.seconds))));
        system = ReplaceAll(system, "{WORDS}", std::to_string(static_cast<int>(std::lround(j.opt.seconds * 2.3))));
        std::string user = "Length: " + std::to_string(static_cast<int>(std::lround(j.opt.seconds))) + " seconds.\n";
        user += j.idea.empty() ? "The user's idea: none. Choose a lively, natural action that suits the picture.\n" : "The user's idea (follow it): " + j.idea + "\n";
        if (!j.context.empty()) user += "The prompt the picture was made from (what it shows; do not copy it): " + j.context.substr(0, 1200) + "\n";
        if (!see || j.first.empty()) user += j.first.empty() ? "There is no start picture: invent the scene from the idea.\n" : "You cannot see the picture: go by the prompt it was made from.\n";
        if (presenter) { // (left to itself the writer once gave a Chinese-looking presenter Mandarin lines for an English idea: the model then
                         //  laid Mandarin subtitles over the picture, its livestream habit)
            const bool ascii = std::all_of(j.idea.begin(), j.idea.end(), [](char c) { return static_cast<unsigned char>(c) < 128; });
            user += ascii ? "The speech, the Summary's language and the voice are ENGLISH, whatever the person looks like.\n"
                          : "The speech is in the same language as the user's idea.\n";
        }
        std::string err;
        bool written = false;
        if (!presenter && !longVideo && !ltx) { // the form held by a schema (in words alone the writers skipped the timeline, the audio and keep lines)
            const int beats = BeatsFor(j.opt.seconds);
            const json schema = {{"type", "object"},
                                 {"properties",
                                  {{"opening_look_and_place", {{"type", "string"}, {"minLength", 60}, {"maxLength", 400}}},
                                   {"timeline", {{"type", "array"}, {"minItems", beats}, {"maxItems", beats},
                                                 {"items", {{"type", "object"},
                                                            {"properties", {{"from_second", {{"type", "number"}}}, {"to_second", {{"type", "number"}}},
                                                                            {"what_happens_and_camera", {{"type", "string"}, {"minLength", 80}, {"maxLength", 500}}}}},
                                                            {"required", {"from_second", "to_second", "what_happens_and_camera"}}}}}},
                                   {"spoken_words", {{"type", "string"}, {"maxLength", 240}}},
                                   {"audio", {{"type", "string"}, {"minLength", 40}, {"maxLength", 300}}},
                                   {"must_not_change", {{"type", "string"}, {"minLength", 30}, {"maxLength", 500}}}}},
                                 {"required", {"opening_look_and_place", "timeline", "spoken_words", "audio", "must_not_change"}}};
            const std::string jsys = system + "\nReply as JSON: opening_look_and_place = part 1, timeline = part 2 (one item per beat, from 0 to " +
                                     std::to_string(static_cast<int>(std::lround(j.opt.seconds))) +
                                     " seconds), spoken_words = part 3 (who says what; \"\" when nobody speaks), audio = part 4, must_not_change = part 5.";
            std::string js;
            if (WriterChatJson(jsys, user, images, schema.dump(), 1200, 0.8, js, &err)) {
                const json a = json::parse(js, nullptr, false);
                if (a.is_object() && a.contains("timeline") && a["timeline"].is_array() && !a["timeline"].empty()) {
                    auto str = [&](const char* k) { return a.contains(k) && a[k].is_string() ? Trimmed(a[k].get<std::string>()) : std::string(); };
                    std::string look = str("opening_look_and_place");
                    if (look.find("opens exactly on image 1") == std::string::npos) look += (look.empty() || look.back() == '.' ? " " : ". ") + std::string("The scene opens exactly on image 1.");
                    out = look + "\nTimeline:";
                    const json& tl = a["timeline"];
                    const double total = j.opt.seconds, step = total / tl.size();
                    for (size_t b = 0; b < tl.size(); ++b) { // (the times evened out to cover the whole clip: they came back short of it)
                        const std::string what = tl[b].is_object() && tl[b].contains("what_happens_and_camera") && tl[b]["what_happens_and_camera"].is_string()
                                                     ? Trimmed(tl[b]["what_happens_and_camera"].get<std::string>()) : std::string();
                        out += "\n[" + SecondsText(b * step) + "s-" + SecondsText(b + 1 == tl.size() ? total : (b + 1) * step) + "s] " + what;
                    }
                    const std::string speech = str("spoken_words");
                    if (!speech.empty() && speech != "\"\"" && speech.find("nobody") == std::string::npos && speech.find("No one") == std::string::npos) out += "\n" + speech;
                    out += "\nAudio: " + str("audio");
                    std::string keep = str("must_not_change");
                    if (keep.find("nothing morphs") == std::string::npos) keep += (keep.empty() || keep.back() == '.' ? " " : "; ") + std::string(keep.find("exactly as in image 1") != std::string::npos ? "nothing morphs." : "exactly as in image 1; nothing morphs.");
                    out += "\nKeep: " + keep;
                    written = true;
                }
            }
        }
        if (longVideo) { // planned part by part: each part 2-4 new beats with the words said in them, then split as before
            // (seen 2026-10-25, 60 s vlog: one list of 20 beats forced by the schema came back as 6 actions repeated to fill it - the video
            // looped blink, wave, point, laugh - and no words were written, so H3 made her babble)
            // (seen 2026-10-25, the next 60 s vlog: 3.3 s beats with five actions and camera moves each - "points the phone at the lake, the
            //  camera pans across the water, then tilts down", "spins in a small circle", "the camera follows her" - made H3 cut to her back and
            //  smear her face as it came back; 20-word lines in 3 s; the sunset and the stars within a minute. Now: 1-2 beats a part, one
            //  action each, the camera picked from gentle moves, the words cut to the beat's time)
            json cams = json::array({"static", "slow push-in", "slow pull-out", "gentle handheld follow", "slow pan"});
            {
                const std::string tag = "Camera wish (only if it suits this picture): "; // (a wish that suits it: one of the choices too)
                const size_t at = j.idea.find(tag);
                if (at != std::string::npos) {
                    const size_t end = j.idea.find('.', at + tag.size());
                    cams.push_back(j.idea.substr(at + tag.size(), end == std::string::npos ? std::string::npos : end - at - tag.size()));
                }
            }
            const json beat = {{"type", "object"},
                               {"properties", {{"what_the_people_do", {{"type", "string"}, {"minLength", 50}, {"maxLength", 240}}},
                                               {"camera", {{"type", "string"}, {"enum", cams}}},
                                               {"spoken_words", {{"type", "string"}, {"maxLength", 200}}}}},
                               {"required", {"what_the_people_do", "camera", "spoken_words"}}};
            const json schema = {{"type", "object"},
                                 {"properties",
                                  {{"opening_look_and_place", {{"type", "string"}, {"minLength", 60}, {"maxLength", 450}}},
                                   {"parts", {{"type", "array"}, {"minItems", chunks}, {"maxItems", chunks},
                                              {"items", {{"type", "object"},
                                                         {"properties", {{"beats", {{"type", "array"}, {"minItems", 1}, {"maxItems", 2}, {"items", beat}}}}},
                                                         {"required", {"beats"}}}}}},
                                   {"audio", {{"type", "string"}, {"minLength", 40}, {"maxLength", 300}}},
                                   {"must_not_change", {{"type", "string"}, {"minLength", 30}, {"maxLength", 500}}}}},
                                 {"required", {"opening_look_and_place", "parts", "audio", "must_not_change"}}};
            const std::string jsys =
                system + "\nReply as JSON: opening_look_and_place = part 1; parts = the " + std::to_string(chunks) + " parts of " + SecondsText(chunkSeconds) +
                " seconds in order, each with 1 or 2 beats (their times are set for you: write no times); audio = part 4; must_not_change = part 5. "
                "A beat is ONE simple, calm action of the people (what_the_people_do: no camera words in it), and its camera move from the list. "
                "Every beat is NEW: it carries the story on from the one before and never repeats an action or a sentence of an earlier beat or part - "
                "the last part ends the story. Time does not jump: the light, the weather and the time of day stay as in the picture (no sunset, no "
                "night falling, no fade to black). The people stay in the shot the whole time: the camera never turns away from them to show the "
                "place, and nobody spins around. In a selfie or vlog the phone stays in the person's hand facing them; if they walk, they walk with it "
                "held out in front, facing it. spoken_words = the exact words said in that beat (\"\" when nobody speaks): when someone talks (a vlog, "
                "an interview, a conversation), write real, simple sentences that make sense together and carry on from the last ones, about what the "
                "idea says, and SHORT: at most " + std::to_string(static_cast<int>(chunkSeconds * 2.4)) + " words for a beat that fills its part, half "
                "that when the part has two beats.";
            auto strip = [](std::string t) { // (a "10s-20s:" or "[3s-6s]" the writer put in front anyway)
                t = Trimmed(t);
                for (int guard = 0; guard < 3; ++guard) {
                    size_t i = 0;
                    if (i < t.size() && t[i] == '[') ++i;
                    const size_t d0 = i;
                    while (i < t.size() && (isdigit(static_cast<unsigned char>(t[i])) || t[i] == '.' || t[i] == 's' || t[i] == ' ' || t[i] == '-')) ++i;
                    if (i > d0 && i < t.size() && (t[i] == ':' || t[i] == ']') && isdigit(static_cast<unsigned char>(t[d0]))) t = Trimmed(t.substr(i + 1));
                    else break;
                }
                return t;
            };
            for (int attempt = 0; attempt < 2 && !written; ++attempt) {
                std::string js;
                if (!WriterChatJson(jsys + (attempt ? " Your last plan repeated beats: write every beat new." : ""), user, images, schema.dump(), 1000 + chunks * 600,
                                    attempt ? 0.9 : 0.8, js, &err))
                    break;
                const json a = json::parse(js, nullptr, false);
                if (!a.is_object() || !a.contains("parts") || !a["parts"].is_array() || static_cast<int>(a["parts"].size()) < chunks) continue;
                auto str = [&](const char* k) { return a.contains(k) && a[k].is_string() ? Trimmed(a[k].get<std::string>()) : std::string(); };
                std::string plan = str("opening_look_and_place") + "\nTimeline:";
                std::set<std::string> seen;
                int repeats = 0;
                for (int c = 0; c < chunks; ++c) {
                    const json& part = a["parts"][c];
                    const json beats = part.is_object() && part.contains("beats") && part["beats"].is_array() ? part["beats"] : json::array();
                    const int here = std::max(1, static_cast<int>(beats.size()));
                    const double s0 = c * chunkSeconds, s1 = c + 1 == chunks ? j.opt.seconds : (c + 1) * chunkSeconds;
                    for (int q = 0; q < static_cast<int>(beats.size()); ++q) {
                        const json& bt = beats[q];
                        std::string what = bt.is_object() && bt.contains("what_the_people_do") && bt["what_the_people_do"].is_string()
                                               ? strip(bt["what_the_people_do"].get<std::string>()) : std::string();
                        what = DropCameraClauses(what); // (the camera is said once, from the list)
                        const std::string cam = bt.is_object() && bt.contains("camera") && bt["camera"].is_string() ? bt["camera"].get<std::string>() : std::string("static");
                        std::string words = bt.is_object() && bt.contains("spoken_words") && bt["spoken_words"].is_string() ? Trimmed(bt["spoken_words"].get<std::string>()) : std::string();
                        while (!words.empty() && (words.front() == '"' || words.front() == '\'')) words.erase(words.begin());
                        while (!words.empty() && (words.back() == '"' || words.back() == '\'')) words.pop_back();
                        words = FitWords(words, static_cast<int>((s1 - s0) / here * 2.5)); // (about 2.5 words a second, cut at a sentence's end)
                        if (!what.empty() && what.back() != '.') what += ".";
                        what += " Camera: " + cam + ".";
                        std::string low = what.substr(0, 50);
                        for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                        if (!seen.insert(low).second) ++repeats;
                        if (!words.empty() && words.find("nobody") == std::string::npos) what += " Saying: \"" + words + "\"";
                        plan += "\n[" + SecondsText(s0 + (s1 - s0) * q / here) + "s-" + SecondsText(s0 + (s1 - s0) * (q + 1) / here) + "s] " + what;
                    }
                }
                if (repeats >= 2 && attempt == 0) {
                    StepLog("writer: the long plan repeated " + std::to_string(repeats) + " beats - written again");
                    continue;
                }
                plan += "\nAudio: " + str("audio");
                std::string keep = str("must_not_change");
                if (keep.find("nothing morphs") == std::string::npos) keep += (keep.empty() || keep.back() == '.' ? " " : "; ") + std::string(keep.find("exactly as in image 1") != std::string::npos ? "nothing morphs." : "exactly as in image 1; nothing morphs.");
                out = plan + "\nKeep: " + keep;
                written = true;
            }
        }
        if (!written && !WriterChat(system, user, images, longVideo ? 500 + longBeats * 90 : 900, 0.8, out, &err)) {
            out.clear();
            note("the prompt writer did not answer" + (err.empty() ? std::string() : ": " + err));
        }
        out = Trimmed(out);
        if (out.rfind("```", 0) == 0) { // a code block after all
            out = out.substr(out.find('\n') == std::string::npos ? 3 : out.find('\n') + 1);
            const size_t e = out.rfind("```");
            if (e != std::string::npos) out.resize(e);
            out = Trimmed(out);
        }
        if (j.opt.quality != 3) out = CleanWritten(out); // (it strips quotes at line ends: the presenter form ends a line with the speech's)
        else out = ReplaceAll(out, "**", "");
        if (longVideo && !out.empty()) { // the one timeline cut into the parts; unreadable: written part by part the old way
            std::string parts = SplitLongPlan(out, chunks, chunkSeconds, !j.last.empty());
            if (parts.empty()) {
                std::string old = ReplaceAll(ReplaceAll(kLongVideoSystem, "{CHUNKS}", std::to_string(chunks)), "{CHUNK}", SecondsText(chunkSeconds));
                old = ReplaceAll(old, "{SECONDS}", std::to_string(static_cast<int>(std::lround(j.opt.seconds))));
                old = ReplaceAll(old, "{LAST}", j.last.empty() ? std::string() : std::string(" The second attached picture is the LAST frame (<Picture 2>): the action must lead there."));
                std::string again;
                if (WriterChat(old, user, images, 1400, 0.8, again, &err)) parts = CleanWritten(Trimmed(again));
                note("the writer's timeline could not be read: written part by part");
            }
            if (!parts.empty()) out = parts;
        }
        if (longVideo && out.find("\n---") == std::string::npos) { // the writer left out the "---" lines (seen): a part starts at each continuation
            for (size_t at = out.find("Seamless continuation", 1); at != std::string::npos; at = out.find("Seamless continuation", at + 1)) {
                const std::string sep = out[at - 1] == '\n' ? "---\n" : "\n---\n";
                out.insert(at, sep);
                at += sep.size();
            }
        }
    }
    WriterRelease(session, false);
    SetWritePhase("");
    if (!session.ok) note("the prompt writer could not be started");
    if (out.size() < 40) out = FallbackVideoPrompt(j.idea, j.context, j.opt.seconds, !j.last.empty()); // nothing usable came back
    return j.opt.quality == 3 ? PresenterReady(out) : out;
}

// ---- the page's state
// ---- camera moves (Animate, 2026-10-23, Higgsfield's Cinema Studio): said to the writer and, when the prompt is your own, at its end
struct CameraMove {
    const char* name;
    const char* text; // nullptr = the writer picks
};
const CameraMove kCameras[] = {
    {"Auto", nullptr},
    {"Still", "a locked-off static camera, it does not move"},
    {"Push in", "a slow, steady push-in toward the subject"},
    {"Pull out", "a slow pull-out revealing more of the scene"},
    {"Pan", "a smooth pan following the action sideways"},
    {"Orbit", "the camera orbits around the subject in a half circle"},
    {"Crane up", "a crane shot rising up and over the scene"},
    {"Tracking", "a tracking shot moving alongside the subject"},
    {"Handheld", "handheld camera with natural shake, documentary style"},
    {"Dolly zoom", "a dolly zoom (vertigo effect): the background stretches while the subject stays the same size"},
    {"Low angle", "a low-angle shot looking up at the subject"},
    {"FPV drone", "a fast FPV drone shot sweeping through the scene"},
};
constexpr int kCameraCount = static_cast<int>(sizeof(kCameras) / sizeof(kCameras[0]));

// ---- motion presets (Animate): a card fills the idea, its length and its camera move
struct Preset {
    const char* name;
    const char* idea;
    double seconds = 0; // 0 = the length you set
    int camera = 0;     // kCameras (0 = Auto)
};
const Preset kPresets[] = {
    {"Viral dance", "does a viral TikTok dance to an upbeat track, big confident moves, hits every beat, ends with a pose and a grin", 8, 1},
    {"Catwalk", "struts toward the camera like on a fashion runway, turns at the end with a hair flip and a cool look over the shoulder", 6, 7},
    {"Talk to camera", "talks straight to the camera like a vlogger, lively gestures, says one short catchy line, then laughs", 6, 8},
    {"Laugh it off", "bursts out laughing at something off-camera, doubles over, wipes a tear, shakes their head", 5, 2},
    {"Kung fu", "pulls off a fast kung fu sequence: a spinning kick, a punch flurry, lands in a fighting stance; whooshes and impacts", 6, 5},
    {"Hero landing", "drops from above and lands in a superhero landing, cracks the ground, slowly rises and looks up", 5, 10},
    {"Moonwalk", "moonwalks smoothly across the floor, spins, freezes on tiptoe with a hat tip", 6, 4},
    {"Mic drop", "raps a short punchy line into a microphone, bounces to the beat, then drops the mic", 6, 8},
    {"Spin & wink", "spins around once, stops facing the camera, winks and points at the viewer", 4, 2},
    {"Slow-mo hair flip", "in slow motion, flips their hair, wind blowing, lights flaring, dreamy music", 5, 5},
    {"Shocked", "reacts with exaggerated shock to something off-camera, jaw drops, hands to the face, a comedic sting", 4, 9},
    {"Unboxing", "excitedly opens a mystery box, gasps at what is inside, holds it up to the camera", 6, 2},
    {"Bullet time", "everything freezes mid-action in bullet time while the camera sweeps around them, then time snaps back to normal speed", 5, 5},
    {"Walk away", "walks calmly toward the camera as a huge explosion goes off behind them, not looking back, debris and fire in the air", 5, 3},
    {"Zero gravity", "starts floating up into the air in zero gravity, hair and clothes drifting, laughing in surprise", 5, 6},
    {"Superhero takeoff", "crouches, then launches into the sky like a superhero, dust blasting outward, the camera tilting up after them", 5, 10},
    {"Glow up", "a quick transformation: spins once and their outfit and hair change into a glamorous red-carpet look with sparkles", 5, 2},
    {"Rain dance", "dances joyfully in pouring rain at night under a street light, splashing in puddles", 6, 7},
};
constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));
int g_presetCardFrame = -10; // the last frame a motion-preset card played its drawing (hovered)
std::map<std::string, std::wstring> g_presetShots;        // by preset: the last video made with it (its card's picture)
std::vector<std::pair<int, std::string>> g_presetWaiting; // activity -> preset

struct VideoPage {
    bool loaded = false;
    H3Options opt;
    std::wstring first;
    std::string prompt;
    std::string idea;      // what you want to happen (optional)
    std::string context;   // the start picture's own prompt: what it shows (helps the writer; never sent as it is)
    bool autoWrite = true; // Make writes the video prompt first when it is missing or out of date
    std::string writtenKey; // what the prompt was written for (pictures, idea, length): a change makes it out of date
    bool edited = false;    // you changed the written prompt: it is kept
    bool pressMake = false; // Make, as if clicked (the palette, tests)
    // which LoRAs ComfyUI has (empty until asked; asked when the page opens)
    std::future<std::vector<std::string>> lorasFut;
    std::vector<std::string> loras;
    bool lorasKnown = false;
    double lorasAsked = -1000.0;
    // ...or, while ComfyUI is not running, its loras folder on disk
    std::future<std::vector<std::string>> diskFut;
    std::vector<std::string> onDisk;
    bool diskKnown = false;
    // Veda sparse attention: used when this is on and ComfyUI has its node and predictor (asked with the LoRAs)
    bool veda = true;
    std::future<ComfyVeda> vedaFut;
    ComfyVeda vedaInfo;
    bool vedaKnown = false;
    double vedaAsked = -1000.0;
    bool vedaWasReady = false; // ComfyUI had it the last time it was asked (the switch shows before it answers again)
    bool continuumWas = false; // ...and H3 Continuum (videos over 15 s)
    bool swapMask = true;      // Swap the person: only the masked person is made again (when MaskVidExperiments and SAM 3 are there)
    // Fizgig H3 Tweaks (shootthesound/ComfyUI-Fizgig-H3-Tweaks): a model patch, no models. Used when ComfyUI has its node (asked with Veda).
    float detail = 0.15f;        // Detail & Contrast: + crisper texture and local contrast, - softer (its own default 0.15)
    float promptStrength = 0.0f; // how strongly the prompt comes through (Turbo runs without CFG); for a swap, the new person's look
    // Make it crisp (Open-H3-Regenerate): a finished video redrawn at a higher resolution with fine detail
    int crispTo = 1440;          // the short side it is made at (Make it crisp goes to 1440 at most; the quick upscale to 2160 too)
    bool crispAfterSwap = false; // every swap is made crisp once it is done
    bool facesAfterSwap = true;  // every swap's faces take the original's expressions once it is done (when LivePortrait is set up)
    bool oneAtATime = true;      // a masked swap of several people: one person a pass (never mixes them up; a render each)
    bool starUseSheet = true;    // Recreate the clip with them: a character with a character sheet goes in as its sheet
    // the layout, dragged by the user (0 = the default)
    float leftW = 0;        // the form's width (unscaled pixels)
    float playerFrac = 0.45f; // the player's share of the right side's height
    float thumbW = 132.0f;  // a video's thumbnail width (unscaled)
    int camera = 0;         // the camera move (kCameras; 0 = Auto: the writer picks)
    bool cameraByPreset = false; // ...set by a preset card: back to Auto when the idea is no longer that preset's
};
VideoPage g_v;
// Presenter takes being made: once back, each is checked for text over the picture (OverlayRise) and, if so, made again once with a new seed
struct PresenterTake {
    int act = 0;
    std::wstring first;
    std::string prompt;
    H3Options opt;
    std::string label;
    bool retry = true; // false: it is the second take already
};
std::vector<PresenterTake> g_presenterTakes;
bool g_presenterRetrying = false;
// A swap sent, to check once it is back: did the person change at all? (a vague look let the LoRA give back the clip as it was)
struct PeopleMask;
struct WhoBox { // a new person in one shot of a masked swap: their biggest frame and its box (at the person masks' size)
    int shot = 0, subject = 0, frame = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
struct SwapTake {
    int act = 0;
    struct WriteJobLite {
        std::vector<std::wstring> who;
        std::wstring refVideo;
        double refStart = 0;
        bool keepSound = true;
        std::string idea, label;
        H3Options opt;
        int starQuality = 0;
        std::shared_ptr<const PeopleScan> scan; // the page's picks (a retry keeps them)
        std::vector<std::vector<std::vector<int>>> picks;
        int longId = 0, longPart = -1;
    } job;
    std::wstring clip;  // the cut part that guided it
    std::string prompt;
    bool retry = true;  // false: it is the second take already
    std::wstring people;     // a masked swap: its person masks (BuildPeopleMasks), their size and shots
    int peopleW = 0, peopleH = 0;
    std::vector<int> shots;  // first frame of each shot, then the end
    std::string keptNote;    // set when a shot came back with someone unchanged: "1.2-2.2 s"
    std::vector<WhoBox> boxes; // where each new person is, shot by shot
    std::string comfyUrl;
    int pass = -1;                           // one person at a time: whose pass; the next is sent when this take is kept
    WriteJob chainJob;                       // the whole job (everyone), its prompt, and the people found in the clip
    std::string chainPrompt;
    std::shared_ptr<PeopleMask> chainPeople;
};
std::vector<SwapTake> g_swapTakes;
// A long swap (over 15 s): its parts' kept takes, joined once all are back - trimmed to their exact lengths, the clip's own sound under them
struct LongSwap {
    int id = 0;
    std::vector<std::wstring> files; // each part's kept take ("" = not yet)
    std::vector<double> lens;        // each part's length (s)
    std::wstring src;                // the clip (its sound)
    double start = 0;
    bool keepSound = true;
    std::string label;
    int people = 1;
    bool joining = false;
};
std::mutex g_longMutex;
std::vector<LongSwap> g_longSwaps;
int g_nextLongId = 1;
struct LongDone {
    std::wstring video, orig; // the joined swap, and the clip's part it replaces (for the faces pass)
    std::string label, error;
    int people = 1;
};
std::vector<LongDone> g_longDone; // (under g_longMutex)
std::mutex g_crispQueueMutex;
std::vector<std::wstring> g_crispQueue; // finished swaps to make crisp (Make it crisp after a swap)
struct FacesJob {
    std::wstring video, clip;
    int people = 1;
};
std::vector<FacesJob> g_facesQueue; // finished swaps whose faces take the original's expressions (under g_crispQueueMutex)
bool g_swapRetrying = false;
std::vector<std::pair<SwapTake, std::wstring>> g_nextPass; // one person at a time: a kept pass and its video, for the next person (under g_crispQueueMutex)
std::mutex g_swapFoundMutex;
std::vector<std::pair<SwapTake, double>> g_swapUnchanged; // takes that came back as the clip itself (their likeness to it, dB)
struct Swap {
    std::wstring tmp, video;
    std::string verdict;
    int tries = 0;
};
std::mutex g_swapMutex;
std::vector<Swap> g_swaps; // repaired videos waiting to be put in place (UI thread)
std::mutex g_overlayMutex;
std::vector<std::pair<PresenterTake, std::string>> g_overlayFound; // checked takes with text: (the take, the file name)
std::set<int> g_overlayChecking;

// tests (MV_VIDEO_MAKE "...|idea:..."): a prompt being written for a test run
std::future<std::string> g_testWrite;
H3Options g_testOpt;
std::wstring g_testFirst;

std::string Utf8(const std::wstring& w); // (below)
// Fizgig H3 Tweaks after the LoRAs: in when it is set to do something and ComfyUI has it, or has not said yet (then taken out again as the
// run is sent if it lacks it). Detail "stable across frames": only detail that is the same in every frame, so it does not shimmer.
bool FizgigOn() {
    if (std::fabs(g_v.detail) < 0.001f && g_v.promptStrength < 0.001f) return false;
    return !g_v.vedaKnown || g_v.vedaInfo.fizgig;
}
json FizgigNode(const json& model) {
    return {{"model", model}, {"high_freq_detail", std::round(g_v.detail * 100.0f) / 100.0f}, {"detail_mode", "stable across frames"},
            {"composition", 0.0}, {"prompt_strength", std::round(g_v.promptStrength * 100.0f) / 100.0f}, {"report", false}};
}

int g_starMethod = 0; // Star in a video: 0 swap the person (Character Swap LoRA, the clip guides), 1 recreate the clip with them (reference)
std::wstring g_swapFolder; // Star in a video: your folder of clips to swap people into (its browser on the page)
bool g_starPick = false;   // "Choose from the gallery": the gallery shows a bar to take the selected picture

std::wstring SettingsPath() { return DataDir() + L"\\video_settings.json"; }

void LoadSettings() {
    g_v.loaded = true;
    std::ifstream f(SettingsPath());
    if (!f) return;
    json j = json::parse(f, nullptr, false);
    if (!j.is_object()) return;
    g_v.opt.quality = std::clamp(j.value("quality", 1), 0, kQualityCount - 1);
    g_v.opt.res = std::clamp(j.value("res", 2), 0, 3);
    g_v.opt.shape = std::clamp(j.value("shape", 0), 0, 3);
    g_v.opt.seconds = std::clamp(j.value("seconds", 5.0), 3.0, kLongMax);
    g_v.continuumWas = j.value("continuumWas", false);
    g_v.swapMask = j.value("swapMask", true);
    g_v.opt.times = std::clamp(j.value("times", 1), 1, 4);
    g_v.autoWrite = j.value("autoWrite", true);
    g_v.veda = j.value("veda", true);
    g_v.vedaWasReady = j.value("vedaWasReady", false);
    g_v.leftW = std::clamp(j.value("leftW", 0.0f), 0.0f, 4000.0f);
    g_v.playerFrac = std::clamp(j.value("playerFrac", 0.45f), 0.15f, 0.85f);
    g_v.thumbW = std::clamp(j.value("thumbW", 132.0f), 80.0f, 420.0f);
    g_starMethod = std::clamp(j.value("starMethod", 0), 0, 1);
    g_v.camera = std::max(0, j.value("camera", 0));
    g_v.cameraByPreset = j.value("cameraByPreset", false);
    if (!j.contains("cameraByPreset")) g_v.camera = 0; // (the first build that has it: a move left on by a preset before is not kept)
    if (j.contains("presetShots") && j["presetShots"].is_object()) {
        std::error_code ec;
        for (auto it = j["presetShots"].begin(); it != j["presetShots"].end(); ++it) {
            if (!it.value().is_string()) continue;
            const std::string s = it.value().get<std::string>();
            std::wstring p(static_cast<size_t>(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0)), L'\0');
            if (!s.empty()) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), p.data(), static_cast<int>(p.size()));
            if (fs::exists(p, ec)) g_presetShots[it.key()] = p;
        }
    }
    g_v.detail = std::clamp(j.value("detail", 0.15f), -0.5f, 0.6f);
    g_v.promptStrength = std::clamp(j.value("promptStrength", 0.0f), 0.0f, 1.0f);
    g_v.crispTo = std::clamp(j.value("crispTo", 1440), 720, 2160);
    g_v.crispAfterSwap = j.value("crispAfterSwap", false);
    g_v.facesAfterSwap = j.value("facesAfterSwap", true);
    g_v.oneAtATime = j.value("oneAtATime", true);
    g_v.starUseSheet = j.value("starUseSheet", true);
    const std::string folder = j.value("swapFolder", std::string());
    g_swapFolder.assign(static_cast<size_t>(MultiByteToWideChar(CP_UTF8, 0, folder.c_str(), static_cast<int>(folder.size()), nullptr, 0)), L'\0');
    if (!folder.empty()) MultiByteToWideChar(CP_UTF8, 0, folder.c_str(), static_cast<int>(folder.size()), g_swapFolder.data(), static_cast<int>(g_swapFolder.size()));
}

void SaveSettings() {
    if (!g_v.loaded) LoadSettings(); // (saved before anything read them: it would write the defaults over yours)
    std::error_code ec;
    fs::create_directories(DataDir(), ec);
    std::ofstream f(SettingsPath(), std::ios::trunc);
    if (f) f << json({{"quality", g_v.opt.quality}, {"res", g_v.opt.res}, {"shape", g_v.opt.shape}, {"seconds", g_v.opt.seconds}, {"times", g_v.opt.times}, {"autoWrite", g_v.autoWrite}, {"veda", g_v.veda}, {"vedaWasReady", g_v.vedaWasReady}, {"continuumWas", g_v.continuumWas}, {"swapMask", g_v.swapMask},
                     {"leftW", g_v.leftW}, {"playerFrac", g_v.playerFrac}, {"thumbW", g_v.thumbW}, {"starMethod", g_starMethod},
                     {"detail", g_v.detail}, {"promptStrength", g_v.promptStrength}, {"crispTo", g_v.crispTo},
                     {"crispAfterSwap", g_v.crispAfterSwap}, {"facesAfterSwap", g_v.facesAfterSwap}, {"oneAtATime", g_v.oneAtATime}, {"starUseSheet", g_v.starUseSheet},
                     {"swapFolder", Utf8(g_swapFolder)}, {"camera", g_v.camera}, {"cameraByPreset", g_v.cameraByPreset}, {"presetShots", [] {
                         json m = json::object();
                         for (const auto& [k, v] : g_presetShots) m[k] = Utf8(v);
                         return m;
                     }()}}).dump(1);
}

// The camera move picked (nullptr = Auto), and your idea with it: what the video prompt is written for
const char* CameraText() { return kCameras[std::clamp(g_v.camera, 0, kCameraCount - 1)].text; }
std::string LowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string IdeaWithCamera() {
    const char* cam = CameraText();
    if (!cam) return g_v.idea;
    return (g_v.idea.empty() ? std::string() : g_v.idea + ". ") + "Camera wish (only if it suits this picture): " + cam + ".";
}

// A video sent from Animate with a preset's idea: its card shows it once it is made
void NotePresetSend() {
    for (const Preset& p : kPresets)
        if (g_v.idea == p.idea) g_presetWaiting.push_back({LastEditActivity(), p.name});
}

void PumpPresetShots(AppState& app) {
    if (g_presetWaiting.empty() || !app.comfySvc) return;
    const std::vector<ComfyActivity> acts = app.comfySvc->Activity();
    for (size_t i = 0; i < g_presetWaiting.size();) {
        bool done = false;
        for (const ComfyActivity& a : acts) {
            if (a.id != g_presetWaiting[i].first) continue;
            if (a.state == ComfyState::Done) {
                for (const ComfyResult& r : a.files)
                    if (!r.localPath.empty() && r.video) g_presetShots[g_presetWaiting[i].second] = r.localPath;
                SaveSettings();
                done = true;
            } else if (a.state == ComfyState::Failed) {
                done = true;
            }
        }
        if (done) g_presetWaiting.erase(g_presetWaiting.begin() + i);
        else ++i;
    }
}

// Did ComfyUI say it has Veda's node and this predictor?
bool VedaReady(const char* predictor) {
    return g_v.vedaKnown && g_v.vedaInfo.node &&
           std::find(g_v.vedaInfo.predictors.begin(), g_v.vedaInfo.predictors.end(), predictor) != g_v.vedaInfo.predictors.end();
}

// Does a run with this predictor get Veda's node? The switch is on, and ComfyUI has it - or has not answered yet: the node then goes in anyway,
// and is taken out again as the run is sent if that ComfyUI does not have it (DropMissingVeda)
bool VedaFor(const char* predictor) {
    return g_v.veda && (VedaReady(predictor) || !g_v.vedaKnown);
}

bool HasLora(const char* name) { // (by file name: the LoRA sorter may have put it in a model folder)
    if (g_v.lorasKnown) return LoraListed(g_v.loras, name);
    if (g_v.diskKnown) return LoraListed(g_v.onDisk, name);
    return true; // cannot tell: ComfyUI says so if it is missing
}

std::vector<std::string> LorasOnDisk();

// Which LoRAs ComfyUI has (and which are on disk): asked when a page that needs them is open, again after a minute
void RefreshLoraList(AppState& app) {
    if (g_v.lorasFut.valid() && g_v.lorasFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        g_v.loras = g_v.lorasFut.get();
        g_v.lorasKnown = !g_v.loras.empty();
    }
    if (!g_v.lorasFut.valid() && ImGui::GetTime() - g_v.lorasAsked > 60.0) {
        g_v.lorasAsked = ImGui::GetTime();
        g_v.lorasFut = std::async(std::launch::async, ComfyListLoras, app.comfy.url);
        if (!g_v.diskFut.valid() && !g_v.diskKnown) g_v.diskFut = std::async(std::launch::async, LorasOnDisk);
    }
    if (g_v.diskFut.valid() && g_v.diskFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        g_v.onDisk = g_v.diskFut.get();
        g_v.diskKnown = !ComfyModelsDir().empty();
    }
}

std::vector<std::string> LorasOnDisk() {
    std::vector<std::string> out;
    const std::wstring dir = ComfyModelsDir();
    if (dir.empty()) return out;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(fs::path(dir) / L"loras", fs::directory_options::skip_permission_denied, ec)) { // (in model folders too)
        if (!e.is_regular_file(ec)) continue;
        const std::string f = Utf8(e.path().filename().wstring());
        for (const char* want : {kFastLora, kBalancedLora, kPresenterLora, kSwapLora, kRefLoras[0], kRefLoras[1], kRefLoras[2], kOrbitLora, kCrispLora}) {
            if (LoraFileName(f) == LoraFileName(want)) out.push_back(want);
        }
    }
    return out;
}

bool Chip(const char* label, bool on) {
    ImGui::PushStyleColor(ImGuiCol_Button, on ? ui::ChipActiveColor() : ui::ChipColor());
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::ChipHoverColor());
    const bool clicked = ImGui::Button(label);
    ImGui::PopStyleColor(2);
    return clicked;
}

// A picture slot: its thumbnail, or a dashed box; buttons to fill or clear it
bool PictureSlot(AppState& app, const char* id, const char* title, std::wstring& path, float size) {
    bool changed = false;
    ImGui::PushID(id);
    ImGui::BeginGroup();
    ImGui::TextColored(ui::kTextDim, "%s", title);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##slot", ImVec2(size, size));
    const bool hov = ImGui::IsItemHovered();
    if (hov && !path.empty() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) ViewPictureLarge(app, path);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!path.empty()) {
        dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), ui::ThumbPlaceholder(), ui::Dp(8.0f));
        const AppState::LibThumb lt = app.LibraryThumb(path);
        if (lt.srv && lt.w > 0 && lt.h > 0) {
            const float s = std::min(size / lt.w, size / lt.h);
            const ImVec2 a(p.x + (size - lt.w * s) * 0.5f, p.y + (size - lt.h * s) * 0.5f);
            dl->AddImageRounded((ImTextureID)(intptr_t)lt.srv, a, ImVec2(a.x + lt.w * s, a.y + lt.h * s), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, ui::Dp(6.0f));
        }
        if (hov) {
            const std::wstring name = fs::path(path).filename().wstring();
            std::string utf8(WideCharToMultiByte(CP_UTF8, 0, name.c_str(), static_cast<int>(name.size()), nullptr, 0, nullptr, nullptr), '\0');
            WideCharToMultiByte(CP_UTF8, 0, name.c_str(), static_cast<int>(name.size()), utf8.data(), static_cast<int>(utf8.size()), nullptr, nullptr);
            ImGui::SetTooltip("%s\nDouble click: see it big", utf8.c_str());
        }
    } else {
        dl->AddRect(p, ImVec2(p.x + size, p.y + size), hov ? ImGui::GetColorU32(ui::kAccent) : IM_COL32(255, 255, 255, 50), ui::Dp(8.0f), 0, ui::Dp(1.5f));
        const char* t = "none";
        const ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2(p.x + (size - ts.x) * 0.5f, p.y + (size - ts.y) * 0.5f), ImGui::GetColorU32(ui::kTextDim), t);
    }
    // a picture dragged from the gallery
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("MV_IMAGES")) {
            const int idx = *static_cast<const int*>(pl->Data);
            if (idx >= 0 && idx < static_cast<int>(app.entries.size())) path = app.entries[idx].path, changed = true;
        }
        ImGui::EndDragDropTarget();
    }
    const bool haveSel = app.selectedIndex >= 0 && app.selectedIndex < static_cast<int>(app.entries.size()) && !app.entries[app.selectedIndex].metadata.isVideo;
    ImGui::BeginDisabled(!haveSel);
    if (ImGui::SmallButton("Gallery's")) path = app.entries[app.selectedIndex].path, changed = true;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("The picture selected in the gallery");
    ImGui::SameLine();
    if (ImGui::SmallButton("File...")) {
        const std::wstring f = PickPicture(L"A picture for the video");
        if (!f.empty()) path = f, changed = true;
    }
    if (!path.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) path.clear(), changed = true;
    }
    ImGui::EndGroup();
    ImGui::PopID();
    return changed;
}

} // namespace

void ViewPictureLarge(AppState& app, const std::wstring& path) {
    for (size_t i = 0; i < app.entries.size(); ++i) {
        if (_wcsicmp(app.entries[i].path.c_str(), path.c_str()) == 0) { // in the gallery's folder: shown there
            app.SelectIndex(static_cast<int>(i));
            app.requestTab = 0;
            return;
        }
    }
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); // elsewhere: the picture viewer Windows uses
}

namespace {

std::string ShortLabel(const std::string& text) {
    std::string s = text.substr(0, 44);
    for (char& c : s) if (c == '\n' || c == '\r') c = ' ';
    if (text.size() > 44) s += "...";
    return s;
}

std::string Utf8(const std::wstring& w) {
    std::string out(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), static_cast<int>(out.size()), nullptr, nullptr);
    return out;
}

// what a written prompt belongs to: change the pictures, the idea or the length and it is out of date
std::string PromptKey(const std::wstring& first, const std::wstring& last, const std::string& idea, double seconds) {
    // (Presenter writes its own form: switching to it or away writes the prompt again)
    return Utf8(first) + "|" + Utf8(last) + "|" + idea + "|" + std::to_string(static_cast<int>(std::lround(seconds))) + (g_v.opt.quality == 3 ? "|presenter" : "") +
           (g_v.opt.quality == kLtxQuality ? "|ltx" : ""); // (LTX-2.5 writes its own form too)
}

// ---- the videos: ComfyUI's output folder, played on this page
struct Clip {
    std::wstring path;
    std::string name;
    fs::file_time_type time;
};
struct Videos {
    std::vector<Clip> clips;
    std::future<std::vector<Clip>> fut;
    double scanned = -1000.0;
    int doneSeen = -1;
    std::unique_ptr<VideoPlayer> player;
    bool muted = false;
    std::set<std::wstring> picked; // Ctrl+clicked, to delete together
    int confirmAll = 0;            // "Clear out all": the second click deletes
};
Videos g_vid;

// Videos taken off the list and moved to the Recycle Bin (with the files the app keeps next to them); the player lets go of one it shows.
// Clearing out the videos made (2026-10-23): to the Recycle Bin, so a wrong click can be undone.
void DeleteVideos(AppState& app, const std::vector<std::wstring>& videos) {
    if (videos.empty()) return;
    if (g_vid.player && g_vid.player->IsOpen())
        for (const std::wstring& v : videos)
            if (_wcsicmp(g_vid.player->Path().c_str(), v.c_str()) == 0) g_vid.player->Close();
    std::vector<std::wstring> files;
    std::error_code ec;
    for (const std::wstring& v : videos) {
        files.push_back(v);
        for (const wchar_t* side : {L".take.json", L".rgb", L".frame0.png", L".frame1.png", L".frame2.png", L".frame3.png"})
            if (fs::exists(v + side, ec)) files.push_back(v + side);
    }
    std::wstring err;
    if (!DeleteFilesToRecycleBin(GetActiveWindow(), files, &err)) {
        app.SetStatus("Could not delete them: " + Utf8(err));
        return;
    }
    std::set<std::wstring> gone(videos.begin(), videos.end());
    g_vid.clips.erase(std::remove_if(g_vid.clips.begin(), g_vid.clips.end(), [&](const Clip& c) { return gone.count(c.path) > 0; }), g_vid.clips.end());
    for (const std::wstring& v : videos) g_vid.picked.erase(v);
    g_vid.scanned = -1000.0; // (looked at again)
    if (app.comfySvc) app.comfySvc->ClearResults(true);
    app.SetStatus(videos.size() == 1 ? std::string("Moved the video to the Recycle Bin") : "Moved " + std::to_string(videos.size()) + " videos to the Recycle Bin");
}

std::vector<Clip> ScanVideos(std::vector<std::wstring> dirs) {
    std::vector<Clip> out;
    for (const std::wstring& d : dirs) {
        std::error_code ec;
        if (d.empty() || !fs::is_directory(d, ec)) continue;
        int looked = 0;
        for (fs::recursive_directory_iterator it(d, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end && looked < 5000; it.increment(ec), ++looked) {
            if (it.depth() > 2) {
                it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            std::wstring ext = it->path().extension().wstring();
            for (wchar_t& c : ext) c = static_cast<wchar_t>(towlower(c));
            if (ext != L".mp4" && ext != L".webm" && ext != L".mov" && ext != L".mkv") continue;
            { // a Star / Swap guide clip that an older version kept as if ComfyUI had made it (ref_<n>_<n>, or 16 hex digits)
                const std::wstring stem = it->path().stem().wstring();
                const bool hex16 = stem.size() == 16 && std::all_of(stem.begin(), stem.end(), [](wchar_t ch) { return iswxdigit(ch) != 0; });
                if (hex16 || stem.rfind(L"ref_", 0) == 0) continue;
            }
            Clip c;
            c.path = it->path().wstring();
            c.name = Utf8(it->path().filename().wstring());
            c.time = it->last_write_time(ec);
            if (std::none_of(out.begin(), out.end(), [&](const Clip& o) { return _wcsicmp(o.path.c_str(), c.path.c_str()) == 0; })) out.push_back(std::move(c));
        }
    }
    std::sort(out.begin(), out.end(), [](const Clip& a, const Clip& b) { return a.time > b.time; });
    if (out.size() > 120) out.resize(120);
    return out;
}

// The videos on disk: looked at again every 15 s, and as soon as a video is done
void RefreshClips(AppState& app) {
    int doneNow = 0;
    if (app.comfySvc) {
        for (const ComfyActivity& a : app.comfySvc->Activity()) doneNow += a.state == ComfyState::Done ? 1 : 0;
    }
    if (g_vid.fut.valid() && g_vid.fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) g_vid.clips = g_vid.fut.get();
    if (!g_vid.fut.valid() && (ImGui::GetTime() - g_vid.scanned > 15.0 || doneNow != g_vid.doneSeen)) {
        g_vid.scanned = ImGui::GetTime();
        g_vid.doneSeen = doneNow;
        const std::wstring out = ComfyOutputDir(app.comfy);
        g_vid.fut = std::async(std::launch::async, ScanVideos, std::vector<std::wstring>{out.empty() ? std::wstring() : out + L"\\video", DataDir() + L"\\comfy_results"});
    }
}

void PlayClip(AppState& app, const std::wstring& path, bool autoplay = true) {
    if (!g_vid.player) g_vid.player = std::make_unique<VideoPlayer>();
    ID3D11DeviceContext* ctx = nullptr;
    app.Device()->GetImmediateContext(&ctx);
    if (!g_vid.player->Open(app.Device(), ctx, path, 0.85f, g_vid.muted, true, autoplay)) app.SetStatus(g_vid.player->Error());
    if (ctx) ctx->Release();
}

std::string Clock(double s) {
    const int t = static_cast<int>(std::max(0.0, s));
    char buf[16];
    snprintf(buf, sizeof(buf), "%d:%02d", t / 60, t % 60);
    return buf;
}

bool RecreateVideo(AppState& app, const std::wstring& video);

// The player: the video fitted in the box, click to pause, a bar under it
void DrawCrispRow(AppState& app, const std::wstring& video); // (below)

void DrawPlayer(AppState& app, float boxH) {
    VideoPlayer* vp = g_vid.player.get();
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##screen", ImVec2(w, boxH));
    const bool clicked = ImGui::IsItemClicked();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + boxH), IM_COL32(0, 0, 0, 255), ui::Dp(8.0f));
    if (!vp || !vp->IsOpen()) {
        const char* t = EditsMode() == 5 ? "Pick a video on the right to open it here" : "Pick a video below to play it here";
        const ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (boxH - ts.y) * 0.5f), ImGui::GetColorU32(ui::kTextDim), t);
        return;
    }
    vp->Update();
    if (ID3D11ShaderResourceView* srv = vp->Srv(); srv && vp->Width() > 0 && vp->Height() > 0) {
        const float s = std::min(w / vp->Width(), boxH / vp->Height());
        const ImVec2 a(p.x + (w - vp->Width() * s) * 0.5f, p.y + (boxH - vp->Height() * s) * 0.5f);
        if (app.ShouldBlur(vp->Path())) { // NSFW (blurring on): its blurred still instead of the frames (the player is blurred too)
            const AppState::LibThumb lt = app.LibraryThumb(vp->Path());
            if (lt.srv) dl->AddImage((ImTextureID)(intptr_t)lt.srv, a, ImVec2(a.x + vp->Width() * s, a.y + vp->Height() * s));
        } else {
            dl->AddImage((ImTextureID)(intptr_t)srv, a, ImVec2(a.x + vp->Width() * s, a.y + vp->Height() * s));
        }
        DrawNsfwOverlay(app, vp->Path(), a, ImVec2(a.x + vp->Width() * s, a.y + vp->Height() * s));
    }
    if (clicked) vp->Toggle();
    if (ImGui::Button(vp->Playing() ? "Pause" : "Play", ImVec2(ui::Dp(64.0f), 0))) vp->Toggle();
    ImGui::SameLine();
    float pos = static_cast<float>(vp->Position());
    const float dur = static_cast<float>(std::max(0.01, vp->Duration()));
    ImGui::SetNextItemWidth(std::max(ui::Dp(80.0f), ImGui::GetContentRegionAvail().x - ui::Dp(340.0f)));
    if (ImGui::SliderFloat("##seek", &pos, 0.0f, dur, (Clock(pos) + " / " + Clock(dur)).c_str())) vp->Seek(pos);
    ImGui::SameLine();
    if (ImGui::Checkbox("Mute", &g_vid.muted)) vp->SetMuted(g_vid.muted);
    ImGui::SameLine();
    {
        float pct = VideoPlayer::VideoVolume() * 100.0f;
        ImGui::SetNextItemWidth(ui::Dp(84.0f));
        if (ImGui::SliderFloat("##vidvol", &pct, 0.0f, 100.0f, "vol %.0f%%")) {
            app.videoVolume = pct / 100.0f;
            VideoPlayer::SetVideoVolume(app.videoVolume);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) app.SaveUiSettings();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("The volume of every video in the app (also under View)");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Explorer")) {
        const std::wstring arg = L"/select,\"" + vp->Path() + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy")) {
        std::string msg;
        CopyImagesToClipboard(GetActiveWindow(), {vp->Path()}, false, &msg);
        app.SetStatus(msg);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("The video file, to paste in Explorer, a chat or an editor");
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy clean")) {
        std::string msg;
        CopyImagesToClipboard(GetActiveWindow(), {vp->Path()}, true, &msg);
        app.SetStatus(msg);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("A copy without its metadata (no prompt, no workflow inside), to post");
    ImGui::SameLine();
    if (ImGui::SmallButton("Recreate")) RecreateVideo(app, vp->Path());
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("Loads this video's start and end pictures, prompt, length, quality and size onto the page,\nto make it again another way");
    DrawCrispRow(app, vp->Path());
    // what it is, and how long it took to make
    double took = 0;
    std::string what;
    const bool known = ComfyGenerationTime(vp->Path(), took, &what);
    std::string line = Utf8(fs::path(vp->Path()).filename().wstring()) + "   " + std::to_string(vp->Width()) + " x " + std::to_string(vp->Height());
    if (known) line += "   made in " + FormatTook(took) + (what.empty() ? std::string() : "  (" + what.substr(0, what.find(':')) + ")");
    ImGui::TextDisabled("%s", line.c_str());
}

// ================================================================== the page's three modes
// 0 Animate a picture, 1 Star in a video (you, in a clip you give: its moves, camera and music), 2 Create a character (Higgsfield's AI influencer:
// a character built from looks, drawn by Qwen in a white studio, then animated or put into a video).
int g_mode = 0;


// ---- star in a video
struct Star {
    std::vector<std::wstring> who; // the people / characters to put in, in order (<Picture 1>...)
    std::wstring video;            // the clip whose motion, camera and timing are followed
    double start = 0;              // where in it to begin (seconds)
    bool keepSound = true;         // its soundtrack as the video's sound (<Audio 1>)
    std::string idea;              // what else to change ("she takes his place", "in a neon club")
    std::string prompt;
    bool edited = false;
    std::string writtenKey;
    int quality = 0;               // 0 fast (the ref2v turbo LoRA), 1 best (20 steps; 16 for a swap)
    VideoInfo info;                // of `video` (length, size)
    std::wstring infoFor;
    // each picture is one person: their name (a saved character's, or the file's; shown on their card, given to the writer) and whom they
    // replace in the clip ("" = decided from the idea and from what the clip shows), kept by the picture's lowercased path
    std::map<std::wstring, std::string> names, replaces;
};
Star g_star;
bool g_starBrowse = false; // the clip browser is open (it is always open while no clip is chosen)

std::wstring StarKeyOf(const std::wstring& pic) {
    std::wstring k = pic;
    for (wchar_t& c : k) c = static_cast<wchar_t>(towlower(c));
    return k;
}
std::string StarName(const std::wstring& pic) {
    auto it = g_star.names.find(StarKeyOf(pic));
    if (it != g_star.names.end() && !it->second.empty()) return it->second;
    if (const CharacterProfile* p = FindProfile(pic); p && !p->name.empty()) return p->name;
    std::string n = Utf8(fs::path(pic).stem().wstring());
    std::replace(n.begin(), n.end(), '_', ' ');
    return n;
}
std::string StarReplaces(const std::wstring& pic) {
    auto it = g_star.replaces.find(StarKeyOf(pic));
    return it == g_star.replaces.end() ? std::string() : it->second;
}
void StarAdd(const std::wstring& pic) {
    if (pic.empty() || g_star.who.size() >= 4) return;
    for (const std::wstring& w : g_star.who) {
        if (_wcsicmp(w.c_str(), pic.c_str()) == 0) return;
    }
    g_star.who.push_back(pic);
}

const char* const kRefModel = "minimax_h3_ref2va_pruned_int8_convrot.safetensors";
const std::string& StarUnetOverride() { // tests: MV_STAR_UNET names another model (to check the wiring without the reference model)
    static const std::string over = [] {
        char buf[256] = {};
        return GetEnvironmentVariableA("MV_STAR_UNET", buf, sizeof(buf)) ? std::string(buf) : std::string();
    }();
    return over;
}
// Star in a video's model: PRISM when it is there (since 2026-10-26; it is a ref2va model too). Tested on the same swap (a woman into a man's
// walk at a gas station, 768p, 5 s, the same seed and prompt, BFS swap LoRA + lightx2v 4-step): the same moves, likeness and background as
// Comfy-Org's ref2va int8, a little more texture in the hair, at the same speed - from a file 3.4 GB smaller, and the same one Animate a
// picture uses, so going from one to the other loads nothing new.
std::string StarUnet() {
    if (!StarUnetOverride().empty()) return StarUnetOverride();
    return HavePrism() ? std::string(kPrism) : std::string(kRefModel);
}
const char* const kRefModelUrl = "https://huggingface.co/Comfy-Org/MiniMax-H3/resolve/main/diffusion_models/minimax_h3_ref2va_pruned_int8_convrot.safetensors";


std::string StarKey() {
    std::string k;
    for (const std::wstring& w : g_star.who) k += Utf8(w) + "=" + StarReplaces(w) + ";";
    return k + "|" + Utf8(g_star.video) + "|" + std::to_string(static_cast<int>(g_star.start)) + "|" + g_star.idea + "|" + (g_star.keepSound ? "s" : "") + "|" +
           (g_starMethod == 0 ? "swap|" : "") +
           std::to_string(static_cast<int>(std::lround(g_v.opt.seconds)));
}

// ---- Recreate: a video made with H3 back on the page - its pictures, prompt, length, quality and size, read from the workflow ComfyUI
// saved inside it - to make it again at another size, quality or length
std::wstring WideOf(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// A file named in the workflow: the picture this app uploaded under that name, else ComfyUI's own copy in its input folder
std::wstring FindInput(const std::string& name) {
    if (name.empty()) return {};
    std::error_code ec;
    const std::wstring ours = ComfyUploadedFrom(name);
    if (!ours.empty() && fs::exists(ours, ec)) return ours;
    const std::wstring in = ComfyInputDir();
    if (in.empty()) return {};
    std::wstring rel = WideOf(name);
    std::replace(rel.begin(), rel.end(), L'/', L'\\');
    const fs::path p = fs::path(in) / rel;
    return fs::exists(p, ec) ? p.wstring() : std::wstring();
}

// The file a node input comes from: follows the links back to a LoadImage / LoadVideo (through scaling and GetVideoComponents)
std::string SourceFile(const json& wf, const json& link, int depth = 0) {
    if (depth > 8 || !link.is_array() || link.empty()) return {};
    const std::string id = link[0].is_string() ? link[0].get<std::string>() : link[0].is_number() ? std::to_string(link[0].get<long long>()) : std::string();
    if (!wf.contains(id) || !wf[id].is_object() || !wf[id].contains("inputs")) return {};
    const json& n = wf[id];
    const json& in = n["inputs"];
    const std::string cls = n.value("class_type", std::string());
    if (cls == "LoadImage") return in.value("image", std::string());
    if (cls == "LoadVideo") return in.value("file", std::string());
    for (const char* k : {"image", "images", "video", "pixels"}) {
        if (in.contains(k) && in[k].is_array()) return SourceFile(wf, in[k], depth + 1);
    }
    return {};
}

int NearestRes(double pixels) {
    int best = 2;
    double bestD = 1e30;
    for (int i = 0; i < 4; ++i) {
        const double d = std::fabs(std::log(pixels / (static_cast<double>(kRes[i].w) * kRes[i].h)));
        if (d < bestD) bestD = d, best = i;
    }
    return best;
}

bool RecreateVideo(AppState& app, const std::wstring& video) {
    const std::string fileName = Utf8(fs::path(video).filename().wstring());
    auto fail = [&](const std::string& why) {
        Toast(ToastKind::Error, "Cannot recreate " + fileName, why, {}, {}, "recreate");
        return false;
    };
    json wf;
    for (const RawField& f : ReadVideoWorkflowFields(video)) {
        if (f.key == "prompt") {
            wf = json::parse(f.value, nullptr, false);
            break;
        }
    }
    if (!wf.is_object()) return fail("It has no ComfyUI workflow saved inside.");
    const json* i2v = nullptr;
    const json* r2v = nullptr;
    const json* guide = nullptr;
    bool swapLora = false;
    std::string lora;
    double megapixels = 0;
    for (const auto& [id, n] : wf.items()) {
        if (!n.is_object() || !n.contains("inputs") || !n["inputs"].is_object()) continue;
        const std::string cls = n.value("class_type", std::string());
        if (cls == "MiniMaxH3ImageToVideo") i2v = &n["inputs"];
        else if (cls == "MiniMaxH3ReferenceToVideo") r2v = &n["inputs"];
        else if (cls == "MiniMaxH3AddGuide") guide = &n["inputs"];
        else if (cls == "LoraLoaderModelOnly") {
            const std::string name = n["inputs"].value("lora_name", std::string());
            if (name == kSwapLora) swapLora = true;
            else lora = name;
        }
        else if (cls == "ImageScaleToTotalPixels" && n["inputs"].contains("megapixels") && n["inputs"]["megapixels"].is_number()) megapixels = n["inputs"]["megapixels"].get<double>();
    }
    const json* in = i2v ? i2v : r2v;
    H3Options& o = VideoOptions();
    if (!in) { // LTX-2.5 (BuildLtx): its prompt, pictures, length and size, onto Animate a picture with the LTX quality - pick another and it is kept
        const json* cond = nullptr;
        const json* empty = nullptr;
        const json* firstAt = nullptr;
        const json* lastAt = nullptr;
        bool twoPass = false;
        for (const auto& [id, n] : wf.items()) {
            if (!n.is_object() || !n.contains("inputs") || !n["inputs"].is_object()) continue;
            const std::string cls = n.value("class_type", std::string());
            const json& ni = n["inputs"];
            if (cls == "LTXVConditioning") cond = &ni;
            else if (cls == "EmptyLTXVLatentVideo") empty = &ni;
            else if (cls == "LatentUpscaleModelLoader") twoPass = true; // (the first pass is at half size)
            else if (cls == "LTXVImgToVideoInplace" && !firstAt && ni.contains("image")) firstAt = &ni["image"];
            else if (cls == "LTXVAddGuide" && ni.contains("image") && ni.contains("frame_idx") && ni["frame_idx"].is_number()) {
                if (ni["frame_idx"].get<int>() == 0) firstAt = &ni["image"];
                else lastAt = &ni["image"];
            }
        }
        if (!cond || !empty) return fail("It was not made with MiniMax H3 or LTX-2.5 (no video node of theirs in its workflow).");
        std::string ltxPrompt;
        if ((*cond).contains("positive") && (*cond)["positive"].is_array() && !(*cond)["positive"].empty()) {
            const json& link = (*cond)["positive"];
            const std::string id = link[0].is_string() ? link[0].get<std::string>() : link[0].is_number() ? std::to_string(link[0].get<long long>()) : std::string();
            if (wf.contains(id) && wf[id].contains("inputs")) ltxPrompt = wf[id]["inputs"].value("text", std::string());
        }
        if ((*empty).contains("length") && (*empty)["length"].is_number()) o.seconds = std::clamp(std::round(((*empty)["length"].get<double>() - 1.0) / 24.0), 3.0, 15.0);
        double lw = (*empty).contains("width") && (*empty)["width"].is_number() ? (*empty)["width"].get<double>() : 0;
        double lh = (*empty).contains("height") && (*empty)["height"].is_number() ? (*empty)["height"].get<double>() : 0;
        if (twoPass) lw *= 2, lh *= 2;
        if (lw > 0 && lh > 0) {
            o.res = NearestRes(lw * lh);
            o.shape = lw > lh * 1.05 ? 1 : lh > lw * 1.05 ? 2 : 3;
        }
        std::vector<std::string> gone;
        auto pictureOf = [&](const json* link) {
            if (!link) return std::wstring();
            const std::string name = SourceFile(wf, *link);
            const std::wstring path = FindInput(name);
            if (!name.empty() && path.empty()) gone.push_back(name);
            return path;
        };
        const std::wstring first = pictureOf(firstAt), last = pictureOf(lastAt);
        if (!gone.empty()) return fail("Its picture " + gone.front() + " is not in ComfyUI's input folder any more.");
        o.quality = kLtxQuality;
        g_mode = 0;
        g_v.first = first;
        o.last = last;
        g_v.idea.clear();
        g_v.context.clear();
        g_v.prompt = ltxPrompt;
        g_v.edited = !ltxPrompt.empty(); // kept as it is, whatever quality is picked now (it is not written again)
        g_v.writtenKey = PromptKey(g_v.first, o.last, g_v.idea, o.seconds);
        SaveSettings();
        Toast(ToastKind::Info, "Loaded " + fileName,
              "Its picture, prompt and settings are on the page (LTX-2.5). Pick another quality (an H3 one) if you like - the prompt stays as it was - and press Make.", {}, {},
              "recreate");
        return true;
    }
    // what both kinds share: the prompt, the length, the size
    const std::string prompt = (*in).contains("prompt") && (*in)["prompt"].is_string() ? (*in)["prompt"].get<std::string>() : std::string();
    if ((*in).contains("length") && (*in)["length"].is_number()) o.seconds = std::clamp(std::round((*in)["length"].get<double>() / 24.0), 3.0, 15.0);
    const bool fixedSize = (*in).contains("width") && (*in)["width"].is_number() && (*in).contains("height") && (*in)["height"].is_number();
    const double w = fixedSize ? (*in)["width"].get<double>() : 0, h = fixedSize ? (*in)["height"].get<double>() : 0;
    if (fixedSize) o.res = NearestRes(w * h);
    else if (megapixels > 0) o.res = NearestRes(megapixels * 1.0e6);
    std::vector<std::string> missing;
    auto resolve = [&](const json& link) {
        const std::string name = SourceFile(wf, link);
        const std::wstring path = FindInput(name);
        if (!name.empty() && path.empty()) missing.push_back(name);
        return path;
    };
    if (i2v) {
        const std::wstring first = (*in).contains("first_frame") ? resolve((*in)["first_frame"]) : std::wstring();
        const std::wstring last = (*in).contains("last_frame") ? resolve((*in)["last_frame"]) : std::wstring();
        if (!missing.empty()) return fail("Its picture " + missing.front() + " is not in ComfyUI's input folder any more.");
        if (lora == kFastLora) o.quality = 0;
        else if (lora == kBalancedLora) o.quality = 1;
        else if (lora == kPresenterLora) o.quality = 3;
        else if (lora.empty()) o.quality = 2;
        o.shape = !fixedSize ? 0 : w > h * 1.05 ? 1 : h > w * 1.05 ? 2 : 3; // its own shape (no fixed size: like the picture)
        g_mode = 0;
        g_v.first = first;
        o.last = last;
        g_v.idea.clear();
        g_v.context.clear();
        g_v.prompt = prompt;
        g_v.edited = !prompt.empty(); // its own prompt, kept as it is whatever quality is picked now (another one used to write it again)
        g_v.writtenKey = PromptKey(g_v.first, o.last, g_v.idea, o.seconds);
    } else {
        std::vector<std::wstring> who;
        for (int k = 0; k < 9; ++k) {
            const std::string key = "ref_images.ref_image_" + std::to_string(k);
            if ((*in).contains(key)) {
                const std::wstring p = resolve((*in)[key]);
                if (!p.empty()) who.push_back(p);
            }
        }
        const std::wstring clip = (*in).contains("ref_videos.ref_video_0") ? resolve((*in)["ref_videos.ref_video_0"])
                                  : guide && (*guide).contains("image")    ? resolve((*guide)["image"])
                                                                           : std::wstring();
        if (who.empty() || clip.empty())
            return fail(missing.empty() ? std::string("Its pictures or its clip are not named in the workflow.") : "Its file " + missing.front() + " is not in ComfyUI's input folder any more.");
        g_mode = 1;
        g_star.who = who;
        g_star.video = clip; // the clip as it was cut for that video: it starts at 0
        g_star.start = 0;
        g_starMethod = swapLora ? 0 : 1;
        g_star.keepSound = swapLora ? wf.dump().find("TrimAudioDuration") != std::string::npos : (*in).contains("ref_video_audios.ref_video_audio_0");
        g_star.quality = lora.empty() ? 1 : 0;
        g_star.idea.clear();
        g_star.prompt = prompt;
        g_star.edited = !prompt.empty(); // (kept as it is, like Animate a picture's)
        g_star.writtenKey = StarKey();
    }
    SaveSettings();
    Toast(ToastKind::Info, "Loaded " + fileName, "Its pictures, prompt and settings are on the page: change the size, quality or length and press Make.", {}, {}, "recreate");
    return true;
}

// the reference model: on disk next to ComfyUI's models? (asked again now and then)
struct RefModelState {
    bool known = false, have = false, omni = false; // omni: FastH3-Omni (Recreate's Fast)
    double asked = -1000.0;
    std::wstring dir; // ComfyUI's diffusion_models folder
    // the download (started by the user's click)
    std::atomic<bool> downloading{false};
    std::atomic<bool> failed{false};
    std::wstring part;
};
RefModelState g_ref;

void CheckRefModel() {
    if (ImGui::GetTime() - g_ref.asked < (g_ref.downloading ? 2.0 : 20.0)) return;
    g_ref.asked = ImGui::GetTime();
    const std::wstring models = ComfyModelsDir();
    g_ref.known = !models.empty();
    if (!g_ref.known) return;
    g_ref.dir = models + L"\\diffusion_models";
    std::error_code ec;
    g_ref.have = fs::exists(fs::path(g_ref.dir) / kRefModel, ec) || StarUnet() != kRefModel;
    g_ref.omni = fs::exists(fs::path(g_ref.dir) / kOmniModel, ec) && StarUnetOverride().empty(); // (Recreate's Fast keeps FastH3-Omni)
    g_ref.part = (fs::path(g_ref.dir) / kRefModel).wstring() + L".part";
}

struct FileDownload {
    std::atomic<bool> running{false}, failed{false};
    std::wstring part;
};
FileDownload g_balDl;

// A file into ComfyUI's models/<sub> folder in the background (curl: resumes, retries). The user's click starts it. With `sha`, the file
// must have that SHA-256 (else it is deleted and the download counts as failed).
void StartDownload(FileDownload& d, const char* url, const char* file, const wchar_t* sub, double minGb, const char* sha = nullptr) {
    const std::wstring models = ComfyModelsDir();
    if (d.running || models.empty()) return;
    const std::wstring dest = models + L"\\" + sub + L"\\" + std::wstring(file, file + strlen(file));
    d.part = dest + L".part";
    d.running = true;
    d.failed = false;
    std::thread([&d, dest, u = std::wstring(url, url + strlen(url)), minGb, want = std::string(sha ? sha : "")]() {
        std::string out;
        const bool ok = RunCaptureHidden(L"curl.exe -L --fail --retry 5 --retry-delay 10 -C - --no-progress-meter -o \"" + dest + L".part\" \"" + u + L"\"", out, true);
        std::error_code ec;
        bool good = ok && fs::exists(dest + L".part", ec) && fs::file_size(dest + L".part", ec) > static_cast<uintmax_t>(minGb * 1073741824.0);
        if (good && !want.empty()) {
            std::string hash;
            RunCaptureIdle(L"powershell.exe -NoProfile -Command \"(Get-FileHash -Algorithm SHA256 -LiteralPath '" + dest + L".part').Hash\"", hash, false);
            std::string h;
            for (char c : hash) {
                if (isxdigit(static_cast<unsigned char>(c))) h += static_cast<char>(tolower(static_cast<unsigned char>(c)));
            }
            if (h != want) {
                fs::remove(dest + L".part", ec); // a damaged or different file: next time from the start
                good = false;
            }
        }
        if (good) {
            fs::rename(dest + L".part", dest, ec);
            d.failed = static_cast<bool>(ec);
        } else {
            d.failed = true;
        }
        d.running = false;
    }).detach();
}
FileDownload g_swapDl, g_swapTurboDl, g_omniDl, g_prismDl;

// ---- Make it crisp: Open-H3-Regenerate-2K (NVlabs, 2026-10-28; Efficient-Large-Model/Open-H3-Regenerate-2K, MiniMax H3 Community License)
// redraws a video's fine detail at 1080p or 2K with MiniMax H3 itself: the clip goes through H3's VAE at H3's own size (768p), the MiniMax H3
// latent upscaler (LBH-123-AI) takes the latent to the new size, and FL2VA with the Open-H3 LoRA (its refiner + LightX2V Turbo + LMS Sharp, one
// file made by tools\h3turbo\setup_open_h3.ps1) samples it again: 2 steps from sigma 0.7, Veda on when ComfyUI has it. Since 2026-10-28 instead of
// FlashVSR+. Tested on the RTX 3090 with a 1344 x 768 H3 test clip (a freckled woman
// in a greenhouse): 5 s to 2560 x 1440 in 177 s, to 1920 x 1088 in 99 s; skin, lashes, knit and ferns redrawn and the face kept. The release
// setting (3 steps from 0.909) aged the face and straightened the curls, and took 512 s at 1080p without Veda (324 s for 2 steps without it). The
// prompt made no difference, so none is sent. 10 s in one pass filled the 24 GB and stalled; over 124 frames MMH3 Split Upscale samples a job
// in 124-frame chunks that overlap by 22 with motion anchors (10 s at 2K: 481 s from cold, no seam to be seen). A job is at most 8 s (2026-10-28: a 6 s video
// in two 3 s parts was wasteful); longer videos go in parts of up to
// 8 s and at least 3 s, each its own job, cross-faded over 8 frames (15.5 s in 5 s parts:
// no visible join, the PC at 86-94 GB; 15 s jobs had taken ComfyUI to 110 GB, acceptable on a 128 GB PC).
struct ScriptRun {
    std::atomic<bool> running{false}, finished{false};
    std::wstring log;
    std::string line; // the script's last line: the step it is on, then "ok" or "error: ..."
    double readAt = 0;
};
std::string FileKey(const std::wstring& path); // (below)
std::wstring HashName(const std::string& key);

const char* const kCrispUpscaler = "minimax_h3_latent_upscaler_3d_conv_v1_bf16.safetensors";
const double kCrispSetupGb = 13.7; // the two downloads (12.5 + 1.2 GB); the LoRA they make is 10 GB
ScriptRun g_crispSetup;

bool CrispNode() { // the latent upscaler's nodes, in ComfyUI Desktop's custom_nodes
    const std::wstring install = ComfyInstallDir();
    std::error_code ec;
    return !install.empty() && fs::exists(fs::path(install) / L"ComfyUI\\custom_nodes\\Comfyui_Minimax_h3_latent_Upscaler\\__init__.py", ec);
}
bool CrispFiles() { // the FL2VA base and the upscaler's model on disk, the LoRA listed
    const std::wstring models = ComfyModelsDir(), install = ComfyInstallDir();
    if (models.empty()) return false;
    std::error_code ec;
    const fs::path m(models);
    const bool up = fs::exists(m / L"latent_upscale_models" / kCrispUpscaler, ec) ||
                    (!install.empty() && fs::exists(fs::path(install) / L"ComfyUI\\models\\latent_upscale_models" / kCrispUpscaler, ec));
    return up && fs::exists(m / L"diffusion_models" / kUnet, ec) && HasLora(kCrispLora);
}
bool CrispDownloading() { return g_crispSetup.running; }
bool CrispReady() { return !g_crispSetup.running && CrispNode() && CrispFiles(); }
// Veda for it: the switch on, and ComfyUI said it has the predictor - or is not running and had it last time (a raw job has no DropMissingVeda)
bool CrispVeda() { return g_v.veda && (VedaReady(kVedaPredictor) || (!g_v.vedaKnown && g_v.vedaWasReady)); }
double CrispMissingGb() { return HasLora(kCrispLora) ? 0.7 : kCrispSetupGb; }

// The set-up (the user's click): tools\h3turbo\setup_open_h3.ps1 downloads, checks and converts the LoRA (its downloads on the data folder's drive,
// not C:), and the latent upscaler's nodes and model when they are missing
void StartCrispSetup(AppState& app) {
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    const fs::path script = fs::path(buf).parent_path() / L"tools" / L"h3turbo" / L"setup_open_h3.ps1";
    const std::wstring models = ComfyModelsDir(), install = ComfyInstallDir();
    std::error_code ec;
    if (g_crispSetup.running) return;
    if (models.empty() || install.empty() || !fs::exists(script, ec)) {
        g_crispSetup.line = models.empty() || install.empty() ? "error: ComfyUI Desktop was not found" : "error: tools\\h3turbo\\setup_open_h3.ps1 is missing";
        app.SetStatus("Make it crisp: " + g_crispSetup.line.substr(7));
        return;
    }
    fs::create_directories(DataDir() + L"\\setup_tmp", ec);
    g_crispSetup.log = DataDir() + L"\\open_h3_setup.log";
    fs::remove(g_crispSetup.log, ec);
    g_crispSetup.line = "starting";
    g_crispSetup.finished = false;
    g_crispSetup.running = true;
    const std::wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + script.wstring() + L"\" -Models \"" + models + L"\" -Install \"" + install +
                             L"\" -Temp \"" + DataDir() + L"\\setup_tmp\" -Log \"" + g_crispSetup.log + L"\"";
    std::thread([cmd]() {
        std::string out;
        RunCaptureHidden(cmd, out, true);
        g_crispSetup.running = false;
        g_crispSetup.finished = true;
    }).detach();
}

const std::string& CrispSetupLine() {
    if (!g_crispSetup.log.empty() && ImGui::GetTime() - g_crispSetup.readAt > 0.5) {
        g_crispSetup.readAt = ImGui::GetTime();
        std::ifstream f(g_crispSetup.log);
        std::string l, last;
        while (std::getline(f, l)) {
            if (l.rfind("\xEF\xBB\xBF", 0) == 0) l = l.substr(3);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            if (!l.empty()) last = l;
        }
        if (!last.empty()) g_crispSetup.line = last;
    }
    return g_crispSetup.line;
}

long long GpuTotalMiB() {
    static long long total = -2;
    if (total == -2) {
        std::string out;
        RunCaptureHidden(L"nvidia-smi --query-gpu=memory.total --format=csv,noheader,nounits", out, true);
        total = out.empty() || !isdigit(static_cast<unsigned char>(out[0])) ? -1 : std::atoll(out.c_str());
    }
    return total;
}

// H3's own size for the draft (as ComfyUI's adapt_canvas): a 768 short side, at most 768 x 1344 of area, multiples of 32
void CrispCanvas(int srcW, int srcH, int& w, int& h) {
    const double ratio = static_cast<double>(std::max(srcW, 16)) / std::max(srcH, 16);
    double nw = ratio >= 1.0 ? 768.0 * ratio : 768.0, nh = ratio >= 1.0 ? 768.0 : 768.0 / ratio;
    if (nw * nh > 768.0 * 1344.0) {
        const double s = std::sqrt(768.0 * 1344.0 / (nw * nh));
        nw *= s, nh *= s;
    }
    w = std::max(32, static_cast<int>(std::lround(nw / 32.0)) * 32);
    h = std::max(32, static_cast<int>(std::lround(nh / 32.0)) * 32);
}

// The size it is made at: the short side about `shortSide` (multiples of 32: 1080p is 1088), the long side by the video's shape; never under
// the draft's size (the latent upscaler only enlarges)
void CrispSize(int srcW, int srcH, int shortSide, int& w, int& h) {
    srcW = std::max(srcW, 16), srcH = std::max(srcH, 16);
    const double aspect = static_cast<double>(std::max(srcW, srcH)) / std::min(srcW, srcH);
    const int s = (shortSide + 31) / 32 * 32, l = std::max(s, static_cast<int>(std::lround(s * aspect / 32.0)) * 32);
    w = srcW >= srcH ? l : s, h = srcW >= srcH ? s : l;
    int cw = 0, ch = 0;
    CrispCanvas(srcW, srcH, cw, ch);
    w = std::max(w, cw), h = std::max(h, ch);
}

const int kCrispPartFrames = 192; // a job's frames: 8 s (17k + 5); longer videos go in parts
int CrispAligned(int frames) { // H3's frame grid: 17k + 5
    int n = std::max(5, frames);
    while (n % 17 != 5) ++n;
    return n;
}

// One job: the prepared clip (already at the draft size and padded to the grid) -> 2 steps at w x h -> the first `frames` frames, the clip's
// sound and frame rate
json BuildCrisp(int w, int h, int frames, bool veda, const char* prefix) {
    json wf;
    auto node = [&](const char* id, const char* cls, json inputs) { wf[id] = {{"class_type", cls}, {"inputs", std::move(inputs)}, {"_meta", {{"title", cls}}}}; };
    auto L = [](const char* id, int slot = 0) { return json::array({id, slot}); };
    const int aligned = CrispAligned(frames);
    node("1", "UNETLoader", {{"unet_name", kUnet}, {"weight_dtype", "default"}});
    node("2", "LoraLoaderModelOnly", {{"model", L("1")}, {"lora_name", kCrispLora}, {"strength_model", 1.0}});
    json model = L("2");
    if (veda) {
        node("3", "VedaSparseAttention", VedaNode(model, kVedaPredictor));
        model = L("3");
    }
    node("4", "CLIPLoader", {{"clip_name", kTextEncoder}, {"type", "minimax"}, {"device", "default"}});
    node("5", "VAELoader", {{"vae_name", kVideoVae}});
    node("6", "LoadVideo", {{"file", "@clip"}});
    node("7", "GetVideoComponents", {{"video", L("6")}});
    node("8", "ImageFromBatch", {{"image", L("7")}, {"batch_index", 0}, {"length", aligned}});
    node("9", "VAEEncode", {{"pixels", L("8")}, {"vae", L("5")}});
    node("10", "MinimaxH3LatentUpscaler3D", {{"latent", L("9")}, {"model_name", kCrispUpscaler}, {"mode", "target dimensions"}, {"mode.width", w}, {"mode.height", h},
                                             {"align", 32}, {"enable_temporal_chunking", true}, {"force_unload", true}, {"device", "cuda"}, {"precision", "bf16"}});
    node("11", "MiniMaxH3ImageToVideo", {{"clip", L("4")}, {"vae", L("5")}, {"prompt", ""}, {"width", w}, {"height", h}, {"length", aligned}}); // (the empty AV latent's sound half)
    node("12", "LTXVSeparateAVLatent", {{"av_latent", L("11", 1)}});
    node("13", "LTXVConcatAVLatent", {{"video_latent", L("10")}, {"audio_latent", L("12", 1)}});
    node("14", "RandomNoise", {{"noise_seed", 7}});
    node("15", "KSamplerSelect", {{"sampler_name", "euler"}});
    node("16", "ManualSigmas", {{"sigmas", "0.7, 0.494118, 0.0"}}); // (Open-H3's 2-step grid: the shift-6 rule from 0.7)
    if (aligned > 124) { // in 124-frame chunks, the next one anchored on the last 22 frames of the one before
        node("17", "MMH3TemporalSplitParamsV10", {{"chunk_frames", 124}, {"temporal_overlap_frames", 22}, {"anchor_strength", 0.999}, {"motion_anchor_frames", "22"},
                                                  {"identity_anchor_frames", 0}}); // (identity anchors would pin the soft draft back in)
        node("18", "MMH3SplitUpscale", {{"model", model}, {"conditioning", L("11")}, {"latent", L("13")}, {"noise", L("14")}, {"sampler", L("15")}, {"sigmas", L("16")},
                                        {"cfg", 1.0}, {"seam_polish", "off"}, {"color_match", true}, {"temporal_split_param", L("17")}});
    } else {
        node("19", "BasicGuider", {{"model", model}, {"conditioning", L("11")}});
        node("18", "SamplerCustomAdvanced", {{"noise", L("14")}, {"guider", L("19")}, {"sampler", L("15")}, {"sigmas", L("16")}, {"latent_image", L("13")}});
    }
    node("20", "LTXVSeparateAVLatent", {{"av_latent", L("18")}});
    node("21", "VAEDecodeTiled", {{"samples", L("20")}, {"vae", L("5")}, {"tile_size", 768}, {"overlap", 64}, {"temporal_size", 64}, {"temporal_overlap", 8}});
    json images = L("21");
    if (aligned != frames) {
        node("22", "ImageFromBatch", {{"image", L("21")}, {"batch_index", 0}, {"length", frames}});
        images = L("22");
    }
    node("23", "CreateVideo", {{"images", images}, {"audio", L("7", 1)}, {"fps", L("7", 2)}});
    node("24", "SaveVideo", {{"video", L("23")}, {"filename_prefix", prefix}, {"format", "auto"}, {"format.codec", "auto"}});
    return wf;
}

// A video being made crisp: prepared on a worker thread (probed, cut into parts when it is long, each part scaled to the draft size and padded to
// H3's frame grid by ffmpeg), sent from the UI thread, and - in parts - joined once every part is back
struct CrispPart {
    std::wstring clip;   // the prepared clip that is uploaded
    int frames = 0;      // its real frames (before the padding)
    int activity = 0;    // its job
    std::wstring result; // what came back
};
struct CrispRun {
    std::wstring video, label;
    int w = 0, h = 0, overlap = 0;
    double fps = 24.0;
    std::vector<CrispPart> parts;
    bool sent = false, joining = false;
};
std::mutex g_crispMutex;
std::vector<CrispRun> g_crispRuns; // prepared or being made (under g_crispMutex)
std::vector<std::string> g_crispErrors;
std::vector<std::wstring> g_crispJoined;
int g_crispPreparing = 0;

std::wstring CrispWorkDir() { return DataDir() + L"\\h3\\crisp"; }

void PrepareCrisp(std::wstring video, int shortSide) {
    auto fail = [&](const std::string& why) {
        std::lock_guard<std::mutex> lock(g_crispMutex);
        g_crispErrors.push_back(Utf8(fs::path(video).filename().wstring()) + ": " + why);
        --g_crispPreparing;
    };
    const std::wstring ff = FfmpegPath();
    if (ff.empty()) return fail("ffmpeg was not found");
    VideoInfo vi = ProbeVideoFfmpeg(video);
    if (vi.width <= 0 || vi.seconds <= 0) return fail("could not read the video");
    const double fps = vi.fps > 1.0 ? vi.fps : 24.0;
    int total = 0;
    { // the exact frame count (the container's length can be a frame off)
        std::string out;
        const std::wstring probe = fs::path(ff).parent_path() / L"ffprobe.exe";
        std::error_code ec;
        if (fs::exists(probe, ec))
            RunCaptureHidden(L"\"" + probe + L"\" -v error -select_streams v:0 -count_packets -show_entries stream=nb_read_packets -of csv=p=0 \"" + video + L"\"", out, true);
        total = std::atoi(out.c_str());
        if (total <= 0) total = static_cast<int>(std::lround(vi.seconds * fps));
    }
    CrispRun run;
    run.video = video;
    run.fps = fps;
    CrispSize(vi.width, vi.height, shortSide, run.w, run.h);
    int cw = 0, ch = 0;
    CrispCanvas(vi.width, vi.height, cw, ch);
    // up to 8 s in one job; longer: parts of 8 s filled in order, 8 frames shared with the next for the cross-fade, and a last part under 3 s
    // made 3 s from the one before (9 s: 6 + 3, 17 s: 8 + 6 + 3)
    std::vector<std::pair<int, int>> cuts; // start frame, frames
    if (total <= kCrispPartFrames) cuts.push_back({0, total});
    else {
        const int part = kCrispPartFrames, overlap = 8;
        run.overlap = overlap;
        for (int start = 0;;) {
            if (total - start <= part) {
                cuts.push_back({start, total - start});
                break;
            }
            cuts.push_back({start, part});
            start += part - overlap;
        }
        const int shortest = static_cast<int>(std::lround(3.0 * fps));
        if (cuts.size() >= 2 && cuts.back().second < shortest) {
            auto& a = cuts[cuts.size() - 2];
            auto& b = cuts.back();
            const int end = b.first + b.second;
            b.first = end - shortest;
            b.second = shortest;
            a.second = b.first + overlap - a.first;
        }
    }
    std::error_code ec;
    fs::create_directories(CrispWorkDir(), ec);
    for (const auto& e : fs::directory_iterator(CrispWorkDir(), ec)) { // last runs' clips (a day old)
        std::error_code ec2;
        if (e.is_regular_file(ec2) && fs::last_write_time(e, ec2) < fs::file_time_type::clock::now() - std::chrono::hours(24)) fs::remove(e, ec2);
    }
    const std::wstring stem = HashName(FileKey(video) + std::to_string(shortSide));
    for (size_t k = 0; k < cuts.size(); ++k) {
        const int start = cuts[k].first, len = cuts[k].second;
        CrispPart p;
        p.frames = len;
        p.clip = CrispWorkDir() + L"\\" + stem + L"_" + std::to_wstring(k) + L".mp4";
        wchar_t vf[512], af[160];
        // scaled to cover the draft size and centre-cropped, then the last frame held to H3's grid
        swprintf(vf, 512, L"trim=start_frame=%d:end_frame=%d,setpts=PTS-STARTPTS,scale=%d:%d:force_original_aspect_ratio=increase:flags=lanczos,crop=%d:%d,"
                 L"tpad=stop_mode=clone:stop=%d", start, start + len, cw, ch, cw, ch, CrispAligned(len) - len);
        swprintf(af, 160, L"atrim=start=%.4f:end=%.4f,asetpts=PTS-STARTPTS", start / fps, (start + len) / fps);
        std::string log;
        fs::remove(p.clip, ec);
        RunCaptureIdle(L"\"" + ff + L"\" -y -v error -i \"" + video + L"\" -vf \"" + vf + L"\" -af \"" + af + L"\" -c:v libx264 -preset veryfast -crf 12 -pix_fmt yuv420p "
                       L"-c:a aac -b:a 192k \"" + p.clip + L"\"", log, true);
        if (!fs::exists(p.clip, ec) || fs::file_size(p.clip, ec) < 1000) { // (a video with no sound: the same without it)
            RunCaptureIdle(L"\"" + ff + L"\" -y -v error -i \"" + video + L"\" -vf \"" + vf + L"\" -an -c:v libx264 -preset veryfast -crf 12 -pix_fmt yuv420p \"" + p.clip + L"\"", log, true);
        }
        if (!fs::exists(p.clip, ec) || fs::file_size(p.clip, ec) < 1000) return fail("ffmpeg could not prepare it: " + log.substr(0, 200));
        run.parts.push_back(p);
    }
    std::lock_guard<std::mutex> lock(g_crispMutex);
    g_crispRuns.push_back(std::move(run));
    --g_crispPreparing;
}

bool MakeCrisp(AppState& app, const std::wstring& video) {
    if (video.empty()) return false;
    if (!CrispReady()) {
        app.SetStatus("Make it crisp needs Open-H3 first (Set up Make it crisp, under the player)");
        return false;
    }
    if (ComfyHasNode(app.comfy.url, "MinimaxH3LatentUpscaler3D") == 0) {
        app.SetStatus("ComfyUI has not loaded the H3 latent upscaler yet: restart ComfyUI once (it was just set up), then Make it crisp");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_crispMutex);
        ++g_crispPreparing;
    }
    std::thread(PrepareCrisp, video, g_v.crispTo).detach();
    app.SetStatus("Make it crisp: preparing " + Utf8(fs::path(video).filename().wstring()) + "...");
    return true;
}

// Once a frame: prepared runs are sent, parts that came back are collected, long ones joined (a worker thread: ffmpeg cross-fades the parts and
// puts the whole video's own sound under them)
void PumpCrisp(AppState& app) {
    std::lock_guard<std::mutex> lock(g_crispMutex);
    for (const std::string& e : g_crispErrors) Toast(ToastKind::Error, "Could not make it crisp", e, {}, [](AppState& s) { s.requestTab = 10; }, "crisp");
    g_crispErrors.clear();
    for (const std::wstring& made : g_crispJoined) {
        Toast(ToastKind::Info, "The crisp video is ready", Utf8(fs::path(made).filename().wstring()) + ": its parts joined", {}, [](AppState& s) { s.requestTab = 10; }, "crisp");
        PlayInVideoPage(app, made, false);
    }
    g_crispJoined.clear();
    const bool veda = CrispVeda();
    const std::vector<ComfyActivity> acts = app.comfySvc ? app.comfySvc->Activity() : std::vector<ComfyActivity>();
    for (CrispRun& r : g_crispRuns) {
        const std::string name = Utf8(fs::path(r.video).filename().wstring());
        if (!r.sent) {
            r.sent = true;
            for (size_t k = 0; k < r.parts.size(); ++k) {
                ComfyJob job;
                job.apiWorkflow = BuildCrisp(r.w, r.h, r.parts[k].frames, veda, r.parts.size() > 1 ? "video/CrispPart" : "video/Crisp").dump();
                job.rawUploads = {{"@clip", r.parts[k].clip}};
                job.label = "Crisp (" + std::to_string(r.w) + " x " + std::to_string(r.h) + (veda ? ", Veda" : "") +
                            (r.parts.size() > 1 ? ", part " + std::to_string(k + 1) + " of " + std::to_string(r.parts.size()) : std::string()) + "): " + name;
                job.times = 1;
                r.parts[k].activity = SendComfyJob(app, std::move(job));
                if (!r.parts[k].activity) r.parts.clear(); // (it said why)
            }
            if (!r.parts.empty())
                Toast(ToastKind::Info, "Making it crisp", name + " at " + std::to_string(r.w) + " x " + std::to_string(r.h) + " (Open-H3)" +
                      (r.parts.size() > 1 ? ", in " + std::to_string(r.parts.size()) + " parts" : std::string()), {}, [](AppState& s) { s.requestTab = 10; }, "crisp");
            continue;
        }
        if (r.parts.size() < 2 || r.joining) continue;
        bool failed = false, all = true;
        for (CrispPart& p : r.parts) {
            for (const ComfyActivity& a : acts) {
                if (a.id != p.activity) continue;
                if (a.state == ComfyState::Failed) failed = true;
                if (a.state == ComfyState::Done && p.result.empty()) {
                    for (const ComfyResult& f : a.files) {
                        if (!f.localPath.empty()) p.result = f.localPath;
                    }
                }
            }
            all = all && !p.result.empty();
        }
        if (failed) {
            Toast(ToastKind::Error, "Could not make it crisp", name + ": a part failed (its error is in the ComfyUI list)", {}, [](AppState& s) { s.requestTab = 10; }, "crisp");
            r.joining = true; // (dropped below)
            continue;
        }
        if (!all) continue;
        r.joining = true;
        std::thread([r]() {
            const std::wstring ff = FfmpegPath();
            const fs::path first = r.parts.front().result;
            const std::wstring out = (first.parent_path() / (L"Crisp_" + fs::path(r.video).stem().wstring() + L".mp4")).wstring();
            std::wstring cmd = L"\"" + ff + L"\" -y -v error";
            for (const CrispPart& p : r.parts) cmd += L" -i \"" + p.result + L"\"";
            cmd += L" -i \"" + r.video + L"\"";
            std::string fc;
            double at = 0;
            const double fade = r.overlap / r.fps;
            std::string prev = "[0:v]";
            for (size_t k = 1; k < r.parts.size(); ++k) {
                at += r.parts[k - 1].frames / r.fps - fade;
                char b[200];
                snprintf(b, sizeof(b), "%s[%d:v]xfade=transition=fade:duration=%.4f:offset=%.4f[x%d];", prev.c_str(), static_cast<int>(k), fade, at, static_cast<int>(k));
                fc += b;
                prev = "[x" + std::to_string(k) + "]";
            }
            fc.pop_back();
            cmd += L" -filter_complex \"" + WideOf(fc) + L"\" -map \"" + WideOf(prev) + L"\" -map \"" + std::to_wstring(r.parts.size()) +
                   L":a:0?\" -c:v libx264 -preset medium -crf 16 -pix_fmt yuv420p -c:a aac -b:a 192k -shortest \"" + out + L"\"";
            std::string log;
            RunCaptureHidden(cmd, log, true);
            std::error_code ec;
            std::lock_guard<std::mutex> lock2(g_crispMutex);
            if (!fs::exists(out, ec) || fs::file_size(out, ec) < 1000) {
                g_crispErrors.push_back(Utf8(fs::path(r.video).filename().wstring()) + ": ffmpeg could not join the parts: " + log.substr(0, 200));
                return;
            }
            std::vector<std::wstring> parts;
            for (const CrispPart& p : r.parts) parts.push_back(p.result);
            std::wstring err;
            DeleteFilesToRecycleBin(nullptr, parts, &err); // (the parts, now that they are one video)
            g_crispJoined.push_back(out);
        }).detach();
    }
    g_crispRuns.erase(std::remove_if(g_crispRuns.begin(), g_crispRuns.end(), [](const CrispRun& r) { return r.parts.empty() || (r.sent && (r.parts.size() < 2 || r.joining)); }),
                      g_crispRuns.end());
}

// Quick upscale: Lanczos to the chosen size and a light sharpen, encoded on the GPU (NVENC) when it can be - no AI, no detail redrawn,
// seconds instead of minutes (ai honobono's "FAST" against SeedVR2's "QUALITY" on an RTX 3060 Ti: FHD in about 15 s against 14 min 38 s, 4K
// in 51 s against 53 min 50 s). Into comfy_results as Quick<size>_<name>.mp4, then shown in the player.
struct QuickUp {
    std::future<std::wstring> fut;
    std::string error;
};
QuickUp g_quick;

std::wstring QuickUpscale(std::wstring video, int shortSide, std::string* error) {
    const std::wstring ff = FfmpegPath();
    if (ff.empty()) {
        *error = "ffmpeg was not found";
        return {};
    }
    std::error_code ec;
    const std::wstring dir = DataDir() + L"\\comfy_results";
    fs::create_directories(dir, ec);
    const std::wstring out = dir + L"\\Quick" + std::to_wstring(shortSide) + L"_" + fs::path(video).stem().wstring() + L".mp4";
    wchar_t vf[256];
    swprintf(vf, 256, L"scale='if(gt(iw,ih),-2,%d)':'if(gt(iw,ih),%d,-2)':flags=lanczos,unsharp=5:5:0.6:5:5:0.0", shortSide, shortSide);
    std::string log;
    // (NVENC's H.264 stops at 4096 px a side - a 9:16 4K video is 2160 x 3840 and more: then x264, which took the 2160 x 4800 one)
    for (const wchar_t* enc : {L"-c:v h264_nvenc -preset p5 -rc vbr -cq 18 -b:v 0", L"-c:v libx264 -preset medium -crf 17"}) {
        fs::remove(out, ec);
        RunCaptureIdle(L"\"" + ff + L"\" -y -v error -i \"" + video + L"\" -vf \"" + vf + L"\" " + enc + L" -pix_fmt yuv420p -c:a copy \"" + out + L"\"", log, true);
        if (fs::exists(out, ec) && ProbeVideo(out).seconds > 0) return out; // (a failed try can leave a file with no video in it)
    }
    fs::remove(out, ec);
    *error = log.empty() ? std::string("ffmpeg made nothing") : log.substr(0, 200);
    return {};
}

// Under the player: Make it crisp, its size and model, or what it takes to get it
void DrawCrispRow(AppState& app, const std::wstring& video) {
    ImGui::PushID("crisp");
    const char* about = "Open-H3-Regenerate (NVlabs) redraws the video's fine detail - skin, hair, eyes, fabric - at 1080p or 2K with MiniMax H3 "
                        "itself, in 2 steps; the motion, the faces and the sound are kept.";
    if (CrispDownloading()) {
        ImGui::TextColored(ui::kAccentHover, "Setting up Make it crisp: %s", CrispSetupLine().c_str());
    } else if (!CrispReady()) {
        const std::string& line = CrispSetupLine();
        const bool failed = g_crispSetup.finished && line.rfind("error", 0) == 0;
        char label[96];
        snprintf(label, sizeof(label), "%s (%.1f GB)", failed ? "Set up Make it crisp (failed: retry)" : "Set up Make it crisp", CrispMissingGb());
        if (ImGui::SmallButton(label)) StartCrispSetup(app);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("%s\nDownloads Open-H3-Regenerate-2K's LoRA (Efficient-Large-Model, 13.7 GB, checked) and turns it into a 10 GB ComfyUI LoRA "
                              "in models\\loras\\MiniMax H3; adds the MiniMax H3 latent upscaler (LBH-123-AI) when ComfyUI does not have it. Needs the "
                              "FL2VA model that Fast uses.%s%s",
                              about, failed ? "\n\nLast time: " : "", failed ? line.c_str() : "");
    } else {
        ImGui::BeginDisabled(video.empty());
        if (ImGui::SmallButton("Make it crisp")) MakeCrisp(app, video);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s\nA new video (Crisp_...) next to this one.\n\nMeasured on an RTX 3090 with Veda: 5 s to 2560 x 1440 in about 3 min, "
                              "to 1920 x 1088 in under 2; about 3x that without Veda. Up to 8 s in one go; longer videos go in parts of 3 to 8 s "
                              "that cross-fade. 2K is the most it makes (4K is the quick upscale only).",
                              about);
        int pending = 0;
        {
            std::lock_guard<std::mutex> lock(g_crispMutex);
            pending = g_crispPreparing;
        }
        if (pending) ImGui::SameLine(), ImGui::TextDisabled("preparing...");
    }
    ImGui::SameLine();
    if (g_quick.fut.valid() && g_quick.fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const std::wstring made = g_quick.fut.get();
        if (made.empty()) app.SetStatus("Quick upscale failed: " + g_quick.error);
        else app.SetStatus("Quick upscale saved: " + Utf8(fs::path(made).filename().wstring())), PlayInVideoPage(app, made, false);
    }
    ImGui::BeginDisabled(video.empty() || g_quick.fut.valid());
    if (ImGui::SmallButton(g_quick.fut.valid() ? "Upscaling..." : "Quick upscale")) {
        g_quick.error.clear();
        g_quick.fut = std::async(std::launch::async, QuickUpscale, video, g_v.crispTo, &g_quick.error);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("To the size on the right in seconds: a clean resize (Lanczos) and a light sharpen, no AI - nothing is redrawn, so no new "
                          "detail, but nothing changes either. A new video (Quick..._) next to this one.\n\nMake it crisp redraws fine detail with "
                          "Open-H3 but takes minutes (about 3 min for 5 s at 2K on an RTX 3090).");
    // what it would cost, for the video in the player: its new size, how much bigger, and about how long
    if (g_vid.player && g_vid.player->IsOpen() && _wcsicmp(g_vid.player->Path().c_str(), video.c_str()) == 0 && g_vid.player->Width() > 0) {
        const double w = g_vid.player->Width(), h = g_vid.player->Height(), dur = std::max(1.0, g_vid.player->Duration());
        int ow = 0, oh = 0;
        CrispSize(static_cast<int>(w), static_cast<int>(h), std::min(g_v.crispTo, 1440), ow, oh);
        const double scale = std::min(ow, oh) / std::min(w, h);
        // (measured with Veda: 177 s for 124 frames at 3.7 MP, 99 s at 2.1 MP, a part of a long video a little less; about 3x without Veda)
        const double mp = static_cast<double>(ow) * oh / 1.0e6, frames = dur * 24.0;
        const double chunks = std::ceil(std::max(1.0, frames - 22.0) / 102.0); // (124-frame chunks overlapping by 22, about the same across parts)
        const double secs = (40.0 + chunks * 38.0 * mp) * (CrispVeda() ? 1.0 : 3.0);
        ImGui::SameLine();
        if (scale < 1.3)
            ImGui::TextColored(ui::kWarn, "only %.2fx bigger (%d x %d): little to gain, about %.0f min", scale, ow, oh, std::ceil(secs / 60.0));
        else
            ImGui::TextDisabled("%.1fx bigger, %d x %d, about %.0f min", scale, ow, oh, std::ceil(secs / 60.0));
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ui::Dp(84.0f));
    static const int sizes[] = {1080, 1440, 2160};
    char cur[16];
    snprintf(cur, sizeof(cur), "%dp", g_v.crispTo);
    if (ImGui::BeginCombo("##crispto", cur)) {
        for (int sz : sizes) {
            char l[16];
            snprintf(l, sizeof(l), "%dp", sz);
            if (ImGui::Selectable(l, g_v.crispTo == sz)) g_v.crispTo = sz, SaveSettings();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("The short side it is made at (1080p: a 9:16 phone video becomes 1080 x 1920). Make it crisp makes 2160p as 1440p (2K).");
    ImGui::PopID();
}


// ---- Presenter: TaoMate's 3-step LoRA, downloaded and converted by tools\h3turbo\setup_taomate.ps1 (the user's click starts it; ScriptRun is above)
ScriptRun g_taoSetup;

void StartPresenterSetup() {
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    const fs::path script = fs::path(buf).parent_path() / L"tools" / L"h3turbo" / L"setup_taomate.ps1";
    const std::wstring models = ComfyModelsDir(), install = ComfyInstallDir();
    std::error_code ec;
    if (g_taoSetup.running || models.empty() || !fs::exists(script, ec)) {
        g_taoSetup.line = models.empty() ? "error: ComfyUI's models folder was not found" : "error: tools\\h3turbo\\setup_taomate.ps1 is missing";
        return;
    }
    fs::create_directories(DataDir(), ec);
    g_taoSetup.log = DataDir() + L"\\taomate_setup.log";
    fs::remove(g_taoSetup.log, ec);
    g_taoSetup.line = "starting";
    g_taoSetup.running = true;
    std::wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + script.wstring() + L"\" -Models \"" + models + L"\" -Log \"" + g_taoSetup.log + L"\"";
    if (!install.empty()) cmd += L" -Python \"" + install + L"\\ComfyUI\\.venv\\Scripts\\python.exe\"";
    std::thread([cmd]() {
        std::string out;
        RunCaptureHidden(cmd, out, true);
        g_taoSetup.running = false;
        g_taoSetup.finished = true;
    }).detach();
}

const std::string& PresenterSetupLine() {
    if (!g_taoSetup.log.empty() && ImGui::GetTime() - g_taoSetup.readAt > 0.5) {
        g_taoSetup.readAt = ImGui::GetTime();
        std::ifstream f(g_taoSetup.log);
        std::string l, last;
        while (std::getline(f, l)) {
            if (l.rfind("\xEF\xBB\xBF", 0) == 0) l = l.substr(3);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            if (!l.empty()) last = l;
        }
        if (!last.empty()) g_taoSetup.line = last;
    }
    return g_taoSetup.line;
}

// ---- Veda, set up by the user's click: tools\veda\setup_veda.ps1 puts its node, triton-windows and the predictors into ComfyUI Desktop's
// install; ComfyUI then needs a restart to load them
struct VedaSetup {
    std::atomic<bool> running{false}, finished{false};
    std::wstring log;
    std::string line; // the script's last line: the step it is on, then "ok" or "error: ..."
    double readAt = 0;
    bool readLast = false; // read once more after it finished
};
VedaSetup g_vedaSetup;

std::wstring VedaScript() {
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    const fs::path f = fs::path(buf).parent_path() / L"tools" / L"veda" / L"setup_veda.ps1";
    std::error_code ec;
    return fs::exists(f, ec) ? f.wstring() : std::wstring();
}

void StartVedaSetup() {
    const std::wstring script = VedaScript(), install = ComfyInstallDir();
    if (g_vedaSetup.running || script.empty() || install.empty()) return;
    std::error_code ec;
    fs::create_directories(DataDir(), ec);
    g_vedaSetup.log = DataDir() + L"\\veda_setup.log";
    fs::remove(g_vedaSetup.log, ec);
    g_vedaSetup.line = "starting";
    g_vedaSetup.finished = false;
    g_vedaSetup.readLast = false;
    g_vedaSetup.running = true;
    std::wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + script + L"\" -Install \"" + install + L"\" -Log \"" + g_vedaSetup.log + L"\"";
    wchar_t extra[2048] = {}; // (for tests: -Nodes / -Predictors / -PyTarget folders of their own)
    if (GetEnvironmentVariableW(L"MV_VEDA_ARGS", extra, 2048)) cmd += L" " + std::wstring(extra);
    std::thread([cmd]() {
        std::string out;
        RunCaptureHidden(cmd, out, true);
        g_vedaSetup.finished = true;
        g_vedaSetup.running = false;
    }).detach();
}

// The setup's last line, read again twice a second while it runs
const std::string& VedaSetupLine() {
    if (!g_vedaSetup.log.empty() && (g_vedaSetup.running || !g_vedaSetup.readLast) && ImGui::GetTime() - g_vedaSetup.readAt > 0.5) {
        g_vedaSetup.readAt = ImGui::GetTime();
        g_vedaSetup.readLast = g_vedaSetup.finished;
        std::ifstream f(g_vedaSetup.log);
        std::string l, last;
        while (std::getline(f, l)) {
            if (!l.empty() && l.rfind("\xEF\xBB\xBF", 0) == 0) l = l.substr(3); // the BOM PowerShell writes
            if (!l.empty() && l.back() == '\r') l.pop_back();
            if (!l.empty()) last = l;
        }
        if (!last.empty()) g_vedaSetup.line = last;
    }
    return g_vedaSetup.line;
}

// ---- Fizgig H3 Tweaks, set up by the user's click (tools\fizgig\setup_fizgig.ps1: the node only, into ComfyUI Desktop's custom_nodes)
ScriptRun g_fizSetup;

void StartFizgigSetup() {
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    const fs::path script = fs::path(buf).parent_path() / L"tools" / L"fizgig" / L"setup_fizgig.ps1";
    const std::wstring install = ComfyInstallDir();
    std::error_code ec;
    if (g_fizSetup.running || install.empty() || !fs::exists(script, ec)) {
        g_fizSetup.line = install.empty() ? "error: ComfyUI Desktop was not found" : "error: tools\\fizgig\\setup_fizgig.ps1 is missing";
        return;
    }
    fs::create_directories(DataDir(), ec);
    g_fizSetup.log = DataDir() + L"\\fizgig_setup.log";
    fs::remove(g_fizSetup.log, ec);
    g_fizSetup.line = "starting";
    g_fizSetup.finished = false;
    g_fizSetup.running = true;
    std::wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + script.wstring() + L"\" -Install \"" + install + L"\" -Log \"" + g_fizSetup.log + L"\"";
    std::thread([cmd]() {
        std::string out;
        RunCaptureHidden(cmd, out, true);
        g_fizSetup.running = false;
        g_fizSetup.finished = true;
    }).detach();
}

const std::string& FizgigSetupLine() {
    if (!g_fizSetup.log.empty() && ImGui::GetTime() - g_fizSetup.readAt > 0.5) {
        g_fizSetup.readAt = ImGui::GetTime();
        std::ifstream f(g_fizSetup.log);
        std::string l, last;
        while (std::getline(f, l)) {
            if (l.rfind("\xEF\xBB\xBF", 0) == 0) l = l.substr(3);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            if (!l.empty()) last = l;
        }
        if (!last.empty()) g_fizSetup.line = last;
    }
    return g_fizSetup.line;
}

// "Detail & contrast" and "Prompt strength" (Fizgig H3 Tweaks) when ComfyUI has the node, else its one-click setup
void DrawTweaksRow(bool swap) {
    ImGui::PushID("tweaks");
    const char* about = "Fizgig H3 Tweaks (github.com/shootthesound/ComfyUI-Fizgig-H3-Tweaks, MIT): nudges what H3 draws while it samples, "
                        "no extra models and no extra time.";
    if (g_v.vedaKnown && g_v.vedaInfo.fizgig) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Detail:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ui::Dp(150.0f));
        if (ImGui::SliderFloat("##detail", &g_v.detail, -0.5f, 0.6f, g_v.detail == 0 ? "off" : "%+.2f")) g_v.detail = std::round(g_v.detail * 20.0f) / 20.0f;
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("%s\nDetail & contrast: above 0 crisper texture (skin, hair, fabric) and deeper local contrast, on the last steps; below 0 "
                              "softer and airier.\nOnly detail that is the same in every frame is added, so it does not shimmer. 0.15 is clean; "
                              "much higher can look overbaked. Right-click: back to 0.15.",
                              about);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) g_v.detail = 0.15f, SaveSettings();
        ImGui::SameLine();
        ImGui::TextDisabled(swap ? "Likeness:" : "Prompt:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ui::Dp(130.0f));
        if (ImGui::SliderFloat("##pstrength", &g_v.promptStrength, 0.0f, 1.0f, g_v.promptStrength == 0 ? "off" : "%+.2f")) g_v.promptStrength = std::round(g_v.promptStrength * 20.0f) / 20.0f;
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("%s\nPrompt strength: the fast (turbo) videos run without CFG, so nothing sets how closely they follow the words. Above 0 "
                              "what the prompt names comes through more strongly%s. Start around 0.2.",
                              about, swap ? " - for a swap, the new person's look as the prompt describes it" : "");
    } else if (g_fizSetup.running) {
        ImGui::TextColored(ui::kAccentHover, "Setting up Fizgig H3 Tweaks: %s", FizgigSetupLine().c_str());
    } else if (g_fizSetup.finished && FizgigSetupLine() == "ok") {
        if (ImGui::GetTime() - g_v.vedaAsked > 10.0) g_v.vedaAsked = -1000.0; // asked again every 10 s until ComfyUI has loaded it
        ImGui::TextColored(ui::kWarn, "Fizgig H3 Tweaks is set up: restart ComfyUI to turn it on.");
    } else if (g_v.vedaKnown && !ComfyInstallDir().empty()) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Sharper detail:");
        ImGui::SameLine();
        const bool failed = g_fizSetup.finished;
        if (ImGui::SmallButton(failed ? "Set up Fizgig H3 Tweaks (failed: retry)" : "Set up Fizgig H3 Tweaks (tiny, no models)")) StartFizgigSetup();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("%s\nAdds a Detail & contrast dial (crisper skin, hair and fabric) and a Prompt strength dial.\nInstalls one custom node into "
                              "ComfyUI Desktop (about 30 KB); then ComfyUI restarts.%s",
                              about, failed ? ("\n\nLast time: " + FizgigSetupLine()).c_str() : "");
    }
    ImGui::PopID();
}

// ---- H3 Continuum (videos over 15 s), set up by the user's click (tools\continuum\setup_continuum.ps1: the node pack only, the published release)
ScriptRun g_contSetup;

// Its node pack in ComfyUI Desktop's custom_nodes (looked at every few seconds): there even before ComfyUI has been started or restarted
bool ContinuumOnDisk() {
    static double at = -100.0;
    static bool there = false;
    if (ImGui::GetTime() - at > 3.0) {
        at = ImGui::GetTime();
        const std::wstring install = ComfyInstallDir();
        std::error_code ec;
        there = !install.empty() && fs::exists(fs::path(install) / L"ComfyUI" / L"custom_nodes" / L"ComfyUI-H3-Continuum" / L"nodes.py", ec);
    }
    return there;
}

// ComfyUI has it, or (not asked yet: ComfyUI is not running, and it is started with what is installed) it is installed
bool LongReady() { return g_v.vedaKnown ? g_v.vedaInfo.continuum : (g_v.continuumWas || ContinuumOnDisk()); }


void StartContinuumSetup() {
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    const fs::path script = fs::path(buf).parent_path() / L"tools" / L"continuum" / L"setup_continuum.ps1";
    const std::wstring install = ComfyInstallDir();
    std::error_code ec;
    if (g_contSetup.running || install.empty() || !fs::exists(script, ec)) {
        g_contSetup.line = install.empty() ? "error: ComfyUI Desktop was not found" : "error: tools\\continuum\\setup_continuum.ps1 is missing";
        return;
    }
    fs::create_directories(DataDir(), ec);
    g_contSetup.log = DataDir() + L"\\continuum_setup.log";
    fs::remove(g_contSetup.log, ec);
    g_contSetup.line = "starting";
    g_contSetup.finished = false;
    g_contSetup.running = true;
    std::wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + script.wstring() + L"\" -Install \"" + install + L"\" -Log \"" + g_contSetup.log + L"\"";
    std::thread([cmd]() {
        std::string out;
        RunCaptureHidden(cmd, out, true);
        g_contSetup.running = false;
        g_contSetup.finished = true;
    }).detach();
}

const std::string& ContinuumSetupLine() {
    if (!g_contSetup.log.empty() && ImGui::GetTime() - g_contSetup.readAt > 0.5) {
        g_contSetup.readAt = ImGui::GetTime();
        std::ifstream f(g_contSetup.log);
        std::string l, last;
        while (std::getline(f, l)) {
            if (l.rfind("\xEF\xBB\xBF", 0) == 0) l = l.substr(3);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            if (!l.empty()) last = l;
        }
        if (!last.empty()) g_contSetup.line = last;
    }
    return g_contSetup.line;
}

// Under Length: what it takes to make videos over 15 s, when ComfyUI does not have H3 Continuum yet
void DrawContinuumRow() {
    if (LongReady()) return;
    ImGui::PushID("continuum");
    const char* about = "H3 Continuum (github.com/ukr8b3g-cmyk/ComfyUI-H3-Continuum, MIT): makes MiniMax H3 videos longer than 15 s in parts that "
                        "continue each other, picture and sound, as one continuous shot.";
    if (g_contSetup.running) {
        ImGui::TextColored(ui::kAccentHover, "Setting up H3 Continuum: %s", ContinuumSetupLine().c_str());
    } else if ((g_contSetup.finished && ContinuumSetupLine() == "ok") || ContinuumOnDisk()) { // installed, but the running ComfyUI started without it
        if (ImGui::GetTime() - g_v.vedaAsked > 10.0) g_v.vedaAsked = -1000.0; // asked again every 10 s until ComfyUI has loaded it
        ImGui::TextColored(ui::kWarn, "H3 Continuum is set up: restart ComfyUI, then the length goes up to a minute.");
    } else if (!ComfyInstallDir().empty()) { // (ComfyUI need not be running to install it)
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Longer than 15 s:");
        ImGui::SameLine();
        const bool failed = g_contSetup.finished;
        if (ImGui::SmallButton(failed ? "Set up H3 Continuum (failed: retry)" : "Set up H3 Continuum (20 MB, no models)")) StartContinuumSetup();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("%s\nInstalls its node pack (release 3.9.8, checked against its published SHA-256) into ComfyUI Desktop; then ComfyUI "
                              "restarts.\nIt uses the H3 model you already have. Tested: the joins are as smooth as the rest, and the sound runs "
                              "straight through them.%s",
                              about, failed ? ("\n\nLast time: " + ContinuumSetupLine()).c_str() : "");
    }
    ImGui::PopID();
}

// ---- Person masking for Swap (MaskVidExperiments), set up by the user's click (tools\maskvid\setup_maskvid.ps1: the node pack only)
ScriptRun g_maskSetup;

void StartMaskSetup() {
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    const fs::path script = fs::path(buf).parent_path() / L"tools" / L"maskvid" / L"setup_maskvid.ps1";
    const std::wstring install = ComfyInstallDir();
    std::error_code ec;
    if (g_maskSetup.running || install.empty() || !fs::exists(script, ec)) {
        g_maskSetup.line = install.empty() ? "error: ComfyUI Desktop was not found" : "error: tools\\maskvid\\setup_maskvid.ps1 is missing";
        return;
    }
    fs::create_directories(DataDir(), ec);
    g_maskSetup.log = DataDir() + L"\\maskvid_setup.log";
    fs::remove(g_maskSetup.log, ec);
    g_maskSetup.line = "starting";
    g_maskSetup.finished = false;
    g_maskSetup.running = true;
    std::wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + script.wstring() + L"\" -Install \"" + install + L"\" -Log \"" + g_maskSetup.log + L"\"";
    std::thread([cmd]() {
        std::string out;
        RunCaptureHidden(cmd, out, true);
        g_maskSetup.running = false;
        g_maskSetup.finished = true;
    }).detach();
}

const std::string& MaskSetupLine() {
    if (!g_maskSetup.log.empty() && ImGui::GetTime() - g_maskSetup.readAt > 0.5) {
        g_maskSetup.readAt = ImGui::GetTime();
        std::ifstream f(g_maskSetup.log);
        std::string l, last;
        while (std::getline(f, l)) {
            if (l.rfind("\xEF\xBB\xBF", 0) == 0) l = l.substr(3);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            if (!l.empty()) last = l;
        }
        if (!last.empty()) g_maskSetup.line = last;
    }
    return g_maskSetup.line;
}

// Swap the person: "Only change the person" when ComfyUI can mask, else what it takes
void DrawMaskRow() {
    ImGui::PushID("swapmask");
    const char* about = "Person masking: SAM 3 finds the person in every frame and only that part of the clip is made again - the room, the other "
                        "people and the camera stay the clip's own pixels. Tested against the usual swap: the background stayed the clip's "
                        "(35-39 dB against 22), the new body followed the moves more closely, the likeness was about the same.";
    if (g_v.vedaKnown && g_v.vedaInfo.maskvid && !g_v.vedaInfo.sam3.empty()) {
        if (ImGui::Checkbox("Only change the person (keep everything else exactly)", &g_v.swapMask)) SaveSettings();
        if (g_v.swapMask) {
            if (ImGui::Checkbox("Two or more people: one at a time (never mixes them up)", &g_v.oneAtATime)) SaveSettings();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("Each new person gets a pass of their own: their picture, their masks, their shots; the next person is swapped into "
                                  "that video. A render for each person.\n\nTested: swapping two at once put the wrong person in some shots in most "
                                  "takes (Ava in the boy's close-up); one at a time put the right one in every shot, and kept the faces' expressions.");
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("%s\n\nA part with hard cuts is swapped the usual way: the mask cannot follow people across a cut (tested: someone who first\n"
                              "appears after a cut stayed the original).",
                              about);
    } else if (g_maskSetup.running) {
        ImGui::TextColored(ui::kAccentHover, "Setting up person masking: %s", MaskSetupLine().c_str());
    } else if ((g_maskSetup.finished && MaskSetupLine() == "ok") || (g_v.vedaKnown && g_v.vedaInfo.maskvid && g_v.vedaInfo.sam3.empty())) {
        if (ImGui::GetTime() - g_v.vedaAsked > 10.0) g_v.vedaAsked = -1000.0;
        ImGui::TextColored(ui::kWarn, g_v.vedaInfo.maskvid ? "Person masking needs a SAM 3 checkpoint in ComfyUI's checkpoints (sam3.1_multiplex_fp16)."
                                                          : "Person masking is set up: restart ComfyUI to turn it on.");
    } else if (!ComfyInstallDir().empty()) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Keep the rest exactly:");
        ImGui::SameLine();
        const bool failed = g_maskSetup.finished;
        if (ImGui::SmallButton(failed ? "Set up person masking (failed: retry)" : "Set up person masking (tiny, no models)")) StartMaskSetup();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("%s\nInstalls MaskVidExperiments (github.com/drozbay/MaskVidExperiments, GPL-3.0; about 100 KB) into ComfyUI Desktop; then "
                              "ComfyUI restarts. It uses the SAM 3 model you have.%s",
                              about, failed ? ("\n\nLast time: " + MaskSetupLine()).c_str() : "");
    }
    ImGui::PopID();
}

// ---- The original's facial expressions on the swapped faces (LivePortrait, tools\liveportrait\setup_liveportrait.ps1, by the user's click).
// The swap draws each new face from its picture and kept its expression: the girl's smile as she walks in and her open-mouthed surprise as
// she falls came out with the new face's mouth closed the whole time. LivePortrait moves a face that is already there: each
// swapped face is cropped, and the original's face in the same frame sets how open the mouth and the eyes are (retargeting by landmark ratios,
// which do not depend on whose face it is), with the swap's own head pose kept; then it is pasted back. Tested 2026-10-17 on the cafeteria
// take: taking the original's head motion too (the node's video mode) slid the new faces in their crops and widened Ben's eyes, and its raw
// expression values made Ava squint; the ratios kept both faces clean and opened her mouth at the fall. Lips x1.3: on a rap test clip
// x1.6 opened the mouth 27% wider than the rapper's on average (x1.0 too little); the mouth then moved as much as his (0.10 against 0.106 of the
// face's height) where the swap alone hardly moved it (0.02), and followed his (correlation +0.32 against +0.02). Faces are taken
// largest first in the take and in the clip alike (the setup's fix: the node took them in detection order). About 60 s a person
// for 6 s. It does not carry a smile with the mouth closed (that is left to the prompt's "what their faces do").
ScriptRun g_lpSetup;

void StartLpSetup() {
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    const fs::path script = fs::path(buf).parent_path() / L"tools" / L"liveportrait" / L"setup_liveportrait.ps1";
    const std::wstring install = ComfyInstallDir();
    std::error_code ec;
    if (g_lpSetup.running || install.empty() || !fs::exists(script, ec)) {
        g_lpSetup.line = install.empty() ? "error: ComfyUI Desktop was not found" : "error: tools\\liveportrait\\setup_liveportrait.ps1 is missing";
        return;
    }
    fs::create_directories(DataDir(), ec);
    g_lpSetup.log = DataDir() + L"\\liveportrait_setup.log";
    fs::remove(g_lpSetup.log, ec);
    g_lpSetup.line = "starting";
    g_lpSetup.finished = false;
    g_lpSetup.running = true;
    std::wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + script.wstring() + L"\" -Install \"" + install + L"\" -Log \"" + g_lpSetup.log + L"\"";
    std::thread([cmd]() {
        std::string out;
        RunCaptureHidden(cmd, out, true);
        g_lpSetup.running = false;
        g_lpSetup.finished = true;
    }).detach();
}

const std::string& LpSetupLine() {
    if (!g_lpSetup.log.empty() && ImGui::GetTime() - g_lpSetup.readAt > 0.5) {
        g_lpSetup.readAt = ImGui::GetTime();
        std::ifstream f(g_lpSetup.log);
        std::string l, last;
        while (std::getline(f, l)) {
            if (l.rfind("\xEF\xBB\xBF", 0) == 0) l = l.substr(3);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            if (!l.empty()) last = l;
        }
        if (!last.empty()) g_lpSetup.line = last;
    }
    return g_lpSetup.line;
}

bool FacesReady() { return g_v.vedaKnown && g_v.vedaInfo.liveportrait; }

// The faces pass: the take's faces (largest first; a second pass for the second person) with the clip's mouth and eye openness
json BuildFaces(int people) {
    json wf;
    auto node = [&](const std::string& id, const char* cls, json inputs) { wf[id] = {{"class_type", cls}, {"inputs", std::move(inputs)}, {"_meta", {{"title", cls}}}}; };
    node("1", "LoadVideo", {{"file", "@take"}});
    node("2", "GetVideoComponents", {{"video", json::array({"1", 0})}});
    node("3", "LoadVideo", {{"file", "@clip"}});
    node("4", "GetVideoComponents", {{"video", json::array({"3", 0})}});
    node("5", "GetImageSize", {{"image", json::array({"2", 0})}});
    // the clip as long as the take (it is longer: the swap snaps to 17k + 5 frames; seen: a faces video of 149 frames for a 141-frame take)
    node("7", "ImageFromBatch", {{"image", json::array({"4", 0})}, {"batch_index", 0}, {"length", json::array({"5", 2})}});
    node("6", "ImageScale", {{"image", json::array({"7", 0})}, {"upscale_method", "lanczos"}, {"width", json::array({"5", 0})}, {"height", json::array({"5", 1})}, {"crop", "center"}});
    node("10", "DownloadAndLoadLivePortraitModels", {{"precision", "fp16"}, {"mode", "human"}});
    node("11", "LivePortraitLoadFaceAlignmentCropper", {{"face_detector", "blazeface_back_camera"}, {"landmarkrunner_device", "CPU"}, {"face_detector_device", "cuda"},
                                                        {"face_detector_dtype", "fp16"}, {"keep_model_loaded", true}});
    json frames = json::array({"2", 0});
    for (int p = 0; p < std::clamp(people, 1, 2); ++p) {
        auto id = [&](int k) { return std::to_string(20 + p * 10 + k); };
        json crop = {{"pipeline", json::array({"10", 0})}, {"cropper", json::array({"11", 0})}, {"dsize", 512}, {"scale", 2.3}, {"vx_ratio", 0.0}, {"vy_ratio", -0.125},
                     {"face_index", p}, {"face_index_order", "large-small"}, {"rotate", true}};
        json src = crop, drv = crop;
        src["source_image"] = frames;
        drv["source_image"] = json::array({"6", 0});
        node(id(0), "LivePortraitCropper", src);
        node(id(1), "LivePortraitCropper", drv);
        node(id(2), "LivePortraitRetargeting", {{"driving_crop_info", json::array({id(1), 1})}, {"eye_retargeting", true}, {"eyes_retargeting_multiplier", 1.0},
                                                {"lip_retargeting", true}, {"lip_retargeting_multiplier", 1.3}});
        node(id(3), "LivePortraitProcess", {{"pipeline", json::array({"10", 0})}, {"crop_info", json::array({id(0), 1})}, {"source_image", frames},
                                            {"driving_images", json::array({id(1), 0})}, {"lip_zero", false}, {"lip_zero_threshold", 0.03}, {"stitching", true},
                                            {"delta_multiplier", 1.0}, {"mismatch_method", "constant"}, {"relative_motion_mode", "relative_rotation_only"},
                                            {"driving_smooth_observation_variance", 3e-6}, {"opt_retargeting_info", json::array({id(2), 0})}});
        node(id(4), "LivePortraitComposite", {{"source_image", frames}, {"cropped_image", json::array({id(3), 0})}, {"liveportrait_out", json::array({id(3), 1})}});
        frames = json::array({id(4), 0});
    }
    node("90", "CreateVideo", {{"images", frames}, {"audio", json::array({"2", 1})}, {"fps", json::array({"2", 2})}});
    node("91", "SaveVideo", {{"video", json::array({"90", 0})}, {"filename_prefix", "video/Faces"}, {"format", "auto"}, {"format.codec", "auto"}});
    return wf;
}

bool MakeFaces(AppState& app, const FacesJob& f) {
    if (f.video.empty() || f.clip.empty() || !FacesReady()) return false;
    ComfyJob job;
    job.apiWorkflow = BuildFaces(f.people).dump();
    job.rawUploads = {{"@take", f.video}, {"@clip", f.clip}};
    job.label = "Video (Faces): " + Utf8(fs::path(f.video).filename().wstring());
    job.times = 1;
    const int id = SendComfyJob(app, std::move(job));
    if (id) Toast(ToastKind::Info, "Copying the facial expressions", Utf8(fs::path(f.video).filename().wstring()) + ": the original's mouth and eyes on the new faces (LivePortrait)",
                  {}, [](AppState& s) { s.requestTab = 10; }, "faces");
    return id != 0;
}

// Under the swap's options: the switch when ComfyUI has LivePortrait, else what it takes
void DrawFacesRow() {
    ImGui::PushID("faces");
    const char* about = "LivePortrait moves the swapped faces the way the original's moved: how open the mouth and the eyes are, frame by frame "
                        "(a surprised open mouth, a blink), with the new face and its head pose kept. About a minute a person for 6 s.";
    if (FacesReady()) {
        if (ImGui::Checkbox("Then copy the original's facial expressions (LivePortrait)", &g_v.facesAfterSwap)) SaveSettings();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("%s\nA second video, Faces_..., once the swap is done (then made crisp, when that is on). A smile with the mouth closed is not "
                              "carried; the prompt asks for the faces' expressions too.", about);
    } else if (g_lpSetup.running) {
        ImGui::TextColored(ui::kAccentHover, "Setting up facial expressions: %s", LpSetupLine().c_str());
    } else if (g_lpSetup.finished && LpSetupLine() == "ok") {
        if (ImGui::GetTime() - g_v.vedaAsked > 10.0) g_v.vedaAsked = -1000.0;
        ImGui::TextColored(ui::kWarn, "Facial expressions are set up: restart ComfyUI to turn them on.");
    } else if (!ComfyInstallDir().empty()) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Faces:");
        ImGui::SameLine();
        const bool failed = g_lpSetup.finished;
        if (ImGui::SmallButton(failed ? "Set up facial expressions (failed: retry)" : "Set up facial expressions (LivePortrait, 0.7 GB)")) StartLpSetup();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("%s\nInstalls ComfyUI-LivePortraitKJ (github.com/kijai, MIT) with two fixes for this ComfyUI, four small Python packages "
                              "(scikit-image, tifffile, pykalman, scikit-base; ComfyUI's numpy is kept) and the LivePortrait models (0.6 GB, MIT) into "
                              "ComfyUI Desktop; its face finder downloads 0.1 GB on first use. Then ComfyUI restarts.%s",
                              about, failed ? ("\n\nLast time: " + LpSetupLine()).c_str() : "");
    }
    ImGui::PopID();
}

// "Speed up with Veda": the switch when ComfyUI has it, else what it takes to get it
void DrawVedaRow(AppState& app, const char* predictor) {
    ImGui::PushID(predictor);
    const bool node = g_v.vedaKnown && g_v.vedaInfo.node;
    const bool have = VedaReady(predictor) || (!g_v.vedaKnown && g_v.vedaWasReady);
    const char* about = "Veda sparse attention (github.com/veda-sparse/Veda-on-ComfyUI): a small learned predictor picks the ~10% of H3's attention "
                        "that matters and skips the rest, so videos render about 2-3x faster.";
    const std::string& line = VedaSetupLine();
    const bool setUp = g_vedaSetup.finished && line == "ok";
    if (have) {
        if (ImGui::Checkbox("Speed up with Veda", &g_v.veda)) SaveSettings();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s\nTurn it off to compare with full attention.", about);
        ImGui::SameLine();
        ImGui::TextDisabled(g_v.veda ? "about twice as fast" : "off: full attention");
    } else if (g_vedaSetup.running) {
        ImGui::TextColored(ui::kAccentHover, "Setting up Veda: %s", line.c_str());
    } else if (setUp) {
        // ComfyUI loads new nodes when it starts: ask it again every 10 s until it has them
        if (ImGui::GetTime() - g_v.vedaAsked > 10.0) g_v.vedaAsked = -1000.0;
        ImGui::TextColored(ui::kWarn, "Veda is set up: restart ComfyUI to turn it on.");
        if (ComfyOwned()) {
            bool busy = false;
            if (app.comfySvc) {
                for (const ComfyActivity& a : app.comfySvc->Activity()) busy |= a.state != ComfyState::Done && a.state != ComfyState::Failed;
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(busy);
            if (ImGui::SmallButton("Restart ComfyUI")) {
                std::string message;
                ComfyStopOwned();
                ComfyStart(app.comfy, message);
                app.SetStatus(message);
            }
            ImGui::EndDisabled();
            if (busy && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("After the videos being made");
        } else {
            ImGui::SameLine();
            ImGui::TextDisabled("(close and open ComfyUI Desktop)");
        }
    } else if (!g_v.vedaKnown) {
        ImGui::TextDisabled("Speed up with Veda: start ComfyUI to see if it is set up");
    } else if (VedaScript().empty()) {
        ImGui::TextDisabled("Speed up with Veda: tools\\veda is missing next to Try2Create.exe");
    } else if (ComfyInstallDir().empty()) {
        ImGui::TextDisabled("Speed up with Veda: set it up in ComfyUI (custom node Veda-on-ComfyUI)");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s\nThe automatic setup is for ComfyUI Desktop.", about);
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Speed up:");
        ImGui::SameLine();
        const bool failed = g_vedaSetup.finished && !setUp;
        if (ImGui::SmallButton(failed ? "Set up Veda (failed: retry)" : node ? "Get Veda's predictors (550 MB)" : "Set up Veda (about 0.6 GB)")) StartVedaSetup();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("%s\nInstalls into ComfyUI Desktop: its custom node, triton-windows 3.8 (about 50 MB) into ComfyUI's Python,\n"
                              "and two 275 MB predictors into ComfyUI\\models\\veda. Then ComfyUI restarts.",
                              about);
        if (failed) {
            ImGui::SameLine();
            ImGui::TextColored(ui::kWarn, "%s", line.c_str());
        }
    }
    ImGui::PopID();
}

void StartRefDownload() {
    if (g_ref.downloading || g_ref.dir.empty()) return;
    g_ref.downloading = true;
    g_ref.failed = false;
    const std::wstring dest = (fs::path(g_ref.dir) / kRefModel).wstring();
    std::thread([dest]() {
        std::string out;
        const std::wstring cmd = L"curl.exe -L --fail --retry 5 --retry-delay 10 -C - --no-progress-meter -o \"" + dest + L".part\" \"" +
                                 std::wstring(kRefModelUrl, kRefModelUrl + strlen(kRefModelUrl)) + L"\"";
        const bool ok = RunCaptureHidden(cmd, out, true);
        std::error_code ec;
        if (ok && fs::exists(dest + L".part", ec) && fs::file_size(dest + L".part", ec) > 1000000000ull) {
            fs::rename(dest + L".part", dest, ec);
            g_ref.failed = static_cast<bool>(ec);
        } else {
            g_ref.failed = true;
        }
        g_ref.downloading = false;
        g_ref.asked = -1000.0;
    }).detach();
}

// ---- What a swap can reuse, so the same person or the same part of a clip again costs nothing: what the writer saw (a picture's look and sex,
// a clip part's performer, moves and sex) in swap_cache.json, the cut clips in h3\clips, the clips' film strips in h3\strips. With both
// descriptions known the writer is not started at all, and ComfyUI keeps H3 loaded (the writer's model would push it out of the GPU).
std::mutex g_swapCacheMutex;
json g_swapCache;
bool g_swapCacheLoaded = false;

json& SwapCacheLocked() { // (hold g_swapCacheMutex)
    if (!g_swapCacheLoaded) {
        g_swapCacheLoaded = true;
        std::ifstream f(DataDir() + L"\\swap_cache.json");
        if (f) g_swapCache = json::parse(f, nullptr, false);
        if (!g_swapCache.is_object()) g_swapCache = json::object();
    }
    return g_swapCache;
}

void SaveSwapCacheLocked() {
    std::error_code ec;
    fs::create_directories(DataDir(), ec);
    const std::wstring path = DataDir() + L"\\swap_cache.json";
    {
        std::ofstream f(path + L".tmp", std::ios::trunc);
        if (!f) return;
        f << g_swapCache.dump(1);
    }
    fs::rename(path + L".tmp", path, ec);
}

// a file as it is now: its path, size and time (a changed file is a new key)
std::string FileKey(const std::wstring& path) {
    std::error_code ec;
    std::wstring low = path;
    std::transform(low.begin(), low.end(), low.begin(), ::towlower);
    const auto size = fs::file_size(path, ec);
    const auto time = fs::last_write_time(path, ec).time_since_epoch().count();
    return Utf8(low) + "|" + std::to_string(ec ? 0 : size) + "|" + std::to_string(static_cast<long long>(time));
}

std::wstring HashName(const std::string& key) { // FNV-1a, 16 hex digits
    unsigned long long h = 1469598103934665603ull;
    for (unsigned char c : key) h = (h ^ c) * 1099511628211ull;
    wchar_t buf[24];
    swprintf(buf, 24, L"%016llx", h);
    return buf;
}

// keeps the newest `keep` files of a cache folder
void PruneCacheDir(const std::wstring& dir, size_t keep) {
    std::error_code ec;
    std::vector<std::pair<fs::file_time_type, fs::path>> files;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.is_regular_file(ec)) files.emplace_back(e.last_write_time(ec), e.path());
    }
    if (files.size() <= keep) return;
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t i = keep; i < files.size(); ++i) fs::remove(files[i].second, ec);
}

// The length a swap really makes: H3 snaps to 17k + 5 frames (at 24 fps), and the guide must cover all of them - so the nearest such
// length to `want`, no longer than the clip (from 3 s, at most 15 s)
// The longest part a swap takes: a minute (made in parts of at most 10 s, joined); Recreate stays within one 15 s take
double StarMaxLen() { return g_starMethod == 0 ? 60.0 : 15.0; }
constexpr double kSwapPartMax = 10.0; // a swap's longest single take: longer ones are made in parts and joined

double SwapLength(double want, double clipSeconds) {
    if (g_starMethod == 0 && want > 15.2) { // a long swap: any length up to a minute (its parts are each made at H3's own lengths)
        const double mx = std::min(60.0, clipSeconds > 0 ? clipSeconds : 60.0);
        return std::round(std::min(want, mx) * 24.0) / 24.0;
    }
    const int maxFrames = static_cast<int>(std::floor(std::min(15.2, clipSeconds > 0 ? clipSeconds : 15.2) * 24.0));
    int best = 73, bestD = 1 << 30;
    for (int f = 73; f <= 362; f += 17) {
        if (f > maxFrames && f != 73) break;
        const int d = std::abs(f - static_cast<int>(std::lround(want * 24.0)));
        if (d < bestD) bestD = d, best = f;
    }
    return best / 24.0;
}

// The clip as the model wants it: from `start`, at most `seconds` long, 24 fps, about 768 px on the short side (ffmpeg). "" = no ffmpeg: the
// original goes as it is (its own frame rate makes the motion faster or slower). Cut once: the same part again is the same file.
std::wstring PrepareClip(const std::wstring& src, double start, double seconds, int shortSide, std::string& note) {
    const std::wstring ff = FfmpegPath();
    if (ff.empty()) {
        note = "ffmpeg is not on the PATH: the clip goes as it is (motion may be faster or slower)";
        return src;
    }
    static std::mutex oneAtATime; // (a cut made ahead, while you choose, and Make's own: never the same file twice at once)
    std::lock_guard<std::mutex> one(oneAtATime);
    const std::wstring dir = DataDir() + L"\\h3\\clips";
    std::error_code ec;
    fs::create_directories(dir, ec);
    char part[96];
    snprintf(part, sizeof(part), "|%.3f|%.3f|%d", start, seconds, shortSide);
    const std::wstring out = dir + L"\\" + HashName(FileKey(src) + part) + L".mp4";
    if (fs::exists(out, ec) && fs::file_size(out, ec) > 1000) return out;
    PruneCacheDir(dir, 150); // (each cut and its three frame pictures)
    // the short side as the video's own: every reference pixel rides along every sampling step (a 768 px clip made a 480p video 4x slower)
    char span[200];
    snprintf(span, sizeof(span), "-ss %.2f -t %.2f -i \"", std::max(0.0, start), std::clamp(seconds, 1.0, 15.0) + 0.3);
    char vf[200];
    snprintf(vf, sizeof(vf), "\" -vf \"fps=24,scale='if(gt(iw,ih),-2,%d)':'if(gt(iw,ih),%d,-2)'\" ", shortSide, shortSide);
    const std::wstring cmd = L"\"" + ff + L"\" -y -hide_banner -loglevel error " + std::wstring(span, span + strlen(span)) + src + std::wstring(vf, vf + strlen(vf)) +
                             L"-c:v libx264 -preset veryfast -crf 18 -pix_fmt yuv420p -c:a aac -b:a 192k \"" + out + L"\"";
    std::string log;
    RunCaptureHidden(cmd, log, true);
    if (!fs::exists(out, ec) || fs::file_size(out, ec) < 1000) {
        fs::remove(out, ec);
        note = "ffmpeg could not cut the clip: it goes as it is";
        return src;
    }
    return out;
}

// One picture of the clip, for the prompt writer to look at
std::wstring ClipFrame(const std::wstring& clip, double at, int n) {
    const std::wstring ff = FfmpegPath();
    if (ff.empty()) return {};
    const std::wstring out = clip + L".frame" + std::to_wstring(n) + L".png";
    char t[32];
    snprintf(t, sizeof(t), "-ss %.2f", std::max(0.0, at));
    std::string log;
    RunCaptureHidden(L"\"" + ff + L"\" -y -hide_banner -loglevel error " + std::wstring(t, t + strlen(t)) + L" -i \"" + clip + L"\" -frames:v 1 \"" + out + L"\"", log, true);
    std::error_code ec;
    return fs::exists(out, ec) ? out : std::wstring();
}

// The reference model does what the words say: the place and the action have to come from the CLIP (three of its frames, seen by the writer),
// and the pictures only say who. Written from the pictures alone, the video took their plain studio and an action of its own.
// (the timeline's moves never name the clip's clothes: "adjusting their suit jacket" put a woman in the dancer's blazer and grey trousers)
const char* const kStarSystem =
    "You write prompts for MiniMax H3 reference-to-video, which makes video WITH sound from references. The new video must RECREATE the reference "
    "clip <Video 1> shot for shot - the same place, the same framing and camera moves, the same actions in the same order and timing - with only "
    "the performer replaced by <Subject 1>.\n"
    "The attached pictures, in order: {PICTURES}{FRAMES}\n"
    "The pictures of <Subject 1> only tell you WHO (face, hair, body, clothes): ignore their background, pose and anything they hold or carry (a drink, "
    "food, a phone, a sign) - none of it is in the video unless the clip's frames show it. The setting and every action come "
    "from the clip's frames.\n"
    "Write it in this form:\n"
    "subject_definitions: <Subject 1> is the person in <Picture 1>, then how they look: face, hair, and their WHOLE outfit as <Picture 1> shows it "
    "(top, bottom, shoes; what is not seen, made to go with it).\n"
    "Setting: the place, light and mood of <Video 1> as its frames show them.\n"
    "Timeline: {BEATS} beats as [Xs-Ys] covering 0 to {SECONDS} seconds: what happens in <Video 1> at that time (read it off the frames: where the "
    "performer is, what they do, where they go), now done by <Subject 1>, and the camera. Moves and places only: never name what the clip's "
    "performer wears or touches of their clothes (no suit, jacket, tie, dress, hat - not \"adjusts their jacket\"); <Subject 1> wears only their "
    "own clothes.\n"
    "{AUDIO}\n"
    "Reply with ONLY the prompt (under 200 words): no preface, no notes, no numbering, no markdown.";

std::string StripSex(std::string s); // (below)

std::string StarOpening(const WriteJob& j) {
    if (j.who.size() >= 2) { // several people: each takes someone's place
        std::string s = "Recreate <Video 1> with new performers: the same place, framing, camera moves, actions and timing as <Video 1>; only the "
                        "people change.";
        for (size_t k = 0; k < j.who.size() && k < 4; ++k) {
            const std::string subj = "<Subject " + std::to_string(k + 1) + ">";
            const std::string given = k < j.replaces.size() ? StripSex(j.replaces[k]) : std::string();
            s += " " + subj + " (the person in <Picture " + std::to_string(k + 1) + ">) takes the place of " +
                 (given.empty() ? std::string(k == 0 ? "the most prominent performer" : "another performer") : given) + ", with " + subj +
                 "'s own face, hair, body and clothes.";
        }
        s += " None of the original performers appears.";
        return s + (j.idea.empty() ? std::string() : " Also: " + j.idea + ".");
    }
    const std::string given = !j.replaces.empty() ? StripSex(j.replaces[0]) : std::string();
    return "Recreate <Video 1> with <Subject 1> (the person in <Picture 1>) as " + (given.empty() ? std::string("the performer") : "the one in place of " + given) +
           ": the same place, framing, camera moves, actions and timing as <Video 1>; only the performer changes, to <Subject 1>'s face, hair, body "
           "and clothes (nothing of the original performer's outfit). The original performer of <Video 1> never appears: <Subject 1> is the one in "
           "every shot, from the first frame to the last." +
           (j.idea.empty() ? std::string() : " Also: " + j.idea + ".");
}

std::string WriteStarPrompt(const WriteJob& j, const std::wstring& clip, const std::string& comfyUrl, std::string& note) {
    const int secs = static_cast<int>(std::lround(j.opt.seconds));
    auto fallback = [&]() {
        std::string s = StarOpening(j) + "\nsubject_definitions: <Subject 1> is the person in <Picture 1>.\n";
        s += j.keepSound ? "Audio: use <Audio 1> exactly as it is." : "Audio: sounds that match the movement and the place, and fitting music.";
        return s;
    };
    if (!WriterConfigured()) {
        note = "the prompt writer is not set up (Clef mode > Prompt writer)";
        return fallback();
    }
    const ClefSession session = WriterAcquire(comfyUrl, SetWritePhase);
    std::string out;
    if (session.ok) {
        SetWritePhase("Writing the video prompt...");
        const bool see = WriterCanSee();
        std::vector<std::string> images;
        std::string pics;
        int attached = 0;
        for (size_t k = 0; k < j.who.size(); ++k) {
            const std::string url = see ? ImageDataUrl(j.who[k], 640) : std::string();
            if (url.empty()) continue;
            images.push_back(url);
            pics += "picture " + std::to_string(++attached) + " is <Picture " + std::to_string(k + 1) + "> (<Subject " + std::to_string(k + 1) + ">" +
                    (k < j.names.size() && !j.names[k].empty() ? ", called " + j.names[k] : std::string()) + "); ";
        }
        // three frames of the clip: its start, middle and end
        const double len = std::clamp(j.opt.seconds, 1.0, 15.0);
        std::string frames;
        const double at[3] = {0.1, len * 0.5, std::max(0.2, len - 0.3)};
        const char* when[3] = {"its start", "its middle", "its end"};
        for (int f = 0; f < 3 && see; ++f) {
            const std::wstring png = ClipFrame(clip, at[f], f);
            const std::string url = png.empty() ? std::string() : ImageDataUrl(png, 640);
            if (url.empty()) continue;
            images.push_back(url);
            char buf[96];
            snprintf(buf, sizeof(buf), "picture %d is a frame of <Video 1> at %.1f s (%s); ", ++attached, at[f], when[f]);
            frames += buf;
        }
        if (frames.empty()) frames = "(no frames of the clip could be shown: describe the clip's place and actions only in general terms) ";
        std::string system = kStarSystem;
        system = ReplaceAll(system, "{PICTURES}", pics);
        system = ReplaceAll(system, "{FRAMES}", frames);
        system = ReplaceAll(system, "{BEATS}", std::to_string(BeatsFor(j.opt.seconds)));
        system = ReplaceAll(system, "{SECONDS}", std::to_string(secs));
        system = ReplaceAll(system, "{AUDIO}", j.keepSound ? std::string("Audio: use <Audio 1> exactly as it is.")
                                                           : std::string("Audio: one line: voices, sound effects matching the moves, ambience, music."));
        std::string user = "Length: " + std::to_string(secs) + " seconds.\n" +
                           (j.idea.empty() ? std::string("The user's idea: none, just put <Subject 1> in place of the performer.\n") : "The user's idea (follow it): " + j.idea + "\n");
        if (j.who.size() >= 2) { // several people: each is their own subject
            user += "There are " + std::to_string(std::min<size_t>(j.who.size(), 4)) + " new people, each their own subject: ";
            for (size_t k = 0; k < j.who.size() && k < 4; ++k) {
                const std::string given = k < j.replaces.size() ? StripSex(j.replaces[k]) : std::string();
                user += "<Subject " + std::to_string(k + 1) + "> is the person in <Picture " + std::to_string(k + 1) + ">" +
                        (k < j.names.size() && !j.names[k].empty() ? " (" + j.names[k] + ")" : std::string()) +
                        (given.empty() ? std::string() : ", in place of " + given) + "; ";
            }
            user += "define each in subject_definitions and say in the timeline who does what (names in the user's idea mean these people).\n";
        }
        std::string err;
        for (int attempt = 0; attempt < 2; ++attempt) { // (seen once: an answer with nothing usable in it, where the next take was fine)
            std::string raw;
            err.clear();
            if (!WriterChat(system, user, images, 1000, attempt == 0 ? 0.6 : 0.3, raw, &err)) {
                note = "the prompt writer did not answer" + (err.empty() ? std::string() : ": " + err);
                continue;
            }
            out = CleanWritten(Trimmed(raw));
            if (out.size() >= 40) {
                note.clear();
                break;
            }
            std::ofstream(DataDir() + L"\\star_writer_last.txt", std::ios::binary) << raw; // (what it said, when it was not usable)
        }
    } else {
        note = "the prompt writer could not be started";
    }
    WriterRelease(session, false);
    SetWritePhase("");
    if (out.size() < 40) { // (the tags are in the opening line: the writer's part need not repeat them)
        if (note.empty()) note = "the prompt writer wrote nothing usable";
        std::ofstream(DataDir() + L"\\star_writer_note.txt", std::ios::binary) << note;
        return fallback();
    }
    return StarOpening(j) + "\n" + out; // the opening line always: it is what makes the clip's motion lead
}

// ---- Swap the person (Character Swap LoRA). Its prompt has a fixed form (body_swap:, subject_definitions, summary, retention_analysis,
// detailed_description, as in the LoRA's own and wangweihong's workflows); the writer only fills in three things it can see: how the new
// person looks (the LoRA takes the look from the WORDS as much as from the picture: a vague one blends the clip's performer into it),
// who in the clip is replaced, and what they do.
// Two looks, each kept for next time (swap_cache.json): the person (all their pictures) and the part of the clip.
const char* const kSwapLookSystem =
    "You describe a person for MiniMax H3 Character Swap, which puts them into a video in place of someone else. The attached pictures all "
    "show this same person (<Picture 1>). Reply with exactly two lines and nothing else:\n"
    "LOOK: how the person looks, written to continue the words \"<Subject 1> is the person shown in <Picture 1>,\": apparent age and sex, skin "
    "tone and ethnicity as seen, face shape and features, eye colour, hair (colour, length, style), body build and height, then the outfit "
    "from top to bottom with colours and materials, and the shoes. Concrete and visual, 40 to 80 words. Only what the pictures show; ignore "
    "the background and the pose.\n"
    "SEX: man or woman.";
const char* const kSwapClipSystem =
    "You describe a video clip for MiniMax H3 Character Swap: its main performer will be replaced by someone else. The attached pictures are "
    "frames of the clip, in order: {FRAMES}\n"
    "Reply with exactly three lines and nothing else:\n"
    "TARGET: who is replaced, as a short noun phrase: \"the main performer\" if one person stands out; if several people are in it, say which "
    "one by place and look (e.g. \"the dancer in the middle in the black T-shirt\").\n"
    "MOTION: one or two sentences in English: what that person does through the clip (moves, turns, gestures, where they go) and how the "
    "camera moves. Call the person \"the performer\", never \"he\" or \"she\" (someone of either sex will do it).\n"
    "SEX: that person's sex, man or woman.";

std::string SwapField(const std::string& text, const char* key) { // "KEY: value"; markdown around the key ("**LOOK**:") or the value on the next line too
    std::istringstream in(text);
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) {
        std::string clean;
        for (char c : line) {
            if (c != '*' && c != '#' && c != '\r') clean += c;
        }
        lines.push_back(Trimmed(clean));
    }
    std::string k = key; // "LOOK:" -> "LOOK"
    if (!k.empty() && k.back() == ':') k.pop_back();
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string l = lines[i];
        while (!l.empty() && (l.front() == '-' || l.front() == ' ')) l.erase(l.begin());
        if (l.size() < k.size() || _strnicmp(l.c_str(), k.c_str(), k.size()) != 0) continue;
        std::string v = Trimmed(l.substr(k.size()));
        if (v.empty() || v.front() != ':') continue;
        v = Trimmed(v.substr(1));
        if (v.empty() && i + 1 < lines.size()) v = lines[i + 1];
        while (!v.empty() && (v.front() == '"' || v.front() == ' ')) v.erase(v.begin());
        while (!v.empty() && (v.back() == '"' || v.back() == ' ')) v.pop_back();
        return v;
    }
    return {};
}

// When a woman takes a man's place (or the other way round) the LoRA kept the clip's body: the face changed but his hands, arms and build
// stayed. Said plainly, part by part, the whole body follows the picture.
std::string SwapBody(const std::string& sexes) {
    std::string l;
    for (char c : sexes) l += static_cast<char>(tolower(static_cast<unsigned char>(c)));
    const size_t a = l.find_first_of("mw");
    if (a == std::string::npos) return {};
    const bool newWoman = l.compare(a, 5, "woman") == 0;
    const size_t b = l.find_first_of("mw", a + (newWoman ? 5 : 3));
    if (b == std::string::npos) return {};
    const bool oldWoman = l.compare(b, 5, "woman") == 0;
    if (newWoman == oldWoman) return {};
    if (newWoman)
        return " <Subject 1> is a woman and replaces a man: her WHOLE body is a woman's, not only the face - slender feminine hands with narrow "
               "wrists and fingers, slim arms, narrow shoulders, a woman's chest, waist, hips and legs, her own height and build. Nothing of the "
               "man's hands, arms, shoulders, build or clothes remains; she does his moves with her own body.";
    return " <Subject 1> is a man and replaces a woman: his WHOLE body is a man's, not only the face - large masculine hands with broad wrists "
           "and thick fingers, muscular arms, broad shoulders, a man's flat chest, waist, hips and legs, his own height and build. Nothing of "
           "the woman's hands, arms, figure, hair or clothes remains; he does her moves with his own body.";
}

std::string SwapPrompt(std::string look, std::string target, std::string motion, const WriteJob& j, const std::string& sexes = std::string()) {
    { // the writer sometimes repeats the words it was asked to continue ("<Subject 1> is the person shown in <Picture 1>: a young woman...")
        const size_t pic = look.find("<Picture 1>");
        if (look.rfind("<Subject 1>", 0) == 0 && pic != std::string::npos && pic < 60) {
            size_t a = pic + strlen("<Picture 1>");
            while (a < look.size() && strchr(" ,:;-", look[a])) ++a;
            look = look.substr(a);
        }
    }
    if (look.empty()) look = "with exactly the face, hair, skin tone, body and clothes that picture shows";
    while (!look.empty() && (look.back() == '.' || look.back() == ',')) look.pop_back();
    if (target.empty()) target = "the main performer";
    while (!target.empty() && target.back() == '.') target.pop_back();
    if (!motion.empty() && motion.back() != '.') motion += '.';
    std::string p = "body_swap:\nsubject_definitions:\n<Subject 1> is the person shown in <Picture 1>, " + look +
                    ". Their face, hair, skin tone, body proportions and clothing define the replacement character.\n"
                    "summary:\n[video generation + reference] Replace " + target + " in the aligned guide with <Subject 1>. Follow the source "
                    "performance, camera and editing timing frame by frame.\n"
                    "retention_analysis:\n<Subject 1>: fully_preserved - Preserve the face, skin tone, hair, body and outfit of <Picture 1>.\n"
                    "Guide scene: fully_preserved - Preserve the source environment, background, props, lighting, camera, framing, motion, timing and cuts.\n"
                    "detailed_description:\nOnly " + target + " becomes <Subject 1>. " + (motion.empty() ? std::string() : motion + " ") +
                    "Match the guide's complete body movement, limb poses, hand contact, head direction and facial expressions naturally; <Subject 1>'s "
                    "hair and clothing follow the moving body. Keep <Subject 1>'s own face, features and skin tone exactly as in <Picture 1>: do not "
                    "blend in the original performer's face, hair, skin or clothes." + SwapBody(sexes) +
                    " Do not use the reference picture's background. No additional people, no added text or watermark.";
    if (!j.idea.empty()) p += " Also: " + j.idea + ".";
    if (!j.keepSound) p += "\nAudio: sounds that match the movement and the place, and fitting music.";
    return p;
}

// The clip's performer is someone of the other sex as often as not: "he" / "she" in what they do would pull the old body back in
std::string NoPronouns(const std::string& text) {
    static const std::pair<const char*, const char*> swaps[] = {{"he", "the performer"}, {"she", "the performer"}, {"his", "their"}, {"her", "their"},
                                                                {"him", "them"}, {"himself", "themselves"}, {"herself", "themselves"},
                                                                {"He", "The performer"}, {"She", "The performer"}, {"His", "Their"}, {"Her", "Their"}};
    std::string out;
    size_t i = 0;
    while (i < text.size()) {
        if (!isalpha(static_cast<unsigned char>(text[i]))) {
            out += text[i++];
            continue;
        }
        size_t e = i;
        while (e < text.size() && isalpha(static_cast<unsigned char>(text[e]))) ++e;
        const std::string w = text.substr(i, e - i);
        const char* to = nullptr;
        for (const auto& [from, rep] : swaps) {
            if (w == from) to = rep;
        }
        out += to ? to : w;
        i = e;
    }
    return out;
}


// ---- several people at once (a picture each): <Subject k> is the person in <Picture k>, and each replaces someone else in the clip. The
// user names who is who in their own words ("the girl with ava and the boy with ben") or picks it on each card; the writer matches the rest
// against frames of the clip. (Before, every picture was taken as the same person: the second one was ignored.)
const char* const kSwapPeopleSystem =
    "You look at frames of a video clip, in order: {FRAMES}\n"
    "List the people who are clearly visible (at most 4), most prominent first. Reply with one line per person and nothing else:\n"
    "PERSON: <a short noun phrase that tells them apart by look and place, e.g. \"the girl in the pink floral top\", \"the man on the left in "
    "the black suit\"> | <man or woman>";
const char* const kSwapAssignSystem =
    "You set up MiniMax H3 Character Swap, which puts new people into a video in place of people in it. The attached pictures are frames of "
    "the clip, in order: {FRAMES}\n"
    "The new people:\n{SUBJECTS}"
    "Decide whom in the clip each new person replaces: follow the user's words when they say it (names refer to the new people above), "
    "otherwise match by sex and prominence; never the same clip person twice. When the user's words fit more than one person (\"the girl\" in a "
    "crowd), it is the most prominent of them: in the most frames, biggest, at the centre of the action - not someone in the background. "
    "Reply with exactly these lines and nothing else:\n"
    "{LINES}"
    "MOTION: one or two sentences: what those people do through the clip and how the camera moves, calling each by the same phrase you "
    "used above (never he, she, his or her).";

std::vector<std::string> ClipFrameUrls(const std::wstring& clip, double offset, double seconds, std::string& frames) {
    std::vector<std::string> images;
    const double len = std::clamp(seconds, 1.0, 15.0);
    const double at[4] = {0.1, len * 0.33, len * 0.66, std::max(0.2, len - 0.3)};
    for (int f = 0; f < 4; ++f) {
        const std::wstring png = ClipFrame(clip, offset + at[f], 10 + f);
        const std::string url = png.empty() ? std::string() : ImageDataUrl(png, 640);
        if (url.empty()) continue;
        images.push_back(url);
        char buf[48];
        snprintf(buf, sizeof(buf), "%s%.1f s", frames.empty() ? "" : ", ", at[f]);
        frames += buf;
    }
    if (!frames.empty()) frames += ".";
    return images;
}

// Who is in the clip (for the cast cards' "replaces" lists): "the girl in the pink floral top (woman)"; kept for next time
std::string PeopleKey(const std::wstring& video, double start, double seconds) {
    char part[64];
    snprintf(part, sizeof(part), "|%.3f|%.3f", start, seconds);
    return FileKey(video) + part;
}
std::vector<std::string> FindClipPeople(std::wstring video, double start, double seconds, std::string url, std::string* note) {
    const std::string key = PeopleKey(video, start, seconds);
    {
        std::lock_guard<std::mutex> lock(g_swapCacheMutex);
        const json& c = SwapCacheLocked();
        if (c.contains("people") && c["people"].contains(key) && c["people"][key].is_array() && !c["people"][key].empty())
            return c["people"][key].get<std::vector<std::string>>();
    }
    std::vector<std::string> out;
    if (!WriterConfigured()) {
        if (note) *note = "the prompt writer is not set up (Clef mode > Prompt writer)";
        return out;
    }
    const ClefSession session = WriterAcquire(url, SetWritePhase);
    if (session.ok && WriterCanSee()) {
        SetWritePhase("Looking for the people in the clip...");
        std::string frames;
        const std::vector<std::string> images = ClipFrameUrls(video, start, seconds, frames);
        std::string text, err;
        if (!images.empty() && WriterChat(ReplaceAll(kSwapPeopleSystem, "{FRAMES}", frames), "List the people.", images, 300, 0.2, text, &err)) {
            std::istringstream in(text);
            for (std::string l; std::getline(in, l) && out.size() < 4;) {
                std::string clean;
                for (char ch : l) {
                    if (ch != '*' && ch != '\r') clean += ch;
                }
                clean = Trimmed(clean);
                if (_strnicmp(clean.c_str(), "PERSON:", 7) != 0) continue;
                std::string v = Trimmed(clean.substr(7));
                std::string sex;
                if (const size_t bar = v.find('|'); bar != std::string::npos) sex = Trimmed(v.substr(bar + 1)), v = Trimmed(v.substr(0, bar));
                while (!v.empty() && (v.front() == '"' || v.back() == '"')) v.erase(v.front() == '"' ? v.begin() : v.end() - 1);
                if (v.empty()) continue;
                for (char& ch : sex) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
                out.push_back(v + (sex.rfind("wom", 0) == 0 ? " (woman)" : sex.rfind("man", 0) == 0 ? " (man)" : ""));
            }
        } else if (note) {
            *note = "the prompt writer did not answer" + (err.empty() ? std::string() : ": " + err);
        }
    } else if (note) {
        *note = session.ok ? "the prompt writer cannot see pictures" : "the prompt writer could not be started";
    }
    WriterRelease(session, false);
    SetWritePhase("");
    if (!out.empty()) {
        std::lock_guard<std::mutex> lock(g_swapCacheMutex);
        SwapCacheLocked()["people"][key] = out;
        SaveSwapCacheLocked();
    }
    return out;
}

std::string StripSex(std::string s) { // "the girl in the pink top (woman)" -> "the girl in the pink top"
    for (const char* t : {" (woman)", " (man)"}) {
        const size_t at = s.rfind(t);
        if (at != std::string::npos && at + strlen(t) == s.size()) s.resize(at);
    }
    return s;
}

std::string WriteSwapPromptMulti(const WriteJob& j, const std::wstring& clip, const std::string& comfyUrl, std::string& note) {
    const size_t n = std::min<size_t>(j.who.size(), 4);
    std::vector<std::string> looks(n), sexNew(n), targets(n), sexOld(n);
    std::string motion;
    // each person's look (kept per picture)
    for (size_t k = 0; k < n; ++k) {
        std::lock_guard<std::mutex> lock(g_swapCacheMutex);
        const json& c = SwapCacheLocked();
        const std::string key = FileKey(j.who[k]) + ";";
        if (c.contains("looks") && c["looks"].contains(key)) looks[k] = c["looks"][key].value("look", std::string()), sexNew[k] = c["looks"][key].value("sex", std::string());
    }
    std::string assignKey = FileKey(j.refVideo);
    {
        char part[64];
        snprintf(part, sizeof(part), "|%.3f|%.3f|", j.refStart, j.opt.seconds);
        assignKey += part + j.idea;
        for (size_t k = 0; k < n; ++k) assignKey += "|" + FileKey(j.who[k]) + "=" + (k < j.replaces.size() ? j.replaces[k] : std::string());
    }
    {
        std::lock_guard<std::mutex> lock(g_swapCacheMutex);
        const json& c = SwapCacheLocked();
        if (c.contains("assign") && c["assign"].contains(assignKey)) {
            const json& a = c["assign"][assignKey];
            for (size_t k = 0; k < n && k < a.value("targets", json::array()).size(); ++k) targets[k] = a["targets"][k].get<std::string>(), sexOld[k] = a["sexes"][k].get<std::string>();
            motion = a.value("motion", std::string());
        }
    }
    const bool needLooks = std::any_of(looks.begin(), looks.end(), [](const std::string& l) { return l.size() < 20; });
    const bool needAssign = std::any_of(targets.begin(), targets.end(), [](const std::string& t) { return t.empty(); });
    if ((needLooks || needAssign) && !WriterConfigured()) note = "the prompt writer is not set up (Clef mode > Prompt writer)";
    if ((needLooks || needAssign) && WriterConfigured()) {
        const ClefSession session = WriterAcquire(comfyUrl, SetWritePhase);
        const bool see = session.ok && WriterCanSee();
        if (!session.ok) note = "the prompt writer could not be started";
        else if (!see) note = "the prompt writer cannot see pictures: each person is only \"as in their picture\"";
        for (size_t k = 0; see && k < n; ++k) {
            if (looks[k].size() >= 20) continue;
            SetWritePhase("Looking at " + (k < j.names.size() ? j.names[k] : std::string("them")) + "...");
            const std::string url = ImageDataUrl(j.who[k], 768);
            std::string out, err;
            if (!url.empty() && WriterChat(kSwapLookSystem, "Describe the person.", {url}, 400, 0.3, out, &err)) {
                looks[k] = SwapField(out, "LOOK:");
                sexNew[k] = SwapField(out, "SEX:");
                if (looks[k].size() < 20) { // it sometimes leaves the label out (seen): the look is then everything but the SEX line
                    std::istringstream in(out);
                    std::string rest;
                    for (std::string l; std::getline(in, l);) {
                        l = Trimmed(l);
                        if (l.empty() || _strnicmp(l.c_str(), "SEX", 3) == 0 || _strnicmp(l.c_str(), "**SEX", 5) == 0) continue;
                        rest += (rest.empty() ? "" : " ") + l;
                    }
                    looks[k] = rest;
                }
                std::ofstream(DataDir() + L"\\swap_writer_look" + std::to_wstring(k + 1) + L".txt", std::ios::binary) << out; // (what it said)
            } else if (note.empty()) {
                note = "the prompt writer did not describe " + (k < j.names.size() ? j.names[k] : std::string("someone")) + (err.empty() ? std::string() : ": " + err);
            }
            if (looks[k].size() >= 20) {
                std::lock_guard<std::mutex> lock(g_swapCacheMutex);
                SwapCacheLocked()["looks"][FileKey(j.who[k]) + ";"] = {{"look", looks[k]}, {"sex", sexNew[k]}};
                SaveSwapCacheLocked();
            }
        }
        if (see && needAssign) {
            SetWritePhase("Matching the people to the clip...");
            std::string frames;
            const std::vector<std::string> images = ClipFrameUrls(clip, 0.0, j.opt.seconds, frames);
            std::string subjects, lines;
            for (size_t k = 0; k < n; ++k) {
                const std::string tag = "SUBJECT " + std::to_string(k + 1);
                subjects += tag + ": " + (k < j.names.size() && !j.names[k].empty() ? j.names[k] : "(no name)") + " - " +
                            (sexNew[k].empty() ? std::string() : sexNew[k] + ", ") + (looks[k].size() > 160 ? looks[k].substr(0, 160) + "..." : looks[k]);
                const std::string given = k < j.replaces.size() ? j.replaces[k] : std::string();
                if (!given.empty()) subjects += " (the user chose: replaces " + StripSex(given) + ")";
                else if (k < j.said.size() && !j.said[k].empty()) subjects += " (the user's words: replaces " + StripSex(j.said[k]) + " - describe that person by look and place)";
                subjects += "\n";
                lines += tag + ": <the person IN THE CLIP they replace, described by how that person looks and where they are in the frames (never a "
                               "new person's name)> | <that clip person's sex: man or woman>\n";
            }
            std::string system = ReplaceAll(ReplaceAll(ReplaceAll(kSwapAssignSystem, "{FRAMES}", frames), "{SUBJECTS}", subjects), "{LINES}", lines);
            const std::string user = j.idea.empty() ? std::string("Match them.") : "The user says: " + j.idea;
            std::string out, err;
            if (!images.empty() && WriterChat(system, user, images, 500, 0.2, out, &err)) {
                for (size_t k = 0; k < n; ++k) {
                    std::string v = SwapField(out, ("SUBJECT " + std::to_string(k + 1) + ":").c_str());
                    std::string sex;
                    if (const size_t bar = v.find('|'); bar != std::string::npos) sex = Trimmed(v.substr(bar + 1)), v = Trimmed(v.substr(0, bar));
                    targets[k] = v, sexOld[k] = sex;
                }
                motion = NoPronouns(SwapField(out, "MOTION:"));
                for (size_t k = 0; k < n && k < j.names.size(); ++k) { // (seen: "Ava walks through the cafeteria... lifts Ben")
                    const std::string& nm = j.names[k];
                    if (nm.size() < 2) continue;
                    for (size_t at = 0; (at = [&] {
                             for (size_t i = at; i + nm.size() <= motion.size(); ++i)
                                 if (_strnicmp(motion.c_str() + i, nm.c_str(), nm.size()) == 0 && (i == 0 || !isalnum(static_cast<unsigned char>(motion[i - 1]))) &&
                                     (i + nm.size() == motion.size() || !isalnum(static_cast<unsigned char>(motion[i + nm.size()]))))
                                     return i;
                             return std::string::npos;
                         }()) != std::string::npos;) {
                        const std::string tag = "<Subject " + std::to_string(k + 1) + ">";
                        motion.replace(at, nm.size(), tag);
                        at += tag.size();
                    }
                }
                std::ofstream(DataDir() + L"\\swap_writer_last.txt", std::ios::binary) << out;
            } else if (note.empty()) {
                note = "the prompt writer did not answer" + (err.empty() ? std::string() : ": " + err);
            }
            if (std::none_of(targets.begin(), targets.end(), [](const std::string& t) { return t.empty(); })) {
                std::lock_guard<std::mutex> lock(g_swapCacheMutex);
                SwapCacheLocked()["assign"][assignKey] = {{"targets", targets}, {"sexes", sexOld}, {"motion", motion}};
                SaveSwapCacheLocked();
            }
        }
        WriterRelease(session, false);
        SetWritePhase("");
    }
    for (size_t k = 0; k < n && k < j.names.size(); ++k) { // a name means nothing to H3: its subject (seen: "Ava walks... lifts Ben")
        const std::string& nm = j.names[k];
        if (nm.size() < 2) continue;
        const std::string tag = "<Subject " + std::to_string(k + 1) + ">";
        for (size_t i = 0; i + nm.size() <= motion.size(); ++i) {
            if (_strnicmp(motion.c_str() + i, nm.c_str(), nm.size()) != 0) continue;
            if (i > 0 && isalnum(static_cast<unsigned char>(motion[i - 1]))) continue;
            if (i + nm.size() < motion.size() && isalnum(static_cast<unsigned char>(motion[i + nm.size()]))) continue;
            motion.replace(i, nm.size(), tag);
            i += tag.size() - 1;
        }
    }
    // a target that is one of the new people's names is no target (seen: "Ava takes the woman's place" came back as "Replace Ava with
    // <Subject 1>"): the clip person's sex is said instead
    for (size_t k = 0; k < n; ++k) {
        std::string low = targets[k];
        for (char& ch : low) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
        bool isName = false;
        for (const std::string& nm : j.names) {
            std::string ln = nm;
            for (char& ch : ln) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
            if (ln.size() >= 2 && low.find(ln) != std::string::npos) isName = true;
        }
        if (isName) {
            std::string sex = sexOld[k];
            for (char& ch : sex) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
            targets[k] = sex.rfind("wom", 0) == 0 ? "the woman" : sex.rfind("man", 0) == 0 ? "the man" : (k == 0 ? "the most prominent person" : "the other person");
        }
    }
    // what the user chose on the cards wins; anyone still without a target takes "the person number k" in the clip
    for (size_t k = 0; k < n; ++k) {
        const std::string given = k < j.replaces.size() ? j.replaces[k] : std::string();
        if (!given.empty()) {
            targets[k] = StripSex(given);
            if (given.find("(woman)") != std::string::npos) sexOld[k] = "woman";
            else if (given.find("(man)") != std::string::npos) sexOld[k] = "man";
        }
        if (targets[k].empty() && k < j.said.size() && !j.said[k].empty()) {
            targets[k] = StripSex(j.said[k]);
            if (j.said[k].find("(woman)") != std::string::npos) sexOld[k] = "woman";
            else if (j.said[k].find("(man)") != std::string::npos) sexOld[k] = "man";
        }
        if (targets[k].empty()) targets[k] = k == 0 ? "the most prominent person" : "the next most prominent person";
        while (!targets[k].empty() && targets[k].back() == '.') targets[k].pop_back();
    }
    // the prompt, in the LoRA's form, with a subject per person
    auto subj = [](size_t k) { return "<Subject " + std::to_string(k + 1) + ">"; };
    auto pic = [](size_t k) { return "<Picture " + std::to_string(k + 1) + ">"; };
    std::string p = "body_swap:\nsubject_definitions:\n";
    for (size_t k = 0; k < n; ++k) {
        std::string look = looks[k];
        if (look.rfind("<Subject ", 0) == 0) { // (seen: the second person's look began "<Subject 1> is the person shown in <Picture 1>, a young man")
            const size_t at = look.find("<Picture ");
            const size_t close = at == std::string::npos ? std::string::npos : look.find('>', at);
            if (close != std::string::npos && close < 70) {
                size_t a = close + 1;
                while (a < look.size() && strchr(" ,:;-", look[a])) ++a;
                look = look.substr(a);
            }
        }
        while (!look.empty() && (look.back() == '.' || look.back() == ',')) look.pop_back();
        if (look.size() < 20) look = "with exactly the face, hair, skin tone, body and clothes that picture shows";
        p += subj(k) + " is the person shown in " + pic(k) + ", " + look + ". Their face, hair, skin tone, body proportions and clothing define this replacement character.\n";
    }
    p += "summary:\n[video generation + reference] Replace ";
    for (size_t k = 0; k < n; ++k) p += (k == 0 ? "" : k + 1 == n ? " and " : ", ") + targets[k] + " with " + subj(k);
    p += " in the aligned guide. Follow the source performance, camera and editing timing frame by frame.\nretention_analysis:\n";
    for (size_t k = 0; k < n; ++k) p += subj(k) + ": fully_preserved - Preserve the face, skin tone, hair, body and outfit of " + pic(k) + ".\n";
    p += "Guide scene: fully_preserved - Preserve the source environment, background, props, lighting, camera, framing, motion, timing and cuts.\n"
         "detailed_description:\n";
    for (size_t k = 0; k < n; ++k) p += (k == 0 ? "" : " ") + std::string(1, static_cast<char>(toupper(static_cast<unsigned char>(targets[k][0])))) + targets[k].substr(1) + " becomes " + subj(k) + ".";
    if (!motion.empty()) p += " " + motion + (motion.back() == '.' ? "" : ".");
    p += " Each of them matches the guide's complete body movement, limb poses, hand contact, head direction and facial expressions naturally; their hair "
         "and clothing follow the moving body. Each keeps their own face, features and skin tone exactly as in their own picture (";
    for (size_t k = 0; k < n; ++k) p += (k == 0 ? "" : ", ") + subj(k) + " as " + pic(k);
    // (tried 2026-10-17: "keeps their own identity ... but makes the facial expressions of the person they replace" put Ava in the boy's
    // close-up and Ben in the girl's shots; the faces' expressions are said per shot instead, and LivePortrait moves mouths and eyes after)
    p += "): do not blend in the original performers' faces, hair, skin or clothes, and do not mix the new people up.";
    for (size_t k = 0; k < n; ++k) p += ReplaceAll(SwapBody(sexNew[k] + " " + sexOld[k]), "<Subject 1>", subj(k));
    p += " Do not use the reference pictures' backgrounds. No additional people, no added text or watermark.";
    if (!j.keepSound) p += "\nAudio: sounds that match the movement and the place, and fitting music.";
    return p;
}

std::string WriteSwapPrompt(const WriteJob& j, const std::wstring& clip, const std::string& comfyUrl, std::string& note) {
    if (j.who.size() >= 2) return WriteSwapPromptMulti(j, clip, comfyUrl, note); // several people, each in someone's place
    std::string lookKey;
    for (const std::wstring& w : j.who) lookKey += FileKey(w) + ";";
    char part[64];
    snprintf(part, sizeof(part), "|%.3f|%.3f|", j.refStart, j.opt.seconds);
    const std::string clipKey = FileKey(j.refVideo) + part + j.idea;
    std::string look, sexNew, target, motion, sexOld;
    {
        std::lock_guard<std::mutex> lock(g_swapCacheMutex);
        const json& c = SwapCacheLocked();
        if (c.contains("looks") && c["looks"].contains(lookKey)) {
            look = c["looks"][lookKey].value("look", std::string());
            sexNew = c["looks"][lookKey].value("sex", std::string());
        }
        if (c.contains("clips") && c["clips"].contains(clipKey)) {
            target = c["clips"][clipKey].value("target", std::string());
            motion = c["clips"][clipKey].value("motion", std::string());
            sexOld = c["clips"][clipKey].value("sex", std::string());
        }
    }
    const bool needLook = look.size() < 20, needClip = target.empty();
    if ((needLook || needClip) && !WriterConfigured()) note = "the prompt writer is not set up (Clef mode > Prompt writer)";
    if ((needLook || needClip) && WriterConfigured()) {
        const ClefSession session = WriterAcquire(comfyUrl, SetWritePhase);
        if (!session.ok) note = "the prompt writer could not be started";
        const bool see = session.ok && WriterCanSee();
        if (session.ok && !see) note = "the prompt writer cannot see pictures: the swap prompt says only \"as in the picture\" (describe the look in the prompt for a closer match)";
        if (see && needLook) {
            SetWritePhase("Looking at who goes in...");
            std::vector<std::string> images;
            for (const std::wstring& w : j.who) {
                const std::string url = ImageDataUrl(w, 768);
                if (!url.empty()) images.push_back(url);
            }
            std::string out, err;
            if (!images.empty() && WriterChat(kSwapLookSystem, "Describe the person.", images, 400, 0.3, out, &err)) {
                look = SwapField(out, "LOOK:");
                sexNew = SwapField(out, "SEX:");
                if (look.size() < 20) { // it sometimes leaves the label out: the look is then everything but the SEX line
                    std::istringstream in(out);
                    std::string rest;
                    for (std::string l; std::getline(in, l);) {
                        l = Trimmed(l);
                        if (l.empty() || _strnicmp(l.c_str(), "SEX", 3) == 0 || _strnicmp(l.c_str(), "**SEX", 5) == 0) continue;
                        rest += (rest.empty() ? "" : " ") + l;
                    }
                    look = rest;
                }
                std::ofstream(DataDir() + L"\\swap_writer_last.txt", std::ios::binary) << out; // (what it said, when it could not be read)
            } else {
                note = "the prompt writer did not answer" + (err.empty() ? std::string() : ": " + err);
            }
            if (look.size() >= 20) {
                std::lock_guard<std::mutex> lock(g_swapCacheMutex);
                SwapCacheLocked()["looks"][lookKey] = {{"look", look}, {"sex", sexNew}};
                SaveSwapCacheLocked();
            }
        }
        if (see && needClip) {
            SetWritePhase("Watching the clip...");
            const double len = std::clamp(j.opt.seconds, 1.0, 15.0);
            const double at[4] = {0.1, len * 0.33, len * 0.66, std::max(0.2, len - 0.3)};
            std::vector<std::string> images;
            std::string frames;
            for (int f = 0; f < 4; ++f) {
                const std::wstring png = ClipFrame(clip, at[f], f);
                const std::string url = png.empty() ? std::string() : ImageDataUrl(png, 640);
                if (url.empty()) continue;
                images.push_back(url);
                char buf[48];
                snprintf(buf, sizeof(buf), "%s%.1f s", frames.empty() ? "" : ", ", at[f]);
                frames += buf;
            }
            std::string out, err;
            const std::string user = j.idea.empty() ? std::string("Describe the clip.") : "Describe the clip. The user says (who to replace, or what to change): " + j.idea;
            if (!images.empty() && WriterChat(ReplaceAll(kSwapClipSystem, "{FRAMES}", frames + "."), user, images, 400, 0.3, out, &err)) {
                target = SwapField(out, "TARGET:");
                motion = NoPronouns(SwapField(out, "MOTION:"));
                sexOld = SwapField(out, "SEX:");
            } else if (note.empty()) {
                note = "the prompt writer did not answer" + (err.empty() ? std::string() : ": " + err);
            }
            if (!target.empty()) {
                std::lock_guard<std::mutex> lock(g_swapCacheMutex);
                SwapCacheLocked()["clips"][clipKey] = {{"target", target}, {"motion", motion}, {"sex", sexOld}};
                SaveSwapCacheLocked();
            }
        }
        WriterRelease(session, false);
        SetWritePhase("");
    }
    if (!j.replaces.empty() && !j.replaces[0].empty()) { // chosen on its card
        target = StripSex(j.replaces[0]);
        if (j.replaces[0].find("(woman)") != std::string::npos) sexOld = "woman";
        else if (j.replaces[0].find("(man)") != std::string::npos) sexOld = "man";
    }
    if (look.size() < 20 && j.context.size() >= 20) look = j.context, note.clear(); // a saved character's description
    if (look.size() < 20 && note.empty()) note = "the prompt writer wrote no description of the person: the swap prompt says only \"as in the picture\"";
    return SwapPrompt(look.size() < 20 ? std::string() : look, target, motion, j, sexNew + " " + sexOld);
}

// Whom each named person replaces, read straight from the user's words - "replace the girl with Ava and the boy with Ben", "swap the man
// for Ben", "Ava takes the woman's place and Ben the man's", "Ava as the dancer", "the girl becomes Ava" - so the names are never left to
// the writer (it once gave back "Replace Ava with <Subject 1>"). A phrase is matched to the people found in the clip when it can be
// ("the girl" -> "the girl in the pink floral top (woman)"). "" where the words do not say.
std::vector<std::string> ReplacesFromIdea(const std::string& idea, const std::vector<std::string>& names, const std::vector<std::string>& found) {
    std::vector<std::string> out(names.size());
    std::string low = " " + idea + " ";
    for (char& c : low) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    for (char& c : low)
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    auto word = [&](size_t at, size_t len) { return at > 0 && !isalnum(static_cast<unsigned char>(low[at - 1])) && at + len < low.size() && !isalnum(static_cast<unsigned char>(low[at + len])); };
    auto findName = [&](const std::string& nm, size_t from) {
        for (size_t at = low.find(nm, from); at != std::string::npos; at = low.find(nm, at + 1))
            if (word(at, nm.size())) return at;
        return std::string::npos;
    };
    std::vector<std::string> lnames;
    for (const std::string& n : names) {
        std::string l = Trimmed(n);
        for (char& c : l) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        lnames.push_back(l);
    }
    auto clean = [&](std::string p) { // trims, cuts at the next clause, leaves "the girl in red"
        for (const char* stop : {",", ".", ";", "!", "?", " and ", " then ", " while ", " but ", " with ", " for ", " by "}) {
            const size_t at = p.find(stop);
            if (at != std::string::npos) p = p.substr(0, at);
        }
        for (const std::string& nm : lnames) { // never another name
            const size_t at = nm.size() >= 2 ? p.find(nm) : std::string::npos;
            if (at != std::string::npos) p = p.substr(0, at);
        }
        p = Trimmed(p);
        for (const char* tail : {"'s place", "'s role", "'s spot", " place", " role", "'s"}) {
            const std::string t = tail;
            if (p.size() > t.size() && p.compare(p.size() - t.size(), t.size(), t) == 0) p = Trimmed(p.substr(0, p.size() - t.size()));
        }
        for (const char* lead : {"replace ", "swap out ", "swap ", "change ", "switch ", "turn "}) {
            if (p.rfind(lead, 0) == 0) p = Trimmed(p.substr(strlen(lead)));
        }
        if (p.rfind("out ", 0) == 0) p = Trimmed(p.substr(4));
        if (p.size() < 2 || p.size() > 80) return std::string();
        if (p.rfind("the ", 0) != 0 && p.rfind("a ", 0) != 0 && p.rfind("an ", 0) != 0) p = "the " + p;
        return p;
    };
    for (size_t k = 0; k < lnames.size(); ++k) {
        const std::string& nm = lnames[k];
        if (nm.size() < 2) continue;
        const size_t at = findName(nm, 0);
        if (at == std::string::npos) continue;
        std::string got;
        // before the name: "... the girl with|for|by NAME"
        for (const char* link : {" with ", " for ", " by ", " becomes ", " turns into ", " into ", " -> ", " = "}) {
            const std::string l = link;
            if (at >= l.size() && low.compare(at - l.size(), l.size(), l) == 0) {
                size_t from = 0;
                for (const char* start : {"replace ", "swap ", "change ", "switch ", " and ", ", ", "; ", ". ", " then "}) {
                    const size_t s = low.rfind(start, at - l.size());
                    if (s != std::string::npos && s + strlen(start) > from && s + strlen(start) <= at - l.size()) from = s + strlen(start);
                }
                for (const std::string& other : lnames) { // "... with Ava and the boy with Ben": after the previous name
                    if (other == nm || other.size() < 2) continue;
                    const size_t o = findName(other, 0);
                    if (o != std::string::npos && o < at - l.size() && o + other.size() > from) from = o + other.size();
                }
                got = clean(low.substr(from, at - l.size() - from));
                break;
            }
        }
        // after the name: "NAME replaces|takes the place of|instead of|as|plays|in place of the X", "NAME takes the X's place", "NAME the X's"
        if (got.empty()) {
            std::string rest = low.substr(at + nm.size());
            for (const char* link : {" replaces ", " takes the place of ", " in place of ", " instead of ", " takes over from ", " takes ", " as ", " plays ", " is ", " becomes ", " "}) {
                const std::string l = link;
                if (rest.rfind(l, 0) == 0) {
                    std::string p = rest.substr(l.size());
                    if (l == " " && p.rfind("the ", 0) != 0) continue; // "Ben the man's" only
                    got = clean(p);
                    break;
                }
            }
        }
        if (got.empty()) continue;
        // the closest of the people found in the clip, by shared words (beyond "the")
        std::string pick = got;
        int best = 0;
        for (const std::string& f : found) {
            std::string lf = StripSex(f); // (the "(man)" tag is no word of the description: "the man" found the boy by it)
            for (char& c : lf) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            int score = 0;
            std::istringstream in(got);
            for (std::string w; in >> w;) {
                if (w == "the" || w == "a" || w == "an" || w == "in" || w == "on" || w == "of" || w.size() < 3) continue;
                size_t p = lf.find(w);
                while (p != std::string::npos && !((p == 0 || !isalpha(static_cast<unsigned char>(lf[p - 1]))) && (p + w.size() >= lf.size() || !isalpha(static_cast<unsigned char>(lf[p + w.size()])))))
                    p = lf.find(w, p + 1);
                if (p != std::string::npos) score += 2;
            }
            if (score > best) best = score, pick = f;
        }
        // only a phrase about a person ("a video with Ava dancing" names no one to replace)
        static const char* const people[] = {"woman", "man", "girl", "boy", "lady", "guy", "person", "dancer", "singer", "kid", "child", "teen", "one", "student",
                                             "player", "rapper", "dude", "actor", "actress", "performer", "waiter", "waitress", "driver", "officer", "chef"};
        bool personish = best > 0;
        for (const char* w : people) {
            const size_t at2 = got.find(w);
            if (at2 != std::string::npos && (at2 == 0 || !isalpha(static_cast<unsigned char>(got[at2 - 1])))) personish = true;
        }
        if (!personish) continue;
        // "the woman" also means one found as "(woman)", "the man" one found as "(man)", when nothing else matched
        if (best == 0) {
            const bool woman = got.find("woman") != std::string::npos || got.find("girl") != std::string::npos || got.find("lady") != std::string::npos;
            const bool man = !woman && (got.find(" man") != std::string::npos || got.find("boy") != std::string::npos || got.find("guy") != std::string::npos);
            int count = 0;
            std::string only;
            for (const std::string& f : found)
                if ((woman && f.find("(woman)") != std::string::npos) || (man && f.find("(man)") != std::string::npos)) ++count, only = f;
            if (count == 1) pick = only;
        }
        out[k] = pick;
    }
    // two names on one person: the words were not understood - leave both to the writer
    for (size_t a = 0; a < out.size(); ++a)
        for (size_t b = a + 1; b < out.size(); ++b)
            if (!out[a].empty() && out[a] == out[b]) out[a].clear(), out[b].clear();
    return out;
}

int CountCuts(const std::wstring& clip); // (below: the masked swap)
bool DecodeRaw(const std::wstring& video, int frames, int w, int h, bool gray, std::vector<uint8_t>& out); // (below)

// A swap's picture as it goes in: the person alone, cut out with SAM 3 and put on white, cropped round them. Why: what the picture shows
// around the person comes back in the swap - the grass behind Mickey grew in the room the mask leaves round the new person, and a selfie's
// concert hall would too; the prompt writer's look is also of the person alone then. The biggest person is kept (a selfie had someone small
// far behind). A picture already on white (Create a character's studio pictures) is left as it is. Cached by the picture. "" = as it was.
std::wstring CleanReference(const std::wstring& pic, const std::string& comfyUrl, const std::string& sam3) {
    if (sam3.empty() || pic.empty()) return {};
    const std::wstring dir = DataDir() + L"\\h3\\refs";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::wstring out = dir + L"\\clean_" + HashName(FileKey(pic)) + L".png";
    if (fs::exists(out, ec)) return out;
    { // a white border already: a studio picture
        std::vector<uint8_t> thumb;
        if (DecodeRaw(pic, 1, 64, 64, true, thumb) && thumb.size() >= 64 * 64) {
            double sum = 0;
            int n = 0;
            for (int i = 0; i < 64; ++i) sum += thumb[i] + thumb[63 * 64 + i] + thumb[i * 64] + thumb[i * 64 + 63], n += 4;
            if (sum / n > 232) return {};
        }
    }
    SetWritePhase("Cutting the person out of their picture (SAM 3)...");
    json g;
    g["1"] = {{"class_type", "CheckpointLoaderSimple"}, {"inputs", {{"ckpt_name", sam3}}}};
    g["2"] = {{"class_type", "CLIPTextEncode"}, {"inputs", {{"clip", json::array({"1", 1})}, {"text", "person"}}}};
    g["3"] = {{"class_type", "LoadImage"}, {"inputs", {{"image", "example.png"}}}};
    g["4"] = {{"class_type", "SAM3_Detect"}, {"inputs", {{"model", json::array({"1", 0})}, {"image", json::array({"3", 0})}, {"conditioning", json::array({"2", 0})},
                                                         {"threshold", 0.4}, {"refine_iterations", 2}, {"individual_masks", true}}}};
    g["5"] = {{"class_type", "MaskToImage"}, {"inputs", {{"mask", json::array({"4", 0})}}}};
    g["6"] = {{"class_type", "SaveImage"}, {"inputs", {{"images", json::array({"5", 0})}, {"filename_prefix", "refs/person"}}}};
    std::vector<std::wstring> got;
    std::string err;
    if (!ComfyRunGraph(comfyUrl, g.dump(), {{"3", pic}}, dir, got, err, 300) || got.empty()) return {};
    // the biggest person, and where they are
    std::wstring best;
    size_t bestArea = 0;
    int W = 0, H = 0, x0 = 0, y0 = 0, x1 = -1, y1 = -1;
    for (const std::wstring& m : got) {
        const VideoInfo vi = ProbeVideoFfmpeg(m);
        std::vector<uint8_t> px;
        if (vi.width <= 0 || vi.height <= 0 || !DecodeRaw(m, 1, vi.width, vi.height, true, px) || px.size() < static_cast<size_t>(vi.width) * vi.height) continue;
        size_t area = 0;
        int a0 = vi.width, b0 = vi.height, a1 = -1, b1 = -1;
        for (int y = 0; y < vi.height; ++y)
            for (int x = 0; x < vi.width; ++x)
                if (px[static_cast<size_t>(y) * vi.width + x] > 127) ++area, a0 = std::min(a0, x), a1 = std::max(a1, x), b0 = std::min(b0, y), b1 = std::max(b1, y);
        if (area > bestArea) bestArea = area, best = m, W = vi.width, H = vi.height, x0 = a0, y0 = b0, x1 = a1, y1 = b1;
    }
    if (best.empty() || bestArea < static_cast<size_t>(W) * H / 50) { // nobody, or only someone tiny
        for (const std::wstring& m : got) fs::remove(m, ec);
        return {};
    }
    // cropped round them (a margin of 12%), on white
    const int padX = (x1 - x0) * 12 / 100 + 8, padY = (y1 - y0) * 12 / 100 + 8;
    const int cx = std::max(0, x0 - padX) / 2 * 2, cy = std::max(0, y0 - padY) / 2 * 2;
    const int cw = (std::min(W, x1 + padX) - cx) / 2 * 2, ch = (std::min(H, y1 + padY) - cy) / 2 * 2;
    char fc[400];
    snprintf(fc, sizeof(fc), "[1:v]format=gray,scale=%d:%d[m];[0:v]scale=%d:%d,format=rgba[f];[f][m]alphamerge[a];color=white:s=%dx%d[bg];[bg][a]overlay=shortest=1,crop=%d:%d:%d:%d",
             W, H, W, H, W, H, cw, ch, cx, cy);
    std::string log;
    RunCaptureHidden(L"\"" + FfmpegPath() + L"\" -y -v error -i \"" + pic + L"\" -i \"" + best + L"\" -filter_complex \"" + WideOf(fc) + L"\" -frames:v 1 \"" + out + L"\"", log, true);
    for (const std::wstring& m : got) fs::remove(m, ec);
    return fs::exists(out, ec) && fs::file_size(out, ec) > 1000 ? out : std::wstring();
}

WriteResult RunWriteJob(WriteJob j, std::string comfyUrl) {
    WriteResult r;
    StepLog("prompt job start: " + j.label);
    struct Done { std::string label; ~Done() { StepLog("prompt job done: " + label); } } done{j.label};
    // each person alone on white, before the writer looks at them (Recreate too: a selfie's popcorn box and theatre sign came into the stage dance)
    if (j.kind == 1 && !j.sam3.empty()) {
        bool any = false;
        for (std::wstring& w : j.who) {
            const std::wstring c = CleanReference(w, comfyUrl, j.sam3);
            if (!c.empty()) w = c, any = true;
        }
        if (any) r.who = j.who;
    }
    if (j.kind == 1) {
        r.clip = PrepareClip(j.refVideo, j.refStart, FramesFor(j.opt.seconds) / 24.0, std::min(kRes[std::clamp(j.opt.res, 0, 3)].w, kRes[std::clamp(j.opt.res, 0, 3)].h), r.note);
        if (j.starMethod == 0 && !r.clip.empty()) CountCuts(r.clip); // (kept: the send asks again)
        if (j.starMethod == 0 && !j.idea.empty() && !j.names.empty()) { // whom the user's words name for each person (the cards' choice wins)
            std::vector<std::string> found;
            {
                std::lock_guard<std::mutex> lock(g_swapCacheMutex);
                const json& c = SwapCacheLocked();
                const std::string pk = PeopleKey(j.refVideo, j.refStart, j.opt.seconds);
                if (c.contains("people") && c["people"].contains(pk) && c["people"][pk].is_array()) found = c["people"][pk].get<std::vector<std::string>>();
            }
            // a hint to the writer, not the target itself: its description by look and place ("the girl in the pink floral top carrying a
            // tray") is what finds the person in every shot; a bare "the girl" made the person masks pick wrong people (tested)
            j.said = ReplacesFromIdea(j.idea, j.names, found);
            for (size_t k = 0; k < j.said.size() && k < j.replaces.size(); ++k)
                if (!j.replaces[k].empty()) j.said[k].clear();
            static const bool dump = GetEnvironmentVariableW(L"MV_DUMP_GRAPH", nullptr, 0) > 0; // tests: what was read
            if (dump) {
                std::ofstream f(DataDir() + L"\\idea_replaces.txt", std::ios::binary);
                for (size_t k = 0; k < j.names.size(); ++k) f << j.names[k] << " -> " << (k < j.said.size() ? j.said[k] : std::string()) << "\n";
            }
        }
        r.text = !j.write ? j.prompt : j.starMethod == 0 ? WriteSwapPrompt(j, r.clip, comfyUrl, r.note) : WriteStarPrompt(j, r.clip, comfyUrl, r.note);
        return r;
    }
    r.text = WriteVideoPrompt(j, comfyUrl);
    std::lock_guard<std::mutex> lock(g_phaseMutex);
    r.note = g_writeNote;
    return r;
}

// The star workflow: the ref2va model, the pictures and the clip (and its sound) as references
json BuildStar(const WriteJob& j, int w, int h, bool haveLora, const std::string& lora, bool omni) {
    json wf;
    auto node = [&](const std::string& id, const char* cls, json inputs) { wf[id] = {{"class_type", cls}, {"inputs", std::move(inputs)}, {"_meta", {{"title", cls}}}}; };
    omni = omni && j.starQuality == 0;
    const bool fast = j.starQuality == 0 && haveLora && !omni;
    node("1", "UNETLoader", {{"unet_name", omni ? std::string(kOmniModel) : StarUnet()}, {"weight_dtype", "default"}});
    json model = json::array({"1", 0});
    if (omni) { // its own settings: shift 12/3 (not V2's 10), 8 steps, cfg 1
        node("2", "MiniMaxH3SigmaShift", {{"model", model}, {"shift_video", 12.0}, {"shift_audio", 3.0}});
        model = json::array({"2", 0});
    }
    if (fast) {
        node("2", "LoraLoaderModelOnly", {{"model", model}, {"lora_name", lora}, {"strength_model", 1.0}});
        model = json::array({"2", 0});
    }
    if (FizgigOn()) {
        node("23", "FizgigH3Tweaks", FizgigNode(model));
        model = json::array({"23", 0});
    }
    if (j.opt.veda) {
        node("3", "VedaSparseAttention", VedaNode(model, kVedaStarPredictor));
        model = json::array({"3", 0});
    }
    node("4", "CLIPLoader", {{"clip_name", kTextEncoder}, {"type", "minimax"}, {"device", "default"}});
    node("5", "VAELoader", {{"vae_name", kVideoVae}});
    node("6", "VAELoader", {{"vae_name", kAudioVae}});
    json r2v = {{"clip", json::array({"4", 0})}, {"vae", json::array({"5", 0})}, {"audio_vae", json::array({"6", 0})}, {"prompt", ""},
                {"width", w}, {"height", h}, {"length", FramesFor(j.opt.seconds)}, {"ref_image_size", "match"}};
    for (size_t k = 0; k < j.who.size() && k < 9; ++k) {
        const std::string id = std::to_string(30 + k);
        node(id, "LoadImage", {{"image", "example.png"}});
        r2v["ref_images.ref_image_" + std::to_string(k)] = json::array({id, 0});
    }
    node("40", "LoadVideo", {{"file", "example.mp4"}});
    node("41", "GetVideoComponents", {{"video", json::array({"40", 0})}});
    r2v["ref_videos.ref_video_0"] = json::array({"41", 0});
    if (j.keepSound) r2v["ref_video_audios.ref_video_audio_0"] = json::array({"41", 1});
    node("8", "MiniMaxH3ReferenceToVideo", r2v);
    node("11", "RandomNoise", {{"noise_seed", 42}});
    node("12", "KSamplerSelect", {{"sampler_name", omni ? "euler" : "res_multistep"}});
    // the turbo LoRA at its step count, Omni's 8; without them the model's own 20 steps (beta suits reference-heavy prompts)
    const int steps = omni ? 8 : fast ? (lora.find("8step") != std::string::npos ? 8 : 4) : 20;
    node("13", "BasicScheduler", {{"model", model}, {"scheduler", fast || omni ? "simple" : "beta"}, {"steps", steps}, {"denoise", 1.0}});
    node("14", "BasicGuider", {{"model", model}, {"conditioning", json::array({"8", 0})}});
    node("15", "SamplerCustomAdvanced", {{"noise", json::array({"11", 0})}, {"guider", json::array({"14", 0})}, {"sampler", json::array({"12", 0})},
                                         {"sigmas", json::array({"13", 0})}, {"latent_image", json::array({"8", 1})}});
    node("16", "VAEDecode", {{"samples", json::array({"15", 0})}, {"vae", json::array({"5", 0})}});
    node("17", "VAEDecodeAudio", {{"samples", json::array({"15", 0})}, {"vae", json::array({"6", 0})}});
    node("18", "CreateVideo", {{"images", json::array({"16", 0})}, {"audio", json::array({"17", 0})}, {"fps", 24.0}});
    node("19", "SaveVideo", {{"video", json::array({"18", 0})}, {"filename_prefix", "video/H3_Star"}, {"format", "auto"}, {"format.codec", "auto"}});
    return wf;
}

// The swap workflow (wangweihong's 05_Turbo+VEDA_4step, native nodes): ref2va + the Character Swap LoRA (+ the ref2v turbo LoRA), shift 12/3,
// INT8 attention where ComfyUI has it, Veda's R2VA predictor; the picture as <Picture 1>, the clip's frames as a guide at frame 0 (AddGuide
// crops them to the video's size and to 17k + 5 frames itself); the clip's own sound, or the generated one.
json BuildSwap(const WriteJob& j, int w, int h, const std::string& turbo, bool chunkFfn) {
    json wf;
    auto node = [&](const std::string& id, const char* cls, json inputs) { wf[id] = {{"class_type", cls}, {"inputs", std::move(inputs)}, {"_meta", {{"title", cls}}}}; };
    const bool fast = j.starQuality == 0 && !turbo.empty();
    const int frames = FramesFor(j.opt.seconds);
    node("1", "UNETLoader", {{"unet_name", StarUnet()}, {"weight_dtype", "default"}});
    node("2", "LoraLoaderModelOnly", {{"model", json::array({"1", 0})}, {"lora_name", kSwapLora}, {"strength_model", 1.0}});
    json model = json::array({"2", 0});
    if (fast) {
        node("3", "LoraLoaderModelOnly", {{"model", model}, {"lora_name", turbo}, {"strength_model", 1.0}});
        model = json::array({"3", 0});
    }
    if (FizgigOn()) {
        node("54", "FizgigH3Tweaks", FizgigNode(model));
        model = json::array({"54", 0});
    }
    node("50", "MiniMaxH3SigmaShift", {{"model", model}, {"shift_video", 12.0}, {"shift_audio", 3.0}});
    node("51", "ModelAttentionBackend", {{"model", json::array({"50", 0})}, {"attention", "comfy kitchen attention"}}); // falls back to PyTorch by itself
    model = json::array({"51", 0});
    if (chunkFfn) { // KJNodes: lower peak VRAM on a 24 GB card
        node("52", "MiniMaxChunkFeedForward", {{"model", model}, {"chunks", 8}, {"seq_threshold", 4096}});
        model = json::array({"52", 0});
    }
    if (j.opt.veda) {
        node("53", "VedaSparseAttention", VedaNode(model, kVedaStarPredictor));
        model = json::array({"53", 0});
    }
    node("4", "CLIPLoader", {{"clip_name", kTextEncoder}, {"type", "minimax"}, {"device", "default"}});
    node("5", "VAELoader", {{"vae_name", kVideoVae}});
    node("6", "VAELoader", {{"vae_name", kAudioVae}});
    json r2v = {{"clip", json::array({"4", 0})}, {"vae", json::array({"5", 0})}, {"audio_vae", json::array({"6", 0})}, {"prompt", ""},
                {"width", w}, {"height", h}, {"length", frames}, {"ref_image_size", "match"}};
    for (size_t k = 0; k < j.who.size() && k < 9; ++k) {
        const std::string id = std::to_string(30 + k);
        node(id, "LoadImage", {{"image", "example.png"}});
        r2v["ref_images.ref_image_" + std::to_string(k)] = json::array({id, 0});
    }
    node("8", "MiniMaxH3ReferenceToVideo", r2v);
    node("40", "LoadVideo", {{"file", "example.mp4"}});
    node("41", "GetVideoComponents", {{"video", json::array({"40", 0})}});
    node("42", "ImageFromBatch", {{"image", json::array({"41", 0})}, {"batch_index", 0}, {"length", frames}}); // never longer than the video
    node("9", "MiniMaxH3AddGuide", {{"positive", json::array({"8", 0})}, {"latent", json::array({"8", 1})}, {"vae", json::array({"5", 0})},
                                    {"image", json::array({"42", 0})}, {"frame_idx", 0}});
    // (tried 2026-10-22: the clip's soundtrack anchored here too, so the model hears the singing - the new person's mouth still stayed shut;
    // the mouth comes from the faces pass, LivePortrait, instead)
    node("11", "RandomNoise", {{"noise_seed", 42}});
    node("12", "KSamplerSelect", {{"sampler_name", fast ? "euler" : "er_sde"}});
    const int steps = fast ? (turbo.find("8step") != std::string::npos ? 8 : 4) : 16;
    node("13", "BasicScheduler", {{"model", model}, {"scheduler", "simple"}, {"steps", steps}, {"denoise", 1.0}});
    node("14", "BasicGuider", {{"model", model}, {"conditioning", json::array({"9", 0})}});
    node("15", "SamplerCustomAdvanced", {{"noise", json::array({"11", 0})}, {"guider", json::array({"14", 0})}, {"sampler", json::array({"12", 0})},
                                         {"sigmas", json::array({"13", 0})}, {"latent_image", json::array({"8", 1})}});
    node("16", "VAEDecode", {{"samples", json::array({"15", 0})}, {"vae", json::array({"5", 0})}});
    json audio;
    if (j.keepSound) {
        node("17", "TrimAudioDuration", {{"audio", json::array({"41", 1})}, {"start_index", 0.0}, {"duration", frames / 24.0}});
    } else {
        node("17", "VAEDecodeAudio", {{"samples", json::array({"15", 0})}, {"vae", json::array({"6", 0})}});
    }
    node("18", "CreateVideo", {{"images", json::array({"16", 0})}, {"audio", json::array({"17", 0})}, {"fps", 24.0}});
    node("19", "SaveVideo", {{"video", json::array({"18", 0})}, {"filename_prefix", "video/H3_Swap"}, {"format", "auto"}, {"format.codec", "auto"}});
    return wf;
}

// The turbo LoRA a swap uses: a 4-step one first (lightx2v's, then Comfy-Org's v0.1), else the 8-step one
std::string SwapTurbo() {
    for (const char* l : {kSwapTurbo, kRefLoras[2], kRefLoras[1]}) {
        if (HasLora(l)) return l;
    }
    return {};
}

// ---- The masked swap: SAM 3 finds the person in every frame, and only that part of the clip is made again; everything else is the clip's own
// pixels. Tested 2026-10-17 (RTX 3090, 640p, 5 s, a man into a woman's dance, same seed): against the guide swap the background stayed the
// clip's (PSNR outside the people 35-39 dB against 22), the new body followed the dancer's outline better (mask overlap 0.73-0.90 against 0.68),
// and the likeness was about the same (0.79-0.82). Across a hard cut the in-graph track failed (someone who only appears after a cut got no
// mask and stayed the original), so the people are now found shot by shot first (BuildPeopleMasks, below); the in-graph track is left for
// when the prompt writer cannot tell the people apart, and then only for a part without cuts.
// (github.com/drozbay/MaskVidExperiments: the mask cleanup, and the pixel-to-latent reduction that knows H3's frame grouping.)
std::mutex g_cutsMutex;
std::map<std::wstring, int> g_cuts; // clip part -> hard cuts in it (-1 = could not be looked at)

int CountCuts(const std::wstring& clip) {
    {
        std::lock_guard<std::mutex> lock(g_cutsMutex);
        auto it = g_cuts.find(clip);
        if (it != g_cuts.end()) return it->second;
    }
    int n = -1;
    const std::wstring ff = FfmpegPath();
    if (!ff.empty()) {
        std::string out;
        RunCaptureIdle(L"\"" + ff + L"\" -hide_banner -threads 2 -i \"" + clip + L"\" -an -vf \"select='gt(scene,0.4)',metadata=print:file=-\" -f null -", out, false);
        n = 0;
        for (size_t at = out.find("lavfi.scene_score"); at != std::string::npos; at = out.find("lavfi.scene_score", at + 10)) ++n;
    }
    std::lock_guard<std::mutex> lock(g_cutsMutex);
    g_cuts[clip] = n;
    return n;
}

// Whom each new person replaces, from the prompt's own summary ("Replace the girl carrying a tray with <Subject 1> and the boy ... with
// <Subject 2> in the aligned guide"; one person: "Replace the dancer in the aligned guide with <Subject 1>"). "" where it cannot be read.
std::vector<std::string> MaskPhrases(const std::string& prompt, size_t people) {
    std::vector<std::string> out(std::max<size_t>(1, people));
    std::string low = prompt;
    for (char& c : low) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    const size_t a = low.find("replace ");
    const size_t b = low.find(" in the aligned guide", a == std::string::npos ? 0 : a);
    if (a == std::string::npos || b == std::string::npos || b <= a) return out;
    const std::string summary = low.substr(a + 8, b - a - 8);
    if (summary.find(" with <subject 1>") == std::string::npos) { // the one-person form
        out[0] = summary;
        return out;
    }
    for (size_t k = 0; k < out.size(); ++k) {
        const std::string tag = " with <subject " + std::to_string(k + 1) + ">";
        const size_t t = summary.find(tag);
        if (t == std::string::npos) break;
        size_t from = 0; // the phrase starts after the previous subject (", " or " and ")
        if (k > 0) {
            const size_t prev = summary.find("<subject " + std::to_string(k) + ">");
            if (prev != std::string::npos) from = summary.find('>', prev) + 1;
        }
        std::string phrase = summary.substr(from, t - from);
        for (const char* sep : {" and ", ", "}) {
            if (phrase.rfind(sep, 0) == 0) phrase = phrase.substr(strlen(sep));
        }
        out[k] = phrase;
    }
    return out;
}

// What SAM 3 looks for (the masks made inside the swap, when the prompt writer cannot tell the people apart): one short noun per person -
// SAM 3 finds "girl" far better than a whole sentence
std::vector<std::string> MaskTargets(const std::string& prompt, size_t people) {
    std::vector<std::string> out;
    static const char* const nouns[] = {"woman", "man", "girl", "boy", "lady", "guy", "child", "kid", "teenager", "dancer", "singer", "rapper", "player", "person"};
    auto noun = [&](const std::string& phrase) {
        size_t best = std::string::npos;
        std::string pick = "person";
        for (const char* n : nouns) {
            const std::string w = n;
            for (size_t at = phrase.find(w); at != std::string::npos; at = phrase.find(w, at + 1)) {
                const bool start = at == 0 || !isalpha(static_cast<unsigned char>(phrase[at - 1]));
                const bool end = at + w.size() >= phrase.size() || !isalpha(static_cast<unsigned char>(phrase[at + w.size()]));
                if (start && end && at < best) best = at, pick = w;
                if (start && end) break;
            }
        }
        return pick;
    };
    for (const std::string& phrase : MaskPhrases(prompt, people)) out.push_back(phrase.empty() ? std::string("person") : noun(phrase));
    return out;
}

// The phrases with the clip person's sex, for the prompt writer to find them by ("the girl carrying a tray (woman)")
std::vector<std::string> PeopleToFind(const std::string& prompt, size_t people) {
    std::vector<std::string> out = MaskPhrases(prompt, people);
    const std::vector<std::string> nouns = MaskTargets(prompt, people);
    for (size_t k = 0; k < out.size(); ++k) {
        if (out[k].empty()) out[k] = k == 0 ? "the most prominent person" : "the next most prominent person";
        const std::string& nn = k < nouns.size() ? nouns[k] : std::string();
        if (nn == "woman" || nn == "girl" || nn == "lady") out[k] += " (woman)";
        else if (nn == "man" || nn == "boy" || nn == "guy") out[k] += " (man)";
    }
    return out;
}

// ---- Who is where, shot by shot (the masked swap's people). One SAM 3 "person" track per shot (a hard cut starts a new one: ffmpeg's scene
// change, finer than before - a cut it missed at 0.4 killed every track at once), room for 10 people in each so a person who walks back in is
// picked up again; the prompt writer then sees one frame per shot with every person outlined in a colour and says which colour each new
// person replaces (asked to describe each outline, and what they do, first: without that it took a seated girl for "the boy at the table", and
// a man walking past in a dark top for him). The chosen masks are mended in the app: a frame where someone vanished or shrank takes the
// masks on both sides, what moved right next to them is added (a fast hand blurs past SAM 3), and each frame takes its two neighbours' masks
// too. Tested 2026-10-17 on the cafeteria clip (149
// frames, 8 shots): every shot labelled right; the old one-track mask left the boy the original in 62 frames, this one in none, the
// background outside the people stayed the clip's (35.6 dB against 29.5 for the guide swap).
struct PeopleMask {
    bool ok = false;
    std::wstring video; // black/white mp4 at the analysis size, one frame per clip frame
    std::string note;   // why it could not be made
    int shots = 0, labelled = 0;
    double coverage = 0; // frames with someone masked
    std::string timeline; // who is on screen in each shot ("0.0-1.3 s: <Subject 1> in the centre; ..."), for the prompt
    std::vector<int> bounds; // first frame of each shot, then the end
    int w = 0, h = 0;        // the masks' size
    std::vector<WhoBox> boxes;
    std::vector<std::wstring> videos;    // each person's own masks (one person at a time)
    std::vector<std::wstring> protects;  // for each person's pass: the other chosen people, never to be painted over
    std::vector<std::string> timelines;  // each person's own shots, as <Subject 1> ("0.0-1.2 s: <Subject 1> on the right, smiling; ...")
};
std::mutex g_peopleMaskMutex;
std::map<std::string, PeopleMask> g_peopleMasks; // key: clip, frames, what is looked for

// Frame numbers where a new shot starts
std::vector<int> ShotCuts(const std::wstring& clip, int frames) {
    std::vector<int> cuts;
    const std::wstring ff = FfmpegPath();
    if (ff.empty()) return cuts;
    std::string out;
    RunCaptureIdle(L"\"" + ff + L"\" -hide_banner -threads 2 -i \"" + clip + L"\" -an -vf \"select='gt(scene,0.25)',showinfo\" -f null -", out, true);
    for (size_t at = out.find("pts_time:"); at != std::string::npos; at = out.find("pts_time:", at + 9)) {
        const int f = static_cast<int>(std::lround(atof(out.c_str() + at + 9) * 24.0));
        if (f >= 3 && f <= frames - 3 && (cuts.empty() || f - cuts.back() >= 3)) cuts.push_back(f);
    }
    return cuts;
}

// A video's frames as raw bytes (gray or rgb24) at w x h (scaled to cover and centre-cropped, as ImageScale "center" does)
bool DecodeRaw(const std::wstring& video, int frames, int w, int h, bool gray, std::vector<uint8_t>& out) {
    const std::wstring ff = FfmpegPath();
    if (ff.empty()) return false;
    const std::wstring raw = video + (gray ? L".gray" : L".rgb");
    wchar_t vf[160];
    swprintf(vf, 160, L"scale=%d:%d:force_original_aspect_ratio=increase:flags=lanczos,crop=%d:%d", w, h, w, h);
    std::string log;
    RunCaptureIdle(L"\"" + ff + L"\" -y -v error -i \"" + video + L"\" -frames:v " + std::to_wstring(frames) + L" -vf \"" + vf + L"\" -f rawvideo -pix_fmt " +
                       (gray ? L"gray" : L"rgb24") + L" \"" + raw + L"\"",
                   log, true);
    std::ifstream f(raw, std::ios::binary);
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    f.close();
    std::error_code ec;
    fs::remove(raw, ec);
    return !out.empty() && out.size() % (static_cast<size_t>(w) * h * (gray ? 1 : 3)) == 0;
}

// Binary dilation by a (2rx+1) x (2ry+1) box, with running sums (fast at any size)
void DilateMaskXY(const uint8_t* in, uint8_t* out, int w, int h, int rx, int ry) {
    std::vector<int> rows(static_cast<size_t>(w) * h), sum(std::max(w, h) + 1);
    for (int y = 0; y < h; ++y) {
        sum[0] = 0;
        for (int x = 0; x < w; ++x) sum[x + 1] = sum[x] + (in[y * w + x] ? 1 : 0);
        for (int x = 0; x < w; ++x) rows[y * w + x] = sum[std::min(w, x + rx + 1)] - sum[std::max(0, x - rx)] > 0;
    }
    for (int x = 0; x < w; ++x) {
        sum[0] = 0;
        for (int y = 0; y < h; ++y) sum[y + 1] = sum[y] + rows[y * w + x];
        for (int y = 0; y < h; ++y) out[y * w + x] = sum[std::min(h, y + ry + 1)] - sum[std::max(0, y - ry)] > 0;
    }
}
void DilateMask(const uint8_t* in, uint8_t* out, int w, int h, int r) { DilateMaskXY(in, out, w, h, r, r); }

const char* const kPeopleLabelSystem =
    "The picture is one frame of a video clip. Some people in it are outlined, each in its own colour: {COLOURS}.\n"
    "The people to find (described as they look in the clip):\n{TARGETS}"
    "First, one line per outline: <colour>: <man, woman or unclear>, <their clothes and hair in a few words>, <what they are doing and where>, "
    "<facing the camera, side-on, or seen from behind>, face: <their facial expression in a few words: smiling, laughing, mouth open in surprise, "
    "talking, frowning, neutral, or not visible>. "
    "Then exactly one line per subject:\n"
    "{LINES}"
    "Give the outline colour that surrounds that person (two colours if their outline is split in two), or none if they are not outlined or "
    "not in this frame. A subject must match by sex, by clothes AND by what they are doing or where they are (sitting is not walking past); "
    "someone who only looks a little alike is none. Never give one colour to two subjects.";

// ---- Who they replace, picked on the page (2026-10-21). As soon as a clip part is chosen its people are found (SAM 3, one track of everyone
// per shot - BuildPeopleMasks' first step), shown one frame per shot with everyone outlined, and the person each new person replaces is
// clicked; the most prominent one is picked at first, and a pick is followed into the other shots by the colours of the person's clothes and
// hair (a histogram of their pixels). Why: the prompt writer told the people apart from words shot by shot ("the man in front"), and the
// test swaps went wrong - on a 2-shot clip it echoed its instructions back for the first shot (nobody masked for 7.4 s) and in the last
// 0.6 s "the man in front" was another man, so the new person appeared only at the very end. Now nothing is guessed, and what will be
// replaced is seen before anything is made.
struct ScanPerson {
    int o = 0;                         // the SAM 3 object in its shot
    double maxArea = 0;                // its biggest share of the frame
    int frames = 0;                    // frames it is in
    std::string seen;                  // per frame of the shot: '1' where it is
    float hist[72] = {};               // its colours: 64 hue/saturation/value bins and 8 greys, summing to 1
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1; // its box in the shot's shown frame (masks' size); x1 < x0: not in that frame
    std::wstring crop;                 // its clearest view, outlined (for the writer to say who it is)
};
struct PeopleScan {
    bool ok = false;
    std::string note, key;
    std::wstring tracks;                       // SAM 3's tracks: shot by shot, object by object, n * K frames at aw x ah
    int n = 0, aw = 0, ah = 0, K = 10;
    std::vector<int> bounds;                   // first frame of each shot, then the end
    std::vector<std::vector<ScanPerson>> kept; // the people worth showing, per shot
    std::vector<int> shown;                    // the frame each shot shows
    std::vector<std::wstring> png;             // that frame with everyone outlined (twice the masks' size)
    std::vector<std::vector<uint8_t>> labels;  // that frame, who is where: index into kept[s], 255 = nobody (masks' size)
    // a long part (over 15 s) is made in parts, each found on its own (its own scan); here their shots one after another
    std::vector<std::shared_ptr<PeopleScan>> parts;
    std::vector<double> partStart, partLen;    // seconds from the part's start
    std::vector<std::pair<int, int>> shotOf;   // each shot here: (part, its shot there)
};
std::string g_scanPhase;
std::string g_scanPart; // a long part: which of its parts is being looked at ("part 2 of 3"), "" = one part
std::mutex g_scanPhaseMutex;
void ScanPhase(const std::string& p) {
    if (!p.empty()) StepLog("people: " + p);
    std::lock_guard<std::mutex> lock(g_scanPhaseMutex);
    g_scanPhase = p;
}

float HistSim(const float* a, const float* b) {
    float s = 0;
    for (int i = 0; i < 72; ++i) s += std::min(a[i], b[i]);
    return s;
}

// A scan kept next to its tracks: what was found (json) and the label maps (raw), read back instead of running SAM 3 again
void SaveScan(const PeopleScan& r, const std::wstring& stem) {
    json j = {{"key", r.key}, {"tracks", Utf8(r.tracks)}, {"n", r.n}, {"aw", r.aw}, {"ah", r.ah}, {"K", r.K}, {"bounds", r.bounds}, {"shown", r.shown}};
    j["png"] = json::array();
    for (const std::wstring& p : r.png) j["png"].push_back(Utf8(p));
    j["kept"] = json::array();
    for (const auto& shot : r.kept) {
        json a = json::array();
        for (const ScanPerson& p : shot)
            a.push_back({{"o", p.o}, {"maxArea", p.maxArea}, {"frames", p.frames}, {"seen", p.seen}, {"hist", std::vector<float>(p.hist, p.hist + 72)}, {"box", {p.x0, p.y0, p.x1, p.y1}}, {"crop", Utf8(p.crop)}});
        j["kept"].push_back(a);
    }
    std::ofstream(stem + L"_meta.json", std::ios::binary) << j.dump();
    std::ofstream lb(stem + L"_labels.bin", std::ios::binary);
    for (const auto& l : r.labels) lb.write(reinterpret_cast<const char*>(l.data()), static_cast<std::streamsize>(l.size()));
}

bool LoadScan(PeopleScan& r, const std::wstring& stem) {
    std::ifstream f(stem + L"_meta.json", std::ios::binary);
    if (!f) return false;
    const json j = json::parse(f, nullptr, false);
    std::error_code ec;
    if (!j.is_object() || j.value("key", std::string()) != r.key) return false;
    r.tracks = WideOf(j.value("tracks", std::string()));
    if (!fs::exists(r.tracks, ec)) return false;
    r.n = j.value("n", 0), r.aw = j.value("aw", 0), r.ah = j.value("ah", 0), r.K = j.value("K", 10);
    r.bounds = j.value("bounds", std::vector<int>());
    r.shown = j.value("shown", std::vector<int>());
    r.png.clear();
    for (const json& p : j.value("png", json::array())) r.png.push_back(WideOf(p.get<std::string>()));
    r.kept.clear();
    for (const json& shot : j.value("kept", json::array())) {
        r.kept.emplace_back();
        for (const json& p : shot) {
            ScanPerson q;
            q.o = p.value("o", 0), q.maxArea = p.value("maxArea", 0.0), q.frames = p.value("frames", 0), q.seen = p.value("seen", std::string());
            const std::vector<float> h = p.value("hist", std::vector<float>());
            for (size_t i = 0; i < 72 && i < h.size(); ++i) q.hist[i] = h[i];
            const std::vector<int> b = p.value("box", std::vector<int>());
            if (b.size() == 4) q.x0 = b[0], q.y0 = b[1], q.x1 = b[2], q.y1 = b[3];
            q.crop = WideOf(p.value("crop", std::string()));
            r.kept.back().push_back(q);
        }
    }
    const size_t px = static_cast<size_t>(r.aw) * r.ah, shots = r.bounds.size() > 0 ? r.bounds.size() - 1 : 0;
    if (r.kept.size() != shots || r.png.size() != shots || px == 0) return false;
    std::ifstream lb(stem + L"_labels.bin", std::ios::binary);
    r.labels.assign(shots, std::vector<uint8_t>(px, 255));
    for (auto& l : r.labels)
        if (!lb.read(reinterpret_cast<char*>(l.data()), static_cast<std::streamsize>(px))) return false;
    for (const std::wstring& p : r.png)
        if (!fs::exists(p, ec)) return false;
    r.ok = true;
    return true;
}

// The people of a clip part (worker thread): the part cut as a swap cuts it, SAM 3 per shot, and for each person worth showing their colours,
// their box and a crop; one frame per shot outlined for the page
std::shared_ptr<PeopleScan> ScanPeople(std::wstring src, double start, double seconds, std::string comfyUrl, std::string sam3) {
    auto r = std::make_shared<PeopleScan>();
    char part[64];
    snprintf(part, sizeof(part), "|%.3f|%.3f", start, seconds);
    r->key = FileKey(src) + part;
    ScanPhase("Cutting the part...");
    std::string note;
    const std::wstring clip = PrepareClip(src, start, FramesFor(seconds) / 24.0, 480, note);
    const VideoInfo vi = ProbeVideo(clip);
    const int frames = FramesFor(seconds);
    const int n = std::max(1, std::min(frames, vi.seconds > 0 ? static_cast<int>(std::floor(vi.seconds * 24.0 + 0.01)) : frames));
    const int vw = vi.width > 0 ? vi.width : 480, vh = vi.height > 0 ? vi.height : 854;
    const double f = std::min(1.0, 512.0 / std::max(vw, vh));
    const int aw = std::max(64, static_cast<int>(vw * f) / 8 * 8), ah = std::max(64, static_cast<int>(vh * f) / 8 * 8);
    const size_t px = static_cast<size_t>(aw) * ah;
    const int K = r->K;
    const std::wstring dir = DataDir() + L"\\h3\\masks";
    const std::wstring stem = dir + L"\\scan_" + HashName(r->key + "|" + std::to_string(aw) + "x" + std::to_string(ah));
    if (LoadScan(*r, stem)) { // found before
        ScanPhase("");
        return r;
    }
    r->n = n, r->aw = aw, r->ah = ah;
    r->bounds = {0};
    for (int c : ShotCuts(clip, n)) r->bounds.push_back(c);
    r->bounds.push_back(n);
    const int shots = static_cast<int>(r->bounds.size()) - 1;
    const auto& bounds = r->bounds;
    ScanPhase("Finding the people in every shot (SAM 3)...");
    json g;
    auto node = [&](const std::string& id, const char* cls, json inputs) { g[id] = {{"class_type", cls}, {"inputs", std::move(inputs)}}; };
    node("1", "LoadVideo", {{"file", "example.mp4"}});
    node("2", "GetVideoComponents", {{"video", json::array({"1", 0})}});
    node("3", "ImageFromBatch", {{"image", json::array({"2", 0})}, {"batch_index", 0}, {"length", n}});
    node("4", "ImageScale", {{"image", json::array({"3", 0})}, {"upscale_method", "lanczos"}, {"width", aw}, {"height", ah}, {"crop", "center"}});
    node("5", "CheckpointLoaderSimple", {{"ckpt_name", sam3}});
    node("6", "CLIPTextEncode", {{"clip", json::array({"5", 1})}, {"text", "person"}});
    json last;
    for (int s = 0; s < shots; ++s) {
        const std::string ss = std::to_string(s);
        node("s" + ss, "ImageFromBatch", {{"image", json::array({"4", 0})}, {"batch_index", bounds[s]}, {"length", bounds[s + 1] - bounds[s]}});
        node("t" + ss, "SAM3_VideoTrack", {{"images", json::array({"s" + ss, 0})}, {"model", json::array({"5", 0})}, {"detection_threshold", 0.4}, {"max_objects", K},
                                          {"detect_interval", 1}, {"conditioning", json::array({"6", 0})}});
        for (int o = 0; o < K; ++o) {
            const std::string so = ss + "_" + std::to_string(o);
            node("m" + so, "SAM3_TrackToMask", {{"track_data", json::array({"t" + ss, 0})}, {"object_indices", std::to_string(o)}});
            node("i" + so, "MaskToImage", {{"mask", json::array({"m" + so, 0})}});
            if (last.is_null()) {
                last = json::array({"i" + so, 0});
            } else {
                node("b" + so, "ImageBatch", {{"image1", last}, {"image2", json::array({"i" + so, 0})}});
                last = json::array({"b" + so, 0});
            }
        }
    }
    node("90", "CreateVideo", {{"images", last}, {"fps", 24.0}});
    node("91", "SaveVideo", {{"video", json::array({"90", 0})}, {"filename_prefix", "masks/people"}, {"format", "auto"}, {"format.codec", "auto"}});
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::vector<std::wstring> got;
    std::string err;
    if (!ComfyRunGraph(comfyUrl, g.dump(), {{"1", clip}}, dir, got, err, 1800) || got.empty()) {
        r->note = "SAM 3 could not look for the people: " + err;
        ScanPhase("");
        return r;
    }
    r->tracks = stem + L"_tracks.mp4";
    fs::remove(r->tracks, ec);
    fs::rename(got[0], r->tracks, ec);
    if (ec) r->tracks = got[0];
    ScanPhase("Looking at who is who...");
    std::vector<uint8_t> tracks, rgb;
    if (!DecodeRaw(r->tracks, n * K, aw, ah, true, tracks) || tracks.size() < px * n * K || !DecodeRaw(clip, n, aw, ah, false, rgb) || rgb.size() < px * 3 * n) {
        r->note = "could not read the person tracks";
        ScanPhase("");
        return r;
    }
    auto obj = [&](int s, int o, int t) {
        size_t at = 0;
        for (int q = 0; q < s; ++q) at += static_cast<size_t>(bounds[q + 1] - bounds[q]) * K;
        return tracks.data() + (at + static_cast<size_t>(o) * (bounds[s + 1] - bounds[s]) + t) * px;
    };
    auto area = [&](const uint8_t* m, int step) {
        size_t c = 0;
        for (size_t i = 0; i < px; i += step) c += m[i] > 127;
        return static_cast<double>(c) * step / px;
    };
    // the people worth showing (as BuildPeopleMasks keeps them), their colours
    r->kept.assign(shots, {});
    for (int s = 0; s < shots; ++s) {
        const int len = bounds[s + 1] - bounds[s];
        std::vector<ScanPerson> all;
        for (int o = 0; o < K; ++o) {
            ScanPerson p;
            p.o = o;
            p.seen.assign(len, '0');
            for (int t = 0; t < len; ++t) {
                const double a = area(obj(s, o, t), 1);
                p.frames += a > 0.002;
                if (a > 0.002) p.seen[t] = '1';
                p.maxArea = std::max(p.maxArea, a);
            }
            if (p.frames >= 3 && p.maxArea >= 0.005) all.push_back(p);
        }
        double big = 0;
        for (const ScanPerson& x : all) big = std::max(big, x.maxArea);
        for (ScanPerson& x : all) {
            if (x.maxArea < std::max(0.012, 0.08 * big) || r->kept[s].size() >= 8) continue;
            double sum = 0;
            int used = 0;
            for (int t = 0; t < len && used < 10; t += std::max(1, len / 10)) {
                const uint8_t* m = obj(s, x.o, t);
                if (area(m, 4) < 0.002) continue;
                ++used;
                const uint8_t* fr = rgb.data() + static_cast<size_t>(bounds[s] + t) * px * 3;
                for (size_t i = 0; i < px; i += 2) {
                    if (m[i] <= 127) continue;
                    const float R = fr[i * 3] / 255.0f, G = fr[i * 3 + 1] / 255.0f, B = fr[i * 3 + 2] / 255.0f;
                    const float mx = std::max({R, G, B}), mn = std::min({R, G, B}), d = mx - mn;
                    const float sat = mx > 0 ? d / mx : 0;
                    int bin;
                    if (sat < 0.18f || mx < 0.12f) {
                        bin = 64 + std::min(7, static_cast<int>(mx * 8));
                    } else {
                        float hue = mx == R ? (G - B) / d : mx == G ? 2 + (B - R) / d : 4 + (R - G) / d;
                        hue = hue < 0 ? hue + 6 : hue;
                        bin = std::min(7, static_cast<int>(hue / 6 * 8)) * 8 + (sat > 0.55f ? 4 : 0) + std::min(3, static_cast<int>(mx * 4));
                    }
                    x.hist[bin] += 1;
                    sum += 1;
                }
            }
            if (sum > 0)
                for (float& v : x.hist) v = static_cast<float>(v / sum);
            r->kept[s].push_back(x);
        }
    }
    // one frame per shot, everyone outlined; who is where in it; each person's box and clearest view
    static const uint8_t cols[8][3] = {{255, 70, 70}, {60, 220, 90}, {70, 140, 255}, {255, 220, 40}, {40, 230, 230}, {240, 80, 240}, {255, 150, 30}, {255, 255, 255}};
    const int W2 = aw * 2, H2 = ah * 2;
    auto big2 = [&](int frame) {
        std::vector<uint8_t> img(static_cast<size_t>(W2) * H2 * 4, 255);
        const uint8_t* fr = rgb.data() + static_cast<size_t>(frame) * px * 3;
        for (int y = 0; y < H2; ++y)
            for (int x = 0; x < W2; ++x) memcpy(&img[(static_cast<size_t>(y) * W2 + x) * 4], &fr[(static_cast<size_t>(y / 2) * aw + x / 2) * 3], 3);
        return img;
    };
    std::vector<uint8_t> edge(static_cast<size_t>(W2) * H2), thick(edge.size());
    auto outline = [&](std::vector<uint8_t>& img, const uint8_t* m, const uint8_t* colour) {
        auto in = [&](int x, int y) { return x >= 0 && y >= 0 && x < W2 && y < H2 && m[(y / 2) * aw + x / 2] > 127; };
        for (int y = 0; y < H2; ++y)
            for (int x = 0; x < W2; ++x) edge[static_cast<size_t>(y) * W2 + x] = in(x, y) && (!in(x - 1, y) || !in(x + 1, y) || !in(x, y - 1) || !in(x, y + 1));
        DilateMask(edge.data(), thick.data(), W2, H2, 1);
        for (size_t p = 0; p < thick.size(); ++p)
            if (thick[p]) memcpy(&img[p * 4], colour, 3);
    };
    r->shown.assign(shots, 0);
    r->png.assign(shots, std::wstring());
    r->labels.assign(shots, std::vector<uint8_t>(px, 255));
    for (int s = 0; s < shots; ++s) {
        const int len = bounds[s + 1] - bounds[s];
        auto& kept = r->kept[s];
        int best = len / 2;
        double bestSum = -1;
        for (int t = 0; t < len && !kept.empty(); ++t) {
            double sum = 0;
            for (const ScanPerson& x : kept) sum += area(obj(s, x.o, t), 4) / std::max(0.01, x.maxArea);
            if (sum > bestSum) bestSum = sum, best = t;
        }
        r->shown[s] = best;
        std::vector<uint8_t> img = big2(bounds[s] + best);
        std::vector<double> owner(px, 2.0);
        for (size_t i = 0; i < kept.size(); ++i) {
            const uint8_t* m = obj(s, kept[i].o, best);
            outline(img, m, cols[i % 8]);
            const double a = area(m, 1);
            for (int y = 0; y < ah; ++y)
                for (int x = 0; x < aw; ++x) {
                    const size_t p = static_cast<size_t>(y) * aw + x;
                    if (m[p] <= 127) continue;
                    kept[i].x0 = kept[i].x1 < kept[i].x0 ? x : std::min(kept[i].x0, x);
                    kept[i].x1 = std::max(kept[i].x1, x);
                    kept[i].y0 = kept[i].y1 < 0 ? y : std::min(kept[i].y0, y);
                    kept[i].y1 = std::max(kept[i].y1, y);
                    if (a < owner[p]) owner[p] = a, r->labels[s][p] = static_cast<uint8_t>(i); // (the smaller person is the one in front)
                }
            if (kept[i].x1 < 0) kept[i].x0 = 0;
            // their clearest view, outlined, cropped
            int t = 0;
            double most = 0;
            for (int u = 0; u < len; ++u) {
                const double a2 = area(obj(s, kept[i].o, u), 2);
                if (a2 > most) most = a2, t = u;
            }
            const uint8_t* mm = obj(s, kept[i].o, t);
            int bx0 = aw, by0 = ah, bx1 = -1, by1 = -1;
            for (int y = 0; y < ah; ++y)
                for (int x = 0; x < aw; ++x)
                    if (mm[y * aw + x] > 127) bx0 = std::min(bx0, x), bx1 = std::max(bx1, x), by0 = std::min(by0, y), by1 = std::max(by1, y);
            if (bx1 < 0) continue;
            std::vector<uint8_t> view = big2(bounds[s] + t);
            static const uint8_t red[3] = {255, 0, 0};
            outline(view, mm, red);
            const int padX = (bx1 - bx0) * 35 / 100 + 8, padY = (by1 - by0) * 35 / 100 + 8;
            const int cx0 = std::max(0, 2 * (bx0 - padX)), cy0 = std::max(0, 2 * (by0 - padY)), cx1 = std::min(W2, 2 * (bx1 + padX)), cy1 = std::min(H2, 2 * (by1 + padY));
            const int cw = cx1 - cx0, ch = cy1 - cy0;
            if (cw < 8 || ch < 8) continue;
            std::vector<uint8_t> crop(static_cast<size_t>(cw) * ch * 4);
            for (int y = 0; y < ch; ++y) memcpy(&crop[static_cast<size_t>(y) * cw * 4], &view[(static_cast<size_t>(cy0 + y) * W2 + cx0) * 4], static_cast<size_t>(cw) * 4);
            const std::wstring cp = stem + L"_s" + std::to_wstring(s) + L"_p" + std::to_wstring(i) + L".png";
            if (WriteThumbnailPng(cp, crop.data(), cw, ch)) kept[i].crop = cp;
        }
        const std::wstring png = stem + L"_s" + std::to_wstring(s) + L".png";
        if (WriteThumbnailPng(png, img.data(), W2, H2)) r->png[s] = png;
    }
    r->ok = true;
    SaveScan(*r, stem);
    ScanPhase("");
    return r;
}

// Where a long swap is cut into parts: each at most 10 s (a 15 s part fills a 24 GB card and pages: 20+ minutes for what
// takes 7), cut where the clip itself cuts when there is a cut 4-10 s in (a join there cannot be seen), else into equal parts
std::vector<std::pair<double, double>> PlanSwapParts(const std::wstring& src, double start, double seconds) {
    std::vector<double> cuts;
    const std::wstring ff = FfmpegPath();
    if (!ff.empty()) {
        std::string out;
        char span[96];
        snprintf(span, sizeof(span), "-ss %.3f -t %.3f", start, seconds);
        RunCaptureIdle(L"\"" + ff + L"\" -hide_banner " + std::wstring(span, span + strlen(span)) + L" -i \"" + src + L"\" -an -vf \"select='gt(scene,0.25)',showinfo\" -f null -", out, true);
        for (size_t at = out.find("pts_time:"); at != std::string::npos; at = out.find("pts_time:", at + 9)) cuts.push_back(atof(out.c_str() + at + 9));
    }
    std::vector<std::pair<double, double>> parts;
    double c = 0;
    while (seconds - c > kSwapPartMax + 0.01) {
        const double rest = seconds - c;
        const double even = rest / std::ceil(rest / kSwapPartMax); // (no cut to use: equal parts, never a tiny last one)
        double b = -1;
        for (double t : cuts)
            if (t >= c + 4.0 && t <= c + kSwapPartMax && seconds - t >= 3.0) b = t; // (the latest such cut)
        if (b < 0) b = c + even;
        b = std::round(b * 24.0) / 24.0;
        parts.push_back({c, b - c});
        c = b;
    }
    parts.push_back({c, seconds - c});
    // At the clip's end the model makes only the 17k + 5 frames the clip still has (a 29 s swap of a 29 s clip came back 28.75 s: the last
    // part's 232 frames made as 226). The last part then is made exactly 17k + 5 frames long; the parts before take the rest (each at most 10 s).
    if (parts.size() >= 2) {
        const VideoInfo vi = ProbeVideoFfmpeg(src);
        std::vector<int> f;
        for (const auto& p : parts) f.push_back(static_cast<int>(std::lround(p.second * 24.0)));
        const int last = f.back();
        const int have = static_cast<int>(std::floor((vi.seconds - start - parts.back().first) * 24.0)) - 1; // (a frame spare)
        if (vi.seconds > 0 && FramesFor(last / 24.0) > have) {
            int fit = std::min(last, have);
            fit -= (fit - 5 + 17) % 17;
            int d = last - fit;
            for (size_t i = f.size() - 1; i-- > 0 && d > 0 && fit >= 73;) {
                const int add = std::min(d, static_cast<int>(std::lround(kSwapPartMax * 24.0)) - f[i]);
                if (add > 0) f[i] += add, f.back() -= add, d -= add;
            }
            double at = 0;
            for (size_t i = 0; i < parts.size(); ++i) parts[i] = {at, f[i] / 24.0}, at += f[i] / 24.0;
        }
    }
    return parts;
}

// The people of a part of any length: one scan, or (over 10 s) a scan per part, their shots one after another
std::shared_ptr<PeopleScan> ScanPeopleLong(std::wstring src, double start, double seconds, std::string comfyUrl, std::string sam3) {
    if (seconds <= kSwapPartMax + 0.2) return ScanPeople(src, start, seconds, comfyUrl, sam3);
    auto r = std::make_shared<PeopleScan>();
    char part[64];
    snprintf(part, sizeof(part), "|%.3f|%.3f", start, seconds);
    r->key = FileKey(src) + part;
    const auto plan = PlanSwapParts(src, start, seconds);
    r->bounds = {0};
    for (size_t p = 0; p < plan.size(); ++p) {
        {
            std::lock_guard<std::mutex> lock(g_scanPhaseMutex);
            g_scanPart = "part " + std::to_string(p + 1) + " of " + std::to_string(plan.size());
        }
        std::shared_ptr<PeopleScan> ps = ScanPeople(src, start + plan[p].first, plan[p].second, comfyUrl, sam3);
        if (!ps->ok) {
            r->note = "part " + std::to_string(p + 1) + ": " + ps->note;
            std::lock_guard<std::mutex> lock(g_scanPhaseMutex);
            g_scanPart.clear();
            return r;
        }
        const int off = static_cast<int>(std::lround(plan[p].first * 24.0)), exact = static_cast<int>(std::lround(plan[p].second * 24.0));
        r->aw = ps->aw, r->ah = ps->ah, r->K = ps->K;
        for (size_t s = 0; s + 1 < ps->bounds.size(); ++s) {
            if (ps->bounds[s] >= exact - 2) continue; // (a shot only in the frames made past the part's end, which the join trims)
            r->kept.push_back(ps->kept[s]);
            r->png.push_back(ps->png[s]);
            r->labels.push_back(ps->labels[s]);
            r->shown.push_back(ps->shown[s]);
            r->shotOf.push_back({static_cast<int>(p), static_cast<int>(s)});
            r->bounds.push_back(off + std::min(ps->bounds[s + 1], exact));
        }
        r->parts.push_back(ps);
        r->partStart.push_back(plan[p].first);
        r->partLen.push_back(plan[p].second);
    }
    r->n = static_cast<int>(std::lround(seconds * 24.0));
    r->ok = !r->kept.empty();
    if (!r->ok) r->note = "nobody was found in it";
    ScanPhase("");
    std::lock_guard<std::mutex> lock(g_scanPhaseMutex);
    g_scanPart.clear();
    return r;
}

// The person masks of a clip part: targets[k] is whom <Subject k> replaces ("the girl carrying a tray (woman)"). Runs on a worker thread.
PeopleMask BuildPeopleMasks(const std::wstring& clip, int frames, int w, int h, const std::vector<std::string>& targets, const std::string& comfyUrl,
                            const std::string& sam3, const std::vector<float>& room, std::shared_ptr<const PeopleScan> scan,
                            std::vector<std::vector<std::vector<int>>> given) {
    PeopleMask r;
    std::string key = FileKey(clip) + "|" + std::to_string(frames) + "|" + std::to_string(w) + "x" + std::to_string(h);
    for (const std::string& t : targets) key += "|" + t;
    for (float f : room) key += "|" + std::to_string(static_cast<int>(f * 100));
    const bool fromScan = scan && scan->ok && !given.empty(); // the people picked on the page: their tracks are the scan's
    if (fromScan) {
        key += "|picked|" + Utf8(scan->tracks);
        for (const auto& shot : given)
            for (const auto& who : shot) {
                key += "/";
                for (int o : who) key += std::to_string(o) + ",";
            }
    }
    {
        std::lock_guard<std::mutex> lock(g_peopleMaskMutex);
        auto it = g_peopleMasks.find(key);
        std::error_code ec;
        if (it != g_peopleMasks.end() && it->second.ok && fs::exists(it->second.video, ec)) return it->second;
    }
    const VideoInfo vi = ProbeVideo(clip);
    const int n = fromScan ? scan->n : std::max(1, std::min(frames, vi.seconds > 0 ? static_cast<int>(std::floor(vi.seconds * 24.0 + 0.01)) : frames));
    const double f = std::min(1.0, 512.0 / std::max(w, h));
    const int aw = fromScan ? scan->aw : std::max(64, static_cast<int>(w * f) / 8 * 8), ah = fromScan ? scan->ah : std::max(64, static_cast<int>(h * f) / 8 * 8);
    const size_t px = static_cast<size_t>(aw) * ah;
    std::vector<int> bounds = {0};
    if (fromScan) {
        bounds = scan->bounds;
    } else {
        for (int c : ShotCuts(clip, n)) bounds.push_back(c);
        bounds.push_back(n);
    }
    r.shots = static_cast<int>(bounds.size()) - 1;
    const int K = 10;
    const std::wstring dir = DataDir() + L"\\h3\\masks";
    std::vector<std::wstring> got;
    std::string err;
    if (fromScan) {
        got = {scan->tracks};
    } else {
        // 1. ComfyUI: per shot a track of everyone, every object's mask in turn - one video, shot by shot, object by object
        SetWritePhase("Finding the people in every shot...");
        json g;
        auto node = [&](const std::string& id, const char* cls, json inputs) { g[id] = {{"class_type", cls}, {"inputs", std::move(inputs)}}; };
        node("1", "LoadVideo", {{"file", "example.mp4"}});
        node("2", "GetVideoComponents", {{"video", json::array({"1", 0})}});
        node("3", "ImageFromBatch", {{"image", json::array({"2", 0})}, {"batch_index", 0}, {"length", n}});
        node("4", "ImageScale", {{"image", json::array({"3", 0})}, {"upscale_method", "lanczos"}, {"width", aw}, {"height", ah}, {"crop", "center"}});
        node("5", "CheckpointLoaderSimple", {{"ckpt_name", sam3}});
        node("6", "CLIPTextEncode", {{"clip", json::array({"5", 1})}, {"text", "person"}});
        json last;
        for (int s = 0; s < r.shots; ++s) {
            const std::string ss = std::to_string(s);
            node("s" + ss, "ImageFromBatch", {{"image", json::array({"4", 0})}, {"batch_index", bounds[s]}, {"length", bounds[s + 1] - bounds[s]}});
            node("t" + ss, "SAM3_VideoTrack", {{"images", json::array({"s" + ss, 0})}, {"model", json::array({"5", 0})}, {"detection_threshold", 0.4}, {"max_objects", K},
                                              {"detect_interval", 1}, {"conditioning", json::array({"6", 0})}});
            for (int o = 0; o < K; ++o) {
                const std::string so = ss + "_" + std::to_string(o);
                node("m" + so, "SAM3_TrackToMask", {{"track_data", json::array({"t" + ss, 0})}, {"object_indices", std::to_string(o)}});
                node("i" + so, "MaskToImage", {{"mask", json::array({"m" + so, 0})}});
                if (last.is_null()) {
                    last = json::array({"i" + so, 0});
                } else {
                    node("b" + so, "ImageBatch", {{"image1", last}, {"image2", json::array({"i" + so, 0})}});
                    last = json::array({"b" + so, 0});
                }
            }
        }
        node("90", "CreateVideo", {{"images", last}, {"fps", 24.0}});
        node("91", "SaveVideo", {{"video", json::array({"90", 0})}, {"filename_prefix", "masks/people"}, {"format", "auto"}, {"format.codec", "auto"}});
        if (!ComfyRunGraph(comfyUrl, g.dump(), {{"1", clip}}, dir, got, err, 1800) || got.empty()) {
            r.note = "SAM 3 could not look for the people: " + err;
            return r;
        }
    }
    std::vector<uint8_t> tracks, rgb;
    if (!DecodeRaw(got[0], n * K, aw, ah, true, tracks) || tracks.size() < px * n * K || !DecodeRaw(clip, n, aw, ah, false, rgb) || rgb.size() < px * 3 * n) {
        r.note = "could not read the person tracks";
        return r;
    }
    std::error_code ec;
    if (!fromScan) fs::remove(got[0], ec); // (a scan's tracks stay: the page's picks use them)
    auto obj = [&](int s, int o, int t) { // shot s, object o, frame t of the shot
        size_t at = 0;
        for (int q = 0; q < s; ++q) at += static_cast<size_t>(bounds[q + 1] - bounds[q]) * K;
        return tracks.data() + (at + static_cast<size_t>(o) * (bounds[s + 1] - bounds[s]) + t) * px;
    };
    // 2. the people worth asking about in each shot: seen in 3+ frames, not tiny next to the biggest
    struct Obj { int o; double maxArea; };
    std::vector<std::vector<Obj>> kept(r.shots);
    if (fromScan) {
        for (int s = 0; s < r.shots; ++s)
            for (const ScanPerson& p : scan->kept[s]) kept[s].push_back({p.o, p.maxArea});
    }
    for (int s = 0; !fromScan && s < r.shots; ++s) {
        const int len = bounds[s + 1] - bounds[s];
        std::vector<Obj> all;
        for (int o = 0; o < K; ++o) {
            int present = 0;
            double maxArea = 0;
            for (int t = 0; t < len; ++t) {
                const uint8_t* m = obj(s, o, t);
                size_t c = 0;
                for (size_t i = 0; i < px; ++i) c += m[i] > 127;
                const double a = static_cast<double>(c) / px;
                present += a > 0.002;
                maxArea = std::max(maxArea, a);
            }
            if (present >= 3 && maxArea >= 0.005) all.push_back({o, maxArea});
        }
        double big = 0;
        for (const Obj& x : all) big = std::max(big, x.maxArea);
        for (const Obj& x : all)
            if (x.maxArea >= std::max(0.012, 0.08 * big) && kept[s].size() < 8) kept[s].push_back(x);
    }
    // 3. the prompt writer: which outline is whom, shot by shot
    static const struct { const char* name; uint8_t c[3]; } cols[8] = {{"red", {255, 0, 0}},      {"green", {0, 230, 0}},     {"blue", {40, 90, 255}},     {"yellow", {255, 255, 0}},
                                                                     {"cyan", {0, 255, 255}},  {"magenta", {255, 0, 255}}, {"orange", {255, 140, 0}}, {"white", {255, 255, 255}}};
    std::vector<std::vector<std::vector<int>>> picks(r.shots, std::vector<std::vector<int>>(targets.size())); // shot -> subject -> objects
    std::vector<std::vector<char>> behind(r.shots, std::vector<char>(targets.size(), 0)); // seen from behind there
    std::vector<std::vector<std::string>> faces(r.shots, std::vector<std::string>(targets.size())); // their expression there ("mouth open in surprise")
    if (fromScan) { // picked on the page (clicked, or followed there by the colours of their clothes): no telling apart from words
        for (int s2 = 0; s2 < r.shots && s2 < static_cast<int>(given.size()); ++s2)
            for (size_t k = 0; k < targets.size() && k < given[s2].size(); ++k) {
                picks[s2][k] = given[s2][k];
                r.labelled += static_cast<int>(given[s2][k].size());
            }
    } else {
        if (!WriterConfigured()) {
            r.note = "the prompt writer is not set up (it tells the people apart)";
            return r;
        }
        const ClefSession session = WriterAcquire(comfyUrl, SetWritePhase);
        if (!session.ok || !WriterCanSee()) {
            WriterRelease(session, false);
            r.note = session.ok ? "the prompt writer cannot see pictures (it tells the people apart)" : "the prompt writer could not be started";
            return r;
        }
        std::string tg, lines;
        for (size_t k = 0; k < targets.size(); ++k) {
            tg += "SUBJECT " + std::to_string(k + 1) + ": " + targets[k] + "\n";
            lines += "SUBJECT " + std::to_string(k + 1) + ": <colour or none>\n";
        }
        const int W2 = aw * 2, H2 = ah * 2;
        auto big = [&](int frame) { // a clip frame at twice the size (bilinear, RGBA), so the outlines do not hide much
            std::vector<uint8_t> img(static_cast<size_t>(W2) * H2 * 4);
            const uint8_t* fr = rgb.data() + static_cast<size_t>(frame) * px * 3;
            for (int y = 0; y < H2; ++y) {
                const float sy = std::clamp((y + 0.5f) / 2 - 0.5f, 0.0f, ah - 1.0f);
                const int y0 = static_cast<int>(sy), y1 = std::min(ah - 1, y0 + 1);
                const float fy = sy - y0;
                for (int x = 0; x < W2; ++x) {
                    const float sx = std::clamp((x + 0.5f) / 2 - 0.5f, 0.0f, aw - 1.0f);
                    const int x0 = static_cast<int>(sx), x1 = std::min(aw - 1, x0 + 1);
                    const float fx = sx - x0;
                    for (int c = 0; c < 3; ++c) {
                        const float v = (fr[(y0 * aw + x0) * 3 + c] * (1 - fx) + fr[(y0 * aw + x1) * 3 + c] * fx) * (1 - fy) + (fr[(y1 * aw + x0) * 3 + c] * (1 - fx) + fr[(y1 * aw + x1) * 3 + c] * fx) * fy;
                        img[(static_cast<size_t>(y) * W2 + x) * 4 + c] = static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f));
                    }
                    img[(static_cast<size_t>(y) * W2 + x) * 4 + 3] = 255;
                }
            }
            return img;
        };
        std::vector<uint8_t> edge(static_cast<size_t>(W2) * H2), thick(edge.size());
        auto outline = [&](std::vector<uint8_t>& img, const uint8_t* m, const uint8_t* colour) { // a mask's edge, 3 px wide
            auto in = [&](int x, int y) { return x >= 0 && y >= 0 && x < W2 && y < H2 && m[(y / 2) * aw + x / 2] > 127; };
            for (int y = 0; y < H2; ++y)
                for (int x = 0; x < W2; ++x) edge[static_cast<size_t>(y) * W2 + x] = in(x, y) && (!in(x - 1, y) || !in(x + 1, y) || !in(x, y - 1) || !in(x, y + 1));
            DilateMask(edge.data(), thick.data(), W2, H2, 1);
            for (size_t p = 0; p < thick.size(); ++p)
                if (thick[p]) memcpy(&img[p * 4], colour, 3);
        };
        for (int s = 0; s < r.shots; ++s) {
            if (kept[s].empty()) continue;
            SetWritePhase("Telling the people apart (shot " + std::to_string(s + 1) + " of " + std::to_string(r.shots) + ")...");
            const int len = bounds[s + 1] - bounds[s];
            int best = 0;
            double bestSum = -1;
            for (int t = 0; t < len; ++t) { // the frame that shows the most of them
                double sum = 0;
                for (const Obj& x : kept[s]) {
                    const uint8_t* m = obj(s, x.o, t);
                    size_t c = 0;
                    for (size_t i = 0; i < px; i += 4) c += m[i] > 127;
                    sum += static_cast<double>(c) / (px / 4) / std::max(0.01, x.maxArea);
                }
                if (sum > bestSum) bestSum = sum, best = t;
            }
            std::vector<uint8_t> img = big(bounds[s] + best);
            std::string colours;
            for (size_t i = 0; i < kept[s].size(); ++i) {
                outline(img, obj(s, kept[s][i].o, best), cols[i].c);
                colours += (i ? ", " : "") + std::string(cols[i].name);
            }
            const std::wstring png = dir + L"\\shot" + std::to_wstring(s) + L".png";
            if (!WriteThumbnailPng(png, img.data(), W2, H2)) continue;
            const std::string url = ImageDataUrl(png, std::max(W2, H2));
            std::string out;
            const std::string system = ReplaceAll(ReplaceAll(ReplaceAll(kPeopleLabelSystem, "{COLOURS}", colours), "{TARGETS}", tg), "{LINES}", lines);
            if (url.empty() || !WriterChat(system, "Which outline is each subject?", {url}, 400, 0.1, out, &err)) continue;
            std::ofstream(dir + L"\\shot" + std::to_wstring(s) + L".txt", std::ios::binary) << out; // (what it said, next to what it saw)
            std::string lowOut = out;
            for (char& ch : lowOut) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
            auto sexOf = [&](const std::string& colour) { // what its own line says: "red: man, dark sweater..." -> 'm'
                std::istringstream in(lowOut);
                for (std::string l; std::getline(in, l);) {
                    std::string c2;
                    for (char ch : l)
                        if (ch != '*' && ch != '-') c2 += ch;
                    c2 = Trimmed(c2);
                    if (c2.rfind(colour + ":", 0) != 0) continue;
                    const std::string rest = c2.substr(colour.size() + 1);
                    if (rest.find("woman") != std::string::npos || rest.find("girl") != std::string::npos) return 'w';
                    if (rest.find(" man") != std::string::npos || rest.find("boy") != std::string::npos || rest.rfind("man", 0) == 0) return 'm';
                }
                return '?';
            };
            for (size_t k = 0; k < targets.size(); ++k) {
                std::string v = SwapField(out, ("SUBJECT " + std::to_string(k + 1) + ":").c_str());
                for (char& ch : v) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
                const char want = targets[k].find("(woman)") != std::string::npos ? 'w' : targets[k].find("(man)") != std::string::npos ? 'm' : '?';
                for (size_t i = 0; i < kept[s].size(); ++i) {
                    const std::string c = cols[i].name;
                    const char is = sexOf(c);
                    if (want != '?' && is != '?' && is != want) continue; // (seen: "SUBJECT 1: red" for the girl, "red: man, dark sweater")
                    for (size_t at = v.find(c); at != std::string::npos; at = v.find(c, at + 1)) {
                        if ((at == 0 || !isalpha(static_cast<unsigned char>(v[at - 1]))) && (at + c.size() >= v.size() || !isalpha(static_cast<unsigned char>(v[at + c.size()])))) {
                            picks[s][k].push_back(kept[s][i].o);
                            { // its own line: "green: man, ..., seen from behind"
                                std::istringstream in(lowOut);
                                for (std::string l; std::getline(in, l);) {
                                    std::string c2;
                                    for (char ch : l)
                                        if (ch != '*' && ch != '-') c2 += ch;
                                    c2 = Trimmed(c2);
                                    if (c2.rfind(c + ":", 0) != 0) continue;
                                    if (c2.find("behind") != std::string::npos || c2.find("from the back") != std::string::npos || c2.find("back to the camera") != std::string::npos)
                                        behind[s][k] = 1;
                                    else if (c2.find("side") != std::string::npos && !behind[s][k]) // side-on (seen: the boy at the table, "seen from the side", redrawn as the original)
                                        behind[s][k] = 2;
                                    if (faces[s][k].empty()) {
                                        std::string f;
                                        const size_t fa = c2.find("face:");
                                        if (fa != std::string::npos) {
                                            f = Trimmed(c2.substr(fa + 5));
                                        } else { // (seen without the label: "red: woman, ..., pink floral top, mouth open in surprise, facing slightly upward")
                                            std::istringstream parts(c2.substr(c.size() + 1));
                                            for (std::string seg; std::getline(parts, seg, ',');) {
                                                for (const char* w : {"smil", "laugh", "grin", "mouth open", "open mouth", "surpris", "shock", "frown", "talking", "shout", "scream", "crying", "wink"})
                                                    if (f.empty() && seg.find(w) != std::string::npos) f = Trimmed(seg);
                                            }
                                        }
                                        while (!f.empty() && strchr(".,;", f.back())) f.pop_back();
                                        if (f.rfind("neutral with ", 0) == 0) f = Trimmed(f.substr(13)); // "neutral with a slight smile" -> "a slight smile"
                                        if (!f.empty() && f.size() <= 60 && f.find("not visible") == std::string::npos && f != "neutral" && f.rfind("neutral,", 0) != 0 &&
                                            f.find("unclear") == std::string::npos && f != "none")
                                            faces[s][k] = f;
                                    }
                                }
                            }
                            ++r.labelled;
                            break;
                        }
                    }
                }
            }
        }
        // each pick checked against that person's clearest view (their biggest pick): "the same person?" (it caught a blonde girl given as the
        // dark-haired girl in the pink top, from another shot; 0.4 s a pair)
        static const uint8_t red[3] = {255, 0, 0};
        auto view = [&](int s, int o, std::vector<uint8_t>& out, int& ow, int& oh) { // the person at their biggest in the shot, outlined, cropped
            const int len = bounds[s + 1] - bounds[s];
            int t = 0;
            size_t most = 0;
            for (int u = 0; u < len; ++u) {
                const uint8_t* m = obj(s, o, u);
                size_t c = 0;
                for (size_t i = 0; i < px; i += 2) c += m[i] > 127;
                if (c > most) most = c, t = u;
            }
            const uint8_t* m = obj(s, o, t);
            int x0 = aw, y0 = ah, x1 = -1, y1 = -1;
            for (int y = 0; y < ah; ++y)
                for (int x = 0; x < aw; ++x)
                    if (m[y * aw + x] > 127) x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
            if (x1 < 0) return false;
            std::vector<uint8_t> img = big(bounds[s] + t);
            outline(img, m, red);
            const int padX = (x1 - x0) * 35 / 100 + 8, padY = (y1 - y0) * 35 / 100 + 8;
            const int cx0 = std::max(0, 2 * (x0 - padX)), cy0 = std::max(0, 2 * (y0 - padY)), cx1 = std::min(W2, 2 * (x1 + padX)), cy1 = std::min(H2, 2 * (y1 + padY));
            const double sc = std::min(1.0, 384.0 / std::max(cx1 - cx0, cy1 - cy0));
            ow = std::max(1, static_cast<int>((cx1 - cx0) * sc)), oh = std::max(1, static_cast<int>((cy1 - cy0) * sc));
            out.assign(static_cast<size_t>(ow) * oh * 4, 255);
            for (int y = 0; y < oh; ++y)
                for (int x = 0; x < ow; ++x) {
                    const int sx = std::min(W2 - 1, cx0 + static_cast<int>(x / sc)), sy = std::min(H2 - 1, cy0 + static_cast<int>(y / sc));
                    memcpy(&out[(static_cast<size_t>(y) * ow + x) * 4], &img[(static_cast<size_t>(sy) * W2 + sx) * 4], 4);
                }
            return true;
        };
        for (size_t k = 0; k < targets.size(); ++k) {
            int as = -1, ao = -1;
            double biggest = 0;
            size_t count = 0;
            for (int s = 0; s < r.shots; ++s)
                for (int o : picks[s][k]) {
                    ++count;
                    for (const Obj& x : kept[s])
                        if (x.o == o && x.maxArea > biggest) biggest = x.maxArea, as = s, ao = o;
                }
            if (count < 2 || as < 0) continue;
            SetWritePhase("Checking it is the same person in every shot...");
            std::vector<uint8_t> a;
            int aw2 = 0, ah2 = 0;
            const std::wstring pa = dir + L"\\same" + std::to_wstring(k + 1) + L"_anchor.png";
            if (!view(as, ao, a, aw2, ah2) || !WriteThumbnailPng(pa, a.data(), aw2, ah2)) continue;
            const std::string ua = ImageDataUrl(pa, 384);
            for (int s = 0; s < r.shots; ++s) {
                auto& list = picks[s][k];
                for (size_t i = 0; i < list.size();) {
                    if (s == as && list[i] == ao) {
                        ++i;
                        continue;
                    }
                    std::vector<uint8_t> b;
                    int bw = 0, bh = 0;
                    const std::wstring pb = dir + L"\\same" + std::to_wstring(k + 1) + L"_shot" + std::to_wstring(s) + L".png";
                    std::string out;
                    if (view(s, list[i], b, bw, bh) && WriteThumbnailPng(pb, b.data(), bw, bh) &&
                        WriterChat("Both pictures are frames from different shots of the same video; in each, one person is outlined in red. Is the outlined "
                                   "person in picture 2 the same person as the outlined person in picture 1? Judge by clothes, hair, skin and build, not by "
                                   "pose, size or camera angle. Reply with one word: yes or no.",
                                   "Picture 1, then picture 2. Same person?", {ua, ImageDataUrl(pb, 384)}, 10, 0.0, out, &err)) {
                        std::string low = Trimmed(out);
                        for (char& ch : low) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
                        std::ofstream(pb + L".txt", std::ios::binary) << out;
                        if (low.rfind("no", 0) == 0) {
                            list.erase(list.begin() + static_cast<long>(i));
                            --r.labelled;
                            continue;
                        }
                    }
                    ++i;
                }
            }
        }
        WriterRelease(session, false);
    }
    if (r.labelled == 0) {
        r.note = "the prompt writer found none of the people to replace in the clip";
        return r;
    }
    // who is on screen in each shot, and where (the middle of their masks): it goes into the prompt instead of the writer's guess at the
    // action, which, written from four frames across cuts, tied the wrong person to a shot (seen: "<Subject 1> ... lifts <Subject 2>")
    r.timelines.assign(targets.size(), std::string());
    for (int s = 0; s < r.shots; ++s) {
        const int len = bounds[s + 1] - bounds[s];
        std::string who;
        for (size_t k = 0; k < targets.size(); ++k) {
            if (picks[s][k].empty()) continue;
            double sx = 0, cnt = 0;
            for (int t = 0; t < len; t += 2)
                for (int o : picks[s][k]) {
                    const uint8_t* m = obj(s, o, t);
                    for (int y = 0; y < ah; y += 4)
                        for (int x = 0; x < aw; x += 4)
                            if (m[y * aw + x] > 127) sx += x, cnt += 1;
                }
            if (cnt == 0) continue;
            const double cx = sx / cnt / aw;
            const std::string sk = "<Subject " + std::to_string(k + 1) + ">";
            who += (who.empty() ? "" : " and ") + sk + (cx < 0.36 ? " on the left" : cx > 0.64 ? " on the right" : " in the centre");
            // seen from behind, the model went by the guide: the original's dark hair and sweater from the back (seen in a test)
            if (behind[s][k])
                who += std::string(behind[s][k] == 1 ? ", seen from behind (" : ", seen from the side (") + sk + "'s own hair and clothes from <Picture " + std::to_string(k + 1) + ">, not the guide's)";
            // their face too: the new face kept its picture's closed mouth through the girl's smile and her surprise as she trips (seen in a test)
            if (!faces[s][k].empty()) who += ", " + faces[s][k];
            { // the same, as this person's own line (one person at a time: they are <Subject 1> in their pass)
                char at[48];
                snprintf(at, sizeof(at), "%.1f-%.1f s: ", bounds[s] / 24.0, bounds[s + 1] / 24.0);
                std::string one = std::string("<Subject 1>") + (cx < 0.36 ? " on the left" : cx > 0.64 ? " on the right" : " in the centre");
                if (behind[s][k]) one += std::string(behind[s][k] == 1 ? ", seen from behind" : ", seen from the side") + " (<Subject 1>'s own hair and clothes from <Picture 1>, not the guide's)";
                if (!faces[s][k].empty()) one += ", " + faces[s][k];
                r.timelines[k] += (r.timelines[k].empty() ? "" : "; ") + std::string(at) + one;
            }
            WhoBox b; // their biggest frame in the shot, for the check after the take
            b.shot = s, b.subject = static_cast<int>(k);
            size_t most = 0;
            for (int t = 0; t < len; ++t) {
                size_t c = 0;
                for (int o : picks[s][k]) {
                    const uint8_t* m = obj(s, o, t);
                    for (size_t i = 0; i < px; i += 3) c += m[i] > 127;
                }
                if (c > most) most = c, b.frame = bounds[s] + t;
            }
            b.x0 = aw, b.y0 = ah, b.x1 = -1, b.y1 = -1;
            for (int o : picks[s][k]) {
                const uint8_t* m = obj(s, o, b.frame - bounds[s]);
                for (int y = 0; y < ah; ++y)
                    for (int x = 0; x < aw; ++x)
                        if (m[y * aw + x] > 127) b.x0 = std::min(b.x0, x), b.x1 = std::max(b.x1, x), b.y0 = std::min(b.y0, y), b.y1 = std::max(b.y1, y);
            }
            if (b.x1 > b.x0 + 8 && b.y1 > b.y0 + 8) r.boxes.push_back(b);
        }
        char span[48];
        snprintf(span, sizeof(span), "%.1f-%.1f s: ", bounds[s] / 24.0, bounds[s + 1] / 24.0);
        r.timeline += (r.timeline.empty() ? "" : "; ") + std::string(span) + (who.empty() ? (targets.size() > 1 ? "none of them" : "not <Subject 1>") : who);
    }
    // 4. the masks, mended: gaps from both sides, motion next to the person, two frames each way in time
    SetWritePhase("Mending the person masks...");
    std::vector<uint8_t> outMask(px * n, 0), gray(px * n);
    for (size_t i = 0; i < px * n; ++i) gray[i] = static_cast<uint8_t>((rgb[i * 3] * 77 + rgb[i * 3 + 1] * 150 + rgb[i * 3 + 2] * 29) >> 8);
    const int rad = std::max(3, static_cast<int>(0.06 * std::max(aw, ah)));
    std::vector<uint8_t> tmp(px), tmp2(px);
    auto mend = [&](int only, std::vector<uint8_t>& into) { // everyone's masks (only -1) or one person's; returns the frames with someone
    int covered = 0;
    for (int s = 0; s < r.shots; ++s) {
        const int len = bounds[s + 1] - bounds[s];
        std::vector<int> mine;
        for (size_t k = 0; k < picks[s].size(); ++k)
            if (only < 0 || static_cast<int>(k) == only)
                for (int o : picks[s][k]) mine.push_back(o);
        if (mine.empty()) continue;
        std::vector<uint8_t> sel(px * len, 0), everyone(px * len, 0);
        std::vector<double> area(len, 0);
        for (int t = 0; t < len; ++t) {
            uint8_t* d = sel.data() + px * t;
            uint8_t* e = everyone.data() + px * t;
            for (int o = 0; o < K; ++o) {
                const uint8_t* m = obj(s, o, t);
                const bool chosen = std::find(mine.begin(), mine.end(), o) != mine.end();
                for (size_t i = 0; i < px; ++i) {
                    if (m[i] > 127) {
                        e[i] = 1;
                        if (chosen) d[i] = 1;
                    }
                }
            }
            size_t c = 0;
            for (size_t i = 0; i < px; ++i) c += d[i];
            area[t] = static_cast<double>(c) / px;
        }
        std::vector<uint8_t> fixedM = sel;
        std::vector<bool> good(len);
        for (int t = 0; t < len; ++t) { // gone, or far smaller than around it
            std::vector<double> around;
            for (int u = std::max(0, t - 6); u <= std::min(len - 1, t + 6); ++u)
                if (area[u] > 0) around.push_back(area[u]);
            good[t] = area[t] > 0;
            if (!around.empty()) {
                std::nth_element(around.begin(), around.begin() + around.size() / 2, around.end());
                if (area[t] < 0.35 * around[around.size() / 2]) good[t] = false;
            }
        }
        for (int t = 0; t < len; ++t) {
            if (good[t]) continue;
            int p = -1, q = -1;
            for (int u = t - 1; u >= std::max(0, t - 12) && p < 0; --u)
                if (good[u]) p = u;
            for (int u = t + 1; u <= std::min(len - 1, t + 12) && q < 0; ++u)
                if (good[u]) q = u;
            if ((p < 0 && q < 0) || (q < 0 && t - p > 6) || (p < 0 && q - t > 6)) continue;
            std::fill(tmp.begin(), tmp.end(), 0);
            for (int side : {p, q})
                if (side >= 0)
                    for (size_t i = 0; i < px; ++i) tmp[i] |= sel[px * side + i];
            if (p >= 0 && q >= 0 && q - p > 3) DilateMask(tmp.data(), tmp2.data(), aw, ah, rad), tmp.swap(tmp2);
            for (size_t i = 0; i < px; ++i) fixedM[px * t + i] |= tmp[i];
        }
        for (int t = 0; t < len; ++t) { // motion close to them that the mask left out (not the camera moving, not someone else)
            uint8_t* fm = fixedM.data() + px * t;
            if (std::find(fm, fm + px, 1) == fm + px) continue;
            const uint8_t* g0 = gray.data() + px * (bounds[s] + std::max(t - 1, 0));
            const uint8_t* g1 = gray.data() + px * (bounds[s] + t);
            const uint8_t* g2 = gray.data() + px * (bounds[s] + std::min(t + 1, len - 1));
            std::vector<uint8_t> mv(px);
            for (int y = 1; y < ah - 1; ++y)
                for (int x = 1; x < aw - 1; ++x) {
                    int d0 = 0, d2 = 0; // 3x3 averages, so grain does not count
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const size_t i = static_cast<size_t>(y + dy) * aw + x + dx;
                            d0 += g1[i] - g0[i];
                            d2 += g2[i] - g1[i];
                        }
                    mv[static_cast<size_t>(y) * aw + x] = std::max(std::abs(d0), std::abs(d2)) > 22 * 9;
                }
            DilateMask(everyone.data() + px * t, tmp.data(), aw, ah, rad);
            size_t outside = 0, moving = 0;
            for (size_t i = 0; i < px; ++i)
                if (!tmp[i]) ++outside, moving += mv[i];
            if (outside == 0 || moving > outside / 5) continue; // the camera moves: everything changes
            DilateMask(fm, tmp.data(), aw, ah, rad);
            const uint8_t* ev = everyone.data() + px * t;
            const uint8_t* sl = sel.data() + px * t;
            for (size_t i = 0; i < px; ++i) tmp2[i] = mv[i] && tmp[i] && !(ev[i] && !sl[i]) && !fm[i];
            std::vector<uint8_t> opened(px); // specks out: a pixel stays when its whole 3x3 moved
            for (int y = 1; y < ah - 1; ++y)
                for (int x = 1; x < aw - 1; ++x) {
                    bool all = true;
                    for (int dy = -1; dy <= 1 && all; ++dy)
                        for (int dx = -1; dx <= 1 && all; ++dx) all = tmp2[static_cast<size_t>(y + dy) * aw + x + dx] != 0;
                    opened[static_cast<size_t>(y) * aw + x] = all;
                }
            DilateMask(opened.data(), tmp.data(), aw, ah, 1);
            for (size_t i = 0; i < px; ++i) fm[i] |= tmp[i] & tmp2[i];
        }
        // room for the new person: they are often wider than the one they replace (a man for a slim dancer, a coat, a shoulder bag), and what
        // they draw outside the mask is the clip's again - a test showed Ben's bag fading in and out at the dancer's edge. Each person's mask is
        // widened by a share of their own width (more when their picture shows something they carry), never over someone else.
        for (size_t k = 0; k < picks[s].size(); ++k) {
            if (picks[s][k].empty() || (only >= 0 && static_cast<int>(k) != only)) continue;
            const float share = k < room.size() ? room[k] : 0.10f;
            for (int t = 0; t < len; ++t) {
                std::fill(tmp.begin(), tmp.end(), 0);
                int x0 = aw, x1 = -1;
                for (int o : picks[s][k]) {
                    const uint8_t* m = obj(s, o, t);
                    for (int y = 0; y < ah; ++y)
                        for (int x = 0; x < aw; ++x)
                            if (m[y * aw + x] > 127) tmp[static_cast<size_t>(y) * aw + x] = 1, x0 = std::min(x0, x), x1 = std::max(x1, x);
                }
                if (x1 < x0) continue;
                const int rx = std::clamp(static_cast<int>(share * (x1 - x0 + 1)), 2, aw / 6);
                DilateMaskXY(tmp.data(), tmp2.data(), aw, ah, rx, std::max(2, rx / 3));
                const uint8_t* ev = everyone.data() + px * t;
                const uint8_t* sl = sel.data() + px * t;
                uint8_t* fm = fixedM.data() + px * t;
                for (size_t i = 0; i < px; ++i)
                    if (tmp2[i] && !(ev[i] && !sl[i])) fm[i] = 1;
            }
        }
        for (int t = 0; t < len; ++t) { // a frame takes its neighbours' masks too
            uint8_t* o = into.data() + px * (bounds[s] + t);
            for (int u = std::max(0, t - 2); u <= std::min(len - 1, t + 2); ++u)
                for (size_t i = 0; i < px; ++i) o[i] |= fixedM[px * u + i];
            if (std::find(o, o + px, 1) != o + px) ++covered;
        }
    }
    for (uint8_t& v : into) v = v ? 255 : 0;
    return covered;
    };
    r.coverage = static_cast<double>(mend(-1, outMask)) / n;
    // 5. the mask videos (lossless): everyone's, and each person's own
    std::hash<std::string> hasher;
    char name[32];
    snprintf(name, sizeof(name), "%016llx", static_cast<unsigned long long>(hasher(key)));
    std::string log;
    auto writeMask = [&](const std::vector<uint8_t>& m, const std::wstring& tag) {
        const std::wstring raw = dir + L"\\" + std::wstring(name, name + strlen(name)) + tag + L".gray",
                           video = dir + L"\\people_" + std::wstring(name, name + strlen(name)) + tag + L".mp4";
        {
            std::ofstream f(raw, std::ios::binary);
            f.write(reinterpret_cast<const char*>(m.data()), static_cast<std::streamsize>(m.size()));
        }
        RunCaptureIdle(L"\"" + FfmpegPath() + L"\" -y -v error -f rawvideo -pix_fmt gray -s " + std::to_wstring(aw) + L"x" + std::to_wstring(ah) + L" -r 24 -i \"" + raw +
                           L"\" -c:v libx264 -qp 0 -pix_fmt yuv420p \"" + video + L"\"",
                       log, true);
        fs::remove(raw, ec);
        return fs::exists(video, ec) && fs::file_size(video, ec) > 0 ? video : std::wstring();
    };
    const std::wstring video = writeMask(outMask, L"");
    if (video.empty()) {
        r.note = "could not write the mask video" + (log.empty() ? std::string() : ": " + log.substr(0, 200));
        return r;
    }
    if (targets.size() > 1) {
        for (size_t k = 0; k < targets.size(); ++k) {
            std::vector<uint8_t> one(px * n, 0);
            mend(static_cast<int>(k), one);
            r.videos.push_back(writeMask(one, L"_p" + std::to_wstring(k + 1)));
            // the others as they are in each frame (2 px more): the pass may not paint over them. Seen: Ben's grown mask reached over Ava
            // where he sat next to her, and his denim sleeve and the yellow of his shirt came out on her jacket.
            std::vector<uint8_t> others(px * n, 0), grown(px);
            for (int s = 0; s < r.shots; ++s) {
                const int len = bounds[s + 1] - bounds[s];
                for (int t = 0; t < len; ++t) {
                    std::fill(tmp.begin(), tmp.end(), 0);
                    for (size_t q = 0; q < picks[s].size(); ++q) {
                        if (q == k) continue;
                        for (int o : picks[s][q]) {
                            const uint8_t* m = obj(s, o, t);
                            for (size_t i = 0; i < px; ++i)
                                if (m[i] > 127) tmp[i] = 1;
                        }
                    }
                    DilateMask(tmp.data(), grown.data(), aw, ah, 2);
                    uint8_t* dst = others.data() + px * (bounds[s] + t);
                    for (size_t i = 0; i < px; ++i) dst[i] = grown[i] ? 255 : 0;
                }
            }
            r.protects.push_back(writeMask(others, L"_x" + std::to_wstring(k + 1)));
        }
    }
    r.ok = true;
    r.video = video;
    r.bounds = bounds;
    r.w = aw;
    r.h = ah;
    SetWritePhase("");
    std::lock_guard<std::mutex> lock(g_peopleMaskMutex);
    g_peopleMasks[key] = r;
    return r;
}


// The swap's graph (BuildSwap) changed so only the masked people are made again: the clip's frames at the video's size, a SAM 3 track per person
// (masks added), cleaned and grown, reduced to the latent's grid, on the VAE-encoded clip; the clip is <Video 1> (its motion) instead of the guide.
// With `people` (BuildPeopleMasks' video, uploaded into node 80) the masks come from it, already mended, and are grown less; without, a SAM 3
// track per noun is made in the graph (one shot only: it cannot follow anyone across a cut). The guide stays: masked alone, the model once
// put the boy where only the girl was masked, in a fast shot at the end; with the guide every shot had the right person (cafeteria clip).
void MaskSwap(json& wf, const std::vector<std::string>& targets, const std::string& sam3, int w, int h, bool people, bool protect = false) {
    auto node = [&](const std::string& id, const char* cls, json inputs) { wf[id] = {{"class_type", cls}, {"inputs", std::move(inputs)}, {"_meta", {{"title", cls}}}}; };
    node("43", "ImageScale", {{"image", json::array({"42", 0})}, {"upscale_method", "lanczos"}, {"width", w}, {"height", h}, {"crop", "center"}});
    if (people) {
        node("80", "LoadVideo", {{"file", "example.mp4"}});
        node("81", "GetVideoComponents", {{"video", json::array({"80", 0})}});
        node("82", "ImageFromBatch", {{"image", json::array({"81", 0})}, {"batch_index", 0}, {"length", wf["42"]["inputs"]["length"]}});
        node("83", "ImageScale", {{"image", json::array({"82", 0})}, {"upscale_method", "bilinear"}, {"width", w}, {"height", h}, {"crop", "center"}});
        node("84", "ImageToMask", {{"image", json::array({"83", 0})}, {"channel", "red"}});
        node("63", "ThresholdMask", {{"mask", json::array({"84", 0})}, {"value", 0.5}});
    }
    if (!people) node("60", "CheckpointLoaderSimple", {{"ckpt_name", sam3}});
    // one track per noun (the masks are added together anyway), with one object more than people of that noun: tested, with exactly one a
    // dancer turning away was lost at frame 71 and never found again (61 of 132 frames unmasked: she came back as herself); with a spare slot
    // and a lower threshold none was lost, on both clips tried, and "man" / "woman" each masked the right one of two people
    std::vector<std::pair<std::string, int>> nouns;
    for (const std::string& t : targets) {
        auto it = std::find_if(nouns.begin(), nouns.end(), [&](const auto& n) { return n.first == t; });
        if (it == nouns.end()) nouns.emplace_back(t, 1);
        else ++it->second;
    }
    json last;
    for (size_t t = 0; !people && t < nouns.size() && t < 4; ++t) {
        const std::string c = "61" + std::to_string(t), v = "62" + std::to_string(t), m = "63" + std::to_string(t);
        node(c, "CLIPTextEncode", {{"clip", json::array({"60", 1})}, {"text", nouns[t].first}});
        node(v, "SAM3_VideoTrack", {{"images", json::array({"43", 0})}, {"model", json::array({"60", 0})}, {"detection_threshold", 0.3}, {"max_objects", nouns[t].second + 1},
                                    {"detect_interval", 1}, {"conditioning", json::array({c, 0})}});
        node(m, "SAM3_TrackToMask", {{"track_data", json::array({v, 0})}, {"object_indices", ""}});
        if (last.is_null()) {
            last = json::array({m, 0});
        } else {
            const std::string add = "69" + std::to_string(t);
            node(add, "MaskComposite", {{"destination", last}, {"source", json::array({m, 0})}, {"x", 0}, {"y", 0}, {"operation", "add"}});
            last = json::array({add, 0});
        }
    }
    if (!people) node("63", "ThresholdMask", {{"mask", last}, {"value", 0.5}});
    node("64", "MVEx_MaskCleanup", {{"masks", json::array({"63", 0})}, {"threshold", 0.5}, {"method", "shrink_grow"}, {"method.shrink", 4}, {"method.min_frames", 1}, {"edge_grow", 4}});
    // grown wide and soft: fast hands blur past a tight mask and kept the clip's (tested: 16 px left a raised hand the original's, 32 px took it)
    // (the people video is already grown in time and by the motion next to each person: less here)
    node("65", "GrowMaskWithBlur", {{"mask", json::array({"64", 0})}, {"expand", people ? 16 : 32}, {"incremental_expandrate", 0.0}, {"tapered_corners", true}, {"flip_input", false},
                                    {"blur_radius", people ? 8.0 : 10.0}, {"lerp_alpha", 1.0}, {"decay_factor", 1.0}});
    node("70", "VAEEncode", {{"pixels", json::array({"43", 0})}, {"vae", json::array({"5", 0})}});
    json final = json::array({"65", 0});
    if (protect) { // one person at a time: the other people (node 85's video) are taken out of the grown mask
        node("85", "LoadVideo", {{"file", "example.mp4"}});
        node("86", "GetVideoComponents", {{"video", json::array({"85", 0})}});
        node("87", "ImageFromBatch", {{"image", json::array({"86", 0})}, {"batch_index", 0}, {"length", wf["42"]["inputs"]["length"]}});
        node("88", "ImageScale", {{"image", json::array({"87", 0})}, {"upscale_method", "bilinear"}, {"width", w}, {"height", h}, {"crop", "center"}});
        node("89", "ImageToMask", {{"image", json::array({"88", 0})}, {"channel", "red"}});
        node("66", "MaskComposite", {{"destination", json::array({"65", 0})}, {"source", json::array({"89", 0})}, {"x", 0}, {"y", 0}, {"operation", "subtract"}});
        final = json::array({"66", 0});
    }
    if (people) { // the made frames pasted into the clip's through the mask: outside it, the clip's (or the earlier pass's) own pixels exactly
        node("67", "ImageCompositeMasked", {{"destination", json::array({"43", 0})}, {"source", json::array({"16", 0})}, {"x", 0}, {"y", 0}, {"resize_source", false},
                                            {"mask", final}});
        wf["18"]["inputs"]["images"] = json::array({"67", 0});
    }
    node("71", "MVEx_MaskToLatentSpace", {{"masks", final}, {"compression", "auto"}, {"vae", json::array({"5", 0})}, {"spatial_method", "max"},
                                          {"temporal_method", "max"}, {"grow_spatial", 0}, {"grow_temporal", 0}, {"grow_temp_mode", "both"}});
    node("72", "SetLatentNoiseMask", {{"samples", json::array({"70", 0})}, {"mask", json::array({"71", 0})}});
    node("73", "LTXVSeparateAVLatent", {{"av_latent", json::array({"8", 1})}});
    node("74", "LTXVConcatAVLatent", {{"video_latent", json::array({"72", 0})}, {"audio_latent", json::array({"73", 1})}});
    wf["8"]["inputs"]["ref_videos.ref_video_0"] = json::array({"43", 0});
    wf["9"]["inputs"]["latent"] = json::array({"74", 0}); // (the guide's one output is the conditioning)
    wf["14"]["inputs"]["conditioning"] = json::array({"9", 0});
    wf["15"]["inputs"]["latent_image"] = json::array({"74", 0});
}

// A masked swap back from ComfyUI: the first shot where the masked people came back as the clip's own (inside their masks, shrunk a little,
// over 24 dB like the clip in most of the shot's frames), or -1. Tested on four takes of the cafeteria clip: a take that kept the boy for a
// whole shot read 24/24 frames, every swapped shot 0.
int ShotKeptOriginal(const std::wstring& video, const SwapTake& t) {
    if (t.shots.size() < 2 || t.peopleW <= 0) return -1;
    const int n = t.shots.back(), w = t.peopleW, h = t.peopleH;
    const size_t px = static_cast<size_t>(w) * h;
    std::vector<uint8_t> out, src, mask;
    if (!DecodeRaw(video, n, w, h, true, out) || !DecodeRaw(t.clip, n, w, h, true, src) || !DecodeRaw(t.people, n, w, h, true, mask)) return -1;
    const int frames = static_cast<int>(std::min({out.size(), src.size(), mask.size()}) / px);
    std::vector<uint8_t> inv(px), grown(px);
    for (size_t s = 0; s + 1 < t.shots.size(); ++s) {
        const bool brief = t.shots[s + 1] - t.shots[s] < 8; // under a third of a second: only someone big in it is worth a second take
        int kept = 0, looked = 0;
        double area = 0;
        for (int f = t.shots[s]; f < t.shots[s + 1] && f < frames; ++f) {
            const uint8_t* m = mask.data() + px * f;
            for (size_t i = 0; i < px; ++i) inv[i] = m[i] <= 127;
            DilateMask(inv.data(), grown.data(), w, h, 7); // inside, away from the soft edge
            double err = 0;
            size_t cnt = 0;
            for (size_t i = 0; i < px; ++i)
                if (!grown[i]) {
                    const double d = static_cast<double>(out[px * f + i]) - src[px * f + i];
                    err += d * d;
                    ++cnt;
                }
            if (cnt < 400) continue;
            ++looked;
            area += static_cast<double>(cnt) / px;
            if (10.0 * std::log10(255.0 * 255.0 / std::max(1e-6, err / cnt)) > 24.0) ++kept;
        }
        // (seen: a 4-frame shot of the boy small at the edge flagged both takes of his pass; the girl kept as herself in the last 4 and 7
        // frames, big in the frame, is a miss everyone sees)
        if (brief && looked > 0 && area / looked < 0.03) continue;
        if (looked >= 3 && kept * 2 > looked) return static_cast<int>(s);
    }
    return -1;
}

// A masked swap back from ComfyUI: the first new person, in the first shot, who does not look like their picture (asked of the prompt writer
// with their picture and a crop of the take), or -1. Seen: the boy seen from behind came back with the original's dark hair and sweater -
// redrawn, so ShotKeptOriginal passed it - instead of Ben's buzzed hair and denim shirt.
int ShotWrongPerson(const std::wstring& video, const SwapTake& t, int& subject) {
    if (t.boxes.empty() || t.shots.size() < 2 || t.peopleW <= 0 || !WriterConfigured()) return -1;
    const int n = t.shots.back(), w = t.peopleW, h = t.peopleH;
    std::vector<uint8_t> rgb;
    if (!DecodeRaw(video, n, w, h, false, rgb)) return -1;
    const ClefSession session = WriterAcquire(t.comfyUrl, nullptr);
    if (!session.ok || !WriterCanSee()) {
        WriterRelease(session, false);
        return -1;
    }
    const std::wstring dir = DataDir() + L"\\h3\\masks";
    int bad = -1;
    for (const WhoBox& b : t.boxes) {
        if (b.subject >= static_cast<int>(t.job.who.size()) || static_cast<size_t>(b.frame + 1) * w * h * 3 > rgb.size()) continue;
        // a person under 3% of the frame is not worth a whole second take (seen: the boy small at the edge of a 4-frame shot flagged both
        // takes of his pass)
        if (static_cast<double>(b.x1 - b.x0) * (b.y1 - b.y0) < 0.03 * w * h) continue;
        const int padX = (b.x1 - b.x0) / 6 + 4, padY = (b.y1 - b.y0) / 8 + 4;
        const int x0 = std::max(0, b.x0 - padX), y0 = std::max(0, b.y0 - padY), x1 = std::min(w - 1, b.x1 + padX), y1 = std::min(h - 1, b.y1 + padY);
        const int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
        std::vector<uint8_t> crop(static_cast<size_t>(cw) * ch * 4, 255);
        const uint8_t* fr = rgb.data() + static_cast<size_t>(b.frame) * w * h * 3;
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x) memcpy(&crop[(static_cast<size_t>(y) * cw + x) * 4], &fr[((static_cast<size_t>(y0) + y) * w + x0 + x) * 3], 3);
        const std::wstring png = dir + L"\\took_s" + std::to_wstring(b.shot) + L"_p" + std::to_wstring(b.subject + 1) + L".png";
        if (!WriteThumbnailPng(png, crop.data(), cw, ch)) continue;
        std::string out, err;
        if (!WriterChat("Picture 1 shows a person. Picture 2 is cut from a frame of a video and shows one person, maybe from behind or from the side. "
                        "Does the person in picture 2 have the hair and the clothes of the person in picture 1 (the same colours, the same kind of "
                        "top, the same haircut)? Judge only hair and clothes, not the pose, the face angle or the light. Reply with one word: yes or no.",
                        "Picture 1, then picture 2. Same hair and clothes?", {ImageDataUrl(t.job.who[b.subject], 512), ImageDataUrl(png, 384)}, 10, 0.0, out, &err))
            continue;
        std::string low = Trimmed(out);
        for (char& c : low) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        std::ofstream(png + L".txt", std::ios::binary) << out;
        if (low.rfind("no", 0) == 0) {
            bad = b.shot;
            subject = b.subject;
            break;
        }
    }
    WriterRelease(session, false);
    return bad;
}

// A part with cuts: the prompt says who is on screen in each shot (from BuildPeopleMasks) in place of the writer's guess at the action
std::string WithShotTimeline(const std::string& prompt, const std::string& timeline) {
    const std::string line = "Shot by shot (the cuts of the guide), who is on screen and what their faces do: " + timeline + ".";
    const size_t d = prompt.find("detailed_description:\n");
    if (d == std::string::npos) return prompt;
    const size_t many = prompt.find(" Each of them matches the guide's", d), one = prompt.find("Match the guide's", d);
    const size_t e = many != std::string::npos ? many : one;
    if (e == std::string::npos) return prompt;
    size_t b = std::string::npos; // the end of the "... becomes <Subject k>." sentences
    for (size_t at = prompt.find("becomes <Subject ", d); at != std::string::npos && at < e; at = prompt.find("becomes <Subject ", at + 1)) {
        const size_t dot = prompt.find(">.", at);
        if (dot != std::string::npos && dot < e) b = dot + 2;
    }
    if (b == std::string::npos) return prompt;
    return prompt.substr(0, b) + " " + line + (many != std::string::npos ? "" : " ") + prompt.substr(many != std::string::npos ? e : e);
}

// What a new person carries stays with them: "<Subject 1> keeps a brown leather messenger bag (as in <Picture 1>) in every frame". Seen:
// the person the swap replaced had no bag, and once Ben's arm swung over it the model dropped the bag for the rest of the clip.
std::string WithCarriedThings(std::string prompt, size_t people) {
    static const char* const things[] = {"bag", "backpack", "purse", "satchel", "tote", "guitar", "umbrella", "cape", "cloak", "sword", "skateboard"};
    std::string add;
    for (size_t k = 0; k < std::max<size_t>(1, people); ++k) {
        const std::string tag = "<Subject " + std::to_string(k + 1) + "> is the person shown in";
        const size_t a = prompt.find(tag);
        if (a == std::string::npos) continue;
        const size_t b = prompt.find('\n', a);
        const std::string look = prompt.substr(a, b == std::string::npos ? std::string::npos : b - a);
        std::string low = look;
        for (char& c : low) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        for (const char* t : things) {
            size_t at = low.find(t);
            while (at != std::string::npos && at > 0 && isalpha(static_cast<unsigned char>(low[at - 1]))) at = low.find(t, at + 1); // not "handbags" inside a word start
            if (at == std::string::npos) continue;
            size_t from = at; // back to its article: "a brown leather messenger bag"
            for (int words = 0; words < 6 && from > 1; ++words) {
                const size_t sp = low.rfind(' ', from - 2); // the space before the word before
                const size_t ws = sp == std::string::npos ? 0 : sp + 1;
                const std::string w = low.substr(ws, from - 1 - ws);
                if (w.empty() || strchr(",.;:", w.back())) break;
                from = ws;
                if (w == "a" || w == "an" || w == "the" || w == "his" || w == "her" || w == "their") break;
            }
            size_t end = at + strlen(t);
            while (end < low.size() && isalpha(static_cast<unsigned char>(low[end]))) ++end;
            std::string thing = Trimmed(look.substr(from, end - from));
            if (thing.empty() || thing.size() > 60) continue;
            add += " <Subject " + std::to_string(k + 1) + "> keeps " + thing + " (as in <Picture " + std::to_string(k + 1) +
                   ">) on them in every frame, also while their arms move over it.";
            break;
        }
    }
    if (add.empty()) return prompt;
    const size_t at = prompt.find(" Do not use the reference picture");
    if (at != std::string::npos) prompt.insert(at, add);
    else prompt += add;
    return prompt;
}

// How much wider than the person they replace each new person may be (a share of that person's width): more when their look in the prompt has
// something they carry or wear wide
std::vector<float> RoomFor(const std::string& prompt, size_t people) {
    std::vector<float> out(std::max<size_t>(1, people), 0.10f);
    std::string low = prompt;
    for (char& c : low) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    for (size_t k = 0; k < out.size(); ++k) {
        const std::string tag = "<subject " + std::to_string(k + 1) + "> is the person shown in";
        const size_t a = low.find(tag);
        if (a == std::string::npos) continue;
        const size_t b = low.find('\n', a);
        const std::string look = low.substr(a, b == std::string::npos ? std::string::npos : b - a);
        for (const char* w : {"bag", "backpack", "purse", "satchel", "tote", "guitar", "case", "umbrella", "cape", "cloak", "trench", "long coat",
                              "gown", "ball dress", "wings", "sword", "skateboard"}) {
            if (look.find(w) != std::string::npos) out[k] = 0.20f;
        }
    }
    return out;
}

// "the boy in the dark sweater sitting at a table" -> "the boy sitting at a table": the clothes and hair of the person being replaced find
// them in the clip, but in the prompt they were drawn back onto the new person (Ben, seen from the side, came back with the original's dark
// hair and dark sweater in four takes; the test whose prompt said only "the boy sitting at a table" drew him right)
std::string ShortTarget(const std::string& phrase) {
    static const char* const looks[] = {"shirt", "top", "sweater", "hoodie", "jacket", "coat", "dress", "skirt", "jeans", "pants", "trousers", "shorts",
                                        "suit", "uniform", "blouse", "cardigan", "vest", "tank", "sweatshirt", "jersey", "outfit", "hat", "cap",
                                        "glasses", "scarf", "boots", "shoes", "sneakers", "hair", "ponytail", "beard", "floral", "striped", "leather",
                                        "denim", "sleeve", "collar", "tie", "bag", "backpack", "clothes", "clothing"};
    std::istringstream in(phrase);
    std::vector<std::string> w;
    for (std::string x; in >> x;) w.push_back(x);
    auto isLook = [&](std::string x) {
        while (!x.empty() && !isalpha(static_cast<unsigned char>(x.back()))) x.pop_back();
        for (char& c : x) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (x.size() > 2 && x.back() == 's' && x[x.size() - 2] != 's' && x != "glasses" && x != "jeans" && x != "pants" && x != "trousers" && x != "shorts" && x != "boots" && x != "shoes" && x != "sneakers" && x != "clothes")
            x.pop_back();
        for (const char* l : looks)
            if (x == l || (std::string(l) == "shirt" && x == "t-shirt")) return true;
        return false;
    };
    static const char* const starts[] = {"in", "wearing", "with", "dressed"};
    static const char* const stops[] = {"sitting", "standing", "walking", "carrying", "holding", "dancing", "running", "talking", "eating", "at", "on",
                                        "near", "by", "behind", "next", "who", "that", "while", "through", "across", "seated", "lying", "leaning",
                                        "kneeling", "looking", "smiling", "laughing", "singing", "playing", "riding", "driving", "jumping"};
    std::vector<std::string> out;
    for (size_t i = 0; i < w.size();) {
        bool start = false;
        for (const char* s : starts) start = start || w[i] == s;
        if (start && i > 0) { // "in the dark sweater" up to the next action or place; dropped only when it names clothes or hair
            size_t e = i + 1;
            bool look = false;
            while (e < w.size()) {
                bool stop = false;
                for (const char* s : stops) stop = stop || w[e] == s;
                for (const char* s : starts) stop = stop || w[e] == s; // the next clause ("in the center with long dark hair")
                if (stop) break;
                look = look || isLook(w[e]);
                if (w[e].back() == ',') { // a comma ends it too
                    ++e;
                    break;
                }
                ++e;
            }
            if (look) {
                i = e;
                continue;
            }
        }
        out.push_back(w[i++]);
    }
    std::string r;
    for (const std::string& x : out) r += (r.empty() ? "" : " ") + x;
    return r.size() >= 3 ? r : phrase;
}

// The prompt of a masked swap with each target said without the replaced person's clothes and hair (ShortTarget)
std::string WithoutOldLooks(std::string prompt, size_t people) {
    for (const std::string& phrase : MaskPhrases(prompt, people)) {
        const std::string shorter = ShortTarget(phrase);
        if (phrase.empty() || shorter == phrase) continue;
        for (size_t at = 0;;) { // every place it is said, whatever its first letter's case
            size_t found = std::string::npos;
            for (size_t i = at; i + phrase.size() <= prompt.size(); ++i)
                if (_strnicmp(prompt.c_str() + i, phrase.c_str(), phrase.size()) == 0) {
                    found = i;
                    break;
                }
            if (found == std::string::npos) break;
            std::string rep = shorter;
            if (isupper(static_cast<unsigned char>(prompt[found]))) rep[0] = static_cast<char>(toupper(static_cast<unsigned char>(rep[0])));
            prompt.replace(found, phrase.size(), rep);
            at = found + rep.size();
        }
    }
    return prompt;
}

// One person at a time: the prompt of person k's own pass, from the prompt for everyone - their look, whom they replace (said without that
// person's clothes), their own shots and faces as <Subject 1>, and the body notes the prompt had for them. Tested 2026-10-17 on the cafeteria
// clip: every take that swapped both at once put the wrong person in some shot (Ava in the boy's close-up, Ben in the girl's shots); the two
// passes put the right one in every shot, kept the first pass's Ava through the second, and her smile and open-mouthed surprise came through.
std::string OnePersonPrompt(const std::string& all, int k, size_t people, const std::string& timeline, const std::string& emphasis) {
    const std::string sk = "<Subject " + std::to_string(k + 1) + ">";
    const std::string tag = sk + " is the person shown in <Picture " + std::to_string(k + 1) + ">, ";
    std::string look;
    if (const size_t a = all.find(tag); a != std::string::npos) {
        const size_t e = all.find('\n', a);
        look = all.substr(a + tag.size(), e == std::string::npos ? std::string::npos : e - a - tag.size());
        if (const size_t cut = look.find(" Their face, hair"); cut != std::string::npos) look.resize(cut);
        while (!look.empty() && (look.back() == '.' || look.back() == ' ')) look.pop_back();
    }
    if (look.size() < 10) look = "with exactly the face, hair, skin tone, body and clothes that picture shows";
    const std::vector<std::string> phrases = MaskPhrases(all, people);
    const std::string target = k < static_cast<int>(phrases.size()) && !phrases[k].empty() ? ShortTarget(phrases[k]) : std::string("the person");
    std::string bodies; // (the notes for a man in a woman's place and the like, said for them)
    if (const size_t a = all.find("do not mix the new people up."); a != std::string::npos) {
        const size_t e = all.find(" Do not use the reference", a);
        const std::string rest = all.substr(a + 29, e == std::string::npos ? std::string::npos : e - a - 29);
        size_t from = 0;
        while (from < rest.size()) {
            size_t end = rest.find(". ", from);
            end = end == std::string::npos ? rest.size() : end + 1;
            const std::string sentence = rest.substr(from, end - from);
            if (sentence.find(sk) != std::string::npos && sentence.find(" keeps ") == std::string::npos) bodies += ReplaceAll(sentence, sk, "<Subject 1>");
            from = end;
        }
    }
    while (!bodies.empty() && bodies.back() == ' ') bodies.pop_back();
    return "body_swap:\nsubject_definitions:\n" + (emphasis.empty() ? std::string() : emphasis + "\n") + "<Subject 1> is the person shown in <Picture 1>, " + look +
           ". Their face, hair, skin tone, body proportions and clothing define the replacement character.\n"
           "summary:\n[video generation + reference] Replace " + target + " in the aligned guide with <Subject 1>. Follow the source performance, camera "
           "and editing timing frame by frame.\n"
           "retention_analysis:\n<Subject 1>: fully_preserved - Preserve the face, skin tone, hair, body and outfit of <Picture 1>.\n"
           "Guide scene: fully_preserved - Preserve the source environment, background, props, lighting, camera, framing, motion, timing and cuts.\n"
           "detailed_description:\nOnly " + target + " becomes <Subject 1>; everyone else stays exactly as in the guide." +
           (timeline.empty() ? std::string() : " Shot by shot (the cuts of the guide), when <Subject 1> is on screen and what their face does: " + timeline + ".") +
           " Match the guide's complete body movement, limb poses, hand contact, head direction and facial expressions naturally; <Subject 1>'s hair "
           "and clothing follow the moving body. Keep <Subject 1>'s own face, features and skin tone exactly as in <Picture 1>: do not blend in the "
           "original performer's face, hair, skin or clothes." + (bodies.empty() ? std::string() : " " + bodies) +
           " Do not use the reference picture's background. No additional people, no added text or watermark.";
}

// Swaps waiting for their person masks (made on a worker thread, then sent)
struct PeopleWait {
    WriteJob job;
    std::string prompt;
    std::wstring clip;
    bool retrying = false;
};
std::deque<PeopleWait> g_peopleWait;
std::deque<PeopleWait> g_comfyWait; // swaps waiting for ComfyUI to answer (what it has decides how they are made)
bool MaskOk(); // (below: person masking installed, from ComfyUI or its folders)
double g_comfyWaitSince = 0;
std::future<PeopleMask> g_maskFut;

bool SendStar(AppState& app, const WriteJob& j, const std::string& prompt, const std::wstring& clip, const PeopleMask* people = nullptr) {
    StepLog(std::string("send ") + (j.starMethod == 0 ? "swap" : "recreate") + (people ? " (people masks ready)" : "") + ": " + j.label);
    std::string lora;
    for (const char* l : kRefLoras) {
        if (HasLora(l)) {
            lora = l;
            break;
        }
    }
    // the video's size: the clip's shape at the chosen resolution
    const VideoInfo vi = ProbeVideo(clip);
    H3Options o = j.opt;
    o.shape = 0;
    int w = 0, h = 0;
    H3Size(o, vi.width > 0 ? vi.width : 16, vi.height > 0 ? vi.height : 9, w, h);
    WriteJob run = j;
    run.opt.veda = VedaFor(kVedaStarPredictor);
    const bool swap = j.starMethod == 0;
    std::string text = WithLoraTriggers(app, prompt, app.comfy.h3Loras); // (your H3 LoRAs' trigger words)
    if (swap) {
        lora = SwapTurbo();
        if (Trimmed(text).rfind("body_swap:", 0) != 0) text = "body_swap:\n" + text; // the LoRA's trigger, also on a prompt typed by hand
    }
    // Recreate: a character whose sheet shows what its picture cannot (a back unlike its front) goes in as its sheet. Tested 2026-10-16: in
    // reference-to-video that sheet drew a costume's back as designed instead of guessing it; for characters that look alike from behind the
    // sheet changed nothing or was worse, in a swap it let the clip's performer leak back in, and in edits a whole sheet brought its several
    // figures into the picture - so those keep the picture.
    std::vector<std::wstring> refs = j.who;
    if (!swap && g_v.starUseSheet && !refs.empty() && CharacterSheetUseful(refs[0])) {
        const CharacterProfile* cp = FindProfile(refs[0]);
        std::error_code ec2;
        if (cp && !cp->sheet.empty() && fs::exists(cp->sheet, ec2)) {
            refs[0] = cp->sheet;
            text = "<Picture 1> is the character sheet of <Subject 1>: front, side and back views and four expressions of the same person - use it for "
                   "how <Subject 1> looks from every angle; one <Subject 1> in the video.\n" + text;
        }
    }
    // one person at a time (several people, masked): this pass swaps only who[pass], with their own masks and a one-person prompt
    bool oneByOne = swap && people && people->ok && g_v.oneAtATime && j.who.size() > 1 && people->videos.size() == j.who.size() &&
                    people->timelines.size() == j.who.size();
    for (size_t k = 0; oneByOne && k < people->videos.size(); ++k) oneByOne = !people->videos[k].empty();
    const int pass = oneByOne ? std::clamp(j.pass, 0, static_cast<int>(j.who.size()) - 1) : -1;
    if (oneByOne) {
        run.who = {j.who[pass]};
        refs = run.who;
    }
    json wf = swap ? BuildSwap(run, w, h, lora, ComfyHasNode(app.comfy.url, "MiniMaxChunkFeedForward") == 1) : BuildStar(run, w, h, !lora.empty(), lora, g_ref.omni);
    ApplyLoras(wf, app.comfy.h3Loras, false); // your MiniMax H3 LoRAs (the Video page's LoRAs row)
    // only the person made again (the masked swap), when it is set up and the part has no hard cut (SAM 3 cannot follow people across one)
    bool masked = false;
    static const bool testNoMask = GetEnvironmentVariableW(L"MV_SWAP_NOMASK", nullptr, 0) > 0; // tests: the guide swap, to compare
    std::wstring peopleVideo, protectVideo;
    // ComfyUI not answering yet, but person masking is installed: wait for it (starting it) before choosing how to swap - chosen blind, the
    // swap went out the old way, without masking and without the faces pass
    if (swap && g_v.swapMask && !testNoMask && !g_v.vedaKnown && MaskOk() && !people) {
        if (g_comfyWait.empty()) g_comfyWaitSince = ImGui::GetTime();
        g_comfyWait.push_back({j, prompt, clip, g_swapRetrying});
        std::string message;
        ComfyStart(app.comfy, message);
        g_v.vedaAsked = -1000.0;
        app.SetStatus("Starting ComfyUI first, to see what it has (person masking, facial expressions); then the swap is sent");
        return true;
    }
    if (swap && g_v.swapMask && !testNoMask && g_v.vedaKnown && g_v.vedaInfo.maskvid && !g_v.vedaInfo.sam3.empty()) {
        if (!people && (WriterConfigured() || (j.scan && !j.picks.empty()))) { // first the people's masks (BuildPeopleMasks), then the swap
            g_peopleWait.push_back({j, prompt, clip, g_swapRetrying});
            app.SetStatus("Finding the people in every shot of the clip, then the swap is sent");
            Toast(ToastKind::Info, "Finding the people in the clip", j.label + ": SAM 3 tracks everyone shot by shot and the prompt writer picks whom to replace",
                  {}, [](AppState& s) { s.requestTab = 10; }, "videosend");
            return true;
        }
        if (people && people->ok && oneByOne) {
            const bool protect = pass < static_cast<int>(people->protects.size()) && !people->protects[pass].empty();
            MaskSwap(wf, {}, std::string(), w, h, true, protect);
            if (protect) protectVideo = people->protects[pass];
            peopleVideo = people->videos[pass];
            masked = true;
            text = WithCarriedThings(OnePersonPrompt(text, pass, j.who.size(), people->timelines[pass], j.emphasis), 1);
        } else if (people && people->ok) {
            MaskSwap(wf, {}, std::string(), w, h, true);
            peopleVideo = people->video;
            masked = true;
            if (people->shots > 1 && !people->timeline.empty()) text = WithShotTimeline(text, people->timeline);
            text = WithoutOldLooks(text, j.who.size());
            text = WithCarriedThings(text, j.who.size());
        } else {
            const int cuts = CountCuts(clip);
            const std::string why = people && !people->note.empty() ? " (" + people->note + ")" : std::string();
            if (cuts == 0) {
                MaskSwap(wf, MaskTargets(text, j.who.size()), g_v.vedaInfo.sam3, w, h, false);
                masked = true;
                if (!why.empty()) app.SetStatus("The person mask was made the simple way" + why);
            } else if (cuts > 0) {
                app.SetStatus("This part of the clip has " + std::to_string(cuts) + (cuts == 1 ? " cut" : " cuts") + ": swapped the usual way" + why);
            }
        }
    }
    wf["8"]["inputs"]["prompt"] = text; // (it is put there again as the job is sent; here so the saved file shows what was sent)
    static int n = 0;
    const std::wstring dir = DataDir() + L"\\h3";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::wstring file = dir + L"\\star_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(++n) + L".json";
    {
        std::ofstream f(file, std::ios::trunc);
        if (!f) return false;
        f << wf.dump(1);
    }
    std::vector<std::pair<std::string, std::wstring>> pics;
    for (size_t k = 0; k < refs.size() && k < 9; ++k) pics.emplace_back(std::to_string(30 + k), refs[k]);
    pics.emplace_back("40", clip);
    if (!peopleVideo.empty()) pics.emplace_back("80", peopleVideo);
    if (!protectVideo.empty()) pics.emplace_back("85", protectVideo);
    const bool omni = !swap && j.starQuality == 0 && g_ref.omni;
    const bool fast = j.starQuality == 0 && (!lora.empty() || omni);
    const std::string passName = !oneByOne ? std::string() : pass < static_cast<int>(j.names.size()) && !j.names[pass].empty() ? j.names[pass] : "person " + std::to_string(pass + 1);
    const bool sent = SendH3Workflow(app, file, std::wstring(), pics, text, j.opt.seconds, oneByOne ? 1 : j.opt.times,
                                     std::string(swap ? "Video (Swap" : "Video (Star") + (masked ? ", masked" : "") +
                                         (oneByOne ? ", " + std::to_string(pass + 1) + " of " + std::to_string(j.who.size()) + ": " + passName : std::string()) +
                                         (omni ? ", FastH3-Omni" : fast ? "" : ", best") + (run.opt.veda ? ", Veda" : "") + "): " + j.label);
    if (sent) Toast(ToastKind::Info, swap ? "Swapping in the new person" : "Putting you in the video", j.label, {}, [](AppState& s) { s.requestTab = 10; }, "videosend");
    if (sent && swap) {
        SwapTake t;
        t.act = LastEditActivity();
        t.job = {j.who, j.refVideo, j.refStart, j.keepSound, j.idea, j.label, j.opt, j.starQuality, j.scan, j.picks, j.longId, j.longPart};
        t.clip = clip;
        t.prompt = text;
        t.retry = !g_swapRetrying;
        if (!peopleVideo.empty() && people) t.people = peopleVideo, t.peopleW = people->w, t.peopleH = people->h, t.shots = people->bounds, t.boxes = people->boxes;
        t.comfyUrl = app.comfy.url;
        if (oneByOne) { // this pass: its own person (checked against their picture), and what the next pass needs
            t.pass = pass;
            t.chainJob = j;
            if (t.chainJob.origClip.empty()) t.chainJob.origClip = clip;
            t.chainPrompt = prompt;
            t.chainPeople = std::make_shared<PeopleMask>(*people);
            t.job.who = run.who;
            std::vector<WhoBox> mine;
            for (WhoBox b : people->boxes)
                if (b.subject == pass) b.subject = 0, mine.push_back(b);
            t.boxes = mine;
        }
        g_swapTakes.push_back(std::move(t));
    }
    return sent;
}

void ModeTabs(AppState& app) {
    static const char* names[] = {"Animate a picture", "Star in a video", "Make a story"};
    if (g_mode > 2) g_mode = 0;
    for (int m = 0; m < 3; ++m) {
        if (m) ImGui::SameLine(0.0f, ui::Dp(4.0f));
        if (Chip(names[m], g_mode == m)) g_mode = m;
    }
    // characters are made in the Characters tab: from there they are used everywhere (Clef, the Vision board, edits, here)
    ImGui::SameLine(0.0f, ui::Dp(4.0f));
    if (ImGui::Button("Create a character...##videomodes")) OpenCharacterCreator(app);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("In the Characters tab: then animate it, star it in a video, or use it in Clef mode and on the Vision board");
    ImGui::Spacing();
}


// ---- Star in a video: the form (inside the scrolling form) and its footer
// ---- Your swap clips: a folder of clips (dances, music videos...) shown as a grid on the page, newest first
struct SwapLib {
    std::wstring dir;                       // the folder scanned
    std::vector<std::wstring> files;
    double scannedAt = -1000.0;
    std::map<std::wstring, VideoInfo> info; // length and size, probed in the background (kept in swap_cache.json)
    std::future<std::vector<std::pair<std::wstring, VideoInfo>>> probe;
};
SwapLib g_lib;

bool IsClipFile(const fs::path& p) {
    std::wstring e = p.extension().wstring();
    std::transform(e.begin(), e.end(), e.begin(), ::towlower);
    return e == L".mp4" || e == L".mov" || e == L".webm" || e == L".mkv" || e == L".m4v" || e == L".avi";
}

void ScanSwapLib() {
    if (g_lib.dir == g_swapFolder && ImGui::GetTime() - g_lib.scannedAt < 10.0) return;
    g_lib.dir = g_swapFolder;
    g_lib.scannedAt = ImGui::GetTime();
    std::vector<std::pair<fs::file_time_type, std::wstring>> found;
    std::error_code ec;
    if (!g_swapFolder.empty()) {
        for (const auto& e : fs::directory_iterator(g_swapFolder, ec)) {
            if (e.is_regular_file(ec) && IsClipFile(e.path())) found.emplace_back(e.last_write_time(ec), e.path().wstring());
        }
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    g_lib.files.clear();
    for (auto& f : found) g_lib.files.push_back(std::move(f.second));
    // lengths: from the cache, the rest probed in the background
    std::vector<std::wstring> todo;
    {
        std::lock_guard<std::mutex> lock(g_swapCacheMutex);
        const json& c = SwapCacheLocked();
        for (const std::wstring& f : g_lib.files) {
            if (g_lib.info.count(f)) continue;
            const std::string k = FileKey(f);
            if (c.contains("probe") && c["probe"].contains(k)) {
                VideoInfo vi;
                vi.seconds = c["probe"][k].value("s", 0.0);
                vi.width = c["probe"][k].value("w", 0);
                vi.height = c["probe"][k].value("h", 0);
                g_lib.info[f] = vi;
            } else {
                todo.push_back(f);
            }
        }
    }
    if (!todo.empty() && !g_lib.probe.valid())
        g_lib.probe = std::async(std::launch::async, [todo]() {
            std::vector<std::pair<std::wstring, VideoInfo>> out;
            for (const std::wstring& f : todo) out.emplace_back(f, ProbeVideo(f));
            return out;
        });
}

void PumpSwapLib() {
    if (!g_lib.probe.valid() || g_lib.probe.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    std::lock_guard<std::mutex> lock(g_swapCacheMutex);
    json& c = SwapCacheLocked();
    for (const auto& [f, vi] : g_lib.probe.get()) {
        g_lib.info[f] = vi;
        if (vi.seconds > 0) c["probe"][FileKey(f)] = {{"s", vi.seconds}, {"w", vi.width}, {"h", vi.height}};
    }
    SaveSwapCacheLocked();
}

void ChooseStarClip(const std::wstring& path) {
    g_starBrowse = false;
    if (_wcsicmp(path.c_str(), g_star.video.c_str()) == 0) return;
    g_star.video = path;
    g_star.start = 0;
    g_star.infoFor.clear();
}

void DrawSwapLibrary(AppState& app) {
    ScanSwapLib();
    PumpSwapLib();
    if (g_swapFolder.empty()) {
        ImGui::TextColored(ui::kTextDim, "Keep your clips to swap into in one folder, and choose them here:");
        ImGui::SameLine();
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ui::kTextDim, "Your clips (%d):", static_cast<int>(g_lib.files.size()));
        ImGui::SameLine();
        ImGui::TextUnformatted(Utf8(fs::path(g_swapFolder).filename().wstring()).c_str());
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", Utf8(g_swapFolder).c_str());
        ImGui::SameLine();
    }
    if (ImGui::SmallButton(g_swapFolder.empty() ? "Choose the folder..." : "Change...")) {
        const std::wstring d = OpenFolderDialog(GetActiveWindow());
        if (!d.empty()) g_swapFolder = d, g_lib.scannedAt = -1000.0, SaveSettings();
    }
    if (!g_swapFolder.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Open it")) ShellExecuteW(nullptr, L"open", g_swapFolder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("In Explorer: drop new clips in, they show up here");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Another file...")) {
        wchar_t buf[MAX_PATH * 2] = {};
        OPENFILENAMEW ofn = {sizeof(ofn)};
        ofn.hwndOwner = GetActiveWindow();
        ofn.lpstrFilter = L"Videos\0*.mp4;*.mov;*.webm;*.mkv;*.avi;*.m4v\0All files\0*.*\0";
        ofn.lpstrFile = buf;
        ofn.nMaxFile = MAX_PATH * 2;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&ofn)) ChooseStarClip(buf);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("A clip from anywhere (or right-click one under Videos >)");
    if (g_lib.files.empty()) return;
    // the grid: tall tiles (most clips are phone videos), two rows, scroll for more
    const float tw = ui::Dp(58.0f), th = tw * 16.0f / 9.0f, gap = ui::Dp(5.0f);
    const float avail = ImGui::GetContentRegionAvail().x;
    const int perRow = std::max(1, static_cast<int>((avail - ui::Dp(14.0f) + gap) / (tw + gap)));
    const int rows = (static_cast<int>(g_lib.files.size()) + perRow - 1) / perRow;
    ImGui::BeginChild("##swaplib", ImVec2(0, std::min(rows, 2) * (th + gap) + (rows > 2 ? gap : 0.0f)), ImGuiChildFlags_None);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (size_t i = 0; i < g_lib.files.size(); ++i) {
        const std::wstring& f = g_lib.files[i];
        if (i % perRow) ImGui::SameLine(0.0f, gap);
        ImGui::PushID(static_cast<int>(i));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##clip", ImVec2(tw, th));
        const bool visible = ImGui::IsItemVisible();
        const bool chosen = _wcsicmp(f.c_str(), g_star.video.c_str()) == 0;
        if (visible) {
            dl->AddRectFilled(p, ImVec2(p.x + tw, p.y + th), IM_COL32(0, 0, 0, 255), ui::Dp(5.0f));
            const AppState::LibThumb lt = app.LibraryThumb(f);
            if (lt.srv && lt.w > 0) {
                const float sc = std::max(tw / lt.w, th / lt.h); // fill the tile (cropped)
                const float uw = std::min(1.0f, tw / (lt.w * sc)), vh = std::min(1.0f, th / (lt.h * sc));
                dl->AddImageRounded((ImTextureID)(intptr_t)lt.srv, p, ImVec2(p.x + tw, p.y + th), ImVec2((1 - uw) * 0.5f, (1 - vh) * 0.5f),
                                    ImVec2((1 + uw) * 0.5f, (1 + vh) * 0.5f), IM_COL32_WHITE, ui::Dp(5.0f));
            }
            auto it = g_lib.info.find(f);
            if (it != g_lib.info.end() && it->second.seconds > 0) {
                const std::string d = FormatDuration(it->second.seconds);
                const ImVec2 ts = ImGui::CalcTextSize(d.c_str());
                const ImVec2 a(p.x + tw - ts.x - ui::Dp(6.0f), p.y + th - ts.y - ui::Dp(4.0f));
                dl->AddRectFilled(ImVec2(a.x - ui::Dp(3.0f), a.y - ui::Dp(1.0f)), ImVec2(a.x + ts.x + ui::Dp(3.0f), a.y + ts.y + ui::Dp(1.0f)), IM_COL32(0, 0, 0, 170), ui::Dp(3.0f));
                dl->AddText(a, IM_COL32_WHITE, d.c_str());
            }
            if (chosen || ImGui::IsItemHovered())
                dl->AddRect(p, ImVec2(p.x + tw, p.y + th), chosen ? ImGui::GetColorU32(ui::kAccent) : IM_COL32(255, 255, 255, 120), ui::Dp(5.0f), 0, ui::Dp(chosen ? 2.5f : 1.5f));
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
            auto it = g_lib.info.find(f);
            if (it != g_lib.info.end()) ImGui::SetTooltip("%s\n%s  %dx%d\nClick: use it   Double-click: play it", Utf8(fs::path(f).filename().wstring()).c_str(), FormatDuration(it->second.seconds).c_str(), it->second.width, it->second.height);
            else ImGui::SetTooltip("%s", Utf8(fs::path(f).filename().wstring()).c_str());
        }
        if (ImGui::IsItemClicked()) ChooseStarClip(f);
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) PlayClip(app, f);
        ImGui::PopID();
    }
    ImGui::EndChild();
}

// ---- The trimmer: the chosen clip as a film strip; drag the part's ends or the part itself. Its length is what H3 really makes
// (17k + 5 frames), so what is marked is exactly what goes in.
struct Strip {
    std::string forKey;
    LoadedImage img;
    int tiles = 0;
    std::future<LoadedImage> fut;
};
Strip g_strip;
double g_partSeek = -1.0;   // the player goes here once the clip is open (Play this part)
bool g_partLoop = false;    // the player loops the marked part
std::string g_precutKey;    // the part cut ahead of Make
std::future<void> g_precut;

void EnsureStrip(AppState& app, const std::wstring& clip, double seconds) {
    const std::string key = FileKey(clip);
    if (g_strip.forKey == Utf8(clip) + key && (g_strip.img.srv || g_strip.fut.valid())) {
        if (g_strip.fut.valid() && g_strip.fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) g_strip.img = g_strip.fut.get();
        return;
    }
    if (g_strip.fut.valid()) return; // the last one first
    if (g_strip.img.srv) ReleaseAfterFrame(g_strip.img.srv);
    g_strip.img = LoadedImage{};
    g_strip.forKey = Utf8(clip) + key;
    g_strip.tiles = 16;
    const std::wstring dir = DataDir() + L"\\h3\\strips";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::wstring png = dir + L"\\" + HashName(key) + L".png";
    ID3D11Device* dev = app.Device();
    g_strip.fut = std::async(std::launch::async, [png, clip, seconds, dev, dir]() {
        std::error_code ec2;
        if (!fs::exists(png, ec2)) {
            const std::wstring ff = FfmpegPath();
            if (!ff.empty()) {
                char vf[160];
                snprintf(vf, sizeof(vf), "fps=%.5f,scale=-2:120,tile=16x1", 16.0 / std::max(0.5, seconds - 0.25)); // (a little denser: all 16 before the end)
                std::string log;
                RunCaptureIdle(L"\"" + ff + L"\" -y -hide_banner -loglevel error -i \"" + clip + L"\" -vf \"" + std::wstring(vf, vf + strlen(vf)) +
                                   L"\" -frames:v 1 -an \"" + png + L"\"",
                               log, true);
                PruneCacheDir(dir, 300);
            }
        }
        LoadedImage img;
        std::string err;
        if (fs::exists(png, ec2)) LoadImageThumbnail(dev, png, 4096, img, nullptr, &err);
        return img;
    });
}

void DrawTrimmer(AppState& app) {
    H3Options& o = g_v.opt;
    const double dur = g_star.info.seconds;
    if (g_star.video.empty() || dur <= 0) return;
    const double len = std::min(SwapLength(o.seconds, dur), dur);
    double start = std::clamp(g_star.start, 0.0, std::max(0.0, dur - len));
    EnsureStrip(app, g_star.video, dur);
    const float W = ImGui::GetContentRegionAvail().x, H = ui::Dp(62.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##trim", ImVec2(W, H));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + W, p.y + H), IM_COL32(0, 0, 0, 255), ui::Dp(6.0f));
    if (g_strip.img.srv && g_strip.tiles > 0) { // the frames, each cropped to fill its slot
        const float slot = W / g_strip.tiles;
        const float tileW = static_cast<float>(g_strip.img.width) / g_strip.tiles, tileH = static_cast<float>(g_strip.img.height);
        const float a = tileW / tileH, sa = slot / H;
        float du = 1.0f, dv = 1.0f;
        if (a > sa) du = sa / a;
        else dv = a / sa;
        for (int k = 0; k < g_strip.tiles; ++k) {
            const float u0 = (k + (1 - du) * 0.5f) / g_strip.tiles, u1 = (k + (1 + du) * 0.5f) / g_strip.tiles;
            dl->AddImage((ImTextureID)(intptr_t)g_strip.img.srv, ImVec2(p.x + slot * k, p.y), ImVec2(p.x + slot * (k + 1), p.y + H), ImVec2(u0, (1 - dv) * 0.5f), ImVec2(u1, (1 + dv) * 0.5f));
        }
    }
    auto X = [&](double t) { return p.x + static_cast<float>(t / dur) * W; };
    const float xs = X(start), xe = X(start + len), hw = ui::Dp(8.0f);
    dl->AddRectFilled(p, ImVec2(xs, p.y + H), IM_COL32(0, 0, 0, 165));
    dl->AddRectFilled(ImVec2(xe, p.y), ImVec2(p.x + W, p.y + H), IM_COL32(0, 0, 0, 165));
    const ImU32 acc = ImGui::GetColorU32(ui::kAccent);
    dl->AddRect(ImVec2(xs, p.y), ImVec2(xe, p.y + H), acc, ui::Dp(4.0f), 0, ui::Dp(2.5f));
    for (float hx : {xs, xe - hw}) {
        dl->AddRectFilled(ImVec2(hx, p.y), ImVec2(hx + hw, p.y + H), acc, ui::Dp(3.0f));
        dl->AddLine(ImVec2(hx + hw * 0.5f, p.y + H * 0.35f), ImVec2(hx + hw * 0.5f, p.y + H * 0.65f), IM_COL32(255, 255, 255, 220), ui::Dp(1.5f));
    }
    // the playhead, when the player has this clip
    if (g_vid.player && g_vid.player->IsOpen() && _wcsicmp(g_vid.player->Path().c_str(), g_star.video.c_str()) == 0) {
        const float x = X(std::clamp(g_vid.player->Position(), 0.0, dur));
        dl->AddLine(ImVec2(x, p.y - ui::Dp(3.0f)), ImVec2(x, p.y + H + ui::Dp(3.0f)), IM_COL32(255, 255, 255, 235), ui::Dp(2.0f));
    }
    // dragging: an end (resizes, in 17k + 5 frame steps), the part (moves), elsewhere (the part jumps there)
    static int mode = 0;
    static double grabStart = 0, grabLen = 0;
    static float grabX = 0;
    const float mx = ImGui::GetIO().MousePos.x;
    const bool nearL = std::fabs(mx - xs) < hw * 1.4f, nearR = std::fabs(mx - xe) < hw * 1.4f;
    if (ImGui::IsItemHovered() && (nearL || nearR)) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActivated()) {
        grabX = mx;
        grabStart = start;
        grabLen = len;
        if (nearR) mode = 2;
        else if (nearL) mode = 1;
        else {
            mode = 3;
            if (mx < xs || mx > xe) grabStart = start = std::clamp((mx - p.x) / W * dur - len * 0.5, 0.0, std::max(0.0, dur - len));
        }
    }
    double newLen = len;
    if (ImGui::IsItemActive() && mode) {
        const double d = (mx - grabX) / W * dur;
        if (mode == 3) start = std::clamp(grabStart + d, 0.0, std::max(0.0, dur - len));
        if (mode == 2) newLen = SwapLength(std::clamp(grabLen + d, 3.0, std::min(StarMaxLen(), dur - grabStart)), dur - grabStart);
        if (mode == 1) {
            const double end = grabStart + grabLen;
            newLen = SwapLength(std::clamp(grabLen - d, 3.0, std::min(StarMaxLen(), end)), end);
            start = std::max(0.0, end - newLen);
        }
    }
    if (ImGui::IsItemDeactivated()) mode = 0, SaveSettings();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) && !ImGui::IsItemActive()) {
        const double t = std::clamp((mx - p.x) / W * dur, 0.0, dur);
        ImGui::SetTooltip("%.2f s\nDrag the ends to set the part, drag the part to move it", t);
    }
    g_star.start = start;
    o.seconds = newLen;
    // the numbers, frame steps and the player
    char lbl[96];
    snprintf(lbl, sizeof(lbl), "%.2f s to %.2f s:  %.2f s (%d frames)", start, start + newLen, newLen, static_cast<int>(std::lround(newLen * 24.0)));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(lbl);
    ImGui::SameLine();
    const double frame = 1.0 / 24.0;
    if (ImGui::SmallButton("<##fr")) g_star.start = std::max(0.0, start - frame);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Start one frame earlier");
    ImGui::SameLine(0.0f, ui::Dp(2.0f));
    if (ImGui::SmallButton(">##fr")) g_star.start = std::min(std::max(0.0, dur - newLen), start + frame);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Start one frame later");
    ImGui::SameLine();
    if (ImGui::SmallButton(g_partLoop ? "Stop##part" : "Play the part")) {
        if (g_partLoop && g_vid.player) g_vid.player->Pause(), g_partLoop = false;
        else {
            if (!(g_vid.player && g_vid.player->IsOpen() && _wcsicmp(g_vid.player->Path().c_str(), g_star.video.c_str()) == 0)) PlayClip(app, g_star.video);
            g_partSeek = start;
            g_partLoop = true;
            if (g_vid.player) g_vid.player->Play();
        }
    }
    const bool haveHead = g_vid.player && g_vid.player->IsOpen() && _wcsicmp(g_vid.player->Path().c_str(), g_star.video.c_str()) == 0;
    ImGui::SameLine();
    ImGui::BeginDisabled(!haveHead);
    if (ImGui::SmallButton("Start here")) g_star.start = std::clamp(g_vid.player->Position(), 0.0, std::max(0.0, dur - newLen)), g_partLoop = false;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("The part starts where the player is (play the clip, pause where you want it)");
    ImGui::SameLine();
    if (ImGui::SmallButton("End here") && haveHead) {
        const double end = std::clamp(g_vid.player->Position(), start + 3.0, std::min(dur, start + StarMaxLen()));
        o.seconds = SwapLength(end - start, dur - start);
        g_partLoop = false;
        SaveSettings();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("The part ends where the player is");
    ImGui::EndDisabled();
    // the part plays on a loop
    if (g_partLoop) {
        if (!haveHead) {
            g_partLoop = false;
        } else if (g_partSeek >= 0 && g_vid.player->Duration() > 0) {
            g_vid.player->Seek(g_partSeek);
            g_partSeek = -1.0;
        } else if (g_partSeek < 0) {
            const double pos = g_vid.player->Position();
            if (pos > g_star.start + o.seconds || pos < g_star.start - 0.25) g_vid.player->Seek(g_star.start);
        }
    }
    // cut it ahead (idle, in the background) once the part holds still: Make then starts at once
    if (!ImGui::IsItemActive() && mode == 0 && !g_star.video.empty()) {
        const int shortSide = std::min(kRes[std::clamp(o.res, 0, 3)].w, kRes[std::clamp(o.res, 0, 3)].h);
        char k[64];
        snprintf(k, sizeof(k), "|%.3f|%.3f|%d", g_star.start, o.seconds, shortSide);
        const std::string key = Utf8(g_star.video) + k;
        static double stillSince = 0;
        static std::string lastKey;
        if (key != lastKey) lastKey = key, stillSince = ImGui::GetTime();
        if (key != g_precutKey && ImGui::GetTime() - stillSince > 1.5 && (!g_precut.valid() || g_precut.wait_for(std::chrono::seconds(0)) == std::future_status::ready)) {
            g_precutKey = key;
            g_precut = std::async(std::launch::async, [src = g_star.video, st = g_star.start, sec = FramesFor(o.seconds) / 24.0, shortSide]() {
                std::string note;
                PrepareClip(src, st, sec, shortSide, note);
            });
        }
    }
}

// Swap's two files, one click: the Character Swap LoRA (0.6 GB) and, if missing, the comparison's 4-step turbo (1.9 GB), SHA-256 checked
void StartSwapDownloads() {
    if (!HasLora(kSwapLora)) StartDownload(g_swapDl, kSwapLoraUrl, kSwapLora, L"loras", 0.5, kSwapLoraSha);
    if (!HasLora(kSwapTurbo)) StartDownload(g_swapTurboDl, kSwapTurboUrl, kSwapTurbo, L"loras", 1.5, kSwapTurboSha);
    g_v.lorasAsked = -1000.0; // look again when they are there
    g_v.diskKnown = false;
}

void DrawSwapFiles() {
    const bool haveSwap = HasLora(kSwapLora), haveTurbo = HasLora(kSwapTurbo);
    static bool wasRunning = false;
    const bool running = g_swapDl.running || g_swapTurboDl.running;
    if (wasRunning && !running) g_v.lorasAsked = -1000.0, g_v.diskKnown = false; // finished: look again
    wasRunning = running;
    if (haveSwap && haveTurbo) return;
    if (running) {
        std::error_code ec;
        double gb = 0;
        for (const FileDownload* d : {&g_swapDl, &g_swapTurboDl}) {
            if (d->running && fs::exists(d->part, ec)) gb += fs::file_size(d->part, ec) / 1073741824.0;
        }
        ImGui::TextColored(ui::kAccentHover, "Downloading the Character Swap files: %.1f of %.1f GB (then checked)...", gb, (haveSwap ? 0.0 : 0.58) + (haveTurbo ? 0.0 : 1.82));
        return;
    }
    const bool failed = g_swapDl.failed || g_swapTurboDl.failed;
    char label[64];
    snprintf(label, sizeof(label), "%s (%.1f GB)", failed ? "Get them (failed: retry)" : haveSwap ? "Get the 4-step turbo" : "Get Character Swap",
             (haveSwap ? 0.0 : 0.58) + (haveTurbo ? 0.0 : 1.82));
    if (ImGui::SmallButton(label)) StartSwapDownloads();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("Into ComfyUI's loras folder, checked against their SHA-256:\n%s%s%s%s\n\nThe Character Swap LoRA is Alissonerdx's (BFS collection); "
                          "its license allows your own videos, also commercial,\nbut not running it as a hosted service.",
                          haveSwap ? "" : kSwapLora, haveSwap ? "" : " (0.6 GB, Alissonerdx/Minimax-H3-ComfyUI)\n", haveTurbo ? "" : kSwapTurbo,
                          haveTurbo ? "" : " (1.8 GB, lightx2v/Minimax-h3-Turbo: 4 steps instead of 8)");
    ImGui::SameLine();
    ImGui::TextDisabled(haveSwap ? "4 steps instead of 8: about twice as fast" : "Swapping the person needs the Character Swap LoRA.");
}

// Recreate's Fast model (FastH3-Omni, 20.7 GB), one click, SHA-256 checked
void DrawOmniDownload() {
    static bool wasRunning = false;
    if (wasRunning && !g_omniDl.running) g_ref.asked = -1000.0, g_v.diskKnown = false; // finished: look again
    wasRunning = g_omniDl.running;
    if (g_omniDl.running) {
        std::error_code ec;
        const double gb = fs::exists(g_omniDl.part, ec) ? fs::file_size(g_omniDl.part, ec) / 1073741824.0 : 0.0;
        ImGui::TextColored(ui::kAccentHover, "Downloading FastH3-Omni: %.1f of 20.7 GB (then checked)...", gb);
        return;
    }
    if (ImGui::SmallButton(g_omniDl.failed ? "Get FastH3-Omni (failed: retry)" : "Get FastH3-Omni (20.7 GB)"))
        StartDownload(g_omniDl, kOmniUrl, kOmniModel, L"diffusion_models", 20.0, kOmniSha), g_v.diskKnown = false;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("%s into ComfyUI's diffusion_models, checked against its SHA-256 (FastVideo/FastH3-Omni-Comfy, MiniMax-H3 community licence).\n"
                          "The reference model distilled to 8 steps: with Veda as fast as Fast is now, with the same likeness and closer to your prompt.\n"
                          "Needs ComfyUI 0.42 or newer.", kOmniModel);
    ImGui::SameLine();
    ImGui::TextDisabled("Fast then uses it: closer to your prompt, as quick");
}

// ---- Heavy jobs: tested 2026-10-17, a 10 s swap at 768p with two people filled the 24 GB card and the 64 GB of RAM (ComfyUI alone held 38 GB):
// the first steps took 72 s, the third 467 s once Windows started paging the model to disk - 20+ minutes for what takes about 7. Said before
// it is sent. (`reference`: Star in a video, whose model and pictures need more than animating a picture.)
std::string HeavyNote(int w, int h, double seconds, bool reference) {
    const double load = static_cast<double>(w) * h * FramesFor(std::min(seconds, 15.0));
    if (load <= (reference ? 1.9e8 : 2.6e8)) return {}; // (a 10 s part at 640p, 1.8e8, ran without paging: 690 s)
    return std::string("Heavy for this PC: this much video at once fills the graphics card and the RAM, and once Windows pages the model to disk "
                       "each step gets several times slower (tested: a 10 s 768p swap took 20+ minutes instead of about 7). ") +
           (reference ? "640p, or a part of 8 s or less, is much quicker." : "640p, or a shorter video, is much quicker.");
}

// ---- Star in a video: who is in the clip (for the cast cards), found by the writer from frames of the marked part, kept for next time
std::future<std::vector<std::string>> g_peopleFut;
std::string g_peopleKey, g_peopleNote;
std::vector<std::string> g_people;

void PumpClipPeople() {
    if (g_peopleFut.valid() && g_peopleFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        g_people = g_peopleFut.get();
        g_peopleNote = g_people.empty() ? "No one found (is the prompt writer set up, and can it see pictures?)" : std::string();
    }
    const std::string key = g_star.video.empty() ? std::string() : PeopleKey(g_star.video, g_star.start, g_v.opt.seconds);
    if (key != g_peopleKey && !g_peopleFut.valid()) {
        g_peopleKey = key;
        g_people.clear();
        g_peopleNote.clear();
        if (!key.empty()) { // found before for this part: shown at once
            std::lock_guard<std::mutex> lock(g_swapCacheMutex);
            const json& c = SwapCacheLocked();
            if (c.contains("people") && c["people"].contains(key) && c["people"][key].is_array()) g_people = c["people"][key].get<std::vector<std::string>>();
        }
    }
}

// One person of the cast: their picture, their name, whom they replace in the clip
bool MaskReady(); // (below: person masking is in the running ComfyUI)
// Who each new person replaces, picked on the page (see ScanPeople)
const ImU32 kCastCol[4] = {IM_COL32(70, 160, 255, 255), IM_COL32(255, 150, 40, 255), IM_COL32(70, 210, 110, 255), IM_COL32(235, 90, 235, 255)};
struct WhoPick {
    std::shared_ptr<PeopleScan> scan;            // the clip part's people (ok = false: why it failed)
    std::future<std::shared_ptr<PeopleScan>> fut;
    std::string wantKey;                         // the part being chosen, and since when (found once it stays put)
    double wantAt = 0;
    std::vector<std::vector<int>> pick;          // [shot][card]: index into scan->kept[shot], -1 = nobody there
    std::vector<std::vector<char>> manual;       // [shot][card]: clicked there (not followed by colour)
    std::vector<std::pair<int, int>> anchor;     // [card]: (shot, index) whose colours are followed; shot -1 = none yet
    int active = 0;                              // the card a click picks for
    std::vector<std::string> desc;               // [card]: whom they replace in words ("the man (bald, brown fur coat) (man)")
    std::vector<std::pair<int, int>> descOf;     // [card]: the anchor that description is of
    std::future<std::string> descFut;
    int descCard = -1;
    std::pair<int, int> descFor{-1, -1};
    std::map<std::wstring, LoadedImage> tex;     // the shots' outlined frames
    // Only on a click: Find the people, or Make (ComfyUI and SAM 3 take the GPU and memory - they had started by themselves as soon as a
    // clip was chosen, 2026-10-22). askedKey: the part they were asked for; makeWhenReady: Make was pressed before they were found.
    std::string askedKey;
    bool makeWhenReady = false;
} g_pick;

std::string PickKey() {
    if (g_star.video.empty()) return {};
    char part[64];
    snprintf(part, sizeof(part), "|%.3f|%.3f", g_star.start, g_v.opt.seconds);
    return FileKey(g_star.video) + part;
}

bool PickCurrent() { return g_pick.scan && g_pick.scan->ok && g_pick.scan->key == PickKey(); }

// A card's pick followed into the shots not clicked: the person there whose colours are most like the anchor's (and clearly so)
void FollowPick(size_t k) {
    const PeopleScan& sc = *g_pick.scan;
    const auto [as, ai] = g_pick.anchor[k];
    if (as < 0) return;
    const float* a = sc.kept[as][ai].hist;
    for (size_t s = 0; s < sc.kept.size(); ++s) {
        if (g_pick.manual[s][k]) continue;
        if (static_cast<int>(s) == as) {
            g_pick.pick[s][k] = ai;
            continue;
        }
        int best = -1;
        float bs = 0, second = 0;
        for (size_t i = 0; i < sc.kept[s].size(); ++i) {
            bool taken = false;
            for (size_t q = 0; q < g_pick.pick[s].size(); ++q) taken |= q != k && g_pick.pick[s][q] == static_cast<int>(i);
            if (taken) continue;
            const float sim = HistSim(a, sc.kept[s][i].hist);
            if (sim > bs) second = bs, bs = sim, best = static_cast<int>(i);
            else second = std::max(second, sim);
        }
        g_pick.pick[s][k] = best >= 0 && bs >= 0.5f && bs - second >= 0.06f ? best : -1;
    }
}

// The picks sized to the scan and the cards; a card without an anchor gets the most prominent person nobody else has (and not one like them)
void AutoPicks() {
    if (!g_pick.scan || !g_pick.scan->ok) return;
    const PeopleScan& sc = *g_pick.scan;
    const size_t cards = g_star.who.size(), shots = sc.kept.size();
    if (g_pick.pick.size() != shots) g_pick.pick.assign(shots, {}), g_pick.manual.assign(shots, {});
    for (size_t s = 0; s < shots; ++s) g_pick.pick[s].resize(cards, -1), g_pick.manual[s].resize(cards, 0);
    g_pick.anchor.resize(cards, {-1, -1});
    g_pick.desc.resize(cards), g_pick.descOf.resize(cards, {-1, -1});
    // where the first pick is looked for: the first shot of a second or more with people in it - the main performer as the clip opens
    // (over a whole 18 s clip the most prominent person overall was the other man, who comes to the front later)
    size_t firstShot = shots;
    for (size_t s = 0; s < shots && firstShot == shots; ++s)
        if (!sc.kept[s].empty() && sc.bounds[s + 1] - sc.bounds[s] >= 24) firstShot = s;
    for (size_t k = 0; k < cards; ++k) {
        if (g_pick.anchor[k].first >= 0) continue;
        double best = 0;
        std::pair<int, int> at{-1, -1};
        for (size_t s = 0; s < shots; ++s)
            for (size_t i = 0; i < sc.kept[s].size(); ++i) {
                if (k == 0 && firstShot < shots && s != firstShot) continue;
                bool clash = false;
                for (size_t q = 0; q < cards; ++q) {
                    if (q == k || g_pick.anchor[q].first < 0) continue;
                    clash |= g_pick.pick[s][q] == static_cast<int>(i) || HistSim(sc.kept[s][i].hist, sc.kept[g_pick.anchor[q].first][g_pick.anchor[q].second].hist) > 0.6f;
                }
                const double score = sc.kept[s][i].maxArea * sc.kept[s][i].frames;
                if (!clash && score > best) best = score, at = {static_cast<int>(s), static_cast<int>(i)};
            }
        g_pick.anchor[k] = at;
        FollowPick(k);
    }
}

// A click on person i of shot s for card k: picked there (taken from any other card), followed into the shots not clicked; a click on the
// one already picked takes them off (nobody there)
void ClickPick(int s, int i, size_t k) {
    if (g_pick.pick[s][k] == i) {
        g_pick.pick[s][k] = -1;
        g_pick.manual[s][k] = 1;
        return;
    }
    for (size_t q = 0; q < g_pick.pick[s].size(); ++q)
        if (q != k && g_pick.pick[s][q] == i) g_pick.pick[s][q] = -1, g_pick.manual[s][q] = 1;
    g_pick.pick[s][k] = i;
    g_pick.manual[s][k] = 1;
    g_pick.anchor[k] = {s, i};
    FollowPick(k);
}

// Whom they replace in words, from their clearest view (the writer; one person, one question)
std::string DescribePerson(std::wstring crop, std::string comfyUrl) {
    StepLog("describing whom they replace");
    struct Done { ~Done() { StepLog("described"); } } done;
    const ClefSession session = WriterAcquire(comfyUrl, [](const std::string&) {});
    std::string out, err;
    if (session.ok && WriterCanSee())
        WriterChat("One person in the picture is outlined in red. Say who they are in under 14 words, starting with man, woman, boy, girl or person, "
                   "then their hair, then their clothes. Example: man, bald, round sunglasses, brown fur coat over a red shirt",
                   "Who is outlined?", {ImageDataUrl(crop, 512)}, 60, 0.1, out, &err);
    WriterRelease(session, false);
    return Trimmed(out);
}

// What the prompt says they replace: "the man (bald, round sunglasses, brown fur coat) (man)", else where they are
std::string PickedReplaces(size_t k) {
    if (k < g_pick.desc.size() && !g_pick.desc[k].empty()) {
        std::string d = g_pick.desc[k];
        while (!d.empty() && strchr(".\r\n ", d.back())) d.pop_back();
        const size_t comma = d.find(',');
        std::string first = Trimmed(d.substr(0, comma)), rest = comma == std::string::npos ? std::string() : Trimmed(d.substr(comma + 1));
        std::string low = first;
        for (char& c : low) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (low.size() < 12 && !low.empty()) {
            const char* sex = low == "man" || low == "boy" ? " (man)" : low == "woman" || low == "girl" ? " (woman)" : "";
            return "the " + low + (rest.empty() ? std::string() : " (" + rest + ")") + sex;
        }
    }
    if (!g_pick.scan || k >= g_pick.anchor.size() || g_pick.anchor[k].first < 0) return {};
    const ScanPerson& p = g_pick.scan->kept[g_pick.anchor[k].first][g_pick.anchor[k].second];
    const double cx = (p.x0 + p.x1) * 0.5 / g_pick.scan->aw;
    return cx < 0.36 ? "the person on the left" : cx > 0.64 ? "the person on the right" : "the person in the centre";
}

// Every frame on the page: the scan for the part being chosen (once it stays put a moment), the picks kept in step, the descriptions
void PumpPick(AppState& app) {
    if (g_pick.fut.valid() && g_pick.fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        for (auto& [path, img] : g_pick.tex)
            if (img.srv) ReleaseAfterFrame(img.srv);
        g_pick.tex.clear();
        g_pick.scan = g_pick.fut.get();
        g_pick.pick.clear(), g_pick.manual.clear(), g_pick.anchor.clear(), g_pick.desc.clear(), g_pick.descOf.clear();
        AutoPicks();
    }
    const bool asked = !g_star.video.empty() && g_pick.askedKey == PickKey();
    if (g_starMethod == 0 && asked && !g_v.vedaKnown && MaskOk()) { // asked for, ComfyUI not answering yet: started (once a minute)
        static double startedAt = -1000.0;
        if (ImGui::GetTime() - startedAt > 60.0) {
            startedAt = ImGui::GetTime();
            std::string message;
            ComfyStart(app.comfy, message);
            g_v.vedaAsked = -1000.0;
        }
    }
    if (g_starMethod != 0 || !MaskReady() || g_star.video.empty()) return;
    const std::string key = PickKey();
    if (key != g_pick.wantKey) g_pick.wantKey = key, g_pick.wantAt = ImGui::GetTime();
    if (asked && !g_pick.fut.valid() && (!g_pick.scan || g_pick.scan->key != key) && ImGui::GetTime() - g_pick.wantAt > 1.2 && (StepLog("finding the people"), true))
        g_pick.fut = std::async(std::launch::async, ScanPeopleLong, g_star.video, g_star.start, g_v.opt.seconds, app.comfy.url, g_v.vedaInfo.sam3);
    if (!PickCurrent()) return;
    if (g_pick.anchor.size() != g_star.who.size()) {
        if (g_pick.anchor.size() > g_star.who.size()) { // a card was taken out: start the picks over
            g_pick.pick.clear(), g_pick.manual.clear(), g_pick.anchor.clear(), g_pick.desc.clear(), g_pick.descOf.clear();
        }
        AutoPicks();
        g_pick.active = std::clamp(g_pick.active, 0, std::max(0, static_cast<int>(g_star.who.size()) - 1));
    }
    if (g_pick.descFut.valid() && g_pick.descFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const std::string d = g_pick.descFut.get();
        if (g_pick.descCard >= 0 && g_pick.descCard < static_cast<int>(g_pick.desc.size())) g_pick.desc[g_pick.descCard] = d, g_pick.descOf[g_pick.descCard] = g_pick.descFor;
    }
    if (!g_pick.descFut.valid() && WriterConfigured() && !g_writeFut.valid()) {
        for (size_t k = 0; k < g_pick.anchor.size(); ++k) {
            const auto a = g_pick.anchor[k];
            if (a.first < 0 || g_pick.descOf[k] == a) continue;
            const std::wstring crop = g_pick.scan->kept[a.first][a.second].crop;
            if (crop.empty()) {
                g_pick.descOf[k] = a;
                continue;
            }
            g_pick.desc[k].clear();
            g_pick.descCard = static_cast<int>(k), g_pick.descFor = a;
            g_pick.descFut = std::async(std::launch::async, DescribePerson, crop, app.comfy.url);
            break;
        }
    }
}

// Who a pick stands for in its shot: the person clicked (or followed), and the pieces of their track SAM 3 lost and found again as a new
// object - never on screen at the same time as them, and dressed alike. Seen on a test clip: the man in front was object 0 for 134
// frames and object 6 for the other 40 of the shot; the pick alone would have left him unmasked for 1.7 s.
std::vector<int> PickedSet(size_t s, size_t k) {
    std::vector<int> out;
    if (!g_pick.scan || s >= g_pick.pick.size() || k >= g_pick.pick[s].size() || g_pick.pick[s][k] < 0) return out;
    const auto& kept = g_pick.scan->kept[s];
    const int i = g_pick.pick[s][k];
    out.push_back(i);
    for (size_t j = 0; j < kept.size(); ++j) {
        if (static_cast<int>(j) == i) continue;
        bool other = false;
        for (size_t q = 0; q < g_pick.pick[s].size(); ++q) other |= q != k && g_pick.pick[s][q] == static_cast<int>(j);
        if (other || HistSim(kept[i].hist, kept[j].hist) < 0.5f) continue;
        // (a handover: SAM 3 had both for 5 frames there, 0.85 alike, while the man in a fur coat went from one object to the other)
        int both = 0;
        for (size_t t = 0; t < kept[i].seen.size() && t < kept[j].seen.size(); ++t) both += kept[i].seen[t] == '1' && kept[j].seen[t] == '1';
        if (!kept[i].seen.empty() && both <= std::max(6, std::min(kept[i].frames, kept[j].frames) * 15 / 100)) out.push_back(static_cast<int>(j));
    }
    return out;
}

// The picks as BuildPeopleMasks takes them: [shot][card] -> the SAM 3 objects
std::vector<std::vector<std::vector<int>>> PicksAsObjects() {
    std::vector<std::vector<std::vector<int>>> out;
    if (!PickCurrent()) return out;
    for (size_t s = 0; s < g_pick.pick.size(); ++s) {
        out.emplace_back();
        for (size_t k = 0; k < g_pick.pick[s].size(); ++k) {
            std::vector<int> objs;
            for (int i : PickedSet(s, k)) objs.push_back(g_pick.scan->kept[s][i].o);
            out.back().push_back(objs);
        }
    }
    return out;
}

void DrawCastCard(AppState& app, size_t k, float w, int& remove) {
    const std::wstring pic = g_star.who[k];
    const std::wstring key = StarKeyOf(pic);
    ImGui::PushID(static_cast<int>(k));
    ImGui::BeginGroup();
    const float ph = w * 1.3f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##face", ImVec2(w, ph));
    const bool hov = ImGui::IsItemHovered();
    if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) ViewPictureLarge(app, pic);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + ph), ui::ThumbPlaceholder(), ui::Dp(10.0f));
    const AppState::LibThumb lt = app.LibraryThumb(pic);
    if (lt.srv && lt.w > 0 && lt.h > 0) {
        const float s = std::min(w / lt.w, ph / lt.h);
        const ImVec2 a(p.x + (w - lt.w * s) * 0.5f, p.y + (ph - lt.h * s) * 0.5f);
        dl->AddImageRounded((ImTextureID)(intptr_t)lt.srv, a, ImVec2(a.x + lt.w * s, a.y + lt.h * s), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, ui::Dp(9.0f));
    }
    // its number: what the prompt calls them (<Picture k>, <Subject k>)
    const ImVec2 b(p.x + ui::Dp(14.0f), p.y + ui::Dp(14.0f));
    dl->AddCircleFilled(b, ui::Dp(10.0f), ImGui::GetColorU32(ui::kAccent), 20);
    char num[4];
    snprintf(num, sizeof(num), "%d", static_cast<int>(k) + 1);
    const ImVec2 ns = ImGui::CalcTextSize(num);
    dl->AddText(ImVec2(b.x - ns.x * 0.5f, b.y - ns.y * 0.5f), IM_COL32_WHITE, num);
    // x to take them out (over the picture's corner, while the pointer is on it)
    const ImVec2 xc(p.x + w - ui::Dp(14.0f), p.y + ui::Dp(14.0f));
    const bool onX = hov && ImGui::IsMouseHoveringRect(ImVec2(xc.x - ui::Dp(11.0f), xc.y - ui::Dp(11.0f)), ImVec2(xc.x + ui::Dp(11.0f), xc.y + ui::Dp(11.0f)));
    if (hov) {
        dl->AddCircleFilled(xc, ui::Dp(10.0f), onX ? IM_COL32(220, 70, 70, 240) : IM_COL32(10, 12, 16, 200), 20);
        const float r = ui::Dp(4.0f);
        dl->AddLine(ImVec2(xc.x - r, xc.y - r), ImVec2(xc.x + r, xc.y + r), IM_COL32_WHITE, ui::Dp(1.6f));
        dl->AddLine(ImVec2(xc.x - r, xc.y + r), ImVec2(xc.x + r, xc.y - r), IM_COL32_WHITE, ui::Dp(1.6f));
        ImGui::SetTooltip("%s\n<Picture %d> in the prompt%s", Utf8(fs::path(pic).filename().wstring()).c_str(), static_cast<int>(k) + 1, onX ? "\nClick: take them out" : "");
    }
    if (onX && ImGui::IsItemClicked()) remove = static_cast<int>(k);
    else if (ImGui::IsItemClicked() && g_starMethod == 0) g_pick.active = static_cast<int>(k);
    if (g_starMethod == 0) // (its colour: its boxes in Who they replace)
        dl->AddRect(p, ImVec2(p.x + w, p.y + ph), kCastCol[k % 4], ui::Dp(10.0f), 0, ui::Dp(g_pick.active == static_cast<int>(k) && g_star.who.size() > 1 ? 3.5f : 2.0f));
    // the name
    std::string name = StarName(pic);
    char nb[64];
    snprintf(nb, sizeof(nb), "%s", name.c_str());
    ImGui::SetNextItemWidth(w);
    if (ImGui::InputTextWithHint("##name", "name", nb, sizeof(nb))) g_star.names[key] = nb;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Their name: say who does what with it in your idea (\"Ava takes the girl's place\")");
    // whom they replace: a swap's are clicked under Who they replace; Recreate says it in words
    if (g_starMethod == 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kCastCol[k % 4]));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
        std::string d = k < g_pick.desc.size() ? g_pick.desc[k] : std::string();
        if (d.size() > 44) d = d.substr(0, d.find_last_of(" ,", 41)) + "...";
        ImGui::TextWrapped("%s", PickCurrent() ? (d.empty() ? "replaces the boxed person" : ("replaces: " + d).c_str()) : "replaces: (click them below)");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::EndGroup();
        ImGui::PopID();
        return;
    }
    const std::string cur = StarReplaces(pic);
    ImGui::TextDisabled("in place of");
    ImGui::SetNextItemWidth(w);
    const std::string preview = cur.empty() ? std::string("auto") : StripSex(cur);
    if (ImGui::BeginCombo("##rep", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::Selectable("Auto (from my idea and the clip)", cur.empty())) g_star.replaces.erase(key);
        if (!g_people.empty()) ImGui::SeparatorText("In the clip");
        for (const std::string& who : g_people) {
            if (ImGui::Selectable(who.c_str(), cur == who)) g_star.replaces[key] = who;
        }
        if (g_people.empty()) ImGui::TextDisabled(g_peopleFut.valid() ? "Looking for the people in the clip..." : "Find the people in the clip (below the cast) to pick here");
        ImGui::SeparatorText("Or say it");
        static char typed[128] = {};
        if (ImGui::IsWindowAppearing()) snprintf(typed, sizeof(typed), "%s", StripSex(cur).c_str());
        ImGui::SetNextItemWidth(ui::Dp(260.0f));
        if (ImGui::InputTextWithHint("##typed", "e.g. the man on the left", typed, sizeof(typed), ImGuiInputTextFlags_EnterReturnsTrue) && typed[0])
            g_star.replaces[key] = typed, ImGui::CloseCurrentPopup();
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("Who in the clip this person takes the place of. Auto: the writer decides from your idea and the clip\n(with one person: the main performer).");
    ImGui::EndGroup();
    ImGui::PopID();
}

// "+ Add someone": your characters (with their names), the gallery, a file
void DrawAddCastCard(AppState& app, float w) {
    const float ph = w * 1.3f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##addcast", ImVec2(w, ph + 2.0f * ImGui::GetFrameHeightWithSpacing()))) ImGui::OpenPopup("##addcastmenu");
    const bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + ph), hov ? IM_COL32(255, 255, 255, 16) : IM_COL32(255, 255, 255, 6), ui::Dp(10.0f));
    dl->AddRect(p, ImVec2(p.x + w, p.y + ph), hov ? ImGui::GetColorU32(ui::kAccent) : IM_COL32(255, 255, 255, 60), ui::Dp(10.0f), 0, ui::Dp(1.5f));
    const ImVec2 c(p.x + w * 0.5f, p.y + ph * 0.45f);
    dl->AddLine(ImVec2(c.x - ui::Dp(12.0f), c.y), ImVec2(c.x + ui::Dp(12.0f), c.y), IM_COL32(255, 255, 255, 200), ui::Dp(2.0f));
    dl->AddLine(ImVec2(c.x, c.y - ui::Dp(12.0f)), ImVec2(c.x, c.y + ui::Dp(12.0f)), IM_COL32(255, 255, 255, 200), ui::Dp(2.0f));
    const char* t = g_star.who.empty() ? "Add who goes in" : "Add someone";
    const ImVec2 ts = ImGui::CalcTextSize(t);
    dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y + ui::Dp(22.0f)), ImGui::GetColorU32(ImGuiCol_Text, hov ? 1.0f : 0.7f), t);
    if (ImGui::BeginPopup("##addcastmenu")) {
        ImGui::TextDisabled("Your characters");
        const float tw = ui::Dp(56.0f), th = tw * 1.35f;
        int shown = 0;
        for (const CharacterProfile& cp : AllProfiles()) {
            if (!cp.saved || cp.image.empty()) continue;
            if (shown % 6) ImGui::SameLine(0.0f, ui::Dp(6.0f));
            ImGui::PushID(cp.key.c_str());
            ImGui::BeginGroup();
            const AppState::LibThumb lt = app.LibraryThumb(cp.image);
            const bool in = std::any_of(g_star.who.begin(), g_star.who.end(), [&](const std::wstring& w2) { return _wcsicmp(w2.c_str(), cp.image.c_str()) == 0; });
            ImGui::BeginDisabled(in);
            if (lt.srv && lt.w > 0 ? ImGui::ImageButton("##c", (ImTextureID)(intptr_t)lt.srv, ImVec2(tw, th)) : ImGui::Button("?", ImVec2(tw, th))) {
                StarAdd(cp.image);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            const std::string nm = cp.name.size() > 9 ? cp.name.substr(0, 8) + "." : cp.name;
            ImGui::TextDisabled("%s", nm.c_str());
            ImGui::EndGroup();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s%s", cp.name.c_str(), in ? " (already in)" : "");
            ImGui::PopID();
            ++shown;
        }
        if (shown == 0) ImGui::TextDisabled("None yet");
        ImGui::Separator();
        if (ImGui::MenuItem("Pick a picture in the gallery...")) {
            g_starPick = true;
            app.requestTab = 0;
        }
        if (ImGui::MenuItem("A picture file...")) {
            const std::wstring f = PickPicture(L"Who goes in the video");
            if (!f.empty()) StarAdd(f);
        }
        const bool haveSel = app.selectedIndex >= 0 && app.selectedIndex < static_cast<int>(app.entries.size()) && !app.entries[app.selectedIndex].metadata.isVideo;
        if (ImGui::MenuItem("The picture selected in the gallery", nullptr, false, haveSel)) StarAdd(app.entries[app.selectedIndex].path);
        if (ImGui::MenuItem("Create a character...")) OpenCharacterCreator(app);
        ImGui::EndPopup();
    }
}

// The chosen clip as one card (the browser opens with Change)
void DrawClipCard(AppState& app) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui::Dp(10.0f), ui::Dp(10.0f)));
    ImGui::BeginChild("##clipcard", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    const float th = ui::Dp(84.0f);
    const float tw = g_star.info.height > 0 ? std::clamp(th * g_star.info.width / g_star.info.height, th * 0.5f, th * 1.8f) : th * 0.56f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##clipthumb", ImVec2(tw, th));
    if (ImGui::IsItemClicked()) PlayClip(app, g_star.video);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Play it");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + tw, p.y + th), IM_COL32(0, 0, 0, 255), ui::Dp(6.0f));
    const AppState::LibThumb lt = app.LibraryThumb(g_star.video);
    if (lt.srv && lt.w > 0) {
        const float s = std::min(tw / lt.w, th / lt.h);
        const ImVec2 a(p.x + (tw - lt.w * s) * 0.5f, p.y + (th - lt.h * s) * 0.5f);
        dl->AddImageRounded((ImTextureID)(intptr_t)lt.srv, a, ImVec2(a.x + lt.w * s, a.y + lt.h * s), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, ui::Dp(5.0f));
    }
    ImGui::SameLine(0.0f, ui::Dp(12.0f));
    ImGui::BeginGroup();
    const std::string name = Utf8(fs::path(g_star.video).filename().wstring());
    ImGui::PushFont(ui::BoldFont());
    ImGui::TextUnformatted(name.size() > 46 ? (name.substr(0, 43) + "...").c_str() : name.c_str());
    ImGui::PopFont();
    ImGui::TextDisabled("%s  -  %d x %d%s%s", FormatDuration(g_star.info.seconds).c_str(), g_star.info.width, g_star.info.height,
                        g_swapFolder.empty() ? "" : "  -  from ", g_swapFolder.empty() ? "" : Utf8(fs::path(g_swapFolder).filename().wstring()).c_str());
    if (ImGui::SmallButton(g_starBrowse ? "Close the clips" : "Change the clip")) g_starBrowse = !g_starBrowse;
    ImGui::SameLine();
    if (ImGui::SmallButton("Play")) PlayClip(app, g_star.video);
    ImGui::SameLine(0.0f, ui::Dp(14.0f));
    ImGui::Checkbox("Keep its sound", &g_star.keepSound);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Its music, voices and sounds go under the new video as they are (else new sound is made)");
    ImGui::EndGroup();
    ImGui::EndChild();
}

// ---- Everything the best swap uses, in one place: what is ready, what is not, one button for all of it, and the restart ComfyUI needs.
// (There had been six separate set-up buttons on the page with no word on which were needed; a swap then ran without person masking.)
// The installs that run a script go one after another (two pip installs into ComfyUI's Python at once could clash); downloads go together.
std::deque<int> g_setupQueue; // scripts still to run: 0 Veda, 1 person masking, 2 facial expressions

bool MaskReady() { return g_v.vedaKnown && g_v.vedaInfo.maskvid && !g_v.vedaInfo.sam3.empty(); }

// What is installed, read from ComfyUI Desktop's folders, for when ComfyUI is not running (the panel said only "Start ComfyUI to see what is
// set up", with no button - there was no way to the set-up). Looked at again every 5 s.
struct OnDisk {
    double at = -1000.0;
    bool veda = false, mask = false, sam3 = false, faces = false;
};
OnDisk g_onDisk;

const OnDisk& StarOnDisk() {
    if (ImGui::GetTime() - g_onDisk.at < 5.0) return g_onDisk;
    g_onDisk.at = ImGui::GetTime();
    const std::wstring install = ComfyInstallDir(), models = ComfyModelsDir();
    std::error_code ec;
    const fs::path nodes = fs::path(install) / L"ComfyUI" / L"custom_nodes", own = fs::path(install) / L"ComfyUI" / L"models";
    auto model = [&](const wchar_t* sub, const std::wstring& file) {
        return (!models.empty() && fs::exists(fs::path(models) / sub / file, ec)) || (!install.empty() && fs::exists(own / sub / file, ec));
    };
    g_onDisk.veda = !install.empty() && fs::exists(nodes / L"Veda-on-ComfyUI", ec) && model(L"veda", fs::path(kVedaStarPredictor).wstring());
    g_onDisk.mask = !install.empty() && fs::exists(nodes / L"MaskVidExperiments" / L"nodes_mask_to_latent.py", ec);
    g_onDisk.sam3 = false;
    for (const fs::path& dir : {fs::path(models) / L"checkpoints", own / L"checkpoints"}) {
        if (dir.empty() || !fs::exists(dir, ec)) continue;
        for (const auto& e : fs::directory_iterator(dir, ec))
            if (e.path().filename().wstring().find(L"sam3") != std::wstring::npos) g_onDisk.sam3 = true;
    }
    g_onDisk.faces = !install.empty() && fs::exists(nodes / L"ComfyUI-LivePortraitKJ", ec) && model(L"liveportrait", L"warping_module.safetensors");
    return g_onDisk;
}

// Ready: what ComfyUI says when it is running, else what is on disk
bool VedaOk() { return g_v.vedaKnown ? VedaReady(kVedaStarPredictor) : StarOnDisk().veda; }
bool MaskOk() { return g_v.vedaKnown ? MaskReady() : StarOnDisk().mask && StarOnDisk().sam3; }
bool FacesOk() { return g_v.vedaKnown ? FacesReady() : StarOnDisk().faces; }

// The best settings for a swap, from tests on a rap clip (5 s, an elf character, RTX 3090):
// - 1080p straight from H3 gave the face far more detail than 768p (a full-body performer's face is only ~3 x 4 of H3's 32 px tokens at
//   768p) and took 496 s, against 195 s + 389 s for 768p and SeedVR2 to 1080p; past about 6 s, 1080p is too big for 24 GB (the 38 GB RAM case),
//   so it is 768p and Make it crisp after
// - the faces pass made the mouth rap along; person masking keeps the rest of the clip; one person at a time never mixes people up
struct BestPlan {
    int res;
    bool crisp;
};
BestPlan BestFor(double seconds) {
    const long long vram = GpuTotalMiB();
    if (vram >= 20000 && seconds <= 6.5) return {3, false};
    return {2, CrispReady()};
}

void DrawBestSettings(const char* id) {
    H3Options& o = g_v.opt;
    const BestPlan best = BestFor(o.seconds);
    const bool vedaOk = VedaOk(), maskOk = MaskOk(), facesOk = FacesOk();
    const bool already = o.res == best.res && (!vedaOk || g_v.veda) && (!maskOk || (g_v.swapMask && g_v.oneAtATime)) && (!facesOk || g_v.facesAfterSwap) &&
                         g_v.crispAfterSwap == best.crisp;
    ImGui::PushID(id);
    std::string what = std::string(kRes[best.res].name) + (best.crisp ? " + Make it crisp" : "");
    if (maskOk) what += ", person masking (one at a time)";
    if (facesOk) what += ", facial expressions";
    if (vedaOk) what += ", Veda";
    if (already) {
        ImGui::TextDisabled("Using the best settings: %s", what.c_str());
    } else {
        if (ImGui::SmallButton("Use the best settings")) {
            o.res = best.res;
            g_v.crispAfterSwap = best.crisp;
            if (vedaOk) g_v.veda = true;
            if (maskOk) g_v.swapMask = true, g_v.oneAtATime = true;
            if (facesOk) g_v.facesAfterSwap = true;
            SaveSettings();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", what.c_str());
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Tested on a 5 s rap clip: 1080p gave the face far more detail than 768p and took less time than 768p + SeedVR2 then did; the "
                          "facial-expressions pass made the mouth rap along (it hardly moved without it). Past about 6 s, 1080p is too big for a 24 GB "
                          "card, so it is 768p and Make it crisp after.");
    ImGui::PopID();
}

void DrawStarSetup(AppState& app) {
    const bool scriptBusy = g_vedaSetup.running || g_maskSetup.running || g_lpSetup.running;
    if (!scriptBusy && !g_setupQueue.empty()) {
        const int next = g_setupQueue.front();
        g_setupQueue.pop_front();
        if (next == 0 && !VedaOk()) StartVedaSetup();
        if (next == 1 && !MaskOk()) StartMaskSetup();
        if (next == 2 && !FacesOk()) StartLpSetup();
    }
    struct Item {
        const char* name;
        const char* why;
        bool ready, busy, restart; // restart: set up, ComfyUI has not loaded it yet
        double gb;
    };
    const bool swapFiles = HasLora(kSwapLora) && HasLora(kSwapTurbo);
    const Item items[] = {
        {"The H3 reference model", "the video model a swap runs on", g_ref.have, g_ref.downloading, false, 19.5},
        {"Character Swap LoRA + 4-step turbo", "swaps the person; 4 steps instead of 8", swapFiles, g_swapDl.running || g_swapTurboDl.running, false,
         (HasLora(kSwapLora) ? 0.0 : 0.58) + (HasLora(kSwapTurbo) ? 0.0 : 1.82)},
        {"Veda", "2-3x faster", VedaOk(), g_vedaSetup.running || (!g_setupQueue.empty() && std::find(g_setupQueue.begin(), g_setupQueue.end(), 0) != g_setupQueue.end()),
         g_v.vedaKnown && g_vedaSetup.finished && VedaSetupLine() == "ok" && !VedaReady(kVedaStarPredictor), 0.6},
        {"Person masking", "only the person is made again; the rest stays the clip's", MaskOk(),
         g_maskSetup.running || std::find(g_setupQueue.begin(), g_setupQueue.end(), 1) != g_setupQueue.end(),
         g_v.vedaKnown && g_maskSetup.finished && MaskSetupLine() == "ok" && !MaskReady(), 0.0},
        {"Facial expressions (LivePortrait)", "the mouth and eyes move like the original's", FacesOk(),
         g_lpSetup.running || std::find(g_setupQueue.begin(), g_setupQueue.end(), 2) != g_setupQueue.end(),
         g_v.vedaKnown && g_lpSetup.finished && LpSetupLine() == "ok" && !FacesReady(), 0.7},
        {"Make it crisp (Open-H3)", "redraws fine detail at 1080p or 2K afterwards", CrispReady(), CrispDownloading(), false, CrispMissingGb()},
    };
    int missing = 0, restart = 0;
    double gb = 0;
    for (const Item& it : items) {
        if (!it.ready && !it.busy && !it.restart) ++missing, gb += it.gb;
        if (it.restart) ++restart;
    }
    bool anyBusy = false;
    for (const Item& it : items) anyBusy |= it.busy;
    if (!g_v.vedaKnown) { // (what is installed is read from its folders meanwhile)
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("ComfyUI is not running (it starts by itself when you make a video).");
        ImGui::SameLine();
        if (ImGui::SmallButton("Start ComfyUI##setup")) {
            std::string message;
            ComfyStart(app.comfy, message);
            app.SetStatus(message);
            g_v.vedaAsked = -1000.0;
        }
    }
    const bool allReady = missing == 0 && restart == 0 && !anyBusy;
    ImGui::PushID("starsetup");
    if (allReady) {
        ImGui::TextColored(ui::kAccent, "Everything for the best swap is set up");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("The H3 reference model, the Character Swap LoRA and its 4-step turbo, Veda, person masking, facial expressions "
                              "(LivePortrait) and Make it crisp (Open-H3)");
        DrawBestSettings("bestready");
        ImGui::PopID();
        return;
    }
    ImGui::BeginChild("##setuplist", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::TextUnformatted("What the best swap uses");
    for (const Item& it : items) {
        const ImVec4 col = it.ready ? ui::kAccent : it.busy ? ui::kAccentHover : it.restart ? ui::kWarn : ui::kTextDim;
        ImGui::TextColored(col, "%s", it.ready ? "  ready  " : it.busy ? "  getting it  " : it.restart ? "  restart  " : "  missing  ");
        ImGui::SameLine(ui::Dp(110.0f));
        ImGui::TextUnformatted(it.name);
        ImGui::SameLine();
        if (!it.ready && !it.busy && !it.restart && it.gb > 0.05) ImGui::TextDisabled("- %s (%.1f GB)", it.why, it.gb);
        else ImGui::TextDisabled("- %s", it.why);
    }
    if ((g_v.vedaKnown && g_v.vedaInfo.maskvid && g_v.vedaInfo.sam3.empty()) || (!g_v.vedaKnown && StarOnDisk().mask && !StarOnDisk().sam3))
        ImGui::TextColored(ui::kWarn, "Person masking also needs a SAM 3 checkpoint in ComfyUI's checkpoints (sam3.1_multiplex_fp16).");
    if (missing > 0) {
        char label[96];
        snprintf(label, sizeof(label), "Set up everything missing (%d, about %.1f GB)", missing, gb);
        if (ui::PrimaryButton(label, ImVec2(0.0f, ui::Dp(30.0f)))) {
            if (!g_ref.have && !g_ref.downloading && g_ref.known) StartRefDownload();
            if (!swapFiles) StartSwapDownloads();
            if (!CrispReady() && !CrispDownloading()) StartCrispSetup(app);
            for (int s : {0, 1, 2}) {
                const bool done = s == 0 ? VedaOk() : s == 1 ? MaskOk() : FacesOk();
                if (!done && std::find(g_setupQueue.begin(), g_setupQueue.end(), s) == g_setupQueue.end()) g_setupQueue.push_back(s);
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("Downloads and installs everything still missing into ComfyUI Desktop, one install after another; the files are checked "
                              "against their SHA-256. ComfyUI is restarted once at the end so it loads the new nodes.");
    }
    if (restart > 0 && !anyBusy) {
        ImGui::TextColored(ui::kWarn, "Restart ComfyUI to turn on what was set up.");
        if (ImGui::GetTime() - g_v.vedaAsked > 10.0) g_v.vedaAsked = -1000.0; // keep asking it until it has them
        if (ComfyOwned()) {
            bool busy = false;
            if (app.comfySvc) {
                for (const ComfyActivity& a : app.comfySvc->Activity()) busy |= a.state != ComfyState::Done && a.state != ComfyState::Failed;
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(busy);
            if (ImGui::SmallButton("Restart ComfyUI##setup")) {
                std::string message;
                ComfyStopOwned();
                ComfyStart(app.comfy, message);
                app.SetStatus(message);
                g_v.vedaAsked = -1000.0;
            }
            ImGui::EndDisabled();
            if (busy && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("After the videos being made");
        } else {
            ImGui::SameLine();
            ImGui::TextDisabled("(close and open ComfyUI Desktop)");
        }
    }
    for (const char* line : {VedaSetupLine().c_str(), MaskSetupLine().c_str(), LpSetupLine().c_str()}) {
        const std::string l = line;
        if (l.rfind("error", 0) == 0) ImGui::TextColored(ui::kWarn, "Last try: %s", l.c_str());
    }
    ImGui::EndChild();
    DrawBestSettings("bestlist");
    ImGui::PopID();
}

// Everything the best swap uses is there and loaded (then its list goes under More options)
bool StarSetupComplete() {
    const bool there = g_ref.have && HasLora(kSwapLora) && HasLora(kSwapTurbo) && VedaOk() && MaskOk() && FacesOk() && CrispReady();
    return there && (!g_v.vedaKnown || (MaskReady() && VedaReady(kVedaStarPredictor) && FacesReady()));
}

LoadedImage* PickTexture(AppState& app, const std::wstring& path) {
    auto it = g_pick.tex.find(path);
    if (it == g_pick.tex.end()) {
        LoadedImage img;
        std::string err;
        if (!LoadImageTexture(app.Device(), path, img, &err)) img = LoadedImage{};
        it = g_pick.tex.emplace(path, img).first;
    }
    return it->second.srv ? &it->second : nullptr;
}

// 3. Who they replace: one frame per shot, everyone outlined, the picked people boxed in their card's colour; click to pick
void DrawWhoTheyReplace(AppState& app) {
    ui::SectionHeading("3   WHO THEY REPLACE");
    ImGui::PushTextWrapPos(0.0f);
    if (!MaskOk()) {
        ImGui::TextDisabled("Person masking isn't set up (More options > Set up everything missing). Without it the main performer is replaced.");
        ImGui::PopTextWrapPos();
        return;
    }
    if (g_star.video.empty() || g_star.who.empty()) {
        ImGui::TextDisabled("Choose the clip and who goes in: then the people in the clip can be found here, to click the one to replace.");
        ImGui::PopTextWrapPos();
        return;
    }
    const std::string key = PickKey();
    if (g_pick.askedKey != key && !PickCurrent()) { // nothing runs until asked
        const int parts = static_cast<int>(std::ceil(std::max(1.0, g_v.opt.seconds) / kSwapPartMax - 0.02));
        if (ui::PrimaryButton("Find the people##scan", ImVec2(0.0f, ui::Dp(30.0f)))) g_pick.askedKey = key, g_pick.wantAt = -100.0;
        ImGui::SameLine();
        ImGui::TextDisabled("SAM 3 finds everyone in the clip (about %d min%s), then you click who is replaced. Or just press Make: it finds them "
                            "first and replaces the main performer.", std::max(1, parts * 3 / 2), g_v.vedaKnown ? "" : ", ComfyUI starts first");
        ImGui::PopTextWrapPos();
        return;
    }
    if (!MaskReady()) {
        ImGui::TextDisabled("Starting ComfyUI to find the people in the clip...");
        ImGui::PopTextWrapPos();
        return;
    }
    if (!g_pick.scan || g_pick.scan->key != key) {
        std::string phase, part;
        {
            std::lock_guard<std::mutex> lock(g_scanPhaseMutex);
            phase = g_scanPhase, part = g_scanPart;
        }
        ImGui::TextColored(ui::kAccentHover, "%s", g_pick.fut.valid() ? ("Finding the people in the clip" + (part.empty() ? std::string() : " (" + part + ")") + ": " +
                                                                         (phase.empty() ? std::string("...") : phase) + " (about a minute for each 15 s)").c_str()
                                                                     : "The people in the clip are found once the part stays put...");
        // (seen: an earlier swap was still being made, and the search waited behind it in ComfyUI's queue with no word of why)
        int videos = 0;
        if (g_pick.fut.valid() && app.comfySvc)
            for (const ComfyActivity& a : app.comfySvc->Activity())
                videos += a.label.rfind("Video (", 0) == 0 && a.state != ComfyState::Done && a.state != ComfyState::Failed;
        if (videos > 0)
            ImGui::TextColored(ui::kWarn, "ComfyUI is making %s first (it does one job at a time): the people are found right after. Then click who they replace here.",
                               videos == 1 ? "another video" : "other videos");
        ImGui::PopTextWrapPos();
        return;
    }
    if (!g_pick.scan->ok) {
        ImGui::TextColored(ui::kWarn, "Could not find the people: %s", g_pick.scan->note.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::SmallButton("Try again##scan")) g_pick.scan.reset(), g_pick.wantAt = -100.0;
        return;
    }
    const PeopleScan& sc = *g_pick.scan;
    const size_t cards = g_star.who.size();
    if (cards > 1) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Clicking picks for:");
        for (size_t k = 0; k < cards; ++k) {
            ImGui::SameLine(0.0f, ui::Dp(4.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kCastCol[k % 4]));
            if (Chip((StarName(g_star.who[k]) + "##pickfor" + std::to_string(k)).c_str(), g_pick.active == static_cast<int>(k))) g_pick.active = static_cast<int>(k);
            ImGui::PopStyleColor();
        }
    }
    ImGui::TextDisabled(cards > 1 ? "Click a person to make them the one the chosen card replaces; click again to take them off."
                                  : "Click the person to replace (picked: boxed). It is followed into the other shots by their clothes - click there to correct it.");
    ImGui::PopTextWrapPos();
    // the shots
    const float th = ui::Dp(200.0f), gap = ui::Dp(8.0f);
    const int shots = static_cast<int>(sc.kept.size());
    for (int s = 0; s < shots; ++s) {
        const float tw = th * sc.aw / std::max(1, sc.ah);
        if (s > 0) {
            if (ImGui::GetContentRegionAvail().x - ImGui::GetItemRectMax().x + ImGui::GetCursorScreenPos().x > tw + gap) ImGui::SameLine(0.0f, gap);
        }
        ImGui::BeginGroup();
        ImGui::PushID(s);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##shot", ImVec2(tw, th));
        const bool hov = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + tw, p.y + th), ui::ThumbPlaceholder(), ui::Dp(6.0f));
        if (const LoadedImage* img = PickTexture(app, sc.png[s])) dl->AddImage((ImTextureID)(intptr_t)img->srv, p, ImVec2(p.x + tw, p.y + th));
        const float kx = tw / sc.aw, ky = th / sc.ah;
        auto box = [&](const ScanPerson& q) { return std::make_pair(ImVec2(p.x + q.x0 * kx, p.y + q.y0 * ky), ImVec2(p.x + (q.x1 + 1) * kx, p.y + (q.y1 + 1) * ky)); };
        int under = -1;
        if (hov) {
            const int lx = std::clamp(static_cast<int>((ImGui::GetIO().MousePos.x - p.x) / kx), 0, sc.aw - 1);
            const int ly = std::clamp(static_cast<int>((ImGui::GetIO().MousePos.y - p.y) / ky), 0, sc.ah - 1);
            const uint8_t l = sc.labels[s][static_cast<size_t>(ly) * sc.aw + lx];
            if (l != 255 && l < sc.kept[s].size()) under = l;
        }
        for (size_t k = 0; k < cards && s < static_cast<int>(g_pick.pick.size()); ++k)
            for (int i : PickedSet(s, k)) {
            if (sc.kept[s][i].x1 < 0) continue;
            const auto [a, b] = box(sc.kept[s][i]);
            dl->AddRect(a, b, kCastCol[k % 4], ui::Dp(3.0f), 0, ui::Dp(3.0f));
            const std::string nm = StarName(g_star.who[k]);
            const ImVec2 ts = ImGui::CalcTextSize(nm.c_str());
            dl->AddRectFilled(ImVec2(a.x, a.y), ImVec2(a.x + ts.x + ui::Dp(8.0f), a.y + ts.y + ui::Dp(4.0f)), kCastCol[k % 4], ui::Dp(3.0f));
            dl->AddText(ImVec2(a.x + ui::Dp(4.0f), a.y + ui::Dp(2.0f)), IM_COL32(10, 12, 16, 255), nm.c_str());
            }
        if (under >= 0 && sc.kept[s][under].x1 >= 0) {
            const auto [a, b] = box(sc.kept[s][under]);
            dl->AddRect(a, b, IM_COL32(255, 255, 255, 220), ui::Dp(3.0f), 0, ui::Dp(1.5f));
            const int k = std::clamp(g_pick.active, 0, static_cast<int>(cards) - 1);
            const bool already = s < static_cast<int>(g_pick.pick.size()) && g_pick.pick[s][k] == under;
            ImGui::SetTooltip(already ? "Click: %s replaces nobody in this shot" : "Click: %s replaces this person", StarName(g_star.who[k]).c_str());
            if (ImGui::IsItemClicked()) ClickPick(s, under, static_cast<size_t>(k));
        }
        if (sc.kept[s].empty()) dl->AddText(ImVec2(p.x + ui::Dp(8.0f), p.y + ui::Dp(8.0f)), IM_COL32(255, 255, 255, 200), "nobody found");
        ImGui::TextDisabled("%.1f - %.1f s", sc.bounds[s] / 24.0, sc.bounds[s + 1] / 24.0);
        ImGui::PopID();
        ImGui::EndGroup();
    }
    // what will happen, before anything is made
    for (size_t k = 0; k < cards && !g_pick.pick.empty(); ++k) {
        int with = 0, peopled = 0;
        double secs = 0, all = 0;
        std::string missing;
        for (int s = 0; s < shots; ++s) {
            const double len = (sc.bounds[s + 1] - sc.bounds[s]) / 24.0;
            all += len;
            if (sc.kept[s].empty()) continue;
            ++peopled;
            if (g_pick.pick[s][k] >= 0) {
                ++with, secs += len;
            } else {
                char m[48];
                snprintf(m, sizeof(m), "%s%.1f-%.1f s", missing.empty() ? "" : ", ", sc.bounds[s] / 24.0, sc.bounds[s + 1] / 24.0);
                missing += m;
            }
        }
        const ImVec2 c = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(c.x + ui::Dp(6.0f), c.y + ImGui::GetTextLineHeight() * 0.5f + ui::Dp(1.0f)), ui::Dp(5.0f), kCastCol[k % 4]);
        ImGui::Dummy(ImVec2(ui::Dp(14.0f), 1.0f));
        ImGui::SameLine();
        const std::string d = k < g_pick.desc.size() ? g_pick.desc[k] : std::string();
        if (with == 0) {
            ImGui::TextColored(ui::kWarn, "%s replaces nobody yet: click the person they replace.", StarName(g_star.who[k]).c_str());
            continue;
        }
        ImGui::Text("%s replaces %s", StarName(g_star.who[k]).c_str(), d.empty() ? "the boxed person" : d.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("- in %d of %d shot%s, %.1f of %.1f s", with, peopled, peopled == 1 ? "" : "s", secs, all);
        if (!missing.empty()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ui::kWarn, "   Not picked in %s: if they are in it, click them there.", missing.c_str());
            ImGui::PopTextWrapPos();
        }
    }
}

void DrawStarMode(AppState& app, float footerH) {
    H3Options& o = g_v.opt;
    CheckRefModel();
    PumpClipPeople();
    PumpPick(app);
    // how: swap the person (the clip guides every frame) or recreate the clip with them (the clip as a reference)
    if (Chip("Swap the person##sm0", g_starMethod == 0)) g_starMethod = 0, SaveSettings();
    ImGui::SameLine(0.0f, ui::Dp(4.0f));
    if (Chip("Recreate the clip with them##sm1", g_starMethod == 1)) g_starMethod = 1, SaveSettings();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled(g_starMethod == 0 ? "The clip stays as it is - place, camera, moves, timing; only the people in it change (Character Swap)."
                                          : "A new video made from the clip as a reference: freer (another place, another outfit), but the clip's people can blend in.");
    ImGui::PopTextWrapPos();
    if (g_starMethod != 0) DrawSwapFiles(); // (a swap's are in the set-up panel)

    // ---- 1. the clip
    ui::SectionHeading("1   THE CLIP");
    if (!g_star.video.empty()) {
        if (g_star.infoFor != g_star.video) {
            auto known = g_lib.info.find(g_star.video); // the browser probed it already
            g_star.info = known != g_lib.info.end() && known->second.seconds > 0 ? known->second : ProbeVideo(g_star.video);
            g_star.infoFor = g_star.video;
            o.seconds = SwapLength(o.seconds, g_star.info.seconds);
        }
        DrawClipCard(app);
    }
    if (g_star.video.empty() || g_starBrowse) DrawSwapLibrary(app);
    if (!g_star.video.empty()) {
        ImGui::Spacing();
        DrawTrimmer(app);
    } else if (g_lib.files.empty()) {
        ImGui::TextDisabled("Choose a clip (or right-click one under Videos >).");
    }

    // ---- 2. the cast
    ui::SectionHeading("2   WHO GOES IN");
    {
        const float w = ui::Dp(118.0f), gap = ui::Dp(12.0f);
        int remove = -1;
        for (size_t k = 0; k < g_star.who.size(); ++k) {
            if (k) ImGui::SameLine(0.0f, gap);
            if (k && ImGui::GetContentRegionAvail().x < w) ImGui::NewLine();
            DrawCastCard(app, k, w, remove);
        }
        if (remove >= 0) {
            g_star.replaces.erase(StarKeyOf(g_star.who[remove]));
            g_star.who.erase(g_star.who.begin() + remove);
        }
        if (g_star.who.size() < 4) {
            if (!g_star.who.empty()) ImGui::SameLine(0.0f, gap);
            DrawAddCastCard(app, w);
        }
    }
    if (g_starMethod == 1 && !g_star.who.empty() && !g_star.video.empty()) { // Recreate: whom they replace, in words, for the cards' lists
        ImGui::BeginDisabled(g_peopleFut.valid());
        if (ImGui::SmallButton(g_peopleFut.valid() ? "Looking for the people in the clip..." : g_people.empty() ? "Find the people in the clip" : "Find them again")) {
            if (!g_people.empty()) { // asked again: forget what was found for this part
                std::lock_guard<std::mutex> lock(g_swapCacheMutex);
                SwapCacheLocked()["people"].erase(g_peopleKey);
            }
            g_people.clear();
            g_peopleNote.clear();
            g_peopleFut = std::async(std::launch::async, FindClipPeople, g_star.video, g_star.start, o.seconds, app.comfy.url, nullptr);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("The prompt writer looks at the marked part and lists who is in it: then each card can pick whom it replaces.\nOptional - "
                              "your idea (\"Ava takes the girl's place\") does the same.");
        if (!g_people.empty()) { // (the line stays open only when something follows: the next heading drew on it)
            std::string list;
            for (const std::string& who : g_people) list += (list.empty() ? "" : ",  ") + who;
            ImGui::SameLine();
            ImGui::TextDisabled("In it: %s", list.c_str());
        } else if (!g_peopleNote.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(ui::kWarn, "%s", g_peopleNote.c_str());
        }
    }
    if (g_starMethod == 1 && !g_star.who.empty()) { // Recreate: its character sheet, when it has one
        const CharacterProfile* cp = FindProfile(g_star.who.front());
        if (cp && !cp->sheet.empty() && CharacterSheetUseful(g_star.who.front())) {
            if (ImGui::Checkbox("Use its character sheet", &g_v.starUseSheet)) SaveSettings();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("The character goes in as its sheet (front, side and back views) instead of the picture: in tests it kept the hardest angles "
                                  "best and drew the back of a costume as designed.\n(Swap the person always uses the picture: there the sheet let the clip's "
                                  "performer leak back in.)");
        }
    }

    // ---- 3. who they replace (a swap): clicked
    if (g_starMethod == 0) DrawWhoTheyReplace(app);

    // ---- direction
    ui::SectionHeading(g_starMethod == 0 ? "4   DIRECTION (OPTIONAL)" : "3   DIRECTION (OPTIONAL)");
    WrapEdit("##staridea", g_star.idea, ui::Dp(48.0f));
    if (g_star.idea.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled(g_starMethod == 0 ? "Anything else, e.g. \"she smiles the whole time\". (Who is replaced is clicked above.)"
                                              : "e.g. \"in a neon-lit club\", \"wearing a gold suit\". Empty: they simply take the performers' places.");
        ImGui::PopTextWrapPos();
    }
    const std::string key = StarKey();
    const bool writingHere = g_writeFut.valid() && g_writing.toPage && g_writing.kind == 1;
    if (writingHere) {
        std::string phase;
        {
            std::lock_guard<std::mutex> lock(g_phaseMutex);
            phase = g_phase;
        }
        ImGui::TextColored(ui::kAccentHover, "%s", phase.empty() ? "Cutting the clip and writing the prompt..." : phase.c_str());
    }
    if (ImGui::TreeNode("The prompt H3 gets##starprompttree")) {
        ImGui::Checkbox("Write it for me##star", &g_v.autoWrite);
        if (!writingHere && WrapEdit("##starprompt", g_star.prompt, ui::Dp(120.0f))) g_star.edited = true;
        if (g_star.prompt.empty())
            ImGui::TextDisabled(g_starMethod == 0 ? "Written when you press Make: body_swap: and how each person looks (the closer, the better the likeness)."
                                                  : "Written when you press Make (it names <Picture 1>, <Video 1> and <Audio 1>).");
        else if (g_v.autoWrite && !g_star.edited && g_star.writtenKey != key) ImGui::TextColored(ui::kTextDim, "Out of date: written again when you press Make.");
        ImGui::TreePop();
    } else if (!g_star.prompt.empty() && g_v.autoWrite && !g_star.edited && g_star.writtenKey != key) {
        ImGui::SameLine();
        ImGui::TextDisabled("(written again when you press Make)");
    }

    // ---- output: quality and size; the rest under More options (what is missing for the best swap comes first)
    ui::SectionHeading(g_starMethod == 0 ? "5   OUTPUT" : "4   OUTPUT");
    if (g_starMethod == 0 && !StarSetupComplete()) DrawStarSetup(app);
    std::string fastLora;
    for (const char* l : kRefLoras) {
        if (HasLora(l)) {
            fastLora = l;
            break;
        }
    }
    if (g_starMethod == 0) fastLora = SwapTurbo();
    else if (g_ref.omni) fastLora = std::string(kOmniModel) + " (FastH3-Omni, 8 steps)";
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Quality");
    ImGui::SameLine();
    if (Chip(fastLora.empty() ? "Fast (no turbo LoRA)##sq0" : "Fast##sq0", g_star.quality == 0)) g_star.quality = 0;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip(fastLora.empty() ? "Needs a ref2v turbo LoRA in ComfyUI's loras folder (e.g. minimax_h3_ref2v_turbo_4step_v1.0_768p_comfyui_bf16)" : "%s", fastLora.c_str());
    ImGui::SameLine(0.0f, ui::Dp(4.0f));
    if (Chip(g_starMethod == 0 ? "Best (16 steps)##sq1" : "Best (20 steps)##sq1", g_star.quality == 1 || fastLora.empty())) g_star.quality = 1;
    if (fastLora.empty()) g_star.quality = 1;
    ImGui::SameLine(0.0f, ui::Dp(18.0f));
    ImGui::TextDisabled("Size");
    for (int ri = 0; ri < 4; ++ri) {
        ImGui::SameLine(0.0f, ui::Dp(4.0f));
        char lbl[24];
        snprintf(lbl, sizeof(lbl), "%s##sres%d", kRes[ri].name, ri);
        if (Chip(lbl, o.res == ri)) o.res = ri, SaveSettings();
    }
    if (g_star.video.empty()) { // (with a clip, the trimmer sets the length)
        float sec = static_cast<float>(o.seconds);
        ImGui::SetNextItemWidth(ui::Dp(220.0f));
        if (ImGui::SliderFloat("##starsecs", &sec, 3.0f, 15.0f, "%.0f seconds")) o.seconds = std::round(sec), SaveSettings();
    }
    if (g_starMethod != 0 && g_ref.known && !g_ref.omni) DrawOmniDownload();
    // (a swap's set-ups are all in the panel above: the rows show only their switches)
    if (ImGui::CollapsingHeader("More options##starmore")) {
        if (g_starMethod == 0 && StarSetupComplete()) DrawStarSetup(app);
        if (g_starMethod != 0 || VedaReady(kVedaStarPredictor)) DrawVedaRow(app, kVedaStarPredictor);
        else if (VedaOk() && ImGui::Checkbox("Speed up with Veda##disk", &g_v.veda)) SaveSettings();
        DrawModelLoras(app, app.comfy.h3Loras, "MiniMax H3", "slr"); // (the same list as Make a video)
        if (g_starMethod == 0 && MaskReady()) {
            DrawMaskRow();
        } else if (g_starMethod == 0 && MaskOk()) { // (installed; ComfyUI not running yet)
            if (ImGui::Checkbox("Only change the person (keep everything else exactly)##disk", &g_v.swapMask)) SaveSettings();
            if (g_v.swapMask && ImGui::Checkbox("Two or more people: one at a time (never mixes them up)##disk", &g_v.oneAtATime)) SaveSettings();
        }
        DrawTweaksRow(g_starMethod == 0);
        if (g_starMethod == 0) {
            if (FacesReady()) DrawFacesRow();
            else if (FacesOk() && ImGui::Checkbox("Then copy the original's facial expressions (LivePortrait)##disk", &g_v.facesAfterSwap)) SaveSettings();
            if (CrispReady() && ImGui::Checkbox("Then make it crisp (Open-H3)", &g_v.crispAfterSwap)) SaveSettings();
            if (CrispReady() && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("Once the swap is done it is redrawn at %dp with fine detail (Make it crisp, under the player): a second video, Crisp_...%s\n\n"
                                  "It keeps the faces (MiniMax H3 redraws only the fine detail); making the swap at 1080p to begin with still gives the most detail.",
                                  std::min(g_v.crispTo, 1440), CrispReady() ? "" : "\nNeeds Open-H3: Set up Make it crisp, under the player.");
        }
    }
    // the size of the job: its time, and whether it is too big for this PC
    int vw = 0, vh = 0;
    {
        H3Options so = o;
        so.shape = 0;
        H3Size(so, g_star.info.width > 0 ? g_star.info.width : 9, g_star.info.height > 0 ? g_star.info.height : 16, vw, vh);
    }
    const bool longSwap = g_starMethod == 0 && o.seconds > kSwapPartMax + 0.2; // (made in parts of at most 10 s: memory is a part's)
    const std::string heavy = HeavyNote(vw, vh, longSwap ? kSwapPartMax : o.seconds, true);
    if (!heavy.empty()) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ui::kWarn, "%s", heavy.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild(); // the form

    // the footer: the model check, then Make
    ImGui::Separator();
    if (!g_ref.have) {
        if (!g_ref.known) {
            ImGui::TextColored(ui::kTextDim, "Needs %s in ComfyUI's diffusion_models (its models folder was not found).", kRefModel);
        } else if (g_ref.downloading) {
            std::error_code ec;
            const double gb = fs::exists(g_ref.part, ec) ? fs::file_size(g_ref.part, ec) / 1073741824.0 : 0.0;
            ImGui::TextColored(ui::kAccentHover, "Downloading the reference model: %.1f of 19.5 GB...", gb);
        } else {
            if (ui::PrimaryButton("Download it (19.5 GB)", ImVec2(ui::Dp(190.0f), ui::Dp(38.0f)))) StartRefDownload();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s from Comfy-Org/MiniMax-H3 into ComfyUI's diffusion_models", kRefModel);
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::TextUnformatted("Starring in a video needs MiniMax H3's reference model.");
            ImGui::TextDisabled(g_ref.failed ? "The last download failed: try again." : "One time, 19.5 GB; it resumes if stopped.");
            ImGui::EndGroup();
        }
        return;
    }
    const bool needWrite = g_v.autoWrite && (g_star.prompt.empty() || (!g_star.edited && g_star.writtenKey != key));
    static const bool testSwapAnyway = GetEnvironmentVariableW(L"MV_STAR_SWAPANYWAY", nullptr, 0) > 0; // tests: let ComfyUI check the graph
    const bool swapReady = g_starMethod != 0 || HasLora(kSwapLora) || testSwapAnyway;
    // a swap with person masking waits for the people to be found and each card to replace someone (a failed search: the old way)
    bool picksMissing = false;
    // (and while the writer describes whom they replace: started together, the prompt's own writing failed and the take had no look of the
    // new person at all - the model drew the other man in the clip instead)
    // (and while ComfyUI has not answered whether masking is there: pressed then, a 29 s swap went as one 15 s job that filled the card)
    const bool comfyPending = g_starMethod == 0 && !g_v.vedaKnown && MaskOk() && !g_star.video.empty() && !g_star.who.empty();
    const bool scanPending = comfyPending || (g_starMethod == 0 && MaskReady() && !g_star.video.empty() && !g_star.who.empty() &&
                                              (!g_pick.scan || g_pick.scan->key != PickKey() || g_pick.descFut.valid()));
    if (g_starMethod == 0 && PickCurrent())
        for (size_t k = 0; k < g_star.who.size(); ++k) {
            bool any = false;
            for (const auto& shot : g_pick.pick) any |= k < shot.size() && shot[k] >= 0;
            picksMissing |= !any;
        }
    const bool readyButPeople = !writingHere && swapReady && !g_star.who.empty() && !g_star.video.empty() && (needWrite || !g_star.prompt.empty()) && !picksMissing;
    const bool ready = readyButPeople && !scanPending;
    if (g_pick.makeWhenReady && (!readyButPeople || g_pick.askedKey != PickKey())) g_pick.makeWhenReady = false; // (the part or the people changed)
    ImGui::BeginDisabled(!readyButPeople || g_pick.makeWhenReady);
    bool pressed = ui::PrimaryButton(needWrite ? "Write it and make" : "Make the video", ImVec2(ui::Dp(190.0f), ui::Dp(38.0f))) ||
                   (readyButPeople && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Enter));
    ImGui::EndDisabled();
    if (g_v.pressMake && readyButPeople) pressed = true;
    static const bool holdMake = GetEnvironmentVariableW(L"MV_STAR_HOLDMAKE", nullptr, 0) > 0; // tests: pressed once the form is filled
    if (readyButPeople || !holdMake) g_v.pressMake = false;
    if (pressed && !ready && g_starMethod == 0) { // the people first (ComfyUI started if need be), then it is made by itself
        g_pick.askedKey = PickKey();
        if (!g_pick.scan || g_pick.scan->key != g_pick.askedKey) g_pick.wantAt = -100.0;
        g_pick.makeWhenReady = true;
        pressed = false;
        StepLog("Make pressed: the people are found first");
    }
    bool make = pressed && ready;
    if (g_pick.makeWhenReady && ready) make = true, g_pick.makeWhenReady = false;
    if (make) StepLog("Make: " + std::string(g_starMethod == 0 ? "swap" : "recreate") + ", " + std::to_string(static_cast<int>(o.seconds)) + " s");
    if (make) {
        WriteJob j;
        j.kind = 1;
        j.who = g_star.who;
        for (const std::wstring& w : g_star.who) j.names.push_back(StarName(w)), j.replaces.push_back(StarReplaces(w));
        if (g_v.vedaKnown) j.sam3 = g_v.vedaInfo.sam3; // (the pictures' people are cut out onto white with it)
        if (g_starMethod == 0 && PickCurrent()) { // whom each replaces: clicked on the page
            j.scan = g_pick.scan;
            j.picks = PicksAsObjects();
            for (size_t k = 0; k < j.replaces.size(); ++k) j.replaces[k] = PickedReplaces(k);
        }
        j.refVideo = g_star.video;
        j.refStart = g_star.start;
        j.keepSound = g_star.keepSound;
        j.idea = g_star.idea;
        j.opt = o;
        j.starQuality = g_star.quality;
        j.starMethod = g_starMethod;
        for (const CharacterProfile& cp : AllProfiles()) { // a saved character's own description: the look when the writer cannot see
            if (!g_star.who.empty() && !cp.image.empty() && _wcsicmp(cp.image.c_str(), g_star.who.front().c_str()) == 0) j.context = cp.description;
        }
        j.key = key;
        j.write = needWrite;
        j.prompt = g_star.prompt;
        j.toPage = true;
        j.sendAfter = true;
        std::string names;
        for (size_t k = 0; k < j.names.size(); ++k) names += (k == 0 ? "" : k + 1 == j.names.size() ? " & " : ", ") + j.names[k];
        j.label = ShortLabel(names + " in " + Utf8(fs::path(g_star.video).stem().wstring()));
        if (g_starMethod == 0 && j.scan && !j.scan->parts.empty()) { // a long swap: its parts, joined when all are kept
            const PeopleScan& sc = *j.scan;
            LongSwap ls;
            ls.id = g_nextLongId++;
            ls.files.assign(sc.parts.size(), std::wstring());
            ls.lens = sc.partLen;
            ls.src = g_star.video, ls.start = g_star.start, ls.keepSound = g_star.keepSound, ls.label = j.label, ls.people = static_cast<int>(j.who.size());
            {
                std::lock_guard<std::mutex> lock(g_longMutex);
                g_longSwaps.push_back(ls);
            }
            for (size_t p = 0; p < sc.parts.size(); ++p) {
                WriteJob jp = j;
                jp.refStart = g_star.start + sc.partStart[p];
                jp.opt.seconds = sc.partLen[p];
                jp.scan = sc.parts[p];
                jp.picks.assign(sc.parts[p]->kept.size(), std::vector<std::vector<int>>(j.who.size()));
                for (size_t s = 0; s < sc.shotOf.size() && s < j.picks.size(); ++s)
                    if (sc.shotOf[s].first == static_cast<int>(p)) jp.picks[sc.shotOf[s].second] = j.picks[s];
                jp.longId = ls.id, jp.longPart = static_cast<int>(p);
                jp.key += "|part" + std::to_string(p);
                jp.label = j.label + " (part " + std::to_string(p + 1) + " of " + std::to_string(sc.parts.size()) + ")";
                g_writeQueue.push_back(std::move(jp));
            }
            Toast(ToastKind::Info, "A long swap, in " + std::to_string(sc.parts.size()) + " parts", j.label + ": each part is swapped on its own, then they are joined with the clip's sound",
                  {}, [](AppState& s) { s.requestTab = 10; }, "videosend");
        } else if (g_starMethod == 0 && j.opt.seconds > kSwapPartMax + 0.2) { // long, without person masking: in parts all the same (the main performer)
            const auto plan = PlanSwapParts(g_star.video, g_star.start, j.opt.seconds);
            LongSwap ls;
            ls.id = g_nextLongId++;
            ls.files.assign(plan.size(), std::wstring());
            for (const auto& pp : plan) ls.lens.push_back(pp.second);
            ls.src = g_star.video, ls.start = g_star.start, ls.keepSound = g_star.keepSound, ls.label = j.label, ls.people = static_cast<int>(j.who.size());
            {
                std::lock_guard<std::mutex> lock(g_longMutex);
                g_longSwaps.push_back(ls);
            }
            for (size_t p = 0; p < plan.size(); ++p) {
                WriteJob jp = j;
                jp.refStart = g_star.start + plan[p].first;
                jp.opt.seconds = plan[p].second;
                jp.scan.reset(), jp.picks.clear();
                jp.longId = ls.id, jp.longPart = static_cast<int>(p);
                jp.key += "|part" + std::to_string(p);
                jp.label = j.label + " (part " + std::to_string(p + 1) + " of " + std::to_string(plan.size()) + ")";
                g_writeQueue.push_back(std::move(jp));
            }
        } else {
            g_writeQueue.push_back(std::move(j));
        }
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    if (!swapReady) ImGui::TextDisabled("Get Character Swap first (at the top), or choose Recreate.");
    else if (scanPending && g_pick.makeWhenReady) ImGui::TextColored(ui::kAccentHover, "%s", comfyPending ? "Starting ComfyUI, finding the people, then it is made..." : g_pick.descFut.valid() ? "Describing whom they replace, then it is made..." : "Finding the people in the clip, then it is made...");
    else if (scanPending && g_pick.askedKey == PickKey()) ImGui::TextDisabled(comfyPending ? "Starting ComfyUI first (it finds the people in the clip)." : g_pick.descFut.valid() ? "Describing whom they replace..." : "Finding the people in the clip first (Who they replace).");
    else if (picksMissing) ImGui::TextColored(ui::kWarn, "Click who each person replaces (Who they replace).");
    else if (g_star.who.empty() || g_star.video.empty()) ImGui::TextDisabled(g_star.video.empty() ? "Choose the clip first." : "Add who goes in.");
    else {
        std::string who;
        for (size_t k = 0; k < g_star.who.size(); ++k) who += (k == 0 ? "" : k + 1 == g_star.who.size() ? " & " : ", ") + StarName(g_star.who[k]);
        ImGui::Text("%s: %s, %.0f s%s, %s, %s", g_starMethod == 0 ? "Swap" : "Recreate", who.c_str(), o.seconds,
                    g_starMethod == 0 && o.seconds > kSwapPartMax + 0.2 ? " (in parts of 10 s at most, joined)" : "",
                    kRes[o.res].name, g_star.quality == 0 ? "Fast" : "Best");
    }
    { // about how long. Measured 2026-10-21/22 on the RTX 3090, Fast + Veda, 640 x 1152, with loading: a swap's 10 s part 690 s and a 5 s one
      // 377 s (Recreate with FastH3-Omni about the same: 386-395 s for 5 s), and finding the people about 85 s for each 10 s. The old
      // guess (twice an animation) said 14 minutes for a 29 s swap that took 39.
        const int parts = longSwap ? std::max(2, static_cast<int>(std::ceil(o.seconds / kSwapPartMax - 0.02))) : 1;
        const double part = o.seconds / parts;
        double est = parts * (46.0 + 3.6e-6 * vw * vh * FramesFor(part));
        if (g_star.quality != 0) est *= 3.0;                         // 16 or 20 steps instead of 4 or 8
        if (!(g_v.veda && (g_v.vedaKnown ? VedaReady(kVedaStarPredictor) : VedaOk()))) est *= 2.0; // Veda: about twice as fast (ComfyUI off: installed counts)
        if (g_starMethod == 0 && MaskOk() && !PickCurrent()) est += 8.5 * o.seconds;
        ImGui::TextDisabled("about %.0f minutes%s  (Ctrl+Enter)", std::max(1.0, std::round(est / 60.0)), heavy.empty() ? "" : " if it fits in memory");
    }
    ImGui::EndGroup();
}


} // namespace

std::string VideoSam3() { return g_v.vedaKnown ? g_v.vedaInfo.sam3 : std::string(); } // (ComfyUI's SAM 3 checkpoint, once it answered)

H3Options& VideoOptions() {
    if (!g_v.loaded) LoadSettings();
    return g_v.opt;
}

bool SendH3(AppState& app, const std::wstring& first, const std::string& asked, const H3Options& o, const std::string& label) {
    // with the trigger words of the H3 LoRAs that are on (not LTX-2.5's: they are not put on it)
    const std::string prompt = o.quality == kLtxQuality ? asked : WithLoraTriggers(app, asked, o.ownLoras || !o.loras.empty() ? o.loras : app.comfy.h3Loras);
    int srcW = 0, srcH = 0;
    if (!first.empty()) {
        const AppState::LibThumb lt = app.LibraryThumb(first);
        srcW = lt.w, srcH = lt.h;
        if (srcW <= 0) srcW = 16, srcH = 9; // not read yet: its own shape is kept by the workflow anyway
    }
    int w = 0, h = 0;
    H3Size(o, srcW, srcH, w, h);
    H3Options run = o;
    // Presenter always takes Veda when it is set up: tested at 640p and 768p, without it the presenter LoRA lets leaves, branches and captions
    // drift over the picture (its livestream training); with Veda's sparse attention the picture stays clean
    run.veda = VedaFor(kVedaPredictor) || (o.quality == 3 && (VedaReady(kVedaPredictor) || !g_v.vedaKnown));
    // Presenter ends on the start picture when no end picture is given: tested at 768p with Veda, a clip whose speech was "three easy tips"
    // got a title card over the picture every way but this (a negative prompt at CFG 1.5 and the LoRA at 0.8 still showed it); with a clean
    // last frame to arrive at, nothing is laid over the picture, and the presenter still talks and gestures in between
    const bool longVideo = run.seconds > 15.0 + 1e-6 || run.parts >= 2;
    if (longVideo && !LongReady()) {
        app.SetStatus("Videos over 15 s need H3 Continuum (Video page > Length: Set up): made at 15 s instead");
        run.seconds = std::min(run.seconds, 15.0);
        run.parts = 0;
    }
    const bool asLong = run.seconds > 15.0 + 1e-6 || run.parts >= 2;
    if (o.quality == kLtxQuality && asLong) {
        app.SetStatus("LTX-2.5 makes one clip of up to 15 s: this long video is made with Fast");
        run.quality = 0;
    } else if (o.quality == kLtxQuality) {
        if (!LtxReady()) {
            app.SetStatus("LTX-2.5's files are not in ComfyUI's models folders");
            return false;
        }
        w = std::max(64, w / 64 * 64), h = std::max(64, h / 64 * 64); // (its first pass is half the size, in steps of 32)
        ComfyJob job;
        job.apiWorkflow = BuildLtx(prompt, run.seconds, !first.empty(), !run.last.empty(), w, h).dump();
        if (!first.empty()) job.rawUploads.push_back({"@first", first});
        if (!run.last.empty()) job.rawUploads.push_back({"@last", run.last});
        job.label = "Video (LTX-2.5): " + label;
        job.times = std::max(1, o.times);
        if (o.fixedSeed) job.seed = 42;
        const int act = SendComfyJob(app, std::move(job));
        if (act) Toast(ToastKind::Info, "Making a video (LTX-2.5)", label, {}, [](AppState& s) { s.requestTab = 10; }, "videosend");
        return act != 0;
    }
    if (o.quality == 3 && run.last.empty() && !first.empty() && !asLong) run.last = first;
    json wf = asLong ? BuildH3Long(run, !first.empty(), !run.last.empty(), w, h) : BuildH3(run, !first.empty(), !run.last.empty(), w, h);
    if (asLong && wf.contains("11")) wf["11"]["inputs"]["prompt_mode"] = ContinuumPromptMode(prompt);
    ApplyLoras(wf, o.ownLoras || !o.loras.empty() ? o.loras : app.comfy.h3Loras, false); // your MiniMax H3 LoRAs (Video page / Generate), before the speed LoRA in the chain
    // one file per run: runs waiting in the queue keep their own settings
    static int n = 0;
    const std::wstring dir = DataDir() + L"\\h3";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::wstring file = dir + L"\\run_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(++n) + L".json";
    {
        std::ofstream f(file, std::ios::trunc);
        if (!f) {
            app.SetStatus("Could not write the video workflow");
            return false;
        }
        f << wf.dump(1);
    }
    std::vector<std::pair<std::string, std::wstring>> nodePics;
    if (!run.last.empty()) nodePics.emplace_back("20", run.last);
    const Quality& q = kQuality[std::clamp(run.quality, 0, kQualityCount - 1)];
    const std::string how = std::string(q.name) + (run.veda ? ", Veda" : "") + (asLong ? ", " + std::to_string(static_cast<int>(std::lround(run.seconds))) + " s long" : std::string());
    const bool sent = SendH3Workflow(app, file, first, nodePics, o.quality == 3 ? PresenterReady(prompt) : prompt, std::min(run.seconds, 15.0), o.times,
                                     (asLong ? "Long video (" : "Video (") + how + "): " + label, o.fixedSeed);
    if (sent) Toast(ToastKind::Info, "Making a video (" + how + ")", label, {}, [](AppState& s) { s.requestTab = 10; }, "videosend");
    if (sent && o.quality == 3) g_presenterTakes.push_back({LastEditActivity(), first, prompt, o, label, !g_presenterRetrying});
    return sent;
}

// ---- for the story maker (story_ui.cpp)
std::string WriteVideoPromptFor(const std::wstring& first, const std::string& idea, const std::string& context, const H3Options& o, const std::string& comfyUrl,
                                std::string* note) {
    WriteJob j;
    j.first = first;
    j.idea = idea;
    j.context = context;
    j.opt = o;
    const std::string text = WriteVideoPrompt(j, comfyUrl);
    if (note) {
        std::lock_guard<std::mutex> lock(g_phaseMutex);
        *note = g_writeNote;
    }
    return text;
}

double EstimateH3Seconds(const H3Options& o, int w, int h) {
    H3Options e = o;
    e.veda = (g_v.veda || o.quality == 3) && (VedaReady(kVedaPredictor) || (!g_v.vedaKnown && g_v.vedaWasReady)); // (Presenter always takes it)
    return EstimateSeconds(e, w, h);
}

void H3SizeFor(const H3Options& o, int srcW, int srcH, int& w, int& h) { H3Size(o, srcW, srcH, w, h); }

const char* H3QualityName(int quality) { return kQuality[std::clamp(quality, 0, kQualityCount - 1)].name; }

void PlayInVideoPage(AppState& app, const std::wstring& path, bool autoplay) { PlayClip(app, path, autoplay); }
bool H3LongReady() { return LongReady(); }

bool SendH3Turbo(AppState& app, const std::wstring& picture, const std::string& prompt, double seconds, const std::string& label, int times) {
    H3Options o = VideoOptions();
    o.seconds = seconds > 0 ? seconds : o.seconds;
    o.times = std::max(1, times);
    o.last.clear();
    return SendH3(app, picture, prompt, o, label);
}

int SendH3LoraPreview(AppState& app, const std::string& prompt, const std::vector<LoraEntry>& loras) {
    H3Options o = VideoOptions();
    o.quality = HasLora(kFastLora) ? 0 : 2; // Fast (4 steps), else the model alone
    o.res = 0;                              // 480p
    o.shape = 1;                            // landscape
    o.seconds = 3;
    o.times = 1;
    o.last.clear();
    o.parts = 0, o.partSeconds = 0;
    o.loras = loras, o.ownLoras = true, o.fixedSeed = true;
    return SendH3(app, std::wstring(), prompt, o, "LoRA preview") ? LastEditActivity() : 0;
}

void SendH3Auto(AppState& app, const std::wstring& first, const std::string& idea, double seconds, const std::string& label, int times,
                const std::vector<LoraEntry>& loras) {
    WriteJob j;
    j.first = first;
    j.idea = idea;
    j.label = label;
    j.opt = VideoOptions();
    j.opt.loras = loras;
    j.opt.times = std::max(1, times);
    j.opt.seconds = seconds > 0 ? seconds : j.opt.seconds;
    j.opt.last.clear();
    j.sendAfter = true;
    if (!g_v.autoWrite) {
        SendH3(app, first, idea, j.opt, label); // written by hand
        return;
    }
    g_writeQueue.push_back(std::move(j));
}

void StarPickBar(AppState& app) {
    static bool testPick = [] { // MV_UI_TEST "starpick": the bar as "Choose from the gallery" opens it (tests)
        char buf[256] = {};
        return GetEnvironmentVariableA("MV_UI_TEST", buf, sizeof(buf)) && strstr(buf, "starpick") != nullptr;
    }();
    if (testPick) testPick = false, g_starPick = true;
    if (!g_starPick) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y - ui::Dp(70.0f)), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.96f);
    if (ImGui::Begin("##starpick", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
        const bool ok = app.selectedIndex >= 0 && app.selectedIndex < static_cast<int>(app.entries.size()) && !app.entries[app.selectedIndex].metadata.isVideo;
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ui::kAccentHover, "Who goes in the video?");
        ImGui::SameLine();
        if (app.currentFolder.empty()) ImGui::TextUnformatted("Open a folder of pictures first.");
        else if (!ok) ImGui::TextUnformatted("Click a picture.");
        else ImGui::TextUnformatted(Utf8(fs::path(app.entries[app.selectedIndex].path).filename().wstring()).c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(!ok);
        if (ui::PrimaryButton("Use this picture")) StarInVideo(app, app.entries[app.selectedIndex].path);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) g_starPick = false, app.requestTab = 10;
    }
    ImGui::End();
}

void StarInVideo(AppState& app, const std::wstring& who) {
    VideoOptions();
    g_starPick = false;
    if (std::find(g_star.who.begin(), g_star.who.end(), who) == g_star.who.end()) g_star.who.insert(g_star.who.begin(), who);
    if (g_star.who.size() > 4) g_star.who.resize(4);
    g_mode = 1;
    app.requestTab = 10;
}

void OpenVideoPage(AppState& app, const std::wstring& first, const std::string& picturePrompt, const std::string& videoPrompt) {
    VideoOptions();
    g_v.first = first;
    g_v.opt.last.clear();
    g_v.context = picturePrompt; // what the picture shows: for the writer, never sent as it is
    g_v.idea.clear();
    g_v.prompt = videoPrompt;
    g_v.writtenKey.clear();
    g_v.edited = !videoPrompt.empty(); // kept, not written again
    app.requestTab = 10;
}

// ---- A character's 360-degree view (Characters tab): Pablo Dawson's 360 Orbit LoRA for H3 FL2VA (huggingface.co/pablodawson/MiniMax-H3-360-
// Orbit-LoRA, MiniMax H3 community licence, trained on orbits around human Gaussian splats). The same picture as the first AND the last frame:
// the camera goes once around the frozen character and lands back on the picture, so the clip loops, and orbit_view.cpp turns it like a
// turntable. This is the cheap half of "picture -> H3 orbit -> 3D Gaussian splats -> rigged Gaussian VRM": no splat training, no rig.
// Tested 2026-10-18 on the RTX 3090 (768 x 768, 73 frames, its prompt word for word): with the Fast (Wuli 4-step) LoRA it took 50 s and
// turned Ava and a knight cleanly all the way round (his hammer stays on his back, her ponytail and boots too); Balanced (8 steps) was as
// good but scattered specks over the backdrop. The picture is made square by stretching its own left and right edges (a flat colour showed as
// lighter panels beside a studio gradient), never cropped.
namespace {
const char* const kOrbitLoraUrl = "https://huggingface.co/pablodawson/MiniMax-H3-360-Orbit-LoRA/resolve/main/minimax_h3_flf2v_lora_v1.safetensors";
const char* const kOrbitLoraSha = "14f13e3effaf3e729fdc0c97680344aa63f473be0c55963f963d718b3db2a4d4";
const char* const kOrbitPrompt =
    "One frozen instant. Only the camera moves. In a continuous 360 orbit. Preserve every person and object in exactly the same world position, "
    "orientation, shape and pose throughout the shot. Airborne objects remain suspended at the captured height and angle: no wobbling, shaking, "
    "spinning, drifting, falling or continued action. Keep faces, hands, clothing, liquids and the background motionless while retaining their "
    "natural appearance. Camera parallax is the only source of apparent movement. No cuts, zoom, morphing or added objects.";
FileDownload g_orbitDl;

struct PendingOrbit {
    int activity = 0;
    std::wstring image;
};
std::vector<PendingOrbit> g_pendingOrbits;

bool OrbitLoraHere() {
    if (g_orbitDl.running) return false;
    if (g_v.lorasKnown) return HasLora(kOrbitLora); // what ComfyUI lists (its extra model folders too)
    std::error_code ec;
    const std::wstring models = ComfyModelsDir();
    if (models.empty()) return false;
    for (const auto& e : fs::recursive_directory_iterator(fs::path(models) / L"loras", fs::directory_options::skip_permission_denied, ec)) { // (a model folder too)
        if (e.is_regular_file(ec) && LoraFileName(Utf8(e.path().filename().wstring())) == LoraFileName(kOrbitLora)) return true;
    }
    return false;
}

// The picture made square (its edge columns or rows stretched outwards), saved for the upload; "" = could not read it
std::wstring SquareForOrbit(const std::wstring& image) {
    std::vector<unsigned char> rgba;
    int w = 0, h = 0;
    std::string err;
    if (!DecodeImageRGBA(image, rgba, w, h, &err) || w < 16 || h < 16) return {};
    const int side = std::max(w, h), x0 = (side - w) / 2, y0 = (side - h) / 2;
    std::vector<unsigned char> sq(static_cast<size_t>(side) * side * 4);
    for (int y = 0; y < side; ++y) {
        const int sy = std::clamp(y - y0, 0, h - 1);
        for (int x = 0; x < side; ++x) {
            const int sx = std::clamp(x - x0, 0, w - 1);
            std::memcpy(&sq[(static_cast<size_t>(y) * side + x) * 4], &rgba[(static_cast<size_t>(sy) * w + sx) * 4], 4);
        }
    }
    std::error_code ec;
    const fs::path dir = fs::path(DataDir()) / L"orbit_in";
    fs::create_directories(dir, ec);
    const std::wstring out = (dir / (std::to_wstring(std::hash<std::wstring>{}(image)) + L".png")).wstring();
    return WriteThumbnailPng(out, sq.data(), side, side) ? out : std::wstring();
}

json BuildOrbit() {
    json wf;
    auto node = [&](const char* id, const char* cls, json inputs) { wf[id] = {{"class_type", cls}, {"inputs", std::move(inputs)}, {"_meta", {{"title", cls}}}}; };
    // the quickest step LoRA there is: Fast (4 steps), else Balanced (8), else the model alone at the LoRA's own 28 steps
    const bool fast = HasLora(kFastLora), bal = !fast && HasLora(kBalancedLora);
    node("1", "UNETLoader", {{"unet_name", kUnet}, {"weight_dtype", "default"}});
    json model = json::array({"1", 0});
    if (fast || bal) {
        node("2", "LoraLoaderModelOnly", {{"model", model}, {"lora_name", fast ? kFastLora : kBalancedLora}, {"strength_model", 1.0}});
        model = json::array({"2", 0});
    }
    node("20", "LoraLoaderModelOnly", {{"model", model}, {"lora_name", kOrbitLora}, {"strength_model", 1.0}});
    node("3", "MiniMaxH3SigmaShift", {{"model", json::array({"20", 0})}, {"shift_video", fast ? 6.0 : 12.0}, {"shift_audio", 3.0}});
    model = json::array({"3", 0});
    if (VedaFor(kVedaPredictor)) {
        node("21", "VedaSparseAttention", VedaNode(model, kVedaPredictor));
        model = json::array({"21", 0});
    }
    node("4", "CLIPLoader", {{"clip_name", kTextEncoder}, {"type", "minimax"}, {"device", "default"}});
    node("5", "VAELoader", {{"vae_name", kVideoVae}});
    node("7", "LoadImage", {{"image", "@square"}});
    node("9", "ImageScale", {{"image", json::array({"7", 0})}, {"upscale_method", "lanczos"}, {"width", 768}, {"height", 768}, {"crop", "disabled"}});
    node("8", "MiniMaxH3ImageToVideo", {{"clip", json::array({"4", 0})}, {"vae", json::array({"5", 0})}, {"prompt", kOrbitPrompt}, {"width", 768}, {"height", 768},
                                        {"length", 73}, {"first_frame", json::array({"9", 0})}, {"last_frame", json::array({"9", 0})}});
    node("11", "RandomNoise", {{"noise_seed", 42}});
    node("12", "KSamplerSelect", {{"sampler_name", "euler"}});
    node("13", "BasicScheduler", {{"model", model}, {"scheduler", "simple"}, {"steps", fast ? 4 : bal ? 8 : 28}, {"denoise", 1.0}});
    node("14", "BasicGuider", {{"model", model}, {"conditioning", json::array({"8", 0})}});
    node("15", "SamplerCustomAdvanced", {{"noise", json::array({"11", 0})}, {"guider", json::array({"14", 0})}, {"sampler", json::array({"12", 0})},
                                         {"sigmas", json::array({"13", 0})}, {"latent_image", json::array({"8", 1})}});
    node("16", "VAEDecode", {{"samples", json::array({"15", 0})}, {"vae", json::array({"5", 0})}});
    node("18", "CreateVideo", {{"images", json::array({"16", 0})}, {"fps", 24.0}}); // (no sound: a frozen moment)
    node("19", "SaveVideo", {{"video", json::array({"18", 0})}, {"filename_prefix", "Characters/orbit"}, {"format", "auto"}, {"format.codec", "auto"}});
    return wf;
}
} // namespace

bool CharacterOrbitPending(const std::wstring& image) {
    for (const PendingOrbit& p : g_pendingOrbits) {
        if (_wcsicmp(p.image.c_str(), image.c_str()) == 0) return true;
    }
    return false;
}

void MakeCharacterOrbit(AppState& app, const std::wstring& image) {
    if (image.empty() || CharacterOrbitPending(image)) return;
    if (!OrbitLoraHere()) {
        Toast(ToastKind::Error, "The 360-degree view needs its LoRA", "Press \"Download the 360 LoRA\" first (155 MB)", {}, {}, "orbit");
        return;
    }
    const std::wstring square = SquareForOrbit(image);
    if (square.empty()) {
        Toast(ToastKind::Error, "Could not read the picture", Utf8(image), {}, {}, "orbit");
        return;
    }
    ComfyJob job;
    job.apiWorkflow = BuildOrbit().dump();
    job.rawUploads = {{"@square", square}};
    const CharacterProfile* cp = FindProfile(image);
    const std::string name = cp && !cp->name.empty() ? cp->name : Utf8(fs::path(image).stem().wstring());
    job.label = "360 view: " + name;
    job.times = 1;
    const int id = SendComfyJob(app, std::move(job));
    if (!id) return;
    g_pendingOrbits.push_back({id, image});
    Toast(ToastKind::Info, "Making a 360-degree view", name + ": the camera goes once around them (H3, about a minute)", {}, [](AppState& s) { s.requestTab = 5; }, "orbit");
}

// The download button, while the LoRA is missing; true when it is there
bool DrawOrbitSetup(AppState& app) {
    RefreshLoraList(app);
    static bool wasRunning = false;
    if (wasRunning && !g_orbitDl.running) g_v.lorasAsked = -1000.0, g_v.diskKnown = false; // downloaded: look again
    wasRunning = g_orbitDl.running;
    if (OrbitLoraHere()) return true;
    if (!g_v.lorasKnown && !g_v.diskKnown) {
        ImGui::TextDisabled("Looking for the 360 LoRA...");
        return false;
    }
    if (g_orbitDl.running) {
        ImGui::TextDisabled("Downloading the 360 Orbit LoRA (155 MB)...");
        return false;
    }
    if (ImGui::Button("Download the 360 LoRA (155 MB)")) StartDownload(g_orbitDl, kOrbitLoraUrl, kOrbitLora, L"loras", 0.1, kOrbitLoraSha);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("Pablo Dawson's 360 Orbit LoRA for MiniMax H3 (huggingface.co/pablodawson/MiniMax-H3-360-Orbit-LoRA) into ComfyUI's loras "
                          "folder. It uses the H3 model the Video page already has.");
    if (g_orbitDl.failed) {
        ImGui::SameLine();
        ImGui::TextColored(ui::kWarn, "the last try failed");
    }
    return false;
}

void PumpCharacterOrbits(AppState& app) {
    if (g_pendingOrbits.empty() || !app.comfySvc) return;
    const std::vector<ComfyActivity> acts = app.comfySvc->Activity();
    for (size_t i = 0; i < g_pendingOrbits.size();) {
        const PendingOrbit po = g_pendingOrbits[i];
        const ComfyActivity* a = nullptr;
        for (const ComfyActivity& x : acts) {
            if (x.id == po.activity) a = &x;
        }
        if (a && a->state != ComfyState::Done && a->state != ComfyState::Failed) {
            ++i;
            continue;
        }
        g_pendingOrbits.erase(g_pendingOrbits.begin() + i);
        std::wstring file;
        if (a && a->state == ComfyState::Done) {
            for (const ComfyResult& r : a->files) {
                if (!r.localPath.empty() && r.video) file = r.localPath;
            }
        }
        CharacterProfile* p = FindProfile(po.image);
        if (file.empty() || !p) {
            Toast(ToastKind::Error, "Could not make the 360-degree view", a ? a->message : std::string("ComfyUI lost the job"), {}, [](AppState& s) { s.requestTab = 4; }, "orbit");
            continue;
        }
        p->orbit = file;
        SaveProfiles();
        Toast(ToastKind::Success, "360-degree view ready: " + p->name, "Drag the picture in the Characters tab to turn them around", {}, [](AppState& s) { s.requestTab = 5; }, "orbit");
    }
}

std::string VideoTabLabel(AppState& app) {
    int running = 0;
    if (app.comfySvc) {
        for (const ComfyActivity& a : app.comfySvc->Activity()) {
            if (a.label.rfind("Video (", 0) == 0 && a.state != ComfyState::Done && a.state != ComfyState::Failed) ++running;
        }
    }
    running += static_cast<int>(g_writeQueue.size()) + (g_writeFut.valid() ? 1 : 0);
    return running ? "Video (" + std::to_string(running) + ")" : std::string("Video");
}

void VideoPagePressMake() { g_v.pressMake = true; }

bool VideoPageAnimating(AppState& app) {
    return app.activeTab == 10 && ((g_vid.player && g_vid.player->Playing()) || g_writeFut.valid() || ImGui::GetFrameCount() - g_presetCardFrame < 3);
}

void PumpVideo(AppState& app) {
    { // tests: MV_SPLIT_TEST=file - the long plan in it cut into parts (MV_SPLIT_SECONDS, default 30) -> file.split.txt, and its Continuum mode
        static bool done = false;
        wchar_t f[MAX_PATH] = {};
        if (!done && GetEnvironmentVariableW(L"MV_SPLIT_TEST", f, MAX_PATH)) {
            done = true;
            std::ifstream in(f, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            wchar_t sec[16] = {};
            const double seconds = GetEnvironmentVariableW(L"MV_SPLIT_SECONDS", sec, 16) ? _wtof(sec) : 30.0;
            int chunks = 2;
            double chunkSeconds = 8.0;
            LongPlan(seconds, chunks, chunkSeconds);
            const std::string parts = SplitLongPlan(ss.str(), chunks, chunkSeconds, false);
            std::ofstream(std::wstring(f) + L".split.txt", std::ios::binary) << parts << "\n=== mode: " << ContinuumPromptMode(parts) << "\n";
        }
    }
    // Veda: does ComfyUI have it? Asked every two minutes, so every H3 button gets it, not just this page
    if (g_v.vedaFut.valid() && g_v.vedaFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const ComfyVeda v = g_v.vedaFut.get();
        if (v.reachable) {
            g_v.vedaInfo = v;
            g_v.vedaKnown = true;
            const bool ready = VedaReady(kVedaPredictor);
            if (ready != g_v.vedaWasReady) g_v.vedaWasReady = ready, SaveSettings();
            if (v.continuum != g_v.continuumWas) g_v.continuumWas = v.continuum, SaveSettings();
        }
    }
    // a video Veda stopped (its kernel cannot start: triton gone after a ComfyUI update, an unsupported GPU...): Veda is turned off, so the next one works
    if (app.comfySvc && g_v.veda) {
        static std::set<int> seen;
        for (const ComfyActivity& a : app.comfySvc->Activity()) {
            if (a.state != ComfyState::Failed || a.label.find(", Veda)") == std::string::npos || a.message.find("Veda") == std::string::npos || !seen.insert(a.id).second) continue;
            g_v.veda = false;
            SaveSettings();
            std::string why = a.message.substr(0, a.message.find('\n'));
            if (why.size() > 300) why = why.substr(0, 300) + "...";
            Toast(ToastKind::Error, "Veda stopped the video, so it is turned off", why + "\nMake it again: it runs without Veda. Turn it back on under Quality on the Video page.", {},
                  [](AppState& s) { s.requestTab = 10; }, "vedaoff");
            break;
        }
    }
    { // tests: MV_OVERLAY_TEST "<video>|<video>...": OverlayRise of each into %LOCALAPPDATA%\Try2Create\overlay_test.txt
        static bool done = false;
        wchar_t buf[4096] = {};
        if (!done && GetEnvironmentVariableW(L"MV_OVERLAY_TEST", buf, 4096)) {
            done = true;
            std::thread([all = std::wstring(buf)]() {
                std::ofstream out(DataDir() + L"\\overlay_test.txt", std::ios::binary);
                for (size_t a = 0; a < all.size();) {
                    const size_t b = std::min(all.find(L'|', a), all.size());
                    const std::wstring f = all.substr(a, b - a);
                    char sb[16] = {};
                    wchar_t likeClip[1024] = {};
                    if (GetEnvironmentVariableW(L"MV_LIKE_CLIP", likeClip, 1024)) { // tests: how much a swap is still its clip
                        out << Utf8(fs::path(f).filename().wstring()) << " likeness " << LikenessToClip(f, likeClip) << "\n";
                        out.flush();
                    } else if (GetEnvironmentVariableA("MV_SEAM_TEST", sb, sizeof(sb))) { // through the UI thread's swap, as a finished video goes
                        std::wstring tmp;
                        const int seams = RepairDecoderSeams(f, &tmp);
                        out << Utf8(fs::path(f).filename().wstring()) << " seams " << seams << "\n";
                        out.flush();
                        if (!tmp.empty()) {
                            std::lock_guard<std::mutex> lock(g_swapMutex);
                            g_swaps.push_back({tmp, f, json({{"decoder_seams_repaired", seams}}).dump()});
                        }
                    }
                    else out << Utf8(fs::path(f).filename().wstring()) << " " << OverlayRise(f) << "\n";
                    a = b + 1;
                }
            }).detach();
        }
    }
    // every H3 video the app made, once it is back: its decoder seams repaired (RepairDecoderSeams), and a Presenter take then checked for text
    // over the picture (OverlayRise); one at a time in the background
    if (app.comfySvc) {
        static std::set<int> handled;
        static std::mutex oneAtATime;
        for (const ComfyActivity& a : app.comfySvc->Activity()) {
            if (handled.count(a.id) || a.label.rfind("Video (", 0) != 0) continue;
            PresenterTake take;
            bool presenter = false;
            SwapTake swapTake;
            bool swapped = false;
            for (size_t i = 0; i < g_swapTakes.size(); ++i) {
                if (g_swapTakes[i].act != a.id) continue;
                if (a.state == ComfyState::Done || a.state == ComfyState::Failed) {
                    swapTake = g_swapTakes[i];
                    swapped = a.state == ComfyState::Done;
                    g_swapTakes.erase(g_swapTakes.begin() + i);
                }
                break;
            }
            for (size_t i = 0; i < g_presenterTakes.size(); ++i) {
                if (g_presenterTakes[i].act != a.id) continue;
                if (a.state == ComfyState::Done || a.state == ComfyState::Failed) {
                    take = g_presenterTakes[i];
                    presenter = true;
                    g_presenterTakes.erase(g_presenterTakes.begin() + i);
                }
                break;
            }
            if (a.state == ComfyState::Failed) handled.insert(a.id);
            if (a.state != ComfyState::Done) continue;
            handled.insert(a.id);
            std::vector<std::wstring> files;
            for (const ComfyResult& r : a.files) {
                if (r.video && !r.localPath.empty()) files.push_back(r.localPath);
            }
            if (files.empty()) continue;
            const bool facesBack = a.label.rfind("Video (Faces", 0) == 0;
            std::thread([take, presenter, files, swapTake, swapped, facesBack]() {
                std::lock_guard<std::mutex> one(oneAtATime);
                for (const std::wstring& f : files) {
                    std::wstring repairedTmp;
                    const int seams = RepairDecoderSeams(f, &repairedTmp);
                    json verdict = {{"decoder_seams_repaired", seams}};
                    bool again = false; // a second take is coming: no faces or crisp for this one
                    if (facesBack && g_v.crispAfterSwap) {
                        std::lock_guard<std::mutex> lock(g_crispQueueMutex);
                        g_crispQueue.push_back(f);
                    }
                    if (swapped && !swapTake.people.empty()) { // a masked swap: did anyone stay the original in some shot?
                        SwapTake t = swapTake;
                        int shot = ShotKeptOriginal(f, t);
                        verdict["shot_kept_original"] = shot;
                        if (shot < 0) {
                            int who = 0;
                            shot = ShotWrongPerson(f, t, who);
                            verdict["shot_wrong_person"] = shot;
                            if (shot >= 0) verdict["wrong_subject"] = who + 1;
                        }
                        if (shot >= 0) {
                            char span[48];
                            snprintf(span, sizeof(span), "%.1f-%.1f s", t.shots[shot] / 24.0, t.shots[shot + 1] / 24.0);
                            t.keptNote = span;
                            again = t.retry;
                            std::lock_guard<std::mutex> lock(g_swapFoundMutex);
                            g_swapUnchanged.push_back({t, 0.0});
                        }
                    }
                    if (swapped && !swapTake.clip.empty() && swapTake.people.empty()) {
                        const double like = LikenessToClip(f, swapTake.clip);
                        verdict["likeness_to_clip_db"] = like;
                        verdict["person_unchanged"] = like > kSwapUnchanged;
                        if (like > kSwapUnchanged) {
                            again = swapTake.retry;
                            std::lock_guard<std::mutex> lock(g_swapFoundMutex);
                            g_swapUnchanged.push_back({swapTake, like});
                        }
                    }
                    if (swapped && !again) { // the take that is kept: the next person's pass, or its faces, then (on the faces video) crisp
                        std::lock_guard<std::mutex> lock(g_crispQueueMutex);
                        const bool chain = swapTake.pass >= 0;
                        const std::wstring orig = chain ? swapTake.chainJob.origClip : swapTake.clip;
                        const int everyone = static_cast<int>(chain ? swapTake.chainJob.who.size() : swapTake.job.who.size());
                        const int longId = chain ? swapTake.chainJob.longId : swapTake.job.longId, longPart = chain ? swapTake.chainJob.longPart : swapTake.job.longPart;
                        if (chain && swapTake.pass + 1 < everyone) {
                            g_nextPass.push_back({swapTake, f});
                        } else if (longId > 0) { // a part of a long swap: kept for the join
                            std::lock_guard<std::mutex> lock2(g_longMutex);
                            for (LongSwap& ls : g_longSwaps)
                                if (ls.id == longId && longPart >= 0 && longPart < static_cast<int>(ls.files.size())) ls.files[longPart] = f;
                        } else if (g_v.facesAfterSwap && FacesReady() && !orig.empty())
                            g_facesQueue.push_back({f, orig, everyone});
                        else if (g_v.crispAfterSwap)
                            g_crispQueue.push_back(f);
                    }
                    if (presenter) {
                        const double rise = OverlayRise(repairedTmp.empty() ? f : repairedTmp);
                        verdict["overlay_rise"] = rise;
                        verdict["text_over_picture"] = rise > kOverlayRise;
                        if (rise > kOverlayRise) {
                            std::lock_guard<std::mutex> lock(g_overlayMutex);
                            g_overlayFound.push_back({take, Utf8(fs::path(f).filename().wstring())});
                        }
                    }
                    if (repairedTmp.empty()) {
                        std::ofstream(f + L".take.json", std::ios::binary) << verdict.dump();
                    } else { // put in place on the UI thread: the video is often open in a player by now (you watch it as it arrives)
                        std::lock_guard<std::mutex> lock(g_swapMutex);
                        g_swaps.push_back({repairedTmp, f, verdict.dump()});
                    }
                }
            }).detach();
        }
    }
    { // repaired videos into place. A player that has one open lets go of it for the moment of the swap and carries on where it was;
      // before, the swap waited 10 s for a playing video and gave up, and the flashes stayed ("decoder_seams_repaired": -1).
        static double lastTry = 0;
        std::lock_guard<std::mutex> lock(g_swapMutex);
        if (!g_swaps.empty() && ImGui::GetTime() - lastTry > 0.25) {
            lastTry = ImGui::GetTime();
            for (size_t i = 0; i < g_swaps.size();) {
                Swap& w = g_swaps[i];
                std::error_code ec;
                fs::rename(w.tmp, w.video, ec);
                if (ec && ++w.tries < 480) { // a player let go (Windows closes the file a moment later), or another program has it: two minutes
                    if (w.tries < 20) VideoPlayer::LetGo(w.video);
                    else if (w.tries == 20) VideoPlayer::TakeBack(w.video); // not our players after 5 s: they carry on meanwhile
                    ++i;
                    continue;
                }
                VideoPlayer::TakeBack(w.video); // the players that let go carry on, with the repaired file
                if (ec) {
                    fs::remove(w.tmp, ec);
                    json v = json::parse(w.verdict, nullptr, false);
                    if (v.is_object()) v["decoder_seams_repaired"] = -1, w.verdict = v.dump();
                }
                std::ofstream(w.video + L".take.json", std::ios::binary) << w.verdict;
                g_swaps.erase(g_swaps.begin() + i);
            }
        }
    }
    PumpCrisp(app); // (Make it crisp: prepared runs sent, long ones joined)
    { // finished swaps made crisp, once their repaired file is in place (none of them waiting to be swapped in)
        std::lock_guard<std::mutex> lock(g_crispQueueMutex);
        for (size_t i = 0; i < g_crispQueue.size();) {
            bool waiting = false;
            {
                std::lock_guard<std::mutex> l2(g_swapMutex);
                for (const Swap& w : g_swaps) waiting = waiting || _wcsicmp(w.video.c_str(), g_crispQueue[i].c_str()) == 0;
            }
            if (waiting) {
                ++i;
                continue;
            }
            if (CrispReady()) MakeCrisp(app, g_crispQueue[i]);
            g_crispQueue.erase(g_crispQueue.begin() + i);
        }
        for (size_t i = 0; i < g_facesQueue.size();) { // the same for the faces
            bool waiting = false;
            {
                std::lock_guard<std::mutex> l2(g_swapMutex);
                for (const Swap& w : g_swaps) waiting = waiting || _wcsicmp(w.video.c_str(), g_facesQueue[i].video.c_str()) == 0;
            }
            if (waiting) {
                ++i;
                continue;
            }
            MakeFaces(app, g_facesQueue[i]);
            g_facesQueue.erase(g_facesQueue.begin() + i);
        }
        for (size_t i = 0; i < g_nextPass.size();) { // one person at a time: the next person's pass, on the kept video
            bool waiting = false;
            {
                std::lock_guard<std::mutex> l2(g_swapMutex);
                for (const Swap& w : g_swaps) waiting = waiting || _wcsicmp(w.video.c_str(), g_nextPass[i].second.c_str()) == 0;
            }
            if (waiting) {
                ++i;
                continue;
            }
            const SwapTake t = g_nextPass[i].first;
            const std::wstring video = g_nextPass[i].second;
            g_nextPass.erase(g_nextPass.begin() + i);
            WriteJob jj = t.chainJob;
            jj.pass = t.pass + 1;
            jj.emphasis.clear();
            SendStar(app, jj, t.chainPrompt, video, t.chainPeople.get());
        }
    }
    { // long swaps: joined once every part is kept (a worker thread), then the faces pass or crisp on the whole
        std::lock_guard<std::mutex> lock(g_longMutex);
        for (LongSwap& ls : g_longSwaps) {
            if (ls.joining || std::any_of(ls.files.begin(), ls.files.end(), [](const std::wstring& f) { return f.empty(); })) continue;
            ls.joining = true;
            std::thread([ls]() {
                LongDone d;
                d.label = ls.label, d.people = ls.people;
                const std::wstring ff = FfmpegPath();
                double total = 0;
                for (double l : ls.lens) total += l;
                std::error_code ec;
                const fs::path first = ls.files.front();
                const std::wstring out = (first.parent_path() / (L"H3_Swap_Long_" + first.stem().wstring() + L".mp4")).wstring();
                std::wstring cmd = L"\"" + ff + L"\" -y -v error";
                for (const std::wstring& f : ls.files) cmd += L" -i \"" + f + L"\"";
                char span[96];
                snprintf(span, sizeof(span), " -ss %.3f -t %.3f", ls.start, total);
                if (ls.keepSound) cmd += std::wstring(span, span + strlen(span)) + L" -i \"" + ls.src + L"\"";
                std::string fc, vl, al;
                for (size_t i = 0; i < ls.files.size(); ++i) {
                    char b[200];
                    snprintf(b, sizeof(b), "[%d:v]trim=end_frame=%d,setpts=PTS-STARTPTS,fps=24[v%d];", static_cast<int>(i), static_cast<int>(std::lround(ls.lens[i] * 24.0)), static_cast<int>(i));
                    fc += b;
                    vl += "[v" + std::to_string(i) + "]";
                    if (!ls.keepSound) {
                        snprintf(b, sizeof(b), "[%d:a]atrim=end=%.3f,asetpts=PTS-STARTPTS[a%d];", static_cast<int>(i), ls.lens[i], static_cast<int>(i));
                        fc += b;
                        al += "[a" + std::to_string(i) + "]";
                    }
                }
                fc += vl + "concat=n=" + std::to_string(ls.files.size()) + ":v=1:a=0[v]";
                if (!ls.keepSound) fc += ";" + al + "concat=n=" + std::to_string(ls.files.size()) + ":v=0:a=1[a]";
                cmd += L" -filter_complex \"" + WideOf(fc) + L"\" -map \"[v]\" -map " + (ls.keepSound ? L"\"" + std::to_wstring(ls.files.size()) + L":a:0?\"" : std::wstring(L"\"[a]\"")) +
                       L" -c:v libx264 -preset medium -crf 16 -pix_fmt yuv420p -c:a aac -b:a 192k -shortest \"" + out + L"\"";
                std::string log;
                RunCaptureHidden(cmd, log, true);
                if (!fs::exists(out, ec) || fs::file_size(out, ec) < 1000) {
                    d.error = log.empty() ? std::string("ffmpeg could not join the parts") : log.substr(0, 300);
                } else {
                    d.video = out;
                    // the clip's part as one piece (the faces pass follows the original's expressions over the whole length)
                    const std::wstring orig = DataDir() + L"\\h3\\clips\\long_" + HashName(FileKey(ls.src) + span) + L".mp4";
                    if (!fs::exists(orig, ec))
                        RunCaptureHidden(L"\"" + ff + L"\" -y -v error" + std::wstring(span, span + strlen(span)) + L" -i \"" + ls.src + L"\" -vf fps=24 -c:v libx264 -preset veryfast -crf 18 -pix_fmt yuv420p -c:a aac \"" + orig + L"\"", log, true);
                    if (fs::exists(orig, ec)) d.orig = orig;
                }
                std::lock_guard<std::mutex> lock(g_longMutex);
                g_longDone.push_back(d);
            }).detach();
        }
        for (const LongDone& d : g_longDone) {
            if (!d.error.empty()) {
                Toast(ToastKind::Error, "Could not join the long swap", d.label + ": " + d.error, {}, [](AppState& s) { s.requestTab = 10; }, "swap_long");
                continue;
            }
            Toast(ToastKind::Info, "The long swap is ready", d.label + ": its parts joined into " + Utf8(fs::path(d.video).filename().wstring()), {}, [](AppState& s) { s.requestTab = 10; }, "swap_long");
            std::lock_guard<std::mutex> lock2(g_crispQueueMutex);
            if (g_v.facesAfterSwap && FacesReady() && !d.orig.empty()) g_facesQueue.push_back({d.video, d.orig, d.people});
            else if (g_v.crispAfterSwap) g_crispQueue.push_back(d.video);
        }
        g_longDone.clear();
        g_longSwaps.erase(std::remove_if(g_longSwaps.begin(), g_longSwaps.end(), [](const LongSwap& ls) { return ls.joining; }), g_longSwaps.end());
    }
    { // a swap that came back as the clip itself: made again once, the new person's look said first and louder
        std::vector<std::pair<SwapTake, double>> found;
        {
            std::lock_guard<std::mutex> lock(g_swapFoundMutex);
            found.swap(g_swapUnchanged);
        }
        for (const auto& [t, like] : found) {
            if (t.retry && t.pass >= 0 && t.chainPeople) { // one person at a time: the same pass again, the same person said louder
                WriteJob jj = t.chainJob;
                jj.pass = t.pass;
                jj.label = t.chainJob.label + " (again)";
                jj.emphasis = t.keptNote.empty() ? "<Subject 1> REPLACES the performer completely: a different person with a different face, hair, body and "
                                                   "clothes, exactly as in <Picture 1>; the original performer must not remain visible."
                                                 : "<Subject 1> REPLACES the performer completely in every shot, also from " + t.keptNote +
                                                       ": the original face, hair and clothes must not remain, also seen from behind.";
                g_swapRetrying = true;
                SendStar(app, jj, t.chainPrompt, t.clip, t.chainPeople.get());
                g_swapRetrying = false;
            } else if (t.retry) {
                WriteJob j;
                j.kind = 1;
                j.starMethod = 0;
                j.who = t.job.who;
                j.refVideo = t.job.refVideo;
                j.refStart = t.job.refStart;
                j.keepSound = t.job.keepSound;
                j.idea = t.job.idea;
                j.label = t.job.label + " (again)";
                j.opt = t.job.opt;
                j.opt.times = 1;
                j.starQuality = t.job.starQuality;
                j.scan = t.job.scan, j.picks = t.job.picks; // (the same people, picked on the page)
                j.longId = t.job.longId, j.longPart = t.job.longPart;
                std::string again = t.prompt;
                const size_t at = again.find("subject_definitions:\n");
                if (at != std::string::npos)
                    again.insert(at + strlen("subject_definitions:\n"),
                                 t.keptNote.empty() ? "<Subject 1> REPLACES the performer completely: a different person with a different face, hair, "
                                                      "body and clothes, exactly as in <Picture 1>; the original performer must not remain visible.\n"
                                                    : "In every shot each new person REPLACES the original performer completely, also from " + t.keptNote +
                                                          ": the original faces, hair and clothes must not remain anywhere, also on someone seen from behind.\n");
                g_swapRetrying = true;
                SendStar(app, j, again, t.clip);
                g_swapRetrying = false;
            }
            if (!t.keptNote.empty())
                Toast(ToastKind::Info, "Someone was not swapped", t.job.label + ": at " + t.keptNote + (t.retry ? " someone stayed as in the clip, or did not look like their picture. Making it again." : " again in the second take. Try another seed, or another picture."),
                      {}, [](AppState& s) { s.requestTab = 10; }, "swap_unchanged");
            else
                Toast(ToastKind::Info, "The person did not change", t.job.label + (t.retry ? ": the video came back as the clip itself. Making it again, the new look said louder."
                                                                                   : ": the second take too. Try another picture (clear, full-body), or write the look into the prompt."),
                      {}, [](AppState& s) { s.requestTab = 10; }, "swap_unchanged");
        }
    }
    {
        std::vector<std::pair<PresenterTake, std::string>> found;
        {
            std::lock_guard<std::mutex> lock(g_overlayMutex);
            found.swap(g_overlayFound);
        }
        for (const auto& [t, name] : found) {
            if (t.retry) {
                H3Options o = t.opt;
                o.times = 1;
                g_presenterRetrying = true;
                SendH3(app, t.first, t.prompt, o, t.label + " (again: text came over the picture)");
                g_presenterRetrying = false;
            }
            Toast(ToastKind::Info, "Text came over the picture", name + (t.retry ? ": Presenter's habit from its livestream training. Making it again with a new seed."
                                                                          : ": the second take has it too. Try other words (talk naturally, no lists), or 5 seconds."),
                  {}, [](AppState& s) { s.requestTab = 10; }, "presenter_overlay");
        }
    }
    if (!g_v.vedaFut.valid() && ImGui::GetTime() - g_v.vedaAsked > (g_v.vedaKnown ? 60.0 : 5.0)) { // every 5 s until ComfyUI answers
        if (!app.comfyLoaded) app.comfyLoaded = true, LoadComfySettings(app.comfy); // its address
        g_v.vedaAsked = ImGui::GetTime();
        g_v.vedaFut = std::async(std::launch::async, ComfyAskVeda, app.comfy.url);
    }
    // the prompt writer (and, for a star video, the clip cutting), one job at a time
    if (!g_writeFut.valid() && !g_writeQueue.empty()) {
        g_writing = std::move(g_writeQueue.front());
        g_writeQueue.pop_front();
        if (g_writing.kind == 1 || g_writing.write)
            Toast(ToastKind::Info, g_writing.kind == 1 ? "Preparing the clip and the prompt" : "Writing the video prompt",
                  g_writing.idea.empty() ? std::string(g_writing.kind == 1 ? "Cutting the clip; the prompt writer looks at who stars in it" : "The prompt writer looks at the picture: action, camera, sound")
                                         : g_writing.idea,
                  {}, [](AppState& s) { s.requestTab = 10; }, "videowrite");
        g_writeFut = std::async(std::launch::async, RunWriteJob, g_writing, app.comfy.url);
    }
    if (g_writeFut.valid() && g_writeFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const WriteResult r = g_writeFut.get();
        if (!r.note.empty())
            Toast(ToastKind::Error, g_writing.kind == 1 ? "Note on the star video" : "Used a simple video prompt",
                  "Because " + r.note + ". You can edit the prompt on the Video page.", {}, [](AppState& s) { s.requestTab = 10; }, "videowrite");
        if (g_writing.toPage && g_writing.write) {
            if (g_writing.kind == 1) {
                g_star.prompt = r.text;
                g_star.writtenKey = r.note.empty() ? g_writing.key : std::string(); // (a stand-in prompt is written again next time)
                g_star.edited = false;
            } else {
                g_v.prompt = r.text;
                g_v.writtenKey = g_writing.key;
                g_v.edited = false;
            }
        }
        if (g_writing.sendAfter) {
            if (g_writing.kind == 1 && !r.who.empty()) g_writing.who = r.who; // (each person cut out onto white)
            if (g_writing.kind == 1) SendStar(app, g_writing, r.text, r.clip);
            else if (SendH3(app, g_writing.first, r.text, g_writing.opt, g_writing.label) && g_writing.toPage) NotePresetSend();
        }
    }
    { // tests: MV_TEST_SHOTCHECK="take|clip|people|w|h|0,30,54,..." -> ShotKeptOriginal's answer in shotcheck.txt
        static bool once = false;
        wchar_t buf[2048] = {};
        if (!once && GetEnvironmentVariableW(L"MV_TEST_SHOTCHECK", buf, 2048) > 0) {
            once = true;
            std::vector<std::wstring> parts;
            std::wstringstream in(buf);
            for (std::wstring p; std::getline(in, p, L'|');) parts.push_back(p);
            if (parts.size() == 6) {
                SwapTake t;
                t.clip = parts[1];
                t.people = parts[2];
                t.peopleW = _wtoi(parts[3].c_str());
                t.peopleH = _wtoi(parts[4].c_str());
                std::wstringstream b(parts[5]);
                for (std::wstring v; std::getline(b, v, L',');) t.shots.push_back(_wtoi(v.c_str()));
                std::ofstream(DataDir() + L"\\shotcheck.txt") << ShotKeptOriginal(parts[0], t) << "\n";
            }
        }
    }
    // swaps waiting for ComfyUI: sent once it answers (or after 3 minutes without an answer, the old way)
    if (!g_comfyWait.empty() && (g_v.vedaKnown || ImGui::GetTime() - g_comfyWaitSince > 180.0)) {
        std::deque<PeopleWait> ready;
        ready.swap(g_comfyWait);
        const bool was = g_swapRetrying;
        for (const PeopleWait& p : ready) {
            g_swapRetrying = p.retrying;
            if (!g_v.vedaKnown) {
                const bool mask = g_v.swapMask;
                g_v.swapMask = false; // (no answer: the way that needs nothing extra)
                SendStar(app, p.job, p.prompt, p.clip);
                g_v.swapMask = mask;
            } else {
                SendStar(app, p.job, p.prompt, p.clip);
            }
        }
        g_swapRetrying = was;
    }
    // swaps waiting for their person masks, one at a time
    if (!g_maskFut.valid() && !g_peopleWait.empty() && !g_writeFut.valid()) {
        const PeopleWait& p = g_peopleWait.front();
        const VideoInfo vi = ProbeVideo(p.clip);
        H3Options o = p.job.opt;
        o.shape = 0;
        int w = 0, h = 0;
        H3Size(o, vi.width > 0 ? vi.width : 16, vi.height > 0 ? vi.height : 9, w, h);
        std::string text = p.prompt;
        g_maskFut = std::async(std::launch::async, BuildPeopleMasks, p.clip, FramesFor(p.job.opt.seconds), w, h, PeopleToFind(text, p.job.who.size()), app.comfy.url,
                                 g_v.vedaInfo.sam3, RoomFor(text, p.job.who.size()), p.job.scan, p.job.picks);
    }
    if (g_maskFut.valid() && g_maskFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const PeopleMask m = g_maskFut.get();
        SetWritePhase("");
        PeopleWait p = std::move(g_peopleWait.front());
        g_peopleWait.pop_front();
        if (m.ok) {
            char buf[160];
            snprintf(buf, sizeof(buf), "Found the people: %d shot%s, someone masked in %.0f%% of the frames", m.shots, m.shots == 1 ? "" : "s", m.coverage * 100.0);
            app.SetStatus(buf);
        }
        const bool was = g_swapRetrying;
        g_swapRetrying = p.retrying;
        SendStar(app, p.job, p.prompt, p.clip, &m);
        g_swapRetrying = was;
    }
    // the player stops when you leave the page
    if (app.activeTab != 10 && g_vid.player && g_vid.player->Playing()) g_vid.player->Pause();
}

// A bar to drag (vertical: between a left and a right side; else between a top and a bottom). True while it is dragged.
bool Splitter(const char* id, bool vertical, const ImVec2& size) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, size);
    const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    if (hov || act) ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
    const ImU32 col = act ? ImGui::GetColorU32(ui::kAccent) : hov ? ImGui::GetColorU32(ui::kAccentHover) : IM_COL32(255, 255, 255, 28);
    const float t = (hov || act) ? ui::Dp(2.0f) : ui::Dp(1.0f), m = ui::Dp(10.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (vertical) dl->AddLine(ImVec2(p.x + size.x * 0.5f, p.y + m), ImVec2(p.x + size.x * 0.5f, p.y + size.y - m), col, t);
    else dl->AddLine(ImVec2(p.x + m, p.y + size.y * 0.5f), ImVec2(p.x + size.x - m, p.y + size.y * 0.5f), col, t);
    return act && (ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f);
}

void DrawVideoTab(AppState& app, float height) {
    H3Options& o = VideoOptions();
    {
        static const std::string spec = [] {
            char buf[256] = {};
            return GetEnvironmentVariableA("MV_UI_TEST", buf, sizeof(buf)) ? std::string(buf) : std::string();
        }();
        auto num = [&](const char* key) -> int {
            const size_t at = spec.find(key);
            if (at == std::string::npos) return -1;
            const size_t c = at + strlen(key);
            return c < spec.size() && spec[c] == ':' ? atoi(spec.c_str() + c + 1) : 0;
        };
        static bool done = false;
        if (!done) {
            done = true;
            if (num("videomode") >= 0) g_mode = num("videomode");
        }
        static bool makeDone = false; // tests: MV_VIDEO_MAKE "<picture>|<quality 0-3>|<prompt>": made at the lowest resolution, tall, 5 s
        if (!makeDone && app.comfyLoaded) {
            wchar_t mk[4096] = {};
            if (GetEnvironmentVariableW(L"MV_VIDEO_MAKE", mk, 4096)) {
                makeDone = true;
                const std::wstring all = mk;
                const size_t a = all.find(L'|'), b = all.find(L'|', a + 1);
                if (a != std::wstring::npos && b != std::wstring::npos) {
                    H3Options o = g_v.opt;
                    o.quality = std::clamp(_wtoi(all.substr(a + 1, b - a - 1).c_str()), 0, kQualityCount - 1);
                    char rb[8] = {};
                    o.res = GetEnvironmentVariableA("MV_VIDEO_RES", rb, sizeof(rb)) ? std::clamp(atoi(rb), 0, 3) : 0;
                    o.shape = 2;
                    char sb[8] = {};
                    o.seconds = GetEnvironmentVariableA("MV_VIDEO_SECONDS", sb, sizeof(sb)) ? std::clamp(atof(sb), 3.0, kLongMax) : 5.0;
                    o.times = 1;
                    wchar_t lb[2048] = {};
                    o.last = GetEnvironmentVariableW(L"MV_VIDEO_LAST", lb, 2048) ? std::wstring(lb) : std::wstring(); // (and an end picture)
                    std::string prompt;
                    const std::wstring wp = all.substr(b + 1);
                    prompt.resize(WideCharToMultiByte(CP_UTF8, 0, wp.c_str(), static_cast<int>(wp.size()), nullptr, 0, nullptr, nullptr));
                    WideCharToMultiByte(CP_UTF8, 0, wp.c_str(), static_cast<int>(wp.size()), prompt.data(), static_cast<int>(prompt.size()), nullptr, nullptr);
                    if (prompt.rfind("idea:", 0) == 0) { // the writer writes it first (the page's own path), then it is made
                        g_testWrite = std::async(std::launch::async, [first = all.substr(0, a), idea = prompt.substr(5), o, url = app.comfy.url]() {
                            std::string note;
                            const std::string text = WriteVideoPromptFor(first, idea, std::string(), o, url, &note);
                            std::ofstream(DataDir() + L"\\test_written_prompt.txt", std::ios::binary) << text << "\n---\nnote: " << note;
                            return text;
                        });
                        g_testOpt = o;
                        g_testFirst = all.substr(0, a);
                    } else {
                        SendH3(app, all.substr(0, a), prompt, o, "Video (test)");
                    }
                }
            }
        }
        if (g_testWrite.valid() && g_testWrite.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            const std::string written = g_testWrite.get();
            if (GetEnvironmentVariableW(L"MV_VIDEO_WRITE_ONLY", nullptr, 0) == 0) SendH3(app, g_testFirst, written, g_testOpt, "Video (test)"); // (MV_VIDEO_WRITE_ONLY: only written)
        }
        static bool recreateDone = false; // tests: MV_RECREATE names a video to recreate when the page opens
        if (!recreateDone) {
            recreateDone = true;
            wchar_t buf[1024] = {};
            if (GetEnvironmentVariableW(L"MV_RECREATE", buf, 1024)) RecreateVideo(app, buf);
            if (GetEnvironmentVariableW(L"MV_CRISP", buf, 1024)) { // tests: Make it crisp on this video ("<video>|<short side>")
                std::wstring spec = buf;
                const size_t bar = spec.find(L'|');
                if (bar != std::wstring::npos) {
                    g_v.crispTo = _wtoi(spec.c_str() + bar + 1);
                    spec = spec.substr(0, bar);
                }
                MakeCrisp(app, spec);
            }
        }
        static bool starDone = false;
        const int sr = num("starrun");
        if (sr >= 0 && !starDone && !g_vid.clips.empty() && sr < static_cast<int>(app.entries.size())) {
            starDone = true;
            wchar_t buf2[8] = {};
            g_mode = 1;
            g_star.who = {app.entries[sr].path};
            g_star.video = g_vid.clips.front().path;
            { // tests: MV_STAR_WHO / MV_STAR_VIDEO name the picture and the clip
                wchar_t buf[1024] = {};
                if (GetEnvironmentVariableW(L"MV_STAR_WHO", buf, 1024)) { // several: "a.png|b.png"
                    g_star.who.clear();
                    std::wstring all = buf;
                    for (size_t a = 0; a <= all.size();) {
                        const size_t bar = std::min(all.find(L'|', a), all.size());
                        if (bar > a) g_star.who.push_back(all.substr(a, bar - a));
                        a = bar + 1;
                    }
                }
                if (GetEnvironmentVariableW(L"MV_STAR_VIDEO", buf, 1024)) g_star.video = buf;
            }
            g_star.start = 0;
            g_star.idea = "";
            {
                char ib[1024] = {};
                if (GetEnvironmentVariableA("MV_STAR_IDEA", ib, sizeof(ib))) g_star.idea = ib; // tests: the idea ("replace the girl with ava...")
            }
            o.res = 0;
            o.seconds = 3;
            if (GetEnvironmentVariableW(L"MV_STAR_RES", buf2, 8)) o.res = std::clamp(_wtoi(buf2), 0, 3); // tests: size, length, where in the clip
            if (GetEnvironmentVariableW(L"MV_STAR_SECONDS", buf2, 8)) o.seconds = std::clamp(_wtof(buf2), 3.0, 60.0);
            if (GetEnvironmentVariableW(L"MV_STAR_START", buf2, 8)) g_star.start = std::max(0.0, _wtof(buf2));
            if (GetEnvironmentVariableW(L"MV_STAR_QUALITY", buf2, 8)) g_star.quality = _wtoi(buf2) == 1 ? 1 : 0;
            if (GetEnvironmentVariableW(L"MV_DETAIL", buf2, 8)) g_v.detail = static_cast<float>(_wtof(buf2)); // tests: Fizgig's dials
            if (GetEnvironmentVariableW(L"MV_PSTRENGTH", buf2, 8)) g_v.promptStrength = static_cast<float>(_wtof(buf2));
            g_v.pressMake = !GetEnvironmentVariableW(L"MV_STAR_NOMAKE", buf2, 8); // MV_STAR_NOMAKE: only fill the form
            if (GetEnvironmentVariableW(L"MV_STAR_METHOD", buf2, 8)) g_starMethod = buf2[0] == L'1' ? 1 : 0;
        }
    }
    RefreshLoraList(app); // which LoRAs ComfyUI has: asked when the page opens, again after a minute
    RefreshClips(app); // the videos on disk

    DrawVideoTools(app, VideoSam3());
    if (VideoToolsMadeVideo()) g_vid.scanned = -1000; // (a tool made one: the list looks again)
    PumpPresetShots(app);
    const std::string idea = IdeaWithCamera(); // (your idea and the camera move: what the prompt is written for)
    const std::string key = PromptKey(g_v.first, o.last, idea, o.seconds);
    const bool writingHere = g_writeFut.valid() && g_writing.toPage;
    bool changed = false;
    ImGui::BeginChild("##video", ImVec2(0, height), false);
    const float avail = ImGui::GetContentRegionAvail().x;
    const float splitW = ui::Dp(6.0f);
    const float leftW = g_v.leftW > 0 ? std::clamp(ui::Dp(g_v.leftW), std::min(avail * 0.5f, ui::Dp(360.0f)), std::max(ui::Dp(360.0f), avail - ui::Dp(320.0f)))
                                      : std::clamp(avail * 0.5f, std::min(avail, ui::Dp(420.0f)), ui::Dp(720.0f));
    ImGui::BeginChild("##videoleft", ImVec2(leftW, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    const float footerH = ui::Dp(66.0f);
    ImGui::BeginChild("##videoform", ImVec2(0, -footerH), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushFont(ui::TitleFont());
    ImGui::TextUnformatted("Video with sound");
    ImGui::PopFont();
    ModeTabs(app);
    if (g_mode == 2) {
        DrawStoryMode(app, footerH);
    } else if (g_mode == 1) {
        DrawStarMode(app, footerH);
    } else {

        // pictures
        const float slot = ui::Dp(120.0f);
        PictureSlot(app, "first", "Start picture (first frame)", g_v.first, slot);
        ImGui::SameLine(0.0f, ui::Dp(18.0f));
        PictureSlot(app, "last", "End picture (optional)", o.last, slot);
        ImGui::Spacing();

        // what happens: your idea, and the video prompt written from it and the pictures
        ui::SectionHeading("YOUR IDEA (OPTIONAL)");
        WrapEdit("##videoidea", g_v.idea, ui::Dp(54.0f));
        if (g_v.idea.empty()) ImGui::TextDisabled("e.g. \"she spins around and laughs\". Empty: the writer picks a lively action that suits the picture.");
        // motion presets (Higgsfield's): a card fills the idea, its length and its camera move. Each card is a drawing of the move (preset_art.cpp;
        // it plays while hovered) - not the last video made with it, which showed the same face on every card; that one is on its right-click
        {
            static int hovered = -1;
            static double hoverAt = 0.0;
            const float cw = ui::Dp(104.0f), ch = ui::Dp(80.0f), gap = ui::Dp(6.0f);
            const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
            for (int k = 0; k < kPresetCount; ++k) {
                if (k && ImGui::GetItemRectMax().x + gap + cw <= right) ImGui::SameLine(0.0f, gap);
                ImGui::PushID(k);
                const Preset& pr = kPresets[k];
                const bool on = g_v.idea == pr.idea;
                const ImVec2 p = ImGui::GetCursorScreenPos();
                if (ImGui::InvisibleButton("##preset", ImVec2(cw, ch))) {
                    if (on) {
                        g_v.idea.clear();
                    } else {
                        g_v.idea = pr.idea;
                        if (pr.seconds > 0 && o.seconds <= 15.0) o.seconds = pr.seconds;
                        g_v.camera = pr.camera;
                        g_v.cameraByPreset = pr.camera != 0;
                    }
                    changed = true;
                }
                const bool hov = ImGui::IsItemHovered();
                if (hov && hovered != k) hovered = k, hoverAt = ImGui::GetTime();
                else if (!hov && hovered == k) hovered = -1;
                if (hov) g_presetCardFrame = ImGui::GetFrameCount();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 ts = ImGui::CalcTextSize(pr.name);
                const float strip = ts.y + ui::Dp(6.0f);
                DrawPresetArt(dl, k, p, cw, ch - strip * 0.55f, hov ? static_cast<float>(ImGui::GetTime() - hoverAt) : -1.0f);
                dl->AddRectFilled(ImVec2(p.x, p.y + ch - strip), ImVec2(p.x + cw, p.y + ch), IM_COL32(0, 0, 0, 175), ui::Dp(6.0f), ImDrawFlags_RoundCornersBottom);
                dl->PushClipRect(p, ImVec2(p.x + cw, p.y + ch), true);
                const float fit = std::min(1.0f, (cw - ui::Dp(10.0f)) / std::max(1.0f, ts.x)); // (a long name shrinks to fit)
                dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * fit, ImVec2(p.x + ui::Dp(6.0f), p.y + ch - ts.y * (1.0f + fit) * 0.5f - ui::Dp(3.0f)),
                            IM_COL32(255, 255, 255, 240), pr.name);
                dl->PopClipRect();
                auto shot = g_presetShots.find(pr.name);
                if (on) dl->AddRect(p, ImVec2(p.x + cw, p.y + ch), ImGui::GetColorU32(ui::kAccent), ui::Dp(6.0f), 0, ui::Dp(2.5f));
                else if (hov) dl->AddRect(p, ImVec2(p.x + cw, p.y + ch), ImGui::GetColorU32(ui::kAccentHover), ui::Dp(6.0f), 0, ui::Dp(1.5f));
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                    ImGui::SetTooltip("%s\n%s%s%s", pr.name, pr.idea, pr.seconds > 0 ? ("\n" + std::to_string(static_cast<int>(pr.seconds)) + " s").c_str() : "",
                                      pr.camera ? (std::string(", camera: ") + kCameras[pr.camera].name).c_str() : "");
                if (shot != g_presetShots.end() && ImGui::BeginPopupContextItem("##presetctx")) {
                    if (ImGui::MenuItem("Play the last one made with it")) PlayInVideoPage(app, shot->second);
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
        }
        // the camera move (a preset's goes back to Auto once the idea is not that preset's any more)
        if (g_v.cameraByPreset && std::none_of(std::begin(kPresets), std::end(kPresets), [](const Preset& pr) { return g_v.idea == pr.idea; }))
            g_v.camera = 0, g_v.cameraByPreset = false, changed = true;
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Camera:");
        for (int c = 0; c < kCameraCount; ++c) {
            ImGui::SameLine(0.0f, ui::Dp(4.0f));
            if (ImGui::GetCursorPosX() + ImGui::CalcTextSize(kCameras[c].name).x + ui::Dp(20.0f) > ImGui::GetContentRegionMax().x) ImGui::NewLine();
            ImGui::PushStyleColor(ImGuiCol_Button, g_v.camera == c ? ui::ChipActiveColor() : ui::ChipColor());
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::ChipHoverColor());
            if (ImGui::SmallButton(kCameras[c].name)) g_v.camera = c, g_v.cameraByPreset = false, changed = true;
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", kCameras[c].text ? kCameras[c].text : "The writer picks the camera that suits the action");
        }
        if (ImGui::Checkbox("Write the video prompt for me", &g_v.autoWrite)) changed = true;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("A picture's own prompt describes a still frame: sent as it is, the video barely moves.\n"
                              "The prompt writer looks at the start (and end) picture and writes what happens:\naction beats over the clip, the camera, and the sound.");
        ui::SectionHeading("VIDEO PROMPT");
        ImGui::SameLine();
        ImGui::BeginDisabled(writingHere);
        if (ImGui::SmallButton(g_v.prompt.empty() ? "Write it now" : "Write it again")) {
            WriteJob j;
            j.first = g_v.first, j.last = o.last, j.idea = idea, j.context = g_v.context, j.key = key, j.opt = o, j.toPage = true;
            g_writeQueue.push_back(std::move(j));
        }
        ImGui::EndDisabled();
        if (writingHere) {
            std::string phase;
            {
                std::lock_guard<std::mutex> lock(g_phaseMutex);
                phase = g_phase;
            }
            ImGui::TextColored(ui::kAccentHover, "%s", phase.empty() ? "Writing the video prompt..." : phase.c_str());
        } else {
            if (WrapEdit("##videoprompt", g_v.prompt, ui::Dp(130.0f))) g_v.edited = true;
            if (g_v.prompt.empty())
                ImGui::TextDisabled(g_v.autoWrite ? "Written when you press Make (or now: Write it now)." : "Write what happens and what it sounds like.");
            else if (g_v.autoWrite && !g_v.edited && g_v.writtenKey != key)
                ImGui::TextColored(ui::kTextDim, "Out of date (the pictures, idea or length changed): written again when you press Make.");
        }
        ImGui::Spacing();

        // settings
        ui::SectionHeading("QUALITY");
        for (int qi = 0; qi < kQualityCount; ++qi) {
            if (qi) ImGui::SameLine();
            const bool have = qi == kLtxQuality ? LtxReady() : !kQuality[qi].lora || HasLora(kQuality[qi].lora);
            ImGui::BeginDisabled(!have);
            if (Chip(kQuality[qi].name, o.quality == qi)) o.quality = qi, changed = true;
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) {
                if (have) ImGui::SetTooltip("%s", kQuality[qi].about);
                else if (qi == kLtxQuality)
                    ImGui::SetTooltip("%s\nNeeds Lightricks/LTX-2.5's files in ComfyUI's models folders (the repo is gated: accept its licence on huggingface.co, then "
                                      "download): diffusion_models\\%s, text_encoders\\%s, vae\\%s and %s, latent_upscale_models\\%s", kQuality[qi].about, kLtxModel,
                                      kLtxTextEncoder, kLtxVideoVae, kLtxAudioVae, kLtxUpscaler);
                else ImGui::SetTooltip("%s\nNeeds %s in ComfyUI's loras folder", kQuality[qi].about, kQuality[qi].lora);
            }
            if (qi == 1 && !have) { // Balanced's file is one click away
                ImGui::SameLine(0.0f, ui::Dp(2.0f));
                if (g_balDl.running) {
                    std::error_code ec;
                    const double gb = fs::exists(g_balDl.part, ec) ? fs::file_size(g_balDl.part, ec) / 1073741824.0 : 0.0;
                    ImGui::TextColored(ui::kAccentHover, "%.1f / 1.6 GB", gb);
                } else if (ImGui::SmallButton(g_balDl.failed ? "Get it (failed: retry)" : "Get it (1.6 GB)")) {
                    StartDownload(g_balDl, kBalancedUrl, kBalancedLora, L"loras", 1.0);
                    g_v.lorasAsked = -1000.0; // look again when it is there
                    g_v.diskKnown = false;
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Downloads %s into ComfyUI's loras folder", kBalancedLora);
            }
            if (qi == 3 && !have) { // Presenter: downloaded and converted by tools\h3turbo\setup_taomate.ps1
                ImGui::SameLine(0.0f, ui::Dp(2.0f));
                const std::string& line = PresenterSetupLine();
                if (g_taoSetup.running) {
                    std::error_code ec;
                    const std::wstring part = std::wstring(_wgetenv(L"TEMP") ? _wgetenv(L"TEMP") : L"") + L"\\taomate\\adapter_model.safetensors.part";
                    const double gb = fs::exists(part, ec) ? fs::file_size(part, ec) / 1073741824.0 : 0.0;
                    if (line.rfind("download", 0) == 0) ImGui::TextColored(ui::kAccentHover, "%.1f / 2.3 GB", gb);
                    else ImGui::TextColored(ui::kAccentHover, "%s...", line.c_str());
                } else if (ImGui::SmallButton(line.rfind("error", 0) == 0 ? "Get it (failed: retry)" : "Get it (2.5 GB)")) {
                    StartPresenterSetup();
                    g_v.lorasAsked = -1000.0;
                    g_v.diskKnown = false;
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                    ImGui::SetTooltip("Downloads TaoMate-H3-FL2VA (TaoLiveAIGC, 2.5 GB) and converts it into a ComfyUI LoRA (1.2 GB) in the loras folder.%s",
                                      line.rfind("error", 0) == 0 ? ("\n\nLast time: " + line).c_str() : "");
            }
        }
        if (g_taoSetup.finished.exchange(false)) g_v.lorasAsked = -1000.0, g_v.diskKnown = false; // look for it again
        if (!g_balDl.running && g_v.diskKnown && !HasLora(kBalancedLora) && ImGui::GetTime() - g_v.lorasAsked > 5.0) g_v.diskKnown = false; // re-check after a download
        if (o.quality > 0 && kQuality[o.quality].lora && !HasLora(kQuality[o.quality].lora)) o.quality = 0;
        if (o.quality == kLtxQuality && !LtxReady()) o.quality = 0;
        ImGui::SameLine();
        ImGui::TextDisabled("%s", kQuality[o.quality].about);
        if (o.quality == 0 && o.seconds > 15.0 + 1e-6) // (tested 2026-10-24: the same 20 s first-person video, Fast 5.5 flicker vs Balanced 4.1; Fast's 2nd part smeared)
            ImGui::TextColored(ui::kWarn, "Fast loses detail over a long video (it smeared a 20 s video's second part; Balanced held it): Balanced is safer here.");
        if (o.quality != kLtxQuality && !HavePrism() && g_ref.known) { // the H3 model it makes with: PRISM is one click away
            static bool wasRunning = false;
            if (wasRunning && !g_prismDl.running) g_ref.asked = -1000.0, g_v.diskKnown = false; // finished: look again
            wasRunning = g_prismDl.running;
            if (g_prismDl.running) {
                std::error_code ec;
                const double gb = fs::exists(g_prismDl.part, ec) ? fs::file_size(g_prismDl.part, ec) / 1073741824.0 : 0.0;
                ImGui::TextColored(ui::kAccentHover, "Downloading PRISM: %.1f of 14.8 GB (then checked)...", gb);
            } else {
                if (ImGui::SmallButton(g_prismDl.failed ? "Get the PRISM model (failed: retry)" : "Get the PRISM model (14.8 GB)"))
                    StartDownload(g_prismDl, kPrismUrl, kPrism, L"diffusion_models", 15.0, kPrismSha), g_v.diskKnown = false;
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                    ImGui::SetTooltip("%s into ComfyUI's diffusion_models, checked against its SHA-256 (OliviaRossi/Minimax-h3_Singularity-x-10Eros-Max_PRISM).\n"
                                      "Singularity's fine-tune blended with 10Eros-Max's turbo: as quick as now, sharper, and closer to the timeline and its sound.",
                                      kPrism);
                ImGui::SameLine();
                ImGui::TextDisabled("then every quality makes with it: sharper, same speed");
            }
        }
        DrawVedaRow(app, kVedaPredictor);
        DrawTweaksRow(false);
        DrawModelLoras(app, app.comfy.h3Loras, "MiniMax H3", "vlr"); // (the Generate page's Video and Star in a video use the same ones)
        if (o.quality == 3) { // Presenter: Veda and a clean last frame are part of what keeps it clean
            if (o.last.empty())
                ImGui::TextDisabled("Presenter ends on the start picture (it keeps titles and captions off the picture); pick an end picture to end elsewhere.");
            if (VedaReady(kVedaPredictor)) {
                if (!g_v.veda) ImGui::TextDisabled("Presenter always uses Veda: it keeps leaves and captions from drifting over the picture.");
            } else if (g_v.vedaKnown) {
                ImGui::TextColored(ui::kWarn, "Presenter without Veda: leaves, branches or captions can drift over the picture. Set up Veda above for clean videos.");
            }
        }

        ui::SectionHeading("RESOLUTION");
        int srcW = 0, srcH = 0;
        if (!g_v.first.empty()) {
            const AppState::LibThumb lt = app.LibraryThumb(g_v.first);
            srcW = lt.w, srcH = lt.h;
            if (srcW <= 0) srcW = 16, srcH = 9;
        }
        for (int ri = 0; ri < 4; ++ri) {
            if (ri) ImGui::SameLine();
            H3Options t = o;
            t.res = ri;
            int w = 0, h = 0;
            H3Size(t, srcW, srcH, w, h);
            char lbl[48];
            snprintf(lbl, sizeof(lbl), "%s  %dx%d##res%d", kRes[ri].name, w, h, ri);
            if (Chip(lbl, o.res == ri)) o.res = ri, changed = true;
            if (ri == 2 && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("What the model was trained at");
            if (ri == 3 && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Sharper, but much slower and heavier on the GPU");
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Shape");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ui::Dp(170.0f));
        if (ImGui::Combo("##shape", &o.shape, kShapes, 4)) changed = true;
        if (o.shape == 0 && g_v.first.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("(no picture: landscape)");
        }

        ui::SectionHeading("LENGTH");
        const bool longOk = LongReady();
        float sec = static_cast<float>(o.seconds);
        ImGui::SetNextItemWidth(ui::Dp(220.0f));
        if (ImGui::SliderFloat("##secs", &sec, 3.0f, longOk ? static_cast<float>(kLongMax) : 15.0f, "%.0f seconds")) o.seconds = std::round(sec), changed = true;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip(longOk ? "Up to 15 s: one clip. Longer (up to a minute): H3 Continuum makes it in parts that continue each other, picture and sound."
                                     : "MiniMax H3 makes up to 15 s at a time. For longer videos, set up H3 Continuum (below).");
        ImGui::SameLine();
        if (o.seconds > 15.0 + 1e-6 && longOk) {
            int chunks = 2;
            double chunkSeconds = 8.0;
            LongPlan(o.seconds, chunks, chunkSeconds);
            ImGui::TextDisabled("%d parts of %.1f s, one continuous shot", chunks, chunkSeconds);
            if (!g_v.first.empty() && FindProfile(g_v.first)) { // a character's studio picture (tested: a long video from one can drift back to it)
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(ui::kWarn, "This start picture is a white-studio photo: a long video from it can drift back to the studio near the end. "
                                              "A picture of them in the place works better (Edit with Qwen..., or Make a story's scene pictures).");
                ImGui::PopTextWrapPos();
            }
        } else {
            ImGui::TextDisabled("%d frames at 24 fps", FramesFor(o.seconds));
        }
        DrawContinuumRow();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("How many");
        for (int n : {1, 2, 4}) {
            ImGui::SameLine();
            char lbl[8];
            snprintf(lbl, sizeof(lbl), "%d##t%d", n, n);
            if (Chip(lbl, o.times == n)) o.times = n, changed = true;
        }
        if (changed) SaveSettings();
        ImGui::EndChild();

        // make it: always in view under the form
        ImGui::Separator();
        int w = 0, h = 0;
        H3Size(o, srcW, srcH, w, h);
        H3Options eo = o;
        eo.veda = (g_v.veda || eo.quality == 3) && (VedaReady(kVedaPredictor) || (!g_v.vedaKnown && g_v.vedaWasReady));
        const double est = EstimateSeconds(eo, w, h);
        const bool needWrite = g_v.autoWrite && (g_v.prompt.empty() || (!g_v.edited && g_v.writtenKey != key));
        const bool ready = !writingHere && (needWrite || g_v.prompt.find_first_not_of(" \t\r\n") != std::string::npos);
        ImGui::BeginDisabled(!ready);
        bool make = ui::PrimaryButton(needWrite ? "Write it and make" : "Make the video", ImVec2(ui::Dp(190.0f), ui::Dp(38.0f))) ||
                          (ready && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Enter));
        ImGui::EndDisabled();
        if (g_v.pressMake && ready) make = true;
        g_v.pressMake = false;
        const std::string label = ShortLabel(!g_v.idea.empty() ? g_v.idea : !g_v.first.empty() ? Utf8(fs::path(g_v.first).filename().wstring()) : g_v.prompt);
        if (make) {
            if (needWrite) {
                WriteJob j;
                j.first = g_v.first, j.last = o.last, j.idea = idea, j.context = g_v.context, j.key = key, j.opt = o, j.label = label;
                j.toPage = true;
                j.sendAfter = true;
                g_writeQueue.push_back(std::move(j));
            } else {
                std::string prompt = g_v.prompt; // (a camera move picked, not in the prompt: said at its end)
                const char* cam = CameraText();
                if (cam && LowerAscii(prompt).find(LowerAscii(cam)) == std::string::npos) prompt += std::string("\nCamera: ") + cam + ".";
                if (SendH3(app, g_v.first, prompt, H3Options(o), label)) NotePresetSend();
            }
        }
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::Text("%d x %d, %.0f s, %s", w, h, o.seconds, kQuality[o.quality].name);
        {
            int chunks = 1;
            double part = o.seconds;
            if (o.seconds > 15.0 + 1e-6) LongPlan(o.seconds, chunks, part);
            const std::string heavy = HeavyNote(w, h, part, false);
            if (!heavy.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", heavy.c_str());
            if (!heavy.empty()) {
                ImGui::SameLine();
                ImGui::TextColored(ui::kWarn, "(heavy)");
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", heavy.c_str());
            }
        }
        if (est < 90) ImGui::TextDisabled("about %.0f seconds%s (Ctrl+Enter)", est, o.times > 1 ? " for all" : "");
        else ImGui::TextDisabled("about %.0f minutes%s (Ctrl+Enter)", std::ceil(est / 60.0), o.times > 1 ? " for all" : "");
        ImGui::EndGroup();
    }
    ImGui::EndChild();

    // a bar between the two sides: drag it to give one more room
    ImGui::SameLine(0.0f, 0.0f);
    if (Splitter("##vsplit", true, ImVec2(splitW, ImGui::GetContentRegionAvail().y))) g_v.leftW = (leftW + ImGui::GetIO().MouseDelta.x) / ui::Dp(1.0f);
    if (ImGui::IsItemDeactivated()) SaveSettings();
    ImGui::SameLine(0.0f, 0.0f);

    // ---- the right side: the player, what is being made, the videos
    ImGui::BeginChild("##videoright", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    const float rightH = ImGui::GetContentRegionAvail().y;
    { // MV_UI_TEST "videoplay": plays the newest video once a new one is there (tests)
        static const bool testPlay = [] {
            char buf[256] = {};
            return GetEnvironmentVariableA("MV_UI_TEST", buf, sizeof(buf)) && strstr(buf, "videoplay") != nullptr;
        }();
        static const bool testPlayNow = [] { // "playnow": the newest video at once
            char buf[256] = {};
            return GetEnvironmentVariableA("MV_UI_TEST", buf, sizeof(buf)) && strstr(buf, "playnow") != nullptr;
        }();
        static bool playedNow = false;
        if (testPlayNow && !playedNow && !g_vid.clips.empty()) {
            playedNow = true;
            char spec[256] = {};
            GetEnvironmentVariableA("MV_UI_TEST", spec, sizeof(spec));
            PlayClip(app, g_vid.clips.front().path, strstr(spec, "paused") == nullptr); // "playnow;paused": opened as a finished video is
        }
        static const bool testCycle = [] { // "playcycle": a different video every 300 ms (the open/close race under load)
            char buf[256] = {};
            return GetEnvironmentVariableA("MV_UI_TEST", buf, sizeof(buf)) && strstr(buf, "playcycle") != nullptr;
        }();
        static ULONGLONG cycleAt = 0;
        static size_t cycleN = 0;
        if (testCycle && !g_vid.clips.empty() && GetTickCount64() - cycleAt > 300) {
            cycleAt = GetTickCount64();
            PlayClip(app, g_vid.clips[cycleN++ % std::min<size_t>(g_vid.clips.size(), 12)].path);
        }
        static size_t firstCount = static_cast<size_t>(-1);
        if (testPlay && g_vid.fut.valid() == false && !g_vid.clips.empty()) {
            if (firstCount == static_cast<size_t>(-1)) firstCount = g_vid.clips.size();
            else if (g_vid.clips.size() > firstCount && !(g_vid.player && g_vid.player->IsOpen())) PlayClip(app, g_vid.clips.front().path);
        }
    }
    const float playerH = std::clamp(rightH * g_v.playerFrac, ui::Dp(120.0f), std::max(ui::Dp(120.0f), rightH - ui::Dp(160.0f)));
    DrawPlayer(app, playerH);
    // a bar under the player: drag it to make the player or the videos bigger
    if (Splitter("##hsplit", false, ImVec2(ImGui::GetContentRegionAvail().x, splitW))) g_v.playerFrac = std::clamp((playerH + ImGui::GetIO().MouseDelta.y) / rightH, 0.15f, 0.85f);
    if (ImGui::IsItemDeactivated()) SaveSettings();
    // being made now
    const long long now = ComfyService::NowMs();
    if (app.comfySvc) {
        for (const ComfyActivity& a : app.comfySvc->Activity()) {
            if (a.label.rfind("Video (", 0) != 0 && a.label.rfind("Long video (", 0) != 0) continue;
            const bool done = a.state == ComfyState::Done, failed = a.state == ComfyState::Failed;
            if (done || (failed && now - a.finishedMs > 60000)) continue; // finished ones are in the videos below
            ImGui::PushID(a.id);
            const long long secs = std::max(0LL, (failed ? a.finishedMs : now) - (a.runningMs > 0 ? a.runningMs : a.startedMs)) / 1000;
            if (failed) ImGui::TextColored(ui::kBad, "%s: %s", a.label.c_str(), a.message.c_str());
            else {
                ImGui::TextColored(ui::kAccentHover, "%s  %lld:%02lld", a.state == ComfyState::Running ? "Generating" : "Waiting", secs / 60, secs % 60);
                ImGui::SameLine();
                ImGui::TextUnformatted(a.label.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton(a.state == ComfyState::Running ? "Stop" : "Cancel")) app.comfySvc->Cancel(a.id);
                if (a.state == ComfyState::Running) DrawComfyJobProgress(a.id);
            }
            ImGui::PopID();
        }
    }
    if (g_writeFut.valid()) ImGui::TextColored(ui::kAccentHover, "Writing the video prompt for \"%s\"...", g_writing.label.c_str());
    if (g_maskFut.valid() && !g_peopleWait.empty()) {
        std::string ph;
        {
            std::lock_guard<std::mutex> lock(g_phaseMutex);
            ph = g_phase;
        }
        ImGui::TextColored(ui::kAccentHover, "Finding the people for \"%s\": %s", g_peopleWait.front().job.label.c_str(), ph.empty() ? "..." : ph.c_str());
    }
    for (size_t i = 1; i < g_peopleWait.size(); ++i) ImGui::TextDisabled("Next: find the people for \"%s\"", g_peopleWait[i].job.label.c_str());
    for (const WriteJob& j : g_writeQueue) ImGui::TextDisabled("Next: write and make \"%s\"", j.label.c_str());
    ui::SectionHeading("VIDEOS");
    if (!g_vid.clips.empty()) { // delete: the picked ones, or all of them (asked twice); to the Recycle Bin
        ImGui::SameLine(0.0f, ui::Dp(12.0f));
        if (!g_vid.picked.empty()) {
            char l[64];
            snprintf(l, sizeof(l), "Delete %d picked", static_cast<int>(g_vid.picked.size()));
            if (ImGui::SmallButton(l)) DeleteVideos(app, std::vector<std::wstring>(g_vid.picked.begin(), g_vid.picked.end()));
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Moves them to the Recycle Bin (you can restore them from there)");
            ImGui::SameLine();
            if (ImGui::SmallButton("Unpick")) g_vid.picked.clear();
            // the picked, oldest first (the list shows the newest first)
            std::vector<std::wstring> picked;
            for (size_t k = g_vid.clips.size(); k-- > 0;)
                if (g_vid.picked.count(g_vid.clips[k].path)) picked.push_back(g_vid.clips[k].path);
            if (picked.size() == 2) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Transition between them...")) OpenTransition(picked[0], picked[1]);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("A clip from the first one's last frame to the second's first (then all three joined)");
            }
            if (picked.size() >= 2) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Make an edit from them...")) OpenAutoEdit(picked);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("The prompt writer plans the cuts, ffmpeg cuts and joins them, with music if you like");
            }
        } else {
            char l[64];
            snprintf(l, sizeof(l), g_vid.confirmAll ? "Sure? Delete all %d: click again" : "Clear out all...", static_cast<int>(g_vid.clips.size()));
            if (ImGui::SmallButton(l)) {
                if (!g_vid.confirmAll) g_vid.confirmAll = 1;
                else {
                    g_vid.confirmAll = 0;
                    std::vector<std::wstring> all;
                    for (const Clip& c : g_vid.clips) all.push_back(c.path);
                    DeleteVideos(app, all);
                }
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("Moves every video shown here to the Recycle Bin.\nTo delete only some: Ctrl+click them, or right-click one > Delete.");
            if (g_vid.confirmAll && !ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) g_vid.confirmAll = 0; // (a click elsewhere: no)
            ImGui::SameLine();
            ImGui::TextDisabled("(Ctrl+click videos: transition, edit)");
        }
    }
    {
        const std::string tools = VideoToolsStatus();
        if (!tools.empty()) ImGui::TextColored(ui::kAccentHover, "%s", tools.c_str());
    }
    if (!g_vid.clips.empty()) { // their size
        ImGui::SameLine(ImGui::GetContentRegionMax().x - ui::Dp(170.0f));
        ImGui::SetNextItemWidth(ui::Dp(170.0f));
        if (ImGui::SliderFloat("##thumbw", &g_v.thumbW, 80.0f, 420.0f, "size %.0f")) g_v.thumbW = std::round(g_v.thumbW);
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("How big the videos are (Ctrl + mouse wheel over them too)");
    }
    if (g_vid.clips.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::kTextDim);
        ImGui::TextWrapped("None in ComfyUI's output folder (video) yet. When one is done it shows up here.");
        ImGui::PopStyleColor();
    }
    // a grid of their first frames, in a scrolling box of its own (the player stays in view)
    ImGui::BeginChild("##videogrid", ImVec2(0, 0), ImGuiChildFlags_None, ImGui::GetIO().KeyCtrl ? ImGuiWindowFlags_NoScrollWithMouse : 0); // Ctrl: the wheel resizes
    if (ImGui::IsWindowHovered() && ImGui::GetIO().KeyCtrl && ImGui::GetIO().MouseWheel != 0.0f) {
        g_v.thumbW = std::clamp(g_v.thumbW * (ImGui::GetIO().MouseWheel > 0 ? 1.1f : 1.0f / 1.1f), 80.0f, 420.0f);
        SaveSettings();
    }
    const float tw = ui::Dp(g_v.thumbW), thh = tw * 0.62f, gap = ui::Dp(8.0f);
    const int cols = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + gap) / (tw + gap)));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (size_t k = 0; k < g_vid.clips.size(); ++k) {
        const Clip& c = g_vid.clips[k];
        if (k % cols) ImGui::SameLine(0.0f, gap);
        ImGui::PushID(static_cast<int>(k));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##clip", ImVec2(tw, thh + ImGui::GetTextLineHeight() + ui::Dp(4.0f)));
        const bool hov = ImGui::IsItemHovered();
        const bool picked = g_vid.picked.count(c.path) > 0;
        if (ImGui::IsItemClicked()) {
            if (ImGui::GetIO().KeyCtrl) { // pick it (to delete several)
                if (picked) g_vid.picked.erase(c.path);
                else g_vid.picked.insert(c.path);
            } else {
                PlayClip(app, c.path);
            }
        }
        if (ImGui::BeginPopupContextItem("##clipmenu")) {
            if (ImGui::MenuItem("Recreate (load its pictures, prompt and settings)")) RecreateVideo(app, c.path);
            if (ImGui::MenuItem("Star in it (use as the reference video)")) {
                g_star.video = c.path;
                g_star.start = 0;
                g_mode = 1;
            }
            if (ImGui::MenuItem("Show in Explorer")) {
                const std::wstring arg = L"/select,\"" + c.path + L"\"";
                ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
            }
            if (ImGui::MenuItem("Copy")) {
                std::string msg;
                CopyImagesToClipboard(GetActiveWindow(), {c.path}, false, &msg);
                app.SetStatus(msg);
            }
            if (ImGui::MenuItem("Copy without metadata")) {
                std::string msg;
                CopyImagesToClipboard(GetActiveWindow(), {c.path}, true, &msg);
                app.SetStatus(msg);
            }
            ImGui::Separator();
            if (ImGui::MenuItem(picked ? "Unpick" : "Pick (to delete several)", "Ctrl+click")) {
                if (picked) g_vid.picked.erase(c.path);
                else g_vid.picked.insert(c.path);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Remove the background...")) OpenBackgroundRemoval(c.path);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Keep the person (or anything you name) on transparency, a colour or a picture");
            if (ImGui::MenuItem("Make an edit with others...", nullptr, false, g_vid.picked.size() >= 1)) {
                std::vector<std::wstring> clips;
                for (size_t k = g_vid.clips.size(); k-- > 0;)
                    if (g_vid.picked.count(g_vid.clips[k].path) || g_vid.clips[k].path == c.path) clips.push_back(g_vid.clips[k].path);
                OpenAutoEdit(clips);
            }
            if (g_vid.picked.size() == 1 && !g_vid.picked.count(c.path) && ImGui::MenuItem("Transition from the picked one to this...")) OpenTransition(*g_vid.picked.begin(), c.path);
            NsfwMarkMenu(app, c.path);
            if (ImGui::MenuItem("Delete (to the Recycle Bin)")) {
                ImGui::EndPopup();
                ImGui::PopID();
                DeleteVideos(app, {c.path});
                break; // (the list changed under the loop)
            }
            ImGui::EndPopup();
        }
        if (ImGui::IsItemVisible()) {
            const bool playing = g_vid.player && g_vid.player->IsOpen() && _wcsicmp(g_vid.player->Path().c_str(), c.path.c_str()) == 0;
            dl->AddRectFilled(p, ImVec2(p.x + tw, p.y + thh), IM_COL32(0, 0, 0, 255), ui::Dp(6.0f));
            const AppState::LibThumb lt = app.LibraryThumb(c.path);
            if (lt.srv && lt.w > 0 && lt.h > 0) {
                const float s = std::min(tw / lt.w, thh / lt.h);
                const ImVec2 a(p.x + (tw - lt.w * s) * 0.5f, p.y + (thh - lt.h * s) * 0.5f);
                dl->AddImage((ImTextureID)(intptr_t)lt.srv, a, ImVec2(a.x + lt.w * s, a.y + lt.h * s));
            }
            if (hov || playing) dl->AddRect(p, ImVec2(p.x + tw, p.y + thh), ImGui::GetColorU32(playing ? ui::kAccent : ui::kAccentHover), ui::Dp(6.0f), 0, ui::Dp(2.0f));
            if (picked) { // picked: a thick border and a tick
                dl->AddRect(p, ImVec2(p.x + tw, p.y + thh), ImGui::GetColorU32(ui::kWarn), ui::Dp(6.0f), 0, ui::Dp(3.5f));
                const ImVec2 t(p.x + ui::Dp(14.0f), p.y + ui::Dp(14.0f));
                dl->AddCircleFilled(t, ui::Dp(10.0f), ImGui::GetColorU32(ui::kWarn), 16);
                dl->AddLine(ImVec2(t.x - ui::Dp(5.0f), t.y), ImVec2(t.x - ui::Dp(1.0f), t.y + ui::Dp(4.0f)), IM_COL32(20, 20, 20, 255), ui::Dp(2.0f));
                dl->AddLine(ImVec2(t.x - ui::Dp(1.0f), t.y + ui::Dp(4.0f)), ImVec2(t.x + ui::Dp(5.0f), t.y - ui::Dp(4.0f)), IM_COL32(20, 20, 20, 255), ui::Dp(2.0f));
            }
            const ImVec2 mid(p.x + tw * 0.5f, p.y + thh * 0.5f);
            if (!playing) {
                dl->AddCircleFilled(mid, ui::Dp(13.0f), IM_COL32(10, 12, 16, 150), 20);
                dl->AddTriangleFilled(ImVec2(mid.x - ui::Dp(4.0f), mid.y - ui::Dp(6.0f)), ImVec2(mid.x - ui::Dp(4.0f), mid.y + ui::Dp(6.0f)), ImVec2(mid.x + ui::Dp(7.0f), mid.y), IM_COL32_WHITE);
            }
            const float right = p.x + tw;
            ImGui::PushStyleColor(ImGuiCol_Text, playing ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : ui::kTextDim);
            ImGui::RenderTextEllipsis(dl, ImVec2(p.x, p.y + thh + ui::Dp(2.0f)), ImVec2(right, p.y + thh + ui::Dp(4.0f) + ImGui::GetTextLineHeight()), right, right, c.name.c_str(), nullptr, nullptr);
            ImGui::PopStyleColor();
            double took = 0;
            std::string what;
            if (ComfyGenerationTime(c.path, took, &what)) { // how long it took, top right
                const std::string t = FormatTook(took);
                const ImVec2 ts = ImGui::CalcTextSize(t.c_str());
                const ImVec2 b(p.x + tw - ts.x - ui::Dp(10.0f), p.y + ui::Dp(4.0f));
                dl->AddRectFilled(b, ImVec2(b.x + ts.x + ui::Dp(6.0f), b.y + ts.y + ui::Dp(2.0f)), IM_COL32(10, 12, 16, 190), ui::Dp(4.0f));
                dl->AddText(ImVec2(b.x + ui::Dp(3.0f), b.y + ui::Dp(1.0f)), IM_COL32(230, 232, 238, 255), t.c_str());
            }
            if (hov) {
                if (took > 0) ImGui::SetTooltip("%s\nMade in %s: %s\nClick: play it here   Ctrl+click: pick   Right-click: Recreate, Delete...", c.name.c_str(), FormatTook(took).c_str(), what.c_str());
                else ImGui::SetTooltip("%s\nClick: play it here   Ctrl+click: pick   Right-click: Recreate, Delete...", c.name.c_str());
            }
        }
        ImGui::PopID();
    }
    ImGui::EndChild(); // the grid
    ImGui::EndChild();
    ImGui::EndChild();
}

// The Edits page's Videos mode (2026-10-28: making a video crisp belongs with the other edits too): the Video page's player with Make
// it crisp and the quick upscale under it, and the videos to pick from - the ones made here, or any file
void DrawVideoEditsBody(AppState& app, float height) {
    if (!g_v.loaded) LoadSettings();
    RefreshClips(app);
    if (!ImGui::BeginTable("##videoedits", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings, ImVec2(0, height))) return;
    ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthStretch, 0.62f);
    ImGui::TableSetupColumn("Videos", ImGuiTableColumnFlags_WidthStretch, 0.38f);
    ImGui::TableNextRow(0, height);
    ImGui::TableSetColumnIndex(0);
    ImGui::BeginChild("##ve_player", ImVec2(0, height - ui::Dp(4.0f)), ImGuiChildFlags_None);
    ui::SectionHeading("MAKE A VIDEO CRISP");
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Pick a video on the right, then Make it crisp: MiniMax H3 redraws its fine detail (skin, hair, eyes, fabric) at 1080p or 2K and "
                        "keeps its motion, faces and sound. The quick upscale only resizes, in seconds.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    DrawPlayer(app, std::max(ui::Dp(160.0f), ImGui::GetContentRegionAvail().y - ui::Dp(90.0f)));
    ImGui::EndChild();
    ImGui::TableSetColumnIndex(1);
    ImGui::BeginChild("##ve_list", ImVec2(0, height - ui::Dp(4.0f)), ImGuiChildFlags_None);
    ui::SectionHeading("VIDEOS");
    if (ImGui::Button("Choose a video file...")) {
        wchar_t buf[MAX_PATH * 2] = {};
        OPENFILENAMEW ofn = {sizeof(ofn)};
        ofn.hwndOwner = GetActiveWindow();
        ofn.lpstrFilter = L"Videos\0*.mp4;*.webm;*.mov;*.mkv\0All files\0*.*\0";
        ofn.lpstrFile = buf;
        ofn.nMaxFile = MAX_PATH * 2;
        ofn.lpstrTitle = L"A video to make crisp";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&ofn)) PlayClip(app, buf, false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Open the Video page")) app.requestTab = 10;
    if (g_vid.clips.empty()) ImGui::TextDisabled("None made yet: the ones the Video page makes show up here.");
    ImGui::BeginChild("##ve_grid", ImVec2(0, 0), ImGuiChildFlags_None);
    const float tw = ui::Dp(150.0f), thh = tw * 0.62f, gap = ui::Dp(8.0f);
    const int cols = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + gap) / (tw + gap)));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (size_t k = 0; k < g_vid.clips.size(); ++k) {
        const Clip& c = g_vid.clips[k];
        if (k % cols) ImGui::SameLine(0.0f, gap);
        ImGui::PushID(static_cast<int>(k));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##clip", ImVec2(tw, thh + ImGui::GetTextLineHeight() + ui::Dp(4.0f)))) PlayClip(app, c.path, false);
        const bool hov = ImGui::IsItemHovered();
        if (ImGui::IsItemVisible()) {
            const bool shown = g_vid.player && g_vid.player->IsOpen() && _wcsicmp(g_vid.player->Path().c_str(), c.path.c_str()) == 0;
            dl->AddRectFilled(p, ImVec2(p.x + tw, p.y + thh), IM_COL32(0, 0, 0, 255), ui::Dp(6.0f));
            const AppState::LibThumb lt = app.LibraryThumb(c.path);
            if (lt.srv && lt.w > 0 && lt.h > 0) {
                const float sc = std::min(tw / lt.w, thh / lt.h);
                const ImVec2 a(p.x + (tw - lt.w * sc) * 0.5f, p.y + (thh - lt.h * sc) * 0.5f);
                dl->AddImage((ImTextureID)(intptr_t)lt.srv, a, ImVec2(a.x + lt.w * sc, a.y + lt.h * sc));
            }
            if (hov || shown) dl->AddRect(p, ImVec2(p.x + tw, p.y + thh), ImGui::GetColorU32(shown ? ui::kAccent : ui::kAccentHover), ui::Dp(6.0f), 0, ui::Dp(2.0f));
            const float right = p.x + tw;
            ImGui::PushStyleColor(ImGuiCol_Text, shown ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : ui::kTextDim);
            ImGui::RenderTextEllipsis(dl, ImVec2(p.x, p.y + thh + ui::Dp(2.0f)), ImVec2(right, p.y + thh + ui::Dp(4.0f) + ImGui::GetTextLineHeight()), right, right, c.name.c_str(), nullptr, nullptr);
            ImGui::PopStyleColor();
            if (hov) ImGui::SetTooltip("%s\nClick: open it on the left (then Make it crisp)", c.name.c_str());
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::EndChild();
    ImGui::EndTable();
}
