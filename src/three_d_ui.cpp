// The 3D page (2026-10-30): one picture becomes a 3D object you can turn around: a 3D Gaussian splat, sides it never showed included, up to a million
// Gaussians in a standard 3DGS .ply. Made by TripoSplat (VAST-AI) through ComfyUI's own nodes: 22-27 s on a 24 GB card, tested on a fox and a perfume bottle
// of mine, the graph of Comfy-Org's template written out. Or by Idol (speridlabs, Apache-2.0; tools\idol, its own Python): it reads the picture through
// two gated encoders (DINOv3, FLUX.2's VAE), so the page checks the Hugging Face login and says what is missing. 87 s on a 24 GB card (its torch.compile speed-up
// needs Triton, which has no Windows build), and on a full-body picture of mine it kept a face TripoSplat smeared.
// Each object keeps its own folder in <data>\3d: the .ply and the picture as the model saw it (cut out and framed: its thumbnail). The view is splat_view.cpp.
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app_state.h"
#include "image_attributes.h"
#include "recycle_bin.h"
#include "comfy.h"
#include "imgui.h"
#include "json.hpp"
#include "splat_view.h"
#include "ui_extra.h"
#include "ui_theme.h"
#include "win_dialogs.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::string U8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Env(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2] = {};
    return GetEnvironmentVariableW(name, buf, MAX_PATH * 2) ? std::wstring(buf) : std::wstring();
}

fs::path ExeDir() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}

// tools\idol beside the program, or two folders up from a build folder
fs::path IdolDir() {
    std::error_code ec;
    for (fs::path d = ExeDir(); !d.empty(); d = d.parent_path()) {
        if (fs::exists(d / L"tools" / L"idol" / L".venv" / L"Scripts" / L"python.exe", ec)) return d / L"tools" / L"idol";
        if (d == d.parent_path()) break;
    }
    return {};
}

fs::path ObjectsDir() { return fs::path(Env(L"LOCALAPPDATA")) / L"Try2Create" / L"3d"; }

struct Object {
    std::wstring ply, thumb, dir, source;
    std::string name, engine;
    int gaussians = 0;
    uintmax_t bytes = 0;
    fs::file_time_type at;
};

const struct {
    const char* name;
    int gaussians;
    const char* about;
} kDetail[] = {{"Standard", 262144, "262,144 Gaussians: the quickest, and what Idol is scored at"},
               {"High", 524288, "524,288 Gaussians: denser, a bigger file"},
               {"Max", 1048576, "1,048,576 Gaussians: all it can make; about 62 MB"}};

struct Page {
    std::wstring picture;
    int engine = 0; // 0 TripoSplat (ComfyUI), 1 Idol
    int activity = 0;       // TripoSplat: what ComfyUI is making (its entry in Activity())
    std::wstring activityOut; // ...and the folder its files go to
    int kind = 0;   // Idol: 0 a render or a generated picture, 1 a photo
    int detail = 0;
    int seed = 42;
    // what it needs: 0 not asked yet, 1 asking, 2 ready, 3 tools\idol is not there, 4 the Hugging Face terms are not accepted, 5 could not tell
    int need = 0;
    std::future<int> needFut;
    // the run
    std::thread run;
    std::atomic<bool> running{false}, finished{false};
    std::mutex mu;
    std::string status, error;
    std::wstring madePly;
    HANDLE process = nullptr;
    std::vector<Object> objects;
    bool scanned = false;
    int view = 0;           // 0 make, 1 the gallery of everything made
    char search[128] = {};
    int sort = 0;           // 0 newest, 1 oldest, 2 by name
    float tile = 132.0f;
    double startedAt = 0;
} g_p;

std::wstring EnvBlock(const fs::path& root) {
    std::wstring block;
    if (LPWCH all = GetEnvironmentStringsW()) {
        for (LPWCH e = all; *e; e += wcslen(e) + 1) block.append(e).push_back(L'\0');
        FreeEnvironmentStringsW(all);
    }
    const std::wstring profile = Env(L"USERPROFILE");
    for (const std::wstring& v : {L"HF_HOME=" + (root / L"hf").wstring(), L"HF_TOKEN_PATH=" + profile + L"\\.cache\\huggingface\\token", std::wstring(L"PYTHONUTF8=1"),
                                  std::wstring(L"PYTHONUNBUFFERED=1"), std::wstring(L"HF_HUB_DISABLE_SYMLINKS_WARNING=1")})
        block.append(v).push_back(L'\0');
    block.push_back(L'\0');
    return block;
}

// Runs it with no window; each line it prints goes to onLine. The exit code, or -1 if it did not start.
int RunPython(const fs::path& root, std::wstring args, HANDLE* track, const std::function<void(const std::string&)>& onLine) {
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 16)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = wr;
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = L"\"" + (root / L".venv" / L"Scripts" / L"python.exe").wstring() + L"\" " + args;
    std::wstring env = EnvBlock(root);
    const std::wstring dir = (root / L"sdk").wstring();
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, env.data(), dir.c_str(), &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        return -1;
    }
    if (track) *track = pi.hProcess;
    std::string line;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got) {
        for (DWORD i = 0; i < got; ++i) {
            if (buf[i] == '\n' || buf[i] == '\r') {
                if (!line.empty()) onLine(line), line.clear();
            } else {
                line += buf[i];
            }
        }
    }
    if (!line.empty()) onLine(line);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    if (track) *track = nullptr;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(code);
}

// Can this Hugging Face login read the two gated encoders? (their own check: nothing is downloaded)
int CheckNeeds(fs::path root) {
    if (root.empty()) return 3;
    std::string out;
    const int code = RunPython(root,
                               L"-c \"from huggingface_hub import auth_check\n"
                               L"for r in ('facebook/dinov3-vith16plus-pretrain-lvd1689m','black-forest-labs/FLUX.2-dev'): auth_check(r)\n"
                               L"print('all-ok')\"",
                               nullptr, [&](const std::string& l) { out += l + "\n"; });
    if (out.find("all-ok") != std::string::npos) return 2;
    return code >= 0 && (out.find("Gated") != std::string::npos || out.find("401") != std::string::npos || out.find("403") != std::string::npos) ? 4 : 5;
}

void Scan() {
    g_p.scanned = true;
    g_p.objects.clear();
    std::error_code ec;
    for (const auto& d : fs::directory_iterator(ObjectsDir(), fs::directory_options::skip_permission_denied, ec)) {
        if (!d.is_directory(ec)) continue;
        for (const auto& f : fs::directory_iterator(d.path(), ec)) {
            if (f.path().extension() != L".ply") continue;
            Object o;
            o.ply = f.path().wstring();
            o.dir = d.path().wstring();
            o.thumb = (d.path() / (f.path().stem().wstring() + L"_input.png")).wstring();
            o.name = U8(f.path().stem().wstring());
            o.at = f.last_write_time(ec);
            o.bytes = f.file_size(ec);
            if (InBin(o.dir)) break; // (deleted: it waits on the Recycle bin page)
            std::error_code none;
            std::ifstream info(d.path() / L"info.json", std::ios::binary);
            if (info) {
                const json j = json::parse(info, nullptr, false);
                if (j.is_object()) {
                    const std::string src = j.value("source", std::string());
                    o.source = fs::u8path(src).wstring();
                    o.engine = j.value("engine", std::string());
                    o.gaussians = j.value("gaussians", 0);
                }
            }
            if (!fs::exists(o.thumb, none) && !o.source.empty()) o.thumb = o.source; // (no cut-out kept: the picture it was made from)
            g_p.objects.push_back(std::move(o));
            break;
        }
    }
    std::sort(g_p.objects.begin(), g_p.objects.end(), [](const Object& a, const Object& b) { return a.at > b.at; });
}

// TripoSplat's files in ComfyUI's models folder; the ones that are not there
std::vector<std::string> TripoMissing() {
    static std::vector<std::string> missing;
    static double at = -1000.0;
    if (ImGui::GetTime() - at < 3.0) return missing;
    at = ImGui::GetTime();
    missing.clear();
    const std::wstring models = ComfyModelsDir();
    if (models.empty()) return missing; // (not known: ComfyUI will say)
    std::error_code ec;
    for (const auto& [dir, file] : {std::pair<const wchar_t*, const wchar_t*>{L"diffusion_models", L"triposplat_fp16.safetensors"}, {L"vae", L"triposplat_vae_decoder_fp16.safetensors"},
                                    {L"vae", L"flux2-vae.safetensors"}, {L"clip_vision", L"dino_v3_vit_h.safetensors"}, {L"background_removal", L"birefnet.safetensors"}})
        if (!fs::exists(fs::path(models) / dir / file, ec)) missing.push_back(U8(file));
    return missing;
}

// Comfy-Org's template written out: the background taken off, the object framed at 1024, 20 steps of cfg 3, then the Gaussians
void MakeTripo(AppState& app) {
    json g;
    auto node = [&](const char* id, const char* cls, json inputs) { g[id] = {{"class_type", cls}, {"inputs", std::move(inputs)}}; };
    auto L = [](const char* id, int k = 0) { return json::array({id, k}); };
    // (each its own name: the same picture again would otherwise be skipped by ComfyUI, which keeps what it has already saved, and no cut-out would come back)
    const std::string prefix = "3D/try2create_" + std::to_string(GetTickCount64());
    node("1", "LoadImage", {{"image", "@picture"}});
    node("2", "LoadBackgroundRemovalModel", {{"bg_removal_name", "birefnet.safetensors"}});
    node("3", "RemoveBackground", {{"bg_removal_model", L("2")}, {"image", L("1")}});
    node("4", "TripoSplatPreprocessImage", {{"image", L("1")}, {"mask", L("3")}, {"erode_radius", 1}, {"size", 1024}});
    node("5", "CLIPVisionLoader", {{"clip_name", "dino_v3_vit_h.safetensors"}});
    node("6", "VAELoader", {{"vae_name", "flux2-vae.safetensors"}});
    node("7", "VAELoader", {{"vae_name", "triposplat_vae_decoder_fp16.safetensors"}});
    node("8", "UNETLoader", {{"unet_name", "triposplat_fp16.safetensors"}, {"weight_dtype", "default"}});
    node("9", "TripoSplatConditioning", {{"clip_vision", L("5")}, {"vae", L("6")}, {"image", L("4")}});
    node("10", "KSampler", {{"model", L("8")}, {"positive", L("9", 0)}, {"negative", L("9", 1)}, {"latent_image", L("9", 2)}, {"seed", 1}, {"steps", 20}, {"cfg", 3.0},
                            {"sampler_name", "dpmpp_2m"}, {"scheduler", "simple"}, {"denoise", 1.0}});
    node("11", "VAEDecodeTripoSplat", {{"samples", L("10")}, {"vae", L("7")}, {"num_gaussians", kDetail[g_p.detail].gaussians}, {"seed", 1}});
    node("12", "SplatToFile3D", {{"splat", L("11")}, {"format", "ply"}});
    node("13", "SaveGLB", {{"mesh", L("12")}, {"filename_prefix", prefix}}); // (with a splat it writes the file as it is)
    node("14", "SaveImage", {{"images", L("4")}, {"filename_prefix", prefix + "_input"}});
    ComfyJob job;
    job.apiWorkflow = g.dump();
    job.rawUploads = {{"@picture", g_p.picture}};
    job.label = "3D: " + U8(fs::path(g_p.picture).filename().wstring());
    job.times = 1;
    job.quiet = true;
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stamp[32];
    swprintf(stamp, 32, L"%04d%02d%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    g_p.activityOut = (ObjectsDir() / (fs::path(g_p.picture).stem().wstring() + L"_" + stamp)).wstring();
    g_p.startedAt = ImGui::GetTime();
    g_p.finished = false;
    {
        std::lock_guard<std::mutex> lock(g_p.mu);
        g_p.status = "Sending it to ComfyUI...", g_p.error.clear(), g_p.madePly.clear();
    }
    g_p.activity = SendComfyJob(app, std::move(job));
    g_p.running = g_p.activity != 0;
}

// What ComfyUI is doing with it; when it is done its files move to the object's own folder
void PumpTripo(AppState& app) {
    if (!g_p.activity || !app.comfySvc) return;
    for (const ComfyActivity& a : app.comfySvc->Activity()) {
        if (a.id != g_p.activity) continue;
        std::lock_guard<std::mutex> lock(g_p.mu);
        if (a.state == ComfyState::Failed) {
            g_p.error = a.message, g_p.status.clear();
        } else if (a.state != ComfyState::Done) {
            g_p.status = a.message;
            return;
        } else {
            const fs::path out = g_p.activityOut, stem = fs::path(g_p.picture).stem();
            std::error_code ec;
            fs::create_directories(out, ec);
            auto take = [&](const std::wstring& from, const fs::path& to) {
                std::error_code e;
                fs::rename(from, to, e);
                if (e) { // (another drive)
                    e.clear();
                    fs::copy_file(from, to, fs::copy_options::overwrite_existing, e);
                    if (!e) fs::remove(from, e);
                }
                return !e;
            };
            std::wstring ply;
            for (const ComfyResult& r : a.files) {
                if (r.localPath.empty()) continue;
                if (r.model3d) {
                    const fs::path to = out / (stem.wstring() + L".ply");
                    if (take(r.localPath, to)) ply = to.wstring();
                } else {
                    take(r.localPath, out / (stem.wstring() + L"_input.png"));
                }
            }
            g_p.status.clear();
            if (!ply.empty()) {
                g_p.madePly = ply;
                std::ofstream(out / L"info.json", std::ios::binary) << json({{"source", U8(g_p.picture)}, {"engine", "TripoSplat"}, {"gaussians", kDetail[g_p.detail].gaussians}}).dump(1);
            } else {
                g_p.error = "ComfyUI finished, but its 3D file could not be found";
            }
        }
        g_p.activity = 0;
        g_p.running = false, g_p.finished = true;
        return;
    }
}

void Make(AppState& app) {
    if (g_p.running || g_p.picture.empty()) return;
    if (g_p.engine == 0) {
        MakeTripo(app);
        return;
    }
    if (g_p.run.joinable()) g_p.run.join();
    const fs::path root = IdolDir();
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stamp[32];
    swprintf(stamp, 32, L"%04d%02d%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    const fs::path out = ObjectsDir() / (fs::path(g_p.picture).stem().wstring() + L"_" + stamp);
    std::error_code ec;
    fs::create_directories(out, ec);
    const std::wstring args = L"-m idol.sample --image \"" + g_p.picture + L"\" --out \"" + out.wstring() + L"\" --mode " + (g_p.kind ? L"real" : L"synthetic") + L" --seed " +
                              std::to_wstring(g_p.seed) + L" --no_optimize --num_gaussians " + std::to_wstring(kDetail[g_p.detail].gaussians); // (its speed-up needs Triton, which has no Windows build)
    const std::string url = app.comfy.url;
    g_p.running = true, g_p.finished = false;
    g_p.startedAt = ImGui::GetTime();
    {
        std::lock_guard<std::mutex> lock(g_p.mu);
        g_p.status = "Starting Idol...", g_p.error.clear(), g_p.madePly.clear();
    }
    g_p.run = std::thread([root, args, out, url, picture = g_p.picture, gaussians = kDetail[g_p.detail].gaussians] {
        std::string msg, last, wrong;
        ComfyFreeMemory(url, msg); // (ComfyUI gives its memory back: Idol needs 7.2 GB)
        const int code = RunPython(root, args, &g_p.process, [&](const std::string& l) {
            last = l;
            if (l.rfind("error:", 0) == 0 || l.find("Error") != std::string::npos) wrong = l;
            std::lock_guard<std::mutex> lock(g_p.mu);
            g_p.status = l.size() > 110 ? l.substr(0, 110) + "..." : l;
        });
        std::wstring ply;
        std::error_code e2;
        for (const auto& f : fs::directory_iterator(out, e2))
            if (f.path().extension() == L".ply") ply = f.path().wstring();
        std::lock_guard<std::mutex> lock(g_p.mu);
        if (code == 0 && !ply.empty()) {
            g_p.madePly = ply;
            g_p.status.clear();
            std::ofstream(out / L"info.json", std::ios::binary) << json({{"source", U8(picture)}, {"engine", "Idol"}, {"gaussians", gaussians}}).dump(1);
        } else {
            g_p.error = code == -1 ? "Idol could not be started" : wrong.empty() ? (last.empty() ? "Idol stopped" : last) : wrong;
            g_p.status.clear();
            fs::remove_all(out, e2); // (an empty try)
        }
        g_p.running = false, g_p.finished = true;
    });
}

bool Chip(const char* label, bool on) {
    ImGui::PushStyleColor(ImGuiCol_Button, on ? ui::ChipActiveColor() : ui::ChipColor());
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::ChipHoverColor());
    const bool c = ImGui::Button(label);
    ImGui::PopStyleColor(2);
    return c;
}

std::wstring PickPly() {
    wchar_t buf[MAX_PATH * 2] = {};
    OPENFILENAMEW ofn = {sizeof(ofn)};
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = L"3D Gaussian splats\0*.ply\0All files\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.lpstrTitle = L"A 3D Gaussian splat";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&ofn) ? std::wstring(buf) : std::wstring();
}

void DrawNeeds() {
    if (g_p.need == 3) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(ui::kWarn, "Idol is a separate install for people comfortable with Python: its code and a Python with PyTorch in a tools\\idol folder next to this program. TripoSplat needs none of that.");
        ImGui::PopTextWrapPos();
        if (ImGui::Button("Idol's page")) ShellExecuteW(nullptr, L"open", L"https://huggingface.co/speridlabs/idol", nullptr, nullptr, SW_SHOWNORMAL);
    } else if (g_p.need == 4) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(ui::kWarn, "One step only you can do: Idol reads pictures through two gated models. Sign in to Hugging Face, press Agree on both pages, then Check again.");
        ImGui::PopTextWrapPos();
        if (ImGui::Button("DINOv3 page")) ShellExecuteW(nullptr, L"open", L"https://huggingface.co/facebook/dinov3-vith16plus-pretrain-lvd1689m", nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::SameLine();
        if (ImGui::Button("FLUX.2 page")) ShellExecuteW(nullptr, L"open", L"https://huggingface.co/black-forest-labs/FLUX.2-dev", nullptr, nullptr, SW_SHOWNORMAL);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Only its VAE is downloaded (about 300 MB)");
        ImGui::SameLine();
    } else if (g_p.need == 5) {
        ImGui::TextColored(ui::kWarn, "Could not reach Hugging Face to check Idol's models.");
        ImGui::SameLine();
    }
    if (g_p.need == 4 || g_p.need == 5) {
        if (ImGui::Button("Check again")) g_p.need = 0;
    }
}

} // namespace

bool ThreeDAnimating() { return splat::Spinning() || splat::Reading() || g_p.running; }

void ThreeDRescan() { g_p.scanned = false; }

// A picture for the page (dropped on it, or sent from the gallery): nothing is made until Make
void ThreeDTakePicture(const std::wstring& path) { g_p.picture = path; }

void OpenThreeD(AppState& app, const std::wstring& picture) {
    g_p.picture = picture;
    app.requestTab = 14;
}

namespace {

void DeleteObject(AppState& app, const Object& o) {
    if (_wcsicmp(splat::Path().c_str(), o.ply.c_str()) == 0) splat::Close();
    BinItem item;
    item.name = o.name;
    item.kind = 2;
    item.files = {o.dir};
    item.thumb = o.thumb;
    BinAdd(app, std::move(item));
    g_p.scanned = false;
}

// The tiles of what was made: a click opens it in the view, the x sends it to the Recycle bin page, a right click has the rest. rows: how many rows at most (0: all);
// when the gallery asks: only those matching its search, in its order.
void DrawObjects(AppState& app, float tile, int rows) {
    std::vector<const Object*> list;
    std::string want = g_p.search;
    for (char& c : want) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const Object& o : g_p.objects) {
        std::string low = o.name;
        for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (rows || want.empty() || low.find(want) != std::string::npos) list.push_back(&o);
    }
    if (!rows && g_p.sort == 1) std::reverse(list.begin(), list.end());
    if (!rows && g_p.sort == 2) std::sort(list.begin(), list.end(), [](const Object* a, const Object* b) { return a->name < b->name; });
    const float gap = ImGui::GetStyle().ItemSpacing.x, nameH = rows ? 0.0f : ImGui::GetTextLineHeightWithSpacing() * 2.0f;
    const int perRow = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + gap) / (tile + gap)));
    int col = 0, shown = 0, drop = -1;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (const Object* op : list) {
        if (rows && shown >= perRow * rows) break;
        const Object& o = *op;
        ++shown;
        if (col) ImGui::SameLine();
        col = (col + 1) % perRow;
        ImGui::PushID(o.ply.c_str());
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##obj", ImVec2(tile, tile + nameH));
        const bool hov = ImGui::IsItemHovered(), showing = _wcsicmp(splat::Path().c_str(), o.ply.c_str()) == 0;
        const float xr = ui::Dp(10.0f);
        const ImVec2 xc(p.x + tile - xr - ui::Dp(3.0f), p.y + xr + ui::Dp(3.0f)), m = ImGui::GetIO().MousePos;
        const bool onX = hov && (m.x - xc.x) * (m.x - xc.x) + (m.y - xc.y) * (m.y - xc.y) <= xr * xr;
        if (ImGui::IsItemClicked()) {
            if (onX) drop = static_cast<int>(&o - g_p.objects.data());
            else splat::Open(o.ply);
        }
        if (ImGui::BeginPopupContextItem("##objmenu")) {
            if (ImGui::MenuItem("Open it")) splat::Open(o.ply);
            if (ImGui::MenuItem("Show in Explorer")) {
                const std::wstring arg = L"/select,\"" + o.ply + L"\"";
                ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
            }
            std::error_code ec;
            ImGui::BeginDisabled(o.source.empty() || !fs::exists(o.source, ec));
            if (ImGui::MenuItem("Use its picture again")) g_p.picture = o.source, g_p.view = 0;
            ImGui::EndDisabled();
            ImGui::Separator();
            if (ImGui::MenuItem("Delete")) drop = static_cast<int>(&o - g_p.objects.data());
            ImGui::EndPopup();
        }
        dl->AddRectFilled(p, ImVec2(p.x + tile, p.y + tile), IM_COL32(255, 255, 255, 14), ui::Dp(6.0f));
        const AppState::LibThumb lt = app.LibraryThumb(o.thumb);
        if (lt.srv) dl->AddImageRounded((ImTextureID)(intptr_t)lt.srv, p, ImVec2(p.x + tile, p.y + tile), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, ui::Dp(6.0f));
        if (showing || hov) dl->AddRect(p, ImVec2(p.x + tile, p.y + tile), ImGui::GetColorU32(showing ? ui::kAccent : ui::kAccentHover), ui::Dp(6.0f), 0, ui::Dp(2.0f));
        const std::string when = FormatLocalDateTime(o.at);
        if (nameH > 0.0f) {
            std::string n = o.name;
            while (n.size() > 3 && ImGui::CalcTextSize(n.c_str()).x > tile) n.pop_back();
            dl->AddText(ImVec2(p.x + ui::Dp(2.0f), p.y + tile + ui::Dp(2.0f)), ImGui::GetColorU32(ImGuiCol_Text), n.c_str());
            dl->AddText(ImVec2(p.x + ui::Dp(2.0f), p.y + tile + ui::Dp(2.0f) + ImGui::GetTextLineHeight()), ImGui::GetColorU32(ui::kTextDim), when.c_str());
        }
        if (hov) {
            dl->AddCircleFilled(xc, xr, onX ? ui::WithAlpha(ui::kBad, 0.95f) : IM_COL32(0, 0, 0, 190));
            const float d = xr * 0.42f;
            dl->AddLine(ImVec2(xc.x - d, xc.y - d), ImVec2(xc.x + d, xc.y + d), IM_COL32_WHITE, ui::Dp(1.6f));
            dl->AddLine(ImVec2(xc.x - d, xc.y + d), ImVec2(xc.x + d, xc.y - d), IM_COL32_WHITE, ui::Dp(1.6f));
            if (onX) ImGui::SetTooltip("Delete (the Recycle bin page gets it back)");
            else ImGui::SetTooltip("%s\n%s%s%s  %.0f MB\nClick: open it   Right-click: more", o.name.c_str(), when.c_str(), o.engine.empty() ? "" : "  ", o.engine.c_str(), o.bytes / 1048576.0);
        }
        ImGui::PopID();
    }
    if (!rows && list.empty()) ImGui::TextDisabled(g_p.objects.empty() ? "What you make shows here, and stays here." : "Nothing with that name.");
    if (drop >= 0) {
        const Object o = g_p.objects[drop];
        DeleteObject(app, o);
    }
}

void DrawGallery(AppState& app) {
    ImGui::PushFont(ui::TitleFont());
    ImGui::TextUnformatted("Your 3D objects");
    ImGui::PopFont();
    ImGui::SetNextItemWidth(std::min(ui::Dp(220.0f), ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint("##3dsearch", "Search by name", g_p.search, sizeof(g_p.search));
    static const char* const kSorts[] = {"Newest", "Oldest", "By name"};
    for (int k = 0; k < 3; ++k) {
        ui::SameLineIfFits(kSorts[k]);
        if (Chip(kSorts[k], g_p.sort == k)) g_p.sort = k;
    }
    ImGui::SetNextItemWidth(std::min(ui::Dp(160.0f), ImGui::GetContentRegionAvail().x));
    ImGui::SliderFloat("##3dtile", &g_p.tile, 80.0f, 240.0f, "Tile size");
    ImGui::SameLine();
    if (ImGui::SmallButton("Open the folder")) {
        std::error_code ec;
        fs::create_directories(ObjectsDir(), ec);
        ShellExecuteW(nullptr, L"open", ObjectsDir().wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Each object is a folder here: its .ply and the picture it was made from");
    ImGui::Spacing();
    DrawObjects(app, ui::Dp(g_p.tile), 0);
}

} // namespace

void DrawThreeDTab(AppState& app, float height) {
    static bool testOnce = [] { // tests: MV_3D_PLY opens a .ply; MV_3D_PICTURE sets the picture (MV_3D_PHOTO, MV_3D_MAKE: presses Make once it can)
        const std::wstring ply = Env(L"MV_3D_PLY"), pic = Env(L"MV_3D_PICTURE");
        if (!ply.empty()) splat::Open(ply);
        if (!pic.empty()) g_p.picture = pic;
        g_p.kind = !Env(L"MV_3D_PHOTO").empty();
        g_p.engine = !Env(L"MV_3D_IDOL").empty();
        return true;
    }();
    (void)testOnce;
    static bool testMake = !Env(L"MV_3D_MAKE").empty();
    PumpTripo(app);
    if (g_p.need == 0 && g_p.engine == 1) {
        g_p.need = 1;
        g_p.needFut = std::async(std::launch::async, CheckNeeds, IdolDir());
    }
    if (g_p.need == 1 && g_p.needFut.valid() && g_p.needFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) g_p.need = g_p.needFut.get();
    if (g_p.finished.exchange(false)) {
        std::lock_guard<std::mutex> lock(g_p.mu);
        if (!g_p.madePly.empty()) {
            splat::Open(g_p.madePly);
            app.SetStatus("Made it 3D in " + std::to_string(static_cast<int>(ImGui::GetTime() - g_p.startedAt)) + " s");
        }
        g_p.scanned = false;
    }
    if (!g_p.scanned) Scan();
    const std::vector<std::string> tripoMissing = TripoMissing();
    const bool ready = g_p.engine == 0 ? tripoMissing.empty() : g_p.need == 2;
    if (testMake && ready && !g_p.picture.empty()) testMake = false, Make(app);

    if (!ImGui::BeginTable("##threed", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings, ImVec2(0, height))) return;
    ImGui::TableSetupColumn("make", ImGuiTableColumnFlags_WidthStretch, 0.36f);
    ImGui::TableSetupColumn("view", ImGuiTableColumnFlags_WidthStretch, 0.64f);
    ImGui::TableNextRow(0, height);
    ImGui::TableSetColumnIndex(0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui::Dp(12.0f), ui::Dp(10.0f)));
    ImGui::BeginChild("##3dmake", ImVec2(0, height), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    if (Chip("Make", g_p.view == 0)) g_p.view = 0;
    ImGui::SameLine(0, ui::Dp(4.0f));
    {
        char lbl[48];
        snprintf(lbl, sizeof(lbl), "Gallery (%d)###3dgallery", static_cast<int>(g_p.objects.size()));
        if (Chip(lbl, g_p.view == 1)) g_p.view = 1;
    }
    ImGui::Spacing();
    if (g_p.view == 1) {
        DrawGallery(app);
    } else {
    ImGui::PushFont(ui::TitleFont());
    ImGui::TextUnformatted("Turn a picture into 3D");
    ImGui::PopFont();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("One picture of one object, person or creature: all of it is made, the sides the picture never showed too. Works best on a clear subject.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (Chip("TripoSplat", g_p.engine == 0)) g_p.engine = 0;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Through ComfyUI, about 25 s. Faces come out smeared when they are small in the picture, and it can open facing any way");
    ImGui::SameLine(0, ui::Dp(4.0f));
    if (Chip("Idol", g_p.engine == 1)) g_p.engine = 1;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("About 90 s, and it keeps faces far better; it opens as the picture looks. Needs access to DINOv3 and FLUX.2 on Hugging Face");
    if (g_p.engine == 1) DrawNeeds();
    else if (!tripoMissing.empty()) {
        std::string names;
        for (const std::string& n : tripoMissing) names += (names.empty() ? "" : ", ") + n;
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(ui::kWarn, "Not in ComfyUI's models folder yet (from VAST-AI/TripoSplat on Hugging Face): %s", names.c_str());
        ImGui::PopTextWrapPos();
        ImGui::BeginDisabled(SetupDownloading() || ComfyModelsDir().empty());
        if (ImGui::Button("Download them (up to 3.8 GB)")) SetupDownloadPack("3D");
        ImGui::EndDisabled();
    }
    ui::SectionHeading("THE PICTURE");
    const float side = ui::Dp(150.0f);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##3dpicture", ImVec2(side, side)) && !g_p.picture.empty()) app.OpenFile(g_p.picture), app.requestTab = 0;
    if (!g_p.picture.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Click: find it in the gallery");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(at, ImVec2(at.x + side, at.y + side), IM_COL32(255, 255, 255, 12), ui::Dp(8.0f));
    if (g_p.picture.empty()) {
        const char* t = "Drop a picture here";
        const ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2(at.x + (side - ts.x) * 0.5f, at.y + (side - ts.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled), t);
    } else {
        const AppState::LibThumb lt = app.LibraryThumb(g_p.picture);
        if (lt.srv && lt.w > 0 && lt.h > 0) {
            const float s = std::min(side / lt.w, side / lt.h), iw = lt.w * s, ih = lt.h * s;
            const ImVec2 a(at.x + (side - iw) * 0.5f, at.y + (side - ih) * 0.5f);
            dl->AddImageRounded((ImTextureID)(intptr_t)lt.srv, a, ImVec2(a.x + iw, a.y + ih), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, ui::Dp(8.0f));
        }
    }
    const bool sel = app.selectedIndex >= 0 && app.selectedIndex < static_cast<int>(app.entries.size()) && !app.entries[app.selectedIndex].metadata.isVideo;
    ImGui::BeginDisabled(!sel);
    if (ImGui::SmallButton("Gallery's")) g_p.picture = app.entries[app.selectedIndex].path;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("The picture selected in the gallery");
    ImGui::SameLine();
    if (ImGui::SmallButton("File...")) {
        const std::wstring f = OpenImageFileDialog(GetActiveWindow());
        if (!f.empty()) g_p.picture = f;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Paste")) {
        std::string msg;
        const std::wstring p = PasteBoardImage(msg);
        if (!p.empty()) g_p.picture = p;
        else app.SetStatus(msg.empty() ? "No picture on the clipboard" : msg);
    }
    ImGui::Spacing();
    if (g_p.engine == 1) {
        ImGui::TextDisabled("It is");
        ImGui::SameLine();
        if (Chip("A render or a made picture", g_p.kind == 0)) g_p.kind = 0;
        ImGui::SameLine(0, ui::Dp(4.0f));
        if (Chip("A photo", g_p.kind == 1)) g_p.kind = 1;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Leaves more room around the object when it is framed");
    }
    ImGui::TextDisabled("Detail");
    for (int k = 0; k < 3; ++k) {
        ImGui::SameLine(0, ui::Dp(k ? 4.0f : 8.0f));
        if (Chip(kDetail[k].name, g_p.detail == k)) g_p.detail = k;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", kDetail[k].about);
    }
    ImGui::Spacing();
    const bool can = !g_p.running && !g_p.picture.empty() && ready;
    ImGui::BeginDisabled(!can);
    if (ui::PrimaryButton(g_p.running ? "Making it 3D..." : "Make it 3D", ImVec2(ui::Dp(190.0f), ui::Dp(38.0f)))) Make(app);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (g_p.running) {
        if (ImGui::Button("Stop")) {
            if (g_p.activity) app.comfySvc->Cancel(g_p.activity);
            else if (g_p.process) TerminateProcess(g_p.process, 1);
        }
    } else {
        ImGui::TextDisabled(g_p.engine == 1 ? (g_p.need == 1 ? "Checking..." : "about 90 s") : "about 30 s");
    }
    {
        std::lock_guard<std::mutex> lock(g_p.mu);
        ImGui::PushTextWrapPos(0);
        if (!g_p.status.empty()) ImGui::TextDisabled("%s", g_p.status.c_str());
        if (!g_p.error.empty()) ImGui::TextColored(ui::kBad, "%s", g_p.error.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::Spacing();
    if (!g_p.objects.empty()) {
        ui::SectionHeading("LATELY");
        DrawObjects(app, ui::Dp(96.0f), 1);
    }
    ImGui::Spacing();
    if (ImGui::SmallButton("Open a .ply...")) {
        const std::wstring f = PickPly();
        if (!f.empty()) splat::Open(f);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Look at any 3D Gaussian splat file");
    } // (the Make side)
    ImGui::EndChild();

    // the view
    ImGui::TableSetColumnIndex(1);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui::Dp(10.0f), ui::Dp(10.0f)));
    ImGui::BeginChild("##3dview", ImVec2(0, height), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (Chip("Turn by itself", splat::g_spin)) splat::g_spin = !splat::g_spin;
    ImGui::SameLine(0, ui::Dp(4.0f));
    if (Chip("Upside down?", !splat::g_flip)) splat::g_flip = !splat::g_flip;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Turns it over, for a file that keeps its up the other way");
    if (splat::Count() > 0) {
        ImGui::SameLine();
        ImGui::BeginDisabled(splat::Path().empty());
        if (ImGui::SmallButton("Show in Explorer")) {
            const std::wstring arg = L"/select,\"" + splat::Path() + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("%d Gaussians", splat::Count());
    }
    ImGui::SameLine();
    ImGui::TextDisabled("drag: turn   right drag: move   wheel: zoom   double click: start");
    const ImVec2 room = ImGui::GetContentRegionAvail();
    splat::Draw(app.Device(), ImVec2(room.x, std::max(ui::Dp(120.0f), room.y - ImGui::GetStyle().WindowPadding.y)));
    if (splat::Count() == 0) {
        const ImVec2 q = ImGui::GetItemRectMin();
        const char* t = splat::Reading() ? "Opening it..." : !splat::Error().empty() ? splat::Error().c_str() : "What you make turns here";
        const ImVec2 ts = ImGui::CalcTextSize(t), r = ImGui::GetItemRectSize();
        ImGui::GetWindowDrawList()->AddText(ImVec2(q.x + (r.x - ts.x) * 0.5f, q.y + (r.y - ts.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled), t);
    }
    ImGui::EndChild();
    ImGui::EndTable();
}
