#include "clef.h"
#include "clef_grades.h"
#include "clef_tags.h"
#include "ui_extra.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>

#include "imgui.h"
#include "ui_theme.h"
#include "lib_sort.h"
#include "clipboard_image.h"
#include "rename_template.h"
#include "metadata_strip.h"
#include "win_dialogs.h"
#include "imgui_internal.h"
#include <shellapi.h>

namespace fs = std::filesystem;

bool g_boxSelecting = false;

namespace {

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(len > 0 ? len - 1 : 0, '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring ToWide2(const std::string& s) {
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

std::string BaseName(const ImageEntry& e) { return ToUtf8(fs::path(e.displayName).filename().wstring()); }

void CenterNextWindow(float w, float h) {
    ImGui::SetNextWindowSize(ImVec2(ui::Dp(w), ui::Dp(h)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
}

} // namespace

// ---------------------------------------------------------------------------
// Rating stars
// ---------------------------------------------------------------------------

namespace ui {

bool RatingStars(const char* id, int& rating) {
    ImGui::PushID(id);
    const char* star = "\xE2\x98\x85"; // U+2605
    const ImVec2 glyph = ImGui::CalcTextSize(star);
    const float gap = Dp(3.0f);
    const float starW = glyph.x + gap;
    const ImVec2 size(starW * 5.0f, std::max(glyph.y, ImGui::GetFrameHeight()));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##stars", size);
    const bool hovered = ImGui::IsItemHovered();
    int hoverStar = 0;
    if (hovered) hoverStar = std::clamp(static_cast<int>((ImGui::GetIO().MousePos.x - p.x) / starW) + 1, 1, 5);
    bool changed = false;
    if (ImGui::IsItemClicked() && hoverStar > 0) {
        rating = (hoverStar == rating) ? 0 : hoverStar;
        changed = true;
    }
    const int shown = hovered ? hoverStar : rating;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 1; i <= 5; ++i) {
        dl->AddText(ImVec2(p.x + (i - 1) * starW, p.y + (size.y - glyph.y) * 0.5f), StarColor(i <= shown), star);
    }
    if (hovered) ImGui::SetTooltip("%s", hoverStar == rating ? "Click to clear the rating" : "Rate this image (keys 1-5, 0 clears)");
    ImGui::PopID();
    return changed;
}

} // namespace ui

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

namespace {

struct Stats {
    int total = 0, withMetadata = 0;
    unsigned long long bytes = 0;
    std::map<std::string, int> generators;
    std::map<int, int> months; // year*100+month
    int people[8] = {};        // 0..6, 7 = unknown
    int ratings[6] = {};
    std::unordered_map<std::string, int> models, samplers, tags;
    double stepsSum = 0, cfgSum = 0;
    int stepsCount = 0, cfgCount = 0;
};

// "(blue hair:1.2)" -> "blue hair"
std::string NormalizeTag(std::string t) {
    t = Lower(t);
    size_t colon = t.rfind(':');
    if (colon != std::string::npos && colon + 1 < t.size() &&
        std::all_of(t.begin() + colon + 1, t.end(), [](unsigned char c) { return std::isdigit(c) || c == '.' || c == ')' || c == ']'; })) {
        t.erase(colon);
    }
    while (!t.empty() && (t.front() == '(' || t.front() == '[' || t.front() == '{' || t.front() == ' ')) t.erase(t.begin());
    while (!t.empty() && (t.back() == ')' || t.back() == ']' || t.back() == '}' || t.back() == ' ')) t.pop_back();
    return t;
}

std::string FindParam(const KeyValueList& params, const char* key) {
    for (const auto& kv : params) {
        if (_stricmp(kv.first.c_str(), key) == 0) return kv.second;
    }
    return {};
}

void Accumulate(Stats& s, const ImageEntry& e) {
    s.total++;
    s.bytes += e.size;
    s.ratings[std::clamp(e.rating, 0, 5)]++;
    if (e.year) s.months[e.year * 100 + e.month]++;
    if (!e.thumbReady) return;
    if (e.metadata.hasMetadata) {
        s.withMetadata++;
        s.generators[GeneratorName(e.metadata.generator)]++;
    }
    s.people[e.people < 0 ? 7 : std::min(e.people, 6)]++;
    const auto& params = e.metadata.parsed.params;
    std::string model = FindParam(params, "Model");
    if (!model.empty()) s.models[model]++;
    std::string sampler = FindParam(params, "Sampler");
    if (!sampler.empty()) s.samplers[sampler]++;
    char* end = nullptr;
    std::string steps = FindParam(params, "Steps");
    if (!steps.empty()) {
        double v = std::strtod(steps.c_str(), &end);
        if (end != steps.c_str()) { s.stepsSum += v; s.stepsCount++; }
    }
    std::string cfg = FindParam(params, "CFG scale");
    if (!cfg.empty()) {
        double v = std::strtod(cfg.c_str(), &end);
        if (end != cfg.c_str()) { s.cfgSum += v; s.cfgCount++; }
    }
    for (const std::string& tag : e.metadata.parsed.tags) {
        std::string n = NormalizeTag(tag);
        if (n.size() >= 2 && n.size() <= 60) s.tags[n]++;
    }
}

template <typename Map>
std::vector<std::pair<std::string, int>> TopN(const Map& m, size_t n) {
    std::vector<std::pair<std::string, int>> v(m.begin(), m.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
    if (v.size() > n) v.resize(n);
    return v;
}

// One labelled horizontal bar.
void Bar(const char* label, int value, int maxValue, float labelWidth) {
    const float h = ImGui::GetTextLineHeight() + ui::Dp(6.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float valueW = ImGui::CalcTextSize("000000").x;
    const float barX = p.x + labelWidth;
    const float barW = std::max(ui::Dp(20.0f), avail - labelWidth - valueW - ui::Dp(8.0f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::Dummy(ImVec2(avail, h));
    ImVec2 textPos(p.x, p.y + ui::Dp(3.0f));
    dl->PushClipRect(p, ImVec2(p.x + labelWidth - ui::Dp(6.0f), p.y + h), true);
    dl->AddText(textPos, ImGui::GetColorU32(ImGuiCol_Text), label);
    dl->PopClipRect();
    const float frac = maxValue > 0 ? static_cast<float>(value) / maxValue : 0.0f;
    dl->AddRectFilled(ImVec2(barX, p.y + ui::Dp(3.0f)), ImVec2(barX + barW, p.y + h - ui::Dp(3.0f)), ImGui::GetColorU32(ImGuiCol_FrameBg), ui::Dp(4.0f));
    if (frac > 0)
        dl->AddRectFilled(ImVec2(barX, p.y + ui::Dp(3.0f)), ImVec2(barX + std::max(ui::Dp(3.0f), barW * frac), p.y + h - ui::Dp(3.0f)),
                          ImGui::GetColorU32(ui::kAccent), ui::Dp(4.0f));
    char num[24];
    snprintf(num, sizeof(num), "%d", value);
    dl->AddText(ImVec2(barX + barW + ui::Dp(8.0f), textPos.y), ImGui::GetColorU32(ui::kTextDim), num);
}

void BarList(const std::vector<std::pair<std::string, int>>& rows, float labelWidth) {
    int maxValue = 0;
    for (const auto& r : rows) maxValue = std::max(maxValue, r.second);
    for (const auto& r : rows) Bar(r.first.c_str(), r.second, maxValue, labelWidth);
}

} // namespace

bool FileStillThere(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

int DrawListCleanup(const char* id, bool any) {
    int act = 0;
    ImGui::PushID(id);
    ImGui::BeginDisabled(!any);
    if (ImGui::SmallButton("Take off missing")) act = 1;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Takes off the entries that have no picture left to show:\nmoved or deleted files, and failed ones. Nothing is deleted.");
    const float clearW = ImGui::CalcTextSize("Clear the list...").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    if (ImGui::GetContentRegionAvail().x - ImGui::GetItemRectSize().x - ImGui::GetStyle().ItemSpacing.x >= clearW) ImGui::SameLine(); // (a narrow column: under it)
    if (ImGui::SmallButton("Clear the list...")) ImGui::OpenPopup("##clearlist");
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("##clearlist")) {
        ImGui::TextUnformatted("Clear this list?");
        ImGui::TextDisabled("Every finished entry goes (what is still being made stays).\nThe pictures themselves stay where they are on disk.");
        if (ui::PrimaryButton("Clear it")) {
            act = 2;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::Separator();
    ImGui::PopID();
    return act;
}

void DrawNsfwCover(AppState& app, const std::wstring& path, ImVec2 a, ImVec2 b) {
    const char* text = "Hidden: NSFW   -   Show it";
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const ImVec2 pad(ui::Dp(12.0f), ui::Dp(7.0f));
    const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
    const ImVec2 r0(c.x - ts.x * 0.5f - pad.x, c.y - ts.y * 0.5f - pad.y), r1(c.x + ts.x * 0.5f + pad.x, c.y + ts.y * 0.5f + pad.y);
    if (r1.x - r0.x > b.x - a.x || r1.y - r0.y > b.y - a.y) return; // (too small for it)
    const bool hot = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && ImGui::IsMouseHoveringRect(r0, r1);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(r0, r1, hot ? IM_COL32(40, 44, 54, 235) : IM_COL32(16, 18, 24, 210), ui::Dp(16.0f));
    dl->AddRect(r0, r1, IM_COL32(255, 255, 255, hot ? 120 : 50), ui::Dp(16.0f));
    dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), IM_COL32(235, 237, 242, 255), text);
    if (hot) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("Shows this picture until the app closes.\nBlurring is under View > Blur NSFW pictures.");
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            std::wstring key = path;
            std::transform(key.begin(), key.end(), key.begin(), ::towlower);
            app.nsfwRevealed.insert(key);
        }
    }
}

void DrawNsfwOverlay(AppState& app, const std::wstring& path, ImVec2 a, ImVec2 b) {
    if (!app.blurNsfw || path.empty()) return;
    if (app.ShouldBlur(path)) {
        DrawNsfwCover(app, path, a, b);
        return;
    }
    std::wstring key = path;
    std::transform(key.begin(), key.end(), key.begin(), ::towlower);
    if (!app.nsfwRevealed.count(key) || !ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) || !ImGui::IsMouseHoveringRect(a, b)) return;
    // shown on purpose: while the pointer is over it, "Blur it again" in its corner
    const char* text = "Blur it again";
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const ImVec2 pad(ui::Dp(9.0f), ui::Dp(5.0f));
    const ImVec2 r1(b.x - ui::Dp(8.0f), a.y + ui::Dp(8.0f) + ts.y + pad.y * 2), r0(r1.x - ts.x - pad.x * 2, a.y + ui::Dp(8.0f));
    if (r0.x < a.x || r1.y > b.y) return; // (too small for it)
    const bool hot = ImGui::IsMouseHoveringRect(r0, r1);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(r0, r1, hot ? IM_COL32(40, 44, 54, 240) : IM_COL32(16, 18, 24, 200), ui::Dp(12.0f));
    dl->AddRect(r0, r1, IM_COL32(255, 255, 255, hot ? 120 : 50), ui::Dp(12.0f));
    dl->AddText(ImVec2(r0.x + pad.x, r0.y + pad.y), IM_COL32(235, 237, 242, 255), text);
    if (hot) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("Hides it again (you showed it).\nRight-click a picture to mark it NSFW or not for good.");
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) app.nsfwRevealed.erase(key);
    }
}

void NsfwMarkMenu(AppState& app, const std::wstring& path) {
    if (path.empty()) return;
    const int m = app.NsfwMark(path);
    ImGui::Separator();
    if (ImGui::MenuItem("Mark as NSFW (always blurred)", nullptr, m == 1)) app.SetNsfwMark(path, m == 1 ? -1 : 1);
    if (ImGui::MenuItem("Mark as not NSFW (never blurred)", nullptr, m == 0)) app.SetNsfwMark(path, m == 0 ? -1 : 0);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("Your call wins over the classifier's score, everywhere the picture shows. Click a ticked one to undo it.");
    std::wstring key = path;
    std::transform(key.begin(), key.end(), key.begin(), ::towlower);
    if (app.nsfwRevealed.count(key) && ImGui::MenuItem("Blur it again")) app.nsfwRevealed.erase(key);
}

void DrawStatsWindow(AppState& app) {
    if (!app.showStats) return;
    CenterNextWindow(600, 760);
    if (!ImGui::Begin("Statistics", &app.showStats, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    static bool onlyView = true;
    ImGui::Checkbox("Only the images in the current view (search and filters applied)", &onlyView);

    static Stats stats;
    static unsigned cachedEpoch = ~0u;
    static bool cachedOnlyView = false;
    static size_t cachedViewSize = 0, cachedEntries = 0;
    if (cachedEpoch != app.filterEpoch || cachedOnlyView != onlyView || cachedViewSize != app.visibleOrder.size() ||
        cachedEntries != app.entries.size()) {
        cachedEpoch = app.filterEpoch;
        cachedOnlyView = onlyView;
        cachedViewSize = app.visibleOrder.size();
        cachedEntries = app.entries.size();
        stats = Stats();
        if (onlyView) {
            for (int i : app.visibleOrder) {
                if (i >= 0 && i < static_cast<int>(app.entries.size())) Accumulate(stats, app.entries[i]);
            }
        } else {
            for (const ImageEntry& e : app.entries) Accumulate(stats, e);
        }
    }
    const Stats& s = stats;

    if (s.total == 0) {
        ImGui::Spacing();
        ImGui::TextDisabled("Open a folder to see statistics.");
        ImGui::End();
        return;
    }
    if (app.IsLoading()) ImGui::TextColored(ui::kWarn, "Still scanning: %d / %d images read", app.LoadedCount(), app.TotalCount());

    const float labelW = ui::Dp(230.0f);
    ImGui::BeginChild("stats_scroll", ImVec2(0, 0), false);

    ui::SectionHeading("OVERVIEW");
    ImGui::Text("%d images  -  %s", s.total, ui::FormatBytes(s.bytes).c_str());
    ImGui::Text("%d with generation metadata (%d%%)", s.withMetadata, s.total ? s.withMetadata * 100 / s.total : 0);
    if (s.stepsCount) ImGui::Text("Average steps %.1f", s.stepsSum / s.stepsCount);
    if (s.cfgCount) ImGui::Text("Average CFG scale %.1f", s.cfgSum / s.cfgCount);

    if (!s.generators.empty()) {
        ImGui::Spacing();
        ui::SectionHeading("GENERATION METHOD");
        BarList(TopN(s.generators, 8), labelW);
    }

    if (!s.months.empty()) {
        ImGui::Spacing();
        ui::SectionHeading("IMAGES PER MONTH");
        // Continuous month range, most recent 48 months.
        int firstKey = s.months.begin()->first, lastKey = s.months.rbegin()->first;
        auto next = [](int key) { return key % 100 == 12 ? (key / 100 + 1) * 100 + 1 : key + 1; };
        std::vector<std::pair<int, int>> series;
        for (int k = firstKey; k <= lastKey; k = next(k)) {
            auto it = s.months.find(k);
            series.push_back({k, it == s.months.end() ? 0 : it->second});
        }
        if (series.size() > 48) series.erase(series.begin(), series.end() - 48);
        int maxV = 1;
        for (auto& p : series) maxV = std::max(maxV, p.second);
        const float chartH = ui::Dp(110.0f);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float chartW = ImGui::GetContentRegionAvail().x;
        ImGui::InvisibleButton("##months", ImVec2(chartW, chartH));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float slot = chartW / series.size();
        const int hoverIdx = ImGui::IsItemHovered() ? static_cast<int>((ImGui::GetIO().MousePos.x - p0.x) / slot) : -1;
        for (size_t i = 0; i < series.size(); ++i) {
            float hgt = series[i].second ? std::max(ui::Dp(2.0f), (chartH - ui::Dp(4.0f)) * series[i].second / maxV) : 0.0f;
            ImVec2 a(p0.x + i * slot + slot * 0.12f, p0.y + chartH - hgt);
            ImVec2 b(p0.x + (i + 1) * slot - slot * 0.12f, p0.y + chartH);
            dl->AddRectFilled(a, b, ImGui::GetColorU32(static_cast<int>(i) == hoverIdx ? ui::kAccentHover : ui::kAccent), ui::Dp(2.0f));
        }
        dl->AddLine(ImVec2(p0.x, p0.y + chartH), ImVec2(p0.x + chartW, p0.y + chartH), ImGui::GetColorU32(ImGuiCol_Border));
        if (hoverIdx >= 0 && hoverIdx < static_cast<int>(series.size())) {
            ImGui::SetTooltip("%s %d: %d image%s", MonthName(series[hoverIdx].first % 100), series[hoverIdx].first / 100,
                              series[hoverIdx].second, series[hoverIdx].second == 1 ? "" : "s");
        }
        ImGui::TextDisabled("%s %d  to  %s %d", MonthName(series.front().first % 100), series.front().first / 100,
                            MonthName(series.back().first % 100), series.back().first / 100);
    }

    ImGui::Spacing();
    ui::SectionHeading("PEOPLE IN IMAGE");
    {
        std::vector<std::pair<std::string, int>> rows;
        static const char* kNames[] = {"None", "1 (solo)", "2", "3", "4", "5", "6 or more", "Unknown"};
        for (int i = 0; i < 8; ++i) {
            if (s.people[i]) rows.push_back({kNames[i], s.people[i]});
        }
        BarList(rows, labelW);
    }

    bool anyRated = false;
    for (int i = 1; i <= 5; ++i) anyRated |= s.ratings[i] > 0;
    if (anyRated) {
        ImGui::Spacing();
        ui::SectionHeading("RATINGS");
        std::vector<std::pair<std::string, int>> rows;
        for (int i = 5; i >= 1; --i) {
            if (s.ratings[i]) rows.push_back({std::to_string(i) + (i == 1 ? " star" : " stars"), s.ratings[i]});
        }
        rows.push_back({"Unrated", s.ratings[0]});
        BarList(rows, labelW);
    }

    if (!s.models.empty()) {
        ImGui::Spacing();
        ui::SectionHeading("TOP MODELS");
        BarList(TopN(s.models, 10), labelW);
    }
    if (!s.samplers.empty()) {
        ImGui::Spacing();
        ui::SectionHeading("TOP SAMPLERS");
        BarList(TopN(s.samplers, 8), labelW);
    }
    if (!s.tags.empty()) {
        ImGui::Spacing();
        ui::SectionHeading("MOST USED TAGS");
        BarList(TopN(s.tags, 30), labelW);
    }
    ImGui::EndChild();
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Compare
// ---------------------------------------------------------------------------

namespace {

std::vector<std::string> NormalizedTags(const std::vector<std::string>& tags) {
    std::vector<std::string> out;
    for (const auto& t : tags) {
        std::string n = NormalizeTag(t);
        if (!n.empty()) out.push_back(n);
    }
    return out;
}

void ChipList(const char* id, const std::vector<std::string>& tags, const ImVec4& bg) {
    if (tags.empty()) {
        ImGui::TextDisabled("(none)");
        return;
    }
    ImGui::PushID(id);
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ui::Dp(12.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ui::Dp(9.0f), ui::Dp(3.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui::Dp(6.0f), ui::Dp(6.0f)));
    ImGui::PushStyleColor(ImGuiCol_Button, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, bg);
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    for (size_t i = 0; i < tags.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::Button(tags[i].c_str());
        const float x2 = ImGui::GetItemRectMax().x;
        ImGui::PopID();
        if (i + 1 < tags.size()) {
            const float nextW = ImGui::CalcTextSize(tags[i + 1].c_str()).x + style.FramePadding.x * 2.0f;
            if (x2 + style.ItemSpacing.x + nextW < right) ImGui::SameLine();
        }
    }
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(3);
    ImGui::PopID();
}

} // namespace

void DrawCompareWindow(AppState& app) {
    if (!app.showCompare) return;
    const int ia = app.compareA, ib = app.compareB;
    if (ia < 0 || ib < 0 || ia >= static_cast<int>(app.entries.size()) || ib >= static_cast<int>(app.entries.size())) {
        app.CloseCompare();
        return;
    }
    CenterNextWindow(1180, 820);
    bool open = true;
    if (!ImGui::Begin("Compare images", &open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        if (!open) app.CloseCompare();
        return;
    }
    const ImageEntry* ent[2] = {&app.entries[ia], &app.entries[ib]};

    // Zoom and pan are shared so both images always show the same region.
    static float zoomMul = 1.0f;
    static ImVec2 pan(0, 0);
    static int lastA = -1, lastB = -1;
    if (lastA != ia || lastB != ib) {
        lastA = ia;
        lastB = ib;
        zoomMul = 1.0f;
        pan = ImVec2(0, 0);
    }

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float paneH = std::max(ui::Dp(220.0f), avail.y * 0.46f);
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float paneW = (avail.x - gap) * 0.5f;
    ImGuiIO& io = ImGui::GetIO();

    for (int side = 0; side < 2; ++side) {
        if (side) ImGui::SameLine();
        ImGui::PushID(side);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::ViewerBackground());
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::BeginChild("pane", ImVec2(paneW, paneH), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 size = ImGui::GetContentRegionAvail();
        const LoadedImage& tex = app.compareTex[side];
        ImGui::InvisibleButton("##img", size, ImGuiButtonFlags_MouseButtonLeft);
        const bool hovered = ImGui::IsItemHovered();
        if (hovered && io.MouseWheel != 0.0f) {
            const ImVec2 center(origin.x + size.x * 0.5f, origin.y + size.y * 0.5f);
            const float old = zoomMul;
            zoomMul = std::clamp(old * std::pow(1.18f, io.MouseWheel), 0.2f, 40.0f);
            const float k = zoomMul / old;
            pan.x -= (io.MousePos.x - center.x - pan.x) * (k - 1.0f);
            pan.y -= (io.MousePos.y - center.y - pan.y) * (k - 1.0f);
        }
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            pan.x += io.MouseDelta.x;
            pan.y += io.MouseDelta.y;
        }
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            zoomMul = 1.0f;
            pan = ImVec2(0, 0);
        }
        if (tex.srv && tex.width > 0 && tex.height > 0) {
            const float fit = std::min(size.x / tex.width, size.y / tex.height);
            const float scale = fit * zoomMul;
            const ImVec2 dim(tex.width * scale, tex.height * scale);
            const ImVec2 center(origin.x + size.x * 0.5f, origin.y + size.y * 0.5f);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
            dl->AddImage((ImTextureID)(intptr_t)tex.srv, ImVec2(center.x - dim.x * 0.5f + pan.x, center.y - dim.y * 0.5f + pan.y),
                         ImVec2(center.x + dim.x * 0.5f + pan.x, center.y + dim.y * 0.5f + pan.y));
            dl->PopClipRect();
            char label[160];
            snprintf(label, sizeof(label), "%c  %s   %d \xC3\x97 %d   %s", side ? 'B' : 'A', BaseName(*ent[side]).c_str(), tex.width,
                     tex.height, ui::FormatBytes(ent[side]->size).c_str());
            const ImVec2 ts = ImGui::CalcTextSize(label);
            dl->AddRectFilled(ImVec2(origin.x + 8, origin.y + 8), ImVec2(origin.x + 8 + ts.x + ui::Dp(14.0f), origin.y + 8 + ts.y + ui::Dp(8.0f)),
                              IM_COL32(10, 12, 15, 175), ui::Dp(8.0f));
            dl->AddText(ImVec2(origin.x + 8 + ui::Dp(7.0f), origin.y + 8 + ui::Dp(4.0f)), IM_COL32(225, 230, 238, 255), label);
        } else {
            ImGui::GetWindowDrawList()->AddText(origin, ImGui::GetColorU32(ui::kTextDim), "Could not load this image");
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopID();
    }

    if (ImGui::Button("Swap A and B")) {
        std::swap(app.compareTex[0], app.compareTex[1]);
        std::swap(app.compareA, app.compareB);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset zoom")) {
        zoomMul = 1.0f;
        pan = ImVec2(0, 0);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Mouse wheel zooms both, drag pans both, double-click resets.");

    ImGui::Spacing();
    ImGui::BeginChild("cmp_diff", ImVec2(0, 0), false);
    const ImageMetadata& ma = ent[0]->metadata;
    const ImageMetadata& mb = ent[1]->metadata;

    ui::SectionHeading("PARAMETERS");
    {
        std::vector<std::string> keys;
        for (const auto& kv : ma.parsed.params) if (std::find(keys.begin(), keys.end(), kv.first) == keys.end()) keys.push_back(kv.first);
        for (const auto& kv : mb.parsed.params) if (std::find(keys.begin(), keys.end(), kv.first) == keys.end()) keys.push_back(kv.first);
        auto value = [](const KeyValueList& l, const std::string& k) {
            for (const auto& kv : l) if (kv.first == k) return kv.second;
            return std::string();
        };
        if (keys.empty()) {
            ImGui::TextDisabled("Neither image has generation parameters.");
        } else if (ImGui::BeginTable("cmp_params", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, ui::Dp(120.0f));
            ImGui::TableSetupColumn("A", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("B", ImGuiTableColumnFlags_WidthStretch);
            int differing = 0;
            for (const std::string& k : keys) {
                std::string va = value(ma.parsed.params, k), vb = value(mb.parsed.params, k);
                const bool differs = va != vb;
                differing += differs;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(ui::kTextDim, "%s", k.c_str());
                for (int col = 0; col < 2; ++col) {
                    ImGui::TableSetColumnIndex(col + 1);
                    const std::string& v = col ? vb : va;
                    if (v.empty()) ImGui::TextDisabled("-");
                    else if (differs) ImGui::TextColored(ui::kWarn, "%s", v.c_str());
                    else ImGui::TextWrapped("%s", v.c_str());
                }
            }
            ImGui::EndTable();
            ImGui::TextDisabled("%d of %d parameters differ", differing, static_cast<int>(keys.size()));
        }
    }

    ImGui::Spacing();
    ui::SectionHeading("PROMPT TAGS");
    {
        std::vector<std::string> ta = NormalizedTags(ma.parsed.tags), tb = NormalizedTags(mb.parsed.tags);
        std::set<std::string> sa(ta.begin(), ta.end()), sb(tb.begin(), tb.end());
        std::vector<std::string> onlyA, onlyB, both;
        for (const auto& t : sa) (sb.count(t) ? both : onlyA).push_back(t);
        for (const auto& t : sb) if (!sa.count(t)) onlyB.push_back(t);
        const ImVec4 red = ui::IsLight() ? ImVec4(0.98f, 0.82f, 0.82f, 1) : ImVec4(0.42f, 0.18f, 0.18f, 1);
        const ImVec4 green = ui::IsLight() ? ImVec4(0.80f, 0.94f, 0.82f, 1) : ImVec4(0.16f, 0.36f, 0.22f, 1);
        ImGui::TextColored(ui::kBad, "Only in A (%d)", static_cast<int>(onlyA.size()));
        ChipList("onlyA", onlyA, red);
        ImGui::Spacing();
        ImGui::TextColored(ui::kGood, "Only in B (%d)", static_cast<int>(onlyB.size()));
        ChipList("onlyB", onlyB, green);
        ImGui::Spacing();
        ImGui::TextColored(ui::kTextDim, "In both (%d)", static_cast<int>(both.size()));
        ChipList("both", both, ui::ChipColor());
    }

    ImGui::Spacing();
    ui::SectionHeading("PROMPTS");
    if (ImGui::BeginTable("cmp_prompts", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        for (int col = 0; col < 2; ++col) {
            ImGui::TableSetColumnIndex(col);
            const ImageMetadata& m = col ? mb : ma;
            ImGui::TextColored(ui::kTextDim, col ? "B positive" : "A positive");
            ImGui::TextWrapped("%s", m.parsed.positivePrompt.empty() ? "(empty)" : m.parsed.positivePrompt.c_str());
            if (!m.parsed.negativePrompt.empty()) {
                ImGui::Spacing();
                ImGui::TextColored(ui::kTextDim, col ? "B negative" : "A negative");
                ImGui::TextWrapped("%s", m.parsed.negativePrompt.c_str());
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::End();
    if (!open) app.CloseCompare();
}

// ---------------------------------------------------------------------------
// Similar images
// ---------------------------------------------------------------------------

void DrawSimilarWindow(AppState& app) {
    if (!app.showSimilar) return;
    CenterNextWindow(820, 700);
    if (!ImGui::Begin("Find similar images", &app.showSimilar, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("Finds images that look alike even when they differ in size, format or compression: variations of one "
                       "generation, upscales, re-saves. Works on the %d image(s) in the current folder. It never changes any files.",
                       static_cast<int>(app.entries.size()));
    ImGui::Spacing();

    ImGui::SetNextItemWidth(ui::Dp(260.0f));
    ImGui::SliderInt("Similarity", &app.similarMaxDistance, 0, 10, app.similarMaxDistance == 0 ? "Identical-looking" : "Allow %d differences");
    ui::HelpMarker("0: only images that look identical.\n1-4: resized or re-compressed copies, small edits.\n"
                   "5-8: the same composition (typical seed variations).\n9-10: loosely similar. Expect false matches.");

    const bool running = app.IsSimilarScanning();
    ImGui::BeginDisabled(running || app.entries.empty());
    if (ui::PrimaryButton(("Scan " + std::to_string(app.entries.size()) + " images").c_str())) app.StartSimilarScan(app.similarMaxDistance);
    ImGui::EndDisabled();
    if (running) {
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) app.CancelSimilarScan();
        const int total = app.SimilarTotal(), cur = app.SimilarCurrent();
        ImGui::ProgressBar(total ? static_cast<float>(cur) / total : 0.0f, ImVec2(-1, 0),
                           (std::to_string(cur) + " / " + std::to_string(total)).c_str());
    }
    if (!app.similarMessage.empty()) {
        ImGui::TextColored(ui::kTextDim, "%s", app.similarMessage.c_str());
    }

    if (!app.similarGroups.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Show all in gallery")) {
            std::vector<int> all;
            for (const auto& g : app.similarGroups) all.insert(all.end(), g.begin(), g.end());
            app.SetRestrict(all);
            app.restrictLabel = "similar images";
        }
        ImGui::Separator();
        ImGui::BeginChild("similar_groups", ImVec2(0, 0), false);
        const float cell = ui::Dp(104.0f);
        const float gap = ui::Dp(8.0f);
        const int perRow = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + gap) / (cell + gap)));
        int shownGroups = 0;
        for (size_t gi = 0; gi < app.similarGroups.size() && shownGroups < 150; ++gi, ++shownGroups) {
            const std::vector<int>& group = app.similarGroups[gi];
            ImGui::PushID(static_cast<int>(gi));
            ImGui::PushFont(ui::BoldFont());
            ImGui::Text("Group %d", static_cast<int>(gi) + 1);
            ImGui::PopFont();
            ImGui::SameLine();
            ImGui::TextDisabled("%d images", static_cast<int>(group.size()));
            ImGui::SameLine();
            if (ImGui::SmallButton("Select all but the largest")) {
                int keep = group[0];
                for (int i : group) if (app.entries[i].size > app.entries[keep].size) keep = i;
                app.multiSelected.clear();
                for (int i : group) if (i != keep) app.multiSelected.insert(i);
                app.SelectIndex(keep, false);
                app.SetStatus("Selected " + std::to_string(group.size() - 1) + " smaller copies. Press Delete to remove them.");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Show in gallery")) {
                app.SetRestrict(group);
                app.restrictLabel = "a group of similar images";
            }
            int shown = 0;
            for (int idx : group) {
                if (idx < 0 || idx >= static_cast<int>(app.entries.size())) continue;
                if (shown >= 60) break;
                const ImageEntry& e = app.entries[idx];
                if (shown % perRow) ImGui::SameLine(0, gap);
                ImGui::PushID(idx);
                ImGui::BeginGroup();
                const bool selected = app.multiSelected.count(idx) > 0 || idx == app.selectedIndex;
                bool clicked;
                ID3D11ShaderResourceView* thumbShown = e.thumbReady ? app.ShownSrv(e.thumbSrv, app.ShouldBlur(e)) : nullptr;
                if (thumbShown) {
                    ImVec2 uv0(0, 0), uv1(1, 1);
                    if (e.thumbW > e.thumbH && e.thumbH > 0) { float m = (1.0f - float(e.thumbH) / e.thumbW) * 0.5f; uv0.x = m; uv1.x = 1 - m; }
                    else if (e.thumbH > e.thumbW && e.thumbW > 0) { float m = (1.0f - float(e.thumbW) / e.thumbH) * 0.5f; uv0.y = m; uv1.y = 1 - m; }
                    clicked = ImGui::ImageButton("##t", (ImTextureID)(intptr_t)thumbShown, ImVec2(cell, cell), uv0, uv1,
                                                 selected ? ui::kAccent : ImVec4(0, 0, 0, 0));
                } else {
                    clicked = ImGui::Button("...", ImVec2(cell, cell));
                }
                if (clicked) app.SelectIndex(idx);
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cell);
                ImGui::TextDisabled("%s", ui::FormatBytes(e.size).c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndGroup();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", ToUtf8(e.displayName).c_str());
                ImGui::PopID();
                ++shown;
            }
            if (group.size() > 60) ImGui::TextDisabled("... and %d more", static_cast<int>(group.size()) - 60);
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::PopID();
        }
        if (app.similarGroups.size() > 150) ImGui::TextDisabled("Showing the first 150 of %d groups.", static_cast<int>(app.similarGroups.size()));
        ImGui::EndChild();
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Appearance
// ---------------------------------------------------------------------------

bool DrawAppearanceWindow(AppState& app) {
    if (!app.showAppearance) return false;
    bool changed = false;
    ImGui::SetNextWindowSize(ImVec2(ui::Dp(380.0f), 0), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::Begin("Appearance", &app.showAppearance, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        ui::SectionHeading("THEME");
        int theme = app.lightTheme ? 1 : 0;
        if (ImGui::RadioButton("Dark", &theme, 0)) { app.lightTheme = false; changed = true; }
        ImGui::SameLine();
        if (ImGui::RadioButton("Light", &theme, 1)) { app.lightTheme = true; changed = true; }

        ImGui::Spacing();
        ui::SectionHeading("ACCENT");
        for (int i = 0; i < ui::AccentCount(); ++i) { // (a round swatch for each; the chosen one ringed)
            if (i) ImGui::SameLine(0, ui::Dp(6.0f));
            ImGui::PushID(i);
            const float d = ui::Dp(26.0f);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##accent", ImVec2(d, d)) && app.accent != i) app.accent = i, changed = true;
            const bool hov = ImGui::IsItemHovered();
            if (hov) ImGui::SetTooltip("%s", ui::AccentName(i));
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 c(p.x + d * 0.5f, p.y + d * 0.5f);
            dl->AddCircleFilled(c, d * 0.5f - ui::Dp(4.0f), ImGui::GetColorU32(ui::AccentSwatch(i)), 24);
            if (app.accent == i || hov) dl->AddCircle(c, d * 0.5f - ui::Dp(1.0f), ImGui::GetColorU32(app.accent == i ? ImGuiCol_Text : ImGuiCol_TextDisabled), 24, ui::Dp(1.5f));
            ImGui::PopID();
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", ui::AccentName(app.accent));

        ImGui::Spacing();
        ui::SectionHeading("INTERFACE SIZE");
        static float percent = 100.0f;
        static bool init = false;
        if (!init || ImGui::IsWindowAppearing()) {
            percent = app.uiScaleMul * 100.0f;
            init = true;
        }
        ImGui::SetNextItemWidth(ui::Dp(240.0f));
        ImGui::SliderFloat("##uiscale", &percent, 70.0f, 180.0f, "%.0f%%");
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            app.uiScaleMul = std::clamp(percent / 100.0f, 0.7f, 1.8f);
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset")) {
            percent = 100.0f;
            app.uiScaleMul = 1.0f;
            changed = true;
        }
        ImGui::TextDisabled("Applied when you let go of the slider. Stacks on top of\nyour Windows display scaling.");

        ImGui::Spacing();
        ui::SectionHeading("VIDEOS");
        float pct = app.videoVolume * 100.0f;
        ImGui::SetNextItemWidth(ui::Dp(240.0f));
        if (ImGui::SliderFloat("Volume##allvideos", &pct, 0.0f, 100.0f, "%.0f%%")) {
            app.videoVolume = pct / 100.0f;
            VideoPlayer::SetVideoVolume(app.videoVolume);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
        ImGui::TextDisabled("Every video in the app plays at this volume (the gallery,\nthe Video page, ComfyUI). Music keeps its own.");
        if (ImGui::Checkbox("Blur NSFW pictures (Ctrl+Shift+B)", &app.blurNsfw)) {
            app.nsfwRevealed.clear();
            changed = true;
        }
    }
    ImGui::End();
    if (changed) app.SaveUiSettings();
    return changed;
}

// ---------------------------------------------------------------------------
// People detection
// ---------------------------------------------------------------------------

void DrawPeopleDetectWindow(AppState& app) {
    if (!app.showPeopleDetect) return;
    ImGui::SetNextWindowSize(ImVec2(ui::Dp(520.0f), 0), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Detect people in images", &app.showPeopleDetect, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    ImGui::PushTextWrapPos(ui::Dp(500.0f));
    ImGui::TextUnformatted("Counts the people actually visible in each image with an object-detection model, instead of guessing "
                           "from the prompt. The result replaces the prompt-based estimate for sorting, filtering (People) and "
                           "searching (people:2). Images are only read, never changed.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    const bool available = app.PeopleModelAvailable();
    if (available) {
        ImGui::TextColored(ui::kGood, "Model: models\\people.onnx found");
        if (!app.peopleDevice.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", app.peopleDevice.c_str());
        }
    } else {
        ImGui::TextColored(ui::kWarn, "Model not found: put a YOLOv8 person detector at models\\people.onnx (see the README).");
    }

    const int total = static_cast<int>(app.entries.size());
    const int detected = app.DetectedPeopleCount();
    ImGui::Text("%d of %d images in this folder already counted from the image.", detected, total);

    const bool running = app.IsPeopleScanning();
    ImGui::Spacing();
    ImGui::BeginDisabled(!available || running || total == 0 || detected == total);
    if (ui::PrimaryButton(("Detect in all " + std::to_string(total - detected) + " remaining").c_str())) {
        std::vector<int> all;
        for (int i = 0; i < total; ++i) all.push_back(i);
        app.StartPeopleScan(all);
    }
    ImGui::EndDisabled();
    const int selected = static_cast<int>(app.multiSelected.size());
    if (selected >= 1) {
        ImGui::SameLine();
        ImGui::BeginDisabled(!available || running);
        if (ImGui::Button(("Detect in " + std::to_string(selected) + " selected").c_str())) {
            app.StartPeopleScan(std::vector<int>(app.multiSelected.begin(), app.multiSelected.end()));
        }
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(running || detected == 0);
    if (ImGui::Button("Use prompt estimates again")) app.ForgetDetectedPeople();
    ImGui::EndDisabled();

    if (running) {
        const int cur = app.PeopleCurrent(), tot = app.PeopleTotal();
        ImGui::ProgressBar(tot ? static_cast<float>(cur) / tot : 0.0f, ImVec2(-1, 0), (std::to_string(cur) + " / " + std::to_string(tot)).c_str());
        if (ImGui::SmallButton("Cancel")) app.CancelPeopleScan();
    }
    if (!app.peopleMessage.empty()) ImGui::TextColored(ui::kTextDim, "%s", app.peopleMessage.c_str());
    ImGui::Spacing();
    ImGui::TextDisabled("Results are remembered, so each image is only analysed once. Roughly 15 images per second on a GPU.");
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Shared: text input bound to a std::string
// ---------------------------------------------------------------------------

namespace {

int StdStringResize(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* str = static_cast<std::string*>(data->UserData);
        str->resize(data->BufTextLen);
        data->Buf = str->data();
    }
    return 0;
}

bool InputStd(const char* label, const char* hint, std::string& value, float width) {
    ImGui::SetNextItemWidth(width);
    return ImGui::InputTextWithHint(label, hint, value.data(), value.capacity() + 1, ImGuiInputTextFlags_CallbackResize, StdStringResize, &value);
}

// Images an action applies to: the multi-selection, or the single selected image.
std::vector<int> SelectionTargets(const AppState& app) {
    std::vector<int> out;
    for (int i : app.visibleOrder) {
        if (app.multiSelected.count(i)) out.push_back(i);
    }
    for (int i : app.multiSelected) { // anything selected but currently filtered out
        if (std::find(out.begin(), out.end(), i) == out.end() && i >= 0 && i < static_cast<int>(app.entries.size())) out.push_back(i);
    }
    if (out.empty() && app.selectedIndex >= 0 && app.selectedIndex < static_cast<int>(app.entries.size())) out.push_back(app.selectedIndex);
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Export clean copies
// ---------------------------------------------------------------------------

void DrawCleanExportWindow(AppState& app) {
    if (!app.showCleanExport) return;
    ImGui::SetNextWindowSize(ImVec2(ui::Dp(560.0f), 0), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Export clean copies", &app.showCleanExport, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    const std::vector<int> targets = SelectionTargets(app);
    int cleanable = 0;
    bool novelAI = false;
    for (int i : targets) {
        cleanable += CanStripMetadata(app.entries[i].path) ? 1 : 0;
        novelAI |= app.entries[i].metadata.generator == Generator::NovelAI;
    }

    ImGui::PushTextWrapPos(ui::Dp(540.0f));
    ImGui::TextUnformatted("Saves copies of the selected images without the prompt, seed, settings, camera data or any other "
                           "information stored inside the file. The picture itself is not re-encoded, so quality is "
                           "untouched. Your originals are never changed.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::Text("%d image(s) selected, %d of them PNG, JPEG or WebP.", static_cast<int>(targets.size()), cleanable);
    if (cleanable < static_cast<int>(targets.size())) {
        ImGui::TextDisabled("Other formats carry no metadata this tool can remove and are copied unchanged.");
    }
    if (novelAI) {
        ImGui::TextColored(ui::kWarn, "NovelAI images also hide data in the picture's pixels. That is not removed here.");
    }

    ui::SectionHeading("WHAT TO KEEP");
    ImGui::RadioButton("Nothing: remove all metadata", &app.cleanMode, 0);
    ImGui::RadioButton("Only the prompt (PNG), for sharing a recipe without seeds and settings", &app.cleanMode, 1);

    ui::SectionHeading("SAVE TO");
    InputStd("##cleandir", "default: a \"clean\" folder inside the opened folder", app.cleanExportDir, ui::Dp(430.0f));
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        std::wstring picked = OpenFolderDialog(GetActiveWindow());
        if (!picked.empty()) app.cleanExportDir = ToUtf8(picked);
    }
    ImGui::Checkbox("Open the folder when finished", &app.cleanOpenWhenDone);
    ImGui::TextDisabled("Files that already exist there are never overwritten; a number is added instead.");

    ImGui::Spacing();
    const bool running = app.IsCleanExporting();
    ImGui::BeginDisabled(running || targets.empty());
    if (ui::PrimaryButton(("Export " + std::to_string(targets.size()) + " clean cop" + (targets.size() == 1 ? "y" : "ies")).c_str())) {
        app.StartCleanExport(targets);
    }
    ImGui::EndDisabled();
    if (running) {
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) app.CancelCleanExport();
        const int total = app.CleanTotal(), cur = app.CleanCurrent();
        ImGui::ProgressBar(total ? static_cast<float>(cur) / total : 0.0f, ImVec2(-1, 0), (std::to_string(cur) + " / " + std::to_string(total)).c_str());
    }
    if (!app.cleanMessage.empty()) ImGui::TextColored(ui::kTextDim, "%s", app.cleanMessage.c_str());
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Rename by metadata
// ---------------------------------------------------------------------------

void DrawRenameWindow(AppState& app) {
    if (!app.showRename) return;
    ImGui::SetNextWindowSize(ImVec2(ui::Dp(760.0f), ui::Dp(640.0f)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Rename by metadata", &app.showRename, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    // Which images, in the order they appear in the gallery (this drives {n}).
    std::vector<int> targets;
    if (app.renameScope == 1) {
        targets = app.visibleOrder;
    } else {
        targets = SelectionTargets(app);
    }

    ImGui::TextUnformatted("Name pattern");
    InputStd("##renametmpl", "e.g. {date}_{model}_{seed}", app.renameTemplate, ui::Dp(520.0f));
    ImGui::SameLine();
    if (ImGui::BeginCombo("##renamepreset", "Presets", ImGuiComboFlags_WidthFitPreview)) {
        static const char* kPresets[] = {"{date}_{model}_{seed}", "{n:4}_{prompt:30}", "{year}-{month}-{day}_{name}",
                                         "{generator}_{people}p_{name}", "{model}/{name}", "{rating}stars_{name}"};
        for (const char* p : kPresets) {
            if (ImGui::Selectable(p)) app.renameTemplate = p;
        }
        ImGui::EndCombo();
    }
    if (ImGui::CollapsingHeader("What can I use in the pattern?")) {
        if (ImGui::BeginTable("tokens", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
            for (const auto& t : RenameTokenHelp()) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(ui::kAccentHover, "%s", t.first);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(t.second);
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("Characters Windows doesn't allow in file names are replaced. The file extension is kept.");
    }

    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Apply to");
    ImGui::SameLine();
    ImGui::RadioButton(("the selected images (" + std::to_string(SelectionTargets(app).size()) + ")").c_str(), &app.renameScope, 0);
    ImGui::SameLine();
    ImGui::RadioButton(("everything in the current view (" + std::to_string(app.visibleOrder.size()) + ")").c_str(), &app.renameScope, 1);
    ImGui::SetNextItemWidth(ui::Dp(120.0f));
    ImGui::InputInt("First {n}", &app.renameStart, 1, 10);
    app.renameStart = std::clamp(app.renameStart, 0, 999999);

    // Preview (first rows only; the whole list is planned again when applying).
    static std::vector<RenamePlanItem> preview;
    static std::string previewError, lastTemplate;
    static int lastStart = -1, lastScope = -1;
    static size_t lastCount = 0;
    static unsigned lastEpoch = ~0u;
    static int lastFirst = -2;
    const int first = targets.empty() ? -1 : targets.front();
    if (lastTemplate != app.renameTemplate || lastStart != app.renameStart || lastScope != app.renameScope ||
        lastCount != targets.size() || lastEpoch != app.filterEpoch || lastFirst != first) {
        lastTemplate = app.renameTemplate;
        lastStart = app.renameStart;
        lastScope = app.renameScope;
        lastCount = targets.size();
        lastEpoch = app.filterEpoch;
        lastFirst = first;
        std::vector<int> head(targets.begin(), targets.begin() + std::min<size_t>(targets.size(), 300));
        previewError.clear();
        preview = BuildRenamePlan(app, head, app.renameTemplate, app.renameStart, &previewError);
    }

    int changed = 0, adjusted = 0;
    for (const auto& item : preview) {
        changed += item.changed ? 1 : 0;
        adjusted += item.adjusted ? 1 : 0;
    }
    ImGui::Spacing();
    if (!previewError.empty()) ImGui::TextColored(ui::kBad, "%s", previewError.c_str());
    ImGui::Text("Preview of the first %d: %d renamed, %d unchanged%s", static_cast<int>(preview.size()), changed,
                static_cast<int>(preview.size()) - changed, adjusted ? (", " + std::to_string(adjusted) + " got a number added to stay unique").c_str() : "");

    const float footer = ui::Dp(110.0f);
    if (ImGui::BeginTable("renameprev", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV,
                          ImVec2(0, std::max(ui::Dp(120.0f), ImGui::GetContentRegionAvail().y - footer)))) {
        ImGui::TableSetupColumn("Current name");
        ImGui::TableSetupColumn("New name");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (const auto& item : preview) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(ToUtf8(fs::path(item.from).filename().wstring()).c_str());
            ImGui::TableSetColumnIndex(1);
            const std::string to = ToUtf8(fs::path(item.to).filename().wstring());
            if (!item.changed) ImGui::TextDisabled("%s", to.c_str());
            else if (item.adjusted) ImGui::TextColored(ui::kWarn, "%s", to.c_str());
            else ImGui::TextColored(ui::kGood, "%s", to.c_str());
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    const bool busy = app.IsRenaming();
    ImGui::BeginDisabled(busy || targets.empty() || changed == 0);
    if (ui::PrimaryButton(("Rename " + std::to_string(targets.size()) + " file(s)").c_str())) {
        std::string error;
        std::vector<RenamePlanItem> plan = BuildRenamePlan(app, targets, app.renameTemplate, app.renameStart, &error);
        std::vector<std::pair<std::wstring, std::wstring>> moves;
        for (const auto& item : plan) {
            if (item.changed) moves.push_back({item.from, item.to});
        }
        std::wstring prompt = L"Rename " + std::to_wstring(moves.size()) + L" file(s)?\n\nYou can revert it afterwards with \"Undo last rename\".";
        if (!moves.empty() && MessageBoxW(GetActiveWindow(), prompt.c_str(), L"Rename by metadata", MB_YESNO | MB_ICONQUESTION) == IDYES) {
            app.StartRename(moves);
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(busy || !app.CanUndoRename());
    if (ImGui::Button("Undo last rename")) {
        if (MessageBoxW(GetActiveWindow(), L"Restore the file names from the last rename?", L"Undo rename", MB_YESNO | MB_ICONQUESTION) == IDYES) {
            app.StartUndoRename();
        }
    }
    ImGui::EndDisabled();
    if (busy) {
        const int total = app.RenameTotal(), cur = app.RenameCurrent();
        ImGui::ProgressBar(total ? static_cast<float>(cur) / total : 0.0f, ImVec2(-1, 0), (std::to_string(cur) + " / " + std::to_string(total)).c_str());
    }
    if (!app.renameMessage.empty()) ImGui::TextColored(ui::kTextDim, "%s", app.renameMessage.c_str());
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Vision library tab
// ---------------------------------------------------------------------------

namespace {

// First `maxChars` characters of `s` without cutting a UTF-8 character; newlines become spaces.
std::string Preview(const std::string& s, size_t maxChars) {
    std::string out;
    size_t chars = 0;
    for (size_t i = 0; i < s.size() && chars < maxChars;) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t step = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
        out.append(s, i, step);
        if (c == '\n' || c == '\r') out.back() = ' ';
        i += step;
        ++chars;
    }
    if (out.size() < s.size()) out += "...";
    return out;
}

void DrawPill(ImDrawList* dl, ImVec2& pos, const char* text, ImU32 bg) {
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const float padX = ui::Dp(6.0f), padY = ui::Dp(1.0f);
    dl->AddRectFilled(pos, ImVec2(pos.x + ts.x + padX * 2, pos.y + ts.y + padY * 2), bg, ui::Dp(8.0f));
    dl->AddText(ImVec2(pos.x + padX, pos.y + padY), IM_COL32(255, 255, 255, 245), text);
    pos.x += ts.x + padX * 2 + ui::Dp(5.0f);
}

void TextSection(const char* id, const char* heading, const std::string& text, const char* copiedMessage, AppState& app,
                 const std::wstring* picture = nullptr, double seconds = 0) {
    ImGui::PushID(id);
    if (heading[0]) ui::SectionHeading(heading);
    if (ImGui::SmallButton("Copy")) {
        ImGui::SetClipboardText(text.c_str());
        app.SetStatus(copiedMessage);
    }
    if (picture) { // straight to ComfyUI with this text
        ImGui::SameLine();
        if (ImGui::SmallButton("Send to ComfyUI")) {
            const std::string label = ToUtf8(fs::path(*picture).filename().wstring());
            if (seconds > 0) SendToComfy(app, text, std::string(), *picture, label, false, seconds); // a video prompt: the picture is its first frame
            else SendToComfy(app, text, std::string(), *picture, label);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Edit & send...")) {
            OpenSendEditor(app, text, std::string(), *picture, ToUtf8(fs::path(*picture).filename().wstring()), seconds);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Change this prompt before sending it. The saved prompt stays as it is.");
        if (seconds > 0) { // a video prompt: also straight to the 4-step video model
            ImGui::SameLine();
            if (ImGui::SmallButton("H3 Turbo")) SendH3Turbo(app, *picture, text, seconds, ToUtf8(fs::path(*picture).filename().wstring()));
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("A %.0f-second video with sound from MiniMax H3 Turbo in 4 steps, starting from this picture", std::clamp(seconds, 3.0, 15.0));
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%d words", static_cast<int>(std::count(text.begin(), text.end(), ' ')) + 1);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::PopID();
}

} // namespace


// Starts a new Vision result for library images: output 0 plain prompt, 1 styled (styleSel / custom), 2 tags, 3 video.
void StartLibraryDescribe(AppState& app, int output, int styleSel, const std::string& customStyle, int seconds, const std::vector<std::wstring>& paths) {
    if (app.visionModel.empty()) {
        app.showVisionWindow = true;
        app.SetStatus("Choose a vision model in the Vision window first.");
        return;
    }
    const int styles = VisionStyleCount();
    app.visionMode = output == 2 ? AppState::VisionMode::Tags : (output == 3 ? AppState::VisionMode::VideoScene : AppState::VisionMode::ImagePrompt);
    if (output == 3) app.videoOptions.seconds = seconds;
    if (output == 1) {
        if (styleSel >= styles) { app.visionStyle = "custom"; app.visionStyleCustom = customStyle; }
        else app.visionStyle = VisionStyleAt(styleSel).key;
    } else if (output == 0) {
        app.visionStyle.clear();
    }
    app.SaveVisionSettings();
    app.RequestVisionPaths(paths);
}

// Right-click menu of a library picture.
void LibraryItemMenu(AppState& app, int idx) {
    const auto& item = app.visionLib[idx];
    auto copy = [&](const std::string& text, const char* what) {
        ImGui::SetClipboardText(text.c_str());
        app.SetStatus(std::string("Copied the ") + what + ".");
    };
    if (ImGui::MenuItem("Copy prompt", nullptr, false, !item.prompt.empty())) copy(item.prompt, "prompt");
    if (ImGui::MenuItem("Copy tags", nullptr, false, !item.tags.empty())) copy(item.tags, "tags");
    if (!item.styled.empty() && ImGui::BeginMenu("Copy styled prompt")) {
        for (const auto& st : item.styled) {
            if (ImGui::MenuItem(st.first.c_str())) copy(st.second, "styled prompt");
        }
        ImGui::EndMenu();
    }
    if (!item.videos.empty() && ImGui::BeginMenu("Copy video prompt")) {
        for (const auto& v : item.videos) {
            char label[24];
            snprintf(label, sizeof(label), "%d seconds", v.first);
            if (ImGui::MenuItem(label)) copy(v.second, "video prompt");
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Copy everything")) {
        std::string all = item.prompt;
        if (!item.tags.empty()) all += "\n\nTags:\n" + item.tags;
        for (const auto& st : item.styled) all += "\n\nStyled (" + st.first + "):\n" + st.second;
        for (const auto& v : item.videos) all += "\n\nVideo scene (" + std::to_string(v.first) + " s):\n" + v.second;
        copy(all, "texts");
    }
    ImGui::Separator();
    if (ImGui::BeginMenu("Vision prompt this again")) {
        const std::vector<std::wstring> one{item.imagePath};
        if (ImGui::MenuItem("Plain image prompt")) StartLibraryDescribe(app, 0, 0, "", 10, one);
        if (ImGui::BeginMenu("In a style")) {
            for (int i = 0; i < VisionStyleCount(); ++i) {
                if (ImGui::MenuItem(VisionStyleAt(i).label)) StartLibraryDescribe(app, 1, i, "", 10, one);
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Tag list")) StartLibraryDescribe(app, 2, 0, "", 10, one);
        if (ImGui::BeginMenu("Video scene")) {
            for (int sec : {5, 10, 15, 20, 30}) {
                char label[16];
                snprintf(label, sizeof(label), "%d seconds", sec);
                if (ImGui::MenuItem(label)) StartLibraryDescribe(app, 3, 0, "", sec, one);
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("More options...")) {
            app.SelectVisionLib(idx);
            app.visionLibRedoRequest = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Move to folder")) {
        const std::wstring root = ToWide2(app.visionOutputDir);
        if (ImGui::MenuItem("(top level)", nullptr, false, !item.dir.empty())) {
            app.SetStatus(app.MoveImages({item.imagePath}, root).message);
        }
        int shown = 0;
        for (const std::string& d : app.visionLibDirs) {
            if (++shown > 60) { ImGui::TextDisabled("... use the folder pane for the rest"); break; }
            if (ImGui::MenuItem(d.c_str(), nullptr, false, d != item.dir)) {
                app.SetStatus(app.MoveImages({item.imagePath}, root + L"\\" + ToWide2(d)).message);
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Copy image")) {
        std::string msg;
        CopyImagesToClipboard(GetActiveWindow(), {item.imagePath}, false, &msg);
        app.SetStatus(msg);
    }
    if (ImGui::MenuItem("Copy image without metadata")) {
        std::string msg;
        CopyImagesToClipboard(GetActiveWindow(), {item.imagePath}, true, &msg);
        app.SetStatus(msg);
    }
    if (ImGui::MenuItem("Save as a character")) AddSavedCharacter(app, item.imagePath);
    if (ImGui::MenuItem("Swap a character into this picture...")) SetSwapScene(app, item.imagePath);
    if (ImGui::BeginMenu("Add to Vision board")) {
        if (ImGui::MenuItem("As a character")) AddImagesToBoard(app, {item.imagePath}, 0);
        if (ImGui::MenuItem("As the background")) AddImagesToBoard(app, {item.imagePath}, 1);
        if (ImGui::MenuItem("As an item")) AddImagesToBoard(app, {item.imagePath}, 2);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Send to ComfyUI")) {
        const std::string label = ToUtf8(item.name);
        auto send = [&](const std::string& text) { SendToComfy(app, text, std::string(), item.imagePath, label); };
        if (ImGui::MenuItem("Image prompt", nullptr, false, !item.prompt.empty())) send(item.prompt);
        if (ImGui::MenuItem("Tag list", nullptr, false, !item.tags.empty())) send(item.tags);
        for (const auto& st : item.styled) {
            if (ImGui::MenuItem(("In the style: " + st.first).c_str())) send(st.second);
        }
        for (const auto& v : item.videos) {
            char text[64];
            snprintf(text, sizeof(text), "Video prompt (%d s), this picture as the first frame", v.first);
            if (ImGui::MenuItem(text)) SendToComfy(app, v.second, std::string(), item.imagePath, label, false, static_cast<double>(v.first));
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Edit this picture with Qwen...")) OpenPictureEditor(app, {item.imagePath});
        if (ImGui::MenuItem("Add to the Clef mode cast")) AddToClefCast(app, {item.imagePath});
        if (ImGui::MenuItem("Upscale this picture...")) OpenUpscaler(app, {item.imagePath});
        if (ImGui::MenuItem("Design with PhotoCraft...")) OpenDesigner(app, {item.imagePath});
        if (ImGui::BeginMenu("Pixel art")) {
            PixelArtQuickContents(app, {item.imagePath});
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit, then send...")) {
            auto edit = [&](const std::string& text, double secs) { OpenSendEditor(app, text, std::string(), item.imagePath, label, secs); };
            if (ImGui::MenuItem("Image prompt", nullptr, false, !item.prompt.empty())) edit(item.prompt, 0);
            if (ImGui::MenuItem("Tag list", nullptr, false, !item.tags.empty())) edit(item.tags, 0);
            for (const auto& st : item.styled) {
                if (ImGui::MenuItem(("In the style: " + st.first).c_str())) edit(st.second, 0);
            }
            for (const auto& v : item.videos) {
                char text[48];
                snprintf(text, sizeof(text), "Video prompt (%d s)", v.first);
                if (ImGui::MenuItem(text)) edit(v.second, static_cast<double>(v.first));
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Workflow to run")) {
            ComfyWorkflowMenu(app);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Generate page...")) app.requestTab = 4;
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Show in Explorer")) {
        std::wstring args = L"/select,\"" + item.imagePath + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }
    if (ImGui::MenuItem("Copy image path")) ImGui::SetClipboardText(ToUtf8(item.imagePath).c_str());
}

// Drag a result onto a folder in the folder pane: image path as the payload.
void LibraryDragSource(const AppState::VisionLibItem& item) {
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
        ImGui::SetDragDropPayload("MV_LIBITEM", item.imagePath.c_str(), (item.imagePath.size() + 1) * sizeof(wchar_t));
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(ToUtf8(item.name).c_str());
        ImGui::TextDisabled("Drop it on a folder");
        ImGui::EndTooltip();
        ImGui::EndDragDropSource();
    }
}

void DrawLibraryAutoSort(AppState& app, const std::vector<int>& shown) {
    static int minGroup = 3, maxGroups = 12;
    static bool onlyTop = true;
    struct Proposal {
        std::string name;
        bool use = true;
        std::vector<int> items;
        char buf[96] = {0};
    };
    static std::vector<Proposal> proposals;
    static bool dirty = true;
    if (ImGui::BeginPopupModal("Auto-sort the Vision library", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(ui::Dp(520.0f));
        ImGui::TextDisabled("Groups the pictures that share tags (or, without tags, key words of the prompt) into folders. "
                            "You see the plan first and can rename or skip any folder.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        dirty |= ImGui::Checkbox("Only pictures that are not in a folder yet", &onlyTop);
        ImGui::SetNextItemWidth(ui::Dp(220.0f));
        dirty |= ImGui::SliderInt("At least this many per folder", &minGroup, 2, 30);
        ImGui::SetNextItemWidth(ui::Dp(220.0f));
        dirty |= ImGui::SliderInt("At most this many folders", &maxGroups, 2, 40);
        if (dirty || ImGui::IsWindowAppearing()) {
            dirty = false;
            std::vector<int> pool;
            for (int idx : shown) {
                if (!onlyTop || app.visionLib[idx].dir.empty()) pool.push_back(idx);
            }
            std::vector<SortDoc> docs;
            for (int idx : pool) docs.push_back({app.visionLib[idx].tags, app.visionLib[idx].prompt});
            const std::vector<SortGroup> groups = ProposeGroups(docs, minGroup, maxGroups);
            proposals.clear();
            for (const SortGroup& g : groups) {
                Proposal p;
                p.name = g.name;
                snprintf(p.buf, sizeof(p.buf), "%s", g.name.c_str());
                for (int d : g.docs) p.items.push_back(pool[d]);
                proposals.push_back(std::move(p));
            }
        }
        ImGui::Separator();
        int total = 0;
        for (const Proposal& p : proposals) total += p.use ? static_cast<int>(p.items.size()) : 0;
        if (proposals.empty()) {
            ImGui::TextDisabled("Nothing to group with these settings. Try a lower minimum, or describe more pictures (tag lists work best).");
        }
        ImGui::BeginChild("##sortplan", ImVec2(ui::Dp(520.0f), std::min(ui::Dp(320.0f), ImGui::GetFrameHeightWithSpacing() * static_cast<float>(proposals.size()) + ui::Dp(8.0f))), false);
        for (size_t i = 0; i < proposals.size(); ++i) {
            Proposal& p = proposals[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::Checkbox("##use", &p.use);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ui::Dp(260.0f));
            ImGui::InputText("##name", p.buf, sizeof(p.buf));
            ImGui::SameLine();
            ImGui::TextColored(ui::kTextDim, "%d picture%s", static_cast<int>(p.items.size()), p.items.size() == 1 ? "" : "s");
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::Spacing();
        ImGui::BeginDisabled(total == 0);
        if (ui::PrimaryButton(("Create folders and move " + std::to_string(total) + " pictures").c_str(), ImVec2(ui::Dp(300.0f), 0))) {
            const std::wstring root = ToWide2(app.visionOutputDir);
            int moved = 0, made = 0, failed = 0;
            for (const Proposal& p : proposals) {
                if (!p.use || p.items.empty() || p.buf[0] == 0) continue;
                std::string err;
                app.CreateFolder(root, p.buf, &err); // an existing folder is fine: pictures just join it
                std::string clean;
                for (unsigned char c : std::string(p.buf)) clean += (c < 32 || std::strchr("<>:\"/\\|?*", c)) ? '_' : static_cast<char>(c);
                std::vector<std::wstring> paths;
                for (int idx : p.items) paths.push_back(app.visionLib[idx].imagePath);
                const AppState::MoveReport rep = app.MoveImages(paths, root + L"\\" + ToWide2(clean));
                moved += rep.moved;
                failed += rep.failed;
                ++made;
            }
            app.SetStatus("Sorted " + std::to_string(moved) + " pictures into " + std::to_string(made) + " folders" + (failed ? " (" + std::to_string(failed) + " failed)." : "."));
            dirty = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(ui::Dp(90.0f), 0))) {
            dirty = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

namespace {

// ---- selecting several pictures in the Vision library (click, Ctrl / Shift click, drag a box) and sending them to ComfyUI ----
std::set<std::wstring> g_libMulti; // the selected pictures
int g_libAnchor = -1;              // where a Shift-click range starts (position in the shown list)
int g_libTimes = 1;                // send to ComfyUI this many times
int g_libBatch = 0;
std::string g_sendKey = "best";    // which text to send: best / prompt / tags / style:<name> / video:<seconds> / video:any
bool g_describeForSelection = false;

bool LibIsSelected(const AppState& app, int idx) {
    if (g_libMulti.count(app.visionLib[idx].imagePath)) return true;
    return g_libMulti.empty() && idx == app.visionLibSelected;
}

std::vector<int> LibSelection(const AppState& app, const std::vector<int>& order) {
    std::vector<int> out;
    for (int idx : order) {
        if (g_libMulti.count(app.visionLib[idx].imagePath)) out.push_back(idx);
    }
    if (out.empty() && g_libMulti.empty() && std::find(order.begin(), order.end(), app.visionLibSelected) != order.end()) out.push_back(app.visionLibSelected);
    return out;
}

void LibSelectRange(const AppState& app, const std::vector<int>& order, int a, int b, bool keepOthers) {
    if (!keepOthers) g_libMulti.clear();
    for (int j = std::min(a, b); j <= std::max(a, b); ++j) {
        if (j >= 0 && j < static_cast<int>(order.size())) g_libMulti.insert(app.visionLib[order[j]].imagePath);
    }
}

// a click on the picture at `pos` of the shown list: plain = only it, Ctrl = add / remove it, Shift = everything from the last click
void LibClick(AppState& app, const std::vector<int>& order, int pos) {
    const ImGuiIO& io = ImGui::GetIO();
    const int idx = order[pos];
    const std::wstring& path = app.visionLib[idx].imagePath;
    if (io.KeyShift && g_libAnchor >= 0 && g_libAnchor < static_cast<int>(order.size())) {
        LibSelectRange(app, order, g_libAnchor, pos, io.KeyCtrl);
    } else if (io.KeyCtrl) {
        if (g_libMulti.empty() && app.visionLibSelected >= 0 && app.visionLibSelected != idx) g_libMulti.insert(app.visionLib[app.visionLibSelected].imagePath);
        auto it = g_libMulti.find(path);
        if (it != g_libMulti.end()) {
            g_libMulti.erase(it);
            g_libAnchor = pos;
            return;
        }
        g_libMulti.insert(path);
        g_libAnchor = pos;
    } else {
        g_libMulti.clear();
        g_libMulti.insert(path);
        g_libAnchor = pos;
    }
    app.SelectVisionLib(idx);
}

// the text of a picture that `key` asks for ("" = it has none); `seconds` > 0 for a video prompt
std::string PickText(const AppState::VisionLibItem& it, const std::string& key, double& seconds) {
    seconds = 0;
    if (key == "best") {
        if (!it.prompt.empty()) return it.prompt;
        if (!it.tags.empty()) return it.tags;
        if (!it.styled.empty()) return it.styled.front().second;
        if (!it.videos.empty()) {
            seconds = it.videos.front().first;
            return it.videos.front().second;
        }
        return {};
    }
    if (key == "prompt") return it.prompt;
    if (key == "tags") return it.tags;
    if (key.rfind("style:", 0) == 0) {
        for (const auto& st : it.styled) {
            if (st.first == key.substr(6)) return st.second;
        }
        return {};
    }
    if (key == "video:any") {
        if (it.videos.empty()) return {};
        seconds = it.videos.front().first;
        return it.videos.front().second;
    }
    if (key.rfind("video:", 0) == 0) {
        const int sec = std::atoi(key.c_str() + 6);
        for (const auto& v : it.videos) {
            if (v.first == sec) {
                seconds = v.first;
                return v.second;
            }
        }
    }
    return {};
}

std::string KeyLabel(const std::string& key) {
    if (key == "best") return "The best prompt each one has";
    if (key == "prompt") return "Image prompt";
    if (key == "tags") return "Tag list";
    if (key == "video:any") return "Video prompt (any length)";
    if (key.rfind("style:", 0) == 0) return "In the style: " + key.substr(6);
    if (key.rfind("video:", 0) == 0) return "Video prompt (" + key.substr(6) + " s)";
    return key;
}

// what can be sent from these pictures: only kinds at least one of them has
std::vector<std::string> SendKeys(const AppState& app, const std::vector<int>& sel) {
    std::set<std::string> styles;
    std::set<int> seconds;
    bool prompt = false, tags = false;
    for (int idx : sel) {
        const auto& it = app.visionLib[idx];
        prompt |= !it.prompt.empty();
        tags |= !it.tags.empty();
        for (const auto& st : it.styled) styles.insert(st.first);
        for (const auto& v : it.videos) seconds.insert(v.first);
    }
    std::vector<std::string> keys{"best"};
    if (prompt) keys.push_back("prompt");
    if (tags) keys.push_back("tags");
    for (const std::string& s : styles) keys.push_back("style:" + s);
    if (!seconds.empty()) keys.push_back("video:any");
    for (int s : seconds) keys.push_back("video:" + std::to_string(s));
    return keys;
}

int CountHaving(const AppState& app, const std::vector<int>& sel, const std::string& key) {
    int n = 0;
    for (int idx : sel) {
        double secs;
        if (!PickText(app.visionLib[idx], key, secs).empty()) ++n;
    }
    return n;
}

// Sends the chosen text of every selected picture; a picture that does not have it is skipped (and named in the message).
void SendLibrarySelection(AppState& app, const std::vector<int>& sel, const std::string& key, int times) {
    times = std::clamp(times, 1, 100);
    const int batch = ++g_libBatch;
    int sent = 0, skipped = 0;
    std::string skippedNames;
    for (int idx : sel) {
        const auto& item = app.visionLib[idx];
        double secs = 0;
        const std::string text = PickText(item, key, secs);
        if (text.empty()) {
            if (++skipped <= 4) skippedNames += (skippedNames.empty() ? "" : ", ") + ToUtf8(item.name);
            continue;
        }
        if (!SendToComfy(app, text, std::string(), item.imagePath, ToUtf8(item.name), secs <= 0, secs, times, batch)) return; // no workflow chosen yet
        ++sent;
    }
    std::string msg;
    if (sent == 0) {
        msg = "Nothing sent: none of the " + std::to_string(skipped) + " selected has \"" + KeyLabel(key) + "\"";
    } else {
        msg = "Sent " + std::to_string(sent) + (sent == 1 ? " picture" : " pictures") + " to ComfyUI" + (times > 1 ? ", " + std::to_string(times) + " times each" : std::string()) +
              " (" + std::to_string(sent * times) + " to make): watch them on the Generate page";
        if (skipped > 0) msg += ". Skipped " + std::to_string(skipped) + " without \"" + KeyLabel(key) + "\": " + skippedNames + (skipped > 4 ? ", ..." : "");
    }
    app.SetStatus(msg);
}

std::vector<std::wstring> PathsOf(const AppState& app, const std::vector<int>& sel) {
    std::vector<std::wstring> out;
    for (int idx : sel) out.push_back(app.visionLib[idx].imagePath);
    return out;
}

// Right-click menu when several pictures are selected.
void LibraryMultiMenu(AppState& app, const std::vector<int>& order) {
    const std::vector<int> sel = LibSelection(app, order);
    ImGui::TextColored(ui::kAccent, "%d pictures selected", static_cast<int>(sel.size()));
    ImGui::Separator();
    if (ImGui::BeginMenu("Send to ComfyUI")) {
        for (const std::string& key : SendKeys(app, sel)) {
            const int have = CountHaving(app, sel, key);
            const std::string text = KeyLabel(key) + "   (" + std::to_string(have) + " of " + std::to_string(sel.size()) + ")";
            if (ImGui::MenuItem(text.c_str(), nullptr, false, have > 0)) SendLibrarySelection(app, sel, key, g_libTimes);
        }
        ImGui::Separator();
        ImGui::TextDisabled("Pictures without that text are skipped.");
        ImGui::TextDisabled("Each is made %d time%s (change it in the bar above).", g_libTimes, g_libTimes == 1 ? "" : "s");
        ImGui::Separator();
        if (ImGui::BeginMenu("Workflow to run")) {
            ComfyWorkflowMenu(app);
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Edit with Qwen Image 2.1 (the first is edited)...")) OpenPictureEditor(app, PathsOf(app, sel));
    if (ImGui::MenuItem("Add these to the Clef mode cast")) AddToClefCast(app, PathsOf(app, sel));
    if (ImGui::MenuItem("Upscale these...")) OpenUpscaler(app, PathsOf(app, sel));
    if (ImGui::BeginMenu("Pixel art (these)")) {
        PixelArtQuickContents(app, PathsOf(app, sel));
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Vision prompt these again")) {
        const std::vector<std::wstring> paths = PathsOf(app, sel);
        if (ImGui::MenuItem("Plain image prompt")) StartLibraryDescribe(app, 0, 0, "", 10, paths);
        if (ImGui::BeginMenu("In a style")) {
            for (int i = 0; i < VisionStyleCount(); ++i) {
                if (ImGui::MenuItem(VisionStyleAt(i).label)) StartLibraryDescribe(app, 1, i, "", 10, paths);
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Tag list")) StartLibraryDescribe(app, 2, 0, "", 10, paths);
        if (ImGui::BeginMenu("Video scene")) {
            for (int sec : {5, 10, 15, 20, 30}) {
                char label[16];
                snprintf(label, sizeof(label), "%d seconds", sec);
                if (ImGui::MenuItem(label)) StartLibraryDescribe(app, 3, 0, "", sec, paths);
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Move to folder")) {
        const std::wstring root = ToWide2(app.visionOutputDir);
        const std::vector<std::wstring> paths = PathsOf(app, sel);
        if (ImGui::MenuItem("(top level)")) app.SetStatus(app.MoveImages(paths, root).message);
        int shown = 0;
        for (const std::string& d : app.visionLibDirs) {
            if (++shown > 60) { ImGui::TextDisabled("... use the folder pane for the rest"); break; }
            if (ImGui::MenuItem(d.c_str())) app.SetStatus(app.MoveImages(paths, root + L"\\" + ToWide2(d)).message);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Add to Vision board")) {
        const std::vector<std::wstring> paths = PathsOf(app, sel);
        if (ImGui::MenuItem("As characters")) AddImagesToBoard(app, paths, 0);
        if (ImGui::MenuItem("As the background")) AddImagesToBoard(app, paths, 1);
        if (ImGui::MenuItem("As items")) AddImagesToBoard(app, paths, 2);
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Copy the images")) {
        std::string msg;
        CopyImagesToClipboard(GetActiveWindow(), PathsOf(app, sel), false, &msg);
        app.SetStatus(msg);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Clear the selection")) {
        g_libMulti.clear();
        g_libAnchor = -1;
    }
}

void LibContextMenu(AppState& app, const std::vector<int>& order, int pos) {
    const int idx = order[pos];
    if (!g_libMulti.count(app.visionLib[idx].imagePath)) { // right click on something that is not selected: it becomes the selection
        g_libMulti.clear();
        g_libMulti.insert(app.visionLib[idx].imagePath);
        g_libAnchor = pos;
        app.SelectVisionLib(idx);
    }
    if (g_libMulti.size() > 1) LibraryMultiMenu(app, order);
    else LibraryItemMenu(app, idx);
}

// The bar above the list: what is selected, and send it to ComfyUI.
void DrawSelectionBar(AppState& app, const std::vector<int>& order, const std::vector<int>& sel) {
    ImGui::AlignTextToFramePadding();
    if (sel.empty()) {
        ImGui::TextColored(ui::kTextDim, "Nothing selected. Click a picture, Ctrl or Shift click for several, or drag a box around them.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Select all shown")) LibSelectRange(app, order, 0, static_cast<int>(order.size()) - 1, false);
        return;
    }
    ImGui::TextColored(ui::kAccent, "%d selected", static_cast<int>(sel.size()));
    ImGui::SameLine();
    if (ImGui::SmallButton("All shown")) LibSelectRange(app, order, 0, static_cast<int>(order.size()) - 1, false);
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
        g_libMulti.clear();
        g_libAnchor = -1;
    }
    ImGui::SameLine();
    ImGui::TextColored(ui::kTextDim, "  |  Send to ComfyUI:");
    ImGui::SameLine();
    const std::vector<std::string> keys = SendKeys(app, sel);
    if (std::find(keys.begin(), keys.end(), g_sendKey) == keys.end() && g_sendKey != "best") {
        // the choice is not available for this selection: the combo still shows it, and the count says 0
    }
    const int have = CountHaving(app, sel, g_sendKey);
    ImGui::SetNextItemWidth(ui::Dp(230.0f));
    if (ImGui::BeginCombo("##sendwhat", KeyLabel(g_sendKey).c_str())) {
        for (const std::string& key : keys) {
            const std::string text = KeyLabel(key) + "   (" + std::to_string(CountHaving(app, sel, key)) + " of " + std::to_string(sel.size()) + ")";
            if (ImGui::Selectable(text.c_str(), key == g_sendKey)) g_sendKey = key;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ui::Dp(105.0f));
    ImGui::InputInt("times##libtimes", &g_libTimes, 1, 5);
    g_libTimes = std::clamp(g_libTimes, 1, 100);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("How many pictures ComfyUI makes from each one (every one with its own seed)");
    ImGui::SameLine();
    ImGui::BeginDisabled(have == 0);
    if (ui::PrimaryButton("Send", ImVec2(ui::Dp(80.0f), 0))) SendLibrarySelection(app, sel, g_sendKey, g_libTimes);
    ImGui::EndDisabled();
    if (sel.size() == 1) {
        ImGui::SameLine();
        ImGui::BeginDisabled(have == 0);
        if (ImGui::Button("Edit & send...")) {
            const auto& item = app.visionLib[sel.front()];
            double secs = 0;
            const std::string text = PickText(item, g_sendKey, secs);
            OpenSendEditor(app, text, std::string(), item.imagePath, ToUtf8(item.name), secs);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Change the chosen prompt before sending it. The saved prompt stays as it is.");
    }
    ImGui::SameLine();
    const int skip = static_cast<int>(sel.size()) - have;
    if (have == 0) ImGui::TextColored(ui::kWarn, "none of them has that");
    else ImGui::TextColored(ui::kTextDim, "= %d to make%s", have * g_libTimes, skip > 0 ? (", skips " + std::to_string(skip) + " without it").c_str() : "");
    ImGui::SameLine();
    if (ImGui::SmallButton(("Workflow: " + ComfyWorkflowLabel(app) + " v").c_str())) ImGui::OpenPopup("##libworkflow");
    if (ImGui::BeginPopup("##libworkflow")) {
        ComfyWorkflowMenu(app);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Describe / restyle...")) {
        if (app.visionLibSelected < 0 || app.visionLibSelected >= static_cast<int>(app.visionLib.size())) app.SelectVisionLib(sel.front());
        g_describeForSelection = sel.size() > 1;
        app.visionLibRedoRequest = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Make a new prompt (in a style, tags or a video scene) for the selected pictures");
}

} // namespace

void DrawVisionLibrary(AppState& app, float height) {
    app.RequestVisionLibrary();
    ImGuiStyle& style = ImGui::GetStyle();

    // ---- header: where the results live ----
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ui::kTextDim, "Vision results folder");
    ImGui::SameLine();
    if (app.visionOutputDir.empty()) ImGui::TextColored(ui::kWarn, "not set");
    else ImGui::TextUnformatted(app.visionOutputDir.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Choose...")) {
        std::wstring picked = OpenFolderDialog(GetActiveWindow());
        if (!picked.empty()) {
            app.visionOutputDir = ToUtf8(picked);
            app.SaveVisionSettings();
            app.RefreshVisionLibrary();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) app.RefreshVisionLibrary();
    ImGui::SameLine();
    ImGui::BeginDisabled(app.visionOutputDir.empty());
    if (ImGui::Button("Open folder")) ShellExecuteW(nullptr, L"open", app.visionOutputDir.empty() ? L"" : fs::path(app.visionOutputDir).wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    ImGui::EndDisabled();

    std::error_code ec;
    if (app.visionOutputDir.empty() || !fs::is_directory(fs::path(app.visionOutputDir), ec)) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(ui::Dp(620.0f));
        ImGui::TextDisabled("Pick the folder your Vision results are saved to. Each described image sits there next to a .txt with its "
                            "prompt (and a .tags.txt or .video-10s.txt for tag lists and video scenes). They show up here to browse and read.");
        ImGui::PopTextWrapPos();
        return;
    }

    // ---- filters ----
    static std::string query;
    static int show = 0, sortMode = 0;
    InputStd("##libsearch", "Search names, prompts, tags and video prompts", query, ui::Dp(360.0f));
    ImGui::SameLine();
    static const char* kShow[] = {"All results", "With a video prompt", "Without a video prompt", "With tags"};
    ImGui::SetNextItemWidth(ui::Dp(190.0f));
    ImGui::Combo("##libshow", &show, kShow, IM_ARRAYSIZE(kShow));
    ImGui::SameLine();
    static const char* kSort[] = {"Recently described", "Described longest ago", "Name", "Image date (newest)", "Clef: genre", "Clef: mood", "Clef: setting", "Clef: era", "Clef: subject"};
    ImGui::SetNextItemWidth(ui::Dp(180.0f));
    ImGui::Combo("##libsort", &sortMode, kSort, IM_ARRAYSIZE(kSort));
    ImGui::SameLine();
    // List (picture, name and the start of the prompt per row) or Boxes (thumbnails in a grid, like the gallery)
    for (int mode = 0; mode < 2; ++mode) {
        const bool on = (mode == 1) == app.visionLibBoxes;
        if (mode) ImGui::SameLine(0, ui::Dp(2.0f));
        if (on) {
            ImGui::PushStyleColor(ImGuiCol_Button, ui::kAccentActive);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        }
        if (ImGui::Button(mode ? "Boxes" : "List") && !on) {
            app.visionLibBoxes = mode == 1;
            app.SaveUiSettings();
        }
        if (on) ImGui::PopStyleColor(2);
    }
    if (app.visionLibBoxes) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ui::Dp(120.0f));
        ImGui::SliderFloat("##libboxsize", &app.visionLibBoxSize, 80.0f, 280.0f, "Size %.0f");
        if (ImGui::IsItemDeactivatedAfterEdit()) app.SaveUiSettings();
    }
    ImGui::SameLine();
    if (ImGui::Button(app.visionLibFolders ? "Hide folders" : "Folders")) {
        app.visionLibFolders = !app.visionLibFolders;
        app.SaveUiSettings();
    }
    ImGui::SameLine();
    const bool openSort = ImGui::Button("Auto-sort...");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Make folders from the tags the pictures have in common");

    // ---- Clef: tags every entry (genre, mood, setting, era, subject) from its picture and description, then filter by them
    static int clefFilter[5] = {0, 0, 0, 0, 0};
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ui::kTextDim, "Clef");
        ImGui::SameLine();
        if (ClefTagRunning()) {
            int d = 0, t = 0;
            std::string phase;
            ClefTagProgress(d, t, phase);
            ImGui::TextColored(ui::kWarn, "%s (%d of %d)", phase.c_str(), d, t);
            ImGui::SameLine();
            if (ImGui::SmallButton("Stop##cleftagstop")) ClefTagStop();
        } else {
            auto inputs = [&](const std::vector<int>& idx) {
                std::vector<ClefTagInput> items;
                for (int i : idx) {
                    const auto& it = app.visionLib[i];
                    ClefTagInput in;
                    in.imagePath = it.imagePath;
                    in.text = it.prompt + (it.tags.empty() ? std::string() : "\nTags: " + it.tags);
                    in.mtime = static_cast<long long>(it.mtime.time_since_epoch().count());
                    items.push_back(std::move(in));
                }
                return items;
            };
            std::vector<int> all(app.visionLib.size());
            for (size_t i = 0; i < all.size(); ++i) all[i] = static_cast<int>(i);
            if (ImGui::SmallButton("Tag the library")) ClefTagStart(inputs(all), app.comfy.url, true);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("Clef reads every result (the picture and its description) and tags it with a genre, a mood, a setting, an era and a subject.\nThen filter and sort the library by them. Only new results are tagged; the tags are kept.");
            ImGui::SameLine();
            if (ImGui::SmallButton("Tag again")) ClefTagStart(inputs(all), app.comfy.url, false);
        }
        for (int k = 0; k < 5; ++k) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ui::Dp(150.0f));
            const std::vector<std::string>& names = ClefTagNames(k);
            const std::string cur = clefFilter[k] <= 0 ? std::string("Any ") + ClefTagKindName(k) : names[std::min<size_t>(clefFilter[k] - 1, names.size() - 1)];
            if (ImGui::BeginCombo((std::string("##clefTag") + std::to_string(k)).c_str(), cur.c_str())) {
                if (ImGui::Selectable((std::string("Any ") + ClefTagKindName(k)).c_str(), clefFilter[k] <= 0)) clefFilter[k] = 0;
                for (size_t n = 0; n < names.size(); ++n) {
                    if (ImGui::Selectable(names[n].c_str(), clefFilter[k] == static_cast<int>(n) + 1)) clefFilter[k] = static_cast<int>(n) + 1;
                }
                ImGui::EndCombo();
            }
        }
    }

    // Filtered, sorted view of the list (rebuilt only when something changed).
    static std::vector<int> order;
    static unsigned lastVersion = ~0u;
    static std::string lastQuery;
    static int lastShow = -1, lastSort = -1;
    static std::string lastDir;
    static bool lastTop = false;
    static unsigned lastTagVersion = 0;
    static int lastClef[5] = {-1, -1, -1, -1, -1};
    bool clefChanged = lastTagVersion != ClefTagsVersion();
    for (int k = 0; k < 5; ++k) clefChanged = clefChanged || lastClef[k] != clefFilter[k];
    if (clefChanged || lastVersion != app.visionLibVersion || lastQuery != query || lastShow != show || lastSort != sortMode || lastDir != app.visionLibDir || lastTop != app.visionLibTopOnly) {
        if (lastVersion != app.visionLibVersion) { // forget selected pictures that are gone
            std::set<std::wstring> keep;
            for (const auto& it : app.visionLib) {
                if (g_libMulti.count(it.imagePath)) keep.insert(it.imagePath);
            }
            g_libMulti.swap(keep);
            g_libAnchor = -1;
        }
        lastTagVersion = ClefTagsVersion();
        for (int k = 0; k < 5; ++k) lastClef[k] = clefFilter[k];
        lastDir = app.visionLibDir;
        lastTop = app.visionLibTopOnly;
        lastVersion = app.visionLibVersion;
        lastQuery = query;
        lastShow = show;
        lastSort = sortMode;
        order.clear();
        const std::string q = Lower(query);
        for (int i = 0; i < static_cast<int>(app.visionLib.size()); ++i) {
            const auto& it = app.visionLib[i];
            if (app.visionLibTopOnly && !it.dir.empty()) continue;
            if (!app.visionLibDir.empty()) { // inside the folder picked in the folder pane
                if (it.dir.size() < app.visionLibDir.size() || _strnicmp(it.dir.c_str(), app.visionLibDir.c_str(), app.visionLibDir.size()) != 0 ||
                    (it.dir.size() > app.visionLibDir.size() && it.dir[app.visionLibDir.size()] != '\\')) continue;
            }
            if (show == 1 && it.videos.empty()) continue;
            if (show == 2 && !it.videos.empty()) continue;
            if (show == 3 && it.tags.empty()) continue;
            if (!q.empty() && it.searchLower.find(q) == std::string::npos) continue;
            if (clefFilter[0] || clefFilter[1] || clefFilter[2] || clefFilter[3] || clefFilter[4]) {
                ClefTags t;
                if (!ClefTagsFor(it.imagePath, t)) continue;
                const int have[5] = {t.genre, t.mood, t.setting, t.era, t.subject};
                bool ok = true;
                for (int k = 0; k < 5; ++k) ok = ok && (clefFilter[k] <= 0 || have[k] == clefFilter[k] - 1);
                if (!ok) continue;
            }
            order.push_back(i);
        }
        // the scan already lists them by when their newest result was written
        if (sortMode == 1) std::reverse(order.begin(), order.end());
        else if (sortMode == 3) {
            std::sort(order.begin(), order.end(), [&](int a, int b) { return app.visionLib[a].mtime > app.visionLib[b].mtime; });
        } else if (sortMode == 2) {
            std::sort(order.begin(), order.end(), [&](int a, int b) { return _wcsicmp(app.visionLib[a].name.c_str(), app.visionLib[b].name.c_str()) < 0; });
        } else if (sortMode >= 4) { // by one of Clef's tags, the untagged last
            const int kind = sortMode - 4;
            auto key = [&](int i) {
                ClefTags t;
                if (!ClefTagsFor(app.visionLib[i].imagePath, t)) return 1 << 20;
                const int have[5] = {t.genre, t.mood, t.setting, t.era, t.subject};
                return have[kind] < 0 ? 1 << 20 : have[kind];
            };
            std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
                const int ka = key(a), kb = key(b);
                return ka != kb ? ka < kb : _wcsicmp(app.visionLib[a].name.c_str(), app.visionLib[b].name.c_str()) < 0;
            });
        }
    }
    int withVideo = 0;
    for (const auto& it : app.visionLib) withVideo += it.videos.empty() ? 0 : 1;
    ImGui::TextColored(ui::kTextDim, "%d of %d results   -   %d with a video prompt", static_cast<int>(order.size()), static_cast<int>(app.visionLib.size()), withVideo);
    if (app.visionLibSelected >= 0 && app.visionLibSelected < static_cast<int>(app.visionLib.size())) {
        const std::string line = ClefTagLine(app.visionLib[app.visionLibSelected].imagePath);
        if (!line.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(ui::kAccent, "Clef: %s", line.c_str());
        }
    }
    if (app.VisionLibraryScanning() || !app.visionLibLoaded) {
        ImGui::SameLine();
        ImGui::TextColored(ui::kWarn, "reading the folder...");
    }

    if (openSort) ImGui::OpenPopup("Auto-sort the Vision library");
    DrawLibraryAutoSort(app, order);
    const std::vector<int> libSel = LibSelection(app, order);
    DrawSelectionBar(app, order, libSel);

    // Up/Down moves through the list.
    int move = 0;
    static int boxCols = 1; // columns of the boxes view from the last frame, so Up/Down can jump a row
    if (!ImGui::GetIO().WantTextInput) {
        const int row = app.visionLibBoxes ? boxCols : 1;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) move = row;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) move = -row;
        if (app.visionLibBoxes) {
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) move = 1;
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) move = -1;
        }
    }
    if (!ImGui::GetIO().WantTextInput && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A) && !order.empty()) LibSelectRange(app, order, 0, static_cast<int>(order.size()) - 1, false);
    if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape) && g_libMulti.size() > 1) {
        g_libMulti.clear();
        if (app.visionLibSelected >= 0 && app.visionLibSelected < static_cast<int>(app.visionLib.size())) g_libMulti.insert(app.visionLib[app.visionLibSelected].imagePath);
    }
    bool scrollToSelection = false;
    if (move != 0 && !order.empty()) {
        auto it = std::find(order.begin(), order.end(), app.visionLibSelected);
        int pos = it == order.end() ? (move > 0 ? -1 : static_cast<int>(order.size())) : static_cast<int>(it - order.begin());
        pos = std::clamp(pos + move, 0, static_cast<int>(order.size()) - 1);
        app.SelectVisionLib(order[pos]);
        if (ImGui::GetIO().KeyShift && g_libAnchor >= 0 && g_libAnchor < static_cast<int>(order.size())) {
            LibSelectRange(app, order, g_libAnchor, pos, false); // Shift + arrows grows the selection
        } else {
            g_libMulti.clear();
            g_libMulti.insert(app.visionLib[order[pos]].imagePath);
            g_libAnchor = pos;
        }
        scrollToSelection = true;
    }

    const float bodyH = std::max(ui::Dp(120.0f), ImGui::GetContentRegionAvail().y);
    (void)height;
    const bool boxes = app.visionLibBoxes;
    const bool folders = app.visionLibFolders;
    const char* tableId = folders ? (boxes ? "lib_layout_bf" : "lib_layout_f") : (boxes ? "lib_layout_b" : "lib_layout");
    if (!ImGui::BeginTable(tableId, folders ? 3 : 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings, ImVec2(0, bodyH))) return;
    if (folders) ImGui::TableSetupColumn("Folders", ImGuiTableColumnFlags_WidthStretch, 0.14f);
    ImGui::TableSetupColumn("List", ImGuiTableColumnFlags_WidthStretch, boxes ? 0.43f : 0.30f);
    ImGui::TableSetupColumn("Reader", ImGuiTableColumnFlags_WidthStretch, boxes ? 0.43f : 0.56f);
    ImGui::TableNextRow(0, bodyH);

    int col = 0;
    if (folders) {
        ImGui::TableSetColumnIndex(col++);
        DrawLibraryFolderPane(app, bodyH);
    }
    // ---- the list ----
    ImGui::TableSetColumnIndex(col++);
    ImGui::BeginChild("lib_list", ImVec2(0, bodyH), true);
    const ImVec2 origin0 = ImGui::GetCursorScreenPos(); // top of the (scrolled) content: item rectangles are measured from here
    const float baseX = ImGui::GetCursorPosX();
    std::function<void(int, ImVec2&, ImVec2&)> rectOf;
    if (order.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled(app.visionLib.empty() ? (app.visionLibLoaded ? "No results in this folder yet.\nDescribe an image and it will appear here." : "")
                                                   : "Nothing matches the search or filter.");
    }
    if (boxes) {
        const float cell = ui::Dp(app.visionLibBoxSize);
        const float gap = ui::Dp(8.0f);
        const float labelH = ImGui::GetTextLineHeight() + ui::Dp(6.0f);
        const float availW = ImGui::GetContentRegionAvail().x;
        const int cols = std::max(1, static_cast<int>((availW + gap) / (cell + gap)));
        boxCols = cols;
        const float used = cols * cell + (cols - 1) * gap;
        const float offX = ImGui::GetCursorPosX() + std::max(0.0f, (availW - used) * 0.5f);
        const float rowH2 = cell + labelH + gap;
        const int n = static_cast<int>(order.size());
        const int rowsN = (n + cols - 1) / cols;
        rectOf = [=](int k, ImVec2& a, ImVec2& sz) {
            a = ImVec2((offX - baseX) + (k % cols) * (cell + gap), (k / cols) * rowH2);
            sz = ImVec2(cell, cell + labelH);
        };
        if (scrollToSelection) {
            auto it = std::find(order.begin(), order.end(), app.visionLibSelected);
            if (it != order.end()) {
                const float top = ((it - order.begin()) / cols) * rowH2;
                const float scrollY = ImGui::GetScrollY(), view = ImGui::GetWindowHeight();
                if (top < scrollY) ImGui::SetScrollY(top);
                else if (top + rowH2 > scrollY + view) ImGui::SetScrollY(top + rowH2 - view + style.ItemSpacing.y);
            }
        }
        ImGuiListClipper clipper2;
        clipper2.Begin(rowsN, rowH2);
        while (clipper2.Step()) {
            for (int row = clipper2.DisplayStart; row < clipper2.DisplayEnd; ++row) {
                const ImVec2 rowStart = ImGui::GetCursorScreenPos();
                for (int c = 0; c < cols; ++c) {
                    const int k = row * cols + c;
                    if (k >= n) break;
                    const int idx = order[k];
                    const auto& item = app.visionLib[idx];
                    ImGui::PushID(idx);
                    const ImVec2 p(rowStart.x + (offX - ImGui::GetCursorPosX()) + c * (cell + gap), rowStart.y);
                    ImGui::SetCursorScreenPos(p);
                    if (ImGui::InvisibleButton("##box", ImVec2(cell, cell + labelH))) LibClick(app, order, k);
                    const bool hovered = ImGui::IsItemHovered();
                    LibraryDragSource(item);
                    if (ImGui::BeginPopupContextItem("##libctx")) {
                        LibContextMenu(app, order, k);
                        ImGui::EndPopup();
                    }
                    const bool selected = LibIsSelected(app, idx);
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    const ImVec2 p1(p.x + cell, p.y + cell + labelH);
                    if (selected) {
                        dl->AddRectFilled(p, p1, ui::WithAlpha(ui::kAccent, 0.22f), ui::Dp(8.0f));
                        dl->AddRect(p, p1, ui::WithAlpha(ui::kAccent, 1.0f), ui::Dp(8.0f), 0, ui::Dp(2.0f));
                    } else if (hovered) {
                        dl->AddRectFilled(p, p1, IM_COL32(255, 255, 255, 16), ui::Dp(8.0f));
                    }
                    const float pad = ui::Dp(4.0f);
                    const ImVec2 a(p.x + pad, p.y + pad), b(p1.x - pad, p.y + cell - pad);
                    AppState::LibThumb thumb = app.LibraryThumb(item.imagePath);
                    if (thumb.srv && thumb.w > 0 && thumb.h > 0) {
                        ImVec2 uv0(0, 0), uv1(1, 1);
                        if (thumb.w > thumb.h) { float m = (1.0f - float(thumb.h) / thumb.w) * 0.5f; uv0.x = m; uv1.x = 1 - m; }
                        else if (thumb.h > thumb.w) { float m = (1.0f - float(thumb.w) / thumb.h) * 0.5f; uv0.y = m; uv1.y = 1 - m; }
                        dl->AddImageRounded((ImTextureID)(intptr_t)thumb.srv, a, b, uv0, uv1, IM_COL32_WHITE, ui::Dp(6.0f));
                    } else {
                        dl->AddRectFilled(a, b, ui::ThumbPlaceholder(), ui::Dp(6.0f));
                    }
                    // what exists for it, as small chips along the bottom of the picture
                    ImVec2 chip(a.x + ui::Dp(4.0f), b.y - ImGui::GetTextLineHeight() - ui::Dp(8.0f));
                    if (!item.prompt.empty()) DrawPill(dl, chip, "P", IM_COL32(70, 120, 210, 235));
                    if (!item.tags.empty()) DrawPill(dl, chip, "T", IM_COL32(60, 150, 120, 235));
                    if (!item.styled.empty()) DrawPill(dl, chip, "S", IM_COL32(205, 100, 150, 235));
                    if (!item.videos.empty()) {
                        char label[24];
                        snprintf(label, sizeof(label), "V %ds", item.videos.front().first);
                        DrawPill(dl, chip, label, IM_COL32(150, 98, 235, 235));
                    }
                    ImGui::PushStyleColor(ImGuiCol_Text, selected ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : ui::kTextDim);
                    ImGui::RenderTextEllipsis(dl, ImVec2(p.x + pad, p.y + cell), ImVec2(p1.x - pad, p1.y), p1.x - pad, p1.x - pad, ToUtf8(item.name).c_str(), nullptr, nullptr);
                    ImGui::PopStyleColor();
                    if (hovered && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
                        const std::string& first = !item.prompt.empty() ? item.prompt : (!item.videos.empty() ? item.videos.front().second : item.tags);
                        ImGui::BeginTooltip();
                        ImGui::PushTextWrapPos(ui::Dp(380.0f));
                        ImGui::TextUnformatted(ToUtf8(item.name).c_str());
                        ImGui::TextDisabled("%s", Preview(first, 320).c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::EndTooltip();
                    }
                    ImGui::PopID();
                }
                ImGui::SetCursorScreenPos(rowStart);
                ImGui::Dummy(ImVec2(availW, rowH2 - style.ItemSpacing.y));
            }
        }
    } else {
    const float rowH = ui::Dp(84.0f);
    const float listW = ImGui::GetContentRegionAvail().x;
    rectOf = [=](int k, ImVec2& a, ImVec2& sz) {
        a = ImVec2(0, k * rowH);
        sz = ImVec2(listW, rowH - ui::Dp(4.0f));
    };
    if (scrollToSelection) {
        auto it = std::find(order.begin(), order.end(), app.visionLibSelected);
        if (it != order.end()) {
            float top = (it - order.begin()) * rowH;
            float scrollY = ImGui::GetScrollY(), view = ImGui::GetWindowHeight();
            if (top < scrollY) ImGui::SetScrollY(top);
            else if (top + rowH > scrollY + view) ImGui::SetScrollY(top + rowH - view + style.ItemSpacing.y);
        }
    }
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(order.size()), rowH);
    while (clipper.Step()) {
        for (int k = clipper.DisplayStart; k < clipper.DisplayEnd; ++k) {
            const int idx = order[k];
            const auto& item = app.visionLib[idx];
            ImGui::PushID(idx);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const bool selected = LibIsSelected(app, idx);
            if (ImGui::InvisibleButton("##row", ImVec2(listW, rowH - ui::Dp(4.0f)))) LibClick(app, order, k);
            const bool hovered = ImGui::IsItemHovered();
            LibraryDragSource(item);
            if (ImGui::BeginPopupContextItem("##libctx")) {
                LibContextMenu(app, order, k);
                ImGui::EndPopup();
            }
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 q1(p.x + listW, p.y + rowH - ui::Dp(4.0f));
            if (selected) {
                dl->AddRectFilled(p, q1, ui::WithAlpha(ui::kAccent, 0.20f), ui::Dp(8.0f));
                dl->AddRect(p, q1, ui::WithAlpha(ui::kAccent, 0.9f), ui::Dp(8.0f), 0, ui::Dp(1.5f));
            } else if (hovered) {
                dl->AddRectFilled(p, q1, IM_COL32(255, 255, 255, 14), ui::Dp(8.0f));
            }

            // preview, cropped to a square
            const float ts = ui::Dp(72.0f);
            const ImVec2 t0(p.x + ui::Dp(6.0f), p.y + ui::Dp(4.0f)), t1(t0.x + ts, t0.y + ts);
            AppState::LibThumb thumb = app.LibraryThumb(item.imagePath);
            if (thumb.srv && thumb.w > 0 && thumb.h > 0) {
                ImVec2 uv0(0, 0), uv1(1, 1);
                if (thumb.w > thumb.h) { float m = (1.0f - float(thumb.h) / thumb.w) * 0.5f; uv0.x = m; uv1.x = 1 - m; }
                else if (thumb.h > thumb.w) { float m = (1.0f - float(thumb.w) / thumb.h) * 0.5f; uv0.y = m; uv1.y = 1 - m; }
                dl->AddImageRounded((ImTextureID)(intptr_t)thumb.srv, t0, t1, uv0, uv1, IM_COL32_WHITE, ui::Dp(6.0f));
            } else {
                dl->AddRectFilled(t0, t1, ui::ThumbPlaceholder(), ui::Dp(6.0f));
            }

            // name, badges, start of the prompt
            const float tx = t1.x + ui::Dp(10.0f);
            const float textW = q1.x - tx - ui::Dp(8.0f);
            dl->PushClipRect(ImVec2(tx, p.y), ImVec2(q1.x - ui::Dp(4.0f), q1.y), true);
            ImFont* bold = ui::BoldFont();
            dl->AddText(bold, bold->FontSize, ImVec2(tx, p.y + ui::Dp(3.0f)), ImGui::GetColorU32(ImGuiCol_Text), ToUtf8(item.name).c_str());
            ImVec2 pill(tx, p.y + ui::Dp(3.0f) + ImGui::GetTextLineHeight() + ui::Dp(2.0f));
            if (!item.prompt.empty()) DrawPill(dl, pill, "Prompt", IM_COL32(70, 120, 210, 255));
            if (!item.tags.empty()) DrawPill(dl, pill, "Tags", IM_COL32(60, 150, 120, 255));
            for (const auto& st : item.styled) DrawPill(dl, pill, ("Style " + st.first).c_str(), IM_COL32(205, 100, 150, 255));
            for (const auto& v : item.videos) {
                char label[24];
                snprintf(label, sizeof(label), "Video %ds", v.first);
                DrawPill(dl, pill, label, IM_COL32(150, 98, 235, 255));
            }
            const std::string& first = !item.prompt.empty() ? item.prompt : (!item.videos.empty() ? item.videos.front().second : item.tags);
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(tx, pill.y + ImGui::GetTextLineHeight() + ui::Dp(5.0f)),
                        ImGui::GetColorU32(ui::kTextDim), Preview(first, 90).c_str(), nullptr, textW);
            dl->PopClipRect();
            ImGui::PopID();
        }
    }
    }
    // drag a box on the empty space to select every picture it touches (Ctrl / Shift keeps what was selected)
    {
        static bool banding = false, moved = false;
        static ImVec2 bandStart(0, 0);
        static std::set<std::wstring> bandBase;
        const ImGuiIO& io = ImGui::GetIO();
        const ImVec2 wp = ImGui::GetWindowPos();
        const float ww = ImGui::GetWindowWidth();
        if (!banding && rectOf && ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            io.MousePos.x < wp.x + ww - style.ScrollbarSize - 2.0f) {
            banding = true;
            g_boxSelecting = true;
            moved = false;
            bandStart = ImVec2(io.MousePos.x - origin0.x, io.MousePos.y - origin0.y);
            bandBase = (io.KeyCtrl || io.KeyShift) ? g_libMulti : std::set<std::wstring>();
        }
        if (banding) {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                banding = false;
                if (!moved && !io.KeyCtrl && !io.KeyShift) g_libMulti.clear(); // a click on empty space unselects
                if (moved) {
                    const std::vector<int> now = LibSelection(app, order);
                    const bool curIn = app.visionLibSelected >= 0 && app.visionLibSelected < static_cast<int>(app.visionLib.size()) &&
                                       g_libMulti.count(app.visionLib[app.visionLibSelected].imagePath) > 0;
                    if (!now.empty() && !curIn) app.SelectVisionLib(now.front()); // the reader shows one of the selected
                }
            } else {
                const ImVec2 cur(io.MousePos.x - origin0.x, io.MousePos.y - origin0.y);
                if (std::fabs(cur.x - bandStart.x) > 4.0f || std::fabs(cur.y - bandStart.y) > 4.0f) moved = true;
                if (moved) {
                    const ImVec2 lo(std::min(bandStart.x, cur.x), std::min(bandStart.y, cur.y)), hi(std::max(bandStart.x, cur.x), std::max(bandStart.y, cur.y));
                    g_libMulti = bandBase;
                    for (int k = 0; k < static_cast<int>(order.size()); ++k) {
                        ImVec2 a, sz;
                        rectOf(k, a, sz);
                        if (a.x < hi.x && a.x + sz.x > lo.x && a.y < hi.y && a.y + sz.y > lo.y) g_libMulti.insert(app.visionLib[order[k]].imagePath);
                    }
                    ImDrawList* bdl = ImGui::GetWindowDrawList();
                    const ImVec2 p0(origin0.x + lo.x, origin0.y + lo.y), p1(origin0.x + hi.x, origin0.y + hi.y);
                    bdl->AddRectFilled(p0, p1, ui::WithAlpha(ui::kAccent, 0.16f));
                    bdl->AddRect(p0, p1, ui::WithAlpha(ui::kAccent, 0.9f), 0.0f, 0, ui::Dp(1.5f));
                    if (io.MousePos.y < wp.y + ui::Dp(24.0f)) ImGui::SetScrollY(ImGui::GetScrollY() - ui::Dp(14.0f)); // near an edge: scroll
                    else if (io.MousePos.y > wp.y + ImGui::GetWindowHeight() - ui::Dp(24.0f)) ImGui::SetScrollY(ImGui::GetScrollY() + ui::Dp(14.0f));
                }
            }
        }
    }
    ImGui::EndChild();

    // ---- right: reader ----
    ImGui::TableSetColumnIndex(col++);
    ImGui::BeginChild("lib_reader", ImVec2(0, bodyH), true);
    if (app.visionLibSelected < 0 || app.visionLibSelected >= static_cast<int>(app.visionLib.size())) {
        ImGui::Spacing();
        ImGui::TextDisabled("Select a result on the left to read its prompt.");
    } else {
        const auto& item = app.visionLib[app.visionLibSelected];
        ImGui::PushFont(ui::BoldFont());
        ImGui::TextUnformatted(ToUtf8(item.name).c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextDisabled("%s  -  described %s", ui::FormatBytes(item.size).c_str(), FormatLocalDateTime(item.resultTime).c_str());
        if (ImGui::SmallButton("Show in Explorer")) {
            std::wstring args = L"/select,\"" + item.imagePath + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy image path")) ImGui::SetClipboardText(ToUtf8(item.imagePath).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy image")) {
            std::string msg;
            CopyImagesToClipboard(GetActiveWindow(), {item.imagePath}, false, &msg);
            app.SetStatus(msg);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy clean")) {
            std::string msg;
            CopyImagesToClipboard(GetActiveWindow(), {item.imagePath}, true, &msg);
            app.SetStatus(msg);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Copy the picture without any metadata inside");
        ImGui::SameLine();
        if (ImGui::SmallButton("Design...")) OpenDesigner(app, {item.imagePath});
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Memes, thumbnails, movie posters, captions, speech bubbles, or people placed into it (PhotoCraft)");
        ImGui::SameLine();
        PixelArtButton(app, {item.imagePath}, "##pxquicklib");
        ImGui::SameLine();
        if (ImGui::SmallButton("Describe again...") || app.visionLibRedoRequest) {
            if (!app.visionLibRedoRequest) g_describeForSelection = libSel.size() > 1;
            app.visionLibRedoRequest = false;
            ImGui::OpenPopup("##describeagain");
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Make another result for this image: a different style, tags or a video scene");
        if (ImGui::BeginPopup("##describeagain")) {
            static int output = 1;    // 0 plain prompt, 1 styled prompt, 2 tags, 3 video scene
            static int styleSel = 0;  // index into the presets, or the preset count for "custom"
            static std::string customStyle;
            static int seconds = 10;
            static int scope = 0;     // 0 this image, 1 everything in the list
            const int styles = VisionStyleCount();
            ImGui::TextUnformatted("Make another result for");
            ImGui::RadioButton(("this image##scope0"), &scope, 0);
            ImGui::SameLine();
            const std::string allLabel = "all " + std::to_string(order.size()) + " shown in the list##scope1";
            ImGui::RadioButton(allLabel.c_str(), &scope, 1);
            if (libSel.size() > 1) {
                ImGui::SameLine();
                const std::string selLabel = "the " + std::to_string(libSel.size()) + " selected##scope2";
                ImGui::RadioButton(selLabel.c_str(), &scope, 2);
            }
            if (ImGui::IsWindowAppearing()) scope = g_describeForSelection && libSel.size() > 1 ? 2 : 0;
            ImGui::Separator();
            ImGui::RadioButton("Image prompt (replaces the plain one)##out0", &output, 0);
            ImGui::RadioButton("Image prompt in a style##out1", &output, 1);
            if (output == 1) {
                ImGui::Indent(ui::Dp(22.0f));
                ImGui::SetNextItemWidth(ui::Dp(240.0f));
                const char* preview = styleSel >= styles ? "Custom style..." : VisionStyleAt(styleSel).label;
                if (ImGui::BeginCombo("##redostyle", preview)) {
                    for (int i = 0; i < styles; ++i) {
                        if (ImGui::Selectable(VisionStyleAt(i).label, styleSel == i)) styleSel = i;
                    }
                    if (ImGui::Selectable("Custom style...", styleSel >= styles)) styleSel = styles;
                    ImGui::EndCombo();
                }
                if (styleSel >= styles) InputStd("##redocustom", "e.g. dark fantasy oil painting", customStyle, ui::Dp(300.0f));
                ImGui::Unindent(ui::Dp(22.0f));
            }
            ImGui::RadioButton("Tag list##out2", &output, 2);
            ImGui::RadioButton("Video scene##out3", &output, 3);
            if (output == 3) {
                ImGui::Indent(ui::Dp(22.0f));
                for (int sec : {5, 10, 15, 20, 30}) {
                    char label[16];
                    snprintf(label, sizeof(label), "%d s##redosec%d", sec, sec);
                    if (sec != 5) ImGui::SameLine();
                    if (ImGui::RadioButton(label, seconds == sec)) seconds = sec;
                }
                ImGui::Unindent(ui::Dp(22.0f));
            }
            ImGui::Spacing();
            const bool customMissing = output == 1 && styleSel >= styles && customStyle.empty();
            ImGui::BeginDisabled(customMissing);
            if (ui::PrimaryButton("Start", ImVec2(ui::Dp(110.0f), 0))) {
                std::vector<std::wstring> paths;
                if (scope == 0) paths.push_back(item.imagePath);
                else if (scope == 2) paths = PathsOf(app, libSel);
                else for (int idx : order) paths.push_back(app.visionLib[idx].imagePath);
                StartLibraryDescribe(app, output, styleSel, customStyle, seconds, paths);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(ui::Dp(90.0f), 0))) ImGui::CloseCurrentPopup();
            ImGui::TextDisabled("New results are saved next to the old ones and appear at the top of the list.");
            ImGui::EndPopup();
        }
        if (!item.prompt.empty() || !item.tags.empty() || !item.videos.empty() || !item.styled.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy everything")) {
                std::string all = item.prompt;
                if (!item.tags.empty()) all += "\n\nTags:\n" + item.tags;
                for (const auto& st : item.styled) all += "\n\nStyled (" + st.first + "):\n" + st.second;
                for (const auto& v : item.videos) all += "\n\nVideo scene (" + std::to_string(v.first) + " s):\n" + v.second;
                ImGui::SetClipboardText(all.c_str());
                app.SetStatus("Copied everything for this image.");
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ui::kAccent);
            const bool sendComfy = ImGui::SmallButton("Send to ComfyUI");
            ImGui::PopStyleColor();
            if (sendComfy) {
                // the best text this picture has: its image prompt, else the tag list, else a styled / video prompt
                if (!item.prompt.empty()) SendToComfy(app, item.prompt, std::string(), item.imagePath, ToUtf8(item.name));
                else if (!item.tags.empty()) SendToComfy(app, item.tags, std::string(), item.imagePath, ToUtf8(item.name));
                else if (!item.styled.empty()) SendToComfy(app, item.styled.front().second, std::string(), item.imagePath, ToUtf8(item.name));
                else SendToComfy(app, item.videos.front().second, std::string(), item.imagePath, ToUtf8(item.name), false, static_cast<double>(item.videos.front().first));
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Queue this picture's prompt in ComfyUI (the workflow it is running; set up in Tools > ComfyUI)");
            ImGui::SameLine();
            if (ImGui::SmallButton("Animate...")) // a video scene Vision wrote for it is already a video prompt: it goes in as it is
                OpenVideoPage(app, item.imagePath, item.prompt, item.videos.empty() ? std::string() : item.videos.front().second);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip(item.videos.empty() ? "Make it move, with sound: the Video page (the prompt writer writes what happens)"
                                                      : "Make it move, with sound: the Video page, with the video scene written for it");
            ImGui::SameLine();
            if (ImGui::SmallButton("Star in a video...")) StarInVideo(app, item.imagePath);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Put this person into a clip: the Video page, Star in a video");
            ImGui::SameLine();
            if (ImGui::SmallButton("Make a song...")) MusicFromPicture(app, item.imagePath, item.prompt);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("A song about this picture: the prompt writer looks at it and writes the music and lyrics (Sound page)");
        }

        // Picture and text share the reader; drag the bar between them (or use the buttons)
        // to make either one the dominant one. The split is remembered.
        ImGui::SameLine();
        ImGui::TextColored(ui::kTextDim, "  Layout:");
        ImGui::SameLine();
        if (ImGui::SmallButton("Big picture")) app.visionLibImageFrac = 0.72f;
        ImGui::SameLine();
        if (ImGui::SmallButton("Even")) app.visionLibImageFrac = 0.5f;
        ImGui::SameLine();
        if (ImGui::SmallButton("Big text")) app.visionLibImageFrac = 0.22f;
        ImGui::SameLine();
        if (ImGui::SmallButton(app.visionLibSideBySide ? "Stack" : "Side by side")) app.visionLibSideBySide = !app.visionLibSideBySide;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Put the picture above the text, or next to it");

        const bool side = app.visionLibSideBySide;
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float bar = ui::Dp(9.0f);
        const float total = (side ? avail.x : avail.y) - bar;
        const float minPane = ui::Dp(side ? 160.0f : 90.0f);
        float imgSize = std::clamp(total * app.visionLibImageFrac, minPane, std::max(minPane, total - minPane));

        auto drawPicture = [&](ImVec2 sz) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::ViewerBackground());
            ImGui::BeginChild("lib_image", sz, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            if (app.visionLibImage.srv) {
                const ImVec2 origin = ImGui::GetCursorScreenPos(), size = ImGui::GetContentRegionAvail();
                const float s = std::min(size.x / app.visionLibImage.width, size.y / app.visionLibImage.height);
                const ImVec2 dim(app.visionLibImage.width * s, app.visionLibImage.height * s);
                const ImVec2 p0(origin.x + (size.x - dim.x) * 0.5f, origin.y + (size.y - dim.y) * 0.5f);
                const bool blur = app.ShouldBlur(app.visionLibImagePath);
                if (ID3D11ShaderResourceView* shown = app.ShownSrv(app.visionLibImage.srv, blur))
                    ImGui::GetWindowDrawList()->AddImage((ImTextureID)(intptr_t)shown, p0, ImVec2(p0.x + dim.x, p0.y + dim.y));
                DrawNsfwOverlay(app, app.visionLibImagePath, p0, ImVec2(p0.x + dim.x, p0.y + dim.y));
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();
        };
        auto drawSplitter = [&]() {
            const ImVec2 sz = side ? ImVec2(bar, avail.y) : ImVec2(avail.x, bar);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##libsplit", sz);
            const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
            if (hot) ImGui::SetMouseCursor(side ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
            if (ImGui::IsItemActive() && total > 1.0f) {
                const float delta = side ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y;
                app.visionLibImageFrac = std::clamp(app.visionLibImageFrac + delta / total, 0.12f, 0.88f);
            }
            if (ImGui::IsItemDeactivated()) app.SaveUiSettings();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImU32 col = ImGui::GetColorU32(hot ? ui::kAccentHover : ui::kTextDim);
            if (side) dl->AddRectFilled(ImVec2(p.x + bar * 0.5f - 1, p.y + avail.y * 0.5f - ui::Dp(16)), ImVec2(p.x + bar * 0.5f + 1, p.y + avail.y * 0.5f + ui::Dp(16)), col, 1.0f);
            else dl->AddRectFilled(ImVec2(p.x + avail.x * 0.5f - ui::Dp(16), p.y + bar * 0.5f - 1), ImVec2(p.x + avail.x * 0.5f + ui::Dp(16), p.y + bar * 0.5f + 1), col, 1.0f);
        };
        auto drawText = [&]() {
            ImGui::BeginChild("lib_text", ImVec2(0, 0), false);
            if (item.prompt.empty() && item.tags.empty() && item.videos.empty() && item.styled.empty()) {
                ImGui::TextDisabled("No result text found for this image.");
            }
            if (!item.prompt.empty()) TextSection("prompt", "IMAGE PROMPT", item.prompt, "Copied the image prompt.", app, &item.imagePath);
            if (!item.styled.empty()) {
                ui::SectionHeading("STYLED PROMPT");
                if (item.styled.size() == 1) {
                    ImGui::TextColored(ImVec4(0.93f, 0.50f, 0.70f, 1.0f), "%s", item.styled.front().first.c_str());
                    TextSection("style0", "", item.styled.front().second, "Copied the styled prompt.", app, &item.imagePath);
                } else if (ImGui::BeginTabBar("##stylelist")) {
                    for (size_t i = 0; i < item.styled.size(); ++i) {
                        if (ImGui::BeginTabItem(item.styled[i].first.c_str())) {
                            TextSection(("style" + std::to_string(i)).c_str(), "", item.styled[i].second, "Copied the styled prompt.", app, &item.imagePath);
                            ImGui::EndTabItem();
                        }
                    }
                    ImGui::EndTabBar();
                }
            }
            if (!item.videos.empty()) {
                ui::SectionHeading("VIDEO PROMPT");
                if (item.videos.size() == 1) {
                    const auto& v = item.videos.front();
                    ImGui::TextColored(ImVec4(0.72f, 0.55f, 1.0f, 1.0f), "%d second video", v.first);
                    TextSection("video0", "", v.second, "Copied the video prompt.", app, &item.imagePath, v.first);
                } else if (ImGui::BeginTabBar("##videolengths")) {
                    for (size_t i = 0; i < item.videos.size(); ++i) {
                        char label[24];
                        snprintf(label, sizeof(label), "%d seconds", item.videos[i].first);
                        if (ImGui::BeginTabItem(label)) {
                            TextSection(("video" + std::to_string(i)).c_str(), "", item.videos[i].second, "Copied the video prompt.", app, &item.imagePath, item.videos[i].first);
                            ImGui::EndTabItem();
                        }
                    }
                    ImGui::EndTabBar();
                }
            }
            if (!item.tags.empty()) TextSection("tags", "TAGS", item.tags, "Copied the tags.", app, &item.imagePath);
            ImGui::EndChild();
        };

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        if (side) {
            drawPicture(ImVec2(imgSize, avail.y));
            ImGui::SameLine();
            drawSplitter();
            ImGui::SameLine();
        } else {
            drawPicture(ImVec2(0, imgSize));
            drawSplitter();
        }
        ImGui::PopStyleVar();
        drawText();
    }
    ImGui::EndChild();
    ImGui::EndTable();
}
