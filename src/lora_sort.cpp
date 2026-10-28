// LoRAs by model: which model a LoRA file is for (read from the file itself), each model's LoRAs with their strengths on the Generate page
// and in Create a character, and the LoRA sorter (Tools menu), which moves the LoRA files into one folder per model inside ComfyUI's loras
// folder ("Krea 2", "MOSS-Image"...), with a way to put them back.
//
// How the model is known (checked on 26 LoRAs, 2026-10-20): most trainers write it into the file's header ("ss_base_model_version":
// krea2, moss_image, qwen_image_2, minimax_h3), but not all do (converted ones carry none, one carried a joke), so the shape of the weights
// decides first - every model names its layers its own way:
//   Krea 2         blocks.N.attn.wq / .gate (and txtfusion), 6144 wide
//   MOSS-Image     layers.N.proj.qkv / .ffn_up_gate, 3072 wide
//   Qwen Image 2.1 transformer_blocks.N.img_mlp / txt_mlp, 4096 wide (Qwen Image 1: 3072)
//   MiniMax H3     blocks.N.attn.qkv_proj and mlp.fc1
// and the usual older ones (Flux, SDXL, SD 1.5, Wan...). The file name is only a last guess, never sorted without the user ticking it.
//
// ComfyUI lists a LoRA in a subfolder as "Krea 2\name.safetensors". The app finds its LoRAs either way (comfy.cpp FixLoraNames: every
// lora_name sent is matched by its file name), but a workflow saved inside ComfyUI that names a moved LoRA must pick it again there - the
// sorter says so, and Put them back undoes a sort.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <windows.h>

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

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::wstring EnvW(const wchar_t* name) {
    wchar_t buf[1024] = {};
    const DWORD n = GetEnvironmentVariableW(name, buf, 1024);
    return n > 0 && n < 1024 ? std::wstring(buf) : std::wstring();
}

// The models the sorter knows, in the order they are offered; each is also the folder name
const char* const kModels[] = {"Krea 2", "Qwen Image 2.1", "MOSS-Image", "MiniMax H3", "Qwen Image", "Flux", "SDXL", "SD 1.5", "SD 2", "SD 3",
                               "Wan", "LTX Video", "Hunyuan Video", "Z-Image", "Chroma", "HiDream", "Lumina"};
constexpr int kModelCount = static_cast<int>(sizeof(kModels) / sizeof(kModels[0]));

bool IsModelFolder(const std::wstring& name) {
    for (const char* m : kModels) {
        if (_wcsicmp(W(m).c_str(), name.c_str()) == 0) return true;
    }
    return false;
}

struct Guess {
    std::string model; // "" = not known
    std::string how;   // "its training tag", "the shape of its weights", "its file name"
    bool sure = false; // from the file's contents (the name alone is not)
};

// The model named in the header's training tags (only names this file knows; anything else is ignored)
std::string ModelFromTags(const json& meta) {
    if (!meta.is_object()) return {};
    std::string t;
    for (const char* k : {"ss_base_model_version", "modelspec.architecture", "base_model", "ss_sd_model_name", "base_model_version", "model_type"}) {
        if (meta.contains(k) && meta[k].is_string()) t += " " + Lower(meta[k].get<std::string>());
    }
    if (t.empty()) return {};
    auto has = [&](const char* s) { return t.find(s) != std::string::npos; };
    if (has("krea2") || has("krea-2") || has("krea_2")) return "Krea 2";
    if (has("moss")) return "MOSS-Image";
    if (has("minimax_h3") || has("minimax-h3")) return "MiniMax H3";
    if (has("qwen_image_2") || has("qwen-image-2") || has("qwen_image2") || has("qwenimage2")) return "Qwen Image 2.1";
    if (has("qwen_image") || has("qwen-image")) return "Qwen Image";
    if (has("chroma")) return "Chroma";
    if (has("flux")) return "Flux";
    if (has("sdxl") || has("sd_xl") || has("stable-diffusion-xl") || has("illustrious") || has("pony") || has("noobai")) return "SDXL";
    if (has("sd3") || has("stable-diffusion-3")) return "SD 3";
    if (has("sd_v2") || has("stable-diffusion-v2")) return "SD 2";
    if (has("sd_v1") || has("stable-diffusion-v1") || has("sd1.5") || has("sd15")) return "SD 1.5";
    if (has("wan2") || has("wan_2") || has("wan-2") || has("wanvideo") || has("wan_video")) return "Wan";
    if (has("ltx")) return "LTX Video";
    if (has("hunyuan")) return "Hunyuan Video";
    if (has("z_image") || has("z-image") || has("zimage")) return "Z-Image";
    if (has("hidream")) return "HiDream";
    if (has("lumina")) return "Lumina";
    return {};
}

// The model by how the weights are named and how wide they are (keys lower-cased, dots as underscores, so kohya and diffusers names match)
std::string ModelFromLayout(const json& tensors) {
    bool qkvProj = false, mlpFc = false, wq = false, txtfusion = false, mossProj = false, imgMlp = false, doubleBlocks = false, singleBlocks = false,
         tokenRefiner = false, selfAttnQ = false, crossAttn = false, unetBlocks = false, te2 = false, attn1 = false, attn2 = false, layersAttention = false;
    int qWidth = 0, crossWidth = 0, maxDouble = -1;
    for (auto it = tensors.begin(); it != tensors.end(); ++it) {
        std::string k = Lower(it.key());
        std::replace(k.begin(), k.end(), '.', '_');
        auto has = [&](const char* s) { return k.find(s) != std::string::npos; };
        if (has("attn_qkv_proj")) qkvProj = true;
        if (has("mlp_fc1") || has("mlp_fc2")) mlpFc = true;
        if ((has("attn_wq") || has("attn_wk") || has("attn_gate")) && has("blocks_")) wq = true;
        if (has("txtfusion")) txtfusion = true;
        if (has("layers_") && (has("proj_qkv") || has("proj_ffn_up_gate") || has("proj_ffn_down"))) mossProj = true;
        if (has("img_mlp") || has("txt_mlp") || has("img_mod_")) imgMlp = true;
        if (has("double_blocks")) {
            doubleBlocks = true;
            const size_t at = k.find("double_blocks_");
            if (at != std::string::npos) maxDouble = std::max(maxDouble, std::atoi(k.c_str() + at + 14));
        }
        if (has("single_blocks") || has("single_transformer_blocks")) singleBlocks = true;
        if (has("individual_token_refiner") || has("txt_in_")) tokenRefiner = true;
        if (has("self_attn_q")) selfAttnQ = true;
        if (has("cross_attn")) crossAttn = true;
        if (has("input_blocks") || has("output_blocks") || has("down_blocks") || has("up_blocks") || has("middle_block") || has("mid_block")) unetBlocks = true;
        if (has("lora_te2") || has("text_encoder_2")) te2 = true;
        if (has("attn1")) attn1 = true;
        if (has("attn2")) attn2 = true;
        if (has("layers_") && (has("attention_to_q") || has("attention_qkv") || has("feed_forward"))) layersAttention = true;
        // how wide: the input of a down / A matrix of a query
        const bool down = has("lora_down") || has("lora_a");
        if (down && it.value().is_object() && it.value().contains("shape") && it.value()["shape"].is_array() && it.value()["shape"].size() == 2) {
            const int in = it.value()["shape"][1].get<int>();
            if (has("to_q") || has("attn_wq") || has("qkv")) qWidth = std::max(qWidth, in);
            if (has("attn2_to_k")) crossWidth = std::max(crossWidth, in);
        }
    }
    if (qkvProj && mlpFc) return "MiniMax H3";
    if (wq || txtfusion) return "Krea 2";
    if (mossProj) return "MOSS-Image";
    if (imgMlp) return qWidth == 3072 ? "Qwen Image" : "Qwen Image 2.1";
    if (doubleBlocks || singleBlocks) return tokenRefiner || maxDouble >= 19 ? "Hunyuan Video" : "Flux";
    if (selfAttnQ && crossAttn) return "Wan";
    if (unetBlocks) return te2 || crossWidth == 2048 ? "SDXL" : crossWidth == 1024 ? "SD 2" : crossWidth == 768 ? "SD 1.5" : "SDXL";
    if (attn1 && attn2) return "LTX Video";
    if (layersAttention) return "Z-Image";
    return {};
}

std::string ModelFromName(const std::string& fileName) {
    const std::string n = Lower(fileName);
    auto has = [&](const char* s) { return n.find(s) != std::string::npos; };
    if (has("krea")) return "Krea 2";
    if (has("moss")) return "MOSS-Image";
    if (has("minimax") || has("_h3") || has("h3_") || has("-h3") || has("mmh3")) return "MiniMax H3";
    if (has("qwen")) return "Qwen Image 2.1";
    if (has("flux")) return "Flux";
    if (has("sdxl") || has("illustrious") || has("pony")) return "SDXL";
    if (has("wan")) return "Wan";
    return {};
}

Guess Classify(const std::wstring& path) {
    Guess g;
    const std::string name = U8(fs::path(path).filename().wstring());
    std::ifstream f(path, std::ios::binary);
    unsigned long long n = 0;
    if (f && Lower(fs::path(path).extension().string()) == ".safetensors" && f.read(reinterpret_cast<char*>(&n), 8) && n > 2 && n < 200ull * 1024 * 1024) {
        std::string head(static_cast<size_t>(n), '\0');
        if (f.read(head.data(), static_cast<std::streamsize>(n))) {
            json h = json::parse(head, nullptr, false);
            if (h.is_object()) {
                json meta = h.contains("__metadata__") ? h["__metadata__"] : json();
                h.erase("__metadata__");
                const std::string byLayout = ModelFromLayout(h), byTag = ModelFromTags(meta);
                if (!byLayout.empty()) g = {byLayout, byTag == byLayout ? "training tag + weight shapes" : "weight shapes", true};
                else if (!byTag.empty()) g = {byTag, "training tag", true};
            }
        }
    }
    if (g.model.empty()) {
        g.model = ModelFromName(name);
        if (!g.model.empty()) g.how = "file name only: check it";
    }
    return g;
}

// Read once per file (and again when it changes)
std::mutex g_cacheMutex;
std::map<std::wstring, std::pair<std::string, Guess>> g_cache; // path -> (size+time, guess)

Guess ClassifyCached(const std::wstring& path) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    const auto time = fs::last_write_time(path, ec).time_since_epoch().count();
    const std::string stamp = std::to_string(size) + ":" + std::to_string(time);
    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        auto it = g_cache.find(path);
        if (it != g_cache.end() && it->second.first == stamp) return it->second.second;
    }
    Guess g = Classify(path);
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    g_cache[path] = {stamp, g};
    return g;
}

// ComfyUI's loras folders on this computer (tests: MV_LORA_ROOT, a folder of their own)
std::vector<std::wstring> LoraRoots() {
    std::vector<std::wstring> out;
    const std::wstring test = EnvW(L"MV_LORA_ROOT");
    if (!test.empty()) return {test};
    std::error_code ec;
    auto add = [&](const std::wstring& d) {
        if (d.empty() || !fs::is_directory(d, ec)) return;
        for (const std::wstring& o : out) {
            if (fs::equivalent(o, d, ec)) return;
        }
        out.push_back(d);
    };
    const std::wstring models = ComfyModelsDir(), install = ComfyInstallDir();
    if (!models.empty()) add(models + L"\\loras");
    if (!install.empty()) add(install + L"\\ComfyUI\\models\\loras"), add(install + L"\\models\\loras");
    return out;
}

// The file behind one of ComfyUI's LoRA names ("Krea 2\x.safetensors"), "" = not on this computer
std::wstring LoraFile(const std::string& comfyName) {
    std::wstring rel = W(comfyName);
    std::replace(rel.begin(), rel.end(), L'/', L'\\');
    std::error_code ec;
    for (const std::wstring& root : LoraRoots()) {
        const fs::path p = fs::path(root) / rel;
        if (fs::is_regular_file(p, ec)) return p.wstring();
    }
    return {};
}

std::string ModelOfName(const std::string& comfyName) {
    const std::wstring file = LoraFile(comfyName);
    if (!file.empty()) {
        const Guess g = ClassifyCached(file);
        if (!g.model.empty()) return g.model;
    }
    // not on this computer: its folder if it was sorted, else its name
    const size_t at = comfyName.find_first_of("/\\");
    if (at != std::string::npos && IsModelFolder(W(comfyName.substr(0, at)))) return comfyName.substr(0, at);
    return ModelFromName(comfyName);
}

// ---- the LoRAs ComfyUI offers, each with its model: asked in the background, again after a minute or when the sorter moved files
using Offer = LoraOffer;
struct OfferList {
    std::vector<Offer> list;
    bool known = false;
    std::future<std::vector<Offer>> fut;
    double asked = -1000.0;
} g_offers;

std::vector<Offer> FetchOffers(std::string url) {
    std::vector<Offer> out;
    for (const std::string& n : ComfyListLoras(url)) out.push_back({n, ModelOfName(n)});
    return out;
}

void PumpOffers(AppState& app) {
    if (g_offers.fut.valid() && g_offers.fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        g_offers.list = g_offers.fut.get();
        g_offers.known = !g_offers.list.empty();
    }
    if (!g_offers.fut.valid() && ImGui::GetTime() - g_offers.asked > 60.0) {
        g_offers.asked = ImGui::GetTime();
        g_offers.fut = std::async(std::launch::async, FetchOffers, app.comfy.url);
    }
}

std::string ShortName(const std::string& name) {
    const size_t at = name.find_last_of("/\\");
    std::string s = at == std::string::npos ? name : name.substr(at + 1);
    if (s.size() > 12 && Lower(s.substr(s.size() - 12)) == ".safetensors") s.resize(s.size() - 12);
    return s;
}

// ---- the sorter
struct Row {
    std::wstring path;    // the file now
    std::wstring root;    // the loras folder it is in
    std::string file;     // its name
    double mb = 0;
    Guess guess;
    int model = -1;       // index into kModels, -1 = leave it where it is
    bool on = false;
};
struct Move {
    std::wstring from, to;
};
struct Sorter {
    bool open = false;
    bool scanning = false;
    std::future<std::pair<std::vector<Row>, int>> scan;
    std::vector<Row> rows;
    int alreadySorted = 0;
    std::string note;
} g_sort;

std::wstring MovesFile() {
    wchar_t buf[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    return std::wstring(buf) + L"\\Try2Create\\lora_moves.json";
}

std::vector<Move> ReadMoves() {
    std::vector<Move> out;
    std::ifstream f(MovesFile(), std::ios::binary);
    if (!f) return out;
    const json j = json::parse(f, nullptr, false);
    if (!j.is_array()) return out;
    for (const json& m : j) {
        if (m.is_object() && m.contains("from") && m.contains("to")) out.push_back({W(m["from"].get<std::string>()), W(m["to"].get<std::string>())});
    }
    return out;
}

void WriteMoves(const std::vector<Move>& moves) {
    json j = json::array();
    for (const Move& m : moves) j.push_back({{"from", U8(m.from)}, {"to", U8(m.to)}});
    std::ofstream f(MovesFile(), std::ios::binary | std::ios::trunc);
    if (f) f << j.dump(1);
}

int ModelIndex(const std::string& model) {
    for (int i = 0; i < kModelCount; ++i) {
        if (model == kModels[i]) return i;
    }
    return -1;
}

// Every LoRA lying loose in a loras folder (files already in a folder of their own are left as they are)
std::vector<Row> Scan(int* already) {
    std::vector<Row> out;
    int sorted = 0;
    std::error_code ec;
    for (const std::wstring& root : LoraRoots()) {
        for (const auto& e : fs::directory_iterator(root, ec)) {
            if (e.is_directory(ec)) {
                if (IsModelFolder(e.path().filename().wstring())) {
                    for (const auto& f : fs::recursive_directory_iterator(e.path(), ec)) sorted += f.is_regular_file(ec) ? 1 : 0;
                }
                continue;
            }
            const std::string ext = Lower(e.path().extension().string());
            if (ext != ".safetensors" && ext != ".pt" && ext != ".ckpt" && ext != ".bin") continue;
            Row r;
            r.path = e.path().wstring();
            r.root = root;
            r.file = U8(e.path().filename().wstring());
            r.mb = static_cast<double>(e.file_size(ec)) / (1024.0 * 1024.0);
            r.guess = ClassifyCached(r.path);
            r.model = ModelIndex(r.guess.model);
            r.on = r.guess.sure && r.model >= 0; // a guess from the name alone waits for a tick
            out.push_back(r);
        }
    }
    std::sort(out.begin(), out.end(), [](const Row& a, const Row& b) { return a.model != b.model ? (a.model < 0 ? false : b.model < 0 ? true : a.model < b.model) : Lower(a.file) < Lower(b.file); });
    if (already) *already = sorted;
    return out;
}

void StartScan() {
    g_sort.scanning = true;
    g_sort.scan = std::async(std::launch::async, [] {
        int already = 0;
        std::vector<Row> rows = Scan(&already);
        return std::make_pair(std::move(rows), already);
    });
}

void DoMoves() {
    std::vector<Move> log = ReadMoves();
    int moved = 0, failed = 0, skipped = 0;
    std::string firstError;
    std::error_code ec;
    for (Row& r : g_sort.rows) {
        if (!r.on || r.model < 0) continue;
        const fs::path dir = fs::path(r.root) / W(kModels[r.model]);
        const fs::path to = dir / fs::path(r.path).filename();
        fs::create_directories(dir, ec);
        if (fs::exists(to, ec)) {
            ++skipped;
            if (firstError.empty()) firstError = r.file + ": a file of that name is already in " + kModels[r.model];
            continue;
        }
        if (MoveFileExW(r.path.c_str(), to.wstring().c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) {
            log.push_back({r.path, to.wstring()});
            ++moved;
        } else {
            ++failed;
            if (firstError.empty()) firstError = r.file + (GetLastError() == ERROR_SHARING_VIOLATION ? ": in use (ComfyUI has it loaded; try again when it is idle)" : ": could not be moved");
        }
    }
    WriteMoves(log);
    g_sort.note = "Moved " + std::to_string(moved) + (moved == 1 ? " LoRA" : " LoRAs") + (skipped ? ", " + std::to_string(skipped) + " left (same name already there)" : "") +
                  (failed ? ", " + std::to_string(failed) + " could not be moved" : "") + (firstError.empty() ? "" : ". " + firstError);
    g_offers.asked = -1000.0; // ComfyUI lists them under their new names
    StartScan();
}

void PutBack() {
    std::vector<Move> log = ReadMoves(), left;
    int back = 0;
    std::error_code ec;
    for (auto it = log.rbegin(); it != log.rend(); ++it) {
        if (fs::exists(it->to, ec) && !fs::exists(it->from, ec) && MoveFileExW(it->to.c_str(), it->from.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) {
            ++back;
            const fs::path dir = fs::path(it->to).parent_path();
            if (fs::is_empty(dir, ec)) fs::remove(dir, ec); // the model folder the sort made, now empty
        } else if (fs::exists(it->to, ec)) {
            left.push_back(*it);
        }
    }
    std::reverse(left.begin(), left.end());
    WriteMoves(left);
    g_sort.note = "Put " + std::to_string(back) + (back == 1 ? " LoRA" : " LoRAs") + " back where they were" + (left.empty() ? "" : " (" + std::to_string(left.size()) + " could not be)");
    g_offers.asked = -1000.0;
    StartScan();
}

} // namespace

std::string LoraModelOf(const std::string& comfyName) { return ModelOfName(comfyName); }

const std::vector<LoraOffer>& LoraOffers(AppState& app) {
    PumpOffers(app);
    return g_offers.list;
}

void RefreshLoraOffers() { g_offers.asked = -1000.0; }

std::wstring LoraLocalFile(const std::string& comfyName) { return LoraFile(comfyName); }

std::string LoraHeaderMetadata(const std::wstring& file) {
    std::ifstream f(file, std::ios::binary);
    unsigned long long n = 0;
    if (!f || !f.read(reinterpret_cast<char*>(&n), 8) || n < 2 || n > 200ull * 1024 * 1024) return {};
    std::string head(static_cast<size_t>(n), '\0');
    if (!f.read(head.data(), static_cast<std::streamsize>(n))) return {};
    const json h = json::parse(head, nullptr, false);
    return h.is_object() && h.contains("__metadata__") ? h["__metadata__"].dump() : std::string();
}

bool StrengthSlider(const char* id, double& strength, float width) {
    float f = static_cast<float>(strength);
    ImGui::SetNextItemWidth(width);
    bool changed = ImGui::SliderFloat(id, &f, 0.0f, 2.0f, "%.2f");
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    if (changed) {
        if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) { // dragged: it snaps to the marks (every 0.25) near them
            const float q = std::round(f * 4.0f) / 4.0f;
            if (std::fabs(q - f) < 0.05f) f = q;
        }
        strength = std::clamp(std::round(f * 100.0f) / 100.0, -1.0, 2.0);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY); // (the wheel steps it, not the page)
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            strength = std::clamp(std::round(strength * 4.0 + (wheel > 0 ? 1.0 : -1.0)) / 4.0, 0.0, 2.0);
            changed = true;
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) strength = 1.0, changed = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("Strength: 1 = as trained, lower = weaker, above 1 = stronger (2 at most).\nA mark every 0.25 (it snaps to them); mouse wheel: 0.25 at a "
                          "time; right-click: 1; Ctrl+click: type it (below 0 = the opposite).");
    // the marks: every 0.25, longer at 0, 1 and 2 (where the grab's centre sits for that value)
    const ImGuiStyle& st = ImGui::GetStyle();
    const float pad = 2.0f, grab = st.GrabMinSize;
    const float x0 = a.x + pad + grab * 0.5f, x1 = b.x - pad - grab * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int k = 0; k <= 8; ++k) {
        const float x = std::round(x0 + (x1 - x0) * k / 8.0f);
        const bool whole = k % 4 == 0;
        dl->AddLine(ImVec2(x, b.y - ui::Dp(whole ? 6.0f : 4.0f)), ImVec2(x, b.y - 1.0f), ImGui::GetColorU32(ImGuiCol_Text, whole ? 0.55f : 0.32f), 1.0f);
    }
    return changed;
}

// One model's LoRAs: on / off, the name, the strength, take it out; "+ LoRA" lists what ComfyUI has for this model
bool DrawModelLoras(AppState& app, std::vector<LoraEntry>& list, const char* model, const char* id, const char* label) {
    PumpOffers(app);
    bool changed = false;
    ImGui::PushID(id);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", label);
    int remove = -1;
    for (size_t i = 0; i < list.size(); ++i) {
        LoraEntry& l = list[i];
        ImGui::PushID(static_cast<int>(i));
        const std::string shortName = ShortName(l.name);
        // one LoRA: its showcase picture, tick, name, strength, x - on the same line while it fits
        const float tw = ui::Dp(32.0f), th = ui::Dp(40.0f);
        const float rowW = tw + ImGui::GetFrameHeight() + ImGui::CalcTextSize(shortName.c_str()).x + ui::Dp(170.0f) + ui::Dp(52.0f);
        const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        if (ImGui::GetItemRectMax().x + ui::Dp(14.0f) + rowW <= right) ImGui::SameLine(0, ui::Dp(14.0f));
        ImGui::BeginGroup();
        if (DrawLoraTile(app, l.name, tw, th)) ShowLoraInBrowser(l.name, model);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("See what it does (LoRAs page)");
        ImGui::SameLine(0, ui::Dp(4.0f));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (th - ImGui::GetFrameHeight()) * 0.5f);
        if (ImGui::Checkbox("##on", &l.on)) changed = true;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("On / off (it stays in the list)");
        ImGui::SameLine(0, ui::Dp(3.0f));
        ImGui::AlignTextToFramePadding();
        if (l.on) ImGui::TextUnformatted(shortName.c_str());
        else ImGui::TextDisabled("%s", shortName.c_str());
        const std::string trig = LoraTrigger(l.name);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("%s%s%s", l.name.c_str(), trig.empty() ? "" : "\nTrigger words: ", trig.c_str());
        ImGui::SameLine(0, ui::Dp(4.0f));
        if (StrengthSlider("##str", l.strength, ui::Dp(170.0f))) changed = true;
        ImGui::SameLine(0, ui::Dp(2.0f));
        if (ImGui::SmallButton("x")) remove = static_cast<int>(i);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Take it out");
        ImGui::EndGroup();
        ImGui::PopID();
    }
    if (remove >= 0) list.erase(list.begin() + remove), changed = true;
    if (ImGui::GetItemRectMax().x + ui::Dp(80.0f) <= ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x) ImGui::SameLine(0, ui::Dp(10.0f));
    static bool testPick = GetEnvironmentVariableW(L"MV_LORA_PICK", nullptr, 0) > 0; // tests: the first list's "+ LoRA" opens by itself (after 3 s)
    if (ImGui::SmallButton("+ LoRA") || (testPick && ImGui::GetTime() > 3.0 && !(testPick = false))) {
        ImGui::OpenPopup("##addlora");
        g_offers.asked = -1000.0; // a fresh list (one may have been added)
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Add one of ComfyUI's LoRAs made for %s", model && *model ? model : "this model");
    if (ImGui::BeginPopup("##addlora")) {
        static char filter[64] = {};
        static bool all = false;
        ImGui::SetNextItemWidth(ui::Dp(220.0f));
        ImGui::InputTextWithHint("##filter", "Search", filter, sizeof(filter));
        ImGui::SameLine();
        ImGui::Checkbox("Every LoRA", &all);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Also the ones made for other models (they usually do nothing, or break the picture)");
        ImGui::BeginChild("##offers", ImVec2(ui::Dp(600.0f), ui::Dp(420.0f)));
        const std::string f = Lower(filter);
        int shown = 0;
        const float cw = ui::Dp(104.0f), ch = cw * 1.25f, gap = ui::Dp(8.0f);
        const int cols = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + gap) / (cw + gap)));
        for (const Offer& o : g_offers.list) {
            const bool fits = !model || !*model || o.model == model;
            if (!all && !fits) continue;
            if (!f.empty() && Lower(o.name).find(f) == std::string::npos) continue;
            const bool there = std::any_of(list.begin(), list.end(), [&](const LoraEntry& l) { return LoraFileName(l.name) == LoraFileName(o.name); });
            if (shown++ % cols) ImGui::SameLine(0, gap);
            ImGui::BeginGroup();
            ImGui::PushID(o.name.c_str());
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const bool click = DrawLoraTile(app, o.name, cw, ch);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("%s%s%s%s", o.name.c_str(), o.model.empty() ? "" : "\nFor ", o.model.c_str(), there ? "\n(already in the list)" : "\nClick: add it");
            if (there) { // added: dimmed, with a tick
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(p, ImVec2(p.x + cw, p.y + ch), IM_COL32(0, 0, 0, 140), ui::Dp(4.0f));
                const char* t = "added";
                const ImVec2 ts = ImGui::CalcTextSize(t);
                const ImVec2 at(p.x + (cw - ts.x) * 0.5f, p.y + ch - ts.y - ui::Dp(9.0f)); // (at the bottom: a name drawn in the box stays readable)
                dl->AddRectFilled(ImVec2(at.x - ui::Dp(8.0f), at.y - ui::Dp(3.0f)), ImVec2(at.x + ts.x + ui::Dp(8.0f), at.y + ts.y + ui::Dp(3.0f)), ImGui::GetColorU32(ui::kAccent),
                                  ui::Dp(9.0f));
                dl->AddText(at, IM_COL32(255, 255, 255, 240), t);
            }
            if (click && !there) {
                list.push_back({o.name, 1.0, true});
                changed = true;
                ImGui::CloseCurrentPopup();
            }
            // the name, cut to the card (another model's: shown in brackets)
            std::string n = ShortName(o.name) + (fits || o.model.empty() ? std::string() : " (" + o.model + ")");
            const size_t full = n.size();
            while (n.size() > 3 && ImGui::CalcTextSize(n.c_str()).x > cw) n.pop_back();
            if (n.size() < full && n.size() > 3) n = n.substr(0, n.size() - 2) + "..";
            if (there) ImGui::TextDisabled("%s", n.c_str());
            else ImGui::TextUnformatted(n.c_str());
            ImGui::PopID();
            ImGui::EndGroup();
        }
        if (!g_offers.known) ImGui::TextDisabled(g_offers.fut.valid() ? "Asking ComfyUI for its LoRAs..." : "ComfyUI did not answer: start it (Generate > ComfyUI) to pick LoRAs.");
        else if (!shown) ImGui::TextWrapped("No LoRA for %s in ComfyUI's loras folder%s.", model && *model ? model : "it", all ? "" : " (tick Every LoRA to see the others)");
        ImGui::EndChild();
        if (ImGui::SmallButton("See what they do (LoRAs page)...")) {
            OpenLoraBrowser(model);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    // the trigger words of those that are on: put into the prompt when it is sent (a LoRA often does little without them)
    std::string words;
    for (const LoraEntry& l : list) {
        if (!l.on || l.strength <= 0) continue;
        const std::string t = LoraTrigger(l.name);
        if (!t.empty()) words += (words.empty() ? "" : ", ") + t;
    }
    if (!words.empty()) {
        if (ImGui::Checkbox("Add their trigger words:##trig", &app.comfy.loraTriggers)) changed = true;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("Put into the prompt when it is sent (not twice: words already in it are left alone).\nFound in the LoRA's file, or on Civitai.");
        ImGui::SameLine();
        ImGui::PushTextWrapPos(0);
        if (app.comfy.loraTriggers) ImGui::TextColored(ui::kAccent, "%s", words.c_str());
        else ImGui::TextDisabled("%s", words.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::PopID();
    if (changed) SaveComfySettings(app.comfy);
    return changed;
}

void OpenLoraSorter() {
    g_sort.open = true;
    g_sort.note.clear();
    StartScan();
}

void DrawLoraSorter(AppState& app) {
    (void)app;
    static bool testOnce = [] { // tests: MV_UI_TEST "lorasort" opens it; "lorasort:move" also presses Move once it has looked (MV_LORA_ROOT: the test folder)
        char buf[256] = {};
        return GetEnvironmentVariableA("MV_UI_TEST", buf, sizeof(buf)) && strstr(buf, "lorasort") != nullptr;
    }();
    static bool testMove = [] {
        char buf[256] = {};
        return GetEnvironmentVariableA("MV_UI_TEST", buf, sizeof(buf)) && strstr(buf, "lorasort:move") != nullptr;
    }();
    static bool testBack = [] { // "lorasort:back": presses Put them back once it has looked
        char buf[256] = {};
        return GetEnvironmentVariableA("MV_UI_TEST", buf, sizeof(buf)) && strstr(buf, "lorasort:back") != nullptr;
    }();
    if (testOnce) testOnce = false, OpenLoraSorter();
    if (g_sort.scanning && g_sort.scan.valid() && g_sort.scan.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto [rows, already] = g_sort.scan.get();
        g_sort.rows = std::move(rows);
        g_sort.alreadySorted = already;
        g_sort.scanning = false;
        if (testMove) testMove = false, DoMoves();
        else if (testBack) testBack = false, PutBack();
    }
    if (!g_sort.open) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(ui::Dp(860.0f), ui::Dp(600.0f)), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Sort LoRAs by model", &g_sort.open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    const std::vector<std::wstring> roots = LoraRoots();
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted("Each LoRA goes into a folder named after the model it was made for, inside ComfyUI's loras folder. The model is read from the "
                           "file itself (its training tag, or how its weights are shaped), so a badly named file still lands right.");
    ImGui::TextDisabled("This app finds every LoRA after the move. In ComfyUI itself a sorted LoRA is listed as \"Krea 2\\name\": a workflow you saved in "
                        "ComfyUI that uses a moved LoRA needs it picked again there. Put them back undoes the sort.");
    ImGui::PopTextWrapPos();
    if (roots.empty()) {
        ImGui::TextColored(ui::kAccent, "ComfyUI's loras folder was not found on this computer.");
        ImGui::End();
        return;
    }
    for (const std::wstring& r : roots) ImGui::TextDisabled("Folder: %s", U8(r).c_str());
    ImGui::Separator();
    const float footer = ImGui::GetFrameHeightWithSpacing() * 2.2f;
    if (g_sort.scanning) {
        ImGui::TextUnformatted("Reading the LoRA files...");
    } else if (g_sort.rows.empty()) {
        ImGui::TextUnformatted(g_sort.alreadySorted ? "Every LoRA is already in a model folder." : "No LoRA files lie loose in the loras folder.");
    } else if (ImGui::BeginTable("##loras", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable,
                                 ImVec2(0, ImGui::GetContentRegionAvail().y - footer))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ui::Dp(24.0f));
        ImGui::TableSetupColumn("LoRA", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, ui::Dp(70.0f));
        ImGui::TableSetupColumn("Model (its folder)", ImGuiTableColumnFlags_WidthFixed, ui::Dp(170.0f));
        ImGui::TableSetupColumn("Known by", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < g_sort.rows.size(); ++i) {
            Row& r = g_sort.rows[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(r.model < 0);
            ImGui::Checkbox("##on", &r.on);
            ImGui::EndDisabled();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(r.file.c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled(r.mb >= 1024 ? "%.1f GB" : "%.0f MB", r.mb >= 1024 ? r.mb / 1024.0 : r.mb);
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##model", r.model < 0 ? "(leave it)" : kModels[r.model])) {
                if (ImGui::Selectable("(leave it)", r.model < 0)) r.model = -1, r.on = false;
                for (int m = 0; m < kModelCount; ++m) {
                    if (ImGui::Selectable(kModels[m], r.model == m)) r.model = m, r.on = true;
                }
                ImGui::EndCombo();
            }
            ImGui::TableNextColumn();
            if (r.guess.model.empty()) ImGui::TextDisabled("not known: pick its model, or leave it");
            else if (!r.guess.sure) ImGui::TextColored(ui::kAccent, "%s", r.guess.how.c_str());
            else ImGui::TextDisabled("%s", r.guess.how.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    int ticked = 0;
    for (const Row& r : g_sort.rows) ticked += r.on && r.model >= 0 ? 1 : 0;
    ImGui::BeginDisabled(!ticked || g_sort.scanning);
    char lbl[64];
    snprintf(lbl, sizeof(lbl), "Move %d into model folders", ticked);
    if (ui::PrimaryButton(lbl)) DoMoves();
    ImGui::EndDisabled();
    const size_t done = ReadMoves().size();
    if (done) {
        ImGui::SameLine();
        char back[64];
        snprintf(back, sizeof(back), "Put them back (%d)", static_cast<int>(done));
        if (ImGui::Button(back)) PutBack();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Moves every LoRA the sorter moved back to where it was");
    }
    ImGui::SameLine();
    if (ImGui::Button("Look again")) StartScan();
    if (g_sort.alreadySorted) {
        ImGui::SameLine();
        ImGui::TextDisabled("%d already in model folders", g_sort.alreadySorted);
    }
    if (!g_sort.note.empty()) ImGui::TextColored(ui::kAccent, "%s", g_sort.note.c_str());
    ImGui::End();
}
