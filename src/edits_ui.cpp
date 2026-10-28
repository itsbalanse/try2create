// The Edits tab: every picture edit sent to Qwen Image 2.1 Edit, with its result, what it was made from and the Vision board it came from.
// Characters (or any picture) can be swapped on the fly: the edit is sent again with the other picture in its place.
#include <algorithm>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <windows.h>
#include <shellapi.h>

#include "board.h"
#include "clipboard_image.h"
#include "crash_log.h"
#include "image_texture.h"
#include "imgui.h"
#include "json.hpp"
#include "ui_extra.h"
#include "ui_theme.h"
#include "win_dialogs.h"

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

int StringResize(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* str = static_cast<std::string*>(data->UserData);
        str->resize(data->BufTextLen);
        data->Buf = str->data();
    }
    return 0;
}

struct EditRecord {
    int id = 0;
    int activity = 0; // while ComfyUI is still making it
    std::string label, instruction, prompt, error, state, message;
    std::vector<std::wstring> images;
    std::vector<std::string> roles;
    bool hasBoard = false;
    Board board; // the Vision board as it was when the edit was sent
    int scene = 0;
    int canvas = 0;
    int steps = 0, res = 0, times = 1;
    std::vector<std::wstring> results;
    long long when = 0;
    int resultIdx = 0;
};

int g_editsMode = 6; // 0 Qwen picture edits, 1 MOSS designs and box edits, 2 text over a picture (PhotoCraft), 3 upscale, 4 apps (one-click recipes),
                     // 5 videos (Make it crisp, quick upscale), 6 the start screen (a card for each)
std::deque<EditRecord> g_edits; // oldest first (a deque: adding an edit while the tab holds a pointer to another one must not move it)
bool g_loaded = false;
int g_nextId = 1;
int g_sel = -1; // record id

fs::path EditsDir() { return fs::path(BoardsDir()).parent_path() / L"edits"; }

void SaveEdits() {
    json j;
    j["next"] = g_nextId;
    j["edits"] = json::array();
    for (const EditRecord& r : g_edits) {
        json e = {{"id", r.id}, {"label", r.label}, {"instruction", r.instruction}, {"prompt", r.prompt}, {"error", r.error}, {"roles", r.roles},
                  {"hasBoard", r.hasBoard}, {"scene", r.scene}, {"canvas", r.canvas}, {"when", r.when}, {"images", json::array()}, {"results", json::array()}};
        for (const std::wstring& p : r.images) e["images"].push_back(ToUtf8(p));
        for (const std::wstring& p : r.results) e["results"].push_back(ToUtf8(p));
        j["edits"].push_back(std::move(e));
    }
    std::error_code ec;
    fs::create_directories(EditsDir(), ec);
    std::ofstream out(EditsDir() / L"edits.json", std::ios::binary | std::ios::trunc);
    if (out) out << j.dump(2, ' ', false, json::error_handler_t::replace);
}

void LoadEdits() {
    if (g_loaded) return;
    g_loaded = true;
    std::ifstream f(EditsDir() / L"edits.json", std::ios::binary);
    if (!f) return;
    json j = json::parse(f, nullptr, false);
    if (!j.is_object()) return;
    g_nextId = j.value("next", 1);
    if (!j.contains("edits") || !j["edits"].is_array()) return;
    for (const json& e : j["edits"]) {
        EditRecord r;
        r.id = e.value("id", 0);
        r.label = e.value("label", std::string());
        r.instruction = e.value("instruction", std::string());
        r.prompt = e.value("prompt", std::string());
        r.error = e.value("error", std::string());
        r.scene = e.value("scene", 0);
        r.canvas = e.value("canvas", 0);
        r.when = e.value("when", 0LL);
        r.hasBoard = e.value("hasBoard", false);
        if (e.contains("roles") && e["roles"].is_array()) {
            for (const json& x : e["roles"]) r.roles.push_back(x.is_string() ? x.get<std::string>() : std::string());
        }
        if (e.contains("images") && e["images"].is_array()) {
            for (const json& x : e["images"]) r.images.push_back(ToWide(x.is_string() ? x.get<std::string>() : std::string()));
        }
        if (e.contains("results") && e["results"].is_array()) {
            for (const json& x : e["results"]) r.results.push_back(ToWide(x.is_string() ? x.get<std::string>() : std::string()));
        }
        if (r.hasBoard) {
            const std::wstring path = (EditsDir() / (L"board_" + std::to_wstring(r.id) + L".json")).wstring();
            if (!LoadBoard(path, r.board)) r.hasBoard = false;
        }
        g_nextId = std::max(g_nextId, r.id + 1);
        if (r.id > 0) g_edits.push_back(std::move(r));
    }
    // an edit that was still being made when the program closed never finished
    for (EditRecord& r : g_edits) {
        if (r.results.empty() && r.error.empty()) r.error = "It did not finish (the program was closed)";
    }
    if (!g_edits.empty()) g_sel = g_edits.back().id;
}

EditRecord* Find(int id) {
    for (EditRecord& r : g_edits) {
        if (r.id == id) return &r;
    }
    return nullptr;
}

// the progress of the edits that are still being made, from ComfyUI's activity list
void Sync(AppState& app) {
    bool any = false;
    for (const EditRecord& r : g_edits) any = any || r.activity > 0;
    if (!any || !app.comfySvc) return;
    const std::vector<ComfyActivity> acts = app.comfySvc->Activity();
    bool changed = false;
    for (EditRecord& r : g_edits) {
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
            case ComfyState::Running: r.state = "Generating"; break;
            default: r.state.clear(); break;
        }
        if (a->state == ComfyState::Done || a->state == ComfyState::Failed) CrashNote(std::string("edit: ComfyUI finished an edit (") + (a->state == ComfyState::Done ? "done" : "failed") + ")");
        if (a->state == ComfyState::Done && a->runningMs > 0 && a->finishedMs > a->runningMs && r.steps > 0) {
            RecordEditRun(r.steps, r.res, static_cast<int>(r.images.size()), r.canvas, r.times, (a->finishedMs - a->runningMs) / 1000.0); // teaches the time estimate
        }
        if (a->state == ComfyState::Done) {
            for (const ComfyResult& f : a->files) {
                if (!f.localPath.empty()) r.results.push_back(f.localPath);
            }
            if (r.results.empty()) r.error = "ComfyUI made no picture";
            r.activity = 0;
            changed = true;
        } else if (a->state == ComfyState::Failed) {
            r.error = a->message;
            r.activity = 0;
            changed = true;
        }
    }
    if (changed) SaveEdits();
}

void Thumb(AppState& app, const std::wstring& path, ImVec2 size, bool crop = true) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const AppState::LibThumb t = app.LibraryThumb(path);
    if (t.srv && t.w > 0 && t.h > 0) {
        ImVec2 uv0(0, 0), uv1(1, 1), a = p, b(p.x + size.x, p.y + size.y);
        if (crop) {
            if (t.w > t.h) { const float m = (1.0f - float(t.h) / t.w) * 0.5f; uv0.x = m; uv1.x = 1 - m; }
            else if (t.h > t.w) { const float m = (1.0f - float(t.w) / t.h) * 0.5f; uv0.y = m; uv1.y = 1 - m; }
        } else {
            const float s = std::min(size.x / t.w, size.y / t.h);
            a = ImVec2(p.x + (size.x - t.w * s) * 0.5f, p.y + (size.y - t.h * s) * 0.5f);
            b = ImVec2(a.x + t.w * s, a.y + t.h * s);
        }
        dl->AddImageRounded((ImTextureID)(intptr_t)t.srv, a, b, uv0, uv1, IM_COL32_WHITE, ui::Dp(5.0f));
    } else {
        dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), ui::ThumbPlaceholder(), ui::Dp(5.0f));
    }
}

std::string NameOf(const std::wstring& image) {
    if (const CharacterProfile* p = FindProfile(image)) {
        if (!p->name.empty()) return p->name;
    }
    return ToUtf8(fs::path(image).stem().wstring());
}

// The edit again with `newImage` in place of `oldImage`: on the Vision board's cast when the edit came from a board, otherwise just that picture.
void SwapIn(AppState& app, const EditRecord& rec, const std::wstring& oldImage, const std::wstring& newImage) {
    if (newImage.empty() || _wcsicmp(oldImage.c_str(), newImage.c_str()) == 0) return;
    EditContext ctx;
    std::vector<std::wstring> images;
    std::string instruction;
    if (rec.hasBoard) {
        bool isCharacter = false;
        for (const BoardNode& n : rec.board.nodes) isCharacter = isCharacter || (n.kind == BoardKind::Character && _wcsicmp(n.image.c_str(), oldImage.c_str()) == 0);
        if (isCharacter) {
            ctx.board = RecastBoard(rec.board, {{oldImage, newImage}});
        } else { // an item or the setting: just its picture
            ctx.board = rec.board;
            for (BoardNode& n : ctx.board.nodes) {
                if (n.kind != BoardKind::Character && _wcsicmp(n.image.c_str(), oldImage.c_str()) == 0) {
                    n.image = newImage;
                    n.title.clear();
                    n.text = ToUtf8(fs::path(newImage).stem().wstring());
                }
            }
        }
        ctx.hasBoard = true;
        ctx.scene = rec.scene;
        ctx.prompt = rec.prompt; // the extra direction given with the first one
        ctx.canvas = rec.canvas;
        int canvas = rec.canvas;
        BoardEditOptions eo;
        eo.styleMode = app.comfy.editStyle;
        eo.flags = DirectorFlagsFor(app.comfy, eo.styleMode);
        instruction = BoardEditRequest(ctx.board, ctx.scene, ctx.prompt, images, ctx.roles, canvas, eo);
        ctx.looksText = eo.looksOut;
        ctx.canvas = canvas;
    } else {
        images = rec.images;
        ctx.roles = rec.roles;
        ctx.canvas = rec.canvas;
        instruction = rec.instruction;
        for (size_t i = 0; i < images.size(); ++i) {
            if (_wcsicmp(images[i].c_str(), oldImage.c_str()) != 0) continue;
            images[i] = newImage;
            if (i < ctx.roles.size() && !ctx.roles[i].empty()) {
                const std::string from = ctx.roles[i], to = NameOf(newImage);
                const size_t at = instruction.find(from);
                if (at != std::string::npos) instruction.replace(at, from.size(), to);
                ctx.roles[i] = to;
            }
        }
    }
    if (images.empty() || instruction.empty()) {
        app.SetStatus("Nothing to send");
        return;
    }
    SendPictureEdit(app, images, instruction, rec.label + " (swapped to " + NameOf(newImage) + ")", 1, &ctx);
}

// The Vision board as it was, drawn small: click a card to swap its picture. Returns the picture clicked ("" = none).
std::wstring DrawMiniBoard(AppState& app, const EditRecord& rec, ImVec2 size) {
    ImGui::BeginChild("##minibooard", size, true, ImGuiWindowFlags_NoScrollbar);
    std::wstring clicked;
    std::vector<const BoardNode*> nodes;
    for (const BoardNode& n : rec.board.nodes) {
        if (n.scene == rec.scene) nodes.push_back(&n);
    }
    if (nodes.empty()) {
        ImGui::TextDisabled("This board has no cards in this scene.");
        ImGui::EndChild();
        return clicked;
    }
    auto cardH = [](const BoardNode& n) { return n.w * 1.15f + 60.0f; };
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (const BoardNode* n : nodes) {
        x0 = std::min(x0, n->x);
        y0 = std::min(y0, n->y);
        x1 = std::max(x1, n->x + n->w);
        y1 = std::max(y1, n->y + cardH(*n));
    }
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float pad = ui::Dp(8.0f);
    const float s = std::min((avail.x - 2 * pad) / std::max(1.0f, x1 - x0), (avail.y - 2 * pad) / std::max(1.0f, y1 - y0));
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##minibg", avail);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto sx = [&](float x) { return origin.x + pad + (x - x0) * s; };
    auto sy = [&](float y) { return origin.y + pad + (y - y0) * s; };
    auto find = [&](int id) -> const BoardNode* {
        for (const BoardNode* n : nodes) {
            if (n->id == id) return n;
        }
        return nullptr;
    };
    for (const BoardLink& l : rec.board.links) { // who does what to whom
        const BoardNode *f = find(l.from), *t = find(l.to);
        if (!f || !t) continue;
        const ImVec2 a(sx(f->x + f->w * 0.5f), sy(f->y + cardH(*f) * 0.5f)), c(sx(t->x + t->w * 0.5f), sy(t->y + cardH(*t) * 0.5f));
        dl->AddLine(a, c, IM_COL32(110, 226, 255, 190), 2.0f);
        const ImVec2 mid((a.x + c.x) * 0.5f, (a.y + c.y) * 0.5f);
        const ImVec2 ts = ImGui::CalcTextSize(l.label.c_str());
        dl->AddRectFilled(ImVec2(mid.x - ts.x * 0.5f - 5, mid.y - ts.y * 0.5f - 2), ImVec2(mid.x + ts.x * 0.5f + 5, mid.y + ts.y * 0.5f + 2), IM_COL32(30, 40, 52, 235), 8.0f);
        dl->AddText(ImVec2(mid.x - ts.x * 0.5f, mid.y - ts.y * 0.5f), IM_COL32(200, 235, 255, 255), l.label.c_str());
    }
    const ImGuiIO& io = ImGui::GetIO();
    for (const BoardNode* n : nodes) {
        const ImVec2 a(sx(n->x), sy(n->y)), c(sx(n->x + n->w), sy(n->y + cardH(*n)));
        const bool hover = ImGui::IsWindowHovered() && io.MousePos.x >= a.x && io.MousePos.x <= c.x && io.MousePos.y >= a.y && io.MousePos.y <= c.y;
        const uint32_t head = n->kind == BoardKind::Character ? 0xB0705A : n->kind == BoardKind::Background ? 0x6AA876 : n->kind == BoardKind::Item ? 0x4F96D6 : 0x888888;
        dl->AddRectFilled(a, c, IM_COL32(44, 44, 50, 255), 6.0f);
        dl->AddRectFilled(a, ImVec2(c.x, a.y + std::min(ui::Dp(16.0f), (c.y - a.y) * 0.2f)), IM_COL32(head & 0xFF, (head >> 8) & 0xFF, (head >> 16) & 0xFF, 255), 6.0f, ImDrawFlags_RoundCornersTop);
        const float hh = std::min(ui::Dp(16.0f), (c.y - a.y) * 0.2f);
        const ImVec2 ia(a.x + 3, a.y + hh + 3), ib(c.x - 3, c.y - 3);
        if (!n->image.empty()) {
            const AppState::LibThumb t = app.LibraryThumb(n->image);
            if (t.srv && t.w > 0 && t.h > 0) {
                const float k = std::min((ib.x - ia.x) / t.w, (ib.y - ia.y) / t.h);
                const ImVec2 ma(ia.x + ((ib.x - ia.x) - t.w * k) * 0.5f, ia.y + ((ib.y - ia.y) - t.h * k) * 0.5f);
                dl->AddImage((ImTextureID)(intptr_t)t.srv, ma, ImVec2(ma.x + t.w * k, ma.y + t.h * k));
            }
        }
        const std::string name = rec.board.DisplayName(*n);
        dl->PushClipRect(a, ImVec2(c.x, a.y + hh), true);
        dl->AddText(ImVec2(a.x + 4, a.y + 1), IM_COL32(255, 255, 255, 240), name.c_str());
        dl->PopClipRect();
        if (hover && n->kind != BoardKind::Note) {
            dl->AddRect(a, c, IM_COL32(110, 226, 255, 255), 6.0f, 0, 3.0f);
            ImGui::SetTooltip("%s\nClick to swap it for another picture", name.c_str());
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !n->image.empty()) clicked = n->image;
        }
    }
    ImGui::EndChild();
    return clicked;
}

void DrawResult(AppState& app, EditRecord& rec, float height) {
    static LoadedImage img;
    static std::wstring imgPath;
    static float zoomMul = 1.0f;
    static ImVec2 pan(0, 0);
    ImGuiIO& io = ImGui::GetIO();
    rec.resultIdx = std::clamp(rec.resultIdx, 0, std::max(0, static_cast<int>(rec.results.size()) - 1));
    const std::wstring path = rec.results.empty() ? std::wstring() : rec.results[rec.resultIdx];
    if (path != imgPath) {
        ReleaseImage(img);
        img = LoadedImage{};
        imgPath = path;
        zoomMul = 1.0f;
        pan = ImVec2(0, 0);
        std::string err;
        if (!path.empty()) LoadImageTexture(app.Device(), path, img, &err);
    }
    if (!rec.results.empty()) {
        ImGui::BeginDisabled(rec.resultIdx <= 0);
        if (ImGui::SmallButton("< Older")) --rec.resultIdx;
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(rec.resultIdx >= static_cast<int>(rec.results.size()) - 1);
        if (ImGui::SmallButton("Newer >")) ++rec.resultIdx;
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Text("%d / %d", rec.resultIdx + 1, static_cast<int>(rec.results.size()));
        ImGui::SameLine();
        if (ImGui::SmallButton("Open in the gallery")) {
            app.OpenFile(path);
            app.requestTab = 0;
        }
        ui::SameLineIfFits("Show in Explorer");
        if (ImGui::SmallButton("Show in Explorer")) {
            const std::wstring args = L"/select,\"" + path + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        }
        ui::SameLineIfFits("Copy image");
        if (ImGui::SmallButton("Copy image")) {
            std::string msg;
            CopyImagesToClipboard(GetActiveWindow(), {path}, false, &msg);
            app.SetStatus(msg);
        }
        ui::SameLineIfFits("Edit this result again...");
        if (ImGui::SmallButton("Edit this result again...")) OpenPictureEditor(app, {path}, std::string(), rec.label + " (refined)");
        ui::SameLineIfFits("Upscale...");
        if (ImGui::SmallButton("Upscale...")) OpenUpscaler(app, {path});
        ui::SameLineIfFits("Pixel art...");
        if (ImGui::SmallButton("Design...")) OpenDesigner(app, {path});
        ImGui::SameLine();
        PixelArtButton(app, {path}, "##pxquickedit");
    }
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 region(std::max(40.0f, avail.x), std::max(ui::Dp(120.0f), height - ImGui::GetCursorPosY() - ui::Dp(8.0f)));
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##editimg", region);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, ImVec2(origin.x + region.x, origin.y + region.y), IM_COL32(10, 12, 16, 255), ui::Dp(6.0f));
    if (img.srv && img.width > 0 && img.height > 0) {
        const float fit = std::min(region.x / img.width, region.y / img.height);
        if (hovered && io.MouseWheel != 0.0f) {
            const float k = std::clamp(std::pow(1.18f, io.MouseWheel) * zoomMul, 0.2f, 20.0f) / zoomMul;
            const ImVec2 c(origin.x + region.x * 0.5f, origin.y + region.y * 0.5f);
            pan.x = (io.MousePos.x - c.x) - (io.MousePos.x - c.x - pan.x) * k;
            pan.y = (io.MousePos.y - c.y) - (io.MousePos.y - c.y - pan.y) * k;
            zoomMul *= k;
        }
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
            pan.x += io.MouseDelta.x;
            pan.y += io.MouseDelta.y;
        }
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            zoomMul = 1.0f;
            pan = ImVec2(0, 0);
        }
        const float scale = fit * zoomMul;
        const ImVec2 dim(img.width * scale, img.height * scale);
        const ImVec2 c(origin.x + region.x * 0.5f + pan.x, origin.y + region.y * 0.5f + pan.y);
        dl->PushClipRect(origin, ImVec2(origin.x + region.x, origin.y + region.y), true);
        dl->AddImage((ImTextureID)(intptr_t)img.srv, ImVec2(c.x - dim.x * 0.5f, c.y - dim.y * 0.5f), ImVec2(c.x + dim.x * 0.5f, c.y + dim.y * 0.5f));
        dl->PopClipRect();
    } else {
        std::string text = rec.activity > 0 ? (rec.state.empty() ? std::string("Generating...") : rec.state + ": " + rec.message) : !rec.error.empty() ? "Failed: " + rec.error : "No picture";
        const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
        dl->AddText(ImVec2(origin.x + std::max(8.0f, (region.x - ts.x) * 0.5f), origin.y + (region.y - ts.y) * 0.5f), rec.error.empty() ? IM_COL32(220, 220, 230, 230) : IM_COL32(255, 120, 110, 255), text.c_str());
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------
int RegisterEdit(AppState& app, int activityId, const std::string& label, const std::string& instruction, const std::vector<std::wstring>& images, const EditContext* ctx, int times) {
    LoadEdits();
    EditRecord r;
    r.id = g_nextId++;
    r.activity = activityId;
    r.label = label;
    r.instruction = instruction;
    r.images = images;
    r.when = static_cast<long long>(std::time(nullptr));
    r.state = "Waiting";
    r.steps = app.comfy.editSteps;
    r.res = app.comfy.editResolution;
    r.times = std::max(1, times);
    if (ctx) {
        r.roles = ctx->roles;
        r.hasBoard = ctx->hasBoard;
        r.scene = ctx->scene;
        r.canvas = ctx->canvas;
        r.prompt = ctx->prompt;
        if (ctx->hasBoard) r.board = ctx->board;
    }
    while (r.roles.size() < r.images.size()) r.roles.push_back(ToUtf8(fs::path(r.images[r.roles.size()]).stem().wstring()));
    if (r.hasBoard) {
        std::error_code ec;
        fs::create_directories(EditsDir(), ec);
        SaveBoard(r.board, (EditsDir() / (L"board_" + std::to_wstring(r.id) + L".json")).wstring());
    }
    g_sel = r.id;
    g_edits.push_back(std::move(r));
    SaveEdits();
    return g_edits.back().id;
}

std::string EditsTabLabel(AppState& app) {
    LoadEdits();
    Sync(app);
    int n = 0;
    for (const EditRecord& r : g_edits) n += r.activity > 0 ? 1 : 0;
    return (n > 0 ? "Edits (" + std::to_string(n) + ")" : std::string("Edits")) + "###editstab";
}

void SetEditsMode(int mode) {
    g_editsMode = std::clamp(mode, 0, 6); // (was clamped to 0-3: "Apps (one click)" - mode 4 - turned into Upscale, so its button did nothing)
    ShowLayoutStudio(g_editsMode == 1);
}
int EditsMode() { return g_editsMode; }

int g_homeAnimFrame = -10; // the last frame a start-screen card was hovered (its drawing plays)
void NoteEditsHomeAnimating() { g_homeAnimFrame = ImGui::GetFrameCount(); }
bool EditsHomeAnimating() { return ImGui::GetFrameCount() - g_homeAnimFrame < 3; }

// The start screen (2026-10-28: the modes needed a visual way in, with examples): a card for
// each kind of edit - a drawing of it (preset_art.cpp), what it is for, and examples - that opens it
void DrawEditsHome(AppState& app, float height) {
    (void)app;
    struct Card {
        int mode;
        const char* title;
        const char* what;
        const char* examples[4];
    };
    static const Card kCards[] = {
        {4, "One-click apps", "Pick a recipe and a picture: nothing to write.", {"Relight it", "A new camera angle", "Turn it into a plushie", "A product shot"}},
        {0, "Change anything", "Say what to change; the picture is redrawn around it.", {"\"make it night\"", "\"put her in a red dress\"", "\"remove the car\"", "\"in the style of anime\""}},
        {1, "Designs & exact text", "Posters, covers and thumbnails with the exact words; or redo just what is inside a box.", {"A movie poster", "A YouTube thumbnail", "Recolour the jacket", "Swap the sign's words"}},
        {2, "Text over a picture", "Real text layers over your picture (a layered .psd).", {"A meme", "Captions", "Speech bubbles", "A title card"}},
        {3, "Upscale a picture", "Bigger, with new fine detail and better faces.", {"2x sharper", "Fix small faces", "More skin and hair detail", "Clean up an old render"}},
        {5, "Videos", "Make a video crisp: MiniMax H3 redraws its detail at 1080p or 2K.", {"768p to 2K", "Sharper faces and hair", "Sound kept", "Quick upscale to 4K"}},
    };
    ImGui::BeginChild("##editshome", ImVec2(0, height), ImGuiChildFlags_None);
    ImGui::Dummy(ImVec2(0, ui::Dp(6.0f)));
    ImGui::PushFont(ui::TitleFont());
    ImGui::TextUnformatted("What do you want to do?");
    ImGui::PopFont();
    ImGui::TextDisabled("Pick one; the buttons above switch between them any time.");
    ImGui::Dummy(ImVec2(0, ui::Dp(6.0f)));
    const float avail = ImGui::GetContentRegionAvail().x, gap = ui::Dp(12.0f);
    const int cols = std::clamp(static_cast<int>((avail + gap) / (ui::Dp(300.0f) + gap)), 1, 3);
    const float cw = (avail - gap * (cols - 1)) / cols, art = cw * 0.5f, ch = art + ui::Dp(118.0f);
    static int hovered = -1;
    static double hoverAt = 0.0;
    for (int i = 0; i < static_cast<int>(sizeof(kCards) / sizeof(kCards[0])); ++i) {
        const Card& c = kCards[i];
        if (i % cols) ImGui::SameLine(0.0f, gap);
        ImGui::PushID(i);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::InvisibleButton("##card", ImVec2(cw, ch));
        const bool hov = ImGui::IsItemHovered();
        if (hov && hovered != i) hovered = i, hoverAt = ImGui::GetTime();
        else if (!hov && hovered == i) hovered = -1;
        if (hov) NoteEditsHomeAnimating();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float r = ui::Dp(10.0f);
        dl->AddRectFilled(p, ImVec2(p.x + cw, p.y + ch), IM_COL32(255, 255, 255, hov ? 18 : 10), r);
        DrawEditsArt(dl, c.mode, p, cw, art, hov ? static_cast<float>(ImGui::GetTime() - hoverAt) : -1.0f);
        const float pad = ui::Dp(12.0f);
        float y = p.y + art + ui::Dp(8.0f);
        ImGui::PushFont(ui::TitleFont());
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.8f, ImVec2(p.x + pad, y), ImGui::GetColorU32(ImGuiCol_Text), c.title);
        y += ImGui::GetFontSize() * 0.8f + ui::Dp(4.0f);
        ImGui::PopFont();
        dl->AddText(nullptr, 0.0f, ImVec2(p.x + pad, y), ImGui::GetColorU32(ui::kTextDim), c.what, nullptr, cw - pad * 2.0f);
        y += ImGui::GetFontSize() * 2.3f + ui::Dp(6.0f);
        float x = p.x + pad; // the examples, as chips
        for (const char* e : c.examples) {
            const ImVec2 ts = ImGui::CalcTextSize(e);
            const float w = ts.x + ui::Dp(12.0f);
            if (x + w > p.x + cw - pad) x = p.x + pad, y += ts.y + ui::Dp(8.0f);
            if (y + ts.y > p.y + ch - ui::Dp(4.0f)) break;
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + ts.y + ui::Dp(4.0f)), IM_COL32(255, 255, 255, 22), ui::Dp(8.0f));
            dl->AddText(ImVec2(x + ui::Dp(6.0f), y + ui::Dp(2.0f)), ImGui::GetColorU32(ImGuiCol_Text), e);
            x += w + ui::Dp(6.0f);
        }
        if (hov) dl->AddRect(p, ImVec2(p.x + cw, p.y + ch), ImGui::GetColorU32(ui::kAccentHover), r, 0, ui::Dp(1.5f));
        if (clicked) SetEditsMode(c.mode);
        ImGui::PopID();
        if (i % cols == cols - 1) ImGui::Dummy(ImVec2(0, gap * 0.5f));
    }
    ImGui::EndChild();
}

void DrawEditsTab(AppState& app, float height) {
    static bool testMode = [] { // tests: MV_EDITS_MODE=n opens that mode once
        char buf[8] = {};
        if (GetEnvironmentVariableA("MV_EDITS_MODE", buf, sizeof(buf))) g_editsMode = std::clamp(atoi(buf), 0, 6);
        return true;
    }();
    (void)testMode;
    LoadEdits();
    Sync(app);
    { // every way to change a picture, one page: Qwen (say what to change), MOSS (boxes: designs with exact words, quick box edits), PhotoCraft
      // (text and graphics over a picture), upscale (bigger, better faces)
        const float y0 = ImGui::GetCursorPosY();
        static const char* kModes[] = {"Change anything", "Designs & exact text", "Text over a picture", "Upscale a picture", "One-click apps", "Videos",
                                       "All edits"};
        static const char* kTips[] = {
            "Say what to change: Qwen Image 2.1 Edit redraws the picture (best for anything a body does, a new background, a new style; ~30 s)",
            "Boxes: designs with exact words (covers, posters, thumbnails), or a quick edit of what is inside a box on any picture - recolour, remove, "
            "replace, change words (~4 s; the rest stays exactly as it is)",
            "Memes, thumbnails, captions, speech bubbles: real text layers over your picture (PhotoCraft, a layered .psd)",
            "Bigger pictures, better faces",
            "Ready-made recipes: relight, a new angle, expand, outfit swap, headshot, plushie, product shot... pick one, pick the picture, Make it",
            "Make a video crisp (MiniMax H3 redraws its detail at 1080p or 2K) or upscale it quickly",
            "Every kind of edit, with examples"};
        static const int kOrder[] = {6, 4, 0, 1, 2, 3, 5}; // (the start screen, then Apps)
        for (int k = 0; k < 7; ++k) {
            const int m = kOrder[k];
            if (k) ImGui::SameLine();
            const bool on = g_editsMode == m;
            ImGui::PushStyleColor(ImGuiCol_Button, on ? ui::WithAlpha(ui::kAccent, 0.85f) : ImGui::GetColorU32(ImGuiCol_Button));
            if (ImGui::Button(kModes[m])) SetEditsMode(m);
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", kTips[m]);
        }
        height -= ImGui::GetCursorPosY() - y0;
        if (g_editsMode == 6) {
            DrawEditsHome(app, height);
            return;
        }
        if (g_editsMode == 5) {
            DrawVideoEditsBody(app, height);
            return;
        }
        if (g_editsMode == 1) {
            DrawLayoutStudioBody(app, height);
            return;
        }
        if (g_editsMode == 2) {
            DrawDesignerBody(app, height);
            return;
        }
        if (g_editsMode == 3) {
            DrawUpscaleTab(app, height);
            return;
        }
        if (g_editsMode == 4) {
            DrawAppsBody(app, height);
            return;
        }
    }
    if (!ImGui::BeginTable("edits_layout", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings, ImVec2(0, height))) return;
    ImGui::TableSetupColumn("List", ImGuiTableColumnFlags_WidthStretch, 0.18f);
    ImGui::TableSetupColumn("Picture", ImGuiTableColumnFlags_WidthStretch, 0.46f);
    ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch, 0.36f);
    ImGui::TableNextRow(0, height);

    // ---- the list, newest first
    ImGui::TableSetColumnIndex(0);
    ImGui::BeginChild("edits_list", ImVec2(0, height), true);
    if (const int cleanup = DrawListCleanup("editsclean", !g_edits.empty())) {
        const size_t before = g_edits.size();
        g_edits.erase(std::remove_if(g_edits.begin(), g_edits.end(), [&](EditRecord& r) {
                          if (r.activity > 0) return false; // (still being made)
                          if (cleanup == 2) return true;
                          r.results.erase(std::remove_if(r.results.begin(), r.results.end(), [](const std::wstring& f) { return !FileStillThere(f); }), r.results.end());
                          return r.results.empty();
                      }),
                      g_edits.end());
        if (!Find(g_sel)) g_sel = g_edits.empty() ? -1 : g_edits.back().id;
        SaveEdits();
        const size_t n = before - g_edits.size();
        app.SetStatus(n == 0 ? std::string("Every edit on the list still has its pictures") : "Took " + std::to_string(n) + (n == 1 ? " edit" : " edits") + " off the list (no files deleted)");
    }
    if (g_edits.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Every picture edit shows up here, newest first, and is kept between sessions.");
        ImGui::PopTextWrapPos();
    }
    int removeId = -1;
    for (size_t k = g_edits.size(); k-- > 0;) {
        EditRecord& r = g_edits[k];
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
        Thumb(app, r.results.empty() ? (r.images.empty() ? std::wstring() : r.images.front()) : r.results.back(), ImVec2(row - 8, row - 8));
        ImGui::SetCursorScreenPos(keep);
        dl->PushClipRect(ImVec2(pos.x + row, pos.y), ImVec2(pos.x + w - 4, pos.y + row), true);
        dl->AddText(ImVec2(pos.x + row + 2, pos.y + 6), ImGui::GetColorU32(ImGuiCol_Text), r.label.c_str());
        const std::string sub = r.activity > 0 ? (r.state.empty() ? "Generating..." : r.state + "...") : !r.error.empty() ? "Failed" : std::to_string(r.results.size()) + (r.results.size() == 1 ? " picture" : " pictures");
        dl->AddText(ImVec2(pos.x + row + 2, pos.y + 6 + ImGui::GetFontSize() + 3), r.error.empty() ? ImGui::GetColorU32(ui::kTextDim) : ImGui::GetColorU32(ui::kBad), sub.c_str());
        dl->PopClipRect();
        ImGui::PopID();
    }
    if (removeId >= 0) {
        g_edits.erase(std::remove_if(g_edits.begin(), g_edits.end(), [&](const EditRecord& r) { return r.id == removeId; }), g_edits.end());
        if (g_sel == removeId) g_sel = g_edits.empty() ? -1 : g_edits.back().id;
        SaveEdits();
    }
    ImGui::EndChild();

    EditRecord* rec = Find(g_sel);
    if (!rec && !g_edits.empty()) rec = &g_edits.back();

    // ---- the result
    ImGui::TableSetColumnIndex(1);
    ImGui::BeginChild("edits_view", ImVec2(0, height), true, ImGuiWindowFlags_NoScrollbar);
    if (!rec) {
        ImGui::Spacing();
        ImGui::PushFont(ui::BoldFont());
        ImGui::TextUnformatted("Nothing edited yet");
        ImGui::PopFont();
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Edits come from:");
        ImGui::BulletText("Edit with Qwen... on a picture in the gallery, the Vision library or the ComfyUI viewer");
        ImGui::BulletText("The Vision board's \"Make the picture from the cards\"");
        ImGui::BulletText("Swapping a character in the Characters tab, and Clef mode");
        ImGui::Spacing();
        ImGui::TextDisabled("Each one shows its result here, what it was made from and the board it came from, where any character can be swapped for another.");
        ImGui::PopTextWrapPos();
    }
    else {
        ImGui::PushFont(ui::BoldFont());
        ImGui::TextUnformatted(rec->label.c_str());
        ImGui::PopFont();
        DrawResult(app, *rec, height);
    }
    ImGui::EndChild();

    // ---- what it was made from, the instruction, the board
    ImGui::TableSetColumnIndex(2);
    ImGui::BeginChild("edits_details", ImVec2(0, height), true);
    if (rec) {
        static std::wstring swapOld;
        static bool wantSwap = false;
        static std::string instr;
        static int instrFor = -1, times = 1;
        if (instrFor != rec->id) {
            instr = rec->instruction;
            instrFor = rec->id;
        }
        ImGui::TextUnformatted("Made from");
        for (size_t i = 0; i < rec->images.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            Thumb(app, rec->images[i], ImVec2(ui::Dp(52.0f), ui::Dp(52.0f)));
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::TextColored(i == 0 ? ui::kAccent : ui::kTextDim, "<image%d>", static_cast<int>(i) + 1);
            ImGui::TextUnformatted(i < rec->roles.size() ? rec->roles[i].c_str() : "");
            if (ImGui::SmallButton("Swap...")) {
                swapOld = rec->images[i];
                wantSwap = true; // opened below, outside this row's ID scope
            }
            ImGui::EndGroup();
            ImGui::PopID();
        }
        if (wantSwap) {
            ImGui::OpenPopup("##editswap");
            wantSwap = false;
        }
        if (ImGui::BeginPopup("##editswap")) {
            ImGui::TextUnformatted("Swap it for:");
            ImGui::BeginChild("##editswaplist", ImVec2(ui::Dp(320.0f), ui::Dp(260.0f)), true);
            bool any = false;
            for (const CharacterProfile& p : AllProfiles()) {
                if (!p.saved || _wcsicmp(p.image.c_str(), swapOld.c_str()) == 0) continue;
                any = true;
                const float row = ui::Dp(44.0f);
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                ImGui::PushID(p.key.c_str());
                const bool pick = ImGui::Selectable("##pick", false, 0, ImVec2(0, row));
                ImGui::PopID();
                ImGui::SetCursorScreenPos(ImVec2(pos.x + 2, pos.y + 2));
                Thumb(app, p.image, ImVec2(row - 4, row - 4));
                ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x + row + 8, pos.y + (row - ImGui::GetFontSize()) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), NameOf(p.image).c_str());
                ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + row + ImGui::GetStyle().ItemSpacing.y));
                if (pick) {
                    SwapIn(app, *rec, swapOld, p.image);
                    ImGui::CloseCurrentPopup();
                    break;
                }
            }
            if (!any) ImGui::TextDisabled("No other saved characters (Characters tab).");
            ImGui::EndChild();
            if (ImGui::Button("A picture file...")) {
                const std::wstring f = OpenImageFileDialog(GetActiveWindow());
                if (!f.empty()) {
                    SwapIn(app, *rec, swapOld, f);
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Vision library's picture")) {
                if (app.visionLibSelected >= 0 && app.visionLibSelected < static_cast<int>(app.visionLib.size())) {
                    SwapIn(app, *rec, swapOld, app.visionLib[app.visionLibSelected].imagePath);
                    ImGui::CloseCurrentPopup();
                } else {
                    app.SetStatus("Select a picture in the Vision library first");
                }
            }
            ImGui::EndPopup();
        }
        ImGui::Spacing();
        ImGui::TextUnformatted("What was asked");
        WrapEdit("##editinstr", instr, ui::Dp(170.0f));
        if (ui::PrimaryButton("Make another version", ImVec2(0, 0))) {
            EditContext ctx;
            ctx.roles = rec->roles;
            ctx.hasBoard = rec->hasBoard;
            ctx.board = rec->board;
            ctx.scene = rec->scene;
            ctx.prompt = rec->prompt;
            ctx.canvas = rec->canvas;
            SendPictureEdit(app, rec->images, instr, rec->label, std::clamp(times, 1, 100), &ctx);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ui::Dp(100.0f));
        ImGui::InputInt("times##edittimes", &times, 1, 5);
        times = std::clamp(times, 1, 100);
        if (!rec->error.empty() && rec->results.empty()) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(ui::kBad, "%s", rec->error.c_str());
            ImGui::PopTextWrapPos();
        }
        if (rec->hasBoard) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextUnformatted("The Vision board it came from");
            const std::wstring clicked = DrawMiniBoard(app, *rec, ImVec2(-FLT_MIN, ui::Dp(250.0f)));
            if (!clicked.empty()) {
                swapOld = clicked;
                ImGui::OpenPopup("##editswap");
            }
            if (ImGui::Button("Open this board in the Vision board tab")) {
                app.board.board = rec->board;
                app.board.curScene = std::clamp(rec->scene, 0, static_cast<int>(rec->board.scenes.size()) - 1);
                app.board.fitRequest = true;
                app.board.selNode = app.board.selLink = -1;
                app.board.histDirty = true;
                app.board.dirty = true;
                app.board.dirtyAt = ImGui::GetTime();
                app.requestTab = 3;
                app.SetStatus("The board is on the Vision board tab (Ctrl+Z brings the previous one back)");
            }
        }
    }
    ImGui::EndChild();
    ImGui::EndTable();
}
