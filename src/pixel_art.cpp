// See pixel_art.h. Everything works in OKLab, where distances match what the eye sees, so palettes keep the colours that matter.
#include "pixel_art.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <random>

#include "image_texture.h"

namespace {

float SrgbToLinear(float c) {
    c /= 255.0f;
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float LinearToSrgb(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return (c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f) * 255.0f;
}

// a lookup table: the 256 possible channel values, linearised once
const float* LinearTable() {
    static float table[256];
    static bool ready = false;
    if (!ready) {
        for (int i = 0; i < 256; ++i) table[i] = SrgbToLinear(static_cast<float>(i));
        ready = true;
    }
    return table;
}

float Dist2(const PixelLab& x, const PixelLab& y) {
    const float dl = x.L - y.L, da = x.a - y.a, db = x.b - y.b;
    return dl * dl + da * da + db * db;
}

uint32_t Hex(const char* s) { return static_cast<uint32_t>(std::strtoul(s, nullptr, 16)); }

std::vector<uint32_t> Hexes(std::initializer_list<const char*> list) {
    std::vector<uint32_t> out;
    for (const char* s : list) out.push_back(Hex(s));
    return out;
}

// The dominant colour of a set of pixels: two clusters (started from the two most different pixels), the bigger one wins. A plain average would
// smear the soft edge of a cell into a colour that is in neither neighbour.
PixelLab Dominant(const std::vector<PixelLab>& px) {
    if (px.empty()) return {};
    if (px.size() == 1) return px[0];
    PixelLab mean;
    for (const PixelLab& p : px) {
        mean.L += p.L;
        mean.a += p.a;
        mean.b += p.b;
    }
    const float inv = 1.0f / static_cast<float>(px.size());
    mean.L *= inv;
    mean.a *= inv;
    mean.b *= inv;
    size_t ia = 0;
    float best = -1;
    for (size_t i = 0; i < px.size(); ++i) {
        const float d = Dist2(px[i], mean);
        if (d > best) {
            best = d;
            ia = i;
        }
    }
    size_t ib = 0;
    best = -1;
    for (size_t i = 0; i < px.size(); ++i) {
        const float d = Dist2(px[i], px[ia]);
        if (d > best) {
            best = d;
            ib = i;
        }
    }
    if (best < 1e-6f) return mean; // all the same colour
    PixelLab ca = px[ia], cb = px[ib];
    size_t na = 0, nb = 0;
    for (int it = 0; it < 5; ++it) {
        PixelLab sa, sb;
        na = nb = 0;
        for (const PixelLab& p : px) {
            if (Dist2(p, ca) <= Dist2(p, cb)) {
                sa.L += p.L; sa.a += p.a; sa.b += p.b;
                ++na;
            } else {
                sb.L += p.L; sb.a += p.a; sb.b += p.b;
                ++nb;
            }
        }
        if (na) ca = {sa.L / na, sa.a / na, sa.b / na};
        if (nb) cb = {sb.L / nb, sb.a / nb, sb.b / nb};
    }
    return na >= nb ? ca : cb;
}

// Edge energy between neighbouring columns (vertical = false) or rows: prof[i] = change between i and i + 1.
std::vector<double> EdgeProfile(const uint8_t* rgba, int w, int h, bool rows) {
    const int n = (rows ? h : w) - 1;
    std::vector<double> prof(std::max(0, n), 0.0);
    if (n <= 0) return prof;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int nx = rows ? x : x + 1, ny = rows ? y + 1 : y;
            if (nx >= w || ny >= h) continue;
            const uint8_t* p = rgba + (static_cast<size_t>(y) * w + x) * 4;
            const uint8_t* q = rgba + (static_cast<size_t>(ny) * w + nx) * 4;
            prof[rows ? y : x] += std::abs(p[0] - q[0]) + std::abs(p[1] - q[1]) + std::abs(p[2] - q[2]);
        }
    }
    return prof;
}

// The period and phase whose comb of positions lands on the most edges: a z-score, so many aligned edges beat a few lucky strong ones.
void BestComb(const std::vector<double>& prof, double& zBest, double& period, double& phase) {
    const int n = static_cast<int>(prof.size());
    double mu = 0, sd = 0;
    for (double v : prof) mu += v;
    mu /= std::max(1, n);
    for (double v : prof) sd += (v - mu) * (v - mu);
    sd = std::sqrt(sd / std::max(1, n)) + 1e-9;
    zBest = -1e9;
    period = 0;
    phase = 0;
    for (double p = 2.0; p <= std::min(48.0, n / 4.0); p += p < 12 ? 0.05 : 0.1) {
        const double step = p >= 4 ? 0.5 : 0.25;
        for (double o = 0; o < p; o += step) {
            double sum = 0;
            int k = 0;
            for (double pos = o; pos < n; pos += p) {
                const int i = static_cast<int>(std::lround(pos));
                if (i < 0 || i >= n) continue;
                sum += prof[i];
                ++k;
            }
            if (k < 4) continue;
            const double z = (sum / k - mu) / sd * std::sqrt(static_cast<double>(k));
            if (z > zBest) {
                zBest = z;
                period = p;
                phase = o;
            }
        }
    }
}

// The cell edges: the comb's positions, each moved onto a clear edge nearby (AI-drawn grids are not perfectly regular).
std::vector<float> Edges(const std::vector<double>& prof, double p, double o, int pixels) {
    const int n = static_cast<int>(prof.size());
    std::vector<double> nonzero;
    for (double v : prof) {
        if (v > 0) nonzero.push_back(v);
    }
    double strong = 0;
    if (!nonzero.empty()) {
        std::nth_element(nonzero.begin(), nonzero.begin() + nonzero.size() / 2, nonzero.end());
        strong = nonzero[nonzero.size() / 2] * 1.5;
    }
    const int r = std::max(1, static_cast<int>(std::lround(p * 0.3)));
    std::vector<float> xs{0.0f};
    for (int k = 0;; ++k) {
        const int idx = static_cast<int>(std::lround(o + k * p));
        if (idx >= n) break;
        if (idx < 0) continue;
        int j = idx;
        for (int t = std::max(0, idx - r); t <= std::min(n - 1, idx + r); ++t) {
            if (prof[t] > prof[j]) j = t;
        }
        const float edge = static_cast<float>((prof[j] > strong ? j : idx) + 1);
        if (edge - xs.back() >= p * 0.5 && pixels - edge >= p * 0.5) xs.push_back(edge);
    }
    xs.push_back(static_cast<float>(pixels));
    return xs;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------
PixelLab PixelRgbToLab(uint8_t r8, uint8_t g8, uint8_t b8) {
    const float* lin = LinearTable();
    const float r = lin[r8], g = lin[g8], b = lin[b8];
    const float l = std::cbrt(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
    const float m = std::cbrt(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
    const float s = std::cbrt(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
    return {0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s, 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
            0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s};
}

void PixelLabToRgb(const PixelLab& c, uint8_t out[3]) {
    const float l = c.L + 0.3963377774f * c.a + 0.2158037573f * c.b;
    const float m = c.L - 0.1055613458f * c.a - 0.0638541728f * c.b;
    const float s = c.L - 0.0894841775f * c.a - 1.2914855480f * c.b;
    const float l3 = l * l * l, m3 = m * m * m, s3 = s * s * s;
    const float rgb[3] = {4.0767416621f * l3 - 3.3077115913f * m3 + 0.2309699292f * s3, -1.2684380046f * l3 + 2.6097574011f * m3 - 0.3413193965f * s3,
                          -0.0041960863f * l3 - 0.7034186147f * m3 + 1.7076147010f * s3};
    for (int i = 0; i < 3; ++i) out[i] = static_cast<uint8_t>(std::lround(std::clamp(LinearToSrgb(rgb[i]), 0.0f, 255.0f)));
}

const std::vector<PixelPalettePreset>& PixelPalettePresets() {
    static const std::vector<PixelPalettePreset> presets = {
        {"Made from the picture", {}},
        {"PICO-8 (16)", Hexes({"000000", "1D2B53", "7E2553", "008751", "AB5236", "5F574F", "C2C3C7", "FFF1E8", "FF004D", "FFA300", "FFEC27", "00E436", "29ADFF", "83769C", "FF77A8", "FFCCAA"})},
        {"Sweetie 16", Hexes({"1A1C2C", "5D275D", "B13E53", "EF7D57", "FFCD75", "A7F070", "38B764", "257179", "29366F", "3B5DC9", "41A6F6", "73EFF7", "F4F4F4", "94B0C2", "566C86", "333C57"})},
        {"DawnBringer 16", Hexes({"140C1C", "442434", "30346D", "4E4A4E", "854C30", "346524", "D04648", "757161", "597DCE", "D27D2C", "8595A1", "6DAA2C", "D2AA99", "6DC2CA", "DAD45E", "DEEED6"})},
        {"DawnBringer 32", Hexes({"000000", "222034", "45283C", "663931", "8F563B", "DF7126", "D9A066", "EEC39A", "FBF236", "99E550", "6ABE30", "37946E", "4B692F", "524B24", "323C39", "3F3F74",
                                  "306082", "5B6EE1", "639BFF", "5FCDE4", "CBDBFC", "FFFFFF", "9BADB7", "847E87", "696A6A", "595652", "76428A", "AC3232", "D95763", "D77BBA", "8F974A", "8A6F30"})},
        {"Endesga 32", Hexes({"BE4A2F", "D77643", "EAD4AA", "E4A672", "B86F50", "733E39", "3E2731", "A22633", "E43B44", "F77622", "FEAE34", "FEE761", "63C74D", "3E8948", "265C42", "193C3E",
                              "124E89", "0099DB", "2CE8F5", "FFFFFF", "C0CBDC", "8B9BB4", "5A6988", "3A4466", "262B44", "181425", "FF0044", "68386C", "B55088", "F6757A", "E8B796", "C28569"})},
        {"Game Boy (4 greens)", Hexes({"0F380F", "306230", "8BAC0F", "9BBC0F"})},
        {"Game Boy Pocket (4 greys)", Hexes({"000000", "555555", "AAAAAA", "FFFFFF"})},
        {"1-bit (black and white)", Hexes({"000000", "FFFFFF"})},
    };
    return presets;
}

PixelCells SampleCells(const uint8_t* rgba, int width, int height, int cols, int rows) {
    std::vector<float> xs(cols + 1), ys(rows + 1);
    for (int i = 0; i <= cols; ++i) xs[i] = static_cast<float>(width) * i / cols;
    for (int i = 0; i <= rows; ++i) ys[i] = static_cast<float>(height) * i / rows;
    return SampleCellsOnGrid(rgba, width, height, xs, ys);
}

PixelCells SampleCellsOnGrid(const uint8_t* rgba, int width, int height, const std::vector<float>& xs, const std::vector<float>& ys) {
    PixelCells c;
    if (xs.size() < 2 || ys.size() < 2 || width <= 0 || height <= 0) return c;
    c.w = static_cast<int>(xs.size()) - 1;
    c.h = static_cast<int>(ys.size()) - 1;
    c.lab.resize(static_cast<size_t>(c.w) * c.h);
    c.opaque.assign(static_cast<size_t>(c.w) * c.h, 1);
    std::vector<PixelLab> px;
    for (int cy = 0; cy < c.h; ++cy) {
        for (int cx = 0; cx < c.w; ++cx) {
            // the middle half of the cell (at least one pixel): its edges are blended with the neighbours
            const float x0 = xs[cx], x1 = xs[cx + 1], y0 = ys[cy], y1 = ys[cy + 1];
            const float mx = (x1 - x0) * 0.25f, my = (y1 - y0) * 0.25f;
            int ax = static_cast<int>(std::floor(x0 + mx)), bx = static_cast<int>(std::ceil(x1 - mx));
            int ay = static_cast<int>(std::floor(y0 + my)), by = static_cast<int>(std::ceil(y1 - my));
            ax = std::clamp(ax, 0, width - 1);
            ay = std::clamp(ay, 0, height - 1);
            bx = std::clamp(std::max(bx, ax + 1), 1, width);
            by = std::clamp(std::max(by, ay + 1), 1, height);
            px.clear();
            int see = 0, all = 0;
            for (int y = ay; y < by; ++y) {
                for (int x = ax; x < bx; ++x) {
                    const uint8_t* p = rgba + (static_cast<size_t>(y) * width + x) * 4;
                    ++all;
                    if (p[3] < 128) {
                        ++see;
                        continue;
                    }
                    px.push_back(PixelRgbToLab(p[0], p[1], p[2]));
                }
            }
            const size_t i = static_cast<size_t>(cy) * c.w + cx;
            if (see * 2 > all || px.empty()) {
                c.opaque[i] = 0;
                continue;
            }
            c.lab[i] = Dominant(px);
        }
    }
    return c;
}

bool DetectPixelGrid(const uint8_t* rgba, int width, int height, std::vector<float>& xs, std::vector<float>& ys, float* cellSize) {
    if (width < 16 || height < 16) return false;
    const std::vector<double> px = EdgeProfile(rgba, width, height, false), py = EdgeProfile(rgba, width, height, true);
    double zx = 0, pX = 0, oX = 0, zy = 0, pY = 0, oY = 0;
    BestComb(px, zx, pX, oX);
    BestComb(py, zy, pY, oY);
    // a real grid lines up many edges (z well above 5 on AI pixel art and upscaled sprites, about 3 on photos)
    if (zx < 5.0 || zy < 5.0 || pX < 2.0 || pY < 2.0) return false;
    xs = Edges(px, pX, oX, width);
    ys = Edges(py, pY, oY, height);
    if (cellSize) *cellSize = static_cast<float>((pX + pY) * 0.5);
    return xs.size() >= 3 && ys.size() >= 3;
}

std::vector<PixelLab> MakePixelPalette(const std::vector<const PixelCells*>& cells, const PixelOptions& options) {
    const auto& presets = PixelPalettePresets();
    if (options.palette > 0 && options.palette < static_cast<int>(presets.size())) {
        std::vector<PixelLab> out;
        for (uint32_t c : presets[options.palette].colors) out.push_back(PixelRgbToLab((c >> 16) & 255, (c >> 8) & 255, c & 255));
        return out;
    }
    std::vector<PixelLab> x;
    for (const PixelCells* c : cells) {
        if (!c) continue;
        for (size_t i = 0; i < c->lab.size(); ++i) {
            if (c->opaque[i]) x.push_back(c->lab[i]);
        }
    }
    const int k = std::clamp(options.colors, 2, 64);
    if (x.empty()) return {};
    // few distinct colours: they are the palette
    std::vector<PixelLab> uniq;
    for (const PixelLab& p : x) {
        bool seen = false;
        for (const PixelLab& u : uniq) {
            if (Dist2(p, u) < 1e-6f) {
                seen = true;
                break;
            }
        }
        if (!seen) uniq.push_back(p);
        if (static_cast<int>(uniq.size()) > k) break;
    }
    if (static_cast<int>(uniq.size()) <= k) return uniq;
    // k-means++ start (fixed seed: the same picture always gives the same palette), then refine
    std::mt19937 rng(1);
    std::vector<PixelLab> c;
    c.push_back(x[rng() % x.size()]);
    std::vector<float> d(x.size());
    for (size_t i = 0; i < x.size(); ++i) d[i] = Dist2(x[i], c[0]);
    while (static_cast<int>(c.size()) < k) {
        double sum = 0;
        for (float v : d) sum += v;
        if (sum <= 0) break;
        double r = std::uniform_real_distribution<double>(0.0, sum)(rng);
        size_t pick = 0;
        for (; pick + 1 < x.size(); ++pick) {
            r -= d[pick];
            if (r <= 0) break;
        }
        c.push_back(x[pick]);
        for (size_t i = 0; i < x.size(); ++i) d[i] = std::min(d[i], Dist2(x[i], c.back()));
    }
    std::vector<int> label(x.size(), 0);
    for (int it = 0; it < 24; ++it) {
        bool moved = false;
        for (size_t i = 0; i < x.size(); ++i) {
            int best = 0;
            float bd = Dist2(x[i], c[0]);
            for (int j = 1; j < static_cast<int>(c.size()); ++j) {
                const float dd = Dist2(x[i], c[j]);
                if (dd < bd) {
                    bd = dd;
                    best = j;
                }
            }
            if (label[i] != best) moved = true;
            label[i] = best;
        }
        std::vector<PixelLab> sum(c.size());
        std::vector<int> n(c.size(), 0);
        for (size_t i = 0; i < x.size(); ++i) {
            sum[label[i]].L += x[i].L;
            sum[label[i]].a += x[i].a;
            sum[label[i]].b += x[i].b;
            ++n[label[i]];
        }
        for (size_t j = 0; j < c.size(); ++j) {
            if (n[j]) c[j] = {sum[j].L / n[j], sum[j].a / n[j], sum[j].b / n[j]};
        }
        if (!moved && it > 0) break;
    }
    return c;
}

void RenderPixelArt(const PixelCells& cells, const std::vector<PixelLab>& palette, const PixelOptions& options, std::vector<uint8_t>& rgba) {
    const int w = cells.w, h = cells.h;
    rgba.assign(static_cast<size_t>(w) * h * 4, 0);
    if (w <= 0 || h <= 0 || palette.empty()) return;
    std::vector<int> idx(static_cast<size_t>(w) * h, -1);
    for (size_t i = 0; i < idx.size(); ++i) {
        if (!cells.opaque[i]) continue;
        int best = 0;
        float bd = Dist2(cells.lab[i], palette[0]);
        for (int j = 1; j < static_cast<int>(palette.size()); ++j) {
            const float d = Dist2(cells.lab[i], palette[j]);
            if (d < bd) {
                bd = d;
                best = j;
            }
        }
        idx[i] = best;
    }
    auto at = [&](int x, int y) -> int& { return idx[static_cast<size_t>(y) * w + x]; };
    // a lone pixel whose four neighbours all agree, and that is close to them in colour, is noise: it takes their colour (eyes and highlights stay: they contrast)
    if (options.clean) {
        std::vector<int> out = idx;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const int me = at(x, y);
                if (me < 0) continue;
                int first = -2, count = 0;
                bool same = true;
                const int dx[4] = {-1, 1, 0, 0}, dy[4] = {0, 0, -1, 1};
                for (int k = 0; k < 4; ++k) {
                    const int nx = x + dx[k], ny = y + dy[k];
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    const int v = at(nx, ny);
                    if (first == -2) first = v;
                    else if (v != first) same = false;
                    ++count;
                }
                if (count >= 3 && same && first >= 0 && first != me && Dist2(palette[first], palette[me]) < 0.12f * 0.12f) out[static_cast<size_t>(y) * w + x] = first;
            }
        }
        idx.swap(out);
    }
    // the background: the colour that owns most of the border (and colours very close to it), flood-filled from the border
    if (options.transparent) {
        std::vector<int> count(palette.size(), 0);
        int border = 0;
        for (int x = 0; x < w; ++x) {
            for (int y : {0, h - 1}) {
                if (at(x, y) >= 0) ++count[at(x, y)];
                ++border;
            }
        }
        for (int y = 1; y < h - 1; ++y) {
            for (int x : {0, w - 1}) {
                if (at(x, y) >= 0) ++count[at(x, y)];
                ++border;
            }
        }
        const int bg = static_cast<int>(std::max_element(count.begin(), count.end()) - count.begin());
        if (count[bg] * 2 >= border) {
            std::vector<uint8_t> fill(idx.size(), 0);
            std::vector<std::pair<int, int>> stack;
            auto like = [&](int v) { return v >= 0 && Dist2(palette[v], palette[bg]) < 0.10f * 0.10f; };
            for (int x = 0; x < w; ++x) {
                stack.push_back({x, 0});
                stack.push_back({x, h - 1});
            }
            for (int y = 0; y < h; ++y) {
                stack.push_back({0, y});
                stack.push_back({w - 1, y});
            }
            while (!stack.empty()) {
                const auto [x, y] = stack.back();
                stack.pop_back();
                if (x < 0 || y < 0 || x >= w || y >= h) continue;
                const size_t i = static_cast<size_t>(y) * w + x;
                if (fill[i] || !like(idx[i])) continue;
                fill[i] = 1;
                stack.push_back({x + 1, y});
                stack.push_back({x - 1, y});
                stack.push_back({x, y + 1});
                stack.push_back({x, y - 1});
            }
            // the fringe: pixels on the edge of the cut-out that are still close to the background colour (the soft edge of a drawing on white)
            std::vector<uint8_t> grow = fill;
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const size_t i = static_cast<size_t>(y) * w + x;
                    if (fill[i] || idx[i] < 0 || Dist2(palette[idx[i]], palette[bg]) >= 0.20f * 0.20f) continue;
                    if ((x > 0 && fill[i - 1]) || (x + 1 < w && fill[i + 1]) || (y > 0 && fill[i - w]) || (y + 1 < h && fill[i + w])) grow[i] = 1;
                }
            }
            // background caught inside the shape (between an arm and the body, inside a ring): pockets of the background colour of 4+ pixels go too
            // (eyes and highlights are smaller)
            std::vector<uint8_t> seen(idx.size(), 0);
            for (size_t s = 0; s < idx.size(); ++s) {
                if (grow[s] || seen[s] || idx[s] < 0 || Dist2(palette[idx[s]], palette[bg]) >= 0.06f * 0.06f) continue;
                std::vector<size_t> comp{s}, todo{s};
                seen[s] = 1;
                while (!todo.empty()) {
                    const size_t i = todo.back();
                    todo.pop_back();
                    const int x = static_cast<int>(i % w), y = static_cast<int>(i / w);
                    const int nx[4] = {x - 1, x + 1, x, x}, ny[4] = {y, y, y - 1, y + 1};
                    for (int k = 0; k < 4; ++k) {
                        if (nx[k] < 0 || ny[k] < 0 || nx[k] >= w || ny[k] >= h) continue;
                        const size_t j = static_cast<size_t>(ny[k]) * w + nx[k];
                        if (seen[j] || grow[j] || idx[j] < 0 || Dist2(palette[idx[j]], palette[bg]) >= 0.06f * 0.06f) continue;
                        seen[j] = 1;
                        comp.push_back(j);
                        todo.push_back(j);
                    }
                }
                if (comp.size() >= 4) {
                    for (size_t i : comp) grow[i] = 1;
                }
            }
            for (size_t i = 0; i < idx.size(); ++i) {
                if (grow[i]) idx[i] = -1;
            }
        }
        if (options.outline) { // the darkest colour, one pixel wide, around everything that is left
            int dark = 0;
            for (int j = 1; j < static_cast<int>(palette.size()); ++j) {
                if (palette[j].L < palette[dark].L) dark = j;
            }
            std::vector<int> out = idx;
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    if (at(x, y) >= 0) continue;
                    bool touches = false;
                    if (x > 0 && at(x - 1, y) >= 0) touches = true;
                    if (x + 1 < w && at(x + 1, y) >= 0) touches = true;
                    if (y > 0 && at(x, y - 1) >= 0) touches = true;
                    if (y + 1 < h && at(x, y + 1) >= 0) touches = true;
                    if (touches) out[static_cast<size_t>(y) * w + x] = dark;
                }
            }
            idx.swap(out);
        }
    }
    std::vector<std::array<uint8_t, 3>> rgb(palette.size());
    for (size_t j = 0; j < palette.size(); ++j) PixelLabToRgb(palette[j], rgb[j].data());
    for (size_t i = 0; i < idx.size(); ++i) {
        if (idx[i] < 0) continue;
        uint8_t* o = rgba.data() + i * 4;
        o[0] = rgb[idx[i]][0];
        o[1] = rgb[idx[i]][1];
        o[2] = rgb[idx[i]][2];
        o[3] = 255;
    }
}

std::vector<uint8_t> PixelSubjectCrop(const uint8_t* rgba, int width, int height, double aspect, int& outW, int& outH) {
    outW = width;
    outH = height;
    std::vector<uint8_t> copy(rgba, rgba + static_cast<size_t>(width) * height * 4);
    if (width < 8 || height < 8 || aspect <= 0) return copy;
    // the background: the median colour of the border
    std::vector<int> ch[3];
    auto take = [&](int x, int y) {
        const uint8_t* q = rgba + (static_cast<size_t>(y) * width + x) * 4;
        for (int c = 0; c < 3; ++c) ch[c].push_back(q[c]);
    };
    for (int x = 0; x < width; ++x) {
        take(x, 0);
        take(x, height - 1);
    }
    for (int y = 1; y < height - 1; ++y) {
        take(0, y);
        take(width - 1, y);
    }
    int bg[3];
    for (int c = 0; c < 3; ++c) {
        std::nth_element(ch[c].begin(), ch[c].begin() + ch[c].size() / 2, ch[c].end());
        bg[c] = ch[c][ch[c].size() / 2];
    }
    int x0 = width, y0 = height, x1 = -1, y1 = -1;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const uint8_t* q = rgba + (static_cast<size_t>(y) * width + x) * 4;
            if (q[3] < 128) continue;
            if (std::abs(q[0] - bg[0]) + std::abs(q[1] - bg[1]) + std::abs(q[2] - bg[2]) <= 40) continue;
            x0 = std::min(x0, x);
            y0 = std::min(y0, y);
            x1 = std::max(x1, x);
            y1 = std::max(y1, y);
        }
    }
    if (x1 < x0 || y1 < y0) return copy;
    const double cw = x1 - x0 + 1.0, chh = y1 - y0 + 1.0;
    const double sideW = std::max(cw, chh * aspect) * 1.12, sideH = sideW / aspect;
    const double cx = (x0 + x1 + 1) * 0.5, cy = (y0 + y1 + 1) * 0.5;
    const int a = static_cast<int>(std::lround(cx - sideW * 0.5)), b = static_cast<int>(std::lround(cy - sideH * 0.5));
    const int w = std::max(8, static_cast<int>(std::lround(sideW))), h = std::max(8, static_cast<int>(std::lround(sideH)));
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < out.size(); i += 4) {
        out[i] = static_cast<uint8_t>(bg[0]);
        out[i + 1] = static_cast<uint8_t>(bg[1]);
        out[i + 2] = static_cast<uint8_t>(bg[2]);
        out[i + 3] = 255;
    }
    for (int y = 0; y < h; ++y) {
        const int sy = b + y;
        if (sy < 0 || sy >= height) continue;
        for (int x = 0; x < w; ++x) {
            const int sx = a + x;
            if (sx < 0 || sx >= width) continue;
            std::memcpy(out.data() + (static_cast<size_t>(y) * w + x) * 4, rgba + (static_cast<size_t>(sy) * width + sx) * 4, 4);
        }
    }
    outW = w;
    outH = h;
    return out;
}

std::vector<uint8_t> PixelBoxDownscale(const uint8_t* rgba, int width, int height, int outW, int outH) {
    std::vector<uint8_t> out(static_cast<size_t>(outW) * outH * 4, 0);
    for (int y = 0; y < outH; ++y) {
        const int y0 = static_cast<int>(static_cast<long long>(y) * height / outH);
        const int y1 = std::max(y0 + 1, static_cast<int>(static_cast<long long>(y + 1) * height / outH));
        for (int x = 0; x < outW; ++x) {
            const int x0 = static_cast<int>(static_cast<long long>(x) * width / outW);
            const int x1 = std::max(x0 + 1, static_cast<int>(static_cast<long long>(x + 1) * width / outW));
            double sum[4] = {0, 0, 0, 0};
            double wsum = 0;
            for (int sy = y0; sy < y1; ++sy) {
                for (int sx = x0; sx < x1; ++sx) {
                    const uint8_t* p = rgba + (static_cast<size_t>(sy) * width + sx) * 4;
                    const double a = p[3] / 255.0; // colours weighted by alpha, so transparent pixels do not darken the edges
                    sum[0] += p[0] * a;
                    sum[1] += p[1] * a;
                    sum[2] += p[2] * a;
                    sum[3] += p[3];
                    wsum += a;
                }
            }
            const double n = static_cast<double>(y1 - y0) * (x1 - x0);
            uint8_t* o = out.data() + (static_cast<size_t>(y) * outW + x) * 4;
            for (int c = 0; c < 3; ++c) o[c] = static_cast<uint8_t>(wsum > 0 ? std::lround(sum[c] / wsum) : 0);
            o[3] = static_cast<uint8_t>(std::lround(sum[3] / n));
        }
    }
    return out;
}

std::vector<uint8_t> PixelNearestUpscale(const uint8_t* rgba, int width, int height, int factor) {
    factor = std::max(1, factor);
    const int ow = width * factor, oh = height * factor;
    std::vector<uint8_t> out(static_cast<size_t>(ow) * oh * 4);
    for (int y = 0; y < oh; ++y) {
        const uint8_t* srow = rgba + static_cast<size_t>(y / factor) * width * 4;
        uint8_t* drow = out.data() + static_cast<size_t>(y) * ow * 4;
        for (int x = 0; x < ow; ++x) std::memcpy(drow + x * 4, srow + (x / factor) * 4, 4);
    }
    return out;
}

std::vector<uint8_t> PixelMedian3(const uint8_t* rgba, int width, int height) {
    std::vector<uint8_t> out(rgba, rgba + static_cast<size_t>(width) * height * 4);
    uint8_t v[9];
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            for (int c = 0; c < 3; ++c) {
                int n = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    const int yy = std::clamp(y + dy, 0, height - 1);
                    for (int dx = -1; dx <= 1; ++dx) v[n++] = rgba[(static_cast<size_t>(yy) * width + std::clamp(x + dx, 0, width - 1)) * 4 + c];
                }
                std::nth_element(v, v + 4, v + 9);
                out[(static_cast<size_t>(y) * width + x) * 4 + c] = v[4];
            }
        }
    }
    return out;
}

std::vector<uint8_t> PixelRoll(const uint8_t* rgba, int width, int height, int dx, int dy) {
    std::vector<uint8_t> out(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        const int sy = ((y - dy) % height + height) % height;
        for (int x = 0; x < width; ++x) {
            const int sx = ((x - dx) % width + width) % width;
            std::memcpy(out.data() + (static_cast<size_t>(y) * width + x) * 4, rgba + (static_cast<size_t>(sy) * width + sx) * 4, 4);
        }
    }
    return out;
}

bool PixelLoadRGBA(const std::wstring& path, std::vector<uint8_t>& rgba, int& width, int& height, std::string* error) {
    return DecodeImageRGBA(path, rgba, width, height, error);
}

bool PixelSavePng(const std::wstring& path, const uint8_t* rgba, int width, int height) { return WriteThumbnailPng(path, rgba, width, height); }

// ------------------------------------------------------------------- sprite sheets
static void SheetBorderMedian(const uint8_t* rgba, int w, int h, int med[3]) {
    std::vector<int> ch3[3];
    auto take = [&](int x, int y) {
        const uint8_t* q = rgba + (static_cast<size_t>(y) * w + x) * 4;
        for (int c = 0; c < 3; ++c) ch3[c].push_back(q[c]);
    };
    for (int x = 0; x < w; x += 2) {
        take(x, 0);
        take(x, h - 1);
    }
    for (int y = 0; y < h; y += 2) {
        take(0, y);
        take(w - 1, y);
    }
    for (int c = 0; c < 3; ++c) {
        std::vector<int>& v = ch3[c];
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        med[c] = v.empty() ? 255 : v[v.size() / 2];
    }
}

bool PixelSliceSheet(const uint8_t* rgba, int W, int H, int expected, int refFrame, double artTall, PixelSheet& out) {
    out = PixelSheet{};
    if (!rgba || W < 16 || H < 16) return false;
    int bg[3];
    SheetBorderMedian(rgba, W, H, bg);
    std::vector<uint8_t> mask(static_cast<size_t>(W) * H, 0);
    std::vector<int> colCount(W, 0);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const uint8_t* q = rgba + (static_cast<size_t>(y) * W + x) * 4;
            if (std::abs(q[0] - bg[0]) + std::abs(q[1] - bg[1]) + std::abs(q[2] - bg[2]) > 48) {
                mask[static_cast<size_t>(y) * W + x] = 1;
                ++colCount[x];
            }
        }
    }
    // runs of drawn columns; gaps of a few pixels belong to the same frame; specks are dropped
    std::vector<std::pair<int, int>> spans;
    for (int x = 0; x < W;) {
        if (colCount[x] < 2) {
            ++x;
            continue;
        }
        const int s = x;
        while (x < W && colCount[x] >= 2) ++x;
        if (!spans.empty() && s - spans.back().second < std::max(6, W / 400)) spans.back().second = x;
        else spans.push_back({s, x});
    }
    int widest = 0;
    for (const auto& s : spans) widest = std::max(widest, s.second - s.first);
    spans.erase(std::remove_if(spans.begin(), spans.end(), [&](const std::pair<int, int>& s) { return s.second - s.first < std::max(W / 50, widest / 4); }), spans.end());
    const int n = std::max(1, expected);
    while (static_cast<int>(spans.size()) > n) { // more pieces than frames: join the two closest
        size_t best = 0;
        for (size_t i = 1; i + 1 < spans.size(); ++i) {
            if (spans[i + 1].first - spans[i].second < spans[best + 1].first - spans[best].second) best = i;
        }
        spans[best].second = spans[best + 1].second;
        spans.erase(spans.begin() + best + 1);
    }
    if (static_cast<int>(spans.size()) < n) { // frames touching each other: even slices
        spans.clear();
        for (int i = 0; i < n; ++i) spans.push_back({W * i / n, W * (i + 1) / n});
    }
    struct Fr {
        int x0, x1, y0, y1;
        double cx;
    };
    std::vector<Fr> frs;
    for (const auto& s : spans) {
        Fr f{W, -1, H, -1, 0.0};
        double sum = 0, cnt = 0;
        for (int y = 0; y < H; ++y) {
            for (int x = s.first; x < s.second; ++x) {
                if (!mask[static_cast<size_t>(y) * W + x]) continue;
                f.x0 = std::min(f.x0, x);
                f.x1 = std::max(f.x1, x + 1);
                f.y0 = std::min(f.y0, y);
                f.y1 = std::max(f.y1, y + 1);
                sum += x;
                cnt += 1;
            }
        }
        if (cnt < 16) continue;
        f.cx = sum / cnt; // the middle of the body (steadier than the middle of the box, which moves with every swinging arm and leg)
        frs.push_back(f);
    }
    if (frs.empty()) return false;
    int top = H, bottom = 0, half = 1;
    std::vector<int> heights;
    for (const Fr& f : frs) {
        top = std::min(top, f.y0);
        bottom = std::max(bottom, f.y1);
        half = std::max(half, static_cast<int>(std::ceil(std::max(f.cx - f.x0, f.x1 - f.cx))));
        heights.push_back(f.y1 - f.y0);
    }
    // the frame showing the figure at its plain height (standing), else the tallest
    const int refH = refFrame >= 0 && refFrame < static_cast<int>(heights.size()) ? heights[refFrame] : *std::max_element(heights.begin(), heights.end());
    const double typical = std::max(8, refH);
    const int marginB = std::max(2, static_cast<int>((bottom - top) * 0.03)), marginT = std::max(2, static_cast<int>((bottom - top) * 0.05));
    const int cellPxW = half * 2 + static_cast<int>(half * 0.12) + 4, cellPxH = bottom - top + marginB + marginT;
    double k = artTall / typical; // art pixels per sheet pixel
    k = std::min(k, 256.0 / std::max(cellPxW, cellPxH));
    const int aw = std::clamp(static_cast<int>(std::lround(cellPxW * k)), 8, 256), ah = std::clamp(static_cast<int>(std::lround(cellPxH * k)), 8, 256);
    const int CW = std::max(aw, static_cast<int>(std::lround(aw / k))), CH = std::max(ah, static_cast<int>(std::lround(ah / k)));
    const int nf = static_cast<int>(frs.size());
    std::vector<uint8_t>& sheet = out.rgba;
    sheet.assign(static_cast<size_t>(nf) * CW * CH * 4, 0);
    for (size_t i = 0; i < sheet.size(); i += 4) {
        sheet[i] = static_cast<uint8_t>(bg[0]);
        sheet[i + 1] = static_cast<uint8_t>(bg[1]);
        sheet[i + 2] = static_cast<uint8_t>(bg[2]);
        sheet[i + 3] = 255;
    }
    const double sx = static_cast<double>(CW) / cellPxW, sy = static_cast<double>(CH) / cellPxH; // (CW, CH) are (cellPxW, cellPxH) rounded to whole art pixels
    for (int i = 0; i < nf; ++i) {
        const Fr& f = frs[i];
        // a frame's pixel (x, y) of the sheet -> its place on the frame (the body's middle in the middle, the shared ground line at the bottom)
        for (int y = 0; y < CH; ++y) {
            const int srcY = static_cast<int>(std::floor(y / sy)) - marginT + top;
            if (srcY < f.y0 || srcY >= f.y1) continue;
            for (int x = 0; x < CW; ++x) {
                const int srcX = static_cast<int>(std::floor(x / sx - cellPxW * 0.5 + f.cx));
                if (srcX < f.x0 || srcX >= f.x1) continue;
                std::memcpy(sheet.data() + (static_cast<size_t>(y) * nf * CW + static_cast<size_t>(i) * CW + x) * 4, rgba + (static_cast<size_t>(srcY) * W + srcX) * 4, 3);
            }
        }
    }
    out.frames = nf;
    out.cellW = CW;
    out.cellH = CH;
    out.artW = aw;
    out.artH = ah;
    return true;
}
