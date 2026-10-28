// The "Vision board" tab: pictures from the Vision library become cards (characters, a background, items, notes) on a
// ComfyUI-like canvas, joined by labelled relations ("Knight fights Dragon"); the board turns into one scene prompt.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <windows.h>
#include <shellapi.h>

#include "imgui.h"
#include "json.hpp"
#include "clipboard_image.h"
#include "clef.h"
#include "relation_import.h"
#include <commdlg.h>
#include "crash_log.h"
#include "stb_image_write.h"
#include "ui_extra.h"
#include "ui_theme.h"
#include "win_dialogs.h"

namespace fs = std::filesystem;

namespace {

constexpr float kHeaderH = 28.0f;  // card header, in board units
constexpr float kPad = 6.0f;
constexpr float kLine = 20.0f;
constexpr int kTextLines = 4;
constexpr float kFontUnits = 17.0f;

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

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

ImU32 Rgb(uint32_t rgb, int alpha = 255) { return IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, alpha); }

uint32_t KindColor(BoardKind k) {
    switch (k) {
        case BoardKind::Character: return 0x4F6FD8;
        case BoardKind::Background: return 0x3C9A6A;
        case BoardKind::Item: return 0xD08A2E;
        case BoardKind::Note: return 0x7A7A86;
    }
    return 0x777777;
}

int StdStringResize(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* str = static_cast<std::string*>(data->UserData);
        str->resize(data->BufTextLen);
        data->Buf = str->data();
    }
    return 0;
}

bool InputLine(const char* label, const char* hint, std::string& value, float width) {
    ImGui::SetNextItemWidth(width);
    return ImGui::InputTextWithHint(label, hint, value.data(), value.capacity() + 1, ImGuiInputTextFlags_CallbackResize, StdStringResize, &value);
}

bool InputBox(const char* label, std::string& value, float width, float height, bool readOnly = false) {
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_CallbackResize;
    if (readOnly) flags |= ImGuiInputTextFlags_ReadOnly;
    return ImGui::InputTextMultiline(label, value.data(), value.capacity() + 1, ImVec2(width, height), flags, StdStringResize, &value);
}

// Read-only text that wraps (a multi-line input box in this ImGui version does not wrap).
void WrappedBox(const char* id, const std::string& text, float height) {
    ImGui::BeginChild(id, ImVec2(0, height), true);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(text.empty() ? "" : text.c_str(), text.c_str() + text.size());
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

void ResetHistory(BoardViewState& st); // (defined with the undo code below)

void Touch(BoardViewState& st) {
    st.histDirty = true;
    st.dirty = true;
    st.dirtyAt = ImGui::GetTime();
}

std::wstring CurrentPath() { return BoardsDir() + L"\\_current.json"; }

std::wstring NamedPath(const std::string& name) {
    std::string clean;
    for (char c : name) clean += (std::strchr("\\/:*?\"<>|", c) || static_cast<unsigned char>(c) < 32) ? '_' : c;
    return BoardsDir() + L"\\" + ToWide(clean) + L".json";
}

std::string StyleText(const BoardViewState& st) {
    const int n = VisionStyleCount();
    if (st.styleIdx >= 1 && st.styleIdx <= n) return VisionStyleAt(st.styleIdx - 1).directive;
    if (st.styleIdx == n + 1) return st.customStyle;
    return {};
}

BoardPromptOptions Options(const BoardViewState& st) {
    BoardPromptOptions o;
    o.format = st.format;
    o.scene = st.curScene;
    o.seconds = st.seconds;
    o.style = StyleText(st);
    o.extra = st.extra;
    o.attachImages = st.attach;
    return o;
}


void OpenBoardPictureEdit(AppState& app, BoardViewState& st, const std::string& prompt);
bool g_boardPixel = false;     // pixel art mode: the board's pictures also become pixel art (Pixel art tab)
bool g_boardPixelLook = true;  // ...and ask for a pixel-art look

void TouchProfiles(BoardViewState& st) {
    st.profilesDirty = true;
    st.profilesDirtyAt = ImGui::GetTime();
}

std::wstring SettingsPath() { return fs::path(BoardsDir()).parent_path().wstring() + L"\\board_settings.json"; }

void LoadBoardSettings(BoardViewState& st) {
    std::ifstream f(SettingsPath(), std::ios::binary);
    if (!f) return;
    nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
    if (j.is_object()) {
        st.exportDir = j.value("exportDir", std::string());
        st.relGroups = j.value("relGroups", st.relGroups);
    }
}

void SaveBoardSettings(const BoardViewState& st) {
    nlohmann::json j = {{"exportDir", st.exportDir}, {"relGroups", st.relGroups}};
    std::ofstream out(SettingsPath(), std::ios::binary | std::ios::trunc);
    if (out) out << j.dump(2);
}

std::wstring ExportRoot(const BoardViewState& st) { return st.exportDir.empty() ? DefaultExportDir() : ToWide(st.exportDir); }

// Writes the board now (the scratch file and, for a named board, its own file).
void FlushBoard(BoardViewState& st) {
    SaveBoard(st.board, BoardsDir() + L"\\_current.json");
    if (!st.name.empty()) SaveBoard(st.board, NamedPath(st.name));
    st.dirty = false;
}

// An unnamed board with something on it is kept under a time-stamped name before it is replaced, so nothing is lost.
// Returns the file it was kept in ("" when there was nothing to keep).
std::wstring ArchiveScratch(BoardViewState& st) {
    if (!st.name.empty() || st.board.nodes.empty()) return {};
    std::time_t now = std::time(nullptr);
    std::tm tmv{};
    localtime_s(&tmv, &now);
    char buf[64];
    std::strftime(buf, sizeof(buf), "Untitled %Y-%m-%d %H-%M-%S", &tmv);
    const std::wstring path = NamedPath(buf);
    SaveBoard(st.board, path);
    st.browserDirty = true;
    return path;
}

// Saves the board that is about to be replaced (under its name, or as "Untitled <time>") and remembers it for the Back button.
void KeepCurrent(BoardViewState& st) {
    FlushBoard(st);
    std::wstring kept;
    if (!st.name.empty()) {
        kept = NamedPath(st.name);
    } else {
        kept = ArchiveScratch(st);
        if (!kept.empty()) st.browserDirty = true;
    }
    if (!kept.empty()) {
        st.prevBoardPath = kept;
        st.prevBoardName = ToUtf8(fs::path(kept).stem().wstring());
    }
}

void OpenBoardFile(AppState& app, BoardViewState& st, const std::wstring& path) {
    Board loaded;
    if (!LoadBoard(path, loaded)) {
        app.SetStatus("Could not read that board");
        return;
    }
    KeepCurrent(st);
    for (BoardNode& n : loaded.nodes) n.describing = false;
    st.board = std::move(loaded);
    st.name = ToUtf8(fs::path(path).stem().wstring());
    st.curScene = 0;
    st.selNode = st.selLink = -1;
    st.result.clear();
    st.fitRequest = true;
    if (st.board.scenes.size() > 1) st.format = 2; // it has beats: it is a video board
    Touch(st);
    ResetHistory(st);
}

void EnsureBoardIndex(BoardViewState& st) {
    if (!st.browserDirty) return;
    st.browserBoards = ScanBoards();
    st.browserDirty = false;
}

int BoardsWithCharacter(const BoardViewState& st, const std::string& key) {
    int n = 0;
    for (const BoardInfo& b : st.browserBoards) {
        for (const BoardCharRef& c : b.characters) {
            if (c.key == key) {
                ++n;
                break;
            }
        }
    }
    return n;
}

// a picture fitted into a box (the box is filled dark first)
void DrawThumbAt(AppState& app, ImDrawList* dl, const std::wstring& path, ImVec2 a, ImVec2 b, float round = 3.0f) {
    // built-in icons are drawn in dark colours too: give them a light backing so every one shows
    const bool icon = path.find(L"icons-v1\\") != std::wstring::npos;
    dl->AddRectFilled(a, b, icon ? IM_COL32(214, 219, 228, 255) : IM_COL32(21, 21, 24, 255), round);
    if (path.empty()) return;
    const AppState::LibThumb t = app.LibraryThumb(path);
    if (t.srv && t.w > 0) {
        const float k = std::min((b.x - a.x) / t.w, (b.y - a.y) / t.h);
        const float iw = t.w * k, ih = t.h * k;
        const ImVec2 m(a.x + ((b.x - a.x) - iw) * 0.5f, a.y + ((b.y - a.y) - ih) * 0.5f);
        dl->AddImage((ImTextureID)(intptr_t)t.srv, m, ImVec2(m.x + iw, m.y + ih));
    }
}

uint32_t SceneColor(int scene) {
    static const uint32_t colors[] = {0x4F6FD8, 0x3C9A6A, 0xD08A2E, 0xB05FD0, 0xD0556A, 0x4FB0C0};
    return colors[((scene % 6) + 6) % 6];
}


// ---------------------------------------------------------------------------------------------------------------
// Undo / redo: the board as it was before each finished edit (a drag, a typed line, a delete...) is kept
void ResetHistory(BoardViewState& st) {
    st.undoStack.clear();
    st.redoStack.clear();
    st.lastSnapshot = st.board;
    st.histDirty = false;
}

void CommitHistory(BoardViewState& st) {
    if (!st.histDirty || ImGui::IsAnyItemActive() || ImGui::IsMouseDown(0)) return; // wait until the edit is finished
    st.undoStack.push_back(st.lastSnapshot);
    if (st.undoStack.size() > 100) st.undoStack.erase(st.undoStack.begin());
    st.redoStack.clear();
    st.lastSnapshot = st.board;
    st.histDirty = false;
}

void RestoreBoard(BoardViewState& st, Board snapshot) {
    snapshot.panX = st.board.panX; // the view stays where it is
    snapshot.panY = st.board.panY;
    snapshot.zoom = st.board.zoom;
    for (BoardNode& n : snapshot.nodes) n.describing = false;
    st.board = std::move(snapshot);
    st.lastSnapshot = st.board;
    st.histDirty = false;
    st.curScene = std::clamp(st.curScene, 0, static_cast<int>(st.board.scenes.size()) - 1);
    if (!st.board.Find(st.selNode)) st.selNode = -1;
    bool linkAlive = false;
    for (const BoardLink& l : st.board.links) linkAlive = linkAlive || l.id == st.selLink;
    if (!linkAlive) st.selLink = -1;
    st.dirty = true;
    st.dirtyAt = ImGui::GetTime();
}

bool Undo(AppState& app, BoardViewState& st) {
    CommitHistory(st);
    if (st.undoStack.empty()) {
        app.SetStatus("Nothing to undo");
        return false;
    }
    st.redoStack.push_back(st.board);
    Board previous = std::move(st.undoStack.back());
    st.undoStack.pop_back();
    RestoreBoard(st, std::move(previous));
    return true;
}

bool Redo(AppState& app, BoardViewState& st) {
    CommitHistory(st);
    if (st.redoStack.empty()) {
        app.SetStatus("Nothing to redo");
        return false;
    }
    st.undoStack.push_back(st.board);
    Board next = std::move(st.redoStack.back());
    st.redoStack.pop_back();
    RestoreBoard(st, std::move(next));
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Pasting a picture
std::wstring StampedPastePath() {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    wchar_t name[64];
    swprintf_s(name, L"pasted_%04d%02d%02d_%02d%02d%02d.png", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    std::error_code ec;
    const std::wstring dir = BoardsDir() + L"\\pasted";
    fs::create_directories(dir, ec);
    std::wstring path = dir + L"\\" + name;
    for (int i = 2; fs::exists(path, ec) && i < 1000; ++i) {
        wchar_t alt[96];
        swprintf_s(alt, L"%s\\pasted_%04d%02d%02d_%02d%02d%02d_%d.png", dir.c_str(), tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour,
                   tmv.tm_min, tmv.tm_sec, i);
        path = alt;
    }
    return path;
}

bool IsImagePath(const std::wstring& p) {
    const std::wstring ext = fs::path(p).extension().wstring();
    for (const wchar_t* e : {L".png", L".jpg", L".jpeg", L".webp", L".bmp", L".gif", L".tif", L".tiff", L".avif"}) {
        if (_wcsicmp(ext.c_str(), e) == 0) return true;
    }
    return false;
}

void StbWrite(void* ctx, void* data, int size) { static_cast<std::string*>(ctx)->append(static_cast<const char*>(data), static_cast<size_t>(size)); }

// Picture on the clipboard: a copied file, PNG bytes (browsers), or a bitmap. Returns the file to use (empty = none).
std::wstring PasteImage(std::string& message) {
    std::wstring result;
    if (!OpenClipboard(GetActiveWindow())) {
        message = "Could not open the clipboard";
        return result;
    }
    if (HANDLE h = GetClipboardData(CF_HDROP)) {
        HDROP drop = static_cast<HDROP>(h);
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count && result.empty(); ++i) {
            wchar_t buf[MAX_PATH * 2];
            if (DragQueryFileW(drop, i, buf, MAX_PATH * 2) && IsImagePath(buf)) result = buf;
        }
    }
    if (result.empty()) {
        static const UINT png = RegisterClipboardFormatW(L"PNG");
        if (HANDLE h = GetClipboardData(png)) {
            const size_t size = GlobalSize(h);
            if (const void* p = GlobalLock(h)) {
                const std::wstring path = StampedPastePath();
                std::ofstream out(path, std::ios::binary);
                if (out) {
                    out.write(static_cast<const char*>(p), static_cast<std::streamsize>(size));
                    result = path;
                }
                GlobalUnlock(h);
            }
        }
    }
    if (result.empty()) {
        HANDLE h = GetClipboardData(CF_DIB);
        if (h) {
            if (const unsigned char* p = static_cast<const unsigned char*>(GlobalLock(h))) {
                const BITMAPINFOHEADER* bih = reinterpret_cast<const BITMAPINFOHEADER*>(p);
                const int w = bih->biWidth, ah = std::abs(bih->biHeight), bpp = bih->biBitCount;
                if (w > 0 && ah > 0 && (bpp == 24 || bpp == 32) && (bih->biCompression == BI_RGB || bih->biCompression == BI_BITFIELDS)) {
                    size_t offset = bih->biSize + (bih->biCompression == BI_BITFIELDS && bih->biSize == sizeof(BITMAPINFOHEADER) ? 12 : 0);
                    const size_t stride = ((static_cast<size_t>(w) * bpp + 31) / 32) * 4;
                    std::vector<unsigned char> rgba(static_cast<size_t>(w) * ah * 4);
                    bool anyAlpha = false;
                    for (int y = 0; y < ah; ++y) {
                        const int srcY = bih->biHeight > 0 ? ah - 1 - y : y;
                        const unsigned char* row = p + offset + stride * srcY;
                        for (int x = 0; x < w; ++x) {
                            const unsigned char* s = row + x * (bpp / 8);
                            unsigned char* d = &rgba[(static_cast<size_t>(y) * w + x) * 4];
                            d[0] = s[2];
                            d[1] = s[1];
                            d[2] = s[0];
                            d[3] = bpp == 32 ? s[3] : 255;
                            anyAlpha = anyAlpha || d[3] != 0;
                        }
                    }
                    if (!anyAlpha) {
                        for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
                    }
                    std::string png;
                    if (stbi_write_png_to_func(StbWrite, &png, w, ah, 4, rgba.data(), w * 4)) {
                        const std::wstring path = StampedPastePath();
                        std::ofstream out(path, std::ios::binary);
                        if (out) {
                            out.write(png.data(), static_cast<std::streamsize>(png.size()));
                            result = path;
                        }
                    }
                }
                GlobalUnlock(h);
            }
        }
    }
    CloseClipboard();
    if (result.empty() && message.empty()) message = "There is no picture on the clipboard";
    return result;
}

// ---------------------------------------------------------------------------------------------------------------
// Adding and describing cards
const char* RoleInstruction(BoardKind k) {
    switch (k) {
        case BoardKind::Character:
            return "Describe ONLY the main character (person, creature or figure) in this image, as a reference for writing a scene prompt. In 2-3 "
                   "sentences give: species/gender/age, hair, face, build, clothing and accessories, plus their usual expression. Do not describe "
                   "the background and do not describe what they are doing. No preamble.";
        case BoardKind::Background:
            return "Describe ONLY the setting of this image, as a reference for writing a scene prompt: the place, time of day, weather, lighting, "
                   "colours, mood and the main objects in it. Ignore any people or characters. 2-3 sentences, no preamble.";
        case BoardKind::Item:
            return "Describe ONLY the main object in this image, as a reference for writing a scene prompt: what it is, shape, size, material, "
                   "colours and any distinctive details. 1-2 sentences, no preamble.";
        default: return "Describe this image in 2 sentences.";
    }
}

bool ModelReady(AppState& app) {
    if (!app.visionModel.empty()) return true;
    app.SetStatus("Pick a vision model first (Vision prompt window)");
    app.showVisionWindow = true;
    return false;
}

void StartDescribe(AppState& app, BoardViewState& st, int id) {
    BoardNode* n = st.board.Find(id);
    if (!n || n->image.empty() || n->describing || !ModelReady(app)) return;
    n->describing = true;
    VisionService::ComposeRequest rq;
    rq.tag = "node:" + std::to_string(id);
    rq.prompt = RoleInstruction(n->kind);
    rq.images = {n->image};
    rq.numPredict = 300;
    rq.temperature = 0.3;
    rq.maxImageSide = 768;
    app.vision.Compose(app.VisionEndpoint(), app.visionModel, app.visionKeepAlive, std::move(rq));
}

const AppState::VisionLibItem* FindLibItem(const AppState& app, const std::wstring& path) {
    for (const AppState::VisionLibItem& it : app.visionLib) {
        if (_wcsicmp(it.imagePath.c_str(), path.c_str()) == 0) return &it;
    }
    return nullptr;
}

// The prompt a picture already has: its Vision result in the library, or the .txt / .tags.txt that sits next to it.
std::string KnownPrompt(const AppState& app, const std::wstring& path) {
    if (const AppState::VisionLibItem* it = FindLibItem(app, path)) {
        if (!it->prompt.empty()) return it->prompt;
        if (!it->tags.empty()) return it->tags;
    }
    fs::path p(path);
    for (const wchar_t* ext : {L".txt", L".tags.txt"}) {
        fs::path t = p;
        t.replace_extension(ext);
        std::ifstream f(t, std::ios::binary);
        if (!f) continue;
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (text.size() > 8000) text.resize(8000);
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
        size_t a = 0;
        while (a < text.size() && std::isspace(static_cast<unsigned char>(text[a]))) ++a;
        if (a < text.size()) return text.substr(a);
    }
    return {};
}

int AddPicture(AppState& app, BoardViewState& st, const std::wstring& path, BoardKind kind, float bx, float by, const std::string& title = std::string(),
               const std::string& text = std::string()) {
    for (const BoardNode& o : st.board.nodes) { // do not stack the new card exactly on another one
        if (std::fabs(o.x - bx) < 20 && std::fabs(o.y - by) < 20) {
            bx += 36;
            by += 36;
        }
    }
    const int id = st.board.Add(kind, path, std::string(), bx, by, st.curScene);
    st.selNode = id;
    st.selLink = -1;
    BoardNode* n = st.board.Find(id);
    n->title = title;
    n->text = text;
    if (kind == BoardKind::Character && !path.empty()) EnsureProfileOf(st.board, id); // its own inventory (a saved character brings its saved one)
    n = st.board.Find(id);
    if (kind == BoardKind::Character) { // the same picture is the same character everywhere: bring its name and description along
        if (const CharacterProfile* p = FindProfile(path)) {
            if (title.empty()) n->title = p->name;
            if (text.empty()) n->text = p->description;
        } else {
            EnsureProfile(path);
        }
    }
    if (!n->text.empty()) {
        // a saved character already says who it is
    } else if (const std::string known = KnownPrompt(app, path); !known.empty()) {
        n->text = known; // already described (a Vision result sits next to the picture): never ask the model again
    } else if (st.autoDescribe && !app.visionModel.empty()) {
        StartDescribe(app, st, id);
    }
    Touch(st);
    return id;
}

// ---------------------------------------------------------------------------------------------------------------
// Built-in icons, scenes and "My icons" (your own pictures kept for reuse)
std::string LowerCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

const BuiltinIcon* FindIcon(const std::string& id) {
    for (const BuiltinIcon& i : BuiltinIcons())
        if (i.id == id) return &i;
    return nullptr;
}

struct IconRef {
    bool ok = false;
    std::string name, category, desc;
    std::wstring path;
};

// your own pictures: everything in the items folder (inventory pictures, "+ My image...")
const std::vector<SavedItem>& MyIcons(bool refresh = false) {
    static std::vector<SavedItem> list;
    static double at = -100.0;
    if (refresh || ImGui::GetTime() - at > 3.0) {
        list = ListSavedItems();
        at = ImGui::GetTime();
    }
    return list;
}

// An icon id is a built-in id ("backpack", "bg_forest") or "my:<file path>" for one of your own pictures.
IconRef ResolveIcon(const std::string& id) {
    IconRef r;
    if (id.rfind("my:", 0) == 0) {
        r.path = ToWide(id.substr(3));
        r.category = "My icons";
        for (const SavedItem& s : MyIcons()) {
            if (_wcsicmp(s.path.c_str(), r.path.c_str()) == 0) r.name = s.name;
        }
        if (r.name.empty()) r.name = ToUtf8(fs::path(r.path).stem().wstring());
        std::error_code ec;
        r.ok = fs::exists(r.path, ec);
        return r;
    }
    if (const BuiltinIcon* ic = FindIcon(id)) {
        r.path = BuiltinIconImage(id);
        r.name = ic->name;
        r.category = ic->category;
        r.desc = ic->desc;
        r.ok = !r.path.empty();
    }
    return r;
}

InvSlot SlotForCategory(const std::string& cat) {
    if (cat == "Clothes") return InvSlot::Wearing;
    if (cat == "Bags") return InvSlot::Carrying;
    if (cat == "Vehicles") return InvSlot::Vehicle;
    return InvSlot::Holding;
}

// A searchable grid. mode 0: items (clothes, bags, tools...) + my icons; 1: scenes (backgrounds) + my icons; 2: everything.
// Returns the id picked ("" = none): by a click, or a double-click when `doubleClick` is set. Every cell can also be dragged
// out (payload MV_BOARDICON). "+ My image" adds your own picture to "My icons"; right click one of those to rename or remove it.
std::string IconGrid(AppState& app, BoardViewState& st, int mode, float width, float height, bool doubleClick) {
    static int catByMode[3] = {0, 0, 0};
    std::vector<const char*> cats;
    if (mode == 1) {
        cats = {"All scenes", "My icons"};
    } else {
        cats = {"All", "Clothes", "Bags", "Vehicles", "Weapons & tools", "Everyday"};
        if (mode == 2) cats.push_back("Backgrounds");
        cats.push_back("My icons");
    }
    int& category = catByMode[std::clamp(mode, 0, 2)];
    if (category < 0 || category >= static_cast<int>(cats.size())) category = 0;
    std::string picked;
    InputLine("##iconsearch", mode == 1 ? "Search scenes" : "Search icons", st.iconSearch, width * 0.45f);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::Combo("##iconcat", &category, cats.data(), static_cast<int>(cats.size()));
    if (ImGui::SmallButton("+ My image...")) {
        const std::wstring file = OpenImageFileDialog(GetActiveWindow());
        if (!file.empty() && !StoreItemImage(file, ToUtf8(fs::path(file).stem().wstring())).empty()) {
            MyIcons(true);
            category = static_cast<int>(cats.size()) - 1;
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add one of your own pictures as an icon you can reuse on any board");
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Paste as icon")) {
        std::string msg;
        const std::wstring pasted = PasteImage(msg);
        if (!pasted.empty() && !StoreItemImage(pasted, "my icon").empty()) {
            MyIcons(true);
            category = static_cast<int>(cats.size()) - 1;
        } else if (pasted.empty()) {
            app.SetStatus(msg);
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy a picture (from a browser, anywhere), then paste it here");

    struct Entry {
        std::string id, name;
        std::wstring path;
        bool custom = false, wide = false;
    };
    std::vector<Entry> entries;
    const std::string q = LowerCopy(st.iconSearch);
    const std::string catName = cats[category];
    const bool allCat = catName == "All" || catName == "All scenes";
    auto matches = [&](const std::string& name, const std::string& id) {
        return q.empty() || LowerCopy(name).find(q) != std::string::npos || LowerCopy(id).find(q) != std::string::npos;
    };
    for (const BuiltinIcon& ic : BuiltinIcons()) {
        const bool scene = ic.category == "Backgrounds";
        if ((mode == 0 && scene) || (mode == 1 && !scene)) continue;
        if (!allCat && catName != ic.category) continue;
        if (!matches(ic.name, ic.id)) continue;
        entries.push_back({ic.id, ic.name, std::wstring(), false, scene});
    }
    if (allCat || catName == "My icons") {
        for (const SavedItem& s : MyIcons()) {
            if (matches(s.name, s.name)) entries.push_back({"my:" + ToUtf8(s.path), s.name, s.path, true, false});
        }
    }

    ImGui::BeginChild("##icongrid", ImVec2(width, height), true);
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float availW = ImGui::GetContentRegionAvail().x;
    static std::set<std::string> ready;
    static int budgetFrame = -1, budget = 0;
    if (ImGui::GetFrameCount() != budgetFrame) { // draw a few new icons per frame so the first opening does not stall
        budgetFrame = ImGui::GetFrameCount();
        budget = 8;
    }
    float lineX = 0;
    bool first = true;
    for (Entry& e : entries) {
        const float cw = e.wide ? ui::Dp(96.0f) : ui::Dp(62.0f), ch = e.wide ? ui::Dp(54.0f) : ui::Dp(62.0f);
        if (!first) {
            if (lineX + spacing + cw <= availW) {
                ImGui::SameLine();
                lineX += spacing;
            } else {
                lineX = 0;
            }
        }
        first = false;
        lineX += cw;
        ImGui::PushID(e.id.c_str());
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::InvisibleButton("##i", ImVec2(cw, ch));
        const bool hov = ImGui::IsItemHovered();
        std::wstring path = e.path;
        if (!e.custom) {
            if (ready.count(e.id) || budget > 0) {
                if (!ready.count(e.id)) --budget;
                path = BuiltinIconImage(e.id);
                ready.insert(e.id);
            }
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        DrawThumbAt(app, dl, path, pos, ImVec2(pos.x + cw, pos.y + ch), 5.0f);
        if (hov) dl->AddRect(pos, ImVec2(pos.x + cw, pos.y + ch), ImGui::GetColorU32(ui::kAccent), 5.0f, 0, 2.0f);
        if (hov) ImGui::SetTooltip("%s", e.name.c_str());
        if (doubleClick ? (hov && ImGui::IsMouseDoubleClicked(0)) : clicked) picked = e.id;
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
            ImGui::SetDragDropPayload("MV_BOARDICON", e.id.c_str(), e.id.size() + 1);
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(e.name.c_str());
            ImGui::TextDisabled("Drop it on a character for its inventory, or on the board");
            ImGui::EndTooltip();
            ImGui::EndDragDropSource();
        }
        if (e.custom && ImGui::BeginPopupContextItem("##myctx")) {
            static std::string renameBuf;
            if (ImGui::IsWindowAppearing()) renameBuf = e.name;
            InputLine("##myrename", "name", renameBuf, ui::Dp(200.0f));
            if (ImGui::MenuItem("Rename")) {
                RenameSavedItem(e.path, renameBuf);
                MyIcons(true);
            }
            if (ImGui::MenuItem("Remove from my icons (Recycle Bin)")) {
                std::wstring err;
                DeleteFilesToRecycleBin(GetActiveWindow(), {e.path}, &err);
                MyIcons(true);
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (entries.empty()) ImGui::TextDisabled(catName == "My icons" ? "Nothing here yet: press \"+ My image...\" or \"+ Paste as icon\"." : "No match.");
    ImGui::EndChild();
    return picked;
}

void AddInvIcon(BoardViewState& st, CharacterProfile& p, const std::string& id) {
    const IconRef r = ResolveIcon(id);
    if (!r.ok) return;
    p.AddItem(SlotForCategory(r.category), LowerCopy(r.name), r.path);
    TouchProfiles(st);
}

// an icon or scene as a card on the board (scenes become Background cards, the rest Items)
int AddIconCard(AppState& app, BoardViewState& st, const std::string& id, float bx, float by) {
    const IconRef r = ResolveIcon(id);
    if (!r.ok) return -1;
    const BoardKind kind = r.category == "Backgrounds" ? BoardKind::Background : BoardKind::Item;
    return AddPicture(app, st, r.path, kind, bx, by, r.name, r.desc.empty() ? LowerCopy(r.name) : r.desc);
}

// Another picture for a card: a character becomes someone else (name, description and their own inventory come with them, relations and
// place stay), an item / setting just changes picture.
void ReplaceCharacter(AppState& app, BoardViewState& st, int id, const std::wstring& newImage, bool keepGear) {
    CrashNote("board: swapping a character");
    BoardNode* n = st.board.Find(id);
    if (!n || n->kind != BoardKind::Character || n->image.empty() || newImage.empty()) return;
    const std::wstring old = n->image;
    if (_wcsicmp(old.c_str(), newImage.c_str()) == 0) return;
    std::vector<InvItem> gear;
    int nextGear = 1;
    if (const CharacterProfile* p = ProfileOf(*n)) {
        gear = p->items;
        nextGear = p->nextItemId;
    }
    const CharacterProfile* tpl = FindProfile(newImage);
    const std::string title = tpl && !tpl->name.empty() ? tpl->name : ToUtf8(fs::path(newImage).stem().wstring());
    std::string text = tpl ? tpl->description : std::string();
    if (text.empty()) text = KnownPrompt(app, newImage);
    for (BoardNode& o : st.board.nodes) { // every card of this character (the other beats too)
        if (o.kind != BoardKind::Character || _wcsicmp(o.image.c_str(), old.c_str()) != 0) continue;
        o.image = newImage;
        o.title = title;
        o.text = text;
        o.profile.clear();
    }
    CharacterProfile& np = EnsureProfileOf(st.board, id);
    const std::string key = np.key;
    if (keepGear && np.items.empty() && !gear.empty()) {
        np.items = gear;
        np.nextItemId = nextGear;
    }
    for (BoardNode& o : st.board.nodes) {
        if (o.kind == BoardKind::Character && _wcsicmp(o.image.c_str(), newImage.c_str()) == 0) o.profile = key;
    }
    SaveProfiles();
    st.selNode = id;
    if (text.empty() && st.autoDescribe && !app.visionModel.empty()) StartDescribe(app, st, id);
    Touch(st);
    app.SetStatus("Swapped in " + title);
}

void SwapCardPicture(AppState& app, BoardViewState& st, int id, const std::wstring& newImage) {
    CrashNote("board: swapping a card's picture");
    BoardNode* n = st.board.Find(id);
    if (!n || n->kind == BoardKind::Note || newImage.empty()) return;
    if (n->kind == BoardKind::Character) {
        ReplaceCharacter(app, st, id, newImage, false);
        return;
    }
    n->image = newImage;
    n->title.clear();
    n->text = KnownPrompt(app, newImage);
    if (n->text.empty()) n->text = LowerCopy(ToUtf8(fs::path(newImage).stem().wstring()));
    st.selNode = id;
    Touch(st);
    app.SetStatus("Swapped the picture");
}

// A built-in icon dropped on an item / setting card
void SwapCardIcon(AppState& app, BoardViewState& st, int id, const std::string& iconId) {
    BoardNode* n = st.board.Find(id);
    const IconRef r = ResolveIcon(iconId);
    if (!n || !r.ok || n->kind == BoardKind::Note) return;
    n->image = r.path;
    n->title = r.name;
    n->text = r.desc.empty() ? LowerCopy(r.name) : r.desc;
    st.selNode = id;
    Touch(st);
    app.SetStatus("Swapped the picture");
}

std::string IconIdOfPath(const std::wstring& path) {
    if (path.empty()) return {};
    if (path.find(L"icons-v1\\") != std::wstring::npos) {
        const std::wstring stem = fs::path(path).stem().wstring();
        return std::string(stem.begin(), stem.end());
    }
    if (_wcsicmp(fs::path(path).parent_path().wstring().c_str(), ItemsDir().c_str()) == 0) return "my:" + ToUtf8(path);
    return {};
}

// A drop-down list of icons / scenes (with search and categories). Returns the id chosen this frame ("" = none).
std::string IconCombo(AppState& app, BoardViewState& st, const char* id, const std::string& currentId, float width, int mode = 0) {
    std::string picked;
    const IconRef cur = currentId.empty() ? IconRef() : ResolveIcon(currentId);
    ImGui::SetNextItemWidth(width);
    ImGui::SetNextWindowSizeConstraints(ImVec2(ui::Dp(380.0f), 0), ImVec2(FLT_MAX, ui::Dp(480.0f)));
    if (ImGui::BeginCombo(id, cur.ok ? cur.name.c_str() : (mode == 1 ? "Choose a scene..." : "Choose an icon..."), ImGuiComboFlags_HeightLargest)) {
        picked = IconGrid(app, st, mode, ui::Dp(360.0f), ui::Dp(280.0f), false);
        if (!picked.empty()) ImGui::CloseCurrentPopup();
        ImGui::EndCombo();
    }
    return picked;
}

// ---------------------------------------------------------------------------------------------------------------
// Canvas
struct View {
    ImVec2 p0;
    float sc, panX, panY;
    ImVec2 ToScreen(float x, float y) const { return ImVec2(p0.x + (x - panX) * sc, p0.y + (y - panY) * sc); }
    ImVec2 ToBoard(ImVec2 s) const { return ImVec2((s.x - p0.x) / sc + panX, (s.y - p0.y) / sc + panY); }
};

struct Geo {
    float h = 0, imgH = 0, textH = 0, stripH = 0;
    AppState::LibThumb thumb;
    const CharacterProfile* profile = nullptr;
};

constexpr float kStripH = 34.0f;

Geo NodeGeo(AppState& app, const BoardNode& n) {
    Geo g;
    const float imgW = n.w - 2 * kPad;
    if (n.kind == BoardKind::Note) {
        g.textH = kLine * 6 + 4;
    } else {
        g.thumb = n.image.empty() ? AppState::LibThumb{} : app.LibraryThumb(n.image);
        const float aspect = g.thumb.w > 0 ? static_cast<float>(g.thumb.h) / g.thumb.w : 0.75f;
        g.imgH = std::clamp(imgW * aspect, 70.0f, imgW * 1.1f);
        g.textH = kLine * kTextLines + 4;
        if (n.kind == BoardKind::Character) {
            g.profile = ProfileOf(n);
            if (!n.image.empty()) g.stripH = kStripH;
        }
    }
    if (n.textH > 0) g.textH = n.textH; // resized by hand
    g.h = kHeaderH + (g.imgH > 0 ? g.imgH + kPad : kPad) + (g.stripH > 0 ? g.stripH + kPad : 0.0f) + g.textH + kPad;
    return g;
}

struct LinkGeo {
    ImVec2 a, b, c1, c2, mid;
    ImVec2 chipMin, chipMax;
    bool valid = false;
};

ImVec2 Bez(const LinkGeo& g, float t) {
    const float u = 1 - t;
    return ImVec2(u * u * u * g.a.x + 3 * u * u * t * g.c1.x + 3 * u * t * t * g.c2.x + t * t * t * g.b.x,
                  u * u * u * g.a.y + 3 * u * u * t * g.c1.y + 3 * u * t * t * g.c2.y + t * t * t * g.b.y);
}

void AddTextClipped(ImDrawList* dl, float fs, ImVec2 pos, ImU32 col, const std::string& text, const ImVec4& clip, float wrap = 0.0f) {
    if (text.empty()) return;
    pos.x = std::round(pos.x); // whole pixels keep the glyphs crisp
    pos.y = std::round(pos.y);
    dl->AddText(ImGui::GetFont(), fs, pos, col, text.c_str(), text.c_str() + text.size(), wrap, &clip);
}

float TextWidth(float fs, const std::string& s) { return ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.0f, s.c_str(), s.c_str() + s.size()).x; }

// font size for text that follows the zoom: native size (sharp) while that fits, scaled otherwise
float FontFor(float sc) {
    const float native = ImGui::GetFontSize();
    const float want = kFontUnits * sc;
    return (want >= native * 0.85f && want <= native * 1.6f) ? native : want;
}

void FitView(Board& b, ImVec2 size, float dp, int scene) {
    bool any = false;
    for (const BoardNode& n : b.nodes) any = any || n.scene == scene;
    if (!any) {
        b.panX = b.panY = 0;
        b.zoom = 1;
        return;
    }
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (const BoardNode& n : b.nodes) {
        if (n.scene != scene) continue;
        x0 = std::min(x0, n.x);
        y0 = std::min(y0, n.y);
        x1 = std::max(x1, n.x + n.w);
        y1 = std::max(y1, n.y + 330.0f);
    }
    const float margin = 40.0f;
    const float sc = std::min((size.x - margin) / std::max(1.0f, x1 - x0), (size.y - margin) / std::max(1.0f, y1 - y0));
    b.zoom = std::clamp(sc / dp, 0.15f, 1.2f);
    const float s = b.zoom * dp;
    b.panX = (x0 + x1) * 0.5f - size.x * 0.5f / s;
    b.panY = (y0 + y1) * 0.5f - size.y * 0.5f / s;
}

// ---- relations: the built-in ones and yours, in one list or grouped by category ----
std::string g_relComfyUrl = "http://127.0.0.1:8188"; // where ComfyUI is, for Clef's GPU turn-taking during an import (set each frame)
// True when one was picked (its text in `picked`).
bool RelationList(BoardViewState& st, BoardKind from, BoardKind to, const std::string& current, std::string& picked) {
    const std::vector<RelationEntry> all = BoardRelations(from, to);
    bool any = false;
    std::string removeLabel;
    std::string cat;

    // type to find: every word has to be in the action or its category. Enter takes the first match; with no match it adds what you typed as a new action of your own.
    static std::string filter;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    const bool enter = ImGui::InputTextWithHint("##relfilter", "type to find an action...", filter.data(), filter.capacity() + 1,
                                                ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_EnterReturnsTrue, StdStringResize, &filter);
    std::vector<std::string> words;
    {
        std::string low = Lower(filter), w;
        for (char c : low + " ") {
            if (c == ' ') {
                if (!w.empty()) words.push_back(w);
                w.clear();
            } else {
                w += c;
            }
        }
    }
    const bool searching = !words.empty();
    int matches = 0;
    std::string firstMatch;
    for (const RelationEntry& e : all) {
        if (searching) {
            const std::string hay = Lower(e.label + " " + e.category);
            bool ok = true;
            for (const std::string& w : words) ok = ok && hay.find(w) != std::string::npos;
            if (!ok) continue;
        } else if (st.relGroups && e.category != cat) {
            cat = e.category;
            ImGui::Spacing();
            ImGui::TextColored(ui::kTextDim, "%s", cat.c_str());
        }
        ++matches;
        if (firstMatch.empty()) firstMatch = e.label;
        ImGui::PushID(e.label.c_str());
        if (ImGui::Selectable(e.label.c_str(), current == e.label)) {
            picked = e.label;
            any = true;
        }
        if (searching) { // which category it is in, on the right
            const ImVec2 ts = ImGui::CalcTextSize(e.category.c_str());
            ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 8.0f, ImGui::GetWindowContentRegionMax().x - ts.x - 4.0f));
            ImGui::TextColored(ui::kTextDim, "%s", e.category.c_str());
        }
        if (e.mine && ImGui::BeginPopupContextItem("##myrel")) {
            if (ImGui::MenuItem("Remove from my relations")) removeLabel = e.label;
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (searching && matches == 0) {
        std::string typed = filter;
        while (!typed.empty() && typed.back() == ' ') typed.pop_back();
        if (ImGui::Selectable(("Add \"" + typed + "\" as an action of my own").c_str())) {
            picked = typed;
            any = true;
            AddMyRelation(from, to, typed, "My own");
        }
    }
    if (enter && searching && !any) {
        if (matches > 0) {
            picked = firstMatch;
        } else {
            picked = filter;
            while (!picked.empty() && picked.back() == ' ') picked.pop_back();
            AddMyRelation(from, to, picked, "My own");
        }
        any = !picked.empty();
    }
    if (any) filter.clear();
    if (!removeLabel.empty()) RemoveMyRelation(removeLabel);
    return any;
}

// "or type your own", saved for next time (optionally under a category), and the switch for grouping.
bool RelationCustom(BoardViewState& st, BoardKind from, BoardKind to, std::string& picked) {
    static std::string text, newCategory;
    static bool remember = true;
    static int categoryIdx = -1; // -1 = "My own" (looked up below: the list of categories grows)
    bool any = false;
    ImGui::SetNextItemWidth(ui::Dp(260.0f));
    const bool enter = ImGui::InputTextWithHint("##relcustom", "or type your own, then Enter", text.data(), text.capacity() + 1,
                                                ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_EnterReturnsTrue, StdStringResize, &text);
    const std::vector<std::string> cats = RelationCategories();
    if (categoryIdx < 0) {
        const auto mine = std::find(cats.begin(), cats.end(), std::string("My own"));
        categoryIdx = mine == cats.end() ? 0 : static_cast<int>(mine - cats.begin());
    }
    categoryIdx = std::clamp(categoryIdx, 0, static_cast<int>(cats.size()));
    ImGui::Checkbox("Save it for next time", &remember);
    if (remember) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ui::Dp(150.0f));
        const char* preview = categoryIdx < static_cast<int>(cats.size()) ? cats[categoryIdx].c_str() : "New category...";
        if (ImGui::BeginCombo("##relcat", preview)) {
            for (size_t i = 0; i < cats.size(); ++i) {
                if (ImGui::Selectable(cats[i].c_str(), static_cast<int>(i) == categoryIdx)) categoryIdx = static_cast<int>(i);
            }
            if (ImGui::Selectable("New category...", categoryIdx >= static_cast<int>(cats.size()))) categoryIdx = static_cast<int>(cats.size());
            ImGui::EndCombo();
        }
        if (categoryIdx >= static_cast<int>(cats.size())) {
            ImGui::SetNextItemWidth(ui::Dp(200.0f));
            ImGui::InputTextWithHint("##relnewcat", "category name (couple, funny, rivals...)", newCategory.data(), newCategory.capacity() + 1, ImGuiInputTextFlags_CallbackResize, StdStringResize, &newCategory);
        }
    }
    if (ImGui::Checkbox("Group by category", &st.relGroups)) SaveBoardSettings(st);
    ImGui::SameLine();
    ImGui::BeginDisabled(RelationImportRunning());
    if (ImGui::SmallButton("Import from a .txt file...")) {
        wchar_t buf[MAX_PATH * 2] = {};
        OPENFILENAMEW ofn = {sizeof(ofn)};
        ofn.hwndOwner = GetActiveWindow();
        ofn.lpstrFilter = L"Text files\0*.txt\0All files\0*.*\0\0";
        ofn.lpstrFile = buf;
        ofn.nMaxFile = MAX_PATH * 2;
        ofn.lpstrTitle = L"Choose a text file with one action per line (laughs with, hangs out with...)";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        wchar_t testFile[MAX_PATH * 2] = {};
        const DWORD tn = GetEnvironmentVariableW(L"MV_RELATION_FILE", testFile, MAX_PATH * 2); // (for tests: skips the dialog)
        if (tn > 0 && tn < MAX_PATH * 2) RelationImportStart(testFile, g_relComfyUrl);
        else if (GetOpenFileNameW(&ofn)) RelationImportStart(buf, g_relComfyUrl);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("One action per line, for example:\n  laughs with\n  hangs out with\nClef reads each one and sorts it into a category by himself (Fighting, Couple, Funny, Adventure...).\nLines that are already in the library are skipped.");
    {
        const std::string status = RelationImportStatus();
        if (!status.empty()) {
            ImGui::PushTextWrapPos(ui::Dp(420.0f));
            ImGui::TextColored(RelationImportRunning() ? ui::kWarn : ui::kGood, "%s", status.c_str());
            ImGui::PopTextWrapPos();
        }
    }
    if (enter && !text.empty()) {
        picked = text;
        any = true;
        if (remember) AddMyRelation(from, to, text, categoryIdx < static_cast<int>(cats.size()) ? cats[categoryIdx] : newCategory);
        text.clear();
    }
    return any;
}

void RelationMenu(BoardViewState& st, BoardLink& l) {
    const BoardNode *f = st.board.Find(l.from), *t = st.board.Find(l.to);
    if (!f || !t) return;
    std::string picked;
    if (RelationList(st, f->kind, t->kind, l.label, picked)) {
        l.label = picked;
        Touch(st);
    }
    ImGui::Separator();
    if (RelationCustom(st, f->kind, t->kind, picked)) {
        l.label = picked;
        Touch(st);
    }
}

// A card made out of thin air: at the end of a dragged link (joined to the card the link came from), or on its own
// when the panel was opened by a double-click on the empty board (airFrom < 0).
void CreateLinked(AppState& app, BoardViewState& st, BoardKind kind, const std::wstring& image, const std::string& title = std::string(),
                  const std::string& text = std::string()) {
    Board& b = st.board;
    const bool linked = st.airFrom >= 0 && b.Find(st.airFrom) != nullptr;
    const float x = !linked ? st.airX - 115.0f : st.airOutput ? st.airX : st.airX - 230.0f;
    int id;
    if (image.empty()) {
        id = b.Add(kind, std::wstring(), std::string(), x, st.airY - 40, st.curScene);
        st.selNode = id;
        Touch(st);
    } else {
        id = AddPicture(app, st, image, kind, x, st.airY - 40, title, !text.empty() ? text : (title.empty() ? std::string() : LowerCopy(title)));
    }
    if (!linked) return;
    const int from = st.airOutput ? st.airFrom : id, to = st.airOutput ? id : st.airFrom;
    const int made = b.Connect(from, to);
    if (made >= 0) {
        st.selLink = made;
        st.selNode = -1;
        st.relPickLink = made;
        st.openRelPick = true;
    }
    Touch(st);
}

// The panel shown when a link is dropped on empty canvas: what could be added there, already linked to the card it came from.
// (A plain window rather than a popup: it stays until an option is taken, Esc is pressed or you click elsewhere.)
static void AirLog(const char* what) { // TEMP-DEBUG
    const char* tmp = std::getenv("TEMP");
    const std::string path = std::string(tmp ? tmp : ".") + "/air.log";
    if (FILE* f = std::fopen(path.c_str(), "a")) {
        std::fprintf(f, "%d %s\n", ImGui::GetFrameCount(), what);
        std::fclose(f);
    }
}

void DrawAirPanel(AppState& app, BoardViewState& st) {
    if (!st.airOpen) return;
    Board& b = st.board;
    const BoardNode* src = st.airFrom >= 0 ? b.Find(st.airFrom) : nullptr;
    if (st.airFrom >= 0 && !src) {
        AirLog("closed: source card missing");
        st.airOpen = false;
        return;
    }
    {
        // keep the whole panel on screen: put it near the mouse, shifted up / left as far as needed
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        const float panelW = ui::Dp(450.0f), panelH = std::min(ui::Dp(520.0f), disp.y - ui::Dp(20.0f));
        const float x = std::clamp(st.airScreenX, 10.0f, std::max(10.0f, disp.x - panelW - 10.0f));
        const float y = std::clamp(st.airScreenY, 10.0f, std::max(10.0f, disp.y - panelH - 10.0f));
        ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, panelH));
    }
    ImGui::SetNextWindowFocus();
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove;
    bool done = false;
    if (ImGui::Begin("##airpanel", nullptr, flags)) {
        if (src) ImGui::Text("Add something to %s", b.DisplayName(*src).c_str());
        else ImGui::TextUnformatted("Add something to this board");
        ImGui::SameLine(ui::Dp(380.0f));
        if (ImGui::SmallButton("Close")) done = true;
        struct Tab {
            int id; // 0-3 a new card of that kind, 4 link to the card it was dropped on, 5 the character's inventory
            const char* label;
            uint32_t color;
        };
        const BoardNode* target = st.airTarget >= 0 ? b.Find(st.airTarget) : nullptr;
        const bool inventoryOk = src && src->kind == BoardKind::Character && !src->image.empty();
        std::vector<Tab> tabs;
        if (src && target) tabs.push_back({4, "Link", 0x6EE2FF});
        if (inventoryOk) tabs.push_back({5, "Inventory", 0xB05FD0});
        tabs.push_back({0, "Character", KindColor(BoardKind::Character)});
        tabs.push_back({1, "Item", KindColor(BoardKind::Item)});
        tabs.push_back({2, "Background", KindColor(BoardKind::Background)});
        tabs.push_back({3, "Note", KindColor(BoardKind::Note)});
        bool tabFound = false;
        for (const Tab& tb : tabs) tabFound = tabFound || tb.id == st.airKind;
        if (!tabFound) st.airKind = tabs.front().id;
        for (size_t i = 0; i < tabs.size(); ++i) {
            if (i > 0) ImGui::SameLine();
            const bool active = st.airKind == tabs[i].id;
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(Rgb(tabs[i].color, active ? 255 : 90)));
            if (ImGui::Button(tabs[i].label)) st.airKind = tabs[i].id;
            ImGui::PopStyleColor();
        }
        const BoardKind kind = static_cast<BoardKind>(std::clamp(st.airKind, 0, 3));
        if (st.airKind >= 4) {
            ImGui::TextDisabled(st.airKind == 4 ? "Pick what happens between the two cards" : "Add to what this character wears, holds, carries or has nearby");
        } else if (src) {
            const auto& presets = BoardRelationPresets(src->kind, kind);
            ImGui::TextDisabled("Linked as \"%s %s ...\" (you can change it after)", b.DisplayName(*src).c_str(), presets.front());
        } else {
            ImGui::TextDisabled("It is placed where you double-clicked");
        }
        ImGui::Separator();

        if (st.airKind == 4 && src && target) {
            const BoardNode* from = st.airOutput ? src : target;
            const BoardNode* to = st.airOutput ? target : src;
            ImGui::Text("%s  ...  %s", b.DisplayName(*from).c_str(), b.DisplayName(*to).c_str());
            auto connect = [&](const std::string& label) {
                const int made = b.Connect(from->id, to->id, label);
                if (made >= 0) {
                    st.selLink = made;
                    st.selNode = -1;
                    Touch(st);
                } else {
                    app.SetStatus("Those two cards are already connected that way");
                }
                done = true;
            };
            ImGui::BeginChild("##airrel", ImVec2(ui::Dp(420.0f), ui::Dp(210.0f)), true);
            {
                std::string pk;
                if (RelationList(st, from->kind, to->kind, std::string(), pk)) connect(pk);
            }
            ImGui::EndChild();
            {
                std::string pk;
                if (RelationCustom(st, from->kind, to->kind, pk)) connect(pk);
            }
        } else if (st.airKind == 5 && inventoryOk) {
            CharacterProfile& prof = EnsureProfileOf(b, src->id);
            ImGui::Text("Inventory of %s", b.DisplayName(*src).c_str());
            auto addFile = [&](const std::wstring& file, const std::string& name) {
                const std::wstring stored = StoreItemImage(file, name);
                if (!stored.empty()) {
                    prof.AddItem(InvSlot::Holding, name, stored);
                    TouchProfiles(st);
                    st.selNode = src->id;
                    st.invFocus = true;
                    done = true;
                }
            };
            if (ImGui::Button("Picture from the clipboard")) {
                std::string msg;
                const std::wstring pasted = PasteImage(msg);
                if (!pasted.empty()) addFile(pasted, "item");
                else app.SetStatus(msg);
            }
            ImGui::SameLine();
            if (ImGui::Button("Picture file...")) {
                const std::wstring file = OpenImageFileDialog(GetActiveWindow());
                if (!file.empty()) addFile(file, ToUtf8(fs::path(file).stem().wstring()));
            }
            ImGui::SameLine();
            if (ImGui::Button("Blank item")) {
                prof.AddItem(InvSlot::Holding, std::string(), std::wstring());
                TouchProfiles(st);
                st.selNode = src->id;
                st.invFocus = true;
                done = true;
            }
            const std::string pick = IconGrid(app, st, 0, ui::Dp(440.0f), ui::Dp(260.0f), false);
            if (!pick.empty()) {
                AddInvIcon(st, prof, pick);
                st.selNode = src->id;
                st.invFocus = true;
                done = true;
            }
        } else if (kind == BoardKind::Note) {
            if (ImGui::Button("An empty note")) {
                CreateLinked(app, st, kind, std::wstring());
                done = true;
            }
        } else {
            if (kind == BoardKind::Character) {
                ImGui::TextUnformatted("Saved characters");
                bool any = false;
                for (const CharacterProfile& p : AllProfiles()) {
                    if (!p.saved) continue;
                    any = true;
                    const std::string label = (p.name.empty() ? ToUtf8(fs::path(p.image).stem().wstring()) : p.name) + "##saved" + p.key;
                    const ImVec2 pos = ImGui::GetCursorScreenPos();
                    const float h = ui::Dp(30.0f);
                    if (ImGui::Selectable(label.c_str(), false, 0, ImVec2(ui::Dp(250.0f), h))) {
                        CreateLinked(app, st, kind, p.image);
                        done = true;
                    }
                    DrawThumbAt(app, ImGui::GetWindowDrawList(), p.image, ImVec2(pos.x + ui::Dp(256.0f), pos.y), ImVec2(pos.x + ui::Dp(256.0f) + h, pos.y + h));
                }
                if (!any) ImGui::TextDisabled("None yet: select a character card and press Save character.");
                ImGui::Separator();
            }
            if (ImGui::Button("Picture from the clipboard")) {
                std::string msg;
                const std::wstring p = PasteImage(msg);
                if (!p.empty()) {
                    CreateLinked(app, st, kind, p);
                    done = true;
                } else {
                    app.SetStatus(msg);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Picture file...")) {
                const std::wstring p = OpenImageFileDialog(GetActiveWindow());
                if (!p.empty()) {
                    CreateLinked(app, st, kind, p);
                    done = true;
                }
            }
            if (kind == BoardKind::Item || kind == BoardKind::Background) {
                ImGui::TextUnformatted(kind == BoardKind::Item ? "Icons (clothes, bags, cars, tools...) and my icons" : "Built-in scenes and my pictures");
                const std::string pick = IconGrid(app, st, kind == BoardKind::Item ? 0 : 1, ui::Dp(420.0f), ui::Dp(170.0f), false);
                if (!pick.empty()) {
                    const IconRef r = ResolveIcon(pick);
                    if (r.ok) CreateLinked(app, st, kind, r.path, r.name, r.desc);
                    done = true;
                }
            }
            ImGui::TextUnformatted("Vision library");
            app.RequestVisionLibrary();
            ImGui::BeginChild("##airlib", ImVec2(ui::Dp(420.0f), ui::Dp(100.0f)), true);
            const float cell = ui::Dp(56.0f), spacing = ImGui::GetStyle().ItemSpacing.x;
            const int cols = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + spacing) / (cell + spacing)));
            const int shown = std::min(static_cast<int>(app.visionLib.size()), 80);
            for (int i = 0; i < shown; ++i) {
                if (i % cols != 0) ImGui::SameLine();
                ImGui::PushID(i);
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                if (ImGui::InvisibleButton("##a", ImVec2(cell, cell))) {
                    CreateLinked(app, st, kind, app.visionLib[i].imagePath);
                    done = true;
                }
                DrawThumbAt(app, ImGui::GetWindowDrawList(), app.visionLib[i].imagePath, pos, ImVec2(pos.x + cell, pos.y + cell));
                if (ImGui::IsItemHovered()) ImGui::GetWindowDrawList()->AddRect(pos, ImVec2(pos.x + cell, pos.y + cell), ImGui::GetColorU32(ui::kAccent), 3.0f, 0, 2.0f);
                ImGui::PopID();
            }
            if (app.visionLib.empty()) ImGui::TextDisabled("The library is empty.");
            ImGui::EndChild();
        }
        const bool settled = ImGui::GetFrameCount() > st.airOpenFrame + 30; // ignore stray events right after it opens (half a second)
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) && settled) {
            AirLog("esc");
            done = true;
        }
        // a click anywhere outside the panel (and not on one of its popups) closes it
        if (!done && settled && ImGui::IsMouseClicked(0) && !ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByPopup | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
            AirLog("click outside");
            done = true;
        }
    }
    ImGui::End();
    if (done) {
        AirLog("closed: done");
        st.airOpen = false;
    }
}

void DrawCanvas(AppState& app, BoardViewState& st, ImVec2 sz) {
    Board& b = st.board;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float dp = ui::Dp(1.0f);
    if (st.fitRequest && sz.x > 50 && sz.y > 50) {
        FitView(b, sz, dp, st.curScene);
        st.fitRequest = false;
    }
    ImGui::InvisibleButton("##boardcanvas", sz, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    ImGuiIO& io = ImGui::GetIO();

    // zoom around the mouse
    if (hovered && io.MouseWheel != 0.0f) {
        const float before = b.zoom * dp;
        const ImVec2 m(io.MousePos.x - p0.x, io.MousePos.y - p0.y);
        const float wx = b.panX + m.x / before, wy = b.panY + m.y / before;
        b.zoom = std::clamp(b.zoom * std::pow(1.15f, io.MouseWheel), 0.15f, 3.0f);
        const float after = b.zoom * dp;
        b.panX = wx - m.x / after;
        b.panY = wy - m.y / after;
    }
    View v{p0, b.zoom * dp, b.panX, b.panY};
    const float sc = v.sc;
    st.centerX = b.panX + sz.x * 0.5f / sc;
    st.centerY = b.panY + sz.y * 0.5f / sc;

    // geometry of every card and link (the cards stay in order: later = on top); rebuilt whenever the board changes
    std::vector<Geo> geo;
    std::vector<LinkGeo> lg;
    auto indexOf = [&](int id) {
        for (size_t i = 0; i < b.nodes.size(); ++i)
            if (b.nodes[i].id == id) return static_cast<int>(i);
        return -1;
    };
    const float fs = FontFor(sc);
    auto rebuild = [&]() {
        geo.assign(b.nodes.size(), Geo());
        for (size_t i = 0; i < b.nodes.size(); ++i) geo[i] = NodeGeo(app, b.nodes[i]);
        lg.assign(b.links.size(), LinkGeo());
        for (size_t i = 0; i < b.links.size(); ++i) {
            const int fi = indexOf(b.links[i].from), ti = indexOf(b.links[i].to);
            if (fi < 0 || ti < 0 || b.nodes[fi].scene != st.curScene) continue;
            const BoardNode &f = b.nodes[fi], &t = b.nodes[ti];
            LinkGeo& g = lg[i];
            // the line leaves and enters the sides that face each other, so a reversed connection (the "from" card to the right of the
            // "to" card) is a plain curve instead of a loop around both cards
            const float fh = geo[fi].h, th = geo[ti].h, gap = 24.0f;
            ImVec2 da, db; // directions in which the line leaves / enters
            if (f.x + f.w + gap <= t.x) {
                g.a = v.ToScreen(f.x + f.w, f.y + fh * 0.5f);
                g.b = v.ToScreen(t.x, t.y + th * 0.5f);
                da = ImVec2(1, 0);
                db = ImVec2(-1, 0);
            } else if (t.x + t.w + gap <= f.x) {
                g.a = v.ToScreen(f.x, f.y + fh * 0.5f);
                g.b = v.ToScreen(t.x + t.w, t.y + th * 0.5f);
                da = ImVec2(-1, 0);
                db = ImVec2(1, 0);
            } else if (t.y + th * 0.5f >= f.y + fh * 0.5f) {
                g.a = v.ToScreen(f.x + f.w * 0.5f, f.y + fh);
                g.b = v.ToScreen(t.x + t.w * 0.5f, t.y);
                da = ImVec2(0, 1);
                db = ImVec2(0, -1);
            } else {
                g.a = v.ToScreen(f.x + f.w * 0.5f, f.y);
                g.b = v.ToScreen(t.x + t.w * 0.5f, t.y + th);
                da = ImVec2(0, -1);
                db = ImVec2(0, 1);
            }
            const float span = da.x != 0 ? std::fabs(g.b.x - g.a.x) : std::fabs(g.b.y - g.a.y);
            const float d = std::max(50.0f * sc, span * 0.5f);
            g.c1 = ImVec2(g.a.x + da.x * d, g.a.y + da.y * d);
            g.c2 = ImVec2(g.b.x + db.x * d, g.b.y + db.y * d);
            g.mid = Bez(g, 0.5f);
            const float tw = TextWidth(fs, b.links[i].label);
            const float ph = fs + 8 * sc, pw = tw + 16 * sc;
            g.chipMin = ImVec2(g.mid.x - pw * 0.5f, g.mid.y - ph * 0.5f);
            g.chipMax = ImVec2(g.mid.x + pw * 0.5f, g.mid.y + ph * 0.5f);
            g.valid = true;
        }
    };
    rebuild();
    const float portR = std::max(4.0f, 5.5f * sc), portHit = std::max(10.0f, 9.0f * sc);
    auto inRect = [](ImVec2 p, ImVec2 a, ImVec2 c) { return p.x >= a.x && p.x <= c.x && p.y >= a.y && p.y <= c.y; };
    auto dist2 = [](ImVec2 a, ImVec2 c) { return (a.x - c.x) * (a.x - c.x) + (a.y - c.y) * (a.y - c.y); };
    auto nodeAt = [&](ImVec2 m) {
        for (int i = static_cast<int>(b.nodes.size()) - 1; i >= 0; --i) {
            const BoardNode& n = b.nodes[i];
            if (n.scene != st.curScene) continue;
            if (inRect(m, v.ToScreen(n.x, n.y), v.ToScreen(n.x + n.w, n.y + geo[i].h))) return i;
        }
        return -1;
    };

    // the sides of a card can be dragged to resize it: 1 left, 2 right, 4 top, 8 bottom (the corner at the bottom right is the old grip).
    // -1 = the pointer is not on the card at all
    auto edgesAt = [&](int i, ImVec2 m) -> int {
        if (i < 0 || i >= static_cast<int>(b.nodes.size()) || i >= static_cast<int>(geo.size())) return -1; // a card added this frame has no geometry yet
        const BoardNode& n = b.nodes[i];
        const ImVec2 a = v.ToScreen(n.x, n.y), c = v.ToScreen(n.x + n.w, n.y + geo[i].h);
        const float band = 6.0f, slop = 4.0f;
        if (m.x < a.x - slop || m.x > c.x + slop || m.y < a.y - slop || m.y > c.y + slop) return -1;
        int e = 0;
        if (m.x < a.x + band) e |= 1;
        if (m.x > c.x - band) e |= 2;
        if (m.y < a.y + band) e |= 4;
        if (m.y > c.y - band) e |= 8;
        if (m.x > c.x - 14 && m.y > c.y - 14) e |= 2 | 8;
        return e;
    };
    // the card's text area made as tall as its text needs (the text wraps at the card's width)
    auto fitText = [&](BoardNode& n) {
        const float wrapPx = (n.w - 2 * kPad) * sc;
        const ImVec2 szPx = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, wrapPx, n.text.c_str(), n.text.c_str() + n.text.size());
        n.textH = std::clamp(szPx.y / sc + 10.0f, kLine + 4.0f, 1500.0f);
    };

    // --- input ---
    const ImVec2 mouse = io.MousePos;
    if (hovered && ImGui::IsMouseClicked(0)) {
        st.dragMoved = 0;
        int hit = -1;
        bool outPort = false, inPort = false;
        int edges = 0;
        for (int i = static_cast<int>(b.nodes.size()) - 1; i >= 0 && hit < 0; --i) { // ports first: they stick out of the card
            const BoardNode& n = b.nodes[i];
            if (n.scene != st.curScene) continue;
            if (dist2(mouse, v.ToScreen(n.x + n.w, n.y + geo[i].h * 0.5f)) <= portHit * portHit) {
                hit = i;
                outPort = true;
            } else if (dist2(mouse, v.ToScreen(n.x, n.y + geo[i].h * 0.5f)) <= portHit * portHit) {
                hit = i;
                inPort = true;
            }
        }
        if (hit < 0) {
            for (int i = static_cast<int>(b.nodes.size()) - 1; i >= 0; --i) { // the card under the pointer (and which of its sides, for resizing)
                if (b.nodes[i].scene != st.curScene) continue;
                const int e = edgesAt(i, mouse);
                if (e >= 0) {
                    hit = i;
                    edges = e;
                    break;
                }
            }
        }
        int chip = -1;
        if (hit < 0 || (!outPort && !inPort)) { // chips sit on top of the cards
            for (size_t i = 0; i < b.links.size(); ++i) {
                if (lg[i].valid && inRect(mouse, lg[i].chipMin, lg[i].chipMax)) chip = static_cast<int>(i);
            }
        }
        if (chip >= 0 && !outPort && !inPort) {
            st.selLink = b.links[chip].id;
            st.selNode = -1;
        } else if (hit >= 0) {
            const int id = b.nodes[hit].id;
            st.selNode = id;
            st.selLink = -1;
            if (outPort || inPort) {
                st.linkFrom = id;
                st.linkFromOutput = outPort;
            } else {
                st.resizeNode = edges ? id : -1;
                st.resizeEdges = edges;
                st.dragNode = edges ? -1 : id;
                const BoardNode& hn = b.nodes[hit];
                if (edges == 0 && hn.kind == BoardKind::Character && geo[hit].stripH > 0) { // the inventory strip: show the inventory
                    const float top = hn.y + kHeaderH + geo[hit].imgH + kPad;
                    if (inRect(mouse, v.ToScreen(hn.x, top), v.ToScreen(hn.x + hn.w, top + geo[hit].stripH))) {
                        st.invFocus = true;
                        st.invQuick = true; // and the add menu right here
                        st.dragNode = -1;
                    }
                }
                if (hit != static_cast<int>(b.nodes.size()) - 1) { // bring to front
                    BoardNode moved = b.nodes[hit];
                    b.nodes.erase(b.nodes.begin() + hit);
                    b.nodes.push_back(std::move(moved));
                    rebuild();
                }
            }
        } else {
            st.selNode = st.selLink = -1;
            st.panning = true;
        }
    }
    if (hovered && ImGui::IsMouseDoubleClicked(0)) {
        bool onChip = false;
        for (size_t i = 0; i < b.links.size(); ++i) { // double click on a relation's text: rewrite it right there
            if (lg[i].valid && inRect(mouse, lg[i].chipMin, lg[i].chipMax)) {
                st.editLink = b.links[i].id;
                st.editLinkBuf = b.links[i].label;
                st.editLinkFocus = true;
                st.selLink = b.links[i].id;
                st.selNode = -1;
                onChip = true;
            }
        }
        if (!onChip) { // double click on the bottom edge of a card: show all of its text
            for (int i = static_cast<int>(b.nodes.size()) - 1; i >= 0; --i) {
                if (b.nodes[i].scene != st.curScene) continue;
                const int e = edgesAt(i, mouse);
                if (e < 0) continue;
                if ((e & 8)) {
                    fitText(b.nodes[i]);
                    Touch(st);
                    st.resizeNode = st.dragNode = -1;
                }
                break;
            }
        }
    }
    if (hovered && ImGui::IsMouseDoubleClicked(0) && nodeAt(mouse) < 0) { // double-click on the empty board: add something right here
        bool onChip = false;
        for (size_t i = 0; i < b.links.size(); ++i) onChip = onChip || (lg[i].valid && inRect(mouse, lg[i].chipMin, lg[i].chipMax));
        if (!onChip) {
            const ImVec2 at = v.ToBoard(mouse);
            st.airFrom = -1;
            st.airTarget = -1;
            st.airX = at.x;
            st.airY = at.y;
            st.airScreenX = mouse.x + 6;
            st.airScreenY = mouse.y + 6;
            st.airKind = static_cast<int>(BoardKind::Character);
            st.airOpen = true;
            st.airOpenFrame = ImGui::GetFrameCount();
        }
    }
    if (io.MouseDown[0]) {
        const float dx = io.MouseDelta.x / sc, dy = io.MouseDelta.y / sc;
        st.dragMoved += std::fabs(io.MouseDelta.x) + std::fabs(io.MouseDelta.y);
        if (st.dragNode >= 0) {
            if (BoardNode* n = b.Find(st.dragNode)) {
                n->x += dx;
                n->y += dy;
                if (dx != 0 || dy != 0) Touch(st);
            }
        } else if (st.resizeNode >= 0) {
            if (BoardNode* n = b.Find(st.resizeNode)) {
                const int e = st.resizeEdges;
                const int gi = indexOf(n->id);
                const float curText = n->textH > 0 ? n->textH : (gi >= 0 ? geo[gi].textH : kLine * kTextLines + 4.0f);
                const float minText = kLine + 4.0f;
                if (e & 2) n->w = std::clamp(n->w + dx, 150.0f, 800.0f);
                if (e & 1) {
                    const float nw = std::clamp(n->w - dx, 150.0f, 800.0f);
                    n->x += n->w - nw;
                    n->w = nw;
                }
                if (e & 8) n->textH = std::clamp(curText + dy, minText, 1500.0f);
                if (e & 4) {
                    const float nt = std::clamp(curText - dy, minText, 1500.0f);
                    n->y += curText - nt;
                    n->textH = nt;
                }
                if (dx != 0 || dy != 0) Touch(st);
            }
        } else if (st.panning) {
            b.panX -= dx;
            b.panY -= dy;
        }
    }
    if (ImGui::IsItemActive() && io.MouseDown[2]) {
        b.panX -= io.MouseDelta.x / sc;
        b.panY -= io.MouseDelta.y / sc;
    }
    if (ImGui::IsMouseReleased(0)) {
        if (st.linkFrom >= 0) {
            const int target = nodeAt(mouse);
            if (target >= 0 && b.nodes[target].id != st.linkFrom) { // dropped on another card: ask what to add (a link is one of the options)
                const ImVec2 at = v.ToBoard(mouse);
                st.airFrom = st.linkFrom;
                st.airOutput = true; // dropped on a card: the card you dragged from is the one that acts (it does X to the other), whichever dot you used
                st.airTarget = b.nodes[target].id;
                st.airX = at.x;
                st.airY = at.y;
                st.airScreenX = mouse.x + 6;
                st.airScreenY = mouse.y + 6;
                st.airKind = 4;
                AirLog("opened on card");
                { char m[96]; std::snprintf(m, sizeof(m), "  mouse down=%d clicked=%d released=%d", io.MouseDown[0] ? 1 : 0, ImGui::IsMouseClicked(0) ? 1 : 0, ImGui::IsMouseReleased(0) ? 1 : 0); AirLog(m); }
                st.airOpen = true;
                st.airOpenFrame = ImGui::GetFrameCount();
            } else if (target < 0 && hovered && st.dragMoved > 14.0f) { // dropped in the air: offer what could go there
                const ImVec2 at = v.ToBoard(mouse);
                st.airFrom = st.linkFrom;
                st.airOutput = st.linkFromOutput;
                st.airTarget = -1;
                st.airX = at.x;
                st.airY = at.y;
                st.airScreenX = mouse.x + 6;
                st.airScreenY = mouse.y + 6;
                st.airKind = (b.Find(st.linkFrom) && b.Find(st.linkFrom)->kind == BoardKind::Character) ? static_cast<int>(BoardKind::Item) : static_cast<int>(BoardKind::Character);
                st.airOpen = true;
            st.airOpenFrame = ImGui::GetFrameCount();
            }
        }
        st.dragNode = st.resizeNode = st.linkFrom = -1;
        st.panning = false;
    }
    rebuild();
    // right click: card / relation menus, or add a note
    if (hovered && ImGui::IsMouseClicked(1)) {
        const int ni = nodeAt(mouse);
        int chip = -1;
        for (size_t i = 0; i < b.links.size(); ++i) {
            if (lg[i].valid && inRect(mouse, lg[i].chipMin, lg[i].chipMax)) chip = static_cast<int>(i);
        }
        st.ctxNode = chip < 0 && ni >= 0 ? b.nodes[ni].id : -1;
        st.ctxLink = chip >= 0 ? b.links[chip].id : -1;
        if (st.ctxNode >= 0) {
            st.selNode = st.ctxNode;
            st.selLink = -1;
        } else if (st.ctxLink >= 0) {
            st.selLink = st.ctxLink;
            st.selNode = -1;
        }
        ImGui::OpenPopup("##boardctx");
    }
    // keys
    if (hovered && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
            if (st.selNode >= 0) b.Remove(st.selNode);
            else if (st.selLink >= 0) b.RemoveLink(st.selLink);
            st.selNode = st.selLink = -1;
            Touch(st);
        }
    }
    // dropping pictures from the library / gallery: onto a card they SWAP that card's picture (a character becomes the new one), onto empty
    // board they are added
    int dropHover = -1;
    if (ImGui::BeginDragDropTarget()) {
        const ImVec2 at = v.ToBoard(mouse);
        const BoardKind kind = static_cast<BoardKind>(std::clamp(st.addKind, 0, 2));
        const int under = nodeAt(mouse);
        const bool swappable = under >= 0 && b.nodes[under].kind != BoardKind::Note;
        if (ImGui::GetDragDropPayload() && ImGui::GetDragDropPayload()->Delivery) CrashNote(std::string("board: a picture was dropped ") + (swappable ? "on a card" : "on the empty board"));
        if (const ImGuiPayload* live = ImGui::GetDragDropPayload()) {
            if (swappable && (live->IsDataType("MV_LIBITEM") || live->IsDataType("MV_BOARDCHAR") || live->IsDataType("MV_IMAGES") || live->IsDataType("MV_BOARDICON"))) dropHover = under;
        }
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("MV_LIBITEM", ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
            if (swappable) SwapCardPicture(app, st, b.nodes[under].id, static_cast<const wchar_t*>(pl->Data));
            else AddPicture(app, st, static_cast<const wchar_t*>(pl->Data), kind, at.x - 115, at.y - 40);
        } else if (const ImGuiPayload* pi = ImGui::AcceptDragDropPayload("MV_BOARDICON", ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
            const std::string iconId = static_cast<const char*>(pi->Data);
            if (under >= 0 && b.nodes[under].kind == BoardKind::Character && !b.nodes[under].image.empty()) {
                AddInvIcon(st, EnsureProfileOf(b, b.nodes[under].id), iconId);
                st.selNode = b.nodes[under].id;
                st.invFocus = true;
            } else if (swappable) {
                SwapCardIcon(app, st, b.nodes[under].id, iconId);
            } else {
                AddIconCard(app, st, iconId, at.x - 60, at.y - 40);
            }
        } else if (const ImGuiPayload* pc = ImGui::AcceptDragDropPayload("MV_BOARDCHAR", ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
            if (swappable && b.nodes[under].kind == BoardKind::Character) ReplaceCharacter(app, st, b.nodes[under].id, static_cast<const wchar_t*>(pc->Data), false);
            else AddPicture(app, st, static_cast<const wchar_t*>(pc->Data), BoardKind::Character, at.x - 115, at.y - 40);
        } else if (const ImGuiPayload* pg = ImGui::AcceptDragDropPayload("MV_IMAGES", ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
            const int* idx = static_cast<const int*>(pg->Data);
            const int count = static_cast<int>(pg->DataSize / sizeof(int));
            for (int i = 0; i < count; ++i) {
                if (idx[i] < 0 || idx[i] >= static_cast<int>(app.entries.size())) continue;
                if (i == 0 && swappable) SwapCardPicture(app, st, b.nodes[under].id, app.entries[idx[i]].path);
                else AddPicture(app, st, app.entries[idx[i]].path, kind, at.x - 115 + i * 40, at.y - 40 + i * 40);
            }
        }
        ImGui::EndDragDropTarget();
    }

    // the pointer shows which side of a card it would resize
    if (hovered || (st.resizeNode >= 0 && io.MouseDown[0])) {
        int e = 0;
        if (st.resizeNode >= 0 && io.MouseDown[0]) {
            e = st.resizeEdges;
        } else if (!io.MouseDown[0] && st.linkFrom < 0) {
            for (int i = static_cast<int>(b.nodes.size()) - 1; i >= 0; --i) {
                if (b.nodes[i].scene != st.curScene) continue;
                const int ee = edgesAt(i, mouse);
                if (ee < 0) continue;
                if (i >= static_cast<int>(geo.size())) break;
                const BoardNode& hn = b.nodes[i];
                const bool nearPort = dist2(mouse, v.ToScreen(hn.x + hn.w, hn.y + geo[i].h * 0.5f)) <= portHit * portHit || dist2(mouse, v.ToScreen(hn.x, hn.y + geo[i].h * 0.5f)) <= portHit * portHit;
                e = nearPort ? 0 : ee;
                break;
            }
        }
        const bool horiz = (e & 3) != 0, vert = (e & 12) != 0;
        if (horiz && vert) ImGui::SetMouseCursor((e == (1 | 4) || e == (2 | 8) || e == 10 || e == 5 || e == (2 | 8 | 4)) ? ImGuiMouseCursor_ResizeNWSE : ImGuiMouseCursor_ResizeNESW);
        else if (horiz) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        else if (vert) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }

    rebuild(); // the input above may have added / removed / moved things

    // --- drawing ---
    dl->PushClipRect(p0, ImVec2(p0.x + sz.x, p0.y + sz.y), true);
    dl->AddRectFilled(p0, ImVec2(p0.x + sz.x, p0.y + sz.y), Rgb(0x1B1B1F));
    {
        const float step = 40.0f * sc;
        if (step > 8.0f) {
            const float ox = std::fmod(-b.panX * sc, step), oy = std::fmod(-b.panY * sc, step);
            for (float x = ox; x < sz.x; x += step) dl->AddLine(ImVec2(p0.x + x, p0.y), ImVec2(p0.x + x, p0.y + sz.y), Rgb(0x26262B), 1.0f);
            for (float y = oy; y < sz.y; y += step) dl->AddLine(ImVec2(p0.x, p0.y + y), ImVec2(p0.x + sz.x, p0.y + y), Rgb(0x26262B), 1.0f);
        }
    }
    if (b.nodes.empty()) {
        const char* msg = "Drag pictures from the Vision library on the left onto this board";
        const char* msg2 = "or paste an image with Ctrl+V.  Link two cards by dragging from the dot on a card's right edge to another card.";
        const ImVec2 s1 = ImGui::CalcTextSize(msg), s2 = ImGui::CalcTextSize(msg2);
        dl->AddText(ImVec2(p0.x + (sz.x - s1.x) * 0.5f, p0.y + sz.y * 0.4f), Rgb(0x9A9AAE), msg);
        dl->AddText(ImVec2(p0.x + (sz.x - s2.x) * 0.5f, p0.y + sz.y * 0.4f + s1.y + 6), Rgb(0x70707F), msg2);
    }

    // links
    for (size_t i = 0; i < b.links.size(); ++i) {
        if (!lg[i].valid) continue;
        const LinkGeo& g = lg[i];
        const BoardNode* f = b.Find(b.links[i].from);
        const bool sel = b.links[i].id == st.selLink;
        const bool touching = (st.selNode >= 0) && (b.links[i].from == st.selNode || b.links[i].to == st.selNode);
        const uint32_t col = f ? KindColor(f->kind) : 0x888888;
        dl->AddBezierCubic(g.a, g.c1, g.c2, g.b, sel ? IM_COL32(255, 255, 255, 240) : Rgb(col, touching ? 255 : 190), std::max(1.5f, (sel ? 3.5f : 2.5f) * sc));
        // arrow head at the end
        const ImVec2 pre = Bez(g, 0.96f);
        float dx = g.b.x - pre.x, dy = g.b.y - pre.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len > 0.01f) {
            dx /= len;
            dy /= len;
            const float s = std::max(6.0f, 8.0f * sc);
            const ImVec2 tip(g.b.x - dx * portR, g.b.y - dy * portR);
            dl->AddTriangleFilled(tip, ImVec2(tip.x - dx * s - dy * s * 0.55f, tip.y - dy * s + dx * s * 0.55f),
                                  ImVec2(tip.x - dx * s + dy * s * 0.55f, tip.y - dy * s - dx * s * 0.55f), sel ? IM_COL32(255, 255, 255, 240) : Rgb(col, 230));
        }
    }

    // cards
    for (size_t i = 0; i < b.nodes.size(); ++i) {
        const BoardNode& n = b.nodes[i];
        if (n.scene != st.curScene) continue;
        const Geo& g = geo[i];
        const ImVec2 a = v.ToScreen(n.x, n.y), c = v.ToScreen(n.x + n.w, n.y + g.h);
        if (c.x < p0.x || a.x > p0.x + sz.x || c.y < p0.y || a.y > p0.y + sz.y) continue;
        const bool sel = n.id == st.selNode;
        const float round = std::max(3.0f, 7.0f * sc);
        const ImVec4 clip(std::max(a.x, p0.x), std::max(a.y, p0.y), std::min(c.x, p0.x + sz.x), std::min(c.y, p0.y + sz.y));
        dl->AddRectFilled(ImVec2(a.x + 3 * sc, a.y + 4 * sc), ImVec2(c.x + 3 * sc, c.y + 4 * sc), IM_COL32(0, 0, 0, 70), round);
        dl->AddRectFilled(a, c, Rgb(0x2C2C32), round);
        const ImVec2 hb(c.x, a.y + kHeaderH * sc);
        dl->AddRectFilled(a, hb, Rgb(KindColor(n.kind)), round, ImDrawFlags_RoundCornersTop);
        const std::string title = b.DisplayName(n);
        const std::string tag = BoardKindName(n.kind);
        const float tagW = TextWidth(fs * 0.85f, tag);
        const float headerFs = fs;
        AddTextClipped(dl, headerFs, ImVec2(a.x + 9 * sc, a.y + (kHeaderH * sc - headerFs) * 0.5f), IM_COL32(255, 255, 255, 245), title,
                       ImVec4(a.x, a.y, c.x - tagW - 14 * sc, hb.y));
        if (c.x - a.x > tagW + 80 * sc) {
            AddTextClipped(dl, fs * 0.85f, ImVec2(c.x - tagW - 9 * sc, a.y + (kHeaderH * sc - fs * 0.85f) * 0.5f), IM_COL32(255, 255, 255, 150), tag,
                           ImVec4(a.x, a.y, c.x, hb.y));
        }
        float y = n.y + kHeaderH;
        if (g.imgH > 0) {
            const ImVec2 ia = v.ToScreen(n.x + kPad, y + 0), ib = v.ToScreen(n.x + n.w - kPad, y + g.imgH);
            dl->AddRectFilled(ia, ib, n.image.find(L"icons-v1\\") != std::wstring::npos ? IM_COL32(214, 219, 228, 255) : IM_COL32(21, 21, 24, 255), 3.0f);
            if (g.thumb.srv && g.thumb.w > 0) {
                const float boxW = ib.x - ia.x, boxH = ib.y - ia.y;
                const float k = std::min(boxW / g.thumb.w, boxH / g.thumb.h);
                const float iw = g.thumb.w * k, ih = g.thumb.h * k;
                const ImVec2 ma(ia.x + (boxW - iw) * 0.5f, ia.y + (boxH - ih) * 0.5f);
                dl->AddImage((ImTextureID)(intptr_t)g.thumb.srv, ma, ImVec2(ma.x + iw, ma.y + ih));
            } else {
                const char* m = g.thumb.failed ? "picture not found" : "loading...";
                const ImVec2 ts = ImGui::CalcTextSize(m);
                dl->AddText(ImVec2((ia.x + ib.x - ts.x) * 0.5f, (ia.y + ib.y - ts.y) * 0.5f), Rgb(0x707080), m);
            }
            y += g.imgH + kPad;
        } else {
            y += kPad;
        }
        if (g.stripH > 0) { // what the character wears / holds / carries
            const float cell = kStripH, gap = 4.0f;
            const int fit = std::max(1, static_cast<int>((n.w - 2 * kPad + gap) / (cell + gap)));
            const int count = g.profile ? static_cast<int>(g.profile->items.size()) : 0;
            if (count < fit) { // the empty slot: click it to fill the inventory
                const ImVec2 pa = v.ToScreen(n.x + kPad + count * (cell + gap), y), pb = v.ToScreen(n.x + kPad + count * (cell + gap) + cell, y + cell);
                dl->AddRect(pa, pb, IM_COL32(255, 255, 255, 80), 3.0f, 0, 1.5f);
                const ImVec2 ts = ImGui::CalcTextSize("+");
                dl->AddText(ImVec2((pa.x + pb.x - ts.x) * 0.5f, (pa.y + pb.y - ts.y) * 0.5f), IM_COL32(255, 255, 255, 140), "+");
                if (count == 0) {
                    AddTextClipped(dl, fs * 0.9f, ImVec2(pb.x + 6 * sc, pa.y + (pb.y - pa.y - fs * 0.9f) * 0.5f), Rgb(0x70707F), "inventory: clothes, items...",
                                   ImVec4(pb.x, pa.y, c.x, pb.y));
                }
                if (ImGui::IsMouseHoveringRect(pa, pb) && ImGui::IsWindowHovered()) ImGui::SetTooltip("Inventory: click to add what this character wears, holds or carries");
            }
            for (int k = 0; k < std::min(count, fit); ++k) {
                const InvItem& it = g.profile->items[k];
                const ImVec2 ca = v.ToScreen(n.x + kPad + k * (cell + gap), y), cb = v.ToScreen(n.x + kPad + k * (cell + gap) + cell, y + cell);
                const uint32_t slotColor = it.slot == InvSlot::Wearing ? 0x6A8CFF : it.slot == InvSlot::Holding ? 0xFFB347 : it.slot == InvSlot::Carrying ? 0x6FD39A : 0xE06C75;
                const bool iconItem = it.image.find(L"icons-v1\\") != std::wstring::npos;
                dl->AddRectFilled(ca, cb, iconItem ? IM_COL32(214, 219, 228, 255) : Rgb(0x151518), 3.0f);
                const AppState::LibThumb ti = it.image.empty() ? AppState::LibThumb{} : app.LibraryThumb(it.image);
                if (ti.srv && ti.w > 0) {
                    const float kz = std::min((cb.x - ca.x) / ti.w, (cb.y - ca.y) / ti.h);
                    const float iw = ti.w * kz, ih = ti.h * kz;
                    const ImVec2 ma(ca.x + ((cb.x - ca.x) - iw) * 0.5f, ca.y + ((cb.y - ca.y) - ih) * 0.5f);
                    dl->AddImage((ImTextureID)(intptr_t)ti.srv, ma, ImVec2(ma.x + iw, ma.y + ih));
                } else if (!it.name.empty()) {
                    const std::string initial = it.name.substr(0, 1);
                    const ImVec2 ts = ImGui::CalcTextSize(initial.c_str());
                    dl->AddText(ImVec2((ca.x + cb.x - ts.x) * 0.5f, (ca.y + cb.y - ts.y) * 0.5f), Rgb(0xB0B0C0), initial.c_str());
                }
                dl->AddRect(ca, cb, Rgb(slotColor, 220), 3.0f, 0, 1.5f);
                if (k == fit - 1 && count > fit) {
                    const std::string more = "+" + std::to_string(count - fit + 1);
                    dl->AddRectFilled(ca, cb, IM_COL32(0, 0, 0, 170), 3.0f);
                    const ImVec2 ts = ImGui::CalcTextSize(more.c_str());
                    dl->AddText(ImVec2((ca.x + cb.x - ts.x) * 0.5f, (ca.y + cb.y - ts.y) * 0.5f), IM_COL32(255, 255, 255, 240), more.c_str());
                }
                if (ImGui::IsMouseHoveringRect(ca, cb) && ImGui::IsWindowHovered()) {
                    ImGui::SetTooltip("%s: %s", InvSlotName(it.slot), it.name.c_str());
                }
            }
            y += g.stripH + kPad;
        }
        // description
        const ImVec2 ta = v.ToScreen(n.x + kPad, y), tb = v.ToScreen(n.x + n.w - kPad, y + g.textH);
        const ImVec4 tclip(std::max(ta.x, clip.x), std::max(ta.y, clip.y), std::min(tb.x, clip.z), std::min(tb.y, clip.w));
        if (n.describing) {
            AddTextClipped(dl, fs, ta, Rgb(0xE0B050), "Describing...", tclip);
        } else if (!n.text.empty()) {
            AddTextClipped(dl, fs, ta, Rgb(0xC8C8D0), n.text, tclip, tb.x - ta.x);
        } else {
            AddTextClipped(dl, fs, ta, Rgb(0x707080), n.kind == BoardKind::Note ? "(empty note)" : "(no description - use the panel on the right)", tclip, tb.x - ta.x);
        }
        // border, ports, resize grip
        dl->AddRect(a, c, sel ? IM_COL32(255, 255, 255, 235) : IM_COL32(255, 255, 255, 36), round, 0, sel ? 2.0f : 1.0f);
        if (static_cast<int>(i) == dropHover) { // a picture is being dragged over this card: dropping it swaps
            dl->AddRectFilled(a, c, Rgb(0x6EE2FF, 60), round);
            dl->AddRect(a, c, Rgb(0x6EE2FF, 255), round, 0, 3.0f);
            const char* msg = n.kind == BoardKind::Character ? "Drop to swap this character" : "Drop to swap this picture";
            const ImVec2 ts = ImGui::CalcTextSize(msg);
            const ImVec2 mid((a.x + c.x) * 0.5f, (a.y + c.y) * 0.5f);
            dl->AddRectFilled(ImVec2(mid.x - ts.x * 0.5f - 8, mid.y - ts.y * 0.5f - 5), ImVec2(mid.x + ts.x * 0.5f + 8, mid.y + ts.y * 0.5f + 5), IM_COL32(20, 30, 40, 235), 6.0f);
            dl->AddText(ImVec2(mid.x - ts.x * 0.5f, mid.y - ts.y * 0.5f), IM_COL32(110, 226, 255, 255), msg);
        }
        // while a link is being dragged: show which cards it can end on (the others are dimmed)
        if (st.linkFrom >= 0 && io.MouseDown[0]) {
            if (n.id == st.linkFrom) {
                dl->AddRect(a, c, Rgb(0x6EE2FF, 200), round, 0, 2.0f);
            } else {
                const int from = st.linkFrom, to = n.id; // the card you drag from acts on the one you drop on
                bool exists = false;
                for (const BoardLink& l : b.links) exists = exists || (l.from == from && l.to == to);
                if (exists) {
                    dl->AddRectFilled(a, c, IM_COL32(0, 0, 0, 120), round);
                } else {
                    const bool over = nodeAt(mouse) == static_cast<int>(i);
                    if (over) dl->AddRectFilled(a, c, Rgb(0x6EE2FF, 40), round);
                    dl->AddRect(ImVec2(a.x - 3, a.y - 3), ImVec2(c.x + 3, c.y + 3), Rgb(0x6EE2FF, over ? 255 : 170), round + 3, 0, over ? 3.0f : 2.0f);
                    const BoardNode* fn = b.Find(from);
                    const BoardNode* tn = b.Find(to);
                    if (fn && tn) {
                        const std::string hint = std::string("+ ") + BoardRelationPresets(fn->kind, tn->kind).front();
                        const float tw = TextWidth(fs, hint);
                        const ImVec2 pa(a.x + 6 * sc, a.y - fs - 10 * sc), pb(pa.x + tw + 12 * sc, pa.y + fs + 6 * sc);
                        dl->AddRectFilled(pa, pb, Rgb(0x6EE2FF, over ? 255 : 210), 4.0f);
                        AddTextClipped(dl, fs, ImVec2(pa.x + 6 * sc, pa.y + 3 * sc), IM_COL32(10, 20, 30, 255), hint, ImVec4(pa.x, pa.y, pb.x, pb.y));
                    }
                }
            }
        }
        const ImVec2 pin = v.ToScreen(n.x, n.y + g.h * 0.5f), pout = v.ToScreen(n.x + n.w, n.y + g.h * 0.5f);
        for (const ImVec2& pp : {pin, pout}) {
            const bool close = dist2(mouse, pp) <= portHit * portHit;
            dl->AddCircleFilled(pp, portR, close ? IM_COL32(255, 255, 255, 255) : Rgb(KindColor(n.kind)));
            dl->AddCircle(pp, portR, Rgb(0x1B1B1F), 0, 1.5f);
        }
        dl->AddTriangleFilled(ImVec2(c.x - 11 * sc, c.y - 3 * sc), ImVec2(c.x - 3 * sc, c.y - 3 * sc), ImVec2(c.x - 3 * sc, c.y - 11 * sc), IM_COL32(255, 255, 255, 70));
    }

    // relation chips on top
    for (size_t i = 0; i < b.links.size(); ++i) {
        if (!lg[i].valid) continue;
        const LinkGeo& g = lg[i];
        const bool sel = b.links[i].id == st.selLink;
        const BoardNode* f = b.Find(b.links[i].from);
        dl->AddRectFilled(g.chipMin, g.chipMax, sel ? IM_COL32(245, 245, 250, 255) : Rgb(0x23232A, 245), (g.chipMax.y - g.chipMin.y) * 0.5f);
        dl->AddRect(g.chipMin, g.chipMax, sel ? IM_COL32(255, 255, 255, 255) : Rgb(f ? KindColor(f->kind) : 0x888888), (g.chipMax.y - g.chipMin.y) * 0.5f, 0, 1.5f);
        AddTextClipped(dl, fs, ImVec2(g.chipMin.x + 8 * sc, g.chipMin.y + 4 * sc), sel ? IM_COL32(20, 20, 24, 255) : Rgb(0xE8E8F0), b.links[i].label,
                       ImVec4(g.chipMin.x, g.chipMin.y, g.chipMax.x, g.chipMax.y));
    }
    // the link being dragged
    if (st.linkFrom >= 0 && io.MouseDown[0]) {
        const int fi = indexOf(st.linkFrom);
        if (fi >= 0) {
            const BoardNode& n = b.nodes[fi];
            // the line always starts at the dot you grabbed and ends at the pointer (the card you drag from is the one that acts)
            const float side = st.linkFromOutput ? 1.0f : -1.0f;
            const ImVec2 pa = v.ToScreen(st.linkFromOutput ? n.x + n.w : n.x, n.y + geo[fi].h * 0.5f);
            const ImVec2 pb = mouse;
            const float d = std::max(50.0f * sc, std::fabs(pb.x - pa.x) * 0.5f);
            dl->AddBezierCubic(pa, ImVec2(pa.x + d * side, pa.y), ImVec2(pb.x - d * side, pb.y), pb, Rgb(0x6EE2FF, 230), std::max(2.0f, 3.0f * sc));
        }
    }
    if (st.selNode < 0 && st.selLink < 0) { // a hint that never covers a card you are working on
        const char* hint = "Drag a card to move it  -  drag from its dots to another card  -  wheel: zoom  -  right click: menu";
        const ImVec2 hs = ImGui::CalcTextSize(hint);
        const ImVec2 hp(p0.x + 8, p0.y + sz.y - hs.y - 10);
        dl->AddRectFilled(ImVec2(hp.x - 8, hp.y - 4), ImVec2(hp.x + hs.x + 8, hp.y + hs.y + 4), IM_COL32(18, 20, 24, 242), 6.0f);
        dl->AddText(hp, Rgb(0x8A8A98), hint);
    }
    dl->PopClipRect();

    // a relation's text being rewritten in place
    if (st.editLink >= 0) {
        int li = -1;
        for (size_t i = 0; i < b.links.size(); ++i) {
            if (b.links[i].id == st.editLink) li = static_cast<int>(i);
        }
        if (li < 0 || !lg[li].valid) {
            st.editLink = -1;
        } else {
            const LinkGeo& g = lg[li];
            const float w = std::max(ui::Dp(170.0f), g.chipMax.x - g.chipMin.x + 40.0f);
            ImGui::SetCursorScreenPos(ImVec2(g.mid.x - w * 0.5f, g.mid.y - ImGui::GetFrameHeight() * 0.5f));
            ImGui::SetNextItemWidth(w);
            if (st.editLinkFocus) {
                ImGui::SetKeyboardFocusHere();
                st.editLinkFocus = false;
            }
            const bool enter = ImGui::InputText("##linkedit", st.editLinkBuf.data(), st.editLinkBuf.capacity() + 1,
                                                ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll, StdStringResize, &st.editLinkBuf);
            const bool esc = ImGui::IsKeyPressed(ImGuiKey_Escape);
            if (esc) {
                st.editLink = -1;
            } else if (enter || ImGui::IsItemDeactivated()) {
                if (!st.editLinkBuf.empty() && st.editLinkBuf != b.links[li].label) {
                    b.links[li].label = st.editLinkBuf;
                    Touch(st);
                }
                st.editLink = -1;
            }
        }
    }

    // swap a card's character with another saved character
    if (st.openSwap) {
        ImGui::OpenPopup("##swapchar");
        st.openSwap = false;
    }
    if (ImGui::BeginPopup("##swapchar")) {
        const BoardNode* sn = b.Find(st.swapNode);
        if (!sn || sn->kind != BoardKind::Character) {
            ImGui::CloseCurrentPopup();
        } else {
            ImGui::Text("Swap %s with:", b.DisplayName(*sn).c_str());
            ImGui::Checkbox("The new character keeps the old one's things", &st.swapKeepGear);
            ImGui::BeginChild("##swaplist", ImVec2(ui::Dp(330.0f), ui::Dp(280.0f)), true);
            bool any = false;
            for (const CharacterProfile& p : AllProfiles()) {
                if (!p.saved || _wcsicmp(p.image.c_str(), sn->image.c_str()) == 0) continue;
                any = true;
                const float row = ui::Dp(44.0f);
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                ImGui::PushID(p.key.c_str());
                if (ImGui::Selectable("##pick", false, 0, ImVec2(0, row))) {
                    ReplaceCharacter(app, st, st.swapNode, p.image, st.swapKeepGear);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::PopID();
                DrawThumbAt(app, ImGui::GetWindowDrawList(), p.image, ImVec2(pos.x + 2, pos.y + 2), ImVec2(pos.x + row - 2, pos.y + row - 2));
                ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x + row + 8, pos.y + (row - ImGui::GetFontSize()) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text),
                                                    (p.name.empty() ? ToUtf8(fs::path(p.image).stem().wstring()) : p.name).c_str());
            }
            if (!any) ImGui::TextDisabled("No other saved characters yet.");
            ImGui::EndChild();
            if (ImGui::Button("Another picture...")) {
                const std::wstring f = OpenImageFileDialog(GetActiveWindow());
                if (!f.empty()) {
                    AddSavedCharacter(app, f);
                    ReplaceCharacter(app, st, st.swapNode, f, st.swapKeepGear);
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("The Vision library's selected picture")) {
                if (app.visionLibSelected >= 0 && app.visionLibSelected < static_cast<int>(app.visionLib.size())) {
                    const std::wstring f = app.visionLib[app.visionLibSelected].imagePath;
                    AddSavedCharacter(app, f);
                    ReplaceCharacter(app, st, st.swapNode, f, st.swapKeepGear);
                    ImGui::CloseCurrentPopup();
                } else {
                    app.SetStatus("Select a picture in the Vision library first");
                }
            }
            ImGui::TextDisabled("Tip: drag a picture onto a card to swap it.");
        }
        ImGui::EndPopup();
    }

    if (st.openRelPick) {
        ImGui::OpenPopup("##relpick");
        st.openRelPick = false;
    }
    // what happens between the two cards just linked
    if (ImGui::BeginPopup("##relpick")) {
        BoardLink* l = nullptr;
        for (BoardLink& x : b.links)
            if (x.id == st.relPickLink) l = &x;
        const BoardNode* f = l ? b.Find(l->from) : nullptr;
        const BoardNode* tn = l ? b.Find(l->to) : nullptr;
        if (l && f && tn) {
            ImGui::TextDisabled("%s  ...  %s", b.DisplayName(*f).c_str(), b.DisplayName(*tn).c_str());
            ImGui::TextUnformatted("What does the first do to the second?");
            ImGui::Separator();
            std::string pk;
            ImGui::BeginChild("##relpicklist", ImVec2(ui::Dp(300.0f), ui::Dp(260.0f)), false);
            const bool pickedOne = RelationList(st, f->kind, tn->kind, l->label, pk);
            ImGui::EndChild();
            if (pickedOne) {
                l->label = pk;
                Touch(st);
                ImGui::CloseCurrentPopup();
            }
            ImGui::Separator();
            if (RelationCustom(st, f->kind, tn->kind, pk)) {
                l->label = pk;
                Touch(st);
                ImGui::CloseCurrentPopup();
            }
        } else {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // context menu
    if (ImGui::BeginPopup("##boardctx")) {
        if (st.ctxNode >= 0) {
            BoardNode* n = b.Find(st.ctxNode);
            if (n) {
                if (n->kind == BoardKind::Character && !n->image.empty() && ImGui::MenuItem("Swap with another character...")) {
                    st.swapNode = n->id;
                    st.openSwap = true;
                }
                if (!n->text.empty() && ImGui::MenuItem("Show all the text")) {
                    fitText(*n);
                    Touch(st);
                }
                if (n->kind != BoardKind::Note && ImGui::MenuItem("Describe with the vision model")) StartDescribe(app, st, n->id);
                if (n->kind != BoardKind::Note && ImGui::BeginMenu("Change role")) {
                    for (int k = 0; k < 3; ++k) {
                        if (ImGui::MenuItem(BoardKindName(static_cast<BoardKind>(k)), nullptr, n->kind == static_cast<BoardKind>(k))) {
                            n->kind = static_cast<BoardKind>(k);
                            Touch(st);
                        }
                    }
                    ImGui::EndMenu();
                }
                if (b.scenes.size() > 1 && ImGui::BeginMenu("Copy to scene")) {
                    for (int sc = 0; sc < static_cast<int>(b.scenes.size()); ++sc) {
                        if (sc == n->scene) continue;
                        if (ImGui::MenuItem(b.SceneName(sc).c_str())) {
                            const BoardNode copy = *n;
                            const int id = b.Add(copy.kind, copy.image, copy.text, copy.x, copy.y, sc);
                            if (BoardNode* c = b.Find(id)) {
                                c->title = copy.title;
                                c->w = copy.w;
                                c->profile = copy.profile; // the same character: the same inventory
                            }
                            Touch(st);
                        }
                    }
                    ImGui::EndMenu();
                }
                if (!n->image.empty()) {
                    if (ImGui::MenuItem("Show in Explorer")) {
                        const std::wstring args = L"/select,\"" + n->image + L"\"";
                        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Remove card")) {
                    b.Remove(n->id);
                    st.selNode = -1;
                    Touch(st);
                }
            }
        } else if (st.ctxLink >= 0) {
            BoardLink* l = nullptr;
            for (BoardLink& x : b.links)
                if (x.id == st.ctxLink) l = &x;
            if (l) {
                RelationMenu(st, *l);
                ImGui::Separator();
                if (ImGui::MenuItem("Swap direction")) {
                    std::swap(l->from, l->to);
                    Touch(st);
                }
                if (ImGui::MenuItem("Remove relation")) {
                    b.RemoveLink(l->id);
                    st.selLink = -1;
                    Touch(st);
                }
            }
        } else {
            const ImVec2 at = v.ToBoard(ImGui::GetMousePosOnOpeningCurrentPopup());
            if (ImGui::MenuItem("Add a note here")) {
                st.selNode = b.Add(BoardKind::Note, std::wstring(), std::string(), at.x - 100, at.y - 30, st.curScene);
                st.selLink = -1;
                Touch(st);
            }
            if (ImGui::MenuItem("Fit everything in view")) st.fitRequest = true;
        }
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Left: pictures of the Vision library

void DrawSavedCharacters(AppState& app, BoardViewState& st) {
    if (ImGui::SmallButton("+ Create a character...")) OpenCharacterCreator(app);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Build someone from looks (or your photo) in the Characters tab: they show up here");
    int shown = 0;
    const float row = ui::Dp(54.0f);
    for (CharacterProfile& p : AllProfiles()) {
        if (!p.saved) continue;
        ++shown;
        ImGui::PushID(p.key.c_str());
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        ImGui::InvisibleButton("##c", ImVec2(w, row));
        const bool hov = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + row), hov ? Rgb(0x2E2E36) : Rgb(0x24242A), 5.0f);
        DrawThumbAt(app, dl, p.image, ImVec2(pos.x + 3, pos.y + 3), ImVec2(pos.x + row - 3, pos.y + row - 3));
        const std::string name = p.name.empty() ? ToUtf8(fs::path(p.image).stem().wstring()) : p.name;
        dl->AddText(ImVec2(pos.x + row + 6, pos.y + 6), ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
        const std::string sub = std::to_string(p.items.size()) + (p.items.size() == 1 ? " item" : " items");
        dl->AddText(ImVec2(pos.x + row + 6, pos.y + 6 + ImGui::GetFontSize() + 2), ImGui::GetColorU32(ui::kTextDim), sub.c_str());
        if (hov && ImGui::IsMouseDoubleClicked(0)) AddPicture(app, st, p.image, BoardKind::Character, st.centerX - 115, st.centerY - 120);
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
            ImGui::SetDragDropPayload("MV_BOARDCHAR", p.image.c_str(), (p.image.size() + 1) * sizeof(wchar_t));
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(name.c_str());
            ImGui::TextDisabled("Drop it on the board");
            ImGui::EndTooltip();
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginPopupContextItem("##charctx")) {
            if (ImGui::MenuItem("Add to the board")) AddPicture(app, st, p.image, BoardKind::Character, st.centerX - 115, st.centerY - 120);
            if (ImGui::MenuItem("Boards with this character")) {
                st.showBrowser = true;
                st.browserChar = p.key;
                st.browserDirty = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Remove from saved characters")) {
                p.saved = false;
                TouchProfiles(st);
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (shown == 0) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("No saved characters yet. Select a character card on the board and press \"Save character\": its name, description and "
                            "inventory are kept here, ready to drag onto any board.");
        ImGui::PopTextWrapPos();
    } else {
        ImGui::TextDisabled("Double-click or drag onto the board");
    }
}

void DrawLibraryPanel(AppState& app, BoardViewState& st, float height) {
    ImGui::BeginChild("board_lib", ImVec2(0, height), true);
    {
        int mode = st.libShowChars ? 1 : st.libShowIcons ? 2 : 0;
        const char* modes[] = {"Vision library pictures", "Saved characters", "Icons: clothes, bags, cars..."};
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##libmode", &mode, modes, 3)) {
            st.libShowChars = mode == 1;
            st.libShowIcons = mode == 2;
        }
    }
    if (st.libShowIcons) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Drag an icon onto a character for its inventory, or onto the board as an item. Double-click adds it to the selected character.");
        ImGui::PopTextWrapPos();
        const std::string pick = IconGrid(app, st, 2, ImGui::GetContentRegionAvail().x, std::max(ui::Dp(120.0f), ImGui::GetContentRegionAvail().y - ui::Dp(36.0f)), true);
        if (!pick.empty()) {
            // double-click: onto the selected character's inventory, otherwise as an item card
            const BoardNode* sel = st.selNode >= 0 ? st.board.Find(st.selNode) : nullptr;
            const IconRef picked = ResolveIcon(pick);
            if (sel && sel->kind == BoardKind::Character && !sel->image.empty() && picked.category != "Backgrounds") {
                AddInvIcon(st, EnsureProfileOf(st.board, sel->id), pick);
                st.invFocus = true;
            } else {
                AddIconCard(app, st, pick, st.centerX - 60, st.centerY - 60);
            }
        }
        ImGui::EndChild();
        return;
    }
    if (st.libShowChars) {
        ImGui::BeginChild("##savedchars", ImVec2(0, 0), false);
        DrawSavedCharacters(app, st);
        ImGui::EndChild();
        ImGui::EndChild();
        return;
    }
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Drag a picture onto the board");
    ImGui::PopTextWrapPos();
    InputLine("##boardsearch", "Search the library", st.search, -FLT_MIN);
    ImGui::SetNextItemWidth(-FLT_MIN);
    const char* kinds[] = {"Add as: Character", "Add as: Background", "Add as: Item"};
    ImGui::Combo("##boardaddkind", &st.addKind, kinds, 3);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("What a picture becomes when you drop it on the board (right click a picture for another role)");
    ImGui::Checkbox("Describe new pictures", &st.autoDescribe);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The vision model writes the card's description for its role (character / setting / object)");

    app.RequestVisionLibrary();
    static std::vector<int> shown;
    static unsigned shownVersion = ~0u;
    static std::string shownSearch = "\x01";
    if (shownVersion != app.visionLibVersion || shownSearch != st.search) {
        shown.clear();
        const std::string q = Lower(st.search);
        for (size_t i = 0; i < app.visionLib.size(); ++i) {
            if (q.empty() || app.visionLib[i].searchLower.find(q) != std::string::npos) shown.push_back(static_cast<int>(i));
        }
        shownVersion = app.visionLibVersion;
        shownSearch = st.search;
    }
    ImGui::TextDisabled("%d pictures%s", static_cast<int>(shown.size()), app.VisionLibraryScanning() ? "  (reading...)" : "");
    if (app.visionLib.empty() && !app.VisionLibraryScanning()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("The library is empty. Describe some pictures with the Vision prompt window first: they are saved into the Vision "
                            "output folder and show up here.");
        ImGui::PopTextWrapPos();
    }

    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float cell = ui::Dp(86.0f);
    const float avail = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize;
    const int cols = std::max(1, static_cast<int>((avail + spacing) / (cell + spacing)));
    const int rows = (static_cast<int>(shown.size()) + cols - 1) / cols;
    ImGui::BeginChild("board_lib_grid", ImVec2(0, 0), false);
    ImGuiListClipper clipper;
    clipper.Begin(rows, cell + spacing);
    while (clipper.Step()) {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
            for (int c = 0; c < cols; ++c) {
                const int k = r * cols + c;
                if (k >= static_cast<int>(shown.size())) break;
                const AppState::VisionLibItem& it = app.visionLib[shown[k]];
                if (c > 0) ImGui::SameLine();
                ImGui::PushID(shown[k]);
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::InvisibleButton("##t", ImVec2(cell, cell));
                const bool hov = ImGui::IsItemHovered();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(p, ImVec2(p.x + cell, p.y + cell), Rgb(0x1E1E22), 4.0f);
                const AppState::LibThumb t = app.LibraryThumb(it.imagePath);
                if (t.srv && t.w > 0) {
                    const float kz = std::min(cell / t.w, cell / t.h);
                    const float iw = t.w * kz, ih = t.h * kz;
                    const ImVec2 ma(p.x + (cell - iw) * 0.5f, p.y + (cell - ih) * 0.5f);
                    dl->AddImage((ImTextureID)(intptr_t)t.srv, ma, ImVec2(ma.x + iw, ma.y + ih));
                }
                if (hov) dl->AddRect(p, ImVec2(p.x + cell, p.y + cell), ImGui::GetColorU32(ui::kAccent), 4.0f, 0, 2.0f);
                if (hov && ImGui::IsMouseDoubleClicked(0)) {
                    AddPicture(app, st, it.imagePath, static_cast<BoardKind>(std::clamp(st.addKind, 0, 2)), st.centerX - 115, st.centerY - 120);
                }
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
                    ImGui::SetDragDropPayload("MV_LIBITEM", it.imagePath.c_str(), (it.imagePath.size() + 1) * sizeof(wchar_t));
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted(ToUtf8(it.name).c_str());
                    ImGui::TextDisabled("Drop it on the board");
                    ImGui::EndTooltip();
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginPopupContextItem("##libctx")) {
                    for (int kd = 0; kd < 3; ++kd) {
                        const std::string label = std::string("Add as ") + BoardKindName(static_cast<BoardKind>(kd));
                        if (ImGui::MenuItem(label.c_str())) {
                            AddPicture(app, st, it.imagePath, static_cast<BoardKind>(kd), st.centerX - 115, st.centerY - 120);
                        }
                    }
                    ImGui::EndPopup();
                } else if (hov && !ImGui::IsMouseDown(0)) {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted(ToUtf8(it.name).c_str());
                    const std::string snip = it.prompt.size() > 220 ? it.prompt.substr(0, 220) + "..." : it.prompt;
                    if (!snip.empty()) {
                        ImGui::PushTextWrapPos(ui::Dp(320.0f));
                        ImGui::TextDisabled("%s", snip.c_str());
                        ImGui::PopTextWrapPos();
                    }
                    ImGui::TextDisabled("Double-click or drag to add");
                    ImGui::EndTooltip();
                }
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();
    ImGui::EndChild();
}


// ---------------------------------------------------------------------------------------------------------------
// Inventory of a character
bool PlaceButton(bool& first, const char* label) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float bw = ImGui::CalcTextSize(label).x + style.FramePadding.x * 2;
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - style.ScrollbarSize - style.WindowPadding.x;
    if (!first && ImGui::GetItemRectMax().x + style.ItemSpacing.x + bw <= right) ImGui::SameLine();
    first = false;
    return ImGui::Button(label);
}

void AddInvFromFile(BoardViewState& st, CharacterProfile& p, const std::wstring& file, const std::string& name) {
    const std::wstring stored = StoreItemImage(file, name);
    if (stored.empty()) return;
    p.AddItem(InvSlot::Holding, name, stored);
    TouchProfiles(st);
}

void DrawInventory(AppState& app, BoardViewState& st, BoardNode& n) {
    CharacterProfile& p = EnsureProfileOf(st.board, n.id);
    ImGui::SeparatorText("Inventory");
    if (st.invFocus) {
        ImGui::SetScrollHereY(0.0f);
        st.invFocus = false;
    }
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("What this card wears, holds and carries on THIS board. Use \"Save character\" to keep it for next time.");
    ImGui::PopTextWrapPos();
    // the "+" slot on the card asked for the add menu; the menu's own buttons can ask for the other popups
    if (st.invQuick) {
        ImGui::OpenPopup("##invicons");
        st.invQuick = false;
    }
    if (st.invOpenUrl) {
        st.itemUrl.clear();
        st.itemUrlName.clear();
        ImGui::OpenPopup("##itemurl");
        st.invOpenUrl = false;
    }
    if (st.invOpenSaved) {
        st.savedItems = ListSavedItems();
        ImGui::OpenPopup("##saveditems");
        st.invOpenSaved = false;
    }
    const float cell = ui::Dp(38.0f);
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    int removeAt = -1;
    for (size_t i = 0; i < p.items.size(); ++i) {
        InvItem& it = p.items[i];
        ImGui::PushID(it.id);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(cell, cell * 1.6f));
        DrawThumbAt(app, ImGui::GetWindowDrawList(), it.image, pos, ImVec2(pos.x + cell, pos.y + cell * 1.6f));
        ImGui::SameLine();
        ImGui::BeginGroup();
        const float inputW = ImGui::GetContentRegionAvail().x - ui::Dp(30.0f) - spacing;
        if (InputLine("##iname", "item (e.g. water gun)", it.name, inputW)) TouchProfiles(st);
        ImGui::SetNextItemWidth(ui::Dp(88.0f));
        int slot = static_cast<int>(it.slot);
        const char* slots[] = {"Wearing", "Holding", "Carrying", "Vehicle"};
        if (ImGui::Combo("##islot", &slot, slots, 4)) {
            it.slot = static_cast<InvSlot>(slot);
            TouchProfiles(st);
        }
        ImGui::SameLine();
        {
            const std::string pick = IconCombo(app, st, "##iicon", IconIdOfPath(it.image), ui::Dp(84.0f), 0);
            if (!pick.empty()) {
                const IconRef r = ResolveIcon(pick);
                if (r.ok) {
                    it.image = r.path;
                    if (it.name.empty()) it.name = LowerCopy(r.name);
                    TouchProfiles(st);
                }
            }
        }
        ImGui::SameLine();
        if (InputLine("##idetail", "detail", it.text, inputW - ui::Dp(88.0f) - ui::Dp(84.0f) - 2 * spacing)) TouchProfiles(st);
        ImGui::EndGroup();
        ImGui::SameLine();
        if (ImGui::Button("x", ImVec2(ui::Dp(26.0f), 0))) removeAt = static_cast<int>(i);
        ImGui::PopID();
    }
    if (removeAt >= 0) {
        p.items.erase(p.items.begin() + removeAt);
        TouchProfiles(st);
    }
    bool first = true;
    if (PlaceButton(first, "+ Item")) {
        p.AddItem(InvSlot::Holding, std::string(), std::wstring());
        TouchProfiles(st);
    }
    if (PlaceButton(first, "Paste picture")) {
        std::string msg;
        const std::wstring pasted = PasteImage(msg);
        if (!pasted.empty()) AddInvFromFile(st, p, pasted, "item");
        else app.SetStatus(msg);
    }
    if (PlaceButton(first, "File...")) {
        const std::wstring file = OpenImageFileDialog(GetActiveWindow());
        if (!file.empty()) AddInvFromFile(st, p, file, ToUtf8(fs::path(file).stem().wstring()));
    }
    if (PlaceButton(first, "Web link...")) {
        st.itemUrl.clear();
        st.itemUrlName.clear();
        ImGui::OpenPopup("##itemurl");
    }
    if (PlaceButton(first, "Icons...")) ImGui::OpenPopup("##invicons");
    if (PlaceButton(first, "Saved items...")) {
        st.savedItems = ListSavedItems();
        ImGui::OpenPopup("##saveditems");
    }
    if (st.urlJob.valid()) ImGui::TextDisabled("Downloading the picture...");

    if (ImGui::BeginPopup("##itemurl")) {
        ImGui::TextUnformatted("Picture from the web");
        ImGui::TextDisabled("In your browser: right click the picture > Copy image address");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        InputLine("##itemurlfield", "https://...", st.itemUrl, ui::Dp(380.0f));
        InputLine("##itemurlname", "name (e.g. water gun)", st.itemUrlName, ui::Dp(380.0f));
        ImGui::BeginDisabled(st.itemUrl.empty() || st.urlJob.valid());
        if (ImGui::Button("Download and add")) {
            const std::string url = st.itemUrl, name = st.itemUrlName.empty() ? std::string("item") : st.itemUrlName;
            st.urlJobKey = p.key;
            st.urlJobName = name;
            st.urlJob = std::async(std::launch::async, [url, name]() {
                std::wstring stored;
                std::string error;
                DownloadItemImage(url, name, stored, error);
                return std::make_pair(stored, error);
            });
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##invicons")) {
        ImGui::TextUnformatted("Add to the inventory of this character");
        if (ImGui::Button("Paste picture")) {
            std::string msg;
            const std::wstring pasted = PasteImage(msg);
            if (!pasted.empty()) AddInvFromFile(st, p, pasted, "item");
            else app.SetStatus(msg);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("File...")) {
            const std::wstring file = OpenImageFileDialog(GetActiveWindow());
            if (!file.empty()) AddInvFromFile(st, p, file, ToUtf8(fs::path(file).stem().wstring()));
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Web link...")) {
            st.invOpenUrl = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Saved items...")) {
            st.invOpenSaved = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Blank item")) {
            p.AddItem(InvSlot::Holding, std::string(), std::wstring());
            TouchProfiles(st);
            ImGui::CloseCurrentPopup();
        }
        ImGui::TextDisabled("or pick an icon:");
        const std::string pick = IconGrid(app, st, 0, ui::Dp(460.0f), ui::Dp(330.0f), false);
        if (!pick.empty()) {
            AddInvIcon(st, p, pick);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##saveditems")) {
        ImGui::TextUnformatted("Saved item pictures");
        ImGui::TextDisabled("Every picture you add to an inventory is kept in %s", ToUtf8(ItemsDir()).c_str());
        if (st.savedItems.empty()) ImGui::TextDisabled("Nothing saved yet.");
        const float box = ui::Dp(72.0f);
        ImGui::BeginChild("##saveditemsgrid", ImVec2(ui::Dp(440.0f), ui::Dp(300.0f)), true);
        const int cols = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + spacing) / (box + spacing)));
        for (size_t k = 0; k < st.savedItems.size(); ++k) {
            if (k % cols != 0) ImGui::SameLine();
            ImGui::PushID(static_cast<int>(k));
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##si", ImVec2(box, box))) {
                p.AddItem(InvSlot::Holding, st.savedItems[k].name, st.savedItems[k].path);
                TouchProfiles(st);
                ImGui::CloseCurrentPopup();
            }
            DrawThumbAt(app, ImGui::GetWindowDrawList(), st.savedItems[k].path, pos, ImVec2(pos.x + box, pos.y + box));
            if (ImGui::IsItemHovered()) {
                ImGui::GetWindowDrawList()->AddRect(pos, ImVec2(pos.x + box, pos.y + box), ImGui::GetColorU32(ui::kAccent), 3.0f, 0, 2.0f);
                ImGui::SetTooltip("%s", st.savedItems[k].name.c_str());
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::EndPopup();
    }

    // the boards this character is on
    EnsureBoardIndex(st);
    const int count = BoardsWithCharacter(st, p.key);
    ImGui::Spacing();
    if (count > 0) ImGui::TextDisabled("On %d saved board%s", count, count == 1 ? "" : "s");
    else ImGui::TextDisabled("Not on a saved board yet");
    if (ImGui::SmallButton("Show the boards of this character")) {
        st.showBrowser = true;
        st.browserChar = p.key;
        st.browserDirty = true;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Beats: every beat is its own board, shown from a start time to an end time
std::string SecLabel(float t) {
    char buf[32];
    if (std::fabs(t - std::round(t)) < 0.05f) std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(std::lround(t)));
    else std::snprintf(buf, sizeof(buf), "%.1f", t);
    return buf;
}

std::string BeatLabel(const Board& b, int i) {
    const BoardScene& s = b.scenes[i];
    std::string label = std::to_string(i + 1) + ": " + SecLabel(s.start) + "-" + SecLabel(s.end) + " s";
    if (!s.name.empty()) label += "  " + s.name;
    return label;
}

void SelectBeat(BoardViewState& st, int i) {
    if (i < 0 || i >= static_cast<int>(st.board.scenes.size()) || i == st.curScene) return;
    st.curScene = i;
    st.selNode = st.selLink = -1;
    st.fitRequest = true;
    st.tlReveal = i;
}

void AddBeatAfterCurrent(BoardViewState& st, bool copy) {
    st.curScene = st.board.AddSceneAfter(st.curScene, copy);
    st.board.Ripple(st.curScene);
    st.selNode = st.selLink = -1;
    st.fitRequest = true;
    st.tlReveal = st.curScene;
    Touch(st);
}

void DrawSceneTabs(BoardViewState& st) {
    Board& b = st.board;
    if (ImGui::Button("<") ) SelectBeat(st, st.curScene - 1);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Previous scene");
    ImGui::SameLine();
    if (ImGui::Button(">")) SelectBeat(st, st.curScene + 1);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Next scene");
    for (int i = 0; i < static_cast<int>(b.scenes.size()); ++i) {
        ImGui::PushID(i);
        ImGui::SameLine();
        const bool active = i == st.curScene;
        const ImVec4 col = ImGui::ColorConvertU32ToFloat4(Rgb(SceneColor(i), active ? 255 : 110));
        ImGui::PushStyleColor(ImGuiCol_Button, col);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(Rgb(SceneColor(i), 200)));
        if (ImGui::Button((BeatLabel(b, i) + "##beat").c_str())) SelectBeat(st, i);
        ImGui::PopStyleColor(2);
        if (ImGui::BeginPopupContextItem("##beatctx")) {
            if (ImGui::MenuItem("Rename...")) {
                st.renameScene = i;
                st.sceneBuf = b.scenes[i].name;
                ImGui::CloseCurrentPopup();
                st.relPickLink = -2; // opens the rename popup below
            }
            if (ImGui::MenuItem("Delete this scene", nullptr, false, b.scenes.size() > 1)) {
                b.RemoveScene(i);
                st.curScene = std::clamp(st.curScene >= i && st.curScene > 0 ? st.curScene - 1 : st.curScene, 0, static_cast<int>(b.scenes.size()) - 1);
                st.selNode = st.selLink = -1;
                st.fitRequest = true;
                Touch(st);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    ImGui::SameLine();
    if (ImGui::Button("+ Scene")) AddBeatAfterCurrent(st, true);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A new scene after this one, starting as a copy of this scene's board: change the relations, add a card...");
    ImGui::SameLine();
    if (ImGui::Button("+ Blank")) AddBeatAfterCurrent(st, false);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A new scene with an empty board (a different place, other characters)");
    if (st.relPickLink == -2) {
        st.relPickLink = -1;
        ImGui::OpenPopup("##scenerename");
    }
    if (ImGui::BeginPopup("##scenerename")) {
        ImGui::TextUnformatted("Name this scene");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputTextWithHint("##scenename", "e.g. Plane fight", st.sceneBuf.data(), st.sceneBuf.capacity() + 1,
                                                    ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_EnterReturnsTrue, StdStringResize, &st.sceneBuf);
        if ((ImGui::Button("OK") || enter) && st.renameScene >= 0 && st.renameScene < static_cast<int>(b.scenes.size())) {
            b.scenes[st.renameScene].name = st.sceneBuf;
            Touch(st);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void DrawTimeline(AppState& app, BoardViewState& st, float height) {
    (void)app;
    Board& b = st.board;
    ImGui::BeginChild("##timeline", ImVec2(0, height), true, ImGuiWindowFlags_NoScrollWithMouse);
    if (ImGui::SmallButton(st.showTimeline ? "v" : ">")) st.showTimeline = !st.showTimeline;
    ImGui::SameLine();
    ImGui::TextUnformatted("Timeline");
    ImGui::SameLine();
    ImGui::TextDisabled("%d scene%s, %.1f s in all. Drag a scene to move it, drag its edges to trim, click the ruler to scrub.", static_cast<int>(b.scenes.size()),
                        b.scenes.size() == 1 ? "" : "s", b.TotalSec());
    if (!st.showTimeline) {
        ImGui::EndChild();
        return;
    }

    // ---- editing tools (a video editor's: split, clone, delete, ripple delete, reorder, snap)
    {
        const int cur = std::clamp(st.curScene, 0, static_cast<int>(b.scenes.size()) - 1);
        auto splitHere = [&]() {
            const int at = b.SplitScene(cur, st.playhead);
            if (at >= 0) {
                st.tlReveal = at;
                st.curScene = at;
                st.selNode = st.selLink = -1;
                st.fitRequest = true;
                Touch(st);
            } else {
                app.SetStatus("Put the playhead (click the ruler) inside the scene you want to cut");
            }
        };
        auto cloneHere = [&]() {
            const int at = b.CloneScene(cur);
            if (at >= 0) {
                st.tlReveal = at;
                st.curScene = at;
                st.selNode = st.selLink = -1;
                st.fitRequest = true;
                Touch(st);
            }
        };
        auto deleteHere = [&](bool ripple) {
            if (b.scenes.size() <= 1) {
                app.SetStatus("A board needs at least one scene");
                return;
            }
            b.RemoveScene(cur);
            if (ripple) b.CloseGap(std::min(cur, static_cast<int>(b.scenes.size()) - 1));
            st.curScene = std::clamp(cur > 0 ? cur - 1 : 0, 0, static_cast<int>(b.scenes.size()) - 1);
            st.selNode = st.selLink = -1;
            st.fitRequest = true;
            Touch(st);
        };
        if (ImGui::Button("Split (S)")) splitHere();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Cut the selected scene in two at the playhead (both halves keep the board)");
        ImGui::SameLine();
        if (ImGui::Checkbox("Razor (C)", &st.tlRazor)) {}
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("With the razor on, clicking a scene on the timeline cuts it right there");
        ImGui::SameLine();
        if (ImGui::Button("Clone (Ctrl+D)")) cloneHere();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("A copy of the selected scene right after it (Alt+drag a scene does the same)");
        ImGui::SameLine();
        if (ImGui::Button("Delete")) deleteHere(false);
        ImGui::SameLine();
        if (ImGui::Button("Ripple delete")) deleteHere(true);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Delete the scene and pull the later ones earlier to close the gap (Shift+Delete)");
        ImGui::SameLine();
        if (ImGui::Button("<<")) {
            if (b.SwapScene(cur, -1)) {
                st.curScene = cur - 1;
                Touch(st);
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Move this scene earlier in the order");
        ImGui::SameLine();
        if (ImGui::Button(">>")) {
            if (b.SwapScene(cur, 1)) {
                st.curScene = cur + 1;
                Touch(st);
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Move this scene later in the order");
        ImGui::SameLine();
        ImGui::Checkbox("Snap", &st.tlSnap);
        ImGui::SameLine();
        if (ImGui::Button("Fit")) st.tlLen = 0;

        // keyboard: only while the mouse is over the timeline and no text box is being typed in
        const ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !io.WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_S, false) && !io.KeyCtrl) splitHere();
            if (ImGui::IsKeyPressed(ImGuiKey_C, false) && !io.KeyCtrl) st.tlRazor = !st.tlRazor;
            if (ImGui::IsKeyPressed(ImGuiKey_D, false) && io.KeyCtrl) cloneHere();
            if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) deleteHere(io.KeyShift);
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) st.playhead = std::max(0.0f, st.playhead - (io.KeyShift ? 1.0f : 0.1f));
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) st.playhead += io.KeyShift ? 1.0f : 0.1f;
        }
    }

    // ---- the timeline itself: ruler + one track. Drag a beat to move it, drag its edges to trim it, click the ruler to place the playhead.
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = std::max(50.0f, ImGui::GetContentRegionAvail().x);
        const float rulerH = ui::Dp(20.0f), trackH = ui::Dp(52.0f), h = rulerH + trackH;
        ImGui::InvisibleButton("##tlbar", ImVec2(w, h), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
        const bool hovered = ImGui::IsItemHovered();
        ImGuiIO& io = ImGui::GetIO();
        const ImVec2 mouse = io.MousePos;
        const float total = std::max(1.0f, b.TotalSec());
        // tlLen 0 = "fit": the whole timeline is always in view, so a scene you add can never be off screen
        if (st.tlLen <= 0.0f) st.tlStart = 0;
        float viewLen = st.tlLen > 0.0f ? st.tlLen : total * 1.08f + 0.5f;
        if (st.tlReveal >= 0 && st.tlReveal < static_cast<int>(b.scenes.size()) && st.tlLen > 0.0f) { // bring that scene into view
            const BoardScene& rs = b.scenes[st.tlReveal];
            const float margin = std::max(0.3f, viewLen * 0.06f);
            if (rs.end - rs.start + 2 * margin > viewLen) {
                st.tlLen = viewLen = rs.end - rs.start + 2 * margin;
                st.tlStart = std::max(0.0f, rs.start - margin);
            } else if (rs.start - margin < st.tlStart) {
                st.tlStart = std::max(0.0f, rs.start - margin);
            } else if (rs.end + margin > st.tlStart + viewLen) {
                st.tlStart = rs.end + margin - viewLen;
            }
        }
        st.tlReveal = -1;
        // wheel: zoom around the pointer (the time under the mouse stays under the mouse); Shift / Alt / sideways wheel: scroll
        if (hovered && (io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f)) {
            if (io.KeyShift || io.KeyAlt || io.MouseWheelH != 0.0f) {
                const float d = io.MouseWheelH != 0.0f ? io.MouseWheelH : io.MouseWheel;
                if (st.tlLen <= 0.0f) st.tlLen = viewLen;
                st.tlStart = std::max(0.0f, st.tlStart - d * viewLen * 0.12f);
            } else {
                const float tm = st.tlStart + (mouse.x - p.x) / (w / viewLen);
                const float newLen = std::clamp(viewLen * std::pow(0.8f, io.MouseWheel), 0.5f, 3600.0f);
                st.tlLen = viewLen = newLen;
                st.tlStart = std::max(0.0f, tm - (mouse.x - p.x) / w * newLen);
            }
        }
        const float pps = w / viewLen; // pixels per second
        auto toX = [&](float t) { return p.x + (t - st.tlStart) * pps; };
        auto toT = [&](float x) { return st.tlStart + (x - p.x) / pps; };
        auto round1 = [](float v) { return std::round(v * 10.0f) / 10.0f; };
        float gridStep = 60.0f; // the ruler's tick spacing: things also snap to it
        for (float s : {0.1f, 0.2f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 15.0f, 30.0f, 60.0f, 120.0f, 300.0f}) {
            gridStep = s;
            if (pps * s >= ui::Dp(48.0f)) break;
        }
        float snapGuide = -1.0f; // where the last snap landed (drawn as a yellow line)
        // snapping: magnets on 0, the playhead and every scene's start / end, then on the ruler ticks
        auto snap = [&](float t, int skipScene) {
            if (!st.tlSnap || io.KeyCtrl) return round1(t);
            const float reach = ui::Dp(12.0f) / pps;
            float best = t, bestD = reach;
            auto consider = [&](float c) {
                if (std::fabs(c - t) < bestD) {
                    bestD = std::fabs(c - t);
                    best = c;
                }
            };
            consider(0.0f);
            consider(st.playhead);
            for (int i = 0; i < static_cast<int>(b.scenes.size()); ++i) {
                if (i == skipScene) continue;
                consider(b.scenes[i].start);
                consider(b.scenes[i].end);
            }
            if (bestD < reach) {
                snapGuide = best;
                return best;
            }
            const float g = std::round(t / gridStep) * gridStep;
            if (std::fabs(g - t) < reach) {
                snapGuide = g;
                return g;
            }
            return round1(t);
        };
        const ImVec2 trackA(p.x, p.y + rulerH), trackB(p.x + w, p.y + h);
        auto sceneAt = [&](float t) {
            for (int i = 0; i < static_cast<int>(b.scenes.size()); ++i)
                if (t >= b.scenes[i].start && t < b.scenes[i].end) return i;
            return -1;
        };
        // which beat / edge is under the mouse
        int hitScene = -1, hitEdge = 0; // edge: -1 start, +1 end
        if (hovered && mouse.y >= trackA.y) {
            const float edge = ui::Dp(7.0f);
            for (int i = static_cast<int>(b.scenes.size()) - 1; i >= 0 && hitScene < 0; --i) {
                const float x0 = toX(b.scenes[i].start), x1 = toX(b.scenes[i].end);
                if (mouse.x >= x0 - 2 && mouse.x <= x1 + 2) {
                    hitScene = i;
                    if (mouse.x <= x0 + edge) hitEdge = -1;
                    else if (mouse.x >= x1 - edge) hitEdge = 1;
                }
            }
        }
        if (hovered && hitScene >= 0 && hitEdge != 0 && !st.tlRazor) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        else if (hovered && hitScene >= 0 && st.tlRazor) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

        if (hovered && ImGui::IsMouseDoubleClicked(0) && mouse.y >= trackA.y && hitScene < 0) { // a quick new scene right where you clicked
            const float t0 = std::max(0.0f, snap(toT(mouse.x), -1));
            float prevEnd = 0, nextStart = 1e9f;
            for (const BoardScene& sc : b.scenes) {
                if (sc.end <= t0 + 0.001f) prevEnd = std::max(prevEnd, sc.end);
                else if (sc.start > t0) nextStart = std::min(nextStart, sc.start);
            }
            const float ns = std::max(t0, prevEnd), ne = std::min(ns + 3.0f, nextStart);
            if (ne - ns >= 0.3f) {
                BoardScene s;
                s.start = ns;
                s.end = ne;
                b.scenes.push_back(std::move(s));
                int cur = static_cast<int>(b.scenes.size()) - 1;
                b.SortScenes(&cur);
                st.curScene = cur;
                st.selNode = st.selLink = -1;
                st.fitRequest = true;
                st.tlReveal = cur;
                Touch(st);
            } else {
                app.SetStatus("No room for a scene there: trim a neighbour first");
            }
            st.tlMode = 0;
        } else if (ImGui::IsItemClicked(0)) {
            if (mouse.y < trackA.y || hitScene < 0) { // the ruler (or empty track): place the playhead and look at that moment
                st.tlMode = 1;
            } else if (st.tlRazor) { // razor: cut here
                const int at = b.SplitScene(hitScene, snap(toT(mouse.x), -1));
                if (at >= 0) {
                    st.curScene = at;
                    st.selNode = st.selLink = -1;
                    st.fitRequest = true;
                    Touch(st);
                }
            } else {
                int target = hitScene;
                if (hitEdge == 0 && io.KeyAlt) { // Alt + drag: clone, then drag the clone
                    const int clone = b.CloneScene(hitScene);
                    if (clone >= 0) {
                        target = clone;
                        Touch(st);
                    }
                }
                SelectBeat(st, target);
                st.tlMode = hitEdge < 0 && target == hitScene ? 3 : hitEdge > 0 && target == hitScene ? 4 : 2;
                st.tlGrabScene = target;
                st.tlOrigStart = b.scenes[target].start;
                st.tlOrigEnd = b.scenes[target].end;
                st.tlGrabDx = toT(mouse.x) - b.scenes[target].start;
                st.tlMoved = 0;
            }
        }
        if (ImGui::IsItemClicked(2)) st.tlMode = 5;
        if (st.tlMode != 0 && (io.MouseDown[0] || io.MouseDown[2])) {
            const float t = toT(mouse.x);
            st.tlMoved += std::fabs(io.MouseDelta.x) + std::fabs(io.MouseDelta.y);
            const bool dragging = st.tlMoved >= 3.0f;
            if (st.tlMode == 1) {
                st.playhead = std::max(0.0f, snap(t, -1));
                const int idx = sceneAt(st.playhead);
                if (idx >= 0) SelectBeat(st, idx);
            } else if (st.tlMode == 5) {
                st.tlStart = std::max(0.0f, st.tlStart - io.MouseDelta.x / pps);
            } else if (dragging && st.tlGrabScene >= 0 && st.tlGrabScene < static_cast<int>(b.scenes.size())) {
                const int i = st.tlGrabScene;
                BoardScene& s = b.scenes[i];
                const float prevEnd = i > 0 ? b.scenes[i - 1].end : 0.0f;
                const float nextStart = i + 1 < static_cast<int>(b.scenes.size()) ? b.scenes[i + 1].start : 1e9f;
                if (st.tlMode == 2) { // move: keep the length, snap either edge, stay between the neighbours
                    const float len = st.tlOrigEnd - st.tlOrigStart;
                    float ns = t - st.tlGrabDx;
                    const float sStart = snap(ns, i), sEnd = snap(ns + len, i) - len;
                    ns = std::fabs(sStart - ns) <= std::fabs(sEnd - ns) ? sStart : sEnd;
                    ns = std::clamp(ns, prevEnd, std::max(prevEnd, nextStart - len));
                    if (ns != s.start) Touch(st);
                    s.start = ns;
                    s.end = ns + len;
                } else if (st.tlMode == 3) { // trim the start
                    const float ns = std::clamp(snap(t, i), prevEnd, s.end - 0.1f);
                    if (ns != s.start) Touch(st);
                    s.start = ns;
                } else if (st.tlMode == 4) { // trim the end
                    const float ne = std::clamp(snap(t, i), s.start + 0.1f, nextStart);
                    if (ne != s.end) Touch(st);
                    s.end = ne;
                }
            }
        }
        if (!io.MouseDown[0] && !io.MouseDown[2]) {
            st.tlMode = 0;
            st.tlGrabScene = -1;
        }

        // ---- drawing
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), Rgb(0x1B1B1F), 4.0f);
        dl->PushClipRect(p, ImVec2(p.x + w, p.y + h), true);
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rulerH), Rgb(0x25252B));
        {
            float step = 60;
            for (float s : {0.1f, 0.2f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 15.0f, 30.0f, 60.0f, 120.0f, 300.0f}) {
                step = s;
                if (pps * s >= ui::Dp(48.0f)) break;
            }
            const float first = std::floor(st.tlStart / step) * step;
            for (float s = first; toX(s) < p.x + w + 1; s += step) {
                const float x = toX(s);
                if (x < p.x - 1) continue;
                dl->AddLine(ImVec2(x, p.y + rulerH - 6), ImVec2(x, p.y + h), Rgb(0x34343C), 1.0f);
                char lab[24];
                if (step < 1.0f) std::snprintf(lab, sizeof(lab), "%.1f s", s);
                else if (s >= 60.0f) std::snprintf(lab, sizeof(lab), "%d:%02d", static_cast<int>(s) / 60, static_cast<int>(s) % 60);
                else std::snprintf(lab, sizeof(lab), "%d s", static_cast<int>(std::lround(s)));
                dl->AddText(ImVec2(x + 3, p.y + 2), Rgb(0x8A8A98), lab);
            }
        }
        for (int i = 0; i < static_cast<int>(b.scenes.size()); ++i) {
            const BoardScene& s = b.scenes[i];
            const ImVec2 a(toX(s.start), trackA.y + 3), c(toX(s.end) - 1, trackB.y - 3);
            if (c.x <= p.x || a.x >= p.x + w || c.x <= a.x) continue;
            const bool sel = i == st.curScene;
            dl->AddRectFilled(a, c, Rgb(SceneColor(i), sel ? 235 : 150), 4.0f);
            // a thumbnail of the beat's first card, like a clip in a video editor
            std::wstring thumb;
            for (const BoardNode& n : b.nodes) {
                if (n.scene == i && !n.image.empty()) {
                    thumb = n.image;
                    if (n.kind == BoardKind::Character || thumb.empty()) break;
                }
            }
            const float th = c.y - a.y - 6;
            float textX = a.x + 6;
            if (!thumb.empty() && c.x - a.x > th + 14) {
                DrawThumbAt(app, dl, thumb, ImVec2(a.x + 3, a.y + 3), ImVec2(a.x + 3 + th, a.y + 3 + th), 3.0f);
                textX = a.x + th + 9;
            }
            dl->PushClipRect(ImVec2(std::max(a.x, p.x), a.y), ImVec2(std::min(c.x, p.x + w), c.y), true);
            const std::string title = std::to_string(i + 1) + (s.name.empty() ? std::string() : "  " + s.name);
            dl->AddText(ImVec2(textX, a.y + 3), IM_COL32(255, 255, 255, 245), title.c_str());
            char len[32];
            std::snprintf(len, sizeof(len), "%.1f s", s.end - s.start);
            dl->AddText(ImVec2(textX, a.y + 3 + ImGui::GetFontSize() + 1), IM_COL32(255, 255, 255, 170), len);
            dl->PopClipRect();
            if (sel) {
                dl->AddRect(a, c, IM_COL32(255, 255, 255, 245), 4.0f, 0, 2.0f);
                // where it starts and where it ends, right on the clip
                char t0[24], t1[24];
                std::snprintf(t0, sizeof(t0), "%.1f", s.start);
                std::snprintf(t1, sizeof(t1), "%.1f s", s.end);
                const float fsz = ImGui::GetFontSize();
                const float w1 = ImGui::CalcTextSize(t1).x;
                if (c.x - a.x > ImGui::CalcTextSize(t0).x + w1 + 24) {
                    dl->AddRectFilled(ImVec2(a.x + 2, c.y - fsz - 3), ImVec2(a.x + 6 + ImGui::CalcTextSize(t0).x, c.y - 1), IM_COL32(0, 0, 0, 150), 3.0f);
                    dl->AddText(ImVec2(a.x + 4, c.y - fsz - 2), IM_COL32(255, 255, 255, 240), t0);
                    dl->AddRectFilled(ImVec2(c.x - w1 - 6, c.y - fsz - 3), ImVec2(c.x - 2, c.y - 1), IM_COL32(0, 0, 0, 150), 3.0f);
                    dl->AddText(ImVec2(c.x - w1 - 4, c.y - fsz - 2), IM_COL32(255, 255, 255, 240), t1);
                }
            }
            // trim handles
            if (sel || (hovered && hitScene == i)) {
                const float hw = ui::Dp(5.0f);
                dl->AddRectFilled(a, ImVec2(a.x + hw, c.y), IM_COL32(255, 255, 255, hitEdge < 0 && hitScene == i ? 255 : 150), 4.0f);
                dl->AddRectFilled(ImVec2(c.x - hw, a.y), c, IM_COL32(255, 255, 255, hitEdge > 0 && hitScene == i ? 255 : 150), 4.0f);
            }
            if (hovered && hitScene == i && st.tlMode == 0) {
                ImGui::SetTooltip("%s\n%s", BeatLabel(b, i).c_str(), s.text.c_str());
            }
        }
        // the snap guide while dragging
        if (st.tlMode >= 2 && st.tlMode <= 4 && st.tlMoved >= 3.0f && snapGuide >= 0.0f) {
            const float x = toX(snapGuide);
            dl->AddLine(ImVec2(x, p.y), ImVec2(x, p.y + h), IM_COL32(255, 214, 80, 255), 2.0f);
            char g[24];
            std::snprintf(g, sizeof(g), "%.1f s", snapGuide);
            dl->AddText(ImVec2(x + 4, p.y + rulerH + 2), IM_COL32(255, 214, 80, 255), g);
        }
        // the playhead
        {
            const float x = toX(st.playhead);
            if (x >= p.x && x <= p.x + w) {
                dl->AddLine(ImVec2(x, p.y), ImVec2(x, p.y + h), IM_COL32(255, 80, 80, 255), 2.0f);
                dl->AddTriangleFilled(ImVec2(x - 6, p.y), ImVec2(x + 6, p.y), ImVec2(x, p.y + 10), IM_COL32(255, 80, 80, 255));
            }
        }
        dl->PopClipRect();
        char phLabel[48];
        std::snprintf(phLabel, sizeof(phLabel), "Playhead %.1f s   (wheel: zoom, Shift+wheel: scroll)", st.playhead);
        ImGui::TextDisabled("%s", phLabel);
    }

    // the beat being edited
    BoardScene& s = b.scenes[std::clamp(st.curScene, 0, static_cast<int>(b.scenes.size()) - 1)];
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Scene %d", st.curScene + 1);
    ImGui::SameLine();
    if (InputLine("##beatname", "name (optional)", s.name, ui::Dp(140.0f))) Touch(st);
    ImGui::SameLine();
    ImGui::TextUnformatted("from");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ui::Dp(120.0f));
    if (ImGui::InputFloat("##beatstart", &s.start, 0.5f, 1.0f, "%.1f s")) {
        s.start = std::max(0.0f, s.start);
        if (s.end < s.start + 0.1f) s.end = s.start + 0.1f;
        Touch(st);
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("to");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ui::Dp(120.0f));
    if (ImGui::InputFloat("##beatend", &s.end, 0.5f, 1.0f, "%.1f s")) {
        s.end = std::max(s.start + 0.1f, s.end);
        Touch(st);
    }
    if (InputLine("##beattext", "what happens in this scene (e.g. he ducks and swings a chair)", s.text, -FLT_MIN)) Touch(st);
    if (!ImGui::IsAnyItemActive()) b.SortScenes(&st.curScene); // re-order once a time box is released
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------------------------------------------
// The browser of saved boards and characters
std::string DateText(long long unixSeconds) {
    if (unixSeconds <= 0) return {};
    std::time_t t = static_cast<std::time_t>(unixSeconds);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    char buf[40];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tmv);
    return buf;
}

void DrawBrowser(AppState& app, BoardViewState& st) {
    if (!st.showBrowser) return;
    ImGui::SetNextWindowSize(ImVec2(ui::Dp(1000.0f), ui::Dp(640.0f)), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Vision boards and characters##boardbrowser", &st.showBrowser)) {
        ImGui::End();
        return;
    }
    EnsureBoardIndex(st);
    InputLine("##bbsearch", "Search boards and characters", st.browserSearch, ui::Dp(320.0f));
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) st.browserDirty = true;
    ImGui::SameLine();
    ImGui::TextDisabled("%d saved boards in %s", static_cast<int>(st.browserBoards.size()), ToUtf8(BoardsDir()).c_str());
    const std::string query = Lower(st.browserSearch);

    // characters: everything with a profile plus everyone who is on a saved board
    struct CharRow {
        std::string key, name;
        std::wstring image;
        int boards = 0;
    };
    std::vector<CharRow> chars;
    auto addChar = [&](const std::string& key, const std::string& name, const std::wstring& image) {
        for (CharRow& c : chars) {
            if (c.key == key) {
                if (c.name.empty()) c.name = name;
                return;
            }
        }
        chars.push_back({key, name, image, 0});
    };
    for (const CharacterProfile& p : AllProfiles()) {
        if (p.key.find('#') == std::string::npos) addChar(p.key, p.name, p.image); // a card's own inventory is not another character
    }
    for (const BoardInfo& bi : st.browserBoards) {
        for (const BoardCharRef& c : bi.characters) addChar(c.key, c.name, c.image);
    }
    for (CharRow& c : chars) {
        c.boards = BoardsWithCharacter(st, c.key);
        if (c.name.empty()) c.name = ToUtf8(fs::path(c.image).stem().wstring());
    }
    std::sort(chars.begin(), chars.end(), [](const CharRow& a, const CharRow& c) { return a.boards != c.boards ? a.boards > c.boards : a.name < c.name; });

    if (ImGui::BeginTable("##bbtable", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings, ImVec2(0, 0))) {
        ImGui::TableSetupColumn("Characters", ImGuiTableColumnFlags_WidthFixed, ui::Dp(260.0f));
        ImGui::TableSetupColumn("Boards", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::BeginChild("##bbchars", ImVec2(0, 0), false);
        if (ImGui::Selectable("All boards", st.browserChar.empty())) st.browserChar.clear();
        ImGui::Separator();
        const float row = ui::Dp(44.0f);
        for (const CharRow& c : chars) {
            if (!query.empty() && Lower(c.name).find(query) == std::string::npos) continue;
            ImGui::PushID(c.key.c_str());
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x;
            if (ImGui::Selectable("##crow", st.browserChar == c.key, 0, ImVec2(w - ui::Dp(58.0f), row))) st.browserChar = c.key;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            DrawThumbAt(app, dl, c.image, ImVec2(pos.x + 2, pos.y + 2), ImVec2(pos.x + row - 2, pos.y + row - 2));
            dl->AddText(ImVec2(pos.x + row + 6, pos.y + 4), ImGui::GetColorU32(ImGuiCol_Text), c.name.c_str());
            const std::string sub = std::to_string(c.boards) + (c.boards == 1 ? " board" : " boards");
            dl->AddText(ImVec2(pos.x + row + 6, pos.y + 4 + ImGui::GetFontSize() + 2), ImGui::GetColorU32(ui::kTextDim), sub.c_str());
            ImGui::SameLine();
            if (ImGui::Button("Add", ImVec2(ui::Dp(52.0f), row)) && !c.image.empty()) {
                AddPicture(app, st, c.image, BoardKind::Character, st.centerX - 115, st.centerY - 120);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Put this character on the board you are editing");
            ImGui::PopID();
        }
        ImGui::EndChild();

        ImGui::TableSetColumnIndex(1);
        ImGui::BeginChild("##bbboards", ImVec2(0, 0), false);
        if (!st.browserChar.empty()) {
            std::string who = st.browserChar;
            for (const CharRow& c : chars) {
                if (c.key == st.browserChar) who = c.name;
            }
            ImGui::TextUnformatted(("Boards with " + who).c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Show all")) st.browserChar.clear();
        }
        const float cw = ui::Dp(210.0f), ch = ui::Dp(200.0f);
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const int cols = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + spacing) / (cw + spacing)));
        int shown = 0;
        for (const BoardInfo& bi : st.browserBoards) {
            if (!st.browserChar.empty()) {
                bool has = false;
                for (const BoardCharRef& c : bi.characters) has = has || c.key == st.browserChar;
                if (!has) continue;
            }
            if (!query.empty()) {
                bool match = Lower(bi.name).find(query) != std::string::npos;
                for (const BoardCharRef& c : bi.characters) match = match || Lower(c.name).find(query) != std::string::npos;
                if (!match) continue;
            }
            if (shown % cols != 0) ImGui::SameLine();
            ++shown;
            ImGui::PushID(ToUtf8(bi.path).c_str());
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const bool open = ImGui::InvisibleButton("##card", ImVec2(cw, ch));
            const bool hov = ImGui::IsItemHovered();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const bool current = !st.name.empty() && st.name == bi.name;
            dl->AddRectFilled(pos, ImVec2(pos.x + cw, pos.y + ch), Rgb(hov ? 0x2E2E36 : 0x26262C), 6.0f);
            DrawThumbAt(app, dl, bi.thumb, ImVec2(pos.x + 6, pos.y + 6), ImVec2(pos.x + cw - 6, pos.y + ch - 70));
            // the characters on it, as small portraits over the picture
            for (size_t k = 0; k < bi.characters.size() && k < 6; ++k) {
                const float s = ui::Dp(26.0f);
                const ImVec2 a(pos.x + 10 + k * (s + 3), pos.y + ch - 70 - s - 4);
                DrawThumbAt(app, dl, bi.characters[k].image, a, ImVec2(a.x + s, a.y + s));
                dl->AddRect(a, ImVec2(a.x + s, a.y + s), IM_COL32(255, 255, 255, 160), 3.0f);
            }
            dl->AddText(ImVec2(pos.x + 8, pos.y + ch - 64), ImGui::GetColorU32(ImGuiCol_Text), bi.name.c_str());
            std::string sub = std::to_string(bi.cards) + " cards";
            if (bi.scenes > 1) sub += ", " + std::to_string(bi.scenes) + " scenes";
            dl->AddText(ImVec2(pos.x + 8, pos.y + ch - 64 + ImGui::GetFontSize() + 3), ImGui::GetColorU32(ui::kTextDim), sub.c_str());
            dl->AddText(ImVec2(pos.x + 8, pos.y + ch - 64 + 2 * (ImGui::GetFontSize() + 3)), ImGui::GetColorU32(ui::kTextDim), DateText(bi.modified).c_str());
            if (current || hov) dl->AddRect(pos, ImVec2(pos.x + cw, pos.y + ch), current ? Rgb(0x6EE2FF) : ImGui::GetColorU32(ui::kAccent), 6.0f, 0, 2.0f);
            if (hov) {
                std::string who;
                for (const BoardCharRef& c : bi.characters) who += (who.empty() ? "" : ", ") + c.name;
                if (!who.empty()) ImGui::SetTooltip("%s", who.c_str());
            }
            if (open) {
                OpenBoardFile(app, st, bi.path);
                app.SetStatus("Opened the board \"" + bi.name + "\"");
            }
            if (ImGui::BeginPopupContextItem("##boardmenu")) {
                if (ImGui::MenuItem("Open")) OpenBoardFile(app, st, bi.path);
                if (ImGui::MenuItem("Show in Explorer")) {
                    const std::wstring args = L"/select,\"" + bi.path + L"\"";
                    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Delete...")) st.deletePath = bi.path;
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        if (shown == 0) ImGui::TextDisabled(st.browserBoards.empty() ? "No saved boards yet. Use Save as... on the board, or press New: a board with cards on it is kept automatically." : "No board matches.");
        ImGui::EndChild();
        ImGui::EndTable();
    }
    if (!st.deletePath.empty()) {
        ImGui::OpenPopup("Delete board?");
        if (ImGui::BeginPopupModal("Delete board?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(ToUtf8(fs::path(st.deletePath).stem().wstring()).c_str());
            ImGui::TextDisabled("It goes to the Recycle Bin. The pictures on it are not touched.");
            if (ImGui::Button("Delete")) {
                std::wstring err;
                if (DeleteFilesToRecycleBin(GetActiveWindow(), {st.deletePath}, &err)) {
                    if (!st.name.empty() && NamedPath(st.name) == st.deletePath) st.name.clear();
                    st.browserDirty = true;
                }
                st.deletePath.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                st.deletePath.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------------------------------------------
// Preparing a board for a generator
void DrawExportPanel(AppState& app, BoardViewState& st, const BoardPromptOptions& opt) {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Prepare for generation");
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Copies every picture of the board (characters, background, items, their inventory) into one folder, with the prompt "
                        "next to them, ready to drag or paste into ComfyUI or any other generator.");
    ImGui::PopTextWrapPos();
    const std::string hint = ToUtf8(DefaultExportDir());
    if (InputLine("##exportdir", hint.c_str(), st.exportDir, ImGui::GetContentRegionAvail().x - ui::Dp(78.0f))) SaveBoardSettings(st);
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        const std::wstring picked = OpenFolderDialog(GetActiveWindow());
        if (!picked.empty()) {
            st.exportDir = ToUtf8(picked);
            SaveBoardSettings(st);
        }
    }
    ImGui::BeginDisabled(st.board.nodes.empty());
    if (ImGui::Button("Export pictures + prompt", ImVec2(-FLT_MIN, 0))) {
        std::string name = st.name;
        if (name.empty()) { // an unnamed board gets a folder of its own each time
            std::time_t now = std::time(nullptr);
            std::tm tmv{};
            localtime_s(&tmv, &now);
            char buf[64];
            std::strftime(buf, sizeof(buf), "Untitled board %Y-%m-%d %H-%M-%S", &tmv);
            name = buf;
        }
        const BoardExportResult r = ExportBoardForGeneration(st.board, name, ExportRoot(st), st.result, opt);
        if (!r.error.empty()) {
            app.SetStatus(r.error);
        } else {
            st.lastExportFolder = r.folder;
            st.lastExportFiles = r.files;
            ImGui::SetClipboardText((st.result.empty() ? AssembleBoardPrompt(st.board, opt) : st.result).c_str());
            app.SetStatus("Exported " + std::to_string(r.pictures) + " pictures and the prompt to " + ToUtf8(r.folder) + " (the prompt is on the clipboard)");
        }
    }
    ImGui::EndDisabled();
    if (!st.lastExportFolder.empty()) {
        ImGui::TextDisabled("Last export: %s", ToUtf8(st.lastExportFolder).c_str());
        if (ImGui::Button("Open folder")) ShellExecuteW(nullptr, L"open", st.lastExportFolder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::SameLine();
        if (ImGui::Button("Copy the pictures") && !st.lastExportFiles.empty()) {
            std::string msg;
            CopyImagesToClipboard(GetActiveWindow(), st.lastExportFiles, false, &msg);
            app.SetStatus(msg);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Puts the pictures on the clipboard: paste into a generator (the first as an image, all as files)");
        ImGui::SameLine();
        if (ImGui::Button("Copy the prompt")) ImGui::SetClipboardText((st.result.empty() ? AssembleBoardPrompt(st.board, opt) : st.result).c_str());
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Right: inspector + prompt
void DrawInspector(AppState& app, BoardViewState& st) {
    Board& b = st.board;
    const float w = ImGui::GetContentRegionAvail().x;
    BoardNode* n = st.selNode >= 0 ? b.Find(st.selNode) : nullptr;
    BoardLink* l = nullptr;
    for (BoardLink& x : b.links)
        if (x.id == st.selLink) l = &x;
    if (n) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Rgb(KindColor(n->kind))), "%s card", BoardKindName(n->kind));
        if (n->kind != BoardKind::Note) {
            ImGui::SetNextItemWidth(-FLT_MIN);
            int k = static_cast<int>(n->kind);
            const char* names[] = {"Character", "Background", "Item"};
            if (ImGui::Combo("##nodekind", &k, names, 3)) {
                n->kind = static_cast<BoardKind>(k);
                Touch(st);
            }
        }
        if (InputLine("##nodetitle", "Name (e.g. Knight)", n->title, -FLT_MIN)) {
            Touch(st);
            if (n->kind == BoardKind::Character && !n->image.empty()) {
                EnsureProfile(n->image).name = n->title; // the character keeps its name on every board
                TouchProfiles(st);
            }
        }
        if (InputBox("##nodetext", n->text, w, ui::Dp(n->kind == BoardKind::Note ? 120.0f : 96.0f))) {
            Touch(st);
            if (n->kind == BoardKind::Character && !n->image.empty()) { // a saved character keeps its description up to date
                CharacterProfile& p = EnsureProfile(n->image);
                p.description = n->text;
                if (p.saved) TouchProfiles(st);
            }
        }
        if (n->kind == BoardKind::Item || n->kind == BoardKind::Background) { // a drop-down of icons / scenes (and your own pictures)
            const std::string pick = IconCombo(app, st, "##itemicon", IconIdOfPath(n->image), -FLT_MIN, n->kind == BoardKind::Background ? 1 : 0);
            if (!pick.empty()) {
                const IconRef r = ResolveIcon(pick);
                if (r.ok) {
                    n->image = r.path;
                    if (n->title.empty()) n->title = r.name;
                    if (n->text.empty()) n->text = r.desc.empty() ? LowerCopy(r.name) : r.desc;
                    Touch(st);
                }
            }
        }
        if (n->kind == BoardKind::Character && !n->image.empty()) {
            CharacterProfile& p = EnsureProfile(n->image);
            if (ImGui::Button(p.saved ? "Update saved character" : "Save character")) {
                p.saved = true;
                p.name = n->title;
                p.description = n->text;
                if (const CharacterProfile* own = ProfileOf(*n)) { // the saved character keeps this card's inventory
                    p.items = own->items;
                    p.nextItemId = own->nextItemId;
                }
                SaveProfiles();
                app.SetStatus("Saved " + (p.name.empty() ? std::string("the character") : p.name) + " with its inventory: find it under Saved characters on the left");
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keeps the name, description and inventory so you can add this character to any board");
            if (p.saved) {
                ImGui::SameLine();
                ImGui::TextColored(ui::kGood, "saved");
            }
            if (ImGui::Button("Swap with another character...")) {
                st.swapNode = n->id;
                st.openSwap = true;
            }
            DrawInventory(app, st, *n);
        }
        if (n->kind != BoardKind::Note) {
            ImGui::BeginDisabled(n->describing);
            if (ImGui::Button(n->describing ? "Describing..." : "Describe with the vision model")) StartDescribe(app, st, n->id);
            ImGui::EndDisabled();
        }
        if (ImGui::Button("Remove card")) {
            b.Remove(n->id);
            st.selNode = -1;
            Touch(st);
        }
        // relations of this card
        bool any = false;
        for (const BoardLink& x : b.links) {
            if (x.from != n->id && x.to != n->id) continue;
            const BoardNode *f = b.Find(x.from), *t = b.Find(x.to);
            if (!f || !t) continue;
            if (!any) {
                ImGui::Spacing();
                ImGui::TextDisabled("Relations");
                any = true;
            }
            const std::string line = b.DisplayName(*f) + " " + x.label + " " + b.DisplayName(*t);
            if (ImGui::Selectable(line.c_str())) {
                st.selLink = x.id;
                st.selNode = -1;
            }
        }
    } else if (l) {
        const BoardNode *f = b.Find(l->from), *t = b.Find(l->to);
        ImGui::TextUnformatted("Relation");
        if (f && t) ImGui::TextDisabled("%s  ->  %s", b.DisplayName(*f).c_str(), b.DisplayName(*t).c_str());
        if (InputLine("##linklabel", "what happens (e.g. fights)", l->label, -FLT_MIN)) Touch(st);
        ImGui::TextDisabled("Suggestions");
        ImGui::BeginChild("##presets", ImVec2(0, ui::Dp(250.0f)), true);
        RelationMenu(st, *l);
        ImGui::EndChild();
        if (ImGui::Button("Swap direction")) {
            std::swap(l->from, l->to);
            Touch(st);
        }
        ImGui::SameLine();
        if (ImGui::Button("Remove relation")) {
            b.RemoveLink(l->id);
            st.selLink = -1;
            Touch(st);
        }
    } else {
        ImGui::TextDisabled("Click a card or a relation to edit it.");
        if (ImGui::TreeNode("How the board works")) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("Cards: Character (who), Background (where), Item (what they hold or use), Note (anything else, e.g. the mood or the camera).\n\n"
                                "Relations: drag from the dot on a card's edge to another card and pick what happens: \"fights\", \"holds\", \"stands in\"...\n\n"
                                "Each character card has its own inventory on this board. \"Save character\" keeps it for the Characters tab and for next time.");
            ImGui::PopTextWrapPos();
            ImGui::TreePop();
        }
    }
}


// A tiny diagram of what the chosen shot size keeps in frame: a figure cropped by the camera's frame.
void DrawFramingPreview(ImDrawList* dl, ImVec2 a, ImVec2 b, int shot) {
    // the part of a standing figure (0 = top of the head, 1 = feet) that fills the frame
    static const float region[9][2] = {{-0.10f, 1.10f}, {0.02f, 0.10f}, {-0.02f, 0.22f}, {-0.02f, 0.36f}, {-0.02f, 0.55f},
                                       {-0.02f, 0.68f}, {-0.06f, 1.06f}, {-0.45f, 1.45f}, {-1.5f, 2.5f}};
    const int k = std::clamp(shot, 0, 8);
    const float top = region[k][0], bot = region[k][1];
    const float boxH = b.y - a.y, boxW = b.x - a.x;
    const float fig = boxH / (bot - top); // pixel height of the whole figure
    const float y0 = a.y - top * fig;
    const float cx = (a.x + b.x) * 0.5f;
    dl->AddRectFilled(a, b, IM_COL32(28, 30, 38, 255), 4.0f);
    dl->PushClipRect(a, b, true);
    dl->AddLine(ImVec2(a.x, y0 + fig), ImVec2(b.x, y0 + fig), IM_COL32(70, 74, 90, 255), 1.0f); // the ground
    const ImU32 body = IM_COL32(200, 205, 220, 255);
    const float lw = std::max(1.5f, fig * 0.045f);
    dl->AddCircleFilled(ImVec2(cx, y0 + fig * 0.07f), fig * 0.07f, body, 24);                                        // head
    dl->AddRectFilled(ImVec2(cx - fig * 0.11f, y0 + fig * 0.16f), ImVec2(cx + fig * 0.11f, y0 + fig * 0.50f), body, fig * 0.03f); // torso
    dl->AddLine(ImVec2(cx - fig * 0.13f, y0 + fig * 0.18f), ImVec2(cx - fig * 0.18f, y0 + fig * 0.46f), body, lw);   // arms
    dl->AddLine(ImVec2(cx + fig * 0.13f, y0 + fig * 0.18f), ImVec2(cx + fig * 0.18f, y0 + fig * 0.46f), body, lw);
    dl->AddLine(ImVec2(cx - fig * 0.05f, y0 + fig * 0.50f), ImVec2(cx - fig * 0.06f, y0 + fig * 0.98f), body, lw * 1.3f); // legs
    dl->AddLine(ImVec2(cx + fig * 0.05f, y0 + fig * 0.50f), ImVec2(cx + fig * 0.06f, y0 + fig * 0.98f), body, lw * 1.3f);
    dl->PopClipRect();
    dl->AddRect(a, b, IM_COL32(255, 214, 80, 255), 4.0f, 0, 2.0f);
    (void)boxW;
}

// The camera and lighting of the scene being edited. Every scene keeps its own; a new scene starts with the previous one's.
void DrawCameraPanel(BoardViewState& st) {
    Board& b = st.board;
    BoardScene& sc = b.scenes[std::clamp(st.curScene, 0, static_cast<int>(b.scenes.size()) - 1)];
    if (!ImGui::CollapsingHeader("Camera and lighting", ImGuiTreeNodeFlags_DefaultOpen)) return;
    const float boxW = ui::Dp(66.0f), boxH = ui::Dp(88.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(boxW, boxH));
    DrawFramingPreview(ImGui::GetWindowDrawList(), p, ImVec2(p.x + boxW, p.y + boxH), sc.shot);
    ImGui::SameLine();
    ImGui::BeginGroup();
    auto combo = [&](const char* id, const char* name, int kind, int& value) {
        const auto& list = CameraChoices(kind);
        value = std::clamp(value, 0, static_cast<int>(list.size()) - 1);
        ImGui::SetNextItemWidth(-FLT_MIN);
        const std::string preview = std::string(name) + ": " + list[value].label;
        if (ImGui::BeginCombo(id, preview.c_str())) {
            for (int i = 0; i < static_cast<int>(list.size()); ++i) {
                if (ImGui::Selectable(list[i].label, i == value)) {
                    value = i;
                    Touch(st);
                }
            }
            ImGui::EndCombo();
        }
    };
    combo("##camshot", "Shot", 0, sc.shot);
    combo("##camangle", "Angle", 1, sc.angle);
    combo("##camlens", "Lens", 2, sc.lens);
    ImGui::EndGroup();
    if (st.format == 2) combo("##cammove", "Camera move", 3, sc.movement);
    combo("##camlight", "Light", 4, sc.lighting);
    combo("##camdof", "Focus", 5, sc.dof);
    if (b.scenes.size() > 1 && ImGui::SmallButton("Same camera for every scene")) {
        for (BoardScene& o : b.scenes) {
            o.shot = sc.shot;
            o.angle = sc.angle;
            o.lens = sc.lens;
            o.movement = sc.movement;
            o.lighting = sc.lighting;
            o.dof = sc.dof;
        }
        Touch(st);
    }
    const std::string text = CameraText(sc, st.format == 2);
    if (!text.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", text.c_str());
        ImGui::PopTextWrapPos();
    }
}

void DrawPromptPanel(AppState& app, BoardViewState& st) {
    Board& b = st.board;
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::TextUnformatted("Scene prompt");
    const char* formats[] = {"Image prompt (paragraph)", "Tags (SD / Illustrious / Pony)", "Video scene (uses the timeline)"};
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::Combo("##boardformat", &st.format, formats, 3);
    if (st.format == 2) {
        ImGui::TextDisabled("%d scene%s over %.0f s: see the timeline under the board", static_cast<int>(b.scenes.size()), b.scenes.size() == 1 ? "" : "s", b.TotalSec());
    } else if (b.scenes.size() > 1) {
        ImGui::TextDisabled("Describes %s", b.SceneName(st.curScene).c_str());
    }
    DrawCameraPanel(st);
    const int styles = VisionStyleCount();
    const char* preview = st.styleIdx == 0 ? "Style: as is" : st.styleIdx <= styles ? VisionStyleAt(st.styleIdx - 1).label : "Style: custom...";
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##boardstyle", preview)) {
        if (ImGui::Selectable("As is", st.styleIdx == 0)) st.styleIdx = 0;
        for (int i = 0; i < styles; ++i) {
            if (ImGui::Selectable(VisionStyleAt(i).label, st.styleIdx == i + 1)) st.styleIdx = i + 1;
        }
        if (ImGui::Selectable("Custom...", st.styleIdx == styles + 1)) st.styleIdx = styles + 1;
        ImGui::EndCombo();
    }
    if (st.styleIdx == styles + 1) InputLine("##boardcustom", "describe the style", st.customStyle, -FLT_MIN);
    {
        BoardScene& sceneNote = b.scenes[std::clamp(st.curScene, 0, static_cast<int>(b.scenes.size()) - 1)]; // the same note as the timeline's "what happens" box
        if (InputLine("##scenenote", "What happens in this scene (the scene note)", sceneNote.text, -FLT_MIN)) Touch(st);
    }
    InputLine("##boardextra", "Extra direction (camera, mood, light...)", st.extra, -FLT_MIN);
    ImGui::Checkbox("The model also looks at the pictures", &st.attach);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Slower, but it can match the real look of every card. Needs a vision model.");

    const BoardPromptOptions opt = Options(st);
    const std::string draft = AssembleBoardPrompt(b, opt); // also tells whether the written prompt is out of date
    auto writeNow = [&]() {
        if (!ModelReady(app)) return;
        VisionService::ComposeRequest rq;
        rq.tag = "board";
        rq.prompt = BuildBoardBrief(b, opt, rq.images);
        rq.numPredict = st.format == 2 ? std::min(3000, 500 + 300 * static_cast<int>(b.scenes.size())) : 700;
        rq.temperature = 0.7;
        st.writing = true;
        st.pendingDraft = draft;
        app.vision.Compose(app.VisionEndpoint(), app.visionModel, app.visionKeepAlive, std::move(rq));
    };
    ImGui::Spacing();
    ImGui::BeginDisabled(st.writing || b.nodes.empty());
    if (ImGui::Button(st.writing ? "Writing..." : "Write the prompt with the vision model", ImVec2(-FLT_MIN, 0))) writeNow();
    ImGui::EndDisabled();
    if (ImGui::Button("Same scene, other characters...", ImVec2(-FLT_MIN, 0))) OpenRecast();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Do this scene again with different characters (\"now with 3 and 4\")");
    if (!app.visionModel.empty()) ImGui::TextDisabled("Model: %s", app.visionModel.c_str());
    else if (ImGui::SmallButton("Pick a vision model...")) app.showVisionWindow = true;
    if (!st.result.empty() || st.writing) {
        ImGui::Spacing();
        ImGui::TextUnformatted("Final prompt");
        if (!st.writing && !st.result.empty() && st.resultDraft != draft) { // cards, relations, inventory or camera changed after it was written
            ImGui::TextColored(ui::kWarn, "The board changed since this was written.");
            ImGui::SameLine();
            if (ImGui::SmallButton("Write it again")) writeNow();
        }
        if (st.editResult) InputBox("##boardresult", st.result, w, ui::Dp(190.0f));
        else WrappedBox("##boardresultview", st.writing && st.result.empty() ? std::string("Writing...") : st.result, ui::Dp(190.0f));
        if (ImGui::Button("Copy prompt")) {
            ImGui::SetClipboardText(st.result.c_str());
            app.SetStatus("Copied the scene prompt");
        }
        ImGui::SameLine();
        ImGui::Checkbox("Edit", &st.editResult);
        ImGui::SameLine();
        if (ImGui::Button("Save as .txt...")) {
            const std::wstring dir = BoardsDir() + L"\\prompts";
            std::error_code ec;
            fs::create_directories(dir, ec);
            std::time_t t = std::time(nullptr);
            std::tm tmv{};
            localtime_s(&tmv, &t);
            wchar_t name[64];
            swprintf_s(name, L"\\scene_%04d%02d%02d_%02d%02d%02d.txt", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
            std::ofstream out(dir + name, std::ios::binary);
            out << st.result;
            app.SetStatus("Saved to " + ToUtf8(dir + name));
        }
    }
    ImGui::Checkbox("Pixel art mode##boardpx", &g_boardPixel);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("The pictures made from this board (the Qwen edit, or the prompt sent to ComfyUI) also become real pixel art, in the Pixel art tab (set \"Vision board\")");
    if (g_boardPixel) PixelArtModeOptions("boardpxopt", g_boardPixelLook);
    if (ImGui::Button("Make the picture from the cards (Qwen Edit)...", ImVec2(-FLT_MIN, 0))) {
        const bool stale = !st.result.empty() && st.resultDraft != draft; // the written prompt no longer matches the board: use the current draft
        if (stale) app.SetStatus("The written prompt was out of date, so the picture uses the board as it is now. Press \"Write the prompt\" again for a polished one.");
        OpenBoardPictureEdit(app, st, st.result.empty() || stale ? std::string() : st.result);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Sends the cards' pictures (setting, characters, items) and the prompt to Qwen Image 2.1 Edit, which can use up to 10 pictures");
    if (!st.result.empty() && ImGui::Button("Edit and send...")) OpenSendEditor(app, st.result, std::string(), std::wstring(), "Vision board", st.format == 2 ? static_cast<double>(b.TotalSec()) : 0.0);
    if (!st.result.empty()) ImGui::SameLine();
    auto sendPrompt = [&](const std::string& text, const char* label) { // pixel art mode: with the look asked for, and watched
        const std::string p = g_boardPixel && g_boardPixelLook ? text + " " + PixelArtLookClause() : text;
        if (SendToComfy(app, p, std::string(), std::wstring(), label, true, st.format == 2 ? static_cast<double>(b.TotalSec()) : 0.0) && g_boardPixel)
            PixelArtAfter(app, LastEditActivity(), "Vision board", "Vision board", true);
    };
    if (!st.result.empty() && ImGui::Button("Send the final prompt to ComfyUI")) sendPrompt(st.result, "Vision board");
    ImGui::Spacing();
    ImGui::TextDisabled("Quick draft (no model needed)");
    std::string draftText = draft;
    WrappedBox("##boarddraft", draftText, ui::Dp(120.0f));
    if (ImGui::Button("Copy draft")) {
        ImGui::SetClipboardText(draft.c_str());
        app.SetStatus("Copied the draft prompt");
    }
    ImGui::SameLine();
    if (ImGui::Button("Send draft to ComfyUI")) sendPrompt(draft, "Vision board draft");
    DrawExportPanel(app, st, opt);

}


// The board's pictures as references for Qwen Image 2.1 Edit: the setting first (when there is one, it is the picture that is edited), then the
// characters, the items and what the characters carry. The instruction asks for ONE scene in one place (same ground, light and scale, interacting) and
// forbids the "two photos side by side / blended" look; without a setting the result gets a new canvas instead of the frame of the first character.
// The text the directions are decided from: where, what the setting card says, what happens, the notes.
std::string DirectorContext(const Board& b, int scene, const std::string& extra, const std::string& place) {
    std::string t = place;
    auto add = [&](const std::string& s) {
        if (!s.empty()) t += (t.empty() ? "" : ". ") + s;
    };
    for (const BoardNode& n : b.nodes) {
        if (n.scene != scene) continue;
        if (n.kind == BoardKind::Background) add(b.DisplayName(n) + " " + n.text);
        if (n.kind == BoardKind::Note) add(n.text);
    }
    if (!b.scenes.empty()) add(b.scenes[std::clamp(scene, 0, static_cast<int>(b.scenes.size()) - 1)].text);
    for (const BoardLink& l : b.links) {
        const BoardNode *f = b.Find(l.from), *to = b.Find(l.to);
        if (f && to && f->scene == scene) add(b.DisplayName(*f) + " " + l.label + " " + b.DisplayName(*to));
    }
    add(extra);
    return t;
}

// Concrete outfit / posture / expression for the board's characters (left to right, the same order as the instruction), as a sentence for the instruction.
std::string LooksFor(const Board& b, int scene, const std::vector<std::wstring>& images, const std::string& extra, const BoardEditOptions& opt) {
    if (opt.styleMode <= 0) return {};
    std::vector<const BoardNode*> chars;
    for (const BoardNode& n : b.nodes) {
        if (n.scene == scene && n.kind == BoardKind::Character) chars.push_back(&n);
    }
    std::sort(chars.begin(), chars.end(), [](const BoardNode* x, const BoardNode* y) { return x->x < y->x; });
    std::vector<DirectorChar> dc;
    for (const BoardNode* n : chars) {
        int tag = 0;
        for (size_t i = 0; i < images.size(); ++i) {
            if (_wcsicmp(images[i].c_str(), n->image.c_str()) == 0) tag = static_cast<int>(i) + 1;
        }
        if (tag == 0) continue;
        DirectorChar c;
        c.name = b.DisplayName(*n);
        c.description = n->text;
        c.tag = "<image" + std::to_string(tag) + ">";
        dc.push_back(c);
    }
    if (dc.empty()) return {};
    std::vector<CharLook> looks;
    if (opt.fixedLooks) { // decided already: by picture
        for (size_t i = 0; i < dc.size(); ++i) {
            CharLook l;
            for (const BoardNode* n : chars) {
                if (b.DisplayName(*n) != dc[i].name) continue;
                for (const auto& f : *opt.fixedLooks) {
                    if (_wcsicmp(f.first.c_str(), n->image.c_str()) == 0) l = f.second;
                }
            }
            looks.push_back(l);
        }
    } else {
        looks = DirectLooks(dc, DirectorContext(b, scene, extra, opt.place), opt.flags, ClefGetSettings().use, nullptr);
    }
    return LooksClause(dc, looks, opt.flags);
}

std::string MakeBoardEdit(const Board& b, int scene, const std::string& extra, std::vector<std::wstring>& images, std::vector<std::string>& names, int& canvas, BoardEditOptions& opt) {
    const int styleMode = opt.styleMode;
    images.clear();
    names.clear();
    canvas = 0;
    std::string roles;
    std::vector<std::pair<int, int>> tagOf; // node id -> picture number
    int settingTag = 0;
    auto add = [&](const std::wstring& image, const std::string& what, const std::string& shortName) -> int {
        if (image.empty() || images.size() >= 10) return 0;
        for (size_t i = 0; i < images.size(); ++i) {
            if (_wcsicmp(images[i].c_str(), image.c_str()) == 0) return static_cast<int>(i) + 1;
        }
        images.push_back(image);
        names.push_back(shortName);
        roles += (roles.empty() ? "" : " ") + std::string("<image") + std::to_string(images.size()) + "> is " + what + ".";
        return static_cast<int>(images.size());
    };
    std::vector<const BoardNode*> chars;
    for (const BoardNode& n : b.nodes) {
        if (n.scene != scene) continue;
        if (n.kind == BoardKind::Background) {
            const int t = add(n.image, "the setting (" + b.DisplayName(n) + ")", b.DisplayName(n));
            if (t > 0 && settingTag == 0) settingTag = t;
            tagOf.push_back({n.id, t});
        } else if (n.kind == BoardKind::Character) {
            chars.push_back(&n);
        }
    }
    std::sort(chars.begin(), chars.end(), [](const BoardNode* x, const BoardNode* y) { return x->x < y->x; });
    for (const BoardNode* n : chars) tagOf.push_back({n->id, add(n->image, "the character " + b.DisplayName(*n) + (styleMode > 0 && (opt.flags.outfit || opt.flags.adaptOutfit) ? " (face, hair and build)" : " (face, hair, build and outfit)"), b.DisplayName(*n))});
    for (const BoardNode& n : b.nodes) {
        if (n.scene == scene && n.kind == BoardKind::Item) tagOf.push_back({n.id, add(n.image, "the " + b.DisplayName(n), b.DisplayName(n))});
    }
    for (const BoardNode* n : chars) {
        if (const CharacterProfile* p = ProfileOf(*n)) {
            for (const InvItem& it : p->items) {
                if (!it.name.empty()) add(it.image, "the " + it.name + " of " + b.DisplayName(*n), it.name);
            }
        }
    }
    if (images.empty()) return {};
    auto phrase = [&](int id) {
        std::string name;
        if (const BoardNode* n = b.Find(id)) name = b.DisplayName(*n);
        for (const auto& t : tagOf) {
            if (t.first == id && t.second > 0) return name + " (<image" + std::to_string(t.second) + ">)";
        }
        return name;
    };
    std::string actions;
    for (const BoardLink& l : b.links) {
        const BoardNode *f = b.Find(l.from), *t = b.Find(l.to);
        if (!f || !t || f->scene != scene) continue;
        actions += (actions.empty() ? "" : " ") + phrase(l.from) + " " + l.label + " " + phrase(l.to) + ".";
    }
    std::string notes;
    if (!b.scenes.empty()) notes = b.scenes[std::clamp(scene, 0, static_cast<int>(b.scenes.size()) - 1)].text;
    for (const BoardNode& n : b.nodes) {
        if (n.scene == scene && n.kind == BoardKind::Note && !n.text.empty()) notes += (notes.empty() ? "" : " ") + n.text;
    }
    std::string out = "Make ONE single new image of ONE shared scene. " + roles + " ";
    out += "Everyone is in the same place at the same time: they stand on the same ground, close together and interacting, with one consistent perspective, lighting "
           "and colour grade and a believable relative scale. ";
    out += settingTag > 0 ? "The place is the setting shown in <image" + std::to_string(settingTag) + ">: keep it. "
                          : (opt.place.empty() ? std::string("Choose one fitting, detailed real-world location (for example a street, a room or a park) and put all the characters in it: ")
                                               : "The place is " + opt.place + ": put all the characters in it, ") +
                            "the plain backdrops behind the characters in the reference pictures are NOT part of the scene and must not appear. ";
    if (!actions.empty()) out += "What happens: " + actions + " ";
    if (!notes.empty()) out += notes + " ";
    if (!b.scenes.empty()) {
        const std::string cam = CameraText(b.scenes[std::clamp(scene, 0, static_cast<int>(b.scenes.size()) - 1)], false);
        if (!cam.empty()) out += "Camera and lighting: " + cam + ". ";
    }
    if (!extra.empty()) out += extra + " ";
    opt.looksOut.clear();
    if (styleMode > 0 && chars.size() >= 1) {
        out += EditStyleClause(styleMode) + " ";
        opt.looksOut = LooksFor(b, scene, images, extra + " " + opt.place, opt);
        if (!opt.looksOut.empty()) out += opt.looksOut + " ";
    }
    out += "The reference pictures only show who each character is: do not copy their poses, framing, clothes or backgrounds, and do not make a collage, a side by side, a split "
           "screen, a double exposure or a blend of the pictures.";
    canvas = settingTag > 0 ? 0 : (chars.size() >= 2 ? 1 : 3); // no setting picture: a new canvas (landscape for a group, portrait for one person)
    return out;
}

void OpenBoardPictureEdit(AppState& app, BoardViewState& st, const std::string& prompt) {
    EditContext ctx;
    std::vector<std::wstring> images;
    std::string extra = st.extra;
    while (!extra.empty() && std::isspace(static_cast<unsigned char>(extra.back()))) extra.pop_back();
    const std::string style = StyleText(st);
    if (!style.empty()) extra += (extra.empty() ? "" : " ") + std::string("Style: ") + style + ".";
    // the instruction is built from the board itself (pictures, relations, camera); the written final prompt, when there is a current one, is added as
    // a scene description that the window can switch off (it describes the characters in words, which the pictures already show)
    BoardEditOptions eo;
    eo.styleMode = app.comfy.editStyle;
    eo.flags = DirectorFlagsFor(app.comfy, eo.styleMode);
    std::string instruction = MakeBoardEdit(st.board, st.curScene, extra, images, ctx.roles, ctx.canvas, eo);
    ctx.looksText = eo.looksOut;
    if (!prompt.empty() && !instruction.empty()) {
        instruction += " " + FinalPromptClause(prompt);
        ctx.finalPrompt = prompt;
    }
    if (images.empty()) {
        app.SetStatus("The board has no pictures yet");
        return;
    }
    ctx.hasBoard = true;
    ctx.board = st.board;
    ctx.scene = st.curScene;
    ctx.prompt = extra;
    if (g_boardPixel) { // pixel art mode: the editor's "Then make it pixel art" is on
        ctx.pixelArt = true;
        ctx.pixelGroup = "Vision board";
        if (g_boardPixelLook) instruction += " " + PixelArtLookClause();
    }
    OpenPictureEditor(app, images, instruction, g_boardPixel ? "Vision board picture (pixel art)" : "Vision board picture", &ctx);
}

void PumpBoardResults(AppState& app, BoardViewState& st) {
    if (app.composeResults.empty()) return;
    std::vector<VisionService::Result> results = std::move(app.composeResults);
    app.composeResults.clear();
    for (VisionService::Result& r : results) {
        const std::string tag = ToUtf8(r.path);
        if (tag != "board" && tag.rfind("node:", 0) != 0) { // not ours (the Characters tab asked): leave it for its owner
            app.composeResults.push_back(std::move(r));
            continue;
        }
        if (tag == "board") {
            st.writing = false;
            if (r.text.empty()) app.SetStatus("The model did not write a prompt: " + r.error);
            else {
                st.result = r.text;
                st.resultDraft = st.pendingDraft; // what the board looked like when it was written
            }
        } else if (tag.rfind("node:", 0) == 0) {
            if (BoardNode* n = st.board.Find(std::atoi(tag.c_str() + 5))) {
                n->describing = false;
                if (!r.text.empty()) {
                    n->text = r.text;
                    Touch(st);
                } else {
                    app.SetStatus("Could not describe the picture: " + r.error);
                }
            }
        }
    }
}

void LoadBoardOnce(BoardViewState& st) {
    if (st.loaded) return;
    st.loaded = true;
    LoadBoard(CurrentPath(), st.board);
    for (BoardNode& n : st.board.nodes) n.describing = false;
    st.fitRequest = true;
    LoadBoardSettings(st);
    if (st.board.scenes.size() > 1) st.format = 2;
    ResetHistory(st);
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------
void DrawBoardTab(AppState& app, float height) {
    g_relComfyUrl = app.comfy.url;
    BoardViewState& st = app.board;
    LoadBoardOnce(st);
    {
        // Ctrl+Z undoes, Ctrl+Y / Ctrl+Shift+Z redoes (not while a text box is being typed in: it has its own undo)
        ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl && !io.WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_Z)) {
                if (io.KeyShift) Redo(app, st);
                else Undo(app, st);
            } else if (ImGui::IsKeyPressed(ImGuiKey_Y)) {
                Redo(app, st);
            }
        }
    }
    PumpBoardResults(app, st);
    // a finished web download adds its picture to the character's inventory
    if (st.urlJob.valid() && st.urlJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const std::pair<std::wstring, std::string> got = st.urlJob.get();
        if (!got.first.empty()) {
            for (CharacterProfile& p : AllProfiles()) {
                if (p.key == st.urlJobKey) {
                    p.AddItem(InvSlot::Holding, st.urlJobName, got.first);
                    TouchProfiles(st);
                }
            }
            app.SetStatus("Added " + st.urlJobName + " to the inventory");
        } else {
            app.SetStatus(got.second.empty() ? "The download failed" : got.second);
        }
    }
    // a request the worker dropped (Cancel) never answers: do not stay stuck on "Describing..." / "Writing..."
    bool waiting = st.writing;
    for (const BoardNode& n : st.board.nodes) waiting = waiting || n.describing;
    if (waiting && !app.vision.GetSnapshot().busy && app.composeResults.empty()) {
        if (st.idleSince == 0) st.idleSince = ImGui::GetTime();
        if (ImGui::GetTime() - st.idleSince > 4.0) {
            st.writing = false;
            for (BoardNode& n : st.board.nodes) n.describing = false;
            st.idleSince = 0;
        }
    } else {
        st.idleSince = 0;
    }

    if (ImGui::BeginTable("board_layout", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings, ImVec2(0, height))) {
        ImGui::TableSetupColumn("Library", ImGuiTableColumnFlags_WidthStretch, 0.18f);
        ImGui::TableSetupColumn("Board", ImGuiTableColumnFlags_WidthStretch, 0.52f);
        ImGui::TableSetupColumn("Prompt", ImGuiTableColumnFlags_WidthStretch, 0.30f);
        ImGui::TableNextRow(0, height);

        ImGui::TableSetColumnIndex(0);
        DrawLibraryPanel(app, st, height);

        ImGui::TableSetColumnIndex(1);
        {
            ImGui::BeginChild("board_center", ImVec2(0, height), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            bool firstButton = true;
            auto place = [&](const char* label) {
                const float bw = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2;
                if (!firstButton && ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + bw <= ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x) ImGui::SameLine();
                firstButton = false;
                return ImGui::Button(label);
            };
            if (place("New")) {
                KeepCurrent(st); // the board you leave is always kept: Back (or Boards...) brings it back
                if (!st.prevBoardName.empty()) app.SetStatus("Kept the previous board as \"" + st.prevBoardName + "\": press Back to reopen it");
                st.board = Board();
                st.name.clear();
                st.curScene = 0;
                st.selNode = st.selLink = -1;
                st.result.clear();
                st.fitRequest = true;
                Touch(st);
                ResetHistory(st);
            }
            if (!st.prevBoardPath.empty()) {
                const std::string backLabel = "Back to " + (st.prevBoardName.size() > 22 ? st.prevBoardName.substr(0, 22) + "..." : st.prevBoardName);
                if (place(backLabel.c_str())) OpenBoardFile(app, st, st.prevBoardPath);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reopen the board you had before (the current one is kept too)");
            }
            ImGui::BeginDisabled(st.undoStack.empty() && !st.histDirty);
            if (place("Undo")) Undo(app, st);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Undo the last change (Ctrl+Z): a deleted card comes back with its inventory");
            ImGui::BeginDisabled(st.redoStack.empty());
            if (place("Redo")) Redo(app, st);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Redo (Ctrl+Y)");
            if (place(st.format == 2 ? "Video: on" : "Video: off")) st.format = st.format == 2 ? 0 : 2;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Video mode shows the scenes and the timeline and writes the prompt as a time-coded video script");
            if (place("Boards...")) {
                st.showBrowser = true;
                st.browserDirty = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Browse the saved boards, and the boards each character is on");
            if (place("Save as...")) {
                st.saveName = st.name;
                ImGui::OpenPopup("##boardsave");
            }
            if (place("+ Note")) {
                st.selNode = st.board.Add(BoardKind::Note, std::wstring(), std::string(), st.centerX - 100, st.centerY - 60, st.curScene);
                st.selLink = -1;
                Touch(st);
            }
            if (place("Paste image (Ctrl+V)")) {
                std::string msg;
                const std::wstring p = PasteImage(msg);
                if (!p.empty()) {
                    AddPicture(app, st, p, static_cast<BoardKind>(std::clamp(st.addKind, 0, 2)), st.centerX - 115, st.centerY - 120);
                } else {
                    app.SetStatus(msg);
                }
            }
            if (place("Add file...")) {
                const std::wstring p = OpenImageFileDialog(GetActiveWindow());
                if (!p.empty()) {
                    AddPicture(app, st, p, static_cast<BoardKind>(std::clamp(st.addKind, 0, 2)), st.centerX - 115, st.centerY - 120);
                }
            }
            const int selCount = static_cast<int>(app.multiSelected.size());
            if (selCount > 0) {
                const std::string label = selCount == 1 ? "Add the selected picture" : "Add " + std::to_string(selCount) + " gallery pictures";
                if (place(label.c_str())) {
                    float ox = 0;
                    for (int i : app.multiSelected) {
                        if (i >= 0 && i < static_cast<int>(app.entries.size())) {
                            AddPicture(app, st, app.entries[i].path, static_cast<BoardKind>(std::clamp(st.addKind, 0, 2)), st.centerX - 115 + ox, st.centerY - 120);
                            ox += 250;
                        }
                    }
                }
            }
            if (place("Fit")) st.fitRequest = true;
            { // the board's name (and counts when there is room): on the button line when it fits, else on its own line
                const std::string boardName = st.name.empty() ? "scratch board (saved automatically)" : st.name;
                const std::string full = boardName + "  -  " + std::to_string(st.board.nodes.size()) + " cards, " + std::to_string(st.board.links.size()) + " relations";
                const float room = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - ImGui::GetItemRectMax().x - ImGui::GetStyle().ItemSpacing.x;
                const std::string& shown = ImGui::CalcTextSize(full.c_str()).x <= room ? full : boardName;
                if (ImGui::CalcTextSize(shown.c_str()).x <= room) ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", shown.c_str());
            }

            if (ImGui::BeginPopup("##boardsave")) {
                ImGui::TextUnformatted("Name this board");
                if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
                const bool enter = ImGui::InputTextWithHint("##boardsavename", "e.g. Knight vs Dragon", st.saveName.data(), st.saveName.capacity() + 1,
                                                            ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_EnterReturnsTrue, StdStringResize, &st.saveName);
                if ((ImGui::Button("Save") || enter) && !st.saveName.empty()) {
                    if (SaveBoard(st.board, NamedPath(st.saveName))) {
                        st.name = st.saveName;
                        st.browserDirty = true;
                        app.SetStatus("Saved the board \"" + st.name + "\"");
                    } else {
                        app.SetStatus("Could not save the board");
                    }
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            // Ctrl+V anywhere on this tab (when no text box is being edited)
            if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false) && !ImGui::GetIO().WantTextInput) {
                std::string msg;
                const std::wstring p = PasteImage(msg);
                if (!p.empty()) {
                    AddPicture(app, st, p, static_cast<BoardKind>(std::clamp(st.addKind, 0, 2)), st.centerX - 115, st.centerY - 120);
                } else {
                    app.SetStatus(msg);
                }
            }

            const bool video = st.format == 2;
            if (video || st.board.scenes.size() > 1) DrawSceneTabs(st); // beats only matter for a video (or a board that already has several)
            const float timelineH = !video ? 0.0f : st.showTimeline ? ui::Dp(285.0f) : ui::Dp(36.0f);
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            DrawCanvas(app, st, ImVec2(std::max(50.0f, avail.x), std::max(50.0f, avail.y - (timelineH > 0 ? timelineH + ImGui::GetStyle().ItemSpacing.y : 0.0f))));
            if (video) DrawTimeline(app, st, timelineH);
            ImGui::EndChild();
        }

        ImGui::TableSetColumnIndex(2);
        {
            ImGui::BeginChild("board_side", ImVec2(0, height), true);
            DrawInspector(app, st);
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            DrawPromptPanel(app, st);
            ImGui::EndChild();
        }
        ImGui::EndTable();
    }

    CommitHistory(st);
    DrawBrowser(app, st);
    DrawAirPanel(app, st);
    if (st.profilesDirty && ImGui::GetTime() - st.profilesDirtyAt > 1.0 && !ImGui::IsAnyItemActive()) {
        SaveProfiles();
        st.profilesDirty = false;
    }
    // autosave a moment after the last change
    if (st.dirty && ImGui::GetTime() - st.dirtyAt > 1.0 && !ImGui::IsMouseDown(0)) {
        SaveBoard(st.board, CurrentPath());
        if (!st.name.empty()) SaveBoard(st.board, NamedPath(st.name));
        st.dirty = false;
    }
}

// Right click a picture in the gallery / Vision library > Add to Vision board.
void AddImagesToBoard(AppState& app, const std::vector<std::wstring>& paths, int kind) {
    BoardViewState& st = app.board;
    LoadBoardOnce(st);
    const BoardKind k = static_cast<BoardKind>(std::clamp(kind, 0, 2));
    float offset = 0;
    for (const std::wstring& p : paths) {
        AddPicture(app, st, p, k, st.centerX - 115 + offset, st.centerY - 120 + offset);
        offset += 40;
    }
    app.requestTab = 3;
    app.SetStatus(paths.size() == 1 ? "Added the picture to the Vision board" : "Added " + std::to_string(paths.size()) + " pictures to the Vision board");
}

std::wstring PasteBoardImage(std::string& message) { return PasteImage(message); }

BoardPromptOptions BoardCurrentOptions(const BoardViewState& st) { return Options(st); }

std::string BoardEditRequest(const Board& board, int scene, const std::string& extra, std::vector<std::wstring>& images, std::vector<std::string>& roles, int& canvas, BoardEditOptions& options) {
    return MakeBoardEdit(board, scene, extra, images, roles, canvas, options);
}

std::string BoardLooksClause(const Board& board, int scene, const std::vector<std::wstring>& images, const std::string& extra, const BoardEditOptions& options) {
    return LooksFor(board, scene, images, extra, options);
}

// Clef mode: the scenario as a board (characters left to right, linked by what they do), so the same instruction builder, the Edits tab and its swaps work on it.
Board BoardFromScenario(const std::vector<CastMember>& cast, const Scenario& sc, bool placeCard) {
    Board b;
    std::vector<int> ids;
    float x = 0;
    for (int idx : sc.cast) {
        if (idx < 0 || idx >= static_cast<int>(cast.size())) continue;
        const int id = b.Add(BoardKind::Character, cast[idx].image, cast[idx].description, x, 0, 0);
        if (BoardNode* n = b.Find(id)) n->title = cast[idx].name;
        ids.push_back(id);
        x += 330;
    }
    for (size_t i = 0; i + 1 < ids.size() && i < sc.relations.size(); ++i) b.Connect(ids[i], ids[i + 1], sc.relations[i]);
    if (placeCard && !ids.empty()) {
        const int bg = b.Add(BoardKind::Background, std::wstring(), sc.PlaceText(), 330.0f * 0.5f * (ids.size() - 1), -300.0f, 0);
        if (BoardNode* n = b.Find(bg)) n->title = "Setting";
        for (int id : ids) b.Connect(id, bg, "stands in");
    }
    if (!b.scenes.empty()) {
        BoardScene& s = b.scenes[0];
        s.shot = sc.shot;
        s.angle = sc.angle;
        s.lighting = sc.lighting;
        s.text = sc.mood + ". " + sc.time + ". " + sc.twist + ".";
        if (!sc.eraPhrase.empty()) s.text += " The world and era: " + sc.era + " (" + sc.eraPhrase + "): everything, clothes and surroundings, belongs to it.";
        if (!sc.genre.empty()) s.text += " Genre: " + sc.genre + ".";
        if (!sc.prop.empty()) s.text += " " + sc.prop.substr(0, 1) + sc.prop.substr(1) + " plays a part in the scene.";
        if (!sc.beat.empty()) s.text += " The moment: " + sc.beat + ".";
        if (!sc.photoStyle.empty()) s.text += " Photographed as " + sc.photoStyle + ".";
    }
    return b;
}
