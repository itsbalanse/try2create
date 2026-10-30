// Generate (the page that was "ComfyUI"): one place to make pictures, designs and videos from a prompt. Pick who makes it - Krea 2 (people,
// photographs), Qwen Image 2.1 (creatures and unusual looks), MOSS-Image (fast, and the only one that writes many words right), your own
// ComfyUI workflow, or a video (MiniMax H3) - the shape and how many; add pictures and it becomes a Qwen Image Edit of them. What is made shows
// below, in the results browser that was the ComfyUI tab's.
// Why these: compared on the same prompts and seeds (2026-10-16/17, 24 GB card) Krea 2 was the most reliable for people, complex scenes and most
// art styles (7.3 s); Qwen followed creatures and caricatures best (10-12 s); MOSS 8-step was fastest (3.4 s) with the cleanest text.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <future>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>
#include <commdlg.h>

#include "app_state.h"
#include "clef.h"
#include "comfy.h"
#include "imgui.h"
#include "json.hpp"
#include "library_ui.h"
#include "notify.h"
#include "ui_extra.h"
#include "ui_theme.h"
#include "image_texture.h" // DecodeImageRGBA (a pose drawing trimmed)
#include "video_util.h"
#include "vision_prompt.h" // ImageDataUrl
#include "win_dialogs.h"

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

enum Engine { kAuto, kKrea, kQwen, kMoss, kMoody, kWorkflow, kVideo, kEngineCount };
const char* const kEngineName[] = {"Auto", "Krea 2", "Qwen 2.1", "MOSS", "Moody MOSS", "My workflow", "Video"};
const char* const kEngineAbout[] = {
    "Krea 2 for people and most pictures, Qwen 2.1 when it is a creature, a caricature or an extreme look (as the Characters tab does)",
    "Real-looking people, photographs, most art styles, busy scenes (about 7 s)",
    "Creatures, caricatures, extreme looks: follows unusual descriptions best (about 11 s)",
    "Fastest (about 4 s) and writes words right: signs, titles, captions. For a cover or a poster with many lines, use Design with exact text",
    "moodyMossMix, a MOSS-Image finetune: darker, more cinematic light and colour, words still right; its own settings (25 steps, CFG 3.5), about 45 s. "
    "Write it as flowing description (the shot, the people, the place, the light), as MOSS prompts are",
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

// Krea 2's nine official style LoRAs (Comfy-Org/Krea-2 loras/, 0.47 GB each) with the words each was trained on, at 0.8 (Freefield's styles
// app, neuregex/freefield, MIT); and the style reference LoRA: the look of a picture you add on anything you describe
struct KreaStyle {
    const char* name;
    const char* file;
    const char* words;
};
const KreaStyle kStyles[] = {
    {"Soft watercolor", "krea2_softwatercolor.safetensors", "Art Deco watercolor style"},
    {"Ink wash", "krea2_darkbrush.safetensors", "monochrome ink wash style"},
    {"Stippling", "krea2_dotmatrix.safetensors", "Monochrome stippling style"},
    {"Kid's drawing", "krea2_kidsdrawing.safetensors", "naive expressive sketch style"},
    {"Neon drip", "krea2_neondrip.safetensors", "Textured abstract style"},
    {"Rainy window", "krea2_rainywindow.safetensors", "rainy window style"},
    {"Retro anime", "krea2_retroanime.safetensors", "Purple retro anime style"},
    {"Sunset blur", "krea2_sunsetblur.safetensors", "ethereal motion blur style"},
    {"Vintage tarot", "krea2_vintagetarot.safetensors", "vintage tarot style"},
};
constexpr int kKreaStyleCount = static_cast<int>(sizeof(kStyles) / sizeof(kStyles[0]));
const char* const kStyleRefFile = "krea2_style_reference.safetensors";

// The cinema look (Freefield's Cinema Studio): a film still's genre, era, camera body, lens, focal length, aperture, light and colour, woven
// into the prompt. "Any" leaves that part out.
struct CinePick {
    const char* label;
    const char* text;
};
struct CineControl {
    const char* name;
    std::vector<CinePick> picks;
    int start; // what it is set to when the cinema look is turned on
};
const CineControl kCine[] = {
    {"Genre", {{"Any", ""}, {"Drama", " from a drama"}, {"Noir", " from a film noir"}, {"Comedy", " from a comedy"}, {"Horror", " from a horror film"},
               {"Action", " from an action film"}, {"Epic", " from an epic"}, {"Sci-fi", " from a science-fiction film"}, {"Western", " from a western"}}, 0},
    {"Era", {{"Any", ""}, {"1920s", " set in the 1920s"}, {"1950s", " set in the 1950s"}, {"1960s", " set in the 1960s"}, {"1970s", " set in the 1970s"},
             {"1980s", " set in the 1980s"}, {"1990s", " set in the 1990s"}, {"2000s", " set in the 2000s"}, {"Present day", " set in the present day"},
             {"Future", " set in the near future"}}, 0},
    {"Camera", {{"Any", ""}, {"8K digital", "the crisp, clean detail of 8K digital cinema"}, {"Full-frame digital", "the full-frame digital cinema look, natural and smooth"},
                {"70mm film", "the rich detail and fine grain of large-format 70mm film"}, {"Super 35 digital", "the polished Super 35 digital studio look"},
                {"16mm film", "the soft, grainy texture of 16mm film"}, {"Large-format digital", "the smooth, wide-latitude large-format digital look"}}, 2},
    {"Lens", {{"Any", ""}, {"Tilt-shift", "tilt-shift selective focus"}, {"Compact anamorphic", "subtle anamorphic oval bokeh and horizontal flares"},
              {"Extreme macro", "extreme macro detail"}, {"70s cinema prime", "the warm, slightly soft rendering of 1970s cinema glass"},
              {"Classic anamorphic", "a classic anamorphic widescreen look with oval bokeh and blue streak flares"}, {"Modern prime", "clean, sharp modern rendering"},
              {"Warm cinema prime", "warm-toned cinematic rendering"}, {"Swirl bokeh", "swirling vintage bokeh"}, {"Vintage prime", "soft vintage rendering with gentle flare"},
              {"Halation", "a halation glow around the highlights"}, {"Clinical sharp", "ultra-sharp clinical detail"}}, 5},
    {"Focal length", {{"Any", ""}, {"8 mm", "an ultra-wide 8mm perspective"}, {"14 mm", "a wide-angle 14mm perspective"}, {"24 mm", "a dynamic wide 24mm perspective"},
                      {"35 mm", "a natural 35mm perspective"}, {"50 mm", "a standard 50mm perspective"}, {"85 mm", "a classic 85mm portrait perspective"}}, 4},
    {"Aperture", {{"Any", ""}, {"f/1.4", "aperture f/1.4, shallow depth of field, creamy bokeh"}, {"f/4", "aperture f/4, balanced depth of field"},
                  {"f/11", "aperture f/11, deep focus clarity, sharp foreground to background"}}, 1},
    {"Lighting", {{"Any", ""}, {"Cinematic", "cinematic lighting"}, {"Silhouette", "the subject in silhouette against a bright background"},
                  {"Practicals", "lit only by practical lamps visible in the scene"}, {"Window", "soft daylight from a single window at the side"},
                  {"Overhead", "a hard light falling from straight above, deep shadows under the brow"}, {"Backlight", "a strong light behind the subject, a glowing rim and flare"},
                  {"Soft cross", "two soft lights crossing from opposite sides"}, {"Low key", "low-key lighting, mostly shadow, one small key light"},
                  {"Neon", "colored light from neon signs, magenta and cyan"}}, 1},
    {"Colour", {{"Any", ""}, {"Natural", "natural color science"}, {"Teal and orange", "teal shadows and warm orange skin tones"},
                {"Bleach bypass", "desaturated, high-contrast bleach-bypass color"}, {"Warm amber", "warm amber and brown tones"}, {"Cold blue", "cold blue and steel-gray tones"},
                {"Emerald and gold", "deep emerald greens with gold highlights"}, {"Pastel", "soft pastel colors, low contrast"}, {"Neon night", "saturated magenta, cyan and violet"},
                {"Faded film", "faded film colors with lifted blacks"}, {"Monochrome", "black and white"}, {"Sepia", "sepia-toned"}, {"Crimson", "deep crimson reds against black"},
                {"Earth tones", "earthy ochre, olive and rust tones"}}, 1},
};
constexpr int kCineCount = static_cast<int>(sizeof(kCine) / sizeof(kCine[0]));

struct Gen {
    std::string prompt, before; // before: the prompt as it was before the writer improved it (undo)
    std::vector<std::wstring> refs;
    // MOSS / Moody MOSS: a picture whose layout (its depth: placement, scale, pose) it draws on - a photo, a video frame or a pose's mannequin
    std::wstring layout;
    bool layoutDrawn = false; // (a drawn mannequin: the control lets go half-way)
    std::string layoutName;
    std::string poseId, poseWords; // the layout is a library pose: the other makers get it in the writer's words, added by Make
    int posePeople = 1;            // how many people the pose is for (its library: one person, or two)
    std::vector<std::wstring> people; // Generate: people to put in the picture - the prompt writer describes each one's looks, Make adds them
    bool poseAsking = false;       // (the writer is describing it)
    char idea[512] = {};           // Generate's people, no pose: what is happening - the writer turns it into the prompt, a new idea replaces the old prompt
    std::string ideaKey, ideaAsked, ideaPrompt; // what the prompt was written from (idea + people: the people are then in it), what is being written, the result
    std::future<std::string> ideaFut;
    bool ideaWant = false;         // Write it was pressed before the people were described
    bool poseAsWords = false;      // Generate: the pose only in words (a guide), never as a depth map - for Krea 2, Auto and MOSS
    bool makeWaiting = false;      // Make was pressed before the pictures' tags or the pose's words came: it goes on when they do
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
    // the look: a Krea 2 style (or a picture's look), the cinema look
    int style = -1;           // kStyles; kKreaStyleCount = like a picture (styleRef); -1 = none
    std::wstring styleRef;
    float styleStrength = 0.8f;
    bool cinema = false;
    int cine[kCineCount] = {};
    std::future<std::vector<std::string>> lorasFut; // ComfyUI's LoRAs (are the style files there?)
    std::vector<std::string> loras;
    bool lorasAsked = false;
};
// Two of them: Generate's, and Edits > People & outfits' (each its own words, pictures and pose; the same code draws and makes both).
// g_gen is the one being drawn or made: each page's entry points (and callbacks that come back later) point it at theirs first.
Gen g_gens[2];
Gen* g_genCur = &g_gens[0];
#define g_gen (*g_genCur)
bool OnPeoplePage() { return g_genCur == &g_gens[1]; }
bool g_sendingFromGenerate = false; // (Generate's own "My workflow" Make goes through SendToComfy too)

// What each added picture is for (its role), first guessed by the prompt writer looking at it, then yours to change; Make writes the
// instruction from the roles, so a picture of someone plus a pose needs no words at all.
enum Role { kRoleChange, kRoleWho, kRoleOutfit, kRolePlace, kRoleThing, kRoleLook, kRoleCount };
const char* const kRoleName[kRoleCount] = {"change it", "person", "outfit", "place", "thing", "style"};
const char* const kRoleAbout[kRoleCount] = {
    "This picture is changed (the others are used on it); it stays the same picture otherwise",
    "This person or character is in the new picture, with their own face, hair and body",
    "The new picture's person wears these clothes",
    "The new picture is set in this place",
    "This object, animal or creature is in the new picture",
    "The new picture is in this picture's style and look (medium, colours, light)"};
struct RefTag {
    int role = kRoleWho;
    std::string what;              // the writer's few words about it ("a young woman with long red hair in a red dress, 3D render")
    std::future<std::string> fut;  // the writer looking at it
    bool mine = false;             // you chose the role (the writer's guess no longer changes it)
    int target = -1;               // changing a picture: whom this person replaces (kTargets; -1 = by their looks: the man / the woman / the person)
};
std::map<std::wstring, RefTag> g_tags;
enum Target { kTgtPerson, kTgtMan, kTgtWoman, kTgtLeft, kTgtRight, kTgtMiddle, kTgtAdd, kTargetCount };
const char* const kTargetName[kTargetCount] = {"the person", "the man", "the woman", "the person on the left", "the person on the right",
                                               "the person in the middle", "no one (add them)"};

bool TagsPending() { // (a picture not looked at yet counts: its look starts this frame)
    for (const std::wstring& r : g_gen.refs) {
        auto it = g_tags.find(r);
        if (it == g_tags.end() || it->second.fut.valid()) return true;
    }
    return false;
}
} // namespace
int g_genSub = 0; // the page's sub-tab (GenerateSubTab)
namespace {

// The writer says what a picture is: {"kind": ..., "what": ...} ("" = no writer)
std::string TagPicture(std::wstring path, std::string comfyUrl) {
    if (!WriterConfigured()) return {};
    const std::string img = ImageDataUrl(path, 640);
    if (img.empty()) return {};
    const ClefSession session = WriterAcquire(comfyUrl, [](const std::string&) {});
    if (!session.ok) {
        WriterRelease(session, false);
        return {};
    }
    static const char* const kSchema =
        R"({"type":"object","properties":{"kind":{"type":"string","enum":["person","outfit","place","thing"]},"what":{"type":"string","minLength":8,"maxLength":120}},"required":["kind","what"]})";
    std::string out, err;
    const bool ok = WriterChatJson(
        "You label a picture someone adds to an image generator, so it knows what the picture is for.",
        "What is this picture mainly of? kind: person = one or more people or characters (real, drawn, 3D or a creature character) are the subject; "
        "outfit = clothes are the point (shown on their own, laid flat, on a mannequin, or a fashion shot of the outfit); place = a scene, room or "
        "landscape with no main person; thing = an object or an animal. what: at most 14 words - for a person who they look like (man or woman, "
        "age, hair, clothes, and whether it is a photo, a drawing or 3D); for the rest what it is.",
        {img}, kSchema, 120, 0.1, out, &err);
    WriterRelease(session, false);
    return ok ? out : std::string();
}

// Generate's people: the writer puts each one's looks into words (kept while the app runs)
struct PersonWords {
    std::string words;
    std::future<std::string> fut;
};
std::map<std::wstring, PersonWords> g_personWords;

std::string DescribePerson(std::wstring path, std::string comfyUrl) {
    if (!WriterConfigured()) return {};
    const std::string img = ImageDataUrl(path, 768);
    if (img.empty()) return {};
    const ClefSession session = WriterAcquire(comfyUrl, [](const std::string&) {});
    if (!session.ok) {
        WriterRelease(session, false);
        return {};
    }
    std::string out, err;
    const bool ok = WriterChat("You describe a person (or character) from a picture so an image model can draw the same person again in a new picture.",
                               "In one sentence of at most 60 words, starting with \"a\": man or woman, age, skin and ethnicity, face (shape, eyes, "
                               "brows, nose, lips, any beard or makeup), hair (colour, length, style), build, their clothes, and whether they are a real "
                               "photo, a drawing or a 3D character. Only how they look - not the background, not the pose. Just the sentence.",
                               {img}, 200, 0.2, out, &err);
    WriterRelease(session, false);
    if (!ok) return {};
    const size_t s = out.find_first_not_of(" \t\r\n\""), e = out.find_last_not_of(" \t\r\n\"");
    return s == std::string::npos ? std::string() : out.substr(s, e - s + 1);
}

// A two-person pose drawn from its depth (Krea 2, MOSS): the drawing's shape does not say who is who. With the people described and "person 2 is
// the one on the right", Krea 2 drew her standing beside him and someone else in his arms - three people (pose pair60_P054, both seeds, 2026-10-30).
// One short line of who does what to whom, named as the words name them, and no left/right: him carrying her, on every guide and seed.
std::map<std::string, std::string> g_poseActions; // pose id + the words -> the line ("" when the writer could not)
std::future<std::string> g_actionFut;
std::string g_actionKey;

std::string PoseAction(std::wstring drawing, std::string words, std::string comfyUrl) {
    if (!WriterConfigured()) return {};
    const std::string img = ImageDataUrl(drawing, 768);
    if (img.empty()) return {};
    const ClefSession session = WriterAcquire(comfyUrl, [](const std::string&) {});
    if (!session.ok) {
        WriterRelease(session, false);
        return {};
    }
    std::string out, err;
    const bool ok = WriterChat("You say what two people do together in a pose drawing, for an image model that draws them from the drawing's depth.",
                               "The drawing shows two mannequins." + (words.empty() ? std::string(" Name them by their bodies (a man, a woman).")
                                                                                  : " The picture to make: \"" + words + "\" Name the two the way these words "
                                                                                    "name them (\"the man\", \"the woman\", \"the knight\", or \"person 1\" and "
                                                                                    "\"person 2\" when that is how they are named), matched by their bodies.") +
                                   " In one sentence of at most 30 words: who does what to whom - who holds, carries, hugs, leans on or looks at whom, and "
                                   "how (arms, legs, heads). Nothing about clothes, looks, the place or the drawing. Just the sentence.",
                               {img}, 120, 0.2, out, &err);
    WriterRelease(session, false);
    if (!ok) return {};
    const size_t a = out.find_first_not_of(" \t\r\n\""), e = out.find_last_not_of(" \t\r\n\"");
    return a == std::string::npos ? std::string() : out.substr(a, e - a + 1);
}

void PumpAction() {
    if (g_actionFut.valid() && g_actionFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) g_poseActions[g_actionKey] = g_actionFut.get();
}
bool ActionPending() { return g_actionFut.valid(); }

bool PeoplePending() {
    for (const std::wstring& p : g_gen.people) {
        auto it = g_personWords.find(p);
        if (it == g_personWords.end() || it->second.fut.valid()) return true;
    }
    return false;
}

void PumpTags() {
    for (auto& [path, pw] : g_personWords)
        if (pw.fut.valid() && pw.fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) pw.words = pw.fut.get();
    for (auto& [path, t] : g_tags) {
        if (!t.fut.valid() || t.fut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
        const json j = json::parse(t.fut.get(), nullptr, false);
        if (!j.is_object()) continue;
        t.what = j.value("what", std::string());
        if (!t.mine) {
            const std::string k = j.value("kind", std::string());
            t.role = k == "outfit" ? kRoleOutfit : k == "place" ? kRolePlace : k == "thing" ? kRoleThing : kRoleWho;
        }
    }
}

// the style files' download (to ComfyUI's models\loras, by the Set up button)
struct StyleDownload {
    std::atomic<bool> running{false}, failed{false}, finished{false};
    std::atomic<int> done{0}, total{0};
};
StyleDownload g_styleDl;

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
    const bool hit = ImGui::Button(label);
    ImGui::PopStyleColor(2);
    return hit;
}

// The writer turns a short idea into a full prompt for the chosen maker (its own words kept: names, quoted text)
const char* const kImproveSystem =
    "You turn a short idea into one prompt for an image model. Keep everything the user asked for: subjects, how many, who does what, style, "
    "and any words in quotes exactly as written (they will be printed in the picture). Add what makes a picture specific: the subject's look, "
    "the setting, the camera and framing, the lighting, the colours, the medium or style, the mood. One rich paragraph in English (quoted words "
    "stay in their language), no lists, no quality tags, no explanations. Answer with the prompt only.";

// How the writer writes: its own way (kImproveSystem), or a .txt of yours as its instructions - a system prompt such as the Moody MOSS
// prompt guide (moodyMossPromptWriting). The list and the choice are kept in %LOCALAPPDATA%\Try2Create\writer_styles.json.
struct WriterStyles {
    bool loaded = false;
    std::vector<std::wstring> files;
    int chosen = -1; // -1 = the default way
};
WriterStyles g_ws;

std::wstring WriterStylesPath() {
    wchar_t buf[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    return std::wstring(buf) + L"\\Try2Create\\writer_styles.json";
}

std::string WsUtf8(const std::wstring& w) {
    std::string s(static_cast<size_t>(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), static_cast<int>(s.size()), nullptr, nullptr);
    return s;
}
std::wstring WsWide(const std::string& s) {
    std::wstring w(static_cast<size_t>(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), static_cast<int>(w.size()));
    return w;
}

void LoadWriterStyles() {
    g_ws.loaded = true;
    std::ifstream f(WriterStylesPath(), std::ios::binary);
    const json j = f ? json::parse(f, nullptr, false) : json();
    if (!j.is_object()) return;
    if (j.contains("files") && j["files"].is_array())
        for (const json& p : j["files"])
            if (p.is_string()) g_ws.files.push_back(WsWide(p.get<std::string>()));
    g_ws.chosen = std::clamp(j.value("chosen", -1), -1, static_cast<int>(g_ws.files.size()) - 1);
}

void SaveWriterStyles() {
    json j = {{"chosen", g_ws.chosen}, {"files", json::array()}};
    for (const std::wstring& p : g_ws.files) j["files"].push_back(WsUtf8(p));
    std::error_code ec;
    fs::create_directories(fs::path(WriterStylesPath()).parent_path(), ec);
    std::ofstream f(WriterStylesPath(), std::ios::binary | std::ios::trunc);
    if (f) f << j.dump(1);
}

// The chosen file's text (UTF-8, its BOM dropped; at most 40 000 characters), "" = the default way or it cannot be read
std::string ChosenWriterStyle() {
    if (!g_ws.loaded) LoadWriterStyles();
    if (g_ws.chosen < 0 || g_ws.chosen >= static_cast<int>(g_ws.files.size())) return {};
    std::ifstream f(g_ws.files[g_ws.chosen], std::ios::binary);
    if (!f) return {};
    std::string t((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (t.rfind("\xEF\xBB\xBF", 0) == 0) t = t.substr(3);
    if (t.size() > 40000) t.resize(40000);
    return t;
}

std::string Improve(std::string comfyUrl, std::string idea, std::string engine, std::string style) {
    const ClefSession session = WriterAcquire(comfyUrl, [](const std::string&) {});
    if (!session.ok) return "error: the prompt writer could not be started (Clef mode tab > Prompt writer)";
    std::string reply, err;
    // (a guide of yours may ask for several paragraphs: more room to answer)
    const bool ok = style.empty() ? WriterChat(kImproveSystem, "The image model it is for: " + engine + " (do not write its name in the prompt).\nThe idea: " + idea, {}, 900, 0.6, reply, &err)
                                  : WriterChat(style, "The image model it is for: " + engine + " (do not write its name in the prompt).\nThe idea: " + idea, {}, 1600, 0.6, reply, &err);
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

// The cinema look around the prompt: "A cinematic film still from a film noir set in the 1950s: <prompt>. The look: ...; Lighting: ...; Color: ..."
std::string WithCinema(const std::string& prompt) {
    if (!g_gen.cinema) return prompt;
    auto pick = [](int k) { return std::string(kCine[k].picks[std::clamp(g_gen.cine[k], 0, static_cast<int>(kCine[k].picks.size()) - 1)].text); };
    std::string out = "A cinematic film still" + pick(0) + pick(1) + ": " + prompt;
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    std::string look;
    for (int k = 2; k <= 5; ++k) {
        if (!pick(k).empty()) look += (look.empty() ? "" : "; ") + pick(k);
    }
    if (!look.empty()) out += ". The look: " + look;
    if (!pick(6).empty()) out += ". Lighting: " + pick(6);
    if (!pick(7).empty()) out += ". Color: " + pick(7);
    return out + ". High dynamic range, ultra-detailed";
}

bool StyleFileThere(const std::string& file) {
    if (LoraListed(g_gen.loras, file)) return true;
    const std::wstring models = ComfyModelsDir();
    std::error_code ec;
    const std::wstring f(file.begin(), file.end());
    return !models.empty() && (fs::exists(fs::path(models) / L"loras" / f, ec) || fs::exists(fs::path(models) / L"loras" / L"Krea 2" / f, ec)); // (the LoRA sorter's folder)
}

void DownloadStyleFiles(const std::wstring& models, std::vector<std::string> files) {
    if (g_styleDl.running || models.empty() || files.empty()) return;
    g_styleDl.running = true, g_styleDl.failed = false, g_styleDl.finished = false;
    g_styleDl.done = 0, g_styleDl.total = static_cast<int>(files.size());
    std::thread([models, files]() {
        bool ok = true;
        for (const std::string& f : files) {
            std::error_code ec0;
            const fs::path dir = fs::is_directory(fs::path(models) / L"loras" / L"Krea 2", ec0) ? fs::path(models) / L"loras" / L"Krea 2" : fs::path(models) / L"loras",
                           dest = dir / std::wstring(f.begin(), f.end());
            std::error_code ec;
            fs::create_directories(dir, ec);
            if (!fs::exists(dest, ec)) {
                const std::wstring part = dest.wstring() + L".part", url = L"https://huggingface.co/Comfy-Org/Krea-2/resolve/main/loras/" + std::wstring(f.begin(), f.end());
                std::string out;
                const bool good = RunCaptureHidden(L"curl.exe -L --fail --retry 5 --retry-delay 10 -C - --no-progress-meter -o \"" + part + L"\" \"" + url + L"\"", out, true) &&
                                  fs::file_size(part, ec) > 400ull * 1024 * 1024; // (each is about 480 MB)
                if (good) fs::rename(part, dest, ec);
                if (!good || ec) {
                    ok = false;
                    break;
                }
            }
            ++g_styleDl.done;
        }
        g_styleDl.failed = !ok;
        g_styleDl.running = false;
        g_styleDl.finished = true;
    }).detach();
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

// Krea 2 with the style reference LoRA: the look of `look` (its colours, medium, brushwork) on what the prompt describes. The picture goes in
// through Qwen's edit text encoder as a reference latent (index_timestep_zero), as Comfy-Org's Krea 2 style reference workflow does.
int SendKreaStyleRef(AppState& app, const std::string& prompt, const std::wstring& look, int w, int h, int times, const std::string& savePrefix,
                     const std::vector<LoraEntry>& loras) {
    const std::string krea = GenerateKreaModel();
    if (krea.empty() || look.empty()) return 0;
    const json g = {
        {"1", {{"class_type", "UNETLoader"}, {"inputs", {{"unet_name", krea}, {"weight_dtype", "default"}}}}},
        {"2", {{"class_type", "CLIPLoader"}, {"inputs", {{"clip_name", "qwen3vl_4b_fp8_scaled.safetensors"}, {"type", "krea2"}, {"device", "default"}}}}},
        {"3", {{"class_type", "VAELoader"}, {"inputs", {{"vae_name", "qwen_image_vae.safetensors"}}}}},
        {"4", {{"class_type", "LoraLoaderModelOnly"}, {"inputs", {{"model", json::array({"1", 0})}, {"lora_name", kStyleRefFile}, {"strength_model", 1.0}}}}},
        {"5", {{"class_type", "ModelSamplingFlux"}, {"inputs", {{"model", json::array({"4", 0})}, {"width", w}, {"height", h}, {"max_shift", 1.15}, {"base_shift", 0.5}}}}},
        {"6", {{"class_type", "LoadImage"}, {"inputs", {{"image", "@look"}}}}},
        {"7", {{"class_type", "TextEncodeQwenImageEditPlus"}, {"inputs", {{"clip", json::array({"2", 0})}, {"vae", json::array({"3", 0})}, {"image1", json::array({"6", 0})}, {"prompt", prompt}}}}},
        {"8", {{"class_type", "FluxKontextMultiReferenceLatentMethod"}, {"inputs", {{"conditioning", json::array({"7", 0})}, {"reference_latents_method", "index_timestep_zero"}}}}},
        {"9", {{"class_type", "ConditioningZeroOut"}, {"inputs", {{"conditioning", json::array({"8", 0})}}}}},
        {"10", {{"class_type", "CFGGuider"}, {"inputs", {{"model", json::array({"5", 0})}, {"positive", json::array({"8", 0})}, {"negative", json::array({"9", 0})}, {"cfg", 1.0}}}}},
        {"11", {{"class_type", "KSamplerSelect"}, {"inputs", {{"sampler_name", "euler"}}}}},
        {"12", {{"class_type", "BasicScheduler"}, {"inputs", {{"model", json::array({"5", 0})}, {"scheduler", "simple"}, {"steps", 8}, {"denoise", 1.0}}}}},
        {"13", {{"class_type", "EmptyLatentImage"}, {"inputs", {{"width", w}, {"height", h}, {"batch_size", 1}}}}},
        {"14", {{"class_type", "RandomNoise"}, {"inputs", {{"noise_seed", 1}}}}},
        {"15", {{"class_type", "SamplerCustomAdvanced"}, {"inputs", {{"noise", json::array({"14", 0})}, {"guider", json::array({"10", 0})}, {"sampler", json::array({"11", 0})},
                                                                     {"sigmas", json::array({"12", 0})}, {"latent_image", json::array({"13", 0})}}}}},
        {"16", {{"class_type", "VAEDecode"}, {"inputs", {{"samples", json::array({"15", 0})}, {"vae", json::array({"3", 0})}}}}},
        {"17", {{"class_type", "SaveImage"}, {"inputs", {{"images", json::array({"16", 0})}, {"filename_prefix", savePrefix}}}}},
    };
    ComfyJob job;
    job.apiWorkflow = g.dump();
    job.rawUploads = {{"@look", look}};
    job.label = "Krea 2 (a picture's look): " + LabelOf(prompt);
    job.prompt = prompt;
    job.times = std::max(1, times);
    job.loras = loras;
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

bool MakeOne(AppState& app, const std::string& promptIn, int engine, int times, std::string& by, bool look = false) {
    std::string prompt = OneLine(promptIn);
    if (prompt.empty()) {
        g_gen.note = "Write what to make first";
        return false;
    }
    // the look (pictures from text only: an edit keeps its pictures' look, a video has its own prompt)
    const bool picture = engine != kVideo && g_gen.refs.empty();
    if (look && picture) prompt = WithCinema(prompt);
    if (look && picture && g_gen.style >= 0 && (engine == kAuto || engine == kKrea)) { // a Krea 2 style (when Krea 2 makes it): its LoRA, its words
        if (GenerateKreaModel().empty()) {
            g_gen.note = "The styles are Krea 2's: no Krea 2 model in ComfyUI's diffusion_models";
            return false;
        }
        const Shape& sh = kShapes[std::clamp(g_gen.shape, 0, kShapeCount - 1)];
        if (g_gen.style >= kKreaStyleCount) {
            if (g_gen.styleRef.empty()) {
                g_gen.note = "Add the picture whose look to copy (Style: Like a picture)";
                return false;
            }
            by = "Krea 2 with a picture's look";
            return SendKreaStyleRef(app, WithLoraTriggers(app, prompt, app.comfy.kreaLoras), g_gen.styleRef, sh.w, sh.h, std::clamp(times, 1, 16), "Generate/krea_look",
                                    app.comfy.kreaLoras) != 0;
        }
        const KreaStyle& st = kStyles[g_gen.style];
        std::vector<LoraEntry> loras = app.comfy.kreaLoras;
        loras.push_back({st.file, g_gen.styleStrength, true});
        by = std::string("Krea 2, ") + st.name;
        return SendKreaPicture(app, WithLoraTriggers(app, prompt, app.comfy.kreaLoras) + ", " + st.words, sh.w, sh.h, std::clamp(times, 1, 16), "Generate/krea_style",
                               loras) != 0;
    }
    const std::string label = LabelOf(prompt);
    const Shape& sh = kShapes[std::clamp(g_gen.shape, 0, kShapeCount - 1)];
    times = std::clamp(times, 1, 16);
    const bool autoPicks = engine == kAuto;
    if (engine == kAuto) engine = AutoEngine(prompt);
    if (autoPicks && !g_gen.layout.empty() && g_gen.layoutDrawn && engine == kQwen && !KreaDepthControlLora().empty() && !GenerateKreaModel().empty())
        engine = kKrea; // (Auto with a pose drawing: Krea 2 follows its depth; Qwen does not)
    if (engine == kVideo) {
        SendH3Auto(app, g_gen.refs.empty() ? std::wstring() : g_gen.refs[0], prompt, g_gen.seconds, label, times); // (with the H3 LoRAs: every H3 video has them)
        by = "MiniMax H3";
        return true;
    }
    if (!g_gen.refs.empty()) { // with pictures: a Qwen Image 2.1 edit of them
        EditContext ctx;
        ctx.canvas = !g_gen.newPicture ? 0 : sh.ax > sh.ay ? 1 : sh.ax == sh.ay ? 2 : 3;
        by = app.comfy.editEngine == "flux3" ? "FLUX 3 Klein" : "Qwen Image 2.1 Edit";
        return SendPictureEdit(app, g_gen.refs, prompt, label, times, &ctx);
    }
    // Qwen 2.1 and Krea 2 on the layout too, when their depth controls are there (else the pose goes in as words only)
    if (engine == kQwen && !g_gen.layout.empty() && !g_gen.layoutDrawn && !QwenDepthControl().empty()) { // (photos and frames only)
        by = "Qwen Image 2.1, on the layout";
        return SendQwenDepthPrompt(app, WithLoraTriggers(app, prompt, app.comfy.qwenLoras), g_gen.layout, g_gen.layoutDrawn, times, app.comfy.qwenLoras) != 0;
    }
    if (engine == kKrea && !g_gen.layout.empty() && !KreaDepthControlLora().empty() && !GenerateKreaModel().empty()) {
        by = "Krea 2, on the layout";
        return SendKreaDepthPrompt(app, WithLoraTriggers(app, prompt, app.comfy.kreaLoras), g_gen.layout, g_gen.layoutDrawn, times, app.comfy.kreaLoras) != 0;
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
    if ((engine == kMoss || engine == kMoody) && !g_gen.layout.empty()) { // on a picture's layout (its depth)
        if (MossDepthControlLora().empty()) {
            g_gen.note = "The layout needs darask0's MOSS-Image color-depth control (moss-color-depth-control.safetensors) in ComfyUI's loras\\MOSS-Image";
            return false;
        }
        by = engine == kMoody ? "Moody MOSS, on the layout" : "MOSS-Image, on the layout";
        return SendMossLayoutPrompt(app, engine == kMoody, WithLoraTriggers(app, prompt, app.comfy.mossLoras), g_gen.layout, g_gen.layoutDrawn, times,
                                    app.comfy.mossLoras) != 0;
    }
    if (engine == kMoss) {
        if (!MossReady(app)) {
            g_gen.note = "MOSS-Image is not in ComfyUI yet: Edits > Designs & box edits has its download button";
            return false;
        }
        by = "MOSS-Image";
        return SendMossPrompt(app, WithLoraTriggers(app, prompt, app.comfy.mossLoras), sh.w, sh.h, times, std::string(), app.comfy.mossLoras) != 0;
    }
    if (engine == kMoody) {
        if (MoodyMossModel().empty()) {
            g_gen.note = "No Moody MOSS (moodyMossMix...safetensors) in ComfyUI's diffusion_models";
            return false;
        }
        by = "Moody MOSS";
        return SendMoodyMossPrompt(app, WithLoraTriggers(app, prompt, app.comfy.mossLoras), sh.w, sh.h, times, app.comfy.mossLoras) != 0;
    }
    by = "your workflow";
    g_sendingFromGenerate = true; // (its own Make: the box keeps what is in it)
    const bool sent = SendToComfy(app, prompt, std::string(), std::wstring(), label, false, 0.0, times);
    g_sendingFromGenerate = false;
    return sent;
}

// The writer puts the chosen pose into words (Make waits for them when it needs them)
void DescribeGenPose(AppState& app, const LibItem& pose) {
    if (g_gen.poseAsking) return;
    g_gen.poseAsking = true;
    LibraryDescribePose(app, pose, [id = pose.id, gp = g_genCur](AppState&, const std::string& words) {
        g_genCur = gp; // (whose pose it was)
        g_gen.poseAsking = false;
        if (g_gen.poseId != id) return; // (another pose since)
        g_gen.poseWords = words;
        if (words.empty()) {
            g_gen.note = "The prompt writer could not describe the pose (is it running?)";
            g_gen.makeWaiting = false;
        }
    });
}

// The pose's words are wanted when someone other than MOSS makes it (MOSS follows the drawing - when there are no pictures to use)
// (Qwen 2.1 and Krea 2 follow it too when their depth controls are there: then the words are left out - Qwen took "the figure sits..." for a
// mannequin to draw, a white studio and a suit like the drawing's body, where the depth alone gave a firefighter at the station)
bool PoseNeedsWords() {
    const bool qwenDepth = !QwenDepthControl().empty(), kreaDepth = !KreaDepthControlLora().empty();
    // (Qwen only on photos: on a pose drawing's depth it either let go of the pose or drew a body-painted mannequin - tested 2026-10-30 at
    // strengths 0.45-1.0; so a pose goes to Qwen in words. Auto with a pose drawing goes to Krea 2 when its depth control is there.)
    (void)qwenDepth;
    const bool depthDraws = g_gen.refs.empty() && !g_gen.poseAsWords && (g_gen.engine == kMoss || g_gen.engine == kMoody || (g_gen.engine == kKrea && kreaDepth) ||
                                                   (g_gen.engine == kAuto && kreaDepth && !GenerateKreaModel().empty()));
    return !g_gen.poseId.empty() && !(depthDraws && !OnPeoplePage());
}
// ...and a two-person pose drawn from its depth wants its who-does-what line (PoseAction)
bool PoseNeedsAction() { return !OnPeoplePage() && !g_gen.poseId.empty() && g_gen.posePeople >= 2 && !PoseNeedsWords(); }

// Generate's people in words, as Make adds them; n = how many have words
std::string PeopleWords(int& n) {
    std::string who;
    n = 0;
    for (const std::wstring& p : g_gen.people) {
        const std::string& w = g_personWords[p].words;
        if (w.empty()) continue;
        ++n;
        who += " Person " + std::to_string(n) + ": " + w + (w.back() == '.' ? "" : ".");
    }
    if (n == 1) who = who.substr(who.find(':') + 1); // (one person: no number)
    if (n > 0) who = std::string(n == 1 ? "The person:" : "The people:") + who;
    return who;
}

// The idea box: people chosen, no pose, a few words of what is happening. The writer writes the prompt from them; a new idea takes the old prompt's
// place (Undo brings it back), so nothing has to be selected and deleted.
bool IdeaInUse() { return !OnPeoplePage() && !g_gen.people.empty() && g_gen.poseId.empty() && g_gen.engine != kVideo && g_gen.idea[0]; }
std::string IdeaKey() {
    int n = 0;
    return std::string(g_gen.idea) + "|" + PeopleWords(n);
}
void WriteIdea(AppState& app) {
    if (g_gen.ideaFut.valid()) return;
    int n = 0;
    const std::string who = PeopleWords(n);
    g_gen.ideaAsked = IdeaKey();
    g_gen.note = "The prompt writer is writing out your idea...";
    g_gen.ideaFut = std::async(std::launch::async, Improve, app.comfy.url,
                               std::string(g_gen.idea) + (who.empty() ? "" : ". " + who + " Keep them looking exactly like this and say what each one does."),
                               std::string(kEngineName[g_gen.engine]), ChosenWriterStyle());
}
void PumpIdea(AppState& app) {
    if (g_gen.ideaWant && !PeoplePending()) g_gen.ideaWant = false, WriteIdea(app);
    if (g_gen.prompt.empty() && !g_gen.ideaFut.valid()) g_gen.ideaKey.clear();
    if (!g_gen.idea[0] && !g_gen.ideaKey.empty()) { // (the idea taken out: its prompt goes too, unless you changed it)
        if (g_gen.prompt == g_gen.ideaPrompt) g_gen.prompt.clear();
        g_gen.ideaKey.clear();
    }
    if (!g_gen.ideaFut.valid() || g_gen.ideaFut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    const std::string r = g_gen.ideaFut.get();
    if (r.empty() || r.rfind("error: ", 0) == 0) {
        g_gen.note = r.empty() ? "The prompt writer did not answer" : r.substr(7);
        g_gen.makeWaiting = false;
        return;
    }
    if (!g_gen.prompt.empty() && g_gen.prompt != r) g_gen.before = g_gen.prompt;
    g_gen.prompt = r, g_gen.ideaPrompt = r, g_gen.ideaKey = g_gen.ideaAsked;
    g_gen.note = "Your idea, written out (Undo puts the last prompt back)";
}

int RoleOf(const std::wstring& p) {
    auto it = g_tags.find(p);
    return it == g_tags.end() ? static_cast<int>(kRoleWho) : it->second.role;
}
std::string WhatOf(const std::wstring& p) {
    auto it = g_tags.find(p);
    return it == g_tags.end() ? std::string() : it->second.what;
}
std::string JoinAnd(const std::vector<std::string>& v) {
    std::string s;
    for (size_t k = 0; k < v.size(); ++k) s += (k == 0 ? "" : k + 1 == v.size() ? " and " : ", ") + v[k];
    return s;
}

// What Make sends with pictures: the picture to change first (`order`), then a sentence written from the pictures' roles and the pose, and
// your words. Words of yours that name <imageN> themselves are sent as they are (plus the pose).
// Whom a person replaces when a picture is changed: yours, else by their looks (a woman replaces the woman, a man the man)
int TargetOf(const std::wstring& p) {
    auto it = g_tags.find(p);
    if (it != g_tags.end() && it->second.target >= 0) return it->second.target;
    std::string w = " " + WhatOf(p) + " ";
    for (char& c : w) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (w.find("woman") != std::string::npos || w.find("girl") != std::string::npos || w.find("female") != std::string::npos ||
        w.find("princess") != std::string::npos || w.find(" lady") != std::string::npos)
        return kTgtWoman;
    if (w.find(" man ") != std::string::npos || w.find(" man,") != std::string::npos || w.find("boy") != std::string::npos ||
        w.find(" male") != std::string::npos || w.find("guy") != std::string::npos)
        return kTgtMan;
    return kTgtPerson;
}

bool ChangingAPicture() { return !g_gen.refs.empty() && RoleOf(g_gen.refs[0]) == kRoleChange; }

std::string ComposeWithPictures(const std::string& words, std::vector<std::wstring>& order, bool& change) {
    order = g_gen.refs; // (the picture to change is always picture 1)
    change = ChangingAPicture();
    const std::string pose = PoseNeedsWords() ? g_gen.poseWords : std::string();
    if (words.find("<image") != std::string::npos) return words + (pose.empty() ? "" : " Pose: " + pose);
    auto tag = [](size_t k) { return "<image" + std::to_string(k + 1) + ">"; };
    auto with = [&](size_t k) {
        const std::string w = WhatOf(order[k]);
        return tag(k) + (w.empty() ? "" : " (" + w + ")");
    };
    std::vector<std::string> by[kRoleCount];
    for (size_t k = change ? 1 : 0; k < order.size(); ++k) by[RoleOf(order[k])].push_back(with(k));
    const auto& who = by[kRoleWho];
    const auto& outfit = by[kRoleOutfit];
    const auto& place = by[kRolePlace];
    const auto& thing = by[kRoleThing];
    const auto& look = by[kRoleLook];
    std::string said = words.empty() ? std::string() : " " + words + (words.back() == '.' ? "" : ".");
    if (said.size() > 1) said[1] = static_cast<char>(std::toupper(static_cast<unsigned char>(said[1])));
    std::string s;
    if (change) {
        s = "Change " + with(0) + "." + said;
        for (size_t k = 1; k < order.size(); ++k) { // each person: in someone's place, or added
            if (RoleOf(order[k]) != kRoleWho) continue;
            const int t = TargetOf(order[k]);
            if (t == kTgtAdd) s += " Add the person from " + with(k) + " into it, next to who is there, with their own face, hair, body and clothes.";
            else
                s += std::string(" Replace ") + kTargetName[t] + " in " + tag(0) + " with the person from " + with(k) +
                     ": their own face, hair, body and clothes, standing where they stood, in the same pose and light.";
        }
        if (!outfit.empty()) s += " Dress the person in it in exactly the outfit from " + JoinAnd(outfit) + ": every piece, its colours and materials.";
        if (!place.empty()) s += " Move them into the place from " + JoinAnd(place) + ".";
        if (!thing.empty()) s += " Add what is shown in " + JoinAnd(thing) + ".";
        if (!look.empty()) s += " Redo it in the style and look of " + JoinAnd(look) + ".";
        if (!pose.empty()) s += " Pose them: " + pose;
        s += " Keep everything else in " + tag(0) + " as it is: the place, the light, the framing and anyone not replaced.";
        return s;
    }
    s = "A new picture";
    if (!who.empty()) s += " of the " + std::string(who.size() == 1 ? "person" : "people") + " from " + JoinAnd(who);
    else if (!thing.empty()) s += " of what is shown in " + JoinAnd(thing);
    if (!outfit.empty()) s += ", wearing exactly the outfit from " + JoinAnd(outfit);
    if (!place.empty()) s += ", in the place from " + JoinAnd(place);
    if (!who.empty() && !thing.empty()) s += ", with what is shown in " + JoinAnd(thing);
    if (!look.empty()) s += ", in the style and look of " + JoinAnd(look);
    s += "." + said;
    if (!outfit.empty()) s += " Every piece of the outfit, its colours and materials, and nothing of their old clothes.";
    if (!pose.empty()) s += " Pose: " + pose;
    if (!who.empty())
        s += std::string(" They keep their own face, hair and body exactly") + (outfit.empty() ? ", and their clothes" : "") +
             ". The same kind of picture as theirs (a photo stays a photo, a drawn or 3D character stays so).";
    return s;
}

// The same in a few plain words, for the line above Make ("A new picture of the person in 1, in the pose - by FLUX 3 Klein")
std::string PlanLine(AppState& app) {
    if (!OnPeoplePage()) { // Generate: what the people, the pose or the layout do to the picture
        const bool qwenDepth = !QwenDepthControl().empty(), kreaDepth = !KreaDepthControlLora().empty();
        // (follows the layout's depth itself: MOSS, Krea 2 with its control (Auto too, it picks Krea for a pose), Qwen with its control on photos only)
        const bool moss = g_gen.engine == kMoss || g_gen.engine == kMoody || (g_gen.engine == kKrea && kreaDepth) || (g_gen.engine == kAuto && kreaDepth) ||
                          (g_gen.engine == kQwen && qwenDepth && !g_gen.layoutDrawn);
        const size_t n = g_gen.people.size();
        const std::string folks = n == 0 ? std::string() : n == 1 ? "person 1 as they look" : "persons 1 to " + std::to_string(n) + " as they look";
        if (g_gen.layout.empty()) return folks.empty() ? std::string() : (g_gen.idea[0] ? "your idea written out, with " : "your words, with ") + folks + " (the prompt writer describes them; nobody's picture is copied).";
        if (g_gen.poseId.empty())
            return moss ? "the new picture copies the layout of " + g_gen.layoutName.substr(g_gen.layoutName.rfind(' ') + 1) + (folks.empty() ? "." : ", with " + folks + ".")
                        : "this maker cannot copy a layout.";
        const std::string who = g_gen.posePeople >= 2 ? "two people, posed together like the drawing" : "one person, posed like the drawing";
        return "your words, with " + who + (folks.empty() ? "" : " - " + folks) +
               (moss ? std::string(" (") + kEngineName[g_gen.engine] + " follows the drawing's depth, and its words)." : " (the prompt writer puts the pose into words).");
    }
    if (g_gen.refs.empty()) return {};
    const bool change = ChangingAPicture();
    std::vector<std::string> parts[kRoleCount], bits;
    for (size_t k = change ? 1 : 0; k < g_gen.refs.size(); ++k) parts[RoleOf(g_gen.refs[k])].push_back(std::to_string(k + 1));
    auto pics = [](const std::vector<std::string>& v) { return std::string(v.size() == 1 ? "picture " : "pictures ") + JoinAnd(v); };
    std::string s;
    if (change) {
        for (size_t k = 1; k < g_gen.refs.size(); ++k) {
            if (RoleOf(g_gen.refs[k]) != kRoleWho) continue;
            const int t = TargetOf(g_gen.refs[k]);
            bits.push_back(t == kTgtAdd ? "the person from picture " + std::to_string(k + 1) + " is added"
                                        : std::string(kTargetName[t]) + " in it is replaced by the person from picture " + std::to_string(k + 1));
        }
        if (!parts[kRoleOutfit].empty()) bits.push_back("they wear the outfit from " + pics(parts[kRoleOutfit]));
        if (!parts[kRolePlace].empty()) bits.push_back("it moves to the place in " + pics(parts[kRolePlace]));
        if (!parts[kRoleThing].empty()) bits.push_back("what is in " + pics(parts[kRoleThing]) + " is added");
        if (!parts[kRoleLook].empty()) bits.push_back("it takes the style of " + pics(parts[kRoleLook]));
        if (!g_gen.poseId.empty()) bits.push_back("they are posed like the drawing");
        s = "Picture 1 is changed" + (bits.empty() ? std::string(" as your words say") : ": " + JoinAnd(bits)) + ". The rest of it stays as it is.";
    } else {
        std::string shape = kShapes[std::clamp(g_gen.shape, 0, kShapeCount - 1)].name;
        if (!shape.empty()) shape[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(shape[0])));
        if (!parts[kRoleWho].empty()) bits.push_back((parts[kRoleWho].size() == 1 ? "the person from " : "the people from ") + pics(parts[kRoleWho]) + (parts[kRoleWho].size() > 1 ? " together" : ""));
        if (!parts[kRoleThing].empty()) bits.push_back("what is in " + pics(parts[kRoleThing]));
        s = "A brand-new " + shape + " picture" + (bits.empty() ? std::string() : " of " + JoinAnd(bits));
        if (!parts[kRoleOutfit].empty()) s += ", wearing the outfit from " + pics(parts[kRoleOutfit]);
        if (!parts[kRolePlace].empty()) s += ", set in the place from " + pics(parts[kRolePlace]);
        if (!parts[kRoleLook].empty()) s += ", in the style of " + pics(parts[kRoleLook]);
        if (!g_gen.poseId.empty()) s += ", posed like the drawing";
        s += ". Nobody is replaced (to put someone into a picture, choose Change a picture).";
    }
    s += std::string(" Made by ") + (app.comfy.editEngine == "flux3" ? "FLUX 3 Klein" : "Qwen Image 2.1 Edit");
    if (g_gen.engine != kAuto && g_gen.engine != kVideo) s += std::string(" - ") + kEngineName[g_gen.engine] + " can't use pictures";
    return s + ".";
}

void Make(AppState& app) {
    std::string by, prompt = OneLine(g_gen.prompt);
    const int times = std::clamp(g_gen.times, 1, 16);
    // first what the pictures need: the writer's look at each, the pose in words (Make goes on by itself once they are there)
    const bool poseWait = PoseNeedsWords() && g_gen.poseWords.empty();
    if (poseWait) {
        LibItem pose;
        pose.id = g_gen.poseId, pose.image = g_gen.layout;
        DescribeGenPose(app, pose);
    }
    const bool tagsWait = OnPeoplePage() && TagsPending(), peopleWait = !OnPeoplePage() && PeoplePending();
    if (peopleWait)
        for (const std::wstring& p : g_gen.people)
            if (!g_personWords.count(p)) g_personWords[p].fut = std::async(std::launch::async, DescribePerson, p, app.comfy.url);
    if (poseWait || tagsWait || peopleWait) {
        g_gen.makeWaiting = true;
        g_gen.note = "Getting ready: the prompt writer is looking at the " + std::string(tagsWait || peopleWait ? "pictures" : "pose") + "...";
        return;
    }
    if (!OnPeoplePage()) { // Generate: from words, directed by the people (their looks) and the pose (how many, how they stand); a video's picture is its start
        auto add = [&prompt](const std::string& t) { prompt += std::string(prompt.empty() || prompt.back() == '.' ? "" : ".") + (prompt.empty() ? "" : " ") + t; };
        if (IdeaInUse() && IdeaKey() != g_gen.ideaKey) { // a new idea (or other people): written out first, then Make goes on by itself
            WriteIdea(app);
            g_gen.makeWaiting = true;
            return;
        }
        int n = 0;
        const std::string who = PeopleWords(n);
        if (!g_gen.ideaKey.empty()) n = 0; // (the written prompt already has them in it)
        const bool action = PoseNeedsAction();
        if (action) { // a two-person pose from its depth: first who does what to whom (the writer's line, from the drawing and these words)
            const std::string key = g_gen.poseId + "|" + prompt + "|" + who;
            auto it = g_poseActions.find(key);
            if (it == g_poseActions.end()) {
                if (!g_actionFut.valid()) g_actionKey = key, g_actionFut = std::async(std::launch::async, PoseAction, g_gen.layout, prompt + (who.empty() ? "" : " " + who), app.comfy.url);
                g_gen.makeWaiting = true;
                g_gen.note = "Getting ready: the prompt writer is working out who does what in the pose...";
                return;
            }
            add("Two people" + (it->second.empty() ? std::string(".") : ": " + it->second));
            add("Both are fully clothed, dressed as described (or in ordinary everyday clothes)."); // (the drawing's are bare mannequins)
        }
        if (n > 0) {
            add(who);
            // (not with a pose from its depth: there "person 2 on the right" made a third person, standing beside the pair)
            if (n == 2 && g_gen.posePeople >= 2 && !g_gen.poseId.empty() && !action) prompt += " Person 1 is the one on the left, person 2 the one on the right.";
            prompt += " Draw them exactly like this.";
        }
        if (!g_gen.poseId.empty() && !action) {
            // ("Two people, posed together" made Krea 2 draw a couple posing for the camera, side by side, instead of the drawing's embrace)
            const std::string many = g_gen.posePeople >= 2 ? "Two people" : "One person";
            if (PoseNeedsWords()) // (the drawing is of mannequins: the words only place the bodies - the clothes are the prompt's, or ordinary ones)
                prompt += std::string(prompt.empty() || prompt.back() == '.' ? "" : ".") + (prompt.empty() ? "" : " ") + many + " - the pose: " + g_gen.poseWords +
                          " This only sets how they stand and hold each other: they are fully clothed, dressed as described (or in ordinary everyday clothes), "
                          "and they look exactly as described.";
            else add(many + "."); // (MOSS follows the drawing: only how many)
        }
        if (GetEnvironmentVariableW(L"MV_GEN_PROMPT", nullptr, 0) > 0) { // (tests: what it sent)
            wchar_t base[MAX_PATH] = {};
            GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
            std::ofstream t(fs::path(base) / L"Try2Create" / L"generate_test_sent.txt", std::ios::binary | std::ios::trunc);
            t << prompt;
        }
        const std::vector<std::wstring> keep = g_gen.refs;
        const std::wstring keepLayout = g_gen.layout;
        if (g_gen.engine != kVideo) g_gen.refs.clear();
        if (g_gen.poseAsWords && !g_gen.poseId.empty()) g_gen.layout.clear(); // (the pose only in words: no depth map)
        const bool ok = MakeOne(app, prompt, g_gen.engine, times, by, true);
        g_gen.refs = keep, g_gen.layout = keepLayout;
        if (ok) g_gen.note = "Sent to " + by + (times > 1 ? " (" + std::to_string(times) + " pictures)" : std::string());
        return;
    }
    if (g_gen.refs.empty()) {
        g_gen.note = "Add the pictures to use first (a person, an outfit...)";
        return;
    }
    std::vector<std::wstring> order;
    bool change = false;
    const std::string sent = ComposeWithPictures(prompt, order, change);
    if (GetEnvironmentVariableW(L"MV_GEN_PROMPT", nullptr, 0) > 0) { // (tests: what it sent)
        wchar_t base[MAX_PATH] = {};
        GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
        std::ofstream t(fs::path(base) / L"Try2Create" / L"generate_test_sent.txt", std::ios::binary | std::ios::trunc);
        t << sent;
    }
    const std::vector<std::wstring> keep = g_gen.refs;
    const bool keepNew = g_gen.newPicture;
    g_gen.refs = order, g_gen.newPicture = !change;
    const bool ok = MakeOne(app, sent, g_gen.engine, times, by, true);
    g_gen.refs = keep, g_gen.newPicture = keepNew;
    if (ok) g_gen.note = "Sent to " + by + (times > 1 ? " (" + std::to_string(times) + " pictures)" : std::string());
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

std::wstring GenDataDir() {
    wchar_t buf[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    return std::wstring(buf) + L"\\Try2Create\\generate";
}

// A pose drawing trimmed to its figures (and a 5% margin), kept in generate\poses: the library's drawings have wide white margins, and Krea 2's
// depth control let go of a small couple's pose (pair60_P001, three tries: standing side by side, facing us) where the same drawing filling
// the frame kept it (facing each other, her knee raised). The original if it cannot be read.
std::wstring TrimmedPose(const std::wstring& drawing, const std::string& id) {
    const std::wstring out = GenDataDir() + L"\\poses\\" + std::wstring(id.begin(), id.end()) + L"_trim.png";
    std::error_code ec;
    if (fs::exists(out, ec)) return out;
    std::vector<unsigned char> px;
    int w = 0, h = 0;
    if (!DecodeImageRGBA(drawing, px, w, h, nullptr) || w <= 0 || h <= 0) return drawing;
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const unsigned char* p = &px[(static_cast<size_t>(y) * w + x) * 4];
            if (p[3] > 32 && p[0] + p[1] + p[2] < 3 * 200) x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
        }
    if (x1 < x0 || y1 < y0) return drawing;
    const int pad = static_cast<int>(std::max(x1 - x0, y1 - y0) * 0.05f);
    x0 = std::max(0, x0 - pad), y0 = std::max(0, y0 - pad), x1 = std::min(w - 1, x1 + pad), y1 = std::min(h - 1, y1 + pad);
    const int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
    std::vector<unsigned char> crop(static_cast<size_t>(cw) * ch * 4);
    for (int y = 0; y < ch; ++y) std::memcpy(&crop[static_cast<size_t>(y) * cw * 4], &px[(static_cast<size_t>(y + y0) * w + x0) * 4], static_cast<size_t>(cw) * 4);
    fs::create_directories(fs::path(out).parent_path(), ec);
    return WriteThumbnailPng(out, crop.data(), cw, ch) ? out : drawing;
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
    else if (label.rfind("Moody MOSS: ", 0) == 0) engine = kMoody;
    g_genCur = &g_gens[0];
    std::string by;
    const std::vector<std::wstring> keep = g_gen.refs;
    const std::wstring keepLayout = g_gen.layout;
    g_gen.refs.clear(); // (the composer's pictures are not this one's)
    g_gen.layout.clear();
    const bool ok = MakeOne(app, prompt, engine, times, by);
    g_gen.refs = keep;
    g_gen.layout = keepLayout;
    return ok;
}

void GenerateShowSent(const std::string& prompt) {
    if (g_sendingFromGenerate) return;
    Gen& g = g_gens[0];
    if (!g.prompt.empty() && g.prompt != prompt) g.before = g.prompt; // (Undo brings yours back)
    g.prompt = prompt;
    g.refs.clear(), g.people.clear();
    g.idea[0] = 0, g.ideaKey.clear();
    g.layout.clear(), g.layoutName.clear(), g.poseId.clear(), g.poseWords.clear(), g.posePeople = 1;
    g.makeWaiting = false;
    g.note = "This is the prompt you just sent to ComfyUI";
}

// The picture's LoRAs go on Generate's list for the model that made it (My workflow's when it was made elsewhere): by this ComfyUI's names; one it
// does not have is listed unticked, so what the picture used still shows.
void GenerateUseLoras(AppState& app, const ImageMetadata& meta) {
    std::string models;
    std::vector<LoraEntry> loras = LorasInPicture(meta, &models);
    if (loras.empty()) {
        app.SetStatus("No LoRAs found in this picture");
        return;
    }
    for (char& ch : models) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    ComfySettings& c = app.comfy;
    int engine = kWorkflow;
    std::vector<LoraEntry>* list = &c.promptLoras;
    if (models.find("moody") != std::string::npos) engine = kMoody, list = &c.mossLoras;
    else if (models.find("krea") != std::string::npos) engine = kKrea, list = &c.kreaLoras;
    else if (models.find("qwen") != std::string::npos) engine = kQwen, list = &c.qwenLoras;
    else if (models.find("moss") != std::string::npos) engine = kMoss, list = &c.mossLoras;
    else if (models.find("minimax_h3") != std::string::npos || models.find("h3_") != std::string::npos) engine = kVideo, list = &c.h3Loras;
    std::string missing;
    int nMissing = 0;
    std::vector<LoraEntry> keep;
    for (LoraEntry& l : loras) {
        std::string low = l.name;
        for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (engine == kVideo && (low.find("turbo") != std::string::npos || low.find("h3_flex") != std::string::npos || low.find("taomate") != std::string::npos)) continue; // (the quality's own)
        const std::string name = ResolveLora(l.name);
        if (name.empty()) {
            missing += (missing.empty() ? "" : ", ") + LoraDisplayName(l.name);
            ++nMissing;
            l.on = false;
            if (l.name.find('.') == std::string::npos) l.name += ".safetensors";
        } else {
            l.name = name;
        }
        keep.push_back(std::move(l));
    }
    *list = std::move(keep);
    SaveComfySettings(c);
    g_genCur = &g_gens[0];
    g_gen.engine = engine;
    g_genSub = 0;
    app.requestTab = 4;
    app.SetStatus(std::to_string(list->size()) + (list->size() == 1 ? " LoRA" : " LoRAs") + " on Generate's " + kEngineName[engine] +
                  (nMissing ? " (not in ComfyUI's loras folder, left unticked: " + missing + ")" : ""));
}

bool GenerateTakePictures(const std::vector<std::wstring>& paths, bool peoplePage) {
    g_genCur = &g_gens[peoplePage ? 1 : 0];
    if (peoplePage || g_gen.engine == kVideo) { // (pictures to use; a video's start)
        for (const std::wstring& p : paths) AddRef(p);
        return true;
    }
    g_genSub = 0; // Generate: people whose looks go into the words
    for (const std::wstring& p : paths)
        if (!p.empty() && g_gen.people.size() < 4 && std::find(g_gen.people.begin(), g_gen.people.end(), p) == g_gen.people.end()) g_gen.people.push_back(p);
    return true;
}

void GenerateWithPrompt(AppState& app, const std::string& prompt, const std::vector<std::wstring>& refs) {
    if (!refs.empty()) { // pictures to use: that is an edit (Edits > People & outfits)
        Gen& p = g_gens[1];
        p.prompt = prompt, p.refs = refs, p.focus = true;
        SetEditsMode(8);
        app.requestTab = 6;
        return;
    }
    g_genCur = &g_gens[0];
    g_gen.prompt = prompt;
    g_gen.focus = true;
    g_genSub = 0;
    app.requestTab = 4;
}

namespace {

// The look: a Krea 2 style (or a picture's look) and the cinema look, folded away until wanted; what is on shows in the header
void DrawLook(AppState& app) {
    if (!g_gen.lorasAsked && !g_gen.lorasFut.valid()) {
        g_gen.lorasAsked = true;
        g_gen.lorasFut = std::async(std::launch::async, [url = app.comfy.url]() { return ComfyListLoras(url); });
    }
    // the Krea 2 styles (only when Krea 2 makes it: Auto or Krea 2)
    if (g_gen.engine == kAuto || g_gen.engine == kKrea) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Krea 2 style:");
    ImGui::SameLine();
    if (Chip("None##style", g_gen.style < 0)) g_gen.style = -1;
    std::vector<std::string> missing;
    for (int k = 0; k <= kKreaStyleCount; ++k) {
        const char* name = k < kKreaStyleCount ? kStyles[k].name : "Like a picture...";
        const char* file = k < kKreaStyleCount ? kStyles[k].file : kStyleRefFile;
        const bool there = StyleFileThere(file);
        if (!there) missing.push_back(file);
        ui::SameLineIfFits(name);
        ImGui::BeginDisabled(!there);
        if (Chip(name, g_gen.style == k)) g_gen.style = g_gen.style == k ? -1 : k;
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s%s", k < kKreaStyleCount ? (std::string("Krea 2's official \"") + kStyles[k].name + "\" style LoRA (adds: " + kStyles[k].words + ")").c_str()
                                                       : "The look of a picture you add (its colours, medium, brushwork) on what you describe: Krea 2's style reference LoRA",
                              there ? "" : "\nNot downloaded yet: Set up below");
    }
    if (!missing.empty()) {
        char lbl[96];
        if (g_styleDl.running) snprintf(lbl, sizeof(lbl), "Downloading %d of %d...", g_styleDl.done.load() + 1, g_styleDl.total.load());
        else snprintf(lbl, sizeof(lbl), "Set up the styles (%.1f GB)", missing.size() * 0.47);
        ImGui::BeginDisabled(g_styleDl.running || ComfyModelsDir().empty());
        if (ImGui::Button(lbl)) DownloadStyleFiles(ComfyModelsDir(), missing);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Downloads %d LoRA files from Comfy-Org/Krea-2 into ComfyUI's models\\loras (Krea 2 Community License)%s", static_cast<int>(missing.size()),
                              ComfyModelsDir().empty() ? "\nComfyUI's models folder is not known yet: pick ComfyUI in Help > Set up Try2Create" : "");
    }
    if (g_gen.style >= 0 && g_gen.style < kKreaStyleCount) {
        ImGui::SetNextItemWidth(ui::Dp(160.0f));
        ImGui::SliderFloat("Style strength", &g_gen.styleStrength, 0.3f, 1.2f, "%.2f");
    }
    if (g_gen.style == kKreaStyleCount) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Its look from:");
        ImGui::SameLine();
        if (!g_gen.styleRef.empty()) {
            const AppState::LibThumb lt = app.LibraryThumb(g_gen.styleRef);
            const float th = ui::Dp(40.0f);
            if (lt.srv) ImGui::Image((ImTextureID)(intptr_t)lt.srv, ImVec2(th, th));
            else ImGui::Button("?##lookref", ImVec2(th, th));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click: take it out");
            if (ImGui::IsItemClicked()) g_gen.styleRef.clear();
            ImGui::SameLine();
        }
        if (ImGui::Button("Gallery's picture##look")) {
            if (app.selectedIndex >= 0 && app.selectedIndex < static_cast<int>(app.entries.size())) g_gen.styleRef = app.entries[app.selectedIndex].path;
            else g_gen.note = "Pick a picture in the gallery first";
        }
        ImGui::SameLine();
        if (ImGui::Button("File...##look")) {
            const std::wstring f = OpenImageFileDialog(GetActiveWindow());
            if (!f.empty()) g_gen.styleRef = f;
        }
        if (g_gen.styleRef.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("(a painting, an illustration, a photo with a strong look)");
        }
    }
    } // (the Krea 2 styles)
    // the cinema look (any maker: it is words in the prompt)
    if (ImGui::Checkbox("Cinema look", &g_gen.cinema) && g_gen.cinema) {
        bool any = false;
        for (int k = 0; k < kCineCount; ++k) any |= g_gen.cine[k] != 0;
        for (int k = 0; !any && k < kCineCount; ++k) g_gen.cine[k] = kCine[k].start;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("The picture as a still from a film: the genre, the era, the camera and lens, the light and the colour go into the prompt");
    if (g_gen.cinema) {
        const float w = ui::Dp(150.0f);
        for (int k = 0; k < kCineCount; ++k) {
            ImGui::PushID(k);
            if (k % 4) ImGui::SameLine(0, ui::Dp(12.0f));
            ImGui::BeginGroup();
            ImGui::TextDisabled("%s", kCine[k].name);
            ImGui::SetNextItemWidth(w);
            int& v = g_gen.cine[k];
            if (ImGui::BeginCombo("##cine", kCine[k].picks[std::clamp(v, 0, static_cast<int>(kCine[k].picks.size()) - 1)].label)) {
                for (int i = 0; i < static_cast<int>(kCine[k].picks.size()); ++i) {
                    if (ImGui::Selectable(kCine[k].picks[i].label, v == i)) v = i;
                    if (*kCine[k].picks[i].text && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", kCine[k].picks[i].text + (kCine[k].picks[i].text[0] == ' ' ? 1 : 0));
                }
                ImGui::EndCombo();
            }
            ImGui::EndGroup();
            ImGui::PopID();
        }
        if (ImGui::Button("Reset##cine")) {
            for (int k = 0; k < kCineCount; ++k) g_gen.cine[k] = kCine[k].start;
        }
        if (!g_gen.refs.empty() || g_gen.engine == kVideo) {
            ImGui::SameLine();
            ImGui::TextDisabled("(only for pictures made from text: not for edits or videos)");
        }
    }
}

// The look as one chip that says what is on; its choices in a popup
void DrawLookButton(AppState& app) {
    if (g_gen.lorasFut.valid() && g_gen.lorasFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) g_gen.loras = g_gen.lorasFut.get();
    if (g_styleDl.finished.exchange(false)) {
        g_gen.note = g_styleDl.failed ? "A style file did not download (try Set up again: it carries on where it stopped)" : "The Krea 2 styles are ready";
        g_gen.lorasAsked = false;
    }
    if (g_gen.engine == kVideo) return; // (a video has its own look)
    const bool krea = g_gen.engine == kAuto || g_gen.engine == kKrea; // (the styles are Krea 2's: other makers only get the cinema look)
    std::string on;
    if (krea && g_gen.style >= 0) on = g_gen.style < kKreaStyleCount ? kStyles[g_gen.style].name : "a picture's look";
    if (g_gen.cinema) on += on.empty() ? "cinema" : ", cinema";
    const std::string lbl = (on.empty() ? std::string(krea ? "Style..." : "Cinema look...") : (krea ? "Style: " : "Look: ") + on) + "###genlook";
    ui::SameLineIfFits(lbl.c_str());
    if (Chip(lbl.c_str(), !on.empty())) ImGui::OpenPopup("##genlookpop");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip(krea ? "A Krea 2 style, the look of a picture, or a cinema look (film, era, camera, light)"
                               : "A cinema look: the film, era, camera and light go into the prompt (the styles are Krea 2's: pick Auto or Krea 2 for them)");
    ImGui::SetNextWindowSize(ImVec2(ui::Dp(660.0f), 0.0f));
    if (ImGui::BeginPopup("##genlookpop")) {
        ImGui::PushTextWrapPos(0);
        DrawLook(app);
        ImGui::PopTextWrapPos();
        ImGui::EndPopup();
    }
}

// The maker's LoRAs (one list per model: Create a character uses the same ones) as one chip naming what is on; the lists in a popup
void DrawLorasButton(AppState& app, bool krea) {
    struct List {
        std::vector<LoraEntry>* list;
        const char *model, *id, *label;
    };
    std::vector<List> ls;
    ComfySettings& c = app.comfy;
    if (OnPeoplePage()) { // (the picture editor's)
        if (c.editEngine == "flux3") return; // (FLUX 3 Klein edits load no LoRAs)
        ls.push_back({&c.editLoras, "Qwen Image 2.1", "glre", "Edit LoRAs:"});
    } else if (g_gen.engine == kAuto) {
        if (krea) ls.push_back({&c.kreaLoras, "Krea 2", "glrk", "Krea 2 LoRAs:"});
        ls.push_back({&c.qwenLoras, "Qwen Image 2.1", "glrq", "Qwen 2.1 LoRAs:"});
    } else if (g_gen.engine == kKrea) ls.push_back({&c.kreaLoras, "Krea 2", "glrk", "LoRAs:"});
    else if (g_gen.engine == kQwen) ls.push_back({&c.qwenLoras, "Qwen Image 2.1", "glrq", "LoRAs:"});
    else if (g_gen.engine == kMoss || g_gen.engine == kMoody) ls.push_back({&c.mossLoras, "MOSS-Image", "glrm", "LoRAs:"});
    else if (g_gen.engine == kWorkflow) ls.push_back({&c.promptLoras, "", "glrw", "LoRAs on your workflow:"});
    else if (g_gen.engine == kVideo) ls.push_back({&c.h3Loras, "MiniMax H3", "glrv", "LoRAs:"});
    std::string on;
    int n = 0;
    for (const List& l : ls) {
        for (const LoraEntry& e : *l.list) {
            if (!e.on || ++n > 2) continue;
            std::string name = e.name.substr(e.name.find_last_of("\\/") == std::string::npos ? 0 : e.name.find_last_of("\\/") + 1);
            if (name.size() > 12 && name.compare(name.size() - 12, 12, ".safetensors") == 0) name.resize(name.size() - 12);
            if (name.size() > 22) name = name.substr(0, 20) + "..";
            char st[16];
            snprintf(st, sizeof(st), " %.2g", e.strength);
            on += (on.empty() ? "" : ", ") + name + st;
        }
    }
    const std::string lbl = (n == 0 ? std::string("LoRAs...") : "LoRAs: " + on + (n > 2 ? " +" + std::to_string(n - 2) : std::string())) + "###genloras";
    ui::SameLineIfFits(lbl.c_str());
    if (Chip(lbl.c_str(), n > 0)) ImGui::OpenPopup("##genloraspop");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("The LoRAs this maker loads, each at its own strength");
    ImGui::SetNextWindowSize(ImVec2(ui::Dp(620.0f), 0.0f));
    if (ImGui::BeginPopup("##genloraspop")) {
        for (const List& l : ls) DrawModelLoras(app, *l.list, l.model, l.id, l.label);
        ImGui::EndPopup();
    }
}

void PollMany(AppState& app);

} // namespace

int& GenerateSubTab() { return g_genSub; }

namespace {

// The pictures panel: on Edits > People & outfits the pictures to use (each with its role), a pose and outfits; on Generate what directs
// the picture - a pose (how many people, how they stand), MOSS's copied layout, a video's start picture
void DrawPicturesPanel(AppState& app) {
    // ---- a panel you can drop pictures on (or paste into), with buttons for each way in
    const bool people = OnPeoplePage(), moss = !people && (g_gen.engine == kMoss || g_gen.engine == kMoody);
    const bool layoutable = moss || (!people && ((g_gen.engine == kQwen || g_gen.engine == kAuto) && !QwenDepthControl().empty())) ||
                            (!people && ((g_gen.engine == kKrea || g_gen.engine == kAuto) && !KreaDepthControlLora().empty())); // (who can follow a picture's depth)
    const bool pictures = people || g_gen.engine == kVideo; // (Generate takes a picture only as a video's start)
    ImGui::Spacing();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui::Dp(10.0f), ui::Dp(8.0f)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
    ImGui::BeginChild("##genpics", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (people) {
        ui::Eyebrow("Pictures to use");
        ImGui::SameLine();
        ImGui::TextDisabled(g_gen.refs.empty() ? "add a person (and an outfit, a place...): drag pictures here, or Ctrl+V to paste one."
                                               : "the button under each says what it is for. Click a picture to take it out.");
    } else {
        ui::Eyebrow("Direct it");
        ImGui::SameLine();
        ImGui::TextDisabled(g_gen.engine == kVideo ? "the video's start picture, and a pose."
                                                   : "drag people here (their looks go into your words), and a pose (how many, how they stand).");
    }
    const float th = ui::Dp(72.0f);
    auto thumb = [&](const std::wstring& path, const char* id, const char* tag) { // a picture, its tag under it; true = clicked (take it out)
        ImGui::BeginGroup();
        const AppState::LibThumb lt = app.LibraryThumb(path);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        if (lt.srv) {
            const float a = lt.w > 0 && lt.h > 0 ? static_cast<float>(lt.w) / lt.h : 1.0f;
            const ImVec2 sz = a >= 1.0f ? ImVec2(th, th / a) : ImVec2(th * a, th);
            ImGui::InvisibleButton(id, ImVec2(th, th));
            ImGui::GetWindowDrawList()->AddRectFilled(at, ImVec2(at.x + th, at.y + th), ImGui::GetColorU32(ImGuiCol_WindowBg), ui::Dp(6.0f));
            const ImVec2 p0(at.x + (th - sz.x) * 0.5f, at.y + (th - sz.y) * 0.5f);
            ImGui::GetWindowDrawList()->AddImageRounded((ImTextureID)(intptr_t)lt.srv, p0, ImVec2(p0.x + sz.x, p0.y + sz.y), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, ui::Dp(6.0f));
        } else {
            ImGui::Button((std::string("...") + id).c_str(), ImVec2(th, th));
        }
        const bool hovered = ImGui::IsItemHovered(), clicked = ImGui::IsItemClicked();
        if (hovered) { // an x in the corner says what a click does
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float r = ui::Dp(9.0f);
            const ImVec2 c(at.x + th - r - ui::Dp(3.0f), at.y + r + ui::Dp(3.0f));
            dl->AddCircleFilled(c, r, IM_COL32(0, 0, 0, 190));
            const float d = r * 0.45f;
            dl->AddLine(ImVec2(c.x - d, c.y - d), ImVec2(c.x + d, c.y + d), IM_COL32_WHITE, ui::Dp(1.6f));
            dl->AddLine(ImVec2(c.x - d, c.y + d), ImVec2(c.x + d, c.y - d), IM_COL32_WHITE, ui::Dp(1.6f));
        }
        if (tag) {
            const float tw = ImGui::CalcTextSize(tag).x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (th - tw) * 0.5f));
            ImGui::TextDisabled("%s", tag);
        }
        ImGui::EndGroup();
        return clicked;
    };
    // each new picture is looked at by the prompt writer (what is it: a person, an outfit, a place, a thing); Make waits for that if pressed early
    PumpTags();
    for (const std::wstring& r : g_gen.refs) {
        if (!people || g_tags.count(r)) continue;
        g_tags[r].fut = std::async(std::launch::async, TagPicture, r, app.comfy.url);
    }
    for (const std::wstring& p : g_gen.people) // (Generate's people: described as soon as they are added)
        if (!people && !g_personWords.count(p)) g_personWords[p].fut = std::async(std::launch::async, DescribePerson, p, app.comfy.url);
    PumpAction();
    PumpIdea(app);
    if (g_gen.makeWaiting && !(people && TagsPending()) && !(!people && PeoplePending()) && !(PoseNeedsWords() && g_gen.poseWords.empty()) && !ActionPending() && !g_gen.ideaFut.valid()) {
        g_gen.makeWaiting = false;
        Make(app);
    }
    // the two things it can do with them: a brand-new picture with them in it, or a change to one of them (picture 1)
    auto setChange = [](int k) { // k = the picture to change (it becomes picture 1); -1 = none
        for (const std::wstring& r : g_gen.refs) {
            RefTag& o = g_tags[r];
            if (o.role == kRoleChange) o.role = kRoleWho, o.mine = true;
        }
        if (k < 0 || k >= static_cast<int>(g_gen.refs.size())) return;
        std::rotate(g_gen.refs.begin(), g_gen.refs.begin() + k, g_gen.refs.begin() + k + 1);
        RefTag& t = g_tags[g_gen.refs[0]];
        t.role = kRoleChange, t.mine = true;
    };
    const bool changing = people && ChangingAPicture();
    if (people && !g_gen.refs.empty()) {
        if (Chip("New picture with them", !changing)) setChange(-1);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("A brand-new picture with the people and things below in it. Nobody is replaced.");
        ImGui::SameLine(0, ui::Dp(4.0f));
        if (Chip("Change a picture", changing)) {
            if (!changing) setChange(0);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("Picture 1 is the one changed: the people below replace someone in it (or are added), outfits go on, the rest stays.\n"
                              "Another picture: set it to \"picture to change\" with the button under it.");
        ImGui::SameLine();
        ImGui::TextDisabled(changing ? "Picture 1 is changed; the others go into it." : "Everyone below goes into a brand-new picture.");
    }
    int remove = -1, makeChange = -1;
    for (size_t k = 0; pictures && k < g_gen.refs.size(); ++k) {
        if (k) ImGui::SameLine();
        ImGui::PushID(static_cast<int>(k));
        RefTag& t = g_tags[g_gen.refs[k]];
        ImGui::BeginGroup();
        if (!people) { // (a video's start picture)
            if (thumb(g_gen.refs[k], "##ref", k == 0 ? "start" : "")) remove = static_cast<int>(k);
            ImGui::EndGroup();
            ImGui::PopID();
            continue;
        }
        if (thumb(g_gen.refs[k], "##ref", nullptr)) remove = static_cast<int>(k);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("<image%d>%s%s\nClick: take it out", static_cast<int>(k) + 1, t.what.empty() ? "" : ": ", t.what.c_str());
        // what it is for: the writer's guess, yours to change
        const bool looking = t.fut.valid();
        char lbl[64];
        if (k == 0 && changing) snprintf(lbl, sizeof(lbl), "1: picture to change##role");
        else snprintf(lbl, sizeof(lbl), "%d: %s##role", static_cast<int>(k) + 1, looking ? "looking..." : kRoleName[t.role]);
        if (ImGui::Button(lbl)) ImGui::OpenPopup("##rolepop");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("%s", looking ? "The prompt writer is looking at it" : (std::string(kRoleAbout[t.role]) + "\nClick to change").c_str());
        if (ImGui::BeginPopup("##rolepop")) {
            ImGui::TextDisabled("Picture %d is...", static_cast<int>(k) + 1);
            if (!t.what.empty()) ImGui::TextDisabled("(%s)", t.what.c_str());
            ImGui::Separator();
            for (int r = kRoleWho; r < kRoleCount; ++r) {
                if (ImGui::Selectable(kRoleName[r], t.role == r)) {
                    if (t.role == kRoleChange) setChange(-1);
                    t.role = r, t.mine = true;
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", kRoleAbout[r]);
            }
            ImGui::Separator();
            if (ImGui::Selectable("the picture to change", t.role == kRoleChange)) makeChange = static_cast<int>(k);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", kRoleAbout[kRoleChange]);
            ImGui::EndPopup();
        }
        // changing a picture: whom this person takes the place of
        if (changing && k > 0 && t.role == kRoleWho) {
            const int tg = TargetOf(g_gen.refs[k]);
            const std::string rl = std::string(tg == kTgtAdd ? "added" : std::string("replaces ") + kTargetName[tg]) + "##tgt";
            if (ImGui::Button(rl.c_str())) ImGui::OpenPopup("##tgtpop");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Who in picture 1 this person takes the place of (click to choose)");
            if (ImGui::BeginPopup("##tgtpop")) {
                ImGui::TextDisabled("The person from picture %d replaces...", static_cast<int>(k) + 1);
                ImGui::Separator();
                for (int r = 0; r < kTargetCount; ++r)
                    if (ImGui::Selectable(kTargetName[r], tg == r)) t.target = r;
                ImGui::EndPopup();
            }
        }
        ImGui::EndGroup();
        ImGui::PopID();
    }
    if (makeChange >= 0) setChange(makeChange);
    if (remove >= 0) g_gen.refs.erase(g_gen.refs.begin() + remove);
    if (!g_gen.layout.empty()) {
        if (pictures && !g_gen.refs.empty()) ImGui::SameLine(0, ui::Dp(14.0f));
        const std::string tag = !g_gen.poseId.empty() ? std::string(g_gen.posePeople >= 2 ? "2 people" : "1 person") + (!PoseNeedsWords() || !g_gen.poseWords.empty() ? "" : "...")
                                                      : "layout";
        if (thumb(g_gen.layout, "##lay", tag.c_str())) g_gen.layout.clear(), g_gen.layoutName.clear(), g_gen.poseId.clear(), g_gen.poseWords.clear();
        if (ImGui::IsItemHovered()) {
            if (!g_gen.poseId.empty())
                ImGui::SetTooltip("%s\n%s\nClick: take it out", g_gen.layoutName.c_str(),
                                  !PoseNeedsWords() ? "MOSS draws the person in this pose (from the drawing's depth)"
                                  : g_gen.poseWords.empty() ? "The writer puts it into words: Make adds them"
                                                                 : ("Make adds to your prompt: Pose: " + g_gen.poseWords).c_str());
            else ImGui::SetTooltip("%s\n%s\nClick: take it out", g_gen.layoutName.c_str(), layoutable ? "Its layout (where things are, how big) is copied" : "This maker cannot copy a layout (MOSS, Moody MOSS, Qwen 2.1 and Krea 2 with their depth controls can)");
        }
    }
    // Generate's people: their looks go into the words (the prompt writer describes each one)
    int removePerson = -1;
    for (size_t k = 0; !people && k < g_gen.people.size(); ++k) {
        if (k || (pictures && !g_gen.refs.empty()) || !g_gen.layout.empty()) ImGui::SameLine(0, ui::Dp(k ? 8.0f : 14.0f));
        ImGui::PushID(1000 + static_cast<int>(k));
        auto it = g_personWords.find(g_gen.people[k]);
        const bool busy = it == g_personWords.end() || it->second.fut.valid();
        char tag[24];
        snprintf(tag, sizeof(tag), busy ? "person %d..." : "person %d", static_cast<int>(k) + 1);
        if (thumb(g_gen.people[k], "##person", tag)) removePerson = static_cast<int>(k);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Person %d: %s\nClick: take them out", static_cast<int>(k) + 1,
                              busy ? "the prompt writer is looking at them" : it->second.words.empty() ? "(the prompt writer could not describe them)" : it->second.words.c_str());
        ImGui::PopID();
    }
    if (removePerson >= 0) g_gen.people.erase(g_gen.people.begin() + removePerson);
    if (!people && !g_gen.people.empty() && g_gen.poseId.empty() && g_gen.engine != kVideo) { // what is happening to them
        const bool writing = g_gen.ideaFut.valid() || g_gen.ideaWant;
        const char* writeLbl = writing ? "Writing..." : "Write it";
        ui::Eyebrow("The idea");
        ImGui::SetNextItemWidth(std::max(ui::Dp(120.0f), ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(writeLbl).x - ImGui::GetStyle().FramePadding.x * 2.0f - ImGui::GetStyle().ItemSpacing.x));
        const bool enter = ImGui::InputTextWithHint("##genidea", "What is happening? e.g. they argue in a rainy car park", g_gen.idea, sizeof(g_gen.idea), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        ImGui::BeginDisabled(writing || !g_gen.idea[0]);
        if (ImGui::Button(writeLbl) || (enter && !writing && g_gen.idea[0])) {
            if (PeoplePending()) g_gen.ideaWant = true;
            else WriteIdea(app);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("The prompt writer writes the whole prompt from this and the people's looks.\nChange the idea and write it again: the new one takes the old prompt's place (Undo brings it back).");
        if (g_gen.idea[0] && !writing && IdeaKey() != g_gen.ideaKey) ImGui::TextDisabled("Make writes it out first: it takes the place of what is in the box.");
    }
    // the ways in
    auto addPerson = [&](const std::wstring& p) {
        if (p.empty() || g_gen.people.size() >= 4 || std::find(g_gen.people.begin(), g_gen.people.end(), p) != g_gen.people.end()) return;
        g_gen.people.push_back(p);
    };
    if (!people && g_gen.engine != kVideo) {
        if (ImGui::Button("Person...")) ImGui::OpenPopup("##genperson");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("Someone to put in the picture: the prompt writer describes how they look (face, hair, build, clothes)\n"
                              "and Make adds it to your words - any maker can draw them. Up to four.");
        if (ImGui::BeginPopup("##genperson")) {
            if (ImGui::MenuItem("The gallery's picture")) {
                if (app.selectedIndex >= 0 && app.selectedIndex < static_cast<int>(app.entries.size())) addPerson(app.entries[app.selectedIndex].path);
                else g_gen.note = "Pick a picture in the gallery first";
            }
            if (ImGui::MenuItem("A picture file...")) addPerson(OpenImageFileDialog(GetActiveWindow()));
            if (ImGui::MenuItem("Paste")) {
                std::string msg;
                const std::wstring p = PasteBoardImage(msg);
                if (!p.empty()) addPerson(p);
                else g_gen.note = msg.empty() ? "No picture on the clipboard" : msg;
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
    }
    auto addGallery = [&]() {
        if (app.selectedIndex >= 0 && app.selectedIndex < static_cast<int>(app.entries.size())) AddRef(app.entries[app.selectedIndex].path);
        else g_gen.note = "Pick a picture in the gallery first";
    };
    auto paste = [&]() {
        std::string msg;
        const std::wstring p = PasteBoardImage(msg);
        if (!p.empty()) GenerateTakePictures({p}, people), g_gen.note.clear();
        else g_gen.note = msg.empty() ? "No picture on the clipboard" : msg;
    };
    if (pictures) {
        if (ImGui::Button("From the gallery")) addGallery();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("The picture picked in the gallery");
        ImGui::SameLine();
        if (ImGui::Button("File...")) AddRef(OpenImageFileDialog(GetActiveWindow()));
        ImGui::SameLine();
        if (ImGui::Button("Paste")) paste();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("A picture you copied (Ctrl+V does the same while you are not typing)");
        ImGui::SameLine(0, ui::Dp(14.0f));
    }
    if (ImGui::Button("Pose...")) {
        OpenLibraryPicker(app, 0, people ? "A pose for them" : "A pose to direct the picture", [gp = g_genCur](AppState& a, const LibItem& it) {
            g_genCur = gp; // (whose pose)
            g_gen.layout = TrimmedPose(it.image, it.id), g_gen.layoutDrawn = true, g_gen.layoutName = "Pose " + it.id;
            g_gen.poseId = it.id, g_gen.poseWords = LibraryPoseWords(it.id), g_gen.posePeople = LibraryPosePeople(it.id);
            g_gen.note.clear();
            if (g_gen.poseWords.empty() && PoseNeedsWords()) DescribeGenPose(a, it);
        });
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip(people ? "A pose from your poses library: the people are posed like the drawing (the prompt writer puts it into words)"
                                 : "A pose from your poses library directs the picture: how many people (one, or two together) and how they stand,\n"
                                   "sit or hold each other. MOSS, Moody MOSS and Krea 2 follow the drawing itself (its depth); the others get it in words.");
    if (!people && !g_gen.poseId.empty() && (g_gen.engine == kKrea || g_gen.engine == kAuto || g_gen.engine == kMoss || g_gen.engine == kMoody)) {
        // how the pose directs it: its exact shape (the drawing's depth), or only in words - a looser guide the picture can move away from
        ImGui::SameLine(0, ui::Dp(10.0f));
        if (Chip("Exact shape", !g_gen.poseAsWords)) g_gen.poseAsWords = false;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("The people are drawn in exactly the drawing's pose and place in the frame (from its depth)");
        ImGui::SameLine(0, ui::Dp(4.0f));
        if (Chip("Words only", g_gen.poseAsWords) && !g_gen.poseAsWords) {
            g_gen.poseAsWords = true;
            if (g_gen.poseWords.empty()) {
                LibItem it;
                it.id = g_gen.poseId, it.image = g_gen.layout;
                DescribeGenPose(app, it);
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("The pose is only written into the prompt (the prompt writer describes it): a guide, not a map -\n"
                              "the picture is framed freely and the pose may come out looser");
    }
    if (people) {
        ImGui::SameLine();
        if (ImGui::Button("Outfit...")) {
            OpenLibraryPicker(app, 1, "An outfit for them", [gp = g_genCur](AppState&, const LibItem& it) {
                g_genCur = gp;
                if (g_gen.refs.size() >= 10) return;
                AddRef(it.image);
                RefTag& t = g_tags[it.image]; // (no need to look: it is an outfit, its keywords say what)
                t.role = kRoleOutfit, t.what = it.words, t.mine = true;
            });
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("An outfit from your outfits library: its picture as a reference, its words in the prompt");
    }
    if (layoutable) {
        ImGui::SameLine();
        if (ImGui::Button("Copy a layout...")) ImGui::OpenPopup("##genlayout");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("Copy where things are, how big and the pose from a photo or a video's first frame (its depth)");
        if (ImGui::BeginPopup("##genlayout")) {
            if (ImGui::MenuItem("From the gallery's picture or video")) {
                if (app.selectedIndex >= 0 && app.selectedIndex < static_cast<int>(app.entries.size())) {
                    std::wstring p = app.entries[app.selectedIndex].path;
                    if (IsVideoPath(p)) { // its first frame
                        const std::wstring out = GenDataDir() + L"\\layout_frame.png";
                        std::error_code ec;
                        fs::create_directories(GenDataDir(), ec);
                        fs::remove(out, ec);
                        std::string log;
                        RunCaptureHidden(L"\"" + FfmpegPath() + L"\" -y -v error -i \"" + p + L"\" -frames:v 1 \"" + out + L"\"", log, true);
                        p = fs::exists(out, ec) ? out : std::wstring();
                    }
                    if (p.empty()) g_gen.note = "Could not take a frame from that video";
                    else g_gen.layout = p, g_gen.layoutDrawn = false, g_gen.poseId.clear(), g_gen.poseWords.clear(),
                         g_gen.layoutName = "Layout of " + fs::path(app.entries[app.selectedIndex].path).filename().u8string();
                } else {
                    g_gen.note = "Pick a picture in the gallery first";
                }
            }
            if (ImGui::MenuItem("From a file...")) {
                const std::wstring p = OpenImageFileDialog(GetActiveWindow());
                if (!p.empty()) g_gen.layout = p, g_gen.layoutDrawn = false, g_gen.poseId.clear(), g_gen.poseWords.clear(), g_gen.layoutName = "Layout of " + fs::path(p).filename().u8string();
            }
            ImGui::EndPopup();
        }
    }
    if (g_gen.engine != kVideo) { // what Make will do with them, in a few words (what it sends, on hover)
        const std::string plan = PlanLine(app);
        if (!plan.empty()) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(ui::kAccent, "What happens: %s", plan.c_str());
            ImGui::PopTextWrapPos();
            if (!g_gen.refs.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
                std::vector<std::wstring> order;
                bool change = false;
                const std::string sent = ComposeWithPictures(OneLine(g_gen.prompt), order, change);
                ImGui::SetNextWindowSize(ImVec2(ui::Dp(520.0f), 0.0f));
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(0);
                ImGui::TextDisabled("What it sends:");
                ImGui::TextUnformatted(sent.c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        }
    }
    if (moss && !g_gen.layout.empty() && MossDepthControlLora().empty())
        ImGui::TextColored(ui::kAccent, "The layout needs moss-color-depth-control.safetensors in ComfyUI's loras\\MOSS-Image");
    ImGui::EndChild();
    if (ImGui::BeginDragDropTarget()) { // (pictures to use; on Generate, people)
        std::vector<std::wstring> got;
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("MV_LIBITEM")) got.push_back(static_cast<const wchar_t*>(pl->Data));
        else if (const ImGuiPayload* pg = ImGui::AcceptDragDropPayload("MV_IMAGES")) {
            const int* idx = static_cast<const int*>(pg->Data);
            for (int k = 0; k < static_cast<int>(pg->DataSize / sizeof(int)); ++k) {
                if (idx[k] >= 0 && idx[k] < static_cast<int>(app.entries.size())) got.push_back(app.entries[idx[k]].path);
            }
        }
        if (!got.empty()) GenerateTakePictures(got, people);
        ImGui::EndDragDropTarget();
    }
    // Ctrl+V on the page (not while typing): a copied picture goes in
    if (!ImGui::GetIO().WantTextInput && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::GetIO().KeyCtrl &&
        ImGui::IsKeyPressed(ImGuiKey_V, false))
        paste();
}

} // namespace

// The top of the Generate page: what to make, by whom, how; Make
void DrawGenerateComposer(AppState& app) {
    g_genCur = &g_gens[0];
    { // tests: MV_GEN_PROMPT (+ MV_GEN_ENGINE 0-6: Auto, Krea, Qwen, MOSS, Moody MOSS, workflow, video; MV_GEN_TIMES) fills it in and presses Make once
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
            if (GetEnvironmentVariableW(L"MV_GEN_STYLE", e, 8)) g_gen.style = std::clamp(_wtoi(e), -1, kKreaStyleCount); // (MV_GEN_STYLEREF: the look's picture)
            if (GetEnvironmentVariableW(L"MV_GEN_STYLEREF", buf, 2048)) g_gen.styleRef = buf;
            if (GetEnvironmentVariableW(L"MV_GEN_LAYOUT", buf, 2048)) { // MOSS on this picture's layout (MV_GEN_LAYOUT_DRAWN: a mannequin)
                g_gen.layout = buf, g_gen.layoutName = "test";
                g_gen.layoutDrawn = GetEnvironmentVariableW(L"MV_GEN_LAYOUT_DRAWN", e, 8) > 0;
            }
            if (GetEnvironmentVariableW(L"MV_GEN_PEOPLE", buf, 2048)) { // people to put in it, "a|b" (their looks into the words)
                const std::wstring s = buf;
                for (size_t a = 0; a <= s.size();) {
                    size_t b = s.find(L'|', a);
                    if (b == std::wstring::npos) b = s.size();
                    if (b > a) g_gen.people.push_back(s.substr(a, b - a));
                    a = b + 1;
                }
            }
            if (GetEnvironmentVariableW(L"MV_GEN_IDEA", buf, 2048)) snprintf(g_gen.idea, sizeof(g_gen.idea), "%s", WsUtf8(buf).c_str()); // (tests: the idea box)
            if (GetEnvironmentVariableW(L"MV_GEN_POSE", buf, 2048)) { // "<pose id>|<its drawing>"
                const std::wstring s = buf;
                const size_t bar = s.find(L'|');
                if (bar != std::wstring::npos) {
                    g_gen.layout = s.substr(bar + 1), g_gen.layoutDrawn = true;
                    { const std::wstring id = s.substr(0, bar); g_gen.layout = TrimmedPose(g_gen.layout, std::string(id.begin(), id.end())); }
                    const std::wstring id = s.substr(0, bar);
                    g_gen.poseId.assign(id.begin(), id.end());
                    g_gen.layoutName = "Pose " + g_gen.poseId;
                    g_gen.poseWords = LibraryPoseWords(g_gen.poseId), g_gen.posePeople = LibraryPosePeople(g_gen.poseId);
                    g_gen.poseAsWords = GetEnvironmentVariableW(L"MV_GEN_POSE_WORDS", nullptr, 0) > 0; // (its Words only)
                }
            }
            if (GetEnvironmentVariableW(L"MV_GEN_CINEMA", e, 8)) {
                g_gen.cinema = true;
                for (int k = 0; k < kCineCount; ++k) g_gen.cine[k] = kCine[k].start;
                g_gen.cine[0] = 2, g_gen.cine[1] = 3; // (noir, the 1950s)
            }
            if (GetEnvironmentVariableW(L"MV_GEN_MANY", e, 8)) { // MV_GEN_MANY=N: the prompt is a theme for Many at once
                g_gen.theme = p, g_gen.prompt.clear();
                g_gen.many = std::clamp(_wtoi(e), 1, 40);
                g_gen.manyFut = std::async(std::launch::async, WriteMany, app.comfy.url, OneLine(g_gen.theme), g_gen.many);
            } else if (GetEnvironmentVariableW(L"MV_GEN_SEQUENCE", e, 8)) { // MV_GEN_SEQUENCE="151": one job per engine digit, sent at once (queue tests)
                for (const wchar_t* c = e; *c; ++c) {
                    std::string by;
                    MakeOne(app, g_gen.prompt + " (" + std::to_string(c - e + 1) + ")", std::clamp(*c - L'0', 0, kEngineCount - 1), 1, by);
                }
            } else if (GetEnvironmentVariableW(L"MV_GEN_WRITE", e, 8)) { // MV_GEN_WRITE: Write it out for me instead of Make (MV_GEN_WSTYLE: with this .txt)
                if (!g_ws.loaded) LoadWriterStyles();
                if (GetEnvironmentVariableW(L"MV_GEN_WSTYLE", buf, 2048)) g_ws.files = {buf}, g_ws.chosen = 0;
                else g_ws.chosen = -1;
                g_gen.improveFut = std::async(std::launch::async, Improve, app.comfy.url, g_gen.prompt, std::string(kEngineName[g_gen.engine]), ChosenWriterStyle());
            } else {
                Make(app);
            }
        }
    }
    if (g_gen.improveFut.valid() && g_gen.improveFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const std::string r = g_gen.improveFut.get();
        if (GetEnvironmentVariableW(L"MV_GEN_WRITE", nullptr, 0) > 0) { // (tests: what it wrote)
            std::ofstream t(fs::path(WriterStylesPath()).parent_path() / L"writer_test_out.txt", std::ios::binary | std::ios::trunc);
            t << r;
        }
        if (r.rfind("error: ", 0) == 0) g_gen.note = r.substr(7);
        else if (!r.empty()) g_gen.before = g_gen.prompt, g_gen.prompt = r, g_gen.note = "The writer filled it in (Undo puts yours back)";
    }
    const bool krea = !GenerateKreaModel().empty(), moody = !MoodyMossModel().empty();
    if (g_gen.engine == kKrea && !krea) g_gen.engine = kAuto;
    if (g_gen.engine == kMoody && !moody) g_gen.engine = kAuto;

    // who makes it
    for (int e = 0; e < kEngineCount; ++e) {
        if (e) ui::SameLineIfFits(kEngineName[e]);
        if (e == kMoody && !moody) continue; // (only when its file is there)
        ImGui::BeginDisabled(e == kKrea && !krea);
        if (Chip(kEngineName[e], g_gen.engine == e)) g_gen.engine = e;
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", kEngineAbout[e]);
    }
    ui::SameLineIfFits("Design with exact text...");
    if (ImGui::Button("Design with exact text...")) OpenLayoutWithIdea(app, g_gen.prompt);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("Covers, posters, thumbnails, memes: MOSS-Image draws it from boxes the prompt writer plans from this idea, which you can move "
                          "and reword (Edits > Layout designs)");
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
                                            g_gen.refs.empty() ? "Describe what to make   (Ctrl+Enter)" : "Anything to add? (optional: the pictures and their roles below say what to make)   (Ctrl+Enter)");
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
    if (!g_ws.loaded) LoadWriterStyles();
    const std::string shown = g_ws.chosen < 0 ? std::string("Default way") : WsUtf8(fs::path(g_ws.files[g_ws.chosen]).stem().wstring());
    ImGui::BeginDisabled(g_gen.improveFut.valid() || g_gen.prompt.empty());
    if (ImGui::Button(g_gen.improveFut.valid() ? "Writing..." : "Write it out for me")) {
        g_gen.note = "The prompt writer is filling it in...";
        g_gen.improveFut = std::async(std::launch::async, Improve, app.comfy.url, g_gen.prompt, std::string(kEngineName[g_gen.engine]), ChosenWriterStyle());
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("The local prompt writer turns your short idea into a full prompt (look, setting, camera, light, style), keeping your words.\n"
                          "How it writes: %s (the arrow beside it)", shown.c_str());
    // how it writes: its own way, or a .txt of yours (a prompt guide written for an LLM) - just an arrow, its name in the tooltips
    ImGui::SameLine(0, ui::Dp(2.0f));
    if (ImGui::Button("v##wstyle")) ImGui::OpenPopup("##wstylepop");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("How the prompt writer writes - now: %s.\nIts default way, or a .txt you load - instructions written for an LLM, such as a model's\n"
                          "prompt guide (the Moody MOSS one). The file is read each time, so edits to it count.", shown.c_str());
    if (ImGui::BeginPopup("##wstylepop")) {
        if (ImGui::Selectable("Default way", g_ws.chosen < 0)) g_ws.chosen = -1, SaveWriterStyles();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("The app's own instructions: one rich paragraph, your words kept");
        for (size_t k = 0; k < g_ws.files.size(); ++k) {
            ImGui::PushID(static_cast<int>(k));
            std::error_code ec;
            const bool there = fs::exists(g_ws.files[k], ec);
            const std::string name = WsUtf8(fs::path(g_ws.files[k]).filename().wstring()) + (there ? "" : "  (missing)");
            if (ImGui::Selectable(name.c_str(), g_ws.chosen == static_cast<int>(k))) g_ws.chosen = static_cast<int>(k), SaveWriterStyles();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", WsUtf8(g_ws.files[k]).c_str());
            ImGui::PopID();
        }
        ImGui::Separator();
        if (ImGui::Selectable("Load a .txt...")) {
            wchar_t buf[MAX_PATH * 2] = {};
            OPENFILENAMEW ofn = {sizeof(ofn)};
            ofn.hwndOwner = GetActiveWindow();
            ofn.lpstrFilter = L"Text\0*.txt;*.md\0All files\0*.*\0";
            ofn.lpstrFile = buf;
            ofn.nMaxFile = MAX_PATH * 2;
            ofn.lpstrTitle = L"How the prompt writer should write (its instructions)";
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameW(&ofn)) {
                const std::wstring p = buf;
                auto it = std::find_if(g_ws.files.begin(), g_ws.files.end(), [&](const std::wstring& f) { return _wcsicmp(f.c_str(), p.c_str()) == 0; });
                if (it == g_ws.files.end()) g_ws.files.push_back(p), it = g_ws.files.end() - 1;
                g_ws.chosen = static_cast<int>(it - g_ws.files.begin());
                SaveWriterStyles();
            }
        }
        if (g_ws.chosen >= 0 && ImGui::Selectable("Forget this one")) {
            g_ws.files.erase(g_ws.files.begin() + g_ws.chosen);
            g_ws.chosen = -1;
            SaveWriterStyles();
        }
        ImGui::EndPopup();
    }
    if (g_ws.chosen >= 0) { // (not the default: say so, briefly)
        ImGui::SameLine();
        ImGui::TextDisabled("%s", shown.size() > 18 ? (shown.substr(0, 16) + "..").c_str() : shown.c_str());
    }
    if (!g_gen.before.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Undo")) g_gen.prompt = g_gen.before, g_gen.before.clear(), g_gen.ideaKey.clear();
    }
    DrawLorasButton(app, krea);
    DrawLookButton(app);
    DrawPicturesPanel(app);
    if (!g_gen.note.empty()) ImGui::TextColored(ui::kAccent, "%s", g_gen.note.c_str());
    PollMany(app);
}

// ---- many at once: a theme -> the writer writes N different prompts -> each is made by the maker chosen on Make (Auto decides for each)
namespace {
void PollMany(AppState& app) {
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
                if (MakeOne(app, p.get<std::string>(), g_gen.engine, 1, by, true)) ++sent, ++byCount[by];
            }
            std::string who;
            for (const auto& [k, v] : byCount) who += (who.empty() ? "" : ", ") + k + " " + std::to_string(v);
            g_gen.note = "Wrote " + std::to_string(g_gen.manyPrompts.size()) + " prompts and sent " + std::to_string(sent) + (who.empty() ? "" : " (" + who + ")");
        }
    }
}
} // namespace

// Edits > People & outfits: pictures of people, outfits, places and a pose; each picture's role says how it is used, and Make writes the edit
void DrawPeopleComposer(AppState& app) {
    g_genCur = &g_gens[1];
    { // tests: MV_PEOPLE_REFS="a|b" (+ MV_PEOPLE_CHANGE: picture 1 is changed; MV_PEOPLE_POSE="<id>|<drawing>"; MV_PEOPLE_PROMPT: words) and Make once
        static bool done = false;
        wchar_t buf[2048] = {}, e[8] = {};
        if (!done && GetEnvironmentVariableW(L"MV_PEOPLE_REFS", buf, 2048)) {
            done = true;
            const std::wstring s = buf;
            for (size_t a = 0; a <= s.size();) {
                size_t b = s.find(L'|', a);
                if (b == std::wstring::npos) b = s.size();
                if (b > a) AddRef(s.substr(a, b - a));
                a = b + 1;
            }
            if (!g_gen.refs.empty() && GetEnvironmentVariableW(L"MV_PEOPLE_CHANGE", e, 8)) {
                RefTag& t = g_tags[g_gen.refs[0]];
                t.role = kRoleChange, t.mine = true;
            }
            if (GetEnvironmentVariableW(L"MV_PEOPLE_POSE", buf, 2048)) {
                const std::wstring ps = buf;
                const size_t bar = ps.find(L'|');
                if (bar != std::wstring::npos) {
                    const std::wstring id = ps.substr(0, bar);
                    g_gen.layout = ps.substr(bar + 1), g_gen.layoutDrawn = true, g_gen.poseId.assign(id.begin(), id.end());
                    g_gen.layoutName = "Pose " + g_gen.poseId;
                    g_gen.poseWords = LibraryPoseWords(g_gen.poseId), g_gen.posePeople = LibraryPosePeople(g_gen.poseId);
                }
            }
            if (GetEnvironmentVariableW(L"MV_PEOPLE_PROMPT", buf, 2048)) {
                const std::wstring w = buf;
                g_gen.prompt = WsUtf8(w);
            }
            Make(app);
        }
    }
    ImGui::TextDisabled("Put people into pictures, dress them, pose them. Add their pictures below; Make writes the edit from what each one is for.");
    // who draws it: the picture editor (the same choice as Settings > Picture edits)
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Made by");
    ImGui::SameLine();
    const bool flux = app.comfy.editEngine == "flux3";
    if (Chip("FLUX 3 Klein", flux)) app.comfy.editEngine = "flux3", SaveComfySettings(app.comfy);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Quick (about 15-20 s), keeps faces well");
    ImGui::SameLine(0, ui::Dp(4.0f));
    if (Chip("Qwen Image 2.1 Edit", !flux)) app.comfy.editEngine = "qwen", SaveComfySettings(app.comfy);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Slower; less censored; loads the edit LoRAs");
    // your words (optional) and Make
    if (g_gen.focus) {
        ImGui::SetKeyboardFocusHere();
        g_gen.focus = false;
    }
    const float makeW = ui::Dp(96.0f), boxH = ui::Dp(72.0f);
    WrappedBox("##peopleprompt", g_gen.prompt, ImVec2(ImGui::GetContentRegionAvail().x - makeW - ImGui::GetStyle().ItemSpacing.x, boxH));
    const bool ctrlEnter = ImGui::IsItemActive() && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Enter, false);
    if (g_gen.prompt.empty() && !ImGui::IsItemActive()) {
        const ImVec2 p = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + ImGui::GetStyle().FramePadding.x, p.y + ImGui::GetStyle().FramePadding.y), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                            "Anything to add? (optional: \"at the beach\", \"laughing\")   (Ctrl+Enter)");
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ui::kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::kAccentHover);
    if (ImGui::Button("Make##people", ImVec2(makeW, boxH)) || ctrlEnter) Make(app);
    ImGui::PopStyleColor(2);
    if (!ChangingAPicture()) { // (a new picture: its shape)
        for (int s = 0; s < kShapeCount; ++s) {
            if (s) ui::SameLineIfFits(kShapes[s].name);
            if (Chip(kShapes[s].name, g_gen.shape == s)) g_gen.shape = s;
        }
        ui::SameLineIfFits("x 1 2 4 ");
    }
    ImGui::TextDisabled("x");
    for (int n : {1, 2, 4}) {
        char lbl[12];
        snprintf(lbl, sizeof(lbl), "%d##pn", n);
        ImGui::SameLine(0, ui::Dp(4.0f));
        if (Chip(lbl, g_gen.times == n)) g_gen.times = n;
    }
    DrawLorasButton(app, false);
    DrawPicturesPanel(app);
    if (!g_gen.note.empty()) ImGui::TextColored(ui::kAccent, "%s", g_gen.note.c_str());
}

void DrawGenerateMany(AppState& app) {
    g_genCur = &g_gens[0];
    PollMany(app);
    {
        ImGui::TextDisabled("Say the theme (a style, a place, a character, a mood...); the prompt writer writes that many different prompts and each is made "
                            "by %s, %s (as chosen on Make).", kEngineName[g_gen.engine], g_gen.engine == kVideo ? "as a video" : kShapes[g_gen.shape].name);
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
        if (!g_gen.manyPrompts.empty()) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        if (!g_gen.manyPrompts.empty() && ImGui::TreeNode("The prompts it wrote")) {
            for (size_t i = 0; i < g_gen.manyPrompts.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Button("Use")) g_gen.prompt = g_gen.manyPrompts[i], g_gen.focus = true, g_genSub = 0;
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Put it in Make's box (to change it, or make more of it)");
                ImGui::SameLine();
                ImGui::PushTextWrapPos(0);
                ImGui::TextUnformatted(g_gen.manyPrompts[i].c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        if (!g_gen.note.empty()) ImGui::TextColored(ui::kAccent, "%s", g_gen.note.c_str());
    }
}
