// The Video page's "Make a music video" mode: a song and your characters become one connected video that sings and acts it out.
//   1. the song (from the Sound page, with its timed lyrics, or any file) and the part of it, the cast and an idea;
//   2. the prompt writer plans the clips: the song is cut into overlapping clips of about 15 s (10 s at 736p), each with the lyric lines
//      sung in it, who is in it and what happens - one continuous take, each clip carrying straight on from the one before;
//   3. one ComfyUI job makes them all: H3's reference-to-video model with the cast's pictures (Star in a video's model, turbo and Veda), and
//      seitanism's ComfyUI-H3-Motion-Context-MultiRef (GPL-3.0, a node pack in ComfyUI: only its nodes are used, none of its code is here):
//      MiniMaxH3SongMaskedAVContext writes the exact slice of the song into each clip's sound and keeps H3 from changing it (so the singing
//      follows the real song), and copies the last 39 frames of the clip before into the start of the next; its streamer joins the clips
//      under the song.
// Tested 2026-10-29 (24 GB card, the pack's CC0 song and pictures, PRISM + the 4-step reference turbo + Veda, 960 x 544): two 15 s clips,
// 28.5 s of video, in 307 s from a cold start; the sound was the song itself (correlation 0.9999), the join could not be seen, and the
// singer's face, hair and clothes stayed the same through both.
// The plan is kept in %LOCALAPPDATA%\Try2Create\music_video.json.
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "app_state.h"
#include "board.h"
#include "clef.h"
#include "comfy.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "json.hpp"
#include "music_lyrics.h"
#include "notify.h"
#include "ui_extra.h"
#include "ui_theme.h"
#include "video_fx.h"
#include "video_util.h"

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
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string SafeName(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c)) out += static_cast<char>(c);
        else if (!out.empty() && out.back() != '_') out += '_';
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    if (out.size() > 40) out.resize(40);
    return out.empty() ? std::string("music_video") : out;
}

std::string Clock(double s) {
    char b[16];
    snprintf(b, sizeof(b), "%d:%02d", static_cast<int>(s) / 60, static_cast<int>(s) % 60);
    return b;
}

// ---- the plan
struct Member {
    std::wstring image;
    std::string name, role; // "Mia", "sings" / "dances" / "plays the guitar"
};

struct Clip {
    double start = 0; // in the part of the song (seconds)
    int frames = 362; // H3 frames (5 + 17k at 24 fps), the 39 carried over from the clip before included
    std::string lyrics; // the lines sung in it (from the timed lyrics; "" = none, or not known)
    std::string who;    // the cast in it, by name ("" = everyone)
    std::string action; // what happens (the writer's, then yours)
    std::string moves;  // the dance moves of this clip (each clip has its own: dancers looped one phrase when nothing said otherwise)
};

struct Plan {
    std::wstring song;
    std::string title = "My music video", idea;
    double from = 0, to = 0; // the part of the song (to 0 = to its end)
    std::vector<Member> cast;
    std::string look;        // one line of visual style for every clip
    std::string location, style, genre; // picked from the cards or typed: they go to the writer, for the clips and for Make a song
    std::vector<Clip> clips;
    int quality = 0;         // the model: 0 Fast, 1 Best, 2 Steady, 3 Omni, 4 Pika, 5 LTX-2.5 (VideoRefStack, kModels)
    int clipLen = 0;         // 0 clips of 15 s, 1 of 10 s
    int seed = 0;            // 0 = new every time
    int songSeconds = 90;    // Make a song
    bool songNoVoice = false;
    int res = 0;             // kRes
    int shape = 0;           // 0 portrait 9:16, 1 landscape 16:9
    bool captions = true;    // the lyrics burned in as captions (a copy: the video without them is kept)
    std::wstring finalVideo, captioned; // the video as made, and its copy with the lyrics ("" = none yet)
};
Plan g_p;
bool g_loaded = false, g_dirty = false;
double g_dirtyAt = 0;

struct Res {
    const char* name;
    int w, h;  // landscape
    int frames; // a clip's length: 15 s, or 10 s at 736p (memory)
};
const Res kRes[] = {{"544p", 960, 544, 362}, {"640p", 1152, 640, 362}, {"736p", 1280, 736, 243}};
constexpr int kContext = 39; // frames each clip shares with the one before (1.625 s): H3's 5/22/39/56 grid
constexpr int kLtxModel = 5;
bool Ltx() { return g_p.quality == kLtxModel; }
int ClipFrames() { return g_p.res == 2 || g_p.clipLen == 1 ? (Ltx() ? 241 : 243) : (Ltx() ? 361 : 362); }
int Overlap() { return Ltx() ? 1 : kContext; } // (LTX: each clip starts on the last frame of the one before)

// the song's facts, read when it is chosen
struct SongSection {
    std::string name; // "Verse 1", "Chorus 2", "Intro" (from the lyrics' tags), or "Part 3" (from the pauses in the singing)
    double start = 0, end = 0;
    std::string firstLine;
};
struct SongFacts {
    std::wstring path;
    double seconds = 0;
    std::vector<TimedLyric> lines; // timed lyrics (.srt next to it), empty = none
    std::string untimed;           // lyrics saved in the song without timing
    std::vector<SongSection> sections; // one after the other, from 0 to the song's end (empty: no timed lyrics)
};
SongFacts g_song;
std::future<SongFacts> g_songFut;

std::mutex g_statusMutex;
std::string g_status;
void SetStatus(const std::string& s) {
    std::lock_guard<std::mutex> lock(g_statusMutex);
    g_status = s;
}
std::string Status() {
    std::lock_guard<std::mutex> lock(g_statusMutex);
    return g_status;
}

std::wstring PlanPath() { return DataDir() + L"\\music_video.json"; }

void Save() {
    json j;
    j["song"] = Utf8(g_p.song);
    j["title"] = g_p.title;
    j["idea"] = g_p.idea;
    j["from"] = g_p.from;
    j["to"] = g_p.to;
    j["look"] = g_p.look;
    j["location"] = g_p.location;
    j["style"] = g_p.style;
    j["genre"] = g_p.genre;
    j["clipLen"] = g_p.clipLen;
    j["seed"] = g_p.seed;
    j["songSeconds"] = g_p.songSeconds;
    j["songNoVoice"] = g_p.songNoVoice;
    j["quality"] = g_p.quality;
    j["res"] = g_p.res;
    j["shape"] = g_p.shape;
    j["final"] = Utf8(g_p.finalVideo);
    j["captions"] = g_p.captions;
    j["captioned"] = Utf8(g_p.captioned);
    j["cast"] = json::array();
    for (const Member& m : g_p.cast) j["cast"].push_back({{"image", Utf8(m.image)}, {"name", m.name}, {"role", m.role}});
    j["clips"] = json::array();
    for (const Clip& c : g_p.clips) j["clips"].push_back({{"start", c.start}, {"frames", c.frames}, {"lyrics", c.lyrics}, {"who", c.who}, {"action", c.action}, {"moves", c.moves}});
    std::error_code ec;
    fs::create_directories(DataDir(), ec);
    std::ofstream f(PlanPath(), std::ios::binary | std::ios::trunc);
    if (f) f << j.dump(1, ' ', false, json::error_handler_t::replace);
    g_dirty = false;
}

void Changed() {
    g_dirty = true;
    g_dirtyAt = ImGui::GetTime();
}

SongFacts ReadSong(std::wstring path);

void Load() {
    g_loaded = true;
    std::ifstream f(PlanPath(), std::ios::binary);
    const json j = f ? json::parse(f, nullptr, false) : json();
    if (!j.is_object()) return;
    g_p.song = Wide(j.value("song", std::string()));
    g_p.title = j.value("title", g_p.title);
    g_p.idea = j.value("idea", std::string());
    g_p.from = std::max(0.0, j.value("from", 0.0));
    g_p.to = std::max(0.0, j.value("to", 0.0));
    g_p.look = j.value("look", std::string());
    g_p.location = j.value("location", std::string());
    g_p.style = j.value("style", std::string());
    g_p.genre = j.value("genre", std::string());
    g_p.clipLen = std::clamp(j.value("clipLen", 0), 0, 1);
    g_p.seed = j.value("seed", 0);
    g_p.songSeconds = std::clamp(j.value("songSeconds", 90), 20, 300);
    g_p.songNoVoice = j.value("songNoVoice", false);
    g_p.quality = std::clamp(j.value("quality", 0), 0, 5);
    g_p.res = std::clamp(j.value("res", 0), 0, 2);
    g_p.shape = std::clamp(j.value("shape", 0), 0, 1);
    g_p.finalVideo = Wide(j.value("final", std::string()));
    g_p.captions = j.value("captions", true);
    g_p.captioned = Wide(j.value("captioned", std::string()));
    if (j.contains("cast") && j["cast"].is_array())
        for (const json& c : j["cast"]) g_p.cast.push_back({Wide(c.value("image", std::string())), c.value("name", std::string()), c.value("role", std::string())});
    if (j.contains("clips") && j["clips"].is_array())
        for (const json& c : j["clips"])
            g_p.clips.push_back({c.value("start", 0.0), c.value("frames", 362), c.value("lyrics", std::string()), c.value("who", std::string()), c.value("action", std::string()), c.value("moves", std::string())});
    std::error_code ec;
    if (!g_p.song.empty() && fs::exists(g_p.song, ec)) g_songFut = std::async(std::launch::async, ReadSong, g_p.song);
    if (!g_p.captioned.empty() && !fs::exists(g_p.captioned, ec)) g_p.captioned.clear();
}

// ---- the song
// Its sections, from the timed lines: songs made on the Sound page keep their lyrics with the section tags ([intro] [verse] [chorus]...), and
// their .srt has one entry per sung line of them in order - so each tag's lines give its times. Otherwise (other lyrics, or the counts differ)
// the pauses of 2.5 s or more in the singing split it into parts. Each section runs from just before its first line to the next one.
std::vector<SongSection> FindSections(const std::vector<TimedLyric>& lines, const std::string& lyrics, double seconds) {
    struct Tagged {
        std::string name;
        int first = -1, count = 0; // its sung lines in the .srt
    };
    std::vector<Tagged> tags;
    int sung = 0;
    size_t a = 0;
    while (a <= lyrics.size() && !lyrics.empty()) {
        size_t b = lyrics.find('\n', a);
        if (b == std::string::npos) b = lyrics.size();
        std::string l = Trim(lyrics.substr(a, b - a));
        a = b + 1;
        if (l.empty()) continue;
        if (l.front() == '[') {
            std::string n = Trim(l.substr(1, l.find(']') == std::string::npos ? std::string::npos : l.find(']') - 1));
            if (!n.empty()) n[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(n[0])));
            tags.push_back({n.empty() ? std::string("Part") : n});
            continue;
        }
        if (tags.empty()) tags.push_back({"Start"});
        if (tags.back().first < 0) tags.back().first = sung;
        ++tags.back().count, ++sung; // (as SungLyricLines counts them: an ad-lib in brackets is sung too)
    }
    std::vector<SongSection> out;
    if (!tags.empty() && sung == static_cast<int>(lines.size())) {
        double lastEnd = 0;
        for (const Tagged& t : tags) {
            SongSection s;
            s.name = t.name;
            if (t.count > 0) {
                s.start = std::max(lastEnd, lines[t.first].start - 0.5);
                s.firstLine = Trim(lines[t.first].text);
                lastEnd = lines[t.first + t.count - 1].end;
            } else {
                s.start = out.empty() ? 0.0 : lastEnd + 0.3; // an instrumental part: after the singing before it
            }
            out.push_back(s);
        }
    } else {
        for (size_t k = 0; k < lines.size(); ++k) {
            if (k == 0 || lines[k].start - lines[k - 1].end >= 2.5) {
                SongSection s;
                s.name = "Part " + std::to_string(out.size() + 1);
                s.start = k == 0 ? 0.0 : std::max(lines[k - 1].end, lines[k].start - 0.5);
                s.firstLine = Trim(lines[k].text);
                out.push_back(s);
            }
        }
    }
    if (!out.empty()) out.front().start = 0; // (an intro before the first line belongs to the first section)
    // each section ends where the next starts; one shorter than a second goes into the one before
    for (size_t k = 0; k < out.size(); ++k) out[k].end = k + 1 < out.size() ? out[k + 1].start : seconds;
    std::vector<SongSection> kept;
    for (const SongSection& s : out) {
        if (s.end - s.start < 1.0 && !kept.empty()) kept.back().end = s.end;
        else kept.push_back(s);
    }
    // repeated names get numbers ("Chorus 1", "Chorus 2")
    const std::vector<SongSection> names = kept;
    for (size_t k = 0; k < kept.size(); ++k) {
        int same = 0, nth = 0;
        for (size_t j = 0; j < names.size(); ++j) same += names[j].name == names[k].name ? 1 : 0, nth += j <= k && names[j].name == names[k].name ? 1 : 0;
        if (same > 1 && names[k].name.find_first_of("0123456789") == std::string::npos) kept[k].name += " " + std::to_string(nth);
    }
    return kept;
}

// The lyrics heard from a song that came with none (Get the lyrics from the song): kept with the app's data, not next to your song
std::wstring HeardSrtPath(const std::wstring& song) {
    char h[32];
    snprintf(h, sizeof(h), "%016llx", static_cast<unsigned long long>(std::hash<std::wstring>{}(song)));
    return (fs::path(DataDir()) / L"music_video" / L"lyrics" / (Wide(h) + L".srt")).wstring();
}

SongFacts ReadSong(std::wstring path) {
    SongFacts s;
    s.path = path;
    const std::wstring ff = FfmpegPath();
    const std::wstring probe = ff.empty() ? std::wstring() : (fs::path(ff).parent_path() / L"ffprobe.exe").wstring();
    std::error_code ec;
    if (!probe.empty() && fs::exists(probe, ec)) {
        std::string out;
        RunCaptureHidden(L"\"" + probe + L"\" -v error -show_entries format=duration -of csv=p=0 \"" + path + L"\"", out, false);
        s.seconds = std::atof(out.c_str());
    }
    const std::wstring srt = SrtPathOf(path);
    if (fs::exists(srt, ec)) s.lines = ReadSrt(srt);
    if (s.lines.empty() && fs::exists(HeardSrtPath(path), ec)) s.lines = ReadSrt(HeardSrtPath(path));
    const std::string lyrics = LyricsFromSongFile(path);
    if (s.lines.empty()) s.untimed = lyrics;
    else s.sections = FindSections(s.lines, lyrics, s.seconds);
    return s;
}

double PartEnd() { return g_p.to > g_p.from + 1.0 ? std::min(g_p.to, g_song.seconds > 0 ? g_song.seconds : g_p.to) : g_song.seconds; }
double PartLength() { return std::max(0.0, PartEnd() - g_p.from); }

// The clips for a part this long: each `frames` long and starting (frames - 39) / 24 s after the one before; the last one only as long as
// what is left needs (rounded up to H3's 5 + 17k frames)
std::vector<Clip> ClipTimes(double seconds, int frames) {
    std::vector<Clip> out;
    if (seconds < 1.0) return out;
    const double step = (frames - Overlap()) / 24.0;
    double start = 0;
    while (true) {
        Clip c;
        c.start = start;
        const double left = seconds - start;
        if (left * 24.0 <= frames) {
            int need = static_cast<int>(std::ceil(left * 24.0));
            need = std::max(need, out.empty() ? 5 + 17 * 4 : kContext + 34); // (a last clip of a few frames: still a little past the shared ones)
            c.frames = Ltx() ? 1 + 8 * static_cast<int>(std::ceil((need - 1) / 8.0)) : 5 + 17 * static_cast<int>(std::ceil((need - 5) / 17.0));
            out.push_back(c);
            break;
        }
        c.frames = frames;
        out.push_back(c);
        start += step;
    }
    return out;
}

// the lyric lines sung inside a clip (times in the song, the clip's in the part)
std::string LyricsIn(double start, double seconds) {
    std::string out;
    const double a = g_p.from + start, b = a + seconds;
    for (const TimedLyric& l : g_song.lines) {
        if (l.end > a + 0.3 && l.start < b - 0.3) out += (out.empty() ? "" : " ") + Trim(l.text);
    }
    return out;
}

bool PlanFits() { // the clips are those of this song, part and size (else they are planned again)
    const std::vector<Clip> want = ClipTimes(PartLength(), ClipFrames());
    if (want.size() != g_p.clips.size() || want.empty()) return false;
    for (size_t k = 0; k < want.size(); ++k) {
        if (std::fabs(want[k].start - g_p.clips[k].start) > 0.01 || want[k].frames != g_p.clips[k].frames) return false;
    }
    return true;
}

std::string Looks(const Member& m) {
    const CharacterProfile* p = FindProfile(m.image);
    return p ? p->description : std::string();
}

std::string RoleOf(size_t k) {
    const std::string r = Trim(g_p.cast[k].role);
    return !r.empty() ? r : k == 0 ? "sings" : "dances";
}

bool Vocal(const std::string& role) {
    const std::string r = Lower(role);
    return r.find("sing") != std::string::npos || r.find("rap") != std::string::npos || r.find("vocal") != std::string::npos;
}

bool Dances(const std::string& role) { return Lower(role).find("danc") != std::string::npos; }

// Each clip its own moves, never the same one twice until the list is used up: the same words and the same noise made every clip's dance the same phrase, over and over
const char* const kMoves[] = {
    "a sharp body roll into a slow shoulder shimmy", "quick crossing footwork from side to side", "a full spin with the arms out, landing low",
    "hips swaying while both arms wave overhead", "a drop to one knee, then springing back up", "a backward glide on the toes",
    "popping and locking with hard robotic arm hits", "big jumps on the beat with a fist in the air", "a slow reach and turn, one hand tracing the air",
    "a side-step groove with a chest pop on every beat", "a low crouching shuffle, rising with the head rolled back", "a strut at the camera with a hair flip and a pointing hit",
    "a two-step, snapping the fingers", "a wave running through both arms from one hand to the other", "a kick and a quick pivot, then the other way",
    "sudden frozen poses on the beat, a new pose each bar", "a slow-motion lean back with the arms floating up", "stomping in a circle with the elbows out",
};

void GiveMoves(std::vector<Clip>& clips, unsigned seed) {
    bool anyone = false;
    for (size_t k = 0; k < g_p.cast.size(); ++k) anyone = anyone || Dances(RoleOf(k));
    if (!anyone) return;
    std::vector<int> order(std::size(kMoves));
    for (size_t k = 0; k < order.size(); ++k) order[k] = static_cast<int>(k);
    std::mt19937 rng(seed);
    std::shuffle(order.begin(), order.end(), rng);
    size_t at = 0;
    for (Clip& c : clips) {
        const int n = c.frames > 300 ? 4 : 3;
        c.moves.clear();
        for (int i = 0; i < n; ++i) c.moves += (i ? ", then " : "") + std::string(kMoves[order[at++ % order.size()]]);
    }
}

// who is in a clip: the cast its WHO names (everyone when it names nobody)
std::vector<int> WhoIn(const Clip& c) {
    std::vector<std::pair<size_t, int>> found;
    const std::string who = Lower(c.who);
    for (size_t k = 0; k < g_p.cast.size(); ++k) {
        std::string name = Lower(Trim(g_p.cast[k].name));
        if (const size_t sp = name.find(' '); sp != std::string::npos) name.resize(sp);
        const size_t at = name.empty() ? std::string::npos : who.find(name);
        if (at != std::string::npos) found.emplace_back(at, static_cast<int>(k));
    }
    std::sort(found.begin(), found.end());
    std::vector<int> out;
    for (const auto& f : found) out.push_back(f.second);
    if (out.empty())
        for (size_t k = 0; k < g_p.cast.size(); ++k) out.push_back(static_cast<int>(k));
    return out;
}

// ---- 1. the writer plans the clips
const char* const kPlanSystem =
    "You plan music videos made by a video model that hears the song. The video is {N} clips of about {S} seconds that form ONE continuous "
    "camera take: each clip starts exactly where the one before ended (the same frame, pose and camera), so there are NO cuts and no jumps. "
    "Change comes from the camera moving, the performers walking into a new space, and the place itself transforming (light changes, a room "
    "opens onto a beach, rain starts, walls fall away) - never from a cut.\n"
    "Rules:\n"
    "- Use only the performers listed, by their exact names; nobody else has a face in the video.\n"
    "- WHO: the names of the performers in that clip. Everyone who sings is in every clip with lyrics.\n"
    "- ACTION: 2 to 4 sentences in plain words: where they are, what they do (they sing when the clip has lyrics; in an instrumental clip "
    "they dance, move and look, mouths relaxed), how the camera moves, and how the clip ends with the movement still going, so the next "
    "clip can carry it on. Always name people, never just he / she. Match the mood and the words of the lyrics in that clip.\n"
    "- A clip given MOVES: whoever dances does exactly those moves, in that order, and names them in ACTION. No move is repeated from clip to clip, and nobody "
    "loops one phrase of a dance.\n"
    "- If a place, a look or a music genre is given, the whole video follows it: every clip stays true to them, the performance suits the genre.\n"
    "- Their clothes stay the same through the whole video. No written words, signs or captions anywhere.\n"
    "- LOOK: one sentence of visual style for the whole video (light, colour, lens, film look).\n"
    "- TITLE: a short title for the video.";

struct PlanResult {
    std::string title, look, error;
    std::vector<std::pair<std::string, std::string>> clips; // who, action
};
std::future<PlanResult> g_planFut;

PlanResult WritePlan(std::string user, int n, double clipSeconds, std::string url) {
    PlanResult r;
    if (!WriterConfigured()) {
        r.error = "The prompt writer is not set up (Clef mode > Prompt writer).";
        return r;
    }
    std::string system = kPlanSystem;
    system.replace(system.find("{N}"), 3, std::to_string(n));
    system.replace(system.find("{S}"), 3, std::to_string(static_cast<int>(std::lround(clipSeconds))));
    const json schema = {{"type", "object"},
                         {"properties",
                          {{"title", {{"type", "string"}, {"minLength", 2}}},
                           {"look", {{"type", "string"}, {"minLength", 20}}},
                           {"clips",
                            {{"type", "array"},
                             {"minItems", n},
                             {"maxItems", n},
                             {"items",
                              {{"type", "object"},
                               {"properties", {{"who", {{"type", "string"}, {"minLength", 2}}}, {"action", {{"type", "string"}, {"minLength", 60}}}}},
                               {"required", {"who", "action"}}}}}}}},
                         {"required", {"title", "look", "clips"}}};
    const ClefSession session = WriterAcquire(url, SetStatus);
    if (!session.ok) {
        WriterRelease(session, false);
        r.error = "The prompt writer could not be started.";
        return r;
    }
    SetStatus("Planning the clips...");
    std::string out, err;
    const bool ok = WriterChatJson(system, user, {}, schema.dump(), 7000, 0.8, out, &err);
    WriterRelease(session, false);
    SetStatus("");
    if (!ok) {
        r.error = "The prompt writer did not answer" + (err.empty() ? std::string() : ": " + err);
        return r;
    }
    const json j = json::parse(out, nullptr, false);
    if (!j.is_object() || !j.contains("clips") || !j["clips"].is_array()) {
        r.error = "The writer's answer had no clips in it. Try again.";
        return r;
    }
    r.title = Trim(j.value("title", std::string()));
    r.look = Trim(j.value("look", std::string()));
    for (const json& c : j["clips"])
        if (c.is_object()) r.clips.emplace_back(Trim(c.value("who", std::string())), Trim(c.value("action", std::string())));
    if (r.clips.empty()) r.error = "The writer's answer had no clips in it. Try again.";
    return r;
}

void StartPlan(AppState& app) {
    if (g_planFut.valid() || g_p.cast.empty() || PartLength() < 1.0) return;
    std::vector<Clip> clips = ClipTimes(PartLength(), ClipFrames());
    for (Clip& c : clips) c.lyrics = LyricsIn(c.start, c.frames / 24.0);
    GiveMoves(clips, g_p.seed ? static_cast<unsigned>(g_p.seed) : std::random_device{}());
    std::string user = "The song: " + (g_p.title.empty() ? Utf8(fs::path(g_p.song).stem().wstring()) : g_p.title) + ".\nThe performers:\n";
    for (size_t k = 0; k < g_p.cast.size(); ++k) {
        const std::string looks = Looks(g_p.cast[k]);
        user += "- " + g_p.cast[k].name + " (" + RoleOf(k) + ")" + (looks.empty() ? std::string() : ": " + looks) + "\n";
    }
    user += "The idea: " + (g_p.idea.empty() ? std::string("make up one that suits the song and the performers") : g_p.idea) + "\n";
    if (!g_p.location.empty()) user += "The place: " + g_p.location + "\n";
    if (!g_p.style.empty()) user += "The look: " + g_p.style + "\n";
    if (!g_p.genre.empty()) user += "The music genre: " + g_p.genre + "\n";
    // (tested 2026-10-30: a push-in on a hand, then a pull back: LTX drew new clothes on what had left the frame)
    if (Ltx()) user += "The camera stays far enough back to show everyone full-length in every clip, moving around them, never in close on a face or a hand: what leaves the frame comes back changed.\n";
    if (g_song.lines.empty() && !g_song.untimed.empty()) user += "The lyrics (their timing is not known):\n" + g_song.untimed.substr(0, 3000) + "\n";
    user += "The clips, in order:\n";
    for (size_t k = 0; k < clips.size(); ++k) {
        user += "Clip " + std::to_string(k + 1) + " (" + Clock(clips[k].start) + "-" + Clock(clips[k].start + clips[k].frames / 24.0) + "): " +
                (clips[k].lyrics.empty() ? std::string(g_song.lines.empty() ? "(lyrics not timed)" : "(instrumental)") : "lyrics \"" + clips[k].lyrics + "\"") +
                (clips[k].moves.empty() ? std::string() : ". MOVES: " + clips[k].moves) + "\n";
    }
    g_p.clips = std::move(clips); // (the times now; the writer's words when it answers)
    Changed();
    g_planFut = std::async(std::launch::async, WritePlan, user, static_cast<int>(g_p.clips.size()), (ClipFrames() - Overlap()) / 24.0, app.comfy.url);
}

// ---- 2. each clip's H3 prompt (the structure H3's prompt guide and the pack's example use)
bool Ascii(const std::string& s) {
    return std::all_of(s.begin(), s.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
}

std::string ClipPrompt(size_t k) {
    const Clip& c = g_p.clips[k];
    const std::vector<int> who = WhoIn(c);
    const bool last = k + 1 == g_p.clips.size();
    std::string p = "subject_definitions:\n", singers;
    int singer = -1, voices = 0;
    for (size_t i = 0; i < who.size(); ++i) {
        const Member& m = g_p.cast[who[i]];
        const std::string n = std::to_string(i + 1), looks = Looks(m), role = RoleOf(who[i]);
        p += "<Subject " + n + "> is " + m.name + ", the person in <Picture " + n + ">, who " + role + (looks.empty() ? std::string() : ": " + looks) + "\n";
        if (Vocal(role)) {
            if (singer < 0) singer = static_cast<int>(i);
            singers += (singers.empty() ? "<Subject " : " and <Subject ") + n + ">";
            ++voices;
        }
    }
    const std::string sing = voices > 1 ? singers + " take turns and sing together on the hooks, each with" : singers + " visibly sings with";
    std::string first = c.action.substr(0, c.action.find(". ") == std::string::npos ? c.action.size() : c.action.find(". ") + 1);
    p += "\nsummary:\n[reference generation] " + std::string(k == 0 ? "Begin one continuous music-video take. " : "Continue the same unbroken take from the incoming frames. ") + first + "\n";
    p += "\nretention_analysis:\n";
    for (size_t i = 0; i < who.size(); ++i) {
        const std::string n = std::to_string(i + 1);
        p += "<Subject " + n + "> (appears throughout [Shot 1]): fully_preserved - preserve the facial identity, hair, body proportions, wardrobe, colours and "
             "distinctive details of <Picture " + n + "> while expression, pose, lighting, framing and movement follow the performance" +
             (k == 0 ? "." : ", continuing from the protected incoming frames.") + "\n";
    }
    p += "\ndetailed_description:\n[Shot 1] ";
    if (k > 0) p += c.moves.empty() ? "Continue the exact incoming motion, pose and camera move without any cut. " : "Carry straight on from the incoming frames without any cut, then go into the new moves. ";
    p += c.action;
    if (!p.empty() && p.back() != '.') p += ".";
    if (!c.moves.empty())
        for (size_t i = 0; i < who.size(); ++i)
            if (Dances(RoleOf(who[i]))) p += " <Subject " + std::to_string(i + 1) + "> dances, a new move every few seconds and never the same one twice: " + c.moves + ".";
    if (!c.lyrics.empty() && singer >= 0) {
        p += " " + sing + " clear, readable mouth and jaw movement in exact sync with the protected song: <d>" +
             std::string(Ascii(c.lyrics) ? "[English] " : "") + c.lyrics + "</d>";
    } else if (c.lyrics.empty() && !g_song.lines.empty()) {
        p += " This part of the song is instrumental: nobody sings, mouths stay relaxed while they move with the music.";
    } else if (singer >= 0) {
        p += " " + singers + (voices > 1 ? " sing" : " sings") + " along to the vocals of the protected song wherever there are vocals.";
    }
    if (!g_p.look.empty()) p += "\n\n" + g_p.look;
    if (!g_p.style.empty()) p += "\n\nThe look: " + g_p.style + ".";
    p += std::string("\n\nNo written words, signs or captions anywhere. ") +
         (last ? "End on a held, final image as the song ends." : "End with the movement and the camera still under way, so the next clip continues the same shot.");
    p += "\n\noverall_soundscape:\nVery subtle ambience beneath the song.\n\nnon_diegetic_music:\nThe protected master-song audio drives the singing, breathing, "
         "dancing, body rhythm and emotional timing.";
    return p;
}

// On LTX-2.5: one flowing paragraph (the way it is written for), the sound being the song's own
std::string ClipPromptLtx(size_t k) {
    const Clip& c = g_p.clips[k];
    std::string p = c.action;
    if (!p.empty() && p.back() != '.') p += ".";
    const std::vector<int> who = WhoIn(c);
    for (int i : who) {
        if (!c.moves.empty() && Dances(RoleOf(i))) p += " " + g_p.cast[i].name + " dances, a new move every few seconds and never the same one twice: " + c.moves + ".";
        if (!c.lyrics.empty() && Vocal(RoleOf(i))) p += " " + g_p.cast[i].name + " sings to the music with clear mouth movement: \"" + c.lyrics + "\".";
    }
    if (!g_p.look.empty()) p += " " + g_p.look;
    if (!g_p.style.empty()) p += " The look: " + g_p.style + ".";
    return p + " Their faces and clothes stay the same. No written words anywhere.";
}

// ---- 3. the job: every clip in one graph, joined under the song
void VideoSize(int& w, int& h) {
    const Res& r = kRes[std::clamp(g_p.res, 0, 2)];
    w = g_p.shape == 0 ? r.h : r.w;
    h = g_p.shape == 0 ? r.w : r.h;
    if (Ltx()) w = 64 * static_cast<int>(std::lround(w / 64.0)), h = 64 * static_cast<int>(std::lround(h / 64.0)); // (LTX's sizes are multiples of 64: the first pass is half)
}

json BuildGraph(const H3RefStack& s, unsigned seed) {
    json g;
    auto node = [&](const std::string& id, const char* cls, json inputs, const std::string& title = {}) {
        g[id] = {{"class_type", cls}, {"inputs", std::move(inputs)}, {"_meta", {{"title", title.empty() ? std::string(cls) : title}}}};
    };
    auto L = [](const std::string& id, int slot = 0) { return json::array({id, slot}); };
    node("unet", "UNETLoader", {{"unet_name", s.unet}, {"weight_dtype", "default"}});
    std::string model = "unet";
    if (!s.turbo.empty()) node("turbo", "LoraLoaderModelOnly", {{"model", L(model)}, {"lora_name", s.turbo}, {"strength_model", 1.0}}), model = "turbo";
    if (s.shiftVideo > 0) node("shift", "MiniMaxH3SigmaShift", {{"model", L(model)}, {"shift_video", s.shiftVideo}, {"shift_audio", s.shiftAudio}}), model = "shift";
    if (s.veda) {
        node("veda", "VedaSparseAttention", {{"model", L(model)}, {"predictor", s.vedaPredictor}, {"generated_sparsity", "32"}, {"reference_sparsity", "32"},
                                             {"full_attention_layers", ""}, {"full_attention_steps", ""}, {"verbose", false}});
        model = "veda";
    }
    node("clip", "CLIPLoader", {{"clip_name", s.textEncoder}, {"type", "minimax"}, {"device", "default"}});
    node("vvae", "VAELoader", {{"vae_name", s.videoVae}});
    node("avae0", "VAELoader", {{"vae_name", s.audioVae}});
    node("avae", "MiniMaxH3AudioVAECompatibility", {{"audio_vae", L("avae0")}});
    node("song", "LoadAudio", {{"audio", "@song"}}, "The song (the part used, padded)");
    for (size_t k = 0; k < g_p.cast.size(); ++k) node("pic" + std::to_string(k + 1), "LoadImage", {{"image", "@pic" + std::to_string(k + 1)}}, g_p.cast[k].name);
    node("ksel", "KSamplerSelect", {{"sampler_name", s.sampler}});
    if (!s.sigmas.empty()) node("sched", "ManualSigmas", {{"sigmas", s.sigmas}});
    else node("sched", "BasicScheduler", {{"model", L(model)}, {"scheduler", s.scheduler}, {"steps", s.steps}, {"denoise", 1.0}});
    int w = 0, h = 0;
    VideoSize(w, h);
    std::string prev;
    for (size_t k = 0; k < g_p.clips.size(); ++k) {
        const Clip& c = g_p.clips[k];
        const std::string n = std::to_string(k + 1);
        json r2v = {{"clip", L("clip")}, {"vae", L("vvae")}, {"audio_vae", L("avae")}, {"prompt", ClipPrompt(k)}, {"width", w}, {"height", h},
                    {"length", c.frames}, {"ref_image_size", "match"}};
        const std::vector<int> who = WhoIn(c);
        for (size_t i = 0; i < who.size(); ++i) r2v["ref_images.ref_image_" + std::to_string(i)] = L("pic" + std::to_string(who[i] + 1));
        node("r2v" + n, "MiniMaxH3ReferenceToVideo", r2v, "Clip " + n + ": the pictures and its prompt");
        json ctx = {{"latent", L("r2v" + n, 1)}, {"audio_vae", L("avae")}, {"master_audio", L("song")}, {"clip_start_seconds", c.start},
                    {"context_length", prev.empty() ? 0 : kContext}, {"source_fps", 24.0}, {"crop", "disabled"}};
        if (!prev.empty()) ctx["source_latent"] = L(prev);
        node("ctx" + n, "MiniMaxH3SongMaskedAVContext", ctx, "Clip " + n + ": its slice of the song, and the clip before's last frames");
        node("noise" + n, "RandomNoise", {{"noise_seed", seed + static_cast<unsigned>(k) * 7919u}}); // (each clip its own: the same noise brought the same motion back)
        node("guide" + n, "BasicGuider", {{"model", L(model)}, {"conditioning", L("r2v" + n)}});
        node("samp" + n, "SamplerCustomAdvanced", {{"noise", L("noise" + n)}, {"guider", L("guide" + n)}, {"sampler", L("ksel")}, {"sigmas", L("sched")},
                                                   {"latent_image", L("ctx" + n)}},
             "Clip " + n);
        prev = "samp" + n;
    }
    json fin = {{"video_vae", L("vvae")}, {"master_audio", L("song")}, {"input_count", static_cast<int>(g_p.clips.size())}, {"context_frames", kContext},
                {"video_overlap_frames", kContext}, {"filename_prefix", "video/MusicVideo_" + SafeName(g_p.title)}, {"pix_fmt", "yuv420p"}, {"crf", 19},
                {"save_metadata", false}, {"trim_to_audio", false}, {"save_output", true}};
    for (size_t k = 0; k < g_p.clips.size(); ++k) fin["clip_" + std::to_string(k + 1)] = L("samp" + std::to_string(k + 1));
    node("final", "MiniMaxH3StreamLiveMusicVideoToVHS", fin, "The clips joined under the song");
    node("done", "MiniMaxH3FinalizeVHSOutput", {{"filenames", L("final")}});
    return g;
}

// The part of the song, from 0, with 2 s of silence after it: the last clip may run a few frames past the song (it ends on H3's frame grid)
std::wstring CutSong(std::wstring song, double from, double seconds, std::wstring name) {
    const std::wstring ff = FfmpegPath();
    if (ff.empty()) return {};
    std::error_code ec;
    const fs::path dir = fs::path(DataDir()) / L"music_video";
    fs::create_directories(dir, ec);
    const std::wstring out = (dir / name).wstring();
    wchar_t args[256];
    swprintf(args, 256, L" -y -v error -ss %.3f -i \"", from);
    wchar_t af[160];
    swprintf(af, 160, L"\" -vn -af \"atrim=end=%.3f,asetpts=PTS-STARTPTS,apad=pad_dur=2\" -ar 48000 -ac 2 \"", seconds);
    std::string log;
    RunCaptureHidden(L"\"" + ff + L"\"" + args + song + af + out + L"\"", log, true);
    return fs::exists(out, ec) && fs::file_size(out, ec) > 1000 ? out : std::wstring();
}

// LTX-2.5's clips are saved one by one: here they become one video under the song
std::wstring JoinClips(std::vector<std::wstring> clips, std::wstring song, double seconds, std::wstring out) {
    const std::wstring ff = FfmpegPath();
    if (ff.empty() || clips.empty()) return {};
    std::error_code ec;
    const fs::path list = fs::path(DataDir()) / L"music_video" / L"join.txt";
    fs::create_directories(list.parent_path(), ec);
    {
        std::ofstream f(list, std::ios::binary | std::ios::trunc);
        for (const std::wstring& c : clips) {
            std::string p = Utf8(c);
            std::replace(p.begin(), p.end(), '\\', '/');
            f << "file '" << p << "'\n";
        }
    }
    wchar_t len[64];
    swprintf(len, 64, L" -t %.3f ", seconds);
    std::string log;
    RunCaptureHidden(L"\"" + ff + L"\" -y -v error -f concat -safe 0 -i \"" + list.wstring() + L"\" -i \"" + song + L"\" -map 0:v -map 1:a -c:v copy -c:a aac -b:a 256k" + len +
                         L"-movflags +faststart \"" + out + L"\"", log, true);
    return fs::exists(out, ec) && fs::file_size(out, ec) > 1000 ? out : std::wstring();
}

struct Run {
    std::future<std::wstring> cut;
    std::future<std::wstring> join;
    json graph;
    int activity = 0;
    int scene = 0;   // LTX-2.5: the first picture, everyone in the place, being drawn
    bool ltx = false;
    unsigned seed = 42;
    std::wstring part;
    std::string label;
};
std::unique_ptr<Run> g_run;
std::future<std::wstring> g_hearFut; // "Hear it": the part cut to a file, then played
std::future<std::string> g_hearLyrFut; // "Get the lyrics from the song": whisper listening ("" = done, else why not)
std::string g_hearNote;
bool g_makeAfterPlan = false; // Make was pressed before the clips were planned: they are planned, then it is made

double EstimateSeconds() { // from the test: 2 x 15 s at 960 x 544 in 307 s from cold (about 125 s a clip, 60 s of loading)
    int w = 0, h = 0;
    VideoSize(w, h);
    double s = 60.0;
    const double px = static_cast<double>(w) * h / (960.0 * 544.0);
    if (Ltx()) { // (Animate's measured LTX-2.5 times, for the clips of the part; 60 s of loading and about 40 s for the first picture)
        s += 40.0;
        for (const Clip& c : ClipTimes(PartLength(), ClipFrames())) s += 6.0 + 52.0 * (static_cast<double>(w) * h / 1e6) * (c.frames / 24.0 / 5.0);
        return s;
    }
    const H3RefStack st = VideoRefStack(g_p.quality);
    const double steps = st.steps / 4.0 * (st.turbo.empty() ? 1.15 : 1.0); // (the model alone: no LoRA, but cfg 1 all the same)
    // (the clips planned, or those the part will be cut into; PlanFits is defined below, so the times are compared here)
    const std::vector<Clip> want = ClipTimes(PartLength(), ClipFrames());
    bool same = want.size() == g_p.clips.size();
    for (size_t k = 0; same && k < want.size(); ++k) same = want[k].frames == g_p.clips[k].frames;
    for (const Clip& c : same ? g_p.clips : want) s += 125.0 * (c.frames / 362.0) * px * steps * (st.veda ? 1.0 : 2.4);
    return s;
}

// ---- 4. the lyrics as captions: a copy of the video with the timed lines burned in (ffmpeg's subtitles filter, as Make a story's captions),
// each line on screen while it is sung; the picture is encoded again, the sound copied
std::string AssTime(double t) {
    t = std::max(0.0, t);
    const int cs = static_cast<int>(std::lround(t * 100.0));
    char buf[32];
    snprintf(buf, sizeof(buf), "%d:%02d:%02d.%02d", cs / 360000, (cs / 6000) % 60, (cs / 100) % 60, cs % 100);
    return buf;
}

// the lines of the part used, in the video's time (from 0)
std::vector<TimedLyric> PartLyrics() {
    std::vector<TimedLyric> out;
    const double a = g_p.from, b = PartEnd();
    for (const TimedLyric& l : g_song.lines) {
        if (l.end <= a + 0.1 || l.start >= b - 0.1 || Trim(l.text).empty()) continue;
        out.push_back({std::max(0.0, l.start - a), std::min(b, l.end) - a, Trim(l.text)});
    }
    return out;
}

std::future<std::pair<bool, std::string>> g_capFut;

std::pair<bool, std::string> BurnLyrics(std::wstring video, std::vector<TimedLyric> lines, std::wstring out) {
    const std::wstring ff = FfmpegPath();
    if (ff.empty()) return {false, "ffmpeg.exe was not found (it puts the lyrics on the video)"};
    const VideoInfo vi = ProbeVideo(video);
    const int W = std::max(64, vi.width), H = std::max(64, vi.height);
    const int size = std::max(18, std::min(W, H) / 14), outline = std::max(2, std::min(W, H) / 200), margin = H / 9;
    std::string ass = "[Script Info]\nScriptType: v4.00+\nPlayResX: " + std::to_string(W) + "\nPlayResY: " + std::to_string(H) +
                      "\nWrapStyle: 0\n\n[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, "
                      "Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
                      "Style: Default,Arial," + std::to_string(size) + ",&H00FFFFFF,&H000000FF,&H00000000,&H80000000,-1,0,0,0,100,100,0,0,1," +
                      std::to_string(outline) + ",1,2," + std::to_string(W / 12) + "," + std::to_string(W / 12) + "," + std::to_string(margin) +
                      ",1\n\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";
    for (const TimedLyric& l : lines) {
        std::string t = l.text;
        for (char& c : t) c = c == '{' || c == '}' || c == '\n' || c == '\r' ? ' ' : c;
        ass += "Dialogue: 0," + AssTime(l.start) + "," + AssTime(l.end + 0.15) + ",Default,,0,0,0,," + t + "\n";
    }
    const fs::path dir = fs::path(DataDir()) / L"music_video";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::wstring assPath = (dir / L"lyrics.ass").wstring();
    {
        std::ofstream a(assPath, std::ios::binary | std::ios::trunc);
        a << ass;
    }
    std::string p = Utf8(assPath); // a path inside a filter: C\:/Users/...
    std::replace(p.begin(), p.end(), '\\', '/');
    std::string esc;
    for (char c : p) esc += c == ':' ? std::string("\\:") : c == '\'' ? std::string("\\'") : std::string(1, c);
    fs::remove(out, ec);
    std::string log;
    RunCaptureHidden(L"\"" + ff + L"\" -y -hide_banner -loglevel error -i \"" + video + L"\" -vf \"subtitles='" + Wide(esc) +
                         L"'\" -c:v libx264 -preset medium -crf 19 -pix_fmt yuv420p -c:a copy -movflags +faststart \"" + out + L"\"",
                     log, true);
    if (!fs::exists(out, ec) || fs::file_size(out, ec) < 1000) {
        if (log.size() > 300) log = log.substr(log.size() - 300);
        return {false, "ffmpeg could not put the lyrics on it: " + (log.empty() ? std::string("no output") : log)};
    }
    return {true, Utf8(out)};
}

bool CanCaption() { return !g_song.lines.empty() && !PartLyrics().empty(); }

void StartCaptions() {
    if (g_capFut.valid() || g_p.finalVideo.empty() || !CanCaption()) return;
    const fs::path v(g_p.finalVideo);
    const std::wstring out = (v.parent_path() / (v.stem().wstring() + L"_lyrics.mp4")).wstring();
    g_capFut = std::async(std::launch::async, BurnLyrics, g_p.finalVideo, PartLyrics(), out);
}

// what Play shows: the copy with the lyrics when captions are on and it is there
std::wstring ShownVideo() { return g_p.captions && !g_p.captioned.empty() ? g_p.captioned : g_p.finalVideo; }

// ---- the node pack (seitanism/ComfyUI-H3-Motion-Context-MultiRef, the commit tested), put into ComfyUI by your click
const char* const kPackCommit = "361624fb406b63eb6694442eac6c895fc1533a70";
const wchar_t* const kPackDir = L"ComfyUI-H3-Motion-Context-MultiRef";
std::atomic<bool> g_packBusy{false};
std::mutex g_packMutex;
std::string g_packNote;

bool PackThere() {
    const std::wstring install = ComfyInstallDir();
    std::error_code ec;
    return !install.empty() && fs::exists(fs::path(ComfyAppDir()) / L"custom_nodes" / kPackDir / L"h3_song_audio_context.py", ec);
}

void InstallPack() {
    const std::wstring install = ComfyInstallDir();
    if (install.empty() || g_packBusy) return;
    g_packBusy = true;
    std::thread([install]() {
        std::error_code ec;
        const fs::path work = fs::path(DataDir()) / L"music_video" / L"pack";
        fs::remove_all(work, ec);
        fs::create_directories(work, ec);
        const fs::path script = work / L"get_pack.ps1", log = work / L"get_pack.log";
        {
            std::ofstream f(script, std::ios::binary | std::ios::trunc);
            f << "$ErrorActionPreference = 'Stop'\n"
                 "$ProgressPreference = 'SilentlyContinue'\n"
                 "$w = '" << Utf8(work.wstring()) << "'\n"
                 "$dest = '" << Utf8((fs::path(ComfyAppDir()) / L"custom_nodes" / kPackDir).wstring()) << "'\n"
                 "Invoke-WebRequest 'https://github.com/seitanism/ComfyUI-H3-Motion-Context-MultiRef/archive/" << kPackCommit << ".zip' -OutFile \"$w\\pack.zip\"\n"
                 "Expand-Archive \"$w\\pack.zip\" \"$w\\x\" -Force\n"
                 "$src = Get-ChildItem \"$w\\x\" -Directory | Select-Object -First 1\n"
                 "if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }\n"
                 "Move-Item $src.FullName $dest\n"
                 "'ok'\n";
        }
        std::string out;
        RunCaptureHidden(L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + script.wstring() + L"\"", out, true);
        const bool ok = PackThere();
        std::lock_guard<std::mutex> lock(g_packMutex);
        g_packNote = ok ? "The music video nodes are in ComfyUI: restart ComfyUI once so it loads them." : "Could not get the nodes: " + Trim(out).substr(0, 200);
        g_packBusy = false;
    }).detach();
}

void StartMake(AppState& app) {
    if (g_run || g_p.clips.empty() || g_p.cast.empty()) return;
    const int has = Ltx() ? 1 : ComfyHasNode(app.comfy.url, "MiniMaxH3SongMaskedAVContext");
    if (has == 0) {
        Toast(ToastKind::Info, "Restart ComfyUI once", PackThere() ? "so it loads the music video nodes (they are in its custom_nodes)." : "The music video nodes are not in ComfyUI yet: Get the nodes, then restart it.",
              {}, {}, "musicvideo");
        return;
    }
    auto r = std::make_unique<Run>();
    r->seed = g_p.seed ? static_cast<unsigned>(g_p.seed) : std::random_device{}() % 2000000000u + 1u;
    r->ltx = Ltx();
    if (!r->ltx) r->graph = BuildGraph(VideoRefStack(g_p.quality), r->seed);
    r->label = "Music video: " + g_p.title;
    r->cut = std::async(std::launch::async, CutSong, g_p.song, g_p.from, PartLength(), std::wstring(L"song_part.wav"));
    g_p.finalVideo.clear();
    g_p.captioned.clear();
    Changed();
    g_run = std::move(r);
}

// LTX-2.5 has no reference pictures: the first picture has everyone, in the place, at the video's shape (FLUX 3 or Qwen, as a TV scene card's)
void StartLtxScene(AppState& app) {
    ComfyJob job;
    std::string who;
    for (size_t k = 0; k < g_p.cast.size(); ++k) {
        job.images.push_back(g_p.cast[k].image);
        who += "<image" + std::to_string(k + 1) + "> is " + g_p.cast[k].name + ". ";
    }
    const std::string place = !g_p.location.empty() ? g_p.location : !g_p.clips.empty() && !g_p.clips.front().action.empty() ? g_p.clips.front().action : g_p.idea;
    job.prompt = who + "One real photo of exactly " + std::to_string(g_p.cast.size()) + (g_p.cast.size() == 1 ? " person" : " people") + " together in this place: " + place +
                 ". Keep every person recognisably the same: the same face, hair, skin, build and clothes. Never add or duplicate anyone. They stand full-length, facing the camera, in the "
                 "frame with their heads included, with clear sharp faces. The light on them matches the place." + (g_p.style.empty() ? std::string() : " The look: " + g_p.style + ".");
    job.canvas = g_p.shape == 0 ? 5 : 4;
    job.label = "Music video: everyone in the place";
    job.editEngine = Flux3EditReady() ? "flux3" : "qwen";
    job.times = 1;
    job.quiet = true;
    g_run->scene = SendComfyJob(app, std::move(job));
}

void SendLtxClips(AppState& app, const std::wstring& first) {
    std::vector<LtxMusicClip> clips;
    for (size_t k = 0; k < g_p.clips.size(); ++k) clips.push_back({ClipPromptLtx(k), g_p.clips[k].start, g_p.clips[k].frames});
    int w = 0, h = 0;
    VideoSize(w, h);
    const json graph = BuildLtxMusic(clips, w, h, g_run->seed, "video/MusicLTX_" + SafeName(g_p.title) + "_" + std::to_string(g_run->seed));
    ComfyJob job;
    job.apiWorkflow = graph.dump();
    {
        std::ofstream dump(DataDir() + L"\\music_video_last.json", std::ios::trunc);
        dump << graph.dump(1);
    }
    job.rawUploads = {{"@song", g_run->part}, {"@first", first}};
    job.label = g_run->label;
    job.times = 1;
    job.seed = 42;
    job.quiet = true; // (the joined video is the result)
    const int n = static_cast<int>(g_p.clips.size());
    for (int k = 0; k < n; ++k) {
        const std::string name = "Clip " + std::to_string(k + 1) + " of " + std::to_string(n) + " (" + Clock(g_p.clips[k].start) + " - " + Clock(g_p.clips[k].start + g_p.clips[k].frames / 24.0) + ")";
        job.stages.push_back(name);
        job.stages.push_back(name + ", sharpening");
    }
    job.finishing = "Saving the last clip";
    g_run->activity = SendComfyJob(app, std::move(job));
    if (!g_run->activity) g_run.reset();
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

// A paragraph box that wraps as you type and grows to show all of it (2026-10-30: a box 2 lines high cut the writer's text off, and typing ran off to the side). Its
// line ends are spaces changed in place to line feeds, so the text keeps its length and the cursor stays where it is
void WrapPlace(char* s, int len, float width) {
    float x = 0;
    int lastSpace = -1;
    for (int k = 0; k < len; ++k) {
        if (s[k] == '\n') s[k] = ' ';
        const float cw = ImGui::CalcTextSize(s + k, s + k + 1).x;
        if (s[k] == ' ') lastSpace = k;
        x += cw;
        if (x > width && lastSpace >= 0) {
            s[lastSpace] = '\n';
            x = ImGui::CalcTextSize(s + lastSpace + 1, s + k + 1).x;
            lastSpace = -1;
        }
    }
}

struct BoxEdit {
    std::string* buf;
    float width;
};

int BoxCallback(ImGuiInputTextCallbackData* d) {
    auto* b = static_cast<BoxEdit*>(d->UserData);
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        b->buf->resize(d->BufTextLen);
        d->Buf = b->buf->data();
    } else if (d->EventFlag == ImGuiInputTextFlags_CallbackAlways) {
        std::string was(d->Buf, d->BufTextLen);
        WrapPlace(d->Buf, d->BufTextLen, b->width);
        if (was.compare(0, std::string::npos, d->Buf, d->BufTextLen) != 0) d->BufDirty = true;
    }
    return 0;
}

bool TextBox(const char* id, std::string& text, const char* hint = nullptr) {
    static std::map<ImGuiID, std::string> bufs;
    const ImGuiID key = ImGui::GetID(id);
    std::string& b = bufs[key];
    const ImGuiStyle& st = ImGui::GetStyle();
    const float width = std::max(ui::Dp(80.0f), ImGui::GetContentRegionAvail().x - st.FramePadding.x * 2.0f - ui::Dp(6.0f));
    if (ImGui::GetActiveID() != key) { // (while typing, the box's own copy is the one to show)
        b = text;
        WrapPlace(b.data(), static_cast<int>(b.size()), width);
    }
    const int lines = 1 + static_cast<int>(std::count(b.begin(), b.end(), '\n'));
    const ImVec2 size(-FLT_MIN, std::max(2, lines + 1) * ImGui::GetTextLineHeight() + st.FramePadding.y * 2.0f);
    BoxEdit e{&b, width};
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const bool changed = ImGui::InputTextMultiline(id, b.data(), b.capacity() + 1, size, ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_CallbackAlways, BoxCallback, &e);
    if (text.empty() && b.empty() && hint && !ImGui::IsItemActive())
        ImGui::GetWindowDrawList()->AddText(ImVec2(at.x + st.FramePadding.x, at.y + st.FramePadding.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), hint);
    if (changed) {
        text = b;
        std::replace(text.begin(), text.end(), '\n', ' ');
    }
    return changed;
}

bool Fold(const char* label) { return ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen); }

struct Pick {
    const char* name;
    const char* text; // what the writer gets
    int art;          // DrawMusicArt
};

// Picture cards to choose from (click again to leave it out), as Animate a picture's
bool CardRow(const char* id, const Pick* picks, size_t count, std::string& value) {
    static ImGuiID hovered = 0;
    static double hoverAt = 0;
    const float gap = ui::Dp(6.0f), cw = ui::Dp(100.0f), ch = ui::Dp(62.0f);
    const int cols = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + gap) / (cw + gap)));
    bool changed = false;
    ImGui::PushID(id);
    for (size_t k = 0; k < count; ++k) {
        const Pick& pk = picks[k];
        if (k % cols) ImGui::SameLine(0.0f, gap);
        ImGui::PushID(static_cast<int>(k));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool on = value == pk.text;
        if (ImGui::InvisibleButton("##card", ImVec2(cw, ch))) {
            value = on ? std::string() : std::string(pk.text);
            changed = true;
        }
        const ImGuiID me = ImGui::GetItemID();
        const bool hov = ImGui::IsItemHovered();
        if (hov && hovered != me) hovered = me, hoverAt = ImGui::GetTime();
        else if (!hov && hovered == me) hovered = 0;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 ts = ImGui::CalcTextSize(pk.name), q(p.x + cw, p.y + ch);
        const float strip = ts.y + ui::Dp(5.0f);
        DrawMusicArt(dl, pk.art, p, cw, ch - strip * 0.5f, hov ? static_cast<float>(ImGui::GetTime() - hoverAt) : -1.0f);
        dl->AddRectFilled(ImVec2(p.x, q.y - strip), q, IM_COL32(0, 0, 0, 185));
        const float fit = std::min(1.0f, (cw - ui::Dp(8.0f)) / std::max(1.0f, ts.x));
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * fit, ImVec2(p.x + ui::Dp(4.0f), q.y - ts.y * (1.0f + fit) * 0.5f - ui::Dp(2.5f)), IM_COL32(255, 255, 255, 240), pk.name);
        if (on) dl->AddRect(p, q, ImGui::GetColorU32(ui::kAccent), ui::Dp(4.0f), 0, ui::Dp(2.5f));
        else if (hov) dl->AddRect(p, q, ImGui::GetColorU32(ui::kAccentHover), ui::Dp(4.0f), 0, ui::Dp(1.5f));
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", pk.text);
        ImGui::PopID();
    }
    ImGui::PopID();
    return changed;
}

void Thumb(AppState& app, const std::wstring& path, ImVec2 size) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), ui::ThumbPlaceholder(), ui::Dp(5.0f));
    const AppState::LibThumb lt = path.empty() ? AppState::LibThumb{} : app.LibraryThumb(path);
    if (lt.srv && lt.w > 0 && lt.h > 0) {
        const float s = std::min(size.x / lt.w, size.y / lt.h);
        const ImVec2 a(p.x + (size.x - lt.w * s) * 0.5f, p.y + (size.y - lt.h * s) * 0.5f);
        dl->AddImage((ImTextureID)(intptr_t)lt.srv, a, ImVec2(a.x + lt.w * s, a.y + lt.h * s));
    }
}

void SetSong(const std::wstring& song) {
    if (_wcsicmp(song.c_str(), g_p.song.c_str()) == 0) return;
    g_p.song = song;
    g_p.from = g_p.to = 0;
    g_p.clips.clear();
    g_p.finalVideo.clear();
    g_p.captioned.clear();
    g_p.title = Utf8(fs::path(song).stem().wstring());
    for (const MusicSongInfo& s : MusicSongs())
        if (_wcsicmp(s.path.c_str(), song.c_str()) == 0 && !s.title.empty()) g_p.title = s.title;
    g_song = SongFacts();
    g_songFut = std::async(std::launch::async, ReadSong, song);
    Changed();
}

// The song made from what is on this page: who is in it and what they do, the idea, the place, the look, the genre
std::string SongIdea() {
    std::string s = g_p.genre.empty() ? "A song for a music video." : "A " + g_p.genre + " song for a music video.";
    // (2026-10-30: two singers came out as one man's voice: the writer was told what each does, never that they share the song)
    std::string singers;
    int voices = 0;
    for (size_t k = 0; k < g_p.cast.size(); ++k)
        if (Vocal(RoleOf(k))) singers += (voices++ ? " and " : "") + g_p.cast[k].name;
    if (voices > 1 && !g_p.songNoVoice)
        // Tested 2026-10-30 a cappella, so the pitch could be measured, about 40 takes: a section tag of [male vocal] / [female vocal] plus the caption's casting brought the second
        // voice in the most, with nothing sung by mistake; plain tags alone stayed on one voice, [singer a] got sung ("Singer up"), a higher ar_cfg_scale only made it worse.
        // It is not a switch: about half the takes follow it well.
        s += " The vocals are a DUET of " + std::to_string(voices) + " different people, " + singers + ", one voice for each person in the pictures: a man has a male voice, a woman a female voice. "
             "Vocal Details says: two singers with equal parts, a man and a woman, gives each their own clearly different timbre and delivery (for example a deep gravelly baritone rapper and a bright high soprano "
             "with a smooth melodic delivery), and says they alternate verse by verse and sing every chorus together. Arrangement goes through the song and names who sings each section. In LYRICS, instead of "
             "[verse] each verse starts with the tag [male vocal] or [female vocal], taking turns, each verse about 4 lines, and the choruses are plain [chorus]. Each voice gets about the same number of lines.";
    if (!g_p.idea.empty()) s += " The idea: " + g_p.idea;
    if (!g_p.location.empty()) s += " Set in: " + g_p.location + ".";
    if (!g_p.style.empty()) s += " The mood of the look: " + g_p.style + ".";
    return s;
}

std::string SongPeople() {
    std::string s;
    for (size_t k = 0; k < g_p.cast.size(); ++k) {
        const std::string looks = Looks(g_p.cast[k]);
        s += g_p.cast[k].name + " " + RoleOf(k) + (looks.empty() ? std::string() : " (" + looks + ")") + ". ";
    }
    return s;
}

void DrawMakeSong(AppState& app) {
    const std::string status = MusicForVideoStatus();
    ImGui::BeginDisabled(!status.empty());
    if (ImGui::Button(status.empty() ? "Make a song from all this##mvmakesong" : "Making the song...##mvmakesong")) MusicMakeForVideo(app, SongIdea(), SongPeople(), [] {
        std::vector<std::wstring> pics;
        for (const Member& m : g_p.cast) pics.push_back(m.image);
        return pics;
    }(), g_p.songSeconds, g_p.songNoVoice);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("The writer looks at the pictures and writes the words and the music from who is in it, what they do, the idea, the place, the look and the genre;\n"
                          "MiniMax Music makes it, its lyrics are timed, and it becomes this video's song by itself. It also lands on the Sound page.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ui::Dp(120.0f));
    if (ImGui::SliderInt("##mvsonglen", &g_p.songSeconds, 20, 240, "%d s long")) Changed();
    ImGui::SameLine();
    if (ImGui::Checkbox("No singing##mvsong", &g_p.songNoVoice)) Changed();
    if (!status.empty()) ImGui::TextColored(ui::kAccentHover, "%s", status.c_str());
}

void DrawSong(AppState& app) {
    if (!Fold("The song##mvfold")) return;
    const std::string shown = g_p.song.empty() ? std::string("choose a song") : Utf8(fs::path(g_p.song).filename().wstring());
    ImGui::SetNextItemWidth(ui::Dp(320.0f));
    if (ImGui::BeginCombo("##mvsong", shown.c_str(), ImGuiComboFlags_HeightLarge)) {
        const std::vector<MusicSongInfo> songs = MusicSongs();
        if (songs.empty()) ImGui::TextDisabled("Songs made on the Sound page show here once it has been opened");
        for (const MusicSongInfo& s : songs) {
            const std::string label = (s.title.empty() ? Utf8(fs::path(s.path).filename().wstring()) : s.title) + "  (" + Clock(s.seconds) + ")##" + Utf8(s.path);
            if (ImGui::Selectable(label.c_str(), _wcsicmp(s.path.c_str(), g_p.song.c_str()) == 0)) SetSong(s.path);
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Choose a file...##mvsong")) {
        wchar_t buf[MAX_PATH * 2] = {};
        OPENFILENAMEW ofn = {sizeof(ofn)};
        ofn.hwndOwner = GetActiveWindow();
        ofn.lpstrFilter = L"Songs\0*.mp3;*.wav;*.m4a;*.aac;*.ogg;*.flac;*.opus\0All files\0*.*\0";
        ofn.lpstrFile = buf;
        ofn.nMaxFile = MAX_PATH * 2;
        ofn.lpstrTitle = L"The song of the music video";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&ofn)) SetSong(buf);
    }
    DrawMakeSong(app);
    if (g_p.song.empty()) return;
    if (g_songFut.valid()) {
        ImGui::TextDisabled("Reading the song...");
        return;
    }
    if (g_song.seconds <= 0) {
        ImGui::TextColored(ui::kWarn, "Could not read the song's length (is ffmpeg there?)");
        return;
    }
    std::error_code hec;
    const bool heard = fs::exists(HeardSrtPath(g_p.song), hec) && !fs::exists(SrtPathOf(g_p.song), hec);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s long, %s", Clock(g_song.seconds).c_str(),
                        !g_song.lines.empty() ? (std::to_string(g_song.lines.size()) + (heard ? " lyric lines, heard from the song" : " timed lyric lines") +
                                                 ": the singing follows them").c_str()
                                              : "no timed lyrics with it yet");
    // the words of any song: whisper listens to it (Get the lyrics from the song) - for a song made elsewhere there is nothing to time
    if (g_song.lines.empty() || heard) {
        ImGui::SameLine();
        ImGui::BeginDisabled(g_hearLyrFut.valid());
        if (ImGui::SmallButton(g_hearLyrFut.valid() ? "Listening to the song..." : heard ? "Listen again" : "Get the lyrics from the song"))
            g_hearLyrFut = std::async(std::launch::async, HearSongLyrics, g_p.song, HeardSrtPath(g_p.song));
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Whisper (the speech model the Sound page uses) listens to the song and writes down the words with their times: then the\n"
                              "song gets sections, each clip knows the lines sung in it, and Lyrics as captions works. About a minute for a\n"
                              "3-minute song; fast rap or heavy mixing can come out with a few wrong words. Your song file is not changed.");
        if (!g_hearNote.empty()) ImGui::TextColored(ui::kWarn, "%s", g_hearNote.c_str());
    }
    // the part used: a section (or several in a row), the whole song, or any from / to - a 3-minute song is about 14 clips
    auto setPart = [](double a, double b) {
        g_p.from = std::max(0.0, a);
        g_p.to = b >= g_song.seconds - 0.05 ? 0.0 : b;
        Changed();
    };
    const double end = PartEnd();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Which part:");
    ImGui::SameLine();
    if (Chip("Whole song##mvwhole", g_p.from <= 0.05 && g_p.to <= 0)) setPart(0, g_song.seconds);
    for (size_t k = 0; k < g_song.sections.size(); ++k) {
        const SongSection& s = g_song.sections[k];
        const std::string label = s.name + "  " + Clock(s.start) + "##mvsec" + std::to_string(k);
        ImGui::SameLine(0.0f, ui::Dp(4.0f));
        if (ImGui::GetCursorPosX() + ImGui::CalcTextSize(label.c_str(), nullptr, true).x + ui::Dp(20.0f) > ImGui::GetContentRegionMax().x) ImGui::NewLine();
        const bool in = s.start >= g_p.from - 0.05 && s.end <= end + 0.05 && !(g_p.from <= 0.05 && g_p.to <= 0);
        if (Chip(label.c_str(), in)) {
            if (ImGui::GetIO().KeyShift && !(g_p.from <= 0.05 && g_p.to <= 0)) setPart(std::min(g_p.from, s.start), std::max(end, s.end)); // (more sections in a row)
            else setPart(s.start, s.end);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("%s - %s%s%s\nShift+click: from the part chosen up to this one (several sections in a row)", Clock(s.start).c_str(), Clock(s.end).c_str(),
                              s.firstLine.empty() ? "" : "\n\"", s.firstLine.empty() ? "" : (s.firstLine + "...\"").c_str());
    }
    if (g_song.sections.empty() && !g_song.lines.empty()) ImGui::TextDisabled("(no sections found in its lyrics: use from and to)");
    else if (g_song.lines.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(sections come with the lyrics: Get the lyrics from the song, above; from and to work on any song)");
    }
    // from and to, by hand (Ctrl+click on one to type the seconds)
    float from = static_cast<float>(g_p.from), to = static_cast<float>(end);
    ImGui::SetNextItemWidth(ui::Dp(170.0f));
    const bool a = ImGui::SliderFloat("##mvfrom", &from, 0.0f, static_cast<float>(g_song.seconds) - 3.0f, "from %.1f s");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Where the video starts in the song (Ctrl+click to type it)");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ui::Dp(170.0f));
    const bool b = ImGui::SliderFloat("##mvto", &to, 3.0f, static_cast<float>(g_song.seconds), "to %.1f s");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Where it ends (Ctrl+click to type it)");
    if (a || b) setPart(std::round(from * 10.0f) / 10.0, std::max<double>(std::round(from * 10.0f) / 10.0 + 3.0, std::round(to * 10.0f) / 10.0));
    ImGui::SameLine();
    ImGui::BeginDisabled(g_hearFut.valid());
    if (ImGui::SmallButton(g_hearFut.valid() ? "Cutting...##mvhear" : "Hear it##mvhear"))
        g_hearFut = std::async(std::launch::async, CutSong, g_p.song, g_p.from, PartLength(), std::wstring(L"hear_part.wav"));
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Plays just this part in the player");
    ImGui::SameLine();
    const size_t n = ClipTimes(PartLength(), ClipFrames()).size();
    ImGui::TextDisabled("%s - %s, %s: %d clip%s", Clock(g_p.from).c_str(), Clock(end).c_str(), Clock(PartLength()).c_str(), static_cast<int>(n), n == 1 ? "" : "s");
    // its first and last sung lines, to be sure it is the right part
    const std::vector<TimedLyric> sung = PartLyrics();
    if (!sung.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::kTextDim);
        if (sung.size() == 1) ImGui::TextWrapped("\"%s\"", sung.front().text.c_str());
        else ImGui::TextWrapped("\"%s\" ... \"%s\"  (%d lines)", sung.front().text.c_str(), sung.back().text.c_str(), static_cast<int>(sung.size()));
        ImGui::PopStyleColor();
    }
}

// someone in it, from any picture: a saved character's name, else the file's ("Person 2" for a name like "7007d0afb2693a2e")
void AddMember(const std::wstring& pic) {
    if (pic.empty() || g_p.cast.size() >= 3) return;
    for (const Member& m : g_p.cast)
        if (_wcsicmp(m.image.c_str(), pic.c_str()) == 0) return;
    Member m;
    m.image = pic;
    if (const CharacterProfile* p = FindProfile(pic); p && !p->name.empty()) m.name = p->name;
    if (m.name.empty()) {
        std::string stem = Utf8(fs::path(pic).stem().wstring());
        const int digits = static_cast<int>(std::count_if(stem.begin(), stem.end(), [](unsigned char c) { return std::isdigit(c); }));
        for (char& c : stem) c = c == '_' || c == '-' ? ' ' : c;
        stem = Trim(stem);
        m.name = stem.empty() || stem.size() > 24 || digits * 3 > static_cast<int>(stem.size()) ? "Person " + std::to_string(g_p.cast.size() + 1) : stem;
    }
    g_p.cast.push_back(m);
    Changed();
}

const char* const kRoles[] = {"sings", "raps", "dances", "sings and dances", "plays the guitar", "plays the drums", "plays the piano", "plays the DJ decks",
                              "the love interest", "walks and poses", "hypes up the crowd", "acts out the story"};

void DrawCast(AppState& app) {
    if (!Fold("Who is in it##mvfold")) return;
    const float tw = ui::Dp(54.0f), th = ui::Dp(96.0f), cardW = ui::Dp(250.0f);
    int remove = -1;
    for (size_t k = 0; k < g_p.cast.size(); ++k) {
        Member& m = g_p.cast[k];
        ImGui::PushID(static_cast<int>(k));
        if (k && ImGui::GetContentRegionAvail().x > cardW + ui::Dp(8.0f)) ImGui::SameLine(0.0f, ui::Dp(10.0f));
        ImGui::BeginGroup();
        Thumb(app, m.image, ImVec2(tw, th));
        ImGui::SameLine(0.0f, ui::Dp(4.0f));
        ImGui::BeginGroup();
        if (InputLine("##name", "name", m.name, cardW - tw)) Changed();
        if (InputLine("##role", k == 0 ? "sings" : "dances", m.role, cardW - tw - ImGui::GetFrameHeight() - ui::Dp(4.0f))) Changed();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("What they do: pick one from the list beside it, or type your own (whoever sings or raps mouths the lyrics)");
        ImGui::SameLine(0.0f, ui::Dp(4.0f));
        if (ImGui::ArrowButton("##rolelist", ImGuiDir_Down)) ImGui::OpenPopup("##roles");
        if (ImGui::BeginPopup("##roles")) {
            for (const char* r : kRoles)
                if (ImGui::Selectable(r, m.role == r)) m.role = r, Changed();
            ImGui::EndPopup();
        }
        if (ImGui::SmallButton("remove")) remove = static_cast<int>(k);
        ImGui::EndGroup();
        ImGui::EndGroup();
        ImGui::PopID();
    }
    if (remove >= 0) g_p.cast.erase(g_p.cast.begin() + remove), Changed();
    if (g_p.cast.size() >= 3) return;
    std::vector<const CharacterProfile*> saved;
    for (const CharacterProfile& p : AllProfiles()) {
        if (p.saved && !p.image.empty() && std::none_of(g_p.cast.begin(), g_p.cast.end(), [&](const Member& m) { return _wcsicmp(m.image.c_str(), p.image.c_str()) == 0; }))
            saved.push_back(&p);
    }
    if (!g_p.cast.empty()) ImGui::Spacing();
    // anyone: a picture from the gallery or a file, or one of your saved characters (their description helps the writer)
    if (ImGui::Button("+ A picture from the gallery...##mvcast")) {
        PickPictureFromGallery(app, "Who is in the music video?", [](AppState& a, const std::wstring& pic) {
            AddMember(pic);
            SetVideoMode(3);
            a.requestTab = 10;
        });
    }
    ImGui::SameLine();
    if (ImGui::Button("+ A picture file...##mvcast")) {
        wchar_t buf[MAX_PATH * 2] = {};
        OPENFILENAMEW ofn = {sizeof(ofn)};
        ofn.hwndOwner = GetActiveWindow();
        ofn.lpstrFilter = L"Pictures\0*.png;*.jpg;*.jpeg;*.webp;*.bmp\0All files\0*.*\0";
        ofn.lpstrFile = buf;
        ofn.nMaxFile = MAX_PATH * 2;
        ofn.lpstrTitle = L"Who is in the music video";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&ofn)) AddMember(buf);
    }
    if (!saved.empty()) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ui::Dp(200.0f));
        if (ImGui::BeginCombo("##mvaddcast", "+ One of your characters", ImGuiComboFlags_HeightLarge)) {
            for (const CharacterProfile* p : saved) {
                ImGui::PushID(p->key.c_str());
                const AppState::LibThumb lt = app.LibraryThumb(p->image);
                if (lt.srv && lt.w > 0 && lt.h > 0) {
                    ImGui::Image((ImTextureID)(intptr_t)lt.srv, ImVec2(ui::Dp(22.0f) * lt.w / lt.h, ui::Dp(22.0f)));
                    ImGui::SameLine();
                }
                if (ImGui::Selectable(p->name.empty() ? "(no name)" : p->name.c_str())) AddMember(p->image);
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    }
    if (g_p.cast.empty()) ImGui::TextDisabled("Anyone with a clear picture: a photo, a drawing, a saved character. A full-body picture lets them dance whole.");
}

const Pick kIdeas[] = {
    {"Performance", "A stylish performance video: they sing and dance to the camera in one striking place that slowly changes with the song.", 5},
    {"Dreamlike", "A dreamlike journey: the place keeps transforming around them - rooms open onto oceans, light pours in, the world bends with the music.", 17},
    {"Night city", "A night-time walk through neon-lit city streets, rain on the ground, the camera gliding along as they sing.", 0},
    {"Love story", "A love story told in one take: they meet, drift apart and find each other again as the song builds.", 34},
    {"Concert", "A live concert on a big stage: lights, smoke and a crowd seen from behind, the camera sweeping around the band.", 6},
    {"Road trip", "A road trip with the windows down: they sing to each other, stop by the road, dance on the car's hood as the sun goes down.", 4},
    {"Against the odds", "Starting from nothing, they push through hard times step by step, and end on top of the world as the song peaks.", 9},
    {"Big night out", "Getting ready, the walk in, the dance floor: one wild night that ends watching the sun come up.", 13},
};
const Pick kPlaces[] = {
    {"Neon street", "a neon-lit city street at night, wet ground reflecting the signs", 0},
    {"Rooftop", "a city rooftop at night, skyline and moon behind them", 1},
    {"Beach", "an empty beach at sunset, gentle waves, warm golden light", 2},
    {"Forest", "a deep green forest, sun shafts through the trees", 3},
    {"Desert road", "an endless desert highway under a huge sky", 4},
    {"Big stage", "a huge concert stage with moving lights and haze", 5},
    {"Club", "an underground club with lasers and a mirror ball", 6},
    {"Warehouse", "a bare industrial warehouse, tall windows, shafts of dusty light", 7},
    {"Mansion", "a white marble mansion with columns and a long hall", 8},
    {"Mountain", "a windy mountain top above the clouds", 9},
    {"Space station", "a space station with the Earth turning in the window", 10},
    {"Snowy town", "a snowy little town at dusk, warm windows, falling snow", 11},
};
const Pick kStyles[] = {
    {"Cinematic", "cinematic film, anamorphic lens, shallow focus, rich contrast", 12},
    {"Neon glow", "saturated neon pinks and blues, glowing light on the skin", 13},
    {"Black & white", "high-contrast black and white film", 14},
    {"VHS", "a 90s VHS tape: soft, grainy, faded colour", 15},
    {"Anime", "hand-drawn anime, bold lines, flat colours", 16},
    {"Dreamy", "soft dreamy pastels, hazy light, gentle bloom", 17},
    {"Gritty", "gritty handheld street documentary, harsh light, film grain", 18},
    {"Luxury", "glossy luxury commercial, clean highlights, gold accents", 19},
    {"70s film", "warm 1970s film stock, faded oranges and browns", 20},
    {"Dark horror", "dark horror: deep shadows, blood-red light", 21},
};
const Pick kGenres[] = {
    {"Pop", "pop", 22}, {"Hip hop", "hip hop", 23}, {"Drill", "UK drill rap", 24}, {"R&B", "smooth R&B", 25}, {"Rock", "rock", 26}, {"Metal", "heavy metal", 27},
    {"EDM", "electronic dance music", 28}, {"Latin", "reggaeton and Latin pop", 29}, {"K-pop", "K-pop", 30}, {"Country", "country", 31}, {"Ballad", "a slow piano ballad", 32}, {"Jazz", "smooth jazz", 33},
};

int g_clipsFold = 0; // 1 open every clip, 2 fold every clip (one frame)

void DrawClip(size_t k) {
    Clip& c = g_p.clips[k];
    ImGui::PushID(static_cast<int>(k));
    if (g_clipsFold) ImGui::SetNextItemOpen(g_clipsFold == 1);
    char head[160];
    snprintf(head, sizeof(head), "Clip %d   %s - %s   %s###clip", static_cast<int>(k) + 1, Clock(c.start).c_str(), Clock(c.start + c.frames / 24.0).c_str(), c.who.substr(0, 60).c_str());
    if (ImGui::TreeNodeEx(head, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth)) {
        if (InputLine("##who", "who is in it (empty: everyone)", c.who, ui::Dp(260.0f))) Changed();
        if (!c.lyrics.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ui::kTextDim);
            ImGui::TextWrapped("\"%s\"", c.lyrics.c_str());
            ImGui::PopStyleColor();
        }
        if (TextBox("##action", c.action, "What happens in this clip")) Changed();
        if (!c.moves.empty()) {
            ImGui::TextDisabled("The dance moves of this clip:");
            if (TextBox("##moves", c.moves)) Changed();
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

// tests: MV_MV_SONG (a song file), MV_MV_CAST "picture=name=role|..." (any pictures), MV_MV_IDEA, MV_MV_PART "from,to", MV_MV_RES "0-2",
// MV_MV_SHAPE "0/1"; MV_UI_TEST "mvplan" plans the clips once, "mvmake" presses Make once they are planned, "mvcaption" puts the lyrics on the last video
void TestHooks(AppState& app) {
    static bool done = false, planned = false, made = false;
    wchar_t w[2048] = {};
    if (!done) {
        done = true;
        if (GetEnvironmentVariableW(L"MV_MV_SONG", w, 2048)) SetSong(w);
        if (GetEnvironmentVariableW(L"MV_MV_FINAL", w, 2048)) g_p.finalVideo = w, g_p.captioned.clear(), Changed(); // (a video made already)
        if (GetEnvironmentVariableW(L"MV_MV_CAST", w, 2048)) {
            g_p.cast.clear();
            const std::wstring all = w;
            size_t a = 0;
            while (a < all.size()) {
                const size_t b = std::min(all.find(L'|', a), all.size());
                const std::wstring one = all.substr(a, b - a);
                a = b + 1;
                const size_t e1 = one.find(L'='), e2 = e1 == std::wstring::npos ? std::wstring::npos : one.find(L'=', e1 + 1);
                AddMember(one.substr(0, e1)); // (named as a picture added on the page is, unless a name is given)
                if (g_p.cast.empty()) continue;
                if (e1 != std::wstring::npos) g_p.cast.back().name = Utf8(one.substr(e1 + 1, e2 == std::wstring::npos ? std::wstring::npos : e2 - e1 - 1));
                if (e2 != std::wstring::npos) g_p.cast.back().role = Utf8(one.substr(e2 + 1));
            }
            Changed();
        }
        char buf[2048] = {};
        if (GetEnvironmentVariableA("MV_MV_IDEA", buf, sizeof(buf))) g_p.idea = buf, Changed();
        if (GetEnvironmentVariableA("MV_MV_MODEL", buf, sizeof(buf))) g_p.quality = std::clamp(std::atoi(buf), 0, 5), Changed();
        if (GetEnvironmentVariableA("MV_MV_GENRE", buf, sizeof(buf))) g_p.genre = buf, Changed();
        if (GetEnvironmentVariableA("MV_MV_SONGLEN", buf, sizeof(buf))) g_p.songSeconds = std::atoi(buf), Changed();
        if (GetEnvironmentVariableA("MV_MV_RES", buf, sizeof(buf))) g_p.res = std::clamp(std::atoi(buf), 0, 2), Changed();
        if (GetEnvironmentVariableA("MV_MV_SHAPE", buf, sizeof(buf))) g_p.shape = std::clamp(std::atoi(buf), 0, 1), Changed();
        if (GetEnvironmentVariableA("MV_MV_PART", buf, sizeof(buf))) {
            g_p.from = std::atof(buf);
            const char* comma = strchr(buf, ',');
            g_p.to = comma ? std::atof(comma + 1) : 0.0;
            Changed();
        }
    }
    static const std::string tests = [] {
        char b[256] = {};
        return GetEnvironmentVariableA("MV_UI_TEST", b, sizeof(b)) ? std::string(b) : std::string();
    }();
    if (!planned && tests.find("mvplan") != std::string::npos && !g_songFut.valid() && g_song.seconds > 0 && !g_p.cast.empty()) planned = true, StartPlan(app);
    if (!made && tests.find("mvmake") != std::string::npos && !g_planFut.valid() && !g_songFut.valid() && PlanFits() && app.comfyLoaded &&
        (planned || tests.find("mvplan") == std::string::npos))
        made = true, StartMake(app);
    static bool songMade = false; // "mvsong": Make a song, as its button does
    if (!songMade && tests.find("mvsong") != std::string::npos && app.comfyLoaded) {
        songMade = true;
        std::vector<std::wstring> pics;
        for (const Member& m : g_p.cast) pics.push_back(m.image);
        MusicMakeForVideo(app, SongIdea(), SongPeople(), pics, g_p.songSeconds, g_p.songNoVoice);
    }
    static bool heard = false; // "mvhear": Get the lyrics from the song, once the song is read
    if (!heard && tests.find("mvhear") != std::string::npos && !g_songFut.valid() && g_song.seconds > 0)
        heard = true, g_hearLyrFut = std::async(std::launch::async, HearSongLyrics, g_p.song, HeardSrtPath(g_p.song));
    static bool captioned = false; // "mvcaption": the lyrics put on the video made last
    if (!captioned && tests.find("mvcaption") != std::string::npos && !g_songFut.valid() && !g_p.finalVideo.empty() && CanCaption())
        captioned = true, g_p.captions = true, StartCaptions();
}

} // namespace

void MusicVideoSongMoved(const std::wstring& from, const std::wstring& to) {
    std::error_code ec;
    if (fs::exists(HeardSrtPath(from), ec)) fs::rename(HeardSrtPath(from), HeardSrtPath(to), ec); // (the lyrics heard from it are kept by its path)
    if (!g_loaded) Load();
    if (_wcsicmp(from.c_str(), g_p.song.c_str()) != 0) return;
    g_p.song = to;
    g_song.path = to;
    Changed();
}

void OpenMusicVideo(AppState& app, const std::wstring& song) {
    if (!g_loaded) Load();
    SetSong(song);
    SetVideoMode(3);
    app.requestTab = 10;
}

void PumpMusicVideo(AppState& app) {
    if (!g_loaded) Load();
    TestHooks(app);
    if (g_songFut.valid() && g_songFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        SongFacts s = g_songFut.get();
        if (_wcsicmp(s.path.c_str(), g_p.song.c_str()) == 0) g_song = std::move(s);
    }
    if (g_planFut.valid() && g_planFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        PlanResult r = g_planFut.get();
        if (!r.error.empty()) {
            Toast(ToastKind::Error, "Could not plan the music video", r.error, {}, [](AppState& s) { s.requestTab = 10; }, "musicvideo");
            g_makeAfterPlan = false;
        } else {
            for (size_t k = 0; k < g_p.clips.size() && k < r.clips.size(); ++k) g_p.clips[k].who = r.clips[k].first, g_p.clips[k].action = r.clips[k].second;
            if (!r.look.empty()) g_p.look = r.look;
            if (!g_makeAfterPlan)
                Toast(ToastKind::Success, "The music video is planned", std::to_string(g_p.clips.size()) + " clips. Check them, then Make.", {},
                      [](AppState& s) { s.requestTab = 10; }, "musicvideo");
        }
        Changed();
        if (g_makeAfterPlan && r.error.empty()) g_makeAfterPlan = false, StartMake(app); // (Make: planned, then straight on)
    }
    if (g_run) {
        if (g_run->cut.valid() && g_run->cut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            const std::wstring part = g_run->cut.get();
            if (part.empty()) {
                Toast(ToastKind::Error, "Could not make the music video", "The part of the song could not be cut (is ffmpeg there?)", {}, {}, "musicvideo");
                g_run.reset();
            } else if (g_run->ltx) {
                g_run->part = part;
                StartLtxScene(app);
                if (!g_run->scene) g_run.reset();
            } else {
                ComfyJob job;
                job.apiWorkflow = g_run->graph.dump();
                {
                    std::ofstream dump(DataDir() + L"\\music_video_last.json", std::ios::trunc);
                    dump << g_run->graph.dump(1);
                }
                job.rawUploads = {{"@song", part}};
                for (size_t k = 0; k < g_p.cast.size(); ++k) job.rawUploads.push_back({"@pic" + std::to_string(k + 1), g_p.cast[k].image});
                job.label = g_run->label;
                job.times = 1;
                job.seed = 42;
                const int n = static_cast<int>(g_p.clips.size());
                for (int k = 0; k < n; ++k)
                    job.stages.push_back("Clip " + std::to_string(k + 1) + " of " + std::to_string(n) + " (" + Clock(g_p.clips[k].start) + " - " + Clock(g_p.clips[k].start + g_p.clips[k].frames / 24.0) + ")");
                job.finishing = "Decoding the clips and joining them under the song";
                g_run->activity = SendComfyJob(app, std::move(job));
                if (!g_run->activity) g_run.reset();
            }
        } else if (g_run->scene && !g_run->activity && app.comfySvc) { // LTX-2.5's first picture
            for (const ComfyActivity& a : app.comfySvc->Activity()) {
                if (a.id != g_run->scene) continue;
                if (a.state == ComfyState::Done || a.state == ComfyState::Failed) {
                    std::wstring first = g_p.cast.front().image; // (without it, the first person's own picture)
                    for (const ComfyResult& r : a.files)
                        if (!r.video && !r.audio && !r.model3d && !r.localPath.empty()) first = r.localPath;
                    SendLtxClips(app, first);
                }
                break;
            }
        } else if (g_run->join.valid()) {
            if (g_run->join.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                g_p.finalVideo = g_run->join.get();
                if (g_p.finalVideo.empty()) Toast(ToastKind::Error, "Could not join the clips", "They are in ComfyUI's output folder (MusicLTX_...)", {}, {}, "musicvideo");
                else if (g_p.captions && CanCaption()) StartCaptions();
                else {
                    Toast(ToastKind::Success, "The music video is ready: " + g_p.title, Utf8(fs::path(g_p.finalVideo).filename().wstring()), {}, [](AppState& s) { s.requestTab = 10; }, "musicvideo");
                    PlayInVideoPage(app, g_p.finalVideo, false);
                }
                Changed();
                g_run.reset();
            }
        } else if (g_run->activity && app.comfySvc) {
            for (const ComfyActivity& a : app.comfySvc->Activity()) {
                if (a.id != g_run->activity) continue;
                if (a.state == ComfyState::Failed) {
                    g_run.reset();
                } else if (a.state == ComfyState::Done && g_run->ltx) {
                    std::vector<std::wstring> parts;
                    for (const ComfyResult& f : a.files)
                        if (!f.localPath.empty() && f.video) parts.push_back(f.localPath);
                    std::sort(parts.begin(), parts.end());
                    if (parts.empty()) {
                        Toast(ToastKind::Error, "Could not make the music video", "ComfyUI sent no clips back", {}, {}, "musicvideo");
                        g_run.reset();
                    } else {
                        const std::wstring out = (fs::path(parts.front()).parent_path() / (L"MusicVideo_" + Wide(SafeName(g_p.title)) + L"_" + std::to_wstring(g_run->seed) + L".mp4")).wstring();
                        g_run->join = std::async(std::launch::async, JoinClips, parts, g_run->part, PartLength(), out);
                    }
                } else if (a.state == ComfyState::Done) {
                    for (const ComfyResult& f : a.files)
                        if (!f.localPath.empty() && f.video && (g_p.finalVideo.empty() || f.localPath.find(L"-audio") != std::wstring::npos)) g_p.finalVideo = f.localPath;
                    if (!g_p.finalVideo.empty() && g_p.captions && CanCaption()) {
                        StartCaptions(); // (announced once the lyrics are on it)
                    } else if (!g_p.finalVideo.empty()) {
                        Toast(ToastKind::Success, "The music video is ready: " + g_p.title, Utf8(fs::path(g_p.finalVideo).filename().wstring()), {},
                              [](AppState& s) { s.requestTab = 10; }, "musicvideo");
                        PlayInVideoPage(app, g_p.finalVideo, false);
                    }
                    Changed();
                    g_run.reset();
                }
                break;
            }
        }
    }
    if (const std::wstring song = MusicForVideoTake(); !song.empty()) { // Make a song is done: it is this video's song
        SetSong(song);
        Toast(ToastKind::Success, "The song is ready", g_p.title + ": it is this music video's song now", {}, [](AppState& s) { s.requestTab = 10; }, "musicvideo");
    }
    if (const std::string why = MusicForVideoFailed(); !why.empty()) Toast(ToastKind::Error, "Could not make the song", why, {}, [](AppState& s) { s.requestTab = 10; }, "musicvideo");
    if (g_hearLyrFut.valid() && g_hearLyrFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const std::string why = g_hearLyrFut.get();
        g_hearNote = why.empty() ? std::string() : "Could not get the lyrics: " + why;
        if (why.empty()) {
            g_songFut = std::async(std::launch::async, ReadSong, g_p.song); // (its lines, sections and captions now)
            Toast(ToastKind::Success, "The lyrics are in", "Heard from the song: its sections, the lines of each clip and the captions use them.", {},
                  [](AppState& s) { s.requestTab = 10; }, "musicvideo");
        }
    }
    if (g_hearFut.valid() && g_hearFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const std::wstring part = g_hearFut.get();
        if (!part.empty()) PlayInVideoPage(app, part, true);
        else Toast(ToastKind::Error, "Could not cut that part", "Is ffmpeg there?", {}, {}, "musicvideo");
    }
    if (g_capFut.valid() && g_capFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const auto [ok, msg] = g_capFut.get();
        if (ok) {
            g_p.captioned = Wide(msg);
            Toast(ToastKind::Success, "The music video is ready: " + g_p.title, "with the lyrics as captions", {}, [](AppState& s) { s.requestTab = 10; }, "musicvideo");
            PlayInVideoPage(app, g_p.captioned, false);
        } else {
            Toast(ToastKind::Error, "Could not add the lyrics", msg + "\nThe video without them is there.", {}, {}, "musicvideo");
            if (!g_p.finalVideo.empty()) PlayInVideoPage(app, g_p.finalVideo, false);
        }
        Changed();
    }
    if (g_dirty && ImGui::GetTime() - g_dirtyAt > 1.0) Save();
}

void DrawMusicVideoMode(AppState& app, float footerH) {
    (void)footerH;
    if (!g_loaded) Load();
    ImGui::PushStyleColor(ImGuiCol_Text, ui::kTextDim);
    ImGui::TextWrapped("A song (or a part of it) and anyone in a picture become one connected video: the writer plans the clips on the song's lyrics, and each clip "
                       "hears its slice of the real song (the singing follows it) and carries straight on from the one before. The song itself "
                       "is the video's sound, unchanged.");
    ImGui::PopStyleColor();

    if (!PackThere()) {
        ImGui::Spacing();
        ImGui::TextColored(ui::kWarn, "It needs the music video nodes in ComfyUI (ComfyUI-H3-Motion-Context-MultiRef by seitanism, GPL-3.0).");
        ImGui::BeginDisabled(g_packBusy || ComfyInstallDir().empty());
        if (ImGui::Button(g_packBusy ? "Getting the nodes..." : "Get the nodes (about 0.5 MB)")) InstallPack();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("From github.com/seitanism/ComfyUI-H3-Motion-Context-MultiRef (the version tested) into ComfyUI's custom_nodes; then restart ComfyUI once");
    }
    {
        std::lock_guard<std::mutex> lock(g_packMutex);
        if (!g_packNote.empty()) ImGui::TextColored(ui::kAccentHover, "%s", g_packNote.c_str());
    }

    DrawSong(app);
    DrawCast(app);

    if (Fold("The idea##mvfold")) {
        if (TextBox("##mvidea", g_p.idea, "What it looks like and what happens. Empty: the writer makes one up from the song.")) Changed();
        ImGui::Spacing();
        if (CardRow("ideas", kIdeas, std::size(kIdeas), g_p.idea)) Changed();
    }
    if (Fold("Place, look and genre##mvfold")) {
        ImGui::TextDisabled("Where it happens");
        if (InputLine("##mvplace", "pick a card, or type your own", g_p.location, -FLT_MIN)) Changed();
        if (CardRow("places", kPlaces, std::size(kPlaces), g_p.location)) Changed();
        ImGui::Spacing();
        ImGui::TextDisabled("The look");
        if (InputLine("##mvstyle", "pick a card, or type your own", g_p.style, -FLT_MIN)) Changed();
        if (CardRow("styles", kStyles, std::size(kStyles), g_p.style)) Changed();
        ImGui::Spacing();
        ImGui::TextDisabled("The genre (the performance suits it, and Make a song makes this kind of song)");
        if (InputLine("##mvgenre", "pick a card, or type your own", g_p.genre, -FLT_MIN)) Changed();
        if (CardRow("genres", kGenres, std::size(kGenres), g_p.genre)) Changed();
    }
    ImGui::Spacing();
    const bool planning = g_planFut.valid();
    ImGui::BeginDisabled(planning || g_p.cast.empty() || g_song.seconds <= 0 || g_run != nullptr);
    if (ImGui::Button(g_p.clips.empty() ? "Plan the clips" : "Plan the clips again")) StartPlan(app);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(g_p.cast.empty() ? "Add who is in it first" : "The prompt writer: who is in each clip and what happens, on the lyrics sung in it. Nothing is made yet.");
    const std::string st = Status();
    if (planning || !st.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ui::kAccentHover, "%s", st.empty() ? "Planning..." : st.c_str());
    }

    if (!g_p.clips.empty()) {
        char head[64];
        snprintf(head, sizeof(head), "Clips (%d)###mvclips", static_cast<int>(g_p.clips.size()));
        if (Fold(head)) {
            if (!PlanFits()) ImGui::TextColored(ui::kWarn, "The song, its part, the size or the model changed: plan the clips again (Make does it by itself).");
            ImGui::TextDisabled("The look of the whole video:");
            if (TextBox("##mvlook", g_p.look)) Changed();
            if (ImGui::SmallButton("Open them all")) g_clipsFold = 1;
            ImGui::SameLine();
            if (ImGui::SmallButton("Fold them all")) g_clipsFold = 2;
            for (size_t k = 0; k < g_p.clips.size(); ++k) DrawClip(k);
            g_clipsFold = 0;
        }
    }

    const bool canCaption = CanCaption();
    if (Fold("The video##mvfold")) {
        // the model: H3's reference ways keep the faces from the pictures; LTX-2.5 starts on one picture of everyone
        struct Model {
            const char* name;
            const char* about;
        };
        static const Model kModels[] = {
            {"Fast", "H3 with the 4-step reference turbo: about 2 minutes a 15 s clip at 544p with Veda"},
            {"Best", "H3 alone (12 steps on PRISM): about 3x the time"},
            {"Steady", "H3 with the 8-step reference turbo: about 2x Fast's time, steadier faces and motion"},
            {"Omni", "FastH3-Omni, 8 steps: about 2x Fast's time. True to a still picture, but in a dance test (2026-10-30) the singer turned into a copy of the dancer: check the people stay themselves"},
            {"Pika", "Pika H3 Flex's reference LoRA, 6 steps: about 1.5x Fast's time"},
            {"LTX-2.5", "Lightricks' LTX-2.5: the quickest. It has no reference pictures, so everyone is first drawn into one picture in the place, and each clip goes on from "
                        "the last frame of the one before. It hears the song and the joins are clean, but clothes and faces drift more than on H3, above all when the camera comes in close."},
        };
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Model:");
        for (int m : {0, 2, 3, 4, 1, 5}) {
            ImGui::SameLine(0.0f, ui::Dp(4.0f));
            const bool have = VideoRefHas(m);
            ImGui::BeginDisabled(!have);
            char lbl[32];
            snprintf(lbl, sizeof(lbl), "%s##mvm%d", kModels[m].name, m);
            if (Chip(lbl, g_p.quality == m)) g_p.quality = m, Changed();
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s%s", kModels[m].about, have ? "" : "\n(its files are not in ComfyUI's models folders)");
        }
        if (!VideoRefHas(g_p.quality)) ImGui::TextColored(ui::kWarn, "%s's files are not in ComfyUI's models folders: it would make this with Fast.", kModels[g_p.quality].name);
        if (Ltx()) {
            ImGui::TextDisabled("Uses: LTX-2.5, 2 passes a clip, starting from one picture of everyone (Veda is for H3)");
        } else {
            ImGui::TextDisabled("Uses: %s", VideoRefStack(g_p.quality).about.c_str());
            ImGui::SameLine();
            ImGui::BeginDisabled(!VideoVedaCan());
            bool veda = VideoVedaOn() && VideoVedaCan();
            if (ImGui::Checkbox("Veda##mvveda", &veda)) SetVideoVeda(veda);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(VideoVedaCan() ? "Sparse attention: sampling about 2.8x quicker. The same switch as Animate a picture's" : "Not set up yet: Animate a picture has its Set up button");
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Size:");
        for (int r = 0; r < 3; ++r) {
            ImGui::SameLine(0.0f, ui::Dp(4.0f));
            char lbl[24];
            snprintf(lbl, sizeof(lbl), "%s##mvr%d", kRes[r].name, r);
            if (Chip(lbl, g_p.res == r)) g_p.res = r, Changed();
            if (r == 2 && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("10 s clips instead of 15 (memory): more of them");
        }
        ImGui::SameLine(0.0f, ui::Dp(16.0f));
        if (Chip("Portrait 9:16##mvs0", g_p.shape == 0)) g_p.shape = 0, Changed();
        ImGui::SameLine(0.0f, ui::Dp(4.0f));
        if (Chip("Landscape 16:9##mvs1", g_p.shape == 1)) g_p.shape = 1, Changed();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Clips:");
        ImGui::SameLine(0.0f, ui::Dp(4.0f));
        if (Chip("15 s##mvl0", g_p.clipLen == 0 && g_p.res != 2)) g_p.clipLen = 0, Changed();
        ImGui::SameLine(0.0f, ui::Dp(4.0f));
        if (Chip("10 s##mvl1", g_p.clipLen == 1 || g_p.res == 2)) g_p.clipLen = 1, Changed();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Shorter clips: more changes along the song, a little more time");
        ImGui::SameLine(0.0f, ui::Dp(16.0f));
        ImGui::SetNextItemWidth(ui::Dp(130.0f));
        if (ImGui::InputInt("Seed##mvseed", &g_p.seed, 0, 0)) g_p.seed = std::max(0, g_p.seed), Changed();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("0: new every time. Any other number: the same noise and dance moves again");
        ImGui::SameLine(0.0f, ui::Dp(16.0f));
    ImGui::BeginDisabled(!canCaption);
    bool caps = g_p.captions && canCaption;
    if (ImGui::Checkbox("Lyrics as captions", &caps)) g_p.captions = caps, Changed();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(canCaption ? "Each line of the lyrics on screen while it is sung, so the words can be followed.\n"
                                       "It goes on a copy: the video without them is kept, and this can be switched any time."
                                     : "It needs the song's timed lyrics: press Get the lyrics from the song (under the song, at the top)");
    }
    if (!g_p.finalVideo.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ui::kGood, "The video:");
        ImGui::SameLine();
        ImGui::TextUnformatted(Utf8(fs::path(ShownVideo()).filename().wstring()).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Play##mvfinal")) PlayInVideoPage(app, ShownVideo());
        ImGui::SameLine();
        if (ImGui::SmallButton("Show in Explorer##mvfinal")) {
            const std::wstring arg = L"/select,\"" + ShownVideo() + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
        }
        if (g_capFut.valid()) {
            ImGui::SameLine();
            ImGui::TextColored(ui::kAccentHover, "Putting the lyrics on it...");
        } else if (g_p.captions && canCaption && g_p.captioned.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Put the lyrics on it##mvcap")) StartCaptions();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("A copy with the lyrics as captions (about as long as the video plays; nothing is made again)");
        }
    }
    ImGui::EndChild(); // the form

    // the footer: make it
    ImGui::Separator();
    // Make plans the clips first when they are not planned (or were for another part or size), then makes the video straight on
    const bool haveSong = g_song.seconds > 0 && !g_songFut.valid();
    const bool ready = haveSong && !g_p.cast.empty() && !planning && !g_makeAfterPlan && PackThere();
    if (!g_run) {
        ImGui::BeginDisabled(!ready);
        if (ui::PrimaryButton(g_makeAfterPlan ? "Planning, then making..." : "Make the music video", ImVec2(ui::Dp(210.0f), ui::Dp(38.0f)))) {
            if (PlanFits()) StartMake(app);
            else g_makeAfterPlan = true, StartPlan(app);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(!PackThere() ? "Get the music video nodes first (at the top of the page)"
                              : !haveSong   ? "Choose the song first"
                              : g_p.cast.empty() ? "Add who is in it"
                              : PlanFits() ? "Makes it from the clips below"
                                           : "Plans the clips (the prompt writer) and makes the video straight on. To check the plan first: Plan the clips");
    } else {
        ImGui::BeginDisabled();
        ui::PrimaryButton("Making it...", ImVec2(ui::Dp(210.0f), ui::Dp(38.0f)));
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    int w = 0, h = 0;
    VideoSize(w, h);
    if (g_run) {
        std::string msg = g_run->join.valid() ? "Joining the clips under the song..." : g_run->activity ? "In ComfyUI's queue" : g_run->scene ? "Drawing everyone in the place" : "Cutting the song...";
        bool shown = false;
        if ((g_run->activity || g_run->scene) && app.comfySvc)
            for (const ComfyActivity& a : app.comfySvc->Activity())
                if (a.id == (g_run->activity ? g_run->activity : g_run->scene) && a.state != ComfyState::Done && a.state != ComfyState::Failed) {
                    if (a.state == ComfyState::Running) DrawComfyJobProgress(a), shown = true;
                    else if (!a.message.empty()) msg = a.message;
                }
        if (!shown) ImGui::TextUnformatted(msg.substr(0, 90).c_str());
    } else if (g_capFut.valid()) {
        ImGui::TextUnformatted("Putting the lyrics on it...");
    } else {
        const int n = static_cast<int>(PlanFits() ? g_p.clips.size() : ClipTimes(PartLength(), ClipFrames()).size());
        ImGui::Text("%d clip%s, %s of video%s", n, n == 1 ? "" : "s", Clock(PartLength()).c_str(), PlanFits() ? "" : " (planned when you press Make)");
    }
    if (PartLength() > 0) ImGui::TextDisabled("about %d minutes, %d x %d", static_cast<int>(std::ceil(EstimateSeconds() / 60.0)), w, h);
    else ImGui::TextDisabled("%d x %d", w, h);
    ImGui::EndGroup();
}
