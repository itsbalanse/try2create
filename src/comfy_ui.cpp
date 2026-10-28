// The ComfyUI tab (what was sent, what ComfyUI is doing, what it made) and the helper the gallery / Vision library / Vision board call to send a prompt.
#include <algorithm>
#include <cmath>
#include <future>
#include <fstream>
#include <map>
#include <filesystem>
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include "clipboard_image.h"
#include "crash_log.h"
#include "image_texture.h"
#include "imgui.h"
#include "imgui_internal.h" // RenderTextEllipsis
#include "clef.h"
#include "clef_grades.h"
#include "ui_extra.h"
#include "video_player.h"
#include "ui_theme.h"
#include "win_dialogs.h"
#include "workflow.h"

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

int StringResize(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* str = static_cast<std::string*>(data->UserData);
        str->resize(data->BufTextLen);
        data->Buf = str->data();
    }
    return 0;
}

bool InputStr(const char* label, const char* hint, std::string& value, float width) {
    ImGui::SetNextItemWidth(width);
    return ImGui::InputTextWithHint(label, hint, value.data(), value.capacity() + 1, ImGuiInputTextFlags_CallbackResize, StringResize, &value);
}

static int g_lastEditActivity = 0; // the activity id of the last thing sent (a picture edit or a prompt)

void EnsureComfy(AppState& app) {
    if (!app.comfyLoaded) {
        app.comfyLoaded = true;
        LoadComfySettings(app.comfy);
    }
    if (!app.comfySvc) app.comfySvc = std::make_unique<ComfyService>();
}

std::string WorkflowName(const std::string& path) { return path.size() > 5 ? path.substr(0, path.size() - 5) : path; }

// the list of ComfyUI's saved workflows is refreshed in the background, at most every `seconds`
void RefreshListSometimes(AppState& app, double seconds) {
    static double last = -1000.0;
    const double now = ImGui::GetTime();
    if (now - last < seconds || app.comfySvc->Refreshing()) return;
    last = now;
    app.comfySvc->RefreshWorkflows(app.comfy);
}

std::string CurrentWorkflowLabel(const ComfySettings& s) {
    if (s.useLastRun) return "The workflow ComfyUI ran last";
    if (!s.savedWorkflow.empty()) return WorkflowName(s.savedWorkflow);
    return s.workflowPath.empty() ? "Choose a workflow..." : ToUtf8(fs::path(s.workflowPath).filename().wstring());
}

const char* StateName(ComfyState s) {
    switch (s) {
        case ComfyState::Waiting: return "Waiting";
        case ComfyState::Starting: return "Starting ComfyUI";
        case ComfyState::Sending: return "Sending";
        case ComfyState::Queued: return "Queued";
        case ComfyState::Running: return "Generating";
        case ComfyState::Done: return "Done";
        case ComfyState::Failed: return "Failed";
    }
    return "";
}

ImVec4 StateColor(ComfyState s) {
    switch (s) {
        case ComfyState::Done: return ui::kGood;
        case ComfyState::Failed: return ui::kBad;
        case ComfyState::Running: return ui::kAccent;
        default: return ui::kWarn;
    }
}

std::string Elapsed(const ComfyActivity& a) {
    const long long end = a.finishedMs > 0 ? a.finishedMs : ComfyService::NowMs();
    const long long sec = std::max(0LL, (end - a.startedMs) / 1000);
    char buf[32];
    if (sec >= 60) std::snprintf(buf, sizeof(buf), "%lld min %lld s", sec / 60, sec % 60);
    else std::snprintf(buf, sizeof(buf), "%lld s", sec);
    return buf;
}

void OpenWithWindows(const std::wstring& path) { ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

void ShowFileInExplorer(const std::wstring& path) {
    const std::wstring args = L"/select,\"" + path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

// The viewer's video player (a video result plays right in the viewer)
std::unique_ptr<VideoPlayer> g_resPlayer;
bool g_resMuted = false;

// One picture or video in the grid: click to look at it (a video plays in the viewer), double click to open a picture in the gallery.
bool DrawResultTile(AppState& app, const ComfyResult& r, float size, int id, bool selected) {
    ImGui::PushID(id);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##tile", ImVec2(size, size));
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p1(p.x + size, p.y + size);
    bool drawn = false;
    if (!r.localPath.empty() && !r.audio) { // a video shows its first frame
        const AppState::LibThumb t = app.LibraryThumb(r.localPath);
        if (t.srv && t.w > 0 && t.h > 0) {
            ImVec2 uv0(0, 0), uv1(1, 1);
            if (t.w > t.h) { const float m = (1.0f - float(t.h) / t.w) * 0.5f; uv0.x = m; uv1.x = 1 - m; }
            else if (t.h > t.w) { const float m = (1.0f - float(t.w) / t.h) * 0.5f; uv0.y = m; uv1.y = 1 - m; }
            dl->AddImageRounded((ImTextureID)(intptr_t)t.srv, p, p1, uv0, uv1, IM_COL32_WHITE, ui::Dp(6.0f));
            drawn = true;
        }
    }
    if (!drawn) {
        dl->AddRectFilled(p, p1, ui::ThumbPlaceholder(), ui::Dp(6.0f));
        const char* text = r.audio ? "SONG" : r.video ? "VIDEO" : (r.localPath.empty() ? "not on this PC" : "...");
        const ImVec2 ts = ImGui::CalcTextSize(text);
        dl->AddText(ImVec2(p.x + (size - ts.x) * 0.5f, p.y + (size - ts.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled), text);
    }
    if (r.video && drawn) { // a play sign on it
        const ImVec2 mid(p.x + size * 0.5f, p.y + size * 0.5f);
        dl->AddCircleFilled(mid, ui::Dp(12.0f), IM_COL32(10, 12, 16, 160), 20);
        dl->AddTriangleFilled(ImVec2(mid.x - ui::Dp(4.0f), mid.y - ui::Dp(6.0f)), ImVec2(mid.x - ui::Dp(4.0f), mid.y + ui::Dp(6.0f)), ImVec2(mid.x + ui::Dp(7.0f), mid.y), IM_COL32_WHITE);
    }
    if (selected) dl->AddRect(p, p1, ui::WithAlpha(ui::kAccent, 1.0f), ui::Dp(6.0f), 0, ui::Dp(2.5f));
    else if (hovered) dl->AddRect(p, p1, IM_COL32(255, 255, 255, 90), ui::Dp(6.0f), 0, ui::Dp(1.5f));
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !r.localPath.empty() && !r.video && !r.audio) { // a video or song plays in the viewer
        app.OpenFile(r.localPath);
        app.requestTab = 0;
    }
    if (hovered && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        if (r.seconds > 0) ImGui::SetTooltip("%s\n%s\nMade in %s", r.filename.c_str(), r.label.c_str(), FormatTook(r.seconds).c_str());
        else ImGui::SetTooltip("%s\n%s", r.filename.c_str(), r.label.c_str());
    }
    if (ImGui::BeginPopupContextItem("##tilemenu")) {
        ImGui::BeginDisabled(r.localPath.empty());
        if (ImGui::MenuItem(r.video || r.audio ? "Play in your own player" : "Open in the gallery")) {
            if (r.video || r.audio) OpenWithWindows(r.localPath);
            else {
                app.OpenFile(r.localPath);
                app.requestTab = 0;
            }
        }
        if (ImGui::MenuItem("Show in Explorer")) ShowFileInExplorer(r.localPath);
        if (!r.audio && ImGui::MenuItem("Test LoRA mixes on it...")) TestLoraMixesOn(app, r.localPath);
        if (!r.audio) NsfwMarkMenu(app, r.localPath);
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return clicked;
}

// Everything ComfyUI made this session: a grid on the left and a viewer on the right (arrow keys, mouse wheel zoom, drag to pan).
// The results browser's state (the grid and the viewer are drawn apart: the grid under the Generate composer, the viewer beside them)
struct ResBrowser {
    int sel = -1;
    bool follow = true; // a new result is shown as soon as it is made, until you pick an older one
    size_t lastN = 0;
    LoadedImage img;
    std::wstring imgPath;
    std::string imgError;
    float zoomMul = 1.0f;
    ImVec2 pan = ImVec2(0, 0);
};
ResBrowser g_rb;

// Which result is shown (arrows, new ones), and its picture or player loaded: once a frame, before the grid and the viewer
void ResultsPrepare(AppState& app, const std::vector<ComfyResult>& results) {
    if (results.empty()) return;
    int& sel = g_rb.sel;
    bool& follow = g_rb.follow;
    size_t& lastN = g_rb.lastN;
    LoadedImage& img = g_rb.img;
    std::wstring& imgPath = g_rb.imgPath;
    std::string& imgError = g_rb.imgError;
    float& zoomMul = g_rb.zoomMul;
    ImVec2& pan = g_rb.pan;
    const int n = static_cast<int>(results.size());
    if (results.size() != lastN) {
        if (results.size() > lastN && follow) sel = n - 1;
        lastN = results.size();
    }
    if (sel < 0 || sel >= n) sel = n - 1;
    ImGuiIO& io = ImGui::GetIO();
    if (app.activeTab == 4 && !io.WantTextInput && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) { sel = std::max(0, sel - 1); follow = sel == n - 1; }
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) { sel = std::min(n - 1, sel + 1); follow = sel == n - 1; }
    }
    const ComfyResult& cur = results[sel];
    // a video plays right here
    const bool media = cur.video || cur.audio;
    if (media && !cur.localPath.empty()) {
        if (!g_resPlayer) g_resPlayer = std::make_unique<VideoPlayer>();
        if (!g_resPlayer->IsOpen() || _wcsicmp(g_resPlayer->Path().c_str(), cur.localPath.c_str()) != 0) {
            ID3D11DeviceContext* ctx = nullptr;
            app.Device()->GetImmediateContext(&ctx);
            // paused on its first frame: it plays when you press Play or click it (it used to start, and loop, as soon as it came in)
            if (!g_resPlayer->Open(app.Device(), ctx, cur.localPath, 0.85f, g_resMuted, true, false)) app.SetStatus(g_resPlayer->Error());
            if (ctx) ctx->Release();
        }
    } else if (g_resPlayer && g_resPlayer->IsOpen()) {
        g_resPlayer->Close();
    }
    VideoPlayer* vp = media && g_resPlayer && g_resPlayer->IsOpen() ? g_resPlayer.get() : nullptr;
    const std::wstring wantPath = media ? std::wstring() : cur.localPath;
    if (wantPath != imgPath) {
        ReleaseImage(img);
        img = LoadedImage{};
        imgPath = wantPath;
        imgError.clear();
        zoomMul = 1.0f;
        pan = ImVec2(0, 0);
        if (!wantPath.empty() && !LoadImageTexture(app.Device(), wantPath, img, &imgError) && imgError.empty()) imgError = "Could not open this picture";
    }

}

void DrawResultsGrid(AppState& app, const std::vector<ComfyResult>& results, float h) {
    if (results.empty()) {
        ImGui::TextDisabled("What you make shows here.");
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    int& sel = g_rb.sel;
    bool& follow = g_rb.follow;
    const int n = static_cast<int>(results.size());
    static float tileDp = 92.0f; // Ctrl + mouse wheel over the grid, or the slider, sizes the tiles
    ImGui::BeginChild("##resgrid", ImVec2(0, h), true, ImGui::GetIO().KeyCtrl ? ImGuiWindowFlags_NoScrollWithMouse : 0);
    {
        if (ImGui::IsWindowHovered() && io.KeyCtrl && io.MouseWheel != 0.0f) tileDp = std::clamp(tileDp + io.MouseWheel * 12.0f, 56.0f, 260.0f);
        ImGui::SetNextItemWidth(std::min(ui::Dp(160.0f), ImGui::GetContentRegionAvail().x));
        ImGui::SliderFloat("##tilesize", &tileDp, 56.0f, 260.0f, "Tile size");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Or Ctrl + mouse wheel over the tiles");
        const float tile = ui::Dp(tileDp);
        const float avail = ImGui::GetContentRegionAvail().x;
        const int perRow = std::max(1, static_cast<int>((avail + ImGui::GetStyle().ItemSpacing.x) / (tile + ImGui::GetStyle().ItemSpacing.x)));
        int col = 0;
        for (int i = n - 1; i >= 0; --i) {
            if (col > 0) ImGui::SameLine();
            if (DrawResultTile(app, results[i], tile, i, i == sel)) {
                sel = i;
                follow = i == n - 1;
            }
            if (++col >= perRow) col = 0;
        }
    }
    ImGui::EndChild();

}

void DrawResultsViewer(AppState& app, const std::vector<ComfyResult>& results, float h) {
    if (results.empty()) {
        ImGui::BeginChild("##resview", ImVec2(0, h), true);
        ImGui::TextDisabled("The picture you pick (or the newest) shows here, big.");
        ImGui::EndChild();
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    int& sel = g_rb.sel;
    bool& follow = g_rb.follow;
    LoadedImage& img = g_rb.img;
    std::string& imgError = g_rb.imgError;
    float& zoomMul = g_rb.zoomMul;
    ImVec2& pan = g_rb.pan;
    const int n = static_cast<int>(results.size());
    const ComfyResult& cur = results[std::clamp(sel, 0, n - 1)];
    const bool media = cur.video || cur.audio;
    VideoPlayer* vp = media && g_resPlayer && g_resPlayer->IsOpen() ? g_resPlayer.get() : nullptr;
    ImGui::BeginChild("##resview", ImVec2(0, h), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    bool want100 = false;
    if (ImGui::SmallButton("< Older")) { sel = std::max(0, sel - 1); follow = false; }
    ImGui::SameLine();
    if (ImGui::SmallButton("Newer >")) { sel = std::min(n - 1, sel + 1); follow = sel == n - 1; }
    ImGui::SameLine();
    ImGui::Text("%d / %d", sel + 1, n);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", cur.filename.c_str());
    if (!media) {
        if (ImGui::SmallButton("Fit")) { zoomMul = 1.0f; pan = ImVec2(0, 0); }
        ImGui::SameLine();
        if (ImGui::SmallButton("100%")) want100 = true;
        ImGui::SameLine();
    }
    if (cur.seconds > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("  made in %s", FormatTook(cur.seconds).c_str());
    }
    ImGui::BeginDisabled(cur.localPath.empty());
    if (media) {
        if (vp) { // the player's controls
            if (ImGui::SmallButton(vp->Playing() ? "Pause" : "Play")) vp->Toggle();
            ImGui::SameLine();
            float pos = static_cast<float>(vp->Position());
            const float dur = static_cast<float>(std::max(0.01, vp->Duration()));
            char clock[48];
            snprintf(clock, sizeof(clock), "%d:%02d / %d:%02d", static_cast<int>(pos) / 60, static_cast<int>(pos) % 60, static_cast<int>(dur) / 60, static_cast<int>(dur) % 60);
            ImGui::SetNextItemWidth(ui::Dp(240.0f));
            if (ImGui::SliderFloat("##resseek", &pos, 0.0f, dur, clock)) vp->Seek(pos);
            ImGui::SameLine();
            if (ImGui::Checkbox("Mute", &g_resMuted)) vp->SetMuted(g_resMuted);
            ImGui::SameLine();
            {
                float pct = VideoPlayer::VideoVolume() * 100.0f;
                ImGui::SetNextItemWidth(ui::Dp(84.0f));
                if (ImGui::SliderFloat("##resvol", &pct, 0.0f, 100.0f, "vol %.0f%%")) {
                    app.videoVolume = pct / 100.0f;
                    VideoPlayer::SetVideoVolume(app.videoVolume);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) app.SaveUiSettings();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("The volume of every video in the app (also under View)");
            }
            ImGui::SameLine();
        }
        if (ImGui::SmallButton("Open in your player")) OpenWithWindows(cur.localPath);
    } else if (ImGui::SmallButton("Open in the gallery")) {
        app.OpenFile(cur.localPath);
        app.requestTab = 0;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Show in Explorer")) ShowFileInExplorer(cur.localPath);
    if (!media) {
        ui::SameLineIfFits("Edit this picture...");
        if (ImGui::SmallButton("Edit this picture...")) OpenPictureEditor(app, {cur.localPath});
        ui::SameLineIfFits("Box edit...");
        if (ImGui::SmallButton("Box edit...")) OpenLayoutStudio(app, cur.localPath);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Recolour, remove, replace or reword what is inside a box (MOSS, ~4 s); the rest stays exactly");
        ui::SameLineIfFits("Characters from it...");
        if (ImGui::SmallButton("Characters from it...")) OpenCharacterExtract(app, cur.localPath);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Every person or creature in it becomes one of your characters, with a name and a backstory");
        ui::SameLineIfFits("Upscale...");
        if (ImGui::SmallButton("Upscale...")) OpenUpscaler(app, {cur.localPath});
        ui::SameLineIfFits("Text over it...");
        if (ImGui::SmallButton("Text over it...")) OpenDesigner(app, {cur.localPath});
        ui::SameLineIfFits("Pixel art...");
        PixelArtButton(app, {cur.localPath}, "##pxquickcomf");
        ui::SameLineIfFits("Copy image");
        if (ImGui::SmallButton("Copy image")) {
            std::string msg;
            CopyImagesToClipboard(GetActiveWindow(), {cur.localPath}, false, &msg);
            app.SetStatus(msg);
        }
    }
    ui::SameLineIfFits("Copy clean");
    if (ImGui::SmallButton("Copy clean")) {
        std::string msg;
        CopyImagesToClipboard(GetActiveWindow(), {cur.localPath}, true, &msg);
        app.SetStatus(msg);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("A copy without its metadata (no prompt, no workflow inside), to post");
    ImGui::EndDisabled();

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float promptH = cur.prompt.empty() ? ui::Dp(30.0f) : ui::Dp(150.0f);
    const ImVec2 region(std::max(40.0f, avail.x), std::max(ui::Dp(120.0f), avail.y - promptH));
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##resimg", region);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, ImVec2(origin.x + region.x, origin.y + region.y), IM_COL32(10, 12, 16, 255), ui::Dp(6.0f));
    if (img.srv && img.width > 0 && img.height > 0) {
        const float fit = std::min(region.x / img.width, region.y / img.height);
        if (want100) {
            zoomMul = 1.0f / fit;
            pan = ImVec2(0, 0);
        }
        if (hovered && io.MouseWheel != 0.0f) {
            const float k = std::clamp(std::pow(1.18f, io.MouseWheel) * zoomMul, 0.2f, 20.0f) / zoomMul;
            const ImVec2 c(origin.x + region.x * 0.5f, origin.y + region.y * 0.5f);
            pan.x = (io.MousePos.x - c.x) - (io.MousePos.x - c.x - pan.x) * k; // the point under the pointer stays put
            pan.y = (io.MousePos.y - c.y) - (io.MousePos.y - c.y - pan.y) * k;
            zoomMul *= k;
        }
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
            pan.x += io.MouseDelta.x;
            pan.y += io.MouseDelta.y;
        }
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            zoomMul = zoomMul > 1.01f ? 1.0f : std::min(20.0f, 1.0f / fit);
            pan = ImVec2(0, 0);
        }
        const float scale = fit * zoomMul;
        const ImVec2 dim(img.width * scale, img.height * scale);
        const ImVec2 c(origin.x + region.x * 0.5f + pan.x, origin.y + region.y * 0.5f + pan.y);
        dl->PushClipRect(origin, ImVec2(origin.x + region.x, origin.y + region.y), true);
        const bool blur = app.ShouldBlur(cur.localPath);
        if (ID3D11ShaderResourceView* shown = app.ShownSrv(img.srv, blur))
            dl->AddImage((ImTextureID)(intptr_t)shown, ImVec2(c.x - dim.x * 0.5f, c.y - dim.y * 0.5f), ImVec2(c.x + dim.x * 0.5f, c.y + dim.y * 0.5f));
        DrawNsfwOverlay(app, cur.localPath, origin, ImVec2(origin.x + region.x, origin.y + region.y));
        char zoomText[48];
        std::snprintf(zoomText, sizeof(zoomText), "%d x %d   %.0f%%", img.width, img.height, scale * 100.0f);
        dl->AddText(ImVec2(origin.x + ui::Dp(8.0f), origin.y + region.y - ImGui::GetTextLineHeight() - ui::Dp(6.0f)), IM_COL32(255, 255, 255, 170), zoomText);
        dl->PopClipRect();
    } else if (vp) { // the video, fitted; click to pause
        vp->Update();
        if (ID3D11ShaderResourceView* srv = vp->Srv(); srv && vp->Width() > 0 && vp->Height() > 0) {
            const float s = std::min(region.x / vp->Width(), region.y / vp->Height());
            const ImVec2 a(origin.x + (region.x - vp->Width() * s) * 0.5f, origin.y + (region.y - vp->Height() * s) * 0.5f);
            const ImVec2 b(a.x + vp->Width() * s, a.y + vp->Height() * s);
            if (app.ShouldBlur(cur.localPath)) { // NSFW: its blurred still
                const AppState::LibThumb lt = app.LibraryThumb(cur.localPath);
                if (lt.srv) dl->AddImage((ImTextureID)(intptr_t)lt.srv, a, b);
            } else {
                dl->AddImage((ImTextureID)(intptr_t)srv, a, b);
            }
            DrawNsfwOverlay(app, cur.localPath, a, b);
        } else if (cur.audio) { // a song: no picture
            const std::string t = std::string(vp->Playing() ? "Playing " : "Paused: ") + cur.filename;
            const ImVec2 ts = ImGui::CalcTextSize(t.c_str());
            dl->AddText(ImVec2(origin.x + (region.x - ts.x) * 0.5f, origin.y + (region.y - ts.y) * 0.5f), IM_COL32(220, 222, 230, 255), t.c_str());
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) vp->Toggle();
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) OpenWithWindows(cur.localPath);
    } else {
        const std::string text = cur.video ? "Video: press Open in your player to watch it"
                                 : !imgError.empty() ? imgError
                                 : cur.localPath.empty() ? "This file is not on this computer" : "Loading...";
        const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
        dl->AddText(ImVec2(origin.x + (region.x - ts.x) * 0.5f, origin.y + (region.y - ts.y) * 0.5f), IM_COL32(200, 200, 205, 220), text.c_str());
        if (cur.video && hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !cur.localPath.empty()) OpenWithWindows(cur.localPath);
    }

    // the prompt that made it: change it and send it again for a new version
    ImGui::BeginChild("##resprompt", ImVec2(0, 0), false);
    if (!cur.prompt.empty()) {
        static std::string edited;
        static size_t editedFor = ~size_t(0);
        static std::string editedFrom;
        static int again = 1;
        if (editedFor != static_cast<size_t>(sel) || editedFrom != cur.filename) { // another picture: its own prompt
            edited = cur.prompt;
            editedFor = static_cast<size_t>(sel);
            editedFrom = cur.filename;
        }
        const bool changedText = edited != cur.prompt;
        ImGui::TextDisabled("From: %s", cur.label.c_str());
        ImGui::SameLine();
        ImGui::TextUnformatted(changedText ? "(prompt edited)" : "Edit the prompt, then send it again:");
        const float boxH = std::max(ui::Dp(50.0f), ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() - ui::Dp(4.0f));
        WrapEdit("##editprompt", edited, boxH);
        if (ui::PrimaryButton(changedText ? "Send the edited prompt" : "Make another one", ImVec2(0, 0))) {
            again = std::clamp(again, 1, 100);
            if (GenerateAgain(app, edited, cur.label, again)) {
                app.SetStatus("Sent " + std::to_string(again) + (again == 1 ? " new version" : " new versions") + ": " + cur.label);
            }
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ui::Dp(110.0f));
        ImGui::InputInt("times##again", &again, 1, 5);
        again = std::clamp(again, 1, 100);
        ImGui::SameLine();
        ImGui::BeginDisabled(!changedText);
        if (ImGui::Button("Undo my edits")) edited = cur.prompt;
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Copy")) {
            ImGui::SetClipboardText(edited.c_str());
            app.SetStatus("Copied the prompt");
        }
    } else {
        ImGui::TextDisabled("From: %s (no prompt was kept for this one)", cur.label.c_str());
    }
    ImGui::EndChild();
    ImGui::EndChild();
}

} // namespace

bool SendToComfy(AppState& app, const std::string& prompt, const std::string& negative, const std::wstring& picture, const std::string& label, bool useDefaults,
                 double seconds, int times, int batch) {
    EnsureComfy(app);
    if (!ClefGradeRunning()) LlmStopOwned(); // ComfyUI needs the GPU: a Clef loaded for something else gives it back
    if (prompt.empty()) {
        app.SetStatus("There is no prompt to send");
        return false;
    }
    if (!app.comfy.useLastRun && app.comfy.savedWorkflow.empty() && app.comfy.workflowPath.empty()) {
        app.requestTab = 4;
        app.SetStatus("Choose which ComfyUI workflow to run first (Generate page)");
        return false;
    }
    ComfyJob job;
    job.prompt = prompt;
    job.label = label;
    job.seconds = seconds;
    job.times = times;
    job.batch = batch;
    if (useDefaults) {
        if (app.comfy.sendNegative) job.negative = negative;
        if (app.comfy.sendPicture) job.picture = picture;
    } else {
        job.negative = negative;
        job.picture = picture;
    }
    g_lastEditActivity = app.comfySvc->Send(app.comfy, std::move(job));
    app.SetStatus("Sent \"" + label + "\" to ComfyUI at " + app.comfy.url + ": watch it on the Generate page");
    return true;
}

std::wstring H3TurboWorkflow(bool fromPicture) {
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    const fs::path f = fs::path(buf).parent_path() / L"tools" / L"h3turbo" /
                       (fromPicture ? L"H3_Turbo_4step_picture_to_video_API.json" : L"H3_Turbo_4step_text_to_video_API.json");
    std::error_code ec;
    return fs::exists(f, ec) ? f.wstring() : std::wstring();
}

bool SendH3Workflow(AppState& app, const std::wstring& workflow, const std::wstring& picture, const std::vector<std::pair<std::string, std::wstring>>& nodePictures,
                    const std::string& prompt, double seconds, int times, const std::string& label, bool fixedSeed) {
    EnsureComfy(app);
    if (!ClefGradeRunning()) LlmStopOwned(); // the video model wants the whole GPU
    if (prompt.find_first_not_of(" \t\r\n") == std::string::npos) {
        app.SetStatus("There is no prompt to send");
        return false;
    }
    // this workflow as it is: none of the ComfyUI tab's choices (its workflow, its LoRAs, which node holds the prompt)
    ComfySettings s = app.comfy;
    s.useLastRun = false;
    s.savedWorkflow.clear();
    s.workflowPath = workflow;
    s.promptLoras.clear();
    s.promptReplaceLoras = false;
    s.promptNode.clear();
    s.negativeNode.clear();
    s.videoLength = true;
    s.defaultFps = 24;
    s.randomSeed = !fixedSeed && GetEnvironmentVariableW(L"MV_FIXED_SEED", nullptr, 0) == 0; // (tests, the LoRA browser: the workflow's own seed, to compare)
    s.sendPicture = !picture.empty();
    s.sendNegative = false;
    ComfyJob job;
    job.prompt = prompt;
    job.label = label;
    job.seconds = std::clamp(seconds > 0 ? seconds : 5.0, 3.0, 15.0); // trained on about 5-15 s
    job.times = std::max(1, times);
    job.picture = picture;
    job.nodePictures = nodePictures;
    g_lastEditActivity = app.comfySvc->Send(s, std::move(job));
    app.SetStatus("Sent \"" + label + "\" to MiniMax H3: watch it above the gallery, on the Video page or on the Generate page");
    return true;
}

std::string ComfyWorkflowLabel(AppState& app) {
    EnsureComfy(app);
    return CurrentWorkflowLabel(app.comfy);
}

void PumpComfy(AppState& app) {
    EnsureComfy(app);
    app.comfySvc->SetAddress(app.comfy.url);
    for (const std::string& m : app.comfySvc->TakeMessages()) app.SetStatus(m);
    // the auto-importer: ComfyUI's saved workflows are looked at (and new ones copied into the Workflows tab) now and then
    static double lastAuto = -1000.0;
    const double now = ImGui::GetTime();
    if (app.comfy.autoImport && now > 8.0 && now - lastAuto > 120.0 && app.comfySvc->Reachable()) {
        lastAuto = now;
        app.comfySvc->RefreshWorkflows(app.comfy);
    }
}

std::string ComfyTabLabel(AppState& app) {
    EnsureComfy(app);
    const int n = app.comfySvc->Active();
    return (n > 0 ? "ComfyUI (" + std::to_string(n) + ")" : std::string("ComfyUI")) + "###comfytab";
}

// Items for a menu: pick which workflow the prompts run in.
void ComfyWorkflowMenu(AppState& app) {
    EnsureComfy(app);
    RefreshListSometimes(app, 20.0);
    ComfySettings& s = app.comfy;
    bool changed = false;
    if (ImGui::MenuItem("The workflow ComfyUI ran last", nullptr, s.useLastRun)) {
        s.useLastRun = true;
        changed = true;
    }
    ImGui::Separator();
    const std::vector<ComfySavedWorkflow> list = app.comfySvc->SavedWorkflows();
    if (list.empty()) ImGui::TextDisabled("%s", app.comfySvc->Refreshing() ? "Reading ComfyUI's workflows..." : "No saved workflows found");
    int shown = 0;
    for (const ComfySavedWorkflow& w : list) {
        if (++shown > 40) break;
        if (ImGui::MenuItem(WorkflowName(w.path).c_str(), nullptr, !s.useLastRun && s.savedWorkflow == w.path)) {
            s.useLastRun = false;
            s.savedWorkflow = w.path;
            changed = true;
        }
    }
    if (changed) SaveComfySettings(s);
}

void DrawComfyTab(AppState& app, float height) {
    EnsureComfy(app);
    ComfySettings& s = app.comfy;
    ComfyService& svc = *app.comfySvc;
    RefreshListSometimes(app, 30.0);
    static std::string checkMessage;
    static bool checkOk = false;
    static ComfyTemplateInfo tmpl;
    static bool tmplValid = false;
    bool changed = false;

    // two columns: making (what to make, what was made, ComfyUI, what was sent) on the left; the picture, big, on the right
    if (!ImGui::BeginTable("##genpage", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings, ImVec2(0, height))) return;
    ImGui::TableSetupColumn("make", ImGuiTableColumnFlags_WidthStretch, 0.46f);
    ImGui::TableSetupColumn("view", ImGuiTableColumnFlags_WidthStretch, 0.54f);
    ImGui::TableNextRow(0, height);
    ImGui::TableSetColumnIndex(0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui::Dp(12.0f), ui::Dp(10.0f))); // (not against the page's edge)
    ImGui::BeginChild("##comfytab", ImVec2(0, height), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    ImGui::PushTextWrapPos(0);
    DrawGenerateComposer(app);
    ImGui::Spacing();
    // ---- 4. what ComfyUI made: a grid and a viewer
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Made");
    const std::vector<ComfyResult> results = svc.Results();
    if (!results.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear the list")) {
            const int n = svc.ClearResults(false);
            app.SetStatus("Cleared " + std::to_string(n) + (n == 1 ? " picture" : " pictures") + " from the list (the files are still on disk)");
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Empties this list. Nothing is deleted: the files stay in ComfyUI's output folder.");
        ui::SameLineIfFits("Take off moved or deleted files");
        if (ImGui::SmallButton("Take off moved or deleted files")) {
            const int n = svc.ClearResults(true);
            app.SetStatus(n == 0 ? std::string("Every file on the list is still there") : "Took " + std::to_string(n) + (n == 1 ? " missing file" : " missing files") + " off the list");
        }
    }
    const std::wstring outDirNow = ComfyOutputDir(s);
    if (!outDirNow.empty()) {
        ui::SameLineIfFits("All of ComfyUI's output in the gallery");
        if (ImGui::SmallButton("All of ComfyUI's output in the gallery")) {
            app.OpenFolder(outDirNow);
            app.requestTab = 0;
        }
        ui::SameLineIfFits("Show in Explorer");
        if (ImGui::SmallButton("Show in Explorer")) OpenWithWindows(outDirNow);
    }
    ResultsPrepare(app, results);
    DrawResultsGrid(app, results, std::max(ui::Dp(200.0f), ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 3.0f));

    // ---- 1. is ComfyUI running, and where do prompts go (folded: open while it is not running)
    const bool up = svc.Reachable();
    if (!up && !ComfyOwned()) { // (one line: everything else about ComfyUI is folded below)
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ui::kBad, "ComfyUI is not running");
        ImGui::SameLine();
        if (ImGui::SmallButton("Start it##genstart")) {
            std::string message;
            ComfyStart(s, message);
            app.SetStatus(message);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(it also starts by itself when you press Make)");
    }
    const bool showConn = ImGui::CollapsingHeader(up ? "ComfyUI: running - your workflow, the server###comfyconn" : "ComfyUI: not running - your workflow, the server###comfyconn");
    if (showConn) {
    const ImVec2 dot = ImGui::GetCursorScreenPos();
    const float r = ImGui::GetTextLineHeight() * 0.35f;
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(dot.x + r + 2, dot.y + ImGui::GetTextLineHeight() * 0.5f), r, ImGui::GetColorU32(up ? ui::kGood : ui::kBad));
    ImGui::Dummy(ImVec2(r * 2 + 6, 0));
    ImGui::SameLine();
    if (up) {
        const std::string v = svc.Version();
        ImGui::Text("ComfyUI is running%s at %s", v.empty() ? "" : (" (" + v + ")").c_str(), s.url.c_str());
    } else {
        ImGui::Text("ComfyUI is not running at %s", s.url.c_str());
    }
    if (ComfyOwned()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(started by this app, runs in the background and closes with it)");
    }
    if (up && !ComfyOwned()) {
        // nothing to do
    } else if (ComfyOwned()) {
        if (ImGui::Button("Stop ComfyUI")) {
            ComfyStopOwned();
            app.SetStatus("Stopped the ComfyUI that this app started");
        }
    } else if (ImGui::Button("Start ComfyUI (background)")) {
        std::string message;
        ComfyStart(s, message);
        app.SetStatus(message);
    }
    if (up) {
        if (ComfyOwned()) ImGui::SameLine();
        // with a job of ours waiting or being made, ComfyUI opens on its workflow (the progress shows on its nodes): Try2Create's bridge does that
        std::string jobId, jobName;
        for (const ComfyActivity& a : svc.Activity()) {
            if (a.promptId.empty() || (a.state != ComfyState::Running && a.state != ComfyState::Queued)) continue;
            if (jobId.empty() || a.state == ComfyState::Running) jobId = a.promptId, jobName = a.label;
        }
        if (ImGui::Button(jobId.empty() ? "Open ComfyUI in the browser" : "Open it in ComfyUI (browser)")) {
            std::string base = s.url;
            while (!base.empty() && base.back() == '/') base.pop_back();
            if (jobId.empty()) {
                OpenWithWindows(ToWide(base));
            } else if (ComfyBridgeInstalled() && ComfyBridgeLoaded(s.url)) { // (installed: also brings its script up to date)
                std::string name; // the tab's name, for a URL
                for (unsigned char c : jobName.substr(0, 60)) {
                    char hex[4];
                    snprintf(hex, sizeof(hex), "%%%02X", c);
                    name += std::isalnum(c) ? std::string(1, static_cast<char>(c)) : std::string(hex);
                }
                OpenWithWindows(ToWide(base + "/?try2create=" + jobId + "&name=" + name));
            } else if (ComfyBridgeInstalled()) {
                OpenWithWindows(ToWide(base));
                app.SetStatus("Try2Create's bridge is added but this ComfyUI has not loaded it yet: restart ComfyUI once, then this opens the running workflow");
            } else {
                ImGui::OpenPopup("Open the running workflow");
            }
        }
        if (!jobId.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("Opens ComfyUI in your browser on the workflow being made now (%s),\nso you can watch it run node by node", jobName.c_str());
        if (ImGui::BeginPopupModal("Open the running workflow", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("To open the workflow that is being made, ComfyUI needs Try2Create's bridge:");
            ImGui::TextDisabled("a tiny extension with no nodes (custom_nodes\\try2create_bridge, two short files). It reads the job from\n"
                                "ComfyUI's queue and opens it with ComfyUI's own loader. ComfyUI loads it the next time it starts.");
            if (ImGui::Button("Add it")) {
                std::string message;
                ComfyInstallBridge(message);
                app.SetStatus(message);
                OpenWithWindows(ToWide(s.url));
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Just open ComfyUI")) {
                OpenWithWindows(ToWide(s.url));
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
    ImGui::TextDisabled("Every Send to ComfyUI button runs in the workflow below. If ComfyUI is not running it starts in the background, with no window.");
    ImGui::Spacing();

    // ---- 2. which workflow
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Send prompt runs:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ui::Dp(340.0f));
    if (ImGui::BeginCombo("##comfywf", CurrentWorkflowLabel(s).c_str())) {
        if (ImGui::Selectable("The workflow ComfyUI ran last", s.useLastRun)) {
            s.useLastRun = true;
            changed = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Nothing to choose: open a workflow in ComfyUI and press Queue once. Prompts sent from here then run that workflow.");
        ImGui::Separator();
        ImGui::TextDisabled("Saved in ComfyUI");
        const std::vector<ComfySavedWorkflow> list = svc.SavedWorkflows();
        if (list.empty()) ImGui::TextDisabled("%s", svc.Refreshing() ? "Reading..." : "None found (is ComfyUI running?)");
        for (const ComfySavedWorkflow& w : list) {
            if (ImGui::Selectable(WorkflowName(w.path).c_str(), !s.useLastRun && s.savedWorkflow == w.path)) {
                s.useLastRun = false;
                s.savedWorkflow = w.path;
                changed = true;
            }
        }
        ImGui::Separator();
        if (ImGui::Selectable("A workflow file on this computer...", !s.useLastRun && s.savedWorkflow.empty())) {
            s.useLastRun = false;
            s.savedWorkflow.clear();
            changed = true;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) svc.RefreshWorkflows(s);
    ImGui::SameLine();
    ImGui::BeginDisabled(!s.useLastRun && s.savedWorkflow.empty() && s.workflowPath.empty());
    if (ImGui::Button("Check it")) {
        tmpl = InspectComfyTemplate(s);
        tmplValid = true;
    }
    ImGui::EndDisabled();
    const std::string listMessage = svc.ListMessage();
    if (!listMessage.empty()) ImGui::TextDisabled("%s", listMessage.c_str());
    ImGui::TextDisabled("Picture edits (Vision board, Edit with Qwen..., Characters, Clef mode) run the built-in Qwen Image 2.1 Edit workflow with %s, up to 10 pictures.",
                        s.editUnet.empty() ? "the standard Qwen Image 2.1 model" : s.editUnet.c_str());
    if (!s.useLastRun && s.savedWorkflow.empty()) {
        ImGui::TextWrapped("%s", s.workflowPath.empty() ? "(no file chosen yet)" : ToUtf8(s.workflowPath).c_str());
        if (ImGui::Button("Choose file...")) {
            const std::wstring p = OpenWorkflowFileDialog(GetActiveWindow());
            if (!p.empty()) {
                s.workflowPath = p;
                changed = true;
            }
        }
        ImGui::SameLine();
        static std::vector<WorkflowLibItem> library;
        if (ImGui::BeginCombo("##comfylib", "From my Workflows page", ImGuiComboFlags_WidthFitPreview)) {
            if (ImGui::IsWindowAppearing()) library = ScanWorkflowLibrary();
            if (library.empty()) ImGui::TextDisabled("Nothing there yet");
            for (const WorkflowLibItem& item : library) {
                if (ImGui::Selectable((item.name + "##" + ToUtf8(item.jsonPath)).c_str())) {
                    s.workflowPath = item.jsonPath;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
    }
    if (tmplValid) {
        ImGui::PushTextWrapPos(0);
        if (tmpl.ok) {
            ImGui::TextColored(ui::kGood, "This workflow works: %d nodes", tmpl.nodes);
            ImGui::TextDisabled("%s", tmpl.summary.c_str());
        } else {
            ImGui::TextColored(ui::kBad, "%s", tmpl.error.c_str());
        }
        ImGui::PopTextWrapPos();
    }

    } // (ComfyUI's connection and workflow)

    // ---- 5. the rest (rarely needed)
    ImGui::Spacing();
    static const bool optsTest = [] { // MV_UI_TEST "comfyopts" opens it (tests)
        char buf[256] = {};
        return GetEnvironmentVariableA("MV_UI_TEST", buf, sizeof(buf)) && strstr(buf, "comfyopts") != nullptr;
    }();
    if (optsTest) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    static bool startTest = [] { // MV_UI_TEST "comfystart" starts the background server once, as its button does (tests)
        char buf[256] = {};
        return GetEnvironmentVariableA("MV_UI_TEST", buf, sizeof(buf)) && strstr(buf, "comfystart") != nullptr;
    }();
    static bool bridgeTest = [] { // MV_UI_TEST "comfybridge" adds the browser bridge once, as the popup's button does (tests)
        char buf[256] = {};
        return GetEnvironmentVariableA("MV_UI_TEST", buf, sizeof(buf)) && strstr(buf, "comfybridge") != nullptr;
    }();
    if (bridgeTest) {
        bridgeTest = false;
        std::string message;
        ComfyInstallBridge(message);
        app.SetStatus(message);
    }
    if (startTest) {
        startTest = false;
        std::string message;
        ComfyStart(s, message);
        app.SetStatus(message);
    }
    if (ImGui::CollapsingHeader("Options")) {
        if (InputStr("##comfyurl", "http://127.0.0.1:8188", s.url, ui::Dp(260.0f))) changed = true;
        ImGui::SameLine();
        ImGui::TextDisabled("ComfyUI's address");
        ImGui::SameLine();
        if (ImGui::SmallButton("Test")) checkOk = ComfyCheck(s.url, checkMessage);
        if (!checkMessage.empty()) ImGui::TextColored(checkOk ? ui::kGood : ui::kBad, "%s", checkMessage.c_str());
        if (ImGui::Checkbox("Start ComfyUI by itself when I send a prompt and it is not running", &s.autoStart)) changed = true;
        if (ImGui::Checkbox("Close the ComfyUI this app started when I close this app", &s.closeWithApp)) changed = true;
        if (ImGui::Checkbox("Copy ComfyUI's saved workflows into my Workflows page by themselves", &s.autoImport)) changed = true;
        ImGui::SetNextItemWidth(ui::Dp(260.0f));
        const char* modes[] = {"A background server (no window)", "The ComfyUI Desktop app"};
        if (ImGui::Combo("How to start it", &s.startMode, modes, 2)) changed = true;
        if (s.startMode == 1 && ImGui::Checkbox("Show the Desktop window when it starts", &s.showWindow)) changed = true;
        if (s.startMode == 0) {
            float gb = static_cast<float>(s.vramHeadroom);
            ImGui::SetNextItemWidth(ui::Dp(260.0f));
            if (ImGui::SliderFloat("Graphics memory to keep free", &gb, 0.0f, 8.0f, gb < 0.05f ? "ComfyUI's default" : "%.1f GB")) {
                s.vramHeadroom = std::round(gb * 2.0f) / 2.0;
                changed = true;
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("For the background server (ComfyUI 0.40 and newer; used the next time it starts).\n"
                                  "Big models such as MiniMax H3 otherwise fill the card to the last few hundred MB, and steps stall\n"
                                  "(2-4x slower), worse when another app or a game takes some memory. A few GB free costs almost no speed.");
            float rgb = static_cast<float>(s.ramHeadroom);
            ImGui::SetNextItemWidth(ui::Dp(260.0f));
            if (ImGui::SliderFloat("RAM to keep free", &rgb, 0.0f, 64.0f, rgb < 0.5f ? "ComfyUI's default (10 GB)" : "%.0f GB")) {
                s.ramHeadroom = std::round(rgb);
                changed = true;
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("For the background server (used the next time it starts). ComfyUI keeps models and results in RAM until only\n"
                                  "this much is free, then lets go of them between steps of a job. Its own default is 10 GB, which lets a long\n"
                                  "video job fill a 128 GB PC. More free RAM means some models load again from disk the next time.\n"
                                  "(At most a third of this PC's RAM is used.)");
        }
        if (ImGui::Checkbox("Run it behind other programs (keeps the PC smooth while it generates)", &s.background)) changed = true;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("ComfyUI started by this app gets below-normal CPU and GPU priority: the desktop, a browser playing a video and this app\n"
                              "get the graphics card first, and generation uses what is left (it is only slower while something else needs the GPU).\n"
                              "Used the next time it starts.");
        std::wstring found;
        ComfyFindDesktop(found);
        std::string exe = ToUtf8(s.desktopExe);
        if (InputStr("##comfyexe", found.empty() ? "ComfyUI Desktop program (blank = automatic)" : ToUtf8(found).c_str(), exe, ui::Dp(430.0f))) {
            s.desktopExe = ToWide(exe);
            changed = true;
        }
        const std::wstring outDir = ComfyOutputDir(s);
        std::string out = ToUtf8(s.outputDir);
        if (InputStr("##comfyout", outDir.empty() ? "ComfyUI's output folder (blank = automatic)" : ToUtf8(outDir).c_str(), out, ui::Dp(430.0f))) {
            s.outputDir = ToWide(out);
            changed = true;
        }
        ImGui::Spacing();
        ImGui::TextUnformatted("What gets sent");
        if (ImGui::Checkbox("New random seed for every run", &s.randomSeed)) changed = true;
        if (ImGui::Checkbox("Also send the picture's negative prompt (gallery pictures)", &s.sendNegative)) changed = true;
        if (ImGui::Checkbox("Also send the picture itself, into the workflow's Load Image node (img2img)", &s.sendPicture)) changed = true;
        ImGui::TextDisabled("Video prompts always send their picture as the first frame.");
        if (ImGui::Checkbox("Video workflows: set the clip length from the prompt's seconds", &s.videoLength)) changed = true;
        if (s.videoLength) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ui::Dp(100.0f));
            if (ImGui::InputInt("fps if the workflow has none", &s.defaultFps, 0, 0)) {
                s.defaultFps = std::clamp(s.defaultFps, 1, 120);
                changed = true;
            }
        }
        ImGui::SetNextItemWidth(ui::Dp(200.0f));
        if (ImGui::SliderInt("Pictures per click (the Vision library has its own)", &s.count, 1, 8)) changed = true;
        ImGui::Spacing();
        ImGui::TextUnformatted("Picture editing (Qwen Image 2.1 Edit)");
        {
            ImGui::SetNextItemWidth(ui::Dp(430.0f));
            if (ImGui::BeginCombo("Board scenes: how freely to change the characters##optstyle", EditStyleName(s.editStyle))) {
                for (int m = 0; m <= 2; ++m) {
                    if (ImGui::Selectable(EditStyleName(m), s.editStyle == m)) {
                        s.editStyle = m;
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
        }
        ImGui::TextUnformatted("Board scenes: tell the model to change");
        ImGui::SameLine();
        if (ImGui::Checkbox("outfits##optout", &s.directOutfit)) changed = true;
        ImGui::SameLine();
        if (ImGui::Checkbox("poses##optpose", &s.directPose)) changed = true;
        ImGui::SameLine();
        if (ImGui::Checkbox("expressions##optexp", &s.directExpression)) changed = true;
        if (ImGui::Checkbox("Go to the Edits tab each time I send an edit", &s.editJump)) changed = true;
        if (DrawEditModelPicker(s, "optmodel")) changed = false; // saved by the picker itself
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        if (ImGui::TreeNode("LoRAs: which ones load on top of the model, at what strength")) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("Each list is loaded in order, on the model only (files of ComfyUI's models/loras). Untick one to keep it in the list without loading it.");
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            ImGui::TextUnformatted("Picture edits (Qwen: the edit window, Vision board scenes, Clef mode)");
            DrawLoraList(s, s.editLoras, "loraedit");
            ImGui::Spacing();
            ImGui::TextUnformatted("The ComfyUI workflow chosen above (Krea or any other: Send prompt, Clef mode text prompts)");
            DrawLoraList(s, s.promptLoras, "loraprompt");
            if (ImGui::Checkbox("Take out the LoRAs that workflow already has first##lorareplace", &s.promptReplaceLoras)) changed = true;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("Bypasses the workflow's own LoRA loader nodes (the standard ones), so only the list above is loaded.\nUntick it to add these on top of the workflow's own.");
            ImGui::TextDisabled("They are put on the model that feeds the sampler, whatever the workflow looks like.");
            ImGui::Spacing();
            ImGui::TextUnformatted("Edits > Upscale: the MOSS-Image redraw pass (MOSS LoRAs only)");
            DrawLoraList(s, s.upscaleLoras, "loraupscale");
            ImGui::TreePop();
        }
        ImGui::SetNextItemWidth(ui::Dp(200.0f));
        if (ImGui::SliderInt("Steps##editsteps", &s.editSteps, 4, 60)) changed = true;
        ImGui::SetNextItemWidth(ui::Dp(200.0f));
        if (ImGui::SliderInt("Size (pixels, 0 = like the first picture)##editres", &s.editResolution, 0, 2048)) changed = true;
        ImGui::TextDisabled("If the prompt goes to the wrong node, enter its id (the # badge in ComfyUI):");
        if (InputStr("##comfyprompt", "prompt node id (blank = automatic)", s.promptNode, ui::Dp(260.0f))) changed = true;
        if (InputStr("##comfyneg", "negative prompt node id (blank = automatic)", s.negativeNode, ui::Dp(260.0f))) changed = true;
        ImGui::TextDisabled("The background server's log: %%LOCALAPPDATA%%\\Try2Create\\comfy_server.log");
    }
    // ---- 3. what happened to everything sent
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    static int hiddenUpTo = 0; // "Clear the finished ones": finished sends up to this one are not listed (the service keeps them: pages still read them)
    std::vector<ComfyActivity> activity = svc.Activity();
    activity.erase(std::remove_if(activity.begin(), activity.end(), [](const ComfyActivity& a) {
                       return a.id <= hiddenUpTo && (a.state == ComfyState::Done || a.state == ComfyState::Failed);
                   }),
                   activity.end());
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("What you sent");
    const bool anyFinished = std::any_of(activity.begin(), activity.end(), [](const ComfyActivity& a) { return a.state == ComfyState::Done || a.state == ComfyState::Failed; });
    if (anyFinished) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear the finished ones")) {
            for (const ComfyActivity& a : activity) hiddenUpTo = std::max(hiddenUpTo, a.id);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Takes the done and failed ones off this list (what is still running stays)");
    }
    if (activity.empty()) {
        ImGui::TextDisabled("Nothing yet: write what to make above and press Make (or Send to ComfyUI on a picture, a Vision library item or the Vision board).");
    } else {
        ImGui::BeginChild("##comfyactivity", ImVec2(0, ui::Dp(130.0f)), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
        for (size_t i = activity.size(); i-- > 0;) {
            const ComfyActivity& a = activity[i];
            ImGui::PushID(a.id);
            ImGui::TextColored(StateColor(a.state), "%s", StateName(a.state));
            ImGui::SameLine();
            ImGui::TextUnformatted(a.label.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("  %s", Elapsed(a).c_str());
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(a.state == ComfyState::Failed ? ui::kBad : ui::kTextDim, "%s", a.message.c_str());
            if (a.state != ComfyState::Failed && a.state != ComfyState::Done) ImGui::TextDisabled("in %s, at %s", a.workflow.c_str(), a.address.c_str());
            ImGui::PopTextWrapPos();
            if (a.state == ComfyState::Running) DrawComfyJobProgress(a.id);
            if (a.state != ComfyState::Failed && a.state != ComfyState::Done) {
                if (ImGui::SmallButton(a.state == ComfyState::Running ? "Stop" : "Cancel")) svc.Cancel(a.id);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                    ImGui::SetTooltip(a.state == ComfyState::Running ? "Interrupts this run only (someone else's run in the same ComfyUI keeps going)" : "Takes it out before it runs");
            }
            if (a.state == ComfyState::Failed && ImGui::SmallButton("Copy the error")) ImGui::SetClipboardText(a.message.c_str());
            ImGui::Separator();
            ImGui::PopID();
        }
        ImGui::EndChild();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && ImGui::GetMousePos().y > ImGui::GetItemRectMax().y - ui::Dp(6.0f))
            ImGui::SetTooltip("Drag the bottom edge to make this list taller or shorter");
    }

    ImGui::PopTextWrapPos();
    if (changed) {
        SaveComfySettings(s);
        tmplValid = false;
    }
    ImGui::EndChild(); // (the left column)
    ImGui::TableSetColumnIndex(1);
    DrawResultsViewer(app, svc.Results(), height);
    ImGui::EndTable();
}

// Tested 2026-10-17: a 10 s 768p swap went from 72 s a step to 467 s once the PC ran out of RAM (ComfyUI held 38 GB of 64; Windows was paging the
// model to disk) - and nothing on screen said whether it was still working. This says where it is, and why it is slow.
void DrawComfyJobProgress(int activityId) {
    static std::map<int, double> quickest; // each job's quickest step so far
    std::string line;
    bool slow = false;
    ComfyProgress pr;
    if (ComfyOwned() && ComfyReadProgress(pr) && pr.steps > 0) {
        auto mmss = [](double s) {
            char b[32];
            const int t = static_cast<int>(std::lround(std::max(0.0, s)));
            if (t >= 60) snprintf(b, sizeof(b), "%d min %02d s", t / 60, t % 60);
            else snprintf(b, sizeof(b), "%d s", t);
            return std::string(b);
        };
        double& q = quickest[activityId];
        if (pr.secPerStep > 0 && pr.step >= 1 && (q <= 0 || pr.secPerStep < q)) q = pr.secPerStep;
        if (pr.step >= pr.steps) line = "all " + std::to_string(pr.steps) + " steps done: decoding the video...";
        else {
            line = std::to_string(pr.step) + " of " + std::to_string(pr.steps) + " steps done";
            if (pr.secPerStep > 0) line += ", " + mmss(pr.secPerStep) + " a step";
            if (pr.leftSeconds > 0) line += ", about " + mmss(pr.leftSeconds) + " left";
        }
        slow = q > 0 && pr.step >= 2 && pr.secPerStep > 3.0 * q;
        if (slow) {
            char b[96];
            snprintf(b, sizeof(b), " (its first steps took %.0f s)", q);
            line += b;
        }
    }
    if (!line.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(slow ? ui::kWarn : ui::kTextDim, "%s", line.c_str());
        ImGui::PopTextWrapPos();
    }
    const double freeGb = FreeRamGB();
    if (freeGb > 0 && freeGb < 3.0) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ui::kWarn, "The PC is nearly out of memory (%.1f GB of RAM free): the model is being moved to and from the disk, which makes each step "
                                      "several times slower. It will finish; next time 640p or a shorter part is much quicker.", freeGb);
        ImGui::PopTextWrapPos();
    } else if (slow) {
        ImGui::TextDisabled("Something else may be using the graphics card (a game?), or the job is too big for its memory.");
    }
}

void EnsureComfyService(AppState& app) { EnsureComfy(app); }

// The page a job belongs on, from its label (the tracker's "Show it" button): videos, songs, drama, edits... else Generate
int ComfyJobTab(const std::string& label) {
    auto starts = [&](std::initializer_list<const char*> ps) {
        for (const char* p : ps) {
            if (label.rfind(p, 0) == 0) return true;
        }
        return false;
    };
    if (starts({"Video", "Long video", "Crisp", "Swap", "Star", "Story", "360", "Recreate", "Faces", "Transition", "Background"})) return 10;
    if (starts({"Music", "Song", "Cover"})) return 11;
    if (starts({"Drama"})) return 12;
    if (starts({"Upscale", "Edit", "MOSS", "Qwen", "App", "Cut out", "Layout", "Design"})) return 6;
    if (starts({"Character"})) return 5;
    if (starts({"Pixel"})) return 9;
    if (starts({"Clef"})) return 8;
    if (starts({"Vision board"})) return 3;
    return 4;
}

// The status bar's ComfyUI tracker, on every page (the user, 2026-10-28: "an easier to follow progress tracker all across the map for jobs ...
// instead of having to switch to the generate tab"): the job running now - its name, step, time a step and time left, or how long it has been
// going - and how many wait. Click: all of them, each with Stop / Cancel and its page. Draws nothing (and returns false) when nothing is on.
bool DrawComfyTracker(AppState& app, float width) {
    if (!app.comfySvc) return false;
    const std::vector<ComfyActivity> acts = app.comfySvc->Activity();
    const ComfyActivity* run = nullptr;
    int waiting = 0;
    for (const ComfyActivity& a : acts) {
        if (a.state == ComfyState::Running) run = run ? run : &a;
        else if (a.state != ComfyState::Done && a.state != ComfyState::Failed) ++waiting;
    }
    if (!run && !waiting) return false;
    const long long now = ComfyService::NowMs();
    auto mmss = [](long long s) {
        char b[32];
        s = std::max(0LL, s);
        if (s >= 3600) snprintf(b, sizeof(b), "%lld h %02lld min", s / 3600, (s / 60) % 60);
        else if (s >= 60) snprintf(b, sizeof(b), "%lld:%02lld", s / 60, s % 60);
        else snprintf(b, sizeof(b), "%lld s", s);
        return std::string(b);
    };
    std::string text;
    float frac = -1.0f; // (< 0: no steps known - a moving bar)
    if (run) {
        ComfyProgress pr;
        const bool steps = ComfyOwned() && ComfyReadProgress(pr) && pr.steps > 0;
        const long long secs = (now - (run->runningMs > 0 ? run->runningMs : run->startedMs)) / 1000;
        std::string name = run->label;
        if (steps && pr.step >= pr.steps) text = "decoding";
        else if (steps) {
            text = "step " + std::to_string(std::min(pr.step + 1, pr.steps)) + "/" + std::to_string(pr.steps);
            if (pr.leftSeconds > 0) text += ", ~" + mmss(static_cast<long long>(pr.leftSeconds)) + " left";
            frac = static_cast<float>(pr.step) / pr.steps;
        } else text = mmss(secs);
        if (steps) text += " (" + mmss(secs) + ")";
        text = name + ": " + text;
    } else {
        text = "Waiting for ComfyUI";
    }
    if (waiting) text += "  +" + std::to_string(waiting) + " waiting";
    ImGui::PushID("comfytracker");
    const ImVec2 size(width, ImGui::GetFrameHeight() * 0.8f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (frac < 0.0f) ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), size, "");
    else ImGui::ProgressBar(frac, size, "");
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked();
    { // the text over the bar, cut to fit with "..."
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float pad = ui::Dp(6.0f);
        const ImVec2 t0(p0.x + pad, p0.y + (size.y - ImGui::GetFontSize()) * 0.5f);
        ImGui::RenderTextEllipsis(dl, t0, ImVec2(p0.x + size.x - pad, p0.y + size.y), p0.x + size.x - pad, p0.x + size.x - pad, text.c_str(), nullptr, nullptr);
    }
    if (hovered) ImGui::SetTooltip("%s\n\nClick: every job, with Stop and its page", text.c_str());
    if (clicked) ImGui::OpenPopup("##jobs");
    if (ImGui::BeginPopup("##jobs")) {
        ImGui::TextUnformatted("ComfyUI jobs");
        ImGui::Separator();
        int shown = 0;
        for (const ComfyActivity& a : acts) {
            if (a.state == ComfyState::Done || a.state == ComfyState::Failed) continue;
            ImGui::PushID(a.id);
            const long long secs = (now - (a.runningMs > 0 ? a.runningMs : a.startedMs)) / 1000;
            ImGui::TextColored(a.state == ComfyState::Running ? ui::kAccentHover : ui::kTextDim, "%s", a.state == ComfyState::Running ? "Running" : "Waiting");
            ImGui::SameLine();
            ImGui::TextUnformatted(a.label.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s", mmss(secs).c_str());
            if (a.state == ComfyState::Running) DrawComfyJobProgress(a.id);
            if (ImGui::SmallButton(a.state == ComfyState::Running ? "Stop" : "Cancel")) app.comfySvc->Cancel(a.id);
            ImGui::SameLine();
            if (ImGui::SmallButton("Show it")) {
                app.requestTab = ComfyJobTab(a.label);
                ImGui::CloseCurrentPopup();
            }
            ImGui::Separator();
            ImGui::PopID();
            if (++shown >= 12) {
                ImGui::TextDisabled("...and more on the Generate page");
                break;
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return true;
}

// Green dot: ComfyUI answers at the configured address. Red: it does not. Click: the ComfyUI tab.
void DrawComfyStatusDot(AppState& app) {
    EnsureComfy(app);
    const bool up = app.comfySvc->Reachable();
    const int busy = app.comfySvc->Active();
    const float h = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##comfydot", ImVec2(ui::Dp(30.0f), h));
    const bool hot = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p.x + ui::Dp(15.0f), p.y + h * 0.5f);
    const ImVec4 col = !up ? ui::kBad : (busy > 0 ? ui::kWarn : ui::kGood);
    if (hot) dl->AddCircleFilled(c, ui::Dp(11.0f), IM_COL32(255, 255, 255, 22), 24);
    if (up) dl->AddCircleFilled(c, ui::Dp(9.0f), ImGui::GetColorU32(ImVec4(col.x, col.y, col.z, 0.22f)), 24);
    dl->AddRect(ImVec2(c.x - ui::Dp(5.5f), c.y - ui::Dp(5.5f)), ImVec2(c.x + ui::Dp(5.5f), c.y + ui::Dp(5.5f)), ImGui::GetColorU32(col), ui::Dp(2.0f), 0, ui::Dp(2.0f)); // a square: not the vision dot
    dl->AddRectFilled(ImVec2(c.x - ui::Dp(3.0f), c.y - ui::Dp(3.0f)), ImVec2(c.x + ui::Dp(3.0f), c.y + ui::Dp(3.0f)), ImGui::GetColorU32(col), ui::Dp(1.0f));
    if (hot) {
        ImGui::BeginTooltip();
        ImGui::Text("ComfyUI: %s", app.comfy.url.c_str());
        ImGui::TextColored(col, "%s", !up ? "Not running (it starts by itself when you send a prompt)" : (busy > 0 ? "Generating..." : "Running"));
        ImGui::TextDisabled("Click to open the Generate page");
        ImGui::EndTooltip();
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) app.requestTab = 4;
}

// ---------------------------------------------------------------------------------------------------------------
// "Edit and send": a window for changing a prompt before it goes to ComfyUI, from anywhere. It works on a COPY of the text:
// the picture's own metadata, its Vision results and the board stay exactly as they are.
namespace {
struct SendEditor {
    bool open = false;
    std::string original, prompt, negative, label;
    std::wstring picture;
    double seconds = 0;
    int times = 1;
    bool useNegative = false, usePicture = false;
    bool focus = false;
};
SendEditor g_editor;
} // namespace

void OpenSendEditor(AppState& app, const std::string& prompt, const std::string& negative, const std::wstring& picture, const std::string& label, double seconds) {
    EnsureComfy(app);
    SendEditor& e = g_editor;
    e.open = true;
    e.focus = true;
    e.original = prompt;
    e.prompt = prompt;
    e.negative = negative;
    e.label = label;
    e.picture = picture;
    e.seconds = seconds;
    e.times = std::clamp(app.comfy.count, 1, 100);
    e.useNegative = app.comfy.sendNegative && !negative.empty();
    e.usePicture = seconds > 0 || app.comfy.sendPicture; // a video prompt's picture is its first frame
}

// "Check the prompt with Clef": rates a prompt before any generation time is spent on it
void DrawPromptTriage(AppState& app, const std::string& prompt, const char* id) {
    static std::string forPrompt, text;
    static bool good = false;
    std::string p, t;
    bool g = false;
    if (ClefRatePromptTake(p, t, g)) {
        forPrompt = p;
        text = t;
        good = g;
    }
    const bool busy = ClefRatePromptRunning();
    ImGui::BeginDisabled(busy || prompt.find_first_not_of(" \t\r\n") == std::string::npos);
    if (ImGui::SmallButton((std::string(busy ? "Clef is reading it..." : "Check the prompt with Clef") + "##" + id).c_str())) ClefRatePromptStart(prompt, app.comfy.url);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("Clef rates the prompt before you spend generation time on it: how specific it is, whether it contradicts itself, whether it asks for too much.\nIt loads for a moment (taking the GPU from ComfyUI) and is closed again when ComfyUI needs the GPU.");
    if (!text.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(forPrompt == prompt ? (good ? ui::kGood : ui::kWarn) : ui::kTextDim, "%s", forPrompt == prompt ? "" : "(for an earlier version of the text)");
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(forPrompt == prompt ? (good ? ui::kGood : ui::kWarn) : ui::kTextDim, "%s", text.c_str());
        ImGui::PopTextWrapPos();
    }
}

void DrawSendEditor(AppState& app) {
    SendEditor& e = g_editor;
    if (!e.open) return;
    EnsureComfy(app);
    ImGui::SetNextWindowSize(ImVec2(ui::Dp(640.0f), ui::Dp(620.0f)), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (e.focus) {
        ImGui::SetNextWindowFocus();
        e.focus = false;
    }
    if (!ImGui::Begin("Edit and send to ComfyUI", &e.open)) {
        ImGui::End();
        return;
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("From:");
    ImGui::SameLine();
    ImGui::TextUnformatted(e.label.c_str());
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("This is a copy: the picture's real prompt (its metadata, its Vision result, the board) is not changed.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    const float lineH = ImGui::GetFrameHeightWithSpacing();
    const float rest = ImGui::GetContentRegionAvail().y - lineH * 9.0f;
    const bool showNeg = !e.negative.empty() || e.useNegative;
    const float promptH = std::max(ui::Dp(100.0f), showNeg ? rest * 0.65f : rest);
    ImGui::TextUnformatted("Prompt");
    WrapEdit("##edprompt", e.prompt, promptH);
    if (showNeg) {
        ImGui::TextUnformatted("Negative prompt");
        WrapEdit("##edneg", e.negative, std::max(ui::Dp(50.0f), rest - promptH - lineH));
    }
    ImGui::Spacing();
    if (!e.negative.empty()) ImGui::Checkbox("Also send the negative prompt", &e.useNegative);
    if (!e.picture.empty()) {
        if (e.seconds > 0) {
            ImGui::TextDisabled("Video prompt (%.0f s): the picture goes in as the first frame.", e.seconds);
        } else {
            ImGui::Checkbox("Also send the picture itself, into the workflow's Load Image node", &e.usePicture);
        }
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Workflow:");
    ImGui::SameLine();
    if (ImGui::Button((ComfyWorkflowLabel(app) + "  v").c_str())) ImGui::OpenPopup("##edwf");
    if (ImGui::BeginPopup("##edwf")) {
        ComfyWorkflowMenu(app);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ui::Dp(110.0f));
    ImGui::InputInt("times##edtimes", &e.times, 1, 5);
    e.times = std::clamp(e.times, 1, 100);
    DrawPromptTriage(app, e.prompt, "sendedit");
    ImGui::Spacing();

    const bool changed = e.prompt != e.original;
    const bool empty = e.prompt.find_first_not_of(" \t\r\n") == std::string::npos;
    ImGui::BeginDisabled(empty);
    auto send = [&]() {
        const std::string label = e.label + (changed ? " (edited)" : "");
        return SendToComfy(app, e.prompt, e.useNegative ? e.negative : std::string(), (e.usePicture || e.seconds > 0) ? e.picture : std::wstring(), label, false, e.seconds, e.times);
    };
    if (ui::PrimaryButton("Send", ImVec2(ui::Dp(110.0f), 0)) ||
        (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Enter)))
        send();
    ImGui::SameLine();
    if (ImGui::Button("Send and close")) {
        if (send()) e.open = false;
    }
    if (e.seconds > 0 && !H3TurboWorkflow(!e.picture.empty()).empty()) { // a video prompt: the 4-step video model, whatever the workflow above is
        ImGui::SameLine();
        if (ImGui::Button("Make with H3 Turbo")) {
            if (SendH3Turbo(app, e.picture, e.prompt, e.seconds, e.label + (changed ? " (edited)" : ""), e.times)) e.open = false;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("MiniMax H3 Turbo: a %.0f-second video with sound in 4 steps%s", std::clamp(e.seconds, 3.0, 15.0), e.picture.empty() ? "" : ", the picture as its first frame");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!changed);
    if (ImGui::Button("Back to the original")) e.prompt = e.original;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Copy")) {
        ImGui::SetClipboardText(e.prompt.c_str());
        app.SetStatus("Copied the prompt");
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------------------------------------------
// Picture editing with Qwen Image 2.1 Edit: up to 10 pictures (the first is edited, the others are references) and an instruction that calls them
// <image1>, <image2>... Used by the Vision board ("make the picture from the cards"), the gallery, the Vision library, the Characters tab and
// the ComfyUI viewer.
bool WrapEdit(const char* id, std::string& text, float height) {
    static std::map<ImGuiID, int> mode; // 0 = decide from the length, 1 = reading, 2 = editing
    const ImGuiID key = ImGui::GetID(id);
    int& m = mode[key];
    const bool editing = m == 2 || (m == 0 && text.size() <= 110 && text.find('\n') == std::string::npos);
    bool changed = false;
    const float bar = ImGui::GetFrameHeightWithSpacing();
    if (!editing) {
        ImGui::BeginChild(id, ImVec2(-FLT_MIN, std::max(ui::Dp(40.0f), height - bar)), true);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopTextWrapPos();
        const bool dbl = ImGui::IsWindowHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        ImGui::EndChild();
        if (dbl || ImGui::SmallButton((std::string("Edit the text##") + id).c_str())) m = 2;
        ImGui::SameLine();
        ImGui::TextDisabled("%d characters - double click the text to edit it", static_cast<int>(text.size()));
    } else {
        const bool longText = text.size() > 110 || text.find('\n') != std::string::npos;
        changed = ImGui::InputTextMultiline(id, text.data(), text.capacity() + 1, ImVec2(-FLT_MIN, std::max(ui::Dp(40.0f), height - (longText ? bar : 0.0f))),
                                            ImGuiInputTextFlags_CallbackResize, StringResize, &text);
        if (longText) {
            if (ImGui::SmallButton((std::string("Done - show it all##") + id).c_str())) m = 1;
            ImGui::SameLine();
            ImGui::TextDisabled("while editing, long lines scroll sideways");
        }
    }
    return changed;
}

std::string FinalPromptClause(const std::string& finalPrompt) { return "Scene description (the written final prompt of the board): " + finalPrompt; }

std::string EditStyleClause(int mode) {
    if (mode == 1) {
        return "Shoot it like a spontaneous candid photograph, not a cut-and-paste: every character stays instantly recognizable by their face, hair and body type, but their "
               "clothes, pose and expression are NEW and come from the moment, not from the reference pictures. Let them really interact (a hand on a shoulder, leaning in, "
               "laughing, overlapping, eye contact). Harmonize lighting, colour, depth of field and grain across everyone, with soft believable shadows and contact points, "
               "as if a single camera captured it.";
    }
    if (mode == 2) {
        return "Re-imagine every character for this scene while keeping who they are: keep their face and facial features, body shape and proportions, hair and their signature "
               "visual style and character design (the same design language, colour palette and aesthetic). Everything else belongs to the scene: a new pose and posture, a new "
               "expression and gaze, outfit details adapted to the place and weather, and lighting, colour grade and rendering shared by the whole image so nothing looks pasted.";
    }
    return {};
}

bool DrawLoraList(ComfySettings& s, std::vector<LoraEntry>& list, const char* id) {
    static std::vector<std::string> choices; // what ComfyUI offers, fetched in the background
    static std::future<std::vector<std::string>> fetch;
    static bool asked = false;
    if (fetch.valid() && fetch.wait_for(std::chrono::seconds(0)) == std::future_status::ready) choices = fetch.get();
    if (!asked && !fetch.valid()) { // the first time the list is shown (and again with the refresh button)
        asked = true;
        fetch = std::async(std::launch::async, [url = s.url]() { return ComfyListLoras(url); });
    }
    bool changed = false;
    ImGui::PushID(id);
    int remove = -1;
    for (size_t i = 0; i < list.size(); ++i) {
        LoraEntry& l = list[i];
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Checkbox("##on", &l.on)) changed = true;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ui::Dp(250.0f));
        if (InputStr("##name", "LoRA file (models/loras)", l.name, ui::Dp(250.0f))) changed = true;
        ImGui::SameLine();
        if (ImGui::SmallButton("v")) ImGui::OpenPopup("##pick");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Choose one of ComfyUI's LoRAs");
        if (ImGui::BeginPopup("##pick")) {
            if (choices.empty()) ImGui::TextDisabled("ComfyUI did not give a list (is it running?). Type the file name.");
            ImGui::BeginChild("##picklist", ImVec2(ui::Dp(380.0f), ui::Dp(260.0f)), false);
            for (const std::string& n : choices) {
                if (ImGui::Selectable(n.c_str(), n == l.name)) {
                    l.name = n;
                    changed = true;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndChild();
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ui::Dp(150.0f));
        float st = static_cast<float>(l.strength);
        if (ImGui::SliderFloat("##str", &st, -1.0f, 2.0f, "strength %.2f")) {
            l.strength = std::round(st * 100.0f) / 100.0f;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) remove = static_cast<int>(i);
        ImGui::PopID();
    }
    if (remove >= 0) {
        list.erase(list.begin() + remove);
        changed = true;
    }
    if (ImGui::SmallButton("+ Add a LoRA")) {
        list.push_back(LoraEntry{});
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Add from a file...")) {
        wchar_t buf[MAX_PATH * 2] = {};
        OPENFILENAMEW ofn = {sizeof(ofn)};
        ofn.hwndOwner = GetActiveWindow();
        ofn.lpstrFilter = L"LoRA files\0*.safetensors;*.pt;*.ckpt\0All files\0*.*\0\0";
        ofn.lpstrFile = buf;
        ofn.nMaxFile = MAX_PATH * 2;
        ofn.lpstrTitle = L"Choose a LoRA file (inside ComfyUI's models/loras)";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&ofn)) { // the name ComfyUI knows it by: what comes after "loras\\"
            std::string full = ToUtf8(buf), low = full;
            for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            const size_t at = low.rfind("\\loras\\");
            std::string name = at != std::string::npos ? full.substr(at + 7) : ToUtf8(fs::path(buf).filename().wstring());
            LoraEntry l;
            l.name = name;
            list.push_back(l);
            changed = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh ComfyUI's list") && !fetch.valid()) fetch = std::async(std::launch::async, [url = s.url]() { return ComfyListLoras(url); });
    ImGui::PopID();
    if (changed) SaveComfySettings(s);
    return changed;
}

bool DrawEditModelPicker(ComfySettings& s, const char* id) {
    struct Choice {
        const char* label;
        const char* file;
    };
    static const Choice choices[] = {{"Qwen Image 2.1 Turbo Q8 (GGUF, 4-8 steps)", "qwen_image_2.1_turbo_Q8_0.gguf"},
                                     {"The standard Qwen Image 2.1 (found automatically)", ""}};
    const int nChoices = 2;
    int cur = -1;
    for (int i = 0; i < nChoices; ++i) {
        if (s.editUnet == choices[i].file) cur = i;
    }
    const std::string preview = cur >= 0 ? choices[cur].label : "Other file: " + s.editUnet;
    bool changed = false;
    // up to 360 wide, but the label after it always stays visible in a narrow panel
    const float labelW = ImGui::CalcTextSize("Model for edits").x + ImGui::GetStyle().ItemInnerSpacing.x;
    ImGui::SetNextItemWidth(std::max(ui::Dp(160.0f), std::min(ui::Dp(360.0f), ImGui::GetContentRegionAvail().x - labelW)));
    if (ImGui::BeginCombo((std::string("Model for edits##") + id).c_str(), preview.c_str())) {
        for (int i = 0; i < nChoices; ++i) {
            if (ImGui::Selectable(choices[i].label, cur == i)) {
                const bool wasTurbo = IsTurboModel(s.editUnet);
                s.editUnet = choices[i].file;
                if (IsTurboModel(s.editUnet) != wasTurbo) s.editSteps = IsTurboModel(s.editUnet) ? 6 : 25; // turbo models want 4-8 steps, full ones 25+
                changed = true;
            }
        }
        ImGui::Separator();
        ImGui::TextDisabled("Or type another file of models/diffusion_models:");
        static std::string custom;
        if (ImGui::IsWindowAppearing()) custom = cur < 0 ? s.editUnet : std::string();
        if (InputStr((std::string("##custommodel") + id).c_str(), "file name, e.g. my_model.safetensors", custom, ui::Dp(330.0f)) && !custom.empty()) {
            s.editUnet = custom;
            changed = true;
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("The Qwen model that picture edits load (Edit window, Vision board scenes, swaps, Clef mode).\nIf ComfyUI does not list the file, the edit stops with a message instead of using another model.");
    if (changed) SaveComfySettings(s);
    return changed;
}

DirectorFlags DirectorFlagsFor(const ComfySettings& s, int styleMode) {
    DirectorFlags f;
    f.outfit = s.directOutfit;
    f.pose = s.directPose;
    f.expression = s.directExpression;
    f.adaptOutfit = styleMode == 2;
    return f;
}

const char* EditStyleName(int mode) {
    return mode == 1 ? "Nano banana style: new outfits, poses and expressions, loose and natural" : mode == 2 ? "Fit everything to the scene (keep face, body and style)" : "As asked (nothing extra)";
}

// ---------------------------------------------------------------------------------------------------------------
// How long an edit takes on this computer. The work grows with the steps and the pixels (and with every reference picture the model reads), so each
// finished edit gives a "seconds per unit of work"; the estimate is the median of the last few of those, a rough guess before there are any.
namespace {
std::vector<double> g_secPerWork;
bool g_timingLoaded = false;

fs::path TimingPath() { return fs::path(BoardsDir()).parent_path() / L"edit_timing.json"; }

void LoadTiming() {
    if (g_timingLoaded) return;
    g_timingLoaded = true;
    std::ifstream f(TimingPath(), std::ios::binary);
    if (!f) return;
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    size_t pos = 0;
    while (pos < text.size()) { // a plain list of numbers, one per line
        const size_t end = text.find('\n', pos);
        const double v = std::atof(text.substr(pos, end == std::string::npos ? std::string::npos : end - pos).c_str());
        if (v > 0.01 && v < 1000) g_secPerWork.push_back(v);
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    while (g_secPerWork.size() > 9) g_secPerWork.erase(g_secPerWork.begin());
}

double Median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

double SecPerWork() {
    LoadTiming();
    return g_secPerWork.empty() ? 0.9 : Median(g_secPerWork);
}
} // namespace

double EditWorkUnits(int steps, int res, int nImages, int canvas) {
    const double mp = std::pow(static_cast<double>(res == 0 ? 1024 : std::max(256, res)), 2.0) / 1e6;
    const int refs = canvas == 0 ? std::max(0, nImages - 1) : std::max(0, nImages); // with a new canvas every picture is a reference
    return steps * mp * (1.0 + 0.5 * refs);
}

void RecordEditRun(int steps, int res, int nImages, int canvas, int times, double generatingSeconds) {
    LoadTiming();
    const double perPicture = generatingSeconds / std::max(1, times);
    double k = std::max(0.02, (perPicture - 4.0) / std::max(0.05, EditWorkUnits(steps, res, nImages, canvas)));
    if (g_secPerWork.size() >= 3 && k > 4.0 * Median(g_secPerWork)) return; // the first run also loads the models: not typical
    g_secPerWork.push_back(k);
    while (g_secPerWork.size() > 9) g_secPerWork.erase(g_secPerWork.begin());
    std::error_code ec;
    fs::create_directories(TimingPath().parent_path(), ec);
    std::ofstream out(TimingPath(), std::ios::binary | std::ios::trunc);
    for (double v : g_secPerWork) out << v << "\n";
}

double EstimateEditSeconds(int steps, int res, int nImages, int canvas) { return 4.0 + SecPerWork() * EditWorkUnits(steps, res, nImages, canvas); }

std::string EditEstimateSource() {
    LoadTiming();
    if (g_secPerWork.empty()) return "a rough guess: no edit has been timed on this PC yet, so it will learn from your first runs";
    return "measured on this PC, from your last " + std::to_string(g_secPerWork.size()) + (g_secPerWork.size() == 1 ? " run" : " runs") + " (the first run after ComfyUI starts is slower: it loads the models)";
}

std::string FormatEditTime(double seconds) {
    const int s = static_cast<int>(std::lround(seconds));
    if (s < 60) return std::to_string(std::max(1, s)) + " s";
    if (s < 600) return std::to_string(s / 60) + " min" + (s % 60 >= 5 ? " " + std::to_string(s % 60) + " s" : std::string());
    return "about " + std::to_string((s + 30) / 60) + " min";
}

int LastEditActivity() { return g_lastEditActivity; }

bool SendPictureEdit(AppState& app, const std::vector<std::wstring>& images, const std::string& instruction, const std::string& label, int times, const EditContext* ctx) {
    EnsureComfy(app);
    if (images.empty()) {
        app.SetStatus("There is no picture to edit");
        return false;
    }
    if (instruction.find_first_not_of(" \t\r\n") == std::string::npos) {
        app.SetStatus("Say what to change first");
        return false;
    }
    ComfyJob job;
    job.prompt = instruction;
    job.label = label;
    job.images.assign(images.begin(), images.begin() + std::min<size_t>(images.size(), 10));
    job.times = times;
    job.canvas = ctx ? std::clamp(ctx->canvas, 0, 6) : 0;
    if (!ClefGradeRunning()) LlmStopOwned(); // ComfyUI needs the GPU: a Clef loaded for something else gives it back
    CrashNote("edit: sending " + std::to_string(job.images.size()) + " pictures to Qwen Image 2.1 Edit");
    const std::vector<std::wstring> sent = job.images;
    const int activity = app.comfySvc->Send(app.comfy, std::move(job));
    g_lastEditActivity = activity;
    RegisterEdit(app, activity, label, instruction, sent, ctx, std::max(1, times));
    if (ctx && ctx->pixelArt && activity > 0) PixelArtAfter(app, activity, ctx->pixelGroup.empty() ? std::string("Edits") : ctx->pixelGroup, label, ctx->pixelGroup != "Clef mode");
    if (app.comfy.editJump) {
        app.requestTab = 6;
        if (EditsMode() >= 5) SetEditsMode(0); // (on the start screen or Videos: to the edits list, where it shows)
    }
    app.SetStatus("Sent \"" + label + "\" to ComfyUI for editing (Qwen Image 2.1 Edit): see it in the Edits tab");
    return true;
}

// ComfyUI is about to need the GPU: a Clef that this app loaded for something else gives it back
static void ClefGivesGpuBack() {
    if (!ClefGradeRunning()) LlmStopOwned();
}

int SendUpscale(AppState& app, ComfyJob job) {
    EnsureComfy(app);
    ClefGivesGpuBack();
    if (job.images.empty()) {
        app.SetStatus("There is no picture to upscale");
        return 0;
    }
    CrashNote(std::string("upscale: sending ") + (job.upscale == 2 ? "faces" : "whole picture"));
    return app.comfySvc->Send(app.comfy, std::move(job));
}

int SendSheetEdit(AppState& app, ComfyJob job) {
    EnsureComfy(app);
    ClefGivesGpuBack();
    CrashNote("pixel art: sending a Qwen sprite sheet (" + std::to_string(job.sheetW) + " x " + std::to_string(job.sheetH) + ")");
    return app.comfySvc->Send(app.comfy, std::move(job));
}

int SendComfyJob(AppState& app, ComfyJob job) {
    EnsureComfy(app);
    ClefGivesGpuBack(); // the LLMs this app loaded give the GPU back
    CrashNote("sending \"" + job.label + "\"");
    return app.comfySvc->Send(app.comfy, std::move(job));
}

int SendPixel(AppState& app, ComfyJob job) {
    EnsureComfy(app);
    // Stable Diffusion 1.5 needs little memory (about 3 GB): the LLMs this app loaded keep running unless the card is nearly full
    const long long freeMiB = GpuFreeMiB();
    if (freeMiB >= 0 && freeMiB < 4500) ClefGivesGpuBack();
    CrashNote("pixel art: sending a Retro Diffusion job (" + std::to_string(job.pixW) + " x " + std::to_string(job.pixH) + ")");
    return app.comfySvc->Send(app.comfy, std::move(job));
}

namespace {
struct PicEditor {
    bool open = false, focus = false;
    std::vector<std::wstring> images;
    std::string instruction, label;
    int times = 1;
    EditContext ctx;
    bool hasCtx = false;
};
PicEditor g_pic;

void EditorThumb(AppState& app, const std::wstring& path, float size) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const AppState::LibThumb t = app.LibraryThumb(path);
    if (t.srv && t.w > 0 && t.h > 0) {
        ImVec2 uv0(0, 0), uv1(1, 1);
        if (t.w > t.h) { const float m = (1.0f - float(t.h) / t.w) * 0.5f; uv0.x = m; uv1.x = 1 - m; }
        else if (t.h > t.w) { const float m = (1.0f - float(t.w) / t.h) * 0.5f; uv0.y = m; uv1.y = 1 - m; }
        dl->AddImageRounded((ImTextureID)(intptr_t)t.srv, p, ImVec2(p.x + size, p.y + size), uv0, uv1, IM_COL32_WHITE, ui::Dp(5.0f));
    } else {
        dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), ui::ThumbPlaceholder(), ui::Dp(5.0f));
    }
}

bool PresetChips(const std::vector<std::pair<const char*, const char*>>& presets, std::string& picked) {
    bool any = false;
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    for (size_t i = 0; i < presets.size(); ++i) {
        const float w = ImGui::CalcTextSize(presets[i].first).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        if (i > 0 && ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + w < right) ImGui::SameLine();
        if (ImGui::SmallButton(presets[i].first)) {
            picked = presets[i].second;
            any = true;
        }
    }
    return any;
}
} // namespace

void OpenPictureEditor(AppState& app, const std::vector<std::wstring>& images, const std::string& instruction, const std::string& label, const EditContext* ctx) {
    EnsureComfy(app);
    PicEditor& e = g_pic;
    e.open = true;
    e.focus = true;
    e.images.assign(images.begin(), images.begin() + std::min<size_t>(images.size(), 10));
    e.instruction = instruction;
    e.label = label;
    e.times = 1;
    e.hasCtx = ctx != nullptr;
    if (ctx) e.ctx = *ctx;
    else e.ctx = EditContext();
    if (!ctx && e.images.size() >= 2) e.ctx.canvas = 0; // several loose pictures: the first one is edited unless you choose a new canvas
}

void DrawPictureEditor(AppState& app) {
    PicEditor& e = g_pic;
    if (!e.open) return;
    EnsureComfy(app);
    ImGui::SetNextWindowSize(ImVec2(ui::Dp(720.0f), ui::Dp(860.0f)), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(ui::Dp(420.0f), ui::Dp(300.0f)), ImVec2(ImGui::GetIO().DisplaySize.x - ui::Dp(40.0f), ImGui::GetIO().DisplaySize.y - ui::Dp(40.0f)));
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (e.focus) {
        ImGui::SetNextWindowFocus();
        e.focus = false;
    }
    if (!ImGui::Begin("Edit pictures with Qwen Image 2.1", &e.open)) {
        ImGui::End();
        return;
    }
    ImGui::BeginChild("##piccontent", ImVec2(0, -(ImGui::GetFrameHeightWithSpacing() + ui::Dp(8.0f))), false); // the Edit button stays in view below it
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("ComfyUI changes the FIRST picture as you say, using the others as references (up to 10). Call them <image1>, <image2>... in your "
                        "instruction. The originals are never changed: the result is a new picture.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    int remove = -1, up = -1;
    for (size_t i = 0; i < e.images.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        EditorThumb(app, e.images[i], ui::Dp(56.0f));
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextColored(i == 0 ? ui::kAccent : ui::kTextDim, "<image%d>  %s", static_cast<int>(i) + 1, i == 0 ? "the picture that gets edited" : "reference");
        ImGui::TextUnformatted(ToUtf8(fs::path(e.images[i]).filename().wstring()).c_str());
        ImGui::EndGroup();
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ui::Dp(110.0f));
        ImGui::BeginDisabled(i == 0);
        if (ImGui::SmallButton("Up")) up = static_cast<int>(i);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) remove = static_cast<int>(i);
        ImGui::PopID();
    }
    if (remove >= 0) e.images.erase(e.images.begin() + remove);
    if (up > 0) std::swap(e.images[up], e.images[up - 1]);
    if (e.images.empty()) ImGui::TextColored(ui::kWarn, "No pictures yet: add the one to edit.");

    auto add = [&](const std::wstring& p) {
        if (p.empty() || e.images.size() >= 10) return;
        for (const std::wstring& o : e.images) {
            if (_wcsicmp(o.c_str(), p.c_str()) == 0) return;
        }
        e.images.push_back(p);
    };
    ImGui::Spacing();
    ImGui::BeginDisabled(e.images.size() >= 10);
    if (ImGui::SmallButton("+ File...")) add(OpenImageFileDialog(GetActiveWindow()));
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Gallery picture")) {
        if (app.selectedIndex >= 0 && app.selectedIndex < static_cast<int>(app.entries.size())) add(app.entries[app.selectedIndex].path);
        else app.SetStatus("Open a picture in the gallery first");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Vision library picture")) {
        if (app.visionLibSelected >= 0 && app.visionLibSelected < static_cast<int>(app.visionLib.size())) add(app.visionLib[app.visionLibSelected].imagePath);
        else app.SetStatus("Select a picture in the Vision library first");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Saved character...")) ImGui::OpenPopup("##edchars");
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("##edchars")) {
        bool any = false;
        for (const CharacterProfile& p : AllProfiles()) {
            if (!p.saved) continue;
            any = true;
            const std::string name = p.name.empty() ? ToUtf8(fs::path(p.image).stem().wstring()) : p.name;
            if (ImGui::Selectable((name + "##" + p.key).c_str())) add(p.image);
        }
        if (!any) ImGui::TextDisabled("No saved characters yet (Characters tab).");
        ImGui::EndPopup();
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("What to change");
    const float boxH = std::max(ui::Dp(120.0f), ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 12.5f);
    WrapEdit("##picinstr", e.instruction, boxH);
    DrawPromptTriage(app, e.instruction, "picedit");
    std::string picked;
    if (PresetChips({{"Replace the clothes", "Replace the character's clothes in <image1> with the outfit shown in <image2>. Keep the face, pose and background of <image1>."},
                     {"Put a character in", "Put the character from <image2> into <image1>, keeping the scene, pose and lighting of <image1>."},
                     {"New background", "Replace the background of <image1> with the setting shown in <image2>. Keep the people exactly as they are."},
                     {"Add an object", "Add the object from <image2> to <image1> in a natural place. Keep everything else in <image1> unchanged."},
                     {"Improve quality", "Improve the quality, sharpness and detail of <image1> without changing its content, colours or composition."},
                     {"Copy the style", "Redraw <image1> in the art style of <image2>. Keep the content and composition of <image1>."}}, picked))
        e.instruction = picked;
    if (ImGui::SmallButton("Surprise me: a new look for <image1>")) {
        e.instruction = "Change <image1>: the person " + RandomLookSentence(std::string(), std::string()) +
                        ". Keep their face, hair and body recognizable, and keep the framing and the background of <image1>.";
        e.ctx.looksText.clear();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("Picks a concrete new outfit, body posture and expression at random (by Clef when it is running) and writes them as the instruction.");
    if (!e.ctx.finalPrompt.empty()) {
        const std::string full = FinalPromptClause(e.ctx.finalPrompt);
        bool on = e.instruction.find(full) != std::string::npos;
        if (ImGui::Checkbox("Include the board's final prompt (the one the vision model wrote)", &on)) {
            const size_t at = e.instruction.find(full);
            if (on && at == std::string::npos) {
                e.instruction += (e.instruction.empty() || e.instruction.back() == ' ' ? "" : " ") + full;
            } else if (!on && at != std::string::npos) {
                e.instruction.erase(at, full.size());
                while (!e.instruction.empty() && e.instruction.back() == ' ') e.instruction.pop_back();
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("The final prompt describes the characters in words, which the pictures already show. It can help to set the mood and details of the scene,\nbut it can also pull the result away from the pictures: try both.");
    }
    {
        int mode = 0;
        for (int m = 1; m <= 2; ++m) {
            if (e.instruction.find(EditStyleClause(m)) != std::string::npos) mode = m;
        }
        // the concrete directions (outfit, posture, expression) for the board's characters: swapped in place, or added after the style sentence
        auto reroll = [&](int styleMode) {
            if (!e.ctx.hasBoard || e.ctx.roles.size() != e.images.size()) return;
            BoardEditOptions eo;
            eo.styleMode = styleMode;
            eo.flags = DirectorFlagsFor(app.comfy, styleMode);
            const std::string fresh = BoardLooksClause(e.ctx.board, e.ctx.scene, e.images, e.ctx.prompt, eo);
            const size_t at = e.ctx.looksText.empty() ? std::string::npos : e.instruction.find(e.ctx.looksText);
            if (at != std::string::npos) {
                e.instruction.erase(at, e.ctx.looksText.size() + (at + e.ctx.looksText.size() < e.instruction.size() && e.instruction[at + e.ctx.looksText.size()] == ' ' ? 1 : 0));
            }
            if (!fresh.empty()) {
                size_t where = at != std::string::npos ? at : e.instruction.size();
                if (at == std::string::npos) { // after the style sentence when it is there
                    const std::string style = EditStyleClause(styleMode);
                    const size_t s = style.empty() ? std::string::npos : e.instruction.find(style);
                    if (s != std::string::npos) where = s + style.size() + (s + style.size() < e.instruction.size() && e.instruction[s + style.size()] == ' ' ? 1 : 0);
                }
                const bool atEnd = where >= e.instruction.size();
                e.instruction.insert(where, (atEnd && !e.instruction.empty() && e.instruction.back() != ' ' ? " " : "") + fresh + (atEnd ? "" : " "));
            }
            e.ctx.looksText = fresh;
        };
        ImGui::TextUnformatted("How freely to change the characters");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##editstyle", EditStyleName(mode))) {
            for (int m = 0; m <= 2; ++m) {
                if (ImGui::Selectable(EditStyleName(m), mode == m)) {
                    for (int k = 1; k <= 2; ++k) { // swap the sentence for the chosen one
                        const std::string old = EditStyleClause(k);
                        const size_t at = e.instruction.find(old);
                        if (at != std::string::npos) e.instruction.erase(at, old.size());
                    }
                    while (!e.instruction.empty() && e.instruction.back() == ' ') e.instruction.pop_back();
                    if (m > 0) e.instruction += (e.instruction.empty() ? "" : " ") + EditStyleClause(m);
                    app.comfy.editStyle = m; // remembered for the next board scene
                    SaveComfySettings(app.comfy);
                    reroll(m);
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("Nano banana style: new outfits, poses and expressions in a loose, natural, candid look; the characters stay recognizable by face, hair and body.\n"
                              "Fit everything to the scene: the face, body and the character's own visual style stay; pose, expression and outfit details adapt to the scene.");
        if (mode > 0 && e.ctx.hasBoard) {
            bool changed = false;
            ImGui::TextUnformatted("Tell the model to change:");
            ImGui::SameLine();
            if (ImGui::Checkbox("outfits##dirout", &app.comfy.directOutfit)) changed = true;
            ImGui::SameLine();
            if (ImGui::Checkbox("poses##dirpose", &app.comfy.directPose)) changed = true;
            ImGui::SameLine();
            if (ImGui::Checkbox("expressions##direxp", &app.comfy.directExpression)) changed = true;
            ImGui::SameLine();
            if (ImGui::SmallButton("Shuffle the directions") || changed) {
                SaveComfySettings(app.comfy);
                reroll(mode);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("The model only changes clothes, pose and expression when it is told WHAT to change, so every character gets a concrete new outfit, body posture and\n"
                                  "expression that fit the place (picked by Clef when it is running, otherwise at random). They are in the text above: edit them freely.");
        }
    }
    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Workflow: Qwen Image 2.1 Edit (built in)");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ui::Dp(100.0f));
    ImGui::InputInt("times##pictimes", &e.times, 1, 5);
    e.times = std::clamp(e.times, 1, 100);
    {
        const char* canvases[] = {"Edit the first picture (its size and frame)", "New canvas: landscape 3:2", "New canvas: square 1:1", "New canvas: portrait 2:3",
                                  "New canvas: cinematic 16:9", "New canvas: phone 9:16", "New canvas: ultra-wide 21:9"};
        ImGui::SetNextItemWidth(ui::Dp(330.0f));
        ImGui::Combo("Shape of the result##canvas", &e.ctx.canvas, canvases, 7);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("To edit one picture, keep \"Edit the first picture\". To put several characters together in ONE scene, pick a new canvas: the pictures only\n"
                              "show who they are, and the result is not tied to the frame of the first one (no side-by-side or blended look).");
    }
    {   // quality versus time: a slider with notches, each showing how long it takes on THIS computer
        ComfySettings& cs = app.comfy;
        struct Level {
            const char* name;
            int steps, res;
        };
        static const Level fullLevels[] = {{"Draft", 8, 640}, {"Fast", 14, 768}, {"Balanced", 25, 1024}, {"High", 40, 1280}, {"Max", 50, 1792}};
        static const Level turboLevels[] = {{"Draft", 4, 640}, {"Fast", 5, 768}, {"Balanced", 6, 1024}, {"High", 8, 1280}, {"Max", 10, 1536}}; // a turbo model: 4-8 steps
        const Level* levels = IsTurboModel(cs.editUnet) ? turboLevels : fullLevels;
        int level = -1;
        for (int i = 0; i < 5; ++i) {
            if (cs.editSteps == levels[i].steps && cs.editResolution == levels[i].res) level = i;
        }
        const int nImages = static_cast<int>(e.images.size());
        ImGui::Spacing();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Quality");
        ImGui::SameLine();
        const double est = EstimateEditSeconds(cs.editSteps, cs.editResolution, nImages, e.ctx.canvas);
        ImGui::TextColored(ui::kAccent, "%s", level >= 0 ? levels[level].name : "Custom");
        ImGui::SameLine();
        ImGui::TextDisabled("  about %s per picture", FormatEditTime(est).c_str());
        int shown = level >= 0 ? level : 2;
        if (level < 0) { // the nearest notch, for the slider to sit on
            double best = 1e9;
            for (int i = 0; i < 5; ++i) {
                const double d = std::fabs(EstimateEditSeconds(levels[i].steps, levels[i].res, nImages, e.ctx.canvas) - est);
                if (d < best) { best = d; shown = i; }
            }
        }
        ImGui::Indent(ui::Dp(30.0f)); // room for the first and last notch names
        const float width = std::max(ui::Dp(300.0f), ImGui::GetContentRegionAvail().x - ui::Dp(60.0f));
        ImGui::SetNextItemWidth(width);
        int moved = shown;
        if (ImGui::SliderInt("##qlevel", &moved, 0, 4, "")) {
            if (moved != shown || level < 0) {
                cs.editSteps = levels[moved].steps;
                cs.editResolution = levels[moved].res;
                SaveComfySettings(cs);
                level = moved;
            }
        }
        const ImVec2 rmin = ImGui::GetItemRectMin(), rmax = ImGui::GetItemRectMax();
        const float grab = ImGui::GetStyle().GrabMinSize;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (int i = 0; i < 5; ++i) { // each notch: its name and what it costs in time
            const float x = rmin.x + grab * 0.5f + (rmax.x - rmin.x - grab) * i / 4.0f;
            const std::string name = levels[i].name;
            const std::string time = FormatEditTime(EstimateEditSeconds(levels[i].steps, levels[i].res, nImages, e.ctx.canvas));
            const bool on = level == i;
            const ImVec2 ns = ImGui::CalcTextSize(name.c_str()), ts = ImGui::CalcTextSize(time.c_str());
            const ImU32 col = on ? ImGui::GetColorU32(ui::kAccent) : ImGui::GetColorU32(ui::kTextDim);
            dl->AddText(ImVec2(x - ns.x * 0.5f, rmax.y + 2.0f), col, name.c_str());
            dl->AddText(ImVec2(x - ts.x * 0.5f, rmax.y + 2.0f + ns.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), time.c_str());
        }
        ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight() * 2.0f + 6.0f));
        ImGui::Unindent(ui::Dp(30.0f));
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Time is %s.", EditEstimateSource().c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::TreeNode("Fine-tune (steps and size)")) {
            bool changed = false;
            DrawEditModelPicker(cs, "qmodel");
            ImGui::TextUnformatted("LoRAs for edits");
            DrawLoraList(cs, cs.editLoras, "loraeditwin");
            ImGui::SetNextItemWidth(ui::Dp(240.0f));
            if (ImGui::SliderInt("Steps##qsteps", &cs.editSteps, 4, 60)) changed = true;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ui::Dp(240.0f));
            if (ImGui::SliderInt("Size (pixels, 0 = like the first picture)##qres", &cs.editResolution, 0, 2048)) changed = true;
            if (changed) SaveComfySettings(cs);
            ImGui::TreePop();
        }
    }
    ImGui::EndChild();
    const bool ready = !e.images.empty() && e.instruction.find_first_not_of(" \t\r\n") != std::string::npos;
    ImGui::BeginDisabled(!ready);
    auto send = [&]() {
        const std::string label = e.label.empty() ? ToUtf8(fs::path(e.images.front()).filename().wstring()) : e.label;
        EditContext c = e.ctx; // the pictures were changed in the window: the board is only kept when nothing was added or removed
        if (c.roles.size() != e.images.size()) c.hasBoard = false;
        return SendPictureEdit(app, e.images, e.instruction, label, e.times, &c);
    };
    ImGui::Checkbox("Then make it pixel art##edpx", &e.ctx.pixelArt);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("The result also becomes real pixel art (the Pixel art tab's settings), in the Pixel art tab");
    if (ui::PrimaryButton("Edit", ImVec2(ui::Dp(110.0f), 0)) ||
        (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Enter))) {
        if (send()) e.open = false; // the Edits tab shows it from here
    }
    ImGui::EndDisabled();
    ImGui::End();
}

void PumpComfyResults(AppState& app) {
    if (app.activeTab != 4 && g_resPlayer && g_resPlayer->Playing()) g_resPlayer->Pause(); // its sound stops when you leave the ComfyUI tab
}
