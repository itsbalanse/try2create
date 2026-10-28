// Create a character (after Higgsfield's AI Influencer), in the Characters tab: build someone from looks (character type, gender, build, hair,
// style, features... or shuffle), optionally from a photo of you (your face is kept), and Qwen Image 2.1 draws them in a white studio.
// Every character made becomes a SAVED character with a name and a description, so it is there everywhere characters are used: the
// Vision board, Clef mode's cast, picture edits, the Video page (animate it, star it in a video).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include "app_state.h"
#include "board.h"
#include "clef.h"
#include "comfy.h"
#include "json.hpp"
#include "imgui.h"
#include "image_texture.h"   // DecodeImageRGBA
#include "imgui_internal.h" // RenderTextEllipsis
#include "notify.h"
#include "ui_extra.h"
#include "ui_theme.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::string Utf8(const std::wstring& w) {
    std::string out(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), static_cast<int>(out.size()), nullptr, nullptr);
    return out;
}

bool Chip(const char* label, bool on) {
    ImGui::PushStyleColor(ImGuiCol_Button, on ? ui::ChipActiveColor() : ui::ChipColor());
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::ChipHoverColor());
    const bool clicked = ImGui::Button(label);
    ImGui::PopStyleColor(2);
    return clicked;
}

std::wstring PickPicture(const wchar_t* title) {
    wchar_t buf[MAX_PATH * 2] = {};
    OPENFILENAMEW ofn = {sizeof(ofn)};
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = L"Pictures\0*.png;*.jpg;*.jpeg;*.webp;*.bmp\0All files\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&ofn) ? std::wstring(buf) : std::wstring();
}

// A picture fitted into a box
void Fit(AppState& app, ImDrawList* dl, const std::wstring& path, ImVec2 p, ImVec2 size, float round) {
    const AppState::LibThumb lt = app.LibraryThumb(path);
    if (!lt.srv || lt.w <= 0 || lt.h <= 0) return;
    const float s = std::min(size.x / lt.w, size.y / lt.h);
    const ImVec2 a(p.x + (size.x - lt.w * s) * 0.5f, p.y + (size.y - lt.h * s) * 0.5f);
    dl->AddImageRounded((ImTextureID)(intptr_t)lt.srv, a, ImVec2(a.x + lt.w * s, a.y + lt.h * s), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, round);
}

// ---- create a character (AI influencer)
struct Trait {
    const char* group;
    std::vector<const char*> options;
    bool multi = false;
};
const std::vector<Trait>& Traits() {
    static const std::vector<Trait> t = {
        {"Character type", {"Human", "Insect", "Frog", "Cat", "Dog", "Rodent", "Bird", "Lizard", "Alien", "Robot"}},
        {"Look", {"Average", "Attractive", "Bold", "Extreme"}},
        {"Gender", {"Woman", "Man", "Non-binary", "Androgynous"}},
        {"Age", {"Young adult", "Adult", "Middle-aged", "Senior"}},
        {"Build", {"Skinny", "Slim", "Athletic", "Muscular", "Curvy", "Heavy", "Huge bodybuilder"}},
        {"Height", {"Short", "Average height", "Very tall"}},
        {"Proportions", {"Normal", "Big head", "Tiny head", "Long neck", "Lanky limbs", "Broad shoulders"}},
        {"Head shape", {"Round", "Oval", "Square jaw", "Long face", "Egg-shaped", "Pointy chin"}},
        {"Ethnicity", {"African", "East Asian", "South Asian", "Latino", "Middle Eastern", "European"}},
        {"Skin", {"Very fair", "Fair", "Olive", "Tan", "Brown", "Dark", "Freckled", "Pale grey"}},
        {"Hairstyle", {"Bowl cut", "Afro", "Mullet", "Buzz cut", "Long straight", "Curly bob", "Mohawk", "Slicked back", "Braids", "Top knot", "Bald",
                       "Pompadour", "Shaggy", "Pixie cut", "Twin tails", "Dreadlocks", "Wavy shoulder-length", "Undercut", "Bun", "Side part", "Messy bed hair",
                       "Flat top", "Mushroom", "Comb-over", "Victory rolls", "Liberty spikes"}},
        {"Hair color", {"Black", "Dark brown", "Brown", "Blonde", "Platinum", "Red", "Ginger", "Pink", "Green", "Blue", "Purple", "Silver", "White"}},
        {"Eye color", {"Brown", "Hazel", "Green", "Blue", "Grey", "Amber", "Violet", "Heterochromia"}},
        {"Facial hair", {"None", "Stubble", "Mustache", "Handlebar mustache", "Full beard", "Goatee", "Mutton chops", "Soul patch"}},
        {"Style", {"Casual", "Streetwear", "Business suit", "Luxury designer", "Old money", "Worn and shabby", "Work clothes", "Elegant dress", "Retro 70s",
                   "Punk leather", "Sporty tracksuit", "Y2K", "Cyberpunk"}},
        {"Features", {"Freckles", "Gap teeth", "Big nose", "Unibrow", "Scar", "Tattoos", "Piercings", "Dimples", "Big ears", "Rosy cheeks", "Beauty mark",
                      "Bushy eyebrows", "Crooked smile", "Wrinkles"}, true},
        {"Accessories", {"Sunglasses", "Glasses", "Earmuffs", "Gold chain", "Headphones", "Bucket hat"}, true},
    };
    return t;
}

struct Creator {
    std::vector<std::vector<int>> picked; // per group: chosen options (empty = any)
    std::string extra;                    // anything else, in words
    std::string name;                     // what to call them ("" = a name from their looks)
    std::wstring photo;                   // optional: you (the face is kept)
    int framing = 0;                      // 0 full body, 1 portrait
    int openGroup = 0;
};
Creator g_cr;

// Do your own words say the character is a person? ("human gangster", "a woman", "old man") - they win over a creature type picked
bool WordsSayHuman(const std::string& words) {
    std::string w = " ";
    for (char c : words) w += std::isalnum(static_cast<unsigned char>(c)) ? static_cast<char>(std::tolower(static_cast<unsigned char>(c))) : ' ';
    w += " ";
    for (const char* k : {"human", "person", "man", "woman", "guy", "girl", "boy", "lady", "gentleman", "dude", "people", "men", "women"})
        if (w.find(std::string(" ") + k + " ") != std::string::npos) return true;
    return false;
}

std::string CharacterPrompt(std::string* description = nullptr) {
    const auto& traits = Traits();
    auto one = [&](const char* group) -> std::string {
        for (size_t g = 0; g < traits.size(); ++g) {
            if (strcmp(traits[g].group, group) == 0 && g < g_cr.picked.size() && !g_cr.picked[g].empty()) return traits[g].options[g_cr.picked[g].front()];
        }
        return {};
    };
    auto many = [&](const char* group) {
        std::string out;
        for (size_t g = 0; g < traits.size(); ++g) {
            if (strcmp(traits[g].group, group) != 0 || g >= g_cr.picked.size()) continue;
            for (int k : g_cr.picked[g]) out += (out.empty() ? "" : ", ") + std::string(traits[g].options[k]);
        }
        return out;
    };
    auto low = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string type = one("Character type"), look = one("Look"), gender = one("Gender"), age = one("Age"), build = one("Build");
    const bool creature = !type.empty() && type != "Human" && !WordsSayHuman(g_cr.extra); // ("human gangster" typed: a picked animal type goes)
    // a creature: its head is said first and plainly (buried in the middle, the model draws a person)
    static const std::pair<const char*, const char*> kHeads[] = {
        {"Insect", "the head of a giant insect: compound eyes, antennae and mandibles, glossy chitin"},
        {"Frog", "the head of a frog: wide mouth, big bulging eyes, smooth damp green skin"},
        {"Cat", "the head of a cat: fur, whiskers, pointed ears, cat eyes and nose"},
        {"Dog", "the head of a dog: snout, wet nose, floppy ears, fur"},
        {"Rodent", "the head of a capybara-like rodent: fur, buck teeth, small round ears"},
        {"Bird", "the head of a bird: feathers, a beak and round bird eyes"},
        {"Lizard", "the head of a lizard: scales, slit-pupil eyes, a reptile snout"},
        {"Alien", "the head of a grey alien: huge black almond eyes, smooth grey skin, no hair"},
        {"Robot", "the head of a robot: metal plates, rivets and glowing eyes"},
    };
    std::string head;
    for (const auto& [t, h] : kHeads) {
        if (type == t) head = h;
    }
    const std::string who = gender.empty() ? "person" : low(gender == "Androgynous" ? "androgynous person" : gender == "Non-binary" ? "non-binary person" : gender);
    std::string person = low(age.empty() ? "" : age + " ") + low(build.empty() ? "" : build + " ") + (creature ? "" : low(one("Ethnicity").empty() ? "" : one("Ethnicity") + " ")) + who;
    std::string subject;
    if (creature) {
        // no "woman", "senior", "person" here: such words pull the model back to a human face. Male / female only as the clothes' cut.
        const std::string cut = gender == "Woman" ? "female " : gender == "Man" ? "male " : "";
        subject = "a humanoid " + low(type) + " - " + head + " - with a " + low(build.empty() ? "" : build + " ") + cut + "humanoid body";
    } else {
        subject = "a " + person;
    }
    std::vector<std::string> bits;
    if (!one("Height").empty()) bits.push_back(low(one("Height")));
    if (!one("Proportions").empty() && one("Proportions") != "Normal") bits.push_back(low(one("Proportions")));
    if (!one("Head shape").empty() && !creature) bits.push_back(low(one("Head shape")) + " head shape");
    if (!one("Skin").empty() && !creature) bits.push_back(low(one("Skin")) + " skin");
    const std::string hairStyle = one("Hairstyle"), hairColor = one("Hair color");
    if (hairStyle == "Bald") bits.push_back("bald");
    else if (!hairStyle.empty() || !hairColor.empty()) bits.push_back(low(hairColor + (hairColor.empty() ? "" : " ")) + low(hairStyle.empty() ? "hair" : hairStyle + " hair"));
    if (!one("Eye color").empty()) bits.push_back(low(one("Eye color")) + " eyes");
    if (!one("Facial hair").empty() && one("Facial hair") != "None") bits.push_back(low(one("Facial hair")));
    if (!many("Features").empty()) bits.push_back(low(many("Features")));
    static const std::pair<const char*, const char*> kStyles[] = { // what a style looks like, said plainly (a story needs rich and poor to read at a glance)
        {"Luxury designer", "wearing an expensive tailored designer outfit, a luxury watch and polished leather shoes, visibly wealthy"},
        {"Worn and shabby", "wearing worn, faded, patched cheap clothes and scuffed old shoes, visibly poor"},
        {"Work clothes", "wearing dusty work clothes: a faded work jacket, jeans and work boots"},
        {"Elegant dress", "wearing an elegant fitted dress and heels"},
        {"Casual", "wearing casual everyday clothes"},
    };
    if (!one("Style").empty()) {
        std::string wear = "wearing " + low(one("Style")) + " clothes";
        for (const auto& [s, w] : kStyles) {
            if (one("Style") == s) wear = w;
        }
        bits.push_back(wear);
    }
    if (!many("Accessories").empty()) bits.push_back("with " + low(many("Accessories")));
    std::string details;
    for (const std::string& b : bits) details += (details.empty() ? "" : ", ") + b;
    std::string look_;
    if (look == "Attractive") look_ = " Very attractive, like a model: a beautiful symmetrical face, clear skin, bright eyes and a warm, confident smile.";
    if (look == "Bold") look_ = " A striking, characterful face with memorable, slightly exaggerated features.";
    if (look == "Extreme") look_ = " Absurdly exaggerated caricature proportions and features, wildly unusual and meme-worthy, yet fully photorealistic.";
    const std::string frame = g_cr.framing == 0 ? "Full-body photograph, head to shoes in frame, standing straight and facing the camera, arms relaxed"
                                                : "Waist-up portrait photograph, facing the camera, looking into the lens";
    if (description) {
        std::string d = subject + (details.empty() ? "" : ", " + details) + "." + look_;
        if (!d.empty()) d[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(d[0])));
        *description = d;
    }
    std::string extra = g_cr.extra;
    while (!extra.empty() && (extra.back() == ' ' || extra.back() == '.' || extra.back() == '\n')) extra.pop_back();
    std::string p = frame + ", of " + (extra.empty() ? std::string() : extra + ": ") + subject + (details.empty() ? "" : ", " + details) + "." + look_ +
                    " Plain seamless white studio backdrop with a soft floor shadow, bright even studio lighting, sharp focus, photorealistic editorial fashion photo, natural skin texture.";
    // a creature: said first too (what comes first weighs most), and once more at the end
    if (creature) p = "A humanoid " + low(type) + " with a real " + low(type) + " head. " + p + " The head is a real " + low(type) + "'s head, not a human's.";
    return p;
}

void ShuffleCharacter() {
    static std::mt19937 rng{std::random_device{}()};
    const auto& traits = Traits();
    g_cr.picked.assign(traits.size(), {});
    for (size_t g = 0; g < traits.size(); ++g) {
        const int n = static_cast<int>(traits[g].options.size());
        if (traits[g].multi) {
            const int count = std::uniform_int_distribution<int>(0, 2)(rng);
            for (int c = 0; c < count; ++c) {
                const int k = std::uniform_int_distribution<int>(0, n - 1)(rng);
                if (std::find(g_cr.picked[g].begin(), g_cr.picked[g].end(), k) == g_cr.picked[g].end()) g_cr.picked[g].push_back(k);
            }
        } else if (strcmp(traits[g].group, "Character type") == 0) {
            g_cr.picked[g] = {std::uniform_int_distribution<int>(0, 3)(rng) == 0 ? std::uniform_int_distribution<int>(1, n - 1)(rng) : 0}; // mostly human
        } else {
            g_cr.picked[g] = {std::uniform_int_distribution<int>(0, n - 1)(rng)};
        }
    }
}



// A name for a new character from its looks ("Platinum Frog", "Mullet Woman"): renamed in the Characters tab whenever you like
std::string CharacterName() {
    const auto& traits = Traits();
    auto one = [&](const char* group) -> std::string {
        for (size_t g = 0; g < traits.size(); ++g) {
            if (strcmp(traits[g].group, group) == 0 && g < g_cr.picked.size() && !g_cr.picked[g].empty()) return traits[g].options[g_cr.picked[g].front()];
        }
        return {};
    };
    const std::string type = one("Character type");
    std::string base = !type.empty() && type != "Human" ? type : !one("Gender").empty() ? one("Gender") : std::string("Character");
    if (base == "Non-binary" || base == "Androgynous") base = "Person";
    std::string first = one("Hair color");
    if (first.empty()) first = one("Hairstyle");
    if (first.empty()) first = one("Style");
    std::string name = first.empty() ? base : first + " " + base;
    // the same name again: a number
    int n = 1;
    std::string tryName = name;
    for (bool clash = true; clash;) {
        clash = false;
        for (const CharacterProfile& p : AllProfiles()) {
            if (p.saved && p.name == tryName) {
                clash = true;
                tryName = name + " " + std::to_string(++n);
                break;
            }
        }
    }
    return tryName;
}

struct PendingChar {
    int activity = 0;
    std::string name, description;
    bool autoName = false; // the app made the name up from the looks: the writer gives them a real one
};

// ---- Who draws a character. Compared on 2026-10-16 (the same six prompts, seeds and 880 x 1184, RTX 3090): Krea 2 Turbo draws people
// clearly more like photographs (sharp faces: scars, skin, stubble) and faster (7.9 s against 12.7 s); Qwen Image 2.1 follows the words
// more literally and draws creatures and caricatures far better (a frog-man's head grows out of the body - Krea's looked like a rubber
// mask - and "absurdly long thin legs" really are). So: Krea for people, Qwen for creatures and extreme looks, unless you pick one.
int g_engine = 0; // 0 auto, 1 Qwen Image 2.1, 2 Krea 2, 3 MOSS-Image (fast, softer faces; only when picked)

// The Krea 2 Turbo model on disk ("" = none): the official int8 first, else another Krea 2 text-to-image one (not the GGUF edit models)
std::string KreaModelFile() {
    static std::string found;
    static ULONGLONG at = 0;
    if (at && GetTickCount64() - at < 30000) return found;
    at = GetTickCount64();
    found.clear();
    const std::wstring models = ComfyModelsDir();
    if (models.empty()) return found;
    std::error_code ec;
    if (!fs::exists(fs::path(models) / L"text_encoders" / L"qwen3vl_4b_fp8_scaled.safetensors", ec) || !fs::exists(fs::path(models) / L"vae" / L"qwen_image_vae.safetensors", ec))
        return found;
    for (const wchar_t* sub : {L"diffusion_models", L"unet"}) {
        if (fs::exists(fs::path(models) / sub / L"krea2_turbo_int8_convrot.safetensors", ec)) return found = "krea2_turbo_int8_convrot.safetensors";
    }
    for (const wchar_t* sub : {L"diffusion_models", L"unet"}) {
        for (const auto& e : fs::directory_iterator(fs::path(models) / sub, ec)) {
            std::string n = Utf8(e.path().filename().wstring()), low = n;
            for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (low.find("krea2") != std::string::npos && low.find("edit") == std::string::npos && e.path().extension() == L".safetensors") return found = n;
        }
    }
    return found;
}

std::string CharacterFrame() {
    return g_cr.framing == 0 ? "Full-body photograph, head to shoes in frame, standing straight and facing the camera, arms relaxed, of "
                             : "Waist-up portrait photograph, facing the camera, looking into the lens, of ";
}
const char* const kStudio = " Plain seamless white studio backdrop with a soft floor shadow, bright even studio lighting, sharp focus, photorealistic "
                            "editorial fashion photo, natural skin texture.";

// Draws one character picture (saved under Characters/): the activity id, 0 = not sent. `unusual`: a creature or a caricature (Qwen's).
int SendCharacterDraw(AppState& app, const std::string& described, bool unusual, std::string& engine) {
    if (g_engine == 3) {
        engine = "MOSS-Image";
        return SendMossPrompt(app, WithLoraTriggers(app, described, app.comfy.mossLoras), 880, g_cr.framing == 0 ? 1184 : 1104, 1, "Characters/character",
                              app.comfy.mossLoras);
    }
    const std::string krea = KreaModelFile();
    const bool useKrea = !krea.empty() && (g_engine == 2 || (g_engine == 0 && !unusual));
    const std::string prompt = WithLoraTriggers(app, described, useKrea ? app.comfy.kreaLoras : app.comfy.qwenLoras); // (their trigger words)
    if (useKrea) {
        const int w = 880, h = g_cr.framing == 0 ? 1184 : 1104;
        json g = {
            {"1", {{"class_type", "UNETLoader"}, {"inputs", {{"unet_name", krea}, {"weight_dtype", "default"}}}}},
            {"2", {{"class_type", "CLIPLoader"}, {"inputs", {{"clip_name", "qwen3vl_4b_fp8_scaled.safetensors"}, {"type", "krea2"}, {"device", "default"}}}}},
            {"3", {{"class_type", "VAELoader"}, {"inputs", {{"vae_name", "qwen_image_vae.safetensors"}}}}},
            {"4", {{"class_type", "CLIPTextEncode"}, {"inputs", {{"clip", json::array({"2", 0})}, {"text", prompt}}}}},
            {"5", {{"class_type", "ConditioningZeroOut"}, {"inputs", {{"conditioning", json::array({"4", 0})}}}}},
            {"6", {{"class_type", "EmptyLatentImage"}, {"inputs", {{"width", w}, {"height", h}, {"batch_size", 1}}}}},
            {"7", {{"class_type", "KSampler"}, {"inputs", {{"model", json::array({"1", 0})}, {"positive", json::array({"4", 0})}, {"negative", json::array({"5", 0})},
                                                          {"latent_image", json::array({"6", 0})}, {"seed", 1}, {"steps", 8}, {"cfg", 1.0}, {"sampler_name", "euler"},
                                                          {"scheduler", "simple"}, {"denoise", 1.0}}}}},
            {"8", {{"class_type", "VAEDecode"}, {"inputs", {{"samples", json::array({"7", 0})}, {"vae", json::array({"3", 0})}}}}},
            {"9", {{"class_type", "SaveImage"}, {"inputs", {{"images", json::array({"8", 0})}, {"filename_prefix", "Characters/character"}}}}},
        };
        ComfyJob job;
        job.apiWorkflow = g.dump(); // (sent with a new seed)
        job.label = "Character (Krea 2)";
        job.times = 1;
        job.loras = app.comfy.kreaLoras;
        engine = "Krea 2";
        return SendComfyJob(app, std::move(job));
    }
    ComfyJob job;
    job.label = "Character";
    job.savePrefix = "Characters/character";
    job.draw = 3;
    job.prompt = prompt;
    job.pixW = 3, job.pixH = 4; // portrait shape, about one megapixel
    job.times = 1;
    job.loras = app.comfy.qwenLoras;
    engine = "Qwen Image 2.1";
    return SendSheetEdit(app, std::move(job));
}

// ---- Character sheets (after the "Simple" workflow of the Qwen Image 2.1 character-sheet set, its static prompt): one picture of a character
// becomes a 3:2 sheet - four expressions down the left, then front, side and back views - drawn by Qwen Image 2.1 Edit from the picture.
// It is kept with the character (CharacterProfile::sheet) and used as an extra reference where it helps (see the tests in the README).
const char* const kSheetPrompt =
    "Composition: Clean 3:2 landscape layout on a warm off-white background with generous spacing. No frames, borders, dividers, text, labels, "
    "watermarks, or logos. Soft, even neutral studio lighting across all panels. Rendering style, line weight, texture, and shading must strictly "
    "match the reference image.\n"
    "Left Column (20% width): 4 stacked, identically scaled and cropped head-and-shoulders portraits of the reference subject:\n"
    "1. Neutral: Relaxed brows, open eyes, closed mouth (identity anchor).\n"
    "2. Joy: Gentle closed-mouth smile, relaxed cheeks, open eyes (no strain/crinkles).\n"
    "3. Anger: Brows drawn down/inward, narrowed intense eyes, firmly pressed lips, tight jaw.\n"
    "4. Sorrow: Inner brows raised, heavy drooping eyelids, downturned mouth, relaxed jaw (distinct from anger).\n"
    "Right Row (80% width): 3 full-height, identically scaled figures of the reference subject in neutral relaxed stance: Front View, Clean Side "
    "Profile, and Back View. Soft oval floor shadows.\n"
    "Consistency & Constraints: Preserve exact identity, hair, facial features, garments, asymmetry, and accessories from the reference. Do not add, "
    "redesign, beautify, age, or extrapolate items based on lore/profession. No extra characters, shifting proportions, extra limbs, or style drift.";

struct PendingSheet {
    int activity = 0;
    std::wstring image; // the character it is for
};
std::vector<PendingSheet> g_pendingSheets;

bool SheetPending(const std::wstring& image) {
    for (const PendingSheet& p : g_pendingSheets) {
        if (_wcsicmp(p.image.c_str(), image.c_str()) == 0) return true;
    }
    return false;
}

// Qwen Image 2.1 Edit from the picture: the model picture editing uses (a turbo: 6 steps euler; the full model: 25 steps res_multistep/beta,
// the workflow's), the reference at about 2.4 MP and seen by the encoder at 1536, a 2272 x 1504 canvas (3:2, 3.4 MP: the workflow's "balanced")
void MakeCharacterSheet(AppState& app, const std::wstring& image) {
    if (image.empty() || SheetPending(image)) return;
    std::string unet = app.comfy.editUnet.empty() ? std::string("qwen_image_2.1_turbo_Q8_0.gguf") : app.comfy.editUnet;
    std::string low = unet;
    for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const bool gguf = low.size() > 5 && low.compare(low.size() - 5, 5, ".gguf") == 0, turbo = low.find("turbo") != std::string::npos;
    json g = {
        {"1", gguf ? json{{"class_type", "UnetLoaderGGUF"}, {"inputs", {{"unet_name", unet}}}}
                   : json{{"class_type", "UNETLoader"}, {"inputs", {{"unet_name", unet}, {"weight_dtype", "default"}}}}},
        {"2", {{"class_type", "CLIPLoader"}, {"inputs", {{"clip_name", "qwen3vl_8b_int8_convrot.safetensors"}, {"type", "qwen_image"}, {"device", "default"}}}}},
        {"3", {{"class_type", "VAELoader"}, {"inputs", {{"vae_name", "qwen_image_2.1_vae_bf16.safetensors"}}}}},
        {"4", {{"class_type", "QwenImage21Cache"}, {"inputs", {{"model", json::array({"1", 0})}, {"device", "auto"}, {"dtype", "default"}}}}},
        {"5", {{"class_type", "ModelAttentionBackend"}, {"inputs", {{"model", json::array({"4", 0})}, {"attention", "comfy kitchen attention"}}}}},
        {"10", {{"class_type", "LoadImage"}, {"inputs", {{"image", "@ref"}}}}},
        {"11", {{"class_type", "ImageScaleToTotalPixels"}, {"inputs", {{"image", json::array({"10", 0})}, {"upscale_method", "lanczos"}, {"megapixels", 2.36}, {"resolution_steps", 32}}}}},
        {"20", {{"class_type", "TextEncodeQwenImage21"}, {"inputs", {{"clip", json::array({"2", 0})}, {"vae", json::array({"3", 0})}, {"prompt", kSheetPrompt},
                                                                  {"negative_prompt", ""}, {"resolution", 1536}, {"images.image_1", json::array({"11", 0})}}}}},
        {"21", {{"class_type", "EmptyLatentImage"}, {"inputs", {{"width", 2272}, {"height", 1504}, {"batch_size", 1}}}}},
        {"30", {{"class_type", "KSampler"}, {"inputs", {{"model", json::array({"5", 0})}, {"positive", json::array({"20", 0})}, {"negative", json::array({"20", 1})},
                                                       {"latent_image", json::array({"21", 0})}, {"seed", 1}, {"steps", turbo ? 6 : 25}, {"cfg", 1.0},
                                                       {"sampler_name", turbo ? "euler" : "res_multistep"}, {"scheduler", turbo ? "simple" : "beta"}, {"denoise", 1.0}}}}},
        {"31", {{"class_type", "VAEDecode"}, {"inputs", {{"samples", json::array({"30", 0})}, {"vae", json::array({"3", 0})}}}}},
        {"32", {{"class_type", "SaveImage"}, {"inputs", {{"images", json::array({"31", 0})}, {"filename_prefix", "Characters/sheet"}}}}},
    };
    ComfyJob job;
    job.apiWorkflow = g.dump();
    job.rawUploads = {{"@ref", image}};
    const CharacterProfile* cp = FindProfile(image);
    job.label = "Character sheet: " + (cp && !cp->name.empty() ? cp->name : Utf8(fs::path(image).stem().wstring()));
    job.times = 1;
    const int id = SendComfyJob(app, std::move(job));
    if (!id) return;
    g_pendingSheets.push_back({id, image});
    Toast(ToastKind::Info, "Making a character sheet", (cp ? cp->name : std::string()) + ": front, side and back views and four expressions (Qwen Image 2.1, about a minute)",
          {}, [](AppState& s) { s.requestTab = 5; }, "charsheet");
}

// Is a sheet worth using? Tested 2026-10-16 (reference-to-video, three characters): a sheet helped only the one whose back looks nothing like
// its front (a warrior whose cape hides her armour: likeness 0.865 -> 0.883, worst frame 0.712 -> 0.767); for a woman in a bomber jacket it
// changed nothing and for a man in a long coat it was worse (0.887 -> 0.866) - their picture already says how they look from behind. So the
// sheet's front figure and back figure are compared by colour (hue/saturation/value histograms of the figure, not the paper): how much they
// overlap, 1 = the same. Measured: man 0.85, woman 0.70, warrior 0.38. Below 0.55 the back shows something new: the sheet is used.
constexpr double kSheetBackDiffers = 0.55;

double SheetFrontBackOverlap(const std::wstring& sheet) {
    std::vector<unsigned char> rgba;
    int w = 0, h = 0;
    std::string err;
    if (!DecodeImageRGBA(sheet, rgba, w, h, &err) || w < 100 || h < 100) return -1.0;
    auto hist = [&](double x0, double x1) {
        std::vector<double> bins(12 * 6 * 6, 0.0);
        double total = 0;
        const int a = static_cast<int>(w * x0), b = static_cast<int>(w * x1);
        for (int y = 0; y < h; y += 2) {
            for (int x = a; x < b; x += 2) {
                const unsigned char* p = &rgba[(static_cast<size_t>(y) * w + x) * 4];
                const double r = p[0] / 255.0, g = p[1] / 255.0, bl = p[2] / 255.0;
                const double mx = std::max({r, g, bl}), mn = std::min({r, g, bl}), d = mx - mn;
                const double s = mx > 0 ? d / mx : 0, v = mx;
                if (v * 255.0 >= 215.0 && s * 255.0 <= 40.0) continue; // the off-white paper
                double hue = 0;
                if (d > 0) hue = mx == r ? std::fmod((g - bl) / d, 6.0) : mx == g ? (bl - r) / d + 2.0 : (r - g) / d + 4.0;
                hue = std::fmod(hue * 60.0 + 360.0, 360.0);
                const int hi = std::min(11, static_cast<int>(hue / 30.0)), si = std::min(5, static_cast<int>(s * 6.0)), vi = std::min(5, static_cast<int>(v * 6.0));
                bins[(hi * 6 + si) * 6 + vi] += 1;
                total += 1;
            }
        }
        for (double& v : bins) v /= std::max(1.0, total);
        return bins;
    };
    const std::vector<double> front = hist(0.22, 0.48), back = hist(0.72, 0.99); // the sheet's layout: portraits, then front, side, back
    double overlap = 0;
    for (size_t i = 0; i < front.size(); ++i) overlap += std::min(front[i], back[i]);
    return overlap;
}

// Measures a character's sheet once (kept in its profile): 1 = use it, 0 = the picture says enough
int MeasureSheetNeeded(CharacterProfile& p) {
    if (p.sheet.empty()) return 0;
    if (p.sheetNeeded >= 0) return p.sheetNeeded;
    const double overlap = SheetFrontBackOverlap(p.sheet);
    if (overlap < 0) return 0; // (could not read it: the picture)
    p.sheetNeeded = overlap < kSheetBackDiffers ? 1 : 0;
    SaveProfiles();
    return p.sheetNeeded;
}

void PumpCharacterSheets(AppState& app) {
    if (g_pendingSheets.empty() || !app.comfySvc) return;
    const std::vector<ComfyActivity> acts = app.comfySvc->Activity();
    for (size_t i = 0; i < g_pendingSheets.size();) {
        const PendingSheet ps = g_pendingSheets[i];
        const ComfyActivity* a = nullptr;
        for (const ComfyActivity& x : acts) {
            if (x.id == ps.activity) a = &x;
        }
        if (a && a->state != ComfyState::Done && a->state != ComfyState::Failed) {
            ++i;
            continue;
        }
        g_pendingSheets.erase(g_pendingSheets.begin() + i);
        std::wstring file;
        if (a && a->state == ComfyState::Done) {
            for (const ComfyResult& r : a->files) {
                if (!r.localPath.empty()) {
                    file = r.localPath;
                    break;
                }
            }
        }
        CharacterProfile* p = FindProfile(ps.image);
        if (file.empty() || !p) {
            Toast(ToastKind::Error, "Could not make the character sheet", a ? a->message : std::string("ComfyUI lost the job"), {}, [](AppState& s) { s.requestTab = 4; }, "charsheet");
            continue;
        }
        p->sheet = file;
        p->sheetNeeded = -1;
        const int needed = MeasureSheetNeeded(*p);
        SaveProfiles();
        Toast(ToastKind::Success, "Character sheet ready: " + p->name,
              needed ? "Its back looks unlike its front, so the sheet is used for its Recreate videos"
                     : "Its picture already shows how it looks from every side, so videos and edits keep using the picture (the sheet is there to look at)",
              file, {}, "charsheet");
    }
}

// "Platinum Frog" again: "Platinum Frog 2"
std::string UniqueCharacterName(const std::string& base, const std::vector<std::string>& alsoTaken = {}) {
    std::string tryName = base.empty() ? std::string("Character") : base;
    for (int n = 1;; ++n) {
        bool clash = std::find(alsoTaken.begin(), alsoTaken.end(), tryName) != alsoTaken.end();
        for (const CharacterProfile& p : AllProfiles()) clash = clash || (p.saved && p.name == tryName);
        if (!clash) return tryName;
        static const char* const kLast[] = {"Ashdown", "Blackwood", "Calder", "Drummond", "Everly", "Fairbrook", "Hawthorne", "Ironside", "Kestrel",
                                            "Larkin", "Marlowe", "Northcott", "Pembrook", "Rookwood", "Sterling", "Thorne", "Vance", "Whitlock", "Yarrow",
                                            "Moreau", "Okafor", "Nakamura", "Castillo", "Petrov", "Lindqvist", "Ferreira", "Haddad", "Kowalski", "Varga"};
        tryName = base + " " + kLast[(n - 1) % (sizeof(kLast) / sizeof(kLast[0]))]; // ("Lyra Thorne", not "Lyra 2")
    }
}
std::vector<PendingChar> g_pendingChars;
std::wstring g_chosenChar; // the character picked on the creator page (its actions)
bool g_showCreator = false;

const char* const kMergeSystem =
    "You write the look of ONE character for a full-body studio photograph. You get the looks the user picked from lists, and the user's own words. "
    "The user's words come first: whatever they say wins over any pick that disagrees (\"human\" or a human role - gangster, nurse, CEO, knight - "
    "wins over an animal, robot or alien type; their clothes over a picked style), and you add what their words imply (a gangster: the clothes, "
    "jewellery and attitude a gangster has). Keep every pick that does not disagree with their words.\n"
    "Reply with exactly one line and nothing else:\nKIND | look\n"
    "KIND: human, or creature (only when the character is not an ordinary human). look: 40 to 80 words that follow the words \"a full-body "
    "photograph of\": what they are first (a creature: \"a humanoid <animal> with a real <animal> head\"), age and sex, build, face, hair, skin, "
    "then the outfit from top to bottom with colours and materials, shoes, and what they hold. Concrete and visual, no backstory. Clothed.";

// (a worker thread) the look written from the picks and your words: {look, creature}; look "" = the writer could not (the picks and words are used as they are)
std::pair<std::string, bool> WriteCharacterLook(std::string url, std::string picks, std::string words) {
    if (!WriterConfigured()) return {std::string(), false};
    const ClefSession session = WriterAcquire(url, [](const std::string&) {});
    if (!session.ok) return {std::string(), false};
    std::string text, err;
    const json schema = {{"type", "object"},
                         {"properties", {{"kind", {{"type", "string"}, {"enum", {"human", "creature"}}}}, {"look", {{"type", "string"}, {"minLength", 200}, {"maxLength", 700}}}}},
                         {"required", {"kind", "look"}}};
    const std::string user = "The user's own words (they win): " + words + "\nPicked from the lists: " + (picks.empty() ? std::string("nothing") : picks);
    bool ok = WriterChatJson(std::string(kMergeSystem) + "\nReply as JSON: kind, and look (the words that follow \"a full-body photograph of\").", user, {}, schema.dump(), 700, 0.7, text, &err);
    if (ok) {
        const json a = json::parse(text, nullptr, false);
        if (a.is_object() && a.contains("look") && a["look"].is_string()) text = a.value("kind", std::string("human")) + " | " + a["look"].get<std::string>();
    } else {
        ok = WriterChat(kMergeSystem, user, {}, 600, 0.7, text, &err);
    }
    WriterRelease(session, false);
    if (!ok) return {std::string(), false};
    std::string line;
    std::istringstream in(text);
    for (std::string l; std::getline(in, l);)
        if (l.find('|') != std::string::npos && l.size() > 40) line = l;
    if (line.empty()) line = text;
    const size_t bar = line.find('|');
    std::string kind = bar == std::string::npos ? std::string() : line.substr(0, bar);
    std::string look = bar == std::string::npos ? line : line.substr(bar + 1);
    while (!look.empty() && strchr(" \t\r\"*", look.front())) look.erase(look.begin());
    while (!look.empty() && strchr(" \t\r\".*", look.back())) look.pop_back();
    for (const char* lead : {"a full-body photograph of ", "full-body photograph of ", "a full body photograph of ", "look: "})
        if (_strnicmp(look.c_str(), lead, strlen(lead)) == 0) look = look.substr(strlen(lead));
    for (char& c : kind) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (look.size() < 30) return {std::string(), false};
    std::string lowLook = look;
    for (char& c : lowLook) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return {look, (kind.find("creature") != std::string::npos || lowLook.find("humanoid") != std::string::npos) && !WordsSayHuman(words)};
}

struct LookWrite { // a character waiting for its look to be written
    std::future<std::pair<std::string, bool>> fut;
    std::string name, fallbackPrompt, fallbackDescription;
    bool autoName = false, fallbackUnusual = false;
    int framing = 0;
};
std::vector<std::unique_ptr<LookWrite>> g_lookWrites;

void SendCharacterNow(AppState& app, const std::string& name, bool autoName, const std::string& prompt, const std::string& description, bool unusual);

void PumpLookWrites(AppState& app) {
    for (size_t i = 0; i < g_lookWrites.size();) {
        LookWrite& w = *g_lookWrites[i];
        if (w.fut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++i;
            continue;
        }
        std::pair<std::string, bool> r;
        try {
            r = w.fut.get();
        } catch (...) {
        }
        if (r.first.empty()) {
            SendCharacterNow(app, w.name, w.autoName, w.fallbackPrompt, w.fallbackDescription, w.fallbackUnusual);
        } else {
            std::string d = r.first + ".";
            d[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(d[0])));
            const std::string frame = w.framing == 0 ? "Full-body photograph, head to shoes in frame, standing straight and facing the camera, arms relaxed, of "
                                                     : "Waist-up portrait photograph, facing the camera, looking into the lens, of ";
            SendCharacterNow(app, w.name, w.autoName, frame + r.first + "." + kStudio, d, r.second);
        }
        g_lookWrites.erase(g_lookWrites.begin() + i);
    }
}

void MakeCharacter(AppState& app) {
    std::string description;
    const std::string prompt = CharacterPrompt(&description);
    // your words typed (and no photo of you): the prompt writer writes the look from the picks and the words, the words winning
    if (!g_cr.extra.empty() && g_cr.photo.empty() && WriterConfigured()) {
        const std::string words = g_cr.extra;
        std::string picks; // (the looks picked, without your words; an animal type dropped when your words say human)
        CharacterPrompt(&picks);
        auto w = std::make_unique<LookWrite>();
        w->autoName = g_cr.name.empty();
        w->name = g_cr.name.empty() ? CharacterName() : g_cr.name;
        g_cr.name.clear();
        w->fallbackPrompt = prompt, w->fallbackDescription = description;
        w->framing = g_cr.framing;
        const auto& traits = Traits();
        for (size_t g = 0; g < traits.size() && g < g_cr.picked.size(); ++g)
            for (int k : g_cr.picked[g])
                if ((strcmp(traits[g].group, "Character type") == 0 && strcmp(traits[g].options[k], "Human") != 0 && !WordsSayHuman(words)) ||
                    (strcmp(traits[g].group, "Look") == 0 && strcmp(traits[g].options[k], "Extreme") == 0))
                    w->fallbackUnusual = true;
        w->fut = std::async(std::launch::async, WriteCharacterLook, app.comfy.url, picks, words);
        g_lookWrites.push_back(std::move(w));
        Toast(ToastKind::Info, "Writing the look", "The prompt writer puts your words (\"" + words + "\") together with the looks picked; then it is drawn", {},
              [](AppState& s) { s.requestTab = 5; }, "charlook");
        return;
    }
    ComfyJob job;
    job.label = "Character";
    job.savePrefix = "Characters/character";
    if (!g_cr.photo.empty()) { // you, restyled: the face is kept
        job.images = {g_cr.photo};
        job.canvas = 3;
        job.prompt = "Make a new photograph of the person in <image1>, keeping their face and identity exactly. " + prompt;
        job.sheetW = 768, job.sheetH = 1024;
    }
    job.times = 1;
    PendingChar pc;
    pc.autoName = g_cr.name.empty();
    pc.name = g_cr.name.empty() ? CharacterName() : g_cr.name;
    g_cr.name.clear(); // the next one gets its own
    pc.description = description;
    std::string engine = "Qwen Image 2.1";
    if (!g_cr.photo.empty()) {
        pc.activity = SendSheetEdit(app, std::move(job));
    } else {
        const auto& traits = Traits();
        bool unusual = false; // a creature, or an extreme caricature: Qwen's
        for (size_t g = 0; g < traits.size() && g < g_cr.picked.size(); ++g) {
            for (int k : g_cr.picked[g]) {
                if (strcmp(traits[g].group, "Character type") == 0 && strcmp(traits[g].options[k], "Human") != 0) unusual = true;
                if (strcmp(traits[g].group, "Look") == 0 && strcmp(traits[g].options[k], "Extreme") == 0) unusual = true;
            }
        }
        pc.activity = SendCharacterDraw(app, prompt, unusual, engine);
    }
    g_pendingChars.push_back(pc);
    Toast(ToastKind::Info, "Making " + pc.name, engine + " draws it; then it is one of your characters everywhere", {}, [](AppState& s) { s.requestTab = 5; }, "charmake");
}

void SendCharacterNow(AppState& app, const std::string& name, bool autoName, const std::string& prompt, const std::string& description, bool unusual) {
    PendingChar pc;
    pc.autoName = autoName, pc.name = name, pc.description = description;
    std::string engine = "Qwen Image 2.1";
    pc.activity = SendCharacterDraw(app, prompt, unusual, engine);
    if (!pc.activity) return;
    g_pendingChars.push_back(pc);
    Toast(ToastKind::Info, "Making " + pc.name, engine + " draws it; then it is one of your characters everywhere", {}, [](AppState& s) { s.requestTab = 5; }, "charmake");
}

// ---- Invent a cast from an idea ("medieval, strong, weird"): the prompt writer makes up several different characters, you look at the
// list (untick, rename), and Make them sends them all to be drawn. Nothing is drawn before you press it.
struct Invented {
    std::string name, look;
    bool unusual = false; // a creature or a caricature (drawn by Qwen in Auto)
    bool on = true;
};
std::string g_idea;
int g_inventCount = 0; // 0 = the writer decides (4 to 8)
std::future<std::pair<std::vector<Invented>, std::string>> g_inventFut; // the list, or why there is none
std::vector<Invented> g_invented;
std::string g_inventNote;

const char* const kInventSystem =
    "You invent characters for full-body studio photographs. Reply with one character per line and nothing else, in this form:\n"
    "Name | KIND | look\n"
    "Name: an uncommon, memorable name that fits who they are and their world (first and last name for people), never a famous character's. KIND: human, or creature (anything that is not an ordinary human: animals, monsters, robots, "
    "aliens, extremely exaggerated caricatures). look: 35 to 70 words that follow the words \"a full-body photograph of\": what they are first "
    "(for a creature: \"a humanoid <animal> with a real <animal> head, ...\"), apparent age and sex, build and height, face, hair, skin, then the "
    "outfit from top to bottom with colours and materials, the shoes, and what they hold or carry. Concrete and visual: no backstory, no "
    "personality words. Make every character clearly different from the others - vary sex, age, build, skin tone and ethnicity, outfits and "
    "colours - unless the idea says otherwise. Follow the idea closely: creatures only when the idea asks for them (gangsters are people). Keep it "
    "suitable for everyone: clothed, no gore.";

std::pair<std::vector<Invented>, std::string> InventCast(std::string url, std::string idea, int count) {
    std::vector<Invented> out;
    if (!WriterConfigured()) return {out, "The prompt writer is not set up (Clef mode > Prompt writer)."};
    const ClefSession session = WriterAcquire(url, [](const std::string&) {});
    if (!session.ok) return {out, "The prompt writer could not be started."};
    std::string takenNames;
    for (const CharacterProfile& p : AllProfiles()) {
        if (p.saved && !p.name.empty()) takenNames += (takenNames.empty() ? "" : ", ") + p.name;
    }
    const std::string user = "Taken names (never use these, nor near copies): " + (takenNames.empty() ? std::string("none") : takenNames) + "\nIdea: " + idea + "\nHow many: " +
                             (count > 0 ? "exactly " + std::to_string(count) : std::string("as many as suits the idea, between 4 and 8")) + ".";
    std::string text, err;
    const bool ok = WriterChat(kInventSystem, user, {}, 2400, 0.9, text, &err);
    WriterRelease(session, false);
    if (!ok) return {out, "The prompt writer did not answer" + (err.empty() ? std::string(".") : ": " + err)};
    std::istringstream in(text);
    std::vector<std::string> taken;
    for (std::string line; std::getline(in, line);) {
        std::vector<std::string> parts;
        for (size_t a = 0;;) {
            const size_t b = line.find('|', a);
            std::string p = line.substr(a, b == std::string::npos ? std::string::npos : b - a);
            while (!p.empty() && strchr(" \t\r*-#\"", p.front())) p.erase(p.begin());
            while (!p.empty() && strchr(" \t\r*\"", p.back())) p.pop_back();
            parts.push_back(p);
            if (b == std::string::npos) break;
            a = b + 1;
        }
        if (parts.size() < 2 || parts.back().size() < 30) continue; // not a character line
        Invented c;
        c.look = parts.back();
        for (const char* lead : {"a full-body photograph of ", "full-body photograph of ", "a full body photograph of ", "look: ", "Look: "}) {
            if (_strnicmp(c.look.c_str(), lead, strlen(lead)) == 0) c.look = c.look.substr(strlen(lead));
        }
        for (std::string* f : {&parts[0], &parts[1]}) { // "Name: Gorm", "KIND: human"
            const size_t colon = f->find(':');
            if (colon != std::string::npos && colon < 8) *f = f->substr(colon + 1);
            while (!f->empty() && f->front() == ' ') f->erase(f->begin());
        }
        while (!c.look.empty() && (c.look.back() == '.' || c.look.back() == ' ')) c.look.pop_back();
        std::string kind = parts.size() >= 3 ? parts[1] : std::string();
        for (char& ch : kind) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        std::string lookLow = c.look;
        for (char& ch : lookLow) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        c.unusual = kind.find("creature") != std::string::npos || lookLow.find("humanoid") != std::string::npos;
        c.name = UniqueCharacterName(parts[0].size() > 40 ? parts[0].substr(0, 40) : parts[0], taken);
        taken.push_back(c.name);
        out.push_back(std::move(c));
        if (count > 0 && static_cast<int>(out.size()) >= count) break;
    }
    if (out.empty()) return {out, "The prompt writer wrote no characters: try again, or put it in other words."};
    return {out, std::string()};
}

void MakeInvented(AppState& app) {
    int sent = 0;
    std::string engines;
    for (Invented& c : g_invented) {
        if (!c.on) continue;
        PendingChar pc;
        pc.name = c.name;
        std::string d = c.look + ".";
        d[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(d[0])));
        pc.description = d;
        std::string engine;
        pc.activity = SendCharacterDraw(app, CharacterFrame() + c.look + "." + kStudio, c.unusual, engine);
        if (!pc.activity) continue;
        g_pendingChars.push_back(pc);
        if (engines.find(engine) == std::string::npos) engines += (engines.empty() ? "" : " and ") + engine;
        ++sent;
    }
    if (sent) {
        Toast(ToastKind::Info, "Making " + std::to_string(sent) + (sent == 1 ? " character" : " characters"), engines + " draws them; each becomes one of your characters",
              {}, [](AppState& s) { s.requestTab = 5; }, "charmake");
        g_invented.clear();
    }
}

int g_crMode = 0; // the creator: 0 = design one character, 1 = invent a cast from an idea, 2 = out of a picture

int InventedOn() {
    int on = 0;
    for (const Invented& c : g_invented) on += c.on ? 1 : 0;
    return on;
}

void DrawInventSection(AppState& app) {
    ui::SectionHeading("1. YOUR IDEA");
    ImGui::PushStyleColor(ImGuiCol_Text, ui::kTextDim);
    ImGui::TextWrapped("A few words about the kind of characters you want - a theme, a world, a mood. The prompt writer makes up a cast of "
                       "clearly different people (or creatures) for it, each with a name and a look.");
    ImGui::PopStyleColor();
    const bool busy = g_inventFut.valid();
    if (busy && g_inventFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto [list, note] = g_inventFut.get();
        g_invented = std::move(list);
        g_inventNote = note;
    }
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", g_idea.c_str());
    ImGui::SetNextItemWidth(std::max(ui::Dp(160.0f), ImGui::GetContentRegionAvail().x - ui::Dp(240.0f)));
    const bool enter = ImGui::InputTextWithHint("##idea", "e.g. medieval, strong, weird   /   cyberpunk street gang   /   a family of frogs", buf, sizeof(buf),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
    g_idea = buf;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ui::Dp(100.0f));
    const std::string countLabel = g_inventCount == 0 ? std::string("4-8") : std::to_string(g_inventCount);
    if (ImGui::BeginCombo("##inventn", countLabel.c_str())) {
        if (ImGui::Selectable("4-8 (it decides)", g_inventCount == 0)) g_inventCount = 0;
        for (int n = 1; n <= 12; ++n) {
            if (ImGui::Selectable(std::to_string(n).c_str(), g_inventCount == n)) g_inventCount = n;
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("How many characters");
    ImGui::SameLine();
    ImGui::BeginDisabled(g_inventFut.valid() || g_idea.find_first_not_of(' ') == std::string::npos);
    if (ui::PrimaryButton(g_inventFut.valid() ? "Inventing..." : "Invent them") || (enter && !g_inventFut.valid() && g_idea.find_first_not_of(' ') != std::string::npos)) {
        g_inventNote.clear();
        g_inventFut = std::async(std::launch::async, InventCast, app.comfy.url, g_idea, g_inventCount);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("The prompt writer makes up different characters for the idea. You see the list first; Make them draws them.");
    if (!g_inventNote.empty()) ImGui::TextColored(ui::kWarn, "%s", g_inventNote.c_str());
    ui::SectionHeading("2. CHECK THE LIST");
    if (g_invented.empty()) {
        ImGui::TextDisabled(g_inventFut.valid() ? "The prompt writer is making them up..." : "The characters it invents show here: untick the ones you don't want, rename any.");
        return;
    }
    int on = 0;
    for (size_t i = 0; i < g_invented.size(); ++i) {
        Invented& c = g_invented[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::Checkbox("##on", &c.on);
        ImGui::SameLine();
        char nb[64];
        snprintf(nb, sizeof(nb), "%s", c.name.c_str());
        ImGui::SetNextItemWidth(ui::Dp(150.0f));
        if (ImGui::InputText("##name", nb, sizeof(nb))) c.name = nb;
        ImGui::SameLine();
        const bool krea = !KreaModelFile().empty() && (g_engine == 2 || (g_engine == 0 && !c.unusual));
        ImGui::TextDisabled("%s", g_engine == 3 ? "MOSS" : krea ? "Krea" : "Qwen");
        ImGui::SameLine();
        const float wrap = ImGui::GetContentRegionAvail().x;
        std::string shortLook = c.look.size() > 90 ? c.look.substr(0, 87) + "..." : c.look;
        ImGui::PushStyleColor(ImGuiCol_Text, c.on ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : ui::kTextDim);
        ImGui::TextUnformatted(wrap > ui::Dp(120.0f) ? shortLook.c_str() : "");
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ui::Dp(360.0f));
            ImGui::TextUnformatted(c.look.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        on += c.on ? 1 : 0;
        ImGui::PopID();
    }
    if (ImGui::SmallButton("Clear the list")) g_invented.clear(), g_inventNote.clear();
    ui::SectionHeading("3. MAKE THEM");
    ImGui::TextDisabled("Make them (below) draws every ticked one; each becomes one of your characters.");
}

// The two ways to make characters, side by side at the top of the page
void DrawCreatorModes() {
    static bool inventTest = [] { // MV_UI_TEST "charinvent": the creator opens on Invent a cast (tests)
        char buf[256] = {};
        return GetEnvironmentVariableA("MV_UI_TEST", buf, sizeof(buf)) && strstr(buf, "charinvent") != nullptr;
    }();
    if (inventTest) inventTest = false, g_crMode = 1;
    const float gap = ui::Dp(10.0f);
    const float w = (ImGui::GetContentRegionAvail().x - gap * 2.0f) / 3.0f;
    struct Mode {
        const char* title;
        const char* text;
    };
    const Mode modes[3] = {{"Design one character", "Pick their looks (or Shuffle), optionally from your photo, and give them a name."},
                           {"Invent a cast from an idea", "Type an idea like \"medieval, strong, weird\": the AI makes up several characters and draws them all."},
                           {"Take them out of a picture", "Every person or creature in a picture becomes a character of its own, drawn alone in a studio."}};
    const float pad = ui::Dp(12.0f);
    float h = 0; // as tall as the longer text needs
    for (const Mode& md : modes)
        h = std::max(h, pad * 2.0f + ImGui::GetTextLineHeightWithSpacing() + ui::Dp(2.0f) + ImGui::CalcTextSize(md.text, nullptr, false, w - pad * 2.0f).y);
    for (int m = 0; m < 3; ++m) {
        if (m) ImGui::SameLine(0, gap);
        ImGui::PushID(m);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##mode", ImVec2(w, h))) g_crMode = m;
        const bool hot = ImGui::IsItemHovered(), on = g_crMode == m;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), on ? ui::WithAlpha(ui::kAccent, 0.22f) : (hot ? IM_COL32(255, 255, 255, 20) : IM_COL32(255, 255, 255, 9)), ui::Dp(8.0f));
        dl->AddRect(p, ImVec2(p.x + w, p.y + h), on ? ui::WithAlpha(ui::kAccent, 0.95f) : IM_COL32(255, 255, 255, hot ? 70 : 35), ui::Dp(8.0f), 0, ui::Dp(on ? 2.0f : 1.0f));
        // a round mark: filled when chosen
        const ImVec2 dot(p.x + pad + ui::Dp(6.0f), p.y + pad + ImGui::GetTextLineHeight() * 0.5f);
        dl->AddCircle(dot, ui::Dp(6.0f), on ? ui::WithAlpha(ui::kAccent, 1.0f) : IM_COL32(255, 255, 255, 120), 20, ui::Dp(1.5f));
        if (on) dl->AddCircleFilled(dot, ui::Dp(3.2f), ui::WithAlpha(ui::kAccent, 1.0f), 16);
        dl->AddText(ImVec2(p.x + pad + ui::Dp(20.0f), p.y + pad), ImGui::GetColorU32(ImGuiCol_Text), modes[m].title);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p.x + pad, p.y + pad + ImGui::GetTextLineHeightWithSpacing() + ui::Dp(2.0f)),
                    ImGui::GetColorU32(ui::kTextDim), modes[m].text, nullptr, w - pad * 2.0f);
        ImGui::PopID();
    }
}

// Who draws it: Auto (Krea for people, Qwen for creatures and caricatures), or one of them; and that model's LoRAs (the Generate page's too)
void DrawEngineRow(AppState& app) {
    const bool haveKrea = !KreaModelFile().empty();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Drawn by:");
    ImGui::SameLine();
    if (Chip("Auto##eng0", g_engine == 0)) g_engine = 0;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("Krea 2 for people (more like real photographs, and faster), Qwen Image 2.1 for creatures and extreme looks (it follows them far better)");
    ImGui::SameLine();
    if (Chip("Qwen Image 2.1##eng1", g_engine == 1)) g_engine = 1;
    ImGui::SameLine();
    ImGui::BeginDisabled(!haveKrea);
    if (Chip("Krea 2##eng2", g_engine == 2 && haveKrea)) g_engine = 2;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(haveKrea ? "%s" : "Needs a Krea 2 Turbo model in ComfyUI's diffusion_models (e.g. krea2_turbo_int8_convrot), with qwen3vl_4b_fp8_scaled and qwen_image_vae%s",
                          haveKrea ? KreaModelFile().c_str() : "");
    ImGui::SameLine();
    const bool haveMoss = MossReady(app);
    ImGui::BeginDisabled(!haveMoss);
    if (Chip("MOSS-Image##eng3", g_engine == 3 && haveMoss)) g_engine = 3;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(haveMoss ? "MOSS-Image, 8 steps: the fastest (about 3 s); faces come out a little softer than Krea's"
                                   : "MOSS-Image is not in ComfyUI yet: Edits > Designs & box edits has its download button");
    if (g_engine == 3 && !haveMoss) g_engine = 0;
    // the chosen model's LoRAs (Auto: both it may use)
    if (g_engine == 0 || g_engine == 2) DrawModelLoras(app, app.comfy.kreaLoras, "Krea 2", "crlk", g_engine == 0 ? "Krea 2 LoRAs:" : "LoRAs:");
    if (g_engine == 0 || g_engine == 1) DrawModelLoras(app, app.comfy.qwenLoras, "Qwen Image 2.1", "crlq", g_engine == 0 ? "Qwen 2.1 LoRAs:" : "LoRAs:");
    if (g_engine == 3) DrawModelLoras(app, app.comfy.mossLoras, "MOSS-Image", "crlm");
}

// The page: the looks on the left (with Make), your characters on the right (with what to do with them)
void DrawCreatorForm(AppState& app) {
    const auto& traits = Traits();
    if (g_cr.picked.size() != traits.size()) g_cr.picked.assign(traits.size(), {});
    const float footerH = ui::Dp(66.0f);
    ImGui::BeginChild("##crform", ImVec2(0, -footerH), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushFont(ui::TitleFont());
    ImGui::TextUnformatted("Create a character");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, ui::kTextDim);
    ImGui::TextWrapped("Krea 2, Qwen or MOSS draws them in a white studio. They become one of your characters: on the Vision board, in Clef mode, in edits, "
                       "and on the Video page. Three ways to make them:");
    ImGui::PopStyleColor();
    ImGui::Spacing();
    DrawCreatorModes();
    ImGui::Spacing();
    if (g_crMode != 2) DrawEngineRow(app); // (taking them out of a picture is always Qwen Image Edit, from that picture)
    if (g_crMode == 2) {
        DrawExtractSection(app);
        ImGui::EndChild();
        ImGui::Separator();
        const int on = ExtractFoundOn();
        char make[48];
        snprintf(make, sizeof(make), on ? "Make them (%d)" : "Make them", on);
        ImGui::BeginDisabled(on == 0);
        if (ui::PrimaryButton(make, ImVec2(ui::Dp(190.0f), ui::Dp(38.0f)))) MakeExtracted(app);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("You see who was found before anything is drawn; each takes about a minute.");
        return;
    }
    if (g_crMode == 1) {
        DrawInventSection(app);
        ImGui::EndChild();
        ImGui::Separator();
        const int on = InventedOn();
        char make[48];
        snprintf(make, sizeof(make), on ? "Make them (%d)" : "Make them", on);
        ImGui::BeginDisabled(on == 0);
        if (ui::PrimaryButton(make, ImVec2(ui::Dp(190.0f), ui::Dp(38.0f)))) MakeInvented(app);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled(g_invented.empty() ? "First type an idea and press Invent them: you see the list before anything is drawn."
                                               : "Each takes about half a minute; they are drawn one after another.");
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        return;
    }
    ui::SectionHeading("THEIR LOOKS");
    // your photo (optional)
    {
        const float sz = ui::Dp(96.0f);
        ImGui::BeginGroup();
        ImGui::TextColored(ui::kTextDim, "Your photo (optional: your face is kept)");
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##photo", ImVec2(sz, sz));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (g_cr.photo.empty()) {
            dl->AddRect(p, ImVec2(p.x + sz, p.y + sz), IM_COL32(255, 255, 255, 50), ui::Dp(8.0f), 0, ui::Dp(1.5f));
            const char* t = "none";
            const ImVec2 ts = ImGui::CalcTextSize(t);
            dl->AddText(ImVec2(p.x + (sz - ts.x) * 0.5f, p.y + (sz - ts.y) * 0.5f), ImGui::GetColorU32(ui::kTextDim), t);
        } else {
            dl->AddRectFilled(p, ImVec2(p.x + sz, p.y + sz), ui::ThumbPlaceholder(), ui::Dp(8.0f));
            Fit(app, dl, g_cr.photo, p, ImVec2(sz, sz), ui::Dp(6.0f));
        }
        const bool haveSel = app.selectedIndex >= 0 && app.selectedIndex < static_cast<int>(app.entries.size()) && !app.entries[app.selectedIndex].metadata.isVideo;
        ImGui::BeginDisabled(!haveSel);
        if (ImGui::SmallButton("Gallery's")) g_cr.photo = app.entries[app.selectedIndex].path;
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("File...")) {
            const std::wstring f = PickPicture(L"A photo of you");
            if (!f.empty()) g_cr.photo = f;
        }
        if (!g_cr.photo.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) g_cr.photo.clear();
        }
        ImGui::EndGroup();
    }
    ImGui::SameLine(0.0f, ui::Dp(18.0f));
    ImGui::BeginGroup();
    ImGui::TextColored(ui::kTextDim, "Framing");
    if (Chip("Full body", g_cr.framing == 0)) g_cr.framing = 0;
    ImGui::SameLine();
    if (Chip("Portrait", g_cr.framing == 1)) g_cr.framing = 1;
    if (ImGui::Button("Shuffle")) ShuffleCharacter();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Random looks (mostly people, now and then a creature)");
    ImGui::SameLine();
    if (ImGui::Button("Clear")) g_cr.picked.assign(traits.size(), {});
    ImGui::EndGroup();
    // the looks: one group open at a time
    for (size_t g = 0; g < traits.size(); ++g) {
        const Trait& t = traits[g];
        std::string chosen;
        for (int k : g_cr.picked[g]) chosen += (chosen.empty() ? "" : ", ") + std::string(t.options[k]);
        char head[200];
        snprintf(head, sizeof(head), "%s  %s###grp%d", t.group, chosen.empty() ? "" : ("- " + chosen).c_str(), static_cast<int>(g));
        ImGui::SetNextItemOpen(g_cr.openGroup == static_cast<int>(g));
        if (ImGui::CollapsingHeader(head)) {
            g_cr.openGroup = static_cast<int>(g);
            for (size_t k = 0; k < t.options.size(); ++k) {
                if (k) ImGui::SameLine(0.0f, ui::Dp(4.0f));
                if (ImGui::GetCursorPosX() + ImGui::CalcTextSize(t.options[k]).x + ui::Dp(24.0f) > ImGui::GetContentRegionMax().x) ImGui::NewLine();
                auto& sel = g_cr.picked[g];
                const auto it = std::find(sel.begin(), sel.end(), static_cast<int>(k));
                char lbl[64];
                snprintf(lbl, sizeof(lbl), "%s##o%d_%d", t.options[k], static_cast<int>(g), static_cast<int>(k));
                if (Chip(lbl, it != sel.end())) {
                    if (it != sel.end()) sel.erase(it);
                    else if (t.multi) sel.push_back(static_cast<int>(k));
                    else sel = {static_cast<int>(k)};
                }
            }
        } else if (g_cr.openGroup == static_cast<int>(g)) {
            g_cr.openGroup = -1;
        }
    }
    ui::SectionHeading("ANYTHING ELSE (YOUR WORDS WIN)");
    WrapEdit("##crextra", g_cr.extra, ui::Dp(44.0f));
    if (!g_cr.extra.empty())
        ImGui::TextDisabled(WriterConfigured() ? "The prompt writer puts these words together with the looks picked when you press Make: where they disagree, your words win."
                                               : "These words come first in the prompt; \"human\" (man, woman, guy...) drops an animal type picked.");
    if (!g_lookWrites.empty()) ImGui::TextColored(ui::kAccent, "Writing the look of %d...", static_cast<int>(g_lookWrites.size()));
    if (ImGui::TreeNode("The prompt it makes")) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", CharacterPrompt().c_str());
        ImGui::PopTextWrapPos();
        ImGui::TreePop();
    }
    ImGui::EndChild();
    ImGui::Separator();
    if (ui::PrimaryButton("Make the character", ImVec2(ui::Dp(190.0f), ui::Dp(38.0f))) ||
        (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Enter)))
        MakeCharacter(app);
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Name");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ui::Dp(200.0f));
    char nameBuf[64] = {};
    snprintf(nameBuf, sizeof(nameBuf), "%s", g_cr.name.c_str());
    if (ImGui::InputTextWithHint("##crname", CharacterName().c_str(), nameBuf, sizeof(nameBuf))) g_cr.name = nameBuf;
    ImGui::TextDisabled("about half a minute (Ctrl+Enter)");
    ImGui::EndGroup();
}

void CharacterActions(AppState& app, const std::wstring& path, bool menu) {
    auto item = [&](const char* label) { return menu ? ImGui::MenuItem(label) : ImGui::Button(label); };
    auto same = [&]() {
        if (!menu) ImGui::SameLine();
    };
    if (item("Animate")) OpenVideoPage(app, path, std::string());
    same();
    if (item("Star in a video")) StarInVideo(app, path);
    same();
    if (item("Edit with Qwen...")) OpenPictureEditor(app, {path});
    if (!menu) ImGui::NewLine();
    if (item("Add to Clef mode's cast")) AddToClefCast(app, {path});
    same();
    if (item("Put on the Vision board")) AddImagesToBoard(app, {path}, 0);
    same();
    if (item("Open in Your characters")) ShowSavedCharacter(app, path);
    if (!menu) ImGui::NewLine();
    const CharacterProfile* cp = FindProfile(path);
    const bool hasSheet = cp && !cp->sheet.empty();
    ImGui::BeginDisabled(SheetPending(path));
    if (item(SheetPending(path) ? "Making its character sheet..." : hasSheet ? "Make its character sheet again" : "Make a character sheet")) MakeCharacterSheet(app, path);
    ImGui::EndDisabled();
    if (!menu && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Qwen Image 2.1 draws a sheet of this character from its picture: four expressions, then front, side and back views.\n"
                          "Its Recreate videos use it when its back looks unlike its front (a cape, armour); otherwise it is there to look at (about a minute).");
    if (hasSheet) {
        same();
        if (item("Open its sheet")) ShellExecuteW(nullptr, L"open", cp->sheet.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void DrawCharacterWall(AppState& app) {
    ui::SectionHeading("YOUR CHARACTERS");
    ImGui::SameLine();
    DrawClearCharacters("wall");
    std::vector<const CharacterProfile*> list;
    for (const CharacterProfile& p : AllProfiles()) {
        if (p.saved && !p.image.empty()) list.push_back(&p);
    }
    std::reverse(list.begin(), list.end()); // the newest first
    // the actions for the one picked (always in view at the top)
    if (!g_chosenChar.empty()) {
        const CharacterProfile* cp = FindProfile(g_chosenChar);
        ImGui::TextColored(ui::kAccentHover, "%s", cp ? cp->name.c_str() : Utf8(fs::path(g_chosenChar).stem().wstring()).c_str());
        CharacterActions(app, g_chosenChar, false);
        ImGui::Separator();
    } else if (!list.empty()) {
        ImGui::TextDisabled("Click one: animate it, star it in a video, put it in Clef mode or on the Vision board.");
    }
    ImGui::BeginChild("##wall", ImVec2(0, 0));
    const float gap = ui::Dp(8.0f);
    const int cols = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + gap) / (ui::Dp(150.0f) + gap)));
    const float tw = (ImGui::GetContentRegionAvail().x - gap * (cols - 1)) / cols, th = tw * 4.0f / 3.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    int k = 0;
    // being drawn now
    for (const PendingChar& pc : g_pendingChars) {
        if (k % cols) ImGui::SameLine(0.0f, gap);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(tw, th + ImGui::GetTextLineHeightWithSpacing()));
        dl->AddRectFilled(p, ImVec2(p.x + tw, p.y + th), ImGui::GetColorU32(ImGuiCol_FrameBg), ui::Dp(8.0f));
        const float t = static_cast<float>(ImGui::GetTime());
        const ImVec2 c(p.x + tw * 0.5f, p.y + th * 0.5f);
        dl->AddCircle(c, ui::Dp(14.0f), IM_COL32(255, 255, 255, 30), 24, ui::Dp(2.5f));
        dl->PathArcTo(c, ui::Dp(14.0f), t * 5.0f, t * 5.0f + 1.9f, 16);
        dl->PathStroke(ImGui::GetColorU32(ui::kAccentHover), 0, ui::Dp(2.5f));
        dl->AddText(ImVec2(p.x + ui::Dp(4.0f), p.y + th + ui::Dp(2.0f)), ImGui::GetColorU32(ui::kTextDim), ("Drawing " + pc.name + "...").c_str());
        ++k;
    }
    for (const CharacterProfile* cp : list) {
        if (k % cols) ImGui::SameLine(0.0f, gap);
        ImGui::PushID(cp->key.c_str());
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##ch", ImVec2(tw, th + ImGui::GetTextLineHeightWithSpacing()));
        const bool hov = ImGui::IsItemHovered();
        const bool isChosen = _wcsicmp(g_chosenChar.c_str(), cp->image.c_str()) == 0;
        if (ImGui::IsItemClicked()) g_chosenChar = cp->image;
        if (ImGui::BeginPopupContextItem("##chmenu")) {
            CharacterActions(app, cp->image, true);
            ImGui::EndPopup();
        }
        if (ImGui::IsItemVisible()) {
            dl->AddRectFilled(p, ImVec2(p.x + tw, p.y + th), IM_COL32(242, 242, 242, 255), ui::Dp(8.0f));
            Fit(app, dl, cp->image, p, ImVec2(tw, th), ui::Dp(8.0f));
            if (hov || isChosen) dl->AddRect(p, ImVec2(p.x + tw, p.y + th), ImGui::GetColorU32(isChosen ? ui::kAccent : ui::kAccentHover), ui::Dp(8.0f), 0, ui::Dp(2.5f));
            const float right = p.x + tw;
            ImGui::PushStyleColor(ImGuiCol_Text, isChosen ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : ui::kTextDim);
            ImGui::RenderTextEllipsis(dl, ImVec2(p.x + ui::Dp(2.0f), p.y + th + ui::Dp(2.0f)), ImVec2(right, p.y + th + ImGui::GetTextLineHeightWithSpacing()), right, right,
                                      cp->name.c_str(), nullptr, nullptr);
            ImGui::PopStyleColor();
            if (hov && !cp->description.empty()) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ui::Dp(320.0f));
                ImGui::TextUnformatted(cp->description.c_str());
                ImGui::PopTextWrapPos();
                ImGui::TextDisabled("Click: choose it. Right-click: what to do with it");
                ImGui::EndTooltip();
            }
        }
        ImGui::PopID();
        ++k;
    }
    if (k == 0) ImGui::TextDisabled("No characters yet: make one on the left.");
    ImGui::EndChild();
}

} // namespace

void MakeCharacterSheetFor(AppState& app, const std::wstring& image) { MakeCharacterSheet(app, image); }
std::string GenerateKreaModel() { return KreaModelFile(); }
bool CharacterSheetUseful(const std::wstring& image) {
    CharacterProfile* p = FindProfile(image);
    return p && MeasureSheetNeeded(*p) == 1;
}
bool CharacterSheetPending(const std::wstring& image) { return SheetPending(image); }

// "Clear all...": every saved character leaves the list (the Characters tab, the Vision board's, Clef's); their pictures stay on disk
void DrawClearCharacters(const char* id) {
    ImGui::PushID(id);
    int n = 0;
    for (const CharacterProfile& p : AllProfiles()) n += p.saved ? 1 : 0;
    ImGui::BeginDisabled(n == 0);
    if (ImGui::SmallButton("Clear all...")) ImGui::OpenPopup("##clearall");
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("##clearall")) {
        ImGui::Text("Remove all %d saved characters?", n);
        ImGui::TextDisabled("Their pictures stay where they are (ComfyUI's output, Characters):\nadd any of them back later with + From a file.");
        if (ui::PrimaryButton("Remove them")) {
            for (CharacterProfile& p : AllProfiles()) p.saved = false;
            SaveProfiles();
            g_chosenChar.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

namespace {

} // namespace

void OpenCharacterCreator(AppState& app) {
    g_showCreator = true;
    app.requestTab = 5;
}

bool CharacterCreatorShown() { return g_showCreator; }
void ShowCharacterCreator(bool on) { g_showCreator = on; }
void SetCharacterCreatorMode(int mode) { g_crMode = std::clamp(mode, 0, 2); }

void DrawCharacterCreator(AppState& app, float height) {
    PumpLookWrites(app);
    { // MV_UI_TEST "charmake[:N]": shuffle (character type N) and make one, once
        static const int test = [] {
            char buf[256] = {};
            if (!GetEnvironmentVariableA("MV_UI_TEST", buf, sizeof(buf))) return -1;
            const char* at = strstr(buf, "charmake");
            return at ? (at[8] == ':' ? atoi(at + 9) : 0) : -1;
        }();
        static bool done = false;
        if (test >= 0 && !done) {
            done = true;
            ShuffleCharacter();
            if (test > 0) g_cr.picked[0] = {test};
            char words[512] = {}; // (MV_CHAR_WORDS: typed in Anything else)
            if (GetEnvironmentVariableA("MV_CHAR_WORDS", words, sizeof(words))) g_cr.extra = words;
            MakeCharacter(app);
        }
        { // MV_CHAR_ENGINE=N: who draws them (0 auto, 1 Qwen, 2 Krea, 3 MOSS) (tests)
            static bool engineSet = false;
            char e[8] = {};
            if (!engineSet && GetEnvironmentVariableA("MV_CHAR_ENGINE", e, sizeof(e))) engineSet = true, g_engine = std::clamp(atoi(e), 0, 3);
        }
        // MV_INVENT "idea|count[|make]": Invent a cast from the idea, and (make) Make them once the list is in (tests)
        static int inventStage = 0;
        char inv[512] = {};
        if (inventStage < 2 && GetEnvironmentVariableA("MV_INVENT", inv, sizeof(inv))) {
            std::string spec = inv;
            const size_t b1 = spec.find('|'), b2 = b1 == std::string::npos ? std::string::npos : spec.find('|', b1 + 1);
            if (inventStage == 0) {
                inventStage = 1;
                g_idea = spec.substr(0, b1);
                g_inventCount = b1 == std::string::npos ? 0 : atoi(spec.c_str() + b1 + 1);
                g_inventFut = std::async(std::launch::async, InventCast, app.comfy.url, g_idea, g_inventCount);
            } else if (!g_inventFut.valid() && !g_invented.empty()) {
                inventStage = 2;
                std::string dump;
                for (const Invented& c : g_invented) dump += c.name + " | " + (c.unusual ? "creature" : "human") + " | " + c.look + "\n";
                wchar_t lad[MAX_PATH] = {};
                GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH);
                std::ofstream(std::wstring(lad) + L"\\Try2Create\\invent_test.txt", std::ios::binary) << dump;
                if (b2 != std::string::npos && spec.substr(b2 + 1) == "make") MakeInvented(app);
            }
        }
        // MV_CHARACTERS "Gender=Man;Style=Luxury designer;extra=...;name=Marcus|Gender=Woman;..." makes each, once (tests)
        static bool listDone = false;
        char spec[4096] = {};
        if (!listDone && GetEnvironmentVariableA("MV_CHARACTERS", spec, sizeof(spec))) {
            listDone = true;
            const auto& traits = Traits();
            std::string all = spec;
            size_t a = 0;
            while (a <= all.size()) {
                const size_t b = std::min(all.find('|', a), all.size());
                const std::string one = all.substr(a, b - a);
                g_cr.picked.assign(traits.size(), {});
                g_cr.extra.clear();
                g_cr.name.clear();
                size_t c = 0;
                while (c < one.size()) {
                    const size_t d = std::min(one.find(';', c), one.size());
                    const std::string kv = one.substr(c, d - c);
                    const size_t eq = kv.find('=');
                    if (eq != std::string::npos) {
                        const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
                        if (k == "extra") g_cr.extra = v;
                        else if (k == "name") g_cr.name = v;
                        for (size_t g = 0; g < traits.size(); ++g) {
                            if (k != traits[g].group) continue;
                            for (size_t o = 0; o < traits[g].options.size(); ++o) {
                                if (v == traits[g].options[o]) g_cr.picked[g].push_back(static_cast<int>(o));
                            }
                        }
                    }
                    c = d + 1;
                }
                if (!one.empty()) MakeCharacter(app);
                a = b + 1;
            }
        }
    }
    if (!ImGui::BeginTable("##creator", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings, ImVec2(0, height))) return;
    ImGui::TableSetupColumn("form", ImGuiTableColumnFlags_WidthStretch, 0.52f);
    ImGui::TableSetupColumn("wall", ImGuiTableColumnFlags_WidthStretch, 0.48f);
    ImGui::TableNextRow(0, height);
    ImGui::TableSetColumnIndex(0);
    ImGui::BeginChild("##crleft", ImVec2(0, height));
    DrawCreatorForm(app);
    ImGui::EndChild();
    ImGui::TableSetColumnIndex(1);
    ImGui::BeginChild("##crright", ImVec2(0, height), ImGuiChildFlags_AlwaysUseWindowPadding);
    DrawCharacterWall(app);
    ImGui::EndChild();
    ImGui::EndTable();
}

bool CharacterCreatorBusy() { return !g_pendingChars.empty() || !g_pendingSheets.empty(); }

void PumpCharacterCreator(AppState& app) {
    { // tests: MV_MAKE_SHEET names a saved character's picture: its character sheet is made once
        static bool done = false;
        wchar_t buf[1024] = {};
        if (!done && app.comfySvc && GetEnvironmentVariableW(L"MV_MAKE_SHEET", buf, 1024)) {
            done = true;
            MakeCharacterSheet(app, buf);
        }
    }
    { // tests: MV_SHEET_TEST "sheet|sheet|...": each sheet's front/back overlap into %LOCALAPPDATA%\Try2Create\sheet_test.txt
        static bool done = false;
        wchar_t buf[4096] = {};
        if (!done && GetEnvironmentVariableW(L"MV_SHEET_TEST", buf, 4096)) {
            done = true;
            const std::wstring all = buf;
            wchar_t lad[1024] = {};
            GetEnvironmentVariableW(L"LOCALAPPDATA", lad, 1024);
            std::ofstream out(std::wstring(lad) + L"\\Try2Create\\sheet_test.txt", std::ios::binary);
            for (size_t a = 0; a < all.size();) {
                const size_t b = std::min(all.find(L'|', a), all.size());
                const double o = SheetFrontBackOverlap(all.substr(a, b - a));
                out << Utf8(fs::path(all.substr(a, b - a)).filename().wstring()) << " " << o << (o >= 0 && o < kSheetBackDiffers ? " use it" : " picture") << "\n";
                a = b + 1;
            }
        }
    }
    PumpCharacterSheets(app);
    if (g_pendingChars.empty() || !app.comfySvc) return;
    const std::vector<ComfyActivity> acts = app.comfySvc->Activity();
    for (size_t i = 0; i < g_pendingChars.size();) {
        const PendingChar pc = g_pendingChars[i];
        const ComfyActivity* a = nullptr;
        for (const ComfyActivity& x : acts) {
            if (x.id == pc.activity) a = &x;
        }
        if (a && a->state != ComfyState::Done && a->state != ComfyState::Failed) {
            ++i;
            continue;
        }
        g_pendingChars.erase(g_pendingChars.begin() + i);
        std::wstring file;
        if (a && a->state == ComfyState::Done) {
            for (const ComfyResult& r : a->files) {
                if (!r.localPath.empty()) {
                    file = r.localPath;
                    break;
                }
            }
        }
        if (file.empty()) {
            Toast(ToastKind::Error, "Could not make " + pc.name, a ? a->message : std::string("ComfyUI lost the job"), {}, [](AppState& s) { s.requestTab = 4; }, "charmake");
            continue;
        }
        // one of your characters from now on: everywhere characters are used
        CharacterProfile& p = EnsureProfile(file);
        p.saved = true;
        p.name = pc.name;
        if (p.description.empty()) p.description = pc.description;
        SaveProfiles();
        QueueCharacterStory(app, file, pc.autoName); // every character gets a life (and a real name when theirs was made up)
        g_chosenChar = file;
        Toast(ToastKind::Success, "New character: " + pc.name, "In the Characters tab, the Vision board, Clef mode and the Video page", file,
              [file](AppState& s) { ShowSavedCharacter(s, file); }, "charmake");
    }
}
