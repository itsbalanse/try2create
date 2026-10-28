#include <cstring>
#include "workflow.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <fstream>
#include <functional>
#include <set>
#include <windows.h>
#include "image_texture.h"
#include "json.hpp"

using json = nlohmann::ordered_json; // keeps the order the keys have in the file (widget order)

namespace {

constexpr float kTitleH = 30.0f;
constexpr float kSlotH = 20.0f;
constexpr float kWidgetH = 24.0f;
constexpr float kMultilineH = 90.0f;

std::string Lower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return r;
}

bool LooksLikeJsonObject(const std::string& s) {
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        return c == '{';
    }
    return false;
}

std::string Scalar(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_null()) return std::string();
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    return v.dump();
}

uint32_t ParseColor(const std::string& s, bool& ok) {
    ok = false;
    if (s.size() == 7 && s[0] == '#') {
        char* end = nullptr;
        unsigned long v = std::strtoul(s.c_str() + 1, &end, 16);
        if (end && *end == 0) {
            ok = true;
            return static_cast<uint32_t>(v);
        }
    } else if (s.size() == 4 && s[0] == '#') {
        char* end = nullptr;
        unsigned long v = std::strtoul(s.c_str() + 1, &end, 16);
        if (end && *end == 0) {
            ok = true;
            return static_cast<uint32_t>(((v >> 8) & 0xF) * 0x11 << 16 | ((v >> 4) & 0xF) * 0x11 << 8 | (v & 0xF) * 0x11);
        }
    }
    return 0;
}

bool ReadPair(const json& v, float& a, float& b) {
    if (v.is_array() && v.size() >= 2 && v[0].is_number() && v[1].is_number()) {
        a = v[0].get<float>();
        b = v[1].get<float>();
        return true;
    }
    if (v.is_object() && v.contains("0") && v.contains("1") && v["0"].is_number() && v["1"].is_number()) {
        a = v["0"].get<float>();
        b = v["1"].get<float>();
        return true;
    }
    return false;
}

// Widget names for the standard nodes, in widgets_values order (the file itself doesn't name them).
const std::vector<const char*>* KnownWidgets(const std::string& type) {
    static const std::unordered_map<std::string, std::vector<const char*>> table = {
        {"KSampler", {"seed", "control_after_generate", "steps", "cfg", "sampler_name", "scheduler", "denoise"}},
        {"KSamplerAdvanced", {"add_noise", "noise_seed", "control_after_generate", "steps", "cfg", "sampler_name", "scheduler",
                              "start_at_step", "end_at_step", "return_with_leftover_noise"}},
        {"CLIPTextEncode", {"text"}},
        {"CheckpointLoaderSimple", {"ckpt_name"}},
        {"CheckpointLoader", {"config_name", "ckpt_name"}},
        {"EmptyLatentImage", {"width", "height", "batch_size"}},
        {"EmptySD3LatentImage", {"width", "height", "batch_size"}},
        {"LatentUpscale", {"upscale_method", "width", "height", "crop"}},
        {"LatentUpscaleBy", {"upscale_method", "scale_by"}},
        {"SaveImage", {"filename_prefix"}},
        {"LoraLoader", {"lora_name", "strength_model", "strength_clip"}},
        {"LoraLoaderModelOnly", {"lora_name", "strength_model"}},
        {"VAELoader", {"vae_name"}},
        {"CLIPLoader", {"clip_name", "type"}},
        {"UNETLoader", {"unet_name", "weight_dtype"}},
        {"DualCLIPLoader", {"clip_name1", "clip_name2", "type"}},
        {"ControlNetLoader", {"control_net_name"}},
        {"ControlNetApply", {"strength"}},
        {"ControlNetApplyAdvanced", {"strength", "start_percent", "end_percent"}},
        {"LoadImage", {"image", "upload"}},
        {"ImageScale", {"upscale_method", "width", "height", "crop"}},
        {"ImageScaleBy", {"upscale_method", "scale_by"}},
        {"UpscaleModelLoader", {"model_name"}},
        {"CLIPSetLastLayer", {"stop_at_clip_layer"}},
        {"KSamplerSelect", {"sampler_name"}},
        {"BasicScheduler", {"scheduler", "steps", "denoise"}},
        {"RandomNoise", {"noise_seed", "control_after_generate"}},
        {"CFGGuider", {"cfg"}},
        {"FluxGuidance", {"guidance"}},
        {"ModelSamplingFlux", {"max_shift", "base_shift", "width", "height"}},
        {"VAEEncodeForInpaint", {"grow_mask_by"}},
        {"Note", {"text"}},
        {"MarkdownNote", {"text"}},
        {"PrimitiveNode", {"value", "control_after_generate"}},
    };
    auto it = table.find(type);
    return it == table.end() ? nullptr : &it->second;
}

// Output types of the common nodes (API-format files don't say what a link carries).
const std::vector<const char*>* KnownOutputs(const std::string& type) {
    static const std::unordered_map<std::string, std::vector<const char*>> table = {
        {"CheckpointLoaderSimple", {"MODEL", "CLIP", "VAE"}},
        {"CheckpointLoader", {"MODEL", "CLIP", "VAE"}},
        {"CLIPTextEncode", {"CONDITIONING"}},
        {"KSampler", {"LATENT"}},
        {"KSamplerAdvanced", {"LATENT"}},
        {"VAEDecode", {"IMAGE"}},
        {"VAEEncode", {"LATENT"}},
        {"EmptyLatentImage", {"LATENT"}},
        {"EmptySD3LatentImage", {"LATENT"}},
        {"LatentUpscale", {"LATENT"}},
        {"LatentUpscaleBy", {"LATENT"}},
        {"LoraLoader", {"MODEL", "CLIP"}},
        {"LoraLoaderModelOnly", {"MODEL"}},
        {"VAELoader", {"VAE"}},
        {"CLIPLoader", {"CLIP"}},
        {"UNETLoader", {"MODEL"}},
        {"DualCLIPLoader", {"CLIP"}},
        {"CLIPSetLastLayer", {"CLIP"}},
        {"ControlNetLoader", {"CONTROL_NET"}},
        {"ControlNetApply", {"CONDITIONING"}},
        {"ControlNetApplyAdvanced", {"CONDITIONING", "CONDITIONING"}},
        {"LoadImage", {"IMAGE", "MASK"}},
        {"ImageScale", {"IMAGE"}},
        {"ImageScaleBy", {"IMAGE"}},
        {"UpscaleModelLoader", {"UPSCALE_MODEL"}},
        {"ImageUpscaleWithModel", {"IMAGE"}},
        {"KSamplerSelect", {"SAMPLER"}},
        {"BasicScheduler", {"SIGMAS"}},
        {"RandomNoise", {"NOISE"}},
        {"CFGGuider", {"GUIDER"}},
        {"FluxGuidance", {"CONDITIONING"}},
        {"ModelSamplingFlux", {"MODEL"}},
        {"SamplerCustomAdvanced", {"LATENT", "LATENT"}},
    };
    auto it = table.find(type);
    return it == table.end() ? nullptr : &it->second;
}

bool IsLongText(const std::string& v) { return v.size() > 48 || v.find('\n') != std::string::npos; }

void SizeFromContent(WfNode& n) {
    const int rows = static_cast<int>(std::max(n.inputs.size(), n.outputs.size()));
    float h = 10.0f + rows * kSlotH;
    for (const WfWidget& w : n.widgets) h += w.multiline ? kMultilineH + 4.0f : kWidgetH;
    n.h = std::max(h + 8.0f, 46.0f);
    n.w = std::max(n.w, 260.0f);
}

void FinishGraph(WfGraph& g) {
    g.indexById.clear();
    for (size_t i = 0; i < g.nodes.size(); ++i) g.indexById[g.nodes[i].id] = static_cast<int>(i);
    bool first = true;
    auto grow = [&](float x0, float y0, float x1, float y1) {
        if (first) {
            g.minX = x0; g.minY = y0; g.maxX = x1; g.maxY = y1;
            first = false;
        } else {
            g.minX = std::min(g.minX, x0); g.minY = std::min(g.minY, y0);
            g.maxX = std::max(g.maxX, x1); g.maxY = std::max(g.maxY, y1);
        }
    };
    for (const WfNode& n : g.nodes) grow(n.x, n.y - kTitleH, n.x + n.w, n.y + n.h);
    for (const WfGroup& gr : g.groups) grow(gr.x, gr.y, gr.x + gr.w, gr.y + gr.h);
}

// ---- editable graph ("workflow")
// Widget names/values of one editable-graph node: names from the matching API entry when we have it, else the
// known tables.
std::vector<WfWidget> BuildWidgets(const json& jn, const std::string& type, const json* apiNode) {
    std::vector<std::pair<std::string, std::string>> named;
    if (apiNode && apiNode->contains("inputs") && (*apiNode)["inputs"].is_object()) {
        for (auto& [key, val] : (*apiNode)["inputs"].items()) {
            const bool isLink = val.is_array() && val.size() == 2 && !val[0].is_object();
            if (!isLink) named.push_back({key, Scalar(val)});
        }
    } else if (jn.contains("widgets_values")) {
        const json& wv = jn["widgets_values"];
        if (wv.is_object()) {
            for (auto& [key, val] : wv.items()) named.push_back({key, Scalar(val)});
        } else if (wv.is_array()) {
            const auto* names = KnownWidgets(type);
            for (size_t i = 0; i < wv.size(); ++i) {
                std::string name = names && i < names->size() ? (*names)[i] : "value " + std::to_string(i + 1);
                named.push_back({name, Scalar(wv[i])});
            }
        }
    }
    std::vector<WfWidget> out;
    for (auto& [name, value] : named) {
        WfWidget w;
        w.name = name;
        w.value = value;
        w.multiline = IsLongText(value);
        out.push_back(std::move(w));
    }
    return out;
}

using SubgraphMap = std::unordered_map<std::string, const json*>;

bool ParseUiGraph(const json& root, const json* api, const std::string& apiPrefix, const SubgraphMap& subs, bool isRoot, WfGraph& g, std::string* error) {
    if (!root.contains("nodes") || !root["nodes"].is_array()) {
        if (error) *error = "No nodes in this workflow.";
        return false;
    }
    for (const json& jn : root["nodes"]) {
        if (!jn.is_object()) continue;
        WfNode n;
        n.id = jn.value("id", 0);
        n.idText = std::to_string(n.id);
        n.type = jn.value("type", std::string());
        n.title = jn.contains("title") && jn["title"].is_string() ? jn["title"].get<std::string>() : n.type;
        if (auto sub = subs.find(n.type); sub != subs.end()) {
            n.subgraphId = n.type;
            if (!(jn.contains("title") && jn["title"].is_string())) n.title = (*sub->second).value("name", std::string("Subgraph"));
        }
        n.mode = jn.value("mode", 0);
        if (jn.contains("pos")) ReadPair(jn["pos"], n.x, n.y);
        bool sized = jn.contains("size") && ReadPair(jn["size"], n.w, n.h) && n.w > 10 && n.h > 10;
        if (jn.contains("flags") && jn["flags"].is_object()) n.collapsed = jn["flags"].value("collapsed", false);
        if (jn.contains("color") && jn["color"].is_string()) n.color = ParseColor(jn["color"].get<std::string>(), n.hasColor);
        if (jn.contains("bgcolor") && jn["bgcolor"].is_string()) n.bgcolor = ParseColor(jn["bgcolor"].get<std::string>(), n.hasBg);
        if (jn.contains("inputs") && jn["inputs"].is_array()) {
            for (const json& in : jn["inputs"]) {
                WfPort p;
                p.name = in.value("name", std::string());
                if (in.contains("label") && in["label"].is_string()) p.name = in["label"].get<std::string>();
                p.type = in.contains("type") ? Scalar(in["type"]) : std::string();
                if (in.contains("link") && in["link"].is_number_integer()) p.link = in["link"].get<int>();
                n.inputs.push_back(std::move(p));
            }
        }
        if (jn.contains("outputs") && jn["outputs"].is_array()) {
            for (const json& out : jn["outputs"]) {
                WfPort p;
                p.name = out.value("name", std::string());
                if (out.contains("label") && out["label"].is_string()) p.name = out["label"].get<std::string>();
                p.type = out.contains("type") ? Scalar(out["type"]) : std::string();
                n.outputs.push_back(std::move(p));
            }
        }

        const std::string apiKey = apiPrefix + n.idText;
        const json* apiNode = nullptr;
        if (api && api->is_object() && api->contains(apiKey) && (*api)[apiKey].is_object()) apiNode = &(*api)[apiKey];
        n.widgets = BuildWidgets(jn, n.type, apiNode);
        if (!sized) SizeFromContent(n);
        g.nodes.push_back(std::move(n));
    }

    if (root.contains("links") && root["links"].is_array()) {
        for (const json& jl : root["links"]) {
            WfLink l;
            if (jl.is_array() && jl.size() >= 5) {
                l.id = jl[0].is_number_integer() ? jl[0].get<int>() : 0;
                l.from = jl[1].is_number_integer() ? jl[1].get<int>() : 0;
                l.fromSlot = jl[2].is_number_integer() ? jl[2].get<int>() : 0;
                l.to = jl[3].is_number_integer() ? jl[3].get<int>() : 0;
                l.toSlot = jl[4].is_number_integer() ? jl[4].get<int>() : 0;
                if (jl.size() > 5) l.type = Scalar(jl[5]);
            } else if (jl.is_object()) {
                l.id = jl.value("id", 0);
                l.from = jl.value("origin_id", 0);
                l.fromSlot = jl.value("origin_slot", 0);
                l.to = jl.value("target_id", 0);
                l.toSlot = jl.value("target_slot", 0);
                if (jl.contains("type")) l.type = Scalar(jl["type"]);
            } else {
                continue;
            }
            g.links.push_back(std::move(l));
        }
    }
    if (root.contains("groups") && root["groups"].is_array()) {
        for (const json& jg : root["groups"]) {
            if (!jg.is_object()) continue;
            WfGroup gr;
            gr.title = jg.value("title", std::string());
            const json* b = jg.contains("bounding") ? &jg["bounding"] : nullptr;
            if (!b || !b->is_array() || b->size() < 4) continue;
            gr.x = (*b)[0].get<float>(); gr.y = (*b)[1].get<float>();
            gr.w = (*b)[2].get<float>(); gr.h = (*b)[3].get<float>();
            if (jg.contains("color") && jg["color"].is_string()) gr.color = ParseColor(jg["color"].get<std::string>(), gr.hasColor);
            g.groups.push_back(std::move(gr));
        }
    }
    // inside a subgraph: the boxes that stand for its inputs and outputs
    auto ioNode = [&](const char* boxKey, const char* listKey, int id, const char* title, bool asOutputs) {
        if (!root.contains(boxKey) || !root[boxKey].is_object() || !root.contains(listKey) || !root[listKey].is_array()) return;
        WfNode n;
        n.id = id;
        n.idText = std::to_string(id);
        n.type = title;
        n.title = title;
        n.hasColor = n.hasBg = true;
        n.color = 0x2a3a52;
        n.bgcolor = 0x2f3b4d;
        const json& box = root[boxKey];
        float bx = 0, by = 0, bw = 200, bh = 120;
        if (box.contains("bounding") && box["bounding"].is_array() && box["bounding"].size() >= 4) {
            bx = box["bounding"][0].get<float>(); by = box["bounding"][1].get<float>();
            bw = box["bounding"][2].get<float>(); bh = box["bounding"][3].get<float>();
        }
        n.x = bx;
        n.y = by + kTitleH;
        n.w = std::max(140.0f, bw);
        n.h = std::max(60.0f, bh - kTitleH);
        for (const json& p : root[listKey]) {
            WfPort port;
            port.name = p.value("name", std::string());
            port.type = p.contains("type") ? Scalar(p["type"]) : std::string();
            (asOutputs ? n.outputs : n.inputs).push_back(std::move(port));
        }
        g.nodes.push_back(std::move(n));
    };
    ioNode("inputNode", "inputs", -10, "Subgraph inputs", true);
    ioNode("outputNode", "outputs", -20, "Subgraph outputs", false);
    if (isRoot) {
        for (const auto& [id, sub] : subs) {
            if (!sub->contains("nodes") || !(*sub)["nodes"].is_array()) continue;
            for (const json& jn : (*sub)["nodes"]) {
                if (!jn.is_object()) continue;
                ++g.hiddenNodes;
                const std::string type = jn.value("type", std::string());
                if (subs.count(type)) continue; // a nested subgraph node: its own contents are counted on their own
                const std::string title = jn.contains("title") && jn["title"].is_string() ? jn["title"].get<std::string>() : type;
                for (WfWidget& w : BuildWidgets(jn, type, nullptr)) g.deep.push_back({title, std::move(w)});
            }
        }
    }
    FinishGraph(g);
    return true;
}

// ---- API prompt (no layout: nodes are arranged in columns by how far they are from the sources)
bool ParseApiGraph(const json& root, WfGraph& g, std::string* error) {
    if (!root.is_object() || root.empty()) {
        if (error) *error = "Empty workflow.";
        return false;
    }
    std::vector<std::string> keys;
    for (auto& [key, val] : root.items()) {
        if (val.is_object() && val.contains("class_type")) keys.push_back(key);
    }
    if (keys.empty()) {
        if (error) *error = "This is not a ComfyUI workflow.";
        return false;
    }
    auto numeric = [](const std::string& s) { return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); }); };
    std::sort(keys.begin(), keys.end(), [&](const std::string& a, const std::string& b) {
        if (numeric(a) && numeric(b)) return a.size() != b.size() ? a.size() < b.size() : a < b;
        return a < b;
    });
    std::unordered_map<std::string, int> idOf;
    for (size_t i = 0; i < keys.size(); ++i) idOf[keys[i]] = static_cast<int>(i) + 1;

    g.fromApi = true;
    std::vector<std::vector<int>> deps(keys.size());
    int nextLink = 1;
    for (size_t i = 0; i < keys.size(); ++i) {
        const json& jn = root[keys[i]];
        WfNode n;
        n.id = static_cast<int>(i) + 1;
        n.idText = keys[i];
        n.type = jn.value("class_type", std::string());
        n.title = n.type;
        if (jn.contains("_meta") && jn["_meta"].is_object() && jn["_meta"].contains("title") && jn["_meta"]["title"].is_string()) {
            n.title = jn["_meta"]["title"].get<std::string>();
        }
        if (jn.contains("inputs") && jn["inputs"].is_object()) {
            for (auto& [key, val] : jn["inputs"].items()) {
                const bool isLink = val.is_array() && val.size() == 2 && val[0].is_string() && val[1].is_number_integer() && idOf.count(val[0].get<std::string>());
                if (isLink) {
                    WfPort p;
                    p.name = key;
                    p.link = nextLink;
                    WfLink l;
                    l.id = nextLink++;
                    l.from = idOf[val[0].get<std::string>()];
                    l.fromSlot = val[1].get<int>();
                    l.to = n.id;
                    l.toSlot = static_cast<int>(n.inputs.size());
                    deps[i].push_back(l.from - 1);
                    g.links.push_back(l);
                    n.inputs.push_back(std::move(p));
                } else {
                    WfWidget w;
                    w.name = key;
                    w.value = Scalar(val);
                    w.multiline = IsLongText(w.value);
                    n.widgets.push_back(std::move(w));
                }
            }
        }
        g.nodes.push_back(std::move(n));
    }
    // Outputs: the known ones, plus whatever slots other nodes read from.
    for (WfLink& l : g.links) {
        WfNode& src = g.nodes[l.from - 1];
        const auto* known = KnownOutputs(src.type);
        while (static_cast<int>(src.outputs.size()) <= l.fromSlot) {
            WfPort p;
            const size_t idx = src.outputs.size();
            p.type = known && idx < known->size() ? (*known)[idx] : std::string();
            p.name = p.type.empty() ? "out " + std::to_string(idx) : p.type;
            src.outputs.push_back(std::move(p));
        }
        l.type = src.outputs[l.fromSlot].type;
        WfNode& dst = g.nodes[l.to - 1];
        if (l.toSlot < static_cast<int>(dst.inputs.size())) dst.inputs[l.toSlot].type = l.type;
    }
    for (WfNode& n : g.nodes) {
        if (n.outputs.empty()) {
            if (const auto* known = KnownOutputs(n.type)) {
                for (const char* t : *known) n.outputs.push_back({t, t, -1});
            }
        }
        SizeFromContent(n);
    }

    // columns
    std::vector<int> depth(keys.size(), -1);
    std::function<int(size_t, int)> depthOf = [&](size_t i, int guard) -> int {
        if (depth[i] >= 0) return depth[i];
        if (guard > 200) return 0;
        int d = 0;
        for (int dep : deps[i]) d = std::max(d, depthOf(static_cast<size_t>(dep), guard + 1) + 1);
        return depth[i] = d;
    };
    int maxDepth = 0;
    for (size_t i = 0; i < keys.size(); ++i) maxDepth = std::max(maxDepth, depthOf(i, 0));
    std::vector<float> colY(static_cast<size_t>(maxDepth) + 1, 40.0f);
    for (size_t i = 0; i < g.nodes.size(); ++i) {
        WfNode& n = g.nodes[i];
        n.x = depth[i] * 340.0f;
        n.y = colY[depth[i]] + kTitleH;
        colY[depth[i]] = n.y + n.h + 36.0f;
    }
    FinishGraph(g);
    return true;
}

// ---- file helpers
bool ReadFileText(const std::wstring& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return true;
}

bool WriteFileText(const std::wstring& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(f);
}

std::string WideToUtf8Local(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Utf8ToWideLocal(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string SafeName(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (c < 32 || std::strchr("<>:\"/\\|?*", c)) out += '_';
        else out += static_cast<char>(c);
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    if (out.size() > 60) out.resize(60);
    return out.empty() ? "workflow" : out;
}

std::string HashHex(const std::string& text) {
    unsigned long long h = 1469598103934665603ull;
    for (unsigned char c : text) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char buf[20];
    snprintf(buf, sizeof(buf), "%010llx", h & 0xFFFFFFFFFFull);
    return buf;
}

std::wstring LibBase(const std::string& name, const std::string& text) {
    return WorkflowLibraryDir() + L"\\" + Utf8ToWideLocal(SafeName(name) + "_" + HashHex(text));
}

} // namespace

// ---------------------------------------------------------------------------
// finding workflows
// ---------------------------------------------------------------------------
const char* WorkflowKindLabel(WorkflowKind kind) {
    switch (kind) {
        case WorkflowKind::ComfyUI: return "ComfyUI workflow";
        case WorkflowKind::ComfyApi: return "ComfyUI API prompt";
        case WorkflowKind::InvokeAI: return "InvokeAI metadata";
        case WorkflowKind::NovelAI: return "NovelAI parameters";
        case WorkflowKind::Parameters: return "Generation parameters";
    }
    return "";
}

std::vector<EmbeddedWorkflow> FindWorkflows(const ImageMetadata& meta) {
    std::vector<EmbeddedWorkflow> out;
    auto add = [&](WorkflowKind kind, const char* label, const std::string* text) {
        for (const EmbeddedWorkflow& e : out) {
            if (e.text == text) return;
        }
        out.push_back({kind, label, text});
    };
    for (const RawField& f : meta.rawFields) {
        const std::string key = Lower(f.key);
        if (key == "workflow" || key == "prompt") {
            if (!LooksLikeJsonObject(f.value)) continue;
            if (key == "workflow" && f.value.find("\"nodes\"") != std::string::npos) add(WorkflowKind::ComfyUI, "ComfyUI workflow", &f.value);
            else if (f.value.find("class_type") != std::string::npos) add(WorkflowKind::ComfyApi, "ComfyUI API prompt", &f.value);
        } else if (key == "invokeai_metadata" || key == "invokeai_graph" || key == "sd-metadata") {
            add(WorkflowKind::InvokeAI, "InvokeAI metadata", &f.value);
        }
    }
    if (meta.generator == Generator::NovelAI) {
        for (const RawField& f : meta.rawFields) {
            if (Lower(f.key) == "comment") add(WorkflowKind::NovelAI, "NovelAI parameters", &f.value);
        }
    }
    if (meta.generator == Generator::Automatic1111) {
        for (const RawField& f : meta.rawFields) {
            const std::string key = Lower(f.key);
            if (key == "parameters" || key == "exif:usercomment" || key == "exif:imagedescription") {
                add(WorkflowKind::Parameters, "Generation parameters", &f.value);
                break;
            }
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const EmbeddedWorkflow& a, const EmbeddedWorkflow& b) { return a.kind < b.kind; });
    return out;
}

const EmbeddedWorkflow* FindViewableWorkflow(const std::vector<EmbeddedWorkflow>& found) {
    for (const EmbeddedWorkflow& e : found) {
        if (e.Viewable()) return &e;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// graph parsing
// ---------------------------------------------------------------------------
bool ParseWorkflowGraph(const std::string& text, const std::string* apiPrompt, WfGraph& out, std::string* error, const WfSubPath* path) {
    out.Clear();
    json root = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!root.is_object()) {
        if (error) *error = "This is not valid JSON.";
        return false;
    }
    if (root.contains("nodes") && root["nodes"].is_array()) {
        json api;
        if (apiPrompt && !apiPrompt->empty()) api = json::parse(*apiPrompt, nullptr, false);
        SubgraphMap subs;
        if (root.contains("definitions") && root["definitions"].is_object() && root["definitions"].contains("subgraphs") &&
            root["definitions"]["subgraphs"].is_array()) {
            for (const json& sg : root["definitions"]["subgraphs"]) {
                if (sg.is_object() && sg.contains("id") && sg["id"].is_string()) subs[sg["id"].get<std::string>()] = &sg;
            }
        }
        const json* graphRoot = &root;
        std::string prefix, name;
        if (path) {
            for (const auto& [subId, nodeId] : *path) {
                auto it = subs.find(subId);
                if (it == subs.end()) {
                    if (error) *error = "That subgraph is missing from the file.";
                    return false;
                }
                graphRoot = it->second;
                name = graphRoot->value("name", std::string("Subgraph"));
                prefix += std::to_string(nodeId) + ":";
            }
        }
        const bool ok = ParseUiGraph(*graphRoot, api.is_object() ? &api : nullptr, prefix, subs, graphRoot == &root, out, error);
        out.name = name;
        return ok;
    }
    return ParseApiGraph(root, out, error);
}

std::string SummarizeWorkflow(const WfGraph& g) {
    std::string model;
    int loras = 0;
    for (const WfNode& n : g.nodes) {
        if (n.mode == 2 || n.mode == 4) continue;
        for (const WfWidget& w : n.widgets) {
            if ((w.name == "ckpt_name" || w.name == "unet_name") && model.empty()) model = w.value;
            else if (w.name == "lora_name" && !w.value.empty() && w.value != "None") ++loras;
        }
    }
    for (const auto& [title, w] : g.deep) {
        if ((w.name == "ckpt_name" || w.name == "unet_name") && model.empty()) model = w.value;
        else if (w.name == "lora_name" && !w.value.empty() && w.value != "None") ++loras;
    }
    auto shorten = [](std::string s) {
        size_t slash = s.find_last_of("\\/");
        if (slash != std::string::npos) s = s.substr(slash + 1);
        return s;
    };
    std::string s = std::to_string(g.nodes.size() + g.hiddenNodes) + " nodes";
    if (!model.empty()) s += "  -  " + shorten(model);
    if (loras) s += "  -  " + std::to_string(loras) + (loras == 1 ? " LoRA" : " LoRAs");
    return s;
}

// ---------------------------------------------------------------------------
// reading from files
// ---------------------------------------------------------------------------
bool ReadWorkflowFromFile(const std::wstring& path, std::string& textOut, std::string& apiOut, std::string* error) {
    textOut.clear();
    apiOut.clear();
    const bool isJson = path.size() > 5 && _wcsicmp(path.c_str() + path.size() - 5, L".json") == 0;
    if (isJson) {
        if (!ReadFileText(path, textOut)) {
            if (error) *error = "Could not read the file.";
            return false;
        }
        const std::wstring sidecar = path.substr(0, path.size() - 5) + L".api.json";
        ReadFileText(sidecar, apiOut);
        return true;
    }
    ImageMetadata meta = ReadImageMetadata(path);
    std::vector<EmbeddedWorkflow> found = FindWorkflows(meta);
    for (const EmbeddedWorkflow& e : found) {
        if (e.kind == WorkflowKind::ComfyUI) textOut = *e.text;
        else if (e.kind == WorkflowKind::ComfyApi) apiOut = *e.text;
    }
    if (textOut.empty()) {
        textOut = apiOut;
        apiOut.clear();
    }
    if (textOut.empty()) {
        if (error) *error = "This image has no ComfyUI workflow in it.";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// library
// ---------------------------------------------------------------------------
std::wstring WorkflowLibraryDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring base = n ? std::wstring(buf, n) : std::wstring(L".");
    return base + L"\\Try2Create\\workflows";
}

std::wstring SaveWorkflowTextToLibrary(const std::string& name, const std::string& text, bool* existed, std::string* error) {
    if (existed) *existed = false;
    CreateDirectoryW((WorkflowLibraryDir().substr(0, WorkflowLibraryDir().find_last_of(L'\\'))).c_str(), nullptr);
    CreateDirectoryW(WorkflowLibraryDir().c_str(), nullptr);
    WfGraph g;
    std::string perr;
    if (!ParseWorkflowGraph(text, nullptr, g, &perr)) {
        if (error) *error = perr;
        return {};
    }
    const std::wstring base = LibBase(name, text);
    const std::wstring jsonPath = base + L".json";
    if (GetFileAttributesW(jsonPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (existed) *existed = true;
        return jsonPath;
    }
    if (!WriteFileText(jsonPath, text)) {
        if (error) *error = "Could not write to the workflow folder.";
        return {};
    }
    json meta = {{"name", name}, {"kind", g.fromApi ? "ComfyUI API prompt" : "ComfyUI workflow"}, {"source", ""},
                 {"nodes", g.nodes.size()}, {"summary", SummarizeWorkflow(g)}, {"saved", static_cast<long long>(std::time(nullptr))},
                 {"file", WideToUtf8Local(jsonPath.substr(jsonPath.find_last_of(L'\\') + 1))}};
    WriteFileText(base + L".meta.json", meta.dump(2));
    return jsonPath;
}

std::wstring SaveWorkflowToLibrary(ID3D11Device* device, const std::wstring& imagePath, const ImageMetadata& imageMeta,
                                   bool* existed, std::string* error) {
    if (existed) *existed = false;
    std::vector<EmbeddedWorkflow> found = FindWorkflows(imageMeta);
    const EmbeddedWorkflow* wf = FindViewableWorkflow(found);
    if (!wf) {
        if (error) *error = "This image has no ComfyUI workflow in it.";
        return {};
    }
    const std::string& text = *wf->text;
    const std::string* api = nullptr;
    for (const EmbeddedWorkflow& e : found) {
        if (e.kind == WorkflowKind::ComfyApi && wf->kind == WorkflowKind::ComfyUI) api = e.text;
    }
    WfGraph g;
    std::string perr;
    if (!ParseWorkflowGraph(text, api, g, &perr)) {
        if (error) *error = perr;
        return {};
    }
    CreateDirectoryW((WorkflowLibraryDir().substr(0, WorkflowLibraryDir().find_last_of(L'\\'))).c_str(), nullptr);
    CreateDirectoryW(WorkflowLibraryDir().c_str(), nullptr);

    std::wstring stem = imagePath.substr(imagePath.find_last_of(L"\\/") + 1);
    const size_t dot = stem.find_last_of(L'.');
    if (dot != std::wstring::npos) stem.resize(dot);
    const std::string name = WideToUtf8Local(stem);
    const std::wstring base = LibBase(name, text);
    const std::wstring jsonPath = base + L".json";
    const bool already = GetFileAttributesW(jsonPath.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (existed) *existed = already;
    if (!already) {
        if (!WriteFileText(jsonPath, text)) {
            if (error) *error = "Could not write to the workflow folder.";
            return {};
        }
        if (api) WriteFileText(base + L".api.json", *api);
        json meta = {{"name", name}, {"kind", wf->label}, {"source", WideToUtf8Local(imagePath)}, {"nodes", g.nodes.size()},
                     {"summary", SummarizeWorkflow(g)}, {"saved", static_cast<long long>(std::time(nullptr))},
                     {"file", WideToUtf8Local(jsonPath.substr(jsonPath.find_last_of(L'\\') + 1))}};
        WriteFileText(base + L".meta.json", meta.dump(2));
    }
    const std::wstring thumb = base + L".png";
    if (device && GetFileAttributesW(thumb.c_str()) == INVALID_FILE_ATTRIBUTES) {
        LoadedImage img;
        std::vector<unsigned char> rgba;
        std::string terr;
        try {
            if (LoadImageThumbnail(device, imagePath, 192, img, &rgba, &terr)) {
                WriteThumbnailPng(thumb, rgba.data(), img.width, img.height);
                ReleaseImage(img);
            }
        } catch (...) {
        }
    }
    return jsonPath;
}

std::vector<WorkflowLibItem> ScanWorkflowLibrary() {
    std::vector<WorkflowLibItem> items;
    const std::wstring dir = WorkflowLibraryDir();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*.meta.json").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return items;
    do {
        std::string text;
        if (!ReadFileText(dir + L"\\" + fd.cFileName, text)) continue;
        json j = json::parse(text, nullptr, false);
        if (!j.is_object()) continue;
        WorkflowLibItem it;
        const std::wstring metaName = fd.cFileName;
        const std::wstring stem = metaName.substr(0, metaName.size() - 10); // ".meta.json"
        it.jsonPath = dir + L"\\" + stem + L".json";
        if (GetFileAttributesW(it.jsonPath.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        it.thumbPath = dir + L"\\" + stem + L".png";
        if (GetFileAttributesW(it.thumbPath.c_str()) == INVALID_FILE_ATTRIBUTES) it.thumbPath.clear();
        it.name = j.value("name", WideToUtf8Local(stem));
        it.kindLabel = j.value("kind", std::string("ComfyUI workflow"));
        it.summary = j.value("summary", std::string());
        it.source = j.value("source", std::string());
        it.nodes = j.value("nodes", 0);
        it.saved = j.value("saved", 0LL);
        items.push_back(std::move(it));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(items.begin(), items.end(), [](const WorkflowLibItem& a, const WorkflowLibItem& b) { return a.saved > b.saved; });
    return items;
}

bool DeleteWorkflowFromLibrary(const WorkflowLibItem& item) {
    const std::wstring base = item.jsonPath.substr(0, item.jsonPath.size() - 5);
    bool ok = DeleteFileW(item.jsonPath.c_str()) != 0;
    DeleteFileW((base + L".meta.json").c_str());
    DeleteFileW((base + L".api.json").c_str());
    DeleteFileW((base + L".png").c_str());
    return ok;
}

// ---------------------------------------------------------------------------
// batch extraction
// ---------------------------------------------------------------------------
void WorkflowBatch::Start(ID3D11Device* device, std::vector<std::wstring> images) {
    Stop();
    done_ = saved_ = existed_ = none_ = 0;
    total_ = static_cast<int>(images.size());
    finished_ = false;
    cancel_ = false;
    running_ = true;
    thread_ = std::thread([this, device, images = std::move(images)]() {
        for (const std::wstring& path : images) {
            if (cancel_.load()) break;
            try {
                ImageMetadata meta = ReadImageMetadata(path);
                bool existed = false;
                std::string err;
                std::wstring saved = SaveWorkflowToLibrary(device, path, meta, &existed, &err);
                if (saved.empty()) none_++;
                else if (existed) existed_++;
                else saved_++;
            } catch (...) {
                none_++;
            }
            done_++;
        }
        running_ = false;
        finished_ = true;
    });
}

void WorkflowBatch::Stop() {
    cancel_ = true;
    if (thread_.joinable()) thread_.join();
    cancel_ = false;
    running_ = false;
}

bool WorkflowBatch::TakeResult(std::string& message) {
    if (!finished_.load()) return false;
    if (thread_.joinable()) thread_.join();
    finished_ = false;
    message = "Saved " + std::to_string(saved_.load()) + " workflow" + (saved_.load() == 1 ? "" : "s") + " to the library";
    if (existed_.load()) message += ", " + std::to_string(existed_.load()) + " already there";
    if (none_.load()) message += ", " + std::to_string(none_.load()) + " image" + (none_.load() == 1 ? "" : "s") + " without a ComfyUI workflow";
    message += ".";
    return true;
}
