// Layout studio: designs with exact words (magazine and album covers, posters, thumbnails, memes...) drawn by MOSS-Image from a LAYOUT - boxes for
// each text, thing and character, with what each one is and says (MOSS's own "summary + items" JSON, bbox yxyx in pixels from the canvas
// centre). The prompt writer plans the boxes from an idea (after the official MOSS-Image prompt-enhancer skill); you move, resize and reword them
// on the canvas; MOSS draws it.
// Tested 2026-10-18 (RTX 3090, the same designs and seeds): with boxes, MOSS 8-step (SFT + lodestones' DMD LoRA) put every line of a busy
// magazine cover where it was asked and spelled it right in 3.5-5 s, in English and in Japanese; without boxes MOSS garbled the small print, and
// Krea 2 wrote "GUPUS" and made up a Japanese magazine name. Short texts (a meme, a sale poster) came out right with all of them.
// Editing: the same seed with one box reworded changed only that box; repainting only the changed boxes (a mask) keeps everything else exact,
// and fixed Krea's misspelt headline seamlessly. Text OVER a photo cannot be made this way (the box is drawn anew: a knight lost his head under
// a meme caption) - that is PhotoCraft's Design. Box descriptions must be English: Japanese ones came back as tiny printed labels.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>
#include <shellapi.h>

#include "app_state.h"
#include "clef.h"
#include "comfy.h"
#include "image_texture.h"
#include "imgui.h"
#include "json.hpp"
#include "notify.h"
#include "ui_extra.h"
#include "ui_theme.h"
#include "video_util.h"
#include "vision_prompt.h"
#include "win_dialogs.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::string U8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring W16(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
int StrResize(ImGuiInputTextCallbackData* d) {
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* s = static_cast<std::string*>(d->UserData);
        s->resize(static_cast<size_t>(d->BufTextLen));
        d->Buf = s->data();
    }
    return 0;
}
bool Line(const char* id, const char* hint, std::string& v, float width) {
    ImGui::SetNextItemWidth(width);
    return ImGui::InputTextWithHint(id, hint, v.data(), v.capacity() + 1, ImGuiInputTextFlags_CallbackResize, StrResize, &v);
}
bool Box(const char* id, std::string& v, float height) {
    return ImGui::InputTextMultiline(id, v.data(), v.capacity() + 1, ImVec2(-FLT_MIN, height), ImGuiInputTextFlags_CallbackResize, StrResize, &v);
}

// ---- the models (ComfyUI's Comfy-Org/MOSS-Image files; the 8-step LoRA renamed so it says what it is)
struct ModelFile {
    const char* sub;  // models/<sub>
    const char* name; // the file there
    const char* url;
    const char* sha;
    double gb;
};
const ModelFile kSft = {"diffusion_models", "moss_image_sft_int8_convrot.safetensors",
                        "https://huggingface.co/Comfy-Org/MOSS-Image/resolve/main/diffusion_models/moss_image_sft_int8_convrot.safetensors",
                        "b4d1475bc7eeffd7b809e82ea13a1fba1fb85c4e18365af3813722faa5c0fe4f", 5.41};
const ModelFile kTe = {"text_encoders", "qwen3.5_9b_w4a8_gptq.safetensors",
                       "https://huggingface.co/Comfy-Org/MOSS-Image/resolve/main/text_encoders/qwen3.5_9b_w4a8_gptq.safetensors",
                       "4cc6a1543ad2061806da46f35f90cadb4801dcba246c44782cf42ec0feb6407c", 7.34};
const ModelFile kVae = {"vae", "flux2-vae.safetensors", "https://huggingface.co/Comfy-Org/MOSS-Image/resolve/main/vae/flux2-vae.safetensors",
                        "868fe7b343cc8f3a19dbcfcafbc3d5f888802be3f89bd81b65b3621a066ce8f3", 0.34};
const ModelFile kLora = {"loras", "moss_image_sft_8step_dmd_lora.safetensors", "https://huggingface.co/lodestones/MOSS-Image-SFT-8-steps-dmd/resolve/main/lora.safetensors",
                         "4abb14e39c1a59018907581e609f43616ec6810126144ec2b1d2e6f8969628fe", 1.19};
const ModelFile kBase = {"diffusion_models", "moss_image_base_int8_convrot.safetensors",
                         "https://huggingface.co/Comfy-Org/MOSS-Image/resolve/main/diffusion_models/moss_image_base_int8_convrot.safetensors",
                         "f601c1e1161915fb3fea003da0c1782e374dd960fbb3978bc0e4e3fa1f2d8e6f", 5.41};

// ---- kinds of design: the canvas, and the words MOSS's layouts use for them (summary.meta)
struct Kind {
    const char* name;
    int w, h;
    const char* meta;
    const char* canvas;
    const char* hint; // an example idea
};
const Kind kKinds[] = {
    {"Magazine cover", 1024, 1344, "magazine cover", "single", "a tech magazine called PIXEL about the best GPUs for AI, a scientist holding a graphics card"},
    {"Album / song cover", 1024, 1024, "album cover", "single", "synthwave album \"Neon Tides\" by The Midnight Foxes, a chrome fox over a neon ocean"},
    {"Poster / promo", 1024, 1536, "advertising poster", "single", "summer sale, 50% off sneakers, this weekend only, June 21-23"},
    {"Movie poster", 1024, 1536, "movie poster", "single", "a sci-fi thriller called LAST SIGNAL, an astronaut alone on a red planet, in cinemas this winter"},
    {"YouTube thumbnail", 1360, 768, "youtube thumbnail", "single", "a shocked man pointing at a glowing 3D knight, \"360 FROM ONE PHOTO\", FREE!"},
    {"Meme", 1024, 1024, "meme", "single", "a confused cat at a laptop: when the code works but you don't know why"},
    {"Social post (4:5)", 1024, 1280, "social media post", "single", "a cafe announcing its new matcha latte, cosy and green"},
    {"Story / phone (9:16)", 864, 1536, "phone story", "single", "a travel story: 48 hours in Lisbon, yellow trams and pastel streets"},
    {"Event flyer", 1024, 1344, "event flyer", "single", "a jazz night at the Blue Room, Friday 8 PM, free entry"},
    {"Book cover", 1024, 1536, "book cover", "single", "a fantasy novel THE ASH CROWN by Mara Vell, a burning crown in the snow"},
    {"Comic page", 1024, 1536, "comic page", "multi_panel", "a four-panel comic: a robot tries to make coffee and floods the kitchen"},
    {"Infographic", 1024, 1536, "infographic", "single", "how to brew pour-over coffee in 5 steps"},
    {"Custom size", 1024, 1024, "design", "single", "anything with words in it"},
};
constexpr int kKindCount = static_cast<int>(sizeof(kKinds) / sizeof(kKinds[0]));

struct Item {
    std::string label = "text"; // text, object, character
    std::string caption, text;
    float x1 = 0, y1 = 0, x2 = 0, y2 = 0; // top-left pixels
    bool dirty = true;                    // changed since the last picture (repainted on "only what changed")
};

struct Studio {
    bool open = false, focus = false;
    int kind = 0;
    int w = 1024, h = 1344;
    std::string idea, caption, meta;
    std::vector<Item> items;
    int sel = -1;
    // the picture: a blank canvas, or a picture whose words / things are changed (only the boxes are made again)
    std::wstring base;
    LoadedImage baseTex, resultTex;
    std::wstring result; // the last picture made
    bool showBoxes = true;
    int quality = 0;     // 0 Fast (8 steps), 1 Best (Base, 50 steps)
    long long seed = -1; // kept: a reworded box keeps the rest of the picture
    bool keepSeed = true;
    // planning
    std::future<std::string> planFut;
    std::string status, error;
    // drawing
    int job = 0;
    bool jobInpaint = false;
    // canvas editing
    int drag = 0;         // 0 none, 1 move, 2 resize (corner), 3 new box
    int corner = 0;       // 0 tl, 1 tr, 2 bl, 3 br
    std::string newLabel; // the kind of box being drawn ("" = not drawing)
    ImVec2 dragStart;
    // changing it in words: the writer's patch, what it said, and the layout before it (undo)
    std::string change;
    std::future<std::string> editFut;
    std::vector<std::string> notes;
    std::vector<Item> undoItems;
    std::string undoCaption;
    std::vector<std::array<float, 4>> undoErased;
    bool canUndo = false, captionChanged = false;
    // places to paint over with the background on the next "only what changed" (a box moved away or removed)
    std::vector<std::array<float, 4>> erased;
    float dragFrom[4] = {0, 0, 0, 0}; // the picked box before a drag
    // what ComfyUI has (asked while the page is open, or while Generate wants MOSS)
    double wantedAt = -1000;
    std::future<json> askFut;
    double askedAt = -1000;
    bool answered = false, supported = false, haveSft = false, haveBase = false, haveTe = false, haveVae = false, haveLora = false;
};
Studio g_s;

struct Download {
    std::atomic<bool> running{false}, failed{false};
    std::string what;
};
Download g_dl;

const Kind& CurKind() { return kKinds[std::clamp(g_s.kind, 0, kKindCount - 1)]; }

ImU32 LabelColor(const std::string& label, int alpha) {
    if (label == "text") return IM_COL32(70, 130, 255, alpha);
    if (label == "character") return IM_COL32(60, 190, 110, alpha);
    return IM_COL32(245, 140, 40, alpha);
}
const char* LabelName(const std::string& label) { return label == "text" ? "Text" : label == "character" ? "Character" : "Object"; }

// MOSS's prompt: the layout JSON, boxes as [y_min, x_min, y_max, x_max] in pixels with the canvas centre as 0 (its "abs_centralized" / "yxyx")
std::string CompileLayout(const std::vector<Item>& items) {
    json summary = {{"caption", g_s.caption}};
    if (!g_s.meta.empty()) summary["meta"] = json::array({g_s.meta});
    summary["canvas"] = CurKind().canvas;
    json out = json::array();
    for (const Item& it : items) {
        json j = {{"label", it.label}, {"caption", it.caption.empty() ? std::string(LabelName(it.label)) : it.caption}};
        if (it.label == "text") j["text"] = it.text;
        j["bbox"] = {static_cast<int>(std::floor(it.y1)) - g_s.h / 2, static_cast<int>(std::floor(it.x1)) - g_s.w / 2,
                     static_cast<int>(std::ceil(it.y2)) - g_s.h / 2, static_cast<int>(std::ceil(it.x2)) - g_s.w / 2};
        out.push_back(j);
    }
    return json({{"summary", summary}, {"items", out}, {"bbox_coord_space", "abs_centralized"}, {"bbox_format", "yxyx"}}).dump(-1, ' ', false);
}

// ---- the prompt writer plans the boxes (the official MOSS-Image prompt-enhancer skill's rules, and what the tests showed)
const char* const kPlanSystem =
    "You design layouts for MOSS-Image, an image model that draws a picture from a layout: a global description plus boxes, each box a text, an "
    "object or a character with what it looks like and where it is. Answer with ONLY one JSON object, no markdown:\n"
    "{\"summary\":{\"caption\":\"...\"},\"items\":[{\"label\":\"text|object|character\",\"caption\":\"...\",\"text\":\"(text items only)\","
    "\"bbox\":[x_min,y_min,x_max,y_max]}]}\n"
    "Rules:\n"
    "- bbox in actual pixels from the canvas top-left, inside the canvas size you are given. Integers.\n"
    "- summary.caption: the whole image - subject, setting, medium/style, lighting, palette, mood, typography treatment - one rich paragraph in English.\n"
    "- One box per coherent visible unit: each title, cover line, tagline, price, date, logo word; each main person (label character) and each "
    "important thing (label object). A box is a placement, not a border or a panel to draw.\n"
    "- Captions are ALWAYS English, describing appearance (for text: colour, weight, font style, size, alignment, outline or glow; never the words "
    "themselves). Non-English captions get printed as stray labels.\n"
    "- The text field holds the exact visible characters, verbatim, in the language the user wrote them (Japanese stays Japanese). Use \\n for "
    "intended line breaks. Write real copy, never placeholders like 'headline here'. Keep the user's wording exactly when they give it.\n"
    "- Size text boxes for their glyphs: a short title gets a wide, tall box; a long line needs a long box; a small caption a short one. Text boxes "
    "do not overlap each other. Keep faces clear of text unless the design really overlaps them (a magazine masthead may sit behind a head).\n"
    "- Establish reading order and hierarchy: one dominant title, then secondary lines, then small print. Leave breathing room at the edges.\n"
    "- Use the kind of design you are given (magazine cover, album cover, poster, meme, thumbnail...) and its conventions.\n"
    "- Characters and objects may overlap each other and the background naturally.\n"
    "- EVERY item, text items included, MUST have its \"bbox\" with four numbers. Never describe a position only in words.\n"
    "Example for a 1024 x 1344 magazine cover:\n"
    "{\"summary\":{\"caption\":\"A glossy tech magazine cover, a smiling scientist with short copper hair in a white lab coat holds a black graphics card "
    "in a bright electronics store; bold white and yellow condensed typography.\"},\"items\":["
    "{\"label\":\"text\",\"caption\":\"Huge bold white condensed masthead across the top.\",\"text\":\"PIXEL\",\"bbox\":[64,40,960,230]},"
    "{\"label\":\"text\",\"caption\":\"Large yellow bold headline on two lines, left side.\",\"text\":\"THE 7 BEST\\nGPUs FOR AI\",\"bbox\":[56,330,560,520]},"
    "{\"label\":\"character\",\"caption\":\"A young woman with short copper hair in a white lab coat, smiling, holding a graphics card at chest height.\","
    "\"bbox\":[380,300,1000,1344]},"
    "{\"label\":\"text\",\"caption\":\"Bold white cover line, lower left.\",\"text\":\"RTX 5090 vs 5070 Ti\",\"bbox\":[48,1080,520,1150]}]}";

// The words the idea quotes (「...」 『...』 “...” "..."): they must be printed, exactly
std::vector<std::string> QuotedWords(const std::string& idea) {
    static const std::pair<std::string, std::string> kQuotes[] = {{"\xE3\x80\x8C", "\xE3\x80\x8D"}, {"\xE3\x80\x8E", "\xE3\x80\x8F"},
                                                                  {"\xE2\x80\x9C", "\xE2\x80\x9D"}, {"\"", "\""}};
    std::vector<std::string> out;
    for (const auto& q : kQuotes) {
        for (size_t a = idea.find(q.first); a != std::string::npos; a = idea.find(q.first, a + 1)) {
            const size_t from = a + q.first.size(), b = idea.find(q.second, from);
            if (b == std::string::npos) break;
            const std::string w = idea.substr(from, b - from);
            if (!w.empty() && w.size() < 200) out.push_back(w);
            a = b;
        }
    }
    return out;
}

std::string Squash(const std::string& s) { // for comparing words: no spaces or line breaks, lower case
    std::string o;
    for (char c : s) {
        if (c != ' ' && c != '\n' && c != '\r' && c != '\t') o += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return o;
}

// The quoted words that no text item prints
std::vector<std::string> MissingWords(const json& j, const std::vector<std::string>& quoted) {
    std::string all;
    if (j.is_object() && j.contains("items") && j["items"].is_array()) {
        for (const json& it : j["items"]) {
            if (it.is_object() && it.contains("text") && it["text"].is_string()) all += Squash(it["text"].get<std::string>()) + "|";
        }
    }
    std::vector<std::string> missing;
    for (const std::string& q : quoted) {
        if (all.find(Squash(q)) == std::string::npos) missing.push_back(q);
    }
    return missing;
}

std::string PlanLayout(std::string comfyUrl, std::string idea, std::string kind, int w, int h, std::string current, std::wstring picture) {
    const ClefSession session = WriterAcquire(comfyUrl, [](const std::string&) {});
    if (!session.ok) return "error: the prompt writer could not be started (Clef mode tab > Prompt writer)";
    std::string user = "Kind of design: " + kind + ". Canvas: " + std::to_string(w) + " x " + std::to_string(h) + " pixels.\nThe idea: " + idea;
    if (!current.empty()) user += "\n\nThe current layout (keep what the idea does not change, keep its boxes where they are unless asked):\n" + current;
    std::vector<std::string> images;
    if (!picture.empty()) {
        user += "\n\nThe attached picture is the canvas being changed: only the boxes will be drawn again, everything else stays. Describe the picture in "
                "summary.caption, and put the boxes exactly over what has to change (cover the old words completely).";
        const std::string url = ImageDataUrl(picture, 1024);
        if (!url.empty()) images.push_back(url);
    }
    const std::vector<std::string> quoted = QuotedWords(idea);
    std::string reply, err;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!WriterChat(kPlanSystem, user, images, 4000, 0.5, reply, &err)) {
            WriterRelease(session, false);
            return "error: the prompt writer did not answer" + (err.empty() ? std::string() : ": " + err);
        }
        // every item placed? (a writer left the text boxes' bbox out and said where they go in words)
        const size_t a = reply.find('{'), b = reply.rfind('}');
        const json j = a == std::string::npos || b == std::string::npos ? json() : json::parse(reply.substr(a, b - a + 1), nullptr, false);
        int missing = 0;
        if (j.is_object() && j.contains("items") && j["items"].is_array()) {
            for (const json& it : j["items"]) missing += !it.is_object() || !it.contains("bbox") || !it["bbox"].is_array() || it["bbox"].size() != 4 ? 1 : 0;
        } else {
            missing = 1;
        }
        const std::vector<std::string> lost = MissingWords(j, quoted);
        if (!missing && lost.empty()) break;
        user += "\n\nYour previous answer:\n" + reply + "\n\n";
        if (missing)
            user += "It is missing the bbox of " + std::to_string(missing) + " item(s): every item needs \"bbox\":[x_min,y_min,x_max,y_max] in pixels of the " +
                    std::to_string(w) + " x " + std::to_string(h) + " canvas. ";
        if (!lost.empty()) {
            user += "These quoted words of the idea are not printed by any text item (each must be, exactly, in its own text item):";
            for (const std::string& q : lost) user += " \"" + q + "\"";
            user += ". ";
        }
        user += "Answer again with the whole JSON object.";
    }
    WriterRelease(session, false);
    // still missing words: added as text items without a place (ReadPlan stacks them; move them on the canvas)
    const size_t a = reply.find('{'), b = reply.rfind('}');
    json j = a == std::string::npos || b == std::string::npos ? json() : json::parse(reply.substr(a, b - a + 1), nullptr, false);
    if (j.is_object() && j.contains("items") && j["items"].is_array()) {
        const std::vector<std::string> lost = MissingWords(j, quoted);
        for (const std::string& q : lost) j["items"].push_back({{"label", "text"}, {"caption", "Bold clean lettering, easy to read."}, {"text", q}});
        if (!lost.empty()) return j.dump();
    }
    return reply;
}

bool ReadPlan(const std::string& reply, std::string& why) {
    const size_t a = reply.find('{'), b = reply.rfind('}');
    if (a == std::string::npos || b == std::string::npos || b < a) {
        why = "no JSON in the answer";
        return false;
    }
    const json j = json::parse(reply.substr(a, b - a + 1), nullptr, false);
    if (!j.is_object() || !j.contains("items") || !j["items"].is_array()) {
        why = "the answer has no items";
        return false;
    }
    std::vector<Item> items;
    float freeY = 24.0f; // texts still without a place: stacked in rows from the top (better than lost; move them on the canvas)
    for (const json& it : j["items"]) {
        if (!it.is_object()) continue;
        if (!it.contains("bbox") || !it["bbox"].is_array() || it["bbox"].size() != 4) {
            if (it.value("label", std::string()) != "text" || it.value("text", std::string()).empty()) continue;
            Item n;
            n.label = "text";
            n.caption = it.value("caption", std::string());
            n.text = it.value("text", std::string());
            const float rowH = std::max(64.0f, g_s.h * 0.08f);
            n.x1 = g_s.w * 0.06f, n.x2 = g_s.w * 0.94f, n.y1 = std::min(freeY, g_s.h - rowH), n.y2 = n.y1 + rowH;
            freeY += rowH + 16.0f;
            items.push_back(n);
            continue;
        }
        Item n;
        n.label = it.value("label", std::string("object"));
        if (n.label != "text" && n.label != "character") n.label = "object";
        n.caption = it.value("caption", std::string());
        n.text = it.value("text", std::string());
        if (n.label == "text" && n.text.empty()) continue;
        float v[4];
        for (int k = 0; k < 4; ++k) v[k] = it["bbox"][k].is_number() ? it["bbox"][k].get<float>() : 0.0f;
        n.x1 = std::clamp(std::min(v[0], v[2]), 0.0f, static_cast<float>(g_s.w));
        n.x2 = std::clamp(std::max(v[0], v[2]), 0.0f, static_cast<float>(g_s.w));
        n.y1 = std::clamp(std::min(v[1], v[3]), 0.0f, static_cast<float>(g_s.h));
        n.y2 = std::clamp(std::max(v[1], v[3]), 0.0f, static_cast<float>(g_s.h));
        if (n.x2 - n.x1 < 8 || n.y2 - n.y1 < 8) continue;
        items.push_back(n);
    }
    if (items.empty()) {
        why = "no usable boxes";
        return false;
    }
    if (j.contains("summary") && j["summary"].is_object()) g_s.caption = j["summary"].value("caption", g_s.caption);
    g_s.items = std::move(items);
    g_s.sel = -1;
    return true;
}

// The current layout in the writer's own form (top-left pixels), for "change it"
std::string DraftJson() {
    json items = json::array();
    for (const Item& it : g_s.items) {
        json j = {{"label", it.label}, {"caption", it.caption}, {"bbox", {std::lround(it.x1), std::lround(it.y1), std::lround(it.x2), std::lround(it.y2)}}};
        if (it.label == "text") j["text"] = it.text;
        items.push_back(j);
    }
    return json({{"summary", {{"caption", g_s.caption}}}, {"items", items}}).dump(1);
}

// ---- the ComfyUI graph: MOSS-Image from the layout; with a picture, only the boxes (a mask) are drawn again and the rest pasted back as it was
json BuildGraph(const std::string& prompt, bool inpaint) {
    // Best: MOSS-Image-SFT (the file Fast uses) at its official settings - CFG 3, shift 5 (ComfyUI's default for MOSS), the negative branch
    // without layer 22 (its "uncond_skip_layers=[22]"), an empty negative - at 25 steps. Tested 2026-10-26 (10 prompts: 4 simple, 4 complex,
    // 2 layouts, the same seeds) against the Base model at 50 steps, the Best before: every chalkboard and sign right where Base garbled two,
    // every person in the complex scenes as asked (the sleeping boy, the jumping guitarist), sharper faces, in 15.5 s against 30 s; 25 steps
    // looked the same as 50.
    const bool best = g_s.quality == 1 && g_s.haveSft;
    json g;
    auto node = [&](const char* id, const char* cls, json inputs) { g[id] = {{"class_type", cls}, {"inputs", std::move(inputs)}, {"_meta", {{"title", cls}}}}; };
    node("1", "UNETLoader", {{"unet_name", kSft.name}, {"weight_dtype", "default"}});
    json model = json::array({"1", 0});
    if (!best) {
        node("2", "LoraLoaderModelOnly", {{"model", model}, {"lora_name", kLora.name}, {"strength_model", 1.0}});
        model = json::array({"2", 0});
    } else {
        node("11", "SkipLayerGuidanceDiTSimple", {{"model", model}, {"double_layers", ""}, {"single_layers", "22"}, {"start_percent", 0.0}, {"end_percent", 1.0}});
        model = json::array({"11", 0});
    }
    node("3", "CLIPLoader", {{"clip_name", kTe.name}, {"type", "moss_image"}, {"device", "default"}});
    node("4", "VAELoader", {{"vae_name", kVae.name}});
    node("5", "CLIPTextEncode", {{"clip", json::array({"3", 0})}, {"text", prompt}});
    if (best) node("6", "CLIPTextEncode", {{"clip", json::array({"3", 0})}, {"text", ""}});
    else node("6", "ConditioningZeroOut", {{"conditioning", json::array({"5", 0})}});
    json latent;
    if (inpaint) {
        node("20", "LoadImage", {{"image", "@picture"}});
        node("21", "ImageScale", {{"image", json::array({"20", 0})}, {"upscale_method", "lanczos"}, {"width", g_s.w}, {"height", g_s.h}, {"crop", "center"}});
        node("22", "LoadImage", {{"image", "@mask"}});
        node("23", "ImageToMask", {{"image", json::array({"22", 0})}, {"channel", "red"}});
        node("24", "VAEEncode", {{"pixels", json::array({"21", 0})}, {"vae", json::array({"4", 0})}});
        node("25", "SetLatentNoiseMask", {{"samples", json::array({"24", 0})}, {"mask", json::array({"23", 0})}});
        latent = json::array({"25", 0});
    } else {
        node("7", "EmptyFlux2LatentImage", {{"width", g_s.w}, {"height", g_s.h}, {"batch_size", 1}});
        latent = json::array({"7", 0});
    }
    node("8", "KSampler", {{"model", model}, {"positive", json::array({"5", 0})}, {"negative", json::array({"6", 0})}, {"latent_image", latent}, {"seed", 1},
                           {"steps", best ? 25 : 8}, {"cfg", best ? 3.0 : 1.0}, {"sampler_name", "euler"}, {"scheduler", "simple"}, {"denoise", 1.0}});
    node("9", "VAEDecode", {{"samples", json::array({"8", 0})}, {"vae", json::array({"4", 0})}});
    json image = json::array({"9", 0});
    if (inpaint) { // outside the boxes: the picture exactly as it was
        node("26", "ImageCompositeMasked", {{"destination", json::array({"21", 0})}, {"source", image}, {"x", 0}, {"y", 0}, {"resize_source", false},
                                            {"mask", json::array({"23", 0})}});
        image = json::array({"26", 0});
    }
    node("10", "SaveImage", {{"images", image}, {"filename_prefix", "Layout/design"}});
    return g;
}

std::wstring DataDir() {
    wchar_t buf[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    return std::wstring(buf) + L"\\Try2Create\\layout";
}

// White where the boxes are (a little bigger, for the seam), black elsewhere
std::wstring WriteMask(const std::vector<const Item*>& boxes, const std::vector<std::array<float, 4>>& empty) {
    std::vector<unsigned char> px(static_cast<size_t>(g_s.w) * g_s.h * 4, 0);
    for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    std::vector<std::array<float, 4>> rects = empty;
    for (const Item* it : boxes) rects.push_back({it->x1, it->y1, it->x2, it->y2});
    for (const auto& r : rects) {
        const int x1 = std::max(0, static_cast<int>(r[0]) - 6), y1 = std::max(0, static_cast<int>(r[1]) - 6);
        const int x2 = std::min(g_s.w, static_cast<int>(r[2]) + 6), y2 = std::min(g_s.h, static_cast<int>(r[3]) + 6);
        for (int y = y1; y < y2; ++y) {
            for (int x = x1; x < x2; ++x) {
                unsigned char* p = &px[(static_cast<size_t>(y) * g_s.w + x) * 4];
                p[0] = p[1] = p[2] = 255;
            }
        }
    }
    std::error_code ec;
    fs::create_directories(DataDir(), ec);
    const std::wstring path = DataDir() + L"\\mask.png";
    return WriteThumbnailPng(path, px.data(), g_s.w, g_s.h) ? path : std::wstring();
}

bool Ready() { return g_s.supported && g_s.haveSft && g_s.haveTe && g_s.haveVae && g_s.haveLora; }

// onlyChanged: draw again only the boxes changed since the last picture (on it); else the whole design (or, on a picture, all the boxes)
void Make(AppState& app, bool onlyChanged) {
    if (g_s.items.empty()) {
        g_s.error = "Add some boxes first (or let the prompt writer plan them)";
        return;
    }
    std::wstring picture;
    std::vector<const Item*> boxes;
    if (onlyChanged && !g_s.result.empty()) {
        picture = g_s.result;
        for (const Item& it : g_s.items) {
            if (it.dirty) boxes.push_back(&it);
        }
    } else if (!g_s.base.empty()) {
        picture = g_s.base;
        for (const Item& it : g_s.items) boxes.push_back(&it);
    }
    const bool inpaint = !picture.empty();
    if (inpaint && boxes.empty() && g_s.erased.empty()) {
        g_s.error = "Nothing changed since the last picture";
        return;
    }
    if (g_s.seed < 0 || !g_s.keepSeed) g_s.seed = static_cast<long long>(std::random_device{}() % 1000000000ULL);
    ComfyJob job;
    job.apiWorkflow = BuildGraph(CompileLayout(g_s.items), inpaint).dump();
    if (inpaint) {
        const std::wstring mask = WriteMask(boxes, g_s.erased);
        if (mask.empty()) {
            g_s.error = "Could not write the mask";
            return;
        }
        job.rawUploads = {{"@picture", picture}, {"@mask", mask}};
    }
    job.seed = g_s.seed;
    job.times = 1;
    job.label = std::string("Layout: ") + CurKind().name + (inpaint ? " (only the boxes)" : "");
    g_s.job = SendComfyJob(app, std::move(job));
    g_s.jobInpaint = inpaint;
    g_s.error.clear();
    g_s.status = g_s.job ? (g_s.quality == 1 && g_s.haveSft ? "MOSS-Image is drawing it (Best: about 16 s)..." : "MOSS-Image is drawing it (about 5 s once loaded)...") : "";
    // the layout beside it, to open again later
    std::error_code ec;
    fs::create_directories(DataDir(), ec);
}

void SaveLayoutFile(const std::wstring& picture) {
    json items = json::array();
    for (const Item& it : g_s.items)
        items.push_back({{"label", it.label}, {"caption", it.caption}, {"text", it.text}, {"bbox", {it.x1, it.y1, it.x2, it.y2}}});
    const json j = {{"kind", CurKind().name}, {"width", g_s.w}, {"height", g_s.h}, {"caption", g_s.caption}, {"meta", g_s.meta}, {"idea", g_s.idea},
                    {"seed", g_s.seed}, {"quality", g_s.quality}, {"items", items}, {"base", U8(g_s.base)}};
    std::ofstream f(fs::path(picture).replace_extension(L".layout.json"), std::ios::binary | std::ios::trunc);
    if (f) f << j.dump(1);
}

bool LoadLayoutFile(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    const json j = json::parse(std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()), nullptr, false);
    if (!j.is_object() || !j.contains("items")) return false;
    const std::string kind = j.value("kind", std::string());
    for (int k = 0; k < kKindCount; ++k) {
        if (kind == kKinds[k].name) g_s.kind = k;
    }
    g_s.w = j.value("width", 1024), g_s.h = j.value("height", 1024);
    g_s.caption = j.value("caption", std::string());
    g_s.meta = j.value("meta", std::string(CurKind().meta));
    g_s.idea = j.value("idea", std::string());
    g_s.seed = j.value("seed", -1LL);
    g_s.quality = j.value("quality", 0);
    g_s.base = W16(j.value("base", std::string()));
    g_s.items.clear();
    for (const json& it : j["items"]) {
        Item n;
        n.label = it.value("label", std::string("text"));
        n.caption = it.value("caption", std::string());
        n.text = it.value("text", std::string());
        if (it.contains("bbox") && it["bbox"].size() == 4) n.x1 = it["bbox"][0], n.y1 = it["bbox"][1], n.x2 = it["bbox"][2], n.y2 = it["bbox"][3];
        n.dirty = false;
        g_s.items.push_back(n);
    }
    return true;
}

void SetResult(AppState& app, const std::wstring& file) {
    g_s.result = file;
    ReleaseImage(g_s.resultTex);
    g_s.resultTex = LoadedImage{};
    std::string err;
    LoadImageTexture(app.Device(), file, g_s.resultTex, &err);
}

void SetBase(AppState& app, const std::wstring& picture) {
    ReleaseImage(g_s.baseTex);
    g_s.baseTex = LoadedImage{};
    g_s.base = picture;
    if (picture.empty()) return;
    std::string err;
    LoadImageTexture(app.Device(), picture, g_s.baseTex, &err);
    // its size, about the same number of pixels (1 to 2.4 MP), in multiples of 16
    double w = std::max(1, g_s.baseTex.width), h = std::max(1, g_s.baseTex.height);
    const double mp = w * h / 1.0e6, scale = mp > 2.4 ? std::sqrt(2.4 / mp) : mp < 1.0 ? std::sqrt(1.0 / mp) : 1.0;
    g_s.w = std::max(256, static_cast<int>(w * scale) / 16 * 16);
    g_s.h = std::max(256, static_cast<int>(h * scale) / 16 * 16);
    g_s.kind = kKindCount - 1;
}

void StartDownloads(const std::wstring& models, std::vector<ModelFile> files) {
    if (g_dl.running || models.empty() || files.empty()) return;
    g_dl.running = true;
    g_dl.failed = false;
    std::thread([models, files]() {
        bool ok = true;
        for (const ModelFile& m : files) {
            g_dl.what = m.name;
            const std::wstring dir = models + L"\\" + W16(m.sub), dest = dir + L"\\" + W16(m.name);
            std::error_code ec;
            fs::create_directories(dir, ec);
            if (fs::exists(dest, ec)) continue;
            std::string out;
            bool good = RunCaptureHidden(L"curl.exe -L --fail --retry 5 --retry-delay 10 -C - --no-progress-meter -o \"" + dest + L".part\" \"" + W16(m.url) + L"\"", out, true) &&
                        fs::exists(dest + L".part", ec) && fs::file_size(dest + L".part", ec) > static_cast<uintmax_t>(m.gb * 0.9 * 1073741824.0);
            if (good) {
                std::string hash, h;
                RunCaptureIdle(L"powershell.exe -NoProfile -Command \"(Get-FileHash -Algorithm SHA256 -LiteralPath '" + dest + L".part').Hash\"", hash, false);
                for (char c : hash) {
                    if (isxdigit(static_cast<unsigned char>(c))) h += static_cast<char>(tolower(static_cast<unsigned char>(c)));
                }
                if (h != m.sha) fs::remove(dest + L".part", ec), good = false;
            }
            if (good) fs::rename(dest + L".part", dest, ec);
            if (!good || ec) {
                ok = false;
                break;
            }
        }
        g_dl.failed = !ok;
        g_dl.running = false;
    }).detach();
}

json AskComfy(std::string url) {
    auto has = [](const std::vector<std::string>& v, const char* name) { return std::find(v.begin(), v.end(), name) != v.end(); };
    const std::vector<std::string> types = ComfyListChoices(url, "CLIPLoader", "type");
    if (types.empty()) return json{{"answered", false}};
    const std::vector<std::string> unets = ComfyListChoices(url, "UNETLoader", "unet_name"), clips = ComfyListChoices(url, "CLIPLoader", "clip_name"),
                                   vaes = ComfyListChoices(url, "VAELoader", "vae_name"), loras = ComfyListLoras(url);
    return json{{"answered", true}, {"supported", has(types, "moss_image")}, {"sft", has(unets, kSft.name)}, {"base", has(unets, kBase.name)},
                {"te", has(clips, kTe.name)}, {"vae", has(vaes, kVae.name)}, {"lora", LoraListed(loras, kLora.name)}};
}

// ---- changing it in words: the prompt writer turns "make the date red, add a NEW badge" into a PATCH of the layout (only the boxes that change),
// never a rewritten layout - a small local model copying 10 boxes back changes some by accident, and every changed box is drawn again.
const char* const kEditSystem =
    "You edit a design made by MOSS-Image, an image model that draws a picture from a LAYOUT: a description of the whole picture plus numbered "
    "boxes, each a text (with its exact words), an object or a character, with an English description of how it looks and where it is.\n"
    "The picture already exists. When the user asks for a change, ONLY the boxes you change are drawn again, inside their box; everything else "
    "in the picture stays exactly as it is, pixel for pixel. So change as little as possible.\n\n"
    "Answer with ONLY one JSON object, no markdown:\n"
    "{\"notes\":[\"one short line per change, plain English\"],\n"
    " \"change\":[{\"id\":2,\"text\":\"...\",\"caption\":\"...\",\"bbox\":[x_min,y_min,x_max,y_max],\"label\":\"text|object|character\"}],\n"
    " \"add\":[{\"label\":\"text|object|character\",\"caption\":\"...\",\"text\":\"...\",\"bbox\":[x_min,y_min,x_max,y_max]}],\n"
    " \"remove\":[3],\n"
    " \"summary\":\"(leave out unless the WHOLE picture must change)\"}\n"
    "In \"change\" give the id and ONLY the fields that change; leave out empty lists.\n\n"
    "How to make each kind of change go the way the user wants:\n"
    "- New words: change only \"text\", exactly as the user wrote them (their language, their capitals, their punctuation). If the new words are "
    "much longer or shorter, also give a bbox that fits them, keeping the same alignment edge (a left-aligned line keeps its left side, a centred "
    "title stays centred).\n"
    "- Line breaks: use \\n in text. Japanese/Chinese need less width per character than English; a long line of them may need two lines.\n"
    "- Restyle text (colour, weight, font, outline, glow, shadow): change only \"caption\", in English, describing the full look: e.g. \"Huge bold "
    "red condensed sans-serif letters with a thin white outline\". Bigger or smaller text also needs a bigger or smaller bbox.\n"
    "- Move something: change only its bbox (same size unless asked). Its old place is painted over with the background by itself.\n"
    "- Remove something: put its id in \"remove\". Its place is painted over with the background.\n"
    "- Add something (a badge, a price, a sticker, a second line, a logo word, a person, a prop): put it in \"add\" with a bbox in an empty "
    "area that suits it, sized for its words, not covering faces or other text. A badge or sticker: describe its shape and colours in the caption "
    "(e.g. \"A round red sticker with bold white text, slightly tilted\").\n"
    "- Change how a thing or a character looks: change its caption (describe the whole new look, not only the difference).\n"
    "- Translate: change the text of every text box into the language asked, keep the boxes.\n"
    "- Captions are ALWAYS English (non-English captions get printed as stray labels). Text is ALWAYS exactly the words to print.\n"
    "- Boxes are in pixels from the picture's top-left corner; keep them inside the canvas size you are given.\n"
    "- \"summary\" redraws the WHOLE picture: use it only when the user asks to change the overall style, colours, lighting or setting, and say so in notes.\n"
    "- Never invent changes the user did not ask for.\n\n"
    "When there are no boxes yet (a picture made elsewhere): the picture is attached with a yellow grid labelled in its pixels. Find the words or "
    "things the user wants changed on the grid and \"add\" a box for each that COVERS the old one completely, with a little margin (the old content "
    "is painted over). For text, the caption describes how the OLD lettering looks (colour, font, weight, size, angle, the surface it is on) so the "
    "new words match the picture; the text is the new words. Replace a whole line or block of text (a whole headline), never a single word inside "
    "a line: the box must cover every old letter. Use \"add\" for these (there are no ids to change). Also give \"summary\": one English paragraph "
    "describing the picture as it will look AFTER the change (with the new words, never the old ones); never mention the grid.";

// The picture at the canvas size (the grid's numbers must be the boxes' pixels)
std::wstring CanvasPicture(const std::wstring& picture) {
    std::vector<unsigned char> rgba;
    int w = 0, h = 0;
    std::string err;
    if (picture.empty() || !DecodeImageRGBA(picture, rgba, w, h, &err) || w < 2 || h < 2) return {};
    std::vector<unsigned char> out(static_cast<size_t>(g_s.w) * g_s.h * 4);
    for (int y = 0; y < g_s.h; ++y) {
        const float fy = std::clamp((y + 0.5f) * h / g_s.h - 0.5f, 0.0f, h - 1.001f);
        const int y0 = static_cast<int>(fy);
        const float ty = fy - y0;
        for (int x = 0; x < g_s.w; ++x) {
            const float fx = std::clamp((x + 0.5f) * w / g_s.w - 0.5f, 0.0f, w - 1.001f);
            const int x0 = static_cast<int>(fx);
            const float tx = fx - x0;
            for (int c = 0; c < 4; ++c) {
                auto at = [&](int xx, int yy) { return static_cast<float>(rgba[(static_cast<size_t>(yy) * w + xx) * 4 + c]); };
                const float v = (at(x0, y0) * (1 - tx) + at(x0 + 1, y0) * tx) * (1 - ty) + (at(x0, y0 + 1) * (1 - tx) + at(x0 + 1, y0 + 1) * tx) * ty;
                out[(static_cast<size_t>(y) * g_s.w + x) * 4 + c] = static_cast<unsigned char>(std::clamp(v + 0.5f, 0.0f, 255.0f));
            }
        }
    }
    std::error_code ec;
    fs::create_directories(DataDir(), ec);
    const std::wstring path = DataDir() + L"\\canvas_view.png";
    return WriteThumbnailPng(path, out.data(), g_s.w, g_s.h) ? path : std::wstring();
}

// The text Windows' own OCR (Windows.Media.Ocr) reads in a picture, a line each with its exact box in the picture's pixels: "" when it reads
// nothing (or has no recognizer for the language). Tested 2026-10-18 on Krea's magazine cover: the LLM, reading a grid, put the box of its
// misspelt "GUPUS" 80 px off; the OCR had it to the pixel ([46,521,371,623]).
std::string OcrLines(const std::wstring& picture) {
    static const char* const kScript = R"PS(param([string]$path)
Add-Type -AssemblyName System.Runtime.WindowsRuntime
$null = [Windows.Media.Ocr.OcrEngine, Windows.Foundation, ContentType = WindowsRuntime]
$null = [Windows.Graphics.Imaging.BitmapDecoder, Windows.Foundation, ContentType = WindowsRuntime]
$null = [Windows.Storage.StorageFile, Windows.Storage, ContentType = WindowsRuntime]
$asTask = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object { $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
function Await($op, [Type]$t) { $task = $asTask.MakeGenericMethod($t).Invoke($null, @($op)); $task.Wait(-1) | Out-Null; $task.Result }
$file = Await ([Windows.Storage.StorageFile]::GetFileFromPathAsync($path)) ([Windows.Storage.StorageFile])
$stream = Await ($file.OpenAsync([Windows.Storage.FileAccessMode]::Read)) ([Windows.Storage.Streams.IRandomAccessStream])
$decoder = Await ([Windows.Graphics.Imaging.BitmapDecoder]::CreateAsync($stream)) ([Windows.Graphics.Imaging.BitmapDecoder])
$bitmap = Await ($decoder.GetSoftwareBitmapAsync()) ([Windows.Graphics.Imaging.SoftwareBitmap])
$engine = [Windows.Media.Ocr.OcrEngine]::TryCreateFromUserProfileLanguages()
if (-not $engine) { exit 0 }
$result = Await ($engine.RecognizeAsync($bitmap)) ([Windows.Media.Ocr.OcrResult])
$out = foreach ($line in $result.Lines) {
  $r = $line.Words | ForEach-Object { $_.BoundingRect }
  [pscustomobject]@{ text = $line.Text; box = @([int]($r | ForEach-Object { $_.X } | Measure-Object -Minimum).Minimum, [int]($r | ForEach-Object { $_.Y } | Measure-Object -Minimum).Minimum,
    [int]($r | ForEach-Object { $_.X + $_.Width } | Measure-Object -Maximum).Maximum, [int]($r | ForEach-Object { $_.Y + $_.Height } | Measure-Object -Maximum).Maximum) }
}
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
ConvertTo-Json -InputObject @($out) -Compress
)PS";
    if (picture.empty()) return {};
    std::error_code ec;
    fs::create_directories(DataDir(), ec);
    const std::wstring script = DataDir() + L"\\ocr.ps1";
    {
        std::ofstream f(script, std::ios::binary | std::ios::trunc);
        if (!f) return {};
        f << "\xEF\xBB\xBF" << kScript; // (a BOM: PowerShell 5 reads it as UTF-8)
    }
    std::string out;
    RunCaptureHidden(L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + script + L"\" -path \"" + picture + L"\"", out, false);
    const size_t a = out.find('[');
    const json j = a == std::string::npos ? json() : json::parse(out.substr(a), nullptr, false);
    if (!j.is_array() || j.empty()) return {};
    std::string lines;
    int n = 0;
    for (const json& l : j) {
        if (!l.is_object() || !l.contains("box") || !l["box"].is_array() || l["box"].size() != 4) continue;
        lines += "line " + std::to_string(++n) + ": \"" + l.value("text", std::string()) + "\" [" + l["box"][0].dump() + "," + l["box"][1].dump() + "," +
                 l["box"][2].dump() + "," + l["box"][3].dump() + "]\n";
    }
    return lines;
}

// The current boxes as the writer sees them: numbered from 1, top-left pixels
std::string NumberedLayout() {
    json items = json::array();
    for (size_t i = 0; i < g_s.items.size(); ++i) {
        const Item& it = g_s.items[i];
        json j = {{"id", static_cast<int>(i) + 1}, {"label", it.label}, {"caption", it.caption},
                  {"bbox", {std::lround(it.x1), std::lround(it.y1), std::lround(it.x2), std::lround(it.y2)}}};
        if (it.label == "text") j["text"] = it.text;
        items.push_back(j);
    }
    return json({{"summary", g_s.caption}, {"items", items}}).dump(1);
}

std::string EditLayout(std::string comfyUrl, std::string change, std::string kind, int w, int h, std::string layout, bool haveBoxes, std::wstring picture) {
    // the picture with a labelled grid (PhotoCraft draws it, as for Design): the writer can see where things are
    const std::wstring grid = picture.empty() ? std::wstring() : DesignGridPicture(picture, w, h);
    const ClefSession session = WriterAcquire(comfyUrl, [](const std::string&) {});
    if (!session.ok) return "error: the prompt writer could not be started (Clef mode tab > Prompt writer)";
    std::string user = "Kind of design: " + kind + ". Canvas: " + std::to_string(w) + " x " + std::to_string(h) + " pixels.\n";
    user += haveBoxes ? "The layout now (ids, boxes in pixels from the top-left):\n" + layout + "\n" : "There are no boxes yet: the picture was made elsewhere.\n";
    if (!haveBoxes) {
        const std::string ocr = OcrLines(picture); // (the picture is at the canvas size: these are canvas pixels)
        if (!ocr.empty())
            user += "Text read in the picture by OCR, each line with its EXACT box in pixels [x_min,y_min,x_max,y_max] (OCR can misread a word; the "
                    "boxes are right). To replace words, \"add\" a text item whose bbox is the union of the OCR boxes of the lines it replaces, "
                    "grown by about 12 pixels on each side:\n" + ocr;
    }
    if (!grid.empty()) user += "The picture is attached with a yellow grid (not part of the picture): a line every 10%, labelled with its x or y in pixels.\n";
    user += "\nThe change the user asks for: " + change;
    std::vector<std::string> images;
    if (!grid.empty() || !picture.empty()) {
        const std::string url = ImageDataUrl(grid.empty() ? picture : grid, 1024);
        if (!url.empty()) images.push_back(url);
    }
    std::string reply, err;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!WriterChat(kEditSystem, user, images, 3000, 0.3, reply, &err)) {
            WriterRelease(session, false);
            return "error: the prompt writer did not answer" + (err.empty() ? std::string() : ": " + err);
        }
        const size_t a = reply.find('{'), b = reply.rfind('}');
        const json j = a == std::string::npos || b == std::string::npos ? json() : json::parse(reply.substr(a, b - a + 1), nullptr, false);
        std::string problem;
        if (!j.is_object()) problem = "it was not one JSON object";
        else if (!j.contains("change") && !j.contains("add") && !j.contains("remove") && !j.contains("summary")) problem = "it had no change, add, remove or summary";
        else if (j.contains("add") && j["add"].is_array()) {
            for (const json& it : j["add"]) {
                if (!it.is_object() || !it.contains("bbox") || !it["bbox"].is_array() || it["bbox"].size() != 4) problem = "an added item has no bbox";
            }
        }
        if (problem.empty()) break;
        user += "\n\nYour previous answer:\n" + reply + "\n\nIt could not be used: " + problem + ". Answer again with only the JSON object.";
    }
    WriterRelease(session, false);
    // the words the user quoted must be printed: if no box prints them, the writer is told on the next try (here: noted)
    return reply;
}

bool ReadBox(const json& b, float out[4]) {
    if (!b.is_array() || b.size() != 4) return false;
    float v[4];
    for (int k = 0; k < 4; ++k) {
        if (!b[k].is_number()) return false;
        v[k] = b[k].get<float>();
    }
    out[0] = std::clamp(std::min(v[0], v[2]), 0.0f, static_cast<float>(g_s.w)), out[2] = std::clamp(std::max(v[0], v[2]), 0.0f, static_cast<float>(g_s.w));
    out[1] = std::clamp(std::min(v[1], v[3]), 0.0f, static_cast<float>(g_s.h)), out[3] = std::clamp(std::max(v[1], v[3]), 0.0f, static_cast<float>(g_s.h));
    return out[2] - out[0] >= 8 && out[3] - out[1] >= 8;
}

bool ApplyEdit(const std::string& reply, std::string& why) {
    const size_t a = reply.find('{'), b = reply.rfind('}');
    const json j = a == std::string::npos || b == std::string::npos ? json() : json::parse(reply.substr(a, b - a + 1), nullptr, false);
    if (!j.is_object()) {
        why = "no JSON in the answer";
        return false;
    }
    const bool onPicture = !g_s.result.empty() || !g_s.base.empty();
    g_s.undoItems = g_s.items, g_s.undoCaption = g_s.caption, g_s.undoErased = g_s.erased, g_s.canUndo = true;
    std::vector<Item> items = g_s.items;
    std::vector<bool> gone(items.size(), false);
    int changed = 0;
    if (j.contains("change") && j["change"].is_array()) {
        for (const json& c : j["change"]) {
            const int id = c.is_object() ? c.value("id", 0) : 0;
            if (id < 1 || id > static_cast<int>(items.size())) continue;
            Item& it = items[id - 1];
            bool any = false;
            if (c.contains("text") && c["text"].is_string() && c["text"].get<std::string>() != it.text) it.text = c["text"].get<std::string>(), any = true;
            if (c.contains("caption") && c["caption"].is_string() && c["caption"].get<std::string>() != it.caption) it.caption = c["caption"].get<std::string>(), any = true;
            if (c.contains("label") && c["label"].is_string()) {
                const std::string l = c["label"].get<std::string>();
                if ((l == "text" || l == "object" || l == "character") && l != it.label) it.label = l, any = true;
            }
            float v[4];
            if (c.contains("bbox") && ReadBox(c["bbox"], v) && (std::abs(v[0] - it.x1) > 2 || std::abs(v[1] - it.y1) > 2 || std::abs(v[2] - it.x2) > 2 || std::abs(v[3] - it.y2) > 2)) {
                const bool inside = it.x1 >= v[0] && it.y1 >= v[1] && it.x2 <= v[2] && it.y2 <= v[3];
                if (onPicture && !inside) g_s.erased.push_back({it.x1, it.y1, it.x2, it.y2});
                it.x1 = v[0], it.y1 = v[1], it.x2 = v[2], it.y2 = v[3];
                any = true;
            }
            if (any) it.dirty = true, ++changed;
        }
    }
    if (j.contains("remove") && j["remove"].is_array()) {
        for (const json& r : j["remove"]) {
            const int id = r.is_number_integer() ? r.get<int>() : 0;
            if (id < 1 || id > static_cast<int>(items.size()) || gone[id - 1]) continue;
            gone[id - 1] = true;
            const Item& it = items[id - 1];
            if (onPicture) g_s.erased.push_back({it.x1, it.y1, it.x2, it.y2});
            ++changed;
        }
    }
    std::vector<Item> kept;
    for (size_t i = 0; i < items.size(); ++i) {
        if (!gone[i]) kept.push_back(items[i]);
    }
    json adds = j.contains("add") && j["add"].is_array() ? j["add"] : json::array();
    if (g_s.items.empty() && j.contains("change") && j["change"].is_array()) { // (no boxes yet: a "change" with a box can only mean a new one)
        for (const json& c : j["change"]) {
            if (c.is_object() && c.contains("bbox")) {
                json n = c;
                if (!n.contains("label")) n["label"] = n.contains("text") ? "text" : "object";
                adds.push_back(n);
            }
        }
    }
    {
        for (const json& n : adds) {
            if (!n.is_object()) continue;
            Item it;
            it.label = n.value("label", std::string("text"));
            if (it.label != "text" && it.label != "character") it.label = "object";
            it.caption = n.value("caption", std::string());
            it.text = n.value("text", std::string());
            float v[4];
            if ((it.label == "text" && it.text.empty()) || !n.contains("bbox") || !ReadBox(n["bbox"], v)) continue;
            it.x1 = v[0], it.y1 = v[1], it.x2 = v[2], it.y2 = v[3];
            it.dirty = true;
            kept.push_back(it);
            ++changed;
        }
    }
    g_s.captionChanged = false;
    if (j.contains("summary") && j["summary"].is_string() && !j["summary"].get<std::string>().empty() && j["summary"].get<std::string>() != g_s.caption) {
        g_s.caption = j["summary"].get<std::string>();
        g_s.captionChanged = !g_s.result.empty(); // (a picture made here: only a full redraw shows a new overall look)
        ++changed;
    }
    g_s.notes.clear();
    if (j.contains("notes") && j["notes"].is_array()) {
        for (const json& n : j["notes"]) {
            if (n.is_string()) g_s.notes.push_back(n.get<std::string>());
        }
    }
    if (!changed) {
        g_s.canUndo = false;
        g_s.erased = g_s.undoErased;
        why = "it changed nothing";
        return false;
    }
    g_s.items = std::move(kept);
    g_s.sel = -1;
    return true;
}

// Example changes: a click puts one in the box (then fill in your own words)
const char* const kEditExamples[] = {
    "Change the title to \"...\"",
    "Make the date red and bold",
    "Translate every text into Japanese",
    "Add a round red NEW badge in the top right corner",
    "Remove the small print at the bottom",
    "Move the tagline to the bottom, centred",
    "Make the headline bigger, on two lines",
    "Give the person a black leather jacket",
};

void DrawEditBox(AppState& app, bool busy) {
    ImGui::Spacing();
    ui::SectionHeading("CHANGE IT IN WORDS");
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled(g_s.items.empty() ? "Say what to change in this picture. The prompt writer looks at it, finds the words or things, and boxes them; "
                                            "then only the boxes are drawn again."
                                          : "Say what to change. The prompt writer changes only the boxes that need it (marked *); the rest of the picture "
                                            "stays exactly as it is. Put exact new words in quotes.");
    ImGui::PopTextWrapPos();
    ImGui::PushID("change");
    Box("##change", g_s.change, ui::Dp(58.0f));
    ImGui::PopID();
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    for (int i = 0; i < static_cast<int>(sizeof(kEditExamples) / sizeof(kEditExamples[0])); ++i) {
        if (i) { // (on the same line while it fits)
            const float w = ImGui::CalcTextSize(kEditExamples[i]).x + ImGui::GetStyle().FramePadding.x * 2;
            ImGui::SameLine();
            if (ImGui::GetCursorScreenPos().x + w > right) ImGui::NewLine();
        }
        ImGui::PushID(i);
        if (ImGui::SmallButton(kEditExamples[i])) g_s.change = kEditExamples[i];
        ImGui::PopID();
    }
    ImGui::BeginDisabled(busy || g_s.change.empty());
    if (ImGui::Button("Plan the changes")) {
        g_s.status = "The prompt writer is planning the changes...";
        g_s.error.clear();
        const std::wstring pic = CanvasPicture(!g_s.result.empty() ? g_s.result : g_s.base);
        g_s.editFut = std::async(std::launch::async, EditLayout, app.comfy.url, g_s.change, std::string(CurKind().name), g_s.w, g_s.h, NumberedLayout(),
                                 !g_s.items.empty(), pic);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Nothing is drawn yet: you see the changed boxes first (marked *), then press Redo only what changed");
    if (g_s.canUndo) {
        ImGui::SameLine();
        if (ImGui::Button("Undo the changes")) {
            g_s.items = g_s.undoItems, g_s.caption = g_s.undoCaption, g_s.erased = g_s.undoErased;
            g_s.canUndo = false, g_s.captionChanged = false;
            g_s.notes.clear();
            g_s.sel = -1;
        }
    }
    if (!g_s.notes.empty()) {
        ImGui::TextDisabled("The writer changed:");
        for (const std::string& n : g_s.notes) ImGui::BulletText("%s", n.c_str());
    }
    if (!g_s.erased.empty()) ImGui::TextDisabled("%d place(s) left empty will be painted over with the background.", static_cast<int>(g_s.erased.size()));
    if (g_s.captionChanged) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(ui::kWarn, "The overall look changed: only Make it (the whole picture again) shows that; Redo only what changed keeps the rest.");
        ImGui::PopTextWrapPos();
    }
}

void Pump(AppState& app) {
    if (g_s.askFut.valid() && g_s.askFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const json a = g_s.askFut.get();
        g_s.answered = a.value("answered", false);
        if (g_s.answered) {
            g_s.supported = a.value("supported", false), g_s.haveSft = a.value("sft", false), g_s.haveBase = a.value("base", false);
            g_s.haveTe = a.value("te", false), g_s.haveVae = a.value("vae", false), g_s.haveLora = a.value("lora", false);
        }
    }
    static bool wasDownloading = false;
    if (wasDownloading && !g_dl.running) g_s.askedAt = -1000; // look again
    wasDownloading = g_dl.running;
    if ((g_s.open || ImGui::GetTime() - g_s.wantedAt < 2.0) && !g_s.askFut.valid() && ImGui::GetTime() - g_s.askedAt > (Ready() ? 120.0 : 15.0)) {
        g_s.askedAt = ImGui::GetTime();
        g_s.askFut = std::async(std::launch::async, AskComfy, app.comfy.url);
    }
    if (g_s.planFut.valid() && g_s.planFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const std::string reply = g_s.planFut.get();
        g_s.status.clear();
        { // kept, to see what the writer wrote
            std::error_code ec;
            fs::create_directories(DataDir(), ec);
            std::ofstream f(DataDir() + L"\\last_plan.txt", std::ios::binary | std::ios::trunc);
            if (f) f << reply;
        }
        std::string why;
        if (reply.rfind("error: ", 0) == 0) g_s.error = reply.substr(7);
        else if (!ReadPlan(reply, why)) g_s.error = "The prompt writer's layout could not be read (" + why + ")";
        else g_s.error.clear();
    }
    if (g_s.editFut.valid() && g_s.editFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const std::string reply = g_s.editFut.get();
        g_s.status.clear();
        {
            std::error_code ec;
            fs::create_directories(DataDir(), ec);
            std::ofstream f(DataDir() + L"\\last_edit.txt", std::ios::binary | std::ios::trunc);
            if (f) f << reply;
        }
        std::string why;
        if (reply.rfind("error: ", 0) == 0) g_s.error = reply.substr(7);
        else if (!ApplyEdit(reply, why)) g_s.error = "The prompt writer's changes could not be read (" + why + ")";
        else g_s.error.clear();
    }
    if (g_s.job && app.comfySvc) {
        for (const ComfyActivity& a : app.comfySvc->Activity()) {
            if (a.id != g_s.job || (a.state != ComfyState::Done && a.state != ComfyState::Failed)) continue;
            g_s.job = 0;
            g_s.status.clear();
            std::wstring file;
            for (const ComfyResult& r : a.files) {
                if (!r.localPath.empty() && !r.video) file = r.localPath;
            }
            if (file.empty()) {
                g_s.error = "MOSS-Image could not draw it: " + a.message;
                break;
            }
            SetResult(app, file);
            for (Item& it : g_s.items) it.dirty = false;
            g_s.erased.clear();
            g_s.captionChanged = false;
            g_s.canUndo = false;
            SaveLayoutFile(file);
            Toast(ToastKind::Success, "Design ready", U8(fs::path(file).filename().wstring()), file, [](AppState& a) { g_s.open = true, a.requestTab = 6; }, "layout");
            break;
        }
    }
}

// ---- the canvas: the picture (made, or being changed) with the boxes over it; drag to move, corners to resize, draw new ones
void DrawCanvas(AppState& app, ImVec2 size) {
    const float s = std::min(size.x / g_s.w, size.y / g_s.h);
    const ImVec2 dim(g_s.w * s, g_s.h * s);
    const ImVec2 o0 = ImGui::GetCursorScreenPos();
    const ImVec2 o(o0.x + (size.x - dim.x) * 0.5f, o0.y);
    ImGui::InvisibleButton("##canvas", ImVec2(size.x, dim.y));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, ImVec2(o.x + dim.x, o.y + dim.y), IM_COL32(236, 236, 232, 255));
    const LoadedImage& pic = g_s.resultTex.srv ? g_s.resultTex : g_s.baseTex;
    if (pic.srv) dl->AddImage((ImTextureID)(intptr_t)pic.srv, o, ImVec2(o.x + dim.x, o.y + dim.y));
    else {
        for (int k = 1; k < 4; ++k) { // a light grid on the blank canvas
            dl->AddLine(ImVec2(o.x + dim.x * k / 4, o.y), ImVec2(o.x + dim.x * k / 4, o.y + dim.y), IM_COL32(0, 0, 0, 25));
            dl->AddLine(ImVec2(o.x, o.y + dim.y * k / 4), ImVec2(o.x + dim.x, o.y + dim.y * k / 4), IM_COL32(0, 0, 0, 25));
        }
    }
    const ImVec2 m = ImGui::GetIO().MousePos;
    const float mx = (m.x - o.x) / s, my = (m.y - o.y) / s;
    const float grab = 8.0f / s;
    auto snap = [](float v) { return std::round(v / 8.0f) * 8.0f; };
    // a click: a new box, a corner, a box, or nothing
    if (ImGui::IsItemActivated()) {
        g_s.dragStart = ImVec2(mx, my);
        g_s.drag = 0;
        if (!g_s.newLabel.empty()) {
            Item n;
            n.label = g_s.newLabel;
            n.x1 = n.x2 = snap(mx), n.y1 = n.y2 = snap(my);
            if (n.label == "text") n.text = "Text", n.caption = "Bold clean lettering.";
            g_s.items.push_back(n);
            g_s.sel = static_cast<int>(g_s.items.size()) - 1;
            g_s.drag = 3;
        } else {
            if (g_s.sel >= 0 && g_s.sel < static_cast<int>(g_s.items.size())) {
                const Item& it = g_s.items[g_s.sel];
                const float cx[4] = {it.x1, it.x2, it.x1, it.x2}, cy[4] = {it.y1, it.y1, it.y2, it.y2};
                for (int k = 0; k < 4; ++k) {
                    if (std::abs(mx - cx[k]) < grab && std::abs(my - cy[k]) < grab) g_s.drag = 2, g_s.corner = k;
                }
            }
            if (!g_s.drag) {
                int hit = -1;
                float best = 1e30f;
                for (int i = 0; i < static_cast<int>(g_s.items.size()); ++i) { // the smallest box under the mouse (a title inside a picture box)
                    const Item& it = g_s.items[i];
                    const float area = (it.x2 - it.x1) * (it.y2 - it.y1);
                    if (mx >= it.x1 && mx <= it.x2 && my >= it.y1 && my <= it.y2 && area < best) best = area, hit = i;
                }
                g_s.sel = hit;
                if (hit >= 0) g_s.drag = 1;
            }
        }
    }
    if (ImGui::IsItemActive() && g_s.drag && g_s.sel >= 0 && g_s.sel < static_cast<int>(g_s.items.size())) {
        Item& it = g_s.items[g_s.sel];
        const float W = static_cast<float>(g_s.w), H = static_cast<float>(g_s.h);
        const float cx = std::clamp(snap(mx), 0.0f, W), cy = std::clamp(snap(my), 0.0f, H);
        if (g_s.drag == 1) {
            const float dx = snap(mx - g_s.dragStart.x), dy = snap(my - g_s.dragStart.y);
            if (dx != 0 || dy != 0) {
                const float bw = it.x2 - it.x1, bh = it.y2 - it.y1;
                it.x1 = std::clamp(it.x1 + dx, 0.0f, W - bw), it.y1 = std::clamp(it.y1 + dy, 0.0f, H - bh);
                it.x2 = it.x1 + bw, it.y2 = it.y1 + bh;
                g_s.dragStart.x += dx, g_s.dragStart.y += dy;
                it.dirty = true;
            }
        } else if (g_s.drag == 2) {
            if (g_s.corner == 0 || g_s.corner == 2) it.x1 = std::min(cx, it.x2 - 16);
            else it.x2 = std::max(cx, it.x1 + 16);
            if (g_s.corner < 2) it.y1 = std::min(cy, it.y2 - 16);
            else it.y2 = std::max(cy, it.y1 + 16);
            it.dirty = true;
        } else if (g_s.drag == 3) {
            it.x1 = std::min(snap(g_s.dragStart.x), cx), it.x2 = std::max(snap(g_s.dragStart.x), cx);
            it.y1 = std::min(snap(g_s.dragStart.y), cy), it.y2 = std::max(snap(g_s.dragStart.y), cy);
            it.dirty = true;
        }
    }
    if (ImGui::IsItemActivated() && g_s.sel >= 0 && g_s.sel < static_cast<int>(g_s.items.size())) {
        const Item& it = g_s.items[g_s.sel];
        g_s.dragFrom[0] = it.x1, g_s.dragFrom[1] = it.y1, g_s.dragFrom[2] = it.x2, g_s.dragFrom[3] = it.y2;
    }
    if (ImGui::IsItemDeactivated() && (g_s.drag == 1 || g_s.drag == 2) && g_s.sel >= 0 && g_s.sel < static_cast<int>(g_s.items.size()) &&
        (!g_s.result.empty() || !g_s.base.empty())) {
        const Item& it = g_s.items[g_s.sel];
        const float* f = g_s.dragFrom;
        const bool inside = f[0] >= it.x1 && f[1] >= it.y1 && f[2] <= it.x2 && f[3] <= it.y2;
        if (!inside && (f[2] - f[0]) > 0) g_s.erased.push_back({f[0], f[1], f[2], f[3]}); // (what was there is painted over)
    }
    if (ImGui::IsItemDeactivated() && g_s.drag) {
        if (g_s.drag == 3) { // a click without a drag: a box of a sensible size there
            Item& it = g_s.items[g_s.sel];
            if (it.x2 - it.x1 < 24 || it.y2 - it.y1 < 24) {
                const float bw = it.label == "text" ? 400.0f : 300.0f, bh = it.label == "text" ? 96.0f : 300.0f;
                it.x1 = std::clamp(it.x1 - bw / 2, 0.0f, std::max(0.0f, g_s.w - bw)), it.y1 = std::clamp(it.y1 - bh / 2, 0.0f, std::max(0.0f, g_s.h - bh));
                it.x2 = std::min(static_cast<float>(g_s.w), it.x1 + bw), it.y2 = std::min(static_cast<float>(g_s.h), it.y1 + bh);
            }
            g_s.newLabel.clear();
        }
        g_s.drag = 0;
    }
    if (!g_s.showBoxes && g_s.drag == 0) return;
    // the boxes: numbered, coloured by kind, the picked one with its corner handles
    dl->PushClipRect(o, ImVec2(o.x + dim.x, o.y + dim.y), true);
    for (int i = 0; i < static_cast<int>(g_s.items.size()); ++i) {
        const Item& it = g_s.items[i];
        const ImVec2 a(o.x + it.x1 * s, o.y + it.y1 * s), b(o.x + it.x2 * s, o.y + it.y2 * s);
        const bool picked = i == g_s.sel;
        dl->AddRectFilled(a, b, LabelColor(it.label, picked ? 60 : 30));
        dl->AddRect(a, b, picked ? IM_COL32(255, 90, 30, 255) : LabelColor(it.label, 220), 0, 0, picked ? 2.5f : 1.5f);
        std::string tag = std::to_string(i + 1) + " " + LabelName(it.label) + (it.dirty && !g_s.result.empty() ? " *" : "") + ": " +
                          (it.label == "text" ? it.text : it.caption);
        for (char& c : tag) {
            if (c == '\n') c = ' ';
        }
        if (tag.size() > 60) tag = tag.substr(0, 57) + "...";
        const ImVec2 ts = ImGui::CalcTextSize(tag.c_str());
        const float tw = std::min(ts.x + 8, std::max(40.0f, b.x - a.x));
        dl->AddRectFilled(a, ImVec2(a.x + tw, a.y + ts.y + 4), picked ? IM_COL32(255, 90, 30, 230) : LabelColor(it.label, 220));
        dl->PushClipRect(a, ImVec2(a.x + tw, a.y + ts.y + 4), true);
        dl->AddText(ImVec2(a.x + 4, a.y + 2), IM_COL32(255, 255, 255, 255), tag.c_str());
        dl->PopClipRect();
        if (picked) {
            for (const ImVec2& c : {a, ImVec2(b.x, a.y), ImVec2(a.x, b.y), b})
                dl->AddRectFilled(ImVec2(c.x - 4, c.y - 4), ImVec2(c.x + 4, c.y + 4), IM_COL32(255, 255, 255, 255)),
                    dl->AddRect(ImVec2(c.x - 4, c.y - 4), ImVec2(c.x + 4, c.y + 4), IM_COL32(255, 90, 30, 255));
        }
    }
    dl->PopClipRect();
    if (ImGui::IsItemHovered() && !g_s.newLabel.empty()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
}

void DrawSetup(AppState& app) {
    if (!g_s.answered) {
        ImGui::TextDisabled("Asking ComfyUI what it has... (it has to be running: the Generate page starts it)");
        return;
    }
    if (!g_s.supported) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(ui::kWarn, "This ComfyUI cannot run MOSS-Image yet: it needs a ComfyUI from mid-October 2026 or newer (update Comfy Desktop).");
        ImGui::PopTextWrapPos();
        return;
    }
    std::vector<ModelFile> need;
    if (!g_s.haveSft) need.push_back(kSft);
    if (!g_s.haveTe) need.push_back(kTe);
    if (!g_s.haveVae) need.push_back(kVae);
    if (!g_s.haveLora) need.push_back(kLora);
    if (need.empty()) return;
    double gb = 0;
    for (const ModelFile& m : need) gb += m.gb;
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("MOSS-Image is not in ComfyUI yet: the model (int8), its Qwen 3.5 text encoder, the FLUX.2 VAE and the 8-step LoRA (Apache-2.0).");
    ImGui::PopTextWrapPos();
    if (g_dl.running) {
        ImGui::TextDisabled("Downloading %s...", g_dl.what.c_str());
    } else {
        char label[96];
        snprintf(label, sizeof(label), "Download MOSS-Image (%.1f GB)", gb);
        if (ImGui::Button(label)) StartDownloads(ComfyModelsDir(), need);
        if (g_dl.failed) {
            ImGui::SameLine();
            ImGui::TextColored(ui::kWarn, "the last try failed");
        }
    }
}

} // namespace

void OpenLayoutStudio(AppState& app, const std::wstring& picture) {
    g_s.open = true;
    g_s.askedAt = -1000;
    SetEditsMode(1);
    app.requestTab = 6;
    if (!picture.empty()) {
        SetBase(app, picture);
        ReleaseImage(g_s.resultTex);
        g_s.resultTex = LoadedImage{};
        g_s.result.clear();
        g_s.items.clear();
        g_s.sel = -1;
        g_s.caption.clear();
        g_s.meta = "photograph";
        g_s.idea.clear();
        g_s.erased.clear(), g_s.notes.clear(), g_s.canUndo = false, g_s.captionChanged = false;
    } else if (g_s.items.empty() && g_s.base.empty()) {
        const Kind& k = CurKind();
        g_s.w = k.w, g_s.h = k.h, g_s.meta = k.meta;
    }
}

bool LayoutStudioOpen() { return g_s.open; }

bool MossReady(AppState&) { // (false only when ComfyUI said it lacks something: not known yet is not missing)
    g_s.wantedAt = ImGui::GetTime();
    return !g_s.answered || Ready();
}

// Generate's MOSS: a plain prompt (no boxes), 8 steps
int SendMossPrompt(AppState& app, const std::string& prompt, int w, int h, int times, const std::string& savePrefix, const std::vector<LoraEntry>& loras, long long seed) {
    const int keepW = g_s.w, keepH = g_s.h, keepQ = g_s.quality;
    g_s.w = w, g_s.h = h, g_s.quality = 0;
    json g = BuildGraph(prompt, false);
    g["10"]["inputs"]["filename_prefix"] = savePrefix.empty() ? std::string("Generate/moss") : savePrefix;
    g_s.w = keepW, g_s.h = keepH, g_s.quality = keepQ;
    ComfyJob job;
    job.apiWorkflow = g.dump();
    job.label = "MOSS: " + prompt.substr(0, 48);
    job.prompt = prompt;
    job.times = std::max(1, times);
    job.loras = loras;
    job.seed = seed;
    return SendComfyJob(app, std::move(job));
}

// Generate's "Design with exact text": Layout designs with this idea, planned by the writer
void OpenLayoutWithIdea(AppState& app, const std::string& idea) {
    if (!g_s.base.empty()) SetBase(app, L""), g_s.items.clear();
    OpenLayoutStudio(app);
    if (!idea.empty()) {
        g_s.idea = idea;
        if (!g_s.planFut.valid()) {
            g_s.status = "The prompt writer is planning the boxes...";
            g_s.planFut = std::async(std::launch::async, PlanLayout, app.comfy.url, g_s.idea, std::string(CurKind().name), g_s.w, g_s.h, std::string(), g_s.base);
        }
    }
}
void ShowLayoutStudio(bool on) {
    g_s.open = on;
    if (on) g_s.askedAt = -1000;
}

void PumpLayoutStudio(AppState& app) {
    { // tests: MV_LAYOUT_IDEA opens the studio and plans it (MV_LAYOUT_KIND: the kind's number), then Make once the boxes are there
        static int step = 0;
        auto env = [](const wchar_t* name) { // (wide: an ANSI read turns Japanese into "?")
            wchar_t buf[2048] = {};
            return GetEnvironmentVariableW(name, buf, 2048) ? U8(buf) : std::string();
        };
        static std::string idea = env(L"MV_LAYOUT_IDEA");
        static std::wstring picture = W16(env(L"MV_LAYOUT_PICTURE"));
        if (!picture.empty()) { // MV_LAYOUT_PICTURE + MV_LAYOUT_EDIT: a picture made elsewhere, changed in words (the writer finds and boxes it)
            if (step == 0) {
                OpenLayoutStudio(app, picture);
                g_s.change = env(L"MV_LAYOUT_EDIT");
                g_s.editFut = std::async(std::launch::async, EditLayout, app.comfy.url, g_s.change, std::string(CurKind().name), g_s.w, g_s.h, std::string(), false,
                                         CanvasPicture(g_s.base));
                step = 10;
            } else if (step == 10 && !g_s.editFut.valid() && Ready()) {
                step = 11;
                if (!g_s.items.empty()) Make(app, false);
            }
        }
        if (!idea.empty()) {
            if (step == 0) {
                char k[8] = {};
                if (GetEnvironmentVariableA("MV_LAYOUT_KIND", k, sizeof(k))) g_s.kind = std::clamp(atoi(k), 0, kKindCount - 1);
                g_s.w = CurKind().w, g_s.h = CurKind().h, g_s.meta = CurKind().meta;
                OpenLayoutStudio(app);
                g_s.idea = idea;
                g_s.status = "The prompt writer is planning the boxes...";
                g_s.planFut = std::async(std::launch::async, PlanLayout, app.comfy.url, g_s.idea, std::string(CurKind().name), g_s.w, g_s.h, std::string(), g_s.base);
                step = 1;
            } else if (step == 1 && !g_s.planFut.valid() && Ready()) {
                step = 2;
                if (!g_s.items.empty()) Make(app, false);
            } else if (step == 2 && g_s.job == 0 && !g_s.result.empty()) { // MV_LAYOUT_CHANGE "N|new words": then box N reworded, only it redone
                step = 3;
                const std::string c = env(L"MV_LAYOUT_CHANGE"), e = env(L"MV_LAYOUT_EDIT");
                if (!e.empty()) { // MV_LAYOUT_EDIT "a change in words": the writer plans it; then only what changed is redone
                    g_s.change = e;
                    g_s.editFut = std::async(std::launch::async, EditLayout, app.comfy.url, g_s.change, std::string(CurKind().name), g_s.w, g_s.h, NumberedLayout(),
                                             true, CanvasPicture(g_s.result));
                    step = 4;
                } else if (!c.empty()) {
                    const size_t bar = c.find('|');
                    const int k = atoi(c.c_str()) - 1;
                    if (bar != std::string::npos && k >= 0 && k < static_cast<int>(g_s.items.size())) {
                        g_s.items[k].text = c.substr(bar + 1);
                        g_s.items[k].dirty = true;
                        Make(app, true);
                    }
                }
            } else if (step == 4 && !g_s.editFut.valid() && g_s.job == 0) {
                step = 5;
                if (g_s.canUndo) Make(app, true);
            }
        }
    }
    Pump(app);
}

// The Edits page's "Layout designs": the canvas on the left, what it is, the boxes and making it on the right
void DrawLayoutStudioBody(AppState& app, float height) {
    const bool busy = g_s.planFut.valid() || g_s.editFut.valid() || g_s.job != 0;
    const float right = ui::Dp(450.0f);
    const ImVec2 avail(ImGui::GetContentRegionAvail().x, height);
    // ---- left: the canvas
    ImGui::BeginChild("##lscanvas", ImVec2(avail.x - right - ImGui::GetStyle().ItemSpacing.x, avail.y), false);
    {
        ImGui::TextDisabled("Draw a box:");
        for (const char* l : {"text", "object", "character"}) {
            ImGui::SameLine();
            const bool on = g_s.newLabel == l;
            ImGui::PushStyleColor(ImGuiCol_Button, on ? LabelColor(l, 255) : LabelColor(l, 110));
            if (ImGui::SmallButton((std::string("+ ") + LabelName(l)).c_str())) g_s.newLabel = on ? std::string() : std::string(l);
            ImGui::PopStyleColor();
        }
        ImGui::SameLine();
        ImGui::Checkbox("Show the boxes", &g_s.showBoxes);
        ImGui::SameLine();
        ImGui::TextDisabled("%d x %d", g_s.w, g_s.h);
        if (!g_s.newLabel.empty()) ImGui::TextColored(ui::kAccent, "Drag on the picture to draw the %s box (a click puts one there)", LabelName(g_s.newLabel));
        else ImGui::TextDisabled("Click a box to pick it, drag to move it, drag its corners to resize.");
        const ImVec2 c = ImGui::GetContentRegionAvail();
        DrawCanvas(app, ImVec2(c.x, c.y - ui::Dp(4)));
    }
    ImGui::EndChild();
    ImGui::SameLine();
    // ---- right: what it is, the boxes, make it
    ImGui::BeginChild("##lsside", ImVec2(right, avail.y), false);
    DrawSetup(app);
    if (g_s.base.empty() && g_s.result.empty()) { // a quick box edit on any picture
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Edit a picture with boxes:");
        ImGui::SameLine();
        if (ImGui::SmallButton("The gallery's picture")) {
            if (app.selectedIndex >= 0 && app.selectedIndex < static_cast<int>(app.entries.size())) OpenLayoutStudio(app, app.entries[app.selectedIndex].path);
            else g_s.error = "Pick a picture in the gallery first";
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("A file...")) {
            const std::wstring f = OpenImageFileDialog(GetActiveWindow());
            if (!f.empty()) OpenLayoutStudio(app, f);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("Recolour, remove, replace or reword what is inside a box (~4 s); everything outside stays exactly as it is. Things a person "
                              "does with their body (holding, wearing) work better with Picture edits (Qwen).");
        ImGui::Separator();
    }
    if (!g_s.base.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(ui::kAccent, "Changing a picture: %s", U8(fs::path(g_s.base).filename().wstring()).c_str());
        ImGui::TextDisabled("Say what to change below (the writer finds it and boxes it), or draw boxes over the words or things yourself; only the boxes "
                            "are drawn again, the rest stays exactly as it is.");
        ImGui::PopTextWrapPos();
        if (ImGui::SmallButton("Start a new design instead")) {
            SetBase(app, L"");
            g_s.kind = 0;
            g_s.w = kKinds[0].w, g_s.h = kKinds[0].h, g_s.meta = kKinds[0].meta;
            g_s.items.clear(), g_s.sel = -1;
            g_s.erased.clear(), g_s.notes.clear(), g_s.canUndo = false;
        }
    } else {
        ImGui::SetNextItemWidth(ui::Dp(200.0f));
        if (ImGui::Combo("##kind", &g_s.kind, [](void*, int i) { return kKinds[i].name; }, nullptr, kKindCount)) {
            const Kind& k = CurKind();
            if (g_s.kind != kKindCount - 1) {
                const float sx = static_cast<float>(k.w) / g_s.w, sy = static_cast<float>(k.h) / g_s.h;
                for (Item& it : g_s.items) it.x1 *= sx, it.x2 *= sx, it.y1 *= sy, it.y2 *= sy, it.dirty = true;
                g_s.w = k.w, g_s.h = k.h;
            }
            g_s.meta = k.meta;
            ReleaseImage(g_s.resultTex), g_s.resultTex = LoadedImage{}, g_s.result.clear();
        }
        if (g_s.kind == kKindCount - 1) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ui::Dp(80.0f));
            if (ImGui::InputInt("##w", &g_s.w, 16, 64)) g_s.w = std::clamp(g_s.w / 16 * 16, 256, 2048);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ui::Dp(80.0f));
            if (ImGui::InputInt("##h", &g_s.h, 16, 64)) g_s.h = std::clamp(g_s.h / 16 * 16, 256, 2048);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Open a layout...")) {
            const std::wstring f = OpenImageFileDialog(GetActiveWindow());
            if (!f.empty()) {
                const fs::path lay = fs::path(f).replace_extension(L".layout.json");
                std::error_code ec;
                if (fs::exists(lay, ec) && LoadLayoutFile(lay.wstring())) {
                    SetResult(app, f);
                    if (!g_s.base.empty()) SetBase(app, g_s.base);
                } else {
                    g_s.error = "That picture has no layout beside it (only designs made here do)";
                }
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("A design made here earlier: its boxes come back to change them");
    }
    if (!g_s.result.empty() || !g_s.base.empty()) DrawEditBox(app, busy);
    if (g_s.base.empty()) {
    ImGui::Spacing();
    ui::SectionHeading(g_s.result.empty() ? "A NEW DESIGN" : "OR PLAN IT AGAIN FROM AN IDEA");
    ImGui::TextUnformatted("The idea");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Say what it is and the exact words to print (any language: Japanese text is drawn in Japanese)");
    ImGui::PushID("idea");
    Box("##idea", g_s.idea, ui::Dp(70.0f));
    ImGui::PopID();
    if (g_s.idea.empty() && g_s.base.empty()) ImGui::TextDisabled("e.g. %s", CurKind().hint);
    ImGui::BeginDisabled(busy || g_s.idea.empty());
    if (ImGui::Button(g_s.items.empty() ? "Plan the layout" : "Plan it again")) {
        g_s.status = "The prompt writer is planning the boxes...";
        g_s.error.clear();
        g_s.planFut = std::async(std::launch::async, PlanLayout, app.comfy.url, g_s.idea, std::string(CurKind().name), g_s.w, g_s.h, std::string(), g_s.base);
    }
    ImGui::EndDisabled();
    if (!g_s.items.empty()) {
        ImGui::SameLine();
        ImGui::BeginDisabled(busy || g_s.idea.empty());
        if (ImGui::Button("Change it as the idea says")) {
            g_s.status = "The prompt writer is changing the layout...";
            g_s.error.clear();
            g_s.planFut = std::async(std::launch::async, PlanLayout, app.comfy.url, g_s.idea, std::string(CurKind().name), g_s.w, g_s.h, DraftJson(), g_s.base);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("The writer re-plans the boxes from the idea, keeping what it does not change (for small changes, use Change it in words)");
    }
    }
    ImGui::Spacing();
    ImGui::TextUnformatted("The whole picture");
    ImGui::PushID("cap");
    Box("##caption", g_s.caption, ui::Dp(64.0f));
    ImGui::PopID();
    // the boxes
    ImGui::Spacing();
    ImGui::Text("Boxes (%d)", static_cast<int>(g_s.items.size()));
    ImGui::BeginChild("##items", ImVec2(0, ui::Dp(150.0f)), true);
    for (int i = 0; i < static_cast<int>(g_s.items.size()); ++i) {
        const Item& it = g_s.items[i];
        std::string row = std::to_string(i + 1) + "  " + LabelName(it.label) + (it.dirty && !g_s.result.empty() ? " *" : "") + "  " +
                          (it.label == "text" ? it.text : it.caption);
        for (char& c : row) {
            if (c == '\n') c = ' ';
        }
        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_Text, LabelColor(it.label, 255));
        ImGui::Bullet();
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Selectable(row.c_str(), g_s.sel == i)) g_s.sel = i;
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (g_s.sel >= 0 && g_s.sel < static_cast<int>(g_s.items.size())) {
        Item& it = g_s.items[g_s.sel];
        ImGui::PushID(g_s.sel);
        int lab = it.label == "text" ? 0 : it.label == "object" ? 1 : 2;
        ImGui::SetNextItemWidth(ui::Dp(120.0f));
        if (ImGui::Combo("##label", &lab, "Text\0Object\0Character\0")) it.label = lab == 0 ? "text" : lab == 1 ? "object" : "character", it.dirty = true;
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy")) {
            Item c = it;
            c.x1 += 16, c.x2 += 16, c.y1 += 16, c.y2 += 16;
            c.dirty = true;
            g_s.items.push_back(c);
            g_s.sel = static_cast<int>(g_s.items.size()) - 1;
            ImGui::PopID();
            ImGui::EndChild();
            return;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Delete")) {
            if (!g_s.result.empty() || !g_s.base.empty()) g_s.erased.push_back({it.x1, it.y1, it.x2, it.y2}); // (painted over on the next redo)
            g_s.items.erase(g_s.items.begin() + g_s.sel);
            g_s.sel = -1;
            ImGui::PopID();
            ImGui::EndChild();
            return;
        }
        if (it.label == "text") {
            ImGui::TextDisabled("The words, exactly (Enter: a new line)");
            if (Box("##text", it.text, ui::Dp(48.0f))) it.dirty = true;
        }
        ImGui::TextDisabled(it.label == "text" ? "How they look (in English: colour, weight, font, outline...)" : "What it looks like (in English)");
        if (Box("##cap", it.caption, ui::Dp(56.0f))) it.dirty = true;
        int v[4] = {static_cast<int>(it.x1), static_cast<int>(it.y1), static_cast<int>(it.x2), static_cast<int>(it.y2)};
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputInt4("##box", v)) {
            it.x1 = static_cast<float>(std::clamp(v[0], 0, g_s.w - 8)), it.y1 = static_cast<float>(std::clamp(v[1], 0, g_s.h - 8));
            it.x2 = static_cast<float>(std::clamp(v[2], v[0] + 8, g_s.w)), it.y2 = static_cast<float>(std::clamp(v[3], v[1] + 8, g_s.h));
            it.dirty = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("left, top, right, bottom in pixels");
        ImGui::PopID();
    }
    // make it
    ImGui::Separator();
    ImGui::RadioButton("Fast (8 steps)", &g_s.quality, 0);
    ImGui::SameLine();
    ImGui::BeginDisabled(!g_s.haveSft);
    ImGui::RadioButton("Best (25 steps)", &g_s.quality, 1);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("MOSS-Image SFT at its official settings (25 steps with guidance): about 16 s; every word and every person as asked more often, "
                          "sharper faces. Fast is the same model with an 8-step speed LoRA.");
    ImGui::SameLine();
    ImGui::Checkbox("Keep the picture", &g_s.keepSeed);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("The same seed every time: change a word or a box and the rest of the picture stays the same. Off: a new picture each time.");
    ImGui::BeginDisabled(busy || !Ready() || g_s.items.empty());
    if (ImGui::Button(g_s.base.empty() ? "Make it" : "Change the boxed parts", ImVec2(ui::Dp(170.0f), 0))) Make(app, false);
    ImGui::EndDisabled();
    int dirty = static_cast<int>(g_s.erased.size());
    for (const Item& it : g_s.items) dirty += it.dirty ? 1 : 0;
    if (!g_s.result.empty()) {
        ImGui::SameLine();
        ImGui::BeginDisabled(busy || !Ready() || dirty == 0);
        char lbl[64];
        snprintf(lbl, sizeof(lbl), "Redo only what changed (%d)", dirty);
        if (ImGui::Button(lbl)) Make(app, true);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Only the boxes marked * are drawn again, on the last picture; everything else stays exactly as it is");
    }
    if (!g_s.status.empty()) ImGui::TextColored(ui::kAccent, "%s", g_s.status.c_str());
    if (!g_s.error.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(ui::kWarn, "%s", g_s.error.c_str());
        ImGui::PopTextWrapPos();
    }
    if (!g_s.result.empty()) {
        ImGui::TextDisabled("%s", U8(fs::path(g_s.result).filename().wstring()).c_str());
        if (ImGui::SmallButton("Open")) ShellExecuteW(nullptr, L"open", g_s.result.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::SameLine();
        if (ImGui::SmallButton("Show in folder")) {
            const std::wstring args = L"/select,\"" + g_s.result + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Change words in it")) {
            const std::wstring r = g_s.result;
            OpenLayoutStudio(app, r);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Use this picture as the canvas: box what to change, only that is drawn again");
    }
    ImGui::EndChild();
}
