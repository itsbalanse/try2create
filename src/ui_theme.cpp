#include "ui_theme.h"

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
            g.rect = io.Fonts->AddCustomRectFontGlyph(font, static_cast<ImWchar>(cp), g.w, g.h, advance, offset);
            pending.push_back(std::move(g));
        }
    }
    for (HFONT f : faces) DeleteObject(f);
    if (outAscent) *outAscent = ascent;
    if (outDescent) *outDescent = descent;
    return font;
}

bool BuildFontsGdi(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    const std::string regularTtf = FindSystemFont(L"segoeui.ttf");
    if (regularTtf.empty()) return false;
    std::string semiboldTtf = FindSystemFont(L"seguisb.ttf");
    if (semiboldTtf.empty()) semiboldTtf = regularTtf;

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
    const int cell = static_cast<int>(std::round(18.0f * scale));
    const int titleCell = static_cast<int>(std::round(28.0f * scale));

    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    std::vector<PendingGlyph> pending;
    int ascent = 0, descent = 0, boldAscent = 0, boldDescent = 0, titleAscent = 0, titleDescent = 0, displayAscent = 0, displayDescent = 0;

    ImFont* regular = AddGdiFont(dc, L"Segoe UI", FW_NORMAL, kFallbacks, 4, cell, full.Data, regularTtf, pending, &ascent, &descent);
    // True bold (Segoe UI Semibold is too close to regular weight at small hinted sizes).
    ImFont* bold = regular ? AddGdiFont(dc, L"Segoe UI", FW_BOLD, kFallbacks, 4, cell, latin.Data, regularTtf, pending,
                                        &boldAscent, &boldDescent)
                           : nullptr;
    static const ImWchar kAscii[] = {0x20, 0x7E, 0xA0, 0xFF, 0x2013, 0x2014, 0x2026, 0x2026, 0};
    ImFont* title = regular ? AddGdiFont(dc, L"Segoe UI Semibold", FW_NORMAL, nullptr, 0, titleCell, kAscii, semiboldTtf,
                                         pending, &titleAscent, &titleDescent)
                            : nullptr;
    ImFont* display = regular ? AddGdiFont(dc, L"Segoe UI Semibold", FW_NORMAL, nullptr, 0, static_cast<int>(std::round(54.0f * scale)), kAscii, semiboldTtf,
                                           pending, &displayAscent, &displayDescent)
                              : nullptr;
    DeleteDC(dc);
    if (!regular) return false;

    // Pack the atlas, then copy the GDI bitmaps into their reserved rectangles.
    if (!io.Fonts->Build()) return false;
    const int stride = io.Fonts->TexWidth;
    for (const PendingGlyph& g : pending) {
        const ImFontAtlasCustomRect* r = io.Fonts->GetCustomRectByIndex(g.rect);
        if (!r || !r->IsPacked()) continue;
        for (int y = 0; y < g.h; ++y) {
            memcpy(io.Fonts->TexPixelsAlpha8 + static_cast<size_t>(r->Y + y) * stride + r->X,
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
    g_bold = bold;
    g_title = title;
    g_display = display;
    io.FontDefault = regular;
    return true;
}

void BuildFonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    g_bold = nullptr;
    g_title = nullptr;
    g_display = nullptr;
    if (BuildFontsGdi(scale)) return;
    io.Fonts->Clear();
    g_bold = nullptr;
    g_title = nullptr;
    g_display = nullptr;
    BuildFontsStb(scale);
}

void ApplyTheme(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();

    s.WindowPadding = ImVec2(12, 10);
    s.FramePadding = ImVec2(10, 6);
    s.CellPadding = ImVec2(8, 5);
    s.ItemSpacing = ImVec2(8, 7);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.IndentSpacing = 18;
    s.ScrollbarSize = 12;
    s.GrabMinSize = 12;
    s.WindowBorderSize = 1;
    s.ChildBorderSize = 1;
    s.PopupBorderSize = 1;
    s.FrameBorderSize = 0;
    s.WindowRounding = 8;
    s.ChildRounding = 8;
    s.FrameRounding = 6;
    s.PopupRounding = 8;
    s.ScrollbarRounding = 8;
    s.GrabRounding = 6;
    s.TabRounding = 6;
    s.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    s.SeparatorTextBorderSize = 1;
    s.SeparatorTextPadding = ImVec2(10, 4);
    s.DisplayWindowPadding = ImVec2(20, 20);
    s.ScaleAllSizes(scale);

    ImVec4* c = s.Colors;
    auto rgb = [](int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); };

    if (g_light) {
        kAccent = rgb(37, 99, 235);
        kAccentHover = rgb(59, 119, 245);
        kAccentActive = rgb(29, 78, 200);
        kGood = rgb(22, 140, 70);
        kWarn = rgb(176, 112, 0);
        kBad = rgb(200, 45, 45);
        kTextDim = rgb(104, 112, 128);

        c[ImGuiCol_Text] = rgb(26, 30, 38);
        c[ImGuiCol_TextDisabled] = kTextDim;
        c[ImGuiCol_WindowBg] = rgb(240, 242, 246);
        c[ImGuiCol_ChildBg] = rgb(249, 250, 252);
        c[ImGuiCol_PopupBg] = rgb(252, 252, 254, 0.99f);
        c[ImGuiCol_Border] = rgb(212, 217, 227);
        c[ImGuiCol_BorderShadow] = rgb(0, 0, 0, 0);
        c[ImGuiCol_FrameBg] = rgb(228, 232, 240);
        c[ImGuiCol_FrameBgHovered] = rgb(218, 224, 235);
        c[ImGuiCol_FrameBgActive] = rgb(207, 215, 230);
        c[ImGuiCol_TitleBg] = rgb(240, 242, 246);
        c[ImGuiCol_TitleBgActive] = rgb(232, 235, 242);
        c[ImGuiCol_TitleBgCollapsed] = rgb(240, 242, 246);
        c[ImGuiCol_MenuBarBg] = rgb(249, 250, 252);
        c[ImGuiCol_ScrollbarBg] = rgb(0, 0, 0, 0);
        c[ImGuiCol_ScrollbarGrab] = rgb(184, 192, 206);
        c[ImGuiCol_ScrollbarGrabHovered] = rgb(160, 170, 188);
        c[ImGuiCol_ScrollbarGrabActive] = rgb(136, 148, 170);
        c[ImGuiCol_Button] = rgb(222, 227, 237);
        c[ImGuiCol_ButtonHovered] = rgb(208, 216, 230);
        c[ImGuiCol_ButtonActive] = rgb(192, 203, 222);
        c[ImGuiCol_Header] = rgb(226, 231, 240);
        c[ImGuiCol_HeaderHovered] = rgb(214, 222, 236);
        c[ImGuiCol_HeaderActive] = rgb(200, 211, 230);
        c[ImGuiCol_Separator] = rgb(212, 217, 227);
        c[ImGuiCol_ResizeGrip] = rgb(160, 170, 188, 0.5f);
        c[ImGuiCol_Tab] = rgb(228, 232, 240);
        c[ImGuiCol_TabSelected] = rgb(190, 210, 250);
        c[ImGuiCol_ModalWindowDimBg] = rgb(30, 36, 48, 0.35f);
        c[ImGuiCol_TableHeaderBg] = rgb(234, 237, 244);
        c[ImGuiCol_TableBorderStrong] = rgb(212, 217, 227);
        c[ImGuiCol_TableBorderLight] = rgb(224, 228, 237);
        c[ImGuiCol_TableRowBgAlt] = rgb(0, 0, 0, 0.03f);
    } else {
        kAccent = ImVec4(0.30f, 0.60f, 1.00f, 1.00f);
        kAccentHover = ImVec4(0.42f, 0.69f, 1.00f, 1.00f);
        kAccentActive = ImVec4(0.22f, 0.50f, 0.90f, 1.00f);
        kGood = ImVec4(0.45f, 0.85f, 0.55f, 1.00f);
        kWarn = ImVec4(1.00f, 0.78f, 0.33f, 1.00f);
        kBad = ImVec4(1.00f, 0.45f, 0.42f, 1.00f);
        kTextDim = ImVec4(0.55f, 0.59f, 0.66f, 1.00f);

        c[ImGuiCol_Text] = rgb(230, 233, 238);
        c[ImGuiCol_TextDisabled] = kTextDim;
        c[ImGuiCol_WindowBg] = rgb(20, 22, 26);
        c[ImGuiCol_ChildBg] = rgb(24, 27, 32);
        c[ImGuiCol_PopupBg] = rgb(28, 32, 38, 0.98f);
        c[ImGuiCol_Border] = rgb(46, 52, 62);
        c[ImGuiCol_BorderShadow] = rgb(0, 0, 0, 0);
        c[ImGuiCol_FrameBg] = rgb(34, 38, 45);
        c[ImGuiCol_FrameBgHovered] = rgb(42, 48, 57);
        c[ImGuiCol_FrameBgActive] = rgb(50, 57, 68);
        c[ImGuiCol_TitleBg] = rgb(20, 22, 26);
        c[ImGuiCol_TitleBgActive] = rgb(28, 32, 38);
        c[ImGuiCol_TitleBgCollapsed] = rgb(20, 22, 26);
        c[ImGuiCol_MenuBarBg] = rgb(24, 27, 32);
        c[ImGuiCol_ScrollbarBg] = rgb(0, 0, 0, 0);
        c[ImGuiCol_ScrollbarGrab] = rgb(70, 78, 92);
        c[ImGuiCol_ScrollbarGrabHovered] = rgb(92, 102, 120);
        c[ImGuiCol_ScrollbarGrabActive] = rgb(116, 128, 150);
        c[ImGuiCol_Button] = rgb(44, 50, 60);
        c[ImGuiCol_ButtonHovered] = rgb(58, 66, 80);
        c[ImGuiCol_ButtonActive] = rgb(72, 82, 98);
        c[ImGuiCol_Header] = rgb(36, 42, 52);
        c[ImGuiCol_HeaderHovered] = rgb(46, 54, 66);
        c[ImGuiCol_HeaderActive] = rgb(54, 64, 80);
        c[ImGuiCol_Separator] = rgb(46, 52, 62);
        c[ImGuiCol_ResizeGrip] = rgb(70, 78, 92, 0.5f);
        c[ImGuiCol_Tab] = rgb(34, 38, 45);
        c[ImGuiCol_TabSelected] = rgb(48, 100, 180);
        c[ImGuiCol_ModalWindowDimBg] = rgb(0, 0, 0, 0.55f);
        c[ImGuiCol_TableHeaderBg] = rgb(30, 34, 41);
        c[ImGuiCol_TableBorderStrong] = rgb(46, 52, 62);
        c[ImGuiCol_TableBorderLight] = rgb(38, 43, 51);
        c[ImGuiCol_TableRowBgAlt] = rgb(255, 255, 255, 0.025f);
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

ImVec4 ViewerBackground() { return g_light ? ImVec4(0.93f, 0.94f, 0.96f, 1.0f) : ImVec4(0.050f, 0.057f, 0.070f, 1.0f); }
ImU32 ThumbPlaceholder() { return g_light ? IM_COL32(222, 227, 236, 255) : IM_COL32(38, 43, 52, 255); }
ImVec4 ChipColor() { return g_light ? ImVec4(0.86f, 0.89f, 0.95f, 1.0f) : ImVec4(0.18f, 0.21f, 0.27f, 1.0f); }
ImVec4 ChipHoverColor() { return g_light ? ImVec4(0.74f, 0.82f, 0.96f, 1.0f) : ImVec4(0.24f, 0.33f, 0.48f, 1.0f); }
ImVec4 ChipActiveColor() { return g_light ? ImVec4(0.62f, 0.74f, 0.95f, 1.0f) : ImVec4(0.28f, 0.42f, 0.66f, 1.0f); }
ImU32 StarColor(bool filled) {
    if (filled) return g_light ? IM_COL32(214, 140, 0, 255) : IM_COL32(255, 196, 64, 255);
    return g_light ? IM_COL32(170, 178, 192, 255) : IM_COL32(90, 98, 112, 255);
}

ImFont* BoldFont() { return g_bold ? g_bold : ImGui::GetFont(); }
ImFont* TitleFont() { return g_title ? g_title : BoldFont(); }
ImFont* DisplayFont() { return g_display ? g_display : TitleFont(); }

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
    dl->AddText(ImVec2(pos.x + pad.x, pos.y + pad.y), foreground, text);
}

void SectionHeading(const char* text) {
    ImGui::PushFont(BoldFont());
    ImGui::TextColored(kTextDim, "%s", text);
    ImGui::PopFont();
    ImGui::Separator();
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
