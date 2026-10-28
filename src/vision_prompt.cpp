#include <cctype>
#include "vision_prompt.h"
#include "metadata.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <thread>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <windows.h>
#include <winhttp.h>
#include <tlhelp32.h>
#include <cwctype>

#include "json.hpp"
#include "nsfw_classifier.h" // DecodeImageFileRGBA
#include "stb_image_write.h"

namespace fs = std::filesystem;

namespace {

// Synchronous WinHTTP request against the Ollama server. Returns false only
// on transport errors; HTTP errors come back through `status`.
bool HttpRequest(const OllamaEndpoint& ep, const wchar_t* method, const wchar_t* path, const std::string& body,
                 std::string& response, DWORD& status, DWORD timeoutMs, std::string* error) {
    bool ok = false;
    HINTERNET session = WinHttpOpen(L"Try2Create/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connect = session ? WinHttpConnect(session, ep.host.c_str(), static_cast<INTERNET_PORT>(ep.port), 0)
                                : nullptr;
    HINTERNET request = connect ? WinHttpOpenRequest(connect, method, path, nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, 0)
                                : nullptr;
    if (request) {
        // Connecting is quick or not at all; generating can take minutes on
        // a cold model load.
        WinHttpSetTimeouts(request, 3000, 3000, 60000, static_cast<int>(timeoutMs));
        const wchar_t* headers = L"Content-Type: application/json\r\n";
        if (WinHttpSendRequest(request, headers, static_cast<DWORD>(-1), const_cast<char*>(body.data()),
                               static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD size = sizeof(status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(request, &avail) && avail > 0) {
                std::string chunk(avail, '\0');
                DWORD read = 0;
                if (!WinHttpReadData(request, chunk.data(), avail, &read)) break;
                response.append(chunk.data(), read);
            }
            ok = true;
        }
    }
    if (!ok && error) {
        DWORD code = GetLastError();
        *error = code == ERROR_WINHTTP_CANNOT_CONNECT ? "Ollama isn't running"
                 : code == ERROR_WINHTTP_TIMEOUT      ? "Ollama didn't answer in time"
                                                      : "Couldn't reach Ollama (error " + std::to_string(code) + ")";
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    return ok;
}

// POSTs JSON and parses the JSON reply; Ollama reports failures as {"error": "..."}.
bool PostJson(const OllamaEndpoint& ep, const wchar_t* path, const nlohmann::json& body, nlohmann::json& reply,
              DWORD timeoutMs, std::string* error) {
    std::string response;
    DWORD status = 0;
    if (!HttpRequest(ep, L"POST", path, body.dump(), response, status, timeoutMs, error)) return false;
    reply = nlohmann::json::parse(response, nullptr, /*allow_exceptions=*/false);
    if (reply.is_object() && reply.contains("error")) {
        if (error) *error = "Ollama: " + reply["error"].get<std::string>();
        return false;
    }
    if (status != 200 || reply.is_discarded()) {
        if (error) *error = "Ollama returned HTTP " + std::to_string(status);
        return false;
    }
    return true;
}

std::string Base64(const std::string& in) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t v = (uint8_t)in[i] << 16 | (uint8_t)in[i + 1] << 8 | (uint8_t)in[i + 2];
        out += tbl[v >> 18 & 63];
        out += tbl[v >> 12 & 63];
        out += tbl[v >> 6 & 63];
        out += tbl[v & 63];
    }
    if (i < in.size()) {
        uint32_t v = (uint8_t)in[i] << 16 | (i + 1 < in.size() ? (uint8_t)in[i + 1] << 8 : 0);
        out += tbl[v >> 18 & 63];
        out += tbl[v >> 12 & 63];
        out += i + 1 < in.size() ? tbl[v >> 6 & 63] : '=';
        out += '=';
    }
    return out;
}

// Decodes any supported format, shrinks it so the long side is <= maxSide
// (area-averaged), and re-encodes as JPEG: small enough to send quickly,
// and a format every vision model accepts (WebP etc. aren't).
bool EncodeForVision(const std::wstring& path, int maxSide, std::string& jpeg, std::string* error) {
    std::vector<unsigned char> rgba;
    int w = 0, h = 0;
    if (!DecodeImageFileRGBA(path, rgba, w, h)) {
        if (error) *error = "Couldn't decode the image";
        return false;
    }
    int dw = w, dh = h;
    if (std::max(w, h) > maxSide) {
        double s = static_cast<double>(maxSide) / std::max(w, h);
        dw = std::max(1, static_cast<int>(w * s));
        dh = std::max(1, static_cast<int>(h * s));
    }
    std::vector<unsigned char> rgb(static_cast<size_t>(dw) * dh * 3);
    for (int y = 0; y < dh; ++y) {
        int y0 = y * h / dh, y1 = std::max(y0 + 1, (y + 1) * h / dh);
        for (int x = 0; x < dw; ++x) {
            int x0 = x * w / dw, x1 = std::max(x0 + 1, (x + 1) * w / dw);
            unsigned sum[3] = {0, 0, 0};
            for (int sy = y0; sy < y1; ++sy) {
                const unsigned char* p = &rgba[(static_cast<size_t>(sy) * w + x0) * 4];
                for (int sx = x0; sx < x1; ++sx, p += 4) {
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                }
            }
            unsigned n = static_cast<unsigned>((x1 - x0) * (y1 - y0));
            unsigned char* o = &rgb[(static_cast<size_t>(y) * dw + x) * 3];
            for (int c = 0; c < 3; ++c) o[c] = static_cast<unsigned char>(sum[c] / n);
        }
    }
    jpeg.clear();
    auto sink = [](void* ctx, void* data, int size) { static_cast<std::string*>(ctx)->append((const char*)data, size); };
    if (!stbi_write_jpg_to_func(sink, &jpeg, dw, dh, 3, rgb.data(), 92)) {
        if (error) *error = "Couldn't encode the image";
        return false;
    }
    return true;
}

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n\"");
    size_t b = s.find_last_not_of(" \t\r\n\"");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool SameFileContents(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    if (fs::file_size(a, ec) != fs::file_size(b, ec) || ec) return false;
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    std::vector<char> ba(1 << 20), bb(1 << 20);
    while (fa && fb) {
        fa.read(ba.data(), ba.size());
        fb.read(bb.data(), bb.size());
        if (fa.gcount() != fb.gcount() || std::memcmp(ba.data(), bb.data(), static_cast<size_t>(fa.gcount())) != 0)
            return false;
    }
    return true;
}

} // namespace

// A picture as a data URL for a model's "images" input: shrunk to maxSide and re-encoded as JPEG ("" when it cannot be read).
std::string ImageDataUrl(const std::wstring& path, int maxSide) {
    std::string jpeg;
    if (!EncodeForVision(path, maxSide, jpeg, nullptr)) return {};
    return "data:image/jpeg;base64," + Base64(jpeg);
}


const char* DefaultVisionInstruction() {
    return "Write a prompt that a modern text-to-image model (Flux, SDXL, DALL-E 3, Midjourney) could use to "
           "recreate this image. One detailed paragraph of natural language describing: the subject(s) and their "
           "appearance, clothing, pose and expression; the setting and background; composition and camera angle; "
           "lighting and colors; and the art style or medium. Describe only what you actually see. Output only the "
           "prompt itself, with no preamble or quotes.";
}

static const VisionStyle kStyles[] = {
    {"cartoon", "Cartoon", "a western cartoon illustration: bold clean outlines, flat vibrant colours, simplified shapes and exaggerated expressive features"},
    {"anime", "Anime", "a Japanese anime still: crisp cel shading, large expressive eyes, clean line art, saturated colours and dramatic lighting"},
    {"manga", "Manga (black and white)", "a black-and-white manga page panel: ink line work, screentone shading, speed lines and strong contrast"},
    {"comic", "Comic book", "an American comic book panel: heavy ink outlines, halftone dots, dynamic poses and punchy primary colours"},
    {"pixel-art", "Pixel art", "retro pixel art: a limited colour palette, visible square pixels, dithering and sprite-like shapes"},
    {"watercolor", "Watercolor", "a watercolor painting: soft translucent washes, bleeding edges, visible paper texture and gentle colour gradients"},
    {"oil-painting", "Oil painting", "a classical oil painting: rich pigments, visible brush strokes, chiaroscuro lighting and canvas texture"},
    {"pencil-sketch", "Pencil sketch", "a graphite pencil sketch: hatching and cross-hatching, soft smudged shading, monochrome on off-white paper"},
    {"3d-render", "3D render (animated film)", "a stylised 3D animated-film render: soft subsurface skin, rounded forms, glossy materials and warm studio lighting"},
    {"claymation", "Claymation", "stop-motion claymation: handmade clay figures, fingerprint textures, chunky shapes and miniature set lighting"},
    {"low-poly", "Low poly", "low-poly 3D art: faceted flat-shaded triangles, simple geometry and a clean gradient palette"},
    {"cyberpunk", "Cyberpunk", "a cyberpunk scene: neon magenta and cyan lights, rain-slick streets, holograms, chrome and dense futuristic detail"},
    {"fantasy", "Fantasy illustration", "an epic fantasy book illustration: painterly detail, magical glowing light, ornate costumes and dramatic scale"},
    {"photorealistic", "Photorealistic", "a photorealistic photograph: natural lighting, real skin and material textures, shallow depth of field, shot on a full-frame camera"},
    {"vintage-photo", "Vintage film photo", "a vintage film photograph: grain, faded colours, soft halation, light leaks and a 1970s snapshot look"},
    {"noir", "Film noir", "film noir: black-and-white, hard shadows, venetian-blind light, smoke and high contrast"},
    {"ukiyo-e", "Ukiyo-e woodblock", "a Japanese ukiyo-e woodblock print: flat colour areas, bold outlines, washi paper texture and stylised waves and clouds"},
    {"art-nouveau", "Art nouveau poster", "an art nouveau poster: flowing organic lines, floral ornament, muted gilded palette and decorative borders"},
};

int VisionStyleCount() { return static_cast<int>(sizeof(kStyles) / sizeof(kStyles[0])); }
const VisionStyle& VisionStyleAt(int index) { return kStyles[index < 0 ? 0 : (index >= VisionStyleCount() ? VisionStyleCount() - 1 : index)]; }

std::string StyleSlug(const std::string& text) {
    std::string out;
    for (unsigned char c : text) {
        if (std::isalnum(c)) out += static_cast<char>(std::tolower(c));
        else if (!out.empty() && out.back() != '-') out += '-';
        if (out.size() >= 28) break;
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "custom" : out;
}

std::string BuildStyledInstruction(const std::string& baseInstruction, const std::string& styleDirective) {
    return baseInstruction + "\n\nStyle conversion: write the prompt as if this image had been redrawn as " + styleDirective +
           ". Keep the subjects, pose, composition, setting and colour mood of the picture, but describe everything in the visual "
           "language of that style (medium, line work, shading, textures, palette) and name the style explicitly in the prompt. "
           "Output only the prompt itself.";
}

const char* DefaultTagsInstruction() {
    return "List tags for this image as a Danbooru-style prompt for Stable Diffusion models. Output a single "
           "comma-separated line of lowercase tags, most important first: subject count (1girl, 2boys, solo, no humans), "
           "hair color and style, eye color, expression, clothing and accessories, pose and action, body framing "
           "(portrait, upper body, cowboy shot, full body), setting and background, lighting, colors, camera angle, and "
           "art style or medium. Only tags for things you can actually see; use spaces, not underscores. Output only "
           "the tags, with no sentences, numbering or preamble.";
}

static const char* const kVideoCameras[] = {
    "Let the model choose camera movement that suits the scene",
    "Static camera, locked off, no camera movement",
    "Slow push-in towards the subject",
    "Slow pull-back that reveals more of the surroundings",
    "Smooth pan from left to right",
    "Slow orbit around the subject",
    "Tracking shot that follows the subject's movement",
    "Handheld camera with subtle natural shake",
    "Crane movement: rise up to reveal the scene",
};

int VideoCameraCount() { return static_cast<int>(sizeof(kVideoCameras) / sizeof(kVideoCameras[0])); }

const char* VideoCameraLabel(int index) {
    if (index < 0 || index >= VideoCameraCount()) index = 0;
    return kVideoCameras[index];
}

int VideoBeatCount(int seconds) {
    int n = static_cast<int>(seconds / 5.0 + 0.5);
    return n < 1 ? 1 : (n > 6 ? 6 : n);
}

int VideoTokenBudget(int seconds) {
    int tokens = 350 + seconds * 55; // 5 s ~ 625, 30 s ~ 2000
    return tokens < 600 ? 600 : (tokens > 2600 ? 2600 : tokens);
}

const char* DefaultVideoTemplate() {
    return "You write prompts for image-to-video generators (Wan, Kling, Veo, Runway, Sora, LTX). The attached image is the "
           "FIRST FRAME of a {SECONDS}-second video. Write a detailed description of the whole {SECONDS}-second video that "
           "begins exactly from this frame.\n\nRules:\n"
           "- Keep the subject(s), their appearance and outfit, the setting, lighting and the art style exactly as in the "
           "image. Do not add new main characters or change the style.\n"
           "- Pace the action to the length: {PACE}.\n"
           "- Describe only motion that is plausible for the subject and scene. Be concrete about movement: what moves, how "
           "fast, in which direction. Do not just describe mood.\n"
           "- Camera: {CAMERA}.\n"
           "{STRUCTURE}{SOUND}{EXTRA}"
           "\nOutput only the scene description, with no preamble, title or quotes.";
}

std::string BuildVideoInstruction(const VideoSceneOptions& o) {
    const int s = o.seconds < 5 ? 5 : (o.seconds > 30 ? 30 : o.seconds);
    const std::string secs = std::to_string(s);
    const char* pace = s <= 6    ? "a single simple action or small movement; do not rush a story into a few seconds"
                       : s <= 12 ? "one main action with a small development"
                       : s <= 20 ? "two or three sequential beats, each flowing into the next"
                                 : "a short story of several beats with a clear progression and a satisfying end";
    auto clock = [](int total) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d:%02d", total / 60, total % 60);
        return std::string(buf);
    };

    std::string structure;
    if (o.timeline) {
        const int beats = VideoBeatCount(s);
        const int len = static_cast<int>(static_cast<double>(s) / beats + 0.5);
        structure = "- Structure it as " + std::to_string(beats) + " time-coded beat" + (beats == 1 ? "" : "s") +
                    (beats == 1 ? " covering " : " that together cover ") + "0:00 to " + clock(s) +
                    ", one per line, like \"0:00-" + clock(len) + " - ...\", each about " + std::to_string(len) +
                    " seconds long. In each beat say what the subject does, how the camera moves and how light or atmosphere "
                    "changes. Finish with a line starting \"Final frame:\" describing how the video ends.\n";
    } else {
        int words = 35 + s * 7;
        words = words < 70 ? 70 : (words > 260 ? 260 : words);
        structure = "- Write one flowing paragraph of roughly " + std::to_string(words) + " words in the present tense that "
                    "covers the whole " + secs + " seconds in order, including the camera movement and how the video ends.\n";
    }
    const std::string sound = o.sound ? "- Add a last line starting \"Sound:\" describing the ambient sound, music mood and any effects that fit.\n"
                                      : "- Do not describe audio.\n";
    const std::string extra = o.extra.empty() ? std::string()
                                              : "- Extra direction from the user (follow it where it does not contradict the image): " + o.extra + "\n";

    const std::string tmpl = o.templateText.empty() ? std::string(DefaultVideoTemplate()) : o.templateText;
    const std::pair<const char*, std::string> values[] = {
        {"{SECONDS}", secs},          {"{PACE}", pace},   {"{CAMERA}", VideoCameraLabel(o.camera)},
        {"{STRUCTURE}", structure},   {"{SOUND}", sound}, {"{EXTRA}", extra},
    };
    std::string out;
    out.reserve(tmpl.size() + 600);
    for (size_t i = 0; i < tmpl.size();) {
        bool matched = false;
        if (tmpl[i] == '{') {
            for (const auto& [key, value] : values) {
                const size_t n = std::strlen(key);
                if (tmpl.compare(i, n, key) == 0) {
                    out += value;
                    i += n;
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) out += tmpl[i++];
    }
    return out;
}

bool OllamaIsRunning(const OllamaEndpoint& ep) {
    std::string response;
    DWORD status = 0;
    return HttpRequest(ep, L"GET", L"/api/version", "", response, status, 3000, nullptr) && status == 200;
}

namespace {
std::atomic<bool> g_killWithApp{false};

// One job for the process lifetime; its handle closes when the app dies, which ends everything in it.
HANDLE AppKillJob() {
    static HANDLE job = []() -> HANDLE {
        HANDLE h = CreateJobObjectW(nullptr, nullptr);
        if (!h) return nullptr;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(h, JobObjectExtendedLimitInformation, &info, sizeof(info))) {
            CloseHandle(h);
            return nullptr;
        }
        return h;
    }();
    return job;
}
} // namespace

void OllamaSetKillWithApp(bool enabled) { g_killWithApp = enabled; }

bool OllamaStartServer(const OllamaEndpoint& ep, std::string* error) {
    if (OllamaIsRunning(ep)) return true;
    wchar_t exe[MAX_PATH];
    if (!SearchPathW(nullptr, L"ollama.exe", nullptr, MAX_PATH, exe, nullptr)) {
        if (error) *error = "ollama.exe not found on PATH (install it from ollama.com)";
        return false;
    }
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" serve";
    // A hidden console (not DETACHED_PROCESS) so the helper processes Ollama
    // spawns inherit it instead of each opening a console window of its own.
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    // Started suspended so it can join the job before it spawns anything of its own.
    const bool tie = g_killWithApp.load() && AppKillJob() != nullptr;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | (tie ? CREATE_SUSPENDED : 0), nullptr, nullptr, &si, &pi)) {
        if (error) *error = "Couldn't start ollama serve";
        return false;
    }
    if (tie) {
        AssignProcessToJobObject(AppKillJob(), pi.hProcess);
        ResumeThread(pi.hThread);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    for (int i = 0; i < 40; ++i) {
        if (OllamaIsRunning(ep)) return true;
        Sleep(500);
    }
    if (error) *error = "Started Ollama but it didn't come up";
    return false;
}

namespace {
std::wstring LowerW(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

// Ollama's model runner ("llama-server.exe" in current versions, "ollama_llama_server.exe" in older ones) holds the
// model in memory and is a separate process: ending ollama.exe does not end it, so it must be found and ended too.
bool IsOllamaRunner(const std::wstring& lowerName, DWORD pid) {
    if (lowerName.find(L"llama-server") == std::wstring::npos && lowerName.find(L"llama_server") == std::wstring::npos &&
        lowerName.find(L"ollama") == std::wstring::npos) {
        return false;
    }
    if (lowerName.rfind(L"ollama", 0) == 0) return true;
    // a runner is ours only when it lives in Ollama's install folder (not some other llama.cpp server)
    bool ours = false;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH * 2];
        DWORD n = MAX_PATH * 2;
        if (QueryFullProcessImageNameW(h, 0, path, &n)) ours = LowerW(std::wstring(path, n)).find(L"ollama") != std::wstring::npos;
        CloseHandle(h);
    }
    return ours;
}
} // namespace

void OllamaStopServer() {
    // Tray app first, or it immediately restarts the server we kill next. Then everything of Ollama's: the server,
    // its runner processes (also ones left behind by an earlier crash) and anything started by them.
    for (int pass = 0; pass < 3; ++pass) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return;
        std::vector<PROCESSENTRY32W> all;
        PROCESSENTRY32W pe = {sizeof(pe)};
        for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe)) all.push_back(pe);
        CloseHandle(snap);

        std::vector<DWORD> victims;
        for (const PROCESSENTRY32W& e : all) {
            const std::wstring name = LowerW(e.szExeFile);
            if (pass == 0) {
                if (name == L"ollama app.exe") victims.push_back(e.th32ProcessID);
            } else if (IsOllamaRunner(name, e.th32ProcessID)) {
                victims.push_back(e.th32ProcessID);
            }
        }
        if (pass > 0) { // children of those (the runner can be started by ollama.exe under another name)
            for (size_t i = 0; i < victims.size(); ++i) {
                for (const PROCESSENTRY32W& e : all) {
                    if (e.th32ParentProcessID == victims[i] && std::find(victims.begin(), victims.end(), e.th32ProcessID) == victims.end() &&
                        e.th32ProcessID != GetCurrentProcessId()) {
                        victims.push_back(e.th32ProcessID);
                    }
                }
            }
        }
        for (DWORD pid : victims) {
            if (HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid)) {
                TerminateProcess(h, 0);
                WaitForSingleObject(h, 3000);
                CloseHandle(h);
            }
        }
        if (pass == 1 && victims.empty()) break;
    }
}

bool OllamaListVisionModels(const OllamaEndpoint& ep, std::vector<VisionModelInfo>& out, std::string* error) {
    out.clear();
    std::string response;
    DWORD status = 0;
    if (!HttpRequest(ep, L"GET", L"/api/tags", "", response, status, 10000, error)) return false;
    nlohmann::json tags = nlohmann::json::parse(response, nullptr, false);
    if (!tags.is_object() || !tags.contains("models")) {
        if (error) *error = "Unexpected reply from Ollama";
        return false;
    }
    for (auto& m : tags["models"]) {
        std::string name = m.value("name", std::string());
        if (name.empty()) continue;
        nlohmann::json show;
        if (!PostJson(ep, L"/api/show", {{"model", name}}, show, 20000, nullptr)) continue;
        std::vector<std::string> caps;
        if (show.contains("capabilities") && show["capabilities"].is_array())
            for (auto& c : show["capabilities"]) caps.push_back(c.get<std::string>());
        if (std::find(caps.begin(), caps.end(), "vision") == caps.end()) continue;
        VisionModelInfo info;
        info.name = name;
        info.supportsThinking = std::find(caps.begin(), caps.end(), "thinking") != caps.end();
        if (show.contains("details")) {
            info.sizeLabel = show["details"].value("parameter_size", std::string()) + " " +
                             show["details"].value("quantization_level", std::string());
        }
        out.push_back(std::move(info));
    }
    return true;
}

bool OllamaLoadedModels(const OllamaEndpoint& ep, std::vector<std::string>& out, std::string* error) {
    out.clear();
    std::string response;
    DWORD status = 0;
    if (!HttpRequest(ep, L"GET", L"/api/ps", "", response, status, 5000, error)) return false;
    nlohmann::json ps = nlohmann::json::parse(response, nullptr, false);
    if (ps.is_object() && ps.contains("models"))
        for (auto& m : ps["models"]) out.push_back(m.value("name", std::string()));
    return true;
}

bool OllamaLoadModel(const OllamaEndpoint& ep, const std::string& model, const std::string& keepAlive,
                     std::string* error) {
    nlohmann::json reply;
    nlohmann::json body = {{"model", model}};
    if (keepAlive == "-1") body["keep_alive"] = -1; // number: keep until unloaded
    else body["keep_alive"] = keepAlive;
    return PostJson(ep, L"/api/generate", body, reply, 10 * 60 * 1000, error);
}

bool OllamaUnloadModel(const OllamaEndpoint& ep, const std::string& model, std::string* error, int timeoutMs) {
    nlohmann::json reply;
    return PostJson(ep, L"/api/generate", {{"model", model}, {"keep_alive", 0}}, reply, static_cast<DWORD>(timeoutMs), error);
}

bool OllamaDescribeImage(const OllamaEndpoint& ep, const std::string& model, bool modelThinks,
                         const std::string& keepAlive, const std::string& instruction, const std::string& tags,
                         const std::wstring& imagePath, std::string& outText, std::string* error, int numPredict,
                         double temperature) {
    std::string jpeg;
    if (!EncodeForVision(imagePath, 1536, jpeg, error)) return false;

    std::string prompt = instruction.empty() ? DefaultVisionInstruction() : instruction;
    if (!tags.empty()) {
        prompt += "\n\nThe image was originally generated with these tags (they may be inaccurate; trust the image): " +
                  tags.substr(0, 2000);
    }
    nlohmann::json body = {
        {"model", model},
        {"prompt", prompt},
        {"images", nlohmann::json::array({Base64(jpeg)})},
        {"stream", false},
        {"options", {{"temperature", temperature}}},
    };
    if (numPredict > 0) body["options"]["num_predict"] = numPredict;
    if (keepAlive == "-1") body["keep_alive"] = -1;
    else body["keep_alive"] = keepAlive;
    // Thinking models would otherwise spend a long time reasoning first;
    // non-thinking models reject the field, so only send it when supported.
    if (modelThinks) body["think"] = false;

    nlohmann::json reply;
    if (!PostJson(ep, L"/api/generate", body, reply, 10 * 60 * 1000, error)) return false;
    std::string text = reply.value("response", std::string());
    // Some models still emit a <think> block inline; drop it.
    size_t end = text.find("</think>");
    if (end != std::string::npos) text = text.substr(end + 8);
    outText = Trim(text);
    if (outText.empty()) {
        if (error) *error = "The model returned an empty description";
        return false;
    }
    return true;
}

// A plain generate call: prompt + any number of images -> text.
static bool OllamaGenerate(const OllamaEndpoint& ep, const std::string& model, bool modelThinks, const std::string& keepAlive,
                           const std::string& prompt, const std::vector<std::wstring>& images, int maxSide, std::string& outText,
                           std::string* error, int numPredict, double temperature) {
    nlohmann::json imgs = nlohmann::json::array();
    for (const std::wstring& path : images) {
        std::string jpeg;
        if (!EncodeForVision(path, maxSide, jpeg, error)) return false;
        imgs.push_back(Base64(jpeg));
    }
    nlohmann::json body = {
        {"model", model},
        {"prompt", prompt},
        {"stream", false},
        {"options", {{"temperature", temperature}}},
    };
    if (!imgs.empty()) body["images"] = std::move(imgs);
    if (numPredict > 0) body["options"]["num_predict"] = numPredict;
    if (keepAlive == "-1") body["keep_alive"] = -1;
    else body["keep_alive"] = keepAlive;
    if (modelThinks) body["think"] = false;
    nlohmann::json reply;
    if (!PostJson(ep, L"/api/generate", body, reply, 10 * 60 * 1000, error)) return false;
    std::string text = reply.value("response", std::string());
    const size_t end = text.find("</think>");
    if (end != std::string::npos) text = text.substr(end + 8);
    outText = Trim(text);
    if (outText.empty()) {
        if (error) *error = "The model returned an empty answer";
        return false;
    }
    return true;
}

bool SaveImageWithPrompt(const std::wstring& imagePath, const std::wstring& outDir, const std::string& text,
                         std::wstring& savedImagePath, std::string* error, const std::wstring& txtSuffix) {
    std::error_code ec;
    fs::create_directories(outDir, ec);
    if (!fs::is_directory(outDir, ec)) {
        if (error) *error = "Output folder doesn't exist and couldn't be created";
        return false;
    }
    fs::path src(imagePath);
    fs::path dst = fs::path(outDir) / src.filename();
    for (int n = 2; fs::exists(dst, ec) && !SameFileContents(src, dst); ++n) {
        dst = fs::path(outDir) / (src.stem().wstring() + L" (" + std::to_wstring(n) + L")" + src.extension().wstring());
    }
    if (fs::equivalent(src, dst, ec)) {
        // Output folder is the image's own folder: nothing to copy.
    } else if (!fs::exists(dst, ec) && !CopyFileW(src.c_str(), dst.c_str(), TRUE)) {
        if (error) *error = "Couldn't copy the image";
        return false;
    }
    fs::path txt = txtSuffix.empty() ? fs::path(dst).replace_extension(L".txt")
                                     : dst.parent_path() / (dst.stem().wstring() + txtSuffix + L".txt");
    std::ofstream out(txt, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) *error = "Couldn't write the .txt file";
        return false;
    }
    out << text;
    savedImagePath = dst.wstring();
    return true;
}

bool HasSavedResult(const std::wstring& imagePath, const std::wstring& outDir, const std::wstring& txtSuffix) {
    if (outDir.empty()) return false;
    std::error_code ec;
    fs::path src(imagePath);
    fs::path dst = fs::path(outDir) / src.filename();
    for (int n = 2;; ++n) {
        if (!fs::exists(dst, ec)) return false; // no copy of this image in the output folder yet
        if (fs::equivalent(src, dst, ec) || SameFileContents(src, dst)) break;
        dst = fs::path(outDir) / (src.stem().wstring() + L" (" + std::to_wstring(n) + L")" + src.extension().wstring());
    }
    fs::path txt = txtSuffix.empty() ? fs::path(dst).replace_extension(L".txt")
                                     : dst.parent_path() / (dst.stem().wstring() + txtSuffix + L".txt");
    return fs::exists(txt, ec);
}

// ---------------------------------------------------------------------------
// VisionService
// ---------------------------------------------------------------------------

// Owned jointly by the service and its worker thread, so the thread can be
// detached at exit while a long request is still in flight.
struct VisionService::Shared {
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::function<void()>> queue;
    bool stop = false;
    std::atomic<bool> cancel{false};
    std::atomic<bool> working{false};
    Snapshot snap;
    std::vector<Result> results;
    // Models this app asked Ollama to load, unloaded again on exit.
    std::set<std::string> loadedByUs;
    OllamaEndpoint lastEndpoint;
    std::atomic<bool> used{false};  // anything was sent to Ollama this session
    bool shutDown = false;          // Shutdown() already cleaned up

    void Set(const std::function<void(Snapshot&)>& f) {
        std::lock_guard<std::mutex> lock(mutex);
        f(snap);
    }
    void RefreshLoaded(const OllamaEndpoint& ep) {
        std::vector<std::string> loaded;
        bool ok = OllamaLoadedModels(ep, loaded, nullptr);
        Set([&](Snapshot& s) {
            s.serverRunning = ok;
            s.loadedModels = loaded;
        });
    }
};

VisionService::VisionService() : shared_(std::make_shared<Shared>()) {
    std::thread([s = shared_]() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(s->mutex);
                s->cv.wait(lock, [&] { return s->stop || !s->queue.empty(); });
                if (s->stop) return;
                task = std::move(s->queue.front());
                s->queue.pop_front();
                s->snap.busy = true;
            }
            s->working = true;
            try {
                task();
            } catch (...) {
                s->Set([](Snapshot& snap) { snap.lastError = "Unexpected error"; });
            }
            s->working = false;
            std::lock_guard<std::mutex> lock(s->mutex);
            s->snap.busy = !s->queue.empty();
            if (!s->snap.busy) s->snap.activity.clear();
        }
    }).detach();
}

VisionService::~VisionService() {
    std::set<std::string> toUnload;
    OllamaEndpoint ep;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        if (shared_->shutDown) return;
        shared_->stop = true;
        shared_->queue.clear();
        toUnload = shared_->loadedByUs;
        ep = shared_->lastEndpoint;
    }
    shared_->cancel = true;
    shared_->cv.notify_all();
    // Free the GPU memory now rather than when keep_alive runs out.
    for (const auto& model : toUnload) OllamaUnloadModel(ep, model, nullptr, 4000);
}

bool VisionService::UsedThisSession() const { return shared_->used.load(); }

void VisionService::Shutdown(bool stopServer) {
    OllamaEndpoint ep;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        if (shared_->shutDown) return;
        shared_->shutDown = true;
        shared_->stop = true;
        shared_->queue.clear();
        ep = shared_->lastEndpoint;
    }
    shared_->cancel = true;
    shared_->cv.notify_all();
    if (!shared_->used) return; // never touched Ollama: leave it exactly as it was

    // Stopping the server frees all of its GPU memory and ends any request still in
    // flight, so do that straight away instead of politely unloading first: an
    // unload queued behind a long generation can take minutes and made the app look hung.
    if (stopServer) {
        OllamaStopServer();
        return;
    }
    // Server stays up: just free the models, with short timeouts.
    std::vector<std::string> loaded;
    if (OllamaLoadedModels(ep, loaded, nullptr)) {
        for (const auto& model : loaded) OllamaUnloadModel(ep, model, nullptr, 4000);
    }
}

void VisionService::Post(const OllamaEndpoint& ep, std::function<void()> task) {
    shared_->used = true;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->lastEndpoint = ep;
        shared_->queue.push_back(std::move(task));
        shared_->snap.busy = true;
    }
    shared_->cv.notify_one();
}

void VisionService::Refresh(const OllamaEndpoint& ep) {
    auto s = shared_;
    Post(ep, [s, ep]() {
        s->Set([](Snapshot& snap) { snap.activity = "Checking Ollama"; });
        std::vector<VisionModelInfo> models;
        std::string err;
        bool running = OllamaIsRunning(ep);
        if (!running) {
            // Like a bundled backend: start it on demand (the app stops it
            // again on exit).
            s->Set([](Snapshot& snap) { snap.activity = "Starting Ollama"; });
            running = OllamaStartServer(ep, &err);
        }
        if (running) OllamaListVisionModels(ep, models, &err);
        s->Set([&](Snapshot& snap) {
            snap.checked = true;
            snap.serverRunning = running;
            snap.models = models;
            snap.lastError = err;
        });
        if (running) s->RefreshLoaded(ep);
    });
}

void VisionService::PollLoaded(const OllamaEndpoint& ep) {
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        if (shared_->snap.busy) return;
    }
    auto s = shared_;
    Post(ep, [s, ep]() { s->RefreshLoaded(ep); });
}

void VisionService::StartServer(const OllamaEndpoint& ep) {
    auto s = shared_;
    Post(ep, [s, ep]() {
        s->Set([](Snapshot& snap) { snap.activity = "Starting Ollama"; });
        std::string err;
        bool ok = OllamaStartServer(ep, &err);
        s->Set([&](Snapshot& snap) {
            snap.serverRunning = ok;
            snap.lastError = err;
        });
    });
    Refresh(ep);
}

void VisionService::Load(const OllamaEndpoint& ep, const std::string& model, const std::string& keepAlive) {
    auto s = shared_;
    Post(ep, [s, ep, model, keepAlive]() {
        s->Set([](Snapshot& snap) { snap.activity = "Loading model into GPU memory (can take a minute or two)"; });
        std::string err;
        bool ok = OllamaLoadModel(ep, model, keepAlive, &err);
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            if (ok) s->loadedByUs.insert(model);
            s->snap.lastError = err;
        }
        s->RefreshLoaded(ep);
    });
}

void VisionService::Unload(const OllamaEndpoint& ep, const std::string& model) {
    auto s = shared_;
    Post(ep, [s, ep, model]() {
        s->Set([](Snapshot& snap) { snap.activity = "Unloading model"; });
        std::string err;
        bool ok = OllamaUnloadModel(ep, model, &err);
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            if (ok) s->loadedByUs.erase(model);
            s->snap.lastError = err;
        }
        s->RefreshLoaded(ep);
    });
}

void VisionService::Describe(const OllamaEndpoint& ep, const std::string& model, const std::string& keepAlive,
                             const std::string& instruction, std::vector<Item> items, const std::wstring& autoSaveDir,
                             const DescribeOptions& options) {
    auto s = shared_;
    s->cancel = false;
    Post(ep, [s, ep, model, keepAlive, instruction, items = std::move(items), autoSaveDir, options]() {
        if (!OllamaIsRunning(ep)) {
            s->Set([](Snapshot& snap) { snap.activity = "Starting Ollama"; });
            std::string err;
            if (!OllamaStartServer(ep, &err)) {
                s->Set([&](Snapshot& snap) { snap.lastError = err; });
                return;
            }
        }
        // Whether the model takes the "think" flag (sending it to one that
        // doesn't is an error).
        bool thinks = false;
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            for (auto& m : s->snap.models)
                if (m.name == model) thinks = m.supportsThinking;
            s->snap.current = 0;
            s->snap.total = static_cast<int>(items.size());
            s->snap.lastError.clear();
        }
        std::vector<std::string> loaded;
        OllamaLoadedModels(ep, loaded, nullptr);
        bool cold = std::find(loaded.begin(), loaded.end(), model) == loaded.end();

        for (size_t i = 0; i < items.size() && !s->cancel; ++i) {
            const Item& item = items[i];
            s->Set([&](Snapshot& snap) {
                snap.current = static_cast<int>(i);
                snap.activity = cold && i == 0 ? "Loading model + describing (first image takes a minute or two)"
                                               : "Describing";
            });
            Result r;
            r.path = item.path;
            r.mode = options.mode;
            std::string hint = item.tags;
            if (hint.empty() && item.readTags) {
                try {
                    hint = ReadImageMetadata(item.path).parsed.positivePrompt;
                } catch (...) {
                }
            }
            if (OllamaDescribeImage(ep, model, thinks, keepAlive, instruction, hint, item.path, r.text, &r.error,
                                    options.numPredict, options.temperature)) {
                std::lock_guard<std::mutex> lock(s->mutex);
                s->loadedByUs.insert(model);
            }
            const std::wstring& saveInto = item.saveDir.empty() ? autoSaveDir : item.saveDir;
            if (!r.text.empty() && !saveInto.empty()) {
                std::string err;
                if (!SaveImageWithPrompt(item.path, saveInto, r.text, r.savedPath, &err, options.txtSuffix)) r.error = err;
            }
            std::lock_guard<std::mutex> lock(s->mutex);
            if (!r.error.empty()) s->snap.lastError = r.error;
            s->snap.current = static_cast<int>(i + 1);
            s->results.push_back(std::move(r));
        }
        s->RefreshLoaded(ep);
    });
}

void VisionService::Cancel() {
    shared_->cancel = true;
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->queue.clear();
}

VisionService::Snapshot VisionService::GetSnapshot() const {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    return shared_->snap;
}

void VisionService::Compose(const OllamaEndpoint& ep, const std::string& model, const std::string& keepAlive, ComposeRequest request) {
    auto s = shared_;
    s->cancel = false;
    Post(ep, [s, ep, model, keepAlive, request = std::move(request)]() {
        Result r;
        r.mode = "compose";
        {
            int len = MultiByteToWideChar(CP_UTF8, 0, request.tag.c_str(), -1, nullptr, 0);
            std::wstring w(len > 0 ? len - 1 : 0, L'\0');
            if (len > 0) MultiByteToWideChar(CP_UTF8, 0, request.tag.c_str(), -1, w.data(), len);
            r.path = w;
        }
        if (!OllamaIsRunning(ep)) {
            s->Set([](Snapshot& snap) { snap.activity = "Starting Ollama"; });
            std::string err;
            if (!OllamaStartServer(ep, &err)) {
                r.error = err;
                std::lock_guard<std::mutex> lock(s->mutex);
                s->snap.lastError = err;
                s->results.push_back(std::move(r));
                return;
            }
        }
        bool thinks = false;
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            for (auto& m : s->snap.models)
                if (m.name == model) thinks = m.supportsThinking;
            s->snap.current = 0;
            s->snap.total = 0;
            s->snap.lastError.clear();
            s->snap.activity = "Writing";
        }
        if (OllamaGenerate(ep, model, thinks, keepAlive, request.prompt, request.images, request.maxImageSide, r.text, &r.error,
                           request.numPredict, request.temperature)) {
            std::lock_guard<std::mutex> lock(s->mutex);
            s->loadedByUs.insert(model);
        }
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            if (!r.error.empty()) s->snap.lastError = r.error;
            s->results.push_back(std::move(r));
        }
        s->RefreshLoaded(ep);
    });
}

std::vector<VisionService::Result> VisionService::TakeResults() {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    std::vector<Result> out;
    out.swap(shared_->results);
    return out;
}
