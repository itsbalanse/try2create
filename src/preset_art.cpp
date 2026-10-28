// The Video page's motion-preset cards: a drawing for each idea (2026-10-28: each card had shown the last video made with it, so every card
// showed the same face and said nothing about the move). A figure in the move's two key poses -
// it swings between them while the card is hovered - with the idea's props (notes, a mic, a box, rain, an explosion...) on its own colours.
// Everything is drawn with ImGui's draw list in a 100 x 70 box, so it stays sharp at any size.
#include <algorithm>
#include <cmath>
#include <vector>

#include "imgui.h"

namespace {

struct P {
    float x, y;
};
// a stick figure: head, neck, hip, then elbow/hand left and right, knee/foot left and right
struct Pose {
    P head, neck, hip, le, lh, re, rh, lk, lf, rk, rf;
};

struct Canvas {
    ImDrawList* dl;
    ImVec2 o;
    float sx, sy;
    ImVec2 at(P p) const { return ImVec2(o.x + p.x * sx, o.y + p.y * sy); }
    float s(float v) const { return v * sx; }
};

P Mix(P a, P b, float t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }
Pose Mix(const Pose& a, const Pose& b, float t) {
    return {Mix(a.head, b.head, t), Mix(a.neck, b.neck, t), Mix(a.hip, b.hip, t), Mix(a.le, b.le, t), Mix(a.lh, b.lh, t), Mix(a.re, b.re, t),
            Mix(a.rh, b.rh, t), Mix(a.lk, b.lk, t), Mix(a.lf, b.lf, t), Mix(a.rk, b.rk, t), Mix(a.rf, b.rf, t)};
}

const ImU32 kInk = IM_COL32(250, 248, 255, 255);

void Limb(const Canvas& c, P a, P b, P d, float w, ImU32 col) {
    c.dl->AddLine(c.at(a), c.at(b), col, c.s(w));
    c.dl->AddLine(c.at(b), c.at(d), col, c.s(w));
    c.dl->AddCircleFilled(c.at(b), c.s(w * 0.5f), col);
    c.dl->AddCircleFilled(c.at(d), c.s(w * 0.5f), col);
}

void Figure(const Canvas& c, const Pose& f, ImU32 col = kInk, float w = 3.4f) {
    // the far side first, a touch dimmer, so arms and legs read in front of / behind the body
    const ImU32 back = (col & 0x00FFFFFF) | (static_cast<ImU32>(((col >> 24) & 0xFF) * 0.78f) << 24);
    Limb(c, f.hip, f.lk, f.lf, w, back);
    Limb(c, f.neck, f.le, f.lh, w * 0.85f, back);
    c.dl->AddLine(c.at(f.hip), c.at(f.neck), col, c.s(w * 1.35f));
    c.dl->AddCircleFilled(c.at(f.hip), c.s(w * 0.67f), col);
    Limb(c, f.hip, f.rk, f.rf, w, col);
    Limb(c, f.neck, f.re, f.rh, w * 0.85f, col);
    c.dl->AddCircleFilled(c.at(f.head), c.s(4.6f), col);
}

void Note(const Canvas& c, P p, ImU32 col, float k = 1.0f) { // a music note
    c.dl->AddCircleFilled(c.at(p), c.s(1.9f * k), col);
    c.dl->AddLine(c.at({p.x + 1.7f * k, p.y}), c.at({p.x + 1.7f * k, p.y - 7.0f * k}), col, c.s(1.0f));
    c.dl->AddLine(c.at({p.x + 1.7f * k, p.y - 7.0f * k}), c.at({p.x + 4.5f * k, p.y - 5.0f * k}), col, c.s(1.0f));
}

void Sparkle(const Canvas& c, P p, float r, ImU32 col) { // a four-point star
    const ImVec2 q = c.at(p);
    const float a = c.s(r), b = c.s(r * 0.28f);
    const ImVec2 pts[8] = {{q.x, q.y - a}, {q.x + b, q.y - b}, {q.x + a, q.y}, {q.x + b, q.y + b}, {q.x, q.y + a}, {q.x - b, q.y + b}, {q.x - a, q.y}, {q.x - b, q.y - b}};
    c.dl->AddConvexPolyFilled(pts, 8, col);
}

void Burst(const Canvas& c, P p, float r1, float r2, int n, ImU32 col, float spin = 0.0f) { // a spiky star (impacts, explosions)
    std::vector<ImVec2> pts;
    for (int i = 0; i < n * 2; ++i) {
        const float a = spin + i * 3.14159265f / n, r = (i % 2) ? r2 : r1;
        pts.push_back(c.at({p.x + std::cos(a) * r, p.y + std::sin(a) * r * 0.9f}));
    }
    c.dl->AddConcavePolyFilled(pts.data(), static_cast<int>(pts.size()), col);
}

void Arc(const Canvas& c, P center, float r, float a0, float a1, ImU32 col, float w, bool head) { // an arrow along an arc (radians)
    c.dl->PathArcTo(c.at(center), c.s(r), a0, a1, 24);
    c.dl->PathStroke(col, 0, c.s(w));
    if (head) {
        const P e{center.x + std::cos(a1) * r, center.y + std::sin(a1) * r};
        const float d = a1 > a0 ? 1.0f : -1.0f, tx = -std::sin(a1) * d, ty = std::cos(a1) * d;
        const float k = 3.2f;
        c.dl->AddTriangleFilled(c.at({e.x + tx * k, e.y + ty * k}), c.at({e.x - ty * k * 0.7f, e.y + tx * k * 0.7f}), c.at({e.x + ty * k * 0.7f, e.y - tx * k * 0.7f}), col);
    }
}

void Lines(const Canvas& c, P from, P dir, int n, float spread, float len, ImU32 col, float w = 1.0f) { // speed lines
    const float l = std::sqrt(dir.x * dir.x + dir.y * dir.y), ux = dir.x / l, uy = dir.y / l;
    for (int i = 0; i < n; ++i) {
        const float off = (i - (n - 1) * 0.5f) * spread;
        const P a{from.x - uy * off, from.y + ux * off};
        const float ll = len * (0.7f + 0.3f * ((i * 37) % 10) / 10.0f);
        c.dl->AddLine(c.at(a), c.at({a.x + ux * ll, a.y + uy * ll}), col, c.s(w));
    }
}

void Text(const Canvas& c, P p, const char* t, ImU32 col, float size) {
    c.dl->AddText(ImGui::GetFont(), c.s(size), c.at(p), col, t);
}

void Ground(const Canvas& c, float y, ImU32 col) { c.dl->AddLine(c.at({8, y}), c.at({92, y}), col, c.s(0.8f)); }

// the standing figure the poses start from (head at the top, feet on y = 62)
constexpr Pose kStand = {{50, 13}, {50, 20}, {50, 39}, {44, 29}, {42, 38}, {56, 29}, {58, 38}, {47, 50}, {46, 62}, {53, 50}, {54, 62}};

Pose Shift(Pose f, float dx, float dy) {
    for (P* q : {&f.head, &f.neck, &f.hip, &f.le, &f.lh, &f.re, &f.rh, &f.lk, &f.lf, &f.rk, &f.rf}) q->x += dx, q->y += dy;
    return f;
}

} // namespace

// Draws preset `k` (the order of video_ui.cpp's kPresets) into the card at p, size w x h. t: seconds since the card was hovered (< 0: still).
void DrawPresetArt(ImDrawList* dl, int k, ImVec2 p, float w, float h, float t) {
    const Canvas c{dl, p, w / 100.0f, h / 70.0f};
    const bool moving = t >= 0.0f;
    const float tt = moving ? t : 0.0f;
    const float sw = moving ? 0.5f - 0.5f * std::cos(tt * 4.2f) : 0.0f; // 0..1, back and forth
    struct Look {
        ImU32 bg, glow;
    };
    static const Look kLooks[] = {
        {IM_COL32(88, 40, 140, 255), IM_COL32(255, 80, 200, 70)},  // viral dance
        {IM_COL32(40, 40, 52, 255), IM_COL32(255, 230, 180, 60)},  // catwalk
        {IM_COL32(28, 70, 110, 255), IM_COL32(120, 200, 255, 60)}, // talk to camera
        {IM_COL32(150, 90, 20, 255), IM_COL32(255, 220, 90, 80)},  // laugh it off
        {IM_COL32(120, 30, 30, 255), IM_COL32(255, 150, 60, 70)},  // kung fu
        {IM_COL32(40, 50, 90, 255), IM_COL32(150, 190, 255, 70)},  // hero landing
        {IM_COL32(20, 22, 50, 255), IM_COL32(200, 210, 255, 50)},  // moonwalk
        {IM_COL32(60, 20, 70, 255), IM_COL32(255, 120, 220, 60)},  // mic drop
        {IM_COL32(150, 50, 110, 255), IM_COL32(255, 200, 240, 70)}, // spin & wink
        {IM_COL32(170, 90, 80, 255), IM_COL32(255, 210, 170, 80)}, // slow-mo hair flip
        {IM_COL32(200, 150, 30, 255), IM_COL32(255, 255, 200, 80)}, // shocked
        {IM_COL32(30, 100, 90, 255), IM_COL32(255, 240, 150, 80)}, // unboxing
        {IM_COL32(20, 60, 50, 255), IM_COL32(120, 255, 170, 50)},  // bullet time
        {IM_COL32(50, 25, 20, 255), IM_COL32(255, 120, 40, 110)},  // walk away
        {IM_COL32(15, 20, 45, 255), IM_COL32(170, 140, 255, 60)},  // zero gravity
        {IM_COL32(30, 80, 150, 255), IM_COL32(255, 255, 255, 60)}, // superhero takeoff
        {IM_COL32(120, 40, 70, 255), IM_COL32(255, 210, 120, 80)}, // glow up
        {IM_COL32(20, 30, 50, 255), IM_COL32(255, 220, 140, 60)},  // rain dance
    };
    const Look& look = kLooks[std::clamp(k, 0, static_cast<int>(sizeof(kLooks) / sizeof(kLooks[0])) - 1)];
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), look.bg, w * 0.065f);
    dl->PushClipRect(p, ImVec2(p.x + w, p.y + h), true);
    dl->AddCircleFilled(c.at({50, 34}), c.s(34), look.glow, 40);
    dl->AddCircleFilled(c.at({50, 34}), c.s(20), look.glow, 40);
    const ImU32 dim = IM_COL32(255, 255, 255, 70), accent = IM_COL32(255, 220, 90, 255);
    switch (k) {
    case 0: { // Viral dance: arms up, hips swinging, notes floating up
        const Pose a = {{46, 13}, {48, 20}, {51, 39}, {40, 17}, {37, 8}, {57, 25}, {63, 18}, {45, 50}, {42, 62}, {57, 49}, {60, 60}};
        const Pose b = {{54, 13}, {52, 20}, {49, 39}, {43, 25}, {37, 18}, {60, 17}, {63, 8}, {43, 49}, {40, 60}, {55, 50}, {58, 62}};
        Figure(c, Mix(a, b, sw));
        for (int i = 0; i < 3; ++i) {
            const float f = std::fmod(tt * 0.6f + i * 0.33f, 1.0f);
            Note(c, {18.0f + i * 32.0f + (i == 1 ? 14.0f : 0.0f), 40.0f - f * 26.0f}, IM_COL32(255, 150, 230, static_cast<int>(230 * (1.0f - f * 0.6f))));
        }
        break;
    }
    case 1: { // Catwalk: a runway in perspective, the figure striding toward you
        dl->AddQuadFilled(c.at({40, 30}), c.at({60, 30}), c.at({84, 70}), c.at({16, 70}), IM_COL32(255, 255, 255, 30));
        for (float x : {20.0f, 80.0f}) dl->AddCircleFilled(c.at({x, 20}), c.s(2.0f), IM_COL32(255, 240, 200, 200));
        const Pose a = {{50, 12}, {50, 19}, {50, 38}, {45, 28}, {46, 37}, {55, 28}, {57, 36}, {47, 50}, {50, 62}, {54, 49}, {52, 61}};
        const Pose b = {{51, 12}, {51, 19}, {51, 38}, {46, 28}, {44, 36}, {56, 28}, {55, 37}, {48, 49}, {49, 61}, {55, 50}, {52, 62}};
        Figure(c, Shift(Mix(a, b, sw), 0, 0), kInk, 3.0f);
        break;
    }
    case 2: { // Talk to camera: close, waving a hand, a speech bubble
        const Pose f = {{44, 24}, {44, 33}, {44, 66}, {34, 48}, {38, 58}, {56, 44}, {60.0f + sw * 4.0f, 33.0f - sw * 3.0f}, {42, 70}, {42, 80}, {46, 70}, {46, 80}};
        Figure(c, f, kInk, 4.6f);
        dl->AddRectFilled(c.at({62, 8}), c.at({92, 24}), IM_COL32(255, 255, 255, 235), c.s(4.0f));
        dl->AddTriangleFilled(c.at({66, 23}), c.at({72, 23}), c.at({60, 30}), IM_COL32(255, 255, 255, 235));
        for (int i = 0; i < 3; ++i) dl->AddCircleFilled(c.at({70.0f + i * 7.0f, 16}), c.s(1.6f), IM_COL32(40, 70, 110, moving && (static_cast<int>(tt * 3) % 3) < i ? 80 : 255));
        break;
    }
    case 3: { // Laugh it off: doubled over, HA HA
        const Pose a = {{41, 24}, {44, 28}, {52, 40}, {44, 36}, {48, 42}, {50, 34}, {54, 41}, {49, 51}, {47, 62}, {56, 51}, {57, 62}};
        const Pose b = {{40, 27}, {43, 31}, {52, 40}, {44, 38}, {47, 44}, {50, 36}, {54, 43}, {49, 51}, {47, 62}, {56, 51}, {57, 62}};
        Figure(c, Mix(a, b, sw));
        Text(c, {14.0f, 8.0f - sw * 2.0f}, "HA", accent, 13.0f);
        Text(c, {66.0f, 14.0f + sw * 2.0f}, "HA!", accent, 11.0f);
        break;
    }
    case 4: { // Kung fu: a high kick, an impact
        const Pose a = {{40, 15}, {42, 22}, {46, 39}, {36, 26}, {30, 22}, {48, 28}, {44, 33}, {44, 50}, {44, 62}, {58, 33}, {72, 24}};
        const Pose b = {{40, 16}, {42, 23}, {46, 39}, {36, 27}, {30, 24}, {48, 29}, {44, 34}, {44, 50}, {44, 62}, {57, 41}, {68, 38}};
        Figure(c, Mix(b, a, sw));
        Burst(c, {77, 22}, 7.0f + sw * 2.0f, 3.0f, 7, IM_COL32(255, 190, 70, 230), tt);
        Arc(c, {52, 39}, 22, -1.9f, -0.5f, dim, 1.2f, false);
        break;
    }
    case 5: { // Hero landing: one knee down, a fist on the cracked ground
        const Pose f = {{43, 30}, {45, 36}, {54, 47}, {41, 44}, {38, 61}, {54, 42}, {62, 46}, {44, 54}, {44, 62}, {63, 56}, {70, 62}};
        Figure(c, Shift(f, 0, sw * -1.5f));
        const ImU32 crack = IM_COL32(200, 220, 255, 220);
        for (P e : {P{20, 66}, P{30, 69}, P{52, 68}, P{60, 69}})
            dl->AddLine(c.at({38, 62}), c.at(e), crack, c.s(1.0f));
        dl->AddLine(c.at({8, 62}), c.at({92, 62}), crack, c.s(0.8f));
        for (int i = 0; i < 5; ++i) dl->AddCircleFilled(c.at({20.0f + i * 14.0f, 58.0f - (i % 2) * 4.0f - sw * 3.0f}), c.s(2.0f + (i % 3)), IM_COL32(220, 220, 240, 60));
        break;
    }
    case 6: { // Moonwalk: leaning, gliding backwards under the moon
        dl->AddCircleFilled(c.at({80, 14}), c.s(7.0f), IM_COL32(240, 240, 220, 230));
        dl->AddCircleFilled(c.at({83, 12}), c.s(6.0f), look.bg);
        const float x = moving ? -std::fmod(tt * 6.0f, 18.0f) + 8.0f : 0.0f;
        const Pose a = {{46, 13}, {47, 20}, {50, 39}, {42, 29}, {40, 37}, {54, 29}, {56, 36}, {47, 51}, {44, 62}, {55, 51}, {60, 61}};
        const Pose b = {{46, 13}, {47, 20}, {50, 39}, {42, 29}, {40, 37}, {54, 29}, {56, 36}, {49, 51}, {50, 61}, {53, 51}, {52, 62}};
        Figure(c, Shift(Mix(a, b, sw), x, 0));
        Lines(c, {64.0f + x, 60}, {1, 0}, 3, 2.0f, 10.0f, dim);
        Ground(c, 62.5f, dim);
        break;
    }
    case 7: { // Mic drop: the arm out, the mic falling
        const Pose f = {{44, 13}, {45, 20}, {47, 39}, {40, 29}, {38, 38}, {54, 24}, {64, 26}, {45, 50}, {44, 62}, {52, 50}, {54, 62}};
        Figure(c, f);
        const float fall = sw * 20.0f;
        const P m{66.0f, 32.0f + fall};
        dl->AddLine(c.at(m), c.at({m.x + 1.5f, m.y + 9}), IM_COL32(200, 200, 210, 255), c.s(2.0f));
        dl->AddCircleFilled(c.at(m), c.s(3.0f), IM_COL32(170, 170, 185, 255));
        Lines(c, {m.x, m.y - 4.0f}, {0, -1}, 3, 2.0f, 6.0f, dim);
        break;
    }
    case 8: { // Spin & wink: a circular arrow, a sparkle by the face
        const float lean = std::sin(tt * 4.2f) * 3.0f;
        Figure(c, Shift(kStand, lean * 0.3f, 0));
        Arc(c, {50, 42}, 26, 0.4f, 2.7f + (moving ? std::fmod(tt * 2.0f, 1.0f) * 0.6f : 0.0f), IM_COL32(255, 200, 240, 230), 1.6f, true);
        Sparkle(c, {62, 10}, 4.0f + sw * 2.0f, IM_COL32(255, 255, 255, 240));
        break;
    }
    case 9: { // Slow-mo hair flip: a big hair arc, wind
        Figure(c, kStand);
        const float flip = sw;
        for (int i = 0; i < 4; ++i) {
            const P a{50, 10}, b{36.0f - i * 3.0f, 4.0f + flip * 8.0f + i * 2.0f}, d{26.0f - i * 2.0f, 16.0f + flip * 10.0f + i * 3.0f};
            dl->AddBezierQuadratic(c.at(a), c.at(b), c.at(d), IM_COL32(120, 60, 30, 255), c.s(2.2f));
        }
        Lines(c, {68, 14}, {1, 0.15f}, 4, 3.0f, 16.0f, dim);
        break;
    }
    case 10: { // Shocked: hands to the face, an open mouth, "!"
        const Pose f = {{50, 15}, {50, 22}, {50, 40}, {42, 28}, {46, 16}, {58, 28}, {54, 16}, {47, 51}, {45, 62}, {53, 51}, {55, 62}};
        Figure(c, Shift(f, 0, -sw * 1.5f));
        dl->AddCircleFilled(c.at({50, 16.5f}), c.s(1.6f), look.bg);
        Text(c, {68.0f, 4.0f - sw * 2.0f}, "!", IM_COL32(255, 80, 60, 255), 20.0f);
        Text(c, {76.0f, 8.0f - sw * 3.0f}, "!", IM_COL32(255, 80, 60, 255), 14.0f);
        break;
    }
    case 11: { // Unboxing: a box held up, light pouring out
        const float lift = sw * 4.0f;
        const Pose f = {{50, 14}, {50, 21}, {50, 40}, {43, 28}, {41.0f, 20.0f - lift}, {57, 28}, {59.0f, 20.0f - lift}, {47, 51}, {46, 62}, {53, 51}, {54, 62}};
        for (int i = 0; i < 7; ++i) {
            const float a = -2.6f + i * 0.35f;
            dl->AddLine(c.at({50, 14 - lift}), c.at({50 + std::cos(a) * 30, 14 - lift + std::sin(a) * 30}), IM_COL32(255, 240, 150, 90), c.s(2.0f));
        }
        Figure(c, f);
        dl->AddRectFilled(c.at({40, 14 - lift}), c.at({60, 26 - lift}), IM_COL32(200, 140, 70, 255), c.s(1.0f));
        dl->AddLine(c.at({50, 14 - lift}), c.at({50, 26 - lift}), IM_COL32(230, 60, 60, 255), c.s(2.0f));
        break;
    }
    case 12: { // Bullet time: frozen mid-leap, the camera sweeping round, a bullet trail
        const Pose f = {{48, 12}, {48, 19}, {50, 36}, {40, 23}, {33, 20}, {58, 24}, {64, 19}, {44, 44}, {38, 52}, {58, 45}, {60, 55}};
        Figure(c, f);
        Arc(c, {50, 36}, 34, 3.6f, 5.8f, IM_COL32(120, 255, 170, 200), 1.2f, true);
        const float a = 3.6f + (moving ? std::fmod(tt * 0.9f, 1.0f) : 0.3f) * 2.2f;
        const P cam{50 + std::cos(a) * 34, 36 + std::sin(a) * 34 * 0.9f};
        dl->AddRectFilled(c.at({cam.x - 3.5f, cam.y - 2.5f}), c.at({cam.x + 3.5f, cam.y + 2.5f}), IM_COL32(240, 240, 240, 255), c.s(0.8f));
        for (int i = 0; i < 4; ++i) dl->AddCircle(c.at({14.0f + i * 5.0f, 50}), c.s(1.6f - i * 0.25f), IM_COL32(200, 255, 220, 200 - i * 40), 12, c.s(0.7f));
        break;
    }
    case 13: { // Walk away: walking toward you, an explosion behind
        Burst(c, {50, 32}, 30.0f + sw * 4.0f, 18.0f, 11, IM_COL32(255, 110, 30, 230), tt * 0.5f);
        Burst(c, {50, 32}, 20.0f + sw * 3.0f, 12.0f, 9, IM_COL32(255, 200, 60, 240), -tt * 0.4f);
        const Pose a = {{50, 14}, {50, 21}, {50, 40}, {45, 30}, {47, 39}, {55, 30}, {56, 38}, {47, 51}, {48, 62}, {54, 51}, {52, 62}};
        const Pose b = {{50, 14}, {50, 21}, {50, 40}, {45, 30}, {44, 38}, {55, 30}, {57, 38}, {48, 51}, {47, 62}, {53, 51}, {54, 62}};
        Figure(c, Mix(a, b, sw), IM_COL32(20, 14, 12, 255), 3.6f);
        break;
    }
    case 14: { // Zero gravity: tilted, floating up, drifting bits
        const float up = moving ? std::sin(tt * 2.0f) * 4.0f : 0.0f;
        const Pose f = {{56, 14}, {54, 20}, {47, 36}, {46, 22}, {40, 16}, {61, 24}, {68, 20}, {42, 44}, {36, 50}, {52, 46}, {54, 54}};
        Figure(c, Shift(f, 0, -up));
        for (int i = 0; i < 7; ++i) {
            const float y = std::fmod(60.0f - tt * 8.0f - i * 9.0f + 200.0f, 64.0f) + 3.0f;
            dl->AddCircleFilled(c.at({12.0f + i * 13.0f, y}), c.s(1.0f + (i % 3) * 0.5f), IM_COL32(220, 210, 255, 160));
        }
        Ground(c, 64.0f, dim);
        break;
    }
    case 15: { // Superhero takeoff: a fist up, speed lines, a dust ring below
        const float up = sw * 10.0f;
        const Pose f = {{50, 18}, {50, 25}, {50, 42}, {54, 16}, {55, 8}, {44, 31}, {42, 39}, {47, 52}, {47, 60}, {53, 52}, {53, 60}};
        Lines(c, {50, 60 - up}, {0, 1}, 5, 3.0f, 10.0f, IM_COL32(255, 255, 255, 120));
        Figure(c, Shift(f, 0, -up));
        dl->AddEllipse(c.at({50, 64}), ImVec2(c.s(18.0f + sw * 10.0f), c.s(3.0f + sw)), IM_COL32(230, 230, 255, 140), 0, 24, c.s(1.4f));
        break;
    }
    case 16: { // Glow up: a gown, sparkles everywhere
        Figure(c, kStand);
        dl->AddTriangleFilled(c.at({50, 26}), c.at({38, 60}), c.at({62, 60}), IM_COL32(220, 40, 70, 255));
        dl->AddCircleFilled(c.at(kStand.head), c.s(4.6f), kInk);
        for (int i = 0; i < 6; ++i) {
            const float ph = tt * 3.0f + i * 1.7f;
            const float r = moving ? 2.0f + 2.0f * (0.5f + 0.5f * std::sin(ph)) : 2.6f;
            Sparkle(c, {16.0f + (i * 29) % 70, 10.0f + (i * 17) % 44}, r, IM_COL32(255, 230, 150, 240));
        }
        break;
    }
    case 17: { // Rain dance: arms out under a street light, rain, a puddle
        dl->AddLine(c.at({82, 64}), c.at({82, 10}), IM_COL32(150, 150, 160, 255), c.s(1.4f));
        dl->AddLine(c.at({82, 10}), c.at({74, 10}), IM_COL32(150, 150, 160, 255), c.s(1.4f));
        dl->AddTriangleFilled(c.at({74, 10}), c.at({62, 60}), c.at({86, 60}), IM_COL32(255, 220, 140, 35));
        dl->AddEllipseFilled(c.at({48, 64}), ImVec2(c.s(16.0f), c.s(2.6f)), IM_COL32(120, 160, 220, 120));
        const Pose a = {{48, 14}, {48, 21}, {48, 40}, {40, 22}, {32, 18}, {56, 22}, {64, 18}, {45, 51}, {43, 62}, {52, 50}, {56, 58}};
        const Pose b = {{48, 14}, {48, 21}, {48, 40}, {40, 23}, {32, 26}, {56, 23}, {64, 26}, {44, 50}, {40, 58}, {51, 51}, {53, 62}};
        Figure(c, Mix(a, b, sw));
        for (int i = 0; i < 14; ++i) {
            const float x = 6.0f + (i * 41) % 90, y = std::fmod((i * 23) % 60 + tt * 60.0f, 66.0f);
            dl->AddLine(c.at({x, y}), c.at({x - 1.0f, y + 5.0f}), IM_COL32(170, 200, 255, 170), c.s(0.8f));
        }
        break;
    }
    default:
        Figure(c, kStand);
    }
    dl->PopClipRect();
}

namespace {

void Frame(const Canvas& c, P a, P b, ImU32 sky, ImU32 hill, bool sun, float t) { // a little landscape picture (the thing being edited)
    c.dl->AddRectFilled(c.at(a), c.at(b), sky, c.s(1.5f));
    const float w = b.x - a.x, h = b.y - a.y;
    c.dl->AddTriangleFilled(c.at({a.x + w * 0.05f, b.y}), c.at({a.x + w * 0.38f, a.y + h * 0.42f}), c.at({a.x + w * 0.7f, b.y}), hill);
    c.dl->AddTriangleFilled(c.at({a.x + w * 0.4f, b.y}), c.at({a.x + w * 0.7f, a.y + h * 0.55f}), c.at({b.x, b.y}), hill);
    if (sun) c.dl->AddCircleFilled(c.at({a.x + w * 0.78f, a.y + h * 0.26f}), c.s(h * 0.12f), IM_COL32(255, 220, 90, 255));
    else {
        c.dl->AddCircleFilled(c.at({a.x + w * 0.78f, a.y + h * 0.26f}), c.s(h * 0.12f), IM_COL32(235, 235, 220, 255));
        c.dl->AddCircleFilled(c.at({a.x + w * 0.81f, a.y + h * 0.23f}), c.s(h * 0.11f), sky);
        for (int i = 0; i < 5; ++i)
            c.dl->AddCircleFilled(c.at({a.x + w * (0.12f + i * 0.13f), a.y + h * (0.12f + (i % 2) * 0.12f)}), c.s(0.5f + 0.3f * std::sin(t * 5.0f + i)), IM_COL32(255, 255, 255, 220));
    }
    c.dl->AddRect(c.at(a), c.at(b), IM_COL32(255, 255, 255, 200), c.s(1.5f), 0, c.s(0.8f));
}

void Arrow(const Canvas& c, P a, P b, ImU32 col) {
    c.dl->AddLine(c.at(a), c.at(b), col, c.s(1.4f));
    const float dx = b.x - a.x, dy = b.y - a.y, l = std::sqrt(dx * dx + dy * dy), ux = dx / l, uy = dy / l;
    c.dl->AddTriangleFilled(c.at({b.x + ux * 2.5f, b.y + uy * 2.5f}), c.at({b.x - uy * 2.0f, b.y + ux * 2.0f}), c.at({b.x + uy * 2.0f, b.y - ux * 2.0f}), col);
}

} // namespace

// The Edits start screen's cards (edits_ui.cpp DrawEditsHome): a drawing of each kind of edit in a w x h box (laid out on 100 x 50). `mode` is
// the Edits mode (0 Qwen, 1 MOSS, 2 PhotoCraft, 3 upscale, 4 apps, 5 videos); t: seconds since hovered (< 0: still).
void DrawEditsArt(ImDrawList* dl, int mode, ImVec2 p, float w, float h, float t) {
    const Canvas c{dl, p, w / 100.0f, h / 50.0f};
    const bool moving = t >= 0.0f;
    const float tt = moving ? t : 0.0f;
    static const ImU32 kBg[] = {IM_COL32(40, 50, 110, 255), IM_COL32(110, 40, 60, 255), IM_COL32(30, 90, 80, 255),
                                IM_COL32(40, 70, 120, 255), IM_COL32(90, 40, 130, 255), IM_COL32(25, 25, 35, 255)};
    const float r = std::min(w, h) * 0.12f;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), kBg[std::clamp(mode, 0, 5)], r, ImDrawFlags_RoundCornersTop);
    dl->PushClipRect(p, ImVec2(p.x + w, p.y + h), true);
    dl->AddCircleFilled(c.at({50, 25}), c.s(30), IM_COL32(255, 255, 255, 18), 40);
    const ImU32 white = IM_COL32(250, 250, 255, 255), dim = IM_COL32(255, 255, 255, 110);
    switch (mode) {
    case 0: { // change anything: a day picture, a wand, the same picture at night
        Frame(c, {12, 12}, {42, 38}, IM_COL32(120, 190, 255, 255), IM_COL32(60, 140, 80, 255), true, tt);
        Frame(c, {58, 12}, {88, 38}, IM_COL32(20, 30, 70, 255), IM_COL32(30, 60, 50, 255), false, tt);
        Arrow(c, {45, 25}, {53, 25}, white);
        const float k = moving ? 0.5f + 0.5f * std::sin(tt * 6.0f) : 0.6f;
        Sparkle(c, {49, 15}, 2.0f + k * 2.0f, IM_COL32(255, 240, 160, 255));
        Sparkle(c, {54, 40}, 1.5f + (1.0f - k) * 1.5f, IM_COL32(255, 240, 160, 230));
        break;
    }
    case 1: { // designs & exact text: a poster with big words, a dashed box being redone
        dl->AddRectFilled(c.at({34, 5}), c.at({66, 46}), IM_COL32(245, 235, 220, 255), c.s(1.0f));
        dl->AddRectFilled(c.at({37, 8}), c.at({63, 28}), IM_COL32(220, 90, 70, 255), c.s(0.8f));
        Text(c, {39, 9}, "TITLE", white, 7.5f);
        for (int i = 0; i < 3; ++i) dl->AddRectFilled(c.at({38, 31.0f + i * 4.0f}), c.at({62.0f - i * 6.0f, 33.0f + i * 4.0f}), IM_COL32(60, 50, 50, 200));
        const float m = moving ? std::fmod(tt * 8.0f, 4.0f) : 0.0f; // marching ants
        for (float x = 18.0f + m; x < 50.0f; x += 4.0f) dl->AddLine(c.at({x, 20}), c.at({std::min(x + 2.0f, 50.0f), 20}), IM_COL32(255, 230, 90, 255), c.s(0.8f));
        for (float x = 18.0f + m; x < 50.0f; x += 4.0f) dl->AddLine(c.at({x, 36}), c.at({std::min(x + 2.0f, 50.0f), 36}), IM_COL32(255, 230, 90, 255), c.s(0.8f));
        dl->AddLine(c.at({18, 20}), c.at({18, 36}), IM_COL32(255, 230, 90, 255), c.s(0.8f));
        Text(c, {70, 12}, "Aa", white, 14.0f);
        break;
    }
    case 2: { // text over a picture: a meme with a speech bubble and a caption
        Frame(c, {22, 6}, {78, 44}, IM_COL32(120, 190, 255, 255), IM_COL32(60, 140, 80, 255), true, tt);
        dl->AddRectFilled(c.at({26, 9}), c.at({56, 19}), IM_COL32(255, 255, 255, 245), c.s(3.0f));
        dl->AddTriangleFilled(c.at({34, 18.5f}), c.at({40, 18.5f}), c.at({36, 24}), IM_COL32(255, 255, 255, 245));
        Text(c, {29, 10}, moving && static_cast<int>(tt * 2) % 2 ? "OMG" : "LOL", IM_COL32(30, 30, 30, 255), 7.0f);
        dl->AddRectFilled(c.at({22, 36}), c.at({78, 44}), IM_COL32(0, 0, 0, 150));
        Text(c, {33, 36.5f}, "WHEN IT WORKS", white, 6.0f);
        break;
    }
    case 3: { // upscale: a small blocky picture to a big sharp one
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x)
                dl->AddRectFilled(c.at({14.0f + x * 4.0f, 20.0f + y * 4.0f}), c.at({18.0f + x * 4.0f, 24.0f + y * 4.0f}),
                                  IM_COL32(80 + x * 30, 120 + y * 25, 200 - x * 20, 255));
        }
        const float grow = moving ? 0.5f + 0.5f * std::sin(tt * 3.0f) : 1.0f;
        Arrow(c, {34, 28}, {44, 28}, white);
        const float s = 15.0f + grow * 3.0f;
        Frame(c, {68 - s, 25 - s * 1.2f}, {68 + s, 25 + s * 1.2f}, IM_COL32(120, 190, 255, 255), IM_COL32(60, 140, 80, 255), true, tt);
        Text(c, {14, 8}, "2x", IM_COL32(255, 230, 90, 255), 9.0f);
        break;
    }
    case 4: { // apps: a grid of recipes, one lighting up after another
        const char* labels[6] = {"Relight", "Angle", "Plushie", "Product", "Headshot", "Outfit"};
        const int lit = moving ? static_cast<int>(tt * 2.5f) % 6 : 0;
        for (int i = 0; i < 6; ++i) {
            const float x = 13.0f + (i % 3) * 26.0f, y = 6.0f + (i / 3) * 21.0f;
            dl->AddRectFilled(c.at({x, y}), c.at({x + 22, y + 17}), i == lit ? IM_COL32(255, 210, 90, 255) : IM_COL32(255, 255, 255, 40), c.s(2.0f));
            Text(c, {x + 2.0f, y + 5.0f}, labels[i], i == lit ? IM_COL32(40, 30, 20, 255) : white, 5.6f);
        }
        break;
    }
    case 5: { // videos: a film strip, the frames sharpening, a 2K badge
        dl->AddRectFilled(c.at({6, 12}), c.at({94, 38}), IM_COL32(15, 15, 18, 255));
        const float scroll = moving ? std::fmod(tt * 10.0f, 22.0f) : 0.0f;
        for (float x = 6.0f - scroll; x < 96.0f; x += 5.5f) {
            dl->AddRectFilled(c.at({x, 13.5f}), c.at({x + 2.5f, 15.5f}), dim);
            dl->AddRectFilled(c.at({x, 34.5f}), c.at({x + 2.5f, 36.5f}), dim);
        }
        for (int i = 0; i < 5; ++i) {
            const float x = 4.0f + i * 22.0f - scroll;
            Frame(c, {x, 17}, {x + 18, 33}, IM_COL32(120, 190, 255, 255), IM_COL32(60, 140, 80, 255), true, tt);
        }
        dl->AddRectFilled(c.at({70, 3}), c.at({92, 11}), IM_COL32(255, 210, 90, 255), c.s(2.0f));
        Text(c, {75, 3.5f}, "2K", IM_COL32(40, 30, 20, 255), 7.0f);
        Sparkle(c, {16, 42}, 2.5f, IM_COL32(255, 240, 160, 230));
        break;
    }
    default:
        break;
    }
    dl->PopClipRect();
}
