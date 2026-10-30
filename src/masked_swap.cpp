// Star in a video's swap since 2026-10-28: Openjutsu's (github.com/D3voz/openjutsu, MIT) ComfyUI workflows in tools\openjutsu, trimmed per
// job the way its lib/simple.ts, advanced.ts and object.ts do. SAM 3.1 tracks the people in every 6 s clip; the person to replace becomes a
// grey mannequin of their exact outline - the motion reference H3 copies the pose from - and only their grown mask is redrawn (Fantastic
// PromptBuilder's masked video-edit latent), so the background stays pixel for pixel; Qwen3-VL 8B describes the pictures and writes each
// clip's shot list; Context Loop chains the clips (39 frames shared) into one video with the clip's own sound.
// Why: on the same clip and picture (a woman in a greenhouse turned into the old fisherman, 5 s, 24 GB card, PRISM) it swapped him in on the
// first try in 301 s at 736p, where the Character Swap LoRA way came back unchanged twice (509 + 500 s, then a faces pass).
#include "masked_swap.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <map>
#include <windows.h>

#include "comfy.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

bool IsLink(const json& v) { return v.is_array() && v.size() == 2 && v[0].is_string() && v[1].is_number_integer(); }

std::string Title(const json& n) { return n.contains("_meta") ? n["_meta"].value("title", std::string()) : std::string(); }

std::string ByTitle(const json& g, const std::string& title) {
    std::string id;
    int found = 0;
    for (auto it = g.begin(); it != g.end(); ++it) {
        if (Title(it.value()) == title) id = it.key(), ++found;
    }
    return found == 1 ? id : std::string();
}

bool Load(const wchar_t* file, json& g, std::string* error) {
    std::ifstream f(fs::path(OpenjutsuDir()) / file, std::ios::binary);
    g = f ? json::parse(f, nullptr, false) : json();
    if (!g.is_object()) {
        if (error) *error = "tools\\openjutsu is missing or damaged";
        return false;
    }
    return true;
}

// Every input linked to a node that is gone: true when none is left (a trim gone wrong would make ComfyUI refuse the whole job)
bool Dangling(const json& g, std::string* error) {
    for (auto it = g.begin(); it != g.end(); ++it) {
        for (auto in = it.value()["inputs"].begin(); in != it.value()["inputs"].end(); ++in) {
            if (IsLink(in.value()) && !g.contains(in.value()[0].get<std::string>())) {
                if (error) *error = "the workflow lost a link (" + it.key() + "." + in.key() + ")";
                return true;
            }
        }
    }
    return false;
}

// No lip-sync off (lib/simple.ts dropNoLipSync): the mute and face nodes go, H3 hears the soundtrack and the mannequins get no faces
void DropNoLipSync(json& g) {
    static const std::regex mute("^Mute the (clip|reference) audio H3 hears$"), nols("^(No lip-sync in the prompt|SAM: faces|SAM 3\\.1 track faces|Faces \\(shown on the mannequins?\\))$");
    std::map<std::string, json> through; // id -> what replaces links to it (null: the input goes)
    for (auto it = g.begin(); it != g.end(); ++it) {
        const std::string t = Title(it.value());
        if (std::regex_match(t, mute)) through[it.key()] = it.value()["inputs"]["audio"];
        else if (std::regex_match(t, nols)) through[it.key()] = nullptr;
    }
    for (const auto& [id, _] : through) g.erase(id);
    for (auto it = g.begin(); it != g.end(); ++it) {
        json& in = it.value()["inputs"];
        std::vector<std::string> drop;
        for (auto k = in.begin(); k != in.end(); ++k) {
            if (!IsLink(k.value())) continue;
            auto t = through.find(k.value()[0].get<std::string>());
            if (t == through.end()) continue;
            if (!t->second.is_null()) k.value() = t->second;
            else drop.push_back(k.key()); // (no_lip_sync, faces, face_size)
        }
        for (const std::string& k : drop) in.erase(k);
    }
}

// The mannequins' size; another size than 512 gets its own run name, so a rerun never resumes from clips made at another size
void SetMotionSize(json& g, int size) {
    for (auto it = g.begin(); it != g.end(); ++it) {
        if (it.value()["class_type"] == "ImageScaleToMaxDimension" && std::regex_search(Title(it.value()), std::regex("motion reference", std::regex::icase)))
            it.value()["inputs"]["largest_size"] = size;
    }
    if (size == 512) return;
    for (auto it = g.begin(); it != g.end(); ++it) {
        if (it.value()["class_type"] != "MiniMaxH3ChainPlanModern") continue;
        g["motion_run_name"] = {{"class_type", "StringConcatenate"},
                                {"inputs", {{"string_a", it.value()["inputs"]["run_name"]}, {"string_b", "_motion" + std::to_string(size)}, {"delimiter", ""}}},
                                {"_meta", {{"title", "Run name for this motion reference size"}}}};
        it.value()["inputs"]["run_name"] = json::array({"motion_run_name", 0});
        break;
    }
}

// The part of the clip (every export loads it at 24 fps)
void SetSection(json& g, const std::string& video, double start, double seconds) {
    g[video]["inputs"]["skip_first_frames"] = static_cast<int>(std::lround(start * 24.0));
    g[video]["inputs"]["frame_load_cap"] = seconds > 0 ? static_cast<int>(std::lround(seconds * 24.0)) : 0;
    g[video]["inputs"]["video"] = "@clip";
}

// This PC's files, the speed LoRA (or none), the final video's name and place
void SetModels(json& g, const std::string& unet, const std::string& te, const std::string& speed, bool useSpeed, const std::string& out) {
    for (auto it = g.begin(); it != g.end(); ++it) {
        const std::string t = Title(it.value());
        json& in = it.value()["inputs"];
        if (t == "H3 Diffusion Model" && !unet.empty()) in["unet_name"] = unet;
        if (t == "H3 Text Encoder" && !te.empty()) in["clip_name"] = te;
        if (t == "DMAD LoRA" && !speed.empty()) in["lora_name"] = speed;
        if (it.value()["class_type"] == "MiniMaxH3ChainAssemble") {
            in["output_subfolder"] = "video";
            in["filename"] = out;
            in["delete_checkpoints_after_assembly"] = true; // (the clips' latents, once they are one video)
        }
    }
    if (useSpeed) return;
    const std::string lora = ByTitle(g, "DMAD LoRA"); // the model straight through
    if (lora.empty()) return;
    const json model = g[lora]["inputs"]["model"];
    g.erase(lora);
    for (auto it = g.begin(); it != g.end(); ++it) {
        for (auto k = it.value()["inputs"].begin(); k != it.value()["inputs"].end(); ++k) {
            if (IsLink(k.value()) && k.value()[0] == lora) k.value() = model;
        }
    }
}

// Veda's sparse attention on the model the sampler gets (after the LoRA, the shift and the attention backend: last, as on the Video page)
void AddVeda(json& g, const std::string& predictor) {
    if (predictor.empty()) return;
    for (auto it = g.begin(); it != g.end(); ++it) {
        if (it.value()["class_type"] != "BasicGuider" || !IsLink(it.value()["inputs"]["model"])) continue;
        const json model = it.value()["inputs"]["model"];
        g["veda"] = {{"class_type", "VedaSparseAttention"},
                     {"inputs", {{"model", model}, {"predictor", predictor}, {"generated_sparsity", "32"}, {"reference_sparsity", "32"}, {"full_attention_layers", ""},
                                 {"full_attention_steps", ""}, {"verbose", false}}},
                     {"_meta", {{"title", "Veda sparse attention"}}}};
        for (auto n = g.begin(); n != g.end(); ++n) {
            if (n.key() == "veda") continue;
            for (auto k = n.value()["inputs"].begin(); k != n.value()["inputs"].end(); ++k) {
                if (k.key() == "model" && k.value() == model) k.value() = json::array({"veda", 0});
            }
        }
        return;
    }
}

const int kMargins[] = {20, 40, 60}; // the edit mask's growth at SAM's 1008 px (tight, normal, loose)

// Qwen's shot list goes through our Try2CreateShotList before H3 gets it: on a fast-cut clip the 8B model wrote the answer's form back
// ("[Shot 2] At 00:01.917, the camera cuts to <the shot>", every shot), so after the first cut H3 was not told who is in which shot and
// drew the source people back (2026-10-29, a 6-shot clip: Bowser in shot 1, the two originals after). The node keeps lines that describe
// their shot and writes the rest from the role masks (who is in the shot and where).
// The swap's prompt (comfyui-h3-oneclick-swap) says "Live-action footage ... with natural skin tones": a cartoon that can pass for a person
// came out as a real one - Peach on a dancing girl stayed the girl, on a woman walking became a realistic blonde in a white top - where Bowser,
// who can't, was drawn as he is every time (2026-10-29). Said instead: the new people keep their pictures' own look (a photo
// stays a photo), the setting stays the clip's.
void KeepPictureStyle(json& g) {
    std::vector<std::string> fills;
    for (auto it = g.begin(); it != g.end(); ++it)
        if (it.value()["class_type"] == "H3FillShots") fills.push_back(it.key());
    for (const std::string& f : fills) {
        const std::string id = "style_" + f;
        for (auto it = g.begin(); it != g.end(); ++it)
            for (auto k = it.value()["inputs"].begin(); k != it.value()["inputs"].end(); ++k)
                if (IsLink(k.value()) && k.value()[0] == f) k.value() = json::array({id, 0});
        g[id] = {{"class_type", "StringReplace"}, {"_meta", {{"title", "Keep the pictures' own look (Try2Create)"}}},
                 {"inputs", {{"string", json::array({f, 0})},
                             {"find", "Live-action footage in the same setting as <Video 1>, with natural skin tones and the source lighting and lens perspective."},
                             {"replace", "Footage in the same setting as <Video 1>, with the source lighting and lens perspective; the setting stays as it is in "
                                         "<Video 1>. Each replacement person keeps the exact look of their picture: a photographed person stays photographic, a 3D "
                                         "animated or cartoon character stays 3D animated or cartoon, with the face, proportions, colours and rendering of their "
                                         "picture, and is never turned into a realistic person."}}}};
    }
}

void AddShotCheck(json& g) {
    std::vector<std::string> fills; // (one pass per person has one for each pass, both reading the one shot list)
    std::string shots;
    for (auto it = g.begin(); it != g.end(); ++it) {
        if (it.value()["class_type"] == "H3FillShots" && IsLink(it.value()["inputs"]["shots"])) fills.push_back(it.key());
        if (it.value()["class_type"] == "H3ClipShots" && it.value()["inputs"].contains("role_map")) shots = it.key();
    }
    if (fills.empty() || shots.empty()) return;
    g["t2c_shots"] = {{"class_type", "Try2CreateShotList"},
                      {"_meta", {{"title", "Shot list check (Try2Create: who is in each shot)"}}},
                      {"inputs", {{"shots", g[fills[0]]["inputs"]["shots"]}, {"images", g[shots]["inputs"]["images"]}, {"role_map", g[shots]["inputs"]["role_map"]},
                                  {"cut_threshold", g[shots]["inputs"].value("cut_threshold", 40.0)}}}};
    for (const std::string& f : fills) g[f]["inputs"]["shots"] = json::array({"t2c_shots", 0});
    // the labeller sees the 24 biggest appearances, not 12: a 9 s cafeteria clip with 8 cuts and a crowd left the girl's close-up and the last
    // shot unlabelled - so not swapped (2026-10-29)
    for (auto it = g.begin(); it != g.end(); ++it)
        if (it.value()["class_type"] == "H3TrackCrops") it.value()["inputs"]["max_crops"] = 24;
    // and Try2CreateRoleFix after H3 Track Roles: two people on screen together labelled as one subject (a girl with a man behind her both came
    // out Subject 1, 2026-10-29 - Bowser drawn over the two as one figure, Peach never used) - the smaller gets a subject nobody has
    std::string roles, crops;
    for (auto it = g.begin(); it != g.end(); ++it) {
        if (it.value()["class_type"] == "H3TrackRoles") roles = it.key();
        if (it.value()["class_type"] == "H3TrackCrops") crops = it.key();
    }
    if (roles.empty() || crops.empty()) return;
    for (auto it = g.begin(); it != g.end(); ++it)
        for (auto k = it.value()["inputs"].begin(); k != it.value()["inputs"].end(); ++k)
            if (IsLink(k.value()) && k.value()[0] == roles) k.value() = json::array({"t2c_roles", 0});
    g["t2c_roles"] = {{"class_type", "Try2CreateRoleFix"},
                      {"_meta", {{"title", "Role fix (Try2Create: no two people on screen as one subject)"}}},
                      {"inputs", {{"labels", json::array({roles, 0})}, {"segments", json::array({crops, 1})}}}};
}

} // namespace

std::wstring OpenjutsuDir() {
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    return (fs::path(buf).parent_path() / L"tools" / L"openjutsu").wstring();
}

bool InstallShotCheck() {
    const std::wstring install = ComfyInstallDir();
    if (install.empty()) return false;
    const fs::path from = fs::path(OpenjutsuDir()) / L"nodes" / L"try2create_shots" / L"__init__.py";
    const fs::path dir = fs::path(ComfyAppDir()) / L"custom_nodes" / L"try2create_shots";
    std::string want, have;
    auto read = [](const fs::path& p, std::string& out) {
        std::ifstream f(p, std::ios::binary);
        if (!f) return false;
        out.assign(std::istreambuf_iterator<char>(f), {});
        return true;
    };
    if (!read(from, want)) return false;
    if (read(dir / L"__init__.py", have) && have == want) return true;
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream f(dir / L"__init__.py", std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << want;
    return static_cast<bool>(f);
}

bool MaskedSwapNodePacks(std::string* missing) {
    const std::wstring install = ComfyInstallDir();
    std::string m;
    std::error_code ec;
    for (const wchar_t* p : {L"ComfyUI-MiniMaxH3-Context-Loop", L"ComfyUI-Fantastic-MiniMaxH3-PromptBuilder", L"ComfyUI-VideoHelperSuite", L"comfyui-h3-oneclick-swap", L"comfyui-kjnodes"}) {
        bool there = false;
        if (!install.empty()) {
            for (const auto& e : fs::directory_iterator(fs::path(ComfyAppDir()) / L"custom_nodes", ec)) there |= _wcsicmp(e.path().filename().c_str(), p) == 0;
        }
        if (!there) m += std::string(m.empty() ? "" : ", ") + std::string(p, p + wcslen(p));
    }
    if (missing) *missing = m;
    return m.empty();
}

json BuildMaskedSwap(const MaskedSwapOptions& o, std::string* error) {
    bool anyRole = false;
    for (const std::string& r : o.roles) anyRole |= !r.empty();
    const bool simple = o.pictures == 1 && !anyRole; // the biggest person: Simple; else Advanced (the tracks labelled by role)
    json g;
    const bool twoPass = !simple && o.onePass && o.pictures == 2;
    if (!Load(simple ? L"simple.api.json" : twoPass ? L"advanced_maxq.api.json" : L"advanced.api.json", g, error)) return {};
    std::string plan, video;
    if (simple) {
        plan = "11", video = "5";
        g["24"]["inputs"]["text"] = o.target.empty() ? "person" : o.target;
        g["42"]["inputs"]["expand"] = kMargins[std::clamp(o.margin, 0, 2)];
        if (!o.hatFix) {
            for (const char* id : {"47", "48", "49", "50", "51", "52"}) g.erase(id);
            g["42"]["inputs"]["mask"] = json::array({"39", 0});
        }
        g["2"]["inputs"]["image"] = "@pic1";
    } else {
        plan = ByTitle(g, "Swap Plan (roles, seed, size)"), video = ByTitle(g, "Source video (people get replaced)");
        if (plan.empty() || video.empty()) {
            if (error) *error = "tools\\openjutsu\\advanced.api.json is not the one this app knows";
            return {};
        }
        // unused pictures: the photo, its shrink and its describer go, and the inputs that read them
        std::set<std::string> removed;
        for (int k = o.pictures + 1; k <= 4; ++k) {
            const std::string load = ByTitle(g, "Picture " + std::to_string(k)), describe = ByTitle(g, "Describe picture " + std::to_string(k));
            for (auto it = g.begin(); it != g.end(); ++it) {
                const json& in = it.value()["inputs"];
                if (it.value()["class_type"] == "ImageScaleToMaxDimension" && in.contains("image") && IsLink(in["image"]) && in["image"][0] == load) removed.insert(it.key());
            }
            removed.insert(load), removed.insert(describe);
        }
        for (int k = 1; k <= o.pictures; ++k) g[ByTitle(g, "Picture " + std::to_string(k))]["inputs"]["image"] = "@pic" + std::to_string(k);
        std::map<std::string, json> bypass;
        if (!o.hatFix) { // the hat nodes go; whatever read "people + hats" reads the people mask
            for (auto it = g.begin(); it != g.end(); ++it) {
                if (!std::regex_search(Title(it.value()), std::regex("\\bhats?\\b", std::regex::icase))) continue;
                removed.insert(it.key());
                if (it.value()["class_type"] == "MaskComposite" && it.value()["inputs"].value("operation", "") == "add") bypass[it.key()] = it.value()["inputs"]["destination"];
            }
        }
        for (const std::string& id : removed) g.erase(id);
        for (auto it = g.begin(); it != g.end(); ++it) {
            json& in = it.value()["inputs"];
            std::vector<std::string> drop;
            for (auto k = in.begin(); k != in.end(); ++k) {
                if (!IsLink(k.value()) || !removed.count(k.value()[0].get<std::string>())) continue;
                auto b = bypass.find(k.value()[0].get<std::string>());
                if (b != bypass.end()) k.value() = b->second;
                else if (k.key().rfind("picture_", 0) == 0 || k.key().rfind("ref_images.", 0) == 0) drop.push_back(k.key());
            }
            for (const std::string& k : drop) in.erase(k);
        }
        for (auto it = g.begin(); it != g.end(); ++it) {
            if (it.value()["class_type"] == "GrowMaskWithBlur" && Title(it.value()).find(" grow mask ") != std::string::npos)
                it.value()["inputs"]["expand"] = kMargins[std::clamp(o.margin, 0, 2)];
        }
        std::string roles;
        for (int k = 0; k < o.pictures; ++k) { // a picture without a role, when others have one: by screen time, as the plan's own default
            std::string r = k < static_cast<int>(o.roles.size()) ? o.roles[k] : std::string();
            static const char* const kNth[] = {"", "second ", "third ", "fourth "};
            if (r.empty()) r = o.pictures == 1 ? "the main person" : std::string("the person with the ") + kNth[k] + "most screen time";
            for (char& c : r) c = c == '\n' || c == '\r' ? ' ' : c;
            roles += (k ? "\n" : "") + r;
        }
        // Two or more pictures always get roles: left blank, the plan takes Qwen's list of the people, whose roles are asked for "without
        // gender" - where nobody has one (no drummer, no singer) both came back "the person", and H3 was told "the person is always the person
        // in <Picture 1>, the person is always the person in <Picture 2>" (2026-10-29: after the first hard cut it drew each one as themself).
        // Given roles, the plan still lists Qwen's people by look for the track labeller.
        g[plan]["inputs"]["roles"] = anyRole || o.pictures > 1 ? roles : std::string();
    }
    if (o.shotCheck) AddShotCheck(g);
    KeepPictureStyle(g);
    if (!o.noLipSync) DropNoLipSync(g);
    SetMotionSize(g, o.motion);
    g[plan]["inputs"]["resolution"] = o.resolution;
    g[plan]["inputs"]["seed"] = o.seed;
    g[plan]["inputs"]["clip_seconds"] = o.clipSeconds;
    g[plan]["inputs"]["steps"] = o.steps;
    if (simple) g[plan]["inputs"]["roles"] = "";
    SetSection(g, video, o.start, o.seconds);
    SetModels(g, o.unet, o.textEncoder, o.speed, o.speedLora, o.outName);
    AddVeda(g, o.veda);
    if (Dangling(g, error)) return {};
    return g;
}

json BuildMaskCheck(const MaskedSwapOptions& o, std::string* error) {
    json g;
    if (!Load(L"mask.api.json", g, error)) return {};
    g["3"]["inputs"]["text"] = o.target.empty() ? "person" : o.target;
    g["9"]["inputs"]["expand"] = kMargins[std::clamp(o.margin, 0, 2)];
    if (!o.hatFix) {
        for (const char* id : {"10", "11", "12", "13", "19", "20"}) g.erase(id);
        g["9"]["inputs"]["mask"] = json::array({"7", 0});
    }
    // Two or more people, or any named in "in place of": Openjutsu's check shows only the most prominent person (H3PeopleMask, max_people 1)
    // and ignores the roles - a user named "the man" and "the girl" and saw only him (2026-10-29). Then the swap's own labelling goes in:
    // Qwen3-VL names each tracked person as a subject (H3 Track Crops + Roles, our role fix), and the masks are the labelled people.
    bool anyRole = false;
    for (const std::string& r : o.roles) anyRole |= !r.empty();
    if (o.pictures > 1 || anyRole) {
        std::string people;
        static const char* const kNth[] = {"", "second ", "third ", "fourth "};
        for (int k = 0; k < std::clamp(o.pictures, 1, 4); ++k) {
            std::string r = k < static_cast<int>(o.roles.size()) ? o.roles[k] : std::string();
            if (r.empty()) r = o.pictures == 1 ? "the main person" : std::string("the person with the ") + kNth[k] + "most screen time";
            for (char& c : r) c = c == '\n' || c == '\r' ? ' ' : c;
            people += (k ? "\n" : "") + std::string("<Subject ") + std::to_string(k + 1) + ">: " + r;
        }
        g["qwen"] = {{"class_type", "CLIPLoader"}, {"_meta", {{"title", "Describer: Qwen3-VL 8B"}}},
                     {"inputs", {{"clip_name", "qwen3vl_8b_int8_convrot.safetensors"} /* (the swap's describer, as in advanced.api.json - not H3's text encoder, which only encodes: every label came back empty) */, {"type", "stable_diffusion"}, {"device", "default"}}}};
        g["crops"] = {{"class_type", "H3TrackCrops"}, {"_meta", {{"title", "Show each track per shot"}}},
                      {"inputs", {{"cut_threshold", 40}, {"track_data", json::array({"6", 0})}, {"images", json::array({"5", 0})}, {"people", people}, {"max_crops", 24}}}};
        g["roles"] = {{"class_type", "H3TrackRoles"}, {"_meta", {{"title", "Who is each track (Qwen3-VL)"}}},
                      {"inputs", {{"clip", json::array({"qwen", 0})}, {"crops", json::array({"crops", 0})}, {"segments", json::array({"crops", 1})}}}};
        json labels = json::array({"roles", 0});
        if (o.shotCheck) {
            g["t2c_roles"] = {{"class_type", "Try2CreateRoleFix"}, {"_meta", {{"title", "Role fix (Try2Create)"}}},
                              {"inputs", {{"labels", labels}, {"segments", json::array({"crops", 1})}}}};
            labels = json::array({"t2c_roles", 0});
        }
        g["7"] = {{"class_type", "H3RoleMask"}, {"_meta", {{"title", "The people named (+ headroom)"}}},
                  {"inputs", {{"headroom", 0.1}, {"hold_frames", 2}, {"subject", 0}, {"track_data", json::array({"6", 0})}, {"labels", labels}}}};
        g["8"] = {{"class_type", "H3RoleMask"}, {"_meta", {{"title", "The people named, no headroom (for the mannequin)"}}},
                  {"inputs", {{"headroom", 0.0}, {"hold_frames", 1}, {"subject", 0}, {"track_data", json::array({"6", 0})}, {"labels", labels}}}};
    }
    SetMotionSize(g, o.motion);
    SetSection(g, "1", o.start, o.seconds);
    g["18"]["inputs"]["filename_prefix"] = "masks/MaskCheck";
    g["18"]["inputs"]["save_output"] = true;
    if (Dangling(g, error)) return {};
    return g;
}

json BuildObjectEdit(const ObjectEditOptions& o, std::string* error) {
    json g;
    if (!Load(L"object.api.json", g, error)) return {};
    auto id = [&](const char* t) { return ByTitle(g, t); };
    const std::string plan = id("Object Swap Plan (what to change)"), ref = id("Reference Conditioning"), grow = id("Grow mask"),
                      grey = id("Turn it grey (shape and movement only)"), video = id("Source video");
    if (plan.empty() || ref.empty() || grow.empty() || grey.empty() || video.empty()) {
        if (error) *error = "tools\\openjutsu\\object.api.json is not the one this app knows";
        return {};
    }
    std::vector<std::string> things; // the tracked things (several with commas)
    for (size_t a = 0; a <= o.target.size();) {
        size_t b = std::min(o.target.find(',', a), o.target.size());
        std::string t = o.target.substr(a, b - a);
        while (!t.empty() && t.front() == ' ') t.erase(0, 1);
        while (!t.empty() && t.back() == ' ') t.pop_back();
        if (!t.empty()) things.push_back(t);
        a = b + 1;
    }
    const bool background = o.mode == "background", remove = o.mode == "remove";
    if (o.picture && !remove) { // the picture's describer names only what is copied from it
        std::string named;
        std::vector<std::string> list = background ? std::vector<std::string>{"place (the setting, its lighting and what is around)"} : things;
        for (size_t k = 0; k < list.size(); ++k) named += (k == 0 ? "" : k + 1 == list.size() ? " and the " : ", ") + list[k];
        const int words = 30 + 20 * static_cast<int>(list.size());
        json& d = g[id("Describe picture 1")]["inputs"];
        d["prompt"] = "Describe the " + named + " in this picture in about " + std::to_string(words) + " words" + (list.size() > 1 ? ", one sentence for each" : "") +
                      ": their shape, colours, materials, patterns and details. " +
                      (background ? "" : "Include every layer worn there, outer layer first: an open shirt or jacket over a top is part of the top. ") +
                      "No preamble, and nothing about anything else in the picture.";
        d["max_length"] = 2 * words + 40;
        g[id("Picture 1")]["inputs"]["image"] = "@pic1";
    }
    if (!o.picture || remove) {
        for (const char* t : {"Picture 1", "Shrink for the describer", "Describe picture 1"}) g.erase(id(t));
        g[plan]["inputs"].erase("picture_1");
        g[plan]["inputs"].erase("picture_1_text");
        g[ref]["inputs"].erase("ref_images.ref_image_0");
    }
    // H3 copies a grey shape's outline: a changed top would keep the old silhouette, a removed thing would come back grey
    if (remove || (o.mode == "change" && !o.keepOutline)) {
        g[ref]["inputs"].erase("ref_videos.ref_video_0");
        g[ref]["inputs"].erase("ref_video_audios.ref_video_audio_0");
    }
    if (background) { // the edit area is everything but what stays, reaching a little into its edge; all of it turns grey
        g[grow]["inputs"]["flip_input"] = true;
        g[grey]["inputs"]["mask"] = json::array({grow, 0});
        g[grey]["inputs"]["size"] = json::array({grow, 0});
    }
    static const int kObjectMargins[] = {10, 20, 40};
    g[grow]["inputs"]["expand"] = kObjectMargins[std::clamp(o.margin, 0, 2)];
    const int most = o.biggestOnly ? static_cast<int>(things.size()) : 0;
    for (const char* t : {"Tracked area", "Tracked area, tight (for the grey shape)"}) g[id(t)]["inputs"]["max_people"] = most;
    g[id("SAM: what to change")]["inputs"]["text"] = o.target;
    json& p = g[plan]["inputs"];
    p["mode"] = o.mode, p["target"] = o.target, p["change_to"] = o.changeTo, p["keep_outline"] = o.keepOutline, p["seed"] = o.seed;
    p["clip_seconds"] = o.clipSeconds, p["resolution"] = o.resolution, p["steps"] = o.steps;
    SetSection(g, video, o.start, o.seconds);
    SetModels(g, o.unet, o.textEncoder, o.speed, o.speedLora, o.outName);
    AddVeda(g, o.veda);
    if (Dangling(g, error)) return {};
    return g;
}
