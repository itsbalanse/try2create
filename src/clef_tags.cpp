#include "clef_tags.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <windows.h>

#include "clef.h"
#include "director.h"
#include "json.hpp"
#include "vision_prompt.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

struct Stored {
    ClefTags tags;
    long long mtime = 0;
};

std::mutex g_mutex;
std::unordered_map<std::wstring, Stored> g_tags; // lowercased path
bool g_loaded = false;
std::atomic<unsigned> g_version{1};

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

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

fs::path TagsFile() {
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH * 2);
    return fs::path(std::wstring(buf, n)) / L"Try2Create" / L"clef_tags.json";
}

void LoadLocked() {
    if (g_loaded) return;
    g_loaded = true;
    std::ifstream f(TagsFile(), std::ios::binary);
    if (!f) return;
    json j = json::parse(f, nullptr, false);
    if (!j.is_object()) return;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (!it.value().is_array() || it.value().size() < 6) continue;
        Stored s;
        s.mtime = it.value()[0].get<long long>();
        s.tags = {it.value()[1].get<int>(), it.value()[2].get<int>(), it.value()[3].get<int>(), it.value()[4].get<int>(), it.value()[5].get<int>()};
        g_tags[Lower(ToWide(it.key()))] = s;
    }
}

void SaveLocked() {
    json j = json::object();
    for (const auto& kv : g_tags) {
        const ClefTags& t = kv.second.tags;
        j[ToUtf8(kv.first)] = json::array({kv.second.mtime, t.genre, t.mood, t.setting, t.era, t.subject});
    }
    std::error_code ec;
    fs::create_directories(TagsFile().parent_path(), ec);
    std::ofstream out(TagsFile(), std::ios::binary | std::ios::trunc);
    if (out) out << j.dump(-1, ' ', false, json::error_handler_t::replace);
}

const std::vector<std::string>& Settings() {
    static const std::vector<std::string> v = {"a city street", "indoors at home", "a cafe, shop or restaurant", "an office, school or workplace", "nature: forest, mountains or countryside", "a beach, sea or lake",
                                               "a desert or wasteland", "a snowy place", "a castle, ruin or dungeon", "a fantasy landscape", "outer space or a spaceship", "a futuristic city",
                                               "a studio with a plain backdrop", "a bedroom or bathroom", "a vehicle", "no clear setting or an abstract one"};
    return v;
}

const std::vector<std::string>& Subjects() {
    static const std::vector<std::string> v = {"a single person (portrait)", "two people", "a group of people", "a full-body character", "a close-up of a face", "an animal or creature", "a landscape or scenery",
                                               "an object or still life", "a vehicle or machine", "food", "a building or interior", "an abstract picture or pattern"};
    return v;
}

const std::vector<std::string>& Eras() {
    static const std::vector<std::string> v = EraNames();
    return v;
}

// ---------------------------------------------------------------- the job
std::thread g_thread;
std::atomic<bool> g_running{false}, g_stop{false};
std::atomic<int> g_done{0}, g_total{0};
std::mutex g_phaseMutex;
std::string g_phase;
void SetPhase(const std::string& s) {
    std::lock_guard<std::mutex> lock(g_phaseMutex);
    g_phase = s;
}

void Run(std::vector<ClefTagInput> items, std::string comfyUrl, bool onlyNew) {
    std::vector<ClefTagInput> todo;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        LoadLocked();
        for (const ClefTagInput& in : items) {
            auto it = g_tags.find(Lower(in.imagePath));
            if (!(onlyNew && it != g_tags.end() && it->second.mtime == in.mtime)) todo.push_back(in);
        }
    }
    g_total = static_cast<int>(todo.size());
    g_done = 0;
    if (todo.empty()) {
        SetPhase("Nothing new to tag");
        g_running = false;
        return;
    }
    const ClefSession session = ClefAcquire(comfyUrl, SetPhase);
    if (!session.ok) {
        SetPhase("Clef could not be started (Clef mode tab > Set up Clef)");
        g_running = false;
        return;
    }
    auto choice = [](const char* id, const char* text, const std::vector<std::string>& opts) {
        ClefQuestion q;
        q.id = id;
        q.instructions = text;
        q.options = opts;
        return q;
    };
    std::vector<ClefQuestion> qs = {choice("genre", "What genre does this picture or scene belong to?", GenreNames()), choice("mood", "What mood or kind of picture is it?", MoodNames()),
                                    choice("setting", "Where does it take place?", Settings()), choice("era", "In which era or world is it set?", Eras()), choice("subject", "What is the picture of?", Subjects())};
    int tagged = 0;
    for (const ClefTagInput& in : todo) {
        if (g_stop) break;
        SetPhase("Clef is tagging the library");
        const std::string url = ImageDataUrl(in.imagePath, 768);
        std::vector<ClefAnswer> a;
        std::vector<std::string> imgs;
        if (!url.empty()) imgs.push_back(url);
        std::string state = in.text.size() > 1500 ? in.text.substr(0, 1500) : in.text;
        if (state.empty()) state = "A picture.";
        if (imgs.empty() && in.text.empty()) {
            ++g_done;
            continue;
        }
        if (!ClefAsk(state, imgs, qs, a, nullptr) || a.size() != qs.size()) {
            ++g_done;
            continue;
        }
        auto best = [](const ClefAnswer& x) { return static_cast<int>(std::max_element(x.probs.begin(), x.probs.end()) - x.probs.begin()); };
        Stored s;
        s.mtime = in.mtime;
        s.tags = {best(a[0]), best(a[1]), best(a[2]), best(a[3]), best(a[4])};
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_tags[Lower(in.imagePath)] = s;
            ++g_version;
            if (++tagged % 25 == 0) SaveLocked();
        }
        ++g_done;
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        SaveLocked();
    }
    SetPhase("Unloading Clef...");
    ClefRelease(session, true);
    SetPhase("Clef tagged " + std::to_string(tagged) + " pictures");
    g_running = false;
}

struct Joiner {
    ~Joiner() {
        g_stop = true;
        if (g_thread.joinable()) g_thread.join();
    }
} g_joiner;

} // namespace

const std::vector<std::string>& ClefTagNames(int kind) {
    switch (kind) {
        case 0: return GenreNames();
        case 1: return MoodNames();
        case 2: return Settings();
        case 3: return Eras();
        default: return Subjects();
    }
}

const char* ClefTagKindName(int kind) {
    static const char* n[] = {"Genre", "Mood", "Setting", "Era", "Subject"};
    return n[std::clamp(kind, 0, 4)];
}

bool ClefTagsFor(const std::wstring& imagePath, ClefTags& out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadLocked();
    auto it = g_tags.find(Lower(imagePath));
    if (it == g_tags.end()) return false;
    out = it->second.tags;
    return true;
}

std::string ClefTagLine(const std::wstring& imagePath) {
    ClefTags t;
    if (!ClefTagsFor(imagePath, t)) return {};
    std::string s;
    auto add = [&](int kind, int idx) {
        const std::vector<std::string>& names = ClefTagNames(kind);
        if (idx >= 0 && idx < static_cast<int>(names.size())) s += (s.empty() ? "" : "  |  ") + names[idx];
    };
    add(0, t.genre);
    add(1, t.mood);
    add(2, t.setting);
    add(3, t.era);
    add(4, t.subject);
    return s;
}

unsigned ClefTagsVersion() { return g_version.load(); }

void ClefTagStart(const std::vector<ClefTagInput>& items, const std::string& comfyUrl, bool onlyNew) {
    if (g_running) return;
    if (g_thread.joinable()) g_thread.join();
    g_running = true;
    g_stop = false;
    g_done = 0;
    g_total = static_cast<int>(items.size());
    SetPhase("Starting...");
    g_thread = std::thread(Run, items, comfyUrl, onlyNew);
}

void ClefTagStop() { g_stop = true; }
bool ClefTagRunning() { return g_running.load(); }

void ClefTagProgress(int& done, int& total, std::string& phase) {
    done = g_done.load();
    total = g_total.load();
    std::lock_guard<std::mutex> lock(g_phaseMutex);
    phase = g_phase;
}
