// The Pixel art tab. A strong model (Krea through the ComfyUI tab's workflow, or Qwen Image 2.1) draws what is described - they follow a description far better than
// Stable Diffusion 1.5 - and pixel_art.cpp turns the drawing into real pixel art: one picture pixel per art pixel, a small palette (cropped to the subject first, so an
// item fills its pixels). Retro Diffusion's models (SD 1.5 checkpoints that draw every art pixel as an 8 x 8 cell) can draw alone, touch a drawing up, and make tiles
// repeat. Three ways in:
//  - Create: a prompt (and what it is: a character, an item, a tile...) becomes a sprite, an icon, a repeating texture or a scene.
//  - Convert: a picture is shrunk to the art size and redrawn by Retro Diffusion as pixel art (or only pixelized, without AI).
//  - A set: "20 items for a dark fantasy dungeon crawler" - the prompt writer (an LLM) plans the assets, they are made one by one in one style, with one palette.
// Tiles and walls are made to repeat: made at twice the size (the middle has no frame), shifted by half so the wrap-around edges meet in a cross, and that cross
// is redrawn (a masked pass), so the tile has no visible seam.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <random>
#include <cstring>
#include <set>
#include <windows.h>
#include <shellapi.h>

#include "board.h"
#include "clef.h"
#include "clipboard_image.h"
#include "image_texture.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "json.hpp"
#include "metadata.h"
#include "pixel_art.h"
#include "pixel_fixer.h"
#include "notify.h"
#include "ui_extra.h"
#include "ui_theme.h"
#include "vision_prompt.h"
#include "win_dialogs.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(len > 0 ? len - 1 : 0, '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(len > 0 ? len - 1 : 0, L'\0');
    if (len > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len);
    return out;
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

int StringResize(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* str = static_cast<std::string*>(data->UserData);
        str->resize(data->BufTextLen);
        data->Buf = str->data();
    }
    return 0;
}

bool InputLine(const char* id, const char* hint, std::string& value, float width) {
    ImGui::SetNextItemWidth(width);
    return ImGui::InputTextWithHint(id, hint, value.data(), value.capacity() + 1, ImGuiInputTextFlags_CallbackResize, StringResize, &value);
}

// A file name made from a label: letters, digits, spaces, - and _ only.
std::string SafeName(const std::string& s, size_t maxLen = 40) {
    std::string out;
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == ' ' || c == '-' || c == '_') out += c;
        else if (u >= 0x80) out += c; // keep UTF-8 letters
        if (out.size() >= maxLen) break;
    }
    out = Trim(out);
    return out.empty() ? std::string("pixel") : out;
}

// ------------------------------------------------------------------- what a thing is: its size, background and the words that make the model draw it right
struct Kind {
    const char* id;
    const char* label;
    int w, h;
    bool transparent, tile;
    const char* suffix;    // Retro Diffusion's words
    const char* drawLead;  // a strong model's words: "<lead> <description>, <style>, <tail>"
    const char* drawTail;
};
const Kind kKinds[] = {
    {"anything", "Anything", 64, 64, false, false, "", "16-bit pixel art of", "bold simple shapes, flat cel shading with few tones, crisp dark outlines, limited colour palette, no fine texture"},
    {"character", "Character", 64, 64, true, false, "full body, standing, front view, plain white background", "pixel art sprite of",
     "full body, standing, front view, bold simple shapes, flat shading, crisp dark outlines, no fine texture"},
    {"item", "Item / icon", 32, 32, true, false, "game item icon, centered, plain white background", "pixel art game item icon of",
     "centered, bold simple shapes, flat shading, crisp dark outlines, no fine texture"},
    {"tile", "Tile / texture", 32, 32, false, true, "texture, repeating pattern, full frame", "seamless pixel art texture of",
     "repeating pattern that fills the whole frame, top-down view, flat even lighting"},
    {"wall", "Wall", 32, 32, false, true, "wall texture, repeating pattern, full frame, front view", "seamless pixel art wall texture of",
     "repeating pattern that fills the whole frame, front view, flat even lighting"},
    {"platform", "Platform", 64, 24, true, false, "floating platform, side view, plain white background", "pixel art platform of",
     "side view, clean shapes, crisp outlines"},
    {"prop", "Prop", 48, 48, true, false, "game prop, centered, plain white background", "pixel art game prop of",
     "centered, bold simple shapes, flat shading, crisp dark outlines, no fine texture"},
    {"background", "Background", 128, 72, false, false, "detailed background scene, no characters", "16-bit pixel art scene of",
     "no characters, bold simple shapes, flat cel shading with few tones, limited colour palette, no fine texture"},
    {"portrait", "Portrait", 64, 64, false, false, "portrait, head and shoulders", "16-bit pixel art portrait of",
     "head and shoulders, bold simple shapes, flat cel shading with few tones, crisp dark outlines, limited colour palette, no fine texture"},
    {"effect", "Effect", 48, 48, true, false, "game effect sprite, centered, plain black background", "pixel art game effect sprite of",
     "centered, glowing, crisp shapes"},
    {"ui", "UI element", 32, 32, true, false, "game user interface icon, centered, plain white background", "pixel art game user interface element of",
     "centered, clean shapes, crisp outlines"},
};
constexpr int kKindCount = static_cast<int>(sizeof(kKinds) / sizeof(kKinds[0]));

int KindIndex(const std::string& id) {
    const std::string l = Lower(id);
    for (int i = 0; i < kKindCount; ++i) {
        if (l == kKinds[i].id) return i;
    }
    // words an LLM might use for them
    static const std::pair<const char*, const char*> alias[] = {{"texture", "tile"}, {"floor", "tile"},     {"ground", "tile"},  {"terrain", "tile"},   {"enemy", "character"},
                                                                {"npc", "character"}, {"hero", "character"}, {"monster", "character"}, {"boss", "character"}, {"weapon", "item"},
                                                                {"icon", "item"},     {"pickup", "item"},     {"collectible", "item"}, {"obstacle", "prop"},  {"decoration", "prop"},
                                                                {"furniture", "prop"}, {"object", "prop"},    {"scene", "background"}, {"face", "portrait"}, {"fx", "effect"},
                                                                {"vfx", "effect"},    {"button", "ui"},       {"hud", "ui"}};
    for (const auto& a : alias) {
        if (l.find(a.first) != std::string::npos) return KindIndex(a.second);
    }
    return 0;
}

// "pixel art, <what>, <style>, <the kind's words>"
std::string FullPrompt(int kind, const std::string& what, const std::string& style) {
    std::string w = Trim(what);
    if (Lower(w).rfind("pixel art", 0) == 0) {
        w = Trim(w.substr(9));
        if (!w.empty() && (w[0] == ',' || w[0] == ':')) w = Trim(w.substr(1));
    }
    std::string out = "pixel art";
    if (!w.empty()) out += ", " + w;
    if (!Trim(style).empty()) out += ", " + Trim(style);
    const char* suffix = kKinds[std::clamp(kind, 0, kKindCount - 1)].suffix;
    if (*suffix) out += std::string(", ") + suffix;
    return out;
}

// What a strong model (Krea, Qwen) is asked to draw.
std::string DrawPrompt(int kind, const std::string& what, const std::string& style) {
    std::string w = Trim(what);
    if (Lower(w).rfind("pixel art", 0) == 0) {
        w = Trim(w.substr(9));
        if (!w.empty() && (w[0] == ',' || w[0] == ':')) w = Trim(w.substr(1));
    }
    const Kind& k = kKinds[std::clamp(kind, 0, kKindCount - 1)];
    std::string out = std::string(k.drawLead) + " " + w;
    if (k.transparent) out += std::string(", isolated on a plain ") + (std::string(k.id) == "effect" ? "black" : "white") + " background";
    std::string st = Trim(style);
    if (k.transparent && !st.empty()) { // a cut-out needs its plain background: words about backgrounds or atmosphere in the style are left out
        std::string kept, part;
        auto flush = [&]() {
            std::string seg = Trim(part), low = Lower(seg);
            part.clear();
            const size_t against = low.find(" against ");
            if (against != std::string::npos) {
                seg = Trim(seg.substr(0, against));
                low = low.substr(0, against);
            }
            for (const char* bad : {"background", "backdrop", "atmospher", "scene", "lighting"}) {
                if (low.find(bad) != std::string::npos) return;
            }
            if (!seg.empty()) kept += (kept.empty() ? "" : ", ") + seg;
        };
        for (char c : st) {
            if (c == ',') flush();
            else part += c;
        }
        flush();
        st = kept;
    }
    if (!st.empty()) out += ", " + st;
    out += std::string(", ") + k.drawTail;
    return out;
}

// Who draws: 0 Retro Diffusion alone, 1 the ComfyUI tab's workflow (Krea), 2 Qwen Image 2.1
const char* DrawerName(int d) { return d == 1 ? "Krea" : d == 2 ? "Qwen Image 2.1" : "Retro Diffusion"; }

// ------------------------------------------------------------------- animations: Qwen Image 2.1 Edit draws every frame side by side in one picture (a sprite
// sheet: the same character in each frame, which separate edits do not keep), then the frames are cut apart, lined up and made pixel art with one palette.
struct AnimAction {
    const char* id;
    const char* label;
    int frames;
    const char* what;   // "<n> frames of <what>"
    const char* facing; // "" = no facing (turnarounds)
    const char* steps;  // every frame, separated by |
    int fps;
    int ref;            // the frame with the plain standing pose (it sets the size), -1 = the tallest
};
const AnimAction kActions[] = {
    {"idle", "Idle (breathing)", 4, "an idle breathing animation", "facing the viewer",
     "standing relaxed|breathing in: chest and shoulders slightly raised|standing relaxed|breathing out: body slightly lower", 5, 0},
    {"walk", "Walk cycle", 6, "a walking animation (a smooth walk cycle)", "facing right in side view",
     "the left leg far forward and the right leg far back, the right arm swinging forward|the legs passing each other, the body at its highest|the right leg reaching forward|"
     "the right leg far forward and the left leg far back, the left arm swinging forward|the legs passing each other again, the body at its highest|the left leg reaching forward", 9, 1},
    {"run", "Run cycle", 6, "a running animation (a fast run cycle)", "facing right in side view",
     "pushing off with the left leg, body leaning forward|both feet off the ground, legs spread wide|landing on the right foot|pushing off with the right leg, body leaning forward|"
     "both feet off the ground, legs spread wide the other way|landing on the left foot", 12, -1},
    {"attack", "Attack", 6, "an attack animation with its weapon (or its fists)", "facing right in side view",
     "ready stance|winding up, the weapon pulled back|the weapon raised high|swinging forward hard|the strike lands, fully stretched out|recovering to the ready stance", 10, 0},
    {"cast", "Cast a spell", 5, "a spell casting animation", "facing right in side view",
     "ready stance|raising its hands, gathering glowing magic|the magic glowing brightly|thrusting forward, releasing a burst of magic|back to the ready stance", 8, 0},
    {"jump", "Jump", 5, "a jump animation", "facing right in side view",
     "crouching low, ready to jump|pushing off the ground, legs stretched|in the air at the top of the jump, knees tucked|falling, legs reaching down|landing in a crouch", 8, -1},
    {"hurt", "Take damage", 3, "a hit reaction animation", "facing right in side view", "ready stance|hit: recoiling backwards, flinching|recovering", 8, 0},
    {"death", "Death", 5, "a death animation", "facing right in side view",
     "hit and staggering backwards|falling to the knees|collapsing forward|lying on the ground|lying still, fading", 7, 0},
    {"turn4", "Turnaround (4 directions)", 4, "a character turnaround", "", "seen from the front|seen from the left side|seen from behind|seen from the right side", 4, 0},
    {"turn8", "Turnaround (8 directions)", 8, "an 8-direction character turnaround", "",
     "front|front-left three-quarter view|left side|back-left three-quarter view|back|back-right three-quarter view|right side|front-right three-quarter view", 6, 0},
    {"spin", "Spin (items, coins)", 6, "a spinning animation: it turns around its vertical axis", "",
     "seen from the front|turned a quarter|seen edge-on from the side|seen from the back|turned three quarters|almost the front again", 10, 0},
    {"custom", "Something else...", 6, "", "facing right in side view", "", 8, 0},
};
constexpr int kActionCount = static_cast<int>(sizeof(kActions) / sizeof(kActions[0]));

int ActionIndex(const std::string& id) {
    for (int i = 0; i < kActionCount; ++i) {
        if (id == kActions[i].id) return i;
    }
    return kActionCount - 1;
}

std::string AnimPrompt(int action, int n, const std::string& custom, bool left) {
    const AnimAction& a = kActions[action];
    const std::string what = a.what[0] ? a.what : "an animation: " + custom;
    std::string facing = a.facing;
    const size_t p = facing.find("right");
    if (left && p != std::string::npos) facing.replace(p, 5, "left");
    std::string list;
    if (a.steps[0]) {
        std::string steps = a.steps;
        int i = 1;
        size_t at = 0;
        while (at <= steps.size()) {
            const size_t bar = steps.find('|', at);
            const std::string step = steps.substr(at, bar == std::string::npos ? std::string::npos : bar - at);
            list += "Frame " + std::to_string(i++) + ": " + step + ". ";
            if (bar == std::string::npos) break;
            at = bar + 1;
        }
    } else {
        list = "Frame 1 is the start of the motion and frame " + std::to_string(n) + " its end, in even steps. ";
    }
    return "Make a pixel art sprite sheet of the character in <image1>: " + std::to_string(n) + " frames of " + what + " side by side in one row" +
           (facing.empty() ? std::string() : ", the character " + facing) + ". " + list +
           "The same character in every frame: the same size, outfit and colours, holding the same things, full body, evenly spaced, nothing overlapping, plain white background. "
           "Same pixel art style as <image1>.";
}

// ------------------------------------------------------------------- the results
struct PxRecord {
    int id = 0;
    int activity = 0;        // the ComfyUI job while it is being made
    int stage = 0;           // 3 = being drawn (Krea / Qwen), 1 = being made by Retro Diffusion, 2 = the seam pass of a tile, 4 = Qwen Edit drawing the frames, 0 = finished (or failed)
    int mode = 0;            // 0 from a prompt, 1 a picture redrawn by Retro Diffusion, 2 a picture pixelized without AI, 3 an animation of another result
    int kind = 0;            // index into kKinds
    std::string label, desc, prompt, error, state, message; // desc: what was asked; prompt: what was sent to Retro Diffusion
    std::string style;       // the set's / the request's style words
    int drawBy = 0;          // who draws it first: 0 Retro Diffusion alone, 1 the ComfyUI tab's workflow (Krea), 2 Qwen Image 2.1
    int finish = 0;          // a drawing then: 0 pixelized as it is, 1 touched up by Retro Diffusion (2 = automatic, resolved when it is made)
    double touch = 0.35;     // ...how much
    int retries = 0;         // drawings made again because their background was not plain (not saved)
    std::wstring src, raw, cells, pixel; // the picture converted, the 8x picture ComfyUI made, every art pixel's colour (before the palette), the finished pixel art
    int w = 64, h = 64;
    bool tile = false;
    double denoise = 0.55;
    long long seed = -1;
    int setId = 0;
    std::wstring mask;       // a converted picture: SAM 3's mask of what is kept (white), for its see-through background
    int maskAct = 0;         // ...the ComfyUI job making it (-1: the vision model is choosing what to keep)
    std::string keepUsed;    // ...what SAM 3 was asked to keep
    double flashUntil = 0;   // a new result: its row lights up until then (not saved)
    int parent = 0;          // an animation: the result it animates
    int frames = 0;          // an animation: frames side by side in the sheet (each w x h); 0 = one picture
    std::string action;      // ...which action (kActions)
    bool faceLeft = false;   // ...side-view actions face left
    int fps = 8;
    PixelOptions opt;
    long long when = 0;
    LoadedImage tex;         // the pixel art (not saved)
    std::wstring texOf;      // which file tex shows
};

struct PxSet {
    int id = 0;
    std::string request, style;
    bool shared = true;      // one palette for the whole set
    PixelOptions opt;
    long long when = 0;
};

std::deque<PxRecord> g_recs; // oldest first
std::vector<PxSet> g_sets;
bool g_loaded = false;
int g_nextId = 1, g_nextSet = 1;
int g_sel = -1;    // record shown (-1 none)
int g_selSet = -1; // set shown as a grid (-1 none)
std::map<int, PixelCells> g_cellCache; // record id -> its cells

fs::path PxDir() { return fs::path(BoardsDir()).parent_path() / L"pixelart"; }

json OptJson(const PixelOptions& o) { return {{"palette", o.palette}, {"colors", o.colors}, {"transparent", o.transparent}, {"clean", o.clean}, {"outline", o.outline}}; }
PixelOptions OptFrom(const json& j) {
    PixelOptions o;
    if (!j.is_object()) return o;
    o.palette = std::clamp(j.value("palette", 0), 0, static_cast<int>(PixelPalettePresets().size()) - 1);
    o.colors = std::clamp(j.value("colors", 16), 2, 64);
    o.transparent = j.value("transparent", false);
    o.clean = j.value("clean", true);
    o.outline = j.value("outline", false);
    return o;
}

void Save() {
    json j;
    j["next"] = g_nextId;
    j["nextSet"] = g_nextSet;
    j["items"] = json::array();
    for (const PxRecord& r : g_recs) {
        j["items"].push_back({{"id", r.id}, {"mode", r.mode}, {"kind", kKinds[r.kind].id}, {"label", r.label}, {"desc", r.desc}, {"prompt", r.prompt}, {"error", r.error},
                              {"src", ToUtf8(r.src)}, {"raw", ToUtf8(r.raw)}, {"cells", ToUtf8(r.cells)}, {"pixel", ToUtf8(r.pixel)}, {"w", r.w}, {"h", r.h}, {"tile", r.tile},
                              {"denoise", r.denoise}, {"seed", r.seed}, {"set", r.setId}, {"opt", OptJson(r.opt)}, {"when", r.when}, {"style", r.style},
                              {"drawBy", r.drawBy}, {"finish", r.finish}, {"touch", r.touch}, {"mask", ToUtf8(r.mask)}, {"keep", r.keepUsed}, {"parent", r.parent}, {"frames", r.frames}, {"action", r.action},
                              {"faceLeft", r.faceLeft}, {"fps", r.fps}});
    }
    j["sets"] = json::array();
    for (const PxSet& s : g_sets) j["sets"].push_back({{"id", s.id}, {"request", s.request}, {"style", s.style}, {"shared", s.shared}, {"opt", OptJson(s.opt)}, {"when", s.when}});
    std::error_code ec;
    fs::create_directories(PxDir(), ec);
    std::ofstream out(PxDir() / L"pixelart.json", std::ios::binary | std::ios::trunc);
    if (out) out << j.dump(1, ' ', false, json::error_handler_t::replace);
}

void Load() {
    if (g_loaded) return;
    g_loaded = true;
    std::ifstream f(PxDir() / L"pixelart.json", std::ios::binary);
    if (!f) return;
    json j = json::parse(f, nullptr, false);
    if (!j.is_object()) return;
    g_nextId = j.value("next", 1);
    g_nextSet = j.value("nextSet", 1);
    if (j.contains("sets") && j["sets"].is_array()) {
        for (const json& e : j["sets"]) {
            PxSet s;
            s.id = e.value("id", 0);
            s.request = e.value("request", std::string());
            s.style = e.value("style", std::string());
            s.shared = e.value("shared", true);
            s.opt = OptFrom(e.value("opt", json()));
            s.when = e.value("when", 0LL);
            if (s.id > 0) g_sets.push_back(s);
            g_nextSet = std::max(g_nextSet, s.id + 1);
        }
    }
    if (j.contains("items") && j["items"].is_array()) {
        for (const json& e : j["items"]) {
            PxRecord r;
            r.id = e.value("id", 0);
            r.mode = e.value("mode", 0);
            r.kind = KindIndex(e.value("kind", std::string("anything")));
            r.label = e.value("label", std::string());
            r.desc = e.value("desc", std::string());
            r.prompt = e.value("prompt", std::string());
            r.error = e.value("error", std::string());
            r.src = ToWide(e.value("src", std::string()));
            r.raw = ToWide(e.value("raw", std::string()));
            r.cells = ToWide(e.value("cells", std::string()));
            r.pixel = ToWide(e.value("pixel", std::string()));
            r.w = std::clamp(e.value("w", 64), 4, 1024);
            r.h = std::clamp(e.value("h", 64), 4, 1024);
            r.tile = e.value("tile", false);
            r.denoise = e.value("denoise", 0.55);
            r.seed = e.value("seed", -1LL);
            r.setId = e.value("set", 0);
            r.opt = OptFrom(e.value("opt", json()));
            r.when = e.value("when", 0LL);
            r.style = e.value("style", std::string());
            r.drawBy = std::clamp(e.value("drawBy", 0), 0, 2);
            r.finish = std::clamp(e.value("finish", 0), 0, 2);
            r.touch = e.value("touch", 0.35);
            r.mask = ToWide(e.value("mask", std::string()));
            r.keepUsed = e.value("keep", std::string());
            r.parent = e.value("parent", 0);
            r.frames = std::clamp(e.value("frames", 0), 0, 16);
            r.action = e.value("action", std::string());
            r.faceLeft = e.value("faceLeft", false);
            r.fps = std::clamp(e.value("fps", 8), 1, 30);
            if (r.pixel.empty() && r.error.empty()) r.error = "It did not finish (the program was closed)";
            g_nextId = std::max(g_nextId, r.id + 1);
            if (r.id > 0) g_recs.push_back(std::move(r));
        }
    }
    if (!g_recs.empty()) {
        if (g_recs.back().setId > 0) g_selSet = g_recs.back().setId;
        else g_sel = g_recs.back().id;
    }
}

PxRecord* Find(int id) {
    for (PxRecord& r : g_recs) {
        if (r.id == id) return &r;
    }
    return nullptr;
}

void SelectRecord(AppState& app, int id) {
    g_sel = id;
    g_selSet = -1;
    app.requestTab = 9;
}

std::wstring RecordPixel(int id) {
    const PxRecord* r = Find(id);
    return r && r->activity <= 0 ? r->pixel : std::wstring();
}

PxSet* FindSet(int id) {
    for (PxSet& s : g_sets) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

std::vector<PxRecord*> Members(int setId) {
    std::vector<PxRecord*> out;
    for (PxRecord& r : g_recs) {
        if (r.setId == setId) out.push_back(&r);
    }
    return out;
}

// The options a record is drawn with: its own, except the palette (palette, colours, clean-up) of its set when the set shares one. The see-through
// background and the outline always stay its own (a set can mix sprites and tiles).
bool SharedSet(const PxRecord& r) {
    if (r.setId <= 0) return false;
    const PxSet* s = FindSet(r.setId);
    return s && s->shared;
}

PixelOptions Effective(const PxRecord& r) {
    PixelOptions o = r.opt;
    if (SharedSet(r)) {
        const PxSet* s = FindSet(r.setId);
        o.palette = s->opt.palette;
        o.colors = s->opt.colors;
        o.clean = s->opt.clean;
    }
    return o;
}

// ------------------------------------------------------------------- cells on disk: every art pixel's colour before the palette (a small PNG)
bool SaveCells(const std::wstring& path, const PixelCells& c) {
    std::vector<uint8_t> rgba(static_cast<size_t>(c.w) * c.h * 4, 0);
    for (size_t i = 0; i < c.lab.size(); ++i) {
        PixelLabToRgb(c.lab[i], rgba.data() + i * 4);
        rgba[i * 4 + 3] = c.opaque[i] ? 255 : 0;
    }
    return PixelSavePng(path, rgba.data(), c.w, c.h);
}

bool LoadCellsFile(const std::wstring& path, PixelCells& c) {
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (!PixelLoadRGBA(path, rgba, w, h)) return false;
    c.w = w;
    c.h = h;
    c.lab.resize(static_cast<size_t>(w) * h);
    c.opaque.resize(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < c.lab.size(); ++i) {
        c.lab[i] = PixelRgbToLab(rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2]);
        c.opaque[i] = rgba[i * 4 + 3] >= 128 ? 1 : 0;
    }
    return true;
}

// Every art pixel's colour, by the two-stage packing of Pixel Art Fixer (pixel_fixer.h): each cell votes among clean colour labels (crisp outlines), then takes
// the true colour of the pixels carrying the winner.
PixelCells PackCells(const uint8_t* rgba, int w, int h, int cols, int rows) {
    PixelCells c;
    cols = std::max(1, cols);
    rows = std::max(1, rows);
    const std::vector<uint8_t> px = PixelFixPack(rgba, w, h, cols, rows);
    c.w = cols;
    c.h = rows;
    c.lab.resize(static_cast<size_t>(cols) * rows);
    c.opaque.resize(static_cast<size_t>(cols) * rows);
    for (size_t i = 0; i < c.lab.size(); ++i) {
        c.lab[i] = PixelRgbToLab(px[i * 4], px[i * 4 + 1], px[i * 4 + 2]);
        c.opaque[i] = px[i * 4 + 3] >= 128 ? 1 : 0;
    }
    return c;
}

const PixelCells* CellsOf(PxRecord& r) {
    auto it = g_cellCache.find(r.id);
    if (it != g_cellCache.end()) return &it->second;
    if (r.cells.empty()) return nullptr;
    PixelCells c;
    if (!LoadCellsFile(r.cells, c)) return nullptr;
    return &(g_cellCache[r.id] = std::move(c));
}

// Where finished pixel art goes: ComfyUI's output folder (PixelArt), next to what ComfyUI made; otherwise this app's own folder.
std::wstring OutDir(AppState& app) {
    std::wstring dir = ComfyOutputDir(app.comfy);
    if (!dir.empty()) dir += L"\\PixelArt";
    else dir = (PxDir() / L"results").wstring();
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

std::wstring WorkDir() {
    const fs::path d = PxDir() / L"work";
    std::error_code ec;
    fs::create_directories(d, ec);
    return d.wstring();
}

// A texture replaced in the middle of a frame may already be in that frame's draw list: it is released between frames (PumpPixelArt), never at once
// (releasing it at once made the graphics driver read freed memory while the palette slider was dragged).
std::vector<LoadedImage> g_texGrave;
void ReleaseTex(PxRecord& r) {
    if (r.tex.srv) g_texGrave.push_back(r.tex);
    r.tex = LoadedImage{};
    r.texOf.clear();
}

// While a palette slider is dragged the result is redrawn every frame on screen only; its file (and the list) is written when the slider is let go.
struct PendingPng {
    std::wstring path;
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
};
std::map<int, PendingPng> g_pendingPng;
bool g_deferWrites = false, g_pendingSave = false;

// SAM 3's mask on the record's grid: a cell is kept when most of it is kept (cached per mask and grid).
struct MaskCache {
    std::wstring path;
    int w = 0, h = 0;
    std::vector<uint8_t> keep;
};
std::map<int, MaskCache> g_maskCells;
const std::vector<uint8_t>* MaskCells(const PxRecord& r, int w, int h) {
    MaskCache& m = g_maskCells[r.id];
    if (m.path == r.mask && m.w == w && m.h == h && !m.keep.empty()) return &m.keep;
    std::vector<uint8_t> rgba;
    int mw = 0, mh = 0;
    if (r.mask.empty() || !PixelLoadRGBA(r.mask, rgba, mw, mh)) return nullptr;
    const std::vector<uint8_t> onGrid = PixelBoxDownscale(rgba.data(), mw, mh, w, h);
    m.path = r.mask;
    m.w = w;
    m.h = h;
    m.keep.assign(static_cast<size_t>(w) * h, 0);
    for (size_t i = 0; i < m.keep.size(); ++i) m.keep[i] = onGrid[i * 4] >= 128 ? 1 : 0;
    // the pieces of the mask (and of what is around it), 4-connected
    auto pieces = [&](uint8_t value, std::vector<int>& label, std::vector<size_t>& size, std::vector<bool>& edge) {
        label.assign(m.keep.size(), -1);
        size.clear();
        edge.clear();
        std::vector<size_t> todo;
        for (size_t s = 0; s < m.keep.size(); ++s) {
            if (m.keep[s] != value || label[s] >= 0) continue;
            const int id = static_cast<int>(size.size());
            size.push_back(0);
            edge.push_back(false);
            label[s] = id;
            todo.push_back(s);
            while (!todo.empty()) {
                const size_t i = todo.back();
                todo.pop_back();
                ++size[id];
                const int x = static_cast<int>(i % w), y = static_cast<int>(i / w);
                if (x == 0 || y == 0 || x == w - 1 || y == h - 1) edge[id] = true;
                const int nx[4] = {x - 1, x + 1, x, x}, ny[4] = {y, y, y - 1, y + 1};
                for (int k = 0; k < 4; ++k) {
                    if (nx[k] < 0 || ny[k] < 0 || nx[k] >= w || ny[k] >= h) continue;
                    const size_t j = static_cast<size_t>(ny[k]) * w + nx[k];
                    if (m.keep[j] != value || label[j] >= 0) continue;
                    label[j] = id;
                    todo.push_back(j);
                }
            }
        }
    };
    std::vector<int> label;
    std::vector<size_t> size;
    std::vector<bool> edge;
    pieces(1, label, size, edge); // specks far from the subject (people in the distance) go: under 1.5% of the biggest piece
    size_t biggest = 0, kept = 0;
    for (size_t s : size) biggest = std::max(biggest, s);
    for (size_t i = 0; i < m.keep.size(); ++i) {
        if (m.keep[i] && size[label[i]] < std::max<size_t>(6, biggest * 15 / 1000)) m.keep[i] = 0;
        kept += m.keep[i];
    }
    pieces(0, label, size, edge); // small holes inside (a phone's screen, a gap in a held cup) close; big ones (between an arm and the body) stay
    for (size_t i = 0; i < m.keep.size(); ++i) {
        if (!m.keep[i] && !edge[label[i]] && size[label[i]] <= std::max<size_t>(4, kept * 4 / 1000)) m.keep[i] = 1;
    }
    return &m.keep;
}

// Draws the record's cells with its palette into its pixel art file (and texture).
void Render(AppState& app, PxRecord& r, const std::vector<PixelLab>* sharedPalette = nullptr) {
    const PixelCells* c = CellsOf(r);
    if (!c) return;
    const PixelOptions opt = Effective(r);
    PixelCells cut; // a photo's see-through background: only what SAM 3 kept
    if (opt.transparent && !r.mask.empty()) {
        if (const std::vector<uint8_t>* keep = MaskCells(r, c->w, c->h)) {
            cut = *c;
            for (size_t i = 0; i < cut.opaque.size() && i < keep->size(); ++i) {
                if (!(*keep)[i]) cut.opaque[i] = 0;
            }
            c = &cut;
        }
    }
    std::vector<PixelLab> own;
    if (!sharedPalette && r.mode == 3) { // an animation keeps the exact colours of the picture it animates (while its palette settings are the same)
        if (PxRecord* p = Find(r.parent)) {
            const PixelOptions po = Effective(*p);
            if (po.palette == opt.palette && po.colors == opt.colors) {
                std::vector<const PixelCells*> from;
                if (SharedSet(*p)) {
                    for (PxRecord* m : Members(p->setId)) {
                        if (const PixelCells* mc = CellsOf(*m)) from.push_back(mc);
                    }
                } else if (const PixelCells* pc = CellsOf(*p)) {
                    from.push_back(pc);
                }
                if (!from.empty()) own = MakePixelPalette(from, po);
            }
        }
    }
    if (!sharedPalette && own.empty()) own = MakePixelPalette({c}, opt);
    std::vector<uint8_t> rgba;
    RenderPixelArt(*c, sharedPalette ? *sharedPalette : own, opt, rgba);
    if (r.pixel.empty()) {
        std::wstring base = !r.raw.empty() ? fs::path(r.raw).stem().wstring()
                          : r.mode == 0 && !r.src.empty() ? fs::path(r.src).stem().wstring()
                                                          : L"px_" + std::to_wstring(r.id) + L"_" + ToWide(SafeName(r.label, 24));
        r.pixel = OutDir(app) + L"\\" + base + L"_px.png";
    }
    if (g_deferWrites) {
        g_pendingPng[r.id] = PendingPng{r.pixel, rgba, c->w, c->h};
    } else {
        g_pendingPng.erase(r.id);
        PixelSavePng(r.pixel, rgba.data(), c->w, c->h);
    }
    ReleaseTex(r);
    std::string err;
    if (CreateImageFromRGBA(app.Device(), rgba.data(), c->w, c->h, r.tex, &err)) r.texOf = r.pixel;
}

// A whole set with one palette made from all of its finished pictures.
void RenderSet(AppState& app, int setId) {
    PxSet* s = FindSet(setId);
    std::vector<PxRecord*> members = Members(setId);
    if (!s || !s->shared) {
        for (PxRecord* r : members) Render(app, *r);
        return;
    }
    std::vector<const PixelCells*> all;
    for (PxRecord* r : members) {
        if (const PixelCells* c = CellsOf(*r)) all.push_back(c);
    }
    if (all.empty()) return;
    const std::vector<PixelLab> pal = MakePixelPalette(all, s->opt);
    for (PxRecord* r : members) {
        if (CellsOf(*r)) Render(app, *r, &pal);
    }
}

void RenderAny(AppState& app, PxRecord& r) {
    if (SharedSet(r)) RenderSet(app, r.setId);
    else Render(app, r);
}

void EnsureTex(AppState& app, PxRecord& r) {
    if (r.pixel.empty() || (r.tex.srv && r.texOf == r.pixel)) return;
    ReleaseTex(r);
    std::string err;
    if (LoadImageTexture(app.Device(), r.pixel, r.tex, &err)) r.texOf = r.pixel;
    else r.texOf = r.pixel; // do not retry every frame
}

// ------------------------------------------------------------------- drawing pixel art crisp: point sampling instead of the linear sampler ImGui uses
void UsePointSampler(const ImDrawList*, const ImDrawCmd*) {
    auto* rs = static_cast<ImGui_ImplDX11_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
    if (!rs) return;
    static ID3D11SamplerState* point = nullptr;
    if (!point) {
        D3D11_SAMPLER_DESC d = {};
        d.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        d.AddressU = d.AddressV = d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        d.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
        d.MaxLOD = D3D11_FLOAT32_MAX;
        rs->Device->CreateSamplerState(&d, &point);
    }
    if (point) rs->DeviceContext->PSSetSamplers(0, 1, &point);
}

void CrispImage(ImDrawList* dl, ID3D11ShaderResourceView* srv, ImVec2 a, ImVec2 b, ImVec2 uv0 = ImVec2(0, 0), ImVec2 uv1 = ImVec2(1, 1)) {
    dl->AddCallback(UsePointSampler, nullptr);
    dl->AddImage((ImTextureID)(intptr_t)srv, a, b, uv0, uv1);
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

void Checker(ImDrawList* dl, ImVec2 a, ImVec2 b, float cell) {
    dl->AddRectFilled(a, b, IM_COL32(58, 62, 70, 255));
    cell = std::max(4.0f, cell);
    for (float y = a.y; y < b.y; y += cell) {
        for (float x = a.x + (static_cast<int>((y - a.y) / cell) % 2 ? cell : 0.0f); x < b.x; x += cell * 2) {
            dl->AddRectFilled(ImVec2(x, y), ImVec2(std::min(x + cell, b.x), std::min(y + cell, b.y)), IM_COL32(82, 88, 98, 255));
        }
    }
}

// A thumbnail (crisp, fitted, on a checker when it has transparency).
void Thumb(AppState& app, PxRecord& r, ImVec2 p, float size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    EnsureTex(app, r);
    if (!r.tex.srv || r.tex.width <= 0) {
        dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), ui::ThumbPlaceholder(), ui::Dp(4.0f));
        return;
    }
    const int n = std::max(1, r.frames); // an animation: its first frame
    const float tw = static_cast<float>(r.tex.width) / n;
    const float k = std::min(size / tw, size / r.tex.height);
    const ImVec2 dim(tw * k, r.tex.height * k);
    const ImVec2 a(p.x + (size - dim.x) * 0.5f, p.y + (size - dim.y) * 0.5f), b(a.x + dim.x, a.y + dim.y);
    if (Effective(r).transparent) Checker(dl, a, b, ui::Dp(5.0f));
    CrispImage(dl, r.tex.srv, a, b, ImVec2(0, 0), ImVec2(1.0f / n, 1));
}

// ------------------------------------------------------------------- notices: a small card in the corner when something is sent, ready or failed
struct Notice {
    std::string text;
    int recId = 0;
    double until = 0;
    bool bad = false;
};
std::vector<Notice> g_notices;
void SelectRecord(AppState& app, int id); // below
std::wstring RecordPixel(int id);          // below

// A pixel art notice: one of the app's toasts (a result again updates its card).
void Notify(const std::string& text, int recId, bool bad = false) {
    const bool ok = !bad && text.rfind("Pixel art ready", 0) == 0;
    std::function<void(AppState&)> open;
    if (recId > 0) open = [recId](AppState& app) { SelectRecord(app, recId); };
    Toast(bad ? ToastKind::Error : ok ? ToastKind::Success : ToastKind::Info, text, std::string(), recId > 0 && !bad ? RecordPixel(recId) : std::wstring(), open,
          recId > 0 ? "px" + std::to_string(recId) : std::string(), true);
}

// ------------------------------------------------------------------- the prompt writer: improving a prompt, describing a picture, planning a set
std::mutex g_phaseMutex;
std::string g_phase;
void SetPhase(const std::string& s) {
    std::lock_guard<std::mutex> lock(g_phaseMutex);
    g_phase = s;
}
std::string Phase() {
    std::lock_guard<std::mutex> lock(g_phaseMutex);
    return g_phase;
}

struct Planned {
    bool on = true;
    std::string name, prompt;
    int kind = 2, w = 32, h = 32;
};

struct PlanResult {
    bool ok = false;
    std::string error, style;
    std::vector<Planned> assets;
};

int SnapSize(int v) { return std::clamp((v + 4) / 8 * 8, 16, 160); }

PlanResult ParsePlan(const std::string& reply) {
    PlanResult r;
    const size_t a = reply.find('{'), b = reply.rfind('}');
    if (a == std::string::npos || b == std::string::npos || b <= a) {
        r.error = "The prompt writer did not answer with a plan";
        return r;
    }
    json j = json::parse(reply.substr(a, b - a + 1), nullptr, false);
    if (!j.is_object() || !j.contains("assets") || !j["assets"].is_array()) {
        r.error = "The prompt writer's plan could not be read";
        return r;
    }
    r.style = Trim(j.value("style", std::string()));
    std::string colours = Trim(j.value("colours", j.value("colors", std::string())));
    if (!colours.empty()) r.style += (r.style.empty() ? "" : ", ") + colours;
    for (const json& e : j["assets"]) {
        if (!e.is_object()) continue;
        Planned p;
        p.name = Trim(e.value("name", std::string()));
        p.prompt = Trim(e.value("prompt", std::string()));
        p.kind = KindIndex(e.value("kind", std::string("item")));
        const Kind& k = kKinds[p.kind];
        p.w = SnapSize(e.contains("w") && e["w"].is_number() ? e["w"].get<int>() : k.w);
        p.h = SnapSize(e.contains("h") && e["h"].is_number() ? e["h"].get<int>() : k.h);
        if (k.tile) p.h = p.w; // tiles are square
        if (p.prompt.empty()) p.prompt = p.name;
        if (p.name.empty()) p.name = p.prompt.substr(0, 24);
        if (!p.prompt.empty()) r.assets.push_back(p);
    }
    r.ok = !r.assets.empty();
    if (!r.ok) r.error = "The plan had no assets";
    return r;
}

PlanResult PlanSet(std::string request, std::string style, std::string comfyUrl) {
    static const char* kSystem =
        "You plan sets of game art assets that a pixel-art image model will draw one at a time. Reply with ONLY a JSON object, no preface, no code fence:\n"
        "{\"style\": \"...\", \"colours\": \"...\", \"assets\": [{\"name\": \"...\", \"kind\": \"...\", \"prompt\": \"...\", \"w\": 32, \"h\": 32}]}\n"
        "Rules:\n"
        "- Make exactly the number of assets asked for. If no number is given, choose a sensible number (at most 24). Every asset is clearly different: "
        "no two of the same creature, object or design (not four bats: a bat, a slime, a golem...).\n"
        "- Use the kinds the request names, in the numbers it names: if it asks for tiles, walls, platforms or characters, those assets have that kind (2 floor tiles = 2 assets of kind tile).\n"
        "- kind is one of: character, item, tile, wall, platform, prop, background, portrait, effect, ui.\n"
        "- name: 1 to 4 words.\n"
        "- prompt: ONE short descriptive sentence about that asset alone: what it is, its colours and materials, and for characters their clothes and pose. "
        "No style words (the style is added), no quality words (masterpiece, best quality), no backgrounds for characters and items.\n"
        "- tile and wall prompts describe a surface material that fills the whole frame (for example: mossy cobblestone floor with cracks), never a single object.\n"
        "- Sizes in pixels, multiples of 8: characters 48 to 64 (32 to 48 for a small side-view platformer), items, icons and ui 16 to 32, tiles and walls 32 (16 for 8-bit), "
        "platforms wide and short (for example 64 x 24 or 96 x 32), props 32 to 64, effects 32 to 48, portraits 64, backgrounds 128 x 72 to 160 x 96.\n"
        "- style: one short line for the whole set: ONLY the era or console look, the genre and the view (side view, top-down, isometric), for example: 16-bit SNES fantasy RPG, "
        "top-down view. No mood, atmosphere, lighting, places, scenes or backgrounds: it is added to every asset.\n"
        "- colours: a few words about the colours of the set, for example: warm earthy browns and golds.\n"
        "Example. Request: \"3 potions and 2 grass tiles for a cozy farming game\". Answer:\n"
        "{\"style\": \"16-bit cozy farming game, top-down view\", \"colours\": \"soft warm pastel colours\", \"assets\": ["
        "{\"name\": \"Health Potion\", \"kind\": \"item\", \"prompt\": \"a round glass bottle of red liquid with a cork stopper\", \"w\": 32, \"h\": 32}, "
        "{\"name\": \"Mana Potion\", \"kind\": \"item\", \"prompt\": \"a tall glass flask of glowing blue liquid with a wax seal\", \"w\": 32, \"h\": 32}, "
        "{\"name\": \"Stamina Potion\", \"kind\": \"item\", \"prompt\": \"a small square green bottle tied with string\", \"w\": 32, \"h\": 32}, "
        "{\"name\": \"Grass Tile\", \"kind\": \"tile\", \"prompt\": \"short green grass with small clover patches\", \"w\": 32, \"h\": 32}, "
        "{\"name\": \"Flower Grass Tile\", \"kind\": \"tile\", \"prompt\": \"green grass dotted with tiny yellow and white flowers\", \"w\": 32, \"h\": 32}]}";
    std::string user = "What is needed: " + request + "\n";
    if (!Trim(style).empty()) user += "Game and style: " + style + "\n";
    else user += "Game and style: choose one that fits the request.\n";
    PlanResult r;
    const ClefSession session = WriterAcquire(comfyUrl, SetPhase);
    if (!session.ok) {
        r.error = "The prompt writer could not be started (Clef mode tab > Prompt writer)";
        SetPhase("");
        return r;
    }
    SetPhase("The prompt writer is planning the set...");
    std::string reply, err;
    if (!WriterChat(kSystem, user, {}, 4000, 0.7, reply, &err)) {
        r.error = "The prompt writer did not answer" + (err.empty() ? std::string() : ": " + err);
    } else {
        r = ParsePlan(reply);
        if (!Trim(style).empty() && r.style.empty()) r.style = style;
    }
    WriterRelease(session, false);
    SetPhase("");
    return r;
}

std::string WriterOnce(std::string system, std::string user, std::wstring picture, std::string comfyUrl, std::string busyText) {
    const ClefSession session = WriterAcquire(comfyUrl, SetPhase);
    std::string out;
    if (session.ok) {
        SetPhase(busyText);
        std::vector<std::string> images;
        if (!picture.empty()) {
            const std::string url = ImageDataUrl(picture, 768);
            if (!url.empty()) images.push_back(url);
        }
        if (!WriterChat(system, user, images, 300, 0.6, out, nullptr)) out.clear();
    }
    WriterRelease(session, false);
    SetPhase("");
    return Trim(out);
}

// ------------------------------------------------------------------- the controls
struct Ctl {
    Ctl() { // a character to start with: cut out, with a dark outline
        opt.transparent = true;
        opt.outline = true;
    }
    int mode = 0; // 0 create, 1 convert, 2 a set
    // create
    std::string prompt;
    int kind = 1;
    int drawBy = -1;      // -1 = not chosen yet (Krea when the ComfyUI tab's workflow is a Krea one, else Qwen)
    int finish = 2;       // 0 pixelize the drawing, 1 Retro Diffusion touch-up, 2 automatic (by what it is)
    float touch = 0.35f;
    int w = 64, h = 64;
    int count = 1;
    // convert
    std::vector<std::wstring> sources;
    int preview = 0;
    int convW = 64;
    int convBy = 2;        // 0 no AI (only pixelize), 1 Retro Diffusion redraws it, 2 Qwen Image 2.1 Edit redraws it
    std::string keep = "person:8, animal:4, object held in hand:4"; // what SAM 3 keeps for a photo's see-through background (held things: a phone, a cup...)
    bool smartKeep = true; // the vision model (the prompt writer) looks at the picture and names what to keep
    int modeW = 0;         // pixel art mode (Clef mode, Vision board): the width, 0 = one that suits each picture
    float denoise = 0.55f;
    std::string convPrompt;
    std::wstring convPromptFor;
    // a set
    std::string request, style;
    std::vector<Planned> plan;
    std::string planError;
    // shared
    PixelOptions opt;     // the palette of new results (create, sets)
    PixelOptions convOpt; // ...of converted pictures (their background is kept unless asked)
    int steps = 20;
    float cfg = 7.0f;
    std::string negative;
    // the writer at work
    std::future<PlanResult> planFut;
    std::future<std::string> textFut;
    int textTarget = 0; // 1 the create prompt, 2 the convert prompt
};
Ctl g_ctl;

// The checkpoint list of ComfyUI, looked at now and then while the tab shows (are the Retro Diffusion models there?)
struct ModelCheck {
    std::future<std::pair<bool, std::vector<std::string>>> fut;
    bool known = false, reachable = false;
    std::vector<std::string> list;
    double at = -100;
    std::string linkMessage;
};
ModelCheck g_models;

void CheckModels(AppState& app, bool now = false) {
    if (g_models.fut.valid()) {
        if (g_models.fut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        auto res = g_models.fut.get();
        g_models.reachable = res.first;
        g_models.list = std::move(res.second);
        g_models.known = true;
    }
    if (!now && ImGui::GetTime() - g_models.at < 12.0) return;
    g_models.at = ImGui::GetTime();
    g_models.fut = std::async(std::launch::async, [url = app.comfy.url]() {
        std::vector<std::string> list;
        const bool ok = ComfyListCheckpoints(url, list);
        return std::make_pair(ok, list);
    });
}

bool HaveModels() { return !PickPixelModel(g_models.list, 64).empty(); }

// Picture pixels per art pixel for Retro Diffusion at this art size (the 16x model: 16, the others: 8).
int RdScale(int artSize) {
    const std::string m = PickPixelModel(g_models.list, artSize);
    if (m.empty()) return artSize <= 20 ? 16 : 8;
    return PixelModelScale(m);
}

void EnsureComfyReady(AppState& app) {
    if (!app.comfyLoaded) {
        app.comfyLoaded = true;
        LoadComfySettings(app.comfy);
    }
    if (!app.comfySvc) app.comfySvc = std::make_unique<ComfyService>();
}

int SendJob(AppState& app, PxRecord& r, int pass, const std::vector<std::wstring>& images, int artW, int artH, double denoise) {
    ComfyJob job;
    job.pixel = pass;
    job.pixW = artW;
    job.pixH = artH;
    job.prompt = r.prompt;
    job.negative = Trim(g_ctl.negative);
    job.denoise = denoise;
    job.cfg = g_ctl.cfg;
    job.steps = g_ctl.steps;
    job.seed = r.seed;
    job.images = images;
    job.times = 1;
    job.label = "Pixel art: " + r.label;
    return SendPixel(app, job);
}

long long NewSeed() {
    static std::mt19937_64 rng(static_cast<unsigned long long>(std::time(nullptr)) ^ 0x9e3779b97f4a7c15ULL);
    return static_cast<long long>(rng() % (1ULL << 48));
}

// Starts (or restarts) the making of a record from a prompt. A strong model draws it first (then it is pixelized, or touched up by Retro Diffusion), or Retro
// Diffusion makes it alone. Tiles are made at twice the size, the middle is kept (no frame) and then the seam is redrawn.
bool StartCreate(AppState& app, PxRecord& r) {
    r.error.clear();
    r.seed = NewSeed();
    if (r.drawBy > 0) {
        ComfyJob job;
        job.draw = r.drawBy;
        job.prompt = DrawPrompt(r.kind, r.desc, r.style);
        job.pixW = r.w;
        job.pixH = r.h;
        job.seed = r.seed;
        job.times = 1;
        job.label = "Pixel art: " + r.label;
        r.raw.clear();
        r.activity = SendPixel(app, job);
        r.stage = r.activity > 0 ? 3 : 0;
        r.state = r.activity > 0 ? "Waiting" : "";
        if (r.activity <= 0) r.error = "Could not send it to ComfyUI";
        return r.activity > 0;
    }
    const int gw = r.tile ? std::min(r.w * 2, 160) : r.w, gh = r.tile ? std::min(r.h * 2, 160) : r.h;
    r.activity = SendJob(app, r, 1, {}, gw, gh, 1.0);
    r.stage = r.activity > 0 ? 1 : 0;
    r.state = r.activity > 0 ? "Waiting" : "";
    if (r.activity <= 0) r.error = "Could not send it to ComfyUI";
    return r.activity > 0;
}

// What Qwen Image 2.1 Edit is asked when it turns a picture into pixel art. It gets the picture already shrunk onto the art grid (rough squares): asked to
// restyle a photo it keeps it a photo, but a rough pixel picture it cleans up the way a pixel artist would, on the same grid.
std::string QwenPixelPrompt(const PxRecord& r) {
    std::string p = "Clean up <image1> into polished 16-bit pixel art: keep the square pixel grid and the same scene, composition and colours, but redraw it the way a "
                    "pixel artist would: clean readable shapes, flat cel shading with few tones, crisp dark outlines, a limited colour palette, no noise, no blur.";
    std::string d = Trim(r.desc);
    if (d.size() > 300) d = d.substr(0, 300);
    if (!d.empty()) p += " It shows: " + d + ".";
    return p;
}

// Converting a picture: no AI (its own grid, or the size asked), Qwen Image 2.1 Edit redraws it as pixel art (then it is packed onto the grid), or Retro
// Diffusion redraws it (the picture shrunk to the art size and enlarged again in 8 x 8 cells: a clean pixel grid to redraw).
bool StartConvert(AppState& app, PxRecord& r) {
    r.error.clear();
    std::vector<uint8_t> rgba;
    int sw = 0, sh = 0;
    std::string err;
    if (!PixelLoadRGBA(r.src, rgba, sw, sh, &err)) {
        r.error = "Could not read the picture: " + err;
        return false;
    }
    if (r.mode == 2) { // no AI: its own grid when it has one (AI-made "pixel art", found by the Pixel Art Fixer detector), otherwise the size asked for
        std::vector<uint8_t> src = rgba;
        int dw = sw, dh = sh;
        if (static_cast<long long>(sw) * sh > 4000000LL) { // the detector's limit: a smaller copy has the same grid in fewer pixels
            const double k = std::sqrt(4000000.0 / (static_cast<double>(sw) * sh));
            dw = std::max(16, static_cast<int>(sw * k));
            dh = std::max(16, static_cast<int>(sh * k));
            src = PixelBoxDownscale(rgba.data(), sw, sh, dw, dh);
        }
        PixelFixDetection det;
        bool grid = PixelFixDetect(src.data(), dw, dh, true, det);
        if (grid && det.consensus.rfind("fast:", 0) != 0 && det.consensus.find('+') == std::string::npos) {
            // an arbitrated / low-confidence answer: only when the edges really line up (a photo has no grid)
            std::vector<float> xs, ys;
            grid = DetectPixelGrid(src.data(), dw, dh, xs, ys);
        }
        PixelCells c = grid ? PackCells(src.data(), dw, dh, det.cols, det.rows) : PackCells(rgba.data(), sw, sh, r.w, r.h);
        r.w = c.w;
        r.h = c.h;
        r.cells = WorkDir() + L"\\cells_" + std::to_wstring(r.id) + L".png";
        SaveCells(r.cells, c);
        g_cellCache.erase(r.id);
        r.stage = 0;
        RenderAny(app, r);
        return true;
    }
    if (r.drawBy == 2) { // Qwen cleans up the picture shrunk onto the art grid (enlarged in squares to about a megapixel), without the edit LoRAs
        std::vector<uint8_t> flat = rgba;
        for (size_t i = 0; i < flat.size(); i += 4) { // see-through parts become white
            const float a = flat[i + 3] / 255.0f;
            for (int c = 0; c < 3; ++c) flat[i + c] = static_cast<uint8_t>(std::lround(flat[i + c] * a + 255.0f * (1.0f - a)));
            flat[i + 3] = 255;
        }
        // twice the art grid (squares too coarse read as a blurry photo, which Qwen "restores" into a photo again), at most 192 on the long side
        const double k = std::min(2.0, 192.0 / std::max(r.w, r.h));
        const int gw = std::max(r.w, static_cast<int>(std::lround(r.w * k))), gh = std::max(r.h, static_cast<int>(std::lround(r.h * k)));
        const int f = std::max(2, static_cast<int>(std::lround(std::sqrt(1048576.0 / (static_cast<double>(gw) * gh)))));
        const std::vector<uint8_t> shrunk = PixelBoxDownscale(flat.data(), sw, sh, gw, gh);
        const std::vector<uint8_t> rough = PixelNearestUpscale(shrunk.data(), gw, gh, f);
        const std::wstring in = WorkDir() + L"\\rough_" + std::to_wstring(r.id) + L".png";
        if (!PixelSavePng(in, rough.data(), gw * f, gh * f)) {
            r.error = "Could not write the picture for ComfyUI";
            return false;
        }
        ComfyJob job;
        job.images = {in};
        job.prompt = QwenPixelPrompt(r);
        job.sheetW = std::clamp(gw * f, 256, 2048);
        job.sheetH = std::clamp(gh * f, 256, 2048);
        job.savePrefix = "PixelArt/redrawn";
        r.seed = NewSeed();
        job.seed = r.seed;
        job.times = 1;
        job.label = "Pixel art: " + r.label;
        r.raw.clear();
        r.activity = SendSheetEdit(app, job);
        r.stage = r.activity > 0 ? 5 : 0;
        r.state = r.activity > 0 ? "Waiting" : "";
        if (r.activity <= 0) r.error = "Could not send it to ComfyUI";
        return r.activity > 0;
    }
    if (std::max(r.w, r.h) > 160) { // Retro Diffusion's models stop at 160 pixels
        const double k = 160.0 / std::max(r.w, r.h);
        r.w = std::max(8, static_cast<int>(std::lround(r.w * k)));
        r.h = std::max(8, static_cast<int>(std::lround(r.h * k)));
    }
    std::vector<uint8_t> flat = rgba;
    for (size_t i = 0; i < flat.size(); i += 4) { // see-through parts become white (the model has no transparency)
        const float a = flat[i + 3] / 255.0f;
        for (int c = 0; c < 3; ++c) flat[i + c] = static_cast<uint8_t>(std::lround(flat[i + c] * a + 255.0f * (1.0f - a)));
        flat[i + 3] = 255;
    }
    const int sc = RdScale(std::max(r.w, r.h));
    const std::vector<uint8_t> shrunk = PixelBoxDownscale(flat.data(), sw, sh, r.w, r.h);
    const std::vector<uint8_t> grid = PixelNearestUpscale(shrunk.data(), r.w, r.h, sc);
    const std::wstring in = WorkDir() + L"\\in_" + std::to_wstring(r.id) + L".png";
    if (!PixelSavePng(in, grid.data(), r.w * sc, r.h * sc)) {
        r.error = "Could not write the picture for ComfyUI";
        return false;
    }
    r.seed = NewSeed();
    r.activity = SendJob(app, r, 2, {in}, r.w, r.h, r.denoise);
    r.stage = r.activity > 0 ? 1 : 0;
    r.state = r.activity > 0 ? "Waiting" : "";
    if (r.activity <= 0) r.error = "Could not send it to ComfyUI";
    return r.activity > 0;
}

// An animation: Qwen Image 2.1 Edit gets the pixel art (enlarged in whole squares, on white) and draws all the frames side by side on a wide canvas.
bool StartAnimate(AppState& app, PxRecord& r) {
    r.error.clear();
    if (r.src.empty() || GetFileAttributesW(r.src.c_str()) == INVALID_FILE_ATTRIBUTES) {
        PxRecord* p = Find(r.parent);
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        if (!p || p->pixel.empty() || !PixelLoadRGBA(p->pixel, rgba, w, h)) {
            r.error = "The pixel art it animates is gone";
            return false;
        }
        const int f = std::max(1, 448 / std::max(w, h));
        const std::vector<uint8_t> big = PixelNearestUpscale(rgba.data(), w, h, f);
        const int bw = w * f, bh = h * f, pad = std::max(bw, bh) / 8;
        const int cw = bw + pad * 2, ch = bh + pad * 2;
        std::vector<uint8_t> base(static_cast<size_t>(cw) * ch * 4, 255);
        for (int y = 0; y < bh; ++y) {
            for (int x = 0; x < bw; ++x) {
                const uint8_t* s = big.data() + (static_cast<size_t>(y) * bw + x) * 4;
                uint8_t* d = base.data() + (static_cast<size_t>(y + pad) * cw + x + pad) * 4;
                const float a = s[3] / 255.0f; // see-through parts become white
                for (int c = 0; c < 3; ++c) d[c] = static_cast<uint8_t>(std::lround(s[c] * a + 255.0f * (1.0f - a)));
            }
        }
        r.src = WorkDir() + L"\\anim_base_" + std::to_wstring(r.id) + L".png";
        if (!PixelSavePng(r.src, base.data(), cw, ch)) {
            r.error = "Could not write the picture for ComfyUI";
            r.src.clear();
            return false;
        }
    }
    const int n = std::max(2, r.frames);
    ComfyJob job;
    job.images = {r.src};
    job.prompt = r.prompt;
    job.sheetW = std::min(3072, (n * 352 + 31) / 32 * 32); // about 350 pixels a frame, 512 high: room for a whole figure in every frame
    job.sheetH = 512;
    r.seed = NewSeed();
    job.seed = r.seed;
    job.times = 1;
    job.label = "Pixel art: " + r.label;
    r.raw.clear();
    r.activity = SendSheetEdit(app, job);
    r.stage = r.activity > 0 ? 4 : 0;
    r.state = r.activity > 0 ? "Waiting" : "";
    if (r.activity <= 0) r.error = "Could not send it to ComfyUI";
    return r.activity > 0;
}

// Does the picture have a plain background (most of its border one colour, or see-through already)? Then no cut-out is needed: the plain colour is cut away.
bool PlainBackground(const std::wstring& path) {
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (!PixelLoadRGBA(path, rgba, w, h) || w < 4 || h < 4) return true;
    std::vector<const uint8_t*> border;
    for (int x = 0; x < w; x += 2) {
        border.push_back(rgba.data() + static_cast<size_t>(x) * 4);
        border.push_back(rgba.data() + (static_cast<size_t>(h - 1) * w + x) * 4);
    }
    for (int y = 0; y < h; y += 2) {
        border.push_back(rgba.data() + static_cast<size_t>(y) * w * 4);
        border.push_back(rgba.data() + (static_cast<size_t>(y) * w + w - 1) * 4);
    }
    size_t clear = 0;
    int med[3];
    for (int ch = 0; ch < 3; ++ch) {
        std::vector<int> v;
        for (const uint8_t* q : border) v.push_back(q[ch]);
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        med[ch] = v[v.size() / 2];
    }
    size_t plain = 0;
    for (const uint8_t* q : border) {
        if (q[3] < 128) ++clear;
        else if (std::abs(q[0] - med[0]) + std::abs(q[1] - med[1]) + std::abs(q[2] - med[2]) <= 60) ++plain;
    }
    return clear * 2 > border.size() || (plain + clear) * 4 >= border.size() * 3;
}

bool Converted(const PxRecord& r) { return (r.mode == 1 || r.mode == 2) && !r.src.empty(); }
bool NeedsCutout(const PxRecord& r) { return Converted(r) && Effective(r).transparent && r.mask.empty() && r.maskAct == 0 && !PlainBackground(r.src); }

std::string DefaultKeep() { return Trim(g_ctl.keep).empty() ? std::string("person:8, animal:4, object held in hand:4") : Trim(g_ctl.keep); }

// SAM 3 finds what to keep in the picture; the mask makes everything else see-through, whichever way the pixel art was made.
void SendCutout(AppState& app, PxRecord& r, const std::string& keep) {
    EnsureComfyReady(app);
    ComfyJob job;
    job.cutout = 1;
    job.images = {r.src};
    job.prompt = keep;
    job.times = 1;
    job.label = "Pixel art: cutting out " + r.label;
    r.keepUsed = keep;
    r.maskAct = SendPixel(app, job);
    if (r.maskAct <= 0) {
        r.maskAct = 0;
        Notify("Could not send the cut-out of \"" + r.label + "\" to ComfyUI", r.id, true);
    }
}

// Smart: the vision model looks at the picture first and names its main subjects and what they hold, with how many ("woman:1, ipod:1"), so SAM 3 keeps
// those and not the people far away in the background. One picture at a time (the model is loaded once for a queue of them).
std::deque<int> g_keepQueue;
std::future<std::string> g_keepFut;
int g_keepFor = 0;

void StartCutout(AppState& app, PxRecord& r) {
    if (!Converted(r)) return;
    if (g_ctl.smartKeep && WriterConfigured() && WriterCanSee()) {
        r.maskAct = -1;
        g_keepQueue.push_back(r.id);
        return;
    }
    SendCutout(app, r, DefaultKeep());
}

// The vision model's answer as SAM 3 prompts: at most 6 short nouns, each with how many (SAM finds one of a noun without a count).
std::string ParseKeep(const std::string& reply) {
    std::string line = reply;
    const size_t nl = line.find('\n');
    if (nl != std::string::npos) line = line.substr(0, nl);
    std::string clean;
    for (char ch : line) {
        const unsigned char uc = static_cast<unsigned char>(ch);
        if (std::isalnum(uc) || ch == ' ' || ch == ',' || ch == ':' || ch == '-') clean += static_cast<char>(std::tolower(uc));
    }
    std::string out;
    int n = 0;
    size_t at = 0;
    while (at <= clean.size() && n < 6) {
        const size_t comma = clean.find(',', at);
        std::string item = Trim(clean.substr(at, comma == std::string::npos ? std::string::npos : comma - at));
        at = comma == std::string::npos ? clean.size() + 1 : comma + 1;
        if (item.empty() || item.size() > 40) continue;
        if (item.find(':') == std::string::npos) item += ":1";
        out += (out.empty() ? "" : ", ") + item;
        ++n;
    }
    return out;
}

void PumpKeepQueue(AppState& app) {
    if (g_keepFut.valid()) {
        if (g_keepFut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        const std::string keep = ParseKeep(g_keepFut.get());
        PxRecord* r = Find(g_keepFor);
        g_keepFor = 0;
        if (r && r->maskAct == -1) SendCutout(app, *r, keep.empty() ? DefaultKeep() : keep);
    }
    while (!g_keepQueue.empty()) {
        const int id = g_keepQueue.front();
        g_keepQueue.pop_front();
        PxRecord* r = Find(id);
        if (!r || r->maskAct != -1) continue;
        g_keepFor = id;
        g_keepFut = std::async(std::launch::async, WriterOnce,
                               std::string("You choose what to cut out of a picture to make a sprite. Reply with ONLY a comma-separated list for a segmentation model, nothing "
                                           "else: the main subjects (the characters, creatures or objects the picture is about) and anything they hold or carry. Short "
                                           "common nouns (woman, man, girl, dog, sword, phone, cup, bag). After each noun write :N, how many of it are main subjects "
                                           "(woman:1, dog:2). Never list the background, scenery, furniture far away, or small people in the background. At most 5 items."),
                               std::string("What should be cut out of this picture?"), r->src, app.comfy.url, std::string("The vision model is looking at what to keep..."));
        break;
    }
}

// SAM 3's masks, one per thing it found: the main ones (at least 6% of the biggest) are kept, and smaller ones only when they touch those along a good part
// of their edge (a phone in a hand); small figures in the background go. Written as one mask (white = kept).
// A mask's small separate pieces go (figures in the distance that SAM put in the same mask), at full size, before they could merge with the subject.
void KeepMainPieces(std::vector<uint8_t>& m, int W, int H) {
    std::vector<int> label(m.size(), -1);
    std::vector<size_t> size;
    std::vector<size_t> todo;
    for (size_t s = 0; s < m.size(); ++s) {
        if (!m[s] || label[s] >= 0) continue;
        const int id = static_cast<int>(size.size());
        size.push_back(0);
        label[s] = id;
        todo.push_back(s);
        while (!todo.empty()) {
            const size_t i = todo.back();
            todo.pop_back();
            ++size[id];
            const int x = static_cast<int>(i % W), y = static_cast<int>(i / W);
            const int nx[4] = {x - 1, x + 1, x, x}, ny[4] = {y, y, y - 1, y + 1};
            for (int k = 0; k < 4; ++k) {
                if (nx[k] < 0 || ny[k] < 0 || nx[k] >= W || ny[k] >= H) continue;
                const size_t j = static_cast<size_t>(ny[k]) * W + nx[k];
                if (!m[j] || label[j] >= 0) continue;
                label[j] = id;
                todo.push_back(j);
            }
        }
    }
    size_t biggest = 0;
    for (size_t s : size) biggest = std::max(biggest, s);
    for (size_t i = 0; i < m.size(); ++i) {
        if (m[i] && size[label[i]] * 100 < biggest * 2) m[i] = 0;
    }
}

bool CombineMasks(const std::vector<std::wstring>& files, const std::wstring& out) {
    std::vector<std::vector<uint8_t>> masks;
    int W = 0, H = 0;
    for (const std::wstring& f : files) {
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        if (!PixelLoadRGBA(f, rgba, w, h)) continue;
        if (masks.empty()) {
            W = w;
            H = h;
        } else if (w != W || h != H) {
            continue;
        }
        std::vector<uint8_t> m(static_cast<size_t>(w) * h);
        for (size_t i = 0; i < m.size(); ++i) m[i] = rgba[i * 4] >= 128 ? 1 : 0;
        KeepMainPieces(m, w, h);
        masks.push_back(std::move(m));
    }
    if (masks.empty()) return false;
    std::vector<size_t> area(masks.size(), 0);
    size_t biggest = 0;
    for (size_t k = 0; k < masks.size(); ++k) {
        for (uint8_t v : masks[k]) area[k] += v;
        biggest = std::max(biggest, area[k]);
    }
    if (biggest == 0) return false;
    std::vector<uint8_t> keep(static_cast<size_t>(W) * H, 0);
    std::vector<bool> big(masks.size(), false);
    for (size_t k = 0; k < masks.size(); ++k) {
        big[k] = area[k] * 100 >= biggest * 6;
        if (!big[k]) continue;
        for (size_t i = 0; i < keep.size(); ++i) keep[i] |= masks[k][i];
    }
    // the main ones grown by a few pixels (what touches them)
    const int d = std::max(2, std::min(W, H) / 200);
    std::vector<uint8_t> tmp(keep.size(), 0), grown(keep.size(), 0);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            uint8_t v = 0;
            for (int k = std::max(0, x - d); k <= std::min(W - 1, x + d) && !v; ++k) v = keep[static_cast<size_t>(y) * W + k];
            tmp[static_cast<size_t>(y) * W + x] = v;
        }
    }
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            uint8_t v = 0;
            for (int k = std::max(0, y - d); k <= std::min(H - 1, y + d) && !v; ++k) v = tmp[static_cast<size_t>(k) * W + x];
            grown[static_cast<size_t>(y) * W + x] = v;
        }
    }
    for (size_t k = 0; k < masks.size(); ++k) {
        if (big[k] || area[k] == 0) continue;
        size_t edge = 0, touching = 0;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const size_t i = static_cast<size_t>(y) * W + x;
                if (!masks[k][i]) continue;
                const bool border = x == 0 || y == 0 || x == W - 1 || y == H - 1 || !masks[k][i - 1] || !masks[k][i + 1] || !masks[k][i - W] || !masks[k][i + W];
                if (!border) continue;
                ++edge;
                if (grown[i]) ++touching;
            }
        }
        if (edge > 0 && touching * 100 >= edge * 15) {
            for (size_t i = 0; i < keep.size(); ++i) keep[i] |= masks[k][i];
        }
    }
    std::vector<uint8_t> rgba(keep.size() * 4, 255);
    for (size_t i = 0; i < keep.size(); ++i) rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = keep[i] ? 255 : 0;
    return PixelSavePng(out, rgba.data(), W, H);
}

// ------------------------------------------------------------------- finishing: ComfyUI's picture -> cells -> pixel art (and the seam pass of tiles)
void Fail(PxRecord& r, const std::string& why) {
    r.error = why;
    r.activity = 0;
    r.stage = 0;
    r.state.clear();
}

// Qwen's sprite sheet is back: the frames are cut apart and lined up (PixelSliceSheet: the character as many pixels tall as the picture it animates), packed
// onto the art grid in one go and drawn with one palette.
void FinishAnimation(AppState& app, PxRecord& r, const std::wstring& raw) {
    std::vector<uint8_t> rgba;
    int W = 0, H = 0;
    if (!PixelLoadRGBA(raw, rgba, W, H) || W < 16 || H < 16) {
        Fail(r, "Could not read Qwen's sprite sheet");
        return;
    }
    // how tall the character is in the pixel art it animates (its cut-out; a picture with a background: most of its height)
    double artTall = 48;
    if (PxRecord* p = Find(r.parent)) {
        artTall = p->h * 0.88;
        if (const PixelCells* pc = CellsOf(*p)) {
            int y0 = pc->h, y1 = -1;
            for (int y = 0; y < pc->h; ++y) {
                for (int x = 0; x < pc->w; ++x) {
                    if (pc->opaque[static_cast<size_t>(y) * pc->w + x]) {
                        y0 = std::min(y0, y);
                        y1 = std::max(y1, y);
                    }
                }
            }
            if (y1 >= y0 && y1 - y0 + 1 < pc->h) artTall = y1 - y0 + 1;
        }
    }
    const int n = std::max(1, r.frames);
    PixelSheet sh;
    if (!PixelSliceSheet(rgba.data(), W, H, n, kActions[ActionIndex(r.action)].ref, artTall, sh)) {
        Fail(r, "Qwen's sprite sheet had nothing on it");
        return;
    }
    const PixelCells c = PackCells(sh.rgba.data(), sh.frames * sh.cellW, sh.cellH, sh.frames * sh.artW, sh.artH);
    r.raw = raw;
    r.frames = sh.frames;
    r.w = sh.artW;
    r.h = sh.artH;
    r.cells = WorkDir() + L"\\cells_" + std::to_wstring(r.id) + L".png";
    SaveCells(r.cells, c);
    g_cellCache.erase(r.id);
    r.pixel.clear();
    r.activity = 0;
    r.stage = 0;
    r.state.clear();
    if (sh.frames != n) app.SetStatus("\"" + r.label + "\": Qwen drew " + std::to_string(sh.frames) + " frames (" + std::to_string(n) + " were asked for)");
    RenderAny(app, r);
}

// The first pass of a tile is done: keep the middle, shift it by half, redraw the cross where the edges now meet.
void StartSeamPass(AppState& app, PxRecord& r, const std::wstring& raw, bool drawing) {
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (!PixelLoadRGBA(raw, rgba, w, h)) {
        Fail(r, "Could not read ComfyUI's picture");
        return;
    }
    // the art size it was made at: twice the tile (a drawing is read as a grid of that size)
    const int gw = drawing ? r.w * 2 : std::max(1, w / RdScale(std::min(r.w * 2, 160))), gh = drawing ? r.h * 2 : std::max(1, h / RdScale(std::min(r.h * 2, 160)));
    const PixelCells big = PackCells(rgba.data(), w, h, gw, gh);
    PixelCells mid;
    mid.w = r.w;
    mid.h = r.h;
    mid.lab.resize(static_cast<size_t>(r.w) * r.h);
    mid.opaque.assign(static_cast<size_t>(r.w) * r.h, 1);
    const int ox = std::max(0, (gw - r.w) / 2), oy = std::max(0, (gh - r.h) / 2);
    for (int y = 0; y < r.h; ++y) {
        for (int x = 0; x < r.w; ++x) mid.lab[static_cast<size_t>(y) * r.w + x] = big.lab[static_cast<size_t>(std::min(gh - 1, y + oy)) * gw + std::min(gw - 1, x + ox)];
    }
    // shifted by half: the wrap-around edges meet in a cross in the middle
    PixelCells rolled = mid;
    for (int y = 0; y < r.h; ++y) {
        for (int x = 0; x < r.w; ++x) rolled.lab[static_cast<size_t>(y) * r.w + x] = mid.lab[static_cast<size_t>((y + r.h / 2) % r.h) * r.w + (x + r.w / 2) % r.w];
    }
    const std::wstring rolledCells = WorkDir() + L"\\roll_" + std::to_wstring(r.id) + L".png";
    SaveCells(rolledCells, rolled);
    PixelOptions o = Effective(r);
    o.transparent = false;
    o.clean = false;
    std::vector<uint8_t> px;
    RenderPixelArt(rolled, MakePixelPalette({&rolled}, o), o, px);
    const int sc = RdScale(std::max(r.w, r.h));
    const std::vector<uint8_t> grid = PixelNearestUpscale(px.data(), r.w, r.h, sc);
    std::vector<uint8_t> mask(static_cast<size_t>(r.w) * r.h * 4, 0);
    const int bx = std::max(2, r.w / 8), by = std::max(2, r.h / 8);
    for (int y = 0; y < r.h; ++y) {
        for (int x = 0; x < r.w; ++x) {
            const bool on = std::abs(x - r.w / 2 + 0.5) < bx || std::abs(y - r.h / 2 + 0.5) < by;
            uint8_t* m = mask.data() + (static_cast<size_t>(y) * r.w + x) * 4;
            m[0] = m[1] = m[2] = on ? 255 : 0;
            m[3] = 255;
        }
    }
    const std::vector<uint8_t> maskBig = PixelNearestUpscale(mask.data(), r.w, r.h, sc);
    const std::wstring in = WorkDir() + L"\\seam_" + std::to_wstring(r.id) + L".png", mk = WorkDir() + L"\\seammask_" + std::to_wstring(r.id) + L".png";
    if (!PixelSavePng(in, grid.data(), r.w * sc, r.h * sc) || !PixelSavePng(mk, maskBig.data(), r.w * sc, r.h * sc)) {
        Fail(r, "Could not write the seam pass pictures");
        return;
    }
    if (!drawing) r.raw = raw;
    r.activity = SendJob(app, r, 3, {in, mk}, r.w, r.h, 0.6);
    if (r.activity <= 0) {
        Fail(r, "Could not send the seam pass to ComfyUI");
        return;
    }
    r.stage = 2;
    r.state = "Seam pass";
}

void FinishSeam(AppState& app, PxRecord& r, const std::wstring& raw2) {
    PixelCells rolled;
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (!LoadCellsFile(WorkDir() + L"\\roll_" + std::to_wstring(r.id) + L".png", rolled) || !PixelLoadRGBA(raw2, rgba, w, h)) {
        Fail(r, "Could not read the seam pass");
        return;
    }
    const PixelCells redrawn = PackCells(rgba.data(), w, h, r.w, r.h);
    const int bx = std::max(2, r.w / 8), by = std::max(2, r.h / 8);
    for (int y = 0; y < r.h && y < redrawn.h; ++y) {
        for (int x = 0; x < r.w && x < redrawn.w; ++x) {
            const bool on = std::abs(x - r.w / 2 + 0.5) < bx || std::abs(y - r.h / 2 + 0.5) < by;
            if (on) rolled.lab[static_cast<size_t>(y) * r.w + x] = redrawn.lab[static_cast<size_t>(y) * redrawn.w + x];
        }
    }
    r.cells = WorkDir() + L"\\cells_" + std::to_wstring(r.id) + L".png";
    SaveCells(r.cells, rolled);
    g_cellCache.erase(r.id);
    r.activity = 0;
    r.stage = 0;
    r.state.clear();
    RenderAny(app, r);
}

// A strong model's drawing is back: cropped to its subject (sprites, items...), then pixelized, or touched up by Retro Diffusion first.
void FinishDrawing(AppState& app, PxRecord& r, const std::wstring& drawn) {
    r.src = drawn;
    if (r.tile) {
        StartSeamPass(app, r, drawn, true);
        return;
    }
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (!PixelLoadRGBA(drawn, rgba, w, h)) {
        Fail(r, "Could not read the drawing");
        return;
    }
    if (kKinds[r.kind].transparent && r.retries < 1) { // a sprite needs a plain background to be cut out: most of the border one colour
        std::vector<int> ch3[3];
        auto take = [&](int x, int y) {
            const uint8_t* q = rgba.data() + (static_cast<size_t>(y) * w + x) * 4;
            for (int c = 0; c < 3; ++c) ch3[c].push_back(q[c]);
        };
        for (int x = 0; x < w; x += 2) {
            take(x, 0);
            take(x, h - 1);
        }
        for (int y = 0; y < h; y += 2) {
            take(0, y);
            take(w - 1, y);
        }
        int med[3];
        for (int c = 0; c < 3; ++c) {
            std::vector<int> v = ch3[c];
            std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
            med[c] = v[v.size() / 2];
        }
        size_t plain = 0;
        for (size_t i = 0; i < ch3[0].size(); ++i) {
            if (std::abs(ch3[0][i] - med[0]) + std::abs(ch3[1][i] - med[1]) + std::abs(ch3[2][i] - med[2]) <= 60) ++plain;
        }
        if (plain * 4 < ch3[0].size() * 3) {
            ++r.retries;
            app.SetStatus("\"" + r.label + "\" was drawn on a busy background: drawing it again");
            if (StartCreate(app, r)) return;
        }
    }
    int cw = w, ch = h;
    std::vector<uint8_t> crop;
    if (kKinds[r.kind].transparent) crop = PixelSubjectCrop(rgba.data(), w, h, static_cast<double>(r.w) / r.h, cw, ch);
    else crop = rgba;
    const bool touch = r.finish == 1 || (r.finish == 2 && !kKinds[r.kind].transparent); // automatic: full pictures (portraits, scenes) get the touch-up
    if (touch) { // Retro Diffusion redraws it on its own grid (cleaner pixels; details can change)
        const int sc = RdScale(std::max(r.w, r.h));
        const std::vector<uint8_t> shrunk = PixelBoxDownscale(crop.data(), cw, ch, r.w, r.h);
        const std::vector<uint8_t> grid = PixelNearestUpscale(shrunk.data(), r.w, r.h, sc);
        const std::wstring in = WorkDir() + L"\\in_" + std::to_wstring(r.id) + L".png";
        if (!PixelSavePng(in, grid.data(), r.w * sc, r.h * sc)) {
            Fail(r, "Could not write the picture for ComfyUI");
            return;
        }
        r.activity = SendJob(app, r, 2, {in}, r.w, r.h, r.touch);
        if (r.activity <= 0) {
            Fail(r, "Could not send the touch-up to ComfyUI");
            return;
        }
        r.stage = 1;
        r.state = "Touch-up";
        return;
    }
    // pixelized as it is: the two-stage packing on the art grid (crisp outlines, the drawing's true colours), then the palette
    const PixelCells c = PackCells(crop.data(), cw, ch, r.w, r.h);
    r.cells = WorkDir() + L"\\cells_" + std::to_wstring(r.id) + L".png";
    SaveCells(r.cells, c);
    g_cellCache.erase(r.id);
    r.pixel.clear();
    r.activity = 0;
    r.stage = 0;
    r.state.clear();
    RenderAny(app, r);
}

// Qwen's pixel-art redraw of a picture is back: cut to its subject when the background goes, then packed onto the art grid.
void FinishRedrawn(AppState& app, PxRecord& r, const std::wstring& drawn) {
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (!PixelLoadRGBA(drawn, rgba, w, h)) {
        Fail(r, "Could not read Qwen's picture");
        return;
    }
    const PixelCells c = PackCells(rgba.data(), w, h, r.w, r.h); // the whole picture, as composed (a see-through background comes from the mask)
    r.raw = drawn;
    r.cells = WorkDir() + L"\\cells_" + std::to_wstring(r.id) + L".png";
    SaveCells(r.cells, c);
    g_cellCache.erase(r.id);
    r.pixel.clear();
    r.activity = 0;
    r.stage = 0;
    r.state.clear();
    RenderAny(app, r);
}

void Finish(AppState& app, PxRecord& r, const std::wstring& raw) {
    if (r.stage == 5) {
        FinishRedrawn(app, r, raw);
        return;
    }
    if (r.mode == 3) {
        FinishAnimation(app, r, raw);
        return;
    }
    if (r.stage == 3) {
        FinishDrawing(app, r, raw);
        return;
    }
    if (r.tile && r.stage == 1) {
        StartSeamPass(app, r, raw, false);
        return;
    }
    if (r.tile && r.stage == 2) {
        FinishSeam(app, r, raw);
        return;
    }
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (!PixelLoadRGBA(raw, rgba, w, h)) {
        Fail(r, "Could not read ComfyUI's picture");
        return;
    }
    const PixelCells c = PackCells(rgba.data(), w, h, r.w, r.h); // every 8 x 8 (16 x 16 for the 16x model) cell is one art pixel
    r.raw = raw;
    r.w = c.w;
    r.h = c.h;
    r.cells = WorkDir() + L"\\cells_" + std::to_wstring(r.id) + L".png";
    SaveCells(r.cells, c);
    g_cellCache.erase(r.id);
    r.pixel.clear(); // named after the new picture
    r.activity = 0;
    r.stage = 0;
    r.state.clear();
    RenderAny(app, r);
}

void Sync(AppState& app) {
    bool any = false;
    for (const PxRecord& r : g_recs) any = any || r.activity > 0 || r.maskAct > 0;
    if (!any || !app.comfySvc) return;
    const std::vector<ComfyActivity> acts = app.comfySvc->Activity();
    bool changed = false;
    for (PxRecord& r : g_recs) { // SAM 3 cut-outs
        if (r.maskAct <= 0) continue;
        const ComfyActivity* a = nullptr;
        for (const ComfyActivity& x : acts) {
            if (x.id == r.maskAct) a = &x;
        }
        if (!a || a->state == ComfyState::Failed) {
            Notify("The cut-out of \"" + r.label + "\" failed" + (a && !a->message.empty() ? ": " + a->message : std::string()), r.id, true);
            r.maskAct = 0;
            changed = true;
        } else if (a->state == ComfyState::Done) {
            std::vector<std::wstring> files;
            for (const ComfyResult& f : a->files) {
                if (!f.localPath.empty() && !f.video) files.push_back(f.localPath);
            }
            r.maskAct = 0;
            changed = true;
            const std::wstring file = WorkDir() + L"\\mask_" + std::to_wstring(r.id) + L".png";
            if (files.empty() || !CombineMasks(files, file)) {
                Notify("SAM 3 found nothing to keep in \"" + r.label + "\" (" + r.keepUsed + "): change what to keep and cut it out again", r.id, true);
                continue;
            }
            r.mask = file;
            g_maskCells.erase(r.id);
            std::vector<uint8_t> m;
            int mw = 0, mh = 0;
            bool found = false;
            if (PixelLoadRGBA(file, m, mw, mh)) {
                for (size_t i = 0; i < m.size() && !found; i += 4) found = m[i] >= 128;
            }
            if (!found) {
                r.mask.clear();
                Notify("SAM 3 found nothing to keep in \"" + r.label + "\" (" + r.keepUsed + "): change what to keep and cut it out again", r.id, true);
            } else {
                if (!r.cells.empty() && r.activity <= 0) RenderAny(app, r);
                Notify("Cut out: " + r.label + " (kept: " + r.keepUsed + ")", r.id);
            }
        }
    }
    for (PxRecord& r : g_recs) {
        if (r.activity <= 0) continue;
        const ComfyActivity* a = nullptr;
        for (const ComfyActivity& x : acts) {
            if (x.id == r.activity) a = &x;
        }
        if (!a) {
            Fail(r, "ComfyUI lost track of it");
            changed = true;
            continue;
        }
        r.message = a->message;
        switch (a->state) {
            case ComfyState::Waiting: r.state = r.stage == 2 ? "Seam pass: waiting" : "Waiting"; break;
            case ComfyState::Starting: r.state = "Starting ComfyUI"; break;
            case ComfyState::Sending: r.state = "Sending"; break;
            case ComfyState::Queued: r.state = r.stage == 2 ? "Seam pass: queued" : "Queued"; break;
            case ComfyState::Running: r.state = r.stage == 5 ? "Qwen is redrawing it" : r.stage == 4 ? "Qwen Edit is drawing the frames" : r.stage == 2 ? "Seam pass" : r.stage == 3 ? std::string(DrawerName(r.drawBy)) + " is drawing" : r.state == "Touch-up" ? "Touch-up" : "Drawing"; break;
            default: break;
        }
        if (a->state == ComfyState::Done) {
            std::wstring file;
            for (const ComfyResult& f : a->files) {
                if (!f.localPath.empty() && !f.video) {
                    file = f.localPath;
                    break;
                }
            }
            if (file.empty()) Fail(r, "ComfyUI made no picture");
            else Finish(app, r, file);
            if (r.setId <= 0 && r.activity <= 0) Notify(r.error.empty() ? "Pixel art ready: " + r.label : "\"" + r.label + "\" failed: " + r.error, r.id, !r.error.empty());
            changed = true;
        } else if (a->state == ComfyState::Failed) {
            Fail(r, a->message);
            if (r.setId <= 0) Notify("\"" + r.label + "\" failed: " + r.error, r.id, true);
            changed = true;
        }
    }
    if (changed) Save();
}

// ------------------------------------------------------------------- the viewer
struct View {
    float zoom = 0;  // 0 = fit
    ImVec2 pan{0, 0};
    int shown = 0;   // 0 pixel art, 1 the picture it came from
    bool grid = false;
    bool tiled = true;
    bool sheet = false; // an animation: every frame side by side instead of playing it
    bool play = true;
    int frame = 0;
    double frameAt = 0;
    int lastRec = -2;
    LoadedImage before;
    std::wstring beforePath;
};
View g_view;
bool g_playing = false; // an animation is playing on screen (the window keeps drawing)

void DrawPixelCanvas(AppState& app, PxRecord& r, ImVec2 region) {
    View& v = g_view;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##pxcanvas", region);
    const bool hovered = ImGui::IsItemHovered();
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, ImVec2(origin.x + region.x, origin.y + region.y), IM_COL32(14, 16, 20, 255), ui::Dp(6.0f));
    EnsureTex(app, r);
    const bool showBefore = v.shown == 1;
    const std::wstring beforePath = r.mode == 3 ? r.raw : r.mode == 0 ? (r.src.empty() ? r.raw : r.src) : r.src;
    if (showBefore && v.beforePath != beforePath) {
        ReleaseImage(v.before);
        v.before = LoadedImage{};
        v.beforePath = beforePath;
        std::string err;
        if (!beforePath.empty()) LoadImageTexture(app.Device(), beforePath, v.before, &err);
    }
    const LoadedImage* img = showBefore ? &v.before : &r.tex;
    const bool tiled = !showBefore && r.tile && v.tiled;
    if (img->srv && img->width > 0) {
        // measured in art pixels, so the picture it came from fills the same frame as the pixel art
        float artW = static_cast<float>(r.tex.width > 0 ? r.tex.width : r.w), artH = static_cast<float>(r.tex.height > 0 ? r.tex.height : r.h);
        const bool oneFrame = !showBefore && r.frames > 1 && !v.sheet;
        ImVec2 uv0(0, 0), uv1(1, 1);
        if (oneFrame) { // an animation: one frame at a time
            artW /= r.frames;
            if (v.play) {
                g_playing = true;
                const double now = ImGui::GetTime();
                if (now - v.frameAt >= 1.0 / std::max(1, r.fps)) {
                    v.frame = (v.frame + 1) % r.frames;
                    v.frameAt = now;
                }
            }
            v.frame = std::clamp(v.frame, 0, r.frames - 1);
            uv0 = ImVec2(static_cast<float>(v.frame) / r.frames, 0);
            uv1 = ImVec2(static_cast<float>(v.frame + 1) / r.frames, 1);
        }
        if (showBefore && r.mode == 3) { // Qwen's sheet: its own shape
            artW = static_cast<float>(img->width);
            artH = static_cast<float>(img->height);
        }
        const float reps = tiled ? 3.0f : 1.0f;
        float fit = std::min(region.x / (artW * reps), region.y / (artH * reps)) * 0.94f;
        if (fit >= 1.0f) fit = std::floor(fit); // whole numbers keep every pixel the same size
        if (hovered && io.MouseWheel != 0.0f) {
            const float cur = v.zoom > 0 ? v.zoom : fit;
            float next = io.MouseWheel > 0 ? (cur >= 1 ? cur + std::max(1.0f, std::floor(cur * 0.25f)) : cur * 1.25f) : (cur > 1 ? cur - std::max(1.0f, std::floor(cur * 0.2f)) : cur / 1.25f);
            next = std::clamp(next, 0.25f, 64.0f);
            const ImVec2 c(origin.x + region.x * 0.5f, origin.y + region.y * 0.5f);
            const float k = next / cur;
            v.pan.x = (io.MousePos.x - c.x) - (io.MousePos.x - c.x - v.pan.x) * k;
            v.pan.y = (io.MousePos.y - c.y) - (io.MousePos.y - c.y - v.pan.y) * k;
            v.zoom = next;
        }
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
            v.pan.x += io.MouseDelta.x;
            v.pan.y += io.MouseDelta.y;
        }
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            v.zoom = 0;
            v.pan = ImVec2(0, 0);
        }
        const float scale = v.zoom > 0 ? v.zoom : fit;
        const ImVec2 one(artW * scale, artH * scale);
        const ImVec2 all(one.x * reps, one.y * reps);
        const ImVec2 c(origin.x + region.x * 0.5f + v.pan.x, origin.y + region.y * 0.5f + v.pan.y);
        const ImVec2 a(std::floor(c.x - all.x * 0.5f), std::floor(c.y - all.y * 0.5f));
        dl->PushClipRect(origin, ImVec2(origin.x + region.x, origin.y + region.y), true);
        if (!showBefore && Effective(r).transparent) Checker(dl, a, ImVec2(a.x + all.x, a.y + all.y), std::max(ui::Dp(6.0f), scale));
        for (int ty = 0; ty < static_cast<int>(reps); ++ty) {
            for (int tx = 0; tx < static_cast<int>(reps); ++tx) {
                const ImVec2 p(a.x + one.x * tx, a.y + one.y * ty);
                if (showBefore) dl->AddImage((ImTextureID)(intptr_t)img->srv, p, ImVec2(p.x + one.x, p.y + one.y));
                else CrispImage(dl, img->srv, p, ImVec2(p.x + one.x, p.y + one.y), uv0, uv1);
            }
        }
        if (tiled) dl->AddRect(ImVec2(a.x + one.x, a.y + one.y), ImVec2(a.x + one.x * 2, a.y + one.y * 2), IM_COL32(255, 255, 255, 70), 0, 0, 1.0f);
        if (v.grid && !showBefore && scale >= 6.0f) {
            const ImU32 gc = IM_COL32(0, 0, 0, 60);
            for (int x = 0; x <= static_cast<int>(artW * reps); ++x) dl->AddLine(ImVec2(a.x + x * scale, a.y), ImVec2(a.x + x * scale, a.y + all.y), gc);
            for (int y = 0; y <= static_cast<int>(artH * reps); ++y) dl->AddLine(ImVec2(a.x, a.y + y * scale), ImVec2(a.x + all.x, a.y + y * scale), gc);
        }
        dl->PopClipRect();
        char info[128];
        if (oneFrame) snprintf(info, sizeof(info), "%dx  |  %d x %d pixels  |  frame %d of %d", static_cast<int>(std::lround(scale)), static_cast<int>(artW), static_cast<int>(artH), v.frame + 1, r.frames);
        else snprintf(info, sizeof(info), "%dx  |  %d x %d pixels", static_cast<int>(std::lround(scale)), static_cast<int>(artW), static_cast<int>(artH));
        const ImVec2 ts = ImGui::CalcTextSize(info);
        dl->AddRectFilled(ImVec2(origin.x + 8, origin.y + region.y - ts.y - 14), ImVec2(origin.x + 20 + ts.x, origin.y + region.y - 6), IM_COL32(0, 0, 0, 160), ui::Dp(5.0f));
        dl->AddText(ImVec2(origin.x + 14, origin.y + region.y - ts.y - 10), IM_COL32(230, 232, 236, 255), info);
    } else {
        const std::string text = r.activity > 0 ? (r.state.empty() ? std::string("Working...") : r.state) + (r.message.empty() ? std::string() : ": " + r.message)
                                                : !r.error.empty() ? "Failed: " + r.error : showBefore ? "No picture" : "Not made yet";
        ImGui::PushClipRect(origin, ImVec2(origin.x + region.x, origin.y + region.y), true);
        const ImVec2 ts = ImGui::CalcTextSize(text.c_str(), nullptr, false, region.x - 24);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(origin.x + std::max(12.0f, (region.x - ts.x) * 0.5f), origin.y + (region.y - ts.y) * 0.5f),
                    !r.error.empty() ? ImGui::GetColorU32(ui::kBad) : IM_COL32(220, 220, 230, 230), text.c_str(), nullptr, region.x - 24);
        ImGui::PopClipRect();
    }
    if (r.activity > 0 && img->srv) { // being made again: say so over the old one
        const std::string text = (r.state.empty() ? std::string("Working...") : r.state) + (r.message.empty() ? std::string() : ": " + r.message);
        const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
        dl->AddRectFilled(ImVec2(origin.x + 8, origin.y + 8), ImVec2(origin.x + 20 + ts.x, origin.y + 16 + ts.y), IM_COL32(0, 0, 0, 170), ui::Dp(5.0f));
        dl->AddText(ImVec2(origin.x + 14, origin.y + 12), IM_COL32(255, 255, 255, 235), text.c_str());
    }
}

// The folder a set is saved to (MV_PICK_FOLDER answers instead of the dialog, for tests).
std::wstring PickExportFolder() {
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = GetEnvironmentVariableW(L"MV_PICK_FOLDER", buf, MAX_PATH * 2);
    if (n > 0 && n < MAX_PATH * 2) return std::wstring(buf, n);
    return OpenFolderDialog(GetActiveWindow());
}

// The Animate popup of a result: which action, then a new result (an animation) is made from it.
struct AnimChoice {
    int action = 1;
    int frames = 6;
    bool left = false;
    std::string custom;
};
AnimChoice g_anim;

// A new result: an animation of `r` (shown at once, made by ComfyUI).
void MakeAnimation(AppState& app, PxRecord& r, int action, int n, const std::string& custom, bool left) {
    const AnimAction& a = kActions[action];
    const bool own = !a.steps[0];
    PxRecord an;
    an.id = g_nextId++;
    an.mode = 3;
    an.parent = r.id;
    an.kind = r.kind;
    an.label = r.label + " - " + (own ? SafeName(custom, 24) : std::string(a.label));
    an.desc = own ? custom : std::string(a.label);
    an.style = r.style;
    an.action = a.id;
    an.frames = n;
    an.faceLeft = left;
    an.fps = a.fps;
    an.prompt = AnimPrompt(action, n, custom, left);
    an.opt = Effective(r);
    an.w = r.w;
    an.h = r.h;
    an.when = static_cast<long long>(std::time(nullptr));
    g_recs.push_back(std::move(an));
    PxRecord& made = g_recs.back();
    StartAnimate(app, made);
    g_sel = made.id;
    g_selSet = -1;
    g_view.zoom = 0;
    g_view.pan = ImVec2(0, 0);
    Save();
}

void AnimatePopup(AppState& app, PxRecord& r) {
    if (!ImGui::BeginPopup("##pxanim")) return;
    ImGui::PushFont(ui::BoldFont());
    ImGui::TextUnformatted("Animate it");
    ImGui::PopFont();
    ImGui::TextDisabled("Qwen Image 2.1 Edit draws every frame in one picture,\nthen they become pixel art with this picture's colours.");
    ImGui::SetNextItemWidth(ui::Dp(260.0f));
    if (ImGui::BeginCombo("Action##pxanimact", kActions[g_anim.action].label)) {
        for (int i = 0; i < kActionCount; ++i) {
            char lbl[96];
            if (kActions[i].steps[0]) snprintf(lbl, sizeof(lbl), "%s  (%d frames)", kActions[i].label, kActions[i].frames);
            else snprintf(lbl, sizeof(lbl), "%s", kActions[i].label);
            if (ImGui::Selectable(lbl, g_anim.action == i)) g_anim.action = i;
        }
        ImGui::EndCombo();
    }
    const AnimAction& a = kActions[g_anim.action];
    const bool custom = !a.steps[0];
    if (custom) {
        InputLine("##pxanimcustom", "what it does, e.g. swings a sword over its head", g_anim.custom, ui::Dp(260.0f));
        ImGui::SetNextItemWidth(ui::Dp(260.0f));
        ImGui::SliderInt("frames##pxanimn", &g_anim.frames, 2, 8);
    }
    if (std::string(a.facing).find("right") != std::string::npos) ImGui::Checkbox("Facing left (instead of right)", &g_anim.left);
    ImGui::Spacing();
    ImGui::BeginDisabled(custom && Trim(g_anim.custom).empty());
    if (ImGui::Button("Make the animation", ImVec2(ui::Dp(260.0f), 0))) {
        MakeAnimation(app, r, g_anim.action, custom ? g_anim.frames : a.frames, Trim(g_anim.custom), g_anim.left);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::EndPopup();
}

// The frames of an animation as their own PNGs, the sheet and a small json (frame size, count, speed).
void SaveFrames(AppState& app, PxRecord& r) {
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (r.frames < 1 || !PixelLoadRGBA(r.pixel, rgba, w, h)) return;
    const std::wstring dir = PickExportFolder();
    if (dir.empty()) return;
    const int fw = w / r.frames;
    const std::string base = SafeName(r.label);
    int saved = 0;
    json files = json::array();
    for (int i = 0; i < r.frames; ++i) {
        std::vector<uint8_t> one(static_cast<size_t>(fw) * h * 4);
        for (int y = 0; y < h; ++y) std::memcpy(one.data() + static_cast<size_t>(y) * fw * 4, rgba.data() + (static_cast<size_t>(y) * w + i * fw) * 4, static_cast<size_t>(fw) * 4);
        char num[8];
        snprintf(num, sizeof(num), "%02d", i + 1);
        const std::string file = base + " " + num + ".png";
        if (PixelSavePng(dir + L"\\" + ToWide(file), one.data(), fw, h)) ++saved;
        files.push_back(file);
    }
    PixelSavePng(dir + L"\\" + ToWide(base + " sheet.png"), rgba.data(), w, h);
    json info = {{"name", r.label}, {"action", r.action}, {"frames", r.frames}, {"frame", {fw, h}}, {"fps", r.fps}, {"sheet", base + " sheet.png"}, {"files", files}};
    std::ofstream out(fs::path(dir) / ToWide(base + ".json"), std::ios::binary | std::ios::trunc);
    if (out) out << info.dump(2, ' ', false, json::error_handler_t::replace);
    app.SetStatus("Saved " + std::to_string(saved) + " frames, the sheet and " + base + ".json to " + ToUtf8(dir));
}

// The action row of one result.
void ResultActions(AppState& app, PxRecord& r) {
    const bool done = !r.pixel.empty() && GetFileAttributesW(r.pixel.c_str()) != INVALID_FILE_ATTRIBUTES;
    ImGui::BeginDisabled(!done);
    if (ImGui::SmallButton(r.frames > 0 ? "Save the sheet..." : "Save as PNG...")) {
        const std::wstring path = SavePngFileDialog(GetActiveWindow(), ToWide(SafeName(r.label)) + (r.frames > 0 ? L" sheet.png" : L".png"));
        if (!path.empty()) {
            std::error_code ec;
            fs::copy_file(r.pixel, path, fs::copy_options::overwrite_existing, ec);
            app.SetStatus(ec ? "Could not save it: " + ec.message() : "Saved " + ToUtf8(fs::path(path).filename().wstring()));
        }
    }
    ui::SameLineIfFits("Save bigger...");
    if (ImGui::SmallButton("Save bigger...")) ImGui::OpenPopup("##pxbig");
    if (ImGui::BeginPopup("##pxbig")) {
        ImGui::TextDisabled("Every pixel becomes a square of:");
        for (int f : {2, 4, 8, 16}) {
            char lbl[48];
            snprintf(lbl, sizeof(lbl), "%d x %d  (%d x %d pixels)", f, f, r.w * std::max(1, r.frames) * f, r.h * f);
            if (ImGui::MenuItem(lbl)) {
                std::vector<uint8_t> rgba;
                int w = 0, h = 0;
                if (PixelLoadRGBA(r.pixel, rgba, w, h)) {
                    const std::wstring path = SavePngFileDialog(GetActiveWindow(), ToWide(SafeName(r.label)) + L"_x" + std::to_wstring(f) + L".png");
                    if (!path.empty()) {
                        const std::vector<uint8_t> big = PixelNearestUpscale(rgba.data(), w, h, f);
                        app.SetStatus(PixelSavePng(path, big.data(), w * f, h * f) ? "Saved " + ToUtf8(fs::path(path).filename().wstring()) : std::string("Could not save it"));
                    }
                }
            }
        }
        ImGui::EndPopup();
    }
    if (r.frames > 0) {
        ui::SameLineIfFits("Save the frames...");
        if (ImGui::SmallButton("Save the frames...")) SaveFrames(app, r);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Every frame as its own PNG (real size), the sheet, and a .json with the frame size, count and speed");
    }
    ui::SameLineIfFits("Copy");
    if (ImGui::SmallButton("Copy")) {
        std::string msg;
        CopyImagesToClipboard(GetActiveWindow(), {r.pixel}, false, &msg);
        app.SetStatus(msg);
    }
    ui::SameLineIfFits("Show in Explorer");
    if (ImGui::SmallButton("Show in Explorer")) {
        const std::wstring args = L"/select,\"" + r.pixel + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }
    ui::SameLineIfFits("Open in the gallery");
    if (ImGui::SmallButton("Open in the gallery")) {
        app.OpenFile(r.pixel);
        app.requestTab = 0;
    }
    ImGui::EndDisabled();
    if (r.mode != 3) {
        ui::SameLineIfFits("Animate...");
        ImGui::BeginDisabled(!done || r.activity > 0);
        if (ImGui::SmallButton("Animate...")) ImGui::OpenPopup("##pxanim");
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Make a sprite-sheet animation of it: idle, walk, run, attack, jump, death, a turnaround... (Qwen Image 2.1 Edit draws the frames)");
        AnimatePopup(app, r);
    }
    ui::SameLineIfFits("Make it again");
    ImGui::BeginDisabled(r.activity > 0 || ((r.mode == 1 || r.mode == 2) && r.src.empty()));
    if (ImGui::SmallButton("Make it again")) {
        r.retries = 0;
        if (r.mode == 0 ? StartCreate(app, r) : r.mode == 3 ? StartAnimate(app, r) : StartConvert(app, r)) Save();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("The same request again with a new seed: a new version replaces this one");
}

void DrawSetGrid(AppState& app, PxSet& s, float height) {
    std::vector<PxRecord*> members = Members(s.id);
    int done = 0, working = 0, failed = 0;
    for (PxRecord* r : members) {
        if (r->activity > 0) ++working;
        else if (!r->error.empty()) ++failed;
        else if (!r->pixel.empty()) ++done;
    }
    ImGui::TextDisabled("%d of %d made%s%s", done, static_cast<int>(members.size()), working ? (", " + std::to_string(working) + " being made").c_str() : "",
                        failed ? (", " + std::to_string(failed) + " failed").c_str() : "");
    if (!s.style.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Style: %s", s.style.c_str());
        ImGui::PopTextWrapPos();
    }
    if (ImGui::SmallButton("Save the set to a folder...")) {
        const std::wstring dir = PickExportFolder();
        if (!dir.empty()) {
            // each picture, a sprite sheet of all of them (same cell size, left to right) and a list of what is where
            int cw = 8, ch = 8, n = 0;
            for (PxRecord* r : members) {
                if (r->pixel.empty()) continue;
                cw = std::max(cw, r->w);
                ch = std::max(ch, r->h);
                ++n;
            }
            const int cols = std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n)))));
            const int rows = std::max(1, (n + cols - 1) / cols);
            std::vector<uint8_t> sheet(static_cast<size_t>(cols * cw) * rows * ch * 4, 0);
            json list = json::array();
            int k = 0, saved = 0;
            for (PxRecord* r : members) {
                std::vector<uint8_t> rgba;
                int w = 0, h = 0;
                if (r->pixel.empty() || !PixelLoadRGBA(r->pixel, rgba, w, h)) continue;
                char num[8];
                snprintf(num, sizeof(num), "%02d", k + 1);
                const std::string file = std::string(num) + " " + SafeName(r->label) + ".png";
                if (PixelSavePng(dir + L"\\" + ToWide(file), rgba.data(), w, h)) ++saved;
                const int sx = (k % cols) * cw, sy = (k / cols) * ch;
                for (int y = 0; y < h && y < ch; ++y) {
                    std::memcpy(sheet.data() + (static_cast<size_t>(sy + y) * cols * cw + sx) * 4, rgba.data() + static_cast<size_t>(y) * w * 4, static_cast<size_t>(std::min(w, cw)) * 4);
                }
                list.push_back({{"name", r->label}, {"kind", kKinds[r->kind].id}, {"file", file}, {"x", sx}, {"y", sy}, {"w", w}, {"h", h}, {"tiles", r->tile}, {"prompt", r->desc}});
                ++k;
            }
            PixelSavePng(dir + L"\\sheet.png", sheet.data(), cols * cw, rows * ch);
            json info = {{"request", s.request}, {"style", s.style}, {"cell", {cw, ch}}, {"columns", cols}, {"assets", list}};
            std::ofstream out(fs::path(dir) / L"set.json", std::ios::binary | std::ios::trunc);
            if (out) out << info.dump(2, ' ', false, json::error_handler_t::replace);
            app.SetStatus("Saved " + std::to_string(saved) + " pictures, sheet.png and set.json to " + ToUtf8(dir));
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Every picture as its own PNG (real size), sheet.png with all of them in a grid, and set.json (names, kinds, where each one is on the sheet)");
    ImGui::SameLine();
    ImGui::BeginDisabled(failed == 0);
    if (ImGui::SmallButton("Make the failed ones again")) {
        for (PxRecord* r : members) {
            if (r->activity <= 0 && !r->error.empty()) Converted(*r) ? StartConvert(app, *r) : StartCreate(app, *r);
        }
        Save();
    }
    ImGui::EndDisabled();
    ImGui::BeginChild("##setgrid", ImVec2(0, std::max(ui::Dp(100.0f), height - ImGui::GetCursorPosY() - ui::Dp(8.0f))), false);
    const float cell = ui::Dp(150.0f);
    const float avail = ImGui::GetContentRegionAvail().x;
    const int cols = std::max(1, static_cast<int>(avail / (cell + ImGui::GetStyle().ItemSpacing.x)));
    int i = 0;
    for (PxRecord* r : members) {
        ImGui::PushID(r->id);
        if (i % cols) ImGui::SameLine();
        ImGui::BeginGroup();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##cell", ImVec2(cell, cell))) {
            g_sel = r->id;
            g_selSet = -1;
            g_view.zoom = 0;
            g_view.pan = ImVec2(0, 0);
        }
        const bool hov = ImGui::IsItemHovered();
        if (ImGui::BeginPopupContextItem("##cellmenu")) {
            if (ImGui::MenuItem("Make it again", nullptr, false, r->activity <= 0)) {
                r->retries = 0;
                if (Converted(*r)) StartConvert(app, *r);
                else StartCreate(app, *r);
                Save();
            }
            if (ImGui::MenuItem("Open it", nullptr, false, !r->pixel.empty())) {
                g_sel = r->id;
                g_selSet = -1;
            }
            ImGui::EndPopup();
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + cell, p.y + cell), hov ? IM_COL32(255, 255, 255, 22) : IM_COL32(255, 255, 255, 8), ui::Dp(6.0f));
        if (!r->pixel.empty()) {
            const float pad = ui::Dp(10.0f);
            Thumb(app, *r, ImVec2(p.x + pad, p.y + pad), cell - pad * 2);
        } else {
            const char* t = r->activity > 0 ? (r->state.empty() ? "Working..." : r->state.c_str()) : !r->error.empty() ? "Failed" : "Waiting";
            const ImVec2 ts = ImGui::CalcTextSize(t);
            dl->AddText(ImVec2(p.x + (cell - ts.x) * 0.5f, p.y + (cell - ts.y) * 0.5f), r->error.empty() ? ImGui::GetColorU32(ui::kTextDim) : ImGui::GetColorU32(ui::kBad), t);
        }
        if (hov && !ImGui::IsPopupOpen("##cellmenu"))
            ImGui::SetTooltip("%s (%s, %d x %d)\n%s%s\nClick: open it. Right click: make it again", r->label.c_str(), kKinds[r->kind].label, r->w, r->h, r->desc.c_str(),
                              r->error.empty() ? "" : ("\nFailed: " + r->error).c_str());
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cell);
        ImGui::TextUnformatted(r->label.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        ImGui::PopID();
        ++i;
    }
    ImGui::EndChild();
}

// ------------------------------------------------------------------- the palette panel (live: it redraws the chosen result, or the whole set)
bool PaletteControls(PixelOptions& o, bool forSet) {
    bool changed = false;
    const auto& presets = PixelPalettePresets();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##pxpalette", presets[std::clamp(o.palette, 0, static_cast<int>(presets.size()) - 1)].name)) {
        for (int i = 0; i < static_cast<int>(presets.size()); ++i) {
            if (ImGui::Selectable(presets[i].name, o.palette == i)) {
                o.palette = i;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip(forSet ? "One palette for every picture of the set, so they belong together" : "The colours the pixel art may use");
    if (o.palette == 0) {
        ImGui::SetNextItemWidth(ui::Dp(180.0f));
        if (ImGui::SliderInt("colours##pxcolors", &o.colors, 2, 64)) changed = true;
    }
    if (ImGui::Checkbox("See-through background", &o.transparent)) changed = true;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("The colour that owns the border is cut away (sprites, items, icons)");
    ImGui::SameLine();
    ImGui::BeginDisabled(!o.transparent);
    if (ImGui::Checkbox("Outline", &o.outline)) changed = true;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("A dark one-pixel outline around the cut-out");
    if (ImGui::Checkbox("Clean up stray pixels", &o.clean)) changed = true;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("A lone pixel close in colour to all four of its neighbours takes their colour (eyes and highlights stay)");
    return changed;
}

void KindChips(int& kind, int& w, int& h, PixelOptions& opt) {
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    for (int i = 0; i < kKindCount; ++i) {
        const float bw = ImGui::CalcTextSize(kKinds[i].label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        if (i > 0 && ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + bw < right) ImGui::SameLine();
        const bool on = kind == i;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ui::kAccent);
        if (ImGui::SmallButton(kKinds[i].label)) {
            kind = i;
            w = kKinds[i].w;
            h = kKinds[i].h;
            opt.transparent = kKinds[i].transparent;
            opt.outline = kKinds[i].transparent && std::string(kKinds[i].id) != "effect"; // sprites get a dark outline, effects stay soft
        }
        if (on) ImGui::PopStyleColor();
    }
}

void SizeChips(int& w, int& h, bool square) {
    static const int sizes[] = {16, 24, 32, 48, 64, 96, 128, 160};
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    for (int i = 0; i < 8; ++i) {
        char lbl[16];
        snprintf(lbl, sizeof(lbl), "%d", sizes[i]);
        const float bw = ImGui::CalcTextSize(lbl).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        if (i > 0 && ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + bw < right) ImGui::SameLine();
        const bool on = w == sizes[i] && (!square || h == sizes[i]);
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ui::kAccent);
        if (ImGui::SmallButton(lbl)) {
            const double aspect = w > 0 ? static_cast<double>(h) / w : 1.0;
            w = sizes[i];
            h = square ? sizes[i] : std::clamp(static_cast<int>(std::lround(w * aspect / 8.0)) * 8, 8, 160);
        }
        if (on) ImGui::PopStyleColor();
    }
}

// The width of converted pictures: up to 512 (Retro Diffusion's models only up to 160).
void ConvWidthChips(float right) {
    static const int sizes[] = {32, 48, 64, 96, 128, 160, 192, 256, 320, 384, 512};
    for (int i = 0; i < static_cast<int>(sizeof(sizes) / sizeof(sizes[0])); ++i) {
        char lbl[16];
        snprintf(lbl, sizeof(lbl), "%d", sizes[i]);
        const float bw = ImGui::CalcTextSize(lbl).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        if (i > 0 && ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + bw < right) ImGui::SameLine();
        const bool on = g_ctl.convW == sizes[i];
        const bool tooBig = g_ctl.convBy == 1 && sizes[i] > 160;
        ImGui::BeginDisabled(tooBig);
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ui::kAccent);
        if (ImGui::SmallButton(lbl)) g_ctl.convW = sizes[i];
        if (on) ImGui::PopStyleColor();
        ImGui::EndDisabled();
        if (tooBig && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Retro Diffusion's models only work up to 160 pixels");
    }
}

// A width that suits the picture: about 240 pixels on its long side, never more than a third of its own width.
int SmartWidth(int sw, int sh) {
    static const int sizes[] = {32, 48, 64, 96, 128, 160, 192, 256, 320, 384, 512};
    if (sw <= 0 || sh <= 0) return 64;
    const double want = std::min(240.0 * sw / std::max(sw, sh), sw / 3.0);
    int best = sizes[0];
    for (int s : sizes) {
        if (std::abs(s - want) < std::abs(best - want)) best = s;
    }
    return std::max(32, best);
}

std::string ModelLine(int w, int h) {
    const std::string m = PickPixelModel(g_models.list, std::max(w, h));
    if (m.empty()) return std::max(w, h) <= 20 ? "RetroDiffusion16xModel" : std::max(w, h) <= 80 ? "RetroDiffusion24x-64xModel" : "RetroDiffusion128xModel";
    const size_t slash = m.find_last_of("/\\");
    std::string s = slash == std::string::npos ? m : m.substr(slash + 1);
    if (s.size() > 12 && s.compare(s.size() - 12, 12, ".safetensors") == 0) s.resize(s.size() - 12);
    return s;
}

// What a picture shows, for the redraw: its Vision description when it has one, else its prompt.
std::string KnownDescription(AppState& app, const std::wstring& path) {
    for (const auto& item : app.visionLib) {
        if (_wcsicmp(item.imagePath.c_str(), path.c_str()) == 0 && !Trim(item.prompt).empty()) return Trim(item.prompt).substr(0, 400);
    }
    const ImageMetadata meta = ReadImageMetadata(path);
    std::string p = Trim(meta.parsed.positivePrompt);
    if (p.size() > 400) p = p.substr(0, 400);
    return p;
}

void AddSource(AppState& app, const std::wstring& path) {
    if (path.empty()) return;
    for (const std::wstring& s : g_ctl.sources) {
        if (_wcsicmp(s.c_str(), path.c_str()) == 0) return;
    }
    g_ctl.sources.push_back(path);
    g_ctl.preview = static_cast<int>(g_ctl.sources.size()) - 1;
    if (g_ctl.sources.size() == 1) { // a width that suits this picture
        std::vector<uint8_t> probe;
        int sw = 0, sh = 0;
        if (PixelLoadRGBA(path, probe, sw, sh)) g_ctl.convW = g_ctl.convBy == 1 ? std::min(160, SmartWidth(sw, sh)) : SmartWidth(sw, sh);
    }
    if (g_ctl.sources.size() == 1 || Trim(g_ctl.convPrompt).empty()) {
        g_ctl.convPrompt = KnownDescription(app, path);
        g_ctl.convPromptFor = path;
    }
}

void PumpWriter(AppState& app) {
    if (g_ctl.textFut.valid() && g_ctl.textFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const std::string text = g_ctl.textFut.get();
        if (text.empty()) app.SetStatus("The prompt writer gave no answer");
        else if (g_ctl.textTarget == 1) g_ctl.prompt = text;
        else if (g_ctl.textTarget == 2) g_ctl.convPrompt = text;
        g_ctl.textTarget = 0;
    }
    if (g_ctl.planFut.valid() && g_ctl.planFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        PlanResult r = g_ctl.planFut.get();
        if (r.ok) {
            g_ctl.plan = std::move(r.assets);
            if (!r.style.empty()) g_ctl.style = r.style;
            g_ctl.planError.clear();
        } else {
            g_ctl.planError = r.error;
        }
    }
}

bool WriterBusy() { return g_ctl.planFut.valid() || g_ctl.textFut.valid(); }

int DefaultDrawer(AppState& app) {
    if (ComfyHasDrawWorkflow(app.comfy) && Lower(ComfyDrawWorkflowName(app.comfy)).find("krea") != std::string::npos) return 1;
    return 2;
}

// Who draws, and what happens to the drawing (create and sets).
void DrawerControls(AppState& app) {
    if (g_ctl.drawBy < 0) g_ctl.drawBy = DefaultDrawer(app);
    if (g_ctl.drawBy == 1 && !ComfyHasDrawWorkflow(app.comfy)) g_ctl.drawBy = 2;
    ImGui::TextUnformatted("Drawn by");
    const std::string kreaLabel = ComfyHasDrawWorkflow(app.comfy) ? "Krea (" + ComfyDrawWorkflowName(app.comfy) + ")" : std::string("Krea (choose a saved workflow on the Generate page)");
    const char* labels[3] = {"Retro Diffusion alone", kreaLabel.c_str(), "Qwen Image 2.1"};
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##pxdrawer", labels[g_ctl.drawBy])) {
        for (int i : {1, 2, 0}) {
            ImGui::BeginDisabled(i == 1 && !ComfyHasDrawWorkflow(app.comfy));
            if (ImGui::Selectable(labels[i], g_ctl.drawBy == i)) g_ctl.drawBy = i;
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("%s", i == 1 ? "The workflow chosen on the Generate page draws it (without its LoRAs), then it becomes real pixel art. Follows the description best; about 15 s."
                                        : i == 2 ? "Qwen Image 2.1 (the picture-editing model) draws it, then it becomes real pixel art. Follows the description very well."
                                                 : "Retro Diffusion's own models make it directly: the most pixel-art look, but Stable Diffusion 1.5 often misreads the description.");
            }
        }
        ImGui::EndCombo();
    }
    if (g_ctl.drawBy > 0) {
        ImGui::TextUnformatted("Then");
        ImGui::SameLine();
        if (ImGui::RadioButton("Automatic", g_ctl.finish == 2)) g_ctl.finish = 2;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("Sprites, items and tiles are pixelized as drawn; portraits, scenes and other full pictures get the Retro Diffusion touch-up (cleaner pixel shading)");
        ImGui::SameLine();
        if (ImGui::RadioButton("Pixelize", g_ctl.finish == 0)) g_ctl.finish = 0;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("The drawing as it is, cropped to the subject, made into real pixels with the palette below. Keeps the design.");
        ImGui::SameLine();
        if (ImGui::RadioButton("RD touch-up", g_ctl.finish == 1)) g_ctl.finish = 1;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Retro Diffusion redraws the drawing on its pixel grid: cleaner pixel-art shapes and shading, but details and colours can change.");
        if (g_ctl.finish != 0) {
            ImGui::SetNextItemWidth(ui::Dp(180.0f));
            ImGui::SliderFloat("touch-up strength##pxtouch", &g_ctl.touch, 0.2f, 0.6f, "%.2f");
        }
    }
}

// ------------------------------------------------------------------- the three ways in
void DrawCreate(AppState& app) {
    ImGui::TextUnformatted("What should it be?");
    KindChips(g_ctl.kind, g_ctl.w, g_ctl.h, g_ctl.opt);
    ImGui::Spacing();
    ImGui::TextUnformatted("Describe it");
    WrapEdit("##pxprompt", g_ctl.prompt, ui::Dp(80.0f));
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("One short sentence and a few tags works best, e.g. \"a knight in silver armor holding a sword, red cape, contrast lighting\". No quality words.");
    ImGui::PopTextWrapPos();
    ImGui::BeginDisabled(!WriterConfigured() || WriterBusy() || Trim(g_ctl.prompt).empty());
    if (ImGui::SmallButton("Improve it with the prompt writer")) {
        g_ctl.textTarget = 1;
        g_ctl.textFut = std::async(std::launch::async, WriterOnce,
                                   std::string("You write prompts for Retro Diffusion, a pixel-art image model trained on natural language. Rewrite the idea as ONE short descriptive sentence "
                                               "followed by 3 to 6 short tags (details, colours, lighting, view). No quality words such as masterpiece or best quality, do not write "
                                               "'pixel art' (it is added), no quotes. Reply with only the prompt."),
                                   std::string("The idea: ") + g_ctl.prompt + "\nWhat it is: " + kKinds[g_ctl.kind].label, std::wstring(), app.comfy.url,
                                   std::string("The prompt writer is improving the prompt..."));
    }
    ImGui::EndDisabled();
    if (!WriterConfigured() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Set up the prompt writer first (Clef mode tab > Prompt writer)");

    ImGui::Spacing();
    ImGui::Text("Size: %d x %d pixels", g_ctl.w, g_ctl.h);
    const bool square = g_ctl.w == g_ctl.h;
    SizeChips(g_ctl.w, g_ctl.h, square);
    static const char* shapes[] = {"Square", "Landscape 4:3", "Wide 16:9", "Portrait 3:4", "Wide strip 8:3"};
    static const double ratio[] = {1.0, 0.75, 0.5625, 4.0 / 3.0, 0.375};
    int shape = -1;
    for (int i = 0; i < 5; ++i) {
        if (std::abs(static_cast<double>(g_ctl.h) / g_ctl.w - ratio[i]) < 0.06) shape = i;
    }
    ImGui::SetNextItemWidth(ui::Dp(170.0f));
    if (ImGui::BeginCombo("Shape##pxshape", shape >= 0 ? shapes[shape] : "Other")) {
        for (int i = 0; i < 5; ++i) {
            if (ImGui::Selectable(shapes[i], shape == i)) g_ctl.h = std::clamp(static_cast<int>(std::lround(g_ctl.w * ratio[i] / 8.0)) * 8, 8, 160);
        }
        ImGui::EndCombo();
    }
    if (kKinds[g_ctl.kind].tile && g_ctl.w != g_ctl.h) g_ctl.h = g_ctl.w;
    ImGui::Spacing();
    DrawerControls(app);
    ImGui::PushTextWrapPos(0);
    if (g_ctl.drawBy > 0)
        ImGui::TextDisabled("%s draws it, then it becomes %d x %d pixel art%s", DrawerName(g_ctl.drawBy), g_ctl.w, g_ctl.h,
                            kKinds[g_ctl.kind].tile ? "; Retro Diffusion then redraws the seam so the tile repeats" : "");
    else
        ImGui::TextDisabled("Model: %s (picked by size)%s", ModelLine(g_ctl.w, g_ctl.h).c_str(),
                            kKinds[g_ctl.kind].tile ? ". Tiles get a second pass that makes them repeat without a seam." : "");
    ImGui::PopTextWrapPos();
    ImGui::SetNextItemWidth(ui::Dp(140.0f));
    ImGui::SliderInt("versions##pxcount", &g_ctl.count, 1, 8);
}

void DrawConvert(AppState& app) {
    ImGui::TextUnformatted("The picture");
    const float th = ui::Dp(52.0f);
    int removeAt = -1;
    for (size_t i = 0; i < g_ctl.sources.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##src", ImVec2(th, th))) g_ctl.preview = static_cast<int>(i);
        const AppState::LibThumb t = app.LibraryThumb(g_ctl.sources[i]);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (t.srv && t.w > 0 && t.h > 0) {
            ImVec2 uv0(0, 0), uv1(1, 1);
            if (t.w > t.h) { const float m = (1.0f - float(t.h) / t.w) * 0.5f; uv0.x = m; uv1.x = 1 - m; }
            else if (t.h > t.w) { const float m = (1.0f - float(t.w) / t.h) * 0.5f; uv0.y = m; uv1.y = 1 - m; }
            dl->AddImageRounded((ImTextureID)(intptr_t)t.srv, p, ImVec2(p.x + th, p.y + th), uv0, uv1, IM_COL32_WHITE, ui::Dp(5.0f));
        } else {
            dl->AddRectFilled(p, ImVec2(p.x + th, p.y + th), ui::ThumbPlaceholder(), ui::Dp(5.0f));
        }
        if (static_cast<int>(i) == g_ctl.preview) dl->AddRect(p, ImVec2(p.x + th, p.y + th), ui::WithAlpha(ui::kAccent, 1.0f), ui::Dp(5.0f), 0, ui::Dp(2.0f));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nRight click to remove it", ToUtf8(fs::path(g_ctl.sources[i]).filename().wstring()).c_str());
        if (ImGui::BeginPopupContextItem("##srcmenu")) {
            if (ImGui::MenuItem("Remove")) removeAt = static_cast<int>(i);
            ImGui::EndPopup();
        }
        ImGui::PopID();
        if (i + 1 < g_ctl.sources.size() && ImGui::GetContentRegionAvail().x > th * 2.0f) ImGui::SameLine();
    }
    if (removeAt >= 0) {
        g_ctl.sources.erase(g_ctl.sources.begin() + removeAt);
        g_ctl.preview = std::clamp(g_ctl.preview, 0, std::max(0, static_cast<int>(g_ctl.sources.size()) - 1));
    }
    if (g_ctl.sources.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("None yet. Drag pictures from the gallery or the Vision library onto the viewer, or:");
        ImGui::PopTextWrapPos();
    }
    if (ImGui::SmallButton("Add a file...")) AddSource(app, OpenImageFileDialog(GetActiveWindow()));
    ImGui::SameLine();
    const bool haveSel = app.selectedIndex >= 0 && app.selectedIndex < static_cast<int>(app.entries.size());
    ImGui::BeginDisabled(!haveSel);
    if (ImGui::SmallButton("The gallery's picture")) AddSource(app, app.entries[app.selectedIndex].path);
    ImGui::EndDisabled();
    if (!g_ctl.sources.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) g_ctl.sources.clear();
    }

    ImGui::Spacing();
    ImGui::Text("Width: %d pixels (the height follows the picture)", g_ctl.convW);
    ConvWidthChips(ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x);

    ImGui::Spacing();
    ImGui::TextUnformatted("How");
    {
        struct How {
            const char* name;
            int by;
            float d;
            const char* hint;
        };
        static const How hows[] = {
            {"Qwen redraws it", 2, 0.0f, "Qwen Image 2.1 Edit cleans the picture up as pixel art (the same subject, pose and colours; flat shading, clean outlines), then it becomes "
                                         "real pixels. Best for characters, creatures and objects. Photos of whole scenes stay close to a photo: Retro Diffusion "
                                         "restyles those more. About 20 seconds."},
            {"Only pixelize", 0, 0.0f, "No AI: the picture's own colours on a pixel grid, a few seconds. AI-made \"pixel art\" is put on its own grid."},
            {"RD faithful", 1, 0.40f, "Retro Diffusion redraws it lightly: the picture stays very close"},
            {"RD balanced", 1, 0.55f, "Retro Diffusion: clean pixel-art shapes and shading, the same scene and people"},
            {"RD stylized", 1, 0.70f, "Retro Diffusion, freer: more like hand-made pixel art, details may change"}};
        const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        for (int i = 0; i < 5; ++i) {
            const float bw = ImGui::CalcTextSize(hows[i].name).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            if (i > 0 && ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + bw < right) ImGui::SameLine();
            const bool on = hows[i].by == g_ctl.convBy && (hows[i].by != 1 || std::abs(g_ctl.denoise - hows[i].d) < 0.01f);
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ui::kAccent);
            if (ImGui::SmallButton(hows[i].name)) {
                g_ctl.convBy = hows[i].by;
                if (hows[i].by == 1) {
                    g_ctl.denoise = hows[i].d;
                    g_ctl.convW = std::min(g_ctl.convW, 160);
                }
            }
            if (on) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", hows[i].hint);
        }
        if (g_ctl.convBy == 1) {
            ImGui::SetNextItemWidth(ui::Dp(200.0f));
            ImGui::SliderFloat("how much it is redrawn##pxden", &g_ctl.denoise, 0.2f, 0.85f, "%.2f");
        }
    }
    if (g_ctl.convBy != 0) {
        ImGui::Spacing();
        ImGui::TextUnformatted("What it shows");
        WrapEdit("##pxconvprompt", g_ctl.convPrompt, ui::Dp(64.0f));
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Filled in from the picture's description or prompt when it has one. Short is fine: who or what, colours, the place.");
        ImGui::PopTextWrapPos();
        const bool canSee = WriterConfigured() && WriterCanSee();
        ImGui::BeginDisabled(!canSee || WriterBusy() || g_ctl.sources.empty());
        if (ImGui::SmallButton("Let the prompt writer describe it")) {
            g_ctl.textTarget = 2;
            g_ctl.textFut = std::async(std::launch::async, WriterOnce,
                                       std::string("You describe a picture for a pixel-art redraw. Reply with ONE sentence about what it shows (subjects, clothes, colours, setting, view), "
                                                   "then 3 to 5 short tags. No preface, no quotes."),
                                       std::string("Describe the picture."), g_ctl.sources[std::clamp(g_ctl.preview, 0, static_cast<int>(g_ctl.sources.size()) - 1)], app.comfy.url,
                                       std::string("The prompt writer is looking at the picture..."));
        }
        ImGui::EndDisabled();
        if (!canSee && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("The prompt writer needs its mmproj file to look at pictures (Clef mode tab > Prompt writer)");
    }
}

void DrawSetPlanner(AppState& app) {
    ImGui::TextUnformatted("What do you need?");
    WrapEdit("##pxrequest", g_ctl.request, ui::Dp(60.0f));
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("e.g. \"20 items for a dark fantasy dungeon crawler\", \"8 characters for a cyberpunk side-scroller\", \"tiles, platforms and walls for a 16-bit forest platformer\".");
    ImGui::PopTextWrapPos();
    ImGui::TextUnformatted("Game and style (optional)");
    InputLine("##pxstyle", "e.g. 16-bit SNES RPG, top-down view, warm colours", g_ctl.style, -FLT_MIN);
    DrawerControls(app);
    const bool can = WriterConfigured();
    ImGui::BeginDisabled(!can || WriterBusy() || Trim(g_ctl.request).empty());
    if (ImGui::Button(g_ctl.plan.empty() ? "Plan the set" : "Plan it again")) {
        g_ctl.planError.clear();
        g_ctl.planFut = std::async(std::launch::async, PlanSet, g_ctl.request, g_ctl.style, app.comfy.url);
    }
    ImGui::EndDisabled();
    if (!can) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(ui::kWarn, "The prompt writer (an LLM) plans the set: set it up in the Clef mode tab > Prompt writer. You can also add the assets yourself below.");
        ImGui::PopTextWrapPos();
    }
    if (!g_ctl.planError.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(ui::kBad, "%s", g_ctl.planError.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::Spacing();
    int on = 0;
    for (const Planned& p : g_ctl.plan) on += p.on ? 1 : 0;
    ImGui::Text("The plan: %d assets", on);
    if (!g_ctl.plan.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("All")) for (Planned& p : g_ctl.plan) p.on = true;
        ImGui::SameLine();
        if (ImGui::SmallButton("None")) for (Planned& p : g_ctl.plan) p.on = false;
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##plan")) g_ctl.plan.clear();
    }
    int removeAt = -1;
    for (size_t i = 0; i < g_ctl.plan.size(); ++i) {
        Planned& p = g_ctl.plan[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::Separator();
        ImGui::Checkbox("##on", &p.on);
        ImGui::SameLine();
        InputLine("##name", "name", p.name, std::max(ui::Dp(80.0f), ImGui::GetContentRegionAvail().x - ui::Dp(170.0f)));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ui::Dp(110.0f));
        if (ImGui::BeginCombo("##kind", kKinds[p.kind].label)) {
            for (int k = 1; k < kKindCount; ++k) {
                if (ImGui::Selectable(kKinds[k].label, p.kind == k)) {
                    p.kind = k;
                    p.w = kKinds[k].w;
                    p.h = kKinds[k].h;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) removeAt = static_cast<int>(i);
        InputLine("##prompt", "what it looks like", p.prompt, -ui::Dp(96.0f));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ui::Dp(86.0f));
        int wh[2] = {p.w, p.h};
        if (ImGui::DragInt2("##size", wh, 8.0f, 16, 160)) {
            p.w = SnapSize(wh[0]);
            p.h = kKinds[p.kind].tile ? p.w : SnapSize(wh[1]);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Width and height in pixels");
        ImGui::PopID();
    }
    if (removeAt >= 0) g_ctl.plan.erase(g_ctl.plan.begin() + removeAt);
    if (ImGui::SmallButton("+ Add an asset")) {
        Planned p;
        p.kind = 2;
        g_ctl.plan.push_back(p);
    }
}

// A picture to convert with the Convert settings: a new result, started at once. Its id (0: the picture could not be read); `sent` when it went.
int MakeConvert(AppState& app, const std::wstring& src, const std::string& desc, long long now, bool& sent, int width = 0, bool quiet = false) {
    sent = false;
    std::vector<uint8_t> probe;
    int sw = 0, sh = 0;
    if (!PixelLoadRGBA(src, probe, sw, sh)) return 0;
    PxRecord r;
    r.id = g_nextId++;
    r.mode = g_ctl.convBy != 0 ? 1 : 2;
    r.drawBy = g_ctl.convBy == 2 ? 2 : 0;
    r.src = src;
    if (width <= 0) width = g_ctl.convW;
    r.w = g_ctl.convBy == 1 ? std::min(width, 160) : width;
    r.h = std::clamp(static_cast<int>(std::lround(static_cast<double>(sh) * r.w / std::max(1, sw))), 8, 1024);
    r.label = ToUtf8(fs::path(src).stem().wstring()) + " (" + std::to_string(r.w) + ", " + (g_ctl.convBy == 2 ? "Qwen" : g_ctl.convBy == 1 ? "RD" : "no AI") + ")";
    r.desc = desc;
    r.prompt = FullPrompt(0, r.desc, std::string());
    r.denoise = g_ctl.denoise;
    r.opt = g_ctl.convOpt;
    r.when = now;
    r.flashUntil = ImGui::GetTime() + 3.0;
    g_recs.push_back(std::move(r));
    PxRecord& made = g_recs.back();
    sent = StartConvert(app, made);
    if (quiet) {
        if (NeedsCutout(made)) StartCutout(app, made);
        return made.id;
    }
    if (!made.error.empty()) Notify("\"" + made.label + "\" failed: " + made.error, made.id, true);
    else if (made.activity > 0) Notify("Sent to ComfyUI: " + made.label + (made.drawBy == 2 ? " (Qwen, about 20 s)" : " (Retro Diffusion)"), made.id);
    else Notify("Pixel art ready: " + made.label, made.id);
    if (NeedsCutout(made)) StartCutout(app, made);
    return made.id;
}

// The big button: what it makes, for each way in.
void DrawMakeButton(AppState& app) {
    const bool ready = g_ctl.mode == 0 ? !Trim(g_ctl.prompt).empty() : g_ctl.mode == 1 ? !g_ctl.sources.empty() : std::any_of(g_ctl.plan.begin(), g_ctl.plan.end(), [](const Planned& p) { return p.on && !Trim(p.prompt).empty(); });
    char label[64];
    if (g_ctl.mode == 0) snprintf(label, sizeof(label), g_ctl.count > 1 ? "Make %d versions" : "Make pixel art", g_ctl.count);
    else if (g_ctl.mode == 1) snprintf(label, sizeof(label), g_ctl.sources.size() > 1 ? "Make %d into pixel art" : "Make it pixel art", static_cast<int>(g_ctl.sources.size()));
    else {
        int n = 0;
        for (const Planned& p : g_ctl.plan) n += p.on && !Trim(p.prompt).empty() ? 1 : 0;
        snprintf(label, sizeof(label), "Make the set (%d)", n);
    }
    ImGui::BeginDisabled(!ready);
    const bool ctrlEnter = ready && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Enter);
    const bool pressed = ui::PrimaryButton(label, ImVec2(ui::Dp(190.0f), 0)) || ctrlEnter;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Ctrl+Enter");
    if (!pressed) {
        ImGui::EndDisabled();
        return;
    }
    ImGui::EndDisabled();
    EnsureComfyReady(app);
    const long long now = static_cast<long long>(std::time(nullptr));
    int sent = 0, lastId = -1;
    if (g_ctl.mode == 0) {
        const std::string desc = Trim(g_ctl.prompt);
        for (int i = 0; i < g_ctl.count; ++i) {
            PxRecord r;
            r.id = g_nextId++;
            r.mode = 0;
            r.kind = g_ctl.kind;
            r.desc = desc;
            r.label = desc.substr(0, std::min<size_t>(desc.size(), 36));
            r.prompt = FullPrompt(g_ctl.kind, desc, std::string());
            r.drawBy = std::max(0, g_ctl.drawBy);
            r.finish = g_ctl.finish;
            r.touch = g_ctl.touch;
            r.w = g_ctl.w;
            r.h = g_ctl.h;
            r.tile = kKinds[g_ctl.kind].tile;
            r.opt = g_ctl.opt;
            r.when = now;
            g_recs.push_back(std::move(r));
            if (StartCreate(app, g_recs.back())) ++sent;
            lastId = g_recs.back().id;
        }
        g_selSet = -1;
        g_sel = lastId;
    } else if (g_ctl.mode == 1) {
        for (const std::wstring& src : g_ctl.sources) {
            const std::string desc = _wcsicmp(g_ctl.convPromptFor.c_str(), src.c_str()) == 0 || g_ctl.sources.size() == 1 ? Trim(g_ctl.convPrompt) : KnownDescription(app, src);
            bool ok = false;
            const int id = MakeConvert(app, src, desc, now, ok);
            if (id > 0) lastId = id;
            if (ok) ++sent;
        }
        g_selSet = -1;
        g_sel = lastId;
    } else {
        PxSet s;
        s.id = g_nextSet++;
        s.request = Trim(g_ctl.request);
        s.style = Trim(g_ctl.style);
        s.opt = g_ctl.opt;
        s.opt.colors = std::max(s.opt.colors, 32); // a set of different things needs more colours than one picture
        s.opt.transparent = true; // per picture the kind decides; see below
        s.when = now;
        // a set mixing sprites and tiles: the see-through background follows the kind of most of them
        int cut = 0, total = 0;
        for (const Planned& p : g_ctl.plan) {
            if (!p.on || Trim(p.prompt).empty()) continue;
            ++total;
            cut += kKinds[p.kind].transparent ? 1 : 0;
        }
        s.opt.transparent = cut * 2 >= total;
        s.shared = true;
        g_sets.push_back(s);
        for (const Planned& p : g_ctl.plan) {
            if (!p.on || Trim(p.prompt).empty()) continue;
            PxRecord r;
            r.id = g_nextId++;
            r.mode = 0;
            r.kind = p.kind;
            r.desc = Trim(p.prompt);
            r.label = Trim(p.name).empty() ? r.desc.substr(0, 24) : Trim(p.name);
            r.prompt = FullPrompt(p.kind, r.desc, s.style);
            r.style = s.style;
            r.drawBy = std::max(0, g_ctl.drawBy);
            r.finish = g_ctl.finish;
            r.touch = g_ctl.touch;
            r.w = p.w;
            r.h = kKinds[p.kind].tile ? p.w : p.h;
            r.tile = kKinds[p.kind].tile;
            r.opt = s.opt;
            r.opt.transparent = kKinds[p.kind].transparent;
            r.opt.outline = kKinds[p.kind].transparent && std::string(kKinds[p.kind].id) != "effect";
            r.setId = s.id;
            r.when = now;
            g_recs.push_back(std::move(r));
            if (StartCreate(app, g_recs.back())) ++sent;
        }
        g_sel = -1;
        g_selSet = s.id;
    }
    g_view.zoom = 0;
    g_view.pan = ImVec2(0, 0);
    Save();
    if (g_ctl.mode == 1 && g_ctl.convBy == 0) app.SetStatus("Pixelized " + std::to_string(sent) + (sent == 1 ? " picture" : " pictures") + " (no AI)");
    else if (sent > 0) app.SetStatus("Sent " + std::to_string(sent) + (sent == 1 ? " picture" : " pictures") + " to ComfyUI (Retro Diffusion): see the Pixel art tab");
}

void DrawSetupBox(AppState& app) {
    if (!g_models.known || !g_models.reachable || HaveModels()) return;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.35f, 0.25f, 0.05f, 0.35f));
    ImGui::BeginChild("##pxsetup", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(ui::kWarn, "ComfyUI does not have the Retro Diffusion models yet.");
    const std::wstring dir = PixelModelsDir(app.comfy);
    ImGui::TextDisabled("They are in %s. One click links that folder into ComfyUI's checkpoints (no copy). When this app starts ComfyUI itself, the folder is added on its own.",
                        ToUtf8(dir).c_str());
    ImGui::PopTextWrapPos();
    if (ImGui::SmallButton("Link them into ComfyUI")) {
        std::string msg;
        ComfyLinkPixelModels(app.comfy, msg);
        g_models.linkMessage = msg;
        app.SetStatus(msg);
        CheckModels(app, true);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Choose the folder...")) {
        const std::wstring picked = OpenFolderDialog(GetActiveWindow());
        if (!picked.empty()) {
            app.comfy.pixelModels = picked;
            SaveComfySettings(app.comfy);
        }
    }
    if (!g_models.linkMessage.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", g_models.linkMessage.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------
void OpenPixelArt(AppState& app, const std::vector<std::wstring>& paths) {
    Load();
    g_ctl.mode = 1;
    g_ctl.sources.clear();
    g_ctl.convPrompt.clear();
    for (const std::wstring& p : paths) AddSource(app, p);
    app.requestTab = 9;
}

void QuickPixelArt(AppState& app, const std::vector<std::wstring>& paths) {
    Load();
    EnsureComfyReady(app);
    g_ctl.mode = 1; // the Convert panel shows what was sent
    g_ctl.sources = paths;
    g_ctl.preview = 0;
    const long long now = static_cast<long long>(std::time(nullptr));
    int made = 0, lastId = 0;
    for (const std::wstring& p : paths) {
        bool sent = false;
        const int id = MakeConvert(app, p, KnownDescription(app, p), now, sent);
        if (id > 0) {
            ++made;
            lastId = id;
        }
    }
    if (lastId > 0) {
        g_sel = lastId;
        g_selSet = -1;
    }
    Save();
    if (made == 0) {
        Notify("Could not read the picture", 0, true);
        app.SetStatus("Could not read the picture");
    }
}

// ---- pixel art mode: jobs whose pictures become pixel art when they are done
struct PxWatch {
    int activity = 0;
    std::string group, label;
    bool notify = false, done = false;
    std::vector<int> recs;
};
std::vector<PxWatch> g_watches;

int SetFor(const std::string& group) { // the set of the Pixel art tab collecting this mode's pictures (each keeps its own palette)
    for (const PxSet& s : g_sets) {
        if (s.request == group) return s.id;
    }
    PxSet s;
    s.id = g_nextSet++;
    s.request = group;
    s.shared = false;
    s.opt = g_ctl.convOpt;
    s.when = static_cast<long long>(std::time(nullptr));
    g_sets.push_back(s);
    return s.id;
}

void PumpWatches(AppState& app) {
    bool any = false;
    for (const PxWatch& w : g_watches) any = any || !w.done;
    if (!any || !app.comfySvc) return;
    const std::vector<ComfyActivity> acts = app.comfySvc->Activity();
    for (PxWatch& w : g_watches) {
        if (w.done) continue;
        const ComfyActivity* a = nullptr;
        for (const ComfyActivity& x : acts) {
            if (x.id == w.activity) a = &x;
        }
        if (!a || a->state == ComfyState::Failed) {
            w.done = true;
            continue;
        }
        if (a->state != ComfyState::Done) continue;
        w.done = true;
        const long long now = static_cast<long long>(std::time(nullptr));
        const int setId = SetFor(w.group);
        for (const ComfyResult& f : a->files) {
            if (f.localPath.empty() || f.video) continue;
            std::vector<uint8_t> probe;
            int sw = 0, sh = 0;
            if (!PixelLoadRGBA(f.localPath, probe, sw, sh)) continue;
            bool sent = false;
            const int id = MakeConvert(app, f.localPath, std::string(), now, sent, g_ctl.modeW > 0 ? g_ctl.modeW : SmartWidth(sw, sh), true);
            if (PxRecord* r = Find(id)) {
                r->setId = setId;
                if (!w.label.empty()) r->label = w.label + " (" + std::to_string(r->w) + ", " + (r->drawBy == 2 ? "Qwen" : r->mode == 1 ? "RD" : "no AI") + ")";
                w.recs.push_back(id);
                if (r->activity <= 0 && !r->pixel.empty() && w.notify) Notify("Pixel art ready: " + r->label, r->id);
                else if (w.notify) Notify("Making pixel art of " + r->label, r->id);
            }
        }
        Save();
    }
}

// The small "Pixel art" panel (in a popup or a menu): a width that suits the picture, how, see-through, Make.
void PixelArtQuickContents(AppState& app, const std::vector<std::wstring>& paths) {
    Load();
    static std::wstring smartFor; // the width is chosen for each new picture
    const std::wstring key = paths.empty() ? std::wstring() : paths.front();
    if (key != smartFor) {
        smartFor = key;
        std::vector<uint8_t> probe;
        int sw = 0, sh = 0;
        if (!key.empty() && PixelLoadRGBA(key, probe, sw, sh)) g_ctl.convW = g_ctl.convBy == 1 ? std::min(160, SmartWidth(sw, sh)) : SmartWidth(sw, sh);
    }
    ImGui::PushFont(ui::BoldFont());
    ImGui::TextUnformatted(paths.size() > 1 ? ("Pixel art from " + std::to_string(paths.size()) + " pictures").c_str() : "Pixel art from this picture");
    ImGui::PopFont();
    const float right = ImGui::GetCursorScreenPos().x + ui::Dp(330.0f);
    ImGui::TextDisabled("Width in pixels (the height follows)");
    ConvWidthChips(right);
    ImGui::TextDisabled("How");
    struct How {
        const char* name;
        int by;
        const char* hint;
    };
    static const How hows[] = {{"Only pixelize", 0, "No AI: the picture's own colours on a pixel grid, at once"},
                               {"Qwen redraws it", 2, "Qwen Image 2.1 Edit cleans it up as pixel art (flat shading, outlines), about 20 seconds. Best for characters and objects."},
                               {"Retro Diffusion", 1, "Retro Diffusion redraws it (up to 160 pixels): restyles whole scenes the most"}};
    for (int i = 0; i < 3; ++i) {
        if (i) ImGui::SameLine();
        const bool on = g_ctl.convBy == hows[i].by;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ui::kAccent);
        if (ImGui::SmallButton(hows[i].name)) {
            g_ctl.convBy = hows[i].by;
            if (hows[i].by == 1) {
                g_ctl.denoise = 0.55f;
                g_ctl.convW = std::min(g_ctl.convW, 160);
            }
        }
        if (on) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", hows[i].hint);
    }
    ImGui::Checkbox("See-through background", &g_ctl.convOpt.transparent);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("A plain background is cut away; in a photo SAM 3 cuts out what you name below");
    if (g_ctl.convOpt.transparent) {
        ImGui::SameLine();
        ImGui::Checkbox("Outline", &g_ctl.convOpt.outline);
        ImGui::SameLine();
        ImGui::Checkbox("Smart cut-out", &g_ctl.smartKeep);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("The vision model looks at the picture and names its main subjects and what they hold (no people in the background)");
        if (!g_ctl.smartKeep) InputLine("##pxquickkeep", "keep, e.g. person, cat", g_ctl.keep, ui::Dp(330.0f));
    }
    ImGui::Spacing();
    if (ui::PrimaryButton("Make it", ImVec2(ui::Dp(120.0f), 0))) {
        QuickPixelArt(app, paths);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("More options...")) {
        OpenPixelArt(app, paths);
        ImGui::CloseCurrentPopup();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Open the Pixel art tab with this picture (palette, colours, what it shows...)");
}

void PixelArtButton(AppState& app, const std::vector<std::wstring>& paths, const char* popupId) {
    if (ImGui::SmallButton("Pixel art...")) ImGui::OpenPopup(popupId);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Turn it into real pixel art");
    if (ImGui::BeginPopup(popupId)) {
        PixelArtQuickContents(app, paths);
        ImGui::EndPopup();
    }
}

// Cut-outs for other features (the designer's pictures to place): SAM 3 with the Keep words, the main pieces kept, the picture with that alpha, cropped.
std::map<int, std::wstring> g_cutJobs; // ComfyUI job -> its picture

int PixelCutoutPicture(AppState& app, const std::wstring& src) {
    Load();
    EnsureComfyReady(app);
    ComfyJob job;
    job.cutout = 1;
    job.images = {src};
    job.prompt = DefaultKeep();
    job.times = 1;
    job.label = "Cut out " + ToUtf8(fs::path(src).filename().wstring());
    const int id = SendPixel(app, job);
    if (id > 0) g_cutJobs[id] = src;
    return id;
}

int PixelCutoutResult(AppState& app, int activity, std::wstring& cutPng) {
    auto it = g_cutJobs.find(activity);
    if (it == g_cutJobs.end() || !app.comfySvc) return -1;
    const ComfyActivity* a = nullptr;
    const std::vector<ComfyActivity> acts = app.comfySvc->Activity();
    for (const ComfyActivity& x : acts) {
        if (x.id == activity) a = &x;
    }
    if (!a || a->state == ComfyState::Failed) {
        g_cutJobs.erase(it);
        return -1;
    }
    if (a->state != ComfyState::Done) return 0;
    const std::wstring src = it->second;
    g_cutJobs.erase(it);
    std::vector<std::wstring> files;
    for (const ComfyResult& f : a->files) {
        if (!f.localPath.empty() && !f.video) files.push_back(f.localPath);
    }
    const std::wstring maskPath = WorkDir() + L"\\cutmask_" + std::to_wstring(activity) + L".png";
    std::vector<uint8_t> m, rgba;
    int mw = 0, mh = 0, w = 0, h = 0;
    if (files.empty() || !CombineMasks(files, maskPath) || !PixelLoadRGBA(maskPath, m, mw, mh) || !PixelLoadRGBA(src, rgba, w, h) || mw != w || mh != h) return -1;
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            const uint8_t keep = m[i * 4];
            rgba[i * 4 + 3] = static_cast<uint8_t>(rgba[i * 4 + 3] * keep / 255);
            if (keep >= 128) {
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                x1 = std::max(x1, x);
                y1 = std::max(y1, y);
            }
        }
    }
    if (x1 < x0) return -1;
    const int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
    std::vector<uint8_t> crop(static_cast<size_t>(cw) * ch * 4);
    for (int y = 0; y < ch; ++y) std::memcpy(crop.data() + static_cast<size_t>(y) * cw * 4, rgba.data() + (static_cast<size_t>(y + y0) * w + x0) * 4, static_cast<size_t>(cw) * 4);
    cutPng = WorkDir() + L"\\cut_" + std::to_wstring(activity) + L"_" + fs::path(src).stem().wstring() + L".png";
    return PixelSavePng(cutPng, crop.data(), cw, ch) ? 1 : -1;
}

void PixelArtAfter(AppState& app, int activity, const std::string& group, const std::string& label, bool notify) {
    Load();
    if (activity <= 0) return;
    EnsureComfyReady(app);
    PxWatch w;
    w.activity = activity;
    w.group = group;
    w.label = label;
    w.notify = notify;
    g_watches.push_back(w);
}

bool PixelArtThumb(AppState& app, int activity, float size) {
    PxRecord* r = nullptr;
    bool pending = false;
    for (const PxWatch& w : g_watches) {
        if (w.activity != activity) continue;
        pending = !w.done;
        if (!w.recs.empty()) r = Find(w.recs.front());
    }
    if (!r && !pending) return false;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(activity);
    const bool clicked = ImGui::InvisibleButton("##pxmodethumb", ImVec2(size, size));
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), IM_COL32(14, 16, 20, 255), ui::Dp(5.0f));
    if (r && !r->pixel.empty()) {
        Thumb(app, *r, p, size);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Pixel art (%d x %d): click to open it in the Pixel art tab", r->w, r->h);
        if (clicked) {
            g_sel = r->id;
            g_selSet = -1;
            g_view.zoom = 0;
            g_view.pan = ImVec2(0, 0);
            app.requestTab = 9;
        }
    } else {
        const char* t = r && !r->error.empty() ? "Pixel art failed" : "Pixel art...";
        const ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2(p.x + (size - ts.x) * 0.5f, p.y + (size - ts.y) * 0.5f), IM_COL32(220, 220, 230, 220), t);
    }
    return true;
}

std::string PixelArtLookClause() {
    return "Draw it in a 16-bit pixel art style: bold simple shapes, flat cel shading with few tones, crisp dark outlines, a limited colour palette, no fine texture.";
}

void PixelArtModeOptions(const char* id, bool& look) {
    Load();
    ImGui::PushID(id);
    ImGui::Indent(ui::Dp(18.0f));
    ImGui::TextDisabled("Then");
    struct How {
        const char* name;
        int by;
        const char* hint;
    };
    static const How hows[] = {{"Only pixelize", 0, "No AI: each picture's own colours on a pixel grid, at once"},
                               {"Qwen redraws it", 2, "Qwen Image 2.1 Edit cleans it up as pixel art, about 20 seconds more per picture"},
                               {"Retro Diffusion", 1, "Retro Diffusion redraws it (up to 160 pixels)"}};
    for (int i = 0; i < 3; ++i) {
        ImGui::SameLine();
        const bool on = g_ctl.convBy == hows[i].by;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ui::kAccent);
        if (ImGui::SmallButton(hows[i].name)) {
            g_ctl.convBy = hows[i].by;
            if (hows[i].by == 1) g_ctl.denoise = 0.55f;
        }
        if (on) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", hows[i].hint);
    }
    ImGui::TextDisabled("Width");
    static const int widths[] = {0, 64, 96, 128, 160, 192, 256, 320, 384, 512};
    for (int i = 0; i < static_cast<int>(sizeof(widths) / sizeof(widths[0])); ++i) {
        ImGui::SameLine();
        char lbl[16];
        if (widths[i] == 0) snprintf(lbl, sizeof(lbl), "Auto");
        else snprintf(lbl, sizeof(lbl), "%d", widths[i]);
        const bool tooBig = g_ctl.convBy == 1 && widths[i] > 160;
        const bool on = g_ctl.modeW == widths[i];
        ImGui::BeginDisabled(tooBig);
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ui::kAccent);
        if (ImGui::SmallButton(lbl)) g_ctl.modeW = widths[i];
        if (on) ImGui::PopStyleColor();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", widths[i] == 0 ? "A width that suits each picture (about 240 pixels on its long side)"
                                    : tooBig       ? "Retro Diffusion's models only work up to 160 pixels"
                                                   : "Every picture this many pixels wide (the height follows the picture)");
    }
    ImGui::SetNextItemWidth(ui::Dp(180.0f));
    ImGui::SliderInt("colours", &g_ctl.convOpt.colors, 2, 64);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("How many colours each picture's palette has (made from the picture). It can be changed afterwards in the Pixel art tab.");
    ImGui::Checkbox("Ask for a pixel-art look", &look);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("The edit (or prompt) itself asks for 16-bit pixel art (bold shapes, flat shading, outlines), so the pixels look hand-made.\nOff: the picture is made as usual and then pixelized.");
    ImGui::SameLine();
    ImGui::Checkbox("See-through", &g_ctl.convOpt.transparent);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip(g_ctl.smartKeep ? "Cut the main characters out (the vision model names them, SAM 3 cuts them out)" : "Cut out (SAM 3 keeps: %s)", g_ctl.keep.c_str());
    ImGui::TextDisabled("The results are collected in the Pixel art tab.");
    ImGui::Unindent(ui::Dp(18.0f));
    ImGui::PopID();
}

void DrawPixelArtNotices(AppState&) {} // the toasts (notify.h) show them now

std::string PixelTabLabel(AppState& app) {
    Load();
    int n = 0;
    for (const PxRecord& r : g_recs) n += r.activity > 0 ? 1 : 0;
    return (n > 0 ? "Pixel art (" + std::to_string(n) + ")" : std::string("Pixel art")) + "###pixeltab";
}

void PumpPixelArt(AppState& app) {
    Load();
    PumpWatches(app);
    PumpKeepQueue(app);
    // between frames: what the last frame drew is on screen, its old textures can go
    for (LoadedImage& t : g_texGrave) ReleaseImage(t);
    g_texGrave.clear();
    if ((!g_pendingPng.empty() || g_pendingSave) && !ImGui::IsAnyItemActive()) {
        for (auto& [id, p] : g_pendingPng) PixelSavePng(p.path, p.rgba.data(), p.w, p.h);
        g_pendingPng.clear();
        if (g_pendingSave) Save();
        g_pendingSave = false;
    }
    static bool testDone = false; // for tests, once at start: MV_PIXEL_TEST=animate:<result id>:<action> or quick:<picture path>
    if (!testDone) {
        testDone = true;
        wchar_t buf[1024] = {};
        const DWORD n = GetEnvironmentVariableW(L"MV_PIXEL_TEST", buf, 1024);
        const std::string v = n > 0 && n < 1024 ? ToUtf8(std::wstring(buf, n)) : std::string();
        if (v.rfind("animate:", 0) == 0) {
            const std::string rest = v.substr(8);
            const size_t colon = rest.find(':');
            PxRecord* r = Find(std::atoi(rest.substr(0, colon).c_str()));
            if (r && colon != std::string::npos) {
                const int action = ActionIndex(rest.substr(colon + 1));
                MakeAnimation(app, *r, action, kActions[action].frames, "", false);
                app.requestTab = 9;
            }
        } else if (v.rfind("editpx:", 0) == 0) { // a picture edit in pixel art mode (as the Vision board sends it)
            EditContext ctx;
            ctx.pixelArt = true;
            ctx.pixelGroup = "Vision board";
            SendPictureEdit(app, {ToWide(v.substr(7))}, "Make the person wave at the viewer with one hand, smiling. " + PixelArtLookClause(), "Test edit (pixel art)", 1, &ctx);
        } else if (v.rfind("quickcut:", 0) == 0) { // no AI, see-through with an outline, a width that suits the picture
            const std::wstring p = ToWide(v.substr(9));
            std::vector<uint8_t> probe;
            int sw = 0, sh = 0;
            if (PixelLoadRGBA(p, probe, sw, sh)) g_ctl.convW = SmartWidth(sw, sh);
            g_ctl.convBy = 0;
            g_ctl.convOpt.transparent = true;
            g_ctl.convOpt.outline = true;
            g_ctl.smartKeep = false; // tests: the plain words (no vision model)
            QuickPixelArt(app, {p});
            app.requestTab = 9;
        } else if (v.rfind("quick:", 0) == 0) {
            QuickPixelArt(app, {ToWide(v.substr(6))});
            app.requestTab = 9;
        }
    }
    Sync(app);
    PumpWriter(app);
}

bool PixelArtAnimating() { return g_playing; }

void DrawPixelTab(AppState& app, float height) {
    Load();
    g_playing = false;
    EnsureComfyReady(app);
    CheckModels(app);
    if (!ImGui::BeginTable("px_layout", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings, ImVec2(0, height))) return;
    ImGui::TableSetupColumn("List", ImGuiTableColumnFlags_WidthStretch, 0.17f);
    ImGui::TableSetupColumn("Picture", ImGuiTableColumnFlags_WidthStretch, 0.50f);
    ImGui::TableSetupColumn("Controls", ImGuiTableColumnFlags_WidthStretch, 0.33f);
    ImGui::TableNextRow(0, height);

    // ---- what was made, newest first (a set is one row)
    ImGui::TableSetColumnIndex(0);
    ImGui::BeginChild("px_list", ImVec2(0, height), true);
    if (const int cleanup = DrawListCleanup("pxclean", !g_recs.empty())) {
        const size_t before = g_recs.size();
        auto drop = [&](const PxRecord& r) {
            if (r.activity > 0 || r.maskAct != 0) return false; // (still being made)
            return cleanup == 2 || !FileStillThere(r.pixel);
        };
        for (PxRecord& r : g_recs) {
            if (!drop(r)) continue;
            ReleaseTex(r);
            g_cellCache.erase(r.id);
        }
        g_recs.erase(std::remove_if(g_recs.begin(), g_recs.end(), drop), g_recs.end());
        g_sets.erase(std::remove_if(g_sets.begin(), g_sets.end(), [](const PxSet& s) { return Members(s.id).empty(); }), g_sets.end());
        if (!Find(g_sel)) g_sel = -1;
        if (!FindSet(g_selSet)) g_selSet = -1;
        Save();
        const size_t n = before - g_recs.size();
        app.SetStatus(n == 0 ? std::string("Every pixel art on the list is still there") : "Took " + std::to_string(n) + " off the list (no files deleted)");
    }
    if (g_recs.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Pixel art you make shows up here, and is kept between sessions.");
        ImGui::PopTextWrapPos();
    }
    int removeId = -1, removeSet = -1;
    std::set<int> shownSets;
    for (size_t k = g_recs.size(); k-- > 0;) {
        PxRecord& r = g_recs[k];
        const bool isSet = r.setId > 0;
        if (isSet && shownSets.count(r.setId)) continue;
        if (isSet) shownSets.insert(r.setId);
        PxSet* s = isSet ? FindSet(r.setId) : nullptr;
        ImGui::PushID(isSet ? -r.setId : r.id);
        const float row = ui::Dp(60.0f);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        if (ImGui::InvisibleButton("##row", ImVec2(w, row))) {
            if (isSet) {
                g_selSet = r.setId;
                g_sel = -1;
            } else {
                g_sel = r.id;
                g_selSet = -1;
            }
            g_view.zoom = 0;
            g_view.pan = ImVec2(0, 0);
        }
        const bool hov = ImGui::IsItemHovered();
        if (ImGui::BeginPopupContextItem("##rowmenu")) {
            if (ImGui::MenuItem(isSet ? "Remove the set from this list" : "Remove from this list")) {
                if (isSet) removeSet = r.setId;
                else removeId = r.id;
            }
            ImGui::EndPopup();
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const bool sel = isSet ? g_selSet == r.setId || (g_sel > 0 && Find(g_sel) && Find(g_sel)->setId == r.setId) : g_sel == r.id;
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + row), sel ? ui::WithAlpha(ui::kAccent, 0.25f) : (hov ? IM_COL32(255, 255, 255, 18) : IM_COL32(255, 255, 255, 8)), ui::Dp(6.0f));
        if (sel) dl->AddRect(pos, ImVec2(pos.x + w, pos.y + row), ui::WithAlpha(ui::kAccent, 0.9f), ui::Dp(6.0f), 0, ui::Dp(1.5f));
        if (!isSet && r.flashUntil > ImGui::GetTime()) { // just made: it pulses for a moment
            const float t = static_cast<float>(r.flashUntil - ImGui::GetTime());
            dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + row), ui::WithAlpha(ui::kAccent, 0.18f + 0.18f * std::sin(t * 6.0f)), ui::Dp(6.0f));
            g_playing = true; // keep drawing while it pulses
        }
        PxRecord* face = &r;
        std::string line1, line2;
        if (isSet) {
            std::vector<PxRecord*> m = Members(r.setId);
            int done = 0, working = 0;
            for (PxRecord* x : m) {
                if (x->activity > 0) ++working;
                else if (!x->pixel.empty() && x->error.empty()) ++done;
                if (face->pixel.empty() && !x->pixel.empty()) face = x;
            }
            if (!m.empty() && !m.front()->pixel.empty()) face = m.front();
            line1 = "Set: " + (s ? s->request : std::string("?"));
            line2 = std::to_string(done) + " of " + std::to_string(m.size()) + (working ? " (making...)" : "");
        } else {
            line1 = r.label;
            char sub[96];
            if (r.mode == 3) snprintf(sub, sizeof(sub), "%d x %d, %d frames", r.w, r.h, r.frames);
            else snprintf(sub, sizeof(sub), "%d x %d, %s", r.w, r.h, r.mode == 0 ? kKinds[r.kind].label : r.mode == 1 ? "redrawn picture" : "pixelized picture");
            line2 = r.activity > 0 ? (r.state.empty() ? std::string("Working...") : r.state + "...") : !r.error.empty() ? std::string("Failed") : std::string(sub);
        }
        Thumb(app, *face, ImVec2(pos.x + 4, pos.y + 4), row - 8);
        dl->PushClipRect(ImVec2(pos.x + row, pos.y), ImVec2(pos.x + w - 4, pos.y + row), true);
        dl->AddText(ImVec2(pos.x + row + 2, pos.y + 6), ImGui::GetColorU32(ImGuiCol_Text), line1.c_str());
        dl->AddText(ImVec2(pos.x + row + 2, pos.y + 6 + ImGui::GetFontSize() + 3), !isSet && !r.error.empty() ? ImGui::GetColorU32(ui::kBad) : ImGui::GetColorU32(ui::kTextDim), line2.c_str());
        dl->PopClipRect();
        ImGui::PopID();
    }
    if (removeId >= 0 || removeSet >= 0) {
        for (PxRecord& r : g_recs) {
            if (r.id == removeId || (removeSet > 0 && r.setId == removeSet)) {
                ReleaseTex(r);
                g_cellCache.erase(r.id);
            }
        }
        g_recs.erase(std::remove_if(g_recs.begin(), g_recs.end(), [&](const PxRecord& r) { return r.id == removeId || (removeSet > 0 && r.setId == removeSet); }), g_recs.end());
        if (removeSet > 0) g_sets.erase(std::remove_if(g_sets.begin(), g_sets.end(), [&](const PxSet& s) { return s.id == removeSet; }), g_sets.end());
        if (!Find(g_sel)) g_sel = -1;
        if (!FindSet(g_selSet)) g_selSet = -1;
        Save();
    }
    ImGui::EndChild();

    PxRecord* rec = Find(g_sel);
    PxSet* set = FindSet(g_selSet);

    // ---- the picture (or the set)
    ImGui::TableSetColumnIndex(1);
    ImGui::BeginChild("px_view", ImVec2(0, height), true, ImGuiWindowFlags_NoScrollbar);
    if (set) {
        ImGui::PushFont(ui::BoldFont());
        ImGui::TextUnformatted(("Set: " + set->request).c_str());
        ImGui::PopFont();
        DrawSetGrid(app, *set, height);
    } else if (rec) {
        if (rec->setId > 0) {
            if (ImGui::SmallButton("< Back to the set")) {
                g_selSet = rec->setId;
                g_sel = -1;
            }
            ImGui::SameLine();
        }
        ImGui::PushFont(ui::BoldFont());
        ImGui::TextUnformatted(rec->label.c_str());
        ImGui::PopFont();
        if (g_view.lastRec != rec->id) {
            g_view.lastRec = rec->id;
            g_view.shown = 0;
        }
        if (ImGui::RadioButton("Pixel art", g_view.shown == 0)) g_view.shown = 0;
        ImGui::SameLine();
        const bool drawn = rec->mode == 0 && rec->drawBy > 0 && !rec->src.empty();
        if (ImGui::RadioButton(rec->mode == 3 ? "What Qwen drew" : rec->mode != 0 ? "The picture" : drawn ? "The drawing" : "What ComfyUI drew", g_view.shown == 1)) g_view.shown = 1;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("%s", rec->mode == 3 ? "Qwen Image 2.1 Edit's sprite sheet, before the frames were cut apart and made pixel art"
                                    : rec->mode != 0 ? "The picture it was made from"
                                    : drawn       ? (std::string(DrawerName(rec->drawBy)) + "'s drawing, before it became pixel art").c_str()
                                                  : "Retro Diffusion's own picture: 8 x 8 soft cells for every pixel, before it became real pixel art");
        ImGui::SameLine();
        ImGui::Checkbox("Grid", &g_view.grid);
        if (rec->tile) {
            ImGui::SameLine();
            ImGui::Checkbox("Repeat 3 x 3", &g_view.tiled);
        }
        if (rec->frames > 1) {
            ui::SameLineIfFits("Play");
            ImGui::Checkbox("Play", &g_view.play);
            ImGui::SameLine();
            ImGui::Checkbox("All frames", &g_view.sheet);
            ImGui::SameLine();
            ImGui::BeginDisabled(g_view.play || g_view.sheet);
            if (ImGui::ArrowButton("##pxprev", ImGuiDir_Left)) g_view.frame = (g_view.frame + rec->frames - 1) % rec->frames;
            ImGui::SameLine(0, ui::Dp(2.0f));
            if (ImGui::ArrowButton("##pxnext", ImGuiDir_Right)) g_view.frame = (g_view.frame + 1) % rec->frames;
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ui::Dp(110.0f));
            ImGui::SliderInt("##pxfps", &rec->fps, 2, 24, "%d frames/s");
            if (ImGui::IsItemDeactivatedAfterEdit()) Save();
        }
        ResultActions(app, *rec);
        if (!rec->error.empty() && rec->activity <= 0) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(ui::kBad, "%s", rec->error.c_str());
            ImGui::PopTextWrapPos();
        }
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        DrawPixelCanvas(app, *rec, ImVec2(std::max(40.0f, avail.x), std::max(ui::Dp(120.0f), height - ImGui::GetCursorPosY() - ui::Dp(8.0f))));
    } else {
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##pxempty", ImVec2(std::max(40.0f, avail.x), std::max(ui::Dp(120.0f), avail.y - ui::Dp(4.0f))));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), IM_COL32(14, 16, 20, 255), ui::Dp(6.0f));
        const char* text = g_ctl.mode == 1 ? "Drop a picture here to make it pixel art" : "Describe something on the right and press Make";
        const ImVec2 ts = ImGui::CalcTextSize(text);
        dl->AddText(ImVec2(origin.x + (avail.x - ts.x) * 0.5f, origin.y + (avail.y - ts.y) * 0.5f), IM_COL32(200, 204, 214, 220), text);
    }
    // pictures dragged onto the viewer become pictures to convert
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("MV_LIBITEM")) {
            g_ctl.mode = 1;
            AddSource(app, std::wstring(static_cast<const wchar_t*>(pl->Data)));
        } else if (const ImGuiPayload* pg = ImGui::AcceptDragDropPayload("MV_IMAGES")) {
            const int* idx = static_cast<const int*>(pg->Data);
            const int n = static_cast<int>(pg->DataSize / sizeof(int));
            g_ctl.mode = 1;
            for (int i = 0; i < n; ++i) {
                if (idx[i] >= 0 && idx[i] < static_cast<int>(app.entries.size())) AddSource(app, app.entries[idx[i]].path);
            }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::EndChild();

    // ---- the controls
    ImGui::TableSetColumnIndex(2);
    ImGui::BeginChild("px_ctl", ImVec2(0, height), true);
    ImGui::BeginChild("##pxctlbody", ImVec2(0, -(ImGui::GetFrameHeightWithSpacing() + ui::Dp(30.0f))), false);
    DrawSetupBox(app);
    {
        static const char* modes[] = {"Create", "Convert a picture", "Make a set"};
        const float third = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3.0f;
        for (int m = 0; m < 3; ++m) {
            if (m) ImGui::SameLine();
            const bool on = g_ctl.mode == m;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ui::kAccent);
            if (ImGui::Button(modes[m], ImVec2(third, 0))) g_ctl.mode = m;
            if (on) ImGui::PopStyleColor();
        }
    }
    ImGui::Spacing();
    if (g_ctl.mode == 0) DrawCreate(app);
    else if (g_ctl.mode == 1) DrawConvert(app);
    else DrawSetPlanner(app);
    const std::string phase = Phase();
    if (!phase.empty() || WriterBusy()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(ui::kAccent, "%s", phase.empty() ? "The prompt writer is working..." : phase.c_str());
        ImGui::PopTextWrapPos();
    }

    ImGui::Spacing();
    ImGui::Separator();
    // the palette: of the result being looked at (live), else of the next ones
    PixelOptions& nextOpt = g_ctl.mode == 1 ? g_ctl.convOpt : g_ctl.opt;
    PixelOptions shown = set ? set->opt : rec ? Effective(*rec) : nextOpt;
    const PixelOptions before = shown;
    ImGui::TextUnformatted(set ? "Palette of the whole set" : rec ? (SharedSet(*rec) ? "Palette of its set (background: this picture)" : "Palette of this picture") : "Palette of the next pictures");
    if (PaletteControls(shown, set != nullptr || (rec && SharedSet(*rec)))) {
        g_deferWrites = true;
        if (set) {
            set->opt = shown;
            // the see-through background and outline go to the sprite-like pictures of the set (tiles and backgrounds keep theirs)
            for (PxRecord* r : Members(set->id)) {
                if (!set->shared) r->opt = shown;
                else if (kKinds[r->kind].transparent) {
                    r->opt.transparent = shown.transparent;
                    r->opt.outline = shown.outline;
                }
            }
            RenderSet(app, set->id);
        } else if (rec) {
            if (SharedSet(*rec)) {
                PxSet* s = FindSet(rec->setId);
                s->opt.palette = shown.palette;
                s->opt.colors = shown.colors;
                s->opt.clean = shown.clean;
            }
            rec->opt = shown;
            RenderAny(app, *rec);
            if (shown.transparent && !before.transparent && NeedsCutout(*rec)) StartCutout(app, *rec);
        } else {
            nextOpt = shown;
        }
        for (PixelOptions* o : {&g_ctl.opt, &g_ctl.convOpt}) { // the next pictures get the same palette
            o->palette = shown.palette;
            o->colors = shown.colors;
            o->clean = shown.clean;
        }
        (void)before;
        g_deferWrites = false;
        g_pendingSave = true; // written when nothing is being dragged (PumpPixelArt)
    }
    // a photo has no plain background to cut away: SAM 3 cuts out what is named here
    const bool forRec = rec && Converted(*rec) && Effective(*rec).transparent;
    if (forRec || (!rec && !set && g_ctl.mode == 1 && g_ctl.convOpt.transparent)) {
        ImGui::TextUnformatted("Keep (cut out of photos)");
        ImGui::SameLine();
        ImGui::Checkbox("Smart##pxsmartkeep", &g_ctl.smartKeep);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("The vision model (the prompt writer, with its picture file) looks at the picture first and names its main subjects and what they hold,\n"
                              "so the background people and things are left out. Off, or when it cannot see pictures: the words below.");
        if (g_ctl.smartKeep && !(WriterConfigured() && WriterCanSee())) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("The prompt writer cannot look at pictures (Clef mode tab > Prompt writer: it needs its mmproj file), so the words below are used.");
            ImGui::PopTextWrapPos();
        }
        InputLine("##pxkeep", g_ctl.smartKeep ? "used when the vision model cannot choose" : "e.g. person, cat, car", g_ctl.keep, forRec ? -ui::Dp(110.0f) : -FLT_MIN);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("SAM 3 finds these in the picture and everything else becomes see-through (pictures with a plain background need nothing).\n"
                              "Separate several with commas; person:2 keeps at most two people.");
        if (forRec) {
            ImGui::SameLine();
            ImGui::BeginDisabled(rec->maskAct != 0);
            if (ImGui::SmallButton(rec->mask.empty() ? "Cut out" : "Cut out again")) {
                rec->mask.clear();
                g_maskCells.erase(rec->id);
                StartCutout(app, *rec);
                Save();
            }
            ImGui::EndDisabled();
            if (rec->maskAct > 0) ImGui::TextColored(ui::kAccent, "SAM 3 is cutting out: %s", rec->keepUsed.c_str());
            else if (rec->maskAct < 0) ImGui::TextColored(ui::kAccent, "The vision model is choosing what to keep...");
            else if (!rec->keepUsed.empty() && !rec->mask.empty()) ImGui::TextDisabled("Kept: %s", rec->keepUsed.c_str());
        }
    }
    if (set) {
        if (ImGui::Checkbox("One palette for the whole set", &set->shared)) {
            for (PxRecord* r : Members(set->id)) r->opt = set->opt;
            RenderSet(app, set->id);
            Save();
        }
    }
    if (ImGui::TreeNode("Fine-tune##pxfine")) {
        ImGui::SetNextItemWidth(ui::Dp(160.0f));
        ImGui::SliderInt("steps##pxsteps", &g_ctl.steps, 8, 40);
        ImGui::SetNextItemWidth(ui::Dp(160.0f));
        ImGui::SliderFloat("prompt strength (CFG)##pxcfg", &g_ctl.cfg, 2.0f, 12.0f, "%.1f");
        InputLine("##pxneg", "negative prompt (empty = blurry, photo, 3d render, text...)", g_ctl.negative, -FLT_MIN);
        ImGui::TreePop();
    }
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Every result is real pixel art: one pixel per art pixel, with the palette above. The palette can be changed afterwards without making anything again.");
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    DrawMakeButton(app);
    ImGui::EndChild();
    ImGui::EndTable();
}
