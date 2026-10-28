#include "clef_grades.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <windows.h>

#include "app_state.h"
#include "clef.h"
#include "imgui.h"
#include "json.hpp"
#include "vision_prompt.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

struct Grade {
    long long mtime = 0;
    unsigned long long size = 0;
    float nsfw = -1.0f, quality = -1.0f;
    int style = -1;
};

std::mutex g_mutex;
std::unordered_map<std::wstring, Grade> g_grades; // keyed by lowercased path
bool g_loaded = false;
unsigned g_version = 0; // bumped when grades are added: the pump re-applies

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

fs::path GradesFile() {
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH * 2);
    return fs::path(std::wstring(buf, n)) / L"Try2Create" / L"clef_grades.json";
}

void LoadLocked() {
    if (g_loaded) return;
    g_loaded = true;
    std::ifstream f(GradesFile(), std::ios::binary);
    if (!f) return;
    json j = json::parse(f, nullptr, false);
    if (!j.is_object()) return;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (!it.value().is_object()) continue;
        Grade g;
        g.mtime = it.value().value("m", 0LL);
        g.size = it.value().value("s", 0ULL);
        g.nsfw = it.value().value("n", -1.0f);
        g.quality = it.value().value("q", -1.0f);
        g.style = it.value().value("t", -1);
        g_grades[Lower(ToWide(it.key()))] = g;
    }
}

void SaveLocked() {
    json j = json::object();
    for (const auto& kv : g_grades) j[ToUtf8(kv.first)] = {{"m", kv.second.mtime}, {"s", kv.second.size}, {"n", kv.second.nsfw}, {"q", kv.second.quality}, {"t", kv.second.style}};
    std::error_code ec;
    fs::create_directories(GradesFile().parent_path(), ec);
    std::ofstream out(GradesFile(), std::ios::binary | std::ios::trunc);
    if (out) out << j.dump(-1, ' ', false, json::error_handler_t::replace);
}

// ---------------------------------------------------------------- the job
std::thread g_thread;
std::atomic<bool> g_running{false}, g_stop{false};
std::atomic<int> g_done{0}, g_total{0};
std::mutex g_phaseMutex;
std::string g_phase, g_finished; // g_finished: a message for the status line, once
void SetPhase(const std::string& s) {
    std::lock_guard<std::mutex> lock(g_phaseMutex);
    g_phase = s;
}

const std::vector<ClefQuestion>& Questions() {
    static const std::vector<ClefQuestion> qs = {
        {"nsfw", "How sexually explicit is this picture?",
         {"completely safe: nothing sexual, everyone fully dressed", "mildly suggestive: swimwear, bare shoulders, cleavage or a flirty pose", "sexy or revealing: lingerie, partial nudity or an erotic mood",
          "explicit nudity: exposed breasts or genitals", "explicit sexual activity"},
         ClefType::Score},
        {"quality", "How good is this picture as an image: composition, lighting, colour, detail, anatomy and overall appeal?",
         {"very poor: broken or ugly", "poor", "below average", "average", "decent", "good", "very good", "excellent", "outstanding", "a masterpiece"}, ClefType::Score},
        {"style", "What kind of picture is this?",
         {"a realistic photograph", "anime or manga style", "a cartoon or comic", "a 3D render", "a painting or digital art", "pixel art", "a sketch or line art"}, ClefType::Choice}};
    return qs;
}

void Run(std::vector<ClefGradeInput> items, std::string comfyUrl, bool onlyNew) {
    std::vector<ClefGradeInput> todo;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        LoadLocked();
        for (const ClefGradeInput& in : items) {
            auto it = g_grades.find(Lower(in.path));
            const bool have = it != g_grades.end() && it->second.mtime == in.mtime && it->second.size == in.size;
            if (!(onlyNew && have)) todo.push_back(in);
        }
    }
    g_total = static_cast<int>(todo.size());
    g_done = 0;
    if (todo.empty()) {
        std::lock_guard<std::mutex> lock(g_phaseMutex);
        g_finished = "Clef: nothing new to grade";
        g_running = false;
        return;
    }
    const ClefSession session = ClefAcquire(comfyUrl, SetPhase);
    if (!session.ok) {
        std::lock_guard<std::mutex> lock(g_phaseMutex);
        g_finished = "Clef could not be started (see the Clef mode tab: Set up Clef)";
        g_phase.clear();
        g_running = false;
        return;
    }
    int graded = 0, failed = 0;
    for (const ClefGradeInput& in : todo) {
        if (g_stop) break;
        SetPhase("Clef is grading pictures");
        const std::string url = ImageDataUrl(in.path, 896);
        std::vector<ClefAnswer> answers;
        if (url.empty() || !ClefAsk("Grade this picture.", {url}, Questions(), answers, nullptr) || answers.size() != 3) {
            ++failed;
            ++g_done;
            continue;
        }
        Grade g;
        g.mtime = in.mtime;
        g.size = in.size;
        const std::vector<double>& p = answers[0].probs;
        g.nsfw = static_cast<float>(std::clamp((p.size() > 2 ? p[2] + p[3] + p[4] : 0.0) + (p.size() > 1 ? 0.25 * p[1] : 0.0), 0.0, 1.0));
        g.quality = static_cast<float>(std::clamp(answers[1].score / 9.0 * 10.0, 0.0, 10.0));
        const std::vector<double>& st = answers[2].probs;
        g.style = static_cast<int>(std::max_element(st.begin(), st.end()) - st.begin());
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_grades[Lower(in.path)] = g;
            ++g_version;
            if (++graded % 25 == 0) SaveLocked();
        }
        ++g_done;
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        SaveLocked();
    }
    SetPhase("Unloading Clef...");
    ClefRelease(session, true);
    std::lock_guard<std::mutex> lock(g_phaseMutex);
    g_finished = "Clef graded " + std::to_string(graded) + " pictures" + (failed > 0 ? " (" + std::to_string(failed) + " could not be read)" : std::string()) + (g_stop ? " (stopped)" : std::string());
    g_phase.clear();
    g_running = false;
}

struct Joiner {
    ~Joiner() {
        g_stop = true;
        if (g_thread.joinable()) g_thread.join();
    }
} g_joiner;

} // namespace

void ClefGradeStart(const std::vector<ClefGradeInput>& items, const std::string& comfyUrl, bool onlyNew) {
    if (g_running) return;
    if (g_thread.joinable()) g_thread.join();
    g_running = true;
    g_stop = false;
    g_done = 0;
    g_total = static_cast<int>(items.size());
    SetPhase("Starting...");
    g_thread = std::thread(Run, items, comfyUrl, onlyNew);
}

void ClefGradeStop() { g_stop = true; }
bool ClefGradeRunning() { return g_running.load(); }

void ClefGradeProgress(int& done, int& total, std::string& phase) {
    done = g_done.load();
    total = g_total.load();
    std::lock_guard<std::mutex> lock(g_phaseMutex);
    phase = g_phase;
}

void PumpClefGrades(AppState& app) {
    static unsigned seenVersion = 0;
    static size_t cursor = 0;
    static size_t seenCount = 0;
    static const ImageEntry* seenData = nullptr;
    static double lastStatus = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        LoadLocked();
        if (g_version != seenVersion || app.entries.size() != seenCount || app.entries.data() != seenData) { // new grades or a new list: look at every picture again
            seenVersion = g_version;
            seenCount = app.entries.size();
            seenData = app.entries.data();
            cursor = 0;
        }
        bool changed = false;
        for (int n = 0; n < 400 && cursor < app.entries.size(); ++n, ++cursor) {
            ImageEntry& e = app.entries[cursor];
            if (e.clefGraded) continue;
            auto it = g_grades.find(Lower(e.path));
            if (it == g_grades.end()) continue;
            const Grade& g = it->second;
            if (g.mtime != static_cast<long long>(e.mtime.time_since_epoch().count()) || g.size != e.size) continue; // the file changed since
            if (g.nsfw >= 0.0f) e.nsfwImageScore = g.nsfw;
            if (g.quality >= 0.0f) {
                e.qualityGeneral = g.quality;
                e.qualityAnime = g.quality;
                e.qualityAnimeProb = (g.style == 1 || g.style == 2) ? 1.0f : 0.0f;
            }
            if (g.style >= 0) {
                e.styleKind = g.style;
                e.styleConf = 0.9f;
            }
            e.clefGraded = true;
            changed = true;
        }
        if (changed) app.filterEpoch++;
    }
    if (g_running) {
        if (ImGui::GetTime() - lastStatus > 1.0) {
            lastStatus = ImGui::GetTime();
            int d = 0, t = 0;
            std::string phase;
            ClefGradeProgress(d, t, phase);
            app.SetStatus("Clef: " + phase + (t > 0 ? " (" + std::to_string(d) + " of " + std::to_string(t) + ")" : std::string()));
        }
    } else {
        std::lock_guard<std::mutex> lock(g_phaseMutex);
        if (!g_finished.empty()) {
            app.SetStatus(g_finished);
            g_finished.clear();
        }
    }
}

// ---------------------------------------------------------------- prompt triage
namespace {
std::thread g_rateThread;
std::atomic<bool> g_rateRunning{false};
std::mutex g_rateMutex;
std::string g_ratePrompt, g_rateText;
bool g_rateGood = false, g_rateReady = false;

void RateRun(std::string prompt, std::string comfyUrl) {
    std::string text;
    bool good = false;
    const ClefSession session = ClefAcquire(comfyUrl, [](const std::string&) {});
    if (!session.ok) {
        text = "Clef could not be started (Clef mode tab > Set up Clef).";
    } else {
        std::vector<ClefQuestion> qs;
        qs.push_back({"clarity", "How clear, specific and visual is this prompt as instructions for an image generator?",
                      {"garbled or meaningless", "vague and generic", "acceptable", "specific and visual", "excellent: rich, concrete and coherent"}, ClefType::Score});
        qs.push_back({"contradiction", "Does the prompt contradict itself, with details that cannot be true together?", {}, ClefType::Noul});
        qs.push_back({"overloaded", "Does the prompt ask for too many separate things for a single picture?", {}, ClefType::Noul});
        qs.push_back({"nsfw", "How sexually explicit is what this prompt asks for?",
                      {"completely safe", "mildly suggestive", "sexy or revealing", "explicit nudity", "explicit sexual activity"}, ClefType::Score});
        std::vector<ClefAnswer> a;
        std::string state = prompt.size() > 3000 ? prompt.substr(0, 3000) : prompt;
        if (!ClefAsk(state, {}, qs, a, nullptr) || a.size() != 4) {
            text = "Clef did not answer.";
        } else {
            char buf[512];
            const double clarity = a[0].score, contradiction = a[1].pTrue, overloaded = a[2].pTrue, nsfw = a[3].score;
            snprintf(buf, sizeof(buf), "Clef: clarity %.1f / 4   |   contradicts itself: %d%%   |   too much for one picture: %d%%   |   explicitness %.1f / 4", clarity,
                     static_cast<int>(contradiction * 100 + 0.5), static_cast<int>(overloaded * 100 + 0.5), nsfw);
            text = buf;
            if (clarity < 2.0) text += "\nToo vague: say who is in it, where, and what they are doing; add the light and the camera.";
            if (contradiction > 0.5) text += "\nIt looks contradictory: two details seem to exclude each other.";
            if (overloaded > 0.6) text += "\nIt asks for a lot at once: pick the main idea, or split it into several pictures.";
            good = clarity >= 2.5 && contradiction < 0.4 && overloaded < 0.6;
        }
    }
    ClefRelease(session, false); // a few more checks may follow: the idle timer closes it (and sending to ComfyUI does at once)
    std::lock_guard<std::mutex> lock(g_rateMutex);
    g_ratePrompt = prompt;
    g_rateText = text;
    g_rateGood = good;
    g_rateReady = true;
    g_rateRunning = false;
}

struct RateJoiner {
    ~RateJoiner() {
        if (g_rateThread.joinable()) g_rateThread.join();
    }
} g_rateJoiner;
} // namespace

void ClefRatePromptStart(const std::string& prompt, const std::string& comfyUrl) {
    if (g_rateRunning) return;
    if (g_rateThread.joinable()) g_rateThread.join();
    g_rateRunning = true;
    g_rateThread = std::thread(RateRun, prompt, comfyUrl);
}

bool ClefRatePromptRunning() { return g_rateRunning.load(); }

bool ClefRatePromptTake(std::string& forPrompt, std::string& text, bool& good) {
    std::lock_guard<std::mutex> lock(g_rateMutex);
    if (!g_rateReady) return false;
    g_rateReady = false;
    forPrompt = g_ratePrompt;
    text = g_rateText;
    good = g_rateGood;
    return true;
}
