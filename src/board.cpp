#include "board.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ctime>
#include <map>
#include <set>
#include <windows.h>
#include <shlobj.h>
#include <winhttp.h>

#include "json.hpp"

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

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// "a knight, tall." -> "a knight, tall" so sentences can be joined cleanly
std::string NoEndPunct(std::string s) {
    s = Trim(s);
    while (!s.empty() && (s.back() == '.' || s.back() == ',' || s.back() == ';')) s.pop_back();
    return s;
}

std::wstring AppDataDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring base = (n > 0 && n < MAX_PATH) ? std::wstring(buf, n) : std::wstring(L".");
    return base + L"\\Try2Create";
}

std::wstring LowerW(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

std::string SecText(float t) {
    char buf[32];
    if (std::fabs(t - std::round(t)) < 0.05f) std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(std::lround(t)));
    else std::snprintf(buf, sizeof(buf), "%.1f", t);
    return buf;
}

std::string SafeFileName(const std::string& name, const char* fallback) {
    std::string clean;
    for (char c : Trim(name)) clean += (std::strchr("\\/:*?\"<>|", c) || static_cast<unsigned char>(c) < 32) ? '_' : c;
    if (clean.size() > 60) clean.resize(60);
    while (!clean.empty() && (clean.back() == '.' || clean.back() == ' ')) clean.pop_back();
    return clean.empty() ? fallback : clean;
}

bool IsImageExt(const std::wstring& ext) {
    for (const wchar_t* e : {L".png", L".jpg", L".jpeg", L".webp", L".bmp", L".gif", L".tif", L".tiff", L".avif"}) {
        if (_wcsicmp(ext.c_str(), e) == 0) return true;
    }
    return false;
}

long long FileUnixTime(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return 0;
    const unsigned long long ticks = (static_cast<unsigned long long>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
    return ticks > 116444736000000000ULL ? static_cast<long long>((ticks - 116444736000000000ULL) / 10000000ULL) : 0;
}

} // namespace

const char* BoardKindName(BoardKind kind) {
    switch (kind) {
        case BoardKind::Character: return "Character";
        case BoardKind::Background: return "Background";
        case BoardKind::Item: return "Item";
        case BoardKind::Note: return "Note";
    }
    return "";
}

const char* InvSlotName(InvSlot slot) {
    switch (slot) {
        case InvSlot::Wearing: return "Wearing";
        case InvSlot::Holding: return "Holding";
        case InvSlot::Carrying: return "Carrying";
        case InvSlot::Vehicle: return "Vehicle (nearby)";
    }
    return "";
}

BoardNode* Board::Find(int id) {
    for (BoardNode& n : nodes) {
        if (n.id == id) return &n;
    }
    return nullptr;
}
const BoardNode* Board::Find(int id) const {
    for (const BoardNode& n : nodes) {
        if (n.id == id) return &n;
    }
    return nullptr;
}

int Board::Add(BoardKind kind, const std::wstring& image, const std::string& text, float x, float y, int scene) {
    BoardNode n;
    n.id = nextId++;
    n.kind = kind;
    n.scene = std::clamp(scene, 0, static_cast<int>(scenes.size()) - 1);
    n.image = image;
    n.text = text;
    n.x = x;
    n.y = y;
    nodes.push_back(std::move(n));
    return nodes.back().id;
}

int Board::Connect(int from, int to, const std::string& label) {
    const BoardNode *f = Find(from), *t = Find(to);
    if (from == to || !f || !t || f->scene != t->scene) return -1;
    for (const BoardLink& l : links) {
        if (l.from == from && l.to == to) return -1;
    }
    BoardLink l;
    l.id = nextId++;
    l.from = from;
    l.to = to;
    l.label = label.empty() ? BoardRelationPresets(f->kind, t->kind).front() : label;
    links.push_back(std::move(l));
    return links.back().id;
}

void Board::Remove(int nodeId) {
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(), [&](const BoardNode& n) { return n.id == nodeId; }), nodes.end());
    links.erase(std::remove_if(links.begin(), links.end(), [&](const BoardLink& l) { return l.from == nodeId || l.to == nodeId; }), links.end());
}

void Board::RemoveLink(int linkId) {
    links.erase(std::remove_if(links.begin(), links.end(), [&](const BoardLink& l) { return l.id == linkId; }), links.end());
}

namespace {
// What makes two cards "the same one" in different beats: the same kind and picture (or name).
std::string ElementKey(const BoardNode& n) {
    return std::to_string(static_cast<int>(n.kind)) + "|" + (n.image.empty() ? Trim(n.title) + "#" + std::to_string(n.id) : ToUtf8(LowerW(n.image)));
}
} // namespace

std::string Board::DisplayName(const BoardNode& n) const {
    if (!Trim(n.title).empty()) return Trim(n.title);
    // untitled cards are numbered per kind, in order of first appearance; copies in other beats keep the number
    std::vector<std::string> seen;
    int index = 0;
    const std::string mine = ElementKey(n);
    for (const BoardNode& o : nodes) {
        if (o.kind != n.kind) continue;
        const std::string key = ElementKey(o);
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
        seen.push_back(key);
        if (key == mine) index = static_cast<int>(seen.size());
    }
    return seen.size() > 1 ? std::string(BoardKindName(n.kind)) + " " + std::to_string(index) : std::string(BoardKindName(n.kind));
}

std::string Board::SceneName(int scene) const {
    if (scene < 0 || scene >= static_cast<int>(scenes.size())) return "Scene";
    return Trim(scenes[scene].name).empty() ? "Scene " + std::to_string(scene + 1) : Trim(scenes[scene].name);
}

float Board::TotalSec() const {
    float t = 0;
    for (const BoardScene& s : scenes) t = std::max(t, s.end);
    return t;
}

int Board::AddScene(const std::string& name) {
    BoardScene s;
    s.name = name;
    const float len = scenes.empty() ? 3.0f : std::max(0.5f, scenes.back().end - scenes.back().start);
    s.start = scenes.empty() ? 0.0f : scenes.back().end;
    s.end = s.start + len;
    scenes.push_back(std::move(s));
    return static_cast<int>(scenes.size()) - 1;
}


const std::vector<CameraChoice>& CameraChoices(int kind) {
    static const std::vector<CameraChoice> shot = {
        {"Auto", ""},
        {"Extreme close-up (eyes / a detail)", "extreme close-up shot of the eyes or a small detail"},
        {"Close-up (face)", "close-up shot, head and shoulders in frame"},
        {"Medium close-up (chest up)", "medium close-up shot, framed from the chest up"},
        {"Medium shot (waist up, half body)", "medium shot, framed from the waist up (half body), no legs in frame"},
        {"Cowboy shot (mid-thigh up)", "cowboy shot, framed from mid-thigh up"},
        {"Full body shot", "full body shot, the whole figure from head to feet in frame"},
        {"Wide shot", "wide shot, the characters fully visible and small in their surroundings"},
        {"Extreme wide / establishing", "extreme wide establishing shot showing the whole location"},
    };
    static const std::vector<CameraChoice> angle = {
        {"Auto", ""},
        {"Eye level", "eye-level camera angle"},
        {"Low angle (looking up)", "low camera angle looking up at the subject"},
        {"High angle (looking down)", "high camera angle looking down at the subject"},
        {"Bird's-eye view (from above)", "overhead bird's-eye view straight down"},
        {"Worm's-eye view (from the ground)", "worm's-eye view from ground level looking up"},
        {"Dutch angle (tilted)", "tilted dutch angle"},
        {"Over-the-shoulder", "over-the-shoulder shot"},
        {"Point of view (first person)", "first-person point-of-view shot"},
        {"Side profile view", "side profile view"},
        {"From behind", "seen from behind"},
    };
    static const std::vector<CameraChoice> lens = {
        {"Auto", ""},
        {"Wide-angle lens (24 mm)", "wide-angle 24mm lens"},
        {"Standard lens (35-50 mm)", "35mm lens"},
        {"Portrait lens (85 mm)", "85mm portrait lens"},
        {"Telephoto lens (200 mm, compressed)", "200mm telephoto lens with compressed perspective"},
        {"Fisheye lens", "fisheye lens distortion"},
        {"Macro lens", "macro lens"},
    };
    static const std::vector<CameraChoice> movement = {
        {"Auto", ""},
        {"Static, locked off", "static locked-off camera"},
        {"Slow push-in (dolly in)", "slow dolly-in push toward the subject"},
        {"Pull-out (dolly out)", "dolly-out pulling away from the subject"},
        {"Pan", "smooth horizontal pan"},
        {"Tilt", "smooth vertical tilt"},
        {"Tracking shot (follows the action)", "tracking shot following the action"},
        {"Handheld, shaky", "handheld shaky camera"},
        {"Orbit around the subject", "camera orbiting around the subject"},
        {"Crane / drone rising", "crane or drone shot rising upward"},
        {"Whip pan", "fast whip pan"},
        {"Zoom in", "zoom in"},
        {"Slow motion", "slow motion"},
    };
    static const std::vector<CameraChoice> lighting = {
        {"Auto", ""},
        {"Natural daylight", "natural daylight"},
        {"Golden hour (warm, low sun)", "warm golden-hour light from a low sun"},
        {"Overcast, soft", "soft overcast light"},
        {"Soft studio lighting", "soft studio lighting"},
        {"Hard, high contrast", "hard high-contrast lighting"},
        {"Dramatic low-key (deep shadows)", "dramatic low-key lighting with deep shadows"},
        {"Bright high-key", "bright high-key lighting"},
        {"Backlit / rim light", "strong backlight with a rim light around the subject"},
        {"Neon, colourful", "colourful neon lighting"},
        {"Moonlight (cold blue)", "cold blue moonlight"},
        {"Candlelight / firelight", "warm flickering candlelight or firelight"},
        {"Cinematic teal and orange", "cinematic teal-and-orange lighting"},
        {"Volumetric light rays", "volumetric light rays through the air"},
    };
    static const std::vector<CameraChoice> dof = {
        {"Auto", ""},
        {"Shallow depth of field (blurred background)", "shallow depth of field with a softly blurred background"},
        {"Deep focus (everything sharp)", "deep focus with everything sharp"},
        {"Motion blur", "motion blur on the fast movement"},
    };
    static const std::vector<CameraChoice> none;
    switch (kind) {
        case 0: return shot;
        case 1: return angle;
        case 2: return lens;
        case 3: return movement;
        case 4: return lighting;
        case 5: return dof;
    }
    return none;
}

std::string CameraText(const BoardScene& s, bool includeMovement) {
    std::string out;
    const int values[6] = {s.shot, s.angle, s.lens, includeMovement ? s.movement : 0, s.lighting, s.dof};
    for (int k = 0; k < 6; ++k) {
        const auto& list = CameraChoices(k);
        if (values[k] <= 0 || values[k] >= static_cast<int>(list.size())) continue;
        if (!out.empty()) out += ", ";
        out += list[values[k]].phrase;
    }
    return out;
}

int Board::AddSceneAfter(int after, bool copy) {
    after = std::clamp(after, 0, static_cast<int>(scenes.size()) - 1);
    BoardScene s;
    s.start = scenes[after].end;
    s.end = s.start + std::max(0.5f, scenes[after].end - scenes[after].start);
    s.shot = scenes[after].shot;
    s.angle = scenes[after].angle;
    s.lens = scenes[after].lens;
    s.movement = scenes[after].movement;
    s.lighting = scenes[after].lighting;
    s.dof = scenes[after].dof;
    const int at = after + 1;
    for (BoardNode& n : nodes) {
        if (n.scene >= at) ++n.scene;
    }
    scenes.insert(scenes.begin() + at, std::move(s));
    if (copy) {
        std::vector<std::pair<int, int>> idMap; // old card id -> new card id
        const size_t count = nodes.size();
        for (size_t i = 0; i < count; ++i) {
            if (nodes[i].scene != after) continue;
            BoardNode c = nodes[i];
            const int oldId = c.id;
            c.id = nextId++;
            c.scene = at;
            c.describing = false;
            idMap.push_back({oldId, c.id});
            nodes.push_back(std::move(c));
        }
        auto mapped = [&](int id) {
            for (const auto& m : idMap)
                if (m.first == id) return m.second;
            return -1;
        };
        const size_t linkCount = links.size();
        for (size_t i = 0; i < linkCount; ++i) {
            const int f = mapped(links[i].from), t = mapped(links[i].to);
            if (f < 0 || t < 0) continue;
            BoardLink l = links[i];
            l.id = nextId++;
            l.from = f;
            l.to = t;
            links.push_back(std::move(l));
        }
    }
    return at;
}

void Board::RemoveScene(int scene) {
    if (scenes.size() <= 1 || scene < 0 || scene >= static_cast<int>(scenes.size())) return;
    std::vector<int> gone;
    for (const BoardNode& n : nodes) {
        if (n.scene == scene) gone.push_back(n.id);
    }
    for (int id : gone) Remove(id);
    for (BoardNode& n : nodes) {
        if (n.scene > scene) --n.scene;
    }
    scenes.erase(scenes.begin() + scene);
}

int Board::SplitScene(int scene, float t) {
    if (scene < 0 || scene >= static_cast<int>(scenes.size())) return -1;
    const float s = scenes[scene].start, e = scenes[scene].end;
    if (t <= s + 0.05f || t >= e - 0.05f) return -1;
    const int at = AddSceneAfter(scene, true);
    scenes[at].start = t;
    scenes[at].end = e;
    scenes[scene].end = t;
    scenes[at].name = scenes[scene].name;
    scenes[at].text = scenes[scene].text;
    return at;
}

void Board::Ripple(int fromIndex) {
    for (size_t k = static_cast<size_t>(std::max(0, fromIndex)) + 1; k < scenes.size(); ++k) {
        if (scenes[k].start < scenes[k - 1].end) {
            const float d = scenes[k - 1].end - scenes[k].start;
            scenes[k].start += d;
            scenes[k].end += d;
        }
    }
}

int Board::CloneScene(int scene) {
    if (scene < 0 || scene >= static_cast<int>(scenes.size())) return -1;
    const int at = AddSceneAfter(scene, true);
    scenes[at].name = scenes[scene].name;
    scenes[at].text = scenes[scene].text;
    Ripple(at);
    return at;
}

void Board::CloseGap(int scene) {
    if (scene < 0 || scene >= static_cast<int>(scenes.size())) return;
    const float limit = scene == 0 ? 0.0f : scenes[scene - 1].end;
    for (size_t k = static_cast<size_t>(scene); k < scenes.size(); ++k) {
        const float want = k == static_cast<size_t>(scene) ? limit : scenes[k - 1].end;
        const float d = scenes[k].start - want;
        if (d <= 0.0001f && k != static_cast<size_t>(scene)) break;
        if (d > 0) {
            scenes[k].start -= d;
            scenes[k].end -= d;
        }
    }
}

bool Board::SwapScene(int scene, int dir) {
    const int other = scene + dir;
    if (scene < 0 || other < 0 || scene >= static_cast<int>(scenes.size()) || other >= static_cast<int>(scenes.size())) return false;
    const int a = std::min(scene, other), c = std::max(scene, other); // a is earlier in the order
    const float lenA = scenes[a].end - scenes[a].start, lenC = scenes[c].end - scenes[c].start;
    const float gap = scenes[c].start - scenes[a].end;
    BoardScene first = scenes[c], second = scenes[a];
    const float base = scenes[a].start;
    first.start = base;
    first.end = base + lenC;
    second.start = first.end + std::max(0.0f, gap);
    second.end = second.start + lenA;
    for (BoardNode& n : nodes) {
        if (n.scene == a) n.scene = c;
        else if (n.scene == c) n.scene = a;
    }
    scenes[a] = first;
    scenes[c] = second;
    // anything after the pair must not now overlap it
    Ripple(c);
    return true;
}

void Board::SortScenes(int* current) {
    std::vector<int> order(scenes.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
    std::stable_sort(order.begin(), order.end(), [&](int a, int c) { return scenes[a].start < scenes[c].start; });
    bool same = true;
    for (size_t i = 0; i < order.size(); ++i) same = same && order[i] == static_cast<int>(i);
    if (same) return;
    std::vector<int> newIndex(scenes.size());
    std::vector<BoardScene> sorted;
    for (size_t i = 0; i < order.size(); ++i) {
        newIndex[order[i]] = static_cast<int>(i);
        sorted.push_back(scenes[order[i]]);
    }
    for (BoardNode& n : nodes) n.scene = newIndex[std::clamp(n.scene, 0, static_cast<int>(newIndex.size()) - 1)];
    if (current) *current = newIndex[std::clamp(*current, 0, static_cast<int>(newIndex.size()) - 1)];
    scenes = std::move(sorted);
}


// ---------------------------------------------------------------- relations with categories, and the ones you save
namespace {
struct MyRelation {
    std::string label, category;
    int from = -1, to = -1; // card kinds it applies to (-1 = any)
};

std::wstring RelationsPath() { return AppDataDir() + L"\\relations.json"; }

std::vector<MyRelation>& MyRelations() {
    static std::vector<MyRelation> list;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        std::ifstream f(RelationsPath(), std::ios::binary);
        if (f) {
            json j = json::parse(f, nullptr, false);
            if (j.is_array()) {
                for (const json& e : j) {
                    MyRelation r;
                    r.label = e.value("label", std::string());
                    r.category = e.value("category", std::string("My own"));
                    r.from = e.value("from", -1);
                    r.to = e.value("to", -1);
                    if (!r.label.empty()) list.push_back(std::move(r));
                }
            }
        }
    }
    return list;
}

void SaveMyRelations() {
    json j = json::array();
    for (const MyRelation& r : MyRelations()) j.push_back({{"label", r.label}, {"category", r.category}, {"from", r.from}, {"to", r.to}});
    std::error_code ec;
    fs::create_directories(AppDataDir(), ec);
    std::ofstream out(RelationsPath(), std::ios::binary | std::ios::trunc);
    if (out) out << j.dump(2, ' ', false, json::error_handler_t::replace);
}

struct BuiltIn {
    const char* label;
    const char* category;
};
const std::vector<BuiltIn>& BuiltInRelations(BoardKind from, BoardKind to) {
    static const std::vector<BuiltIn> charChar = {
        {"fights against", "Fighting"}, {"duels", "Fighting"}, {"attacks", "Fighting"}, {"strikes down", "Fighting"}, {"is defeated by", "Fighting"}, {"chases", "Fighting"},
        {"fights alongside", "Friends & team"}, {"teams up with", "Friends & team"}, {"defends", "Friends & team"}, {"protects", "Friends & team"},
        {"talks to", "Friends & team"}, {"stands next to", "Friends & team"}, {"high-fives", "Friends & team"}, {"takes a selfie with", "Friends & team"},
        {"hugs", "Couple"}, {"kisses", "Couple"}, {"holds hands with", "Couple"}, {"dances with", "Couple"}, {"looks at", "Couple"}, {"cuddles", "Couple"},
        {"pranks", "Funny"}, {"tickles", "Funny"}, {"photobombs", "Funny"}, {"argues with", "Funny"}, {"laughs at", "Funny"}, {"throws a pie at", "Funny"},
        {"scares", "Funny"}, {"mimics", "Funny"}, {"dares", "Funny"}, {"steals the snack of", "Funny"}, {"swaps hats with", "Funny"}, {"trips over", "Funny"}, {"wakes up", "Funny"},
        {"explores with", "Adventure"}, {"hides from", "Adventure"}, {"rescues", "Adventure"}, {"follows", "Adventure"}, {"sneaks past", "Adventure"}, {"escapes with", "Adventure"},
        {"searches for clues with", "Adventure"}, {"carries to safety", "Adventure"}, {"leads through the dark", "Adventure"}, {"is lost with", "Adventure"},
        {"cooks a meal with", "Everyday"}, {"shares an umbrella with", "Everyday"}, {"toasts with", "Everyday"}, {"plays a board game with", "Everyday"}, {"walks with", "Everyday"},
        {"studies with", "Everyday"}, {"shares a secret with", "Everyday"}, {"teaches a skill to", "Everyday"}, {"waits impatiently with", "Everyday"}, {"builds something with", "Everyday"},
        {"does a trick for", "Everyday"}, {"races", "Everyday"}, {"whispers to", "Everyday"},
        {"faces off with", "Rivals & drama"}, {"challenges", "Rivals & drama"}, {"outsmarts", "Rivals & drama"}, {"bargains with", "Rivals & drama"}, {"stares down", "Rivals & drama"},
        {"confronts", "Rivals & drama"}, {"forgives", "Rivals & drama"}, {"betrays", "Rivals & drama"}, {"swaps places with", "Rivals & drama"}};
    static const std::vector<BuiltIn> charItem = {{"holds", "Holding & using"}, {"wears", "Holding & using"}, {"uses", "Holding & using"}, {"carries", "Holding & using"},
                                                  {"wields", "Holding & using"}, {"points at", "Holding & using"}, {"throws", "Holding & using"}, {"eats", "Holding & using"},
                                                  {"rides", "Holding & using"}, {"pilots", "Holding & using"}, {"drives", "Holding & using"}, {"stands next to", "Holding & using"}};
    static const std::vector<BuiltIn> charBg = {{"stands in", "Places"}, {"walks through", "Places"}, {"is in the foreground of", "Places"},
                                                {"sits in", "Places"}, {"is hiding in", "Places"}, {"flies over", "Places"}};
    static const std::vector<BuiltIn> itemAny = {{"is placed in", "Placement"}, {"lies on", "Placement"}, {"is next to", "Placement"}, {"hangs in", "Placement"}, {"floats near", "Placement"}};
    static const std::vector<BuiltIn> other = {{"is near", "General"}, {"interacts with", "General"}, {"is part of", "General"}, {"is in front of", "General"}, {"is behind", "General"}};
    if (from == BoardKind::Character && to == BoardKind::Character) return charChar;
    if (from == BoardKind::Character && to == BoardKind::Item) return charItem;
    if (from == BoardKind::Character && to == BoardKind::Background) return charBg;
    if (from == BoardKind::Item) return itemAny;
    return other;
}
} // namespace

std::vector<RelationEntry> BoardRelations(BoardKind from, BoardKind to) {
    std::vector<RelationEntry> out;
    for (const BuiltIn& b : BuiltInRelations(from, to)) out.push_back({b.label, b.category, false});
    for (const MyRelation& r : MyRelations()) {
        if ((r.from >= 0 && r.from != static_cast<int>(from)) || (r.to >= 0 && r.to != static_cast<int>(to))) continue;
        bool dup = false;
        for (const RelationEntry& e : out) dup = dup || e.label == r.label;
        if (!dup) out.push_back({r.label, r.category, true});
    }
    std::vector<std::string> order; // categories in order of first appearance
    for (const RelationEntry& e : out) {
        if (std::find(order.begin(), order.end(), e.category) == order.end()) order.push_back(e.category);
    }
    std::stable_sort(out.begin(), out.end(), [&](const RelationEntry& a, const RelationEntry& b) {
        return std::find(order.begin(), order.end(), a.category) < std::find(order.begin(), order.end(), b.category);
    });
    return out;
}

std::vector<std::string> RelationCategories() {
    std::vector<std::string> out = {"Fighting", "Friends & team", "Couple", "Funny", "Adventure", "Everyday", "Rivals & drama", "Holding & using", "Places", "My own"};
    for (const MyRelation& r : MyRelations()) {
        if (std::find(out.begin(), out.end(), r.category) == out.end()) out.push_back(r.category);
    }
    return out;
}

void AddMyRelation(BoardKind from, BoardKind to, const std::string& label, const std::string& category) {
    const std::string l = Trim(label);
    if (l.empty()) return;
    auto& list = MyRelations();
    for (MyRelation& r : list) {
        if (r.label == l) { // saved before: only its category changes
            r.category = Trim(category).empty() ? r.category : Trim(category);
            SaveMyRelations();
            return;
        }
    }
    MyRelation r;
    r.label = l;
    r.category = Trim(category).empty() ? "My own" : Trim(category);
    r.from = static_cast<int>(from);
    r.to = static_cast<int>(to);
    list.push_back(std::move(r));
    SaveMyRelations();
}

void RemoveMyRelation(const std::string& label) {
    auto& list = MyRelations();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const MyRelation& r) { return r.label == label; }), list.end());
    SaveMyRelations();
}

const std::vector<const char*>& BoardRelationPresets(BoardKind from, BoardKind to) {
    static const std::vector<const char*> charChar = {"fights against", "fights alongside", "teams up with", "duels", "attacks", "strikes down",
                                                      "defends", "protects", "chases", "is defeated by", "talks to", "hugs", "looks at", "kisses",
                                                      "stands next to", "holds hands with", "dances with"};
    static const std::vector<const char*> charItem = {"holds", "wears", "uses", "carries", "wields", "points at", "throws", "eats", "rides", "pilots", "drives", "stands next to"};
    static const std::vector<const char*> charBg = {"stands in", "walks through", "is in the foreground of", "sits in", "is hiding in", "flies over"};
    static const std::vector<const char*> itemAny = {"is placed in", "lies on", "is next to", "hangs in", "floats near"};
    static const std::vector<const char*> other = {"is near", "interacts with", "is part of", "is in front of", "is behind"};
    if (from == BoardKind::Character && to == BoardKind::Character) return charChar;
    if (from == BoardKind::Character && to == BoardKind::Item) return charItem;
    if (from == BoardKind::Character && to == BoardKind::Background) return charBg;
    if (from == BoardKind::Item) return itemAny;
    return other;
}

// ---------------------------------------------------------------------------------------------------------------
// Characters
InvItem& CharacterProfile::AddItem(InvSlot slot, const std::string& itemName, const std::wstring& itemImage) {
    InvItem it;
    it.id = nextItemId++;
    it.slot = slot;
    it.name = itemName;
    it.image = itemImage;
    items.push_back(std::move(it));
    return items.back();
}

std::string CharacterKey(const std::wstring& image) {
    unsigned long long h = 1469598103934665603ULL;
    for (wchar_t c : LowerW(image)) {
        h ^= static_cast<unsigned long long>(c);
        h *= 1099511628211ULL;
    }
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%016llx", h);
    return buf;
}

namespace {
std::vector<CharacterProfile>& Profiles() {
    static std::vector<CharacterProfile> list;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        std::ifstream f(AppDataDir() + L"\\characters.json", std::ios::binary);
        if (f) {
            json j = json::parse(f, nullptr, false);
            if (j.is_object() && j.contains("characters") && j["characters"].is_array()) {
                for (const json& c : j["characters"]) {
                    CharacterProfile p;
                    p.image = ToWide(c.value("image", std::string()));
                    if (p.image.empty()) continue;
                    p.key = c.value("key", CharacterKey(p.image));
                    p.name = c.value("name", std::string());
                    if (p.name.rfind("Name:", 0) == 0) p.name = p.name.substr(p.name.find_first_not_of(' ', 5) == std::string::npos ? 5 : p.name.find_first_not_of(' ', 5)); // (an early cast inventer kept the label)
                    p.description = c.value("description", std::string());
                    p.saved = c.value("saved", false);
                    p.sheet = ToWide(c.value("sheet", std::string()));
                    p.sheetNeeded = c.value("sheetNeeded", -1);
                    p.orbit = ToWide(c.value("orbit", std::string()));
                    p.backstory = c.value("backstory", std::string());
                    p.nextItemId = c.value("nextItemId", 1);
                    if (c.contains("items") && c["items"].is_array()) {
                        for (const json& i : c["items"]) {
                            InvItem it;
                            it.id = i.value("id", 0);
                            it.slot = static_cast<InvSlot>(std::clamp(i.value("slot", 1), 0, 3));
                            it.name = i.value("name", std::string());
                            it.text = i.value("text", std::string());
                            it.image = ToWide(i.value("image", std::string()));
                            p.nextItemId = std::max(p.nextItemId, it.id + 1);
                            p.items.push_back(std::move(it));
                        }
                    }
                    list.push_back(std::move(p));
                }
            }
        }
        list.reserve(list.size() + 4096); // references to a profile stay valid while the program is used
    }
    return list;
}

CharacterProfile* ByKey(const std::string& key) {
    if (key.empty()) return nullptr;
    for (CharacterProfile& p : Profiles()) {
        if (p.key == key) return &p;
    }
    return nullptr;
}

// A card's own profile under `key`, made when missing: a copy of the saved character with this picture (or of any earlier one when
// `inheritUnsaved`, used for boards that were made before cards had their own inventory).
CharacterProfile& Instance(const std::string& key, const std::wstring& image, bool inheritUnsaved) {
    if (CharacterProfile* p = ByKey(key)) return *p;
    CharacterProfile fresh;
    fresh.key = key;
    fresh.image = image;
    if (const CharacterProfile* tpl = FindProfile(image)) {
        if (tpl->saved || inheritUnsaved) {
            fresh.name = tpl->name;
            fresh.description = tpl->description;
            fresh.items = tpl->items;
            fresh.nextItemId = tpl->nextItemId;
        }
    }
    Profiles().push_back(std::move(fresh));
    return Profiles().back();
}
} // namespace

std::vector<CharacterProfile>& AllProfiles() { return Profiles(); }

CharacterProfile* ProfileOf(const BoardNode& node) {
    if (node.kind != BoardKind::Character || node.image.empty()) return nullptr;
    return ByKey(node.profile);
}

void DiscardProfile(const std::string& key) {
    if (key.find('#') == std::string::npos) return; // only a card's own inventory
    auto& list = Profiles();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const CharacterProfile& p) { return p.key == key && !p.saved; }), list.end());
}

CharacterProfile& EnsureProfileOf(Board& board, int nodeId, bool inheritUnsaved) {
    BoardNode* n = board.Find(nodeId);
    static unsigned counter = 0;
    if (!n) return EnsureProfile(std::wstring()); // cannot happen: the caller has the card
    if (CharacterProfile* p = ByKey(n->profile)) return *p;
    for (const BoardNode& o : board.nodes) { // the same picture elsewhere on the board is the same character: one inventory
        if (o.id != n->id && o.kind == BoardKind::Character && !o.profile.empty() && _wcsicmp(o.image.c_str(), n->image.c_str()) == 0) {
            if (ByKey(o.profile)) {
                n->profile = o.profile;
                return *ByKey(o.profile);
            }
        }
    }
    char suffix[40];
    std::snprintf(suffix, sizeof(suffix), "#%llx%x", static_cast<unsigned long long>(std::time(nullptr)), ++counter);
    n->profile = CharacterKey(n->image) + suffix;
    return Instance(n->profile, n->image, inheritUnsaved);
}

CharacterProfile* FindProfile(const std::wstring& image) {
    if (image.empty()) return nullptr;
    const std::string key = CharacterKey(image);
    for (CharacterProfile& p : Profiles()) {
        if (p.key == key) return &p;
    }
    return nullptr;
}

CharacterProfile& EnsureProfile(const std::wstring& image) {
    if (CharacterProfile* p = FindProfile(image)) return *p;
    CharacterProfile p;
    p.image = image;
    p.key = CharacterKey(image);
    Profiles().push_back(std::move(p));
    return Profiles().back();
}

void SaveProfiles() {
    json j;
    j["characters"] = json::array();
    for (const CharacterProfile& p : Profiles()) {
        json c = {{"key", p.key}, {"image", ToUtf8(p.image)}, {"name", p.name}, {"description", p.description}, {"saved", p.saved}, {"nextItemId", p.nextItemId}, {"items", json::array()}};
        if (!p.sheet.empty()) c["sheet"] = ToUtf8(p.sheet), c["sheetNeeded"] = p.sheetNeeded;
        if (!p.orbit.empty()) c["orbit"] = ToUtf8(p.orbit);
        if (!p.backstory.empty()) c["backstory"] = p.backstory;
        for (const InvItem& it : p.items) {
            c["items"].push_back({{"id", it.id}, {"slot", static_cast<int>(it.slot)}, {"name", it.name}, {"text", it.text}, {"image", ToUtf8(it.image)}});
        }
        j["characters"].push_back(std::move(c));
    }
    std::error_code ec;
    const std::wstring dir = AppDataDir();
    fs::create_directories(dir, ec);
    const std::wstring path = dir + L"\\characters.json", tmp = path + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << j.dump(2, ' ', false, json::error_handler_t::replace);
    }
    fs::rename(tmp, path, ec);
}

std::string InventoryText(const CharacterProfile& p) {
    std::string out;
    for (int slot = 0; slot < 4; ++slot) {
        std::string list;
        for (const InvItem& it : p.items) {
            if (static_cast<int>(it.slot) != slot || Trim(it.name).empty()) continue;
            if (!list.empty()) list += ", ";
            list += NoEndPunct(it.name);
            if (!Trim(it.text).empty()) list += " (" + NoEndPunct(it.text) + ")";
        }
        if (list.empty()) continue;
        if (!out.empty()) out += "; ";
        if (slot == 3) out += "with " + list + " parked or standing nearby (a vehicle: never carried; used only if a relation says so)";
        else out += std::string(slot == 0 ? "wearing " : slot == 1 ? "holding " : "carrying ") + list;
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// Saved item pictures
std::wstring ItemsDir() { return AppDataDir() + L"\\items"; }

namespace {
std::map<std::string, std::string> LoadItemNames() {
    std::map<std::string, std::string> names;
    std::ifstream f(ItemsDir() + L"\\index.json", std::ios::binary);
    if (!f) return names;
    json j = json::parse(f, nullptr, false);
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (it.value().is_string()) names[it.key()] = it.value().get<std::string>();
        }
    }
    return names;
}

void SaveItemNames(const std::map<std::string, std::string>& names) {
    json j = json::object();
    for (const auto& kv : names) j[kv.first] = kv.second;
    std::ofstream out(ItemsDir() + L"\\index.json", std::ios::binary | std::ios::trunc);
    if (out) out << j.dump(2, ' ', false, json::error_handler_t::replace);
}

std::wstring UniquePath(const std::wstring& dir, const std::wstring& stem, const std::wstring& ext) {
    std::error_code ec;
    std::wstring path = dir + L"\\" + stem + ext;
    for (int i = 2; fs::exists(path, ec) && i < 10000; ++i) path = dir + L"\\" + stem + L" (" + std::to_wstring(i) + L")" + ext;
    return path;
}
} // namespace

std::vector<SavedItem> ListSavedItems() {
    std::vector<std::pair<long long, SavedItem>> found;
    const auto names = LoadItemNames();
    std::error_code ec;
    for (fs::directory_iterator it(ItemsDir(), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file() || !IsImageExt(it->path().extension().wstring())) continue;
        SavedItem s;
        s.path = it->path().wstring();
        const std::string file = ToUtf8(it->path().filename().wstring());
        auto n = names.find(file);
        s.name = n != names.end() ? n->second : ToUtf8(it->path().stem().wstring());
        found.push_back({FileUnixTime(s.path), std::move(s)});
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& c) { return a.first > c.first; });
    std::vector<SavedItem> out;
    for (auto& f : found) out.push_back(std::move(f.second));
    return out;
}

bool RenameSavedItem(const std::wstring& path, const std::string& name) {
    if (Trim(name).empty()) return false;
    auto names = LoadItemNames();
    names[ToUtf8(fs::path(path).filename().wstring())] = Trim(name);
    SaveItemNames(names);
    return true;
}

std::wstring StoreItemImage(const std::wstring& source, const std::string& name) {
    std::error_code ec;
    const std::wstring dir = ItemsDir();
    fs::create_directories(dir, ec);
    std::wstring ext = fs::path(source).extension().wstring();
    if (!IsImageExt(ext)) ext = L".png";
    std::wstring target;
    if (LowerW(fs::path(source).parent_path().wstring()) == LowerW(dir)) {
        target = source; // already in the folder
    } else {
        target = UniquePath(dir, ToWide(SafeFileName(name, "item")), ext);
        if (!CopyFileW(source.c_str(), target.c_str(), TRUE)) return {};
    }
    auto names = LoadItemNames();
    names[ToUtf8(fs::path(target).filename().wstring())] = Trim(name).empty() ? ToUtf8(fs::path(target).stem().wstring()) : Trim(name);
    SaveItemNames(names);
    return target;
}

bool DownloadItemImage(const std::string& url, const std::string& name, std::wstring& storedPath, std::string& error) {
    storedPath.clear();
    const std::wstring wurl = ToWide(Trim(url));
    wchar_t host[256] = {}, path[2048] = {}, extra[2048] = {};
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 2048;
    if (wurl.empty() || !WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc) || (uc.nScheme != INTERNET_SCHEME_HTTP && uc.nScheme != INTERNET_SCHEME_HTTPS)) {
        error = "That is not a web address (it should start with http:// or https://)";
        return false;
    }
    HINTERNET session = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) Try2Create", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) session = WinHttpOpen(L"Try2Create", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        error = "Could not start the download";
        return false;
    }
    WinHttpSetTimeouts(session, 10000, 10000, 20000, 30000);
    std::string body;
    bool ok = false;
    if (HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0)) {
        const std::wstring target = std::wstring(path) + extra;
        const DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
        if (HINTERNET req = WinHttpOpenRequest(conn, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags)) {
            WinHttpAddRequestHeaders(req, L"Accept: image/*,*/*;q=0.8", static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);
            if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
                DWORD status = 0, size = sizeof(status);
                WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
                if (status == 200) {
                    ok = true;
                    for (;;) {
                        DWORD avail = 0;
                        if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
                        std::string chunk(avail, '\0');
                        DWORD got = 0;
                        if (!WinHttpReadData(req, chunk.data(), avail, &got) || got == 0) break;
                        body.append(chunk, 0, got);
                        if (body.size() > 25u * 1024 * 1024) {
                            ok = false;
                            error = "That picture is too big (over 25 MB)";
                            break;
                        }
                    }
                } else {
                    error = "The website answered with error " + std::to_string(status);
                }
            } else {
                error = "Could not reach that address";
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(conn);
    }
    WinHttpCloseHandle(session);
    if (!ok) {
        if (error.empty()) error = "Could not download that address";
        return false;
    }
    const unsigned char* d = reinterpret_cast<const unsigned char*>(body.data());
    const wchar_t* ext = nullptr;
    if (body.size() > 12) {
        if (d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') ext = L".png";
        else if (d[0] == 0xFF && d[1] == 0xD8) ext = L".jpg";
        else if (d[0] == 'G' && d[1] == 'I' && d[2] == 'F') ext = L".gif";
        else if (d[0] == 'B' && d[1] == 'M') ext = L".bmp";
        else if (std::memcmp(d, "RIFF", 4) == 0 && std::memcmp(d + 8, "WEBP", 4) == 0) ext = L".webp";
    }
    if (!ext) {
        error = "That address is not a picture (it looks like a web page: right click the picture and copy its image address)";
        return false;
    }
    std::error_code ec;
    const std::wstring dir = ItemsDir();
    fs::create_directories(dir, ec);
    const std::wstring target = UniquePath(dir, ToWide(SafeFileName(name, "item")), ext);
    {
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "Could not save the picture";
            return false;
        }
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
    }
    auto names = LoadItemNames();
    names[ToUtf8(fs::path(target).filename().wstring())] = Trim(name).empty() ? ToUtf8(fs::path(target).stem().wstring()) : Trim(name);
    SaveItemNames(names);
    storedPath = target;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Prompts
namespace {

struct SceneParts {
    std::vector<const BoardNode*> chars, bgs, items, notes;
};

SceneParts PartsOf(const Board& b, int scene) {
    SceneParts p;
    for (const BoardNode& n : b.nodes) {
        if (n.scene != scene) continue;
        switch (n.kind) {
            case BoardKind::Character: p.chars.push_back(&n); break;
            case BoardKind::Background: p.bgs.push_back(&n); break;
            case BoardKind::Item: p.items.push_back(&n); break;
            case BoardKind::Note: p.notes.push_back(&n); break;
        }
    }
    // characters left to right, the way they sit on the board
    std::sort(p.chars.begin(), p.chars.end(), [](const BoardNode* a, const BoardNode* c) { return a->x < c->x; });
    return p;
}

std::string Describe(const Board& b, const BoardNode& n) {
    const std::string t = NoEndPunct(n.text);
    return t.empty() ? b.DisplayName(n) : t;
}

std::string InventoryOf(const BoardNode& n) {
    if (n.kind != BoardKind::Character) return {};
    const CharacterProfile* p = ProfileOf(n);
    return p ? InventoryText(*p) : std::string();
}

std::string RelationSentences(const Board& b, int scene) {
    std::string out;
    for (const BoardLink& l : b.links) {
        const BoardNode *f = b.Find(l.from), *t = b.Find(l.to);
        if (!f || !t || f->scene != scene) continue;
        if (!out.empty()) out += ", ";
        out += b.DisplayName(*f) + " " + l.label + " " + b.DisplayName(*t);
    }
    return out;
}

struct Sentences {
    std::string out;
    void Add(const std::string& s) {
        const std::string c = Trim(s);
        if (c.empty()) return;
        if (!out.empty()) out += ' ';
        out += c;
        if (out.back() != '.' && out.back() != '!' && out.back() != '?') out += '.';
    }
};

// "who is who" sentences for the cards of a scene that were not introduced yet
void IntroSentences(const Board& b, int scene, std::set<std::string>* introduced, Sentences& s) {
    const SceneParts p = PartsOf(b, scene);
    auto fresh = [&](const BoardNode& n) { return !introduced || introduced->insert(ElementKey(n)).second; };
    for (const BoardNode* n : p.bgs) {
        if (fresh(*n)) s.Add("Setting: " + Describe(b, *n));
    }
    for (const BoardNode* n : p.chars) {
        if (!fresh(*n)) continue;
        std::string text = NoEndPunct(n->text);
        const std::string inv = InventoryOf(*n);
        if (!inv.empty()) text += (text.empty() ? "" : "; ") + inv;
        if (!text.empty()) s.Add(b.DisplayName(*n) + ": " + text);
    }
    for (const BoardNode* n : p.items) {
        if (fresh(*n) && !NoEndPunct(n->text).empty()) s.Add(b.DisplayName(*n) + ": " + Describe(b, *n));
    }
}

// What happens in a scene: its relations, its note, and the notes of its cards.
std::string ActionOf(const Board& b, int scene) {
    std::string out = RelationSentences(b, scene);
    const std::string text = NoEndPunct(b.scenes[scene].text);
    if (!text.empty()) out += (out.empty() ? "" : ". ") + text;
    for (const BoardNode* n : PartsOf(b, scene).notes) {
        const std::string t = NoEndPunct(n->text);
        if (!t.empty()) out += (out.empty() ? "" : ". ") + t;
    }
    return out;
}

bool SceneHasContent(const Board& b, int scene) {
    for (const BoardNode& n : b.nodes) {
        if (n.scene == scene) return true;
    }
    return !Trim(b.scenes[scene].text).empty();
}

std::string RangeText(const Board& b, int scene) { return SecText(b.scenes[scene].start) + "-" + SecText(b.scenes[scene].end) + " s"; }

std::vector<int> VideoScenes(const Board& b) {
    std::vector<int> out;
    for (int i = 0; i < static_cast<int>(b.scenes.size()); ++i) {
        if (SceneHasContent(b, i)) out.push_back(i);
    }
    return out;
}

} // namespace

std::string AssembleBoardPrompt(const Board& b, const BoardPromptOptions& o) {
    const int scene = std::clamp(o.scene, 0, static_cast<int>(b.scenes.size()) - 1);
    if (o.format == 1) { // tag list: phrases separated by commas
        const SceneParts p = PartsOf(b, scene);
        std::vector<std::string> tags;
        auto add = [&](const std::string& t) {
            const std::string c = NoEndPunct(t);
            if (!c.empty() && std::find(tags.begin(), tags.end(), c) == tags.end()) tags.push_back(c);
        };
        if (p.chars.size() == 1) add("solo");
        else if (p.chars.size() > 1) add(std::to_string(p.chars.size()) + " characters");
        for (const BoardNode* n : p.chars) {
            add(Describe(b, *n));
            if (const CharacterProfile* prof = ProfileOf(*n)) {
                for (const InvItem& it : prof->items) {
                    if (!Trim(it.name).empty()) add(it.slot == InvSlot::Vehicle ? it.name + " nearby" : std::string(it.slot == InvSlot::Wearing ? "wearing " : it.slot == InvSlot::Holding ? "holding " : "carrying ") + it.name);
                }
            }
        }
        for (const BoardLink& l : b.links) {
            const BoardNode *f = b.Find(l.from), *t = b.Find(l.to);
            if (f && t && f->scene == scene) add(b.DisplayName(*f) + " " + l.label + " " + b.DisplayName(*t));
        }
        for (const BoardNode* n : p.items) add(Describe(b, *n));
        for (const BoardNode* n : p.bgs) add(Describe(b, *n));
        for (const BoardNode* n : p.notes) add(n->text);
        add(b.scenes[scene].text);
        {
            const std::string cam = CameraText(b.scenes[scene], false);
            size_t pos = 0;
            while (pos < cam.size()) { // each camera / lighting phrase is one tag
                size_t next = cam.find(", ", pos);
                if (next == std::string::npos) next = cam.size();
                add(cam.substr(pos, next - pos));
                pos = next + 2;
            }
        }
        if (!o.extra.empty()) add(o.extra);
        if (!o.style.empty()) add(o.style);
        std::string out;
        for (const std::string& t : tags) out += (out.empty() ? "" : ", ") + t;
        return out;
    }

    Sentences s;
    if (o.format == 2) {
        const std::vector<int> used = VideoScenes(b);
        std::set<std::string> introduced;
        std::string previousBackground;
        s.Add("A " + SecText(b.TotalSec()) + "-second video" + (used.size() > 1 ? " in " + std::to_string(used.size()) + " scenes" : std::string()));
        for (int i : used) {
            const SceneParts p = PartsOf(b, i);
            const std::string bg = p.bgs.empty() ? std::string() : ElementKey(*p.bgs.front());
            if (!bg.empty() && !previousBackground.empty() && bg != previousBackground) s.Add("Cut to the new setting");
            if (!bg.empty()) previousBackground = bg;
            IntroSentences(b, i, &introduced, s);
            const std::string action = ActionOf(b, i);
            const std::string cam = CameraText(b.scenes[i], true);
            if (!action.empty() || !cam.empty()) s.Add(RangeText(b, i) + (cam.empty() ? std::string() : " [" + cam + "]") + ": " + action);
        }
    } else {
        IntroSentences(b, scene, nullptr, s);
        const std::string action = ActionOf(b, scene);
        if (!action.empty()) s.Add(action);
        const std::string cam = CameraText(b.scenes[scene], false);
        if (!cam.empty()) s.Add("Camera and lighting: " + cam);
    }
    if (!Trim(o.extra).empty()) s.Add(o.extra);
    if (!o.style.empty()) s.Add("Style: " + o.style);
    return s.out;
}

std::string BuildBoardBrief(const Board& b, const BoardPromptOptions& o, std::vector<std::wstring>& attached) {
    attached.clear();
    const bool video = o.format == 2;
    std::vector<int> scenes = video ? VideoScenes(b) : std::vector<int>{std::clamp(o.scene, 0, static_cast<int>(b.scenes.size()) - 1)};
    std::string kind;
    switch (o.format) {
        case 1: kind = "ONE comma-separated line of lowercase Danbooru-style tags (subject count first, then characters, their clothing and "
                       "objects, actions and interactions, setting, lighting, composition). No sentences."; break;
        case 2:
            kind = "ONE time-coded video script of " + SecText(b.TotalSec()) + " seconds for an image-to-video generator: one line per scene of the "
                   "timeline, each starting with its time range exactly as given (for example \"0-3 s:\"), describing what happens in that scene, how the "
                   "characters move and interact, and how the camera moves. Where the setting changes between scenes, describe the cut and the new "
                   "setting; where it stays, keep the same place and characters going, only changing the action.";
            break;
        default: kind = "ONE cohesive paragraph of natural language for a text-to-image model (Flux, SDXL, Midjourney) that recreates the whole scene."; break;
    }
    bool hasBackground = false;
    for (const BoardNode& n : b.nodes) {
        if (n.kind == BoardKind::Background && std::find(scenes.begin(), scenes.end(), n.scene) != scenes.end()) hasBackground = true;
    }
    std::string out = "You write prompts for generative image and video models. Below is a \"vision board\": the elements of ONE " +
                      std::string(video ? "video" : "scene") + ", each with a role and a description" +
                      std::string(o.attachImages ? " (the reference pictures are attached, numbered in the same order)" : "") +
                      ", what each character wears and carries, and " + (video ? "the timeline of scenes with the relations in each" : "the relations between them") +
                      ". Combine everything into " + kind + "\n\n";
    out += "ELEMENTS:\n";
    int number = 0;
    size_t pictures = 0;
    std::set<std::string> listed;
    auto picture = [&](const std::wstring& image) -> std::string {
        if (!o.attachImages || image.empty() || pictures >= 10) return {};
        attached.push_back(image);
        ++pictures;
        return " (picture " + std::to_string(attached.size()) + ")";
    };
    for (int sc : scenes) {
        for (const BoardNode& n : b.nodes) {
            if (n.scene != sc || !listed.insert(ElementKey(n)).second) continue;
            ++number;
            out += std::to_string(number) + ". " + BoardKindName(n.kind) + " \"" + b.DisplayName(n) + "\"" + picture(n.image);
            const std::string t = Trim(n.text);
            out += t.empty() ? ": (no description: use the picture)\n" : ": " + t + "\n";
            if (n.kind == BoardKind::Character) {
                if (const CharacterProfile* p = ProfileOf(n)) {
                    for (const InvItem& it : p->items) {
                        if (Trim(it.name).empty()) continue;
                        out += std::string("   - ") + (it.slot == InvSlot::Wearing ? "wears" : it.slot == InvSlot::Holding ? "holds" : it.slot == InvSlot::Carrying ? "carries" : "has nearby (a vehicle: parked or standing, never carried; rides or pilots it only if a relation says so)") + ": " +
                               it.name + (Trim(it.text).empty() ? "" : " (" + Trim(it.text) + ")") + picture(it.image) + "\n";
                    }
                }
            }
        }
    }
    if (video) {
        out += "\nTIMELINE (" + SecText(b.TotalSec()) + " seconds; the relations are exact, read each as \"<first> <relation> <second>\"):\n";
        for (int sc : scenes) {
            const SceneParts p = PartsOf(b, sc);
            out += "- " + RangeText(b, sc) + ": ";
            std::string present;
            for (const BoardNode* n : p.chars) present += (present.empty() ? "" : ", ") + b.DisplayName(*n);
            for (const BoardNode* n : p.items) present += (present.empty() ? "" : ", ") + b.DisplayName(*n);
            if (!p.bgs.empty()) out += "setting: " + b.DisplayName(*p.bgs.front()) + "; ";
            if (!present.empty()) out += "present: " + present + "; ";
            const std::string rel = RelationSentences(b, sc);
            out += rel.empty() ? "no relations; " : "relations: " + rel + "; ";
            const std::string cam = CameraText(b.scenes[sc], true);
            if (!cam.empty()) out += "camera and lighting: " + cam + "; ";
            const std::string note = Trim(b.scenes[sc].text);
            if (!note.empty()) out += "what happens: " + note;
            out += "\n";
        }
    } else {
        bool anyLink = false;
        for (const BoardLink& l : b.links) {
            const BoardNode *f = b.Find(l.from), *t = b.Find(l.to);
            if (!f || !t || f->scene != scenes[0]) continue;
            if (!anyLink) out += "\nRELATIONS (who does what to whom; each one is exact, read it as \"<first> <relation> <second>\"):\n";
            anyLink = true;
            out += "- " + b.DisplayName(*f) + " " + l.label + " " + b.DisplayName(*t) + "\n";
        }
        const std::string note = Trim(b.scenes[scenes[0]].text);
        if (!note.empty()) out += "\nWHAT HAPPENS: " + note + "\n";
        const std::string cam = CameraText(b.scenes[scenes[0]], false);
        if (!cam.empty()) out += "\nCAMERA AND LIGHTING (use exactly this): " + cam + "\n";
    }
    if (!Trim(o.extra).empty()) out += "\nEXTRA DIRECTION: " + Trim(o.extra) + "\n";
    if (!o.style.empty()) out += "\nSTYLE: write it as if it were rendered as " + o.style + ".\n";
    out += "\nRules (follow them, but never mention or paraphrase them in your answer):\n"
           "- Keep every character's own appearance, clothing and items exactly as described.\n"
           "- Show each relation as a clear physical action between exactly the elements it names. \"fights against\" / \"attacks\" / \"strikes down\" "
           "mean opponents; \"fights alongside\" / \"teams up with\" / \"defends\" mean allies on the same side, who must never be shown fighting each "
           "other. Two characters with no relation between them just share the scene. Do not add actions or relations that are not listed.\n"
           "- Do not call anything \"Character 1\", \"Item 2\" or by an element number: use the name when it has one, otherwise a short phrase taken "
           "from its description (for example \"the spiky-haired young man\").\n"
           "- Keep everything physically plausible: a person can only hold or carry things they could really lift. Vehicles, furniture and anything "
           "bigger than a person are never carried, held or worn: show them parked or standing nearby, or ridden / driven / piloted when the relations say "
           "so; without such a relation a vehicle just sits parked nearby and nobody is in it. Clothes are worn, small objects are held or carried in a hand, a pocket or a bag.\n"
           "- Respect the camera framing and lighting when they are given: describe only what is inside the frame (a waist-up or close-up shot shows "
           "no legs or feet, a full body shot shows the whole figure), use the stated angle, lens and lighting, and keep them the same from one scene "
           "to the next unless a scene gives its own.\n"
           "- Do not add characters or objects that are not on the board.\n" +
           std::string(hasBackground ? "- Put the background element behind everything.\n"
                                     : "- There is no background element: do not invent a place, city, sky or scenery; leave the setting plain and unspecified.\n") +
           "Output only the prompt itself, with no preamble, title, notes or quotes.";
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
std::wstring BoardsDir() { return AppDataDir() + L"\\boards"; }

std::wstring DefaultExportDir() {
    PWSTR p = nullptr;
    std::wstring base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &p)) && p) base = p;
    if (p) CoTaskMemFree(p);
    if (base.empty()) base = AppDataDir();
    return base + L"\\Vision Board Scenes";
}

BoardExportResult ExportBoardForGeneration(const Board& b, const std::string& boardName, const std::wstring& rootDir, const std::string& finalPrompt,
                                           const BoardPromptOptions& options) {
    BoardExportResult res;
    std::error_code ec;
    const std::string folderName = SafeFileName(boardName, "Untitled board");
    res.folder = rootDir + L"\\" + ToWide(folderName);
    fs::create_directories(res.folder, ec);
    if (ec) {
        res.error = "Could not create the folder " + ToUtf8(res.folder);
        return res;
    }
    auto copyInto = [&](const std::wstring& dir, int& index, const std::string& label, const std::wstring& source) {
        if (source.empty()) return;
        std::wstring ext = fs::path(source).extension().wstring();
        if (!IsImageExt(ext)) ext = L".png";
        char num[16];
        std::snprintf(num, sizeof(num), "%02d ", ++index);
        const std::wstring target = dir + L"\\" + ToWide(num + SafeFileName(label, "picture")) + ext;
        if (CopyFileW(source.c_str(), target.c_str(), FALSE)) {
            res.files.push_back(target);
            ++res.pictures;
        }
    };
    // every picture once (backgrounds, then characters with their items, then loose items), in order of first appearance
    {
        std::set<std::string> done;
        int index = 0;
        auto addOnce = [&](const std::string& label, const std::wstring& image) {
            if (image.empty() || !done.insert(ToUtf8(LowerW(image))).second) return;
            copyInto(res.folder, index, label, image);
        };
        for (int pass = 0; pass < 3; ++pass) {
            for (int sc = 0; sc < static_cast<int>(b.scenes.size()); ++sc) {
                const SceneParts p = PartsOf(b, sc);
                if (pass == 0) {
                    for (const BoardNode* n : p.bgs) addOnce("background " + b.DisplayName(*n), n->image);
                } else if (pass == 1) {
                    for (const BoardNode* n : p.chars) {
                        addOnce("character " + b.DisplayName(*n), n->image);
                        if (const CharacterProfile* prof = ProfileOf(*n)) {
                            for (const InvItem& it : prof->items) addOnce("item " + b.DisplayName(*n) + " - " + it.name, it.image);
                        }
                    }
                } else {
                    for (const BoardNode* n : p.items) addOnce("item " + b.DisplayName(*n), n->image);
                }
            }
        }
    }
    const bool many = b.scenes.size() > 1;
    auto writeText = [&](const std::wstring& path, const std::string& text) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (out) out << text << "\n";
    };
    const std::string draft = AssembleBoardPrompt(b, options);
    writeText(res.folder + L"\\prompt.txt", Trim(finalPrompt).empty() ? draft : Trim(finalPrompt));
    writeText(res.folder + L"\\prompt (quick draft).txt", draft);
    if (many) { // one prompt per beat, for generating each beat as its own image
        for (int sc = 0; sc < static_cast<int>(b.scenes.size()); ++sc) {
            if (!SceneHasContent(b, sc)) continue;
            BoardPromptOptions o = options;
            o.scene = sc;
            if (o.format == 2) o.format = 0;
            char num[16];
            std::snprintf(num, sizeof(num), "Scene %d ", sc + 1);
            writeText(res.folder + L"\\" + ToWide(std::string(num) + "(" + RangeText(b, sc) + ") prompt.txt"), AssembleBoardPrompt(b, o));
        }
    }
    SaveBoard(b, res.folder + L"\\board.json");
    return res;
}

bool SaveBoard(const Board& b, const std::wstring& path) {
    json j;
    j["nextId"] = b.nextId;
    j["scenes"] = json::array();
    for (const BoardScene& s : b.scenes) j["scenes"].push_back({{"name", s.name}, {"start", s.start}, {"end", s.end}, {"text", s.text}, {"shot", s.shot}, {"angle", s.angle},
                                                       {"lens", s.lens}, {"movement", s.movement}, {"lighting", s.lighting}, {"dof", s.dof}});
    j["view"] = {{"x", b.panX}, {"y", b.panY}, {"zoom", b.zoom}};
    j["nodes"] = json::array();
    for (const BoardNode& n : b.nodes) {
        j["nodes"].push_back({{"id", n.id}, {"kind", static_cast<int>(n.kind)}, {"scene", n.scene}, {"x", n.x}, {"y", n.y}, {"w", n.w},
                              {"image", ToUtf8(n.image)}, {"title", n.title}, {"text", n.text}, {"profile", n.profile}, {"textH", n.textH}});
    }
    j["links"] = json::array();
    for (const BoardLink& l : b.links) j["links"].push_back({{"id", l.id}, {"from", l.from}, {"to", l.to}, {"label", l.label}});
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    const std::wstring tmp = path + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << j.dump(2, ' ', false, json::error_handler_t::replace);
    }
    fs::rename(tmp, path, ec);
    return !ec;
}

bool LoadBoard(const std::wstring& path, Board& b) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    json j = json::parse(f, nullptr, false);
    if (!j.is_object()) return false;
    Board out;
    out.nextId = j.value("nextId", 1);
    if (j.contains("scenes") && j["scenes"].is_array() && !j["scenes"].empty()) {
        out.scenes.clear();
        float next = 0;
        for (const json& s : j["scenes"]) {
            BoardScene sc;
            if (s.is_string()) { // an older board: scenes were only names
                sc.name = s.get<std::string>();
                sc.start = next;
                sc.end = next + 3;
            } else if (s.is_object()) {
                sc.name = s.value("name", std::string());
                sc.start = std::max(0.0f, s.value("start", next));
                sc.end = std::max(sc.start + 0.1f, s.value("end", sc.start + 3.0f));
                sc.text = s.value("text", std::string());
                sc.shot = s.value("shot", 0);
                sc.angle = s.value("angle", 0);
                sc.lens = s.value("lens", 0);
                sc.movement = s.value("movement", 0);
                sc.lighting = s.value("lighting", 0);
                sc.dof = s.value("dof", 0);
            }
            next = sc.end;
            out.scenes.push_back(std::move(sc));
        }
    }
    const int sceneCount = static_cast<int>(out.scenes.size());
    if (j.contains("view") && j["view"].is_object()) {
        out.panX = j["view"].value("x", 0.0f);
        out.panY = j["view"].value("y", 0.0f);
        out.zoom = std::clamp(j["view"].value("zoom", 1.0f), 0.1f, 4.0f);
    }
    if (j.contains("nodes") && j["nodes"].is_array()) {
        for (const json& n : j["nodes"]) {
            BoardNode node;
            node.id = n.value("id", 0);
            node.kind = static_cast<BoardKind>(std::clamp(n.value("kind", 0), 0, 3));
            node.scene = std::clamp(n.value("scene", 0), 0, sceneCount - 1);
            node.x = n.value("x", 0.0f);
            node.y = n.value("y", 0.0f);
            node.w = std::clamp(n.value("w", 230.0f), 140.0f, 800.0f);
            node.image = ToWide(n.value("image", std::string()));
            node.title = n.value("title", std::string());
            node.text = n.value("text", std::string());
            node.profile = n.value("profile", std::string());
            node.textH = std::clamp(n.value("textH", 0.0f), 0.0f, 1500.0f);
            out.nextId = std::max(out.nextId, node.id + 1);
            out.nodes.push_back(std::move(node));
        }
    }
    {   // a board from before cards had their own inventory: give each character picture one inventory of this board (a copy of what it had)
        std::map<std::string, std::string> byPicture;
        for (BoardNode& node : out.nodes) {
            if (node.kind != BoardKind::Character || node.image.empty() || !node.profile.empty()) continue;
            const std::string base = CharacterKey(node.image);
            auto it = byPicture.find(base);
            if (it == byPicture.end()) {
                const std::string key = base + "#" + CharacterKey(path);
                Instance(key, node.image, true);
                it = byPicture.emplace(base, key).first;
            }
            node.profile = it->second;
        }
    }
    if (j.contains("links") && j["links"].is_array()) {
        for (const json& l : j["links"]) {
            BoardLink link;
            link.id = l.value("id", 0);
            link.from = l.value("from", 0);
            link.to = l.value("to", 0);
            link.label = l.value("label", std::string());
            if (out.Find(link.from) && out.Find(link.to)) {
                out.nextId = std::max(out.nextId, link.id + 1);
                out.links.push_back(std::move(link));
            }
        }
    }
    out.SortScenes(nullptr);
    b = std::move(out);
    return true;
}

std::vector<std::wstring> ListBoards() {
    std::vector<std::pair<long long, std::wstring>> found;
    std::error_code ec;
    for (fs::directory_iterator it(BoardsDir(), ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file() && _wcsicmp(it->path().extension().c_str(), L".json") == 0 && it->path().stem() != L"_current") {
            found.push_back({FileUnixTime(it->path().wstring()), it->path().wstring()});
        }
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& c) { return a.first > c.first; });
    std::vector<std::wstring> out;
    for (auto& f : found) out.push_back(f.second);
    return out;
}

std::vector<BoardInfo> ScanBoards() {
    std::vector<BoardInfo> out;
    for (const std::wstring& path : ListBoards()) {
        std::ifstream f(path, std::ios::binary);
        if (!f) continue;
        json j = json::parse(f, nullptr, false);
        if (!j.is_object()) continue;
        BoardInfo info;
        info.path = path;
        info.name = ToUtf8(fs::path(path).stem().wstring());
        info.modified = FileUnixTime(path);
        if (j.contains("scenes") && j["scenes"].is_array()) {
            info.scenes = std::max(1, static_cast<int>(j["scenes"].size()));
            for (const json& s : j["scenes"]) {
                if (s.is_object()) info.totalSec = std::max(info.totalSec, s.value("end", 0.0f));
            }
        }
        if (j.contains("links") && j["links"].is_array()) info.relations = static_cast<int>(j["links"].size());
        std::set<std::string> seen;
        std::wstring anyImage;
        if (j.contains("nodes") && j["nodes"].is_array()) {
            info.cards = static_cast<int>(j["nodes"].size());
            for (const json& n : j["nodes"]) {
                const std::wstring image = ToWide(n.value("image", std::string()));
                if (image.empty()) continue;
                if (anyImage.empty()) anyImage = image;
                if (n.value("kind", 0) != 0) continue;
                BoardCharRef c;
                c.key = CharacterKey(image);
                if (!seen.insert(c.key).second) continue;
                c.image = image;
                c.name = Trim(n.value("title", std::string()));
                if (c.name.empty()) {
                    if (const CharacterProfile* p = FindProfile(image)) c.name = p->name;
                }
                if (c.name.empty()) c.name = ToUtf8(fs::path(image).stem().wstring());
                if (info.thumb.empty()) info.thumb = image;
                info.characters.push_back(std::move(c));
            }
        }
        if (info.thumb.empty()) info.thumb = anyImage;
        out.push_back(std::move(info));
    }
    return out;
}
