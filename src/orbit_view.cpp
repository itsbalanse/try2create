// A character you can turn around: its 360-degree orbit video (H3 + an orbit LoRA, see video_ui.cpp) shown as a turntable. Every frame of the
// clip is a view from another angle, so dragging sideways picks the frame - the "orbit around a 3D character" feel without building a 3D
// model (no Gaussian splats to train, no mesh: one 50-second video on the GPU, then only the CPU and a small texture).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>
#include <d3d11.h>

#include "app_state.h"
#include "imgui.h"
#include "ui_extra.h"
#include "ui_theme.h"
#include "video_util.h"

namespace fs = std::filesystem;

namespace {

constexpr int kSide = 640; // the frames are kept at this size (an orbit is square; 73 frames are about 120 MB)

struct OrbitFrames {
    std::wstring path;
    std::atomic<bool> ready{false}, failed{false};
    std::vector<uint8_t> rgba; // every frame, kSide x kSide RGBA, one after another
    int count = 0;
};

struct OrbitView {
    std::shared_ptr<OrbitFrames> frames;
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    int shown = -1;          // the frame in the texture
    double pos = 0;          // the frame to show (fractional while turning)
    double velocity = 0;     // frames a second, after a drag lets go
    double lastTouch = -100; // when it was last dragged (it starts turning by itself a while after)
    float zoom = 1.0f;
    void Release() {
        if (srv) ReleaseAfterFrame(srv), srv = nullptr;
        if (tex) ReleaseAfterFrame(tex), tex = nullptr;
        shown = -1;
    }
};
OrbitView g_view;
ULONGLONG g_drawnAt = 0; // the last time a view was on screen (the app keeps redrawing only then)

// Every frame of the video, decoded on a worker thread (CPU decoding, like the app's other ffmpeg reads: the GPU's video engine is left alone)
std::shared_ptr<OrbitFrames> LoadFrames(const std::wstring& video) {
    auto f = std::make_shared<OrbitFrames>();
    f->path = video;
    std::thread([f]() {
        const std::wstring ff = FfmpegPath();
        if (ff.empty()) {
            f->failed = true;
            return;
        }
        wchar_t tmp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tmp);
        const std::wstring raw = std::wstring(tmp) + L"try2create_orbit_" + std::to_wstring(GetCurrentThreadId()) + L".rgba";
        wchar_t vf[200];
        swprintf(vf, 200, L"scale=%d:%d:force_original_aspect_ratio=decrease:flags=lanczos,pad=%d:%d:(ow-iw)/2:(oh-ih)/2", kSide, kSide, kSide, kSide);
        std::string log;
        RunCaptureIdle(L"\"" + ff + L"\" -y -v error -i \"" + f->path + L"\" -an -vf \"" + vf + L"\" -f rawvideo -pix_fmt rgba \"" + raw + L"\"", log, true);
        std::ifstream in(raw, std::ios::binary);
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        std::error_code ec;
        fs::remove(raw, ec);
        const size_t frame = static_cast<size_t>(kSide) * kSide * 4;
        int n = static_cast<int>(data.size() / frame);
        if (n < 8) {
            f->failed = true;
            return;
        }
        // the orbit ends on its first frame (the same picture pins both ends): drop that copy, so turning past it does not stall on one view
        long long diff = 0;
        for (size_t i = 0; i < frame; i += 97) diff += std::abs(static_cast<int>(data[i]) - static_cast<int>(data[(n - 1) * frame + i]));
        if (diff / static_cast<double>(frame / 97) < 6.0) --n;
        data.resize(static_cast<size_t>(n) * frame);
        f->rgba = std::move(data);
        f->count = n;
        f->ready = true;
    }).detach();
    return f;
}

bool Upload(AppState& app, const uint8_t* rgba) {
    ID3D11Device* dev = app.Device();
    if (!dev) return false;
    if (!g_view.tex) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = d.Height = kSide;
        d.MipLevels = d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DYNAMIC;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &g_view.tex))) return false;
        if (FAILED(dev->CreateShaderResourceView(g_view.tex, nullptr, &g_view.srv))) {
            g_view.Release();
            return false;
        }
    }
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    if (!ctx) return false;
    D3D11_MAPPED_SUBRESOURCE m{};
    const bool ok = SUCCEEDED(ctx->Map(g_view.tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m));
    if (ok) {
        for (int y = 0; y < kSide; ++y) std::memcpy(static_cast<uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, rgba + static_cast<size_t>(y) * kSide * 4, kSide * 4);
        ctx->Unmap(g_view.tex, 0);
    }
    ctx->Release();
    return ok;
}

} // namespace

bool OrbitViewAnimating() { return GetTickCount64() - g_drawnAt < 400; }

bool DrawOrbitView(AppState& app, const std::wstring& video, ImVec2 size) {
    g_drawnAt = GetTickCount64();
    if (!g_view.frames || g_view.frames->path != video) {
        g_view.Release();
        g_view.frames = LoadFrames(video);
        g_view.pos = 0, g_view.velocity = 0, g_view.zoom = 1.0f;
        g_view.lastTouch = ImGui::GetTime();
    }
    OrbitFrames& f = *g_view.frames;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##orbit", size);
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 end(origin.x + size.x, origin.y + size.y);
    dl->AddRectFilled(origin, end, IM_COL32(10, 12, 16, 255), ui::Dp(6.0f));
    if (f.failed) {
        dl->AddText(ImVec2(origin.x + ui::Dp(12), origin.y + ui::Dp(12)), ImGui::GetColorU32(ImGuiCol_TextDisabled), "Could not read the 360-degree video");
        return false;
    }
    if (!f.ready) {
        dl->AddText(ImVec2(origin.x + ui::Dp(12), origin.y + ui::Dp(12)), ImGui::GetColorU32(ImGuiCol_TextDisabled), "Loading the 360-degree view...");
        return true;
    }
    const double dt = ImGui::GetIO().DeltaTime, now = ImGui::GetTime();
    const double perPixel = f.count / std::max(1.0, static_cast<double>(size.x) * 1.1); // a drag across the box turns it about once
    if (active) {
        const double dx = ImGui::GetIO().MouseDelta.x;
        g_view.pos -= dx * perPixel;
        g_view.velocity = dt > 0 ? -dx * perPixel / dt * 0.5 + g_view.velocity * 0.5 : g_view.velocity;
        g_view.lastTouch = now;
    } else if (std::abs(g_view.velocity) > 0.5) { // let go: it keeps turning and slows down
        g_view.pos += g_view.velocity * dt;
        g_view.velocity *= std::pow(0.04, dt);
        g_view.lastTouch = now;
    } else if (now - g_view.lastTouch > 2.5) {
        g_view.pos += dt * 10.0; // by itself: a slow turntable (a turn in about 7 s)
    }
    if (hovered) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) g_view.zoom = std::clamp(g_view.zoom * (wheel > 0 ? 1.15f : 1.0f / 1.15f), 1.0f, 3.0f);
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) g_view.zoom = 1.0f;
    }
    g_view.pos = std::fmod(g_view.pos, static_cast<double>(f.count));
    if (g_view.pos < 0) g_view.pos += f.count;
    const int frame = std::clamp(static_cast<int>(std::lround(g_view.pos)) % f.count, 0, f.count - 1);
    if (frame != g_view.shown && Upload(app, f.rgba.data() + static_cast<size_t>(frame) * kSide * kSide * 4)) g_view.shown = frame;
    if (g_view.srv) {
        const float s = std::min(size.x, size.y) * g_view.zoom;
        const ImVec2 c(origin.x + size.x * 0.5f, origin.y + size.y * 0.5f);
        dl->PushClipRect(origin, end, true);
        dl->AddImage((ImTextureID)(intptr_t)g_view.srv, ImVec2(c.x - s * 0.5f, c.y - s * 0.5f), ImVec2(c.x + s * 0.5f, c.y + s * 0.5f));
        dl->PopClipRect();
    }
    // the angle, and how to turn it
    char badge[48];
    snprintf(badge, sizeof(badge), "360\xC2\xB0  %d\xC2\xB0", static_cast<int>(std::lround(g_view.pos * 360.0 / f.count)) % 360);
    const ImVec2 bs = ImGui::CalcTextSize(badge);
    const ImVec2 bp(origin.x + ui::Dp(8), origin.y + ui::Dp(8));
    dl->AddRectFilled(ImVec2(bp.x - ui::Dp(4), bp.y - ui::Dp(2)), ImVec2(bp.x + bs.x + ui::Dp(4), bp.y + bs.y + ui::Dp(2)), IM_COL32(0, 0, 0, 150), ui::Dp(4));
    dl->AddText(bp, IM_COL32(235, 235, 235, 255), badge);
    if (hovered && !active) {
        const char* hint = "Drag to turn  -  wheel: zoom  -  double-click: fit";
        const ImVec2 hs = ImGui::CalcTextSize(hint);
        const ImVec2 hp(origin.x + (size.x - hs.x) * 0.5f, end.y - hs.y - ui::Dp(8));
        dl->AddRectFilled(ImVec2(hp.x - ui::Dp(6), hp.y - ui::Dp(3)), ImVec2(hp.x + hs.x + ui::Dp(6), hp.y + hs.y + ui::Dp(3)), IM_COL32(0, 0, 0, 150), ui::Dp(4));
        dl->AddText(hp, IM_COL32(235, 235, 235, 255), hint);
    }
    if (active || hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    return true; // turning: keep redrawing
}
