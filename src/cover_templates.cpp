// Song covers from templates, drawn by photocraft-cli: the picture squared by ffmpeg, then PhotoCraft engine commands (shape fades, type, layer
// styles, adjustment layers) laid out on a 1080 x 1080 grid and scaled to the size asked for. The title is broken into lines and, after a first
// run, made smaller wherever PhotoCraft says it came out wider than its space.
#include "cover_templates.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <set>
#include <sstream>
#include <vector>

#include "json.hpp"
#include "video_util.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// tools\photocraft (any version folder), looked for until it is there (the setup can download it while the app runs)
std::wstring Cli() {
    static std::wstring found;
    static ULONGLONG lookedAt = 0;
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    if (found.empty() && (!lookedAt || GetTickCount64() - lookedAt > 3000)) lookedAt = GetTickCount64(), found = FindPhotocraft(L"photocraft-cli.exe");
    return found;
}

// one argument quoted the way CommandLineToArgvW reads it back
std::wstring QuoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    for (size_t i = 0; i < a.size(); ++i) {
        size_t backslashes = 0;
        while (i < a.size() && a[i] == L'\\') {
            ++backslashes;
            ++i;
        }
        if (i == a.size()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (a[i] == L'"') out.append(backslashes * 2 + 1, L'\\');
        else out.append(backslashes, L'\\');
        out += a[i];
    }
    return out + L"\"";
}

// the fonts PhotoCraft has (asked once): each template names a few, the first one there is used
const std::set<std::string>& Fonts() {
    static std::set<std::string> have;
    static bool asked = false;
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    if (asked) return have;
    asked = true;
    std::string out;
    RunCaptureHidden(QuoteArg(Cli()) + L" run --new " + QuoteArg(L"{\"width\":64,\"height\":64}") + L" --cmd type.fonts", out, false);
    const size_t b = out.find('{');
    const json j = json::parse(b == std::string::npos ? out : out.substr(b, out.find('\n', b) == std::string::npos ? std::string::npos : out.find('\n', b) - b), nullptr, false);
    if (j.is_object() && j.contains("result") && j["result"].contains("families"))
        for (const json& f : j["result"]["families"]) {
            if (f.is_string()) have.insert(f.get<std::string>());
        }
    return have;
}

std::string Font(std::initializer_list<const char*> wanted) {
    const std::set<std::string>& have = Fonts();
    for (const char* f : wanted) {
        if (have.count(f)) return f;
    }
    return have.count("Arial") || have.empty() ? "Arial" : *have.begin();
}

std::string Upper(std::string s) {
    for (char& c : s) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
    }
    return s;
}

std::string Lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    }
    return s;
}

// the title in lines of about `chars` characters (at most `maxLines`; PhotoCraft breaks lines at \r)
std::string Lines(const std::string& text, size_t chars, int maxLines, int* count = nullptr) {
    std::vector<std::string> words;
    std::istringstream in(text);
    for (std::string w; in >> w;) words.push_back(w);
    // as many lines as it needs, then balanced: "THE SUIT AND / THE SMUDGE", not "THE SUIT AND THE / SMUDGE"
    const size_t total = text.size();
    const size_t need = std::clamp<size_t>((total + chars - 1) / std::max<size_t>(1, chars), 1, static_cast<size_t>(std::max(1, maxLines)));
    const size_t target = std::max<size_t>(1, (total + need - 1) / need);
    std::vector<std::string> lines;
    for (const std::string& w : words) {
        if (!lines.empty() && (lines.back().size() + 1 + w.size() <= target || lines.back().size() + 1 + w.size() / 2 <= target))
            lines.back() += " " + w;
        else if (static_cast<int>(lines.size()) < maxLines) lines.push_back(w);
        else lines.back() += " " + w;
    }
    if (count) *count = std::max<int>(1, static_cast<int>(lines.size()));
    std::string out;
    for (const std::string& l : lines) out += (out.empty() ? "" : "\r") + l;
    return out;
}

// A template's commands, written on a 1080 grid and scaled by k
struct Steps {
    double k = 1;
    json list = json::array();
    std::vector<std::pair<size_t, double>> fit; // title steps: (step, the widest it may be, in px)
    double P(double v) const { return std::round(v * k); }
    void Add(const char* cmd, json params) { list.push_back({{"cmd", cmd}, {"params", std::move(params)}}); }
    // a rectangle filled with a gradient (stops: [[t, "#rrggbbaa"], ...]); angle -90 runs top to bottom, 0 left to right
    void Fade(double x, double y, double w, double h, json stops, double angle, const char* style = "linear") {
        Add("shape.create", {{"kind", "rect"}, {"rect", {P(x), P(y), P(w), P(h)}}, {"fill", {{"gradient", {{"stops", std::move(stops)}, {"angle", angle}, {"style", style}}}}}});
    }
    void Box(const char* kind, double x, double y, double w, double h, json fill, json stroke = nullptr, double radius = 0) {
        json p = {{"kind", kind}, {"rect", {P(x), P(y), P(w), P(h)}}, {"fill", std::move(fill)}, {"stroke", std::move(stroke)}};
        if (radius > 0) p["radii"] = P(radius);
        Add("shape.create", std::move(p));
    }
    // point text: x,y is the first line's baseline anchor; maxW > 0 marks it as the title (fitted to that width)
    void Text(double x, double y, const std::string& text, const std::string& font, double size, const char* color, const char* align, double maxW = 0) {
        if (text.empty()) return;
        if (maxW > 0) fit.push_back({list.size(), P(maxW)});
        Add("type.create", {{"x", P(x)}, {"y", P(y)}, {"text", text}, {"align", align}, {"font", font}, {"size", P(size)}, {"color", color}});
    }
    void Style(json params) { Add("type.setStyle", std::move(params)); }
    void Shadow(double distance, double size, int opacity = 60, const char* color = "#000000") {
        Add("layer.layerStyle.dropShadow", {{"color", color}, {"opacity", opacity}, {"distance", P(distance)}, {"size", P(size)}, {"angle", 120}});
    }
    void Glow(const char* color, double size, int spread = 8, int opacity = 85) {
        Add("layer.layerStyle.outerGlow", {{"color", color}, {"size", P(size)}, {"spread", spread}, {"opacity", opacity}, {"technique", "softer"}});
    }
    void Stroke(const char* color, double size) { Add("layer.layerStyle.stroke", {{"color", color}, {"size", P(size)}, {"position", "outside"}}); }
};

struct Template {
    const char* name;
    const char* from; // the background when there is no picture (a gradient)
    const char* to;
};
const Template kTemplates[] = {
    {"Classic", "#2b2d42", "#0b0c10"},  {"Bold", "#ff512f", "#1a1a2e"},   {"Polaroid", "#d9cbb3", "#8c7b65"},  {"Neon", "#3a0ca3", "#10002b"},
    {"Vintage", "#c9a66b", "#5a3a1a"},  {"Minimal", "#4a5568", "#1a202c"}, {"Label", "#9e9e9e", "#2b2b2b"},     {"Script", "#5f4b8b", "#1d1534"},
    {"Lo-fi", "#7b6cd9", "#241b4b"},    {"Poster", "#e94560", "#16213e"},
};

void Build(int t, Steps& s, const std::string& title, const std::string& line2) {
    int n = 1;
    switch (t) {
    case 0: { // Classic: a dark fade at the bottom, a serif title, the second line spaced out
        s.Fade(0, 520, 1080, 560, json::array({json::array({0, "#00000000"}), json::array({1, "#000000e6"})}), -90);
        const std::string tl = Lines(Upper(title), 22, 2, &n);
        s.Text(540, 945 - (n - 1) * 78, tl, Font({"Bodoni MT", "Georgia", "Times New Roman"}), 72, "#ffffff", "center", 960);
        s.Shadow(4, 10);
        s.Text(540, 1010, Upper(line2), Font({"Century Gothic", "Segoe UI", "Arial"}), 22, "#dddddd", "center");
        s.Style({{"tracking", 400}});
        break;
    }
    case 1: { // Bold: a huge condensed title at the top, a yellow line under it
        s.Fade(0, 0, 1080, 560, json::array({json::array({0, "#000000c8"}), json::array({1, "#00000000"})}), -90);
        const std::string tl = Lines(Upper(title), 14, 3, &n);
        s.Text(540, 190, tl, Font({"Impact", "Haettenschweiler", "Arial Black"}), 150, "#ffffff", "center", 990);
        s.Shadow(6, 14, 70);
        s.Text(540, 190 + n * 160 - 40, Upper(line2), Font({"Bahnschrift", "Segoe UI", "Arial"}), 30, "#ffd400", "center");
        s.Style({{"tracking", 250}});
        break;
    }
    case 2: { // Polaroid: the picture in a cream frame, the title handwritten on the wide bottom edge
        s.Add("image.imageSize", {{"width", s.P(960)}, {"height", s.P(960)}});
        s.Add("image.crop", {{"x", 0}, {"y", s.P(40)}, {"width", s.P(960)}, {"height", s.P(840)}});
        s.Add("image.canvasSize", {{"width", s.P(960)}, {"height", s.P(900)}, {"anchor", "bottom"}, {"extensionColor", "#f5f1e8"}});
        s.Add("image.canvasSize", {{"width", s.P(1080)}, {"height", s.P(1080)}, {"anchor", "top"}, {"extensionColor", "#f5f1e8"}});
        s.Text(540, 995, Lines(title, 40, 1), Font({"Segoe Print", "Ink Free", "Lucida Handwriting"}), 54, "#2a2a2a", "center", 960);
        s.Text(540, 1050, line2, Font({"Segoe Print", "Ink Free", "Lucida Handwriting"}), 24, "#7a7266", "center");
        break;
    }
    case 3: { // Neon: the picture darkened, a glowing pink title, a cyan line
        s.Add("layer.newAdjustmentLayer.brightnessContrast", {{"brightness", -55}, {"contrast", 25}});
        s.Add("layer.newAdjustmentLayer.hueSaturation", {{"saturation", 20}});
        const std::string tl = Lines(Upper(title), 16, 3, &n);
        s.Text(540, 900 - (n - 1) * 112, tl, Font({"Bahnschrift", "Segoe UI", "Arial"}), 108, "#ffe8fb", "center", 960);
        s.Glow("#ff2bd6", 34, 12, 90);
        s.Text(540, 990, Upper(line2), Font({"Bahnschrift", "Segoe UI", "Arial"}), 30, "#b8fbff", "center");
        s.Style({{"tracking", 300}});
        s.Glow("#00e5ff", 18, 8, 80);
        break;
    }
    case 4: { // Vintage: sepia, a cream band with a brown border, a round retro title
        s.Add("layer.newAdjustmentLayer.photoFilter", {{"filter", "sepia"}, {"density", 60}});
        s.Add("layer.newAdjustmentLayer.brightnessContrast", {{"brightness", -5}, {"contrast", 15}});
        s.Box("roundedRect", 80, 790, 920, 210, "#f2e6cff0", {{"width", s.P(4)}, {"color", "#5a3a1a"}}, 14);
        const std::string tl = Lines(title, 26, 2, &n);
        s.Text(540, 885 - (n - 1) * 36, tl, Font({"Cooper Black", "Rockwell Extra Bold", "Georgia"}), 66, "#5a3a1a", "center", 860);
        s.Text(540, 965, line2, Font({"Georgia", "Garamond", "Times New Roman"}), 26, "#7a5a3a", "center");
        s.Style({{"italic", true}});
        break;
    }
    case 5: { // Minimal: a light fade, a thin line, a quiet lowercase title bottom left
        s.Fade(0, 640, 1080, 440, json::array({json::array({0, "#00000000"}), json::array({1, "#000000a0"})}), -90);
        s.Box("rect", 80, 905, 70, 4, "#ffffff");
        const std::string tl = Lines(Lower(title), 30, 2, &n);
        s.Text(80, 985 - (n - 1) * 60, tl, Font({"Segoe UI", "Century Gothic", "Arial"}), 56, "#ffffff", "left", 920);
        s.Style({{"weight", 300}});
        s.Text(80, 1032, Lower(line2), Font({"Segoe UI", "Century Gothic", "Arial"}), 22, "#d8d8d8", "left");
        s.Style({{"tracking", 200}});
        break;
    }
    case 6: { // Label: black and white, a red band with a heavy white title
        s.Add("layer.newAdjustmentLayer.blackWhite", json::object());
        s.Add("layer.newAdjustmentLayer.brightnessContrast", {{"brightness", -5}, {"contrast", 20}});
        s.Box("rect", 0, 810, 1080, 200, "#d7262e");
        const std::string tl = Lines(Upper(title), 24, 2, &n);
        s.Text(60, 905 - (n - 1) * 70, tl, Font({"Franklin Gothic Heavy", "Arial Black", "Impact"}), 74, "#ffffff", "left", 960);
        s.Text(60, 968, Upper(line2), Font({"Bahnschrift", "Arial", "Segoe UI"}), 24, "#ffd9d9", "left");
        s.Style({{"tracking", 300}});
        break;
    }
    case 7: { // Script: a soft vignette, a flowing title in the middle
        s.Fade(0, 0, 1080, 1080, json::array({json::array({0, "#00000000"}), json::array({0.55, "#00000030"}), json::array({1, "#000000b4"})}), 90, "radial");
        const std::string tl = Lines(title, 22, 2, &n);
        s.Text(540, 900 - (n - 1) * 125, tl, Font({"Segoe Script", "Brush Script MT", "Lucida Handwriting"}), 92, "#ffffff", "center", 980);
        s.Shadow(3, 16, 75);
        s.Text(540, 1000, Upper(line2), Font({"Garamond", "Georgia", "Times New Roman"}), 24, "#f3e3c3", "center");
        s.Style({{"tracking", 500}});
        s.Shadow(2, 8, 70);
        break;
    }
    case 8: { // Lo-fi: a violet tint, a dark rounded box with a typewriter title
        s.Add("layer.newAdjustmentLayer.photoFilter", {{"filter", "violet"}, {"density", 40}});
        s.Add("layer.newAdjustmentLayer.hueSaturation", {{"saturation", -20}});
        s.Box("roundedRect", 130, 770, 820, 210, "#14102ad0", {{"width", s.P(3)}, {"color", "#ffffff"}}, 18);
        const std::string tl = Lines(Lower(title), 24, 2, &n);
        s.Text(540, 870 - (n - 1) * 32, tl, Font({"Consolas", "Lucida Console", "Courier New"}), 54, "#ffffff", "center", 760);
        s.Text(540, 940, "~ " + Lower(line2) + " ~", Font({"Consolas", "Lucida Console", "Courier New"}), 24, "#c9b8ff", "center");
        break;
    }
    default: { // Poster: dark on the left, the title stacked a word to a line
        s.Fade(0, 0, 780, 1080, json::array({json::array({0, "#000000e0"}), json::array({1, "#00000000"})}), 0);
        const std::string tl = Lines(Upper(title), 9, 5, &n);
        s.Text(60, 200, tl, Font({"Haettenschweiler", "Impact", "Arial Black"}), 150, "#ffffff", "left", 700);
        s.Text(64, 200 + n * 150 - 30, Upper(line2), Font({"Bahnschrift", "Segoe UI", "Arial"}), 28, "#ffcc00", "left");
        s.Style({{"tracking", 300}});
        break;
    }
    }
}

std::string Run(const std::vector<std::wstring>& head, const json& steps, const std::wstring& out, std::string& log) {
    std::wstring cmd = QuoteArg(Cli());
    for (const std::wstring& a : head) cmd += L" " + QuoteArg(a);
    for (const json& s : steps) {
        cmd += L" --cmd " + QuoteArg(Wide(s["cmd"].get<std::string>()));
        if (s.contains("params") && !s["params"].is_null()) cmd += L" --params " + QuoteArg(Wide(s["params"].dump(-1, ' ', false, json::error_handler_t::replace)));
    }
    cmd += L" --out " + QuoteArg(out);
    log.clear();
    RunCaptureHidden(cmd, log, true);
    return log;
}

} // namespace

int CoverTemplateCount() { return static_cast<int>(sizeof(kTemplates) / sizeof(kTemplates[0])); }
const char* CoverTemplateName(int index) { return index >= 0 && index < CoverTemplateCount() ? kTemplates[index].name : ""; }
bool CoverTemplatesAvailable() { return !Cli().empty(); }

std::string SquarePicture(const std::wstring& picture, const std::wstring& outPng, int size) {
    const std::wstring ff = FfmpegPath();
    if (ff.empty()) return "ffmpeg.exe was not found";
    std::error_code ec;
    fs::create_directories(fs::path(outPng).parent_path(), ec);
    fs::remove(outPng, ec);
    std::string log;
    // the square from the middle across, and a little above the middle down (where faces usually are)
    RunCaptureHidden(L"\"" + ff + L"\" -y -hide_banner -loglevel error -i \"" + picture +
                         L"\" -frames:v 1 -vf \"crop='min(iw,ih)':'min(iw,ih)':(iw-min(iw\\,ih))/2:(ih-min(iw\\,ih))/3,scale=" + std::to_wstring(size) + L":" +
                         std::to_wstring(size) + L":flags=lanczos\" \"" + outPng + L"\"",
                     log, true);
    if (!fs::exists(outPng, ec) || fs::file_size(outPng, ec) < 100) return "the picture could not be read" + (log.empty() ? std::string() : ": " + log.substr(0, 160));
    return {};
}

std::string RenderCoverTemplate(int index, const std::wstring& picture, const std::string& title, const std::string& line2, const std::wstring& outPng, int size) {
    if (Cli().empty()) return "PhotoCraft is not set up yet (Help > Set up Try2Create, Helpers)";
    index = std::clamp(index, 0, CoverTemplateCount() - 1);
    std::error_code ec;
    fs::create_directories(fs::path(outPng).parent_path(), ec);
    std::vector<std::wstring> head;
    Steps s;
    s.k = size / 1080.0;
    std::wstring base;
    if (!picture.empty()) {
        base = outPng + L".base.png";
        const std::string err = SquarePicture(picture, base, size);
        if (!err.empty()) return err;
        head = {L"run", base};
    } else { // no picture: the template's own colours
        head = {L"run", L"--new", Wide(json({{"width", size}, {"height", size}}).dump())};
        s.Add("layer.newFillLayer.gradient", {{"from", kTemplates[index].from}, {"to", kTemplates[index].to}, {"angle", -60}, {"style", "linear"}});
    }
    Build(index, s, title.empty() ? std::string("Untitled") : title, line2);
    std::string log;
    for (int pass = 0; pass < 3; ++pass) {
        fs::remove(outPng, ec);
        Run(head, s.list, outPng, log);
        if (!fs::exists(outPng, ec) || s.fit.empty() || pass == 2) break;
        // what PhotoCraft drew: one JSON line per command, in order
        std::vector<json> results;
        std::istringstream in(log);
        for (std::string line; std::getline(in, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] != '{') continue;
            json j = json::parse(line, nullptr, false);
            if (j.is_object() && j.contains("command")) results.push_back(std::move(j));
        }
        bool changed = false;
        for (const auto& [step, maxW] : s.fit) {
            if (step >= results.size()) continue;
            const json& r = results[step];
            json& p = s.list[step]["params"];
            const double fontSize = p["size"].get<double>();
            if (!r.contains("result") || !r["result"].contains("bounds") || !r["result"]["bounds"].is_array() || r["result"]["bounds"].size() < 4) continue;
            const json& b = r["result"]["bounds"];
            const double w = b[2].get<double>() - b[0].get<double>(), h = b[3].get<double>() - b[1].get<double>();
            if (w <= 0 && h <= 0) {
                p["size"] = std::floor(fontSize * 0.7);
                changed = true;
            } else if (w > maxW) {
                p["size"] = std::floor(fontSize * maxW / w * 0.97);
                changed = true;
            }
        }
        if (!changed) break;
    }
    if (!base.empty()) fs::remove(base, ec);
    if (!fs::exists(outPng, ec) || fs::file_size(outPng, ec) < 100) {
        std::string last;
        std::istringstream in(log);
        for (std::string line; std::getline(in, line);) {
            if (!line.empty() && line[0] != '{' && line.find("warning") == std::string::npos) last = line;
        }
        return "PhotoCraft could not make it" + (last.empty() ? std::string() : ": " + last.substr(0, 200));
    }
    return {};
}
