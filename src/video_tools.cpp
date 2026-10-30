// The Video page's tools for videos already made (the VIDEOS list): a transition between two (H3 or LTX-2.5 from A's last frame to B's
// first, joined A + it + B), the background taken out (SAM 3 follows what to keep, ffmpeg puts it on a colour, a picture or transparency),
// and an edit cut from several (the prompt writer plans the cuts, ffmpeg cuts and joins them, with music). Asked for 2026-10-23, from the
// "Open Sourcing Higgsfield" map: its Transitions, Video Background Remover and Katana.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <set>
#include <sstream>
#include <windows.h>
#include <commdlg.h>

#include "clef.h"
#include "comfy.h"
#include "imgui.h"
#include "json.hpp"
#include "metadata.h"
#include "ui_extra.h"
#include "ui_theme.h"
#include "video_util.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::wstring W(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string U8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring DataDir() {
    wchar_t buf[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    return std::wstring(buf) + L"\\Try2Create";
}

// what the tools make goes where the VIDEOS list looks (it scans comfy_results)
std::wstring NewOutput(const std::wstring& stem, const wchar_t* ext) {
    const std::wstring dir = DataDir() + L"\\comfy_results";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    wchar_t when[32];
    wcsftime(when, 32, L"%Y%m%d_%H%M%S", &tm);
    return dir + L"\\" + stem + L"_" + when + ext;
}

std::wstring Stem(const std::wstring& p) {
    std::wstring s = fs::path(p).stem().wstring();
    if (s.size() > 40) s.resize(40);
    return s;
}

// ---- the one job running (a worker thread), what it is doing, and whether it made a video
struct Tool {
    std::future<std::string> fut; // its note when done ("" = fine)
    std::mutex m;
    std::string phase;
    std::string last;             // the last finished job's note, shown a while
    double lastAt = -1000.0;
    std::atomic<bool> made{false}; // a new video is there: the list looks again
} g_t;

void Phase(const std::string& p) {
    std::lock_guard<std::mutex> lock(g_t.m);
    g_t.phase = p;
}

bool Busy() { return g_t.fut.valid(); }

bool HasAudio(const std::wstring& ff, const std::wstring& clip) {
    std::string out;
    RunCaptureHidden(L"\"" + ff + L"\" -hide_banner -i \"" + clip + L"\"", out, true);
    return out.find("Audio:") != std::string::npos;
}

std::string Tail(std::string log) {
    if (log.size() > 400) log = log.substr(log.size() - 400);
    return log.empty() ? std::string("no output") : log;
}

// ---- cuts joined into one video: each part fills the frame (W x H, 24 fps), its sound kept (or silence), music under it all
struct Cut {
    std::wstring clip;
    double start = 0, end = 0; // seconds in the clip (end 0 = to its end)
};

std::string JoinCuts(const std::vector<Cut>& cuts, const std::wstring& music, double musicVolume, bool keepSound, int outW, int outH, const std::wstring& out) {
    const std::wstring ff = FfmpegPath();
    if (ff.empty()) return "ffmpeg.exe was not found (it cuts and joins the videos): install it and put it on the PATH";
    if (cuts.empty()) return "nothing to join";
    outW = std::max(64, outW / 2 * 2), outH = std::max(64, outH / 2 * 2);
    std::wstring cmd = L"\"" + ff + L"\" -y -hide_banner -loglevel error";
    std::string graph;
    double total = 0;
    int input = 0;
    std::vector<std::string> v, a;
    for (const Cut& c : cuts) {
        const double len = std::max(0.2, (c.end > c.start ? c.end : ProbeVideo(c.clip).seconds) - c.start);
        char ss[64];
        snprintf(ss, sizeof(ss), " -ss %.3f -t %.3f", c.start, len);
        cmd += W(ss) + L" -i \"" + c.clip + L"\"";
        const int vi = input++;
        const std::string n = std::to_string(v.size());
        graph += "[" + std::to_string(vi) + ":v]scale=" + std::to_string(outW) + ":" + std::to_string(outH) + ":force_original_aspect_ratio=increase:flags=lanczos,crop=" +
                 std::to_string(outW) + ":" + std::to_string(outH) + ",setsar=1,fps=24,format=yuv420p,trim=duration=" + std::to_string(len) + ",setpts=PTS-STARTPTS[v" + n + "];";
        int ai = vi;
        if (!keepSound || !HasAudio(ff, c.clip)) { // its sound is not wanted, or it has none: the same length of silence
            char sil[96];
            snprintf(sil, sizeof(sil), " -f lavfi -t %.3f -i anullsrc=r=48000:cl=stereo", len);
            cmd += W(sil);
            ai = input++;
        }
        char fades[128];
        snprintf(fades, sizeof(fades), ",atrim=duration=%.3f,asetpts=PTS-STARTPTS,afade=t=in:d=0.05,afade=t=out:st=%.3f:d=0.08", len, std::max(0.0, len - 0.08));
        graph += "[" + std::to_string(ai) + ":a]aresample=48000,aformat=sample_fmts=fltp:channel_layouts=stereo" + std::string(fades) + "[a" + n + "];";
        v.push_back("[v" + n + "]"), a.push_back("[a" + n + "]");
        total += len;
    }
    for (size_t i = 0; i < v.size(); ++i) graph += v[i] + a[i];
    graph += "concat=n=" + std::to_string(v.size()) + ":v=1:a=1[vc][ac]";
    std::string aout = "[ac]";
    std::error_code ec;
    if (!music.empty() && fs::exists(music, ec)) {
        cmd += L" -stream_loop -1 -i \"" + music + L"\"";
        char mix[600];
        if (keepSound) // under the clips' own sound, quieter while they are loud
            snprintf(mix, sizeof(mix),
                     ";[ac]asplit=2[ac1][ac2];[%d:a]aresample=48000,aformat=sample_fmts=fltp:channel_layouts=stereo,volume=%.2f,atrim=0:%.3f,afade=t=in:d=1,"
                     "afade=t=out:st=%.3f:d=2[mus];[mus][ac1]sidechaincompress=threshold=0.03:ratio=6:attack=30:release=400[mud];"
                     "[ac2][mud]amix=inputs=2:duration=first:normalize=0[af]",
                     input, musicVolume, total, std::max(0.0, total - 2.0));
        else
            snprintf(mix, sizeof(mix), ";[%d:a]aresample=48000,aformat=sample_fmts=fltp:channel_layouts=stereo,atrim=0:%.3f,afade=t=in:d=0.5,afade=t=out:st=%.3f:d=2[af]",
                     input, total, std::max(0.0, total - 2.0));
        graph += mix;
        aout = "[af]";
    }
    const std::wstring script = DataDir() + L"\\video_tools_join.txt";
    std::ofstream(script, std::ios::binary | std::ios::trunc) << graph;
    cmd += L" -/filter_complex \"" + script + L"\" -map \"[vc]\" -map \"" + W(aout) + L"\" -c:v libx264 -preset medium -crf 19 -c:a aac -b:a 160k -movflags +faststart \"" +
           out + L"\"";
    fs::remove(out, ec);
    std::string log;
    RunCaptureHidden(cmd, log, true);
    if (!fs::exists(out, ec) || fs::file_size(out, ec) < 1000) return "ffmpeg could not join them: " + Tail(log);
    return {};
}

// ================================================================== a transition between two videos
const char* kTransitionStyles[][2] = {
    {"Smooth camera move", "One continuous shot: the camera glides smoothly from the first scene into the second, the place and the people changing naturally "
                           "on the way - no cut, no fade."},
    {"Morph", "The first scene morphs fluidly into the second: shapes, colours and people melt and reform into it in one continuous, seamless shot."},
    {"Whip pan", "A fast whip pan: the camera swings sharply away with motion blur and lands on the second scene."},
    {"Zoom through", "The camera zooms fast into the first scene, through a detail, and comes out into the second scene."},
    {"Fly through", "The camera flies forward through the first scene, past everything in it, and arrives in the second scene."},
    {"Light flash", "A burst of bright light washes over the first scene and fades to reveal the second scene."},
};

struct Transition {
    bool open = false;
    std::wstring a, b;
    int style = 0;
    float seconds = 3.0f;
    bool join = true;
    // being made
    int activity = 0;
    std::wstring ta, tb; // the clip's frames (A's last, B's first)
    std::wstring ja, jb; // what it is joined between
    bool joinIt = true;
} g_tr;

std::string ExtractEnds(std::wstring a, std::wstring b, std::wstring lastA, std::wstring firstB) {
    const std::wstring ff = FfmpegPath();
    if (ff.empty()) return "ffmpeg.exe was not found (it takes the videos' end frames)";
    Phase("Taking the end frames...");
    std::error_code ec;
    fs::remove(lastA, ec), fs::remove(firstB, ec);
    std::string log;
    RunCaptureHidden(L"\"" + ff + L"\" -y -hide_banner -loglevel error -sseof -0.15 -i \"" + a + L"\" -frames:v 1 -update 1 \"" + lastA + L"\"", log, true);
    if (!fs::exists(lastA, ec)) // (very short, or seeking from its end failed: its frames run out on their own)
        RunCaptureHidden(L"\"" + ff + L"\" -y -hide_banner -loglevel error -i \"" + a + L"\" -update 1 \"" + lastA + L"\"", log, true);
    RunCaptureHidden(L"\"" + ff + L"\" -y -hide_banner -loglevel error -i \"" + b + L"\" -frames:v 1 \"" + firstB + L"\"", log, true);
    if (!fs::exists(lastA, ec) || !fs::exists(firstB, ec)) return "could not take the end frames: " + Tail(log);
    return {};
}

// ================================================================== the background taken out
struct Background {
    bool open = false;
    std::wstring clip;
    char keep[96] = "person";
    int onto = 0;          // 0 transparent (.webm), 1 green, 2 white, 3 black, 4 a picture
    std::wstring picture;
} g_bg;

const char* kOnto[] = {"Transparent (.webm)", "Green screen", "White", "Black", "A picture..."};

std::string RemoveBackground(std::wstring clip, std::string keep, int onto, std::wstring picture, std::string comfyUrl, std::string sam3) {
    const std::wstring ff = FfmpegPath();
    if (ff.empty()) return "ffmpeg.exe was not found (it puts the cut-out together)";
    const VideoInfo full = ProbeVideoFfmpeg(clip);
    if (full.width <= 0 || full.height <= 0) return "could not read the video";
    const double fps = full.fps > 1 ? full.fps : 24.0;
    const double secs = std::min(full.seconds > 0 ? full.seconds : 60.0, 60.0);
    const std::wstring dir = DataDir() + L"\\h3\\nobg";
    std::error_code ec;
    fs::create_directories(dir, ec);
    // SAM 3 looks at a small copy (the mask is scaled back up): at most 512 on the long side, a minute at most
    Phase("Making a small copy for SAM 3...");
    const std::wstring smallCopy = dir + L"\\small.mp4";
    fs::remove(smallCopy, ec);
    std::string log;
    char t[32];
    snprintf(t, sizeof(t), "%.3f", secs);
    const wchar_t* scale = full.width >= full.height ? L"scale=512:-2" : L"scale=-2:512";
    RunCaptureHidden(L"\"" + ff + L"\" -y -hide_banner -loglevel error -t " + W(t) + L" -i \"" + clip + L"\" -an -vf " + scale + L" -c:v libx264 -preset veryfast -crf 18 \"" +
                         smallCopy + L"\"",
                     log, true);
    if (!fs::exists(smallCopy, ec)) return "could not make the small copy: " + Tail(log);
    Phase("SAM 3 is following the " + keep + " through the video...");
    json g;
    auto node = [&](const std::string& id, const char* cls, json inputs) { g[id] = {{"class_type", cls}, {"inputs", std::move(inputs)}}; };
    node("1", "LoadVideo", {{"file", "example.mp4"}});
    node("2", "GetVideoComponents", {{"video", json::array({"1", 0})}});
    node("5", "CheckpointLoaderSimple", {{"ckpt_name", sam3}});
    node("6", "CLIPTextEncode", {{"clip", json::array({"5", 1})}, {"text", keep}});
    node("7", "SAM3_VideoTrack", {{"images", json::array({"2", 0})}, {"model", json::array({"5", 0})}, {"detection_threshold", 0.35}, {"max_objects", 6},
                                  {"detect_interval", 1}, {"conditioning", json::array({"6", 0})}});
    node("8", "SAM3_TrackToMask", {{"track_data", json::array({"7", 0})}, {"object_indices", ""}}); // (every one found: all the people, say)
    node("9", "MaskToImage", {{"mask", json::array({"8", 0})}});
    node("90", "CreateVideo", {{"images", json::array({"9", 0})}, {"fps", json::array({"2", 2})}});
    node("91", "SaveVideo", {{"video", json::array({"90", 0})}, {"filename_prefix", "masks/nobg"}, {"format", "auto"}, {"format.codec", "auto"}});
    std::vector<std::wstring> got;
    std::string err;
    if (!ComfyRunGraph(comfyUrl, g.dump(), {{"1", smallCopy}}, dir, got, err, 3600) || got.empty()) return "SAM 3 could not follow it: " + err;
    const std::wstring mask = got[0];
    // the cut-out at full size: the mask scaled up and softened a little at its edge
    Phase("Putting it together...");
    const int Wd = full.width / 2 * 2, Hd = full.height / 2 * 2;
    const std::string size = std::to_string(Wd) + "x" + std::to_string(Hd);
    std::string fc = "[1:v]scale=" + std::to_string(Wd) + ":" + std::to_string(Hd) + ":flags=bicubic,format=gray,gblur=sigma=1.2[m];[0:v]scale=" + std::to_string(Wd) + ":" +
                     std::to_string(Hd) + ",format=rgba[c];[c][m]alphamerge[fg]";
    std::wstring cmd = L"\"" + ff + L"\" -y -hide_banner -loglevel error -t " + W(t) + L" -i \"" + clip + L"\" -i \"" + mask + L"\"";
    std::wstring out;
    if (onto == 0) {
        out = NewOutput(Stem(clip) + L"_cutout", L".webm");
        cmd += L" -filter_complex \"" + W(fc) + L"\" -map \"[fg]\" -map 0:a? -c:v libvpx-vp9 -pix_fmt yuva420p -b:v 0 -crf 30 -row-mt 1 -c:a libopus \"" + out + L"\"";
    } else {
        char rate[32];
        snprintf(rate, sizeof(rate), "%.3f", fps);
        if (onto == 4) {
            cmd += L" -loop 1 -i \"" + picture + L"\"";
            fc += ";[2:v]scale=" + std::to_string(Wd) + ":" + std::to_string(Hd) + ":force_original_aspect_ratio=increase,crop=" + std::to_string(Wd) + ":" + std::to_string(Hd) +
                  ",setsar=1,fps=" + rate + "[bg]";
        } else {
            const char* colour = onto == 1 ? "0x00B140" : onto == 2 ? "white" : "black";
            fc += ";color=c=" + std::string(colour) + ":s=" + size + ":r=" + rate + "[bg]";
        }
        fc += ";[bg][fg]overlay=shortest=1:format=auto,format=yuv420p[v]";
        out = NewOutput(Stem(clip) + (onto == 1 ? L"_greenscreen" : onto == 2 ? L"_onwhite" : onto == 3 ? L"_onblack" : L"_newbg"), L".mp4");
        cmd += L" -filter_complex \"" + W(fc) + L"\" -map \"[v]\" -map 0:a? -c:v libx264 -preset medium -crf 19 -c:a aac -b:a 160k -movflags +faststart \"" + out + L"\"";
    }
    fs::remove(out, ec);
    RunCaptureHidden(cmd, log, true);
    if (!fs::exists(out, ec) || fs::file_size(out, ec) < 1000) return "ffmpeg could not put it together: " + Tail(log);
    g_t.made = true;
    return full.seconds > 60.5 ? "done (its first minute: longer videos are cut at a minute)" : std::string();
}

// ================================================================== an edit cut from several videos
struct AutoEdit {
    bool open = false;
    std::vector<std::wstring> clips;
    char idea[256] = {};
    float seconds = 20.0f;
    std::wstring music;
    float musicVolume = 0.35f;
    bool keepSound = true;
    int shape = 0; // 0 like the first video, 1 portrait 9:16 (1080x1920), 2 landscape 16:9 (1920x1080)
} g_ed;

const char* kEditSystem =
    "You are a video editor. You get several video clips (their length and what each one shows) and a brief. Plan ONE edit: the order of cuts, "
    "each a piece of one clip. Rules:\n"
    "- Reply with ONLY a JSON array, no other text: [{\"clip\": 1, \"start\": 0.0, \"end\": 2.5}, ...] (clip numbers as given; seconds inside that clip).\n"
    "- The cuts add up to about the target length. Most cuts are 0.8 to 4 seconds; a fast, hype edit uses short cuts, a calm one longer cuts.\n"
    "- Open with a strong moment, end on a strong moment (an action's peak, a smile, a pose). Use every clip at least once when they fit; "
    "avoid two cuts in a row from the same clip unless the brief asks for it.\n"
    "- Never go past a clip's length; start and end inside it, end greater than start.";

std::vector<Cut> PlanFallback(const std::vector<std::wstring>& clips, const std::vector<double>& lens, double target) {
    std::vector<Cut> cuts;
    const double each = std::clamp(target / std::max<size_t>(1, clips.size() * 2), 1.0, 3.0);
    double total = 0;
    for (int round = 0; round < 8 && total < target - 0.5; ++round)
        for (size_t i = 0; i < clips.size() && total < target - 0.5; ++i) {
            const double len = lens[i];
            if (len < 0.6) continue;
            const double d = std::min(each, len);
            const double s = std::fmod((round * 0.37 + 0.2) * len, std::max(0.01, len - d)); // (a different piece each round)
            cuts.push_back({clips[i], s, s + d});
            total += d;
        }
    return cuts;
}

std::string MakeEdit(std::vector<std::wstring> clips, std::string idea, double target, std::wstring music, double musicVolume, bool keepSound, int shape,
                     std::string comfyUrl) {
    Phase("Looking at the clips...");
    std::vector<double> lens;
    std::string user = "The clips:\n";
    for (size_t i = 0; i < clips.size(); ++i) {
        const double len = ProbeVideo(clips[i]).seconds;
        lens.push_back(len);
        // what it shows: the prompt it was made with, when its file keeps one
        std::string shows;
        for (const RawField& f : ReadVideoWorkflowFields(clips[i])) {
            if (f.key != "prompt") continue;
            const json g = json::parse(f.value, nullptr, false);
            if (!g.is_object()) continue;
            for (auto it = g.begin(); it != g.end(); ++it) {
                if (!it.value().is_object() || !it.value().contains("inputs") || !it.value()["inputs"].is_object()) continue;
                for (auto in = it.value()["inputs"].begin(); in != it.value()["inputs"].end(); ++in)
                    if (in.value().is_string() && in.value().get<std::string>().size() > shows.size() && in.value().get<std::string>().find(' ') != std::string::npos)
                        shows = in.value().get<std::string>();
            }
        }
        if (shows.size() > 500) shows = shows.substr(0, 500) + "...";
        char head[96];
        snprintf(head, sizeof(head), "Clip %d: %.1f seconds", static_cast<int>(i + 1), len);
        user += head + std::string(shows.empty() ? "" : ". It shows: " + shows) + "\n";
    }
    char tl[64];
    snprintf(tl, sizeof(tl), "\nTarget length: %.0f seconds.\n", target);
    user += tl + std::string("The brief: ") + (idea.empty() ? "a lively, punchy edit that flows well" : idea) + "\n";
    std::vector<Cut> cuts;
    std::string note;
    if (WriterConfigured()) {
        Phase("The writer is planning the cuts...");
        const ClefSession session = WriterAcquire(comfyUrl, nullptr);
        std::string out;
        const json schema = {{"type", "array"}, {"minItems", 2},
                             {"items", {{"type", "object"},
                                        {"properties", {{"clip", {{"type", "integer"}}}, {"start", {{"type", "number"}}}, {"end", {{"type", "number"}}}}},
                                        {"required", {"clip", "start", "end"}}}}};
        if (session.ok && !WriterChatJson(kEditSystem, user, {}, schema.dump(), 1200, 0.6, out, nullptr)) WriterChat(kEditSystem, user, {}, 1200, 0.6, out, nullptr);
        WriterRelease(session, false);
        const size_t a = out.find('['), b = out.rfind(']');
        const json plan = a != std::string::npos && b != std::string::npos && b > a ? json::parse(out.substr(a, b - a + 1), nullptr, false) : json();
        if (plan.is_array())
            for (const json& c : plan) {
                if (!c.is_object()) continue;
                const int k = c.value("clip", 0) - 1;
                if (k < 0 || k >= static_cast<int>(clips.size())) continue;
                double s = c.value("start", 0.0), e = c.value("end", 0.0);
                s = std::clamp(s, 0.0, std::max(0.0, lens[k] - 0.3));
                e = std::clamp(e, s + 0.3, std::max(s + 0.3, lens[k]));
                if (e - s >= 0.3) cuts.push_back({clips[k], s, e});
            }
        if (cuts.empty()) note = " (the writer's plan could not be read: cut evenly instead)";
    } else {
        note = " (no prompt writer: cut evenly)";
    }
    if (cuts.empty()) cuts = PlanFallback(clips, lens, target);
    if (cuts.empty()) return "the clips are too short to cut";
    double planned = 0;
    for (const Cut& c : cuts) planned += c.end - c.start;
    if (planned < target * 0.75) { // (the writer's plan came out short: more pieces until it is about the length asked for)
        for (const Cut& c : PlanFallback(clips, lens, target - planned)) {
            if (planned >= target - 0.5) break;
            cuts.push_back(c);
            planned += c.end - c.start;
        }
    }
    int Wd = 1080, Hd = 1920;
    if (shape == 2) Wd = 1920, Hd = 1080;
    if (shape == 0) {
        const VideoInfo vi = ProbeVideo(clips.front());
        Wd = vi.width > 0 ? vi.width : 1080, Hd = vi.height > 0 ? vi.height : 1920;
    }
    Phase("Cutting and joining " + std::to_string(cuts.size()) + " cuts...");
    const std::wstring out = NewOutput(L"Edit", L".mp4");
    const std::string err = JoinCuts(cuts, music, musicVolume, keepSound, Wd, Hd, out);
    if (!err.empty()) return err;
    g_t.made = true;
    return "done: " + std::to_string(cuts.size()) + " cuts" + note;
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

void Small(AppState& app, const std::wstring& path, float w, float h) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(0, 0, 0, 255), ui::Dp(4.0f));
    const AppState::LibThumb lt = app.LibraryThumb(path);
    if (lt.srv && lt.w > 0 && lt.h > 0) {
        const float s = std::min(w / lt.w, h / lt.h);
        const ImVec2 a(p.x + (w - lt.w * s) * 0.5f, p.y + (h - lt.h * s) * 0.5f);
        dl->AddImage((ImTextureID)(intptr_t)lt.srv, a, ImVec2(a.x + lt.w * s, a.y + lt.h * s));
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", U8(fs::path(path).filename().wstring()).c_str());
}

void Start(std::future<std::string> f, const std::string& phase) {
    Phase(phase);
    g_t.fut = std::move(f);
}

} // namespace

void OpenTransition(const std::wstring& a, const std::wstring& b) {
    g_tr.open = true, g_tr.a = a, g_tr.b = b;
}

void OpenBackgroundRemoval(const std::wstring& clip) {
    g_bg.open = true, g_bg.clip = clip;
}

void OpenAutoEdit(const std::vector<std::wstring>& clips) {
    g_ed.open = true, g_ed.clips = clips;
}

bool VideoToolsMadeVideo() { return g_t.made.exchange(false); }

std::string VideoToolsStatus() {
    std::lock_guard<std::mutex> lock(g_t.m);
    if (g_t.fut.valid()) return g_t.phase;
    if (g_tr.activity) return "Making the transition (" + std::string(g_tr.joinIt ? "it is joined between them when done" : "on its own") + ")...";
    if (!g_t.last.empty() && ImGui::GetTime() - g_t.lastAt < 20.0) return g_t.last;
    return {};
}

void DrawVideoTools(AppState& app, const std::string& sam3) {
    static int trStage = 0; // the transition: 1 its end frames being taken (worker), 2 being made (ComfyUI), then joined (worker)
    { // tests: MV_TOOL_TEST="bg|clip|onto" / "edit|clip;clip;..." / "tr|a;b" runs that tool once (ComfyUI's SAM 3 known, for bg)
        static bool once = false;
        wchar_t buf[2048] = {};
        if (!once && GetEnvironmentVariableW(L"MV_TOOL_TEST", buf, 2048) > 0 && !Busy() && (std::wstring(buf).rfind(L"bg|", 0) != 0 || !sam3.empty())) {
            once = true;
            std::vector<std::wstring> parts;
            std::wstringstream in(buf);
            for (std::wstring p; std::getline(in, p, L'|');) parts.push_back(p);
            std::vector<std::wstring> clips;
            if (parts.size() > 1) {
                std::wstringstream c(parts[1]);
                for (std::wstring p; std::getline(c, p, L';');) clips.push_back(p);
            }
            if (parts[0] == L"bg" && !clips.empty())
                Start(std::async(std::launch::async, RemoveBackground, clips[0], std::string("person"), parts.size() > 2 ? _wtoi(parts[2].c_str()) : 1, std::wstring(),
                                 app.comfy.url, sam3),
                      "Starting...");
            if (parts[0] == L"edit" && clips.size() >= 2)
                Start(std::async(std::launch::async, MakeEdit, clips, std::string("a fast hype edit"), 12.0, std::wstring(), 0.35, true, 0, app.comfy.url), "Starting...");
            if (parts[0] == L"tr" && clips.size() == 2) {
                const std::wstring dir = DataDir() + L"\\h3\\transitions";
                std::error_code ec;
                fs::create_directories(dir, ec);
                g_tr.ta = dir + L"\\a_last.png", g_tr.tb = dir + L"\\b_first.png", g_tr.ja = clips[0], g_tr.jb = clips[1], g_tr.joinIt = true;
                Start(std::async(std::launch::async, ExtractEnds, clips[0], clips[1], g_tr.ta, g_tr.tb), "Taking the end frames...");
                trStage = 1;
            }
        }
    }
    // the worker's job finished
    if (g_t.fut.valid() && g_t.fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        std::string note;
        try {
            note = g_t.fut.get();
        } catch (...) {
            note = "it failed";
        }
        std::lock_guard<std::mutex> lock(g_t.m);
        g_t.phase.clear();
        if (!(trStage == 1 && note.empty())) { // (the end frames taken: nothing to say yet)
            g_t.last = note.empty() ? std::string("Done: the new video is in the list") : note;
            g_t.lastAt = ImGui::GetTime();
            if (note.rfind("done", 0) == 0 || note.empty()) app.SetStatus(g_t.last);
            else app.SetStatus("Video tools: " + note);
        }
    }
    if (trStage == 1 && !g_t.fut.valid()) {
        std::error_code ec;
        if (fs::exists(g_tr.ta, ec) && fs::exists(g_tr.tb, ec)) {
            H3Options o = VideoOptions();
            o.seconds = std::clamp(static_cast<double>(g_tr.seconds), 2.0, 6.0);
            o.last = g_tr.tb;
            o.times = 1, o.parts = 0, o.partSeconds = 0;
            const std::string prompt = std::string(kTransitionStyles[std::clamp(g_tr.style, 0, 5)][1]) + " Audio: a soft whoosh.";
            if (SendH3(app, g_tr.ta, prompt, o, "Transition")) {
                g_tr.activity = LastEditActivity();
                trStage = 2;
            } else {
                trStage = 0;
            }
        } else {
            trStage = 0;
        }
    }
    if (trStage == 2 && g_tr.activity && app.comfySvc) {
        for (const ComfyActivity& a : app.comfySvc->Activity()) {
            if (a.id != g_tr.activity) continue;
            if (a.state == ComfyState::Done) {
                std::wstring made;
                for (const ComfyResult& r : a.files)
                    if (!r.localPath.empty() && r.video) made = r.localPath;
                g_tr.activity = 0;
                trStage = 0;
                g_t.made = true;
                if (!made.empty() && g_tr.joinIt && !g_t.fut.valid()) {
                    const std::vector<Cut> cuts = {{g_tr.ja, 0, 0}, {made, 0, 0}, {g_tr.jb, 0, 0}};
                    const VideoInfo vi = ProbeVideo(g_tr.ja);
                    const std::wstring out = NewOutput(L"Joined", L".mp4");
                    Start(std::async(std::launch::async,
                                     [cuts, vi, out] {
                                         Phase("Joining the two videos and the transition...");
                                         const std::string e = JoinCuts(cuts, std::wstring(), 0.0, true, vi.width > 0 ? vi.width : 1280, vi.height > 0 ? vi.height : 720, out);
                                         if (e.empty()) g_t.made = true;
                                         return e.empty() ? std::string("done: joined (A, the transition, B)") : e;
                                     }),
                          "Joining...");
                }
            } else if (a.state == ComfyState::Failed) {
                g_tr.activity = 0;
                trStage = 0;
                app.SetStatus("The transition failed: " + a.message);
            }
        }
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 centre(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f);
    // ---- Transition
    if (g_tr.open) {
        ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(ui::Dp(560.0f), 0), ImGuiCond_Appearing);
        if (ImGui::Begin("Transition between two videos", &g_tr.open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextDisabled("A new clip from the first video's last frame to the second's first, then (if ticked) all three joined into one video.");
            const float tw = ui::Dp(200.0f), th = ui::Dp(112.0f);
            Small(app, g_tr.a, tw, th);
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::Dummy(ImVec2(0, th * 0.3f));
            ImGui::TextUnformatted("  then  ");
            if (ImGui::SmallButton("Swap")) std::swap(g_tr.a, g_tr.b);
            ImGui::EndGroup();
            ImGui::SameLine();
            Small(app, g_tr.b, tw, th);
            ui::SectionHeading("HOW IT GOES FROM ONE TO THE OTHER");
            for (int i = 0; i < 6; ++i) {
                if (i) ui::SameLineIfFits(kTransitionStyles[i][0]);
                ImGui::PushStyleColor(ImGuiCol_Button, g_tr.style == i ? ui::ChipActiveColor() : ui::ChipColor());
                if (ImGui::SmallButton(kTransitionStyles[i][0])) g_tr.style = i;
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", kTransitionStyles[i][1]);
            }
            ImGui::SetNextItemWidth(ui::Dp(220.0f));
            ImGui::SliderFloat("##trlen", &g_tr.seconds, 2.0f, 6.0f, "%.0f seconds");
            ImGui::SameLine();
            ImGui::Checkbox("Join all three into one video", &g_tr.join);
            ImGui::TextDisabled("Made with the Video page's quality (%s) and size.", H3QualityName(VideoOptions().quality));
            const bool busy = Busy() || g_tr.activity || trStage != 0;
            ImGui::BeginDisabled(busy);
            if (ui::PrimaryButton(busy ? "Busy (one at a time)..." : "Make the transition")) {
                const std::wstring dir = DataDir() + L"\\h3\\transitions";
                std::error_code ec;
                fs::create_directories(dir, ec);
                g_tr.ta = dir + L"\\a_last.png", g_tr.tb = dir + L"\\b_first.png";
                g_tr.ja = g_tr.a, g_tr.jb = g_tr.b, g_tr.joinIt = g_tr.join;
                Start(std::async(std::launch::async, ExtractEnds, g_tr.a, g_tr.b, g_tr.ta, g_tr.tb), "Taking the end frames...");
                trStage = 1;
                g_tr.open = false;
            }
            ImGui::EndDisabled();
        }
        ImGui::End();
    }
    // ---- Remove the background
    if (g_bg.open) {
        ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::Begin("Remove the background", &g_bg.open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
            Small(app, g_bg.clip, ui::Dp(240.0f), ui::Dp(135.0f));
            ImGui::TextDisabled("SAM 3 follows what to keep through the video; the rest becomes the background you choose (a minute at most).");
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Keep:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ui::Dp(200.0f));
            ImGui::InputText("##keep", g_bg.keep, sizeof(g_bg.keep));
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("A word or two: person, dog, car, the woman in red...");
            ImGui::TextUnformatted("Put it on:");
            for (int i = 0; i < 5; ++i) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, g_bg.onto == i ? ui::ChipActiveColor() : ui::ChipColor());
                if (ImGui::SmallButton(kOnto[i])) {
                    g_bg.onto = i;
                    if (i == 4) {
                        const std::wstring f = PickFile(L"Pictures\0*.png;*.jpg;*.jpeg;*.webp;*.bmp\0All files\0*.*\0", L"The new background");
                        if (!f.empty()) g_bg.picture = f;
                        else if (g_bg.picture.empty()) g_bg.onto = 0;
                    }
                }
                ImGui::PopStyleColor();
            }
            if (g_bg.onto == 4 && !g_bg.picture.empty()) ImGui::TextDisabled("Background: %s", U8(fs::path(g_bg.picture).filename().wstring()).c_str());
            if (g_bg.onto == 0) ImGui::TextDisabled("A .webm with transparency: drop it over anything in an editor (CapCut, Premiere, DaVinci...).");
            if (sam3.empty()) ImGui::TextColored(ui::kWarn, "Needs SAM 3 in ComfyUI (sam3.1_multiplex_fp16 in checkpoints), and ComfyUI running.");
            const bool busy = Busy();
            ImGui::BeginDisabled(busy || sam3.empty() || !g_bg.keep[0]);
            if (ui::PrimaryButton(busy ? "Busy (one at a time)..." : "Remove the background")) {
                Start(std::async(std::launch::async, RemoveBackground, g_bg.clip, std::string(g_bg.keep), g_bg.onto, g_bg.picture, app.comfy.url, sam3), "Starting...");
                g_bg.open = false;
            }
            ImGui::EndDisabled();
        }
        ImGui::End();
    }
    // ---- Make an edit
    if (g_ed.open) {
        ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(ui::Dp(620.0f), 0), ImGuiCond_Appearing);
        if (ImGui::Begin("Make an edit from these videos", &g_ed.open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextDisabled("The prompt writer plans the cuts (order, which part of each), ffmpeg cuts and joins them, with your music under it.");
            const float tw = ui::Dp(96.0f), th = ui::Dp(54.0f);
            for (size_t i = 0; i < g_ed.clips.size(); ++i) {
                if (i) ui::SameLineIfFits("XXXXXXXXXXXX");
                Small(app, g_ed.clips[i], tw, th);
            }
            ui::SectionHeading("WHAT KIND OF EDIT");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##brief", "e.g. a fast hype edit, a calm travel montage, funniest moments first...", g_ed.idea, sizeof(g_ed.idea));
            ImGui::SetNextItemWidth(ui::Dp(220.0f));
            ImGui::SliderFloat("##edlen", &g_ed.seconds, 6.0f, 90.0f, "about %.0f seconds");
            ImGui::SameLine();
            const char* shapes[] = {"Like the first video", "Portrait 9:16", "Landscape 16:9"};
            ImGui::SetNextItemWidth(ui::Dp(180.0f));
            ImGui::Combo("##edshape", &g_ed.shape, shapes, 3);
            ui::SectionHeading("SOUND");
            if (ImGui::SmallButton(g_ed.music.empty() ? "Music..." : "Other music...")) {
                const std::wstring f = PickFile(L"Music\0*.mp3;*.wav;*.flac;*.m4a;*.ogg;*.aac\0All files\0*.*\0", L"Music under the edit");
                if (!f.empty()) g_ed.music = f;
            }
            if (!g_ed.music.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", U8(fs::path(g_ed.music).filename().wstring()).c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("No music")) g_ed.music.clear();
                ImGui::SetNextItemWidth(ui::Dp(160.0f));
                ImGui::SliderFloat("##musvol", &g_ed.musicVolume, 0.05f, 1.0f, "music %.2f");
                ImGui::SameLine();
            }
            ImGui::Checkbox("Keep the videos' own sound", &g_ed.keepSound);
            const bool busy = Busy();
            ImGui::BeginDisabled(busy || g_ed.clips.size() < 2);
            if (ui::PrimaryButton(busy ? "Busy (one at a time)..." : "Make the edit")) {
                Start(std::async(std::launch::async, MakeEdit, g_ed.clips, std::string(g_ed.idea), static_cast<double>(g_ed.seconds), g_ed.music,
                                 static_cast<double>(g_ed.musicVolume), g_ed.keepSound, g_ed.shape, app.comfy.url),
                      "Starting...");
                g_ed.open = false;
            }
            ImGui::EndDisabled();
        }
        ImGui::End();
    }
}
