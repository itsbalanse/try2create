#include "relation_import.h"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <windows.h>

#include "app_state.h"
#include "board.h"
#include "clef.h"

namespace {

struct Imported {
    std::string label, category;
};

std::thread g_thread;
std::atomic<bool> g_running{false};
std::mutex g_mutex;
std::string g_status;
std::vector<Imported> g_done;
bool g_doneReady = false;
std::string g_summary;

void SetStatus(const std::string& s) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_status = s;
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string TrimLine(std::string s) {
    auto issp = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',' || c == ';' || c == '"'; };
    while (!s.empty() && issp(s.back())) s.pop_back();
    size_t a = 0;
    while (a < s.size() && issp(s[a])) ++a;
    s = s.substr(a);
    // a leading bullet or number: "- laughs with", "* laughs with", "3. laughs with"
    size_t i = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
    if (i > 0 && i < s.size() && (s[i] == '.' || s[i] == ')')) s = s.substr(i + 1);
    else if (!s.empty() && (s[0] == '-' || s[0] == '*' || s[0] == '\xE2')) { // "-", "*", or the start of a bullet character
        if (s[0] != '\xE2' || (s.size() > 3 && static_cast<unsigned char>(s[1]) == 0x80 && static_cast<unsigned char>(s[2]) == 0xA2)) s = s.substr(s[0] == '\xE2' ? 3 : 1);
    }
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    return s;
}

const char* Describe(const std::string& cat) {
    static const std::map<std::string, const char*> d = {
        {"Fighting", "hostile or violent: fighting, attacking, chasing, defeating"},
        {"Friends & team", "friendly, cooperative or protective: helping, teaming up, talking, posing together"},
        {"Couple", "romantic, affectionate or intimate between two people"},
        {"Funny", "silly, playful or comedic: pranks, teasing, jokes, slapstick"},
        {"Adventure", "exploring, hiding, rescuing, escaping, following, searching"},
        {"Everyday", "ordinary shared activities: cooking, walking, studying, playing a game, waiting"},
        {"Rivals & drama", "confrontation, rivalry, negotiation, betrayal, forgiving, tension"}};
    auto it = d.find(cat);
    return it == d.end() ? nullptr : it->second;
}

void Run(std::wstring path, std::string comfyUrl, std::vector<std::string> categories, std::set<std::string> existing) {
    std::ifstream f(path, std::ios::binary);
    std::vector<std::string> lines;
    std::set<std::string> seen;
    std::string line;
    while (f && std::getline(f, line)) {
        if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF && static_cast<unsigned char>(line[1]) == 0xBB) line = line.substr(3); // BOM
        line = TrimLine(line);
        if (line.empty() || line.size() > 120) continue;
        const std::string key = Lower(line);
        if (existing.count(key) || seen.count(key)) continue;
        seen.insert(key);
        lines.push_back(line);
    }
    if (lines.empty()) {
        SetStatus("Nothing new in that file (empty, or every line is already in the library)");
        g_running = false;
        return;
    }
    std::vector<Imported> out;
    ClefSession session;
    bool clef = false;
    if (ClefGetSettings().use) {
        SetStatus("Starting Clef...");
        session = ClefAcquire(comfyUrl, [](const std::string& m) { SetStatus(m); });
        clef = session.ok;
    }
    if (clef) {
        // the categories he may choose from, each with what it means
        std::vector<std::string> options;
        for (const std::string& c : categories) {
            const char* d = Describe(c);
            options.push_back(d ? c + ": " + d : c);
        }
        const size_t chunk = 24;
        for (size_t at = 0; at < lines.size(); at += chunk) {
            const size_t end = std::min(lines.size(), at + chunk);
            SetStatus("Clef is sorting the actions (" + std::to_string(at) + " of " + std::to_string(lines.size()) + ")...");
            std::vector<ClefQuestion> qs;
            for (size_t i = at; i < end; ++i) {
                ClefQuestion q;
                q.id = "r" + std::to_string(i);
                q.instructions = "Which category does this action between two characters belong to: \"" + lines[i] + "\"?";
                q.options = options;
                qs.push_back(std::move(q));
            }
            std::vector<ClefAnswer> answers;
            const bool ok = ClefAsk("Sort the actions that one character does to another into categories.", {}, qs, answers, nullptr) && answers.size() == qs.size();
            for (size_t i = at; i < end; ++i) {
                Imported im;
                im.label = lines[i];
                im.category = "My own";
                if (ok) {
                    const std::vector<double>& p = answers[i - at].probs;
                    const size_t best = static_cast<size_t>(std::max_element(p.begin(), p.end()) - p.begin());
                    if (p[best] >= 0.30) im.category = categories[best]; // not sure at all: it stays in "My own"
                }
                out.push_back(std::move(im));
            }
        }
        ClefRelease(session, true);
    } else {
        for (const std::string& l : lines) out.push_back({l, "My own"});
    }
    std::map<std::string, int> counts;
    for (const Imported& i : out) ++counts[i.category];
    std::string summary = "Imported " + std::to_string(out.size()) + (clef ? " actions, sorted by Clef: " : " actions into My own (Clef was not available): ");
    for (const auto& kv : counts) summary += kv.first + " " + std::to_string(kv.second) + ", ";
    if (summary.size() > 2) summary.resize(summary.size() - 2);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_done = std::move(out);
    g_doneReady = true;
    g_summary = summary;
    g_status = summary;
    g_running = false;
}

struct Joiner {
    ~Joiner() {
        if (g_thread.joinable()) g_thread.join();
    }
} g_joiner;

} // namespace

void RelationImportStart(const std::wstring& txtPath, const std::string& comfyUrl) {
    if (g_running) return;
    if (g_thread.joinable()) g_thread.join();
    std::vector<std::string> categories;
    for (const std::string& c : RelationCategories()) {
        if (c == "Holding & using" || c == "Places" || c == "Placement" || c == "My own") continue;
        categories.push_back(c);
    }
    std::set<std::string> existing;
    for (const RelationEntry& e : BoardRelations(BoardKind::Character, BoardKind::Character)) existing.insert(Lower(e.label));
    g_running = true;
    SetStatus("Reading the file...");
    g_thread = std::thread(Run, txtPath, comfyUrl, categories, existing);
}

bool RelationImportRunning() { return g_running.load(); }

std::string RelationImportStatus() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_status;
}

void PumpRelationImport(AppState& app) {
    std::vector<Imported> done;
    std::string summary;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_doneReady) return;
        g_doneReady = false;
        done = std::move(g_done);
        g_done.clear();
        summary = g_summary;
    }
    for (const Imported& i : done) AddMyRelation(BoardKind::Character, BoardKind::Character, i.label, i.category);
    app.SetStatus(summary);
}
