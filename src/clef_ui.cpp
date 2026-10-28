// The Clef tab ("Clef mode", the fun mode): Clef (or plain chance when it is not running) keeps inventing scenarios for your saved characters (who is in it, where,
// when, in what mood, what they do together, how it is shot, what each one wears, how they stand and what their face does) and sends each one to Qwen Image 2.1 Edit,
// one after the other until you stop it. Every picture also lands in the Edits tab, with the board it was made from.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <optional>
#include <set>
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include "crash_log.h"
#include "board.h"
#include "clef.h"
#include "embed_index.h"
#include "vision_prompt.h"
#include "director.h"
#include "image_texture.h"
#include "imgui.h"
#include "ui_extra.h"
#include "ui_theme.h"
#include "win_dialogs.h"
#include "json.hpp"

namespace fs = std::filesystem;

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

int StringResize(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* str = static_cast<std::string*>(data->UserData);
        str->resize(data->BufTextLen);
        data->Buf = str->data();
    }
    return 0;
}

bool InputStr(const char* label, const char* hint, std::string& value, float width) {
    ImGui::SetNextItemWidth(width);
    return ImGui::InputTextWithHint(label, hint, value.data(), value.capacity() + 1, ImGuiInputTextFlags_CallbackResize, StringResize, &value);
}

std::wstring PickFile(const wchar_t* filter, const wchar_t* title) {
    wchar_t buf[MAX_PATH * 2] = {};
    OPENFILENAMEW ofn = {sizeof(ofn)};
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&ofn) ? std::wstring(buf) : std::wstring();
}

struct Round {
    int id = 0;
    int activity = 0;
    std::string summary, note, details, state, message, error, faceNote, prompt, draft; // prompt: what was sent to Krea (text mode)
    std::string matchPrompt, matchNote; // what the picture was meant to show / how well the embedding model finds that in it
    bool weakMatch = false;
    bool writing = false; // text mode: waiting for the vision model to write the prompt
    bool faceChecked = false, drifted = false;
    std::vector<std::pair<std::string, std::wstring>> faceRefs; // who is in it (name, their picture), for the face check
    std::wstring result;
    std::vector<std::wstring> images;
    long long when = 0;
    int pixelJob = 0; // pixel art mode: the ComfyUI job whose picture also becomes pixel art
};

struct Planned {
    Scenario scenario;
    std::vector<CastMember> cast; // the pool the scenario was planned from
};

struct CheckItem {
    int roundId = 0;
    std::wstring result;
    std::vector<std::pair<std::string, std::wstring>> refs;
    std::string prompt; // what the picture was meant to show (for the picture-versus-prompt match)
};
struct CheckResult {
    int roundId = 0;
    std::string text;
    bool drifted = false;
    std::string match; // "Picture vs prompt: 0.71 (good)"
    bool weakMatch = false;
};
struct BatchResult {
    std::vector<Planned> scenarios;
    StoryState story;
    std::vector<std::vector<float>> vectors; // the embeddings of the scenarios planned (for the variety check of the next round)
    std::vector<CheckResult> checks;
};

std::deque<Round> g_rounds; // oldest first
int g_nextId = 1;

struct Run {
    bool running = false;
    bool untilStopped = true;
    int target = 10;     // when not until stopped
    int started = 0;     // pictures sent in this run
    int minChars = 2, maxChars = 3;
    int batch = 8;       // Clef plans this many scenarios, then ComfyUI makes them (the GPU is handed from one to the other)
    int bestOf = 4;      // candidate ideas Clef judges for every picture (1 = no judging)
    bool story = false;  // every picture is the next scene of one story
    bool faceCheck = true; // Clef looks at the finished pictures and checks that the faces did not drift
    bool pixel = false;      // pixel art mode: every picture also becomes pixel art (Pixel art tab)
    bool pixelLook = true;   // ...and the edit asks for a pixel-art look
    bool toKrea = false;   // send the board's scene PROMPT to the workflow chosen in the ComfyUI tab (Krea) instead of editing the character pictures with Qwen
    bool kreaWrite = false; // ...and let the prompt writer (an LLM) write that prompt from the board (otherwise the board's quick draft)
    std::future<std::vector<std::pair<int, std::string>>> writeFut; // the prompts being written: (round id, text)
    bool writeBusy = false;
    std::future<std::vector<std::pair<std::wstring, std::string>>> describeFut; // descriptions being written for cast members: (picture, text)
    bool describeBusy = false;
    std::vector<std::pair<int, std::string>> pendingBriefs; // prompts to be written for this batch: (round id, the brief)
    StoryState storyState;
    std::vector<std::vector<float>> recentVectors; // the embedding of the scenarios made recently: a new one that is too close to them is thrown away
    bool planning = false;
    std::future<BatchResult> plan;
    std::set<std::string> off; // characters (keys) left out of the pool
    bool oneShot = false;
    std::string status;
};
Run g_run;
std::mutex g_phaseMutex;
std::string g_phase; // what the planning thread is doing right now
void SetPhase(const std::string& s) {
    std::lock_guard<std::mutex> lock(g_phaseMutex);
    g_phase = s;
}
std::string GetPhase() {
    std::lock_guard<std::mutex> lock(g_phaseMutex);
    return g_phase;
}

// is the Clef server answering? asked in the background every few seconds
bool ClefUp() {
    static std::future<bool> fut;
    static bool up = false;
    static double last = -100.0;
    if (fut.valid() && fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) up = fut.get();
    if (!fut.valid() && ImGui::GetTime() - last > 4.0) {
        last = ImGui::GetTime();
        fut = std::async(std::launch::async, []() { return ClefPing(); });
    }
    return up;
}

struct CastItem {
    std::wstring image;
    std::string name, description;
    bool on = true;
    bool missing = false; // its picture cannot be read just now (its drive is not plugged in): left out of the pool, not removed (not saved)
};
std::vector<CastItem> g_cast;
bool g_castLoaded = false;
float g_castSize = 150.0f; // card size in dp
float g_castH = 330.0f;    // height of the cast box in dp (drag its bottom edge)

std::wstring CastFile() {
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH * 2);
    return std::wstring(buf, n) + L"\\Try2Create\\clef_cast.json";
}

void SaveCast() {
    nlohmann::json j = {{"size", g_castSize}, {"height", g_castH}, {"items", nlohmann::json::array()}};
    for (const CastItem& m : g_cast) j["items"].push_back({{"image", ToUtf8(m.image)}, {"name", m.name}, {"description", m.description}, {"on", m.on}});
    std::error_code ec;
    fs::create_directories(fs::path(CastFile()).parent_path(), ec);
    std::ofstream out(CastFile(), std::ios::binary | std::ios::trunc);
    if (out) out << j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
}

// Can a cast picture still be read? Moved, renamed, deleted or locked away, Qwen would fail on it every time.
bool PictureReadable(const std::wstring& path) {
    if (path.empty()) return false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    const bool ok = GetFileSizeEx(h, &size) && size.QuadPart > 0;
    CloseHandle(h);
    return ok;
}

// Is the drive (or network share) the picture is on there at all? A USB drive that is not plugged in does not mean the picture is gone.
bool DriveThere(const std::wstring& path) {
    if (path.size() >= 3 && path[1] == L':') return GetFileAttributesW(path.substr(0, 3).c_str()) != INVALID_FILE_ATTRIBUTES;
    if (path.rfind(L"\\\\", 0) == 0) { // \\server\share\...
        const size_t a = path.find(L'\\', 2), b = a == std::wstring::npos ? a : path.find(L'\\', a + 1);
        if (b != std::wstring::npos) return GetFileAttributesW(path.substr(0, b + 1).c_str()) != INVALID_FILE_ATTRIBUTES;
    }
    return true;
}

std::string g_castRemoved; // who was taken out of the cast because their picture is gone (shown under the cast until dismissed)

// Cast members whose picture cannot be read any more leave the cast, so Clef does not keep sending edits that fail. One whose drive is not there is only
// left out of the pool until it comes back. Returns how many left.
int PruneCast(AppState& app) {
    std::vector<std::string> gone;
    for (auto it = g_cast.begin(); it != g_cast.end();) {
        const bool readable = PictureReadable(it->image);
        it->missing = !readable;
        if (!readable && DriveThere(it->image)) {
            gone.push_back(it->name.empty() ? ToUtf8(fs::path(it->image).stem().wstring()) : it->name);
            it = g_cast.erase(it);
        } else {
            ++it;
        }
    }
    if (gone.empty()) return 0;
    SaveCast();
    std::string names;
    for (size_t i = 0; i < gone.size(); ++i) names += (i ? ", " : "") + gone[i];
    g_castRemoved = (g_castRemoved.empty() ? std::string() : g_castRemoved + "; ") + names;
    app.SetStatus("Removed from the Clef cast (the picture is gone): " + names);
    CrashNote("clef: removed " + std::to_string(gone.size()) + " cast members whose picture is gone");
    return static_cast<int>(gone.size());
}

// now and then (and before every round): the cast is checked
void CheckCast(AppState& app, bool now) {
    static double last = -100.0;
    if (!now && ImGui::GetTime() - last < 5.0) return;
    last = ImGui::GetTime();
    PruneCast(app);
}

bool InCast(const std::wstring& image) {
    for (const CastItem& m : g_cast) {
        if (_wcsicmp(m.image.c_str(), image.c_str()) == 0) return true;
    }
    return false;
}

// a picture joins the cast: its name and description come from the saved character it is, when it is one
bool AddCastImage(const std::wstring& image) {
    if (image.empty() || InCast(image) || !PictureReadable(image)) return false;
    CastItem m;
    m.image = image;
    m.name = ToUtf8(fs::path(image).stem().wstring());
    if (const CharacterProfile* p = FindProfile(image)) {
        if (!p->name.empty()) m.name = p->name;
        m.description = p->description;
    }
    g_cast.push_back(std::move(m));
    return true;
}

void LoadCast() {
    if (g_castLoaded) return;
    g_castLoaded = true;
    std::ifstream f(CastFile(), std::ios::binary);
    if (f) {
        nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
        if (j.is_object()) {
            g_castSize = std::clamp(j.value("size", g_castSize), 60.0f, 400.0f);
            g_castH = std::clamp(j.value("height", g_castH), 120.0f, 900.0f);
            if (j.contains("items") && j["items"].is_array()) {
                for (const nlohmann::json& e : j["items"]) {
                    CastItem m;
                    m.image = ToWide(e.value("image", std::string()));
                    m.name = e.value("name", std::string());
                    m.description = e.value("description", std::string());
                    m.on = e.value("on", true);
                    if (!m.image.empty()) g_cast.push_back(std::move(m));
                }
            }
        }
        return;
    }
    for (const CharacterProfile& p : AllProfiles()) { // the first time: the saved characters
        if (p.saved) AddCastImage(p.image);
    }
}

std::vector<CastMember> Pool() {
    std::vector<CastMember> pool;
    for (const CastItem& m : g_cast) {
        if (!m.on || m.missing) continue;
        CastMember c;
        c.image = m.image;
        c.name = m.name.empty() ? ToUtf8(fs::path(m.image).stem().wstring()) : m.name;
        c.description = m.description;
        pool.push_back(std::move(c));
    }
    return pool;
}

void Thumb(AppState& app, const std::wstring& path, ImVec2 size) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const AppState::LibThumb t = app.LibraryThumb(path);
    if (t.srv && t.w > 0 && t.h > 0) {
        const float s = std::min(size.x / t.w, size.y / t.h);
        const ImVec2 a(p.x + (size.x - t.w * s) * 0.5f, p.y + (size.y - t.h * s) * 0.5f);
        dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), ui::ThumbPlaceholder(), ui::Dp(5.0f));
        dl->AddImage((ImTextureID)(intptr_t)t.srv, a, ImVec2(a.x + t.w * s, a.y + t.h * s));
    } else {
        dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), ui::ThumbPlaceholder(), ui::Dp(5.0f));
    }
}

void SyncRounds(AppState& app) {
    bool any = false;
    for (const Round& r : g_rounds) any = any || r.activity > 0;
    if (!any || !app.comfySvc) return;
    const std::vector<ComfyActivity> acts = app.comfySvc->Activity();
    for (Round& r : g_rounds) {
        if (r.activity <= 0) continue;
        const ComfyActivity* a = nullptr;
        for (const ComfyActivity& x : acts) {
            if (x.id == r.activity) a = &x;
        }
        if (!a) {
            r.activity = 0;
            r.error = "ComfyUI lost track of it";
            continue;
        }
        r.message = a->message;
        switch (a->state) {
            case ComfyState::Waiting: r.state = "Waiting"; break;
            case ComfyState::Starting: r.state = "Starting ComfyUI"; break;
            case ComfyState::Sending: r.state = "Sending"; break;
            case ComfyState::Queued: r.state = "Queued"; break;
            case ComfyState::Running: r.state = "Generating"; break;
            default: r.state.clear(); break;
        }
        if (a->state == ComfyState::Done) {
            for (const ComfyResult& f : a->files) {
                if (!f.localPath.empty() && r.result.empty()) r.result = f.localPath;
            }
            if (r.result.empty()) r.error = "ComfyUI made no picture";
            r.activity = 0;
        } else if (a->state == ComfyState::Failed) {
            r.error = a->message;
            r.activity = 0;
            bool pictureGone = false;
            for (const std::wstring& img : r.images) pictureGone = pictureGone || !PictureReadable(img);
            if (pictureGone && PruneCast(app) > 0) { // a cast picture is gone: it left the cast, the run goes on without it
                r.error = "A picture it used is gone (it was removed from the cast): " + a->message;
            } else {
                g_run.running = false; // something is wrong (ComfyUI, the model...): do not keep sending
                g_run.status = "Stopped: " + a->message;
            }
        }
    }
}

int Pending() {
    int n = 0;
    for (const Round& r : g_rounds) n += (r.activity > 0 || r.writing) ? 1 : 0;
    return n;
}

// text mode: the finished prompt goes to ComfyUI (the workflow of the ComfyUI tab: Krea)
void SendKreaPrompt(AppState& app, Round& r, const std::string& prompt) {
    r.prompt = g_run.pixel && g_run.pixelLook ? prompt + " " + PixelArtLookClause() : prompt;
    r.matchPrompt = prompt.size() > 900 ? prompt.substr(0, 900) : prompt;
    r.writing = false;
    if (SendToComfy(app, r.prompt, std::string(), std::wstring(), "Clef mode: " + r.summary.substr(0, 60), true)) {
        r.activity = LastEditActivity();
        if (g_run.pixel) {
            r.pixelJob = r.activity;
            PixelArtAfter(app, r.activity, "Clef mode", r.summary.substr(0, 40), false);
        }
    } else {
        r.error = "Not sent (choose the workflow on the Generate page first)";
    }
}

// the scenario becomes a board, then an instruction, then a picture edit
// What a scenario is meant to show, in a few plain sentences (compared with the finished picture by the embedding model)
std::string MatchText(const Planned& pl) {
    const Scenario& sc = pl.scenario;
    std::string t;
    for (int idx : sc.cast) {
        if (idx >= 0 && idx < static_cast<int>(pl.cast.size())) t += (t.empty() ? "" : "; ") + pl.cast[idx].name + (pl.cast[idx].description.empty() ? std::string() : ", " + pl.cast[idx].description);
    }
    return t + ". " + sc.PlaceText() + ", " + sc.time + ". " + sc.mood + ". " + sc.twist + ".";
}

// A short description of the scenario, to tell whether it repeats an earlier one
std::string VarietyText(const Planned& pl) {
    const Scenario& sc = pl.scenario;
    return sc.PlaceText() + ", " + sc.time + ". " + sc.genre + ". " + sc.mood + ". " + sc.twist + ". " + sc.prop + ". " + (sc.relations.empty() ? std::string() : sc.relations[0]);
}

void SendScenarioKrea(AppState& app, const Planned& pl);

void SendScenario(AppState& app, const Planned& pl) {
    if (g_run.toKrea) {
        SendScenarioKrea(app, pl);
        return;
    }
    const Scenario& sc = pl.scenario;
    Board board = BoardFromScenario(pl.cast, sc);
    std::vector<std::pair<std::wstring, CharLook>> fixed;
    for (size_t i = 0; i < sc.cast.size() && i < sc.looks.size(); ++i) {
        if (sc.cast[i] >= 0 && sc.cast[i] < static_cast<int>(pl.cast.size())) fixed.push_back({pl.cast[sc.cast[i]].image, sc.looks[i]});
    }
    BoardEditOptions eo;
    eo.styleMode = std::max(1, app.comfy.editStyle); // the fun mode always directs the characters
    eo.flags = DirectorFlagsFor(app.comfy, eo.styleMode);
    eo.place = sc.PlaceText();
    eo.fixedLooks = &fixed;
    std::vector<std::wstring> images;
    EditContext ctx;
    int canvas = 0;
    std::string instruction = BoardEditRequest(board, 0, std::string(), images, ctx.roles, canvas, eo);
    if (images.empty() || instruction.empty()) return;
    for (const std::wstring& img : images) { // a picture went missing since the round was planned: it leaves the cast, this scenario is skipped
        if (!PictureReadable(img)) {
            PruneCast(app);
            return;
        }
    }
    if (g_run.pixel) { // pixel art mode: the picture also becomes pixel art when it is done
        ctx.pixelArt = true;
        ctx.pixelGroup = "Clef mode";
        if (g_run.pixelLook) instruction += " " + PixelArtLookClause();
    }
    ctx.hasBoard = true;
    ctx.board = board;
    ctx.scene = 0;
    ctx.canvas = sc.canvas;
    ctx.looksText = eo.looksOut;
    Round r;
    r.id = g_nextId++;
    r.summary = sc.Summary([&] {
        std::vector<DirectorChar> d;
        for (const CastMember& m : pl.cast) d.push_back({m.name, m.description, ""});
        return d;
    }());
    r.note = sc.note;
    r.details = "Place: " + sc.PlaceText() + "\nWhen: " + sc.time + "\nGenre: " + sc.genre + " | Mood: " + sc.mood + "\nTwist: " + sc.twist + " | Object: " + sc.prop;
    for (size_t i = 0; i < sc.cast.size() && i < sc.looks.size(); ++i) {
        if (sc.cast[i] < 0 || sc.cast[i] >= static_cast<int>(pl.cast.size())) continue;
        const CharLook& l = sc.looks[i];
        r.details += "\n" + pl.cast[sc.cast[i]].name + ": " + l.outfit + "; " + l.pose + "; " + l.expression;
    }
    r.images = images;
    r.matchPrompt = MatchText(pl);
    if (g_run.faceCheck) {
        for (int idx : sc.cast) {
            if (idx >= 0 && idx < static_cast<int>(pl.cast.size())) r.faceRefs.push_back({pl.cast[idx].name, pl.cast[idx].image});
        }
    }
    r.when = static_cast<long long>(std::time(nullptr));
    r.state = "Waiting";
    const std::string label = "Clef mode: " + r.summary.substr(0, 60);
    if (SendPictureEdit(app, images, instruction, label, 1, &ctx)) {
        r.activity = LastEditActivity();
        if (g_run.pixel) r.pixelJob = r.activity;
    } else {
        r.error = "Not sent";
    }
    g_rounds.push_back(std::move(r));
}

// One round, on a worker thread. The GPU is shared between Clef and ComfyUI (Qwen alone takes most of a 24 GB card), so it is handed over smartly:
// make room only if there is not enough free (ComfyUI unloads its models, then Ollama's), load Clef, let it plan a whole batch of scenarios, unload Clef,
// and ComfyUI gets the GPU back for the batch (it loads Qwen once and makes them all).
BatchResult PlanBatch(std::vector<CastMember> pool, std::vector<std::string> relations, DirectorFlags flags, int minC, int maxC, int count, std::string comfyUrl, int bestOf, bool story,
                      StoryState storyState, std::vector<CheckItem> checks, std::vector<std::vector<float>> recent) {
    BatchResult out;
    out.story = storyState;
    const ClefSettings cs = ClefGetSettings();
    bool useClef = false;
    ClefSession session;
    if (cs.use && (count > 0 || !checks.empty())) {
        session = ClefAcquire(comfyUrl, SetPhase);
        useClef = session.ok;
    }
    // the embedding model (small; stays loaded next to Clef): picture-versus-prompt match, and variety between scenarios
    ClefSession embed;
    if (EmbedAvailable() && (count > 0 || !checks.empty())) embed = EmbedAcquire(comfyUrl, SetPhase);
    // 1. look at the pictures of the last round: did the faces drift, and does the picture show what was asked?
    for (size_t k = 0; k < checks.size(); ++k) {
        if (embed.ok && !checks[k].prompt.empty()) {
            SetPhase("The embedding model is comparing pictures with their prompts (" + std::to_string(k + 1) + " of " + std::to_string(checks.size()) + ")...");
            std::vector<float> iv;
            std::vector<std::vector<float>> tv;
            const std::string url = ImageDataUrl(checks[k].result, 512);
            if (!url.empty() && EmbedImageData(url, iv) && EmbedTexts({"task: search result | query: " + checks[k].prompt}, tv) && !tv.empty()) {
                const float cosv = EmbedDot(iv, tv[0]);
                char buf[96];
                snprintf(buf, sizeof(buf), "Picture vs prompt: %.2f (%s)", cosv, cosv >= 0.65f ? "good match" : cosv >= 0.58f ? "so-so" : "weak: it may not show what was asked");
                CheckResult cr;
                cr.roundId = checks[k].roundId;
                cr.match = buf;
                cr.weakMatch = cosv < 0.58f;
                out.checks.push_back(cr);
            }
        }
        if (!useClef || checks[k].refs.empty()) continue;
        SetPhase("Clef is checking faces (" + std::to_string(k + 1) + " of " + std::to_string(checks.size()) + ")...");
        const std::string result = ImageDataUrl(checks[k].result, 768);
        std::string text;
        bool drifted = false;
        for (const auto& ref : checks[k].refs) {
            const std::string refUrl = ImageDataUrl(ref.second, 512);
            if (result.empty() || refUrl.empty()) continue;
            ClefQuestion q;
            q.id = "face";
            q.type = ClefType::Score;
            q.instructions = "The first picture shows the character " + ref.first + ". The last picture is a new picture that should contain " + ref.first +
                             ". How well does a person in the last picture match the face, hair and look of the character in the first one?";
            q.options = {"not there, or clearly a different person", "a weak resemblance", "similar but the face has changed", "a good match", "unmistakably the same character"};
            std::vector<ClefAnswer> a;
            if (!ClefAsk("Check whether a character was reproduced faithfully.", {refUrl, result}, {q}, a, nullptr) || a.size() != 1) continue;
            text += (text.empty() ? "" : ", ") + ref.first + " " + std::to_string(a[0].score).substr(0, 3) + "/4";
            if (a[0].score < 1.8) drifted = true;
        }
        if (!text.empty()) {
            CheckResult cr;
            cr.roundId = checks[k].roundId;
            cr.text = text;
            cr.drifted = drifted;
            out.checks.push_back(cr);
        }
    }
    // 2. how well do the pairs of characters work together
    std::vector<DirectorChar> d;
    for (const CastMember& m : pool) d.push_back({m.name, m.description, ""});
    std::vector<std::vector<double>> chemistry;
    if (useClef && count > 0) {
        SetPhase("Clef is judging which characters work well together...");
        chemistry = ComputeChemistry(d);
    }
    // 3. the round's scenarios
    for (int i = 0; i < count; ++i) {
        SetPhase(std::string(useClef ? "Clef is choosing" : "Choosing at random") + " (" + std::to_string(i + 1) + " of " + std::to_string(count) + ")...");
        // up to three tries: a scenario too close to a recent one (the embedding model compares them) is thrown away, the most different one is kept
        Planned best;
        float bestSim = 2.0f;
        std::vector<float> bestVec;
        for (int attempt = 0; attempt < (embed.ok ? 3 : 1); ++attempt) {
            StoryState tryStory = out.story;
            Planned pl;
            pl.cast = pool;
            pl.scenario = PlanScenario(d, minC, maxC, relations, flags, useClef, 0, bestOf, story ? &tryStory : nullptr, chemistry.empty() ? nullptr : &chemistry);
            if (pl.scenario.cast.empty()) continue;
            float sim = 0;
            std::vector<float> vec;
            if (embed.ok) {
                std::vector<std::vector<float>> tv;
                if (EmbedTexts({"task: clustering | query: " + VarietyText(pl)}, tv) && !tv.empty()) {
                    vec = tv[0];
                    for (const auto& r : recent) sim = std::max(sim, EmbedDot(vec, r));
                }
            }
            if (sim < bestSim) {
                bestSim = sim;
                best = std::move(pl);
                bestVec = std::move(vec);
                if (story) out.story = tryStory;
            }
            if (bestSim < 0.88f) break; // different enough
        }
        if (best.scenario.cast.empty()) continue;
        if (!bestVec.empty()) {
            recent.push_back(bestVec);
            if (recent.size() > 80) recent.erase(recent.begin());
            out.vectors.push_back(bestVec);
            if (bestSim >= 0.88f) best.scenario.note += " (similar to an earlier one: the best of 3 tries)";
        }
        out.scenarios.push_back(std::move(best));
    }
    if (session.started) SetPhase("Unloading Clef: the GPU goes back to ComfyUI...");
    ClefRelease(session, true);
    EmbedRelease(embed, true);
    SetPhase("");
    return out;
}

// Text mode puts every character into words, so a cast member without a description is described first: by the prompt writer, which LOOKS at the picture (a description
// already made - on the cast card or a saved character - is used as it is). True while it is waiting for that.
bool WaitForDescriptions(AppState& app) {
    static std::set<std::wstring> tried;
    if (g_run.describeBusy) {
        if (g_run.describeFut.valid() && g_run.describeFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            g_run.describeBusy = false;
            for (const auto& d : g_run.describeFut.get()) {
                for (CastItem& m : g_cast) {
                    if (_wcsicmp(m.image.c_str(), d.first.c_str()) == 0 && m.description.empty()) m.description = d.second;
                }
            }
            SaveCast();
        }
        return g_run.describeBusy;
    }
    std::vector<std::wstring> missing;
    for (CastItem& m : g_cast) {
        if (!m.on || !m.description.empty()) continue;
        if (const CharacterProfile* p = FindProfile(m.image)) {
            if (!p->description.empty()) {
                m.description = p->description;
                SaveCast();
                continue;
            }
        }
        if (!tried.count(m.image)) missing.push_back(m.image);
    }
    if (missing.empty()) return false;
    for (const std::wstring& p : missing) tried.insert(p);
    if (WriterConfigured() && WriterCanSee()) {
        g_run.describeBusy = true;
        SetPhase("Starting...");
        g_run.describeFut = std::async(std::launch::async, [missing, url = app.comfy.url]() {
            std::vector<std::pair<std::wstring, std::string>> out;
            const ClefSession session = WriterAcquire(url, SetPhase);
            for (size_t i = 0; i < missing.size() && session.ok; ++i) {
                SetPhase("The prompt writer is describing the characters (" + std::to_string(i + 1) + " of " + std::to_string(missing.size()) + ")...");
                std::string text;
                const std::string img = ImageDataUrl(missing[i], 768);
                if (!img.empty() && WriterChat("You describe a character's appearance for text-to-image prompts. Reply with ONLY the description: one or two sentences about who they are and how they look (age, gender, build, hair, face, skin, clothes, accessories). No preface.",
                                               "Describe the character in the picture.", {img}, 160, 0.3, text, nullptr))
                    out.push_back({missing[i], text});
            }
            WriterRelease(session, false);
            SetPhase("");
            return out;
        });
        return true;
    }
    if (!app.visionModel.empty()) { // no writer: the old way, the Vision model through the Characters tab
        for (const std::wstring& p : missing) AddSavedCharacter(app, p);
        return false;
    }
    return false;
}

// The prompts of a batch, written one after the other by the prompt writer (loaded once, closed when ComfyUI needs the GPU).
std::vector<std::pair<int, std::string>> WriteAll(std::vector<std::pair<int, std::string>> jobs, std::string comfyUrl) {
    std::vector<std::pair<int, std::string>> out;
    const ClefSession session = WriterAcquire(comfyUrl, SetPhase);
    for (size_t i = 0; i < jobs.size(); ++i) {
        SetPhase("The prompt writer is writing the prompts (" + std::to_string(i + 1) + " of " + std::to_string(jobs.size()) + ")...");
        std::string text;
        if (session.ok && WriterChat("You write prompts for text-to-image models such as Flux and Krea. Reply with ONLY the finished prompt, with no preface, no quotes and no notes.", jobs[i].second, {}, 700, 0.7, text, nullptr))
            out.push_back({jobs[i].first, text});
        else
            out.push_back({jobs[i].first, std::string()}); // the quick draft is used instead
    }
    WriterRelease(session, false);
    SetPhase("");
    return out;
}

// The board of the scenario becomes a scene prompt (the board's quick draft, or written by the vision model), and the prompt goes to Krea.
void SendScenarioKrea(AppState& app, const Planned& pl) {
    const Scenario& sc = pl.scenario;
    Board board = BoardFromScenario(pl.cast, sc, true);
    BoardPromptOptions opt;
    opt.format = 0;
    opt.scene = 0;
    std::string looks;
    for (size_t i = 0; i < sc.cast.size() && i < sc.looks.size(); ++i) {
        if (sc.cast[i] < 0 || sc.cast[i] >= static_cast<int>(pl.cast.size())) continue;
        const CharLook& l = sc.looks[i];
        std::string part = pl.cast[sc.cast[i]].name;
        if (!l.outfit.empty()) part += " wears " + l.outfit;
        if (!l.pose.empty()) part += "; body: " + l.pose;
        if (!l.expression.empty()) part += "; expression: " + l.expression;
        looks += (looks.empty() ? "" : ". ") + part;
    }
    if (!looks.empty()) opt.extra = "What each character wears and how they look in this picture (these replace any clothes mentioned in their descriptions): " + looks + ".";
    Round r;
    r.id = g_nextId++;
    r.summary = sc.Summary([&] {
        std::vector<DirectorChar> d;
        for (const CastMember& m : pl.cast) d.push_back({m.name, m.description, ""});
        return d;
    }());
    r.note = sc.note + " | text prompt for Krea";
    r.details = "Place: " + sc.PlaceText() + "\nWhen: " + sc.time + "\nGenre: " + sc.genre + " | Mood: " + sc.mood + "\nTwist: " + sc.twist + " | Object: " + sc.prop;
    for (int idx : sc.cast) {
        if (idx >= 0 && idx < static_cast<int>(pl.cast.size())) r.images.push_back(pl.cast[idx].image);
    }
    r.when = static_cast<long long>(std::time(nullptr));
    r.state = "Waiting";
    r.draft = AssembleBoardPrompt(board, opt);
    g_rounds.push_back(std::move(r));
    Round& added = g_rounds.back();
    if (g_run.kreaWrite && WriterConfigured()) { // the prompt writer writes it from the board (all the prompts of the batch together, below)
        std::vector<std::wstring> unused;
        g_run.pendingBriefs.push_back({added.id, BuildBoardBrief(board, opt, unused)});
        added.writing = true;
        added.state = "Waiting for the prompt writer";
    } else {
        SendKreaPrompt(app, added, added.draft);
    }
}

void Advance(AppState& app) {
    SyncRounds(app);
    // prompts the writer finished for text-mode rounds
    if (g_run.writeBusy && g_run.writeFut.valid() && g_run.writeFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        g_run.writeBusy = false;
        for (const auto& w : g_run.writeFut.get()) {
            for (Round& r : g_rounds) {
                if (r.id == w.first && r.writing) SendKreaPrompt(app, r, w.second.empty() ? r.draft : w.second);
            }
        }
    }
    if (g_run.planning && g_run.plan.valid() && g_run.plan.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        g_run.planning = false;
        BatchResult result = g_run.plan.get();
        for (const CheckResult& cr : result.checks) {
            for (Round& r : g_rounds) {
                if (r.id == cr.roundId) {
                    if (!cr.text.empty()) {
                        r.faceNote = cr.text;
                        r.drifted = cr.drifted;
                    }
                    if (!cr.match.empty()) {
                        r.matchNote = cr.match;
                        r.weakMatch = cr.weakMatch;
                    }
                    r.faceChecked = true;
                }
            }
        }
        if (g_run.story) g_run.storyState = result.story;
        for (auto& v : result.vectors) g_run.recentVectors.push_back(std::move(v));
        while (g_run.recentVectors.size() > 80) g_run.recentVectors.erase(g_run.recentVectors.begin());
        const std::vector<Planned>& batch = result.scenarios;
        for (const Planned& pl : batch) { // the whole batch goes to ComfyUI at once: it loads Qwen once and makes them one after the other
            if (!g_run.running) break;
            if (!g_run.untilStopped && g_run.started >= g_run.target) break;
            SendScenario(app, pl);
            ++g_run.started;
        }
        if (!g_run.pendingBriefs.empty()) { // text mode with the writer: write this batch's prompts now
            g_run.writeBusy = true;
            SetPhase("Starting...");
            g_run.writeFut = std::async(std::launch::async, WriteAll, g_run.pendingBriefs, app.comfy.url);
            g_run.pendingBriefs.clear();
        }
        if (g_run.running && !batch.empty()) g_run.status = "ComfyUI is making this round's pictures";
    }
    if (g_run.planning || g_run.writeBusy || Pending() > 0) return;
    // pictures that are made and not yet checked: Clef looks at their faces (while it is loaded for the next round, or alone when the run is over)
    std::vector<CheckItem> checks;
    if (g_run.faceCheck || EmbedAvailable()) {
        for (const Round& r : g_rounds) {
            if (!r.faceChecked && !r.result.empty() && (!r.faceRefs.empty() || !r.matchPrompt.empty())) checks.push_back({r.id, r.result, r.faceRefs, r.matchPrompt});
        }
        for (Round& r : g_rounds) { // whatever cannot be checked is not asked again
            if (!r.faceChecked && (r.result.empty() || r.faceRefs.empty()) && r.activity == 0) r.faceChecked = true;
        }
    }
    if (!g_run.running) {
        if (checks.empty()) return;
        g_run.planning = true;
        SetPhase("Starting...");
        g_run.plan = std::async(std::launch::async, PlanBatch, std::vector<CastMember>(), std::vector<std::string>(), DirectorFlagsFor(app.comfy, 1), 1, 1, 0, app.comfy.url, 1, false, StoryState(), checks, std::vector<std::vector<float>>());
        for (Round& r : g_rounds) {
            for (const CheckItem& ci : checks) {
                if (ci.roundId == r.id) r.faceChecked = true; // asked once; the answer fills in when it comes
            }
        }
        return;
    }
    if (g_run.oneShot && g_run.started >= 1) { // the one surprise is done
        g_run.running = false;
        g_run.oneShot = false;
        g_run.status = "Finished";
        return;
    }
    if (!g_run.untilStopped && g_run.started >= g_run.target) {
        g_run.running = false;
        g_run.status = "Finished";
        return;
    }
    if (g_run.toKrea && WaitForDescriptions(app)) {
        g_run.status = "The vision model is describing the characters...";
        return;
    }
    CheckCast(app, true);
    std::vector<CastMember> pool = Pool();
    if (pool.empty()) {
        g_run.running = false;
        g_run.status = g_cast.empty() ? "No characters: save some in the Characters tab first" : "No characters in the pool whose picture can be read";
        return;
    }
    std::vector<std::string> relations;
    for (const RelationEntry& e : BoardRelations(BoardKind::Character, BoardKind::Character)) relations.push_back(e.label);
    const DirectorFlags flags = DirectorFlagsFor(app.comfy, std::max(1, app.comfy.editStyle));
    const int count = g_run.oneShot ? 1 : g_run.untilStopped ? g_run.batch : std::max(1, std::min(g_run.batch, g_run.target - g_run.started));
    g_run.planning = true;
    SetPhase("Starting...");
    g_run.plan = std::async(std::launch::async, PlanBatch, pool, relations, flags, g_run.minChars, g_run.maxChars, count, app.comfy.url, g_run.bestOf, g_run.story, g_run.storyState, checks, g_run.recentVectors);
    for (Round& r : g_rounds) {
        for (const CheckItem& ci : checks) {
            if (ci.roundId == r.id) r.faceChecked = true;
        }
    }
}

void OpenUrl(const wchar_t* url) { ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL); }

// A file path box: up to 360 wide, but always leaving room for the small "Browse..." button after it.
float PathBoxWidth() {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float browse = ImGui::CalcTextSize("Browse...").x + st.FramePadding.x * 2.0f + st.ItemSpacing.x;
    return std::max(ui::Dp(120.0f), std::min(ui::Dp(360.0f), ImGui::GetContentRegionAvail().x - browse));
}

void DrawClefSettings(AppState& app) {
    ClefSettings s = ClefGetSettings();
    bool changed = false;
    const bool up = ClefUp();
    ImGui::TextUnformatted("Clef (the decision model)");
    ImGui::SameLine();
    const bool ready = s.use && !s.exe.empty() && !s.model.empty();
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(up ? ui::kGood : ui::kWarn, "%s", up ? "running" : (ClefOwned() ? "loading..." : ready ? "unloaded (it loads for each round, then frees the GPU)" : "not set up: choices are random"));
    ImGui::TextDisabled("Clef does not write text: for each question it says how likely every option is. The app asks it to pick outfits, poses, expressions, places, moods... and "
                        "samples from its answers, so the choices fit the characters and the scene but still vary. Without it everything is chosen at random.");
    ImGui::PopTextWrapPos();
    if (ImGui::Checkbox("Use Clef when it is running", &s.use)) changed = true;
    if (ImGui::TreeNode("Prompt writer (LLM)##writertree")) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Status: %s. It is a normal LLM served by llama-server, by default your Unsloth Qwen3.5 9B (Q8). If a llama-server with the same model is already running (Unsloth Studio) it is used as it is, "
                            "with no extra GPU memory. Otherwise this app loads its own copy (about 12 GB) only while prompts are written or characters described, then lets the GPU go back to ComfyUI.",
                            WriterStatusText().c_str());
        ImGui::PopTextWrapPos();
        ClefSettings w = ClefGetSettings();
        bool wc = false;
        std::string wm = ToUtf8(w.writerModel), wmm = ToUtf8(w.writerMmproj);
        if (InputStr("##writermodel", "the writer's .gguf model", wm, PathBoxWidth())) {
            w.writerModel = ToWide(wm);
            wc = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Browse...##writermodel")) {
            const std::wstring f = PickFile(L"GGUF models\0*.gguf\0\0", L"Choose the prompt writer's .gguf");
            if (!f.empty()) {
                w.writerModel = f;
                wc = true;
            }
        }
        if (InputStr("##writermm", "its mmproj file (lets it look at pictures; optional)", wmm, PathBoxWidth())) {
            w.writerMmproj = ToWide(wmm);
            wc = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Browse...##writermm")) {
            const std::wstring f = PickFile(L"GGUF files\0*.gguf\0\0", L"Choose the writer's mmproj .gguf");
            if (!f.empty()) {
                w.writerMmproj = f;
                wc = true;
            }
        }
        if (ImGui::Checkbox("Use one that is already running##writerrun", &w.writerUseRunning)) wc = true;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Use a llama-server that is already running this model (Unsloth Studio): no extra GPU memory");
        ImGui::SetNextItemWidth(ui::Dp(200.0f));
        if (ImGui::SliderInt("GPU layers##writergpu", &w.writerGpuLayers, 0, 99)) wc = true;
        if (wc) ClefSetSettings(w);
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Set up Clef")) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("It runs as a local llama.cpp server. The program is found in tools\\llama-cpp-cuda (the CPU build is in tools\\llama-cpp) and the model (Clef-Flash-Q4_K_M.gguf, 6.5 GB) "
                            "in tools\\clef, next to this program, or choose other files below. On the GPU (GPU layers 99) a whole set of choices takes under a second but the model needs about "
                            "7 GB of GPU memory, so the two programs take turns: each round ComfyUI is asked to unload its models only if there is not enough free memory (then Ollama's), "
                            "Clef loads and plans the round's pictures, Clef is closed, and ComfyUI loads Qwen once and makes them all.");
        ImGui::PopTextWrapPos();
        if (ImGui::SmallButton("llama.cpp releases")) OpenUrl(L"https://github.com/ggml-org/llama.cpp/releases");
        ImGui::SameLine();
        if (ImGui::SmallButton("Clef-Flash GGUF")) OpenUrl(L"https://huggingface.co/ggml-org/Clef-Flash-GGUF");
        std::string url = s.url, exe = ToUtf8(s.exe), model = ToUtf8(s.model);
        if (InputStr("##clefurl", "http://127.0.0.1:8099", url, std::min(ui::Dp(260.0f), ImGui::GetContentRegionAvail().x * 0.6f))) {
            s.url = url;
            changed = true;
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("server address");
        if (InputStr("##clefexe", "llama-server.exe", exe, PathBoxWidth())) {
            s.exe = ToWide(exe);
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Browse...##clefexe")) {
            const std::wstring f = PickFile(L"llama-server.exe\0llama-server.exe\0Programs\0*.exe\0\0", L"Choose llama-server.exe");
            if (!f.empty()) {
                s.exe = f;
                changed = true;
            }
        }
        if (InputStr("##clefmodel", "Clef-Flash-Q4_K_M.gguf", model, PathBoxWidth())) {
            s.model = ToWide(model);
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Browse...##clefmodel")) {
            const std::wstring f = PickFile(L"GGUF models\0*.gguf\0\0", L"Choose the Clef .gguf file");
            if (!f.empty()) {
                s.model = f;
                changed = true;
            }
        }
        ImGui::SetNextItemWidth(ui::Dp(200.0f));
        if (ImGui::SliderInt("GPU layers (0 = CPU)##clefgpu", &s.gpuLayers, 0, 99)) changed = true;
        ImGui::SetNextItemWidth(ui::Dp(200.0f));
        float t = static_cast<float>(s.temperature);
        if (ImGui::SliderFloat("Variety##cleft", &t, 1.0f, 3.0f, "%.1f")) {
            s.temperature = t;
            changed = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("1 = follow Clef exactly; higher = more surprising choices");
        if (ImGui::Checkbox("Start it with Clef mode", &s.autoStart)) changed = true;
        std::string msg;
        if (ImGui::Button(up ? "Clef is running" : "Start Clef now")) {
            ClefSetSettings(s);
            changed = false;
            if (!up) {
                ClefStart(msg);
                g_run.status = msg;
            }
        }
        if (ClefOwned()) {
            ImGui::SameLine();
            if (ImGui::Button("Stop Clef")) ClefStopOwned();
        }
        if (!g_run.status.empty() && !g_run.running) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("%s", g_run.status.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::TreePop();
    }
    if (changed) ClefSetSettings(s);
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------
std::string ClefTabLabel(AppState&) {
    const int n = Pending();
    return (g_run.running ? (n > 0 ? "Clef mode (" + std::to_string(n) + ")" : std::string("Clef mode (on)")) : std::string("Clef mode")) + "###clefmodetab";
}

void AddToClefCast(AppState& app, const std::vector<std::wstring>& paths) {
    LoadCast();
    int added = 0;
    for (const std::wstring& p : paths) added += AddCastImage(p) ? 1 : 0;
    if (added > 0) SaveCast();
    app.SetStatus(added > 0 ? "Added " + std::to_string(added) + (added == 1 ? " picture" : " pictures") + " to the Clef cast (Clef mode tab)" : "Already in the Clef cast");
}

void PumpClefMode(AppState& app) {
    LoadCast();
    CheckCast(app, false);
    Advance(app);
}

void DrawClefTab(AppState& app, float height) {
    LoadCast();
    if (!app.comfySvc) {
        app.comfySvc = std::make_unique<ComfyService>();
    }
    if (!app.comfyLoaded) {
        app.comfyLoaded = true;
        LoadComfySettings(app.comfy);
    }
    if (!ImGui::BeginTable("clef_layout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings, ImVec2(0, height))) return;
    ImGui::TableSetupColumn("Controls", ImGuiTableColumnFlags_WidthStretch, 0.36f);
    ImGui::TableSetupColumn("Feed", ImGuiTableColumnFlags_WidthStretch, 0.64f);
    ImGui::TableNextRow(0, height);

    // ---- controls
    ImGui::TableSetColumnIndex(0);
    ImGui::BeginChild("clef_ctl", ImVec2(0, height), true);
    ImGui::PushFont(ui::BoldFont());
    ImGui::TextUnformatted("Clef mode");
    ImGui::PopFont();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Clef makes the choices for you and keeps going: who is in the picture, where and when, the mood, what they do together, the camera, and what each one wears, "
                        "how they stand and what their face does. Every scenario becomes a picture edit with Qwen (the characters' pictures are used), or a text prompt for Krea.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    const std::vector<CastMember> pool = Pool();
    if (g_run.running) {
        if (ui::PrimaryButton("Stop", ImVec2(ui::Dp(120.0f), 0))) {
            g_run.running = false;
            g_run.status = "Stopped (pictures ComfyUI already has will still be made)";
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s (%d sent)", (g_run.planning || g_run.writeBusy || g_run.describeBusy) ? GetPhase().c_str() : g_run.status.c_str(), g_run.started);
    } else {
        ImGui::BeginDisabled(pool.empty());
        if (ui::PrimaryButton("Start", ImVec2(ui::Dp(120.0f), 0))) {
            g_run.running = true;
            g_run.started = 0;
            g_run.oneShot = false;
            g_run.status.clear();
            g_run.storyState = StoryState();
        }
        ImGui::SameLine();
        if (ImGui::Button("One surprise")) {
            g_run.running = true;
            g_run.started = 0;
            g_run.oneShot = true;
            g_run.status.clear();
        }
        ImGui::EndDisabled();
        if (!g_run.status.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", g_run.status.c_str());
        }
    }
    ImGui::Spacing();
    ImGui::Separator();

    {
        int on = 0;
        for (const CastItem& m : g_cast) on += m.on ? 1 : 0;
        ImGui::TextUnformatted("Cast");
        ImGui::SameLine();
        ImGui::TextDisabled("(%d of %d in the pool)", on, static_cast<int>(g_cast.size()));
        bool changed = false;
        if (ImGui::SmallButton("Import saved characters")) {
            int added = 0;
            for (const CharacterProfile& p : AllProfiles()) {
                if (p.saved && AddCastImage(p.image)) ++added;
            }
            app.SetStatus(added > 0 ? "Added " + std::to_string(added) + " saved characters to the Clef cast" : "No new saved characters to add");
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("+ File...")) {
            if (AddCastImage(OpenImageFileDialog(GetActiveWindow()))) changed = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("+ Create...")) OpenCharacterCreator(app);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Create a character (Characters tab), then Import saved characters or \"Add to Clef mode's cast\"");
        ImGui::SameLine();
        if (ImGui::SmallButton("All on")) {
            for (CastItem& m : g_cast) m.on = true;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("All off")) {
            for (CastItem& m : g_cast) m.on = false;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) {
            g_cast.clear();
            changed = true;
        }
        if (!g_castRemoved.empty()) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(ui::kWarn, "Removed from the cast because their picture is gone (moved, renamed or deleted): %s", g_castRemoved.c_str());
            ImGui::PopTextWrapPos();
            if (ImGui::SmallButton("OK##castremoved")) g_castRemoved.clear();
        }
        ImGui::SetNextItemWidth(ui::Dp(200.0f));
        if (ImGui::SliderFloat("Card size##castsize", &g_castSize, 60.0f, 400.0f, "%.0f")) changed = true;
        ImGui::BeginChild("##castbox", ImVec2(0, ui::Dp(g_castH)), true);
        const float card = ui::Dp(g_castSize);
        const ImGuiStyle& st = ImGui::GetStyle();
        int removeAt = -1;
        const float lineH = ImGui::GetTextLineHeight();
        for (size_t i = 0; i < g_cast.size(); ++i) {
            CastItem& m = g_cast[i];
            ImGui::PushID(static_cast<int>(i));
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##card", ImVec2(card, card + lineH + 6.0f));
            const bool hov = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                m.on = !m.on;
                changed = true;
            }
            if (ImGui::BeginPopupContextItem("##cardmenu")) {
                char nameBuf[128];
                snprintf(nameBuf, sizeof(nameBuf), "%s", m.name.c_str());
                ImGui::SetNextItemWidth(ui::Dp(260.0f));
                if (ImGui::InputText("Name##castname", nameBuf, sizeof(nameBuf))) {
                    m.name = nameBuf;
                    changed = true;
                }
                if (InputStr("##castdesc", "what they look like (helps the choices fit them)", m.description, ui::Dp(260.0f))) changed = true;
                if (ImGui::MenuItem("Open in the gallery")) {
                    app.OpenFile(m.image);
                    app.requestTab = 0;
                }
                if (ImGui::MenuItem("Remove from the cast")) removeAt = static_cast<int>(i);
                ImGui::EndPopup();
            }
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(pos, ImVec2(pos.x + card, pos.y + card + lineH + 6.0f), IM_COL32(30, 32, 38, 255), ui::Dp(6.0f));
            const AppState::LibThumb t = app.LibraryThumb(m.image);
            if (t.srv && t.w > 0 && t.h > 0) {
                const float s = std::min((card - 8.0f) / t.w, (card - 8.0f) / t.h);
                const ImVec2 a(pos.x + (card - t.w * s) * 0.5f, pos.y + 4.0f + (card - 8.0f - t.h * s) * 0.5f);
                dl->AddImage((ImTextureID)(intptr_t)t.srv, a, ImVec2(a.x + t.w * s, a.y + t.h * s), ImVec2(0, 0), ImVec2(1, 1), m.on ? IM_COL32_WHITE : IM_COL32(110, 110, 110, 255));
            }
            dl->PushClipRect(ImVec2(pos.x + 3, pos.y + card), ImVec2(pos.x + card - 3, pos.y + card + lineH + 6.0f), true);
            dl->AddText(ImVec2(pos.x + 5, pos.y + card + 2.0f), m.on ? IM_COL32(235, 235, 240, 255) : IM_COL32(130, 130, 140, 255), (m.name.empty() ? "(no name)" : m.name.c_str()));
            dl->PopClipRect();
            if (m.on) dl->AddRect(pos, ImVec2(pos.x + card, pos.y + card + lineH + 6.0f), ui::WithAlpha(ui::kAccent, 0.95f), ui::Dp(6.0f), 0, ui::Dp(2.0f));
            else dl->AddRectFilled(pos, ImVec2(pos.x + card, pos.y + card), IM_COL32(0, 0, 0, 90), ui::Dp(6.0f));
            if (m.missing) { // its drive is not there: out of the pool until it is back
                dl->AddRectFilled(pos, ImVec2(pos.x + card, pos.y + card), IM_COL32(0, 0, 0, 150), ui::Dp(6.0f));
                const char* t = "drive not found";
                const ImVec2 ts = ImGui::CalcTextSize(t);
                dl->AddText(ImVec2(pos.x + (card - ts.x) * 0.5f, pos.y + (card - ts.y) * 0.5f), ImGui::GetColorU32(ui::kWarn), t);
            }
            if (hov) ImGui::SetTooltip("%s\n%s\nClick to %s it, right click to rename, describe or remove", m.name.c_str(), m.description.empty() ? "(no description yet: right click to add one)" : m.description.c_str(),
                                       m.on ? "leave" : "put");
            ImGui::PopID();
            if (pos.x + card + st.ItemSpacing.x + card <= ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x) ImGui::SameLine();
        }
        if (removeAt >= 0) {
            g_cast.erase(g_cast.begin() + removeAt);
            changed = true;
        }
        if (g_cast.empty()) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(ui::kWarn, "The cast is empty. Drop pictures here from the gallery or the Vision library, press Import saved characters, or use \"Add to Clef cast\" in a picture's menu anywhere.");
            ImGui::PopTextWrapPos();
        }
        ImGui::EndChild();
        if (ImGui::BeginDragDropTarget()) { // pictures dragged onto the cast
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("MV_LIBITEM")) {
                if (AddCastImage(std::wstring(static_cast<const wchar_t*>(pl->Data)))) changed = true;
            } else if (const ImGuiPayload* pg = ImGui::AcceptDragDropPayload("MV_IMAGES")) {
                const int* idx = static_cast<const int*>(pg->Data);
                for (int k = 0; k < static_cast<int>(pg->DataSize / sizeof(int)); ++k) {
                    if (idx[k] >= 0 && idx[k] < static_cast<int>(app.entries.size()) && AddCastImage(app.entries[idx[k]].path)) changed = true;
                }
            }
            ImGui::EndDragDropTarget();
        }
        // the bottom edge of the box: drag to make it taller or shorter
        const ImVec2 gp = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##castgrip", ImVec2(-FLT_MIN, ui::Dp(8.0f)));
        if (ImGui::IsItemActive()) {
            g_castH = std::clamp(g_castH + ImGui::GetIO().MouseDelta.y / ui::Scale(), 120.0f, 900.0f);
            changed = true;
        }
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(gp.x + ImGui::GetContentRegionAvail().x * 0.4f, gp.y + 2), ImVec2(gp.x + ImGui::GetContentRegionAvail().x * 0.6f, gp.y + 5),
                                                  IM_COL32(120, 125, 140, 200), 2.0f);
        if (changed) SaveCast();
    }
    ImGui::Spacing();
    ImGui::SetNextItemWidth(ui::Dp(150.0f));
    ImGui::SliderInt("Characters per picture: at least##clefmin", &g_run.minChars, 1, 4);
    ImGui::SetNextItemWidth(ui::Dp(150.0f));
    ImGui::SliderInt("...at most##clefmax", &g_run.maxChars, 1, 4);
    g_run.maxChars = std::max(g_run.maxChars, g_run.minChars);
    ImGui::Checkbox("Keep going until I stop it", &g_run.untilStopped);
    if (!g_run.untilStopped) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ui::Dp(90.0f));
        ImGui::InputInt("pictures##clefn", &g_run.target, 1, 5);
        g_run.target = std::clamp(g_run.target, 1, 500);
    }
    ImGui::SetNextItemWidth(ui::Dp(150.0f));
    ImGui::SliderInt("Ideas Clef judges per picture##clefbest", &g_run.bestOf, 1, 8);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("Clef invents this many candidate scenarios for every picture and JUDGES each one (\"how good is this idea: nonsense, boring, fun or great?\").\nOnly the best is made. 1 = no judging. Needs Clef running.");
    ImGui::Checkbox("Pixel art mode##clefpx", &g_run.pixel);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Every picture is made as usual, then also becomes real pixel art: it shows next to the picture here and in the Pixel art tab (set \"Clef mode\")");
    if (g_run.pixel) PixelArtModeOptions("clefpxopt", g_run.pixelLook);
    ImGui::Checkbox("Krea: send text prompts (no Qwen edit)##clefkrea", &g_run.toKrea);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("Each scenario becomes a Vision board (the characters, the place, what they do, the mood, the camera, what each wears) and the board's scene prompt\nis sent to the workflow chosen on the Generate page (your Krea one). The characters' pictures are not used, only their descriptions.");
    if (g_run.toKrea) {
        ImGui::Indent(ui::Dp(18.0f));
        ImGui::BeginDisabled(!WriterConfigured());
        ImGui::Checkbox("The prompt writer (LLM) writes it##clefkreawrite", &g_run.kreaWrite);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("The LLM turns the winning scenario into a proper Krea prompt (otherwise the board's quick draft is sent)");
        if (!WriterConfigured()) ImGui::TextDisabled("(set up the prompt writer below)");
        else ImGui::TextDisabled("Writer: %s", WriterStatusText().c_str());
        ImGui::Unindent(ui::Dp(18.0f));
    }
    ImGui::BeginDisabled(g_run.toKrea);
    ImGui::Checkbox("Clef checks the faces afterwards##cleffaces", &g_run.faceCheck);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("After a round Clef LOOKS at every finished picture next to the picture of each character in it, and says how well the face was kept.\nPictures where it drifted are marked. Needs Clef's picture file (mmproj) next to the model.");
    ImGui::Checkbox("Tell a story (each picture is the next scene)##clefstory", &g_run.story);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("Clef picks each scene as the continuation of the last ones: some of the same characters carry over, and it chooses the moment of the story\n(arriving, a surprise, an argument, a chase, making up...) given what already happened.");
    ImGui::SetNextItemWidth(ui::Dp(150.0f));
    ImGui::SliderInt("Pictures per round##clefbatch", &g_run.batch, 1, 30);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("One round: Clef loads and plans this many scenarios, then unloads, and ComfyUI loads Qwen once and makes them all.\nA bigger number means fewer swaps of the GPU between the two.");
    {
        ImGui::TextUnformatted("How freely to change the characters");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##clefstyle", EditStyleName(std::max(1, app.comfy.editStyle)))) {
            for (int m = 1; m <= 2; ++m) {
                if (ImGui::Selectable(EditStyleName(m), app.comfy.editStyle == m)) {
                    app.comfy.editStyle = m;
                    SaveComfySettings(app.comfy);
                }
            }
            ImGui::EndCombo();
        }
        DrawEditModelPicker(app.comfy, "clefmodel");
        bool changed = false;
        if (ImGui::Checkbox("outfits##clefout", &app.comfy.directOutfit)) changed = true;
        ImGui::SameLine();
        if (ImGui::Checkbox("poses##clefpose", &app.comfy.directPose)) changed = true;
        ImGui::SameLine();
        if (ImGui::Checkbox("expressions##clefexp", &app.comfy.directExpression)) changed = true;
        if (changed) SaveComfySettings(app.comfy);
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Quality and size are the ones of the edit window (the quality slider there); the shape of each picture is chosen too.");
        ImGui::PopTextWrapPos();
    }
    ImGui::Spacing();
    ImGui::Separator();
    DrawClefSettings(app);
    ImGui::EndChild();

    // ---- the feed
    ImGui::TableSetColumnIndex(1);
    ImGui::BeginChild("clef_feed", ImVec2(0, height), true);
    if (g_rounds.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Nothing yet. Press Start (or One surprise) and the pictures will show up here as they are made. They are also in the Edits tab.");
        ImGui::PopTextWrapPos();
    }
    for (size_t k = g_rounds.size(); k-- > 0;) {
        Round& r = g_rounds[k];
        ImGui::PushID(r.id);
        const float th = ui::Dp(150.0f);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##open", ImVec2(th, th)) && !r.result.empty()) {
            app.OpenFile(r.result);
            app.requestTab = 0;
        }
        const bool hov = ImGui::IsItemHovered();
        ImGui::SetCursorScreenPos(pos);
        Thumb(app, r.result.empty() ? (r.images.empty() ? std::wstring() : r.images.front()) : r.result, ImVec2(th, th));
        if (hov && !r.result.empty()) ImGui::SetTooltip("Click to open it in the gallery");
        if (r.pixelJob > 0) { // pixel art mode: the pixel art next to it
            ImGui::SameLine();
            if (!PixelArtThumb(app, r.pixelJob, th)) ImGui::Dummy(ImVec2(1, th));
        }
        if (r.result.empty() && r.error.empty()) {
            ImGui::GetWindowDrawList()->AddRectFilled(pos, ImVec2(pos.x + th, pos.y + th), IM_COL32(0, 0, 0, 120), ui::Dp(5.0f));
            const std::string t = r.state.empty() ? "..." : r.state;
            const ImVec2 ts = ImGui::CalcTextSize(t.c_str());
            ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x + (th - ts.x) * 0.5f, pos.y + (th - ts.y) * 0.5f), IM_COL32(255, 255, 255, 230), t.c_str());
        }
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::max(ui::Dp(200.0f), ImGui::GetContentRegionAvail().x - ui::Dp(8.0f)));
        ImGui::TextUnformatted(r.summary.c_str());
        ImGui::TextDisabled("%s", r.note.c_str());
        if (!r.matchNote.empty()) ImGui::TextColored(r.weakMatch ? ui::kWarn : ui::kGood, "%s", r.matchNote.c_str());
        if (!r.faceNote.empty()) ImGui::TextColored(r.drifted ? ui::kWarn : ui::kGood, "Face check (Clef): %s%s", r.faceNote.c_str(), r.drifted ? "  - the face drifted" : "");
        if (!r.error.empty()) ImGui::TextColored(ui::kBad, "%s", r.error.c_str());
        else if (r.result.empty()) ImGui::TextDisabled("%s", r.message.c_str());
        ImGui::TextUnformatted(r.details.c_str());
        if (!r.prompt.empty()) ImGui::TextDisabled("Prompt sent: %s", r.prompt.c_str());
        else if (r.writing) ImGui::TextDisabled("The vision model is writing the prompt...");
        ImGui::PopTextWrapPos();
        if (!r.result.empty()) {
            if (ImGui::SmallButton("Edit again...")) OpenPictureEditor(app, {r.result});
            ImGui::SameLine();
            if (ImGui::SmallButton("Upscale...")) OpenUpscaler(app, {r.result});
            ImGui::SameLine();
            if (ImGui::SmallButton("Design...")) OpenDesigner(app, {r.result});
            ImGui::SameLine();
            PixelArtButton(app, {r.result}, "##pxquickclef");
            ImGui::SameLine();
            if (ImGui::SmallButton("Open the Edits tab")) app.requestTab = 6, SetEditsMode(0);
        }
        ImGui::EndGroup();
        ImGui::Separator();
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::EndTable();
}

void ClefShutdown() { LlmStopOwned(); }
