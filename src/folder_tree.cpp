// Folder panes: Explorer-style trees.
//  - Gallery: the opened folder as a tree of its subfolders (with picture counts); clicking one narrows the gallery to
//    it instantly, no rescan. Pictures can be dragged onto a folder to move them; "New folder" makes one. Also recent
//    folders and "This PC" (double-click any folder there to open it).
//  - Vision library: the folders inside the Vision output folder, the same way (images + their result texts move together).
#include <algorithm>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>
#include <shellapi.h>

#include "imgui.h"
#include "ui_extra.h"
#include "ui_theme.h"
#include "win_dialogs.h"

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

std::string LowerKey(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return r;
}

// ---- a tree of folders (relative paths, backslash separated) -----------------------------------------------
struct FNode {
    std::string name;
    std::string rel; // "" for the root
    int count = 0;   // pictures in it and below it
    std::vector<int> kids;
};
using FTree = std::vector<FNode>;

// `own` = pictures directly in each folder; `extra` = more folders to show even when empty.
FTree MakeTree(const std::map<std::string, int>& own, const std::vector<std::string>& extra) {
    FTree t;
    t.push_back(FNode{});
    std::unordered_map<std::string, int> byKey;
    auto ensure = [&](const std::string& rel) -> int {
        int node = 0;
        size_t pos = 0;
        std::string path;
        while (pos <= rel.size() && !rel.empty()) {
            size_t sep = rel.find('\\', pos);
            const std::string part = rel.substr(pos, sep == std::string::npos ? std::string::npos : sep - pos);
            if (!part.empty()) {
                path = path.empty() ? part : path + "\\" + part;
                const std::string key = LowerKey(path);
                auto it = byKey.find(key);
                int child;
                if (it == byKey.end()) {
                    child = static_cast<int>(t.size());
                    FNode n;
                    n.name = part;
                    n.rel = path;
                    t.push_back(std::move(n));
                    byKey[key] = child;
                    t[node].kids.push_back(child);
                } else {
                    child = it->second;
                }
                node = child;
            }
            if (sep == std::string::npos) break;
            pos = sep + 1;
        }
        return node;
    };
    for (const auto& [rel, c] : own) {
        // the count belongs to the folder and every folder above it
        int node = 0;
        t[0].count += c;
        size_t pos = 0;
        std::string path;
        while (!rel.empty()) {
            size_t sep = rel.find('\\', pos);
            const std::string part = rel.substr(pos, sep == std::string::npos ? std::string::npos : sep - pos);
            path = path.empty() ? part : path + "\\" + part;
            node = ensure(path);
            t[node].count += c;
            if (sep == std::string::npos) break;
            pos = sep + 1;
        }
    }
    for (const std::string& rel : extra) ensure(rel);
    for (FNode& n : t) {
        std::sort(n.kids.begin(), n.kids.end(), [&](int a, int b) { return _stricmp(t[a].name.c_str(), t[b].name.c_str()) < 0; });
    }
    return t;
}

// ---- folders on disk ("This PC") --------------------------------------------------------------------
std::unordered_map<std::wstring, std::vector<std::wstring>>& DiskKids() {
    static std::unordered_map<std::wstring, std::vector<std::wstring>> cache;
    return cache;
}

const std::vector<std::wstring>& ListSubfolders(const std::wstring& dir) {
    auto& cache = DiskKids();
    auto it = cache.find(dir);
    if (it != cache.end()) return it->second;
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    const std::wstring pattern = dir + (dir.back() == L'\\' ? L"*" : L"\\*");
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
            if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) continue;
            names.push_back(fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    return cache[dir] = std::move(names);
}

// every folder below `root` (relative, backslash separated), limited so a huge disk can't stall the UI
void WalkDirs(const std::wstring& root, const std::wstring& skip, std::vector<std::string>& out) {
    std::vector<std::pair<std::wstring, std::wstring>> pending{{root, L""}};
    while (!pending.empty() && out.size() < 6000) {
        auto [dir, rel] = std::move(pending.back());
        pending.pop_back();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
            if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) continue;
            const std::wstring sub = dir + L"\\" + fd.cFileName;
            if (!skip.empty() && _wcsicmp(sub.c_str(), skip.c_str()) == 0) continue;
            const std::wstring r = rel.empty() ? std::wstring(fd.cFileName) : rel + L"\\" + fd.cFileName;
            out.push_back(ToUtf8(r));
            pending.push_back({sub, r});
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

// ---- drawing ------------------------------------------------------------------------------------------
void DrawFolderIcon(ImDrawList* dl, ImVec2 p, float h, bool open) {
    const float w = h * 1.15f, hh = h * 0.8f;
    const ImU32 body = IM_COL32(236, 190, 84, 255), tab = IM_COL32(214, 164, 58, 255);
    const float y = p.y + (h - hh) * 0.5f;
    dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + w * 0.45f, y + hh * 0.3f), tab, 2.0f);
    dl->AddRectFilled(ImVec2(p.x, y + hh * 0.18f), ImVec2(p.x + w, y + hh), open ? tab : body, 2.0f);
    if (open) dl->AddRectFilled(ImVec2(p.x + w * 0.1f, y + hh * 0.38f), ImVec2(p.x + w, y + hh), body, 2.0f);
}

struct RowResult {
    bool open = false;
    bool clicked = false;
    bool doubleClicked = false;
};

// One tree row: arrow, folder icon, name and an optional count at the right edge.
RowResult Row(const char* id, const std::string& label, bool leaf, bool selected, bool defaultOpen, int count) {
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding;
    if (leaf) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (selected) flags |= ImGuiTreeNodeFlags_Selected;
    if (defaultOpen) flags |= ImGuiTreeNodeFlags_DefaultOpen;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ui::Dp(2.0f), ui::Dp(2.0f)));
    RowResult r;
    r.open = ImGui::TreeNodeEx(id, flags, "%s", "");
    ImGui::PopStyleVar();
    r.clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen();
    r.doubleClicked = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    const float spacing = ImGui::GetTreeNodeToLabelSpacing();
    const float iconH = ui::Dp(14.0f);
    const float cy = (mn.y + mx.y) * 0.5f;
    DrawFolderIcon(dl, ImVec2(mn.x + spacing - ui::Dp(2.0f), cy - iconH * 0.5f), iconH, r.open && !leaf);
    const float tx = mn.x + spacing + iconH * 1.15f + ui::Dp(5.0f);
    const float textY = cy - ImGui::GetTextLineHeight() * 0.5f;
    float rightLimit = mx.x - ui::Dp(4.0f);
    if (count >= 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", count);
        const float cw = ImGui::CalcTextSize(buf).x;
        dl->AddText(ImVec2(mx.x - cw - ui::Dp(6.0f), textY), ImGui::GetColorU32(ui::kTextDim), buf);
        rightLimit -= cw + ui::Dp(8.0f);
    }
    const ImVec4 clip(tx, mn.y, rightLimit, mx.y);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(tx, textY), ImGui::GetColorU32(ImGuiCol_Text), label.c_str(), nullptr, 0.0f, &clip);
    return r;
}

void ShowInExplorer(const std::wstring& path) {
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// What a tree does with clicks, drops and the right-click menu.
struct TreeActions {
    std::function<void(const std::string& rel)> select;
    std::function<void(const std::string& rel)> menu;                                    // fills the context menu
    std::function<void(const ImGuiPayload*, const std::string& rel)> drop;
    const char* payloadType = "";
};

void DrawFNode(const FTree& t, int index, const std::string& selectedRel, const std::string& rootLabel, const TreeActions& act) {
    const FNode& n = t[index];
    const bool isRoot = index == 0;
    const bool selected = _stricmp(selectedRel.c_str(), n.rel.c_str()) == 0;
    ImGui::PushID(index);
    const RowResult r = Row("##f", isRoot ? rootLabel : n.name, n.kids.empty(), selected, isRoot, n.count);
    if (r.clicked && act.select) act.select(n.rel);
    if (act.drop && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(act.payloadType)) act.drop(p, n.rel);
        ImGui::EndDragDropTarget();
    }
    if (act.menu && ImGui::BeginPopupContextItem("##fctx")) {
        act.menu(n.rel);
        ImGui::EndPopup();
    }
    if (r.open && !n.kids.empty()) {
        for (int kid : n.kids) DrawFNode(t, kid, selectedRel, rootLabel, act);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void DrawDiskNode(AppState& app, const std::wstring& path, const std::wstring& label, int depth) {
    if (depth > 24) return;
    ImGui::PushID(ToUtf8(path).c_str());
    const bool isCurrent = _wcsicmp(path.c_str(), app.currentFolder.c_str()) == 0;
    const RowResult r = Row("##d", ToUtf8(label), false, isCurrent, false, -1);
    if (r.doubleClicked) app.OpenFolder(path); // a drive can hold millions of files: only a double-click opens
    if (ImGui::BeginPopupContextItem("##dctx")) {
        if (ImGui::MenuItem("Open")) app.OpenFolder(path);
        if (ImGui::MenuItem("Show in Explorer")) ShowInExplorer(path);
        if (ImGui::MenuItem("Copy path")) ImGui::SetClipboardText(ToUtf8(path).c_str());
        ImGui::EndPopup();
    }
    if (r.open) {
        const std::vector<std::wstring>& kids = ListSubfolders(path);
        for (const std::wstring& k : kids) DrawDiskNode(app, path + (path.back() == L'\\' ? L"" : L"\\") + k, k, depth + 1);
        if (kids.empty()) ImGui::TextDisabled("  (no subfolders)");
        ImGui::TreePop();
    }
    ImGui::PopID();
}

// "New folder" box shared by both panes. `parent` is the full path of the folder it is made in.
struct NewFolderState {
    bool open = false;
    std::wstring parent;
    std::string label; // shown in the box: where it will be made
    char name[128] = {0};
    std::string error;
};

void DrawNewFolderPopup(AppState& app, NewFolderState& st, const std::function<void(const std::wstring&)>& created) {
    if (st.open) {
        ImGui::OpenPopup("New folder##nf");
        st.open = false;
        st.name[0] = 0;
        st.error.clear();
    }
    if (ImGui::BeginPopupModal("New folder##nf", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled("Made inside: %s", st.label.c_str());
        ImGui::SetNextItemWidth(ui::Dp(300.0f));
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputTextWithHint("##nfname", "Folder name", st.name, sizeof(st.name), ImGuiInputTextFlags_EnterReturnsTrue);
        if (!st.error.empty()) ImGui::TextColored(ui::kBad, "%s", st.error.c_str());
        if (ui::PrimaryButton("Create", ImVec2(ui::Dp(100.0f), 0)) || enter) {
            std::string err;
            if (app.CreateFolder(st.parent, st.name, &err)) {
                if (created) created(st.parent + L"\\" + ToWide(st.name));
                ImGui::CloseCurrentPopup();
            } else {
                st.error = err;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(ui::Dp(90.0f), 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

std::wstring Join(const std::wstring& root, const std::string& rel) {
    return rel.empty() ? root : root + L"\\" + ToWide(rel);
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------
void DrawFolderPane(AppState& app, float height) {
    ImGui::BeginChild("folder_pane", ImVec2(0, height), true);
    static NewFolderState newFolder;

    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ui::kTextDim, "FOLDERS");
    if (ImGui::SmallButton("Browse...")) {
        std::wstring picked = OpenFolderDialog(GetActiveWindow());
        if (!picked.empty()) app.OpenFolder(picked);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(app.currentFolder.empty());
    if (ImGui::SmallButton("New folder")) {
        newFolder.open = true;
        newFolder.parent = Join(app.currentFolder, app.filters.dir);
        newFolder.label = ToUtf8(newFolder.parent);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Makes a folder inside the one selected below.\nThen drag pictures onto it.");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton("Close")) {
        app.showFolderPane = false;
        app.SaveUiSettings();
    }
    ImGui::Spacing();

    ImGui::BeginChild("folder_tree_scroll", ImVec2(0, 0), false);
    if (!app.currentFolder.empty()) {
        struct Cache {
            FTree tree;
            unsigned epoch = ~0u, generation = ~0u;
            size_t entryCount = 0;
            std::wstring folder;
            unsigned long long builtAt = 0;
            std::vector<std::string> diskDirs;
            unsigned dirsGeneration = ~0u;
            std::wstring dirsFolder;
        };
        static Cache c;
        // Folders on disk (also empty ones): re-read when the folder changes or something was made / moved.
        if (c.dirsFolder != app.currentFolder || c.dirsGeneration != app.folderGeneration) {
            c.diskDirs.clear();
            WalkDirs(app.currentFolder, app.hideVisionFolder ? ToWide(app.visionOutputDir) : std::wstring(), c.diskDirs);
            c.dirsFolder = app.currentFolder;
            c.dirsGeneration = app.folderGeneration;
            c.generation = ~0u; // rebuild the tree too
        }
        // The pictures' own folders: rebuilt when the list changes, at most a few times a second while loading.
        const unsigned long long now = GetTickCount64();
        const bool stale = c.epoch != app.filterEpoch || c.entryCount != app.entries.size() || c.folder != app.currentFolder || c.generation != app.folderGeneration;
        if (stale && (c.folder != app.currentFolder || c.generation != app.folderGeneration || c.tree.empty() || now - c.builtAt > 600 || !app.IsLoading())) {
            std::map<std::string, int> own;
            for (const ImageEntry& e : app.entries) {
                if (e.missing) continue;
                const size_t sep = e.displayName.find_last_of(L"\\/");
                std::string dir = sep == std::wstring::npos ? std::string() : ToUtf8(e.displayName.substr(0, sep));
                std::replace(dir.begin(), dir.end(), '/', '\\');
                ++own[dir];
            }
            c.tree = MakeTree(own, c.diskDirs);
            c.epoch = app.filterEpoch;
            c.entryCount = app.entries.size();
            c.folder = app.currentFolder;
            c.generation = app.folderGeneration;
            c.builtAt = now;
        }
        if (!c.tree.empty()) {
            const std::wstring root = app.currentFolder;
            std::wstring rootLabel = std::filesystem::path(root).filename().wstring();
            if (rootLabel.empty()) rootLabel = root;
            TreeActions act;
            act.payloadType = "MV_IMAGES";
            act.select = [&](const std::string& rel) {
                app.filters.dir = rel;
                app.filterEpoch++;
            };
            act.drop = [&](const ImGuiPayload* p, const std::string& rel) {
                const int* idx = static_cast<const int*>(p->Data);
                const int n = p->DataSize / static_cast<int>(sizeof(int));
                std::vector<std::wstring> paths;
                for (int k = 0; k < n; ++k) {
                    if (idx[k] >= 0 && idx[k] < static_cast<int>(app.entries.size())) paths.push_back(app.entries[idx[k]].path);
                }
                const AppState::MoveReport rep = app.MoveImages(paths, Join(root, rel));
                app.SetStatus(rep.message);
            };
            act.menu = [&](const std::string& rel) {
                if (ImGui::MenuItem("Show only this folder")) {
                    app.filters.dir = rel;
                    app.filterEpoch++;
                }
                if (ImGui::MenuItem("New folder inside...")) {
                    newFolder.open = true;
                    newFolder.parent = Join(root, rel);
                    newFolder.label = ToUtf8(newFolder.parent);
                }
                if (!rel.empty() && ImGui::MenuItem("Open as the main folder")) app.OpenFolder(Join(root, rel));
                if (ImGui::MenuItem("Show in Explorer")) ShowInExplorer(Join(root, rel));
                if (ImGui::MenuItem("Copy path")) ImGui::SetClipboardText(ToUtf8(Join(root, rel)).c_str());
            };
            DrawFNode(c.tree, 0, app.filters.dir, ToUtf8(rootLabel), act);
        }
    }
    DrawNewFolderPopup(app, newFolder, nullptr);

    ImGui::Spacing();
    if (!app.recentFolders.empty()) {
        ImGui::SetNextItemOpen(false, ImGuiCond_Once);
        if (ImGui::TreeNodeEx("Recent folders", ImGuiTreeNodeFlags_SpanAvailWidth)) {
            for (const std::string& r : app.recentFolders) {
                ImGui::PushID(r.c_str());
                const std::wstring full = ToWide(r);
                const std::string shown = ToUtf8(std::filesystem::path(full).filename().wstring().empty() ? full : std::filesystem::path(full).filename().wstring());
                if (ImGui::Selectable(shown.c_str())) app.OpenFolder(full);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", r.c_str());
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
    }
    ImGui::SetNextItemOpen(false, ImGuiCond_Once);
    if (ImGui::TreeNodeEx("This PC", ImGuiTreeNodeFlags_SpanAvailWidth)) {
        ImGui::TextDisabled("Double-click a folder to open it");
        const DWORD drives = GetLogicalDrives();
        for (int i = 0; i < 26; ++i) {
            if (!(drives & (1u << i))) continue;
            const std::wstring root = std::wstring(1, static_cast<wchar_t>(L'A' + i)) + L":\\";
            const UINT type = GetDriveTypeW(root.c_str());
            if (type == DRIVE_NO_ROOT_DIR || type == DRIVE_UNKNOWN) continue;
            DrawDiskNode(app, root, root.substr(0, 2) + (type == DRIVE_REMOVABLE ? L" (removable)" : (type == DRIVE_REMOTE ? L" (network)" : L"")), 0);
        }
        ImGui::TreePop();
    }
    ImGui::EndChild();
    ImGui::EndChild();
}

// The folders inside the Vision output folder, for the Vision library tab.
void DrawLibraryFolderPane(AppState& app, float height) {
    ImGui::BeginChild("lib_folder_pane", ImVec2(0, height), true);
    static NewFolderState newFolder;
    const std::wstring root = ToWide(app.visionOutputDir);

    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ui::kTextDim, "FOLDERS");
    if (ImGui::SmallButton("New folder")) {
        newFolder.open = true;
        newFolder.parent = Join(root, app.visionLibDir);
        newFolder.label = ToUtf8(newFolder.parent);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Makes a folder inside the one selected below.\nThen drag results onto it.");
    ImGui::Spacing();

    ImGui::BeginChild("lib_tree_scroll", ImVec2(0, 0), false);
    std::map<std::string, int> own;
    for (const auto& it : app.visionLib) ++own[it.dir];
    static FTree tree;
    static unsigned builtVersion = ~0u;
    static size_t builtDirs = ~size_t(0);
    if (builtVersion != app.visionLibVersion || builtDirs != app.visionLibDirs.size()) {
        tree = MakeTree(own, app.visionLibDirs);
        builtVersion = app.visionLibVersion;
        builtDirs = app.visionLibDirs.size();
    }
    TreeActions act;
    act.payloadType = "MV_LIBITEM";
    act.select = [&](const std::string& rel) {
        app.visionLibDir = rel;
        app.visionLibTopOnly = false;
    };
    act.drop = [&](const ImGuiPayload* p, const std::string& rel) {
        const wchar_t* path = static_cast<const wchar_t*>(p->Data);
        const AppState::MoveReport rep = app.MoveImages({std::wstring(path)}, Join(root, rel));
        app.SetStatus(rep.message);
    };
    act.menu = [&](const std::string& rel) {
        if (ImGui::MenuItem("New folder inside...")) {
            newFolder.open = true;
            newFolder.parent = Join(root, rel);
            newFolder.label = ToUtf8(newFolder.parent);
        }
        if (ImGui::MenuItem("Show in Explorer")) ShowInExplorer(Join(root, rel));
    };
    if (!tree.empty()) {
        DrawFNode(tree, 0, app.visionLibTopOnly ? std::string("\x01") : app.visionLibDir, "All results", act);
        // the pictures that are not in any folder yet
        ImGui::PushID("toponly");
        int top = 0;
        auto it0 = own.find("");
        if (it0 != own.end()) top = it0->second;
        const RowResult r = Row("##top", "Not in a folder", true, app.visionLibTopOnly, false, top);
        if (r.clicked) {
            app.visionLibTopOnly = true;
            app.visionLibDir.clear();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("MV_LIBITEM")) {
                const wchar_t* path = static_cast<const wchar_t*>(p->Data);
                app.SetStatus(app.MoveImages({std::wstring(path)}, root).message);
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::PopID();
    }
    DrawNewFolderPopup(app, newFolder, nullptr);
    ImGui::EndChild();
    ImGui::EndChild();
}
