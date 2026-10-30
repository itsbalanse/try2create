#include "ui_theme.h"

#include "imgui_internal.h" // (FoldButton: the window's inner rectangle)
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>
#include <cstring>

namespace fs = std::filesystem;

namespace ui {

namespace {

float g_scale = 1.0f;
bool g_light = false;
ImFont* g_bold = nullptr;
ImFont* g_title = nullptr;
ImFont* g_display = nullptr; // big text (a song's title): baked big, so it is not a small font blown up
ImFont* g_mono = nullptr;    // labels and badges

// Open Source Canvas's accents (src/lib/theme.ts, MIT): the colour, its strong and its soft shade
struct AccentDef {
    const char* name;
    int c[3], strong[3], soft[3];
};
const AccentDef kAccents[] = {
    {"Violet", {139, 92, 246}, {124, 58, 237}, {196, 181, 253}},  {"Blue", {79, 124, 243}, {59, 100, 216}, {157, 184, 255}},
    {"Teal", {18, 165, 148}, {13, 134, 119}, {94, 234, 212}},     {"Emerald", {22, 163, 74}, {21, 128, 61}, {110, 231, 183}},
    {"Amber", {217, 119, 6}, {180, 83, 9}, {252, 211, 77}},       {"Coral", {232, 93, 61}, {194, 65, 38}, {253, 186, 116}},
    {"Rose", {225, 29, 72}, {190, 18, 60}, {253, 164, 175}},      {"Slate", {100, 116, 139}, {71, 85, 105}, {203, 213, 225}},
};
constexpr int kAccentCount = static_cast<int>(sizeof(kAccents) / sizeof(kAccents[0]));
int g_accent = 0;

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(len > 0 ? len - 1 : 0, '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

// Full path of a font in the Windows fonts folder, or empty if it isn't installed.
std::string FindSystemFont(const wchar_t* fileName) {
    wchar_t winDir[MAX_PATH];
    UINT n = GetWindowsDirectoryW(winDir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::error_code ec;
    fs::path p = fs::path(winDir) / L"Fonts" / fileName;
    return fs::is_regular_file(p, ec) ? WideToUtf8(p.wstring()) : std::string();
}

// Fallback: ImGui's built-in (unhinted) rasterizer, used only if GDI rendering fails.
void BuildFontsStb(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    g_bold = nullptr;
    g_title = nullptr;
    g_display = nullptr;
    g_mono = nullptr;

    const float size = std::round(15.0f * scale);

    // Latin + Cyrillic from the default tables, plus the punctuation, arrows
    // and Greek that prompts and file names commonly contain.
    static ImVector<ImWchar> ranges;
    if (ranges.empty()) {
        static const ImWchar extra[] = {0x0370, 0x03FF, 0x2010, 0x2027, 0x2030, 0x205E, 0x2190, 0x2193, 0x2212, 0x2212, 0};
        ImFontGlyphRangesBuilder builder;
        builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
        builder.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
        builder.AddRanges(extra);
        builder.BuildRanges(&ranges);
    }

    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    cfg.PixelSnapH = true;

    std::string regular = FindSystemFont(L"segoeui.ttf");
    ImFont* main = regular.empty() ? nullptr : io.Fonts->AddFontFromFileTTF(regular.c_str(), size, &cfg, ranges.Data);
    if (!main) {
        ImFontConfig fallback;
        fallback.SizePixels = size;
        io.Fonts->AddFontDefault(&fallback);
    } else {
        // CJK so non-Latin file names and prompts don't render as "?". Merged
        // into the main font; whichever of these Windows has installed.
        ImFontConfig merge = cfg;
        merge.MergeMode = true;
        merge.OversampleH = 1;
        std::string japanese = FindSystemFont(L"YuGothM.ttc");
        if (japanese.empty()) japanese = FindSystemFont(L"meiryo.ttc");
        if (!japanese.empty()) {
            io.Fonts->AddFontFromFileTTF(japanese.c_str(), size, &merge, io.Fonts->GetGlyphRangesJapanese());
        }
        std::string chinese = FindSystemFont(L"msyh.ttc");
        if (!chinese.empty()) {
            io.Fonts->AddFontFromFileTTF(chinese.c_str(), size, &merge, io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        }
        std::string semibold = FindSystemFont(L"seguisb.ttf");
        if (!semibold.empty()) {
            ImFontConfig boldCfg = cfg;
            g_bold = io.Fonts->AddFontFromFileTTF(semibold.c_str(), size, &boldCfg, ranges.Data);
            g_title = io.Fonts->AddFontFromFileTTF(semibold.c_str(), std::round(size * 1.7f), &boldCfg, io.Fonts->GetGlyphRangesDefault());
            g_display = io.Fonts->AddFontFromFileTTF(semibold.c_str(), std::round(size * 3.6f), &boldCfg, io.Fonts->GetGlyphRangesDefault());
        }
    }
    io.FontDefault = io.Fonts->Fonts[0];
}

// ---------------------------------------------------------------------------
// Crisp text: glyphs are rasterized by Windows GDI, which applies the font's
// own hinting at the exact pixel size (the same look as native Windows UI),
// and are fed into ImGui's atlas as custom glyphs. Advances are whole pixels
// and ImGui snaps text origins to the pixel grid, so nothing ends up blurry.
// ---------------------------------------------------------------------------

struct PendingGlyph {
    int rect = 0;
    int w = 0, h = 0;
    std::vector<unsigned char> alpha;
};

HFONT MakeGdiFont(const wchar_t* face, int height, int weight) {
    return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS,
                       CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, VARIABLE_PITCH | FF_DONTCARE, face);
}

bool IsFace(HDC dc, HFONT f, const wchar_t* face) {
    if (!f) return false;
    HGDIOBJ old = SelectObject(dc, f);
    wchar_t name[LF_FACESIZE] = {};
    GetTextFaceW(dc, LF_FACESIZE, name);
    SelectObject(dc, old);
    return _wcsicmp(name, face) == 0;
}

// Adds one font built from GDI glyphs. `ranges` is a zero-terminated list of
// [first,last] code point pairs; each code point comes from the first face in
// {primary, fallbacks...} that has it. Returns null if the primary face or its
// base TTF is unavailable.
ImFont* AddGdiFont(HDC dc, const wchar_t* primaryFace, int primaryWeight, const wchar_t* const* fallbacks,
                   int fallbackCount, int cellPx, const ImWchar* ranges, const std::string& baseTtf,
                   std::vector<PendingGlyph>& pending, int* outAscent, int* outDescent) {
    ImGuiIO& io = ImGui::GetIO();

    HFONT primary = MakeGdiFont(primaryFace, cellPx, primaryWeight);
    if (!IsFace(dc, primary, primaryFace)) {
        if (primary) DeleteObject(primary);
        return nullptr;
    }
    TEXTMETRICW tm = {};
    HGDIOBJ oldFont = SelectObject(dc, primary);
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, oldFont);
    const int ascent = tm.tmAscent, descent = tm.tmDescent;
    const int em = std::max<int>(8, static_cast<int>(tm.tmHeight - tm.tmInternalLeading));

    std::vector<HFONT> faces{primary};
    for (int i = 0; i < fallbackCount; ++i) {
        HFONT f = MakeGdiFont(fallbacks[i], -em, FW_NORMAL);
        if (IsFace(dc, f, fallbacks[i])) faces.push_back(f);
        else if (f) DeleteObject(f);
    }

    // The base ImFont (metrics only); every visible glyph is added below.
    static const ImWchar kSpaceOnly[] = {0x20, 0x20, 0};
    ImFontConfig cfg;
    cfg.GlyphRanges = kSpaceOnly;
    ImFont* font = io.Fonts->AddFontFromFileTTF(baseTtf.c_str(), static_cast<float>(cellPx), &cfg, kSpaceOnly);
    if (!font) {
        for (HFONT f : faces) DeleteObject(f);
        return nullptr;
    }

    const MAT2 identity = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};

    // Light text on a dark background reads thin; a gentle curve evens the weight out.
    static unsigned char curve[65];
    static bool curveLight = false, curveReady = false;
    if (!curveReady || curveLight != g_light) {
        curveLight = g_light;
        for (int i = 0; i <= 64; ++i) curve[i] = static_cast<unsigned char>(std::pow(i / 64.0f, g_light ? 1.0f : 0.92f) * 255.0f + 0.5f);
        curveReady = true;
    }

    // Work out which face supplies each code point with one batched lookup per
    // face (probing glyphs one at a time is what made start-up slow).
    std::vector<WCHAR> cps;
    for (const ImWchar* r = ranges; r[0] != 0; r += 2) {
        for (unsigned cp = r[0]; cp <= r[1] && cp <= 0xFFFF; ++cp) {
            if (cp >= 0x7F && cp <= 0x9F) continue; // control codes
            cps.push_back(static_cast<WCHAR>(cp));
        }
    }
    std::vector<int> owner(cps.size(), -1);
    std::vector<WORD> indices(cps.size());
    for (size_t fi = 0; fi < faces.size(); ++fi) {
        SelectObject(dc, faces[fi]);
        if (GetGlyphIndicesW(dc, cps.data(), static_cast<int>(cps.size()), indices.data(), GGI_MARK_NONEXISTING_GLYPHS) == GDI_ERROR) continue;
        for (size_t k = 0; k < cps.size(); ++k) {
            if (owner[k] < 0 && indices[k] != 0xFFFF) owner[k] = static_cast<int>(fi);
        }
    }

    std::vector<unsigned char> raw(64 * 1024);
    for (size_t fi = 0; fi < faces.size(); ++fi) {
        SelectObject(dc, faces[fi]);
        for (size_t k = 0; k < cps.size(); ++k) {
            if (owner[k] != static_cast<int>(fi)) continue;
            const unsigned cp = cps[k];
            GLYPHMETRICS gm = {};
            DWORD size = GetGlyphOutlineW(dc, cp, GGO_GRAY8_BITMAP, &gm, 0, nullptr, &identity);
            if (size == GDI_ERROR) continue;
            if (size > 0) {
                if (raw.size() < size) raw.resize(size);
                if (GetGlyphOutlineW(dc, cp, GGO_GRAY8_BITMAP, &gm, size, raw.data(), &identity) == GDI_ERROR) continue;
            }

            PendingGlyph g;
            float advance = static_cast<float>(gm.gmCellIncX);
            ImVec2 offset(static_cast<float>(gm.gmptGlyphOrigin.x), static_cast<float>(ascent - gm.gmptGlyphOrigin.y));
            if (size == 0 || gm.gmBlackBoxX == 0 || gm.gmBlackBoxY == 0) {
                g.w = g.h = 1; // blank glyph (space): only the advance matters
                g.alpha.assign(1, 0);
                offset = ImVec2(0, 0);
            } else {
                g.w = static_cast<int>(gm.gmBlackBoxX);
                g.h = static_cast<int>(gm.gmBlackBoxY);
                g.alpha.resize(static_cast<size_t>(g.w) * g.h);
                const int pitch = (g.w + 3) & ~3;
                for (int y = 0; y < g.h; ++y) {
                    for (int x = 0; x < g.w; ++x) {
                        g.alpha[static_cast<size_t>(y) * g.w + x] = curve[std::min<int>(64, raw[static_cast<size_t>(y) * pitch + x])];
                    }
                }
            }
            // (a clear pixel all round: ImGui packs custom rectangles edge to edge, so text drawn at any other size picked up bits of the letters next to it in the atlas)
            g.rect = io.Fonts->AddCustomRectFontGlyph(font, static_cast<ImWchar>(cp), g.w + 2, g.h + 2, advance, ImVec2(offset.x - 1.0f, offset.y - 1.0f));
            pending.push_back(std::move(g));
        }
    }
    for (HFONT f : faces) DeleteObject(f);
    if (outAscent) *outAscent = ascent;
    if (outDescent) *outDescent = descent;
    return font;
}

// A font shipped next to the app (its fonts folder), loaded for this process only (nothing is installed); "" when it is not there.
// Since 2026-10-30: Inter and Space Grotesk (both SIL OFL 1.1, their licences beside them) - Open Source Canvas's UI and display faces.
std::string AppFont(const wchar_t* fileName) {
    wchar_t exe[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return {};
    std::error_code ec;
    const fs::path p = fs::path(exe).parent_path() / L"fonts" / fileName;
    if (!fs::is_regular_file(p, ec)) return {};
    static std::vector<std::wstring> added;
    if (std::find(added.begin(), added.end(), p.wstring()) == added.end() && AddFontResourceExW(p.c_str(), FR_PRIVATE, nullptr) > 0) added.push_back(p.wstring());
    return WideToUtf8(p.wstring());
}

bool BuildFontsGdi(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    // Inter for the UI (its SemiBold as the bold), Space Grotesk Medium for titles; Segoe UI when they are not beside the app
    const std::string interTtf = AppFont(L"Inter-Regular.ttf"), interSemiTtf = AppFont(L"Inter-SemiBold.ttf"), groteskTtf = AppFont(L"SpaceGrotesk-Medium.ttf");
    const bool inter = !interTtf.empty(), interSemi = !interSemiTtf.empty(), grotesk = !groteskTtf.empty();
    const std::string regularTtf = inter ? interTtf : FindSystemFont(L"segoeui.ttf");
    if (regularTtf.empty()) return false;
    std::string semiboldTtf = grotesk ? groteskTtf : FindSystemFont(L"seguisb.ttf");
    if (semiboldTtf.empty()) semiboldTtf = regularTtf;
    const wchar_t* uiFace = inter ? L"Inter" : L"Segoe UI";
    const wchar_t* titleFace = grotesk ? L"Space Grotesk Medium" : L"Segoe UI Semibold";

    // Code points: Latin + Cyrillic + Greek + punctuation/arrows, and CJK for file names and prompts.
    static ImVector<ImWchar> latin, full;
    if (latin.empty()) {
        static const ImWchar extra[] = {0x0370, 0x03FF, 0x2010, 0x2027, 0x2030, 0x205E, 0x2190, 0x2193, 0x2212, 0x2212,
                                        0x2605, 0x2606, 0x2713, 0x2715, 0x25B6, 0x25B6, 0};
        ImFontGlyphRangesBuilder b;
        b.AddRanges(io.Fonts->GetGlyphRangesDefault());
        b.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
        b.AddRanges(extra);
        b.BuildRanges(&latin);
        b.AddRanges(io.Fonts->GetGlyphRangesJapanese());
        b.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        b.AddRanges(io.Fonts->GetGlyphRangesKorean());
        b.BuildRanges(&full);
    }

    static const wchar_t* const kFallbacks[] = {L"Yu Gothic UI", L"Microsoft YaHei UI", L"Malgun Gothic", L"Segoe UI Symbol"};
    const int cell = static_cast<int>(std::round((inter ? 17.0f : 18.0f) * scale)); // (Inter is a little larger than Segoe UI at the same cell)
    const int titleCell = static_cast<int>(std::round(28.0f * scale));

    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    std::vector<PendingGlyph> pending;
    int ascent = 0, descent = 0, boldAscent = 0, boldDescent = 0, titleAscent = 0, titleDescent = 0, displayAscent = 0, displayDescent = 0;

    ImFont* regular = AddGdiFont(dc, uiFace, FW_NORMAL, kFallbacks, 4, cell, full.Data, regularTtf, pending, &ascent, &descent);
    // The bold: Inter SemiBold (Open Source Canvas's 600 weight); with Segoe UI a true bold (its Semibold is too close to regular at small hinted sizes)
    ImFont* bold = !regular ? nullptr
                   : interSemi ? AddGdiFont(dc, L"Inter SemiBold", FW_NORMAL, kFallbacks, 4, cell, latin.Data, interSemiTtf, pending, &boldAscent, &boldDescent)
                               : AddGdiFont(dc, uiFace, FW_BOLD, kFallbacks, 4, cell, latin.Data, regularTtf, pending, &boldAscent, &boldDescent);
    static const ImWchar kAscii[] = {0x20, 0x7E, 0xA0, 0xFF, 0x2013, 0x2014, 0x2018, 0x201D, 0x2026, 0x2026, 0}; // (with the curly quotes: "I'm" in a song's lyrics came out as "I?m")
    ImFont* title = regular ? AddGdiFont(dc, titleFace, FW_NORMAL, nullptr, 0, titleCell, kAscii, semiboldTtf,
                                         pending, &titleAscent, &titleDescent)
                            : nullptr;
    ImFont* display = regular ? AddGdiFont(dc, titleFace, FW_NORMAL, nullptr, 0, static_cast<int>(std::round(54.0f * scale)), kAscii, semiboldTtf,
                                           pending, &displayAscent, &displayDescent)
                              : nullptr;
    // the labels' monospace (Cascadia Mono when Windows has it, else Consolas), small; regular weight (Consolas has no semibold: Windows'
    // made-up bold came out blocky at this size)
    int monoAscent = 0, monoDescent = 0;
    const std::string cascadia = FindSystemFont(L"CascadiaMono.ttf"), consolas = FindSystemFont(L"consola.ttf");
    ImFont* mono = nullptr;
    if (regular && (!cascadia.empty() || !consolas.empty()))
        mono = AddGdiFont(dc, cascadia.empty() ? L"Consolas" : L"Cascadia Mono", FW_NORMAL, nullptr, 0, static_cast<int>(std::round(14.5f * scale)), kAscii,
                          cascadia.empty() ? consolas : cascadia, pending, &monoAscent, &monoDescent);
    DeleteDC(dc);
    if (!regular) return false;

    // Pack the atlas, then copy the GDI bitmaps into their reserved rectangles.
    if (!io.Fonts->Build()) return false;
    const int stride = io.Fonts->TexWidth;
    for (const PendingGlyph& g : pending) {
        const ImFontAtlasCustomRect* r = io.Fonts->GetCustomRectByIndex(g.rect);
        if (!r || !r->IsPacked()) continue;
        for (int y = 0; y < g.h; ++y) {
            memcpy(io.Fonts->TexPixelsAlpha8 + static_cast<size_t>(r->Y + 1 + y) * stride + r->X + 1,
                   g.alpha.data() + static_cast<size_t>(y) * g.w, static_cast<size_t>(g.w));
        }
    }
    // Line metrics from GDI so baselines match the bitmaps exactly.
    regular->Ascent = static_cast<float>(ascent);
    regular->Descent = -static_cast<float>(descent);
    if (bold) {
        bold->Ascent = static_cast<float>(boldAscent);
        bold->Descent = -static_cast<float>(boldDescent);
    }
    if (title) {
        title->Ascent = static_cast<float>(titleAscent);
        title->Descent = -static_cast<float>(titleDescent);
    }
    if (display) {
        display->Ascent = static_cast<float>(displayAscent);
        display->Descent = -static_cast<float>(displayDescent);
    }
    if (mono) {
        mono->Ascent = static_cast<float>(monoAscent);
        mono->Descent = -static_cast<float>(monoDescent);
    }
    g_bold = bold;
    g_title = title;
    g_display = display;
    g_mono = mono;
    io.FontDefault = regular;
    return true;
}

void BuildFonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    g_bold = nullptr;
    g_title = nullptr;
    g_display = nullptr;
    g_mono = nullptr;
    if (BuildFontsGdi(scale)) return;
    io.Fonts->Clear();
    g_bold = nullptr;
    g_title = nullptr;
    g_display = nullptr;
    g_mono = nullptr;
    BuildFontsStb(scale);
}

void ApplyTheme(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();

    // (2026-10-28, "make the whole UI slicker": more air, rounder cards and controls, thin scrollbars; the panels told apart by their shade
    // more than by lines)
    // (2026-10-30, after Open Source Canvas's design - MIT, raph651/open-source-canvas: buttons and chips as pills with real padding,
    // 16-18 px cards, a hairline border on panels and controls, ghost-filled buttons, more air)
    s.WindowPadding = ImVec2(16, 14);
    s.FramePadding = ImVec2(13, 7);
    s.CellPadding = ImVec2(8, 6);
    s.ItemSpacing = ImVec2(8, 8);
    s.ItemInnerSpacing = ImVec2(7, 4);
    s.IndentSpacing = 18;
    s.ScrollbarSize = 9;
    s.GrabMinSize = 12;
    s.WindowBorderSize = 1;
    s.ChildBorderSize = 1;
    s.PopupBorderSize = 1;
    s.FrameBorderSize = 1;
    s.WindowRounding = 16;
    s.ChildRounding = 16;
    s.FrameRounding = 13; // (a button's height is about 30: nearly a pill; a prompt box gets soft corners)
    s.PopupRounding = 14;
    s.ScrollbarRounding = 10;
    s.GrabRounding = 13;
    s.TabRounding = 12;
    s.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    s.SeparatorTextBorderSize = 1;
    s.SeparatorTextPadding = ImVec2(10, 4);
    s.DisplayWindowPadding = ImVec2(20, 20);
    s.ScaleAllSizes(scale);

    ImVec4* c = s.Colors;
    auto rgb = [](int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); };

    // the accent (its colour; strong = pressed; hover a third of the way to its soft shade)
    const AccentDef& a = kAccents[std::clamp(g_accent, 0, kAccentCount - 1)];
    auto mix = [](const int* p, const int* q, float t) { return ImVec4((p[0] + (q[0] - p[0]) * t) / 255.0f, (p[1] + (q[1] - p[1]) * t) / 255.0f,
                                                                       (p[2] + (q[2] - p[2]) * t) / 255.0f, 1.0f); };
    kAccent = rgb(a.c[0], a.c[1], a.c[2]);
    kAccentActive = rgb(a.strong[0], a.strong[1], a.strong[2]);
    kAccentHover = mix(a.c, a.soft, 0.32f);

    // Open Source Canvas's palettes (src/styles.css): light - bg #f1f3f1, panels white, ink #151c17, lines at 10% / 18%; night - bg #090d12,
    // panels #11161d, text #f3f1e8, muted #8d948f / #626a66, lines white at 10% / 18%. Controls are "ghost": a faint fill of the ink over the panel.
    if (g_light) {
        kGood = rgb(22, 140, 70);
        kWarn = rgb(166, 124, 0);
        kBad = rgb(217, 56, 42);
        kTextDim = rgb(93, 101, 95);
        auto ink = [&](float al) { return rgb(21, 28, 23, al); };

        c[ImGuiCol_Text] = rgb(21, 28, 23);
        c[ImGuiCol_TextDisabled] = kTextDim;
        c[ImGuiCol_WindowBg] = rgb(241, 243, 241);
        c[ImGuiCol_ChildBg] = rgb(255, 255, 255, 0.86f);
        c[ImGuiCol_PopupBg] = rgb(255, 255, 255, 0.99f);
        c[ImGuiCol_Border] = ink(0.10f);
        c[ImGuiCol_BorderShadow] = rgb(0, 0, 0, 0);
        c[ImGuiCol_FrameBg] = ink(0.035f);
        c[ImGuiCol_FrameBgHovered] = ink(0.07f);
        c[ImGuiCol_FrameBgActive] = ink(0.10f);
        c[ImGuiCol_TitleBg] = rgb(241, 243, 241);
        c[ImGuiCol_TitleBgActive] = rgb(233, 237, 234);
        c[ImGuiCol_TitleBgCollapsed] = rgb(241, 243, 241);
        c[ImGuiCol_MenuBarBg] = rgb(241, 243, 241);
        c[ImGuiCol_ScrollbarBg] = rgb(0, 0, 0, 0);
        c[ImGuiCol_ScrollbarGrab] = ink(0.13f);
        c[ImGuiCol_ScrollbarGrabHovered] = ink(0.22f);
        c[ImGuiCol_ScrollbarGrabActive] = ink(0.30f);
        c[ImGuiCol_Button] = ink(0.035f);
        c[ImGuiCol_ButtonHovered] = ink(0.075f);
        c[ImGuiCol_ButtonActive] = ink(0.11f);
        c[ImGuiCol_Header] = ink(0.05f);
        c[ImGuiCol_HeaderHovered] = ink(0.08f);
        c[ImGuiCol_HeaderActive] = ink(0.11f);
        c[ImGuiCol_Separator] = ink(0.10f);
        c[ImGuiCol_ResizeGrip] = ink(0.12f);
        c[ImGuiCol_Tab] = ink(0.035f);
        c[ImGuiCol_TabSelected] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.22f);
        c[ImGuiCol_ModalWindowDimBg] = ink(0.30f);
        c[ImGuiCol_TableHeaderBg] = ink(0.04f);
        c[ImGuiCol_TableBorderStrong] = ink(0.10f);
        c[ImGuiCol_TableBorderLight] = ink(0.06f);
        c[ImGuiCol_TableRowBgAlt] = ink(0.025f);
    } else {
        kGood = rgb(110, 231, 160);
        kWarn = rgb(255, 226, 122);
        kBad = rgb(255, 113, 100);
        kTextDim = rgb(141, 148, 143);
        auto ov = [&](float al) { return rgb(255, 255, 255, al); };

        c[ImGuiCol_Text] = rgb(243, 241, 232);
        c[ImGuiCol_TextDisabled] = kTextDim;
        c[ImGuiCol_WindowBg] = rgb(9, 13, 18);
        c[ImGuiCol_ChildBg] = rgb(17, 22, 29, 0.82f); // (a little see-through: the glow behind the page shows)
        c[ImGuiCol_PopupBg] = rgb(17, 22, 29, 0.98f);
        c[ImGuiCol_Border] = ov(0.10f);
        c[ImGuiCol_BorderShadow] = rgb(0, 0, 0, 0);
        c[ImGuiCol_FrameBg] = ov(0.035f);
        c[ImGuiCol_FrameBgHovered] = ov(0.07f);
        c[ImGuiCol_FrameBgActive] = ov(0.10f);
        c[ImGuiCol_TitleBg] = rgb(9, 13, 18);
        c[ImGuiCol_TitleBgActive] = rgb(17, 22, 29);
        c[ImGuiCol_TitleBgCollapsed] = rgb(9, 13, 18);
        c[ImGuiCol_MenuBarBg] = rgb(9, 13, 18, 0.0f);
        c[ImGuiCol_ScrollbarBg] = rgb(0, 0, 0, 0);
        c[ImGuiCol_ScrollbarGrab] = ov(0.13f);
        c[ImGuiCol_ScrollbarGrabHovered] = ov(0.20f);
        c[ImGuiCol_ScrollbarGrabActive] = ov(0.28f);
        c[ImGuiCol_Button] = ov(0.04f);
        c[ImGuiCol_ButtonHovered] = ov(0.08f);
        c[ImGuiCol_ButtonActive] = ov(0.12f);
        c[ImGuiCol_Header] = ov(0.05f);
        c[ImGuiCol_HeaderHovered] = ov(0.08f);
        c[ImGuiCol_HeaderActive] = ov(0.11f);
        c[ImGuiCol_Separator] = ov(0.10f);
        c[ImGuiCol_ResizeGrip] = ov(0.08f);
        c[ImGuiCol_Tab] = ov(0.035f);
        c[ImGuiCol_TabSelected] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.30f);
        c[ImGuiCol_ModalWindowDimBg] = rgb(0, 0, 0, 0.62f);
        c[ImGuiCol_TableHeaderBg] = ov(0.04f);
        c[ImGuiCol_TableBorderStrong] = ov(0.10f);
        c[ImGuiCol_TableBorderLight] = ov(0.06f);
        c[ImGuiCol_TableRowBgAlt] = ov(0.02f);
    }
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_SliderGrab] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.62f); // see-through: the slider's value is drawn under it
    c[ImGuiCol_SliderGrabActive] = ImVec4(kAccentHover.x, kAccentHover.y, kAccentHover.z, 0.85f);
    c[ImGuiCol_SeparatorHovered] = kAccent;
    c[ImGuiCol_SeparatorActive] = kAccentHover;
    c[ImGuiCol_ResizeGripHovered] = kAccent;
    c[ImGuiCol_ResizeGripActive] = kAccentHover;
    c[ImGuiCol_TabHovered] = kAccent;
    c[ImGuiCol_TextSelectedBg] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.35f);
    c[ImGuiCol_DragDropTarget] = kAccent;
    c[ImGuiCol_NavHighlight] = kAccent;
    c[ImGuiCol_NavWindowingHighlight] = rgb(255, 255, 255, 0.7f);
    c[ImGuiCol_NavWindowingDimBg] = rgb(0, 0, 0, 0.5f);
    c[ImGuiCol_TableRowBg] = rgb(0, 0, 0, 0);
    c[ImGuiCol_PlotHistogram] = kAccent;
    c[ImGuiCol_PlotHistogramHovered] = kAccentHover;
}

} // namespace

void Init(float scale, bool light) {
    g_light = light;
    g_scale = scale > 0.1f ? scale : 1.0f;
    BuildFonts(g_scale);
    ApplyTheme(g_scale);
}

float Scale() { return g_scale; }
bool IsLight() { return g_light; }

int AccentCount() { return kAccentCount; }
const char* AccentName(int i) { return kAccents[std::clamp(i, 0, kAccentCount - 1)].name; }
ImVec4 AccentSwatch(int i) {
    const int* p = kAccents[std::clamp(i, 0, kAccentCount - 1)].c;
    return ImVec4(p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, 1.0f);
}
void SetAccentIndex(int i) { g_accent = std::clamp(i, 0, kAccentCount - 1); }
int AccentIndex() { return g_accent; }

ImVec4 ViewerBackground() { return g_light ? ImVec4(0.925f, 0.937f, 0.929f, 1.0f) : ImVec4(0.024f, 0.035f, 0.047f, 1.0f); }
ImU32 ThumbPlaceholder() { return g_light ? IM_COL32(21, 28, 23, 18) : IM_COL32(255, 255, 255, 14); }
// chips: ghost when off (the ink's faint fill, as Open Source Canvas's ghost buttons), the accent when on
ImVec4 ChipColor() { return g_light ? ImVec4(0.082f, 0.11f, 0.09f, 0.045f) : ImVec4(1.0f, 1.0f, 1.0f, 0.05f); }
ImVec4 ChipHoverColor() { return g_light ? ImVec4(0.082f, 0.11f, 0.09f, 0.09f) : ImVec4(1.0f, 1.0f, 1.0f, 0.10f); }
ImVec4 ChipActiveColor() { return ImVec4(kAccent.x, kAccent.y, kAccent.z, g_light ? 0.88f : 0.85f); }
ImU32 StarColor(bool filled) {
    if (filled) return g_light ? IM_COL32(214, 140, 0, 255) : IM_COL32(255, 196, 64, 255);
    return g_light ? IM_COL32(170, 178, 192, 255) : IM_COL32(90, 98, 112, 255);
}

ImFont* BoldFont() { return g_bold ? g_bold : ImGui::GetFont(); }
ImFont* TitleFont() { return g_title ? g_title : BoldFont(); }
ImFont* DisplayFont() { return g_display ? g_display : TitleFont(); }
ImFont* MonoFont() { return g_mono ? g_mono : ImGui::GetFont(); }

void Eyebrow(const char* text, const ImVec4* color) {
    // capitals, a monospace, and letters spread out (about 0.16 em)
    ImFont* f = MonoFont();
    const float size = f->FontSize, track = size * 0.16f;
    std::string up = text ? text : "";
    for (char& ch : up) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    float w = 0.0f;
    for (char ch : up) {
        const char s[2] = {ch, 0};
        w += f->CalcTextSizeA(size, FLT_MAX, 0.0f, s).x + track;
    }
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float lineH = ImGui::GetTextLineHeight();
    ImGui::Dummy(ImVec2(std::max(0.0f, w - track), lineH));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = ImGui::GetColorU32(color ? *color : (g_light ? ImVec4(0.54f, 0.57f, 0.55f, 1.0f) : ImVec4(0.55f, 0.58f, 0.56f, 1.0f)));
    float x = p.x;
    const float y = p.y + (lineH - size) * 0.5f;
    for (char ch : up) {
        const char s[2] = {ch, 0};
        dl->AddText(f, size, ImVec2(x, y), col, s);
        x += f->CalcTextSizeA(size, FLT_MAX, 0.0f, s).x + track;
    }
}

void SameLineIfFits(const char* nextLabel) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float w = ImGui::CalcTextSize(nextLabel, nullptr, true).x + st.FramePadding.x * 2.0f;
    const float limit = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    if (ImGui::GetItemRectMax().x + st.ItemSpacing.x + w <= limit) ImGui::SameLine();
}

bool PrimaryButton(const char* label, const ImVec2& size) {
    ImGui::PushStyleColor(ImGuiCol_Button, kAccentActive);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentHover);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    if (!g_light && ImGui::IsItemVisible()) {
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const float r = ImGui::GetStyle().FrameRounding;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(a, ImVec2(b.x, a.y + (b.y - a.y) * 0.5f), IM_COL32(255, 255, 255, 18), r, ImDrawFlags_RoundCornersTop);
        dl->AddRect(a, b, IM_COL32(255, 255, 255, 28), r);
    }
    return pressed;
}

void HelpMarker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void Pill(const char* text, ImU32 background, ImU32 foreground) {
    ImVec2 textSize = ImGui::CalcTextSize(text);
    ImVec2 pad(Dp(8), Dp(2));
    ImVec2 size(textSize.x + pad.x * 2, textSize.y + pad.y * 2);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), background, size.y * 0.5f);
    dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), (foreground & 0x00FFFFFF) | (static_cast<ImU32>(70) << IM_COL32_A_SHIFT), size.y * 0.5f); // (a hairline edge, as its badges)
    dl->AddText(ImVec2(pos.x + pad.x, pos.y + pad.y), foreground, text);
}

void DrawBackdrop(ImDrawList* dl, ImVec2 min, ImVec2 max) {
    if (g_light) return; // (its day theme is plain)
    const float w = max.x - min.x, h = max.y - min.y;
    // radial-gradient(1100px 680px at 12% -12%, rgba(88,124,255,.19), transparent 62%) and (900px 620px at 84% 2%, rgba(255,122,61,.07), 58%):
    // a gradient made of stacked ellipses, each a little smaller and adding a little more colour
    auto glow = [&](float cx, float cy, float rx, float ry, int r, int g, int b, float alpha, float reach) {
        const int steps = 24;
        for (int k = 0; k < steps; ++k) {
            const float t = 1.0f - static_cast<float>(k) / steps; // 1 at the edge -> small at the middle
            const float a = alpha / steps * 1.35f;
            dl->AddEllipseFilled(ImVec2(cx, cy), ImVec2(rx * reach * t, ry * reach * t), IM_COL32(r, g, b, static_cast<int>(a * 255.0f)), 0.0f, 64);
        }
    };
    dl->PushClipRect(min, max, true);
    glow(min.x + w * 0.12f, min.y - h * 0.12f, Dp(1100.0f) * 0.5f, Dp(680.0f) * 0.5f, 88, 124, 255, 0.19f, 1.24f);
    glow(min.x + w * 0.84f, min.y + h * 0.02f, Dp(900.0f) * 0.5f, Dp(620.0f) * 0.5f, 255, 122, 61, 0.07f, 1.16f);
    dl->PopClipRect();
}

void SectionHeading(const char* text) { // (Open Source Canvas's section label: no bar, no rule - the small capitals and the room under them)
    ImGui::Dummy(ImVec2(0, Dp(2.0f)));
    Eyebrow(text);
    ImGui::Dummy(ImVec2(0, Dp(2.0f)));
}

// (fold buttons are drawn when the frame ends, over what their pane drew after them - a picture would cover them otherwise)
struct PendingFold {
    ImDrawList* dl;
    ImVec2 c, clipMin, clipMax;
    bool hov;
};
std::vector<PendingFold> g_folds;

bool FoldButton(const char* id, const char* tip) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    const float r = Dp(9.0f), pad = Dp(5.0f);
    const ImVec2 back = ImGui::GetCursorScreenPos();
    const ImVec2 c(w->InnerRect.Max.x - r - pad, w->InnerRect.Min.y + r + pad);
    ImGui::SetCursorScreenPos(ImVec2(c.x - r, c.y - r));
    const bool hit = ImGui::InvisibleButton(id, ImVec2(r * 2.0f, r * 2.0f));
    const bool hov = ImGui::IsItemHovered();
    if (hov && tip && *tip) ImGui::SetTooltip("%s", tip);
    g_folds.push_back({ImGui::GetWindowDrawList(), c, w->InnerRect.Min, w->InnerRect.Max, hov});
    ImGui::SetCursorScreenPos(back); // (the pane draws from where it was)
    return hit;
}

void DrawPendingFoldButtons() {
    for (const PendingFold& f : g_folds) {
        f.dl->PushClipRect(f.clipMin, f.clipMax, false);
        const float r = Dp(9.0f), a = r * 0.35f; // a "-": fold
        f.dl->AddCircleFilled(f.c, r, f.hov ? ImGui::GetColorU32(kAccent) : IM_COL32(0, 0, 0, 140), 20);
        f.dl->AddLine(ImVec2(f.c.x - a, f.c.y), ImVec2(f.c.x + a, f.c.y), IM_COL32(255, 255, 255, f.hov ? 255 : 200), Dp(1.6f));
        f.dl->PopClipRect();
    }
    g_folds.clear();
}

bool FoldStrip(const char* id, const char* name, float width, float height) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool hit = ImGui::InvisibleButton(id, ImVec2(width, height));
    const bool hov = ImGui::IsItemHovered();
    if (hov) ImGui::SetTooltip("%s - click to show it again", name);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + height), hov ? IM_COL32(255, 255, 255, 26) : IM_COL32(255, 255, 255, 12), Dp(8.0f));
    float y = p.y + Dp(12.0f);
    for (const char* ch = name; *ch && y < p.y + height - ImGui::GetFontSize(); ++ch) { // (its name, a letter under the other)
        const char s[2] = {*ch, 0};
        dl->AddText(ImVec2(p.x + (width - ImGui::CalcTextSize(s).x) * 0.5f, y), ImGui::GetColorU32(hov ? ImGuiCol_Text : ImGuiCol_TextDisabled), s);
        y += ImGui::GetFontSize() * (*ch == ' ' ? 0.6f : 0.95f);
    }
    return hit;
}

ImU32 WithAlpha(const ImVec4& c, float alpha) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, alpha));
}

std::string FormatBytes(unsigned long long bytes) {
    char buf[32];
    if (bytes < 1024) {
        snprintf(buf, sizeof(buf), "%llu B", bytes);
    } else if (bytes < 1024ull * 1024) {
        snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    } else if (bytes < 1024ull * 1024 * 1024) {
        snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
    } else {
        snprintf(buf, sizeof(buf), "%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
    }
    return buf;
}

} // namespace ui
