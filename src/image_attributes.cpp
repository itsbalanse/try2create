#include "image_attributes.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <windows.h>

namespace {

enum Category { kFemale, kMale, kNeutral, kCategoryCount };

struct PersonWord {
    const char* word;
    Category category;
    bool plural;
};

const PersonWord kPersonWords[] = {
    {"girl", kFemale, false},   {"girls", kFemale, true},   {"woman", kFemale, false}, {"women", kFemale, true},
    {"lady", kFemale, false},   {"ladies", kFemale, true},  {"female", kFemale, false}, {"females", kFemale, true},
    {"boy", kMale, false},      {"boys", kMale, true},      {"man", kMale, false},      {"men", kMale, true},
    {"guy", kMale, false},      {"guys", kMale, true},      {"male", kMale, false},     {"males", kMale, true},
    {"person", kNeutral, false}, {"people", kNeutral, true}, {"child", kNeutral, false}, {"children", kNeutral, true},
    {"kid", kNeutral, false},   {"kids", kNeutral, true},
};

const PersonWord* FindPersonWord(const std::string& w) {
    for (const PersonWord& p : kPersonWords) {
        if (w == p.word) return &p;
    }
    return nullptr;
}

// "one".."six" / "a" / "an" / "2" -> count, or 0 if it isn't a count.
int CountWord(const std::string& w) {
    static const char* kNames[] = {"", "one", "two", "three", "four", "five", "six"};
    for (int i = 1; i <= 6; ++i) {
        if (w == kNames[i]) return i;
    }
    if (w == "a" || w == "an") return 1;
    if (!w.empty() && std::all_of(w.begin(), w.end(), [](unsigned char c) { return std::isdigit(c); }) && w.size() <= 2) {
        return std::min(6, std::atoi(w.c_str()));
    }
    return 0;
}

struct Token {
    std::string text;
    bool afterSingleSpace = false; // separated from the previous token by exactly one space
};

std::vector<Token> Tokenize(const std::string& lower) {
    std::vector<Token> out;
    size_t i = 0;
    while (i < lower.size()) {
        size_t sepStart = i;
        while (i < lower.size() && !(std::isalnum(static_cast<unsigned char>(lower[i])) || lower[i] == '+')) ++i;
        if (i >= lower.size()) break;
        size_t start = i;
        while (i < lower.size() && (std::isalnum(static_cast<unsigned char>(lower[i])) || lower[i] == '+')) ++i;
        Token t;
        t.text = lower.substr(start, i - start);
        t.afterSingleSpace = (start - sepStart == 1 && lower[sepStart] == ' ');
        out.push_back(std::move(t));
    }
    return out;
}

} // namespace

void FileTimeYearMonth(std::filesystem::file_time_type t, int& year, int& month) {
    year = month = 0;
    // MSVC's file_time_type counts 100 ns ticks since 1601, i.e. a FILETIME.
    ULARGE_INTEGER u;
    u.QuadPart = static_cast<unsigned long long>(t.time_since_epoch().count());
    FILETIME ft{u.LowPart, u.HighPart};
    SYSTEMTIME utc, local;
    if (!FileTimeToSystemTime(&ft, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return;
    year = local.wYear;
    month = local.wMonth;
}

int EstimatePeopleCount(const std::string& prompt) {
    if (prompt.empty()) return -1;
    std::string lower(prompt.size(), ' ');
    std::transform(prompt.begin(), prompt.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.find("no humans") != std::string::npos || lower.find("no people") != std::string::npos) return 0;

    std::vector<Token> tokens = Tokenize(lower);
    int counts[kCategoryCount] = {0, 0, 0};
    bool solo = false;
    int atLeast = 0;

    for (size_t i = 0; i < tokens.size(); ++i) {
        const std::string& tok = tokens[i].text;
        if (tok == "solo") { solo = true; continue; }
        if (tok == "duo" || tok == "couple") { atLeast = std::max(atLeast, 2); continue; }
        if (tok == "trio") { atLeast = std::max(atLeast, 3); continue; }
        if (tok == "crowd") { atLeast = 6; continue; }

        // "2girls", "6+boys"
        size_t d = 0;
        while (d < tok.size() && std::isdigit(static_cast<unsigned char>(tok[d]))) ++d;
        if (d > 0 && d < tok.size()) {
            size_t rest = d;
            if (tok[rest] == '+') ++rest;
            if (const PersonWord* p = FindPersonWord(tok.substr(rest))) {
                int n = std::min(6, std::atoi(tok.substr(0, d).c_str()));
                if (n > 0) counts[p->category] = std::max(counts[p->category], n);
            }
            continue;
        }

        if (const PersonWord* p = FindPersonWord(tok)) {
            int n = 0;
            if (i > 0 && tokens[i].afterSingleSpace) {
                const std::string& prev = tokens[i - 1].text;
                n = CountWord(prev);
                if (n == 0 && (prev == "multiple" || prev == "several" || prev == "many" || prev == "two")) n = 2;
            }
            if (n == 0) n = p->plural ? 2 : 1;
            counts[p->category] = std::max(counts[p->category], n);
        }
    }

    int total = counts[kFemale] + counts[kMale] + counts[kNeutral];
    total = std::max(total, atLeast);
    if (total == 0 && solo) total = 1;
    if (total == 0) return -1;
    return std::min(total, 6);
}

std::string PeopleLabel(int count) {
    if (count < 0) return "unknown";
    if (count == 0) return "none";
    if (count == 1) return "1 (solo)";
    if (count >= 6) return "6+";
    return std::to_string(count);
}

bool ParseSizeParam(const std::string& value, int& width, int& height) {
    size_t x = value.find_first_of("xX");
    if (x == std::string::npos || x == 0 || x + 1 >= value.size()) return false;
    char* end1 = nullptr;
    char* end2 = nullptr;
    long w = std::strtol(value.c_str(), &end1, 10);
    long h = std::strtol(value.c_str() + x + 1, &end2, 10);
    if (end1 != value.c_str() + x || w <= 0 || h <= 0 || w > 100000 || h > 100000) return false;
    (void)end2;
    width = static_cast<int>(w);
    height = static_cast<int>(h);
    return true;
}

const char* MonthName(int month) {
    static const char* kNames[] = {"January", "February", "March",     "April",   "May",      "June",
                                   "July",    "August",   "September", "October", "November", "December"};
    return (month >= 1 && month <= 12) ? kNames[month - 1] : "";
}

int ParseMonth(const std::string& s) {
    if (s.empty()) return 0;
    if (std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); })) {
        int m = std::atoi(s.c_str());
        return (m >= 1 && m <= 12) ? m : 0;
    }
    if (s.size() < 3) return 0;
    for (int m = 1; m <= 12; ++m) {
        std::string name = MonthName(m);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (name.compare(0, s.size(), s) == 0) return m; // "mar", "march"
    }
    return 0;
}

const char* GeneratorName(Generator g) {
    switch (g) {
        case Generator::Automatic1111: return "AUTOMATIC1111 / compatible fork";
        case Generator::ComfyUI: return "ComfyUI";
        case Generator::NovelAI: return "NovelAI";
        case Generator::InvokeAI: return "InvokeAI";
        case Generator::Generic: return "Unrecognized format";
        default: return "No metadata";
    }
}

std::string BuildA1111Text(const ImageMetadata& meta) {
    std::string out = meta.parsed.positivePrompt;
    if (!meta.parsed.negativePrompt.empty()) out += "\nNegative prompt: " + meta.parsed.negativePrompt;
    std::string settings;
    for (const auto& kv : meta.parsed.params) {
        if (kv.first.empty()) continue;
        if (!settings.empty()) settings += ", ";
        settings += kv.first + ": " + kv.second;
    }
    if (!settings.empty()) out += "\n" + settings;
    return out;
}

std::string TopFolderOf(const std::wstring& relativePath) {
    size_t slash = relativePath.find_first_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    std::wstring first = relativePath.substr(0, slash);
    int len = WideCharToMultiByte(CP_UTF8, 0, first.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(len > 0 ? len - 1 : 0, '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, first.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

std::string FormatLocalDateTime(std::filesystem::file_time_type t) {
    ULARGE_INTEGER u;
    u.QuadPart = static_cast<unsigned long long>(t.time_since_epoch().count());
    FILETIME ft{u.LowPart, u.HighPart};
    SYSTEMTIME utc, local;
    if (!FileTimeToSystemTime(&ft, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return {};
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d", local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute);
    return buf;
}

// Measured on a thumbnail: no colour at all, then which of the three it looks like. A depth map has no fine detail (a black and white photo does), a mask
// is two tones with little edge, a pose drawing is thin lines on white, an OpenPose skeleton is coloured sticks on black.
int HelperPixels(const unsigned char* px, int w, int h) {
    if (!px || w < 16 || h < 16) return 0;
    const int n = w * h;
    int see = 0, gray = 0, vivid = 0, black = 0, white = 0, light = 0, line = 0, edges = 0;
    int texture = 0; // fine detail: small steps between neighbours (a depth map has sharp outlines, but nothing inside them)
    auto lum = [&](int x, int y) {
        const unsigned char* p = px + (static_cast<size_t>(y) * w + x) * 4;
        return (p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8;
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const unsigned char* p = px + (static_cast<size_t>(y) * w + x) * 4;
            if (p[3] < 250) continue; // (see-through: a cut-out, counted below)
            ++see;
            const int hi = std::max({p[0], p[1], p[2]}), lo = std::min({p[0], p[1], p[2]}), l = lum(x, y);
            if (hi - lo <= 10) ++gray;
            else if (hi - lo > 60) ++vivid;
            if (l < 14) ++black;
            if (l > 242) ++white;
            if (l > 225) ++light;
            else if (l > 14 && l < 215) ++line;
            if (x + 1 < w && y + 1 < h) {
                const int d = std::abs(l - lum(x + 1, y)) + std::abs(l - lum(x, y + 1));
                if (d >= 4 && d <= 48) ++texture;
                if (d > 48) ++edges;
            }
        }
    if (see < n * 0.95) return 0;
    const double N = see;
    if (black > N * 0.72 && vivid > N * 0.008 && black + vivid > N * 0.85) return 3; // coloured sticks on black
    if (gray < N * 0.985) return 0;
    if (black + white > N * 0.96 && edges < N * 0.07) return 2;
    if (light > N * 0.62 && black < N * 0.04 && line > N * 0.01 && line < N * 0.34) return 3; // lines on white
    if (texture < N * 0.25) return 1; // (measured: depth maps 0.12-0.19, black and white photos 0.43-0.67, one blurred hard still 0.57)
    return 0;
}

namespace {
bool OneOf(const std::string& s, std::initializer_list<const char*> list) {
    for (const char* w : list)
        if (s == w) return true;
    return false;
}
}

int HelperKind(const std::wstring& relativePath, int pixels, const std::string& positivePrompt) {
    std::string path; // (the names are ASCII: anything else just never matches)
    for (wchar_t c : relativePath) path += c < 128 ? static_cast<char>(std::tolower(static_cast<int>(c))) : '?';
    auto kindOf = [](const std::string& t) {
        if (OneOf(t, {"depth", "depths", "depthmap", "depthmaps", "zdepth"})) return 1;
        if (OneOf(t, {"mask", "masks", "matte", "mattes", "trimap"})) return 2;
        if (OneOf(t, {"pose", "poses", "openpose", "dwpose", "canny", "lineart", "normalmap", "controlnet"})) return 3;
        return 0;
    };
    size_t start = 0;
    for (size_t at; (at = path.find_first_of("\\/", start)) != std::string::npos; start = at + 1) // a folder of them
        if (const int k = kindOf(path.substr(start, at - start))) return k;
    std::string stem = path.substr(start);
    if (const size_t dot = stem.rfind('.'); dot != std::string::npos) stem.resize(dot);
    if (const size_t fr = stem.rfind(".frame"); fr != std::string::npos && (stem.find(".mp4") != std::string::npos || stem.find(".webm") != std::string::npos)) return 2; // (a video's own frames)
    std::vector<std::string> words;
    std::string cur;
    for (char c : stem + " ") {
        if (std::isalnum(static_cast<unsigned char>(c))) cur += c;
        else if (!cur.empty()) words.push_back(cur), cur.clear();
    }
    auto allDigits = [](const std::string& s) { return s.find_first_not_of("0123456789") == std::string::npos; };
    bool counted = false; // (ComfyUI's _00012_ on the end)
    while (!words.empty() && allDigits(words.back())) words.pop_back(), counted = true;
    if (!words.empty() && !allDigits(words.front())) { // (a number first is "00497-1436313974-a woman in a mask": a prompt, not a name)
        if (const int k = kindOf(words.back()); k && (counted || words.size() <= 4)) return k;
        if (const int k = kindOf(words.front()); k && words.size() <= 4) return k;
    }
    if (pixels == 0) return 0;
    std::string low = positivePrompt;
    for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* w : {"monochrome", "greyscale", "grayscale", "black and white", "black-and-white", "b&w", "noir", "sketch", "line art", "lineart", "manga", "coloring", "colouring",
                          "ink ", "pencil", "silhouette", "charcoal", "storyboard"})
        if (low.find(w) != std::string::npos) return 0;
    return pixels;
}
