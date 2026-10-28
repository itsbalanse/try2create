#include "rename_template.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <windows.h>

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

std::wstring LowerW(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return s;
}

std::string FindParam(const KeyValueList& params, const char* key) {
    for (const auto& kv : params) {
        if (_stricmp(kv.first.c_str(), key) == 0) return kv.second;
    }
    return {};
}

bool LocalTime(std::filesystem::file_time_type t, SYSTEMTIME& out) {
    ULARGE_INTEGER u;
    u.QuadPart = static_cast<unsigned long long>(t.time_since_epoch().count());
    FILETIME ft{u.LowPart, u.HighPart};
    SYSTEMTIME utc;
    return FileTimeToSystemTime(&ft, &utc) && SystemTimeToTzSpecificLocalTime(nullptr, &utc, &out);
}

const char* GeneratorShort(Generator g) {
    switch (g) {
        case Generator::Automatic1111: return "a1111";
        case Generator::ComfyUI: return "comfyui";
        case Generator::NovelAI: return "novelai";
        case Generator::InvokeAI: return "invokeai";
        case Generator::Generic: return "other";
        default: return "nometa";
    }
}

std::string StripModelExtension(std::string m) {
    size_t slash = m.find_last_of("\\/");
    if (slash != std::string::npos) m = m.substr(slash + 1);
    for (const char* ext : {".safetensors", ".ckpt", ".pt", ".pth", ".bin", ".gguf"}) {
        size_t n = strlen(ext);
        if (m.size() > n && _stricmp(m.c_str() + m.size() - n, ext) == 0) {
            m.erase(m.size() - n);
            break;
        }
    }
    return m;
}

// First `maxChars` characters of the prompt, cut at a UTF-8 boundary.
std::string Truncate(const std::string& s, size_t maxChars) {
    size_t chars = 0, i = 0;
    while (i < s.size() && chars < maxChars) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t step = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
        i += step;
        ++chars;
    }
    return s.substr(0, std::min(i, s.size()));
}

} // namespace

std::string SanitizeFileName(const std::string& input, size_t maxBytes) {
    std::string s;
    bool lastSpace = false;
    for (unsigned char c : input) {
        if (c < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') c = '_';
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (lastSpace) continue;
            c = ' ';
            lastSpace = true;
        } else {
            lastSpace = false;
        }
        s.push_back(static_cast<char>(c));
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    if (s.size() > maxBytes) {
        size_t cut = maxBytes;
        while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut; // don't split a character
        s.erase(cut);
        while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
    }
    // Reserved device names (CON, NUL, COM1, ...) are invalid even with an extension.
    std::string stem = s.substr(0, s.find('.'));
    std::string lower = stem;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const std::set<std::string> kReserved = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6",
                                                    "com7", "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6",
                                                    "lpt7", "lpt8", "lpt9"};
    if (kReserved.count(lower)) s = "_" + s;
    return s;
}

const std::vector<std::pair<const char*, const char*>>& RenameTokenHelp() {
    static const std::vector<std::pair<const char*, const char*>> kHelp = {
        {"{name}", "original file name"},
        {"{n}  {n:4}", "running number, optionally zero-padded"},
        {"{date}", "file date, 2026-03-05"},
        {"{time}", "file time, 142530"},
        {"{year} {month} {day}", "parts of the file date"},
        {"{monthname}", "March"},
        {"{model}", "checkpoint name"},
        {"{sampler}", "sampler"},
        {"{steps} {cfg} {seed}", "generation settings"},
        {"{size} {w} {h}", "generated size, 512x768"},
        {"{generator}", "a1111, comfyui, novelai, invokeai"},
        {"{people}", "people in the image"},
        {"{rating}", "star rating, 0-5"},
        {"{prompt}  {prompt:30}", "start of the prompt (default 40 characters)"},
        {"{folder}", "name of the containing folder"},
    };
    return kHelp;
}

std::string ExpandRenameTemplate(const std::string& tmpl, const ImageEntry& e, int sequence, std::string* error) {
    std::string out;
    SYSTEMTIME st = {};
    const bool haveTime = LocalTime(e.mtime, st);
    char buf[64];
    for (size_t i = 0; i < tmpl.size(); ++i) {
        if (tmpl[i] != '{') {
            out.push_back(tmpl[i]);
            continue;
        }
        size_t close = tmpl.find('}', i);
        if (close == std::string::npos) {
            if (error) *error = "Missing } in the template";
            break;
        }
        std::string token = tmpl.substr(i + 1, close - i - 1);
        i = close;
        std::string arg;
        size_t colon = token.find(':');
        if (colon != std::string::npos) {
            arg = token.substr(colon + 1);
            token.erase(colon);
        }
        std::transform(token.begin(), token.end(), token.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const KeyValueList& params = e.metadata.parsed.params;
        std::string value;
        if (token == "name") {
            value = ToUtf8(fs::path(e.displayName).stem().wstring());
        } else if (token == "n") {
            int width = std::clamp(arg.empty() ? 1 : std::atoi(arg.c_str()), 1, 9);
            snprintf(buf, sizeof(buf), "%0*d", width, sequence);
            value = buf;
        } else if (token == "date") {
            if (haveTime) snprintf(buf, sizeof(buf), "%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
            value = haveTime ? buf : "nodate";
        } else if (token == "time") {
            if (haveTime) snprintf(buf, sizeof(buf), "%02d%02d%02d", st.wHour, st.wMinute, st.wSecond);
            value = haveTime ? buf : "notime";
        } else if (token == "year") {
            value = haveTime ? std::to_string(st.wYear) : "0000";
        } else if (token == "month") {
            if (haveTime) snprintf(buf, sizeof(buf), "%02d", st.wMonth);
            value = haveTime ? buf : "00";
        } else if (token == "monthname") {
            value = haveTime ? MonthName(st.wMonth) : "unknown";
        } else if (token == "day") {
            if (haveTime) snprintf(buf, sizeof(buf), "%02d", st.wDay);
            value = haveTime ? buf : "00";
        } else if (token == "model") {
            value = StripModelExtension(FindParam(params, "Model"));
            if (value.empty()) value = "nomodel";
        } else if (token == "sampler") {
            value = FindParam(params, "Sampler");
            if (value.empty()) value = "nosampler";
        } else if (token == "steps") {
            value = FindParam(params, "Steps");
            if (value.empty()) value = "x";
        } else if (token == "cfg") {
            value = FindParam(params, "CFG scale");
            if (value.empty()) value = "x";
        } else if (token == "seed") {
            value = FindParam(params, "Seed");
            if (value.empty()) value = "noseed";
        } else if (token == "size") {
            value = e.genW > 0 ? std::to_string(e.genW) + "x" + std::to_string(e.genH) : "nosize";
        } else if (token == "w") {
            value = std::to_string(e.genW);
        } else if (token == "h") {
            value = std::to_string(e.genH);
        } else if (token == "generator") {
            value = GeneratorShort(e.metadata.generator);
        } else if (token == "people") {
            value = e.people < 0 ? "unk" : std::to_string(e.people);
        } else if (token == "rating") {
            value = std::to_string(e.rating);
        } else if (token == "prompt") {
            size_t n = arg.empty() ? 40 : static_cast<size_t>(std::clamp(std::atoi(arg.c_str()), 1, 120));
            value = Truncate(e.metadata.parsed.positivePrompt, n);
            if (value.empty()) value = "noprompt";
        } else if (token == "folder") {
            value = ToUtf8(fs::path(e.path).parent_path().filename().wstring());
        } else {
            if (error && error->empty()) *error = "Unknown token {" + token + "}";
            continue;
        }
        out += SanitizeFileName(value, 80);
    }
    return SanitizeFileName(out, 150);
}

std::vector<RenamePlanItem> BuildRenamePlan(const AppState& app, const std::vector<int>& indices, const std::string& tmpl,
                                            int startNumber, std::string* error) {
    std::vector<RenamePlanItem> plan;
    if (tmpl.empty()) {
        if (error) *error = "Enter a template first";
        return plan;
    }
    std::set<std::wstring> sources, taken;
    for (int idx : indices) {
        if (idx >= 0 && idx < static_cast<int>(app.entries.size())) sources.insert(LowerW(app.entries[idx].path));
    }
    int seq = startNumber;
    for (int idx : indices) {
        if (idx < 0 || idx >= static_cast<int>(app.entries.size())) continue;
        const ImageEntry& e = app.entries[idx];
        std::string err;
        std::string baseUtf8 = ExpandRenameTemplate(tmpl, e, seq++, &err);
        if (!err.empty() && error && error->empty()) *error = err;
        fs::path from(e.path);
        if (baseUtf8.empty()) baseUtf8 = ToUtf8(from.stem().wstring());
        std::wstring base = ToWide(baseUtf8);
        const std::wstring ext = from.extension().wstring();

        RenamePlanItem item;
        item.index = idx;
        item.from = e.path;
        auto candidate = [&](int n) {
            std::wstring name = base + (n > 1 ? L" (" + std::to_wstring(n) + L")" : L"") + ext;
            return (from.parent_path() / name).wstring();
        };
        for (int n = 1; n < 10000; ++n) {
            std::wstring to = candidate(n);
            std::wstring lower = LowerW(to);
            std::error_code ec;
            const bool sameFile = lower == LowerW(e.path);
            const bool clashesOnDisk = !sameFile && !sources.count(lower) && fs::exists(to, ec);
            if (taken.count(lower) || clashesOnDisk) {
                item.adjusted = true;
                continue;
            }
            taken.insert(lower);
            item.to = to;
            item.changed = to != e.path; // a case-only change still counts
            break;
        }
        if (item.to.empty()) item.to = e.path; // gave up: leave it alone
        plan.push_back(std::move(item));
    }
    return plan;
}
