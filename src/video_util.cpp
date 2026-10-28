#include "video_util.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <regex>
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>

#include "json.hpp"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "propsys.lib")

using json = nlohmann::ordered_json;

namespace {

std::wstring ExtOf(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    size_t slash = path.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return {};
    std::wstring e = path.substr(dot);
    std::transform(e.begin(), e.end(), e.begin(), ::towlower);
    return e;
}

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

struct ComScope {
    bool uninit = false;
    ComScope() { uninit = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); }
    ~ComScope() {
        if (uninit) CoUninitialize();
    }
};

bool EnsureMediaFoundation() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, []() { ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)); });
    return ok;
}

// What the poster-frame decode learned, so a metadata read right after it needn't open the file again.
struct LastProbe {
    std::wstring path;
    VideoInfo info;
};
thread_local LastProbe t_last;

IMFSourceReader* OpenReader(const std::wstring& path) {
    if (!EnsureMediaFoundation()) return nullptr;
    IMFAttributes* attrs = nullptr;
    MFCreateAttributes(&attrs, 2);
    if (attrs) attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    IMFSourceReader* reader = nullptr;
    HRESULT hr = MFCreateSourceReaderFromURL(path.c_str(), attrs, &reader);
    SafeRelease(attrs);
    return SUCCEEDED(hr) ? reader : nullptr;
}

double ReaderDuration(IMFSourceReader* reader) {
    PROPVARIANT v;
    PropVariantInit(&v);
    double seconds = 0;
    if (SUCCEEDED(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &v)) && v.vt == VT_UI8) {
        seconds = static_cast<double>(v.uhVal.QuadPart) / 1e7;
    }
    PropVariantClear(&v);
    return seconds;
}

// ---- ffmpeg fallback (WebM/MKV and codecs Windows lacks), used only when ffmpeg.exe is on the PATH ----
std::wstring FindFfmpeg() {
    static const std::wstring path = []() {
        wchar_t buf[MAX_PATH];
        const DWORD n = SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, buf, nullptr);
        return n > 0 && n < MAX_PATH ? std::wstring(buf, n) : std::wstring();
    }();
    return path;
}

// Runs a command line without a window and collects stdout (plus stderr when `mergeStderr`).
static bool RunCaptureFlags(const std::wstring& cmdline, std::string& out, bool mergeStderr, DWORD flags) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 20)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = INVALID_HANDLE_VALUE;
    if (!mergeStderr) nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = mergeStderr ? wr : nul;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = cmdline;
    const BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | flags, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!started) {
        CloseHandle(rd);
        return false;
    }
    char chunk[65536];
    DWORD got = 0;
    while (ReadFile(rd, chunk, sizeof(chunk), &got, nullptr) && got > 0) out.append(chunk, got);
    WaitForSingleObject(pi.hProcess, 20000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rd);
    return true;
}

bool RunCapture(const std::wstring& cmdline, std::string& out, bool mergeStderr) { return RunCaptureFlags(cmdline, out, mergeStderr, 0); }

VideoInfo FfmpegProbe(const std::wstring& path) {
    VideoInfo info;
    const std::wstring ff = FindFfmpeg();
    if (ff.empty()) return info;
    std::string text;
    if (!RunCapture(L"\"" + ff + L"\" -hide_banner -i \"" + path + L"\"", text, true)) return info;
    std::smatch m;
    if (std::regex_search(text, m, std::regex(R"(Duration: (\d+):(\d+):(\d+(?:\.\d+)?))"))) {
        info.seconds = std::stod(m[1]) * 3600 + std::stod(m[2]) * 60 + std::stod(m[3]);
    }
    {
        std::smatch fm;
        if (std::regex_search(text, fm, std::regex(R"((\d+(?:\.\d+)?) fps)"))) info.fps = std::stod(fm[1]);
    }
    const size_t v = text.find("Video:");
    if (v != std::string::npos) {
        const std::string line = text.substr(v, text.find('\n', v) - v);
        std::smatch sm;
        if (std::regex_search(line, sm, std::regex(R"(, (\d{2,5})x(\d{2,5}))"))) {
            info.width = std::stoi(sm[1]);
            info.height = std::stoi(sm[2]);
        }
    }
    return info;
}

bool FfmpegFrame(const std::wstring& path, std::vector<unsigned char>& rgba, int& width, int& height, VideoInfo& info) {
    const std::wstring ff = FindFfmpeg();
    if (ff.empty()) return false;
    info = FfmpegProbe(path);
    char ss[32];
    snprintf(ss, sizeof(ss), "%.2f", info.seconds > 1.0 ? std::min(info.seconds * 0.1, 3.0) : 0.0);
    std::string ppm;
    const std::wstring cmd = L"\"" + ff + L"\" -v error -ss " + std::wstring(ss, ss + std::strlen(ss)) + L" -i \"" + path +
                             L"\" -frames:v 1 -f image2pipe -c:v ppm -";
    if (!RunCapture(cmd, ppm, false) || ppm.size() < 16 || ppm[0] != 'P' || ppm[1] != '6') return false;
    // header "P6 W H 255" + one whitespace, then RGB bytes
    size_t pos = 2;
    long vals[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        while (pos < ppm.size() && std::isspace(static_cast<unsigned char>(ppm[pos]))) ++pos;
        char* end = nullptr;
        vals[i] = std::strtol(ppm.c_str() + pos, &end, 10);
        pos = static_cast<size_t>(end - ppm.c_str());
    }
    ++pos; // the single whitespace after maxval
    const long w = vals[0], h = vals[1];
    if (w <= 0 || h <= 0 || ppm.size() < pos + static_cast<size_t>(w) * h * 3) return false;
    rgba.assign(static_cast<size_t>(w) * h * 4, 255);
    const unsigned char* src = reinterpret_cast<const unsigned char*>(ppm.data()) + pos;
    for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i) {
        rgba[i * 4 + 0] = src[i * 3 + 0];
        rgba[i * 4 + 1] = src[i * 3 + 1];
        rgba[i * 4 + 2] = src[i * 3 + 2];
    }
    width = static_cast<int>(w);
    height = static_cast<int>(h);
    if (info.width == 0) { info.width = width; info.height = height; }
    return true;
}

// A dark picture with a play triangle, for clips whose codec Windows can't decode.
void MakePlaceholder(std::vector<unsigned char>& rgba, int& w, int& h) {
    w = 320;
    h = 180;
    rgba.assign(static_cast<size_t>(w) * h * 4, 255);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            unsigned char* p = &rgba[(static_cast<size_t>(y) * w + x) * 4];
            const unsigned char v = static_cast<unsigned char>(34 + y * 14 / h);
            p[0] = v; p[1] = static_cast<unsigned char>(v + 2); p[2] = static_cast<unsigned char>(v + 8); p[3] = 255;
        }
    }
    // triangle pointing right, centred
    const float cx = w * 0.5f + 8, cy = h * 0.5f, r = 34.0f;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float dx = x - cx, dy = std::fabs(y - cy);
            if (dx > -r * 0.8f && dx < r * 0.9f && dy < (r * 0.9f - dx) * 0.58f) {
                unsigned char* p = &rgba[(static_cast<size_t>(y) * w + x) * 4];
                p[0] = p[1] = p[2] = 150;
            }
        }
    }
}

// ---- finding JSON documents in the file's metadata ---------------------------------------------

// End of the JSON object starting at buf[start] (one past its closing brace), or npos.
size_t MatchBrace(const std::string& buf, size_t start, size_t limit) {
    int depth = 0;
    bool inString = false, escape = false;
    const size_t end = std::min(buf.size(), start + limit);
    for (size_t i = start; i < end; ++i) {
        const unsigned char c = static_cast<unsigned char>(buf[i]);
        if (inString) {
            if (escape) escape = false;
            else if (c == '\\') escape = true;
            else if (c == '"') inString = false;
            else if (c < 0x09) return std::string::npos; // binary data: not JSON
            continue;
        }
        if (c == '"') inString = true;
        else if (c == '{') ++depth;
        else if (c == '}') {
            if (--depth == 0) return i + 1;
        } else if (c < 0x09) {
            return std::string::npos;
        }
    }
    return std::string::npos;
}

bool LooksLikeStart(const std::string& buf, size_t i) {
    if (i + 2 >= buf.size() || buf[i] != '{' || buf[i + 1] != '"') return false;
    const std::string window = buf.substr(i, 110);
    static const char* kMarkers[] = {"last_node_id", "\"nodes\"", "class_type", "\"inputs\"", "\"prompt\"", "\"workflow\"", "\"version\""};
    for (const char* m : kMarkers) {
        if (window.find(m) != std::string::npos) return true;
    }
    return false;
}

// Python's json.dumps and ffmpeg's metadata escaping leave a few quirks; try the text as it is first.
json ParseLoose(std::string s) {
    json j = json::parse(s, nullptr, false);
    if (!j.is_discarded()) return j;
    std::string cleaned;
    cleaned.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size() && (s[i + 1] == '=' || s[i + 1] == ';' || s[i + 1] == '#')) continue;
        cleaned += s[i];
    }
    j = json::parse(cleaned, nullptr, false);
    if (!j.is_discarded()) return j;
    // NaN / Infinity are valid for Python's encoder
    for (const char* bad : {"NaN", "-Infinity", "Infinity"}) {
        size_t pos = 0;
        while ((pos = cleaned.find(bad, pos)) != std::string::npos) {
            cleaned.replace(pos, std::strlen(bad), "0");
            pos += 1;
        }
    }
    return json::parse(cleaned, nullptr, false);
}

bool IsApiPrompt(const json& j) {
    if (!j.is_object() || j.empty()) return false;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (it.value().is_object() && it.value().contains("class_type")) return true;
    }
    return false;
}

void AddDocument(const json& j, std::vector<RawField>& out, int depth = 0) {
    if (!j.is_object() || depth > 2) return;
    auto add = [&](const char* key, const json& doc) {
        const std::string text = doc.dump();
        for (const RawField& f : out) {
            if (f.value == text) return;
        }
        out.push_back({key, text});
    };
    if (j.contains("nodes") && j["nodes"].is_array()) {
        add("workflow", j);
    } else if (IsApiPrompt(j)) {
        add("prompt", j);
    } else {
        // wrapper such as VideoHelperSuite's {"prompt": {...}, "workflow": {...}}
        for (const char* key : {"workflow", "prompt"}) {
            if (!j.contains(key)) continue;
            const json& v = j[key];
            if (v.is_object()) AddDocument(v, out, depth + 1);
            else if (v.is_string()) AddDocument(ParseLoose(v.get<std::string>()), out, depth + 1);
        }
    }
}

// Videos can carry several unrelated "prompt" documents (a stale one next to the real one). Keep the one whose
// node ids match the workflow, and one workflow.
void ChooseDocuments(std::vector<RawField>& fields) {
    std::vector<RawField> workflows, prompts;
    for (RawField& f : fields) (f.key == "workflow" ? workflows : prompts).push_back(std::move(f));
    fields.clear();
    json bestWorkflow;
    size_t bestNodes = 0;
    std::string workflowText;
    for (const RawField& w : workflows) {
        json j = json::parse(w.value, nullptr, false);
        const size_t n = j.is_object() && j.contains("nodes") && j["nodes"].is_array() ? j["nodes"].size() : 0;
        if (workflowText.empty() || n > bestNodes) {
            bestNodes = n;
            workflowText = w.value;
            bestWorkflow = std::move(j);
        }
    }
    std::set<std::string> ids; // top-level node ids of the workflow
    if (bestWorkflow.is_object() && bestWorkflow.contains("nodes") && bestWorkflow["nodes"].is_array()) {
        for (const json& n : bestWorkflow["nodes"]) {
            if (n.is_object() && n.contains("id")) ids.insert(n["id"].dump());
        }
    }
    const RawField* best = nullptr;
    int bestScore = -1;
    for (const RawField& p : prompts) {
        json j = json::parse(p.value, nullptr, false);
        if (!j.is_object()) continue;
        int score = 0;
        for (auto it = j.begin(); it != j.end(); ++it) {
            const std::string key = it.key();
            if (ids.count(key.substr(0, key.find(':')))) ++score;
        }
        if (score > bestScore || (score == bestScore && best && p.value.size() > best->value.size())) {
            bestScore = score;
            best = &p;
        }
    }
    if (!workflowText.empty()) fields.push_back({"workflow", workflowText});
    if (best) fields.push_back({"prompt", best->value});
}

void ScanBuffer(const std::string& buf, std::vector<RawField>& out) {
    size_t i = 0;
    while (true) {
        i = buf.find("{\"", i);
        if (i == std::string::npos) break;
        if (!LooksLikeStart(buf, i)) {
            i += 2;
            continue;
        }
        const size_t end = MatchBrace(buf, i, 96u * 1024 * 1024);
        if (end == std::string::npos) {
            i += 2;
            continue;
        }
        if (end - i > 60) {
            json j = ParseLoose(buf.substr(i, end - i));
            if (!j.is_discarded()) AddDocument(j, out);
        }
        i = end;
    }
}

bool ReadRange(std::ifstream& f, unsigned long long pos, size_t len, std::string& out) {
    out.resize(len);
    f.clear();
    f.seekg(static_cast<std::streamoff>(pos));
    f.read(out.data(), static_cast<std::streamsize>(len));
    out.resize(static_cast<size_t>(f.gcount()));
    return !out.empty();
}

unsigned long long BE(const unsigned char* p, int n) {
    unsigned long long v = 0;
    for (int i = 0; i < n; ++i) v = (v << 8) | p[i];
    return v;
}

} // namespace

bool IsVideoPath(const std::wstring& path) {
    const std::wstring e = ExtOf(path);
    return e == L".mp4" || e == L".webm" || e == L".mkv" || e == L".mov" || e == L".m4v" || e == L".avi";
}

std::string FormatDuration(double seconds) {
    const int total = static_cast<int>(seconds + 0.5);
    char buf[24];
    if (total >= 3600) snprintf(buf, sizeof(buf), "%d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
    else snprintf(buf, sizeof(buf), "%d:%02d", total / 60, total % 60);
    return buf;
}

bool DecodeVideoFrameRGBA(const std::wstring& path, std::vector<unsigned char>& rgba, int& width, int& height, bool* decoded) {
    if (decoded) *decoded = false;
    ComScope com;
    bool ok = false;
    VideoInfo info;
    IMFSourceReader* reader = OpenReader(path);
    if (reader) {
        reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
        reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
        IMFMediaType* want = nullptr;
        MFCreateMediaType(&want);
        if (want) {
            want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            want->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        }
        HRESULT hr = want ? reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, want) : E_FAIL;
        SafeRelease(want);
        if (SUCCEEDED(hr)) {
            info.seconds = ReaderDuration(reader);
            IMFMediaType* cur = nullptr;
            UINT32 w = 0, h = 0;
            LONG stride = 0;
            if (SUCCEEDED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur))) {
                MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &w, &h);
                stride = static_cast<LONG>(MFGetAttributeUINT32(cur, MF_MT_DEFAULT_STRIDE, 0));
                SafeRelease(cur);
            }
            info.width = static_cast<int>(w);
            info.height = static_cast<int>(h);
            if (info.seconds > 1.0) {
                PROPVARIANT pos;
                PropVariantInit(&pos);
                pos.vt = VT_I8;
                pos.hVal.QuadPart = static_cast<LONGLONG>(std::min(info.seconds * 0.1, 3.0) * 1e7);
                reader->SetCurrentPosition(GUID_NULL, pos);
                PropVariantClear(&pos);
            }
            for (int attempt = 0; attempt < 40 && !ok && w > 0 && h > 0; ++attempt) {
                DWORD streamIndex = 0, flags = 0;
                LONGLONG ts = 0;
                IMFSample* sample = nullptr;
                if (FAILED(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &streamIndex, &flags, &ts, &sample))) break;
                if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) {
                    SafeRelease(sample);
                    break;
                }
                if (!sample) continue;
                IMFMediaBuffer* buffer = nullptr;
                if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)) && buffer) {
                    BYTE* data = nullptr;
                    DWORD maxLen = 0, curLen = 0;
                    if (SUCCEEDED(buffer->Lock(&data, &maxLen, &curLen)) && data) {
                        const LONG absStride = stride != 0 ? std::abs(stride) : static_cast<LONG>(w) * 4;
                        if (static_cast<size_t>(absStride) * h <= curLen) {
                            rgba.assign(static_cast<size_t>(w) * h * 4, 255);
                            for (UINT32 y = 0; y < h; ++y) {
                                const UINT32 srcRow = stride < 0 ? h - 1 - y : y;
                                const unsigned char* s = data + static_cast<size_t>(srcRow) * absStride;
                                unsigned char* d = &rgba[static_cast<size_t>(y) * w * 4];
                                for (UINT32 x = 0; x < w; ++x) { // BGRX -> RGBA
                                    d[x * 4 + 0] = s[x * 4 + 2];
                                    d[x * 4 + 1] = s[x * 4 + 1];
                                    d[x * 4 + 2] = s[x * 4 + 0];
                                    d[x * 4 + 3] = 255;
                                }
                            }
                            width = static_cast<int>(w);
                            height = static_cast<int>(h);
                            ok = true;
                        }
                        buffer->Unlock();
                    }
                    SafeRelease(buffer);
                }
                SafeRelease(sample);
            }
        }
        SafeRelease(reader);
    }
    if (!ok && FfmpegFrame(path, rgba, width, height, info)) ok = true; // WebM / MKV / unusual codecs
    t_last.path = path;
    t_last.info = info;
    if (decoded) *decoded = ok;
    if (!ok) MakePlaceholder(rgba, width, height);
    return true;
}

VideoInfo ProbeVideo(const std::wstring& path) {
    if (_wcsicmp(t_last.path.c_str(), path.c_str()) == 0) return t_last.info;
    ComScope com;
    VideoInfo info;
    if (IMFSourceReader* reader = OpenReader(path)) {
        info.seconds = ReaderDuration(reader);
        IMFMediaType* type = nullptr;
        if (SUCCEEDED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &type))) {
            UINT32 w = 0, h = 0;
            MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h);
            info.width = static_cast<int>(w);
            info.height = static_cast<int>(h);
            SafeRelease(type);
        }
        SafeRelease(reader);
    }
    if (info.seconds <= 0) {
        const VideoInfo viaFfmpeg = FfmpegProbe(path);
        if (viaFfmpeg.seconds > 0) info = viaFfmpeg;
    }
    t_last.path = path;
    t_last.info = info;
    return info;
}

std::vector<RawField> ReadVideoWorkflowFields(const std::wstring& path) {
    std::vector<RawField> out;
    std::ifstream f(path, std::ios::binary);
    if (!f) return out;
    f.seekg(0, std::ios::end);
    const unsigned long long size = static_cast<unsigned long long>(f.tellg());
    if (size < 16) return out;

    const std::wstring ext = ExtOf(path);
    bool scanned = false;
    if (ext == L".mp4" || ext == L".mov" || ext == L".m4v") {
        // walk the top-level atoms and read only the "moov" one (that is where the metadata lives)
        unsigned long long pos = 0;
        while (pos + 8 <= size) {
            std::string head;
            if (!ReadRange(f, pos, 16, head) || head.size() < 8) break;
            const unsigned char* h = reinterpret_cast<const unsigned char*>(head.data());
            unsigned long long len = BE(h, 4);
            const char* type = head.data() + 4;
            if (len == 1 && head.size() >= 16) len = BE(h + 8, 8);
            else if (len == 0) len = size - pos;
            if (len < 8 || pos + len > size + 16) break;
            if (std::memcmp(type, "moov", 4) == 0) {
                std::string moov;
                if (ReadRange(f, pos, static_cast<size_t>(std::min<unsigned long long>(len, 64ull * 1024 * 1024)), moov)) {
                    ScanBuffer(moov, out);
                    scanned = true;
                }
            }
            pos += len;
        }
    }
    if (!scanned) {
        // WebM / MKV (tags near either end) or an MP4 we couldn't walk: both ends of the file
        const size_t chunk = 4u * 1024 * 1024;
        std::string a, b;
        if (size <= chunk * 2) {
            if (ReadRange(f, 0, static_cast<size_t>(size), a)) ScanBuffer(a, out);
        } else {
            if (ReadRange(f, 0, chunk, a)) ScanBuffer(a, out);
            if (ReadRange(f, size - chunk, chunk, b)) ScanBuffer(b, out);
        }
    }
    ChooseDocuments(out);
    return out;
}

std::wstring FfmpegPath() { return FindFfmpeg(); }
VideoInfo ProbeVideoFfmpeg(const std::wstring& path) { return FfmpegProbe(path); }
bool RunCaptureHidden(const std::wstring& cmdline, std::string& out, bool mergeStderr) { return RunCapture(cmdline, out, mergeStderr); }
bool RunCaptureIdle(const std::wstring& cmdline, std::string& out, bool mergeStderr) { return RunCaptureFlags(cmdline, out, mergeStderr, IDLE_PRIORITY_CLASS); }
