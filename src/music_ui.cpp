// The Music tab: songs and soundtracks with MiniMax Music 3, run by ComfyUI itself (Comfy-Org's files, the workflow of ComfyUI's own template).
// Two things drive it: the CAPTION (Global Metadata: genre, tempo, key, mood; Vocal Details; Arrangement) and the LYRICS, whose section tags
// ([intro] [verse] [chorus] [bridge] [instrumental] [outro]) are what shapes the song. The prompt writer can write both from an idea.
// Songs land in ComfyUI's output folder (audio/), play here, and one can go under a story (Video page > Make a story) as its soundtrack.
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <ctime>
#include <functional>
#include <map>
#include <set>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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
#include "video_player.h"
#include "video_util.h"
#include "vision_prompt.h"
#include "music_lyrics.h"
#include "cover_templates.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

const char* const kDit = "minimax_music3_dit_int8_convrot.safetensors";   // the low-VRAM one (2.5 GB)
const char* const kDitFp16 = "minimax_music3_dit_fp16.safetensors";       // used instead when it is there (4.9 GB)
const char* const kTextEncoder = "minimax_music3_text_encoder_pruned_int8_convrot.safetensors";
const char* const kVae = "minimax_music3_dav.safetensors";
const char* const kRepo = "https://huggingface.co/Comfy-Org/MiniMax-Music-3/resolve/main/";
struct ModelFile {
    const char* sub;
    const char* file;
    double gb;
};
const ModelFile kFiles[] = {{"vae", kVae, 0.2}, {"diffusion_models", kDit, 2.5}, {"text_encoders", kTextEncoder, 9.2}};

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
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

// ---- the page's state
struct Song {
    std::wstring path;
    std::string name;
    fs::file_time_type time;
    std::string title; // its title tag ("" = none: the file name is shown)
    double seconds = 0;
};

// the cover a song gets: designed from its picture (a PhotoCraft template), its picture as it is, drawn by Qwen, or none
enum CoverMode { kCoverDesign = 0, kCoverPicture = 1, kCoverAi = 2, kCoverNone = 3 };

struct Written { // what the writer came back with
    std::string title, caption, lyrics, cover, error;
};

// a song being made with its cover: when both are back, the cover goes into the song's file
struct PendingCover {
    int songAct = 0, coverAct = 0; // coverAct: Qwen drawing it (0: not)
    std::shared_future<std::wstring> coverFile; // or made here (a template, the picture squared): its file, "" = it failed
    std::string title, lyrics;     // for the song's file: its title and the lyrics (timed to the singing there)
    std::string caption;           // what was sent (the same song is not sent twice while it is being made)
    bool forVideo = false;         // for Make a music video: the file goes to that page when it is done
    std::wstring cover;
    bool coverDone = false;
};

struct Music {
    bool loaded = false;
    std::string idea, caption, lyrics;
    std::vector<std::wstring> photos; // pictures the song is about (the writer looks at them; the cover is made from them)
    std::string photoContext;     // what they show, in words (their prompts, a Vision board): for a writer that cannot see, and as more to go on
    std::string title;            // the song's title (written with it)
    bool forVideo = false;        // the next Make is for Make a music video
    std::string cover;            // what the cover picture shows (written with it)
    int coverMode = kCoverDesign; // the cover every song gets, put into the song's file
    int coverTemplate = 0;        // the design (cover_templates)
    int coverPhoto = 0;           // which of the pictures the cover is made from
    // the designs drawn small with this picture and title, to choose from
    std::string previewKey;
    std::wstring previewStem;     // ...\cover_previews\<key hash>_ (+ n.png)
    std::shared_ptr<std::atomic<int>> previewDone; // how many are drawn
    std::future<void> previewFut;
    double previewAsked = -1;     // when the picture or title last changed (drawn once they stay a moment)
    // the player
    float volume = 0.9f;
    int repeat = 0;               // 0 off, 1 all, 2 this one
    bool shuffle = false;
    bool wasPlaying = false;
    std::wstring selected;        // the song shown at the top
    std::vector<std::pair<std::string, std::wstring>> covers; // song file name -> its cover picture
    std::vector<PendingCover> pending;
    std::future<std::string> postFut; // a video for posting being made (its path, or "error: ...")
    bool instrumental = false;
    double seconds = 60;
    int steps = 30;
    int times = 1;
    bool edited = false;          // caption / lyrics typed by you: kept when the idea changes
    std::string writtenFor;       // the idea + length they were written for
    // the model files (looked at again now and then)
    bool known = false, have = false;
    std::string dit;
    double checked = -1000;
    // the download (started by your click)
    std::atomic<bool> downloading{false}, failed{false};
    std::wstring partDir;
    std::string downloadNote;
    // the writer
    std::future<Written> writeFut;
    std::string writeNote;
    bool makeAfterWrite = false;
    // the songs
    std::vector<Song> songs;
    std::future<std::vector<Song>> scanFut;
    double scanned = -1000;
    int doneSeen = -1;
    std::unique_ptr<VideoPlayer> player;
    std::wstring playing;
};
Music g_m;

std::wstring SettingsPath() { return DataDir() + L"\\music.json"; }

void Load() {
    g_m.loaded = true;
    std::ifstream f(SettingsPath(), std::ios::binary);
    json j = f ? json::parse(f, nullptr, false) : json();
    if (!j.is_object()) return;
    g_m.idea = j.value("idea", std::string());
    if (j.contains("photos") && j["photos"].is_array()) {
        for (const json& p : j["photos"]) {
            if (p.is_string()) g_m.photos.push_back(Wide(p.get<std::string>()));
        }
    } else if (!j.value("photo", std::string()).empty()) {
        g_m.photos.push_back(Wide(j.value("photo", std::string())));
    }
    g_m.photoContext = j.value("photoContext", std::string());
    g_m.title = j.value("title", std::string());
    g_m.cover = j.value("cover", std::string());
    g_m.coverMode = std::clamp(j.value("coverMode", j.value("makeCover", true) ? static_cast<int>(kCoverDesign) : static_cast<int>(kCoverNone)), 0, 3);
    g_m.coverTemplate = std::clamp(j.value("coverTemplate", 0), 0, CoverTemplateCount() - 1);
    g_m.coverPhoto = std::max(0, j.value("coverPhoto", 0));
    g_m.volume = std::clamp(j.value("volume", 0.9f), 0.0f, 1.0f);
    g_m.repeat = std::clamp(j.value("repeat", 0), 0, 2);
    g_m.shuffle = j.value("shuffle", false);
    if (j.contains("covers") && j["covers"].is_object()) {
        for (const auto& [k, v] : j["covers"].items()) {
            if (v.is_string()) g_m.covers.emplace_back(k, Wide(v.get<std::string>()));
        }
    }
    g_m.caption = j.value("caption", std::string());
    g_m.lyrics = j.value("lyrics", std::string());
    g_m.instrumental = j.value("instrumental", false);
    g_m.seconds = std::clamp(j.value("seconds", 60.0), 10.0, 300.0);
    g_m.steps = std::clamp(j.value("steps", 30), 10, 60);
    g_m.times = std::clamp(j.value("times", 1), 1, 4);
    g_m.edited = j.value("edited", false);
    g_m.writtenFor = j.value("writtenFor", std::string());
}

void Save() {
    std::error_code ec;
    fs::create_directories(DataDir(), ec);
    std::ofstream f(SettingsPath(), std::ios::binary | std::ios::trunc);
    if (!f) return;
    json photos = json::array(), covers = json::object();
    for (const std::wstring& p : g_m.photos) photos.push_back(Utf8(p));
    for (const auto& [song, cover] : g_m.covers) covers[song] = Utf8(cover);
    f << json({{"idea", g_m.idea}, {"photos", photos}, {"photoContext", g_m.photoContext}, {"title", g_m.title}, {"cover", g_m.cover}, {"coverMode", g_m.coverMode},
               {"coverTemplate", g_m.coverTemplate}, {"coverPhoto", g_m.coverPhoto}, {"volume", g_m.volume}, {"repeat", g_m.repeat}, {"shuffle", g_m.shuffle},
               {"covers", covers}, {"caption", g_m.caption}, {"lyrics", g_m.lyrics}, {"instrumental", g_m.instrumental}, {"seconds", g_m.seconds},
               {"steps", g_m.steps}, {"times", g_m.times}, {"edited", g_m.edited}, {"writtenFor", g_m.writtenFor}})
             .dump(1, ' ', false, json::error_handler_t::replace);
}

std::string WriteKey() {
    std::string k = g_m.idea + "|" + std::to_string(static_cast<int>(g_m.seconds)) + "|" + (g_m.instrumental ? "i" : "v");
    for (const std::wstring& p : g_m.photos) k += "|" + Utf8(p);
    return k;
}

std::wstring CoverOf(const std::string& songName) {
    for (const auto& [song, cover] : g_m.covers) {
        if (song == songName) return cover;
    }
    return {};
}

void CheckModel() {
    const std::wstring models = ComfyModelsDir();
    g_m.known = !models.empty();
    if (!g_m.known) return;
    std::error_code ec;
    auto has = [&](const char* sub, const char* f) { return fs::exists(fs::path(models) / sub / f, ec); };
    g_m.dit = has("diffusion_models", kDitFp16) ? kDitFp16 : has("diffusion_models", kDit) ? kDit : "";
    g_m.have = !g_m.dit.empty() && has("text_encoders", kTextEncoder) && has("vae", kVae);
}

void StartDownload() {
    const std::wstring models = ComfyModelsDir();
    if (g_m.downloading || models.empty()) return;
    g_m.downloading = true;
    g_m.failed = false;
    g_m.partDir = models;
    std::thread([models]() {
        bool ok = true;
        for (const ModelFile& m : kFiles) {
            const std::wstring dest = models + L"\\" + Wide(m.sub) + L"\\" + Wide(m.file);
            std::error_code ec;
            if (fs::exists(dest, ec)) continue;
            fs::create_directories(fs::path(dest).parent_path(), ec);
            std::string out;
            RunCaptureHidden(L"curl.exe -L --fail --retry 5 --retry-delay 10 -C - --no-progress-meter -o \"" + dest + L".part\" \"" + Wide(std::string(kRepo) + m.sub + "/" + m.file) + L"\"",
                             out, true);
            if (fs::exists(dest + L".part", ec) && fs::file_size(dest + L".part", ec) > static_cast<uintmax_t>(m.gb * 0.9e9)) {
                fs::rename(dest + L".part", dest, ec);
                if (ec) ok = false;
            } else {
                ok = false;
                break;
            }
        }
        g_m.failed = !ok;
        g_m.downloading = false;
    }).detach();
}

// how far the download is: the files done plus the parts so far (GB)
double DownloadedGb() {
    double gb = 0;
    std::error_code ec;
    for (const ModelFile& m : kFiles) {
        const fs::path f = fs::path(g_m.partDir) / m.sub / m.file;
        if (fs::exists(f, ec)) gb += m.gb;
        else if (fs::exists(f.wstring() + L".part", ec)) gb += fs::file_size(f.wstring() + L".part", ec) / 1e9;
    }
    return gb;
}

// ---- the writer: caption and lyrics from an idea (and a picture)
const char* const kMusicSystem =
    "You write prompts for MiniMax Music 3, a model that makes complete songs. Answer in exactly this form and nothing else:\n"
    "CAPTION:\n"
    "Global Metadata: <genre and sub-genre, tempo in BPM, key, the mood and how it develops over the song, what it is for, the production sound>\n"
    "Vocal Details: <the voice, its delivery and harmonies; or \"Instrumental, no vocals.\">\n"
    "Arrangement: <the instruments and how the song develops: intro, verses, chorus, bridge, outro>\n"
    "LYRICS:\n"
    "<the lyrics, with section tags on their own lines: [intro] [verse] [pre-chorus] [chorus] [bridge] [instrumental] [drop] [outro]>\n"
    "Rules: be specific (instruments, tempo, texture). The tags are what shapes the song: it lasts about {SECONDS} seconds, so use about "
    "{SECTIONS} sections and at most {LINES} sung lines in all (the song stops at its length, even mid-verse). Every word in LYRICS is sung: never write stage directions or descriptions there "
    "(no \"guitar solo here\", no \"instrumental swell\"); a part without singing is just the [instrumental] tag on its own. A tag is only the section's name, never a description of it: that goes in Vocal Details and Arrangement. {VOCALS}\n"
    "Before CAPTION, one line \"TITLE: <a short song title>\". After LYRICS, one paragraph \"COVER: <the cover art of this single: what it "
    "shows (the people and the place from the pictures, if any, in a real place or a designed background - never a plain studio backdrop), "
    "the style (photo, painting, graphic design...), the colours and the mood>\".";

// Every word in the lyrics is sung, but small writers still put the arrangement in them ("Soft kick and brush snare, electric piano shimmer"):
// a line about instruments with nobody in it is taken out (a section left empty keeps its tag, which means "no singing")
// A section tag is only the section's name: the rest of "[chorus - male and female together]" came out sung
std::string PlainTag(const std::string& tag) {
    std::string low = tag;
    for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* k : {"pre-chorus", "post-chorus", "chorus", "bridge", "intro", "outro", "instrumental", "solo", "drop", "hook"})
        if (low.find(k) != std::string::npos) return std::string("[") + k + "]";
    if (low.find("female") != std::string::npos) return "[female vocal]"; // (a duet's verses: music_video.cpp SongIdea)
    if (low.find("male") != std::string::npos) return "[male vocal]";
    if (low.find("verse") != std::string::npos) return "[verse]";
    return tag;
}

std::string CleanLyrics(const std::string& lyrics) {
    static const char* kSound[] = {"piano", "guitar", "drum", "snare", "kick", "hi-hat", "synth", "bass", "strings", "cello", "violin", "brass", "beat",
                                   "shimmer", "swell", "solo", "instrumental", "fade", "riff", "melody", "chords", "808", "rhodes", "pad", "percussion"};
    static const char* kPeople[] = {" i ", " i'", " me ", " my ", " you", " we ", " we'", " us ", " our ", " he ", " she ", " his ", " her ", " they ", " love"};
    std::string out;
    size_t a = 0;
    while (a <= lyrics.size()) {
        size_t b = lyrics.find('\n', a);
        if (b == std::string::npos) b = lyrics.size();
        const std::string line = lyrics.substr(a, b - a);
        a = b + 1;
        std::string low = " ";
        for (char c : line) low += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        low += " ";
        const std::string t = Trim(line);
        bool drop = false;
        if (!t.empty() && t.front() != '[' && t.front() != '(') {
            const bool sound = std::any_of(std::begin(kSound), std::end(kSound), [&](const char* k) { return low.find(k) != std::string::npos; });
            const bool people = std::any_of(std::begin(kPeople), std::end(kPeople), [&](const char* k) { return low.find(k) != std::string::npos; });
            drop = sound && !people;
        }
        if (!drop) out += (out.empty() ? "" : "\n") + (t.size() > 2 && t.front() == '[' && t.back() == ']' ? PlainTag(t) : line);
        if (b >= lyrics.size()) break;
    }
    return out;
}

Written WriteMusic(std::string idea, double seconds, bool instrumental, std::vector<std::wstring> photos, std::string photoContext, std::string url) {
    if (!WriterConfigured()) return {"", "", "", "", "the prompt writer is not set up (Clef mode > Prompt writer)"};
    std::string system = kMusicSystem;
    system.replace(system.find("{SECONDS}"), 9, std::to_string(static_cast<int>(seconds)));
    system.replace(system.find("{SECTIONS}"), 10, std::to_string(std::max(2, static_cast<int>(std::lround(seconds / 20.0)))));
    system.replace(system.find("{LINES}"), 7, std::to_string(std::max(4, static_cast<int>(seconds / 3.5))));
    system.replace(system.find("{VOCALS}"), 8,
                   instrumental ? "It is an INSTRUMENTAL: Vocal Details says \"Instrumental, no vocals.\" and LYRICS has only tags, no words (e.g. [intro] then "
                                  "[instrumental] sections then [outro])."
                                : "Write short, singable lyrics that fit the idea, in the language of the idea.");
    const ClefSession session = WriterAcquire(url, nullptr);
    if (!session.ok) {
        WriterRelease(session, false);
        return {"", "", "", "", "the prompt writer could not be started"};
    }
    // pictures to base it on (one, or a whole set like a Vision board): the writer looks at them, or reads what they show when it cannot see
    std::vector<std::string> images;
    const bool see = WriterCanSee();
    const char* about = photos.size() > 1 ? "a song about the attached pictures, taken together" : "a song about the attached picture";
    std::string user = "The music: " + (idea.empty() ? std::string(photos.empty() ? "anything catchy" : about) : idea) + "\n";
    if (!photos.empty()) {
        for (size_t k = 0; k < photos.size() && k < 6 && see; ++k) {
            const std::string pic = ImageDataUrl(photos[k], photos.size() > 2 ? 512 : 768);
            if (!pic.empty()) images.push_back(pic);
        }
        if (!images.empty()) {
            user += photos.size() > 1 ? "Base the song on the attached pictures as ONE story or setting: the people, the places, what happens between them, "
                                        "the mood and colours. The lyrics tell it, and the style suits it. The same person can be in several pictures: "
                                        "count each person once (in the lyrics and on the cover).\n"
                                      : "Base the song on the attached picture: what it shows, its mood and colours, the story it suggests, the place and the "
                                        "people in it. The lyrics are about it, and the style suits it.\n";
        }
        if (!photoContext.empty()) user += (images.empty() ? "It is about pictures that show: " : "What the pictures are: ") + photoContext + "\n";
    } else if (!photoContext.empty()) {
        user += "More to go on: " + photoContext + "\n";
    }
    user += "Length: about " + std::to_string(static_cast<int>(seconds)) + " seconds.";
    std::string out, err;
    const bool ok = WriterChat(system, user, images, 3000, 0.9, out, &err);
    WriterRelease(session, false);
    if (!ok) return {"", "", "", "", "the prompt writer did not answer" + (err.empty() ? std::string() : ": " + err)};
    const size_t c = out.find("CAPTION:"), l = out.find("LYRICS:");
    if (c == std::string::npos || l == std::string::npos || l < c) return {"", "", "", "", "the writer's answer was not in the asked form: try again"};
    Written w;
    if (const size_t t = out.find("TITLE:"); t != std::string::npos && t < c) {
        w.title = Trim(out.substr(t + 6, out.find('\n', t) - t - 6));
        while (!w.title.empty() && (w.title.front() == '"' || w.title.front() == '*')) w.title.erase(w.title.begin());
        while (!w.title.empty() && (w.title.back() == '"' || w.title.back() == '*')) w.title.pop_back();
    }
    const size_t cv = out.find("COVER:", l);
    w.caption = Trim(out.substr(c + 8, l - c - 8));
    w.lyrics = Trim(out.substr(l + 7, cv == std::string::npos ? std::string::npos : cv - l - 7));
    if (cv != std::string::npos) w.cover = Trim(out.substr(cv + 6));
    for (std::string* s : {&w.lyrics, &w.cover}) {
        if (s->rfind("```", 0) == 0) *s = Trim(s->substr(3));
        if (s->size() >= 3 && s->compare(s->size() - 3, 3, "```") == 0) *s = Trim(s->substr(0, s->size() - 3));
    }
    w.lyrics = FitLyrics(CleanLyrics(w.lyrics), seconds); // tested: 10 sections for 60 s, and the song ended mid-verse
    return w;
}

// ---- making it
std::string Workflow() {
    json wf;
    auto node = [&](const char* id, const char* cls, json inputs) { wf[id] = {{"class_type", cls}, {"inputs", std::move(inputs)}, {"_meta", {{"title", cls}}}}; };
    node("1", "UNETLoader", {{"unet_name", g_m.dit.empty() ? std::string(kDit) : g_m.dit}, {"weight_dtype", "default"}});
    node("2", "CLIPLoader", {{"clip_name", kTextEncoder}, {"type", "minimax"}, {"device", "default"}});
    node("3", "VAELoader", {{"vae_name", kVae}});
    node("4", "MiniMaxMusic3AudioGenerate",
         {{"model", json::array({"1", 0})}, {"clip", json::array({"2", 0})}, {"vae", json::array({"3", 0})}, {"caption", g_m.caption}, {"lyrics", g_m.lyrics},
          {"max_duration", g_m.seconds}, {"seed", 0}, {"steps", g_m.steps}, {"guidance_scale", 1.7}, {"decode_mode", "auto"}, {"ar_cfg_scale", 1.5},
          {"ar_top_k", 50}, {"tile_size", 1536}, {"tile_overlap", 64}});
    node("5", "SaveAudioMP3", {{"audio", json::array({"4", 0})}, {"filename_prefix", "audio/Try2Create_music"}, {"quality", "V0"}});
    return wf.dump();
}

std::string SongLabel() {
    std::string t = !g_m.title.empty() ? g_m.title : g_m.idea.empty() ? std::string("a song") : g_m.idea;
    if (t.size() > 50) t = t.substr(0, 47) + "...";
    return "Music: " + t;
}

// the cover: Qwen Image draws it (with the song's pictures as references, so the same people are on it), square, the title on it
int SendCover(AppState& app) {
    ComfyJob job;
    job.label = "Music cover: " + (g_m.title.empty() ? std::string("a song") : g_m.title);
    // (tested: "album cover" alone gets a photo OF a record sleeve on a table, with made-up small print on it)
    // (tested: said last, the title was left off - it goes first now; and the characters' white studio backdrop came along unless told not to)
    std::string p = g_m.title.empty() ? std::string("A song's cover artwork with no text at all. ")
                                      : "A song's cover artwork with the title \"" + g_m.title + "\" in large, well-designed lettering at the top, spelled exactly; "
                                        "it is the ONLY text (no other words, letters or small print). ";
    p += "The square artwork itself, filling the whole image edge to edge: not a photo of a record, a CD or a book, no mockup, no border. " + g_m.cover +
         " The scene has a real place or a designed background, not the plain white studio backdrop of the reference pictures."
         " A professional, finished cover, striking at thumbnail size. Each person appears only once.";
    job.savePrefix = "audio/covers/cover";
    job.times = 1;
    if (!g_m.photos.empty()) { // the pictures' people and places on it
        for (size_t k = 0; k < g_m.photos.size() && k < 3; ++k) job.images.push_back(g_m.photos[k]);
        for (size_t k = 0; k < job.images.size(); ++k) p += " The people and places of <image" + std::to_string(k + 1) + "> are on the cover.";
        job.sheetW = job.sheetH = 1024;
    } else {
        job.draw = 3;
        job.pixW = job.pixH = 1;
    }
    job.prompt = p;
    return SendSheetEdit(app, std::move(job));
}

// the second line on a designed cover: the genre from the description ("Global Metadata: Electro-funk / Nu-disco, 122 BPM...")
std::string CoverLine2() {
    std::string g = g_m.caption;
    const size_t at = g.find("Global Metadata:");
    if (at != std::string::npos) g = g.substr(at + 16);
    g = Trim(g.substr(0, g.find_first_of(",.;\n")));
    if (g.empty() || g.size() > 28) return "Try2Create";
    return g;
}

// the picture the cover is made from (designed on, or as it is)
std::wstring CoverPhoto() {
    if (g_m.photos.empty()) return {};
    return g_m.photos[std::clamp(g_m.coverPhoto, 0, static_cast<int>(g_m.photos.size()) - 1)];
}

std::wstring CoversDir(AppState& app) {
    const std::wstring out = ComfyOutputDir(app.comfy);
    return out.empty() ? DataDir() + L"\\music_covers" : out + L"\\audio\\covers";
}

// songs whose file is being rewritten (the cover, the lyrics): the player lets go of them, and they cannot be played meanwhile
std::mutex g_busyMutex;
std::set<std::wstring> g_busySongs;
std::wstring LowerPath(std::wstring p) {
    for (wchar_t& c : p) c = static_cast<wchar_t>(towlower(c));
    return p;
}
void SetSongBusy(const std::wstring& song, bool busy) {
    std::lock_guard<std::mutex> lock(g_busyMutex);
    if (busy) g_busySongs.insert(LowerPath(song));
    else g_busySongs.erase(LowerPath(song));
}
bool SongBusy(const std::wstring& song) {
    std::lock_guard<std::mutex> lock(g_busyMutex);
    return g_busySongs.count(LowerPath(song)) > 0;
}

void Make(AppState& app) {
    if (g_m.caption.empty()) return;
    // the same song once at a time (a second press while it is being made only made more covers)
    for (const PendingCover& p : g_m.pending) {
        if (p.caption == g_m.caption && p.lyrics == g_m.lyrics) {
            Toast(ToastKind::Info, "Already being made", SongLabel().substr(7) + ": it is on its way (Generate page: its progress)", {}, [](AppState& s) { s.requestTab = 11; },
                  "music");
            return;
        }
    }
    ComfyJob job;
    job.apiWorkflow = Workflow();
    job.label = SongLabel();
    job.prompt = g_m.caption;
    job.times = g_m.forVideo ? 1 : g_m.times;
    PendingCover pc;
    pc.forVideo = g_m.forVideo;
    g_m.forVideo = false;
    pc.title = g_m.title;
    pc.lyrics = g_m.lyrics;
    pc.caption = g_m.caption;
    pc.songAct = SendComfyJob(app, std::move(job));
    if (!pc.songAct) return;
    // its cover
    const std::wstring photo = CoverPhoto();
    const std::wstring file = CoversDir(app) + L"\\cover_" + std::to_wstring(static_cast<long long>(time(nullptr))) + L"_" + std::to_wstring(pc.songAct) + L".png";
    std::string what;
    if (g_m.coverMode == kCoverAi && !g_m.cover.empty()) {
        pc.coverAct = SendCover(app);
        what = " (Qwen draws its cover)";
    } else if (g_m.coverMode == kCoverDesign && CoverTemplatesAvailable()) {
        const int t = g_m.coverTemplate;
        const std::string title = g_m.title.empty() ? (g_m.idea.empty() ? std::string("Untitled") : g_m.idea.substr(0, 40)) : g_m.title, line2 = CoverLine2();
        pc.coverFile = std::async(std::launch::async, [t, photo, title, line2, file]() { return RenderCoverTemplate(t, photo, title, line2, file).empty() ? file : std::wstring(); }).share();
        what = std::string(" (cover: ") + CoverTemplateName(t) + ")";
    } else if (g_m.coverMode == kCoverPicture && !photo.empty()) {
        pc.coverFile = std::async(std::launch::async, [photo, file]() { return SquarePicture(photo, file).empty() ? file : std::wstring(); }).share();
        what = " (its picture as the cover)";
    }
    pc.coverDone = pc.coverAct == 0 && !pc.coverFile.valid();
    g_m.pending.push_back(pc); // when it is back: the cover and the lyrics go into its file
    Toast(ToastKind::Info, "Making music", SongLabel().substr(7) + what, {}, [](AppState& s) { s.requestTab = 11; }, "music");
}

// The cover goes into the song's own file (an MP3's front-cover picture, with the title): music players and phones show it
bool EmbedCover(const std::wstring& song, const std::wstring& cover, const std::string& title) {
    const std::wstring ff = FfmpegPath();
    std::error_code ec;
    if (ff.empty() || !fs::exists(song, ec) || !fs::exists(cover, ec)) return false;
    const std::wstring tmp = song + L".cover.tmp"; // not .mp3: the song list never shows it
    std::wstring cmd = L"\"" + ff + L"\" -y -hide_banner -loglevel error -i \"" + song + L"\" -i \"" + cover +
                       L"\" -map 0:a -map 1:v -c:a copy -c:v mjpeg -vf \"crop='min(iw,ih)':'min(iw,ih)',scale=1000:1000\" -id3v2_version 3 "
                       L"-metadata:s:v title=\"Album cover\" -metadata:s:v comment=\"Cover (front)\" -disposition:v attached_pic -metadata artist=\"Try2Create\"";
    if (!title.empty()) {
        std::string t = title;
        std::replace(t.begin(), t.end(), '"', '\'');
        cmd += L" -metadata title=\"" + Wide(t) + L"\"";
    }
    cmd += L" -f mp3 \"" + tmp + L"\"";
    std::string out;
    RunCaptureHidden(cmd, out, true);
    if (!fs::exists(tmp, ec) || fs::file_size(tmp, ec) < 1000) {
        fs::remove(tmp, ec);
        return false;
    }
    return ReplaceSongFile(tmp, song);
}

// A video to post the song with: its cover, still, for the whole song (1080 x 1080)
std::string MakePostVideo(std::wstring song, std::wstring cover, std::wstring out, bool tall) {
    const int H = tall ? 1920 : 1080; // tall: 1080 x 1920 (TikTok, Reels, Shorts), the picture filling it
    const std::wstring ff = FfmpegPath();
    if (ff.empty()) return "error: ffmpeg.exe was not found";
    std::error_code ec;
    fs::create_directories(fs::path(out).parent_path(), ec);
    std::string log;
    // the song's own length: "-shortest" alone runs a looped picture a few seconds past the end
    const double secs = ProbeVideo(song).seconds;
    wchar_t len[48] = {};
    if (secs > 0) swprintf(len, 48, L"-t %.3f ", secs);
    // the lyrics as subtitles, when the song has them timed (its .srt): white, bold, outlined, in the lower part of the square
    std::wstring subs;
    const std::vector<TimedLyric> lines = ReadSrt(SrtPathOf(song));
    if (!lines.empty()) {
        const std::wstring ass = DataDir() + L"\\post_" + fs::path(out).stem().wstring() + L".ass"; // one per video: two can be made at once
        std::ofstream a(ass, std::ios::binary | std::ios::trunc);
        a << "[Script Info]\nScriptType: v4.00+\nPlayResX: 1080\nPlayResY: " << H << "\nWrapStyle: 0\n\n[V4+ Styles]\n"
             "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, "
             "Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
             "Style: Default,Arial," << (tall ? 64 : 54) << ",&H00FFFFFF,&H000000FF,&H00000000,&H90000000,-1,0,0,0,100,100,0,0,1,4,2,2,80,80," << (tall ? 420 : 120) << ",1\n\n"
             "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";
        auto t = [](double s) {
            const int cs = static_cast<int>(std::lround(std::max(0.0, s) * 100.0));
            char buf[32];
            snprintf(buf, sizeof(buf), "%d:%02d:%02d.%02d", cs / 360000, (cs / 6000) % 60, (cs / 100) % 60, cs % 100);
            return std::string(buf);
        };
        for (const TimedLyric& l : lines) {
            std::string text = l.text;
            for (char& c : text) {
                if (c == '{' || c == '}') c = ' ';
            }
            a << "Dialogue: 0," << t(l.start) << "," << t(l.end) << ",Default,,0,0,0,," << text << "\n";
        }
        a.close();
        std::string p = Utf8(ass); // a path inside a filter: 'C\:/Users/...'
        std::replace(p.begin(), p.end(), '\\', '/');
        std::string esc;
        for (char c : p) esc += c == ':' ? std::string("\\:") : c == '\'' ? std::string("\\'") : std::string(1, c);
        subs = L",subtitles='" + Wide(esc) + L"'";
    }
    RunCaptureHidden(L"\"" + ff + L"\" -y -hide_banner -loglevel error -loop 1 -framerate " + std::wstring(subs.empty() ? L"2" : L"24") + L" -i \"" + cover +
                         L"\" -i \"" + song +
                         L"\" -map 0:v -map 1:a -vf \"scale=1080:" + std::to_wstring(H) + L":force_original_aspect_ratio=increase:flags=lanczos,crop=1080:" + std::to_wstring(H) + subs + L",format=yuv420p\" -c:v libx264 -tune stillimage -r 24 "
                         L"-c:a aac -b:a 192k " + std::wstring(len) + L"-shortest -movflags +faststart \"" + out + L"\"",
                     log, true);
    if (!fs::exists(out, ec) || fs::file_size(out, ec) < 1000) return "error: ffmpeg could not make it" + (log.empty() ? std::string() : ": " + log.substr(0, 200));
    return Utf8(out);
}

// the lyrics passes running in the background, and what each one did (the song's file name, "timed 18 of 20 lines")
std::mutex g_lyricsMutex;
std::vector<std::pair<std::string, std::string>> g_lyricsNotes;
std::atomic<int> g_lyricsBusy{0};
std::vector<std::pair<std::string, std::string>> g_lyricsDone; // shown in the list

// songs and their covers coming back: once both are there, the cover goes into each take of the song
// A song's file named after the song, with its lyrics files ("Try2Create_music_00001" said nothing, on the page or in a folder). The new path ("" = it stays).
std::wstring NameSongFile(const std::wstring& song, const std::string& title) {
    std::wstring name = Wide(title);
    for (wchar_t& c : name)
        if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) c = L' ';
    while (!name.empty() && (name.back() == L' ' || name.back() == L'.')) name.pop_back();
    while (!name.empty() && name.front() == L' ') name.erase(name.begin());
    if (name.size() > 80) name.resize(80);
    if (name.empty()) return {};
    const fs::path from(song);
    std::error_code ec;
    fs::path to = from.parent_path() / (name + from.extension().wstring());
    if (_wcsicmp(to.c_str(), from.c_str()) == 0) return {};
    for (int k = 2; fs::exists(to, ec) && k < 1000; ++k) to = from.parent_path() / (name + L" " + std::to_wstring(k) + from.extension().wstring());
    for (int i = 0; i < 20; ++i) { // (a player letting go of it)
        ec.clear();
        fs::rename(from, to, ec);
        if (!ec) break;
        Sleep(150);
    }
    if (ec) return {};
    fs::rename(SrtPathOf(song), SrtPathOf(to.wstring()), ec);
    fs::path lrc = from, newLrc = to;
    fs::rename(lrc.replace_extension(L".lrc"), newLrc.replace_extension(L".lrc"), ec);
    return to.wstring();
}

std::vector<std::pair<std::wstring, std::wstring>> g_songsNamed; // (with g_lyricsMutex) done in the background, song -> its file now

// the app's own records of a song follow its file
void SongRenamed(const std::wstring from, const std::wstring to) { // (copies: from can be a song of the list, changed below)
    const std::string was = Utf8(fs::path(from).filename().wstring()), now = Utf8(fs::path(to).filename().wstring());
    for (auto& c : g_m.covers)
        if (c.first == was) c.first = now;
    for (Song& song : g_m.songs)
        if (song.path == from) song.path = to, song.name = now, song.title.clear(); // (the list shows it at once)
    ComfyMoveGenerationTime(from, to);
    MusicVideoSongMoved(from, to);
}

std::mutex g_forVideoMutex;
std::wstring g_forVideoSong;  // done
std::string g_forVideoFailed; // why not
std::atomic<int> g_forVideoBusy{0}; // its cover and lyrics are going into the file

void PumpCovers(AppState& app) {
    if (g_m.pending.empty() || !app.comfySvc) return;
    const std::vector<ComfyActivity> acts = app.comfySvc->Activity();
    auto find = [&](int id) -> const ComfyActivity* {
        for (const ComfyActivity& a : acts) {
            if (a.id == id) return &a;
        }
        return nullptr;
    };
    for (size_t i = 0; i < g_m.pending.size();) {
        PendingCover& pc = g_m.pending[i];
        const ComfyActivity* c = find(pc.coverAct);
        if (!pc.coverDone && pc.coverFile.valid()) { // made here
            if (pc.coverFile.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                pc.cover = pc.coverFile.get();
                pc.coverDone = true;
            }
        } else if (!pc.coverDone && (!c || c->state == ComfyState::Done || c->state == ComfyState::Failed)) {
            pc.coverDone = true;
            if (c && c->state == ComfyState::Done) {
                for (const ComfyResult& r : c->files) {
                    if (!r.localPath.empty() && !r.video && !r.audio) {
                        pc.cover = r.localPath;
                        break;
                    }
                }
            }
        }
        const ComfyActivity* s = find(pc.songAct);
        const bool songOver = !s || s->state == ComfyState::Done || s->state == ComfyState::Failed;
        if (songOver && !(s && s->state == ComfyState::Done)) { // the song failed or was stopped: its cover is not wanted either
            if (c && !pc.coverDone && c->state != ComfyState::Done && c->state != ComfyState::Failed) app.comfySvc->Cancel(pc.coverAct);
            if (pc.forVideo) {
                std::lock_guard<std::mutex> lock(g_forVideoMutex);
                g_forVideoFailed = s && !s->message.empty() ? s->message : "the song was not made";
            }
            g_m.pending.erase(g_m.pending.begin() + i);
            continue;
        }
        if (!songOver || !pc.coverDone) {
            ++i;
            continue;
        }
        if (s && s->state == ComfyState::Done) {
            std::vector<std::wstring> songs;
            for (const ComfyResult& r : s->files) {
                if (r.audio && !r.localPath.empty()) songs.push_back(r.localPath);
            }
            if (!pc.cover.empty()) {
                for (const std::wstring& song : songs) g_m.covers.emplace_back(Utf8(fs::path(song).filename().wstring()), pc.cover);
            }
            // into each take's file: the cover, then the lyrics (timed to the singing when a whisper model is there)
            const std::wstring cover = pc.cover;
            const std::string title = pc.title, lyrics = pc.lyrics;
            ++g_lyricsBusy;
            const bool forVideo = pc.forVideo;
            if (forVideo) ++g_forVideoBusy;
            for (const std::wstring& song : songs) SetSongBusy(song, true);
            std::thread([songs, cover, title, lyrics, forVideo]() {
                Sleep(300); // the player lets go of it first (if it was opened already)
                std::wstring kept; // the first take, by its new name
                for (const std::wstring& song : songs) {
                    if (!cover.empty()) EmbedCover(song, cover, title);
                    const std::string note = lyrics.empty() ? std::string() : AddLyricsToSong(song, lyrics, title);
                    std::wstring named = title.empty() ? std::wstring() : NameSongFile(song, title);
                    if (named.empty()) named = song;
                    if (kept.empty()) kept = named;
                    SetSongBusy(song, false);
                    std::lock_guard<std::mutex> lock(g_lyricsMutex);
                    g_songsNamed.push_back({song, named});
                    g_lyricsNotes.push_back({Utf8(fs::path(named).filename().wstring()), note});
                }
                if (forVideo) {
                    std::lock_guard<std::mutex> lock(g_forVideoMutex);
                    if (!kept.empty()) g_forVideoSong = kept;
                    else g_forVideoFailed = "the song came back without a file";
                    --g_forVideoBusy;
                }
                --g_lyricsBusy;
            }).detach();
            Toast(ToastKind::Success, "Song ready" + (title.empty() ? std::string() : ": " + title),
                  cover.empty() ? std::string("Its lyrics go into the song's file") : std::string("With its cover and lyrics, in the song's file too"), cover,
                  [](AppState& st) { st.requestTab = 11; }, "music");
            Save();
        }
        g_m.pending.erase(g_m.pending.begin() + i);
    }
}

// ---- the songs: ComfyUI's output folder (audio) and the app's copies
// a song's title tag and length (ffprobe), remembered by path and time
struct SongMeta {
    fs::file_time_type time;
    std::string title;
    double seconds = 0;
};
std::mutex g_metaMutex;
std::map<std::wstring, SongMeta> g_meta;

SongMeta ProbeSong(const std::wstring& path, fs::file_time_type time) {
    {
        std::lock_guard<std::mutex> lock(g_metaMutex);
        auto it = g_meta.find(path);
        if (it != g_meta.end() && it->second.time == time) return it->second;
    }
    SongMeta m;
    m.time = time;
    const std::wstring ff = FfmpegPath();
    const std::wstring probe = ff.empty() ? std::wstring() : (fs::path(ff).parent_path() / L"ffprobe.exe").wstring();
    std::error_code ec;
    if (!probe.empty() && fs::exists(probe, ec)) {
        std::string out;
        RunCaptureHidden(L"\"" + probe + L"\" -v error -show_entries format=duration:format_tags=title -of json \"" + path + L"\"", out, false);
        const json j = json::parse(out, nullptr, false);
        if (j.is_object() && j.contains("format") && j["format"].is_object()) {
            const json& f = j["format"];
            if (f.contains("duration") && f["duration"].is_string()) m.seconds = std::atof(f["duration"].get<std::string>().c_str());
            if (f.contains("tags") && f["tags"].is_object())
                for (const auto& [k, v] : f["tags"].items()) {
                    if ((k == "title" || k == "TITLE" || k == "Title") && v.is_string()) m.title = v.get<std::string>();
                }
        }
    }
    std::lock_guard<std::mutex> lock(g_metaMutex);
    g_meta[path] = m;
    return m;
}

// the lyrics saved in a song's workflow go into its file (timed when a whisper model is there): the Lyrics button, and songs repaired
void RunLyricsPass(const std::wstring& path, const std::string& name) {
    ++g_lyricsBusy;
    SetSongBusy(path, true);
    std::thread([path, name]() {
        Sleep(300); // the player lets go of it first
        const std::string lyrics = LyricsFromSongFile(path);
        const std::string n = lyrics.empty() ? std::string("no lyrics saved in this file") : AddLyricsToSong(path, lyrics, std::string());
        SetSongBusy(path, false);
        std::lock_guard<std::mutex> lock(g_lyricsMutex);
        g_lyricsNotes.push_back({name, n});
        --g_lyricsBusy;
    }).detach();
}

std::vector<Song> ScanSongs(std::vector<std::wstring> dirs) {
    std::vector<Song> out;
    for (const std::wstring& d : dirs) {
        std::error_code ec;
        if (d.empty() || !fs::is_directory(d, ec)) continue;
        for (fs::recursive_directory_iterator it(d, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            if (it.depth() > 1) {
                it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            std::wstring name = it->path().filename().wstring();
            for (wchar_t& c : name) c = static_cast<wchar_t>(towlower(c));
            // a song with its cover that could not take the song's place (an older version left "x.mp3.cover.mp3" when the song was playing): it does now
            const std::wstring leftover = L".mp3.cover.mp3";
            if (name.size() > leftover.size() && name.compare(name.size() - leftover.size(), leftover.size(), leftover) == 0) {
                const std::wstring song = it->path().wstring().substr(0, it->path().wstring().size() - 10);
                if (fs::exists(song, ec) && !SongBusy(song) && fs::file_size(it->path(), ec) > 1000) {
                    fs::rename(it->path(), song, ec);
                    if (!ec) RunLyricsPass(song, Utf8(fs::path(song).filename().wstring())); // its lyrics were written into the old file
                }
                continue;
            }
            std::wstring ext = it->path().extension().wstring();
            for (wchar_t& c : ext) c = static_cast<wchar_t>(towlower(c));
            if (ext != L".mp3" && ext != L".flac" && ext != L".wav" && ext != L".opus" && ext != L".ogg") continue;
            Song s;
            s.path = it->path().wstring();
            s.name = Utf8(it->path().filename().wstring());
            s.time = it->last_write_time(ec);
            if (std::none_of(out.begin(), out.end(), [&](const Song& o) { return o.name == s.name; })) out.push_back(std::move(s));
        }
    }
    std::sort(out.begin(), out.end(), [](const Song& a, const Song& b) { return a.time > b.time; });
    if (out.size() > 200) out.resize(200);
    for (Song& s : out) {
        if (SongBusy(s.path)) continue; // read again once it has its cover and lyrics
        const SongMeta m = ProbeSong(s.path, s.time);
        s.title = m.title;
        s.seconds = m.seconds;
    }
    return out;
}

void Play(AppState& app, const std::wstring& path) {
    if (SongBusy(path)) {
        app.SetStatus("Its cover and lyrics are going into the file: a moment");
        return;
    }
    if (!g_m.player) {
        g_m.player = std::make_unique<VideoPlayer>();
        g_m.player->FollowVideoVolume(false); // (its own volume slider)
    }
    ID3D11DeviceContext* ctx = nullptr;
    app.Device()->GetImmediateContext(&ctx);
    if (!g_m.player->Open(app.Device(), ctx, path, g_m.volume, false, g_m.repeat == 2)) app.SetStatus(g_m.player->Error());
    if (ctx) ctx->Release();
    g_m.playing = path;
    g_m.selected = path;
    g_m.wasPlaying = true;
}

int SongIndex(const std::wstring& path) {
    for (size_t k = 0; k < g_m.songs.size(); ++k) {
        if (_wcsicmp(g_m.songs[k].path.c_str(), path.c_str()) == 0) return static_cast<int>(k);
    }
    return -1;
}

// the next (or previous) song: in the list's order, at random with shuffle; past the end only with repeat
void PlayNext(AppState& app, int dir, bool automatic) {
    const int n = static_cast<int>(g_m.songs.size());
    if (n == 0) return;
    int at = SongIndex(g_m.playing);
    if (!automatic && dir < 0 && g_m.player && g_m.player->IsOpen() && g_m.player->Position() > 3.0) { // a few seconds in: back to its start
        g_m.player->Seek(0);
        return;
    }
    int next;
    if (g_m.shuffle && n > 1) {
        static unsigned seed = static_cast<unsigned>(GetTickCount64());
        seed = seed * 1103515245u + 12345u;
        next = static_cast<int>((seed >> 8) % static_cast<unsigned>(n - 1));
        if (next >= at) ++next;
    } else {
        next = at < 0 ? 0 : at + dir;
        if (next >= n || next < 0) {
            if (automatic && g_m.repeat == 0) return; // the end of the list
            next = (next + n) % n;
        }
    }
    Play(app, g_m.songs[next].path);
}

std::string Clock(double s) {
    const int t = static_cast<int>(std::max(0.0, s));
    char buf[16];
    snprintf(buf, sizeof(buf), "%d:%02d", t / 60, t % 60);
    return buf;
}

bool Chip(const char* label, bool on) {
    ImGui::PushStyleColor(ImGuiCol_Button, on ? ui::ChipActiveColor() : ui::ChipColor());
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::ChipHoverColor());
    const bool clicked = ImGui::Button(label);
    ImGui::PopStyleColor(2);
    return clicked;
}

struct Style {
    const char* name;
    const char* idea;
};
const Style kStyles[] = {
    {"Lo-fi chill", "Lo-fi hip-hop for studying at night: dusty drums, warm Rhodes, vinyl crackle, rain"},
    {"Drama score", "A tense, emotional cinematic score for a short drama video: soft piano, swelling strings, a heartbeat pulse"},
    {"Sad piano", "A slow, sad solo piano piece with soft reverb, heartbreak and longing"},
    {"Upbeat pop", "A bright, catchy summer pop song with a big singalong chorus"},
    {"Trap beat", "A hard trap beat: 808 bass, rolling hi-hats, dark synth melody"},
    {"Epic orchestral", "An epic orchestral trailer piece: big drums, brass, choir, rising to a huge climax"},
    {"Acoustic love song", "A warm acoustic love song: fingerpicked guitar, gentle male and female duet"},
    {"Viral dance", "A short, punchy dance track for a viral TikTok dance: four-on-the-floor, catchy vocal chop hook"},
};

// ---- the player (it looks and works like a music app: a big header for the song, its lyrics as it plays, the list, a bar at the bottom)
const ImU32 kGreen = IM_COL32(30, 215, 96, 255);
const ImVec4 kGreenV(0.12f, 0.84f, 0.38f, 1.0f);

// the header's colour: the cover's average, darkened (worked out once per cover)
std::mutex g_tintMutex;
std::map<std::wstring, ImU32> g_tints;
ImU32 CoverTint(const std::wstring& cover) {
    const ImU32 plain = ui::IsLight() ? IM_COL32(200, 200, 205, 255) : IM_COL32(70, 70, 76, 255);
    if (cover.empty()) return plain;
    {
        std::lock_guard<std::mutex> lock(g_tintMutex);
        auto it = g_tints.find(cover);
        if (it != g_tints.end()) return it->second;
        g_tints[cover] = plain;
    }
    std::thread([cover, plain]() {
        const std::wstring ff = FfmpegPath();
        std::string out;
        if (!ff.empty())
            RunCaptureHidden(L"\"" + ff + L"\" -v error -i \"" + cover + L"\" -frames:v 1 -vf scale=1:1:flags=area -f rawvideo -pix_fmt rgb24 -", out, false);
        ImU32 c = plain;
        if (out.size() >= 3) {
            const float k = ui::IsLight() ? 1.0f : 0.62f;
            auto ch = [&](size_t i) { return static_cast<int>(static_cast<unsigned char>(out[out.size() - 3 + i]) * k); };
            c = IM_COL32(ch(0), ch(1), ch(2), 255);
        }
        std::lock_guard<std::mutex> lock(g_tintMutex);
        g_tints[cover] = c;
    }).detach();
    return plain;
}

// a song's timed lyrics (its .srt), read again when the file changes
std::vector<TimedLyric> SongLyrics(const std::wstring& song) {
    static std::wstring of;
    static fs::file_time_type at;
    static std::vector<TimedLyric> lines;
    std::error_code ec;
    const std::wstring srt = SrtPathOf(song);
    const fs::file_time_type t = fs::last_write_time(srt, ec);
    if (ec) return {};
    if (of != srt || at != t) of = srt, at = t, lines = ReadSrt(srt);
    return lines;
}

std::string Ellipsize(const std::string& s, float maxW) {
    if (ImGui::CalcTextSize(s.c_str()).x <= maxW) return s;
    std::string t = s;
    while (!t.empty() && ImGui::CalcTextSize((t + "...").c_str()).x > maxW) {
        t.pop_back();
        while (!t.empty() && (static_cast<unsigned char>(t.back()) & 0xC0) == 0x80) t.pop_back(); // a whole UTF-8 character
        if (!t.empty() && (static_cast<unsigned char>(t.back()) & 0x80)) t.pop_back();
    }
    return t + "...";
}

std::string SongTitle(const Song& s) {
    std::string n = Utf8(fs::path(s.path).stem().wstring());
    if (!s.title.empty() && n.rfind("Try2Create_music", 0) == 0) return s.title; // (a song still under ComfyUI's name; else the file's name is the song's, and Rename changes it)
    std::replace(n.begin(), n.end(), '_', ' ');
    return n;
}

enum Icon { kIcPlay, kIcPause, kIcPrev, kIcNext, kIcShuffle, kIcRepeat, kIcVolume, kIcMuted };
void DrawIcon(ImDrawList* dl, Icon ic, ImVec2 c, float r, ImU32 col) {
    const float t = std::max(1.5f, r * 0.13f);
    switch (ic) {
    case kIcPlay: dl->AddTriangleFilled(ImVec2(c.x - r * 0.4f, c.y - r * 0.55f), ImVec2(c.x - r * 0.4f, c.y + r * 0.55f), ImVec2(c.x + r * 0.55f, c.y), col); break;
    case kIcPause:
        dl->AddRectFilled(ImVec2(c.x - r * 0.42f, c.y - r * 0.5f), ImVec2(c.x - r * 0.14f, c.y + r * 0.5f), col, r * 0.06f);
        dl->AddRectFilled(ImVec2(c.x + r * 0.14f, c.y - r * 0.5f), ImVec2(c.x + r * 0.42f, c.y + r * 0.5f), col, r * 0.06f);
        break;
    case kIcPrev:
        dl->AddRectFilled(ImVec2(c.x - r * 0.55f, c.y - r * 0.5f), ImVec2(c.x - r * 0.38f, c.y + r * 0.5f), col);
        dl->AddTriangleFilled(ImVec2(c.x + r * 0.5f, c.y - r * 0.5f), ImVec2(c.x + r * 0.5f, c.y + r * 0.5f), ImVec2(c.x - r * 0.38f, c.y), col);
        break;
    case kIcNext:
        dl->AddRectFilled(ImVec2(c.x + r * 0.38f, c.y - r * 0.5f), ImVec2(c.x + r * 0.55f, c.y + r * 0.5f), col);
        dl->AddTriangleFilled(ImVec2(c.x - r * 0.5f, c.y - r * 0.5f), ImVec2(c.x - r * 0.5f, c.y + r * 0.5f), ImVec2(c.x + r * 0.38f, c.y), col);
        break;
    case kIcShuffle:
        dl->AddBezierCubic(ImVec2(c.x - r * 0.65f, c.y - r * 0.4f), ImVec2(c.x - r * 0.1f, c.y - r * 0.4f), ImVec2(c.x + r * 0.05f, c.y + r * 0.4f),
                           ImVec2(c.x + r * 0.45f, c.y + r * 0.4f), col, t);
        dl->AddBezierCubic(ImVec2(c.x - r * 0.65f, c.y + r * 0.4f), ImVec2(c.x - r * 0.1f, c.y + r * 0.4f), ImVec2(c.x + r * 0.05f, c.y - r * 0.4f),
                           ImVec2(c.x + r * 0.45f, c.y - r * 0.4f), col, t);
        for (float sy : {-1.0f, 1.0f})
            dl->AddTriangleFilled(ImVec2(c.x + r * 0.4f, c.y + sy * r * 0.4f - r * 0.22f), ImVec2(c.x + r * 0.4f, c.y + sy * r * 0.4f + r * 0.22f),
                                  ImVec2(c.x + r * 0.7f, c.y + sy * r * 0.4f), col);
        break;
    case kIcRepeat:
        dl->AddRect(ImVec2(c.x - r * 0.6f, c.y - r * 0.38f), ImVec2(c.x + r * 0.6f, c.y + r * 0.38f), col, r * 0.3f, 0, t);
        dl->AddTriangleFilled(ImVec2(c.x + r * 0.05f, c.y - r * 0.62f), ImVec2(c.x + r * 0.05f, c.y - r * 0.14f), ImVec2(c.x + r * 0.35f, c.y - r * 0.38f), col);
        break;
    case kIcVolume:
    case kIcMuted:
        dl->AddRectFilled(ImVec2(c.x - r * 0.62f, c.y - r * 0.2f), ImVec2(c.x - r * 0.38f, c.y + r * 0.2f), col);
        dl->AddQuadFilled(ImVec2(c.x - r * 0.38f, c.y - r * 0.2f), ImVec2(c.x - r * 0.02f, c.y - r * 0.52f), ImVec2(c.x - r * 0.02f, c.y + r * 0.52f),
                          ImVec2(c.x - r * 0.38f, c.y + r * 0.2f), col);
        if (ic == kIcMuted) {
            dl->AddLine(ImVec2(c.x + r * 0.2f, c.y - r * 0.25f), ImVec2(c.x + r * 0.7f, c.y + r * 0.25f), col, t);
            dl->AddLine(ImVec2(c.x + r * 0.2f, c.y + r * 0.25f), ImVec2(c.x + r * 0.7f, c.y - r * 0.25f), col, t);
        } else {
            dl->PathArcTo(ImVec2(c.x - r * 0.05f, c.y), r * 0.35f, -0.9f, 0.9f);
            dl->PathStroke(col, 0, t);
            dl->PathArcTo(ImVec2(c.x - r * 0.05f, c.y), r * 0.68f, -0.9f, 0.9f);
            dl->PathStroke(col, 0, t);
        }
        break;
    }
}

// an icon you can click: dim, brighter on hover, green when on (with a dot under it)
bool IconButton(const char* id, Icon ic, float size, bool on = false, bool enabled = true) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size)) && enabled;
    const bool hov = ImGui::IsItemHovered() && enabled;
    ImU32 col = !enabled ? ImGui::GetColorU32(ImGuiCol_TextDisabled) : on ? kGreen : ImGui::GetColorU32(ImGuiCol_Text, hov ? 1.0f : 0.72f);
    const ImVec2 c(p.x + size * 0.5f, p.y + size * 0.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    DrawIcon(dl, ic, c, size * 0.42f, col);
    if (on) dl->AddCircleFilled(ImVec2(c.x, p.y + size + ui::Dp(1.0f)), ui::Dp(1.8f), kGreen);
    return clicked;
}

// the round play button
bool PlayCircle(const char* id, bool playing, float size, ImU32 bg, ImU32 fg, bool enabled = true) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size)) && enabled;
    const bool hov = ImGui::IsItemHovered() && enabled;
    const ImVec2 c(p.x + size * 0.5f, p.y + size * 0.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = size * 0.5f * (hov ? 1.05f : 1.0f);
    dl->AddCircleFilled(c, r, enabled ? bg : ImGui::GetColorU32(ImGuiCol_TextDisabled), 40);
    DrawIcon(dl, playing ? kIcPause : kIcPlay, ImVec2(c.x + (playing ? 0.0f : r * 0.06f), c.y), r * 0.62f, fg);
    return clicked;
}

// a thin bar you can click or drag on (the song's position, the volume): green and with a knob while the mouse is on it
bool SlimBar(const char* id, float width, float frac, float& picked, bool live) {
    const float h = ui::Dp(14.0f), th = ui::Dp(4.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(width, h));
    const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    static ImGuiID dragging = 0;
    static float dragFrac = 0;
    const ImGuiID me = ImGui::GetItemID();
    bool changed = false;
    if (act) {
        dragging = me;
        dragFrac = std::clamp((ImGui::GetIO().MousePos.x - p.x) / std::max(1.0f, width), 0.0f, 1.0f);
        if (live) picked = dragFrac, changed = true;
    }
    if (ImGui::IsItemDeactivated() && dragging == me) {
        picked = dragFrac;
        changed = true;
        dragging = 0;
    }
    const float f = std::clamp(dragging == me ? dragFrac : frac, 0.0f, 1.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float y = p.y + h * 0.5f;
    dl->AddRectFilled(ImVec2(p.x, y - th * 0.5f), ImVec2(p.x + width, y + th * 0.5f), ImGui::GetColorU32(ImGuiCol_Text, 0.22f), th);
    dl->AddRectFilled(ImVec2(p.x, y - th * 0.5f), ImVec2(p.x + width * f, y + th * 0.5f), hov || act ? kGreen : ImGui::GetColorU32(ImGuiCol_Text, 0.9f), th);
    if (hov || act) dl->AddCircleFilled(ImVec2(p.x + width * f, y), ui::Dp(6.0f), ImGui::GetColorU32(ImGuiCol_Text), 20);
    return changed;
}

void Cover(AppState& app, const std::wstring& cover, ImVec2 p, float size, float rounding) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const AppState::LibThumb lt = cover.empty() ? AppState::LibThumb{} : app.LibraryThumb(cover);
    if (lt.srv) {
        dl->AddImageRounded((ImTextureID)(intptr_t)lt.srv, p, ImVec2(p.x + size, p.y + size), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, rounding);
        return;
    }
    dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), ui::ThumbPlaceholder(), rounding);
    // a note, where there is no cover
    const ImU32 c = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImVec2 m(p.x + size * 0.45f, p.y + size * 0.62f);
    dl->AddCircleFilled(m, size * 0.11f, c);
    dl->AddLine(ImVec2(m.x + size * 0.1f, m.y), ImVec2(m.x + size * 0.1f, p.y + size * 0.26f), c, std::max(1.5f, size * 0.04f));
    dl->AddLine(ImVec2(m.x + size * 0.1f, p.y + size * 0.26f), ImVec2(m.x + size * 0.26f, p.y + size * 0.34f), c, std::max(1.5f, size * 0.04f));
}

void PostVideo(AppState& app, const Song& s, const std::wstring& cover) {
    if (g_m.postFut.valid() || cover.empty()) return;
    const std::wstring dir = ComfyOutputDir(app.comfy);
    const std::wstring out = (dir.empty() ? DataDir() + L"\\music" : dir + L"\\video") + L"\\Song_" + fs::path(s.path).stem().wstring() + L".mp4";
    g_m.postFut = std::async(std::launch::async, MakePostVideo, s.path, cover, out, false);
    Toast(ToastKind::Info, "Making the video for posting", SongTitle(s), cover, {}, "music");
}

// what can be done with a song (its row's right-click menu, the header's "...")
void SongMenu(AppState& app, const Song& s, bool isCurrent, VideoPlayer* vp) {
    const std::wstring cover = CoverOf(s.name);
    std::error_code ec;
    { // its name: the title shown, and the file's own name with it
        static char name[128] = "";
        static std::wstring nameFor;
        if (nameFor != s.path) nameFor = s.path, snprintf(name, sizeof(name), "%s", SongTitle(s).c_str());
        ImGui::SetNextItemWidth(ui::Dp(200.0f));
        ImGui::InputTextWithHint("##songname", "the song's name", name, sizeof(name));
        ImGui::SameLine();
        ImGui::BeginDisabled(SongBusy(s.path));
        if (ImGui::SmallButton("Rename")) {
            if (isCurrent && g_m.player) g_m.player->Close(); // (it lets go of the file)
            const std::wstring now = NameSongFile(s.path, name);
            if (!now.empty()) {
                SongRenamed(s.path, now);
                g_m.selected = now;
                g_m.scanned = -100.0;
                Save();
                nameFor.clear();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndDisabled();
        ImGui::Separator();
    }
    if (ImGui::MenuItem(isCurrent && vp && vp->Playing() ? "Pause" : "Play", nullptr, false, !SongBusy(s.path))) {
        if (isCurrent && vp) vp->Toggle();
        else Play(app, s.path);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Under the story")) {
        SetStoryMusic(s.path);
        Toast(ToastKind::Success, "The story's music", SongTitle(s) + " goes under the story when its clips are joined", {}, [](AppState& st) { st.requestTab = 10; }, "music");
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("The soundtrack of Video > Make a story (looped, faded, quieter while people talk)");
    if (ImGui::MenuItem("Make a music video")) OpenMusicVideo(app, s.path);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Video > Make a music video with this song: your characters sing and act it out");
    if (ImGui::MenuItem("Video for posting", nullptr, false, !cover.empty() && !g_m.postFut.valid())) PostVideo(app, s, cover);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(cover.empty() ? "It needs a cover" : "An MP4 of the song with its cover (1080 x 1080) and its lyrics as subtitles, for TikTok, Instagram or YouTube");
    if (ImGui::MenuItem(fs::exists(SrtPathOf(s.path), ec) ? "Time the lyrics again" : "Put the lyrics in the file", nullptr, false, g_lyricsBusy == 0)) RunLyricsPass(s.path, s.name);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("The lyrics saved with the song go into the MP3, timed to the singing when a whisper model is there (.lrc and .srt next to it)");
    ImGui::Separator();
    if (ImGui::MenuItem("Show in Explorer")) {
        const std::wstring arg = L"/select,\"" + s.path + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
    }
    if (ImGui::MenuItem("Open the cover", nullptr, false, !cover.empty())) app.OpenFile(cover);
}

void DrawPlayerPane(AppState& app) {
    static bool playTest = [] { // MV_UI_TEST "musicplay": plays the first song with timed lyrics, from its third line (tests)
        char buf[256] = {};
        return GetEnvironmentVariableA("MV_UI_TEST", buf, sizeof(buf)) && strstr(buf, "musicplay") != nullptr;
    }();
    static double testSeek = -1;
    if (playTest && !g_m.songs.empty()) {
        playTest = false;
        for (const Song& s : g_m.songs) {
            const std::vector<TimedLyric> lines = ReadSrt(SrtPathOf(s.path));
            if (lines.size() < 3) continue;
            Play(app, s.path);
            g_m.selected = s.path;
            testSeek = std::max(0.0, lines[2].start - 0.6);
            break;
        }
    }
    if (testSeek >= 0 && g_m.player && g_m.player->IsOpen() && g_m.player->Duration() > 0) { // (once it knows its length)
        g_m.player->Seek(testSeek);
        testSeek = -1;
    }
    VideoPlayer* vp = g_m.player && g_m.player->IsOpen() ? g_m.player.get() : nullptr;
    if (vp && SongBusy(g_m.playing)) { // its file is being rewritten (cover, lyrics): let go of it
        g_m.player->Close();
        vp = nullptr;
        g_m.wasPlaying = false;
    }
    if (vp) {
        vp->Update();
        const bool playing = vp->Playing();
        if (g_m.wasPlaying && !playing && vp->Duration() > 0 && vp->Position() >= vp->Duration() - 0.4) { // it ended: the next one
            g_m.wasPlaying = false;
            PlayNext(app, +1, true);
            vp = g_m.player && g_m.player->IsOpen() ? g_m.player.get() : nullptr;
        } else {
            g_m.wasPlaying = playing;
        }
    }
    const int playingAt = vp ? SongIndex(g_m.playing) : -1;
    int heroAt = SongIndex(g_m.selected);
    if (heroAt < 0) heroAt = playingAt >= 0 ? playingAt : g_m.songs.empty() ? -1 : 0;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float barH = ui::Dp(88.0f);

    ImGui::BeginChild("##playermain", ImVec2(0, -barH), ImGuiChildFlags_None);
    ImDrawList* mdl = ImGui::GetWindowDrawList();
    const float W = ImGui::GetContentRegionAvail().x;
    // ---- the header: the song's cover, big, its title and what can be done with it
    if (heroAt >= 0) {
        const Song& h = g_m.songs[heroAt];
        const std::wstring cover = CoverOf(h.name);
        const bool heroPlaying = heroAt == playingAt;
        const float cs = std::clamp(W * 0.26f, ui::Dp(110.0f), ui::Dp(220.0f)), pad = ui::Dp(20.0f);
        const ImVec2 top = ImGui::GetCursorScreenPos();
        const float heroH = cs + pad * 2;
        const ImU32 tint = CoverTint(cover);
        mdl->AddRectFilledMultiColor(ImVec2(top.x - ImGui::GetStyle().WindowPadding.x, top.y - ImGui::GetStyle().WindowPadding.y), ImVec2(top.x + W + ImGui::GetStyle().WindowPadding.x, top.y + heroH),
                                     tint, tint, tint & 0x00FFFFFF, tint & 0x00FFFFFF);
        const ImVec2 cp(top.x + pad, top.y + pad);
        mdl->AddRectFilled(ImVec2(cp.x + ui::Dp(4.0f), cp.y + ui::Dp(6.0f)), ImVec2(cp.x + cs + ui::Dp(4.0f), cp.y + cs + ui::Dp(6.0f)), IM_COL32(0, 0, 0, 90), ui::Dp(6.0f));
        Cover(app, cover, cp, cs, ui::Dp(6.0f));
        const float tx = cp.x + cs + ui::Dp(22.0f), tw = std::max(ui::Dp(60.0f), top.x + W - tx - pad);
        const ImU32 strong = ui::IsLight() ? IM_COL32(15, 15, 15, 255) : IM_COL32(255, 255, 255, 255);
        const ImU32 soft = ui::IsLight() ? IM_COL32(40, 40, 40, 200) : IM_COL32(255, 255, 255, 190);
        float y = cp.y + cs * 0.18f;
        mdl->AddText(ui::BoldFont(), ImGui::GetFontSize() * 0.9f, ImVec2(tx, y), soft, "SONG");
        y += ImGui::GetFontSize() * 1.4f;
        // the title as big as it fits (in the big face: the heading face blown up to twice its size looked jagged)
        const std::string title = SongTitle(h);
        ImFont* tf = ui::TitleFont();
        ImFont* df = ui::DisplayFont();
        float fsz = std::min(ui::Dp(52.0f), cs * 0.3f);
        while (fsz > ui::Dp(18.0f) && df->CalcTextSizeA(fsz, FLT_MAX, 0.0f, title.c_str()).x > tw) fsz -= ui::Dp(2.0f);
        ImGui::PushFont(df);
        const std::string shown = df->CalcTextSizeA(fsz, FLT_MAX, 0.0f, title.c_str()).x > tw ? Ellipsize(title, tw * df->FontSize / fsz) : title;
        ImGui::PopFont();
        mdl->AddText(fsz > tf->FontSize * 1.15f ? df : tf, fsz, ImVec2(tx, std::round(y)), strong, shown.c_str());
        y += fsz * 1.15f;
        std::string meta = "Try2Create";
        if (h.seconds > 0) meta += "  \xC2\xB7  " + Clock(h.seconds);
        double took = 0;
        if (ComfyGenerationTime(h.path, took)) meta += "  \xC2\xB7  made in " + FormatTook(took);
        std::error_code ec;
        if (fs::exists(SrtPathOf(h.path), ec)) meta += "  \xC2\xB7  lyrics timed";
        if (SongBusy(h.path)) meta += "  \xC2\xB7  adding its cover and lyrics...";
        mdl->AddText(ImVec2(tx, y), soft, Ellipsize(meta, tw).c_str());
        // the buttons, under it
        ImGui::SetCursorScreenPos(ImVec2(tx, std::max(y + ImGui::GetFontSize() + ui::Dp(12.0f), cp.y + cs - ui::Dp(50.0f))));
        const float rowY = ImGui::GetCursorPosY() + (ui::Dp(50.0f) - ImGui::GetFrameHeight()) * 0.5f; // (every button on the middle of the play button)
        if (PlayCircle("##heroplay", heroPlaying && vp && vp->Playing(), ui::Dp(50.0f), kGreen, IM_COL32(0, 0, 0, 255), !SongBusy(h.path))) {
            if (heroPlaying && vp) vp->Toggle();
            else Play(app, h.path);
        }
        const float playBottom = ImGui::GetItemRectMax().y;
        ImGui::SameLine(0.0f, ui::Dp(14.0f));
        ImGui::SetCursorPosY(rowY);
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Border, soft);
        ImGui::PushStyleColor(ImGuiCol_Text, strong);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ui::Dp(14.0f));
        ImGui::BeginDisabled(cover.empty() || g_m.postFut.valid());
        if (ImGui::Button(g_m.postFut.valid() ? "Making the video..." : "Video for posting")) PostVideo(app, h, cover);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(cover.empty() ? "It needs a cover" : "An MP4 of the song with its cover (1080 x 1080) and its lyrics as subtitles, for TikTok, Instagram or YouTube");
        ImGui::SameLine();
        ImGui::SetCursorPosY(rowY);
        if (ImGui::Button("Under the story")) {
            SetStoryMusic(h.path);
            Toast(ToastKind::Success, "The story's music", title + " goes under the story when its clips are joined", {}, [](AppState& st) { st.requestTab = 10; }, "music");
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("The soundtrack of Video > Make a story (looped, faded, quieter while people talk)");
        ImGui::SameLine();
        ImGui::SetCursorPosY(rowY);
        if (ImGui::Button("...")) ImGui::OpenPopup("##heromenu");
        const float buttonsBottom = std::max(playBottom, ImGui::GetItemRectMax().y);
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(3);
        if (ImGui::BeginPopup("##heromenu")) {
            SongMenu(app, h, heroPlaying, vp);
            ImGui::EndPopup();
        }
        const float below = std::max(top.y + heroH, buttonsBottom + ui::Dp(12.0f)); // a narrow pane: the buttons may reach below the cover
        ImGui::SetCursorScreenPos(ImVec2(top.x, below));
        ImGui::Dummy(ImVec2(W, 0));
        // ---- its lyrics, line by line as it plays
        if (heroPlaying && vp) {
            const std::vector<TimedLyric> lines = SongLyrics(h.path);
            if (!lines.empty()) {
                const double t = vp->Position();
                int cur = -1;
                for (size_t k = 0; k < lines.size(); ++k) {
                    if (lines[k].start <= t + 0.15) cur = static_cast<int>(k);
                }
                const ImVec2 lp = ImGui::GetCursorScreenPos();
                const float big = ui::Dp(26.0f), smallSz = ImGui::GetFontSize() * 1.05f;
                const float step = ui::Dp(34.0f);       // from one line to the next
                const float areaH = step * 3.0f + ui::Dp(8.0f);
                // where the list is: eased toward the line being sung (a jump - a seek, another song - is not animated)
                static float shownAt = 0.0f;
                static std::wstring shownFor;
                const float target = static_cast<float>(cur);
                if (shownFor != h.path || std::fabs(shownAt - target) > 2.5f) shownAt = target, shownFor = h.path;
                shownAt += (target - shownAt) * (1.0f - std::exp(-ImGui::GetIO().DeltaTime * 9.0f));
                if (std::fabs(shownAt - target) < 0.002f) shownAt = target;
                const bool between = cur >= 0 && t > lines[cur].end + 0.3; // an instrumental part
                const float midY = lp.y + ui::Dp(4.0f) + step * 1.0f;   // the sung line's place: the middle of three
                mdl->PushClipRect(ImVec2(lp.x, lp.y), ImVec2(lp.x + W, lp.y + areaH), true);
                ImGui::PushFont(tf); // (Ellipsize measures in it)
                for (int k = static_cast<int>(std::floor(shownAt)) - 2; k <= static_cast<int>(std::floor(shownAt)) + 3; ++k) {
                    if (k < 0 || k >= static_cast<int>(lines.size())) continue;
                    const float d = static_cast<float>(k) - shownAt; // 0 = the sung line, -1 the one before, 1 the next
                    const float closeness = std::clamp(1.0f - std::fabs(d), 0.0f, 1.0f);
                    float size = smallSz + (big - smallSz) * closeness;
                    const float textW = tf->CalcTextSizeA(size, FLT_MAX, 0.0f, lines[k].text.c_str()).x, room = W - pad * 2.0f;
                    if (textW > room) size = std::max(smallSz * 0.8f, size * room / textW); // a long line: smaller, so it fits
                    const float y = midY + d * step + (big - size) * 0.5f;
                    float alpha = std::clamp(1.0f - std::fabs(d) * 0.42f, 0.0f, 1.0f);
                    if (d < 0) alpha *= 0.75f; // (sung already)
                    if (k == cur && between) alpha *= 0.55f;
                    ImVec4 c = ImGui::ColorConvertU32ToFloat4(strong);
                    c.w *= alpha;
                    mdl->AddText(tf, size, ImVec2(lp.x + pad, y), ImGui::GetColorU32(c), Ellipsize(lines[k].text, room * tf->FontSize / size).c_str());
                }
                ImGui::PopFont();
                mdl->PopClipRect();
                ImGui::Dummy(ImVec2(W, areaH + ui::Dp(4.0f)));
            }
        }
    } else {
        ImGui::Spacing();
        ImGui::PushFont(ui::TitleFont());
        ImGui::TextUnformatted("Your music");
        ImGui::PopFont();
        ImGui::TextDisabled("Nothing yet: songs you make show up here (ComfyUI's output folder, audio).");
    }

    // ---- the list
    if (!g_m.songs.empty()) {
        const float idxW = ui::Dp(40.0f), cs = ui::Dp(40.0f), rh = ui::Dp(56.0f), durW = ui::Dp(60.0f);
        const bool wide = W > ui::Dp(520.0f);
        const float madeW = wide ? ui::Dp(120.0f) : 0.0f;
        { // its headings
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const ImU32 c = ImGui::GetColorU32(ImGuiCol_TextDisabled);
            mdl->AddText(ImVec2(p.x + idxW * 0.5f - ImGui::CalcTextSize("#").x * 0.5f, p.y), c, "#");
            mdl->AddText(ImVec2(p.x + idxW + ui::Dp(4.0f), p.y), c, "Title");
            if (wide) mdl->AddText(ImVec2(p.x + W - durW - madeW, p.y), c, "Made in");
            mdl->AddText(ImVec2(p.x + W - durW + ui::Dp(8.0f), p.y), c, "Length");
            ImGui::Dummy(ImVec2(W, ImGui::GetFontSize() + ui::Dp(6.0f)));
            const ImVec2 q = ImGui::GetCursorScreenPos();
            mdl->AddLine(ImVec2(q.x, q.y - ui::Dp(2.0f)), ImVec2(q.x + W, q.y - ui::Dp(2.0f)), ImGui::GetColorU32(ImGuiCol_Separator));
        }
        ImGui::BeginChild("##songrows", ImVec2(0, 0), ImGuiChildFlags_None);
        ImDrawList* rdl = ImGui::GetWindowDrawList();
        const float RW = ImGui::GetContentRegionAvail().x;
        for (size_t k = 0; k < g_m.songs.size(); ++k) {
            const Song& s = g_m.songs[k];
            ImGui::PushID(static_cast<int>(k));
            const bool current = static_cast<int>(k) == playingAt, chosen = static_cast<int>(k) == heroAt;
            const bool busy = SongBusy(s.path);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##row", ImVec2(RW, rh));
            const bool hov = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) g_m.selected = s.path;
            if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !busy) {
                if (current && vp) vp->Toggle();
                else Play(app, s.path);
            }
            if (ImGui::BeginPopupContextItem("##rowmenu")) {
                SongMenu(app, s, current, vp);
                ImGui::EndPopup();
            }
            if (hov || chosen) rdl->AddRectFilled(p, ImVec2(p.x + RW, p.y + rh), ImGui::GetColorU32(ImGuiCol_Text, chosen ? 0.10f : 0.06f), ui::Dp(6.0f));
            // the number: a play button on hover, moving bars while it plays
            const ImVec2 ic(p.x + idxW * 0.5f, p.y + rh * 0.5f);
            if (hov && !busy) {
                const ImVec2 bp(ic.x - ui::Dp(10.0f), ic.y - ui::Dp(10.0f));
                const bool overIcon = ImGui::IsMouseHoveringRect(bp, ImVec2(bp.x + ui::Dp(20.0f), bp.y + ui::Dp(20.0f)));
                DrawIcon(rdl, current && vp && vp->Playing() ? kIcPause : kIcPlay, ic, ui::Dp(9.0f), ImGui::GetColorU32(ImGuiCol_Text));
                if (overIcon && ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                    if (current && vp) vp->Toggle();
                    else Play(app, s.path);
                }
            } else if (current && vp && vp->Playing()) {
                const float t = static_cast<float>(ImGui::GetTime());
                for (int b = 0; b < 3; ++b) {
                    const float hgt = ui::Dp(4.0f) + ui::Dp(9.0f) * (0.5f + 0.5f * std::sin(t * (5.0f + b * 1.7f) + b * 1.3f));
                    const float bx = ic.x - ui::Dp(7.0f) + b * ui::Dp(5.0f);
                    rdl->AddRectFilled(ImVec2(bx, ic.y + ui::Dp(7.0f) - hgt), ImVec2(bx + ui::Dp(3.0f), ic.y + ui::Dp(7.0f)), kGreen);
                }
            } else {
                const std::string n = std::to_string(k + 1);
                const ImVec2 ts = ImGui::CalcTextSize(n.c_str());
                rdl->AddText(ImVec2(ic.x - ts.x * 0.5f, ic.y - ts.y * 0.5f), current ? kGreen : ImGui::GetColorU32(ImGuiCol_TextDisabled), n.c_str());
            }
            // the cover, the title and under it what it is
            const std::wstring cover = CoverOf(s.name);
            Cover(app, cover, ImVec2(p.x + idxW, p.y + (rh - cs) * 0.5f), cs, ui::Dp(4.0f));
            const float tx = p.x + idxW + cs + ui::Dp(12.0f), tw = std::max(ui::Dp(40.0f), RW - (tx - p.x) - durW - madeW - ui::Dp(8.0f));
            const float lh = ImGui::GetFontSize();
            rdl->AddText(ImVec2(tx, p.y + rh * 0.5f - lh - ui::Dp(1.0f)), current ? kGreen : ImGui::GetColorU32(ImGuiCol_Text), Ellipsize(SongTitle(s), tw).c_str());
            std::error_code ec;
            std::string sub = busy ? "adding its cover and lyrics..." : fs::exists(SrtPathOf(s.path), ec) ? "Try2Create  \xC2\xB7  lyrics" : "Try2Create";
            rdl->AddText(ImVec2(tx, p.y + rh * 0.5f + ui::Dp(2.0f)), busy ? ImGui::GetColorU32(ui::kAccentHover) : ImGui::GetColorU32(ImGuiCol_TextDisabled), Ellipsize(sub, tw).c_str());
            double took = 0;
            if (wide && ComfyGenerationTime(s.path, took))
                rdl->AddText(ImVec2(p.x + RW - durW - madeW, p.y + (rh - lh) * 0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled), FormatTook(took).c_str());
            if (s.seconds > 0)
                rdl->AddText(ImVec2(p.x + RW - durW + ui::Dp(8.0f), p.y + (rh - lh) * 0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled), Clock(s.seconds).c_str());
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) && !ImGui::IsPopupOpen("##rowmenu")) ImGui::SetTooltip("Double click: play  \xC2\xB7  right click: more\n%s", s.name.c_str());
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();

    // ---- the bar at the bottom: what is playing, the controls, the volume
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y), ImGui::GetColorU32(ImGuiCol_Separator));
        const Song* now = playingAt >= 0 ? &g_m.songs[playingAt] : nullptr;
        // left: the song
        const float cs = ui::Dp(56.0f), top = p.y + (barH - cs) * 0.5f;
        const float sideW = std::clamp(w * 0.28f, ui::Dp(120.0f), ui::Dp(300.0f));
        if (now) {
            Cover(app, CoverOf(now->name), ImVec2(p.x + ui::Dp(4.0f), top), cs, ui::Dp(4.0f));
            const float tx = p.x + cs + ui::Dp(16.0f), tw = sideW - cs - ui::Dp(16.0f);
            dl->AddText(ImVec2(tx, top + ui::Dp(10.0f)), ImGui::GetColorU32(ImGuiCol_Text), Ellipsize(SongTitle(*now), tw).c_str());
            dl->AddText(ImVec2(tx, top + ui::Dp(30.0f)), ImGui::GetColorU32(ImGuiCol_TextDisabled), "Try2Create");
        } else {
            dl->AddText(ImVec2(p.x + ui::Dp(4.0f), p.y + barH * 0.5f - ImGui::GetFontSize() * 0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                        g_m.songs.empty() ? "" : "Pick a song: double click it");
        }
        // middle: the controls and where it is
        const float midW = std::max(ui::Dp(200.0f), w - sideW * 2), midX = p.x + sideW;
        const float b = ui::Dp(30.0f), play = ui::Dp(36.0f), gap = ui::Dp(14.0f);
        const float rowW = b * 4 + play + gap * 4;
        ImGui::SetCursorScreenPos(ImVec2(midX + (midW - rowW) * 0.5f, p.y + ui::Dp(10.0f)));
        const bool any = !g_m.songs.empty();
        if (IconButton("##shuffle", kIcShuffle, b, g_m.shuffle, any)) g_m.shuffle = !g_m.shuffle, Save();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip(g_m.shuffle ? "Shuffle: on" : "Shuffle");
        ImGui::SameLine(0.0f, gap);
        if (IconButton("##prev", kIcPrev, b, false, any)) PlayNext(app, -1, false);
        ImGui::SameLine(0.0f, gap);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - (play - b) * 0.5f);
        if (PlayCircle("##barplay", vp && vp->Playing(), play, ImGui::GetColorU32(ImGuiCol_Text), ui::IsLight() ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 255), any)) {
            if (vp) vp->Toggle();
            else if (heroAt >= 0) Play(app, g_m.songs[heroAt].path);
        }
        ImGui::SameLine(0.0f, gap);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (play - b) * 0.5f);
        if (IconButton("##next", kIcNext, b, false, any)) PlayNext(app, +1, false);
        ImGui::SameLine(0.0f, gap);
        const ImVec2 rp = ImGui::GetCursorScreenPos();
        if (IconButton("##repeat", kIcRepeat, b, g_m.repeat > 0, any)) {
            g_m.repeat = (g_m.repeat + 1) % 3;
            if (vp) vp->SetLoop(g_m.repeat == 2);
            Save();
        }
        if (g_m.repeat == 2) dl->AddText(ui::BoldFont(), ImGui::GetFontSize() * 0.7f, ImVec2(rp.x + b * 0.5f - ui::Dp(3.0f), rp.y + b * 0.5f - ui::Dp(6.0f)), kGreen, "1");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip(g_m.repeat == 0 ? "Repeat" : g_m.repeat == 1 ? "Repeat: all" : "Repeat: this one");
        // where it is
        const double pos = vp ? vp->Position() : 0, dur = now && now->seconds > 0 ? now->seconds : vp ? std::max(0.01, vp->Duration()) : 0;
        const std::string a = Clock(pos), z = Clock(dur);
        const float tw = ImGui::CalcTextSize("00:00").x;
        const float sy = p.y + ui::Dp(52.0f);
        dl->AddText(ImVec2(midX + ui::Dp(8.0f) + tw - ImGui::CalcTextSize(a.c_str()).x, sy), ImGui::GetColorU32(ImGuiCol_TextDisabled), a.c_str());
        ImGui::SetCursorScreenPos(ImVec2(midX + tw + ui::Dp(16.0f), sy - ui::Dp(1.0f)));
        float picked = 0;
        ImGui::BeginDisabled(!vp);
        if (SlimBar("##seek", midW - tw * 2 - ui::Dp(32.0f), dur > 0 ? static_cast<float>(pos / dur) : 0.0f, picked, false) && vp) vp->Seek(picked * dur);
        ImGui::EndDisabled();
        dl->AddText(ImVec2(midX + midW - tw - ui::Dp(8.0f), sy), ImGui::GetColorU32(ImGuiCol_TextDisabled), z.c_str());
        // right: the volume
        const float vw = std::min(ui::Dp(110.0f), sideW - ui::Dp(48.0f));
        ImGui::SetCursorScreenPos(ImVec2(p.x + w - vw - ui::Dp(36.0f), p.y + barH * 0.5f - ui::Dp(12.0f)));
        static float before = 0.9f;
        if (IconButton("##mute", g_m.volume <= 0.001f ? kIcMuted : kIcVolume, ui::Dp(24.0f))) {
            if (g_m.volume > 0.001f) before = g_m.volume, g_m.volume = 0;
            else g_m.volume = before > 0.01f ? before : 0.9f;
            if (vp) vp->SetVolume(g_m.volume);
            Save();
        }
        ImGui::SameLine(0.0f, ui::Dp(6.0f));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ui::Dp(5.0f));
        float v = g_m.volume;
        if (vw > ui::Dp(30.0f) && SlimBar("##vol", vw, v, v, true)) {
            g_m.volume = v;
            if (vp) vp->SetVolume(v);
            if (!ImGui::IsItemActive()) Save();
        }
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + barH));
    }
}

} // namespace

std::string MakeAudioPostVideo(const std::wstring& audio, const std::wstring& picture, const std::wstring& out, bool tall) {
    return MakePostVideo(audio, picture, out, tall);
}

void OpenMusicTab(AppState& app, const std::string& idea, double seconds, bool instrumental) {
    if (!g_m.loaded) Load();
    g_m.idea = idea;
    g_m.seconds = std::clamp(seconds, 10.0, 300.0);
    g_m.instrumental = instrumental;
    g_m.edited = false;
    Save();
    app.requestTab = 11;
}

void MusicFromPicture(AppState& app, const std::wstring& picture, const std::string& context) {
    if (!g_m.loaded) Load();
    g_m.photos = {picture};
    g_m.photoContext = context;
    g_m.idea.clear(); // the picture is the theme (an idea can be added and the song written again)
    g_m.instrumental = false;
    g_m.edited = false;
    Save();
    app.requestTab = 11;
    CheckModel();
    if (!g_m.writeFut.valid()) { // written at once; made when you press Make (look it over, change the length, the cover...)
        g_m.makeAfterWrite = false;
        g_m.writeFut = std::async(std::launch::async, WriteMusic, g_m.idea, g_m.seconds, g_m.instrumental, g_m.photos, g_m.photoContext, app.comfy.url);
    }
    Toast(ToastKind::Info, "A song about this picture",
          "The prompt writer looks at it and writes the music, the lyrics and a title. Look them over, pick a cover, then press Make the music",
          picture, [](AppState& s) { s.requestTab = 11; }, "music");
}

void MusicMakeForVideo(AppState& app, const std::string& idea, const std::string& context, const std::vector<std::wstring>& photos, double seconds, bool instrumental) {
    if (!g_m.loaded) Load();
    if (g_m.writeFut.valid()) return;
    g_m.idea = idea;
    g_m.photos = photos;
    g_m.photoContext = context;
    g_m.seconds = std::clamp(seconds, 10.0, 300.0);
    g_m.instrumental = instrumental;
    g_m.edited = false;
    g_m.forVideo = true;
    Save();
    CheckModel();
    g_m.makeAfterWrite = true; // (the button is the Make press)
    g_m.writeFut = std::async(std::launch::async, WriteMusic, g_m.idea, g_m.seconds, g_m.instrumental, g_m.photos, g_m.photoContext, app.comfy.url);
}

std::string MusicForVideoStatus() {
    if (g_m.forVideo && g_m.writeFut.valid()) return "Writing the words and the music...";
    if (std::any_of(g_m.pending.begin(), g_m.pending.end(), [](const PendingCover& p) { return p.forVideo; })) return "Making the song (MiniMax Music)...";
    if (g_forVideoBusy > 0) return "Timing its lyrics...";
    return {};
}

std::wstring MusicForVideoTake() {
    std::lock_guard<std::mutex> lock(g_forVideoMutex);
    return std::exchange(g_forVideoSong, std::wstring());
}

std::string MusicForVideoFailed() {
    std::lock_guard<std::mutex> lock(g_forVideoMutex);
    return std::exchange(g_forVideoFailed, std::string());
}

bool MusicPlaying() { return g_m.player && g_m.player->IsOpen() && g_m.player->Playing(); }

std::vector<MusicSongInfo> MusicSongs() {
    std::vector<MusicSongInfo> out;
    for (const Song& s : g_m.songs) out.push_back({s.path, SongTitle(s), s.seconds});
    return out;
}

void PumpMusic(AppState& app) {
    if (!g_m.loaded) Load();
    PumpCovers(app);
    { // what the lyrics passes did
        std::vector<std::pair<std::string, std::string>> notes;
        std::vector<std::pair<std::wstring, std::wstring>> named;
        {
            std::lock_guard<std::mutex> lock(g_lyricsMutex);
            notes.swap(g_lyricsNotes);
            named.swap(g_songsNamed);
        }
        for (const auto& [from, to] : named) { // a new song, under its own name: it is the one shown now
            if (from != to) SongRenamed(from, to);
            g_m.selected = to;
            g_m.scanned = -100.0;
            Save();
        }
        for (const auto& [name, note] : notes) {
            if (note.empty()) continue;
            g_lyricsDone.erase(std::remove_if(g_lyricsDone.begin(), g_lyricsDone.end(), [&](const auto& x) { return x.first == name; }), g_lyricsDone.end());
            g_lyricsDone.push_back({name, note});
            Toast(note.rfind("timed", 0) == 0 ? ToastKind::Success : ToastKind::Info, "Lyrics: " + name, note, {}, [](AppState& st) { st.requestTab = 11; }, "lyrics");
        }
    }
    // MV_MUSIC_POST "<song.mp3>|<cover>|<out.mp4>": Video for posting, once the lyrics pass is done (tests)
    static bool postTest = false;
    wchar_t post[2048] = {};
    if (!postTest && g_lyricsBusy == 0 && ImGui::GetTime() > 3.0 && GetEnvironmentVariableW(L"MV_MUSIC_POST", post, 2048) && !g_m.postFut.valid()) {
        postTest = true;
        const std::wstring all = post;
        const size_t a = all.find(L'|'), b = all.find(L'|', a + 1);
        if (a != std::wstring::npos && b != std::wstring::npos)
            g_m.postFut = std::async(std::launch::async, MakePostVideo, all.substr(0, a), all.substr(a + 1, b - a - 1), all.substr(b + 1), false);
    }
    // MV_MUSIC_PLAY "<song>|<seconds in>": plays it once the list is read (tests)
    static bool playTest = false;
    wchar_t mp[1024] = {};
    if (!playTest && !g_m.songs.empty() && GetEnvironmentVariableW(L"MV_MUSIC_PLAY", mp, 1024)) {
        playTest = true;
        std::wstring all = mp;
        const size_t bar = all.find(L'|');
        Play(app, all.substr(0, bar));
        if (bar != std::wstring::npos && g_m.player) g_m.player->Seek(_wtof(all.c_str() + bar + 1));
    }
    // MV_COVER_TEST "<picture or ->|<title>|<second line>|<out dir>": every cover template, as cover_<n>.png (tests)
    static bool coverTest = false;
    wchar_t ct[2048] = {};
    if (!coverTest && GetEnvironmentVariableW(L"MV_COVER_TEST", ct, 2048)) {
        coverTest = true;
        std::vector<std::wstring> f;
        std::wstring all = ct;
        for (size_t a = 0; a <= all.size();) {
            const size_t b = std::min(all.find(L'|', a), all.size());
            f.push_back(all.substr(a, b - a));
            a = b + 1;
        }
        if (f.size() >= 4)
            std::thread([f]() {
                for (int t = 0; t < CoverTemplateCount(); ++t) {
                    const std::string err = RenderCoverTemplate(t, f[0] == L"-" ? std::wstring() : f[0], Utf8(f[1]), Utf8(f[2]), f[3] + L"\\cover_" + std::to_wstring(t) + L".png", 540);
                    std::ofstream(f[3] + L"\\cover_log.txt", std::ios::app) << t << " " << CoverTemplateName(t) << ": " << (err.empty() ? "ok" : err) << "\n";
                }
            }).detach();
    }
    // MV_MUSIC_LYRICS "<song.mp3>": the lyrics pass on it, as its Lyrics button does (tests)
    static bool lyricsTest = false;
    wchar_t lyr[1024] = {};
    if (!lyricsTest && GetEnvironmentVariableW(L"MV_MUSIC_LYRICS", lyr, 1024)) {
        lyricsTest = true;
        RunLyricsPass(lyr, Utf8(fs::path(lyr).filename().wstring()));
    }
    if (g_m.postFut.valid() && g_m.postFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const std::string r = g_m.postFut.get();
        if (r.rfind("error:", 0) == 0) Toast(ToastKind::Error, "Could not make the video", r.substr(7), {}, {}, "music");
        else Toast(ToastKind::Success, "Video for posting ready", r, {}, [](AppState& s) { s.requestTab = 10; }, "music");
    }
    if (!app.TabShown(11) && g_m.player && g_m.player->Playing()) g_m.player->Pause(); // the song stops when you leave the tab
    // the writer's answer
    if (g_m.writeFut.valid() && g_m.writeFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const Written w = g_m.writeFut.get();
        if (w.error.empty()) {
            g_m.caption = w.caption;
            g_m.lyrics = w.lyrics;
            if (!w.title.empty()) g_m.title = w.title;
            if (!w.cover.empty()) g_m.cover = w.cover;
            g_m.edited = false;
            g_m.writtenFor = WriteKey();
            g_m.writeNote.clear();
            Save();
            if (g_m.makeAfterWrite) Make(app);
        } else {
            g_m.writeNote = w.error;
            Toast(ToastKind::Error, "Could not write the music", g_m.writeNote, {}, [](AppState& s) { s.requestTab = 11; }, "music");
            if (g_m.forVideo) {
                g_m.forVideo = false;
                std::lock_guard<std::mutex> lock(g_forVideoMutex);
                g_forVideoFailed = w.error;
            }
        }
        g_m.makeAfterWrite = false;
    }
    // MV_MUSIC_PICTURE "<picture>": a song about it, as Make a song... does (tests)
    static bool picDone = false;
    wchar_t wpic[1024] = {};
    if (!picDone && app.comfyLoaded && GetEnvironmentVariableW(L"MV_MUSIC_PICTURE", wpic, 1024)) { // several: "a.png|b.png|..."
        picDone = true;
        std::wstring all = wpic;
        std::vector<std::wstring> list;
        for (size_t a = 0; a <= all.size();) {
            const size_t b = std::min(all.find(L'|', a), all.size());
            if (b > a) list.push_back(all.substr(a, b - a));
            a = b + 1;
        }
        if (list.size() == 1) {
            MusicFromPicture(app, list.front(), std::string());
        } else if (!list.empty() && !g_m.writeFut.valid()) { // the whole set, as one setting
            g_m.photos = list;
            g_m.photoContext.clear();
            g_m.idea.clear();
            g_m.edited = false;
            app.requestTab = 11;
            CheckModel();
            g_m.makeAfterWrite = g_m.have;
            g_m.writeFut = std::async(std::launch::async, WriteMusic, g_m.idea, g_m.seconds, g_m.instrumental, g_m.photos, g_m.photoContext, app.comfy.url);
        }
    }
    // MV_MUSIC_TEST "<idea>|<seconds>|<instrumental 0/1>": writes and makes one, once (tests)
    static bool testDone = false;
    char buf[1024] = {};
    if (!testDone && GetEnvironmentVariableA("MV_MUSIC_TEST", buf, sizeof(buf))) {
        testDone = true;
        std::string s = buf;
        const size_t a = s.find('|'), b = a == std::string::npos ? std::string::npos : s.find('|', a + 1);
        g_m.idea = s.substr(0, a);
        if (a != std::string::npos) g_m.seconds = std::clamp(std::atof(s.c_str() + a + 1), 10.0, 300.0);
        if (b != std::string::npos) g_m.instrumental = s[b + 1] == '1';
        g_m.makeAfterWrite = true;
        CheckModel();
        g_m.writeFut = std::async(std::launch::async, WriteMusic, g_m.idea, g_m.seconds, g_m.instrumental, g_m.photos, g_m.photoContext, app.comfy.url);
    }
}

void DrawMusicTab(AppState& app, float height) {
    if (!g_m.loaded) Load();
    if (ImGui::GetTime() - g_m.checked > (g_m.downloading ? 5.0 : 30.0)) {
        g_m.checked = ImGui::GetTime();
        CheckModel();
    }
    // the songs: looked at again every 15 s, and when a music job is done
    int doneNow = 0;
    if (app.comfySvc) {
        for (const ComfyActivity& a : app.comfySvc->Activity()) doneNow += a.state == ComfyState::Done && a.label.rfind("Music:", 0) == 0 ? 1 : 0;
    }
    if (!g_m.scanFut.valid() && (ImGui::GetTime() - g_m.scanned > 15.0 || doneNow != g_m.doneSeen)) {
        g_m.scanned = ImGui::GetTime();
        g_m.doneSeen = doneNow;
        const std::wstring out = ComfyOutputDir(app.comfy);
        g_m.scanFut = std::async(std::launch::async, ScanSongs, std::vector<std::wstring>{out.empty() ? std::wstring() : out + L"\\audio", DataDir() + L"\\comfy_results"});
    }
    if (g_m.scanFut.valid() && g_m.scanFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) g_m.songs = g_m.scanFut.get();

    ImGui::BeginChild("##music", ImVec2(0, height), false);
    const float avail = ImGui::GetContentRegionAvail().x;
    const float leftW = std::clamp(avail * 0.5f, std::min(avail, ui::Dp(420.0f)), ui::Dp(760.0f));
    ImGui::BeginChild("##musicleft", ImVec2(leftW, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    { // MV_MUSIC_SCROLL "<px>": the left side scrolled that far once (tests)
        static int scrolled = 0;
        char sc[32] = {};
        if (scrolled < 3 && GetEnvironmentVariableA("MV_MUSIC_SCROLL", sc, sizeof(sc)) && ImGui::GetTime() > 2.0) ++scrolled, ImGui::SetScrollY(static_cast<float>(std::atof(sc)));
    }
    ImGui::PushFont(ui::TitleFont());
    ImGui::TextUnformatted("Music");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, ui::kTextDim);
    ImGui::TextWrapped("Songs and soundtracks with MiniMax Music 3: say what you want, and the prompt writer turns it into a description of the music and "
                       "lyrics with section tags. Up to 5 minutes, with or without singing.");
    ImGui::PopStyleColor();
    bool changed = false;

    // the model
    if (!g_m.have) {
        ImGui::Spacing();
        if (!g_m.known) {
            ImGui::TextColored(ui::kWarn, "ComfyUI's models folder was not found: MiniMax Music 3 needs %s, %s and %s there.", kDit, kTextEncoder, kVae);
        } else if (g_m.downloading) {
            ImGui::TextColored(ui::kAccentHover, "Downloading MiniMax Music 3: %.1f of about 12 GB...", DownloadedGb());
        } else {
            if (ui::PrimaryButton("Get MiniMax Music 3 (about 12 GB)", ImVec2(ui::Dp(280.0f), ui::Dp(34.0f)))) StartDownload();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("From Comfy-Org/MiniMax-Music-3 on Hugging Face into ComfyUI's models folder:\n%s (2.5 GB), %s (9.2 GB), %s (0.2 GB).\nIt resumes if stopped.",
                                  kDit, kTextEncoder, kVae);
            if (g_m.failed) {
                ImGui::SameLine();
                ImGui::TextColored(ui::kBad, "The last download failed: try again.");
            }
        }
    }

    ui::SectionHeading("WHAT MUSIC");
    if (WrapEdit("##musicidea", g_m.idea, ui::Dp(54.0f))) changed = true;
    if (g_m.idea.empty()) ImGui::TextDisabled("e.g. \"a sad piano piece for a breakup scene\" or \"a summer pop song about a road trip\"");
    for (size_t k = 0; k < sizeof(kStyles) / sizeof(kStyles[0]); ++k) {
        if (k) ImGui::SameLine(0.0f, ui::Dp(4.0f));
        if (ImGui::GetCursorPosX() + ImGui::CalcTextSize(kStyles[k].name).x + ui::Dp(20.0f) > ImGui::GetContentRegionMax().x) ImGui::NewLine();
        if (Chip(kStyles[k].name, g_m.idea == kStyles[k].idea)) g_m.idea = kStyles[k].idea, changed = true;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", kStyles[k].idea);
    }
    // pictures the song is about (optional): one, a few, or a whole Vision board; the writer looks at them, and the cover is made from them
    {
        ImGui::Spacing();
        ImGui::TextUnformatted(g_m.photos.empty() ? "Pictures it is about (optional)" : "Pictures it is about");
        ImGui::SameLine();
        ImGui::TextDisabled(WriterCanSee() ? "the prompt writer looks at them: the people, the places, the story, the mood"
                                           : "the prompt writer goes by what their prompts say (it cannot see)");
        const float sz = ui::Dp(64.0f);
        int remove = -1;
        for (size_t k = 0; k < g_m.photos.size(); ++k) {
            ImGui::PushID(static_cast<int>(k));
            if (k) ImGui::SameLine(0.0f, ui::Dp(4.0f));
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##ph", ImVec2(sz, sz));
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, ImVec2(p.x + sz, p.y + sz), ui::ThumbPlaceholder(), ui::Dp(6.0f));
            const AppState::LibThumb lt = app.LibraryThumb(g_m.photos[k]);
            if (lt.srv && lt.w > 0 && lt.h > 0) {
                const float s2 = std::min(sz / lt.w, sz / lt.h);
                const ImVec2 a2(p.x + (sz - lt.w * s2) * 0.5f, p.y + (sz - lt.h * s2) * 0.5f);
                dl->AddImage((ImTextureID)(intptr_t)lt.srv, a2, ImVec2(a2.x + lt.w * s2, a2.y + lt.h * s2));
            }
            if (ImGui::IsItemHovered()) {
                dl->AddText(ImVec2(p.x + sz - ui::Dp(14.0f), p.y + ui::Dp(2.0f)), IM_COL32(255, 255, 255, 230), "x");
                ImGui::SetTooltip("%s\nClick: take it out", Utf8(fs::path(g_m.photos[k]).filename().wstring()).c_str());
            }
            if (ImGui::IsItemClicked()) remove = static_cast<int>(k);
            ImGui::PopID();
        }
        if (remove >= 0) g_m.photos.erase(g_m.photos.begin() + remove), changed = true;
        auto add = [&](const std::wstring& path, const std::string& context) {
            if (path.empty() || g_m.photos.size() >= 8 || std::find(g_m.photos.begin(), g_m.photos.end(), path) != g_m.photos.end()) return;
            g_m.photos.push_back(path);
            if (!context.empty()) g_m.photoContext += (g_m.photoContext.empty() ? "" : " | ") + context.substr(0, 300);
            changed = true;
        };
        const bool haveSel = app.selectedIndex >= 0 && app.selectedIndex < static_cast<int>(app.entries.size()) && !app.entries[app.selectedIndex].metadata.isVideo;
        ImGui::BeginDisabled(!haveSel);
        if (ImGui::SmallButton("+ Gallery's")) add(app.entries[app.selectedIndex].path, app.entries[app.selectedIndex].metadata.parsed.positivePrompt);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("The picture selected in the gallery");
        ImGui::SameLine();
        if (ImGui::SmallButton("+ File...")) {
            wchar_t buf[MAX_PATH * 2] = {};
            OPENFILENAMEW ofn = {sizeof(ofn)};
            ofn.hwndOwner = GetActiveWindow();
            ofn.lpstrFilter = L"Pictures\0*.png;*.jpg;*.jpeg;*.webp;*.bmp\0All files\0*.*\0";
            ofn.lpstrFile = buf;
            ofn.nMaxFile = MAX_PATH * 2;
            ofn.lpstrTitle = L"A picture the song is about";
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameW(&ofn)) add(buf, std::string());
        }
        // the Vision board: its pictures and what it says (cards, relations, its written prompt), as one setting
        const Board& board = app.board.board;
        const bool boardHas = std::any_of(board.nodes.begin(), board.nodes.end(), [](const BoardNode& n) { return !n.image.empty(); });
        ImGui::SameLine();
        ImGui::BeginDisabled(!boardHas);
        if (ImGui::SmallButton("From the Vision board")) {
            g_m.photos.clear();
            g_m.photoContext.clear();
            std::string words;
            for (const BoardNode& n : board.nodes) {
                if (!n.image.empty()) add(n.image, std::string());
                const std::string name = board.DisplayName(n);
                if (!name.empty() || !n.text.empty()) words += (words.empty() ? "" : "; ") + name + (n.text.empty() ? std::string() : ": " + n.text.substr(0, 200));
            }
            for (const BoardLink& l : board.links) {
                const BoardNode* f = board.Find(l.from);
                const BoardNode* t = board.Find(l.to);
                if (f && t) words += "; " + board.DisplayName(*f) + " " + l.label + " " + board.DisplayName(*t);
            }
            if (!app.board.result.empty()) words += ". The scene: " + app.board.result.substr(0, 600);
            g_m.photoContext = words;
            if (g_m.idea.empty() && !app.board.name.empty()) g_m.idea = "A song about \"" + app.board.name + "\"";
            changed = true;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(boardHas ? "The Vision board as one setting: its pictures, its cards and relations, and its scene" : "The Vision board has no pictures yet");
        if (!g_m.photos.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear##photos")) g_m.photos.clear(), g_m.photoContext.clear(), changed = true;
        }
    }
    if (ImGui::Checkbox("Instrumental (no singing)", &g_m.instrumental)) changed = true;
    float secs = static_cast<float>(g_m.seconds);
    ImGui::SetNextItemWidth(ui::Dp(260.0f));
    if (ImGui::SliderFloat("##musicsecs", &secs, 10.0f, 300.0f, "at most %.0f seconds")) g_m.seconds = std::round(secs / 5.0f) * 5.0, changed = true;
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", Clock(g_m.seconds).c_str());

    // what goes to the model
    const bool writing = g_m.writeFut.valid();
    const bool stale = !g_m.edited && !g_m.caption.empty() && g_m.writtenFor != WriteKey();
    ui::SectionHeading("DESCRIPTION AND LYRICS");
    ImGui::BeginDisabled(writing);
    if (ImGui::SmallButton(g_m.caption.empty() ? "Write them now" : "Write them again")) {
        g_m.makeAfterWrite = false;
        g_m.writeFut = std::async(std::launch::async, WriteMusic, g_m.idea, g_m.seconds, g_m.instrumental, g_m.photos, g_m.photoContext, app.comfy.url);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (writing) ImGui::TextColored(ui::kAccentHover, "The prompt writer is writing them...");
    else if (stale) ImGui::TextColored(ui::kTextDim, "Out of date: written again when you press Make.");
    else if (g_m.caption.empty()) ImGui::TextDisabled("Written when you press Make (or write your own).");
    ImGui::TextDisabled("Description (Global Metadata, Vocal Details, Arrangement)");
    if (WrapEdit("##caption", g_m.caption, ui::Dp(110.0f))) g_m.edited = true, changed = true;
    ImGui::TextDisabled("Lyrics ([intro] [verse] [chorus] [bridge] [instrumental] [outro])");
    if (WrapEdit("##lyrics", g_m.lyrics, ui::Dp(140.0f))) g_m.edited = true, changed = true;
    // its title and cover
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Title");
    ImGui::SameLine();
    {
        char tb[160] = {};
        snprintf(tb, sizeof(tb), "%s", g_m.title.c_str());
        ImGui::SetNextItemWidth(ui::Dp(300.0f));
        if (ImGui::InputTextWithHint("##songtitle", "written with the lyrics", tb, sizeof(tb))) g_m.title = tb, changed = true;
    }
    // its cover: it goes into the song's file, so players and phones show it
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Cover");
        const bool canDesign = CoverTemplatesAvailable(), hasPhoto = !g_m.photos.empty();
        struct Opt {
            int mode;
            const char* label;
            const char* tip;
            bool ok;
        };
        const Opt opts[] = {
            {kCoverDesign, "Design", "Your picture with the title designed on it (PhotoCraft templates, no AI, about a second).\nWithout a picture: the template's colours.",
             canDesign},
            {kCoverPicture, "Picture as is", "The song's first picture, squared, nothing added", hasPhoto},
            {kCoverAi, "AI drawn", "Qwen Image draws a new cover from the song's pictures, with the title on it (a minute or so on the GPU)", true},
            {kCoverNone, "None", "No cover", true},
        };
        for (const Opt& o : opts) {
            ImGui::SameLine(0.0f, ui::Dp(4.0f));
            ImGui::BeginDisabled(!o.ok);
            if (Chip(o.label, g_m.coverMode == o.mode)) g_m.coverMode = o.mode, changed = true;
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s%s", o.tip, o.ok ? "" : o.mode == kCoverDesign ? "\n\nPhotoCraft is not set up yet (Help > Set up Try2Create, Helpers)." : "\n\nAdd a picture first.");
        }
        // which picture the cover is made from (when there are several)
        if ((g_m.coverMode == kCoverDesign || g_m.coverMode == kCoverPicture) && g_m.photos.size() > 1) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("made from");
            const float ps = ui::Dp(30.0f);
            for (size_t k = 0; k < g_m.photos.size(); ++k) {
                ImGui::SameLine(0.0f, ui::Dp(4.0f));
                ImGui::PushID(static_cast<int>(k) + 9000);
                const ImVec2 p = ImGui::GetCursorScreenPos();
                if (ImGui::InvisibleButton("##cp", ImVec2(ps, ps))) g_m.coverPhoto = static_cast<int>(k), changed = true;
                Cover(app, g_m.photos[k], p, ps, ui::Dp(4.0f));
                const bool on = static_cast<int>(k) == std::clamp(g_m.coverPhoto, 0, static_cast<int>(g_m.photos.size()) - 1);
                if (on) ImGui::GetWindowDrawList()->AddRect(p, ImVec2(p.x + ps, p.y + ps), kGreen, ui::Dp(4.0f), 0, ui::Dp(2.0f));
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Make the cover from this picture");
                ImGui::PopID();
            }
        }
        if (g_m.coverMode == kCoverAi) {
            ImGui::TextDisabled("What the cover shows, the style");
            if (WrapEdit("##cover", g_m.cover, ui::Dp(54.0f))) changed = true;
        } else if (g_m.coverMode == kCoverDesign && canDesign) {
            // every design, drawn small with this picture and title: click one
            const std::wstring photo = CoverPhoto();
            const std::string key = Utf8(photo) + "|" + g_m.title + "|" + CoverLine2();
            if (key != g_m.previewKey) {
                if (g_m.previewAsked < 0) g_m.previewAsked = ImGui::GetTime();
                if (!g_m.previewFut.valid() && ImGui::GetTime() - g_m.previewAsked > 0.8) { // once typing stops
                    g_m.previewKey = key;
                    g_m.previewAsked = -1;
                    const std::wstring dir = DataDir() + L"\\cover_previews";
                    g_m.previewStem = dir + L"\\" + std::to_wstring(std::hash<std::string>{}(key) % 100000000ull) + L"_";
                    g_m.previewDone = std::make_shared<std::atomic<int>>(0);
                    const std::string title = g_m.title.empty() ? std::string("Untitled") : g_m.title, line2 = CoverLine2();
                    g_m.previewFut = std::async(std::launch::async, [dir, stem = g_m.previewStem, done = g_m.previewDone, photo, title, line2]() {
                        std::error_code ec;
                        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) { // the older ones go
                            if (it->path().wstring().rfind(stem, 0) != 0) fs::remove(it->path(), ec);
                        }
                        const std::wstring base = stem + L"base.png";
                        const bool squared = !photo.empty() && SquarePicture(photo, base, 600).empty();
                        for (int t = 0; t < CoverTemplateCount(); ++t) {
                            RenderCoverTemplate(t, squared ? base : std::wstring(), title, line2, stem + std::to_wstring(t) + L".png", 300);
                            ++*done;
                        }
                    });
                }
            }
            if (g_m.previewFut.valid() && g_m.previewFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) g_m.previewFut.get();
            const float ts = ui::Dp(78.0f), gap = ui::Dp(6.0f);
            const int done = g_m.previewDone ? g_m.previewDone->load() : 0;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            for (int t = 0; t < CoverTemplateCount(); ++t) {
                ImGui::PushID(t + 7000);
                if (t > 0 && ImGui::GetItemRectMax().x + gap + ts < ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x) ImGui::SameLine(0.0f, gap);
                ImGui::BeginGroup();
                const ImVec2 p = ImGui::GetCursorScreenPos();
                if (ImGui::InvisibleButton("##tpl", ImVec2(ts, ts))) g_m.coverTemplate = t, changed = true;
                const bool hov = ImGui::IsItemHovered();
                dl->AddRectFilled(p, ImVec2(p.x + ts, p.y + ts), ui::ThumbPlaceholder(), ui::Dp(6.0f));
                const AppState::LibThumb lt = t < done ? app.LibraryThumb(g_m.previewStem + std::to_wstring(t) + L".png") : AppState::LibThumb{};
                if (lt.srv) dl->AddImageRounded((ImTextureID)(intptr_t)lt.srv, p, ImVec2(p.x + ts, p.y + ts), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, ui::Dp(6.0f));
                else dl->AddText(ImVec2(p.x + ui::Dp(8.0f), p.y + ts * 0.5f - ui::Dp(7.0f)), ImGui::GetColorU32(ImGuiCol_TextDisabled), "drawing...");
                if (g_m.coverTemplate == t) dl->AddRect(p, ImVec2(p.x + ts, p.y + ts), IM_COL32(30, 215, 96, 255), ui::Dp(6.0f), 0, ui::Dp(3.0f));
                else if (hov) dl->AddRect(p, ImVec2(p.x + ts, p.y + ts), ImGui::GetColorU32(ImGuiCol_Text, 0.5f), ui::Dp(6.0f), 0, ui::Dp(1.5f));
                if (hov && lt.srv) { // bigger
                    ImGui::BeginTooltip();
                    ImGui::Image((ImTextureID)(intptr_t)lt.srv, ImVec2(ui::Dp(260.0f), ui::Dp(260.0f)));
                    ImGui::TextUnformatted(CoverTemplateName(t));
                    ImGui::EndTooltip();
                }
                const char* nm = CoverTemplateName(t);
                const float nw = ImGui::CalcTextSize(nm).x;
                ImGui::SetCursorScreenPos(ImVec2(p.x + std::max(0.0f, (ts - nw) * 0.5f), p.y + ts + ui::Dp(2.0f)));
                if (g_m.coverTemplate == t) ImGui::TextColored(ImVec4(0.12f, 0.84f, 0.38f, 1.0f), "%s", nm);
                else ImGui::TextDisabled("%s", nm);
                ImGui::EndGroup();
                ImGui::PopID();
            }
            if (!hasPhoto) ImGui::TextDisabled("No picture: the designs use their own colours. Add one above to design on it.");
        }
    }

    ui::SectionHeading("MAKE");
    // the lyrics in the song's file are timed to the singing with a whisper.cpp model (ffmpeg's whisper filter)
    {
        static std::atomic<bool> whisperDl{false}, whisperFailed{false};
        static double lookedAt = -1000;
        static bool haveWhisper = false;
        if (ImGui::GetTime() - lookedAt > (whisperDl ? 3.0 : 20.0)) lookedAt = ImGui::GetTime(), haveWhisper = !WhisperModelPath().empty();
        if (!haveWhisper && !WhisperModelDir().empty()) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Lyrics timed to the singing (synced lyrics, subtitles):");
            ImGui::SameLine();
            if (whisperDl) {
                std::error_code ec;
                const std::wstring part = WhisperModelDir() + L"\\ggml-small.bin.part";
                ImGui::TextColored(ui::kAccentHover, "%.0f of 488 MB...", fs::exists(part, ec) ? fs::file_size(part, ec) / 1e6 : 0.0);
            } else if (ImGui::SmallButton(whisperFailed ? "Get it (failed: retry)" : "Get it (488 MB)")) {
                whisperDl = true;
                whisperFailed = false;
                std::thread([]() {
                    const std::wstring dir = WhisperModelDir(), dest = dir + L"\\ggml-small.bin";
                    std::error_code ec;
                    fs::create_directories(dir, ec);
                    std::string out;
                    RunCaptureHidden(L"curl.exe -L --fail --retry 5 --retry-delay 10 -C - --no-progress-meter -o \"" + dest + L".part\" \"" + Wide(WhisperModelUrl()) + L"\"",
                                     out, true);
                    if (fs::exists(dest + L".part", ec) && fs::file_size(dest + L".part", ec) > 400000000) fs::rename(dest + L".part", dest, ec);
                    else whisperFailed = true;
                    whisperDl = false;
                }).detach();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("whisper.cpp's small model (ggerganov/whisper.cpp on Hugging Face) into ComfyUI's models\\whisper.\n"
                                  "Without it the lyrics still go into the MP3, but not timed.");
        }
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Steps");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ui::Dp(140.0f));
    if (ImGui::SliderInt("##musicsteps", &g_m.steps, 10, 60)) changed = true;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("30 is the template's; fewer is faster and rougher");
    ImGui::SameLine();
    ImGui::TextDisabled("  How many");
    for (int n : {1, 2, 4}) {
        ImGui::SameLine();
        char lbl[8];
        snprintf(lbl, sizeof(lbl), "%d##mt%d", n, n);
        if (Chip(lbl, g_m.times == n)) g_m.times = n, changed = true;
    }
    ImGui::Spacing();
    const bool needWrite = !g_m.edited && (g_m.caption.empty() || stale);
    ImGui::BeginDisabled(!g_m.have || writing || (needWrite && g_m.idea.empty() && g_m.caption.empty()));
    if (ui::PrimaryButton(needWrite ? "Write it and make" : "Make the music", ImVec2(ui::Dp(200.0f), ui::Dp(38.0f)))) {
        if (needWrite) {
            g_m.makeAfterWrite = true;
            g_m.writeFut = std::async(std::launch::async, WriteMusic, g_m.idea, g_m.seconds, g_m.instrumental, g_m.photos, g_m.photoContext, app.comfy.url);
        } else {
            Make(app);
        }
    }
    ImGui::EndDisabled();
    // what is being made
    if (app.comfySvc) {
        const long long now = ComfyService::NowMs();
        for (const ComfyActivity& a : app.comfySvc->Activity()) {
            if (a.label.rfind("Music:", 0) != 0 || a.state == ComfyState::Done || (a.state == ComfyState::Failed && now - a.finishedMs > 60000)) continue;
            ImGui::PushID(a.id);
            const long long secsGoing = std::max(0LL, (a.state == ComfyState::Failed ? a.finishedMs : now) - (a.runningMs > 0 ? a.runningMs : a.startedMs)) / 1000;
            if (a.state == ComfyState::Failed) ImGui::TextColored(ui::kBad, "%s", a.message.c_str());
            else {
                ImGui::TextColored(ui::kAccentHover, "%s %lld:%02lld", a.state == ComfyState::Running ? "Making" : "Waiting", secsGoing / 60, secsGoing % 60);
                ImGui::SameLine();
                if (ImGui::SmallButton(a.state == ComfyState::Running ? "Stop" : "Cancel")) app.comfySvc->Cancel(a.id);
            }
            ImGui::PopID();
        }
    }
    if (changed) Save();
    ImGui::EndChild();

    // ---- the right side: your songs, played like in a music app
    ImGui::SameLine();
    ImGui::BeginChild("##musicright", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    DrawPlayerPane(app);
    ImGui::EndChild();
    ImGui::EndChild();
}
