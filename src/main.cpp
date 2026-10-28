// Try2Create
// Single-window Win32 + DirectX11 + Dear ImGui application.

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "imgui_internal.h"
#include <d3d11.h>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <dwmapi.h>
#include <algorithm>
#include <atomic>
#include <thread>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>
#include <functional>
#include <set>
#include <map>
#include <memory>
#include <mutex>
#include <fstream>

#include "app_state.h"
#include "win_dialogs.h"
#include "scrape.h"
#include "resource.h"
#include "tag_generator.h"
#include "tag_import.h"
#include "organizer.h"
#include "vision_prompt.h"
#include "ui_theme.h"
#include "clef.h"
#include "clef_grades.h"
#include "embed_index.h"
#include "relation_import.h"
#include "crash_log.h"
#include "notify.h"
#include "ui_extra.h"
#include "clipboard_image.h"
#include "video_util.h"
#include "video_player.h"
#include "image_attributes.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// D3D11 device/swapchain plumbing (standard Dear ImGui win32+dx11 boilerplate)
// ---------------------------------------------------------------------------
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static bool g_SwapChainOccluded = false;
static UINT g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
static AppState* g_app = nullptr;
static bool g_closeRequested = false; // X pressed after Vision was used: clean up the backend first
static HWND g_hwnd = nullptr;
static bool g_fullscreen = false;
static bool g_uiRebuild = false;  // theme / scale changed: rebuild fonts and style between frames
static float g_dpiScale = 1.0f;   // monitor DPI scale (the user's own scale multiplies this)
static WINDOWPLACEMENT g_prevPlacement = {sizeof(WINDOWPLACEMENT)};

// Dragging thumbnails out of the window into Explorer, a browser, a chat app...
static std::vector<std::wstring> g_dragOutPaths; // set while a thumbnail is being dragged; started after the frame
static bool g_dragOutActive = false;
static unsigned long long g_dragEndTick = 0;

static void RunFileDragOut(HWND hwnd, const std::vector<std::wstring>& paths) {
    std::vector<PIDLIST_ABSOLUTE> pidls;
    for (const auto& p : paths) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(p.c_str(), nullptr, &pidl, 0, nullptr)) && pidl) pidls.push_back(pidl);
    }
    if (!pidls.empty()) {
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(pidls.size()), const_cast<PCIDLIST_ABSOLUTE_ARRAY>(pidls.data()), &items))) {
            IDataObject* data = nullptr;
            if (SUCCEEDED(items->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data)))) {
                DWORD effect = 0;
                g_dragOutActive = true;
                // Copy or link only: dragging out must never move the originals.
                SHDoDragDrop(hwnd, data, nullptr, DROPEFFECT_COPY | DROPEFFECT_LINK, &effect);
                g_dragOutActive = false;
                data->Release();
            }
            items->Release();
        }
    }
    for (auto* pidl : pidls) CoTaskMemFree(pidl);
}

// Borderless fullscreen on the window's monitor, restoring the previous placement on exit.
static void SetFullscreen(HWND hwnd, bool on) {
    if (on == g_fullscreen) return;
    DWORD style = static_cast<DWORD>(GetWindowLongW(hwnd, GWL_STYLE));
    if (on) {
        MONITORINFO mi = {sizeof(mi)};
        if (!GetWindowPlacement(hwnd, &g_prevPlacement) ||
            !GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            return;
        }
        SetWindowLongW(hwnd, GWL_STYLE, static_cast<LONG>(style & ~WS_OVERLAPPEDWINDOW));
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(hwnd, GWL_STYLE, static_cast<LONG>(style | WS_OVERLAPPEDWINDOW));
        SetWindowPlacement(hwnd, &g_prevPlacement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    g_fullscreen = on;
}

bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(len > 0 ? len - 1 : 0, '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(len > 0 ? len - 1 : 0, L'\0');
    if (len > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len);
    return out;
}

const char* GeneratorLabel(Generator g) {
    switch (g) {
        case Generator::Automatic1111: return "AUTOMATIC1111 / compatible fork";
        case Generator::ComfyUI: return "ComfyUI";
        case Generator::NovelAI: return "NovelAI";
        case Generator::InvokeAI: return "InvokeAI";
        case Generator::Generic: return "Unrecognized format (raw metadata shown)";
        default: return "No metadata found";
    }
}

std::string JoinTags(const std::vector<std::string>& tags, const char* sep) {
    std::string out;
    for (size_t i = 0; i < tags.size(); ++i) {
        if (i) out += sep;
        out += tags[i];
    }
    return out;
}

// Starts a new button on the current line when it still fits, else wraps.
// `first` must be true for the first button of a group.
bool WrapButton(const char* label, bool first) {
    ImGuiStyle& style = ImGui::GetStyle();
    float w = ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f;
    if (!first) {
        float x2 = ImGui::GetItemRectMax().x + style.ItemSpacing.x + w;
        if (x2 < ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x) ImGui::SameLine();
    }
    return ImGui::Button(label);
}

void DrawTagChips(const std::vector<std::string>& tags, const char* idSalt) {
    if (tags.empty()) {
        ImGui::TextDisabled("(none)");
        return;
    }
    ImGui::PushID(idSalt);
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ui::Dp(12.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ui::Dp(9.0f), ui::Dp(3.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui::Dp(6.0f), ui::Dp(6.0f)));
    ImGui::PushStyleColor(ImGuiCol_Button, ui::ChipColor());
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::ChipHoverColor());
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ui::ChipActiveColor());
    float windowVisibleX2 = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    for (size_t i = 0; i < tags.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Button(tags[i].c_str())) ImGui::SetClipboardText(tags[i].c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to copy");
        float lastX2 = ImGui::GetItemRectMax().x;
        ImGui::PopID();
        if (i + 1 < tags.size()) {
            ImVec2 nextSize = ImGui::CalcTextSize(tags[i + 1].c_str());
            float nextWidth = nextSize.x + style.FramePadding.x * 2.0f;
            if (lastX2 + style.ItemSpacing.x + nextWidth < windowVisibleX2) ImGui::SameLine();
        }
    }
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(3);
    ImGui::PopID();
}

std::string ToLowerStr(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return std::tolower(c); });
    return r;
}

std::wstring ToLowerStrW(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return s;
}

// A bare hex string under a "*hash*" key (e.g. "Model hash": "abc12345") has
// no human-readable name attached, unlike e.g. "Lora hashes" which already
// pairs names with hashes as one compound string. For the former, offer a
// one-click search on Civitai (which resolves hashes to model names/pages)
// rather than trying to guess at an offline mapping.
bool LooksLikeBareHash(const std::string& key, const std::string& value) {
    std::string keyLower = ToLowerStr(key);
    if (keyLower.find("hash") == std::string::npos) return false;
    if (value.size() < 6 || value.size() > 64) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isxdigit(c); });
}

void DrawParamsTable(const KeyValueList& params) {
    if (params.empty()) {
        ImGui::TextDisabled("(none)");
        return;
    }
    if (ImGui::BeginTable("params_table", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, ui::Dp(112.0f));
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, ui::Dp(104.0f));
        for (size_t i = 0; i < params.size(); ++i) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushStyleColor(ImGuiCol_Text, ui::kTextDim);
            ImGui::TextWrapped("%s", params[i].first.c_str());
            ImGui::PopStyleColor();
            ImGui::TableSetColumnIndex(1);
            ImGui::TextWrapped("%s", params[i].second.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(params[i].second.c_str());
            if (LooksLikeBareHash(params[i].first, params[i].second)) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Find")) {
                    std::string url = "https://civitai.com/search/models?query=" + params[i].second;
                    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Search this hash on Civitai in your browser");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// Green below the questionable band, amber inside it, red at/above the NSFW
// threshold (matching how "NSFW Sort" would file the image).
ImU32 NsfwScoreColor(const AppState& app, float score) {
    if (score >= app.nsfwThreshold) return IM_COL32(240, 90, 90, 235);
    if (app.nsfwUseQuestionable && score >= app.nsfwQuestionableLow) return IM_COL32(245, 190, 70, 235);
    return IM_COL32(110, 210, 120, 235);
}

ImU32 GeneratorColor(Generator g) {
    switch (g) {
        case Generator::Automatic1111: return IM_COL32(86, 140, 235, 255);
        case Generator::ComfyUI: return IM_COL32(60, 170, 130, 255);
        case Generator::NovelAI: return IM_COL32(165, 110, 225, 255);
        case Generator::InvokeAI: return IM_COL32(220, 120, 90, 255);
        case Generator::Generic: return IM_COL32(150, 150, 70, 255);
        default: return IM_COL32(90, 96, 108, 255);
    }
}

std::string FormatFileTime(std::filesystem::file_time_type t) {
    // MSVC's file_time_type counts 100 ns ticks since 1601, i.e. a FILETIME.
    ULARGE_INTEGER u;
    u.QuadPart = static_cast<unsigned long long>(t.time_since_epoch().count());
    FILETIME ft{u.LowPart, u.HighPart};
    SYSTEMTIME utc, local;
    if (!FileTimeToSystemTime(&ft, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return {};
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d", local.wYear, local.wMonth, local.wDay, local.wHour,
             local.wMinute);
    return buf;
}

void ShowInExplorer(const std::wstring& path) {
    std::wstring args = L"/select,\"" + path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

void QueueDelete(AppState& app, int index) {
    app.pendingDeletePaths.clear();
    if (app.multiSelected.size() > 1 && app.multiSelected.count(index)) {
        for (int idx : app.multiSelected) {
            if (idx >= 0 && idx < static_cast<int>(app.entries.size())) app.pendingDeletePaths.push_back(app.entries[idx].path);
        }
    } else {
        app.pendingDeletePaths.push_back(app.entries[index].path);
    }
    app.showDeleteConfirm = true;
}

// Entries a rating/compare action applies to: the whole multi-selection if
// `index` is part of it, otherwise just `index`.
std::vector<int> ActionTargets(const AppState& app, int index) {
    if (app.multiSelected.size() > 1 && app.multiSelected.count(index)) {
        return std::vector<int>(app.multiSelected.begin(), app.multiSelected.end());
    }
    return {index};
}

void ApplyRating(AppState& app, const std::vector<int>& targets, int rating) {
    app.SetRating(targets, rating);
    app.SetStatus(rating == 0 ? "Cleared rating on " + std::to_string(targets.size()) + " image(s)."
                              : "Rated " + std::to_string(targets.size()) + " image(s) " + std::to_string(rating) +
                                    (rating == 1 ? " star." : " stars."));
}

// Opens the Vision window and, if a model is already chosen, starts describing one image
// as the given output kind (seconds only matters for video scenes).
void RunVision(AppState& app, AppState::VisionMode mode, int seconds, int index) {
    app.visionMode = mode;
    if (mode == AppState::VisionMode::VideoScene) app.videoOptions.seconds = std::clamp(seconds, 5, 30);
    app.SaveVisionSettings();
    app.showVisionWindow = true;
    if (!app.visionModel.empty() && index >= 0) app.DescribeWithVision({index});
}

// Red -> orange -> yellow -> green -> teal as the quality goes up.
ImVec4 QualityColor(float q) {
    if (q < 3.0f) return ImVec4(0.95f, 0.38f, 0.36f, 1.0f);
    if (q < 5.0f) return ImVec4(0.96f, 0.62f, 0.28f, 1.0f);
    if (q < 6.5f) return ImVec4(0.93f, 0.85f, 0.35f, 1.0f);
    if (q < 8.0f) return ImVec4(0.50f, 0.85f, 0.45f, 1.0f);
    return ImVec4(0.30f, 0.85f, 0.75f, 1.0f);
}

// Copies pictures to the clipboard (bitmap + PNG + files); `clean` leaves out the metadata.
void CopyPictures(AppState& app, const std::vector<int>& targets, bool clean) {
    std::vector<std::wstring> paths;
    for (int t : targets) {
        if (t >= 0 && t < static_cast<int>(app.entries.size())) paths.push_back(app.entries[t].path);
    }
    std::string msg;
    CopyImagesToClipboard(g_hwnd, paths, clean, &msg);
    app.SetStatus(msg);
}

// Describe several images (the selection) as the given kind of output.
void RunVisionOn(AppState& app, AppState::VisionMode mode, int seconds, const std::vector<int>& targets) {
    app.visionMode = mode;
    if (mode == AppState::VisionMode::VideoScene) app.videoOptions.seconds = std::clamp(seconds, 5, 30);
    app.SaveVisionSettings();
    app.showVisionWindow = true;
    if (!app.visionModel.empty() && !targets.empty()) app.RequestVisionBatch(targets);
}

// Renders one gallery cell at the cursor and returns whether it was clicked
// this frame; the caller (which knows about Ctrl/Shift modifiers and the
// visible-index list needed for range-select) decides what a click means.
void DrawNavIcon(ImDrawList* dl, int icon, ImVec2 c, float s, ImU32 col);

// Test hook (MV_UI_TEST, e.g. "hover:3;new:5;making"): a hover that posted mouse messages cannot hold, a "NEW" badge, sample jobs in the strip.
int UiTestNumber(const char* key) {
    static std::string spec = [] {
        char buf[256] = {};
        return GetEnvironmentVariableA("MV_UI_TEST", buf, sizeof(buf)) ? std::string(buf) : std::string();
    }();
    // whole names only ("h3" is not in "h3runs:0")
    const size_t len = strlen(key);
    for (size_t at = spec.find(key); at != std::string::npos; at = spec.find(key, at + 1)) {
        if (at > 0 && spec[at - 1] != ';') continue;
        const size_t end = at + len;
        if (end == spec.size() || spec[end] == ';') return 0;
        if (spec[end] == ':') return atoi(spec.c_str() + end + 1);
    }
    return -1;
}

std::map<int, ImVec2> g_cellCenters; // tests ("ctrlclick"): where each drawn thumbnail is
void SelLog(const AppState& app, const char* what, int i) { // tests ("ctrlclick"): every change of the selection, in ctrlclick_log.txt
    static const bool on = UiTestNumber("ctrlclick") > 0;
    if (!on) return;
    wchar_t lad[1024] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", lad, 1024);
    std::ofstream out(std::wstring(lad) + L"\\Try2Create\\ctrlclick_log.txt", std::ios::binary | std::ios::app);
    out << ImGui::GetFrameCount() << " " << what << " " << i << " ctrl=" << ImGui::GetIO().KeyCtrl << " multi=" << app.multiSelected.size() << ":";
    for (int idx : app.multiSelected) out << " " << idx;
    out << "\n";
}

bool DrawThumbnailCell(AppState& app, int i, float cell, float labelH) {
    ImageEntry& e = app.entries[i];
    g_cellCenters[i] = ImVec2(ImGui::GetCursorScreenPos().x + cell * 0.5f, ImGui::GetCursorScreenPos().y + cell * 0.55f);
    const bool primary = (i == app.selectedIndex);
    const bool selected = primary || app.multiSelected.count(i) > 0;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 size(cell, cell + labelH);
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);

    // Where the picture sits, and whether the pointer is over this cell (also while it is on one of the hover buttons)
    const float oPad = ui::Dp(4.0f);
    const ImVec2 oa(p0.x + oPad, p0.y + oPad), ob(p1.x - oPad, p0.y + cell - oPad);
    bool hovered = ImGui::IsMouseHoveringRect(p0, p1) && ImGui::IsWindowHovered() && !ImGui::IsMouseDragging(ImGuiMouseButton_Left);
    static const int testHover = UiTestNumber("hover"), testNew = UiTestNumber("new");
    if (i == testHover) hovered = true;
    // The hover buttons are made BEFORE the cell: they take the clicks they are under, and the cell answers every other click at once.
    // (Made after it, the cell would have to allow overlap, and ImGui then holds its hover back a frame: a click that comes with the
    // pointer's arrival, as Ctrl+clicks along a row do, was lost.)
    const bool picking = app.multiSelected.size() > 1; // the selection always holds the current picture: picking means more than that
    const bool showPick = (hovered || picking) && cell >= ui::Dp(70.0f);
    const float pickS = ui::Dp(cell >= ui::Dp(120.0f) ? 26.0f : 22.0f);
    const ImVec2 pickC(oa.x + ui::Dp(6.0f) + pickS * 0.5f, oa.y + ui::Dp(6.0f) + pickS * 0.5f);
    bool pickHov = false;
    if (showPick) {
        ImGui::SetCursorScreenPos(ImVec2(pickC.x - pickS * 0.5f, pickC.y - pickS * 0.5f));
        if (ImGui::InvisibleButton("##pick", ImVec2(pickS, pickS))) {
            if (app.multiSelected.count(i)) app.multiSelected.erase(i);
            else app.multiSelected.insert(i);
            SelLog(app, "pick", i);
            app.lastClickedIndex = i;
            app.SelectIndex(i, /*resetMultiSelect=*/false);
            app.scrollToSelected = false;
        }
        pickHov = ImGui::IsItemHovered();
    }
    struct QuickBtn {
        const char* id;
        int icon;
        const char* tip;
        bool enabled;
        ImVec2 c{0, 0};
        bool hov = false, clicked = false;
    };
    std::vector<QuickBtn> quick;
    float quickS = 0.0f;
    const bool showQuick = hovered && e.thumbReady && cell >= ui::Dp(96.0f);
    if (showQuick) {
        const bool hasPrompt = !e.metadata.parsed.positivePrompt.empty();
        const float gapB = ui::Dp(4.0f), room = (ob.x - oa.x) - ui::Dp(12.0f);
        bool picture = !e.metadata.isVideo;
        int count = picture ? 5 : 2;
        quickS = std::min(ui::Dp(cell >= ui::Dp(150.0f) ? 28.0f : 24.0f), (room - (count - 1) * gapB) / count);
        if (quickS < ui::Dp(19.0f) && count > 2) { // small thumbnails keep Copy and More
            picture = false;
            count = 2;
            quickS = std::min(ui::Dp(24.0f), (room - gapB) / 2.0f);
        }
        quick.push_back({"##qcopy", 10, hasPrompt ? "Copy the prompt" : "No prompt in this file", hasPrompt});
        if (picture) {
            quick.push_back({"##qedit", 6, "Edit with Qwen Image 2.1...", true});
            quick.push_back({"##qpixel", 9, "Pixel art it (Convert settings, smart width)", true});
            quick.push_back({"##qanim", 14, "Animate: a video with sound (the Video page writes what happens)", true});
        }
        quick.push_back({"##qmore", 11, "More (or right-click)", true});
        float x = ob.x - ui::Dp(6.0f) - quickS * 0.5f - (quick.size() - 1) * (quickS + gapB);
        const float y = ob.y - ui::Dp(6.0f) - quickS * 0.5f;
        for (QuickBtn& q : quick) {
            q.c = ImVec2(x, y);
            x += quickS + gapB;
            ImGui::SetCursorScreenPos(ImVec2(q.c.x - quickS * 0.5f, q.c.y - quickS * 0.5f));
            ImGui::BeginDisabled(!q.enabled);
            q.clicked = ImGui::InvisibleButton(q.id, ImVec2(quickS, quickS)) && q.enabled;
            ImGui::EndDisabled();
            q.hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
        }
    }
    ImGui::SetCursorScreenPos(p0);
    bool clicked = ImGui::InvisibleButton("##cell", size);
    if (i == testNew && testNew >= 0 && !e.arrivedTick) e.arrivedTick = GetTickCount64();
    // Dragging inside the program moves pictures onto a folder of the folder pane. Leaving the window with the
    // drag hands it over to Windows (drop into Explorer, a browser, a chat app...): see the main loop.
    if (GetTickCount64() - g_dragEndTick > 400 && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
        const std::vector<int> targets = ActionTargets(app, i);
        ImGui::SetDragDropPayload("MV_IMAGES", targets.data(), targets.size() * sizeof(int));
        ImGui::BeginTooltip();
        ImGui::Text("%d image%s", static_cast<int>(targets.size()), targets.size() == 1 ? "" : "s");
        ImGui::TextDisabled(app.showFolderPane ? "Drop on a folder, or outside the window to copy elsewhere" : "Drop outside the window to copy elsewhere\n(View > Folder tree to move into folders)");
        ImGui::EndTooltip();
        ImGui::EndDragDropSource();
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !selected) app.SelectIndex(i);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float round = ui::Dp(8.0f);
    const float pad = ui::Dp(4.0f);
    if (selected) {
        dl->AddRectFilled(p0, p1, ui::WithAlpha(ui::kAccent, primary ? 0.22f : 0.14f), round);
        dl->AddRect(p0, p1, ui::WithAlpha(ui::kAccent, primary ? 1.0f : 0.55f), round, 0, ui::Dp(primary ? 2.0f : 1.5f));
    } else if (hovered) {
        dl->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 16), round);
    }

    const ImVec2 a(p0.x + pad, p0.y + pad);
    const ImVec2 b(p1.x - pad, p0.y + cell - pad);
    const float imgRound = round - ui::Dp(2.0f);
    ID3D11ShaderResourceView* thumbShown = e.thumbReady ? app.ShownSrv(e.thumbSrv, app.ShouldBlur(e)) : nullptr;
    if (thumbShown) {
        ImVec2 uv0(0, 0), uv1(1, 1);
        if (e.thumbW > e.thumbH && e.thumbH > 0) {
            float f = static_cast<float>(e.thumbH) / e.thumbW;
            float m = (1.0f - f) * 0.5f;
            uv0.x = m; uv1.x = 1.0f - m;
        } else if (e.thumbH > e.thumbW && e.thumbW > 0) {
            float f = static_cast<float>(e.thumbW) / e.thumbH;
            float m = (1.0f - f) * 0.5f;
            uv0.y = m; uv1.y = 1.0f - m;
        }
        ImU32 tint = (selected || hovered) ? IM_COL32(255, 255, 255, 255) : IM_COL32(232, 232, 232, 255);
        dl->AddImageRounded((ImTextureID)(intptr_t)thumbShown, a, b, uv0, uv1, tint, imgRound);
    } else {
        dl->AddRectFilled(a, b, ui::ThumbPlaceholder(), imgRound);
        if (e.thumbReady && !e.thumbSrv) {
            const char* msg = "unreadable";
            ImVec2 ts = ImGui::CalcTextSize(msg);
            dl->AddText(ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f), IM_COL32(120, 128, 140, 255), msg);
        }
    }

    if (e.thumbReady && e.metadata.isVideo) {
        // play triangle in the middle and the clip length bottom-right
        const ImVec2 mid((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        const float r = std::min(b.x - a.x, b.y - a.y) * 0.16f;
        dl->AddCircleFilled(mid, r, IM_COL32(10, 12, 16, 150), 24);
        dl->AddTriangleFilled(ImVec2(mid.x - r * 0.32f, mid.y - r * 0.5f), ImVec2(mid.x - r * 0.32f, mid.y + r * 0.5f),
                              ImVec2(mid.x + r * 0.55f, mid.y), IM_COL32(255, 255, 255, 235));
        if (e.metadata.durationSec > 0) {
            const std::string dur = FormatDuration(e.metadata.durationSec);
            const ImVec2 ts = ImGui::CalcTextSize(dur.c_str());
            const ImVec2 q0(b.x - ts.x - ui::Dp(12.0f), a.y + ui::Dp(5.0f));
            dl->AddRectFilled(q0, ImVec2(q0.x + ts.x + ui::Dp(8.0f), q0.y + ts.y + ui::Dp(2.0f)), IM_COL32(12, 14, 18, 200), ui::Dp(4.0f));
            dl->AddText(ImVec2(q0.x + ui::Dp(4.0f), q0.y + ui::Dp(1.0f)), IM_COL32(240, 240, 245, 255), dur.c_str());
        }
    }
    if (e.thumbReady && e.metadata.hasMetadata && !(hovered && cell >= ui::Dp(96.0f))) { // the hover buttons take its corner
        ImVec2 c(b.x - ui::Dp(9.0f), b.y - ui::Dp(9.0f));
        dl->AddCircleFilled(c, ui::Dp(5.0f), IM_COL32(80, 220, 100, 255));
        dl->AddCircle(c, ui::Dp(5.0f), IM_COL32(20, 20, 20, 180), 0, ui::Dp(1.0f));
    }
    if (e.visionMask) { // already described with Vision: a small speech bubble
        const float w = ui::Dp(22.0f), h = ui::Dp(16.0f);
        ImVec2 q0(b.x - w - ui::Dp(5.0f), a.y + ui::Dp(5.0f));
        ImVec2 q1(q0.x + w, q0.y + h);
        const ImU32 purple = IM_COL32(150, 98, 235, 245);
        dl->AddRectFilled(q0, q1, purple, ui::Dp(4.0f));
        dl->AddTriangleFilled(ImVec2(q0.x + ui::Dp(3.0f), q1.y - ui::Dp(1.0f)), ImVec2(q0.x + ui::Dp(9.0f), q1.y - ui::Dp(1.0f)),
                              ImVec2(q0.x + ui::Dp(3.0f), q1.y + ui::Dp(5.0f)), purple);
        for (int d = 0; d < 3; ++d) dl->AddCircleFilled(ImVec2(q0.x + ui::Dp(5.5f) + d * ui::Dp(5.5f), q0.y + h * 0.5f), ui::Dp(1.7f), IM_COL32(255, 255, 255, 240));
    }
    if (app.showQualityBadges && e.thumbReady) {
        const float q = e.Quality();
        if (q >= 0.0f) { // small chip, top right (below the Vision bubble when there is one)
            char label[8];
            snprintf(label, sizeof(label), "%.1f", q);
            const ImVec2 ts = ImGui::CalcTextSize(label);
            const ImVec2 q0(b.x - ts.x - ui::Dp(13.0f), a.y + ui::Dp(5.0f) + (e.visionMask ? ui::Dp(21.0f) : 0.0f));
            dl->AddRectFilled(q0, ImVec2(q0.x + ts.x + ui::Dp(8.0f), q0.y + ts.y + ui::Dp(2.0f)), IM_COL32(12, 14, 18, 200), ui::Dp(4.0f));
            dl->AddText(ImVec2(q0.x + ui::Dp(4.0f), q0.y + ui::Dp(1.0f)), ImGui::GetColorU32(QualityColor(q)), label);
        }
    }
    float nsfw = EffectiveNsfwScore(e, app.nsfwSource);
    if (app.showNsfwBadges && nsfw >= 0.0f && !hovered && app.multiSelected.size() <= 1) { // the tick box takes its corner then
        char label[8];
        snprintf(label, sizeof(label), "%d%%", static_cast<int>(nsfw * 100.0f + 0.5f));
        ImVec2 textSize = ImGui::CalcTextSize(label);
        ImVec2 q0(a.x + ui::Dp(5.0f), a.y + ui::Dp(5.0f));
        ImVec2 q1(q0.x + textSize.x + ui::Dp(10.0f), q0.y + textSize.y + ui::Dp(2.0f));
        dl->AddRectFilled(q0, q1, NsfwScoreColor(app, nsfw), ui::Dp(4.0f));
        dl->AddText(ImVec2(q0.x + ui::Dp(5.0f), q0.y + ui::Dp(1.0f)), IM_COL32(15, 15, 15, 255), label);
    }

    if (e.rating > 0) {
        char stars[16];
        snprintf(stars, sizeof(stars), "\xE2\x98\x85%d", e.rating);
        ImVec2 ts = ImGui::CalcTextSize(stars);
        ImVec2 q0(a.x + ui::Dp(5.0f), b.y - ts.y - ui::Dp(7.0f));
        dl->AddRectFilled(q0, ImVec2(q0.x + ts.x + ui::Dp(10.0f), q0.y + ts.y + ui::Dp(2.0f)), IM_COL32(12, 14, 18, 190), ui::Dp(4.0f));
        dl->AddText(ImVec2(q0.x + ui::Dp(5.0f), q0.y + ui::Dp(1.0f)), ui::StarColor(true), stars);
    }

    // File name (without folders), clipped with an ellipsis.
    std::string name = WideToUtf8(fs::path(e.displayName).filename().wstring());
    ImVec2 lp0(p0.x + pad, p0.y + cell);
    ImVec2 lp1(p1.x - pad, p1.y);
    ImGui::PushStyleColor(ImGuiCol_Text, selected ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : ui::kTextDim);
    ImGui::RenderTextEllipsis(dl, lp0, lp1, lp1.x, lp1.x, name.c_str(), nullptr, nullptr);
    ImGui::PopStyleColor();

    if (hovered && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(WideToUtf8(e.displayName).c_str());
        std::string time = FormatFileTime(e.mtime);
        ImGui::TextDisabled("%s%s%s", ui::FormatBytes(e.size).c_str(), time.empty() ? "" : "  -  ", time.c_str());
        if (e.people >= 0) ImGui::TextDisabled("People: %s", PeopleLabel(e.people).c_str());
        if (e.visionMask) ImGui::TextColored(ImVec4(0.72f, 0.55f, 1.0f, 1.0f), "Vision: %s", AppState::VisionMaskLabel(e.visionMask).c_str());
        ImGui::EndTooltip();
    }

    if (ImGui::BeginPopupContextItem("##cellmenu")) {
        if (ImGui::MenuItem("Show in Explorer")) ShowInExplorer(e.path);
        if (ImGui::MenuItem("Copy file path")) ImGui::SetClipboardText(WideToUtf8(e.path).c_str());
        NsfwMarkMenu(app, e.path);
        ImGui::Separator();
        {
            const std::vector<int> targets = ActionTargets(app, i);
            const std::string plural = targets.size() > 1 ? " (" + std::to_string(targets.size()) + ")" : std::string();
            if (ImGui::MenuItem(("Copy image" + plural).c_str(), "Ctrl+C")) CopyPictures(app, targets, false);
            if (ImGui::MenuItem(("Copy image without metadata" + plural).c_str(), "Ctrl+Shift+C")) CopyPictures(app, targets, true);
            if (ImGui::BeginMenu(("Add to Vision board" + plural).c_str())) {
                std::vector<std::wstring> paths;
                for (int idx : targets) {
                    if (idx >= 0 && idx < static_cast<int>(app.entries.size())) paths.push_back(app.entries[idx].path);
                }
                if (ImGui::MenuItem("As characters")) AddImagesToBoard(app, paths, 0);
                if (ImGui::MenuItem("As the background")) AddImagesToBoard(app, paths, 1);
                if (ImGui::MenuItem("As items")) AddImagesToBoard(app, paths, 2);
                ImGui::EndMenu();
            }
            {
                auto sendAll = [&](bool useDefaults, bool negative, bool picture) {
                    for (int idx : targets) {
                        if (idx < 0 || idx >= static_cast<int>(app.entries.size())) continue;
                        const ImageEntry& en = app.entries[idx];
                        SendToComfy(app, en.metadata.parsed.positivePrompt, negative ? en.metadata.parsed.negativePrompt : std::string(),
                                    picture ? en.path : std::wstring(), WideToUtf8(fs::path(en.path).filename().wstring()), useDefaults);
                    }
                };
                const bool hasPrompt = !e.metadata.parsed.positivePrompt.empty();
                if (ImGui::MenuItem(("Send prompt to ComfyUI" + plural).c_str(), nullptr, false, hasPrompt)) sendAll(true, true, true);
                if (!e.metadata.isVideo && ImGui::MenuItem("Star in a video...")) StarInVideo(app, e.path);
                if (ImGui::MenuItem("Test LoRA mixes on it...")) TestLoraMixesOn(app, e.path);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Made again (its own ComfyUI workflow and seed) with 2-3 mixes of LoRAs, side by side");
                if (ImGui::MenuItem(("Edit with Qwen Image 2.1..." + plural).c_str())) {
                    std::vector<std::wstring> paths;
                    for (int idx : targets) {
                        if (idx >= 0 && idx < static_cast<int>(app.entries.size())) paths.push_back(app.entries[idx].path);
                    }
                    OpenPictureEditor(app, paths);
                }
                if (ImGui::MenuItem(("Regrade with Clef (NSFW, quality, style)" + plural).c_str(), nullptr, false, !ClefGradeRunning())) {
                    std::vector<ClefGradeInput> items;
                    for (int idx : targets) {
                        if (idx < 0 || idx >= static_cast<int>(app.entries.size())) continue;
                        const ImageEntry& en2 = app.entries[idx];
                        items.push_back({en2.path, static_cast<long long>(en2.mtime.time_since_epoch().count()), en2.size});
                    }
                    ClefGradeStart(items, app.comfy.url, false);
                }
                if (ImGui::MenuItem("Find pictures like this", nullptr, false, EmbedAvailable() && !e.metadata.isVideo)) EmbedFindLike(app, i);
                if (ImGui::MenuItem(("Add to the Clef mode cast" + plural).c_str())) {
                    std::vector<std::wstring> paths;
                    for (int idx : targets) {
                        if (idx >= 0 && idx < static_cast<int>(app.entries.size())) paths.push_back(app.entries[idx].path);
                    }
                    AddToClefCast(app, paths);
                }
                if (ImGui::MenuItem(("Upscale (bigger, better faces)..." + plural).c_str())) {
                    std::vector<std::wstring> paths;
                    for (int idx : targets) {
                        if (idx >= 0 && idx < static_cast<int>(app.entries.size())) paths.push_back(app.entries[idx].path);
                    }
                    OpenUpscaler(app, paths);
                }
                if (ImGui::MenuItem(targets.size() > 1 ? "Design with PhotoCraft (the others placed into the first)..." : "Design with PhotoCraft...", nullptr, false, !e.metadata.isVideo)) {
                    std::vector<std::wstring> paths;
                    for (int idx : targets) {
                        if (idx >= 0 && idx < static_cast<int>(app.entries.size())) paths.push_back(app.entries[idx].path);
                    }
                    OpenDesigner(app, paths);
                }
                if (ImGui::BeginMenu(("Pixel art" + std::string(targets.size() > 1 ? " (these)" : "")).c_str(), !e.metadata.isVideo)) {
                    std::vector<std::wstring> paths;
                    for (int idx : targets) {
                        if (idx >= 0 && idx < static_cast<int>(app.entries.size())) paths.push_back(app.entries[idx].path);
                    }
                    PixelArtQuickContents(app, paths);
                    ImGui::EndMenu();
                }
                if (ImGui::MenuItem("Edit the prompt, then send...", nullptr, false, hasPrompt)) {
                    OpenSendEditor(app, e.metadata.parsed.positivePrompt, e.metadata.parsed.negativePrompt, e.path, WideToUtf8(fs::path(e.path).filename().wstring()));
                }
                if (ImGui::BeginMenu("More ComfyUI options", hasPrompt)) {
                    if (ImGui::MenuItem("Prompt only")) sendAll(false, false, false);
                    if (ImGui::MenuItem("Prompt and negative prompt")) sendAll(false, true, false);
                    if (ImGui::MenuItem("Prompt and this picture as the input image")) sendAll(false, false, true);
                    ImGui::Separator();
                    if (ImGui::BeginMenu("Workflow to run")) {
                        ComfyWorkflowMenu(app);
                        ImGui::EndMenu();
                    }
                    if (ImGui::MenuItem("Generate page...")) app.requestTab = 4;
                    ImGui::EndMenu();
                }
            }
        }
        if (ImGui::MenuItem("Copy prompt", nullptr, false, !e.metadata.parsed.positivePrompt.empty())) {
            ImGui::SetClipboardText(e.metadata.parsed.positivePrompt.c_str());
        }
        if (ImGui::MenuItem("Copy as AUTOMATIC1111 text", nullptr, false, e.metadata.hasMetadata)) {
            ImGui::SetClipboardText(BuildA1111Text(e.metadata).c_str());
            app.SetStatus("Copied prompt and settings.");
        }
        {
            // Gallery copies leave out whole workflows, so the file is read when one is actually used.
            const unsigned mask = e.metadata.workflowMask;
            const unsigned copyable = mask & ~WorkflowBit(WorkflowKind::Parameters);
            if (copyable && ImGui::BeginMenu("Workflow")) {
                for (WorkflowKind kind : {WorkflowKind::ComfyUI, WorkflowKind::ComfyApi, WorkflowKind::InvokeAI, WorkflowKind::NovelAI}) {
                    if (!(mask & WorkflowBit(kind))) continue;
                    const std::string label = std::string("Copy ") + WorkflowKindLabel(kind);
                    if (ImGui::MenuItem(label.c_str())) {
                        const ImageMetadata full = ReadImageMetadata(e.path);
                        for (const EmbeddedWorkflow& wf : FindWorkflows(full)) {
                            if (wf.kind == kind) {
                                ImGui::SetClipboardText(wf.text->c_str());
                                app.SetStatus(std::string("Copied the ") + WorkflowKindLabel(kind) + ".");
                                break;
                            }
                        }
                    }
                }
                if (mask & ViewableWorkflowMask()) {
                    ImGui::Separator();
                    if (ImGui::MenuItem("View in the node viewer")) OpenInNodeViewer(app, e.path, ReadImageMetadata(e.path));
                    if (ImGui::MenuItem("Save to the workflow library")) {
                        bool existed = false;
                        std::string err;
                        std::wstring saved = SaveWorkflowToLibrary(app.Device(), e.path, ReadImageMetadata(e.path), &existed, &err);
                        app.wf.libDirty = true;
                        app.SetStatus(saved.empty() ? err : (existed ? "That workflow is already in the library." : "Saved the workflow to the library."));
                    }
                }
                ImGui::EndMenu();
            }
        }
        {
            const std::vector<int> targets = ActionTargets(app, i);
            const std::string head = targets.size() > 1 ? "Describe " + std::to_string(targets.size()) + " with Vision" : "Describe with Vision";
            if (ImGui::BeginMenu(head.c_str())) {
                if (ImGui::MenuItem("Image prompt")) RunVisionOn(app, AppState::VisionMode::ImagePrompt, 0, targets);
                if (ImGui::MenuItem("Tag list")) RunVisionOn(app, AppState::VisionMode::Tags, 0, targets);
                if (ImGui::BeginMenu("Video scene")) {
                    for (int sec : {5, 10, 20, 30}) {
                        char label[24];
                        snprintf(label, sizeof(label), "%d seconds", sec);
                        if (ImGui::MenuItem(label)) RunVisionOn(app, AppState::VisionMode::VideoScene, sec, targets);
                    }
                    ImGui::EndMenu();
                }
                ImGui::EndMenu();
            }
        }
        if (ImGui::BeginMenu("Rate")) {
            const std::vector<int> targets = ActionTargets(app, i);
            for (int r = 5; r >= 1; --r) {
                char label[32];
                snprintf(label, sizeof(label), "%d star%s", r, r == 1 ? "" : "s");
                if (ImGui::MenuItem(label, std::to_string(r).c_str(), e.rating == r)) ApplyRating(app, targets, r);
            }
            if (ImGui::MenuItem("Clear rating", "0")) ApplyRating(app, targets, 0);
            ImGui::EndMenu();
        }
        if (app.multiSelected.size() == 2 && app.multiSelected.count(i)) {
            if (ImGui::MenuItem("Compare the two selected images", "C")) app.OpenCompare(*app.multiSelected.begin(), *app.multiSelected.rbegin());
        }
        if (ImGui::BeginMenu("Vision")) {
            if (ImGui::MenuItem("Write an image prompt")) RunVision(app, AppState::VisionMode::ImagePrompt, 0, i);
            if (ImGui::MenuItem("Write a tag list")) RunVision(app, AppState::VisionMode::Tags, 0, i);
            ImGui::Separator();
            ImGui::TextDisabled("Video scene (image = first frame)");
            for (int s : {5, 10, 15, 20, 30}) {
                char label[32];
                snprintf(label, sizeof(label), "%d seconds", s);
                if (ImGui::MenuItem(label)) RunVision(app, AppState::VisionMode::VideoScene, s, i);
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Export clean copy (no metadata)...")) {
            if (!app.multiSelected.count(i)) {
                app.multiSelected.clear();
                app.multiSelected.insert(i);
            }
            app.cleanMessage.clear();
            app.showCleanExport = true;
        }
        if (ImGui::MenuItem("Rename by metadata...")) {
            if (!app.multiSelected.count(i)) {
                app.multiSelected.clear();
                app.multiSelected.insert(i);
            }
            app.renameScope = 0;
            app.renameMessage.clear();
            app.showRename = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete (Recycle Bin)...", "Del")) QueueDelete(app, i);
        ImGui::EndPopup();
    }

    // "NEW": it arrived while the folder was open and nobody has looked at it yet (half an hour at most)
    if (e.arrivedTick && GetTickCount64() - e.arrivedTick < 30ull * 60 * 1000 && !hovered) {
        const char* txt = "NEW";
        const ImVec2 ts = ImGui::CalcTextSize(txt);
        const float nsfwH = (app.showNsfwBadges && EffectiveNsfwScore(e, app.nsfwSource) >= 0.0f) ? ts.y + ui::Dp(6.0f) : 0.0f;
        const ImVec2 q0(a.x + ui::Dp(5.0f), a.y + ui::Dp(5.0f) + nsfwH);
        dl->AddRectFilled(q0, ImVec2(q0.x + ts.x + ui::Dp(10.0f), q0.y + ts.y + ui::Dp(2.0f)), ImGui::GetColorU32(ui::kAccent), ui::Dp(9.0f));
        dl->AddText(ImVec2(q0.x + ui::Dp(5.0f), q0.y + ui::Dp(1.0f)), IM_COL32(255, 255, 255, 255), txt);
    }

    // Hover actions (from HeliosGen's gallery cards), drawn over the picture: the tick box to pick several (like Ctrl+click; on every card
    // while some are picked) and the common actions over a dark fade at the bottom. Their buttons were made before the cell (see above).
    if (showPick) {
        const bool checked = app.multiSelected.count(i) > 0;
        if (pickHov) ImGui::SetTooltip(checked ? "Unpick (Ctrl+click)" : "Pick it too (Ctrl+click) - then act on all of them");
        const float r = pickS * 0.36f;
        if (checked) {
            dl->AddRectFilled(ImVec2(pickC.x - r, pickC.y - r), ImVec2(pickC.x + r, pickC.y + r), ImGui::GetColorU32(ui::kAccent), ui::Dp(4.0f));
            DrawNavIcon(dl, 12, pickC, r * 1.25f, IM_COL32(255, 255, 255, 255));
        } else {
            dl->AddRectFilled(ImVec2(pickC.x - r, pickC.y - r), ImVec2(pickC.x + r, pickC.y + r), IM_COL32(8, 10, 14, pickHov ? 200 : 140), ui::Dp(4.0f));
            dl->AddRect(ImVec2(pickC.x - r, pickC.y - r), ImVec2(pickC.x + r, pickC.y + r), IM_COL32(255, 255, 255, pickHov ? 230 : 170), ui::Dp(4.0f), 0, ui::Dp(1.5f));
        }
    }
    if (showQuick) {
        const float fade = std::min(ui::Dp(54.0f), (b.y - a.y) * 0.45f);
        dl->AddRectFilledMultiColor(ImVec2(a.x, b.y - fade), ImVec2(b.x, b.y - imgRound), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 170), IM_COL32(0, 0, 0, 170));
        dl->AddRectFilled(ImVec2(a.x, b.y - imgRound), b, IM_COL32(0, 0, 0, 170), imgRound, ImDrawFlags_RoundCornersBottom);
        auto paths = [&]() {
            std::vector<std::wstring> out;
            for (int idx : ActionTargets(app, i)) {
                if (idx >= 0 && idx < static_cast<int>(app.entries.size())) out.push_back(app.entries[idx].path);
            }
            return out;
        };
        for (const QuickBtn& q : quick) {
            dl->AddCircleFilled(q.c, quickS * 0.5f, q.hov && q.enabled ? IM_COL32(255, 255, 255, 46) : IM_COL32(8, 10, 14, 165), 24);
            dl->AddCircle(q.c, quickS * 0.5f, IM_COL32(255, 255, 255, q.hov ? 70 : 28), 24, 1.0f);
            DrawNavIcon(dl, q.icon, q.c, quickS * 0.46f, q.enabled ? IM_COL32(245, 246, 250, 255) : IM_COL32(150, 154, 162, 160));
            if (q.hov) ImGui::SetTooltip("%s", q.tip);
            if (!q.clicked) continue;
            const std::string id = q.id;
            if (id == "##qcopy") {
                const std::string& prompt = e.metadata.parsed.positivePrompt;
                ImGui::SetClipboardText(prompt.c_str());
                Toast(ToastKind::Info, "Prompt copied", prompt.size() > 90 ? prompt.substr(0, 90) + "..." : prompt, {}, {}, "copyprompt");
            } else if (id == "##qedit") {
                OpenPictureEditor(app, paths());
            } else if (id == "##qpixel") {
                QuickPixelArt(app, paths());
            } else if (id == "##qanim") {
                OpenVideoPage(app, e.path, e.metadata.parsed.positivePrompt);
            } else if (id == "##qmore") {
                if (!selected) app.SelectIndex(i);
                ImGui::OpenPopup("##cellmenu");
            }
        }
    }

    return clicked;
}

// Search mini-syntax, one SearchTerm per comma-separated piece:
//   worst quality        -> plain substring match against prompt/tags/filename
//   -worst quality       -> plain substring EXCLUSION
//   sampler:euler         -> substring match against a specific parameter's value
//   steps>20, cfg<=7      -> numeric comparison against a specific parameter's value
// All terms are AND-ed together.
struct SearchTerm {
    enum class Kind { PlainInclude, PlainExclude, ParamSubstring, ParamNumeric } kind = Kind::PlainInclude;
    std::string text;          // plain text (Plain*), or the operator (ParamNumeric)
    std::string paramKey;
    std::string paramValueStr; // lowercased RHS (ParamSubstring)
    double numericRHS = 0.0;
};

std::string TrimStr(const std::string& s) {
    size_t start = s.find_first_not_of(" \t");
    size_t end = s.find_last_not_of(" \t");
    if (start == std::string::npos) return "";
    return s.substr(start, end - start + 1);
}

SearchTerm ParseSearchTerm(const std::string& raw) {
    std::string t = TrimStr(raw);
    SearchTerm term;
    if (!t.empty() && t[0] == '-') {
        term.kind = SearchTerm::Kind::PlainExclude;
        term.text = ToLowerStr(TrimStr(t.substr(1)));
        return term;
    }
    static const char* kOps[] = {">=", "<=", ">", "<", "=", ":"};
    for (const char* op : kOps) {
        size_t pos = t.find(op);
        if (pos == std::string::npos || pos == 0) continue;
        std::string key = TrimStr(t.substr(0, pos));
        std::string val = TrimStr(t.substr(pos + strlen(op)));
        if (key.empty() || val.empty()) continue;

        if (std::string(op) == ":") {
            term.kind = SearchTerm::Kind::ParamSubstring;
            term.paramKey = key;
            term.paramValueStr = ToLowerStr(val);
            return term;
        }
        try {
            size_t consumed = 0;
            double rhs = std::stod(val, &consumed);
            if (consumed == val.size()) {
                term.kind = SearchTerm::Kind::ParamNumeric;
                term.paramKey = key;
                term.text = op;
                term.numericRHS = rhs;
                return term;
            }
        } catch (...) {
        }
        // Operator matched but the RHS isn't a clean number: fall back to a
        // substring match on the param instead of silently ignoring the term.
        term.kind = SearchTerm::Kind::ParamSubstring;
        term.paramKey = key;
        term.paramValueStr = ToLowerStr(val);
        return term;
    }
    term.kind = SearchTerm::Kind::PlainInclude;
    term.text = ToLowerStr(t);
    return term;
}

std::vector<SearchTerm> SplitSearchTerms(const std::string& query) {
    std::vector<SearchTerm> terms;
    std::string current;
    auto flush = [&]() {
        std::string trimmed = TrimStr(current);
        if (!trimmed.empty()) terms.push_back(ParseSearchTerm(trimmed));
        current.clear();
    };
    for (char c : query) {
        if (c == ',') flush();
        else current += c;
    }
    flush();
    return terms;
}

std::string FindParamCI(const KeyValueList& params, const std::string& key) {
    for (auto& kv : params) {
        if (kv.first.size() == key.size() && _strnicmp(kv.first.c_str(), key.c_str(), key.size()) == 0) {
            return kv.second;
        }
    }
    return "";
}

// 1 portrait, 2 landscape, 3 square, 0 unknown. Prefers the size the image was
// generated at; falls back to the thumbnail's aspect ratio.
int OrientationOf(const ImageEntry& e) {
    int w = e.genW, h = e.genH;
    if (w <= 0 || h <= 0) {
        w = e.thumbW;
        h = e.thumbH;
    }
    if (w <= 0 || h <= 0) return 0;
    float r = static_cast<float>(w) / h;
    return r > 1.05f ? 2 : (r < 0.95f ? 1 : 3);
}

bool CompareNumber(double lhs, const std::string& op, double rhs) {
    if (op == ">=") return lhs >= rhs;
    if (op == "<=") return lhs <= rhs;
    if (op == ">") return lhs > rhs;
    if (op == "<") return lhs < rhs;
    return lhs == rhs;
}

// Keys with built-in meaning: year, month, people, generator, orient. Returns
// true (and sets `result`) when the term's key is one of them.
//   year:2026  year>=2025          month:3  month:march  month>=6
//   people:2   people:solo  people:none  people:3+  people>=2
//   generator:comfy                orient:portrait|landscape|square
bool MatchSpecialKey(const ImageEntry& e, const SearchTerm& term, bool& result) {
    const std::string& key = term.paramKey;
    const bool numeric = term.kind == SearchTerm::Kind::ParamNumeric;
    if (term.kind != SearchTerm::Kind::ParamSubstring && !numeric) return false;
    const std::string& v = term.paramValueStr; // lowercased RHS for ":" terms

    if (_stricmp(key.c_str(), "style") == 0) {
        if (numeric || e.styleKind < 0) { result = false; return true; }
        std::string name = kStyleNames[e.styleKind];
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        result = !v.empty() && name.find(v) != std::string::npos; // style:anime, style:real, style:3d, style:photo
        return true;
    }
    if (_stricmp(key.c_str(), "quality") == 0) {
        const float q = e.Quality();
        if (!numeric || q < 0) { result = false; return true; }
        if (term.text == ">=") result = q >= term.numericRHS;
        else if (term.text == "<=") result = q <= term.numericRHS;
        else if (term.text == ">") result = q > term.numericRHS;
        else if (term.text == "<") result = q < term.numericRHS;
        else result = std::fabs(q - term.numericRHS) < 0.05;
        return true;
    }
    if (_stricmp(key.c_str(), "year") == 0) {
        if (e.year == 0) { result = false; return true; }
        if (numeric) result = CompareNumber(e.year, term.text, term.numericRHS);
        else result = std::to_string(e.year).find(v) != std::string::npos;
        return true;
    }
    if (_stricmp(key.c_str(), "month") == 0) {
        if (e.month == 0) { result = false; return true; }
        if (numeric) { result = CompareNumber(e.month, term.text, term.numericRHS); return true; }
        int mo = ParseMonth(v);
        result = mo != 0 && e.month == mo;
        return true;
    }
    if (_stricmp(key.c_str(), "people") == 0 || _stricmp(key.c_str(), "persons") == 0) {
        if (e.people < 0) { result = false; return true; }
        if (numeric) { result = CompareNumber(e.people, term.text, term.numericRHS); return true; }
        if (v == "solo" || v == "one") result = e.people == 1;
        else if (v == "none" || v == "no" || v == "0" || v == "nobody") result = e.people == 0;
        else if (v == "duo" || v == "couple") result = e.people == 2;
        else if (v == "group" || v == "crowd" || v == "many" || v == "multiple") result = e.people >= 3;
        else if (!v.empty() && std::isdigit(static_cast<unsigned char>(v[0]))) {
            int n = std::atoi(v.c_str());
            result = v.back() == '+' ? e.people >= n : e.people == n;
        } else {
            result = false;
        }
        return true;
    }
    if (_stricmp(key.c_str(), "generator") == 0 || _stricmp(key.c_str(), "gen") == 0) {
        result = !numeric && ToLowerStr(GeneratorLabel(e.metadata.generator)).find(v) != std::string::npos;
        return true;
    }
    if (_stricmp(key.c_str(), "rating") == 0 || _stricmp(key.c_str(), "stars") == 0) {
        if (numeric) { result = CompareNumber(e.rating, term.text, term.numericRHS); return true; }
        if (v == "none" || v == "unrated" || v == "0") result = e.rating == 0;
        else if (!v.empty() && std::isdigit(static_cast<unsigned char>(v[0]))) {
            int n = std::atoi(v.c_str());
            result = v.back() == '+' ? e.rating >= n : e.rating == n;
        } else {
            result = false;
        }
        return true;
    }
    if (_stricmp(key.c_str(), "type") == 0 || _stricmp(key.c_str(), "kind") == 0) {
        if (numeric) { result = false; return true; }
        if (v == "video" || v == "videos" || v == "clip") result = e.metadata.isVideo;
        else if (v == "image" || v == "images" || v == "picture") result = !e.metadata.isVideo;
        else result = false;
        return true;
    }
    if (_stricmp(key.c_str(), "vision") == 0) {
        if (numeric) { result = false; return true; }
        if (v == "done" || v == "yes" || v == "described" || v == "any") result = e.visionMask != 0;
        else if (v == "none" || v == "no" || v == "todo" || v == "new") result = e.visionMask == 0;
        else if (v == "prompt" || v == "image") result = (e.visionMask & 1) != 0;
        else if (v == "tags") result = (e.visionMask & 2) != 0;
        else if (v == "video") result = (e.visionMask & 4) != 0;
        else result = false;
        return true;
    }
    if (_stricmp(key.c_str(), "folder") == 0) {
        std::string dir = ToLowerStr(WideToUtf8(fs::path(e.displayName).parent_path().wstring()));
        result = !numeric && !v.empty() && dir.find(v) != std::string::npos;
        return true;
    }
    if (_stricmp(key.c_str(), "orient") == 0 || _stricmp(key.c_str(), "orientation") == 0) {
        int o = OrientationOf(e);
        const char* names[] = {"", "portrait", "landscape", "square"};
        result = !numeric && o != 0 && !v.empty() && std::string(names[o]).find(v) == 0;
        return true;
    }
    return false;
}

bool MatchesTerm(const ImageEntry& e, const SearchTerm& term, NsfwSource nsfwSource) {
    bool special = false;
    if (MatchSpecialKey(e, term, special)) return special;
    // "nsfw>0.5" / "nsfw<=0.3" filter on the NSFW score rather than a parameter.
    if (term.kind == SearchTerm::Kind::ParamNumeric && _stricmp(term.paramKey.c_str(), "nsfw") == 0) {
        float val = EffectiveNsfwScore(e, nsfwSource);
        if (val < 0.0f) return false;
        if (term.text == ">=") return val >= term.numericRHS;
        if (term.text == "<=") return val <= term.numericRHS;
        if (term.text == ">") return val > term.numericRHS;
        if (term.text == "<") return val < term.numericRHS;
        return false;
    }
    switch (term.kind) {
        case SearchTerm::Kind::PlainInclude:
            return e.searchTextLower.find(term.text) != std::string::npos;
        case SearchTerm::Kind::PlainExclude:
            return e.searchTextLower.find(term.text) == std::string::npos;
        case SearchTerm::Kind::ParamSubstring: {
            std::string v = ToLowerStr(FindParamCI(e.metadata.parsed.params, term.paramKey));
            return v.find(term.paramValueStr) != std::string::npos;
        }
        case SearchTerm::Kind::ParamNumeric: {
            std::string v = FindParamCI(e.metadata.parsed.params, term.paramKey);
            if (v.empty()) return false;
            double val;
            try {
                size_t consumed = 0;
                val = std::stod(v, &consumed);
            } catch (...) {
                return false;
            }
            if (term.text == ">=") return val >= term.numericRHS;
            if (term.text == "<=") return val <= term.numericRHS;
            if (term.text == ">") return val > term.numericRHS;
            if (term.text == "<") return val < term.numericRHS;
            if (term.text == "=") return val == term.numericRHS;
            return false;
        }
    }
    return true;
}

// Indices of entries matching the current search query and the "only show
// images with metadata" toggle (both empty/off = show everything). Entries
// whose metadata hasn't loaded yet are excluded whenever any filter is
// active, since we don't yet know whether they'd match.
std::vector<int> FilterEntries(AppState& app, const std::vector<SearchTerm>& terms) {
    std::vector<int> visible;
    visible.reserve(app.entries.size());
    app.hiddenUnreadable = 0;
    const AppState::GalleryFilters& f = app.filters;
    const std::wstring dirW = f.dir.empty() ? std::wstring() : Utf8ToWide(f.dir);
    std::vector<float> sem; // the similarity of every picture to the meaning query, when there is one
    if (app.semanticOn && !app.semanticVec.empty()) sem = EmbedScoreEntries(app, app.semanticVec);
    bool anyFilterActive = app.metadataMode != 0 || !terms.empty() || f.ActiveCount() > 0 || app.restrictActive || !sem.empty();
    for (int i = 0; i < static_cast<int>(app.entries.size()); ++i) {
        const ImageEntry& e = app.entries[i];
        if (e.missing) continue;
        if (e.thumbReady && !e.thumbSrv && !app.showUnreadable) { // not a readable picture (damaged, or a stub file)
            ++app.hiddenUnreadable;
            continue;
        }
        if (anyFilterActive && !e.thumbReady) continue;
        if (app.metadataMode == 1 && !e.metadata.hasMetadata) continue;
        if (app.metadataMode == 2 && e.metadata.hasMetadata) continue;
        if (f.year != 0 && e.year != f.year) continue;
        if (f.month != 0 && e.month != f.month) continue;
        if (f.generator != AppState::GalleryFilters::kAny && static_cast<int>(e.metadata.generator) != f.generator) continue;
        if (f.people != AppState::GalleryFilters::kAny) {
            if (f.people == AppState::GalleryFilters::kUnknownPeople) {
                if (e.people >= 0) continue;
            } else if (f.people >= 4) {
                if (e.people < 4) continue;
            } else if (e.people != f.people) {
                continue;
            }
        }
        if (app.restrictActive && (i >= static_cast<int>(app.restrictMask.size()) || !app.restrictMask[i])) continue;
        if (!sem.empty() && sem[i] < app.semanticMin) continue;
        if (f.vision == 1 && e.visionMask == 0) continue;
        if (f.vision == 2 && e.visionMask != 0) continue;
        if (f.style != 0) {
            if (f.style < 0 ? e.styleKind >= 0 : e.styleKind != f.style - 1) continue;
        }
        if (f.minQuality != 0) {
            const float q = e.Quality();
            if (f.minQuality < 0 ? q >= 0 : q < static_cast<float>(f.minQuality)) continue;
        }
        if (f.minRating > 0 && e.rating < f.minRating) continue;
        if (f.minRating < 0 && e.rating != 0) continue;
        if (!f.folder.empty()) {
            std::string top = TopFolderOf(e.displayName);
            if (f.folder == "." ? !top.empty() : top != f.folder) continue;
        }
        if (!dirW.empty()) { // inside the folder picked in the folder tree
            const std::wstring& name = e.displayName;
            if (name.size() <= dirW.size() + 1 || _wcsnicmp(name.c_str(), dirW.c_str(), dirW.size()) != 0 ||
                (name[dirW.size()] != L'\\' && name[dirW.size()] != L'/')) continue;
        }
        if (f.orientation != 0 && OrientationOf(e) != f.orientation) continue;
        if (f.kind == 1 && e.metadata.isVideo) continue;
        if (f.kind == 2 && !e.metadata.isVideo) continue;
        if (f.pixel != 0) { // pixel art: the "_px.png" results of the Pixel art tab; the rest of its PixelArt folder is what it made on the way
            const std::wstring& p = e.path;
            const bool px = p.size() > 7 && _wcsicmp(p.c_str() + p.size() - 7, L"_px.png") == 0;
            std::wstring lower = p;
            for (wchar_t& ch : lower) ch = static_cast<wchar_t>(towlower(ch));
            const bool inPixelFolder = lower.find(L"\\pixelart\\") != std::wstring::npos;
            if (f.pixel == 1 && (px || inPixelFolder)) continue;
            if (f.pixel == 2 && !px) continue;
        }
        bool allMatch = true;
        for (const auto& t : terms) {
            if (!MatchesTerm(e, t, app.nsfwSource)) {
                allMatch = false;
                break;
            }
        }
        if (allMatch) visible.push_back(i);
    }
    app.semanticScore = std::move(sem);
    return visible;
}

void SortVisible(AppState& app, std::vector<int>& visible) {
    using SortMode = AppState::SortMode;
    if (app.semanticOn && app.semanticScore.size() == app.entries.size() && !app.semanticVec.empty()) { // by meaning: the closest first
        std::stable_sort(visible.begin(), visible.end(), [&](int a, int b) { return app.semanticScore[a] > app.semanticScore[b]; });
        return;
    }
    // The scan delivers entries sorted by name, so name order needs no work at all.
    if (app.sortMode == SortMode::Name && app.EntriesSortedByName()) {
        if (app.sortDescending) std::reverse(visible.begin(), visible.end());
        return;
    }
    auto less = [&](int a, int b) {
        const ImageEntry& ea = app.entries[a];
        const ImageEntry& eb = app.entries[b];
        bool result;
        switch (app.sortMode) {
            case SortMode::DateModified: result = ea.mtime < eb.mtime; break;
            case SortMode::FileSize: result = ea.size < eb.size; break;
            case SortMode::Generator: result = std::string(GeneratorLabel(ea.metadata.generator)) <
                                                std::string(GeneratorLabel(eb.metadata.generator)); break;
            case SortMode::NsfwScore: result = EffectiveNsfwScore(ea, app.nsfwSource) <
                                                EffectiveNsfwScore(eb, app.nsfwSource); break;
            case SortMode::Rating: result = ea.rating < eb.rating; break;
            case SortMode::Quality: result = ea.Quality() < eb.Quality(); break;
            case SortMode::Style: result = ea.styleKind < eb.styleKind; break;
            case SortMode::People: result = ea.people < eb.people; break;
            case SortMode::Model: result = ea.modelLower < eb.modelLower; break;
            case SortMode::Resolution: result = static_cast<long long>(ea.genW) * ea.genH <
                                                 static_cast<long long>(eb.genW) * eb.genH; break;
            case SortMode::Name:
            default: result = _wcsicmp(ea.displayName.c_str(), eb.displayName.c_str()) < 0; break;
        }
        return app.sortDescending ? !result : result;
    };
    std::stable_sort(visible.begin(), visible.end(), less);
}

// Header text for the group an image belongs to under the current sort.
std::string GroupLabelFor(const AppState& app, const ImageEntry& e) {
    using SM = AppState::SortMode;
    switch (app.sortMode) {
        case SM::DateModified:
            return e.year ? std::string(MonthName(e.month)) + " " + std::to_string(e.year) : "Unknown date";
        case SM::Generator:
            return e.thumbReady ? GeneratorName(e.metadata.generator) : "Loading...";
        case SM::People:
            if (e.people < 0) return "People: unknown";
            if (e.people == 0) return "No people";
            if (e.people == 1) return "1 person (solo)";
            return e.people >= 6 ? "6 or more people" : std::to_string(e.people) + " people";
        case SM::Model:
            return e.modelLower.empty() ? "No model recorded" : e.modelLower;
        case SM::Rating:
            return e.rating ? std::to_string(e.rating) + (e.rating == 1 ? " star" : " stars") : "Unrated";
        case SM::Style:
            return e.styleKind < 0 || e.styleKind >= kStyleCount ? "Not classified yet" : kStyleNames[e.styleKind];
        case SM::Quality: {
            const float q = e.Quality();
            if (q < 0) return "Not scored yet";
            const int band = std::min(9, static_cast<int>(q));
            return std::to_string(band) + " - " + std::to_string(band + 1);
        }
        case SM::NsfwScore: {
            float s = EffectiveNsfwScore(e, app.nsfwSource);
            if (s < 0) return "Not scored";
            if (s >= app.nsfwThreshold) return "NSFW";
            if (app.nsfwUseQuestionable && s >= app.nsfwQuestionableLow) return "Questionable";
            return "Safe";
        }
        case SM::FileSize:
            if (e.size < 256ull * 1024) return "Under 256 KB";
            if (e.size < 1024ull * 1024) return "256 KB - 1 MB";
            if (e.size < 4ull * 1024 * 1024) return "1 - 4 MB";
            if (e.size < 16ull * 1024 * 1024) return "4 - 16 MB";
            return "16 MB and larger";
        case SM::Resolution: {
            double mp = static_cast<double>(e.genW) * e.genH / 1e6;
            if (e.genW <= 0) return "Size unknown";
            if (mp < 0.4) return "Under 0.4 megapixels";
            if (mp < 1.2) return "About 1 megapixel";
            if (mp < 2.5) return "1.2 - 2.5 megapixels";
            if (mp < 5.0) return "2.5 - 5 megapixels";
            return "5 megapixels and up";
        }
        case SM::Name:
        default: {
            std::wstring name = fs::path(e.displayName).filename().wstring();
            if (name.empty()) return "#";
            wchar_t c = name[0];
            if (iswdigit(c)) return "0-9";
            if (iswalpha(c)) return WideToUtf8(std::wstring(1, static_cast<wchar_t>(towupper(c))));
            return "#";
        }
    }
}

// Cheap integer identity of the group an image falls in under the current sort (the
// header text is only built once per group, not once per image).
unsigned long long GroupKeyFor(const AppState& app, const ImageEntry& e) {
    using SM = AppState::SortMode;
    switch (app.sortMode) {
        case SM::DateModified: return static_cast<unsigned long long>(e.year) * 100 + e.month;
        case SM::Generator: return e.thumbReady ? static_cast<unsigned long long>(e.metadata.generator) + 1 : 0;
        case SM::People: return static_cast<unsigned long long>(e.people + 2);
        case SM::Model: return std::hash<std::string>()(e.modelLower);
        case SM::Rating: return static_cast<unsigned long long>(e.rating);
        case SM::Style: return static_cast<unsigned long long>(e.styleKind + 1);
        case SM::Quality: {
            const float q = e.Quality();
            return q < 0 ? 0ull : 1ull + static_cast<unsigned long long>(std::min(9, static_cast<int>(q)));
        }
        case SM::NsfwScore: {
            float s = EffectiveNsfwScore(e, app.nsfwSource);
            if (s < 0) return 0;
            if (s >= app.nsfwThreshold) return 3;
            return (app.nsfwUseQuestionable && s >= app.nsfwQuestionableLow) ? 2 : 1;
        }
        case SM::FileSize:
            return e.size < 256ull * 1024 ? 0 : e.size < 1024ull * 1024 ? 1 : e.size < 4ull * 1024 * 1024 ? 2 : e.size < 16ull * 1024 * 1024 ? 3 : 4;
        case SM::Resolution: {
            if (e.genW <= 0) return 0;
            double mp = static_cast<double>(e.genW) * e.genH / 1e6;
            return mp < 0.4 ? 1 : mp < 1.2 ? 2 : mp < 2.5 ? 3 : mp < 5.0 ? 4 : 5;
        }
        case SM::Name:
        default: {
            size_t slash = e.displayName.find_last_of(L"\\/");
            wchar_t c = e.displayName[slash == std::wstring::npos ? 0 : slash + 1];
            if (iswdigit(c)) return '0';
            return iswalpha(c) ? static_cast<unsigned long long>(towupper(c)) : '#';
        }
    }
}

// Rows of the gallery: optional group headers followed by rows of thumbnails.
struct GridRow {
    float y = 0.0f, h = 0.0f;
    bool header = false;
    int first = 0, count = 0; // thumbnails: visible positions [first, first+count); headers: the group's range
    std::string label;
};

struct GridLayout {
    std::vector<GridRow> rows;
    std::vector<int> rowOfPos; // visible position -> row index
    float totalHeight = 0.0f;
};

void BuildGridLayout(GridLayout& L, const AppState& app, const std::vector<int>& visible, int columns, float cell,
                     float labelH, float gap, float headerH, bool grouped) {
    L.rows.clear();
    L.rowOfPos.assign(visible.size(), 0);
    const float rowH = cell + labelH + gap;
    const int n = static_cast<int>(visible.size());
    float y = 0.0f;
    int pos = 0;
    while (pos < n) {
        int groupEnd = n;
        if (grouped) {
            const unsigned long long key = GroupKeyFor(app, app.entries[visible[pos]]);
            std::string label = GroupLabelFor(app, app.entries[visible[pos]]);
            int j = pos + 1;
            while (j < n && GroupKeyFor(app, app.entries[visible[j]]) == key) ++j;
            groupEnd = j;
            GridRow header;
            header.y = y;
            header.h = headerH;
            header.header = true;
            header.first = pos;
            header.count = groupEnd - pos;
            header.label = std::move(label);
            L.rows.push_back(std::move(header));
            y += headerH;
        }
        for (int p = pos; p < groupEnd; p += columns) {
            GridRow row;
            row.y = y;
            row.h = rowH;
            row.first = p;
            row.count = std::min(columns, groupEnd - p);
            for (int k = 0; k < row.count; ++k) L.rowOfPos[p + k] = static_cast<int>(L.rows.size());
            L.rows.push_back(std::move(row));
            y += rowH;
        }
        pos = groupEnd;
    }
    L.totalHeight = y;
}

// Collapsible "Filters" section: pick a generator, year, month, people count or
// orientation without typing search syntax. Years offered are the ones that
// actually occur in the current folder.
void DrawQuickFilters(AppState& app) {
    using F = AppState::GalleryFilters;
    F& f = app.filters;
    const int active = f.ActiveCount();
    char header[48];
    if (active > 0) snprintf(header, sizeof(header), "Filters  (%d active)###filters", active);
    else snprintf(header, sizeof(header), "Filters###filters");

    if (!ImGui::CollapsingHeader(header)) return;

    // Years / generators present, recomputed only when the entry list changes.
    static unsigned cachedEpoch = ~0u;
    static size_t cachedCount = 0;
    static std::vector<int> years;
    static std::vector<int> generators;
    static std::vector<std::string> folders;
    if (cachedEpoch != app.filterEpoch || cachedCount != app.entries.size()) {
        cachedEpoch = app.filterEpoch;
        cachedCount = app.entries.size();
        years.clear();
        generators.clear();
        folders.clear();
        std::set<std::string> folderSet;
        for (const ImageEntry& e : app.entries) {
            std::string top = TopFolderOf(e.displayName);
            if (!top.empty() && folderSet.size() < 500) folderSet.insert(top);
            if (e.year != 0 && std::find(years.begin(), years.end(), e.year) == years.end()) years.push_back(e.year);
            if (e.thumbReady && e.metadata.hasMetadata) {
                int g = static_cast<int>(e.metadata.generator);
                if (std::find(generators.begin(), generators.end(), g) == generators.end()) generators.push_back(g);
            }
        }
        std::sort(years.begin(), years.end(), std::greater<int>());
        std::sort(generators.begin(), generators.end());
        folders.assign(folderSet.begin(), folderSet.end());
    }

    const float labelW = ImGui::CalcTextSize("Orientation").x + ImGui::GetStyle().ItemSpacing.x;
    auto row = [&](const char* label) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ui::kTextDim, "%s", label);
        ImGui::SameLine(labelW + ImGui::GetStyle().WindowPadding.x);
        ImGui::SetNextItemWidth(-1);
    };

    row("Generation");
    {
        const char* cur = f.generator == F::kAny ? "Any" : GeneratorLabel(static_cast<Generator>(f.generator));
        if (ImGui::BeginCombo("##fgen", cur)) {
            if (ImGui::Selectable("Any", f.generator == F::kAny)) f.generator = F::kAny;
            for (int g : generators) {
                if (ImGui::Selectable(GeneratorLabel(static_cast<Generator>(g)), f.generator == g)) f.generator = g;
            }
            ImGui::EndCombo();
        }
    }

    row("Year");
    {
        std::string cur = f.year == 0 ? "Any" : std::to_string(f.year);
        if (ImGui::BeginCombo("##fyear", cur.c_str())) {
            if (ImGui::Selectable("Any", f.year == 0)) f.year = 0;
            for (int y : years) {
                if (ImGui::Selectable(std::to_string(y).c_str(), f.year == y)) f.year = y;
            }
            ImGui::EndCombo();
        }
    }

    row("Month");
    {
        const char* cur = f.month == 0 ? "Any" : MonthName(f.month);
        if (ImGui::BeginCombo("##fmonth", cur)) {
            if (ImGui::Selectable("Any", f.month == 0)) f.month = 0;
            for (int mo = 1; mo <= 12; ++mo) {
                if (ImGui::Selectable(MonthName(mo), f.month == mo)) f.month = mo;
            }
            ImGui::EndCombo();
        }
    }

    row("People");
    {
        auto label = [](int p) -> std::string {
            if (p == F::kAny) return "Any";
            if (p == F::kUnknownPeople) return "Unknown";
            if (p == 0) return "None";
            if (p == 1) return "1 (solo)";
            if (p >= 4) return "4 or more";
            return std::to_string(p);
        };
        if (ImGui::BeginCombo("##fpeople", label(f.people).c_str())) {
            for (int p : {F::kAny, 0, 1, 2, 3, 4, F::kUnknownPeople}) {
                if (ImGui::Selectable(label(p).c_str(), f.people == p)) f.people = p;
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
            ImGui::SetTooltip("Counted in the image if you ran Tools > Detect people, otherwise estimated from the prompt\n"
                              "(\"1girl\", \"2boys\", \"solo\", \"no humans\", ...). Images with neither are \"Unknown\".");
        }
    }

    row("Orientation");
    {
        static const char* kOrient[] = {"Any", "Portrait", "Landscape", "Square"};
        if (ImGui::BeginCombo("##forient", kOrient[f.orientation])) {
            for (int o = 0; o < 4; ++o) {
                if (ImGui::Selectable(kOrient[o], f.orientation == o)) f.orientation = o;
            }
            ImGui::EndCombo();
        }
    }

    row("Quality");
    {
        auto label = [](int v) -> std::string {
            if (v == 0) return "Any";
            if (v < 0) return "Not scored yet";
            return std::to_string(v) + " or better";
        };
        if (ImGui::BeginCombo("##fquality", label(f.minQuality).c_str())) {
            for (int v : {0, -1, 4, 5, 6, 7, 8}) {
                if (ImGui::Selectable(label(v).c_str(), f.minQuality == v)) f.minQuality = v;
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("From the picture-quality models (Tools > Score picture quality).");
    }

    row("Rating");
    {
        auto label = [](int r) -> std::string {
            if (r == 0) return "Any";
            if (r < 0) return "Unrated";
            return r == 5 ? "5 stars" : "At least " + std::to_string(r) + (r == 1 ? " star" : " stars");
        };
        if (ImGui::BeginCombo("##frating", label(f.minRating).c_str())) {
            for (int r : {0, -1, 1, 2, 3, 4, 5}) {
                if (ImGui::Selectable(label(r).c_str(), f.minRating == r)) f.minRating = r;
            }
            ImGui::EndCombo();
        }
    }

    if (!folders.empty()) {
        row("Folder");
        std::string cur = f.folder.empty() ? "Any" : (f.folder == "." ? "Top folder only" : f.folder);
        if (ImGui::BeginCombo("##ffolder", cur.c_str())) {
            if (ImGui::Selectable("Any", f.folder.empty())) f.folder.clear();
            if (ImGui::Selectable("Top folder only", f.folder == ".")) f.folder = ".";
            for (const std::string& name : folders) {
                if (ImGui::Selectable(name.c_str(), f.folder == name)) f.folder = name;
            }
            ImGui::EndCombo();
        }
    }

    if (ImGui::Checkbox("Show unreadable files", &app.showUnreadable)) app.SaveUiSettings();
    ImGui::BeginDisabled(active == 0);
    if (ImGui::SmallButton("Clear filters")) f.Clear();
    ImGui::EndDisabled();
    ImGui::Spacing();
}

// A small button that continues the current line when it fits and wraps to the next otherwise.
bool SmallButtonFlow(const char* label) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float w = ImGui::CalcTextSize(label, nullptr, true).x + st.FramePadding.x * 2.0f;
    const float limit = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    if (ImGui::GetItemRectMax().x + st.ItemSpacing.x + w <= limit) ImGui::SameLine();
    return ImGui::SmallButton(label);
}

// "Making now" (HeliosGen shows a glowing placeholder card where a result will land): what ComfyUI is working on for this app, as cards above the
// grid - a spinner, what it is, where it is (waiting / in the queue / generating for 0:42) and Cancel. A click opens the ComfyUI tab.
bool g_makingShown = false; // drawn this frame: keeps the spinner moving
void DrawMakingStrip(AppState& app) {
    g_makingShown = false;
    static const bool testMaking = UiTestNumber("making") >= 0;
    std::vector<ComfyActivity> live, acts;
    if (testMaking) {
        const long long now0 = ComfyService::NowMs();
        const char* labels[] = {"Qwen edit: red jacket", "H3 Turbo 4-step: fox in snow", "portrait_0042.png", "Clef mode round 3"};
        const ComfyState states[] = {ComfyState::Running, ComfyState::Queued, ComfyState::Waiting, ComfyState::Queued};
        for (int k = 0; k < 4; ++k) {
            ComfyActivity a;
            a.id = 900000 + k;
            a.label = labels[k];
            a.state = states[k];
            a.startedMs = now0 - 95000 + k * 1000;
            a.runningMs = k == 0 ? now0 - 42000 : 0;
            a.message = k == 0 ? "ComfyUI is generating it now..." : "In ComfyUI's queue";
            acts.push_back(a);
        }
    } else {
        if (!app.comfySvc || app.comfySvc->Active() <= 0) return;
        acts = app.comfySvc->Activity();
    }
    for (const ComfyActivity& a : acts) {
        if (a.state == ComfyState::Done || a.state == ComfyState::Failed) continue;
        if (a.label.rfind("Cut out", 0) == 0) continue; // a step inside Pixel art, not a picture of its own
        live.push_back(a);
    }
    if (live.empty()) return;
    // what is generating now first, then the newest
    std::reverse(live.begin(), live.end());
    std::stable_partition(live.begin(), live.end(), [](const ComfyActivity& a) { return a.state == ComfyState::Running; });
    g_makingShown = true;
    const float t = static_cast<float>(ImGui::GetTime());
    const long long now = ComfyService::NowMs();
    const float cardH = ui::Dp(48.0f), gap = ui::Dp(8.0f);
    const float avail = ImGui::GetContentRegionAvail().x;
    const float moreW = ui::Dp(84.0f);
    float cardW = ui::Dp(236.0f);
    int fit = std::max(1, static_cast<int>((avail + gap) / (cardW + gap)));
    if (fit == 1) cardW = std::min(cardW, avail); // a narrow gallery: one card across
    const int total = static_cast<int>(live.size());
    int shown = std::min(total, fit);
    // the "+N more" button takes a card's place when they do not all fit (a lone card says it itself)
    if (total > fit && fit > 1 && (avail - fit * (cardW + gap)) < moreW) --shown;
    const bool moreInCard = total > shown && shown == 1 && fit == 1;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // newest first: what was just sent is what you are waiting for
    for (int k = 0; k < shown; ++k) {
        const ComfyActivity& a = live[k];
        if (k > 0) ImGui::SameLine(0.0f, gap);
        ImGui::PushID(a.id);
        const ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + cardW, p0.y + cardH);
        ImGui::SetNextItemAllowOverlap();
        const bool open = ImGui::InvisibleButton("##card", ImVec2(cardW, cardH));
        const bool hov = ImGui::IsItemHovered();
        const bool running = a.state == ComfyState::Running;
        const float pulse = 0.5f + 0.5f * std::sin(t * 2.1f);
        const float rr = ui::Dp(10.0f);
        dl->AddRectFilled(p0, p1, ImGui::GetColorU32(hov ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), rr);
        if (running) { // a slow glow across the top, like HeliosGen's pending card
            const ImU32 g0 = ui::WithAlpha(ui::kAccent, 0.10f + 0.16f * pulse), g1 = ui::WithAlpha(ui::kAccent, 0.0f);
            dl->AddRectFilledMultiColor(ImVec2(p0.x + rr * 0.5f, p0.y + 1.0f), ImVec2(p1.x - rr * 0.5f, p0.y + cardH * 0.6f), g0, g0, g1, g1);
        }
        dl->AddRect(p0, p1, running ? ui::WithAlpha(ui::kAccent, 0.35f + 0.35f * pulse) : IM_COL32(255, 255, 255, 30), rr, 0, 1.0f);
        // spinner
        const ImVec2 sc(p0.x + ui::Dp(22.0f), p0.y + cardH * 0.5f);
        const float sr = ui::Dp(8.0f);
        dl->AddCircle(sc, sr, IM_COL32(255, 255, 255, 30), 24, ui::Dp(2.0f));
        const float start = t * (running ? 5.5f : 2.5f);
        dl->PathArcTo(sc, sr, start, start + 1.9f, 16);
        dl->PathStroke(running ? ImGui::GetColorU32(ui::kAccentHover) : IM_COL32(170, 175, 185, 220), 0, ui::Dp(2.0f));
        // what and where
        const float tx = p0.x + ui::Dp(40.0f), right = p1.x - ui::Dp(34.0f);
        ImGui::PushFont(ui::BoldFont());
        ImGui::RenderTextEllipsis(dl, ImVec2(tx, p0.y + ui::Dp(7.0f)), ImVec2(right, p0.y + ui::Dp(26.0f)), right, right, a.label.c_str(), nullptr, nullptr);
        ImGui::PopFont();
        char where[96];
        const long long since = std::max(0LL, now - (running && a.runningMs > 0 ? a.runningMs : a.startedMs)) / 1000;
        switch (a.state) {
            case ComfyState::Running: snprintf(where, sizeof(where), "Generating  %lld:%02lld", since / 60, since % 60); break;
            case ComfyState::Queued: snprintf(where, sizeof(where), "In ComfyUI's queue  %lld:%02lld", since / 60, since % 60); break;
            case ComfyState::Starting: snprintf(where, sizeof(where), "Starting ComfyUI..."); break;
            case ComfyState::Sending: snprintf(where, sizeof(where), "Sending..."); break;
            default: snprintf(where, sizeof(where), "Waiting to be sent"); break;
        }
        if (moreInCard) {
            const size_t len = strlen(where);
            snprintf(where + len, sizeof(where) - len, "   +%d more", total - shown);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, running ? ui::kAccentHover : ui::kTextDim);
        ImGui::RenderTextEllipsis(dl, ImVec2(tx, p0.y + ui::Dp(26.0f)), ImVec2(right, p1.y), right, right, where, nullptr, nullptr);
        ImGui::PopStyleColor();
        // cancel
        const ImVec2 xc(p1.x - ui::Dp(18.0f), p0.y + cardH * 0.5f);
        ImGui::SetCursorScreenPos(ImVec2(xc.x - ui::Dp(11.0f), xc.y - ui::Dp(11.0f)));
        const bool cancel = ImGui::InvisibleButton("##cancel", ImVec2(ui::Dp(22.0f), ui::Dp(22.0f)));
        const bool xHov = ImGui::IsItemHovered();
        if (xHov) {
            dl->AddCircleFilled(xc, ui::Dp(11.0f), ui::WithAlpha(ui::kBad, 0.22f), 20);
            ImGui::SetTooltip(running ? "Stop it (ComfyUI stops this one only)" : "Take it out of the queue");
        }
        if (hov || xHov) DrawNavIcon(dl, 13, xc, ui::Dp(10.0f), xHov ? ImGui::GetColorU32(ui::kBad) : IM_COL32(200, 204, 212, 220));
        if (cancel && app.comfySvc) app.comfySvc->Cancel(a.id);
        else if (open) app.requestTab = 4;
        if (hov && !xHov) ImGui::SetTooltip("%s\n%s%s", a.message.c_str(), a.workflow.empty() ? "" : ("Workflow: " + a.workflow + "\n").c_str(), "Click: the Generate page");
        // back to after the card, for the next SameLine
        ImGui::SetCursorScreenPos(p0);
        ImGui::Dummy(ImVec2(cardW, cardH));
        ImGui::PopID();
    }
    if (total > shown && !moreInCard) {
        ImGui::SameLine(0.0f, gap);
        char more[32];
        snprintf(more, sizeof(more), "+%d more", static_cast<int>(live.size()) - shown);
        if (ImGui::Button(more, ImVec2(0.0f, cardH))) app.requestTab = 4;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Everything that is being made is on the Generate page");
    }
    ImGui::Spacing();
}

// The make bar under the gallery (HeliosGen's prompt bar): write what you want and make it without leaving the gallery. Alone it runs the
// ComfyUI tab's workflow; with pictures (dragged in from the grid, or "Add picked") it is a Qwen Image 2.1 edit of them. Ctrl+Enter makes it.
struct MakeBar {
    char text[2048] = {};
    std::vector<std::wstring> refs;
    int times = 1;
    int canvas = 0;    // with pictures: 0 change the first one, 1 landscape, 2 square, 3 portrait (a new picture, they are only references)
    bool open = false; // being written in, or holding something: shows the options
    bool focus = false; // put the cursor in it (Ctrl+G, the palette)
    bool video = false; // a video with sound (MiniMax H3 Turbo, 4 steps): picture 1, when there is one, is its first frame
    int seconds = 5;
};
MakeBar g_make;

float MakeBarHeight() {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float line = ImGui::GetFrameHeight();
    const bool open = g_make.open || g_make.text[0] || !g_make.refs.empty();
    float h = st.WindowPadding.y * 2.0f + (open ? line * 2.2f : line) + st.ItemSpacing.y + ui::Dp(4.0f);
    if (open) h += line + st.ItemSpacing.y;
    if (!g_make.refs.empty()) h += ui::Dp(44.0f) + st.ItemSpacing.y;
    return h;
}

void AddMakeRefs(const std::vector<std::wstring>& paths) {
    for (const std::wstring& p : paths) {
        if (g_make.refs.size() >= 10) break; // Qwen Image Edit takes ten
        if (std::find(g_make.refs.begin(), g_make.refs.end(), p) == g_make.refs.end()) g_make.refs.push_back(p);
    }
}

void DrawMakeBar(AppState& app) {
    static bool testFilled = false; // MV_UI_TEST "makebar": two pictures and a prompt in it
    if (!testFilled && UiTestNumber("makebar") >= 0 && app.entries.size() >= 2 && !app.IsLoading()) {
        testFilled = true;
        AddMakeRefs({app.entries[0].path, app.entries[1].path});
        strcpy_s(g_make.text, "put <image2> next to <image1> in a neon-lit diner");
    }
    ImGui::Dummy(ImVec2(0.0f, ui::Dp(4.0f)));
    const float h = MakeBarHeight() - ui::Dp(4.0f) - ImGui::GetStyle().ItemSpacing.y;
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, ui::Dp(10.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::WithAlpha(ui::kAccent, 0.05f));
    ImGui::PushStyleColor(ImGuiCol_Border, ui::WithAlpha(ui::kAccent, g_make.open ? 0.55f : 0.22f));
    ImGui::BeginChild("##makebar", ImVec2(0.0f, h), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor(2);
    const float line = ImGui::GetFrameHeight();
    // the reference pictures
    if (!g_make.refs.empty()) {
        const float th = ui::Dp(44.0f);
        int remove = -1;
        for (size_t k = 0; k < g_make.refs.size(); ++k) {
            if (k > 0) ImGui::SameLine(0.0f, ui::Dp(6.0f));
            ImGui::PushID(static_cast<int>(k));
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##ref", ImVec2(th, th));
            const bool hov = ImGui::IsItemHovered();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, ImVec2(p.x + th, p.y + th), ui::ThumbPlaceholder(), ui::Dp(6.0f));
            const AppState::LibThumb lt = app.LibraryThumb(g_make.refs[k]);
            if (lt.srv && lt.w > 0 && lt.h > 0) { // cover the square
                ImVec2 uv0(0, 0), uv1(1, 1);
                const float ar = static_cast<float>(lt.w) / lt.h;
                if (ar > 1.0f) uv0.x = (1.0f - 1.0f / ar) * 0.5f, uv1.x = 1.0f - uv0.x;
                else uv0.y = (1.0f - ar) * 0.5f, uv1.y = 1.0f - uv0.y;
                dl->AddImageRounded((ImTextureID)(intptr_t)lt.srv, p, ImVec2(p.x + th, p.y + th), uv0, uv1, IM_COL32_WHITE, ui::Dp(6.0f));
            }
            char num[8];
            snprintf(num, sizeof(num), "%d", static_cast<int>(k) + 1);
            dl->AddRectFilled(ImVec2(p.x + ui::Dp(2.0f), p.y + ui::Dp(2.0f)), ImVec2(p.x + ui::Dp(15.0f), p.y + ui::Dp(16.0f)), IM_COL32(8, 10, 14, 200), ui::Dp(4.0f));
            dl->AddText(ImVec2(p.x + ui::Dp(5.0f), p.y + ui::Dp(2.0f)), IM_COL32_WHITE, num);
            if (hov) {
                dl->AddRectFilled(p, ImVec2(p.x + th, p.y + th), IM_COL32(0, 0, 0, 110), ui::Dp(6.0f));
                DrawNavIcon(dl, 13, ImVec2(p.x + th * 0.5f, p.y + th * 0.5f), th * 0.3f, IM_COL32_WHITE);
                ImGui::SetTooltip("%s\n<image%d> in what you write. Click: take it out", WideToUtf8(fs::path(g_make.refs[k]).filename().wstring()).c_str(), static_cast<int>(k) + 1);
                if (ImGui::IsItemClicked()) remove = static_cast<int>(k);
            }
            ImGui::PopID();
        }
        if (remove >= 0) g_make.refs.erase(g_make.refs.begin() + remove);
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) g_make.refs.clear();
    }
    // what to make, and the button
    const bool open = g_make.open || g_make.text[0] || !g_make.refs.empty();
    const float btnW = ui::Dp(84.0f);
    auto make = [&]() {
        std::string prompt = g_make.text;
        while (!prompt.empty() && (prompt.back() == ' ' || prompt.back() == '\n')) prompt.pop_back();
        if (prompt.empty()) {
            app.SetStatus("Write what to make first");
            return;
        }
        std::string label = prompt.substr(0, 48);
        for (char& ch : label) if (ch == '\n') ch = ' ';
        if (prompt.size() > 48) label += "...";
        bool sent = false;
        if (g_make.video) {
            SendH3Auto(app, g_make.refs.empty() ? std::wstring() : g_make.refs[0], prompt, g_make.seconds, label, g_make.times); // your text is the idea: a video prompt is written from it
            sent = false; // its own cards say what happens
        } else if (g_make.refs.empty()) {
            sent = SendToComfy(app, prompt, std::string(), std::wstring(), label, false, 0.0, g_make.times);
        } else {
            EditContext ctx;
            ctx.canvas = g_make.canvas;
            sent = SendPictureEdit(app, g_make.refs, prompt, label, g_make.times, &ctx);
        }
        if (sent) Toast(ToastKind::Info, g_make.video ? "Sent to MiniMax H3 Turbo" : g_make.refs.empty() ? "Sent to ComfyUI" : "Sent to Qwen Image 2.1 Edit", label, {}, [](AppState& s) { s.requestTab = 4; }, "makebar");
    };
    const float inputW = std::max(ui::Dp(60.0f), ImGui::GetContentRegionAvail().x - btnW - ImGui::GetStyle().ItemSpacing.x);
    const ImVec2 inputPos = ImGui::GetCursorScreenPos();
    const char* hint = g_make.video ? (g_make.refs.empty() ? "Describe the video and its sound   (Ctrl+Enter)" : "What happens next in picture 1, and the sound   (Ctrl+Enter)")
                       : g_make.refs.empty() ? "Describe a picture to make   (Ctrl+Enter)" : "What to do with them: \"put <image2> into <image1>'s room\"   (Ctrl+Enter)";
    // its background is drawn here and the box itself is see-through: a multi-line box is a child window drawn over this one, and the hint
    // (it has no hint of its own) has to show through it
    const ImVec2 inputSize(inputW, open ? line * 2.2f : line);
    if (g_make.focus) {
        ImGui::SetKeyboardFocusHere();
        g_make.focus = false;
    }
    ImGui::GetWindowDrawList()->AddRectFilled(inputPos, ImVec2(inputPos.x + inputW, inputPos.y + inputSize.y), ImGui::GetColorU32(ImGuiCol_FrameBg), ImGui::GetStyle().FrameRounding);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::InputTextMultiline("##maketext", g_make.text, sizeof(g_make.text), inputSize, ImGuiInputTextFlags_None);
    ImGui::PopStyleColor();
    const bool active = ImGui::IsItemActive();
    if (!g_make.text[0] && !active) { // the hint (multi-line inputs have none of their own)
        const ImVec2 fp = ImGui::GetStyle().FramePadding;
        const float right = inputPos.x + inputW - fp.x;
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), ImVec2(inputPos.x + fp.x, inputPos.y + fp.y), ImVec2(right, inputPos.y + line), right, right, hint, nullptr, nullptr);
        ImGui::PopStyleColor();
    }
    const bool ctrlEnter = active && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Enter, false);
    g_make.open = active || (g_make.open && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows));
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ui::kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::kAccentHover);
    const bool clickedMake = ImGui::Button("Make", ImVec2(btnW, open ? line * 2.2f : line));
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip(g_make.video ? "MiniMax H3 Turbo: a video with sound in 4 steps (Ctrl+Enter)%s" : g_make.refs.empty() ? "Runs %s with this prompt (Ctrl+Enter)" : "Edits these pictures with Qwen Image 2.1 (Ctrl+Enter)%s",
                          g_make.video ? "" : g_make.refs.empty() ? ComfyWorkflowLabel(app).c_str() : "");
    if (clickedMake || ctrlEnter) make();
    if (open) {
        const bool h3 = !H3TurboWorkflow(false).empty();
        if (h3) {
            for (int v = 0; v < 2; ++v) {
                if (v) ImGui::SameLine(0.0f, 0.0f);
                const bool on = g_make.video == (v == 1);
                ImGui::PushStyleColor(ImGuiCol_Button, on ? ui::ChipActiveColor() : ui::ChipColor());
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::ChipHoverColor());
                if (ImGui::SmallButton(v ? "Video##mkv" : "Picture##mkp")) g_make.video = v == 1;
                ImGui::PopStyleColor(2);
                if (v && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                    ImGui::SetTooltip("A video with sound from MiniMax H3 Turbo in 4 steps. With pictures: picture 1 is its first frame.");
            }
            ImGui::SameLine(0.0f, ui::Dp(10.0f));
        } else {
            g_make.video = false;
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("×"); // how many
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("How many to make");
        for (int n : {1, 2, 4}) {
            ImGui::SameLine(0.0f, ui::Dp(4.0f));
            char lbl[8];
            snprintf(lbl, sizeof(lbl), "%d##n%d", n, n);
            const bool on = g_make.times == n;
            ImGui::PushStyleColor(ImGuiCol_Button, on ? ui::ChipActiveColor() : ui::ChipColor());
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::ChipHoverColor());
            if (ImGui::SmallButton(lbl)) g_make.times = n;
            ImGui::PopStyleColor(2);
        }
        ImGui::SameLine(0.0f, ui::Dp(12.0f));
        // what is left of the row goes to the canvas choice / the workflow's name; "Add picked" keeps its place at the end
        const float addW = ImGui::CalcTextSize("Add picked").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float rest = ImGui::GetContentRegionAvail().x - addW - ImGui::GetStyle().ItemSpacing.x * 2.0f;
        if (g_make.video) {
            for (int sec : {5, 8, 10}) {
                char lbl[16];
                snprintf(lbl, sizeof(lbl), "%ds##sec%d", sec, sec);
                const bool on = g_make.seconds == sec;
                ImGui::PushStyleColor(ImGuiCol_Button, on ? ui::ChipActiveColor() : ui::ChipColor());
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::ChipHoverColor());
                if (ImGui::SmallButton(lbl)) g_make.seconds = sec;
                ImGui::PopStyleColor(2);
                ImGui::SameLine(0.0f, ui::Dp(4.0f));
            }
            ImGui::SameLine();
        } else if (!g_make.refs.empty()) {
            static const char* kCanvas[] = {"Change picture 1", "New picture: landscape", "New picture: square", "New picture: portrait"};
            ImGui::SetNextItemWidth(std::clamp(rest, ui::Dp(60.0f), ui::Dp(190.0f)));
            ImGui::Combo("##makecanvas", &g_make.canvas, kCanvas, 4);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Change picture 1 (the others are references), or make a new picture from all of them");
            ImGui::SameLine();
        } else if (rest > ui::Dp(40.0f)) {
            const std::string wf = "in " + ComfyWorkflowLabel(app);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), ImVec2(p.x, p.y + ImGui::GetStyle().FramePadding.y), ImVec2(p.x + rest, p.y + line), p.x + rest, p.x + rest, wf.c_str(), nullptr, nullptr);
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(rest, line));
            ImGui::SameLine();
        }
        if (ImGui::SmallButton("Add picked")) {
            std::vector<std::wstring> paths;
            for (int idx : app.multiSelected) {
                if (idx >= 0 && idx < static_cast<int>(app.entries.size()) && !app.entries[idx].metadata.isVideo) paths.push_back(app.entries[idx].path);
            }
            if (paths.empty() && app.selectedIndex >= 0 && app.selectedIndex < static_cast<int>(app.entries.size())) paths.push_back(app.entries[app.selectedIndex].path);
            AddMakeRefs(paths);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Use the picked pictures (or drag them here from the grid)");
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    // pictures dropped on it from the grid
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("MV_IMAGES")) {
            std::vector<std::wstring> paths;
            const int* ids = static_cast<const int*>(pl->Data);
            for (int k = 0; k < pl->DataSize / static_cast<int>(sizeof(int)); ++k) {
                if (ids[k] >= 0 && ids[k] < static_cast<int>(app.entries.size()) && !app.entries[ids[k]].metadata.isVideo) paths.push_back(app.entries[ids[k]].path);
            }
            AddMakeRefs(paths);
        }
        ImGui::EndDragDropTarget();
    }
}

void PickFolder(AppState& app);

// The folder you are in, and how to get to another: Up, a menu of its subfolders and your recent folders (and Open another folder...), and
// Refresh for pictures added since (also F5; new files normally come in by themselves). Switching folders used to mean finding "Open
// folder" or the hidden folder tree.
void DrawFolderBar(AppState& app) {
    const fs::path cur(app.currentFolder);
    const fs::path parent = cur.has_parent_path() && cur.parent_path() != cur ? cur.parent_path() : fs::path();
    ImGui::BeginDisabled(parent.empty() || app.currentFolder.empty());
    if (ImGui::Button("Up")) app.OpenFolder(parent.wstring(), cur.wstring());
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(parent.empty() ? "Already at the top" : "Up to %s", WideToUtf8(parent.wstring()).c_str());
    ImGui::SameLine();
    const float refreshW = ImGui::CalcTextSize("Refresh").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    std::string name = WideToUtf8(cur.filename().wstring());
    if (name.empty()) name = WideToUtf8(cur.wstring());
    ImGui::SetNextItemWidth(std::max(ui::Dp(80.0f), ImGui::GetContentRegionAvail().x - refreshW - ImGui::GetStyle().ItemSpacing.x));
    if (ImGui::BeginCombo("##folderbar", ("Folder: " + name).c_str(), ImGuiComboFlags_HeightLarge)) {
        static std::vector<fs::path> subs;
        if (ImGui::IsWindowAppearing()) { // its subfolders, read when the menu opens
            subs.clear();
            std::error_code ec;
            for (const auto& e : fs::directory_iterator(cur, fs::directory_options::skip_permission_denied, ec)) {
                if (e.is_directory(ec) && e.path().filename().wstring()[0] != L'.') subs.push_back(e.path());
            }
            std::sort(subs.begin(), subs.end());
        }
        ImGui::TextDisabled("%s", WideToUtf8(cur.wstring()).c_str());
        if (!subs.empty()) {
            ImGui::SeparatorText("Folders inside it");
            for (const fs::path& p : subs) {
                if (ImGui::Selectable(("  " + WideToUtf8(p.filename().wstring()) + "##sub" + WideToUtf8(p.wstring())).c_str())) app.OpenFolder(p.wstring());
            }
        }
        if (!parent.empty() && ImGui::Selectable(("Up: " + WideToUtf8(parent.wstring())).c_str())) app.OpenFolder(parent.wstring(), cur.wstring());
        if (!app.recentFolders.empty()) {
            ImGui::SeparatorText("Recent folders");
            int shown = 0;
            for (const std::string& r : app.recentFolders) {
                if (Utf8ToWide(r) == app.currentFolder) continue;
                if (++shown > 12) break;
                if (ImGui::Selectable((r + "##recent").c_str())) app.OpenFolder(Utf8ToWide(r));
            }
        }
        ImGui::Separator();
        if (ImGui::Selectable("Open another folder...")) PickFolder(app);
        if (ImGui::Selectable(app.showFolderPane ? "Hide the folder tree (Ctrl+B)" : "Show the folder tree (Ctrl+B)")) app.showFolderPane = !app.showFolderPane;
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s\nClick: its folders, your recent folders, or another one", WideToUtf8(cur.wstring()).c_str());
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) app.ReloadFolder();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Look for pictures added (or removed) since (F5)");
}

void DrawThumbnailGrid(AppState& app) {
    static char searchBuf[256] = {0};
    if (app.requestClearSearch) {
        searchBuf[0] = '\0';
        app.searchQuery.clear();
        app.requestClearSearch = false;
    }
    if (app.requestFocusSearch) {
        ImGui::SetKeyboardFocusHere();
        app.requestFocusSearch = false;
    }
    ImGuiStyle& style = ImGui::GetStyle();

    if (app.hasPendingSearch) {
        strncpy_s(searchBuf, app.pendingSearchText.c_str(), _TRUNCATE);
        app.searchQuery = searchBuf;
        app.hasPendingSearch = false;
    }
    DrawFolderBar(app);
    const float menuBtnW = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(-(menuBtnW + style.ItemSpacing.x));
    if (ImGui::InputTextWithHint("##search", "Search prompts, tags, file names   (Ctrl+F)", searchBuf,
                                  sizeof(searchBuf))) {
        app.searchQuery = searchBuf;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        app.PushSearchHistory(app.searchQuery);
        if (app.semanticOn && !app.searchQuery.empty()) {
            app.semanticLikePath.clear();
            EmbedQueryStart(app.searchQuery, app.comfy.url);
        }
    }
    const bool searchHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
    ImGui::SameLine();
    if (ImGui::ArrowButton("##searchmenu", ImGuiDir_Down)) ImGui::OpenPopup("search_popup");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Saved searches and recent searches");
    if (ImGui::BeginPopup("search_popup")) {
        static char nameBuf[64] = {0};
        ImGui::TextColored(ui::kTextDim, "Save the current search");
        ImGui::SetNextItemWidth(ui::Dp(190.0f));
        ImGui::InputTextWithHint("##savename", "Name (optional)", nameBuf, sizeof(nameBuf));
        ImGui::SameLine();
        ImGui::BeginDisabled(app.searchQuery.empty());
        if (ImGui::Button("Save")) {
            app.savedSearches.push_back({nameBuf[0] ? std::string(nameBuf) : app.searchQuery, app.searchQuery});
            nameBuf[0] = '\0';
            app.SaveUiSettings();
        }
        ImGui::EndDisabled();
        if (!app.savedSearches.empty()) {
            ImGui::Separator();
            ImGui::TextColored(ui::kTextDim, "Saved searches");
            int removeAt = -1;
            for (size_t i = 0; i < app.savedSearches.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(app.savedSearches[i].first.c_str(), false, ImGuiSelectableFlags_DontClosePopups, ImVec2(ui::Dp(260.0f), 0))) {
                    app.pendingSearchText = app.savedSearches[i].second;
                    app.hasPendingSearch = true;
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", app.savedSearches[i].second.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("Delete")) removeAt = static_cast<int>(i);
                ImGui::PopID();
            }
            if (removeAt >= 0) {
                app.savedSearches.erase(app.savedSearches.begin() + removeAt);
                app.SaveUiSettings();
            }
        }
        if (!app.searchHistory.empty()) {
            ImGui::Separator();
            ImGui::TextColored(ui::kTextDim, "Recent searches");
            for (const std::string& q : app.searchHistory) {
                if (ImGui::Selectable(q.c_str())) {
                    app.pendingSearchText = q;
                    app.hasPendingSearch = true;
                }
            }
            if (ImGui::SmallButton("Clear recent searches")) {
                app.searchHistory.clear();
                app.SaveUiSettings();
            }
        }
        ImGui::EndPopup();
    }
    // ---- by meaning: the embedding model reads every picture once; the search text (or a picture) is compared with them
    {
        // how many pictures have a vector: counted again only when the list or the index changed (a big folder takes a moment)
        static int indexed = 0, total = 0;
        static unsigned countedVer = ~0u;
        static size_t countedSize = ~size_t(0);
        static const void* countedData = nullptr;
        if (countedVer != EmbedIndexVersion() || countedSize != app.entries.size() || countedData != app.entries.data()) {
            countedVer = EmbedIndexVersion();
            countedSize = app.entries.size();
            countedData = app.entries.data();
            indexed = total = 0;
            for (const ImageEntry& e : app.entries) {
                if (e.metadata.isVideo || e.missing) continue;
                ++total;
                indexed += EmbedHas({e.path, static_cast<long long>(e.mtime.time_since_epoch().count()), e.size}) ? 1 : 0;
            }
        }
        ImGui::BeginDisabled(!EmbedAvailable());
        if (ImGui::Checkbox("By meaning##semantic", &app.semanticOn)) {
            ++app.semanticVersion;
            if (app.semanticOn) {
                if (indexed < total && !EmbedIndexRunning()) {
                    std::vector<EmbedItem> items;
                    for (const ImageEntry& e : app.entries) {
                        if (!e.metadata.isVideo && !e.missing) items.push_back({e.path, static_cast<long long>(e.mtime.time_since_epoch().count()), e.size});
                    }
                    EmbedIndexStart(items, app.comfy.url);
                }
                if (app.semanticVec.empty() && app.semanticLikePath.empty() && !app.searchQuery.empty()) EmbedQueryStart(app.searchQuery, app.comfy.url);
            }
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip(EmbedAvailable() ? "Search by what the pictures SHOW, not by the words in their prompts: type \"a knight in the rain\" and press Enter.\nThe embedding model (EmbeddingGemma-2) reads every picture once (about 20 a second) and remembers it.\nAlso: right click a picture > Find pictures like this."
                                               : "The embedding model is not in tools\\embed yet.");
        if (app.semanticOn) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ui::Dp(90.0f));
            ImGui::SliderFloat("##semmin", &app.semanticMin, 0.2f, 0.8f, "min %.2f");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("How similar a picture has to be to show up (cosine similarity: 0.5 is a loose match, 0.65 a good one)");
            ImGui::SameLine();
            if (ImGui::SmallButton("Search") && !app.searchQuery.empty()) {
                app.semanticLikePath.clear();
                EmbedQueryStart(app.searchQuery, app.comfy.url);
            }
            ImGui::SameLine();
            if (EmbedIndexRunning()) {
                int d = 0, t = 0;
                std::string phase;
                EmbedIndexProgress(d, t, phase);
                ImGui::TextColored(ui::kWarn, "indexing %d of %d", d, t);
                ImGui::SameLine();
                if (ImGui::SmallButton("Stop##embedstop")) EmbedIndexStop();
            } else if (EmbedQueryRunning()) {
                ImGui::TextColored(ui::kWarn, "reading the text...");
            } else {
                ImGui::TextDisabled("%d of %d indexed%s%s", indexed, total, app.semanticText.empty() ? "" : "  |  ", app.semanticText.c_str());
                if (indexed < total) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Index the rest")) {
                        std::vector<EmbedItem> items;
                        for (const ImageEntry& e : app.entries) {
                            if (!e.metadata.isVideo && !e.missing) items.push_back({e.path, static_cast<long long>(e.mtime.time_since_epoch().count()), e.size});
                        }
                        EmbedIndexStart(items, app.comfy.url);
                    }
                }
            }
        }
    }
    if (searchHovered) {
        ImGui::SetTooltip(
            "Plain text: substring match on prompt/tags/filename\n"
            "-text: exclude entries containing text\n"
            "key:value: substring match on a specific parameter (e.g. sampler:euler)\n"
            "key>value, key<value, key>=value, key<=value, key=value: numeric compare\n"
            "(e.g. steps>20, cfg<=7, seed=12345)\n"
            "nsfw>0.5, nsfw<0.3: filter by NSFW score (see Tools > NSFW Sort)\n"
            "year:2026, month:march, month>=6: when the file was made\n"
            "people:solo, people:2, people:3+, people:none: people in the image (from the prompt)\n"
            "generator:comfy, orient:portrait|landscape|square, model:name\n"
            "rating>=4, rating:none, folder:name (subfolder)\n"
            "vision:done, vision:none, vision:video: images described with Vision\n"
            "type:video, type:image: only video clips / only pictures\n"
            "quality>6, quality<4: picture-quality score (Tools > Score picture quality)\n"
            "style:anime, style:real, style:cartoon, style:3d, style:painting: kind of picture\n"
            "Combine multiple terms with commas; all must match.");
    }

    static const char* kSortLabels[] = {"Name",       "Date (year, month)", "File size", "Generation method",
                                         "NSFW score", "People in image",   "Model",     "Resolution", "Star rating", "Quality", "Style (photo, anime...)"};
    const float dirWidth = ImGui::CalcTextSize("Descending").x + style.FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - dirWidth - style.ItemSpacing.x);
    int sortIdx = static_cast<int>(app.sortMode);
    if (ImGui::Combo("##sort", &sortIdx, kSortLabels, IM_ARRAYSIZE(kSortLabels))) {
        app.sortMode = static_cast<AppState::SortMode>(sortIdx);
    }
    ImGui::SameLine();
    if (ImGui::Button(app.sortDescending ? "Descending" : "Ascending", ImVec2(dirWidth, 0))) {
        app.sortDescending = !app.sortDescending;
    }

    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##thumbsize", &app.thumbnailSize, 60.0f, 220.0f, "Thumbnail size  %.0f");
    {
        // The two "does it have X" questions people ask most, right at the top.
        static const char* kMeta[] = {"Metadata: any", "Metadata: yes", "Metadata: no"};
        static const char* kDescribed[] = {"Described: any", "Described: yes", "Described: no"};
        const float half = (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x) * 0.5f;
        ImGui::SetNextItemWidth(half);
        ImGui::Combo("##metamode", &app.metadataMode, kMeta, IM_ARRAYSIZE(kMeta));
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
            ImGui::SetTooltip("Whether the image carries generation metadata (prompt, settings, workflow).\n\"Without metadata\" finds images that were stripped or never had any.");
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        ImGui::Combo("##describedmode", &app.filters.vision, kDescribed, IM_ARRAYSIZE(kDescribed));
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
            ImGui::SetTooltip("Whether Vision (Ollama) already wrote something for the image.");
        }
        static const char* kKinds[] = {"Pics + videos", "Pictures only", "Videos only"};
        const float third = (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x) * 0.5f;
        ImGui::SetNextItemWidth(third);
        ImGui::Combo("##kindmode", &app.filters.kind, kKinds, IM_ARRAYSIZE(kKinds));
        ImGui::SameLine();
        {
            // style of the picture: photo / anime / cartoon ... (found by Tools > Score picture quality)
            const int st = app.filters.style;
            const std::string preview = st == 0 ? "Any style" : (st < 0 ? "Unclassified" : kStyleNames[st - 1]);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##stylemode", preview.c_str())) {
                if (ImGui::Selectable("Any style", st == 0)) app.filters.style = 0;
                for (int k = 0; k < kStyleCount; ++k) {
                    if (ImGui::Selectable(kStyleNames[k], st == k + 1)) app.filters.style = k + 1;
                }
                if (ImGui::Selectable("Not classified yet", st < 0)) app.filters.style = -1;
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Realistic / anime / cartoon / 3D ... found together with the quality score\n(Tools > Score picture quality). Scored pictures only.");
        }
        static const char* kPixel[] = {"Pixel art: shown", "Hide pixel art", "Only pixel art"};
        ImGui::SetNextItemWidth((ImGui::GetContentRegionAvail().x - style.ItemSpacing.x) * 0.5f);
        if (ImGui::Combo("##pixelmode", &app.filters.pixel, kPixel, IM_ARRAYSIZE(kPixel))) app.SaveUiSettings();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("The pixel art the Pixel art tab made (its \"_px\" files), and what it made on the way there (the PixelArt folder: drawings, masks, Retro Diffusion's pictures).\n"
                              "Hide it to browse your pictures, or show only the pixel art. Remembered.");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
            ImGui::SetTooltip("MP4, WebM, MKV, MOV and AVI clips show a poster frame; ComfyUI videos keep their workflow.\nVideos without a workflow count as \"without metadata\".");
        }
    }
    DrawQuickFilters(app);

    // The filtered+sorted list is only rebuilt when something it depends on
    // changed; at 100k+ images doing it every frame would stall the UI.
    struct ViewKey {
        AppState::GalleryFilters filters;
        unsigned restrict = 0;
        unsigned epoch = ~0u;
        std::string query;
        int onlyMeta = 0;
        bool unreadable = false;
        int sortMode = -1;
        bool desc = false;
        int source = -1;
        int semantic = -1;      // on / off
        unsigned semanticVer = 0, embedVer = 0;
        int semanticMin = 0;
        bool operator==(const ViewKey& o) const {
            return filters == o.filters && restrict == o.restrict && epoch == o.epoch && query == o.query && onlyMeta == o.onlyMeta && unreadable == o.unreadable && sortMode == o.sortMode &&
                   desc == o.desc && source == o.source && semantic == o.semantic && semanticVer == o.semanticVer && embedVer == o.embedVer && semanticMin == o.semanticMin;
        }
    };
    static ViewKey lastKey;
    static unsigned viewVersion = 0;
    ViewKey key{app.filters, app.restrictVersion, app.filterEpoch, app.searchQuery, app.metadataMode, app.showUnreadable, static_cast<int>(app.sortMode),
                app.sortDescending, static_cast<int>(app.nsfwSource), app.semanticOn ? 1 : 0, app.semanticVersion, app.semanticOn ? EmbedIndexVersion() : 0u,
                static_cast<int>(app.semanticMin * 100.0f + 0.5f)};
    const bool anyFilterActive = app.metadataMode != 0 || !app.searchQuery.empty() || app.filters.ActiveCount() > 0 || app.restrictActive;
    if (!(key == lastKey)) {
        lastKey = key;
        std::vector<SearchTerm> terms = SplitSearchTerms(app.semanticOn ? std::string() : app.searchQuery); // in "by meaning" mode the text is the meaning, not words to match
        std::vector<int> visible = FilterEntries(app, terms);
        SortVisible(app, visible);
        app.visibleOrder = std::move(visible);
        ++viewVersion;
    }
    const std::vector<int>& visible = app.visibleOrder;

    // One line, always the same height: how many pictures, or what is selected and what to do with it. (The selection used to add two rows
    // of buttons ABOVE the grid as soon as a second picture was Ctrl+clicked: the whole grid jumped down, and the next Ctrl+click landed on
    // another picture or on the group's "Select all" - found with real ImGui clicks: picture 0, Ctrl 1..4 ended up with 0 1 2.)
    if (app.multiSelected.size() > 1) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ui::kAccentHover, "%d selected", static_cast<int>(app.multiSelected.size()));
        ImGui::SameLine();
        if (ImGui::SmallButton("Deselect")) {
            app.multiSelected.clear();
            if (app.selectedIndex >= 0) app.multiSelected.insert(app.selectedIndex);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Esc or a click on empty space does the same");
        if (app.multiSelected.size() == 2) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Compare")) app.OpenCompare(*app.multiSelected.begin(), *app.multiSelected.rbegin());
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("With them...")) ImGui::OpenPopup("##selactions");
        if (ImGui::BeginPopup("##selactions")) {
            if (ImGui::Selectable("Copy their tags")) {
                std::vector<std::string> unionTags;
                for (int idx : app.multiSelected) {
                    if (idx < 0 || idx >= static_cast<int>(app.entries.size())) continue;
                    for (auto& t : app.entries[idx].metadata.parsed.tags) {
                        if (std::find(unionTags.begin(), unionTags.end(), t) == unionTags.end()) unionTags.push_back(t);
                    }
                }
                ImGui::SetClipboardText(JoinTags(unionTags, ", ").c_str());
                app.SetStatus("Copied " + std::to_string(unionTags.size()) + " unique tags.");
            }
            if (ImGui::Selectable("Copy them without metadata")) CopyPictures(app, std::vector<int>(app.multiSelected.begin(), app.multiSelected.end()), true);
            if (ImGui::Selectable("Describe them with Vision")) {
                app.showVisionWindow = true;
                if (!app.visionModel.empty()) app.RequestVisionBatch(std::vector<int>(app.multiSelected.begin(), app.multiSelected.end()));
            }
            if (ImGui::Selectable("Export...")) {
                app.showExportSelectedDialog = true;
                app.exportSelectedResultMessage.clear();
            }
            ImGui::Separator();
            if (ImGui::Selectable("Delete...")) {
                app.pendingDeletePaths.clear();
                for (int idx : app.multiSelected) {
                    if (idx >= 0 && idx < static_cast<int>(app.entries.size())) app.pendingDeletePaths.push_back(app.entries[idx].path);
                }
                app.showDeleteConfirm = true;
            }
            ImGui::EndPopup();
        }
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, ui::kTextDim);
        if (!anyFilterActive && app.hiddenUnreadable == 0) {
            ImGui::Text("%d image%s", app.LiveCount(), app.LiveCount() == 1 ? "" : "s");
        } else {
            ImGui::Text("%d shown of %d", static_cast<int>(visible.size()), app.LiveCount());
        }
        ImGui::PopStyleColor();
        if (app.hiddenUnreadable > 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%d unreadable hidden)", app.hiddenUnreadable);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Files that could not be read as pictures (damaged files, macOS \"._\" stubs).\nShow them again in the View menu.");
        }
        if (app.IsLoading()) {
            ImGui::SameLine();
            ImGui::TextColored(ui::kWarn, "scanning %d/%d%s", app.LoadedCount(), app.TotalCount(), anyFilterActive ? " (more may appear)" : "");
        }
    }
    if (app.restrictActive) {
        ImGui::TextColored(ui::kAccentHover, "Showing %s only", app.restrictLabel.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Show all")) app.ClearRestrict();
    }
    ImGui::Spacing();
    DrawMakingStrip(app);

    ImGui::BeginChild("thumb_grid_scroll", ImVec2(ui::Dp(0.0f), -MakeBarHeight()), false);
    if (visible.empty()) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        if (app.entries.empty()) {
            ImGui::TextDisabled(app.IsListing() ? "Scanning the folder..." : (app.IsLoading() ? "Loading..." : "No supported images in this folder (PNG, JPG, WebP, BMP, GIF)."));
        } else if (app.IsLoading()) {
            ImGui::TextDisabled("Scanning images...");
        } else {
            ImGui::TextDisabled("Nothing matches. Clear the search (Esc)%s.",
                                app.metadataMode != 0 ? " or set the first list above to \"All images\"" : "");
        }
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        DrawMakeBar(app);
        return;
    }

    const float cell = ui::Dp(app.thumbnailSize);
    const float gap = ui::Dp(8.0f);
    const float labelH = ImGui::GetTextLineHeight() + ui::Dp(6.0f);
    const float headerH = ImGui::GetTextLineHeight() + ui::Dp(16.0f);
    const float panelWidth = ImGui::GetContentRegionAvail().x;
    const int columns = std::max(1, static_cast<int>((panelWidth + gap) / (cell + gap)));
    app.galleryColumns = columns;
    const int count = static_cast<int>(visible.size());
    const bool grouped = app.showGroupHeaders && !(app.semanticOn && !app.semanticVec.empty()); // by meaning the order is the similarity: no headings
    // Centre the grid horizontally so leftover width is split evenly.
    const float used = columns * cell + (columns - 1) * gap;
    const ImVec2 base = ImGui::GetCursorPos();
    const float offsetX = base.x + std::max(0.0f, (panelWidth - used) * 0.5f);

    // The row layout is rebuilt only when the view, size or grouping changes.
    static GridLayout layout;
    struct LayoutKey {
        unsigned view = ~0u;
        int columns = 0;
        float cell = 0, labelH = 0, headerH = 0;
        bool grouped = false;
        int sort = -1;
        float nsfw[3] = {};
        bool operator==(const LayoutKey& o) const {
            return view == o.view && columns == o.columns && cell == o.cell && labelH == o.labelH && headerH == o.headerH &&
                   grouped == o.grouped && sort == o.sort && nsfw[0] == o.nsfw[0] && nsfw[1] == o.nsfw[1] && nsfw[2] == o.nsfw[2];
        }
    };
    static LayoutKey layoutKey;
    LayoutKey wanted{viewVersion, columns, cell, labelH, headerH, grouped, static_cast<int>(app.sortMode),
                     {app.nsfwThreshold, app.nsfwQuestionableLow, app.nsfwUseQuestionable ? 1.0f : 0.0f}};
    if (!(wanted == layoutKey)) {
        layoutKey = wanted;
        BuildGridLayout(layout, app, visible, columns, cell, labelH, gap, headerH, grouped);
    }
    const std::vector<GridRow>& rows = layout.rows;

    if (app.scrollToSelected) {
        app.scrollToSelected = false;
        auto it = std::find(visible.begin(), visible.end(), app.selectedIndex);
        if (it != visible.end()) {
            const GridRow& row = rows[layout.rowOfPos[it - visible.begin()]];
            float top = base.y + row.y;
            float bottom = top + row.h;
            float view = ImGui::GetWindowHeight();
            float scrollY = ImGui::GetScrollY();
            if (top < scrollY) ImGui::SetScrollY(std::max(0.0f, top - gap - (grouped ? headerH : 0.0f)));
            else if (bottom > scrollY + view) ImGui::SetScrollY(bottom - view + gap);
        }
    }

    auto handleClick = [&](int i) {
        ImGuiIO& io = ImGui::GetIO();
        if (io.KeyShift && app.lastClickedIndex >= 0) {
            auto itLast = std::find(visible.begin(), visible.end(), app.lastClickedIndex);
            auto itCur = std::find(visible.begin(), visible.end(), i);
            if (itLast != visible.end() && itCur != visible.end()) {
                size_t a2 = std::distance(visible.begin(), itLast);
                size_t b2 = std::distance(visible.begin(), itCur);
                if (a2 > b2) std::swap(a2, b2);
                if (!io.KeyCtrl) app.multiSelected.clear();
                for (size_t k = a2; k <= b2; ++k) app.multiSelected.insert(visible[k]);
                SelLog(app, "shift-range", i);
            }
            app.SelectIndex(i, /*resetMultiSelect=*/false);
        } else if (io.KeyCtrl) {
            if (app.multiSelected.count(i)) app.multiSelected.erase(i);
            else app.multiSelected.insert(i);
            SelLog(app, "ctrlclick", i);
            app.lastClickedIndex = i;
            app.SelectIndex(i, /*resetMultiSelect=*/false);
        } else {
            app.SelectIndex(i); // default resetMultiSelect=true: plain click selects just this one
        }
        app.scrollToSelected = false; // already under the cursor
    };

    // Only visit rows that are on screen: with thousands of images in a folder,
    // building ImGui widgets for every one every frame would make the window
    // look hung. Rows have known positions, so a binary search finds the first.
    const float topLimit = ImGui::GetScrollY() - base.y;
    const float bottomLimit = topLimit + ImGui::GetWindowHeight();
    size_t lo = 0, hi = rows.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (rows[mid].y + rows[mid].h < topLimit) lo = mid + 1;
        else hi = mid;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (size_t r = lo; r < rows.size() && rows[r].y <= bottomLimit; ++r) {
        const GridRow& row = rows[r];
        if (row.header) {
            ImGui::SetCursorPos(ImVec2(base.x, base.y + row.y));
            ImGui::PushID(-1 - static_cast<int>(r));
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float selW = ui::Dp(96.0f);
            ImGui::InvisibleButton("##group", ImVec2(panelWidth - selW, row.h - ui::Dp(2.0f)));
            const bool hov = ImGui::IsItemHovered();
            // "Select all" is its own small button at the right: a stray click on the heading selects nothing
            ImGui::SetCursorScreenPos(ImVec2(p.x + panelWidth - selW, p.y));
            const bool selectAll = ImGui::InvisibleButton("##groupall", ImVec2(selW, row.h - ui::Dp(2.0f)));
            const bool allHov = ImGui::IsItemHovered();
            ImFont* bold = ui::BoldFont();
            const ImVec2 tp(p.x + ui::Dp(4.0f), p.y + row.h - ImGui::GetTextLineHeight() - ui::Dp(7.0f));
            dl->AddText(bold, bold->FontSize, tp, ImGui::GetColorU32(ImGuiCol_Text), row.label.c_str());
            char countText[32];
            snprintf(countText, sizeof(countText), "%d", row.count);
            const float labelW = ImGui::CalcTextSize(row.label.c_str()).x;
            dl->AddText(ImVec2(tp.x + labelW + ui::Dp(10.0f), tp.y), ImGui::GetColorU32(ui::kTextDim), countText);
            dl->AddLine(ImVec2(p.x, p.y + row.h - ui::Dp(4.0f)), ImVec2(p.x + panelWidth, p.y + row.h - ui::Dp(4.0f)),
                        ImGui::GetColorU32(ImGuiCol_Border));
            if (hov || allHov) {
                const char* txt = "Select all";
                const ImVec2 ts = ImGui::CalcTextSize(txt);
                dl->AddText(ImVec2(p.x + panelWidth - ts.x - ui::Dp(6.0f), tp.y), allHov ? ImGui::GetColorU32(ui::kAccentHover) : ImGui::GetColorU32(ui::kTextDim), txt);
            }
            if (allHov) ImGui::SetTooltip("Select the %d images in this group", row.count);
            if (selectAll) {
                app.multiSelected.clear();
                for (int k = 0; k < row.count; ++k) app.multiSelected.insert(visible[row.first + k]);
                SelLog(app, "group-select-all", row.count);
                app.lastClickedIndex = visible[row.first];
                app.SelectIndex(visible[row.first], /*resetMultiSelect=*/false);
                app.scrollToSelected = false;
            }
            ImGui::PopID();
            continue;
        }
        for (int col = 0; col < row.count; ++col) {
            const int i = visible[row.first + col];
            ImGui::SetCursorPos(ImVec2(offsetX + col * (cell + gap), base.y + row.y));
            ImGui::PushID(i);
            bool cellClicked = DrawThumbnailCell(app, i, cell, labelH);
            ImGui::PopID();
            if (cellClicked) handleClick(i);
        }
    }
    ImGui::SetCursorPos(ImVec2(base.x, base.y + layout.totalHeight));
    ImGui::Dummy(ImVec2(1, 1));
    // ---- rubber-band selection: drag a box over empty space to select everything it touches (like Explorer);
    // Ctrl or Shift keeps the current selection. Dragging a selected thumbnail drags the files out instead.
    {
        struct Marquee {
            bool active = false, moved = false, additive = false;
            ImVec2 start{0, 0};
            std::set<int> base;
        };
        static Marquee mq;
        ImGuiIO& io = ImGui::GetIO();
        const ImVec2 winPos = ImGui::GetWindowPos();
        const ImVec2 scroll(ImGui::GetScrollX(), ImGui::GetScrollY());
        auto toLocal = [&](ImVec2 s) { return ImVec2(s.x - winPos.x + scroll.x, s.y - winPos.y + scroll.y); };
        auto toScreen = [&](ImVec2 l) { return ImVec2(l.x + winPos.x - scroll.x, l.y + winPos.y - scroll.y); };
        // Which thumbnail is under a point (grid coordinates): from the layout, not from ImGui's hover - a thumbnail can miss its hover in the
        // frame the pointer arrives with the click, and that click used to start a box here instead; with Ctrl held, the box then added or
        // took away the pictures around it and the Ctrl+clicks undid each other (found with real ImGui clicks: 0, Ctrl 1..4 kept 0 1 2)
        auto cellAt = [&](ImVec2 l) {
            size_t rlo = 0, rhi = rows.size();
            while (rlo < rhi) {
                const size_t mid = (rlo + rhi) / 2;
                if (base.y + rows[mid].y + rows[mid].h < l.y) rlo = mid + 1;
                else rhi = mid;
            }
            for (size_t r = rlo; r < rows.size() && base.y + rows[r].y <= l.y; ++r) {
                const GridRow& row = rows[r];
                if (row.header) continue;
                const float cy0 = base.y + row.y;
                if (l.y < cy0 || l.y > cy0 + cell + labelH) continue;
                for (int col = 0; col < row.count; ++col) {
                    const float cx0 = offsetX + col * (cell + gap);
                    if (l.x >= cx0 && l.x <= cx0 + cell) return visible[row.first + col];
                }
            }
            return -1;
        };
        if (!mq.active && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered()) {
            const int hit = cellAt(toLocal(io.MousePos));
            if (hit >= 0) { // a click on a thumbnail that missed its hover: it is that thumbnail's click
                handleClick(hit);
                SelLog(app, "missed-hover", hit);
            }
        }
        if (!mq.active && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered() && cellAt(toLocal(io.MousePos)) < 0) {
            mq.active = true;
            mq.moved = false;
            mq.start = toLocal(io.MousePos);
            SelLog(app, "marquee-start", static_cast<int>(io.MousePos.x * 10000 + io.MousePos.y));
            mq.additive = io.KeyCtrl || io.KeyShift;
            mq.base = mq.additive ? app.multiSelected : std::set<int>();
        }
        if (mq.active) {
            const ImVec2 cur = toLocal(io.MousePos);
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                if (!mq.moved && !mq.additive) {
                    // a plain click on empty space drops the multi-selection back to the one image
                    app.multiSelected.clear();
                    if (app.selectedIndex >= 0) app.multiSelected.insert(app.selectedIndex);
                    SelLog(app, "marquee-reset", app.selectedIndex);
                } else if (mq.moved && !app.multiSelected.empty() && app.multiSelected.count(app.selectedIndex) == 0) {
                    // the picture shown follows the box: the first selected one in gallery order
                    int first = -1;
                    for (int idx : visible) {
                        if (app.multiSelected.count(idx)) { first = idx; break; }
                    }
                    if (first >= 0) {
                        app.lastClickedIndex = first;
                        app.SelectIndex(first, /*resetMultiSelect=*/false);
                        app.scrollToSelected = false;
                    }
                }
                mq.active = false;
            } else {
                if (!mq.moved && std::fabs(cur.x - mq.start.x) + std::fabs(cur.y - mq.start.y) > ui::Dp(5.0f)) mq.moved = true, SelLog(app, "marquee-moved", 0);
                if (mq.moved) {
                    // scroll when the pointer is held near the top or bottom edge
                    const float edge = ui::Dp(30.0f);
                    const float winTop = winPos.y, winBottom = winPos.y + ImGui::GetWindowHeight();
                    if (io.MousePos.y < winTop + edge) ImGui::SetScrollY(std::max(0.0f, ImGui::GetScrollY() - ui::Dp(16.0f) * (1.0f - std::max(0.0f, io.MousePos.y - winTop) / edge)));
                    else if (io.MousePos.y > winBottom - edge) ImGui::SetScrollY(ImGui::GetScrollY() + ui::Dp(16.0f) * (1.0f - std::max(0.0f, winBottom - io.MousePos.y) / edge));

                    const float x0 = std::min(mq.start.x, cur.x), x1 = std::max(mq.start.x, cur.x);
                    const float y0 = std::min(mq.start.y, cur.y), y1 = std::max(mq.start.y, cur.y);
                    std::set<int> picked = mq.base;
                    size_t rlo = 0, rhi = rows.size();
                    while (rlo < rhi) {
                        size_t mid = (rlo + rhi) / 2;
                        if (base.y + rows[mid].y + rows[mid].h < y0) rlo = mid + 1;
                        else rhi = mid;
                    }
                    for (size_t r = rlo; r < rows.size() && base.y + rows[r].y <= y1; ++r) {
                        const GridRow& row = rows[r];
                        if (row.header) continue;
                        const float cy0 = base.y + row.y, cy1 = cy0 + cell + labelH;
                        if (cy1 < y0 || cy0 > y1) continue;
                        for (int col = 0; col < row.count; ++col) {
                            const float cx0 = offsetX + col * (cell + gap), cx1 = cx0 + cell;
                            if (cx1 >= x0 && cx0 <= x1) picked.insert(visible[row.first + col]);
                        }
                    }
                    if (picked != app.multiSelected) app.multiSelected = std::move(picked);
                    ImDrawList* mdl = ImGui::GetWindowDrawList();
                    const ImVec2 a = toScreen(ImVec2(x0, y0)), b = toScreen(ImVec2(x1, y1));
                    mdl->AddRectFilled(a, b, ui::WithAlpha(ui::kAccent, 0.22f), ui::Dp(2.0f));
                    mdl->AddRect(a, b, ui::WithAlpha(ui::kAccent, 0.95f), ui::Dp(2.0f), 0, ui::Dp(1.0f));
                }
            }
        }
    }
    (void)count;
    ImGui::EndChild();
    DrawMakeBar(app);
}

// Playback bar over the bottom of a playing video: play/pause, position, sound, loop.
// Returns the bar's rectangle so clicks on it don't also toggle playback.
ImVec4 DrawVideoControls(AppState& app, const ImVec2& origin, const ImVec2& avail, bool show) {
    VideoPlayer& vp = *app.videoPlayer;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float barH = ui::Dp(46.0f);
    const ImVec2 b0(origin.x, origin.y + avail.y - barH), b1(origin.x + avail.x, origin.y + avail.y);
    if (!show) return ImVec4(b0.x, b0.y, b1.x, b1.y);
    dl->AddRectFilled(b0, b1, IM_COL32(8, 10, 14, 205));

    const float m = ui::Dp(10.0f), h = ui::Dp(28.0f);
    float x = b0.x + m;
    const float y = b0.y + (barH - h) * 0.5f;

    // play / pause
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    if (ImGui::InvisibleButton("##vp_play", ImVec2(h, h))) vp.Toggle();
    {
        const bool hot = ImGui::IsItemHovered();
        const ImU32 col = hot ? IM_COL32(120, 175, 255, 255) : IM_COL32(235, 238, 245, 255);
        const ImVec2 c(x + h * 0.5f, y + h * 0.5f);
        if (vp.Playing()) {
            dl->AddRectFilled(ImVec2(c.x - h * 0.22f, c.y - h * 0.26f), ImVec2(c.x - h * 0.06f, c.y + h * 0.26f), col, 1.5f);
            dl->AddRectFilled(ImVec2(c.x + h * 0.06f, c.y - h * 0.26f), ImVec2(c.x + h * 0.22f, c.y + h * 0.26f), col, 1.5f);
        } else {
            dl->AddTriangleFilled(ImVec2(c.x - h * 0.17f, c.y - h * 0.28f), ImVec2(c.x - h * 0.17f, c.y + h * 0.28f), ImVec2(c.x + h * 0.27f, c.y), col);
        }
    }
    x += h + m;

    // time
    const double pos = vp.Position(), dur = vp.Duration();
    char tbuf[48];
    snprintf(tbuf, sizeof(tbuf), "%s / %s", FormatDuration(pos).c_str(), dur > 0 ? FormatDuration(dur).c_str() : "--:--");
    dl->AddText(ImVec2(x, b0.y + (barH - ImGui::GetTextLineHeight()) * 0.5f), IM_COL32(220, 224, 232, 255), tbuf);
    x += ImGui::CalcTextSize("00:00 / 00:00").x + m * 1.5f;

    // right-hand group (laid out from the right edge)
    const float volW = ui::Dp(80.0f);
    const float btnW = ui::Dp(58.0f);
    float rx = b1.x - m;
    rx -= ui::Dp(88.0f);
    const float extX = rx;
    rx -= m + btnW;
    const float loopX = rx;
    float volX = 0, muteX = 0;
    if (vp.HasSound()) {
        rx -= m + volW;
        volX = rx;
        rx -= m + btnW;
        muteX = rx;
    }

    // seek bar
    const float seekW = std::max(ui::Dp(60.0f), rx - m - x);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::SetNextItemWidth(seekW);
    float p = static_cast<float>(pos);
    static bool seeking = false;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ui::Dp(4.0f), (h - ImGui::GetFontSize()) * 0.5f));
    ImGui::SliderFloat("##vp_seek", &p, 0.0f, static_cast<float>(std::max(dur, 0.1)), "");
    ImGui::PopStyleVar();
    if (ImGui::IsItemActive()) {
        if (!seeking || std::fabs(p - static_cast<float>(pos)) > 0.04f) vp.Seek(p);
        seeking = true;
    } else {
        seeking = false;
    }

    auto smallButton = [&](const char* label, float bx, float w, bool on) {
        ImGui::SetCursorScreenPos(ImVec2(bx, y));
        if (on) {
            ImGui::PushStyleColor(ImGuiCol_Button, ui::kAccentActive);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        }
        const bool clicked = ImGui::Button(label, ImVec2(w, h));
        if (on) ImGui::PopStyleColor(2);
        return clicked;
    };
    if (vp.HasSound()) {
        if (smallButton(app.videoMuted ? "Muted" : "Sound", muteX, btnW, app.videoMuted)) {
            app.videoMuted = !app.videoMuted;
            vp.SetMuted(app.videoMuted);
            app.SaveUiSettings();
        }
        ImGui::SetCursorScreenPos(ImVec2(volX, y));
        ImGui::SetNextItemWidth(volW);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ui::Dp(4.0f), (h - ImGui::GetFontSize()) * 0.5f));
        float vol = app.videoVolume;
        if (ImGui::SliderFloat("##vp_vol", &vol, 0.0f, 1.0f, "")) {
            app.videoVolume = vol;
            VideoPlayer::SetVideoVolume(vol); // (every video in the app)
            if (app.videoMuted && vol > 0.0f) {
                app.videoMuted = false;
                vp.SetMuted(false);
            }
        }
        ImGui::PopStyleVar();
        if (ImGui::IsItemDeactivatedAfterEdit()) app.SaveUiSettings();
    }
    if (smallButton("Loop", loopX, btnW, app.videoLoop)) {
        app.videoLoop = !app.videoLoop;
        vp.SetLoop(app.videoLoop);
        app.SaveUiSettings();
    }
    if (smallButton("External", extX, ui::Dp(88.0f), false)) {
        ShellExecuteW(nullptr, L"open", vp.Path().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Open in your default video player");
    return ImVec4(b0.x, b0.y, b1.x, b1.y);
}

void DrawImageViewer(AppState& app) {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 1 || avail.y < 1) return;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (!app.currentTexture.srv) {
        const char* msg = app.entries.empty() ? "No image" : "Select an image";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(origin.x + (avail.x - ts.x) * 0.5f, origin.y + (avail.y - ts.y) * 0.5f),
                    ImGui::GetColorU32(ui::kTextDim), msg);
        return;
    }

    // A playing video replaces the still (its poster frame) until it is stopped.
    VideoPlayer* vp = nullptr;
    if (app.VideoOpen() && app.videoPlayer->Srv() && app.selectedIndex >= 0 && app.selectedIndex < static_cast<int>(app.entries.size()) &&
        _wcsicmp(app.videoPlayer->Path().c_str(), app.entries[app.selectedIndex].path.c_str()) == 0) {
        vp = app.videoPlayer.get();
    }
    ID3D11ShaderResourceView* viewSrv = vp ? vp->Srv() : app.currentTexture.srv;
    const float imgW = static_cast<float>(vp ? vp->Width() : app.currentTexture.width);
    const float imgH = static_cast<float>(vp ? vp->Height() : app.currentTexture.height);
    if (vp) {
        app.fitToWindow = true; // videos always fit the pane
    }
    float fit = std::min(avail.x / imgW, avail.y / imgH);
    if (!(fit > 0.0f) || !std::isfinite(fit)) fit = 1.0f;

    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##viewer", avail, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 center(origin.x + avail.x * 0.5f, origin.y + avail.y * 0.5f);

    if (app.fitToWindow) {
        app.zoom = fit;
        app.panX = app.panY = 0.0f;
    }
    if (hovered && io.MouseWheel != 0.0f) {
        // Zoom around the cursor so the point under it stays put.
        float old = app.zoom;
        float next = std::clamp(old * std::pow(1.18f, io.MouseWheel), std::min(fit, 0.05f), 40.0f);
        float k = next / old;
        float relX = io.MousePos.x - center.x - app.panX;
        float relY = io.MousePos.y - center.y - app.panY;
        app.panX -= relX * (k - 1.0f);
        app.panY -= relY * (k - 1.0f);
        app.zoom = next;
        app.fitToWindow = next <= fit * 1.001f;
    }
    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
        app.panX += io.MouseDelta.x;
        app.panY += io.MouseDelta.y;
    }
    if (hovered && !app.currentMetadata.isVideo && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        if (app.fitToWindow) {
            // Jump to 100%, centred on the point that was double-clicked.
            float relX = io.MousePos.x - center.x, relY = io.MousePos.y - center.y;
            app.panX = -relX * (1.0f / fit - 1.0f);
            app.panY = -relY * (1.0f / fit - 1.0f);
            app.zoom = 1.0f;
            app.fitToWindow = false;
        } else {
            app.fitToWindow = true;
        }
    }

    ImVec2 size(imgW * app.zoom, imgH * app.zoom);
    const float maxPanX = std::max(0.0f, (size.x - avail.x) * 0.5f);
    const float maxPanY = std::max(0.0f, (size.y - avail.y) * 0.5f);
    app.panX = std::clamp(app.panX, -maxPanX, maxPanX);
    app.panY = std::clamp(app.panY, -maxPanY, maxPanY);
    if (hovered && (maxPanX > 0.0f || maxPanY > 0.0f)) {
        ImGui::SetMouseCursor(active ? ImGuiMouseCursor_ResizeAll : ImGuiMouseCursor_Hand);
    }

    const ImVec2 p0(center.x - size.x * 0.5f + app.panX, center.y - size.y * 0.5f + app.panY);
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    dl->PushClipRect(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), true);
    const bool blurView = app.selectedIndex >= 0 && app.selectedIndex < static_cast<int>(app.entries.size()) && app.ShouldBlur(app.entries[app.selectedIndex]);
    if (blurView) viewSrv = app.ShownSrv(app.currentTexture.srv, true); // (a video too: its still, blurred)
    if (viewSrv) dl->AddImage((ImTextureID)(intptr_t)viewSrv, p0, p1);
    dl->PopClipRect();
    if (app.selectedIndex >= 0 && app.selectedIndex < static_cast<int>(app.entries.size())) DrawNsfwOverlay(app, app.entries[app.selectedIndex].path, ImVec2(std::max(p0.x, origin.x), std::max(p0.y, origin.y)),
                                ImVec2(std::min(p1.x, origin.x + avail.x), std::min(p1.y, origin.y + avail.y)));

    // previous / next arrows at the sides of the picture (they show while the pointer is over it)
    const float arrowR = ui::Dp(24.0f);
    const ImVec2 prevC(origin.x + ui::Dp(16.0f) + arrowR, origin.y + avail.y * 0.5f);
    const ImVec2 nextC(origin.x + avail.x - ui::Dp(16.0f) - arrowR, origin.y + avail.y * 0.5f);
    const bool showArrows = app.entries.size() > 1 && (hovered || ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem));
    auto inCircle = [&](const ImVec2& c) {
        const float dx = io.MousePos.x - c.x, dy = io.MousePos.y - c.y;
        return dx * dx + dy * dy <= arrowR * arrowR;
    };
    const bool onArrow = showArrows && (inCircle(prevC) || inCircle(nextC));
    if (showArrows) {
        auto arrow = [&](const char* id, const ImVec2& c, bool left) {
            ImGui::SetCursorScreenPos(ImVec2(c.x - arrowR, c.y - arrowR));
            const bool clicked = ImGui::InvisibleButton(id, ImVec2(arrowR * 2, arrowR * 2));
            const bool hot = ImGui::IsItemHovered();
            dl->AddCircleFilled(c, arrowR, hot ? IM_COL32(60, 120, 230, 235) : IM_COL32(10, 12, 16, 150), 32);
            dl->AddCircle(c, arrowR, IM_COL32(255, 255, 255, hot ? 220 : 120), 32, ui::Dp(1.5f));
            const float s = arrowR * 0.38f, dir = left ? -1.0f : 1.0f;
            const ImVec2 chevron[3] = {ImVec2(c.x - dir * s * 0.45f, c.y - s), ImVec2(c.x + dir * s * 0.55f, c.y), ImVec2(c.x - dir * s * 0.45f, c.y + s)};
            dl->AddPolyline(chevron, 3, IM_COL32(255, 255, 255, 240), 0, ui::Dp(2.5f));
            return clicked;
        };
        if (arrow("##viewer_prev", prevC, true)) app.SelectPrev();
        if (arrow("##viewer_next", nextC, false)) app.SelectNext();
    }

    if (app.currentMetadata.isVideo && app.selectedIndex >= 0 && app.selectedIndex < static_cast<int>(app.entries.size())) {
        if (app.VideoOpen()) {
            // playing: controls appear while the pointer is over the video (and always while paused)
            const bool paused = !app.videoPlayer->Playing();
            // AllowWhenBlockedByActiveItem: while a bar button is held down it is the "active item", which
            // would otherwise hide the bar on the release frame and swallow the click.
            const bool barVisible = paused || ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) ||
                                    ImGui::IsMouseDown(0) || ImGui::IsMouseReleased(0);
            const ImVec4 bar = DrawVideoControls(app, origin, avail, barVisible);
            const bool overBar = barVisible && io.MousePos.x >= bar.x && io.MousePos.x <= bar.z && io.MousePos.y >= bar.y && io.MousePos.y <= bar.w;
            if (app.videoPlayer->Failed()) {
                const std::string msg = app.videoPlayer->Error();
                const ImVec2 ts = ImGui::CalcTextSize(msg.c_str());
                dl->AddRectFilled(ImVec2(center.x - ts.x * 0.5f - 14, center.y - 16), ImVec2(center.x + ts.x * 0.5f + 14, center.y + 16), IM_COL32(10, 12, 16, 220), 8.0f);
                dl->AddText(ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f), IM_COL32(255, 170, 160, 255), msg.c_str());
            }
            // click the picture to pause/resume; double-click for fullscreen
            if (hovered && !overBar && !onArrow && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 4.0f).x == 0.0f &&
                !ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                app.videoPlayer->Toggle();
            }
            if (hovered && !overBar && !onArrow && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) SetFullscreen(g_hwnd, !g_fullscreen);
        } else {
            // a still poster frame: big play button over it (double-click starts it too)
            const float r = ui::Dp(38.0f);
            const float dx = io.MousePos.x - center.x, dy = io.MousePos.y - center.y;
            const bool overButton = hovered && !onArrow && dx * dx + dy * dy <= r * r;
            dl->AddCircleFilled(center, r, overButton ? IM_COL32(60, 120, 230, 235) : IM_COL32(10, 12, 16, 170), 32);
            dl->AddCircle(center, r, IM_COL32(255, 255, 255, 190), 32, ui::Dp(2.0f));
            dl->AddTriangleFilled(ImVec2(center.x - r * 0.28f, center.y - r * 0.42f), ImVec2(center.x - r * 0.28f, center.y + r * 0.42f),
                                  ImVec2(center.x + r * 0.46f, center.y), IM_COL32(255, 255, 255, 245));
            if (overButton) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if ((overButton && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 4.0f).x == 0.0f) ||
                (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))) {
                app.PlayVideo(app.selectedIndex);
            }
        }
    }

    // Overlays: image info top-left, zoom controls bottom-right.
    const float m = ui::Dp(10.0f);
    char info[96];
    unsigned long long bytes = (app.selectedIndex >= 0 && app.selectedIndex < static_cast<int>(app.entries.size()))
                                   ? app.entries[app.selectedIndex].size : 0;
    snprintf(info, sizeof(info), "%d \xC3\x97 %d   %s", app.currentTexture.width, app.currentTexture.height,
             ui::FormatBytes(bytes).c_str());
    ImVec2 its = ImGui::CalcTextSize(info);
    ImVec2 ip0(origin.x + m, origin.y + m);
    ImVec2 ip1(ip0.x + its.x + ui::Dp(16.0f), ip0.y + its.y + ui::Dp(8.0f));
    dl->AddRectFilled(ip0, ip1, IM_COL32(10, 12, 15, 170), ui::Dp(8.0f));
    dl->AddText(ImVec2(ip0.x + ui::Dp(8.0f), ip0.y + ui::Dp(4.0f)), IM_COL32(215, 220, 228, 255), info);

    char zoomLabel[16];
    snprintf(zoomLabel, sizeof(zoomLabel), "%d%%", static_cast<int>(app.zoom * 100.0f + 0.5f));
    ImGuiStyle& style = ImGui::GetStyle();
    const float bh = ImGui::GetFrameHeight();
    const float zw = ImGui::CalcTextSize("0000%").x;
    const float fitW = ImGui::CalcTextSize("Fit").x + style.FramePadding.x * 2.0f;
    const float oneW = ImGui::CalcTextSize("100%").x + style.FramePadding.x * 2.0f;
    const float totalW = zw + fitW + oneW + style.ItemSpacing.x * 2.0f;
    if (app.currentMetadata.isVideo) return; // videos have their own bar and always fit
    const ImVec2 zp(origin.x + avail.x - totalW - m, origin.y + avail.y - bh - m);
    dl->AddRectFilled(ImVec2(zp.x - ui::Dp(8.0f), zp.y - ui::Dp(4.0f)), ImVec2(zp.x + totalW + ui::Dp(4.0f), zp.y + bh + ui::Dp(4.0f)),
                      IM_COL32(10, 12, 15, 170), ui::Dp(8.0f)); // a chip like the info label's, so the zoom stays readable on bright pictures
    ImGui::SetCursorScreenPos(zp);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.05f, 0.06f, 0.08f, 0.70f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.30f, 0.44f, 0.90f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.28f, 0.40f, 0.60f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.92f, 0.96f, 1.0f));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImVec4(0.85f, 0.88f, 0.93f, 1.0f), "%s", zoomLabel);
    ImGui::SameLine();
    if (ImGui::Button("Fit")) app.fitToWindow = true;
    ImGui::SameLine();
    if (ImGui::Button("100%")) {
        app.zoom = 1.0f;
        app.fitToWindow = false;
        app.panX = app.panY = 0.0f;
    }
    ImGui::PopStyleColor(4);
}

void DrawMetadataPanel(AppState& app) {
    ImGui::BeginChild("metadata_scroll", ImVec2(ui::Dp(0.0f), 0), false);

    if (app.selectedIndex < 0 || app.selectedIndex >= static_cast<int>(app.entries.size())) {
        ImGui::Dummy(ImVec2(0, ui::Dp(12.0f)));
        ImGui::TextDisabled("Select an image to see its metadata.");
        ImGui::EndChild();
        return;
    }

    const ImageMetadata& meta = app.currentMetadata;
    const ImageEntry& sel = app.entries[app.selectedIndex];

    // ---- Header: file name, generator, size ----
    ImGui::PushFont(ui::BoldFont());
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(WideToUtf8(fs::path(sel.displayName).filename().wstring()).c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopFont();

    ui::Pill(GeneratorLabel(meta.generator), GeneratorColor(meta.generator), IM_COL32(255, 255, 255, 255));
    ImGui::SameLine();
    ImGui::TextDisabled("%d \xC3\x97 %d  -  %s", app.currentTexture.width, app.currentTexture.height,
                        ui::FormatBytes(sel.size).c_str());
    if (!meta.error.empty()) {
        ImGui::TextColored(ui::kBad, "Error: %s", meta.error.c_str());
    }
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Made");
        ImGui::SameLine();
        ImGui::TextUnformatted(sel.year ? (std::string(MonthName(sel.month)) + " " + std::to_string(sel.year)).c_str() : "unknown");
        ImGui::SameLine();
        ImGui::TextDisabled("   People");
        ImGui::SameLine();
        ImGui::TextUnformatted(PeopleLabel(sel.people).c_str());
        ImGui::SameLine();
        ImGui::TextDisabled(sel.peopleFromImage ? "(from image)" : "(from prompt)");
        if (!sel.peopleFromImage && app.PeopleModelAvailable()) {
            ImGui::SameLine();
            ImGui::BeginDisabled(app.IsPeopleScanning());
            if (ImGui::SmallButton("Detect")) app.StartPeopleScan({app.selectedIndex});
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_ForTooltip)) {
                ImGui::SetTooltip("Count the people visible in this image with the detection model");
            }
        }
    }
    { // how long ComfyUI took to make it (recorded when this app sent it)
        double took = 0;
        std::string what;
        if (ComfyGenerationTime(sel.path, took, &what)) {
            ImGui::TextDisabled("Took");
            ImGui::SameLine();
            ImGui::TextUnformatted(FormatTook(took).c_str());
            if (!what.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("to make (%s)", what.substr(0, what.find(':')).c_str());
            }
        }
    }

    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Rating");
        ImGui::SameLine();
        int stars = sel.rating;
        if (ui::RatingStars("detail_stars", stars)) ApplyRating(app, ActionTargets(app, app.selectedIndex), stars);
    }

    // ---- NSFW score ----
    {
        auto pct = [](float s) { return s < 0.0f ? std::string("-") : std::to_string(static_cast<int>(s * 100.0f + 0.5f)) + "%"; };
        float eff = EffectiveNsfwScore(sel, app.nsfwSource);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("NSFW score");
        ImGui::SameLine();
        if (eff >= 0.0f) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(NsfwScoreColor(app, eff)), "%s", pct(eff).c_str());
        } else {
            ImGui::TextDisabled("unknown");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(image %s%s, prompt %s)", pct(sel.nsfwImageScore).c_str(), sel.clefGraded ? " by Clef" : "", pct(sel.nsfwPromptScore).c_str());
        if (sel.nsfwImageScore < 0.0f) {
            ImGui::SameLine();
            ImGui::BeginDisabled(app.IsNsfwJobRunning());
            if (ImGui::SmallButton("Classify")) app.StartNsfwScoreJob({app.selectedIndex});
            ImGui::EndDisabled();
        }
    }

    {
        const float q = sel.Quality();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Quality");
        ImGui::SameLine();
        if (q >= 0.0f) {
            ImGui::TextColored(QualityColor(q), "%.1f / 10", q);
            ImGui::SameLine();
            if (sel.clefGraded) ImGui::TextDisabled("(graded by Clef)");
            else ImGui::TextDisabled("(anime model %.1f, general %.1f)", sel.qualityAnime, sel.qualityGeneral);
            if (sel.styleKind >= 0 && sel.styleKind < kStyleCount) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("Style");
                ImGui::SameLine();
                ImGui::TextUnformatted(kStyleNames[sel.styleKind]);
                ImGui::SameLine();
                ImGui::TextDisabled("(%d%% sure)", static_cast<int>(sel.styleConf * 100.0f + 0.5f));
            }
        } else {
            ImGui::TextDisabled("not scored");
            ImGui::SameLine();
            ImGui::BeginDisabled(app.IsQualityRunning());
            if (ImGui::SmallButton("Score")) app.StartQualityJob({app.selectedIndex});
            ImGui::EndDisabled();
        }
    }

    // ---- Actions ----
    if (meta.isVideo) {
        if (WrapButton("Play video", true)) app.PlayVideo(app.selectedIndex);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Play it here in the viewer (Space pauses)");
        if (WrapButton("Open in player", false)) ShellExecuteW(nullptr, L"open", sel.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::SameLine();
        ImGui::TextDisabled("%s%s%dx%d", meta.durationSec > 0 ? FormatDuration(meta.durationSec).c_str() : "", meta.durationSec > 0 ? "  -  " : "", meta.videoW, meta.videoH);
    }
    if (WrapButton("Describe with Vision", !meta.isVideo)) {
        app.showVisionWindow = true;
        if (!app.visionModel.empty()) app.DescribeWithVision({app.selectedIndex});
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Describe this image with the local vision model (what it writes depends on the Output setting in the Vision window)");
    if (sel.visionMask) { // (video prompts are written by the Video page: Make from it > Animate)
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.72f, 0.55f, 1.0f, 1.0f), "Described: %s", AppState::VisionMaskLabel(sel.visionMask).c_str());
    }
    // The main things to do with it as buttons; copying and the rest in two menus (fewer buttons to read, nothing lost).
    {
        const bool hasPrompt = !meta.parsed.positivePrompt.empty();
        const std::wstring path = sel.path;
        const int idx = app.selectedIndex;
        const std::string label = WideToUtf8(fs::path(path).filename().wstring());
        bool first = true;
        if (!meta.isVideo) { // two menus: change it, or make something from it
            if (WrapButton("Edit...", true)) ImGui::OpenPopup("##editmenu");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Change this picture (the Edits page)");
            first = false;
            bool wantPixel = false;
            if (ImGui::BeginPopup("##editmenu")) {
                if (ImGui::Selectable("Say what to change (Qwen)...")) OpenPictureEditor(app, {path});
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Qwen Image 2.1 Edit: anything a person does, a new background or style (~30 s)");
                if (ImGui::Selectable("Box edit: recolour, remove, change words (MOSS)...")) OpenLayoutStudio(app, path);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Only what is inside a box is drawn again (~4 s); the rest stays exactly as it is");
                if (ImGui::Selectable("Text over it: meme, thumbnail, caption (PhotoCraft)...")) OpenDesigner(app, {path});
                if (ImGui::Selectable("Upscale: bigger, better faces...")) OpenUpscaler(app, {path});
                if (ImGui::Selectable("Pixel art...")) wantPixel = true;
                ImGui::EndPopup();
            }
            if (wantPixel) ImGui::OpenPopup("##pxquick");
            if (ImGui::BeginPopup("##pxquick")) {
                PixelArtQuickContents(app, {path});
                ImGui::EndPopup();
            }
            if (WrapButton("Make from it...", false)) ImGui::OpenPopup("##makemenu");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Something new from this picture: another one like it, a video, a song");
            if (ImGui::BeginPopup("##makemenu")) {
                if (hasPrompt && ImGui::Selectable("Make it again (Generate, its prompt)")) GenerateWithPrompt(app, meta.parsed.positivePrompt);
                if (ImGui::Selectable("Make one like it (Generate, this picture as a reference)")) GenerateWithPrompt(app, hasPrompt ? meta.parsed.positivePrompt : std::string(), {path});
                if (hasPrompt && ImGui::Selectable("Send its prompt to my ComfyUI workflow")) SendToComfy(app, meta.parsed.positivePrompt, meta.parsed.negativePrompt, path, label);
                ImGui::Separator();
                if (ImGui::Selectable("Animate it (a video with sound)...")) OpenVideoPage(app, path, hasPrompt ? meta.parsed.positivePrompt : std::string());
                if (ImGui::Selectable("Star in a video (swap a person in a clip)...")) StarInVideo(app, path);
                if (ImGui::Selectable("A song about it...")) MusicFromPicture(app, path, hasPrompt ? meta.parsed.positivePrompt : std::string());
                ImGui::Separator();
                if (ImGui::Selectable("Characters from it (each person or creature in it)...")) OpenCharacterExtract(app, path);
                ImGui::EndPopup();
            }
        } else if (hasPrompt) {
            if (WrapButton("Make it again", first)) GenerateWithPrompt(app, meta.parsed.positivePrompt);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Its prompt on the Generate page");
            first = false;
        }
        if (!first) ui::SameLineIfFits("Copy______");
        if (ImGui::BeginCombo("##copymenu", "Copy", ImGuiComboFlags_WidthFitPreview | ImGuiComboFlags_HeightLargest)) {
            if (ImGui::Selectable("Copy the picture          Ctrl+C")) CopyPictures(app, {idx}, false);
            if (ImGui::Selectable("Copy without metadata     Ctrl+Shift+C")) CopyPictures(app, {idx}, true);
            ImGui::Separator();
            if (hasPrompt && ImGui::Selectable("Copy the prompt")) {
                ImGui::SetClipboardText(meta.parsed.positivePrompt.c_str());
                app.SetStatus("Copied the prompt.");
            }
            if (ImGui::Selectable("Copy as A1111 text")) {
                ImGui::SetClipboardText(BuildA1111Text(meta).c_str());
                app.SetStatus("Copied prompt and settings in AUTOMATIC1111 format.");
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Prompt, negative prompt and settings as one block that most generators accept when pasted");
            if (ImGui::Selectable("Copy all tags")) {
                ImGui::SetClipboardText(JoinTags(meta.parsed.tags, ", ").c_str());
                app.SetStatus("Copied " + std::to_string(meta.parsed.tags.size()) + " tags.");
            }
            if (ImGui::Selectable("Copy raw metadata")) {
                ImGui::SetClipboardText(meta.rawText.c_str());
                app.SetStatus("Copied raw metadata.");
            }
            const std::vector<EmbeddedWorkflow> workflows = FindWorkflows(meta);
            for (size_t wi = 0; wi < workflows.size(); ++wi) {
                const EmbeddedWorkflow& wf = workflows[wi];
                if (wf.kind == WorkflowKind::Parameters) continue; // "Copy as A1111 text" already covers it
                const std::string wl = std::string("Copy ") + wf.label + "##wf" + std::to_string(wi);
                if (ImGui::Selectable(wl.c_str())) {
                    ImGui::SetClipboardText(wf.text->c_str());
                    app.SetStatus(wf.kind == WorkflowKind::ComfyUI ? std::string("Copied the ComfyUI workflow. Paste it into ComfyUI with Ctrl+V.")
                                                                    : std::string("Copied the ") + wf.label + ".");
                }
            }
            if (ImGui::Selectable("Copy the file path")) {
                ImGui::SetClipboardText(WideToUtf8(path).c_str());
                app.SetStatus("Copied the file path.");
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Copy the picture, its prompt, settings, tags or workflow");
        ui::SameLineIfFits("More______");
        if (ImGui::BeginCombo("##moremenu", "More", ImGuiComboFlags_WidthFitPreview | ImGuiComboFlags_HeightLargest)) {
            if (hasPrompt && ImGui::Selectable("Edit the prompt, then send...")) OpenSendEditor(app, meta.parsed.positivePrompt, meta.parsed.negativePrompt, path, label);
            if (!meta.isVideo && ImGui::Selectable("Animate: a video with sound...")) OpenVideoPage(app, path, hasPrompt ? meta.parsed.positivePrompt : std::string());
            if (!meta.isVideo && ImGui::Selectable("Star in a video...")) StarInVideo(app, path);
            if (FindViewableWorkflow(FindWorkflows(meta)) && ImGui::Selectable("View the workflow")) OpenInNodeViewer(app, sel.path, meta);
            if (!meta.isVideo && ImGui::Selectable("Find pictures like this")) EmbedFindLike(app, idx);
            if (!meta.isVideo && ImGui::Selectable("Add to the Clef cast")) AddToClefCast(app, {path});
            if (ImGui::Selectable("Save a clean copy (no metadata)...")) {
                app.multiSelected = {idx};
                app.cleanMessage.clear();
                app.showCleanExport = true;
            }
            if (ImGui::Selectable("Show in Explorer")) ShowInExplorer(sel.path);
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Edit and send, the workflow, similar pictures, the Clef cast, a clean copy, Explorer");
    }
    ImGui::Spacing();

    if (!meta.hasMetadata) {
        ImGui::Separator();
        ImGui::TextDisabled("No embedded generation metadata found in this file.");
        ImGui::EndChild();
        return;
    }

    if (ImGui::CollapsingHeader("Positive prompt", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::SmallButton("Copy##pos")) {
            ImGui::SetClipboardText(meta.parsed.positivePrompt.c_str());
            app.SetStatus("Copied positive prompt.");
        }
        ImGui::TextWrapped("%s", meta.parsed.positivePrompt.empty() ? "(empty)" : meta.parsed.positivePrompt.c_str());
        ImGui::Spacing();
    }

    if (!meta.parsed.negativePrompt.empty() && ImGui::CollapsingHeader("Negative prompt", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::SmallButton("Copy##neg")) {
            ImGui::SetClipboardText(meta.parsed.negativePrompt.c_str());
            app.SetStatus("Copied negative prompt.");
        }
        ImGui::TextWrapped("%s", meta.parsed.negativePrompt.c_str());
        ImGui::Spacing();
    }

    if (ImGui::CollapsingHeader("Tags", ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawTagChips(meta.parsed.tags, "postags");
        ImGui::Spacing();
    }

    if (!meta.parsed.negativeTags.empty() && ImGui::CollapsingHeader("Negative tags")) {
        DrawTagChips(meta.parsed.negativeTags, "negtags");
        ImGui::Spacing();
    }

    if (ImGui::CollapsingHeader("Parameters", ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawParamsTable(meta.parsed.params);
        ImGui::Spacing();
    }

    if (ImGui::CollapsingHeader("Raw fields")) {
        for (size_t i = 0; i < meta.rawFields.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::TreeNode(meta.rawFields[i].key.c_str())) {
                if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(meta.rawFields[i].value.c_str());
                ImGui::TextWrapped("%s", meta.rawFields[i].value.c_str());
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }

    ImGui::EndChild();
}

void DrawScrapeDialog(AppState& app) {
    if (!app.showScrapeDialog) return;
    ImGui::OpenPopup("Scrape Folder");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ui::Dp(520.0f), 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Scrape Folder", &app.showScrapeDialog, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Scans a folder for images and exports every prompt, tag, and parameter found into a single CSV or JSON file.");
        ImGui::Spacing();

        static char folderBuf[MAX_PATH] = {0};
        if (app.scrapeFolder.empty() && !app.currentFolder.empty()) {
            std::string cur = WideToUtf8(app.currentFolder);
            strncpy_s(folderBuf, cur.c_str(), sizeof(folderBuf) - 1);
            app.scrapeFolder = folderBuf;
        }
        ImGui::InputText("Folder", folderBuf, sizeof(folderBuf));
        app.scrapeFolder = folderBuf;
        ImGui::SameLine();
        if (ImGui::Button("Browse...##folder")) {
            std::wstring picked = OpenFolderDialog(g_hwnd);
            if (!picked.empty()) {
                std::string u8 = WideToUtf8(picked);
                strncpy_s(folderBuf, u8.c_str(), sizeof(folderBuf) - 1);
                app.scrapeFolder = folderBuf;
            }
        }

        ImGui::Checkbox("Include subfolders", &app.scrapeRecursive);

        ImGui::RadioButton("CSV", &app.scrapeFormat, 0);
        ImGui::SameLine();
        ImGui::RadioButton("JSON", &app.scrapeFormat, 1);

        ImGui::Spacing();
        ImGui::BeginDisabled(app.IsScraping());
        if (ui::PrimaryButton("Run Scrape", ImVec2(ui::Dp(140.0f), 0))) {
            if (app.scrapeFolder.empty() || !fs::is_directory(Utf8ToWide(app.scrapeFolder))) {
                app.scrapeResultMessage = "Please choose a valid folder first.";
            } else {
                std::wstring outPath = SaveCsvJsonFileDialog(g_hwnd, app.scrapeFormat == 1);
                if (outPath.empty()) {
                    app.scrapeResultMessage = "Export canceled.";
                } else {
                    app.scrapeResultMessage.clear();
                    app.StartScrapeFolder(Utf8ToWide(app.scrapeFolder), app.scrapeRecursive,
                                          app.scrapeFormat == 1 ? ScrapeFormat::Json : ScrapeFormat::Csv, outPath);
                }
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(ui::Dp(100.0f), 0))) {
            app.showScrapeDialog = false;
            ImGui::CloseCurrentPopup();
        }

        if (app.IsScraping()) {
            int cur = app.ScrapeCurrent(), total = app.ScrapeTotal();
            float frac = total > 0 ? static_cast<float>(cur) / total : 0.0f;
            ImGui::Text("Scanning: %d / %d", cur, total);
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel##scrape")) app.CancelScrape();
            ImGui::ProgressBar(frac, ImVec2(-1, 0));
        }

        if (!app.scrapeResultMessage.empty()) {
            ImGui::Separator();
            ImGui::TextWrapped("%s", app.scrapeResultMessage.c_str());
        }

        ImGui::EndPopup();
    }
}

void DrawExportSelectedDialog(AppState& app) {
    if (!app.showExportSelectedDialog) return;
    ImGui::OpenPopup("Export Selected");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ui::Dp(420.0f), 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Export Selected", &app.showExportSelectedDialog, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Exports metadata for the %d selected image(s) to a single CSV or JSON file.",
                            static_cast<int>(app.multiSelected.size()));
        ImGui::Spacing();

        ImGui::RadioButton("CSV##sel", &app.scrapeFormat, 0);
        ImGui::SameLine();
        ImGui::RadioButton("JSON##sel", &app.scrapeFormat, 1);

        ImGui::Spacing();
        ImGui::BeginDisabled(app.IsScraping());
        if (ui::PrimaryButton("Export...", ImVec2(ui::Dp(120.0f), 0))) {
            std::wstring outPath = SaveCsvJsonFileDialog(g_hwnd, app.scrapeFormat == 1);
            if (outPath.empty()) {
                app.exportSelectedResultMessage = "Export canceled.";
            } else {
                std::vector<std::wstring> paths;
                for (int idx : app.multiSelected) {
                    if (idx >= 0 && idx < static_cast<int>(app.entries.size())) paths.push_back(app.entries[idx].path);
                }
                app.exportSelectedResultMessage.clear();
                app.StartScrapeFiles(paths, app.scrapeFormat == 1 ? ScrapeFormat::Json : ScrapeFormat::Csv, outPath);
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(ui::Dp(100.0f), 0))) {
            app.showExportSelectedDialog = false;
            ImGui::CloseCurrentPopup();
        }

        if (app.IsScraping()) {
            int cur = app.ScrapeCurrent(), total = app.ScrapeTotal();
            float frac = total > 0 ? static_cast<float>(cur) / total : 0.0f;
            ImGui::Text("Exporting: %d / %d", cur, total);
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel##export")) app.CancelScrape();
            ImGui::ProgressBar(frac, ImVec2(-1, 0));
        }

        if (!app.exportSelectedResultMessage.empty()) {
            ImGui::Separator();
            ImGui::TextWrapped("%s", app.exportSelectedResultMessage.c_str());
        }
        ImGui::EndPopup();
    }
}

// Asks before describing a lot of images at once (one click on "all in view" can mean thousands).
void DrawVisionConfirmDialog(AppState& app) {
    if (!app.showVisionConfirm) return;
    ImGui::OpenPopup("Describe many images?");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ui::Dp(460.0f), 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Describe many images?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const int total = static_cast<int>(app.visionPending.size());
        const int todo = app.CountVisionTodo(app.visionPending);
        ImGui::TextWrapped("You are about to send %d image%s to the vision model%s.", todo, todo == 1 ? "" : "s",
                           todo != total ? (" (" + std::to_string(total - todo) + " already have a result and are skipped)").c_str() : "");
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ui::kTextDim);
        ImGui::TextWrapped("Each image takes a few seconds or more, so this can run for a long time and keeps the GPU busy. "
                           "You can stop it at any moment with Cancel in the status bar at the bottom (or in the Vision window); "
                           "what is finished stays saved.");
        ImGui::PopStyleColor();
        ImGui::Spacing();
        static bool dontAsk = false;
        ImGui::Checkbox("Don't ask again", &dontAsk);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("You can turn the question back on in the Vision window.");
        ImGui::Spacing();
        if (ui::PrimaryButton(("Describe " + std::to_string(todo) + " images").c_str(), ImVec2(ui::Dp(170.0f), 0))) {
            if (dontAsk) {
                app.visionWarnBig = false;
                app.SaveVisionSettings();
            }
            std::vector<std::wstring> go = std::move(app.visionPending);
            app.visionPending.clear();
            app.showVisionConfirm = false;
            ImGui::CloseCurrentPopup();
            app.DescribePaths(go);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(ui::Dp(100.0f), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            app.visionPending.clear();
            app.showVisionConfirm = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void DrawDeleteConfirmDialog(AppState& app) {
    if (!app.showDeleteConfirm) return;
    ImGui::OpenPopup("Delete Images?");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ui::Dp(420.0f), 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Delete Images?", &app.showDeleteConfirm, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Move %d image(s) to the Recycle Bin? This can be undone from the Recycle Bin afterward.",
                            static_cast<int>(app.pendingDeletePaths.size()));
        ImGui::Spacing();
        if (ui::PrimaryButton("Delete", ImVec2(ui::Dp(120.0f), 0))) {
            std::wstring err;
            bool ok = DeleteFilesToRecycleBin(g_hwnd, app.pendingDeletePaths, &err);
            std::wstring folder = app.currentFolder;
            app.showDeleteConfirm = false;
            ImGui::CloseCurrentPopup();
            if (ok) {
                app.SetStatus("Deleted " + std::to_string(app.pendingDeletePaths.size()) + " image(s).");
                app.pendingDeletePaths.clear();
                if (!folder.empty()) app.OpenFolder(folder); // rescans; cache makes this fast for survivors
            } else {
                app.SetStatus("Delete failed: " + WideToUtf8(err));
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(ui::Dp(100.0f), 0))) {
            app.showDeleteConfirm = false;
            app.pendingDeletePaths.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void DrawTagGeneratorDialog(AppState& app) {
    if (!app.showTagGeneratorDialog) return;

    if (!app.tagGenInitialized) {
        for (const auto& cat : GetTagCategories()) {
            if (cat.defaultEnabled) app.tagGenEnabledCategories.insert(cat.name);
        }
        app.tagGenInitialized = true;
    }

    ImGui::OpenPopup("Random Tag Generator");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ui::Dp(480.0f), 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Random Tag Generator", &app.showTagGeneratorDialog, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped(
            "Picks a random set of generic Danbooru-style descriptive tags for prompt inspiration. "
            "Fully offline -- draws from a curated local list, plus optionally your own tags imported "
            "from a CSV/JSON file produced by \"Scrape Folder...\" or \"Export Selected...\".");
        ImGui::Spacing();

        ImGui::Text("Categories to draw from:");
        for (const auto& cat : GetTagCategories()) {
            bool enabled = app.tagGenEnabledCategories.count(cat.name) > 0;
            if (ImGui::Checkbox(cat.name.c_str(), &enabled)) {
                if (enabled) app.tagGenEnabledCategories.insert(cat.name);
                else app.tagGenEnabledCategories.erase(cat.name);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(%d)", static_cast<int>(cat.tags.size()));
        }
        if (!app.importedTags.empty()) {
            ImGui::Checkbox("Imported (from file)", &app.tagGenUseImported);
            ImGui::SameLine();
            ImGui::TextDisabled("(%d)", static_cast<int>(app.importedTags.size()));
        }

        ImGui::Spacing();
        if (ImGui::Button("Import CSV/JSON...")) {
            std::wstring path = OpenCsvJsonFileDialog(g_hwnd);
            if (!path.empty()) {
                TagImportResult r = ImportTagsFromFile(path);
                if (r.success) {
                    app.importedTags = std::move(r.tags);
                    app.tagGenUseImported = true;
                }
                app.tagImportResultMessage = r.message;
            }
        }
        if (!app.importedTags.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear Imported")) {
                app.importedTags.clear();
                app.tagImportResultMessage.clear();
            }
        }
        if (!app.tagImportResultMessage.empty()) {
            ImGui::TextDisabled("%s", app.tagImportResultMessage.c_str());
        }

        ImGui::Spacing();
        ImGui::SetNextItemWidth(ui::Dp(160.0f));
        ImGui::SliderInt("Number of tags", &app.tagGenCount, 3, 40);

        ImGui::Spacing();
        if (ui::PrimaryButton("Generate", ImVec2(ui::Dp(120.0f), 0))) {
            const std::vector<std::string> empty;
            app.generatedTags =
                GenerateRandomTags(app.tagGenEnabledCategories, app.tagGenUseImported ? app.importedTags : empty,
                                   app.tagGenCount);
        }
        ImGui::SameLine();
        if (ImGui::Button("Copy All", ImVec2(ui::Dp(100.0f), 0))) {
            ImGui::SetClipboardText(JoinTags(app.generatedTags, ", ").c_str());
        }
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(ui::Dp(100.0f), 0))) {
            app.showTagGeneratorDialog = false;
            ImGui::CloseCurrentPopup();
        }

        if (!app.generatedTags.empty()) {
            ImGui::Separator();
            ImGui::TextWrapped("%s", JoinTags(app.generatedTags, ", ").c_str());
            ImGui::Spacing();
            DrawTagChips(app.generatedTags, "gentags");
        } else {
            ImGui::Separator();
            ImGui::TextDisabled("Click Generate to get a random tag set.");
        }

        ImGui::EndPopup();
    }
}

// Single-line text field bound to a UTF-8 std::string.
bool InputString(const char* label, const char* hint, std::string& value, float width) {
    char buf[1024];
    strncpy_s(buf, value.c_str(), sizeof(buf) - 1);
    ImGui::SetNextItemWidth(ui::Dp(width));
    if (ImGui::InputTextWithHint(label, hint, buf, sizeof(buf))) {
        value = buf;
        return true;
    }
    return false;
}

void DrawNsfwDialog(AppState& app) {
    if (!app.showNsfwDialog) return;
    ImGui::OpenPopup("NSFW Sort");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ui::Dp(640.0f), 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("NSFW Sort", &app.showNsfwDialog, ImGuiWindowFlags_AlwaysAutoResize)) {
        static bool modelPresent = false, ffmpegPresent = false;
        if (ImGui::IsWindowAppearing()) {
            modelPresent = !NsfwClassifier::ModelId().empty();
            ffmpegPresent = NsfwClassifier::FfmpegAvailable();
        }
        const bool busy = app.IsNsfwJobRunning();

        if (modelPresent) {
            ImGui::TextColored(ui::kGood, "Image model: models\\nsfw.onnx found");
        } else {
            ImGui::TextColored(ui::kWarn,
                               "Image model: models\\nsfw.onnx not found (prompt keywords only)");
        }
        ImGui::SameLine();
        ImGui::TextDisabled(ffmpegPresent ? "| ffmpeg found (videos can be scored)"
                                          : "| ffmpeg not on PATH (videos -> unsorted)");

        // ---- Scoring settings (shared by everything below) ----
        ImGui::SeparatorText("Scoring");
        static const char* kSources[] = {"Image model + prompt keywords (higher wins)", "Image model only",
                                         "Prompt keywords only (instant, needs metadata)"};
        int src = static_cast<int>(app.nsfwSource);
        ImGui::SetNextItemWidth(ui::Dp(340.0f));
        if (ImGui::Combo("Score source", &src, kSources, IM_ARRAYSIZE(kSources))) {
            app.nsfwSource = static_cast<NsfwSource>(src);
        }
        ImGui::SetNextItemWidth(ui::Dp(220.0f));
        ImGui::SliderFloat("NSFW threshold", &app.nsfwThreshold, 0.05f, 0.95f, "%.2f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Score at or above this counts as NSFW");
        ImGui::Checkbox("Questionable band from", &app.nsfwUseQuestionable);
        ImGui::SameLine();
        ImGui::BeginDisabled(!app.nsfwUseQuestionable);
        ImGui::SetNextItemWidth(ui::Dp(120.0f));
        ImGui::SliderFloat("up to the threshold", &app.nsfwQuestionableLow, 0.0f, app.nsfwThreshold, "%.2f");
        ImGui::EndDisabled();
        app.nsfwQuestionableLow = std::min(app.nsfwQuestionableLow, app.nsfwThreshold);
        ImGui::Checkbox("Show score badges on thumbnails", &app.showNsfwBadges);

        // ---- Score what's in the gallery, without touching files ----
        ImGui::SeparatorText("Score gallery (no files are changed)");
        int selectedCount = static_cast<int>(app.multiSelected.size());
        ImGui::BeginDisabled(busy || app.entries.empty());
        if (ImGui::Button(("Score all " + std::to_string(app.entries.size()) + " image(s)").c_str())) {
            std::vector<int> indices;
            for (int i = 0; i < static_cast<int>(app.entries.size()); ++i) indices.push_back(i);
            app.nsfwResultMessage.clear();
            app.StartNsfwScoreJob(indices);
        }
        if (selectedCount >= 2) {
            ImGui::SameLine();
            if (ImGui::Button(("Score " + std::to_string(selectedCount) + " selected").c_str())) {
                app.nsfwResultMessage.clear();
                app.StartNsfwScoreJob(std::vector<int>(app.multiSelected.begin(), app.multiSelected.end()));
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Shows badges and enables \"Sort by: NSFW score\" and search filters like nsfw>0.5");
        }

        // ---- Organize a whole folder tree ----
        ImGui::SeparatorText("Collection (where \"Add Folder to Collection...\" sorts into)");
        if (InputString("##collection", "e.g. D:\\Pictures\\AI", app.organizeCollection, 440.0f)) {
            app.SaveOrganizeSettings();
        }
        ImGui::SameLine();
        if (ImGui::Button("Browse...##collection")) {
            std::wstring picked = OpenFolderDialog(g_hwnd);
            if (!picked.empty()) {
                app.organizeCollection = WideToUtf8(picked);
                app.SaveOrganizeSettings();
            }
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("Collection");

        ImGui::SeparatorText("Organize folder");
        std::string currentFolderUtf8 = WideToUtf8(app.currentFolder);
        InputString("##orgsrc", currentFolderUtf8.empty() ? "(choose a folder)" : currentFolderUtf8.c_str(),
                    app.organizeSource, 440.0f);
        ImGui::SameLine();
        if (ImGui::Button("Browse...##orgsrc")) {
            std::wstring picked = OpenFolderDialog(g_hwnd);
            if (!picked.empty()) app.organizeSource = WideToUtf8(picked);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("Folder");
        std::wstring sourceRoot = app.organizeSource.empty() ? app.currentFolder : Utf8ToWide(app.organizeSource);

        InputString("##orgdst", "(same as folder: organize in place)", app.organizeDest, 440.0f);
        ImGui::SameLine();
        if (ImGui::Button("Browse...##orgdst")) {
            std::wstring picked = OpenFolderDialog(g_hwnd);
            if (!picked.empty()) app.organizeDest = WideToUtf8(picked);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("Destination");
        std::wstring destRoot = app.organizeDest.empty() ? sourceRoot : Utf8ToWide(app.organizeDest);

        InputString("##orgexcl", "folders to leave alone, comma-separated", app.organizeExcludes, 440.0f);
        ImGui::SameLine();
        ImGui::TextUnformatted("Skip folders");
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
            ImGui::SetTooltip("Subfolders of the folder above (e.g. Training, ai-tagger) that are skipped entirely.\n"
                              "This app's own folder is always skipped.");
        }

        ImGui::Checkbox("Videos (score frames with ffmpeg)", &app.organizeVideos);
        ImGui::SameLine();
        ImGui::Checkbox("Other files -> <year>\\other", &app.organizeOtherFiles);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Non-image files (mp3, zip, ...) also move, so the old\n"
                              "folders can be removed. Captions (.txt/.json with the same name) always follow\n"
                              "their image. Files directly in the top folder are never moved.");
        }
        int copyMode = app.organizeCopy ? 1 : 0;
        ImGui::RadioButton("Move", &copyMode, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Copy", &copyMode, 1);
        app.organizeCopy = copyMode == 1;
        ImGui::SameLine();
        ImGui::BeginDisabled(app.organizeCopy);
        ImGui::Checkbox("Remove empty folders afterward", &app.organizeRemoveEmpty);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Checkbox("Detect duplicates", &app.organizeDuplicates);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Files byte-for-byte identical to one already in the collection (or to another file\n"
                              "in this run) go to <destination>\\_duplicates instead, for you to review/delete.");
        }
        ImGui::SameLine();
        ImGui::Checkbox("Keep original subfolders", &app.organizeKeepSubfolders);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Off: everything for a year/category lands in one folder (2026\\nsfw\\file.png).\n"
                              "On: 2026\\nsfw\\<original subfolders>\\file.png.\n"
                              "Same-named files are renamed \"name (2).png\" instead of overwritten.");
        }

        std::string destUtf8 = destRoot.empty() ? std::string("<folder>") : WideToUtf8(destRoot);
        const char* classes = app.nsfwUseQuestionable ? "nsfw|questionable|sfw|unsorted" : "nsfw|sfw|unsorted";
        const char* sub = app.organizeKeepSubfolders ? "\\<original subfolders>" : "";
        ImGui::TextDisabled("Images: %s\\<year>\\%s%s\\file", destUtf8.c_str(), classes, sub);
        ImGui::TextDisabled("Videos: %s\\<year>\\video-<%s>%s\\file", destUtf8.c_str(), classes, sub);

        auto makeOptions = [&](bool dryRun) { return app.MakeOrganizeOptions(sourceRoot, destRoot, dryRun); };
        bool sourceOk = !sourceRoot.empty() && fs::is_directory(sourceRoot);
        ImGui::BeginDisabled(busy || !sourceOk);
        if (ImGui::Button("Preview (dry run)", ImVec2(ui::Dp(150.0f), 0))) {
            app.nsfwResultMessage.clear();
            app.StartOrganize(makeOptions(true));
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Scores everything and reports what would move and which folders would be\n"
                              "removed, without changing anything. Scores are cached, so the real run is fast.");
        }
        ImGui::SameLine();
        if (ImGui::Button(app.organizeCopy ? "Organize (copy)..." : "Organize (move)...", ImVec2(ui::Dp(150.0f), 0))) {
            std::wstring prompt =
                std::wstring(app.organizeCopy ? L"Copy" : L"Move") + L" images, videos" +
                (app.organizeOtherFiles ? L" and other files" : L"") + L" under\n\n" + sourceRoot +
                L"\n\ninto <year>\\<nsfw|questionable|sfw|unsorted> (videos: <year>\\video-...) folders" +
                (app.organizeCopy || !app.organizeRemoveEmpty ? L"" : L", then remove folders left empty") +
                L"?\n\nEvery move is logged and can be reverted with \"Undo last organize\".";
            if (MessageBoxW(g_hwnd, prompt.c_str(), L"Organize folder", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                app.nsfwResultMessage.clear();
                app.StartOrganize(makeOptions(false));
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        static std::wstring lastLog;
        if (ImGui::IsWindowAppearing() || !busy) lastLog = LatestOrganizeLog();
        ImGui::BeginDisabled(busy || lastLog.empty());
        if (ImGui::Button("Undo last organize", ImVec2(ui::Dp(150.0f), 0))) {
            if (MessageBoxW(g_hwnd, L"Move every file from the last organize run back to where it was?",
                            L"Undo organize", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                app.nsfwResultMessage.clear();
                app.StartUndoOrganize();
            }
        }
        ImGui::EndDisabled();

        if (busy) {
            int cur = app.NsfwJobCurrent(), total = app.NsfwJobTotal();
            float frac = total > 0 ? static_cast<float>(cur) / total : 0.0f;
            ImGui::Text("%s: %d / %d", app.NsfwJobPhase(), cur, total);
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel##nsfw")) app.CancelNsfwJob();
            ImGui::ProgressBar(frac, ImVec2(-1, 0));
        }
        if (!app.nsfwResultMessage.empty()) {
            ImGui::Separator();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ui::Dp(620.0f));
            ImGui::TextUnformatted(app.nsfwResultMessage.c_str());
            ImGui::PopTextWrapPos();
        }

        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(ui::Dp(100.0f), 0))) {
            app.showNsfwDialog = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// ImGui text input bound to a growable std::string (as imgui_stdlib does).
int StringResizeCallback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* str = static_cast<std::string*>(data->UserData);
        str->resize(data->BufTextLen);
        data->Buf = str->data();
    }
    return 0;
}

bool InputTextMultilineString(const char* label, std::string& str, const ImVec2& size) {
    return ImGui::InputTextMultiline(label, str.data(), str.capacity() + 1, size, ImGuiInputTextFlags_CallbackResize,
                                     StringResizeCallback, &str);
}

void DrawVisionWindow(AppState& app) {
    if (!app.showVisionWindow) return;
    ImGui::SetNextWindowSize(ImVec2(ui::Dp(640.0f), ui::Dp(680.0f)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowCollapsed(false, ImGuiCond_Appearing);
    if (!ImGui::Begin("Vision Prompt", &app.showVisionWindow, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    const OllamaEndpoint ep = app.VisionEndpoint();
    static bool refreshed = false;
    if (!refreshed || ImGui::IsWindowAppearing()) {
        app.vision.Refresh(ep);
        refreshed = true;
    }
    VisionService::Snapshot snap = app.vision.GetSnapshot();
    const bool working = !snap.activity.empty();
    // Ollama unloads on its own when keep_alive runs out; poll so the
    // "loaded" indicator stays truthful.
    static double lastPoll = 0.0;
    if (snap.serverRunning && !snap.busy && ImGui::GetTime() - lastPoll > 5.0) {
        lastPoll = ImGui::GetTime();
        app.vision.PollLoaded(ep);
    }

    ImGui::TextWrapped("Describes an image with a local vision model (via Ollama) as a natural-language prompt you "
                       "can use to recreate it in newer generators. Works on any image, with or without metadata.");

    // ---- What to write (first thing in the window) ----
    ImGui::SeparatorText("What should it write?");
    {
        static const char* kModeNames[] = {"Image prompt", "Tag list", "Video scene"};
        static const char* kModeHints[] = {"One paragraph that recreates the image in a text-to-image model",
                                           "Danbooru-style tags for Stable Diffusion / Illustrious / Pony",
                                           "A detailed video: the image is the first frame, and it describes what happens over 5-30 seconds"};
        for (int i = 0; i < 3; ++i) {
            if (i) ImGui::SameLine();
            const bool active = static_cast<int>(app.visionMode) == i;
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button, ui::kAccentActive);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
            }
            if (ImGui::Button(kModeNames[i], ImVec2(ui::Dp(140.0f), 0))) {
                app.visionMode = static_cast<AppState::VisionMode>(i);
                app.SaveVisionSettings();
            }
            if (active) ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", kModeHints[i]);
        }
        ImGui::TextDisabled("%s", kModeHints[static_cast<int>(app.visionMode)]);

        if (app.visionMode == AppState::VisionMode::ImagePrompt) {
            // Re-imagine the prompt in a style: the model describes the picture as if it were drawn that way.
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Style");
            ImGui::SameLine();
            const int styles = VisionStyleCount();
            int cur = 0; // 0 as it is, 1..styles presets, styles+1 custom
            if (app.visionStyle == "custom") cur = styles + 1;
            else {
                for (int i = 0; i < styles; ++i) if (app.visionStyle == VisionStyleAt(i).key) cur = i + 1;
            }
            const char* preview = cur == 0 ? "Describe it as it is" : (cur == styles + 1 ? "Custom style..." : VisionStyleAt(cur - 1).label);
            ImGui::SetNextItemWidth(ui::Dp(260.0f));
            if (ImGui::BeginCombo("##visionstyle", preview)) {
                if (ImGui::Selectable("Describe it as it is", cur == 0)) { app.visionStyle.clear(); app.SaveVisionSettings(); }
                for (int i = 0; i < styles; ++i) {
                    if (ImGui::Selectable(VisionStyleAt(i).label, cur == i + 1)) { app.visionStyle = VisionStyleAt(i).key; app.SaveVisionSettings(); }
                }
                if (ImGui::Selectable("Custom style...", cur == styles + 1)) { app.visionStyle = "custom"; app.SaveVisionSettings(); }
                ImGui::EndCombo();
            }
            if (app.visionStyle == "custom") {
                if (InputString("##visionstylecustom", "e.g. dark fantasy oil painting, 1980s anime, felt puppets", app.visionStyleCustom, 420.0f)) app.SaveVisionSettings();
            }
            if (!app.StyleDirective().empty()) {
                ImGui::TextDisabled("The prompt is written as if the image were redrawn in this style, and saved next to the plain one\nas <name>.style-%s.txt (the plain prompt is not replaced).", app.StyleSlugKey().c_str());
            }
        }

        if (app.visionMode == AppState::VisionMode::VideoScene) {
            VideoSceneOptions& v = app.videoOptions;
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Length");
            for (int s : {5, 10, 15, 20, 30}) {
                ImGui::SameLine();
                const bool on = v.seconds == s;
                if (on) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ui::kAccentActive);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
                }
                char label[16];
                snprintf(label, sizeof(label), "%d s", s);
                if (ImGui::Button(label, ImVec2(ui::Dp(52.0f), 0))) {
                    v.seconds = s;
                    app.SaveVisionSettings();
                }
                if (on) ImGui::PopStyleColor(2);
            }
            ImGui::SetNextItemWidth(ui::Dp(320.0f));
            if (ImGui::SliderInt("##videolen", &v.seconds, 5, 30, "%d seconds")) app.SaveVisionSettings();
            if (v.timeline) {
                ImGui::SameLine();
                ImGui::TextDisabled("%d beat%s", VideoBeatCount(v.seconds), VideoBeatCount(v.seconds) == 1 ? "" : "s");
            }
            int fmt = v.timeline ? 0 : 1;
            static const char* kFormats[] = {"Time-coded beats (0:00-0:05 - ...)", "One flowing paragraph"};
            ImGui::SetNextItemWidth(ui::Dp(320.0f));
            if (ImGui::Combo("Format", &fmt, kFormats, IM_ARRAYSIZE(kFormats))) {
                v.timeline = fmt == 0;
                app.SaveVisionSettings();
            }
            ImGui::SetNextItemWidth(ui::Dp(320.0f));
            if (ImGui::BeginCombo("Camera", VideoCameraLabel(v.camera))) {
                for (int i = 0; i < VideoCameraCount(); ++i) {
                    if (ImGui::Selectable(VideoCameraLabel(i), v.camera == i)) {
                        v.camera = i;
                        app.SaveVisionSettings();
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::Checkbox("Describe sound and ambience", &v.sound)) app.SaveVisionSettings();
            if (InputString("##videoextra", "extra direction (optional), e.g. rain starts to fall", v.extra, 420.0f)) app.SaveVisionSettings();
            ImGui::TextDisabled("Longer clips get more beats and a longer answer, so they take longer to write.");
        }
    }

    // ---- Server + model ----
    ImGui::SeparatorText("Model");
    if (!snap.checked) {
        ImGui::TextDisabled("Checking Ollama...");
    } else if (snap.serverRunning) {
        ImGui::TextColored(ui::kGood, "Ollama running");
    } else {
        ImGui::TextColored(ui::kBad, "Ollama not running");
        ImGui::SameLine();
        ImGui::BeginDisabled(working);
        if (ImGui::SmallButton("Start Ollama")) app.vision.StartServer(ep);
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(working);
    if (ImGui::SmallButton("Refresh")) app.vision.Refresh(ep);
    ImGui::EndDisabled();

    if (app.visionModel.empty() && !snap.models.empty()) {
        // Default: Qwen3.5 (fast to load, good descriptions), else the first vision model.
        app.visionModel = snap.models.front().name;
        for (const auto& m : snap.models) {
            if (ToLowerStr(m.name).find("qwen3.5") != std::string::npos) {
                app.visionModel = m.name;
                break;
            }
        }
        app.SaveVisionSettings();
    }
    std::string preview = app.visionModel.empty() ? "(no vision model)" : app.visionModel;
    ImGui::SetNextItemWidth(ui::Dp(420.0f));
    if (ImGui::BeginCombo("Vision model", preview.c_str())) {
        for (const auto& m : snap.models) {
            std::string label = m.name + "  (" + m.sizeLabel + ")";
            if (ImGui::Selectable(label.c_str(), m.name == app.visionModel)) {
                app.visionModel = m.name;
                app.SaveVisionSettings();
            }
        }
        ImGui::EndCombo();
    }
    if (snap.checked && snap.serverRunning && snap.models.empty()) {
        ImGui::TextWrapped("No vision-capable models installed. In a terminal, run e.g.  ollama pull qwen2.5vl:7b");
    }

    bool loaded = std::find(snap.loadedModels.begin(), snap.loadedModels.end(), app.visionModel) != snap.loadedModels.end();
    if (loaded) ImGui::TextColored(ui::kGood, "Loaded in GPU memory");
    else ImGui::TextDisabled("Not loaded (loads automatically on first use)");
    ImGui::SameLine();
    ImGui::BeginDisabled(working || app.visionModel.empty() || !snap.serverRunning);
    if (!loaded && ImGui::SmallButton("Load now")) app.vision.Load(ep, app.visionModel, app.visionKeepAlive);
    if (loaded && ImGui::SmallButton("Unload")) app.vision.Unload(ep, app.visionModel);
    ImGui::EndDisabled();

    static const char* kKeepLabels[] = {"5 minutes", "10 minutes", "30 minutes", "1 hour", "until I unload it"};
    static const char* kKeepValues[] = {"5m", "10m", "30m", "1h", "-1"};
    int keepIdx = 1;
    for (int i = 0; i < IM_ARRAYSIZE(kKeepValues); ++i)
        if (app.visionKeepAlive == kKeepValues[i]) keepIdx = i;
    ImGui::SetNextItemWidth(ui::Dp(160.0f));
    if (ImGui::Combo("Keep loaded after use", &keepIdx, kKeepLabels, IM_ARRAYSIZE(kKeepLabels))) {
        app.visionKeepAlive = kKeepValues[keepIdx];
        app.SaveVisionSettings();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("After this long without use, Ollama frees the GPU memory by itself.\n"
                          "The model is also unloaded when you close this app.");
    }
    if (ImGui::Checkbox("When I close the app: unload all models and stop Ollama", &app.visionStopOnExit)) {
        app.SaveVisionSettings();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Only if Vision Prompt was used this session. Frees all GPU memory Ollama holds and\n"
                          "ends its processes (tray app included). Opening this window starts it again.\n"
                          "Turn off if you use Ollama with other apps; models are still unloaded.");
    }

    // ---- Output ----
    ImGui::SeparatorText("Output");
    if (InputString("##visionout", "folder to copy images + .txt prompts into", app.visionOutputDir, 420.0f)) {
        app.SaveVisionSettings();
    }
    ImGui::SameLine();
    if (ImGui::Button("Browse...##visionout")) {
        std::wstring picked = OpenFolderDialog(g_hwnd);
        if (!picked.empty()) {
            app.visionOutputDir = WideToUtf8(picked);
            app.SaveVisionSettings();
        }
    }
    if (ImGui::Checkbox("Auto-save: copy each described image there with a .txt of its prompt", &app.visionAutoSave)) {
        app.SaveVisionSettings();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The original image is never moved. The copy keeps its name and the prompt is written\n"
                          "next to it as <name>.txt. Describing the same image again replaces its .txt.");
    }
    if (ImGui::Checkbox("Give the model the image's original tags/prompt as a hint", &app.visionUseTags)) {
        app.SaveVisionSettings();
    }
    if (ImGui::CollapsingHeader("Default prompts (saved)")) {
        ImGui::TextDisabled("The wording sent to the model for each kind of result. Edits are saved automatically\n"
                            "and used every time, in the gallery, for whole folders and from the details panel.");
        if (app.visionInstruction.empty()) app.visionInstruction = DefaultVisionInstruction();
        if (app.visionTagsInstruction.empty()) app.visionTagsInstruction = DefaultTagsInstruction();
        if (app.videoOptions.templateText.empty()) app.videoOptions.templateText = DefaultVideoTemplate();
        if (ImGui::BeginTabBar("##defaultprompts")) {
            struct Case { const char* name; std::string* text; const char* builtin; ImGuiTabItemFlags flags; };
            const int cur = static_cast<int>(app.visionMode);
            Case cases[3] = {
                {"Image prompt", &app.visionInstruction, DefaultVisionInstruction(), 0},
                {"Tag list", &app.visionTagsInstruction, DefaultTagsInstruction(), 0},
                {"Video scene", &app.videoOptions.templateText, DefaultVideoTemplate(), 0},
            };
            static int lastMode = -1;
            if (lastMode != cur) {
                cases[cur].flags = ImGuiTabItemFlags_SetSelected; // the tab follows the mode picked above
                lastMode = cur;
            }
            for (int i = 0; i < 3; ++i) {
                if (!ImGui::BeginTabItem(cases[i].name, nullptr, cases[i].flags)) continue;
                const bool video = i == 2;
                const float h = ui::Dp(video ? 190.0f : 120.0f);
                std::string id = std::string("##dp") + std::to_string(i);
                if (InputTextMultilineString(id.c_str(), *cases[i].text, ImVec2(-1, h))) app.SaveVisionSettings();
                const bool isDefault = *cases[i].text == cases[i].builtin;
                ImGui::BeginDisabled(isDefault);
                if (ImGui::SmallButton("Reset to the built-in prompt")) {
                    *cases[i].text = cases[i].builtin;
                    app.SaveVisionSettings();
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextColored(isDefault ? ui::kTextDim : ui::kGood, "%s", isDefault ? "built-in" : "customised and saved");
                if (video) {
                    ImGui::TextDisabled("Placeholders filled in from the options above (keep them where you want that text):\n"
                                        "{SECONDS}  {PACE}  {CAMERA}  {STRUCTURE} (beats or paragraph)  {SOUND}  {EXTRA}");
                    if (ImGui::TreeNode("What the model will receive right now")) {
                        std::string preview = BuildVideoInstruction(app.videoOptions);
                        ImGui::InputTextMultiline("##videoinstr", preview.data(), preview.size() + 1, ImVec2(-1, ui::Dp(150.0f)),
                                                  ImGuiInputTextFlags_ReadOnly);
                        ImGui::TreePop();
                    }
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }

    // ---- Actions ----
    ImGui::SeparatorText("Describe");
    bool canRun = !working && snap.serverRunning && !app.visionModel.empty() && !app.IsVisionFolderScanning();
    ImGui::BeginDisabled(!canRun || app.selectedIndex < 0);
    const std::string describeLabel = app.visionMode == AppState::VisionMode::VideoScene
                                          ? "Write " + std::to_string(app.videoOptions.seconds) + " s video scene"
                                          : (app.visionMode == AppState::VisionMode::Tags ? "Generate tags for current image"
                                                                                           : "Describe current image");
    if (ImGui::Button(describeLabel.c_str())) app.DescribeWithVision({app.selectedIndex});
    ImGui::EndDisabled();
    int selectedCount = static_cast<int>(app.multiSelected.size());
    if (selectedCount > 1) {
        ImGui::SameLine();
        ImGui::BeginDisabled(!canRun);
        if (ImGui::Button(("Describe " + std::to_string(selectedCount) + " selected").c_str())) {
            app.RequestVisionBatch(std::vector<int>(app.multiSelected.begin(), app.multiSelected.end()));
        }
        ImGui::EndDisabled();
    }
    ImGui::BeginDisabled(!canRun || app.visibleOrder.empty());
    if (ImGui::Button(("Describe all " + std::to_string(app.visibleOrder.size()) + " in view").c_str())) {
        app.RequestVisionBatch(app.visibleOrder);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_ForTooltip)) {
        ImGui::SetTooltip("Every image currently shown in the gallery (search and filters applied)");
    }
    ImGui::SameLine();
    if (ImGui::Button("Browse results")) app.requestTab = 1;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Open the Vision library tab: read every saved prompt and video prompt next to its image");
    ImGui::SameLine();
    ImGui::BeginDisabled(!canRun);
    const bool openFolderDialog = ImGui::Button("Describe a folder...");
    ImGui::EndDisabled();
    if (openFolderDialog) ImGui::OpenPopup("Describe a folder");
    if (ImGui::Checkbox("Skip images that already have a result in the output folder", &app.visionSkipDone)) app.SaveVisionSettings();
    {
        char label[96];
        snprintf(label, sizeof(label), "Ask before describing more than %d images at once", app.visionWarnOver);
        if (ImGui::Checkbox(label, &app.visionWarnBig)) app.SaveVisionSettings();
    }
    if (!canRun && app.visionOutputDir.empty()) ImGui::TextDisabled("Tip: choose an output folder above, so batch results are saved as image + .txt.");

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Describe a folder", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        static std::string folder;
        static bool recursive = true;
        if (ImGui::IsWindowAppearing()) folder = WideToUtf8(app.currentFolder);
        ImGui::PushTextWrapPos(ui::Dp(520.0f));
        ImGui::TextUnformatted("Describes every image in a folder with the current Output setting. Each image is copied into the "
                               "output folder with its result as a .txt next to it; your originals are not touched.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        InputString("##vfolder", "folder to describe", folder, 420.0f);
        ImGui::SameLine();
        if (ImGui::Button("Browse...##vfolder")) {
            std::wstring picked = OpenFolderDialog(g_hwnd);
            if (!picked.empty()) folder = WideToUtf8(picked);
        }
        ImGui::Checkbox("Include subfolders", &recursive);
        ImGui::TextDisabled("Output folder: %s", app.visionOutputDir.empty() ? "(not set)" : app.visionOutputDir.c_str());
        if (app.hideVisionFolder && !app.visionOutputDir.empty()) ImGui::TextDisabled("The output folder and duplicate copies are skipped automatically.");
        ImGui::Spacing();
        const bool ok = !folder.empty() && fs::is_directory(Utf8ToWide(folder)) && !app.visionOutputDir.empty() && canRun;
        ImGui::BeginDisabled(!ok);
        if (ui::PrimaryButton("Start", ImVec2(ui::Dp(120.0f), 0))) {
            app.StartVisionFolder(Utf8ToWide(folder), recursive);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(ui::Dp(100.0f), 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (app.IsVisionFolderScanning()) {
        ImGui::TextColored(ui::kWarn, "Scanning the folder...");
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel##vfolderscan")) app.CancelVisionFolder();
    }
    if (working) {
        ImGui::TextColored(ui::kWarn, "%s", snap.activity.c_str());
        if (snap.total > 0) {
            ImGui::SameLine();
            ImGui::Text("%d / %d", snap.current, snap.total);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel##vision")) app.vision.Cancel();
    }
    if (!snap.lastError.empty()) ImGui::TextColored(ui::kBad, "%s", snap.lastError.c_str());
    if (!app.visionMessage.empty()) ImGui::TextDisabled("%s", app.visionMessage.c_str());

    // ---- Result for the selected image ----
    ImGui::SeparatorText(app.visionMode == AppState::VisionMode::VideoScene
                             ? "Video scene for the selected image"
                             : (app.visionMode == AppState::VisionMode::Tags ? "Tags for the selected image" : "Prompt for the selected image"));
    if (app.selectedIndex < 0 || app.selectedIndex >= static_cast<int>(app.entries.size())) {
        ImGui::TextDisabled("Select an image in the gallery.");
        ImGui::End();
        return;
    }
    const ImageEntry& sel = app.entries[app.selectedIndex];
    auto it = app.visionTexts.find(AppState::VisionTextKey(sel.path, app.VisionModeKey()));
    if (it == app.visionTexts.end()) {
        ImGui::TextDisabled("Not described yet.");
        ImGui::End();
        return;
    }
    static bool editing = false;
    ImGui::Checkbox("Edit", &editing);
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy##visionprompt")) ImGui::SetClipboardText(it->second.c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(app.visionOutputDir.empty());
    if (ImGui::SmallButton("Save to output folder")) {
        std::wstring saved;
        std::string err;
        if (SaveImageWithPrompt(sel.path, Utf8ToWide(app.visionOutputDir), it->second, saved, &err, app.VisionTxtSuffix())) {
            app.visionMessage = "Saved " + WideToUtf8(fs::path(saved).filename().wstring()) + " + .txt";
        } else {
            app.visionMessage = "Save failed: " + err;
        }
    }
    ImGui::EndDisabled();
    if (editing) {
        InputTextMultilineString("##visionresult", it->second, ImVec2(-1, -1));
    } else {
        ImGui::BeginChild("vision_result", ImVec2(ui::Dp(0.0f), 0), true);
        ImGui::TextWrapped("%s", it->second.c_str());
        ImGui::EndChild();
    }
    ImGui::End();
}

// One-click flow: pick any folder, confirm, and its images/videos are scored
// and moved into the collection (same settings as the NSFW Sort dialog).
void AddFolderToCollection(AppState& app) {
    std::wstring collection = Utf8ToWide(app.organizeCollection);
    std::error_code ec;
    if (collection.empty() || !fs::is_directory(collection, ec)) {
        app.showNsfwDialog = true;
        app.nsfwResultMessage = "Set the Collection folder first (e.g. D:\\Pictures\\AI).";
        return;
    }
    std::wstring picked = OpenFolderDialog(g_hwnd);
    if (picked.empty()) return;

    // Refuse a folder that contains the collection (e.g. the whole drive):
    // that would sweep unrelated files into it.
    std::wstring pickedKey = ToLowerStrW(fs::path(picked).lexically_normal().wstring());
    std::wstring collKey = ToLowerStrW(fs::path(collection).lexically_normal().wstring());
    while (pickedKey.size() > 3 && pickedKey.back() == L'\\') pickedKey.pop_back();
    while (collKey.size() > 3 && collKey.back() == L'\\') collKey.pop_back();
    if (collKey.size() > pickedKey.size() && collKey.compare(0, pickedKey.size(), pickedKey) == 0 &&
        (collKey[pickedKey.size()] == L'\\' || pickedKey.back() == L'\\')) {
        MessageBoxW(g_hwnd, L"That folder contains your collection folder. Pick the folder with the new images instead.",
                    L"Add Folder to Collection", MB_OK | MB_ICONWARNING);
        return;
    }

    std::wstring prompt = std::wstring(app.organizeCopy ? L"Copy" : L"Move") + L" the images and videos in\n\n" +
                          picked + L"\n\ninto\n\n" + collection +
                          L"\\<year>\\<nsfw|questionable|sfw> (videos: video-...)?" +
                          (app.organizeDuplicates ? L"\n\nExact duplicates of files already there go to _duplicates." : L"") +
                          (app.organizeCopy || !app.organizeRemoveEmpty ? L"" : L"\nSubfolders left empty are removed.") +
                          L"\n\nYou can revert it with \"Undo last organize\" in NSFW Sort.";
    if (MessageBoxW(g_hwnd, prompt.c_str(), L"Add Folder to Collection", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    app.nsfwResultMessage.clear();
    app.StartOrganize(app.MakeOrganizeOptions(picked, collection, /*dryRun=*/false));
    app.showNsfwDialog = true; // shows progress, then the result summary
}

void OpenPathFromExplorer(AppState& app, const std::wstring& path) {
    if (fs::is_directory(path)) {
        app.OpenFolder(path);
    } else if (fs::is_regular_file(path)) {
        app.OpenFile(path);
    }
}

// ---------------------------------------------------------------------------
// Application shell: menu bar, toolbar, status bar, welcome screen
// ---------------------------------------------------------------------------

constexpr const char* kAppVersion = "2.0";
bool g_showShortcuts = false;
bool g_showAbout = false;

void ToggleSlideshow(AppState& app) {
    if (app.entries.empty()) return;
    app.slideshowActive = !app.slideshowActive;
    if (app.slideshowActive) app.slideshowNextTime = ImGui::GetTime() + app.slideshowSeconds;
    app.SetStatus(app.slideshowActive ? "Slideshow started (Space or Esc to stop)." : "Slideshow stopped.");
}

void PickFolder(AppState& app) {
    std::wstring path = OpenFolderDialog(g_hwnd);
    if (!path.empty()) app.OpenFolder(path);
}

void PickFile(AppState& app) {
    std::wstring path = OpenImageFileDialog(g_hwnd);
    if (!path.empty()) app.OpenFile(path);
}

// Returns false (and tells the user) if the folder no longer exists.
bool OpenRecentFolder(AppState& app, const std::string& utf8Path) {
    std::wstring w = Utf8ToWide(utf8Path);
    std::error_code ec;
    if (fs::is_directory(w, ec)) {
        app.OpenFolder(w);
        return true;
    }
    app.SetStatus("That folder no longer exists: " + utf8Path);
    return false;
}

void OpenDataFolder() {
    wchar_t buf[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return;
    std::wstring dir = std::wstring(buf) + L"\\Try2Create";
    std::error_code ec;
    fs::create_directories(dir, ec);
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void DrawMenuBar(AppState& app) {
    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open folder...", "Ctrl+Shift+O")) PickFolder(app);
        if (ImGui::MenuItem("Open image...", "Ctrl+O")) PickFile(app);
        std::string toOpen, toForget;
        if (ImGui::BeginMenu("Recent folders", !app.recentFolders.empty())) {
            for (const auto& r : app.recentFolders) {
                if (ImGui::MenuItem(r.c_str())) toOpen = r;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Clear list")) app.recentFolders.clear();
            ImGui::EndMenu();
        }
        if (!toOpen.empty() && !OpenRecentFolder(app, toOpen)) {
            app.recentFolders.erase(std::remove(app.recentFolders.begin(), app.recentFolders.end(), toOpen),
                                    app.recentFolders.end());
        }
        if (ImGui::MenuItem("Reload folder", "F5", false, !app.currentFolder.empty())) app.ReloadFolder();
        ImGui::Separator();
        if (ImGui::MenuItem("Export selected...", nullptr, false, !app.multiSelected.empty())) {
            app.showExportSelectedDialog = true;
            app.exportSelectedResultMessage.clear();
        }
        if (ImGui::MenuItem("Export clean copies (no metadata)...", nullptr, false, !app.entries.empty())) {
            app.cleanMessage.clear();
            app.showCleanExport = true;
        }
        if (ImGui::MenuItem("Rename by metadata...", nullptr, false, !app.entries.empty())) {
            app.renameMessage.clear();
            app.showRename = true;
        }
        if (ImGui::MenuItem("Scrape folder to CSV/JSON...")) {
            app.showScrapeDialog = true;
            app.scrapeResultMessage.clear();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", "Alt+F4")) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Fit to window", "F")) app.fitToWindow = true;
        if (ImGui::MenuItem("Actual size (100%)", "Z")) {
            app.zoom = 1.0f;
            app.fitToWindow = false;
            app.panX = app.panY = 0.0f;
        }
        if (ImGui::MenuItem("Fullscreen", "F11", g_fullscreen)) SetFullscreen(g_hwnd, !g_fullscreen);
        if (ImGui::MenuItem("Slideshow", "Space", app.slideshowActive, !app.entries.empty())) ToggleSlideshow(app);
        if (ImGui::BeginMenu("Slideshow speed")) {
            for (float s : {2.0f, 3.0f, 5.0f, 10.0f, 20.0f}) {
                char label[32];
                snprintf(label, sizeof(label), "%.0f seconds per image", s);
                if (ImGui::MenuItem(label, nullptr, app.slideshowSeconds == s)) {
                    app.slideshowSeconds = s;
                    app.SaveUiSettings();
                }
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Compare the two selected images", "C", false, app.multiSelected.size() == 2)) {
            app.OpenCompare(*app.multiSelected.begin(), *app.multiSelected.rbegin());
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("Show images")) {
            if (ImGui::MenuItem("All images", nullptr, app.metadataMode == 0)) app.metadataMode = 0;
            if (ImGui::MenuItem("Only with metadata", nullptr, app.metadataMode == 1)) app.metadataMode = 1;
            if (ImGui::MenuItem("Only without metadata", nullptr, app.metadataMode == 2)) app.metadataMode = 2;
            ImGui::Separator();
            if (ImGui::MenuItem("Any (described or not)", nullptr, app.filters.vision == 0)) app.filters.vision = 0;
            if (ImGui::MenuItem("Only already described", nullptr, app.filters.vision == 1)) app.filters.vision = 1;
            if (ImGui::MenuItem("Only not described yet", nullptr, app.filters.vision == 2)) app.filters.vision = 2;
            ImGui::Separator();
            if (ImGui::MenuItem("Pictures and videos", nullptr, app.filters.kind == 0)) app.filters.kind = 0;
            if (ImGui::MenuItem("Pictures only", nullptr, app.filters.kind == 1)) app.filters.kind = 1;
            if (ImGui::MenuItem("Videos only", nullptr, app.filters.kind == 2)) app.filters.kind = 2;
            ImGui::EndMenu();
        }
        ImGui::MenuItem("Group the gallery by the current sort", nullptr, &app.showGroupHeaders);
        if (ImGui::MenuItem("Folder tree", "Ctrl+B", app.showFolderPane)) {
            app.showFolderPane = !app.showFolderPane;
            app.SaveUiSettings();
        }
        if (ImGui::MenuItem("Quality badges", nullptr, app.showQualityBadges)) {
            app.showQualityBadges = !app.showQualityBadges;
            app.SaveUiSettings();
        }
        if (ImGui::MenuItem("Show unreadable files", nullptr, app.showUnreadable)) {
            app.showUnreadable = !app.showUnreadable;
            app.SaveUiSettings();
        }
        if (ImGui::MenuItem("Play videos when selected", nullptr, app.videoAutoplay)) {
            app.videoAutoplay = !app.videoAutoplay;
            app.SaveUiSettings();
        }
        if (ImGui::MenuItem("Include subfolders", nullptr, app.scanSubfolders)) {
            app.scanSubfolders = !app.scanSubfolders;
            app.SaveUiSettings();
            app.ReloadFolder();
        }
        ImGui::MenuItem("NSFW score badges", nullptr, &app.showNsfwBadges);
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Video volume");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ui::Dp(150.0f));
            float pct = app.videoVolume * 100.0f;
            if (ImGui::SliderFloat("##menuvol", &pct, 0.0f, 100.0f, "%.0f%%")) {
                app.videoVolume = pct / 100.0f;
                VideoPlayer::SetVideoVolume(app.videoVolume);
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) app.SaveUiSettings();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Every video in the app plays at this volume (the gallery, the Video page, ComfyUI...). Music keeps its own.");
        }
        if (ImGui::MenuItem("Blur NSFW pictures", "Ctrl+Shift+B", app.blurNsfw)) {
            app.blurNsfw = !app.blurNsfw;
            app.nsfwRevealed.clear();
            app.SaveUiSettings();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("Pictures whose NSFW score is %.0f%% or more are blurred everywhere (gallery, viewer, libraries, ComfyUI).\n"
                              "Scores come from NSFW sort (the classifier) and from the prompt. A click on \"Show it\" reveals one picture.", app.nsfwThreshold * 100.0f);
        if (ImGui::MenuItem("Hide the Vision output folder", nullptr, app.hideVisionFolder)) {
            app.hideVisionFolder = !app.hideVisionFolder;
            app.SaveUiSettings();
            app.ReloadFolder();
        }
        if (ImGui::MenuItem("Hide duplicate copies of the same image", nullptr, app.hideDuplicateCopies)) {
            app.hideDuplicateCopies = !app.hideDuplicateCopies;
            app.SaveUiSettings();
            app.ReloadFolder();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Watch the folder for new images", nullptr, &app.watchFolder)) app.SaveUiSettings();
        if (ImGui::MenuItem("Jump to each new image", nullptr, &app.followNewest, app.watchFolder)) app.SaveUiSettings();
        ImGui::Separator();
        if (ImGui::MenuItem("Appearance...")) app.showAppearance = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Tools")) {
        if (ImGui::MenuItem("Layout designs: covers, posters, thumbnails...")) OpenLayoutStudio(app);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("Designs with exact words (magazine and album covers, posters, thumbnails, memes) drawn by MOSS-Image from boxes you can move");
        ImGui::Separator();
        if (ImGui::MenuItem("Find similar images...", nullptr, false, !app.entries.empty())) app.showSimilar = true;
        if (ImGui::MenuItem("Detect people in images...", nullptr, false, !app.entries.empty())) app.showPeopleDetect = true;
        if (ImGui::MenuItem("Statistics...")) app.showStats = true;
        if (ImGui::MenuItem("Score picture quality and style (all shown)", nullptr, false, !app.entries.empty() && !app.IsQualityRunning())) app.StartQualityJob(app.visibleOrder);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Rates how good each picture looks (0-10) with two small local models.\nThen sort, filter (Filters > Quality) or search with quality>6.");
        if (ImGui::MenuItem("Score quality of the selected pictures", nullptr, false, app.selectedIndex >= 0 && !app.IsQualityRunning())) {
            app.StartQualityJob(std::vector<int>(app.multiSelected.begin(), app.multiSelected.end()));
        }
        {
            auto clefItems = [&](const std::vector<int>& idx) {
                std::vector<ClefGradeInput> items;
                for (int i : idx) {
                    if (i < 0 || i >= static_cast<int>(app.entries.size())) continue;
                    const ImageEntry& e = app.entries[i];
                    items.push_back({e.path, static_cast<long long>(e.mtime.time_since_epoch().count()), e.size});
                }
                return items;
            };
            if (ClefGradeRunning()) {
                if (ImGui::MenuItem("Stop Clef grading")) ClefGradeStop();
            } else {
                if (ImGui::MenuItem("Regrade with Clef: NSFW, quality and style (all shown, new ones)", nullptr, false, !app.entries.empty())) ClefGradeStart(clefItems(app.visibleOrder), app.comfy.url, true);
                if (ImGui::MenuItem("Regrade with Clef: all shown again", nullptr, false, !app.entries.empty())) ClefGradeStart(clefItems(app.visibleOrder), app.comfy.url, false);
                if (ImGui::MenuItem("Regrade the selected pictures with Clef", nullptr, false, app.selectedIndex >= 0)) {
                    std::vector<int> sel(app.multiSelected.begin(), app.multiSelected.end());
                    if (sel.empty()) sel.push_back(app.selectedIndex);
                    ClefGradeStart(clefItems(sel), app.comfy.url, false);
                }
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("Clef (the decision model) LOOKS at each picture and grades how explicit it is, how good it looks (0-10) and what kind of picture it is.\nIts grades replace the small models' ones, and are kept. It takes the GPU from ComfyUI for a while (about 2 pictures a second).");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Random tag generator...")) app.showTagGeneratorDialog = true;
        if (ImGui::MenuItem("NSFW sort and organize...")) app.showNsfwDialog = true;
        if (ImGui::MenuItem("Vision prompt (Ollama)...")) app.showVisionWindow = !app.showVisionWindow;
        if (ImGui::MenuItem("Vision library (browse results)")) app.requestTab = 1;
        if (ImGui::MenuItem("Vision board (build a scene prompt)")) app.requestTab = 3;
        if (ImGui::MenuItem("Workflows (ComfyUI workflows in your pictures)")) app.requestTab = 2;
        if (ImGui::MenuItem("LoRA browser: see what each LoRA does...")) OpenLoraBrowser();
        if (ImGui::MenuItem("Sort LoRAs by model...")) OpenLoraSorter();
        if (ImGui::MenuItem("ComfyUI (generate from prompts)")) app.requestTab = 4;
        if (ImGui::BeginMenu("Video scene for the selected image", app.selectedIndex >= 0)) {
            for (int s : {5, 10, 15, 20, 30}) {
                char label[32];
                snprintf(label, sizeof(label), "%d seconds", s);
                if (ImGui::MenuItem(label)) RunVision(app, AppState::VisionMode::VideoScene, s, app.selectedIndex);
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Add folder to collection...", nullptr, false, !app.IsNsfwJobRunning())) AddFolderToCollection(app);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("Keyboard shortcuts")) g_showShortcuts = true;
        if (ImGui::MenuItem("Open settings and cache folder")) OpenDataFolder();
        if (ImGui::MenuItem("Crash and freeze reports")) {
            std::error_code ec;
            fs::create_directories(CrashLogDir(), ec);
            ShellExecuteW(nullptr, L"open", CrashLogDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("About")) g_showAbout = true;
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
}

float ButtonWidth(const char* label) {
    return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
}

// Green dot: the vision model is loaded in Ollama (ready, and using memory). Red: it is not (or Ollama isn't running).
// Click it to unload / load the model.
void DrawVisionStatusDot(AppState& app) {
    const OllamaEndpoint ep = app.VisionEndpoint();
    const VisionService::Snapshot vs = app.vision.GetSnapshot();
    static double lastPoll = -100.0;
    if (ImGui::GetTime() - lastPoll > 6.0 && !vs.busy) {
        lastPoll = ImGui::GetTime();
        if (!vs.checked || !vs.serverRunning) app.vision.Refresh(ep); // also notices Ollama being started later
        else app.vision.PollLoaded(ep);
    }
    bool loaded = false;
    for (const std::string& name : vs.loadedModels) {
        if (app.visionModel.empty() || name == app.visionModel) loaded = true;
    }
    const float h = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##visiondot", ImVec2(ui::Dp(30.0f), h));
    const bool hot = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p.x + ui::Dp(15.0f), p.y + h * 0.5f);
    const ImVec4 col = loaded ? ui::kGood : ui::kBad;
    if (hot) dl->AddCircleFilled(c, ui::Dp(11.0f), IM_COL32(255, 255, 255, 22), 24);
    if (loaded) dl->AddCircleFilled(c, ui::Dp(9.0f), ImGui::GetColorU32(ImVec4(col.x, col.y, col.z, 0.22f)), 24); // soft glow
    dl->AddCircleFilled(c, ui::Dp(5.5f), ImGui::GetColorU32(col), 20);
    dl->AddCircle(c, ui::Dp(5.5f), IM_COL32(0, 0, 0, 90), 20, 1.0f);
    if (hot) {
        ImGui::BeginTooltip();
        ImGui::Text("Vision model: %s", app.visionModel.empty() ? "(none chosen)" : app.visionModel.c_str());
        ImGui::TextColored(col, "%s", !vs.serverRunning ? "Ollama is not running" : (loaded ? "Loaded in memory" : "Not loaded"));
        ImGui::TextDisabled("Click for options");
        ImGui::EndTooltip();
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) ImGui::OpenPopup("##visionmenu");
    if (ImGui::BeginPopup("##visionmenu")) {
        ImGui::TextUnformatted("Vision model");
        ImGui::TextDisabled("%s", app.visionModel.empty() ? "(none chosen yet)" : app.visionModel.c_str());
        ImGui::TextColored(col, "%s", !vs.serverRunning ? "Ollama is not running" : (loaded ? "Loaded in memory" : "Not loaded"));
        ImGui::Separator();
        if (!vs.serverRunning) {
            if (ImGui::MenuItem("Start Ollama")) app.vision.StartServer(ep);
        } else {
            if (ImGui::MenuItem("Unload from memory", nullptr, false, !vs.loadedModels.empty())) {
                for (const std::string& name : vs.loadedModels) app.vision.Unload(ep, name);
                lastPoll = -100.0; // look again right away
                app.SetStatus("Unloading the vision model.");
            }
            if (ImGui::MenuItem("Load now", nullptr, false, !loaded && !app.visionModel.empty())) {
                app.vision.Load(ep, app.visionModel, app.visionKeepAlive);
                lastPoll = -100.0;
            }
        }
        if (ImGui::MenuItem("Open the Vision window")) app.showVisionWindow = true;
        ImGui::EndPopup();
    }
}

void DrawToolbar(AppState& app) {
    ImGuiStyle& style = ImGui::GetStyle();
    if (ui::PrimaryButton("Open folder")) PickFolder(app);
    ImGui::SameLine();
    if (ImGui::Button("Open image")) PickFile(app);
    if (app.activeTab == 0) { // these only mean something for the gallery
        ImGui::SameLine();
        ImGui::BeginDisabled(app.currentFolder.empty());
        if (ImGui::Button("Reload")) app.ReloadFolder();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Re-scan the folder (F5)");

        ImGui::SameLine(0, style.ItemSpacing.x * 2.0f);
        ImGui::BeginDisabled(app.entries.empty());
        if (ImGui::Button("Prev")) app.SelectPrev();
        ImGui::SameLine();
        if (ImGui::Button("Next")) app.SelectNext();
        ImGui::EndDisabled();
    }

    // On the right: only the NSFW blur, as a small eye (Scrape, Random tags, NSFW sort and Vision prompt are in the File and Tools menus)
    const float eyeW = ImGui::GetFrameHeight() * 1.4f;
    const float x = ImGui::GetWindowContentRegionMax().x - eyeW;
    const float leftEnd = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
    if (x > leftEnd + ui::Dp(24.0f)) {
        ImGui::SameLine(x);
        const bool blurOn = app.blurNsfw;
        if (blurOn) ImGui::PushStyleColor(ImGuiCol_Button, ui::WithAlpha(ui::kAccent, 0.85f));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::Button("##blureye", ImVec2(eyeW, ImGui::GetFrameHeight()))) {
            app.blurNsfw = !app.blurNsfw;
            app.nsfwRevealed.clear();
            app.SaveUiSettings();
        }
        if (blurOn) ImGui::PopStyleColor();
        { // an eye, struck through while the blur is off
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 c(p.x + eyeW * 0.5f, p.y + ImGui::GetFrameHeight() * 0.5f);
            const float rx = eyeW * 0.30f, ry = ImGui::GetFrameHeight() * 0.20f;
            const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
            for (int side = -1; side <= 1; side += 2) {
                ImVec2 pts[13];
                for (int k = 0; k <= 12; ++k) {
                    const float t = -1.0f + k / 6.0f;
                    pts[k] = ImVec2(c.x + t * rx, c.y + side * ry * (1.0f - t * t));
                }
                dl->AddPolyline(pts, 13, col, 0, ui::Dp(1.4f));
            }
            dl->AddCircleFilled(c, ry * 0.75f, col);
            if (!blurOn) dl->AddLine(ImVec2(c.x - rx, c.y + ry * 1.3f), ImVec2(c.x + rx, c.y - ry * 1.3f), col, ui::Dp(1.6f));
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("Blur NSFW pictures: %s\nPictures whose NSFW score is %.0f%% or more (from NSFW sort and the prompt) are blurred everywhere.\nCtrl+Shift+B",
                              app.blurNsfw ? "on" : "off", app.nsfwThreshold * 100.0f);
    }
}

void DrawStatusBar(AppState& app) {
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::Separator();
    const float rightEdge = ImGui::GetWindowContentRegionMax().x;
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, ui::kTextDim);

    // Left: what's open (on the gallery tab) or what the tab shows.
    if (app.activeTab == 1) {
        ImGui::TextUnformatted(app.visionOutputDir.empty() ? "No Vision results folder chosen" : app.visionOutputDir.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("|  %d results", static_cast<int>(app.visionLib.size()));
    } else if (app.activeTab == 4) {
        ImGui::Text("ComfyUI: %s", app.comfy.url.c_str());
    } else if (app.activeTab == 6) {
        static const char* kEditsStatus[] = {"Picture edits (Qwen Image 2.1 Edit)", "Designs and box edits (MOSS-Image)", "Text over a picture (PhotoCraft)",
                                             "Upscale: bigger pictures, better faces", "Apps: one-click picture recipes (Qwen Image 2.1 Edit)",
                                             "Videos: Make it crisp (Open-H3) and the quick upscale", "Edits: pick what you want to do"};
        ImGui::TextUnformatted(kEditsStatus[std::clamp(EditsMode(), 0, 6)]);
    } else if (app.activeTab == 7) {
        ImGui::TextUnformatted("Upscale: bigger pictures, better faces");
    } else if (app.activeTab == 8) {
        ImGui::TextUnformatted("Clef mode: scenarios made from choices, picture after picture");
    } else if (app.activeTab == 9) {
        ImGui::TextUnformatted("Pixel art: Retro Diffusion through ComfyUI, made into real pixels here");
    } else if (app.activeTab == 5) {
        int n = 0;
        for (const CharacterProfile& p : AllProfiles()) n += p.saved ? 1 : 0;
        ImGui::Text("%d saved character%s", n, n == 1 ? "" : "s");
    } else if (app.activeTab == 3) {
        ImGui::TextUnformatted("Vision board");
        ImGui::SameLine();
        ImGui::TextDisabled("|  %d cards, %d relations", static_cast<int>(app.board.board.nodes.size()), static_cast<int>(app.board.board.links.size()));
    } else if (app.activeTab == 2) {
        ImGui::TextUnformatted("Workflows");
    } else if (app.currentFolder.empty()) {
        ImGui::TextUnformatted("No folder open");
    } else {
        std::string folder = WideToUtf8(app.currentFolder);
        const float maxW = std::max(ui::Dp(120.0f), rightEdge * 0.38f);
        ImVec2 ts = ImGui::CalcTextSize(folder.c_str());
        if (ts.x > maxW) {
            // Keep the tail (the interesting part) of long paths.
            size_t cut = 0;
            while (cut + 4 < folder.size() &&
                   ImGui::CalcTextSize((std::string("...") + folder.substr(cut)).c_str()).x > maxW) {
                ++cut;
                while (cut < folder.size() && (static_cast<unsigned char>(folder[cut]) & 0xC0) == 0x80) ++cut; // UTF-8 boundary
            }
            folder = "..." + folder.substr(cut);
        }
        ImGui::TextUnformatted(folder.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", WideToUtf8(app.currentFolder).c_str());
    }
    ImGui::PopStyleColor();

    if (app.activeTab == 0 && !app.entries.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        if (app.visibleOrder.size() != app.entries.size()) {
            ImGui::TextDisabled("%d of %d images", static_cast<int>(app.visibleOrder.size()), static_cast<int>(app.entries.size()));
        } else {
            ImGui::TextDisabled("%d images", static_cast<int>(app.entries.size()));
        }
        if (app.HiddenCopies() > 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("|  %d duplicate cop%s hidden", app.HiddenCopies(), app.HiddenCopies() == 1 ? "y" : "ies");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Same file name, size and date as an image that is already shown (for example the copies in the Vision output folder).\nView > Hide duplicate copies turns this off.");
        }
        if (app.multiSelected.size() > 1) {
            ImGui::SameLine();
            ImGui::TextColored(ui::kAccentHover, "|  %d selected", static_cast<int>(app.multiSelected.size()));
        }
    }

    // Right: activity (progress) or the latest message.
    const float leftEnd = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
    struct Progress {
        std::string label;
        int cur = 0, total = 0;
        bool cancelable = false;
        int kind = 0; // 0 load, 1 nsfw, 2 scrape
    };
    std::vector<Progress> jobs;
    if (app.IsListing()) jobs.push_back({"Scanning folder: " + std::to_string(app.ListingFound()) + " images", 0, 0, false, 7});
    if (app.IsLoading()) jobs.push_back({"Loading", app.LoadedCount(), app.TotalCount(), true, 0});
    if (app.IsNsfwJobRunning()) jobs.push_back({app.NsfwJobPhase(), app.NsfwJobCurrent(), app.NsfwJobTotal(), true, 1});
    if (app.IsScraping()) jobs.push_back({"Exporting", app.ScrapeCurrent(), app.ScrapeTotal(), true, 2});
    if (app.IsSimilarScanning()) jobs.push_back({"Comparing", app.SimilarCurrent(), app.SimilarTotal(), true, 3});
    if (app.IsCleanExporting()) jobs.push_back({"Exporting copies", app.CleanCurrent(), app.CleanTotal(), true, 5});
    if (app.IsRenaming()) jobs.push_back({"Renaming", app.RenameCurrent(), app.RenameTotal(), false, 6});
    if (app.IsPeopleScanning()) jobs.push_back({"Counting people", app.PeopleCurrent(), app.PeopleTotal(), true, 4});
    if (app.IsVisionFolderScanning()) jobs.push_back({"Vision: finding images", 0, 0, true, 9});
    if (app.IsQualityRunning()) jobs.push_back({"Scoring quality", app.QualityCurrent(), app.QualityTotal(), true, 10});
    {
        const VisionService::Snapshot vs = app.vision.GetSnapshot();
        if (vs.busy && vs.total > 1) jobs.push_back({"Vision", vs.current, vs.total, true, 8});
    }

    float cursorFromRight = rightEdge;
    { // ComfyUI's jobs first, at the right end (every page)
        const float w = std::min(ui::Dp(460.0f), std::max(0.0f, rightEdge - leftEnd - ui::Dp(60.0f)));
        bool any = false;
        if (app.comfySvc) {
            for (const ComfyActivity& a : app.comfySvc->Activity()) any = any || (a.state != ComfyState::Done && a.state != ComfyState::Failed);
        }
        if (any && w > ui::Dp(160.0f)) {
            ImGui::SameLine(rightEdge - w);
            DrawComfyTracker(app, w);
            cursorFromRight = rightEdge - w - style.ItemSpacing.x * 2.0f;
        }
    }
    for (auto it = jobs.rbegin(); it != jobs.rend(); ++it) {
        const float barW = ui::Dp(150.0f);
        const float cancelW = ButtonWidth("Cancel");
        char overlay[64];
        snprintf(overlay, sizeof(overlay), "%d / %d", it->cur, it->total);
        float labelW = ImGui::CalcTextSize(it->label.c_str()).x;
        float total = labelW + barW + cancelW + style.ItemSpacing.x * 2.0f;
        float x = cursorFromRight - total;
        if (x < leftEnd + ui::Dp(40.0f)) break;
        ImGui::SameLine(x);
        ImGui::TextColored(ui::kWarn, "%s", it->label.c_str());
        ImGui::SameLine();
        float frac = it->total > 0 ? static_cast<float>(it->cur) / it->total : 0.0f;
        ImGui::ProgressBar(frac, ImVec2(barW, ImGui::GetFrameHeight() * 0.75f), overlay);
        ImGui::SameLine();
        ImGui::PushID(it->kind);
        if (ImGui::SmallButton("Cancel")) {
            if (it->kind == 0) app.CancelLoading();
            else if (it->kind == 1) app.CancelNsfwJob();
            else if (it->kind == 3) app.CancelSimilarScan();
            else if (it->kind == 4) app.CancelPeopleScan();
            else if (it->kind == 5) app.CancelCleanExport();
            else if (it->kind == 8) app.vision.Cancel();
            else if (it->kind == 9) app.CancelVisionFolder();
            else if (it->kind == 10) app.CancelQualityJob();
            else app.CancelScrape();
        }
        ImGui::PopID();
        cursorFromRight = x - style.ItemSpacing.x * 2.0f;
    }
    if (jobs.empty() && app.StatusActive()) {
        float w = ImGui::CalcTextSize(app.statusMessage.c_str()).x;
        float x = cursorFromRight - w;
        if (x > leftEnd + ui::Dp(40.0f)) {
            ImGui::SameLine(x);
            ImGui::TextColored(ui::kWarn, "%s", app.statusMessage.c_str());
        }
    }
}

// The app's logo (assets/logo.png, built into the exe), loaded once for the welcome screen and the About box.
const LoadedImage& AppLogo() {
    static LoadedImage logo;
    static bool tried = false;
    if (!tried && g_pd3dDevice) {
        tried = true;
        if (HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_LOGO_PNG), MAKEINTRESOURCEW(10) /* RT_RCDATA */)) {
            if (HGLOBAL mem = LoadResource(nullptr, res)) {
                const auto* bytes = static_cast<const unsigned char*>(LockResource(mem));
                std::string err;
                if (bytes) LoadImageTextureFromMemory(g_pd3dDevice, bytes, SizeofResource(nullptr, res), logo, &err);
            }
        }
    }
    return logo;
}

void DrawWelcome(AppState& app, float height) {
    ImGui::BeginChild("welcome", ImVec2(0, height), false);
    const float width = std::min(ImGui::GetContentRegionAvail().x, ui::Dp(520.0f));
    const float x0 = (ImGui::GetContentRegionAvail().x - width) * 0.5f;
    ImGui::SetCursorPosY(std::max(ui::Dp(24.0f), (height - ui::Dp(380.0f)) * 0.35f));
    ImGui::SetCursorPosX(x0);
    ImGui::BeginGroup();
    const LoadedImage& logo = AppLogo();
    if (logo.srv) {
        const float ls = ui::Dp(64.0f);
        ImGui::Image((ImTextureID)(intptr_t)logo.srv, ImVec2(ls, ls));
        ImGui::SameLine(0, ui::Dp(14.0f));
        ImGui::PushFont(ui::TitleFont());
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (ls - ImGui::GetFontSize()) * 0.5f);
        ImGui::TextUnformatted("Try2Create");
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, ui::Dp(4.0f)));
    } else {
        ImGui::PushFont(ui::TitleFont());
        ImGui::TextUnformatted("Try2Create");
        ImGui::PopFont();
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
    ImGui::TextColored(ui::kTextDim,
                       "Make pictures, videos with sound, pixel art and characters with AI, and read the prompt, settings and tags inside "
                       "every picture (AUTOMATIC1111, ComfyUI, NovelAI, InvokeAI and compatible tools).");
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, ui::Dp(8.0f)));

    if (ui::PrimaryButton("Open folder...", ImVec2(ui::Dp(170.0f), ui::Dp(36.0f)))) PickFolder(app);
    ImGui::SameLine();
    if (ImGui::Button("Open image...", ImVec2(ui::Dp(150.0f), ui::Dp(36.0f)))) PickFile(app);
    ImGui::TextColored(ui::kTextDim, "or drag and drop a folder or image onto this window");

    if (!app.recentFolders.empty()) {
        ImGui::Dummy(ImVec2(0, ui::Dp(10.0f)));
        ui::SectionHeading("RECENT FOLDERS");
        // cards with a fan of the folder's first pictures (HeliosGen's empty state), found in the background: a slow or offline drive never stalls this
        // (a detached thread, not std::async: its future would hold up closing the app while a dead network drive answers)
        struct Preview {
            std::mutex m;
            bool done = false;
            std::vector<std::wstring> pics;
        };
        static std::map<std::string, std::shared_ptr<Preview>> previews;
        const float gapC = ui::Dp(12.0f);
        const int perRow = std::max(1, static_cast<int>((width + gapC) / (ui::Dp(150.0f) + gapC)));
        const float cardW = (width - gapC * (perRow - 1)) / perRow, cardH = cardW * 0.62f + ui::Dp(46.0f);
        std::string forget;
        const size_t n = std::min<size_t>(app.recentFolders.size(), static_cast<size_t>(perRow) * 2);
        for (size_t i = 0; i < n; ++i) {
            const std::string folder = app.recentFolders[i];
            if (!previews.count(folder)) {
                auto pv = std::make_shared<Preview>();
                previews[folder] = pv;
                std::thread([pv, w = Utf8ToWide(folder)]() {
                    std::vector<std::wstring> pics;
                    std::error_code ec;
                    int looked = 0;
                    for (fs::directory_iterator it(w, fs::directory_options::skip_permission_denied, ec), endIt; !ec && it != endIt && pics.size() < 3 && looked < 400; it.increment(ec), ++looked) {
                        std::wstring ext = it->path().extension().wstring();
                        for (wchar_t& ch : ext) ch = static_cast<wchar_t>(towlower(ch));
                        if (ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".webp") pics.push_back(it->path().wstring());
                    }
                    std::lock_guard<std::mutex> lock(pv->m);
                    pv->pics = std::move(pics);
                    pv->done = true;
                }).detach();
            }
            if (i % perRow) ImGui::SameLine(0.0f, gapC);
            ImGui::PushID(static_cast<int>(i));
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            const bool clicked = ImGui::InvisibleButton("##recent", ImVec2(cardW, cardH));
            const bool hov = ImGui::IsItemHovered();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float lift = hov ? ui::Dp(2.0f) : 0.0f;
            const ImVec2 a(p0.x, p0.y - lift), b(p0.x + cardW, p0.y + cardH - lift);
            dl->AddRectFilled(a, b, ImGui::GetColorU32(hov ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), ui::Dp(10.0f));
            dl->AddRect(a, b, hov ? ui::WithAlpha(ui::kAccent, 0.7f) : IM_COL32(255, 255, 255, 22), ui::Dp(10.0f), 0, 1.0f);
            // the fan
            const ImVec2 fc(a.x + cardW * 0.5f, a.y + ui::Dp(8.0f) + cardW * 0.29f);
            const float th = cardW * 0.5f;
            std::vector<std::wstring> pics;
            {
                Preview& pv = *previews[folder];
                std::lock_guard<std::mutex> lock(pv.m);
                if (pv.done) pics = pv.pics;
            }
            if (pics.empty()) {
                DrawNavIcon(dl, 0, fc, ui::Dp(26.0f), IM_COL32(255, 255, 255, 40));
            } else {
                const int k = static_cast<int>(pics.size());
                for (int j = 0; j < k; ++j) {
                    const float spread = hov ? 0.20f : 0.14f;
                    const float ang = k == 1 ? 0.0f : (j - (k - 1) * 0.5f) * spread;
                    const ImVec2 c(fc.x + (j - (k - 1) * 0.5f) * th * (hov ? 0.42f : 0.34f), fc.y + std::fabs(j - (k - 1) * 0.5f) * ui::Dp(5.0f));
                    const float hw = th * 0.38f, hh = th * 0.5f, cs = std::cos(ang), sn = std::sin(ang);
                    auto at = [&](float x, float y) { return ImVec2(c.x + x * cs - y * sn, c.y + x * sn + y * cs); };
                    const ImVec2 q[4] = {at(-hw, -hh), at(hw, -hh), at(hw, hh), at(-hw, hh)};
                    const ImVec2 sh(ui::Dp(2.0f), ui::Dp(3.0f));
                    dl->AddQuadFilled(ImVec2(q[0].x + sh.x, q[0].y + sh.y), ImVec2(q[1].x + sh.x, q[1].y + sh.y), ImVec2(q[2].x + sh.x, q[2].y + sh.y), ImVec2(q[3].x + sh.x, q[3].y + sh.y), IM_COL32(0, 0, 0, 90));
                    const AppState::LibThumb lt = app.LibraryThumb(pics[j]);
                    if (lt.srv && lt.w > 0 && lt.h > 0) {
                        ImVec2 uv0(0, 0), uv1(1, 1); // cover the portrait card
                        const float want = hw / hh, have = static_cast<float>(lt.w) / lt.h;
                        if (have > want) uv0.x = (1.0f - want / have) * 0.5f, uv1.x = 1.0f - uv0.x;
                        else uv0.y = (1.0f - have / want) * 0.5f, uv1.y = 1.0f - uv0.y;
                        dl->AddImageQuad((ImTextureID)(intptr_t)lt.srv, q[0], q[1], q[2], q[3], uv0, ImVec2(uv1.x, uv0.y), uv1, ImVec2(uv0.x, uv1.y));
                    } else {
                        dl->AddQuadFilled(q[0], q[1], q[2], q[3], ui::ThumbPlaceholder());
                    }
                    dl->AddQuad(q[0], q[1], q[2], q[3], IM_COL32(255, 255, 255, 60), 1.0f);
                }
            }
            // name and where it is
            const fs::path fp(Utf8ToWide(folder));
            const std::string name = WideToUtf8(fp.filename().wstring()).empty() ? folder : WideToUtf8(fp.filename().wstring());
            const std::string where = WideToUtf8(fp.parent_path().wstring());
            const float tx = a.x + ui::Dp(10.0f), right = b.x - ui::Dp(8.0f), ty = b.y - ui::Dp(40.0f);
            ImGui::PushFont(ui::BoldFont());
            ImGui::RenderTextEllipsis(dl, ImVec2(tx, ty), ImVec2(right, ty + ui::Dp(20.0f)), right, right, name.c_str(), nullptr, nullptr);
            ImGui::PopFont();
            ImGui::PushStyleColor(ImGuiCol_Text, ui::kTextDim);
            ImGui::RenderTextEllipsis(dl, ImVec2(tx, ty + ui::Dp(19.0f)), ImVec2(right, ty + ui::Dp(38.0f)), right, right, where.c_str(), nullptr, nullptr);
            ImGui::PopStyleColor();
            if (hov) ImGui::SetTooltip("%s", folder.c_str());
            if (clicked && !OpenRecentFolder(app, folder)) forget = folder;
            ImGui::PopID();
        }
        if (!forget.empty()) {
            app.recentFolders.erase(std::remove(app.recentFolders.begin(), app.recentFolders.end(), forget),
                                    app.recentFolders.end());
        }
    }
    ImGui::EndGroup();
    ImGui::EndChild();
}

void DrawShortcutsPopup() {
    if (!g_showShortcuts) return;
    ImGui::OpenPopup("Keyboard shortcuts");
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Keyboard shortcuts", &g_showShortcuts, ImGuiWindowFlags_AlwaysAutoResize)) {
        static const char* kRows[][2] = {
            {"Left / Right", "Previous / next image"},
            {"Up / Down", "Move a row up / down in the gallery"},
            {"Home / End", "First / last image"},
            {"Ctrl+Click, Shift+Click", "Multi-select, range select"},
            {"Ctrl+F", "Focus the search box"},
            {"Esc", "Clear search / collapse multi-selection"},
            {"Ctrl+O", "Open image"},
            {"Ctrl+Shift+O", "Open folder"},
            {"Ctrl+B", "Show / hide the folder tree"},
            {"Ctrl+K", "Search everything: pages, actions, recent folders"},
            {"Ctrl+G", "Write in the gallery's make bar (Ctrl+Enter makes it)"},
            {"Ctrl+1 ... Ctrl+9, Ctrl+0", "Pages: Gallery, Vision library, Workflows, Vision board, ComfyUI, Characters, Edits, Upscale, Clef mode, Pixel art"},
            {"Ctrl+\\", "Sidebar: only icons / names too"},
            {"Ctrl+Enter", "Run (Make it, Edit, Send) in the window you are typing in"},
            {"Ctrl+C / Ctrl+Shift+C", "Copy the picture(s) to the clipboard / without metadata"},
            {"F5", "Reload folder"},
            {"Delete", "Move selected image(s) to the Recycle Bin"},
            {"F  /  Z", "Fit to window  /  actual size"},
            {"1-5, 0", "Rate the selected image(s) 1-5 stars, clear"},
            {"C", "Compare two selected images side by side"},
            {"F11", "Fullscreen (Esc leaves)"},
            {"Space", "Start / stop slideshow"},
            {"Mouse wheel", "Zoom (around the cursor)"},
            {"Drag, double-click", "Pan the image, toggle fit / 100%"},
            {"Right-click a thumbnail", "Show in Explorer, copy prompt, rate, clean copy, rename, delete"},
            {"Drag a thumbnail out", "Copy the file(s) into Explorer, a browser or a chat app"},
            {"Vision library tab", "Browse saved Vision results; Up/Down move through them"},
            {"Workflows page", "ComfyUI node viewer + library of extracted workflows; drag to pan, wheel to zoom"},
        };
        if (ImGui::BeginTable("shortcuts", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
            for (auto& r : kRows) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(ui::kAccentHover, "%s", r[0]);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(r[1]);
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
        if (ui::PrimaryButton("Close", ImVec2(ui::Dp(100.0f), 0))) {
            g_showShortcuts = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void DrawAboutPopup() {
    if (!g_showAbout) return;
    ImGui::OpenPopup("About");
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("About", &g_showAbout, ImGuiWindowFlags_AlwaysAutoResize)) {
        const LoadedImage& logo = AppLogo();
        if (logo.srv) {
            ImGui::Image((ImTextureID)(intptr_t)logo.srv, ImVec2(ui::Dp(72.0f), ui::Dp(72.0f)));
            ImGui::SameLine();
        }
        ImGui::BeginGroup();
        ImGui::Dummy(ImVec2(0, ui::Dp(10.0f)));
        ImGui::PushFont(ui::BoldFont());
        ImGui::Text("Try2Create  %s", kAppVersion);
        ImGui::PopFont();
        ImGui::TextDisabled("Dear ImGui %s, ONNX Runtime + DirectML, stb_image, nlohmann/json", IMGUI_VERSION);
        ImGui::EndGroup();
        ImGui::Spacing();
        ImGui::TextUnformatted("Settings, thumbnails and score caches are kept in:");
        wchar_t buf[MAX_PATH];
        DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
        std::string dir = (len > 0 && len < MAX_PATH) ? WideToUtf8(std::wstring(buf) + L"\\Try2Create") : "(unavailable)";
        ImGui::TextColored(ui::kTextDim, "%s", dir.c_str());
        ImGui::Spacing();
        if (ImGui::Button("Open that folder")) OpenDataFolder();
        ImGui::SameLine();
        if (ui::PrimaryButton("Close", ImVec2(ui::Dp(100.0f), 0))) {
            g_showAbout = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// Borderless full-window viewer with an auto-hiding caption (F11, Esc to leave).
// Full screen's info panel (I): the prompt and settings beside the picture, like HeliosGen's lightbox, with the common actions.
bool g_fsInfo = false;
void DrawFullscreenInfo(AppState& app, float width) {
    if (app.selectedIndex < 0 || app.selectedIndex >= static_cast<int>(app.entries.size())) return;
    const int i = app.selectedIndex;
    ImageEntry& e = app.entries[i];
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.065f, 0.08f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui::Dp(16.0f), ui::Dp(16.0f)));
    ImGui::BeginChild("fs_info", ImVec2(width, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    ImGui::PushFont(ui::BoldFont());
    ImGui::TextWrapped("%s", WideToUtf8(fs::path(e.displayName).filename().wstring()).c_str());
    ImGui::PopFont();
    ImGui::TextDisabled("%d × %d   %s", app.currentTexture.width, app.currentTexture.height, ui::FormatBytes(e.size).c_str());
    // rating: click a star (0-5 keys work too)
    for (int r = 1; r <= 5; ++r) {
        if (r > 1) ImGui::SameLine(0.0f, ui::Dp(2.0f));
        ImGui::PushID(r);
        ImGui::PushStyleColor(ImGuiCol_Text, ui::StarColor(e.rating >= r));
        if (ImGui::SmallButton("\xE2\x98\x85")) app.SetRating({i}, e.rating == r ? 0 : r);
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::Spacing();
    const std::string& prompt = e.metadata.parsed.positivePrompt;
    ui::SectionHeading("PROMPT");
    if (prompt.empty()) {
        ImGui::TextDisabled(e.metadata.isVideo ? "A video (no prompt in it)" : "No prompt in this file");
    } else {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(prompt.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::SmallButton("Copy the prompt")) {
            ImGui::SetClipboardText(prompt.c_str());
            Toast(ToastKind::Info, "Prompt copied", prompt.size() > 90 ? prompt.substr(0, 90) + "..." : prompt, {}, {}, "copyprompt");
        }
    }
    if (!e.metadata.parsed.negativePrompt.empty()) {
        ImGui::Spacing();
        ui::SectionHeading("NEGATIVE");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ui::kTextDim, "%s", e.metadata.parsed.negativePrompt.c_str());
        ImGui::PopTextWrapPos();
    }
    if (!e.metadata.parsed.params.empty()) {
        ImGui::Spacing();
        ui::SectionHeading("SETTINGS");
        DrawParamsTable(e.metadata.parsed.params);
    }
    ImGui::Spacing();
    ui::SectionHeading("DO");
    if (!e.metadata.isVideo) {
        if (ImGui::Button("Edit with Qwen...")) {
            SetFullscreen(g_hwnd, false);
            OpenPictureEditor(app, {e.path});
        }
        ImGui::SameLine();
        if (ImGui::Button("Pixel art it")) QuickPixelArt(app, {e.path});
        ImGui::SameLine();
        if (ImGui::Button("Use in the make bar")) {
            AddMakeRefs({e.path});
            SetFullscreen(g_hwnd, false);
            app.requestTab = 0;
            g_make.focus = true;
        }
    }
    if (!e.metadata.isVideo && ImGui::Button("Animate (video with sound)...")) {
        SetFullscreen(g_hwnd, false);
        OpenVideoPage(app, e.path, prompt);
    }
    if (!e.metadata.isVideo) ImGui::SameLine();
    if (!e.metadata.isVideo && ImGui::Button("Star in a video...")) {
        SetFullscreen(g_hwnd, false);
        StarInVideo(app, e.path);
    }
    if (!prompt.empty() && ImGui::Button("Send its prompt to ComfyUI"))
        SendToComfy(app, prompt, e.metadata.parsed.negativePrompt, e.path, WideToUtf8(fs::path(e.path).filename().wstring()));
    ImGui::Spacing();
    ImGui::TextDisabled("I: hide this   Left/Right: next   Esc: leave full screen");
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void DrawFullscreenViewer(AppState& app) {
    if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_I, false)) g_fsInfo = !g_fsInfo;
    const float infoW = g_fsInfo ? std::min(ui::Dp(380.0f), ImGui::GetContentRegionAvail().x * 0.4f) : 0.0f;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
    ImGui::BeginChild("fs_viewer", ImVec2(g_fsInfo ? -infoW : 0.0f, 0), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    DrawImageViewer(app);

    static double lastActivity = 0.0;
    ImGuiIO& io = ImGui::GetIO();
    if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f || ImGui::IsMouseClicked(0) || io.MouseWheel != 0.0f) {
        lastActivity = ImGui::GetTime();
    }
    if (ImGui::GetTime() - lastActivity < 2.5 && app.selectedIndex >= 0 && app.selectedIndex < static_cast<int>(app.entries.size())) {
        auto pos = std::find(app.visibleOrder.begin(), app.visibleOrder.end(), app.selectedIndex);
        int position = pos == app.visibleOrder.end() ? 0 : static_cast<int>(pos - app.visibleOrder.begin()) + 1;
        char text[256];
        snprintf(text, sizeof(text), "%s    %d / %d    %s",
                 WideToUtf8(fs::path(app.entries[app.selectedIndex].displayName).filename().wstring()).c_str(), position,
                 static_cast<int>(app.visibleOrder.size()),
                 app.slideshowActive ? "Slideshow on (Space stops)" : (g_fsInfo ? "Space: slideshow   Esc: exit" : "I: prompt and info   Space: slideshow   Esc: exit"));
        ImVec2 ts = ImGui::CalcTextSize(text);
        ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        ImVec2 p0(wp.x + (ws.x - ts.x) * 0.5f - ui::Dp(14.0f), wp.y + ws.y - ts.y - ui::Dp(34.0f));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p0, ImVec2(p0.x + ts.x + ui::Dp(28.0f), p0.y + ts.y + ui::Dp(14.0f)), IM_COL32(10, 12, 15, 185), ui::Dp(10.0f));
        dl->AddText(ImVec2(p0.x + ui::Dp(14.0f), p0.y + ui::Dp(7.0f)), IM_COL32(230, 233, 238, 255), text);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    if (g_fsInfo) {
        ImGui::SameLine(0.0f, 0.0f);
        DrawFullscreenInfo(app, infoW);
    }
}

// Global keyboard shortcuts. Arrow keys and single-letter shortcuts are
// skipped while a text field has focus or a dialog is open; Escape always works.
// ------------------------------------------------------------------- the sidebar: the pages, grouped, with what is running in each
int CountInLabel(const std::string& label) { // "Pixel art (2)###pixeltab" -> 2
    const size_t a = label.find('(');
    if (a == std::string::npos) return 0;
    return std::atoi(label.c_str() + a + 1);
}

// Small line icons for the pages, drawn (there is no icon font).
void DrawNavIcon(ImDrawList* dl, int icon, ImVec2 c, float s, ImU32 col) {
    const float h = s * 0.5f, t = std::max(1.3f, s * 0.09f);
    switch (icon) {
        case 0: { // gallery: four tiles
            const float g = s * 0.14f, w = (s - g) * 0.5f;
            for (int i = 0; i < 4; ++i) {
                const float x = c.x - h + (i % 2) * (w + g), y = c.y - h + (i / 2) * (w + g);
                dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + w), col, s * 0.08f);
            }
            break;
        }
        case 1: // vision library: a page of text
            dl->AddRect(ImVec2(c.x - h * 0.78f, c.y - h), ImVec2(c.x + h * 0.78f, c.y + h), col, s * 0.12f, 0, t);
            for (int i = 0; i < 3; ++i) dl->AddLine(ImVec2(c.x - h * 0.42f, c.y - h * 0.42f + i * h * 0.42f), ImVec2(c.x + h * 0.42f, c.y - h * 0.42f + i * h * 0.42f), col, t);
            break;
        case 2: { // workflows: connected nodes
            const ImVec2 a(c.x - h * 0.72f, c.y - h * 0.55f), b(c.x + h * 0.72f, c.y - h * 0.55f), d(c.x, c.y + h * 0.65f);
            dl->AddLine(a, d, col, t);
            dl->AddLine(b, d, col, t);
            for (const ImVec2& p : {a, b, d}) dl->AddCircleFilled(p, s * 0.17f, col, 12);
            break;
        }
        case 3: // vision board: two cards pinned on a board
            dl->AddRect(ImVec2(c.x - h, c.y - h * 0.8f), ImVec2(c.x + h, c.y + h * 0.8f), col, s * 0.1f, 0, t);
            dl->AddRectFilled(ImVec2(c.x - h * 0.62f, c.y - h * 0.42f), ImVec2(c.x - h * 0.06f, c.y + h * 0.18f), col, s * 0.05f);
            dl->AddRectFilled(ImVec2(c.x + h * 0.12f, c.y - h * 0.12f), ImVec2(c.x + h * 0.62f, c.y + h * 0.44f), col, s * 0.05f);
            break;
        case 4: { // comfyui: a sparkle
            const ImVec2 v[4] = {ImVec2(c.x, c.y - h), ImVec2(c.x + h * 0.22f, c.y), ImVec2(c.x, c.y + h), ImVec2(c.x - h * 0.22f, c.y)};
            const ImVec2 hz[4] = {ImVec2(c.x - h, c.y), ImVec2(c.x, c.y - h * 0.22f), ImVec2(c.x + h, c.y), ImVec2(c.x, c.y + h * 0.22f)};
            dl->AddConvexPolyFilled(v, 4, col);
            dl->AddConvexPolyFilled(hz, 4, col);
            break;
        }
        case 5: // characters: head and shoulders
            dl->AddCircleFilled(ImVec2(c.x, c.y - h * 0.38f), s * 0.22f, col, 16);
            dl->PathArcTo(ImVec2(c.x, c.y + h * 0.98f), h * 0.82f, 3.14159265f, 6.2831853f, 18);
            dl->PathFillConvex(col);
            break;
        case 6: // edits: a wand and a spark
            dl->AddLine(ImVec2(c.x - h * 0.85f, c.y + h * 0.85f), ImVec2(c.x + h * 0.25f, c.y - h * 0.25f), col, t * 1.6f);
            dl->AddLine(ImVec2(c.x + h * 0.62f, c.y - h * 0.95f), ImVec2(c.x + h * 0.62f, c.y - h * 0.25f), col, t);
            dl->AddLine(ImVec2(c.x + h * 0.27f, c.y - h * 0.6f), ImVec2(c.x + h * 0.97f, c.y - h * 0.6f), col, t);
            break;
        case 7: // upscale: a small frame growing to a big one
            dl->AddRect(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), col, s * 0.1f, 0, t);
            dl->AddRectFilled(ImVec2(c.x - h * 0.7f, c.y + h * 0.05f), ImVec2(c.x - h * 0.05f, c.y + h * 0.7f), col, s * 0.05f);
            dl->AddLine(ImVec2(c.x + h * 0.05f, c.y - h * 0.05f), ImVec2(c.x + h * 0.6f, c.y - h * 0.6f), col, t);
            dl->AddLine(ImVec2(c.x + h * 0.6f, c.y - h * 0.6f), ImVec2(c.x + h * 0.2f, c.y - h * 0.6f), col, t);
            dl->AddLine(ImVec2(c.x + h * 0.6f, c.y - h * 0.6f), ImVec2(c.x + h * 0.6f, c.y - h * 0.2f), col, t);
            break;
        case 8: // clef mode: a die (chance and choices)
            dl->AddRect(ImVec2(c.x - h * 0.9f, c.y - h * 0.9f), ImVec2(c.x + h * 0.9f, c.y + h * 0.9f), col, s * 0.2f, 0, t);
            for (int i = -1; i <= 1; ++i) dl->AddCircleFilled(ImVec2(c.x + i * h * 0.45f, c.y + i * h * 0.45f), s * 0.09f, col, 8);
            break;
        case 9: { // pixel art: a pixel heart
            static const char* kHeart[5] = {".x.x.", "xxxxx", "xxxxx", ".xxx.", "..x.."};
            const float p = s / 5.0f;
            for (int y = 0; y < 5; ++y) {
                for (int x = 0; x < 5; ++x) {
                    if (kHeart[y][x] != 'x') continue;
                    const ImVec2 a(c.x - h + x * p, c.y - h + y * p);
                    dl->AddRectFilled(a, ImVec2(a.x + p - 0.6f, a.y + p - 0.6f), col);
                }
            }
            break;
        }
        case 10: // copy: two sheets
            dl->AddRect(ImVec2(c.x - h * 0.85f, c.y - h * 0.45f), ImVec2(c.x + h * 0.35f, c.y + h * 0.95f), col, s * 0.08f, 0, t);
            dl->PathLineTo(ImVec2(c.x - h * 0.45f, c.y - h * 0.62f));
            dl->PathLineTo(ImVec2(c.x - h * 0.45f, c.y - h * 0.95f));
            dl->PathLineTo(ImVec2(c.x + h * 0.85f, c.y - h * 0.95f));
            dl->PathLineTo(ImVec2(c.x + h * 0.85f, c.y + h * 0.5f));
            dl->PathLineTo(ImVec2(c.x + h * 0.55f, c.y + h * 0.5f));
            dl->PathStroke(col, 0, t);
            break;
        case 11: // more: three dots
            for (int i = -1; i <= 1; ++i) dl->AddCircleFilled(ImVec2(c.x + i * h * 0.62f, c.y), s * 0.1f, col, 8);
            break;
        case 12: // a tick
            dl->PathLineTo(ImVec2(c.x - h * 0.7f, c.y + h * 0.02f));
            dl->PathLineTo(ImVec2(c.x - h * 0.18f, c.y + h * 0.55f));
            dl->PathLineTo(ImVec2(c.x + h * 0.75f, c.y - h * 0.55f));
            dl->PathStroke(col, 0, t * 1.5f);
            break;
        case 14: { // video: a film frame with sprocket holes
            dl->AddRect(ImVec2(c.x - h, c.y - h * 0.75f), ImVec2(c.x + h, c.y + h * 0.75f), col, s * 0.1f, 0, t);
            for (int k = 0; k < 3; ++k) {
                const float x = c.x - h * 0.55f + k * h * 0.55f;
                dl->AddRectFilled(ImVec2(x - s * 0.05f, c.y - h * 0.62f), ImVec2(x + s * 0.05f, c.y - h * 0.45f), col);
                dl->AddRectFilled(ImVec2(x - s * 0.05f, c.y + h * 0.45f), ImVec2(x + s * 0.05f, c.y + h * 0.62f), col);
            }
            dl->AddTriangleFilled(ImVec2(c.x - h * 0.22f, c.y - h * 0.28f), ImVec2(c.x - h * 0.22f, c.y + h * 0.28f), ImVec2(c.x + h * 0.3f, c.y), col);
            break;
        }
        case 15: { // music: a pair of eighth notes
            const float r = s * 0.11f;
            const ImVec2 a(c.x - h * 0.45f, c.y + h * 0.55f), b(c.x + h * 0.55f, c.y + h * 0.4f);
            dl->AddCircleFilled(a, r, col, 12);
            dl->AddCircleFilled(b, r, col, 12);
            dl->AddLine(ImVec2(a.x + r * 0.9f, a.y), ImVec2(a.x + r * 0.9f, c.y - h * 0.65f), col, t);
            dl->AddLine(ImVec2(b.x + r * 0.9f, b.y), ImVec2(b.x + r * 0.9f, c.y - h * 0.8f), col, t);
            dl->AddLine(ImVec2(a.x + r * 0.9f, c.y - h * 0.65f), ImVec2(b.x + r * 0.9f, c.y - h * 0.8f), col, t * 2.2f);
            break;
        }
        case 16: { // drama: two speech bubbles, talking over each other
            dl->AddRect(ImVec2(c.x - h * 0.85f, c.y - h * 0.7f), ImVec2(c.x + h * 0.25f, c.y + h * 0.05f), col, h * 0.18f, 0, t);
            dl->AddTriangleFilled(ImVec2(c.x - h * 0.6f, c.y + h * 0.02f), ImVec2(c.x - h * 0.3f, c.y + h * 0.02f), ImVec2(c.x - h * 0.7f, c.y + h * 0.3f), col);
            dl->AddRectFilled(ImVec2(c.x - h * 0.15f, c.y - h * 0.15f), ImVec2(c.x + h * 0.85f, c.y + h * 0.55f), col, h * 0.18f);
            dl->AddTriangleFilled(ImVec2(c.x + h * 0.3f, c.y + h * 0.5f), ImVec2(c.x + h * 0.6f, c.y + h * 0.5f), ImVec2(c.x + h * 0.7f, c.y + h * 0.85f), col);
            break;
        }
        case 17: { // LoRAs: three sliders
            for (int k = 0; k < 3; ++k) {
                const float y = c.y + (k - 1) * h * 0.62f, x = c.x + (k == 0 ? -0.3f : k == 1 ? 0.35f : -0.05f) * h;
                dl->AddLine(ImVec2(c.x - h * 0.85f, y), ImVec2(c.x + h * 0.85f, y), col, t);
                dl->AddCircleFilled(ImVec2(x, y), s * 0.12f, col, 12);
            }
            break;
        }
        case 13: // cancel: a cross
            dl->AddLine(ImVec2(c.x - h * 0.6f, c.y - h * 0.6f), ImVec2(c.x + h * 0.6f, c.y + h * 0.6f), col, t * 1.3f);
            dl->AddLine(ImVec2(c.x + h * 0.6f, c.y - h * 0.6f), ImVec2(c.x - h * 0.6f, c.y + h * 0.6f), col, t * 1.3f);
            break;
        default: break;
    }
}

bool g_paletteOpen = false, g_paletteFocus = false;

const char* TabShortcut(int tab) {
    static const char* kKeys[] = {"Ctrl+1", "Ctrl+2", "Ctrl+3", "Ctrl+4", "Ctrl+5", "Ctrl+6", "Ctrl+7", "Ctrl+8", "Ctrl+9", "Ctrl+0"};
    return tab >= 0 && tab < 10 ? kKeys[tab] : "";
}

int g_soundMode = 0; // the Sound page: 0 Music, 1 Drama

// The page sidebar (like the left rail of an app): Browse / Create / Story / Automate, a badge for what is running, search, and the model status at the
// bottom. Collapses to icons (Ctrl+\).
void DrawNavSidebar(AppState& app, float height) {
    struct NavItem {
        int tab;
        const char* label;
        int icon;
        int group;
        const char* hint;
    };
    static const NavItem kNav[] = {
        {0, "Gallery", 0, 0, "Your pictures and their prompts"},
        {1, "Vision library", 1, 0, "What the vision model wrote about your pictures"},
        {13, "LoRAs", 17, 0, "Every LoRA: what it does side by side, and mixes of them tested on a picture"},
        {4, "Generate", 4, 1, "Pictures, designs and videos from a prompt: Krea, Qwen, MOSS, your workflow, H3"},
        {6, "Edits", 6, 1, "Change pictures: Qwen edits, MOSS designs and box edits, text over a picture, upscale"},
        {10, "Video", 14, 1, "Videos with sound from a picture or a prompt (MiniMax H3)"},
        {11, "Sound", 15, 1, "Songs (MiniMax Music 3) and scenes with many voices (Fish Audio Drama 3)"},
        {9, "Pixel art", 9, 1, "Pixel art, sprite sets and animations"},
        {3, "Vision board", 3, 2, "Build a scene from characters, places and relations"},
        {5, "Characters", 5, 2, "Your characters and what they own"},
        {8, "Clef mode", 8, 2, "Endless scenarios with your characters"},
    };
    static const char* kGroups[] = {"BROWSE", "CREATE", "STORY", "AUTOMATE"};
    const bool collapsed = app.navCollapsed;
    const float w = collapsed ? ui::Dp(52.0f) : ui::Dp(184.0f);
    const bool light = ui::IsLight();
    ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    bg = light ? ImVec4(bg.x * 0.95f, bg.y * 0.95f, bg.z * 0.96f, 1.0f) : ImVec4(bg.x * 0.72f, bg.y * 0.72f, bg.z * 0.72f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, ui::Dp(10.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui::Dp(6.0f), ui::Dp(8.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui::Dp(4.0f), ui::Dp(2.0f)));
    ImGui::BeginChild("##nav", ImVec2(w, height), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text), dimCol = ImGui::GetColorU32(ui::kTextDim), accent = ImGui::GetColorU32(ui::kAccent),
                accentText = ImGui::GetColorU32(light ? ui::kAccentActive : ui::kAccentHover);
    const float rowH = ui::Dp(32.0f), icon = ui::Dp(16.0f);
    const float innerW = ImGui::GetContentRegionAvail().x;

    // collapse / expand
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##navtoggle", ImVec2(innerW, ui::Dp(24.0f)));
        const bool hov = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) {
            app.navCollapsed = !app.navCollapsed;
            app.SaveUiSettings();
        }
        if (hov) ImGui::SetTooltip(collapsed ? "Show the page names (Ctrl+\\)" : "Only icons (Ctrl+\\)");
        const float cx = collapsed ? p.x + innerW * 0.5f : p.x + innerW - ui::Dp(12.0f), cy = p.y + ui::Dp(12.0f), a = ui::Dp(4.0f);
        const ImU32 col = hov ? textCol : dimCol;
        const float dir = collapsed ? 1.0f : -1.0f; // >> or <<
        for (int k = 0; k < 2; ++k) {
            const float ox = cx + (k - 0.5f) * ui::Dp(6.0f) * dir;
            dl->AddLine(ImVec2(ox - a * 0.5f * dir, cy - a), ImVec2(ox + a * 0.5f * dir, cy), col, ui::Dp(1.6f));
            dl->AddLine(ImVec2(ox + a * 0.5f * dir, cy), ImVec2(ox - a * 0.5f * dir, cy + a), col, ui::Dp(1.6f));
        }
        if (!collapsed) dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p.x + ui::Dp(8.0f), cy - ImGui::GetFontSize() * 0.5f), dimCol, "Pages");
    }

    int group = -1;
    for (const NavItem& it : kNav) {
        if (it.group != group) {
            group = it.group;
            ImGui::Dummy(ImVec2(0, ui::Dp(group == 0 ? 2.0f : 8.0f)));
            if (collapsed) {
                if (group > 0) {
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    dl->AddLine(ImVec2(p.x + ui::Dp(10.0f), p.y), ImVec2(p.x + innerW - ui::Dp(10.0f), p.y), ImGui::GetColorU32(ImGuiCol_Separator));
                    ImGui::Dummy(ImVec2(0, ui::Dp(4.0f)));
                }
            } else {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui::Dp(8.0f));
                ImGui::PushFont(ui::BoldFont());
                ImGui::PushStyleColor(ImGuiCol_Text, ui::kTextDim);
                ImGui::SetWindowFontScale(0.82f);
                ImGui::TextUnformatted(kGroups[group]);
                ImGui::SetWindowFontScale(1.0f);
                ImGui::PopStyleColor();
                ImGui::PopFont();
            }
        }
        int badge = 0;
        switch (it.tab) {
            case 4: badge = CountInLabel(ComfyTabLabel(app)); break;
            case 6: badge = CountInLabel(EditsTabLabel(app)) + CountInLabel(UpscaleTabLabel(app)); break;
            case 8: badge = CountInLabel(ClefTabLabel(app)); break;
            case 9: badge = CountInLabel(PixelTabLabel(app)); break;
            case 10: badge = CountInLabel(VideoTabLabel(app)); break;
            default: break;
        }
        ImGui::PushID(it.tab);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##navitem", ImVec2(innerW, rowH));
        const bool hov = ImGui::IsItemHovered(), on = app.activeTab == it.tab;
        if (ImGui::IsItemClicked()) {
            app.activeTab = it.tab;
            app.requestTab = it.tab;
        }
        // drag a picture over a page: it opens (to drop it there)
        if (ImGui::BeginDragDropTarget()) {
            if (ImGui::GetDragDropPayload() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) app.requestTab = it.tab;
            ImGui::EndDragDropTarget();
        }
        const ImVec2 q(p.x + innerW, p.y + rowH);
        if (on) {
            dl->AddRectFilled(p, q, ui::WithAlpha(ui::kAccent, light ? 0.14f : 0.18f), ui::Dp(7.0f));
            dl->AddRectFilled(ImVec2(p.x, p.y + ui::Dp(7.0f)), ImVec2(p.x + ui::Dp(3.0f), q.y - ui::Dp(7.0f)), accent, ui::Dp(2.0f));
        } else if (hov) {
            dl->AddRectFilled(p, q, light ? IM_COL32(0, 0, 0, 14) : IM_COL32(255, 255, 255, 14), ui::Dp(7.0f));
        }
        const ImU32 col = on ? accentText : hov ? textCol : dimCol;
        const ImVec2 ic(collapsed ? p.x + innerW * 0.5f : p.x + ui::Dp(18.0f), p.y + rowH * 0.5f);
        DrawNavIcon(dl, it.icon, ic, icon, col);
        if (!collapsed) {
            dl->AddText(ImVec2(p.x + ui::Dp(36.0f), p.y + (rowH - ImGui::GetFontSize()) * 0.5f), on ? textCol : (hov ? textCol : ImGui::GetColorU32(ImVec4(ui::kTextDim.x + 0.12f, ui::kTextDim.y + 0.12f, ui::kTextDim.z + 0.12f, 1.0f))), it.label);
            if (badge > 0) {
                char b[8];
                snprintf(b, sizeof(b), "%d", badge);
                const ImVec2 ts = ImGui::CalcTextSize(b);
                const ImVec2 b1(q.x - ui::Dp(8.0f), p.y + rowH * 0.5f + ts.y * 0.5f + ui::Dp(2.0f)), b0(b1.x - ts.x - ui::Dp(12.0f), b1.y - ts.y - ui::Dp(4.0f));
                dl->AddRectFilled(b0, b1, accent, ui::Dp(9.0f));
                dl->AddText(ImVec2(b0.x + ui::Dp(6.0f), b0.y + ui::Dp(2.0f)), IM_COL32(255, 255, 255, 255), b);
            }
        } else if (badge > 0) {
            dl->AddCircleFilled(ImVec2(ic.x + icon * 0.62f, ic.y - icon * 0.55f), ui::Dp(4.5f), accent, 12);
        }
        if (hov && !ImGui::GetDragDropPayload()) {
            if (collapsed) ImGui::SetTooltip("%s  (%s)\n%s%s", it.label, TabShortcut(it.tab), it.hint, badge > 0 ? ("\n" + std::to_string(badge) + " running").c_str() : "");
            else if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s\n%s", it.hint, TabShortcut(it.tab));
        }
        ImGui::PopID();
    }

    // bottom: search, and whether ComfyUI and the vision model are up
    const float bottomH = collapsed ? rowH + ImGui::GetFrameHeight() * 2.0f + ui::Dp(14.0f) : rowH + ImGui::GetFrameHeight() + ui::Dp(10.0f);
    const float y = height - bottomH - ui::Dp(8.0f);
    if (ImGui::GetCursorPosY() < y) ImGui::SetCursorPosY(y);
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##navsearch", ImVec2(innerW, rowH));
        const bool hov = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) {
            g_paletteOpen = true;
            g_paletteFocus = true;
        }
        const ImVec2 q(p.x + innerW, p.y + rowH);
        dl->AddRect(p, q, hov ? ui::WithAlpha(ui::kAccent, 0.8f) : ImGui::GetColorU32(ImGuiCol_Border), ui::Dp(7.0f));
        const ImU32 col = hov ? textCol : dimCol;
        const ImVec2 c(collapsed ? p.x + innerW * 0.5f : p.x + ui::Dp(18.0f), p.y + rowH * 0.5f);
        dl->AddCircle(ImVec2(c.x - ui::Dp(1.5f), c.y - ui::Dp(1.5f)), ui::Dp(5.0f), col, 16, ui::Dp(1.6f)); // a magnifier
        dl->AddLine(ImVec2(c.x + ui::Dp(2.2f), c.y + ui::Dp(2.2f)), ImVec2(c.x + ui::Dp(6.0f), c.y + ui::Dp(6.0f)), col, ui::Dp(1.8f));
        if (!collapsed) {
            dl->AddText(ImVec2(p.x + ui::Dp(36.0f), p.y + (rowH - ImGui::GetFontSize()) * 0.5f), col, "Search");
            const char* k = "Ctrl+K";
            const ImVec2 ts = ImGui::CalcTextSize(k);
            dl->AddText(ImVec2(q.x - ts.x - ui::Dp(10.0f), p.y + (rowH - ts.y) * 0.5f), dimCol, k);
        }
        if (hov) ImGui::SetTooltip("Search pages, actions and recent folders (Ctrl+K)");
    }
    ImGui::Dummy(ImVec2(0, ui::Dp(4.0f)));
    if (!collapsed) {
        DrawComfyStatusDot(app);
        ImGui::SameLine();
        DrawVisionStatusDot(app);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("ComfyUI  Vision");
    } else {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (innerW - ui::Dp(30.0f)) * 0.5f);
        DrawComfyStatusDot(app);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (innerW - ui::Dp(30.0f)) * 0.5f);
        DrawVisionStatusDot(app);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
}

// ------------------------------------------------------------------- the command palette (Ctrl+K): every page and action, searched by typing
struct PaletteCmd {
    std::string group, name, keys;
    std::function<void()> run;
};
std::vector<std::string> g_paletteRecent; // what was run, newest first
int g_paletteSel = 0, g_paletteFrames = 0;
char g_paletteQuery[256] = {};

std::vector<PaletteCmd> PaletteCommands(AppState& app) {
    std::vector<PaletteCmd> c;
    auto add = [&](const char* g, const std::string& n, const char* k, std::function<void()> f) { c.push_back({g, n, k ? k : "", std::move(f)}); };
    static const char* kTabs[] = {"Gallery", "Vision library", "Workflows", "Vision board", "Generate", "Characters", "Edits", "Upscale", "Clef mode", "Pixel art", "Video", "Music", "Drama", "LoRAs"};
    for (int t = 0; t < 14; ++t) add("Go to", kTabs[t], TabShortcut(t), [&app, t] { app.requestTab = t; });
    add("File", "Open folder...", "Ctrl+Shift+O", [&app] { PickFolder(app); });
    add("File", "Open image...", "Ctrl+O", [&app] { PickFile(app); });
    if (!app.currentFolder.empty()) add("File", "Reload the folder", "F5", [&app] { app.ReloadFolder(); });
    for (const std::string& r : app.recentFolders) add("Recent", r, nullptr, [&app, r] {
        OpenRecentFolder(app, r);
        app.requestTab = 0;
    });
    if (!app.entries.empty()) {
        add("File", "Export clean copies (no metadata)...", nullptr, [&app] {
            app.cleanMessage.clear();
            app.showCleanExport = true;
        });
        add("File", "Rename by metadata...", nullptr, [&app] {
            app.renameMessage.clear();
            app.showRename = true;
        });
    }
    add("File", "Scrape a folder to CSV/JSON...", nullptr, [&app] {
        app.showScrapeDialog = true;
        app.scrapeResultMessage.clear();
    });
    if (app.selectedIndex >= 0 && app.selectedIndex < static_cast<int>(app.entries.size())) {
        const ImageEntry& e = app.entries[app.selectedIndex];
        const std::wstring p = e.path;
        const int i = app.selectedIndex;
        const std::string prompt = e.metadata.parsed.positivePrompt, neg = e.metadata.parsed.negativePrompt, label = WideToUtf8(fs::path(p).filename().wstring());
        if (!e.metadata.isVideo) {
            add("Picture", "Edit with Qwen...", nullptr, [&app, p] { OpenPictureEditor(app, {p}); });
            add("Picture", "Upscale...", nullptr, [&app, p] { OpenUpscaler(app, {p}); });
            add("Picture", "Design: meme, thumbnail, poster...", nullptr, [&app, p] { OpenDesigner(app, {p}); });
            add("Picture", "Change the words in it (Edits > Layout designs)...", nullptr, [&app, p] { OpenLayoutStudio(app, p); });
            add("Picture", "Make pixel art from it...", nullptr, [&app, p] { OpenPixelArt(app, {p}); });
            add("Picture", "Find pictures like this", nullptr, [&app, i] { EmbedFindLike(app, i); });
            add("Picture", "Add to the Clef cast", nullptr, [&app, p] { AddToClefCast(app, {p}); });
            add("Picture", "Video prompt (10 seconds)", nullptr, [&app, i] { RunVision(app, AppState::VisionMode::VideoScene, 10, i); });
        }
        add("Picture", "Describe with Vision", nullptr, [&app, i] {
            app.showVisionWindow = true;
            if (!app.visionModel.empty()) app.DescribeWithVision({i});
        });
        if (!prompt.empty()) {
            add("Picture", "Send its prompt to ComfyUI", nullptr, [&app, p, prompt, neg, label] { SendToComfy(app, prompt, neg, p, label); });
            add("Picture", "Edit the prompt, then send...", nullptr, [&app, p, prompt, neg, label] { OpenSendEditor(app, prompt, neg, p, label); });
            add("Picture", "Copy the prompt", nullptr, [&app, prompt] {
                ImGui::SetClipboardText(prompt.c_str());
                app.SetStatus("Copied the prompt.");
            });
        }
        add("Picture", "Use it in the make bar (as a reference)", nullptr, [&app, p] {
            AddMakeRefs({p});
            app.requestTab = 0;
            g_make.focus = true;
        });
        add("Picture", "Copy the picture", "Ctrl+C", [&app, i] { CopyPictures(app, {i}, false); });
        add("Picture", "Copy without metadata", "Ctrl+Shift+C", [&app, i] { CopyPictures(app, {i}, true); });
        add("Picture", "Show in Explorer", nullptr, [p] { ShowInExplorer(p); });
    }
    add("Tools", "Make a picture (the gallery's make bar)", "Ctrl+G", [&app] {
        app.requestTab = 0;
        g_make.focus = true;
    });
    add("View", "Fullscreen", "F11", [] { SetFullscreen(g_hwnd, !g_fullscreen); });
    if (!app.entries.empty()) add("View", "Slideshow", "Space", [&app] { ToggleSlideshow(app); });
    add("View", "Folder tree", "Ctrl+B", [&app] {
        app.showFolderPane = !app.showFolderPane;
        app.SaveUiSettings();
        app.requestTab = 0;
    });
    add("View", app.navCollapsed ? "Show the page names in the sidebar" : "Sidebar: only icons", "Ctrl+\\", [&app] {
        app.navCollapsed = !app.navCollapsed;
        app.SaveUiSettings();
    });
    add("View", app.lightTheme ? "Dark theme" : "Light theme", nullptr, [&app] {
        app.lightTheme = !app.lightTheme;
        app.SaveUiSettings();
        g_uiRebuild = true;
    });
    add("View", "Appearance (size, theme)...", nullptr, [&app] { app.showAppearance = true; });
    if (!app.entries.empty()) {
        add("Tools", "Find similar images...", nullptr, [&app] { app.showSimilar = true; });
        add("Tools", "Detect people in images...", nullptr, [&app] { app.showPeopleDetect = true; });
        if (!app.IsQualityRunning()) add("Tools", "Score picture quality and style (all shown)", nullptr, [&app] { app.StartQualityJob(app.visibleOrder); });
    }
    add("Tools", "Statistics...", nullptr, [&app] { app.showStats = true; });
    add("Tools", "Layout designs (Edits): covers, posters, thumbnails with exact words...", nullptr, [&app] { OpenLayoutStudio(app); });
    add("Tools", "Random tag generator...", nullptr, [&app] { app.showTagGeneratorDialog = true; });
    add("Tools", "NSFW sort and organize...", nullptr, [&app] { app.showNsfwDialog = true; });
    add("Tools", "Vision prompt window (Ollama)...", nullptr, [&app] { app.showVisionWindow = true; });
    if (!app.IsNsfwJobRunning()) add("Tools", "Add a folder to the collection...", nullptr, [&app] { AddFolderToCollection(app); });
    add("Help", "Keyboard shortcuts", nullptr, [] { g_showShortcuts = true; });
    add("Help", "Open the settings and cache folder", nullptr, [] { OpenDataFolder(); });
    add("Help", "About", nullptr, [] { g_showAbout = true; });
    return c;
}

std::string LowerAscii(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

void DrawCommandPalette(AppState& app) {
    if (!g_paletteOpen) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float w = std::min(ui::Dp(620.0f), vp->WorkSize.x * 0.8f);
    if (g_paletteFocus) {
        g_paletteQuery[0] = 0;
        g_paletteSel = 0;
        g_paletteFrames = 0;
        ImGui::SetNextWindowFocus();
    }
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + ui::Dp(64.0f)), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0), ImVec2(w, vp->WorkSize.y * 0.7f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ui::Dp(12.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui::Dp(10.0f), ui::Dp(10.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, ui::WithAlpha(ui::kAccent, 0.6f));
    std::function<void()> chosen;
    std::string chosenName;
    if (ImGui::Begin("##palette", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        if (g_paletteFocus) {
            ImGui::SetKeyboardFocusHere();
            g_paletteFocus = false;
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextWithHint("##pq", "Go to a page, run an action, open a recent folder...", g_paletteQuery, sizeof(g_paletteQuery))) g_paletteSel = 0;
        std::vector<PaletteCmd> cmds = PaletteCommands(app);
        // the words typed, all found in "group name"; names that start with them first; with nothing typed, what was run last first
        const std::string q = LowerAscii(g_paletteQuery);
        std::vector<std::string> words;
        {
            size_t at = 0;
            while (at < q.size()) {
                const size_t sp = q.find(' ', at);
                const std::string wd = q.substr(at, sp == std::string::npos ? std::string::npos : sp - at);
                if (!wd.empty()) words.push_back(wd);
                if (sp == std::string::npos) break;
                at = sp + 1;
            }
        }
        std::vector<std::pair<int, int>> shown; // (rank, index)
        for (int k = 0; k < static_cast<int>(cmds.size()); ++k) {
            const std::string hay = LowerAscii(cmds[k].group + " " + cmds[k].name);
            bool all = true;
            for (const std::string& wd : words) all = all && hay.find(wd) != std::string::npos;
            if (!all) continue;
            int rank = 3;
            const std::string name = LowerAscii(cmds[k].name);
            if (!words.empty() && name.rfind(words[0], 0) == 0) rank = 0;
            else if (!words.empty() && name.find(" " + words[0]) != std::string::npos) rank = 1;
            else if (words.empty()) {
                const auto it = std::find(g_paletteRecent.begin(), g_paletteRecent.end(), cmds[k].group + "|" + cmds[k].name);
                rank = it != g_paletteRecent.end() ? -100 + static_cast<int>(it - g_paletteRecent.begin()) : 3;
            }
            shown.push_back({rank, k});
        }
        std::stable_sort(shown.begin(), shown.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        if (shown.size() > 40) shown.resize(40);
        const int n = static_cast<int>(shown.size());
        bool moved = false;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            g_paletteSel = n ? (g_paletteSel + 1) % n : 0;
            moved = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            g_paletteSel = n ? (g_paletteSel - 1 + n) % n : 0;
            moved = true;
        }
        g_paletteSel = std::clamp(g_paletteSel, 0, std::max(0, n - 1));
        if ((ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) && n > 0) {
            chosen = cmds[shown[g_paletteSel].second].run;
            chosenName = cmds[shown[g_paletteSel].second].group + "|" + cmds[shown[g_paletteSel].second].name;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) g_paletteOpen = false;
        ImGui::Spacing();
        const float rowH = ui::Dp(28.0f);
        ImGui::BeginChild("##pl", ImVec2(0, std::min(n, 12) * (rowH + ImGui::GetStyle().ItemSpacing.y) + ui::Dp(2.0f)), ImGuiChildFlags_None);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (int r = 0; r < n; ++r) {
            const PaletteCmd& cm = cmds[shown[r].second];
            ImGui::PushID(r);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##row", r == g_paletteSel, ImGuiSelectableFlags_None, ImVec2(0, rowH))) {
                chosen = cm.run;
                chosenName = cm.group + "|" + cm.name;
            }
            if (ImGui::IsItemHovered() && ImGui::GetIO().MouseDelta.x != 0.0f) g_paletteSel = r;
            if (moved && r == g_paletteSel) ImGui::SetScrollHereY();
            const float ty = p.y + (rowH - ImGui::GetFontSize()) * 0.5f;
            dl->AddText(ImVec2(p.x + ui::Dp(8.0f), ty), ImGui::GetColorU32(ui::kTextDim), cm.group.c_str());
            dl->AddText(ImVec2(p.x + ui::Dp(84.0f), ty), ImGui::GetColorU32(ImGuiCol_Text), cm.name.c_str());
            if (!cm.keys.empty()) {
                const ImVec2 ts = ImGui::CalcTextSize(cm.keys.c_str());
                const float x = p.x + ImGui::GetContentRegionAvail().x - ts.x - ui::Dp(8.0f);
                dl->AddRectFilled(ImVec2(x - ui::Dp(5.0f), ty - ui::Dp(2.0f)), ImVec2(x + ts.x + ui::Dp(5.0f), ty + ts.y + ui::Dp(2.0f)), ImGui::GetColorU32(ImGuiCol_FrameBg), ui::Dp(4.0f));
                dl->AddText(ImVec2(x, ty), ImGui::GetColorU32(ui::kTextDim), cm.keys.c_str());
            }
            ImGui::PopID();
        }
        if (n == 0) ImGui::TextDisabled("Nothing matches \"%s\"", g_paletteQuery);
        ImGui::EndChild();
        ImGui::TextDisabled("Up/Down to choose, Enter to run, Esc to close");
        // clicked elsewhere: it closes
        if (++g_paletteFrames > 2 && !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) g_paletteOpen = false;
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    if (chosen) {
        g_paletteOpen = false;
        g_paletteRecent.erase(std::remove(g_paletteRecent.begin(), g_paletteRecent.end(), chosenName), g_paletteRecent.end());
        g_paletteRecent.insert(g_paletteRecent.begin(), chosenName);
        if (g_paletteRecent.size() > 8) g_paletteRecent.resize(8);
        chosen();
    }
}

void HandleShortcuts(AppState& app) {
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_B)) { // blur NSFW pictures, or not
        app.blurNsfw = !app.blurNsfw;
        app.nsfwRevealed.clear();
        app.SaveUiSettings();
        app.SetStatus(app.blurNsfw ? "NSFW pictures are blurred (Ctrl+Shift+B shows them again)" : "NSFW pictures are shown");
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_K) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_P)) {
        g_paletteOpen = !g_paletteOpen;
        g_paletteFocus = g_paletteOpen;
    }
    if (g_paletteOpen) return; // it takes the keys while open
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_G)) { // write in the gallery's make bar
        app.requestTab = 0;
        g_make.focus = true;
    }
    const bool typing = io.WantTextInput;
    const bool dialogOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    if (!typing && !dialogOpen && app.activeTab == 0) {
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) app.SelectNext();
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) app.SelectPrev();
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) app.SelectRelative(std::max(1, app.galleryColumns));
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) app.SelectRelative(-std::max(1, app.galleryColumns));
        if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) app.SelectFirst();
        if (ImGui::IsKeyPressed(ImGuiKey_End, false)) app.SelectLast();
        if (ImGui::IsKeyPressed(ImGuiKey_F, false) && !io.KeyCtrl) app.fitToWindow = true;
        if (ImGui::IsKeyPressed(ImGuiKey_Z, false) && !io.KeyCtrl) {
            app.zoom = 1.0f;
            app.fitToWindow = false;
            app.panX = app.panY = 0.0f;
        }
        // 1-5 rate the selection, 0 clears (like most photo managers).
        if (!io.KeyCtrl && !io.KeyAlt && app.selectedIndex >= 0 && app.selectedIndex < static_cast<int>(app.entries.size())) {
            for (int r = 0; r <= 5; ++r) {
                if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_0 + r), false) ||
                    ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_Keypad0 + r), false)) {
                    ApplyRating(app, ActionTargets(app, app.selectedIndex), r);
                }
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_C, false) && !io.KeyCtrl && app.multiSelected.size() == 2) {
            app.OpenCompare(*app.multiSelected.begin(), *app.multiSelected.rbegin());
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
            if (app.VideoOpen()) app.videoPlayer->Toggle(); // a playing video takes Space
            else ToggleSlideshow(app);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) app.ReloadFolder();
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O)) PickFile(app);
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_O)) PickFolder(app);
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_C)) {
            if (app.selectedIndex >= 0 && app.selectedIndex < static_cast<int>(app.entries.size())) {
                CopyPictures(app, ActionTargets(app, app.selectedIndex), io.KeyShift);
            }
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F)) app.requestFocusSearch = true;
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_B)) {
            app.showFolderPane = !app.showFolderPane;
            app.SaveUiSettings();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !app.showDeleteConfirm && app.selectedIndex >= 0 &&
            app.selectedIndex < static_cast<int>(app.entries.size())) {
            QueueDelete(app, app.selectedIndex);
        }
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F) && typing && !dialogOpen) app.requestFocusSearch = true;
    if (!typing && !dialogOpen) { // Ctrl+1 ... Ctrl+9, Ctrl+0: the pages
        for (int k = 0; k < 9; ++k) {
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | static_cast<ImGuiKey>(ImGuiKey_1 + k))) app.requestTab = k;
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_0)) app.requestTab = 9;
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Backslash)) {
            app.navCollapsed = !app.navCollapsed;
            app.SaveUiSettings();
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F11, false)) SetFullscreen(g_hwnd, !g_fullscreen);
    if (!dialogOpen && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        if (g_fullscreen) {
            SetFullscreen(g_hwnd, false);
            app.slideshowActive = false;
            return;
        }
        if (app.slideshowActive) {
            app.slideshowActive = false;
            app.SetStatus("Slideshow stopped.");
            return;
        }
        if (app.multiSelected.size() > 1) {
            app.multiSelected.clear();
            if (app.selectedIndex >= 0) app.multiSelected.insert(app.selectedIndex);
        }
        app.requestClearSearch = true;
    }
}

} // namespace

// ---------------------------------------------------------------------------
static float g_clear[4] = {0.078f, 0.086f, 0.102f, 1.0f};

// Title bar and background colour to match the theme (title colours need Windows 11).
static void ApplyWindowTheme(HWND hwnd, bool light) {
    BOOL dark = light ? FALSE : TRUE;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    COLORREF caption = light ? RGB(240, 242, 246) : RGB(20, 22, 26);
    DwmSetWindowAttribute(hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &caption, sizeof(caption));
    const float c[4] = {light ? 0.941f : 0.078f, light ? 0.949f : 0.086f, light ? 0.965f : 0.102f, 1.0f};
    memcpy(g_clear, c, sizeof(c));
}

static void CaptureWindowPlacement(AppState& app, HWND hwnd) {
    WINDOWPLACEMENT wp = {sizeof(wp)};
    if (!GetWindowPlacement(hwnd, &wp)) return;
    app.winX = wp.rcNormalPosition.left;
    app.winY = wp.rcNormalPosition.top;
    app.winW = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
    app.winH = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
    app.winMaximized = wp.showCmd == SW_SHOWMAXIMIZED;
    app.hasWinPlacement = app.winW > 200 && app.winH > 200;
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;

    switch (msg) {
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) return 0;
            g_ResizeWidth = LOWORD(lParam);
            g_ResizeHeight = HIWORD(lParam);
            return 0;
        case WM_ERASEBKGND:
            return 1; // Direct3D paints every pixel; avoids a white flash while resizing
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = static_cast<LONG>(ui::Dp(900.0f));
            mmi->ptMinTrackSize.y = static_cast<LONG>(ui::Dp(560.0f));
            return 0;
        }
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0; // disable ALT menu beep
            break;
        case WM_DPICHANGED: {
            // Move/resize to the size Windows suggests for the new monitor,
            // and rebuild fonts + style for the new scale between frames.
            g_dpiScale = static_cast<float>(HIWORD(wParam)) / 96.0f;
            g_uiRebuild = true;
            const RECT* suggested = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(hWnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_DROPFILES: {
            HDROP drop = (HDROP)wParam;
            wchar_t path[MAX_PATH];
            const bool ours = g_dragOutActive || GetTickCount64() - g_dragEndTick < 1000; // our own thumbnails dropped back on us
            if (!ours && DragQueryFileW(drop, 0, path, MAX_PATH) && g_app) {
                OpenPathFromExplorer(*g_app, path);
            }
            DragFinish(drop);
            return 0;
        }
        case WM_CLOSE:
            SetFullscreen(hWnd, false);
            if (g_app) CaptureWindowPlacement(*g_app, hWnd);
            // If Ollama was used, the main loop shows a "shutting down" frame,
            // unloads the model / stops Ollama, and only then exits.
            if (g_app && g_app->vision.UsedThisSession()) {
                g_closeRequested = true;
                return 0;
            }
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    // BGRA + video support lets the Media Foundation video engine decode straight into our textures.
    UINT createDeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
                                                featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
                                                &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (FAILED(hr)) {
        createDeviceFlags = 0;
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
                                           featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
                                           &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    }
    if (FAILED(hr)) {
        // No usable GPU/driver (VMs, remote sessions): fall back to the software rasterizer.
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags,
                                            featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
                                            &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    }
    if (FAILED(hr)) return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (backBuffer) {
        g_pd3dDevice->CreateRenderTargetView(backBuffer, nullptr, &g_mainRenderTargetView);
        backBuffer->Release();
    }
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

// Chooses where the window goes: where it was last time if that is still on a
// monitor, otherwise centred on the primary monitor.
static void PlaceWindow(HWND hwnd, AppState& app, float dpiScale) {
    if (app.hasWinPlacement) {
        RECT r = {app.winX, app.winY, app.winX + app.winW, app.winY + app.winH};
        if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
            SetWindowPos(hwnd, nullptr, r.left, r.top, app.winW, app.winH, SWP_NOZORDER | SWP_NOACTIVATE);
            return;
        }
    }
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    int workW = mi.rcWork.right - mi.rcWork.left, workH = mi.rcWork.bottom - mi.rcWork.top;
    int w = std::min(static_cast<int>(1360 * dpiScale), static_cast<int>(workW * 0.92f));
    int h = std::min(static_cast<int>(860 * dpiScale), static_cast<int>(workH * 0.92f));
    SetWindowPos(hwnd, nullptr, mi.rcWork.left + (workW - w) / 2, mi.rcWork.top + (workH - h) / 2, w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

// The app was called AI Image Metadata Viewer, and everything it keeps (settings, characters, boards, caches, the Clef cast...) is in
// %LOCALAPPDATA%\MetadataViewer. The first start as Try2Create moves that folder to %LOCALAPPDATA%\Try2Create. When something still has a file
// open there (a model server it started, the old program), the new name becomes a link to the old folder instead: nothing is lost, both work.
void MigrateAppData() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    const std::wstring oldDir = std::wstring(buf) + L"\\MetadataViewer", newDir = std::wstring(buf) + L"\\Try2Create";
    const bool haveOld = GetFileAttributesW(oldDir.c_str()) != INVALID_FILE_ATTRIBUTES, haveNew = GetFileAttributesW(newDir.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!haveOld || haveNew) return; // nothing to carry over, or done before
    std::string out;
    if (!MoveFileExW(oldDir.c_str(), newDir.c_str(), 0)) { // in use: the new name points to the old folder (a junction: no admin rights needed)
        RunCaptureHidden(L"cmd.exe /c mklink /J \"" + newDir + L"\" \"" + oldDir + L"\"", out, true);
        return;
    }
    // Moved: the old name points to the new folder. Saved paths inside the data (recent folders, characters' pictures, boards, the Clef cast,
    // the Vision library) name ...\MetadataViewer\..., and they keep working.
    RunCaptureHidden(L"cmd.exe /c mklink /J \"" + oldDir + L"\" \"" + newDir + L"\"", out, true);
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    MigrateAppData();     // it was AI Image Metadata Viewer: its data folder becomes Try2Create's (before anything reads it)
    InstallCrashLogger(); // a crash or a freeze leaves a report in %LOCALAPPDATA%\Try2Create\crash
    OleInitialize(nullptr); // COM (single-threaded apartment) plus the drag-and-drop machinery
    ImGui_ImplWin32_EnableDpiAwareness(); // must happen before any window is created

    HICON appIcon = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
    HICON appIconSmall = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, WndProc, 0, 0, hInstance, appIcon, LoadCursor(nullptr, IDC_ARROW),
                      nullptr, nullptr, L"Try2CreateWndClass", appIconSmall};
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Try2Create", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                               CW_USEDEFAULT, 1360, 860, nullptr, nullptr, wc.hInstance, nullptr);
    g_hwnd = hwnd;
    DragAcceptFiles(hwnd, TRUE);

    // Dark title bar (Windows 10 20H1+/11) in the app's background colour (Windows 11).
    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &darkMode, sizeof(darkMode));
    COLORREF captionColor = RGB(20, 22, 26);
    DwmSetWindowAttribute(hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &captionColor, sizeof(captionColor));

    if (!CreateDeviceD3D(hwnd)) {
        MessageBoxW(hwnd,
                    L"Direct3D 11 could not be initialized, so the app can't start.\n\n"
                    L"Please update your graphics driver and try again.",
                    L"Try2Create", MB_OK | MB_ICONERROR);
        CleanupDeviceD3D();
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    float dpiScale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // two widgets with the same label in one window: ImGui's red "Programmer error" box is for tests, not for the user (it showed on the Video page)
    io.ConfigDebugHighlightIdConflicts = GetEnvironmentVariableA("MV_UI_TEST", nullptr, 0) > 0;

    // Keep the ini file next to the executable so the tool stays portable;
    // fall back to the per-user data folder if that location isn't writable
    // (e.g. the app was copied into Program Files).
    static std::string iniPathUtf8;
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    fs::path exeDir = fs::path(exePath).parent_path();
    fs::path iniPath = exeDir / L"Try2Create.ini";
    {
        std::error_code mec; // the window layout of the app's old name comes along
        if (!fs::exists(iniPath, mec) && fs::exists(exeDir / L"MetadataViewer.ini", mec)) fs::copy_file(exeDir / L"MetadataViewer.ini", iniPath, mec);
    }
    {
        std::ofstream probe(iniPath, std::ios::app);
        if (!probe) {
            wchar_t local[MAX_PATH];
            DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
            if (len > 0 && len < MAX_PATH) {
                fs::path dir = fs::path(local) / L"Try2Create";
                std::error_code ec;
                fs::create_directories(dir, ec);
                iniPath = dir / L"Try2Create.ini";
            }
        }
    }
    iniPathUtf8 = WideToUtf8(iniPath.wstring());
    io.IniFilename = iniPathUtf8.c_str();

    // Settings (theme, UI scale) are needed before the first frame, so the
    // app state is built first. Its big thumbnail index loads in the background.
    AppState app(g_pd3dDevice);
    g_app = &app;
    g_dpiScale = dpiScale;
    ui::Init(dpiScale * app.uiScaleMul, app.lightTheme);
    ApplyWindowTheme(hwnd, app.lightTheme);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    PlaceWindow(hwnd, app, dpiScale);
    ShowWindow(hwnd, app.hasWinPlacement && app.winMaximized ? SW_SHOWMAXIMIZED : nCmdShow);
    UpdateWindow(hwnd);

    // Optional initial path from the command line (drag-onto-exe / "Open with").
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        if (argc > 1) OpenPathFromExplorer(app, argv[1]);
        LocalFree(argv);
    }

    // The window only repaints when something happens (input, background
    // work, a timer tick), so an idle viewer costs next to no CPU/GPU.
    int renderFrames = 4;
    bool wasBusy = false;
    bool done = false;
    int lastTabNoted = -1;
    while (!done) {
        CrashHeartbeat();
        if (app.activeTab != lastTabNoted) {
            lastTabNoted = app.activeTab;
            static const char* tabNames[] = {"Gallery", "Vision library", "Workflows", "Vision board", "Generate", "Characters", "Edits", "Upscale", "Clef mode", "Pixel art", "Video", "Music", "Drama", "LoRAs"};
            CrashNote(std::string("tab: ") + tabNames[std::clamp(app.activeTab, 0, 13)]);
        }
        MSG msg;
        bool gotMessage = false;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
            gotMessage = true;
        }
        if (done) break;
        if (gotMessage) renderFrames = std::max(renderFrames, 4);

        if (g_closeRequested) {
            // The backend cleanup runs on its own thread while this keeps drawing, so the
            // window stays responsive ("Not responding" made people force-close it).
            static std::thread shutdownThread;
            static std::atomic<bool> shutdownDone{false};
            static unsigned long long shutdownStart = 0;
            if (!shutdownThread.joinable()) {
                shutdownStart = GetTickCount64();
                shutdownThread = std::thread([&app]() {
                    ComfyStopOwned(); // a ComfyUI that this app started goes with it
                    ClefShutdown();
                    app.vision.Shutdown(app.visionStopOnExit);
                    shutdownDone = true;
                });
            }
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            const ImGuiViewport* vp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                                    ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::Begin("##shutdown", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove);
            ImGui::TextUnformatted(app.visionStopOnExit ? "Shutting down: stopping Ollama..." : "Shutting down: unloading models...");
            ImGui::End();
            {
            // A picture drag that reaches the edge of the window becomes a normal Windows file drag.
            const ImGuiPayload* pl = ImGui::GetDragDropPayload();
            if (pl && pl->IsDataType("MV_IMAGES") && g_dragOutPaths.empty()) {
                const ImVec2 mp = io.MousePos;
                if (mp.x < 3 || mp.y < 3 || mp.x > io.DisplaySize.x - 3 || mp.y > io.DisplaySize.y - 3) {
                    const int* idx = static_cast<const int*>(pl->Data);
                    const int n = pl->DataSize / static_cast<int>(sizeof(int));
                    for (int k = 0; k < n; ++k) {
                        if (idx[k] >= 0 && idx[k] < static_cast<int>(app.entries.size())) g_dragOutPaths.push_back(app.entries[idx[k]].path);
                    }
                    ImGui::ClearDragDrop();
                }
            }
        }
        ImGui::Render();
            const float* clear = g_clear;
            g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
            g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            g_pSwapChain->Present(1, 0);
            ReleaseFrameLeftovers();

            if (shutdownDone) {
                shutdownThread.join();
                break;
            }
            if (GetTickCount64() - shutdownStart > 15000) {
                // Ollama isn't answering: leave anyway (a server this app started dies with it).
                app.SaveUiSettings();
                TerminateProcess(GetCurrentProcess(), 0);
            }
            continue;
        }

        // Nothing to draw while minimized or fully covered.
        if (IsIconic(hwnd)) {
            ReleaseFrameLeftovers(); // (no frame is being drawn)
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 200, QS_ALLINPUT);
            continue;
        }
        static const bool testAlwaysDraw = UiTestNumber("draw") >= 0; // test captures while the screen is asleep (nothing counts as visible then)
        if (!testAlwaysDraw && g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            ReleaseFrameLeftovers();
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
            continue;
        }
        g_SwapChainOccluded = false;

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        { // MV_UI_TEST "ctrlclick:N": a plain click on the first picture, then Ctrl+clicks on the next N-1 (real ImGui input, not posted
          // messages); what is selected then goes to ctrlclick.txt in the data folder
            static const int n = UiTestNumber("ctrlclick");
            static int step = -40; // (time for the folder to load)
            if (n > 0 && step < n * 4 + 2 && !app.entries.empty()) {
                ImGuiIO& tio = ImGui::GetIO();
                const int k = step >= 0 ? step / 4 : -1, phase = step >= 0 ? step % 4 : -1;
                if (k >= 0 && k < n && g_cellCenters.count(k)) {
                    const bool ctrl = k > 0;
                    if (phase == 0) {
                        tio.AddMousePosEvent(g_cellCenters[k].x, g_cellCenters[k].y);
                        tio.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
                        tio.AddKeyEvent(ImGuiKey_LeftCtrl, ctrl);
                    } else if (phase == 1) {
                        tio.AddMouseButtonEvent(0, true);
                    } else if (phase == 2) {
                        tio.AddMouseButtonEvent(0, false);
                    }
                }
                if (k == n && phase == 1) {
                    tio.AddKeyEvent(ImGuiMod_Ctrl, false);
                    tio.AddKeyEvent(ImGuiKey_LeftCtrl, false);
                    wchar_t lad[1024] = {};
                    GetEnvironmentVariableW(L"LOCALAPPDATA", lad, 1024);
                    std::ofstream out(std::wstring(lad) + L"\\Try2Create\\ctrlclick.txt", std::ios::binary | std::ios::trunc);
                    out << "selected " << app.selectedIndex << " multi " << app.multiSelected.size() << ":";
                    for (int idx : app.multiSelected) out << " " << idx;
                    out << "\n";
                }
                ++step;
            }
        }
        app.PumpBackgroundResults();
        app.PumpScrapeResult();
        app.PumpNsfwResults();
        PruneBlurredCopies();
        app.PumpQualityResults();
        PumpClefGrades(app);
        PumpRelationImport(app);
        PumpClefMode(app);
        PumpPixelArt(app);
        PumpEmbed(app);
        ClefHousekeeping();
        app.PumpVisionResults();
        app.PumpSimilarResults();
        app.PumpPeopleResults();
        app.PumpFolderWatch();
        app.PumpDescribed();
        app.PumpVisionLibrary();
        PumpWorkflows(app);
        PumpComfy(app);
        WatchComfyJobs(app);
        PumpVideo(app);
        PumpComfyResults(app);
        PumpStory(app);
        PumpMusic(app);
        PumpDrama(app);
        PumpCharacterCreator(app);
        PumpCharacterResults(app);
        PumpCharacterOrbits(app);
        PumpCharacterStories(app);
        PumpRecastResults(app);
            app.PumpCleanExport();
        app.PumpRename();
        app.SaveRatingsIfDirty();
        if (app.VideoOpen() && app.activeTab != 0 && !g_fullscreen) app.videoPlayer->Pause(); // another tab is showing
        const bool videoMoving = app.UpdateVideo();

        const bool busy = videoMoving || app.IsLoading() || app.IsNsfwJobRunning() || app.IsScraping() || app.IsSimilarScanning() || app.IsPeopleScanning() || app.IsCleanExporting() || app.IsRenaming() || io.WantTextInput ||
                          (app.activeTab == 9 && PixelArtAnimating()) || ToastsShowing() || g_paletteOpen ||
                          (g_makingShown && app.activeTab == 0 && GetForegroundWindow() == g_hwnd) || // the spinner moves only while you look (not behind a game)
                          VideoPageAnimating(app) || (app.activeTab == 6 && EditsHomeAnimating()) || (app.activeTab == 11 && (MusicPlaying() || DramaPlaying())) || (app.activeTab == 5 && CharacterCreatorShown() && CharacterCreatorBusy()) ||
                          (OrbitViewAnimating() && GetForegroundWindow() == g_hwnd); // a turning character (not behind a game)
        if (wasBusy && !busy) renderFrames = std::max(renderFrames, 3); // show the final state
        wasBusy = busy;
        if (!busy && renderFrames <= 0) {
            // Sleep until input arrives; the timeout keeps timers/tooltips alive.
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 150, QS_ALLINPUT);
            MSG probe;
            if (!PeekMessage(&probe, nullptr, 0U, 0U, PM_NOREMOVE)) renderFrames = 1;
            continue;
        }
        if (renderFrames > 0) --renderFrames;

        if (g_uiRebuild) {
            g_uiRebuild = false;
            ImGui_ImplDX11_InvalidateDeviceObjects();
            ui::Init(g_dpiScale * app.uiScaleMul, app.lightTheme);
            ApplyWindowTheme(hwnd, app.lightTheme);
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        { // MV_UI_TEST "ctrl" / "shift": that key counts as held (posted test clicks cannot hold a real key)
            static const bool testCtrl = UiTestNumber("ctrl") >= 0, testShift = UiTestNumber("shift") >= 0;
            if (testCtrl) ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
            if (testShift) ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, true);
        }
        ImGui::NewFrame();
        { // MV_UI_TEST "inputlog": what the mouse does, frame by frame (to %LOCALAPPDATA%\Try2Create\input_log.txt)
            static const bool log = UiTestNumber("inputlog") >= 0;
            if (log) {
                static FILE* f = nullptr;
                if (!f) {
                    wchar_t buf[MAX_PATH] = {};
                    GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
                    _wfopen_s(&f, (std::wstring(buf) + L"\\Try2Create\\input_log.txt").c_str(), L"w");
                }
                const ImGuiIO& io = ImGui::GetIO();
                static ImVec2 lastPos(-1, -1);
                static bool lastDown = false;
                if (f && (io.MousePos.x != lastPos.x || io.MousePos.y != lastPos.y || io.MouseDown[0] != lastDown)) {
                    fprintf(f, "%.3f pos %.0f,%.0f down %d hoveredId %u ctrl %d\n", ImGui::GetTime(), io.MousePos.x, io.MousePos.y, io.MouseDown[0] ? 1 : 0,
                            ImGui::GetCurrentContext()->HoveredIdPreviousFrame, io.KeyCtrl ? 1 : 0);
                    fflush(f);
                    lastPos = io.MousePos;
                    lastDown = io.MouseDown[0];
                }
            }
        }

        if (app.slideshowActive && !app.entries.empty() && ImGui::GetTime() >= app.slideshowNextTime) {
            app.SelectNext();
            app.slideshowNextTime = ImGui::GetTime() + app.slideshowSeconds;
        }
        HandleShortcuts(app);

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGuiWindowFlags rootFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoScrollbar |
                                     ImGuiWindowFlags_NoScrollWithMouse;
        if (!g_fullscreen) rootFlags |= ImGuiWindowFlags_MenuBar;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, g_fullscreen ? ImVec2(0, 0) : ImVec2(ui::Dp(10.0f), ui::Dp(8.0f)));
        ImGui::Begin("##root", nullptr, rootFlags);
        ImGui::PopStyleVar(3);

        if (!g_fullscreen) {
            DrawMenuBar(app);
            DrawToolbar(app);
            ImGui::Spacing();
        }

        DrawScrapeDialog(app);
        DrawExportSelectedDialog(app);
        DrawDeleteConfirmDialog(app);
        DrawVisionConfirmDialog(app);
        DrawTagGeneratorDialog(app);
        DrawNsfwDialog(app);
        DrawVisionWindow(app);
        DrawShortcutsPopup();
        DrawAboutPopup();

        const ImGuiStyle& style = ImGui::GetStyle();
        const float statusH = g_fullscreen ? 0.0f : ImGui::GetFrameHeight() + style.ItemSpacing.y * 2.0f + 2.0f;
        const float bodyH = std::max(ui::Dp(100.0f), ImGui::GetContentRegionAvail().y - statusH);

        if (g_fullscreen) {
            DrawFullscreenViewer(app);
        } else {
            if (app.requestTab >= 0 && app.requestTab <= 13) app.activeTab = app.requestTab;
            app.requestTab = -1;
            if (app.activeTab == 7) app.activeTab = 6, SetEditsMode(3); // (Upscale is a mode of Edits now)
            if (app.activeTab == 12) app.activeTab = 11, g_soundMode = 1; // (Drama is a mode of Sound)
            DrawNavSidebar(app, bodyH);
            ImGui::SameLine(0, ui::Dp(8.0f));
            ImGui::BeginChild("##page", ImVec2(0, bodyH), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            const float tabBodyH = ImGui::GetContentRegionAvail().y;
            switch (app.activeTab) {
                case 0:
                    StarPickBar(app); // choosing who goes in a video (Star in a video): a floating bar over the gallery
                    if (app.currentFolder.empty()) {
                        DrawWelcome(app, tabBodyH);
                    } else {
                        // A resizable table gives free drag-to-resize splitters between the
                        // gallery/viewer/metadata panes, so the gallery can be widened to see
                        // more thumbnails per row when browsing a large folder.
                        ImGuiTableFlags layoutFlags = ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings;
                        const bool pane = app.showFolderPane;
                        if (ImGui::BeginTable(pane ? "main_layout_f" : "main_layout", pane ? 4 : 3, layoutFlags, ImVec2(0, tabBodyH))) {
                            if (pane) ImGui::TableSetupColumn("Folders", ImGuiTableColumnFlags_WidthStretch, 0.14f);
                            ImGui::TableSetupColumn("Gallery", ImGuiTableColumnFlags_WidthStretch, 0.25f);
                            ImGui::TableSetupColumn("Viewer", ImGuiTableColumnFlags_WidthStretch, 0.38f);
                            ImGui::TableSetupColumn("Metadata", ImGuiTableColumnFlags_WidthStretch, 0.37f);
                            ImGui::TableNextRow(0, tabBodyH);
                            int col = 0;
                            if (pane) {
                                ImGui::TableSetColumnIndex(col++);
                                DrawFolderPane(app, tabBodyH);
                            }
                            ImGui::TableSetColumnIndex(col++);
                            ImGui::BeginChild("left_pane", ImVec2(0, tabBodyH), true);
                            DrawThumbnailGrid(app);
                            ImGui::EndChild();

                            ImGui::TableSetColumnIndex(col++);
                            ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::ViewerBackground());
                            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui::Dp(0.0f), 0));
                            ImGui::BeginChild("center_pane", ImVec2(0, tabBodyH), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                            ImGui::PopStyleVar();
                            DrawImageViewer(app);
                            ImGui::EndChild();
                            ImGui::PopStyleColor();

                            ImGui::TableSetColumnIndex(col++);
                            ImGui::BeginChild("right_pane", ImVec2(0, tabBodyH), true);
                            DrawMetadataPanel(app);
                            ImGui::EndChild();
                            ImGui::EndTable();
                        }
                    }
                    break;
                case 1: DrawVisionLibrary(app, tabBodyH); break;
                case 2: DrawWorkflowTab(app, tabBodyH); break;
                case 3: DrawBoardTab(app, tabBodyH); break;
                case 4: DrawComfyTab(app, tabBodyH); break;
                case 5: DrawCharactersTab(app, tabBodyH); break;
                case 6: DrawEditsTab(app, tabBodyH); break;
                case 7: DrawUpscaleTab(app, tabBodyH); break;
                case 8: DrawClefTab(app, tabBodyH); break;
                case 9: DrawPixelTab(app, tabBodyH); break;
                case 10: DrawVideoTab(app, tabBodyH); break;
                case 13: DrawLoraPage(app, tabBodyH); break;
                case 11: { // Sound: songs, or scenes with many voices
                    const float y0 = ImGui::GetCursorPosY();
                    for (int m = 0; m < 2; ++m) {
                        if (m) ImGui::SameLine();
                        ImGui::PushStyleColor(ImGuiCol_Button, g_soundMode == m ? ui::WithAlpha(ui::kAccent, 0.85f) : ImGui::GetColorU32(ImGuiCol_Button));
                        if (ImGui::Button(m ? "Drama (many voices)" : "Music")) g_soundMode = m;
                        ImGui::PopStyleColor();
                    }
                    const float h = tabBodyH - (ImGui::GetCursorPosY() - y0);
                    if (g_soundMode == 1) DrawDramaTab(app, h);
                    else DrawMusicTab(app, h);
                    break;
                }
                default: app.activeTab = 0; break;
            }
            ImGui::EndChild();
        }

        if (!g_fullscreen) DrawStatusBar(app);
        ImGui::End();

        if (!g_fullscreen) {
            DrawStatsWindow(app);
            DrawSendEditor(app);
            DrawPictureEditor(app);
            DrawRecastWindow(app);
            DrawCompareWindow(app);
            DrawSimilarWindow(app);
            DrawPeopleDetectWindow(app);
            DrawCleanExportWindow(app);
            DrawRenameWindow(app);
            if (DrawAppearanceWindow(app)) g_uiRebuild = true;
            DrawPixelArtNotices(app);
            DesignerTestHook(app);
            { // MV_UI_TEST "fsinfo": the full-screen view with its info panel, inside the window (no real full screen over other windows)
                { // MV_UI_TEST "charcreate" (or "charmake"): the Characters tab's creator
                    static bool ccDone = false;
                    if (!ccDone && (UiTestNumber("charcreate") >= 0 || UiTestNumber("charmake") >= 0)) {
                        ccDone = true;
                        OpenCharacterCreator(app);
                    }
                    static bool clDone = false;
                    if (!clDone && UiTestNumber("charlist") >= 0) { // the saved characters view
                        clDone = true;
                        ShowCharacterCreator(false);
                        app.requestTab = 5;
                    }
                }
                // MV_UI_TEST "tab:N" opens page N (4 = ComfyUI, 10 = Video)
                static const int tabTest = UiTestNumber("tab");
                static bool tabDone = false;
                if (tabTest >= 0 && !tabDone) {
                    tabDone = true;
                    app.requestTab = tabTest;
                }
                // MV_UI_TEST "resultstab": the ComfyUI tab once something ComfyUI made is in (its viewer)
                static const bool resTest = UiTestNumber("resultstab") >= 0;
                static bool resDone = false;
                if (resTest && !resDone && app.comfySvc && !app.comfySvc->Results().empty()) {
                    resDone = true;
                    app.requestTab = 4;
                }
                static const bool fsTest = UiTestNumber("fsinfo") >= 0;
                static bool fsDone = false;
                // MV_UI_TEST "h3:N": picture N of the folder to MiniMax H3 Turbo, as the app's own button does
                static const int h3Test = UiTestNumber("h3");
                static bool h3Sent = false;
                if (h3Test >= 0 && !h3Sent && !app.IsLoading() && h3Test < static_cast<int>(app.entries.size())) {
                    h3Sent = true;
                    SendH3Turbo(app, app.entries[h3Test].path, "The person turns toward the camera, smiles and waves. Sound: a cheerful \"hello there!\" and soft city noise.", 5.0,
                                WideToUtf8(fs::path(app.entries[h3Test].path).filename().wstring()));
                }
                // MV_UI_TEST "videopage:N" opens the Video page with picture N; "h3runs:N" also queues Fast (N to N+1, both frames) and Best (from N), 480p, 3 s
                static const int vpTest = UiTestNumber("videopage"), runTest = UiTestNumber("h3runs");
                static bool vpDone = false;
                if ((vpTest >= 0 || runTest >= 0) && !vpDone && !app.IsLoading() && app.entries.size() > 2) {
                    vpDone = true;
                    const int n = std::max(vpTest, runTest);
                    // a still picture's prompt, as the gallery would hand over
                    OpenVideoPage(app, app.entries[n].path, "masterpiece, best quality, a young knight in polished silver plate armor and a dark red cape, standing in front of a helicopter, overcast sky, detailed");
                    if (UiTestNumber("videomake") >= 0) { // write the prompt for it and make it, at the page's settings
                        VideoOptions().res = 0;
                        VideoOptions().seconds = 5;
                        VideoOptions().quality = 0;
                        VideoPagePressMake();
                    }
                    if (runTest >= 0) {
                        H3Options o;
                        o.res = 0;
                        o.seconds = 3;
                        o.quality = 0;
                        o.last = app.entries[n + 1].path;
                        SendH3(app, app.entries[n].path, "The person walks forward and turns into the other person. Sound: footsteps, a whoosh.", o, "test fast first+last");
                        o.quality = 2;
                        o.last.clear();
                        SendH3(app, app.entries[n].path, "The knight raises his sword and shouts a battle cry. Sound: his voice, wind, distant thunder.", o, "test best");
                    }
                }
                if (fsTest && !fsDone && !app.entries.empty() && app.selectedIndex >= 0) {
                    fsDone = true;
                    g_fullscreen = true;
                    g_fsInfo = true;
                }
            }
            DrawDesigner(app);
            PumpLayoutStudio(app);
            DrawLoraSorter(app);
            DrawLoraBrowser(app);
            DrawCommandPalette(app);
        }
        DrawToasts(app);

        ImGui::Render();
        const float* clearColor = g_clear;
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = g_pSwapChain->Present(1, 0);
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
        ReleaseFrameLeftovers(); // what was let go during the frame (it may have drawn it): now it can go

        if (!g_dragOutPaths.empty()) {
            // The drag runs its own modal loop, so it starts between frames.
            std::vector<std::wstring> paths = std::move(g_dragOutPaths);
            g_dragOutPaths.clear();
            RunFileDragOut(hwnd, paths);
            g_dragEndTick = GetTickCount64();
            io.AddMouseButtonEvent(0, false); // the button was released inside the drag loop
            renderFrames = std::max(renderFrames, 4);
        }
    }

    app.SaveUiSettings();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    ReleaseFrameLeftovers();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    OleUninitialize();
    return 0;
}
