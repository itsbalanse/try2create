#include "notify.h"

#include <algorithm>
#include <map>
#include <vector>
#include <d3d11.h>
#include "app_state.h"
#include "comfy.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "ui_theme.h"

namespace {

struct ToastItem {
    ToastKind kind = ToastKind::Info;
    std::string title, body, key;
    std::wstring image;
    std::function<void(AppState&)> open;
    bool crisp = false;
    double born = 0, until = 0;
    int id = 0;
};

std::vector<ToastItem> g_toasts; // oldest first
int g_nextToast = 1;

double Lifetime(ToastKind k) { return k == ToastKind::Error ? 12.0 : k == ToastKind::Success ? 7.0 : 5.0; }

ImVec4 KindColor(ToastKind k) { return k == ToastKind::Error ? ui::kBad : k == ToastKind::Success ? ui::kGood : ui::kAccentHover; }

// pixel art drawn with the point sampler, so its pixels stay square
void UsePointSampler(const ImDrawList*, const ImDrawCmd*) {
    auto* rs = static_cast<ImGui_ImplDX11_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
    if (!rs) return;
    static ID3D11SamplerState* point = nullptr;
    if (!point) {
        D3D11_SAMPLER_DESC d = {};
        d.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        d.AddressU = d.AddressV = d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        d.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
        d.MaxLOD = D3D11_FLOAT32_MAX;
        rs->Device->CreateSamplerState(&d, &point);
    }
    if (point) rs->DeviceContext->PSSetSamplers(0, 1, &point);
}

} // namespace

void Toast(ToastKind kind, const std::string& title, const std::string& body, const std::wstring& image, std::function<void(AppState&)> open, const std::string& key,
           bool crisp) {
    const double now = ImGui::GetCurrentContext() ? ImGui::GetTime() : 0.0;
    if (!key.empty()) {
        for (ToastItem& t : g_toasts) {
            if (t.key != key) continue;
            t.kind = kind;
            t.title = title;
            t.body = body;
            t.image = image;
            t.open = std::move(open);
            t.crisp = crisp;
            t.until = now + Lifetime(kind);
            return;
        }
    }
    ToastItem t;
    t.kind = kind;
    t.title = title;
    t.body = body;
    t.key = key;
    t.image = image;
    t.open = std::move(open);
    t.crisp = crisp;
    t.born = now;
    t.until = now + Lifetime(kind);
    t.id = g_nextToast++;
    g_toasts.push_back(std::move(t));
    if (g_toasts.size() > 4) g_toasts.erase(g_toasts.begin());
}

bool ToastsShowing() { return !g_toasts.empty(); }

void DrawToasts(AppState& app) {
    if (g_toasts.empty()) return;
    const double now = ImGui::GetTime();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float y = vp->WorkPos.y + vp->WorkSize.y - ui::Dp(44.0f);
    int remove = -1;
    std::function<void(AppState&)> toOpen;
    for (int i = static_cast<int>(g_toasts.size()) - 1; i >= 0; --i) {
        ToastItem& t = g_toasts[i];
        const float fadeIn = static_cast<float>(std::clamp((now - t.born) / 0.18, 0.0, 1.0));
        const float fadeOut = static_cast<float>(std::clamp((t.until - now) / 0.4, 0.0, 1.0));
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - ui::Dp(18.0f) + (1.0f - fadeIn) * ui::Dp(24.0f), y), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.97f);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, std::min(fadeIn, fadeOut));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ui::Dp(10.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui::Dp(12.0f), ui::Dp(10.0f)));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Border, ui::WithAlpha(KindColor(t.kind), 0.55f));
        char name[32];
        snprintf(name, sizeof(name), "##toast%d", t.id);
        if (ImGui::Begin(name, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove)) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 wp = ImGui::GetWindowPos();
            dl->AddRectFilled(wp, ImVec2(wp.x + ui::Dp(3.0f), wp.y + ImGui::GetWindowHeight()), ImGui::GetColorU32(KindColor(t.kind)), ui::Dp(10.0f), ImDrawFlags_RoundCornersLeft);
            const float th = ui::Dp(48.0f);
            if (!t.image.empty()) {
                const AppState::LibThumb lt = app.LibraryThumb(t.image);
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(th, th));
                dl->AddRectFilled(p, ImVec2(p.x + th, p.y + th), ui::ThumbPlaceholder(), ui::Dp(6.0f));
                if (lt.srv && lt.w > 0 && lt.h > 0) {
                    const float s = std::min(th / lt.w, th / lt.h);
                    const ImVec2 a(p.x + (th - lt.w * s) * 0.5f, p.y + (th - lt.h * s) * 0.5f);
                    if (t.crisp) dl->AddCallback(UsePointSampler, nullptr);
                    dl->AddImage((ImTextureID)(intptr_t)lt.srv, a, ImVec2(a.x + lt.w * s, a.y + lt.h * s));
                    if (t.crisp) dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
                }
                ImGui::SameLine();
            }
            ImGui::BeginGroup();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ui::Dp(290.0f));
            ImGui::PushFont(ui::BoldFont());
            ImGui::TextColored(t.kind == ToastKind::Error ? ui::kBad : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s", t.title.c_str());
            ImGui::PopFont();
            if (!t.body.empty()) ImGui::TextDisabled("%s", t.body.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PushID(t.id);
            if (t.open) {
                if (ImGui::SmallButton("Open")) toOpen = t.open, remove = i;
                ImGui::SameLine();
            }
            if (ImGui::SmallButton("Dismiss")) remove = i;
            ImGui::PopID();
            ImGui::EndGroup();
            // the whole card opens it too
            if (t.open && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered()) toOpen = t.open, remove = i;
            if (ImGui::IsWindowHovered()) t.until = std::max(t.until, now + 1.5); // reading it: it stays
            y -= ImGui::GetWindowHeight() + ui::Dp(8.0f);
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(4);
    }
    if (remove >= 0 && remove < static_cast<int>(g_toasts.size())) g_toasts.erase(g_toasts.begin() + remove);
    g_toasts.erase(std::remove_if(g_toasts.begin(), g_toasts.end(), [&](const ToastItem& t) { return t.until < now; }), g_toasts.end());
    if (toOpen) toOpen(app);
}

void WatchComfyJobs(AppState& app) {
    if (!app.comfySvc) return;
    static std::map<int, bool> seenRunning; // jobs seen before they finished
    static std::map<int, bool> announced;
    const std::vector<ComfyActivity> acts = app.comfySvc->Activity();
    for (const ComfyActivity& a : acts) {
        const bool finished = a.state == ComfyState::Done || a.state == ComfyState::Failed;
        if (!finished) {
            seenRunning[a.id] = true;
            continue;
        }
        if (!seenRunning.count(a.id) || announced.count(a.id)) continue;
        announced[a.id] = true;
        // these tabs announce their own results (or would flood: a Clef run makes dozens)
        bool own = false;
        for (const char* p : {"Pixel art", "Cut out", "Clef mode", "Character"}) own = own || a.label.rfind(p, 0) == 0;
        if (own || a.message == "Cancelled") continue; // cancelled on purpose: nothing to tell
        if (a.state == ComfyState::Failed) {
            Toast(ToastKind::Error, "Failed: " + a.label, a.message.size() > 220 ? a.message.substr(0, 220) + "..." : a.message, {},
                  [](AppState& s) { s.requestTab = 4; }, "comfy" + std::to_string(a.id));
        } else {
            std::wstring file;
            int n = 0;
            for (const ComfyResult& f : a.files) {
                if (f.localPath.empty()) continue;
                ++n;
                if (file.empty()) file = f.localPath;
            }
            if (file.empty()) continue;
            const double secs = a.finishedMs > a.startedMs ? (a.finishedMs - a.startedMs) / 1000.0 : 0.0;
            char took[48] = "";
            if (secs > 0) snprintf(took, sizeof(took), "%s in %.0f s", n > 1 ? (std::to_string(n) + " results").c_str() : "Done", secs);
            Toast(ToastKind::Success, "Ready: " + a.label, took[0] ? std::string(took) : std::string(), file,
                  [file](AppState& s) {
                      s.OpenFile(file);
                      s.requestTab = 0;
                  },
                  "comfy" + std::to_string(a.id));
        }
    }
}
