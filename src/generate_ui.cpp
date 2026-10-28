// Generate (the page that was "ComfyUI"): one place to make pictures, designs and videos from a prompt. Pick who makes it - Krea 2 (people,
// photographs), Qwen Image 2.1 (creatures and unusual looks), MOSS-Image (fast, and the only one that writes many words right), your own
// ComfyUI workflow, or a video (MiniMax H3) - the shape and how many; add pictures and it becomes a Qwen Image Edit of them. What is made shows
// below, in the results browser that was the ComfyUI tab's.
// Why these: compared on the same prompts and seeds (2026-10-16/17, RTX 3090) Krea 2 was the most reliable for people, complex scenes and most
// art styles (7.3 s); Qwen followed creatures and caricatures best (10-12 s); MOSS 8-step was fastest (3.4 s) with the cleanest text.
#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <future>
#include <string>
#include <vector>
#include <windows.h>

#include "app_state.h"
#include "clef.h"
#include "comfy.h"
#include "imgui.h"
#include "json.hpp"
#include "notify.h"
#include "ui_extra.h"
#include "ui_theme.h"
#include "win_dialogs.h"

using json = nlohmann::json;

namespace {

enum Engine { kAuto, kKrea, kQwen, kMoss, kWorkflow, kVideo, kEngineCount };
const char* const kEngineName[] = {"Auto", "Krea 2", "Qwen 2.1", "MOSS", "My workflow", "Video"};
const char* const kEngineAbout[] = {
    "Krea 2 for people and most pictures, Qwen 2.1 when it is a creature, a caricature or an extreme look (as the Characters tab does)",
    "Real-looking people, photographs, most art styles, busy scenes (about 7 s)",
    "Creatures, caricatures, extreme looks: follows unusual descriptions best (about 11 s)",
    "Fastest (about 4 s) and writes words right: signs, titles, captions. For a cover or a poster with many lines, use Design with exact text",
    "Your own ComfyUI workflow, with this prompt (choose it under ComfyUI below)",
    "A video with sound (MiniMax H3 Turbo, 4 steps); with a picture added, it starts from it",
};

struct Shape {
    const char* name;
    int w, h;  // Krea / MOSS (multiples of 16, about a megapixel)
    int ax, ay; // the aspect for Qwen
};
const Shape kShapes[] = {{"Portrait 3:4", 880, 1184, 3, 4}, {"Square", 1024, 1024, 1, 1}, {"Landscape 4:3", 1184, 880, 4, 3},
                         {"Wide 16:9", 1360, 768, 16, 9}, {"Tall 9:16", 768, 1360, 9, 16}};
constexpr int kShapeCount = static_cast<int>(sizeof(kShapes) / sizeof(kShapes[0]));

struct Gen {
    std::string prompt, before; // before: the prompt as it was before the writer improved it (undo)
    std::vector<std::wstring> refs;
    int engine = kAuto, shape = 0, times = 1, seconds = 5;
    bool newPicture = false; // with pictures: make a new picture from them (else picture 1 is changed)
    std::future<std::string> improveFut;
    std::string note;
    bool focus = false;
    // many at once: a theme -> the writer writes N different prompts -> each is made
    std::string theme;
    int many = 8;
    std::future<std::string> manyFut;
    std::vector<std::string> manyPrompts; // the last ones written
};
Gen g_gen;

int StrResize(ImGuiInputTextCallbackData* d) {
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* s = static_cast<std::string*>(d->UserData);
        s->resize(static_cast<size_t>(d->BufTextLen));
        d->Buf = s->data();
    }
    return 0;
}

// Word wrap for a text box (this ImGui's text boxes have none: a long prompt ran off to the right as one line). Only spaces and line breaks are
// swapped, so the text keeps its length and the cursor never jumps; an image model reads a line break as a space, and they are spaces again
// when it is sent.
void Reflow(char* buf, int len, float width) {
    for (int i = 0; i < len; ++i) {
        if (buf[i] == '\n') buf[i] = ' ';
    }
    int lineStart = 0, lastSpace = -1;
    for (int i = 0; i <= len; ++i) {
        if (i < len && buf[i] != ' ') continue;
        if (lastSpace > lineStart && ImGui::CalcTextSize(buf + lineStart, buf + i).x > width) {
            buf[lastSpace] = '\n';
            lineStart = lastSpace + 1;
        }
        lastSpace = i;
    }
}

float g_wrapW = 400.0f;
int WrapCallback(ImGuiInputTextCallbackData* d) {
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) return StrResize(d);
    if (d->EventFlag == ImGuiInputTextFlags_CallbackEdit || d->EventFlag == ImGuiInputTextFlags_CallbackAlways) {
        static float lastW = -1.0f;
        if (d->EventFlag == ImGuiInputTextFlags_CallbackAlways && lastW == g_wrapW) return 0; // (only when the box got wider or narrower)
        lastW = g_wrapW;
        std::string before(d->Buf, static_cast<size_t>(d->BufTextLen));
        Reflow(d->Buf, d->BufTextLen, g_wrapW);
        if (std::memcmp(before.data(), d->Buf, before.size()) != 0) d->BufDirty = true;
    }
    return 0;
}

bool WrappedBox(const char* id, std::string& text, ImVec2 size) {
    g_wrapW = std::max(40.0f, size.x - ImGui::GetStyle().FramePadding.x * 2.0f - ImGui::GetStyle().ScrollbarSize - 4.0f);
    // text put in from outside (the writer, a picture's prompt) or a new width: wrapped before it is shown
    static std::map<ImGuiID, std::pair<std::string, float>> seen;
    auto& last = seen[ImGui::GetID(id)];
    if (last.first != text || last.second != g_wrapW) {
        Reflow(text.data(), static_cast<int>(text.size()), g_wrapW);
        last = {text, g_wrapW};
    }
    const bool changed = ImGui::InputTextMultiline(id, text.data(), text.capacity() + 1, size,
                                                   ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_CallbackEdit | ImGuiInputTextFlags_CallbackAlways,
                                                   WrapCallback, &text);
    if (changed) last = {text, g_wrapW};
    return changed;
}

std::string OneLine(std::string s) { // (the wrap's line breaks are spaces to the model)
    for (char& c : s) {
        if (c == '\n' || c == '\r') c = ' ';
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

std::string LabelOf(const std::string& prompt) {
    std::string label = prompt.substr(0, 48);
    for (char& c : label) {
        if (c == '\n') c = ' ';
    }
    return prompt.size() > 48 ? label + "..." : label;
}

bool Chip(const char* label, bool on) {
    ImGui::PushStyleColor(ImGuiCol_Button, on ? ui::ChipActiveColor() : ui::ChipColor());
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::ChipHoverColor());
    const bool hit = ImGui::SmallButton(label);
    ImGui::PopStyleColor(2);
    return hit;
}

// The writer turns a short idea into a full prompt for the chosen maker (its own words kept: names, quoted text)
const char* const kImproveSystem =
    "You turn a short idea into one prompt for an image model. Keep everything the user asked for: subjects, how many, who does what, style, "
    "and any words in quotes exactly as written (they will be printed in the picture). Add what makes a picture specific: the subject's look, "
    "the setting, the camera and framing, the lighting, the colours, the medium or style, the mood. One rich paragraph in English (quoted words "
    "stay in their language), no lists, no quality tags, no explanations. Answer with the prompt only.";

std::string Improve(std::string comfyUrl, std::string idea, std::string engine) {
    const ClefSession session = WriterAcquire(comfyUrl, [](const std::string&) {});
    if (!session.ok) return "error: the prompt writer could not be started (Clef mode tab > Prompt writer)";
    std::string reply, err;
    const bool ok = WriterChat(kImproveSystem, "The image model: " + engine + ".\nThe idea: " + idea, {}, 900, 0.6, reply, &err);
    WriterRelease(session, false);
    if (!ok) return "error: the prompt writer did not answer" + (err.empty() ? std::string() : ": " + err);
    while (!reply.empty() && (reply.back() == '\n' || reply.back() == ' ')) reply.pop_back();
    if (reply.size() >= 2 && reply.front() == '"' && reply.back() == '"') reply = reply.substr(1, reply.size() - 2);
    return reply;
}

// Auto: Qwen for creatures, caricatures and extreme looks (it follows them far better: a frog-man's head grows out of the body, "absurdly long
// legs" really are), Krea for everything else (it was best for people, busy scenes and most art styles)
int AutoEngine(const std::string& prompt) {
    std::string low = prompt;
    for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const char* const kUnusual[] = {"creature", "monster", "dragon", "alien", "goblin", "orc", "troll", "demon", "beast", "kaiju", "mermaid",
                                           "centaur", "werewolf", "zombie", "insect", "humanoid", "anthropomorphic", "caricature", "-headed", " head of a",
                                           "frog", "lizard", "robot", "cyborg", "mutant", "tentacle", "absurdly", "exaggerated", "grotesque", "chibi"};
    for (const char* w : kUnusual) {
        if (low.find(w) != std::string::npos) return kQwen;
    }
    return GenerateKreaModel().empty() ? kQwen : kKrea;
}

// One prompt, `times` pictures, by this maker (Auto decides here); false when it could not be sent (g_gen.note says why)
} // namespace

bool PromptBox(const char* id, std::string& text, ImVec2 size) { return WrappedBox(id, text, size); }
std::string PromptText(const std::string& boxText) { return OneLine(boxText); }

int SendKreaPicture(AppState& app, const std::string& prompt, int w, int h, int times, const std::string& savePrefix, const std::vector<LoraEntry>& loras,
                    long long seed) {
    const std::string krea = GenerateKreaModel();
    if (krea.empty()) return 0;
    const json g = {
        {"1", {{"class_type", "UNETLoader"}, {"inputs", {{"unet_name", krea}, {"weight_dtype", "default"}}}}},
        {"2", {{"class_type", "CLIPLoader"}, {"inputs", {{"clip_name", "qwen3vl_4b_fp8_scaled.safetensors"}, {"type", "krea2"}, {"device", "default"}}}}},
        {"3", {{"class_type", "VAELoader"}, {"inputs", {{"vae_name", "qwen_image_vae.safetensors"}}}}},
        {"4", {{"class_type", "CLIPTextEncode"}, {"inputs", {{"clip", json::array({"2", 0})}, {"text", prompt}}}}},
        {"5", {{"class_type", "ConditioningZeroOut"}, {"inputs", {{"conditioning", json::array({"4", 0})}}}}},
        {"6", {{"class_type", "EmptyLatentImage"}, {"inputs", {{"width", w}, {"height", h}, {"batch_size", 1}}}}},
        {"7", {{"class_type", "KSampler"}, {"inputs", {{"model", json::array({"1", 0})}, {"positive", json::array({"4", 0})}, {"negative", json::array({"5", 0})},
                                                      {"latent_image", json::array({"6", 0})}, {"seed", 1}, {"steps", 8}, {"cfg", 1.0}, {"sampler_name", "euler"},
                                                      {"scheduler", "simple"}, {"denoise", 1.0}}}}},
        {"8", {{"class_type", "VAEDecode"}, {"inputs", {{"samples", json::array({"7", 0})}, {"vae", json::array({"3", 0})}}}}},
        {"9", {{"class_type", "SaveImage"}, {"inputs", {{"images", json::array({"8", 0})}, {"filename_prefix", savePrefix}}}}},
    };
    ComfyJob job;
    job.apiWorkflow = g.dump();
    job.label = "Krea 2: " + LabelOf(prompt);
    job.prompt = prompt;
    job.times = std::max(1, times);
    job.loras = loras;
    job.seed = seed;
    return SendComfyJob(app, std::move(job));
}

int SendQwenPicture(AppState& app, const std::string& prompt, int ax, int ay, int times, const std::string& savePrefix, const std::vector<LoraEntry>& loras,
                    long long seed) {
    ComfyJob job;
    job.label = "Qwen 2.1: " + LabelOf(prompt);
    job.savePrefix = savePrefix;
    job.draw = 3;
    job.prompt = prompt;
    job.pixW = ax, job.pixH = ay;
    job.times = std::max(1, times);
    job.loras = loras;
    job.seed = seed;
    return SendSheetEdit(app, std::move(job));
}

namespace {

bool MakeOne(AppState& app, const std::string& promptIn, int engine, int times, std::string& by) {
    const std::string prompt = OneLine(promptIn);
    if (prompt.empty()) {
        g_gen.note = "Write what to make first";
        return false;
    }
    const std::string label = LabelOf(prompt);
    const Shape& sh = kShapes[std::clamp(g_gen.shape, 0, kShapeCount - 1)];
    times = std::clamp(times, 1, 16);
    if (engine == kAuto) engine = AutoEngine(prompt);
    if (engine == kVideo) {
        SendH3Auto(app, g_gen.refs.empty() ? std::wstring() : g_gen.refs[0], prompt, g_gen.seconds, label, times); // (with the H3 LoRAs: every H3 video has them)
        by = "MiniMax H3";
        return true;
    }
    if (!g_gen.refs.empty()) { // with pictures: a Qwen Image 2.1 edit of them
        EditContext ctx;
        ctx.canvas = !g_gen.newPicture ? 0 : sh.ax > sh.ay ? 1 : sh.ax == sh.ay ? 2 : 3;
        by = "Qwen Image 2.1 Edit";
        return SendPictureEdit(app, g_gen.refs, prompt, label, times, &ctx);
    }
    if (engine == kKrea) {
        if (GenerateKreaModel().empty()) {
            g_gen.note = "No Krea 2 model in ComfyUI's diffusion_models (with qwen3vl_4b_fp8_scaled and qwen_image_vae)";
            return false;
        }
        by = "Krea 2";
        return SendKreaPicture(app, WithLoraTriggers(app, prompt, app.comfy.kreaLoras), sh.w, sh.h, times, "Generate/krea", app.comfy.kreaLoras) != 0;
    }
    if (engine == kQwen) {
        by = "Qwen Image 2.1";
        return SendQwenPicture(app, WithLoraTriggers(app, prompt, app.comfy.qwenLoras), sh.ax, sh.ay, times, "Generate/qwen", app.comfy.qwenLoras) != 0;
    }
    if (engine == kMoss) {
        if (!MossReady(app)) {
            g_gen.note = "MOSS-Image is not in ComfyUI yet: Edits > Designs & box edits has its download button";
            return false;
        }
        by = "MOSS-Image";
        return SendMossPrompt(app, WithLoraTriggers(app, prompt, app.comfy.mossLoras), sh.w, sh.h, times, std::string(), app.comfy.mossLoras) != 0;
    }
    by = "your workflow";
    return SendToComfy(app, prompt, std::string(), std::wstring(), label, false, 0.0, times);
}

void Make(AppState& app) {
    std::string by;
    const int times = std::clamp(g_gen.times, 1, 16);
    if (MakeOne(app, g_gen.prompt, g_gen.engine, times, by)) g_gen.note = "Sent to " + by + (times > 1 ? " (" + std::to_string(times) + " pictures)" : std::string());
}

// Many at once: the writer turns a theme into N different prompts
const char* const kManySystem =
    "You write prompts for an image model. From the user's theme, write the number of prompts asked for, each a different picture: vary the "
    "subjects, places, moments, framing, time of day, lighting and mood within the theme (keep whatever the theme fixes: a style, a character, a "
    "palette). Each prompt is one rich paragraph in English describing what is seen: subject, look, setting, camera and framing, light, colours, "
    "medium or style. Words in quotes in the theme are printed exactly. No numbering, no titles, no quality tags. Answer with ONLY a JSON array of "
    "strings.";

std::string WriteMany(std::string comfyUrl, std::string theme, int n) {
    const ClefSession session = WriterAcquire(comfyUrl, [](const std::string&) {});
    if (!session.ok) return "error: the prompt writer could not be started (Clef mode tab > Prompt writer)";
    json all = json::array();
    std::string err;
    for (int tries = 0; static_cast<int>(all.size()) < n && tries < (n + 7) / 8 + 2; ++tries) { // in batches of 8 (a small model keeps them apart)
        const int want = std::min(8, n - static_cast<int>(all.size()));
        std::string user = "The theme: " + theme + "\nWrite " + std::to_string(want) + " prompts.";
        if (!all.empty()) user += " They must differ from these already written:\n" + all.dump(1);
        std::string reply;
        if (!WriterChat(kManySystem, user, {}, 600 * want + 400, 0.9, reply, &err)) break;
        const size_t a = reply.find('['), b = reply.rfind(']');
        const json j = a == std::string::npos || b == std::string::npos ? json() : json::parse(reply.substr(a, b - a + 1), nullptr, false);
        if (!j.is_array()) continue;
        for (const json& p : j) {
            if (p.is_string() && p.get<std::string>().size() > 12 && static_cast<int>(all.size()) < n) all.push_back(p);
        }
    }
    WriterRelease(session, false);
    if (all.empty()) return "error: the prompt writer did not write any prompts" + (err.empty() ? std::string() : ": " + err);
    return all.dump();
}

void AddRef(const std::wstring& p) {
    if (p.empty() || g_gen.refs.size() >= 10) return;
    if (std::find(g_gen.refs.begin(), g_gen.refs.end(), p) == g_gen.refs.end()) g_gen.refs.push_back(p);
}

} // namespace

bool GenerateAgain(AppState& app, const std::string& prompt, const std::string& label, int times) {
    int engine = kWorkflow;
    if (label.rfind("Krea 2: ", 0) == 0) engine = kKrea;
    else if (label.rfind("Qwen 2.1: ", 0) == 0) engine = kQwen;
    else if (label.rfind("MOSS: ", 0) == 0) engine = kMoss;
    std::string by;
    const std::vector<std::wstring> keep = g_gen.refs;
    g_gen.refs.clear(); // (the composer's pictures are not this one's)
    const bool ok = MakeOne(app, prompt, engine, times, by);
    g_gen.refs = keep;
    return ok;
}

void GenerateWithPrompt(AppState& app, const std::string& prompt, const std::vector<std::wstring>& refs) {
    g_gen.prompt = prompt;
    g_gen.refs = refs;
    g_gen.focus = true;
    app.requestTab = 4;
}

// The top of the Generate page: what to make, by whom, how; Make
void DrawGenerateComposer(AppState& app) {
    { // tests: MV_GEN_PROMPT (+ MV_GEN_ENGINE 0-4, MV_GEN_TIMES) fills it in and presses Make once
        static bool done = false;
        wchar_t buf[2048] = {};
        if (!done && GetEnvironmentVariableW(L"MV_GEN_PROMPT", buf, 2048)) {
            done = true;
            const int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
            std::string p(n > 0 ? n - 1 : 0, '\0');
            WideCharToMultiByte(CP_UTF8, 0, buf, -1, p.data(), n, nullptr, nullptr);
            g_gen.prompt = p;
            wchar_t e[8] = {};
            if (GetEnvironmentVariableW(L"MV_GEN_ENGINE", e, 8)) g_gen.engine = std::clamp(_wtoi(e), 0, kEngineCount - 1);
            if (GetEnvironmentVariableW(L"MV_GEN_TIMES", e, 8)) g_gen.times = std::clamp(_wtoi(e), 1, 4);
            if (GetEnvironmentVariableW(L"MV_GEN_MANY", e, 8)) { // MV_GEN_MANY=N: the prompt is a theme for Many at once
                g_gen.theme = p, g_gen.prompt.clear();
                g_gen.many = std::clamp(_wtoi(e), 1, 40);
                g_gen.manyFut = std::async(std::launch::async, WriteMany, app.comfy.url, OneLine(g_gen.theme), g_gen.many);
            } else {
                Make(app);
            }
        }
    }
    if (g_gen.improveFut.valid() && g_gen.improveFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const std::string r = g_gen.improveFut.get();
        if (r.rfind("error: ", 0) == 0) g_gen.note = r.substr(7);
        else if (!r.empty()) g_gen.before = g_gen.prompt, g_gen.prompt = r, g_gen.note = "The writer filled it in (Undo puts yours back)";
    }
    const bool krea = !GenerateKreaModel().empty();
    if (g_gen.engine == kKrea && !krea) g_gen.engine = kAuto;

    ImGui::PushFont(ui::TitleFont());
    ImGui::TextUnformatted("Generate");
    ImGui::PopFont();
    // who makes it
    for (int e = 0; e < kEngineCount; ++e) {
        if (e) ui::SameLineIfFits(kEngineName[e]);
        ImGui::BeginDisabled(e == kKrea && !krea);
        if (Chip(kEngineName[e], g_gen.engine == e)) g_gen.engine = e;
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", kEngineAbout[e]);
    }
    ui::SameLineIfFits("Design with exact text...");
    if (ImGui::SmallButton("Design with exact text...")) OpenLayoutWithIdea(app, g_gen.prompt);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("Covers, posters, thumbnails, memes: MOSS-Image draws it from boxes the prompt writer plans from this idea, which you can move "
                          "and reword (Edits > Layout designs)");
    ImGui::TextDisabled("%s", g_gen.refs.empty() || g_gen.engine == kVideo ? kEngineAbout[g_gen.engine]
                                                                          : "With pictures added: Qwen Image 2.1 Edit changes them as you say (<image1>, <image2>... in the text)");
    // the maker's LoRAs, each at its own strength (one list per model: Create a character uses the same ones)
    if (!g_gen.refs.empty() && g_gen.engine != kVideo) {
        DrawModelLoras(app, app.comfy.editLoras, "Qwen Image 2.1", "glre", "Edit LoRAs:");
    } else if (g_gen.engine == kAuto) {
        if (krea) DrawModelLoras(app, app.comfy.kreaLoras, "Krea 2", "glrk", "Krea 2 LoRAs:");
        DrawModelLoras(app, app.comfy.qwenLoras, "Qwen Image 2.1", "glrq", "Qwen 2.1 LoRAs:");
    } else if (g_gen.engine == kKrea) {
        DrawModelLoras(app, app.comfy.kreaLoras, "Krea 2", "glrk");
    } else if (g_gen.engine == kQwen) {
        DrawModelLoras(app, app.comfy.qwenLoras, "Qwen Image 2.1", "glrq");
    } else if (g_gen.engine == kMoss) {
        DrawModelLoras(app, app.comfy.mossLoras, "MOSS-Image", "glrm");
    } else if (g_gen.engine == kWorkflow) {
        DrawModelLoras(app, app.comfy.promptLoras, "", "glrw", "LoRAs on your workflow:");
    } else if (g_gen.engine == kVideo) {
        DrawModelLoras(app, app.comfy.h3Loras, "MiniMax H3", "glrv");
    }
    // what to make
    if (g_gen.focus) {
        ImGui::SetKeyboardFocusHere();
        g_gen.focus = false;
    }
    const float makeW = ui::Dp(96.0f), boxH = ui::Dp(96.0f);
    WrappedBox("##genprompt", g_gen.prompt, ImVec2(ImGui::GetContentRegionAvail().x - makeW - ImGui::GetStyle().ItemSpacing.x, boxH));
    const bool ctrlEnter = ImGui::IsItemActive() && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Enter, false);
    if (g_gen.prompt.empty() && !ImGui::IsItemActive()) {
        const ImVec2 p = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + ImGui::GetStyle().FramePadding.x, p.y + ImGui::GetStyle().FramePadding.y), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                            g_gen.refs.empty() ? "Describe what to make   (Ctrl+Enter)" : "What to do with the pictures: \"put <image2> into <image1>'s room\"   (Ctrl+Enter)");
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ui::kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::kAccentHover);
    if (ImGui::Button("Make", ImVec2(makeW, boxH)) || ctrlEnter) Make(app);
    ImGui::PopStyleColor(2);
    // how
    if (g_gen.engine != kVideo) {
        for (int s = 0; s < kShapeCount; ++s) {
            if (s) ui::SameLineIfFits(kShapes[s].name);
            if (Chip(kShapes[s].name, g_gen.shape == s)) g_gen.shape = s;
        }
    } else {
        for (int sec : {5, 8, 10}) {
            char lbl[16];
            snprintf(lbl, sizeof(lbl), "%d s##gsec", sec);
            if (sec != 5) ImGui::SameLine(0, ui::Dp(4.0f));
            if (Chip(lbl, g_gen.seconds == sec)) g_gen.seconds = sec;
        }
    }
    ui::SameLineIfFits("x 1 2 4 ");
    ImGui::TextDisabled("x");
    for (int n : {1, 2, 4}) {
        char lbl[8];
        snprintf(lbl, sizeof(lbl), "%d##gn", n);
        ImGui::SameLine(0, ui::Dp(4.0f));
        if (Chip(lbl, g_gen.times == n)) g_gen.times = n;
    }
    ui::SameLineIfFits("Write it out for me");
    ImGui::BeginDisabled(g_gen.improveFut.valid() || g_gen.prompt.empty());
    if (ImGui::SmallButton(g_gen.improveFut.valid() ? "Writing..." : "Write it out for me")) {
        g_gen.note = "The prompt writer is filling it in...";
        g_gen.improveFut = std::async(std::launch::async, Improve, app.comfy.url, g_gen.prompt, std::string(kEngineName[g_gen.engine]));
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("The local prompt writer turns your short idea into a full prompt (look, setting, camera, light, style), keeping your words");
    if (!g_gen.before.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Undo")) g_gen.prompt = g_gen.before, g_gen.before.clear();
    }
    // pictures to use
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Pictures:");
    int remove = -1;
    for (size_t k = 0; k < g_gen.refs.size(); ++k) {
        ImGui::SameLine();
        ImGui::PushID(static_cast<int>(k));
        const AppState::LibThumb lt = app.LibraryThumb(g_gen.refs[k]);
        const float th = ui::Dp(40.0f);
        if (lt.srv) ImGui::Image((ImTextureID)(intptr_t)lt.srv, ImVec2(th, th));
        else ImGui::Button("?", ImVec2(th, th));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("<image%d>. Click: take it out", static_cast<int>(k) + 1);
        if (ImGui::IsItemClicked()) remove = static_cast<int>(k);
        ImGui::PopID();
    }
    if (remove >= 0) g_gen.refs.erase(g_gen.refs.begin() + remove);
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Gallery's picture")) {
        if (app.selectedIndex >= 0 && app.selectedIndex < static_cast<int>(app.entries.size())) AddRef(app.entries[app.selectedIndex].path);
        else g_gen.note = "Pick a picture in the gallery first";
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("+ File...")) AddRef(OpenImageFileDialog(GetActiveWindow()));
    if (!g_gen.refs.empty() && g_gen.engine != kVideo) {
        ImGui::SameLine();
        ImGui::Checkbox("Make a new picture from them", &g_gen.newPicture);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Off: picture 1 is changed. On: a new picture in the shape above, the pictures are only references.");
    }
    if (g_gen.refs.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(none: made from the text; or drag pictures here)");
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("MV_LIBITEM")) AddRef(std::wstring(static_cast<const wchar_t*>(pl->Data)));
        else if (const ImGuiPayload* pg = ImGui::AcceptDragDropPayload("MV_IMAGES")) {
            const int* idx = static_cast<const int*>(pg->Data);
            for (int k = 0; k < static_cast<int>(pg->DataSize / sizeof(int)); ++k) {
                if (idx[k] >= 0 && idx[k] < static_cast<int>(app.entries.size())) AddRef(app.entries[idx[k]].path);
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (!g_gen.note.empty()) ImGui::TextColored(ui::kAccent, "%s", g_gen.note.c_str());

    // ---- many at once: a theme -> the writer writes N different prompts -> each is made by the maker above (Auto decides for each)
    if (g_gen.manyFut.valid() && g_gen.manyFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const std::string r = g_gen.manyFut.get();
        if (r.rfind("error: ", 0) == 0) {
            g_gen.note = r.substr(7);
        } else {
            const json j = json::parse(r, nullptr, false);
            g_gen.manyPrompts.clear();
            int sent = 0;
            std::map<std::string, int> byCount;
            for (const json& p : j) {
                if (!p.is_string()) continue;
                g_gen.manyPrompts.push_back(p.get<std::string>());
                std::string by;
                if (MakeOne(app, p.get<std::string>(), g_gen.engine, 1, by)) ++sent, ++byCount[by];
            }
            std::string who;
            for (const auto& [k, v] : byCount) who += (who.empty() ? "" : ", ") + k + " " + std::to_string(v);
            g_gen.note = "Wrote " + std::to_string(g_gen.manyPrompts.size()) + " prompts and sent " + std::to_string(sent) + (who.empty() ? "" : " (" + who + ")");
        }
    }
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Many at once: a theme becomes many different pictures")) {
        ImGui::TextDisabled("Say the theme (a style, a place, a character, a mood...); the prompt writer writes that many different prompts and each is made "
                            "by the maker chosen above, in the shape above.");
        WrappedBox("##gentheme", g_gen.theme, ImVec2(ImGui::GetContentRegionAvail().x, ui::Dp(56.0f)));
        if (g_gen.theme.empty() && !ImGui::IsItemActive()) {
            const ImVec2 p = ImGui::GetItemRectMin();
            ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + ImGui::GetStyle().FramePadding.x, p.y + ImGui::GetStyle().FramePadding.y), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                                "e.g. cozy ghibli-style villages at different times of day");
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("How many:");
        for (int n : {4, 8, 12, 20, 40}) {
            char lbl[16];
            snprintf(lbl, sizeof(lbl), "%d##many", n);
            ImGui::SameLine(0, ui::Dp(4.0f));
            if (Chip(lbl, g_gen.many == n)) g_gen.many = n;
        }
        ImGui::SameLine(0, ui::Dp(14.0f));
        ImGui::BeginDisabled(g_gen.manyFut.valid() || OneLine(g_gen.theme).empty());
        char go[64];
        snprintf(go, sizeof(go), g_gen.manyFut.valid() ? "Writing the prompts..." : "Write %d and make them", g_gen.many);
        if (ImGui::Button(go)) {
            g_gen.note = "The prompt writer is writing " + std::to_string(g_gen.many) + " prompts...";
            g_gen.manyFut = std::async(std::launch::async, WriteMany, app.comfy.url, OneLine(g_gen.theme), g_gen.many);
        }
        ImGui::EndDisabled();
        if (!g_gen.manyPrompts.empty() && ImGui::TreeNode("The prompts it wrote")) {
            for (size_t i = 0; i < g_gen.manyPrompts.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::SmallButton("Use")) g_gen.prompt = g_gen.manyPrompts[i];
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Put it in the box above (to change it, or make more of it)");
                ImGui::SameLine();
                ImGui::PushTextWrapPos(0);
                ImGui::TextUnformatted(g_gen.manyPrompts[i].c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
    }
}
