// Star in a video > Swap the person > Repaint a frame: Viggle-Animate (huggingface.co/Viggle/Viggle-Animate, the MiniMax H3 licence), a
// finetune of H3's reference model that carries ONE repainted frame of the clip through the whole clip, distilled to three passes. Its two
// inputs are the clip and that frame; no mask, grey figure, pose or prompt (a fixed text embedding), so nothing pulls the clip's own person
// back. ComfyUI: Saganaki22's ComfyUI-Viggle-Animate-H3 nodes (Apache-2.0) and drbaph's converted weights.
// Why: Peach (a 3D cartoon) on a girl dancing close to the camera failed every way with the grey-figure swap (2026-10-29: no Veda, another
// seed, 10 steps, a flat or blob figure, the source greyed, a stronger prompt, the reference at full size, a full-body Peach, Peach painted
// into the room as a second reference, even that frame as the clip's first: H3 drew a realistic girl in a black tee with Peach printed on
// it). Viggle-Animate, from the frame FLUX 3 painted: Peach in her gown through every move of the dance, 5 s at 480 x 832 in 73 s.
#include "viggle_swap.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#include "clef.h"
#include "comfy.h"
#include "video_util.h"
#include "vision_prompt.h" // ImageDataUrl

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

const char* const kModel = "minimax_h3_ref2va_viggle_pruned_int8_convrot.safetensors"; // 21 GB (drbaph/Viggle-Animate-ComfyUI)
const char* const kLora = "viggle_animate_dmd_lora_r64.safetensors";                  // the DMD LoRA at rank 64 (0.9 GB): three passes
const char* const kTextCond = "fixed_embed_fwd_anyframe.safetensors";                  // the frozen prompt, in models\text_cond
const char* const kVideoVae = "minimax_h3_video_vae_int8_convrot.safetensors";
const wchar_t* const kPack = L"ComfyUI-Viggle-Animate-H3";

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    return fs::path(buf).parent_path().wstring();
}

std::wstring DataDir() {
    wchar_t buf[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    return std::wstring(buf) + L"\\Try2Create";
}

bool FoundUnder(const fs::path& dir, const char* name) { // (the LoRA sorter may have put it in a model folder)
    std::error_code ec;
    if (fs::exists(dir / name, ec)) return true;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        if (it.depth() > 2) it.disable_recursion_pending();
        else if (it->path().filename() == name) return true;
    }
    return false;
}

std::atomic<bool> g_setupRunning{false};
std::mutex g_setupMutex;
std::wstring g_setupLog;
std::string g_setupLine;

} // namespace

bool ViggleReady(std::string* missing) {
    std::string m;
    auto add = [&](const std::string& s) { m += (m.empty() ? "" : ", ") + s; };
    const std::wstring models = ComfyModelsDir(), install = ComfyInstallDir();
    std::error_code ec;
    if (models.empty() || install.empty()) {
        add("ComfyUI");
    } else {
        if (!fs::exists(fs::path(models) / L"diffusion_models" / kModel, ec)) add(std::string("the Viggle-Animate model (") + kModel + ")");
        if (!FoundUnder(fs::path(models) / L"loras", kLora)) add("its speed LoRA");
        if (!fs::exists(fs::path(ComfyAppDir()) / L"models" / L"text_cond" / kTextCond, ec)) add("its text conditioning");
        if (!fs::exists(fs::path(ComfyAppDir()) / L"custom_nodes" / kPack / L"nodes.py", ec)) add("its ComfyUI nodes");
    }
    if (missing) *missing = m;
    return m.empty();
}

void ViggleSize(int clipW, int clipH, double megapixels, int& w, int& h) {
    const double aspect = clipW > 0 && clipH > 0 ? static_cast<double>(clipW) / clipH : 9.0 / 16.0;
    const double area = megapixels * 1e6;
    w = std::max(256, static_cast<int>(std::lround(std::sqrt(area * aspect) / 32.0)) * 32);
    h = std::max(256, static_cast<int>(std::lround(std::sqrt(area / aspect) / 32.0)) * 32);
}

// Tested 2026-10-29 on a 24 GB card against the pack's example graph: the upstream four-point schedule (three passes), euler, shift 3/3,
// comfy kitchen attention; the frame and the clip at the same size
json BuildViggleSwap(int w, int h, int frames, const std::string& outName, const std::string& vedaPredictor) {
    json g;
    auto node = [&](const std::string& id, const char* cls, json inputs, const char* title) {
        g[id] = {{"class_type", cls}, {"inputs", std::move(inputs)}, {"_meta", {{"title", title}}}};
    };
    auto L = [](const std::string& id, int k = 0) { return json::array({id, k}); };
    node("unet", "UNETLoader", {{"unet_name", kModel}, {"weight_dtype", "default"}}, "Viggle-Animate (H3 reference finetune)");
    node("lora", "LoraLoaderModelOnly", {{"model", L("unet")}, {"lora_name", kLora}, {"strength_model", 1.0}}, "Its DMD LoRA (three passes)");
    // sparse attention, about 30% quicker and the same picture (5 s at 480 x 832: 68 s dense, 47 s with Veda's reference-model predictor,
    // 48 s with ComfyUI's training-free sol-attn): Veda when its switch is on, else sol-attn as the pack's example has it
    std::string model = "lora";
    if (!vedaPredictor.empty()) {
        node("veda", "VedaSparseAttention", {{"model", L("lora")}, {"predictor", vedaPredictor}, {"generated_sparsity", "32"}, {"reference_sparsity", "32"},
                                             {"full_attention_layers", ""}, {"full_attention_steps", ""}, {"verbose", false}}, "Veda sparse attention");
        model = "veda";
    }
    node("attn", "ModelAttentionBackend", {{"model", L(model)}, {"attention", "comfy kitchen attention"}}, "Attention");
    model = "attn";
    if (vedaPredictor.empty()) {
        node("sparse", "BlockSparseAttention", {{"model", L("attn")}, {"selection", "sol-attn"}, {"selection.tau", 1.3}, {"start_percent", 0.2}, {"end_percent", 1.0},
                                                {"dense_blocks", ""}, {"min_tokens", 12288}, {"extra_tokens", 256}, {"sink_conditioning", "exact_kv_and_rows"},
                                                {"verbose", false}}, "Sparse attention (sol-attn)");
        model = "sparse";
    }
    node("shift", "MiniMaxH3SigmaShift", {{"model", L(model)}, {"shift_video", 3.0}, {"shift_audio", 3.0}}, "Shift 3 / 3");
    node("tc", "ViggleTextCondLoader", {{"text_cond", kTextCond}}, "Its fixed prompt");
    node("vae", "VAELoader", {{"vae_name", kVideoVae}}, "Video VAE");
    node("vid", "VHS_LoadVideo", {{"video", "@clip"}, {"force_rate", 24}, {"custom_width", 0}, {"custom_height", 0}, {"frame_load_cap", 0}, {"skip_first_frames", 0},
                                  {"select_every_nth", 1}, {"format", "AnimateDiff"}}, "The part of the clip (and its sound)");
    node("vscale", "ImageScale", {{"image", L("vid")}, {"upscale_method", "lanczos"}, {"width", w}, {"height", h}, {"crop", "center"}}, "At the video's size");
    node("ref", "LoadImage", {{"image", "@pic1"}}, "The frame, repainted");
    node("rscale", "ImageScale", {{"image", L("ref")}, {"upscale_method", "lanczos"}, {"width", w}, {"height", h}, {"crop", "center"}}, "At the same size");
    node("ksel", "KSamplerSelect", {{"sampler_name", "euler"}}, "Sampler");
    node("sig", "ManualSigmas", {{"sigmas", "1.0, 0.8571428571428571, 0.6, 0.0"}}, "Its four sigmas (three passes)");
    json images;
    if (frames <= 124) {
        node("cond", "ViggleAnimateConditioning", {{"cond_video", L("vscale")}, {"ref_image", L("rscale")}, {"text_cond", L("tc")}, {"vae", L("vae")},
                                                   {"width", 0}, {"height", 0}, {"length", 124}}, "The clip and the frame");
        node("guide", "BasicGuider", {{"model", L("shift")}, {"conditioning", L("cond", 0)}}, "Guider");
        node("noise", "RandomNoise", {{"noise_seed", 42}}, "Seed");
        node("samp", "SamplerCustomAdvanced", {{"noise", L("noise")}, {"guider", L("guide")}, {"sampler", L("ksel")}, {"sigmas", L("sig")}, {"latent_image", L("cond", 1)}}, "Sample");
        node("dec", "VAEDecode", {{"samples", L("samp")}, {"vae", L("vae")}}, "Decode");
        images = L("dec");
    } else { // a longer part: 124-frame windows, each carried on from five frames of the one before
        node("wcond", "ViggleAnimateConditioningWindowed", {{"cond_video", L("vscale")}, {"ref_image", L("rscale")}, {"text_cond", L("tc")}, {"vae", L("vae")},
                                                            {"width", 0}, {"height", 0}, {"chunk_frames", 124}, {"overlap_frames", 22}, {"continuation", "five_frame_anchor"}},
             "The clip in windows, and the frame");
        node("guide", "BasicGuider", {{"model", L("shift")}, {"conditioning", L("wcond", 1)}}, "Guider");
        node("chunks", "ViggleChunkedSampler", {{"guider", L("guide")}, {"sampler", L("ksel")}, {"sigmas", L("sig")}, {"cond_set", L("wcond")}, {"vae", L("vae")},
                                                {"seed", 42}, {"rerender_chunk", 0}, {"rerender_seed", 0}}, "Sample each window");
        images = L("chunks");
    }
    node("save", "VHS_VideoCombine", {{"images", images}, {"audio", L("vid", 2)}, {"frame_rate", 24}, {"loop_count", 0}, {"filename_prefix", "video/" + outName},
                                      {"format", "video/h264-mp4"}, {"pix_fmt", "yuv420p"}, {"crf", 16}, {"save_metadata", false}, {"trim_to_audio", false},
                                      {"pingpong", false}, {"save_output", true}}, "The video, with the clip's own sound");
    return g;
}

// "Turn X into the one from image 2, in place" - not "replace X with": on a close-up cropped at the chest, a replace made a small doll of
// Peach on the girl's top; turned in place, her face, hair and top became Peach's where they were (2026-10-29)
std::string ViggleRepaintPrompt(const std::string& who, const std::string& looks) {
    std::string l = looks;
    while (!l.empty() && (l.back() == '.' || l.back() == ' ' || l.back() == '\n')) l.pop_back();
    return "Turn " + who + " in image 1 into the one from image 2" + (l.empty() ? std::string() : " (" + l + ")") +
           ", in place: their face, head and hair, their body and clothes become that one's, with its own look - a photographed person stays "
           "photographic, a 3D animated or cartoon character stays that animated character. They stay exactly where " + who + " is, the same size "
           "and pose, turned the same way, with exactly two arms and two hands. Keep everything else in image 1 exactly as it is: anyone else in "
           "it, the place, the camera, the framing and the light.";
}

// The wordings that worked (2026-10-29, FLUX 3 Klein): "Turn the girl in front in image 1 into the 3D animated character from image 2, in
// place: her face becomes that character's 3D animated face with its big blue eyes, her hair becomes that character's long blonde hair with the
// gold crown, and her black top becomes the white strapless wedding dress. Her head stays exactly where it is..." - what she has now, and what
// it becomes, part by part. The writer sees both pictures, so it can name both.
std::string ViggleWriteEdit(const std::wstring& frame, const std::wstring& picture, const std::string& who, const std::string& looks, const std::string& comfyUrl) {
    if (!WriterConfigured()) return {};
    const std::string a = ImageDataUrl(frame, 1024), b = ImageDataUrl(picture, 768);
    if (a.empty() || b.empty()) return {};
    const ClefSession session = WriterAcquire(comfyUrl, [](const std::string&) {});
    if (!session.ok) {
        WriterRelease(session, false);
        return {};
    }
    const std::string system =
        "You write ONE instruction for an image editor that repaints one person in a video frame as another character. Image 1 is the frame, image 2 "
        "the character. The instruction:\n"
        "1. Starts: \"Turn <the person, by what they wear and where they are in the frame> in image 1 into <the character by name and kind, e.g. the 3D "
        "animated character Princess Peach> from image 2, in place:\"\n"
        "2. Then part by part, what the person has NOW and what it becomes, all visible parts: the face (\"her face becomes Peach's 3D animated face "
        "with big blue eyes\"), the hair and anything on the head, each piece of clothing (\"her black oversized t-shirt becomes the white strapless "
        "wedding dress\", shorts, shoes), visible skin and hands.\n"
        "3. Then: \"<She/He> stays exactly where <she/he> is, the same size, in the same pose (<the pose in a few words>), turned the same way, with exactly "
        "two arms and two hands, rendered as in image 2\" and say how (a 3D animated character stays 3D animated, a photographed person stays "
        "photographic, a drawing stays drawn).\n"
        "4. Ends: \"Keep everything else in image 1 exactly as it is: <anyone else in it, briefly>, the place, the camera, the framing and the light.\"\n"
        "Name the character only when you are sure who it is (a wrong name paints the wrong character: Bowser was once called King K. Rool); "
        "otherwise call it \"the character from image 2\" and let the parts say what it looks like. Only the instruction, under 130 words, no "
        "quotes around it.";
    const std::string user = "Repaint: " + who + "." + (looks.empty() ? std::string() : " The character (use this name): " + looks.substr(0, 300));
    std::string out, err;
    const bool ok = WriterChat(system, user, {a, b}, 400, 0.3, out, &err);
    WriterRelease(session, false);
    if (!ok) return {};
    const size_t s = out.find_first_not_of(" \t\r\n\""), e = out.find_last_not_of(" \t\r\n\"");
    const std::string t = s == std::string::npos ? std::string() : out.substr(s, e - s + 1);
    return t.find("image 1") != std::string::npos ? t : std::string(); // (an answer that is not the instruction: the fixed wording instead)
}

json BuildMossRepaint(const std::string& find, const std::string& prompt) {
    json g;
    auto node = [&](const std::string& id, const char* cls, json inputs, const char* title) {
        g[id] = {{"class_type", cls}, {"inputs", std::move(inputs)}, {"_meta", {{"title", title}}}};
    };
    auto L = [](const std::string& id, int k = 0) { return json::array({id, k}); };
    node("1", "UNETLoader", {{"unet_name", "moss_image_sft_int8_convrot.safetensors"}, {"weight_dtype", "default"}}, "MOSS-Image SFT");
    node("11", "SkipLayerGuidanceDiTSimple", {{"model", L("1")}, {"double_layers", ""}, {"single_layers", "22"}, {"start_percent", 0.0}, {"end_percent", 1.0}},
         "Its negative branch without layer 22");
    node("2", "CLIPLoader", {{"clip_name", "qwen3.5_9b_w4a8_gptq.safetensors"}, {"type", "moss_image"}, {"device", "default"}}, "Its text encoder");
    node("3", "VAELoader", {{"vae_name", "flux2-vae.safetensors"}}, "VAE");
    node("5", "CLIPTextEncode", {{"clip", L("2")}, {"text", prompt}}, "Who they become");
    node("6", "CLIPTextEncode", {{"clip", L("2")}, {"text", ""}}, "Empty negative");
    node("20", "LoadImage", {{"image", "@frame"}}, "The frame");
    node("21", "ImageScaleToTotalPixels", {{"image", L("20")}, {"upscale_method", "lanczos"}, {"megapixels", 1.0}, {"resolution_steps", 16}}, "About 1 MP");
    node("40", "CheckpointLoaderSimple", {{"ckpt_name", "sam3.1_multiplex_fp16.safetensors"}}, "SAM 3.1");
    node("41", "CLIPTextEncode", {{"clip", L("40", 1)}, {"text", find}}, "Whom SAM finds");
    node("42", "SAM3_Detect", {{"model", L("40")}, {"image", L("21")}, {"conditioning", L("41")}, {"threshold", 0.4}, {"refine_iterations", 2}, {"individual_masks", false}},
         "Their mask");
    node("43", "GrowMask", {{"mask", L("42")}, {"expand", 24}, {"tapered_corners", true}}, "Grown (a differently built figure fits)");
    node("24", "VAEEncode", {{"pixels", L("21")}, {"vae", L("3")}}, "Encode");
    node("25", "SetLatentNoiseMask", {{"samples", L("24")}, {"mask", L("43")}}, "Only there");
    node("8", "KSampler", {{"model", L("11")}, {"positive", L("5")}, {"negative", L("6")}, {"latent_image", L("25")}, {"seed", 7}, {"steps", 25}, {"cfg", 3.0},
                           {"sampler_name", "euler"}, {"scheduler", "simple"}, {"denoise", 1.0}}, "MOSS's Best: 25 steps, CFG 3");
    node("9", "VAEDecode", {{"samples", L("8")}, {"vae", L("3")}}, "Decode");
    node("26", "ImageCompositeMasked", {{"destination", L("21")}, {"source", L("9")}, {"x", 0}, {"y", 0}, {"resize_source", false}, {"mask", L("43")}},
         "The rest as it was");
    node("10", "SaveImage", {{"images", L("26")}, {"filename_prefix", "Viggle/moss_repaint"}}, "Save");
    return g;
}

std::string ViggleWriteMoss(const std::wstring& frame, const std::wstring& picture, const std::string& who, const std::string& looks, const std::string& comfyUrl) {
    if (!WriterConfigured()) return {};
    const std::string a = ImageDataUrl(frame, 1024), b = ImageDataUrl(picture, 768);
    if (a.empty() || b.empty()) return {};
    const ClefSession session = WriterAcquire(comfyUrl, [](const std::string&) {});
    if (!session.ok) {
        WriterRelease(session, false);
        return {};
    }
    const std::string system =
        "You write the prompt for an inpainting model that redraws ONE masked person in a video frame as another character. It cannot see the "
        "character's picture: your words are all it gets. Image 1 is the frame, image 2 the character. One paragraph: the character's name only when "
        "you are sure who it is (a wrong name draws the wrong one), what kind it is (a 3D animated character, a photographed person, a cartoon), then "
        "its face, eyes, hair and anything on the head, its body, skin and every piece of clothing with colours, exactly as in image 2; then the pose of "
        "the person in image 1 (how they stand or move, where their arms are), how much is in view (full body, from the waist up), and the place and "
        "light of image 1 in a few words. Face first: always describe the face. Under 90 words. Only the prompt, no quotes.";
    std::string out, err;
    const bool ok = WriterChat(system, "The person to redraw: " + who + "." + (looks.empty() ? std::string() : " The character (use this name): " + looks.substr(0, 300)),
                               {a, b}, 300, 0.3, out, &err);
    WriterRelease(session, false);
    if (!ok) return {};
    const size_t s = out.find_first_not_of(" \t\r\n\""), e = out.find_last_not_of(" \t\r\n\"");
    return s == std::string::npos ? std::string() : out.substr(s, e - s + 1);
}

void StartViggleSetup() {
    const fs::path script = fs::path(ExeDir()) / L"tools" / L"viggle" / L"setup_viggle.ps1";
    const std::wstring models = ComfyModelsDir(), install = ComfyInstallDir();
    std::error_code ec;
    std::lock_guard<std::mutex> lock(g_setupMutex);
    if (g_setupRunning) return;
    if (models.empty() || install.empty() || !fs::exists(script, ec)) {
        g_setupLine = models.empty() || install.empty() ? "error: ComfyUI was not found" : "error: tools\\viggle\\setup_viggle.ps1 is missing";
        return;
    }
    fs::create_directories(DataDir(), ec);
    g_setupLog = DataDir() + L"\\viggle_setup.log";
    fs::remove(g_setupLog, ec);
    g_setupLine = "starting";
    g_setupRunning = true;
    const std::wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + script.wstring() + L"\" -Models \"" + models + L"\" -Install \"" + install +
                             L"\" -Log \"" + g_setupLog + L"\"";
    std::thread([cmd]() {
        std::string out;
        RunCaptureHidden(cmd, out, true);
        g_setupRunning = false;
    }).detach();
}

bool ViggleSetupRunning() { return g_setupRunning; }

std::string ViggleSetupLine() {
    static double readAt = 0;
    std::lock_guard<std::mutex> lock(g_setupMutex);
    if (g_setupLog.empty()) return g_setupLine;
    const double now = GetTickCount64() / 1000.0;
    if (now - readAt > 0.5) {
        readAt = now;
        std::ifstream f(g_setupLog);
        std::string l, last;
        while (std::getline(f, l)) {
            if (l.rfind("\xEF\xBB\xBF", 0) == 0) l = l.substr(3);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            if (!l.empty()) last = l;
        }
        if (!last.empty()) g_setupLine = last;
    }
    return g_setupLine;
}
