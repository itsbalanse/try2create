#include "clef.h"

#include <winsock2.h>
#include <iphlpapi.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <windows.h>
#include <winhttp.h>

#include "comfy.h"
#include "json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::mutex g_mutex;
ClefSettings g_settings;
bool g_loaded = false;

std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(len > 0 ? len - 1 : 0, L'\0');
    if (len > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len);
    return out;
}

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(len > 0 ? len - 1 : 0, '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring LocalDir() {
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH * 2);
    return std::wstring(buf, n) + L"\\Try2Create";
}

fs::path SettingsFile() { return fs::path(LocalDir()) / L"clef_settings.json"; }

// The files that sit next to the program: tools\llama-cpp\llama-server.exe and a .gguf in tools\clef
fs::path ToolsDir() {
    wchar_t path[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
    return fs::path(std::wstring(path, n)).parent_path() / L"tools";
}

// The writer since 2026-10-23 (the user's pick): Ternary Bonsai 2 27B Uncensored Heretic (OS-Software; Qwen3.8-27B in ternary weights, 7.2 GB
// as PQ2_0, the faster packing on an RTX 3090) and its mmproj, in tools\bonsai. It needs Prism ML's llama.cpp fork (tools\llama-prism).
bool FindBonsaiFiles(std::wstring& model, std::wstring& mmproj) {
    std::error_code ec;
    std::wstring m, mm;
    for (fs::directory_iterator it(ToolsDir() / L"bonsai", ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != L".gguf" || fs::file_size(it->path(), ec) < 100000000ull / 2) continue; // (a download still starting)
        if (it->path().filename().wstring().rfind(L"mmproj", 0) == 0) mm = it->path().wstring();
        else m = it->path().wstring();
    }
    if (m.empty()) return false;
    model = m, mmproj = mm;
    return true;
}

// The writer model: Bonsai when it is there, else Qwen3.5 9B from Unsloth's download folder (the Hugging Face cache): the Q8 file and its mmproj
void FindWriterFiles(std::wstring& model, std::wstring& mmproj) {
    if (FindBonsaiFiles(model, mmproj)) return;
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH * 2);
    std::error_code ec;
    const fs::path snaps = fs::path(std::wstring(buf, n)) / L".cache" / L"huggingface" / L"hub" / L"models--DavidAU--Qwen3.5-9B-The-Defiant-Fable-Uncensored-Heretic-NEO-IMATRIX-MAX-MTP-GGUF" / L"snapshots";
    for (fs::directory_iterator s(snaps, ec), end; !ec && s != end; s.increment(ec)) {
        for (fs::directory_iterator it(s->path(), ec), e2; !ec && it != e2; it.increment(ec)) {
            const std::wstring name = it->path().filename().wstring();
            if (name.rfind(L"mmproj", 0) == 0 && it->path().extension() == L".gguf") mmproj = it->path().wstring();
            else if (it->path().extension() == L".gguf" && name.find(L"Q8_0") != std::wstring::npos) model = it->path().wstring();
        }
    }
}

void FindBundled(std::wstring& exe, std::wstring& model, std::wstring& mmproj) {
    wchar_t path[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
    const fs::path dir = fs::path(std::wstring(path, n)).parent_path() / L"tools";
    std::error_code ec;
    for (const wchar_t* sub : {L"llama-cpp-cuda", L"llama-cpp"}) { // the CUDA build first: Clef is instant on the GPU
        if (fs::exists(dir / sub / L"llama-server.exe", ec)) {
            exe = (dir / sub / L"llama-server.exe").wstring();
            break;
        }
    }
    for (fs::directory_iterator it(dir / L"clef", ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != L".gguf") continue;
        if (it->path().filename().wstring().rfind(L"mmproj", 0) == 0) mmproj = it->path().wstring();
        else model = it->path().wstring();
    }
}

void LoadLocked() {
    if (g_loaded) return;
    g_loaded = true;
    std::ifstream f(SettingsFile(), std::ios::binary);
    if (!f) {
        FindWriterFiles(g_settings.writerModel, g_settings.writerMmproj);
        FindBundled(g_settings.exe, g_settings.model, g_settings.mmproj);
        g_settings.autoStart = !g_settings.exe.empty() && !g_settings.model.empty(); // found next to the program: start it with Clef mode
        if (g_settings.exe.find(L"cuda") != std::wstring::npos) g_settings.gpuLayers = 99;
        return;
    }
    json j = json::parse(f, nullptr, false);
    if (!j.is_object()) return;
    g_settings.use = j.value("use", g_settings.use);
    g_settings.url = j.value("url", g_settings.url);
    g_settings.exe = ToWide(j.value("exe", std::string()));
    g_settings.model = ToWide(j.value("model", std::string()));
    g_settings.gpuLayers = std::clamp(j.value("gpuLayers", g_settings.gpuLayers), 0, 999);
    const bool gpuSaved = j.contains("gpuLayers");
    g_settings.autoStart = j.value("autoStart", g_settings.autoStart);
    g_settings.temperature = std::clamp(j.value("temperature", g_settings.temperature), 0.3, 4.0);
    if (g_settings.exe.empty() || g_settings.model.empty()) { // not set: use the ones next to the program
        std::wstring exe, model, mm;
        FindBundled(exe, model, mm);
        if (g_settings.exe.empty()) g_settings.exe = exe;
        if (g_settings.model.empty()) g_settings.model = model;
    }
    g_settings.writerUrl = j.value("writerUrl", g_settings.writerUrl);
    g_settings.writerModel = ToWide(j.value("writerModel", std::string()));
    g_settings.writerMmproj = ToWide(j.value("writerMmproj", std::string()));
    g_settings.writerGpuLayers = std::clamp(j.value("writerGpuLayers", g_settings.writerGpuLayers), 0, 999);
    g_settings.writerUseRunning = j.value("writerUseRunning", g_settings.writerUseRunning);
    if (g_settings.writerModel.empty()) FindWriterFiles(g_settings.writerModel, g_settings.writerMmproj);
    if (g_settings.writerModel.find(L"Defiant-Fable") != std::wstring::npos) { // the old default: Bonsai takes its place when it is there
        std::wstring m, mm;
        if (FindBonsaiFiles(m, mm)) g_settings.writerModel = m, g_settings.writerMmproj = mm;
    }
    g_settings.mmproj = ToWide(j.value("mmproj", std::string()));
    if (g_settings.mmproj.empty()) {
        std::wstring exe, model, mm;
        FindBundled(exe, model, mm);
        g_settings.mmproj = mm;
    }
    if (!gpuSaved && g_settings.exe.find(L"cuda") != std::wstring::npos) g_settings.gpuLayers = 99;
}

struct Endpoint {
    std::wstring host;
    INTERNET_PORT port = 80;
};

bool Parse(const std::string& url, Endpoint& ep) {
    std::wstring w = ToWide(url);
    if (w.find(L"://") == std::wstring::npos) w = L"http://" + w;
    wchar_t host[256] = {};
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.dwUrlPathLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(w.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTP) return false;
    ep.host = host;
    ep.port = uc.nPort;
    return !ep.host.empty();
}

bool Http(const Endpoint& ep, const wchar_t* method, const wchar_t* path, const std::string& body, std::string& response, DWORD& status, int timeoutMs) {
    response.clear();
    status = 0;
    HINTERNET session = WinHttpOpen(L"Try2Create", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 1500, 1500, timeoutMs, timeoutMs);
    bool ok = false;
    if (HINTERNET conn = WinHttpConnect(session, ep.host.c_str(), ep.port, 0)) {
        if (HINTERNET req = WinHttpOpenRequest(conn, method, path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0)) {
            if (!body.empty()) WinHttpAddRequestHeaders(req, L"Content-Type: application/json", static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
            const BOOL sent = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                                                 static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
            if (sent && WinHttpReceiveResponse(req, nullptr)) {
                DWORD size = sizeof(status);
                WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
                for (;;) {
                    DWORD avail = 0;
                    if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
                    std::string chunk(avail, '\0');
                    DWORD got = 0;
                    if (!WinHttpReadData(req, chunk.data(), avail, &got) || got == 0) break;
                    response.append(chunk, 0, got);
                    if (response.size() > 8u * 1024 * 1024) break;
                }
                ok = true;
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(conn);
    }
    WinHttpCloseHandle(session);
    return ok;
}

HANDLE g_process = nullptr;
HANDLE g_job = nullptr;

} // namespace

ClefSettings ClefGetSettings() {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadLocked();
    return g_settings;
}

void ClefSetSettings(const ClefSettings& s) {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadLocked();
    g_settings = s;
    json j = {{"use", s.use}, {"url", s.url}, {"exe", ToUtf8(s.exe)}, {"model", ToUtf8(s.model)}, {"mmproj", ToUtf8(s.mmproj)}, {"writerUrl", s.writerUrl}, {"writerModel", ToUtf8(s.writerModel)}, {"writerMmproj", ToUtf8(s.writerMmproj)}, {"writerGpuLayers", s.writerGpuLayers}, {"writerUseRunning", s.writerUseRunning}, {"gpuLayers", s.gpuLayers}, {"autoStart", s.autoStart}, {"temperature", s.temperature}};
    std::error_code ec;
    fs::create_directories(SettingsFile().parent_path(), ec);
    std::ofstream out(SettingsFile(), std::ios::binary | std::ios::trunc);
    if (out) out << j.dump(2, ' ', false, json::error_handler_t::replace);
}

bool ClefPing() {
    Endpoint ep;
    if (!Parse(ClefGetSettings().url, ep)) return false;
    std::string response;
    DWORD status = 0;
    return Http(ep, L"GET", L"/health", std::string(), response, status, 1500) && status == 200;
}

bool ClefWaitReady(int timeoutSeconds) {
    for (int i = 0; i < timeoutSeconds * 2; ++i) {
        if (ClefPing()) return true;
        if (!ClefOwned() && i > 6) return false; // it started but died (bad model file, no memory...)
        Sleep(500);
    }
    return false;
}

long long GpuFreeMiB() {
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = L"nvidia-smi --query-gpu=memory.free --format=csv,noheader,nounits";
    long long result = -1;
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(wr);
        wr = nullptr;
        std::string text;
        char buf[256];
        DWORD got = 0;
        const ULONGLONG t0 = GetTickCount64();
        while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0 && GetTickCount64() - t0 < 6000) text.append(buf, got);
        WaitForSingleObject(pi.hProcess, 2000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        result = text.empty() ? -1 : std::atoll(text.c_str());
        if (result == 0 && text.find('0') == std::string::npos) result = -1;
    }
    if (wr) CloseHandle(wr);
    CloseHandle(rd);
    return result;
}

void FreeOllamaModels() {
    Endpoint ep;
    if (!Parse("http://127.0.0.1:11434", ep)) return;
    std::string response;
    DWORD status = 0;
    if (!Http(ep, L"GET", L"/api/ps", std::string(), response, status, 2000) || status != 200) return;
    json j = json::parse(response, nullptr, false);
    if (!j.is_object() || !j.contains("models") || !j["models"].is_array()) return;
    for (const json& m : j["models"]) {
        const std::string name = m.value("name", std::string());
        if (name.empty()) continue;
        json body = {{"model", name}, {"keep_alive", 0}};
        std::string reply;
        Http(ep, L"POST", L"/api/generate", body.dump(), reply, status, 8000);
    }
}

bool ClefAsk(const std::string& stateJson, const std::vector<std::string>& imageDataUrls, const std::vector<ClefQuestion>& questions, std::vector<ClefAnswer>& answers, std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error) *error = m;
        return false;
    };
    const ClefSettings s = ClefGetSettings();
    if (!s.use) return fail("Clef is switched off");
    Endpoint ep;
    if (!Parse(s.url, ep)) return fail("That is not a web address");
    json qs = json::object();
    for (const ClefQuestion& q : questions) {
        json question = {{"instructions", q.instructions}};
        if (q.type == ClefType::Score) {
            question["type"] = "score";
            question["criteria"] = q.options;
        } else if (q.type == ClefType::Noul) {
            question["type"] = "noul";
        } else {
            json criteria = json::object();
            for (size_t i = 0; i < q.options.size(); ++i) criteria["o" + std::to_string(i + 1)] = q.options[i];
            question["type"] = "choice";
            question["criteria"] = criteria;
        }
        qs[q.id] = question;
    }
    json state = json::parse(stateJson, nullptr, false);
    if (state.is_discarded()) state = stateJson;
    json body = {{"model", "clef"}, {"state", state}, {"questions", qs}};
    if (!imageDataUrls.empty()) body["images"] = imageDataUrls;
    std::string response;
    DWORD status = 0;
    if (!Http(ep, L"POST", L"/v1/systemone", body.dump(-1, ' ', false, json::error_handler_t::replace), response, status, 60000)) return fail("Clef is not answering");
    if (status != 200) return fail("Clef answered with error " + std::to_string(status) + ": " + response.substr(0, 200));
    json reply = json::parse(response, nullptr, false);
    if (!reply.is_object() || !reply.contains("answers") || !reply["answers"].is_object()) return fail("Clef's answer was not what I expected");
    answers.clear();
    for (const ClefQuestion& q : questions) {
        ClefAnswer out;
        if (!reply["answers"].contains(q.id) || !reply["answers"][q.id].is_object()) return fail("Clef left out an answer");
        const json& a = reply["answers"][q.id];
        if (q.type == ClefType::Noul) {
            out.pTrue = a.value("noul", 0.0);
        } else {
            out.probs.assign(q.options.size(), 0.0);
            double sum = 0;
            if (a.contains("probabilities") && a["probabilities"].is_object()) {
                for (size_t i = 0; i < q.options.size(); ++i) {
                    const std::string key = q.type == ClefType::Score ? std::to_string(i) : "o" + std::to_string(i + 1);
                    if (a["probabilities"].contains(key) && a["probabilities"][key].is_number()) out.probs[i] = a["probabilities"][key].get<double>();
                    sum += out.probs[i];
                }
            } else if (a.contains("probabilities") && a["probabilities"].is_array()) {
                for (size_t i = 0; i < q.options.size() && i < a["probabilities"].size(); ++i) {
                    out.probs[i] = a["probabilities"][i].is_number() ? a["probabilities"][i].get<double>() : 0.0;
                    sum += out.probs[i];
                }
            }
            if (sum <= 0) return fail("Clef gave no probabilities");
            for (double& v : out.probs) v /= sum;
            if (q.type == ClefType::Score) out.score = a.value("score", 0.0);
        }
        answers.push_back(std::move(out));
    }
    ClefTouch();
    return true;
}

bool ClefDecide(const std::string& stateJson, const std::vector<ClefQuestion>& questions, std::vector<std::vector<double>>& probabilities, std::string* error) {
    std::vector<ClefAnswer> answers;
    if (!ClefAsk(stateJson, {}, questions, answers, error)) return false;
    probabilities.clear();
    for (ClefAnswer& a : answers) probabilities.push_back(std::move(a.probs));
    return true;
}

bool ClefOwned() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_process && WaitForSingleObject(g_process, 0) == WAIT_TIMEOUT;
}

bool ClefStart(std::string& message) {
    if (ClefPing()) {
        message = "Clef is already running";
        return true;
    }
    const ClefSettings s = ClefGetSettings();
    if (s.exe.empty() || GetFileAttributesW(s.exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        message = "Choose llama-server.exe first";
        return false;
    }
    if (s.model.empty() || GetFileAttributesW(s.model.c_str()) == INVALID_FILE_ATTRIBUTES) {
        message = "Choose the Clef .gguf model file first";
        return false;
    }
    Endpoint ep;
    if (!Parse(s.url, ep)) {
        message = "The address is not valid";
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_process) {
        CloseHandle(g_process);
        g_process = nullptr;
    }
    if (!g_job) {
        g_job = CreateJobObjectW(nullptr, nullptr);
        if (g_job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &info, sizeof(info));
        }
    }
    std::wstring cmd = L"\"" + s.exe + L"\" -m \"" + s.model + L"\"" + (!s.mmproj.empty() && GetFileAttributesW(s.mmproj.c_str()) != INVALID_FILE_ATTRIBUTES ? L" --mmproj \"" + s.mmproj + L"\"" : std::wstring()) + L" --host 127.0.0.1 --port " + std::to_wstring(ep.port) + L" -ngl " + std::to_wstring(s.gpuLayers) + L" -c 16384 -b 4096 -ub 4096"; // the questions of one scenario are a few thousand tokens: they have to fit one batch
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    const std::wstring logPath = LocalDir() + L"\\clef_server.log";
    std::error_code ec;
    fs::create_directories(fs::path(logPath).parent_path(), ec);
    HANDLE log = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nul;
    si.hStdOutput = log;
    si.hStdError = log;
    PROCESS_INFORMATION pi = {};
    const std::wstring dir = fs::path(s.exe).parent_path().wstring();
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | (g_job ? CREATE_SUSPENDED : 0), nullptr, dir.c_str(), &si, &pi);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        message = "Could not start llama-server";
        return false;
    }
    if (g_job) {
        AssignProcessToJobObject(g_job, pi.hProcess);
        ResumeThread(pi.hThread);
    }
    CloseHandle(pi.hThread);
    g_process = pi.hProcess;
    message = "Starting Clef (it loads the model first; log: %LOCALAPPDATA%\\Try2Create\\clef_server.log)";
    return true;
}

void ClefStopOwned() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_job) TerminateJobObject(g_job, 0);
    else if (g_process) TerminateProcess(g_process, 0);
    if (g_process) {
        WaitForSingleObject(g_process, 3000);
        CloseHandle(g_process);
        g_process = nullptr;
    }
    if (g_job) {
        CloseHandle(g_job);
        g_job = nullptr;
    }
}

// ---------------------------------------------------------------------------------------------------------------
namespace {
std::atomic<long long> g_lastUseMs{0};
long long NowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
} // namespace

void ClefTouch() { g_lastUseMs = NowMs(); }

void ClefHousekeeping() {
    static long long lastCheck = 0;
    const long long now = NowMs();
    if (now - lastCheck < 5000) return;
    lastCheck = now;
    if (ClefOwned() && g_lastUseMs.load() > 0 && now - g_lastUseMs.load() > 120000) ClefStopOwned(); // idle: the GPU goes back to ComfyUI
    WriterHousekeeping();
    EmbedHousekeeping();
}

ClefSession ClefAcquire(const std::string& comfyUrl, const std::function<void(const std::string&)>& phase) {
    ClefSession session;
    const ClefSettings cs = ClefGetSettings();
    auto say = [&](const std::string& m) {
        if (phase) phase(m);
    };
    if (!cs.use) return session;
    if (ClefPing()) {
        session.ok = true; // already running (started by hand or earlier): used as it is
        ClefTouch();
        return session;
    }
    if (cs.exe.empty() || cs.model.empty()) return session;
    if (cs.gpuLayers > 0) {
        const long long kNeed = 9500; // MiB: the model, the picture encoder and their working memory
        long long freeMiB = GpuFreeMiB();
        if (freeMiB >= 0 && freeMiB < kNeed) {
            say("Unloading ComfyUI's models to make room for Clef...");
            std::string msg;
            if (!comfyUrl.empty()) ComfyFreeMemory(comfyUrl, msg);
            Sleep(3500);
            freeMiB = GpuFreeMiB();
            if (freeMiB >= 0 && freeMiB < kNeed) {
                say("Unloading Ollama's models to make room for Clef...");
                FreeOllamaModels();
                Sleep(2000);
                freeMiB = GpuFreeMiB();
            }
            if (freeMiB >= 0 && freeMiB < kNeed && WriterOwned()) { // the prompt writer (if this app loaded it) steps aside
                say("The prompt writer steps aside for Clef...");
                WriterStopOwned();
                Sleep(1500);
            }
        }
    }
    say("Loading Clef...");
    std::string msg;
    if (ClefStart(msg) && ClefWaitReady(150)) {
        session.ok = true;
        session.started = ClefOwned();
        ClefTouch();
    } else {
        ClefStopOwned();
    }
    return session;
}

void ClefRelease(const ClefSession& session, bool immediate) {
    if (!session.started) return;
    if (immediate) ClefStopOwned();
    else ClefTouch(); // the idle timer closes it
}

// ---------------------------------------------------------------------------------------------------------------
// The prompt writer: a normal LLM (Qwen3.5 9B from Unsloth by default) that writes the scene prompt from a Vision board for text-to-image models (Krea), and, when it has its
// mmproj file, looks at a character's picture to describe them. A llama-server of its own on port 8100, or - when one with the same model is already running (Unsloth Studio) -
// that one, which costs no extra GPU memory and is never closed by this app.

namespace {
HANDLE g_wProcess = nullptr, g_wJob = nullptr;
std::atomic<long long> g_wLastUse{0};
std::atomic<int> g_wBusy{0}; // loading or answering: never "idle" (it was stopped mid-load when its last use was minutes ago)
std::atomic<bool> g_wStopWanted{false}; // ComfyUI asked for the GPU while it was busy: it stops once the answer is in
int g_externalPort = 0;
long long g_externalChecked = 0;

std::string LowerStr(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string WriterNeedle(const ClefSettings& s) {
    std::string stem = fs::path(s.writerModel).stem().string();
    if (stem.empty()) stem = "Qwen3.5-9B";
    return LowerStr(stem.substr(0, 20));
}

bool EndpointAnswers(const Endpoint& ep, const wchar_t* path, std::string* body) {
    std::string response;
    DWORD status = 0;
    if (!Http(ep, L"GET", path, std::string(), response, status, 1200) || status != 200) return false;
    if (body) *body = std::move(response);
    return true;
}

// A llama-server (not one of ours) that is already serving the writer model, found among the listening ports; 0 = none.
int FindExternalWriter(const ClefSettings& s) {
    if (!s.writerUseRunning) return 0;
    const long long now = static_cast<long long>(GetTickCount64());
    if (now - g_externalChecked < 8000) return g_externalPort;
    g_externalChecked = now;
    g_externalPort = 0;
    DWORD size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);
    std::vector<unsigned char> buf(size);
    if (GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR) return 0;
    const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buf.data());
    const DWORD mine1 = g_process ? GetProcessId(g_process) : 0, mine2 = g_wProcess ? GetProcessId(g_wProcess) : 0;
    const std::string needle = WriterNeedle(s);
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const MIB_TCPROW_OWNER_PID& row = table->table[i];
        if (row.dwOwningPid == mine1 || row.dwOwningPid == mine2) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, row.dwOwningPid);
        if (!h) continue;
        wchar_t path[MAX_PATH * 2] = {};
        DWORD n = MAX_PATH * 2;
        const bool ok = QueryFullProcessImageNameW(h, 0, path, &n) != 0;
        CloseHandle(h);
        if (!ok || LowerStr(ToUtf8(std::wstring(path, n))).find("llama-server") == std::string::npos) continue;
        Endpoint ep;
        ep.host = L"127.0.0.1";
        ep.port = static_cast<INTERNET_PORT>(ntohs(static_cast<u_short>(row.dwLocalPort)));
        std::string models;
        if (EndpointAnswers(ep, L"/v1/models", &models) && LowerStr(models).find(needle) != std::string::npos) {
            g_externalPort = ep.port;
            break;
        }
    }
    return g_externalPort;
}

// The llama-server for the writer: a ternary model (Bonsai: PQ2_0 / PTQ1_0) needs Prism ML's fork (its kernels); anything else the usual one
std::wstring WriterExe(const ClefSettings& s) {
    const std::string m = LowerStr(fs::path(s.writerModel).filename().string());
    if (m.find("bonsai") != std::string::npos || m.find("pq2_0") != std::string::npos || m.find("ptq1_0") != std::string::npos) {
        const fs::path prism = ToolsDir() / L"llama-prism" / L"llama-server.exe";
        std::error_code ec;
        if (fs::exists(prism, ec)) return prism.wstring();
    }
    return s.exe;
}

bool WriterEndpoint(const ClefSettings& s, Endpoint& ep) {
    const int ext = FindExternalWriter(s);
    if (ext > 0) {
        ep.host = L"127.0.0.1";
        ep.port = static_cast<INTERNET_PORT>(ext);
        return true;
    }
    return Parse(s.writerUrl, ep);
}
} // namespace

bool WriterOwned() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_wProcess && WaitForSingleObject(g_wProcess, 0) == WAIT_TIMEOUT;
}

void WriterStopOwned() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_wJob) TerminateJobObject(g_wJob, 0);
    else if (g_wProcess) TerminateProcess(g_wProcess, 0);
    if (g_wProcess) {
        WaitForSingleObject(g_wProcess, 3000);
        CloseHandle(g_wProcess);
        g_wProcess = nullptr;
    }
    if (g_wJob) {
        CloseHandle(g_wJob);
        g_wJob = nullptr;
    }
}

void LlmStopOwned() {
    ClefStopOwned();
    if (g_wBusy.load() > 0) g_wStopWanted = true; // it is loading or answering: it gives the GPU back right after (stopping it now only fails what it is doing)
    else WriterStopOwned();
    EmbedStopOwned();
}

bool WriterConfigured() {
    const ClefSettings s = ClefGetSettings();
    if (FindExternalWriter(s) > 0) return true;
    const std::wstring exe = WriterExe(s);
    return !exe.empty() && !s.writerModel.empty() && GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES && GetFileAttributesW(s.writerModel.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool WriterCanSee() {
    const ClefSettings s = ClefGetSettings();
    return FindExternalWriter(s) > 0 || (!s.writerMmproj.empty() && GetFileAttributesW(s.writerMmproj.c_str()) != INVALID_FILE_ATTRIBUTES);
}

std::string WriterStatusText() {
    const ClefSettings s = ClefGetSettings();
    const int ext = FindExternalWriter(s);
    if (ext > 0) return "using your running llama-server on port " + std::to_string(ext) + " (it is not touched by this app)";
    if (WriterConfigured()) return WriterOwned() ? "loaded" : "unloaded (it loads when prompts are to be written)";
    return "not set up: choose a .gguf model below";
}

static bool WriterStart(std::string& message) {
    const ClefSettings s = ClefGetSettings();
    Endpoint ep;
    if (!Parse(s.writerUrl, ep)) {
        message = "The writer address is not valid";
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_wProcess) {
        CloseHandle(g_wProcess);
        g_wProcess = nullptr;
    }
    if (!g_wJob) {
        g_wJob = CreateJobObjectW(nullptr, nullptr);
        if (g_wJob) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(g_wJob, JobObjectExtendedLimitInformation, &info, sizeof(info));
        }
    }
    const std::wstring exe = WriterExe(s);
    std::wstring cmd = L"\"" + exe + L"\" -m \"" + s.writerModel + L"\"";
    if (!s.writerMmproj.empty() && GetFileAttributesW(s.writerMmproj.c_str()) != INVALID_FILE_ATTRIBUTES) cmd += L" --mmproj \"" + s.writerMmproj + L"\"";
    cmd += L" --host 127.0.0.1 --port " + std::to_wstring(ep.port) + L" -ngl " + std::to_wstring(s.writerGpuLayers) + L" -c 8192 --jinja --reasoning off";
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    const std::wstring logPath = LocalDir() + L"\\writer_server.log";
    std::error_code ec;
    fs::create_directories(fs::path(logPath).parent_path(), ec);
    HANDLE log = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nul;
    si.hStdOutput = log;
    si.hStdError = log;
    PROCESS_INFORMATION pi = {};
    const std::wstring dir = fs::path(exe).parent_path().wstring();
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | (g_wJob ? CREATE_SUSPENDED : 0), nullptr, dir.c_str(), &si, &pi);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        message = "Could not start llama-server for the prompt writer";
        return false;
    }
    if (g_wJob) {
        AssignProcessToJobObject(g_wJob, pi.hProcess);
        ResumeThread(pi.hThread);
    }
    CloseHandle(pi.hThread);
    g_wProcess = pi.hProcess;
    message = "Starting the prompt writer";
    return true;
}

void WriterTouch() { g_wLastUse = static_cast<long long>(GetTickCount64()); }

ClefSession WriterAcquire(const std::string& comfyUrl, const std::function<void(const std::string&)>& phase) {
    ClefSession session;
    const ClefSettings s = ClefGetSettings();
    auto say = [&](const std::string& m) {
        if (phase) phase(m);
    };
    Endpoint ep;
    if (FindExternalWriter(s) > 0) { // your own running server: nothing to load
        session.ok = true;
        return session;
    }
    if (Parse(s.writerUrl, ep) && EndpointAnswers(ep, L"/health", nullptr)) {
        session.ok = true;
        WriterTouch();
        return session;
    }
    if (!WriterConfigured()) return session;
    if (s.writerGpuLayers > 0) {
        std::error_code fec; // MiB: the model, its picture encoder and the context (13.5 GB for the Q8 9B; about 10 GB for Bonsai)
        const long long files = static_cast<long long>((fs::file_size(s.writerModel, fec) + (s.writerMmproj.empty() ? 0 : fs::file_size(s.writerMmproj, fec))) >> 20);
        const long long kNeed = fec || files <= 0 ? 13500 : files + 2500;
        long long freeMiB = GpuFreeMiB();
        if (freeMiB >= 0 && freeMiB < kNeed) {
            say("Unloading ComfyUI's models to make room for the prompt writer...");
            std::string msg;
            if (!comfyUrl.empty()) ComfyFreeMemory(comfyUrl, msg);
            Sleep(3500);
            freeMiB = GpuFreeMiB();
            if (freeMiB >= 0 && freeMiB < kNeed) {
                say("Unloading Ollama's models to make room for the prompt writer...");
                FreeOllamaModels();
                Sleep(2000);
                freeMiB = GpuFreeMiB();
            }
            if (freeMiB >= 0 && freeMiB < kNeed) { // still not enough: Clef steps aside (he is quick to load again)
                say("Clef steps aside for the prompt writer...");
                ClefStopOwned();
                Sleep(1500);
            }
        }
    }
    say("Loading the prompt writer...");
    std::string msg;
    ++g_wBusy;
    WriterTouch();
    if (WriterStart(msg)) {
        for (int i = 0; i < 300; ++i) { // up to 150 s
            if (EndpointAnswers(ep, L"/health", nullptr)) {
                session.ok = true;
                session.started = WriterOwned();
                WriterTouch();
                break;
            }
            if (!WriterOwned() && i > 6) break;
            Sleep(500);
        }
    }
    WriterTouch();
    --g_wBusy;
    if (!session.ok) WriterStopOwned();
    return session;
}

void WriterRelease(const ClefSession& session, bool immediate) {
    if (!session.started) return;
    if (immediate) WriterStopOwned();
    else WriterTouch();
}

static bool WriterChatWith(const std::string& system, const std::string& user, const std::vector<std::string>& imageDataUrls, int maxTokens, double temperature,
                           std::string& out, std::string* error, const std::string& schema);

bool WriterChat(const std::string& system, const std::string& user, const std::vector<std::string>& imageDataUrls, int maxTokens, double temperature, std::string& out, std::string* error) {
    return WriterChatWith(system, user, imageDataUrls, maxTokens, temperature, out, error, std::string());
}

bool WriterChatJson(const std::string& system, const std::string& user, const std::vector<std::string>& imageDataUrls, const std::string& schema, int maxTokens,
                    double temperature, std::string& out, std::string* error) {
    return WriterChatWith(system, user, imageDataUrls, maxTokens, temperature, out, error, schema);
}

static bool WriterChatWith(const std::string& system, const std::string& user, const std::vector<std::string>& imageDataUrls, int maxTokens, double temperature,
                           std::string& out, std::string* error, const std::string& schema) {
    auto fail = [&](const std::string& m) {
        if (error) *error = m;
        return false;
    };
    const ClefSettings s = ClefGetSettings();
    Endpoint ep;
    if (!WriterEndpoint(s, ep)) return fail("The writer address is not valid");
    json content;
    if (imageDataUrls.empty()) {
        content = user;
    } else {
        content = json::array();
        content.push_back({{"type", "text"}, {"text", user}});
        for (const std::string& u : imageDataUrls) content.push_back({{"type", "image_url"}, {"image_url", {{"url", u}}}});
    }
    json body = {{"model", "writer"}, {"messages", json::array({{{"role", "system"}, {"content", system}}, {{"role", "user"}, {"content", content}}})},
                 {"max_tokens", maxTokens}, {"temperature", temperature}, {"stream", false}, {"chat_template_kwargs", {{"enable_thinking", false}}}};
    if (!schema.empty()) {
        const json sj = json::parse(schema, nullptr, false);
        if (sj.is_object()) body["response_format"] = {{"type", "json_schema"}, {"json_schema", {{"name", "answer"}, {"schema", sj}}}};
    }
    std::string response;
    DWORD status = 0;
    WriterTouch();
    ++g_wBusy;
    const bool sent = Http(ep, L"POST", L"/v1/chat/completions", body.dump(-1, ' ', false, json::error_handler_t::replace), response, status, 240000);
    if (--g_wBusy == 0 && g_wStopWanted.exchange(false)) WriterStopOwned();
    WriterTouch();
    if (!sent) return fail("The prompt writer is not answering");
    if (status != 200) return fail("The prompt writer answered with error " + std::to_string(status));
    json reply = json::parse(response, nullptr, false);
    if (!reply.is_object() || !reply.contains("choices") || !reply["choices"].is_array() || reply["choices"].empty()) return fail("The prompt writer's answer was not what I expected");
    out = reply["choices"][0].value("message", json::object()).value("content", std::string());
    const size_t closeThink = out.find("</think>"); // a model that thinks anyway: only what comes after
    if (closeThink != std::string::npos) out = out.substr(closeThink + 8);
    if (schema.empty()) { // (JSON keeps its quotes)
        while (!out.empty() && (out.front() == ' ' || out.front() == '\n' || out.front() == '\r' || out.front() == '"')) out.erase(out.begin());
        while (!out.empty() && (out.back() == ' ' || out.back() == '\n' || out.back() == '\r' || out.back() == '"')) out.pop_back();
    }
    WriterTouch();
    return !out.empty() ? true : fail("The prompt writer wrote nothing");
}

void WriterHousekeeping() {
    static long long lastCheck = 0;
    const long long now = static_cast<long long>(GetTickCount64());
    if (now - lastCheck < 5000) return;
    lastCheck = now;
    if (g_wBusy.load() == 0 && WriterOwned() && g_wLastUse.load() > 0 && now - g_wLastUse.load() > 120000) WriterStopOwned(); // idle: the GPU goes back to ComfyUI
}

// ---------------------------------------------------------------------------------------------------------------
// The embedding model (EmbeddingGemma-2): turns text and pictures into vectors of one shared space, so a sentence can be compared with a picture. A llama-server of its own on
// port 8101 (about 3 GB of GPU memory), loaded when something needs it and closed when ComfyUI needs the GPU or after two idle minutes.
namespace {
HANDLE g_eProcess = nullptr, g_eJob = nullptr;
std::atomic<long long> g_eLastUse{0};

void FindEmbedFiles(std::wstring& model, std::wstring& mmproj) {
    wchar_t path[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
    const fs::path dir = fs::path(std::wstring(path, n)).parent_path() / L"tools" / L"embed";
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != L".gguf") continue;
        if (it->path().filename().wstring().rfind(L"mmproj", 0) == 0) mmproj = it->path().wstring();
        else model = it->path().wstring();
    }
}

bool EmbedEndpoint(Endpoint& ep) {
    ep.host = L"127.0.0.1";
    ep.port = 8101;
    return true;
}
} // namespace

bool EmbedOwned() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_eProcess && WaitForSingleObject(g_eProcess, 0) == WAIT_TIMEOUT;
}

void EmbedStopOwned() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_eJob) TerminateJobObject(g_eJob, 0);
    else if (g_eProcess) TerminateProcess(g_eProcess, 0);
    if (g_eProcess) {
        WaitForSingleObject(g_eProcess, 3000);
        CloseHandle(g_eProcess);
        g_eProcess = nullptr;
    }
    if (g_eJob) {
        CloseHandle(g_eJob);
        g_eJob = nullptr;
    }
}

bool EmbedAvailable() {
    std::wstring model, mmproj;
    FindEmbedFiles(model, mmproj);
    const ClefSettings s = ClefGetSettings();
    return !model.empty() && !s.exe.empty() && GetFileAttributesW(s.exe.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void EmbedTouch() { g_eLastUse = static_cast<long long>(GetTickCount64()); }

void EmbedHousekeeping() {
    if (EmbedOwned() && g_eLastUse.load() > 0 && static_cast<long long>(GetTickCount64()) - g_eLastUse.load() > 120000) EmbedStopOwned(); // idle: the GPU goes back to ComfyUI
}

ClefSession EmbedAcquire(const std::string& comfyUrl, const std::function<void(const std::string&)>& phase) {
    ClefSession session;
    auto say = [&](const std::string& m) {
        if (phase) phase(m);
    };
    Endpoint ep;
    EmbedEndpoint(ep);
    if (EndpointAnswers(ep, L"/health", nullptr)) {
        session.ok = true;
        EmbedTouch();
        return session;
    }
    std::wstring model, mmproj;
    FindEmbedFiles(model, mmproj);
    const ClefSettings s = ClefGetSettings();
    if (model.empty() || s.exe.empty()) return session;
    const long long kNeed = 4500; // MiB
    long long freeMiB = GpuFreeMiB();
    if (freeMiB >= 0 && freeMiB < kNeed) {
        say("Unloading ComfyUI's models to make room for the embedding model...");
        std::string msg;
        if (!comfyUrl.empty()) ComfyFreeMemory(comfyUrl, msg);
        Sleep(3500);
        freeMiB = GpuFreeMiB();
        if (freeMiB >= 0 && freeMiB < kNeed) {
            FreeOllamaModels();
            Sleep(2000);
        }
    }
    say("Loading the embedding model...");
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_eProcess) {
            CloseHandle(g_eProcess);
            g_eProcess = nullptr;
        }
        if (!g_eJob) {
            g_eJob = CreateJobObjectW(nullptr, nullptr);
            if (g_eJob) {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
                info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                SetInformationJobObject(g_eJob, JobObjectExtendedLimitInformation, &info, sizeof(info));
            }
        }
        std::wstring cmd = L"\"" + s.exe + L"\" -m \"" + model + L"\"";
        if (!mmproj.empty()) cmd += L" --mmproj \"" + mmproj + L"\"";
        cmd += L" --embeddings --host 127.0.0.1 --port 8101 -ngl 99 -c 4096 -b 4096 -ub 4096 --flash-attn on";
        SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
        const std::wstring logPath = LocalDir() + L"\\embed_server.log";
        std::error_code ec;
        fs::create_directories(fs::path(logPath).parent_path(), ec);
        HANDLE log = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        STARTUPINFOW si = {sizeof(si)};
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdInput = nul;
        si.hStdOutput = log;
        si.hStdError = log;
        PROCESS_INFORMATION pi = {};
        const std::wstring dir = fs::path(s.exe).parent_path().wstring();
        const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | (g_eJob ? CREATE_SUSPENDED : 0), nullptr, dir.c_str(), &si, &pi);
        if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        if (!ok) return session;
        if (g_eJob) {
            AssignProcessToJobObject(g_eJob, pi.hProcess);
            ResumeThread(pi.hThread);
        }
        CloseHandle(pi.hThread);
        g_eProcess = pi.hProcess;
    }
    for (int i = 0; i < 240; ++i) { // up to two minutes
        if (EndpointAnswers(ep, L"/health", nullptr)) {
            session.ok = true;
            session.started = EmbedOwned();
            EmbedTouch();
            break;
        }
        if (!EmbedOwned() && i > 6) break;
        Sleep(500);
    }
    if (!session.ok) EmbedStopOwned();
    return session;
}

void EmbedRelease(const ClefSession& session, bool immediate) {
    if (!session.started) return;
    if (immediate) EmbedStopOwned();
    else EmbedTouch();
}

namespace {
// the vectors of one request, cut to kEmbedDim (Matryoshka) and normalised
bool EmbedRequest(const json& input, size_t expected, std::vector<std::vector<float>>& out) {
    Endpoint ep;
    EmbedEndpoint(ep);
    json body = {{"model", "embed"}, {"input", input}};
    std::string response;
    DWORD status = 0;
    if (!Http(ep, L"POST", L"/v1/embeddings", body.dump(-1, ' ', false, json::error_handler_t::replace), response, status, 60000) || status != 200) return false;
    json reply = json::parse(response, nullptr, false);
    if (!reply.is_object() || !reply.contains("data") || !reply["data"].is_array() || reply["data"].size() != expected) return false;
    out.clear();
    for (const json& d : reply["data"]) {
        if (!d.contains("embedding") || !d["embedding"].is_array() || d["embedding"].size() < static_cast<size_t>(kEmbedDim)) return false;
        std::vector<float> v(kEmbedDim);
        double norm = 0;
        for (int i = 0; i < kEmbedDim; ++i) {
            v[i] = d["embedding"][i].get<float>();
            norm += static_cast<double>(v[i]) * v[i];
        }
        norm = std::sqrt(norm);
        if (norm <= 0) return false;
        for (float& f : v) f = static_cast<float>(f / norm);
        out.push_back(std::move(v));
    }
    EmbedTouch();
    return true;
}
} // namespace

bool EmbedTexts(const std::vector<std::string>& texts, std::vector<std::vector<float>>& out) {
    if (texts.empty()) return false;
    return EmbedRequest(json(texts), texts.size(), out);
}

bool EmbedImageData(const std::string& dataUrl, std::vector<float>& out) {
    json input = json::array({{{"content", json::array({{{"type", "image_url"}, {"image_url", {{"url", dataUrl}}}}})}}});
    std::vector<std::vector<float>> v;
    if (!EmbedRequest(input, 1, v)) return false;
    out = std::move(v[0]);
    return true;
}
