#include "metadata_parsers.h"
#include <algorithm>
#include <cctype>
#include <functional>
#include <sstream>
#include <unordered_map>
#include "json.hpp"

using json = nlohmann::json;

namespace {

std::string Trim(const std::string& s) {
    size_t start = 0, end = s.size();
    while (start < end && std::isspace(static_cast<unsigned char>(s[start]))) start++;
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) end--;
    return s.substr(start, end - start);
}

std::string ToLower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return std::tolower(c); });
    return r;
}

const std::string* FindField(const std::vector<RawField>& fields, const char* key) {
    std::string lowerKey = ToLower(key);
    for (const auto& f : fields) {
        if (ToLower(f.key) == lowerKey) return &f.value;
    }
    return nullptr;
}

// Splits an A1111-style settings line ("Steps: 20, Sampler: Euler a, ...")
// into key/value pairs, respecting commas nested inside quotes/parens.
KeyValueList ParseSettingsLine(const std::string& line) {
    KeyValueList out;
    std::vector<std::string> segments;
    std::string current;
    int depth = 0;
    bool inQuotes = false;
    for (char c : line) {
        if (c == '"') inQuotes = !inQuotes;
        if (!inQuotes) {
            if (c == '(' || c == '[' || c == '{') depth++;
            else if (c == ')' || c == ']' || c == '}') { if (depth > 0) depth--; }
        }
        if (c == ',' && depth == 0 && !inQuotes) {
            segments.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    if (!Trim(current).empty()) segments.push_back(current);

    for (auto& seg : segments) {
        size_t colon = seg.find(": ");
        if (colon == std::string::npos) continue;
        std::string key = Trim(seg.substr(0, colon));
        std::string value = Trim(seg.substr(colon + 2));
        if (!value.empty() && value.front() == '"' && value.back() == '"' && value.size() >= 2) {
            value = value.substr(1, value.size() - 2);
        }
        if (!key.empty()) out.push_back({key, value});
    }
    return out;
}

// Parses the classic AUTOMATIC1111 "parameters" text block. Also matches
// forks/derivatives (Forge, reForge, SD.Next) that emit the same format.
bool ParseA1111Style(const std::string& rawIn, ParsedFields& out) {
    std::string text = rawIn;
    // Normalize CRLF -> LF for consistent line splitting.
    size_t p;
    while ((p = text.find("\r\n")) != std::string::npos) text.replace(p, 2, "\n");

    size_t lastNl = text.find_last_of('\n');
    std::string lastLine = lastNl == std::string::npos ? text : text.substr(lastNl + 1);
    bool hasSettingsLine = lastLine.find("Steps:") != std::string::npos;
    size_t settingsStart = hasSettingsLine ? (lastNl == std::string::npos ? 0 : lastNl + 1) : text.size();

    const std::string negMarker = "\nNegative prompt: ";
    size_t negIdx = text.find(negMarker);
    if (negIdx == std::string::npos && text.rfind("Negative prompt: ", 0) == 0) negIdx = 0; // starts at position 0 edge case

    if (negIdx != std::string::npos) {
        out.positivePrompt = Trim(text.substr(0, negIdx));
        size_t negTextStart = (negIdx == 0) ? std::string("Negative prompt: ").size()
                                             : negIdx + negMarker.size();
        size_t negTextEnd = hasSettingsLine && settingsStart > negTextStart ? settingsStart - 1 : text.size();
        if (negTextEnd < negTextStart) negTextEnd = negTextStart;
        out.negativePrompt = Trim(text.substr(negTextStart, negTextEnd - negTextStart));
    } else {
        size_t posEnd = hasSettingsLine && settingsStart > 0 ? settingsStart - 1 : text.size();
        out.positivePrompt = Trim(text.substr(0, posEnd));
        out.negativePrompt.clear();
    }

    if (hasSettingsLine) {
        out.params = ParseSettingsLine(Trim(lastLine));
    }

    return hasSettingsLine || !out.positivePrompt.empty();
}

std::string JsonScalarToString(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_null()) return "";
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number_float()) {
        std::ostringstream oss;
        oss << v.get<double>();
        return oss.str();
    }
    return v.dump();
}

bool IsLinkRef(const json& v) { return v.is_array() && v.size() == 2 && v[0].is_string(); }

// The text a node produces: a text-encode node's prompt, followed through the primitives, switches and
// prompt enhancers that can sit in front of it.
std::string GetComfyNodeText(const json& nodes, const std::string& nodeId, int depth = 0) {
    if (depth > 12) return "";
    auto it = nodes.find(nodeId);
    if (it == nodes.end() || !it->is_object() || !it->contains("inputs") || !(*it)["inputs"].is_object()) return "";
    const json& node = *it;
    const std::string classType = node.value("class_type", "");
    const json& inputs = node["inputs"];

    // a literal string, or whatever the input is linked to
    auto value = [&](const char* key) -> std::string {
        if (!inputs.contains(key)) return "";
        const json& v = inputs[key];
        if (v.is_string()) return v.get<std::string>();
        if (IsLinkRef(v)) return GetComfyNodeText(nodes, v[0].get<std::string>(), depth + 1);
        return "";
    };

    if (classType.find("CLIPTextEncode") != std::string::npos) {
        std::string t = value("text");
        if (!t.empty()) return t;
        std::string combined = value("text_g");
        std::string l = value("text_l");
        if (!l.empty()) combined += (combined.empty() ? "" : "\n") + l;
        return combined;
    }
    if (classType.find("Switch") != std::string::npos) {
        bool known = false, on = false;
        if (inputs.contains("switch")) {
            const json& sw = inputs["switch"];
            if (sw.is_boolean()) { known = true; on = sw.get<bool>(); }
            else if (IsLinkRef(sw)) {
                auto sit = nodes.find(sw[0].get<std::string>());
                if (sit != nodes.end() && sit->is_object() && sit->contains("inputs") && (*sit)["inputs"].contains("value") && (*sit)["inputs"]["value"].is_boolean()) {
                    known = true;
                    on = (*sit)["inputs"]["value"].get<bool>();
                }
            }
        }
        if (known) return value(on ? "on_true" : "on_false");
        std::string t = value("on_true");
        return t.empty() ? value("on_false") : t;
    }
    if (classType.find("TextGenerate") != std::string::npos || classType.find("Enhance") != std::string::npos) {
        return value("prompt"); // the written prompt that goes in (the enhanced text is only known to the run)
    }
    if (classType.find("String") != std::string::npos || classType.find("Text") != std::string::npos || classType.find("Primitive") != std::string::npos ||
        classType.find("Prompt") != std::string::npos) {
        for (const char* key : {"value", "text", "string", "prompt"}) {
            std::string t = value(key);
            if (!t.empty()) return t;
        }
    }
    return "";
}

// positive/negative conditioning: follow it back through conditioning nodes to the text behind it
std::string ResolveConditioning(const json& nodes, const std::string& nodeId, const char* which, int depth = 0) {
    if (depth > 12) return "";
    auto it = nodes.find(nodeId);
    if (it == nodes.end() || !it->is_object() || !it->contains("inputs") || !(*it)["inputs"].is_object()) return "";
    const json& inputs = (*it)["inputs"];
    // ConditioningZeroOut is an empty negative made from the positive (Krea 2, Flux, Qwen at CFG 1): no text, not the positive's again
    if ((*it).value("class_type", "").find("ZeroOut") != std::string::npos) return "";
    std::string t = GetComfyNodeText(nodes, nodeId);
    if (!t.empty()) return t;
    for (const char* key : {which, "conditioning", "cond"}) {
        if (inputs.contains(key) && IsLinkRef(inputs[key])) {
            t = ResolveConditioning(nodes, inputs[key][0].get<std::string>(), which, depth + 1);
            if (!t.empty()) return t;
        }
    }
    return "";
}

void ParseComfyUI(const json& root, ParsedFields& out) {
    if (!root.is_object()) return;

    // The prompts: whichever node takes both a positive and a negative conditioning (sampler, guider,
    // conditioning node) tells which text is which.
    for (auto& [id, node] : root.items()) {
        if (!node.is_object() || !node.contains("inputs") || !node["inputs"].is_object()) continue;
        const json& inputs = node["inputs"];
        if (!inputs.contains("positive") || !IsLinkRef(inputs["positive"])) continue;
        const std::string pos = ResolveConditioning(root, inputs["positive"][0].get<std::string>(), "positive");
        const std::string neg = inputs.contains("negative") && IsLinkRef(inputs["negative"])
                                    ? ResolveConditioning(root, inputs["negative"][0].get<std::string>(), "negative")
                                    : std::string();
        if (pos.empty() && neg.empty()) continue;
        out.positivePrompt = pos;
        out.negativePrompt = neg;
        break;
    }

    // Settings of the first real sampler (KSampler and friends)...
    for (auto& [id, node] : root.items()) {
        if (!node.is_object() || !node.contains("inputs") || !node["inputs"].is_object()) continue;
        std::string classType = node.value("class_type", "");
        if (classType.find("Sampler") == std::string::npos) continue;
        const json& inputs = node["inputs"];
        if (!inputs.contains("steps") && !inputs.contains("cfg")) continue;

        if (inputs.contains("steps")) out.params.push_back({"Steps", JsonScalarToString(inputs["steps"])});
        if (inputs.contains("cfg")) out.params.push_back({"CFG scale", JsonScalarToString(inputs["cfg"])});
        if (inputs.contains("sampler_name")) out.params.push_back({"Sampler", JsonScalarToString(inputs["sampler_name"])});
        if (inputs.contains("scheduler")) out.params.push_back({"Scheduler", JsonScalarToString(inputs["scheduler"])});
        if (inputs.contains("seed")) out.params.push_back({"Seed", JsonScalarToString(inputs["seed"])});
        else if (inputs.contains("noise_seed")) out.params.push_back({"Seed", JsonScalarToString(inputs["noise_seed"])});
        if (inputs.contains("denoise")) out.params.push_back({"Denoise", JsonScalarToString(inputs["denoise"])});
        break;
    }
    // ...and for graphs built from separate sampler / noise / scheduler / guider nodes (video models)
    auto have = [&](const char* key) {
        for (const auto& kv : out.params) if (kv.first == key) return true;
        return false;
    };
    for (auto& [id, node] : root.items()) {
        if (!node.is_object() || !node.contains("inputs") || !node["inputs"].is_object()) continue;
        const std::string classType = node.value("class_type", "");
        const json& inputs = node["inputs"];
        if (classType == "KSamplerSelect" && inputs.contains("sampler_name") && !have("Sampler")) out.params.push_back({"Sampler", JsonScalarToString(inputs["sampler_name"])});
        else if (classType.find("RandomNoise") != std::string::npos && inputs.contains("noise_seed") && !have("Seed")) out.params.push_back({"Seed", JsonScalarToString(inputs["noise_seed"])});
        else if (classType.find("Scheduler") != std::string::npos && inputs.contains("steps") && !IsLinkRef(inputs["steps"]) && !have("Steps")) out.params.push_back({"Steps", JsonScalarToString(inputs["steps"])});
        else if (classType.find("Guider") != std::string::npos && !have("CFG scale")) {
            for (const char* key : {"cfg", "video_cfg"}) {
                if (inputs.contains(key) && !IsLinkRef(inputs[key])) { out.params.push_back({"CFG scale", JsonScalarToString(inputs[key])}); break; }
            }
        }
    }

    if (out.positivePrompt.empty()) {
        std::vector<std::string> texts;
        for (auto& [id, node] : root.items()) {
            std::string t = GetComfyNodeText(root, id);
            if (!t.empty()) texts.push_back(t);
        }
        if (texts.size() >= 1) out.positivePrompt = texts[0];
        if (texts.size() >= 2) out.negativePrompt = texts[1];
    }

    for (auto& [id, node] : root.items()) {
        if (!node.is_object() || !node.contains("inputs")) continue;
        std::string classType = node.value("class_type", "");
        const json& inputs = node["inputs"];
        if (classType.find("CheckpointLoader") != std::string::npos && inputs.contains("ckpt_name")) {
            out.params.push_back({"Model", JsonScalarToString(inputs["ckpt_name"])});
        }
        if (classType == "EmptyLatentImage" && inputs.contains("width") && inputs.contains("height")) {
            out.params.push_back({"Size", JsonScalarToString(inputs["width"]) + "x" + JsonScalarToString(inputs["height"])});
        }
    }
}

// A ComfyUI workflow in its editable (node graph) form, for files that don't also carry the API prompt:
// follows the sampler's "positive"/"negative" inputs back to the text nodes.
void ParseComfyWorkflowUi(const json& root, ParsedFields& out) {
    if (!root.is_object() || !root.contains("nodes") || !root["nodes"].is_array()) return;
    std::unordered_map<int, const json*> nodes;
    for (const json& n : root["nodes"]) {
        if (n.is_object() && n.contains("id") && n["id"].is_number_integer()) nodes[n["id"].get<int>()] = &n;
    }
    std::unordered_map<int, int> linkOrigin; // link id -> origin node id
    if (root.contains("links") && root["links"].is_array()) {
        for (const json& l : root["links"]) {
            if (l.is_array() && l.size() >= 5 && l[0].is_number_integer() && l[1].is_number_integer()) linkOrigin[l[0].get<int>()] = l[1].get<int>();
            else if (l.is_object() && l.contains("id") && l.contains("origin_id")) linkOrigin[l["id"].get<int>()] = l["origin_id"].get<int>();
        }
    }
    auto isTextNode = [](const std::string& type) {
        return type.find("TextEncode") != std::string::npos || type.find("Prompt") != std::string::npos ||
               type.find("Text") != std::string::npos;
    };
    std::function<std::string(int, int)> textOf = [&](int id, int hops) -> std::string {
        auto it = nodes.find(id);
        if (it == nodes.end() || hops > 8) return "";
        const json& n = *it->second;
        const std::string type = n.value("type", "");
        if (type.find("ZeroOut") != std::string::npos) return ""; // (an empty negative made from the positive)
        if (isTextNode(type) && n.contains("widgets_values") && n["widgets_values"].is_array()) {
            for (const json& v : n["widgets_values"]) {
                if (v.is_string() && !v.get<std::string>().empty()) return v.get<std::string>();
            }
        }
        if (n.contains("inputs") && n["inputs"].is_array()) {
            for (const json& in : n["inputs"]) {
                if (!in.is_object() || !in.contains("link") || !in["link"].is_number_integer()) continue;
                auto lo = linkOrigin.find(in["link"].get<int>());
                if (lo == linkOrigin.end()) continue;
                const std::string name = in.value("name", "");
                if (name == "clip" || name == "model" || name == "vae" || name == "latent_image") continue;
                std::string t = textOf(lo->second, hops + 1);
                if (!t.empty()) return t;
            }
        }
        return "";
    };
    auto inputText = [&](const json& n, const char* inputName) -> std::string {
        if (!n.contains("inputs") || !n["inputs"].is_array()) return "";
        for (const json& in : n["inputs"]) {
            if (in.is_object() && in.value("name", "") == inputName && in.contains("link") && in["link"].is_number_integer()) {
                auto lo = linkOrigin.find(in["link"].get<int>());
                if (lo != linkOrigin.end()) return textOf(lo->second, 0);
            }
        }
        return "";
    };
    auto widget = [&](const json& n, size_t idx) -> std::string {
        if (n.contains("widgets_values") && n["widgets_values"].is_array() && idx < n["widgets_values"].size()) return JsonScalarToString(n["widgets_values"][idx]);
        return "";
    };

    for (const json& n : root["nodes"]) {
        if (!n.is_object() || n.value("mode", 0) != 0) continue;
        std::string pos = inputText(n, "positive"), neg = inputText(n, "negative");
        if (pos.empty() && neg.empty()) continue;
        out.positivePrompt = pos;
        out.negativePrompt = neg;
        const std::string type = n.value("type", "");
        if (type == "KSampler") {
            out.params.push_back({"Seed", widget(n, 0)});
            out.params.push_back({"Steps", widget(n, 2)});
            out.params.push_back({"CFG scale", widget(n, 3)});
            out.params.push_back({"Sampler", widget(n, 4)});
            out.params.push_back({"Scheduler", widget(n, 5)});
            out.params.push_back({"Denoise", widget(n, 6)});
        } else if (type == "KSamplerAdvanced") {
            out.params.push_back({"Seed", widget(n, 1)});
            out.params.push_back({"Steps", widget(n, 3)});
            out.params.push_back({"CFG scale", widget(n, 4)});
            out.params.push_back({"Sampler", widget(n, 5)});
            out.params.push_back({"Scheduler", widget(n, 6)});
        }
        break;
    }
    if (out.positivePrompt.empty()) {
        std::vector<std::string> texts;
        for (const json& n : root["nodes"]) {
            if (!n.is_object() || n.value("mode", 0) != 0 || !isTextNode(n.value("type", ""))) continue;
            if (n.contains("widgets_values") && n["widgets_values"].is_array() && !n["widgets_values"].empty() && n["widgets_values"][0].is_string()) {
                const std::string t = n["widgets_values"][0].get<std::string>();
                if (!t.empty()) texts.push_back(t);
            }
        }
        if (!texts.empty()) out.positivePrompt = texts[0];
        if (texts.size() > 1) out.negativePrompt = texts[1];
    }
    for (const json& n : root["nodes"]) {
        if (!n.is_object()) continue;
        const std::string type = n.value("type", "");
        if (type.find("CheckpointLoader") != std::string::npos || type == "UNETLoader" || type == "UnetLoaderGGUF") {
            const std::string name = widget(n, type.find("CheckpointLoader") != std::string::npos && type != "CheckpointLoaderSimple" ? 1 : 0);
            if (!name.empty()) {
                out.params.push_back({"Model", name});
                break;
            }
        }
    }
}

void ParseNovelAI(const json& c, ParsedFields& out) {
    out.positivePrompt = c.value("prompt", "");
    out.negativePrompt = c.value("uc", "");
    if (c.contains("steps")) out.params.push_back({"Steps", JsonScalarToString(c["steps"])});
    if (c.contains("sampler")) out.params.push_back({"Sampler", JsonScalarToString(c["sampler"])});
    if (c.contains("scale")) out.params.push_back({"CFG scale", JsonScalarToString(c["scale"])});
    if (c.contains("seed")) out.params.push_back({"Seed", JsonScalarToString(c["seed"])});
    if (c.contains("strength")) out.params.push_back({"Strength", JsonScalarToString(c["strength"])});
    if (c.contains("noise")) out.params.push_back({"Noise", JsonScalarToString(c["noise"])});
    if (c.contains("width") && c.contains("height")) {
        out.params.push_back({"Size", JsonScalarToString(c["width"]) + "x" + JsonScalarToString(c["height"])});
    }
}

void ParseInvokeAINew(const json& j, ParsedFields& out) {
    out.positivePrompt = j.value("positive_prompt", "");
    out.negativePrompt = j.value("negative_prompt", "");
    if (j.contains("steps")) out.params.push_back({"Steps", JsonScalarToString(j["steps"])});
    if (j.contains("cfg_scale")) out.params.push_back({"CFG scale", JsonScalarToString(j["cfg_scale"])});
    if (j.contains("seed")) out.params.push_back({"Seed", JsonScalarToString(j["seed"])});
    if (j.contains("scheduler")) out.params.push_back({"Sampler", JsonScalarToString(j["scheduler"])});
    if (j.contains("width") && j.contains("height")) {
        out.params.push_back({"Size", JsonScalarToString(j["width"]) + "x" + JsonScalarToString(j["height"])});
    }
    if (j.contains("model") && j["model"].is_object() && j["model"].contains("model_name")) {
        out.params.push_back({"Model", JsonScalarToString(j["model"]["model_name"])});
    }
}

void ParseInvokeAILegacy(const json& j, ParsedFields& out) {
    if (!j.contains("image") || !j["image"].is_object()) return;
    const json& img = j["image"];
    if (img.contains("prompt")) {
        if (img["prompt"].is_array()) {
            std::string combined;
            for (auto& p : img["prompt"]) {
                if (p.is_object() && p.contains("prompt")) {
                    if (!combined.empty()) combined += " ";
                    combined += JsonScalarToString(p["prompt"]);
                }
            }
            out.positivePrompt = combined;
        } else if (img["prompt"].is_string()) {
            out.positivePrompt = img["prompt"].get<std::string>();
        }
    }
    if (img.contains("steps")) out.params.push_back({"Steps", JsonScalarToString(img["steps"])});
    if (img.contains("cfg_scale")) out.params.push_back({"CFG scale", JsonScalarToString(img["cfg_scale"])});
    if (img.contains("seed")) out.params.push_back({"Seed", JsonScalarToString(img["seed"])});
    if (img.contains("sampler")) out.params.push_back({"Sampler", JsonScalarToString(img["sampler"])});
    if (img.contains("width") && img.contains("height")) {
        out.params.push_back({"Size", JsonScalarToString(img["width"]) + "x" + JsonScalarToString(img["height"])});
    }
}

void ParseDreamString(const std::string& dream, ParsedFields& out) {
    size_t q1 = dream.find('"');
    size_t q2 = q1 == std::string::npos ? std::string::npos : dream.find('"', q1 + 1);
    std::string rest = dream;
    if (q1 != std::string::npos && q2 != std::string::npos) {
        out.positivePrompt = dream.substr(q1 + 1, q2 - q1 - 1);
        rest = dream.substr(q2 + 1);
    }
    std::istringstream iss(rest);
    std::string tok;
    int width = 0, height = 0;
    while (iss >> tok) {
        if (tok.size() < 2 || tok[0] != '-') continue;
        char flag = tok[1];
        std::string value = tok.substr(2);
        if (value.empty() && iss.peek() != EOF) { iss >> value; }
        switch (flag) {
            case 's': out.params.push_back({"Steps", value}); break;
            case 'S': out.params.push_back({"Seed", value}); break;
            case 'C': out.params.push_back({"CFG scale", value}); break;
            case 'A': out.params.push_back({"Sampler", value}); break;
            case 'W': width = atoi(value.c_str()); break;
            case 'H': height = atoi(value.c_str()); break;
            default: break;
        }
    }
    if (width > 0 && height > 0) {
        out.params.push_back({"Size", std::to_string(width) + "x" + std::to_string(height)});
    }
}

} // namespace

std::vector<std::string> SplitPromptTags(const std::string& prompt) {
    std::vector<std::string> result;
    std::string current;
    int depth = 0;
    auto flush = [&]() {
        std::string t = Trim(current);
        if (!t.empty()) result.push_back(t);
        current.clear();
    };
    for (char c : prompt) {
        if (c == '(' || c == '[' || c == '{') { depth++; current += c; }
        else if (c == ')' || c == ']' || c == '}') { if (depth > 0) depth--; current += c; }
        else if ((c == ',' || c == '\n') && depth == 0) flush();
        else current += c;
    }
    flush();
    return result;
}

void InterpretRawFields(ImageMetadata& meta) {
    meta.hasMetadata = !meta.rawFields.empty();
    if (!meta.hasMetadata) return;

    // 1. InvokeAI (new JSON schema)
    if (const std::string* v = FindField(meta.rawFields, "invokeai_metadata")) {
        try {
            json j = json::parse(*v);
            ParseInvokeAINew(j, meta.parsed);
            meta.generator = Generator::InvokeAI;
        } catch (...) {}
    }

    // 2. InvokeAI (legacy sd-metadata JSON)
    if (meta.generator == Generator::Unknown) {
        if (const std::string* v = FindField(meta.rawFields, "sd-metadata")) {
            try {
                json j = json::parse(*v);
                ParseInvokeAILegacy(j, meta.parsed);
                meta.generator = Generator::InvokeAI;
            } catch (...) {}
        }
    }

    // 3. InvokeAI (ancient "Dream" CLI string)
    if (meta.generator == Generator::Unknown) {
        if (const std::string* v = FindField(meta.rawFields, "Dream")) {
            ParseDreamString(*v, meta.parsed);
            meta.generator = Generator::InvokeAI;
        }
    }

    // 4. NovelAI ("Comment" holds a JSON object with prompt/uc/steps/...)
    if (meta.generator == Generator::Unknown) {
        if (const std::string* v = FindField(meta.rawFields, "Comment")) {
            try {
                json j = json::parse(*v);
                if (j.is_object() && j.contains("prompt")) {
                    ParseNovelAI(j, meta.parsed);
                    meta.generator = Generator::NovelAI;
                }
            } catch (...) {}
        }
    }

    // 5. ComfyUI ("prompt" holds the API-format node graph as a JSON object)
    if (meta.generator == Generator::Unknown) {
        if (const std::string* v = FindField(meta.rawFields, "prompt")) {
            try {
                json j = json::parse(*v);
                if (j.is_object()) {
                    ParseComfyUI(j, meta.parsed);
                    meta.generator = Generator::ComfyUI;
                }
            } catch (...) {}
        }
    }

    // 5b. ComfyUI with only the editable graph ("workflow"), as videos usually carry
    if (meta.generator == Generator::Unknown) {
        if (const std::string* v = FindField(meta.rawFields, "workflow")) {
            try {
                json j = json::parse(*v);
                if (j.is_object() && j.contains("nodes")) {
                    ParseComfyWorkflowUi(j, meta.parsed);
                    meta.generator = Generator::ComfyUI;
                }
            } catch (...) {}
        }
    }

    // 6. AUTOMATIC1111-style "parameters" block, or the same format smuggled
    //    into an EXIF/XMP field by JPEG/WEBP exporters (A1111 and forks do this).
    if (meta.generator == Generator::Unknown) {
        static const char* kCandidateKeys[] = {"parameters", "EXIF:UserComment", "EXIF:ImageDescription"};
        for (const char* key : kCandidateKeys) {
            if (const std::string* v = FindField(meta.rawFields, key)) {
                if (ParseA1111Style(*v, meta.parsed)) {
                    meta.generator = Generator::Automatic1111;
                    break;
                }
            }
        }
    }
    if (meta.generator == Generator::Unknown) {
        for (const auto& f : meta.rawFields) {
            if (f.value.find("Negative prompt:") != std::string::npos || f.value.find("\nSteps:") != std::string::npos) {
                if (ParseA1111Style(f.value, meta.parsed)) {
                    meta.generator = Generator::Automatic1111;
                    break;
                }
            }
        }
    }

    // 7. Nothing recognized, but raw fields exist: still show them as-is.
    if (meta.generator == Generator::Unknown) {
        meta.generator = Generator::Generic;
    }

    meta.parsed.tags = SplitPromptTags(meta.parsed.positivePrompt);
    meta.parsed.negativeTags = SplitPromptTags(meta.parsed.negativePrompt);
}
