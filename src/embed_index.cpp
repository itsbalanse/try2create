#include "embed_index.h"

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
#include "vision_prompt.h"

namespace fs = std::filesystem;

namespace {

struct Stored {
    long long mtime = 0;
    unsigned long long size = 0;
    std::vector<float> v; // kEmbedDim floats
};

std::mutex g_mutex;
std::unordered_map<std::wstring, Stored> g_vectors; // lowercased path
bool g_loaded = false;
std::atomic<unsigned> g_version{1};

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

fs::path CacheFile() {
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH * 2);
    return fs::path(std::wstring(buf, n)) / L"Try2Create" / L"embeddings.bin";
}

// file: "MVE1", dim, then per picture: path length (u32), path (wchar_t), mtime (i64), size (u64), dim floats
void LoadLocked() {
    if (g_loaded) return;
    g_loaded = true;
    std::ifstream f(CacheFile(), std::ios::binary);
    if (!f) return;
    char magic[4] = {};
    int dim = 0;
    f.read(magic, 4);
    f.read(reinterpret_cast<char*>(&dim), sizeof(dim));
    if (std::string(magic, 4) != "MVE1" || dim != kEmbedDim) return;
    for (;;) {
        unsigned len = 0;
        if (!f.read(reinterpret_cast<char*>(&len), sizeof(len)) || len == 0 || len > 4096) break;
        std::wstring path(len, L'\0');
        Stored s;
        s.v.resize(kEmbedDim);
        if (!f.read(reinterpret_cast<char*>(path.data()), len * sizeof(wchar_t)) || !f.read(reinterpret_cast<char*>(&s.mtime), sizeof(s.mtime)) ||
            !f.read(reinterpret_cast<char*>(&s.size), sizeof(s.size)) || !f.read(reinterpret_cast<char*>(s.v.data()), kEmbedDim * sizeof(float)))
            break;
        g_vectors[Lower(path)] = std::move(s);
    }
}

void SaveLocked() {
    std::error_code ec;
    fs::create_directories(CacheFile().parent_path(), ec);
    const fs::path tmp = CacheFile().wstring() + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out.write("MVE1", 4);
        const int dim = kEmbedDim;
        out.write(reinterpret_cast<const char*>(&dim), sizeof(dim));
        for (const auto& kv : g_vectors) {
            const unsigned len = static_cast<unsigned>(kv.first.size());
            out.write(reinterpret_cast<const char*>(&len), sizeof(len));
            out.write(reinterpret_cast<const char*>(kv.first.data()), len * sizeof(wchar_t));
            out.write(reinterpret_cast<const char*>(&kv.second.mtime), sizeof(kv.second.mtime));
            out.write(reinterpret_cast<const char*>(&kv.second.size), sizeof(kv.second.size));
            out.write(reinterpret_cast<const char*>(kv.second.v.data()), kEmbedDim * sizeof(float));
        }
    }
    fs::rename(tmp, CacheFile(), ec);
}

// ---------------------------------------------------------------- the indexing job
std::thread g_thread;
std::atomic<bool> g_running{false}, g_stop{false};
std::atomic<int> g_done{0}, g_total{0};
std::mutex g_phaseMutex;
std::string g_phase, g_finished;
void SetPhase(const std::string& s) {
    std::lock_guard<std::mutex> lock(g_phaseMutex);
    g_phase = s;
}

void Run(std::vector<EmbedItem> items, std::string comfyUrl) {
    std::vector<EmbedItem> todo;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        LoadLocked();
        for (const EmbedItem& in : items) {
            auto it = g_vectors.find(Lower(in.path));
            if (it == g_vectors.end() || it->second.mtime != in.mtime || it->second.size != in.size) todo.push_back(in);
        }
    }
    g_total = static_cast<int>(todo.size());
    g_done = 0;
    if (todo.empty()) {
        g_running = false;
        return;
    }
    const ClefSession session = EmbedAcquire(comfyUrl, SetPhase);
    if (!session.ok) {
        std::lock_guard<std::mutex> lock(g_phaseMutex);
        g_finished = "The embedding model could not be started (tools\\embed and the llama.cpp in tools)";
        g_phase.clear();
        g_running = false;
        return;
    }
    int added = 0, failed = 0;
    for (const EmbedItem& in : todo) {
        if (g_stop) break;
        SetPhase("Reading the pictures' meaning");
        const std::string url = ImageDataUrl(in.path, 512);
        std::vector<float> v;
        if (url.empty() || !EmbedImageData(url, v)) {
            ++failed;
            ++g_done;
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            Stored s;
            s.mtime = in.mtime;
            s.size = in.size;
            s.v = std::move(v);
            g_vectors[Lower(in.path)] = std::move(s);
            ++g_version;
            if (++added % 200 == 0) SaveLocked();
        }
        ++g_done;
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        SaveLocked();
    }
    EmbedRelease(session, false); // searches follow: the model stays for a while (closed when ComfyUI needs the GPU, or when idle)
    std::lock_guard<std::mutex> lock(g_phaseMutex);
    g_finished = "Indexed " + std::to_string(added) + " pictures" + (failed > 0 ? " (" + std::to_string(failed) + " could not be read)" : std::string()) + (g_stop ? " (stopped)" : std::string());
    g_phase.clear();
    g_running = false;
}

// ---------------------------------------------------------------- the search text
std::thread g_qThread;
std::atomic<bool> g_qRunning{false};
std::mutex g_qMutex;
std::string g_qText;
std::vector<float> g_qVec;
bool g_qReady = false;

void QueryRun(std::string text, std::string comfyUrl) {
    const ClefSession session = EmbedAcquire(comfyUrl, [](const std::string&) {});
    std::vector<std::vector<float>> v;
    if (session.ok && EmbedTexts({"task: search result | query: " + text}, v) && !v.empty()) {
        std::lock_guard<std::mutex> lock(g_qMutex);
        g_qText = text;
        g_qVec = v[0];
        g_qReady = true;
    }
    EmbedRelease(session, false);
    g_qRunning = false;
}

struct Joiner {
    ~Joiner() {
        g_stop = true;
        if (g_thread.joinable()) g_thread.join();
        if (g_qThread.joinable()) g_qThread.join();
    }
} g_joiner;

} // namespace

void EmbedIndexStart(const std::vector<EmbedItem>& items, const std::string& comfyUrl) {
    if (g_running) return;
    if (g_thread.joinable()) g_thread.join();
    g_running = true;
    g_stop = false;
    g_done = 0;
    g_total = static_cast<int>(items.size());
    SetPhase("Starting...");
    g_thread = std::thread(Run, items, comfyUrl);
}

void EmbedIndexStop() { g_stop = true; }
bool EmbedIndexRunning() { return g_running.load(); }

void EmbedIndexProgress(int& done, int& total, std::string& phase) {
    done = g_done.load();
    total = g_total.load();
    std::lock_guard<std::mutex> lock(g_phaseMutex);
    phase = g_phase;
}

unsigned EmbedIndexVersion() { return g_version.load(); }

bool EmbedHas(const EmbedItem& item) {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadLocked();
    auto it = g_vectors.find(Lower(item.path));
    return it != g_vectors.end() && it->second.mtime == item.mtime && it->second.size == item.size;
}

bool EmbedGet(const EmbedItem& item, std::vector<float>& out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadLocked();
    auto it = g_vectors.find(Lower(item.path));
    if (it == g_vectors.end() || it->second.mtime != item.mtime || it->second.size != item.size) return false;
    out = it->second.v;
    return true;
}

void EmbedQueryStart(const std::string& text, const std::string& comfyUrl) {
    if (g_qRunning) return;
    if (g_qThread.joinable()) g_qThread.join();
    g_qRunning = true;
    g_qThread = std::thread(QueryRun, text, comfyUrl);
}

bool EmbedQueryRunning() { return g_qRunning.load(); }

bool EmbedQueryTake(std::string& text, std::vector<float>& vec) {
    std::lock_guard<std::mutex> lock(g_qMutex);
    if (!g_qReady) return false;
    g_qReady = false;
    text = g_qText;
    vec = g_qVec;
    return true;
}

float EmbedDot(const std::vector<float>& a, const std::vector<float>& b) {
    float s = 0;
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) s += a[i] * b[i];
    return s;
}

std::vector<float> EmbedScoreEntries(const AppState& app, const std::vector<float>& query) {
    std::vector<float> out(app.entries.size(), -2.0f);
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadLocked();
    for (size_t i = 0; i < app.entries.size(); ++i) {
        const ImageEntry& e = app.entries[i];
        auto it = g_vectors.find(Lower(e.path));
        if (it == g_vectors.end() || it->second.size != e.size) continue;
        out[i] = EmbedDot(query, it->second.v);
    }
    return out;
}

void EmbedFindLike(AppState& app, int entryIndex) {
    if (entryIndex < 0 || entryIndex >= static_cast<int>(app.entries.size())) return;
    const ImageEntry& e = app.entries[entryIndex];
    app.semanticOn = true;
    app.semanticLikePath = e.path;
    app.semanticText = "like " + std::filesystem::path(e.path).filename().string();
    app.semanticVec.clear();
    EmbedItem item{e.path, static_cast<long long>(e.mtime.time_since_epoch().count()), e.size};
    if (!EmbedHas(item)) EmbedIndexStart({item}, app.comfy.url);
    ++app.semanticVersion;
}

void PumpEmbed(AppState& app) {
    static double lastStatus = 0;
    // a finished search text
    std::string text;
    std::vector<float> vec;
    if (EmbedQueryTake(text, vec)) {
        app.semanticVec = std::move(vec);
        app.semanticText = text;
        app.semanticLikePath.clear();
        ++app.semanticVersion;
    }
    // "more like this": waits for the picture's own vector
    if (!app.semanticLikePath.empty() && app.semanticVec.empty()) {
        for (const ImageEntry& e : app.entries) {
            if (e.path != app.semanticLikePath) continue;
            std::vector<float> v;
            if (EmbedGet({e.path, static_cast<long long>(e.mtime.time_since_epoch().count()), e.size}, v)) {
                app.semanticVec = std::move(v);
                ++app.semanticVersion;
            }
            break;
        }
    }
    if (g_running) {
        if (ImGui::GetTime() - lastStatus > 1.0) {
            lastStatus = ImGui::GetTime();
            int d = 0, t = 0;
            std::string phase;
            EmbedIndexProgress(d, t, phase);
            app.SetStatus("Embedding model: " + phase + (t > 0 ? " (" + std::to_string(d) + " of " + std::to_string(t) + ")" : std::string()));
        }
    } else {
        std::lock_guard<std::mutex> lock(g_phaseMutex);
        if (!g_finished.empty()) {
            app.SetStatus(g_finished);
            g_finished.clear();
        }
    }
}
