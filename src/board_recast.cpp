// "Same scene, other characters": the Vision board's scene (cards, relations, camera, style) with different characters in it. Pick who plays
// each character, and get the prompt for that cast; keep several casts side by side ("now with 3 and 4") and send any of them to ComfyUI.
#include <algorithm>
#include <filesystem>
#include <windows.h>

#include "board.h"
#include "imgui.h"
#include "ui_extra.h"
#include "ui_theme.h"
#include "vision_prompt.h"
#include "win_dialogs.h"

namespace fs = std::filesystem;

namespace {

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(len > 0 ? len - 1 : 0, '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), len, nullptr, nullptr);
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

struct Cast {
    int id = 0;
    std::vector<std::pair<std::wstring, std::wstring>> map; // (a character on the board, who plays it instead; "" = the same)
    std::string result;   // the prompt written by the model
    std::string draft;    // the quick draft (no model)
    bool writing = false;
};
std::vector<Cast> g_casts;
int g_nextId = 1;
bool g_open = false;
bool g_keepGear = false; // the new character also gets what the old one wore / held / carried

struct Slot {
    std::wstring image;
    std::string name;
};

bool SameImage(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

// the characters on the board, left to right (one slot per picture, whatever the scene)
std::vector<Slot> Slots(const Board& b) {
    std::vector<const BoardNode*> nodes;
    for (const BoardNode& n : b.nodes) {
        if (n.kind != BoardKind::Character || n.image.empty()) continue;
        bool seen = false;
        for (const BoardNode* o : nodes) seen = seen || SameImage(o->image, n.image);
        if (!seen) nodes.push_back(&n);
    }
    std::sort(nodes.begin(), nodes.end(), [](const BoardNode* a, const BoardNode* c) { return a->x < c->x; });
    std::vector<Slot> out;
    for (const BoardNode* n : nodes) out.push_back({n->image, b.DisplayName(*n)});
    return out;
}

std::wstring PickFor(const Cast& c, const std::wstring& image) {
    for (const auto& m : c.map) {
        if (SameImage(m.first, image)) return m.second;
    }
    return {};
}

void SetPick(Cast& c, const std::wstring& image, const std::wstring& who) {
    for (auto& m : c.map) {
        if (SameImage(m.first, image)) {
            m.second = who;
            return;
        }
    }
    c.map.push_back({image, who});
}

std::string CharLabel(const std::wstring& image) {
    if (const CharacterProfile* p = FindProfile(image)) {
        if (!p->name.empty()) return p->name;
    }
    return ToUtf8(fs::path(image).stem().wstring());
}

struct Variant {
    Board board;
    std::vector<std::string> keys; // the temporary inventories made for it
};

// the board with the cast's characters in place of the originals: their picture, name, description and their own inventory
// (the old character's things only come along when asked)
Variant MakeVariant(const Board& original, const Cast& c) {
    Variant v;
    v.board = original;
    for (BoardNode& n : v.board.nodes) {
        if (n.kind != BoardKind::Character || n.image.empty()) continue;
        const std::wstring who = PickFor(c, n.image);
        if (who.empty() || SameImage(who, n.image)) continue;
        std::vector<InvItem> oldItems;
        int oldNext = 1;
        if (const CharacterProfile* old = ProfileOf(n)) {
            oldItems = old->items;
            oldNext = old->nextItemId;
        }
        const CharacterProfile* tpl = FindProfile(who);
        n.image = who;
        n.title = tpl && !tpl->name.empty() ? tpl->name : ToUtf8(fs::path(who).stem().wstring());
        n.text = tpl ? tpl->description : std::string();
        n.profile.clear();
        CharacterProfile& np = EnsureProfileOf(v.board, n.id, false);
        v.keys.push_back(np.key);
        if (g_keepGear && np.items.empty() && !oldItems.empty()) {
            np.items = oldItems;
            np.nextItemId = oldNext;
        }
    }
    return v;
}

void Discard(const Variant& v) {
    for (const std::string& k : v.keys) DiscardProfile(k);
}

bool ModelReady(AppState& app) {
    if (!app.visionModel.empty()) return true;
    app.SetStatus("Pick a vision model first (Vision prompt window)");
    app.showVisionWindow = true;
    return false;
}

void Write(AppState& app, Cast& c) {
    if (c.writing || !ModelReady(app)) return;
    const BoardPromptOptions opt = BoardCurrentOptions(app.board);
    Variant v = MakeVariant(app.board.board, c);
    VisionService::ComposeRequest rq;
    rq.tag = "recast:" + std::to_string(c.id);
    rq.prompt = BuildBoardBrief(v.board, opt, rq.images);
    c.draft = AssembleBoardPrompt(v.board, opt);
    Discard(v);
    rq.numPredict = opt.format == 2 ? std::min(3000, 500 + 300 * static_cast<int>(v.board.scenes.size())) : 700;
    rq.temperature = 0.7;
    c.writing = true;
    app.vision.Compose(app.VisionEndpoint(), app.visionModel, app.visionKeepAlive, std::move(rq));
}

void Thumb(AppState& app, const std::wstring& path, float size) {
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

} // namespace

void OpenRecast() {
    g_open = true;
    if (g_casts.empty()) {
        Cast c;
        c.id = g_nextId++;
        g_casts.push_back(std::move(c));
    }
}

void PumpRecastResults(AppState& app) {
    if (app.composeResults.empty()) return;
    std::vector<VisionService::Result> keep;
    for (VisionService::Result& r : app.composeResults) {
        const std::string tag = ToUtf8(r.path);
        if (tag.rfind("recast:", 0) != 0) {
            keep.push_back(std::move(r));
            continue;
        }
        const int id = std::atoi(tag.c_str() + 7);
        for (Cast& c : g_casts) {
            if (c.id != id) continue;
            c.writing = false;
            if (r.text.empty()) app.SetStatus("The model did not write the prompt: " + r.error);
            else c.result = r.text;
        }
    }
    app.composeResults = std::move(keep);
}

void DrawRecastWindow(AppState& app) {
    if (!g_open) return;
    ImGui::SetNextWindowSize(ImVec2(ui::Dp(760.0f), ui::Dp(680.0f)), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Same scene, other characters", &g_open)) {
        ImGui::End();
        return;
    }
    Board& board = app.board.board;
    const std::vector<Slot> slots = Slots(board);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Pick who plays each character. The scene, the relations (\"fights\"...), the camera and the style stay exactly as on the board. "
                        "Make several casts to compare them.");
    ImGui::PopTextWrapPos();
    if (slots.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ui::kWarn, "There are no character cards with a picture on the board yet.");
        ImGui::End();
        return;
    }
    struct SavedChar {
        std::wstring image;
        std::string key;
    };
    std::vector<SavedChar> saved; // copies: writing a prompt makes and forgets temporary profiles, so no pointers into the list are kept
    for (const CharacterProfile& p : AllProfiles()) {
        if (p.saved) saved.push_back({p.image, p.key});
    }
    if (saved.empty()) {
        ImGui::TextColored(ui::kWarn, "No saved characters yet: add some in the Characters tab (or press \"Save character\" on a card).");
    }
    ImGui::Checkbox("The new characters also get what the old ones wear, hold and carry", &g_keepGear);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Off: each new character brings only their own saved inventory");
    int removeAt = -1;
    for (size_t ci = 0; ci < g_casts.size(); ++ci) {
        Cast& c = g_casts[ci];
        ImGui::PushID(c.id);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::PushFont(ui::BoldFont());
        ImGui::Text("Cast %d", static_cast<int>(ci) + 1);
        ImGui::PopFont();
        for (const Slot& s : slots) {
            ImGui::PushID(ToUtf8(s.image).c_str());
            Thumb(app, s.image, ui::Dp(40.0f));
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s", s.name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("->");
            ImGui::SameLine();
            const std::wstring who = PickFor(c, s.image);
            if (!who.empty()) {
                Thumb(app, who, ui::Dp(40.0f));
                ImGui::SameLine();
            }
            ImGui::SetNextItemWidth(ui::Dp(240.0f));
            const std::string preview = who.empty() ? std::string("(keep the same)") : CharLabel(who);
            if (ImGui::BeginCombo("##who", preview.c_str())) {
                if (ImGui::Selectable("(keep the same)", who.empty())) SetPick(c, s.image, std::wstring());
                for (const SavedChar& p : saved) {
                    if (ImGui::Selectable((CharLabel(p.image) + "##" + p.key).c_str(), SameImage(p.image, who))) SetPick(c, s.image, p.image);
                }
                ImGui::Separator();
                if (ImGui::Selectable("+ Another picture...")) {
                    const std::wstring f = OpenImageFileDialog(GetActiveWindow());
                    if (!f.empty()) {
                        AddSavedCharacter(app, f);
                        SetPick(c, s.image, f);
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();
        }
        ImGui::Spacing();
        ImGui::BeginDisabled(c.writing);
        if (ui::PrimaryButton(c.writing ? "Writing..." : "Write the prompt", ImVec2(ui::Dp(150.0f), 0))) Write(app, c);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Quick draft")) {
            const BoardPromptOptions opt = BoardCurrentOptions(app.board);
            Variant v = MakeVariant(board, c);
            c.draft = AssembleBoardPrompt(v.board, opt);
            Discard(v);
            c.result.clear();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Without the model: the cards' descriptions put together");
        ImGui::SameLine();
        if (ImGui::Button("Put this cast on the board")) {
            Variant v = MakeVariant(board, c); // its inventories stay: they are the board's now
            board.nodes = v.board.nodes;
            SaveProfiles();
            app.board.histDirty = true;
            app.board.dirty = true;
            app.board.dirtyAt = ImGui::GetTime();
            app.SetStatus("The board now has this cast (Ctrl+Z undoes it)");
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Replace the characters on the board itself");
        ImGui::SameLine();
        ImGui::BeginDisabled(g_casts.size() < 2);
        if (ImGui::Button("Remove")) removeAt = static_cast<int>(ci);
        ImGui::EndDisabled();

        std::string& text = c.result.empty() ? c.draft : c.result;
        if (!text.empty() || c.writing) {
            ImGui::TextDisabled(c.writing ? "The model is writing it..." : (c.result.empty() ? "Quick draft:" : "Prompt:"));
            WrapEdit("##castresult", text, ui::Dp(150.0f));
            if (ImGui::Button("Copy")) {
                ImGui::SetClipboardText(text.c_str());
                app.SetStatus("Copied the prompt");
            }
            ImGui::SameLine();
            const double secs = app.board.format == 2 ? static_cast<double>(board.TotalSec()) : 0.0;
            std::string label = "Vision board, cast " + std::to_string(ci + 1);
            if (ui::PrimaryButton("Send to ComfyUI", ImVec2(0, 0))) SendToComfy(app, text, std::string(), std::wstring(), label, true, secs);
            ImGui::SameLine();
            if (ImGui::Button("Edit and send...")) OpenSendEditor(app, text, std::string(), std::wstring(), label, secs);
        }
        ImGui::PopID();
    }
    if (removeAt >= 0) g_casts.erase(g_casts.begin() + removeAt);

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("+ Another cast")) {
        Cast c;
        c.id = g_nextId++;
        if (!g_casts.empty()) c.map = g_casts.back().map; // starts as a copy of the last one: change just who you want
        g_casts.push_back(std::move(c));
    }
    ImGui::SameLine();
    if (ImGui::Button("Write all")) {
        for (Cast& c : g_casts) Write(app, c);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("The model writes them one after the other.");
    ImGui::End();
}

Board RecastBoard(const Board& original, const std::vector<std::pair<std::wstring, std::wstring>>& map) {
    Cast c;
    c.map = map;
    Variant v = MakeVariant(original, c); // the inventories made for it stay: the returned board refers to them
    return v.board;
}
