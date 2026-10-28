// Built-in icons (clothes, bags, vehicles, tools...) for character inventories. They are small SVG drawings that a tiny
// rasterizer (paths, rectangles, circles, polygons, lines; fills and strokes) turns into PNG files the first time they are
// used, so they behave like any other picture: thumbnails, export, attaching them for the vision model.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <windows.h>

#include "board.h"
#include "stb_image_write.h"

namespace fs = std::filesystem;

namespace {

struct Pt {
    float x, y;
};
using Poly = std::vector<Pt>;
struct Sub {
    Poly pts;
    bool closed = false;
};
struct Rgba {
    float r = 0, g = 0, b = 0, a = 1;
};

constexpr int kSS = 4; // sub-scanlines per pixel row

// ---------------------------------------------------------------- attribute / number parsing
std::string Attr(const std::string& tag, const char* name) {
    const std::string key = std::string(name) + "=\"";
    size_t pos = 0;
    while ((pos = tag.find(key, pos)) != std::string::npos) {
        if (pos == 0 || std::isspace(static_cast<unsigned char>(tag[pos - 1]))) {
            const size_t start = pos + key.size();
            const size_t end = tag.find('"', start);
            return end == std::string::npos ? std::string() : tag.substr(start, end - start);
        }
        pos += key.size();
    }
    return {};
}

float Num(const std::string& s, float def) { return s.empty() ? def : static_cast<float>(std::atof(s.c_str())); }

bool ParseColor(const std::string& s, Rgba& out) {
    if (s.empty() || s == "none") return false;
    std::string h = s;
    if (h == "white") h = "#ffffff";
    else if (h == "black") h = "#000000";
    if (h[0] != '#') return false;
    h = h.substr(1);
    if (h.size() == 3) h = std::string() + h[0] + h[0] + h[1] + h[1] + h[2] + h[2];
    if (h.size() != 6) return false;
    const unsigned v = static_cast<unsigned>(std::strtoul(h.c_str(), nullptr, 16));
    out.r = ((v >> 16) & 255) / 255.0f;
    out.g = ((v >> 8) & 255) / 255.0f;
    out.b = (v & 255) / 255.0f;
    return true;
}

std::vector<float> Numbers(const std::string& s) {
    std::vector<float> out;
    const char* p = s.c_str();
    while (*p) {
        if (std::isdigit(static_cast<unsigned char>(*p)) || *p == '-' || *p == '+' || *p == '.') {
            char* end = nullptr;
            const float v = std::strtof(p, &end);
            if (end == p) {
                ++p;
                continue;
            }
            out.push_back(v);
            p = end;
        } else {
            ++p;
        }
    }
    return out;
}

// ---------------------------------------------------------------- shapes -> polygons
void Cubic(Poly& out, Pt a, Pt b, Pt c, Pt d) {
    for (int i = 1; i <= 12; ++i) {
        const float t = i / 12.0f, u = 1 - t;
        out.push_back({u * u * u * a.x + 3 * u * u * t * b.x + 3 * u * t * t * c.x + t * t * t * d.x,
                       u * u * u * a.y + 3 * u * u * t * b.y + 3 * u * t * t * c.y + t * t * t * d.y});
    }
}

void Quad(Poly& out, Pt a, Pt b, Pt c) {
    for (int i = 1; i <= 10; ++i) {
        const float t = i / 10.0f, u = 1 - t;
        out.push_back({u * u * a.x + 2 * u * t * b.x + t * t * c.x, u * u * a.y + 2 * u * t * b.y + t * t * c.y});
    }
}

std::vector<Sub> ParsePath(const std::string& d) {
    std::vector<Sub> subs;
    const char* p = d.c_str();
    Pt cur{0, 0}, start{0, 0}, lastC{0, 0};
    char cmd = 0, prev = 0;
    auto read = [&](float& v) {
        while (*p && (std::isspace(static_cast<unsigned char>(*p)) || *p == ',')) ++p;
        char* end = nullptr;
        v = std::strtof(p, &end);
        if (end == p) return false;
        p = end;
        return true;
    };
    while (*p) {
        while (*p && (std::isspace(static_cast<unsigned char>(*p)) || *p == ',')) ++p;
        if (!*p) break;
        if (std::isalpha(static_cast<unsigned char>(*p))) cmd = *p++;
        else if (cmd == 'M') cmd = 'L';
        else if (cmd == 'm') cmd = 'l';
        const bool rel = std::islower(static_cast<unsigned char>(cmd)) != 0;
        const char up = static_cast<char>(std::toupper(static_cast<unsigned char>(cmd)));
        float a[6];
        auto abs = [&](float x, float y) { return Pt{rel ? cur.x + x : x, rel ? cur.y + y : y}; };
        if (up == 'Z') {
            if (!subs.empty()) subs.back().closed = true;
            cur = start;
            prev = 'Z';
            continue;
        }
        if (up == 'M') {
            if (!read(a[0]) || !read(a[1])) break;
            cur = abs(a[0], a[1]);
            start = cur;
            subs.push_back(Sub());
            subs.back().pts.push_back(cur);
        } else if (up == 'L') {
            if (!read(a[0]) || !read(a[1])) break;
            cur = abs(a[0], a[1]);
            if (subs.empty()) subs.push_back(Sub());
            subs.back().pts.push_back(cur);
        } else if (up == 'H') {
            if (!read(a[0])) break;
            cur.x = rel ? cur.x + a[0] : a[0];
            if (subs.empty()) subs.push_back(Sub());
            subs.back().pts.push_back(cur);
        } else if (up == 'V') {
            if (!read(a[0])) break;
            cur.y = rel ? cur.y + a[0] : a[0];
            if (subs.empty()) subs.push_back(Sub());
            subs.back().pts.push_back(cur);
        } else if (up == 'C') {
            if (!read(a[0]) || !read(a[1]) || !read(a[2]) || !read(a[3]) || !read(a[4]) || !read(a[5])) break;
            const Pt c1 = abs(a[0], a[1]), c2 = abs(a[2], a[3]), e = abs(a[4], a[5]);
            if (subs.empty()) subs.push_back(Sub());
            Cubic(subs.back().pts, cur, c1, c2, e);
            lastC = c2;
            cur = e;
        } else if (up == 'S') {
            if (!read(a[0]) || !read(a[1]) || !read(a[2]) || !read(a[3])) break;
            const Pt c1 = (prev == 'C' || prev == 'S') ? Pt{2 * cur.x - lastC.x, 2 * cur.y - lastC.y} : cur;
            const Pt c2 = abs(a[0], a[1]), e = abs(a[2], a[3]);
            if (subs.empty()) subs.push_back(Sub());
            Cubic(subs.back().pts, cur, c1, c2, e);
            lastC = c2;
            cur = e;
        } else if (up == 'Q') {
            if (!read(a[0]) || !read(a[1]) || !read(a[2]) || !read(a[3])) break;
            const Pt c = abs(a[0], a[1]), e = abs(a[2], a[3]);
            if (subs.empty()) subs.push_back(Sub());
            Quad(subs.back().pts, cur, c, e);
            lastC = c;
            cur = e;
        } else if (up == 'T') {
            if (!read(a[0]) || !read(a[1])) break;
            const Pt c = (prev == 'Q' || prev == 'T') ? Pt{2 * cur.x - lastC.x, 2 * cur.y - lastC.y} : cur;
            const Pt e = abs(a[0], a[1]);
            if (subs.empty()) subs.push_back(Sub());
            Quad(subs.back().pts, cur, c, e);
            lastC = c;
            cur = e;
        } else {
            break; // arcs and the rest are not used by the built-in icons
        }
        prev = up;
    }
    return subs;
}

Poly Ellipse(float cx, float cy, float rx, float ry) {
    Poly p;
    for (int i = 0; i < 40; ++i) {
        const float t = 6.2831853f * i / 40.0f;
        p.push_back({cx + rx * std::cos(t), cy + ry * std::sin(t)});
    }
    return p;
}

Poly RoundRect(float x, float y, float w, float h, float r) {
    r = std::min(r, std::min(w, h) * 0.5f);
    Poly p;
    if (r <= 0.01f) return {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
    const float cx[4] = {x + w - r, x + w - r, x + r, x + r}, cy[4] = {y + r, y + h - r, y + h - r, y + r};
    for (int c = 0; c < 4; ++c) {
        for (int i = 0; i <= 6; ++i) {
            const float t = (-90.0f + 90.0f * c + 90.0f * i / 6.0f) * 0.0174532925f;
            p.push_back({cx[c] + r * std::cos(t), cy[c] + r * std::sin(t)});
        }
    }
    return p;
}

float SignedArea(const Poly& p) {
    float a = 0;
    for (size_t i = 0; i < p.size(); ++i) {
        const Pt &u = p[i], &v = p[(i + 1) % p.size()];
        a += u.x * v.y - v.x * u.y;
    }
    return a * 0.5f;
}

// a stroke as a set of polygons: a quad per segment and a round blob at every vertex, all wound the same way
std::vector<Poly> StrokePolys(const std::vector<Sub>& subs, float width) {
    std::vector<Poly> out;
    const float h = width * 0.5f;
    auto blob = [&](Pt c) { out.push_back(Ellipse(c.x, c.y, h, h)); };
    for (const Sub& s : subs) {
        const size_t n = s.pts.size();
        if (n == 0) continue;
        const size_t segs = s.closed ? n : n - 1;
        for (size_t i = 0; i < segs; ++i) {
            const Pt a = s.pts[i], b = s.pts[(i + 1) % n];
            const float dx = b.x - a.x, dy = b.y - a.y, len = std::sqrt(dx * dx + dy * dy);
            if (len < 1e-4f) continue;
            const float nx = -dy / len * h, ny = dx / len * h;
            out.push_back({{a.x + nx, a.y + ny}, {b.x + nx, b.y + ny}, {b.x - nx, b.y - ny}, {a.x - nx, a.y - ny}});
        }
        if (width >= 2.0f) {
            for (size_t i = 0; i < n; ++i) {
                if (i % 3 == 0 || i + 1 == n) blob(s.pts[i]); // enough blobs to round the joins without drowning in polygons
            }
        } else {
            blob(s.pts.front());
            blob(s.pts.back());
        }
    }
    for (Poly& p : out) {
        if (SignedArea(p) < 0) std::reverse(p.begin(), p.end());
    }
    return out;
}

// ---------------------------------------------------------------- rasterizing
struct Canvas {
    int W = 128, H = 128;
    std::vector<float> px; // premultiplied RGBA
    Canvas(int w, int h) : W(w), H(h), px(static_cast<size_t>(w) * h * 4, 0.0f) {}
};

void Fill(Canvas& cv, const std::vector<Poly>& polys, float scale, const Rgba& color, float alpha) {
    struct Edge {
        float x0, y0, x1, y1;
        int dir;
    };
    std::vector<Edge> edges;
    for (const Poly& p : polys) {
        if (p.size() < 3) continue;
        for (size_t i = 0; i < p.size(); ++i) {
            const Pt a = p[i], b = p[(i + 1) % p.size()];
            if (a.y == b.y) continue;
            edges.push_back({a.x * scale, a.y * scale, b.x * scale, b.y * scale, b.y > a.y ? 1 : -1});
        }
    }
    if (edges.empty()) return;
    const int W = cv.W, H = cv.H;
    std::vector<float> cov(static_cast<size_t>(W) * H, 0.0f);
    std::vector<std::pair<float, int>> hits;
    for (int y = 0; y < H; ++y) {
        for (int k = 0; k < kSS; ++k) {
            const float ys = y + (k + 0.5f) / kSS;
            hits.clear();
            for (const Edge& e : edges) {
                const float lo = std::min(e.y0, e.y1), hi = std::max(e.y0, e.y1);
                if (ys < lo || ys >= hi) continue;
                hits.push_back({e.x0 + (ys - e.y0) * (e.x1 - e.x0) / (e.y1 - e.y0), e.dir});
            }
            if (hits.size() < 2) continue;
            std::sort(hits.begin(), hits.end());
            int wind = 0;
            for (size_t i = 0; i + 1 < hits.size(); ++i) {
                wind += hits[i].second;
                if (wind == 0) continue;
                const float xa = std::max(0.0f, hits[i].first), xb = std::min(static_cast<float>(W), hits[i + 1].first);
                if (xb <= xa) continue;
                const int i0 = std::min(W - 1, static_cast<int>(xa)), i1 = std::min(W - 1, static_cast<int>(xb));
                float* row = &cov[static_cast<size_t>(y) * W];
                if (i0 == i1) {
                    row[i0] += (xb - xa) / kSS;
                } else {
                    row[i0] += (i0 + 1 - xa) / kSS;
                    for (int x = i0 + 1; x < i1; ++x) row[x] += 1.0f / kSS;
                    row[i1] += (xb - i1) / kSS;
                }
            }
        }
    }
    for (int i = 0; i < W * H; ++i) {
        const float a = std::min(1.0f, cov[i]) * alpha;
        if (a <= 0) continue;
        float* d = &cv.px[i * 4];
        d[0] = color.r * a + d[0] * (1 - a);
        d[1] = color.g * a + d[1] * (1 - a);
        d[2] = color.b * a + d[2] * (1 - a);
        d[3] = a + d[3] * (1 - a);
    }
}

void StbWrite(void* ctx, void* data, int size) { static_cast<std::string*>(ctx)->append(static_cast<const char*>(data), static_cast<size_t>(size)); }

bool RenderSvg(const std::string& body, const std::wstring& pngPath, int vbw, int vbh) {
    const int unit = vbw > 64 ? 4 : 2; // pixels per drawing unit: icons are 128 px, scenes 384 px wide
    const int W = vbw * unit, H = vbh * unit;
    Canvas cv(W, H);
    const float scale = static_cast<float>(unit);
    size_t pos = 0;
    while ((pos = body.find('<', pos)) != std::string::npos) {
        const size_t end = body.find('>', pos);
        if (end == std::string::npos) break;
        const std::string tag = body.substr(pos + 1, end - pos - 1);
        pos = end + 1;
        if (tag.empty() || tag[0] == '/' || tag[0] == '!') continue;
        size_t nameEnd = 0;
        while (nameEnd < tag.size() && std::isalnum(static_cast<unsigned char>(tag[nameEnd]))) ++nameEnd;
        const std::string name = tag.substr(0, nameEnd);

        Rgba fill, stroke;
        const std::string fillAttr = Attr(tag, "fill");
        const bool hasFill = fillAttr.empty() ? name != "line" : ParseColor(fillAttr, fill);
        const bool hasStroke = ParseColor(Attr(tag, "stroke"), stroke);
        if (fillAttr.empty() && hasFill) fill = Rgba{0, 0, 0, 1};
        const float strokeWidth = Num(Attr(tag, "stroke-width"), 1.0f);
        const float opacity = Num(Attr(tag, "opacity"), 1.0f);

        std::vector<Sub> subs;
        if (name == "path") {
            subs = ParsePath(Attr(tag, "d"));
        } else if (name == "rect") {
            Sub s;
            s.pts = RoundRect(Num(Attr(tag, "x"), 0), Num(Attr(tag, "y"), 0), Num(Attr(tag, "width"), 0), Num(Attr(tag, "height"), 0), Num(Attr(tag, "rx"), 0));
            s.closed = true;
            subs.push_back(std::move(s));
        } else if (name == "circle") {
            Sub s;
            const float r = Num(Attr(tag, "r"), 0);
            s.pts = Ellipse(Num(Attr(tag, "cx"), 0), Num(Attr(tag, "cy"), 0), r, r);
            s.closed = true;
            subs.push_back(std::move(s));
        } else if (name == "ellipse") {
            Sub s;
            s.pts = Ellipse(Num(Attr(tag, "cx"), 0), Num(Attr(tag, "cy"), 0), Num(Attr(tag, "rx"), 0), Num(Attr(tag, "ry"), 0));
            s.closed = true;
            subs.push_back(std::move(s));
        } else if (name == "polygon" || name == "polyline") {
            const std::vector<float> n = Numbers(Attr(tag, "points"));
            Sub s;
            for (size_t i = 0; i + 1 < n.size(); i += 2) s.pts.push_back({n[i], n[i + 1]});
            s.closed = name == "polygon";
            subs.push_back(std::move(s));
        } else if (name == "line") {
            Sub s;
            s.pts = {{Num(Attr(tag, "x1"), 0), Num(Attr(tag, "y1"), 0)}, {Num(Attr(tag, "x2"), 0), Num(Attr(tag, "y2"), 0)}};
            subs.push_back(std::move(s));
        } else {
            continue;
        }
        if (hasFill) {
            std::vector<Poly> polys;
            for (const Sub& s : subs) polys.push_back(s.pts);
            Fill(cv, polys, scale, fill, opacity);
        }
        if (hasStroke) Fill(cv, StrokePolys(subs, strokeWidth), scale, stroke, opacity);
    }
    std::vector<unsigned char> rgba(static_cast<size_t>(W) * H * 4);
    for (int i = 0; i < W * H; ++i) {
        const float* s = &cv.px[i * 4];
        const float a = s[3];
        rgba[i * 4 + 0] = static_cast<unsigned char>(std::clamp(a > 0 ? s[0] / a : 0.0f, 0.0f, 1.0f) * 255.0f + 0.5f);
        rgba[i * 4 + 1] = static_cast<unsigned char>(std::clamp(a > 0 ? s[1] / a : 0.0f, 0.0f, 1.0f) * 255.0f + 0.5f);
        rgba[i * 4 + 2] = static_cast<unsigned char>(std::clamp(a > 0 ? s[2] / a : 0.0f, 0.0f, 1.0f) * 255.0f + 0.5f);
        rgba[i * 4 + 3] = static_cast<unsigned char>(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
    std::string png;
    if (!stbi_write_png_to_func(StbWrite, &png, W, H, 4, rgba.data(), W * 4)) return false;
    std::ofstream out(pngPath, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(png.data(), static_cast<std::streamsize>(png.size()));
    return true;
}

// ---------------------------------------------------------------- the icons (64 x 64 drawings)
struct IconDef {
    const char* id;
    const char* name;
    const char* category;
    const char* svg;
    int vbw = 64, vbh = 64;   // size of the drawing (scenes are 96 x 54)
    const char* desc = "";    // what a scene is, in words (used as the card's description)
};

const IconDef kIcons[] = {
    // ---- Clothes
    {"tshirt", "T-shirt", "Clothes", R"(<path fill="#4F8FE8" d="M21 9 L7 17 L12 28 L19 25 L19 55 L45 55 L45 25 L52 28 L57 17 L43 9 C41 14 37 16 32 16 C27 16 23 14 21 9 Z"/><path fill="none" stroke="#3669B5" stroke-width="2" d="M21 9 C23 14 27 16 32 16 C37 16 41 14 43 9"/>)"},
    {"hoodie", "Hoodie", "Clothes", R"(<path fill="#7A5BD0" d="M20 20 L6 30 L11 42 L18 38 L18 56 L46 56 L46 38 L53 42 L58 30 L44 20 Z"/><path fill="#6347B5" d="M20 20 C20 6 44 6 44 20 C40 27 24 27 20 20 Z"/><path fill="none" stroke="#5238A0" stroke-width="2" d="M28 24 L28 34 M36 24 L36 34 M24 47 L40 47"/>)"},
    {"jacket", "Jacket", "Clothes", R"(<path fill="#B5651D" d="M20 10 L6 18 L10 46 L18 46 L18 56 L46 56 L46 46 L54 46 L58 18 L44 10 L32 22 Z"/><path fill="#8F4E14" d="M20 10 L32 22 L26 26 L18 16 Z M44 10 L32 22 L38 26 L46 16 Z"/><path fill="none" stroke="#5E3410" stroke-width="2" d="M32 22 L32 56"/>)"},
    {"dress", "Dress", "Clothes", R"(<path fill="#E0527A" d="M24 8 L28 8 L32 14 L36 8 L40 8 L40 22 L47 57 L17 57 L24 22 Z"/><rect fill="#B73A5E" x="24" y="21" width="16" height="4"/>)"},
    {"pants", "Pants", "Clothes", R"(<path fill="#3A4A7A" d="M18 6 L46 6 L51 57 L36 57 L32 24 L28 57 L13 57 Z"/><rect fill="#2A3560" x="18" y="6" width="28" height="5"/>)"},
    {"shorts", "Shorts", "Clothes", R"(<path fill="#D98E32" d="M16 12 L48 12 L53 44 L36 44 L32 28 L28 44 L11 44 Z"/><rect fill="#B06F1F" x="16" y="12" width="32" height="5"/>)"},
    {"skirt", "Skirt", "Clothes", R"(<path fill="#C0398F" d="M22 12 L42 12 L55 52 L9 52 Z"/><rect fill="#962B70" x="22" y="12" width="20" height="5"/>)"},
    {"hat", "Hat", "Clothes", R"(<ellipse fill="#6B4A2B" cx="32" cy="42" rx="27" ry="8"/><path fill="#8B6238" d="M17 41 C17 14 47 14 47 41 Z"/><rect fill="#3E2A17" x="17" y="34" width="30" height="5"/>)"},
    {"cap", "Baseball cap", "Clothes", R"(<path fill="#D94545" d="M10 42 C10 16 46 14 50 40 Z"/><path fill="#A83030" d="M48 40 C58 40 62 44 62 46 C54 46 46 44 38 42 Z"/><circle fill="#fff" cx="30" cy="16" r="3"/>)"},
    {"beanie", "Beanie", "Clothes", R"(<path fill="#43A047" d="M10 40 C10 14 54 14 54 40 Z"/><rect fill="#2E7D32" x="10" y="36" width="44" height="12" rx="4"/><circle fill="#fff" cx="32" cy="13" r="6"/>)"},
    {"boots", "Boots", "Clothes", R"(<path fill="#7B4B2A" d="M22 6 L40 6 L40 34 L54 40 L54 54 L20 54 Z"/><rect fill="#3E2713" x="20" y="52" width="36" height="6" rx="2"/><rect fill="#5E3820" x="22" y="6" width="18" height="6"/>)"},
    {"sneakers", "Sneakers", "Clothes", R"(<path fill="#ECECF2" d="M6 38 L24 36 L32 28 L40 36 L54 40 L60 48 L60 54 L6 54 Z"/><path fill="#E04848" d="M20 44 L44 44 L50 48 L12 48 Z"/><rect fill="#444" x="6" y="53" width="54" height="5" rx="2"/>)"},
    {"gloves", "Gloves", "Clothes", R"(<path fill="#6A4FD0" d="M20 58 L20 36 L15 26 C14 21 21 20 22 25 L24 31 L24 15 C24 10 31 10 31 15 L31 28 L31 11 C31 6 38 6 38 11 L38 28 L40 15 C41 10 47 11 46 16 L43 36 L49 31 C53 29 56 33 52 37 L44 56 Z"/><rect fill="#4B35A8" x="20" y="52" width="26" height="7" rx="2"/>)"},
    {"scarf", "Scarf", "Clothes", R"(<path fill="#D6453D" d="M14 18 C14 10 50 10 50 18 L50 28 C50 36 14 36 14 28 Z"/><path fill="#B33027" d="M38 32 L50 32 L52 58 L40 58 Z"/><path fill="none" stroke="#fff" stroke-width="2" d="M44 40 L51 40 M45 48 L52 48"/>)"},
    {"glasses", "Glasses", "Clothes", R"(<circle fill="none" stroke="#333" stroke-width="3" cx="19" cy="34" r="11"/><circle fill="none" stroke="#333" stroke-width="3" cx="45" cy="34" r="11"/><path fill="none" stroke="#333" stroke-width="3" d="M30 32 C31 29 33 29 34 32 M8 33 L3 28 M56 33 L61 28"/>)"},
    {"sunglasses", "Sunglasses", "Clothes", R"(<path fill="#1F2937" d="M6 26 L30 26 L28 42 C27 47 12 47 10 42 Z"/><path fill="#1F2937" d="M34 26 L58 26 L54 42 C52 47 37 47 36 42 Z"/><rect fill="#1F2937" x="29" y="27" width="6" height="4"/><path fill="#fff" opacity="0.35" d="M12 29 L20 29 L12 37 Z"/>)"},
    {"tie", "Tie", "Clothes", R"(<path fill="#B22828" d="M27 6 L37 6 L36 16 L28 16 Z"/><path fill="#C93030" d="M28 16 L36 16 L42 46 L32 58 L22 46 Z"/>)"},
    {"helmet", "Knight helmet", "Clothes", R"(<path fill="#A9B3C2" d="M12 56 L12 30 C12 12 52 12 52 30 L52 56 L44 56 L44 42 L20 42 L20 56 Z"/><rect fill="#2B2F36" x="20" y="29" width="24" height="5"/><path fill="#D33" d="M32 14 C27 3 41 1 46 8 C41 8 37 10 35 16 Z"/>)"},
    {"armor", "Armor", "Clothes", R"(<path fill="#9AA5B5" d="M18 8 L46 8 L58 20 L50 30 L50 56 L14 56 L14 30 L6 20 Z"/><path fill="none" stroke="#6B7685" stroke-width="2" d="M32 8 L32 56 M14 44 L50 44"/><circle fill="#C79A3A" cx="32" cy="30" r="4"/>)"},
    {"crown", "Crown", "Clothes", R"(<path fill="#F2C94C" d="M8 48 L8 20 L20 34 L32 14 L44 34 L56 20 L56 48 Z"/><rect fill="#D9A62E" x="8" y="46" width="48" height="9" rx="2"/><circle fill="#E04848" cx="32" cy="42" r="3"/><circle fill="#3B82F6" cx="18" cy="50" r="2"/><circle fill="#3B82F6" cx="46" cy="50" r="2"/>)"},
    {"cape", "Cape", "Clothes", R"(<path fill="#C0282D" d="M20 6 L44 6 L58 58 L6 58 Z"/><path fill="none" stroke="#8F1B20" stroke-width="2" d="M26 14 L20 58 M38 14 L44 58"/><circle fill="#F2C94C" cx="32" cy="9" r="4"/>)"},
    {"mask", "Mask", "Clothes", R"(<path fill="#2B2B33" d="M6 22 C6 12 58 12 58 22 L54 40 C50 48 40 40 32 44 C24 40 14 48 10 40 Z"/><ellipse fill="#F4F4F4" cx="21" cy="30" rx="8" ry="5"/><ellipse fill="#F4F4F4" cx="43" cy="30" rx="8" ry="5"/>)"},
    // ---- Bags
    {"backpack", "Backpack", "Bags", R"(<path fill="none" stroke="#B85A1F" stroke-width="4" d="M26 14 C26 4 38 4 38 14"/><path fill="#E07B39" d="M14 22 C14 10 50 10 50 22 L52 56 L12 56 Z"/><rect fill="#B85A1F" x="20" y="36" width="24" height="14" rx="3"/><rect fill="#8F4413" x="20" y="28" width="24" height="3"/>)"},
    {"handbag", "Handbag", "Bags", R"(<path fill="none" stroke="#8F2A5A" stroke-width="4" d="M21 26 C21 6 43 6 43 26"/><path fill="#C2397A" d="M8 26 L56 26 L51 56 L13 56 Z"/><circle fill="#F2C94C" cx="32" cy="34" r="3"/>)"},
    {"suitcase", "Suitcase", "Bags", R"(<path fill="none" stroke="#2A4FA0" stroke-width="4" d="M23 18 L23 9 L41 9 L41 18"/><rect fill="#3B6FD0" x="7" y="18" width="50" height="36" rx="5"/><rect fill="#2A4FA0" x="17" y="18" width="4" height="36"/><rect fill="#2A4FA0" x="43" y="18" width="4" height="36"/><circle fill="#333" cx="16" cy="57" r="3"/><circle fill="#333" cx="48" cy="57" r="3"/>)"},
    {"briefcase", "Briefcase", "Bags", R"(<path fill="none" stroke="#3E2A17" stroke-width="4" d="M24 22 L24 14 L40 14 L40 22"/><rect fill="#6B4A2B" x="7" y="22" width="50" height="32" rx="4"/><rect fill="#4C3320" x="7" y="34" width="50" height="3"/><rect fill="#F2C94C" x="28" y="31" width="8" height="8" rx="1"/>)"},
    {"satchel", "Messenger bag", "Bags", R"(<path fill="none" stroke="#6B4A2B" stroke-width="4" d="M14 26 C14 2 50 2 50 26"/><path fill="#8B6238" d="M10 26 L54 26 L52 54 L12 54 Z"/><path fill="#6B4A2B" d="M10 26 L54 26 L50 40 L14 40 Z"/><rect fill="#F2C94C" x="29" y="36" width="6" height="7" rx="1"/>)"},
    {"shoppingbag", "Shopping bag", "Bags", R"(<path fill="none" stroke="#2F8F4A" stroke-width="3" d="M22 22 C22 6 42 6 42 22"/><path fill="#5BBE6A" d="M12 20 L52 20 L54 58 L10 58 Z"/><circle fill="#2F8F4A" cx="22" cy="26" r="2"/><circle fill="#2F8F4A" cx="42" cy="26" r="2"/>)"},
    {"duffel", "Duffel bag", "Bags", R"(<path fill="none" stroke="#3B4A63" stroke-width="4" d="M18 30 C22 12 42 12 46 30"/><path fill="#556B8D" d="M5 36 C5 26 59 26 59 36 L57 50 L7 50 Z"/><path fill="none" stroke="#3B4A63" stroke-width="2" d="M6 38 L58 38"/><rect fill="#F2C94C" x="29" y="35" width="6" height="6" rx="1"/>)"},
    // ---- Vehicles
    {"car", "Car", "Vehicles", R"(<path fill="#E04848" d="M4 40 L10 28 L22 22 L42 22 L54 30 L60 34 L60 47 L4 47 Z"/><path fill="#BEE3F8" d="M24 25 L40 25 L48 31 L22 31 Z"/><circle fill="#2B2B33" cx="18" cy="47" r="8"/><circle fill="#9AA5B5" cx="18" cy="47" r="3.5"/><circle fill="#2B2B33" cx="47" cy="47" r="8"/><circle fill="#9AA5B5" cx="47" cy="47" r="3.5"/>)"},
    {"sportscar", "Sports car", "Vehicles", R"(<path fill="#F2C94C" d="M3 44 L10 34 L28 27 L44 29 L58 37 L61 45 L3 46 Z"/><path fill="#2B2B33" d="M28 29 L42 30 L50 36 L24 36 Z"/><circle fill="#2B2B33" cx="17" cy="46" r="7"/><circle fill="#ccc" cx="17" cy="46" r="3"/><circle fill="#2B2B33" cx="49" cy="46" r="7"/><circle fill="#ccc" cx="49" cy="46" r="3"/>)"},
    {"truck", "Truck", "Vehicles", R"(<rect fill="#7E8C9A" x="3" y="16" width="36" height="28" rx="2"/><path fill="#3B6FD0" d="M39 26 L52 26 L61 36 L61 44 L39 44 Z"/><path fill="#BEE3F8" d="M43 29 L51 29 L56 35 L43 35 Z"/><circle fill="#2B2B33" cx="16" cy="46" r="7"/><circle fill="#2B2B33" cx="48" cy="46" r="7"/><circle fill="#aaa" cx="16" cy="46" r="3"/><circle fill="#aaa" cx="48" cy="46" r="3"/>)"},
    {"bus", "Bus", "Vehicles", R"(<rect fill="#F2B632" x="3" y="14" width="58" height="32" rx="5"/><rect fill="#BEE3F8" x="8" y="19" width="10" height="10"/><rect fill="#BEE3F8" x="21" y="19" width="10" height="10"/><rect fill="#BEE3F8" x="34" y="19" width="10" height="10"/><rect fill="#BEE3F8" x="47" y="19" width="10" height="10"/><circle fill="#2B2B33" cx="16" cy="47" r="7"/><circle fill="#2B2B33" cx="48" cy="47" r="7"/><rect fill="#E08A00" x="3" y="36" width="58" height="3"/>)"},
    {"motorcycle", "Motorcycle", "Vehicles", R"(<circle fill="none" stroke="#2B2B33" stroke-width="5" cx="14" cy="44" r="10"/><circle fill="none" stroke="#2B2B33" stroke-width="5" cx="50" cy="44" r="10"/><path fill="none" stroke="#D33" stroke-width="5" d="M14 44 L26 30 L40 30 L50 44"/><path fill="none" stroke="#2B2B33" stroke-width="4" d="M26 27 L38 27 M40 30 L45 20 L52 20"/><circle fill="#F2C94C" cx="33" cy="38" r="5"/>)"},
    {"bicycle", "Bicycle", "Vehicles", R"(<circle fill="none" stroke="#2B2B33" stroke-width="3" cx="14" cy="42" r="11"/><circle fill="none" stroke="#2B2B33" stroke-width="3" cx="50" cy="42" r="11"/><path fill="none" stroke="#D33" stroke-width="3" d="M14 42 L28 42 L37 24 L22 24 L28 42 M37 24 L50 42 M37 24 L35 17 M31 17 L39 17"/>)"},
    {"airplane", "Airplane", "Vehicles", R"(<path fill="#B8C6D6" d="M32 22 L60 38 L60 44 L32 36 L4 44 L4 38 Z"/><path fill="#B8C6D6" d="M32 50 L44 58 L44 62 L32 58 L20 62 L20 58 Z"/><ellipse fill="#ECF1F7" cx="32" cy="32" rx="6" ry="28"/><ellipse fill="#3B82F6" cx="32" cy="12" rx="3" ry="4"/>)"},
    {"helicopter", "Helicopter", "Vehicles", R"(<ellipse fill="#D33" cx="26" cy="38" rx="18" ry="11"/><path fill="#D33" d="M40 34 L60 28 L60 33 L42 43 Z"/><path fill="none" stroke="#2B2B33" stroke-width="3" d="M6 20 L48 20 M27 20 L27 28 M10 54 L44 54 M18 46 L16 54 M36 46 L38 54"/><ellipse fill="#BEE3F8" cx="19" cy="36" rx="8" ry="6"/>)"},
    {"boat", "Sailboat", "Vehicles", R"(<path fill="#F8F8F8" d="M32 6 L32 38 L54 38 Z"/><path fill="#E04848" d="M28 12 L28 38 L12 38 Z"/><path fill="#3B6FD0" d="M6 40 L58 40 L50 54 L14 54 Z"/><path fill="none" stroke="#8FD0F0" stroke-width="2" d="M4 58 C10 54 14 62 20 58 C26 54 30 62 36 58 C42 54 46 62 52 58 C56 55 59 58 60 58"/>)"},
    {"skateboard", "Skateboard", "Vehicles", R"(<path fill="#9B59B6" d="M6 28 L58 28 C62 28 62 35 58 35 L6 35 C2 35 2 28 6 28 Z"/><rect fill="#777" x="14" y="36" width="6" height="4"/><rect fill="#777" x="44" y="36" width="6" height="4"/><circle fill="#F2C94C" cx="17" cy="44" r="5"/><circle fill="#F2C94C" cx="47" cy="44" r="5"/>)"},
    {"scooter", "Scooter", "Vehicles", R"(<circle fill="#2B2B33" cx="12" cy="50" r="7"/><circle fill="#2B2B33" cx="52" cy="50" r="7"/><rect fill="#38BDF8" x="10" y="42" width="40" height="5" rx="2"/><path fill="none" stroke="#555" stroke-width="4" d="M50 44 L44 8 M38 8 L50 8"/>)"},
    {"rocket", "Rocket", "Vehicles", R"(<path fill="#ECF1F7" d="M32 3 C46 14 46 34 41 48 L23 48 C18 34 18 14 32 3 Z"/><path fill="#D33" d="M24 34 L12 52 L24 48 Z M40 34 L52 52 L40 48 Z"/><circle fill="#38BDF8" cx="32" cy="22" r="6"/><path fill="#F2994A" d="M27 48 L37 48 L32 62 Z"/>)"},
    // ---- Weapons & tools
    {"sword", "Sword", "Weapons & tools", R"(<path fill="#D5DCE5" d="M30 3 L34 3 L36 40 L28 40 Z"/><rect fill="#C79A3A" x="18" y="40" width="28" height="5" rx="2"/><rect fill="#6B4A2B" x="29" y="45" width="6" height="11"/><circle fill="#C79A3A" cx="32" cy="58" r="3.5"/>)"},
    {"shield", "Shield", "Weapons & tools", R"(<path fill="#3B6FD0" d="M9 9 L55 9 L55 34 C55 48 41 56 32 61 C23 56 9 48 9 34 Z"/><path fill="#F2C94C" d="M32 17 L41 28 L32 46 L23 28 Z"/><path fill="none" stroke="#2A4FA0" stroke-width="3" d="M9 9 L55 9 L55 34 C55 48 41 56 32 61 C23 56 9 48 9 34 Z"/>)"},
    {"bow", "Bow and arrow", "Weapons & tools", R"(<path fill="none" stroke="#8B6238" stroke-width="4" d="M20 5 C50 20 50 44 20 59"/><path fill="none" stroke="#ddd" stroke-width="1.5" d="M20 5 L20 59"/><path fill="none" stroke="#A8A8A8" stroke-width="2" d="M8 32 L54 32"/><polygon fill="#888" points="58,32 51,27 51,37"/>)"},
    {"pistol", "Pistol", "Weapons & tools", R"(<rect fill="#4B5563" x="5" y="17" width="46" height="12" rx="2"/><path fill="#374151" d="M30 28 L44 28 L40 52 L28 52 Z"/><path fill="none" stroke="#374151" stroke-width="2.5" d="M26 29 C26 38 34 38 34 29"/><rect fill="#2B2F36" x="5" y="17" width="6" height="5"/>)"},
    {"watergun", "Water gun", "Weapons & tools", R"(<path fill="#29B6F6" d="M6 22 L40 22 L47 29 L47 38 L27 38 L27 54 L16 54 L16 38 L6 38 Z"/><rect fill="#81D4FA" x="20" y="11" width="20" height="11" rx="4"/><rect fill="#0288D1" x="2" y="25" width="6" height="7"/><rect fill="#0277BD" x="29" y="40" width="6" height="9" rx="2"/><circle fill="#81D4FA" cx="58" cy="26" r="3"/><circle fill="#81D4FA" cx="60" cy="34" r="2"/>)"},
    {"chair", "Chair", "Weapons & tools", R"(<rect fill="#A9743E" x="19" y="5" width="26" height="28" rx="3"/><path fill="none" stroke="#7B5329" stroke-width="2" d="M26 8 L26 30 M32 8 L32 30 M38 8 L38 30"/><rect fill="#8B5E2F" x="15" y="32" width="34" height="9" rx="2"/><rect fill="#6B4721" x="17" y="41" width="6" height="19"/><rect fill="#6B4721" x="41" y="41" width="6" height="19"/>)"},
    {"hammer", "Hammer", "Weapons & tools", R"(<rect fill="#8B6238" x="28" y="14" width="8" height="46" rx="2"/><rect fill="#8E99A8" x="12" y="6" width="40" height="15" rx="3"/><rect fill="#6B7685" x="12" y="6" width="8" height="15" rx="2"/>)"},
    {"axe", "Axe", "Weapons & tools", R"(<rect fill="#8B6238" x="28" y="6" width="6" height="52" rx="2"/><path fill="#B8C4D2" d="M34 10 C54 5 60 24 51 35 C47 27 41 25 34 25 Z"/>)"},
    {"knife", "Knife", "Weapons & tools", R"(<path fill="#D5DCE5" d="M30 3 C39 14 39 30 35 43 L28 43 Z"/><rect fill="#4B3A2A" x="27" y="43" width="9" height="16" rx="2"/><rect fill="#C79A3A" x="25" y="41" width="13" height="3"/>)"},
    {"bat", "Baseball bat", "Weapons & tools", R"(<path fill="#D9A66A" d="M27 3 L37 3 L38 38 L35 60 L29 60 L26 38 Z"/><rect fill="#8B6238" x="29" y="50" width="6" height="10" rx="2"/>)"},
    {"umbrella", "Umbrella", "Weapons & tools", R"(<path fill="#E04848" d="M5 32 C5 8 59 8 59 32 C53 28 47 28 42 32 C38 28 26 28 22 32 C17 28 11 28 5 32 Z"/><path fill="none" stroke="#333" stroke-width="3" d="M32 32 L32 54 C32 61 24 61 24 54"/>)"},
    {"flashlight", "Flashlight", "Weapons & tools", R"(<rect fill="#4B5563" x="24" y="24" width="16" height="35" rx="3"/><path fill="#6B7280" d="M19 6 L45 6 L40 24 L24 24 Z"/><rect fill="#FDE68A" x="21" y="4" width="22" height="4" rx="2"/><circle fill="#F2C94C" cx="32" cy="36" r="3"/>)"},
    {"torch", "Torch", "Weapons & tools", R"(<rect fill="#8B6238" x="28" y="26" width="8" height="34" rx="2"/><path fill="#F2994A" d="M32 2 C44 14 44 24 40 28 L24 28 C20 24 20 14 32 2 Z"/><path fill="#FDE68A" d="M32 14 C38 20 38 26 35 28 L29 28 C26 26 26 20 32 14 Z"/>)"},
    {"wrench", "Wrench", "Weapons & tools", R"(<path fill="none" stroke="#8E99A8" stroke-width="8" d="M12 54 L36 30"/><circle fill="none" stroke="#8E99A8" stroke-width="7" cx="44" cy="20" r="9"/>)"},
    {"rope", "Rope", "Weapons & tools", R"(<circle fill="none" stroke="#C9A66B" stroke-width="6" cx="30" cy="32" r="21"/><circle fill="none" stroke="#B08A4D" stroke-width="5" cx="30" cy="32" r="11"/><path fill="none" stroke="#C9A66B" stroke-width="6" d="M48 46 C54 52 52 58 58 60"/>)"},
    {"lightsaber", "Laser sword", "Weapons & tools", R"(<rect fill="#5CF0FF" opacity="0.35" x="25" y="3" width="14" height="37" rx="7"/><rect fill="#5CF0FF" x="29" y="4" width="6" height="36" rx="3"/><rect fill="#E8F8FF" x="31" y="6" width="2" height="32"/><rect fill="#555" x="28" y="40" width="8" height="19" rx="2"/><rect fill="#999" x="26" y="40" width="12" height="3"/>)"},
    {"staff", "Magic staff", "Weapons & tools", R"(<path fill="none" stroke="#8B6238" stroke-width="5" d="M32 14 L32 61"/><circle fill="#5CF0FF" opacity="0.4" cx="32" cy="10" r="10"/><circle fill="#5CF0FF" cx="32" cy="10" r="7"/>)"},
    {"wand", "Magic wand", "Weapons & tools", R"(<path fill="none" stroke="#2B2B33" stroke-width="5" d="M10 56 L44 22"/><polygon fill="#F2C94C" points="48,8 51,16 59,17 53,23 54,31 48,27 41,31 43,23 37,17 45,16"/><circle fill="#F2C94C" cx="14" cy="22" r="2.5"/><circle fill="#F2C94C" cx="58" cy="44" r="2"/>)"},
    // ---- Backgrounds (scenes)
    {"bg_nightcity", "City street at night", "Backgrounds", R"(<rect fill="#0B1026" x="0" y="0" width="96" height="54"/><rect fill="#1B2347" x="3" y="12" width="15" height="30"/><rect fill="#232C5C" x="19" y="6" width="13" height="36"/><rect fill="#1B2347" x="33" y="16" width="16" height="26"/><rect fill="#232C5C" x="50" y="9" width="14" height="33"/><rect fill="#1B2347" x="65" y="14" width="13" height="28"/><rect fill="#232C5C" x="79" y="8" width="16" height="34"/><rect fill="#F2D16B" x="6" y="16" width="3" height="3"/><rect fill="#F2D16B" x="11" y="22" width="3" height="3"/><rect fill="#F2D16B" x="22" y="12" width="3" height="3"/><rect fill="#F2D16B" x="27" y="20" width="3" height="3"/><rect fill="#F2D16B" x="22" y="28" width="3" height="3"/><rect fill="#F2D16B" x="37" y="21" width="3" height="3"/><rect fill="#F2D16B" x="53" y="14" width="3" height="3"/><rect fill="#F2D16B" x="58" y="24" width="3" height="3"/><rect fill="#F2D16B" x="69" y="19" width="3" height="3"/><rect fill="#F2D16B" x="83" y="13" width="3" height="3"/><rect fill="#F2D16B" x="88" y="26" width="3" height="3"/><rect fill="#FF3DA5" x="35" y="25" width="12" height="4"/><rect fill="#38E1FF" x="66" y="22" width="9" height="3"/><rect fill="#2B2B35" x="0" y="42" width="96" height="12"/><rect fill="#E8E8F0" x="6" y="48" width="10" height="1.2"/><rect fill="#E8E8F0" x="30" y="48" width="10" height="1.2"/><rect fill="#E8E8F0" x="54" y="48" width="10" height="1.2"/><rect fill="#E8E8F0" x="78" y="48" width="10" height="1.2"/><rect fill="#FF3DA5" opacity="0.3" x="35" y="43" width="12" height="6"/><rect fill="#38E1FF" opacity="0.3" x="66" y="44" width="9" height="6"/>)", 96, 54, "a rainy neon city street at night, wet asphalt reflecting the signs"},
    {"bg_daycity", "City skyline (day)", "Backgrounds", R"(<rect fill="#8EC5F2" x="0" y="0" width="96" height="54"/><circle fill="#FFE08A" cx="80" cy="12" r="6"/><rect fill="#B9C6D6" x="4" y="20" width="12" height="26"/><rect fill="#8FA1B8" x="17" y="12" width="14" height="34"/><rect fill="#B9C6D6" x="32" y="22" width="11" height="24"/><rect fill="#7C8FA8" x="44" y="8" width="13" height="38"/><rect fill="#B9C6D6" x="58" y="18" width="12" height="28"/><rect fill="#8FA1B8" x="71" y="24" width="12" height="22"/><rect fill="#B9C6D6" x="84" y="16" width="12" height="30"/><rect fill="#DCEBF8" x="20" y="16" width="3" height="3"/><rect fill="#DCEBF8" x="25" y="24" width="3" height="3"/><rect fill="#DCEBF8" x="48" y="14" width="3" height="3"/><rect fill="#DCEBF8" x="48" y="24" width="3" height="3"/><rect fill="#DCEBF8" x="7" y="26" width="3" height="3"/><rect fill="#6B7380" x="0" y="46" width="96" height="8"/><rect fill="#F5F5F5" x="8" y="50" width="9" height="1"/><rect fill="#F5F5F5" x="38" y="50" width="9" height="1"/><rect fill="#F5F5F5" x="68" y="50" width="9" height="1"/>)", 96, 54, "a sunny city street with tall buildings under a blue sky"},
    {"bg_forest", "Forest", "Backgrounds", R"(<rect fill="#BFE3C0" x="0" y="0" width="96" height="54"/><rect fill="#2F6B3A" x="0" y="38" width="96" height="16"/><polygon fill="#1F5A2E" points="10,40 20,8 30,40"/><polygon fill="#2A7038" points="26,42 38,12 50,42"/><polygon fill="#1F5A2E" points="46,40 58,6 70,40"/><polygon fill="#2A7038" points="64,42 76,14 88,42"/><polygon fill="#1F5A2E" points="80,40 90,18 96,40"/><rect fill="#5C3D1E" x="19" y="40" width="3" height="8"/><rect fill="#5C3D1E" x="37" y="42" width="3" height="8"/><rect fill="#5C3D1E" x="57" y="40" width="3" height="8"/><rect fill="#5C3D1E" x="75" y="42" width="3" height="8"/><path fill="#8B6B3E" d="M30 54 C40 46 56 46 66 54 Z"/>)", 96, 54, "a dense green forest with tall pine trees and a dirt path"},
    {"bg_beach", "Beach", "Backgrounds", R"(<rect fill="#7CC8F5" x="0" y="0" width="96" height="54"/><circle fill="#FFE08A" cx="76" cy="12" r="7"/><rect fill="#2E86C1" x="0" y="26" width="96" height="10"/><rect fill="#5DADE2" x="0" y="30" width="96" height="6"/><path fill="#FFFFFF" opacity="0.8" d="M0 36 C8 33 14 39 22 36 C30 33 36 39 44 36 C52 33 58 39 66 36 C74 33 80 39 88 36 L96 36 L96 38 L0 38 Z"/><rect fill="#F1D9A0" x="0" y="37" width="96" height="17"/><rect fill="#7B5B2E" x="14" y="16" width="3" height="26"/><path fill="#2E8B3E" d="M15 16 C8 12 4 16 2 20 C8 17 12 17 15 18 Z M15 16 C22 11 27 14 29 19 C23 16 19 16 15 18 Z M15 16 C14 8 20 6 24 8 C19 9 16 12 15 16 Z"/>)", 96, 54, "a sunny tropical beach with calm blue sea and a palm tree"},
    {"bg_desert", "Desert", "Backgrounds", R"(<rect fill="#F6C177" x="0" y="0" width="96" height="54"/><circle fill="#FFF1B8" cx="20" cy="14" r="8"/><path fill="#E0A458" d="M0 34 C20 24 36 40 56 32 C72 26 86 34 96 30 L96 54 L0 54 Z"/><path fill="#C98A3E" d="M0 44 C24 36 40 50 64 42 C78 38 88 44 96 42 L96 54 L0 54 Z"/><rect fill="#3F7A3A" x="70" y="22" width="4" height="16" rx="2"/><rect fill="#3F7A3A" x="65" y="26" width="3" height="7" rx="1.5"/><rect fill="#3F7A3A" x="76" y="25" width="3" height="6" rx="1.5"/>)", 96, 54, "a hot open desert with sand dunes and a cactus"},
    {"bg_mountains", "Mountains", "Backgrounds", R"(<rect fill="#9CC8EE" x="0" y="0" width="96" height="54"/><polygon fill="#7C8FA8" points="0,40 24,10 48,40"/><polygon fill="#97A8BE" points="30,40 56,6 86,40"/><polygon fill="#FFFFFF" points="24,10 19,18 24,16 29,19"/><polygon fill="#FFFFFF" points="56,6 50,16 56,14 62,17"/><rect fill="#3E7A3C" x="0" y="38" width="96" height="16"/><rect fill="#4C90C8" x="22" y="42" width="52" height="8" rx="4"/><polygon fill="#1F5A2E" points="6,40 11,26 16,40"/><polygon fill="#1F5A2E" points="82,42 88,26 94,42"/>)", 96, 54, "snow-capped mountains above a green valley with a lake"},
    {"bg_snow", "Snowy field", "Backgrounds", R"(<rect fill="#CFE6F5" x="0" y="0" width="96" height="54"/><rect fill="#F4F9FD" x="0" y="32" width="96" height="22"/><path fill="#E3EEF6" d="M0 40 C20 34 40 44 60 38 C76 34 88 40 96 38 L96 54 L0 54 Z"/><polygon fill="#2A6B4A" points="10,36 18,12 26,36"/><polygon fill="#2A6B4A" points="70,38 78,16 86,38"/><polygon fill="#FFFFFF" points="18,12 14,22 22,22"/><polygon fill="#FFFFFF" points="78,16 74,26 82,26"/><circle fill="#FFFFFF" cx="40" cy="12" r="1.2"/><circle fill="#FFFFFF" cx="56" cy="20" r="1.2"/><circle fill="#FFFFFF" cx="30" cy="24" r="1.2"/>)", 96, 54, "a quiet snowy field with pine trees under a pale winter sky"},
    {"bg_space", "Outer space", "Backgrounds", R"(<rect fill="#05060F" x="0" y="0" width="96" height="54"/><circle fill="#FFFFFF" cx="8" cy="8" r="0.7"/><circle fill="#FFFFFF" cx="22" cy="20" r="0.6"/><circle fill="#FFFFFF" cx="40" cy="6" r="0.8"/><circle fill="#FFFFFF" cx="60" cy="14" r="0.6"/><circle fill="#FFFFFF" cx="84" cy="8" r="0.8"/><circle fill="#FFFFFF" cx="90" cy="30" r="0.6"/><circle fill="#FFFFFF" cx="14" cy="40" r="0.7"/><circle fill="#FFFFFF" cx="50" cy="44" r="0.6"/><circle fill="#FFFFFF" cx="72" cy="48" r="0.8"/><circle fill="#7A4BD8" cx="66" cy="26" r="14"/><circle fill="#5B34B0" cx="62" cy="30" r="9" opacity="0.6"/><ellipse fill="none" stroke="#D9C7FF" stroke-width="1.6" cx="66" cy="26" rx="22" ry="5"/><circle fill="#E8A33D" cx="20" cy="34" r="6"/>)", 96, 54, "deep outer space with stars, a purple ringed planet and a small orange moon"},
    {"bg_livingroom", "Living room", "Backgrounds", R"(<rect fill="#D9C7A7" x="0" y="0" width="96" height="54"/><rect fill="#8B6238" x="0" y="38" width="96" height="16"/><rect fill="#9ED0F0" x="58" y="8" width="26" height="20"/><rect fill="#FFFFFF" x="70" y="8" width="2" height="20"/><rect fill="#FFFFFF" x="58" y="17" width="26" height="2"/><rect fill="#B04A4A" x="8" y="26" width="38" height="16" rx="3"/><rect fill="#8F3A3A" x="8" y="20" width="38" height="9" rx="3"/><rect fill="#6B4A2B" x="52" y="36" width="22" height="5" rx="1"/><rect fill="#3E2A17" x="54" y="41" width="2" height="8"/><rect fill="#3E2A17" x="70" y="41" width="2" height="8"/><rect fill="#F2C94C" x="88" y="14" width="3" height="22"/><circle fill="#FFE9A8" cx="89.5" cy="12" r="4"/>)", 96, 54, "a cosy living room with a red sofa, a window, a coffee table and a floor lamp"},
    {"bg_castle", "Castle", "Backgrounds", R"(<rect fill="#A8D4F2" x="0" y="0" width="96" height="54"/><rect fill="#5FA05A" x="0" y="40" width="96" height="14"/><rect fill="#9C9CA8" x="26" y="18" width="44" height="26"/><rect fill="#868693" x="20" y="10" width="12" height="34"/><rect fill="#868693" x="64" y="10" width="12" height="34"/><polygon fill="#C0392B" points="18,10 26,0 34,10"/><polygon fill="#C0392B" points="62,10 70,0 78,10"/><rect fill="#868693" x="42" y="12" width="12" height="32"/><polygon fill="#C0392B" points="40,12 48,2 56,12"/><path fill="#4B3621" d="M43 44 L43 33 C43 27 53 27 53 33 L53 44 Z"/><rect fill="#2B2B33" x="24" y="20" width="3" height="5"/><rect fill="#2B2B33" x="68" y="20" width="3" height="5"/><rect fill="#FFFFFF" x="6" y="8" width="14" height="4" rx="2"/>)", 96, 54, "a stone castle with red-roofed towers on a green hill"},
    {"bg_sea", "Open sea", "Backgrounds", R"(<rect fill="#9ED7F5" x="0" y="0" width="96" height="30"/><rect fill="#1F6FA8" x="0" y="26" width="96" height="28"/><rect fill="#2E86C1" x="0" y="34" width="96" height="20"/><path fill="none" stroke="#BFE6FA" stroke-width="1.4" d="M2 32 C8 29 14 35 20 32 C26 29 32 35 38 32 C44 29 50 35 56 32 C62 29 68 35 74 32 C80 29 86 35 92 32"/><path fill="none" stroke="#BFE6FA" stroke-width="1.4" d="M0 42 C8 39 14 45 22 42 C30 39 36 45 44 42 C52 39 58 45 66 42 C74 39 80 45 88 42 L96 42"/><path fill="none" stroke="#BFE6FA" stroke-width="1.4" d="M4 50 C12 47 18 53 26 50 C34 47 40 53 48 50 C56 47 62 53 70 50 C78 47 84 53 92 50"/><ellipse fill="#FFFFFF" cx="20" cy="10" rx="9" ry="3"/><ellipse fill="#FFFFFF" cx="68" cy="14" rx="11" ry="3.5"/>)", 96, 54, "the open sea with rolling waves under a bright sky"},
    {"bg_sunset", "Sunset hills", "Backgrounds", R"(<rect fill="#3B2A63" x="0" y="0" width="96" height="14"/><rect fill="#B2456E" x="0" y="14" width="96" height="10"/><rect fill="#F07C4A" x="0" y="24" width="96" height="10"/><rect fill="#F8C65B" x="0" y="32" width="96" height="10"/><circle fill="#FFF1B8" cx="48" cy="36" r="10"/><path fill="#2A1F3D" d="M0 40 C16 30 34 40 52 36 C70 32 84 38 96 34 L96 54 L0 54 Z"/><path fill="#1B1430" d="M0 48 C20 42 42 52 64 46 C78 43 88 47 96 45 L96 54 L0 54 Z"/>)", 96, 54, "rolling hills against a glowing orange and purple sunset"},
    {"bg_sky", "Sky and clouds", "Backgrounds", R"(<rect fill="#2F8BD8" x="0" y="0" width="96" height="54"/><rect fill="#5AA9EA" x="0" y="20" width="96" height="34"/><rect fill="#8CC8F5" x="0" y="38" width="96" height="16"/><ellipse fill="#FFFFFF" cx="20" cy="14" rx="13" ry="4.5"/><ellipse fill="#FFFFFF" cx="30" cy="11" rx="8" ry="4"/><ellipse fill="#FFFFFF" cx="70" cy="26" rx="16" ry="5"/><ellipse fill="#FFFFFF" cx="82" cy="23" rx="9" ry="4.5"/><ellipse fill="#FFFFFF" cx="38" cy="44" rx="18" ry="5"/><ellipse fill="#FFFFFF" cx="52" cy="41" rx="10" ry="4.5"/><ellipse fill="#FFFFFF" cx="8" cy="36" rx="9" ry="3"/>)", 96, 54, "high up in a bright blue sky with white clouds, perfect for an aerial fight"},
    {"bg_rooftop", "Rooftop at night", "Backgrounds", R"(<rect fill="#0A0E24" x="0" y="0" width="96" height="54"/><circle fill="#F4F1DE" cx="78" cy="10" r="5"/><rect fill="#1A2144" x="4" y="22" width="10" height="20"/><rect fill="#222A55" x="16" y="16" width="12" height="26"/><rect fill="#1A2144" x="30" y="24" width="9" height="18"/><rect fill="#222A55" x="42" y="14" width="11" height="28"/><rect fill="#1A2144" x="56" y="20" width="12" height="22"/><rect fill="#222A55" x="70" y="26" width="10" height="16"/><rect fill="#F2D16B" x="19" y="22" width="2" height="2"/><rect fill="#F2D16B" x="46" y="20" width="2" height="2"/><rect fill="#F2D16B" x="59" y="27" width="2" height="2"/><rect fill="#F2D16B" x="7" y="28" width="2" height="2"/><rect fill="#3A3A46" x="0" y="42" width="96" height="12"/><rect fill="#2B2B35" x="0" y="42" width="96" height="3"/><rect fill="#555566" x="76" y="34" width="14" height="8"/><rect fill="#555566" x="8" y="36" width="3" height="6"/>)", 96, 54, "a flat city rooftop at night with the skyline and a full moon behind"},
    {"bg_arena", "Fighting arena", "Backgrounds", R"(<rect fill="#14141C" x="0" y="0" width="96" height="54"/><polygon fill="#F2E7B0" opacity="0.18" points="20,0 30,0 40,40 10,40"/><polygon fill="#F2E7B0" opacity="0.18" points="66,0 76,0 86,40 56,40"/><rect fill="#2A2A38" x="0" y="14" width="96" height="14"/><rect fill="#3B3B4D" x="6" y="30" width="84" height="18" rx="2"/><rect fill="#B33030" x="10" y="32" width="76" height="14" rx="1"/><rect fill="#E8E8F0" x="8" y="26" width="80" height="1.6"/><rect fill="#E04848" x="8" y="22" width="80" height="1.6"/><rect fill="#E8E8F0" x="8" y="18" width="80" height="1.6"/><rect fill="#E8E8F0" x="6" y="16" width="3" height="32"/><rect fill="#E8E8F0" x="87" y="16" width="3" height="32"/>)", 96, 54, "a boxing-style fighting arena with ring ropes, a red mat and bright spotlights"},
    {"bg_dojo", "Dojo", "Backgrounds", R"(<rect fill="#E9DDBF" x="0" y="0" width="96" height="40"/><rect fill="#B58A52" x="0" y="38" width="96" height="16"/><rect fill="#6B4A2B" x="0" y="0" width="96" height="4"/><rect fill="#6B4A2B" x="0" y="36" width="96" height="3"/><rect fill="#6B4A2B" x="30" y="4" width="2" height="32"/><rect fill="#6B4A2B" x="64" y="4" width="2" height="32"/><rect fill="#6B4A2B" x="0" y="4" width="2" height="32"/><rect fill="#6B4A2B" x="94" y="4" width="2" height="32"/><circle fill="#C0392B" cx="48" cy="20" r="9"/><rect fill="#D9C79F" x="6" y="8" width="20" height="26"/><rect fill="#D9C79F" x="70" y="8" width="20" height="26"/><rect fill="#9E7342" x="0" y="46" width="96" height="1"/><rect fill="#9E7342" x="0" y="50" width="96" height="1"/>)", 96, 54, "a traditional wooden dojo with paper screens and a red sun banner"},
    {"bg_alley", "Back alley", "Backgrounds", R"(<rect fill="#2C2A33" x="0" y="0" width="96" height="54"/><rect fill="#8B4A3A" x="0" y="0" width="30" height="42"/><rect fill="#7A3F31" x="66" y="0" width="30" height="42"/><rect fill="#3B3A44" x="30" y="0" width="36" height="30"/><rect fill="#46454F" x="0" y="42" width="96" height="12"/><path fill="#3B3A44" d="M30 30 L66 30 L66 42 L30 42 Z"/><rect fill="#2E7D4F" x="36" y="30" width="22" height="12" rx="1"/><rect fill="#1F5A38" x="36" y="30" width="22" height="3"/><rect fill="#F2D16B" x="6" y="8" width="5" height="7"/><rect fill="#F2D16B" x="80" y="14" width="5" height="7"/><rect fill="#6B3226" x="0" y="14" width="30" height="1"/><rect fill="#6B3226" x="0" y="26" width="30" height="1"/><rect fill="#5E2B20" x="66" y="20" width="30" height="1"/><rect fill="#5E2B20" x="66" y="32" width="30" height="1"/>)", 96, 54, "a narrow dark brick alley with a green dumpster and dim windows"},
    {"bg_classroom", "Classroom", "Backgrounds", R"(<rect fill="#E6D8B8" x="0" y="0" width="96" height="54"/><rect fill="#8B6238" x="0" y="40" width="96" height="14"/><rect fill="#2F5D45" x="22" y="6" width="52" height="24"/><rect fill="#B8946A" x="20" y="4" width="56" height="2.5"/><rect fill="#B8946A" x="20" y="29" width="56" height="2.5"/><rect fill="#FFFFFF" x="30" y="12" width="14" height="1.5"/><rect fill="#FFFFFF" x="30" y="17" width="22" height="1.5"/><rect fill="#B8946A" x="8" y="34" width="22" height="3"/><rect fill="#6B4A2B" x="10" y="37" width="2" height="9"/><rect fill="#6B4A2B" x="26" y="37" width="2" height="9"/><rect fill="#B8946A" x="66" y="34" width="22" height="3"/><rect fill="#6B4A2B" x="68" y="37" width="2" height="9"/><rect fill="#6B4A2B" x="84" y="37" width="2" height="9"/><circle fill="#F4F4F4" cx="86" cy="12" r="5"/>)", 96, 54, "a school classroom with a green chalkboard and wooden desks"},
    {"bg_park", "Park", "Backgrounds", R"(<rect fill="#A9DCF5" x="0" y="0" width="96" height="54"/><rect fill="#5FB04F" x="0" y="30" width="96" height="24"/><path fill="#D8C9A0" d="M40 54 C44 44 52 38 60 30 L68 30 C62 40 58 46 62 54 Z"/><rect fill="#6B4A2B" x="14" y="22" width="3" height="16"/><circle fill="#2F8F3F" cx="15.5" cy="18" r="10"/><circle fill="#3AA64A" cx="11" cy="22" r="6"/><rect fill="#6B4A2B" x="78" y="20" width="3" height="18"/><circle fill="#2F8F3F" cx="79.5" cy="15" r="11"/><rect fill="#8B6238" x="24" y="36" width="16" height="2.5"/><rect fill="#8B6238" x="24" y="32" width="16" height="2"/><rect fill="#5C3D1E" x="26" y="38" width="2" height="5"/><rect fill="#5C3D1E" x="36" y="38" width="2" height="5"/><ellipse fill="#FFFFFF" cx="46" cy="9" rx="10" ry="3"/>)", 96, 54, "a sunny city park with big trees, grass, a path and a wooden bench"},
    {"bg_highway", "Highway", "Backgrounds", R"(<rect fill="#8EC5F2" x="0" y="0" width="96" height="30"/><polygon fill="#7C8FA8" points="0,30 20,14 40,30"/><polygon fill="#97A8BE" points="30,30 56,12 84,30"/><rect fill="#5FA05A" x="0" y="28" width="96" height="26"/><polygon fill="#44444E" points="40,28 56,28 96,54 0,54"/><polygon fill="#E8E8F0" points="47,30 49,30 49,36 46,36"/><polygon fill="#E8E8F0" points="46,40 50,40 52,48 44,48"/><polygon fill="#E8E8F0" points="44,52 52,52 53,54 43,54"/><rect fill="#F2C94C" x="0" y="52" width="0" height="0"/>)", 96, 54, "an empty highway stretching toward the mountains on the horizon"},
    // ---- Everyday
    {"phone", "Smartphone", "Everyday", R"(<rect fill="#1F2937" x="17" y="3" width="30" height="58" rx="5"/><rect fill="#60A5FA" x="20" y="9" width="24" height="42" rx="1"/><circle fill="#4B5563" cx="32" cy="56" r="2.5"/>)"},
    {"camera", "Camera", "Everyday", R"(<rect fill="#374151" x="5" y="18" width="54" height="36" rx="5"/><rect fill="#4B5563" x="11" y="11" width="14" height="8" rx="2"/><circle fill="#111827" cx="32" cy="36" r="13"/><circle fill="#60A5FA" cx="32" cy="36" r="8"/><circle fill="#F2C94C" cx="51" cy="25" r="2.5"/>)"},
    {"book", "Book", "Everyday", R"(<rect fill="#7C3AED" x="11" y="6" width="42" height="52" rx="3"/><rect fill="#5B21B6" x="11" y="6" width="7" height="52" rx="2"/><rect fill="#F5F0E1" x="24" y="14" width="22" height="10" rx="1"/><rect fill="#F5F0E1" x="24" y="28" width="22" height="3"/>)"},
    {"guitar", "Guitar", "Everyday", R"(<rect fill="#6B4A2B" x="29" y="3" width="6" height="28"/><rect fill="#4C3320" x="27" y="1" width="10" height="8" rx="2"/><circle fill="#D9893B" cx="32" cy="38" r="9"/><circle fill="#D9893B" cx="32" cy="48" r="12"/><circle fill="#3B2A17" cx="32" cy="43" r="4"/>)"},
    {"soccerball", "Soccer ball", "Everyday", R"(<circle fill="#fff" stroke="#333" stroke-width="2" cx="32" cy="32" r="26"/><polygon fill="#333" points="32,18 44,27 40,41 24,41 20,27"/><path fill="none" stroke="#333" stroke-width="2" d="M32 18 L32 7 M44 27 L55 23 M40 41 L47 51 M24 41 L17 51 M20 27 L9 23"/>)"},
    {"balloon", "Balloon", "Everyday", R"(<ellipse fill="#E04848" cx="32" cy="25" rx="17" ry="21"/><polygon fill="#B73030" points="28,46 36,46 32,41"/><path fill="none" stroke="#777" stroke-width="1.5" d="M32 46 C27 52 37 56 32 62"/><ellipse fill="#fff" opacity="0.35" cx="25" cy="16" rx="4" ry="7"/>)"},
    {"lantern", "Lantern", "Everyday", R"(<path fill="none" stroke="#555" stroke-width="3" d="M22 8 C22 0 42 0 42 8"/><rect fill="#555" x="22" y="7" width="20" height="6" rx="2"/><rect fill="#D9893B" x="17" y="13" width="30" height="38" rx="5"/><rect fill="#F8E08E" x="22" y="18" width="20" height="28" rx="3"/><rect fill="#555" x="20" y="51" width="24" height="6" rx="2"/>)"},
    {"key", "Key", "Everyday", R"(<circle fill="none" stroke="#C79A3A" stroke-width="7" cx="17" cy="32" r="11"/><rect fill="#C79A3A" x="27" y="29" width="33" height="7"/><rect fill="#C79A3A" x="46" y="36" width="5" height="9"/><rect fill="#C79A3A" x="55" y="36" width="5" height="7"/>)"},
    {"potion", "Potion", "Everyday", R"(<path fill="#E8EEF5" opacity="0.85" d="M26 7 L38 7 L38 22 L52 50 C55 57 50 61 44 61 L20 61 C14 61 9 57 12 50 L26 22 Z"/><path fill="#9B59B6" d="M17 44 L47 44 L52 50 C55 57 50 61 44 61 L20 61 C14 61 9 57 12 50 Z"/><rect fill="#8B6238" x="25" y="2" width="14" height="7" rx="2"/>)"},
    {"coffee", "Coffee cup", "Everyday", R"(<path fill="none" stroke="#fff" stroke-width="4" d="M44 26 C56 26 56 42 42 42"/><path fill="#F8F8F8" d="M10 20 L46 20 L42 54 C41 58 15 58 14 54 Z"/><ellipse fill="#6B4A2B" cx="28" cy="20" rx="18" ry="4"/><path fill="none" stroke="#bbb" stroke-width="2.5" d="M20 14 C16 8 24 8 20 2 M30 14 C26 8 34 8 30 2"/>)"},
    {"pizza", "Pizza slice", "Everyday", R"(<path fill="#F2B632" d="M32 60 L5 14 C20 5 44 5 59 14 Z"/><path fill="none" stroke="#C98A1B" stroke-width="6" d="M5 14 C20 5 44 5 59 14"/><circle fill="#C0392B" cx="24" cy="22" r="4.5"/><circle fill="#C0392B" cx="40" cy="22" r="4.5"/><circle fill="#C0392B" cx="32" cy="36" r="4.5"/>)"},
    {"burger", "Burger", "Everyday", R"(<path fill="#E3A857" d="M7 28 C7 8 57 8 57 28 Z"/><path fill="#6BBF59" d="M6 30 L58 30 L54 36 L48 33 L42 37 L36 33 L30 37 L24 33 L18 37 L12 33 L8 36 Z"/><rect fill="#6B3E26" x="6" y="35" width="52" height="8" rx="4"/><rect fill="#E3A857" x="8" y="44" width="48" height="10" rx="5"/>)"},
    {"trophy", "Trophy", "Everyday", R"(<path fill="none" stroke="#D9A62E" stroke-width="4" d="M18 12 C6 12 6 28 20 30 M46 12 C58 12 58 28 44 30"/><path fill="#F2C94C" d="M16 6 L48 6 L45 28 C43 38 21 38 19 28 Z"/><rect fill="#D9A62E" x="29" y="36" width="6" height="12"/><rect fill="#D9A62E" x="19" y="47" width="26" height="10" rx="2"/>)"},
    {"coin", "Gold coin", "Everyday", R"(<circle fill="#D9A62E" cx="32" cy="32" r="26"/><circle fill="#F2C94C" cx="32" cy="32" r="21"/><path fill="none" stroke="#D9A62E" stroke-width="4" d="M26 24 C26 18 38 18 38 24 C38 30 26 33 26 40 C26 46 38 46 38 40"/>)"},
};

} // namespace

const std::vector<BuiltinIcon>& BuiltinIcons() {
    static const std::vector<BuiltinIcon> list = []() {
        std::vector<BuiltinIcon> out;
        for (const IconDef& d : kIcons) out.push_back({d.id, d.name, d.category, d.desc});
        return out;
    }();
    return list;
}

std::wstring BuiltinIconImage(const std::string& id) {
    static std::map<std::string, std::wstring> cache;
    auto hit = cache.find(id);
    if (hit != cache.end()) return hit->second;
    for (const IconDef& d : kIcons) {
        if (id != d.id) continue;
        const std::wstring dir = ItemsDir() + L"\\icons-v1";
        std::error_code ec;
        fs::create_directories(dir, ec);
        const std::wstring path = dir + L"\\" + std::wstring(id.begin(), id.end()) + L".png";
        if (!fs::exists(path, ec) && !RenderSvg(d.svg, path, d.vbw, d.vbh)) return {};
        cache[id] = path;
        return path;
    }
    return {};
}
