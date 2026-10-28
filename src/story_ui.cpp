// The Video page's "Make a story" mode: a short drama from your characters - the kind of AI story video that does well on TikTok.
//   1. the cast (saved characters, and who they are in the story), the idea and the length;
//   2. the prompt writer (the local LLM) turns the idea into scenes: the place, who is in it, the shot, what happens and what is said;
//   3. every scene gets its first frame from Qwen Image Edit (the cast's own pictures as references, so faces and clothes stay the same),
//      then a video with sound and speech from MiniMax H3 (a video prompt written by looking at that frame);
//   4. the clips are joined into one video with ffmpeg, the spoken lines burned in as captions if asked.
// "Make everything" does it in that order - all pictures, all video prompts, all videos, then the join - so each model loads only once and
// the pictures can be checked (and remade) before the long part. The story is kept in %LOCALAPPDATA%\Try2Create\story.json.
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include "app_state.h"
#include "board.h"
#include "clef.h"
#include "comfy.h"
#include "imgui.h"
#include "json.hpp"
#include "notify.h"
#include "ui_extra.h"
#include "ui_theme.h"
#include "video_util.h"
#include "vision_prompt.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::wstring DataDir() {
    wchar_t buf[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    return std::wstring(buf) + L"\\Try2Create";
}

std::string Trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n*-");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n*");
    return s.substr(a, b - a + 1);
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// a file name from a title ("Rich vs Poor!" -> "Rich_vs_Poor")
std::string SafeName(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c)) out += static_cast<char>(c);
        else if (!out.empty() && out.back() != '_') out += '_';
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    if (out.size() > 40) out.resize(40);
    return out.empty() ? std::string("story") : out;
}

// ---- the story
struct CastMember {
    std::wstring image;
    std::string name, role; // "Marcus", "the rich man"
    std::string voice;      // said in every scene's prompt: H3 makes a new voice per clip otherwise
};

struct Scene {
    int uid = 0;            // stays with the scene when scenes move (its jobs are found by it)
    std::string place, who, shot, action, lines; // lines: one "Name: words" per line
    double seconds = 7;
    std::wstring picture, clip;
    std::string videoPrompt;
    int pictureAct = 0;     // its picture is being made (the activity's id)
    bool clipPending = false; // its video was sent and is not back yet
    std::string error;
    bool cont = false;      // it carries straight on from the scene before (same place and people, no cut): with H3 Continuum both are one video
};

struct Story {
    std::string id; // in the jobs' labels: "[<id>-<uid>]"
    std::string title = "My story", idea;
    std::vector<CastMember> cast;
    std::vector<Scene> scenes;
    int nextUid = 1;
    double length = 105;         // seconds the whole video should last
    int quality = 1, res = 1;    // H3: Balanced, 640p
    int shape = 0;               // 0 portrait 9:16 (TikTok, Reels, Shorts), 1 landscape 16:9
    bool captions = true;
    bool fullHd = true;          // the joined video at 1080 x 1920 (or 1920 x 1080): what TikTok, Reels and Shorts expect
    std::wstring music;          // one soundtrack under the whole story (the clips themselves are made without music)
    double musicVolume = 0.25;
    std::wstring finalVideo;
    int step = 0;                // "Make everything": 0 not running, 1 pictures, 2 video prompts, 3 videos, 4 joining
};
Story g_s;
bool g_loaded = false;
bool g_dirty = false;
double g_dirtyAt = 0;

std::mutex g_statusMutex;
std::string g_status; // what the writer / the join is doing

void SetStatus(const std::string& s) {
    std::lock_guard<std::mutex> lock(g_statusMutex);
    g_status = s;
}
std::string Status() {
    std::lock_guard<std::mutex> lock(g_statusMutex);
    return g_status;
}

std::wstring StoryPath() { return DataDir() + L"\\story.json"; }

std::string NewId() {
    static std::mt19937 rng{std::random_device{}()};
    char buf[16];
    snprintf(buf, sizeof(buf), "%06x", static_cast<unsigned>(rng() & 0xFFFFFF));
    return buf;
}

void Save() {
    json j;
    j["id"] = g_s.id;
    j["title"] = g_s.title;
    j["idea"] = g_s.idea;
    j["length"] = g_s.length;
    j["quality"] = g_s.quality;
    j["res"] = g_s.res;
    j["shape"] = g_s.shape;
    j["captions"] = g_s.captions;
    j["fullHd"] = g_s.fullHd;
    j["music"] = Utf8(g_s.music);
    j["musicVolume"] = g_s.musicVolume;
    j["nextUid"] = g_s.nextUid;
    j["final"] = Utf8(g_s.finalVideo);
    j["cast"] = json::array();
    for (const CastMember& c : g_s.cast) j["cast"].push_back({{"image", Utf8(c.image)}, {"name", c.name}, {"role", c.role}, {"voice", c.voice}});
    j["scenes"] = json::array();
    for (const Scene& sc : g_s.scenes)
        j["scenes"].push_back({{"uid", sc.uid}, {"place", sc.place}, {"who", sc.who}, {"shot", sc.shot}, {"action", sc.action}, {"lines", sc.lines},
                               {"seconds", sc.seconds}, {"picture", Utf8(sc.picture)}, {"clip", Utf8(sc.clip)}, {"videoPrompt", sc.videoPrompt}, {"cont", sc.cont}});
    std::error_code ec;
    fs::create_directories(DataDir(), ec);
    std::ofstream f(StoryPath(), std::ios::binary | std::ios::trunc);
    if (f) f << j.dump(1, ' ', false, json::error_handler_t::replace);
    g_dirty = false;
}

void Changed() {
    g_dirty = true;
    g_dirtyAt = ImGui::GetTime();
}

void Load() {
    g_loaded = true;
    std::ifstream f(StoryPath(), std::ios::binary);
    json j = f ? json::parse(f, nullptr, false) : json();
    if (!j.is_object()) {
        g_s.id = NewId();
        return;
    }
    g_s.id = j.value("id", NewId());
    g_s.title = j.value("title", g_s.title);
    g_s.idea = j.value("idea", std::string());
    g_s.length = std::clamp(j.value("length", 105.0), 20.0, 300.0);
    g_s.quality = std::clamp(j.value("quality", 1), 0, 2);
    g_s.res = std::clamp(j.value("res", 1), 0, 2);
    g_s.shape = std::clamp(j.value("shape", 0), 0, 1);
    g_s.captions = j.value("captions", true);
    g_s.fullHd = j.value("fullHd", true);
    g_s.music = Wide(j.value("music", std::string()));
    g_s.musicVolume = std::clamp(j.value("musicVolume", 0.25), 0.02, 1.0);
    g_s.nextUid = j.value("nextUid", 1);
    g_s.finalVideo = Wide(j.value("final", std::string()));
    if (j.contains("cast") && j["cast"].is_array()) {
        for (const json& c : j["cast"])
            g_s.cast.push_back({Wide(c.value("image", std::string())), c.value("name", std::string()), c.value("role", std::string()), c.value("voice", std::string())});
    }
    if (j.contains("scenes") && j["scenes"].is_array()) {
        for (const json& c : j["scenes"]) {
            Scene sc;
            sc.uid = c.value("uid", g_s.nextUid++);
            sc.place = c.value("place", std::string());
            sc.who = c.value("who", std::string());
            sc.shot = c.value("shot", std::string());
            sc.action = c.value("action", std::string());
            sc.lines = c.value("lines", std::string());
            sc.seconds = std::clamp(c.value("seconds", 7.0), 3.0, 15.0);
            sc.picture = Wide(c.value("picture", std::string()));
            sc.clip = Wide(c.value("clip", std::string()));
            sc.videoPrompt = c.value("videoPrompt", std::string());
            sc.cont = c.value("cont", false);
            std::error_code ec;
            if (!sc.picture.empty() && !fs::exists(sc.picture, ec)) sc.picture.clear();
            if (!sc.clip.empty() && !fs::exists(sc.clip, ec)) sc.clip.clear();
            g_s.nextUid = std::max(g_s.nextUid, sc.uid + 1);
            g_s.scenes.push_back(std::move(sc));
        }
    }
}

// who is in a scene: the cast members its WHO names, in that order
std::vector<int> WhoIn(const Scene& sc) {
    std::vector<int> out;
    const std::string who = Lower(sc.who);
    std::vector<std::pair<size_t, int>> found; // where in WHO, which cast member
    for (size_t k = 0; k < g_s.cast.size(); ++k) {
        std::string name = Lower(Trim(g_s.cast[k].name));
        if (const size_t sp = name.find(' '); sp != std::string::npos) name.resize(sp); // the first name is enough
        if (name.empty()) continue;
        const size_t at = who.find(name);
        if (at != std::string::npos) found.emplace_back(at, static_cast<int>(k));
    }
    std::sort(found.begin(), found.end());
    for (const auto& f : found) out.push_back(f.second);
    if (out.size() > 4) out.resize(4);
    return out;
}

std::string CastLine(int k) {
    const CastMember& c = g_s.cast[k];
    return c.name + (c.role.empty() ? std::string() : " (" + c.role + ")");
}

// a voice for a cast member that has none: told apart from the others of the same sex
std::string DefaultVoice(int k) {
    const CharacterProfile* p = FindProfile(g_s.cast[k].image);
    const std::string d = Lower((p ? p->description : std::string()) + " " + g_s.cast[k].role);
    const bool woman = d.find("woman") != std::string::npos || d.find("girl") != std::string::npos || d.find(" she ") != std::string::npos;
    const bool old = d.find("senior") != std::string::npos || d.find("middle-aged") != std::string::npos;
    int same = 0; // how many of the same sex come before
    for (int i = 0; i < k; ++i) {
        const CharacterProfile* q = FindProfile(g_s.cast[i].image);
        const std::string e = Lower((q ? q->description : std::string()) + " " + g_s.cast[i].role);
        same += ((e.find("woman") != std::string::npos || e.find("girl") != std::string::npos) == woman) ? 1 : 0;
    }
    static const char* kMen[] = {"a deep, smooth, self-assured man's voice", "a soft, warm, slightly husky young man's voice", "a bright, quick, energetic man's voice",
                                 "a gravelly, slow man's voice"};
    static const char* kWomen[] = {"a clear, warm young woman's voice", "a low, smoky woman's voice", "a light, playful woman's voice", "a calm, mature woman's voice"};
    std::string v = woman ? kWomen[same % 4] : kMen[same % 4];
    if (old) v = "an older, " + v.substr(2);
    return v;
}

std::string VoiceOf(int k) { return g_s.cast[k].voice.empty() ? DefaultVoice(k) : g_s.cast[k].voice; }

// what a cast member looks like, from their saved character
std::string Looks(int k) {
    const CharacterProfile* p = FindProfile(g_s.cast[k].image);
    return p ? p->description : std::string();
}

// the job labels: the scene is found by "[<story id>-<uid>]" whatever its number is by then
std::string Tag(const Scene& sc) { return "[" + g_s.id + "-" + std::to_string(sc.uid) + "]"; }
int SceneNumber(const Scene& sc) {
    for (size_t k = 0; k < g_s.scenes.size(); ++k) {
        if (g_s.scenes[k].uid == sc.uid) return static_cast<int>(k) + 1;
    }
    return 0;
}

// ---- 1. the writer turns the idea into scenes
const char* const kStorySystem =
    "You write short vertical-video drama stories: the viral AI-made kind on TikTok. Simple and emotional, easy to follow, with a hook in the "
    "first seconds, a clear conflict that escalates, a twist and a satisfying ending. Each scene becomes ONE continuous shot of 5 to 8 seconds "
    "made by a video model that also makes the sound and the speech.\n"
    "Rules:\n"
    "- Use only the characters listed, by their exact names. At most 3 characters in a scene, usually 1 or 2.\n"
    "- Each scene: one place, one simple, clearly visible action (people turn, walk, hand over things, slam a door, hug, cry, laugh, kneel...), "
    "and at most 2 short spoken lines, 12 words at most for the whole scene: they are spoken aloud in 5 to 8 seconds. A scene may have no "
    "lines (then write no SAYS line at all).\n"
    "- Use 3 or 4 places and move to another place at least every 4 scenes (a story that stays in one place looks like one long scene). "
    "When a place comes back, copy its PLACE line word for word (never write \"same place\").\n"
    "- In ACTION always name people (\"Marcus hands Sofia the box\"), never just he / she: each scene is drawn on its own.\n"
    "- Vary the shots: close-ups for emotion, medium shots for talking, wide shots for arrivals.\n"
    "- Most stories have moments where a scene runs straight on from the one before: the same place and the same people, the conversation "
    "goes on, a reaction right after a line, a gesture, someone takes or hands over a thing a person is already holding. Mark those with "
    "CONTINUES: yes - usually once or twice in each place. Such a scene is made as one continuous shot with the scene before (the voices and "
    "the sound of the place carry on), and the camera keeps its view. So do not mark a scene whose action needs something new in the picture "
    "(a table, car, door or object that was not in the scene before) or a walk to another spot. At most 2 scenes in a row continue, never "
    "scene 1, never when the place changes.\n"
    "- Write exactly {N} scenes.\n"
    "Answer in exactly this format and nothing else:\n"
    "TITLE: <a catchy title>\n"
    "SCENE 1\n"
    "PLACE: <where, the time of day and the light>\n"
    "WHO: <names, comma separated>\n"
    "SHOT: <close-up / medium shot / wide shot, and of whom>\n"
    "ACTION: <what visibly happens, one or two sentences>\n"
    "SAYS: <Name>: <the line>\n"
    "SAYS: <Name>: <the line>\n"
    "SECONDS: <5 to 8>\n"
    "CONTINUES: yes   (only when it carries on from the scene before; otherwise leave this line out)\n"
    "SCENE 2\n"
    "...";

struct ScenesResult {
    std::vector<Scene> scenes;
    std::string title, error;
};
std::future<ScenesResult> g_scenesFut;

std::vector<Scene> ParseScenes(const std::string& text, std::string& title) {
    std::vector<Scene> out;
    size_t a = 0;
    while (a < text.size()) {
        size_t b = text.find('\n', a);
        if (b == std::string::npos) b = text.size();
        const std::string line = Trim(text.substr(a, b - a));
        a = b + 1;
        if (line.empty()) continue;
        const std::string up = Lower(line);
        auto after = [&](size_t n) { return Trim(line.substr(std::min(n, line.size()))); };
        if (up.rfind("title:", 0) == 0) title = after(6);
        else if (up.rfind("scene", 0) == 0 && (up.size() <= 9 || up.find(':') == std::string::npos || up.find(':') > 8)) out.emplace_back();
        else if (out.empty()) continue;
        else if (up.rfind("place:", 0) == 0) out.back().place = after(6);
        else if (up.rfind("who:", 0) == 0) out.back().who = after(4);
        else if (up.rfind("shot:", 0) == 0) out.back().shot = after(5);
        else if (up.rfind("action:", 0) == 0) out.back().action = after(7);
        else if (up.rfind("says:", 0) == 0) {
            std::string l = after(5);
            if (l.size() > 1 && (l.front() == '"' || l.front() == '\'')) l = l.substr(1);
            out.back().lines += (out.back().lines.empty() ? "" : "\n") + l;
        } else if (up.rfind("seconds:", 0) == 0) {
            out.back().seconds = std::clamp(std::atof(after(8).c_str()), 4.0, 10.0);
            if (out.back().seconds <= 0) out.back().seconds = 7;
        } else if (up.rfind("continues:", 0) == 0) {
            out.back().cont = Lower(after(10)).rfind("yes", 0) == 0;
        }
    }
    out.erase(std::remove_if(out.begin(), out.end(), [](const Scene& s) { return s.action.empty() && s.lines.empty(); }), out.end());
    for (size_t i = 0; i < out.size(); ++i) {
        Scene& sc = out[i];
        // lines: only "Name: words" (the writer also writes "(No dialogue in this scene)"), without the quotes around the words
        std::string kept;
        size_t a2 = 0;
        int words = 0;
        while (a2 < sc.lines.size()) {
            size_t b2 = sc.lines.find('\n', a2);
            if (b2 == std::string::npos) b2 = sc.lines.size();
            std::string l = Trim(sc.lines.substr(a2, b2 - a2));
            a2 = b2 + 1;
            const size_t colon = l.find(':');
            if (l.empty() || l.front() == '(' || colon == std::string::npos || colon > 24) continue;
            std::string name = Trim(l.substr(0, colon)), said = Trim(l.substr(colon + 1));
            for (const char* q : {"\"", "\xE2\x80\x9C", "\xE2\x80\x9D", "\xE2\x80\x9E", "'"}) { // straight and curly quotes around the words
                const std::string qs = q;
                if (said.rfind(qs, 0) == 0) said = said.substr(qs.size());
                if (said.size() >= qs.size() && said.compare(said.size() - qs.size(), qs.size(), qs) == 0) said.resize(said.size() - qs.size());
            }
            said = Trim(said);
            if (said.empty() || said.front() == '(') continue;
            for (size_t w = 0; w < said.size(); ++w) words += (said[w] != ' ' && (w == 0 || said[w - 1] == ' ')) ? 1 : 0;
            kept += (kept.empty() ? "" : "\n") + name + ": " + said;
        }
        sc.lines = kept;
        // time to say it: about 2.5 words a second, and a breath at each end
        sc.seconds = std::clamp(std::max(sc.seconds, std::ceil(words / 2.5 + 1.5)), 4.0, 10.0);
        // "Same penthouse kitchen" is nothing to a picture model, which sees one scene at a time: the place's full words from before
        const std::string low = Lower(sc.place);
        if (low.rfind("same", 0) == 0 || low.rfind("the same", 0) == 0) {
            std::string best;
            int bestHits = 0;
            for (size_t k = 0; k < i; ++k) {
                const std::string other = Lower(out[k].place);
                if (other.rfind("same", 0) == 0 || other.rfind("the same", 0) == 0) continue;
                int hits = 0;
                for (size_t w = 0; w < low.size();) { // words of 4+ letters both share ("kitchen", "penthouse")
                    size_t e = low.find_first_of(" ,.;", w);
                    if (e == std::string::npos) e = low.size();
                    const std::string word = low.substr(w, e - w);
                    if (word.size() >= 4 && word != "same" && other.find(word) != std::string::npos) ++hits;
                    w = e + 1;
                }
                if (hits > bestHits) bestHits = hits, best = out[k].place;
            }
            if (!best.empty()) sc.place = best;
            else if (i > 0) sc.place = out[i - 1].place;
        }
        // carrying on only where it can: not the first scene, and not into another place
        if (sc.cont && (i == 0 || Lower(Trim(sc.place)) != Lower(Trim(out[i - 1].place)))) sc.cont = false;
    }
    for (size_t i = 2; i < out.size(); ++i) { // runs of at most 3 scenes (the writer was told so)
        if (out[i].cont && out[i - 1].cont && out[i - 2].cont) out[i].cont = false;
    }
    for (std::string* t : {&title}) {
        while (!t->empty() && (t->front() == '"' || t->front() == '*')) t->erase(t->begin());
        while (!t->empty() && (t->back() == '"' || t->back() == '*')) t->pop_back();
    }
    return out;
}

ScenesResult WriteScenes(std::string cast, std::string idea, double length, std::string url) {
    ScenesResult r;
    if (!WriterConfigured()) {
        r.error = "The prompt writer is not set up (Clef mode > Prompt writer).";
        return r;
    }
    const int n = std::clamp(static_cast<int>(std::lround(length / 7.0)), 3, 24);
    std::string system = kStorySystem;
    system.replace(system.find("{N}"), 3, std::to_string(n));
    const std::string user = "The characters:\n" + cast + "\nThe story: " + (idea.empty() ? std::string("make one up that suits these characters") : idea) +
                             "\nLength: about " + std::to_string(static_cast<int>(length)) + " seconds, so " + std::to_string(n) + " scenes.";
    const ClefSession session = WriterAcquire(url, SetStatus);
    if (!session.ok) {
        WriterRelease(session, false);
        r.error = "The prompt writer could not be started.";
        return r;
    }
    SetStatus("Writing the scenes...");
    std::string out, err;
    const bool ok = WriterChat(system, user, {}, 6000, 0.9, out, &err);
    WriterRelease(session, false);
    SetStatus("");
    if (!ok) {
        r.error = "The prompt writer did not answer" + (err.empty() ? std::string() : ": " + err);
        return r;
    }
    r.scenes = ParseScenes(out, r.title);
    if (r.scenes.empty()) r.error = "The writer's answer had no scenes in it. Try again.";
    return r;
}

// ---- 2. a scene's first frame: Qwen Image Edit with the cast's pictures as references
std::string PicturePrompt(const Scene& sc, const std::vector<int>& who) {
    // ONE photo: with several people and a "close-up", Qwen makes a collage (a panel each) - tested: 4 of 15 scenes came out split
    std::string p = std::string(g_s.shape == 0 ? "One single vertical 9:16 photograph" : "One single wide 16:9 photograph") +
                    ", one frame from a realistic short drama film (not a collage, no split panels, no frames within the frame). ";
    if (!sc.place.empty()) p += "The place: " + sc.place + ". ";
    for (size_t k = 0; k < who.size(); ++k) p += "The person from <image" + std::to_string(k + 1) + "> is " + CastLine(who[k]) + ". ";
    std::string shot = sc.shot;
    if (who.size() >= 2 && Lower(shot).find("close") != std::string::npos) { // a close-up of one with others around: a medium shot of them together
        shot = "Medium shot of";
        for (size_t k = 0; k < who.size(); ++k) shot += std::string(k == 0 ? " " : k + 1 == who.size() ? " and " : ", ") + g_s.cast[who[k]].name;
        shot += " together in the same frame";
    }
    if (!shot.empty()) p += shot + ": ";
    p += sc.action;
    if (!p.empty() && p.back() != '.') p += ".";
    if (!who.empty()) {
        std::string names;
        for (size_t k = 0; k < who.size(); ++k) names += std::string(k == 0 ? "" : k + 1 == who.size() ? " and " : ", ") + g_s.cast[who[k]].name;
        p += " Exactly " + std::to_string(who.size()) + (who.size() == 1 ? " person is" : " people are") + " in the photo (" + names + "), each one only once, "
             "at their real size. Each keeps exactly the face, hairstyle, body and outfit from their own picture. The white studio backdrop of the pictures "
             "is not part of the scene.";
    } else {
        p += " No people in the frame.";
    }
    p += " Natural cinematic light, realistic photo, sharp focus, no text, no captions, no logos.";
    return p;
}

void SendPicture(AppState& app, Scene& sc) {
    const std::vector<int> who = WhoIn(sc);
    ComfyJob job;
    job.label = "Story picture: " + g_s.title + ", scene " + std::to_string(SceneNumber(sc)) + " " + Tag(sc);
    job.prompt = PicturePrompt(sc, who);
    job.savePrefix = "Story/" + SafeName(g_s.title) + "_scene";
    job.times = 1;
    if (!who.empty()) { // the cast's pictures: a new canvas of the video's shape
        for (int k : who) job.images.push_back(g_s.cast[k].image);
        job.sheetW = g_s.shape == 0 ? 768 : 1344;
        job.sheetH = g_s.shape == 0 ? 1344 : 768;
    } else { // a place alone: drawn from the words
        job.draw = 3;
        job.pixW = g_s.shape == 0 ? 9 : 16;
        job.pixH = g_s.shape == 0 ? 16 : 9;
    }
    sc.error.clear();
    sc.pictureAct = SendSheetEdit(app, std::move(job));
    if (sc.pictureAct == 0) sc.error = "could not send the picture";
}

// ---- scenes that carry on from the one before: with H3 Continuum a run of them is ONE video, the first scene's picture its first frame and each
// scene a part with its own words. Without it each is made on its own, as before.
// Tested 2026-10-16 (a 4-scene rooftop story, Balanced + Veda, 640 x 1152; scenes 1-3 marked by the writer): the group took 531 s for 21 s
// against 603 s for the three clips plus 56 s for their two extra pictures; the faces, clothes and light stayed the same through it, where the
// separate scenes changed her hair and the light. One join still cut: scene 2 slid cash "across the wooden table", there was no table in the
// shot, and H3 cut to one 7 frames after the join - with 39 continuity frames and with "keep the framing" wording too. The join whose action
// fitted the shot was seamless. So the writer only marks scenes whose action fits the view.
bool Grouped(size_t k) { return k > 0 && k < g_s.scenes.size() && g_s.scenes[k].cont && H3LongReady(); }
size_t LeadOf(size_t k) {
    while (Grouped(k)) --k;
    return k;
}
size_t GroupEnd(size_t lead) { // one past its last scene
    size_t e = lead + 1;
    while (e < g_s.scenes.size() && Grouped(e)) ++e;
    return e;
}
size_t IndexOf(const Scene& sc) {
    for (size_t k = 0; k < g_s.scenes.size(); ++k) {
        if (g_s.scenes[k].uid == sc.uid) return k;
    }
    return 0;
}

// ---- 3. the video prompts (the writer looks at each frame), then the videos
struct PromptTask {
    int uid = 0;
    std::wstring picture;
    std::string idea, context;
    double seconds = 7;
};
struct PromptDone {
    int uid = 0;
    std::string text, note;
};
std::future<void> g_promptsFut;
std::vector<int> g_sendAfterPrompt; // scenes whose "Make the video" waits for their prompt
std::mutex g_promptsMutex;
std::vector<PromptDone> g_promptsDone;
std::atomic<bool> g_promptsStop{false};

std::string SpokenLines(const Scene& sc) {
    std::string out;
    size_t a = 0;
    while (a < sc.lines.size()) {
        size_t b = sc.lines.find('\n', a);
        if (b == std::string::npos) b = sc.lines.size();
        const std::string l = Trim(sc.lines.substr(a, b - a));
        a = b + 1;
        const size_t colon = l.find(':');
        if (l.empty() || colon == std::string::npos) continue;
        std::string words = Trim(l.substr(colon + 1));
        while (!words.empty() && (words.front() == '"' || words.back() == '"')) words.erase(words.front() == '"' ? words.begin() : words.end() - 1);
        out += (out.empty() ? "" : " Then ") + Trim(l.substr(0, colon)) + " says: \"" + words + "\".";
    }
    return out;
}

PromptTask TaskFor(const Scene& sc) {
    PromptTask t;
    t.uid = sc.uid;
    t.picture = sc.picture;
    t.seconds = sc.seconds;
    const std::string spoken = SpokenLines(sc);
    t.idea = sc.action + (spoken.empty() ? " Nobody says a word in this scene: no dialogue, no voices." : " Spoken, in this order, exactly these words and nothing else: " + spoken);
    // tested: left to itself the writer gives every scene its own piano or strings, and the joined story changes music every few seconds.
    // One soundtrack goes under the whole story at the join instead.
    t.idea += " Sound: only the voices and the natural sounds of the place - NO music, no score, no singing.";
    t.context = "A scene of a short drama. Place: " + sc.place + ". Shot: " + sc.shot + ". Who is who:";
    for (int k : WhoIn(sc)) t.context += " " + CastLine(k) + (Looks(k).empty() ? std::string() : ": " + Looks(k)) + ".";
    // the same voice in every scene: said in the idea, so the Audio line names it
    std::string voices;
    for (int k : WhoIn(sc)) {
        if (sc.lines.find(g_s.cast[k].name) != std::string::npos) voices += (voices.empty() ? "" : "; ") + g_s.cast[k].name + " speaks in " + VoiceOf(k);
    }
    if (!voices.empty()) t.idea += " Voices (name them in the Audio line, exactly): " + voices + ".";
    return t;
}

void WritePrompts(std::vector<PromptTask> tasks, H3Options o, std::string url) {
    for (size_t k = 0; k < tasks.size() && !g_promptsStop; ++k) {
        SetStatus("Writing the video prompt for scene " + std::to_string(k + 1) + " of " + std::to_string(tasks.size()) + "...");
        o.seconds = tasks[k].seconds;
        PromptDone d;
        d.uid = tasks[k].uid;
        d.text = WriteVideoPromptFor(tasks[k].picture, tasks[k].idea, tasks[k].context, o, url, &d.note);
        std::lock_guard<std::mutex> lock(g_promptsMutex);
        g_promptsDone.push_back(std::move(d));
    }
    SetStatus("");
}

void StartPrompts(AppState& app, bool all) {
    if (g_promptsFut.valid()) return;
    std::vector<PromptTask> tasks;
    for (size_t k = 0; k < g_s.scenes.size(); ++k) {
        const Scene& sc = g_s.scenes[k];
        if (!Grouped(k) && !sc.picture.empty() && (all || sc.videoPrompt.empty())) tasks.push_back(TaskFor(sc));
    }
    if (tasks.empty()) return;
    H3Options o = VideoOptions();
    o.quality = g_s.quality;
    o.res = g_s.res;
    o.shape = 0;
    o.last.clear();
    g_promptsStop = false;
    g_promptsFut = std::async(std::launch::async, WritePrompts, std::move(tasks), o, app.comfy.url);
}

// A continued scene's part of the prompt: its own action and words, written as the continuation H3 Continuum expects (no new picture: the camera
// carries on). Built from the scene, so it needs no writer and no picture.
std::string ContinuationSection(const Scene& sc) {
    std::string s = "Seamless continuation of the immediately preceding clip: " + sc.action;
    if (!s.empty() && s.back() != '.') s += ".";
    const std::string spoken = SpokenLines(sc);
    s += spoken.empty() ? " Nobody speaks in this part." : " " + spoken;
    s += " Same people, clothes, place and light. One continuous shot: no cut or reset.";
    std::string voices;
    for (int k : WhoIn(sc)) {
        if (sc.lines.find(g_s.cast[k].name) != std::string::npos) voices += (voices.empty() ? "" : "; ") + g_s.cast[k].name + " in " + VoiceOf(k);
    }
    s += " Audio: " + (voices.empty() ? std::string() : voices + "; ") + "the natural sounds of the place, NO music.";
    return s;
}

H3Options StoryOptions(const Scene& sc) {
    H3Options o = VideoOptions();
    o.quality = g_s.quality;
    o.res = g_s.res;
    o.shape = 0; // like the picture: it was made in the video's shape
    o.seconds = sc.seconds;
    o.times = 1;
    o.last.clear();
    return o;
}

// A group's whole prompt: the lead's written prompt, then each continued scene's part ("" = the scene leads no group)
std::string GroupPrompt(const Scene& lead, double* partSeconds = nullptr) {
    const size_t at = IndexOf(lead), end = GroupEnd(at);
    if (end - at < 2) return {};
    std::string prompt = lead.videoPrompt;
    double part = lead.seconds;
    for (size_t k = at + 1; k < end; ++k) {
        prompt += "\n---\n" + ContinuationSection(g_s.scenes[k]);
        part = std::max(part, g_s.scenes[k].seconds);
    }
    if (partSeconds) *partSeconds = part;
    return prompt;
}

void SendVideo(AppState& app, Scene& sc) {
    if (sc.picture.empty() || sc.videoPrompt.empty()) return;
    sc.error.clear();
    const size_t lead = IndexOf(sc), end = GroupEnd(lead);
    if (end - lead >= 2) { // it and the scenes carrying on from it: one video in parts (H3 Continuum)
        H3Options o = StoryOptions(sc);
        double part = sc.seconds;
        const std::string prompt = GroupPrompt(sc, &part);
        o.parts = static_cast<int>(end - lead);
        o.partSeconds = std::clamp(part, 4.0, 10.0); // (every part is as long: the longest scene's time, so its words fit)
        o.seconds = o.parts * o.partSeconds;
        const std::string label = "Story: " + g_s.title + ", scenes " + std::to_string(lead + 1) + "-" + std::to_string(end) + " " + Tag(sc);
        sc.clipPending = SendH3(app, sc.picture, prompt, o, label);
        for (size_t k = lead + 1; k < end; ++k) {
            g_s.scenes[k].clip.clear();
            g_s.scenes[k].error.clear();
            g_s.scenes[k].clipPending = sc.clipPending;
        }
        if (!sc.clipPending) sc.error = "could not send the video";
        return;
    }
    const std::string label = "Story: " + g_s.title + ", scene " + std::to_string(SceneNumber(sc)) + " " + Tag(sc);
    sc.clipPending = SendH3(app, sc.picture, sc.videoPrompt, StoryOptions(sc), label);
    if (!sc.clipPending) sc.error = "could not send the video";
}

// ---- 4. the clips joined into one video
std::future<std::pair<bool, std::string>> g_joinFut;

std::string AssTime(double t) {
    t = std::max(0.0, t);
    const int cs = static_cast<int>(std::lround(t * 100.0));
    char buf[32];
    snprintf(buf, sizeof(buf), "%d:%02d:%02d.%02d", cs / 360000, (cs / 6000) % 60, (cs / 100) % 60, cs % 100);
    return buf;
}

std::string AssText(std::string s) {
    for (char& c : s) {
        if (c == '{' || c == '}') c = ' ';
        if (c == '\n' || c == '\r') c = ' ';
    }
    return s;
}

struct JoinPart {
    std::wstring clip;
    std::vector<std::string> lines; // what is said in it, for the captions
    std::vector<std::vector<std::string>> sections; // a video of scenes that carry on: each scene's lines, in its own sectionSeconds
    double sectionSeconds = 0;
};

struct JoinOptions {
    bool captions = true, fullHd = true;
    std::wstring music; // one soundtrack under the whole story ("" = none)
    double musicVolume = 0.25;
};

// Where people talk in a clip: from the first to the last sound louder than the room (ffmpeg's silencedetect). {0, 0} = not found.
std::pair<double, double> SpeechSpan(const std::wstring& ff, const std::wstring& clip, double d) {
    std::string out;
    RunCaptureHidden(L"\"" + ff + L"\" -hide_banner -nostats -i \"" + clip + L"\" -vn -af silencedetect=n=-30dB:d=0.25 -f null -", out, true);
    std::vector<std::pair<double, double>> silent;
    double start = -1;
    for (size_t at = out.find("silence_"); at != std::string::npos; at = out.find("silence_", at + 8)) {
        if (out.compare(at, 14, "silence_start:") == 0) start = std::atof(out.c_str() + at + 14);
        else if (out.compare(at, 12, "silence_end:") == 0) silent.emplace_back(start < 0 ? 0.0 : start, std::atof(out.c_str() + at + 12)), start = -1;
    }
    if (start >= 0) silent.emplace_back(start, d);
    double a = 0, b = d;
    if (!silent.empty() && silent.front().first <= 0.05) a = silent.front().second;
    if (!silent.empty() && silent.back().second >= d - 0.05) b = silent.back().first;
    return b - a >= 0.6 ? std::make_pair(a, b) : std::make_pair(0.0, 0.0);
}

int Words(const std::string& s) {
    int n = 0;
    for (size_t i = 0; i < s.size(); ++i) n += (s[i] != ' ' && (i == 0 || s[i - 1] == ' ')) ? 1 : 0;
    return std::max(1, n);
}

std::pair<bool, std::string> JoinClips(std::vector<JoinPart> parts, JoinOptions opt, std::wstring out) {
    const std::wstring ff = FfmpegPath();
    if (ff.empty()) return {false, "ffmpeg.exe was not found (it joins the clips): install it and put it on the PATH"};
    SetStatus("Joining " + std::to_string(parts.size()) + " clips...");
    const bool captions = opt.captions;
    const VideoInfo first = ProbeVideo(parts.front().clip);
    int W = std::max(64, first.width / 2 * 2), H = std::max(64, first.height / 2 * 2);
    if (opt.fullHd) { // the platforms' own size: their re-encoding treats it better than a small upload
        const bool tall = H >= W;
        W = tall ? 1080 : 1920;
        H = tall ? 1920 : 1080;
    }
    std::string graph, ass;
    double t = 0;
    for (size_t i = 0; i < parts.size(); ++i) {
        const std::string n = std::to_string(i);
        // each clip fills the frame (a sliver is cut off when its shape differs a little: no black bars)
        graph += "[" + n + ":v]scale=" + std::to_string(W) + ":" + std::to_string(H) + ":force_original_aspect_ratio=increase:flags=lanczos,crop=" + std::to_string(W) +
                 ":" + std::to_string(H) + ",setsar=1,fps=24,format=yuv420p[v" + n + "];";
        // the sound: a few hundredths of a second faded at each cut, or every cut clicks
        const double d = std::max(1.0, ProbeVideo(parts[i].clip).seconds);
        char fades[96];
        snprintf(fades, sizeof(fades), ",afade=t=in:d=0.05,afade=t=out:st=%.3f:d=0.08", std::max(0.0, d - 0.08));
        graph += "[" + n + ":a]aresample=48000,aformat=sample_fmts=fltp:channel_layouts=stereo" + std::string(fades) + "[a" + n + "];";
        // the captions: over the part of the clip where someone talks, each line for a time that suits its words
        const size_t m = parts[i].lines.size();
        if (captions && !parts[i].sections.empty()) { // each scene's lines over its own part (speech is not looked for: parts run on into each other)
            const double ss = parts[i].sectionSeconds > 0 ? parts[i].sectionSeconds : d / parts[i].sections.size();
            for (size_t s = 0; s < parts[i].sections.size(); ++s) {
                const std::vector<std::string>& ls = parts[i].sections[s];
                if (ls.empty()) continue;
                const double s0 = s * ss + 0.3, s1 = std::min(d, (s + 1) * ss) - 0.2;
                if (s1 <= s0) continue;
                int total = 0;
                for (const std::string& l : ls) total += Words(l);
                double at = s0;
                for (const std::string& l : ls) {
                    const double len = (s1 - s0) * Words(l) / total;
                    ass += "Dialogue: 0," + AssTime(t + at) + "," + AssTime(t + at + len) + ",Default,,0,0,0,," + AssText(l) + "\n";
                    at += len;
                }
            }
        } else if (captions && m > 0) {
            auto [s0, s1] = SpeechSpan(ff, parts[i].clip, d);
            if (s1 <= s0) s0 = 0.3, s1 = d - 0.2;
            s0 = std::max(0.0, s0 - 0.1), s1 = std::min(d, s1 + 0.3);
            int total = 0;
            for (const std::string& l : parts[i].lines) total += Words(l);
            double at = s0;
            for (size_t k = 0; k < m; ++k) {
                const double len = (s1 - s0) * Words(parts[i].lines[k]) / total;
                ass += "Dialogue: 0," + AssTime(t + at) + "," + AssTime(t + at + len) + ",Default,,0,0,0,," + AssText(parts[i].lines[k]) + "\n";
                at += len;
            }
        }
        t += d;
    }
    for (size_t i = 0; i < parts.size(); ++i) graph += "[v" + std::to_string(i) + "][a" + std::to_string(i) + "]";
    graph += "concat=n=" + std::to_string(parts.size()) + ":v=1:a=1[vc][ac]";
    // one soundtrack under it all: looped to the length, faded in and out, and quieter while people talk
    std::string aout = "[ac]";
    std::error_code mec;
    if (!opt.music.empty() && fs::exists(opt.music, mec)) {
        const std::string m = std::to_string(parts.size());
        char mix[512];
        snprintf(mix, sizeof(mix),
                 ";[ac]asplit=2[ac1][ac2];[%s:a]aresample=48000,aformat=sample_fmts=fltp:channel_layouts=stereo,volume=%.2f,atrim=0:%.3f,"
                 "afade=t=in:d=1.5,afade=t=out:st=%.3f:d=2.5[mus];[mus][ac1]sidechaincompress=threshold=0.03:ratio=6:attack=30:release=400[mud];"
                 "[ac2][mud]amix=inputs=2:duration=first:normalize=0[af]",
                 m.c_str(), opt.musicVolume, t, std::max(0.0, t - 2.5));
        graph += mix;
        aout = "[af]";
    }
    const std::wstring dir = DataDir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::string vout = "[vc]";
    if (captions && !ass.empty()) {
        const int fs_ = std::max(18, H / 22), outline = std::max(2, H / 300), margin = H / 5;
        const std::string head = "[Script Info]\nScriptType: v4.00+\nPlayResX: " + std::to_string(W) + "\nPlayResY: " + std::to_string(H) +
                                 "\nWrapStyle: 0\n\n[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, "
                                 "Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
                                 "Style: Default,Arial,"
                                 + std::to_string(fs_) + ",&H00FFFFFF,&H000000FF,&H00000000,&H80000000,-1,0,0,0,100,100,0,0,1," + std::to_string(outline) + ",1,2," +
                                 std::to_string(W / 12) + "," + std::to_string(W / 12) + "," + std::to_string(margin) +
                                 ",1\n\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";
        std::ofstream a(dir + L"\\story_captions.ass", std::ios::binary | std::ios::trunc);
        a << head << ass;
        a.close();
        std::string p = Utf8(dir + L"\\story_captions.ass"); // a path inside a filter: C\:/Users/...
        std::replace(p.begin(), p.end(), '\\', '/');
        std::string esc;
        for (char c : p) esc += c == ':' ? std::string("\\:") : c == '\'' ? std::string("\\'") : std::string(1, c);
        graph += ";[vc]subtitles='" + esc + "'[vs]";
        vout = "[vs]";
    }
    {
        std::ofstream g(dir + L"\\story_join.txt", std::ios::binary | std::ios::trunc);
        g << graph;
    }
    std::wstring cmd = L"\"" + ff + L"\" -y -hide_banner -loglevel error";
    for (const JoinPart& p : parts) cmd += L" -i \"" + p.clip + L"\"";
    if (aout == "[af]") cmd += L" -stream_loop -1 -i \"" + opt.music + L"\"";
    cmd += L" -/filter_complex \"" + dir + L"\\story_join.txt\" -map \"" + Wide(vout) + L"\" -map \"" + Wide(aout) + L"\" -c:v libx264 -preset medium -crf 20 -c:a aac "
           L"-b:a 160k -movflags +faststart \"" + out + L"\"";
    fs::create_directories(fs::path(out).parent_path(), ec);
    fs::remove(out, ec);
    std::string log;
    RunCaptureHidden(cmd, log, true);
    SetStatus("");
    if (!fs::exists(out, ec) || fs::file_size(out, ec) < 1000) {
        if (log.size() > 400) log = log.substr(log.size() - 400);
        return {false, "ffmpeg could not join them: " + (log.empty() ? std::string("no output") : log)};
    }
    return {true, Utf8(out)};
}

void StartJoin(AppState& app) {
    if (g_joinFut.valid()) return;
    auto linesOf = [](const Scene& sc) {
        std::vector<std::string> out;
        size_t a = 0;
        while (a < sc.lines.size()) {
            size_t b = sc.lines.find('\n', a);
            if (b == std::string::npos) b = sc.lines.size();
            const std::string l = Trim(sc.lines.substr(a, b - a));
            a = b + 1;
            const size_t colon = l.find(':');
            std::string words = Trim(colon == std::string::npos ? l : l.substr(colon + 1));
            while (!words.empty() && words.front() == '"') words.erase(words.begin());
            while (!words.empty() && words.back() == '"') words.pop_back();
            if (!words.empty()) out.push_back(words);
        }
        return out;
    };
    std::vector<JoinPart> parts;
    for (size_t k = 0; k < g_s.scenes.size(); ++k) {
        const Scene& sc = g_s.scenes[k];
        if (sc.clip.empty() || Grouped(k)) continue; // (a continued scene is in its lead's video)
        JoinPart p;
        p.clip = sc.clip;
        p.lines = linesOf(sc);
        const size_t end = GroupEnd(k);
        if (end - k >= 2) {
            double part = 0;
            for (size_t j = k; j < end; ++j) p.sections.push_back(linesOf(g_s.scenes[j])), part = std::max(part, g_s.scenes[j].seconds);
            p.sectionSeconds = std::clamp(part, 4.0, 10.0);
        }
        parts.push_back(std::move(p));
    }
    if (parts.empty()) return;
    std::wstring dir = ComfyOutputDir(app.comfy);
    dir = dir.empty() ? DataDir() + L"\\stories" : dir + L"\\video";
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::tm tm = {};
    localtime_s(&tm, &now);
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm);
    const std::wstring out = dir + L"\\Story_" + Wide(SafeName(g_s.title)) + L"_" + Wide(stamp) + L".mp4";
    JoinOptions opt;
    opt.captions = g_s.captions;
    opt.fullHd = g_s.fullHd;
    opt.music = g_s.music;
    opt.musicVolume = g_s.musicVolume;
    g_joinFut = std::async(std::launch::async, JoinClips, std::move(parts), opt, out);
}

// ---- after a restart: videos ComfyUI finished while the app was closed are found again by the prompt saved inside them
std::future<std::vector<std::pair<int, std::wstring>>> g_relinkFut;

std::vector<std::pair<int, std::wstring>> Relink(std::vector<std::pair<int, std::string>> want, std::vector<std::wstring> dirs) {
    std::vector<std::pair<int, std::wstring>> found;
    std::vector<fs::file_time_type> when;
    const auto oldest = fs::file_time_type::clock::now() - std::chrono::hours(24 * 7);
    for (const std::wstring& d : dirs) {
        std::error_code ec;
        if (d.empty() || !fs::is_directory(d, ec)) continue;
        for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec) || _wcsicmp(it->path().extension().c_str(), L".mp4") != 0) continue;
            const auto t = it->last_write_time(ec);
            if (ec || t < oldest) continue;
            for (const RawField& f : ReadVideoWorkflowFields(it->path().wstring())) {
                if (f.key != "prompt") continue;
                const json wf = json::parse(f.value, nullptr, false);
                if (!wf.is_object()) break;
                for (const auto& [id, n] : wf.items()) {
                    if (!n.is_object() || !n.contains("inputs")) continue;
                    const std::string cls = n.value("class_type", std::string());
                    const bool parts = cls == "PrimitiveStringMultiline"; // H3 Continuum: the scene's prompt, then its continued scenes' parts
                    if (cls != "MiniMaxH3ImageToVideo" && !parts) continue;
                    const std::string text = n["inputs"].value(parts ? "value" : "prompt", std::string());
                    for (const auto& [uid, prompt] : want) {
                        if (text != prompt) continue; // (a group's text node holds its whole prompt: matched whole, so a scene that no longer leads it does not get it)
                        auto at = std::find_if(found.begin(), found.end(), [&](const auto& x) { return x.first == uid; });
                        if (at == found.end()) found.emplace_back(uid, it->path().wstring()), when.push_back(t);
                        else if (t > when[at - found.begin()]) at->second = it->path().wstring(), when[at - found.begin()] = t; // the newest
                    }
                }
                break;
            }
        }
    }
    return found;
}

int CountPictures() { // (a continued scene needs none: it counts as having one)
    int n = 0;
    for (size_t k = 0; k < g_s.scenes.size(); ++k) n += (g_s.scenes[k].picture.empty() && !Grouped(k)) ? 0 : 1;
    return n;
}
int CountClips() {
    int n = 0;
    for (const Scene& sc : g_s.scenes) n += sc.clip.empty() ? 0 : 1;
    return n;
}
bool PicturesBusy() {
    for (const Scene& sc : g_s.scenes) {
        if (sc.pictureAct) return true;
    }
    return false;
}
bool VideosBusy() {
    for (const Scene& sc : g_s.scenes) {
        if (sc.clipPending) return true;
    }
    return false;
}

// ---- the page
int StrResize(ImGuiInputTextCallbackData* d) {
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* s = static_cast<std::string*>(d->UserData);
        s->resize(d->BufTextLen);
        d->Buf = s->data();
    }
    return 0;
}

bool InputLine(const char* id, const char* hint, std::string& v, float w) {
    ImGui::SetNextItemWidth(w);
    return ImGui::InputTextWithHint(id, hint, v.data(), v.capacity() + 1, ImGuiInputTextFlags_CallbackResize, StrResize, &v);
}

bool Chip(const char* label, bool on) {
    ImGui::PushStyleColor(ImGuiCol_Button, on ? ui::ChipActiveColor() : ui::ChipColor());
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::ChipHoverColor());
    const bool clicked = ImGui::Button(label);
    ImGui::PopStyleColor(2);
    return clicked;
}

void Thumb(AppState& app, const std::wstring& path, ImVec2 size, const char* empty) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), ui::ThumbPlaceholder(), ui::Dp(5.0f));
    const AppState::LibThumb lt = path.empty() ? AppState::LibThumb{} : app.LibraryThumb(path);
    if (lt.srv && lt.w > 0 && lt.h > 0) {
        const float s = std::min(size.x / lt.w, size.y / lt.h);
        const ImVec2 a(p.x + (size.x - lt.w * s) * 0.5f, p.y + (size.y - lt.h * s) * 0.5f);
        dl->AddImage((ImTextureID)(intptr_t)lt.srv, a, ImVec2(a.x + lt.w * s, a.y + lt.h * s));
    } else if (empty) {
        const ImVec2 ts = ImGui::CalcTextSize(empty);
        dl->AddText(ImVec2(p.x + (size.x - ts.x) * 0.5f, p.y + (size.y - ts.y) * 0.5f), ImGui::GetColorU32(ui::kTextDim), empty);
    }
}

struct Premise {
    const char* name;
    const char* idea;
};
const Premise kPremises[] = {
    {"Rich vs poor", "Two men want the same woman: one is rich and shows off, one is poor but kind. She has to choose, and the ending is a twist."},
    {"Secret millionaire", "Everyone treats the poor-looking one badly, until it turns out they are secretly a millionaire."},
    {"Glow-up revenge", "Someone is dumped and humiliated, comes back transformed and successful, and the one who left regrets it."},
    {"Wrong number", "A wrong-number text starts a romance between two strangers, with a misunderstanding before the happy ending."},
    {"Betrayal", "A best friend is caught betraying the main character, and the main character gets clever revenge."},
};

void DrawCast(AppState& app) {
    ui::SectionHeading("THE CAST");
    const float tw = ui::Dp(62.0f), th = ui::Dp(110.0f), cardW = ui::Dp(250.0f); // wide enough to read the role and the voice
    int remove = -1;
    for (size_t k = 0; k < g_s.cast.size(); ++k) {
        CastMember& c = g_s.cast[k];
        ImGui::PushID(static_cast<int>(k));
        if (k && ImGui::GetContentRegionAvail().x > cardW + ui::Dp(8.0f)) ImGui::SameLine(0.0f, ui::Dp(10.0f));
        ImGui::BeginGroup();
        Thumb(app, c.image, ImVec2(tw, th), "?");
        ImGui::SameLine(0.0f, ui::Dp(4.0f));
        ImGui::BeginGroup();
        if (InputLine("##name", "name", c.name, cardW - tw)) Changed();
        if (InputLine("##role", "who they are", c.role, cardW - tw)) Changed();
        if (InputLine("##voice", DefaultVoice(static_cast<int>(k)).c_str(), c.voice, cardW - tw)) Changed();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Their voice, said in every scene so it stays the same (empty: the one shown)");
        if (ImGui::SmallButton("remove")) remove = static_cast<int>(k);
        ImGui::EndGroup();
        ImGui::EndGroup();
        ImGui::PopID();
    }
    if (remove >= 0) {
        g_s.cast.erase(g_s.cast.begin() + remove);
        Changed();
    }
    // add one of the saved characters
    std::vector<const CharacterProfile*> saved;
    for (const CharacterProfile& p : AllProfiles()) {
        if (p.saved && !p.image.empty() && std::none_of(g_s.cast.begin(), g_s.cast.end(), [&](const CastMember& c) { return _wcsicmp(c.image.c_str(), p.image.c_str()) == 0; }))
            saved.push_back(&p);
    }
    if (g_s.cast.size() < 4) {
        if (!g_s.cast.empty()) ImGui::Spacing();
        if (saved.empty()) {
            ImGui::TextDisabled(g_s.cast.empty() ? "Your story needs characters: make them first." : "No more saved characters.");
        } else {
            ImGui::SetNextItemWidth(ui::Dp(220.0f));
            if (ImGui::BeginCombo("##addcast", "+ Add one of your characters", ImGuiComboFlags_HeightLarge)) {
                for (const CharacterProfile* p : saved) {
                    ImGui::PushID(p->key.c_str());
                    const AppState::LibThumb lt = app.LibraryThumb(p->image);
                    if (lt.srv && lt.w > 0 && lt.h > 0) {
                        ImGui::Image((ImTextureID)(intptr_t)lt.srv, ImVec2(ui::Dp(22.0f) * lt.w / lt.h, ui::Dp(22.0f)));
                        ImGui::SameLine();
                    }
                    if (ImGui::Selectable(p->name.empty() ? "(no name)" : p->name.c_str())) {
                        g_s.cast.push_back({p->image, p->name, std::string(), std::string()});
                        Changed();
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Create a character...##storycast")) OpenCharacterCreator(app);
    }
}

void DrawScene(AppState& app, size_t k, int& moveUp, int& moveDown, int& remove) {
    Scene& sc = g_s.scenes[k];
    ImGui::PushID(sc.uid);
    const float tw = g_s.shape == 0 ? ui::Dp(54.0f) : ui::Dp(96.0f), th = g_s.shape == 0 ? ui::Dp(96.0f) : ui::Dp(54.0f);
    const bool grouped = Grouped(k);
    if (grouped) { // no picture of its own: an arrow from the scene above
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(tw, th));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRect(p, ImVec2(p.x + tw, p.y + th), ImGui::GetColorU32(ImGuiCol_TextDisabled, 0.5f), ui::Dp(5.0f));
        const char* t = "carries\non";
        const ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2(p.x + (tw - ts.x) * 0.5f, p.y + (th - ts.y) * 0.5f), ImGui::GetColorU32(ui::kTextDim), t);
    } else {
        Thumb(app, sc.picture, ImVec2(tw, th), sc.pictureAct ? "..." : nullptr);
    }
    if (ImGui::IsItemHovered() && !sc.picture.empty() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) app.OpenFile(sc.picture);
    ImGui::SameLine();
    ImGui::BeginGroup();
    std::string who;
    for (int c : WhoIn(sc)) who += (who.empty() ? "" : ", ") + g_s.cast[c].name;
    ImGui::Text("Scene %d", static_cast<int>(k) + 1);
    ImGui::SameLine();
    ImGui::TextDisabled("%.0f s  %s", sc.seconds, who.empty() ? "(no one)" : who.c_str());
    // where it is
    ImGui::SameLine();
    if (!sc.clip.empty()) ImGui::TextColored(ui::kGood, "video ready");
    else if (sc.clipPending) ImGui::TextColored(ui::kAccentHover, "video being made");
    else if (!sc.videoPrompt.empty()) ImGui::TextColored(ui::kTextDim, "prompt written");
    else if (!sc.picture.empty()) ImGui::TextColored(ui::kTextDim, "picture ready");
    else if (sc.pictureAct) ImGui::TextColored(ui::kAccentHover, "picture being made");
    if (!sc.error.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ui::kBad, "%s", sc.error.c_str());
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(sc.action.c_str());
    if (!sc.lines.empty()) ImGui::TextColored(ImVec4(0.85f, 0.80f, 0.60f, 1.0f), "%s", sc.lines.c_str());
    ImGui::PopTextWrapPos();
    // carrying on from the scene above: one continuous shot with it (H3 Continuum)
    if (k > 0) {
        char lbl[64];
        snprintf(lbl, sizeof(lbl), "Carries on from scene %d (one shot)", static_cast<int>(k));
        if (ImGui::Checkbox(lbl, &sc.cont)) {
            Scene& lead = g_s.scenes[LeadOf(sc.cont ? k : k - 1)]; // its video changes: made again
            if (!lead.clipPending) lead.clip.clear();
            if (!sc.clipPending) sc.clip.clear();
            Changed();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("Same place, same people, the action goes straight on: instead of a cut to a new picture, this scene is made together with "
                              "the one above as one video (H3 Continuum), so the camera, the voices and the sound of the room carry on.\n"
                              "It gets no picture of its own. Untick it for a cut to its own shot.");
        if (sc.cont && !H3LongReady()) {
            ImGui::SameLine();
            ImGui::TextColored(ui::kWarn, "needs H3 Continuum (Animate a picture > Length: Set up): made as its own clip for now");
        }
    }
    if (grouped) { // its video is the lead's
        const size_t lead = LeadOf(k);
        if (!sc.clip.empty()) {
            if (ImGui::SmallButton("Play")) PlayInVideoPage(app, sc.clip);
            ImGui::SameLine();
        }
        ImGui::TextDisabled("made with scene %d, in one video", static_cast<int>(lead) + 1);
        ImGui::SameLine();
        if (ImGui::SmallButton("^")) moveUp = static_cast<int>(k);
        ImGui::SameLine();
        if (ImGui::SmallButton("v")) moveDown = static_cast<int>(k);
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) remove = static_cast<int>(k);
    }
    // what to do with it
    if (!grouped && !sc.clip.empty()) {
        if (ImGui::SmallButton("Play")) PlayInVideoPage(app, sc.clip);
        ImGui::SameLine();
    }
    if (!grouped) {
    ImGui::BeginDisabled(sc.pictureAct != 0);
    if (ImGui::SmallButton(sc.picture.empty() ? "Make the picture" : "New picture")) {
        sc.picture.clear();
        sc.videoPrompt.clear();
        SendPicture(app, sc);
        Changed();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton("Picture from a file...")) {
        wchar_t buf[MAX_PATH * 2] = {};
        OPENFILENAMEW ofn = {sizeof(ofn)};
        ofn.hwndOwner = GetActiveWindow();
        ofn.lpstrFilter = L"Pictures\0*.png;*.jpg;*.jpeg;*.webp\0All files\0*.*\0";
        ofn.lpstrFile = buf;
        ofn.nMaxFile = MAX_PATH * 2;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&ofn)) {
            sc.picture = buf;
            sc.videoPrompt.clear();
            Changed();
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(sc.picture.empty() || sc.clipPending || g_promptsFut.valid());
    if (ImGui::SmallButton(sc.clip.empty() ? "Make the video" : "New video")) {
        sc.clip.clear();
        if (sc.videoPrompt.empty()) { // its prompt first, then the video (sent by the pump when the prompt is in)
            g_promptsStop = false;
            std::vector<PromptTask> one{TaskFor(sc)};
            g_promptsFut = std::async(std::launch::async, WritePrompts, std::move(one), StoryOptions(sc), app.comfy.url);
            g_sendAfterPrompt.push_back(sc.uid);
            sc.error.clear();
        } else {
            SendVideo(app, sc);
        }
        Changed();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton("^")) moveUp = static_cast<int>(k);
    ImGui::SameLine();
    if (ImGui::SmallButton("v")) moveDown = static_cast<int>(k);
    ImGui::SameLine();
    if (ImGui::SmallButton("x")) remove = static_cast<int>(k);
    } // (!grouped)
    if (ImGui::TreeNode("Edit the scene")) {
        bool text = false;
        const float w = ImGui::GetContentRegionAvail().x;
        text |= InputLine("##place", "the place", sc.place, w);
        text |= InputLine("##who", "who is in it (names)", sc.who, w);
        text |= InputLine("##shot", "the shot", sc.shot, w);
        ImGui::TextDisabled("What happens");
        text |= WrapEdit("##action", sc.action, ui::Dp(44.0f));
        ImGui::TextDisabled("What is said (one \"Name: words\" a line)");
        text |= WrapEdit("##lines", sc.lines, ui::Dp(44.0f));
        float secs = static_cast<float>(sc.seconds);
        ImGui::SetNextItemWidth(ui::Dp(200.0f));
        if (ImGui::SliderFloat("##secs", &secs, 3.0f, 12.0f, "%.0f seconds")) sc.seconds = std::round(secs), text = true;
        if (!sc.videoPrompt.empty() && ImGui::TreeNode("The video prompt")) {
            if (WrapEdit("##vp", sc.videoPrompt, ui::Dp(120.0f))) Changed();
            ImGui::TreePop();
        }
        if (text) {
            sc.videoPrompt.clear(); // written again for the new words
            Changed();
        }
        ImGui::TreePop();
    }
    ImGui::EndGroup();
    ImGui::PopID();
    ImGui::Separator();
}

} // namespace

void SetStoryMusic(const std::wstring& song) {
    if (!g_loaded) Load();
    g_s.music = song;
    Changed();
}

// what music would suit the story: an instrumental bed under the talking, as long as the story
std::string StoryMusicIdea(double* seconds) {
    if (!g_loaded) Load();
    double total = 0;
    for (const Scene& sc : g_s.scenes) total += sc.seconds;
    if (seconds) *seconds = total > 0 ? total : g_s.length;
    return "An instrumental soundtrack for a short drama video called \"" + g_s.title + "\"" + (g_s.idea.empty() ? std::string() : ": " + g_s.idea) +
           " Emotional and cinematic, with a build-up to the twist and a resolving ending, soft enough to talk over.";
}

// tests: MV_STORY_CAST "Marcus=the rich man|Danny=..." (saved characters by name), MV_STORY_IDEA, MV_STORY_LENGTH;
// MV_UI_TEST "storywrite" writes the scenes once, "storymake" presses Make everything once the scenes are there
void StoryTestHooks(AppState& app) {
    static bool done = false, wrote = false, made = false;
    char buf[2048] = {};
    if (!done) {
        done = true;
        if (GetEnvironmentVariableA("MV_STORY_CAST", buf, sizeof(buf))) {
            g_s.cast.clear();
            std::string all = buf;
            size_t a = 0;
            while (a < all.size()) {
                const size_t b = std::min(all.find('|', a), all.size());
                const std::string one = all.substr(a, b - a);
                a = b + 1;
                const size_t eq = one.find('=');
                const std::string name = one.substr(0, eq), role = eq == std::string::npos ? std::string() : one.substr(eq + 1);
                for (const CharacterProfile& p : AllProfiles()) {
                    if (p.saved && p.name == name) {
                        g_s.cast.push_back({p.image, name, role, std::string()});
                        break;
                    }
                }
            }
            Changed();
        }
        if (GetEnvironmentVariableA("MV_STORY_IDEA", buf, sizeof(buf))) g_s.idea = buf, Changed();
        if (GetEnvironmentVariableA("MV_STORY_LENGTH", buf, sizeof(buf))) g_s.length = std::clamp(std::atof(buf), 20.0, 300.0), Changed();
        wchar_t wbuf[1024] = {};
        if (GetEnvironmentVariableW(L"MV_STORY_MUSIC", wbuf, 1024)) g_s.music = wbuf, Changed();
    }
    static const std::string tests = [] {
        char b[256] = {};
        return GetEnvironmentVariableA("MV_UI_TEST", b, sizeof(b)) ? std::string(b) : std::string();
    }();
    if (!wrote && tests.find("storywrite") != std::string::npos && !g_s.cast.empty()) {
        wrote = true;
        std::string cast;
        for (size_t k = 0; k < g_s.cast.size(); ++k) cast += "- " + CastLine(static_cast<int>(k)) + (Looks(static_cast<int>(k)).empty() ? "" : ": " + Looks(static_cast<int>(k))) + "\n";
        g_scenesFut = std::async(std::launch::async, WriteScenes, cast, g_s.idea, g_s.length, app.comfy.url);
    }
    static bool pics = false;
    if (!pics && tests.find("storypics") != std::string::npos && !g_s.scenes.empty() && !g_scenesFut.valid() && (wrote || tests.find("storywrite") == std::string::npos)) {
        pics = true;
        for (Scene& sc : g_s.scenes) {
            if (sc.picture.empty() && !sc.pictureAct) SendPicture(app, sc);
        }
        Changed();
    }
    if (!made && tests.find("storymake") != std::string::npos && !g_s.scenes.empty() && !g_scenesFut.valid()) {
        made = true;
        // MV_STORY_CONT "2,4" (those scenes carry on from the one before) or "none"; MV_STORY_FRESH: the videos are made again (pictures kept)
        if (GetEnvironmentVariableA("MV_STORY_CONT", buf, sizeof(buf))) {
            const std::string list = std::string(",") + buf + ",";
            for (size_t k = 0; k < g_s.scenes.size(); ++k) g_s.scenes[k].cont = list.find("," + std::to_string(k + 1) + ",") != std::string::npos;
        }
        if (GetEnvironmentVariableA("MV_STORY_FRESH", buf, sizeof(buf))) {
            for (Scene& sc : g_s.scenes) sc.clip.clear(), sc.error.clear();
            g_s.finalVideo.clear();
        }
        g_s.step = 1;
        Changed();
    }
}

void PumpStory(AppState& app) {
    if (!g_loaded) Load();
    StoryTestHooks(app);
    // once after starting: scenes with a written prompt but no video may have had it made while the app was closed
    static bool relinkAsked = false;
    if (!relinkAsked && app.comfyLoaded) {
        relinkAsked = true;
        std::vector<std::pair<int, std::string>> want;
        for (size_t k = 0; k < g_s.scenes.size(); ++k) {
            const Scene& sc = g_s.scenes[k];
            if (Grouped(k) || !sc.clip.empty() || sc.clipPending || sc.videoPrompt.empty()) continue;
            const std::string group = GroupPrompt(sc);
            want.emplace_back(sc.uid, group.empty() ? sc.videoPrompt : group);
        }
        if (!want.empty()) {
            const std::wstring out = ComfyOutputDir(app.comfy);
            g_relinkFut = std::async(std::launch::async, Relink, std::move(want),
                                     std::vector<std::wstring>{out.empty() ? std::wstring() : out + L"\\video", DataDir() + L"\\comfy_results"});
        }
    }
    if (g_relinkFut.valid() && g_relinkFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        int n = 0;
        for (const auto& [uid, path] : g_relinkFut.get()) {
            for (Scene& sc : g_s.scenes) {
                if (sc.uid == uid && sc.clip.empty() && !sc.clipPending) sc.clip = path, ++n;
            }
        }
        if (n > 0) {
            Toast(ToastKind::Info, "Found " + std::to_string(n) + " story video" + (n == 1 ? "" : "s"), "Made while the app was closed: they are in their scenes again.", {},
                  [](AppState& s) { s.requestTab = 10; }, "story");
            Changed();
        }
    }
    // the scenes, written
    if (g_scenesFut.valid() && g_scenesFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        ScenesResult r = g_scenesFut.get();
        if (!r.error.empty()) {
            Toast(ToastKind::Error, "Could not write the story", r.error, {}, [](AppState& s) { s.requestTab = 10; }, "story");
        } else {
            for (Scene& sc : r.scenes) sc.uid = g_s.nextUid++;
            g_s.scenes = std::move(r.scenes);
            if (!r.title.empty()) g_s.title = r.title;
            g_s.id = NewId();
            g_s.finalVideo.clear();
            double total = 0;
            for (const Scene& sc : g_s.scenes) total += sc.seconds;
            Toast(ToastKind::Success, "The story is written: " + g_s.title,
                  std::to_string(g_s.scenes.size()) + " scenes, " + std::to_string(static_cast<int>(total)) + " s. Check them, then Make everything.", {},
                  [](AppState& s) { s.requestTab = 10; }, "story");
        }
        Changed();
    }
    // video prompts, as they are written
    {
        std::vector<PromptDone> done;
        {
            std::lock_guard<std::mutex> lock(g_promptsMutex);
            done.swap(g_promptsDone);
        }
        for (PromptDone& d : done) {
            for (Scene& sc : g_s.scenes) {
                if (sc.uid != d.uid) continue;
                sc.videoPrompt = d.text;
                if (!d.note.empty()) sc.error = "simple prompt: " + d.note;
                Changed();
                // a single "Make the video": sent as soon as its prompt is there
                const auto wait = std::find(g_sendAfterPrompt.begin(), g_sendAfterPrompt.end(), sc.uid);
                if (wait != g_sendAfterPrompt.end()) {
                    g_sendAfterPrompt.erase(wait);
                    SendVideo(app, sc);
                }
            }
        }
        if (g_promptsFut.valid() && g_promptsFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) g_promptsFut.get();
    }
    // pictures and videos coming back
    if (app.comfySvc && (PicturesBusy() || VideosBusy())) {
        const std::vector<ComfyActivity> acts = app.comfySvc->Activity();
        for (Scene& sc : g_s.scenes) {
            if (sc.pictureAct) {
                for (const ComfyActivity& a : acts) {
                    if (a.id != sc.pictureAct) continue;
                    if (a.state == ComfyState::Done) {
                        for (const ComfyResult& r : a.files) {
                            if (!r.localPath.empty() && !r.video) {
                                sc.picture = r.localPath;
                                break;
                            }
                        }
                        if (sc.picture.empty()) sc.error = "no picture came back";
                        sc.pictureAct = 0;
                        Changed();
                    } else if (a.state == ComfyState::Failed) {
                        sc.error = "picture failed: " + a.message.substr(0, 160);
                        sc.pictureAct = 0;
                        Changed();
                    }
                }
            }
            if (sc.clipPending && !Grouped(IndexOf(sc))) { // (a continued scene's video is its lead's: set below)
                const std::string tag = Tag(sc);
                const ComfyActivity* found = nullptr;
                for (const ComfyActivity& a : acts) {
                    if (a.label.find(tag) != std::string::npos && (a.label.rfind("Video (", 0) == 0 || a.label.rfind("Long video (", 0) == 0)) found = &a; // the newest one
                }
                if (found && found->state == ComfyState::Done) {
                    for (const ComfyResult& r : found->files) {
                        if (!r.localPath.empty() && r.video) {
                            sc.clip = r.localPath;
                            break;
                        }
                    }
                    if (sc.clip.empty()) sc.error = "no video came back";
                    sc.clipPending = false;
                    Changed();
                } else if (found && found->state == ComfyState::Failed) {
                    sc.error = "video failed: " + found->message.substr(0, 160);
                    sc.clipPending = false;
                    Changed();
                }
            }
        }
        for (size_t k = 0; k < g_s.scenes.size(); ++k) { // continued scenes: done when their lead is
            if (!Grouped(k) || !g_s.scenes[k].clipPending) continue;
            const Scene& lead = g_s.scenes[LeadOf(k)];
            if (lead.clipPending) continue;
            g_s.scenes[k].clip = lead.clip;
            g_s.scenes[k].error = lead.error;
            g_s.scenes[k].clipPending = false;
            Changed();
        }
    }
    // the join
    if (g_joinFut.valid() && g_joinFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const auto [ok, msg] = g_joinFut.get();
        if (ok) {
            g_s.finalVideo = Wide(msg);
            Toast(ToastKind::Success, "The story video is ready: " + g_s.title, msg, {}, [](AppState& s) { s.requestTab = 10; }, "story");
            PlayInVideoPage(app, g_s.finalVideo, false); // ready in the player; you press Play
        } else {
            Toast(ToastKind::Error, "Could not join the clips", msg, {}, {}, "story");
        }
        if (g_s.step == 4) g_s.step = 0;
        Changed();
    }
    // "Make everything": pictures, then video prompts, then videos, then the join
    if (g_s.step == 1) {
        bool sent = false;
        for (size_t k = 0; k < g_s.scenes.size(); ++k) {
            Scene& sc = g_s.scenes[k];
            if (!Grouped(k) && sc.picture.empty() && !sc.pictureAct && sc.error.empty()) SendPicture(app, sc), sent = true;
        }
        if (sent) Changed();
        if (!PicturesBusy()) g_s.step = 2;
    }
    if (g_s.step == 2) {
        bool missing = false;
        for (const Scene& sc : g_s.scenes) missing |= !sc.picture.empty() && sc.videoPrompt.empty();
        if (missing && !g_promptsFut.valid()) StartPrompts(app, false);
        if (!missing && !g_promptsFut.valid()) g_s.step = 3;
    }
    if (g_s.step == 3) {
        bool sent = false;
        for (size_t k = 0; k < g_s.scenes.size(); ++k) {
            Scene& sc = g_s.scenes[k];
            if (!Grouped(k) && sc.clip.empty() && !sc.clipPending && !sc.picture.empty() && !sc.videoPrompt.empty() && sc.error.find("failed") == std::string::npos)
                SendVideo(app, sc), sent = true;
        }
        if (sent) Changed();
        if (!VideosBusy()) {
            g_s.step = CountClips() > 0 ? 4 : 0;
            if (g_s.step == 4) StartJoin(app);
        }
    }
    if (g_dirty && ImGui::GetTime() - g_dirtyAt > 1.0) Save();
}

void DrawStoryMode(AppState& app, float footerH) {
    (void)footerH;
    if (!g_loaded) Load();
    ImGui::PushStyleColor(ImGuiCol_Text, ui::kTextDim);
    ImGui::TextWrapped("A short drama from your characters, like the AI story videos on TikTok: the writer turns your idea into scenes, every scene gets a "
                       "picture of your cast and a video with sound and speech, and the clips are joined into one video.");
    ImGui::PopStyleColor();

    DrawCast(app);

    ui::SectionHeading("THE STORY");
    if (InputLine("##title", "title", g_s.title, ui::Dp(320.0f))) Changed();
    ImGui::TextDisabled("What happens (a sentence or two is enough):");
    if (WrapEdit("##storyidea", g_s.idea, ui::Dp(54.0f))) Changed();
    for (size_t k = 0; k < sizeof(kPremises) / sizeof(kPremises[0]); ++k) {
        if (k) ImGui::SameLine(0.0f, ui::Dp(4.0f));
        if (ImGui::GetCursorPosX() + ImGui::CalcTextSize(kPremises[k].name).x + ui::Dp(20.0f) > ImGui::GetContentRegionMax().x) ImGui::NewLine();
        if (Chip(kPremises[k].name, g_s.idea == kPremises[k].idea)) g_s.idea = kPremises[k].idea, Changed();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", kPremises[k].idea);
    }
    float len = static_cast<float>(g_s.length);
    ImGui::SetNextItemWidth(ui::Dp(260.0f));
    if (ImGui::SliderFloat("##storylen", &len, 30.0f, 180.0f, "%.0f seconds long")) g_s.length = std::round(len / 5.0f) * 5.0, Changed();
    ImGui::SameLine();
    ImGui::TextDisabled("about %d scenes", std::clamp(static_cast<int>(std::lround(g_s.length / 7.0)), 3, 24));
    const bool writing = g_scenesFut.valid();
    ImGui::BeginDisabled(writing || g_s.cast.empty() || g_s.step != 0);
    if (ImGui::Button(g_s.scenes.empty() ? "Write the scenes" : "Write the scenes again")) {
        std::string cast;
        for (size_t k = 0; k < g_s.cast.size(); ++k) {
            cast += "- " + CastLine(static_cast<int>(k));
            const std::string looks = Looks(static_cast<int>(k));
            if (!looks.empty()) cast += ": " + looks;
            cast += "\n";
        }
        g_scenesFut = std::async(std::launch::async, WriteScenes, cast, g_s.idea, g_s.length, app.comfy.url);
    }
    ImGui::EndDisabled();
    if (g_s.cast.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Add your characters first");
    const std::string st = Status();
    if (writing || !st.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ui::kAccentHover, "%s", st.empty() ? "Writing..." : st.c_str());
    }

    // the scenes
    double total = 0;
    for (const Scene& sc : g_s.scenes) total += sc.seconds;
    char head[64];
    snprintf(head, sizeof(head), "SCENES  (%d, %d:%02d)", static_cast<int>(g_s.scenes.size()), static_cast<int>(total) / 60, static_cast<int>(total) % 60);
    ui::SectionHeading(head);
    int up = -1, down = -1, remove = -1;
    for (size_t k = 0; k < g_s.scenes.size(); ++k) DrawScene(app, k, up, down, remove);
    if (up > 0) std::swap(g_s.scenes[up], g_s.scenes[up - 1]), Changed();
    if (down >= 0 && down + 1 < static_cast<int>(g_s.scenes.size())) std::swap(g_s.scenes[down], g_s.scenes[down + 1]), Changed();
    if (remove >= 0 && !g_s.scenes[remove].pictureAct && !g_s.scenes[remove].clipPending) g_s.scenes.erase(g_s.scenes.begin() + remove), Changed();
    if (ImGui::SmallButton("+ Add a scene")) {
        Scene sc;
        sc.uid = g_s.nextUid++;
        if (!g_s.scenes.empty()) sc.place = g_s.scenes.back().place, sc.who = g_s.scenes.back().who;
        g_s.scenes.push_back(sc);
        Changed();
    }

    // how it is made
    ui::SectionHeading("VIDEO");
    for (int q = 0; q < 3; ++q) {
        if (q) ImGui::SameLine();
        if (Chip(H3QualityName(q), g_s.quality == q)) g_s.quality = q, Changed();
    }
    ImGui::SameLine(0.0f, ui::Dp(16.0f));
    static const char* kResNames[] = {"480p", "640p", "768p"};
    for (int r = 0; r < 3; ++r) {
        ImGui::SameLine();
        char lbl[16];
        snprintf(lbl, sizeof(lbl), "%s##sr%d", kResNames[r], r);
        if (Chip(lbl, g_s.res == r)) g_s.res = r, Changed();
    }
    if (Chip("Portrait 9:16##ss0", g_s.shape == 0)) g_s.shape = 0, Changed();
    ImGui::SameLine();
    if (Chip("Landscape 16:9##ss1", g_s.shape == 1)) g_s.shape = 1, Changed();
    ImGui::SameLine();
    if (ImGui::Checkbox("Captions (the spoken lines)", &g_s.captions)) Changed();
    ImGui::SameLine();
    if (ImGui::Checkbox(g_s.shape == 0 ? "Export at 1080 x 1920" : "Export at 1920 x 1080", &g_s.fullHd)) Changed();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("The joined video at the size TikTok, Reels and Shorts expect (the clips are scaled up)");
    // one soundtrack for the whole story: the clips are made without music (each would get its own otherwise)
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Music:");
    ImGui::SameLine();
    ImGui::TextUnformatted(g_s.music.empty() ? "none" : Utf8(fs::path(g_s.music).filename().wstring()).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Choose...##music")) {
        wchar_t buf[MAX_PATH * 2] = {};
        OPENFILENAMEW ofn = {sizeof(ofn)};
        ofn.hwndOwner = GetActiveWindow();
        ofn.lpstrFilter = L"Music\0*.mp3;*.wav;*.m4a;*.aac;*.ogg;*.flac\0All files\0*.*\0";
        ofn.lpstrFile = buf;
        ofn.nMaxFile = MAX_PATH * 2;
        ofn.lpstrTitle = L"Music under the whole story";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&ofn)) g_s.music = buf, Changed();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("One soundtrack under the whole story, looped to its length, faded in and out, and quieter while people talk.\n"
                          "The clips themselves are made without music, or each would get its own.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Make music for it...")) {
        double secs = 0;
        const std::string idea = StoryMusicIdea(&secs);
        OpenMusicTab(app, idea, secs, true);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("The Sound page, with a soundtrack for this story to make (MiniMax Music 3);\nthen Under the story there");
    if (!g_s.music.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("x##music")) g_s.music.clear(), Changed();
        ImGui::SameLine();
        float vol = static_cast<float>(g_s.musicVolume * 100.0);
        ImGui::SetNextItemWidth(ui::Dp(140.0f));
        if (ImGui::SliderFloat("##musvol", &vol, 2.0f, 100.0f, "volume %.0f%%")) g_s.musicVolume = vol / 100.0, Changed();
    }
    if (!g_s.finalVideo.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ui::kGood, "The video:");
        ImGui::SameLine();
        ImGui::TextUnformatted(Utf8(fs::path(g_s.finalVideo).filename().wstring()).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Play##final")) PlayInVideoPage(app, g_s.finalVideo);
        ImGui::SameLine();
        if (ImGui::SmallButton("Show in Explorer##final")) {
            const std::wstring arg = L"/select,\"" + g_s.finalVideo + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
        }
    }
    ImGui::EndChild(); // the form

    // the footer: make it
    ImGui::Separator();
    const int n = static_cast<int>(g_s.scenes.size()), pics = CountPictures(), clips = CountClips();
    if (g_s.step == 0) {
        ImGui::BeginDisabled(n == 0 || g_scenesFut.valid());
        if (ui::PrimaryButton(clips == n && n > 0 ? "Join the clips again" : "Make everything", ImVec2(ui::Dp(190.0f), ui::Dp(38.0f)))) {
            if (clips == n) StartJoin(app), g_s.step = 4;
            else {
                for (Scene& sc : g_s.scenes) sc.error.clear();
                g_s.step = 1;
            }
            Changed();
        }
        ImGui::EndDisabled();
    } else {
        if (ImGui::Button("Stop after what is sent", ImVec2(ui::Dp(190.0f), ui::Dp(38.0f)))) {
            g_s.step = 0;
            g_promptsStop = true;
            Changed();
        }
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    // how far, and how long the rest takes
    H3Options o = VideoOptions();
    o.quality = g_s.quality;
    o.res = g_s.res;
    o.shape = 0;
    int w = 0, h = 0;
    H3SizeFor(o, g_s.shape == 0 ? 9 : 16, g_s.shape == 0 ? 16 : 9, w, h);
    double rest = 0;
    for (const Scene& sc : g_s.scenes) {
        if (sc.picture.empty()) rest += 25.0;
        if (sc.clip.empty()) {
            H3Options so = o;
            so.seconds = sc.seconds;
            rest += EstimateH3Seconds(so, w, h) + 20.0;
        }
    }
    static const char* kSteps[] = {"", "making the pictures", "writing the video prompts", "making the videos", "joining the clips"};
    ImGui::Text("Pictures %d / %d   Videos %d / %d%s%s", pics, n, clips, n, g_s.step ? "   - " : "", kSteps[std::clamp(g_s.step, 0, 4)]);
    if (rest > 0) ImGui::TextDisabled("about %d minutes left, %d x %d", static_cast<int>(std::ceil(rest / 60.0)), w, h);
    else ImGui::TextDisabled("%d x %d", w, h);
    ImGui::EndGroup();
}
