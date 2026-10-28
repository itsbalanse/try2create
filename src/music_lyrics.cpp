#include "music_lyrics.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "comfy.h"
#include "json.hpp"
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

std::string Trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

std::vector<std::string> Lines(const std::string& text) {
    std::vector<std::string> out;
    size_t a = 0;
    while (a <= text.size()) {
        size_t b = text.find('\n', a);
        if (b == std::string::npos) b = text.size();
        out.push_back(text.substr(a, b - a));
        if (b >= text.size()) break;
        a = b + 1;
    }
    return out;
}

// a path inside an ffmpeg filter option, quoted: 'C\:/Users/...'
std::string FilterPath(const std::wstring& path) {
    std::string p = Utf8(path);
    std::replace(p.begin(), p.end(), '\\', '/');
    std::string out = "'";
    for (char c : p) out += c == ':' ? std::string("\\:") : c == '\'' ? std::string("\\'") : std::string(1, c);
    return out + "'";
}

// ---- matching what was heard to what was written
std::vector<std::string> Words(const std::string& s) {
    std::vector<std::string> out;
    std::string w;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '\'' || c >= 0x80) w += static_cast<char>(std::tolower(c));
        else if (!w.empty()) out.push_back(w), w.clear();
    }
    if (!w.empty()) out.push_back(w);
    // the curly apostrophe (and the music note whisper puts around sung lines) are not part of words
    for (std::string& x : out) {
        for (const char* junk : {"\xE2\x80\x99", "\xE2\x99\xAA", "\xE2\x99\xAB"}) {
            for (size_t at = x.find(junk); at != std::string::npos; at = x.find(junk)) x.erase(at, 3);
        }
    }
    out.erase(std::remove_if(out.begin(), out.end(), [](const std::string& x) { return x.empty() || x == "'"; }), out.end());
    return out;
}

int Distance(const std::string& a, const std::string& b) {
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = static_cast<int>(j);
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = static_cast<int>(i);
        for (size_t j = 1; j <= b.size(); ++j) cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

int Similarity(const std::string& a, const std::string& b) { // 2 the same, 1 close ("polished" / "poly"), -1 different
    if (a == b) return 2;
    const size_t n = std::min(a.size(), b.size());
    if (n >= 3 && (a.compare(0, n, b, 0, n) == 0)) return 1;
    if (std::max(a.size(), b.size()) >= 4 && Distance(a, b) <= (std::max(a.size(), b.size()) >= 7 ? 2 : 1)) return 1;
    return -1;
}

struct HeardWord {
    std::string w;
    double t = 0;
};

// The lyric lines, timed: the written words are aligned with the heard ones (Needleman-Wunsch), and each line starts at its first matched word
std::vector<TimedLyric> Align(const std::vector<std::string>& lines, const std::vector<TimedLyric>& heard, double duration) {
    std::vector<std::string> a;
    std::vector<int> lineOf;
    for (size_t l = 0; l < lines.size(); ++l) {
        for (const std::string& w : Words(lines[l])) a.push_back(w), lineOf.push_back(static_cast<int>(l));
    }
    std::vector<HeardWord> b;
    for (const TimedLyric& h : heard) { // a segment's words spread over its time by their length
        const std::vector<std::string> ws = Words(h.text);
        size_t total = 0;
        for (const std::string& w : ws) total += w.size() + 1;
        size_t at = 0;
        for (const std::string& w : ws) {
            b.push_back({w, h.start + (h.end - h.start) * (total ? static_cast<double>(at) / total : 0.0)});
            at += w.size() + 1;
        }
    }
    const size_t n = a.size(), m = b.size();
    std::vector<TimedLyric> out;
    if (n == 0 || m == 0) return out;
    std::vector<int> score((n + 1) * (m + 1));
    std::vector<char> from((n + 1) * (m + 1)); // 0 diagonal, 1 up (a word not heard), 2 left (a heard word not written)
    auto at = [&](size_t i, size_t j) { return i * (m + 1) + j; };
    for (size_t i = 0; i <= n; ++i) score[at(i, 0)] = -static_cast<int>(i), from[at(i, 0)] = 1;
    for (size_t j = 0; j <= m; ++j) score[at(0, j)] = 0, from[at(0, j)] = 2; // heard words before the lyrics start cost nothing
    for (size_t i = 1; i <= n; ++i) {
        for (size_t j = 1; j <= m; ++j) {
            const int d = score[at(i - 1, j - 1)] + Similarity(a[i - 1], b[j - 1].w);
            const int u = score[at(i - 1, j)] - 1, l = score[at(i, j - 1)] - 1;
            if (d >= u && d >= l) score[at(i, j)] = d, from[at(i, j)] = 0;
            else if (u >= l) score[at(i, j)] = u, from[at(i, j)] = 1;
            else score[at(i, j)] = l, from[at(i, j)] = 2;
        }
    }
    std::vector<double> wordTime(n, -1);
    size_t i = n, j = m;
    while (i > 0 && j > 0) {
        const char f = from[at(i, j)];
        if (f == 0) {
            if (Similarity(a[i - 1], b[j - 1].w) > 0) wordTime[i - 1] = b[j - 1].t;
            --i, --j;
        } else if (f == 1) --i;
        else --j;
    }
    // each line: its first and last matched word
    std::vector<double> start(lines.size(), -1), last(lines.size(), -1);
    std::vector<int> matched(lines.size(), 0), count(lines.size(), 0);
    for (size_t k = 0; k < n; ++k) {
        ++count[lineOf[k]];
        if (wordTime[k] < 0) continue;
        const int l = lineOf[k];
        ++matched[l];
        if (start[l] < 0) start[l] = wordTime[k];
        last[l] = std::max(last[l], wordTime[k]);
    }
    // a line is trusted when enough of its words were heard; the others are placed between their neighbours
    int lastHeard = -1;
    for (size_t l = 0; l < lines.size(); ++l) {
        if (matched[l] * 3 < count[l] || matched[l] == 0) start[l] = -1;
        else lastHeard = static_cast<int>(l);
    }
    if (lastHeard < 0) return out;
    for (size_t l = 0; l < lines.size(); ++l) {
        if (start[l] >= 0 || static_cast<int>(l) > lastHeard) continue;
        int p = static_cast<int>(l) - 1, q = static_cast<int>(l) + 1;
        while (p >= 0 && start[p] < 0) --p;
        while (q < static_cast<int>(lines.size()) && start[q] < 0) ++q;
        if (p >= 0 && q <= lastHeard) start[l] = start[p] + (start[q] - start[p]) * (static_cast<double>(l) - p) / (q - p);
    }
    for (size_t l = 0; l < lines.size() && static_cast<int>(l) <= lastHeard; ++l) { // lines after the last heard one were not sung (the song ended)
        if (start[l] < 0) continue;
        TimedLyric t;
        t.start = std::max(0.0, start[l] - 0.15);
        t.end = (last[l] > start[l] ? last[l] : start[l]) + 0.9;
        t.text = Trim(lines[l]);
        out.push_back(t);
    }
    std::sort(out.begin(), out.end(), [](const TimedLyric& x, const TimedLyric& y) { return x.start < y.start; });
    for (size_t k = 0; k < out.size(); ++k) {
        if (k + 1 < out.size()) out[k].end = std::min(out[k].end, out[k + 1].start - 0.05);
        out[k].end = std::max(out[k].end, out[k].start + 0.8);
        if (duration > 0) out[k].end = std::min(out[k].end, duration);
    }
    return out;
}

std::string Clock(double t, bool srt) { // 00:01:02,345 (srt) or 01:02.34 (lrc)
    const long long ms = std::llround(std::max(0.0, t) * 1000.0);
    char buf[32];
    if (srt) snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld,%03lld", ms / 3600000, (ms / 60000) % 60, (ms / 1000) % 60, ms % 1000);
    else snprintf(buf, sizeof(buf), "%02lld:%02lld.%02lld", ms / 60000, (ms / 1000) % 60, (ms % 1000) / 10);
    return buf;
}

double ParseClock(const std::string& s) { // 00:01:02,345
    int h = 0, m = 0, sec = 0, ms = 0;
    if (sscanf_s(s.c_str(), "%d:%d:%d,%d", &h, &m, &sec, &ms) < 3) return -1;
    return h * 3600.0 + m * 60.0 + sec + ms / 1000.0;
}

// ---- the ID3 tag: USLT (the words) and SYLT (timed), written into an ID3v2.3 tag
void Be32(std::string& out, uint32_t v) {
    for (int k = 3; k >= 0; --k) out += static_cast<char>((v >> (k * 8)) & 0xFF);
}

std::string Utf16(const std::string& utf8, bool terminate) { // a BOM, the text as UTF-16LE, and a 00 00 end when asked
    std::string out("\xFF\xFE", 2);
    for (wchar_t c : Wide(utf8)) out += static_cast<char>(c & 0xFF), out += static_cast<char>((c >> 8) & 0xFF);
    if (terminate) out += std::string("\0\0", 2);
    return out;
}

std::string Frame(const char* id, const std::string& data) {
    std::string f(id, 4);
    Be32(f, static_cast<uint32_t>(data.size()));
    f += std::string("\0\0", 2);
    return f + data;
}

bool ReadAll(const std::wstring& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

} // namespace

bool ReplaceSongFile(const std::wstring& tmp, const std::wstring& song) {
    std::error_code ec;
    for (int i = 0; i < 40; ++i) { // a player that has it open lets go when it is closed (the Music tab closes it): up to 10 s
        fs::rename(tmp, song, ec);
        if (!ec) return true;
        Sleep(250);
    }
    fs::remove(tmp, ec);
    return false;
}

namespace {

// the file's tag as ID3v2.3 without unsynchronisation (ffmpeg rewrites it so when it is anything else)
bool EnsureId3v23(const std::wstring& mp3) {
    std::string data;
    if (!ReadAll(mp3, data)) return false;
    if (data.size() > 10 && data.compare(0, 3, "ID3") == 0 && data[3] == 3 && data[5] == 0) return true;
    const std::wstring ff = FfmpegPath();
    if (ff.empty()) return false;
    const std::wstring tmp = mp3 + L".id3.tmp";
    std::string log;
    RunCaptureHidden(L"\"" + ff + L"\" -y -hide_banner -loglevel error -i \"" + mp3 + L"\" -map 0 -c copy -id3v2_version 3 -write_id3v1 0 -f mp3 \"" + tmp + L"\"", log, true);
    std::error_code ec;
    if (!fs::exists(tmp, ec) || fs::file_size(tmp, ec) < 1000) return fs::remove(tmp, ec), false;
    return ReplaceSongFile(tmp, mp3);
}

bool WriteLyricFrames(const std::wstring& mp3, const std::string& plain, const std::vector<TimedLyric>& timed) {
    if (!EnsureId3v23(mp3)) return false;
    std::string data;
    if (!ReadAll(mp3, data) || data.size() < 10) return false;
    const uint32_t tagSize = ((data[6] & 0x7F) << 21) | ((data[7] & 0x7F) << 14) | ((data[8] & 0x7F) << 7) | (data[9] & 0x7F);
    if (10 + static_cast<size_t>(tagSize) > data.size()) return false;
    // the frames there are, without old lyrics
    std::string frames;
    size_t p = 10;
    while (p + 10 <= 10 + static_cast<size_t>(tagSize) && data[p] != 0) {
        const uint32_t sz = (static_cast<unsigned char>(data[p + 4]) << 24) | (static_cast<unsigned char>(data[p + 5]) << 16) |
                            (static_cast<unsigned char>(data[p + 6]) << 8) | static_cast<unsigned char>(data[p + 7]);
        if (p + 10 + sz > 10 + static_cast<size_t>(tagSize)) break;
        const std::string id = data.substr(p, 4);
        if (id != "USLT" && id != "SYLT") frames += data.substr(p, 10 + sz);
        p += 10 + sz;
    }
    // USLT: encoding 1 (UTF-16), language, an empty description, the words
    frames += Frame("USLT", std::string("\x01", 1) + "eng" + Utf16("", true) + Utf16(plain, false));
    if (!timed.empty()) { // SYLT: encoding, language, times in milliseconds (2), lyrics (1), description, then each line and its time
        std::string s = std::string("\x01", 1) + "eng" + std::string("\x02\x01", 2) + Utf16("", true);
        for (const TimedLyric& t : timed) {
            s += Utf16(t.text, true);
            Be32(s, static_cast<uint32_t>(std::llround(t.start * 1000.0)));
        }
        frames += Frame("SYLT", s);
    }
    frames += std::string(1024, '\0'); // room to grow
    const uint32_t size = static_cast<uint32_t>(frames.size());
    std::string head = data.substr(0, 6);
    head[5] = 0;
    head += static_cast<char>((size >> 21) & 0x7F);
    head += static_cast<char>((size >> 14) & 0x7F);
    head += static_cast<char>((size >> 7) & 0x7F);
    head += static_cast<char>(size & 0x7F);
    const std::wstring tmp = mp3 + L".lyrics.tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << head << frames;
        f.write(data.data() + 10 + tagSize, static_cast<std::streamsize>(data.size() - 10 - tagSize));
        if (!f) return false;
    }
    return ReplaceSongFile(tmp, mp3);
}

} // namespace

std::vector<std::string> SungLyricLines(const std::string& lyrics) {
    std::vector<std::string> out;
    for (std::string l : Lines(lyrics)) {
        l = Trim(l);
        if (l.empty() || l.front() == '[') continue;
        if (l.front() == '(' && l.back() == ')') l = Trim(l.substr(1, l.size() - 2)); // an ad-lib is sung too
        if (!l.empty()) out.push_back(l);
    }
    return out;
}

std::string FitLyrics(const std::string& lyrics, double seconds) {
    // about 3.5 s a sung line once intros and instrumental parts are counted: past that, the song ends before the words do
    const int budget = std::max(4, static_cast<int>(seconds / 3.5));
    struct Section {
        std::string text;
        int sung = 0;
        bool outro = false;
    };
    std::vector<Section> sections;
    for (const std::string& raw : Lines(lyrics)) {
        const std::string l = Trim(raw);
        if (!l.empty() && l.front() == '[') {
            sections.push_back({});
            std::string low;
            for (char c : l) low += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            sections.back().outro = low.find("outro") != std::string::npos;
        } else if (sections.empty()) {
            sections.push_back({});
        }
        sections.back().text += raw + "\n";
        if (!l.empty() && l.front() != '[') ++sections.back().sung;
    }
    int total = 0;
    for (const Section& s : sections) total += s.sung;
    if (total <= budget) return lyrics;
    std::string out;
    int used = 0;
    const Section* outro = !sections.empty() && sections.back().outro ? &sections.back() : nullptr;
    const int keepForOutro = outro ? std::min(outro->sung, 2) : 0;
    for (const Section& s : sections) {
        if (&s == outro) break;
        if (used + s.sung > budget - keepForOutro && used > 0) break;
        out += s.text;
        used += s.sung;
    }
    if (outro) out += outro->text;
    return Trim(out);
}

std::wstring WhisperModelDir() {
    const std::wstring models = ComfyModelsDir();
    return models.empty() ? std::wstring() : models + L"\\whisper";
}

std::wstring WhisperModelPath() {
    const std::wstring dir = WhisperModelDir();
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return {};
    for (const wchar_t* name : {L"ggml-small.bin", L"ggml-medium.bin", L"ggml-large-v3-turbo.bin", L"ggml-base.bin", L"ggml-small.en.bin", L"ggml-base.en.bin"}) {
        if (fs::exists(fs::path(dir) / name, ec)) return (fs::path(dir) / name).wstring();
    }
    for (const auto& e : fs::directory_iterator(dir, ec)) { // any other ggml model
        const std::wstring n = e.path().filename().wstring();
        if (n.rfind(L"ggml-", 0) == 0 && e.path().extension() == L".bin") return e.path().wstring();
    }
    return {};
}

const char* WhisperModelUrl() { return "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.bin"; }

std::wstring SrtPathOf(const std::wstring& song) { return (fs::path(song).replace_extension(L".srt")).wstring(); }

std::vector<TimedLyric> ReadSrt(const std::wstring& srt) {
    std::vector<TimedLyric> out;
    std::string text;
    if (!ReadAll(srt, text)) return out;
    const std::vector<std::string> lines = Lines(text);
    for (size_t k = 0; k < lines.size(); ++k) {
        const size_t arrow = lines[k].find("-->");
        if (arrow == std::string::npos) continue;
        TimedLyric t;
        t.start = ParseClock(Trim(lines[k].substr(0, arrow)));
        t.end = ParseClock(Trim(lines[k].substr(arrow + 3)));
        for (size_t j = k + 1; j < lines.size() && !Trim(lines[j]).empty(); ++j) t.text += (t.text.empty() ? "" : " ") + Trim(lines[j]);
        if (t.start >= 0 && t.end >= t.start) out.push_back(t);
    }
    return out;
}

// One of the song's tags (ffprobe), "" = none
static std::string SongTag(const std::wstring& song, const std::string& name) {
    const std::wstring ff = FfmpegPath();
    if (ff.empty()) return {};
    std::wstring probe = (fs::path(ff).parent_path() / L"ffprobe.exe").wstring();
    std::error_code ec;
    if (!fs::exists(probe, ec)) return {};
    std::string out;
    RunCaptureHidden(L"\"" + probe + L"\" -v error -show_entries format_tags=" + Wide(name) + L" -of json \"" + song + L"\"", out, false);
    const json j = json::parse(out, nullptr, false);
    if (!j.is_object() || !j.contains("format") || !j["format"].contains("tags")) return {};
    for (const auto& [k, v] : j["format"]["tags"].items()) { // the key's case differs between files
        std::string low;
        for (char c : k) low += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (low == name && v.is_string()) return v.get<std::string>();
    }
    return {};
}

std::string LyricsFromSongFile(const std::wstring& song) {
    const json wf = json::parse(SongTag(song, "prompt"), nullptr, false);
    if (!wf.is_object()) return {};
    for (const auto& [id, n] : wf.items()) {
        if (n.is_object() && n.contains("inputs") && n["inputs"].contains("lyrics") && n["inputs"]["lyrics"].is_string()) return n["inputs"]["lyrics"].get<std::string>();
    }
    return {};
}

std::string AddLyricsToSong(const std::wstring& song, const std::string& lyrics, const std::string& title) {
    const std::vector<std::string> lines = SungLyricLines(lyrics);
    if (lines.empty()) return "no sung lines (an instrumental)";
    std::string plain;
    for (const std::string& l : lines) plain += (plain.empty() ? "" : "\n") + l;
    // listen: ffmpeg's whisper filter writes what it hears, with times
    std::vector<TimedLyric> timed;
    std::string why;
    const std::wstring model = WhisperModelPath(), ff = FfmpegPath();
    if (model.empty()) why = "no whisper model (Sound page: Get it)";
    else if (ff.empty()) why = "ffmpeg.exe was not found";
    else {
        wchar_t tmpDir[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tmpDir);
        const std::wstring heardSrt = std::wstring(tmpDir) + L"try2create_heard_" + std::to_wstring(GetCurrentThreadId()) + L".srt";
        std::error_code ec;
        fs::remove(heardSrt, ec);
        std::string log;
        RunCaptureHidden(L"\"" + ff + L"\" -hide_banner -loglevel error -i \"" + song + L"\" -vn -af \"whisper=model=" + Wide(FilterPath(model)) +
                             L":language=auto:queue=20:format=srt:destination=" + Wide(FilterPath(heardSrt)) + L"\" -f null -",
                         log, true);
        const std::vector<TimedLyric> heard = ReadSrt(heardSrt);
        fs::remove(heardSrt, ec);
        if (heard.empty()) why = "whisper heard nothing" + (log.empty() ? std::string() : ": " + log.substr(0, 160));
        else {
            timed = Align(lines, heard, ProbeVideo(song).seconds);
            if (timed.empty()) why = "what whisper heard did not match the lyrics";
        }
    }
    // in the file (USLT, SYLT), and next to it (.lrc for players, .srt for videos)
    const bool tagged = WriteLyricFrames(song, plain, timed);
    if (!timed.empty()) {
        std::ofstream lrc(fs::path(song).replace_extension(L".lrc"), std::ios::binary | std::ios::trunc);
        const std::string ti = title.empty() ? SongTag(song, "title") : title; // the Lyrics button: the cover pass tagged it
        if (!ti.empty()) lrc << "[ti:" << ti << "]\n";
        lrc << "[ar:Try2Create]\n";
        for (const TimedLyric& t : timed) lrc << "[" << Clock(t.start, false) << "]" << t.text << "\n";
        std::ofstream srt(SrtPathOf(song), std::ios::binary | std::ios::trunc);
        for (size_t k = 0; k < timed.size(); ++k)
            srt << (k + 1) << "\n" << Clock(timed[k].start, true) << " --> " << Clock(timed[k].end, true) << "\n" << timed[k].text << "\n\n";
    }
    std::string note = timed.empty() ? "lyrics without timing" + (why.empty() ? std::string() : " (" + why + ")")
                                     : "timed " + std::to_string(timed.size()) + " of " + std::to_string(lines.size()) + " lines";
    if (!tagged) note += "; could not write them into the MP3";
    return note;
}
