// The "Workflows" tab: a ComfyUI node viewer plus the library of workflows extracted from images.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <future>
#include <set>
#include <unordered_map>
#include <windows.h>
#include <shellapi.h>

#include "imgui.h"
#include "ui_extra.h"
#include "ui_theme.h"
#include "win_dialogs.h"
#include "workflow.h"

namespace fs = std::filesystem;

namespace {

constexpr float kTitleH = 30.0f;
constexpr float kSlotH = 20.0f;
constexpr float kWidgetH = 24.0f;
constexpr float kFont = 14.0f; // text size in graph units

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

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

ImU32 Rgb(uint32_t rgb, int alpha = 255) { return IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, alpha); }

uint32_t LinkColor(const std::string& type) {
    static const std::unordered_map<std::string, uint32_t> colors = {
        {"CLIP", 0xFFD500},        {"CLIP_VISION", 0xA8DADC},  {"CLIP_VISION_OUTPUT", 0xAD7452}, {"CONDITIONING", 0xFFA931},
        {"CONTROL_NET", 0x6EE2FF}, {"IMAGE", 0x64B5F6},        {"LATENT", 0xFF9CF9},             {"MASK", 0x81C784},
        {"MODEL", 0xB39DDB},       {"STYLE_MODEL", 0xC2FFAE},  {"VAE", 0xFF6E6E},                {"NOISE", 0xB0B0B0},
        {"GUIDER", 0x66FFFF},      {"SAMPLER", 0xECB4B4},      {"SIGMAS", 0xCDFFCD},             {"UPSCALE_MODEL", 0xFFB347},
    };
    auto it = colors.find(type);
    return it == colors.end() ? 0x9A9AAE : it->second;
}

std::string Truncate(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    size_t cut = n;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut; // keep UTF-8 whole
    return s.substr(0, cut) + "...";
}

std::string DateText(long long unixSeconds) {
    if (unixSeconds <= 0) return {};
    std::time_t t = static_cast<std::time_t>(unixSeconds);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tmv);
    return buf;
}

bool NodeMatches(const WfNode& n, const std::string& lowerQuery) {
    if (lowerQuery.empty()) return false;
    if (Lower(n.title).find(lowerQuery) != std::string::npos || Lower(n.type).find(lowerQuery) != std::string::npos) return true;
    for (const WfWidget& w : n.widgets) {
        if (Lower(w.value).find(lowerQuery) != std::string::npos) return true;
    }
    return false;
}

void LoadGraph(AppState& app, const std::string& text, const std::string& api, const std::string& title,
               const std::wstring& source, const std::wstring& libPath, const char* kind) {
    WorkflowViewState& s = app.wf;
    s.open = true;
    s.text = text;
    s.api = api;
    s.title = title;
    s.kindLabel = kind;
    s.sourceImage = source;
    s.libPath = libPath;
    s.selNode = -1;
    s.centerOn = -1;
    s.viewFresh = true;
    s.error.clear();
    s.summary.clear();
    s.subPath.clear();
    s.subNames.clear();
    if (!ParseWorkflowGraph(text, api.empty() ? nullptr : &api, s.graph, &s.error)) {
        s.graph.Clear();
        if (s.error.empty()) s.error = "Could not read this workflow.";
        return;
    }
    s.summary = SummarizeWorkflow(s.graph);
    if (s.graph.fromApi) s.kindLabel = "ComfyUI API prompt (laid out automatically)";
}

// Re-reads the graph for the subgraph level currently open.
void Reparse(WorkflowViewState& s) {
    std::string err;
    WfGraph g;
    if (ParseWorkflowGraph(s.text, s.api.empty() ? nullptr : &s.api, g, &err, &s.subPath)) {
        s.graph = std::move(g);
        s.error.clear();
    } else {
        s.error = err;
    }
    s.selNode = -1;
    s.centerOn = -1;
    s.viewFresh = true;
}

void EnterSubgraph(WorkflowViewState& s, const WfNode& n) {
    if (n.subgraphId.empty()) return;
    s.subPath.push_back({n.subgraphId, n.id});
    s.subNames.push_back(n.title);
    Reparse(s);
}

void LeaveSubgraph(WorkflowViewState& s, size_t depth) { // depth = how many levels stay open
    if (depth >= s.subPath.size()) return;
    s.subPath.resize(depth);
    s.subNames.resize(depth);
    Reparse(s);
}

bool SaveCurrent(AppState& app) {
    WorkflowViewState& s = app.wf;
    if (!s.open || s.text.empty()) return false;
    bool existed = false;
    std::string err;
    std::wstring saved;
    if (!s.sourceImage.empty()) {
        ImageMetadata meta = ReadImageMetadata(s.sourceImage);
        saved = SaveWorkflowToLibrary(app.Device(), s.sourceImage, meta, &existed, &err);
    }
    if (saved.empty()) saved = SaveWorkflowTextToLibrary(s.title, s.text, &existed, &err);
    if (saved.empty()) {
        app.SetStatus(err.empty() ? "Could not save the workflow." : err);
        return false;
    }
    s.libPath = saved;
    s.libDirty = true;
    app.SetStatus(existed ? "That workflow is already in the library." : "Saved the workflow to the library.");
    return true;
}

void OpenLibraryItem(AppState& app, const WorkflowLibItem& it) {
    std::string text, api, err;
    if (!ReadWorkflowFromFile(it.jsonPath, text, api, &err)) {
        app.SetStatus("Could not open that workflow: " + err);
        return;
    }
    LoadGraph(app, text, api, it.name, ToWide(it.source), it.jsonPath, it.kindLabel.c_str());
}

void OpenFromFile(AppState& app) {
    std::wstring path = OpenWorkflowFileDialog(GetActiveWindow());
    if (path.empty()) return;
    std::string text, api, err;
    if (!ReadWorkflowFromFile(path, text, api, &err)) {
        app.SetStatus(err.empty() ? "No workflow found in that file." : err);
        return;
    }
    const bool image = !(path.size() > 5 && _wcsicmp(path.c_str() + path.size() - 5, L".json") == 0);
    LoadGraph(app, text, api, ToUtf8(fs::path(path).stem().wstring()), image ? path : std::wstring(), std::wstring(),
              api.empty() ? "ComfyUI workflow" : "ComfyUI workflow");
}

// ---------------------------------------------------------------------------
// canvas
// ---------------------------------------------------------------------------
struct View {
    ImVec2 origin;
    float sc;
    float panX, panY;
    ImVec2 ToScreen(float wx, float wy) const { return ImVec2(origin.x + (wx - panX) * sc, origin.y + (wy - panY) * sc); }
};

ImVec2 SlotPos(const WfNode& n, bool output, int slot) {
    if (n.collapsed) return ImVec2(output ? n.x + std::min(n.w, 180.0f) : n.x, n.y - kTitleH * 0.5f);
    return ImVec2(output ? n.x + n.w : n.x, n.y + (static_cast<float>(slot) + 0.7f) * kSlotH);
}

void AddTextClipped(ImDrawList* dl, float fs, ImVec2 pos, ImU32 col, const std::string& text, const ImVec4& clip, float wrap = 0.0f) {
    if (text.empty()) return;
    // whole pixels keep the glyphs crisp: the atlas is only sharp when drawn 1:1 at a pixel boundary
    pos.x = std::round(pos.x);
    pos.y = std::round(pos.y);
    dl->AddText(ImGui::GetFont(), fs, pos, col, text.c_str(), text.c_str() + text.size(), wrap, &clip);
}

float TextWidth(float fs, const std::string& s) { return ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.0f, s.c_str(), s.c_str() + s.size()).x; }

void DrawNode(ImDrawList* dl, const View& v, const WfNode& n, bool selected, bool matched, bool linkedToSelected) {
    const float sc = v.sc;
    const float nodeW = n.collapsed ? std::min(n.w, 180.0f) : n.w;
    const ImVec2 a = v.ToScreen(n.x, n.y - kTitleH);
    const ImVec2 b = v.ToScreen(n.x + nodeW, n.collapsed ? n.y : n.y + n.h);
    const int alpha = n.mode == 2 ? 110 : 255;
    const float round = std::max(2.0f, 7.0f * sc);

    uint32_t bg = n.hasBg ? n.bgcolor : 0x353535;
    uint32_t title = n.hasColor ? n.color : 0x2a2a2a;
    if (n.mode == 4) {
        bg = 0x553377;
        title = 0x663399;
    }
    const ImVec2 titleEnd(b.x, a.y + kTitleH * sc);
    if (!n.collapsed) dl->AddRectFilled(a, b, Rgb(bg, alpha), round);
    dl->AddRectFilled(a, n.collapsed ? b : titleEnd, Rgb(title, alpha), round, n.collapsed ? 0 : ImDrawFlags_RoundCornersTop);
    ImU32 border = IM_COL32(15, 15, 15, alpha);
    float borderW = std::max(1.0f, sc);
    if (matched) { border = IM_COL32(255, 214, 80, 255); borderW = std::max(2.0f, 2.5f * sc); }
    else if (selected) { border = ImGui::GetColorU32(ui::kAccentHover); borderW = std::max(2.0f, 2.5f * sc); }
    else if (linkedToSelected) { border = IM_COL32(120, 170, 255, 200); }
    else if (!n.subgraphId.empty()) { border = IM_COL32(90, 150, 230, 230); borderW = std::max(1.5f, 2.0f * sc); }
    dl->AddRect(a, b, border, round, 0, borderW);
    if (n.collapsed) {
        const float fs = ImGui::GetFontSize();
        if (b.x - a.x > 40.0f) {
            const ImVec4 clip(a.x, a.y - 4, b.x, b.y + 4);
            AddTextClipped(dl, fs, ImVec2(a.x + 8 * sc, a.y + (kTitleH * sc - fs) * 0.5f), IM_COL32(220, 220, 220, alpha), n.title, clip);
        }
        return;
    }

    // Text is drawn at the font's own size (scaled glyphs go blurry); only the layout zooms.
    // Titles are what you navigate by, so they stay at every zoom; ports and values appear once
    // there is room for them.
    const float native = ImGui::GetFontSize();
    if (b.x - a.x > 40.0f) {
        const ImVec4 titleClip(a.x, a.y - 4, b.x - 6 * sc, titleEnd.y + 4);
        AddTextClipped(dl, native, ImVec2(a.x + 10 * sc, a.y + (kTitleH * sc - native) * 0.5f), IM_COL32(225, 225, 225, alpha), n.title, titleClip);
    }
    const float ratio = kFont * sc / native;
    if (ratio < 0.65f) return; // too small for ports and values: boxes and titles only
    const float fs = ratio <= 1.3f ? native : kFont * sc;

    if (!n.subgraphId.empty() && n.widgets.empty() && b.y - a.y > native * 4) {
        const char* label = "Subgraph - double-click to open";
        const ImVec2 ts = ImGui::CalcTextSize(label);
        if (ts.x < b.x - a.x - 10.0f) {
            AddTextClipped(dl, native, ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y) * 0.5f), IM_COL32(120, 170, 235, alpha), label, ImVec4(a.x, a.y, b.x, b.y));
        }
    }

    // ports
    const int rows = static_cast<int>(std::max(n.inputs.size(), n.outputs.size()));
    const float r = std::max(2.0f, 4.5f * sc);
    for (size_t i = 0; i < n.inputs.size(); ++i) {
        const ImVec2 p = v.ToScreen(SlotPos(n, false, static_cast<int>(i)).x, SlotPos(n, false, static_cast<int>(i)).y);
        const ImU32 col = Rgb(LinkColor(n.inputs[i].type), alpha);
        if (n.inputs[i].link >= 0) dl->AddCircleFilled(p, r, col, 12);
        else dl->AddCircle(p, r, col, 12, std::max(1.0f, sc));
        const ImVec4 clip(a.x, p.y - 12 * sc, b.x - n.w * 0.45f * sc, p.y + 12 * sc);
        AddTextClipped(dl, fs, ImVec2(p.x + 10 * sc, p.y - fs * 0.55f), IM_COL32(170, 170, 170, alpha), n.inputs[i].name, clip);
    }
    for (size_t i = 0; i < n.outputs.size(); ++i) {
        const ImVec2 sp = SlotPos(n, true, static_cast<int>(i));
        const ImVec2 p = v.ToScreen(sp.x, sp.y);
        const ImU32 col = Rgb(LinkColor(n.outputs[i].type), alpha);
        dl->AddCircleFilled(p, r, col, 12);
        const float tw = TextWidth(fs, n.outputs[i].name);
        const ImVec4 clip(a.x + n.w * 0.4f * sc, p.y - 12 * sc, b.x, p.y + 12 * sc);
        AddTextClipped(dl, fs, ImVec2(p.x - 10 * sc - tw, p.y - fs * 0.55f), IM_COL32(170, 170, 170, alpha), n.outputs[i].name, clip);
    }

    // widgets: single-line values as rows, long text (prompts) in the space that is left
    float top = n.y + rows * kSlotH + 6.0f;
    int multi = 0, single = 0;
    for (const WfWidget& w : n.widgets) (w.multiline ? multi : single)++;
    float multiH = 0;
    if (multi > 0) multiH = std::max(36.0f, (n.y + n.h - 8.0f - top - single * kWidgetH) / multi - 4.0f);
    for (const WfWidget& w : n.widgets) {
        if (w.multiline) {
            const ImVec2 r0 = v.ToScreen(n.x + 10, top), r1 = v.ToScreen(n.x + n.w - 10, top + multiH);
            dl->AddRectFilled(r0, r1, IM_COL32(28, 28, 28, alpha), 5.0f * sc);
            if (sc > 0.35f) {
                const ImVec4 clip(r0.x + 2, r0.y + 2, r1.x - 2, r1.y - 2);
                AddTextClipped(dl, fs, ImVec2(r0.x + 6 * sc, r0.y + 4 * sc), IM_COL32(205, 205, 205, alpha), Truncate(w.value, 700), clip,
                               (r1.x - r0.x) - 12 * sc);
            }
            top += multiH + 4.0f;
        } else {
            const ImVec2 r0 = v.ToScreen(n.x + 10, top), r1 = v.ToScreen(n.x + n.w - 10, top + 20.0f);
            dl->AddRectFilled(r0, r1, IM_COL32(28, 28, 28, alpha), 10.0f * sc);
            if (sc > 0.35f) {
                const ImVec4 clip(r0.x + 2, r0.y, r1.x - 2, r1.y);
                const std::string val = Truncate(w.value, 120);
                const float vw = TextWidth(fs, val);
                const float labelRoom = (r1.x - r0.x) - vw - 24 * sc;
                AddTextClipped(dl, fs, ImVec2(r0.x + 10 * sc, r0.y + (20.0f * sc - fs) * 0.5f), IM_COL32(135, 135, 135, alpha), w.name,
                               ImVec4(clip.x, clip.y, clip.x + std::max(0.0f, labelRoom) + 6 * sc, clip.w));
                AddTextClipped(dl, fs, ImVec2(std::max(r0.x + 10 * sc, r1.x - 10 * sc - vw), r0.y + (20.0f * sc - fs) * 0.5f),
                               IM_COL32(215, 215, 215, alpha), val, clip);
            }
            top += kWidgetH;
        }
    }
}

void FitView(WorkflowViewState& s, ImVec2 canvas) {
    const WfGraph& g = s.graph;
    if (g.nodes.empty()) return;
    const float margin = 40.0f;
    const float w = g.maxX - g.minX + margin * 2, h = g.maxY - g.minY + margin * 2;
    const float dp = ui::Dp(1.0f);
    s.zoom = std::clamp(std::min(canvas.x / (w * dp), canvas.y / (h * dp)), 0.04f, 1.5f);
    const float sc = s.zoom * dp;
    s.panX = (g.minX + g.maxX) * 0.5f - canvas.x * 0.5f / sc;
    s.panY = (g.minY + g.maxY) * 0.5f - canvas.y * 0.5f / sc;
}

void DrawCanvas(AppState& app, WorkflowViewState& s, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(32, 32, 36, 255));
    ImGui::BeginChild("wf_canvas", size, true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 sz = ImGui::GetContentRegionAvail();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float dp = ui::Dp(1.0f);

    if (s.viewFresh && sz.x > 50 && sz.y > 50) {
        FitView(s, sz);
        s.viewFresh = false;
    }
    if (s.centerOn >= 0) {
        if (const WfNode* n = s.graph.Find(s.centerOn)) {
            const float sc = s.zoom * dp;
            s.panX = n->x + n->w * 0.5f - sz.x * 0.5f / sc;
            s.panY = n->y + n->h * 0.5f - sz.y * 0.5f / sc;
        }
        s.centerOn = -1;
    }

    ImGui::InvisibleButton("##canvas", sz, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const ImGuiIO& io = ImGui::GetIO();
    static float dragged = 0.0f;
    if (ImGui::IsItemActivated()) dragged = 0.0f;
    if (ImGui::IsItemActive()) {
        const float sc = s.zoom * dp;
        s.panX -= io.MouseDelta.x / sc;
        s.panY -= io.MouseDelta.y / sc;
        dragged += std::fabs(io.MouseDelta.x) + std::fabs(io.MouseDelta.y);
    }
    if (hovered && io.MouseWheel != 0.0f) {
        const float before = s.zoom * dp;
        const ImVec2 m(io.MousePos.x - p0.x, io.MousePos.y - p0.y);
        const float wx = s.panX + m.x / before, wy = s.panY + m.y / before;
        s.zoom = std::clamp(s.zoom * std::pow(1.15f, io.MouseWheel), 0.04f, 4.0f);
        const float after = s.zoom * dp;
        s.panX = wx - m.x / after;
        s.panY = wy - m.y / after;
    }

    View v{p0, s.zoom * dp, s.panX, s.panY};
    dl->PushClipRect(p0, ImVec2(p0.x + sz.x, p0.y + sz.y), true);

    // grid
    {
        const float step = 50.0f * v.sc;
        if (step > 9.0f) {
            const float ox = std::fmod(-s.panX * v.sc, step), oy = std::fmod(-s.panY * v.sc, step);
            const ImU32 gc = IM_COL32(255, 255, 255, 14);
            for (float x = ox; x < sz.x; x += step) dl->AddLine(ImVec2(p0.x + x, p0.y), ImVec2(p0.x + x, p0.y + sz.y), gc);
            for (float y = oy; y < sz.y; y += step) dl->AddLine(ImVec2(p0.x, p0.y + y), ImVec2(p0.x + sz.x, p0.y + y), gc);
        }
    }

    const WfGraph& g = s.graph;
    auto visible = [&](float x0, float y0, float x1, float y1) {
        const ImVec2 a = v.ToScreen(x0, y0), b = v.ToScreen(x1, y1);
        return b.x >= p0.x && a.x <= p0.x + sz.x && b.y >= p0.y && a.y <= p0.y + sz.y;
    };

    for (const WfGroup& gr : g.groups) {
        if (!visible(gr.x, gr.y, gr.x + gr.w, gr.y + gr.h)) continue;
        const uint32_t col = gr.hasColor ? gr.color : 0x3F789E;
        const ImVec2 a = v.ToScreen(gr.x, gr.y), b = v.ToScreen(gr.x + gr.w, gr.y + gr.h);
        dl->AddRectFilled(a, b, Rgb(col, 40), 6.0f * v.sc);
        dl->AddRect(a, b, Rgb(col, 130), 6.0f * v.sc, 0, std::max(1.0f, v.sc));
        const float native = ImGui::GetFontSize(); // 1:1 glyphs stay sharp at any zoom
        const float fs = 22.0f * v.sc > native * 1.3f ? 22.0f * v.sc : native;
        if (b.x - a.x > 60.0f) AddTextClipped(dl, fs, ImVec2(a.x + 10 * v.sc, a.y + std::max(3.0f, 6 * v.sc)), IM_COL32(220, 220, 220, 210), gr.title, ImVec4(a.x, a.y, b.x, a.y + std::max(fs + 8.0f, 40 * v.sc)));
    }

    // links
    std::set<int> neighbours;
    if (s.selNode >= 0) {
        for (const WfLink& l : g.links) {
            if (l.from == s.selNode) neighbours.insert(l.to);
            if (l.to == s.selNode) neighbours.insert(l.from);
        }
    }
    for (const WfLink& l : g.links) {
        const WfNode* from = g.Find(l.from);
        const WfNode* to = g.Find(l.to);
        if (!from || !to) continue;
        const ImVec2 sa = SlotPos(*from, true, l.fromSlot), sb = SlotPos(*to, false, l.toSlot);
        if (!visible(std::min(sa.x, sb.x), std::min(sa.y, sb.y) - 10, std::max(sa.x, sb.x), std::max(sa.y, sb.y) + 10)) continue;
        const ImVec2 a = v.ToScreen(sa.x, sa.y), b = v.ToScreen(sb.x, sb.y);
        const float dist = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
        const bool hot = s.selNode >= 0 && (l.from == s.selNode || l.to == s.selNode);
        const int alpha = (s.selNode >= 0 && !hot) ? 90 : 215;
        dl->AddBezierCubic(a, ImVec2(a.x + dist * 0.25f, a.y), ImVec2(b.x - dist * 0.25f, b.y), b, Rgb(LinkColor(l.type), alpha),
                           std::clamp((hot ? 3.5f : 2.2f) * v.sc, 1.0f, 6.0f));
    }

    // nodes (the selected one on top)
    const std::string query = Lower(s.nodeSearch);
    auto drawIf = [&](const WfNode& n) {
        if (!visible(n.x, n.y - kTitleH, n.x + n.w, n.y + n.h)) return;
        DrawNode(dl, v, n, n.id == s.selNode, NodeMatches(n, query), neighbours.count(n.id) != 0);
    };
    for (const WfNode& n : g.nodes) if (n.id != s.selNode) drawIf(n);
    if (const WfNode* sel = g.Find(s.selNode)) drawIf(*sel);
    dl->PopClipRect();

    // a click (not a drag) selects the node under the cursor, or nothing
    if (ImGui::IsItemDeactivated() && dragged < 5.0f && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        const float wx = s.panX + (io.MousePos.x - p0.x) / v.sc, wy = s.panY + (io.MousePos.y - p0.y) / v.sc;
        int hit = -1;
        for (size_t i = g.nodes.size(); i-- > 0;) {
            const WfNode& n = g.nodes[i];
            const float w = n.collapsed ? std::min(n.w, 180.0f) : n.w, h = n.collapsed ? 0.0f : n.h;
            if (wx >= n.x && wx <= n.x + w && wy >= n.y - kTitleH && wy <= n.y + h) {
                hit = n.id;
                break;
            }
        }
        s.selNode = hit;
        if (hit != -1 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            if (const WfNode* hn = g.Find(hit)) {
                if (!hn->subgraphId.empty()) {
                    const WfNode copy = *hn; // the graph is replaced while entering
                    EnterSubgraph(s, copy);
                }
            }
        }
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !s.subPath.empty()) {
        // double-click on empty canvas inside a subgraph goes back up
        const float wx = s.panX + (io.MousePos.x - p0.x) / v.sc, wy = s.panY + (io.MousePos.y - p0.y) / v.sc;
        bool onNode = false;
        for (const WfNode& n : g.nodes) {
            if (wx >= n.x && wx <= n.x + n.w && wy >= n.y - kTitleH && wy <= n.y + n.h) onNode = true;
        }
        if (!onNode) LeaveSubgraph(s, s.subPath.size() - 1);
    }

    // little legend
    const ImVec2 hint(p0.x + 8, p0.y + sz.y - ImGui::GetTextLineHeight() - 6);
    dl->AddText(hint, IM_COL32(150, 150, 160, 200), "Drag to pan  -  wheel to zoom  -  click a node for its values");
    ImGui::EndChild();
}

void CopyButton(const char* id, const std::string& text, AppState& app, const char* what) {
    ImGui::PushID(id);
    if (ImGui::SmallButton("Copy")) {
        ImGui::SetClipboardText(text.c_str());
        app.SetStatus(std::string("Copied ") + what + ".");
    }
    ImGui::PopID();
}

void DrawInspector(AppState& app, WorkflowViewState& s, ImVec2 size) {
    ImGui::BeginChild("wf_inspector", size, true);
    const WfGraph& g = s.graph;
    const WfNode* n = g.Find(s.selNode);
    if (!n) {
        ui::SectionHeading("THIS WORKFLOW");
        ImGui::TextWrapped("%s", s.summary.c_str());
        ImGui::TextDisabled("%d links, %d groups", static_cast<int>(g.links.size()), static_cast<int>(g.groups.size()));
        ImGui::Spacing();
        ImGui::Spacing();

        // prompts and models at a glance, so the graph doesn't have to be read to find them
        bool any = false;
        auto isNote = [](const std::string& type) { return type.find("Note") != std::string::npos; };
        for (const WfNode& node : g.nodes) {
            if (node.mode == 2 || isNote(node.type)) continue;
            for (const WfWidget& w : node.widgets) {
                if (!w.multiline || w.name != "text") continue;
                if (!any) {
                    ui::SectionHeading("TEXT / PROMPTS");
                    any = true;
                }
                ImGui::PushID(&w);
                ImGui::TextColored(ui::kTextDim, "%s (#%s)", node.title.c_str(), node.idText.c_str());
                ImGui::SameLine();
                CopyButton("cp", w.value, app, "the text");
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(Truncate(w.value, 1200).c_str());
                ImGui::PopTextWrapPos();
                ImGui::Spacing();
                ImGui::PopID();
            }
        }
        for (const auto& [title, w] : g.deep) { // prompts inside subgraphs
            if (!w.multiline || w.name != "text" || isNote(title)) continue;
            if (!any) {
                ui::SectionHeading("TEXT / PROMPTS");
                any = true;
            }
            ImGui::PushID(&w);
            ImGui::TextColored(ui::kTextDim, "%s (in a subgraph)", title.c_str());
            ImGui::SameLine();
            CopyButton("cp", w.value, app, "the text");
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(Truncate(w.value, 1200).c_str());
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            ImGui::PopID();
        }
        bool models = false;
        std::vector<const WfWidget*> modelCandidates;
        for (const WfNode& node : g.nodes) {
            for (const WfWidget& w : node.widgets) modelCandidates.push_back(&w);
        }
        for (const auto& d : g.deep) modelCandidates.push_back(&d.second);
        for (const WfWidget* wp : modelCandidates) {
            const WfWidget& w = *wp;
            {
                static const char* kKeys[] = {"ckpt_name", "unet_name", "vae_name", "lora_name", "clip_name", "clip_name1", "clip_name2",
                                              "control_net_name", "model_name", "upscale_model"};
                if (std::none_of(std::begin(kKeys), std::end(kKeys), [&](const char* k) { return w.name == k; })) continue;
                if (w.value.empty() || w.value == "None") continue;
                if (!models) {
                    ui::SectionHeading("MODELS");
                    models = true;
                }
                ImGui::TextColored(ui::kTextDim, "%s", w.name.c_str());
                ImGui::SameLine();
                ImGui::TextWrapped("%s", w.value.c_str());
            }
        }
        if (g.nodes.empty()) ImGui::TextDisabled("Nothing to show.");
        ImGui::EndChild();
        return;
    }
    if (!n->subgraphId.empty()) {
        // placed here so the button is the first thing under the title
    }

    ImGui::PushFont(ui::BoldFont());
    ImGui::TextWrapped("%s", n->title.c_str());
    ImGui::PopFont();
    ImGui::TextColored(ui::kTextDim, "%s  -  node #%s%s", n->type.c_str(), n->idText.c_str(),
                       n->mode == 2 ? "  -  muted" : (n->mode == 4 ? "  -  bypassed" : ""));
    if (ImGui::SmallButton("Deselect")) s.selNode = -1;
    if (!n->subgraphId.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Open subgraph")) {
            const WfNode copy = *n;
            EnterSubgraph(s, copy);
            ImGui::EndChild();
            return;
        }
    }

    if (!n->widgets.empty()) {
        ui::SectionHeading("VALUES");
        for (size_t i = 0; i < n->widgets.size(); ++i) {
            const WfWidget& w = n->widgets[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TextColored(ui::kTextDim, "%s", w.name.c_str());
            ImGui::SameLine();
            CopyButton("cp", w.value, app, "the value");
            if (w.multiline) {
                const float lines = 1.0f + static_cast<float>(w.value.size()) / 34.0f + static_cast<float>(std::count(w.value.begin(), w.value.end(), '\n'));
                const float boxH = std::clamp(ImGui::GetTextLineHeight() * (lines + 0.8f), ui::Dp(52.0f), ui::Dp(300.0f));
                ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(24, 24, 28, 255));
                ImGui::BeginChild("##v", ImVec2(-1, boxH), true);
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(w.value.c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndChild();
                ImGui::PopStyleColor();
            } else {
                ImGui::TextWrapped("%s", w.value.c_str());
            }
            ImGui::PopID();
        }
    }
    auto linkList = [&](const char* heading, bool inputs) {
        bool shown = false;
        for (const WfLink& l : g.links) {
            const int other = inputs ? (l.to == n->id ? l.from : -1) : (l.from == n->id ? l.to : -1);
            if (other < 0) continue;
            const WfNode* o = g.Find(other);
            if (!o) continue;
            if (!shown) {
                ui::SectionHeading(heading);
                shown = true;
            }
            const int slot = inputs ? l.toSlot : l.fromSlot;
            const auto& ports = inputs ? n->inputs : n->outputs;
            const std::string port = slot >= 0 && slot < static_cast<int>(ports.size()) ? ports[slot].name : std::string("?");
            ImGui::PushID(l.id * 2 + (inputs ? 0 : 1) + other * 1000);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Rgb(LinkColor(l.type))), "%s", port.c_str());
            ImGui::SameLine();
            ImGui::TextUnformatted(inputs ? "from" : "to");
            ImGui::SameLine();
            if (ImGui::SmallButton(o->title.c_str())) {
                s.selNode = o->id;
                s.centerOn = o->id;
            }
            ImGui::PopID();
        }
    };
    linkList("INPUTS FROM", true);
    linkList("OUTPUTS TO", false);
    ImGui::EndChild();
}

void DrawLibrary(AppState& app, WorkflowViewState& s, float height) {
    ImGui::BeginChild("wf_library", ImVec2(0, height), true);
    ImGuiStyle& style = ImGui::GetStyle();

    const bool haveSel = app.selectedIndex >= 0 && app.selectedIndex < static_cast<int>(app.entries.size());
    bool selHas = false;
    if (haveSel) selHas = (app.entries[app.selectedIndex].metadata.workflowMask & ViewableWorkflowMask()) != 0;
    ImGui::BeginDisabled(!selHas);
    if (ui::PrimaryButton("Extract from the selected image", ImVec2(-1, 0))) {
        const ImageEntry& e = app.entries[app.selectedIndex];
        OpenInNodeViewer(app, e.path, ReadImageMetadata(e.path)); // the gallery's copy leaves out whole workflows
        SaveCurrent(app);
    }
    ImGui::EndDisabled();
    if (!selHas) {
        ImGui::TextDisabled(haveSel ? "The selected gallery image has no ComfyUI workflow." : "Select an image in the gallery first.");
    }

    // every ComfyUI image currently shown in the gallery
    std::vector<std::wstring> viewPaths;
    for (int idx : app.visibleOrder) {
        if (idx < 0 || idx >= static_cast<int>(app.entries.size())) continue;
        const ImageEntry& e = app.entries[idx];
        if (e.thumbReady && e.metadata.generator == Generator::ComfyUI) viewPaths.push_back(e.path);
    }
    const bool running = s.batch.Running();
    ImGui::BeginDisabled(running || viewPaths.empty());
    char label[96];
    snprintf(label, sizeof(label), "Extract all %d ComfyUI images in view", static_cast<int>(viewPaths.size()));
    if (ImGui::Button(label, ImVec2(-1, 0))) s.batch.Start(app.Device(), viewPaths);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        ImGui::SetTooltip("Saves the workflow of every ComfyUI image the gallery currently shows (respecting its filters and search)\n"
                          "into the library so you can browse them here without the images.");
    }
    if (running) {
        const float frac = s.batch.Total() ? static_cast<float>(s.batch.Done()) / s.batch.Total() : 0.0f;
        char text[48];
        snprintf(text, sizeof(text), "%d / %d", s.batch.Done(), s.batch.Total());
        ImGui::ProgressBar(frac, ImVec2(-ui::Dp(70.0f), 0), text);
        ImGui::SameLine();
        if (ImGui::Button("Stop")) s.batch.Stop();
    } else if (!s.message.empty()) {
        ImGui::TextColored(ui::kGood, "%s", s.message.c_str());
    }

    const float half = (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x) * 0.5f;
    if (ImGui::Button("Open a file...", ImVec2(half, 0))) OpenFromFile(app);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("A ComfyUI .json workflow, or any image that has one inside");
    ImGui::SameLine();
    if (ImGui::Button("Open library folder", ImVec2(-1, 0))) {
        CreateDirectoryW(WorkflowLibraryDir().c_str(), nullptr);
        ShellExecuteW(nullptr, L"open", WorkflowLibraryDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    ImGui::Spacing();
    ui::SectionHeading("LIBRARY");
    static char searchBuf[128] = {0};
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##wfsearch", "Search the library", searchBuf, sizeof(searchBuf))) s.listSearch = searchBuf;
    const std::string q = Lower(s.listSearch);

    std::vector<int> order;
    for (int i = 0; i < static_cast<int>(s.lib.size()); ++i) {
        const WorkflowLibItem& it = s.lib[i];
        if (!q.empty() && Lower(it.name).find(q) == std::string::npos && Lower(it.summary).find(q) == std::string::npos) continue;
        order.push_back(i);
    }
    ImGui::TextColored(ui::kTextDim, "%d of %d saved%s", static_cast<int>(order.size()), static_cast<int>(s.lib.size()), s.libLoading ? "  (loading...)" : "");

    ImGui::BeginChild("wf_list", ImVec2(0, 0), false);
    if (s.lib.empty() && !s.libLoading) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Nothing saved yet. Select a ComfyUI image in the gallery and press \"Extract from the selected image\", "
                            "or extract everything shown in the gallery at once.");
        ImGui::PopTextWrapPos();
    }
    const float rowH = ui::Dp(66.0f);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(order.size()), rowH + style.ItemSpacing.y);
    int deleteIndex = -1;
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const int idx = order[row];
            const WorkflowLibItem& it = s.lib[idx];
            ImGui::PushID(idx);
            const bool selected = !s.libPath.empty() && _wcsicmp(s.libPath.c_str(), it.jsonPath.c_str()) == 0;
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x;
            if (ImGui::Selectable("##row", selected, 0, ImVec2(0, rowH))) OpenLibraryItem(app, it);
            if (ImGui::BeginPopupContextItem("##wfmenu")) {
                if (ImGui::MenuItem("Open")) OpenLibraryItem(app, it);
                if (ImGui::MenuItem("Copy workflow")) {
                    std::string text, api, err;
                    if (ReadWorkflowFromFile(it.jsonPath, text, api, &err)) {
                        ImGui::SetClipboardText(text.c_str());
                        app.SetStatus("Copied the workflow.");
                    }
                }
                if (ImGui::MenuItem("Show in Explorer")) {
                    std::wstring args = L"/select,\"" + it.jsonPath + L"\"";
                    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
                }
                if (ImGui::MenuItem("Show the source image", nullptr, false, !it.source.empty() && fs::exists(ToWide(it.source)))) {
                    app.OpenFile(ToWide(it.source));
                    app.requestTab = 0;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Remove from the library")) deleteIndex = idx;
                ImGui::EndPopup();
            }
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float pad = ui::Dp(4.0f);
            const float box = rowH - pad * 2;
            dl->AddRectFilled(ImVec2(p.x + pad, p.y + pad), ImVec2(p.x + pad + box, p.y + pad + box), ui::ThumbPlaceholder(), 4.0f);
            if (!it.thumbPath.empty()) {
                const AppState::LibThumb th = app.LibraryThumb(it.thumbPath);
                if (th.srv && th.w > 0 && th.h > 0) {
                    const float k = std::min(box / th.w, box / th.h);
                    const float tw = th.w * k, thh = th.h * k;
                    const ImVec2 a(p.x + pad + (box - tw) * 0.5f, p.y + pad + (box - thh) * 0.5f);
                    dl->AddImage((ImTextureID)(intptr_t)th.srv, a, ImVec2(a.x + tw, a.y + thh));
                }
            }
            const float tx = p.x + pad * 2 + box;
            const float textW = w - (tx - p.x) - pad;
            const ImVec4 clip(tx, p.y, tx + textW, p.y + rowH);
            const float lh = ImGui::GetTextLineHeight();
            AddTextClipped(dl, ImGui::GetFontSize(), ImVec2(tx, p.y + pad), ImGui::GetColorU32(ImGuiCol_Text), it.name, clip);
            AddTextClipped(dl, ImGui::GetFontSize(), ImVec2(tx, p.y + pad + lh + 2), ImGui::GetColorU32(ui::kTextDim), it.summary, clip);
            AddTextClipped(dl, ImGui::GetFontSize(), ImVec2(tx, p.y + pad + (lh + 2) * 2), ImGui::GetColorU32(ui::kTextDim),
                           (it.kindLabel.find("API") != std::string::npos ? "API prompt" : "workflow") + std::string("  -  ") + DateText(it.saved), clip);
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::EndChild();

    if (deleteIndex >= 0 && deleteIndex < static_cast<int>(s.lib.size())) {
        const WorkflowLibItem it = s.lib[deleteIndex];
        const std::wstring prompt = L"Remove \"" + ToWide(it.name) + L"\" from the workflow library?\n\nThe image it came from is not touched.";
        if (MessageBoxW(GetActiveWindow(), prompt.c_str(), L"Workflow library", MB_YESNO | MB_ICONQUESTION) == IDYES) {
            DeleteWorkflowFromLibrary(it);
            if (!s.libPath.empty() && _wcsicmp(s.libPath.c_str(), it.jsonPath.c_str()) == 0) s.libPath.clear();
            s.libDirty = true;
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// shared with the gallery
// ---------------------------------------------------------------------------
void OpenInNodeViewer(AppState& app, const std::wstring& imagePath, const ImageMetadata& meta) {
    std::vector<EmbeddedWorkflow> found = FindWorkflows(meta);
    const EmbeddedWorkflow* wf = FindViewableWorkflow(found);
    if (!wf) {
        app.SetStatus("This image has no ComfyUI workflow in it.");
        return;
    }
    std::string api;
    if (wf->kind == WorkflowKind::ComfyUI) {
        for (const EmbeddedWorkflow& e : found) {
            if (e.kind == WorkflowKind::ComfyApi) api = *e.text;
        }
    }
    const std::string name = ToUtf8(fs::path(imagePath).stem().wstring());
    // keep the library entry highlighted when the same workflow is already saved
    LoadGraph(app, *wf->text, api, name, imagePath, std::wstring(), wf->label);
    app.requestTab = 2;
}

void PumpWorkflows(AppState& app) {
    WorkflowViewState& s = app.wf;
    if (s.libLoading && s.libFuture.valid() && s.libFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        s.lib = s.libFuture.get();
        s.libLoading = false;
        s.libLoaded = true;
    }
    std::string message;
    if (s.batch.TakeResult(message)) {
        s.message = message;
        s.libDirty = true;
        app.SetStatus(message);
    }
}

void DrawWorkflowTab(AppState& app, float height) {
    WorkflowViewState& s = app.wf;
    if (s.libDirty && !s.libLoading) {
        s.libDirty = false;
        s.libLoading = true;
        s.libFuture = std::async(std::launch::async, []() { return ScanWorkflowLibrary(); });
    }

    ImGuiTableFlags flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings;
    if (!ImGui::BeginTable("wf_layout", 2, flags, ImVec2(0, height))) return;
    ImGui::TableSetupColumn("Library", ImGuiTableColumnFlags_WidthStretch, 0.27f);
    ImGui::TableSetupColumn("Viewer", ImGuiTableColumnFlags_WidthStretch, 0.73f);
    ImGui::TableNextRow(0, height);

    ImGui::TableSetColumnIndex(0);
    DrawLibrary(app, s, height);

    ImGui::TableSetColumnIndex(1);
    ImGui::BeginChild("wf_right", ImVec2(0, height), false);
    if (!s.open) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::PushFont(ui::BoldFont());
        ImGui::TextUnformatted("ComfyUI node viewer");
        ImGui::PopFont();
        ImGui::TextDisabled("See how the workflow behind an image looks: every node, its values and how they are wired together.\n\n"
                            "- In the gallery, select a ComfyUI image and use \"View workflow\" in the details panel (or right-click it).\n"
                            "- \"Extract\" on the left saves workflows into this program's library so you can browse them later, "
                            "even after the images are gone.\n"
                            "- \"Open a file...\" shows any ComfyUI .json workflow or an image that has one inside.");
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        ImGui::EndTable();
        return;
    }

    // toolbar
    ImGui::PushFont(ui::BoldFont());
    ImGui::TextUnformatted(s.title.c_str());
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextColored(ui::kTextDim, "%s  -  %s", s.kindLabel.c_str(), s.summary.c_str());

    if (!s.subPath.empty()) {
        // where we are: Workflow > Subgraph > ... (click a name to go back up to it)
        if (ImGui::SmallButton((s.title.empty() ? std::string("Top level") : s.title).c_str())) LeaveSubgraph(s, 0);
        for (size_t i = 0; i < s.subNames.size(); ++i) {
            ImGui::SameLine();
            ImGui::TextDisabled(">");
            ImGui::SameLine();
            ImGui::PushID(static_cast<int>(i));
            if (i + 1 < s.subNames.size()) {
                if (ImGui::SmallButton(s.subNames[i].c_str())) LeaveSubgraph(s, i + 1);
            } else {
                ImGui::TextUnformatted(s.subNames[i].c_str());
            }
            ImGui::PopID();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Up")) LeaveSubgraph(s, s.subPath.size() - 1);
    }
    if (ImGui::Button("Fit")) s.viewFresh = true;
    ImGui::SameLine();
    if (ImGui::Button("100%")) {
        s.zoom = 1.0f;
        if (const WfNode* n = s.graph.Find(s.selNode)) s.centerOn = n->id;
    }
    ImGui::SameLine();
    if (ImGui::Button(s.graph.fromApi && s.api.empty() ? "Copy API prompt" : "Copy workflow")) {
        ImGui::SetClipboardText(s.text.c_str());
        app.SetStatus("Copied it. Paste it into ComfyUI with Ctrl+V on the canvas.");
    }
    if (!s.api.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Copy API prompt")) {
            ImGui::SetClipboardText(s.api.c_str());
            app.SetStatus("Copied the API prompt.");
        }
    }
    ImGui::SameLine();
    const bool saved = !s.libPath.empty();
    ImGui::BeginDisabled(saved);
    if (ImGui::Button(saved ? "In the library" : "Save to library")) SaveCurrent(app);
    ImGui::EndDisabled();
    if (!s.sourceImage.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Show source image")) {
            app.OpenFile(s.sourceImage);
            app.requestTab = 0;
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ui::Dp(190.0f));
    static char nodeBuf[128] = {0};
    bool jump = false;
    if (ImGui::InputTextWithHint("##nodesearch", "Find a node or value", nodeBuf, sizeof(nodeBuf), ImGuiInputTextFlags_EnterReturnsTrue)) jump = true;
    s.nodeSearch = nodeBuf;
    if (!s.nodeSearch.empty()) {
        const std::string q = Lower(s.nodeSearch);
        std::vector<int> hits;
        for (const WfNode& n : s.graph.nodes) {
            if (NodeMatches(n, q)) hits.push_back(n.id);
        }
        ImGui::SameLine();
        ImGui::TextColored(hits.empty() ? ui::kBad : ui::kGood, "%d found", static_cast<int>(hits.size()));
        if (jump && !hits.empty()) {
            auto it = std::find(hits.begin(), hits.end(), s.selNode);
            const int next = (it == hits.end() || it + 1 == hits.end()) ? hits.front() : *(it + 1);
            s.selNode = next;
            s.centerOn = next;
        }
    }

    if (!s.error.empty()) {
        ImGui::TextColored(ui::kBad, "%s", s.error.c_str());
    } else {
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float bar = ui::Dp(8.0f);
        const float inspector = std::clamp(s.inspectorW, ui::Dp(220.0f), std::max(ui::Dp(220.0f), avail.x * 0.6f));
        DrawCanvas(app, s, ImVec2(avail.x - inspector - bar, avail.y));
        ImGui::SameLine(0, 0);
        ImGui::InvisibleButton("##wfsplit", ImVec2(bar, avail.y));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemActive()) s.inspectorW = std::clamp(s.inspectorW - ImGui::GetIO().MouseDelta.x, ui::Dp(220.0f), avail.x * 0.6f);
        ImGui::SameLine(0, 0);
        DrawInspector(app, s, ImVec2(0, avail.y));
    }
    ImGui::EndChild();
    ImGui::EndTable();
}
