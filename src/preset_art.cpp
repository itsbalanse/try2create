// The Video page's motion-preset cards: a drawing for each idea (2026-10-28: each card had shown the last video made with it, so every card
// showed the same face and said nothing about the move). A figure in the move's two key poses -
// it swings between them while the card is hovered - with the idea's props (notes, a mic, a box, rain, an explosion...) on its own colours.
// Everything is drawn with ImGui's draw list in a 100 x 70 box, so it stays sharp at any size.
#include <algorithm>
#include <cmath>
#include <vector>

#include "imgui.h"
#include "ui_theme.h"

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
    // the fonts are rasterized for one exact size, and came apart when drawn much larger: the smallest one big enough, drawn smaller, stays clean
    const float px = c.s(size);
    ImFont* font = ImGui::GetFont();
    for (ImFont* bigger : {ui::TitleFont(), ui::DisplayFont()})
        if (font->FontSize < px && bigger) font = bigger;
    c.dl->AddText(font, px, c.at(p), col, t);
}

void Ground(const Canvas& c, float y, ImU32 col) { c.dl->AddLine(c.at({8, y}), c.at({92, y}), col, c.s(0.8f)); }

// the standing figure the poses start from (head at the top, feet on y = 62)
constexpr Pose kStand = {{50, 13}, {50, 20}, {50, 39}, {44, 29}, {42, 38}, {56, 29}, {58, 38}, {47, 50}, {46, 62}, {53, 50}, {54, 62}};

Pose Shift(Pose f, float dx, float dy) {
    for (P* q : {&f.head, &f.neck, &f.hip, &f.le, &f.lh, &f.re, &f.rh, &f.lk, &f.lf, &f.rk, &f.rf}) q->x += dx, q->y += dy;
    return f;
}

// smaller (k < 1) around the point o (a figure far away)
Pose Scale(Pose f, P o, float k) {
    for (P* q : {&f.head, &f.neck, &f.hip, &f.le, &f.lh, &f.re, &f.rh, &f.lk, &f.lf, &f.rk, &f.rf}) q->x = o.x + (q->x - o.x) * k, q->y = o.y + (q->y - o.y) * k;
    return f;
}

Pose At(float x, float k = 1.0f) { return Shift(Scale(kStand, {50, 62}, k), x - 50.0f, 0.0f); } // a standing figure at x, k its size

void Bar(const Canvas& c, float y0, float y1, ImU32 col) { c.dl->AddRectFilled(c.at({0, y0}), c.at({100, y1}), col); } // a band across the picture

void Bottle(const Canvas& c, P b, float k, ImU32 col) { // "the product": a bottle standing on b, 31k tall
    c.dl->AddRectFilled(c.at({b.x - 6 * k, b.y - 22 * k}), c.at({b.x + 6 * k, b.y}), col, c.s(2.0f * k));
    c.dl->AddRectFilled(c.at({b.x - 2.5f * k, b.y - 28 * k}), c.at({b.x + 2.5f * k, b.y - 21 * k}), col);
    c.dl->AddRectFilled(c.at({b.x - 3.2f * k, b.y - 31 * k}), c.at({b.x + 3.2f * k, b.y - 27.5f * k}), IM_COL32(40, 40, 50, 255), c.s(0.8f * k));
    c.dl->AddRectFilled(c.at({b.x - 6 * k, b.y - 15 * k}), c.at({b.x + 6 * k, b.y - 7 * k}), IM_COL32(255, 255, 255, 220));
}

void Box(const Canvas& c, P m, float r, float a, ImU32 col) { // a square box, half-size r, turned by a
    ImVec2 q[4];
    for (int i = 0; i < 4; ++i) {
        const float b = a + 0.785398f + i * 1.570796f;
        q[i] = c.at({m.x + std::cos(b) * r * 1.414f, m.y + std::sin(b) * r * 1.414f});
    }
    c.dl->AddConvexPolyFilled(q, 4, col);
}

void Cloud(const Canvas& c, P m, float k, ImU32 col) {
    for (P d : {P{-8, 0}, P{0, -4}, P{8, 0}, P{-3, 2}, P{4, 2}}) c.dl->AddCircleFilled(c.at({m.x + d.x * k, m.y + d.y * k}), c.s(6.0f * k), col);
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
        {IM_COL32(110, 30, 120, 255), IM_COL32(255, 180, 240, 70)}, // pop idol
        {IM_COL32(25, 20, 40, 255), IM_COL32(180, 120, 255, 60)},  // rap video
        {IM_COL32(150, 70, 40, 255), IM_COL32(255, 200, 120, 70)}, // mukbang
        {IM_COL32(60, 40, 30, 255), IM_COL32(255, 190, 130, 60)},  // asmr whisper
        {IM_COL32(45, 30, 60, 255), IM_COL32(255, 200, 150, 50)},  // asmr studio
        {IM_COL32(30, 30, 36, 255), IM_COL32(255, 255, 255, 30)},  // behind the scenes
        {IM_COL32(40, 90, 140, 255), IM_COL32(255, 255, 255, 40)}, // melting doodles
        {IM_COL32(30, 36, 50, 255), IM_COL32(150, 170, 220, 40)},  // storm giant
        {IM_COL32(50, 120, 200, 255), IM_COL32(255, 255, 255, 50)}, // roller coaster
        {IM_COL32(180, 90, 140, 255), IM_COL32(255, 200, 140, 90)}, // cloud surf
        {IM_COL32(70, 60, 80, 255), IM_COL32(255, 230, 240, 60)},  // asmr product
        {IM_COL32(40, 20, 90, 255), IM_COL32(200, 150, 255, 80)},  // magic button
        {IM_COL32(200, 120, 30, 255), IM_COL32(255, 240, 180, 70)}, // kick ad
        {IM_COL32(60, 130, 170, 255), IM_COL32(255, 255, 255, 50)}, // toy truck ad
        {IM_COL32(40, 15, 15, 255), IM_COL32(255, 90, 30, 90)},    // volcano ad
        {IM_COL32(24, 28, 54, 255), IM_COL32(170, 200, 255, 70)},  // origin story
        {IM_COL32(16, 14, 18, 255), IM_COL32(255, 130, 50, 70)},   // movie trailer
        {IM_COL32(70, 50, 80, 255), IM_COL32(255, 190, 200, 60)},  // confession
        {IM_COL32(170, 110, 40, 255), IM_COL32(255, 230, 170, 70)}, // story time
        {IM_COL32(150, 80, 120, 255), IM_COL32(255, 220, 230, 70)}, // get ready
        {IM_COL32(60, 30, 130, 255), IM_COL32(120, 220, 255, 70)}, // full routine
        {IM_COL32(90, 25, 40, 255), IM_COL32(255, 170, 80, 70)},   // fight scene
        {IM_COL32(35, 40, 60, 255), IM_COL32(255, 200, 120, 50)},  // chase
        {IM_COL32(170, 60, 30, 255), IM_COL32(255, 200, 90, 80)},  // cooking show
        {IM_COL32(35, 25, 80, 255), IM_COL32(210, 150, 255, 80)},  // magic trick
        {IM_COL32(40, 90, 130, 255), IM_COL32(255, 230, 170, 60)}, // time passes
        {IM_COL32(90, 50, 130, 255), IM_COL32(255, 220, 130, 90)}, // transformation
        {IM_COL32(25, 20, 60, 255), IM_COL32(255, 120, 200, 70)},  // concert finale
        {IM_COL32(18, 22, 30, 255), IM_COL32(200, 225, 255, 60)},  // product story
        {IM_COL32(170, 70, 90, 255), IM_COL32(255, 210, 190, 80)}, // group hug
        {IM_COL32(40, 120, 170, 255), IM_COL32(255, 245, 170, 80)}, // high five
        {IM_COL32(90, 60, 160, 255), IM_COL32(255, 255, 255, 60)}, // group selfie
        {IM_COL32(120, 30, 110, 255), IM_COL32(120, 220, 255, 70)}, // duet dance
        {IM_COL32(150, 90, 30, 255), IM_COL32(255, 225, 130, 80)}, // cheers
        {IM_COL32(200, 120, 40, 255), IM_COL32(255, 245, 190, 90)}, // surprise
        {IM_COL32(30, 36, 60, 255), IM_COL32(255, 170, 90, 60)},   // squad walk
        {IM_COL32(100, 120, 190, 255), IM_COL32(255, 255, 255, 60)}, // pillow fight
        {IM_COL32(150, 30, 120, 255), IM_COL32(255, 200, 120, 70)}, // group routine
        {IM_COL32(35, 30, 60, 255), IM_COL32(120, 255, 200, 60)},  // dance battle
        {IM_COL32(150, 50, 40, 255), IM_COL32(255, 220, 120, 70)}, // argue and laugh
        {IM_COL32(60, 40, 100, 255), IM_COL32(255, 190, 220, 70)}, // slow dance
        {IM_COL32(60, 130, 130, 255), IM_COL32(255, 235, 180, 80)}, // reunion
        {IM_COL32(120, 110, 90, 255), IM_COL32(255, 255, 240, 70)}, // photo gone wrong
        {IM_COL32(30, 100, 60, 255), IM_COL32(255, 240, 150, 70)}, // team huddle
        {IM_COL32(40, 25, 30, 255), IM_COL32(255, 110, 60, 70)},   // band jam
        {IM_COL32(24, 50, 110, 255), IM_COL32(120, 180, 255, 60)}, // breaking news
        {IM_COL32(40, 110, 170, 255), IM_COL32(255, 255, 255, 50)}, // weather report
        {IM_COL32(25, 30, 70, 255), IM_COL32(255, 200, 120, 60)},  // talk show
        {IM_COL32(190, 130, 70, 255), IM_COL32(255, 240, 200, 70)}, // sitcom
        {IM_COL32(120, 125, 120, 255), IM_COL32(255, 255, 255, 40)}, // mockumentary
        {IM_COL32(110, 30, 60, 255), IM_COL32(255, 180, 200, 60)}, // soap opera
        {IM_COL32(30, 120, 120, 255), IM_COL32(220, 255, 250, 60)}, // hospital drama
        {IM_COL32(90, 55, 30, 255), IM_COL32(255, 215, 150, 60)},  // courtroom
        {IM_COL32(70, 70, 110, 255), IM_COL32(255, 200, 230, 60)}, // teen drama
        {IM_COL32(90, 60, 40, 255), IM_COL32(255, 225, 150, 80)},  // period drama
        {IM_COL32(20, 22, 28, 255), IM_COL32(255, 240, 180, 60)},  // interrogation
        {IM_COL32(18, 20, 40, 255), IM_COL32(90, 130, 255, 70)},   // crime scene
        {IM_COL32(16, 16, 22, 255), IM_COL32(200, 200, 220, 40)},  // true crime
        {IM_COL32(20, 40, 45, 255), IM_COL32(255, 200, 110, 50)},  // spy thriller
        {IM_COL32(50, 20, 20, 255), IM_COL32(255, 80, 60, 70)},    // defuse the bomb
        {IM_COL32(15, 20, 45, 255), IM_COL32(255, 70, 70, 60)},    // spaceship bridge
        {IM_COL32(60, 45, 40, 255), IM_COL32(255, 150, 60, 70)},   // throne room
        {IM_COL32(50, 60, 40, 255), IM_COL32(200, 220, 150, 40)},  // zombie show
        {IM_COL32(15, 15, 30, 255), IM_COL32(180, 190, 255, 40)},  // haunted house
        {IM_COL32(190, 130, 60, 255), IM_COL32(255, 240, 170, 80)}, // western showdown
        {IM_COL32(150, 60, 110, 255), IM_COL32(255, 220, 240, 60)}, // confessional
        {IM_COL32(120, 30, 50, 255), IM_COL32(255, 170, 120, 80)}, // dating show
        {IM_COL32(50, 20, 90, 255), IM_COL32(255, 215, 110, 80)},  // talent audition
        {IM_COL32(30, 40, 150, 255), IM_COL32(255, 220, 90, 80)},  // game show
        {IM_COL32(170, 60, 25, 255), IM_COL32(255, 210, 120, 70)}, // cooking contest
        {IM_COL32(60, 130, 90, 255), IM_COL32(255, 255, 220, 70)}, // home makeover
        {IM_COL32(50, 100, 50, 255), IM_COL32(230, 255, 170, 60)}, // nature documentary
        {IM_COL32(150, 110, 50, 255), IM_COL32(255, 235, 170, 70)}, // history documentary
        {IM_COL32(30, 110, 60, 255), IM_COL32(255, 255, 220, 60)}, // sports highlight
        {IM_COL32(40, 15, 25, 255), IM_COL32(255, 110, 60, 80)},   // wrestling promo
        {IM_COL32(80, 170, 200, 255), IM_COL32(255, 245, 150, 80)}, // kids show
        {IM_COL32(190, 40, 40, 255), IM_COL32(255, 240, 120, 80)}, // infomercial
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
    case 18: { // Pop idol: spotlights from above, an arm up, confetti falling
        for (float x : {22.0f, 78.0f}) dl->AddTriangleFilled(c.at({x, -2}), c.at({40, 64}), c.at({60, 64}), IM_COL32(255, 240, 200, 40));
        const Pose a = {{50, 13}, {50, 20}, {50, 39}, {44, 28}, {40, 36}, {57, 15}, {62, 6}, {47, 50}, {45, 62}, {53, 50}, {55, 62}};
        const Pose b = {{50, 13}, {50, 20}, {50, 39}, {44, 27}, {38, 22}, {57, 15}, {60, 5}, {46, 50}, {42, 62}, {54, 50}, {57, 62}};
        Figure(c, Mix(a, b, sw));
        static const ImU32 kConfetti[] = {IM_COL32(255, 90, 160, 230), IM_COL32(90, 200, 255, 230), IM_COL32(255, 220, 80, 230), IM_COL32(140, 255, 140, 230)};
        for (int i = 0; i < 12; ++i) {
            const float x = 6.0f + (i * 37) % 88, y = std::fmod((i * 29) % 60 + tt * 14.0f, 66.0f);
            dl->AddRectFilled(c.at({x - 1.2f, y - 0.7f}), c.at({x + 1.2f, y + 0.7f}), kConfetti[i % 4]);
        }
        break;
    }
    case 19: { // Rap video: from below, the hands going, flashing lights, smoke
        const int flash = moving ? static_cast<int>(tt * 4.0f) % 2 : 0;
        for (int i = 0; i < 3; ++i)
            dl->AddTriangleFilled(c.at({15.0f + i * 35, 0}), c.at({8.0f + i * 35, 44}), c.at({24.0f + i * 35, 44}), IM_COL32(200, 120, 255, flash == i % 2 ? 70 : 25));
        const Pose a = {{50, 9}, {50, 17}, {50, 38}, {42, 26}, {36, 18}, {58, 26}, {56, 34}, {44, 50}, {38, 64}, {56, 50}, {62, 64}};
        const Pose b = {{50, 9}, {50, 17}, {50, 38}, {42, 26}, {38, 32}, {58, 26}, {64, 18}, {44, 50}, {38, 64}, {56, 50}, {62, 64}};
        Figure(c, Mix(a, b, sw), kInk, 4.0f);
        for (int i = 0; i < 6; ++i)
            dl->AddCircleFilled(c.at({10.0f + i * 16.0f + std::sin(tt + i) * 2.0f, 62.0f - (i % 2) * 3.0f}), c.s(7.0f), IM_COL32(220, 220, 235, 45));
        break;
    }
    case 20: { // Mukbang: at a table of food, a bite going up to the mouth, a mic
        const Pose f = {{50, 16}, {50, 24}, {50, 52}, {40, 36}, {44, 46}, {60, 36}, {58.0f - sw * 4.0f, 44.0f - sw * 20.0f}, {48, 60}, {48, 70}, {52, 60}, {52, 70}};
        Figure(c, f, kInk, 4.2f);
        dl->AddRectFilled(c.at({4, 48}), c.at({96, 72}), IM_COL32(120, 80, 50, 255));
        dl->PathArcTo(c.at({28, 48}), c.s(9.0f), 0.0f, 3.14159f, 16);
        dl->PathFillConvex(IM_COL32(240, 240, 245, 255));
        for (int i = 0; i < 4; ++i) dl->AddCircleFilled(c.at({22.0f + i * 4.0f, 47.0f - (i % 2)}), c.s(2.2f), IM_COL32(240, 170, 60, 255));
        dl->AddEllipseFilled(c.at({70, 50}), ImVec2(c.s(10.0f), c.s(2.5f)), IM_COL32(240, 240, 245, 255));
        dl->AddCircleFilled(c.at({67, 48}), c.s(2.5f), IM_COL32(220, 80, 60, 255));
        dl->AddCircleFilled(c.at({73, 48}), c.s(2.2f), IM_COL32(110, 200, 90, 255));
        const P h = f.rh; // the chopsticks and the bite
        dl->AddLine(c.at(h), c.at({h.x - 6.5f, h.y - 4.0f}), IM_COL32(230, 200, 150, 255), c.s(0.9f));
        dl->AddLine(c.at({h.x, h.y + 1.0f}), c.at({h.x - 6.5f, h.y - 3.0f}), IM_COL32(230, 200, 150, 255), c.s(0.9f));
        dl->AddCircleFilled(c.at({h.x - 6.5f, h.y - 3.5f}), c.s(1.8f), IM_COL32(240, 170, 60, 255));
        dl->AddLine(c.at({90, 48}), c.at({86, 32}), IM_COL32(50, 50, 60, 255), c.s(1.2f));
        dl->AddCircleFilled(c.at({86, 29}), c.s(3.2f), IM_COL32(40, 40, 50, 255));
        break;
    }
    case 21: { // ASMR whisper: close up, a hand by the mouth, soft sound rings, fingers tapping the table
        const Pose f = {{46, 20}, {46, 29}, {46, 62}, {36, 42}, {40.0f, 52.0f + sw * 2.0f}, {56, 40}, {52, 24}, {44, 70}, {44, 80}, {48, 70}, {48, 80}};
        Figure(c, f, kInk, 4.4f);
        for (int i = 0; i < 3; ++i) {
            const float r = 6.0f + i * 5.0f + (moving ? std::fmod(tt * 6.0f, 5.0f) : 0.0f);
            Arc(c, {54, 22}, r, -0.6f, 0.6f, IM_COL32(255, 230, 200, 180 - i * 50), 1.0f, false);
        }
        dl->AddLine(c.at({8, 56}), c.at({92, 56}), dim, c.s(0.8f));
        Text(c, {72, 38}, "shh", IM_COL32(255, 230, 200, 200), 9.0f);
        break;
    }
    case 22: { // ASMR studio: fairy lights behind, a big microphone in front, a soft brush
        dl->AddBezierCubic(c.at({4, 6}), c.at({35, 16}), c.at({65, 2}), c.at({96, 10}), IM_COL32(255, 255, 255, 50), c.s(0.6f));
        for (int i = 0; i < 9; ++i) {
            const float x = 8.0f + i * 10.5f, y = 7.5f + std::sin(i * 0.9f) * 2.5f + (i % 2) * 1.5f;
            const int a = moving ? 140 + static_cast<int>(115.0f * (0.5f + 0.5f * std::sin(tt * 3.0f + i))) : 220;
            dl->AddCircleFilled(c.at({x, y}), c.s(1.4f), IM_COL32(255, 220, 140, a));
        }
        const P brush{54.0f + sw * 3.0f, 36.0f - sw * 2.0f};
        const Pose f = {{38, 22}, {38, 30}, {38, 62}, {30, 42}, {34, 52}, {46, 40}, brush, {36, 70}, {36, 80}, {40, 70}, {40, 80}};
        Figure(c, f, kInk, 4.2f);
        dl->AddLine(c.at(brush), c.at({brush.x + 4.0f, brush.y - 3.0f}), IM_COL32(200, 150, 90, 255), c.s(1.2f));
        dl->AddCircleFilled(c.at({brush.x + 5.0f, brush.y - 3.8f}), c.s(1.8f), IM_COL32(255, 230, 240, 255));
        dl->AddRectFilled(c.at({61, 28}), c.at({73, 48}), IM_COL32(60, 60, 72, 255), c.s(6.0f));
        for (int i = 0; i < 4; ++i) dl->AddLine(c.at({62, 32.0f + i * 4}), c.at({72, 32.0f + i * 4}), IM_COL32(120, 120, 135, 255), c.s(0.6f));
        dl->AddLine(c.at({67, 48}), c.at({67, 72}), IM_COL32(60, 60, 72, 255), c.s(1.6f));
        break;
    }
    case 23: { // Behind the scenes: the camera pulls back from the shot to the film set around it
        const float k = moving ? 1.0f - 0.45f * std::min(1.0f, std::fmod(tt * 0.5f, 1.4f)) : 0.55f;
        dl->AddRectFilled(c.at({6, 12}), c.at({20, 26}), IM_COL32(250, 250, 240, 220)); // a softbox
        dl->AddLine(c.at({13, 26}), c.at({13, 62}), dim, c.s(1.0f));
        dl->AddLine(c.at({74, 2}), c.at({54, 12}), IM_COL32(180, 180, 190, 255), c.s(1.0f)); // the boom mic
        dl->AddRectFilled(c.at({48, 10}), c.at({56, 14}), IM_COL32(120, 120, 130, 255), c.s(1.5f));
        Figure(c, Scale(kStand, {84, 62}, 0.5f), IM_COL32(220, 220, 230, 255), 2.2f); // crew
        Figure(c, Scale(Shift(kStand, -26, 0), {24, 62}, 0.45f), IM_COL32(220, 220, 230, 255), 2.0f);
        const float hw = 32.0f * k, hh = 24.0f * k; // the shot itself
        dl->AddRectFilled(c.at({50 - hw, 34 - hh}), c.at({50 + hw, 34 + hh}), IM_COL32(110, 180, 230, 255));
        dl->AddTriangleFilled(c.at({50 - hw, 34 + hh}), c.at({50 - hw * 0.2f, 34 - hh * 0.2f}), c.at({50 + hw * 0.5f, 34 + hh}), IM_COL32(60, 150, 80, 255));
        dl->AddCircleFilled(c.at({50 + hw * 0.55f, 34 - hh * 0.45f}), c.s(4.0f * k), IM_COL32(255, 220, 90, 255));
        dl->AddRect(c.at({50 - hw, 34 - hh}), c.at({50 + hw, 34 + hh}), IM_COL32(255, 255, 255, 230), 0, 0, c.s(1.0f));
        dl->AddRectFilled(c.at({72, 50}), c.at({86, 60}), IM_COL32(25, 25, 25, 255)); // the clapperboard
        dl->AddQuadFilled(c.at({72, 46.5f}), c.at({86, 44.5f}), c.at({86, 48.0f}), c.at({72, 50.0f}), IM_COL32(240, 240, 240, 255));
        break;
    }
    case 24: { // Melting doodles: cartoon doodles drawn over the picture melt and drip down
        dl->AddTriangleFilled(c.at({0, 66}), c.at({40, 34}), c.at({90, 66}), IM_COL32(60, 140, 90, 255));
        const float melt = moving ? std::min(1.0f, std::fmod(tt * 0.35f, 1.3f)) : 0.45f;
        const P at[3] = {{24, 20}, {54, 16}, {78, 28}};
        const ImU32 col[3] = {IM_COL32(255, 90, 160, 255), IM_COL32(255, 220, 60, 255), IM_COL32(90, 230, 255, 255)};
        Sparkle(c, at[0], 7.0f, col[0]);
        dl->AddCircle(c.at(at[1]), c.s(7.0f), col[1], 24, c.s(1.6f)); // a smiley
        for (float x : {-2.5f, 2.5f}) dl->AddCircleFilled(c.at({at[1].x + x, at[1].y - 2.0f}), c.s(1.0f), col[1]);
        Arc(c, {at[1].x, at[1].y + 0.5f}, 3.8f, 0.4f, 2.7f, col[1], 1.4f, false);
        Arc(c, at[2], 5.0f, 0.0f, 5.5f, col[2], 1.6f, false); // a swirl
        Arc(c, at[2], 2.2f, 0.0f, 5.0f, col[2], 1.4f, false);
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                const float x = at[i].x - 4.0f + j * 4.0f, y = at[i].y + 4.0f, len = melt * (12.0f + j * 5.0f + i * 3.0f);
                if (len < 0.5f) continue;
                dl->AddLine(c.at({x, y}), c.at({x, y + len}), col[i], c.s(1.8f));
                dl->AddCircleFilled(c.at({x, y + len}), c.s(1.4f), col[i]);
            }
        break;
    }
    case 25: { // Storm giant: the clouds gather into a giant behind the town, lightning in its chest, rain
        const ImU32 cl = IM_COL32(150, 160, 185, 235);
        const float rise = moving ? std::min(1.0f, tt * 0.4f) : 1.0f, y0 = 64.0f - 48.0f * rise; // its head
        dl->AddLine(c.at({38, y0 + 15}), c.at({12, y0 + 2}), cl, c.s(6.0f));
        dl->AddLine(c.at({62, y0 + 15}), c.at({88, y0 + 2}), cl, c.s(6.0f));
        Cloud(c, {50, y0 + 20}, 1.7f, cl);
        dl->AddCircleFilled(c.at({50, y0}), c.s(9.0f), cl);
        for (float x : {-3.5f, 3.5f}) dl->AddCircleFilled(c.at({50 + x, y0 - 1.0f}), c.s(1.3f), IM_COL32(255, 250, 200, 255));
        if (!moving || static_cast<int>(tt * 5.0f) % 4 == 0) {
            const ImVec2 bolt[4] = {c.at({48, y0 + 13}), c.at({54, y0 + 19}), c.at({48, y0 + 21}), c.at({54, y0 + 29})};
            dl->AddPolyline(bolt, 4, IM_COL32(255, 240, 120, 255), 0, c.s(1.6f));
        }
        for (int i = 0; i < 6; ++i) { // the town in front of it
            const float x = 6.0f + i * 15.0f, hgt = 7.0f + (i * 5) % 6;
            dl->AddRectFilled(c.at({x, 64 - hgt}), c.at({x + 10, 64}), IM_COL32(22, 24, 34, 255));
            dl->AddTriangleFilled(c.at({x - 1, 64 - hgt}), c.at({x + 5, 59 - hgt}), c.at({x + 11, 64 - hgt}), IM_COL32(22, 24, 34, 255));
        }
        for (int i = 0; i < 12; ++i) {
            const float x = 4.0f + (i * 43) % 92, y = std::fmod((i * 19) % 60 + tt * 50.0f, 66.0f);
            dl->AddLine(c.at({x, y}), c.at({x - 1.0f, y + 4.0f}), IM_COL32(170, 190, 230, 140), c.s(0.7f));
        }
        break;
    }
    case 26: { // Roller coaster: over the top and down the drop, hands up
        const P a{-2, 56}, b{30, 6}, d{52, 18}, e{100, 66};
        auto at = [&](float u) {
            const float v = 1.0f - u;
            return P{v * v * v * a.x + 3 * v * v * u * b.x + 3 * v * u * u * d.x + u * u * u * e.x, v * v * v * a.y + 3 * v * v * u * b.y + 3 * v * u * u * d.y + u * u * u * e.y};
        };
        for (int i = 1; i < 9; ++i) {
            const P q = at(i / 9.0f);
            dl->AddLine(c.at(q), c.at({q.x, 70}), IM_COL32(255, 255, 255, 60), c.s(0.8f));
        }
        dl->AddBezierCubic(c.at(a), c.at(b), c.at(d), c.at(e), IM_COL32(230, 80, 80, 255), c.s(1.8f));
        const float u = moving ? 0.25f + std::fmod(tt * 0.3f, 0.6f) : 0.42f;
        const P q = at(u);
        dl->AddRectFilled(c.at({q.x - 5, q.y - 5}), c.at({q.x + 5, q.y}), IM_COL32(255, 220, 60, 255), c.s(1.5f));
        dl->AddCircleFilled(c.at({q.x, q.y - 8}), c.s(2.8f), kInk);
        for (float s : {-1.0f, 1.0f}) dl->AddLine(c.at({q.x + s * 1.5f, q.y - 6}), c.at({q.x + s * 5.0f, q.y - 14}), kInk, c.s(1.6f));
        if (u > 0.45f) Lines(c, {q.x - 6, q.y - 8}, {-1, -0.8f}, 3, 2.5f, 8.0f, IM_COL32(255, 255, 255, 140));
        break;
    }
    case 27: { // Cloud surf: carving over pink clouds at sunset on a surfboard
        dl->AddCircleFilled(c.at({80, 16}), c.s(8.0f), IM_COL32(255, 180, 90, 230));
        const float bob = moving ? std::sin(tt * 2.5f) * 2.0f : 0.0f;
        for (int i = 0; i < 5; ++i) {
            const float x = std::fmod(i * 28.0f - tt * 14.0f + 420.0f, 140.0f) - 20.0f;
            Cloud(c, {x, 54.0f + (i % 2) * 5.0f}, 1.1f, IM_COL32(255, 190, 215, 230));
        }
        const Pose f = {{50, 14}, {50, 21}, {50, 34}, {42, 24}, {34, 22}, {58, 25}, {66, 28}, {44, 39}, {40, 44}, {56, 39}, {59, 44}};
        Figure(c, Shift(f, 0, bob));
        dl->AddEllipseFilled(c.at({50, 45.5f + bob}), ImVec2(c.s(15.0f), c.s(2.0f)), IM_COL32(255, 255, 255, 245));
        for (int i = 0; i < 5; ++i) dl->AddCircleFilled(c.at({32.0f - i * 5.0f, 45.0f + bob - (i % 2) * 2.0f}), c.s(2.2f - i * 0.3f), IM_COL32(255, 255, 255, 150 - i * 25));
        break;
    }
    case 28: { // ASMR product: two fingertips tap and scratch the product, close up
        Bottle(c, {50, 62}, 1.6f, IM_COL32(90, 170, 255, 255));
        for (int s = 0; s < 2; ++s) {
            const float d = s ? sw : 1.0f - sw, y = 34.0f + s * 8.0f, side = s ? 1.0f : -1.0f;
            const float px = 50.0f + side * (10.0f + 4.0f * d);
            dl->AddLine(c.at({px + side * 14.0f, y + 6.0f}), c.at({px, y}), IM_COL32(240, 200, 170, 255), c.s(4.0f)); // a finger
            dl->AddCircleFilled(c.at({px, y}), c.s(2.0f), IM_COL32(230, 80, 120, 255));                               // its nail
            if (d < 0.3f) Lines(c, {px + side * 3.0f, y - 4.0f}, {side, -1.0f}, 3, 2.0f, 3.0f, IM_COL32(255, 255, 255, 180));
        }
        break;
    }
    case 29: { // Magic button: a hand presses a glowing button, the product floats up in sparks
        const float lift = moving ? std::sin(tt * 2.0f) * 3.0f + 4.0f : 0.0f;
        Bottle(c, {34, 58 - lift}, 1.2f, IM_COL32(255, 150, 80, 255));
        dl->AddCircleFilled(c.at({72, 55}), c.s(15.0f), IM_COL32(255, 80, 120, static_cast<int>(40 + 60 * sw)));
        dl->AddEllipseFilled(c.at({72, 58}), ImVec2(c.s(12.0f), c.s(4.0f)), IM_COL32(60, 60, 70, 255));
        dl->AddEllipseFilled(c.at({72, 55.0f + sw * 2.0f}), ImVec2(c.s(10.0f), c.s(3.5f)), IM_COL32(255, 60, 90, 255));
        dl->AddLine(c.at({82, 28.0f + sw * 4.0f}), c.at({73, 47.0f + sw * 5.5f}), IM_COL32(240, 200, 170, 255), c.s(4.0f)); // the finger
        for (int i = 0; i < 6; ++i) {
            const float ph = tt * 3.0f + i * 1.3f, r = moving ? 1.5f + 1.8f * (0.5f + 0.5f * std::sin(ph)) : 2.2f;
            Sparkle(c, {16.0f + (i * 13) % 36, 10.0f + (i * 23) % 40 - lift}, r, IM_COL32(255, 240, 180, 230));
        }
        break;
    }
    case 30: { // Kick ad: the product stands still, boxes slide at it and get knocked flying
        Ground(c, 60.5f, dim);
        Bottle(c, {50, 60}, 1.2f, IM_COL32(120, 220, 140, 255));
        const float ph = moving ? std::fmod(tt * 0.7f, 1.0f) : 0.35f;
        for (int s = 0; s < 2; ++s) {
            const float dir = s ? 1.0f : -1.0f, q = std::fmod(ph + s * 0.5f, 1.0f);
            P m;
            float rot = 0.0f;
            if (q < 0.5f) {
                m = {50.0f + dir * (56.0f - q * 2.0f * 43.0f), 55}; // sliding in
            } else {
                const float u = (q - 0.5f) * 2.0f;
                m = {50.0f + dir * (13.0f + u * 44.0f), 55.0f - u * 44.0f + u * u * 14.0f};
                rot = u * 4.0f * dir;
                if (u < 0.15f) Burst(c, {50.0f + dir * 8.5f, 52}, 5.0f, 2.0f, 6, accent);
            }
            Box(c, m, 5.0f, rot, IM_COL32(165, 165, 170, 255));
        }
        break;
    }
    case 31: { // Toy truck ad: a toy delivery truck with the product on its side drives up and parks beside it
        Ground(c, 59.0f, dim);
        const ImU32 product = IM_COL32(255, 120, 90, 255);
        Bottle(c, {64, 58}, 1.2f, product);
        const float u = moving ? std::min(1.0f, tt * 0.5f) : 1.0f, x = -30.0f + (1.0f - (1.0f - u) * (1.0f - u)) * 56.0f;
        dl->AddRectFilled(c.at({x - 22, 43}), c.at({x, 56}), IM_COL32(245, 245, 245, 255), c.s(1.0f));
        dl->AddRectFilled(c.at({x, 47}), c.at({x + 8, 56}), IM_COL32(230, 230, 235, 255), c.s(1.5f));
        dl->AddRectFilled(c.at({x + 2, 48.5f}), c.at({x + 6.5f, 52}), IM_COL32(120, 180, 230, 255));
        Bottle(c, {x - 11, 54.5f}, 0.32f, product);
        for (float wx : {x - 17.0f, x - 5.0f, x + 4.5f}) {
            dl->AddCircleFilled(c.at({wx, 57}), c.s(2.6f), IM_COL32(30, 30, 35, 255));
            dl->AddCircleFilled(c.at({wx, 57}), c.s(1.0f), IM_COL32(180, 180, 190, 255));
        }
        break;
    }
    case 32: { // Volcano ad: an erupting volcano behind the product, a red glow over it
        dl->AddQuadFilled(c.at({8, 66}), c.at({40, 22}), c.at({60, 22}), c.at({92, 66}), IM_COL32(60, 35, 30, 255));
        dl->AddCircleFilled(c.at({50, 46}), c.s(30.0f), IM_COL32(255, 60, 20, 55), 40);
        for (int i = 0; i < 10; ++i) {
            const float f = std::fmod(tt * 0.8f + i * 0.1f, 1.0f);
            const float x = 50.0f + (i - 4.5f) * 4.0f * f, y = 22.0f - std::sin(f * 3.14159f) * 18.0f + f * 6.0f;
            dl->AddCircleFilled(c.at({x, y}), c.s(2.2f - f), IM_COL32(255, 120 + (i * 13) % 100, 30, static_cast<int>(255 * (1.0f - f * 0.7f))));
        }
        Bottle(c, {50, 64}, 1.1f, IM_COL32(230, 230, 240, 255));
        break;
    }
    // ---- the longer ones
    case 33: { // Origin story: rain, a lightning bolt, the figure rising with a clenched fist
        for (int i = 0; i < 12; ++i) {
            const float x = 6.0f + i * 8.0f, y = std::fmod(i * 17.0f + tt * 40.0f, 70.0f);
            dl->AddLine(c.at({x, y}), c.at({x - 1.0f, y + 6.0f}), dim, c.s(0.7f));
        }
        const ImVec2 bolt[] = {c.at({78, 2}), c.at({70, 20}), c.at({76, 20}), c.at({66, 40})};
        if (!moving || std::fmod(tt, 1.1f) < 0.25f) dl->AddPolyline(bolt, 4, IM_COL32(230, 235, 255, 255), 0, c.s(1.8f));
        const Pose f = {{44, 13}, {45, 20}, {46, 39}, {39, 29}, {38, 39}, {53, 18}, {55, 8}, {44, 50}, {43, 62}, {50, 50}, {51, 62}};
        Figure(c, Shift(f, 0, -sw * 4.0f));
        dl->AddCircleFilled(c.at({55, 7}), c.s(3.5f), IM_COL32(190, 215, 255, 110));
        break;
    }
    case 34: { // Movie trailer: black bars, a figure turning in the dark, sparks falling
        for (int i = 0; i < 9; ++i) {
            const float f = std::fmod(tt * 0.5f + i * 0.11f, 1.0f);
            dl->AddCircleFilled(c.at({14.0f + i * 9.0f, 12.0f + f * 46.0f}), c.s(1.0f), IM_COL32(255, 170, 70, static_cast<int>(240 * (1.0f - f))));
        }
        const Pose a = {{50, 16}, {50, 23}, {50, 41}, {45, 32}, {44, 41}, {55, 32}, {56, 41}, {48, 52}, {47, 62}, {52, 52}, {53, 62}};
        const Pose b = {{47, 16}, {49, 23}, {50, 41}, {44, 32}, {42, 41}, {55, 32}, {55, 41}, {48, 52}, {46, 62}, {52, 52}, {54, 62}};
        Figure(c, Mix(a, b, sw), IM_COL32(205, 200, 205, 255));
        dl->AddRectFilled(c.at({0, 0}), c.at({100, 10}), IM_COL32(0, 0, 0, 255));
        dl->AddRectFilled(c.at({0, 60}), c.at({100, 70}), IM_COL32(0, 0, 0, 255));
        break;
    }
    case 35: { // Confession: close, a hand to the chest, a small heart
        const Pose f = {{46, 24}, {46, 33}, {46, 66}, {36, 46}, {44, 42}, {56, 46}, {50.0f, 42.0f + sw * 2.0f}, {44, 70}, {44, 80}, {48, 70}, {48, 80}};
        Figure(c, f, kInk, 4.6f);
        const P h{74, 20};
        const float k = 1.0f + sw * 0.2f;
        const ImU32 pink = IM_COL32(255, 130, 170, 255);
        dl->AddCircleFilled(c.at({h.x - 3.0f * k, h.y}), c.s(3.4f * k), pink);
        dl->AddCircleFilled(c.at({h.x + 3.0f * k, h.y}), c.s(3.4f * k), pink);
        dl->AddTriangleFilled(c.at({h.x - 6.2f * k, h.y + 1.0f}), c.at({h.x + 6.2f * k, h.y + 1.0f}), c.at({h.x, h.y + 9.0f * k}), pink);
        break;
    }
    case 36: { // Story time: close, both hands up, the bubbles getting bigger
        const Pose f = {{36, 25}, {36, 34}, {36, 66}, {24, 46}, {20.0f, 36.0f - sw * 3.0f}, {48, 46}, {52.0f, 36.0f + sw * 3.0f}, {34, 70}, {34, 80}, {38, 70}, {38, 80}};
        Figure(c, f, kInk, 4.6f);
        const int show = moving ? static_cast<int>(tt * 1.6f) % 4 : 3;
        for (int i = 0; i < 3; ++i)
            if (i < show) dl->AddCircleFilled(c.at({58.0f + i * 11.0f, 30.0f - i * 8.0f}), c.s(2.5f + i * 2.0f), IM_COL32(255, 255, 255, 235));
        if (show == 3) Text(c, {78.0f, 2.0f}, "HA", accent, 10.0f);
        break;
    }
    case 37: { // Get ready: a mirror with the figure in it, a sparkle
        dl->AddRectFilled(c.at({54, 6}), c.at({90, 64}), IM_COL32(210, 230, 245, 70), c.s(3.0f));
        dl->AddRect(c.at({54, 6}), c.at({90, 64}), IM_COL32(255, 255, 255, 220), c.s(3.0f), 0, c.s(1.2f));
        const Pose f = {{30, 13}, {30, 20}, {30, 39}, {38, 26}, {35.0f + sw * 2.0f, 15.0f}, {23, 29}, {21, 38}, {27, 50}, {26, 62}, {33, 50}, {34, 62}};
        Figure(c, f);
        const Pose m = {{72, 18}, {72, 24}, {72, 40}, {66, 29}, {69.0f - sw * 2.0f, 19.0f}, {78, 31}, {79, 39}, {70, 50}, {69, 60}, {74, 50}, {75, 60}};
        Figure(c, m, IM_COL32(255, 255, 255, 150), 2.6f);
        Sparkle(c, {84, 13}, 3.0f + sw * 2.0f, accent);
        break;
    }
    case 38: { // Full routine: the moves one after another, across the card
        const Pose a = {{46, 13}, {48, 20}, {51, 39}, {40, 17}, {37, 8}, {57, 25}, {63, 18}, {45, 50}, {42, 62}, {57, 49}, {60, 60}};
        const Pose b = {{54, 13}, {52, 20}, {49, 39}, {43, 25}, {37, 18}, {60, 17}, {63, 8}, {43, 49}, {40, 60}, {55, 50}, {58, 62}};
        const Pose d = {{50, 34}, {50, 40}, {50, 52}, {41, 44}, {36, 54}, {59, 44}, {64, 54}, {42, 54}, {40, 62}, {58, 54}, {60, 62}};
        const int beat = moving ? static_cast<int>(tt * 2.0f) % 3 : 1;
        Figure(c, Scale(Shift(a, -30, 0), {20, 62}, 0.72f), beat == 0 ? kInk : IM_COL32(255, 255, 255, 110));
        Figure(c, Scale(b, {50, 62}, 0.8f), beat == 1 ? kInk : IM_COL32(255, 255, 255, 110));
        Figure(c, Scale(Shift(d, 30, 0), {80, 62}, 0.8f), beat == 2 ? kInk : IM_COL32(255, 255, 255, 110));
        Ground(c, 62.5f, dim);
        Note(c, {50.0f, 10.0f - sw * 3.0f}, IM_COL32(120, 225, 255, 230));
        break;
    }
    case 39: { // Fight scene: a kick into one who falls back, an impact
        const Pose a = {{28, 15}, {30, 22}, {34, 39}, {24, 27}, {19, 22}, {37, 28}, {33, 34}, {32, 50}, {31, 62}, {46, 33}, {58, 26}};
        const Pose d = {{76.0f + sw * 3.0f, 19}, {73, 25}, {70, 42}, {79, 26}, {85, 21}, {64, 29}, {59, 24}, {70, 52}, {66, 62}, {76, 52}, {80, 62}};
        Figure(c, a);
        Figure(c, d, IM_COL32(255, 200, 190, 255));
        Burst(c, {59, 26}, 7.0f + sw * 2.0f, 3.0f, 7, IM_COL32(255, 200, 80, 235), tt);
        Ground(c, 62.5f, dim);
        break;
    }
    case 40: { // Chase: running past buildings going the other way
        const float slide = moving ? std::fmod(tt * 34.0f, 34.0f) : 8.0f;
        for (int i = -1; i < 5; ++i) {
            const float x = i * 34.0f + 34.0f - slide, top = 8.0f + (i & 1) * 12.0f;
            dl->AddRectFilled(c.at({x, top}), c.at({x + 24.0f, 62}), IM_COL32(60, 66, 92, 255));
            for (int w = 0; w < 3; ++w) dl->AddRectFilled(c.at({x + 4.0f + w * 6.0f, top + 5.0f}), c.at({x + 7.0f + w * 6.0f, top + 10.0f}), IM_COL32(255, 210, 120, 150));
        }
        const Pose a = {{56, 13}, {54, 20}, {47, 37}, {45, 26}, {38, 21}, {62, 27}, {68, 19}, {57, 45}, {64, 56}, {42, 49}, {32, 44}};
        const Pose b = {{56, 14}, {54, 21}, {47, 38}, {62, 28}, {67, 21}, {45, 27}, {38, 23}, {42, 49}, {34, 55}, {57, 46}, {65, 42}};
        Figure(c, Mix(a, b, sw));
        Lines(c, {22, 28}, {-1, 0}, 3, 6.0f, 12.0f, dim);
        Ground(c, 62.5f, dim);
        break;
    }
    case 41: { // Cooking show: a figure, a pan, flames going up
        const Pose f = {{30, 13}, {30, 20}, {30, 39}, {24, 29}, {23, 38}, {40, 27}, {52, 30}, {27, 50}, {26, 62}, {33, 50}, {34, 62}};
        Figure(c, f);
        dl->AddLine(c.at({50, 30}), c.at({62, 33}), IM_COL32(190, 190, 200, 255), c.s(2.0f));
        dl->AddRectFilled(c.at({60, 31}), c.at({86, 36}), IM_COL32(50, 50, 58, 255), c.s(2.0f));
        for (int i = 0; i < 5; ++i) {
            const float x = 63.0f + i * 5.0f, h2 = 8.0f + 6.0f * std::sin(tt * 7.0f + i * 1.7f) * (moving ? 1.0f : 0.5f) + (i == 2 ? 7.0f : 0.0f);
            dl->AddTriangleFilled(c.at({x - 3.0f, 31}), c.at({x + 3.0f, 31}), c.at({x, 31.0f - h2}), IM_COL32(255, 150 + i * 16, 50, 235));
        }
        break;
    }
    case 42: { // Magic trick: a card thrown up, turning into sparkles
        const Pose f = {{30, 13}, {30, 20}, {30, 39}, {24, 29}, {23, 38}, {38, 22}, {46, 14}, {27, 50}, {26, 62}, {33, 50}, {34, 62}};
        Figure(c, f);
        const float up = moving ? std::fmod(tt * 0.6f, 1.0f) : 0.45f;
        Box(c, {50.0f + up * 18.0f, 13.0f + (1.0f - up) * 4.0f}, 3.2f, tt * 2.0f, IM_COL32(255, 255, 255, static_cast<int>(245 * (1.0f - up * 0.6f))));
        for (int i = 0; i < 6; ++i) {
            const float a = i * 1.05f + tt, r = 5.0f + up * 18.0f;
            Sparkle(c, {68.0f + std::cos(a) * r, 26.0f + std::sin(a) * r * 0.8f}, 2.2f, IM_COL32(215, 170, 255, 230));
        }
        break;
    }
    case 43: { // Time passes: the sun racing over a figure on a bench, the sky turning
        const float a = moving ? std::fmod(tt * 1.4f, 3.14159f) : 1.0f;
        const P sun{50.0f - std::cos(a) * 42.0f, 54.0f - std::sin(a) * 46.0f};
        dl->AddCircleFilled(c.at(sun), c.s(5.0f), IM_COL32(255, 225, 120, 255));
        Arc(c, {50, 54}, 42, 3.14159f, 6.28318f, dim, 0.8f, false);
        dl->AddRectFilled(c.at({34, 52}), c.at({66, 55}), IM_COL32(120, 80, 50, 255));
        const Pose f = {{50, 24}, {50, 31}, {50, 49}, {44, 39}, {45, 48}, {56, 39}, {55, 48}, {44, 51}, {44, 62}, {56, 51}, {56, 62}};
        Figure(c, f);
        Ground(c, 62.5f, dim);
        break;
    }
    case 44: { // Transformation: light climbing around the figure in a spiral
        const float rise = moving ? std::fmod(tt * 0.5f, 1.0f) : 0.6f;
        for (int i = 0; i < 16; ++i) {
            const float f = i / 15.0f, y = 62.0f - f * 54.0f * (0.3f + rise * 0.7f), a = f * 12.0f + tt * 3.0f;
            Sparkle(c, {50.0f + std::cos(a) * (15.0f - f * 5.0f), y}, 1.2f + f * 1.2f, IM_COL32(255, 225, 130, 235));
        }
        const Pose f = {{50, 13}, {50, 20}, {50, 39}, {42, 24}, {36, 15}, {58, 24}, {64, 15}, {47, 50}, {44, 62}, {53, 50}, {56, 62}};
        Figure(c, Mix(kStand, f, sw));
        break;
    }
    case 45: { // Concert finale: beams from above, a crowd's heads, fireworks
        for (int i = 0; i < 4; ++i) {
            const float x = 16.0f + i * 22.0f, sway = std::sin(tt * 1.6f + i) * 10.0f;
            dl->AddTriangleFilled(c.at({x, 0}), c.at({x - 9.0f + sway, 56}), c.at({x + 9.0f + sway, 56}), IM_COL32(255, 200, 240, 28));
        }
        Burst(c, {20, 15}, 6.0f + sw * 3.0f, 2.0f, 8, IM_COL32(255, 210, 90, 230), tt);
        Burst(c, {82, 12}, 5.0f + (1.0f - sw) * 3.0f, 2.0f, 8, IM_COL32(120, 220, 255, 230), -tt);
        const Pose f = {{50, 13}, {50, 20}, {50, 38}, {44, 28}, {43, 37}, {57, 17}, {62, 7}, {47, 48}, {46, 58}, {53, 48}, {54, 58}};
        Figure(c, f, kInk, 3.0f);
        for (int i = 0; i < 11; ++i) dl->AddCircleFilled(c.at({5.0f + i * 9.0f, 66.0f - (i & 1) * 2.0f - sw * (i % 3)}), c.s(4.0f), IM_COL32(10, 8, 26, 255));
        break;
    }
    case 46: { // Product story: a beam of light sliding over the product, the camera circling
        const float at = moving ? 30.0f + std::fmod(tt * 16.0f, 40.0f) : 44.0f;
        dl->AddTriangleFilled(c.at({at + 8.0f, 0}), c.at({at - 12.0f, 66}), c.at({at + 18.0f, 66}), IM_COL32(220, 235, 255, 42));
        Bottle(c, {50, 62}, 1.25f, IM_COL32(205, 212, 226, 255));
        Arc(c, {50, 40}, 34, 0.5f + tt, 2.4f + tt, dim, 1.0f, true);
        for (int i = 0; i < 5; ++i) {
            const float f = std::fmod(tt * 0.45f + i * 0.2f, 1.0f);
            dl->AddCircleFilled(c.at({30.0f + i * 10.0f, 8.0f + f * 50.0f}), c.s(0.9f), IM_COL32(200, 225, 255, 200));
        }
        break;
    }
    // ---- two or more people
    case 47: { // Group hug: three leaning in, arms across, a heart over them
        const float in = sw * 4.0f;
        const Pose l = {{36.0f + in, 16}, {37, 23}, {36, 41}, {44, 29}, {52, 31}, {30, 30}, {29, 38}, {34, 51}, {33, 62}, {39, 51}, {40, 62}};
        const Pose r = {{64.0f - in, 16}, {63, 23}, {64, 41}, {56, 29}, {48, 31}, {70, 30}, {71, 38}, {61, 51}, {60, 62}, {66, 51}, {67, 62}};
        Figure(c, Scale(kStand, {50, 62}, 0.92f), IM_COL32(255, 215, 200, 255));
        Figure(c, l), Figure(c, r);
        const ImU32 pink = IM_COL32(255, 150, 180, 255);
        const float y = 7.0f - sw * 2.0f;
        dl->AddCircleFilled(c.at({47.5f, y}), c.s(2.8f), pink), dl->AddCircleFilled(c.at({52.5f, y}), c.s(2.8f), pink);
        dl->AddTriangleFilled(c.at({44.8f, y + 0.8f}), c.at({55.2f, y + 0.8f}), c.at({50, y + 7.0f}), pink);
        break;
    }
    case 48: { // High five: two jumping, hands meeting, an impact
        const float up = -sw * 7.0f;
        const Pose l = {{30, 18}, {31, 25}, {31, 43}, {38, 22}, {47, 13}, {25, 32}, {23, 40}, {28, 52}, {26, 62}, {35, 52}, {36, 62}};
        const Pose r = {{70, 18}, {69, 25}, {69, 43}, {62, 22}, {53, 13}, {75, 32}, {77, 40}, {65, 52}, {64, 62}, {72, 52}, {74, 62}};
        Figure(c, Shift(l, 0, up)), Figure(c, Shift(r, 0, up), IM_COL32(255, 235, 170, 255));
        Burst(c, {50, 12.0f + up}, 6.0f + sw * 3.0f, 2.5f, 8, IM_COL32(255, 245, 150, 240), tt);
        Ground(c, 62.5f, dim);
        break;
    }
    case 49: { // Group selfie: three heads close in a phone's frame, a flash
        dl->AddRectFilled(c.at({26, 6}), c.at({74, 64}), IM_COL32(20, 20, 30, 255), c.s(4.0f));
        dl->AddRect(c.at({26, 6}), c.at({74, 64}), IM_COL32(235, 235, 245, 255), c.s(4.0f), 0, c.s(1.3f));
        const ImU32 sk[] = {IM_COL32(255, 220, 190, 255), IM_COL32(200, 150, 110, 255), IM_COL32(245, 200, 170, 255)};
        const float hx[] = {38, 50, 62}, hy[] = {34, 26, 36};
        for (int i = 0; i < 3; ++i) {
            const float y = hy[i] + std::sin(tt * 5.0f + i) * 1.2f;
            dl->AddCircleFilled(c.at({hx[i], y}), c.s(7.5f), sk[i]);
            dl->AddCircleFilled(c.at({hx[i] - 2.5f, y - 1.0f}), c.s(0.9f), IM_COL32(30, 30, 40, 255));
            dl->AddCircleFilled(c.at({hx[i] + 2.5f, y - 1.0f}), c.s(0.9f), IM_COL32(30, 30, 40, 255));
            dl->AddCircleFilled(c.at({hx[i], y + 3.0f}), c.s(1.8f), IM_COL32(150, 40, 50, 255));
        }
        if (moving && std::fmod(tt, 1.3f) < 0.15f) dl->AddRectFilled(c.at({26, 6}), c.at({74, 64}), IM_COL32(255, 255, 255, 190), c.s(4.0f));
        break;
    }
    case 50: { // Duet dance: two in step, mirrored
        const Pose a = {{46, 13}, {48, 20}, {51, 39}, {40, 17}, {37, 8}, {57, 25}, {63, 18}, {45, 50}, {42, 62}, {57, 49}, {60, 60}};
        const Pose b = {{54, 13}, {52, 20}, {49, 39}, {43, 25}, {37, 18}, {60, 17}, {63, 8}, {43, 49}, {40, 60}, {55, 50}, {58, 62}};
        const Pose f = Mix(a, b, sw);
        Figure(c, Shift(f, -20, 0));
        Figure(c, Shift(f, 20, 0), IM_COL32(170, 235, 255, 255));
        Note(c, {50.0f, 18.0f - sw * 5.0f}, IM_COL32(255, 180, 240, 230));
        Ground(c, 62.5f, dim);
        break;
    }
    case 51: { // Cheers: two glasses meeting over two figures
        const float in = sw * 3.0f;
        const Pose l = {{28, 15}, {29, 22}, {29, 41}, {37, 25}, {44.0f + in, 19}, {23, 30}, {22, 39}, {26, 51}, {25, 62}, {32, 51}, {33, 62}};
        const Pose r = {{72, 15}, {71, 22}, {71, 41}, {63, 25}, {56.0f - in, 19}, {77, 30}, {78, 39}, {68, 51}, {67, 62}, {74, 51}, {75, 62}};
        Figure(c, l), Figure(c, r, IM_COL32(255, 235, 200, 255));
        const ImU32 glass = IM_COL32(255, 225, 120, 235);
        dl->AddRectFilled(c.at({44.0f + in, 9}), c.at({49.0f + in, 19}), glass, c.s(0.8f));
        dl->AddRectFilled(c.at({51.0f - in, 9}), c.at({56.0f - in, 19}), glass, c.s(0.8f));
        if (sw > 0.8f) Sparkle(c, {50, 7}, 4.0f, IM_COL32(255, 255, 255, 240));
        Ground(c, 62.5f, dim);
        break;
    }
    case 52: { // Surprise!: two jumping out, one leaping back
        const Pose a = {{24, 15}, {25, 22}, {25, 40}, {18, 16}, {14, 8}, {33, 16}, {37, 8}, {21, 51}, {19, 62}, {29, 51}, {31, 62}};
        Figure(c, Shift(a, 0, -sw * 3.0f)), Figure(c, Shift(a, 20, -(1.0f - sw) * 3.0f), IM_COL32(255, 240, 190, 255));
        const Pose b = {{79, 17}, {78, 24}, {75, 41}, {85, 20}, {89, 12}, {70, 22}, {67, 15}, {72, 52}, {68, 62}, {79, 51}, {82, 62}};
        Figure(c, Shift(b, sw * 3.0f, 0), IM_COL32(190, 225, 255, 255));
        Text(c, {62, 2}, "!!", accent, 13.0f);
        break;
    }
    case 53: { // Squad walk: a line of them coming on, the middle one nearest
        const Pose a = {{50, 12}, {50, 19}, {50, 38}, {45, 28}, {46, 37}, {55, 28}, {57, 36}, {47, 50}, {50, 62}, {54, 49}, {52, 61}};
        const Pose b = {{51, 12}, {51, 19}, {51, 38}, {46, 28}, {44, 36}, {56, 28}, {55, 37}, {48, 49}, {49, 61}, {55, 50}, {52, 62}};
        const Pose f = Mix(a, b, sw);
        for (float dx : {-34.0f, 34.0f}) Figure(c, Shift(Scale(f, {50, 62}, 0.72f), dx, 0), IM_COL32(205, 200, 215, 255));
        for (float dx : {-18.0f, 18.0f}) Figure(c, Shift(Scale(f, {50, 62}, 0.85f), dx, 0), IM_COL32(230, 225, 235, 255));
        Figure(c, f);
        Ground(c, 62.5f, dim);
        break;
    }
    case 54: { // Pillow fight: two swinging, feathers in the air
        const Pose l = {{30, 16}, {31, 23}, {32, 41}, {38, 20}, {44, 14}, {25, 30}, {24, 38}, {29, 52}, {27, 62}, {36, 52}, {37, 62}};
        const Pose r = {{70, 16}, {69, 23}, {68, 41}, {62, 28}, {55, 26}, {75, 30}, {76, 38}, {65, 52}, {64, 62}, {72, 52}, {74, 62}};
        Figure(c, l), Figure(c, r, IM_COL32(255, 230, 200, 255));
        dl->AddRectFilled(c.at({41.0f + sw * 4.0f, 6}), c.at({55.0f + sw * 4.0f, 15}), IM_COL32(245, 245, 255, 255), c.s(3.0f));
        dl->AddRectFilled(c.at({46, 24}), c.at({57, 32}), IM_COL32(255, 210, 225, 255), c.s(3.0f));
        for (int i = 0; i < 9; ++i) {
            const float f = std::fmod(tt * 0.35f + i * 0.13f, 1.0f), x = 16.0f + i * 9.0f + std::sin(tt * 2.0f + i) * 3.0f;
            dl->AddEllipseFilled(c.at({x, 4.0f + f * 58.0f}), ImVec2(c.s(1.6f), c.s(0.7f)), IM_COL32(255, 255, 255, 215), tt + i);
        }
        break;
    }
    case 55: { // Group routine: a V of five, all in the same pose
        const Pose a = {{46, 13}, {48, 20}, {51, 39}, {40, 17}, {37, 8}, {57, 25}, {63, 18}, {45, 50}, {42, 62}, {57, 49}, {60, 60}};
        const Pose b = {{54, 13}, {52, 20}, {49, 39}, {43, 25}, {37, 18}, {60, 17}, {63, 8}, {43, 49}, {40, 60}, {55, 50}, {58, 62}};
        const Pose f = Mix(a, b, sw);
        for (float dx : {-36.0f, 36.0f}) Figure(c, Shift(Scale(f, {50, 62}, 0.62f), dx, -6), IM_COL32(255, 205, 225, 255), 2.8f);
        for (float dx : {-19.0f, 19.0f}) Figure(c, Shift(Scale(f, {50, 62}, 0.76f), dx, -3), IM_COL32(255, 230, 240, 255), 3.0f);
        Figure(c, f);
        Note(c, {12.0f, 16.0f - sw * 4.0f}, IM_COL32(255, 215, 130, 230));
        break;
    }
    case 56: { // Dance battle: face to face, one down on the floor, the other waiting, sparks between
        const Pose l = {{33, 34}, {31, 40}, {29, 52}, {38, 44}, {40, 56}, {23, 42}, {19, 34}, {38, 54}, {46, 61}, {22, 56}, {14, 62}};
        const Pose l2 = {{31, 14}, {31, 21}, {31, 40}, {23, 16}, {20, 7}, {39, 16}, {42, 7}, {28, 51}, {25, 62}, {35, 51}, {38, 62}};
        const Pose r = {{70, 14}, {70, 21}, {70, 40}, {64, 27}, {68, 34}, {76, 27}, {72, 34}, {67, 51}, {66, 62}, {74, 51}, {75, 62}};
        Figure(c, Mix(l, l2, sw));
        Figure(c, r, IM_COL32(150, 255, 210, 255));
        Burst(c, {52, 30}, 4.0f + sw * 3.0f, 1.8f, 7, IM_COL32(150, 255, 210, 220), tt);
        Ground(c, 62.5f, dim);
        break;
    }
    case 57: { // Argue and laugh: nose to nose, pointing, zigzags turning into HA
        const Pose l = {{39, 15}, {38, 22}, {35, 41}, {44, 27}, {51, 23}, {29, 30}, {27, 38}, {33, 51}, {31, 62}, {39, 51}, {41, 62}};
        const Pose r = {{61, 15}, {62, 22}, {65, 41}, {56, 28}, {49, 31}, {71, 30}, {73, 38}, {62, 51}, {61, 62}, {68, 51}, {70, 62}};
        Figure(c, l), Figure(c, r, IM_COL32(255, 225, 170, 255));
        if (sw < 0.6f) {
            const ImVec2 z[] = {c.at({42, 7}), c.at({46, 3}), c.at({49, 9}), c.at({53, 3}), c.at({58, 7})};
            dl->AddPolyline(z, 5, accent, 0, c.s(1.3f));
        } else {
            Text(c, {41, 0}, "HA HA", accent, 9.0f);
        }
        break;
    }
    case 58: { // Slow dance: two close, turning, hearts rising
        const Pose l = {{44, 15}, {45, 22}, {44, 41}, {50, 28}, {57, 25}, {50, 31}, {55, 36}, {42, 51}, {41, 62}, {47, 51}, {48, 62}};
        const Pose r = {{56, 17}, {55, 24}, {56, 41}, {50, 30}, {46, 25}, {50, 33}, {46, 37}, {53, 51}, {52, 62}, {59, 51}, {60, 62}};
        const float d = (sw - 0.5f) * 4.0f;
        Figure(c, Shift(l, d, 0)), Figure(c, Shift(r, d, 0), IM_COL32(255, 200, 225, 255));
        for (int i = 0; i < 3; ++i) {
            const float f = std::fmod(tt * 0.4f + i * 0.33f, 1.0f), x = 34.0f + i * 16.0f, y = 34.0f - f * 26.0f;
            dl->AddCircleFilled(c.at({x, y}), c.s(1.5f), IM_COL32(255, 170, 200, static_cast<int>(230 * (1.0f - f))));
        }
        Ground(c, 62.5f, dim);
        break;
    }
    case 59: { // Reunion: running toward each other from far apart
        const float g = moving ? std::min(1.0f, std::fmod(tt * 0.5f, 1.3f)) : 0.45f;
        const Pose a = {{56, 13}, {54, 20}, {47, 37}, {45, 26}, {38, 21}, {62, 27}, {68, 19}, {57, 45}, {64, 56}, {42, 49}, {32, 44}};
        Figure(c, Shift(a, -34.0f + g * 24.0f, 0));
        Pose b = a;
        for (P* q : {&b.head, &b.neck, &b.hip, &b.le, &b.lh, &b.re, &b.rh, &b.lk, &b.lf, &b.rk, &b.rf}) q->x = 100.0f - q->x;
        Figure(c, Shift(b, 34.0f - g * 24.0f, 0), IM_COL32(255, 235, 190, 255));
        if (g > 0.95f) Sparkle(c, {50, 8}, 5.0f, accent);
        Ground(c, 62.5f, dim);
        break;
    }
    case 60: { // Photo gone wrong: a row tipping over like dominoes
        for (int i = 0; i < 4; ++i) {
            const float x = 22.0f + i * 19.0f, tip = (i + 1) * 0.11f * sw * 4.0f;
            const P feet{x, 62}, hipP{x + std::sin(tip) * 22.0f, 62.0f - std::cos(tip) * 22.0f}, neck{x + std::sin(tip) * 41.0f, 62.0f - std::cos(tip) * 41.0f},
                head{x + std::sin(tip) * 49.0f, 62.0f - std::cos(tip) * 49.0f};
            dl->AddLine(c.at(feet), c.at(hipP), kInk, c.s(3.2f));
            dl->AddLine(c.at(hipP), c.at(neck), kInk, c.s(4.0f));
            dl->AddCircleFilled(c.at(head), c.s(4.4f), kInk);
        }
        Ground(c, 62.5f, dim);
        if (moving && std::fmod(tt, 1.5f) < 0.12f) dl->AddRectFilled(c.at({0, 0}), c.at({100, 70}), IM_COL32(255, 255, 255, 170));
        break;
    }
    case 61: { // Team huddle: heads in a ring seen from above, hands stacked in the middle
        for (int i = 0; i < 6; ++i) {
            const float a = i * 1.0472f + sw * 0.15f;
            const P h{50.0f + std::cos(a) * 25.0f, 35.0f + std::sin(a) * 21.0f};
            dl->AddLine(c.at(h), c.at({50.0f + std::cos(a) * 7.0f, 35.0f + std::sin(a) * 6.0f}), IM_COL32(245, 240, 225, 255), c.s(2.6f));
            dl->AddCircleFilled(c.at(h), c.s(6.0f), IM_COL32(245, 240, 225, 255));
        }
        dl->AddCircleFilled(c.at({50, 35}), c.s(5.5f + sw * 1.5f), accent);
        break;
    }
    case 62: { // Band jam: a guitar, a singer, drums behind
        dl->AddCircle(c.at({50, 50}), c.s(11.0f), IM_COL32(255, 255, 255, 170), 24, c.s(1.2f));
        dl->AddLine(c.at({38, 42}), c.at({62, 42}), IM_COL32(255, 255, 255, 170), c.s(1.2f));
        const Pose g = {{24, 16}, {25, 23}, {25, 41}, {19, 31}, {30, 36}, {33, 30}, {38, 34}, {22, 51}, {20, 62}, {29, 51}, {31, 62}};
        Figure(c, g);
        dl->AddLine(c.at({18, 42}), c.at({44, 28}), IM_COL32(255, 180, 80, 255), c.s(2.2f));
        const Pose s = {{76, 16}, {75, 23}, {75, 41}, {70, 26}, {72, 18}, {82, 26}, {86.0f, 16.0f + sw * 4.0f}, {72, 51}, {70, 62}, {78, 51}, {80, 62}};
        Figure(c, s, IM_COL32(255, 215, 180, 255));
        for (int i = 0; i < 2; ++i) Note(c, {44.0f + i * 14.0f, 22.0f - sw * 4.0f - i * 5.0f}, IM_COL32(255, 150, 90, 235));
        break;
    }
    // ---- scenes from TV
    case 63: { // Breaking news: an anchor at a desk, a red banner
        Figure(c, At(50, 0.9f));
        dl->AddRectFilled(c.at({22, 40}), c.at({78, 56}), IM_COL32(190, 200, 220, 255), c.s(1.5f));
        Bar(c, 56, 70, IM_COL32(200, 30, 35, 255));
        Text(c, {8.0f - std::fmod(tt * 8.0f, 12.0f), 58}, "BREAKING NEWS", IM_COL32_WHITE, 9.5f);
        break;
    }
    case 64: { // Weather report: a map with a storm, an arm sweeping across it
        dl->AddRectFilled(c.at({36, 8}), c.at({94, 52}), IM_COL32(70, 170, 120, 255), c.s(2.0f));
        Cloud(c, {66.0f + sw * 8.0f, 24}, 1.3f, IM_COL32(235, 240, 250, 255));
        dl->AddTriangleFilled(c.at({64.0f + sw * 8.0f, 32}), c.at({60.0f + sw * 8.0f, 42}), c.at({67.0f + sw * 8.0f, 37}), IM_COL32(255, 230, 90, 255));
        const Pose f = {{22, 15}, {22, 22}, {22, 40}, {30, 22}, {38.0f + sw * 8.0f, 20}, {16, 30}, {15, 39}, {20, 51}, {19, 62}, {25, 51}, {26, 62}};
        Figure(c, f);
        break;
    }
    case 65: { // Talk show: a host at a desk, a guest on a couch, a skyline behind
        for (int i = 0; i < 8; ++i) dl->AddRectFilled(c.at({4.0f + i * 12.0f, 26.0f - (i * 37 % 11)}), c.at({13.0f + i * 12.0f, 44}), IM_COL32(50, 60, 110, 255));
        dl->AddRectFilled(c.at({52, 44}), c.at({92, 58}), IM_COL32(170, 60, 70, 255), c.s(3.0f));
        Figure(c, At(28, 0.8f));
        dl->AddRectFilled(c.at({12, 42}), c.at({44, 62}), IM_COL32(120, 80, 50, 255), c.s(1.5f));
        const Pose g = {{70, 21}, {70, 28}, {70, 46}, {64, 36}, {66.0f, 30.0f - sw * 4.0f}, {76, 36}, {79, 46}, {62, 48}, {60, 62}, {66, 48}, {64, 62}};
        Figure(c, g, IM_COL32(255, 230, 190, 255));
        break;
    }
    case 66: { // Sitcom: a couch, a door, HA HA from the audience
        dl->AddRectFilled(c.at({74, 16}), c.at({92, 62}), IM_COL32(130, 80, 40, 255));
        dl->AddRectFilled(c.at({10, 44}), c.at({46, 60}), IM_COL32(90, 120, 150, 255), c.s(3.0f));
        Figure(c, At(28, 0.75f), IM_COL32(255, 235, 200, 255));
        Figure(c, Shift(At(68), sw * 4.0f, 0));
        Text(c, {42, 3}, "HA HA", IM_COL32_WHITE, 9.5f);
        break;
    }
    case 67: { // Mockumentary: a close stare at the lens, a frame of the camera around it
        const Pose f = {{50, 24}, {50, 33}, {50, 66}, {38, 48}, {38, 62}, {62, 48}, {62, 62}, {48, 70}, {48, 80}, {52, 70}, {52, 80}};
        Figure(c, f, kInk, 4.6f);
        for (float x : {47.0f, 53.0f}) dl->AddCircleFilled(c.at({x + sw, 23}), c.s(0.9f), IM_COL32(40, 40, 45, 255));
        dl->AddLine(c.at({47, 28}), c.at({53, 28}), IM_COL32(40, 40, 45, 255), c.s(0.8f));
        for (P q : {P{10, 8}, P{90, 8}, P{10, 62}, P{90, 62}}) {
            dl->AddLine(c.at(q), c.at({q.x + (q.x < 50 ? 7.0f : -7.0f), q.y}), IM_COL32_WHITE, c.s(1.0f));
            dl->AddLine(c.at(q), c.at({q.x, q.y + (q.y < 35 ? 7.0f : -7.0f)}), IM_COL32_WHITE, c.s(1.0f));
        }
        break;
    }
    case 68: { // Soap opera: two turned to each other, a glass falling
        Figure(c, At(34)), Figure(c, At(66), IM_COL32(255, 205, 215, 255));
        const float y = 30.0f + sw * 26.0f;
        dl->AddTriangleFilled(c.at({49, y}), c.at({55, y}), c.at({52, y + 6.0f}), IM_COL32(230, 90, 110, 255));
        dl->AddLine(c.at({52, y + 6.0f}), c.at({52, y + 11.0f}), IM_COL32(240, 240, 250, 255), c.s(0.8f));
        Text(c, {47, 2}, "!", accent, 14.0f);
        break;
    }
    case 69: { // Hospital drama: a gurney pushed along fast, a heartbeat line
        dl->AddRectFilled(c.at({24, 40}), c.at({72, 45}), IM_COL32(235, 240, 245, 255), c.s(1.0f));
        dl->AddCircleFilled(c.at({34, 36}), c.s(4.0f), IM_COL32(235, 240, 245, 255));
        for (float x : {30.0f, 66.0f}) dl->AddLine(c.at({x, 45}), c.at({x, 60}), dim, c.s(1.0f)), dl->AddCircleFilled(c.at({x, 61}), c.s(1.6f), dim);
        const Pose a = {{80, 15}, {79, 22}, {80, 40}, {74, 31}, {70, 38}, {84, 30}, {72, 40}, {78, 51}, {75, 62}, {83, 51}, {86, 62}};
        Figure(c, Shift(a, sw * 2.0f, 0), IM_COL32(170, 240, 225, 255));
        const ImVec2 beat[] = {c.at({6, 18}), c.at({18, 18}), c.at({22, 8}), c.at({26, 26}), c.at({30, 18}), c.at({44, 18})};
        dl->AddPolyline(beat, 6, IM_COL32(120, 255, 170, 255), 0, c.s(1.3f));
        break;
    }
    case 70: { // Courtroom: a judge up high, a gavel coming down, a lawyer pointing
        dl->AddRectFilled(c.at({54, 34}), c.at({96, 62}), IM_COL32(120, 75, 40, 255), c.s(1.5f));
        dl->AddCircleFilled(c.at({74, 24}), c.s(5.0f), kInk);
        dl->AddRectFilled(c.at({66, 29}), c.at({82, 36}), IM_COL32(25, 25, 30, 255));
        const float a = -0.4f + sw * 0.8f;
        dl->AddLine(c.at({84, 34}), c.at({84.0f + std::cos(a) * 9.0f, 34.0f + std::sin(a) * 9.0f}), IM_COL32(200, 150, 90, 255), c.s(2.0f));
        const Pose f = {{30, 14}, {30, 21}, {30, 40}, {38, 23}, {49, 22}, {24, 30}, {23, 39}, {27, 51}, {26, 62}, {33, 51}, {34, 62}};
        Figure(c, f);
        break;
    }
    case 71: { // Teen drama: lockers, a figure walking, books dropping
        for (int i = 0; i < 9; ++i) dl->AddRect(c.at({4.0f + i * 10.5f, 6}), c.at({13.0f + i * 10.5f, 54}), IM_COL32(255, 255, 255, 70), 0.0f, 0, c.s(0.8f));
        const Pose a = {{50, 12}, {50, 19}, {50, 38}, {45, 28}, {46, 37}, {55, 28}, {57, 36}, {47, 50}, {50, 62}, {54, 49}, {52, 61}};
        const Pose b = {{51, 12}, {51, 19}, {51, 38}, {46, 28}, {44, 36}, {56, 28}, {55, 37}, {48, 49}, {49, 61}, {55, 50}, {52, 62}};
        Figure(c, Mix(a, b, sw));
        for (int i = 0; i < 2; ++i) Box(c, {66.0f + i * 7.0f, 34.0f + sw * 24.0f - i * 4.0f}, 2.6f, tt + i, IM_COL32(255, 190, 210, 255));
        break;
    }
    case 72: { // Period drama: a bow in a candlelit room
        for (int i = 0; i < 3; ++i) {
            const float x = 20.0f + i * 30.0f;
            dl->AddLine(c.at({x, 12}), c.at({x, 4}), dim, c.s(0.8f));
            dl->AddCircleFilled(c.at({x, 11.0f}), c.s(1.6f + (moving ? std::sin(tt * 9.0f + i) * 0.3f : 0.0f)), IM_COL32(255, 220, 130, 255));
        }
        const Pose b = {{38, 26}, {36, 30}, {33, 44}, {41, 34}, {44, 42}, {38, 37}, {42, 45}, {33, 54}, {31, 62}, {36, 54}, {37, 62}};
        Figure(c, Mix(At(36), b, sw));
        dl->AddTriangleFilled(c.at({68, 28}), c.at({54, 62}), c.at({82, 62}), IM_COL32(190, 120, 170, 255));
        Figure(c, At(68), IM_COL32(255, 225, 200, 255));
        dl->AddTriangleFilled(c.at({68, 32}), c.at({56, 62}), c.at({80, 62}), IM_COL32(190, 120, 170, 255));
        break;
    }
    case 73: { // Interrogation: a hanging lamp, a table, two across it
        dl->AddLine(c.at({50, 0}), c.at({50, 10}), dim, c.s(0.8f));
        dl->AddTriangleFilled(c.at({44, 16}), c.at({56, 16}), c.at({50, 9}), IM_COL32(150, 150, 160, 255));
        dl->AddTriangleFilled(c.at({44, 16}), c.at({56, 16}), c.at({82, 50}), IM_COL32(255, 240, 180, 26));
        dl->AddTriangleFilled(c.at({44, 16}), c.at({18, 50}), c.at({82, 50}), IM_COL32(255, 240, 180, 26));
        Figure(c, At(26, 0.8f)), Figure(c, At(74, 0.8f), IM_COL32(190, 195, 205, 255));
        dl->AddRectFilled(c.at({16, 44}), c.at({84, 48}), IM_COL32(120, 125, 135, 255));
        dl->AddRectFilled(c.at({42.0f + sw * 8.0f, 41}), c.at({51.0f + sw * 8.0f, 44}), IM_COL32(240, 240, 240, 255));
        break;
    }
    case 74: { // Crime scene: police tape, red and blue lights
        const bool red = !moving || std::fmod(tt, 0.5f) < 0.25f;
        dl->AddCircleFilled(c.at({14, 16}), c.s(11.0f), red ? IM_COL32(255, 50, 50, 90) : IM_COL32(60, 90, 255, 90));
        dl->AddCircleFilled(c.at({86, 16}), c.s(11.0f), red ? IM_COL32(60, 90, 255, 90) : IM_COL32(255, 50, 50, 90));
        Figure(c, Shift(At(50), 0, sw * 2.0f));
        dl->AddQuadFilled(c.at({0, 30}), c.at({100, 38}), c.at({100, 44}), c.at({0, 36}), IM_COL32(245, 205, 40, 255));
        for (int i = 0; i < 6; ++i) dl->AddQuadFilled(c.at({6.0f + i * 17.0f, 31.0f + i * 1.4f}), c.at({11.0f + i * 17.0f, 31.0f + i * 1.4f}), c.at({9.0f + i * 17.0f, 37.0f + i * 1.4f}), c.at({4.0f + i * 17.0f, 37.0f + i * 1.4f}), IM_COL32(30, 30, 30, 255));
        break;
    }
    case 75: { // True crime: half a face lit from one side, an old photo
        dl->AddCircleFilled(c.at({42, 32}), c.s(19.0f), IM_COL32(205, 205, 215, 255));
        dl->AddRectFilled(c.at({42, 8}), c.at({64, 56}), look.bg);
        dl->AddCircleFilled(c.at({35, 29}), c.s(1.4f), IM_COL32(25, 25, 30, 255));
        dl->AddRectFilled(c.at({66, 14}), c.at({88, 36}), IM_COL32(225, 220, 205, 255));
        dl->AddRectFilled(c.at({68, 16}), c.at({86, 31}), IM_COL32(60, 60, 65, 255));
        Figure(c, Scale(Shift(At(77), 0, -30.0f), {77, 31}, 0.3f), IM_COL32(200, 200, 205, 255), 1.6f);
        break;
    }
    case 76: { // Spy thriller: a figure at a cufflink, chips and a card
        const Pose f = {{46, 13}, {46, 20}, {46, 39}, {54, 26}, {47.0f, 24.0f + sw}, {38, 27}, {42, 26}, {43, 50}, {42, 62}, {50, 50}, {51, 62}};
        Figure(c, f, IM_COL32(235, 235, 245, 255));
        dl->AddLine(c.at({46, 21}), c.at({46, 24}), IM_COL32(20, 20, 25, 255), c.s(1.8f));
        for (int i = 0; i < 4; ++i) dl->AddEllipseFilled(c.at({74, 60.0f - i * 2.2f}), ImVec2(c.s(6.0f), c.s(1.8f)), i % 2 ? IM_COL32(200, 40, 40, 255) : IM_COL32(240, 240, 240, 255));
        Sparkle(c, {18, 16}, 2.5f + sw, accent);
        break;
    }
    case 77: { // Defuse the bomb: a timer, two wires, cutters closing
        dl->AddRectFilled(c.at({22, 16}), c.at({78, 42}), IM_COL32(20, 22, 26, 255), c.s(2.0f));
        Text(c, {27, 18}, moving && std::fmod(tt, 1.0f) < 0.5f ? "00:01" : "00:02", IM_COL32(255, 50, 50, 255), 17.0f);
        dl->AddBezierCubic(c.at({34, 42}), c.at({34, 54}), c.at({66, 52}), c.at({66, 42}), IM_COL32(70, 130, 255, 255), c.s(1.6f));
        dl->AddBezierCubic(c.at({44, 42}), c.at({44, 62}), c.at({60, 60}), c.at({60, 42}), IM_COL32(255, 70, 70, 255), c.s(1.6f));
        dl->AddLine(c.at({62.0f, 66}), c.at({50.0f - sw * 2.0f, 50}), kInk, c.s(1.8f));
        dl->AddLine(c.at({72.0f, 60}), c.at({50.0f + sw * 2.0f, 50}), kInk, c.s(1.8f));
        break;
    }
    case 78: { // Spaceship bridge: stars through a window, a captain, red alert
        dl->AddRectFilled(c.at({8, 6}), c.at({92, 34}), IM_COL32(5, 6, 16, 255), c.s(3.0f));
        for (int i = 0; i < 16; ++i) dl->AddCircleFilled(c.at({10.0f + (i * 37 % 80), 8.0f + (i * 53 % 24)}), c.s(0.5f), IM_COL32_WHITE);
        if (!moving || std::fmod(tt, 0.8f) < 0.4f) Bar(c, 0, 3, IM_COL32(255, 40, 40, 255));
        Figure(c, At(50, 0.8f));
        for (float x : {14.0f, 72.0f}) dl->AddRectFilled(c.at({x, 50}), c.at({x + 15.0f, 62}), IM_COL32(50, 60, 100, 255), c.s(1.0f)), dl->AddRectFilled(c.at({x + 2.0f, 52}), c.at({x + 13.0f, 56}), IM_COL32(90, 220, 255, 255));
        break;
    }
    case 79: { // Throne room: a throne of blades, torches, a raised sword
        for (float x : {8.0f, 92.0f}) {
            dl->AddTriangleFilled(c.at({x - 3.0f, 26}), c.at({x + 3.0f, 26}), c.at({x, 15.0f - sw * 3.0f}), IM_COL32(255, 150, 50, 255));
            dl->AddLine(c.at({x, 26}), c.at({x, 40}), dim, c.s(1.2f));
        }
        for (int i = 0; i < 7; ++i) dl->AddLine(c.at({30.0f + i * 6.7f, 48}), c.at({32.0f + i * 5.6f, 8.0f + (i * 29 % 13)}), IM_COL32(130, 130, 145, 255), c.s(1.6f));
        const Pose f = {{50, 17}, {50, 24}, {50, 42}, {43, 32}, {42, 41}, {56, 22}, {58.0f, 14.0f - sw * 2.0f}, {47, 52}, {46, 62}, {53, 52}, {54, 62}};
        Figure(c, f);
        dl->AddLine(c.at({58.0f, 14.0f - sw * 2.0f}), c.at({60.0f, 1.0f - sw * 2.0f}), IM_COL32(230, 235, 245, 255), c.s(1.5f));
        break;
    }
    case 80: { // Zombie show: a survivor with a bat, arms reaching behind
        const ImU32 z = IM_COL32(150, 175, 120, 255);
        for (int i = 0; i < 3; ++i) {
            const float x = 70.0f + i * 12.0f - sw * 3.0f;
            const Pose f = {{x, 22}, {x - 1.0f, 28}, {x, 44}, {x - 7.0f, 30}, {x - 14.0f, 29}, {x - 6.0f, 34}, {x - 13.0f, 33}, {x - 2.0f, 53}, {x - 3.0f, 62}, {x + 2.0f, 53}, {x + 3.0f, 62}};
            Figure(c, f, z, 2.8f);
        }
        const Pose s = {{30, 14}, {30, 21}, {30, 40}, {24, 28}, {19, 22}, {36, 28}, {30, 34}, {28, 51}, {26, 62}, {33, 51}, {35, 62}};
        Figure(c, s);
        dl->AddLine(c.at({19, 22}), c.at({12, 9}), IM_COL32(190, 140, 90, 255), c.s(2.2f));
        break;
    }
    case 81: { // Haunted house: a flashlight beam, a pale shape behind
        dl->AddTriangleFilled(c.at({34, 30}), c.at({96, 12}), c.at({96, 50}), IM_COL32(255, 245, 190, 60));
        Figure(c, At(28));
        dl->AddCircleFilled(c.at({10, 22}), c.s(5.0f + sw), IM_COL32(230, 235, 250, 150));
        dl->AddRectFilled(c.at({5.0f - sw, 22}), c.at({15.0f + sw, 60}), IM_COL32(230, 235, 250, 110), c.s(4.0f));
        for (float x : {8.0f, 12.0f}) dl->AddCircleFilled(c.at({x, 21}), c.s(0.9f), IM_COL32(10, 10, 20, 255));
        break;
    }
    case 82: { // Western showdown: two at each end of a street, a sun, a tumbleweed
        dl->AddCircleFilled(c.at({50, 16}), c.s(9.0f), IM_COL32(255, 240, 170, 255));
        Bar(c, 50, 70, IM_COL32(150, 100, 50, 255));
        Figure(c, At(16, 0.8f), IM_COL32(40, 25, 20, 255)), Figure(c, At(84, 0.8f), IM_COL32(40, 25, 20, 255));
        for (float x : {16.0f, 84.0f}) dl->AddLine(c.at({x - 6.0f, 20}), c.at({x + 6.0f, 20}), IM_COL32(40, 25, 20, 255), c.s(1.8f));
        const float x = moving ? std::fmod(tt * 14.0f, 80.0f) + 10.0f : 44.0f;
        dl->AddCircle(c.at({x, 58}), c.s(4.0f), IM_COL32(90, 60, 30, 255), 8, c.s(1.0f));
        dl->AddCircle(c.at({x, 58}), c.s(2.2f), IM_COL32(90, 60, 30, 255), 6, c.s(0.8f));
        break;
    }
    case 83: { // Confessional: alone on a stool, a whisper
        const Pose f = {{46, 15}, {46, 22}, {46, 41}, {40, 31}, {41, 40}, {53, 28}, {54.0f, 21.0f}, {53, 46}, {54, 60}, {39, 46}, {38, 60}};
        Figure(c, Shift(f, sw * 2.0f, 0));
        dl->AddLine(c.at({46, 42}), c.at({46, 62}), dim, c.s(1.4f));
        Text(c, {60.0f, 8.0f - sw * 2.0f}, "psst", IM_COL32_WHITE, 8.5f);
        break;
    }
    case 84: { // Dating show: a held-out rose, candles
        for (int i = 0; i < 5; ++i) {
            const float x = 12.0f + i * 19.0f;
            dl->AddRectFilled(c.at({x - 1.2f, 56}), c.at({x + 1.2f, 66}), IM_COL32(240, 235, 220, 255));
            dl->AddCircleFilled(c.at({x, 54.0f - (moving ? std::sin(tt * 8.0f + i) * 0.5f : 0.0f)}), c.s(1.4f), IM_COL32(255, 210, 110, 255));
        }
        const Pose f = {{32, 14}, {32, 21}, {32, 40}, {40, 26}, {50.0f + sw * 4.0f, 24}, {26, 30}, {25, 39}, {29, 50}, {28, 60}, {35, 50}, {36, 60}};
        Figure(c, f), Figure(c, At(76, 0.9f), IM_COL32(255, 215, 215, 255));
        dl->AddLine(c.at({50.0f + sw * 4.0f, 24}), c.at({56.0f + sw * 4.0f, 18}), IM_COL32(70, 170, 80, 255), c.s(1.0f));
        dl->AddCircleFilled(c.at({57.0f + sw * 4.0f, 16}), c.s(2.6f), IM_COL32(230, 30, 60, 255));
        break;
    }
    case 85: { // Talent audition: a spotlight, three judges from behind, golden bits falling
        dl->AddTriangleFilled(c.at({50, 0}), c.at({32, 50}), c.at({68, 50}), IM_COL32(255, 245, 200, 50));
        Figure(c, Scale(At(50), {50, 48}, 0.72f), kInk, 2.8f);
        for (int i = 0; i < 3; ++i) {
            const float x = 24.0f + i * 26.0f;
            dl->AddCircleFilled(c.at({x, 57}), c.s(5.0f), IM_COL32(15, 10, 30, 255));
            dl->AddRectFilled(c.at({x - 8.0f, 61}), c.at({x + 8.0f, 70}), IM_COL32(15, 10, 30, 255));
        }
        for (int i = 0; i < 9; ++i) dl->AddCircleFilled(c.at({10.0f + i * 10.0f, std::fmod(tt * 16.0f + i * 13.0f, 50.0f)}), c.s(0.8f), IM_COL32(255, 215, 110, 240));
        break;
    }
    case 86: { // Game show: podiums, a big buzzer, lights chasing round
        for (int i = 0; i < 14; ++i) {
            const bool on = (i + static_cast<int>(tt * 8.0f)) % 3 == 0;
            dl->AddCircleFilled(c.at({4.0f + i * 7.1f, 5}), c.s(1.5f), on ? IM_COL32(255, 230, 90, 255) : IM_COL32(120, 110, 60, 255));
        }
        Figure(c, Scale(At(50), {50, 46}, 0.72f));
        dl->AddQuadFilled(c.at({34, 46}), c.at({66, 46}), c.at({70, 68}), c.at({30, 68}), IM_COL32(225, 60, 90, 255));
        dl->AddEllipseFilled(c.at({50, 45.0f + sw * 1.5f}), ImVec2(c.s(7.0f), c.s(2.6f)), IM_COL32(255, 230, 90, 255));
        break;
    }
    case 87: { // Cooking contest: a clock running out, a plate slammed down
        dl->AddCircleFilled(c.at({78, 18}), c.s(11.0f), IM_COL32(245, 245, 240, 255));
        const float a = -1.57f + tt * 3.0f;
        dl->AddLine(c.at({78, 18}), c.at({78.0f + std::cos(a) * 8.5f, 18.0f + std::sin(a) * 8.5f}), IM_COL32(220, 40, 40, 255), c.s(1.2f));
        const Pose f = {{32, 14}, {32, 21}, {32, 40}, {24, 16}, {20, 7}, {40, 16}, {44, 7}, {29, 51}, {27, 62}, {36, 51}, {37, 62}};
        Figure(c, f);
        dl->AddRectFilled(c.at({26, 8.0f}), c.at({38, 10.0f}), IM_COL32_WHITE);
        dl->AddRectFilled(c.at({50, 54}), c.at({96, 66}), IM_COL32(200, 200, 205, 255));
        dl->AddEllipseFilled(c.at({72, 52.0f - sw}), ImVec2(c.s(11.0f), c.s(2.6f)), IM_COL32_WHITE);
        break;
    }
    case 88: { // Home makeover: a house behind, hands dropping from eyes, sparkles
        dl->AddTriangleFilled(c.at({58, 24}), c.at({78, 8}), c.at({98, 24}), IM_COL32(200, 80, 60, 255));
        dl->AddRectFilled(c.at({62, 24}), c.at({94, 56}), IM_COL32(245, 235, 210, 255));
        dl->AddRectFilled(c.at({74, 38}), c.at({82, 56}), IM_COL32(120, 80, 50, 255));
        const Pose a = {{28, 15}, {28, 22}, {28, 40}, {22, 21}, {27, 16}, {34, 21}, {29, 16}, {25, 51}, {24, 62}, {31, 51}, {32, 62}};
        const Pose b = {{28, 15}, {28, 22}, {28, 40}, {21, 16}, {17, 7}, {35, 16}, {39, 7}, {25, 51}, {24, 62}, {31, 51}, {32, 62}};
        Figure(c, Mix(a, b, sw));
        for (int i = 0; i < 3; ++i) Sparkle(c, {66.0f + i * 12.0f, 16.0f + i * 14.0f}, 2.0f + sw * 1.5f, accent);
        break;
    }
    case 89: { // Nature documentary: seen through tall grass, a camera lens at the side
        Figure(c, At(58, 0.75f));
        for (int i = 0; i < 22; ++i) {
            const float x = i * 4.8f, h = 26.0f + (i * 7 % 13), lean = std::sin(tt * 2.0f + i) * 2.0f;
            dl->AddLine(c.at({x, 70}), c.at({x + 3.0f + lean, 70.0f - h}), IM_COL32(120, 190, 70, 255), c.s(1.2f));
        }
        dl->AddCircleFilled(c.at({12, 14}), c.s(8.0f), IM_COL32(20, 20, 25, 255));
        dl->AddCircleFilled(c.at({12, 14}), c.s(4.0f), IM_COL32(70, 100, 150, 255));
        break;
    }
    case 90: { // History documentary: columns, a figure with open hands, the sun low
        dl->AddCircleFilled(c.at({82, 18}), c.s(8.0f), IM_COL32(255, 225, 130, 255));
        for (int i = 0; i < 3; ++i) {
            const float x = 54.0f + i * 15.0f;
            dl->AddRectFilled(c.at({x, 20.0f + (i == 2 ? 14.0f : 0.0f)}), c.at({x + 7.0f, 62}), IM_COL32(235, 220, 185, 255));
        }
        dl->AddRectFilled(c.at({50, 16}), c.at({78, 20}), IM_COL32(235, 220, 185, 255));
        const Pose f = {{24, 15}, {24, 22}, {24, 40}, {16, 29}, {13.0f, 25.0f - sw * 2.0f}, {32, 29}, {36.0f, 25.0f - sw * 2.0f}, {21, 51}, {20, 62}, {27, 51}, {28, 62}};
        Figure(c, f);
        Ground(c, 62.5f, dim);
        break;
    }
    case 91: { // Sports highlight: a kick, a ball into the net, the crowd
        for (int i = 0; i < 26; ++i) dl->AddCircleFilled(c.at({2.0f + i * 3.9f, 5.0f + (i * 11 % 8)}), c.s(1.3f), IM_COL32(255, 255, 255, 60 + (i * 40) % 150));
        dl->AddRect(c.at({72, 22}), c.at({98, 62}), IM_COL32_WHITE, 0.0f, 0, c.s(1.2f));
        for (int i = 1; i < 5; ++i) dl->AddLine(c.at({72.0f + i * 5.2f, 22}), c.at({72.0f + i * 5.2f, 62}), IM_COL32(255, 255, 255, 70), c.s(0.5f));
        const Pose a = {{30, 16}, {31, 23}, {34, 40}, {26, 30}, {21, 25}, {40, 29}, {45, 24}, {31, 51}, {30, 62}, {44, 44}, {53, 38}};
        Figure(c, a);
        dl->AddCircleFilled(c.at({56.0f + sw * 30.0f, 38.0f - std::sin(sw * 3.14f) * 10.0f}), c.s(3.0f), IM_COL32_WHITE);
        break;
    }
    case 92: { // Wrestling promo: ropes, a mic, fire at the sides
        for (float x : {6.0f, 94.0f})
            for (int i = 0; i < 3; ++i) dl->AddTriangleFilled(c.at({x - 5.0f + i * 5.0f, 52}), c.at({x - 1.0f + i * 5.0f, 52}), c.at({x - 3.0f + i * 5.0f, 34.0f - std::sin(tt * 9.0f + i) * 5.0f}), IM_COL32(255, 140, 40, 235));
        const Pose f = {{50, 14}, {50, 21}, {50, 40}, {42, 18}, {37, 9}, {58, 26}, {53, 19}, {47, 51}, {45, 62}, {54, 51}, {56, 62}};
        Figure(c, Shift(f, 0, -sw * 1.5f));
        for (int i = 0; i < 3; ++i) dl->AddLine(c.at({0, 44.0f + i * 7.0f}), c.at({100, 44.0f + i * 7.0f}), IM_COL32(255, 80, 80, 200), c.s(0.9f));
        Ground(c, 62.5f, dim);
        break;
    }
    case 93: { // Kids show: a figure waving, a sock puppet, a rainbow
        const ImU32 band[] = {IM_COL32(240, 80, 80, 200), IM_COL32(250, 190, 70, 200), IM_COL32(110, 210, 110, 200)};
        for (int i = 0; i < 3; ++i) Arc(c, {50, 66}, 46.0f - i * 5.0f, 3.14159f, 6.28318f, band[i], 2.6f, false);
        const Pose f = {{36, 15}, {36, 22}, {36, 40}, {30, 30}, {29, 39}, {44, 18}, {46.0f + sw * 4.0f, 8}, {33, 51}, {32, 62}, {39, 51}, {40, 62}};
        Figure(c, f);
        const float y = 36.0f - sw * 3.0f;
        dl->AddRectFilled(c.at({62, y}), c.at({76, 64}), IM_COL32(255, 150, 190, 255), c.s(5.0f));
        for (float x : {66.0f, 72.0f}) dl->AddCircleFilled(c.at({x, y + 6.0f}), c.s(2.0f), IM_COL32_WHITE), dl->AddCircleFilled(c.at({x, y + 6.0f}), c.s(0.9f), IM_COL32(30, 30, 40, 255));
        break;
    }
    case 94: { // Infomercial: a grey half and a bright half, the product, a price starburst
        dl->AddRectFilled(c.at({0, 0}), c.at({44, 70}), IM_COL32(105, 105, 110, 255));
        const Pose a = {{22, 17}, {23, 24}, {24, 41}, {16, 33}, {14, 42}, {31, 33}, {33, 42}, {21, 52}, {19, 62}, {28, 52}, {30, 62}};
        Figure(c, a, IM_COL32(215, 215, 220, 255));
        Bottle(c, {66, 62}, 1.1f, IM_COL32(245, 245, 250, 255));
        Burst(c, {86, 18}, 11.0f + sw * 1.5f, 7.0f, 10, IM_COL32(255, 225, 60, 255), tt * 0.5f);
        Text(c, {79, 13}, "$19", IM_COL32(180, 20, 20, 255), 8.5f);
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
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), mode == 8 ? IM_COL32(120, 60, 50, 255) : kBg[std::clamp(mode, 0, 5)], r, ImDrawFlags_RoundCornersTop);
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
    case 8: { // people & outfits: a photo, and the person in it swapped for another, their outfit changing colour
        Frame(c, {10, 8}, {42, 42}, IM_COL32(120, 190, 255, 255), IM_COL32(60, 140, 80, 255), true, tt);
        Frame(c, {58, 8}, {90, 42}, IM_COL32(120, 190, 255, 255), IM_COL32(60, 140, 80, 255), true, tt);
        auto person = [&](float x, ImU32 skin, ImU32 cloth) {
            dl->AddCircleFilled(c.at({x, 19}), c.s(3.4f), skin, 20);
            dl->AddRectFilled(c.at({x - 5.5f, 23.5f}), c.at({x + 5.5f, 40}), cloth, c.s(3.0f), ImDrawFlags_RoundCornersTop);
        };
        person(26, IM_COL32(240, 200, 170, 255), IM_COL32(70, 80, 100, 255));
        const float k = moving ? 0.5f + 0.5f * std::sin(tt * 3.0f) : 1.0f; // (the new outfit's colour comes and goes)
        person(74, IM_COL32(200, 150, 120, 255), IM_COL32(static_cast<int>(120 + 110 * k), 60, static_cast<int>(140 - 40 * k), 255));
        Arrow(c, {45, 25}, {55, 25}, white);
        Sparkle(c, {83, 13}, 2.0f, IM_COL32(255, 240, 160, 230));
        break;
    }
    default:
        break;
    }
    dl->PopClipRect();
}

// Make a music video's cards: places (0-11), looks (12-21), genres (22-33), and hearts (34, the love story)
void DrawMusicArt(ImDrawList* dl, int art, ImVec2 p, float w, float h, float t) {
    const Canvas c{dl, p, w / 100.0f, h / 50.0f};
    const float tt = t >= 0.0f ? t : 0.0f;
    struct Sky { ImU32 top, bottom; };
    static const Sky kSky[] = {
        {IM_COL32(20, 12, 60, 255), IM_COL32(120, 30, 110, 255)},   {IM_COL32(12, 16, 50, 255), IM_COL32(45, 50, 110, 255)},   {IM_COL32(250, 120, 70, 255), IM_COL32(255, 200, 110, 255)},
        {IM_COL32(30, 90, 70, 255), IM_COL32(150, 200, 120, 255)},  {IM_COL32(240, 150, 70, 255), IM_COL32(250, 215, 150, 255)}, {IM_COL32(15, 10, 30, 255), IM_COL32(70, 20, 90, 255)},
        {IM_COL32(10, 8, 25, 255), IM_COL32(30, 15, 70, 255)},      {IM_COL32(50, 52, 60, 255), IM_COL32(95, 95, 100, 255)},   {IM_COL32(110, 170, 230, 255), IM_COL32(210, 230, 245, 255)},
        {IM_COL32(90, 150, 220, 255), IM_COL32(220, 235, 250, 255)}, {IM_COL32(5, 5, 20, 255), IM_COL32(25, 20, 60, 255)},     {IM_COL32(80, 110, 160, 255), IM_COL32(200, 215, 235, 255)},
        {IM_COL32(25, 30, 40, 255), IM_COL32(70, 60, 50, 255)},     {IM_COL32(15, 5, 40, 255), IM_COL32(70, 10, 100, 255)},    {IM_COL32(20, 20, 20, 255), IM_COL32(120, 120, 120, 255)},
        {IM_COL32(35, 25, 70, 255), IM_COL32(90, 50, 110, 255)},    {IM_COL32(255, 130, 170, 255), IM_COL32(120, 180, 255, 255)}, {IM_COL32(255, 190, 220, 255), IM_COL32(190, 210, 255, 255)},
        {IM_COL32(50, 40, 40, 255), IM_COL32(95, 70, 55, 255)},     {IM_COL32(30, 25, 20, 255), IM_COL32(90, 70, 30, 255)},    {IM_COL32(190, 90, 40, 255), IM_COL32(240, 180, 90, 255)},
        {IM_COL32(10, 5, 10, 255), IM_COL32(70, 10, 20, 255)},
        {IM_COL32(255, 90, 170, 255), IM_COL32(255, 180, 90, 255)}, {IM_COL32(30, 30, 40, 255), IM_COL32(200, 150, 40, 255)},  {IM_COL32(20, 20, 25, 255), IM_COL32(150, 30, 40, 255)},
        {IM_COL32(60, 20, 70, 255), IM_COL32(180, 70, 120, 255)},   {IM_COL32(60, 30, 20, 255), IM_COL32(200, 70, 30, 255)},   {IM_COL32(15, 15, 15, 255), IM_COL32(80, 80, 85, 255)},
        {IM_COL32(20, 200, 200, 255), IM_COL32(90, 60, 220, 255)},  {IM_COL32(230, 60, 40, 255), IM_COL32(250, 190, 40, 255)}, {IM_COL32(250, 110, 190, 255), IM_COL32(110, 170, 255, 255)},
        {IM_COL32(120, 80, 40, 255), IM_COL32(220, 170, 90, 255)},  {IM_COL32(70, 90, 140, 255), IM_COL32(190, 200, 225, 255)}, {IM_COL32(20, 40, 50, 255), IM_COL32(40, 110, 110, 255)},
        {IM_COL32(150, 30, 70, 255), IM_COL32(250, 120, 140, 255)},
    };
    const Sky& sky = kSky[std::clamp(art, 0, 34)];
    dl->AddRectFilledMultiColor(p, ImVec2(p.x + w, p.y + h), sky.top, sky.top, sky.bottom, sky.bottom);
    dl->PushClipRect(p, ImVec2(p.x + w, p.y + h), true);
    const ImU32 ink = IM_COL32(10, 10, 20, 235), white = IM_COL32(250, 250, 255, 240), gold = IM_COL32(255, 220, 110, 255);
    switch (art) {
    case 0: { // a neon city street
        const float hs[] = {26, 34, 22, 38, 28, 33, 24, 30};
        for (int i = 0; i < 8; ++i) {
            dl->AddRectFilled(c.at({i * 13.0f - 2.0f, 50.0f - hs[i]}), c.at({i * 13.0f + 10.0f, 50.0f}), ink);
            for (int y = 0; y < 4; ++y) dl->AddRectFilled(c.at({i * 13.0f + 1.0f, 50.0f - hs[i] + 3.0f + y * 6.0f}), c.at({i * 13.0f + 4.0f, 50.0f - hs[i] + 5.0f + y * 6.0f}), i % 2 ? IM_COL32(255, 80, 180, 230) : IM_COL32(80, 220, 255, 230));
        }
        break;
    }
    case 1: { // a rooftop at night
        dl->AddCircleFilled(c.at({74, 13}), c.s(7), IM_COL32(240, 240, 220, 255), 24);
        dl->AddCircleFilled(c.at({77, 11}), c.s(6.5f), sky.top, 24);
        dl->AddRectFilled(c.at({0, 38}), c.at({100, 50}), ink);
        for (int i = 0; i < 10; ++i) dl->AddLine(c.at({i * 11.0f, 32}), c.at({i * 11.0f, 38}), ink, c.s(0.8f));
        dl->AddLine(c.at({0, 32}), c.at({100, 32}), ink, c.s(1.0f));
        for (int i = 0; i < 6; ++i) dl->AddCircleFilled(c.at({8.0f + i * 15.0f, 8.0f + (i % 3) * 7.0f}), c.s(0.6f), white);
        break;
    }
    case 2: { // a beach at sunset
        dl->AddCircleFilled(c.at({50, 28}), c.s(12), IM_COL32(255, 245, 190, 255), 32);
        dl->AddRectFilled(c.at({0, 32}), c.at({100, 42}), IM_COL32(40, 90, 150, 255));
        for (int i = 0; i < 4; ++i) dl->AddLine(c.at({8.0f + i * 22.0f + std::sin(tt * 2.0f + i) * 2.0f, 36}), c.at({24.0f + i * 22.0f + std::sin(tt * 2.0f + i) * 2.0f, 36}), IM_COL32(255, 255, 255, 170), c.s(0.8f));
        dl->AddRectFilled(c.at({0, 42}), c.at({100, 50}), IM_COL32(235, 205, 150, 255));
        break;
    }
    case 3: { // a forest
        for (int i = 0; i < 7; ++i) {
            const float x = 6.0f + i * 15.0f, b = 46.0f - (i % 2) * 3.0f;
            dl->AddRectFilled(c.at({x - 1.0f, b - 6.0f}), c.at({x + 1.0f, b + 4.0f}), IM_COL32(70, 45, 30, 255));
            for (int l = 0; l < 3; ++l) dl->AddTriangleFilled(c.at({x, b - 34.0f + l * 9.0f}), c.at({x - 8.0f - l * 1.5f, b - 18.0f + l * 7.0f}), c.at({x + 8.0f + l * 1.5f, b - 18.0f + l * 7.0f}), IM_COL32(18, 60 + l * 14, 42, 255));
        }
        break;
    }
    case 4: { // a desert highway
        dl->AddCircleFilled(c.at({78, 13}), c.s(7), IM_COL32(255, 250, 210, 255), 24);
        dl->AddTriangleFilled(c.at({0, 34}), c.at({26, 20}), c.at({54, 34}), IM_COL32(170, 90, 60, 255));
        dl->AddRectFilled(c.at({0, 34}), c.at({100, 50}), IM_COL32(205, 140, 80, 255));
        dl->AddTriangleFilled(c.at({49, 34}), c.at({51, 34}), c.at({80, 50}), IM_COL32(50, 50, 55, 255));
        dl->AddTriangleFilled(c.at({49, 34}), c.at({20, 50}), c.at({80, 50}), IM_COL32(50, 50, 55, 255));
        dl->AddLine(c.at({50, 36}), c.at({50, 50}), gold, c.s(0.8f));
        break;
    }
    case 5: { // a big stage
        for (int i = 0; i < 5; ++i) {
            const float x = 10.0f + i * 20.0f, sw = std::sin(tt * 2.0f + i) * 6.0f;
            dl->AddTriangleFilled(c.at({x, 0}), c.at({x - 9.0f + sw, 42}), c.at({x + 9.0f + sw, 42}), i % 2 ? IM_COL32(255, 80, 200, 70) : IM_COL32(90, 200, 255, 70));
        }
        dl->AddRectFilled(c.at({0, 40}), c.at({100, 50}), ink);
        break;
    }
    case 6: { // an underground club
        dl->AddCircleFilled(c.at({50, 12}), c.s(6), IM_COL32(220, 220, 240, 255), 20);
        for (int i = 0; i < 7; ++i) {
            const float a = 0.3f + i * 0.42f + tt;
            dl->AddLine(c.at({50, 12}), c.at({50.0f + std::cos(a) * 70.0f, 12.0f + std::abs(std::sin(a)) * 60.0f}), i % 2 ? IM_COL32(60, 255, 160, 170) : IM_COL32(255, 60, 130, 170), c.s(0.6f));
        }
        for (int i = 0; i < 8; ++i) dl->AddCircleFilled(c.at({6.0f + i * 13.0f, 47}), c.s(3.5f), ink, 12);
        break;
    }
    case 7: { // an empty warehouse
        for (int i = 0; i < 4; ++i) {
            dl->AddRectFilled(c.at({10.0f + i * 22.0f, 6}), c.at({26.0f + i * 22.0f, 26}), IM_COL32(190, 215, 235, 210));
            dl->AddLine(c.at({18.0f + i * 22.0f, 6}), c.at({18.0f + i * 22.0f, 26}), ink, c.s(0.6f));
            dl->AddLine(c.at({10.0f + i * 22.0f, 16}), c.at({26.0f + i * 22.0f, 16}), ink, c.s(0.6f));
        }
        dl->AddRectFilled(c.at({0, 40}), c.at({100, 50}), IM_COL32(45, 45, 50, 255));
        break;
    }
    case 8: { // a mansion
        dl->AddRectFilled(c.at({18, 18}), c.at({82, 44}), IM_COL32(245, 240, 230, 255));
        dl->AddTriangleFilled(c.at({14, 18}), c.at({50, 6}), c.at({86, 18}), IM_COL32(225, 215, 200, 255));
        for (int i = 0; i < 6; ++i) dl->AddRectFilled(c.at({22.0f + i * 11.0f, 20}), c.at({25.0f + i * 11.0f, 44}), IM_COL32(205, 195, 180, 255));
        dl->AddRectFilled(c.at({0, 44}), c.at({100, 50}), IM_COL32(70, 130, 80, 255));
        break;
    }
    case 9: { // a mountain top
        dl->AddTriangleFilled(c.at({-8, 50}), c.at({34, 12}), c.at({72, 50}), IM_COL32(85, 100, 125, 255));
        dl->AddTriangleFilled(c.at({30, 50}), c.at({70, 20}), c.at({110, 50}), IM_COL32(60, 75, 100, 255));
        dl->AddTriangleFilled(c.at({34, 12}), c.at({27, 20}), c.at({41, 20}), white);
        dl->AddTriangleFilled(c.at({70, 20}), c.at({64, 25}), c.at({76, 25}), white);
        break;
    }
    case 10: { // a space station
        for (int i = 0; i < 16; ++i) dl->AddCircleFilled(c.at({3.0f + i * 6.3f, 4.0f + (i * 7 % 13) * 3.2f}), c.s(0.45f), white);
        dl->AddCircleFilled(c.at({78, 40}), c.s(18), IM_COL32(50, 110, 190, 255), 36);
        dl->AddRectFilled(c.at({22, 16}), c.at({44, 22}), IM_COL32(200, 205, 215, 255));
        dl->AddRectFilled(c.at({10, 12}), c.at({20, 26}), IM_COL32(60, 90, 170, 255));
        dl->AddRectFilled(c.at({46, 12}), c.at({56, 26}), IM_COL32(60, 90, 170, 255));
        break;
    }
    case 11: { // a snowy town
        for (int i = 0; i < 4; ++i) {
            const float x = 8.0f + i * 23.0f;
            dl->AddRectFilled(c.at({x, 32}), c.at({x + 16, 46}), IM_COL32(120, 70, 55, 255));
            dl->AddTriangleFilled(c.at({x - 3, 32}), c.at({x + 8, 21}), c.at({x + 19, 32}), white);
            dl->AddRectFilled(c.at({x + 6, 36}), c.at({x + 10, 41}), gold);
        }
        dl->AddRectFilled(c.at({0, 46}), c.at({100, 50}), white);
        for (int i = 0; i < 14; ++i) dl->AddCircleFilled(c.at({3.0f + i * 7.1f, std::fmod(4.0f + i * 11.0f + tt * 8.0f, 30.0f)}), c.s(0.6f), white);
        break;
    }
    case 12: { // cinematic: wide bars
        dl->AddCircleFilled(c.at({30, 25}), c.s(16), IM_COL32(255, 180, 90, 90), 32);
        dl->AddCircleFilled(c.at({30, 25}), c.s(8), IM_COL32(255, 210, 140, 160), 32);
        dl->AddRectFilled(c.at({0, 0}), c.at({100, 8}), IM_COL32(0, 0, 0, 255));
        dl->AddRectFilled(c.at({0, 42}), c.at({100, 50}), IM_COL32(0, 0, 0, 255));
        break;
    }
    case 13: { // neon glow
        for (int r = 3; r >= 0; --r) dl->AddCircle(c.at({50, 25}), c.s(14), r ? IM_COL32(255, 60, 220, 50) : IM_COL32(255, 180, 245, 255), 40, c.s(0.8f + r * 1.4f));
        for (int r = 3; r >= 0; --r) dl->AddLine(c.at({18, 42}), c.at({82, 42}), r ? IM_COL32(60, 220, 255, 50) : IM_COL32(190, 245, 255, 255), c.s(0.6f + r * 1.2f));
        break;
    }
    case 14: { // black and white
        dl->AddRectFilled(c.at({50, 0}), c.at({100, 50}), IM_COL32(235, 235, 235, 255));
        dl->AddCircleFilled(c.at({50, 25}), c.s(14), IM_COL32(150, 150, 150, 255), 32);
        dl->AddCircleFilled(c.at({44, 22}), c.s(4), IM_COL32(245, 245, 245, 255), 16);
        dl->AddCircleFilled(c.at({56, 22}), c.s(4), IM_COL32(20, 20, 20, 255), 16);
        break;
    }
    case 15: { // VHS
        for (int i = 0; i < 6; ++i) dl->AddRectFilled(c.at({0, 6.0f + i * 7.0f}), c.at({100, 7.5f + i * 7.0f}), IM_COL32(255, 255, 255, 30));
        dl->AddRectFilled(c.at({0, std::fmod(tt * 18.0f, 50.0f)}), c.at({100, std::fmod(tt * 18.0f, 50.0f) + 4.0f}), IM_COL32(255, 255, 255, 55));
        Text(c, {8, 7}, "PLAY", white, 8.0f);
        dl->AddTriangleFilled(c.at({38, 9}), c.at({38, 15}), c.at({43, 12}), white);
        dl->AddRectFilled(c.at({0, 40}), c.at({33, 42}), IM_COL32(255, 60, 90, 255));
        dl->AddRectFilled(c.at({33, 40}), c.at({66, 42}), IM_COL32(80, 255, 130, 255));
        dl->AddRectFilled(c.at({66, 40}), c.at({100, 42}), IM_COL32(80, 130, 255, 255));
        break;
    }
    case 16: { // anime: speed lines and a star
        for (int i = 0; i < 18; ++i) {
            const float a = i * 0.349f;
            dl->AddLine(c.at({50.0f + std::cos(a) * 14.0f, 25.0f + std::sin(a) * 9.0f}), c.at({50.0f + std::cos(a) * 80.0f, 25.0f + std::sin(a) * 50.0f}), IM_COL32(255, 255, 255, 170), c.s(0.5f));
        }
        Sparkle(c, {50, 25}, 9.0f + 2.0f * std::sin(tt * 4.0f), white);
        break;
    }
    case 17: { // dreamy pastel clouds
        Cloud(c, {28, 22}, 1.1f, IM_COL32(255, 255, 255, 230));
        Cloud(c, {68, 32}, 1.3f, IM_COL32(255, 245, 250, 220));
        Sparkle(c, {52, 12}, 3.0f, gold);
        Sparkle(c, {82, 12}, 2.0f, white);
        break;
    }
    case 18: { // gritty street: bricks
        for (int y = 0; y < 7; ++y)
            for (int x = -1; x < 8; ++x) {
                const float bx = x * 15.0f + (y % 2) * 7.5f, by = y * 7.2f;
                dl->AddRect(c.at({bx, by}), c.at({bx + 15.0f, by + 7.2f}), IM_COL32(25, 18, 18, 200), 0.0f, 0, c.s(0.5f));
            }
        dl->AddCircleFilled(c.at({66, 30}), c.s(10), IM_COL32(255, 80, 40, 150), 24);
        break;
    }
    case 19: { // luxury gloss: a diamond
        dl->AddTriangleFilled(c.at({34, 18}), c.at({66, 18}), c.at({50, 42}), IM_COL32(190, 235, 255, 255));
        dl->AddTriangleFilled(c.at({34, 18}), c.at({42, 9}), c.at({50, 18}), IM_COL32(235, 250, 255, 255));
        dl->AddTriangleFilled(c.at({50, 18}), c.at({58, 9}), c.at({66, 18}), IM_COL32(150, 215, 245, 255));
        dl->AddTriangleFilled(c.at({42, 9}), c.at({58, 9}), c.at({50, 18}), white);
        Sparkle(c, {72, 12}, 3.0f, gold);
        Sparkle(c, {28, 34}, 2.0f, gold);
        break;
    }
    case 20: { // retro 70s: a sunburst
        for (int i = 0; i < 5; ++i) dl->AddCircleFilled(c.at({50, 50}), c.s(46.0f - i * 9.0f), i % 2 ? IM_COL32(250, 190, 90, 255) : IM_COL32(170, 60, 30, 255), 48);
        break;
    }
    case 21: { // horror dark
        dl->AddCircleFilled(c.at({50, 20}), c.s(11), IM_COL32(230, 60, 50, 255), 28);
        dl->AddRectFilled(c.at({0, 42}), c.at({100, 50}), IM_COL32(5, 3, 5, 255));
        for (int i = 0; i < 6; ++i) dl->AddLine(c.at({8.0f + i * 17.0f, 50}), c.at({8.0f + i * 17.0f + (i % 2 ? 3.0f : -3.0f), 18.0f + (i % 3) * 7.0f}), IM_COL32(5, 3, 5, 255), c.s(1.2f));
        break;
    }
    case 34: { // a heart
        const float k = 1.0f + (t >= 0.0f ? 0.08f * std::sin(tt * 5.0f) : 0.0f);
        dl->AddCircleFilled(c.at({50.0f - 7.0f * k, 20}), c.s(8.0f * k), white, 24);
        dl->AddCircleFilled(c.at({50.0f + 7.0f * k, 20}), c.s(8.0f * k), white, 24);
        dl->AddTriangleFilled(c.at({50.0f - 14.5f * k, 23}), c.at({50.0f + 14.5f * k, 23}), c.at({50, 42}), white);
        break;
    }
    default: { // a genre: a sound meter and a note, its colours and speed its own
        const int bars = 11;
        const float speed = 3.0f + (art % 5) * 1.6f;
        for (int i = 0; i < bars; ++i) {
            const float v = 0.5f + 0.5f * std::sin(i * (0.9f + (art % 3) * 0.5f) + tt * speed);
            const float x = 7.0f + i * 8.0f, top = 44.0f - (7.0f + v * 26.0f);
            dl->AddRectFilled(c.at({x, top}), c.at({x + 5.0f, 44}), IM_COL32(255, 255, 255, 215), c.s(1.0f));
        }
        Note(c, {80, 14}, ink, 1.3f);
        break;
    }
    }
    dl->PopClipRect();
}

