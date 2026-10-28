#include "comfy.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <fstream>
#include <random>
#include <regex>
#include <set>
#include <windows.h>
#include <tlhelp32.h>
#include <thread>
#include <winhttp.h>

#include "clef.h"
#include "json.hpp"
#include "workflow.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

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

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

long long NowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

std::wstring SettingsPath() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring base = (n > 0 && n < MAX_PATH) ? std::wstring(buf, n) : std::wstring(L".");
    return base + L"\\Try2Create\\comfy_settings.json";
}

// ---------------------------------------------------------------- HTTP
struct Endpoint {
    bool https = false;
    std::wstring host;
    INTERNET_PORT port = 80;
    std::wstring base; // path prefix (when ComfyUI sits behind a sub-path), no trailing slash
};

bool ParseEndpoint(const std::string& url, Endpoint& ep) {
    std::wstring w = ToWide(Trim(url));
    if (w.empty()) return false;
    if (w.find(L"://") == std::wstring::npos) w = L"http://" + w;
    wchar_t host[256] = {}, path[1024] = {};
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(w.c_str(), 0, 0, &uc)) return false;
    if (uc.nScheme != INTERNET_SCHEME_HTTP && uc.nScheme != INTERNET_SCHEME_HTTPS) return false;
    ep.https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    ep.host = host;
    ep.port = uc.nPort;
    ep.base = path;
    while (!ep.base.empty() && ep.base.back() == L'/') ep.base.pop_back();
    return !ep.host.empty();
}

bool Http(const Endpoint& ep, const wchar_t* method, const std::wstring& path, const std::string& body, const wchar_t* contentType, std::string& response,
          DWORD& status, int timeoutMs, std::string* error) {
    response.clear();
    status = 0;
    HINTERNET session = WinHttpOpen(L"Try2Create", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        if (error) *error = "Could not start the connection";
        return false;
    }
    WinHttpSetTimeouts(session, 3000, 3000, timeoutMs, timeoutMs);
    bool ok = false;
    if (HINTERNET conn = WinHttpConnect(session, ep.host.c_str(), ep.port, 0)) {
        const std::wstring target = ep.base + path;
        if (HINTERNET req = WinHttpOpenRequest(conn, method, target.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               ep.https ? WINHTTP_FLAG_SECURE : 0)) {
            if (contentType) {
                const std::wstring header = std::wstring(L"Content-Type: ") + contentType;
                WinHttpAddRequestHeaders(req, header.c_str(), static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
            }
            const BOOL sent = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                                                 static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
            if (sent && WinHttpReceiveResponse(req, nullptr)) {
                DWORD size = sizeof(status);
                WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
                for (;;) {
                    DWORD avail = 0;
                    if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
                    std::string chunk(avail, '\0');
                    DWORD got = 0;
                    if (!WinHttpReadData(req, chunk.data(), avail, &got) || got == 0) break;
                    response.append(chunk, 0, got);
                    if (response.size() > 32u * 1024 * 1024) break;
                }
                ok = true;
            } else if (error) {
                *error = "ComfyUI did not answer (is it running, and is the address right?)";
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(conn);
    }
    WinHttpCloseHandle(session);
    if (!ok && error && error->empty()) *error = "Could not connect";
    return ok;
}

bool ReadFileBytes(const std::wstring& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// Which file was uploaded under which name: a video's workflow only names ComfyUI's copy, and Recreate wants the picture itself.
// One "name<TAB>path" line per upload in %LOCALAPPDATA%\Try2Create\comfy_uploads.txt; the last line for a name wins.
std::mutex g_uploadsMutex;

std::wstring UploadsLogPath() {
    wchar_t buf[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    return std::wstring(buf) + L"\\Try2Create\\comfy_uploads.txt";
}

void RememberUpload(const std::string& name, const std::wstring& path) {
    std::lock_guard<std::mutex> lock(g_uploadsMutex);
    std::ofstream f(UploadsLogPath(), std::ios::app | std::ios::binary);
    if (f) f << name << '\t' << ToUtf8(path) << '\n';
}

// POST /upload/image: the picture becomes available to a LoadImage node. Returns the name to put in the node.
bool UploadImage(const Endpoint& ep, const std::wstring& path, std::string& nameOut, std::string& error) {
    std::string bytes;
    if (!ReadFileBytes(path, bytes)) {
        error = "Could not read the picture to upload";
        return false;
    }
    const std::string boundary = "----Try2CreateBoundary7f3a9c";
    const std::string filename = ToUtf8(fs::path(path).filename().wstring());
    std::string body = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"image\"; filename=\"" + filename +
                       "\"\r\nContent-Type: application/octet-stream\r\n\r\n";
    body += bytes;
    body += "\r\n--" + boundary + "\r\nContent-Disposition: form-data; name=\"overwrite\"\r\n\r\ntrue\r\n--" + boundary + "--\r\n";
    std::string response;
    DWORD status = 0;
    const std::wstring ct = L"multipart/form-data; boundary=" + ToWide(boundary);
    if (!Http(ep, L"POST", L"/upload/image", body, ct.c_str(), response, status, 60000, &error)) return false;
    if (status != 200) {
        error = "ComfyUI refused the picture (error " + std::to_string(status) + ")";
        return false;
    }
    json j = json::parse(response, nullptr, false);
    if (!j.is_object() || !j.contains("name")) {
        error = "ComfyUI answered the upload in an unexpected way";
        return false;
    }
    nameOut = j.value("name", std::string());
    const std::string sub = j.value("subfolder", std::string());
    if (!sub.empty()) nameOut = sub + "/" + nameOut;
    RememberUpload(nameOut, path);
    return true;
}

// ---------------------------------------------------------------- the workflow
bool IsApiFormat(const json& j) {
    if (!j.is_object() || j.empty()) return false;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (!it.value().is_object() || !it.value().contains("class_type") || !it.value().contains("inputs")) return false;
    }
    return true;
}

// The workflow ComfyUI is running right now (or, failing that, ran last): its API graph is in /queue and /history, so whatever you have open
// in ComfyUI and pressed Queue on once can be reused without exporting anything.
bool LoadLastRunTemplate(const ComfySettings& s, json& out, std::string& error) {
    Endpoint ep;
    if (!ParseEndpoint(s.url, ep)) {
        error = "The ComfyUI address is not valid";
        return false;
    }
    std::string response, netError;
    DWORD status = 0;
    if (!Http(ep, L"GET", L"/queue", std::string(), nullptr, response, status, 5000, &netError) || status != 200) {
        error = netError.empty() ? "Could not reach ComfyUI (is it running?)" : netError;
        return false;
    }
    json q = json::parse(response, nullptr, false);
    if (q.is_object()) {
        for (const char* key : {"queue_running", "queue_pending"}) {
            if (!q.contains(key) || !q[key].is_array() || q[key].empty()) continue;
            const json& item = q[key][0];
            if (item.is_array() && item.size() > 2 && IsApiFormat(item[2])) {
                out = item[2];
                return true;
            }
        }
    }
    if (!Http(ep, L"GET", L"/history?max_items=5", std::string(), nullptr, response, status, 8000, &netError) || status != 200) {
        error = netError.empty() ? "Could not read ComfyUI's history" : netError;
        return false;
    }
    json hist = json::parse(response, nullptr, false);
    double bestNumber = -1;
    if (hist.is_object()) {
        for (auto it = hist.begin(); it != hist.end(); ++it) {
            const json& p = it.value().contains("prompt") ? it.value()["prompt"] : json();
            if (!p.is_array() || p.size() < 3 || !IsApiFormat(p[2])) continue;
            const double number = p[0].is_number() ? p[0].get<double>() : 0;
            if (number >= bestNumber) {
                bestNumber = number;
                out = p[2];
            }
        }
    }
    if (bestNumber >= 0) return true;
    error = "ComfyUI has not run a workflow yet. Open the workflow you want in ComfyUI, press Queue once, then send again: I reuse the workflow ComfyUI ran "
            "last.";
    return false;
}


std::string Enc(const std::string& s) {
    std::string out;
    char buf[8];
    for (unsigned char ch : s) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out += static_cast<char>(ch);
        } else {
            std::snprintf(buf, sizeof(buf), "%%%02X", ch);
            out += buf;
        }
    }
    return out;
}

// ---------------------------------------------------------------- ComfyUI's saved workflows
bool ListSaved(const Endpoint& ep, std::vector<ComfySavedWorkflow>& out, std::string& error) {
    out.clear();
    std::string response;
    DWORD status = 0;
    if (!Http(ep, L"GET", L"/api/userdata?dir=workflows&recurse=true&split=false&full_info=true", std::string(), nullptr, response, status, 8000, &error)) return false;
    if (status != 200) {
        error = "ComfyUI would not list its saved workflows (error " + std::to_string(status) + ")";
        return false;
    }
    json j = json::parse(response, nullptr, false);
    if (!j.is_array()) {
        error = "ComfyUI answered the workflow list in an unexpected way";
        return false;
    }
    for (const json& e : j) {
        ComfySavedWorkflow w;
        if (e.is_string()) {
            w.path = e.get<std::string>();
        } else if (e.is_object()) {
            w.path = e.value("path", std::string());
            if (e.contains("modified") && e["modified"].is_number()) w.modified = static_cast<long long>(e["modified"].get<double>());
        }
        if (w.path.size() > 5 && _stricmp(w.path.c_str() + w.path.size() - 5, ".json") == 0) out.push_back(std::move(w));
    }
    std::sort(out.begin(), out.end(), [](const ComfySavedWorkflow& a, const ComfySavedWorkflow& b) { return a.modified != b.modified ? a.modified > b.modified : a.path < b.path; });
    return true;
}

bool FetchSaved(const Endpoint& ep, const std::string& path, std::string& text, std::string& error) {
    DWORD status = 0;
    if (!Http(ep, L"GET", L"/api/userdata/" + ToWide(Enc("workflows/" + path)), std::string(), nullptr, text, status, 15000, &error)) return false;
    if (status != 200) {
        error = "ComfyUI could not give me the workflow \"" + path + "\" (error " + std::to_string(status) + ")";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- editor workflow -> API format
// ComfyUI keeps the node definitions at /object_info; they say which inputs are links and which are widgets, in the order the
// editor stores the widget values.
std::mutex g_infoMutex;
nlohmann::ordered_json g_objectInfo;
long long g_objectInfoAt = 0;

bool LoadObjectInfo(const Endpoint& ep, nlohmann::ordered_json& out, std::string& error) {
    std::lock_guard<std::mutex> lock(g_infoMutex);
    if (!g_objectInfo.is_null() && NowMs() - g_objectInfoAt < 120000) {
        out = g_objectInfo;
        return true;
    }
    std::string response;
    DWORD status = 0;
    if (!Http(ep, L"GET", L"/object_info", std::string(), nullptr, response, status, 30000, &error) || status != 200) {
        if (error.empty()) error = "Could not read ComfyUI's node list";
        return false;
    }
    nlohmann::ordered_json j = nlohmann::ordered_json::parse(response, nullptr, false);
    if (!j.is_object()) {
        error = "ComfyUI's node list was not readable";
        return false;
    }
    g_objectInfo = std::move(j);
    g_objectInfoAt = NowMs();
    out = g_objectInfo;
    return true;
}

bool IsWidgetType(const nlohmann::ordered_json& type) {
    if (type.is_array()) return true; // a list of choices
    if (!type.is_string()) return false;
    const std::string t = type.get<std::string>();
    return t == "INT" || t == "FLOAT" || t == "STRING" || t == "BOOLEAN" || t == "COMBO";
}

bool ConvertEditorWorkflow(const Endpoint& ep, const json& ui, json& api, std::string& error) {
    if (!ui.is_object() || !ui.contains("nodes") || !ui["nodes"].is_array()) {
        error = "That file is not a ComfyUI workflow";
        return false;
    }
    if (ui.contains("definitions") && ui["definitions"].is_object() && ui["definitions"].contains("subgraphs") && !ui["definitions"]["subgraphs"].empty()) {
        error = "This workflow uses subgraphs, which I cannot convert. Queue it once in ComfyUI and use \"the workflow ComfyUI is running\" instead.";
        return false;
    }
    nlohmann::ordered_json info;
    if (!LoadObjectInfo(ep, info, error)) return false;

    std::map<long long, const json*> nodes;
    for (const json& n : ui["nodes"]) {
        if (n.is_object() && n.contains("id") && n["id"].is_number_integer()) nodes[n["id"].get<long long>()] = &n;
    }
    std::map<long long, const json*> links; // link id -> [id, origin, slot, target, slot, type]
    if (ui.contains("links") && ui["links"].is_array()) {
        for (const json& l : ui["links"]) {
            if (l.is_array() && l.size() >= 5 && l[0].is_number_integer()) links[l[0].get<long long>()] = &l;
        }
    }
    auto isVirtual = [](const std::string& t) { return t == "Reroute" || t == "PrimitiveNode" || t == "Note" || t == "MarkdownNote"; };
    auto modeOf = [](const json& n) { return n.value("mode", 0); };

    // where a link really comes from: through reroutes and bypassed nodes; a primitive node gives a plain value instead
    struct Source {
        bool ok = false, literal = false;
        long long node = 0;
        int slot = 0;
        json value;
    };
    std::function<Source(long long, int, int)> trace = [&](long long id, int slot, int depth) -> Source {
        Source s;
        auto it = nodes.find(id);
        if (it == nodes.end() || depth > 32) return s;
        const json& n = *it->second;
        const std::string type = n.value("type", std::string());
        if (type == "PrimitiveNode") {
            if (n.contains("widgets_values") && n["widgets_values"].is_array() && !n["widgets_values"].empty()) {
                s.ok = s.literal = true;
                s.value = n["widgets_values"][0];
            }
            return s;
        }
        auto follow = [&](int inputIndex) -> Source {
            Source none;
            if (!n.contains("inputs") || !n["inputs"].is_array() || inputIndex < 0 || inputIndex >= static_cast<int>(n["inputs"].size())) return none;
            const json& in = n["inputs"][inputIndex];
            if (!in.contains("link") || !in["link"].is_number_integer()) return none;
            auto l = links.find(in["link"].get<long long>());
            if (l == links.end()) return none;
            return trace((*l->second)[1].get<long long>(), (*l->second)[2].get<int>(), depth + 1);
        };
        if (type == "Reroute") return follow(0);
        if (modeOf(n) == 4) { // bypassed: the output is the input of the same kind
            std::string outType;
            if (n.contains("outputs") && n["outputs"].is_array() && slot >= 0 && slot < static_cast<int>(n["outputs"].size())) outType = n["outputs"][slot].value("type", std::string());
            if (n.contains("inputs") && n["inputs"].is_array()) {
                for (int i = 0; i < static_cast<int>(n["inputs"].size()); ++i) {
                    if (n["inputs"][i].value("type", std::string()) == outType) return follow(i);
                }
            }
            return follow(0);
        }
        s.ok = true;
        s.node = id;
        s.slot = slot;
        return s;
    };

    api = json::object();
    std::string missing;
    for (const auto& kv : nodes) {
        const json& n = *kv.second;
        const std::string type = n.value("type", std::string());
        if (type.empty() || isVirtual(type) || modeOf(n) == 2 || modeOf(n) == 4) continue;
        if (!info.contains(type)) {
            if (missing.find(type) == std::string::npos) missing += (missing.empty() ? "" : ", ") + type;
            continue;
        }
        const nlohmann::ordered_json& def = info[type];
        std::vector<std::pair<std::string, nlohmann::ordered_json>> inputDefs; // name, [type, options]
        auto collect = [&](const char* group) {
            if (!def.contains("input") || !def["input"].is_object() || !def["input"].contains(group) || !def["input"][group].is_object()) return;
            std::vector<std::string> order;
            if (def.contains("input_order") && def["input_order"].is_object() && def["input_order"].contains(group) && def["input_order"][group].is_array()) {
                for (const auto& name : def["input_order"][group]) order.push_back(name.get<std::string>());
            } else {
                for (auto it = def["input"][group].begin(); it != def["input"][group].end(); ++it) order.push_back(it.key());
            }
            for (const std::string& name : order) {
                if (def["input"][group].contains(name)) inputDefs.push_back({name, def["input"][group][name]});
            }
        };
        collect("required");
        collect("optional");

        json node = {{"class_type", type}, {"inputs", json::object()}};
        const json empty = json::array();
        const json& values = n.contains("widgets_values") ? n["widgets_values"] : empty;
        size_t vi = 0;
        std::set<std::string> widgetNames; // the editor marks which of a node's inputs are widgets
        if (n.contains("inputs") && n["inputs"].is_array()) {
            for (const json& in : n["inputs"]) {
                if (in.is_object() && in.contains("widget")) widgetNames.insert(in.value("name", std::string()));
            }
        }
        std::function<void(const std::string&, const nlohmann::ordered_json&, bool)> handle;
        handle = [&](const std::string& key, const nlohmann::ordered_json& spec, bool top) {
            const size_t dot = key.rfind('.');
            const std::string shortName = dot == std::string::npos ? key : key.substr(dot + 1);
            const nlohmann::ordered_json type0 = spec.is_array() && !spec.empty() ? spec[0] : nlohmann::ordered_json();
            const nlohmann::ordered_json opts = spec.is_array() && spec.size() > 1 && spec[1].is_object() ? spec[1] : nlohmann::ordered_json::object();
            const std::string typeName = type0.is_string() ? type0.get<std::string>() : std::string();
            const bool dynamic = typeName.find("DYNAMICCOMBO") != std::string::npos; // a combo whose choice brings its own inputs
            const bool widget = dynamic || ((IsWidgetType(type0) || widgetNames.count(shortName)) && !opts.value("forceInput", false));
            json literal;
            bool haveLiteral = false;
            if (widget) {
                if (values.is_array() && vi < values.size()) {
                    literal = values[vi];
                    haveLiteral = true;
                } else if (values.is_object() && values.contains(shortName)) {
                    literal = values[shortName];
                    haveLiteral = true;
                }
                ++vi;
                const bool control = opts.value("control_after_generate", false) || ((shortName == "seed" || shortName == "noise_seed") && typeName == "INT");
                const bool upload = opts.value("image_upload", false) || opts.value("video_upload", false) || opts.value("animated_image_upload", false);
                if (!dynamic && (control || upload)) ++vi; // the editor stores one more value after these widgets
            }
            if (top && n.contains("inputs") && n["inputs"].is_array()) { // a link plugged into this input?
                for (const json& in : n["inputs"]) {
                    if (in.value("name", std::string()) != shortName || !in.contains("link") || !in["link"].is_number_integer()) continue;
                    auto l = links.find(in["link"].get<long long>());
                    if (l == links.end()) continue;
                    const Source src = trace((*l->second)[1].get<long long>(), (*l->second)[2].get<int>(), 0);
                    if (src.ok && src.literal) {
                        node["inputs"][key] = src.value;
                        return;
                    }
                    if (src.ok) {
                        node["inputs"][key] = json::array({std::to_string(src.node), src.slot});
                        return;
                    }
                }
            }
            if (!widget) return;
            if (dynamic) {
                std::string selected = haveLiteral && literal.is_string() ? literal.get<std::string>() : std::string();
                const nlohmann::ordered_json options = opts.contains("options") && opts["options"].is_array() ? opts["options"] : nlohmann::ordered_json::array();
                const nlohmann::ordered_json* chosen = nullptr;
                for (const auto& o : options) {
                    if (o.is_object() && o.value("key", std::string()) == selected) chosen = &o;
                }
                if (!chosen && !options.empty()) {
                    chosen = &options[0];
                    selected = (*chosen).value("key", std::string());
                }
                node["inputs"][key] = selected;
                if (chosen && (*chosen).contains("inputs") && (*chosen)["inputs"].is_object()) {
                    for (const char* group : {"required", "optional"}) {
                        const auto& g = (*chosen)["inputs"];
                        if (!g.contains(group) || !g[group].is_object()) continue;
                        for (auto it = g[group].begin(); it != g[group].end(); ++it) handle(key + "." + it.key(), it.value(), false);
                    }
                }
                return;
            }
            if (haveLiteral) {
                node["inputs"][key] = literal;
            } else if (opts.contains("default")) { // the editor had no value: the node's own default
                node["inputs"][key] = json::parse(opts["default"].dump(), nullptr, false);
            } else if (type0.is_array() && !type0.empty()) {
                node["inputs"][key] = json::parse(type0[0].dump(), nullptr, false);
            }
        };
        for (const auto& d : inputDefs) handle(d.first, d.second, true);
        api[std::to_string(kv.first)] = std::move(node);
    }
    if (!missing.empty()) {
        error = "This workflow needs nodes ComfyUI does not have: " + missing;
        return false;
    }
    if (!IsApiFormat(api)) {
        error = "The workflow had nothing to run";
        return false;
    }
    // every required input must have a value: otherwise ComfyUI fails half way ("missing 1 required positional argument")
    std::string problems;
    for (auto it = api.begin(); it != api.end(); ++it) {
        const std::string cls = it.value().value("class_type", std::string());
        if (!info.contains(cls) || !info[cls].contains("input") || !info[cls]["input"].contains("required")) continue;
        for (auto r = info[cls]["input"]["required"].begin(); r != info[cls]["input"]["required"].end(); ++r) {
            const bool special = r.value().is_array() && !r.value().empty() && r.value()[0].is_string() && r.value()[0].get<std::string>().rfind("COMFY_", 0) == 0 && r.value()[0].get<std::string>().find("DYNAMICCOMBO") == std::string::npos;
            if (!special && !it.value()["inputs"].contains(r.key())) problems += (problems.empty() ? "" : "; ") + cls + " (node " + it.key() + ") has no value for \"" + r.key() + "\"";
        }
    }
    if (!problems.empty()) {
        error = "I could not convert this saved workflow safely: " + problems + ". Run the workflow once in ComfyUI (press Queue) and choose \"ComfyUI's current / last run\" instead.";
        return false;
    }
    return true;
}

bool LoadSavedTemplate(const ComfySettings& s, json& out, std::string& error) {
    Endpoint ep;
    if (!ParseEndpoint(s.url, ep)) {
        error = "The ComfyUI address is not valid";
        return false;
    }
    std::string text;
    if (!FetchSaved(ep, s.savedWorkflow, text, error)) return false;
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded()) {
        error = "ComfyUI's saved workflow was not readable";
        return false;
    }
    if (IsApiFormat(j)) {
        out = std::move(j);
        return true;
    }
    return ConvertEditorWorkflow(ep, j, out, error);
}


// Video workflows: frames = seconds x fps in the node(s) that hold the clip length (Wan, LTX, Hunyuan, Mochi, AnimateDiff...).
void ApplyVideoLength(json& wf, double seconds, int defaultFps) {
    if (seconds <= 0) return;
    double fps = 0;
    for (auto it = wf.begin(); it != wf.end() && fps <= 0; ++it) {
        const json& in = it.value()["inputs"];
        if (!in.is_object()) continue;
        for (const char* k : {"fps", "frame_rate", "framerate"}) {
            if (in.contains(k) && in[k].is_number() && in[k].get<double>() >= 1) fps = in[k].get<double>();
        }
    }
    if (fps <= 0) fps = defaultFps;
    const long long base = std::max(1LL, std::llround(seconds * fps));
    for (auto it = wf.begin(); it != wf.end(); ++it) {
        json& in = it.value()["inputs"];
        if (!in.is_object()) continue;
        const std::string cls = it.value().value("class_type", std::string());
        auto has = [&](const char* part) { return cls.find(part) != std::string::npos; };
        const bool videoNode = has("Wan") || has("LTX") || has("Hunyuan") || has("Mochi") || has("Cosmos") || has("Video") || has("Frames") || has("AnimateDiff") ||
                               has("Latent") || has("CogVideo") || has("Hyvid");
        for (const char* k : {"length", "num_frames", "frames", "video_frames", "frame_count", "frames_number"}) {
            if (!in.contains(k) || !in[k].is_number_integer()) continue;
            const std::string key = k;
            if (!videoNode && key != "num_frames" && key != "video_frames" && key != "frame_count") continue;
            long long frames = base;
            if (has("Wan")) frames = 4 * std::llround((frames - 1) / 4.0) + 1;       // Wan wants 4n + 1 frames
            else if (has("LTX")) frames = 8 * std::llround((frames - 1) / 8.0) + 1;  // LTX wants 8n + 1
            in[k] = std::max(1LL, frames);
        }
    }
}

bool LoadTemplate(const ComfySettings& s, json& out, std::string& error) {
    if (s.useLastRun) return LoadLastRunTemplate(s, out, error);
    if (!s.savedWorkflow.empty()) return LoadSavedTemplate(s, out, error);
    if (s.workflowPath.empty()) {
        error = "No workflow chosen yet";
        return false;
    }
    std::string text, api, err;
    if (!ReadWorkflowFromFile(s.workflowPath, text, api, &err)) {
        error = err.empty() ? "Could not read the workflow" : err;
        return false;
    }
    for (const std::string* src : {&api, &text}) {
        if (src->empty()) continue;
        json j = json::parse(*src, nullptr, false);
        if (j.is_discarded()) continue;
        if (j.is_object() && j.contains("prompt") && IsApiFormat(j["prompt"])) j = j["prompt"];
        if (IsApiFormat(j)) {
            out = std::move(j);
            return true;
        }
    }
    error = "That file is a ComfyUI editor workflow, and ComfyUI can only run the API format. In ComfyUI use Workflow > Export (API), then choose that "
            "file here. A picture that ComfyUI generated also works: it carries the API version.";
    return false;
}

std::string LinkedNode(const json& v) {
    if (v.is_array() && v.size() == 2 && (v[0].is_string() || v[0].is_number_integer())) return v[0].is_string() ? v[0].get<std::string>() : std::to_string(v[0].get<long long>());
    return {};
}

const char* const kTextKeys[] = {"text", "prompt", "positive_prompt", "text_g", "text_l", "clip_l", "t5xxl", "sequence_prompt", "string", "value"};

bool HasTextInput(const json& inputs) {
    for (const char* k : kTextKeys) {
        if ((std::string(k) == "string" || std::string(k) == "value")) continue; // those only count when reached through a link
        auto it = inputs.find(k);
        if (it != inputs.end() && (it->is_string() || !LinkedNode(*it).empty())) return true;
    }
    return false;
}

// Follows conditioning links (ConditioningCombine, FluxGuidance, ControlNetApply...) until a node that holds the prompt text.
std::string ResolveTextNode(const json& wf, const std::string& id, int depth = 0) {
    if (depth > 8 || id.empty() || !wf.contains(id)) return {};
    const json& n = wf[id];
    if (!n.contains("inputs") || !n["inputs"].is_object()) return {};
    const json& inputs = n["inputs"];
    if (HasTextInput(inputs)) return id;
    for (auto it = inputs.begin(); it != inputs.end(); ++it) {
        const std::string link = LinkedNode(it.value());
        if (link.empty()) continue;
        const std::string key = it.key();
        if (key.find("conditioning") == std::string::npos && key != "positive" && key != "negative" && key.find("cond") == std::string::npos) continue;
        const std::string r = ResolveTextNode(wf, link, depth + 1);
        if (!r.empty()) return r;
    }
    return {};
}

struct Targets {
    std::set<std::string> positive, negative, seeds, loadImages;
};

Targets FindTargets(const json& wf, const ComfySettings& s) {
    Targets t;
    for (auto it = wf.begin(); it != wf.end(); ++it) {
        const json& inputs = it.value()["inputs"];
        if (!inputs.is_object()) continue;
        if (inputs.contains("positive") && inputs.contains("negative")) {
            const std::string pos = ResolveTextNode(wf, LinkedNode(inputs["positive"]));
            const std::string neg = ResolveTextNode(wf, LinkedNode(inputs["negative"]));
            if (!pos.empty()) t.positive.insert(pos);
            if (!neg.empty()) t.negative.insert(neg);
        }
        if (inputs.contains("seed") && inputs["seed"].is_number()) t.seeds.insert(it.key());
        if (inputs.contains("base_seed") && inputs["base_seed"].is_number()) t.seeds.insert(it.key()); // H3 Continuum
        if (inputs.contains("sequence_prompt") && !LinkedNode(inputs["sequence_prompt"]).empty()) t.positive.insert(it.key()); // (its text node is filled)
        if (inputs.contains("noise_seed") && inputs["noise_seed"].is_number()) t.seeds.insert(it.key());
        if (it.value().value("class_type", std::string()) == "LoadImage") t.loadImages.insert(it.key());
    }
    if (t.positive.empty()) { // a guider with one conditioning (BasicGuider: Flux, MiniMax H3...): its prompt is up that link
        for (auto it = wf.begin(); it != wf.end(); ++it) {
            const json& inputs = it.value()["inputs"];
            if (!inputs.is_object() || !inputs.contains("conditioning") || it.value().value("class_type", std::string()).find("Guider") == std::string::npos) continue;
            const std::string pos = ResolveTextNode(wf, LinkedNode(inputs["conditioning"]));
            if (!pos.empty()) t.positive.insert(pos);
        }
    }
    if (t.positive.empty()) { // no sampler found: the first text-encode node is the prompt, the second the negative
        std::vector<std::string> encoders;
        for (auto it = wf.begin(); it != wf.end(); ++it) {
            const std::string cls = it.value().value("class_type", std::string());
            if (cls.find("TextEncode") != std::string::npos && it.value()["inputs"].is_object() && HasTextInput(it.value()["inputs"])) encoders.push_back(it.key());
        }
        if (!encoders.empty()) t.positive.insert(encoders[0]);
        if (encoders.size() > 1 && t.negative.empty()) t.negative.insert(encoders[1]);
    }
    if (!Trim(s.promptNode).empty() && wf.contains(Trim(s.promptNode))) {
        t.positive.clear();
        t.positive.insert(Trim(s.promptNode));
    }
    if (!Trim(s.negativeNode).empty() && wf.contains(Trim(s.negativeNode))) {
        t.negative.clear();
        t.negative.insert(Trim(s.negativeNode));
    }
    for (const std::string& p : t.positive) t.negative.erase(p); // never overwrite the prompt with the negative
    return t;
}

// Puts `text` into the node's text input(s); follows a link to a string / primitive node when the text comes from one.
bool SetText(json& wf, const std::string& id, const std::string& text, int depth = 0) {
    if (depth > 3 || !wf.contains(id) || !wf[id]["inputs"].is_object()) return false;
    json& inputs = wf[id]["inputs"];
    bool any = false;
    for (const char* k : kTextKeys) {
        const std::string key = k;
        if (key == "string" || key == "value") continue;
        auto it = inputs.find(key);
        if (it == inputs.end()) continue;
        if (it->is_string()) {
            *it = text;
            any = true;
        } else {
            const std::string link = LinkedNode(*it);
            if (!link.empty() && wf.contains(link) && wf[link]["inputs"].is_object()) {
                json& src = wf[link]["inputs"];
                for (const char* sk : {"value", "string", "text", "prompt"}) {
                    auto sit = src.find(sk);
                    if (sit != src.end() && sit->is_string()) {
                        *sit = text;
                        any = true;
                        break;
                    }
                }
            }
        }
    }
    return any;
}

std::string DescribeNodes(const json& wf, const std::set<std::string>& ids) {
    std::string out;
    for (const std::string& id : ids) {
        if (!out.empty()) out += ", ";
        out += id;
        if (wf.contains(id)) out += " (" + wf[id].value("class_type", std::string()) + ")";
    }
    return out.empty() ? "none found" : out;
}

std::string QueueError(const std::string& body, DWORD status) {
    json j = json::parse(body, nullptr, false);
    std::string out;
    if (j.is_object()) {
        if (j.contains("error") && j["error"].is_object()) {
            out = j["error"].value("message", std::string());
            const std::string details = j["error"].value("details", std::string());
            if (!details.empty()) out += (out.empty() ? "" : ": ") + details;
        } else if (j.contains("error") && j["error"].is_string()) {
            out = j["error"].get<std::string>();
        }
        if (j.contains("node_errors") && j["node_errors"].is_object()) {
            for (auto it = j["node_errors"].begin(); it != j["node_errors"].end(); ++it) {
                const json& e = it.value();
                if (!e.contains("errors") || !e["errors"].is_array()) continue;
                for (const json& one : e["errors"]) {
                    out += std::string(out.empty() ? "" : "; ") + "node " + it.key() + " (" + e.value("class_type", std::string()) + "): " + one.value("message", std::string());
                    const std::string d = one.value("details", std::string());
                    if (!d.empty()) out += " " + d;
                }
            }
        }
    }
    if (out.empty()) out = "ComfyUI refused the prompt (error " + std::to_string(status) + ")";
    return out;
}

std::mt19937_64& Rng() {
    static std::mt19937_64 rng{std::random_device{}()};
    return rng;
}

} // namespace


// ---------------------------------------------------------------- ComfyUI in the background
namespace {
HANDLE g_comfyJob = nullptr;
HANDLE g_comfyProcess = nullptr;
std::mutex g_comfyMutex;

std::wstring EnvW(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH * 2);
    return (n > 0 && n < MAX_PATH * 2) ? std::wstring(buf, n) : std::wstring();
}

// What ComfyUI Desktop itself uses to start its server (it keeps these in its own settings files).
struct HeadlessLaunch {
    std::wstring python, workDir, mainPy, yaml, inputDir, outputDir;
    std::string args;
};

bool FindHeadless(HeadlessLaunch& h) {
    const std::wstring data = EnvW(L"APPDATA") + L"\\Comfy Desktop";
    std::string text;
    if (!ReadFileBytes(data + L"\\installations.json", text)) return false;
    json list = json::parse(text, nullptr, false);
    if (!list.is_array()) return false;
    const json* best = nullptr;
    double bestWhen = -1;
    for (const json& e : list) {
        if (!e.is_object() || e.value("sourceId", std::string()) != "standalone" || e.value("status", std::string("installed")) != "installed") continue;
        const double when = e.contains("lastLaunchedAt") && e["lastLaunchedAt"].is_number() ? e["lastLaunchedAt"].get<double>() : 0;
        if (when >= bestWhen) {
            best = &e;
            bestWhen = when;
        }
    }
    if (!best) return false;
    const std::wstring install = ToWide(best->value("installPath", std::string()));
    if (install.empty()) return false;
    for (const std::wstring& p : {install + L"\\ComfyUI\\.venv\\Scripts\\python.exe", install + L"\\standalone-env\\python.exe"}) {
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) {
            h.python = p;
            break;
        }
    }
    if (h.python.empty() || GetFileAttributesW((install + L"\\ComfyUI\\main.py").c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    h.workDir = install;
    h.mainPy = L"ComfyUI\\main.py";
    h.args = best->value("launchArgs", std::string());
    const std::wstring yaml = data + L"\\instance-model-paths\\" + ToWide(best->value("id", std::string())) + L".yaml";
    if (GetFileAttributesW(yaml.c_str()) != INVALID_FILE_ATTRIBUTES) h.yaml = yaml;
    std::string settingsText;
    if (ReadFileBytes(data + L"\\settings.json", settingsText)) {
        json s = json::parse(settingsText, nullptr, false);
        if (s.is_object()) {
            h.inputDir = ToWide(s.value("inputDir", std::string()));
            h.outputDir = ToWide(s.value("outputDir", std::string()));
        }
    }
    return true;
}
} // namespace

bool ComfyFindDesktop(std::wstring& exe) {
    const std::wstring local = EnvW(L"LOCALAPPDATA");
    const std::wstring candidates[] = {local + L"\\Programs\\Comfy Desktop\\Comfy Desktop.exe", local + L"\\Programs\\ComfyUI\\ComfyUI.exe",
                                       local + L"\\Programs\\comfyui-electron\\ComfyUI.exe", L"C:\\Program Files\\ComfyUI\\ComfyUI.exe",
                                       L"C:\\Program Files\\Comfy Desktop\\Comfy Desktop.exe"};
    for (const std::wstring& c : candidates) {
        if (!local.empty() && GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES) {
            exe = c;
            return true;
        }
    }
    return false;
}

// ComfyUI Desktop's models folder: the base_path of its model-paths file, else the install's own models folder.
std::wstring ComfyModelsDir() {
    HeadlessLaunch h;
    if (!FindHeadless(h)) return {};
    std::string text;
    if (!h.yaml.empty() && ReadFileBytes(h.yaml, text)) {
        // the first base_path that is not in a comment (Comfy Desktop's file starts with a commented example: "#     base_path: '...'")
        for (size_t k = text.find("base_path:"); k != std::string::npos; k = text.find("base_path:", k + 10)) {
            const size_t lineStart = text.rfind('\n', k) == std::string::npos ? 0 : text.rfind('\n', k) + 1;
            const size_t first = text.find_first_not_of(" \t", lineStart);
            if (first != std::string::npos && text[first] == '#') continue;
            size_t a = text.find_first_not_of(" \t", k + 10), e = text.find_first_of("\r\n", k);
            if (e == std::string::npos) e = text.size();
            if (a == std::string::npos || a >= e) continue;
            std::string v = text.substr(a, e - a);
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
            if (v.size() >= 2 && (v.front() == '\'' || v.front() == '"') && v.back() == v.front()) v = v.substr(1, v.size() - 2);
            const std::wstring dir = ToWide(v);
            if (!dir.empty() && GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) return dir;
        }
    }
    const std::wstring own = h.workDir + L"\\ComfyUI\\models";
    return GetFileAttributesW(own.c_str()) != INVALID_FILE_ATTRIBUTES ? own : std::wstring();
}

std::wstring ComfyInstallDir() {
    HeadlessLaunch h;
    return FindHeadless(h) ? h.workDir : std::wstring();
}

std::wstring ComfyInputDir() {
    const std::wstring test = EnvW(L"MV_COMFY_INPUT");
    if (!test.empty()) return test;
    HeadlessLaunch h;
    if (!FindHeadless(h)) return {};
    return h.inputDir.empty() ? h.workDir + L"\\ComfyUI\\input" : h.inputDir;
}

// ---- the browser bridge
namespace {
const char* const kBridgeInit = R"PY(# Try2Create's browser bridge: no nodes, only a small frontend extension (js/try2create_bridge.js).
# ComfyUI opened as http://127.0.0.1:8188/?try2create=<prompt id> shows the workflow of that job (queued, running or done),
# loaded with ComfyUI's own API-to-graph loader, so its progress can be watched on the nodes. Written by Try2Create; safe to delete.
NODE_CLASS_MAPPINGS = {}
NODE_DISPLAY_NAME_MAPPINGS = {}
WEB_DIRECTORY = "./js"
)PY";

const char* const kBridgeJs = R"JS(import { app } from "../../scripts/app.js";
import { api } from "../../scripts/api.js";

// Try2Create's browser bridge: ComfyUI opened as /?try2create=<prompt id>&name=<label> shows that job's workflow
// (queued, running or done) in a new tab, loaded with ComfyUI's own API-to-graph loader. ComfyUI sends a job's progress
// only to the client that queued it, so this page then takes Try2Create's client id: the progress shows here.
function followTry2Create() {
  if (window.name === "try2create") return;
  window.name = "try2create";
  sessionStorage.setItem("clientId", "try2create");
  try {
    api.socket?.close(); // it reconnects by itself, as window.name
  } catch (e) {
    console.warn("[Try2Create] could not reconnect", e);
  }
}
async function findPrompt(id) {
  try {
    const q = await (await fetch("/queue")).json();
    for (const item of [...(q.queue_running || []), ...(q.queue_pending || [])]) {
      if (Array.isArray(item) && item[1] === id) return item[2];
    }
    const h = await (await fetch("/history/" + encodeURIComponent(id))).json();
    if (h && h[id] && Array.isArray(h[id].prompt)) return h[id].prompt[2];
  } catch (e) {
    console.error("[Try2Create] could not ask ComfyUI for the job", e);
  }
  return null;
}

app.registerExtension({
  name: "Try2Create.OpenJob",
  async setup() {
    const url = new URL(window.location.href);
    const id = url.searchParams.get("try2create");
    if (!id) return;
    const name = url.searchParams.get("name") || "Try2Create job";
    url.searchParams.delete("try2create");
    url.searchParams.delete("name");
    window.history.replaceState(window.history.state, "", url.toString());
    const prompt = await findPrompt(id);
    if (!prompt) {
      console.warn("[Try2Create] ComfyUI does not have job " + id + " any more");
      return;
    }
    // after the frontend has put back its own tabs, or they would cover this one
    setTimeout(async () => {
      try {
        await app.loadApiJson(prompt, name);
      } catch (e) {
        console.error("[Try2Create]", e);
      }
      followTry2Create();
    }, 1500);
  },
});
)JS";

std::wstring BridgeDir() {
    std::wstring nodes = EnvW(L"MV_COMFY_NODES"); // (tests: a custom_nodes folder of their own)
    if (nodes.empty()) {
        const std::wstring install = ComfyInstallDir();
        if (install.empty()) return {};
        nodes = install + L"\\ComfyUI\\custom_nodes";
    }
    return nodes + L"\\try2create_bridge";
}
} // namespace

bool ComfyBridgeInstalled() {
    const std::wstring dir = BridgeDir();
    std::error_code ec;
    if (dir.empty() || !fs::exists(dir + L"\\__init__.py", ec) || !fs::exists(dir + L"\\js\\try2create_bridge.js", ec)) return false;
    std::string js; // an older version of its script is brought up to date (ComfyUI serves it from disk: no restart needed)
    if (ReadFileBytes(dir + L"\\js\\try2create_bridge.js", js) && js != kBridgeJs) {
        std::ofstream f(dir + L"\\js\\try2create_bridge.js", std::ios::binary | std::ios::trunc);
        if (f) f << kBridgeJs;
    }
    return true;
}

bool ComfyInstallBridge(std::string& message) {
    const std::wstring dir = BridgeDir();
    if (dir.empty()) {
        message = "ComfyUI Desktop's folder was not found";
        return false;
    }
    std::error_code ec;
    fs::create_directories(dir + L"\\js", ec);
    std::ofstream py(dir + L"\\__init__.py", std::ios::binary | std::ios::trunc);
    std::ofstream js(dir + L"\\js\\try2create_bridge.js", std::ios::binary | std::ios::trunc);
    if (!py || !js) {
        message = "Could not write into " + ToUtf8(dir);
        return false;
    }
    py << kBridgeInit;
    js << kBridgeJs;
    message = "Added Try2Create's bridge to ComfyUI (" + ToUtf8(dir) + "). Restart ComfyUI once to load it.";
    return true;
}

bool ComfyBridgeLoaded(const std::string& url) {
    Endpoint ep;
    std::string response;
    DWORD status = 0;
    return ParseEndpoint(url, ep) && Http(ep, L"GET", L"/extensions", std::string(), nullptr, response, status, 3000, nullptr) && status == 200 &&
           response.find("try2create_bridge") != std::string::npos;
}

int ComfyHasNode(const std::string& url, const std::string& node) {
    Endpoint ep;
    std::string response;
    DWORD status = 0;
    if (!ParseEndpoint(url, ep) || !Http(ep, L"GET", (L"/object_info/" + ToWide(node)).c_str(), std::string(), nullptr, response, status, 4000, nullptr)) return -1;
    return status == 200 && response.find("\"" + node + "\"") != std::string::npos ? 1 : 0;
}

std::string ComfyHistoryText(const std::string& url, const std::string& promptId) {
    Endpoint ep;
    std::string response;
    DWORD status = 0;
    if (promptId.empty() || !ParseEndpoint(url, ep) ||
        !Http(ep, L"GET", (L"/history/" + ToWide(promptId)).c_str(), std::string(), nullptr, response, status, 5000, nullptr) || status != 200)
        return {};
    const json j = json::parse(response, nullptr, false);
    if (!j.is_object() || !j.contains(promptId) || !j[promptId].contains("outputs")) return {};
    std::string text;
    for (const auto& [node, out] : j[promptId]["outputs"].items()) {
        if (!out.is_object() || !out.contains("text") || !out["text"].is_array()) continue;
        for (const json& t : out["text"]) {
            if (t.is_string()) text += (text.empty() ? "" : "\n") + t.get<std::string>();
        }
    }
    return text;
}

// ---- how long each result took: {"<lower-case path>": {"s": seconds, "what": label, "at": unix time}}, newest 5000 kept
namespace {
std::mutex g_timesMutex;
json g_times;
bool g_timesLoaded = false;

std::wstring TimesPath() { return EnvW(L"LOCALAPPDATA") + L"\\Try2Create\\generation_times.json"; }

std::string TimeKey(const std::wstring& path) {
    std::wstring low = path;
    for (wchar_t& c : low) c = static_cast<wchar_t>(towlower(c));
    std::replace(low.begin(), low.end(), L'/', L'\\');
    return ToUtf8(low);
}

void LoadTimes() { // with g_timesMutex held
    if (g_timesLoaded) return;
    g_timesLoaded = true;
    std::string text;
    if (ReadFileBytes(TimesPath(), text)) g_times = json::parse(text, nullptr, false);
    if (!g_times.is_object()) g_times = json::object();
}

void RecordGenerationTime(const std::wstring& file, double seconds, const std::string& what) {
    if (file.empty() || seconds <= 0) return;
    std::lock_guard<std::mutex> lock(g_timesMutex);
    LoadTimes();
    g_times[TimeKey(file)] = {{"s", std::round(seconds * 10.0) / 10.0}, {"what", what}, {"at", static_cast<long long>(time(nullptr))}};
    if (g_times.size() > 5000) { // the oldest go
        std::vector<std::pair<long long, std::string>> byAge;
        for (const auto& [k, v] : g_times.items()) byAge.emplace_back(v.value("at", 0LL), k);
        std::sort(byAge.begin(), byAge.end());
        for (size_t i = 0; i + 5000 < byAge.size(); ++i) g_times.erase(byAge[i].second);
    }
    std::error_code ec;
    fs::create_directories(fs::path(TimesPath()).parent_path(), ec);
    std::ofstream f(TimesPath(), std::ios::binary | std::ios::trunc);
    if (f) f << g_times.dump(0, ' ', false, json::error_handler_t::replace);
}

// ComfyUI's own clock for a finished prompt: from "execution_start" to "execution_success" in its history (0 = not there)
double HistorySeconds(const json& h) {
    long long start = 0, end = 0;
    if (h.contains("status") && h["status"].is_object() && h["status"].contains("messages") && h["status"]["messages"].is_array()) {
        for (const json& m : h["status"]["messages"]) {
            if (!m.is_array() || m.size() < 2 || !m[0].is_string() || !m[1].is_object() || !m[1].contains("timestamp") || !m[1]["timestamp"].is_number()) continue;
            const std::string what = m[0].get<std::string>();
            if (what == "execution_start") start = m[1]["timestamp"].get<long long>();
            else if (what == "execution_success") end = m[1]["timestamp"].get<long long>();
        }
    }
    return start > 0 && end > start ? (end - start) / 1000.0 : 0.0;
}
} // namespace

bool ComfyGenerationTime(const std::wstring& file, double& seconds, std::string* what) {
    std::lock_guard<std::mutex> lock(g_timesMutex);
    LoadTimes();
    const auto it = g_times.find(TimeKey(file));
    if (it == g_times.end() || !it->is_object()) return false;
    seconds = it->value("s", 0.0);
    if (what) *what = it->value("what", std::string());
    return seconds > 0;
}

std::string FormatTook(double seconds) {
    const long long s = std::llround(std::max(0.0, seconds));
    char buf[32];
    if (s < 60) snprintf(buf, sizeof(buf), "%lld s", s);
    else if (s < 3600) snprintf(buf, sizeof(buf), "%lld min %02lld s", s / 60, s % 60);
    else snprintf(buf, sizeof(buf), "%lld h %02lld min", s / 3600, (s % 3600) / 60);
    return buf;
}

std::wstring ComfyUploadedFrom(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_uploadsMutex);
    std::ifstream f(UploadsLogPath(), std::ios::binary);
    std::string line, found;
    while (std::getline(f, line)) {
        const size_t tab = line.find('\t');
        if (tab != std::string::npos && line.compare(0, tab, name) == 0 && tab == name.size()) found = line.substr(tab + 1);
    }
    return found.empty() ? std::wstring() : ToWide(found);
}

std::wstring PixelModelsDir(const ComfySettings& s) {
    if (!s.pixelModels.empty()) return s.pixelModels;
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return (fs::path(buf).parent_path() / L"retrodiffusion").wstring();
}

namespace {
bool HasPixelModels(const std::wstring& dir) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::wstring n = e.path().filename().wstring();
        std::transform(n.begin(), n.end(), n.begin(), ::towlower);
        if (n.find(L"retrodiffusion") != std::wstring::npos && e.path().extension() == L".safetensors") return true;
    }
    return false;
}

std::wstring PixelLinkPath() {
    const std::wstring models = ComfyModelsDir();
    return models.empty() ? std::wstring() : models + L"\\checkpoints\\RetroDiffusion";
}

// The extra model-paths file given to a ComfyUI this app starts, so the Retro Diffusion folder is a checkpoint folder without any link ("" = not needed).
std::wstring PixelPathsYaml(const ComfySettings& s) {
    const std::wstring dir = PixelModelsDir(s);
    if (!HasPixelModels(dir)) return {};
    const std::wstring link = PixelLinkPath();
    if (!link.empty() && GetFileAttributesW(link.c_str()) != INVALID_FILE_ATTRIBUTES) return {}; // already linked into ComfyUI's own folder
    std::string base = ToUtf8(fs::path(dir).parent_path().wstring()), name = ToUtf8(fs::path(dir).filename().wstring());
    auto quote = [](std::string v) {
        std::string out = "'";
        for (char ch : v) out += ch == '\'' ? std::string("''") : std::string(1, ch);
        return out + "'";
    };
    const std::wstring path = EnvW(L"LOCALAPPDATA") + L"\\Try2Create\\pixel_model_paths.yaml";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return {};
    out << "try2create_pixel_art:\n  base_path: " << quote(base) << "\n  checkpoints: " << quote(name + "/") << "\n";
    return out ? path : std::wstring();
}
} // namespace

bool ComfyLinkPixelModels(const ComfySettings& s, std::string& message) {
    const std::wstring dir = PixelModelsDir(s);
    if (!HasPixelModels(dir)) {
        message = "There are no RetroDiffusion*.safetensors files in " + ToUtf8(dir) + ". Choose the folder that has them.";
        return false;
    }
    const std::wstring link = PixelLinkPath();
    if (link.empty()) {
        message = "I could not find ComfyUI's models folder. Copy the Retro Diffusion files into ComfyUI's models\\checkpoints folder yourself.";
        return false;
    }
    if (GetFileAttributesW(link.c_str()) != INVALID_FILE_ATTRIBUTES) {
        message = "ComfyUI's checkpoints folder already has a RetroDiffusion folder (" + ToUtf8(link) + ")";
        return true;
    }
    std::wstring cmd = L"cmd.exe /c mklink /J \"" + link + L"\" \"" + dir + L"\"";
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        message = "Could not run mklink";
        return false;
    }
    WaitForSingleObject(pi.hProcess, 10000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code != 0 || GetFileAttributesW(link.c_str()) == INVALID_FILE_ATTRIBUTES) {
        message = "Could not make the link " + ToUtf8(link) + " (mklink failed)";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_infoMutex);
        g_objectInfoAt = 0; // ComfyUI's lists are read again
    }
    message = "Linked: ComfyUI now finds the Retro Diffusion models in checkpoints\\RetroDiffusion (no files were copied)";
    return true;
}

std::wstring ComfyOutputDir(const ComfySettings& s) {
    if (!s.outputDir.empty()) return s.outputDir;
    HeadlessLaunch h;
    if (FindHeadless(h)) return h.outputDir;
    return {};
}

bool ComfyOwned() {
    std::lock_guard<std::mutex> lock(g_comfyMutex);
    return g_comfyProcess && WaitForSingleObject(g_comfyProcess, 0) == WAIT_TIMEOUT;
}

void ComfyStopOwned() {
    std::lock_guard<std::mutex> lock(g_comfyMutex);
    if (g_comfyJob) TerminateJobObject(g_comfyJob, 0); // ends the server and everything it started
    else if (g_comfyProcess) TerminateProcess(g_comfyProcess, 0);
    if (g_comfyProcess) {
        WaitForSingleObject(g_comfyProcess, 3000);
        CloseHandle(g_comfyProcess);
        g_comfyProcess = nullptr;
    }
    if (g_comfyJob) {
        CloseHandle(g_comfyJob);
        g_comfyJob = nullptr;
    }
}

// ComfyUI behind the other programs: CPU priority below normal, and below normal in Windows' GPU scheduler too, so the desktop, a browser
// playing a video and this app's window get the GPU first and generation takes what is left. (A 10 s 1024x1280 video keeps the GPU busy
// for 5-6 minutes; at normal priority the PC lagged, YouTube froze and the NVIDIA driver logged resets.) The GPU priority is not inherited, and
// the venv's python.exe starts the real interpreter as a child: the whole process tree gets it, for its first minute.
static void SetBackgroundPriority(DWORD pid) {
    typedef LONG(APIENTRY * SetGpuPriority)(HANDLE, int);
    static const SetGpuPriority setGpu = reinterpret_cast<SetGpuPriority>(GetProcAddress(GetModuleHandleW(L"gdi32.dll"), "D3DKMTSetProcessSchedulingPriorityClass"));
    if (HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        SetPriorityClass(h, BELOW_NORMAL_PRIORITY_CLASS);
        if (setGpu) setGpu(h, 1); // D3DKMT_SCHEDULINGPRIORITYCLASS_BELOW_NORMAL
        CloseHandle(h);
    }
}

static void BackgroundTree(DWORD root) {
    std::thread([root]() {
        std::set<DWORD> done;
        for (int pass = 0; pass < 30; ++pass) { // a minute: the interpreter and any helpers it starts
            std::set<DWORD> tree = {root};
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snap != INVALID_HANDLE_VALUE) {
                PROCESSENTRY32W e = {sizeof(e)};
                for (bool grew = true; grew;) { // children of children
                    grew = false;
                    if (!Process32FirstW(snap, &e)) break;
                    do {
                        if (tree.count(e.th32ParentProcessID) && !tree.count(e.th32ProcessID)) tree.insert(e.th32ProcessID), grew = true;
                    } while (Process32NextW(snap, &e));
                }
                CloseHandle(snap);
            }
            for (DWORD pid : tree) {
                if (done.insert(pid).second) SetBackgroundPriority(pid);
            }
            Sleep(2000);
        }
    }).detach();
}

// ---- ComfyUI's scratch files off the Windows drive (2026-10-26: "you can't be using my main drive 100%"). Long videos (H3 Continuum's decode
// cache, 2 GB a part), the upscalers' streamed frames (3.5 GB per 10 s at 1280 x 1920) and Python's temp files write tens of GB; on this PC
// C: is a QLC SSD that slows to a crawl once its write cache fills, and 40 GB of Continuum caches had been left in %TEMP% by ComfyUI runs that
// were stopped (a stopped process never cleans up). So the ComfyUI this app starts gets its temp folders on the fixed drive other than the
// Windows one with the most room (at least 100 GB free): "<drive>:\Try2Create scratch". MV_SCRATCH_DIR names another folder; none
// that fits: Windows' temp, as before.
std::wstring ComfyScratchDir() {
    const std::wstring over = EnvW(L"MV_SCRATCH_DIR");
    if (!over.empty()) return over;
    wchar_t win[MAX_PATH] = {};
    GetWindowsDirectoryW(win, MAX_PATH);
    const wchar_t system = static_cast<wchar_t>(towupper(win[0]));
    std::wstring best;
    ULONGLONG bestFree = 100ull << 30;
    const DWORD drives = GetLogicalDrives();
    for (int k = 2; k < 26; ++k) { // C: onwards
        if (!(drives & (1u << k)) || static_cast<wchar_t>(L'A' + k) == system) continue;
        const std::wstring root = std::wstring(1, static_cast<wchar_t>(L'A' + k)) + L":\\";
        if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED) continue; // (not a USB stick, a network or optical drive)
        ULARGE_INTEGER freeBytes = {};
        if (GetDiskFreeSpaceExW(root.c_str(), &freeBytes, nullptr, nullptr) && freeBytes.QuadPart > bestFree) bestFree = freeBytes.QuadPart, best = root;
    }
    return best.empty() ? std::wstring() : best + L"Try2Create scratch";
}

// Continuum's caches whose ComfyUI is gone ("h3-decode-cache-<pid>-..."): the process that made them can no longer clean them up
void RemoveStaleDecodeCaches(const std::vector<std::wstring>& parents) {
    for (const std::wstring& parent : parents) {
        std::error_code ec;
        if (parent.empty() || !fs::is_directory(parent, ec)) continue;
        for (const auto& e : fs::directory_iterator(parent, ec)) {
            const std::wstring name = e.path().filename().wstring();
            if (!e.is_directory(ec) || name.rfind(L"h3-decode-cache-", 0) != 0) continue;
            const DWORD pid = static_cast<DWORD>(_wtoi(name.c_str() + 16));
            HANDLE ph = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
            bool alive = false;
            if (ph) {
                DWORD code = 0;
                alive = GetExitCodeProcess(ph, &code) && code == STILL_ACTIVE;
                CloseHandle(ph);
            }
            if (!alive) fs::remove_all(e.path(), ec);
        }
    }
}

bool ComfyStart(const ComfySettings& s, std::string& message) {
    if (ComfyOwned()) {
        message = "ComfyUI was already started by this app";
        return true;
    }
    std::lock_guard<std::mutex> lock(g_comfyMutex);
    if (g_comfyProcess) {
        CloseHandle(g_comfyProcess);
        g_comfyProcess = nullptr;
    }
    const bool tie = s.closeWithApp;
    if (tie && !g_comfyJob) {
        g_comfyJob = CreateJobObjectW(nullptr, nullptr);
        if (g_comfyJob) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(g_comfyJob, JobObjectExtendedLimitInformation, &info, sizeof(info));
        }
    }
    auto finish = [&](PROCESS_INFORMATION& pi) {
        if (tie && g_comfyJob) {
            AssignProcessToJobObject(g_comfyJob, pi.hProcess);
            ResumeThread(pi.hThread);
        }
        CloseHandle(pi.hThread);
        g_comfyProcess = pi.hProcess;
        if (s.background) BackgroundTree(pi.dwProcessId);
    };

    // 1. a background server: the same python and arguments ComfyUI Desktop uses, but no window at all
    HeadlessLaunch h;
    if (s.startMode == 0 && FindHeadless(h)) {
        Endpoint ep;
        const int port = ParseEndpoint(s.url, ep) ? ep.port : 8188;
        std::wstring cmd = L"\"" + h.python + L"\" -s " + h.mainPy;
        if (!h.args.empty()) cmd += L" " + ToWide(h.args);
        cmd += L" --listen 127.0.0.1 --port " + std::to_wstring(port);
        // Left alone, dynamic VRAM fills the card to the last 0.7 GB with a big model such as MiniMax H3: steps then stall while Windows
        // shuffles memory (2-4x slower, worse when another app or a game takes some). Keeping a few GB free costs almost nothing.
        if (s.vramHeadroom > 0 && h.args.find("--vram-headroom") == std::string::npos) {
            std::string cli;
            if (ReadFileBytes(h.workDir + L"\\ComfyUI\\comfy\\cli_args.py", cli) && cli.find("\"--vram-headroom\"") != std::string::npos) { // an unknown flag stops ComfyUI
                wchar_t gb[32];
                swprintf(gb, 32, L" --vram-headroom %.1f", s.vramHeadroom);
                cmd += gb;
            }
        }
        // Its RAM-pressure cache keeps models, pinned copies and results until only 10 GB is free: Make it crisp (H3 + its 10 GB LoRA + the
        // 32B text encoder) took a 128 GB PC to 123 GB on 2026-10-28. With more headroom it lets go between nodes, well before Windows pages.
        if (s.ramHeadroom > 0 && h.args.find("--cache-") == std::string::npos) {
            std::string cli;
            if (ReadFileBytes(h.workDir + L"\\ComfyUI\\comfy\\cli_args.py", cli) && cli.find("\"--cache-ram\"") != std::string::npos) {
                MEMORYSTATUSEX ms = {sizeof(ms)};
                GlobalMemoryStatusEx(&ms);
                const double total = ms.ullTotalPhys / 1073741824.0; // (never more than a third of the RAM: a 32 GB PC keeps about 11)
                wchar_t gb[32];
                swprintf(gb, 32, L" --cache-ram %.0f", std::max(2.0, std::min(s.ramHeadroom, total / 3.0)));
                cmd += gb;
            }
        }
        if (!h.yaml.empty()) cmd += L" --extra-model-paths-config \"" + h.yaml + L"\"";
        const std::wstring pixelYaml = PixelPathsYaml(s); // the Retro Diffusion folder as one more checkpoint folder (pixel art)
        if (!pixelYaml.empty()) cmd += L" --extra-model-paths-config \"" + pixelYaml + L"\"";
        const std::wstring testInput = EnvW(L"MV_COMFY_INPUT"); // (for tests: a folder of their own instead of ComfyUI Desktop's)
        if (!testInput.empty()) cmd += L" --input-directory \"" + testInput + L"\"";
        else if (!h.inputDir.empty()) cmd += L" --input-directory \"" + h.inputDir + L"\"";
        const std::wstring outDir = s.outputDir.empty() ? h.outputDir : s.outputDir;
        if (!outDir.empty()) cmd += L" --output-directory \"" + outDir + L"\"";
        // its scratch files on the roomiest drive that is not the Windows one (ComfyScratchDir); left-over Continuum caches go first
        const std::wstring scratch = ComfyScratchDir();
        std::error_code sec;
        if (!scratch.empty()) fs::create_directories(fs::path(scratch) / L"tmp", sec), fs::create_directories(fs::path(scratch) / L"h3", sec);
        const bool useScratch = !scratch.empty() && !sec;
        if (useScratch) cmd += L" --temp-directory \"" + scratch + L"\"";
        std::thread([parents = std::vector<std::wstring>{EnvW(L"TEMP"), useScratch ? scratch + L"\\h3" : std::wstring()}]() { RemoveStaleDecodeCaches(parents); }).detach();
        const std::wstring keepTemp = EnvW(L"TEMP"), keepTmp = EnvW(L"TMP");
        if (useScratch) { // (for ComfyUI only: put back once it is started)
            SetEnvironmentVariableW(L"TEMP", (scratch + L"\\tmp").c_str());
            SetEnvironmentVariableW(L"TMP", (scratch + L"\\tmp").c_str());
            SetEnvironmentVariableW(L"H3_DECODE_CACHE_TEMP_DIR", (scratch + L"\\h3").c_str());
        }
        // its output goes to a file, which Python writes in the ANSI code page unless told otherwise: a custom node printing an emoji then
        // stops ComfyUI while it starts (ComfyUI Desktop sets this for its own server)
        SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");
        SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
        const std::wstring logPath = EnvW(L"LOCALAPPDATA") + L"\\Try2Create\\comfy_server.log";
        std::error_code ec;
        fs::create_directories(fs::path(logPath).parent_path(), ec);
        HANDLE log = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        STARTUPINFOW si = {sizeof(si)};
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdInput = nul;
        si.hStdOutput = log;
        si.hStdError = log;
        PROCESS_INFORMATION pi = {};
        const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | (tie && g_comfyJob ? CREATE_SUSPENDED : 0), nullptr,
                                       h.workDir.c_str(), &si, &pi);
        if (useScratch) {
            SetEnvironmentVariableW(L"TEMP", keepTemp.c_str());
            SetEnvironmentVariableW(L"TMP", keepTmp.c_str());
            SetEnvironmentVariableW(L"H3_DECODE_CACHE_TEMP_DIR", nullptr);
        }
        if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        if (ok) {
            finish(pi);
            message = "Starting ComfyUI as a background server (no window)";
            return true;
        }
    }

    // 2. the ComfyUI Desktop app itself (a window, hidden unless asked to show)
    std::wstring exe = s.desktopExe;
    if (exe.empty() && !ComfyFindDesktop(exe)) {
        message = "I could not find ComfyUI to start. Start it yourself, or enter its program on the Generate page.";
        return false;
    }
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = s.showWindow ? SW_SHOWMINNOACTIVE : SW_HIDE;
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = L"\"" + exe + L"\"";
    const std::wstring dir = fs::path(exe).parent_path().wstring();
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, tie && g_comfyJob ? CREATE_SUSPENDED : 0, nullptr, dir.c_str(), &si, &pi)) {
        message = "Could not start ComfyUI Desktop";
        return false;
    }
    finish(pi);
    message = "Starting the ComfyUI Desktop app";
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
bool LoadComfySettings(ComfySettings& s) {
    std::ifstream f(SettingsPath(), std::ios::binary);
    if (!f) return false;
    json j = json::parse(f, nullptr, false);
    if (!j.is_object()) return false;
    s.url = j.value("url", s.url);
    s.workflowPath = ToWide(j.value("workflow", std::string()));
    s.savedWorkflow = j.value("savedWorkflow", std::string());
    s.autoImport = j.value("autoImport", true);
    s.useLastRun = j.value("useLastRun", s.workflowPath.empty() && s.savedWorkflow.empty());
    s.randomSeed = j.value("randomSeed", s.randomSeed);
    s.sendNegative = j.value("sendNegative", s.sendNegative);
    s.sendPicture = j.value("sendPicture", s.sendPicture);
    s.count = std::clamp(j.value("count", s.count), 1, 16);
    s.videoLength = j.value("videoLength", s.videoLength);
    s.defaultFps = std::clamp(j.value("defaultFps", s.defaultFps), 1, 120);
    s.autoStart = j.value("autoStart", s.autoStart);
    s.closeWithApp = j.value("closeWithApp", s.closeWithApp);
    s.loraTriggers = j.value("loraTriggers", s.loraTriggers);
    s.vramHeadroom = std::clamp(j.value("vramHeadroom", s.vramHeadroom), 0.0, 12.0);
    s.ramHeadroom = std::clamp(j.value("ramHeadroom", s.ramHeadroom), 0.0, 96.0);
    s.background = j.value("background", true);
    s.showWindow = j.value("showWindow", s.showWindow);
    s.startMode = std::clamp(j.value("startMode", s.startMode), 0, 1);
    s.editStyle = std::clamp(j.value("editStyle", s.editStyle), 0, 2);
    s.editJump = j.value("editJump", s.editJump);
    s.directOutfit = j.value("directOutfit", s.directOutfit);
    s.directPose = j.value("directPose", s.directPose);
    s.directExpression = j.value("directExpression", s.directExpression);
    s.editUnet = j.value("editUnet", s.editUnet);
    auto readLoras = [](const json& arr, std::vector<LoraEntry>& out) {
        out.clear();
        if (!arr.is_array()) return;
        for (const json& e : arr) {
            if (!e.is_object()) continue;
            LoraEntry l;
            l.name = e.value("name", std::string());
            l.strength = std::clamp(e.value("strength", 1.0), -2.0, 3.0);
            l.on = e.value("on", true);
            out.push_back(l);
        }
    };
    if (j.contains("editLoras")) readLoras(j["editLoras"], s.editLoras);
    else if (j.contains("editLora")) { // an older settings file: its one LoRA
        s.editLoras.clear();
        s.editLoras.push_back({j.value("editLoraName", std::string("qwen2.1fun.safetensors")), std::clamp(j.value("editLoraStrength", 0.7), 0.0, 2.0), j.value("editLora", true)});
    }
    if (j.contains("promptLoras")) readLoras(j["promptLoras"], s.promptLoras);
    s.promptReplaceLoras = j.value("promptReplaceLoras", s.promptReplaceLoras);
    if (j.contains("upscaleLoras")) readLoras(j["upscaleLoras"], s.upscaleLoras);
    if (j.contains("kreaLoras")) readLoras(j["kreaLoras"], s.kreaLoras);
    if (j.contains("qwenLoras")) readLoras(j["qwenLoras"], s.qwenLoras);
    if (j.contains("mossLoras")) readLoras(j["mossLoras"], s.mossLoras);
    if (j.contains("h3Loras")) readLoras(j["h3Loras"], s.h3Loras);
    s.editSteps = std::clamp(j.value("editSteps", s.editSteps), 1, 200);
    s.editResolution = std::clamp(j.value("editResolution", s.editResolution), 0, 4096);
    s.outputDir = ToWide(j.value("outputDir", std::string()));
    s.desktopExe = ToWide(j.value("desktopExe", std::string()));
    s.pixelModels = ToWide(j.value("pixelModels", std::string()));
    s.promptNode = j.value("promptNode", std::string());
    s.negativeNode = j.value("negativeNode", std::string());
    if (j.value("editModelRev", 0) < 2) { // picture editing moved to Qwen Image 2.1 Turbo (GGUF): switched once, the picker can switch back
        s.editUnet = "qwen_image_2.1_turbo_Q8_0.gguf";
        if (s.editSteps > 10) s.editSteps = 6;
    }
    return true;
}

void SaveComfySettings(const ComfySettings& s) {
    auto lorasJson = [](const std::vector<LoraEntry>& list) {
        json a = json::array();
        for (const LoraEntry& l : list) a.push_back({{"name", l.name}, {"strength", l.strength}, {"on", l.on}});
        return a;
    };
    json j = {{"url", s.url}, {"useLastRun", s.useLastRun}, {"savedWorkflow", s.savedWorkflow}, {"autoImport", s.autoImport}, {"workflow", ToUtf8(s.workflowPath)}, {"randomSeed", s.randomSeed}, {"sendNegative", s.sendNegative},
              {"sendPicture", s.sendPicture}, {"count", s.count}, {"videoLength", s.videoLength}, {"defaultFps", s.defaultFps}, {"autoStart", s.autoStart},
              {"closeWithApp", s.closeWithApp}, {"loraTriggers", s.loraTriggers}, {"vramHeadroom", s.vramHeadroom}, {"ramHeadroom", s.ramHeadroom}, {"background", s.background}, {"showWindow", s.showWindow}, {"startMode", s.startMode}, {"editStyle", s.editStyle}, {"editJump", s.editJump}, {"directOutfit", s.directOutfit}, {"directPose", s.directPose}, {"directExpression", s.directExpression}, {"editUnet", s.editUnet}, {"editModelRev", 2}, {"editLoras", lorasJson(s.editLoras)}, {"promptLoras", lorasJson(s.promptLoras)}, {"promptReplaceLoras", s.promptReplaceLoras}, {"upscaleLoras", lorasJson(s.upscaleLoras)}, {"kreaLoras", lorasJson(s.kreaLoras)}, {"qwenLoras", lorasJson(s.qwenLoras)}, {"mossLoras", lorasJson(s.mossLoras)}, {"h3Loras", lorasJson(s.h3Loras)}, {"editSteps", s.editSteps}, {"editResolution", s.editResolution}, {"outputDir", ToUtf8(s.outputDir)}, {"desktopExe", ToUtf8(s.desktopExe)}, {"pixelModels", ToUtf8(s.pixelModels)}, {"promptNode", s.promptNode}, {"negativeNode", s.negativeNode}};
    std::error_code ec;
    fs::create_directories(fs::path(SettingsPath()).parent_path(), ec);
    std::ofstream out(SettingsPath(), std::ios::binary | std::ios::trunc);
    if (out) out << j.dump(2);
}

ComfyTemplateInfo InspectComfyTemplate(const ComfySettings& s) {
    ComfyTemplateInfo info;
    json wf;
    if (!LoadTemplate(s, wf, info.error)) return info;
    info.nodes = static_cast<int>(wf.size());
    const Targets t = FindTargets(wf, s);
    if (t.positive.empty()) {
        info.error = "I could not find where the prompt goes in this workflow (no text-encode node feeds a sampler). Enter the node id of the prompt "
                     "text under \"If the prompt goes to the wrong place\".";
        return info;
    }
    info.summary = std::string(s.useLastRun ? "workflow: the one ComfyUI ran last\n" : !s.savedWorkflow.empty() ? "workflow: " + s.savedWorkflow + " (converted from the editor format)\n" : "") + "prompt: node " + DescribeNodes(wf, t.positive) + "\nnegative prompt: node " + DescribeNodes(wf, t.negative) + "\nseed: node " +
                   DescribeNodes(wf, t.seeds) + "\ninput picture: node " + DescribeNodes(wf, t.loadImages);
    info.ok = true;
    return info;
}

bool ComfyCheck(const std::string& url, std::string& message) {
    Endpoint ep;
    if (!ParseEndpoint(url, ep)) {
        message = "That is not a web address (try http://127.0.0.1:8188)";
        return false;
    }
    std::string response, error;
    DWORD status = 0;
    if (!Http(ep, L"GET", L"/system_stats", std::string(), nullptr, response, status, 3000, &error)) {
        message = "Could not reach ComfyUI there. Start it (and check the address).";
        return false;
    }
    if (status != 200) {
        message = "Something answered, but not ComfyUI (error " + std::to_string(status) + ")";
        return false;
    }
    json j = json::parse(response, nullptr, false);
    std::string version;
    if (j.is_object() && j.contains("system") && j["system"].is_object()) version = j["system"].value("comfyui_version", std::string());
    message = "Connected to ComfyUI" + (version.empty() ? std::string() : " " + version);
    return true;
}

static std::atomic<bool> g_comfyHoldsDrama{false}; // the running ComfyUI has loaded Drama 3 (as far as this app knows)
static std::atomic<bool> g_forgetDrama{false};   // ComfyUI was restarted from the Drama page: it holds none now

bool ComfyHoldsDrama() { return g_comfyHoldsDrama.load(); }

bool ComfyGiveBackDramaRam(const ComfySettings& settings, std::string& message) {
    if (!ComfyOwned()) {
        message = "This app did not start ComfyUI: close and open it to give Drama 3's RAM back";
        return false;
    }
    ComfyStopOwned();
    Sleep(1500);
    g_forgetDrama = true;
    g_comfyHoldsDrama = false;
    return ComfyStart(settings, message);
}

bool ComfyFreeMemory(const std::string& url, std::string& message) {
    Endpoint ep;
    if (!ParseEndpoint(url, ep)) {
        message = "That is not a web address";
        return false;
    }
    std::string response;
    DWORD status = 0;
    // free_memory also clears ComfyUI's cache: with Drama 3 in it, that makes the next scene load Drama 3 again on top of the copy that
    // stays in RAM (ComfyUI does not give its pinned RAM back). The models still leave the GPU.
    const json body = {{"unload_models", true}, {"free_memory", !g_comfyHoldsDrama.load()}};
    if (!Http(ep, L"POST", L"/free", body.dump(), L"application/json", response, status, 8000, &message)) return false;
    if (status != 200) {
        message = "ComfyUI answered " + std::to_string(status);
        return false;
    }
    message = "ComfyUI is unloading its models";
    return true;
}

// ---------------------------------------------------------------- LoRAs for any workflow
namespace {
std::string LinkId(const json& v) { return v.is_string() ? v.get<std::string>() : v.dump(); }

// [nodeId, slot] pointing at that node's output?
bool IsLinkTo(const json& v, const std::string& id, int slot) { return v.is_array() && v.size() == 2 && LinkId(v[0]) == id && v[1].is_number_integer() && v[1].get<int>() == slot; }

bool IsLoraLoader(const json& node) {
    const std::string cls = node.value("class_type", std::string());
    return cls == "LoraLoaderModelOnly" || cls == "LoraLoader";
}

void ReplaceLinks(json& graph, const std::string& oldId, int oldSlot, const json& replacement, const std::string& exceptNode) {
    for (auto it = graph.begin(); it != graph.end(); ++it) {
        if (it.key() == exceptNode || !it.value().is_object() || !it.value().contains("inputs") || !it.value()["inputs"].is_object()) continue;
        for (auto in = it.value()["inputs"].begin(); in != it.value()["inputs"].end(); ++in) {
            if (IsLinkTo(in.value(), oldId, oldSlot)) in.value() = replacement;
        }
    }
}
} // namespace

// Puts `loras` (in order) on the model that feeds the sampler(s) of an API-format workflow: the chain starts right after the model loader (after any patching nodes are
// followed back), so everything downstream of it gets the LoRAs. With replaceOwn the workflow's own LoRA loaders are taken out first. Returns how many LoRA nodes were added.
int ApplyLoras(json& graph, const std::vector<LoraEntry>& loras, bool replaceOwn) {
    if (!graph.is_object()) return 0;
    if (replaceOwn) { // bypass the existing loaders: whoever used their outputs uses their inputs instead
        std::vector<std::string> gone;
        for (auto it = graph.begin(); it != graph.end(); ++it) {
            if (!it.value().is_object() || !IsLoraLoader(it.value()) || !it.value().contains("inputs")) continue;
            const json& in = it.value()["inputs"];
            if (in.contains("model") && in["model"].is_array()) ReplaceLinks(graph, it.key(), 0, in["model"], it.key());
            if (it.value().value("class_type", std::string()) == "LoraLoader" && in.contains("clip") && in["clip"].is_array()) ReplaceLinks(graph, it.key(), 1, in["clip"], it.key());
            gone.push_back(it.key());
        }
        for (const std::string& id : gone) graph.erase(id);
    }
    std::vector<LoraEntry> on;
    for (const LoraEntry& l : loras) {
        if (l.on && !l.name.empty()) on.push_back(l);
    }
    if (on.empty()) return 0;
    // the model sources behind every sampler (or guider): follow the "model" input back through the nodes that only patch it
    std::vector<std::pair<std::string, int>> sources;
    for (auto it = graph.begin(); it != graph.end(); ++it) {
        if (!it.value().is_object() || !it.value().contains("inputs") || !it.value()["inputs"].is_object()) continue;
        const std::string cls = it.value().value("class_type", std::string());
        if (cls.find("Sampler") == std::string::npos && cls.find("Guider") == std::string::npos) continue;
        const json& in = it.value()["inputs"];
        if (!in.contains("model") || !in["model"].is_array() || in["model"].size() != 2) continue;
        std::string id = LinkId(in["model"][0]);
        int slot = in["model"][1].is_number_integer() ? in["model"][1].get<int>() : 0;
        for (int guard = 0; guard < 64; ++guard) {
            if (!graph.contains(id) || !graph[id].is_object() || !graph[id].contains("inputs")) break;
            const json& pin = graph[id]["inputs"];
            if (!pin.contains("model") || !pin["model"].is_array() || pin["model"].size() != 2) break; // a loader: nothing feeds its model
            slot = pin["model"][1].is_number_integer() ? pin["model"][1].get<int>() : 0;
            id = LinkId(pin["model"][0]);
        }
        if (std::find(sources.begin(), sources.end(), std::make_pair(id, slot)) == sources.end()) sources.push_back({id, slot});
    }
    int added = 0;
    int nextId = 9000;
    for (const auto& src : sources) {
        json prev = json::array({src.first, src.second});
        std::string firstNew;
        for (const LoraEntry& l : on) {
            std::string id;
            do id = std::to_string(nextId++);
            while (graph.contains(id));
            graph[id] = {{"class_type", "LoraLoaderModelOnly"}, {"inputs", {{"model", prev}, {"lora_name", l.name}, {"strength_model", l.strength}}}};
            if (firstNew.empty()) firstNew = id;
            prev = json::array({id, 0});
            ++added;
        }
        ReplaceLinks(graph, src.first, src.second, prev, firstNew); // every user of the model now gets the LoRA'd one (the first new node keeps reading the original)
        // the nodes of the chain after the first must not be rewired back to themselves: they read the previous new node, which ReplaceLinks never matches
    }
    return added;
}

namespace {
bool ListCheckpointsAt(const Endpoint& ep, std::vector<std::string>& out) {
    out.clear();
    std::string response;
    DWORD status = 0;
    if (!Http(ep, L"GET", L"/object_info/CheckpointLoaderSimple", std::string(), nullptr, response, status, 8000, nullptr) || status != 200) return false;
    json j = json::parse(response, nullptr, false);
    if (!j.is_object() || !j.contains("CheckpointLoaderSimple")) return false;
    const json& n = j["CheckpointLoaderSimple"];
    if (!n.contains("input") || !n["input"].contains("required") || !n["input"]["required"].contains("ckpt_name")) return false;
    const json& spec = n["input"]["required"]["ckpt_name"];
    if (!spec.is_array() || spec.empty() || !spec[0].is_array()) return false;
    for (const json& v : spec[0]) {
        if (v.is_string()) out.push_back(v.get<std::string>());
    }
    return true;
}
} // namespace

bool ComfyListCheckpoints(const std::string& url, std::vector<std::string>& out) {
    Endpoint ep;
    return ParseEndpoint(url, ep) && ListCheckpointsAt(ep, out);
}

int PixelModelScale(const std::string& model) {
    std::string l = model;
    for (char& ch : l) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    const size_t slash = l.find_last_of("/\\");
    if (slash != std::string::npos) l = l.substr(slash + 1);
    return l.find("16x") != std::string::npos ? 16 : 8;
}

std::string PickPixelModel(const std::vector<std::string>& checkpoints, int artSize) {
    auto low = [](std::string v) {
        for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return v;
    };
    std::string m16, m64, m128;
    for (const std::string& name : checkpoints) {
        const std::string l = low(name);
        if (l.find("retrodiffusion") == std::string::npos) continue;
        if (l.find("128x") != std::string::npos) m128 = name;
        else if (l.find("24x-64x") != std::string::npos || l.find("64x") != std::string::npos) m64 = name;
        else if (l.find("16x") != std::string::npos) m16 = name;
    }
    // icons up to 20 pixels: the 16x model; sprites up to 80: the 24x-64x one; bigger pictures and scenes: the 128x one (each falls back on the others)
    const std::vector<std::string> order = artSize <= 20 ? std::vector<std::string>{m16, m64, m128} : artSize <= 80 ? std::vector<std::string>{m64, m128, m16} : std::vector<std::string>{m128, m64, m16};
    for (const std::string& m : order) {
        if (!m.empty()) return m;
    }
    return {};
}

bool ComfyHasDrawWorkflow(const ComfySettings& s) { return !s.useLastRun && (!s.savedWorkflow.empty() || !s.workflowPath.empty()); }

std::string ComfyDrawWorkflowName(const ComfySettings& s) {
    std::string name = !s.savedWorkflow.empty() ? s.savedWorkflow : ToUtf8(fs::path(s.workflowPath).filename().wstring());
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    if (name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0) name.resize(name.size() - 5);
    return name;
}

std::vector<std::string> ComfyListLoras(const std::string& url) {
    std::vector<std::string> out;
    Endpoint ep;
    if (!ParseEndpoint(url, ep)) return out;
    std::string response;
    DWORD status = 0;
    if (!Http(ep, L"GET", L"/object_info/LoraLoaderModelOnly", std::string(), nullptr, response, status, 5000, nullptr) || status != 200) return out;
    json j = json::parse(response, nullptr, false);
    if (!j.is_object() || !j.contains("LoraLoaderModelOnly")) return out;
    const json& n = j["LoraLoaderModelOnly"];
    if (!n.contains("input") || !n["input"].contains("required") || !n["input"]["required"].contains("lora_name")) return out;
    const json& spec = n["input"]["required"]["lora_name"];
    if (!spec.is_array() || spec.empty()) return out;
    // the names: the first item (a classic node), or {"options": [...]} after "COMBO" (a node on ComfyUI's newer io API)
    const json* names = spec[0].is_array() ? &spec[0] : nullptr;
    if (!names && spec.size() > 1 && spec[1].is_object() && spec[1].contains("options") && spec[1]["options"].is_array()) names = &spec[1]["options"];
    if (!names) return out;
    for (const json& v : *names) {
        if (v.is_string()) out.push_back(v.get<std::string>());
    }
    return out;
}

std::string LoraFileName(const std::string& name) {
    const size_t at = name.find_last_of("/\\");
    std::string f = at == std::string::npos ? name : name.substr(at + 1);
    for (char& c : f) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return f;
}

bool LoraListed(const std::vector<std::string>& list, const std::string& name) {
    if (std::find(list.begin(), list.end(), name) != list.end()) return true;
    const std::string f = LoraFileName(name);
    for (const std::string& l : list) {
        if (LoraFileName(l) == f) return true;
    }
    return false;
}

// Every lora_name of a graph as ComfyUI lists it now: a LoRA that was sorted into a folder ("Krea 2\x.safetensors") is still found when the
// graph names it without the folder (the app's own graphs, a workflow saved before the sort), and the other way round. Asked at most every 15 s.
int FixLoraNames(const Endpoint& ep, json& wf) {
    if (!wf.is_object()) return 0;
    bool any = false;
    for (auto& [id, n] : wf.items()) {
        if (n.is_object() && n.contains("inputs") && n["inputs"].is_object() && n["inputs"].contains("lora_name") && n["inputs"]["lora_name"].is_string()) any = true;
    }
    if (!any) return 0;
    static std::mutex m;
    static std::vector<std::string> list;
    static ULONGLONG at = 0;
    std::vector<std::string> have;
    {
        std::lock_guard<std::mutex> lock(m);
        if (!at || GetTickCount64() - at > 15000) {
            std::string response;
            DWORD status = 0;
            if (Http(ep, L"GET", L"/object_info/LoraLoaderModelOnly", std::string(), nullptr, response, status, 5000, nullptr) && status == 200) {
                const json j = json::parse(response, nullptr, false);
                std::vector<std::string> fresh;
                if (j.is_object() && j.contains("LoraLoaderModelOnly")) {
                    const json& spec = j["LoraLoaderModelOnly"]["input"]["required"]["lora_name"];
                    if (spec.is_array() && !spec.empty() && spec[0].is_array()) {
                        for (const json& v : spec[0]) {
                            if (v.is_string()) fresh.push_back(v.get<std::string>());
                        }
                    }
                }
                if (!fresh.empty()) list = std::move(fresh), at = GetTickCount64();
            }
        }
        have = list;
    }
    if (have.empty()) return 0;
    int fixed = 0;
    for (auto& [id, n] : wf.items()) {
        if (!n.is_object() || !n.contains("inputs") || !n["inputs"].is_object() || !n["inputs"].contains("lora_name") || !n["inputs"]["lora_name"].is_string()) continue;
        const std::string name = n["inputs"]["lora_name"].get<std::string>();
        if (name.empty() || name == "None" || std::find(have.begin(), have.end(), name) != have.end()) continue;
        const std::string f = LoraFileName(name);
        for (const std::string& h : have) {
            if (LoraFileName(h) == f) {
                n["inputs"]["lora_name"] = h;
                ++fixed;
                break;
            }
        }
    }
    return fixed;
}

// The choices of one input of one node (a model list, a CLIPLoader's types...): empty when ComfyUI cannot be reached or has no such node
std::vector<std::string> ComfyListChoices(const std::string& url, const std::string& node, const std::string& input) {
    std::vector<std::string> out;
    Endpoint ep;
    if (!ParseEndpoint(url, ep)) return out;
    std::string response;
    DWORD status = 0;
    const std::wstring path = L"/object_info/" + std::wstring(node.begin(), node.end());
    if (!Http(ep, L"GET", path, std::string(), nullptr, response, status, 5000, nullptr) || status != 200) return out;
    const json j = json::parse(response, nullptr, false);
    if (!j.is_object() || !j.contains(node) || !j[node].contains("input")) return out;
    for (const char* group : {"required", "optional"}) {
        if (!j[node]["input"].contains(group) || !j[node]["input"][group].contains(input)) continue;
        const json& spec = j[node]["input"][group][input];
        if (!spec.is_array() || spec.empty() || !spec[0].is_array()) continue;
        for (const json& v : spec[0]) {
            if (v.is_string()) out.push_back(v.get<std::string>());
        }
    }
    return out;
}

ComfyVeda ComfyAskVeda(const std::string& url);

// Takes out the model patches this ComfyUI cannot run - VedaSparseAttention without its node or predictor, FizgigH3Tweaks without its node:
// whatever used one gets its model input. True = a Veda node went (the job's label then loses ", Veda").
bool DropMissingVeda(const std::string& url, json& wf) {
    std::vector<std::string> ids;
    for (const auto& [id, n] : wf.items()) {
        const std::string cls = n.is_object() ? n.value("class_type", std::string()) : std::string();
        if (cls == "VedaSparseAttention" || cls == "FizgigH3Tweaks") ids.push_back(id);
    }
    if (ids.empty()) return false;
    const ComfyVeda v = ComfyAskVeda(url);
    bool dropped = false;
    for (const std::string& id : ids) {
        const json& in = wf[id]["inputs"];
        const bool fizgig = wf[id].value("class_type", std::string()) == "FizgigH3Tweaks";
        const std::string predictor = in.value("predictor", std::string());
        if (fizgig ? v.fizgig : v.node && std::find(v.predictors.begin(), v.predictors.end(), predictor) != v.predictors.end()) continue;
        const json model = in.contains("model") ? in["model"] : json();
        for (auto& [other, n] : wf.items()) {
            if (other == id || !n.is_object() || !n.contains("inputs")) continue;
            for (auto& [key, link] : n["inputs"].items()) {
                if (link.is_array() && link.size() == 2 && link[0].is_string() && link[0].get<std::string>() == id) link = model;
            }
        }
        wf.erase(id);
        if (!fizgig) dropped = true;
    }
    return dropped;
}

bool ComfyReadProgress(ComfyProgress& out) {
    static ComfyProgress last;
    static bool lastOk = false;
    static unsigned long long readAt = 0;
    const unsigned long long now = GetTickCount64();
    if (now - readAt < 500) {
        out = last;
        return lastOk;
    }
    readAt = now;
    lastOk = false;
    const std::wstring path = EnvW(L"LOCALAPPDATA") + L"\\Try2Create\\comfy_server.log";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    const LONGLONG want = std::min<LONGLONG>(size.QuadPart, 6000);
    LARGE_INTEGER pos{};
    pos.QuadPart = size.QuadPart - want;
    SetFilePointerEx(f, pos, nullptr, FILE_BEGIN);
    std::string tail(static_cast<size_t>(want), '\0');
    DWORD got = 0;
    ReadFile(f, tail.data(), static_cast<DWORD>(want), &got, nullptr);
    CloseHandle(f);
    tail.resize(got);
    // the last bar: "| a/b [mm:ss<mm:ss, X s/it]" or "..., X it/s]"
    const size_t bar = tail.rfind("it]");
    if (bar == std::string::npos) return false;
    const size_t open = tail.rfind('[', bar);
    const size_t pipe = open == std::string::npos ? std::string::npos : tail.rfind('|', open);
    if (pipe == std::string::npos || bar - pipe > 80) return false;
    int a = 0, b = 0;
    if (sscanf(tail.c_str() + pipe + 1, " %d/%d", &a, &b) != 2 || b <= 0) return false;
    auto clock = [](const std::string& s) { // "07:46" or "1:02:03"
        int h = 0, m = 0, sec = 0;
        if (sscanf(s.c_str(), "%d:%d:%d", &h, &m, &sec) == 3) return h * 3600.0 + m * 60.0 + sec;
        if (sscanf(s.c_str(), "%d:%d", &m, &sec) == 2) return m * 60.0 + sec;
        return 0.0;
    };
    const std::string inside = tail.substr(open + 1, bar - open - 1); // "10:12<07:46, 466.90s/" or "00:05<00:10,  1.50"
    const size_t lt = inside.find('<'), comma = inside.find(',');
    ComfyProgress p;
    p.step = a;
    p.steps = b;
    if (lt != std::string::npos && comma != std::string::npos && comma > lt) p.leftSeconds = clock(inside.substr(lt + 1, comma - lt - 1));
    if (comma != std::string::npos) {
        const double rate = std::atof(inside.c_str() + comma + 1);
        const bool perStep = tail.compare(bar - 2, 2, "s/") == 0; // "s/it]" against "it/s]"
        p.secPerStep = perStep ? rate : rate > 0 ? 1.0 / rate : 0.0;
    }
    last = p;
    lastOk = true;
    out = p;
    return true;
}

double FreeRamGB() {
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    return GlobalMemoryStatusEx(&ms) ? ms.ullAvailPhys / 1073741824.0 : 0.0;
}

ComfyVeda ComfyAskVeda(const std::string& url) {
    ComfyVeda v;
    Endpoint ep;
    if (!ParseEndpoint(url, ep)) return v;
    std::string response;
    DWORD status = 0;
    if (!Http(ep, L"GET", L"/object_info/VedaSparseAttention", std::string(), nullptr, response, status, 5000, nullptr)) return v;
    v.reachable = true;
    {
        std::string fz;
        DWORD fzStatus = 0;
        if (Http(ep, L"GET", L"/object_info/FizgigH3Tweaks", std::string(), nullptr, fz, fzStatus, 5000, nullptr) && fzStatus == 200) {
            const json f = json::parse(fz, nullptr, false);
            v.fizgig = f.is_object() && f.contains("FizgigH3Tweaks");
        }
        std::string ct;
        DWORD ctStatus = 0;
        if (Http(ep, L"GET", L"/object_info/H3ContinuumSamplerV39", std::string(), nullptr, ct, ctStatus, 5000, nullptr) && ctStatus == 200) {
            const json c = json::parse(ct, nullptr, false);
            v.continuum = c.is_object() && c.contains("H3ContinuumSamplerV39");
        }
        std::string mv;
        DWORD mvStatus = 0;
        if (Http(ep, L"GET", L"/object_info/MVEx_MaskToLatentSpace", std::string(), nullptr, mv, mvStatus, 5000, nullptr) && mvStatus == 200) {
            const json m = json::parse(mv, nullptr, false);
            v.maskvid = m.is_object() && m.contains("MVEx_MaskToLatentSpace");
        }
        std::string lp;
        DWORD lpStatus = 0;
        if (Http(ep, L"GET", L"/object_info/LivePortraitProcess", std::string(), nullptr, lp, lpStatus, 5000, nullptr) && lpStatus == 200) {
            const json l = json::parse(lp, nullptr, false);
            v.liveportrait = l.is_object() && l.contains("LivePortraitProcess");
        }
        std::string ck;
        DWORD ckStatus = 0;
        if (Http(ep, L"GET", L"/object_info/CheckpointLoaderSimple", std::string(), nullptr, ck, ckStatus, 5000, nullptr) && ckStatus == 200) {
            const json c = json::parse(ck, nullptr, false);
            if (c.is_object() && c.contains("CheckpointLoaderSimple")) {
                const json& names = c["CheckpointLoaderSimple"]["input"]["required"]["ckpt_name"][0];
                if (names.is_array()) {
                    for (const json& nm : names) { // sam3.1_multiplex_fp16 first, else any SAM 3
                        const std::string s = nm.is_string() ? nm.get<std::string>() : std::string();
                        std::string low = s;
                        for (char& ch : low) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
                        if (low.find("sam3") != std::string::npos && (v.sam3.empty() || low.find("multiplex") != std::string::npos)) v.sam3 = s;
                    }
                }
            }
        }
    }
    const json j = json::parse(response, nullptr, false); // {} when the node is not installed
    v.node = status == 200 && j.is_object() && j.contains("VedaSparseAttention");
    if (!v.node) return v;
    response.clear();
    if (Http(ep, L"GET", L"/models/veda", std::string(), nullptr, response, status, 5000, nullptr) && status == 200) {
        const json files = json::parse(response, nullptr, false);
        if (files.is_array()) {
            for (const json& f : files) {
                if (f.is_string()) v.predictors.push_back(f.get<std::string>());
            }
        }
    }
    return v;
}

// ---------------------------------------------------------------- picture editing: Qwen Image 2.1 Edit
namespace {

// One of the files a loader offers: the first of `preferred` it has, else the first containing one of `contains` (empty = none)
std::string PickChoice(const nlohmann::ordered_json& info, const char* node, const char* input, const std::vector<std::string>& preferred, const std::vector<std::string>& contains) {
    if (!info.contains(node)) return {};
    const auto& n = info[node];
    if (!n.contains("input") || !n["input"].contains("required") || !n["input"]["required"].contains(input)) return {};
    const auto& spec = n["input"]["required"][input];
    if (!spec.is_array() || spec.empty() || !spec[0].is_array()) return {};
    std::vector<std::string> have;
    for (const auto& v : spec[0]) {
        if (v.is_string()) have.push_back(v.get<std::string>());
    }
    for (const std::string& p : preferred) {
        if (std::find(have.begin(), have.end(), p) != have.end()) return p;
    }
    for (const std::string& p : preferred) { // in a subfolder now (the LoRA sorter puts them in one per model)
        for (const std::string& v : have) {
            if (LoraFileName(v) == LoraFileName(p)) return v;
        }
    }
    for (const std::string& part : contains) {
        for (const std::string& v : have) {
            if (v.find(part) != std::string::npos) return v;
        }
    }
    return {};
}

// The Qwen Image 2.1 Edit workflow as an API graph: up to 10 pictures (image_1 is edited, the rest are references), the instruction, a new seed.
// The same nodes as ComfyUI's template (text encoder with the pictures, int8 cache, KSampler euler / simple, cfg 1), without the optional
// prompt enhancer.
bool BuildQwenEdit(const Endpoint& ep, const ComfySettings& s, const std::vector<std::string>& names, const std::string& prompt, long long seed, int canvas, json& out, std::string& error) {
    nlohmann::ordered_json info;
    if (!LoadObjectInfo(ep, info, error)) return false;
    if (!info.contains("TextEncodeQwenImage21")) {
        error = "This ComfyUI does not have the Qwen Image 2.1 nodes yet. Update ComfyUI (ComfyUI Desktop updates itself; with the Manager: Update ComfyUI) and send again.";
        return false;
    }
    std::string unet;
    const bool gguf = s.editUnet.size() > 5 && (s.editUnet.compare(s.editUnet.size() - 5, 5, ".gguf") == 0 || s.editUnet.compare(s.editUnet.size() - 5, 5, ".GGUF") == 0);
    if (gguf && !info.contains("UnetLoaderGGUF")) {
        error = "The model " + s.editUnet + " is a GGUF file, and ComfyUI does not have the GGUF loader. Install ComfyUI-GGUF (ComfyUI Manager > Custom Nodes > search \"GGUF\" by city96), restart ComfyUI and send again.";
        return false;
    }
    if (!s.editUnet.empty()) { // the model chosen in Options (Qwen Image 2.1 Turbo Q8 GGUF by default)
        unet = PickChoice(info, gguf ? "UnetLoaderGGUF" : "UNETLoader", "unet_name", {s.editUnet}, {});
        if (unet.empty()) {
            error = "ComfyUI does not list the model " + s.editUnet + " (it belongs in models/diffusion_models; ComfyUI may need a restart to see new files). To use the standard Qwen Image 2.1 model, clear the model name on the Generate page > Options.";
            return false;
        }
    } else {
        unet = PickChoice(info, "UNETLoader", "unet_name", {"qwen-image-2.1-UC-int8_convrot.safetensors", "qwen_image_2.1_int8_convrot.safetensors", "qwen_image_2.1_bf16.safetensors"},
                          {"qwen-image-2.1", "qwen_image_2.1", "qwen_image_2_1"});
    }
    const std::string clip = PickChoice(info, "CLIPLoader", "clip_name", {"qwen3vl_8b_int8_convrot.safetensors", "qwen3vl_8b_bf16.safetensors"}, {"qwen3vl_8b"});
    const std::string vae = PickChoice(info, "VAELoader", "vae_name", {"qwen_image_2.1_vae_bf16.safetensors"}, {"qwen_image_2.1_vae"});
    std::string missing;
    if (unet.empty()) missing += "the Qwen Image 2.1 diffusion model (for example qwen_image_2.1_int8_convrot.safetensors in models/diffusion_models)";
    if (clip.empty()) missing += std::string(missing.empty() ? "" : ", ") + "the text encoder qwen3vl_8b_int8_convrot.safetensors (models/text_encoders)";
    if (vae.empty()) missing += std::string(missing.empty() ? "" : ", ") + "the VAE qwen_image_2.1_vae_bf16.safetensors (models/vae)";
    if (!missing.empty()) {
        error = "ComfyUI does not have " + missing + ". If it is still downloading, send again when it has finished (ComfyUI may need a restart to see new files).";
        return false;
    }
    out = json::object();
    if (gguf) out["1"] = {{"class_type", "UnetLoaderGGUF"}, {"inputs", {{"unet_name", unet}}}};
    else out["1"] = {{"class_type", "UNETLoader"}, {"inputs", {{"unet_name", unet}, {"weight_dtype", "default"}}}};
    out["2"] = {{"class_type", "CLIPLoader"}, {"inputs", {{"clip_name", clip}, {"type", "qwen_image"}, {"device", "default"}}}};
    out["3"] = {{"class_type", "VAELoader"}, {"inputs", {{"vae_name", vae}}}};
    std::string modelNode = "1";
    int loraId = 50;
    for (const LoraEntry& l : s.editLoras) { // the LoRAs on the model, before the cache (the strength is the model's: the text encoder is left alone)
        if (!l.on || l.name.empty()) continue;
        const std::string listed = PickChoice(info, "LoraLoaderModelOnly", "lora_name", {l.name}, {});
        if (listed.empty()) {
            error = "ComfyUI does not list the LoRA " + l.name + " (it belongs in models/loras; ComfyUI may need a restart to see new files). "
                    "To edit without it, untick or remove it on the Generate page > Options > LoRAs.";
            return false;
        }
        const std::string id = std::to_string(loraId++);
        out[id] = {{"class_type", "LoraLoaderModelOnly"}, {"inputs", {{"model", json::array({modelNode, 0})}, {"lora_name", listed}, {"strength_model", l.strength}}}};
        modelNode = id;
    }
    if (info.contains("QwenImage21Cache")) { // keeps the pictures' encoding in spare VRAM / RAM between steps
        out["4"] = {{"class_type", "QwenImage21Cache"}, {"inputs", {{"model", json::array({modelNode, 0})}, {"device", "auto"}, {"dtype", "default"}}}};
        modelNode = "4";
    }
    json enc = {{"clip", json::array({"2", 0})}, {"vae", json::array({"3", 0})}, {"prompt", prompt}, {"negative_prompt", ""}, {"resolution", s.editResolution}};
    for (size_t i = 0; i < names.size() && i < 10; ++i) {
        const std::string id = std::to_string(10 + i);
        out[id] = {{"class_type", "LoadImage"}, {"inputs", {{"image", names[i]}}}};
        enc["images.image_" + std::to_string(i + 1)] = json::array({id, 0});
    }
    out["20"] = {{"class_type", "TextEncodeQwenImage21"}, {"inputs", enc}};
    // a new canvas (several characters in one scene): the sampler starts from an empty picture of the chosen shape, so the result is not tied to the
    // frame of the first reference (which is what makes characters come out as two photos side by side or a blend)
    json latent = json::array({"20", 2});
    if (canvas >= 1 && canvas <= 6) {
        const double pixels = std::pow(static_cast<double>(std::max(512, s.editResolution == 0 ? 1024 : s.editResolution)), 2.0);
        static const double aspects[] = {1.0, 3.0 / 2.0, 1.0, 2.0 / 3.0, 16.0 / 9.0, 9.0 / 16.0, 21.0 / 9.0}; // like the first picture, landscape, square, portrait, 16:9, 9:16, 21:9
        const double aspect = aspects[canvas];
        auto round32 = [](double v) { return std::max(512, static_cast<int>(std::lround(v / 32.0)) * 32); };
        out["21"] = {{"class_type", "EmptyLatentImage"}, {"inputs", {{"width", round32(std::sqrt(pixels * aspect))}, {"height", round32(std::sqrt(pixels / aspect))}, {"batch_size", 1}}}};
        latent = json::array({"21", 0});
    }
    out["30"] = {{"class_type", "KSampler"},
                 {"inputs", {{"model", json::array({modelNode, 0})}, {"seed", seed}, {"steps", s.editSteps}, {"cfg", 1.0}, {"sampler_name", "euler"}, {"scheduler", "simple"},
                             {"positive", json::array({"20", 0})}, {"negative", json::array({"20", 1})}, {"latent_image", latent}, {"denoise", 1.0}}}};
    out["31"] = {{"class_type", "VAEDecode"}, {"inputs", {{"samples", json::array({"30", 0})}, {"vae", json::array({"3", 0})}}}};
    out["32"] = {{"class_type", "SaveImage"}, {"inputs", {{"images", json::array({"31", 0})}, {"filename_prefix", "Qwen_image_2.1"}}}};
    return true;
}

// The size a strong model draws pixel art at: about one megapixel, in the shape of the art (multiples of 16).
void DrawSize(int artW, int artH, int& w, int& h) {
    const double aspect = static_cast<double>(std::max(1, artW)) / std::max(1, artH);
    w = std::clamp(static_cast<int>(std::lround(std::sqrt(1048576.0 * aspect) / 16.0)) * 16, 256, 2048);
    h = std::clamp(static_cast<int>(std::lround(std::sqrt(1048576.0 / aspect) / 16.0)) * 16, 256, 2048);
}

// Every empty-latent node of a workflow gets this size (links from size helpers such as a resolution selector are replaced by numbers).
void SetLatentSize(json& wf, int w, int h) {
    for (auto it = wf.begin(); it != wf.end(); ++it) {
        if (!it.value().is_object() || !it.value().contains("inputs")) continue;
        const std::string type = it.value().value("class_type", std::string());
        if (type.find("Empty") == std::string::npos || type.find("Latent") == std::string::npos) continue;
        json& in = it.value()["inputs"];
        if (in.contains("width")) in["width"] = w;
        if (in.contains("height")) in["height"] = h;
    }
}

// ---- pixel art: a Retro Diffusion checkpoint (Stable Diffusion 1.5) at 8x the art size, so that every art pixel is one 8 x 8 cell of the latent grid; the app
// turns the result into real pixel art (one pixel per art pixel, a small palette) afterwards. 1 = from the prompt; 2 = redraw a picture (uploaded[0], a clean pixel
// grid at 8x); 3 = redraw only the white part of a mask (uploaded[1]): the seam pass that makes a tile repeat without a visible edge.
bool BuildPixel(const Endpoint& ep, const ComfyJob& job, const std::vector<std::string>& uploaded, long long seed, json& out, std::string& error, std::string& engine) {
    const int w = std::clamp(job.pixW, 8, 256), h = std::clamp(job.pixH, 8, 256);
    std::vector<std::string> checkpoints;
    if (!ListCheckpointsAt(ep, checkpoints)) {
        error = "Could not read ComfyUI's checkpoint list";
        return false;
    }
    const std::string model = PickPixelModel(checkpoints, std::max(w, h));
    if (model.empty()) {
        error = "ComfyUI has no Retro Diffusion model. In the Pixel art tab press \"Link them into ComfyUI\" (or copy the RetroDiffusion*.safetensors files into ComfyUI's models\\checkpoints folder), then send again.";
        return false;
    }
    if (job.pixel >= 2 && uploaded.empty()) {
        error = "The picture to redraw was not uploaded";
        return false;
    }
    if (job.pixel == 3 && uploaded.size() < 2) {
        error = "The mask of the seam pass was not uploaded";
        return false;
    }
    const std::string negative = job.negative.empty() ? std::string("blurry, photo, photograph, 3d render, text, watermark, signature, border, frame") : job.negative;
    out = json::object();
    out["1"] = {{"class_type", "CheckpointLoaderSimple"}, {"inputs", {{"ckpt_name", model}}}};
    out["2"] = {{"class_type", "CLIPTextEncode"}, {"inputs", {{"clip", json::array({"1", 1})}, {"text", job.prompt}}}};
    out["3"] = {{"class_type", "CLIPTextEncode"}, {"inputs", {{"clip", json::array({"1", 1})}, {"text", negative}}}};
    std::string latent;
    if (job.pixel == 1) {
        const int scale = PixelModelScale(model); // the 16x model draws 16 x 16 per art pixel
        out["4"] = {{"class_type", "EmptyLatentImage"}, {"inputs", {{"width", w * scale}, {"height", h * scale}, {"batch_size", 1}}}};
        latent = "4";
    } else {
        out["10"] = {{"class_type", "LoadImage"}, {"inputs", {{"image", uploaded[0]}}}};
        out["11"] = {{"class_type", "VAEEncode"}, {"inputs", {{"pixels", json::array({"10", 0})}, {"vae", json::array({"1", 2})}}}};
        latent = "11";
        if (job.pixel == 3) {
            out["12"] = {{"class_type", "LoadImageMask"}, {"inputs", {{"image", uploaded[1]}, {"channel", "red"}}}};
            out["13"] = {{"class_type", "SetLatentNoiseMask"}, {"inputs", {{"samples", json::array({"11", 0})}, {"mask", json::array({"12", 0})}}}};
            latent = "13";
        }
    }
    out["5"] = {{"class_type", "KSampler"},
                {"inputs", {{"model", json::array({"1", 0})}, {"positive", json::array({"2", 0})}, {"negative", json::array({"3", 0})}, {"latent_image", json::array({latent, 0})},
                            {"seed", seed}, {"steps", job.steps > 0 ? std::clamp(job.steps, 4, 80) : 20}, {"cfg", std::clamp(job.cfg, 1.0, 20.0)}, {"sampler_name", "euler_ancestral"},
                            {"scheduler", "normal"}, {"denoise", job.pixel == 1 ? 1.0 : std::clamp(job.denoise, 0.05, 1.0)}}}};
    out["6"] = {{"class_type", "VAEDecode"}, {"inputs", {{"samples", json::array({"5", 0})}, {"vae", json::array({"1", 2})}}}};
    out["7"] = {{"class_type", "SaveImage"}, {"inputs", {{"images", json::array({"6", 0})}, {"filename_prefix", "PixelArt/rd"}}}};
    std::string shortName = model;
    const size_t slash = shortName.find_last_of("/\\");
    if (slash != std::string::npos) shortName = shortName.substr(slash + 1);
    engine = "Retro Diffusion (" + shortName + "), " + std::to_string(w) + " x " + std::to_string(h) + " pixels";
    return true;
}

// Pixel art drawn first by Qwen Image 2.1: text to image (the edit graph with no pictures, without the edit LoRAs), saved under PixelArt.
bool BuildQwenDraw(const Endpoint& ep, const ComfySettings& s, const ComfyJob& job, long long seed, json& out, std::string& error, std::string& engine) {
    ComfySettings plain = s;
    plain.editLoras.clear();
    plain.editResolution = 1024;
    if (!BuildQwenEdit(ep, plain, {}, job.prompt, seed, 2, out, error)) return false;
    int w = 1024, h = 1024;
    DrawSize(job.pixW, job.pixH, w, h);
    if (out.contains("21")) {
        out["21"]["inputs"]["width"] = w;
        out["21"]["inputs"]["height"] = h;
    }
    if (out.contains("32")) out["32"]["inputs"]["filename_prefix"] = job.draw == 3 && !job.savePrefix.empty() ? job.savePrefix : std::string("PixelArt/drawn");
    engine = "Qwen Image 2.1 draws it (" + std::to_string(w) + " x " + std::to_string(h) + ")" + std::string(job.draw == 3 ? "" : ", then it becomes pixel art");
    return true;
}

// ---- upscaling: the whole picture, or its faces. Resize with an ESRGAN-type model from models/upscale_models when ComfyUI has one that suits the
// picture (photo or anime), otherwise Lanczos; then, when asked, a low-denoise pass of MOSS-Image that redraws the detail (everywhere, or only where
// SAM 3 finds faces). The LoRA of the edit workflow is not used here.
std::string LowerAscii(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

// The upscale model that suits the picture best ("" = ComfyUI has none) and how many times it enlarges (from its name: 2x_..., ..._x4plus, 8x...)
std::string PickUpscaleModel(const nlohmann::ordered_json& info, bool anime, int& native) {
    native = 4;
    if (!info.contains("UpscaleModelLoader") || !info.contains("ImageUpscaleWithModel")) return {};
    const auto& n = info["UpscaleModelLoader"];
    if (!n.contains("input") || !n["input"].contains("required") || !n["input"]["required"].contains("model_name")) return {};
    const auto& spec = n["input"]["required"]["model_name"];
    if (!spec.is_array() || spec.empty() || !spec[0].is_array()) return {};
    std::string best;
    int bestScore = -1;
    for (const auto& v : spec[0]) {
        if (!v.is_string()) continue;
        const std::string name = v.get<std::string>(), low = LowerAscii(name);
        const bool drawn = low.find("anime") != std::string::npos || low.find("manga") != std::string::npos || low.find("lollypop") != std::string::npos || low.find("illustration") != std::string::npos;
        int score = 1;
        if (anime) {
            if (drawn) score += 100;
            if (low.find("animesharp") != std::string::npos) score += 10;
        } else {
            if (drawn) score -= 1; // a photo is better served by a photo model, but an anime one is still better than a stretch
            if (low.find("ultrasharp") != std::string::npos) score += 100;
            else if (low.find("realesrgan_x4plus") != std::string::npos || low.find("real-esrgan") != std::string::npos) score += 90;
            else if (low.find("siax") != std::string::npos) score += 80;
            else if (low.find("remacri") != std::string::npos) score += 70;
            else if (low.find("clearreality") != std::string::npos || low.find("nmkd") != std::string::npos) score += 60;
        }
        if (score > bestScore) {
            bestScore = score;
            best = name;
            native = (low.find("2x") != std::string::npos || low.find("x2") != std::string::npos) ? 2 : (low.find("8x") != std::string::npos || low.find("x8") != std::string::npos) ? 8 : 4;
        }
    }
    return best;
}

// MOSS draws from a description (it does not see the picture), so: the picture's own prompt when it has one (LoRA tags and weights taken out),
// then what a sharp version of it looks like
std::string UpscalePrompt(bool faces, bool anime, const std::string& own) {
    std::string subject = std::regex_replace(own, std::regex(R"(<lora:[^>]*>)", std::regex::icase), "");
    subject = std::regex_replace(subject, std::regex(R"(\(([^()]*):[0-9.]+\))"), "$1"); // "(red hair:1.2)" -> "red hair"
    subject = std::regex_replace(subject, std::regex(R"(\s+)"), " ");
    while (!subject.empty() && (subject.back() == ' ' || subject.back() == ',' || subject.back() == '.')) subject.pop_back();
    while (!subject.empty() && (subject.front() == ' ' || subject.front() == ',')) subject.erase(subject.begin());
    if (subject.size() > 1200) subject.resize(1200);
    std::string look;
    if (faces && anime)
        look = "Clean, sharp faces: crisp consistent line art, eyes with clear highlights and detailed irises, clean mouth and eyebrow lines, tidy hair strands at the "
               "hairline, the same drawn anime art style, not realistic, not 3D.";
    else if (faces)
        look = "Sharp, detailed faces: clear eyes with natural catchlights, natural skin texture with pores and fine lines, no plastic or over-smoothed skin, clean lips "
               "and teeth, individual eyebrow hairs and hair strands at the hairline.";
    else if (anime)
        look = "A clean high-resolution anime illustration: crisp consistent line art, clean flat colour and smooth shading, no JPEG artifacts, noise, blur or halos.";
    else
        look = "A sharp, high-resolution photograph full of real fine detail: natural skin texture, individual hairs, fabric weave, sharp eyes, readable small text, "
               "no noise, blur or compression artifacts, no plastic or over-smoothed look.";
    return subject.empty() ? look : subject + ". " + look;
}

// An upscaling job as an API graph. `engine` says what is done (shown with the job); false + `error` when it cannot be.
// SAM 3 (ComfyUI's own nodes and the sam3.1 checkpoint): the mask of what the prompt names, white = kept.
bool BuildCutout(const Endpoint& ep, const ComfyJob& job, const std::vector<std::string>& uploaded, json& out, std::string& error, std::string& engine) {
    nlohmann::ordered_json info;
    if (!LoadObjectInfo(ep, info, error)) return false;
    if (!info.contains("SAM3_Detect")) {
        error = "This ComfyUI does not have the SAM 3 nodes yet (update ComfyUI): they cut people out of photos for a see-through background.";
        return false;
    }
    std::vector<std::string> checkpoints;
    if (!ListCheckpointsAt(ep, checkpoints)) {
        error = "Could not read ComfyUI's checkpoint list";
        return false;
    }
    std::string sam;
    for (const std::string& name : checkpoints) {
        std::string l = name;
        std::transform(l.begin(), l.end(), l.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (l.find("sam3") != std::string::npos) {
            sam = name;
            break;
        }
    }
    if (sam.empty()) {
        error = "ComfyUI has no SAM 3 model: sam3.1_multiplex_fp16.safetensors belongs in models/checkpoints (it cuts people out of photos for a see-through background).";
        return false;
    }
    if (uploaded.empty()) {
        error = "The picture to cut out was not uploaded";
        return false;
    }
    out = json::object();
    out["1"] = {{"class_type", "CheckpointLoaderSimple"}, {"inputs", {{"ckpt_name", sam}}}};
    out["2"] = {{"class_type", "CLIPTextEncode"}, {"inputs", {{"clip", json::array({"1", 1})}, {"text", job.prompt.empty() ? std::string("person") : job.prompt}}}};
    out["3"] = {{"class_type", "LoadImage"}, {"inputs", {{"image", uploaded[0]}}}};
    out["4"] = {{"class_type", "SAM3_Detect"},
                {"inputs", {{"model", json::array({"1", 0})}, {"image", json::array({"3", 0})}, {"conditioning", json::array({"2", 0})}, {"threshold", 0.4},
                            {"refine_iterations", 2}, {"individual_masks", true}}}};
    out["5"] = {{"class_type", "MaskToImage"}, {"inputs", {{"mask", json::array({"4", 0})}}}};
    out["6"] = {{"class_type", "SaveImage"}, {"inputs", {{"images", json::array({"5", 0})}, {"filename_prefix", "PixelArt/mask"}}}};
    engine = "SAM 3 cuts out: " + job.prompt;
    return true;
}

bool BuildUpscale(const Endpoint& ep, const ComfySettings& s, const std::string& name, const ComfyJob& job, long long seed, json& out, std::string& error, std::string& engine) {
    nlohmann::ordered_json info;
    if (!LoadObjectInfo(ep, info, error)) return false;
    const bool faces = job.upscale == 2;
    const double scale = std::clamp(job.scale, 1.0, 4.0);
    const bool resize = scale > 1.01;
    out = json::object();
    out["10"] = {{"class_type", "LoadImage"}, {"inputs", {{"image", name}}}};
    std::string img = "10";
    bool plainStretch = false;
    if (resize) {
        int native = 4;
        const std::string model = PickUpscaleModel(info, job.anime, native);
        if (!model.empty()) {
            out["11"] = {{"class_type", "UpscaleModelLoader"}, {"inputs", {{"model_name", model}}}};
            out["12"] = {{"class_type", "ImageUpscaleWithModel"}, {"inputs", {{"upscale_model", json::array({"11", 0})}, {"image", json::array({"10", 0})}}}};
            img = "12";
            const double f = scale / native;
            if (std::fabs(f - 1.0) > 0.02) {
                out["13"] = {{"class_type", "ImageScaleBy"}, {"inputs", {{"image", json::array({"12", 0})}, {"upscale_method", "lanczos"}, {"scale_by", f}}}};
                img = "13";
            }
            engine = "resizing with " + model;
        } else {
            out["13"] = {{"class_type", "ImageScaleBy"}, {"inputs", {{"image", json::array({"10", 0})}, {"upscale_method", "lanczos"}, {"scale_by", scale}}}};
            img = "13";
            plainStretch = true;
            engine = "resizing with Lanczos (no upscale model in ComfyUI's models/upscale_models)";
        }
    }
    double detail = std::clamp(job.detail, 0.0, 1.0);
    if (faces) detail = std::max(detail, 0.2);
    if (plainStretch && detail < 0.15) detail = 0.2; // a plain stretch is only blurry: let the MOSS pass redraw a little
    const double outPixels = job.srcW > 0 && job.srcH > 0 ? static_cast<double>(job.srcW) * job.srcH * scale * scale : 0.0;
    bool refine = detail > 0.01;
    if (refine && outPixels > 6.6e6) {
        if (faces) {
            error = "That picture would be " + std::to_string(static_cast<int>(outPixels / 1e6)) + " megapixels: too big for the face pass. Pick a smaller size (1x or 1.5x) and send again.";
            return false;
        }
        refine = false;
        engine += "; too big for the detail pass (over 6.5 megapixels), so only resized";
    }
    // The redraw pass (2026-10-26, was Qwen Image 2.1, whose model is gone): MOSS-Image SFT at the Layout studio's Best settings - CFG 3, the
    // negative branch without layer 22, euler / simple, 25 steps - as a low-denoise pass over the resized picture. MOSS draws from words only
    // (no picture input), so its prompt is the picture's own prompt when it has one, plus what a sharp version looks like. Faces: SAM 3 finds
    // them and only they are redrawn (blended back in with a soft edge); without SAM 3 the whole picture is redrawn with the faces wording.
    if (refine) {
        const std::string unet = PickChoice(info, "UNETLoader", "unet_name", {"moss_image_sft_int8_convrot.safetensors"}, {"moss_image_sft"});
        const std::string clip = PickChoice(info, "CLIPLoader", "clip_name", {"qwen3.5_9b_w4a8_gptq.safetensors"}, {"qwen3.5_9b"});
        const std::string vae = PickChoice(info, "VAELoader", "vae_name", {"flux2-vae.safetensors"}, {"flux2-vae", "flux2_vae"});
        if (unet.empty() || clip.empty() || vae.empty()) {
            error = "ComfyUI is missing MOSS-Image (its SFT model, text encoder or VAE), which redraws the detail: Edits > Designs & box edits has its download button.";
            return false;
        }
        out["1"] = {{"class_type", "UNETLoader"}, {"inputs", {{"unet_name", unet}, {"weight_dtype", "default"}}}};
        out["2"] = {{"class_type", "CLIPLoader"}, {"inputs", {{"clip_name", clip}, {"type", "moss_image"}, {"device", "default"}}}};
        out["3"] = {{"class_type", "VAELoader"}, {"inputs", {{"vae_name", vae}}}};
        std::string modelNode = "1";
        int upLoraId = 50;
        for (const LoraEntry& l : s.upscaleLoras) {
            if (!l.on || l.name.empty()) continue;
            const std::string listed = PickChoice(info, "LoraLoaderModelOnly", "lora_name", {l.name}, {});
            if (listed.empty()) {
                error = "ComfyUI does not list the LoRA " + l.name + " (Generate page > Options > LoRAs).";
                return false;
            }
            const std::string id = std::to_string(upLoraId++);
            out[id] = {{"class_type", "LoraLoaderModelOnly"}, {"inputs", {{"model", json::array({modelNode, 0})}, {"lora_name", listed}, {"strength_model", l.strength}}}};
            modelNode = id;
        }
        if (info.contains("SkipLayerGuidanceDiTSimple")) {
            out["4"] = {{"class_type", "SkipLayerGuidanceDiTSimple"},
                        {"inputs", {{"model", json::array({modelNode, 0})}, {"double_layers", ""}, {"single_layers", "22"}, {"start_percent", 0.0}, {"end_percent", 1.0}}}};
            modelNode = "4";
        }
        out["20"] = {{"class_type", "CLIPTextEncode"}, {"inputs", {{"clip", json::array({"2", 0})}, {"text", UpscalePrompt(faces, job.anime, job.prompt)}}}};
        out["21"] = {{"class_type", "CLIPTextEncode"}, {"inputs", {{"clip", json::array({"2", 0})}, {"text", ""}}}};
        std::string faceMask, softMask; // faces: where SAM 3 found them (grown a little), and that with a soft edge for blending back
        if (faces) {
            std::vector<std::string> checkpoints;
            std::string sam;
            if (info.contains("SAM3_Detect") && info.contains("GrowMask") && info.contains("ImageBlur") && ListCheckpointsAt(ep, checkpoints)) {
                for (const std::string& c : checkpoints) {
                    std::string l = c;
                    std::transform(l.begin(), l.end(), l.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                    if (l.find("sam3") != std::string::npos) {
                        sam = c;
                        break;
                    }
                }
            }
            if (!sam.empty()) {
                out["40"] = {{"class_type", "CheckpointLoaderSimple"}, {"inputs", {{"ckpt_name", sam}}}};
                out["41"] = {{"class_type", "CLIPTextEncode"}, {"inputs", {{"clip", json::array({"40", 1})}, {"text", "face"}}}};
                out["42"] = {{"class_type", "SAM3_Detect"},
                             {"inputs", {{"model", json::array({"40", 0})}, {"image", json::array({img, 0})}, {"conditioning", json::array({"41", 0})}, {"threshold", 0.4},
                                         {"refine_iterations", 2}, {"individual_masks", false}}}};
                const int grow = std::max(8, static_cast<int>(std::lround(std::sqrt(std::max(outPixels, 1e6)) / 64.0)));
                out["43"] = {{"class_type", "GrowMask"}, {"inputs", {{"mask", json::array({"42", 0})}, {"expand", grow}, {"tapered_corners", true}}}};
                out["44"] = {{"class_type", "MaskToImage"}, {"inputs", {{"mask", json::array({"43", 0})}}}};
                out["45"] = {{"class_type", "ImageBlur"}, {"inputs", {{"image", json::array({"44", 0})}, {"blur_radius", 15}, {"sigma", 6.0}}}};
                out["46"] = {{"class_type", "ImageToMask"}, {"inputs", {{"image", json::array({"45", 0})}, {"channel", "red"}}}};
                faceMask = "43";
                softMask = "46";
            }
        }
        const bool tiled = outPixels > 4.2e6 && info.contains("VAEEncodeTiled") && info.contains("VAEDecodeTiled");
        auto tileInputs = [&](const char* node, json inputs) { // the tile sizes (and the video ones, in versions that ask for them) are filled in from what the node wants
            if (info[node].contains("input") && info[node]["input"].contains("required")) {
                for (auto it = info[node]["input"]["required"].begin(); it != info[node]["input"]["required"].end(); ++it) {
                    if (inputs.contains(it.key())) continue;
                    if (it.key() == "tile_size") inputs["tile_size"] = 1024;
                    else if (it.key() == "overlap") inputs["overlap"] = 64;
                    else if (it.key() == "temporal_size") inputs["temporal_size"] = 64;
                    else if (it.key() == "temporal_overlap") inputs["temporal_overlap"] = 8;
                }
            }
            return inputs;
        };
        if (tiled) out["22"] = {{"class_type", "VAEEncodeTiled"}, {"inputs", tileInputs("VAEEncodeTiled", {{"pixels", json::array({img, 0})}, {"vae", json::array({"3", 0})}})}};
        else out["22"] = {{"class_type", "VAEEncode"}, {"inputs", {{"pixels", json::array({img, 0})}, {"vae", json::array({"3", 0})}}}};
        std::string latent = "22";
        if (!faceMask.empty()) {
            out["23"] = {{"class_type", "SetLatentNoiseMask"}, {"inputs", {{"samples", json::array({"22", 0})}, {"mask", json::array({faceMask, 0})}}}};
            latent = "23";
        }
        out["30"] = {{"class_type", "KSampler"},
                     {"inputs", {{"model", json::array({modelNode, 0})}, {"seed", seed}, {"steps", job.steps > 0 ? job.steps : 25}, {"cfg", 3.0}, {"sampler_name", "euler"}, {"scheduler", "simple"},
                                 {"positive", json::array({"20", 0})}, {"negative", json::array({"21", 0})}, {"latent_image", json::array({latent, 0})}, {"denoise", detail}}}};
        if (tiled) out["31"] = {{"class_type", "VAEDecodeTiled"}, {"inputs", tileInputs("VAEDecodeTiled", {{"samples", json::array({"30", 0})}, {"vae", json::array({"3", 0})}})}};
        else out["31"] = {{"class_type", "VAEDecode"}, {"inputs", {{"samples", json::array({"30", 0})}, {"vae", json::array({"3", 0})}}}};
        std::string drawn = "31";
        if (!softMask.empty()) { // outside the faces: the resized picture exactly as it was
            out["33"] = {{"class_type", "ImageCompositeMasked"},
                         {"inputs", {{"destination", json::array({img, 0})}, {"source", json::array({"31", 0})}, {"x", 0}, {"y", 0}, {"resize_source", false}, {"mask", json::array({softMask, 0})}}}};
            drawn = "33";
        }
        img = drawn;
        engine += std::string(engine.empty() ? "" : ", then ") +
                  (faces ? (softMask.empty() ? "redrawing it with MOSS (no SAM 3 in ComfyUI to find the faces, so the whole picture)" : "redrawing the faces SAM 3 finds with MOSS")
                         : "redrawing the detail with MOSS");
    }
    out["32"] = {{"class_type", "SaveImage"}, {"inputs", {{"images", json::array({img, 0})}, {"filename_prefix", faces ? "Upscale_faces" : "Upscale"}}}};
    return true;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------
ComfyService::ComfyService() { thread_ = std::thread([this]() { Loop(); }); }

ComfyService::~ComfyService() {
    stop_ = true;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (refreshThread_.joinable()) refreshThread_.join();
}

long long ComfyService::NowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

std::vector<ComfySavedWorkflow> ComfyService::SavedWorkflows() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return saved_;
}

std::string ComfyService::ListMessage() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return listMessage_;
}

std::string ComfyService::Version() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return version_;
}

void ComfyService::SetAddress(const std::string& url) {
    std::lock_guard<std::mutex> lock(mutex_);
    address_ = url;
}

std::vector<ComfyActivity> ComfyService::Activity() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return activity_;
}

std::vector<ComfyResult> ComfyService::Results() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return results_;
}

int ComfyService::ClearResults(bool onlyMissing) {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t before = results_.size();
    if (!onlyMissing) {
        results_.clear();
    } else {
        results_.erase(std::remove_if(results_.begin(), results_.end(), [](const ComfyResult& r) {
                           std::error_code ec;
                           return r.localPath.empty() || !fs::exists(r.localPath, ec);
                       }),
                       results_.end());
    }
    return static_cast<int>(before - results_.size());
}

std::vector<std::string> ComfyService::TakeMessages() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    out.swap(messages_);
    return out;
}

void ComfyService::Say(const std::string& m) {
    std::lock_guard<std::mutex> lock(mutex_);
    messages_.push_back(m);
}

void ComfyService::Update(int activityId, ComfyState state, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (ComfyActivity& a : activity_) {
        if (a.id != activityId || a.state == ComfyState::Failed) continue;
        a.state = state;
        a.message = message;
        if (state == ComfyState::Running && a.runningMs == 0) a.runningMs = NowMs();
        if (state == ComfyState::Failed || state == ComfyState::Done) a.finishedMs = NowMs();
    }
}

void ComfyService::Unlabel(int activityId, const std::string& part) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (ComfyActivity& a : activity_) {
        const size_t at = a.id == activityId ? a.label.find(part) : std::string::npos;
        if (at != std::string::npos) a.label.erase(at, part.size());
    }
}

void ComfyService::FinishPrompt(int activityId, bool ok, const std::string& message, std::vector<ComfyResult> files) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (ComfyActivity& a : activity_) {
        if (a.id != activityId) continue;
        if (a.pending > 0) --a.pending;
        for (const ComfyResult& f : files) {
            const bool seen = std::any_of(a.files.begin(), a.files.end(), [&](const ComfyResult& r) { return r.filename == f.filename && r.subfolder == f.subfolder; });
            if (!seen) a.files.push_back(f);
        }
        if (!ok) {
            a.state = ComfyState::Failed;
            a.message = message;
            a.finishedMs = NowMs();
        } else if (a.pending == 0 && a.state != ComfyState::Failed) {
            a.state = ComfyState::Done;
            a.message = a.files.empty() ? std::string("Finished (ComfyUI made no picture)") : "Made " + std::to_string(a.files.size()) + (a.files.size() == 1 ? " file" : " files");
            a.finishedMs = NowMs();
        }
    }
    for (ComfyResult& f : files) {
        // the same file once: a prompt's history can name a file twice (two output nodes, or the history read again)
        const bool seen = std::any_of(results_.begin(), results_.end(), [&](const ComfyResult& r) {
            return !f.localPath.empty() ? _wcsicmp(r.localPath.c_str(), f.localPath.c_str()) == 0
                                        : r.filename == f.filename && r.subfolder == f.subfolder && r.type == f.type;
        });
        if (!seen) results_.push_back(std::move(f));
    }
    if (results_.size() > 400) results_.erase(results_.begin(), results_.begin() + (results_.size() - 400));
}

int ComfyService::Send(const ComfySettings& settings, ComfyJob job) {
    int activityId = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job.activityId = ++nextActivity_;
        activityId = job.activityId;
        ComfyActivity a;
        a.id = job.activityId;
        a.label = job.label.empty() ? std::string("prompt") : job.label;
        a.prompt = job.prompt;
        a.address = settings.url;
        a.workflow = job.upscale > 0 ? std::string("Upscale (resize, then MOSS-Image redraws the detail)") : !job.images.empty() ? "Qwen Image 2.1 Edit (" + std::to_string(std::min<size_t>(job.images.size(), 10)) + (job.images.size() == 1 ? " picture)" : " pictures)") : settings.useLastRun ? "the workflow ComfyUI ran last" : !settings.savedWorkflow.empty() ? settings.savedWorkflow : ToUtf8(fs::path(settings.workflowPath).filename().wstring());
        a.state = ComfyState::Waiting;
        a.message = "Waiting to be sent";
        a.startedMs = NowMs();
        activity_.push_back(std::move(a));
        if (activity_.size() > 200) activity_.erase(activity_.begin());
        queue_.push_back({settings, std::move(job)});
        address_ = settings.url;
    }
    ++active_;
    cv_.notify_all();
    return activityId;
}

namespace {
// takes one prompt back from ComfyUI: out of the queue, or interrupted when it is the one generating (by its id, so nobody else's run stops)
void TakeBackPrompt(const std::string& url, const std::string& promptId) {
    Endpoint ep;
    if (!ParseEndpoint(url, ep)) return;
    std::string response, error;
    DWORD status = 0;
    Http(ep, L"POST", L"/queue", json({{"delete", json::array({promptId})}}).dump(), L"application/json", response, status, 4000, &error);
    Http(ep, L"POST", L"/interrupt", json({{"prompt_id", promptId}}).dump(), L"application/json", response, status, 4000, &error);
}
} // namespace

void ComfyService::Cancel(int activityId) {
    std::vector<std::pair<std::string, std::string>> takeBack; // (address, prompt id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        bool known = false;
        for (ComfyActivity& a : activity_) {
            if (a.id != activityId || a.state == ComfyState::Done || a.state == ComfyState::Failed) continue;
            known = true;
            a.state = ComfyState::Failed;
            a.message = "Cancelled";
            a.finishedMs = NowMs();
            a.pending = 0;
        }
        if (!known) return;
        const size_t before = queue_.size();
        queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [&](const Queued& q) { return q.job.activityId == activityId; }), queue_.end());
        active_ -= static_cast<int>(before - queue_.size());
        for (const Watching& w : watching_) {
            if (w.activityId == activityId) takeBack.emplace_back(w.settings.url, w.id);
        }
        const size_t watched = watching_.size();
        watching_.erase(std::remove_if(watching_.begin(), watching_.end(), [&](const Watching& w) { return w.activityId == activityId; }), watching_.end());
        active_ -= static_cast<int>(watched - watching_.size());
        cancelled_.insert(activityId); // still being sent: its prompt is taken back when ComfyUI answers
    }
    if (!takeBack.empty()) {
        std::thread([takeBack]() {
            for (const auto& [url, id] : takeBack) TakeBackPrompt(url, id);
        }).detach();
    }
    Say("Cancelled");
}

void ComfyService::Ping() {
    std::string url;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        url = address_;
    }
    if (url.empty()) return;
    Endpoint ep;
    std::string response, error;
    DWORD status = 0;
    bool up = false;
    std::string version;
    if (ParseEndpoint(url, ep) && Http(ep, L"GET", L"/system_stats", std::string(), nullptr, response, status, 2500, &error) && status == 200) {
        up = true;
        json j = json::parse(response, nullptr, false);
        if (j.is_object() && j.contains("system") && j["system"].is_object()) version = j["system"].value("comfyui_version", std::string());
    }
    up_ = up;
    std::lock_guard<std::mutex> lock(mutex_);
    version_ = version;
}

void ComfyService::Loop() {
    while (!stop_) {
        Queued q;
        bool have = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(watching_.empty() ? 1000 : 1500), [this]() { return stop_ || !queue_.empty(); });
            if (stop_) return;
            if (!queue_.empty()) {
                q = std::move(queue_.front());
                queue_.pop_front();
                have = true;
            }
        }
        if (have) {
            Process(q);
            --active_;
        }
        Poll();
        if (NowMs() - lastPingMs_ > 5000) { // is ComfyUI up? (the indicator in the toolbar)
            lastPingMs_ = NowMs();
            Ping();
        }
    }
}

void ComfyService::RefreshWorkflows(const ComfySettings& settings) {
    if (refreshing_.exchange(true)) return;
    if (refreshThread_.joinable()) refreshThread_.join();
    refreshThread_ = std::thread([this, settings]() {
        Endpoint ep;
        std::vector<ComfySavedWorkflow> list;
        std::string error;
        std::string message;
        if (!ParseEndpoint(settings.url, ep)) {
            message = "The ComfyUI address is not valid";
        } else if (!ListSaved(ep, list, error)) {
            message = error.empty() ? "Could not list ComfyUI's saved workflows" : error;
        } else {
            message = list.empty() ? "ComfyUI has no saved workflows yet (save one in ComfyUI with Ctrl+S)" : std::string();
            if (settings.autoImport) { // copy new / changed ones into the Workflows tab
                static std::set<std::string> done;
                int imported = 0;
                for (const ComfySavedWorkflow& w : list) {
                    if (stop_) break;
                    const std::string key = w.path + "|" + std::to_string(w.modified);
                    if (done.count(key)) continue;
                    std::string text, err2;
                    if (!FetchSaved(ep, w.path, text, err2)) continue;
                    bool existed = false;
                    std::string name = w.path.substr(0, w.path.size() - 5);
                    std::replace(name.begin(), name.end(), '/', '-');
                    if (!SaveWorkflowTextToLibrary(name, text, &existed, &err2).empty() && !existed) ++imported;
                    done.insert(key);
                }
                if (imported > 0) message = "Imported " + std::to_string(imported) + " workflow" + (imported == 1 ? "" : "s") + " into your Workflows page";
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            saved_ = std::move(list);
            listMessage_ = message;
        }
        refreshing_ = false;
    });
}

void ComfyService::Process(Queued& q) {
    const int act = q.job.activityId;
    const std::string label = q.job.label.empty() ? std::string("prompt") : q.job.label;
    auto fail = [&](const std::string& why) {
        FinishPrompt(act, false, why, {});
        Say("ComfyUI: " + why);
    };
    Endpoint ep;
    if (!ParseEndpoint(q.settings.url, ep)) {
        fail("that address is not valid (" + q.settings.url + ")");
        return;
    }
    std::string error;
    {   // ComfyUI is not running: start it in the background and wait for it
        std::string response, netError;
        DWORD status = 0;
        const bool up = Http(ep, L"GET", L"/system_stats", std::string(), nullptr, response, status, 2000, &netError) && status == 200;
        if (!up) {
            if (!q.settings.autoStart) {
                fail("ComfyUI is not running at " + q.settings.url + ". Start it, or turn on \"start it by itself\" on the Generate page.");
                return;
            }
            std::string startMessage;
            Update(act, ComfyState::Starting, "ComfyUI is not running: starting it in the background...");
            if (!ComfyOwned() && !ComfyStart(q.settings, startMessage)) {
                fail(startMessage);
                return;
            }
            bool ready = false;
            for (int i = 0; i < 120 && !stop_; ++i) { // up to four minutes
                Sleep(2000);
                if (Http(ep, L"GET", L"/system_stats", std::string(), nullptr, response, status, 2000, &netError) && status == 200) {
                    ready = true;
                    break;
                }
                if (i == 15) Update(act, ComfyState::Starting, "Still starting ComfyUI (it loads its models first)...");
            }
            if (!ready) {
                fail("ComfyUI did not come up in four minutes. Look at %LOCALAPPDATA%\\Try2Create\\comfy_server.log, or start it yourself and send again.");
                return;
            }
            up_ = true;
        }
    }
    const bool pixel = q.job.pixel > 0 || q.job.draw == 2 || q.job.draw == 3; // 3: a plain picture drawn by Qwen (no pixel art after)
    const bool edit = !q.job.images.empty() || pixel; // a built-in workflow (picture editing, upscaling, pixel art): the tab's workflow is not used
    json base;
    Targets targets;
    std::string uploaded;
    std::vector<std::string> editNames;
    std::vector<std::pair<std::string, std::string>> nodeUploads; // (LoadImage node id, the name ComfyUI gave the picture)
    // jobs sent together (a selection from the Vision library) read the workflow once
    static json batchBase;
    static int batchId = 0;
    static std::string batchKey;
    const bool raw = !q.job.apiWorkflow.empty(); // a complete workflow of the app's own (music, drama): only its files to upload
    std::vector<std::pair<std::string, std::string>> rawNames; // (placeholder in the workflow, the name ComfyUI gave the file)
    if (raw) {
        for (size_t i = 0; i < q.job.rawUploads.size(); ++i) {
            Update(act, ComfyState::Sending, "Uploading file " + std::to_string(i + 1) + " of " + std::to_string(q.job.rawUploads.size()) + "...");
            std::string name;
            if (!UploadImage(ep, q.job.rawUploads[i].second, name, error)) {
                fail(error);
                return;
            }
            rawNames.push_back({q.job.rawUploads[i].first, name});
        }
    } else if (edit) { // the pictures go to ComfyUI first; the workflow is the built-in Qwen Image 2.1 Edit one
        const size_t n = pixel ? std::min<size_t>(q.job.images.size(), 2) : q.job.upscale > 0 ? 1 : std::min<size_t>(q.job.images.size(), 10);
        for (size_t i = 0; i < n; ++i) {
            Update(act, ComfyState::Sending, "Uploading picture " + std::to_string(i + 1) + " of " + std::to_string(n) + "...");
            std::string name;
            if (!UploadImage(ep, q.job.images[i], name, error)) {
                fail(error);
                return;
            }
            editNames.push_back(name);
        }
    } else {
    Update(act, ComfyState::Sending, "Reading the workflow...");
    const std::string key = q.settings.url + "|" + (q.settings.useLastRun ? "last" : q.settings.savedWorkflow + "|" + ToUtf8(q.settings.workflowPath));
    if (q.job.batch != 0 && q.job.batch == batchId && key == batchKey) {
        base = batchBase;
    } else {
        if (!LoadTemplate(q.settings, base, error)) {
            fail(error);
            return;
        }
        if (q.job.batch != 0) {
            batchBase = base;
            batchId = q.job.batch;
            batchKey = key;
        }
    }
    targets = FindTargets(base, q.settings);
    if (targets.positive.empty()) {
        fail("I could not find where the prompt goes in that workflow. Enter the prompt node id on the Generate page (Options).");
        return;
    }
    for (const auto& [node, file] : q.job.nodePictures) {
        if (!base.contains(node)) continue;
        targets.loadImages.erase(node);
        Update(act, ComfyState::Sending, "Uploading a picture...");
        std::string name;
        if (!UploadImage(ep, file, name, error)) {
            fail(error);
            return;
        }
        nodeUploads.emplace_back(node, name);
    }
    if (!q.job.picture.empty() && !targets.loadImages.empty()) {
        Update(act, ComfyState::Sending, "Uploading the picture...");
        if (!UploadImage(ep, q.job.picture, uploaded, error)) {
            fail(error);
            return;
        }
    }
    }
    const int count = q.job.times > 0 ? std::clamp(q.job.times, 1, 100) : std::clamp(q.settings.count, 1, 16);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (ComfyActivity& a : activity_) {
            if (a.id == act) a.pending = count;
        }
    }
    for (int i = 0; i < count; ++i) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cancelled_.count(act)) return; // the rest of a batch that was cancelled
        }
        json wf;
        std::string engine;
        if (raw) { // sent as it is, with a new seed for every take (or job.seed, job.seed + 1...: a voice the seed picked is kept)
            wf = json::parse(q.job.apiWorkflow, nullptr, false);
            if (!wf.is_object()) {
                fail("The workflow to send is not valid JSON");
                return;
            }
            const long long seed = q.job.seed >= 0 ? q.job.seed + i : static_cast<long long>(Rng()() % (1ULL << 52));
            for (auto& [id, n] : wf.items()) {
                if (!n.is_object() || !n.contains("inputs")) continue;
                for (const char* k : {"seed", "noise_seed"}) {
                    if (n["inputs"].contains(k) && n["inputs"][k].is_number()) n["inputs"][k] = seed;
                }
                for (auto& [k, v] : n["inputs"].items()) { // the uploaded files' names
                    if (!v.is_string()) continue;
                    for (const auto& [placeholder, name] : rawNames) {
                        if (v.get<std::string>() == placeholder) v = name;
                    }
                }
            }
        } else if (q.job.draw == 2 || q.job.draw == 3) {
            const long long seed = q.job.seed >= 0 ? q.job.seed + i : static_cast<long long>(Rng()() % (1ULL << 52));
            if (!BuildQwenDraw(ep, q.settings, q.job, seed, wf, error, engine)) {
                fail(error);
                return;
            }
        } else if (q.job.cutout) {
            if (!BuildCutout(ep, q.job, editNames, wf, error, engine)) {
                fail(error);
                return;
            }
        } else if (pixel) {
            const long long seed = q.job.seed >= 0 ? q.job.seed + i : static_cast<long long>(Rng()() % (1ULL << 52));
            if (!BuildPixel(ep, q.job, editNames, seed, wf, error, engine)) {
                fail(error);
                return;
            }
        } else if (edit && q.job.upscale > 0) {
            const long long seed = static_cast<long long>(Rng()() % (1ULL << 52));
            if (!BuildUpscale(ep, q.settings, editNames.front(), q.job, seed, wf, error, engine)) {
                fail(error);
                return;
            }
        } else if (edit && q.job.sheetW > 0 && q.job.sheetH > 0) { // a sprite sheet: a new canvas of the asked size, no LoRAs (they change the style)
            const long long seed = q.job.seed >= 0 ? q.job.seed + i : static_cast<long long>(Rng()() % (1ULL << 52));
            ComfySettings plain = q.settings;
            plain.editLoras.clear();
            if (!BuildQwenEdit(ep, plain, editNames, q.job.prompt, seed, 2, wf, error)) {
                fail(error);
                return;
            }
            if (wf.contains("21")) {
                wf["21"]["inputs"]["width"] = (std::clamp(q.job.sheetW, 256, 4096) + 15) / 16 * 16;
                wf["21"]["inputs"]["height"] = (std::clamp(q.job.sheetH, 256, 4096) + 15) / 16 * 16;
            }
            if (wf.contains("32")) wf["32"]["inputs"]["filename_prefix"] = q.job.savePrefix.empty() ? std::string("PixelArt/anim") : q.job.savePrefix;
            engine = "Qwen Image 2.1 Edit, no LoRAs (" + std::to_string(q.job.sheetW) + " x " + std::to_string(q.job.sheetH) + ")";
        } else if (edit) {
            const long long seed = static_cast<long long>(Rng()() % (1ULL << 52));
            if (!BuildQwenEdit(ep, q.settings, editNames, q.job.prompt, seed, q.job.canvas, wf, error)) {
                fail(error);
                return;
            }
        } else {
        wf = base;
        for (const std::string& id : targets.positive) SetText(wf, id, q.job.prompt);
        if (!q.job.negative.empty()) {
            for (const std::string& id : targets.negative) SetText(wf, id, q.job.negative);
        }
        if (q.settings.randomSeed) {
            for (const std::string& id : targets.seeds) {
                json& in = wf[id]["inputs"];
                const long long seed = static_cast<long long>(Rng()() % (1ULL << 52));
                if (in.contains("seed") && in["seed"].is_number()) in["seed"] = seed;
                if (in.contains("noise_seed") && in["noise_seed"].is_number()) in["noise_seed"] = seed;
                if (in.contains("base_seed") && in["base_seed"].is_number()) in["base_seed"] = seed;
            }
        }
        if (!uploaded.empty()) {
            for (const std::string& id : targets.loadImages) wf[id]["inputs"]["image"] = uploaded;
        }
        for (const auto& [node, name] : nodeUploads) { // a LoadImage takes "image", a LoadVideo "file"
            json& in = wf[node]["inputs"];
            in[in.contains("file") ? "file" : "image"] = name;
        }
        if (q.settings.videoLength && q.job.seconds > 0) ApplyVideoLength(wf, q.job.seconds, q.settings.defaultFps);
        if (q.job.draw == 1) { // pixel art drawn by this workflow: its size, a known seed, and none of its LoRAs (realism LoRAs fight the pixel style)
            int w = 1024, h = 1024;
            DrawSize(q.job.pixW, q.job.pixH, w, h);
            SetLatentSize(wf, w, h);
            if (q.job.seed >= 0) {
                for (const std::string& id : targets.seeds) {
                    json& in = wf[id]["inputs"];
                    if (in.contains("seed") && in["seed"].is_number()) in["seed"] = q.job.seed + i;
                    if (in.contains("noise_seed") && in["noise_seed"].is_number()) in["noise_seed"] = q.job.seed + i;
                }
            }
            ApplyLoras(wf, {}, true);
            engine = "the workflow draws it (" + std::to_string(w) + " x " + std::to_string(h) + "), then it becomes pixel art";
        } else if (!q.settings.promptLoras.empty() || q.settings.promptReplaceLoras) ApplyLoras(wf, q.settings.promptLoras, q.settings.promptReplaceLoras);
        }
        {
            wchar_t dumpPath[MAX_PATH * 2] = {};
            const DWORD dn = GetEnvironmentVariableW(L"MV_DUMP_GRAPH", dumpPath, MAX_PATH * 2); // (for tests: writes the workflow that is sent)
            if (dn > 0 && dn < MAX_PATH * 2) {
                std::ofstream dump(dumpPath, std::ios::binary | std::ios::trunc);
                if (dump) dump << wf.dump(1);
            }
        }
        // Veda sparse attention is optional: the Video page puts its node in whenever its switch is on, and it stays only when this ComfyUI
        // has the node and the predictor - asked now, when ComfyUI is surely up. Otherwise it is bypassed and the label loses ", Veda".
        if (DropMissingVeda(q.settings.url, wf)) Unlabel(act, ", Veda");
        // Drama 3 keeps its experts in pinned system RAM (31 GB for the int8 file), and ComfyUI's /free does not give it back (measured:
        // still held after /free and another prompt). Loading it again with other settings puts a second copy on top of the first (the PC
        // ran down to 3 GB free in a test), and work after it finds the RAM gone. When this app started ComfyUI, it restarts it at those
        // moments, once ComfyUI's queue is empty; otherwise it goes ahead and says so.
        {
            static std::string lastDramaKey; // the loader of the Drama 3 this ComfyUI holds ("" = none that we know of)
            if (g_forgetDrama.exchange(false)) lastDramaKey.clear();
            std::string key, modelName;
            bool drama = false;
            for (const auto& [id, n] : wf.items()) {
                const std::string c = n.is_object() ? n.value("class_type", std::string()) : std::string();
                if (c.rfind("Drama3", 0) == 0 || c.rfind("Qwen3ASR", 0) == 0) drama = true; // (its transcripts belong with it)
                if (c == "Drama3Loader" && n.contains("inputs")) key = n["inputs"].dump(), modelName = n["inputs"].value("model_name", std::string());
            }
            MEMORYSTATUSEX ms = {sizeof(ms)};
            const double freeGb = GlobalMemoryStatusEx(&ms) ? ms.ullAvailPhys / 1e9 : 1e9;
            double needGb = 36;
            {
                std::error_code ec;
                const std::wstring models = ComfyModelsDir();
                const uintmax_t size = models.empty() ? 0 : fs::file_size(fs::path(models) / L"diffusion_models" / ToWide(modelName), ec);
                if (!ec && size > 0) needGb = size / 1e9 * 0.8 + 4;
            }
            std::string why;
            if (!key.empty() && !lastDramaKey.empty() && key != lastDramaKey) why = "Drama 3 loads with other settings: its RAM is given back first";
            else if (!key.empty() && lastDramaKey.empty() && freeGb < needGb) why = "making room in RAM for Drama 3";
            else if (!drama && !lastDramaKey.empty() && freeGb < 32) why = "giving back the RAM Drama 3 holds";
            // loading it takes nearly all of a 64 GB PC's RAM for about a minute (the file read while its experts are copied): Ollama's
            // models go first
            if (!key.empty() && key != lastDramaKey) FreeOllamaModels();
            if (!why.empty() && ComfyOwned()) {
                Update(act, ComfyState::Starting, "Waiting for ComfyUI's queue to empty, then restarting it (" + why + ")...");
                for (int i = 0; i < 1800 && !stop_; ++i) { // the prompts before this one finish first (up to an hour)
                    std::string qr;
                    DWORD qs = 0;
                    if (!Http(ep, L"GET", L"/queue", std::string(), nullptr, qr, qs, 3000, nullptr) || qs != 200) break;
                    const json qj = json::parse(qr, nullptr, false);
                    if (!qj.is_object() || (qj.value("queue_running", json::array()).empty() && qj.value("queue_pending", json::array()).empty())) break;
                    Sleep(2000);
                }
                Update(act, ComfyState::Starting, "Restarting ComfyUI (" + why + ")...");
                std::string m;
                ComfyStopOwned();
                Sleep(2000);
                ComfyStart(q.settings, m);
                bool ready = false;
                for (int i = 0; i < 120 && !stop_; ++i) {
                    Sleep(2000);
                    std::string r2;
                    DWORD s2 = 0;
                    if (Http(ep, L"GET", L"/system_stats", std::string(), nullptr, r2, s2, 2000, nullptr) && s2 == 200) {
                        ready = true;
                        break;
                    }
                }
                if (!ready) {
                    fail("ComfyUI did not come back after its restart. Look at %LOCALAPPDATA%\\Try2Create\\comfy_server.log.");
                    return;
                }
                lastDramaKey.clear();
            } else if (!why.empty() && !key.empty() && freeGb < needGb) {
                Say("ComfyUI: only " + std::to_string(static_cast<int>(freeGb)) + " GB of RAM free for Drama 3 (about " + std::to_string(static_cast<int>(needGb)) +
                    " GB needed): if it is slow, restart ComfyUI so it gives its RAM back");
            }
            if (!key.empty()) lastDramaKey = key;
            else if (!drama && !why.empty() && ComfyOwned()) lastDramaKey.clear();
            g_comfyHoldsDrama = !lastDramaKey.empty();
        }
        if (!q.job.loras.empty()) ApplyLoras(wf, q.job.loras, false);
        FixLoraNames(ep, wf);
        json body = {{"prompt", wf}, {"client_id", "try2create"}};
        std::string response;
        DWORD status = 0;
        std::string netError;
        Update(act, ComfyState::Sending, "Sending to ComfyUI...");
        if (!Http(ep, L"POST", L"/prompt", body.dump(-1, ' ', false, json::error_handler_t::replace), L"application/json", response, status, 30000, &netError)) {
            fail(netError);
            return;
        }
        if (status != 200) {
            fail("ComfyUI refused it: " + QueueError(response, status));
            return;
        }
        json reply = json::parse(response, nullptr, false);
        const std::string id = reply.is_object() ? reply.value("prompt_id", std::string()) : std::string();
        const int number = reply.is_object() ? reply.value("number", 0) : 0;
        Update(act, ComfyState::Queued, "In ComfyUI's queue" + (number > 0 ? " (job " + std::to_string(number) + ")" : std::string()) + (engine.empty() ? std::string() : ": " + engine));
        Say("Sent \"" + label + "\" to ComfyUI at " + q.settings.url);
        if (!id.empty()) {
            bool cancelled = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                cancelled = cancelled_.count(act) > 0;
                if (!cancelled) {
                    watching_.push_back({q.settings, id, act, NowMs(), NowMs()});
                    ++active_;
                    for (ComfyActivity& a : activity_) {
                        if (a.id == act) a.promptId = id;
                    }
                }
            }
            if (cancelled) TakeBackPrompt(q.settings.url, id); // cancelled while it was on its way
        } else {
            FinishPrompt(act, true, std::string(), {});
        }
    }
}

namespace {
// ComfyUI's own account of why a run failed ("SaveImageAdvanced.execute() missing 1 required positional argument: 'format'")
std::string HistoryError(const json& h) {
    if (h.contains("status") && h["status"].is_object() && h["status"].contains("messages") && h["status"]["messages"].is_array()) {
        for (const json& m : h["status"]["messages"]) {
            if (!m.is_array() || m.size() < 2 || !m[0].is_string() || m[0].get<std::string>() != "execution_error" || !m[1].is_object()) continue;
            std::string out = m[1].value("exception_message", std::string());
            while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
            const std::string node = m[1].value("node_type", std::string());
            if (!node.empty()) out += " (in the " + node + " node" + (m[1].contains("node_id") ? ", id " + (m[1]["node_id"].is_string() ? m[1]["node_id"].get<std::string>() : m[1]["node_id"].dump()) : std::string()) + ")";
            if (!out.empty()) return out;
        }
    }
    return "ComfyUI reported an error (see its log)";
}

bool LooksLikeVideo(const std::string& name) {
    std::string low;
    for (char ch : name) low += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    for (const char* e : {".mp4", ".webm", ".mkv", ".mov", ".gif", ".avi"}) {
        const std::string ext = e;
        if (low.size() >= ext.size() && low.compare(low.size() - ext.size(), ext.size(), ext) == 0) return true;
    }
    return false;
}

bool LooksLikeAudio(const std::string& name) {
    std::string low;
    for (char ch : name) low += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    for (const char* e : {".mp3", ".flac", ".wav", ".opus", ".ogg", ".m4a"}) {
        const std::string ext = e;
        if (low.size() >= ext.size() && low.compare(low.size() - ext.size(), ext.size(), ext) == 0) return true;
    }
    return false;
}

// Was the file written in the last `maxAgeMs` milliseconds? (A file of the same name from an earlier run must not be taken for this run's result.)
bool WrittenWithin(const std::wstring& path, long long maxAgeMs) {
    WIN32_FILE_ATTRIBUTE_DATA a = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return false;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER t, n;
    t.LowPart = a.ftLastWriteTime.dwLowDateTime;
    t.HighPart = a.ftLastWriteTime.dwHighDateTime;
    n.LowPart = now.dwLowDateTime;
    n.HighPart = now.dwHighDateTime;
    const long long ageMs = (static_cast<long long>(n.QuadPart) - static_cast<long long>(t.QuadPart)) / 10000;
    return ageMs <= maxAgeMs;
}

// Where a result is on this computer: in ComfyUI's output folder when we know it, otherwise fetched through /view into the app's own folder. Only a file
// written since the job was sent counts (maxAgeMs), so an old picture with the same name is never mistaken for the new one.
std::wstring ResolveResultFile(const ComfySettings& s, const Endpoint& ep, const ComfyResult& r, long long maxAgeMs) {
    const std::wstring outDir = ComfyOutputDir(s);
    if (!outDir.empty() && r.type == "output") {
        std::wstring path = outDir;
        if (!r.subfolder.empty()) path += L"\\" + ToWide(r.subfolder);
        path += L"\\" + ToWide(r.filename);
        std::replace(path.begin(), path.end(), L'/', L'\\');
        if (WrittenWithin(path, maxAgeMs)) return path;
    }
    const std::wstring dir = EnvW(L"LOCALAPPDATA") + L"\\Try2Create\\comfy_results";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::wstring target = dir + L"\\" + ToWide(r.filename);
    if (WrittenWithin(target, maxAgeMs)) return target;
    std::string response, error;
    DWORD status = 0;
    const std::string query = "/view?filename=" + Enc(r.filename) + "&subfolder=" + Enc(r.subfolder) + "&type=" + Enc(r.type);
    if (Http(ep, L"GET", ToWide(query), std::string(), nullptr, response, status, 60000, &error) && status == 200 && !response.empty() && response.size() < 32u * 1024 * 1024) {
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (out) {
            out.write(response.data(), static_cast<std::streamsize>(response.size()));
            return target;
        }
    }
    return {};
}
} // namespace

void ComfyService::Poll() {
    std::vector<Watching> mine;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        mine = watching_;
    }
    if (mine.empty()) return;
    // what ComfyUI is doing right now
    std::set<std::string> running;
    std::vector<std::string> pending;
    bool queueKnown = false;
    {
        Endpoint ep;
        std::string response, error;
        DWORD status = 0;
        if (ParseEndpoint(mine[0].settings.url, ep) && Http(ep, L"GET", L"/queue", std::string(), nullptr, response, status, 4000, &error) && status == 200) {
            json q = json::parse(response, nullptr, false);
            if (q.is_object()) {
                queueKnown = true;
                auto ids = [](const json& arr, auto&& add) {
                    if (!arr.is_array()) return;
                    for (const json& item : arr) {
                        if (item.is_array() && item.size() > 1 && item[1].is_string()) add(item[1].get<std::string>());
                    }
                };
                if (q.contains("queue_running")) ids(q["queue_running"], [&](const std::string& id) { running.insert(id); });
                if (q.contains("queue_pending")) ids(q["queue_pending"], [&](const std::string& id) { pending.push_back(id); });
            }
        }
    }
    for (const Watching& w : mine) {
        if (stop_) return;
        bool done = false;
        // No time limit while ComfyUI still has it: ten long videos in a row can take hours. Only a prompt ComfyUI no longer lists (it was
        // restarted, or the queue was cleared) is given up, after ten minutes of not seeing it.
        const bool listed = !queueKnown || running.count(w.id) || std::find(pending.begin(), pending.end(), w.id) != pending.end();
        long long seen = w.seenMs;
        if (listed) {
            seen = NowMs();
            std::lock_guard<std::mutex> lock(mutex_);
            for (Watching& x : watching_) {
                if (x.id == w.id) x.seenMs = seen;
            }
        }
        if (NowMs() - seen > 10LL * 60 * 1000) {
            done = true;
            FinishPrompt(w.activityId, false, "ComfyUI no longer has it in its queue (was it restarted, or the queue cleared?): I stopped waiting", {});
        } else {
            Endpoint ep;
            std::string response, error;
            DWORD status = 0;
            if (ParseEndpoint(w.settings.url, ep) && Http(ep, L"GET", L"/history/" + ToWide(w.id), std::string(), nullptr, response, status, 4000, &error) && status == 200) {
                json j = json::parse(response, nullptr, false);
                if (j.is_object() && j.contains(w.id)) {
                    done = true;
                    const json& h = j[w.id];
                    std::vector<ComfyResult> got;
                    if (h.contains("outputs") && h["outputs"].is_object()) {
                        for (auto it = h["outputs"].begin(); it != h["outputs"].end(); ++it) {
                            for (const char* kind : {"images", "gifs", "videos", "video", "files", "audio"}) { // pictures, video clips and songs
                                if (!it.value().contains(kind) || !it.value()[kind].is_array()) continue;
                                for (const json& im : it.value()[kind]) {
                                    if (!im.is_object()) continue;
                                    ComfyResult r;
                                    {
                                        std::lock_guard<std::mutex> lock(mutex_);
                                        for (const ComfyActivity& a : activity_) {
                                            if (a.id != w.activityId) continue;
                                            r.label = a.label;
                                            r.prompt = a.prompt;
                                        }
                                    }
                                    r.filename = im.value("filename", std::string());
                                    r.subfolder = im.value("subfolder", std::string());
                                    r.type = im.value("type", std::string("output"));
                                    r.audio = std::string(kind) == "audio" || LooksLikeAudio(r.filename);
                                    r.video = !r.audio && (std::string(kind) != "images" || LooksLikeVideo(r.filename));
                                    // (not "input": LoadVideo shows the clip it loaded, a Star / Swap's own guide clip, as if it were made)
                                    if (!r.filename.empty() && r.type != "temp" && r.type != "input") got.push_back(std::move(r));
                                }
                            }
                        }
                    }
                    const std::string state = h.contains("status") && h["status"].is_object() ? h["status"].value("status_str", std::string()) : std::string();
                    if (state == "error") {
                        const std::string why = HistoryError(h);
                        FinishPrompt(w.activityId, false, "ComfyUI failed: " + why, {});
                        Say("ComfyUI failed: " + why);
                    } else {
                        const long long maxAge = std::max(0LL, NowMs() - w.startedMs) + 120000; // since it was sent (and some slack for clocks)
                        double took = HistorySeconds(h); // ComfyUI's own clock; else since it was sent (includes waiting in the queue)
                        if (took <= 0) took = (NowMs() - w.startedMs) / 1000.0;
                        for (ComfyResult& r : got) {
                            r.localPath = ResolveResultFile(w.settings, ep, r, maxAge);
                            r.seconds = took;
                            RecordGenerationTime(r.localPath, took, r.label);
                        }
                        std::string names;
                        for (const ComfyResult& r : got) names += (names.empty() ? "" : ", ") + r.filename;
                        FinishPrompt(w.activityId, true, std::string(), got);
                        Say(got.empty() ? "ComfyUI finished (no picture in its output)" : "ComfyUI made " + names);
                    }
                }
            }
            if (!done) {
                if (running.count(w.id)) {
                    Update(w.activityId, ComfyState::Running, "ComfyUI is generating it now...");
                } else {
                    int position = 0;
                    for (size_t i = 0; i < pending.size(); ++i)
                        if (pending[i] == w.id) position = static_cast<int>(i) + 1;
                    Update(w.activityId, ComfyState::Queued, position > 0 ? "Waiting in ComfyUI's queue (" + std::to_string(position) + " ahead of or at its place)" : "In ComfyUI's queue");
                }
            }
        }
        if (done) {
            std::lock_guard<std::mutex> lock(mutex_);
            watching_.erase(std::remove_if(watching_.begin(), watching_.end(), [&](const Watching& x) { return x.id == w.id; }), watching_.end());
            --active_;
        }
    }
}

// A small graph run start to end from a background thread (no activity row): the files uploaded into the given nodes, the graph queued, its
// outputs downloaded into outDir. For a step ComfyUI does on the way to a job (the person masks of a swap).
bool ComfyRunGraph(const std::string& url, const std::string& graphJson, const std::vector<std::pair<std::string, std::wstring>>& uploads,
                   const std::wstring& outDir, std::vector<std::wstring>& outFiles, std::string& error, int timeoutSec) {
    outFiles.clear();
    Endpoint ep;
    if (!ParseEndpoint(url, ep)) {
        error = "The ComfyUI address is not valid";
        return false;
    }
    json wf = json::parse(graphJson, nullptr, false);
    if (!wf.is_object()) {
        error = "The graph is not valid";
        return false;
    }
    for (const auto& [node, path] : uploads) {
        std::string name;
        if (!UploadImage(ep, path, name, error)) return false;
        if (wf.contains(node)) wf[node]["inputs"][wf[node].value("class_type", std::string()) == "LoadVideo" ? "file" : "image"] = name;
    }
    FixLoraNames(ep, wf);
    json body = {{"prompt", wf}, {"client_id", "try2create"}};
    std::string response;
    DWORD status = 0;
    if (!Http(ep, L"POST", L"/prompt", body.dump(-1, ' ', false, json::error_handler_t::replace), L"application/json", response, status, 30000, &error)) return false;
    if (status != 200) {
        error = "ComfyUI refused it: " + QueueError(response, status);
        return false;
    }
    json reply = json::parse(response, nullptr, false);
    const std::string id = reply.is_object() ? reply.value("prompt_id", std::string()) : std::string();
    if (id.empty()) {
        error = "ComfyUI did not queue it";
        return false;
    }
    const auto start = std::chrono::steady_clock::now();
    auto lastAnswer = start; // (seen: ComfyUI died under a too-big swap and this waited its whole 30 minutes - and the app's exit with it)
    for (;;) {
        Sleep(1000);
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(timeoutSec)) {
            error = "ComfyUI took too long";
            return false;
        }
        if (!Http(ep, L"GET", L"/history/" + ToWide(id), std::string(), nullptr, response, status, 10000, nullptr) || status != 200) {
            if (std::chrono::steady_clock::now() - lastAnswer > std::chrono::seconds(20)) {
                error = "ComfyUI stopped answering (it may have closed or run out of memory)";
                return false;
            }
            continue;
        }
        lastAnswer = std::chrono::steady_clock::now();
        json h = json::parse(response, nullptr, false);
        if (!h.is_object() || !h.contains(id)) continue;
        const json& run = h[id];
        if (run.contains("status") && run["status"].value("status_str", std::string()) == "error") {
            error = HistoryError(run);
            return false;
        }
        if (!run.contains("outputs") || !run["outputs"].is_object()) continue;
        std::error_code ec;
        fs::create_directories(outDir, ec);
        for (auto it = run["outputs"].begin(); it != run["outputs"].end(); ++it) {
            for (auto kind = it.value().begin(); kind != it.value().end(); ++kind) {
                if (!kind.value().is_array()) continue;
                for (const json& f : kind.value()) {
                    if (!f.is_object() || !f.contains("filename") || f.value("type", std::string()) == "input") continue;
                    const std::string fn = f.value("filename", std::string()), sub = f.value("subfolder", std::string()), type = f.value("type", std::string("output"));
                    std::string bytes;
                    if (!Http(ep, L"GET", ToWide("/view?filename=" + Enc(fn) + "&subfolder=" + Enc(sub) + "&type=" + Enc(type)), std::string(), nullptr, bytes, status, 120000, &error) || status != 200) {
                        if (error.empty()) error = "Could not fetch what ComfyUI made";
                        return false;
                    }
                    const std::wstring local = outDir + L"\\" + fs::path(ToWide(fn)).filename().wstring();
                    std::ofstream(local, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                    outFiles.push_back(local);
                }
            }
        }
        if (outFiles.empty()) error = "ComfyUI made nothing";
        return !outFiles.empty();
    }
}
