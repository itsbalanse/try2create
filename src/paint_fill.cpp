// Edits > Paint and fill (2026-10-29, after NVAI-open's fill, github.com/Mantux775/NVAI-open, MIT): brush over a picture what should change, say
// what goes there - or nothing, and it is taken out - and FLUX 3 Image Klein redraws only the painted area (comfy.cpp BuildFlux3Edit: the masked
// latent with Differential Diffusion, then pasted back through the mask, so the rest of the picture stays exactly as it was). The result goes to
// the Edits list like every edit.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>
#include <windows.h>

#include "app_state.h"
#include "image_texture.h"
#include "imgui.h"
#include "pixel_art.h"
#include "ui_extra.h"
#include "ui_theme.h"
#include "win_dialogs.h"

namespace fs = std::filesystem;

namespace {

struct Stroke {
    std::vector<ImVec2> pts; // in 0..1 of the picture
    float radius = 0.04f;    // a share of the picture's width
    bool erase = false;
};

struct PaintState {
    std::wstring picture;
    LoadedImage img;
    std::string error;
    std::vector<Stroke> strokes;
    bool painting = false;
    float brush = 0.04f;
    bool eraser = false;
    char words[600] = {};
    int times = 1;
};
PaintState g_p;

std::string U8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

void SetPicture(AppState& app, const std::wstring& path) {
    if (path.empty()) return;
    LoadedImage img;
    std::string err;
    if (!LoadImageTexture(app.Device(), path, img, &err)) {
        g_p.error = "Could not open the picture: " + err;
        return;
    }
    ReleaseImage(g_p.img);
    g_p.img = img;
    g_p.picture = path;
    g_p.strokes.clear();
    g_p.error.clear();
}

// The mask at w x h: white where the brush went (the eraser takes it back), in the order painted
std::vector<uint8_t> Rasterize(int w, int h) {
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4, 0);
    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    auto stamp = [&](float cx, float cy, float r, bool erase) {
        const int x0 = std::max(0, static_cast<int>(cx - r)), x1 = std::min(w - 1, static_cast<int>(cx + r));
        const int y0 = std::max(0, static_cast<int>(cy - r)), y1 = std::min(h - 1, static_cast<int>(cy + r));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                if ((x - cx) * (x - cx) + (y - cy) * (y - cy) > r * r) continue;
                uint8_t* p = &rgba[(static_cast<size_t>(y) * w + x) * 4];
                p[0] = p[1] = p[2] = erase ? 0 : 255;
            }
    };
    for (const Stroke& s : g_p.strokes) {
        const float r = s.radius * w;
        for (size_t k = 0; k < s.pts.size(); ++k) {
            const ImVec2 a(s.pts[k].x * w, s.pts[k].y * h);
            const ImVec2 b = k + 1 < s.pts.size() ? ImVec2(s.pts[k + 1].x * w, s.pts[k + 1].y * h) : a;
            const float len = std::hypot(b.x - a.x, b.y - a.y);
            const int n = std::max(1, static_cast<int>(len / std::max(1.0f, r * 0.3f)));
            for (int t = 0; t <= n; ++t) stamp(a.x + (b.x - a.x) * t / n, a.y + (b.y - a.y) * t / n, r, s.erase);
        }
    }
    return rgba;
}

bool AnyPainted() {
    for (const Stroke& s : g_p.strokes)
        if (!s.erase && !s.pts.empty()) return true;
    return false;
}

void Make(AppState& app) {
    if (g_p.picture.empty() || !AnyPainted()) return;
    std::vector<unsigned char> pic;
    int w = 0, h = 0;
    std::string err;
    if (!DecodeImageRGBA(g_p.picture, pic, w, h, &err) || w <= 0 || h <= 0) {
        g_p.error = "Could not read the picture: " + err;
        return;
    }
    wchar_t buf[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    const fs::path dir = fs::path(buf) / L"Try2Create" / L"masks";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const auto now = std::chrono::system_clock::now().time_since_epoch().count();
    const std::wstring mask = (dir / (L"fill_" + std::to_wstring(now) + L".png")).wstring();
    const std::wstring grey = (dir / (L"fill_" + std::to_wstring(now) + L"_grey.png")).wstring();
    const std::vector<uint8_t> rgba = Rasterize(w, h);
    for (size_t i = 0; i < rgba.size(); i += 4) // the copy FLUX 3 is shown: the painted area flat grey
        if (rgba[i]) pic[i] = pic[i + 1] = pic[i + 2] = 128, pic[i + 3] = 255;
    if (!PixelSavePng(mask, rgba.data(), w, h) || !PixelSavePng(grey, pic.data(), w, h)) {
        g_p.error = "Could not save the painted area";
        return;
    }
    std::string words = g_p.words;
    while (!words.empty() && (words.back() == ' ' || words.back() == '\n' || words.back() == '.')) words.pop_back();
    // (NVAI-open's words for its fill)
    const std::string fillWords = "Fill the flat grey area with new content that blends seamlessly into the picture, with the same light, perspective and style, "
                                  "and keep everything outside the grey area unchanged.";
    const std::string instruction = words.empty() ? fillWords + " Continue the surrounding scene so that the area fits in naturally."
                                                  : fillWords + " Fill it with: " + words + ".";
    EditContext ctx;
    ctx.fillMask = true;
    ctx.mask = mask;
    ctx.grey = grey;
    const std::string label = words.empty() ? std::string("Take out the painted area") : "Fill: " + words.substr(0, 60);
    SendPictureEdit(app, {g_p.picture}, instruction, label, g_p.times, &ctx);
}

} // namespace

void PaintFillTakePicture(AppState& app, const std::wstring& path) { SetPicture(app, path); }

void DrawPaintFillBody(AppState& app, float height) {
    { // tests: MV_FILL_TEST "picture|x0,y0,x1,y1|words" (the box in 0..1, painted as one stroke) makes it once
        static bool once = false;
        wchar_t buf[2048] = {};
        if (!once && GetEnvironmentVariableW(L"MV_FILL_TEST", buf, 2048) > 0) {
            once = true;
            const std::wstring all = buf;
            const size_t a = all.find(L'|'), b = all.find(L'|', a + 1);
            if (a != std::wstring::npos) {
                SetPicture(app, all.substr(0, a));
                float x0 = 0.4f, y0 = 0.4f, x1 = 0.6f, y1 = 0.6f;
                swscanf_s(all.substr(a + 1, b == std::wstring::npos ? std::wstring::npos : b - a - 1).c_str(), L"%f,%f,%f,%f", &x0, &y0, &x1, &y1);
                Stroke s;
                s.radius = 0.03f;
                for (float y = y0; y <= y1 + 1e-4f; y += 0.025f) { // back and forth over the box
                    s.pts.push_back(ImVec2(x0, y));
                    s.pts.push_back(ImVec2(x1, y));
                }
                g_p.strokes.push_back(s);
                const std::string words = b == std::wstring::npos ? std::string() : U8(all.substr(b + 1));
                strncpy_s(g_p.words, words.c_str(), sizeof(g_p.words) - 1);
                Make(app);
            }
        }
    }
    if (!ImGui::BeginTable("##paintfill", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings, ImVec2(0, height))) return;
    ImGui::TableSetupColumn("picture", ImGuiTableColumnFlags_WidthStretch, 0.66f);
    ImGui::TableSetupColumn("controls", ImGuiTableColumnFlags_WidthStretch, 0.34f);
    ImGui::TableNextRow(0, height);

    // ---- the picture, painted on
    ImGui::TableSetColumnIndex(0);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 region(ImGui::GetContentRegionAvail().x, height - ui::Dp(8.0f));
    ImGui::InvisibleButton("##paintcanvas", region, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, ImVec2(origin.x + region.x, origin.y + region.y), IM_COL32(255, 255, 255, 8), ui::Dp(6.0f));
    if (g_p.img.srv && g_p.img.width > 0 && g_p.img.height > 0) {
        const float s = std::min(region.x / g_p.img.width, region.y / g_p.img.height);
        const ImVec2 dim(g_p.img.width * s, g_p.img.height * s);
        const ImVec2 a(origin.x + (region.x - dim.x) * 0.5f, origin.y + (region.y - dim.y) * 0.5f), b(a.x + dim.x, a.y + dim.y);
        dl->AddImage((ImTextureID)(intptr_t)g_p.img.srv, a, b);
        const ImVec2 m = ImGui::GetIO().MousePos;
        const ImVec2 at((m.x - a.x) / dim.x, (m.y - a.y) / dim.y);
        const bool left = ImGui::IsMouseDown(ImGuiMouseButton_Left), right = ImGui::IsMouseDown(ImGuiMouseButton_Right);
        if (hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
            Stroke st;
            st.radius = g_p.brush;
            st.erase = g_p.eraser || right;
            g_p.strokes.push_back(st);
            g_p.painting = true;
        }
        if (g_p.painting && (left || right)) {
            Stroke& st = g_p.strokes.back();
            const ImVec2 c(std::clamp(at.x, 0.0f, 1.0f), std::clamp(at.y, 0.0f, 1.0f));
            if (st.pts.empty() || std::hypot((c.x - st.pts.back().x) * dim.x, (c.y - st.pts.back().y) * dim.y) > 2.0f) st.pts.push_back(c);
        } else {
            g_p.painting = false;
        }
        // what is painted, in red over the picture (erased parts drawn back over with the picture's own pixels is too costly: a darker
        // overlay shows them instead)
        dl->PushClipRect(a, b, true);
        for (const Stroke& st : g_p.strokes) {
            const ImU32 col = st.erase ? IM_COL32(30, 30, 40, 150) : IM_COL32(255, 60, 70, 110);
            const float r = st.radius * dim.x;
            for (size_t k = 0; k < st.pts.size(); ++k) {
                const ImVec2 p(a.x + st.pts[k].x * dim.x, a.y + st.pts[k].y * dim.y);
                dl->AddCircleFilled(p, r, col, 20);
                if (k + 1 < st.pts.size()) dl->AddLine(p, ImVec2(a.x + st.pts[k + 1].x * dim.x, a.y + st.pts[k + 1].y * dim.y), col, r * 2.0f);
            }
        }
        dl->PopClipRect();
        if (hovered) dl->AddCircle(m, g_p.brush * dim.x, IM_COL32(255, 255, 255, 220), 32, 1.5f); // the brush
    } else {
        const char* text = "Drop a picture here, or pick one on the right";
        const ImVec2 ts = ImGui::CalcTextSize(text);
        dl->AddText(ImVec2(origin.x + (region.x - ts.x) * 0.5f, origin.y + (region.y - ts.y) * 0.5f), IM_COL32(220, 220, 230, 230), text);
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("MV_LIBITEM")) SetPicture(app, std::wstring(static_cast<const wchar_t*>(pl->Data)));
        else if (const ImGuiPayload* pg = ImGui::AcceptDragDropPayload("MV_IMAGES")) {
            const int* idx = static_cast<const int*>(pg->Data);
            if (pg->DataSize >= static_cast<int>(sizeof(int)) && idx[0] >= 0 && idx[0] < static_cast<int>(app.entries.size())) SetPicture(app, app.entries[idx[0]].path);
        }
        ImGui::EndDragDropTarget();
    }

    // ---- what to do
    ImGui::TableSetColumnIndex(1);
    ImGui::BeginChild("##paintctl", ImVec2(0, height));
    ui::SectionHeading("Paint and fill");
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Paint over what should change, then say what goes there - or leave it empty to take it out. Only the painted area is "
                        "redrawn (FLUX 3 Image Klein); the rest stays exactly as it is.");
    ImGui::PopTextWrapPos();
    if (ImGui::Button("Pick a picture...")) SetPicture(app, OpenImageFileDialog(nullptr));
    if (!g_p.picture.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", U8(fs::path(g_p.picture).filename().wstring()).c_str());
    }
    ImGui::Spacing();
    ImGui::SetNextItemWidth(ui::Dp(180.0f));
    float pct = g_p.brush * 100.0f;
    if (ImGui::SliderFloat("Brush##paintbrush", &pct, 0.5f, 15.0f, "%.1f%%")) g_p.brush = pct / 100.0f;
    ImGui::Checkbox("Eraser##painteraser", &g_p.eraser);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Or paint with the right mouse button to erase");
    ImGui::SameLine();
    ImGui::BeginDisabled(g_p.strokes.empty());
    if (ImGui::Button("Undo##paintundo")) g_p.strokes.pop_back();
    ImGui::SameLine();
    if (ImGui::Button("Clear##paintclear")) g_p.strokes.clear();
    ImGui::EndDisabled();
    ImGui::Spacing();
    ImGui::TextUnformatted("What goes there");
    ImGui::InputTextMultiline("##paintwords", g_p.words, sizeof(g_p.words), ImVec2(-FLT_MIN, ui::Dp(70.0f)));
    ImGui::TextDisabled("e.g. \"a black cat sleeping\", \"a wooden door\". Empty: taken out.");
    ImGui::SetNextItemWidth(ui::Dp(100.0f));
    ImGui::InputInt("times##painttimes", &g_p.times, 1, 2);
    g_p.times = std::clamp(g_p.times, 1, 8);
    ImGui::Spacing();
    const bool ready = Flux3EditReady();
    ImGui::BeginDisabled(!ready || g_p.picture.empty() || !AnyPainted());
    if (ui::PrimaryButton(g_p.words[0] ? "Fill it" : "Take it out", ImVec2(ui::Dp(160.0f), ui::Dp(36.0f)))) Make(app);
    ImGui::EndDisabled();
    if (!ready) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(ui::kWarn, "Needs FLUX 3 Image Klein in ComfyUI: flux3-image-klein-int8-convrot.safetensors in models/diffusion_models "
                                      "(black-forest-labs/flux-3-image-klein-comfy, accept its licence on Hugging Face), with the FLUX.2 VAE and Qwen3-VL 4B.");
        ImGui::PopTextWrapPos();
    } else {
        ImGui::TextDisabled("about 20 s; the result shows in Change anything's list");
    }
    if (!g_p.error.empty()) ImGui::TextColored(ui::kBad, "%s", g_p.error.c_str());
    ImGui::EndChild();
    ImGui::EndTable();
}
