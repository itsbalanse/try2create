// The app's own Recycle bin (2026-10-30, see recycle_bin.h): a page of what was deleted with an x. Restoring only puts it back on its lists, as the file never moved.
#include "recycle_bin.h"

#include <windows.h>

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>

#include "app_state.h"
#include "imgui.h"
#include "json.hpp"
#include "ui_extra.h"
#include "ui_theme.h"
#include "win_dialogs.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::vector<BinItem> g_items;
std::set<std::wstring> g_paths; // lower case: everything in the bin
bool g_loaded = false;

std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

std::string U8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring W(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

fs::path ListFile() {
    wchar_t buf[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    return fs::path(buf) / L"Try2Create" / L"recycle_bin.json";
}

void Reindex() {
    g_paths.clear();
    for (const BinItem& i : g_items)
        for (const std::wstring& f : i.files) g_paths.insert(Lower(f));
}

void Save() {
    json j = json::array();
    for (const BinItem& i : g_items) {
        json files = json::array();
        for (const std::wstring& f : i.files) files.push_back(U8(f));
        json r = {{"has", i.hasResult}};
        if (i.hasResult)
            r = {{"has", true}, {"label", i.result.label}, {"prompt", i.result.prompt}, {"filename", i.result.filename}, {"subfolder", i.result.subfolder}, {"type", i.result.type},
                 {"path", U8(i.result.localPath)}, {"video", i.result.video}, {"audio", i.result.audio}, {"seconds", i.result.seconds}};
        j.push_back({{"name", i.name}, {"kind", i.kind}, {"files", std::move(files)}, {"thumb", U8(i.thumb)}, {"when", i.when}, {"result", std::move(r)}});
    }
    std::error_code ec;
    fs::create_directories(ListFile().parent_path(), ec);
    std::ofstream(ListFile(), std::ios::binary | std::ios::trunc) << j.dump(1);
}

void Load() {
    if (g_loaded) return;
    g_loaded = true;
    std::ifstream in(ListFile(), std::ios::binary);
    if (!in) return;
    const json j = json::parse(in, nullptr, false);
    if (!j.is_array()) return;
    std::error_code ec;
    for (const json& o : j) {
        if (!o.is_object()) continue;
        BinItem i;
        i.name = o.value("name", std::string());
        i.kind = o.value("kind", 0);
        i.thumb = W(o.value("thumb", std::string()));
        i.when = o.value("when", 0LL);
        if (o.contains("files") && o["files"].is_array())
            for (const json& f : o["files"])
                if (f.is_string() && fs::exists(W(f.get<std::string>()), ec)) i.files.push_back(W(f.get<std::string>()));
        if (o.contains("result") && o["result"].is_object() && o["result"].value("has", false)) {
            const json& r = o["result"];
            i.hasResult = true;
            i.result.label = r.value("label", std::string());
            i.result.prompt = r.value("prompt", std::string());
            i.result.filename = r.value("filename", std::string());
            i.result.subfolder = r.value("subfolder", std::string());
            i.result.type = r.value("type", std::string("output"));
            i.result.localPath = W(r.value("path", std::string()));
            i.result.video = r.value("video", false);
            i.result.audio = r.value("audio", false);
            i.result.seconds = r.value("seconds", 0.0);
        }
        if (!i.files.empty()) g_items.push_back(std::move(i));
    }
    Reindex();
}

void Restore(AppState& app, size_t k) {
    BinItem i = std::move(g_items[k]);
    g_items.erase(g_items.begin() + k);
    Reindex();
    Save();
    if (i.hasResult && app.comfySvc) app.comfySvc->RestoreResult(i.result);
    app.filterEpoch++;
    if (i.kind == 1) VideosRescan();
    if (i.kind == 2) ThreeDRescan();
}

// Everything still there, to the Windows Recycle Bin
bool SendAll(std::wstring* err) {
    std::vector<std::wstring> files;
    std::error_code ec;
    for (const BinItem& i : g_items)
        for (const std::wstring& f : i.files)
            if (fs::exists(f, ec)) files.push_back(f);
    if (!files.empty() && !DeleteFilesToRecycleBin(nullptr, files, err)) return false;
    g_items.clear();
    Reindex();
    Save();
    return true;
}

} // namespace

void BinAdd(AppState& app, BinItem item) {
    Load();
    item.when = static_cast<long long>(std::time(nullptr));
    g_items.push_back(std::move(item));
    Reindex();
    Save();
    app.filterEpoch++;
    app.SetStatus("In the Recycle bin page until you close the app: restore it there");
}

bool InBin(const std::wstring& path) {
    Load();
    return !g_paths.empty() && g_paths.count(Lower(path)) > 0;
}

int BinCount() {
    Load();
    return static_cast<int>(g_items.size());
}

void BinFlush() {
    Load();
    std::wstring err;
    SendAll(&err); // (if it fails the list stays, and the next close tries again)
}

void DrawBinTab(AppState& app, float height) {
    Load();
    ImGui::BeginChild("##binpage", ImVec2(0, height), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushFont(ui::TitleFont());
    ImGui::TextUnformatted("Recycle bin");
    ImGui::PopFont();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("What you delete with an x waits here, untouched. Click one to get it back. When you close the app they go to the Windows Recycle Bin.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (g_items.empty()) {
        ImGui::TextDisabled("Nothing here.");
        ImGui::EndChild();
        return;
    }
    if (ImGui::Button("Restore all")) {
        while (!g_items.empty()) Restore(app, g_items.size() - 1);
        ImGui::EndChild();
        return;
    }
    ImGui::SameLine();
    if (ImGui::Button("Send to the Windows Recycle Bin now")) {
        std::wstring err;
        if (SendAll(&err)) app.SetStatus("Moved them to the Windows Recycle Bin");
        else app.SetStatus("Could not move them: " + U8(err));
        ImGui::EndChild();
        return;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%d waiting", static_cast<int>(g_items.size()));
    ImGui::Spacing();
    static const char* const kKinds[] = {"Picture", "Video", "3D", "Sound"};
    const float tile = ui::Dp(132.0f), gap = ImGui::GetStyle().ItemSpacing.x, textH = ImGui::GetTextLineHeightWithSpacing();
    const int perRow = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + gap) / (tile + gap)));
    int col = 0, restore = -1;
    for (size_t k = g_items.size(); k-- > 0;) { // (the newest first)
        const BinItem& it = g_items[k];
        if (col) ImGui::SameLine();
        col = (col + 1) % perRow;
        ImGui::PushID(static_cast<int>(k));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##item", ImVec2(tile, tile + textH))) restore = static_cast<int>(k);
        const bool hov = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + tile, p.y + tile), IM_COL32(255, 255, 255, 14), ui::Dp(6.0f));
        const AppState::LibThumb lt = app.LibraryThumb(it.thumb);
        if (lt.srv && lt.w > 0 && lt.h > 0) {
            const float s = std::min(tile / lt.w, tile / lt.h), iw = lt.w * s, ih = lt.h * s;
            const ImVec2 a(p.x + (tile - iw) * 0.5f, p.y + (tile - ih) * 0.5f);
            dl->AddImageRounded((ImTextureID)(intptr_t)lt.srv, a, ImVec2(a.x + iw, a.y + ih), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, ui::Dp(6.0f));
        }
        const char* kind = kKinds[std::clamp(it.kind, 0, 3)];
        const ImVec2 ks = ImGui::CalcTextSize(kind);
        dl->AddRectFilled(ImVec2(p.x + ui::Dp(4.0f), p.y + ui::Dp(4.0f)), ImVec2(p.x + ui::Dp(10.0f) + ks.x, p.y + ui::Dp(6.0f) + ks.y), IM_COL32(0, 0, 0, 170), ui::Dp(4.0f));
        dl->AddText(ImVec2(p.x + ui::Dp(7.0f), p.y + ui::Dp(5.0f)), IM_COL32(235, 235, 240, 255), kind);
        if (hov) {
            dl->AddRectFilled(p, ImVec2(p.x + tile, p.y + tile), IM_COL32(0, 0, 0, 120), ui::Dp(6.0f));
            dl->AddRect(p, ImVec2(p.x + tile, p.y + tile), ImGui::GetColorU32(ui::kAccent), ui::Dp(6.0f), 0, ui::Dp(2.0f));
            const char* t = "Restore";
            const ImVec2 ts = ImGui::CalcTextSize(t);
            dl->AddText(ImVec2(p.x + (tile - ts.x) * 0.5f, p.y + (tile - ts.y) * 0.5f), IM_COL32_WHITE, t);
            ImGui::SetTooltip("%s\nClick: put it back", it.name.c_str());
        }
        ImGui::PushClipRect(ImVec2(p.x, p.y + tile), ImVec2(p.x + tile, p.y + tile + textH), true);
        dl->AddText(ImVec2(p.x + ui::Dp(2.0f), p.y + tile + ui::Dp(2.0f)), ImGui::GetColorU32(ui::kTextDim), it.name.c_str());
        ImGui::PopClipRect();
        ImGui::PopID();
    }
    if (restore >= 0) {
        Restore(app, restore);
        app.SetStatus("Put it back");
    }
    ImGui::EndChild();
}
