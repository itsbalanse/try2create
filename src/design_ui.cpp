// Design with PhotoCraft: text, graphics and scenes on a picture. The prompt writer (an LLM that can see the picture) turns what you ask for into PhotoCraft
// engine commands (type layers, layer styles, shapes, placed pictures, adjustment layers), and photocraft-cli runs them: a PNG to look at and a layered
// PSD to change in PhotoCraft. Pictures added to the scene are cut out by SAM 3 first (see PixelCutoutPicture).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <windows.h>
#include <shellapi.h>
#include "clef.h"
#include "image_texture.h"
#include "imgui.h"
#include "json.hpp"
#include "notify.h"
#include "pixel_art.h"
#include "ui_extra.h"
#include "ui_theme.h"
#include "vision_prompt.h"
#include "win_dialogs.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// ------------------------------------------------------------------- photocraft-cli
// It sits in tools\photocraft (any version folder) next to this program.
std::wstring PhotocraftExe(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    const fs::path root = fs::path(buf).parent_path() / L"tools" / L"photocraft";
    std::error_code ec;
    if (fs::exists(root / name, ec)) return (root / name).wstring();
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end && !ec; it.increment(ec)) {
        if (it->is_regular_file(ec) && _wcsicmp(it->path().filename().c_str(), name) == 0) return it->path().wstring();
    }
    return {};
}

// One argument quoted the way CommandLineToArgvW reads it back.
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
        if (a[i] == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out += L'"';
        } else {
            out.append(backslashes, L'\\');
            out += a[i];
        }
    }
    return out + L"\"";
}

// Runs a program without a window and returns what it printed (stdout and stderr together); -1 when it could not start or took too long.
int RunTool(const std::wstring& exe, const std::vector<std::wstring>& args, std::string& output, DWORD timeoutMs = 180000) {
    std::wstring cmd = QuoteArg(exe);
    for (const std::wstring& a : args) cmd += L" " + QuoteArg(a);
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 20)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(0);
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        return -1;
    }
    CloseHandle(wr);
    const ULONGLONG start = GetTickCount64();
    char chunk[4096];
    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            DWORD got = 0;
            if (ReadFile(rd, chunk, std::min<DWORD>(avail, sizeof(chunk)), &got, nullptr) && got > 0) output.append(chunk, got);
            continue;
        }
        if (WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0) {
            DWORD got = 0;
            while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0 && ReadFile(rd, chunk, std::min<DWORD>(avail, sizeof(chunk)), &got, nullptr) && got > 0)
                output.append(chunk, got);
            break;
        }
        if (GetTickCount64() - start > timeoutMs) {
            TerminateProcess(pi.hProcess, 1);
            output += "\n(it took too long and was stopped)";
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            CloseHandle(rd);
            return -1;
        }
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rd);
    return static_cast<int>(code);
}

// The commands the writer may use, with PhotoCraft's own parameter notes (read once from `photocraft-cli commands --json`), and the fonts installed.
struct Reference {
    bool ready = false;
    std::string commands, fonts, error;
};

Reference LoadReference(std::wstring cli) {
    Reference r;
    static const char* kAllowed[] = {
        "type.create", "type.setStyle", "shape.create", "file.placeEmbedded", "edit.transform", "edit.transform.warp", "edit.transform.flipHorizontal",
        "layer.createClippingMask", "layer.renameLayer", "layer.arrange.sendBackward", "layer.arrange.bringForward",
        "layer.layerStyle.dropShadow", "layer.layerStyle.innerShadow", "layer.layerStyle.outerGlow", "layer.layerStyle.innerGlow", "layer.layerStyle.stroke",
        "layer.layerStyle.colorOverlay", "layer.layerStyle.gradientOverlay", "layer.layerStyle.bevelEmboss", "layer.layerStyle.satin",
        "layer.newAdjustmentLayer.brightnessContrast", "layer.newAdjustmentLayer.exposure", "layer.newAdjustmentLayer.vibrance", "layer.newAdjustmentLayer.hueSaturation",
        "layer.newAdjustmentLayer.colorBalance", "layer.newAdjustmentLayer.blackWhite", "layer.newAdjustmentLayer.photoFilter", "layer.newAdjustmentLayer.colorLookup"};
    std::string out;
    if (RunTool(cli, {L"commands", L"--json"}, out, 60000) != 0) {
        r.error = "photocraft-cli did not list its commands";
        return r;
    }
    const size_t a = out.find('[');
    json j = json::parse(a == std::string::npos ? out : out.substr(a), nullptr, false);
    if (!j.is_array()) {
        r.error = "photocraft-cli's command list could not be read";
        return r;
    }
    const std::set<std::string> allowed(std::begin(kAllowed), std::end(kAllowed));
    for (const json& c : j) {
        const std::string id = c.value("id", std::string());
        if (!allowed.count(id)) continue;
        std::string p = c.contains("params") && c["params"].is_string() ? c["params"].get<std::string>() : std::string();
        // the mask/path tail every adjustment layer repeats says nothing useful here
        const size_t tail = p.find(" + {\"path\"");
        if (tail != std::string::npos) p = p.substr(0, tail);
        r.commands += id + ": " + p + "\n";
    }
    // fonts: the good-looking ones that are installed
    static const char* kNice[] = {"Impact", "Arial", "Arial Black", "Bahnschrift", "Segoe UI", "Segoe UI Black", "Segoe Script", "Segoe Print", "Georgia", "Times New Roman",
                                  "Garamond", "Palatino Linotype", "Trebuchet MS", "Verdana", "Century Gothic", "Franklin Gothic Heavy", "Gill Sans Ultra Bold",
                                  "Cooper Black", "Bauhaus 93", "Broadway", "Showcard Gothic", "Stencil", "Algerian", "Castellar", "Copperplate Gothic Bold",
                                  "Old English Text MT", "Chiller", "Jokerman", "Papyrus", "Brush Script MT", "Lucida Handwriting", "Ink Free", "Comic Sans MS",
                                  "Gabriola", "Edwardian Script ITC", "Magneto", "Harlow Solid Italic", "OCR A Extended", "Consolas", "Inter", "JetBrains Mono"};
    std::string fout;
    if (RunTool(cli, {L"run", L"--new", L"{\"width\":64,\"height\":64}", L"--cmd", L"type.fonts"}, fout, 60000) == 0) {
        const size_t b = fout.find('{');
        json fj = json::parse(b == std::string::npos ? fout : fout.substr(b, fout.find('\n', b) == std::string::npos ? std::string::npos : fout.find('\n', b) - b), nullptr, false);
        std::set<std::string> have;
        if (fj.is_object() && fj.contains("result") && fj["result"].contains("families"))
            for (const json& f : fj["result"]["families"]) have.insert(f.get<std::string>());
        for (const char* f : kNice) {
            if (have.count(f)) r.fonts += (r.fonts.empty() ? "" : ", ") + std::string(f);
        }
    }
    if (r.fonts.empty()) r.fonts = "Arial, Impact, Georgia, Segoe UI";
    r.ready = !r.commands.empty();
    if (!r.ready) r.error = "photocraft-cli has none of the commands this needs (a different version?)";
    return r;
}

// ------------------------------------------------------------------- the design
struct Part {
    std::wstring src;        // as added
    bool cut = true;         // cut out by SAM 3 first
    int job = 0;             // the cut-out being made
    std::wstring cutPng;     // the cut-out (or the picture itself)
    int w = 0, h = 0;        // the size placed
    std::string error;
};

struct PlanOut {
    std::string steps, error, reply; // reply: the writer's answer (kept when it could not be read)
};

// A small model's JSON, put right where it is easy: code fences, // comments, trailing commas, curly quotes, and sums written as numbers (384+20).
std::string RepairJson(std::string s) {
    for (const char* q : {"\xe2\x80\x9c", "\xe2\x80\x9d"}) {
        for (size_t at = s.find(q); at != std::string::npos; at = s.find(q, at)) s.replace(at, 3, "\"");
    }
    std::string out;
    bool inStr = false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (inStr) {
            out += c;
            if (c == '\\' && i + 1 < s.size()) out += s[++i];
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') {
            inStr = true;
            out += c;
            continue;
        }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') { // a comment to the end of the line
            while (i < s.size() && s[i] != '\n') ++i;
            continue;
        }
        if (c == ',') { // a trailing comma
            size_t k = i + 1;
            while (k < s.size() && std::isspace(static_cast<unsigned char>(s[k]))) ++k;
            if (k < s.size() && (s[k] == '}' || s[k] == ']')) continue;
        }
        if ((std::isdigit(static_cast<unsigned char>(c)) || (c == '-' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])))) &&
            (out.empty() || std::string(":[, \n\t").find(out.back()) != std::string::npos)) {
            // a number, maybe a sum: 384+20, 768/2, 0.55*92
            size_t k = i;
            while (k < s.size() && std::string("0123456789.+-*/ ").find(s[k]) != std::string::npos) ++k;
            std::string expr = s.substr(i, k - i);
            while (!expr.empty() && expr.back() == ' ') expr.pop_back();
            if (expr.find_first_of("+*/") != std::string::npos || expr.find('-', 1) != std::string::npos) {
                double acc = 0, term = 0;
                char addOp = '+', mulOp = 0;
                bool ok = true;
                size_t p = 0;
                auto readNum = [&](double& v) {
                    while (p < expr.size() && expr[p] == ' ') ++p;
                    size_t used = 0;
                    try {
                        v = std::stod(expr.substr(p), &used);
                    } catch (...) {
                        return false;
                    }
                    p += used;
                    return true;
                };
                double v = 0;
                if (!readNum(v)) ok = false;
                term = v;
                while (ok && p < expr.size()) {
                    while (p < expr.size() && expr[p] == ' ') ++p;
                    if (p >= expr.size()) break;
                    const char op = expr[p++];
                    if (!readNum(v)) {
                        ok = false;
                        break;
                    }
                    if (op == '*') term *= v;
                    else if (op == '/') term = v != 0 ? term / v : term;
                    else {
                        acc = addOp == '+' ? acc + term : acc - term;
                        addOp = op;
                        term = v;
                    }
                    (void)mulOp;
                }
                if (ok) {
                    acc = addOp == '+' ? acc + term : acc - term;
                    char buf[32];
                    snprintf(buf, sizeof(buf), "%g", std::round(acc * 100) / 100);
                    out += buf;
                    i = i + expr.size() - 1;
                    continue;
                }
            }
        }
        out += c;
    }
    return out;
}

json ParseSteps(const std::string& reply, std::string& why) {
    std::string r = reply;
    const size_t a = r.find('{'), b = r.rfind('}');
    if (a == std::string::npos || b == std::string::npos || b <= a) {
        why = "there was no JSON object in it";
        return json();
    }
    r = r.substr(a, b - a + 1);
    json j = json::parse(r, nullptr, false);
    if (j.is_discarded()) j = json::parse(RepairJson(r), nullptr, false);
    if (j.is_discarded()) {
        try {
            (void)json::parse(RepairJson(r));
        } catch (const std::exception& e) {
            why = e.what();
        }
        return json();
    }
    if (j.is_array()) j = json{{"steps", j}};
    if (!j.is_object() || !j.contains("steps") || !j["steps"].is_array() || j["steps"].empty()) {
        why = "it had no \"steps\" list";
        return json();
    }
    return j;
}

struct RunOut {
    bool ok = false;
    std::string log, steps; // steps: as run (text moved or made smaller to fit)
    std::wstring png, psd;
};

struct Designer {
    bool open = false, focus = false;
    std::wstring base;
    int baseW = 0, baseH = 0;
    std::vector<Part> parts;
    std::string request, steps, status, error, log, reply;
    std::future<Reference> refFut;
    Reference ref;
    std::future<PlanOut> planFut;
    std::future<RunOut> runFut;
    bool waitingCuts = false; // planning waits for the cut-outs
    bool change = false;      // the next plan changes the current steps
    int attempt = 0;          // plans made again after the steps failed
    int round = 0;
    std::wstring png, psd;
    LoadedImage tex;
    std::wstring texOf;
};
Designer g_d;

std::wstring OutDir(AppState& app) {
    std::wstring dir = ComfyOutputDir(app.comfy);
    if (!dir.empty()) dir += L"\\Design";
    else {
        wchar_t buf[MAX_PATH * 2] = {};
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH * 2);
        dir = std::wstring(buf, n) + L"\\Try2Create\\design";
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

// The picture with a grid on it (lines every 10%, labelled in the document's pixels, drawn by PhotoCraft), so the writer can tell where things are exactly.
std::wstring GridPicture(const std::wstring& cli, const std::wstring& base, int W, int H) {
    if (cli.empty() || W <= 0 || H <= 0) return {};
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring out = std::wstring(tmp) + L"mv_design_grid.png";
    const double lw = std::max(1.0, std::min(W, H) / 400.0), fs = std::max(11.0, std::min(W, H) / 45.0);
    std::vector<std::wstring> args = {L"run", base};
    auto add = [&](const char* cmd, const json& p) {
        args.push_back(L"--cmd");
        args.push_back(ToWide(cmd));
        args.push_back(L"--params");
        args.push_back(ToWide(p.dump()));
    };
    for (int k = 1; k < 10; ++k) {
        const int x = W * k / 10, y = H * k / 10;
        add("shape.create", {{"kind", "line"}, {"from", json::array({x, 0})}, {"to", json::array({x, H})}, {"weight", lw}, {"fill", "#ffff00b0"}});
        add("shape.create", {{"kind", "line"}, {"from", json::array({0, y})}, {"to", json::array({W, y})}, {"weight", lw}, {"fill", "#ffff00b0"}});
        add("type.create", {{"x", x + 3}, {"y", fs + 2}, {"text", "x" + std::to_string(x)}, {"font", "Arial"}, {"size", fs}, {"color", "#ffff00"}});
        add("layer.layerStyle.stroke", {{"size", 2}, {"color", "#000000"}, {"position", "outside"}});
        add("type.create", {{"x", 3}, {"y", y - 3}, {"text", "y" + std::to_string(y)}, {"font", "Arial"}, {"size", fs}, {"color", "#ffff00"}});
        add("layer.layerStyle.stroke", {{"size", 2}, {"color", "#000000"}, {"position", "outside"}});
    }
    args.push_back(L"--out");
    args.push_back(out);
    std::string log;
    std::error_code ec;
    if (RunTool(cli, args, log, 60000) != 0 || !fs::exists(out, ec)) return {};
    return out;
}

PlanOut PlanDesign(std::wstring cli, std::string request, std::wstring base, int bw, int bh, std::vector<std::pair<std::wstring, std::pair<int, int>>> parts, std::string previous,
                   std::string failure, Reference ref, std::string comfyUrl) {
    PlanOut out;
    const std::string system =
        "You design text, graphics and scenes on top of a picture with PhotoCraft, a Photoshop-like engine, by writing its commands. Reply with ONLY a JSON object, "
        "no code fence, no preface: {\"steps\":[{\"cmd\":\"<command id>\",\"params\":{...}}, ...]}.\n"
        "The document is the picture, " + std::to_string(bw) + " x " + std::to_string(bh) + " pixels, 72 ppi (so a text size in pt is in pixels). x grows to the right, "
        "y grows down, (0,0) is the top-left corner. The middle is x " + std::to_string(bw / 2) + ". The safe area (4% margin): x " + std::to_string(bw * 4 / 100) + " to " +
        std::to_string(bw - bw * 4 / 100) + ", y " + std::to_string(bh * 4 / 100) + " to " + std::to_string(bh - bh * 4 / 100) + ".\n"
        "Text sizes: a line in a heavy font (Impact, capitals) is about 0.55 x size x letters wide, so for a line of N letters across the safe width use size = " +
        std::to_string(bw * 92 / 100) + " / (0.55 x N) at most; longer captions: two or three lines (\\n in the text) or a box. A top caption's baseline is at "
        "y = " + std::to_string(bh * 4 / 100) + " + 0.8 x size (the first line); a bottom caption's last baseline at y = " + std::to_string(bh - bh * 5 / 100) + ".\n"
        "How it works:\n"
        "- Each command acts on the layer the command before it made: a layer style right after type.create styles that text; give \"add\":true to every layer "
        "style after the first on the same layer, or it replaces it.\n"
        "- Text: type.create. Point text: \"x\",\"y\" is the BASELINE of the first line (a 100 pt line rises about 72 px above y); with \"align\":\"center\" x is the "
        "middle. A paragraph that wraps: \"box\":[x,y,w,h] with the size small enough to fit. Colours as \"#rrggbb\". Fonts (use only these): " + ref.fonts + ".\n"
        "- Readable text: strong contrast with what is behind it, a stroke or a drop shadow on busy parts, at least 4% from the edges, never over a face.\n"
        "- Shapes (banners, bubbles, frames, bars): shape.create, drawn before the text that goes on them.\n"
        "- Pictures to place (people, objects, logos; already cut out): file.placeEmbedded with \"fit\":false, \"scale\" in % of the picture's own size and "
        "\"center\":[x,y] where its middle goes; put people standing on the ground and in proportion with the scene. Right after a placed person: a soft "
        "layer.layerStyle.dropShadow, then (to match the scene's light) an adjustment layer followed by layer.createClippingMask, which limits it to that picture.\n"
        "- Adjustment layers change everything below them: use them for a colour grade or a mood, gently (a grade should still look like the photo).\n"
        "- A semi-transparent fill is \"#rrggbbaa\" (\"#00000099\" is black at 60%).\n"
        "- Write the words yourself when the request does not give them: short, specific to what the picture shows, and funny when it is a meme or a funny edit.\n"
        "Recipes:\n"
        "- Meme: Impact, all capitals, white with a black stroke (size about 6% of the text size), centred; setup at the top, punchline at the bottom; nothing else.\n"
        "- Funny edit: a draw.bubble next to the speaker's head (not over the face), its tail pointing at the mouth, with a short silly line (Comic Sans MS); a "
        "sound effect word (BONK!, POW!, BRUH) in a heavy font, bright fill, a thick dark stroke and a drop shadow, in an empty spot; optional punchy caption.\n"
        "- Clickbait thumbnail: 2 to 4 HUGE words (about a third of the height) in a heavy font, yellow or white with a thick black stroke and a hard drop shadow, "
        "on the empty side; a draw.arrow (red, white outline) from near the words to the interesting thing, a draw.circle (red) around that thing; vibrance and "
        "contrast up a lot (adjustment layers first, so the graphics stay bright).\n"
        "- YouTube thumbnail: a short bold title (3 to 5 words) on the emptier side, a solid or semi-transparent colour band behind it, a small accent; colours a "
        "little more vivid. Clean, readable at a small size.\n"
        "- Movie poster: a big title in a cinematic font (Bahnschrift, Impact, Copperplate Gothic Bold, Castellar, Trebuchet MS bold) with wide tracking and a glow "
        "or gradient overlay, a tagline above or below it, a credits block (small, condensed, centred, grey) at the bottom, a release line; a dramatic grade "
        "(colour lookup tealOrange or a cool/warm photo filter, more contrast).\n"
        "- Magazine cover: the name huge across the top, cover lines on one side, small issue and price text.\n"
        "- Explainer scene (the collage look of YouTube explainer videos): the added pictures cut out and placed with draw.place at believable sizes, standing on "
        "the ground (\"bottom\" at their feet), a big object turned a little (rotate) and given a glow if it is the hero; a draw.banner with a date or place at the "
        "top-left; a picture on a draw.screen when someone is on a call or a video; a draw.paper for a script, a letter or a document. Funny, matter-of-fact.\n"
        "Building blocks (use these instead of drawing paths yourself):\n"
        "- draw.arrow {\"from\":[x,y],\"to\":[x,y] (the tip),\"color\":\"#ff0000\",\"width\":px (shaft thickness, 3-5% of the picture),\"outline\":\"#ffffff\"?}\n"
        "- draw.circle {\"center\":[x,y],\"radius\":px (or \"rx\",\"ry\"),\"color\":\"#ff0000\",\"width\":px (ring thickness)} - a ring, nothing inside\n"
        "- draw.place {\"picture\":N (the Nth picture to place),\"height\":px,\"bottom\":[x,y] (where its feet or base go) or \"center\":[x,y],\"flip\":bool?,"
        "\"rotate\":deg?,\"shadow\":bool=true,\"glow\":\"#rrggbb\"?} - a cut-out person or thing put into the scene\n"
        "- draw.screen {\"rect\":[x,y,w,h],\"picture\":N?,\"color\":\"#2fb4ff\",\"style\":\"hologram|tv\"} - a glowing see-through hologram screen (or a TV) showing a picture\n"
        "- draw.banner {\"text\":str,\"position\":\"top-left|top|bottom\",\"color\":\"#000000\",\"textColor\":\"#ffffff\"} - a date or title bar across the picture\n"
        "- draw.paper {\"rect\":[x,y,w,h],\"text\":str (lines with \\n),\"angle\":deg} - a sheet of typed paper (a script, a letter, a report), turned a little\n"
        "- draw.bubble {\"rect\":[x,y,w,h] (the bubble),\"tail\":[x,y] (where the tail points: the speaker's mouth),\"text\":str?,\"font\":str?,"
        "\"fill\":\"#ffffff\",\"outline\":\"#000000\",\"width\":px?,\"shape\":\"round|ellipse\"?} - a speech bubble, the text fitted inside\n"
        "- type.create and shape.create have NO stroke, shadow, glow or effect parameters: every effect is its own layer.layerStyle.<kind> step right after "
        "the layer it is for.\n"
        "Example, a meme on a 1024 x 768 picture:\n"
        "{\"steps\":[{\"cmd\":\"type.create\",\"params\":{\"x\":512,\"y\":105,\"text\":\"WHEN THE CODE COMPILES\",\"align\":\"center\",\"font\":\"Impact\","
        "\"size\":78,\"color\":\"#ffffff\"}},{\"cmd\":\"layer.layerStyle.stroke\",\"params\":{\"size\":5,\"position\":\"outside\",\"color\":\"#000000\"}},"
        "{\"cmd\":\"type.create\",\"params\":{\"x\":512,\"y\":730,\"text\":\"ON THE FIRST TRY\",\"align\":\"center\",\"font\":\"Impact\",\"size\":78,"
        "\"color\":\"#ffffff\"}},{\"cmd\":\"layer.layerStyle.stroke\",\"params\":{\"size\":5,\"position\":\"outside\",\"color\":\"#000000\"}}]}\n"
        "- Only the commands listed here, with only their parameters:\n" + ref.commands;
    std::string user = "What to do: " + request + "\n";
    for (size_t i = 0; i < parts.size(); ++i)
        user += "Picture to place " + std::to_string(i + 1) + ": path \"" + ToUtf8(parts[i].first) + "\", " + std::to_string(parts[i].second.first) + " x " +
                std::to_string(parts[i].second.second) + " pixels.\n";
    if (!previous.empty()) user += "The steps so far (change them as asked and keep the rest; reply with the whole new list):\n" + previous + "\n";
    if (!failure.empty()) user += "These steps failed in PhotoCraft with this message; fix them:\n" + failure + "\n";
    const std::wstring grid = GridPicture(cli, base, bw, bh);
    user += grid.empty() ? "The picture is attached."
                         : "The picture is attached with a yellow grid drawn on it (not part of the design): a line every 10%, labelled with its x or y in the "
                           "document's pixels. Find each thing you point at, circle or keep clear (faces, the subject, empty space) on that grid and use those "
                           "pixel numbers.";
    std::vector<std::string> images;
    const std::string url = ImageDataUrl(grid.empty() ? base : grid, 1024);
    if (!url.empty()) images.push_back(url);
    const ClefSession session = WriterAcquire(comfyUrl, [](const std::string&) {});
    if (!session.ok) {
        out.error = "The prompt writer could not be started (Clef mode tab > Prompt writer)";
        return out;
    }
    std::string reply, err;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!WriterChat(system, user, images, 4000, 0.4, reply, &err)) {
            out.error = "The prompt writer did not answer" + (err.empty() ? std::string() : ": " + err);
            break;
        }
        out.reply = reply;
        std::string why;
        const json j = ParseSteps(reply, why);
        if (!j.is_null()) {
            out.steps = j["steps"].dump(1);
            out.error.clear();
            break;
        }
        out.error = "The prompt writer's answer could not be read as steps (" + why + ")";
        // once more, told what was wrong
        user += "\n\nYour previous answer could not be read (" + why + "). Answer again with ONLY the JSON object {\"steps\":[...]}, every number written out "
                "(no sums), no comments.";
    }
    WriterRelease(session, false);
    return out;
}

// What a small model gets wrong in the shape of the steps, put right: "command" for "cmd", and effects written into type.create / shape.create (stroke,
// shadow, glow...) become the layer style steps PhotoCraft has, right after that layer.
using PartList = std::vector<std::pair<std::wstring, std::pair<int, int>>>; // the pictures to place: path, size

// A rectangle turned by `deg` around (cx, cy): the four corners clockwise from the top-left (edit.transform's quad).
json RotatedQuad(double x0, double y0, double x1, double y1, double cx, double cy, double deg) {
    const double a = deg * 3.14159265358979 / 180.0, c = std::cos(a), s = std::sin(a);
    auto r = [&](double x, double y) { return json::array({std::round((cx + (x - cx) * c - (y - cy) * s) * 10) / 10, std::round((cy + (x - cx) * s + (y - cy) * c) * 10) / 10}); };
    return json::array({r(x0, y0), r(x1, y0), r(x1, y1), r(x0, y1)});
}

double HueOf(const std::string& hex) {
    if (hex.size() < 7 || hex[0] != '#') return 210;
    const double r = std::stoi(hex.substr(1, 2), nullptr, 16) / 255.0, g = std::stoi(hex.substr(3, 2), nullptr, 16) / 255.0, b = std::stoi(hex.substr(5, 2), nullptr, 16) / 255.0;
    const double mx = std::max({r, g, b}), mn = std::min({r, g, b}), dd = mx - mn;
    if (dd <= 0) return 0;
    double h = mx == r ? std::fmod((g - b) / dd, 6.0) : mx == g ? (b - r) / dd + 2 : (r - g) / dd + 4;
    h *= 60;
    return h < 0 ? h + 360 : h;
}

json NormalizeSteps(const json& in, int W, int H, const PartList& parts) {
    json out = json::array();
    if (!in.is_array()) return out;
    auto colour = [](const json& v, const char* def) -> std::string {
        if (v.is_string()) return v.get<std::string>();
        return def;
    };
    auto num = [](const json& p, const char* k, double def) { return p.contains(k) && p[k].is_number() ? p[k].get<double>() : def; };
    auto pt = [](const json& p, const char* k, double& x, double& y) {
        if (!p.contains(k) || !p[k].is_array() || p[k].size() < 2 || !p[k][0].is_number() || !p[k][1].is_number()) return false;
        x = p[k][0].get<double>();
        y = p[k][1].get<double>();
        return true;
    };
    auto str = [](const json& p, const char* k, const char* def) { return p.contains(k) && p[k].is_string() ? p[k].get<std::string>() : std::string(def); };
    for (const json& s0 : in) {
        if (!s0.is_object()) continue;
        json s = s0;
        if (!s.contains("cmd") && s.contains("command")) s["cmd"] = s["command"];
        if (!s.contains("cmd") || !s["cmd"].is_string()) continue;
        if (!s.contains("params") || !s["params"].is_object()) s["params"] = json::object();
        if (s["cmd"] == "layer.layerStyle.fill") s["cmd"] = "layer.layerStyle.colorOverlay"; // a fill colour is a colour overlay
        if (s["params"].contains("layer") && !s["params"]["layer"].is_number()) s["params"].erase("layer"); // a layer named, not numbered: the layer before
        {
            // limits: adjustments that would wreck the picture are left out or toned down
            static const std::set<std::string> wreck = {"layer.newAdjustmentLayer.gradientMap", "layer.newAdjustmentLayer.levels", "layer.newAdjustmentLayer.curves",
                                                        "layer.newAdjustmentLayer.threshold", "layer.newAdjustmentLayer.posterize", "layer.newAdjustmentLayer.invert",
                                                        "image.adjustments.invert", "image.adjustments.threshold"};
            const std::string c = s["cmd"].get<std::string>();
            if (wreck.count(c)) continue;
            json& p = s["params"];
            auto clampKey = [&](const char* k, double lo, double hi) {
                if (p.contains(k) && p[k].is_number()) p[k] = std::clamp(p[k].get<double>(), lo, hi);
            };
            if (c == "layer.newAdjustmentLayer.exposure") {
                clampKey("exposure", -1.5, 1.5);
                clampKey("offset", -0.1, 0.1);
                clampKey("gamma", 0.7, 1.4);
            } else if (c == "layer.newAdjustmentLayer.brightnessContrast") {
                clampKey("brightness", -40, 40);
                clampKey("contrast", -30, 50);
            } else if (c == "layer.newAdjustmentLayer.vibrance") {
                clampKey("vibrance", -60, 70);
                clampKey("saturation", -50, 50);
            } else if (c == "layer.newAdjustmentLayer.hueSaturation" && !(p.contains("colorize") && p["colorize"].is_boolean() && p["colorize"].get<bool>())) {
                clampKey("hue", -30, 30);
                clampKey("saturation", -60, 60);
                clampKey("lightness", -30, 30);
            } else if (c == "layer.newAdjustmentLayer.photoFilter") {
                clampKey("density", 0, 60);
            } else if (c == "shape.create" && W > 0 && H > 0) {
                // a made-up "opacity": the fill's alpha
                if (p.contains("opacity") && p["opacity"].is_number() && p.contains("fill") && p["fill"].is_string() && p["fill"].get<std::string>().size() == 7) {
                    const int a = static_cast<int>(std::lround(std::clamp(p["opacity"].get<double>(), 0.0, 100.0) * 2.55));
                    char hex[4];
                    snprintf(hex, sizeof(hex), "%02x", a);
                    p["fill"] = p["fill"].get<std::string>() + hex;
                }
                p.erase("opacity");
                // inside the picture; a filled shape covering most of it is a band, not a curtain
                if (p.contains("rect") && p["rect"].is_array() && p["rect"].size() >= 4 && p["rect"][0].is_number()) {
                    double x = p["rect"][0].get<double>(), y = p["rect"][1].get<double>(), w = p["rect"][2].get<double>(), h = p["rect"][3].get<double>();
                    x = std::clamp(x, -0.02 * W, 1.0 * W);
                    y = std::clamp(y, -0.02 * H, 1.0 * H);
                    w = std::clamp(w, 1.0, W * 1.04 - x);
                    h = std::clamp(h, 1.0, H * 1.04 - y);
                    const bool filled = p.contains("fill") && !p["fill"].is_null();
                    if (filled && w * h > 0.6 * W * H) {
                        const double nh = std::min(h, 0.32 * H);
                        if (y + h / 2 > H / 2.0) y = y + h - nh; // keep its lower edge
                        h = nh;
                    }
                    if (filled && w * h > 0.25 * W * H && p["fill"].is_string() && p["fill"].get<std::string>().size() == 7) p["fill"] = p["fill"].get<std::string>() + "b3";
                    p["rect"] = json::array({std::round(x), std::round(y), std::round(w), std::round(h)});
                }
            }
        }
        if (s["cmd"] == "type.create" && s["params"].contains("box") && s["params"]["box"].is_array()) { // paragraph text: the point is not used
            s["params"].erase("x");
            s["params"].erase("y");
        }
        if (s["cmd"] == "draw.bubble" && W > 0 && H > 0 && s["params"].contains("rect") && s["params"]["rect"].is_array() && s["params"]["rect"].size() >= 4 &&
            s["params"]["rect"][0].is_number()) { // the whole bubble inside the picture
            json& r = s["params"]["rect"];
            double w = std::min(r[2].get<double>(), W * 0.9), h = std::min(r[3].get<double>(), H * 0.5);
            const double x = std::clamp(r[0].get<double>(), W * 0.03, W * 0.97 - w), y = std::clamp(r[1].get<double>(), H * 0.03, H * 0.97 - h);
            r = json::array({std::round(x), std::round(y), std::round(w), std::round(h)});
        }
        const std::string id = s["cmd"].get<std::string>();
        const json& bp = s["params"];
        // which picture to place: "picture": 1 (the first one) or its path
        auto partOf = [&](const json& p, std::wstring& path, int& pw, int& ph) {
            if (!p.contains("picture")) return false;
            int idx = -1;
            if (p["picture"].is_number()) idx = p["picture"].get<int>() - 1;
            else if (p["picture"].is_string()) {
                const std::wstring want = ToWide(p["picture"].get<std::string>());
                for (size_t i = 0; i < parts.size(); ++i) {
                    if (_wcsicmp(parts[i].first.c_str(), want.c_str()) == 0) idx = static_cast<int>(i);
                }
                if (idx < 0 && !p["picture"].get<std::string>().empty() && std::isdigit(static_cast<unsigned char>(p["picture"].get<std::string>()[0])))
                    idx = std::atoi(p["picture"].get<std::string>().c_str()) - 1;
            }
            if (idx < 0 || idx >= static_cast<int>(parts.size()) || parts[idx].second.first <= 0) return false;
            path = parts[idx].first;
            pw = parts[idx].second.first;
            ph = parts[idx].second.second;
            return true;
        };
        if (id == "draw.place") { // a cut-out person or thing put into the scene: its height, where its middle (or its feet) go, flipped, turned, shadow, glow
            std::wstring path;
            int pw = 0, ph = 0;
            if (!partOf(bp, path, pw, ph)) continue;
            double h = std::min(num(bp, "height", H * 0.6), H * 0.98);
            if (bp.contains("width") && bp["width"].is_number() && !bp.contains("height")) h = bp["width"].get<double>() * ph / pw;
            const double k = h / ph, w = pw * k;
            double cx = W / 2.0, cy = H / 2.0, bx, by;
            if (pt(bp, "bottom", bx, by)) {
                cx = bx;
                cy = by - h / 2;
            } else {
                pt(bp, "center", cx, cy);
            }
            if (w < W) cx = std::clamp(cx, w / 2 + W * 0.01, W * 0.99 - w / 2); // the whole of it in the picture (its feet may stand at the bottom edge)
            cy = std::min(cy, H - h / 2 + h * 0.02);
            out.push_back({{"cmd", "file.placeEmbedded"}, {"params", {{"path", ToUtf8(path)}, {"fit", false}, {"scale", std::round(k * 1000) / 10}, {"center", json::array({cx, cy})}}}});
            if (bp.contains("flip") && bp["flip"].is_boolean() && bp["flip"].get<bool>()) out.push_back({{"cmd", "edit.transform.flipHorizontal"}, {"params", json::object()}});
            const double deg = num(bp, "rotate", 0);
            if (std::abs(deg) > 0.5)
                out.push_back({{"cmd", "edit.transform"}, {"params", {{"rect", json::array({cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2})}, {"quad", RotatedQuad(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2, cx, cy, deg)}}}});
            bool first = true;
            if (!(bp.contains("shadow") && bp["shadow"].is_boolean() && !bp["shadow"].get<bool>())) {
                out.push_back({{"cmd", "layer.layerStyle.dropShadow"}, {"params", {{"color", "#000000"}, {"opacity", 45}, {"angle", 120}, {"distance", std::max(3.0, h * 0.012)}, {"size", std::max(6.0, h * 0.03)}}}});
                first = false;
            }
            if (bp.contains("glow") && bp["glow"].is_string()) {
                json gp = {{"color", bp["glow"]}, {"opacity", 80}, {"size", std::max(10.0, h * 0.05)}, {"spread", 8}};
                if (!first) gp["add"] = true;
                out.push_back({{"cmd", "layer.layerStyle.outerGlow"}, {"params", gp}});
            }
            continue;
        }
        if (id == "draw.screen") { // a hologram (or TV) screen showing a picture
            if (!bp.contains("rect") || !bp["rect"].is_array() || bp["rect"].size() < 4) continue;
            const double x = bp["rect"][0].get<double>(), y = bp["rect"][1].get<double>(), w = bp["rect"][2].get<double>(), h = bp["rect"][3].get<double>();
            const std::string colr = str(bp, "color", "#2fb4ff"), style = str(bp, "style", "hologram");
            const bool holo = style != "tv";
            const double lw = std::max(2.0, std::min(w, h) * 0.012);
            out.push_back({{"cmd", "shape.create"},
                           {"params", {{"kind", "rect"}, {"name", "Screen"}, {"rect", json::array({x, y, w, h})}, {"fill", holo ? colr : std::string("#05070a")},
                                       {"stroke", {{"width", holo ? lw : lw * 3}, {"color", holo ? colr : std::string("#1b1e24")}, {"align", "inside"}}}}}});
            if (holo) { // glass: the colour at a quarter (the layer's fill, so the glow stays strong)
                out.push_back({{"cmd", "layer.setProps"}, {"params", {{"fill", 0.25}}}});
                out.push_back({{"cmd", "layer.layerStyle.outerGlow"}, {"params", {{"color", colr}, {"opacity", 85}, {"size", std::max(10.0, std::min(w, h) * 0.05)}, {"spread", 6}}}});
            }
            std::wstring path;
            int pw = 0, ph = 0;
            if (partOf(bp, path, pw, ph)) {
                const double k = std::min((w * 0.94) / pw, (h * 0.9) / ph);
                out.push_back({{"cmd", "file.placeEmbedded"},
                               {"params", {{"path", ToUtf8(path)}, {"fit", false}, {"scale", std::round(k * 1000) / 10}, {"center", json::array({x + w / 2, y + h / 2})}}}});
                if (holo) {
                    out.push_back({{"cmd", "layer.setProps"}, {"params", {{"opacity", 0.8}}}});
                    out.push_back({{"cmd", "layer.newAdjustmentLayer.hueSaturation"}, {"params", {{"colorize", true}, {"hue", std::round(HueOf(colr))}, {"saturation", 55}, {"lightness", 5}}}});
                    out.push_back({{"cmd", "layer.createClippingMask"}, {"params", json::object()}});
                }
            }
            if (holo) { // the HUD details: slanted stripes in a corner and a bar along the top
                for (int k = 0; k < 3; ++k) {
                    const double sx = x + w * 0.03 + k * w * 0.025, sy = y + h * 0.04, sw = w * 0.012, sh = h * 0.07;
                    out.push_back({{"cmd", "shape.create"},
                                   {"params", {{"kind", "path"}, {"name", "HUD"}, {"fill", colr}, {"path", {{"subpaths", json::array({{{"closed", true},
                                       {"knots", json::array({json::array({sx + sh * 0.5, sy}), json::array({sx + sh * 0.5 + sw, sy}), json::array({sx + sw, sy + sh}), json::array({sx, sy + sh})})}}})}}}}}});
                }
                out.push_back({{"cmd", "shape.create"}, {"params", {{"kind", "rect"}, {"name", "HUD bar"}, {"rect", json::array({x + w * 0.16, y + h * 0.02, w * 0.5, std::max(3.0, h * 0.012)})}, {"fill", colr}}}});
            }
            continue;
        }
        if (id == "draw.banner") { // a date or title bar across the top (or the bottom)
            const std::string text = str(bp, "text", "");
            if (text.empty()) continue;
            const std::string pos = str(bp, "position", "top-left");
            const double bh = num(bp, "height", H * 0.09), by = pos == "bottom" ? H - bh : 0;
            out.push_back({{"cmd", "shape.create"}, {"params", {{"kind", "rect"}, {"name", "Banner"}, {"rect", json::array({0, by, W, bh})}, {"fill", str(bp, "color", "#000000")}}}});
            json tp = {{"text", text}, {"font", str(bp, "font", "Arial")}, {"fauxBold", true}, {"size", std::floor(bh * 0.58)}, {"color", str(bp, "textColor", "#ffffff")},
                       {"y", std::round(by + bh * 0.76)}};
            if (pos == "top" || pos == "bottom") {
                tp["align"] = "center";
                tp["x"] = W / 2;
            } else {
                tp["align"] = "left";
                tp["x"] = std::round(W * 0.015);
            }
            out.push_back({{"cmd", "type.create"}, {"params", tp}});
            continue;
        }
        if (id == "draw.paper") { // a sheet of typed paper (a script, a letter, a report), turned a little
            if (!bp.contains("rect") || !bp["rect"].is_array() || bp["rect"].size() < 4) continue;
            const double x = bp["rect"][0].get<double>(), y = bp["rect"][1].get<double>(), w = bp["rect"][2].get<double>(), h = bp["rect"][3].get<double>();
            const double cx = x + w / 2, cy = y + h / 2, deg = num(bp, "angle", -3);
            out.push_back({{"cmd", "shape.create"}, {"params", {{"kind", "rect"}, {"name", "Paper"}, {"rect", json::array({x, y, w, h})}, {"fill", str(bp, "color", "#f3f3ee")}}}});
            if (std::abs(deg) > 0.5) out.push_back({{"cmd", "edit.transform"}, {"params", {{"rect", json::array({x, y, x + w, y + h})}, {"quad", RotatedQuad(x, y, x + w, y + h, cx, cy, deg)}}}});
            out.push_back({{"cmd", "layer.layerStyle.dropShadow"}, {"params", {{"color", "#000000"}, {"opacity", 50}, {"angle", 120}, {"distance", std::max(4.0, w * 0.015)}, {"size", std::max(8.0, w * 0.04)}}}});
            const std::string text = str(bp, "text", "");
            if (!text.empty()) {
                const double bx = x + w * 0.1, byy = y + h * 0.07, bw = w * 0.8, bhh = h * 0.86;
                out.push_back({{"cmd", "type.create"},
                               {"params", {{"box", json::array({bx, byy, bw, bhh})}, {"text", text}, {"font", str(bp, "font", "Courier New")}, {"size", std::floor(std::max(8.0, w / 26))},
                                           {"color", "#1d1d1d"}, {"align", "left"}}}});
                if (std::abs(deg) > 0.5) out.push_back({{"cmd", "edit.transform"}, {"params", {{"rect", json::array({bx, byy, bx + bw, byy + bhh})}, {"quad", RotatedQuad(bx, byy, bx + bw, byy + bhh, cx, cy, deg)}}}});
            }
            continue;
        }
        if (id == "draw.arrow") { // a filled arrow polygon: shaft and head
            double fx, fy, tx, ty;
            if (!pt(bp, "from", fx, fy) || !pt(bp, "to", tx, ty)) continue;
            const double L = std::hypot(tx - fx, ty - fy);
            if (L < 4) continue;
            const double ux = (tx - fx) / L, uy = (ty - fy) / L, px = -uy, py = ux;
            const double w = std::max(4.0, num(bp, "width", L * 0.12)), head = std::min(L * 0.45, w * 3.0), hw = w * 1.4;
            const double bx = tx - ux * head, by = ty - uy * head;
            json knots = json::array({json::array({fx + px * w / 2, fy + py * w / 2}), json::array({bx + px * w / 2, by + py * w / 2}), json::array({bx + px * hw, by + py * hw}),
                                      json::array({tx, ty}), json::array({bx - px * hw, by - py * hw}), json::array({bx - px * w / 2, by - py * w / 2}),
                                      json::array({fx - px * w / 2, fy - py * w / 2})});
            json sp = {{"kind", "path"}, {"name", "Arrow"}, {"fill", str(bp, "color", "#ff0000")}, {"path", {{"subpaths", json::array({{{"closed", true}, {"knots", knots}}})}}}};
            if (bp.contains("outline") && bp["outline"].is_string()) sp["stroke"] = {{"width", std::max(2.0, w / 6)}, {"color", bp["outline"]}, {"align", "outside"}, {"join", "round"}};
            out.push_back({{"cmd", "shape.create"}, {"params", sp}});
            out.push_back({{"cmd", "layer.layerStyle.dropShadow"}, {"params", {{"color", "#000000"}, {"opacity", 55}, {"distance", 4}, {"size", 6}}}});
            continue;
        }
        if (id == "draw.circle") { // a ring
            double cx, cy;
            if (!pt(bp, "center", cx, cy)) continue;
            const double r = num(bp, "radius", 60), rx = num(bp, "rx", r), ry = num(bp, "ry", r);
            out.push_back({{"cmd", "shape.create"},
                           {"params", {{"kind", "ellipse"}, {"name", "Circle"}, {"rect", json::array({cx - rx, cy - ry, rx * 2, ry * 2})}, {"fill", nullptr},
                                       {"stroke", {{"width", std::max(2.0, num(bp, "width", 10))}, {"color", str(bp, "color", "#ff0000")}, {"align", "center"}}}}}});
            continue;
        }
        if (id == "draw.bubble") { // tail (filled, outlined), the bubble over it, the tail again without outline to hide the seam; the text inside
            if (!bp.contains("rect") || !bp["rect"].is_array() || bp["rect"].size() < 4) continue;
            const double x = bp["rect"][0].get<double>(), y = bp["rect"][1].get<double>(), w = bp["rect"][2].get<double>(), h = bp["rect"][3].get<double>();
            const double cx = x + w / 2, cy = y + h / 2;
            double tx = cx, ty = y + h + h * 0.6;
            pt(bp, "tail", tx, ty);
            const double L = std::max(1.0, std::hypot(tx - cx, ty - cy)), ux = (tx - cx) / L, uy = (ty - cy) / L, k = std::min(w, h) * 0.14;
            // the base of the tail: inside the bubble, part of the way toward the tip
            const double reach = std::min(w, h) * 0.3, bx = cx + ux * reach, by = cy + uy * reach;
            json tri = json::array({json::array({bx - uy * k, by + ux * k}), json::array({tx, ty}), json::array({bx + uy * k, by - ux * k})});
            const std::string fill = str(bp, "fill", "#ffffff"), line = str(bp, "outline", "#000000");
            const double lw = std::max(2.0, num(bp, "width", std::min(w, h) * 0.035));
            const json stroke = {{"width", lw}, {"color", line}, {"align", "outside"}, {"join", "round"}};
            const bool ellipse = str(bp, "shape", "round") == "ellipse";
            out.push_back({{"cmd", "shape.create"}, {"params", {{"kind", "path"}, {"name", "Bubble tail"}, {"fill", fill}, {"stroke", stroke}, {"path", {{"subpaths", json::array({{{"closed", true}, {"knots", tri}}})}}}}}});
            json body = {{"kind", ellipse ? "ellipse" : "roundedRect"}, {"name", "Bubble"}, {"rect", json::array({x, y, w, h})}, {"fill", fill}, {"stroke", stroke}};
            if (!ellipse) body["radii"] = std::min(w, h) * 0.35;
            out.push_back({{"cmd", "shape.create"}, {"params", body}});
            out.push_back({{"cmd", "shape.create"}, {"params", {{"kind", "path"}, {"name", "Bubble tail cover"}, {"fill", fill}, {"stroke", nullptr}, {"path", {{"subpaths", json::array({{{"closed", true}, {"knots", tri}}})}}}}}});
            if (bp.contains("text") && bp["text"].is_string() && !bp["text"].get<std::string>().empty()) {
                const double ix = ellipse ? w * 0.17 : w * 0.09, iy = ellipse ? h * 0.2 : h * 0.12;
                const std::string t = bp["text"].get<std::string>();
                const double size = std::max(10.0, std::min(h * 0.32, (w - 2 * ix) / (0.5 * std::max<size_t>(4, std::min<size_t>(t.size(), 22)))));
                out.push_back({{"cmd", "type.create"},
                               {"params", {{"box", json::array({x + ix, y + iy, w - 2 * ix, h - 2 * iy})}, {"text", t}, {"align", "center"}, {"font", str(bp, "font", "Comic Sans MS")},
                                           {"size", std::floor(size)}, {"color", str(bp, "textColor", "#000000")}}}});
            }
            continue;
        }
        json extra = json::array();
        if (id == "type.create" || id == "shape.create") {
            json& p = s["params"];
            auto styleStep = [&](const std::string& kind, const json& params) {
                json e = {{"cmd", "layer.layerStyle." + kind}, {"params", params}};
                if (!extra.empty()) e["params"]["add"] = true;
                extra.push_back(e);
            };
            // a text stroke ("stroke": {...} / "strokeColor" / "outline")
            if (id == "type.create" && (p.contains("stroke") || p.contains("strokeColor") || p.contains("strokeWidth") || p.contains("outline"))) {
                const json st = p.contains("stroke") ? p["stroke"] : p.contains("outline") ? p["outline"] : json::object();
                json sp = {{"position", "outside"}, {"color", "#000000"}, {"size", 4}};
                if (st.is_object()) {
                    sp["color"] = colour(st.contains("color") ? st["color"] : json(), "#000000");
                    for (const char* k : {"size", "width", "px"})
                        if (st.contains(k) && st[k].is_number()) sp["size"] = st[k];
                } else if (st.is_number()) {
                    sp["size"] = st;
                } else if (st.is_string()) {
                    sp["color"] = st;
                }
                if (p.contains("strokeColor")) sp["color"] = colour(p["strokeColor"], "#000000");
                if (p.contains("strokeWidth") && p["strokeWidth"].is_number()) sp["size"] = p["strokeWidth"];
                styleStep("stroke", sp);
                p.erase("stroke");
                p.erase("strokeColor");
                p.erase("strokeWidth");
                p.erase("outline");
            }
            for (const char* k : {"shadow", "dropShadow"}) {
                if (!p.contains(k)) continue;
                json sp = {{"color", "#000000"}, {"opacity", 70}, {"distance", 6}, {"size", 8}};
                if (p[k].is_object())
                    for (auto& [key, v] : p[k].items()) sp[key] = v;
                if (!(p[k].is_boolean() && !p[k].get<bool>())) styleStep("dropShadow", sp);
                p.erase(k);
            }
            for (const char* k : {"glow", "outerGlow"}) {
                if (!p.contains(k)) continue;
                json sp = {{"color", "#ffffff"}, {"opacity", 75}, {"size", 16}};
                if (p[k].is_object())
                    for (auto& [key, v] : p[k].items()) sp[key] = v;
                else if (p[k].is_string()) sp["color"] = p[k];
                if (!(p[k].is_boolean() && !p[k].get<bool>())) styleStep("outerGlow", sp);
                p.erase(k);
            }
            for (const char* k : {"layerStyle", "effects", "effect", "style"}) { // "layerStyle":"dropShadow" or a list of them
                if (!p.contains(k) || !(p[k].is_string() || p[k].is_array())) continue;
                const json list = p[k].is_array() ? p[k] : json::array({p[k]});
                for (const json& e : list) {
                    const std::string kind = e.is_string() ? e.get<std::string>() : e.is_object() ? e.value("kind", std::string()) : std::string();
                    static const std::set<std::string> kinds = {"dropShadow", "innerShadow", "outerGlow", "innerGlow", "stroke", "colorOverlay", "gradientOverlay", "bevelEmboss", "satin"};
                    if (kinds.count(kind)) styleStep(kind, e.is_object() && e.contains("params") ? e["params"] : json::object());
                }
                if (!(k == std::string("style") && p[k].is_string() && id == "type.create")) p.erase(k);
            }
        }
        out.push_back(s);
        for (const json& e : extra) out.push_back(e);
    }
    return out;
}

// Text that does not fit: PhotoCraft says where each type layer landed; a line wider than the safe area gets smaller, one outside it moves in. True when a
// step was changed (run again).
bool FitText(json& steps, const std::vector<json>& results, int W, int H) {
    const double mx = W * 0.03, my = H * 0.03;
    bool changed = false;
    size_t k = 0; // the result of each step, in order
    for (json& s : steps) {
        if (!s.is_object() || s.value("cmd", s.value("command", std::string())).empty()) continue;
        if (k >= results.size()) break;
        const json& res = results[k++];
        if (s.value("cmd", s.value("command", std::string())) != "type.create" || !s.contains("params") || !s["params"].is_object()) continue;
        if (!res.contains("result") || !res["result"].is_object() || !res["result"].contains("bounds") || !res["result"]["bounds"].is_array() || res["result"]["bounds"].size() < 4)
            continue;
        json& p = s["params"];
        const json& b = res["result"]["bounds"];
        const double x0 = b[0].get<double>(), y0 = b[1].get<double>(), x1 = b[2].get<double>(), y1 = b[3].get<double>();
        const double tw = x1 - x0, th = y1 - y0;
        double size = p.contains("size") && p["size"].is_number() ? p["size"].get<double>() : 12.0;
        if (tw <= 0 && th <= 0) { // nothing was drawn: the text does not fit its box
            if (size > 8) {
                p["size"] = std::max(8.0, std::floor(size * 0.6));
                changed = true;
            }
            continue;
        }
        if (tw > W - 2 * mx || th > H - 2 * my) { // too big: smaller (the next run says where it lands)
            const double k2 = std::min((W - 2 * mx) / std::max(1.0, tw), (H - 2 * my) / std::max(1.0, th)) * 0.97;
            p["size"] = std::max(6.0, std::floor(size * k2));
            changed = true;
            continue;
        }
        double dx = 0, dy = 0;
        if (x0 < mx) dx = mx - x0;
        else if (x1 > W - mx) dx = (W - mx) - x1;
        if (y0 < my) dy = my - y0;
        else if (y1 > H - my) dy = (H - my) - y1;
        if (std::abs(dx) < 1 && std::abs(dy) < 1) continue;
        if (p.contains("box") && p["box"].is_array() && p["box"].size() >= 2) {
            p["box"][0] = std::round(p["box"][0].get<double>() + dx);
            p["box"][1] = std::round(p["box"][1].get<double>() + dy);
        } else {
            p["x"] = std::round((p.contains("x") && p["x"].is_number() ? p["x"].get<double>() : 0.0) + dx);
            p["y"] = std::round((p.contains("y") && p["y"].is_number() ? p["y"].get<double>() : 0.0) + dy);
        }
        changed = true;
    }
    return changed;
}

// The steps on a copy of the picture: a layered PSD (text fitted into the picture, up to two corrections), then a PNG of it. The steps and PhotoCraft's log
// are kept beside it.
RunOut RunDesign(std::wstring cli, std::wstring base, std::string steps, std::wstring png, std::wstring psd, int W, int H, PartList parts) {
    RunOut r;
    json j = json::parse(steps, nullptr, false);
    if (!j.is_array()) {
        r.log = "The steps are not a JSON list";
        return r;
    }
    j = NormalizeSteps(j, W, H, parts);
    int code = -1;
    std::string out;
    for (int pass = 0; pass < 3; ++pass) {
        std::vector<std::wstring> args = {L"run", base};
        for (const json& s : j) {
            const std::string id = s.is_object() ? s.value("cmd", s.value("command", std::string())) : std::string();
            if (id.empty()) continue;
            args.push_back(L"--cmd");
            args.push_back(ToWide(id));
            if (s.contains("params") && s["params"].is_object()) {
                args.push_back(L"--params");
                args.push_back(ToWide(s["params"].dump()));
            }
        }
        args.push_back(L"--out");
        args.push_back(psd);
        out.clear();
        {
            std::ofstream so((fs::path(psd).replace_extension().wstring()) + L".steps.json", std::ios::binary | std::ios::trunc);
            if (so) so << j.dump(1);
        }
        code = RunTool(cli, args, out, 60000);
        if (code != 0 || W <= 0 || H <= 0) break;
        std::vector<json> results;
        size_t at = 0;
        while (at < out.size()) {
            const size_t nl = out.find('\n', at);
            const std::string line = Trim(out.substr(at, nl == std::string::npos ? std::string::npos : nl - at));
            at = nl == std::string::npos ? out.size() : nl + 1;
            if (line.empty() || line[0] != '{') continue;
            json lj = json::parse(line, nullptr, false);
            if (lj.is_object() && lj.contains("command")) results.push_back(std::move(lj));
        }
        if (pass == 2 || !FitText(j, results, W, H)) break;
    }
    r.log = out;
    r.steps = j.dump(1);
    {
        const fs::path stem = fs::path(psd).replace_extension();
        std::ofstream so(stem.wstring() + L".steps.json", std::ios::binary | std::ios::trunc);
        if (so) so << r.steps;
        std::ofstream lo(stem.wstring() + L".log.txt", std::ios::binary | std::ios::trunc);
        if (lo) lo << out;
    }
    std::error_code ec;
    if (code != 0 || !fs::exists(psd, ec)) return r;
    std::string out2;
    if (RunTool(cli, {L"convert", psd, png}, out2) != 0 || !fs::exists(png, ec)) {
        r.log += "\n" + out2;
        return r;
    }
    r.ok = true;
    r.png = png;
    r.psd = psd;
    return r;
}

void ReleaseTex() {
    if (g_d.tex.srv) ReleaseImage(g_d.tex);
    g_d.tex = LoadedImage{};
    g_d.texOf.clear();
}

void StartPlan(AppState& app, const std::string& failure) {
    std::vector<std::pair<std::wstring, std::pair<int, int>>> parts;
    for (const Part& p : g_d.parts) {
        if (!p.cutPng.empty()) parts.push_back({p.cutPng, {p.w, p.h}});
    }
    g_d.status = failure.empty() ? "The prompt writer is planning the design..." : "The steps failed: the prompt writer is fixing them...";
    g_d.error.clear();
    g_d.planFut = std::async(std::launch::async, PlanDesign, PhotocraftExe(L"photocraft-cli.exe"), Trim(g_d.request), g_d.base, g_d.baseW, g_d.baseH, parts, g_d.change ? g_d.steps : std::string(), failure,
                             g_d.ref, app.comfy.url);
}

void StartRun(AppState& app) {
    const std::wstring cli = PhotocraftExe(L"photocraft-cli.exe");
    if (cli.empty()) {
        g_d.error = "photocraft-cli.exe was not found in the tools\\photocraft folder next to this program";
        return;
    }
    ++g_d.round;
    const std::wstring stem = OutDir(app) + L"\\" + fs::path(g_d.base).stem().wstring() + L"_design" + std::to_wstring(static_cast<long long>(std::time(nullptr)) % 1000000);
    g_d.status = "PhotoCraft is making it...";
    PartList parts;
    for (const Part& p : g_d.parts) {
        if (!p.cutPng.empty()) parts.push_back({p.cutPng, {p.w, p.h}});
    }
    g_d.runFut = std::async(std::launch::async, RunDesign, cli, g_d.base, g_d.steps, stem + L".png", stem + L".psd", g_d.baseW, g_d.baseH, parts);
}

// the pictures to place: cut out (SAM 3), or as they are
bool PartsReady(AppState& app) {
    bool ready = true;
    for (Part& p : g_d.parts) {
        if (!p.cutPng.empty() || !p.error.empty()) continue;
        if (!p.cut) {
            p.cutPng = p.src;
        } else if (p.job == 0) {
            p.job = PixelCutoutPicture(app, p.src);
            if (p.job <= 0) {
                p.error = "the cut-out could not be sent; placed as it is";
                p.cutPng = p.src;
            }
        } else {
            std::wstring cut;
            const int st = PixelCutoutResult(app, p.job, cut);
            if (st > 0) p.cutPng = cut;
            else if (st < 0) {
                p.error = "SAM 3 found nothing to cut out; placed as it is";
                p.cutPng = p.src;
            }
        }
        if (!p.cutPng.empty() && p.w == 0) {
            std::vector<uint8_t> rgba;
            PixelLoadRGBA(p.cutPng, rgba, p.w, p.h);
        }
        if (p.cutPng.empty()) ready = false;
    }
    return ready;
}

void Pump(AppState& app) {
    if (g_d.refFut.valid() && g_d.refFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        g_d.ref = g_d.refFut.get();
        if (!g_d.ref.ready) g_d.error = g_d.ref.error;
    }
    if (g_d.waitingCuts && g_d.ref.ready && PartsReady(app)) {
        g_d.waitingCuts = false;
        StartPlan(app, std::string());
    }
    if (g_d.planFut.valid() && g_d.planFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        PlanOut p = g_d.planFut.get();
        if (!p.error.empty()) {
            g_d.error = p.error;
            g_d.status.clear();
            g_d.reply = p.reply;
            if (!p.reply.empty()) { // kept beside the results, to see what it wrote
                std::ofstream f(fs::path(OutDir(app)) / L"last_unreadable_plan.txt", std::ios::binary | std::ios::trunc);
                if (f) f << p.reply;
            }
        } else {
            g_d.steps = p.steps;
            StartRun(app);
        }
    }
    if (g_d.runFut.valid() && g_d.runFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        RunOut r = g_d.runFut.get();
        g_d.log = r.log;
        if (!r.steps.empty()) g_d.steps = r.steps;
        if (r.ok) {
            g_d.png = r.png;
            g_d.psd = r.psd;
            g_d.status = "Done";
            g_d.change = false;
            app.SetStatus("Design saved: " + ToUtf8(fs::path(r.png).filename().wstring()) + " (and a layered .psd beside it)");
            Toast(ToastKind::Success, "Design ready", ToUtf8(fs::path(r.png).filename().wstring()) + " (and a layered .psd)", r.png,
                  [](AppState& a) { SetEditsMode(2), a.requestTab = 6; }, "design");
        } else if (g_d.attempt < 1 && !Trim(g_d.request).empty()) { // once: the writer fixes its steps from PhotoCraft's message
            ++g_d.attempt;
            g_d.change = true;
            StartPlan(app, r.log.size() > 1500 ? r.log.substr(r.log.size() - 1500) : r.log);
        } else {
            g_d.error = "PhotoCraft could not make it (see the steps and the log below)";
            g_d.status.clear();
            Toast(ToastKind::Error, "The design failed", "PhotoCraft could not make it: see the steps and the log", {}, [](AppState& a) { SetEditsMode(2), a.requestTab = 6; }, "design");
        }
    }
}

bool Busy() { return g_d.waitingCuts || g_d.planFut.valid() || g_d.runFut.valid(); }

void AddPart(const std::wstring& p) {
    if (p.empty() || _wcsicmp(p.c_str(), g_d.base.c_str()) == 0) return;
    for (const Part& x : g_d.parts) {
        if (_wcsicmp(x.src.c_str(), p.c_str()) == 0) return;
    }
    Part part;
    part.src = p;
    g_d.parts.push_back(part);
}

void Thumb(AppState& app, const std::wstring& path, float size) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const AppState::LibThumb t = app.LibraryThumb(path);
    dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), ui::ThumbPlaceholder(), ui::Dp(5.0f));
    if (t.srv && t.w > 0 && t.h > 0) {
        const float s = std::min(size / t.w, size / t.h);
        const ImVec2 a(p.x + (size - t.w * s) * 0.5f, p.y + (size - t.h * s) * 0.5f);
        dl->AddImage((ImTextureID)(intptr_t)t.srv, a, ImVec2(a.x + t.w * s, a.y + t.h * s));
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------
std::wstring DesignGridPicture(const std::wstring& picture, int w, int h) { return GridPicture(PhotocraftExe(L"photocraft-cli.exe"), picture, w, h); }

void OpenDesigner(AppState& app, const std::vector<std::wstring>& paths) {
    if (paths.empty()) return;
    if (Busy()) {
        app.SetStatus("The designer is still working on the last one");
        g_d.open = g_d.focus = true;
        return;
    }
    g_d.open = g_d.focus = true;
    SetEditsMode(2);
    app.requestTab = 6;
    if (_wcsicmp(g_d.base.c_str(), paths.front().c_str()) != 0) { // a new picture: a new design
        g_d.base = paths.front();
        g_d.parts.clear();
        g_d.steps.clear();
        g_d.png.clear();
        g_d.psd.clear();
        g_d.log.clear();
        g_d.error.clear();
        g_d.status.clear();
        ReleaseTex();
        std::vector<uint8_t> rgba;
        g_d.baseW = g_d.baseH = 0;
        PixelLoadRGBA(g_d.base, rgba, g_d.baseW, g_d.baseH);
    }
    for (size_t i = 1; i < paths.size(); ++i) AddPart(paths[i]);
    if (!g_d.ref.ready && !g_d.refFut.valid()) {
        const std::wstring cli = PhotocraftExe(L"photocraft-cli.exe");
        if (cli.empty()) g_d.error = "PhotoCraft is not installed: photocraft-cli.exe belongs in the tools\\photocraft folder next to this program";
        else g_d.refFut = std::async(std::launch::async, LoadReference, cli);
    }
}

void DrawDesigner(AppState& app) { Pump(app); } // (its page: Edits > Text over a picture)

void DrawDesignerBody(AppState& app, float height) {
    if (g_d.base.empty()) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Text and graphics over a picture: memes, thumbnails, captions, speech bubbles, banners, people placed into the scene. The "
                            "prompt writer plans it, PhotoCraft draws real text layers (a PNG and a layered .psd).");
        ImGui::PopTextWrapPos();
        if (ImGui::Button("The gallery's picture")) {
            if (app.selectedIndex >= 0 && app.selectedIndex < static_cast<int>(app.entries.size())) OpenDesigner(app, {app.entries[app.selectedIndex].path});
            else app.SetStatus("Pick a picture in the gallery first");
        }
        ImGui::SameLine();
        if (ImGui::Button("A file...")) {
            const std::wstring f = OpenImageFileDialog(GetActiveWindow());
            if (!f.empty()) OpenDesigner(app, {f});
        }
        return;
    }
    ImGui::BeginChild("##designbody", ImVec2(0, height), ImGuiChildFlags_None);
    const float th = ui::Dp(72.0f);
    Thumb(app, g_d.base, th);
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextUnformatted(ToUtf8(fs::path(g_d.base).filename().wstring()).c_str());
    ImGui::TextDisabled("%d x %d pixels", g_d.baseW, g_d.baseH);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Say what to add: titles, captions, speech bubbles, banners, a logo, a colour grade, or people and things placed into the scene. "
                        "The prompt writer looks at the picture and writes PhotoCraft's steps; PhotoCraft makes a PNG and a layered .psd you can change.");
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();

    ImGui::Spacing();
    ImGui::TextUnformatted("Pictures to place (cut out first)");
    int removeAt = -1;
    for (size_t i = 0; i < g_d.parts.size(); ++i) {
        Part& p = g_d.parts[i];
        ImGui::PushID(static_cast<int>(i));
        if (i) ImGui::SameLine();
        ImGui::BeginGroup();
        Thumb(app, p.cutPng.empty() ? p.src : p.cutPng, ui::Dp(64.0f));
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("%s%s%s", ToUtf8(fs::path(p.src).filename().wstring()).c_str(), p.error.empty() ? "" : "\n", p.error.c_str());
        ImGui::BeginDisabled(Busy());
        if (ImGui::Checkbox("cut", &p.cut)) {
            p.cutPng.clear();
            p.job = 0;
            p.w = p.h = 0;
            p.error.clear();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) removeAt = static_cast<int>(i);
        ImGui::EndDisabled();
        ImGui::EndGroup();
        ImGui::PopID();
    }
    if (removeAt >= 0) g_d.parts.erase(g_d.parts.begin() + removeAt);
    if (!g_d.parts.empty()) ImGui::SameLine();
    if (ImGui::Button("+ Add a picture...")) AddPart(OpenImageFileDialog(GetActiveWindow()));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("A character, an object or a logo to put into the picture (or drag one here from the gallery or the Vision library)");
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("MV_LIBITEM")) AddPart(std::wstring(static_cast<const wchar_t*>(pl->Data)));
        else if (const ImGuiPayload* pg = ImGui::AcceptDragDropPayload("MV_IMAGES")) {
            const int* idx = static_cast<const int*>(pg->Data);
            for (int k = 0; k < static_cast<int>(pg->DataSize / sizeof(int)); ++k) {
                if (idx[k] >= 0 && idx[k] < static_cast<int>(app.entries.size())) AddPart(app.entries[idx[k]].path);
            }
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::Spacing();
    ImGui::TextUnformatted(g_d.png.empty() ? "What to do" : "What to change (or a new request)");
    static const std::pair<const char*, const char*> kIdeas[] = {
        {"Meme", "A classic meme: a funny caption about what is happening in the picture, white Impact in capitals with a thick black stroke, the setup at the top and "
                 "the punchline at the bottom."},
        {"Funny edit", "A funny edit: a speech bubble with a silly line from the main character, a comic sound effect word (BONK!, POW!) with a bold colourful "
                       "outline, and a playful caption."},
        {"Clickbait thumbnail", "A clickbait YouTube thumbnail: 2 to 4 huge shouting words (yellow or white, thick black stroke, drop shadow) on the empty side, a big "
                                "red arrow pointing at the most interesting thing, a red circle around it, much more saturation and contrast."},
        {"YouTube thumbnail", "A clean YouTube thumbnail: a short bold title of 3 to 5 words on the emptier side with a solid colour band or shadow behind it for "
                              "contrast, a small accent detail, slightly more vivid colours."},
        {"Movie poster", "A movie poster: a big cinematic title with a glow or metallic look, a short tagline, a credits block small at the bottom, a release line, "
                         "and a dramatic colour grade."},
        {"Speech bubble", "A white rounded speech bubble near the main character's head with a short funny line in a comic font and a black outline."},
        {"Magazine cover", "A magazine cover: the magazine name huge across the top, three cover lines on the left side, a price and issue line, all clear of the face."},
        {"Into the scene", "Place the added picture into the scene standing on the ground, in proportion, with a soft shadow and its light matched to the scene."},
        {"Explainer scene", "An explainer-video collage: the added pictures cut out and placed into the scene at believable sizes on the ground, the most important "
                            "one with a golden glow, a black date banner at the top-left, and a hologram screen if a picture is meant to be shown on a screen."},
    };
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    for (size_t i = 0; i < sizeof(kIdeas) / sizeof(kIdeas[0]); ++i) {
        const float bw = ImGui::CalcTextSize(kIdeas[i].first).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        if (i && ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + bw < right) ImGui::SameLine();
        if (ImGui::SmallButton(kIdeas[i].first)) g_d.request = kIdeas[i].second;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s\n(it fills in the request: change any of it, add your own words or text)", kIdeas[i].second);
    }
    WrapEdit("##designreq", g_d.request, ui::Dp(80.0f));

    const bool haveCli = !PhotocraftExe(L"photocraft-cli.exe").empty();
    const bool canPlan = haveCli && g_d.ref.ready && WriterConfigured() && !Trim(g_d.request).empty() && !Busy();
    ImGui::BeginDisabled(!canPlan);
    auto go = [&](bool change) {
        g_d.change = change;
        g_d.attempt = 0;
        g_d.error.clear();
        g_d.waitingCuts = true;
        g_d.status = g_d.parts.empty() ? "Starting..." : "Cutting out the pictures to place (SAM 3)...";
    };
    if (ui::PrimaryButton(g_d.png.empty() ? "Make it" : "Make it again", ImVec2(ui::Dp(150.0f), 0)) ||
        (canPlan && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Enter)))
        go(false);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("A new design from the request (the steps are planned from scratch)  -  Ctrl+Enter");
    if (!g_d.steps.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Change it")) go(true);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("The request changes the current design (\"make the title red\", \"move it lower\", \"add a subtitle\"); the rest stays");
    }
    ImGui::EndDisabled();
    if (!WriterConfigured()) {
        ImGui::SameLine();
        ImGui::TextColored(ui::kWarn, "Set up the prompt writer first (Clef mode tab > Prompt writer)");
    } else if (!WriterCanSee()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(the writer cannot see pictures: it designs blind)");
    }
    if (Busy() || !g_d.status.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(Busy() ? ui::kAccent : ui::kGood, "%s", g_d.status.c_str());
    }
    if (!g_d.error.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(ui::kBad, "%s", g_d.error.c_str());
        ImGui::PopTextWrapPos();
        if (!g_d.reply.empty() && ImGui::CollapsingHeader("What the prompt writer answered")) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("%s", g_d.reply.size() > 6000 ? (g_d.reply.substr(0, 6000) + "...").c_str() : g_d.reply.c_str());
            ImGui::PopTextWrapPos();
        }
    }

    // the result
    if (!g_d.png.empty()) {
        if (g_d.texOf != g_d.png) {
            ReleaseTex();
            std::string err;
            LoadImageTexture(app.Device(), g_d.png, g_d.tex, &err);
            g_d.texOf = g_d.png;
        }
        if (ImGui::SmallButton("Save as PNG...")) {
            const std::wstring path = SavePngFileDialog(GetActiveWindow(), fs::path(g_d.png).filename().wstring());
            if (!path.empty()) {
                std::error_code ec;
                fs::copy_file(g_d.png, path, fs::copy_options::overwrite_existing, ec);
                app.SetStatus(ec ? "Could not save it: " + ec.message() : "Saved " + ToUtf8(fs::path(path).filename().wstring()));
            }
        }
        ImGui::SameLine();
        const std::wstring app_ = PhotocraftExe(L"photocraft.exe");
        ImGui::BeginDisabled(app_.empty());
        if (ImGui::SmallButton("Open in PhotoCraft")) ShellExecuteW(nullptr, L"open", app_.c_str(), QuoteArg(g_d.psd).c_str(), nullptr, SW_SHOWNORMAL);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("The layered .psd: every text, shape, picture and adjustment stays editable");
        ImGui::SameLine();
        if (ImGui::SmallButton("Open in the gallery")) {
            app.OpenFile(g_d.png);
            app.requestTab = 0;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Show in Explorer")) {
            const std::wstring args = L"/select,\"" + g_d.png + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Design on the result")) { // the result becomes the picture (another layer of changes on top)
            const std::wstring r = g_d.png;
            g_d.base.clear();
            OpenDesigner(app, {r});
        }
        if (g_d.tex.srv && g_d.tex.width > 0) {
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            const float maxH = std::max(ui::Dp(160.0f), avail.y - ui::Dp(150.0f));
            const float s = std::min(avail.x / g_d.tex.width, maxH / g_d.tex.height);
            ImGui::Image((ImTextureID)(intptr_t)g_d.tex.srv, ImVec2(g_d.tex.width * s, g_d.tex.height * s));
        }
    }
    if (!g_d.steps.empty() || !g_d.log.empty()) {
        if (ImGui::CollapsingHeader("The steps (PhotoCraft commands)")) {
            WrapEdit("##designsteps", g_d.steps, ui::Dp(180.0f));
            ImGui::BeginDisabled(Busy() || !haveCli);
            if (ImGui::SmallButton("Run these steps")) {
                g_d.error.clear();
                g_d.attempt = 1; // edited by hand: no automatic fixing
                StartRun(app);
            }
            ImGui::EndDisabled();
            if (!g_d.log.empty()) {
                ImGui::TextDisabled("PhotoCraft's log");
                ImGui::PushTextWrapPos(0);
                ImGui::TextDisabled("%s", g_d.log.size() > 4000 ? (g_d.log.substr(0, 4000) + "...").c_str() : g_d.log.c_str());
                ImGui::PopTextWrapPos();
            }
        }
    }
    ImGui::EndChild();
}

// For tests: MV_DESIGN_TEST=<picture>|<request>[|<picture to place>]
void DesignerTestHook(AppState& app) {
    static bool done = false;
    if (done) return;
    done = true;
    wchar_t buf[4096] = {};
    const DWORD n = GetEnvironmentVariableW(L"MV_DESIGN_TEST", buf, 4096);
    if (n == 0 || n >= 4096) return;
    std::wstring v(buf, n);
    std::vector<std::wstring> f;
    size_t at = 0;
    while (at <= v.size()) {
        const size_t bar = v.find(L'|', at);
        f.push_back(v.substr(at, bar == std::wstring::npos ? std::wstring::npos : bar - at));
        if (bar == std::wstring::npos) break;
        at = bar + 1;
    }
    if (f.size() < 2) return;
    std::vector<std::wstring> paths = {f[0]};
    for (size_t i = 2; i < f.size(); ++i) paths.push_back(f[i]);
    OpenDesigner(app, paths);
    if (!f[1].empty() && f[1][0] == L'@') { // ready-made steps: no planner (the pictures to place are used as they are)
        std::ifstream sf(fs::path(f[1].substr(1)), std::ios::binary);
        g_d.steps.assign(std::istreambuf_iterator<char>(sf), std::istreambuf_iterator<char>());
        for (Part& p : g_d.parts) {
            p.cut = false;
            p.cutPng = p.src;
            std::vector<uint8_t> rgba;
            PixelLoadRGBA(p.cutPng, rgba, p.w, p.h);
        }
        g_d.attempt = 1;
        StartRun(app);
        return;
    }
    g_d.request = ToUtf8(f[1]);
    g_d.waitingCuts = true; // planned as soon as the command list is read
    g_d.status = "Starting...";
}
