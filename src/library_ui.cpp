// Poses & outfits: your own copies of two picture libraries, in a folder of yours (Options: Library folder):
//   - ギア's AI pose gallery (anhao/ai-pose-gallery-zh, its data/pose-zh.json): 1,439 drawn mannequins, 288 poses seen from up to 5 angles;
//   - ギア's AI costume gallery (Stone-dielianhua/ComfyUI_Costume-Gallery_ZH, its data/costume-zh.json): 1,407 outfits with English keywords.
// Their code is MIT; their pictures and data belong to ギア (@giaaibusi) and may not be shared, re-uploaded or sold (and the poses not used
// commercially) - so the app never ships them, it only reads a copy you keep.
// The picker shows them (search, group, category, angle); an edit puts someone into one (FLUX 3 Klein with the two pictures):
//   - an outfit: the outfit's picture and its keywords;
//   - a pose: the drawn mannequin alone was ignored (2026-10-29, FLUX 3 kept her standing), so the writer first puts the pose into words
//     ("sitting on the ground, leaning back on her right hand, her left hand on her hip, legs bent to the side") - then FLUX 3 and Qwen 2.1
//     both sat her down exactly so, keeping her face, hair and clothes.
#include "library_ui.h"

#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <vector>

#include "app_state.h"
#include "clef.h"
#include "comfy.h"
#include "imgui.h"
#include "json.hpp"
#include "notify.h"
#include "ui_extra.h"
#include "ui_theme.h"
#include "vision_prompt.h" // ImageDataUrl

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    std::string s(static_cast<size_t>(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), static_cast<int>(s.size()), nullptr, nullptr);
    return s;
}
std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    std::wstring w(static_cast<size_t>(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), static_cast<int>(w.size()));
    return w;
}
std::wstring DataDir() {
    wchar_t buf[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    return std::wstring(buf) + L"\\Try2Create";
}
std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The libraries' own (Chinese) names, in English
const std::map<std::string, std::string> kNames = {
    {"上半身单人", "Upper body"}, {"全身单人・标准系", "Full body"}, {"全身单人・性感系", "Full body, sexy"}, {"双人缠绵", "Two people, intimate"},
    {"正面", "Front"}, {"斜侧", "Three-quarter"}, {"侧面", "Side"}, {"俯视", "From above"}, {"仰视", "From below"},
    {"趴卧", "Lying face down"}, {"性感・大胆系", "Sexy, bold"}, {"站姿", "Standing"}, {"坐姿", "Sitting"}, {"可爱系", "Cute"},
    {"挤胸・强调事业线系", "Chest pushed together"}, {"跪立・四肢着地", "Kneeling, on all fours"}, {"特殊", "Special"}, {"正常位（传教士）", "Missionary"},
    {"跪立", "Kneeling upright"}, {"侧坐・女孩坐", "Sitting to the side"}, {"后入位", "From behind"}, {"背向坐位", "Sitting, facing away"}, {"蹲姿", "Squatting"},
    {"前倾", "Leaning forward"}, {"贴脸系", "Cheek to cheek"}, {"骑乘位", "On top"}, {"立位", "Standing together"}, {"立位贴身", "Standing, pressed close"},
    {"后仰・挺背", "Leaning back, arched"}, {"扭身", "Twisting"}, {"侧卧位", "Lying on the side"}, {"坐位", "Sitting together"}, {"面对面坐位", "Sitting face to face"},
    {"坐位贴身", "Sitting, pressed close"}, {"贴身", "Pressed close"},
    {"夏季", "Summer"}, {"冬季", "Winter"}, {"秋季", "Autumn"}, {"春季", "Spring"}, {"内衣", "Underwear"}, {"性感", "Sexy"}, {"泳装", "Swimwear"}, {"派对", "Party"}};
std::string En(const std::string& zh) {
    auto it = kNames.find(zh);
    return it != kNames.end() ? it->second : zh;
}

struct Library {
    bool loaded = false;
    std::wstring root;               // the folder (searched for the two libraries)
    std::vector<LibItem> items[2];   // poses, outfits
    std::wstring dirs[2];            // where each was found
};
Library g_lib;

std::wstring SettingsPath() { return DataDir() + L"\\library.json"; }

void SaveRoot() {
    std::error_code ec;
    fs::create_directories(DataDir(), ec);
    std::ofstream f(SettingsPath(), std::ios::binary | std::ios::trunc);
    if (f) f << json({{"root", Utf8(g_lib.root)}}).dump();
}

// A library under `dir`: it, or a folder in it (two levels down), holding data/<file>
std::wstring FindLibrary(const fs::path& dir, const wchar_t* file) {
    std::error_code ec;
    if (fs::exists(dir / L"data" / file, ec)) return dir.wstring();
    for (const auto& a : fs::directory_iterator(dir, ec)) {
        if (!a.is_directory(ec)) continue;
        if (fs::exists(a.path() / L"data" / file, ec)) return a.path().wstring();
        for (const auto& b : fs::directory_iterator(a.path(), ec))
            if (b.is_directory(ec) && fs::exists(b.path() / L"data" / file, ec)) return b.path().wstring();
    }
    return {};
}

void LoadLibraries() {
    g_lib.loaded = true;
    g_lib.items[0].clear(), g_lib.items[1].clear();
    if (g_lib.root.empty()) {
        std::ifstream f(SettingsPath(), std::ios::binary);
        const json j = f ? json::parse(f, nullptr, false) : json();
        if (j.is_object()) g_lib.root = Wide(j.value("root", std::string()));
    }
    if (g_lib.root.empty()) return;
    static const wchar_t* const kFile[] = {L"pose-zh.json", L"costume-zh.json"};
    for (int k = 0; k < 2; ++k) {
        g_lib.dirs[k] = FindLibrary(g_lib.root, kFile[k]);
        if (g_lib.dirs[k].empty()) continue;
        std::ifstream f(fs::path(g_lib.dirs[k]) / L"data" / kFile[k], std::ios::binary);
        const json j = f ? json::parse(f, nullptr, false) : json();
        if (!j.is_array()) continue;
        const fs::path base = g_lib.dirs[k];
        for (const json& e : j) {
            if (!e.is_object()) continue;
            LibItem it;
            it.kind = k;
            it.id = e.value("id", std::string());
            if (k == 0) {
                it.group = En(e.value("体系", std::string()));
                it.category = En(e.value("分类", std::string()));
                it.angle = En(e.value("角度", std::string()));
                it.thumb = (base / Wide(e.value("缩略图", std::string()))).wstring();
                it.image = (base / Wide(e.value("原图", std::string()))).wstring();
            } else {
                it.category = En(e.value("分类", std::string()));
                it.words = e.value("关键词", std::string());
                it.thumb = (base / Wide(e.value("缩略图", std::string()))).wstring();
                const std::string full = e.value("原图", e.value("大图", std::string()));
                it.image = (base / Wide(full)).wstring();
            }
            if (!it.id.empty()) g_lib.items[k].push_back(std::move(it));
        }
    }
}

// ---- the picker
struct Picker {
    bool open = false;
    int kind = 0;
    std::string title, search;
    std::string group = "Full body", category, angle = "Front"; // ("" = any)
    std::function<void(AppState&, const LibItem&)> use;
};
Picker g_pick;

int StrResize(ImGuiInputTextCallbackData* d) {
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* s = static_cast<std::string*>(d->UserData);
        s->resize(d->BufTextLen);
        d->Buf = s->data();
    }
    return 0;
}

bool Chip(const char* label, bool on) {
    ImGui::PushStyleColor(ImGuiCol_Button, on ? ui::ChipActiveColor() : ui::ChipColor());
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::ChipHoverColor());
    const bool hit = ImGui::SmallButton(label);
    ImGui::PopStyleColor(2);
    return hit;
}

bool Matches(const LibItem& it) {
    if (it.kind == 0) {
        if (!g_pick.group.empty() && it.group != g_pick.group) return false;
        if (!g_pick.angle.empty() && it.angle != g_pick.angle) return false;
    }
    if (!g_pick.category.empty() && it.category != g_pick.category) return false;
    if (g_pick.search.empty()) return true;
    const std::string hay = Lower(it.id + " " + it.group + " " + it.category + " " + it.angle + " " + it.words);
    size_t a = 0;
    const std::string q = Lower(g_pick.search);
    while (a < q.size()) { // every word must be there
        size_t b = q.find(' ', a);
        if (b == std::string::npos) b = q.size();
        if (b > a && hay.find(q.substr(a, b - a)) == std::string::npos) return false;
        a = b + 1;
    }
    return true;
}

// ---- the edits
struct Edit {
    LibItem item;
    std::wstring person;
    std::function<void(AppState&, const std::wstring&, const std::string&)> done;
    std::future<std::string> words; // a pose: the writer describing it
    int act = 0;
};
std::vector<std::unique_ptr<Edit>> g_edits;
std::mutex g_wordsMutex;
std::map<std::string, std::string> g_poseWords; // pose id -> the writer's words (kept in library_poses.json)
bool g_wordsLoaded = false;

std::wstring WordsPath() { return DataDir() + L"\\library_poses.json"; }

void LoadWords() {
    g_wordsLoaded = true;
    std::ifstream f(WordsPath(), std::ios::binary);
    const json j = f ? json::parse(f, nullptr, false) : json();
    if (!j.is_object()) return;
    std::lock_guard<std::mutex> lock(g_wordsMutex);
    for (auto it = j.begin(); it != j.end(); ++it)
        if (it.value().is_string()) g_poseWords[it.key()] = it.value().get<std::string>();
}

void SaveWords() {
    json j = json::object();
    {
        std::lock_guard<std::mutex> lock(g_wordsMutex);
        for (const auto& [k, v] : g_poseWords) j[k] = v;
    }
    std::ofstream f(WordsPath(), std::ios::binary | std::ios::trunc);
    if (f) f << j.dump(1);
}

// (a two-person pose is kept under "2:" + its id: before 2026-10-30 they were described as one figure)
std::string WordsKey(const std::string& id) { return (LibraryPosePeople(id) >= 2 ? "2:" : "") + id; }

std::string DescribePose(std::wstring drawing, std::string comfyUrl, int people) {
    if (!WriterConfigured()) return {};
    if (people >= 2) { // two figures: where each one is and how they hold each other
        const std::string img = ImageDataUrl(drawing, 768);
        if (img.empty()) return {};
        const ClefSession session = WriterAcquire(comfyUrl, [](const std::string&) {});
        if (!session.ok) {
            WriterRelease(session, false);
            return {};
        }
        std::string out, err;
        const bool ok = WriterChat("You describe the pose of two drawn mannequins for an image model that will draw two real people in exactly that pose.",
                                   "In at most 80 words: how the two are placed (who is on the left and right, facing each other or the viewer, how close), "
                                   "then for the one on the left and the one on the right: standing, sitting, kneeling or lying, where the torso leans, "
                                   "where each arm and hand is and where it touches the other, the legs, and where the head turns and looks. Nothing about "
                                   "clothing or the drawing style. Just the description.",
                                   {img}, 260, 0.2, out, &err);
        WriterRelease(session, false);
        if (!ok) return {};
        const size_t s = out.find_first_not_of(" \t\r\n\""), e = out.find_last_not_of(" \t\r\n\"");
        return s == std::string::npos ? std::string() : out.substr(s, e - s + 1);
    }
    const std::string img = ImageDataUrl(drawing, 768);
    if (img.empty()) return {};
    const ClefSession session = WriterAcquire(comfyUrl, [](const std::string&) {});
    if (!session.ok) {
        WriterRelease(session, false);
        return {};
    }
    std::string out, err;
    const bool ok = WriterChat("You describe the pose of a drawn mannequin for an image editor that will put a real person into exactly that pose.",
                               "In one sentence of at most 45 words: whether the body stands, sits, kneels or lies, where the torso leans, where each arm "
                               "and hand is and what it rests on, where each leg and foot is, which way the head turns and where it looks, and how the "
                               "body faces the viewer. Use left and right as the figure's own. Nothing about clothing or the drawing style. Just the sentence.",
                               {img}, 160, 0.2, out, &err);
    WriterRelease(session, false);
    if (!ok) return {};
    const size_t s = out.find_first_not_of(" \t\r\n\""), e = out.find_last_not_of(" \t\r\n\"");
    return s == std::string::npos ? std::string() : out.substr(s, e - s + 1);
}

std::string EditPrompt(const LibItem& it, const std::string& poseWords) {
    const std::string keep = " They stay the same kind of picture as image 1 (a photographed person stays photographic, a 3D animated or drawn character stays "
                             "so). Full body from head to feet, one person, a plain light grey studio backdrop, soft even light.";
    if (it.kind == 1)
        return "The person from image 1 wearing exactly the outfit from image 2 (" + it.words + "): every piece of it, with its colours, patterns and "
               "materials, and nothing of their old clothes. They keep their own face, hair, skin and body exactly as in image 1, standing naturally." + keep;
    return "The person from image 1 in exactly the pose of the drawn mannequin in image 2" + (poseWords.empty() ? std::string(".") : ": " + poseWords) +
           " They keep their own face, hair, clothes and body exactly as in image 1; nothing of the drawing's style shows." + keep;
}

void SendEdit(AppState& app, Edit& e, const std::string& poseWords) {
    ComfyJob job;
    job.images = {e.person, e.item.image};
    job.prompt = EditPrompt(e.item, poseWords);
    job.canvas = 3; // (a new portrait picture: a full-body figure)
    job.editEngine = "flux3";
    job.times = 1;
    job.label = (e.item.kind == 1 ? "Outfit " : "Pose ") + e.item.id;
    e.act = SendComfyJob(app, std::move(job));
}

} // namespace

bool LibraryHas(int kind) {
    if (!g_lib.loaded) LoadLibraries();
    return !g_lib.items[std::clamp(kind, 0, 1)].empty();
}

std::string LibraryPoseWords(const std::string& poseId) {
    if (!g_wordsLoaded) LoadWords();
    std::lock_guard<std::mutex> lock(g_wordsMutex);
    auto it = g_poseWords.find(WordsKey(poseId));
    return it != g_poseWords.end() ? it->second : std::string();
}

void OpenLibraryPicker(AppState& app, int kind, const std::string& title, std::function<void(AppState&, const LibItem&)> use) {
    (void)app;
    if (!g_lib.loaded) LoadLibraries();
    g_pick.open = true;
    if (g_pick.kind != kind) g_pick.category.clear();
    g_pick.kind = std::clamp(kind, 0, 1);
    g_pick.title = title;
    g_pick.use = std::move(use);
}

int LibraryEditsBusy() { return static_cast<int>(g_edits.size()); }

void LibraryEdit(AppState& app, const std::wstring& person, const LibItem& item,
                 std::function<void(AppState&, const std::wstring& made, const std::string& why)> done) {
    auto e = std::make_unique<Edit>();
    e->item = item;
    e->person = person;
    e->done = std::move(done);
    const std::string known = item.kind == 0 ? LibraryPoseWords(item.id) : std::string();
    if (item.kind == 0 && known.empty()) e->words = std::async(std::launch::async, DescribePose, item.image, app.comfy.url, LibraryPosePeople(item.id));
    else SendEdit(app, *e, known);
    if (!e->words.valid() && !e->act) {
        if (e->done) e->done(app, std::wstring(), "the picture edit could not be sent");
        return;
    }
    g_edits.push_back(std::move(e));
}

struct Describe {
    std::string id;
    std::future<std::string> words;
    std::function<void(AppState&, const std::string&)> done;
};
std::vector<std::unique_ptr<Describe>> g_describes;

void LibraryDescribePose(AppState& app, const LibItem& pose, std::function<void(AppState&, const std::string& words)> done) {
    const std::string known = LibraryPoseWords(pose.id);
    if (!known.empty()) {
        if (done) done(app, known);
        return;
    }
    auto d = std::make_unique<Describe>();
    d->id = pose.id;
    d->words = std::async(std::launch::async, DescribePose, pose.image, app.comfy.url, LibraryPosePeople(pose.id));
    d->done = std::move(done);
    g_describes.push_back(std::move(d));
}

void PumpLibrary(AppState& app) {
    { // tests: MV_LIB_PICK=0|1 opens the picker; MV_LIB_EDIT="0|<pose id>|<picture>" (1 = an outfit) makes the edit, the result in library_test_out.txt
        static bool done = false;
        if (!done) {
            done = true;
            char buf[2048] = {};
            if (GetEnvironmentVariableA("MV_LIB_PICK", buf, sizeof(buf))) OpenLibraryPicker(app, std::atoi(buf) == 1, "Test", nullptr);
            wchar_t w[2048] = {};
            if (GetEnvironmentVariableW(L"MV_LIB_EDIT", w, 2048)) {
                const std::string s = Utf8(w);
                const size_t a = s.find('|'), b = a == std::string::npos ? a : s.find('|', a + 1);
                if (b != std::string::npos) {
                    if (!g_lib.loaded) LoadLibraries();
                    const int kind = std::atoi(s.substr(0, a).c_str()) == 1;
                    const std::string id = s.substr(a + 1, b - a - 1);
                    const auto f = std::find_if(g_lib.items[kind].begin(), g_lib.items[kind].end(), [&](const LibItem& it) { return it.id == id; });
                    auto out = [](AppState&, const std::wstring& made, const std::string& why) {
                        std::ofstream t(DataDir() + L"\\library_test_out.txt", std::ios::binary | std::ios::trunc);
                        t << (made.empty() ? "failed: " + why : Utf8(made));
                    };
                    if (f == g_lib.items[kind].end()) out(app, {}, "no item " + id);
                    else LibraryEdit(app, Wide(s.substr(b + 1)), *f, out);
                }
            }
        }
    }
    for (auto& d : g_describes) {
        if (!d->words.valid() || d->words.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
        const std::string w = d->words.get();
        if (!w.empty()) {
            {
                std::lock_guard<std::mutex> lock(g_wordsMutex);
                g_poseWords[WordsKey(d->id)] = w;
            }
            SaveWords();
        }
        if (d->done) d->done(app, w);
    }
    g_describes.erase(std::remove_if(g_describes.begin(), g_describes.end(), [](const std::unique_ptr<Describe>& d) { return !d->words.valid(); }), g_describes.end());
    for (auto& e : g_edits) {
        if (e->words.valid() && e->words.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            const std::string w = e->words.get();
            if (!w.empty()) {
                {
                    std::lock_guard<std::mutex> lock(g_wordsMutex);
                    g_poseWords[WordsKey(e->item.id)] = w;
                }
                SaveWords();
            }
            SendEdit(app, *e, w); // (no writer: the pose's picture and the plain wording alone)
            if (!e->act) {
                if (e->done) e->done(app, std::wstring(), "the picture edit could not be sent");
                e->done = nullptr;
            }
        }
        if (!e->act || !app.comfySvc) continue;
        for (const ComfyActivity& a : app.comfySvc->Activity()) {
            if (a.id != e->act) continue;
            if (a.state == ComfyState::Failed) {
                if (e->done) e->done(app, std::wstring(), a.message.substr(0, 160));
                e->done = nullptr, e->act = -1;
            } else if (a.state == ComfyState::Done) {
                std::wstring made;
                for (const ComfyResult& f : a.files)
                    if (!f.localPath.empty() && !f.video) made = f.localPath;
                if (e->done) e->done(app, made, made.empty() ? "no picture came back" : "");
                e->done = nullptr, e->act = -1;
            }
            break;
        }
    }
    g_edits.erase(std::remove_if(g_edits.begin(), g_edits.end(), [](const std::unique_ptr<Edit>& e) { return e->act < 0 || (!e->done && !e->words.valid()); }),
                  g_edits.end());
}

void DrawLibraryFolderSetting() {
    if (!g_lib.loaded) LoadLibraries();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Poses & outfits folder:");
    ImGui::SameLine();
    ImGui::TextUnformatted(g_lib.root.empty() ? "(none)" : Utf8(g_lib.root).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Change...##libroot")) {
        BROWSEINFOW bi = {};
        bi.hwndOwner = GetActiveWindow();
        bi.lpszTitle = L"The folder with your copies of the pose and outfit galleries";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        if (PIDLIST_ABSOLUTE id = SHBrowseForFolderW(&bi)) {
            wchar_t buf[MAX_PATH] = {};
            if (SHGetPathFromIDListW(id, buf)) g_lib.root = buf, SaveRoot(), LoadLibraries();
            CoTaskMemFree(id);
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%d poses, %d outfits", static_cast<int>(g_lib.items[0].size()), static_cast<int>(g_lib.items[1].size()));
}

void DrawLibraryPicker(AppState& app) {
    if (!g_pick.open) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(std::min(vp->WorkSize.x * 0.8f, ui::Dp(1100.0f)), vp->WorkSize.y * 0.82f), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    const std::string head = (g_pick.title.empty() ? std::string("Poses & outfits") : g_pick.title) + "###libpicker";
    if (!ImGui::Begin(head.c_str(), &g_pick.open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    if (Chip("Poses##lk0", g_pick.kind == 0)) g_pick.kind = 0, g_pick.category.clear();
    ImGui::SameLine(0.0f, ui::Dp(4.0f));
    if (Chip("Outfits##lk1", g_pick.kind == 1)) g_pick.kind = 1, g_pick.category.clear();
    ImGui::SameLine(0.0f, ui::Dp(16.0f));
    ImGui::SetNextItemWidth(ui::Dp(260.0f));
    ImGui::InputTextWithHint("##libsearch", g_pick.kind == 1 ? "search: white dress, denim, SPR-0001..." : "search: sitting, kneeling, P006...", g_pick.search.data(),
                             g_pick.search.capacity() + 1, ImGuiInputTextFlags_CallbackResize, StrResize, &g_pick.search);
    ImGui::SameLine();
    DrawLibraryFolderSetting();
    const std::vector<LibItem>& all = g_lib.items[g_pick.kind];
    if (all.empty()) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ui::kWarn, "%s", g_pick.kind == 0 ? "No pose library in that folder: it needs your copy of anhao/ai-pose-gallery-zh (its data\\pose-zh.json)."
                                                             : "No outfit library in that folder: it needs your copy of Stone-dielianhua/ComfyUI_Costume-Gallery_ZH (its data\\costume-zh.json).");
        ImGui::TextDisabled("Their pictures belong to ギア (@giaaibusi) and may not be shared or sold: the app only reads a copy you keep.");
        ImGui::PopTextWrapPos();
        ImGui::End();
        return;
    }
    // the filters, as chips: the values there are, in their order
    auto chips = [&](const char* label, std::string& cur, auto get, const char* idp) {
        std::vector<std::string> vals;
        for (const LibItem& it : all) {
            const std::string v = get(it);
            if (!v.empty() && std::find(vals.begin(), vals.end(), v) == vals.end()) vals.push_back(v);
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label);
        ImGui::SameLine();
        if (Chip((std::string("Any##") + idp).c_str(), cur.empty())) cur.clear();
        for (const std::string& v : vals) {
            ui::SameLineIfFits(v.c_str());
            if (Chip((v + "##" + idp).c_str(), cur == v)) cur = v;
        }
    };
    if (g_pick.kind == 0) {
        chips("Kind", g_pick.group, [](const LibItem& i) { return i.group; }, "lg");
        chips("Seen", g_pick.angle, [](const LibItem& i) { return i.angle; }, "la");
        chips("Pose", g_pick.category, [&](const LibItem& i) { return g_pick.group.empty() || i.group == g_pick.group ? i.category : std::string(); }, "lc");
    } else {
        chips("Kind", g_pick.category, [](const LibItem& i) { return i.category; }, "lo");
    }
    ImGui::Separator();
    std::vector<const LibItem*> shown;
    for (const LibItem& it : all)
        if (Matches(it)) shown.push_back(&it);
    ImGui::TextDisabled("%d %s - click one to use it", static_cast<int>(shown.size()), g_pick.kind == 0 ? "poses" : "outfits");
    ImGui::BeginChild("##libgrid");
    const float tw = ui::Dp(132.0f), th = tw * 4.0f / 3.0f, gap = ui::Dp(6.0f);
    const int perRow = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + gap) / (tw + gap)));
    ImGuiListClipper clip;
    clip.Begin((static_cast<int>(shown.size()) + perRow - 1) / perRow, th + gap);
    const LibItem* chosen = nullptr;
    while (clip.Step()) {
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
            for (int c = 0; c < perRow; ++c) {
                const int k = r * perRow + c;
                if (k >= static_cast<int>(shown.size())) break;
                const LibItem& it = *shown[k];
                if (c) ImGui::SameLine(0.0f, gap);
                ImGui::PushID(k);
                const ImVec2 p = ImGui::GetCursorScreenPos();
                if (ImGui::InvisibleButton("##t", ImVec2(tw, th))) chosen = &it;
                const bool hov = ImGui::IsItemHovered();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(p, ImVec2(p.x + tw, p.y + th), hov ? IM_COL32(255, 255, 255, 40) : ui::ThumbPlaceholder(), ui::Dp(6.0f));
                const AppState::LibThumb lt = app.LibraryThumbRaw(it.thumb);
                if (lt.srv && lt.w > 0 && lt.h > 0) {
                    const float s = std::min(tw / lt.w, th / lt.h);
                    const ImVec2 a(p.x + (tw - lt.w * s) * 0.5f, p.y + (th - lt.h * s) * 0.5f);
                    dl->AddImage((ImTextureID)(intptr_t)lt.srv, a, ImVec2(a.x + lt.w * s, a.y + lt.h * s));
                }
                if (hov) {
                    dl->AddRect(p, ImVec2(p.x + tw, p.y + th), ImGui::GetColorU32(ui::kAccent), ui::Dp(6.0f), 0, ui::Dp(2.0f));
                    if (it.kind == 0) ImGui::SetTooltip("%s\n%s, %s - seen %s", it.id.c_str(), it.group.c_str(), it.category.c_str(), it.angle.c_str());
                    else ImGui::SetTooltip("%s - %s\n%s", it.id.c_str(), it.category.c_str(), it.words.c_str());
                }
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();
    ImGui::End();
    if (chosen) {
        const LibItem pick = *chosen;
        g_pick.open = false;
        if (g_pick.use) {
            auto use = std::move(g_pick.use);
            g_pick.use = nullptr;
            use(app, pick);
        }
    }
}
