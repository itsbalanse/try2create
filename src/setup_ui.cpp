// First-run setup: "Set up Try2Create" (2026-10-28: a first launch should be easy and friendly, with no file to put anywhere by hand and no
// wall of errors). A fresh PC had nothing to tell it that ComfyUI is needed, no way to
// get the models every page expects (they had to be put in by hand), no ffmpeg, and no word on what the GPU can run.
// Five steps in one window: the PC (GPU, VRAM, RAM: what will run well), ComfyUI (found, or install / pick a portable folder / an address),
// the models (packs of Comfy-Org's own files with their sizes, downloaded into ComfyUI's models folder), the helpers (ffmpeg), and what to
// try first. It opens by itself on a first start when ComfyUI or the picture models are missing, and from Help > Set up Try2Create.
// Downloads go on in the background (curl: resumes, retries) and show in the status bar; the window can be closed meanwhile.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <thread>
#include <vector>
#include <windows.h>
#include <dxgi.h>
#include <shellapi.h>

#include "app_state.h"
#include "clef.h"
#include "comfy.h"
#include "imgui.h"
#include "json.hpp"
#include "ui_extra.h"
#include "ui_theme.h"
#include "video_util.h"
#include "win_dialogs.h"

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
std::string U(const std::wstring& w) {
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

// ---------------------------------------------------------------- the PC
struct Pc {
    std::string gpu;
    double vram = 0, ram = 0;
    bool nvidia = false, known = false;
};
Pc ReadPc() {
    Pc pc;
    MEMORYSTATUSEX ms = {sizeof(ms)};
    if (GlobalMemoryStatusEx(&ms)) pc.ram = ms.ullTotalPhys / 1073741824.0;
    IDXGIFactory1* f = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f))) && f) {
        IDXGIAdapter1* a = nullptr;
        for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d = {};
            if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                const double gb = d.DedicatedVideoMemory / 1073741824.0;
                if (gb > pc.vram) pc.vram = gb, pc.gpu = U(d.Description), pc.nvidia = d.VendorId == 0x10DE; // (the biggest card: a laptop's NVIDIA, not its Intel)
            }
            a->Release();
        }
        f->Release();
    }
    pc.known = true;
    return pc;
}
double FreeGb(const std::wstring& path) {
    ULARGE_INTEGER free = {};
    std::wstring p = path;
    while (!p.empty() && GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) p = fs::path(p).parent_path().wstring(); // (a folder not made yet)
    return !p.empty() && GetDiskFreeSpaceExW(p.c_str(), &free, nullptr, nullptr) ? free.QuadPart / 1073741824.0 : -1.0;
}

// ---------------------------------------------------------------- the model packs (Comfy-Org's files; the names the pages look for)
struct File {
    const char* sub;  // models/<sub>
    const char* name;
    const char* url;
    double gb;
};
struct Pack {
    const char* name;
    const char* what;
    double vram, ram; // what it wants to run well
    int tab;          // the page it is for
    const char* licence;
    const char* page; // the model's page
    std::vector<File> files;
};
const std::vector<Pack>& Packs() {
    static const std::vector<Pack> packs = {
        {"Pictures", "Krea 2 Turbo: Generate's model for people, scenes and most styles (8 steps, seconds a picture), also Characters", 12, 32, 4,
         "the Krea 2 licence (see its page)", "https://huggingface.co/Comfy-Org/Krea-2",
         {{"diffusion_models", "krea2_turbo_int8_convrot.safetensors", "https://huggingface.co/Comfy-Org/Krea-2/resolve/main/diffusion_models/krea2_turbo_int8_convrot.safetensors", 13.49},
          {"text_encoders", "qwen3vl_4b_fp8_scaled.safetensors", "https://huggingface.co/Comfy-Org/Krea-2/resolve/main/text_encoders/qwen3vl_4b_fp8_scaled.safetensors", 5.24},
          {"vae", "qwen_image_vae.safetensors", "https://huggingface.co/Comfy-Org/Krea-2/resolve/main/vae/qwen_image_vae.safetensors", 0.25}}},
        {"Edits", "Qwen Image 2.1 Turbo: Edits (one-click apps, \"change anything\"), creatures and extreme looks on Generate", 12, 32, 6,
         "the Qwen Image licence (see its page)", "https://huggingface.co/Comfy-Org/Qwen-Image-2.1",
         {{"diffusion_models", "qwen_image_2.1_turbo_int8_convrot.safetensors", "https://huggingface.co/Comfy-Org/Qwen-Image-2.1/resolve/main/diffusion_models/qwen_image_2.1_turbo_int8_convrot.safetensors", 7.26},
          {"text_encoders", "qwen3vl_8b_int8_convrot.safetensors", "https://huggingface.co/Comfy-Org/Qwen-Image-2.1/resolve/main/text_encoders/qwen3vl_8b_int8_convrot.safetensors", 9.35},
          {"vae", "qwen_image_2.1_vae_bf16.safetensors", "https://huggingface.co/Comfy-Org/Qwen-Image-2.1/resolve/main/vae/qwen_image_2.1_vae_bf16.safetensors", 0.68}}},
        {"Designs & exact text", "MOSS-Image: posters, covers and thumbnails with the words you typed, box edits, the picture upscaler", 12, 32, 6,
         "the MOSS-Image licence (see its page)", "https://huggingface.co/Comfy-Org/MOSS-Image",
         {{"diffusion_models", "moss_image_sft_int8_convrot.safetensors", "https://huggingface.co/Comfy-Org/MOSS-Image/resolve/main/diffusion_models/moss_image_sft_int8_convrot.safetensors", 5.41},
          {"text_encoders", "qwen3.5_9b_w4a8_gptq.safetensors", "https://huggingface.co/Comfy-Org/MOSS-Image/resolve/main/text_encoders/qwen3.5_9b_w4a8_gptq.safetensors", 7.34},
          {"vae", "flux2-vae.safetensors", "https://huggingface.co/Comfy-Org/MOSS-Image/resolve/main/vae/flux2-vae.safetensors", 0.34}}},
        {"Video with sound", "MiniMax H3: Animate a picture, long videos, Star in a video, stories (with its 8-step accelerator, the default quality)", 24, 64, 10,
         "the MiniMax H3 Community License", "https://huggingface.co/Comfy-Org/MiniMax-H3",
         {{"diffusion_models", "minimax_h3_fl2va_pruned_int8_convrot.safetensors", "https://huggingface.co/Comfy-Org/MiniMax-H3/resolve/main/diffusion_models/minimax_h3_fl2va_pruned_int8_convrot.safetensors", 20.97},
          {"text_encoders", "qwen3vl_32b_minimax_h3_nvfp4_awq.safetensors", "https://huggingface.co/Comfy-Org/MiniMax-H3/resolve/main/text_encoders/qwen3vl_32b_minimax_h3_nvfp4_awq.safetensors", 15.69},
          {"vae", "minimax_h3_video_vae_int8_convrot.safetensors", "https://huggingface.co/Comfy-Org/MiniMax-H3/resolve/main/vae/minimax_h3_video_vae_int8_convrot.safetensors", 2.81},
          {"vae", "minimax_h3_audio_vae_fp32.safetensors", "https://huggingface.co/Comfy-Org/MiniMax-H3/resolve/main/vae/minimax_h3_audio_vae_fp32.safetensors", 0.61},
          {"loras", "MiniMax-H3-FL2VA-Acc-8Step_pruned_comfy.safetensors", "https://huggingface.co/Kijai/MiniMax-H3-experimental/resolve/main/loras/MiniMax-H3-FL2VA-Acc-8Step_pruned_comfy.safetensors", 1.6}}},
        {"3D", "TripoSplat: one picture becomes a 3D object you can turn around, on the 3D page", 8, 16, 14, "the TripoSplat licence (see its page)", "https://huggingface.co/VAST-AI/TripoSplat",
         {{"diffusion_models", "triposplat_fp16.safetensors", "https://huggingface.co/VAST-AI/TripoSplat/resolve/main/diffusion_models/triposplat_fp16.safetensors", 0.74},
          {"vae", "triposplat_vae_decoder_fp16.safetensors", "https://huggingface.co/VAST-AI/TripoSplat/resolve/main/vae/triposplat_vae_decoder_fp16.safetensors", 0.58},
          {"vae", "flux2-vae.safetensors", "https://huggingface.co/VAST-AI/TripoSplat/resolve/main/vae/flux2-vae.safetensors", 0.34},
          {"clip_vision", "dino_v3_vit_h.safetensors", "https://huggingface.co/VAST-AI/TripoSplat/resolve/main/clip_vision/dino_v3_vit_h.safetensors", 1.68},
          {"background_removal", "birefnet.safetensors", "https://huggingface.co/VAST-AI/TripoSplat/resolve/main/background_removal/birefnet.safetensors", 0.44}}},
        {"Music", "MiniMax Music 3: songs with lyrics on the Sound page", 8, 16, 11, "the MiniMax Music licence (see its page)", "https://huggingface.co/Comfy-Org/MiniMax-Music-3",
         {{"vae", "minimax_music3_dav.safetensors", "https://huggingface.co/Comfy-Org/MiniMax-Music-3/resolve/main/vae/minimax_music3_dav.safetensors", 0.2},
          {"diffusion_models", "minimax_music3_dit_int8_convrot.safetensors", "https://huggingface.co/Comfy-Org/MiniMax-Music-3/resolve/main/diffusion_models/minimax_music3_dit_int8_convrot.safetensors", 2.5},
          {"text_encoders", "minimax_music3_text_encoder_pruned_int8_convrot.safetensors", "https://huggingface.co/Comfy-Org/MiniMax-Music-3/resolve/main/text_encoders/minimax_music3_text_encoder_pruned_int8_convrot.safetensors", 9.2}}},
    };
    return packs;
}

// a file is there when it is in models/<sub> (or one folder down: a LoRA sorted into a model folder) with about its size
bool FileThere(const std::wstring& models, const File& f) {
    std::error_code ec;
    const fs::path dir = fs::path(models) / W(f.sub);
    if (fs::exists(dir / W(f.name), ec)) return true;
    for (const auto& e : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
        std::error_code ec2;
        if (e.is_directory(ec2) && fs::exists(e.path() / W(f.name), ec2)) return true;
    }
    return false;
}

// ---------------------------------------------------------------- downloads (one at a time, in the background)
struct Downloads {
    std::mutex m;
    std::vector<std::pair<std::wstring, File>> queue; // models folder, file
    std::atomic<bool> running{false}, stop{false};
    std::atomic<HANDLE> proc{nullptr};
    std::string now;       // the file being fetched
    std::wstring part;     // its .part
    double nowGb = 0, doneGb = 0, totalGb = 0;
    std::vector<std::string> failed;
    std::string ffmpeg;    // "", "downloading", "ok", "error: ..."
    std::string photocraft; // the same
    std::string writer;    // the same for the prompt writer; "downloading: <what>" while it goes
    std::wstring writerPart;
};
Downloads g_dl;

// Every download runs in a job that closes with the app (a closed app had left curl downloading in the background; -C - resumes it next time)
HANDLE DownloadJob() {
    static HANDLE job = [] {
        HANDLE j = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (j) SetInformationJobObject(j, JobObjectExtendedLimitInformation, &li, sizeof(li));
        return j;
    }();
    return job;
}

bool RunCurl(const std::wstring& url, const std::wstring& dest) {
    std::wstring cmd = L"curl.exe -L --fail --retry 5 --retry-delay 5 -C - --no-progress-meter -o \"" + dest + L"\" \"" + url + L"\"";
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) return false;
    if (HANDLE job = DownloadJob()) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    g_dl.proc = pi.hProcess;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    g_dl.proc = nullptr;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

void DownloadWorker() {
    for (;;) {
        std::pair<std::wstring, File> job;
        {
            std::lock_guard<std::mutex> lock(g_dl.m);
            if (g_dl.queue.empty() || g_dl.stop) {
                g_dl.queue.clear();
                g_dl.now.clear();
                g_dl.running = false;
                return;
            }
            job = g_dl.queue.front();
            g_dl.queue.erase(g_dl.queue.begin());
            g_dl.now = job.second.name;
            g_dl.nowGb = job.second.gb;
        }
        const File& f = job.second;
        std::error_code ec;
        const fs::path dir = fs::path(job.first) / W(f.sub);
        fs::create_directories(dir, ec);
        const fs::path dest = dir / W(f.name), part = dir / (W(f.name) + L".part");
        {
            std::lock_guard<std::mutex> lock(g_dl.m);
            g_dl.part = part.wstring();
        }
        bool ok = !fs::exists(dest, ec) ? RunCurl(W(f.url), part.wstring()) : true;
        if (ok && fs::exists(part, ec)) {
            if (fs::file_size(part, ec) < static_cast<uintmax_t>(f.gb * 0.9 * 1073741824.0)) ok = false; // (an error page saved as the file)
            else fs::rename(part, dest, ec), ok = !ec;
        }
        std::lock_guard<std::mutex> lock(g_dl.m);
        g_dl.doneGb += f.gb;
        if (!ok && !g_dl.stop) g_dl.failed.push_back(std::string(f.name) + " (the download stopped: Download again resumes it)");
    }
}

void Enqueue(const std::wstring& models, const std::vector<File>& files) {
    std::lock_guard<std::mutex> lock(g_dl.m);
    if (!g_dl.running) g_dl.doneGb = 0, g_dl.totalGb = 0, g_dl.failed.clear();
    for (const File& f : files) {
        bool queued = false;
        for (const auto& q : g_dl.queue) queued = queued || (q.second.name == f.name);
        if (queued || f.name == g_dl.now) continue;
        g_dl.queue.push_back({models, f});
        g_dl.totalGb += f.gb;
    }
    if (!g_dl.running && !g_dl.queue.empty()) {
        g_dl.running = true;
        g_dl.stop = false;
        std::thread(DownloadWorker).detach();
    }
}

void StopDownloads() {
    g_dl.stop = true;
    if (HANDLE p = g_dl.proc.load()) TerminateProcess(p, 1);
}

double PartGb() {
    static double gb = 0, at = -10;
    if (ImGui::GetTime() - at > 0.5) {
        at = ImGui::GetTime();
        std::wstring part;
        {
            std::lock_guard<std::mutex> lock(g_dl.m);
            part = g_dl.part;
        }
        std::error_code ec;
        gb = !part.empty() && fs::exists(part, ec) ? fs::file_size(part, ec) / 1073741824.0 : 0.0;
    }
    return gb;
}

// ffmpeg: BtbN's GPL build (a zip from GitHub), its ffmpeg.exe and ffprobe.exe into %LOCALAPPDATA%\Try2Create\tools\ffmpeg
void DownloadFfmpeg() {
    {
        std::lock_guard<std::mutex> lock(g_dl.m);
        if (g_dl.ffmpeg == "downloading") return;
        g_dl.ffmpeg = "downloading";
    }
    std::thread([]() {
        const std::wstring dir = DataDir() + L"\\tools\\ffmpeg", tmp = DataDir() + L"\\tools\\ffmpeg_download";
        std::error_code ec;
        fs::remove_all(tmp, ec);
        fs::create_directories(tmp, ec);
        fs::create_directories(dir, ec);
        const std::wstring zip = tmp + L"\\ffmpeg.zip";
        std::string result = "ok", out;
        if (!RunCurl(L"https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip", zip)) result = "error: the download failed";
        else {
            RunCaptureHidden(L"tar.exe -xf \"" + zip + L"\" -C \"" + tmp + L"\"", out, true);
            bool copied = false;
            for (const auto& e : fs::recursive_directory_iterator(tmp, ec)) {
                const std::wstring n = e.path().filename().wstring();
                if (n == L"ffmpeg.exe" || n == L"ffprobe.exe") {
                    std::error_code ec2;
                    fs::copy_file(e.path(), fs::path(dir) / n, fs::copy_options::overwrite_existing, ec2);
                    copied = copied || (!ec2 && n == L"ffmpeg.exe");
                }
            }
            if (!copied) result = "error: the zip did not hold ffmpeg.exe";
        }
        fs::remove_all(tmp, ec);
        std::lock_guard<std::mutex> lock(g_dl.m);
        g_dl.ffmpeg = result;
    }).detach();
}

// PhotoCraft (the version the app is tested with, MIT or Apache-2.0): its portable zip into %LOCALAPPDATA%\Try2Create\tools\photocraft
void DownloadPhotocraft() {
    {
        std::lock_guard<std::mutex> lock(g_dl.m);
        if (g_dl.photocraft == "downloading") return;
        g_dl.photocraft = "downloading";
    }
    std::thread([]() {
        const std::wstring dir = DataDir() + L"\\tools\\photocraft", zip = DataDir() + L"\\tools\\photocraft.zip";
        std::error_code ec;
        fs::create_directories(dir, ec);
        std::string result = "ok", out;
        if (!RunCurl(L"https://github.com/storytold/photocraft/releases/download/v0.6.0/photocraft-0.6.0-windows-x64-portable.zip", zip)) result = "error: the download failed";
        else {
            RunCaptureHidden(L"tar.exe -xf \"" + zip + L"\" -C \"" + dir + L"\"", out, true);
            if (FindPhotocraft(L"photocraft-cli.exe").empty()) result = "error: the zip did not hold photocraft-cli.exe";
        }
        fs::remove(zip, ec);
        std::lock_guard<std::mutex> lock(g_dl.m);
        g_dl.photocraft = result;
    }).detach();
}

// The prompt writer (it turns an idea into a video prompt, looks at the pictures, plans stories): llama.cpp's own Windows build (ggml-org, MIT;
// pinned) - CUDA 12.4 with its runtime on an NVIDIA card, else the CPU one - and Qwen3-VL 8B Instruct (Qwen's own GGUF, Apache-2.0: Q4_K_M and its
// vision projector) into %LOCALAPPDATA%\Try2Create\tools\llama-cpp(-cuda) and \writer. A writer model of one's own in tools\bonsai still comes first.
const char* const kLlamaTag = "b11963";
void DownloadWriter(bool nvidia) {
    {
        std::lock_guard<std::mutex> lock(g_dl.m);
        if (g_dl.writer.rfind("downloading", 0) == 0) return;
        g_dl.writer = "downloading: llama.cpp";
    }
    std::thread([nvidia]() {
        const std::wstring tools = DataDir() + L"\\tools", tmp = tools + L"\\llama_download";
        std::error_code ec;
        auto say = [](const std::string& t, const std::wstring& part) {
            std::lock_guard<std::mutex> lock(g_dl.m);
            g_dl.writer = t;
            g_dl.writerPart = part;
        };
        std::string result = "ok", out;
        const ClefSettings cs = ClefGetSettings();
        if (cs.exe.empty() || !fs::exists(cs.exe, ec)) { // (a llama.cpp next to the program is used as it is)
            const std::wstring dest = tools + (nvidia ? L"\\llama-cpp-cuda" : L"\\llama-cpp"), tag = W(kLlamaTag);
            const std::wstring base = L"https://github.com/ggml-org/llama.cpp/releases/download/" + tag + L"/";
            std::vector<std::wstring> zips = nvidia ? std::vector<std::wstring>{L"llama-" + tag + L"-bin-win-cuda-12.4-x64.zip", L"cudart-llama-bin-win-cuda-12.4-x64.zip"}
                                                    : std::vector<std::wstring>{L"llama-" + tag + L"-bin-win-cpu-x64.zip"};
            fs::remove_all(tmp, ec);
            fs::create_directories(tmp, ec);
            fs::create_directories(dest, ec);
            for (const std::wstring& z : zips) {
                say("downloading: llama.cpp (" + U(z) + ")", tmp + L"\\" + z);
                if (!RunCurl(base + z, tmp + L"\\" + z)) {
                    result = "error: llama.cpp could not be downloaded (" + U(z) + ")";
                    break;
                }
                RunCaptureHidden(L"tar.exe -xf \"" + tmp + L"\\" + z + L"\" -C \"" + dest + L"\"", out, true);
            }
            fs::remove_all(tmp, ec);
            if (result == "ok" && !fs::exists(dest + L"\\llama-server.exe", ec)) { // (a zip that keeps it in a folder)
                for (const auto& e : fs::recursive_directory_iterator(dest, ec))
                    if (e.path().filename() == L"llama-server.exe") {
                        for (const auto& f : fs::directory_iterator(e.path().parent_path(), ec)) fs::rename(f.path(), fs::path(dest) / f.path().filename(), ec);
                        break;
                    }
                if (!fs::exists(dest + L"\\llama-server.exe", ec)) result = "error: the llama.cpp zip did not hold llama-server.exe";
            }
        }
        std::wstring model, mm;
        if (result == "ok" && (cs.writerModel.empty() || !fs::exists(cs.writerModel, ec))) {
            const std::wstring dir = tools + L"\\writer";
            fs::create_directories(dir, ec);
            const std::pair<const wchar_t*, double> files[] = {{L"Qwen3VL-8B-Instruct-Q4_K_M.gguf", 5.03}, {L"mmproj-Qwen3VL-8B-Instruct-Q8_0.gguf", 0.75}};
            for (const auto& [f, gb] : files) {
                const std::wstring dest = dir + L"\\" + f, part = dest + L".part";
                if (fs::exists(dest, ec)) continue;
                say("downloading: " + U(f) + " (" + std::to_string(static_cast<int>(std::lround(gb * 10)) / 10) + "." + std::to_string(static_cast<int>(std::lround(gb * 10)) % 10) + " GB)", part);
                if (!RunCurl(L"https://huggingface.co/Qwen/Qwen3-VL-8B-Instruct-GGUF/resolve/main/" + std::wstring(f), part) ||
                    fs::file_size(part, ec) < static_cast<uintmax_t>(gb * 0.9 * 1073741824.0)) {
                    result = "error: " + U(f) + " stopped downloading (Download again resumes it)";
                    break;
                }
                fs::rename(part, dest, ec);
            }
        }
        if (result == "ok") ClefFindDownloaded();
        say(result, L"");
    }).detach();
}

// ---------------------------------------------------------------- the window
struct Setup {
    bool open = false, opened = false;
    int step = 0;
    Pc pc;
    std::future<Pc> pcFut;
    std::future<std::string> checkFut; // "ok:<version>" or "error:<why>"
    std::string check;
    double checkedAt = -100;
    std::vector<bool> pick;
    bool picked = false; // the defaults were picked (they wait for the graphics card to be read)
    char url[256] = {};
    std::string note;
    bool doneSaved = false;
};
Setup g_s;

std::wstring SetupFile() { return DataDir() + L"\\setup.json"; }
bool SetupDone() {
    std::ifstream f(SetupFile());
    if (!f) return false;
    json j = json::parse(f, nullptr, false);
    return j.is_object() && j.value("done", false);
}
void SaveDone() {
    std::error_code ec;
    fs::create_directories(DataDir(), ec);
    std::ofstream(SetupFile()) << json({{"done", true}}).dump();
}

// tests: MV_UI_TEST "setup" / "setup:N" opens it (at step N); MV_SETUP_CLICK="label|label|..." presses those buttons, one a frame, in order
// (posted mouse clicks do not reach the test app)
bool Clicked(const char* label, bool pressed) {
    static std::vector<std::string> q = [] {
        std::vector<std::string> v;
        char buf[1024] = {};
        if (GetEnvironmentVariableA("MV_SETUP_CLICK", buf, sizeof(buf))) {
            std::string s = buf;
            for (size_t a = 0; a <= s.size();) {
                const size_t b = s.find('|', a);
                v.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
                if (b == std::string::npos) break;
                a = b + 1;
            }
        }
        return v;
    }();
    static int frame = -1;
    if (!pressed && !q.empty() && q.front() == label && ImGui::GetFrameCount() != frame && ImGui::GetFrameCount() > 120 && g_s.pc.known) {
        frame = ImGui::GetFrameCount();
        q.erase(q.begin());
        return true;
    }
    return pressed;
}
bool Btn(const char* label, bool primary = false, ImVec2 size = ImVec2(0, 0)) {
    return Clicked(label, primary ? ui::PrimaryButton(label, size) : ImGui::Button(label, size));
}

void Mark(int state) { // 2 good, 1 so-so, 0 bad, -1 nothing yet
    const ImVec4 c = state == 2 ? ui::kGood : state == 1 ? ui::kWarn : state == 0 ? ui::kBad : ImVec4(0.5f, 0.5f, 0.55f, 1.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float r = ImGui::GetTextLineHeight() * 0.42f;
    const ImVec2 m(p.x + r, p.y + ImGui::GetTextLineHeight() * 0.5f);
    dl->AddCircleFilled(m, r, ImGui::GetColorU32(c), 16);
    if (state == 2) {
        dl->AddLine(ImVec2(m.x - r * 0.5f, m.y), ImVec2(m.x - r * 0.1f, m.y + r * 0.4f), IM_COL32(20, 24, 30, 255), r * 0.35f);
        dl->AddLine(ImVec2(m.x - r * 0.1f, m.y + r * 0.4f), ImVec2(m.x + r * 0.55f, m.y - r * 0.4f), IM_COL32(20, 24, 30, 255), r * 0.35f);
    } else if (state == 0 || state == 1) {
        dl->AddLine(ImVec2(m.x, m.y - r * 0.5f), ImVec2(m.x, m.y + r * 0.15f), IM_COL32(20, 24, 30, 255), r * 0.3f);
        dl->AddCircleFilled(ImVec2(m.x, m.y + r * 0.5f), r * 0.17f, IM_COL32(20, 24, 30, 255));
    }
    ImGui::Dummy(ImVec2(r * 2.0f, ImGui::GetTextLineHeight()));
    ImGui::SameLine(0.0f, ui::Dp(8.0f));
}

void Wrapped(const ImVec4& col, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(buf);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

int FitState(double needVram, double needRam) {
    if (!g_s.pc.known) return -1;
    if (g_s.pc.vram >= needVram * 0.95 && g_s.pc.ram >= needRam * 0.9) return 2; // (a 24 GB card reports 23.99, a 64 GB PC about 63.8)
    if (g_s.pc.vram >= needVram * 0.66 && g_s.pc.ram >= needRam * 0.5) return 1;
    return 0;
}

// where ComfyUI is: 0 nothing, 1 found on disk (Desktop or a folder), 2 answering
int WhereComfy(AppState& app, std::string& where) {
    const std::wstring install = ComfyInstallDir();
    const bool up = app.comfySvc && app.comfySvc->Reachable();
    where = !app.comfy.comfyFolder.empty() && !install.empty() ? "your ComfyUI folder, " + U(install)
            : !install.empty()                                ? "ComfyUI Desktop, " + U(install)
            : up                                              ? "a ComfyUI at " + app.comfy.url
                                                              : std::string();
    return up ? 2 : !install.empty() ? 1 : 0;
}

void StepPc() {
    ImGui::TextUnformatted("Welcome! Try2Create puts buttons on top of ComfyUI, which runs the AI models on your graphics card. This takes a few "
                           "minutes (plus the downloads), and you can come back to it any time from Help > Set up Try2Create.");
    ImGui::Spacing();
    ui::SectionHeading("YOUR PC");
    if (!g_s.pc.known) {
        ImGui::TextDisabled("Looking at your graphics card...");
        return;
    }
    Mark(g_s.pc.nvidia ? (g_s.pc.vram >= 12 ? 2 : 1) : 0);
    ImGui::Text("%s, %.0f GB of graphics memory", g_s.pc.gpu.empty() ? "No graphics card found" : g_s.pc.gpu.c_str(), g_s.pc.vram);
    Mark(g_s.pc.ram >= 64 ? 2 : g_s.pc.ram >= 32 ? 1 : 0);
    ImGui::Text("%.0f GB of RAM", g_s.pc.ram);
    if (!g_s.pc.nvidia)
        Wrapped(ui::kWarn, "This is not an NVIDIA card. ComfyUI runs best on NVIDIA; AMD and Intel cards can work with ComfyUI's own setup for them, "
                           "but slower, and some features here may not run.");
    ImGui::Spacing();
    ui::SectionHeading("WHAT WILL RUN WELL");
    for (const Pack& p : Packs()) {
        const int st = FitState(p.vram, p.ram);
        Mark(st);
        ImGui::TextUnformatted(p.name);
        ImGui::SameLine();
        ImGui::TextDisabled(st == 2 ? "- yes" : st == 1 ? "- it should run, slower (it wants %.0f GB of graphics memory, %.0f GB of RAM)"
                                                      : "- probably too big for this PC (it wants %.0f GB of graphics memory, %.0f GB of RAM)",
                            p.vram, p.ram);
    }
    Wrapped(ui::kTextDim, "Reading the prompts and settings inside your pictures (the Gallery) needs none of this: it works right away.");
}

void StepComfy(AppState& app) {
    std::string where;
    const int st = WhereComfy(app, where);
    ImGui::TextUnformatted("ComfyUI is the free program that runs the models. Try2Create starts it in the background by itself when you make something.");
    ImGui::Spacing();
    Mark(st == 0 ? 0 : 2);
    if (st == 0) ImGui::TextUnformatted("ComfyUI was not found on this PC");
    else ImGui::Text("Found %s", where.c_str());
    if (!g_s.check.empty()) {
        const bool ok = g_s.check.rfind("ok", 0) == 0;
        Mark(ok ? 2 : 1);
        ImGui::TextUnformatted(ok ? ("It answers: ComfyUI " + g_s.check.substr(3)).c_str() : g_s.check.substr(6).c_str());
    }
    if (g_s.checkFut.valid() && g_s.checkFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) g_s.check = g_s.checkFut.get();
    if (st >= 1) {
        ImGui::BeginDisabled(g_s.checkFut.valid());
        if (Btn(g_s.checkFut.valid() ? "Starting and checking..." : "Start ComfyUI and check it")) {
            const ComfySettings s = app.comfy;
            g_s.checkFut = std::async(std::launch::async, [s]() {
                std::string msg;
                if (!ComfyCheck(s.url, msg)) { // not running: start it, then wait for it (the first start installs things: minutes)
                    if (!ComfyStart(s, msg)) return "error:Could not start it: " + msg;
                    for (int i = 0; i < 180 && !ComfyCheck(s.url, msg); ++i) Sleep(2000);
                    if (!ComfyCheck(s.url, msg)) return std::string("error:It did not answer within 6 minutes. Start ComfyUI once yourself (its first start finishes installing), then check again.");
                }
                const size_t v = msg.find("ComfyUI ");
                return "ok:" + (v == std::string::npos ? std::string("(version unknown)") : msg.substr(v + 8));
            });
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("(the first start can take a few minutes)");
    }
    ImGui::Spacing();
    ui::SectionHeading(st == 0 ? "GET COMFYUI" : "OR USE ANOTHER COMFYUI");
    ImGui::PushTextWrapPos(0.0f);
    if (st == 0) {
        ImGui::TextUnformatted("1. Install ComfyUI Desktop (recommended): download it, run the installer, open it once so it finishes setting up "
                               "(pick NVIDIA when it asks), then close it and press Look again.");
        if (Btn("Open the ComfyUI download page", true)) ShellExecuteW(nullptr, L"open", L"https://www.comfy.org/download", nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::SameLine();
        if (Btn("Look again")) g_s.check.clear(), g_s.note = ComfyInstallDir().empty() ? "Still not found. Open ComfyUI Desktop once after installing it." : "Found it.";
        ImGui::Spacing();
    }
    ImGui::TextUnformatted(st == 0 ? "2. Already have ComfyUI as the portable zip or a git clone? Pick its folder (the one with ComfyUI\\main.py, or main.py itself):"
                                   : "A portable ComfyUI or a git clone instead? Pick its folder:");
    ImGui::PopTextWrapPos();
    if (Btn("Pick the ComfyUI folder...")) {
        const std::wstring dir = OpenFolderDialog(GetActiveWindow());
        std::string why;
        if (dir.empty()) {
        } else if (ComfyFolderValid(dir, &why)) {
            app.comfy.comfyFolder = dir;
            ComfySetFolder(dir);
            SaveComfySettings(app.comfy);
            g_s.check.clear();
            g_s.note = "Using " + U(dir);
        } else {
            g_s.note = why;
        }
    }
    if (!app.comfy.comfyFolder.empty()) {
        ImGui::SameLine();
        if (Btn("Forget that folder")) app.comfy.comfyFolder.clear(), ComfySetFolder(L""), SaveComfySettings(app.comfy), g_s.note.clear();
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(st == 0 ? "3. ComfyUI running somewhere else (another port, another PC)? Its address:" : "A ComfyUI at another address:");
    ImGui::PopTextWrapPos();
    if (!g_s.url[0]) snprintf(g_s.url, sizeof(g_s.url), "%s", app.comfy.url.c_str());
    ImGui::SetNextItemWidth(ui::Dp(300.0f));
    ImGui::InputText("##comfyurl", g_s.url, sizeof(g_s.url));
    ImGui::SameLine();
    if (Btn("Connect")) {
        app.comfy.url = g_s.url;
        SaveComfySettings(app.comfy);
        const std::string url = g_s.url;
        g_s.check.clear();
        g_s.checkFut = std::async(std::launch::async, [url]() {
            std::string msg;
            if (!ComfyCheck(url, msg)) return "error:" + msg;
            const size_t v = msg.find("ComfyUI ");
            return "ok:" + (v == std::string::npos ? std::string("(version unknown)") : msg.substr(v + 8));
        });
    }
    if (!g_s.note.empty()) Wrapped(ui::kAccentHover, "%s", g_s.note.c_str());
    if (ComfyInstallDir().empty() && app.comfySvc && app.comfySvc->Reachable()) {
        Wrapped(ui::kWarn, "That ComfyUI answers, but its folder is not known, so models cannot be downloaded into it from here: pick its folder "
                           "above too, or put the files in its models folder yourself.");
    }
}

void StepModels(AppState& app) {
    const std::wstring models = ComfyModelsDir();
    const auto& packs = Packs();
    if (!g_s.picked && g_s.pc.known && !models.empty()) { // first time (once the card is known): pick what fits this PC and is not there yet
        g_s.picked = true;
        g_s.pick.assign(packs.size(), false);
        for (size_t i = 0; i < packs.size(); ++i) {
            bool all = !models.empty();
            for (const File& f : packs[i].files) all = all && FileThere(models, f);
            g_s.pick[i] = !all && FitState(packs[i].vram, packs[i].ram) == 2 && i == 0; // (Pictures only: the rest is ticked by whoever wants it)
        }
    }
    if (models.empty()) {
        Mark(0);
        Wrapped(ui::kWarn, "ComfyUI's models folder is not known yet: go back one step and find ComfyUI first.");
        return;
    }
    ImGui::TextUnformatted("Pick only what you want to make - none of it is required, and you can add packs later (Help > Set up Try2Create). "
                           "Each pack downloads the official files (Comfy-Org's) into ComfyUI's models folder:");
    ImGui::TextDisabled("%s", U(models).c_str());
    ImGui::Spacing();
    if (g_s.pick.size() != packs.size()) g_s.pick.assign(packs.size(), false);
    // the packs scroll; what is picked, the drive's room and the button stay in view under them
    const float bottom = ImGui::GetFrameHeightWithSpacing() * (g_dl.running ? 4.6f : 2.4f) + ui::Dp(10.0f);
    ImGui::BeginChild("##packs", ImVec2(0, -bottom), ImGuiChildFlags_None);
    double selected = 0;
    for (size_t i = 0; i < packs.size(); ++i) {
        const Pack& p = packs[i];
        ImGui::PushID(static_cast<int>(i));
        double have = 0, total = 0;
        int missing = 0;
        for (const File& f : p.files) {
            total += f.gb;
            if (FileThere(models, f)) have += f.gb;
            else ++missing;
        }
        const int fit = FitState(p.vram, p.ram);
        bool on = g_s.pick[i];
        ImGui::BeginGroup();
        if (missing == 0) {
            Mark(2);
            ImGui::TextUnformatted(p.name);
            ImGui::SameLine();
            ImGui::TextColored(ui::kGood, "installed");
        } else {
            if (ImGui::Checkbox(p.name, &on)) g_s.pick[i] = on;
            if (Clicked(p.name, false)) g_s.pick[i] = !g_s.pick[i];
            ImGui::SameLine();
            ImGui::TextDisabled("%.1f GB%s", total - have, have > 0 ? " more" : "");
            if (fit != 2) {
                ImGui::SameLine();
                ImGui::TextColored(fit == 1 ? ui::kWarn : ui::kBad, fit == 1 ? "may run slowly here (ComfyUI keeps part of it in RAM)" : "probably too big for this PC");
            }
            if (g_s.pick[i]) selected += total - have;
        }
        ImGui::Indent(ui::Dp(28.0f));
        Wrapped(ui::kTextDim, "%s", p.what);
        ImGui::TextDisabled("Wants %.0f GB of graphics memory and %.0f GB of RAM (this PC: %.0f and %.0f)", p.vram, p.ram, g_s.pc.vram, g_s.pc.ram);
        ImGui::TextDisabled("Licence: %s", p.licence);
        ImGui::SameLine();
        if (ImGui::SmallButton("Model page")) ShellExecuteW(nullptr, L"open", W(p.page).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::Unindent(ui::Dp(28.0f));
        ImGui::EndGroup();
        ImGui::Spacing();
        ImGui::PopID();
    }
    ImGui::EndChild();
    const double free = FreeGb(models);
    ImGui::Separator();
    if (free >= 0) {
        ImGui::Text("Picked: %.1f GB", selected);
        ImGui::SameLine();
        ImGui::TextColored(free < selected + 5 ? ui::kBad : ui::kTextDim, "(%.0f GB free on that drive)", free);
    }
    const bool running = g_dl.running;
    ImGui::BeginDisabled(selected <= 0 || (free >= 0 && free < selected + 2));
    if (Btn(running ? "Add the picked ones to the downloads" : "Download the picked ones", true, ImVec2(0, ui::Dp(30.0f)))) {
        std::vector<File> files;
        for (size_t i = 0; i < packs.size(); ++i)
            if (g_s.pick[i])
                for (const File& f : packs[i].files)
                    if (!FileThere(models, f)) files.push_back(f);
        Enqueue(models, files);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (Btn("Skip for now")) ++g_s.step;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Nothing is downloaded; the pages say what they need when you open them");
    if (running) {
        std::string now;
        double done = 0, total = 0, nowGb = 0;
        {
            std::lock_guard<std::mutex> lock(g_dl.m);
            now = g_dl.now, done = g_dl.doneGb, total = g_dl.totalGb, nowGb = g_dl.nowGb;
        }
        const double part = std::min(PartGb(), nowGb);
        ImGui::ProgressBar(total > 0 ? static_cast<float>((done + part) / total) : 0.0f, ImVec2(-1, 0));
        ImGui::TextDisabled("%s: %.1f of %.1f GB (all: %.1f of %.1f GB). You can close this window: it goes on in the background.", now.c_str(), part, nowGb, done + part, total);
        if (Btn("Stop the downloads")) StopDownloads();
    }
    {
        std::lock_guard<std::mutex> lock(g_dl.m);
        for (const std::string& f : g_dl.failed) Wrapped(ui::kBad, "%s", f.c_str());
    }
    (void)app;
}

void StepHelpers() {
    ImGui::TextUnformatted("Small programs the app uses next to ComfyUI. All optional: skip them and get them later from here when a page asks.");
    ImGui::Spacing();
    std::string st;
    {
        std::lock_guard<std::mutex> lock(g_dl.m);
        st = g_dl.ffmpeg;
    }
    const std::wstring ff = FfmpegPath();
    Mark(!ff.empty() ? 2 : st == "downloading" ? -1 : 1);
    ImGui::TextUnformatted("ffmpeg");
    ImGui::SameLine();
    if (!ff.empty()) ImGui::TextColored(ui::kGood, "found (%s)", U(ff).c_str());
    else if (st == "downloading") ImGui::TextColored(ui::kAccentHover, "downloading (about 200 MB)...");
    else {
        if (Btn("Download it (about 200 MB)", true)) DownloadFfmpeg();
    }
    ImGui::Indent(ui::Dp(28.0f));
    Wrapped(ui::kTextDim, "Cuts, joins and resizes videos: long videos in parts, Make it crisp, the quick upscale, Star in a video, and videos Windows cannot play "
                          "by itself. From github.com/BtbN/FFmpeg-Builds (GPL); it goes in the app's own folder, nothing is installed.");
    if (st.rfind("error", 0) == 0) Wrapped(ui::kBad, "%s", st.c_str());
    ImGui::Unindent(ui::Dp(28.0f));
    ImGui::Spacing();
    std::string pst;
    {
        std::lock_guard<std::mutex> lock(g_dl.m);
        pst = g_dl.photocraft;
    }
    const bool haveCraft = !FindPhotocraft(L"photocraft-cli.exe").empty();
    Mark(haveCraft ? 2 : pst == "downloading" ? -1 : 1);
    ImGui::TextUnformatted("PhotoCraft");
    ImGui::SameLine();
    if (haveCraft) ImGui::TextColored(ui::kGood, "ready");
    else if (pst == "downloading") ImGui::TextColored(ui::kAccentHover, "downloading (75 MB)...");
    else if (Btn("Download it (75 MB)", true)) DownloadPhotocraft();
    ImGui::Indent(ui::Dp(28.0f));
    Wrapped(ui::kTextDim, "The text layers on Edits > Designs and on song covers. From github.com/storytold/photocraft (MIT or Apache-2.0), into the app's own folder.");
    if (pst.rfind("error", 0) == 0) Wrapped(ui::kBad, "%s", pst.c_str());
    ImGui::Unindent(ui::Dp(28.0f));
    ImGui::Spacing();
    std::string wst;
    std::wstring wpart;
    {
        std::lock_guard<std::mutex> lock(g_dl.m);
        wst = g_dl.writer, wpart = g_dl.writerPart;
    }
    const bool ready = WriterConfigured(), going = wst.rfind("downloading", 0) == 0;
    Mark(ready ? 2 : going ? -1 : 1);
    ImGui::TextUnformatted("Prompt writer");
    ImGui::SameLine();
    if (ready) ImGui::TextColored(ui::kGood, "ready (%s)", U(fs::path(ClefGetSettings().writerModel).filename().wstring()).c_str());
    else if (going) {
        std::error_code ec;
        const double gb = !wpart.empty() && fs::exists(wpart, ec) ? fs::file_size(wpart, ec) / 1073741824.0 : 0.0;
        ImGui::TextColored(ui::kAccentHover, "%s - %.2f GB so far", wst.c_str(), gb);
    } else {
        char label[96];
        snprintf(label, sizeof(label), "Download it (about %.1f GB)", g_s.pc.nvidia ? 6.4 : 5.8);
        if (Btn(label, true)) DownloadWriter(g_s.pc.nvidia);
    }
    ImGui::Indent(ui::Dp(28.0f));
    Wrapped(ui::kTextDim, "A small language model that runs on this PC: it turns your idea into a video prompt (looking at the picture), writes stories, "
                          "characters and songs. Without it the app uses simple ready-made prompts. llama.cpp (ggml-org, MIT) and Qwen3-VL 8B "
                          "Instruct (Qwen, Apache-2.0), into the app's own folder.");
    if (wst.rfind("error", 0) == 0) Wrapped(ui::kBad, "%s", wst.c_str());
    ImGui::Unindent(ui::Dp(28.0f));
    ImGui::Spacing();
    if (Btn("Skip for now")) ++g_s.step;
}

void StepStart(AppState& app) {
    ImGui::TextUnformatted("That's it. Some things to try first:");
    ImGui::Spacing();
    const std::wstring models = ComfyModelsDir();
    auto have = [&](size_t i) {
        if (models.empty()) return false;
        for (const File& f : Packs()[i].files)
            if (!FileThere(models, f)) return false;
        return true;
    };
    struct Try {
        const char* label;
        const char* what;
        int tab;
        bool ok;
    };
    const Try tries[] = {
        {"Read the prompts in your pictures", "Open a folder of AI pictures in the Gallery: the prompt, settings and LoRAs of each one", 0, true},
        {"Make a picture", "Generate: type what you want and press Make", 4, have(0) || have(1)},
        {"Try a one-click edit", "Edits: pick a recipe (relight, plushie, new angle...) and a picture", 6, have(1)},
        {"Animate a picture, with sound", "Video: pick a picture and an idea card", 10, have(3)},
    };
    for (const Try& t : tries) {
        ImGui::PushID(t.label);
        ImGui::BeginDisabled(!t.ok);
        if (Btn(t.label, false, ImVec2(ui::Dp(260.0f), 0))) {
            app.requestTab = t.tab;
            if (t.tab == 6) SetEditsMode(4);
            g_s.open = false;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("%s%s", t.what, t.ok ? "" : " (its pack is not downloaded yet)");
        ImGui::PopID();
    }
    ImGui::Spacing();
    Wrapped(ui::kTextDim, "Everything else on a page that needs more (Veda's speed-up, the person swap, Drama...) has its own Set up button there, with "
                          "its size, when you get to it.");
}

} // namespace

// A pack's missing files, started from the page that needs them (no trip through the setup)
void SetupDownloadPack(const char* name) {
    const std::wstring models = ComfyModelsDir();
    if (models.empty()) return;
    for (const Pack& p : Packs()) {
        if (strcmp(p.name, name) != 0) continue;
        std::vector<File> files;
        for (const File& f : p.files)
            if (!FileThere(models, f)) files.push_back(f);
        Enqueue(models, files);
    }
}

void OpenSetup(int step) {
    g_s.open = true;
    g_s.step = std::clamp(step, 0, 4);
}

bool SetupDownloading() { return g_dl.running; }
void StopSetupDownloads() { StopDownloads(); }
bool SetupDownloadProgress(int& doneGb, int& totalGb) {
    if (!g_dl.running) return false;
    const double part = PartGb(); // (it takes the lock itself: before ours, not inside it - that threw, a crash on the first download)
    std::lock_guard<std::mutex> lock(g_dl.m);
    doneGb = static_cast<int>(g_dl.doneGb + std::min(part, g_dl.nowGb)), totalGb = static_cast<int>(std::ceil(g_dl.totalGb));
    return true;
}
std::string SetupDownloadLine(float* frac) {
    std::lock_guard<std::mutex> lock(g_dl.m);
    if (!g_dl.running) return {};
    if (frac) *frac = g_dl.totalGb > 0 ? static_cast<float>(g_dl.doneGb / g_dl.totalGb) : 0.0f;
    char b[200];
    snprintf(b, sizeof(b), "Downloading models: %s (%.0f of %.0f GB done)", g_dl.now.c_str(), g_dl.doneGb, g_dl.totalGb);
    return b;
}

// Once a frame: opens itself on a first start that has nowhere to make things (no ComfyUI, or none of the picture models), then draws it
void DrawFirstRunSetup(AppState& app) {
    EnsureComfyService(app);
    static bool first = true;
    if (first) {
        first = false;
        char buf[256] = {};
        if (GetEnvironmentVariableA("MV_UI_TEST", buf, sizeof(buf)) && strstr(buf, "setup")) {
            const char* at = strstr(buf, "setup:");
            OpenSetup(at ? atoi(at + 6) : 0);
        } else if (!SetupDone()) {
            // (any Krea 2 or Qwen Image 2.1 model counts - a Krea merge or a GGUF instead of Comfy-Org's exact files is fine)
            const std::wstring models = ComfyModelsDir();
            bool any = false;
            std::error_code ec;
            for (const wchar_t* sub : {L"diffusion_models", L"unet"}) {
                for (const auto& e : fs::recursive_directory_iterator(fs::path(models) / sub, fs::directory_options::skip_permission_denied, ec)) {
                    std::wstring n = e.path().filename().wstring();
                    for (wchar_t& c : n) c = static_cast<wchar_t>(towlower(c));
                    any = any || n.find(L"krea2") != std::wstring::npos || n.find(L"qwen_image_2.1") != std::wstring::npos || n.find(L"qwen-image-2.1") != std::wstring::npos;
                    if (any) break;
                }
            }
            if (ComfyInstallDir().empty() || models.empty() || !any) OpenSetup(0);
            else SaveDone(); // (everything it would set up is there: never ask)
        }
    }
    if (g_s.open && !g_s.pc.known && !g_s.pcFut.valid()) g_s.pcFut = std::async(std::launch::async, ReadPc);
    if (g_s.pcFut.valid() && g_s.pcFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) g_s.pc = g_s.pcFut.get();
    if (!g_s.open) return;
    if (!g_s.opened) ImGui::OpenPopup("Set up Try2Create"), g_s.opened = true;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 size(std::min(vp->WorkSize.x - ui::Dp(60.0f), ui::Dp(980.0f)), std::min(vp->WorkSize.y - ui::Dp(60.0f), ui::Dp(700.0f)));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    bool keep = true;
    if (!ImGui::BeginPopupModal("Set up Try2Create", &keep, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        if (!keep || !ImGui::IsPopupOpen("Set up Try2Create")) g_s.open = false, g_s.opened = false;
        return;
    }
    static const char* kSteps[] = {"Your PC", "ComfyUI", "Models", "Helpers", "Start"};
    // the steps down the left, what each one says on the right
    const float left = ui::Dp(170.0f), footer = ImGui::GetFrameHeightWithSpacing() + ui::Dp(14.0f);
    ImGui::BeginChild("##steps", ImVec2(left, -footer), ImGuiChildFlags_None);
    std::string where;
    const int comfy = WhereComfy(app, where);
    const std::wstring models = ComfyModelsDir();
    int packsIn = 0;
    for (const Pack& p : Packs()) {
        bool all = !models.empty();
        for (const File& f : p.files) all = all && FileThere(models, f);
        packsIn += all ? 1 : 0;
    }
    const int states[] = {g_s.pc.known ? (g_s.pc.nvidia && g_s.pc.vram >= 12 ? 2 : 1) : -1, comfy == 0 ? 0 : 2, packsIn ? 2 : g_dl.running ? -1 : 0,
                          FfmpegPath().empty() || !WriterConfigured() ? 1 : 2, -1};
    for (int i = 0; i < 5; ++i) {
        ImGui::PushID(i);
        Mark(states[i]);
        char l[48];
        snprintf(l, sizeof(l), "%d. %s", i + 1, kSteps[i]);
        ImGui::BeginDisabled(i > 1 && comfy == 0); // (ComfyUI is the one thing that is needed: the steps after it wait for it)
        if (ImGui::Selectable(l, g_s.step == i)) g_s.step = i;
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    if (comfy == 0 && g_s.step > 1) g_s.step = 1;
    ImGui::EndChild();
    ImGui::SameLine(0.0f, ui::Dp(16.0f));
    ImGui::BeginChild("##body", ImVec2(0, -footer), ImGuiChildFlags_None);
    ImGui::PushFont(ui::TitleFont());
    ImGui::TextUnformatted(kSteps[g_s.step]);
    ImGui::PopFont();
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0.0f);
    switch (g_s.step) {
    case 0: StepPc(); break;
    case 1: StepComfy(app); break;
    case 2: StepModels(app); break;
    case 3: StepHelpers(); break;
    default: StepStart(app); break;
    }
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::Separator();
    if (g_s.step > 0 && Btn("Back")) --g_s.step;
    ImGui::SameLine();
    if (g_s.step < 4) {
        const bool blocked = g_s.step == 1 && comfy == 0;
        ImGui::BeginDisabled(blocked);
        if (Btn("Next", true) && !blocked) ++g_s.step;
        ImGui::EndDisabled();
        if (blocked) ImGui::SameLine(), ImGui::TextColored(ui::kWarn, "ComfyUI first: it is the one thing everything else needs");
    } else if (Btn("Done", true)) {
        g_s.open = false;
    }
    ImGui::SameLine(0.0f, ui::Dp(16.0f));
    if (Btn("Close")) g_s.open = false;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("The Gallery (reading the prompts in your pictures) works without any of this");
    ImGui::SameLine();
    ImGui::TextDisabled("(Help > Set up Try2Create opens this again)");
    if (!g_s.open) {
        SaveDone();
        ImGui::CloseCurrentPopup();
        g_s.opened = false;
    }
    ImGui::EndPopup();
}
