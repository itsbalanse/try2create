// The Upscale tab: make a picture bigger (the whole picture) or make its faces look better, for photos or anime / drawn pictures (they get other
// models and other wording). The work is done by ComfyUI: an upscale model from models/upscale_models when it has one that suits the picture,
// otherwise Lanczos, then optionally a low-denoise pass of MOSS-Image (25 steps, the Layout studio's Best) that redraws the detail (see BuildUpscale in comfy.cpp).
#include <algorithm>
#include <cmath>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <windows.h>
#include <shellapi.h>

#include "aesthetic_scorer.h"
#include "clipboard_image.h"
#include "image_texture.h"
#include "imgui.h"
#include "json.hpp"
#include "metadata.h"
#include "ui_extra.h"
#include "ui_theme.h"
#include "win_dialogs.h"
#include "board.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

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

struct UpRecord {
    int id = 0;
    int activity = 0; // while ComfyUI is still making it
    std::string label, error, state, message;
    std::wstring src, result;
    int mode = 1; // 1 whole picture, 2 faces
    bool anime = false;
    double scale = 2.0, detail = 0.3;
    int steps = 14, srcW = 0, srcH = 0;
    long long when = 0;
};

std::deque<UpRecord> g_ups; // oldest first
bool g_loaded = false;
int g_nextId = 1;
int g_sel = -1; // record id, -1 = showing the picture to upscale

struct Controls {
    std::vector<std::wstring> sources;
    int preview = 0;   // which source is shown when no result is selected
    int mode = 0;      // 0 whole picture, 1 faces
    int style = 0;     // 0 photo / realistic, 1 anime / drawn
    double scale = 2.0;
    double detail = 0.3;
    int steps = 25; // MOSS at its Best settings
    bool styleAuto = true;
};
Controls g_ctl;

fs::path UpDir() { return fs::path(BoardsDir()).parent_path() / L"upscale"; }

void SaveUps() {
    json j;
    j["next"] = g_nextId;
    j["items"] = json::array();
    for (const UpRecord& r : g_ups) {
        j["items"].push_back({{"id", r.id}, {"label", r.label}, {"error", r.error}, {"src", ToUtf8(r.src)}, {"result", ToUtf8(r.result)}, {"mode", r.mode}, {"anime", r.anime},
                              {"scale", r.scale}, {"detail", r.detail}, {"steps", r.steps}, {"srcW", r.srcW}, {"srcH", r.srcH}, {"when", r.when}});
    }
    std::error_code ec;
    fs::create_directories(UpDir(), ec);
    std::ofstream out(UpDir() / L"upscale.json", std::ios::binary | std::ios::trunc);
    if (out) out << j.dump(2, ' ', false, json::error_handler_t::replace);
}

void LoadUps() {
    if (g_loaded) return;
    g_loaded = true;
    std::ifstream f(UpDir() / L"upscale.json", std::ios::binary);
    if (!f) return;
    json j = json::parse(f, nullptr, false);
    if (!j.is_object() || !j.contains("items") || !j["items"].is_array()) return;
    g_nextId = j.value("next", 1);
    for (const json& e : j["items"]) {
        UpRecord r;
        r.id = e.value("id", 0);
        r.label = e.value("label", std::string());
        r.error = e.value("error", std::string());
        r.src = ToWide(e.value("src", std::string()));
        r.result = ToWide(e.value("result", std::string()));
        r.mode = e.value("mode", 1);
        r.anime = e.value("anime", false);
        r.scale = e.value("scale", 2.0);
        r.detail = e.value("detail", 0.3);
        r.steps = e.value("steps", 14);
        r.srcW = e.value("srcW", 0);
        r.srcH = e.value("srcH", 0);
        r.when = e.value("when", 0LL);
        if (r.result.empty() && r.error.empty()) r.error = "It did not finish (the program was closed)";
        g_nextId = std::max(g_nextId, r.id + 1);
        if (r.id > 0) g_ups.push_back(std::move(r));
    }
    if (!g_ups.empty()) g_sel = g_ups.back().id;
}

UpRecord* Find(int id) {
    for (UpRecord& r : g_ups) {
        if (r.id == id) return &r;
    }
    return nullptr;
}

// work of the redraw pass, in the units of the edit timing (so the estimate learns from the user's own runs)
int EstimateRes(int w, int h, double scale) {
    if (w <= 0 || h <= 0) return 1024;
    return static_cast<int>(std::lround(std::sqrt(static_cast<double>(w) * h) * scale));
}

void Sync(AppState& app) {
    bool any = false;
    for (const UpRecord& r : g_ups) any = any || r.activity > 0;
    if (!any || !app.comfySvc) return;
    const std::vector<ComfyActivity> acts = app.comfySvc->Activity();
    bool changed = false;
    for (UpRecord& r : g_ups) {
        if (r.activity <= 0) continue;
        const ComfyActivity* a = nullptr;
        for (const ComfyActivity& x : acts) {
            if (x.id == r.activity) a = &x;
        }
        if (!a) {
            r.activity = 0;
            r.error = "ComfyUI lost track of it";
            changed = true;
            continue;
        }
        r.message = a->message;
        switch (a->state) {
            case ComfyState::Waiting: r.state = "Waiting"; break;
            case ComfyState::Starting: r.state = "Starting ComfyUI"; break;
            case ComfyState::Sending: r.state = "Sending"; break;
            case ComfyState::Queued: r.state = "Queued"; break;
            case ComfyState::Running: r.state = "Working"; break;
            default: r.state.clear(); break;
        }
        if (a->state == ComfyState::Done) {
            if (a->runningMs > 0 && a->finishedMs > a->runningMs && r.detail > 0.01) {
                RecordEditRun(r.steps, EstimateRes(r.srcW, r.srcH, r.scale), 1, 0, 1, (a->finishedMs - a->runningMs) / 1000.0);
            }
            for (const ComfyResult& f : a->files) {
                if (!f.localPath.empty() && r.result.empty()) r.result = f.localPath;
            }
            if (r.result.empty()) r.error = "ComfyUI made no picture";
            r.activity = 0;
            changed = true;
        } else if (a->state == ComfyState::Failed) {
            r.error = a->message;
            r.activity = 0;
            changed = true;
        }
    }
    if (changed) SaveUps();
}

void Thumb(AppState& app, const std::wstring& path, ImVec2 size) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const AppState::LibThumb t = app.LibraryThumb(path);
    if (t.srv && t.w > 0 && t.h > 0) {
        ImVec2 uv0(0, 0), uv1(1, 1);
        if (t.w > t.h) { const float m = (1.0f - float(t.h) / t.w) * 0.5f; uv0.x = m; uv1.x = 1 - m; }
        else if (t.h > t.w) { const float m = (1.0f - float(t.w) / t.h) * 0.5f; uv0.y = m; uv1.y = 1 - m; }
        dl->AddImageRounded((ImTextureID)(intptr_t)t.srv, p, ImVec2(p.x + size.x, p.y + size.y), uv0, uv1, IM_COL32_WHITE, ui::Dp(5.0f));
    } else {
        dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), ui::ThumbPlaceholder(), ui::Dp(5.0f));
    }
}

// drawn pictures (anime, cartoon, pixel art, sketch) get the anime models and wording
bool LooksDrawn(AppState& app, const std::wstring& path) {
    for (const ImageEntry& e : app.entries) {
        if (e.styleKind >= 0 && _wcsicmp(e.path.c_str(), path.c_str()) == 0) return e.styleKind == 1 || e.styleKind == 2 || e.styleKind == 5 || e.styleKind == 6;
    }
    return false;
}

bool KnownStyle(AppState& app, const std::wstring& path) {
    for (const ImageEntry& e : app.entries) {
        if (e.styleKind >= 0 && _wcsicmp(e.path.c_str(), path.c_str()) == 0) return true;
    }
    return false;
}

void AddSource(AppState& app, const std::wstring& path) {
    if (path.empty()) return;
    for (const std::wstring& s : g_ctl.sources) {
        if (_wcsicmp(s.c_str(), path.c_str()) == 0) return;
    }
    if (g_ctl.sources.empty() && KnownStyle(app, path)) g_ctl.style = LooksDrawn(app, path) ? 1 : 0; // from the style score, when the picture has one
    g_ctl.sources.push_back(path);
    g_sel = -1;
    g_ctl.preview = static_cast<int>(g_ctl.sources.size()) - 1;
}

struct Preset {
    const char* name;
    double scale, detail;
    const char* hint;
};

void ApplyPreset(const Preset& p) {
    g_ctl.scale = p.scale;
    g_ctl.detail = g_ctl.style == 1 ? std::min(p.detail, 0.4) : p.detail; // drawn pictures change faster: a little less
}

// ------------------------------------------------------------------- the viewer: before / after
struct View {
    LoadedImage before, after;
    std::wstring beforePath, afterPath;
    float zoom = 1.0f;
    ImVec2 pan{0, 0};
    float split = 0.5f;
    int shown = 2; // 0 before, 1 after, 2 split
};
View g_view;

void Load(AppState& app, LoadedImage& img, std::wstring& cur, const std::wstring& path) {
    if (cur == path) return;
    ReleaseImage(img);
    img = LoadedImage{};
    cur = path;
    std::string err;
    if (!path.empty()) LoadImageTexture(app.Device(), path, img, &err);
}

void DrawViewer(AppState& app, UpRecord* rec, float height) {
    View& v = g_view;
    const std::wstring beforePath = rec ? rec->src : (g_ctl.preview >= 0 && g_ctl.preview < static_cast<int>(g_ctl.sources.size()) ? g_ctl.sources[g_ctl.preview] : std::wstring());
    const std::wstring afterPath = rec ? rec->result : std::wstring();
    const bool newPair = v.beforePath != beforePath || v.afterPath != afterPath;
    Load(app, v.before, v.beforePath, beforePath);
    Load(app, v.after, v.afterPath, afterPath);
    if (newPair) {
        v.zoom = 1.0f;
        v.pan = ImVec2(0, 0);
    }
    const bool both = v.before.srv && v.after.srv;
    if (rec && !afterPath.empty()) {
        if (ImGui::RadioButton("Before", v.shown == 0)) v.shown = 0;
        ImGui::SameLine();
        if (ImGui::RadioButton("After", v.shown == 1)) v.shown = 1;
        ImGui::SameLine();
        if (ImGui::RadioButton("Compare", v.shown == 2)) v.shown = 2;
        ImGui::SameLine();
        if (v.after.width > 0) ImGui::TextDisabled("%dx%d -> %dx%d", v.before.width, v.before.height, v.after.width, v.after.height);
        if (ImGui::SmallButton("Open in the gallery")) {
            app.OpenFile(afterPath);
            app.requestTab = 0;
        }
        ui::SameLineIfFits("Show in Explorer");
        if (ImGui::SmallButton("Show in Explorer")) {
            const std::wstring args = L"/select,\"" + afterPath + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        }
        ui::SameLineIfFits("Copy image");
        if (ImGui::SmallButton("Copy image")) {
            std::string msg;
            CopyImagesToClipboard(GetActiveWindow(), {afterPath}, false, &msg);
            app.SetStatus(msg);
        }
        ui::SameLineIfFits("Upscale this result again");
        if (ImGui::SmallButton("Upscale this result again")) {
            g_ctl.sources.clear();
            AddSource(app, afterPath);
        }
        ui::SameLineIfFits("Edit with Qwen...");
        if (ImGui::SmallButton("Edit with Qwen...")) OpenPictureEditor(app, {afterPath});
        ui::SameLineIfFits("Pixel art...");
        if (ImGui::SmallButton("Design...")) OpenDesigner(app, {afterPath});
        ImGui::SameLine();
        PixelArtButton(app, {afterPath}, "##pxquickup");
    } else if (v.before.srv) {
        ImGui::TextDisabled("%dx%d", v.before.width, v.before.height);
    }
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 region(std::max(40.0f, avail.x), std::max(ui::Dp(120.0f), height - ImGui::GetCursorPosY() - ui::Dp(rec && !afterPath.empty() && v.shown == 2 ? 40.0f : 8.0f)));
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##upimg", region);
    const bool hovered = ImGui::IsItemHovered();
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, ImVec2(origin.x + region.x, origin.y + region.y), IM_COL32(10, 12, 16, 255), ui::Dp(6.0f));
    const LoadedImage* base = v.after.srv ? &v.after : (v.before.srv ? &v.before : nullptr);
    if (base && base->width > 0 && base->height > 0) {
        const float fit = std::min(region.x / base->width, region.y / base->height);
        if (hovered && io.MouseWheel != 0.0f) {
            const float k = std::clamp(std::pow(1.18f, io.MouseWheel) * v.zoom, 0.2f, 30.0f) / v.zoom;
            const ImVec2 c(origin.x + region.x * 0.5f, origin.y + region.y * 0.5f);
            v.pan.x = (io.MousePos.x - c.x) - (io.MousePos.x - c.x - v.pan.x) * k;
            v.pan.y = (io.MousePos.y - c.y) - (io.MousePos.y - c.y - v.pan.y) * k;
            v.zoom *= k;
        }
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
            v.pan.x += io.MouseDelta.x;
            v.pan.y += io.MouseDelta.y;
        }
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            v.zoom = 1.0f;
            v.pan = ImVec2(0, 0);
        }
        const float scale = fit * v.zoom;
        const ImVec2 dim(base->width * scale, base->height * scale); // both pictures fill the same frame (the result is the same picture, bigger)
        const ImVec2 c(origin.x + region.x * 0.5f + v.pan.x, origin.y + region.y * 0.5f + v.pan.y);
        const ImVec2 a(c.x - dim.x * 0.5f, c.y - dim.y * 0.5f), b(c.x + dim.x * 0.5f, c.y + dim.y * 0.5f);
        dl->PushClipRect(origin, ImVec2(origin.x + region.x, origin.y + region.y), true);
        const bool compare = both && v.shown == 2;
        if (compare) {
            const float sx = a.x + dim.x * v.split;
            dl->PushClipRect(ImVec2(std::max(origin.x, a.x), origin.y), ImVec2(std::min(sx, origin.x + region.x), origin.y + region.y), true);
            dl->AddImage((ImTextureID)(intptr_t)v.before.srv, a, b);
            dl->PopClipRect();
            dl->PushClipRect(ImVec2(std::max(origin.x, sx), origin.y), ImVec2(std::min(b.x, origin.x + region.x), origin.y + region.y), true);
            dl->AddImage((ImTextureID)(intptr_t)v.after.srv, a, b);
            dl->PopClipRect();
            dl->AddLine(ImVec2(sx, std::max(origin.y, a.y)), ImVec2(sx, std::min(origin.y + region.y, b.y)), IM_COL32(255, 255, 255, 230), 2.0f);
            dl->AddText(ImVec2(std::max(origin.x, a.x) + 8, origin.y + 6), IM_COL32(255, 255, 255, 220), "Before");
            const ImVec2 ts = ImGui::CalcTextSize("After");
            dl->AddText(ImVec2(std::min(origin.x + region.x, b.x) - ts.x - 8, origin.y + 6), IM_COL32(255, 255, 255, 220), "After");
        } else {
            const LoadedImage* one = (v.shown == 0 && v.before.srv) || !v.after.srv ? &v.before : &v.after;
            if (one->srv) dl->AddImage((ImTextureID)(intptr_t)one->srv, a, b);
        }
        dl->PopClipRect();
    } else {
        const std::string text = rec ? (rec->activity > 0 ? (rec->state.empty() ? std::string("Working...") : rec->state + ": " + rec->message) : !rec->error.empty() ? "Failed: " + rec->error : "No picture")
                                     : "Add a picture (drop one here, or use the buttons on the right)";
        const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
        dl->AddText(ImVec2(origin.x + std::max(8.0f, (region.x - ts.x) * 0.5f), origin.y + (region.y - ts.y) * 0.5f), rec && !rec->error.empty() ? IM_COL32(255, 120, 110, 255) : IM_COL32(220, 220, 230, 230),
                    text.c_str());
    }
    if (rec && rec->activity > 0 && base) { // still working on a result that is not there yet: say what is happening over the picture
        const std::string text = (rec->state.empty() ? std::string("Working...") : rec->state) + (rec->message.empty() ? std::string() : ": " + rec->message);
        const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
        dl->AddRectFilled(ImVec2(origin.x + 8, origin.y + region.y - ts.y - 16), ImVec2(origin.x + 20 + ts.x, origin.y + region.y - 6), IM_COL32(0, 0, 0, 170), ui::Dp(5.0f));
        dl->AddText(ImVec2(origin.x + 14, origin.y + region.y - ts.y - 11), IM_COL32(255, 255, 255, 235), text.c_str());
    }
    // pictures dragged onto the viewer become the picture to upscale
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("MV_LIBITEM")) {
            g_ctl.sources.clear();
            AddSource(app, std::wstring(static_cast<const wchar_t*>(pl->Data)));
        } else if (const ImGuiPayload* pg = ImGui::AcceptDragDropPayload("MV_IMAGES")) {
            const int* idx = static_cast<const int*>(pg->Data);
            const int n = static_cast<int>(pg->DataSize / sizeof(int));
            g_ctl.sources.clear();
            for (int i = 0; i < n; ++i) {
                if (idx[i] >= 0 && idx[i] < static_cast<int>(app.entries.size())) AddSource(app, app.entries[idx[i]].path);
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (rec && both && v.shown == 2) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##upsplit", &v.split, 0.0f, 1.0f, "Before | After");
    }
}

std::string SizeText(double w, double h) { return std::to_string(static_cast<int>(std::lround(w))) + " x " + std::to_string(static_cast<int>(std::lround(h))); }

} // namespace

// ---------------------------------------------------------------------------------------------------------------
void OpenUpscaler(AppState& app, const std::vector<std::wstring>& paths) {
    g_ctl.sources.clear();
    g_ctl.preview = 0;
    for (const std::wstring& p : paths) AddSource(app, p);
    g_sel = -1;
    app.requestTab = 7;
}

std::string UpscaleTabLabel(AppState& app) {
    LoadUps();
    Sync(app);
    int n = 0;
    for (const UpRecord& r : g_ups) n += r.activity > 0 ? 1 : 0;
    return (n > 0 ? "Upscale (" + std::to_string(n) + ")" : std::string("Upscale")) + "###upscaletab";
}

void DrawUpscaleTab(AppState& app, float height) {
    LoadUps();
    Sync(app);
    if (!ImGui::BeginTable("up_layout", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings, ImVec2(0, height))) return;
    ImGui::TableSetupColumn("List", ImGuiTableColumnFlags_WidthStretch, 0.16f);
    ImGui::TableSetupColumn("Picture", ImGuiTableColumnFlags_WidthStretch, 0.54f);
    ImGui::TableSetupColumn("Controls", ImGuiTableColumnFlags_WidthStretch, 0.30f);
    ImGui::TableNextRow(0, height);

    // ---- what was upscaled, newest first
    ImGui::TableSetColumnIndex(0);
    ImGui::BeginChild("up_list", ImVec2(0, height), true);
    if (const int cleanup = DrawListCleanup("upclean", !g_ups.empty())) {
        const size_t before = g_ups.size();
        g_ups.erase(std::remove_if(g_ups.begin(), g_ups.end(), [&](const UpRecord& r) {
                        if (r.activity > 0) return false; // (still being made)
                        return cleanup == 2 || !FileStillThere(r.result);
                    }),
                    g_ups.end());
        if (!Find(g_sel)) g_sel = -1;
        SaveUps();
        const size_t n = before - g_ups.size();
        app.SetStatus(n == 0 ? std::string("Every upscale on the list is still there") : "Took " + std::to_string(n) + (n == 1 ? " upscale" : " upscales") + " off the list (no files deleted)");
    }
    if (g_ups.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Pictures you upscale show up here, and are kept between sessions.");
        ImGui::PopTextWrapPos();
    }
    int removeId = -1;
    for (size_t k = g_ups.size(); k-- > 0;) {
        UpRecord& r = g_ups[k];
        ImGui::PushID(r.id);
        const float row = ui::Dp(64.0f);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        if (ImGui::InvisibleButton("##row", ImVec2(w, row))) g_sel = r.id;
        const bool hov = ImGui::IsItemHovered();
        if (ImGui::BeginPopupContextItem("##rowmenu")) {
            if (ImGui::MenuItem("Remove from this list")) removeId = r.id; // the pictures themselves stay where they are
            ImGui::EndPopup();
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const bool sel = r.id == g_sel;
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + row), sel ? ui::WithAlpha(ui::kAccent, 0.25f) : (hov ? IM_COL32(255, 255, 255, 18) : IM_COL32(255, 255, 255, 8)), ui::Dp(6.0f));
        if (sel) dl->AddRect(pos, ImVec2(pos.x + w, pos.y + row), ui::WithAlpha(ui::kAccent, 0.9f), ui::Dp(6.0f), 0, ui::Dp(1.5f));
        const ImVec2 keep = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(pos.x + 4, pos.y + 4));
        Thumb(app, r.result.empty() ? r.src : r.result, ImVec2(row - 8, row - 8));
        ImGui::SetCursorScreenPos(keep);
        dl->PushClipRect(ImVec2(pos.x + row, pos.y), ImVec2(pos.x + w - 4, pos.y + row), true);
        dl->AddText(ImVec2(pos.x + row + 2, pos.y + 6), ImGui::GetColorU32(ImGuiCol_Text), r.label.c_str());
        char sub[96];
        snprintf(sub, sizeof(sub), "%s %.3gx%s", r.mode == 2 ? "Faces" : "Whole", r.scale, r.anime ? ", anime" : "");
        const std::string line2 = r.activity > 0 ? (r.state.empty() ? "Working..." : r.state + "...") : !r.error.empty() ? "Failed" : std::string(sub);
        dl->AddText(ImVec2(pos.x + row + 2, pos.y + 6 + ImGui::GetFontSize() + 3), r.error.empty() ? ImGui::GetColorU32(ui::kTextDim) : ImGui::GetColorU32(ui::kBad), line2.c_str());
        dl->PopClipRect();
        ImGui::PopID();
    }
    if (removeId >= 0) {
        g_ups.erase(std::remove_if(g_ups.begin(), g_ups.end(), [&](const UpRecord& r) { return r.id == removeId; }), g_ups.end());
        if (g_sel == removeId) g_sel = -1;
        SaveUps();
    }
    ImGui::EndChild();

    UpRecord* rec = Find(g_sel);

    // ---- the picture
    ImGui::TableSetColumnIndex(1);
    ImGui::BeginChild("up_view", ImVec2(0, height), true, ImGuiWindowFlags_NoScrollbar);
    ImGui::PushFont(ui::BoldFont());
    ImGui::TextUnformatted(rec ? rec->label.c_str() : "Picture to upscale");
    ImGui::PopFont();
    DrawViewer(app, rec, height);
    ImGui::EndChild();

    // ---- the controls
    ImGui::TableSetColumnIndex(2);
    ImGui::BeginChild("up_ctl", ImVec2(0, height), true);
    ImGui::BeginChild("##upctlbody", ImVec2(0, -(ImGui::GetFrameHeightWithSpacing() + ui::Dp(30.0f))), false);

    ImGui::TextUnformatted("1. Pictures");
    {
        int removeAt = -1;
        const float th = ui::Dp(52.0f);
        for (size_t i = 0; i < g_ctl.sources.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##src", ImVec2(th, th))) {
                g_ctl.preview = static_cast<int>(i);
                g_sel = -1;
            }
            ImGui::SetCursorScreenPos(p);
            Thumb(app, g_ctl.sources[i], ImVec2(th, th));
            if (g_sel < 0 && static_cast<int>(i) == g_ctl.preview) ImGui::GetWindowDrawList()->AddRect(p, ImVec2(p.x + th, p.y + th), ui::WithAlpha(ui::kAccent, 1.0f), ui::Dp(5.0f), 0, ui::Dp(2.0f));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nClick to look at it, right click to remove it", ToUtf8(fs::path(g_ctl.sources[i]).filename().wstring()).c_str());
            if (ImGui::BeginPopupContextItem("##srcmenu")) {
                if (ImGui::MenuItem("Remove")) removeAt = static_cast<int>(i);
                ImGui::EndPopup();
            }
            ImGui::PopID();
            if (i + 1 < g_ctl.sources.size() && ImGui::GetContentRegionAvail().x > th * 2.0f) ImGui::SameLine();
        }
        if (removeAt >= 0) {
            g_ctl.sources.erase(g_ctl.sources.begin() + removeAt);
            g_ctl.preview = std::clamp(g_ctl.preview, 0, std::max(0, static_cast<int>(g_ctl.sources.size()) - 1));
        }
        if (g_ctl.sources.empty()) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("None yet. Drag pictures from the gallery or the Vision library onto the picture, or:");
            ImGui::PopTextWrapPos();
        }
        if (ImGui::SmallButton("Add a file...")) AddSource(app, OpenImageFileDialog(GetActiveWindow()));
        ImGui::SameLine();
        const bool haveSel = app.selectedIndex >= 0 && app.selectedIndex < static_cast<int>(app.entries.size());
        ImGui::BeginDisabled(!haveSel);
        if (ImGui::SmallButton("The gallery's selected picture")) AddSource(app, app.entries[app.selectedIndex].path);
        ImGui::EndDisabled();
        if (!g_ctl.sources.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear")) g_ctl.sources.clear();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("2. What to improve");
    {
        const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        for (int m = 0; m < 2; ++m) {
            if (m) ImGui::SameLine();
            const bool on = g_ctl.mode == m;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ui::kAccent);
            if (ImGui::Button(m == 0 ? "The whole picture" : "The faces", ImVec2(half, 0))) {
                g_ctl.mode = m;
                if (m == 1 && g_ctl.detail < 0.3) g_ctl.detail = 0.35;
            }
            if (on) ImGui::PopStyleColor();
        }
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled(g_ctl.mode == 0 ? "Makes it bigger and sharper: an upscale model resizes it, and MOSS-Image can redraw fine detail on top."
                                            : "Redraws only the faces SAM 3 finds (eyes, skin, lines) with MOSS-Image, and leaves everything else as it is. Works best at 2x.");
        ImGui::PopTextWrapPos();
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("3. What kind of picture");
    {
        const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        for (int s = 0; s < 2; ++s) {
            if (s) ImGui::SameLine();
            const bool on = g_ctl.style == s;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ui::kAccent);
            if (ImGui::Button(s == 0 ? "Realistic / photo" : "Anime / drawn", ImVec2(half, 0))) g_ctl.style = s;
            if (on) ImGui::PopStyleColor();
        }
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled(g_ctl.style == 0 ? "Photo models and skin / hair / fabric detail." : "Anime models and clean line art, flat colour: it will not add realism.");
        ImGui::PopTextWrapPos();
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("4. Pick a preset");
    {
        static const Preset whole[] = {{"Sharp 2x", 2.0, 0.0, "just bigger and sharper"}, {"Detailed 2x", 2.0, 0.30, "bigger, with new fine detail"}, {"Rebuild 2x", 2.0, 0.50, "bigger, redrawn more"},
                                       {"Big 4x", 4.0, 0.0, "four times bigger"}, {"Clean up", 1.0, 0.25, "same size, less noise and blur"}};
        static const Preset faces[] = {{"Faces: subtle 2x", 2.0, 0.25, "natural, close to the original"}, {"Faces: strong 2x", 2.0, 0.45, "clearly redrawn faces"}, {"Faces, same size", 1.0, 0.35, "no resize"}};
        const Preset* list = g_ctl.mode == 0 ? whole : faces;
        const int n = g_ctl.mode == 0 ? 5 : 3;
        const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        for (int i = 0; i < n; ++i) {
            const float w = ImGui::CalcTextSize(list[i].name).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            if (i > 0 && ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + w < right) ImGui::SameLine();
            const bool on = std::fabs(g_ctl.scale - list[i].scale) < 0.01 && std::fabs(g_ctl.detail - (g_ctl.style == 1 ? std::min(list[i].detail, 0.4) : list[i].detail)) < 0.01;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ui::kAccent);
            if (ImGui::SmallButton(list[i].name)) ApplyPreset(list[i]);
            if (on) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", list[i].hint);
        }
    }

    // the result size and an honest time estimate
    int w0 = 0, h0 = 0;
    if (g_sel < 0 && g_ctl.preview >= 0 && g_ctl.preview < static_cast<int>(g_ctl.sources.size()) && g_view.beforePath == g_ctl.sources[g_ctl.preview]) {
        w0 = g_view.before.width;
        h0 = g_view.before.height;
    }
    ImGui::Spacing();
    if (w0 > 0) ImGui::Text("Result: %s pixels (from %d x %d, %.3gx)", SizeText(w0 * g_ctl.scale, h0 * g_ctl.scale).c_str(), w0, h0, g_ctl.scale);
    else ImGui::Text("Result: %.3gx the size", g_ctl.scale);
    const bool redraw = g_ctl.mode == 1 || g_ctl.detail > 0.01;
    if (redraw) {
        const double est = EstimateEditSeconds(g_ctl.steps, EstimateRes(w0, h0, g_ctl.scale), 1, 0);
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Redraw time: about %s per picture (%s)", FormatEditTime(est).c_str(), EditEstimateSource().c_str());
        ImGui::PopTextWrapPos();
        if (w0 > 0 && static_cast<double>(w0) * h0 * g_ctl.scale * g_ctl.scale > 6.6e6) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(ui::kWarn, "Too big to redraw (over 6.5 megapixels): %s", g_ctl.mode == 1 ? "pick a smaller size for the faces." : "it will only be resized.");
            ImGui::PopTextWrapPos();
        }
    } else {
        ImGui::TextDisabled("Resize only: a few seconds.");
    }

    if (ImGui::TreeNode("Fine-tune")) {
        float scale = static_cast<float>(g_ctl.scale), detail = static_cast<float>(g_ctl.detail);
        ImGui::SetNextItemWidth(ui::Dp(200.0f));
        if (ImGui::SliderFloat("Size (times)##upscale", &scale, 1.0f, 4.0f, "%.2fx")) g_ctl.scale = std::round(scale * 4.0f) / 4.0f;
        ImGui::SetNextItemWidth(ui::Dp(200.0f));
        if (ImGui::SliderFloat("How much MOSS redraws##updetail", &detail, 0.0f, 0.7f, detail < 0.01f ? "off (resize only)" : "%.2f")) g_ctl.detail = std::round(detail * 20.0f) / 20.0f;
        ImGui::SetNextItemWidth(ui::Dp(200.0f));
        ImGui::SliderInt("Quality (steps of the redraw)##upsteps", &g_ctl.steps, 10, 40);
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Low: the picture stays as it is. High: more new detail, but faces and small things can start to change.");
        ImGui::PopTextWrapPos();
        ImGui::TreePop();
    }
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Resizing uses a model from ComfyUI's models/upscale_models (a photo one such as 4x-UltraSharp, an anime one such as RealESRGAN_x4plus_anime_6B) when it has one, otherwise Lanczos.");
    ImGui::PopTextWrapPos();
    ImGui::EndChild();

    const bool ready = !g_ctl.sources.empty();
    ImGui::BeginDisabled(!ready);
    char label[64];
    snprintf(label, sizeof(label), g_ctl.sources.size() > 1 ? "Upscale %d pictures" : "Upscale", static_cast<int>(g_ctl.sources.size()));
    if (ui::PrimaryButton(label, ImVec2(ui::Dp(170.0f), 0))) {
        for (const std::wstring& src : g_ctl.sources) {
            LoadedImage probe; // the size, to keep the redraw inside what a GPU can hold
            std::string err;
            int sw = 0, sh = 0;
            if (g_view.beforePath == src && g_view.before.srv) {
                sw = g_view.before.width;
                sh = g_view.before.height;
            } else if (LoadImageTexture(app.Device(), src, probe, &err)) {
                sw = probe.width;
                sh = probe.height;
                ReleaseImage(probe);
            }
            ComfyJob job;
            job.images = {src};
            job.upscale = g_ctl.mode == 0 ? 1 : 2;
            job.anime = g_ctl.style == 1;
            job.scale = g_ctl.scale;
            job.detail = g_ctl.detail;
            job.steps = g_ctl.steps;
            job.srcW = sw;
            job.srcH = sh;
            job.times = 1;
            job.label = ToUtf8(fs::path(src).filename().wstring());
            job.prompt = ReadImageMetadata(src).parsed.positivePrompt; // MOSS redraws from words: what the picture shows
            const int act = SendUpscale(app, job);
            if (act <= 0) continue;
            UpRecord r;
            r.id = g_nextId++;
            r.activity = act;
            r.label = job.label;
            r.src = src;
            r.mode = job.upscale;
            r.anime = job.anime;
            r.scale = job.scale;
            r.detail = job.detail;
            r.steps = job.steps;
            r.srcW = sw;
            r.srcH = sh;
            r.when = static_cast<long long>(std::time(nullptr));
            r.state = "Waiting";
            g_sel = r.id;
            g_ups.push_back(std::move(r));
        }
        SaveUps();
        app.SetStatus("Sent to ComfyUI for upscaling: see Edits > Upscale");
    }
    ImGui::EndDisabled();
    ImGui::EndChild();
    ImGui::EndTable();
}
