// The LoRA browser (Tools > LoRA browser, or "See what they do" under + LoRA): every LoRA ComfyUI has, as cards, and for the one picked what
// it does - the same prompt and seed made without it and at a few strengths, side by side - with what its file says about it (trigger words,
// the words it was trained on, the model it was trained on, how long). Nothing is made until you press a button (never on its own).
//
// Pictures are made by the model the LoRA is for (Krea 2, Qwen Image 2.1, MOSS-Image: about 4-12 s each); MiniMax H3 LoRAs get two short
// 480p clips (without / with, about a minute each). The "without" picture of a prompt is made once and shared by every LoRA of that model.
// Kept in %LOCALAPPDATA%\Try2Create\lora_previews (index.json; the pictures stay where ComfyUI's results are fetched to).
//
// The showcase (2026-10-23: a LoRA at 1.5 without its trigger words does little): of 57 LoRAs tested, one had its trigger
// word where trainers are meant to put it; others had it in their title ("sh1row", "In the style of 5h1ba"), their training captions, or only on
// their Civitai page. So before a comparison its trigger words are gathered from all of these (Civitai asked by the file's SHA-256 - nothing else
// is sent - and kept in civitai.json), and the prompt writer writes a prompt with them that shows what the LoRA is for (its character, a subject
// in its style, its concept), from the creator's own example prompts when there are some. Comparisons can be cancelled.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <windows.h>
#include <commdlg.h>

#include "clef.h"
#include "comfy.h"
#include "image_texture.h"
#include "imgui.h"
#include "json.hpp"
#include "metadata.h"
#include "ui_extra.h"
#include "ui_theme.h"
#include "video_util.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

int ApplyLoras(json& graph, const std::vector<LoraEntry>& loras, bool replaceOwn); // (comfy.cpp)

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

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The first n bytes, "..." after them, never cutting a UTF-8 character in half (a half one made saving throw: the crash of 2026-10-23)
std::string CutUtf8(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    return s.substr(0, n) + "...";
}

std::wstring Dir() {
    wchar_t buf[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    const std::wstring d = std::wstring(buf) + L"\\Try2Create\\lora_previews";
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

std::string ShortName(const std::string& name) {
    const size_t at = name.find_last_of("/\\");
    std::string s = at == std::string::npos ? name : name.substr(at + 1);
    if (s.size() > 12 && Lower(s.substr(s.size() - 12)) == ".safetensors") s.resize(s.size() - 12);
    return s;
}

// The models a LoRA can be shown on here, and their test prompts
bool CanShow(const std::string& model) { return model == "Krea 2" || model == "Qwen Image 2.1" || model == "MOSS-Image" || model == "MiniMax H3"; }
bool IsVideoModel(const std::string& model) { return model == "MiniMax H3"; }
double SecondsEach(const std::string& model) { return model == "MOSS-Image" ? 4 : model == "Krea 2" ? 8 : model == "Qwen Image 2.1" ? 13 : 70; }

struct Preset {
    const char* name;
    const char* text;
};
const Preset kPicturePresets[] = {
    {"Portrait", "A close-up portrait photograph of a woman in her thirties with freckles and curly auburn hair, looking at the camera, soft window light, plain background."},
    {"People", "A man and a woman standing side by side on a city street, full body, casual clothes, daylight."},
    {"Scene", "A cozy living room in the evening: a sofa, a reading lamp, plants on the windowsill and a cat asleep on a cushion."},
    {"Object", "A ceramic coffee mug on a wooden table next to a notebook and a pen, morning light."},
};
// The standard prompt (2026-10-23): the same for every LoRA of a model, with that LoRA's trigger words in front, so the
// comparisons of different LoRAs can be set side by side. It has what most LoRAs change: two faces and hands, clothes, a place, light, objects.
const char* kStandardPicture = "Two friends sit at a small table by a sunny cafe window: a young woman with freckles and long dark hair in a green sweater, "
                               "and a bearded man in a denim jacket holding a cup of coffee. A city street with trees outside, a plant and an open book on "
                               "the table, warm afternoon light.";
const char* kStandardVideo = "A young woman in a red coat walks toward the camera along a rainy city street at dusk, shop lights reflecting in the "
                             "puddles; she stops, smiles and says: \"Here we go.\" The camera slowly pushes in. Audio: rain, footsteps, her voice.";
const Preset kVideoPresets[] = {
    {"Walk", "A young woman walks toward the camera along a beach at sunset; the wind moves her hair; she stops and smiles. Audio: waves and wind."},
    {"Talk", "A man sitting in a cafe looks at the camera and says: \"You won't believe what happened today.\" Audio: his voice, cafe chatter."},
    {"Action", "A dog runs across a grassy park, catches a frisbee in the air and lands. Audio: barking, wind, distant children."},
};

// ---- what a file says about itself
struct Info {
    bool read = false;
    std::vector<std::string> triggers; // words to put in the prompt (trigger phrase, trained words)
    std::vector<std::string> tags;     // the words its training captions used most
    std::vector<std::pair<std::string, std::string>> lines;
    std::wstring thumb; // a picture it carries ("" = none)
    std::string title;  // modelspec.title (often its trigger phrase)
};
std::map<std::string, Info> g_info;

// What Civitai says about a LoRA (asked by the file's SHA-256)
struct Civit {
    bool found = false;
    std::string name, type, base, description;
    std::vector<std::string> words;    // its trigger ("trained") words
    std::vector<std::string> examples; // the prompts of the creator's example pictures
};

std::string Base64Decode(const std::string& in) {
    static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, bits = -8;
    for (char c : in) {
        const size_t p = chars.find(c);
        if (p == std::string::npos) continue;
        val = (val << 6) + static_cast<int>(p);
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

const Info& InfoOf(const std::string& name) {
    Info& in = g_info[name];
    if (in.read) return in;
    in.read = true;
    const std::wstring file = LoraLocalFile(name);
    if (file.empty()) return in;
    const json m = json::parse(LoraHeaderMetadata(file), nullptr, false);
    if (!m.is_object()) return in;
    auto str = [&](const char* k) { return m.contains(k) && m[k].is_string() ? m[k].get<std::string>() : std::string(); };
    auto add = [&](const char* label, std::string v) {
        if (v.empty() || v == "None" || v == "null") return;
        if (v.size() > 420) v = CutUtf8(v, 417);
        in.lines.push_back({label, v});
    };
    // trigger words: the keys trainers and sites use for them
    for (auto it = m.begin(); it != m.end(); ++it) {
        const std::string k = Lower(it.key());
        if (!it.value().is_string()) continue;
        if (k.find("trigger") != std::string::npos || k.find("activation") != std::string::npos || k == "trained_words" || k == "ss_trigger_words") {
            std::string v = it.value().get<std::string>();
            const json arr = json::parse(v, nullptr, false); // (some keep a JSON list)
            if (arr.is_array()) {
                for (const json& a : arr) {
                    if (a.is_string() && !a.get<std::string>().empty()) in.triggers.push_back(a.get<std::string>());
                }
            } else if (!v.empty()) {
                in.triggers.push_back(v);
            }
        }
    }
    // the words of its training captions, most used first
    const std::string freq = str("ss_tag_frequency");
    if (!freq.empty()) {
        const json f = json::parse(freq, nullptr, false);
        std::map<std::string, long long> count;
        if (f.is_object()) {
            for (auto d = f.begin(); d != f.end(); ++d) {
                if (!d.value().is_object()) continue;
                for (auto t = d.value().begin(); t != d.value().end(); ++t) {
                    if (t.value().is_number()) count[t.key()] += t.value().get<long long>();
                }
            }
        }
        std::vector<std::pair<long long, std::string>> byCount;
        for (const auto& [t, c] : count) byCount.push_back({c, t});
        std::sort(byCount.rbegin(), byCount.rend());
        for (size_t i = 0; i < byCount.size() && in.tags.size() < 14; ++i) {
            const std::string& t = byCount[i].second;
            if (!t.empty() && t.size() <= 60 && t.find_first_not_of(" \t,.") != std::string::npos) in.tags.push_back(t);
        }
    }
    in.title = str("modelspec.title");
    add("Title", in.title);
    add("Author", str("modelspec.author"));
    add("Description", str("modelspec.description"));
    std::string base = str("ss_sd_model_name");
    if (base.empty()) base = str("base_model");
    if (base.empty()) base = str("ss_base_model_version");
    add("Trained on", base);
    std::string training;
    if (!str("ss_num_train_images").empty()) training += str("ss_num_train_images") + " pictures";
    const std::string epochs = !str("ss_epoch").empty() ? str("ss_epoch") : str("ss_num_epochs");
    if (!epochs.empty()) training += (training.empty() ? "" : ", ") + epochs + " epochs";
    const std::string steps = !str("ss_steps").empty() ? str("ss_steps") : str("ss_max_train_steps");
    if (!steps.empty()) training += (training.empty() ? "" : ", ") + steps + " steps";
    add("Training", training);
    std::string rank = str("ss_network_dim");
    if (rank.empty()) rank = str("lora_rank");
    if (rank.empty()) rank = str("rank");
    if (rank.empty()) rank = str("training_rank");
    add("Rank", rank);
    add("Resolution", !str("ss_resolution").empty() ? str("ss_resolution") : str("modelspec.resolution"));
    add("How to use", !str("ss_usage_hint").empty() ? str("ss_usage_hint") : str("usage"));
    add("Notes", str("notes"));
    // a picture it carries (modelspec.thumbnail: a data URL)
    const std::string th = str("modelspec.thumbnail");
    const size_t comma = th.find(',');
    if (th.rfind("data:image/", 0) == 0 && comma != std::string::npos) {
        const std::string ext = th.find("jpeg") != std::string::npos || th.find("jpg") != std::string::npos ? ".jpg" : ".png";
        const std::wstring out = Dir() + L"\\thumb_" + std::to_wstring(std::hash<std::string>()(name)) + W(ext);
        std::error_code ec;
        if (!fs::exists(out, ec)) std::ofstream(out, std::ios::binary) << Base64Decode(th.substr(comma + 1));
        in.thumb = out;
    }
    return in;
}

// ---- the comparisons made so far
struct Shot {
    double strength = 0; // 0 = without it
    std::wstring path;
    bool video = false;
};
struct Comparison {
    std::string prompt;
    std::vector<Shot> shots; // in order of strength
};
std::map<std::string, Comparison> g_cmp;  // by LoRA file name (lower case)
std::map<std::string, std::wstring> g_base; // "model|prompt" -> the picture without any LoRA
std::map<std::string, std::string> g_showcase; // by Key(lora): the last showcase prompt written for it
std::map<std::string, double> g_pick;          // by Key(lora): the strength whose picture is its showcase (right-click > Use as its showcase picture)
bool g_loaded = false;

std::string Key(const std::string& name) { return LoraFileName(name); }

// Its showcase picture: its comparison at the strength picked for it (else the strongest made), else a picture its file carries
std::wstring CardPicture(const std::string& lora) {
    auto it = g_cmp.find(Key(lora));
    if (it != g_cmp.end() && !it->second.shots.empty()) {
        auto p = g_pick.find(Key(lora));
        if (p != g_pick.end())
            for (const Shot& s : it->second.shots)
                if (std::fabs(s.strength - p->second) < 1e-6) return s.path;
        return it->second.shots.back().path;
    }
    return InfoOf(lora).thumb;
}

void Save() {
    json j = {{"comparisons", json::object()}, {"base", json::object()}};
    for (const auto& [k, c] : g_cmp) {
        json shots = json::array();
        for (const Shot& s : c.shots) shots.push_back({{"strength", s.strength}, {"path", U8(s.path)}, {"video", s.video}});
        j["comparisons"][k] = {{"prompt", c.prompt}, {"shots", shots}};
    }
    for (const auto& [k, p] : g_base) j["base"][k] = U8(p);
    j["showcase"] = g_showcase;
    j["pick"] = g_pick;
    const std::string text = j.dump(1, ' ', false, json::error_handler_t::replace); // (before the file is emptied: text that is not UTF-8 never throws)
    std::ofstream(Dir() + L"\\index.json", std::ios::binary | std::ios::trunc) << text;
}

void Load() {
    if (g_loaded) return;
    g_loaded = true;
    std::ifstream f(Dir() + L"\\index.json", std::ios::binary);
    if (!f) return;
    const json j = json::parse(f, nullptr, false);
    if (!j.is_object()) return;
    std::error_code ec;
    if (j.contains("comparisons") && j["comparisons"].is_object()) {
        for (auto it = j["comparisons"].begin(); it != j["comparisons"].end(); ++it) {
            Comparison c;
            c.prompt = it.value().value("prompt", std::string());
            for (const json& s : it.value().value("shots", json::array())) {
                Shot sh{s.value("strength", 0.0), W(s.value("path", std::string())), s.value("video", false)};
                if (fs::exists(sh.path, ec)) c.shots.push_back(sh);
            }
            if (!c.shots.empty()) g_cmp[it.key()] = c;
        }
    }
    if (j.contains("base") && j["base"].is_object()) {
        for (auto it = j["base"].begin(); it != j["base"].end(); ++it) {
            const std::wstring p = W(it.value().get<std::string>());
            if (fs::exists(p, ec)) g_base[it.key()] = p;
        }
    }
    if (j.contains("showcase") && j["showcase"].is_object())
        for (auto it = j["showcase"].begin(); it != j["showcase"].end(); ++it)
            if (it.value().is_string()) g_showcase[it.key()] = it.value().get<std::string>();
    if (j.contains("pick") && j["pick"].is_object())
        for (auto it = j["pick"].begin(); it != j["pick"].end(); ++it)
            if (it.value().is_number()) g_pick[it.key()] = it.value().get<double>();
}

// ---- making them
struct Pending {
    int activity = 0;
    std::string lora; // "" = the picture without any LoRA
    std::string model, prompt;
    double strength = 0;
};
std::vector<Pending> g_pending;

constexpr long long kSeed = 20261020; // the same seed for every picture of a comparison

int SendOne(AppState& app, const std::string& model, const std::string& prompt, const std::vector<LoraEntry>& loras) {
    if (model == "Krea 2") return SendKreaPicture(app, prompt, 832, 1040, 1, "LoraPreview/krea", loras, kSeed);
    if (model == "Qwen Image 2.1") return SendQwenPicture(app, prompt, 4, 5, 1, "LoraPreview/qwen", loras, kSeed);
    if (model == "MOSS-Image") return SendMossPrompt(app, prompt, 832, 1040, 1, "LoraPreview/moss", loras, kSeed);
    if (model == "MiniMax H3") return SendH3LoraPreview(app, prompt, loras);
    return 0;
}

bool Waiting(const std::string& lora) {
    for (const Pending& p : g_pending) {
        if (p.lora == lora) return true;
    }
    return false;
}

// One LoRA's comparison: the shared "without" (made if this prompt has none yet) and one picture per strength
int Compare(AppState& app, const std::string& lora, const std::string& model, const std::string& prompt, const std::vector<double>& strengths) {
    int sent = 0;
    const std::string baseKey = model + "|" + prompt;
    Comparison& c = g_cmp[Key(lora)];
    if (c.prompt != prompt) c = Comparison{prompt, {}};
    c.shots.erase(std::remove_if(c.shots.begin(), c.shots.end(), [](const Shot& s) { return s.strength != 0; }), c.shots.end()); // made again
    const bool baseThere = g_base.count(baseKey) > 0;
    bool baseWaiting = false;
    for (const Pending& p : g_pending) baseWaiting = baseWaiting || (p.lora.empty() && p.model == model && p.prompt == prompt);
    if (!baseThere && !baseWaiting) {
        const int id = SendOne(app, model, prompt, {});
        if (id) g_pending.push_back({id, std::string(), model, prompt, 0}), ++sent;
    }
    for (double s : strengths) {
        const int id = SendOne(app, model, prompt, {{lora, s, true}});
        if (id) g_pending.push_back({id, lora, model, prompt, s}), ++sent;
    }
    return sent;
}

void PumpPending(AppState& app) {
    if (g_pending.empty() || !app.comfySvc) return;
    const std::vector<ComfyActivity> acts = app.comfySvc->Activity();
    bool changed = false;
    for (size_t i = 0; i < g_pending.size();) {
        const Pending p = g_pending[i];
        const ComfyActivity* a = nullptr;
        for (const ComfyActivity& x : acts) {
            if (x.id == p.activity) a = &x;
        }
        if (a && a->state != ComfyState::Done && a->state != ComfyState::Failed) {
            ++i;
            continue;
        }
        g_pending.erase(g_pending.begin() + i);
        if (!a || a->state != ComfyState::Done) continue;
        std::wstring file;
        bool video = false;
        for (const ComfyResult& r : a->files) {
            if (!r.localPath.empty() && !r.audio) file = r.localPath, video = r.video;
        }
        if (file.empty()) continue;
        changed = true;
        if (p.lora.empty()) {
            g_base[p.model + "|" + p.prompt] = file;
        } else {
            Comparison& c = g_cmp[Key(p.lora)];
            if (c.prompt != p.prompt) c = Comparison{p.prompt, {}};
            c.shots.erase(std::remove_if(c.shots.begin(), c.shots.end(), [&](const Shot& s) { return s.strength == p.strength; }), c.shots.end());
            c.shots.push_back({p.strength, file, video});
            std::sort(c.shots.begin(), c.shots.end(), [](const Shot& a2, const Shot& b2) { return a2.strength < b2.strength; });
        }
    }
    if (changed) Save();
}

// ---- the showcase: trigger words from everywhere, and a prompt written for the LoRA
std::mutex g_civitMutex;
std::map<std::string, Civit> g_civit; // by file name (lower case) + size
bool g_civitLoaded = false;

std::string CivitKey(const std::string& lora) {
    std::error_code ec;
    const std::wstring f = LoraLocalFile(lora);
    return Key(lora) + "|" + std::to_string(f.empty() ? 0 : static_cast<long long>(fs::file_size(f, ec)));
}

void LoadCivit() {
    if (g_civitLoaded) return;
    g_civitLoaded = true;
    std::ifstream f(Dir() + L"\\civitai.json", std::ios::binary);
    const json j = f ? json::parse(f, nullptr, false) : json();
    if (!j.is_object()) return;
    std::lock_guard<std::mutex> lock(g_civitMutex);
    for (auto it = j.begin(); it != j.end(); ++it) {
        Civit c;
        c.found = it.value().value("found", false);
        c.name = it.value().value("name", std::string()), c.type = it.value().value("type", std::string());
        c.base = it.value().value("base", std::string()), c.description = it.value().value("description", std::string());
        c.words = it.value().value("words", std::vector<std::string>());
        c.examples = it.value().value("examples", std::vector<std::string>());
        g_civit[it.key()] = c;
    }
}

void SaveCivit() {
    json j = json::object();
    {
        std::lock_guard<std::mutex> lock(g_civitMutex);
        for (const auto& [k, c] : g_civit)
            j[k] = {{"found", c.found}, {"name", c.name}, {"type", c.type}, {"base", c.base}, {"description", c.description}, {"words", c.words}, {"examples", c.examples}};
    }
    const std::string text = j.dump(1, ' ', false, json::error_handler_t::replace);
    std::ofstream(Dir() + L"\\civitai.json", std::ios::binary | std::ios::trunc) << text;
}

std::string StripHtml(const std::string& s) {
    std::string out;
    bool tag = false;
    for (char c : s) {
        if (c == '<') tag = true;
        else if (c == '>') tag = false, out += ' ';
        else if (!tag) out += c;
    }
    std::string t;
    for (char c : out) // (runs of spaces as one)
        if (!(c == ' ' || c == '\n' || c == '\r' || c == '\t') || (!t.empty() && t.back() != ' ')) t += (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
    return t;
}

// Asked once per file (a background thread): its SHA-256, then Civitai's model-version for it
Civit LookUpCivit(const std::wstring& file) {
    Civit c;
    std::string hash;
    RunCaptureHidden(L"powershell.exe -NoProfile -Command \"(Get-FileHash -Algorithm SHA256 -LiteralPath '" + file + L"').Hash\"", hash, false);
    std::string h;
    for (char ch : hash)
        if (isxdigit(static_cast<unsigned char>(ch))) h += static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    if (h.size() != 64) return c;
    std::string body;
    RunCaptureHidden(L"curl.exe -s -m 20 \"https://civitai.com/api/v1/model-versions/by-hash/" + W(h) + L"\"", body, false);
    const json j = json::parse(body, nullptr, false);
    if (!j.is_object() || !j.contains("model") || !j["model"].is_object()) return c;
    c.found = true;
    c.name = j["model"].value("name", std::string());
    c.type = j["model"].value("type", std::string());
    c.base = j.value("baseModel", std::string());
    if (j.contains("description") && j["description"].is_string()) c.description = StripHtml(j["description"].get<std::string>());
    c.description = CutUtf8(c.description, 697);
    if (j.contains("trainedWords") && j["trainedWords"].is_array())
        for (const json& w : j["trainedWords"])
            if (w.is_string() && !w.get<std::string>().empty()) c.words.push_back(w.get<std::string>());
    if (j.contains("images") && j["images"].is_array())
        for (const json& im : j["images"]) {
            if (c.examples.size() >= 3) break;
            if (!im.is_object() || !im.contains("meta") || !im["meta"].is_object()) continue;
            std::string p = im["meta"].value("prompt", std::string());
            if (p.empty()) continue;
            p = CutUtf8(p, 497);
            c.examples.push_back(p);
        }
    return c;
}

// A title that reads like a trigger phrase: short, and with a made-up word ("sh1row", "m0nm0n") or "style"/"illustration" in it
bool TitleIsTrigger(const std::string& title) {
    if (title.empty() || title.size() > 60) return false;
    const std::string t = Lower(title);
    if (t.find("style") != std::string::npos || t.find("illustration") != std::string::npos) return true;
    std::string word;
    for (size_t i = 0; i <= t.size(); ++i) {
        const char c = i < t.size() ? t[i] : ' ';
        if (isalnum(static_cast<unsigned char>(c)) || c == '_') {
            word += c;
            continue;
        }
        bool letter = false, digit = false;
        for (char w : word) letter |= isalpha(static_cast<unsigned char>(w)) != 0, digit |= isdigit(static_cast<unsigned char>(w)) != 0;
        if (letter && digit && word.size() >= 4 && word.find('_') == std::string::npos) return true; // (not "v3_rank_64")
        word.clear();
    }
    return false;
}

// Every trigger word found, with where it came from: its file, its title, Civitai
std::vector<std::pair<std::string, std::string>> TriggersOf(const std::string& lora) {
    std::vector<std::pair<std::string, std::string>> out;
    auto add = [&](const std::string& w, const char* from) {
        std::string t = w;
        while (!t.empty() && (t.back() == ' ' || t.back() == '.' || t.back() == ',')) t.pop_back();
        if (t.empty()) return;
        for (const auto& o : out)
            if (Lower(o.first) == Lower(t)) return;
        out.push_back({t, from});
    };
    const Info& in = InfoOf(lora);
    for (const std::string& t : in.triggers) add(t, "its file");
    if (TitleIsTrigger(in.title)) add(in.title, "its title");
    std::lock_guard<std::mutex> lock(g_civitMutex);
    auto c = g_civit.find(CivitKey(lora));
    if (c != g_civit.end())
        for (const std::string& w : c->second.words) add(w, "Civitai");
    return out;
}

const char* kShowcaseSystem =
    "You write ONE prompt that shows off what a LoRA does, for a side-by-side test: the same prompt is made without the LoRA and with it, so the "
    "prompt must give the LoRA something to change. Rules:\n"
    "- Use its trigger words exactly as given (they switch it on). If there are many, use the ones for its main subject or style.\n"
    "- Pick what the LoRA is for: a character LoRA shows that character; a style LoRA a typical subject in that style; a concept, pose or "
    "clothing LoRA a scene showing it; a realism, detail or lighting LoRA a portrait or scene where that shows. The training captions, the "
    "creator's example prompts and the description tell you which. When there are example prompts, base yours on them (shorter).\n"
    "- {KIND}\n"
    "- People are dressed, unless the LoRA is made for nudity or adult content (its name, type, description or captions say so): its "
    "example prompts may be nude where the LoRA is only a style - leave that out.\n"
    "- Never put anyone under 18 in suggestive or sexual content.\n"
    "Reply with ONLY the prompt: no preface, no quotes, no notes.";

std::string ShowcaseFallback(const std::string& lora, const std::string& model, const std::string& prompt) {
    const auto trig = TriggersOf(lora);
    {
        std::lock_guard<std::mutex> lock(g_civitMutex);
        auto c = g_civit.find(CivitKey(lora));
        if (c != g_civit.end() && !c->second.examples.empty()) return c->second.examples.front(); // (the creator's own)
    }
    std::string t;
    for (size_t i = 0; i < trig.size() && i < 3; ++i) t += (t.empty() ? "" : ", ") + trig[i].first;
    return t.empty() ? prompt : t + ", " + prompt;
}

// (a background thread) the trigger words gathered, then the prompt written; "" = the writer is not there (the fallback is used)
std::string WriteShowcase(std::string lora, std::string model, std::wstring file, std::string comfyUrl, bool askCivit, Info info, bool write) {
    const std::string ck = CivitKey(lora);
    bool known = false;
    {
        std::lock_guard<std::mutex> lock(g_civitMutex);
        known = g_civit.count(ck) > 0;
    }
    if (askCivit && !known && !file.empty()) {
        const Civit c = LookUpCivit(file);
        {
            std::lock_guard<std::mutex> lock(g_civitMutex);
            g_civit[ck] = c;
        }
        SaveCivit();
    }
    if (!write || !WriterConfigured()) return {}; // (the standard prompt: only its trigger words were wanted)
    Civit c;
    {
        std::lock_guard<std::mutex> lock(g_civitMutex);
        auto it = g_civit.find(ck);
        if (it != g_civit.end()) c = it->second;
    }
    std::string user = "The LoRA's file: " + ShortName(lora) + "\nIt is for: " + model + "\n";
    std::vector<std::string> words = info.triggers;
    if (TitleIsTrigger(info.title)) words.push_back(info.title);
    for (const std::string& w : c.words) words.push_back(w);
    if (!words.empty()) {
        user += "Its trigger words:";
        for (const std::string& w : words) user += " \"" + w + "\"";
        user += "\n";
    } else {
        user += "Its trigger words: none known (then use the words its training captions use most, if they look like a trigger).\n";
    }
    if (!info.title.empty() && !TitleIsTrigger(info.title)) user += "Its title: " + info.title + "\n";
    if (!info.tags.empty()) {
        user += "The words its training captions used most:";
        for (const std::string& t : info.tags) user += " " + t + ";";
        user += "\n";
    }
    if (c.found) {
        user += "On Civitai: \"" + c.name + "\" (" + c.type + (c.base.empty() ? "" : ", made for " + c.base) + ")\n";
        if (!c.description.empty()) user += "Its description: " + c.description + "\n";
        for (size_t i = 0; i < c.examples.size(); ++i) user += "The creator's example prompt " + std::to_string(i + 1) + ": " + c.examples[i] + "\n";
    }
    std::string system = kShowcaseSystem;
    const std::string kind = IsVideoModel(model) ? "It is a 3-second video: one clear action, a camera move, and a last line \"Audio: ...\" with what is heard. Under 70 words."
                                                 : "It is one picture. Under 70 words, plain descriptive English.";
    system.replace(system.find("{KIND}"), 6, kind);
    const ClefSession session = WriterAcquire(comfyUrl, nullptr);
    std::string out;
    if (session.ok) {
        const json schema = {{"type", "object"}, {"properties", {{"prompt", {{"type", "string"}, {"minLength", 60}, {"maxLength", 700}}}}}, {"required", {"prompt"}}};
        std::string js;
        if (WriterChatJson(system + "\nReply as JSON: prompt.", user, {}, schema.dump(), 500, 0.5, js, nullptr)) {
            const json a = json::parse(js, nullptr, false);
            if (a.is_object() && a.contains("prompt") && a["prompt"].is_string()) out = a["prompt"].get<std::string>();
        }
        if (out.empty()) WriterChat(system, user, {}, 400, 0.5, out, nullptr);
    }
    WriterRelease(session, false);
    while (!out.empty() && (out.front() == '"' || out.front() == ' ' || out.front() == '\n')) out.erase(out.begin());
    while (!out.empty() && (out.back() == '"' || out.back() == ' ' || out.back() == '\n')) out.pop_back();
    return out.size() < 12 ? std::string() : out;
}

// A comparison waiting for its showcase prompt; they are written first, all of them, then sent (the writer and the picture model do not swap)
struct Prep {
    std::string lora, model;
    std::vector<double> strengths;
    std::string prompt; // (written)
    int mode = 0;       // 0 a showcase is written, 1 the standard prompt (only its trigger words looked up first)
};
std::deque<Prep> g_prepQueue; // still to write
std::vector<Prep> g_prepDone; // written, waiting for the rest
std::future<std::string> g_prepFut;
Prep g_prepNow;
bool g_prepDrop = false; // cancelled while its prompt was being written: what comes back is dropped

// Civitai looked up for a LoRA put in a maker's list (its trigger words go into the prompt): one at a time, once per file
std::deque<std::string> g_lookQueue;
std::future<void> g_lookFut;

void PumpLookups() {
    if (g_lookFut.valid() && g_lookFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            g_lookFut.get();
        } catch (...) {
        }
    }
    if (g_lookFut.valid() || g_lookQueue.empty()) return;
    const std::string lora = g_lookQueue.front();
    g_lookQueue.pop_front();
    const std::wstring file = LoraLocalFile(lora);
    const std::string ck = CivitKey(lora);
    if (file.empty()) return;
    g_lookFut = std::async(std::launch::async, [file, ck] {
        const Civit c = LookUpCivit(file);
        {
            std::lock_guard<std::mutex> lock(g_civitMutex);
            g_civit[ck] = c;
        }
        SaveCivit();
    });
}

void WantCivit(const std::string& lora, bool civitai) {
    if (!civitai || lora.empty()) return;
    {
        std::lock_guard<std::mutex> lock(g_civitMutex);
        if (g_civit.count(CivitKey(lora))) return;
    }
    if (std::find(g_lookQueue.begin(), g_lookQueue.end(), lora) == g_lookQueue.end()) g_lookQueue.push_back(lora);
}

bool Preparing(const std::string& lora) {
    if (g_prepFut.valid() && g_prepNow.lora == lora) return true;
    for (const Prep& p : g_prepQueue)
        if (p.lora == lora) return true;
    for (const Prep& p : g_prepDone)
        if (p.lora == lora) return true;
    return false;
}

// ---- the window
struct Browser {
    bool open = false;
    std::string model = "Krea 2"; // the filter ("" = all)
    char search[64] = {};
    std::string selected;
    float card = 150.0f;
    std::string prompt;
    bool addTriggers = true;
    int mode = 0;         // the prompt: 0 a showcase written for each LoRA (its trigger words, its examples), 1 the standard one, 2 the one below
    bool civitai = true;  // look its trigger words up on Civitai (by the file's SHA-256)
    bool strengths[5] = {false, true, true, true, false}; // 0.5, 0.75, 1.0, 1.5, 2.0 (see kStrengths)
    int big = -1;    // a picture of the strip shown big (-1 = none)
    int confirmAll = 0;          // asked to click again: 1 make the missing ones, 2 redo all
    std::set<std::string> picked; // Ctrl+clicked: redone together
} g_b;
const double kStrengths[5] = {0.5, 0.75, 1.0, 1.5, 2.0};

std::string ModelFolderOf(const LoraOffer& o) { return o.model.empty() ? std::string("Not known") : o.model; }

std::vector<LoraEntry>* ListFor(AppState& app, const std::string& model) {
    if (model == "Krea 2") return &app.comfy.kreaLoras;
    if (model == "Qwen Image 2.1") return &app.comfy.qwenLoras;
    if (model == "MOSS-Image") return &app.comfy.mossLoras;
    if (model == "MiniMax H3") return &app.comfy.h3Loras;
    return nullptr;
}

std::string PromptFor(const std::string& model, const std::string& lora) {
    if (g_b.mode == 1) { // the standard prompt, with every trigger word found (up to 3)
        const auto trig = TriggersOf(lora);
        std::string t;
        for (size_t i = 0; i < trig.size() && i < 3; ++i) t += (t.empty() ? "" : ", ") + trig[i].first;
        const std::string s = IsVideoModel(model) ? kStandardVideo : kStandardPicture;
        return t.empty() ? s : t + ", " + s;
    }
    std::string p = PromptText(g_b.prompt);
    if (p.empty()) p = IsVideoModel(model) ? kVideoPresets[0].text : kPicturePresets[0].text;
    const Info& in = InfoOf(lora);
    if (g_b.addTriggers && !in.triggers.empty()) {
        std::string t;
        for (size_t i = 0; i < in.triggers.size() && i < 3; ++i) t += (t.empty() ? "" : ", ") + in.triggers[i];
        p = t + ", " + p;
    }
    return p;
}

// A comparison asked for: with a showcase prompt written first (queued), or the prompt below at once
void Ask(AppState& app, const std::string& lora, const std::string& model, const std::vector<double>& strengths, int mode);

std::vector<double> ChosenStrengths(const std::string& model) {
    if (IsVideoModel(model)) return {1.0}; // (a clip each: without and at 1)
    std::vector<double> s;
    for (int i = 0; i < 5; ++i) {
        if (g_b.strengths[i]) s.push_back(kStrengths[i]);
    }
    if (s.empty()) s.push_back(1.0);
    return s;
}

void Ask(AppState& app, const std::string& lora, const std::string& model, const std::vector<double>& strengths, int mode) {
    if (Waiting(lora) || Preparing(lora)) return;
    if (mode == 2) { // my prompt
        Compare(app, lora, model, PromptFor(model, lora), strengths);
        return;
    }
    g_prepQueue.push_back({lora, model, strengths, std::string(), mode});
}

void PumpPrep(AppState& app) {
    if (g_prepFut.valid() && g_prepFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        std::string p;
        try {
            p = g_prepFut.get();
        } catch (...) { // (whatever went wrong while it was written: the fallback prompt)
            p.clear();
        }
        if (g_prepDrop) {
            g_prepDrop = false;
        } else {
            std::string base = PromptText(g_b.prompt);
            if (base.empty()) base = IsVideoModel(g_prepNow.model) ? kVideoPresets[0].text : kPicturePresets[0].text;
            if (g_prepNow.mode == 1) { // the standard prompt, with the trigger words now known
                const int was = g_b.mode;
                g_b.mode = 1;
                p = PromptFor(g_prepNow.model, g_prepNow.lora);
                g_b.mode = was;
            } else {
                if (p.empty()) p = ShowcaseFallback(g_prepNow.lora, g_prepNow.model, base); // (no writer: its own example, or its trigger words + the prompt)
                g_showcase[Key(g_prepNow.lora)] = p;
            }
            g_prepNow.prompt = p;
            g_prepDone.push_back(g_prepNow);
            Save();
        }
    }
    if (!g_prepFut.valid() && !g_prepQueue.empty()) {
        g_prepNow = g_prepQueue.front();
        g_prepQueue.pop_front();
        g_prepFut = std::async(std::launch::async, WriteShowcase, g_prepNow.lora, g_prepNow.model, LoraLocalFile(g_prepNow.lora), app.comfy.url, g_b.civitai,
                               InfoOf(g_prepNow.lora), g_prepNow.mode == 0);
    }
    if (!g_prepFut.valid() && g_prepQueue.empty() && !g_prepDone.empty()) { // all written: now made
        for (const Prep& p : g_prepDone) Compare(app, p.lora, p.model, p.prompt, p.strengths);
        g_prepDone.clear();
    }
}

// Cancelled: one LoRA's comparison ("" = all of them) - those still to be written, and the pictures queued or being made in ComfyUI
void CancelComparisons(AppState& app, const std::string& lora) {
    auto mine = [&](const std::string& l) { return lora.empty() || l == lora; };
    g_prepQueue.erase(std::remove_if(g_prepQueue.begin(), g_prepQueue.end(), [&](const Prep& p) { return mine(p.lora); }), g_prepQueue.end());
    g_prepDone.erase(std::remove_if(g_prepDone.begin(), g_prepDone.end(), [&](const Prep& p) { return mine(p.lora); }), g_prepDone.end());
    if (g_prepFut.valid() && mine(g_prepNow.lora)) g_prepDrop = true;
    for (size_t i = 0; i < g_pending.size();) {
        const Pending& p = g_pending[i];
        bool cancel = mine(p.lora);
        if (p.lora.empty() && !lora.empty()) { // a "without" picture: shared by every LoRA with the same prompt - it goes when nobody else waits for it
            cancel = true;
            for (const Pending& q : g_pending)
                if (!q.lora.empty() && q.lora != lora && q.model == p.model && q.prompt == p.prompt) cancel = false;
        }
        if (cancel) {
            if (app.comfySvc) app.comfySvc->Cancel(p.activity);
            g_pending.erase(g_pending.begin() + i);
        } else {
            ++i;
        }
    }
}

// Full-size pictures for the comparison (the library's thumbnails are 192 px: too small to see what a LoRA changes); the last 24 kept
std::map<std::wstring, LoadedImage> g_full;
std::vector<std::wstring> g_fullOrder;

const LoadedImage* Full(AppState& app, const std::wstring& path) {
    auto it = g_full.find(path);
    if (it != g_full.end()) return it->second.srv ? &it->second : nullptr;
    LoadedImage img;
    std::string err;
    if (!LoadImageTexture(app.Device(), path, img, &err)) img = LoadedImage{};
    g_full[path] = img;
    g_fullOrder.push_back(path);
    if (g_fullOrder.size() > 24) {
        auto old = g_full.find(g_fullOrder.front());
        if (old != g_full.end()) {
            if (old->second.srv) ReleaseAfterFrame(old->second.srv);
            g_full.erase(old);
        }
        g_fullOrder.erase(g_fullOrder.begin());
    }
    return img.srv ? &g_full[path] : nullptr;
}

// A picture (or a clip's first frame) fitted into w x h, blurred when NSFW blur says so; full: at full size. True when clicked.
bool Thumb(AppState& app, const std::wstring& path, float w, float h, const char* id, bool full = false) {
    ID3D11ShaderResourceView* srv = nullptr;
    int iw0 = 0, ih0 = 0;
    if (full) {
        if (const LoadedImage* img = Full(app, path)) srv = img->srv, iw0 = img->width, ih0 = img->height;
    }
    if (!srv) {
        const AppState::LibThumb lt = app.LibraryThumb(path);
        srv = lt.srv, iw0 = lt.w, ih0 = lt.h;
    }
    ImGui::PushID(id);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##t", ImVec2(w, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(255, 255, 255, 10), ui::Dp(4.0f));
    if (srv && iw0 > 0 && ih0 > 0) {
        const float s = std::min(w / iw0, h / ih0);
        const float iw = iw0 * s, ih = ih0 * s;
        const ImVec2 a(p.x + (w - iw) * 0.5f, p.y + (h - ih) * 0.5f);
        const bool blur = app.ShouldBlur(path);
        if (ID3D11ShaderResourceView* shown = app.ShownSrv(srv, blur)) dl->AddImage((ImTextureID)(intptr_t)shown, a, ImVec2(a.x + iw, a.y + ih));
        if (blur) DrawNsfwCover(app, path, a, ImVec2(a.x + iw, a.y + ih));
    }
    ImGui::PopID();
    return clicked;
}

// ---- test mixes (2026-10-23: a picture made again with the same model and seed, with 2-3 mixes of LoRAs at different strengths): the picture's own ComfyUI workflow (inside the file) sent again with its seed, each time with one mix of LoRAs instead of its own
struct Mix {
    std::vector<LoraEntry> loras;
    int activity = 0;
    std::wstring result;
    std::string note; // failed / cancelled
};
struct MixTest {
    std::wstring picture;
    json graph;         // its workflow (API format)
    std::string model;  // whose LoRAs fit ("Krea 2"...; "" = not known: every LoRA offered)
    std::string prompt; // its prompt (the mixes' trigger words go into it)
    long long seed = -1;
    std::string note;   // why it cannot be made again
    std::vector<LoraEntry> own; // the LoRAs it was made with
    std::vector<Mix> mixes;
    int big = -1;       // shown big: 0 the picture, 1.. a mix
    int page = 0;       // the LoRAs page: 0 what each one does, 1 test mixes
} g_mix;

std::string ModelOfGraph(const json& g) {
    for (auto it = g.begin(); it != g.end(); ++it) {
        if (!it.value().is_object() || !it.value().contains("inputs") || !it.value()["inputs"].is_object()) continue;
        const json& in = it.value()["inputs"];
        for (const char* k : {"unet_name", "ckpt_name", "model_name"}) {
            if (!in.contains(k) || !in[k].is_string()) continue;
            const std::string v = Lower(in[k].get<std::string>());
            if (v.find("krea") != std::string::npos) return "Krea 2";
            if (v.find("moss") != std::string::npos) return "MOSS-Image";
            if (v.find("qwen") != std::string::npos) return "Qwen Image 2.1";
            if (v.find("minimax") != std::string::npos || v.find("h3") != std::string::npos) return "MiniMax H3";
        }
    }
    return {};
}

std::string MixText(const std::vector<LoraEntry>& loras) {
    std::string t;
    for (const LoraEntry& l : loras) {
        if (!l.on || l.name.empty()) continue;
        char s[16];
        snprintf(s, sizeof(s), " %.2g", l.strength);
        t += (t.empty() ? "" : " + ") + ShortName(l.name) + s;
    }
    return t.empty() ? std::string("no LoRA") : t;
}

void SetMixPicture(AppState& app, const std::wstring& path) {
    g_mix.picture = path;
    g_mix.graph = json();
    g_mix.note.clear(), g_mix.prompt.clear(), g_mix.own.clear();
    g_mix.seed = -1, g_mix.big = -1;
    for (Mix& m : g_mix.mixes) m.activity = 0, m.result.clear(), m.note.clear();
    const ImageMetadata meta = ReadImageMetadata(path);
    for (const RawField& f : meta.rawFields) {
        if (f.key != "prompt") continue;
        json g = json::parse(f.value, nullptr, false);
        bool nodes = g.is_object() && !g.empty();
        for (auto it = g.begin(); nodes && it != g.end(); ++it) nodes = it.value().is_object() && it.value().contains("class_type");
        if (nodes) {
            g_mix.graph = std::move(g);
            break;
        }
    }
    if (!g_mix.graph.is_object()) {
        g_mix.note = "No ComfyUI workflow inside this file: only pictures made with ComfyUI (Generate, Characters, LoRA comparisons...) can be made again.";
        return;
    }
    g_mix.prompt = meta.parsed.positivePrompt;
    for (auto it = g_mix.graph.begin(); it != g_mix.graph.end(); ++it) {
        const json& in = it.value()["inputs"];
        if (!in.is_object()) continue;
        for (const char* k : {"seed", "noise_seed"})
            if (g_mix.seed < 0 && in.contains(k) && in[k].is_number_integer()) g_mix.seed = in[k].get<long long>();
        const std::string cls = it.value().value("class_type", std::string());
        if (cls.find("Lora") != std::string::npos && in.contains("lora_name") && in["lora_name"].is_string())
            g_mix.own.push_back({in["lora_name"].get<std::string>(), in.contains("strength_model") && in["strength_model"].is_number() ? in["strength_model"].get<double>() : 1.0, true});
    }
    const std::string model = ModelOfGraph(g_mix.graph);
    if (model != g_mix.model) g_mix.mixes.clear(); // (another model's LoRAs do not fit)
    g_mix.model = model;
    if (g_mix.mixes.empty()) { // to start: what it was made with, and the model's LoRA list (or nothing)
        Mix a, b;
        a.loras = g_mix.own;
        if (const std::vector<LoraEntry>* list = ListFor(app, model)) b.loras = *list;
        g_mix.mixes = {a, b};
    }
}

void MakeMixes(AppState& app) {
    for (size_t i = 0; i < g_mix.mixes.size(); ++i) {
        Mix& m = g_mix.mixes[i];
        m.result.clear(), m.note.clear();
        json wf = g_mix.graph;
        ApplyLoras(wf, m.loras, true); // (only this mix's: the picture's own LoRAs are taken out)
        const std::string prompt = g_mix.prompt.empty() ? std::string() : WithLoraTriggers(app, g_mix.prompt, m.loras); // (their trigger words)
        for (auto it = wf.begin(); it != wf.end(); ++it) {
            if (!it.value().is_object() || !it.value().contains("inputs") || !it.value()["inputs"].is_object()) continue;
            json& in = it.value()["inputs"];
            for (auto v = in.begin(); v != in.end(); ++v)
                if (!prompt.empty() && v.value().is_string() && v.value().get<std::string>() == g_mix.prompt) v.value() = prompt;
            if (in.contains("filename_prefix")) in["filename_prefix"] = "LoraMix/mix"; // (kept apart from the rest)
        }
        ComfyJob job;
        job.apiWorkflow = wf.dump(-1, ' ', false, json::error_handler_t::replace);
        job.seed = g_mix.seed; // (the same seed: only the LoRAs change)
        job.times = 1;
        job.label = "LoRA mix " + std::to_string(i + 1) + ": " + MixText(m.loras);
        m.activity = SendComfyJob(app, std::move(job));
        if (!m.activity) m.note = "Not sent (is ComfyUI set up? Generate > ComfyUI)";
    }
}

bool MixMaking(const Mix& m) { return m.activity && m.result.empty() && m.note.empty(); }

void PumpMixes(AppState& app) {
    if (!app.comfySvc || std::none_of(g_mix.mixes.begin(), g_mix.mixes.end(), MixMaking)) return;
    const std::vector<ComfyActivity> acts = app.comfySvc->Activity();
    for (Mix& m : g_mix.mixes) {
        if (!MixMaking(m)) continue;
        for (const ComfyActivity& a : acts) {
            if (a.id != m.activity) continue;
            if (a.state == ComfyState::Done) {
                for (const ComfyResult& r : a.files)
                    if (!r.localPath.empty() && !r.audio) m.result = r.localPath;
                if (m.result.empty()) m.note = "Made, but no picture came back";
            } else if (a.state == ComfyState::Failed) {
                m.note = a.message.empty() ? std::string("Failed") : a.message;
            }
        }
    }
}

std::wstring PickPictureFile() {
    wchar_t buf[MAX_PATH * 2] = {};
    OPENFILENAMEW ofn = {sizeof(ofn)};
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = L"Pictures and videos made with ComfyUI\0*.png;*.webp;*.jpg;*.jpeg;*.mp4;*.webm\0All files\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.lpstrTitle = L"A picture made with ComfyUI";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&ofn) ? std::wstring(buf) : std::wstring();
}

void DrawMixes(AppState& app) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("One picture made again - its own ComfyUI workflow and seed - with 2 or 3 mixes of LoRAs at the strengths you set, side by side: only the "
                        "LoRAs change. The mixes' trigger words go into its prompt.");
    ImGui::PopTextWrapPos();
    if (ImGui::Button("Newest made")) {
        std::wstring newest;
        if (app.comfySvc)
            for (const ComfyResult& r : app.comfySvc->Results())
                if (!r.localPath.empty() && !r.audio && !r.video) newest = r.localPath;
        if (!newest.empty()) SetMixPicture(app, newest);
        else app.SetStatus("Nothing made yet this session: pick the picture in the gallery, or a file");
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("The newest picture ComfyUI made this session");
    ImGui::SameLine();
    const bool sel = app.selectedIndex >= 0 && app.selectedIndex < static_cast<int>(app.entries.size());
    ImGui::BeginDisabled(!sel);
    if (ImGui::Button("Gallery's picture")) SetMixPicture(app, app.entries[app.selectedIndex].path);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(sel ? "The picture selected in the gallery" : "Select a picture in the gallery first (or right-click it there > Test LoRA mixes on it)");
    ImGui::SameLine();
    if (ImGui::Button("File...")) {
        const std::wstring f = PickPictureFile();
        if (!f.empty()) SetMixPicture(app, f);
    }
    if (g_mix.picture.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("Pick a picture: one of these, right-click a picture in the gallery or Generate's results > Test LoRA mixes on it, or a LoRA comparison's picture.");
        return;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", U8(fs::path(g_mix.picture).filename().wstring()).c_str());
    if (!g_mix.note.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(ui::kWarn, "%s", g_mix.note.c_str());
        ImGui::PopTextWrapPos();
        return;
    }
    ImGui::TextDisabled("Made with %s, seed %lld; its LoRAs: %s", g_mix.model.empty() ? "a model not known (every LoRA is offered)" : g_mix.model.c_str(), g_mix.seed,
                        MixText(g_mix.own).c_str());
    // the mixes
    const char* model = g_mix.model.empty() ? nullptr : g_mix.model.c_str();
    int remove = -1;
    for (size_t i = 0; i < g_mix.mixes.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        ui::SectionHeading(("MIX " + std::to_string(i + 1)).c_str());
        DrawModelLoras(app, g_mix.mixes[i].loras, model, "mixloras", "LoRAs:");
        if (std::vector<LoraEntry>* list = ListFor(app, g_mix.model)) {
            if (ImGui::SmallButton(("Use the " + g_mix.model + " LoRAs").c_str())) g_mix.mixes[i].loras = *list;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("This mix becomes the list Generate uses now");
            ImGui::SameLine();
            if (ImGui::SmallButton("Put this mix in that list")) *list = g_mix.mixes[i].loras, SaveComfySettings(app.comfy);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Generate (and Characters) use this mix from now on");
            ImGui::SameLine();
        }
        if (ImGui::SmallButton("Its own LoRAs")) g_mix.mixes[i].loras = g_mix.own;
        if (g_mix.mixes.size() > 2) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove this mix")) remove = static_cast<int>(i);
        }
        ImGui::PopID();
    }
    if (remove >= 0) g_mix.mixes.erase(g_mix.mixes.begin() + remove);
    if (g_mix.mixes.size() < 3 && ImGui::SmallButton("+ Add a mix")) g_mix.mixes.push_back(Mix{});
    ImGui::Spacing();
    const bool making = std::any_of(g_mix.mixes.begin(), g_mix.mixes.end(), MixMaking);
    ImGui::BeginDisabled(making);
    char go[96];
    snprintf(go, sizeof(go), "Make the %d mixes (same picture, same seed)", static_cast<int>(g_mix.mixes.size()));
    if (ui::PrimaryButton(making ? "Making them..." : go)) MakeMixes(app), g_mix.big = -1;
    ImGui::EndDisabled();
    if (making) {
        ImGui::SameLine();
        if (ImGui::Button("Cancel##mix")) {
            for (Mix& m : g_mix.mixes)
                if (MixMaking(m)) {
                    if (app.comfySvc) app.comfySvc->Cancel(m.activity);
                    m.note = "Cancelled";
                }
        }
    }
    // the picture and each mix, side by side
    const size_t n = g_mix.mixes.size() + 1;
    const float gap = ui::Dp(8.0f);
    const float w = std::min(ui::Dp(320.0f), (ImGui::GetContentRegionAvail().x - gap * (n - 1)) / n), h = w * 1.25f;
    for (size_t i = 0; i < n; ++i) {
        if (i) ImGui::SameLine(0, gap);
        ImGui::BeginGroup();
        ImGui::PushID(static_cast<int>(i) + 100);
        const Mix* m = i ? &g_mix.mixes[i - 1] : nullptr;
        const std::wstring path = m ? m->result : g_mix.picture;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (!path.empty()) {
            if (Thumb(app, path, w, h, "mx", true)) g_mix.big = g_mix.big == static_cast<int>(i) ? -1 : static_cast<int>(i);
            if (ImGui::BeginPopupContextItem("##mixshot")) {
                if (ImGui::MenuItem("Open in the gallery")) app.OpenFile(path), app.requestTab = 0;
                NsfwMarkMenu(app, path);
                ImGui::EndPopup();
            }
        } else {
            ImGui::InvisibleButton("##empty", ImVec2(w, h));
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(255, 255, 255, 12), ui::Dp(4.0f));
            const char* t = m && MixMaking(*m) ? "making..." : m && !m->note.empty() ? m->note.c_str() : "not made yet";
            dl->PushClipRect(p, ImVec2(p.x + w, p.y + h), true);
            dl->AddText(nullptr, 0.0f, ImVec2(p.x + ui::Dp(8.0f), p.y + h * 0.45f), ImGui::GetColorU32(m && MixMaking(*m) ? ui::kAccent : ui::kTextDim), t, nullptr,
                        w - ui::Dp(16.0f));
            dl->PopClipRect();
        }
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
        if (m) ImGui::TextDisabled("Mix %d: %s", static_cast<int>(i), MixText(m->loras).c_str());
        else ImGui::TextDisabled("The picture: %s", MixText(g_mix.own).c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopID();
        ImGui::EndGroup();
    }
    if (g_mix.big >= 0 && g_mix.big < static_cast<int>(n)) {
        const std::wstring path = g_mix.big ? g_mix.mixes[g_mix.big - 1].result : g_mix.picture;
        const float bw = std::min(ImGui::GetContentRegionAvail().x, ui::Dp(900.0f));
        if (!path.empty() && Thumb(app, path, bw, bw * 1.25f, "mixbig", true)) g_mix.big = -1;
    }
}

void DrawDetails(AppState& app, const LoraOffer& o) {
    const Info& in = InfoOf(o.name);
    ImGui::PushFont(ui::TitleFont());
    ImGui::TextWrapped("%s", ShortName(o.name).c_str());
    ImGui::PopFont();
    ImGui::TextDisabled("%s  -  %s", o.model.empty() ? "model not known" : ("for " + o.model).c_str(), o.name.c_str());
    const auto trig = TriggersOf(o.name);
    if (!trig.empty()) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Trigger words:");
        for (size_t i = 0; i < trig.size(); ++i) {
            ImGui::SameLine();
            if (ImGui::SmallButton((trig[i].first + "##trig" + std::to_string(i)).c_str())) ImGui::SetClipboardText(trig[i].first.c_str());
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("From %s. Click: copy", trig[i].second.c_str());
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_civitMutex);
        auto c = g_civit.find(CivitKey(o.name));
        if (c != g_civit.end()) {
            if (c->second.found)
                ImGui::TextDisabled("On Civitai: \"%s\" (%s%s)%s", c->second.name.c_str(), c->second.type.c_str(), c->second.base.empty() ? "" : (", " + c->second.base).c_str(),
                                    c->second.examples.empty() ? "" : (", " + std::to_string(c->second.examples.size()) + " example prompts").c_str());
            else ImGui::TextDisabled("Not found on Civitai (by its fingerprint).");
        }
    }
    if (!in.tags.empty()) {
        ImGui::TextDisabled("Its training captions said most:");
        for (size_t i = 0; i < in.tags.size(); ++i) {
            if (i) ui::SameLineIfFits(in.tags[i].c_str());
            if (ImGui::SmallButton((in.tags[i] + "##tag" + std::to_string(i)).c_str())) g_b.prompt = in.tags[i] + ", " + g_b.prompt;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Put it in the test prompt");
        }
    }
    for (const auto& [label, text] : in.lines) {
        ImGui::TextDisabled("%s:", label.c_str());
        ImGui::SameLine();
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopTextWrapPos();
    }
    if (in.lines.empty() && in.triggers.empty() && in.tags.empty()) ImGui::TextDisabled("Its file says nothing about itself: the pictures below show what it does.");

    ui::SectionHeading("WHAT IT DOES");
    if (!CanShow(o.model)) {
        ImGui::TextWrapped("This app has no %s maker, so it cannot show this LoRA.", o.model.empty() ? "matching" : o.model.c_str());
        return;
    }
    const bool video = IsVideoModel(o.model);
    // the prompt: one written to show this LoRA off, or the one below
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Prompt:");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, g_b.mode == 0 ? ui::ChipActiveColor() : ui::ChipColor());
    if (ImGui::SmallButton("Showcase (written for it)")) g_b.mode = 0;
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("Before the comparison the prompt writer writes a prompt that shows what this LoRA is for, with its trigger words\n"
                          "(from its file, its title, its training captions and Civitai) and the creator's example prompts. A LoRA often does\n"
                          "little or nothing without its trigger words.");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, g_b.mode == 1 ? ui::ChipActiveColor() : ui::ChipColor());
    if (ImGui::SmallButton("Standard (the same for all)")) g_b.mode = 1;
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("One test prompt for every LoRA of this model, with each one's trigger words in front: their comparisons can be set side by side");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, g_b.mode == 2 ? ui::ChipActiveColor() : ui::ChipColor());
    if (ImGui::SmallButton("My prompt")) g_b.mode = 2;
    ImGui::PopStyleColor();
    if (g_b.mode == 1) {
        ImGui::Checkbox("Look its trigger words up on Civitai##std", &g_b.civitai);
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", PromptFor(o.model, o.name).c_str());
        ImGui::PopTextWrapPos();
    } else if (g_b.mode == 0) {
        ImGui::Checkbox("Look its trigger words up on Civitai", &g_b.civitai);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("By the file's SHA-256 fingerprint (nothing else is sent); kept for next time");
        auto s = g_showcase.find(Key(o.name));
        if (s != g_showcase.end()) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("Last written: %s", s->second.c_str());
            ImGui::PopTextWrapPos();
        }
    } else {
        const Preset* presets = video ? kVideoPresets : kPicturePresets;
        const int presetCount = video ? 3 : 4;
        for (int i = 0; i < presetCount; ++i) {
            if (i) ImGui::SameLine();
            if (ImGui::SmallButton(presets[i].name)) g_b.prompt = presets[i].text;
        }
        if (g_b.prompt.empty()) g_b.prompt = presets[0].text;
        PromptBox("##lbprompt", g_b.prompt, ImVec2(ImGui::GetContentRegionAvail().x, ui::Dp(64.0f)));
        if (!in.triggers.empty()) ImGui::Checkbox("Start it with the trigger words", &g_b.addTriggers);
    }
    if (!video) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Strengths:");
        for (int i = 0; i < 5; ++i) {
            ImGui::SameLine();
            char l[16];
            snprintf(l, sizeof(l), "%.2g##st%d", kStrengths[i], i);
            ImGui::Checkbox(l, &g_b.strengths[i]);
        }
    }
    const std::vector<double> st = ChosenStrengths(o.model);
    const bool preparing = Preparing(o.name);
    const bool waiting = Waiting(o.name) || preparing;
    ImGui::BeginDisabled(waiting);
    char go[96];
    const char* verb = g_cmp.count(Key(o.name)) ? "Redo" : "Make";
    if (video) snprintf(go, sizeof(go), "%s the comparison (2 short clips, about %d s)", verb, static_cast<int>(SecondsEach(o.model) * 2));
    else snprintf(go, sizeof(go), "%s the comparison (%d pictures, about %d s)", verb, static_cast<int>(st.size()) + 1, static_cast<int>(SecondsEach(o.model) * (st.size() + 1)));
    if (ui::PrimaryButton(preparing ? (g_b.mode == 0 ? "Writing its showcase prompt..." : "Finding its trigger words...") : waiting ? "Making it..." : go)) Ask(app, o.name, o.model, st, g_b.mode), g_b.big = -1;
    ImGui::EndDisabled();
    if (waiting) {
        ImGui::SameLine();
        if (ImGui::Button("Cancel##one")) CancelComparisons(app, o.name);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Stops this LoRA's comparison: its prompt is not written, its pictures not made");
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("The same prompt and seed without it and %s: only the LoRA changes, so what changes in the pictures is what it does.",
                          video ? "with it (480p, 3 s)" : "at each strength ticked");

    // the strip: without | each strength
    auto it = g_cmp.find(Key(o.name));
    if (it == g_cmp.end() || it->second.shots.empty()) {
        if (!waiting) ImGui::TextDisabled("No comparison yet.");
    } else {
        const Comparison& c = it->second;
        std::vector<Shot> strip;
        auto base = g_base.find(o.model + "|" + c.prompt);
        if (base != g_base.end()) strip.push_back({0, base->second, video});
        for (const Shot& s : c.shots) strip.push_back(s);
        const float gap = ui::Dp(6.0f);
        const float w = (ImGui::GetContentRegionAvail().x - gap * (strip.size() - 1)) / std::max<size_t>(1, strip.size());
        const float h = w * (video ? 9.0f / 16.0f : 1.25f);
        for (size_t i = 0; i < strip.size(); ++i) {
            if (i) ImGui::SameLine(0, gap);
            ImGui::BeginGroup();
            if (Thumb(app, strip[i].path, w, h, ("s" + std::to_string(i)).c_str(), true)) {
                if (strip[i].video) PlayInVideoPage(app, strip[i].path);
                else g_b.big = g_b.big == static_cast<int>(i) ? -1 : static_cast<int>(i);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("%s\nRight-click: use it as the LoRA's showcase picture, test mixes on it, mark it NSFW", strip[i].video ? "Play it (Video page)" : "Show it big");
            const bool showcase = strip[i].strength != 0 && CardPicture(o.name) == strip[i].path;
            if (ImGui::BeginPopupContextItem("##shot")) {
                if (strip[i].strength != 0 && ImGui::MenuItem("Use as its showcase picture", nullptr, showcase)) {
                    g_pick[Key(o.name)] = strip[i].strength;
                    Save();
                }
                if (strip[i].strength != 0 && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                    ImGui::SetTooltip("Its card here and in the LoRA lists shows this strength's picture");
                if (ImGui::MenuItem("Test LoRA mixes on this picture")) SetMixPicture(app, strip[i].path), g_mix.page = 1;
                NsfwMarkMenu(app, strip[i].path);
                ImGui::EndPopup();
            }
            char l[48];
            if (strip[i].strength == 0) snprintf(l, sizeof(l), "Without");
            else snprintf(l, sizeof(l), "%.2g%s", strip[i].strength, showcase ? "  (showcase)" : "");
            ImGui::TextDisabled("%s", l);
            ImGui::EndGroup();
        }
        if (g_b.big >= 0 && g_b.big < static_cast<int>(strip.size())) {
            const float bw = ImGui::GetContentRegionAvail().x;
            if (Thumb(app, strip[g_b.big].path, bw, bw * 1.25f, "big", true)) g_b.big = -1;
        }
        ImGui::TextDisabled("Prompt: %s", c.prompt.c_str());
    }
    if (preparing) ImGui::TextColored(ui::kAccent, "%s", g_b.mode == 0 ? "Finding its trigger words and writing a prompt that shows it off..." : "Finding its trigger words...");
    else if (waiting) ImGui::TextColored(ui::kAccent, "Making it in ComfyUI...");

    // use it
    if (std::vector<LoraEntry>* list = ListFor(app, o.model)) {
        ui::SectionHeading("USE IT");
        static double use = 1.0;
        StrengthSlider("##use", use, ui::Dp(170.0f));
        ImGui::SameLine();
        const bool there = std::any_of(list->begin(), list->end(), [&](const LoraEntry& l) { return LoraFileName(l.name) == LoraFileName(o.name); });
        char add[96];
        snprintf(add, sizeof(add), there ? "Set it in the %s LoRAs" : "Add it to the %s LoRAs", o.model.c_str());
        if (ImGui::Button(add)) {
            bool found = false;
            for (LoraEntry& l : *list) {
                if (LoraFileName(l.name) == LoraFileName(o.name)) l.strength = use, l.on = true, found = true;
            }
            if (!found) list->push_back({o.name, use, true});
            SaveComfySettings(app.comfy);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Used by Generate and Create a character (videos: the Video page) from now on");
    }
}

} // namespace

std::string LoraTrigger(const std::string& lora) {
    LoadCivit();
    PumpLookups();
    // (asked every frame by the LoRA lists: kept for 2 s, as finding the file and its size touches the disk)
    static std::map<std::string, std::pair<unsigned long long, std::string>> known;
    const unsigned long long now = GetTickCount64();
    auto k = known.find(lora);
    if (k != known.end() && now - k->second.first < 2000) return k->second.second;
    WantCivit(lora, g_b.civitai);
    std::string word;
    const auto trig = TriggersOf(lora); // (its file, its title, Civitai)
    for (const auto& t : trig)
        if (word.empty() && t.second == std::string("its file")) word = t.first;
    for (const auto& t : trig)
        if (word.empty() && t.second == std::string("Civitai")) word = t.first;
    for (const auto& t : trig)
        if (word.empty() && Lower(t.first).find("lora") == std::string::npos) word = t.first; // (a title that reads like one, not "... LoRA v2")
    known[lora] = {now, word};
    return word;
}

std::string WithLoraTriggers(const AppState& app, const std::string& prompt, const std::vector<LoraEntry>& loras) {
    if (!app.comfy.loraTriggers) return prompt;
    std::vector<std::pair<std::string, std::vector<std::string>>> want; // each LoRA's word to add, and all of its words (lower case)
    for (const LoraEntry& l : loras) {
        if (!l.on || l.name.empty() || l.strength <= 0) continue;
        const std::string t = LoraTrigger(l.name);
        if (t.empty()) continue;
        std::vector<std::string> all{Lower(t)};
        for (const auto& w : TriggersOf(l.name)) all.push_back(Lower(w.first));
        want.push_back({t, all});
    }
    if (want.empty()) return prompt;
    // into every part of a long video (parts split by "---" lines); a first line like "body_swap:" stays first; a timeline or a
    // "<Picture 1> ..." prompt gets them as a line of their own, anything else in front of its first words
    std::vector<std::string> lines;
    size_t at = 0;
    while (true) {
        const size_t nl = prompt.find('\n', at);
        lines.push_back(prompt.substr(at, nl == std::string::npos ? std::string::npos : nl - at));
        if (nl == std::string::npos) break;
        at = nl + 1;
    }
    auto trimmed = [](const std::string& s) {
        const size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    };
    auto dashes = [&](const std::string& s) { const std::string t = trimmed(s); return t.size() >= 3 && t.find_first_not_of('-') == std::string::npos; };
    // the words a part is missing (typed or written in already: not again)
    auto missing = [&](size_t from) {
        std::string part;
        for (size_t k = from; k < lines.size() && !dashes(lines[k]); ++k) part += Lower(lines[k]) + "\n";
        std::string words;
        for (const auto& [t, all] : want) {
            bool there = Lower(words).find(Lower(t)) != std::string::npos;
            for (const std::string& w : all) there = there || part.find(w) != std::string::npos;
            if (!there) words += (words.empty() ? "" : ", ") + t;
        }
        return words;
    };
    std::vector<std::string> out;
    bool start = true; // (at the start of a part)
    std::string words = missing(0);
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string t = trimmed(lines[i]);
        if (dashes(lines[i])) {
            start = true;
            words = missing(i + 1);
        } else if (start && !t.empty() && words.empty()) {
            start = false;
        } else if (start && !t.empty()) {
            const bool tag = t.back() == ':' && t.find(' ') == std::string::npos; // "body_swap:"
            if (!tag) {
                start = false;
                if (t.front() == '[' || t.front() == '<') {
                    out.push_back(words + ".");
                } else {
                    out.push_back(words + ", " + lines[i]);
                    continue;
                }
            }
        }
        out.push_back(lines[i]);
    }
    std::string r;
    for (size_t i = 0; i < out.size(); ++i) r += (i ? "\n" : "") + out[i];
    return r;
}

void OpenLoraBrowser(const char* model) {
    g_b.open = true;
    if (model) g_b.model = model;
    g_b.confirmAll = 0;
    RefreshLoraOffers();
}

void DrawLoraBrowser(AppState& app) {
    Load();
    LoadCivit();
    PumpPending(app);
    PumpPrep(app);
    { // tests: MV_TRIGGER_TEST="prompt" writes it with the Krea 2 and H3 lists' trigger words to lora_previews\trigger_test.txt (once they are known)
        static int left = 600;
        char buf[512] = {};
        if (left > 0 && GetEnvironmentVariableA("MV_TRIGGER_TEST", buf, sizeof(buf)) && --left % 100 == 0) {
            std::string p = buf;
            for (size_t at; (at = p.find("\\n")) != std::string::npos;) p.replace(at, 2, "\n");
            std::ofstream(Dir() + L"\\trigger_test.txt", std::ios::binary | std::ios::trunc)
                << "KREA:\n" << WithLoraTriggers(app, p, app.comfy.kreaLoras) << "\n\nH3:\n" << WithLoraTriggers(app, p, app.comfy.h3Loras) << "\n";
        }
    }
    static bool testOpen = [] { // tests: MV_UI_TEST "lorabrowse" opens it; MV_LORA_COMPARE=name makes that one's comparison once ComfyUI lists it
        char buf[256] = {};
        return GetEnvironmentVariableA("MV_UI_TEST", buf, sizeof(buf)) && strstr(buf, "lorabrowse") != nullptr;
    }();
    if (testOpen) testOpen = false, OpenLoraBrowser();
    PumpMixes(app);
    if (g_b.open) g_b.open = false, app.requestTab = 13; // (the LoRAs page: it was a window)
    {
        static bool testDone = false;
        char want[256] = {};
        char mode[8] = {}; // (MV_LORA_MODE: 0 showcase, 1 standard, 2 my prompt)
        if (!testDone && GetEnvironmentVariableA("MV_LORA_MODE", mode, sizeof(mode))) g_b.mode = std::clamp(atoi(mode), 0, 2);
        if (!testDone && GetEnvironmentVariableA("MV_LORA_COMPARE", want, sizeof(want))) {
            for (const LoraOffer& o : LoraOffers(app)) {
                if (LoraFileName(o.name) == LoraFileName(want)) {
                    testDone = true;
                    g_b.selected = o.name, g_b.model = o.model;
                    auto have = g_cmp.find(Key(o.name)); // (one already made: only shown)
                    if (have == g_cmp.end() || have->second.shots.empty()) Ask(app, o.name, o.model, ChosenStrengths(o.model), g_b.mode);
                }
            }
        }
    }
    { // tests: MV_MIX_TEST=picture puts it on Test mixes once (the LoRAs page); MV_MIX_MAKE also makes the mixes
        static bool once = false;
        wchar_t pic[MAX_PATH] = {};
        if (!once && GetEnvironmentVariableW(L"MV_MIX_TEST", pic, MAX_PATH)) {
            once = true;
            TestLoraMixesOn(app, pic);
            if (GetEnvironmentVariableW(L"MV_MIX_MAKE", nullptr, 0) > 0) MakeMixes(app);
        }
    }
}

// What each LoRA does: the cards, and the one picked
static void DrawBrowse(AppState& app) {
    const std::vector<LoraOffer>& offers = LoraOffers(app);
    // which model's
    const char* models[] = {"Krea 2", "Qwen Image 2.1", "MOSS-Image", "MiniMax H3", "", "Other"};
    const char* labels[] = {"Krea 2", "Qwen 2.1", "MOSS", "MiniMax H3 (video)", "All", "Other"};
    for (int i = 0; i < 6; ++i) {
        if (i) ImGui::SameLine();
        const bool on = g_b.model == models[i];
        ImGui::PushStyleColor(ImGuiCol_Button, on ? ui::ChipActiveColor() : ui::ChipColor());
        if (ImGui::SmallButton(labels[i])) g_b.model = models[i], g_b.confirmAll = 0;
        ImGui::PopStyleColor();
    }
    ImGui::SameLine(0, ui::Dp(16.0f));
    ImGui::SetNextItemWidth(ui::Dp(180.0f));
    ImGui::InputTextWithHint("##search", "Search", g_b.search, sizeof(g_b.search));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ui::Dp(120.0f));
    ImGui::SliderFloat("##card", &g_b.card, 90.0f, 260.0f, "cards %.0f");
    std::vector<const LoraOffer*> shown;
    const std::string f = Lower(g_b.search);
    for (const LoraOffer& o : offers) {
        const bool other = !CanShow(o.model);
        if (g_b.model == "Other" ? !other : (!g_b.model.empty() && o.model != g_b.model)) continue;
        if (!f.empty() && Lower(o.name).find(f) == std::string::npos) continue;
        shown.push_back(&o);
    }
    // previews for every one without one
    int missing = 0;
    for (const LoraOffer* o : shown) missing += CanShow(o->model) && !g_cmp.count(Key(o->name)) && !Waiting(o->name) && !Preparing(o->name) ? 1 : 0;
    if (missing && !g_b.model.empty() && g_b.model != "Other") {
        ImGui::SameLine(0, ui::Dp(16.0f));
        const double secs = SecondsEach(g_b.model) * missing * (IsVideoModel(g_b.model) ? 1 : ChosenStrengths(g_b.model).size()) + SecondsEach(g_b.model);
        char l[96];
        snprintf(l, sizeof(l), g_b.confirmAll == 1 ? "Sure? %d LoRAs, about %d min: click again" : "Make comparisons for the %d without one", missing,
                 static_cast<int>(std::ceil(secs / 60.0)));
        if (ImGui::SmallButton(l)) {
            if (g_b.confirmAll != 1) {
                g_b.confirmAll = 1;
            } else {
                g_b.confirmAll = 0;
                for (const LoraOffer* o : shown) {
                    if (CanShow(o->model) && !g_cmp.count(Key(o->name)) && !Waiting(o->name) && !Preparing(o->name)) Ask(app, o->name, o->model, ChosenStrengths(o->model), g_b.mode);
                }
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip(g_b.mode == 0 ? "Each with a showcase prompt written for it (all written first), at the strengths on the right; queued in ComfyUI one after another"
                                           : "With the test prompt and strengths on the right; queued in ComfyUI one after another");
    }
    // made again: the picked ones (Ctrl+click), or every one shown - with a new showcase prompt written (or the standard / my prompt)
    auto redoable = [&](const LoraOffer* o) { return CanShow(o->model) && !Waiting(o->name) && !Preparing(o->name); };
    if (!g_b.picked.empty()) {
        int n = 0;
        for (const LoraOffer& o : offers) n += g_b.picked.count(o.name) && redoable(&o) ? 1 : 0;
        ImGui::SameLine(0, ui::Dp(16.0f));
        char l[64];
        snprintf(l, sizeof(l), "Redo the %d picked", n);
        ImGui::BeginDisabled(n == 0);
        if (ImGui::SmallButton(l)) {
            for (const LoraOffer& o : offers) {
                if (g_b.picked.count(o.name) && redoable(&o)) Ask(app, o.name, o.model, ChosenStrengths(o.model), g_b.mode);
            }
            g_b.picked.clear();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Their comparisons made again (Ctrl+click a LoRA to pick or unpick it)");
        ImGui::SameLine();
        if (ImGui::SmallButton("Unpick")) g_b.picked.clear();
    } else if (!g_b.model.empty() && g_b.model != "Other") {
        int n = 0;
        for (const LoraOffer* o : shown) n += redoable(o) ? 1 : 0;
        if (n) {
            ImGui::SameLine(0, ui::Dp(16.0f));
            const double secs = SecondsEach(g_b.model) * n * (IsVideoModel(g_b.model) ? 1 : ChosenStrengths(g_b.model).size()) + SecondsEach(g_b.model);
            char l[96];
            snprintf(l, sizeof(l), g_b.confirmAll == 2 ? "Sure? Redo %d, about %d min: click again" : "Redo all %d", n, static_cast<int>(std::ceil(secs / 60.0)));
            if (ImGui::SmallButton(l)) {
                if (g_b.confirmAll != 2) {
                    g_b.confirmAll = 2;
                } else {
                    g_b.confirmAll = 0;
                    for (const LoraOffer* o : shown) {
                        if (redoable(o)) Ask(app, o->name, o->model, ChosenStrengths(o->model), g_b.mode);
                    }
                }
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("Every comparison shown made again, with the prompt choice and strengths on the right.\nTo redo only some: Ctrl+click them, or right-click one.");
        }
    }
    const int writing = static_cast<int>(g_prepQueue.size() + g_prepDone.size()) + (g_prepFut.valid() ? 1 : 0);
    if (!g_pending.empty() || writing) {
        ImGui::SameLine();
        if (writing) ImGui::TextColored(ui::kAccent, "%d prompt%s to write, %d being made", writing, writing == 1 ? "" : "s", static_cast<int>(g_pending.size()));
        else ImGui::TextColored(ui::kAccent, "%d being made", static_cast<int>(g_pending.size()));
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel all")) CancelComparisons(app, std::string());
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Stops every comparison still being written or made (those done stay)");
    }
    ImGui::Separator();
    if (offers.empty()) {
        ImGui::TextDisabled("Asking ComfyUI for its LoRAs... (it has to be running: Generate > ComfyUI)");
        return;
    }
    if (!ImGui::BeginTable("##lb", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV, ImGui::GetContentRegionAvail())) return;
    ImGui::TableSetupColumn("cards", ImGuiTableColumnFlags_WidthStretch, 0.55f);
    ImGui::TableSetupColumn("details", ImGuiTableColumnFlags_WidthStretch, 0.45f);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::BeginChild("##cards");
    const float cw = ui::Dp(g_b.card), ch = cw * 1.25f, gap = ui::Dp(8.0f);
    const int cols = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + gap) / (cw + gap)));
    for (size_t i = 0; i < shown.size(); ++i) {
        const LoraOffer& o = *shown[i];
        if (i % cols) ImGui::SameLine(0, gap);
        ImGui::BeginGroup();
        ImGui::PushID(static_cast<int>(i));
        // the card's picture: its comparison at the strongest strength made, else a picture it carries
        const std::wstring pic = CardPicture(o.name);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        bool click = false;
        if (!pic.empty()) {
            click = Thumb(app, pic, cw, ch, "c");
        } else {
            click = ImGui::InvisibleButton("##c", ImVec2(cw, ch));
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, ImVec2(p.x + cw, p.y + ch), IM_COL32(255, 255, 255, 12), ui::Dp(4.0f));
            const char* m = o.model.empty() ? "?" : o.model.c_str();
            const ImVec2 ts = ImGui::CalcTextSize(m);
            dl->AddText(ImVec2(p.x + (cw - ts.x) * 0.5f, p.y + (ch - ts.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled), m);
        }
        if (click && ImGui::GetIO().KeyCtrl) { // picked (for Redo the picked)
            if (!g_b.picked.erase(o.name)) g_b.picked.insert(o.name);
        } else if (click) {
            g_b.selected = o.name, g_b.big = -1;
        }
        if (ImGui::BeginPopupContextItem("##card")) { // (right-click on the card's picture)
            const bool busy = Waiting(o.name) || Preparing(o.name);
            if (ImGui::MenuItem(g_cmp.count(Key(o.name)) ? "Redo its comparison" : "Make its comparison", nullptr, false, !busy && CanShow(o.model)))
                g_b.selected = o.name, g_b.big = -1, Ask(app, o.name, o.model, ChosenStrengths(o.model), g_b.mode);
            if (ImGui::MenuItem(g_b.picked.count(o.name) ? "Unpick" : "Pick (to redo several)")) {
                if (!g_b.picked.erase(o.name)) g_b.picked.insert(o.name);
            }
            if (busy && ImGui::MenuItem("Cancel its comparison")) CancelComparisons(app, o.name);
            if (!pic.empty()) NsfwMarkMenu(app, pic);
            ImGui::EndPopup();
        }
        if (g_b.selected == o.name)
            ImGui::GetWindowDrawList()->AddRect(p, ImVec2(p.x + cw, p.y + ch), ImGui::GetColorU32(ui::kAccent), ui::Dp(4.0f), 0, ui::Dp(2.0f));
        if (g_b.picked.count(o.name)) { // a tick in the corner
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float r = ui::Dp(10.0f);
            const ImVec2 c(p.x + cw - r - ui::Dp(4.0f), p.y + r + ui::Dp(4.0f));
            dl->AddCircleFilled(c, r, ImGui::GetColorU32(ui::kAccent));
            dl->AddPolyline(std::vector<ImVec2>{ImVec2(c.x - r * 0.45f, c.y), ImVec2(c.x - r * 0.1f, c.y + r * 0.4f), ImVec2(c.x + r * 0.5f, c.y - r * 0.4f)}.data(), 3,
                            IM_COL32(255, 255, 255, 255), 0, ui::Dp(2.0f));
            dl->AddRect(p, ImVec2(p.x + cw, p.y + ch), ImGui::GetColorU32(ui::kAccent), ui::Dp(4.0f), 0, ui::Dp(1.0f));
        }
        if (Waiting(o.name) || Preparing(o.name))
            ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + ui::Dp(6.0f), p.y + ui::Dp(4.0f)), ImGui::GetColorU32(ui::kAccent), Preparing(o.name) ? "writing..." : "making...");
        // the name, cut to the card
        const std::string n = ShortName(o.name);
        std::string fit = n;
        while (fit.size() > 3 && ImGui::CalcTextSize(fit.c_str()).x > cw) fit.pop_back();
        if (fit.size() < n.size() && fit.size() > 3) fit = fit.substr(0, fit.size() - 2) + "..";
        ImGui::TextUnformatted(fit.c_str());
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", o.name.c_str());
        ImGui::PopID();
        ImGui::EndGroup();
    }
    if (shown.empty()) ImGui::TextDisabled("No LoRA here.");
    ImGui::EndChild();
    ImGui::TableSetColumnIndex(1);
    ImGui::BeginChild("##details", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    const LoraOffer* sel = nullptr;
    for (const LoraOffer& o : offers) {
        if (o.name == g_b.selected) sel = &o;
    }
    if (sel) DrawDetails(app, *sel);
    else ImGui::TextDisabled("Click a LoRA to see what it does.");
    ImGui::EndChild();
    ImGui::EndTable();
}

void DrawLoraPage(AppState& app, float height) {
    const float y0 = ImGui::GetCursorPosY();
    const char* pages[] = {"What each LoRA does", "Test mixes on a picture"};
    for (int i = 0; i < 2; ++i) {
        if (i) ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, g_mix.page == i ? ui::WithAlpha(ui::kAccent, 0.85f) : ImGui::GetColorU32(ImGuiCol_Button));
        if (ImGui::Button(pages[i])) g_mix.page = i;
        ImGui::PopStyleColor();
    }
    const float h = std::max(ui::Dp(100.0f), height - (ImGui::GetCursorPosY() - y0));
    ImGui::BeginChild("##lorapage", ImVec2(0, h), ImGuiChildFlags_None, g_mix.page == 0 ? ImGuiWindowFlags_NoScrollbar : 0);
    if (g_mix.page == 0) DrawBrowse(app);
    else DrawMixes(app);
    ImGui::EndChild();
}

void TestLoraMixesOn(AppState& app, const std::wstring& picture) {
    Load();
    SetMixPicture(app, picture);
    g_mix.page = 1;
    app.requestTab = 13;
}

void ShowLoraInBrowser(const std::string& lora, const char* model) {
    g_b.selected = lora, g_b.big = -1;
    g_mix.page = 0;
    OpenLoraBrowser(model);
}

bool DrawLoraTile(AppState& app, const std::string& lora, float w, float h) {
    Load();
    const std::wstring pic = CardPicture(lora);
    if (!pic.empty()) return Thumb(app, pic, w, h, ("lt" + lora).c_str());
    // none yet: a box with "none" (small) or its name in it
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool click = ImGui::InvisibleButton(("##lt" + lora).c_str(), ImVec2(w, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(255, 255, 255, ImGui::IsItemHovered() ? 26 : 14), ui::Dp(4.0f));
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), IM_COL32(255, 255, 255, 34), ui::Dp(4.0f));
    dl->PushClipRect(p, ImVec2(p.x + w, p.y + h), true);
    if (h < ui::Dp(70.0f)) {
        const char* t = "none";
        const ImVec2 ts = ImGui::CalcTextSize(t);
        const float s = std::min(1.0f, (w - ui::Dp(4.0f)) / std::max(1.0f, ts.x));
        ImFont* font = ImGui::GetFont();
        const float fs = ImGui::GetFontSize() * s;
        dl->AddText(font, fs, ImVec2(p.x + (w - ts.x * s) * 0.5f, p.y + (h - fs) * 0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled), t);
    } else {
        const std::string n = ShortName(lora);
        const float pad = ui::Dp(7.0f);
        const ImVec2 ts = ImGui::CalcTextSize(n.c_str(), nullptr, false, w - pad * 2);
        dl->AddText(nullptr, 0.0f, ImVec2(p.x + pad, p.y + std::max(pad, (h - ts.y) * 0.4f)), ImGui::GetColorU32(ImGuiCol_Text), n.c_str(), nullptr, w - pad * 2);
        const char* t = "no showcase yet";
        const ImVec2 t2 = ImGui::CalcTextSize(t);
        if (t2.x < w - pad) dl->AddText(ImVec2(p.x + (w - t2.x) * 0.5f, p.y + h - t2.y - pad), ImGui::GetColorU32(ImGuiCol_TextDisabled), t);
    }
    dl->PopClipRect();
    return click;
}
