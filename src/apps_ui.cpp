// Edits > Apps: one-click picture recipes in categories, each a fixed Qwen Image 2.1 Edit instruction (with a choice or a few words of yours)
// - relight, a new angle, expand, outfit swap, headshot, plushie... Asked for 2026-10-23, from the "Open Sourcing Higgsfield" map (its Apps
// hub). A card shows the last picture you made with it; the results also go to Picture edits as usual.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <windows.h>
#include <commdlg.h>

#include "comfy.h"
#include "imgui.h"
#include "json.hpp"
#include "ui_extra.h"
#include "ui_theme.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

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

// One app. {X} in the instruction is your choice (choices: "Label=text|Label=text") or your words (ask). canvas: 0 the picture itself is
// edited (its frame kept), 1 landscape, 2 square, 3 portrait (a new canvas, the picture a reference). twoPictures: a second picture is needed.
// canvasChoice: the choice is the canvas (Expand). shots: one edit per choice, all at once (Shots).
struct App {
    const char* name;
    const char* category;
    const char* about;
    const char* instruction;
    const char* choices = nullptr;
    const char* ask = nullptr; // the label of a few words of yours
    bool twoPictures = false;
    bool canvasChoice = false;
    bool shots = false;
};

const char* kCategories[] = {"All", "Enhance & style", "Professional", "Face & identity", "Ads & products", "Games & characters", "Extras"};

const App kApps[] = {
    // ---- enhance and style
    {"Relight", "Enhance & style", "New light on the same picture: golden hour, studio, neon...",
     "Relight the picture with {X}. Keep the people, their faces, poses and clothes and the whole scene exactly the same; only the light, the shadows and the colour of the light change.",
     "Golden hour=warm low golden-hour sunlight from the side and long soft shadows|Studio=soft, even studio softbox light and clean soft shadows|Neon night=pink and blue neon light from the sides in a dark night|Candlelight=warm flickering candlelight and deep shadows|Overcast=soft overcast daylight with no harsh shadows|Rim light=a strong backlight that outlines the subject, the front darker|Moonlight=cool blue moonlight at night"},
    {"Skin enhancer", "Enhance & style", "A professional retouch: even skin, real texture kept",
     "Retouch the skin like a professional portrait retoucher: even out blemishes, redness and shine, keep natural pores and skin texture. Keep the face shape, features, expression, age, hair, clothes and background exactly the same."},
    {"Style snap", "Enhance & style", "The same picture redrawn in another style",
     "Redraw the whole picture as {X}, keeping the same composition, people, poses, expressions and colours where they fit.",
     "Anime=a clean modern anime illustration|3D animated=a 3D animated film still with soft lighting|Watercolour=a loose watercolour painting|Oil painting=a classical oil painting with visible brushstrokes|Comic book=a bold comic book panel with ink lines and halftone|Claymation=a claymation still, everything made of plasticine|Pencil sketch=a detailed pencil sketch on paper|Cyberpunk=a neon cyberpunk digital painting"},
    {"Outfit swap", "Enhance & style", "Dress them in something else (say what)",
     "Change the person's outfit to {X}. Keep their face, hair, body, pose, the light and the background exactly the same.", nullptr, "The new outfit"},
    {"Outfit from a picture", "Enhance & style", "The outfit worn in a second picture",
     "Dress the person in the first picture in the outfit worn in the second picture. Keep the first picture's person - face, hair, body, pose - and its background and light exactly the same.",
     nullptr, nullptr, true},
    {"AI stylist", "Enhance & style", "A new hairstyle and makeup (say which)",
     "Give the person {X}. Keep their face, identity, clothes, pose and the background the same.", nullptr, "Hair and makeup"},
    {"Colorize", "Enhance & style", "A black and white photo in natural colour",
     "Colorize this black and white photo with natural, realistic colours: skin, clothes, sky and surroundings as they would really be; keep every detail the same."},
    // ---- professional
    {"Expand", "Professional", "More of the scene around it, in a new shape",
     "Show more of the scene around the first picture: extend it naturally beyond its edges in the same style, light and perspective, keeping everything already in it exactly the same.",
     "Wider (16:9)=1|Square=2|Taller (9:16)=3", nullptr, false, true},
    {"New angle", "Professional", "The same scene from another camera angle",
     "Show the same scene with the same people {X}. Keep everyone's faces, hair, clothes and the place the same.",
     "From the side=seen from the side|From above=seen from high above, a bird's-eye view|Low angle=seen from a low angle looking up|From behind=seen from behind|Close-up=as a close-up of the face|Wide shot=as a wide shot showing the whole place|Over the shoulder=seen over the shoulder of the person"},
    {"Shots", "Professional", "Three framings at once: close-up, medium, wide",
     "Show the same scene with the same people {X}. Keep everyone's faces, hair, clothes and the place the same.",
     "Close-up=as a close-up of the face|Medium=as a medium shot from the waist up|Wide=as a wide shot showing the whole place", nullptr, false, false, true},
    {"Product shot", "Professional", "A studio product photo of the thing in it",
     "Turn this into a professional studio product photo: the product alone on a clean, seamless {X} background, soft studio lighting, crisp detail and a soft shadow beneath. Keep the product exactly the same.",
     "White=white|Pastel=soft pastel|Black=black|Marble=white marble|Wood=warm wooden"},
    // ---- face and identity
    {"Headshot", "Face & identity", "A professional headshot of the person",
     "Make a professional headshot of this person: {X}, shoulders up, looking at the camera with a friendly, confident expression. Keep their face and identity exactly the same.",
     "Business=business attire, soft studio light and a plain grey background|Creative=smart casual clothes, warm natural window light and a blurred office behind|Outdoor=smart casual clothes, outdoors in soft daylight with a blurred park behind"},
    {"Recast", "Face & identity", "The person from a second picture in their place",
     "Replace the person in the first picture with the person from the second picture, in the same pose, clothes, light and place. Keep everything else in the first picture exactly the same.",
     nullptr, nullptr, true},
    {"Age", "Face & identity", "Younger or older, the same person",
     "Make the person look {X}, keeping their identity, expression, clothes, pose and the background.",
     "Younger=clearly about fifteen years younger, with smoother skin|Older=clearly about twenty-five years older, with wrinkles, greying hair and an older face|As a child=like themselves as a child of about eight|Elderly=like themselves at about eighty, with white hair and deep wrinkles"},
    {"Expression", "Face & identity", "A new expression on the same face",
     "Change the person's expression to {X}. Keep everything else exactly the same.",
     "Big smile=a big, warm smile|Laughing=laughing out loud|Surprised=surprised, eyes wide|Angry=angry|Sad=sad, close to tears|Wink=a playful wink"},
    // ---- ads and products
    {"Billboard ad", "Ads & products", "The picture as a huge billboard in a city",
     "Show the first picture as a huge billboard advertisement on a building in a busy city street {X}, seen from the street with people walking by. The picture on the billboard stays exactly the same.",
     "Day=on a sunny day|Dusk=at dusk with the city lights coming on|Night=at night, the billboard lit up among neon signs"},
    {"Signboard", "Ads & products", "A sign in the scene with your words",
     "Put a sign in the scene that reads exactly: \"{X}\", in a style that fits the place (painted, neon, a shop sign...). Keep everything else the same.", nullptr, "The sign's words"},
    {"On a T-shirt", "Ads & products", "The picture printed on a T-shirt, worn",
     "Show the first picture printed on the front of a T-shirt worn by a model in a bright street-style photo; the print stays exactly the same."},
    // ---- games and characters
    {"Plushie", "Games & characters", "The subject as a cute plush toy",
     "Turn the main subject into a cute, soft plush toy with stitched seams and fuzzy fabric, sitting on a bed in cosy light; keep its colours and features recognisable."},
    {"Action figure", "Games & characters", "A collectible figure in its packaging",
     "Turn the person into a collectible action figure in a blister package with a few accessories beside it, a toy store product photo; keep their face, hair and clothes recognisable."},
    {"Game character", "Games & characters", "The person in a video game",
     "Show the person as a character in {X}, keeping their face, hair and clothes recognisable.",
     "Open world=a modern 3D open-world video game screenshot|Retro 16-bit=a retro 16-bit pixel art video game|Fighter=a fighting game character select screen|Cartoon game=a colourful cartoon platform game"},
    {"Sticker", "Games & characters", "A die-cut sticker",
     "Turn the main subject into a die-cut sticker: flat cartoon style, a thick white border around it, on a plain light grey background."},
    // ---- extras
    {"Background remover", "Extras", "Only the subject, on plain white",
     "Remove the background: keep only the main subject, unchanged, on a plain pure white background."},
    {"New background", "Extras", "The subject somewhere else (say where)",
     "Put the main subject in {X}, matching the light and perspective. Keep the subject exactly the same.", nullptr, "The new place"},
    {"Remove something", "Extras", "Take something out (say what)",
     "Remove {X} from the picture and fill the space in naturally. Keep everything else exactly the same.", nullptr, "What to remove"},
    {"Day to night", "Extras", "The same place at another time",
     "Show the same scene {X}, with the light, sky and colours that go with it. Keep everything in it the same.",
     "At night=at night with lights on|At sunset=at sunset|In winter=in winter with snow|In autumn=in autumn with orange leaves|In the rain=in heavy rain with wet reflections"},
};
constexpr int kAppCount = static_cast<int>(sizeof(kApps) / sizeof(kApps[0]));

struct Choice {
    std::string label, text;
};
std::vector<Choice> ChoicesOf(const App& a) {
    std::vector<Choice> out;
    if (!a.choices) return out;
    const std::string s = a.choices;
    size_t at = 0;
    while (at <= s.size()) {
        const size_t bar = s.find('|', at);
        const std::string item = s.substr(at, bar == std::string::npos ? std::string::npos : bar - at);
        const size_t eq = item.find('=');
        out.push_back({item.substr(0, eq), eq == std::string::npos ? item : item.substr(eq + 1)});
        if (bar == std::string::npos) break;
        at = bar + 1;
    }
    return out;
}

struct Page {
    bool loaded = false;
    int category = 0;
    int selected = 0;
    std::wstring first, second;
    int choice = 0;
    char words[200] = {};
    int times = 1;
    std::map<std::string, std::wstring> lastMade; // by app: its newest result (the card's picture)
    std::vector<std::pair<int, std::string>> waiting; // activity -> app
    std::vector<std::pair<std::string, std::wstring>> made; // this session, newest last (app, picture)
} g_a;

std::wstring SavePath() {
    wchar_t buf[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    return std::wstring(buf) + L"\\Try2Create\\apps.json";
}

void Load() {
    if (g_a.loaded) return;
    g_a.loaded = true;
    std::ifstream f(SavePath(), std::ios::binary);
    const json j = f ? json::parse(f, nullptr, false) : json();
    if (!j.is_object() || !j.contains("lastMade") || !j["lastMade"].is_object()) return;
    std::error_code ec;
    for (auto it = j["lastMade"].begin(); it != j["lastMade"].end(); ++it)
        if (it.value().is_string() && fs::exists(W(it.value().get<std::string>()), ec)) g_a.lastMade[it.key()] = W(it.value().get<std::string>());
}

void Save() {
    json m = json::object();
    for (const auto& [k, v] : g_a.lastMade) m[k] = U8(v);
    const std::string text = json({{"lastMade", m}}).dump(1, ' ', false, json::error_handler_t::replace);
    std::ofstream(SavePath(), std::ios::binary | std::ios::trunc) << text;
}

void Pump(AppState& app) {
    if (g_a.waiting.empty() || !app.comfySvc) return;
    const std::vector<ComfyActivity> acts = app.comfySvc->Activity();
    for (size_t i = 0; i < g_a.waiting.size();) {
        bool done = false;
        for (const ComfyActivity& a : acts) {
            if (a.id != g_a.waiting[i].first) continue;
            if (a.state == ComfyState::Done) {
                for (const ComfyResult& r : a.files) {
                    if (r.localPath.empty() || r.video || r.audio) continue;
                    g_a.lastMade[g_a.waiting[i].second] = r.localPath;
                    g_a.made.push_back({g_a.waiting[i].second, r.localPath});
                }
                Save();
                done = true;
            } else if (a.state == ComfyState::Failed) {
                done = true;
            }
        }
        if (done) g_a.waiting.erase(g_a.waiting.begin() + i);
        else ++i;
    }
}

std::wstring PickPicture() {
    wchar_t buf[MAX_PATH * 2] = {};
    OPENFILENAMEW ofn = {sizeof(ofn)};
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = L"Pictures\0*.png;*.jpg;*.jpeg;*.webp;*.bmp\0All files\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.lpstrTitle = L"A picture";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&ofn) ? std::wstring(buf) : std::wstring();
}

// A picture (blurred when NSFW blur says so) fitted into w x h; true when clicked
bool Picture(AppState& app, const std::wstring& path, float w, float h, const char* id) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool click = ImGui::InvisibleButton(id, ImVec2(w, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(255, 255, 255, 12), ui::Dp(6.0f));
    if (!path.empty()) {
        const AppState::LibThumb lt = app.LibraryThumb(path);
        if (lt.srv && lt.w > 0 && lt.h > 0) {
            const float s = std::max(w / lt.w, h / lt.h); // (filled: cropped to the card)
            const float uw = std::min(1.0f, w / (lt.w * s)), vh = std::min(1.0f, h / (lt.h * s));
            dl->AddImageRounded((ImTextureID)(intptr_t)lt.srv, p, ImVec2(p.x + w, p.y + h), ImVec2((1 - uw) * 0.5f, (1 - vh) * 0.5f), ImVec2((1 + uw) * 0.5f, (1 + vh) * 0.5f),
                                IM_COL32_WHITE, ui::Dp(6.0f));
        }
    }
    return click;
}

// One picture slot: its picture, Gallery's / Newest / File...
void Slot(AppState& app, const char* label, std::wstring& path, const char* id) {
    ImGui::BeginGroup();
    ImGui::PushID(id);
    ImGui::TextDisabled("%s", label);
    const float s = ui::Dp(150.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    Picture(app, path, s, s * 1.2f, "##pic");
    if (path.empty()) {
        const char* t = "none yet";
        const ImVec2 ts = ImGui::CalcTextSize(t);
        ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + (s - ts.x) * 0.5f, p.y + (s * 1.2f - ts.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled), t);
    } else if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        ImGui::SetTooltip("%s", U8(fs::path(path).filename().wstring()).c_str());
    }
    const bool sel = app.selectedIndex >= 0 && app.selectedIndex < static_cast<int>(app.entries.size());
    ImGui::BeginDisabled(!sel);
    if (ImGui::SmallButton("Gallery's")) path = app.entries[app.selectedIndex].path;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip(sel ? "The picture selected in the gallery" : "Select a picture in the gallery first");
    ImGui::SameLine();
    if (ImGui::SmallButton("Newest")) {
        std::wstring newest;
        if (app.comfySvc)
            for (const ComfyResult& r : app.comfySvc->Results())
                if (!r.localPath.empty() && !r.audio && !r.video) newest = r.localPath;
        if (!newest.empty()) path = newest;
        else app.SetStatus("Nothing made yet this session");
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("The newest picture ComfyUI made this session");
    ImGui::SameLine();
    if (ImGui::SmallButton("File...")) {
        const std::wstring f = PickPicture();
        if (!f.empty()) path = f;
    }
    ImGui::PopID();
    ImGui::EndGroup();
}

void Make(AppState& app, const App& a) {
    const std::vector<Choice> choices = ChoicesOf(a);
    std::vector<std::wstring> pics = {g_a.first};
    if (a.twoPictures) pics.push_back(g_a.second);
    auto send = [&](const std::string& fill, int canvas, const std::string& label) {
        std::string instruction = a.instruction;
        const size_t at = instruction.find("{X}");
        if (at != std::string::npos) instruction.replace(at, 3, fill);
        EditContext ctx;
        ctx.canvas = canvas;
        if (SendPictureEdit(app, pics, instruction, label, a.shots ? 1 : g_a.times, &ctx)) g_a.waiting.push_back({LastEditActivity(), a.name});
    };
    if (a.shots) { // every framing, at once
        for (const Choice& c : choices) send(c.text, 0, std::string(a.name) + ": " + c.label);
    } else if (a.canvasChoice) {
        const Choice& c = choices[std::clamp(g_a.choice, 0, static_cast<int>(choices.size()) - 1)];
        send(std::string(), std::atoi(c.text.c_str()), std::string(a.name) + ": " + c.label);
    } else if (!choices.empty()) {
        const Choice& c = choices[std::clamp(g_a.choice, 0, static_cast<int>(choices.size()) - 1)];
        send(c.text, 0, std::string(a.name) + ": " + c.label);
    } else {
        send(g_a.words, 0, std::string(a.name) + (a.ask ? std::string(": ") + g_a.words : std::string()));
    }
    app.requestTab = 6; // (stay here: SendPictureEdit may have asked for the Edits page, which this is)
}

} // namespace

void DrawAppsBody(AppState& app, float height) {
    Load();
    Pump(app);
    { // tests: MV_APP_TEST="App name|picture[|choice]" picks it and makes it once
        static bool once = false;
        wchar_t buf[1024] = {};
        if (!once && GetEnvironmentVariableW(L"MV_APP_TEST", buf, 1024) > 0) {
            once = true;
            const std::string spec = U8(buf);
            const size_t bar = spec.find('|'), bar2 = spec.find('|', bar + 1);
            for (int i = 0; i < kAppCount; ++i)
                if (bar != std::string::npos && spec.substr(0, bar) == kApps[i].name) g_a.selected = i;
            g_a.first = W(spec.substr(bar + 1, bar2 == std::string::npos ? std::string::npos : bar2 - bar - 1));
            g_a.choice = bar2 == std::string::npos ? 0 : std::atoi(spec.c_str() + bar2 + 1);
            Make(app, kApps[g_a.selected]);
        }
    }
    if (!ImGui::BeginTable("##apps", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings, ImVec2(0, height))) return;
    ImGui::TableSetupColumn("cards", ImGuiTableColumnFlags_WidthStretch, 0.58f);
    ImGui::TableSetupColumn("app", ImGuiTableColumnFlags_WidthStretch, 0.42f);
    ImGui::TableNextRow(0, height);
    // ---- the cards
    ImGui::TableSetColumnIndex(0);
    ImGui::BeginChild("##appcards", ImVec2(0, height));
    for (int c = 0; c < 7; ++c) {
        if (c) ui::SameLineIfFits(kCategories[c]);
        ImGui::PushStyleColor(ImGuiCol_Button, g_a.category == c ? ui::ChipActiveColor() : ui::ChipColor());
        if (ImGui::SmallButton(kCategories[c])) g_a.category = c;
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    const float cw = ui::Dp(132.0f), ch = ui::Dp(150.0f), gap = ui::Dp(10.0f);
    const int cols = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + gap) / (cw + gap)));
    int shown = 0;
    for (int i = 0; i < kAppCount; ++i) {
        const App& a = kApps[i];
        if (g_a.category != 0 && std::string(a.category) != kCategories[g_a.category]) continue;
        if (shown++ % cols) ImGui::SameLine(0, gap);
        ImGui::BeginGroup();
        ImGui::PushID(i);
        auto last = g_a.lastMade.find(a.name);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool click = Picture(app, last != g_a.lastMade.end() ? last->second : std::wstring(), cw, ch, "##card");
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (last == g_a.lastMade.end()) { // none made yet: its name and what it does, in the box
            dl->AddText(ui::TitleFont(), ui::TitleFont()->FontSize, ImVec2(p.x + ui::Dp(10.0f), p.y + ui::Dp(10.0f)), ImGui::GetColorU32(ImGuiCol_Text), a.name, nullptr, cw - ui::Dp(20.0f));
            dl->AddText(nullptr, 0.0f, ImVec2(p.x + ui::Dp(10.0f), p.y + ch * 0.45f), ImGui::GetColorU32(ImGuiCol_TextDisabled), a.about, nullptr, cw - ui::Dp(20.0f));
        } else { // its name over the picture
            const ImVec2 ts = ImGui::CalcTextSize(a.name);
            dl->AddRectFilled(ImVec2(p.x, p.y + ch - ts.y - ui::Dp(10.0f)), ImVec2(p.x + cw, p.y + ch), IM_COL32(0, 0, 0, 160), ui::Dp(6.0f), ImDrawFlags_RoundCornersBottom);
            dl->AddText(ImVec2(p.x + ui::Dp(8.0f), p.y + ch - ts.y - ui::Dp(5.0f)), IM_COL32(255, 255, 255, 240), a.name);
        }
        if (g_a.selected == i) dl->AddRect(p, ImVec2(p.x + cw, p.y + ch), ImGui::GetColorU32(ui::kAccent), ui::Dp(6.0f), 0, ui::Dp(2.5f));
        else if (ImGui::IsItemHovered()) dl->AddRect(p, ImVec2(p.x + cw, p.y + ch), ImGui::GetColorU32(ui::kAccentHover), ui::Dp(6.0f), 0, ui::Dp(1.5f));
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s\n%s", a.name, a.about);
        if (click && g_a.selected != i) g_a.selected = i, g_a.choice = 0;
        ImGui::PopID();
        ImGui::EndGroup();
    }
    ImGui::EndChild();
    // ---- the app picked
    ImGui::TableSetColumnIndex(1);
    ImGui::BeginChild("##appform", ImVec2(0, height), ImGuiChildFlags_AlwaysUseWindowPadding);
    const App& a = kApps[std::clamp(g_a.selected, 0, kAppCount - 1)];
    ImGui::PushFont(ui::TitleFont());
    ImGui::TextUnformatted(a.name);
    ImGui::PopFont();
    ImGui::TextDisabled("%s  -  %s", a.category, a.about);
    ImGui::Spacing();
    Slot(app, "The picture", g_a.first, "first");
    if (a.twoPictures) {
        ImGui::SameLine(0, ui::Dp(16.0f));
        Slot(app, std::string(a.name) == "Recast" ? "Who goes in" : "The outfit's picture", g_a.second, "second");
    }
    const std::vector<Choice> choices = ChoicesOf(a);
    if (!choices.empty()) {
        ui::SectionHeading(a.shots ? "THE SHOTS (ALL MADE)" : a.canvasChoice ? "THE NEW SHAPE" : "WHICH");
        for (size_t c = 0; c < choices.size(); ++c) {
            if (c) ui::SameLineIfFits(choices[c].label.c_str());
            const bool on = a.shots || g_a.choice == static_cast<int>(c);
            ImGui::PushStyleColor(ImGuiCol_Button, on ? ui::ChipActiveColor() : ui::ChipColor());
            if (ImGui::SmallButton(choices[c].label.c_str()) && !a.shots) g_a.choice = static_cast<int>(c);
            ImGui::PopStyleColor();
            if (!a.canvasChoice && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", choices[c].text.c_str());
        }
    }
    if (a.ask) {
        ui::SectionHeading(a.ask);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##words", g_a.words, sizeof(g_a.words));
    }
    if (!a.shots) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("How many:");
        for (int t = 1; t <= 4; ++t) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, g_a.times == t ? ui::ChipActiveColor() : ui::ChipColor());
            if (ImGui::SmallButton(std::to_string(t).c_str())) g_a.times = t;
            ImGui::PopStyleColor();
        }
    }
    const bool ready = !g_a.first.empty() && (!a.twoPictures || !g_a.second.empty()) && (!a.ask || g_a.words[0]);
    ImGui::Spacing();
    ImGui::BeginDisabled(!ready);
    char go[64];
    snprintf(go, sizeof(go), a.shots ? "Make the %d shots" : "Make it", static_cast<int>(choices.size()));
    if (ui::PrimaryButton(go, ImVec2(ui::Dp(170.0f), ui::Dp(36.0f)))) Make(app, a);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("Qwen Image 2.1 Edit, about %s each", FormatEditTime(EstimateEditSeconds(app.comfy.editSteps, app.comfy.editResolution, a.twoPictures ? 2 : 1, 0)).c_str());
    if (!ready) ImGui::TextDisabled(g_a.first.empty() ? "Pick the picture first." : a.twoPictures && g_a.second.empty() ? "Pick the second picture too." : "Say it in a few words first.");
    const int making = static_cast<int>(std::count_if(g_a.waiting.begin(), g_a.waiting.end(), [&](const auto& w) { return w.second == a.name; }));
    if (making) ImGui::TextColored(ui::kAccent, "Making %d...", making);
    // what it made this session
    std::vector<std::wstring> mine;
    for (const auto& [n, pth] : g_a.made)
        if (n == a.name) mine.push_back(pth);
    if (!mine.empty()) {
        ui::SectionHeading("MADE WITH IT (ALSO IN PICTURE EDITS)");
        const float tw = ui::Dp(150.0f);
        for (size_t k = mine.size(); k-- > 0;) {
            if (k + 1 != mine.size()) ui::SameLineIfFits("XXXXXXXXXXXXXXXXX");
            ImGui::PushID(static_cast<int>(k));
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (Picture(app, mine[k], tw, tw * 1.25f, "##made")) app.OpenFile(mine[k]), app.requestTab = 0;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Open it in the gallery\nRight-click: use it as this app's picture, mark it NSFW");
            if (ImGui::BeginPopupContextItem("##madectx")) {
                if (ImGui::MenuItem("Use it as the picture (do more to it)")) g_a.first = mine[k];
                NsfwMarkMenu(app, mine[k]);
                ImGui::EndPopup();
            }
            DrawNsfwOverlay(app, mine[k], p, ImVec2(p.x + tw, p.y + tw * 1.25f));
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::EndTable();
}
