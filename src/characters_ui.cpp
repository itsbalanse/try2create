// The Characters tab: saved characters (a picture, a name, a description, an inventory) and "swap": put a character into any picture's
// scenario, keeping everything else, as a new prompt for ComfyUI or any other generator.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <windows.h>
#include <shellapi.h>

#include "board.h"
#include "image_texture.h"
#include "imgui.h"
#include "imgui_internal.h" // RenderTextEllipsis
#include "ui_extra.h"
#include "library_ui.h"
#include "notify.h"
#include "ui_theme.h"
#include "vision_prompt.h"
#include "win_dialogs.h"

namespace fs = std::filesystem;

// Tests (MV_UI_TEST, like main.cpp's hooks): "charsel:3" -> 3, "charorbit" -> 0, absent -> -1
static int UiTestNumber(const char* key) {
    char buf[256] = {};
    if (!GetEnvironmentVariableA("MV_UI_TEST", buf, sizeof(buf))) return -1;
    const std::string spec = buf;
    const size_t len = strlen(key);
    for (size_t at = spec.find(key); at != std::string::npos; at = spec.find(key, at + 1)) {
        if (at > 0 && spec[at - 1] != ';') continue;
        const size_t end = at + len;
        if (end == spec.size() || spec[end] == ';') return 0;
        if (spec[end] == ':') return atoi(spec.c_str() + end + 1);
    }
    return -1;
}

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

bool InputLine(const char* id, const char* hint, std::string& value, float width) {
    ImGui::SetNextItemWidth(width);
    return ImGui::InputTextWithHint(id, hint, value.data(), value.capacity() + 1, ImGuiInputTextFlags_CallbackResize, StringResize, &value);
}

bool InputBox(const char* id, std::string& value, float height) {
    return ImGui::InputTextMultiline(id, value.data(), value.capacity() + 1, ImVec2(-FLT_MIN, height), ImGuiInputTextFlags_CallbackResize, StringResize, &value);
}

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string ReadTextFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) text.erase(0, 3);
    if (text.size() > 20000) text.resize(20000);
    return Trim(text);
}

// The prompt that sits next to a picture (a Vision result: <name>.txt, else <name>.tags.txt).
std::string SidecarPrompt(const std::wstring& image) {
    fs::path p(image);
    for (const wchar_t* ext : {L".txt", L".tags.txt"}) {
        fs::path t = p;
        t.replace_extension(ext);
        const std::string text = ReadTextFile(t);
        if (!text.empty()) return text;
    }
    return {};
}

struct SwapState {
    std::wstring scene;        // the picture whose scenario is kept
    std::string scenePrompt;   // its prompt (may be empty: the model then looks at the picture)
    std::string who = "the main person"; // who in the picture is replaced
    bool keepOutfit = false;
    bool sendAfter = false;
    std::string result;
    std::string resultName;    // the character the result is for
    std::string resultLabel;   // what the ComfyUI tab calls it
    std::wstring saveImage;    // the picture a saved result is kept next to
    std::wstring sendPicture;  // the picture sent along to ComfyUI (the scene, for a swap)
    std::string saveStyle;     // the style name a saved result gets ("swap-alice")
    bool writing = false;
};
SwapState g_swap;
std::string g_selKey;                 // the selected character
std::set<std::string> g_describing;   // characters being described by the model right now

std::string CharName(const CharacterProfile& p) { return p.name.empty() ? ToUtf8(fs::path(p.image).stem().wstring()) : p.name; }

CharacterProfile* FindByKey(const std::string& key) {
    for (CharacterProfile& p : AllProfiles()) {
        if (p.key == key) return &p;
    }
    return nullptr;
}

bool ModelReady(AppState& app) {
    if (!app.visionModel.empty()) return true;
    app.SetStatus("Pick a vision model first (Vision prompt window)");
    app.showVisionWindow = true;
    return false;
}

void DescribeCharacter(AppState& app, const CharacterProfile& p) {
    if (g_describing.count(p.key) || !ModelReady(app)) return;
    g_describing.insert(p.key);
    VisionService::ComposeRequest rq;
    rq.tag = "char:" + p.key;
    rq.prompt = "Describe ONLY the main character (person, creature or figure) in this image, as a reference for writing a scene prompt. In 2-3 "
                "sentences give: species/gender/age, hair, face, build, clothing and accessories, plus their usual expression. Do not describe "
                "the background and do not describe what they are doing. No preamble.";
    rq.images = {p.image};
    rq.numPredict = 300;
    rq.temperature = 0.3;
    rq.maxImageSide = 768;
    app.vision.Compose(app.VisionEndpoint(), app.visionModel, app.visionKeepAlive, std::move(rq));
}

std::string StyleSlug(const std::string& name) {
    std::string out;
    for (unsigned char c : name) {
        if (std::isalnum(c)) out += static_cast<char>(std::tolower(c));
        else if (!out.empty() && out.back() != '-') out += '-';
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "character" : out;
}

void StartSwap(AppState& app, const CharacterProfile& p, bool sendAfter) {
    if (g_swap.writing || g_swap.scene.empty() || !ModelReady(app)) return;
    std::string who = Trim(g_swap.who);
    if (who.empty()) who = "the main person";
    const std::string name = CharName(p);
    std::string inv = InventoryText(p);
    std::string t;
    t += "You rewrite image-generation prompts.\n";
    t += "Picture 1 is the original scene.";
    if (!p.image.empty()) t += " Picture 2 is a reference of the new character (" + name + ") who takes the place of " + who + ".";
    t += "\n\n";
    if (!Trim(g_swap.scenePrompt).empty()) t += "The original prompt of picture 1:\n" + Trim(g_swap.scenePrompt) + "\n\n";
    t += "The new character, " + name + ": " + (Trim(p.description).empty() ? std::string("as shown in picture 2") : Trim(p.description));
    if (!inv.empty()) t += " They are " + inv + ".";
    t += "\n\nWrite ONE new image-generation prompt for the same scene as picture 1: the same place, composition, camera angle, lighting, pose, action "
         "and mood, with " + who + " replaced by " + name + ". Describe " + name + "'s face, hair and build";
    t += g_swap.keepOutfit ? ", and keep the clothes the replaced person wears. " : ", and give them their own clothes and accessories. ";
    t += "Keep every other person and object in the scene as it is. Write it as a normal prompt: never mention picture 1, picture 2, a reference, or "
         "the replacement. Reply with the prompt only.";
    VisionService::ComposeRequest rq;
    rq.tag = "swap";
    rq.prompt = t;
    rq.images = {g_swap.scene};
    if (!p.image.empty()) rq.images.push_back(p.image);
    rq.numPredict = 700;
    rq.temperature = 0.5;
    rq.maxImageSide = 768;
    g_swap.writing = true;
    g_swap.sendAfter = sendAfter;
    g_swap.resultName = name;
    g_swap.resultLabel = name + " in " + ToUtf8(fs::path(g_swap.scene).stem().wstring());
    g_swap.saveImage = g_swap.scene;
    g_swap.sendPicture = g_swap.scene;
    g_swap.saveStyle = "swap-" + StyleSlug(name);
    app.vision.Compose(app.VisionEndpoint(), app.visionModel, app.visionKeepAlive, std::move(rq));
}

// The fun modes: a character (and maybe a second one) plus a task in words. `kind` names the saved style ("world", "transform"...).
void StartRemix(AppState& app, const CharacterProfile& p, const std::string& task, const CharacterProfile* other, const std::string& kind, bool sendAfter) {
    if (g_swap.writing || !ModelReady(app)) return;
    const std::string name = CharName(p);
    std::string inv = InventoryText(p);
    std::string t = "You write image-generation prompts.\nPicture 1 shows the character " + name + ": " +
                    (Trim(p.description).empty() ? std::string("as shown in the picture") : Trim(p.description));
    if (!inv.empty()) t += " They are " + inv + ".";
    t += "\n";
    if (other) {
        t += "Picture 2 shows the character " + CharName(*other) + ": " + (Trim(other->description).empty() ? std::string("as shown in the picture") : Trim(other->description)) + "\n";
    }
    t += "\nTask: " + task + "\n\nWrite ONE detailed image-generation prompt: the subject's face, hair, build and clothes, the setting, the pose and action, the "
         "lighting, the camera framing and the mood. Never mention \"picture 1\", \"picture 2\", a reference or the task. Reply with the prompt only.";
    VisionService::ComposeRequest rq;
    rq.tag = "swap";
    rq.prompt = t;
    rq.images = {p.image};
    if (other) rq.images.push_back(other->image);
    rq.numPredict = 700;
    rq.temperature = 0.8;
    rq.maxImageSide = 768;
    g_swap.writing = true;
    g_swap.sendAfter = sendAfter;
    g_swap.resultName = name;
    g_swap.resultLabel = name + (other ? " + " + CharName(*other) : std::string()) + " (" + kind + ")";
    g_swap.saveImage = p.image;
    g_swap.sendPicture.clear();
    g_swap.saveStyle = kind + "-" + StyleSlug(name);
    app.vision.Compose(app.VisionEndpoint(), app.visionModel, app.visionKeepAlive, std::move(rq));
}

// Preset buttons that wrap onto several lines; true when one was pressed (its text in `picked`).
bool PresetRow(const std::vector<const char*>& names, std::string& picked) {
    bool any = false;
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    for (size_t i = 0; i < names.size(); ++i) {
        const float w = ImGui::CalcTextSize(names[i]).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        if (i > 0) {
            if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + w < right) ImGui::SameLine();
        }
        if (ImGui::SmallButton(names[i])) {
            picked = names[i];
            any = true;
        }
    }
    return any;
}

std::string Pick(const std::vector<const char*>& list) {
    static std::mt19937 rng{std::random_device{}()};
    return list[rng() % list.size()];
}

// Writes the swap result next to the picture as a styled prompt ("swap-<name>"), so it shows up under that picture in the Vision library.
void SaveSwap(AppState& app) {
    if (g_swap.result.empty() || g_swap.saveImage.empty()) return;
    const std::wstring suffix = L".style-" + ToWide(g_swap.saveStyle);
    const std::wstring outDir = ToWide(app.visionOutputDir);
    bool inLibrary = false;
    for (const AppState::VisionLibItem& it : app.visionLib) {
        if (_wcsicmp(it.imagePath.c_str(), g_swap.saveImage.c_str()) == 0) inLibrary = true;
    }
    std::string err;
    bool ok = false;
    if (inLibrary) { // already there: the text goes right beside it
        fs::path t(g_swap.saveImage);
        t.replace_extension(suffix + L".txt");
        std::ofstream out(t, std::ios::binary | std::ios::trunc);
        ok = static_cast<bool>(out);
        if (ok) out << g_swap.result;
    } else if (!outDir.empty()) {
        std::wstring saved;
        ok = SaveImageWithPrompt(g_swap.saveImage, outDir, g_swap.result, saved, &err, suffix);
    } else {
        err = "Choose the Vision results folder first (Vision library tab)";
    }
    if (ok) {
        app.RefreshVisionLibrary();
        app.SetStatus("Saved with the picture as the style \"" + g_swap.saveStyle + "\" (Vision library)");
    } else {
        app.SetStatus(err.empty() ? "Could not save it" : err);
    }
}

void PickScene(const std::wstring& path) {
    g_swap.scene = path;
    g_swap.scenePrompt = SidecarPrompt(path);
    g_swap.result.clear();
}

// A picture dropped from the Vision library
bool AcceptLibraryDrop(std::wstring& path) {
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("MV_LIBITEM")) {
            path = static_cast<const wchar_t*>(pl->Data);
            ImGui::EndDragDropTarget();
            return true;
        }
        ImGui::EndDragDropTarget();
    }
    return false;
}

void DrawThumb(AppState& app, const std::wstring& path, ImVec2 size, bool crop) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const AppState::LibThumb t = app.LibraryThumb(path);
    if (t.srv && t.w > 0 && t.h > 0) {
        ImVec2 uv0(0, 0), uv1(1, 1);
        ImVec2 a = p, b(p.x + size.x, p.y + size.y);
        if (crop) {
            if (t.w > t.h) { const float m = (1.0f - float(t.h) / t.w) * 0.5f; uv0.x = m; uv1.x = 1 - m; }
            else if (t.h > t.w) { const float m = (1.0f - float(t.w) / t.h) * 0.5f; uv0.y = m; uv1.y = 1 - m; }
        } else {
            const float s = std::min(size.x / t.w, size.y / t.h);
            const ImVec2 dim(t.w * s, t.h * s);
            a = ImVec2(p.x + (size.x - dim.x) * 0.5f, p.y + (size.y - dim.y) * 0.5f);
            b = ImVec2(a.x + dim.x, a.y + dim.y);
        }
        dl->AddImageRounded((ImTextureID)(intptr_t)t.srv, a, b, uv0, uv1, IM_COL32_WHITE, ui::Dp(6.0f));
    } else {
        dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), ui::ThumbPlaceholder(), ui::Dp(6.0f));
    }
}

void DrawScenePicker(AppState& app) {
    app.RequestVisionLibrary();
    static std::string search;
    InputLine("##swapsearch", "Search the Vision library", search, ui::Dp(320.0f));
    std::vector<int> shown;
    const std::string q = [&] { std::string l = search; std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); }); return l; }();
    for (int i = 0; i < static_cast<int>(app.visionLib.size()); ++i) {
        if (q.empty() || app.visionLib[i].searchLower.find(q) != std::string::npos) shown.push_back(i);
    }
    const float cell = ui::Dp(84.0f), gap = ui::Dp(6.0f);
    const float width = ui::Dp(560.0f);
    const int cols = std::max(1, static_cast<int>((width + gap) / (cell + gap)));
    ImGui::BeginChild("##swappicks", ImVec2(width, ui::Dp(360.0f)), true);
    if (shown.empty()) ImGui::TextDisabled(app.visionLib.empty() ? "The Vision library is empty (describe some pictures first)." : "Nothing matches.");
    const int rows = (static_cast<int>(shown.size()) + cols - 1) / cols;
    ImGuiListClipper clipper;
    clipper.Begin(rows, cell + gap);
    while (clipper.Step()) {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
            for (int c = 0; c < cols; ++c) {
                const int k = r * cols + c;
                if (k >= static_cast<int>(shown.size())) break;
                const AppState::VisionLibItem& it = app.visionLib[shown[k]];
                if (c > 0) ImGui::SameLine(0, gap);
                ImGui::PushID(shown[k]);
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const bool clicked = ImGui::InvisibleButton("##p", ImVec2(cell, cell));
                ImGui::SetCursorScreenPos(p);
                DrawThumb(app, it.imagePath, ImVec2(cell, cell), true);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) || ImGui::IsMouseHoveringRect(p, ImVec2(p.x + cell, p.y + cell))) ImGui::SetTooltip("%s", ToUtf8(it.name).c_str());
                if (clicked) {
                    PickScene(it.imagePath);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------
void AddSavedCharacter(AppState& app, const std::wstring& path) {
    if (path.empty()) return;
    CharacterProfile& p = EnsureProfile(path);
    p.saved = true;
    if (p.name.empty()) p.name = ToUtf8(fs::path(path).stem().wstring());
    SaveProfiles();
    g_selKey = p.key;
    if (Trim(p.description).empty() && !app.visionModel.empty()) DescribeCharacter(app, p);
    if (p.backstory.empty()) QueueCharacterStory(app, path, false);
    app.SetStatus("Saved \"" + p.name + "\" in the Characters tab");
}

void SetSwapScene(AppState& app, const std::wstring& path) {
    PickScene(path);
    app.requestTab = 5;
}

void PumpCharacterResults(AppState& app) {
    if (app.composeResults.empty()) return;
    std::vector<VisionService::Result> keep;
    for (VisionService::Result& r : app.composeResults) {
        const std::string tag = ToUtf8(r.path);
        if (tag.rfind("char:", 0) == 0) {
            const std::string key = tag.substr(5);
            g_describing.erase(key);
            if (CharacterProfile* p = FindByKey(key)) {
                if (!r.text.empty()) {
                    p->description = Trim(r.text);
                    SaveProfiles();
                } else {
                    app.SetStatus("Could not describe the character: " + r.error);
                }
            }
        } else if (tag == "swap") {
            g_swap.writing = false;
            if (r.text.empty()) {
                app.SetStatus("The model did not write the swapped prompt: " + r.error);
            } else {
                g_swap.result = Trim(r.text);
                if (g_swap.sendAfter) {
                    g_swap.sendAfter = false;
                    SendToComfy(app, g_swap.result, std::string(), g_swap.sendPicture, g_swap.resultLabel);
                }
            }
        } else {
            keep.push_back(std::move(r));
        }
    }
    app.composeResults = std::move(keep);
}

void ShowSavedCharacter(AppState& app, const std::wstring& path) {
    g_selKey = CharacterKey(path);
    ShowCharacterCreator(false);
    app.requestTab = 5;
}

void DrawCharactersTab(AppState& app, float height) {
    {
        const bool creating = CharacterCreatorShown();
        auto tab = [](const char* label, bool on) {
            ImGui::PushStyleColor(ImGuiCol_Button, on ? ui::ChipActiveColor() : ui::ChipColor());
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::ChipHoverColor());
            const bool c = ImGui::Button(label);
            ImGui::PopStyleColor(2);
            return c;
        };
        if (tab("Your characters", !creating)) ShowCharacterCreator(false);
        ImGui::SameLine(0.0f, ui::Dp(4.0f));
        if (tab("Create a character", creating && CharacterCreatorMode() != 2)) {
            if (CharacterCreatorMode() == 2) SetCharacterCreatorMode(0);
            ShowCharacterCreator(true);
        }
        ImGui::SameLine(0.0f, ui::Dp(4.0f));
        if (tab("Extract from a picture", creating && CharacterCreatorMode() == 2)) ShowCharacterCreator(true), SetCharacterCreatorMode(2);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Every person or creature in a picture becomes one of your characters: from the gallery, a file, or pasted (or drag a picture onto this page)");
        ImGui::SameLine();
        ImGui::TextDisabled(creating && CharacterCreatorMode() == 2 ? "Every person or creature in a picture becomes one of your characters, drawn alone in a studio."
                            : creating ? "Build someone from looks or your photo: they become one of your characters everywhere."
                                     : "Your characters are used on the Vision board, in Clef mode, in edits and on the Video page.");
        height -= ImGui::GetFrameHeightWithSpacing();
        if (CharacterCreatorShown()) {
            DrawCharacterCreator(app, height);
            return;
        }
    }
    // the side columns fold to strips (the round "-" in their corner)
    static bool foldList = false, foldFun = false;
    const float strip = ui::Dp(28.0f);
    char layoutId[32];
    snprintf(layoutId, sizeof(layoutId), "chars_layout%d%d", foldList, foldFun);
    if (!ImGui::BeginTable(layoutId, 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings, ImVec2(0, height))) return;
    if (foldList) ImGui::TableSetupColumn("Saved", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, strip);
    else ImGui::TableSetupColumn("Saved", ImGuiTableColumnFlags_WidthStretch, 0.22f);
    ImGui::TableSetupColumn("Character", ImGuiTableColumnFlags_WidthStretch, 0.33f);
    if (foldFun) ImGui::TableSetupColumn("Swap", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, strip);
    else ImGui::TableSetupColumn("Swap", ImGuiTableColumnFlags_WidthStretch, 0.45f);
    ImGui::TableNextRow(0, height);

    // ---- saved characters
    ImGui::TableSetColumnIndex(0);
    if (foldList) {
        if (ui::FoldStrip("##stripsaved", "Saved characters", strip, height)) foldList = false;
    } else {
    ImGui::BeginChild("chars_list", ImVec2(0, height), true);
    if (ui::FoldButton("##foldsaved", "Fold the list away")) foldList = true;
    ImGui::TextUnformatted("Saved characters");
    ImGui::SameLine();
    DrawClearCharacters("list");
    { // the ones whose picture is gone; everyone's backstory
        int missing = 0, noStory = 0;
        std::error_code ec;
        for (const CharacterProfile& p : AllProfiles()) {
            if (!p.saved) continue;
            if (p.image.empty() || !fs::exists(p.image, ec)) ++missing;
            else if (p.backstory.empty() || CharacterNameIsPlaceholder(p)) ++noStory;
        }
        if (missing > 0) {
            char lbl[64];
            snprintf(lbl, sizeof(lbl), "Remove %d without a picture", missing);
            if (ImGui::SmallButton(lbl)) {
                for (CharacterProfile& p : AllProfiles()) {
                    if (p.saved && (p.image.empty() || !fs::exists(p.image, ec))) p.saved = false;
                }
                SaveProfiles();
                app.SetStatus("Took " + std::to_string(missing) + " characters whose picture is gone off the list");
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Their picture was moved or deleted: they leave the list");
        }
        if (noStory > 0) {
            const int queued = CharacterStoriesQueued();
            char lbl[80];
            snprintf(lbl, sizeof(lbl), queued ? "Writing backstories (%d left)..." : "Backstories for %d", queued ? queued : noStory);
            if (missing > 0) ui::SameLineIfFits(lbl);
            ImGui::BeginDisabled(queued > 0);
            if (ImGui::SmallButton(lbl)) {
                for (const CharacterProfile& p : AllProfiles()) {
                    if (p.saved && fs::exists(p.image, ec) && (p.backstory.empty() || CharacterNameIsPlaceholder(p))) QueueCharacterStory(app, p.image, false);
                }
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("The writer gives every character without one a backstory, and a real name to those with a made-up one (\"Lyra 2\", \"Blonde Bird\")");
        }
    }
    ImGui::PushStyleColor(ImGuiCol_Button, ui::kAccent);
    if (ImGui::SmallButton("+ Create a character...")) ShowCharacterCreator(true);
    ImGui::PopStyleColor();
    if (ImGui::SmallButton("+ From a file...")) {
        const std::wstring f = OpenImageFileDialog(GetActiveWindow());
        if (!f.empty()) AddSavedCharacter(app, f);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Paste")) {
        std::string msg;
        const std::wstring f = PasteBoardImage(msg);
        if (!f.empty()) AddSavedCharacter(app, f);
        else app.SetStatus(msg);
    }
    if (ImGui::SmallButton("+ Picture shown in the gallery")) {
        if (app.selectedIndex >= 0 && app.selectedIndex < static_cast<int>(app.entries.size())) AddSavedCharacter(app, app.entries[app.selectedIndex].path);
        else app.SetStatus("Open a picture in the gallery first");
    }
    if (ImGui::SmallButton("+ Library picture")) {
        if (app.visionLibSelected >= 0 && app.visionLibSelected < static_cast<int>(app.visionLib.size())) AddSavedCharacter(app, app.visionLib[app.visionLibSelected].imagePath);
        else app.SetStatus("Select a picture in the Vision library first");
    }
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("You can also drag a Vision library picture here.");
    ImGui::PopTextWrapPos();
    ImGui::Separator();
    // how they are shown: a list, a grid or a large grid (Ctrl + mouse wheel sizes the grid)
    {
        static const char* kViews[] = {"List", "Grid", "Large"};
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("View");
        for (int v = 0; v < 3; ++v) {
            ImGui::SameLine(0, v == 0 ? -1.0f : ui::Dp(2.0f));
            ImGui::PushStyleColor(ImGuiCol_Button, app.charView == v ? ui::ChipActiveColor() : ui::ChipColor());
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::ChipHoverColor());
            if (ImGui::SmallButton(kViews[v])) {
                app.charView = v;
                app.charTile = v == 1 ? 96.0f : 150.0f;
                app.SaveUiSettings();
            }
            ImGui::PopStyleColor(2);
            if (v > 0 && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Ctrl + mouse wheel over them makes the tiles bigger or smaller");
        }
    }
    ImGui::BeginChild("##charsscroll", ImVec2(0, 0), ImGuiChildFlags_None, ImGui::GetIO().KeyCtrl && app.charView > 0 ? ImGuiWindowFlags_NoScrollWithMouse : 0);
    if (app.charView > 0 && ImGui::IsWindowHovered() && ImGui::GetIO().KeyCtrl && ImGui::GetIO().MouseWheel != 0.0f) {
        app.charTile = std::clamp(app.charTile + ImGui::GetIO().MouseWheel * 12.0f, 60.0f, 260.0f);
        app.SaveUiSettings();
    }
    const float row = ui::Dp(58.0f);
    const float tileW = ui::Dp(app.charTile), tileH = tileW * 4.0f / 3.0f + ImGui::GetTextLineHeightWithSpacing() + ui::Dp(6.0f);
    const float gridGap = ui::Dp(6.0f);
    const int perRow = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + gridGap) / (tileW + gridGap)));
    int shown = 0;
    std::string removeKey;
    for (CharacterProfile& p : AllProfiles()) {
        if (!p.saved) continue;
        ++shown;
        ImGui::PushID(p.key.c_str());
        if (app.charView > 0) { // a tile: the picture (not cropped: they are full-body) and the name under it
            if ((shown - 1) % perRow != 0) ImGui::SameLine(0, gridGap);
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##c", ImVec2(tileW, tileH))) g_selKey = p.key;
            const bool hov = ImGui::IsItemHovered();
            const bool sel = p.key == g_selKey;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(pos, ImVec2(pos.x + tileW, pos.y + tileH), sel ? ui::WithAlpha(ui::kAccent, 0.25f) : (hov ? IM_COL32(255, 255, 255, 18) : IM_COL32(255, 255, 255, 8)), ui::Dp(6.0f));
            if (sel) dl->AddRect(pos, ImVec2(pos.x + tileW, pos.y + tileH), ui::WithAlpha(ui::kAccent, 0.9f), ui::Dp(6.0f), 0, ui::Dp(1.5f));
            const ImVec2 keep = ImGui::GetCursorScreenPos();
            ImGui::SetCursorScreenPos(ImVec2(pos.x + ui::Dp(3.0f), pos.y + ui::Dp(3.0f)));
            DrawThumb(app, p.image, ImVec2(tileW - ui::Dp(6.0f), tileW * 4.0f / 3.0f - ui::Dp(6.0f)), false);
            ImGui::SetCursorScreenPos(keep);
            const std::string name = CharName(p);
            const float ty = pos.y + tileW * 4.0f / 3.0f + ui::Dp(1.0f);
            ImGui::RenderTextEllipsis(dl, ImVec2(pos.x + ui::Dp(5.0f), ty), ImVec2(pos.x + tileW - ui::Dp(5.0f), ty + ImGui::GetTextLineHeight()),
                                      pos.x + tileW - ui::Dp(5.0f), pos.x + tileW - ui::Dp(5.0f), name.c_str(), nullptr, nullptr);
            if (hov && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", name.c_str());
            if (ImGui::BeginPopupContextItem("##cctx")) {
                if (ImGui::MenuItem("Remove from saved characters")) removeKey = p.key;
                ImGui::EndPopup();
            }
            ImGui::PopID();
            continue;
        }
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        if (ImGui::InvisibleButton("##c", ImVec2(w, row))) g_selKey = p.key;
        const bool hov = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const bool sel = p.key == g_selKey;
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + row), sel ? ui::WithAlpha(ui::kAccent, 0.25f) : (hov ? IM_COL32(255, 255, 255, 18) : IM_COL32(255, 255, 255, 8)), ui::Dp(6.0f));
        if (sel) dl->AddRect(pos, ImVec2(pos.x + w, pos.y + row), ui::WithAlpha(ui::kAccent, 0.9f), ui::Dp(6.0f), 0, ui::Dp(1.5f));
        const ImVec2 keep = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(pos.x + 4, pos.y + 4));
        DrawThumb(app, p.image, ImVec2(row - 8, row - 8), true);
        ImGui::SetCursorScreenPos(keep);
        dl->AddText(ImVec2(pos.x + row + 4, pos.y + 8), ImGui::GetColorU32(ImGuiCol_Text), CharName(p).c_str());
        const std::string sub = g_describing.count(p.key) ? "describing..." : (p.items.empty() ? "" : std::to_string(p.items.size()) + (p.items.size() == 1 ? " item" : " items"));
        dl->AddText(ImVec2(pos.x + row + 4, pos.y + 8 + ImGui::GetFontSize() + 3), ImGui::GetColorU32(ui::kTextDim), sub.c_str());
        if (ImGui::BeginPopupContextItem("##cctx")) {
            if (ImGui::MenuItem("Remove from saved characters")) removeKey = p.key;
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (!removeKey.empty()) {
        if (CharacterProfile* p = FindByKey(removeKey)) p->saved = false;
        SaveProfiles();
        if (g_selKey == removeKey) g_selKey.clear();
    }
    if (shown == 0) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("No characters yet. Add a picture of someone above (or save a character on the Vision board): you can then put them into any "
                            "picture.");
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild(); // (##charsscroll)
    std::wstring dropped;
    if (AcceptLibraryDrop(dropped)) AddSavedCharacter(app, dropped);
    ImGui::EndChild();

    } // (the list)

    // ---- the selected character
    ImGui::TableSetColumnIndex(1);
    ImGui::BeginChild("chars_detail", ImVec2(0, height), true);
    { // tests: MV_UI_TEST "charsel:N" picks the N-th saved character
        static const int selTest = UiTestNumber("charsel");
        static bool selDone = false;
        int k = 0;
        for (CharacterProfile& p : AllProfiles()) {
            if (selTest >= 0 && !selDone && p.saved && k++ == selTest) g_selKey = p.key, selDone = true;
        }
    }
    CharacterProfile* cur = g_selKey.empty() ? nullptr : FindByKey(g_selKey);
    if (cur && !cur->saved) cur = nullptr;
    if (!cur) {
        for (CharacterProfile& p : AllProfiles()) {
            if (p.saved) {
                cur = &p;
                g_selKey = p.key;
                break;
            }
        }
    }
    if (!cur) {
        ImGui::TextDisabled("Pick a character on the left.");
    } else {
        static LoadedImage img;
        static std::wstring imgPath;
        if (imgPath != cur->image) {
            ReleaseImage(img);
            img = LoadedImage{};
            imgPath = cur->image;
            std::string err;
            LoadImageTexture(app.Device(), cur->image, img, &err);
        }
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float boxH = std::min(avail.y * 0.45f, ui::Dp(360.0f));
        // its 360-degree view, when it has one (the picture box turns into a turntable), else the picture
        static bool showPicture = false;
        std::error_code orbitEc;
        const bool haveOrbit = !cur->orbit.empty() && fs::exists(cur->orbit, orbitEc);
        if (haveOrbit && !showPicture) {
            DrawOrbitView(app, cur->orbit, ImVec2(avail.x, boxH));
        } else {
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(avail.x, boxH));
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + boxH), IM_COL32(10, 12, 16, 255), ui::Dp(6.0f));
            if (img.srv && img.width > 0 && img.height > 0) {
                const float s = std::min(avail.x / img.width, boxH / img.height);
                const ImVec2 dim(img.width * s, img.height * s);
                const ImVec2 a(origin.x + (avail.x - dim.x) * 0.5f, origin.y + (boxH - dim.y) * 0.5f);
                dl->AddImage((ImTextureID)(intptr_t)img.srv, a, ImVec2(a.x + dim.x, a.y + dim.y));
            }
        }
        {
            const bool orbitBusy = CharacterOrbitPending(cur->image);
            if (haveOrbit) {
                if (ImGui::SmallButton(showPicture ? "360° view" : "Picture")) showPicture = !showPicture;
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip(showPicture ? "Turn them around (drag the view)" : "Show the picture itself");
                ImGui::SameLine();
            }
            if (DrawOrbitSetup(app)) {
                static const bool orbitTest = UiTestNumber("charorbit") >= 0; // tests: presses Make a 360 view once
                static bool orbitDone = false;
                if (orbitTest && !orbitDone && !orbitBusy) orbitDone = true, MakeCharacterOrbit(app, cur->image);
                ImGui::BeginDisabled(orbitBusy);
                if (ImGui::SmallButton(orbitBusy ? "Making the 360° view..." : haveOrbit ? "Make the 360° view again" : "Make a 360° view")) {
                    MakeCharacterOrbit(app, cur->image);
                    showPicture = false;
                }
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("H3 moves the camera once around them, frozen in place (about a minute on a 24 GB card); then drag the picture "
                                      "to turn them and look at their back and sides. Works best on a full-body picture on a plain backdrop.");
            }
            if (haveOrbit) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Open the video")) ShellExecuteW(nullptr, L"open", cur->orbit.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
        // poses & outfits (library_ui.cpp): a new picture of them in one, which goes straight on into a video or a swap
        {
            struct LibResult {
                std::string key;             // the character it was made from
                std::wstring made;
                std::string what, why;
                bool busy = false;
            };
            static LibResult res;
            const std::wstring person = cur->image;
            const std::string key = cur->key;
            auto start = [&](int kind) {
                OpenLibraryPicker(app, kind, kind == 0 ? "Put " + CharName(*cur) + " in a pose" : "Dress " + CharName(*cur) + " in an outfit",
                                  [person, key](AppState& a, const LibItem& it) {
                                      res = LibResult{key, std::wstring(), (it.kind == 0 ? "in pose " : "in outfit ") + it.id, std::string(), true};
                                      LibraryEdit(a, person, it, [key](AppState& a2, const std::wstring& made, const std::string& why) {
                                          if (res.key != key) return;
                                          res.made = made, res.why = why, res.busy = false;
                                          if (!made.empty()) Toast(ToastKind::Success, "A new picture of them", res.what, made, [](AppState& s) { s.requestTab = 5; }, "libedit");
                                          (void)a2;
                                      });
                                  });
            };
            ImGui::BeginDisabled(res.busy || !(LibraryHas(0) || LibraryHas(1)));
            if (ImGui::SmallButton("Put them in a pose...")) start(0);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(LibraryHas(0) ? "Pick a pose (a drawn mannequin): the writer puts it into words and FLUX 3 makes a new full-body picture of them in it"
                                                : "Needs your copy of the pose gallery in the Poses & outfits folder (the picker's top line)");
            ImGui::SameLine();
            if (ImGui::SmallButton("Dress them in an outfit...")) start(1);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(LibraryHas(1) ? "Pick an outfit: FLUX 3 makes a new full-body picture of them wearing it (its picture and its keywords)"
                                                : "Needs your copy of the outfit gallery in the Poses & outfits folder");
            ImGui::EndDisabled();
            if (!LibraryHas(0) && !LibraryHas(1)) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Set the folder...")) OpenLibraryPicker(app, 0, "Poses & outfits", nullptr);
            }
            if (res.key == key && (res.busy || !res.made.empty() || !res.why.empty())) {
                if (res.busy) {
                    ImGui::TextColored(ui::kAccentHover, "Making them %s... (the writer, then FLUX 3)", res.what.c_str());
                } else if (!res.made.empty()) {
                    const AppState::LibThumb lt = app.LibraryThumb(res.made);
                    const float th = ui::Dp(120.0f);
                    if (lt.srv && lt.h > 0) {
                        ImGui::Image((ImTextureID)(intptr_t)lt.srv, ImVec2(th * lt.w / lt.h, th));
                        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) app.OpenFile(res.made);
                        ImGui::SameLine();
                    }
                    ImGui::BeginGroup();
                    ImGui::TextDisabled("Them %s:", res.what.c_str());
                    if (ImGui::SmallButton("Animate it")) OpenVideoPage(app, res.made, std::string());
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("The Video page with this picture as the first frame");
                    if (ImGui::SmallButton("Star in a video")) StarInVideo(app, res.made);
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Video > Star in a video with this picture as the one who goes in (a full-body picture swaps legs too)");
                    if (ImGui::SmallButton("Save as a new character")) AddSavedCharacter(app, res.made);
                    if (ImGui::SmallButton("Close##libres")) res = LibResult{};
                    ImGui::EndGroup();
                } else {
                    ImGui::TextColored(ui::kWarn, "Could not make it: %s", res.why.c_str());
                }
            }
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Name");
        ImGui::SameLine();
        if (InputLine("##charname", "Name", cur->name, -FLT_MIN)) { /* saved when the box is left */ }
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveProfiles();
        ImGui::TextDisabled("Description (who they are; used when writing prompts)");
        if (InputBox("##chardesc", cur->description, ui::Dp(110.0f))) { /* saved when left */ }
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveProfiles();
        const bool busy = g_describing.count(cur->key) > 0;
        ImGui::BeginDisabled(busy);
        if (ImGui::Button(busy ? "Describing..." : "Describe with the vision model")) DescribeCharacter(app, *cur);
        ImGui::EndDisabled();
        const std::string inv = InventoryText(*cur);
        if (!inv.empty()) {
            ImGui::TextDisabled("Inventory (set on the Vision board):");
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(inv.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::Spacing();
        ui::SectionHeading("BACKSTORY");
        {
            const bool writing = CharacterStoryPending(cur->image);
            if (cur->backstory.empty()) {
                ImGui::PushTextWrapPos(0);
                ImGui::TextDisabled(writing ? "The prompt writer is writing who they are..." : "Who they are: age, how they act, walk and talk, what they like, childhood, school, "
                                                                                                "university, work or none, family, what they want and fear.");
                ImGui::PopTextWrapPos();
            } else { // read as wrapped text (a text box can't wrap); Edit opens the box
                static std::wstring editing;
                if (editing == cur->image) {
                    if (InputBox("##backstory", cur->backstory, ui::Dp(220.0f))) { /* saved when left */ }
                    if (ImGui::IsItemDeactivatedAfterEdit()) SaveProfiles();
                } else {
                    ImGui::BeginChild("##backstoryview", ImVec2(0, ui::Dp(220.0f)), ImGuiChildFlags_FrameStyle);
                    ImGui::PushTextWrapPos(0);
                    ImGui::TextUnformatted(cur->backstory.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::EndChild();
                }
                if (ImGui::SmallButton(editing == cur->image ? "Done" : "Edit")) {
                    if (editing == cur->image) SaveProfiles();
                    editing = editing == cur->image ? std::wstring() : cur->image;
                }
            }
            ImGui::BeginDisabled(writing);
            if (ImGui::Button(writing ? "Writing..." : cur->backstory.empty() ? "Write their backstory" : "Write it again")) QueueCharacterStory(app, cur->image, false);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("The prompt writer looks at their picture and writes who they are%s", CharacterNameIsPlaceholder(*cur) ? " (and gives them a real name)" : "");
            ImGui::SameLine();
            ImGui::BeginDisabled(writing);
            if (ImGui::Button("New name")) QueueCharacterStory(app, cur->image, true);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("The writer invents a new name that fits them (and writes their backstory again)");
        }
        ImGui::Spacing();
        ui::SectionHeading("CHARACTER SHEET");
        if (!cur->sheet.empty()) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled(CharacterSheetUseful(cur->image) ? "Its back looks unlike its front, so this sheet is used for its Recreate videos."
                                                                  : "Its picture already shows how it looks from every side, so videos and edits use the picture "
                                                                    "(tested: there the sheet changed nothing or made it worse). The sheet is here to look at.");
            ImGui::PopTextWrapPos();
            const float w = ImGui::GetContentRegionAvail().x;
            DrawThumb(app, cur->sheet, ImVec2(w, w * 1504.0f / 2272.0f), false);
            if (ImGui::IsItemClicked()) ShellExecuteW(nullptr, L"open", cur->sheet.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Click: open it full size");
        } else {
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("Four expressions, then front, side and back views, drawn from the picture. Only needed for characters that look very "
                                "different from behind (a cape, armour, wings): then their Recreate videos use it. Otherwise the picture is better.");
            ImGui::PopTextWrapPos();
        }
        const bool sheetBusy = CharacterSheetPending(cur->image);
        ImGui::BeginDisabled(sheetBusy);
        if (ImGui::Button(sheetBusy ? "Making the sheet..." : cur->sheet.empty() ? "Make a character sheet" : "Make it again")) MakeCharacterSheetFor(app, cur->image);
        ImGui::EndDisabled();
        if (!cur->sheet.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("Don't use it")) cur->sheet.clear(), SaveProfiles();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Forget the sheet for this character (the file stays on disk)");
        }
        ImGui::Spacing();
        ui::SectionHeading("USE THEM");
        if (ImGui::Button("Animate")) OpenVideoPage(app, cur->image, cur->description);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("A video with sound of them (the Video page writes what happens)");
        ImGui::SameLine();
        if (ImGui::Button("Star in a video")) StarInVideo(app, cur->image);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Put them into a clip: a music video, a dance... (the Video page)");
        ImGui::SameLine();
        if (ImGui::Button("Edit with Qwen...")) OpenPictureEditor(app, {cur->image});
        if (ImGui::Button("Add to Clef mode's cast")) AddToClefCast(app, {cur->image});
        ImGui::SameLine();
        if (ImGui::Button("Put on the Vision board")) AddImagesToBoard(app, {cur->image}, 0);
        ImGui::SameLine();
        if (ImGui::Button("Show in Explorer")) {
            const std::wstring args = L"/select,\"" + cur->image + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        }
    }
    ImGui::EndChild();

    // ---- swap
    ImGui::TableSetColumnIndex(2);
    if (foldFun) {
        if (ui::FoldStrip("##stripfun", "Fun with characters", strip, height)) foldFun = false;
        ImGui::EndTable();
        return;
    }
    ImGui::BeginChild("chars_swap", ImVec2(0, height), true);
    if (ui::FoldButton("##foldfun", "Fold this column away")) foldFun = true;
    ImGui::PushFont(ui::BoldFont());
    ImGui::TextUnformatted("Fun with characters");
    ImGui::PopFont();
    CharacterProfile* who = g_selKey.empty() ? nullptr : FindByKey(g_selKey);
    if (who && !who->saved) who = nullptr;
    if (who) {
        ImGui::TextDisabled("Using:");
        ImGui::SameLine();
        ImGui::TextColored(ui::kAccent, "%s", CharName(*who).c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(pick another on the left)");
    } else {
        ImGui::TextColored(ui::kWarn, "Pick a saved character on the left first.");
    }
    const bool free = who && !g_swap.writing;
    const bool ready = who && who->saved && !g_swap.scene.empty() && !g_swap.writing;
    if (ImGui::BeginTabBar("##funmodes")) {
    if (ImGui::BeginTabItem("Swap into a picture")) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Pick a picture and who in it to replace: the scene stays the same (place, pose, action, mood) but the chosen character takes their "
                        "place. You get a new prompt to copy or send to ComfyUI.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::TextUnformatted("1. The picture (its scenario is kept)");
    {
        const ImVec2 start = ImGui::GetCursorScreenPos();
        if (g_swap.scene.empty()) {
            ImGui::Dummy(ImVec2(ui::Dp(170.0f), ui::Dp(120.0f)));
            ImGui::GetWindowDrawList()->AddRect(start, ImVec2(start.x + ui::Dp(170.0f), start.y + ui::Dp(120.0f)), IM_COL32(255, 255, 255, 50), ui::Dp(6.0f));
            ImGui::GetWindowDrawList()->AddText(ImVec2(start.x + ui::Dp(14.0f), start.y + ui::Dp(52.0f)), IM_COL32(200, 200, 205, 160), "drop a picture here");
        } else {
            DrawThumb(app, g_swap.scene, ImVec2(ui::Dp(170.0f), ui::Dp(170.0f)), false);
        }
        std::wstring dropped;
        if (AcceptLibraryDrop(dropped)) PickScene(dropped);
        ImGui::SameLine();
        ImGui::BeginGroup();
        if (ImGui::Button("From the Vision library...")) ImGui::OpenPopup("##swappicker");
        if (ImGui::Button("Choose a file...")) {
            const std::wstring f = OpenImageFileDialog(GetActiveWindow());
            if (!f.empty()) PickScene(f);
        }
        if (ImGui::Button("The gallery's picture")) {
            if (app.selectedIndex >= 0 && app.selectedIndex < static_cast<int>(app.entries.size())) PickScene(app.entries[app.selectedIndex].path);
            else app.SetStatus("Open a picture in the gallery first");
        }
        if (!g_swap.scene.empty()) ImGui::TextDisabled("%s", ToUtf8(fs::path(g_swap.scene).filename().wstring()).c_str());
        ImGui::EndGroup();
        if (ImGui::BeginPopup("##swappicker")) {
            DrawScenePicker(app);
            ImGui::EndPopup();
        }
    }
    if (!g_swap.scene.empty()) {
        ImGui::TextDisabled("Its prompt (the scenario). Edit it, or leave it empty and the model looks at the picture:");
        InputBox("##swapprompt", g_swap.scenePrompt, ui::Dp(90.0f));
    }
    ImGui::Spacing();
    ImGui::TextUnformatted("2. Who in the picture is replaced (3. is the character picked on the left)");
    InputLine("##swapwho", "e.g. the man on the right, person 2", g_swap.who, -FLT_MIN);
    {
        std::string quick;
        if (PresetRow({"the main person", "person 1", "person 2", "the man", "the woman", "the character on the left", "the character on the right"}, quick)) g_swap.who = quick;
    }
    ImGui::Checkbox("Keep the replaced person's outfit", &g_swap.keepOutfit);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("On: the character wears what the replaced person wears.\nOff: the character keeps their own clothes.");

    ImGui::Spacing();
    ImGui::BeginDisabled(!ready);
    if (ui::PrimaryButton(g_swap.writing ? "Writing..." : "Swap them in", ImVec2(ui::Dp(150.0f), 0))) StartSwap(app, *who, false);
    ImGui::SameLine();
    if (ImGui::Button("Swap and send to ComfyUI")) StartSwap(app, *who, true);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!ready);
    if (ImGui::Button("Swap the picture itself (Qwen Edit)...") && who) {
        std::string target = Trim(g_swap.who);
        if (target.empty()) target = "the main person";
        OpenPictureEditor(app, {g_swap.scene, who->image},
                          "Replace " + target + " in <image1> with the character shown in <image2>. Keep everything else in <image1> exactly the same: the scene, "
                          "pose, camera angle and lighting.",
                          CharName(*who) + " in " + ToUtf8(fs::path(g_swap.scene).stem().wstring()));
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Edits the picture itself with Qwen Image 2.1 Edit (no prompt): <image1> is the scene, <image2> the character");
    ImGui::EndDisabled();
    if (g_swap.scene.empty()) ImGui::TextDisabled("Choose a picture first.");
    ImGui::EndTabItem();
    }

    // ---- fun: transform
    if (ImGui::BeginTabItem("Transform")) {
        static std::string custom;
        std::string picked;
        ImGui::TextDisabled("Keep who they are, change how they look.");
        if (PresetRow({"as a child", "as an old person", "as a chibi", "as an anime character", "as a realistic photo", "as pixel art", "as a plush toy", "as a zombie",
                       "as a superhero", "as a vampire", "as a robot", "as a marble statue", "as a Lego minifigure", "as a 3D cartoon", "as an oil painting", "as a comic book hero"}, picked))
            if (free) StartRemix(app, *who, "Show " + CharName(*who) + " " + picked + ". Keep them recognisable: the same face shape, hair colour and signature traits.", nullptr, "transform", false);
        ImGui::Spacing();
        InputLine("##transformcustom", "or your own: as a 1920s jazz singer, made of candy...", custom, ui::Dp(380.0f));
        ImGui::SameLine();
        ImGui::BeginDisabled(!free || Trim(custom).empty());
        if (ImGui::Button("Write it##transform")) StartRemix(app, *who, "Show " + CharName(*who) + " " + Trim(custom) + ". Keep them recognisable.", nullptr, "transform", false);
        ImGui::EndDisabled();
        ImGui::EndTabItem();
    }

    // ---- fun: cast in a world
    if (ImGui::BeginTabItem("Cast in a world")) {
        static std::string world, action;
        std::string picked;
        ImGui::TextDisabled("Drop them into another genre, with an outfit and a scene that fit.");
        if (PresetRow({"cyberpunk city", "medieval fantasy", "space opera", "wild west", "film noir", "post-apocalypse", "pirate ship", "1920s", "underwater kingdom", "steampunk",
                       "high school", "superhero city", "samurai Japan", "haunted mansion", "desert ruins", "snowy mountains", "royal court", "futuristic racing"}, picked))
            world = picked;
        InputLine("##world", "the world", world, ui::Dp(260.0f));
        ImGui::SameLine();
        InputLine("##worldaction", "doing what? (blank = something fitting)", action, ui::Dp(300.0f));
        ImGui::BeginDisabled(!free || Trim(world).empty());
        if (ui::PrimaryButton("Write the prompt##world", ImVec2(0, 0)))
            StartRemix(app, *who, "Place " + CharName(*who) + " in this world: " + Trim(world) + ", " + (Trim(action).empty() ? std::string("doing something that fits the world") : Trim(action)) +
                                      ". Give them clothes and gear that belong there but keep their face, hair and build.", nullptr, "world", false);
        ImGui::EndDisabled();
        ImGui::EndTabItem();
    }

    // ---- fun: mashup
    if (ImGui::BeginTabItem("Mashup")) {
        static std::string otherKey;
        static int mode = 0;
        static std::string scene;
        ImGui::TextDisabled("Two saved characters: blend them into one, or put them together in a scene.");
        CharacterProfile* other = otherKey.empty() ? nullptr : FindByKey(otherKey);
        if (other && (!other->saved || other == who)) other = nullptr;
        ImGui::SetNextItemWidth(ui::Dp(260.0f));
        if (ImGui::BeginCombo("The second character", other ? CharName(*other).c_str() : "Choose...")) {
            for (CharacterProfile& p : AllProfiles()) {
                if (!p.saved || &p == who) continue;
                if (ImGui::Selectable((CharName(p) + "##" + p.key).c_str(), p.key == otherKey)) otherKey = p.key;
            }
            ImGui::EndCombo();
        }
        ImGui::RadioButton("Blend them into ONE new character (a hybrid)", &mode, 0);
        ImGui::RadioButton("Both together in a scene", &mode, 1);
        if (mode == 1) InputLine("##duoscene", "the scene: having coffee in a rainy city, fighting back to back...", scene, -FLT_MIN);
        ImGui::BeginDisabled(!free || !other || (mode == 1 && Trim(scene).empty()));
        if (ui::PrimaryButton("Write the prompt##mash", ImVec2(0, 0))) {
            if (mode == 0)
                StartRemix(app, *who, "Invent ONE new character that is a believable blend of " + CharName(*who) + " (picture 1) and " + CharName(*other) +
                                          " (picture 2): mix their faces, hair, build and clothing style so both can be recognised in it.", other, "mashup", false);
            else
                StartRemix(app, *who, "Show " + CharName(*who) + " (picture 1) and " + CharName(*other) + " (picture 2) together in this scene: " + Trim(scene) +
                                          ". Both keep their own face, hair, build and clothes.", other, "duo", false);
        }
        ImGui::EndDisabled();
        ImGui::EndTabItem();
    }

    // ---- fun: surprise me
    if (ImGui::BeginTabItem("Surprise me")) {
        static std::string lastRoll;
        ImGui::TextDisabled("A random place, activity, mood and look for your character. Press again for another.");
        auto roll = [&]() {
            const std::string setting = Pick({"a neon-lit night market", "an abandoned lighthouse", "a floating island in the clouds", "a crowded train platform at dawn",
                                              "a giant library with endless shelves", "a rainy rooftop", "a sunflower field at golden hour", "an ancient temple in the jungle",
                                              "a retro arcade", "a frozen lake under the northern lights", "a quiet seaside cafe", "the bridge of a starship",
                                              "a medieval market square", "a desert highway at sunset", "a cozy cabin during a snowstorm", "a carnival after dark"});
            const std::string activity = Pick({"laughing with friends", "running from something unseen", "reading a mysterious letter", "dancing alone", "cooking a huge meal",
                                               "fixing a broken machine", "sleeping on a bench", "riding a bicycle", "arguing with a talking animal", "holding a glowing object",
                                               "taking a selfie", "painting a mural", "waiting for someone", "carrying far too many bags", "playing an instrument",
                                               "staring at the horizon"});
            const std::string mood = Pick({"warm and nostalgic", "tense and cinematic", "silly and cheerful", "dreamlike", "quiet and lonely", "epic", "mysterious", "romantic"});
            const std::string look = Pick({"cinematic photograph", "watercolour illustration", "anime key visual", "oil painting", "retro 80s film still", "comic book panel",
                                           "Studio-style hand-drawn animation", "moody concept art"});
            lastRoll = setting + ", " + activity + ", " + mood + ", " + look;
        };
        ImGui::BeginDisabled(!free);
        bool go = false, goSend = false;
        if (ui::PrimaryButton("Surprise me!", ImVec2(ui::Dp(150.0f), 0))) go = true;
        ImGui::SameLine();
        if (ImGui::Button("Surprise me and send to ComfyUI")) goSend = true;
        ImGui::EndDisabled();
        if (go || goSend) {
            roll();
            StartRemix(app, *who, "Show " + CharName(*who) + " in this situation: " + lastRoll + ". Keep their face, hair and build; choose clothes that suit the situation.", nullptr,
                       "surprise", goSend);
        }
        if (!lastRoll.empty()) ImGui::TextDisabled("Last roll: %s", lastRoll.c_str());
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
    }

    if (!g_swap.result.empty() || g_swap.writing) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextUnformatted(g_swap.writing ? "The model is writing the new prompt..." : ("New prompt: " + g_swap.resultLabel).c_str());
        if (!g_swap.writing) {
            WrapEdit("##swapresult", g_swap.result, ui::Dp(170.0f));
            if (ImGui::Button("Copy")) {
                ImGui::SetClipboardText(g_swap.result.c_str());
                app.SetStatus("Copied the swapped prompt");
            }
            ImGui::SameLine();
            static int times = 1;
            if (ui::PrimaryButton("Send to ComfyUI", ImVec2(0, 0))) SendToComfy(app, g_swap.result, std::string(), g_swap.sendPicture, g_swap.resultLabel, true, 0.0, times);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ui::Dp(100.0f));
            ImGui::InputInt("times##swaptimes", &times, 1, 5);
            times = std::clamp(times, 1, 100);
            ImGui::SameLine();
            if (ImGui::Button("Save with the picture")) SaveSwap(app);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Keeps it as a styled prompt next to the picture in the Vision library, so you can send it later");
            ImGui::SameLine();
            ImGui::BeginDisabled(!who || g_swap.writing || g_swap.scene.empty() || g_swap.saveStyle.rfind("swap-", 0) != 0);
            if (ImGui::Button("Write another version") && who) StartSwap(app, *who, false);
            ImGui::EndDisabled();
        }
    }
    ImGui::EndChild();
    ImGui::EndTable();
}
