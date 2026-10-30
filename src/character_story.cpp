// Every character a person: a name of their own and a backstory (age, how they act, walk and talk, what they like, childhood, school,
// university, work or the lack of it, family, what they want and fear). The prompt writer looks at the character's picture and writes it;
// it also invents the name when the character has none of its own ("Lyra 2", "Blonde Bird", a file name). And: characters taken out of a
// picture - the writer finds each one in it, and Qwen Image Edit draws them alone, full-body, in a white studio, as new characters.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <future>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

#include "app_state.h"
#include "board.h"
#include "clef.h"
#include "comfy.h"
#include "imgui.h"
#include "json.hpp"
#include "notify.h"
#include "ui_extra.h"
#include "ui_theme.h"
#include "vision_prompt.h"
#include "win_dialogs.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::string U8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n\""), b = s.find_last_not_of(" \t\r\n\"");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

json ParseObject(const std::string& reply, char open, char close) {
    const size_t a = reply.find(open), b = reply.rfind(close);
    if (a == std::string::npos || b == std::string::npos || b < a) return json();
    return json::parse(reply.substr(a, b - a + 1), nullptr, false);
}

// ---- names
std::vector<std::string> TakenNames() {
    std::vector<std::string> out;
    for (const CharacterProfile& p : AllProfiles()) {
        if (p.saved && !p.name.empty()) out.push_back(p.name);
    }
    return out;
}

bool Taken(const std::string& name, const std::vector<std::string>& also = {}) {
    const std::string l = Lower(Trim(name));
    for (const std::string& t : TakenNames()) {
        if (Lower(t) == l) return true;
    }
    for (const std::string& t : also) {
        if (Lower(t) == l) return true;
    }
    return false;
}

// A name nobody chose: one the app made up ("Blonde Bird"), a numbered copy ("Lyra 2"), or the picture's file name
bool GenericName(const CharacterProfile& p) {
    const std::string n = Trim(p.name);
    if (n.empty()) return true;
    if (std::isdigit(static_cast<unsigned char>(n.back())) && n.find(' ') != std::string::npos) return true; // "Lyra 2"
    if (Lower(n) == Lower(U8(fs::path(p.image).stem().wstring()))) return true;                             // "character_00012_"
    if (n.find('_') != std::string::npos) return true;
    static const char* const kTraitWords[] = {"Blonde", "Black", "Brown", "Red", "Grey", "Gray", "White", "Blue", "Green", "Pink", "Silver", "Purple", "Bald"};
    static const char* const kKinds[] = {"Man", "Woman", "Person", "Character", "Bird", "Cat", "Dog", "Frog", "Robot", "Alien", "Lizard", "Insect", "Rodent"};
    for (const char* a : kTraitWords) {
        for (const char* b : kKinds) {
            if (Lower(n) == Lower(std::string(a) + " " + b)) return true;
        }
    }
    return false;
}

// When the writer is not there: a made-up name from syllables (never "Lyra 2")
std::string FallbackName(bool creature, const std::vector<std::string>& also) {
    static std::mt19937 rng{std::random_device{}()};
    static const char* const kStart[] = {"Al", "Bel", "Cor", "Dar", "El", "Fen", "Gal", "Hal", "Is", "Jor", "Kael", "Lor", "Mar", "Nor", "Or", "Per", "Quin",
                                         "Ros", "Sel", "Tam", "Ul", "Val", "Wen", "Xan", "Yor", "Zel", "Bryn", "Cass", "Dov", "Eir", "Ione", "Mira", "Tor"};
    static const char* const kEnd[] = {"a", "an", "ar", "en", "ia", "in", "is", "o", "on", "or", "us", "ys", "eth", "wen", "ric", "dan", "lyn", "mir", "ra", "vin"};
    static const char* const kLast[] = {"Ashdown", "Blackwood", "Calder", "Drummond", "Everly", "Fairbrook", "Grimsby", "Hawthorne", "Ironside", "Juniper",
                                        "Kestrel", "Larkin", "Marlowe", "Northcott", "Oakhurst", "Pembrook", "Quill", "Rookwood", "Sterling", "Thorne",
                                        "Underhill", "Vance", "Whitlock", "Yarrow", "Zeller", "Moreau", "Okafor", "Nakamura", "Castillo", "Petrov"};
    for (int tries = 0; tries < 200; ++tries) {
        std::string first = std::string(kStart[rng() % std::size(kStart)]) + kEnd[rng() % std::size(kEnd)];
        const std::string name = creature ? first : first + " " + kLast[rng() % std::size(kLast)];
        if (!Taken(name, also)) return name;
    }
    return "Nameless " + std::to_string(rng() % 10000);
}

// ---- the backstory: what the writer is asked, and how it reads on the page
const char* const kFields[][2] = {
    {"age", "Age"}, {"personality", "How they act"}, {"likes", "Likes"}, {"dislikes", "Dislikes"}, {"walk", "How they walk"}, {"talk", "How they talk"},
    {"childhood", "Childhood"}, {"school", "School"}, {"university", "University or training"}, {"work", "Work"}, {"money_home", "Money and home"},
    {"family_friends", "Family and friends"}, {"love", "Love life"}, {"now", "Now"}, {"wants", "What they want"}, {"fears", "What they fear"},
    {"quirks", "Quirks and habits"}, {"secret", "A secret"},
};

const char* const kStorySystem =
    "You give a character a life story. You see their picture and what is known about them. Write who they are as a person, in English, "
    "concrete and specific, a little surprising, never generic. Fit their world and kind: a medieval knight has a medieval life (no university: "
    "training, apprenticeship, the order), a modern woman a modern one (school, university or not, jobs, unemployment, rent), a creature a "
    "creature's life in its own world. Their look in the picture is a clue (clothes, scars, posture, age, wealth or poverty).\n"
    "Answer with ONLY one JSON object, no markdown:\n"
    "{\"name\":\"...\",\"age\":\"...\",\"personality\":\"...\",\"likes\":\"...\",\"dislikes\":\"...\",\"walk\":\"...\",\"talk\":\"...\","
    "\"childhood\":\"...\",\"school\":\"...\",\"university\":\"...\",\"work\":\"...\",\"money_home\":\"...\",\"family_friends\":\"...\","
    "\"love\":\"...\",\"now\":\"...\",\"wants\":\"...\",\"fears\":\"...\",\"quirks\":\"...\",\"secret\":\"...\"}\n"
    "Each field 1-3 sentences (age: a number, or a range for creatures). walk: their gait and posture, how they move through a room. talk: "
    "their voice, accent, pace, favourite expressions. work: what they do now or did, or how they live without work (unemployed, retired, "
    "heir, thief, student...). name: when asked for a new name, invent one that fits who they are and their world - uncommon, memorable, easy "
    "to say, first and last name for people, a fitting single or double name for creatures - never one of the taken names, never a famous "
    "character's name, no numbers. When not asked, repeat the name you were given.";

std::string FormatStory(const json& j) {
    std::string out;
    for (const auto& f : kFields) {
        if (!j.contains(f[0])) continue;
        std::string v = j[f[0]].is_string() ? j[f[0]].get<std::string>() : j[f[0]].dump();
        v = Trim(v);
        if (v.empty()) continue;
        out += std::string(f[1]) + ": " + v + "\n";
    }
    return out;
}

struct StoryJob {
    std::wstring image;
    std::string name, description;
    bool rename = false;
    std::vector<std::string> taken; // the names in use when it was queued
};
struct StoryResult {
    std::wstring image;
    std::string name, story, error;
};
std::mutex g_mu;
std::deque<StoryJob> g_jobs;
std::vector<StoryResult> g_done;
std::atomic<bool> g_working{false};
std::set<std::wstring> g_queued; // (no two jobs for one character)
std::string g_comfyUrl;

void Worker() {
    ClefSession session = WriterAcquire(g_comfyUrl, [](const std::string&) {});
    for (;;) {
        StoryJob job;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            if (g_jobs.empty()) break;
            job = g_jobs.front();
            g_jobs.pop_front();
        }
        StoryResult r;
        r.image = job.image;
        if (!session.ok) {
            r.error = "the prompt writer could not be started (Clef mode tab > Prompt writer)";
        } else {
            std::string taken;
            for (const std::string& t : job.taken) taken += (taken.empty() ? "" : ", ") + t;
            std::string user = "Their name now: " + (job.name.empty() ? std::string("(none)") : job.name) + ".\n";
            user += job.rename ? "Invent a NEW name for them (theirs is a placeholder).\n" : "Keep their name.\n";
            if (!job.description.empty()) user += "What is known about them: " + job.description + "\n";
            user += "Taken names (never use these): " + (taken.empty() ? std::string("none") : taken) + "\n";
            std::vector<std::string> images;
            const std::string url = ImageDataUrl(job.image, 768);
            if (!url.empty()) images.push_back(url);
            std::string reply, err;
            if (!WriterChat(kStorySystem, user, images, 2600, 0.9, reply, &err)) {
                r.error = "the prompt writer did not answer" + (err.empty() ? std::string() : ": " + err);
            } else {
                const json j = ParseObject(reply, '{', '}');
                if (!j.is_object()) {
                    r.error = "its answer could not be read";
                } else {
                    r.story = FormatStory(j);
                    std::string name = j.value("name", std::string());
                    name = Trim(name);
                    bool clash = name.empty();
                    for (const std::string& t : job.taken) clash = clash || Lower(t) == Lower(name);
                    if (job.rename) r.name = clash ? std::string() : name; // (a clash: the UI thread picks another one)
                    if (r.story.empty()) r.error = "it wrote no backstory";
                }
            }
        }
        std::lock_guard<std::mutex> lock(g_mu);
        g_done.push_back(r);
    }
    WriterRelease(session, false);
    g_working = false;
}

// ---- characters taken out of a picture
struct Found {
    std::string where, look, name;
    bool creature = false, on = true;
};
struct Extract {
    std::wstring picture;
    std::future<std::pair<std::vector<Found>, std::string>> fut;
    std::vector<Found> found;
    std::string note;
    std::vector<std::pair<int, std::string>> pending; // (activity, name) being drawn
    std::vector<std::string> pendingLook;
} g_ex;

const char* const kFindSystem =
    "You find the characters in a picture: every person, creature or figure that could be a character of its own (not crowds far in the "
    "background). For each one write: where - how to point at them in this picture so nobody could pick the wrong one (\"the woman in the red "
    "coat on the left\"); kind - human, or creature for anything else (animals, monsters, robots, aliens, cartoon animals); look - 35 to 70 "
    "words in English of what they look like: what they are, apparent age and sex, build, face, hair, skin, the outfit from top to bottom with "
    "colours and materials, shoes, what they hold, and the art style if the picture is a drawing; name - a fitting, uncommon, memorable name "
    "for them (first and last for people), never one of the taken names, never a famous character's name. Answer with ONLY a JSON array: "
    "[{\"where\":\"...\",\"kind\":\"human|creature\",\"look\":\"...\",\"name\":\"...\"}]";

std::pair<std::vector<Found>, std::string> FindCharacters(std::string comfyUrl, std::wstring picture, std::vector<std::string> takenList) {
    std::vector<Found> out;
    const ClefSession session = WriterAcquire(comfyUrl, [](const std::string&) {});
    if (!session.ok) return {out, "The prompt writer could not be started (Clef mode tab > Prompt writer)."};
    std::string taken;
    for (const std::string& t : takenList) taken += (taken.empty() ? "" : ", ") + t;
    std::vector<std::string> images;
    const std::string url = ImageDataUrl(picture, 1024);
    if (!url.empty()) images.push_back(url);
    std::string reply, err;
    const bool ok = WriterChat(kFindSystem, "Taken names (never use these): " + (taken.empty() ? std::string("none") : taken) + "\nFind the characters in the attached picture.",
                               images, 2400, 0.6, reply, &err);
    WriterRelease(session, false);
    if (!ok) return {out, "The prompt writer did not answer" + (err.empty() ? std::string(".") : ": " + err)};
    const json j = ParseObject(reply, '[', ']');
    std::vector<std::string> names;
    if (j.is_array()) {
        for (const json& c : j) {
            if (!c.is_object()) continue;
            Found f;
            f.where = Trim(c.value("where", std::string()));
            f.look = Trim(c.value("look", std::string()));
            f.creature = Lower(c.value("kind", std::string())).find("creature") != std::string::npos;
            f.name = Trim(c.value("name", std::string()));
            if (f.where.empty() && f.look.empty()) continue;
            bool clash = f.name.empty();
            for (const std::string& t : takenList) clash = clash || Lower(t) == Lower(f.name);
            for (const std::string& t : names) clash = clash || Lower(t) == Lower(f.name);
            if (clash) f.name.clear(); // (named on the UI thread)
            names.push_back(f.name);
            out.push_back(f);
        }
    }
    if (out.empty()) return {out, "No characters were found in it."};
    return {out, std::string()};
}

int FoundOn() {
    int n = 0;
    for (const Found& f : g_ex.found) n += f.on ? 1 : 0;
    return n;
}

void MakeFound(AppState& app) {
    int sent = 0;
    for (const Found& f : g_ex.found) {
        if (!f.on) continue;
        ComfyJob job;
        job.images = {g_ex.picture};
        job.canvas = 3; // a new portrait picture; <image1> is only the reference
        if (f.creature) { // an animal or a monster: a square frame, its own pose, and nobody from the scene with it
            job.sheetW = 1024, job.sheetH = 1024;
            job.prompt = "<image1> is a scene. Make a new picture of only one subject: " + f.where + " from <image1>, alone and large in the "
                         "middle of the frame, its whole body from head to feet and tail, in a natural standing pose seen from a three-quarter "
                         "angle, on a plain seamless white studio backdrop with a soft floor shadow, bright even studio lighting. Keep exactly its "
                         "species, face, eyes, fur or skin, markings, colours, anything it wears, and the art style of <image1> (a drawing stays a "
                         "drawing, a photograph stays a photograph). Leave out every person and every other animal from <image1>: no people at all, "
                         "no street, only this one creature. It looks like this: " + f.look;
        } else {
            job.sheetW = 768, job.sheetH = 1024;
            job.prompt = "<image1> is a scene. Make a new picture of only " + f.where + " from <image1>: a full-body photograph of them alone, head to "
                         "shoes in frame, standing straight and facing the camera, arms relaxed, on a plain seamless white studio backdrop with a soft "
                         "floor shadow, bright even studio lighting. Keep exactly their face, hair, skin, body, clothes, colours, accessories and the art "
                         "style of <image1> (a drawing stays a drawing, a photograph stays a photograph). Nobody else in the picture. They look like this: " +
                         f.look;
        }
        job.label = "Character: " + f.name;
        job.savePrefix = "Characters/character";
        job.times = 1;
        const int id = SendSheetEdit(app, std::move(job));
        if (!id) continue;
        g_ex.pending.push_back({id, f.name});
        g_ex.pendingLook.push_back(f.look);
        ++sent;
    }
    if (sent) {
        Toast(ToastKind::Info, "Taking out " + std::to_string(sent) + (sent == 1 ? " character" : " characters"),
              "Qwen Image Edit draws each one alone in a white studio; then the writer gives them a backstory", {}, [](AppState& s) { s.requestTab = 5; }, "charex");
        g_ex.found.clear();
    }
}

} // namespace

// ================================================================== public
void QueueCharacterStory(AppState& app, const std::wstring& image, bool forceRename) {
    CharacterProfile* p = FindProfile(image);
    if (!p) return;
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_queued.count(image)) return;
    g_queued.insert(image);
    g_jobs.push_back({image, p->name, p->description, forceRename || GenericName(*p), TakenNames()});
    g_comfyUrl = app.comfy.url;
    if (!g_working.exchange(true)) std::thread(Worker).detach();
}

bool CharacterStoryPending(const std::wstring& image) {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_queued.count(image) > 0;
}

int CharacterStoriesQueued() {
    std::lock_guard<std::mutex> lock(g_mu);
    return static_cast<int>(g_queued.size());
}

bool CharacterNameIsPlaceholder(const CharacterProfile& p) { return GenericName(p); }

void OpenCharacterExtract(AppState& app, const std::wstring& picture) {
    if (picture.empty()) return;
    g_ex.picture = picture;
    g_ex.found.clear();
    g_ex.note.clear();
    ShowCharacterCreator(true);
    SetCharacterCreatorMode(2);
    app.requestTab = 5;
    if (!g_ex.fut.valid()) g_ex.fut = std::async(std::launch::async, FindCharacters, app.comfy.url, picture, TakenNames()); // (looking is not drawing: it starts by itself)
}

// The creator's third way: characters out of a picture
void DrawExtractSection(AppState& app) {
    if (g_ex.fut.valid() && g_ex.fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto [list, note] = g_ex.fut.get();
        g_ex.found = std::move(list);
        g_ex.note = note;
        std::vector<std::string> names;
        for (Found& f : g_ex.found) {
            if (f.name.empty()) f.name = FallbackName(f.creature, names);
            names.push_back(f.name);
        }
    }
    ui::SectionHeading("1. THE PICTURE");
    if (!g_ex.picture.empty()) {
        const AppState::LibThumb t = app.LibraryThumb(g_ex.picture);
        if (t.srv && t.w > 0 && t.h > 0) {
            const float h = ui::Dp(150.0f), w = h * t.w / t.h;
            ImGui::Image((ImTextureID)(intptr_t)t.srv, ImVec2(w, h));
            ImGui::SameLine();
        }
        ImGui::BeginGroup();
        ImGui::TextUnformatted(U8(fs::path(g_ex.picture).filename().wstring()).c_str());
    } else {
        ImGui::BeginGroup();
        ImGui::TextDisabled("Pick a picture with people or creatures in it (or drag one onto this page).");
    }
    if (ImGui::SmallButton("The gallery's picture")) {
        if (app.selectedIndex >= 0 && app.selectedIndex < static_cast<int>(app.entries.size())) OpenCharacterExtract(app, app.entries[app.selectedIndex].path);
        else app.SetStatus("Pick a picture in the gallery first");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("A file...")) {
        const std::wstring f = OpenImageFileDialog(GetActiveWindow());
        if (!f.empty()) OpenCharacterExtract(app, f);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Paste##extract")) {
        std::string msg;
        const std::wstring p = PasteBoardImage(msg);
        if (!p.empty()) OpenCharacterExtract(app, p);
        else app.SetStatus(msg.empty() ? "No picture on the clipboard" : msg);
    }
    if (!g_ex.picture.empty()) {
        ImGui::SameLine();
        ImGui::BeginDisabled(g_ex.fut.valid());
        if (ImGui::SmallButton("Look again")) {
            g_ex.found.clear();
            g_ex.fut = std::async(std::launch::async, FindCharacters, app.comfy.url, g_ex.picture, TakenNames());
        }
        ImGui::EndDisabled();
    }
    ImGui::EndGroup();
    ui::SectionHeading("2. WHO IS IN IT");
    if (g_ex.fut.valid()) ImGui::TextDisabled("The prompt writer is looking at the picture...");
    if (!g_ex.note.empty()) ImGui::TextColored(ui::kWarn, "%s", g_ex.note.c_str());
    for (size_t i = 0; i < g_ex.found.size(); ++i) {
        Found& f = g_ex.found[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::Checkbox("##on", &f.on);
        ImGui::SameLine();
        char nb[64];
        snprintf(nb, sizeof(nb), "%s", f.name.c_str());
        ImGui::SetNextItemWidth(ui::Dp(170.0f));
        if (ImGui::InputText("##name", nb, sizeof(nb))) f.name = nb;
        ImGui::SameLine();
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s%s", f.creature ? "(creature) " : "", f.where.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ui::Dp(380.0f));
            ImGui::TextUnformatted(f.look.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        ImGui::PopID();
    }
    ui::SectionHeading("3. MAKE THEM");
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Make them (below): Qwen Image Edit draws each ticked one alone, full-body, in a white studio, keeping their face, clothes and style; "
                       "each becomes one of your characters and gets a backstory.");
    ImGui::PopStyleColor();
}

int ExtractFoundOn() { return FoundOn(); }
void MakeExtracted(AppState& app) { MakeFound(app); }

// Each frame: drawn characters become saved ones; written backstories go into their profiles
void PumpCharacterStories(AppState& app) {
    { // tests: MV_CHAR_STORY=N writes the N-th saved character's backstory; MV_CHAR_EXTRACT=picture takes its characters out (and makes them)
        static int step = 0;
        wchar_t buf[1024] = {};
        if (step == 0 && GetEnvironmentVariableW(L"MV_CHAR_STORY", buf, 1024)) {
            int k = _wtoi(buf), n = 0;
            for (const CharacterProfile& p : AllProfiles()) {
                if (p.saved && n++ == k) {
                    QueueCharacterStory(app, p.image, false);
                    break;
                }
            }
            step = 1;
        }
        static int ex = 0;
        if (ex == 0 && GetEnvironmentVariableW(L"MV_CHAR_EXTRACT", buf, 1024)) {
            OpenCharacterExtract(app, buf);
            ex = 1;
        } else if (ex == 1 && !g_ex.fut.valid() && !g_ex.found.empty()) {
            ex = 2;
            MakeFound(app);
        }
    }
    if (!g_ex.pending.empty() && app.comfySvc) {
        const std::vector<ComfyActivity> acts = app.comfySvc->Activity();
        for (size_t i = 0; i < g_ex.pending.size();) {
            const auto [id, name] = g_ex.pending[i];
            const ComfyActivity* a = nullptr;
            for (const ComfyActivity& x : acts) {
                if (x.id == id) a = &x;
            }
            if (a && a->state != ComfyState::Done && a->state != ComfyState::Failed) {
                ++i;
                continue;
            }
            const std::string look = g_ex.pendingLook[i];
            g_ex.pending.erase(g_ex.pending.begin() + i);
            g_ex.pendingLook.erase(g_ex.pendingLook.begin() + i);
            std::wstring file;
            if (a && a->state == ComfyState::Done) {
                for (const ComfyResult& r : a->files) {
                    if (!r.localPath.empty() && !r.video) file = r.localPath;
                }
            }
            if (file.empty()) {
                Toast(ToastKind::Error, "Could not take out " + name, a ? a->message : std::string("ComfyUI lost the job"), {}, [](AppState& s) { s.requestTab = 4; }, "charex");
                continue;
            }
            CharacterProfile& p = EnsureProfile(file);
            p.saved = true;
            p.name = Taken(name) ? FallbackName(false, {}) : name;
            std::string d = look;
            if (!d.empty()) d[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(d[0])));
            if (p.description.empty()) p.description = d;
            SaveProfiles();
            QueueCharacterStory(app, file, false);
            Toast(ToastKind::Success, "New character: " + p.name, "Taken out of the picture; the writer is giving them a backstory", file, [](AppState& s) { s.requestTab = 5; }, "charex");
        }
    }
    std::vector<StoryResult> done;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        done.swap(g_done);
        for (const StoryResult& r : done) g_queued.erase(r.image);
    }
    for (const StoryResult& r : done) {
        CharacterProfile* p = FindProfile(r.image);
        if (!p) continue;
        if (!r.error.empty()) {
            app.SetStatus("No backstory for " + p->name + ": " + r.error);
            continue;
        }
        const std::string old = p->name;
        if (!r.name.empty() && !Taken(r.name)) p->name = r.name;
        else if (GenericName(*p)) p->name = FallbackName(false, {});
        p->backstory = r.story;
        SaveProfiles();
        app.SetStatus(r.name.empty() || r.name == old ? "Backstory written: " + p->name : old + " is now " + p->name + ", with a backstory");
    }
}
