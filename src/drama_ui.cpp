// The Drama tab: audio stories, comedy and action scenes with many speakers, performed by Fish Audio Drama 3 in ComfyUI
// (github.com/PedroMarinhoDev/ComfyUI-Drama3: a 35B text-to-speech model whose experts stay in system RAM).
//   - the prompt writer writes the scene: a title, a cast (each with a voice description) and the lines, with Drama 3's cues
//     ([laughs] [whispers] [pause]..., a delivery cue at the start of a line, <whisper>...</whisper>)
//   - every character gets a voice: one the model invents from the description (a new one with every seed), one kept from an
//     audition (three takes of a sample line, keep the one you like: the same voice from then on), a saved voice, or your own clip
//     (Qwen3-ASR writes its transcript, which you can correct)
//   - the script's names become <|speaker:N|> tags; kept voices go along as references, so each speaker sounds like their voice
//   - the performance comes back as an MP3 with its lines in it (timed by whisper, with who says them) and can become a video for posting
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "app_state.h"
#include "clef.h"
#include "comfy.h"
#include "imgui.h"
#include "json.hpp"
#include "music_lyrics.h"
#include "notify.h"
#include "ui_extra.h"
#include "ui_theme.h"
#include "video_player.h"
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
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Clock(double s) {
    const int t = static_cast<int>(std::max(0.0, s));
    char buf[16];
    snprintf(buf, sizeof(buf), "%d:%02d", t / 60, t % 60);
    return buf;
}

// what is spoken: the cues ([laughs], <whisper>) taken out
std::string Spoken(const std::string& line) {
    static const std::regex tags(R"(\[[^\]]*\]|<[^>]*>)");
    std::string s = std::regex_replace(line, tags, " ");
    std::string out;
    bool space = false;
    for (char c : s) {
        if (c == ' ' || c == '\t') {
            space = !out.empty();
            continue;
        }
        if (space) out += ' ';
        space = false;
        out += c;
    }
    return out;
}

// ---- the model's files
struct Variant {
    const char* key;  // for the setup script
    const char* file;
    const char* label;
    double gb, ramGb;
    const char* note;
};
const Variant kVariants[] = {
    {"int8", "drama3_int8experts_bf16.safetensors", "int8", 38.7, 48, "as close to the original as FP8 and the fastest on an RTX 3090 (no silent takes in 12 cloned test takes)"},
    {"w6a8", "drama3_w6a8experts_bf16.safetensors", "W6A8", 32.7, 40, "less RAM (no silent takes in 16 cloned test takes)"},
    {"w4a8", "drama3_w4a8experts_bf16.safetensors", "W4A8", 24.6, 32, "the least RAM; falls silent more often with real voice clips"},
    {"fp8", "drama3_fp8experts_bf16.safetensors", "FP8", 38.6, 48, "Fish Audio's own weights byte for byte, a little slower than int8 on a 3090"},
};
const char* const kCodec = "drama3_codec.safetensors";
// Since 2026-10-26 (the user: "use a lower quant version ... make it work the fastest it can"): Drama 3 runs on audio.cpp
// (github.com/0xShug0/audio.cpp, its native server) from Jmica/Drama-3-GGUF's Q3 experts / Q8 backbone file, 18.9 GiB - on a 24 GB card
// wholly on the GPU (Q4's 22.9 GiB would put experts on the CPU). ComfyUI only runs Try2Create's small Drama3GGUFGenerate node
// (tools\drama3gguf), which hands the script to that server: ComfyUI no longer loads the 38.7 GB model and keeps no 35 GB of RAM.
const char* const kGguf = "drama-3-backbone-8bit-experts-Q3_K_S.gguf";
constexpr double kGgufGb = 20.3;

std::wstring ToolsDir() {
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    return (fs::path(buf).parent_path() / L"tools").wstring();
}
std::wstring AudiocppExe() { return ToolsDir() + L"\\audiocpp\\audiocpp_server.exe"; }

double SystemRamGb() {
    MEMORYSTATUSEX m = {sizeof(m)};
    return GlobalMemoryStatusEx(&m) ? m.ullTotalPhys / (1024.0 * 1024 * 1024) : 0;
}

// ---- genres: what the writer is asked for
struct Genre {
    const char* name;
    const char* brief;
};
const Genre kGenres[] = {
    {"Comedy", "a comedy sketch: a funny premise that escalates, quick back and forth, misunderstandings, characters reacting to each other, laughs in the cues, a punchline at the end"},
    {"Action", "an action scene: urgent and breathless, people shouting over chaos, panting, short commands, a plan going wrong, a twist, the stakes clear from what they say"},
    {"Drama", "a dramatic confrontation: tension, a secret coming out, pauses that hurt, emotion building to a breaking point, a quiet last line"},
    {"TikTok story", "a viral TikTok-style drama: a hook in the first line, rich versus poor or a betrayal, a jaw-dropping reveal, every line a little cliffhanger, a satisfying comeback at the end"},
    {"Horror", "a horror scene: whispers, dread, something slowly wrong, nervous laughs, a sudden scream, a chilling last line"},
    {"Romance", "a romantic scene: nervous, warm, teasing, hesitations and soft laughs, a confession"},
    {"Mystery", "a noir mystery: a detective, suspects, clipped tough-guy lines, lies and a reveal"},
    {"Sci-fi", "a sci-fi scene: a ship's crew in trouble, a calm AI voice, technical shouting, a decision"},
    {"Fantasy party", "a fantasy adventure like a tabletop role-playing session: a dramatic game master and players who do silly things, dice luck, chaos"},
    {"Bedtime story", "a gentle bedtime story told by one storyteller who does little voices for the characters, cozy, slow, with a sweet ending"},
    {"Podcast", "two or three podcast hosts bantering about a ridiculous topic, laughing, interrupting, going off on tangents"},
    {"Sports call", "live sports commentary: two commentators building up a wild last-minute play, shouting, screaming with joy"},
    {"Commercial", "a parody radio commercial: an overexcited announcer, testimonials from customers, a fast legal disclaimer at the end"},
};

const char* const kDramaSystem =
    "You write short audio scenes that Fish Audio Drama 3, an expressive text-to-speech model, performs with a different voice for every character.\n"
    "Answer in exactly this format and nothing else:\n"
    "TITLE: a short catchy title\n"
    "CAST:\n"
    "Name | man, woman, boy or girl FIRST, then age, accent, timbre, pace and attitude of the voice (e.g. Hank | man in his sixties, deep and gravelly, "
    "slow Southern drawl, unbothered)\n"
    "(one line per character: {SPEAKERS}; always start the voice with man, woman, boy or girl - without it the voice can change from line to line)\n"
    "SCRIPT:\n"
    "Name: what they say\n"
    "(one line per turn)\n\n"
    "How the performance works, so write for it:\n"
    "- Only voices come out: no music, no sound effects, no narrator unless the cast has one. Make the action clear through what people say "
    "and the sounds people make.\n"
    "- Put cues in square brackets exactly where the sound happens: [laughs] [chuckles] [giggles] [sighs] [gasps] [pause] [long pause] [whispers] "
    "[shouting] [screaming] [crying] [sobbing] [sniffs] [clears throat] [coughs] [groans] [panting] [yawning] [nervous laugh] [scoffs] "
    "[inhales sharply] [mumbling] [sneezes]; a short plain-English direction works too ([sarcastic], [terrified, barely breathing]).\n"
    "- A delivery cue at the very start of a line sets how the whole line sounds: [panicking] Don't open that door!\n"
    "- <whisper>...</whisper>, <slow>...</slow> and <emphasis>...</emphasis> shape just the words inside.\n"
    "- Never use round brackets or asterisks for directions: everything in round brackets is read out loud. No stage directions, no markdown, "
    "no line without a speaker.\n"
    "- Short turns (one to three sentences), lots of back and forth, interruptions with a dash. Characters call each other by name now and then "
    "so listeners can follow who is who.\n"
    "- Use cues generously but where a real performer would: a few per line at most.\n"
    "- Make it {GENRE}.\n"
    "- About {SECONDS} seconds spoken: about {WORDS} words in all.\n"
    "- Write in the language of the idea.";

// ---- the page's state
struct Audition {
    int act = 0;          // its ComfyUI job
    long long seed = 0;
    std::wstring file;    // once it is back
    bool failed = false;
    bool shared = false;  // a take of the whole cast: its clip is cut out of it once whisper has found the line
};

// a take of the whole cast cut into each character's clip (worker thread -> page): (name, job, the clip or "" when the line was not found)
std::mutex g_cutMutex;
std::vector<std::tuple<std::string, int, std::wstring>> g_cuts;
std::set<int> g_cutting; // cast takes being cut

struct Member {
    std::string name, desc;        // desc: the voice, in words (an invented voice comes from it)
    std::wstring clip;             // a kept voice: a clip of it (an audition kept, a saved voice, your own recording)
    std::string transcript;        // what the clip says, word for word
    int asrAct = 0;                // Qwen3-ASR writing the transcript
    std::vector<Audition> auditions;
};

struct SavedVoice { // the voice library: voices kept for any scene
    std::string name, desc, transcript;
    std::wstring clip;
};

struct Written {
    std::string title, script, error;
    std::vector<std::pair<std::string, std::string>> cast;
};

struct Scene { // a performance
    std::wstring path;
    std::string name, title;
    fs::file_time_type time;
    double seconds = 0;
    int heard = -1, lines = 0; // its lines whisper heard (.take.json)
    bool bad = false;
    std::string why;
};

struct Pending { // a performance being made: its lines, for the subtitles; what was sent, to send again if the take goes wrong
    int act = 0;
    std::string title;
    std::vector<std::pair<std::string, std::string>> lines; // (who, what they say)
    std::string workflow, label;
    std::vector<std::pair<std::string, std::wstring>> uploads;
    double expected = 0;  // seconds the script should take
    bool retry = true;    // a wrong take is made again (once)
};
std::mutex g_retryMutex;
std::vector<std::pair<Pending, std::string>> g_retries; // takes to make again, and why

struct Drama {
    bool loaded = false;
    std::string idea, title, script;
    int genre = 0;
    double seconds = 45;
    int speakers = 0;               // 0: the writer picks (2 to 4)
    bool keepCast = false;          // the writer uses the cast as it is
    std::vector<Member> cast;
    std::vector<SavedVoice> library;
    // the performance
    int takes = 1;
    bool lockSeed = false;
    long long seed = 1;
    float temperature = 1.0f, topP = 0.9f;
    int topK = 4097, maxFrames = 600; // (4097 keeps every candidate, Fish's own Space's choice: audio.cpp warns 30 can stall in silence)
    bool split = true;
    bool retryBad = true;           // a take that went wrong is made again, once
    float cacheGb = 0;              // VRAM for cached experts (0: what is free)
    std::wstring backdrop;          // the picture of the video for posting
    bool tall = true;
    // what is there
    double checked = -1000;
    std::string model, asrModel;    // the files found ("" = none); model: the old ComfyUI-Drama3 file, only to say it can go
    bool gguf = false, audiocpp = false; // the Drama 3 GGUF (models\audiocpp) and audio.cpp's server (tools\audiocpp)
    bool node = false;                   // our node and its settings in ComfyUI's custom_nodes (put there by the set-up)
    bool codec = false;
    std::atomic<int> nodes{-2};     // the running ComfyUI has the nodes: 1 yes, 0 no, -1 not answering, -2 not asked yet
    double nodesAsked = -1000;
    // the setup (started by your click)
    std::atomic<bool> setupRunning{false}, setupFinished{false};
    std::wstring setupLog;
    std::string setupLine;
    double setupReadAt = 0;
    int setupVariant = 0, setupAsr = 0; // 0: 1.7B, 1: 0.6B
    // the writer
    std::future<Written> writeFut;
    std::string writeNote;
    bool performAfterWrite = false, auditionAfterWrite = false;
    // the performances
    std::vector<Pending> pending;
    std::vector<Scene> scenes;
    std::future<std::vector<Scene>> scanFut;
    double scanned = -1000;
    int doneSeen = -1;
    std::wstring selected, playing;
    std::unique_ptr<VideoPlayer> player;
    int drawnFrame = -1000;         // the last frame the page was on screen (it is a mode of the Sound page, not a tab of its own)
    std::future<std::string> postFut;
};
Drama g_d;

std::mutex g_notesMutex;
std::vector<std::pair<std::string, std::string>> g_notes; // (title, what happened) from worker threads

std::wstring SettingsPath() { return DataDir() + L"\\drama.json"; }
std::wstring LibraryPath() { return DataDir() + L"\\drama_voices.json"; }
std::wstring VoicesDir() { return DataDir() + L"\\drama_voices"; }

void Load() {
    g_d.loaded = true;
    std::ifstream f(SettingsPath(), std::ios::binary);
    const json j = f ? json::parse(f, nullptr, false) : json();
    if (j.is_object()) {
        g_d.idea = j.value("idea", std::string());
        g_d.title = j.value("title", std::string());
        g_d.script = j.value("script", std::string());
        g_d.genre = std::clamp(j.value("genre", 0), 0, static_cast<int>(std::size(kGenres)) - 1);
        g_d.seconds = std::clamp(j.value("seconds", 45.0), 10.0, 300.0);
        g_d.speakers = std::clamp(j.value("speakers", 0), 0, 8);
        g_d.keepCast = j.value("keepCast", false);
        g_d.takes = std::clamp(j.value("takes", 1), 1, 3);
        g_d.lockSeed = j.value("lockSeed", false);
        g_d.seed = j.value("seed", 1LL);
        g_d.temperature = j.value("temperature", 1.0f);
        g_d.topP = j.value("topP", 0.9f);
        g_d.topK = j.value("topK", 30);
        g_d.maxFrames = j.value("maxFrames", 600);
        if (g_d.topK == 30) g_d.topK = 4097; // (the old nodes' default, saved before audio.cpp)
        g_d.split = j.value("split", true);
        g_d.retryBad = j.value("retryBad", true);
        g_d.cacheGb = j.value("cacheGb", 0.0f);
        g_d.backdrop = Wide(j.value("backdrop", std::string()));
        g_d.tall = j.value("tall", true);
        g_d.setupVariant = std::clamp(j.value("setupVariant", 0), 0, static_cast<int>(std::size(kVariants)) - 1);
        if (j.contains("cast") && j["cast"].is_array())
            for (const json& c : j["cast"]) {
                Member m;
                m.name = c.value("name", std::string());
                m.desc = c.value("desc", std::string());
                m.clip = Wide(c.value("clip", std::string()));
                m.transcript = c.value("transcript", std::string());
                if (!m.name.empty()) g_d.cast.push_back(std::move(m));
            }
    }
    std::ifstream lf(LibraryPath(), std::ios::binary);
    const json l = lf ? json::parse(lf, nullptr, false) : json();
    if (l.is_array())
        for (const json& v : l) {
            SavedVoice s{v.value("name", std::string()), v.value("desc", std::string()), v.value("transcript", std::string()), Wide(v.value("clip", std::string()))};
            std::error_code ec;
            if (!s.name.empty() && fs::exists(s.clip, ec)) g_d.library.push_back(std::move(s));
        }
}

void Save() {
    std::error_code ec;
    fs::create_directories(DataDir(), ec);
    json cast = json::array();
    for (const Member& m : g_d.cast) cast.push_back({{"name", m.name}, {"desc", m.desc}, {"clip", Utf8(m.clip)}, {"transcript", m.transcript}});
    std::ofstream f(SettingsPath(), std::ios::binary | std::ios::trunc);
    if (f)
        f << json({{"idea", g_d.idea}, {"title", g_d.title}, {"script", g_d.script}, {"genre", g_d.genre}, {"seconds", g_d.seconds}, {"speakers", g_d.speakers},
                   {"keepCast", g_d.keepCast}, {"takes", g_d.takes}, {"lockSeed", g_d.lockSeed}, {"seed", g_d.seed}, {"temperature", g_d.temperature},
                   {"topP", g_d.topP}, {"topK", g_d.topK}, {"maxFrames", g_d.maxFrames}, {"split", g_d.split}, {"retryBad", g_d.retryBad}, {"cacheGb", g_d.cacheGb},
                   {"backdrop", Utf8(g_d.backdrop)}, {"tall", g_d.tall}, {"setupVariant", g_d.setupVariant}, {"cast", cast}})
                 .dump(1, ' ', false, json::error_handler_t::replace);
    json lib = json::array();
    for (const SavedVoice& v : g_d.library) lib.push_back({{"name", v.name}, {"desc", v.desc}, {"transcript", v.transcript}, {"clip", Utf8(v.clip)}});
    std::ofstream lf(LibraryPath(), std::ios::binary | std::ios::trunc);
    if (lf) lf << lib.dump(1, ' ', false, json::error_handler_t::replace);
}

// ---- what is installed: the model files (ComfyUI's models folder) and the nodes (the running ComfyUI)
std::wstring AsrDir(bool own) {
    if (own) {
        const std::wstring inst = ComfyInstallDir();
        return inst.empty() ? std::wstring() : inst + L"\\ComfyUI\\models\\asr";
    }
    const std::wstring m = ComfyModelsDir();
    return m.empty() ? std::wstring() : m + L"\\asr";
}

void CheckFiles() {
    const std::wstring models = ComfyModelsDir();
    std::error_code ec;
    g_d.model.clear();
    g_d.asrModel.clear();
    g_d.codec = !models.empty() && fs::exists(fs::path(models) / L"vae" / kCodec, ec);
    g_d.gguf = !models.empty() && fs::exists(fs::path(models) / L"audiocpp" / kGguf, ec);
    g_d.audiocpp = fs::exists(AudiocppExe(), ec);
    const std::wstring install = ComfyInstallDir();
    const fs::path node = fs::path(install) / L"ComfyUI" / L"custom_nodes" / L"ComfyUI-Drama3GGUF";
    g_d.node = !install.empty() && fs::exists(node / L"__init__.py", ec) && fs::exists(node / L"drama3gguf.json", ec);
    if (!models.empty()) {
        for (const Variant& v : kVariants) { // int8 first, then the others
            if (fs::exists(fs::path(models) / L"diffusion_models" / v.file, ec)) {
                g_d.model = v.file;
                break;
            }
        }
    }
    for (bool own : {false, true}) {
        const std::wstring d = AsrDir(own);
        for (const char* f : {"qwen3_asr_1.7b_bf16.safetensors", "qwen3_asr_0.6b_bf16.safetensors"}) {
            if (g_d.asrModel.empty() && !d.empty() && fs::exists(fs::path(d) / f, ec)) g_d.asrModel = f;
        }
    }
}

bool HaveFiles() { return g_d.gguf && g_d.audiocpp && g_d.node; }

std::wstring SetupScript() {
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    const fs::path f = fs::path(buf).parent_path() / L"tools" / L"drama3" / L"setup_drama3.ps1";
    std::error_code ec;
    return fs::exists(f, ec) ? f.wstring() : std::wstring();
}

void StartSetup() {
    const std::wstring script = SetupScript(), install = ComfyInstallDir(), models = ComfyModelsDir();
    if (g_d.setupRunning || script.empty() || install.empty() || models.empty()) return;
    std::error_code ec;
    fs::create_directories(DataDir(), ec);
    g_d.setupLog = DataDir() + L"\\drama3_setup.log";
    fs::remove(g_d.setupLog, ec);
    g_d.setupLine = "starting";
    g_d.setupFinished = false;
    g_d.setupRunning = true;
    std::wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + script + L"\" -Install \"" + install + L"\" -Models \"" + models +
                       L"\" -Gguf -Tools \"" + ToolsDir() + L"\" -Asr " + (g_d.setupAsr == 0 ? L"1.7b" : L"0.6b") + L" -Log \"" + g_d.setupLog + L"\"";
    wchar_t extra[2048] = {}; // (tests: -Nodes of their own)
    if (GetEnvironmentVariableW(L"MV_DRAMA_SETUP_ARGS", extra, 2048)) cmd += L" " + std::wstring(extra);
    std::thread([cmd]() {
        std::string out;
        RunCaptureHidden(cmd, out, true);
        g_d.setupFinished = true;
        g_d.setupRunning = false;
    }).detach();
}

const std::string& SetupLine() {
    if (!g_d.setupLog.empty() && ImGui::GetTime() - g_d.setupReadAt > 0.5) {
        g_d.setupReadAt = ImGui::GetTime();
        std::ifstream f(g_d.setupLog);
        std::string l, last;
        while (std::getline(f, l)) {
            if (l.rfind("\xEF\xBB\xBF", 0) == 0) l = l.substr(3);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            if (!l.empty()) last = l;
        }
        if (!last.empty()) g_d.setupLine = last;
    }
    return g_d.setupLine;
}

// how far the big download is (GB), while it runs
double DownloadedGb() {
    const std::wstring models = ComfyModelsDir();
    std::error_code ec;
    const fs::path part = fs::path(models) / L"audiocpp" / (std::wstring(Wide(kGguf)) + L".part");
    return fs::exists(part, ec) ? fs::file_size(part, ec) / 1e9 : 0.0;
}

// ---- the writer
std::string SpeakersClause() {
    if (g_d.keepCast && !g_d.cast.empty()) {
        std::string c = "exactly these characters, with these names and voices:";
        for (const Member& m : g_d.cast) c += " " + m.name + " | " + (m.desc.empty() ? std::string("(choose a fitting voice)") : m.desc) + ";";
        return c;
    }
    if (g_d.speakers > 0) return std::to_string(g_d.speakers) + " characters";
    return "2 to 4 characters, as the scene needs";
}

Written WriteDrama(std::string idea, int genre, double seconds, std::string speakers, std::string url) {
    Written w;
    if (!WriterConfigured()) {
        w.error = "the prompt writer is not set up (Clef mode > Prompt writer)";
        return w;
    }
    std::string system = kDramaSystem;
    auto put = [&](const char* key, const std::string& v) {
        const size_t at = system.find(key);
        if (at != std::string::npos) system.replace(at, strlen(key), v);
    };
    put("{SPEAKERS}", speakers);
    put("{GENRE}", kGenres[genre].brief);
    put("{SECONDS}", std::to_string(static_cast<int>(seconds)));
    put("{WORDS}", std::to_string(static_cast<int>(seconds * 2.5)));
    const ClefSession session = WriterAcquire(url, nullptr);
    if (!session.ok) {
        WriterRelease(session, false);
        w.error = "the prompt writer could not be started";
        return w;
    }
    std::string out, err;
    const std::string user = "The idea: " + (Trim(idea).empty() ? std::string("make one up that fits the genre") : idea);
    const bool ok = WriterChat(system, user, {}, 4000, 0.9, out, &err);
    WriterRelease(session, false);
    if (!ok) {
        w.error = err.empty() ? std::string("the prompt writer wrote nothing") : err;
        return w;
    }
    // TITLE / CAST / SCRIPT
    int part = 0;
    std::istringstream in(out);
    std::string script;
    for (std::string line; std::getline(in, line);) {
        std::string t = Trim(line);
        while (!t.empty() && (t[0] == '*' || t[0] == '#')) t = Trim(t.substr(1)); // markdown it was told not to use
        const std::string low = Lower(t);
        if (low.rfind("title:", 0) == 0) {
            w.title = Trim(t.substr(6));
            while (!w.title.empty() && (w.title.front() == '"' || w.title.back() == '"' || w.title.back() == '*')) {
                if (w.title.front() == '"') w.title.erase(w.title.begin());
                else w.title.pop_back();
            }
            continue;
        }
        if (low.rfind("cast:", 0) == 0) {
            part = 1;
            continue;
        }
        if (low.rfind("script:", 0) == 0) {
            part = 2;
            continue;
        }
        if (t.empty()) continue;
        if (part == 1) {
            const size_t bar = t.find('|');
            std::string name = Trim(bar == std::string::npos ? t.substr(0, t.find(':')) : t.substr(0, bar));
            while (!name.empty() && (name[0] == '-' || name[0] == '*')) name = Trim(name.substr(1));
            if (name.size() > 1 && std::none_of(name.begin(), name.end(), [](char c) { return c >= 'a' && c <= 'z'; })) // "MARCUS": Marcus
                for (size_t i = 1; i < name.size(); ++i) {
                    if (name[i] >= 'A' && name[i] <= 'Z' && name[i - 1] != ' ' && name[i - 1] != '-') name[i] = static_cast<char>(name[i] + 32);
                }
            std::string desc = bar == std::string::npos ? std::string() : Trim(t.substr(bar + 1));
            for (const char* lead : {"the voice:", "voice:"}) { // the format line, copied
                if (Lower(desc).rfind(lead, 0) == 0) desc = Trim(desc.substr(strlen(lead)));
            }
            if (!name.empty() && name.size() < 40) w.cast.push_back({name, desc});
        } else if (part == 2) {
            if (t[0] == '(') continue; // a stage direction: it would be read out
            t = Trim(std::regex_replace(t, std::regex(R"(<\|speaker:\d+\|>)"), "")); // Drama's own tags, copied in: the names do that
            if (t.empty()) continue;
            script += t + "\n";
        }
    }
    w.script = Trim(script);
    if (w.script.empty()) w.error = "the prompt writer's answer had no script";
    return w;
}

// ---- the script: names become speaker tags
struct Converted {
    std::string text;                                       // for Drama 3 Generate
    std::vector<std::pair<std::string, std::string>> lines; // (who, the words spoken)
    std::vector<int> used;                                  // the cast members who speak
    std::vector<std::string> unknown;                       // names that are not in the cast
};

int MemberIndex(const std::string& name) {
    const std::string n = Lower(Trim(name));
    for (size_t k = 0; k < g_d.cast.size(); ++k) {
        if (Lower(g_d.cast[k].name) == n) return static_cast<int>(k);
    }
    return -1;
}

Converted Convert(const std::string& script) {
    Converted c;
    std::istringstream in(script);
    int last = -1;
    std::set<int> introduced;
    static const std::regex speakerTag(R"(<\|speaker:\d+\|>)");
    for (std::string line; std::getline(in, line);) {
        // (2026-10-26: the writer wrote "<|speaker:1|>Dana: ..." - the line went to the wrong speaker with its name read out, and tags 1 and 2
        // did not match the cast's 0 and 1, so the voices were all over the place; the name decides)
        std::string t = Trim(std::regex_replace(line, speakerTag, ""));
        if (t.empty()) continue;
        int who = -1;
        std::string words = t;
        const size_t colon = t.find(':');
        if (colon != std::string::npos && colon < 40 && t[0] != '[' && t[0] != '<') {
            const std::string name = Trim(t.substr(0, colon));
            const int m = MemberIndex(name);
            if (m >= 0) {
                who = m;
                words = Trim(t.substr(colon + 1));
            } else if (name.find(' ') == std::string::npos || name.size() < 24) {
                if (std::find(c.unknown.begin(), c.unknown.end(), name) == c.unknown.end()) c.unknown.push_back(name);
                continue;
            }
        }
        if (who < 0) who = last >= 0 ? last : 0; // a line without a name: the one speaking goes on
        if (who >= static_cast<int>(g_d.cast.size()) || who > 15 || words.empty()) continue;
        last = who;
        const Member& m = g_d.cast[who];
        // an invented voice comes from its description: given as the delivery cue of the speaker's first line
        if (m.clip.empty() && !m.desc.empty() && !introduced.count(who)) words = "[" + m.desc + "] " + words;
        introduced.insert(who);
        c.text += "<|speaker:" + std::to_string(who) + "|> " + words + "\n";
        c.lines.push_back({m.name, Spoken(words)});
        if (std::find(c.used.begin(), c.used.end(), who) == c.used.end()) c.used.push_back(who);
    }
    return c;
}

// ---- the workflows
json Node(const char* cls, json inputs) { return {{"class_type", cls}, {"inputs", std::move(inputs)}}; }

// The expert cache when it is left on auto: a fixed share of the card. (Sized to "what is free" at each chunk's start, it collapsed to
// nothing from the third chunk on in a test on the 3090, every expert then copied from RAM; 12 GB held 96-98% hits on every chunk.)
float AutoCacheGb() {
    static float gb = -1;
    if (gb < 0) {
        std::string out;
        RunCaptureHidden(L"nvidia-smi --query-gpu=memory.total --format=csv,noheader,nounits", out, false);
        const double total = std::atof(out.c_str()) / 1024.0;
        gb = total >= 20 ? 12.0f : total >= 14 ? 6.0f : total >= 10 ? 3.0f : 0.0f;
    }
    return gb;
}

json Base(json& wf) { return wf; } // (the model is audio.cpp's: nothing to load in ComfyUI)

// The scene through audio.cpp: `references` is the voices, a JSON list of {text: "<|speaker:N|> what the clip says", audio_path} ("" = none)
void Tail(json& wf, const std::string& text, const std::string& references, const char* prefix) {
    wf["3"] = Node("Drama3GGUFGenerate", {{"text", text}, {"seed", 0}, {"temperature", g_d.temperature}, {"top_p", g_d.topP}, {"top_k", g_d.topK},
                                         {"max_tokens", g_d.maxFrames}, {"text_chunk_size", g_d.split ? 128 : 100000}, {"references", references},
                                         // every voice without a kept clip gets one first, its first line alone, used as its reference all through the
                                         // scene (audio.cpp's chunks each hear only the one before: voices changed every line or two)
                                         {"lock_voices", true}});
    wf["5"] = Node("SaveAudioMP3", {{"audio", json::array({"3", 0})}, {"filename_prefix", prefix}, {"quality", "V0"}});
}

// Does a voice description say who it is? Without man / woman (or the like), Drama 3 guesses again and again: a man's lines came out in a
// woman's voice and back. (Descriptions in other alphabets are left alone.)
bool SaysGender(const std::string& desc) {
    std::string low = " " + Lower(desc) + " ";
    for (char& c : low) c = std::isalpha(static_cast<unsigned char>(c)) ? c : ' ';
    if (std::any_of(desc.begin(), desc.end(), [](char c) { return static_cast<unsigned char>(c) >= 0x80; })) return true;
    for (const char* w : {" man ", " woman ", " boy ", " girl ", " male ", " female ", " guy ", " lady ", " gentleman ", " he ", " she ", " men ", " women ",
                          " grandma ", " grandpa ", " mother ", " father ", " mom ", " dad ", " son ", " daughter ", " king ", " queen ", " robot ", " child ",
                          " kid ", " baby ", " creature ", " monster ", " alien "}) {
        if (low.find(w) != std::string::npos) return true;
    }
    return false;
}

std::string SampleLine(const Member& m) {
    return "Hi, I'm " + m.name + ". Honestly, I didn't think today would go like this, but here we are. So, are you ready?";
}

int SendAudition(AppState& app, Member& m, long long seed) {
    json wf = json::object();
    Base(wf);
    Tail(wf, (m.desc.empty() ? std::string() : "[" + m.desc + "] ") + SampleLine(m), std::string(), "drama/auditions/Try2Create_audition");
    wf["3"]["inputs"]["lock_voices"] = false; // (an audition is a new voice each time, in one chunk)
    ComfyJob job;
    job.apiWorkflow = wf.dump();
    job.label = "Drama audition: " + m.name;
    job.prompt = m.desc;
    job.times = 1;
    job.seed = seed;
    return SendComfyJob(app, std::move(job));
}

// Every character without a kept voice says their sample line in ONE take, each as their own speaker (the prompt pass, which costs
// a minute or so whatever the length, is paid once instead of once a character); whisper then finds each line and cuts it out
void AuditionCast(AppState& app, int takes) {
    std::vector<size_t> who;
    std::string text;
    for (size_t k = 0; k < g_d.cast.size() && k < 16; ++k) {
        const Member& m = g_d.cast[k];
        if (!m.clip.empty()) continue;
        who.push_back(k);
        text += "<|speaker:" + std::to_string(k) + "|> " + (m.desc.empty() ? std::string() : "[" + m.desc + "] ") + SampleLine(m) + "\n";
    }
    if (who.empty()) return;
    for (int t = 0; t < takes; ++t) {
        json wf = json::object();
        Base(wf);
        Tail(wf, text, std::string(), "drama/auditions/Try2Create_cast");
        // one chunk: split, it paid the prompt pass twice (a test's two-character takes took 2-3 min); ~9 s of frames a line is plenty
        wf["3"]["inputs"]["text_chunk_size"] = 100000;
        wf["3"]["inputs"]["lock_voices"] = false;
        wf["3"]["inputs"]["max_tokens"] = std::min<int>(8192, static_cast<int>(who.size()) * 200 + 100);
        ComfyJob job;
        job.apiWorkflow = wf.dump();
        job.label = "Drama audition: the cast";
        job.times = 1;
        job.seed = static_cast<long long>(GetTickCount64() % 100000000ull) * 7 + t;
        const long long seed = job.seed;
        const int act = SendComfyJob(app, std::move(job));
        if (!act) continue;
        for (size_t k : who) {
            Audition a;
            a.act = act;
            a.seed = seed;
            a.shared = true;
            g_d.cast[k].auditions.push_back(a);
        }
    }
}

// a cast take: whisper times its lines, and each character's line becomes their clip
void CutCastTake(std::wstring file, int act, std::vector<std::pair<std::string, std::string>> lines) {
    std::string words;
    for (const auto& [name, line] : lines) words += line + "\n";
    AddLyricsToSong(file, words, std::string());
    const std::vector<TimedLyric> timed = ReadSrt(SrtPathOf(file));
    const std::wstring ff = FfmpegPath();
    std::vector<std::tuple<std::string, int, std::wstring>> out;
    for (const auto& [name, line] : lines) {
        std::wstring clip;
        for (const TimedLyric& t : timed) {
            if (t.text != line || ff.empty()) continue;
            std::string safe;
            for (char c : name) safe += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
            const std::wstring dest = fs::path(file).replace_extension().wstring() + L"_" + Wide(safe) + L".wav";
            char range[96];
            snprintf(range, sizeof(range), "-ss %.3f -to %.3f", std::max(0.0, t.start - 0.12), t.end + 0.35);
            std::string log;
            RunCaptureHidden(L"\"" + ff + L"\" -y -hide_banner -loglevel error -i \"" + file + L"\" " + Wide(range) + L" -vn -ac 1 -ar 44100 -c:a pcm_s16le \"" + dest + L"\"", log, true);
            std::error_code ec;
            if (fs::exists(dest, ec) && fs::file_size(dest, ec) > 20000) clip = dest;
            break;
        }
        out.push_back({name, act, clip});
    }
    std::lock_guard<std::mutex> lock(g_cutMutex);
    for (auto& o : out) g_cuts.push_back(std::move(o));
}

void Audition3(AppState& app, Member& m) {
    for (int k = 0; k < 3; ++k) {
        Audition a;
        a.seed = static_cast<long long>(GetTickCount64() % 100000000ull) * 3 + k;
        a.act = SendAudition(app, m, a.seed);
        if (a.act) m.auditions.push_back(a);
    }
}

int SendTranscript(AppState& app, Member& m) {
    if (g_d.asrModel.empty()) return 0;
    json wf = json::object();
    wf["1"] = Node("LoadAudio", {{"audio", "@clip"}});
    wf["2"] = Node("Qwen3ASRLoader", {{"model_name", g_d.asrModel}});
    std::string names;
    for (const Member& c : g_d.cast) names += (names.empty() ? "" : ", ") + c.name;
    wf["3"] = Node("Qwen3ASRTranscribe", {{"model", json::array({"2", 0})}, {"audio", json::array({"1", 0})}, {"language", "auto"}, {"context", names}, {"max_new_tokens", 4096}});
    wf["4"] = Node("PreviewAny", {{"source", json::array({"3", 0})}});
    ComfyJob job;
    job.apiWorkflow = wf.dump();
    job.rawUploads = {{"@clip", m.clip}};
    job.label = "Drama transcript: " + m.name;
    job.times = 1;
    return SendComfyJob(app, std::move(job));
}

void Perform(AppState& app) {
    const Converted c = Convert(g_d.script);
    if (c.text.empty()) {
        app.SetStatus("The script has no lines for the cast");
        return;
    }
    json wf = json::object();
    Base(wf);
    // the voices: each kept clip with what it says, as its speaker (audio.cpp reads the clips from where they are)
    json refs = json::array();
    std::vector<std::pair<std::string, std::wstring>> uploads;
    for (int who : c.used) {
        const Member& m = g_d.cast[who];
        if (m.clip.empty() || Trim(m.transcript).empty()) continue;
        std::string path = Utf8(m.clip);
        std::replace(path.begin(), path.end(), '\\', '/');
        refs.push_back({{"text", "<|speaker:" + std::to_string(who) + "|> " + Trim(m.transcript)}, {"audio_path", path}});
    }
    Tail(wf, c.text, refs.empty() ? std::string() : refs.dump(-1, ' ', false, json::error_handler_t::replace), "drama/Try2Create_drama");
    ComfyJob job;
    job.apiWorkflow = wf.dump();
    job.rawUploads = uploads;
    job.label = "Drama: " + (g_d.title.empty() ? std::string("a scene") : g_d.title);
    job.prompt = g_d.script;
    job.times = g_d.takes;
    if (g_d.lockSeed) job.seed = g_d.seed;
    Pending p;
    p.title = g_d.title;
    p.lines = c.lines;
    p.workflow = job.apiWorkflow;
    p.label = job.label;
    p.uploads = uploads;
    size_t words = 0;
    for (const auto& l : c.lines) {
        std::istringstream in(l.second);
        for (std::string w; in >> w;) ++words;
    }
    p.expected = words / 2.5;
    p.retry = g_d.retryBad && !g_d.lockSeed; // (the same seed would make the same take)
    p.act = SendComfyJob(app, std::move(job));
    if (!p.act) return;
    g_d.pending.push_back(p);
    Toast(ToastKind::Info, "Performing the scene", (g_d.title.empty() ? std::string("The scene") : g_d.title) + ": " + std::to_string(c.lines.size()) + " lines, " +
                                                       std::to_string(c.used.size()) + " voices",
          {}, [](AppState& s) { s.requestTab = 12; }, "drama");
}

// a finished performance: its title in the file, its lines in it (timed by whisper), and the subtitles saying who speaks.
// A take that went wrong (Drama 3 sometimes falls silent or rambles on in noise) is told by how few lines whisper heard, or by running
// far longer than the script: it is noted next to the file (.take.json) and, once, made again.
void FinishPerformance(std::wstring file, Pending p) {
    const std::string title = p.title;
    const auto& lines = p.lines;
    const std::wstring ff = FfmpegPath();
    if (!ff.empty() && !title.empty()) {
        std::string t = title;
        std::replace(t.begin(), t.end(), '"', '\'');
        const std::wstring tmp = file + L".title.tmp";
        std::string log;
        RunCaptureHidden(L"\"" + ff + L"\" -y -hide_banner -loglevel error -i \"" + file + L"\" -map 0 -c copy -id3v2_version 3 -metadata title=\"" + Wide(t) +
                             L"\" -metadata artist=\"Try2Create Drama\" -f mp3 \"" + tmp + L"\"",
                         log, true);
        std::error_code ec;
        if (fs::exists(tmp, ec) && fs::file_size(tmp, ec) > 1000) ReplaceSongFile(tmp, file);
        else fs::remove(tmp, ec);
    }
    std::string words;
    for (const auto& [who, w] : lines) {
        if (!w.empty()) words += w + "\n";
    }
    std::string note = words.empty() ? std::string("no lines") : AddLyricsToSong(file, words, title);
    // the subtitles say who speaks: "Rex: Arrr!"
    const std::vector<TimedLyric> timed = ReadSrt(SrtPathOf(file));
    if (!timed.empty()) {
        std::ofstream srt(SrtPathOf(file), std::ios::binary | std::ios::trunc);
        size_t from = 0;
        for (size_t k = 0; k < timed.size(); ++k) {
            std::string who;
            for (size_t i = from; i < lines.size(); ++i) {
                if (lines[i].second == timed[k].text) {
                    who = lines[i].first;
                    from = i + 1;
                    break;
                }
            }
            auto clock = [](double s) {
                const long long ms = std::llround(std::max(0.0, s) * 1000.0);
                char b[32];
                snprintf(b, sizeof(b), "%02lld:%02lld:%02lld,%03lld", ms / 3600000, (ms / 60000) % 60, (ms / 1000) % 60, ms % 1000);
                return std::string(b);
            };
            srt << (k + 1) << "\n" << clock(timed[k].start) << " --> " << clock(timed[k].end) << "\n" << (who.empty() ? "" : who + ": ") << timed[k].text << "\n\n";
        }
    }
    // did it go wrong?
    int heard = -1, total = static_cast<int>(lines.size());
    if (note.rfind("timed ", 0) == 0) sscanf(note.c_str(), "timed %d of %d", &heard, &total);
    const double seconds = ProbeVideo(file).seconds;
    // a silent stretch: two or more lines in a row that whisper did not hear (whisper alone misses a short line now and then)
    int gap = 0, worstGap = 0;
    if (!timed.empty()) {
        std::set<std::string> said;
        for (const TimedLyric& t : timed) said.insert(t.text);
        for (const auto& [who, w] : lines) {
            gap = said.count(w) || w.empty() ? 0 : gap + 1;
            worstGap = std::max(worstGap, gap);
        }
    }
    std::string why;
    if (heard >= 0 && total > 2 && heard < total * 0.6) why = "only " + std::to_string(heard) + " of " + std::to_string(total) + " lines were heard";
    else if (worstGap >= 2) why = std::to_string(worstGap) + " lines in a row were not heard (it went silent there)";
    else if (p.expected > 0 && seconds > p.expected * 2.2 + 15) why = "it ran " + Clock(seconds) + " for about " + Clock(p.expected) + " of lines";
    {
        std::ofstream t(file + L".take.json", std::ios::binary | std::ios::trunc);
        t << json({{"heard", heard}, {"lines", total}, {"seconds", seconds}, {"bad", !why.empty()}, {"why", why}}).dump();
    }
    if (!why.empty()) {
        note = "a bad take: " + why + (p.retry ? " (another take is on its way)" : " (perform it again for another take)");
        if (p.retry) {
            p.retry = false;
            std::lock_guard<std::mutex> lock(g_retryMutex);
            g_retries.push_back({p, why});
        }
    }
    std::lock_guard<std::mutex> lock(g_notesMutex);
    g_notes.push_back({title.empty() ? Utf8(fs::path(file).filename().wstring()) : title, note});
}

// ---- the performances: ComfyUI's output folder (drama) and the app's copies
std::vector<Scene> ScanScenes(std::vector<std::wstring> dirs) {
    std::vector<Scene> out;
    const std::wstring ff = FfmpegPath();
    const std::wstring probe = ff.empty() ? std::wstring() : (fs::path(ff).parent_path() / L"ffprobe.exe").wstring();
    static std::mutex m;
    static std::map<std::wstring, std::pair<fs::file_time_type, std::pair<std::string, double>>> cache;
    for (const std::wstring& d : dirs) {
        std::error_code ec;
        if (d.empty() || !fs::is_directory(d, ec)) continue;
        for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) {
            const std::wstring name = it->path().filename().wstring();
            if (!it->is_regular_file(ec) || name.rfind(L"Try2Create_drama", 0) != 0 || _wcsicmp(it->path().extension().c_str(), L".mp3") != 0) continue;
            Scene s;
            s.path = it->path().wstring();
            s.name = Utf8(name);
            s.time = it->last_write_time(ec);
            { // the same take twice (ComfyUI's output and the app's copy): the one with its lines timed (its title and subtitles are in it)
                auto twin = std::find_if(out.begin(), out.end(), [&](const Scene& o) { return o.name == s.name; });
                if (twin != out.end()) {
                    if (!fs::exists(SrtPathOf(twin->path), ec) && fs::exists(SrtPathOf(s.path), ec)) out.erase(twin);
                    else continue;
                }
            }
            {
                std::lock_guard<std::mutex> lock(m);
                auto c = cache.find(s.path);
                if (c != cache.end() && c->second.first == s.time) {
                    s.title = c->second.second.first;
                    s.seconds = c->second.second.second;
                    out.push_back(s);
                    continue;
                }
            }
            if (!probe.empty()) {
                std::string o;
                RunCaptureHidden(L"\"" + probe + L"\" -v error -show_entries format=duration:format_tags=title -of json \"" + s.path + L"\"", o, false);
                const json j = json::parse(o, nullptr, false);
                if (j.is_object() && j.contains("format")) {
                    const json& f = j["format"];
                    if (f.contains("duration") && f["duration"].is_string()) s.seconds = std::atof(f["duration"].get<std::string>().c_str());
                    if (f.contains("tags") && f["tags"].is_object())
                        for (const auto& [k, v] : f["tags"].items()) {
                            if (Lower(k) == "title" && v.is_string()) s.title = v.get<std::string>();
                        }
                }
            }
            std::lock_guard<std::mutex> lock(m);
            cache[s.path] = {s.time, {s.title, s.seconds}};
            out.push_back(s);
        }
    }
    for (Scene& s : out) { // how the take went
        std::ifstream t(s.path + L".take.json", std::ios::binary);
        const json j = t ? json::parse(t, nullptr, false) : json();
        if (!j.is_object()) continue;
        s.heard = j.value("heard", -1);
        s.lines = j.value("lines", 0);
        s.bad = j.value("bad", false);
        s.why = j.value("why", std::string());
    }
    std::sort(out.begin(), out.end(), [](const Scene& a, const Scene& b) { return a.time > b.time; });
    return out;
}

void Play(AppState& app, const std::wstring& path) {
    if (!g_d.player) g_d.player = std::make_unique<VideoPlayer>();
    ID3D11DeviceContext* ctx = nullptr;
    app.Device()->GetImmediateContext(&ctx);
    if (!g_d.player->Open(app.Device(), ctx, path, 0.9f, false, false)) app.SetStatus(g_d.player->Error());
    if (ctx) ctx->Release();
    g_d.playing = path;
}

bool Chip(const char* label, bool on) {
    ImGui::PushStyleColor(ImGuiCol_Button, on ? ui::ChipActiveColor() : ui::ChipColor());
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::ChipHoverColor());
    const bool clicked = ImGui::Button(label);
    ImGui::PopStyleColor(2);
    return clicked;
}

// each speaker's colour (the cast list, the lines)
ImU32 SpeakerColor(int k) {
    static const ImU32 c[] = {IM_COL32(255, 159, 67, 255), IM_COL32(84, 160, 255, 255), IM_COL32(255, 107, 129, 255), IM_COL32(29, 209, 161, 255),
                              IM_COL32(200, 130, 255, 255), IM_COL32(254, 202, 87, 255), IM_COL32(72, 219, 251, 255), IM_COL32(255, 138, 101, 255)};
    return c[((k % 8) + 8) % 8];
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

// a kept voice into the library (its clip copied there, so it stays when the audition's file goes)
void KeepInLibrary(const Member& m) {
    std::error_code ec;
    fs::create_directories(VoicesDir(), ec);
    std::string safe;
    for (char c : m.name) safe += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    const std::wstring dest = VoicesDir() + L"\\" + Wide(safe) + L"_" + std::to_wstring(static_cast<long long>(time(nullptr))) + fs::path(m.clip).extension().wstring();
    fs::copy_file(m.clip, dest, fs::copy_options::overwrite_existing, ec);
    if (ec) return;
    g_d.library.erase(std::remove_if(g_d.library.begin(), g_d.library.end(), [&](const SavedVoice& v) { return Lower(v.name) == Lower(m.name); }), g_d.library.end());
    g_d.library.push_back({m.name, m.desc, m.transcript, dest});
}

} // namespace

void PumpDrama(AppState& app) {
    if (!g_d.loaded) Load();
    // the writer's answer
    if (g_d.writeFut.valid() && g_d.writeFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        Written w = g_d.writeFut.get();
        if (w.error.empty()) {
            if (!w.title.empty()) g_d.title = w.title;
            g_d.script = w.script;
            if (!g_d.keepCast || g_d.cast.empty()) { // the writer's cast, keeping the voices of the names that stay
                std::vector<Member> cast;
                for (const auto& [name, desc] : w.cast) {
                    Member m;
                    const int had = MemberIndex(name);
                    if (had >= 0) m = g_d.cast[had];
                    m.name = name;
                    if (had < 0 || m.clip.empty()) m.desc = desc;
                    for (const SavedVoice& v : g_d.library) { // a saved voice of that name
                        if (m.clip.empty() && Lower(v.name) == Lower(name)) m.clip = v.clip, m.transcript = v.transcript;
                    }
                    if (cast.size() < 16) cast.push_back(std::move(m));
                }
                if (!cast.empty()) g_d.cast = std::move(cast);
            }
            g_d.writeNote.clear();
            Save();
            if (g_d.auditionAfterWrite)
                for (Member& m : g_d.cast) {
                    if (m.clip.empty()) {
                        AuditionCast(app, 3);
                        break;
                    }
                }
            if (g_d.performAfterWrite) Perform(app);
        } else {
            g_d.writeNote = w.error;
            Toast(ToastKind::Error, "Could not write the scene", w.error, {}, [](AppState& s) { s.requestTab = 12; }, "drama");
        }
        g_d.performAfterWrite = g_d.auditionAfterWrite = false;
    }
    if (!app.comfySvc) return;
    const std::vector<ComfyActivity> acts = app.comfySvc->Activity();
    auto find = [&](int id) -> const ComfyActivity* {
        for (const ComfyActivity& a : acts) {
            if (a.id == id) return &a;
        }
        return nullptr;
    };
    auto over = [](const ComfyActivity* a) { return !a || a->state == ComfyState::Done || a->state == ComfyState::Failed; };
    for (Member& m : g_d.cast) {
        // auditions coming back (a cast take is cut below)
        for (Audition& a : m.auditions) {
            if (!a.file.empty() || a.failed || a.shared) continue;
            const ComfyActivity* c = find(a.act);
            if (!over(c)) continue;
            for (const ComfyResult& r : c ? c->files : std::vector<ComfyResult>{}) {
                if (r.audio && !r.localPath.empty()) a.file = r.localPath;
            }
            a.failed = a.file.empty();
        }
        // the transcript of a clip
        if (m.asrAct) {
            const ComfyActivity* c = find(m.asrAct);
            if (over(c)) {
                const std::string id = c ? c->promptId : std::string();
                const std::string url = app.comfy.url;
                m.asrAct = 0;
                if (c && c->state == ComfyState::Done) {
                    const std::string text = Trim(ComfyHistoryText(url, id));
                    if (!text.empty()) m.transcript = text;
                    Toast(text.empty() ? ToastKind::Error : ToastKind::Success, "Transcript: " + m.name,
                          text.empty() ? std::string("No speech was heard in the clip") : text, {}, [](AppState& s) { s.requestTab = 12; }, "drama");
                    Save();
                }
            }
        }
    }
    // cast takes coming back: cut into each character's clip
    {
        std::map<int, std::vector<std::pair<std::string, std::string>>> takes;
        for (const Member& m : g_d.cast) {
            for (const Audition& a : m.auditions) {
                if (a.shared && a.file.empty() && !a.failed && !g_cutting.count(a.act)) takes[a.act].push_back({m.name, SampleLine(m)});
            }
        }
        for (auto& [act, lines] : takes) {
            const ComfyActivity* c = find(act);
            if (!over(c)) continue;
            std::wstring file;
            for (const ComfyResult& r : c ? c->files : std::vector<ComfyResult>{}) {
                if (r.audio && !r.localPath.empty()) file = r.localPath;
            }
            g_cutting.insert(act);
            if (file.empty()) {
                for (Member& m : g_d.cast)
                    for (Audition& a : m.auditions) {
                        if (a.act == act) a.failed = true;
                    }
                continue;
            }
            std::thread(CutCastTake, file, act, lines).detach();
        }
        std::vector<std::tuple<std::string, int, std::wstring>> cuts;
        {
            std::lock_guard<std::mutex> lock(g_cutMutex);
            cuts.swap(g_cuts);
        }
        for (const auto& [name, act, clip] : cuts) {
            const int k = MemberIndex(name);
            if (k < 0) continue;
            for (Audition& a : g_d.cast[k].auditions) {
                if (a.act != act) continue;
                a.file = clip;
                a.failed = clip.empty();
            }
        }
    }
    // performances coming back
    for (size_t i = 0; i < g_d.pending.size();) {
        const ComfyActivity* c = find(g_d.pending[i].act);
        if (!over(c)) {
            ++i;
            continue;
        }
        if (c && c->state == ComfyState::Done) {
            for (const ComfyResult& r : c->files) {
                if (!r.audio || r.localPath.empty()) continue;
                std::thread(FinishPerformance, r.localPath, g_d.pending[i]).detach();
                g_d.selected = r.localPath;
            }
            Toast(ToastKind::Success, "Scene ready" + (g_d.pending[i].title.empty() ? std::string() : ": " + g_d.pending[i].title),
                  "Its lines are being timed for the subtitles", {}, [](AppState& s) { s.requestTab = 12; }, "drama");
        }
        g_d.pending.erase(g_d.pending.begin() + i);
    }
    { // takes that went wrong: once more, with a new seed
        std::vector<std::pair<Pending, std::string>> again;
        {
            std::lock_guard<std::mutex> lock(g_retryMutex);
            again.swap(g_retries);
        }
        for (auto& [p, why] : again) {
            ComfyJob job;
            job.apiWorkflow = p.workflow;
            job.rawUploads = p.uploads;
            job.label = p.label + " (another take)";
            job.times = 1;
            p.act = SendComfyJob(app, std::move(job));
            if (p.act) g_d.pending.push_back(p);
            Toast(ToastKind::Info, "That take went wrong", why + ": making another take", {}, [](AppState& s) { s.requestTab = 12; }, "drama");
        }
    }
    {
        std::vector<std::pair<std::string, std::string>> notes;
        {
            std::lock_guard<std::mutex> lock(g_notesMutex);
            notes.swap(g_notes);
        }
        for (const auto& [t, n] : notes) {
            g_d.scanned = -1000; // read the list again (the title is in the file now)
            if (!n.empty()) Toast(n.rfind("timed", 0) == 0 ? ToastKind::Success : ToastKind::Info, "Subtitles: " + t, n, {}, [](AppState& s) { s.requestTab = 12; }, "drama");
        }
    }
    if (g_d.postFut.valid() && g_d.postFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const std::string r = g_d.postFut.get();
        if (r.rfind("error:", 0) == 0) Toast(ToastKind::Error, "Could not make the video", r.substr(7), {}, {}, "drama");
        else Toast(ToastKind::Success, "Video for posting ready", r, {}, [](AppState& s) { s.requestTab = 10; }, "drama");
    }
    if (ImGui::GetFrameCount() - g_d.drawnFrame > 2 && g_d.player && g_d.player->Playing()) g_d.player->Pause(); // the page was left
    // tests: MV_DRAMA_WRITE "<idea>|<genre>|<seconds>[|audition][|perform]"
    static bool testDone = false;
    char buf[2048] = {};
    if (!testDone && app.comfyLoaded && GetEnvironmentVariableA("MV_DRAMA_WRITE", buf, sizeof(buf))) {
        testDone = true;
        std::vector<std::string> f;
        std::string all = buf;
        for (size_t a = 0; a <= all.size();) {
            const size_t b = std::min(all.find('|', a), all.size());
            f.push_back(all.substr(a, b - a));
            a = b + 1;
        }
        g_d.idea = f[0];
        if (f.size() > 1) g_d.genre = std::clamp(std::atoi(f[1].c_str()), 0, static_cast<int>(std::size(kGenres)) - 1);
        if (f.size() > 2) g_d.seconds = std::clamp(std::atof(f[2].c_str()), 10.0, 300.0);
        g_d.auditionAfterWrite = std::find(f.begin(), f.end(), "audition") != f.end();
        g_d.performAfterWrite = std::find(f.begin(), f.end(), "perform") != f.end();
        CheckFiles();
        g_d.writeFut = std::async(std::launch::async, WriteDrama, g_d.idea, g_d.genre, g_d.seconds, SpeakersClause(), app.comfy.url);
    }
    // tests: MV_DRAMA_KEEP: once every audition is back, each character keeps its first take, then the scene is performed with those voices
    static bool keepDone = false;
    if (!keepDone && !g_d.cast.empty() && GetEnvironmentVariableA("MV_DRAMA_KEEP", buf, sizeof(buf))) {
        bool all = true, any = false;
        for (const Member& m : g_d.cast) {
            if (!m.clip.empty()) continue;
            if (m.auditions.empty()) all = false;
            for (const Audition& a : m.auditions) all &= !a.file.empty() || a.failed;
            any = true;
        }
        if (all && any) {
            keepDone = true;
            for (Member& m : g_d.cast) {
                for (const Audition& a : m.auditions) {
                    if (a.file.empty()) continue;
                    m.clip = a.file;
                    m.transcript = SampleLine(m);
                    break;
                }
                m.auditions.clear();
            }
            Save();
            Perform(app);
        }
    }
    // tests: MV_DRAMA_POST "<scene.mp3>|<picture>|<out.mp4>": a tall video for posting
    static bool postTest = false;
    wchar_t wpost[2048] = {};
    if (!postTest && !g_d.postFut.valid() && GetEnvironmentVariableW(L"MV_DRAMA_POST", wpost, 2048)) {
        postTest = true;
        const std::wstring all = wpost;
        const size_t a = all.find(L'|'), b = all.find(L'|', a + 1);
        if (a != std::wstring::npos && b != std::wstring::npos)
            g_d.postFut = std::async(std::launch::async, MakeAudioPostVideo, all.substr(0, a), all.substr(a + 1, b - a - 1), all.substr(b + 1), true);
    }
    // tests: MV_DRAMA_PERFORM: the saved script, performed once ComfyUI is there
    static bool performTest = false;
    if (!performTest && app.comfyLoaded && GetEnvironmentVariableA("MV_DRAMA_PERFORM", buf, sizeof(buf))) {
        performTest = true;
        CheckFiles();
        Perform(app);
    }
}

bool DramaPlaying() { return g_d.player && g_d.player->IsOpen() && g_d.player->Playing(); }

void DrawDramaTab(AppState& app, float height) {
    g_d.drawnFrame = ImGui::GetFrameCount();
    if (!g_d.loaded) Load();
    if (ImGui::GetTime() - g_d.checked > (g_d.setupRunning ? 3.0 : 20.0)) {
        g_d.checked = ImGui::GetTime();
        CheckFiles();
    }
    // the nodes: asked in the background (every 10 s until there, then every minute)
    if (ImGui::GetTime() - g_d.nodesAsked > (g_d.nodes.load() == 1 ? 60.0 : 10.0)) {
        g_d.nodesAsked = ImGui::GetTime();
        std::thread([url = app.comfy.url]() { g_d.nodes = ComfyHasNode(url, "Drama3GGUFGenerate"); }).detach();
    }
    // the performances: looked at again every 15 s, and when a drama job is done
    int doneNow = 0;
    if (app.comfySvc) {
        for (const ComfyActivity& a : app.comfySvc->Activity()) doneNow += a.state == ComfyState::Done && a.label.rfind("Drama:", 0) == 0 ? 1 : 0;
    }
    if (!g_d.scanFut.valid() && (ImGui::GetTime() - g_d.scanned > 15.0 || doneNow != g_d.doneSeen)) {
        g_d.scanned = ImGui::GetTime();
        g_d.doneSeen = doneNow;
        const std::wstring out = ComfyOutputDir(app.comfy);
        g_d.scanFut = std::async(std::launch::async, ScanScenes, std::vector<std::wstring>{out.empty() ? std::wstring() : out + L"\\drama", DataDir() + L"\\comfy_results"});
    }
    if (g_d.scanFut.valid() && g_d.scanFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) g_d.scenes = g_d.scanFut.get();
    { // tests: MV_DRAMA_PLAY plays the newest scene once the list is read
        static bool playTest = false;
        if (!playTest && !g_d.scenes.empty() && GetEnvironmentVariableW(L"MV_DRAMA_PLAY", nullptr, 0) > 0) {
            playTest = true;
            g_d.selected = g_d.scenes[0].path;
            Play(app, g_d.scenes[0].path);
        }
    }

    bool changed = false;
    ImGui::BeginChild("##drama", ImVec2(0, height), false);
    const float avail = ImGui::GetContentRegionAvail().x;
    const float leftW = std::clamp(avail * 0.55f, std::min(avail, ui::Dp(460.0f)), ui::Dp(820.0f));
    ImGui::BeginChild("##dramaleft", ImVec2(leftW, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    { // MV_DRAMA_SCROLL "<px>" (tests)
        static int scrolled = 0;
        char sc[32] = {};
        if (scrolled < 3 && GetEnvironmentVariableA("MV_DRAMA_SCROLL", sc, sizeof(sc)) && ImGui::GetTime() > 2.0) ++scrolled, ImGui::SetScrollY(static_cast<float>(std::atof(sc)));
    }
    ImGui::PushFont(ui::TitleFont());
    ImGui::TextUnformatted("Drama");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, ui::kTextDim);
    ImGui::TextWrapped("Audio scenes with many voices, performed by Fish Audio Drama 3: the prompt writer writes the scene and its cast, every character "
                       "gets a voice, and the lines come out with laughs, sighs, whispers and shouting where they belong.");
    ImGui::PopStyleColor();

    // ---- setup: the model files and the nodes
    // ready once the files and our node are in place: a ComfyUI that is not running yet (-1) is started when the scene is sent; one running
    // without the node (0) needs a restart first
    const bool ready = HaveFiles() && g_d.nodes.load() != 0;
    if (!ready) {
        ui::SectionHeading("SET UP");
        const std::string& line = g_d.setupLog.empty() ? std::string() : SetupLine();
        if (g_d.setupRunning) {
            const double gb = DownloadedGb();
            ImGui::TextColored(ui::kAccentHover, "Setting up: %s", line.c_str());
            if (gb > 0) ImGui::TextColored(ui::kAccentHover, "Drama 3 GGUF: %.1f of %.1f GB", gb, kGgufGb);
        } else if (HaveFiles() && g_d.nodes.load() != 1) {
            {
                ImGui::TextColored(ui::kWarn, "The files are there: ComfyUI needs a restart to load the Drama 3 nodes.");
                if (ComfyOwned()) {
                    bool busy = false;
                    for (const ComfyActivity& a : app.comfySvc ? app.comfySvc->Activity() : std::vector<ComfyActivity>{})
                        busy |= a.state != ComfyState::Done && a.state != ComfyState::Failed;
                    ImGui::SameLine();
                    ImGui::BeginDisabled(busy);
                    if (ImGui::SmallButton("Restart ComfyUI")) {
                        std::string message;
                        ComfyStopOwned();
                        ComfyStart(app.comfy, message);
                        app.SetStatus(message);
                        g_d.nodesAsked = -1000;
                    }
                    ImGui::EndDisabled();
                } else {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(close and open ComfyUI Desktop)");
                }
            }
        } else {
            ImGui::TextWrapped("Drama 3 is a 35B model. It runs on audio.cpp from a %.1f GB file (Q3 experts, 8-bit backbone): on a 24 GB card it sits "
                               "wholly on the GPU, so it is quick and keeps no system RAM. %s", kGgufGb,
                               g_d.model.empty() ? "" : "The old ComfyUI-Drama3 model file is no longer used and can be deleted.");
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Transcripts of your own voice clips (Qwen3-ASR):");
            ImGui::SameLine();
            if (Chip("1.7B  4.1 GB", g_d.setupAsr == 0)) g_d.setupAsr = 0;
            ImGui::SameLine(0.0f, ui::Dp(4.0f));
            if (Chip("0.6B  1.6 GB (twice as fast)", g_d.setupAsr == 1)) g_d.setupAsr = 1;
            const bool can = !SetupScript().empty() && !ComfyInstallDir().empty() && !ComfyModelsDir().empty();
            ImGui::BeginDisabled(!can);
            char go[96];
            const double toGet = (g_d.gguf ? 0.0 : kGgufGb) + (g_d.audiocpp ? 0.0 : 0.65) + (g_d.asrModel.empty() ? (g_d.setupAsr == 0 ? 4.1 : 1.6) : 0.0);
            if (toGet < 0.5) snprintf(go, sizeof(go), "Set up Drama 3 (the files are there: only its node)");
            else snprintf(go, sizeof(go), "Set up Drama 3 (about %.0f GB)", std::max(1.0, toGet));
            if (ui::PrimaryButton(go, ImVec2(ui::Dp(300.0f), ui::Dp(34.0f)))) StartSetup();
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(can ? "Downloads audio.cpp (github.com/0xShug0/audio.cpp) into the app's tools folder and the Drama 3 GGUF (Jmica/Drama-3-GGUF)\n"
                                        "and Qwen3-ASR from Hugging Face into ComfyUI's models folder (it resumes if stopped), and puts Try2Create's small\n"
                                        "Drama 3 node and the ComfyUI-Drama3 transcript nodes into custom_nodes. Then ComfyUI restarts.\n"
                                        "Weights: Fish Audio Community License (free below USD 500k a year in revenue)."
                                      : "ComfyUI Desktop was not found");
            if (!line.empty() && line.rfind("error", 0) == 0) ImGui::TextColored(ui::kBad, "%s", line.c_str());
        }
    }

    // ---- the scene
    ui::SectionHeading("THE SCENE");
    if (WrapEdit("##dramaidea", g_d.idea, ui::Dp(54.0f))) changed = true;
    if (g_d.idea.empty()) ImGui::TextDisabled("e.g. \"two burglars break into the wrong house: it belongs to a retired ninja grandma\"");
    for (int k = 0; k < static_cast<int>(std::size(kGenres)); ++k) {
        if (k) ImGui::SameLine(0.0f, ui::Dp(4.0f));
        if (ImGui::GetCursorPosX() + ImGui::CalcTextSize(kGenres[k].name).x + ui::Dp(20.0f) > ImGui::GetContentRegionMax().x) ImGui::NewLine();
        if (Chip(kGenres[k].name, g_d.genre == k)) g_d.genre = k, changed = true;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", kGenres[k].brief);
    }
    float secs = static_cast<float>(g_d.seconds);
    ImGui::SetNextItemWidth(ui::Dp(220.0f));
    if (ImGui::SliderFloat("##dramasecs", &secs, 10.0f, 300.0f, "about %.0f seconds")) g_d.seconds = std::round(secs / 5.0f) * 5.0, changed = true;
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", Clock(g_d.seconds).c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("  Characters");
    for (int n : {0, 2, 3, 4, 5, 6}) {
        ImGui::SameLine(0.0f, ui::Dp(4.0f));
        char lbl[16];
        snprintf(lbl, sizeof(lbl), n == 0 ? "Any##sp" : "%d##sp", n);
        if (Chip(lbl, g_d.speakers == n)) g_d.speakers = n, changed = true;
    }
    if (ImGui::Checkbox("Keep my cast", &g_d.keepCast)) changed = true;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("The writer writes for the characters below, with their names and voices (a series with the same cast)");
    ImGui::SameLine();
    const bool writing = g_d.writeFut.valid();
    ImGui::BeginDisabled(writing);
    if (ImGui::Button(g_d.script.empty() ? "Write the scene" : "Write it again")) {
        g_d.performAfterWrite = false;
        g_d.writeFut = std::async(std::launch::async, WriteDrama, g_d.idea, g_d.genre, g_d.seconds, SpeakersClause(), app.comfy.url);
    }
    ImGui::EndDisabled();
    if (writing) {
        ImGui::SameLine();
        ImGui::TextColored(ui::kAccentHover, "The prompt writer is writing it...");
    } else if (!g_d.writeNote.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ui::kBad, "%s", g_d.writeNote.c_str());
    }

    // ---- the cast and their voices
    ui::SectionHeading("THE CAST");
    if (g_d.cast.empty()) ImGui::TextDisabled("Written with the scene, or add characters yourself.");
    int remove = -1;
    for (size_t k = 0; k < g_d.cast.size(); ++k) {
        Member& m = g_d.cast[k];
        ImGui::PushID(static_cast<int>(k));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + ui::Dp(6.0f), p.y + ImGui::GetFrameHeight() * 0.5f), ui::Dp(5.0f), SpeakerColor(static_cast<int>(k)));
        ImGui::Dummy(ImVec2(ui::Dp(14.0f), ImGui::GetFrameHeight()));
        ImGui::SameLine();
        char nb[64];
        snprintf(nb, sizeof(nb), "%s", m.name.c_str());
        ImGui::SetNextItemWidth(ui::Dp(120.0f));
        if (ImGui::InputTextWithHint("##name", "Name", nb, sizeof(nb))) m.name = nb, changed = true;
        ImGui::SameLine();
        char db[320];
        snprintf(db, sizeof(db), "%s", m.desc.c_str());
        ImGui::SetNextItemWidth(std::max(ui::Dp(120.0f), ImGui::GetContentRegionAvail().x - ui::Dp(30.0f)));
        if (ImGui::InputTextWithHint("##desc", "the voice: gender, age, accent, tone, pace", db, sizeof(db))) m.desc = db, changed = true;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("An invented voice comes from this description (each seed makes a new one).\nKeep an audition to fix the voice.");
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) remove = static_cast<int>(k);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Take %s out of the cast", m.name.c_str());
        // the voice
        ImGui::Indent(ui::Dp(20.0f));
        if (m.clip.empty() && !Trim(m.desc).empty() && !SaysGender(m.desc))
            ImGui::TextColored(ui::kWarn, "Start it with man, woman, boy or girl: without it %s's voice can change from line to line.", m.name.c_str());
        if (!m.clip.empty()) {
            ImGui::TextColored(ui::kGood, "Kept voice");
            ImGui::SameLine();
            const bool isPlaying = g_d.player && g_d.player->IsOpen() && g_d.player->Playing() && _wcsicmp(g_d.playing.c_str(), m.clip.c_str()) == 0;
            if (ImGui::SmallButton(isPlaying ? "Stop##kv" : "Hear it##kv")) {
                if (isPlaying) g_d.player->Pause();
                else Play(app, m.clip);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Let it go")) m.clip.clear(), m.transcript.clear(), changed = true;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Back to an invented voice (from the description)");
            ImGui::SameLine();
            if (ImGui::SmallButton("Save to my voices")) {
                KeepInLibrary(m);
                changed = true;
                Toast(ToastKind::Success, "Voice saved", m.name + " is in your voices for any scene", {}, {}, "drama");
            }
            // its transcript: it must be every word of the clip
            if (m.asrAct) ImGui::TextColored(ui::kAccentHover, "Qwen3-ASR is writing what the clip says...");
            char tb[600];
            snprintf(tb, sizeof(tb), "%s", m.transcript.c_str());
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputTextWithHint("##tr", "what the clip says, every word", tb, sizeof(tb))) m.transcript = tb, changed = true;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("Every word of the clip, exactly. A transcript that misses the clip's last words is the most common reason a take falls silent.");
        } else {
            ImGui::TextDisabled("Invented voice");
            ImGui::SameLine();
            ImGui::BeginDisabled(!ready);
            if (ImGui::SmallButton("Audition")) Audition3(app, m);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Three takes of a sample line in voices made from the description: keep the one you like,\nand %s sounds like that in every scene", m.name.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Your clip...")) {
                const std::wstring f = PickFile(L"Audio\0*.wav;*.mp3;*.flac;*.ogg;*.m4a;*.opus\0All files\0*.*\0", L"A few seconds of clean speech");
                if (!f.empty()) {
                    m.clip = f;
                    m.transcript.clear();
                    m.asrAct = ready && !g_d.asrModel.empty() ? SendTranscript(app, m) : 0;
                    changed = true;
                }
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("Clone a voice from a few seconds of clean speech, cut between sentences.\nQwen3-ASR writes what it says. Only clone voices you have permission to use.");
            if (!g_d.library.empty()) {
                ImGui::SameLine();
                if (ImGui::SmallButton("My voices")) ImGui::OpenPopup("##lib");
                if (ImGui::BeginPopup("##lib")) {
                    for (const SavedVoice& v : g_d.library) {
                        if (ImGui::MenuItem(v.name.c_str(), v.desc.substr(0, 40).c_str())) {
                            m.clip = v.clip;
                            m.transcript = v.transcript;
                            if (m.desc.empty()) m.desc = v.desc;
                            changed = true;
                        }
                    }
                    ImGui::EndPopup();
                }
            }
            // auditions: hear them, keep one
            for (size_t i = 0; i < m.auditions.size(); ++i) {
                Audition& a = m.auditions[i];
                ImGui::PushID(static_cast<int>(i) + 100);
                if (i) ImGui::SameLine(0.0f, ui::Dp(10.0f));
                if (a.failed) ImGui::TextColored(ui::kBad, "take %d failed", static_cast<int>(i) + 1);
                else if (a.file.empty()) ImGui::TextColored(ui::kAccentHover, "take %d...", static_cast<int>(i) + 1);
                else {
                    const bool isPlaying = g_d.player && g_d.player->IsOpen() && g_d.player->Playing() && _wcsicmp(g_d.playing.c_str(), a.file.c_str()) == 0;
                    char lbl[32];
                    snprintf(lbl, sizeof(lbl), isPlaying ? "Stop %d" : "Hear %d", static_cast<int>(i) + 1);
                    if (ImGui::SmallButton(lbl)) {
                        if (isPlaying) g_d.player->Pause();
                        else Play(app, a.file);
                    }
                    ImGui::SameLine(0.0f, ui::Dp(2.0f));
                    if (ImGui::SmallButton("Keep")) {
                        m.clip = a.file;
                        m.transcript = SampleLine(m);
                        m.auditions.clear();
                        changed = true;
                        ImGui::PopID();
                        break;
                    }
                }
                ImGui::PopID();
            }
        }
        ImGui::Unindent(ui::Dp(20.0f));
        ImGui::PopID();
    }
    if (remove >= 0) g_d.cast.erase(g_d.cast.begin() + remove), changed = true;
    ImGui::BeginDisabled(g_d.cast.size() >= 16);
    if (ImGui::SmallButton("+ Character")) {
        Member m;
        m.name = "Character " + std::to_string(g_d.cast.size() + 1);
        g_d.cast.push_back(m);
        changed = true;
    }
    ImGui::EndDisabled();
    {
        const bool anyInvented = std::any_of(g_d.cast.begin(), g_d.cast.end(), [](const Member& m) { return m.clip.empty(); });
        ImGui::SameLine();
        ImGui::BeginDisabled(!ready || !anyInvented);
        if (ImGui::SmallButton("Audition the cast")) AuditionCast(app, 3);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Three takes of every character without a kept voice saying a sample line, the whole cast in each take\n"
                              "(much quicker than one character at a time: each take's start-up is paid once). Keep the voice you like for each.");
    }

    // ---- the script
    ui::SectionHeading("THE SCRIPT");
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Title");
    ImGui::SameLine();
    {
        char tb[160];
        snprintf(tb, sizeof(tb), "%s", g_d.title.c_str());
        ImGui::SetNextItemWidth(ui::Dp(300.0f));
        if (ImGui::InputTextWithHint("##dtitle", "written with the scene", tb, sizeof(tb))) g_d.title = tb, changed = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("One line per turn:  Name: what they say\n\n"
                          "[laughs] [sighs] [pause] [gasps] [whispers] [shouting] [crying] [clears throat]  where the sound happens\n"
                          "[panicking] Don't open that door!   a delivery cue at the start sets the whole line\n"
                          "<whisper>...</whisper>  <slow>...</slow>  <emphasis>...</emphasis>   shape just those words\n"
                          "Any plain words work as a cue: [terrified, barely breathing]\n"
                          "Text in round brackets is read out loud.");
    if (WrapEdit("##script", g_d.script, ui::Dp(220.0f))) changed = true;
    const Converted conv = Convert(g_d.script);
    {
        size_t words = 0;
        for (const auto& l : conv.lines) {
            std::istringstream in(l.second);
            for (std::string w; in >> w;) ++words;
        }
        ImGui::TextDisabled("%d lines, %d voices, about %s spoken", static_cast<int>(conv.lines.size()), static_cast<int>(conv.used.size()), Clock(words / 2.5).c_str());
        if (!conv.unknown.empty()) {
            std::string u;
            for (const std::string& n : conv.unknown) u += (u.empty() ? "" : ", ") + n;
            ImGui::TextColored(ui::kWarn, "Not in the cast (their lines are left out): %s", u.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Add them")) {
                for (const std::string& n : conv.unknown) {
                    Member m;
                    m.name = n;
                    if (g_d.cast.size() < 16) g_d.cast.push_back(m);
                }
                changed = true;
            }
        }
        bool missing = false;
        for (int who : conv.used) missing |= !g_d.cast[who].clip.empty() && Trim(g_d.cast[who].transcript).empty();
        if (missing) ImGui::TextColored(ui::kWarn, "A kept voice has no transcript yet: it is left out (an invented voice speaks instead).");
    }

    // ---- the performance
    ui::SectionHeading("PERFORM");
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Takes");
    for (int n : {1, 2, 3}) {
        ImGui::SameLine(0.0f, ui::Dp(4.0f));
        char lbl[8];
        snprintf(lbl, sizeof(lbl), "%d##tk", n);
        if (Chip(lbl, g_d.takes == n)) g_d.takes = n, changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Same seed", &g_d.lockSeed)) changed = true;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("Invented voices change with the seed: keep a seed you liked to hear the same voices again (kept voices never change)");
    if (g_d.lockSeed) {
        ImGui::SameLine();
        int sd = static_cast<int>(g_d.seed);
        ImGui::SetNextItemWidth(ui::Dp(110.0f));
        if (ImGui::InputInt("##seed", &sd)) g_d.seed = std::max(0, sd), changed = true;
    }
    if (ImGui::TreeNode("Voice settings")) {
        ImGui::SetNextItemWidth(ui::Dp(200.0f));
        if (ImGui::SliderFloat("temperature", &g_d.temperature, 0.5f, 1.3f, "%.2f")) changed = true;
        ImGui::SetNextItemWidth(ui::Dp(200.0f));
        if (ImGui::SliderFloat("top_p", &g_d.topP, 0.5f, 1.0f, "%.2f")) changed = true;
        ImGui::SetNextItemWidth(ui::Dp(200.0f));
        if (ImGui::SliderInt("top_k", &g_d.topK, 5, 4097, g_d.topK >= 4097 ? "no cap" : "%d", ImGuiSliderFlags_Logarithmic)) changed = true;
        ImGui::TextDisabled("1.0 / 0.9 / no cap are Fish Audio's own (their Space dropped the cap: 30 left takes stuck in silence).");
        if (ImGui::SmallButton("Back to 1.0 / 0.9 / no cap")) g_d.temperature = 1.0f, g_d.topP = 0.9f, g_d.topK = 4097, changed = true;
        ImGui::SetNextItemWidth(ui::Dp(200.0f));
        if (ImGui::SliderInt("max frames a chunk", &g_d.maxFrames, 200, 2000)) changed = true;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("21.5 frames a second of speech; a chunk ends earlier when its speech does");
        if (ImGui::Checkbox("Long scenes in chunks (each hears the ones before)", &g_d.split)) changed = true;
        if (ImGui::Checkbox("A take that goes wrong is made again (once)", &g_d.retryBad)) changed = true;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("Drama 3 sometimes falls silent or rambles on in noise. Whisper checks each take against the script:\n"
                              "fewer than 60%% of the lines heard, or far longer than the script, is a bad take.");
        ImGui::TreePop();
    }
    {
        size_t words = 0;
        for (const auto& l : conv.lines) {
            std::istringstream in(l.second);
            for (std::string w; in >> w;) ++words;
        }
        // measured on the 3090 with audio.cpp and the Q3 GGUF (2026-10-26): 25-29 s scenes in 128-character chunks took 10-12 s once loaded
        // (about 0.02 s a frame, 21.5 frames a second of speech), loading about 15 s; the old ComfyUI-Drama3 int8 file took 0.42 s a frame
        const double spoken = std::max(5.0, words / 2.5), est = spoken * 21.5 * 0.03 + 5.0;
        ImGui::BeginDisabled(!ready || conv.lines.empty());
        if (ui::PrimaryButton("Perform it", ImVec2(ui::Dp(200.0f), ui::Dp(38.0f)))) Perform(app);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("about %s a take (the first one: about 15 s more to load the model)", Clock(est).c_str());
    }
    // the RAM it holds (measured on a 64 GB PC with the int8 file: nearly all of it while loading, 35 GB kept until ComfyUI restarts);
    // audio.cpp keeps its model on the GPU and lets it go after each scene, so this is only for a ComfyUI still holding the old one
    if (ComfyHoldsDrama()) {
        const double ram = SystemRamGb();
        const double keep = HaveFiles() ? std::max(20.0, (g_d.model.find("w4a8") != std::string::npos ? 24.6 : g_d.model.find("w6a8") != std::string::npos ? 32.7 : 38.7) * 0.9) : 35.0;
        ImGui::PushStyleColor(ImGuiCol_Text, ui::kTextDim);
        if (ram > 0 && ram < 96)
            ImGui::TextWrapped("Loading Drama 3 uses nearly all of this PC's %.0f GB of RAM for about a minute (other programs may stutter), then ComfyUI keeps "
                               "about %.0f GB while it is loaded. ComfyUI only gives it back when it restarts: this app restarts the ComfyUI it started when other "
                               "work needs the room.",
                               ram, keep);
        ImGui::PopStyleColor();
        if (ComfyHoldsDrama()) {
            bool busy = false;
            for (const ComfyActivity& a : app.comfySvc ? app.comfySvc->Activity() : std::vector<ComfyActivity>{}) busy |= a.state != ComfyState::Done && a.state != ComfyState::Failed;
            ImGui::BeginDisabled(busy || !ComfyOwned());
            if (ImGui::SmallButton("Give the RAM back now")) {
                std::string message;
                ComfyGiveBackDramaRam(app.comfy, message);
                app.SetStatus(message.empty() ? std::string("ComfyUI restarts: Drama 3's RAM is free again") : message);
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(!ComfyOwned() ? "This app did not start ComfyUI: close and open it to give the RAM back"
                                  : busy       ? "After what is being made"
                                               : "Restarts ComfyUI, which gives back the RAM Drama 3 holds (the next scene loads it again)");
        }
    }
    // what is being made
    if (app.comfySvc) {
        const long long now = ComfyService::NowMs();
        for (const ComfyActivity& a : app.comfySvc->Activity()) {
            if (a.label.rfind("Drama", 0) != 0 || a.state == ComfyState::Done || (a.state == ComfyState::Failed && now - a.finishedMs > 60000)) continue;
            ImGui::PushID(a.id);
            const long long s = std::max(0LL, (a.state == ComfyState::Failed ? a.finishedMs : now) - (a.runningMs > 0 ? a.runningMs : a.startedMs)) / 1000;
            if (a.state == ComfyState::Failed) ImGui::TextColored(ui::kBad, "%s: %s", a.label.c_str(), a.message.c_str());
            else {
                ImGui::TextColored(ui::kAccentHover, "%s  %s %lld:%02lld", a.label.c_str(), a.state == ComfyState::Running ? "running" : "waiting", s / 60, s % 60);
                ImGui::SameLine();
                if (ImGui::SmallButton(a.state == ComfyState::Running ? "Stop" : "Cancel")) app.comfySvc->Cancel(a.id);
            }
            ImGui::PopID();
        }
    }
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ui::kTextDim);
    ImGui::TextWrapped("Drama 3: Fish Audio Community License (free for non-commercial use and below USD 500k a year in revenue). "
                       "Don't clone an identifiable person's voice without their consent.");
    ImGui::PopStyleColor();
    if (changed) Save();
    ImGui::EndChild();

    // ---- the right side: the performances
    ImGui::SameLine();
    ImGui::BeginChild("##dramaright", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    VideoPlayer* vp = g_d.player && g_d.player->IsOpen() ? g_d.player.get() : nullptr;
    if (vp) vp->Update();
    ui::SectionHeading("YOUR SCENES");
    if (g_d.scenes.empty()) ImGui::TextDisabled("Nothing yet: performances show up here (ComfyUI's output folder, drama).");
    int selAt = -1;
    for (size_t k = 0; k < g_d.scenes.size(); ++k) {
        if (_wcsicmp(g_d.scenes[k].path.c_str(), g_d.selected.c_str()) == 0) selAt = static_cast<int>(k);
    }
    if (selAt < 0 && !g_d.scenes.empty()) selAt = 0;
    const float listH = std::min(ui::Dp(56.0f) * static_cast<float>(g_d.scenes.size()) + ui::Dp(4.0f), ImGui::GetContentRegionAvail().y * 0.4f);
    ImGui::BeginChild("##scenes", ImVec2(0, listH));
    for (size_t k = 0; k < g_d.scenes.size(); ++k) {
        const Scene& s = g_d.scenes[k];
        ImGui::PushID(static_cast<int>(k));
        const bool current = vp && _wcsicmp(g_d.playing.c_str(), s.path.c_str()) == 0;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x, rh = ui::Dp(52.0f);
        if (ImGui::InvisibleButton("##row", ImVec2(w, rh))) g_d.selected = s.path;
        const bool hov = ImGui::IsItemHovered();
        if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            if (current) vp->Toggle();
            else Play(app, s.path);
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (hov || static_cast<int>(k) == selAt) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rh), ImGui::GetColorU32(ImGuiCol_Text, static_cast<int>(k) == selAt ? 0.10f : 0.06f), ui::Dp(6.0f));
        const std::string title = s.title.empty() ? s.name : s.title;
        dl->AddText(ImVec2(p.x + ui::Dp(10.0f), p.y + ui::Dp(8.0f)), current ? IM_COL32(30, 215, 96, 255) : ImGui::GetColorU32(ImGuiCol_Text), title.c_str());
        std::string sub = Clock(s.seconds);
        double took = 0;
        if (ComfyGenerationTime(s.path, took)) sub += "  \xC2\xB7  made in " + FormatTook(took);
        std::error_code ec;
        if (s.bad) sub += "  \xC2\xB7  a bad take: " + s.why;
        else if (s.heard >= 0) sub += "  \xC2\xB7  " + std::to_string(s.heard) + " of " + std::to_string(s.lines) + " lines heard";
        else if (fs::exists(SrtPathOf(s.path), ec)) sub += "  \xC2\xB7  subtitles";
        dl->AddText(ImVec2(p.x + ui::Dp(10.0f), p.y + ui::Dp(28.0f)), s.bad ? ImGui::GetColorU32(ui::kWarn) : ImGui::GetColorU32(ImGuiCol_TextDisabled), sub.c_str());
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Double click: play\n%s", s.name.c_str());
        ImGui::PopID();
    }
    ImGui::EndChild();
    // the chosen one: play it, its lines as it plays, a video for posting
    if (selAt >= 0) {
        const Scene& s = g_d.scenes[selAt];
        const bool current = vp && _wcsicmp(g_d.playing.c_str(), s.path.c_str()) == 0;
        ImGui::Separator();
        ImGui::PushFont(ui::TitleFont());
        ImGui::TextUnformatted((s.title.empty() ? s.name : s.title).c_str());
        ImGui::PopFont();
        if (ImGui::Button(current && vp->Playing() ? "Pause" : "Play", ImVec2(ui::Dp(70.0f), 0))) {
            if (current) vp->Toggle();
            else Play(app, s.path);
        }
        ImGui::SameLine();
        float pos = current ? static_cast<float>(vp->Position()) : 0.0f;
        const float dur = static_cast<float>(std::max(0.01, current ? vp->Duration() : s.seconds));
        ImGui::SetNextItemWidth(std::max(ui::Dp(80.0f), ImGui::GetContentRegionAvail().x));
        ImGui::BeginDisabled(!current);
        if (ImGui::SliderFloat("##dseek", &pos, 0.0f, dur, (Clock(pos) + " / " + Clock(dur)).c_str()) && current) vp->Seek(pos);
        ImGui::EndDisabled();
        // a video for posting: a picture, the scene, its subtitles
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Video for posting:");
        ImGui::SameLine();
        if (ImGui::SmallButton(g_d.backdrop.empty() ? "Pick a picture..." : Utf8(fs::path(g_d.backdrop).filename().wstring()).c_str())) {
            const std::wstring f = PickFile(L"Pictures\0*.png;*.jpg;*.jpeg;*.webp;*.bmp\0All files\0*.*\0", L"The picture of the video");
            if (!f.empty()) g_d.backdrop = f, Save();
        }
        const bool haveSel = app.selectedIndex >= 0 && app.selectedIndex < static_cast<int>(app.entries.size()) && !app.entries[app.selectedIndex].metadata.isVideo;
        ImGui::SameLine();
        ImGui::BeginDisabled(!haveSel);
        if (ImGui::SmallButton("Gallery's")) g_d.backdrop = app.entries[app.selectedIndex].path, Save();
        ImGui::EndDisabled();
        if (!g_d.backdrop.empty()) { // the picture itself, small (double click: big)
            const AppState::LibThumb lt = app.LibraryThumb(g_d.backdrop);
            if (lt.srv && lt.w > 0 && lt.h > 0) {
                ImGui::SameLine();
                const float th = ImGui::GetFrameHeight() * 1.6f;
                ImGui::Image((ImTextureID)(intptr_t)lt.srv, ImVec2(th * lt.w / lt.h, th));
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) ViewPictureLarge(app, g_d.backdrop);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Double click: see it big");
            }
        }
        ImGui::SameLine();
        if (Chip("Tall", g_d.tall)) g_d.tall = true, Save();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("The video Make it makes: 1080 x 1920, for TikTok, Reels and Shorts");
        ImGui::SameLine(0.0f, ui::Dp(2.0f));
        if (Chip("Square", !g_d.tall)) g_d.tall = false, Save();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("The video Make it makes: 1080 x 1080");
        ImGui::SameLine();
        ImGui::BeginDisabled(g_d.postFut.valid());
        bool make = ImGui::SmallButton(g_d.postFut.valid() ? "Making it..." : g_d.tall ? "Make it (1080 x 1920)" : "Make it (1080 x 1080)");
        if (make && g_d.backdrop.empty()) { // no picture yet: pick one, then make it
            const std::wstring f = PickFile(L"Pictures\0*.png;*.jpg;*.jpeg;*.webp;*.bmp\0All files\0*.*\0", L"The picture of the video");
            if (!f.empty()) g_d.backdrop = f, Save();
            make = !f.empty();
        }
        if (make) {
            const std::wstring dir = ComfyOutputDir(app.comfy);
            const std::wstring out = (dir.empty() ? DataDir() + L"\\drama" : dir + L"\\video") + L"\\Drama_" + fs::path(s.path).stem().wstring() + L".mp4";
            g_d.postFut = std::async(std::launch::async, MakeAudioPostVideo, s.path, g_d.backdrop, out, g_d.tall);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("An MP4 of the scene over the picture, with its lines as subtitles (who says them), for TikTok, Reels or Shorts%s",
                              g_d.backdrop.empty() ? "\n(asks for the picture first)" : "");
        ImGui::SameLine();
        if (ImGui::SmallButton("Explorer")) {
            const std::wstring arg = L"/select,\"" + s.path + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
        }
        // its lines, the one being said bright
        const std::vector<TimedLyric> lines = ReadSrt(SrtPathOf(s.path));
        ImGui::BeginChild("##lines", ImVec2(0, 0));
        if (lines.empty()) ImGui::TextDisabled("Its lines show here once they are timed (it needs the whisper model: Sound page, Get it).");
        const double t = current ? vp->Position() : -1;
        for (size_t k = 0; k < lines.size(); ++k) {
            const TimedLyric& l = lines[k];
            const bool now = t >= l.start && t < l.end + 0.2;
            const size_t colon = l.text.find(": ");
            const std::string who = colon != std::string::npos && colon < 40 ? l.text.substr(0, colon) : std::string();
            const std::string said = who.empty() ? l.text : l.text.substr(colon + 2);
            ImGui::PushID(static_cast<int>(k));
            if (ImGui::Selectable(("##l" + std::to_string(k)).c_str(), now, 0, ImVec2(0, ImGui::GetTextLineHeightWithSpacing())) && current) vp->Seek(l.start);
            ImGui::SameLine(ui::Dp(8.0f));
            if (!who.empty()) {
                const int m = MemberIndex(who);
                ImGui::PushStyleColor(ImGuiCol_Text, m >= 0 ? ImGui::ColorConvertU32ToFloat4(SpeakerColor(m)) : ui::kTextDim);
                ImGui::TextUnformatted((who + ":").c_str());
                ImGui::PopStyleColor();
                ImGui::SameLine();
            }
            if (now) ImGui::TextUnformatted(said.c_str());
            else ImGui::TextDisabled("%s", said.c_str());
            if (now && vp && vp->Playing()) ImGui::SetScrollHereY(0.4f);
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();
    ImGui::EndChild();
}
