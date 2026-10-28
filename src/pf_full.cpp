// Pixel Art Fixer port: the full-mode evidence (fusionchan.rs, varcontrast.rs, reconsearch.rs), the consensus core (core.rs), the two-stage reconstruction
// (reconstruct.rs::two_stage_pack) and the public API.
#include <future>

#include "pf_internal.h"
#include "pixel_fixer.h"

namespace pf {
namespace {

// ================================================================ fusionchan.rs
std::vector<double> ladder() {
    std::vector<double> steps;
    for (double s = 2.0; s <= 64.0 + 1e-9; s *= 1.03) steps.push_back(rte(s * 1e4) / 1e4);
    return steps;
}

std::vector<float> flatten_channels(const uint8_t* rgba, size_t w, size_t h) {
    std::vector<float> img(w * h * 4);
    for (size_t i = 0; i < w * h; ++i) {
        const size_t p = i * 4;
        const float a = rgba[p + 3] / 255.0f;
        img[p] = rgba[p] * a;
        img[p + 1] = rgba[p + 1] * a;
        img[p + 2] = rgba[p + 2] * a;
        img[p + 3] = rgba[p + 3];
    }
    return img;
}

void e1_profiles(const std::vector<float>& img, size_t w, size_t h, std::vector<double>& e1x, std::vector<double>& e1y) {
    e1x.assign(w + 1, 0.0);
    e1y.assign(h + 1, 0.0);
    std::vector<double> accx(w > 0 ? w - 1 : 0, 0.0);
    for (size_t y = 0; y < h; ++y) {
        const size_t r = y * w * 4;
        for (size_t x = 0; x + 1 < w; ++x) {
            float s = 0;
            for (int c = 0; c < 4; ++c) {
                const float d = img[r + (x + 1) * 4 + c] - img[r + x * 4 + c];
                s += d * d;
            }
            accx[x] += std::sqrt(s);
        }
    }
    for (size_t x = 0; x + 1 < w; ++x) e1x[x + 1] = f32r(accx[x]);
    for (size_t y = 0; y + 1 < h; ++y) {
        const size_t r0 = y * w * 4, r1 = (y + 1) * w * 4;
        double acc = 0;
        for (size_t x = 0; x < w; ++x) {
            float s = 0;
            for (int c = 0; c < 4; ++c) {
                const float d = img[r1 + x * 4 + c] - img[r0 + x * 4 + c];
                s += d * d;
            }
            acc += std::sqrt(s);
        }
        e1y[y + 1] = f32r(acc);
    }
    double mx = 0, my = 0;
    for (size_t i = 1; i < w; ++i) mx = std::max(mx, e1x[i]);
    for (size_t i = 1; i < h; ++i) my = std::max(my, e1y[i]);
    const double top = std::max(std::max(mx, my), 1.0);
    e1x[0] = e1x[w] = e1y[0] = e1y[h] = top;
}

double jpeg_lattice_strength(const std::vector<double>& profile) {
    const std::vector<double> norm = gaussian_filter1d(normalise(profile), 0.6);
    const size_t n = norm.size() - 1;
    std::vector<size_t> on, off;
    for (size_t i = 8; i + 7 < n; i += 8) on.push_back(i);
    for (size_t i = 4; i + 3 < n; i += 8) off.push_back(i);
    if (on.size() < 4 || off.size() < 4) return 0.0;
    double bm = 0;
    for (size_t i = 1; i < n; ++i) bm += norm[i];
    bm /= (double)(n - 1);
    double var = 0;
    for (size_t i = 1; i < n; ++i) var += (norm[i] - bm) * (norm[i] - bm);
    var /= (double)(n - 1);
    const double bs = std::sqrt(var) + 1e-9;
    double son = 0, soff = 0;
    for (size_t i : on) son += norm[i];
    for (size_t i : off) soff += norm[i];
    const double z_on = (son / (double)on.size() - bm) / (bs / std::sqrt((double)on.size()));
    const double z_off = (soff / (double)off.size() - bm) / (bs / std::sqrt((double)off.size()));
    return z_on - std::max(z_off, 0.0);
}

std::vector<double> notch_jpeg(const std::vector<double>& profile) {
    const size_t n = profile.size() - 1;
    std::vector<char> mask(n + 1, 0);
    bool any = false, all = true;
    for (size_t p = 0; p <= n; ++p) {
        if (p != 0 && p != n) mask[p] = (double)std::min(p % 8, 8 - (p % 8)) <= 1.0;
        any = any || mask[p];
        all = all && mask[p];
    }
    if (!any || all) return profile;
    std::vector<double> gx, gy;
    for (size_t i = 0; i <= n; ++i) {
        if (!mask[i]) {
            gx.push_back((double)i);
            gy.push_back(profile[i]);
        }
    }
    std::vector<double> out(n + 1);
    for (size_t i = 0; i <= n; ++i) out[i] = mask[i] ? interp((double)i, gx, gy) : profile[i];
    return out;
}

double rayleigh_score(const std::vector<double>& profile_norm, double step) {
    const size_t n = profile_norm.size() - 1;
    if (step < 2.0 || step > (double)n / 4.0) return 0.0;
    const double dist = (double)std::max(to_i64(step * 0.4), 1LL);
    std::vector<size_t> pk;
    std::vector<double> hts;
    find_peaks(std::vector<double>(profile_norm.begin() + 1, profile_norm.begin() + n), 0.15, dist, pk, hts);
    if (pk.size() < 5) return 0.0;
    double re = 0, im = 0, hsum = 0, h2sum = 0;
    std::vector<double> p(pk.size());
    for (size_t i = 0; i < pk.size(); ++i) {
        p[i] = (double)(pk[i] + 1);
        const double ang = 2 * kPi * p[i] / step;
        re += hts[i] * std::cos(ang);
        im += hts[i] * std::sin(ang);
        hsum += hts[i];
        h2sum += hts[i] * hts[i];
    }
    const double big_r = std::sqrt(re * re + im * im) / hsum, n_eff = hsum * hsum / h2sum;
    const double phase = rem_euclid(std::atan2(im, re) / (2 * kPi) * step, step);
    std::vector<long long> slots;
    for (double pos : p) {
        const long long slot = (long long)rte((pos - phase) / step);
        if (std::fabs(pos - (phase + (double)slot * step)) < 0.35 * step) slots.push_back(slot);
    }
    std::sort(slots.begin(), slots.end());
    slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
    const long long n_slots = std::max(1LL, (long long)((double)n / step) - 1);
    const double occupancy = std::min((double)slots.size() / (double)n_slots, 1.0);
    return std::sqrt(2.0 * n_eff) * big_r * occupancy;
}

struct Tile {
    std::vector<double> pos, heights;
    size_t extent = 0;
};

std::vector<Tile> tile_peaks(const std::vector<float>& dmap, size_t rows, size_t cols, size_t offset) {
    const size_t tw = std::clamp<size_t>(cols / 6, 48, 192), th = std::clamp<size_t>(rows / 6, 48, 192);
    std::vector<Tile> tiles;
    const size_t ymax = (size_t)std::max((long long)rows - (long long)th / 2, 1LL);
    const size_t xmax = (size_t)std::max((long long)cols - (long long)tw / 2, 1LL);
    for (size_t y0 = 0; y0 < ymax; y0 += th) {
        for (size_t x0 = 0; x0 < xmax; x0 += tw) {
            const size_t y1 = std::min(y0 + th, rows), x1 = std::min(x0 + tw, cols);
            if (y1 <= y0 || x1 <= x0) continue;
            std::vector<double> prof(x1 - x0, 0.0);
            for (size_t y = y0; y < y1; ++y) {
                for (size_t x = x0; x < x1; ++x) prof[x - x0] += dmap[y * cols + x];
            }
            for (double& v : prof) v = f32r(v);
            const double scale = percentile(prof, 95.0);
            if (scale <= 0.0) continue;
            std::vector<double> norm(prof.size());
            for (size_t i = 0; i < prof.size(); ++i) norm[i] = std::clamp(prof[i] / scale, 0.0, 1.5);
            std::vector<size_t> pk;
            std::vector<double> hts;
            find_peaks(norm, 0.2, 2.0, pk, hts);
            if (pk.size() >= 4) {
                Tile t;
                for (size_t q : pk) t.pos.push_back((double)(q + x0 + offset));
                t.heights = hts;
                t.extent = prof.size();
                tiles.push_back(std::move(t));
            }
        }
    }
    if (tiles.size() > 360) {
        std::vector<Tile> sel;
        for (size_t i = 0; i < 360; ++i) sel.push_back(tiles[(size_t)((double)i * ((double)tiles.size() - 1.0) / 359.0)]);
        tiles.swap(sel);
    }
    return tiles;
}

double tiles_ray_z(const std::vector<Tile>& tiles, double step) {
    if (tiles.empty() || step < 2.0) return 0.0;
    std::vector<double> zs;
    for (const Tile& t : tiles) {
        if (step > (double)t.extent / 3.0 || t.pos.size() < 4) continue;
        double re = 0, im = 0, hsum = 0, h2sum = 0;
        for (size_t i = 0; i < t.pos.size(); ++i) {
            const double ang = 2 * kPi * t.pos[i] / step;
            re += t.heights[i] * std::cos(ang);
            im += t.heights[i] * std::sin(ang);
            hsum += t.heights[i];
            h2sum += t.heights[i] * t.heights[i];
        }
        const double big_r = std::sqrt(re * re + im * im) / (hsum + 1e-9), n_eff = hsum * hsum / (h2sum + 1e-9);
        const double phase = rem_euclid(std::atan2(im, re) / (2 * kPi) * step, step);
        std::vector<long long> slots;
        for (double pos : t.pos) {
            const long long slot = (long long)rte((pos - phase) / step);
            if (std::fabs(pos - (phase + (double)slot * step)) < 0.35 * step) slots.push_back(slot);
        }
        std::sort(slots.begin(), slots.end());
        slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
        const long long n_slots = std::max(1LL, (long long)((double)t.extent / step));
        const double occ = std::min((double)slots.size() / (double)n_slots, 1.0);
        zs.push_back(std::sqrt(2.0 * n_eff) * big_r * occ - 1.0);
    }
    if (zs.size() < 2) return 0.0;
    double s = 0;
    for (double z : zs) s += z;
    return s / std::sqrt((double)zs.size());
}

struct Spectrum {
    std::vector<double> freqs{0.0}, power{0.0}, bg{0.0};
};

Spectrum axis_spectrum(const std::vector<float>& dmap, size_t rows, size_t cols) {
    const size_t win = std::min<size_t>(cols, 1024);
    Spectrum sp;
    if (win < 32) return sp;
    std::vector<double> window(win);
    for (size_t i = 0; i < win; ++i) window[i] = 0.5 - 0.5 * std::cos(2 * kPi * (double)i / ((double)win - 1.0));
    const size_t hop = std::max<size_t>(win / 2, 1), nbins = win / 2 + 1;
    std::vector<double> specs(nbins, 0.0);
    size_t count = 0;
    std::vector<cd> buf(win);
    for (size_t y0 = 0; y0 + 4 <= rows; y0 += 4) {
        std::vector<double> prof(cols, 0.0);
        for (size_t y = y0; y < y0 + 4; ++y) {
            for (size_t x = 0; x < cols; ++x) prof[x] += dmap[y * cols + x];
        }
        double mean = 0;
        for (double& v : prof) {
            v = f32r(v);
            mean += v;
        }
        mean /= (double)cols;
        for (double& v : prof) v -= mean;
        for (size_t x0 = 0; x0 + win <= cols; x0 += hop) {
            for (size_t i = 0; i < win; ++i) buf[i] = cd(prof[x0 + i] * window[i], 0.0);
            Fft(buf, false);
            for (size_t k = 0; k < nbins; ++k) specs[k] += std::norm(buf[k]);
            ++count;
        }
    }
    if (count == 0) return sp;
    sp.power.resize(nbins);
    sp.freqs.resize(nbins);
    for (size_t k = 0; k < nbins; ++k) {
        sp.power[k] = specs[k] / (double)count;
        sp.freqs[k] = (double)k / (double)win;
    }
    size_t ksz = std::max<size_t>(5, sp.power.size() / 24);
    if (ksz % 2 == 0) ++ksz;
    sp.bg = median_filter1d(sp.power, ksz);
    return sp;
}

double spectral_z(const Spectrum& sp, double step) {
    if (step < 2.0 || sp.freqs.size() < 8) return 0.0;
    const double f = 1.0 / step;
    if (f <= sp.freqs[1] || f >= sp.freqs.back()) return 0.0;
    size_t idx = (size_t)(std::lower_bound(sp.freqs.begin(), sp.freqs.end(), f) - sp.freqs.begin());
    if (idx >= sp.power.size()) idx = sp.power.size() - 1;
    const size_t lo = (size_t)std::max(1LL, (long long)idx - 1), hi = std::min(sp.power.size() - 1, idx + 1);
    double p = -std::numeric_limits<double>::max();
    for (size_t i = lo; i <= hi; ++i) p = std::max(p, sp.power[i]);
    const double b = sp.bg[idx] + 1e-12;
    return 6.0 * std::log10(std::max(p / b, 1e-6));
}

struct AxisEvidence {
    std::vector<double> e1;
    std::vector<Tile> tiles1, tiles2;
    Spectrum spec1;
};

std::vector<float> transpose_map(const std::vector<float>& m, size_t rows, size_t cols) {
    std::vector<float> out(rows * cols);
    for (size_t y = 0; y < rows; ++y) {
        for (size_t x = 0; x < cols; ++x) out[x * rows + y] = m[y * cols + x];
    }
    return out;
}

void build_evidence(const uint8_t* rgba, size_t w, size_t h, AxisEvidence& ex, AxisEvidence& ey) {
    const std::vector<uint8_t> base = prep_u8(rgba, w, h);
    const std::vector<uint8_t> quantized = kmeans_quantize_rgba(base.data(), w, h, 16);
    const std::vector<float> img_o = flatten_channels(rgba, w, h), img_q = flatten_channels(quantized.data(), w, h);
    e1_profiles(img_q, w, h, ex.e1, ey.e1);
    const double jz = std::max(jpeg_lattice_strength(ex.e1), jpeg_lattice_strength(ey.e1));
    if (jz > 5.0) {
        ex.e1 = notch_jpeg(ex.e1);
        ey.e1 = notch_jpeg(ey.e1);
    }
    // gradient (from the quantized picture) and curvature (from the original) maps
    std::vector<float> dqx(h * (w - 1)), dqy((h - 1) * w), cox(h * (w - 2)), coy((h - 2) * w);
    for (size_t y = 0; y < h; ++y) {
        const size_t r = y * w * 4;
        for (size_t x = 0; x + 1 < w; ++x) {
            float s = 0;
            for (int c = 0; c < 4; ++c) {
                const float d = img_q[r + (x + 1) * 4 + c] - img_q[r + x * 4 + c];
                s += d * d;
            }
            dqx[y * (w - 1) + x] = std::sqrt(s);
        }
        for (size_t x = 0; x + 2 < w; ++x) {
            float s = 0;
            for (int c = 0; c < 4; ++c) {
                const float d = img_o[r + (x + 2) * 4 + c] - 2.0f * img_o[r + (x + 1) * 4 + c] + img_o[r + x * 4 + c];
                s += d * d;
            }
            cox[y * (w - 2) + x] = std::sqrt(s);
        }
    }
    for (size_t y = 0; y + 1 < h; ++y) {
        const size_t r0 = y * w * 4, r1 = (y + 1) * w * 4;
        for (size_t x = 0; x < w; ++x) {
            float s = 0;
            for (int c = 0; c < 4; ++c) {
                const float d = img_q[r1 + x * 4 + c] - img_q[r0 + x * 4 + c];
                s += d * d;
            }
            dqy[y * w + x] = std::sqrt(s);
        }
    }
    for (size_t y = 0; y + 2 < h; ++y) {
        const size_t r0 = y * w * 4, r1 = (y + 1) * w * 4, r2 = (y + 2) * w * 4;
        for (size_t x = 0; x < w; ++x) {
            float s = 0;
            for (int c = 0; c < 4; ++c) {
                const float d = img_o[r2 + x * 4 + c] - 2.0f * img_o[r1 + x * 4 + c] + img_o[r0 + x * 4 + c];
                s += d * d;
            }
            coy[y * w + x] = std::sqrt(s);
        }
    }
    const std::vector<float> dqy_t = transpose_map(dqy, h - 1, w), coy_t = transpose_map(coy, h - 2, w);
    auto fy = std::async(std::launch::async, [&]() {
        ey.tiles1 = tile_peaks(dqy_t, w, h - 1, 1);
        ey.tiles2 = tile_peaks(coy_t, w, h - 2, 1);
        ey.spec1 = axis_spectrum(dqy_t, w, h - 1);
    });
    ex.tiles1 = tile_peaks(dqx, h, w - 1, 1);
    ex.tiles2 = tile_peaks(cox, h, w - 2, 1);
    ex.spec1 = axis_spectrum(dqx, h, w - 1);
    fy.get();
}

std::vector<double> fused_curve(const AxisEvidence& ev, const std::vector<double>& steps) {
    const std::vector<double> e1n = normalise(ev.e1);
    const size_t n = steps.size();
    std::vector<std::vector<double>> cols(4, std::vector<double>(n));
    ParFor(n, [&](size_t i) {
        cols[0][i] = rayleigh_score(e1n, steps[i]);
        cols[1][i] = tiles_ray_z(ev.tiles1, steps[i]);
        cols[2][i] = tiles_ray_z(ev.tiles2, steps[i]);
        cols[3][i] = spectral_z(ev.spec1, steps[i]);
    });
    std::vector<double> fused(n, 0.0);
    for (const auto& col : cols) {
        double m = 0;
        for (double v : col) m = std::max(m, std::max(v, 0.0));
        if (m <= 1e-9) continue;
        for (size_t i = 0; i < n; ++i) fused[i] += std::max(col[i], 0.0) / m;
    }
    return fused;
}

// ================================================================ varcontrast.rs
struct CellVarContrast {
    size_t w = 0, h = 0, c = 4;
    std::vector<double> sc; // (h+1)*(w+1) x 5 fused summed-area table
    double total_var = 0, active_var = 0;
    std::vector<double> px, py;

    double g(size_t y, size_t x, size_t ch) const { return sc[(y * (w + 1) + x) * (c + 1) + ch]; }

    CellVarContrast(const uint8_t* rgba, size_t w_, size_t h_) : w(w_), h(h_) {
        const size_t stride = w + 1;
        sc.assign((h + 1) * stride * (c + 1), 0.0);
        auto idx = [&](size_t y, size_t x, size_t ch) { return (y * stride + x) * (c + 1) + ch; };
        for (size_t y = 0; y < h; ++y) {
            double row[5] = {0, 0, 0, 0, 0};
            for (size_t x = 0; x < w; ++x) {
                const size_t p = (y * w + x) * 4;
                const double a = rgba[p + 3] / 255.0;
                const double vals[4] = {rgba[p] * a, rgba[p + 1] * a, rgba[p + 2] * a, (double)rgba[p + 3]};
                double sq = 0;
                for (double v : vals) sq += v * v;
                for (size_t ch = 0; ch < c; ++ch) row[ch] += vals[ch];
                row[c] += sq;
                for (size_t ch = 0; ch <= c; ++ch) sc[idx(y + 1, x + 1, ch)] = sc[idx(y, x + 1, ch)] + row[ch];
            }
        }
        const double n = (double)(h * w);
        double mean_sq = 0;
        for (size_t ch = 0; ch < c; ++ch) {
            const double m = sc[idx(h, w, ch)] / n;
            mean_sq += m * m;
        }
        total_var = sc[idx(h, w, c)] / n - mean_sq;
        size_t bs = 8;
        std::vector<size_t> by, bx;
        for (size_t y = 0; y + bs <= h; y += bs) by.push_back(y);
        for (size_t x = 0; x + bs <= w; x += bs) bx.push_back(x);
        if (by.empty() || bx.empty()) {
            by = {0};
            bx = {0};
            bs = std::min(h, w);
        }
        const double area = (double)(bs * bs), thresh = std::max(0.02 * total_var, 1e-6);
        std::vector<double> apx, apy, allx, ally;
        double active_sum = 0;
        size_t active_cnt = 0;
        for (size_t y0 : by) {
            for (size_t x0 : bx) {
                auto rect = [&](size_t ch) { return sc[idx(y0 + bs, x0 + bs, ch)] - sc[idx(y0, x0 + bs, ch)] - sc[idx(y0 + bs, x0, ch)] + sc[idx(y0, x0, ch)]; };
                double msq = 0;
                for (size_t ch = 0; ch < c; ++ch) {
                    const double m = rect(ch) / area;
                    msq += m * m;
                }
                const double bvar = rect(c) / area - msq;
                const double cx = (double)x0 + (double)bs / 2.0, cy = (double)y0 + (double)bs / 2.0;
                allx.push_back(cx);
                ally.push_back(cy);
                if (bvar > thresh) {
                    apx.push_back(cx);
                    apy.push_back(cy);
                    active_sum += bvar;
                    ++active_cnt;
                }
            }
        }
        if (apx.size() < 8) {
            px = allx;
            py = ally;
        } else {
            px = apx;
            py = apy;
        }
        if (px.size() > 2600) {
            const size_t m = px.size();
            std::vector<std::pair<double, double>> pairs;
            for (size_t i = 0; i < 2600; ++i) {
                const size_t s = (size_t)((double)i * ((double)m - 1.0) / 2599.0);
                pairs.push_back({px[s], py[s]});
            }
            std::stable_sort(pairs.begin(), pairs.end(), [&](const auto& a, const auto& b) { return a.second * (double)w + a.first < b.second * (double)w + b.first; });
            px.clear();
            py.clear();
            for (const auto& p : pairs) {
                px.push_back(p.first);
                py.push_back(p.second);
            }
        }
        active_var = active_cnt > 0 ? active_sum / (double)active_cnt : total_var;
    }

    double cells_variance_mean(double step, double phx, double phy) const {
        double acc = 0;
        for (size_t i = 0; i < px.size(); ++i) {
            const double x0 = phx + std::floor((px[i] - phx) / step) * step;
            const double y0 = phy + std::floor((py[i] - phy) / step) * step;
            const size_t ix0 = (size_t)std::clamp(rte(x0), 0.0, (double)w - 1.0), iy0 = (size_t)std::clamp(rte(y0), 0.0, (double)h - 1.0);
            const size_t ix1 = (size_t)std::clamp(rte(x0 + step), (double)ix0 + 1.0, (double)w);
            const size_t iy1 = (size_t)std::clamp(rte(y0 + step), (double)iy0 + 1.0, (double)h);
            const double area = (double)((ix1 - ix0) * (iy1 - iy0));
            double msq = 0;
            for (size_t ch = 0; ch < c; ++ch) {
                const double m = (g(iy1, ix1, ch) - g(iy1, ix0, ch) - g(iy0, ix1, ch) + g(iy0, ix0, ch)) / area;
                msq += m * m;
            }
            const double s2 = g(iy1, ix1, c) - g(iy1, ix0, c) - g(iy0, ix1, c) + g(iy0, ix0, c);
            acc += std::max(s2 / area - msq, 0.0);
        }
        return acc / (double)std::max<size_t>(px.size(), 1);
    }

    double contrast(double step, size_t n_phases) const {
        if (step < 1.5 || step > (double)w / 3.0 || step > (double)h / 3.0 || total_var <= 1e-9 || px.empty()) return 0.0;
        double best = std::numeric_limits<double>::infinity(), worst = -std::numeric_limits<double>::max();
        for (size_t iy = 0; iy < n_phases; ++iy) {
            for (size_t ix = 0; ix < n_phases; ++ix) {
                const double v = cells_variance_mean(step, (double)ix * (step / (double)n_phases), (double)iy * (step / (double)n_phases));
                best = std::min(best, v);
                worst = std::max(worst, v);
            }
        }
        return (worst - best) / (best + 0.05 * active_var);
    }

    void z_curve(std::vector<double>& logs, std::vector<double>& z) const {
        const double l = (double)std::min(w, h);
        const double max_step = std::min(std::max(l / 8.0, 4.0), 64.0);
        std::vector<double> steps;
        for (double s = 2.0; s <= max_step; s *= 1.04) steps.push_back(s);
        std::vector<double> cs(steps.size());
        ParFor(steps.size(), [&](size_t i) { cs[i] = contrast(steps[i], 3); });
        const std::vector<double> base = median_filter1d(cs, 15);
        std::vector<double> resid(cs.size()), absr(cs.size());
        for (size_t i = 0; i < cs.size(); ++i) {
            resid[i] = cs[i] - base[i];
            absr[i] = std::fabs(resid[i]);
        }
        const double sigma = median(absr) * 1.4826 + 1e-9;
        z.resize(cs.size());
        logs.resize(cs.size());
        for (size_t i = 0; i < cs.size(); ++i) {
            z[i] = resid[i] / sigma;
            logs[i] = std::log(steps[i]);
        }
    }
};

double vc_z_of(const std::vector<double>& logs, const std::vector<double>& z, double step) {
    if (logs.empty()) return 0.0;
    const double ls = std::log(step);
    if (ls < logs.front() || ls > logs.back()) return 0.0;
    return interp(ls, logs, z);
}

// ================================================================ reconsearch.rs
constexpr double RS_S_MIN = 1.6, RS_S_MAX = 24.0, RS_RATIO = 1.025;
constexpr size_t MAX_ROWS = 360, N_ROWBLOCKS = 5;

void eigh3_desc(double a[3][3], double evecs[3][3]) {
    double v[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int it = 0; it < 50; ++it) {
        int p = 0, q = 1;
        double mx = 0;
        for (int i = 0; i < 3; ++i) {
            for (int j = i + 1; j < 3; ++j) {
                if (std::fabs(a[i][j]) > mx) {
                    mx = std::fabs(a[i][j]);
                    p = i;
                    q = j;
                }
            }
        }
        if (mx < 1e-14) break;
        const double theta = 0.5 * (a[q][q] - a[p][p]) / a[p][q];
        const double t = (theta >= 0 ? 1.0 : -1.0) / (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
        const double c = 1.0 / std::sqrt(t * t + 1.0), s = t * c;
        const double app = a[p][p], aqq = a[q][q], apq = a[p][q];
        a[p][p] = app - t * apq;
        a[q][q] = aqq + t * apq;
        a[p][q] = a[q][p] = 0.0;
        for (int i = 0; i < 3; ++i) {
            if (i != p && i != q) {
                const double aip = a[i][p], aiq = a[i][q];
                a[i][p] = a[p][i] = c * aip - s * aiq;
                a[i][q] = a[q][i] = s * aip + c * aiq;
            }
        }
        for (int i = 0; i < 3; ++i) {
            const double vip = v[i][p], viq = v[i][q];
            v[i][p] = c * vip - s * viq;
            v[i][q] = s * vip + c * viq;
        }
    }
    int order[3] = {0, 1, 2};
    std::stable_sort(order, order + 3, [&](int i, int j) { return a[i][i] > a[j][j]; });
    for (int oi = 0; oi < 3; ++oi) {
        for (int ch = 0; ch < 3; ++ch) evecs[oi][ch] = v[ch][order[oi]];
    }
}

void rs_prep(const uint8_t* rgba, size_t w, size_t h, std::vector<float>& ch, size_t& nc) {
    const size_t n = w * h;
    std::vector<float> rgb(n * 3), alpha(n);
    for (size_t i = 0; i < n; ++i) {
        const float a = rgba[i * 4 + 3];
        alpha[i] = a;
        for (int c = 0; c < 3; ++c) rgb[i * 3 + c] = rgba[i * 4 + c] * (a / 255.0f);
    }
    // quantize (k <= 14) to the centroid colours
    const std::vector<size_t> sidx = even_sample(n, 48000);
    std::vector<P3> sample(sidx.size());
    for (size_t s = 0; s < sidx.size(); ++s) sample[s] = {rgb[sidx[s] * 3], rgb[sidx[s] * 3 + 1], rgb[sidx[s] * 3 + 2]};
    std::vector<std::array<int, 3>> coarse;
    for (size_t s = 0; s < sample.size(); s += 7) coarse.push_back({(int)rte(sample[s][0] / 8.0), (int)rte(sample[s][1] / 8.0), (int)rte(sample[s][2] / 8.0)});
    std::sort(coarse.begin(), coarse.end());
    coarse.erase(std::unique(coarse.begin(), coarse.end()), coarse.end());
    const size_t k = std::min<size_t>(14, std::max<size_t>(coarse.size(), 2));
    const std::vector<P3> centers = kmeans(sample, k, 25, 0.25, 3, 12345);
    std::vector<float> q(n * 3);
    ParFor((n + 65535) / 65536, [&](size_t chunk) {
        const size_t a = chunk * 65536, b = std::min(n, a + 65536);
        for (size_t i = a; i < b; ++i) {
            size_t best = 0;
            double bd = std::numeric_limits<double>::infinity();
            for (size_t ci = 0; ci < centers.size(); ++ci) {
                double d = 0;
                for (int c = 0; c < 3; ++c) {
                    const double dd = (double)(rgb[i * 3 + c] - centers[ci][c]);
                    d += dd * dd;
                }
                if (d < bd) {
                    bd = d;
                    best = ci;
                }
            }
            for (int c = 0; c < 3; ++c) q[i * 3 + c] = centers[best][c];
        }
    });
    double mu[3] = {0, 0, 0};
    for (size_t i = 0; i < n; ++i) {
        for (int c = 0; c < 3; ++c) mu[c] += q[i * 3 + c];
    }
    for (double& m : mu) m /= (double)n;
    double cov[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    for (size_t i = 0; i < n; ++i) {
        const double x[3] = {q[i * 3] - mu[0], q[i * 3 + 1] - mu[1], q[i * 3 + 2] - mu[2]};
        for (int a = 0; a < 3; ++a) {
            for (int b = 0; b < 3; ++b) cov[a][b] += x[a] * x[b];
        }
    }
    for (auto& row : cov) {
        for (double& v : row) v /= (double)std::max<size_t>(n, 1);
    }
    double evecs[3][3];
    eigh3_desc(cov, evecs);
    double amean = 0;
    for (float a : alpha) amean += a;
    amean /= (double)n;
    double avar = 0;
    for (float a : alpha) avar += ((double)a - amean) * ((double)a - amean);
    avar /= (double)n;
    const bool use_alpha = std::sqrt(avar) > 2.0;
    nc = use_alpha ? 3 : 2;
    ch.assign(n * nc, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        const double x[3] = {q[i * 3] - mu[0], q[i * 3 + 1] - mu[1], q[i * 3 + 2] - mu[2]};
        for (int comp = 0; comp < 2; ++comp) {
            double acc = 0;
            for (int c = 0; c < 3; ++c) acc += x[c] * evecs[comp][c];
            ch[i * nc + comp] = (float)acc;
        }
        if (use_alpha) ch[i * nc + 2] = (float)(alpha[i] - amean);
    }
}

struct RsAxis {
    size_t w = 0, c = 0, nbr = 1;
    std::vector<float> s;
    std::vector<double> q_t;
    std::vector<size_t> r_edges;
    double t_sum = 1;

    RsAxis(const std::vector<float>& ch, size_t W, size_t H, size_t nc, bool y_axis) {
        const size_t rows0 = y_axis ? W : H, cols = y_axis ? H : W;
        auto get = [&](size_t r, size_t col, size_t cc) { return y_axis ? ch[(col * W + r) * nc + cc] : ch[(r * W + col) * nc + cc]; };
        std::vector<size_t> rows_idx;
        if (rows0 > MAX_ROWS) {
            for (size_t i = 0; i < MAX_ROWS; ++i) rows_idx.push_back((size_t)((double)i * ((double)rows0 - 1.0) / ((double)MAX_ROWS - 1.0)));
        } else {
            for (size_t i = 0; i < rows0; ++i) rows_idx.push_back(i);
        }
        const size_t hh = rows_idx.size();
        w = cols;
        c = nc;
        s.assign(hh * (cols + 1) * nc, 0.0f);
        std::vector<double> row_sq(hh, 0.0);
        for (size_t ri = 0; ri < hh; ++ri) {
            const size_t base = ri * (cols + 1) * nc;
            std::vector<float> acc(nc, 0.0f), accq(nc, 0.0f);
            for (size_t col = 0; col < cols; ++col) {
                for (size_t cc = 0; cc < nc; ++cc) {
                    const float v = get(rows_idx[ri], col, cc);
                    acc[cc] += v;
                    accq[cc] += v * v;
                    s[base + (col + 1) * nc + cc] = acc[cc];
                }
            }
            for (float v : accq) row_sq[ri] += v;
        }
        nbr = std::max<size_t>(1, std::min(N_ROWBLOCKS, hh / 48));
        r_edges.resize(nbr + 1);
        for (size_t i = 0; i <= nbr; ++i) r_edges[i] = (size_t)((double)hh * (double)i / (double)nbr);
        q_t.assign(nbr, 0.0);
        for (size_t b = 0; b < nbr; ++b) {
            for (size_t r = r_edges[b]; r < r_edges[b + 1]; ++r) q_t[b] += row_sq[r];
        }
        t_sum = 0;
        for (size_t b = 0; b < nbr; ++b) {
            double en1 = 0;
            for (size_t r = r_edges[b]; r < r_edges[b + 1]; ++r) {
                const size_t base = r * (cols + 1) * nc;
                double sq = 0;
                for (size_t cc = 0; cc < nc; ++cc) {
                    const double tot = s[base + cols * nc + cc];
                    sq += tot * tot;
                }
                en1 += sq / (double)cols;
            }
            t_sum += std::max(q_t[b] - en1, 1e-9);
        }
    }

    size_t n_phase(double st, bool dense) const {
        const size_t n = (size_t)std::clamp(dense ? 4.0 * st : 2.0 * st, 8.0, 40.0);
        return n & ~(size_t)1;
    }

    void eval_s(double st, bool dense, double& eb, double& er) const {
        const size_t n = n_phase(st, dense), nc = c;
        const long long j_cells = (long long)std::ceil((double)w / st) + 2;
        const size_t nb = (size_t)(j_cells + 1);
        std::vector<std::vector<double>> ens(n);
        for (size_t pi = 0; pi < n; ++pi) {
            const double phase = (double)pi * (st / (double)n);
            std::vector<double> bpos(nb);
            std::vector<size_t> i0(nb);
            std::vector<float> fr(nb), wds(nb - 1);
            for (size_t j = 0; j < nb; ++j) {
                const double b = std::clamp(phase + st * ((double)j - 1.0), 0.0, (double)w);
                bpos[j] = b;
                const size_t ii = (size_t)std::clamp(std::floor(b), 0.0, (double)w - 1.0);
                i0[j] = ii;
                fr[j] = (float)(b - (double)ii);
            }
            for (size_t j = 0; j + 1 < nb; ++j) wds[j] = (float)(bpos[j + 1] - bpos[j]);
            std::vector<double> out(nbr, 0.0);
            std::vector<float> g(nb * nc);
            for (size_t b = 0; b < nbr; ++b) {
                double block_acc = 0;
                for (size_t r = r_edges[b]; r < r_edges[b + 1]; ++r) {
                    const size_t base = r * (w + 1) * nc;
                    for (size_t j = 0; j < nb; ++j) {
                        for (size_t cc = 0; cc < nc; ++cc) {
                            const float g0 = s[base + i0[j] * nc + cc], g1 = s[base + (i0[j] + 1) * nc + cc];
                            g[j * nc + cc] = g0 + (g1 - g0) * fr[j];
                        }
                    }
                    double row_en = 0;
                    for (size_t j = 0; j + 1 < nb; ++j) {
                        const float wd = std::max(wds[j], 1e-3f);
                        float sq = 0;
                        for (size_t cc = 0; cc < nc; ++cc) {
                            const float bs = g[(j + 1) * nc + cc] - g[j * nc + cc];
                            sq += bs * bs;
                        }
                        row_en += (double)(sq / wd);
                    }
                    block_acc += f32r(row_en);
                }
                out[b] = block_acc;
            }
            ens[pi] = out;
        }
        double err_b = 0, err_r = 0;
        for (size_t b = 0; b < nbr; ++b) {
            size_t ib = 0;
            for (size_t p = 0; p < n; ++p) {
                if (ens[p][b] > ens[ib][b]) ib = p;
            }
            err_b += std::max(q_t[b] - ens[ib][b], 0.0);
            err_r += std::max(q_t[b] - ens[(ib + n / 2) % n][b], 0.0);
        }
        eb = err_b / t_sum;
        er = err_r / t_sum;
    }
};

std::vector<double> s_grid(size_t size) {
    const double smax = std::min(RS_S_MAX, (double)size / 8.0);
    const long long n = (long long)std::ceil(std::log(smax / RS_S_MIN) / std::log(RS_RATIO));
    std::vector<double> out;
    for (long long i = 0; i <= n; ++i) out.push_back(RS_S_MIN * std::pow(RS_RATIO, (int)i));
    return out;
}

struct Trend {
    std::vector<double> ls, trend;
    Trend(const std::vector<double>& s_list, const std::vector<double>& eb) {
        for (double s : s_list) ls.push_back(std::log(s));
        trend.resize(eb.size());
        for (size_t i = 0; i < eb.size(); ++i) {
            std::vector<double> sel;
            for (size_t j = 0; j < eb.size(); ++j) {
                if (std::fabs(ls[j] - ls[i]) <= 0.14) sel.push_back(eb[j]);
            }
            trend[i] = std::max(median(sel), 1e-6);
        }
    }
    double at(double s) const { return ls.empty() ? 1e-6 : interp(std::log(s), ls, trend); }
};

double rs_score(double eb, double er, double trend_e) { return std::max(er - eb, 0.0) * std::max(1.0 - eb / trend_e, 0.0); }

// ================================================================ core.rs
struct Prop {
    double step_x, step_y;
    long long cols, rows;
};

bool size_close(const Prop& a, const Prop& b) {
    const long long tol_c = std::max(1LL, (long long)rte(0.01 * (double)b.cols)), tol_r = std::max(1LL, (long long)rte(0.01 * (double)b.rows));
    return std::llabs(a.cols - b.cols) <= tol_c && std::llabs(a.rows - b.rows) <= tol_r;
}

double acf_width(const std::vector<double>& ac) {
    std::vector<double> tail = ac.size() > 40 ? std::vector<double>(ac.begin() + 32, ac.begin() + std::min<size_t>(ac.size(), 96)) : std::vector<double>(ac.begin() + ac.size() / 2, ac.end());
    const double base = tail.empty() ? 0.0 : median(tail);
    const double c0 = std::max(ac[0] - base, 1e-9);
    for (size_t lag = 1; lag < std::min<size_t>(ac.size(), 64); ++lag) {
        if (ac[lag] - base < 0.30 * c0) return (double)lag;
    }
    return 64.0;
}

long long int_median(std::vector<long long> v) {
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 ? v[n / 2] : (long long)((double)(v[n / 2 - 1] + v[n / 2]) / 2.0);
}

PixelFixDetection Result(double sx, double sy, long long cols, long long rows, const std::string& consensus) {
    PixelFixDetection r;
    r.stepX = sx;
    r.stepY = sy;
    r.cols = (int)cols;
    r.rows = (int)rows;
    r.consensus = consensus;
    return r;
}

PixelFixDetection detect_fast(const uint8_t* rgba, size_t w, size_t h) {
    auto frl = std::async(std::launch::async, [&]() { return rl_detect(rgba, w, h); });
    const AcPre pre = ac_prepare(rgba, w, h);
    const AcDetection ac = ac_detect_pre(pre, w, h);
    const RlDetection rl = frl.get();
    const Prop p_ac{ac.step_x, ac.step_y, (long long)ac.cols, (long long)ac.rows}, p_rl{rl.step_x, rl.step_y, rl.cols, rl.rows};
    if (size_close(p_ac, p_rl) && std::min(rl.score_x, rl.score_y) >= 0.30) {
        const long long cols = (long long)rte((double)(p_ac.cols + p_rl.cols) / 2.0), rows = (long long)rte((double)(p_ac.rows + p_rl.rows) / 2.0);
        return Result((double)w / (double)cols, (double)h / (double)rows, cols, rows, "fast:ac+rl(S)");
    }
    const SsDetection ss = ss_detect(rgba, w, h);
    const Prop p_ss{ss.step_x, ss.step_y, ss.cols, ss.rows};
    const std::pair<const char*, const Prop*> named[3] = {{"ac", &p_ac}, {"rl", &p_rl}, {"ss", &p_ss}};
    for (int i = 0; i < 3; ++i) {
        for (int j = i + 1; j < 3; ++j) {
            if (size_close(*named[i].second, *named[j].second))
                return Result(named[i].second->step_x, named[i].second->step_y, named[i].second->cols, named[i].second->rows,
                              std::string("fastmode:") + named[i].first + "+" + named[j].first);
        }
    }
    return Result(p_ac.step_x, p_ac.step_y, p_ac.cols, p_ac.rows, "fastmode:lowconf");
}

PixelFixDetection detect_full(const uint8_t* rgba, size_t w, size_t h) {
    auto frl = std::async(std::launch::async, [&]() { return rl_detect(rgba, w, h); });
    const AcPre pre = ac_prepare(rgba, w, h);
    const AcDetection ac = ac_detect_pre(pre, w, h);
    const RlDetection rl = frl.get();
    const Prop p_ac{ac.step_x, ac.step_y, (long long)ac.cols, (long long)ac.rows}, p_rl{rl.step_x, rl.step_y, rl.cols, rl.rows};
    if (size_close(p_ac, p_rl) && std::min(rl.score_x, rl.score_y) >= 0.30) {
        const long long cols = (long long)rte((double)(p_ac.cols + p_rl.cols) / 2.0), rows = (long long)rte((double)(p_ac.rows + p_rl.rows) / 2.0);
        return Result((double)w / (double)cols, (double)h / (double)rows, cols, rows, "fast:ac+rl(S)");
    }
    const SsDetection ss = ss_detect(rgba, w, h);
    const Prop p_ss{ss.step_x, ss.step_y, ss.cols, ss.rows};
    auto aspect_ok = [&](const Prop& p) {
        const double r = ((double)p.cols / (double)std::max(p.rows, 1LL)) / ((double)w / (double)h);
        return std::fabs(std::log(r)) < 0.35;
    };
    {   // fast path: the three cheap detectors agree on the size
        std::string agree;
        int n = 0;
        const std::pair<const char*, const Prop*> trio[3] = {{"ac", &p_ac}, {"rl", &p_rl}, {"ss", &p_ss}};
        for (const auto& t : trio) {
            if (size_close(*t.second, p_ac)) {
                agree += (n++ ? "+" : "") + std::string(t.first);
            }
        }
        if (n >= 3 && aspect_ok(p_ac)) {
            const long long cols = int_median({p_ac.cols, p_rl.cols, p_ss.cols}), rows = int_median({p_ac.rows, p_rl.rows, p_ss.rows});
            return Result((double)w / (double)cols, (double)h / (double)rows, cols, rows, "fast:" + agree);
        }
    }
    // ---- heavy evidence: fused channels, square packer, distillability
    AxisEvidence evx, evy;
    std::vector<double> vc_logs, vc_z;
    std::vector<float> rch;
    size_t nc = 2;
    auto fev = std::async(std::launch::async, [&]() { build_evidence(rgba, w, h, evx, evy); });
    auto fvc = std::async(std::launch::async, [&]() {
        CellVarContrast vc(rgba, w, h);
        vc.z_curve(vc_logs, vc_z);
    });
    rs_prep(rgba, w, h, rch, nc);
    const RsAxis adx(rch, w, h, nc, false), ady(rch, w, h, nc, true);
    auto coarse = [](const RsAxis& ad, size_t extent) {
        const std::vector<double> full = s_grid(extent);
        std::vector<double> s_list;
        for (size_t i = 0; i < full.size(); i += 3) s_list.push_back(full[i]);
        std::vector<double> eb(s_list.size()), er(s_list.size());
        ParFor(s_list.size(), [&](size_t i) { ad.eval_s(s_list[i], false, eb[i], er[i]); });
        return Trend(s_list, eb);
    };
    const Trend trend_x = coarse(adx, w), trend_y = coarse(ady, h);
    fev.get();
    fvc.get();

    const std::vector<double> steps = ladder();
    std::vector<double> log_steps(steps.size());
    for (size_t i = 0; i < steps.size(); ++i) log_steps[i] = std::log(steps[i]);
    auto norm_curve = [](std::vector<double> c) {
        const double m = vmax(c);
        if (m > 0.0) {
            for (double& v : c) v /= m;
        }
        return c;
    };
    const std::vector<double> curve_x = norm_curve(fused_curve(evx, steps)), curve_y = norm_curve(fused_curve(evy, steps));
    auto argmax_first = [](const std::vector<double>& c) {
        size_t i = 0;
        for (size_t j = 0; j < c.size(); ++j) {
            if (c[j] > c[i]) i = j;
        }
        return i;
    };
    const double fu_sx = steps[argmax_first(curve_x)], fu_sy = steps[argmax_first(curve_y)];
    const Prop p_fu{fu_sx, fu_sy, std::max(1LL, (long long)rte((double)w / fu_sx)), std::max(1LL, (long long)rte((double)h / fu_sy))};
    const std::pair<const char*, const Prop*> named[4] = {{"ac", &p_ac}, {"rl", &p_rl}, {"ss", &p_ss}, {"fu", &p_fu}};
    {   // stage 1: supermajority (>= 3 of 4) on the output size
        struct G {
            size_t cnt, i1;
            std::vector<size_t> grp;
        };
        std::vector<G> groups;
        for (size_t i1 = 0; i1 < 4; ++i1) {
            G g{0, i1, {}};
            for (size_t i2 = 0; i2 < 4; ++i2) {
                if (size_close(*named[i2].second, *named[i1].second)) g.grp.push_back(i2);
            }
            g.cnt = g.grp.size();
            groups.push_back(g);
        }
        std::stable_sort(groups.begin(), groups.end(), [](const G& a, const G& b) { return a.cnt > b.cnt; });
        std::vector<G> ok;
        for (const G& g : groups) {
            if (aspect_ok(*named[g.i1].second)) ok.push_back(g);
        }
        if (!ok.empty() && ok[0].cnt >= 3) {
            std::vector<long long> cs, rs;
            std::string names;
            for (size_t i : ok[0].grp) {
                cs.push_back(named[i].second->cols);
                rs.push_back(named[i].second->rows);
                names += (names.empty() ? "" : "+") + std::string(named[i].first);
            }
            const long long cols = int_median(cs), rows = int_median(rs);
            return Result((double)w / (double)cols, (double)h / (double)rows, cols, rows, names);
        }
    }
    // ---- stage 2: arbitration
    const double detail_cap_x = 8.0 * acf_width(pre.ex.acf), detail_cap_y = 8.0 * acf_width(pre.ey.acf);
    auto recon_at = [&](bool x_axis, double s) {
        const RsAxis& ad = x_axis ? adx : ady;
        const Trend& tr = x_axis ? trend_x : trend_y;
        const size_t extent = x_axis ? w : h;
        if (s < 2.0 || s > (double)extent / 8.0) return 0.0;
        double eb, er;
        ad.eval_s(s, false, eb, er);
        return rs_score(eb, er, tr.at(s));
    };
    auto fused_at = [&](bool x_axis, double s) {
        if (s < steps.front() || s > steps.back()) return 0.0;
        return interp(std::log(s), log_steps, x_axis ? curve_x : curve_y);
    };
    auto vc_at = [&](double s) { return std::max(0.0, std::clamp(vc_z_of(vc_logs, vc_z, s) / 10.0, 0.0, 1.0)); };
    constexpr double AGREE_TOL = 0.02, AGREE_BONUS = 0.25, VC_W = 0.20, SMALLEST_QUALIFIED = 0.78;
    auto pick_axis = [&](bool x_axis, const std::vector<std::pair<double, double>>& ac_cands, size_t extent) {
        std::vector<double> sources;
        for (const auto& n : named) sources.push_back(x_axis ? n.second->step_x : n.second->step_y);
        std::vector<double> pool = sources;
        for (size_t i = 0; i < ac_cands.size() && i < 5; ++i) pool.push_back(ac_cands[i].first);
        std::vector<double> cands;
        for (double s : pool) {
            if (s <= 1.2 || s > (double)extent / 3.0) continue;
            bool dup = false;
            for (double s2 : cands) dup = dup || std::fabs(std::log(s / s2)) < 0.01;
            if (!dup) cands.push_back(s);
        }
        if (cands.empty()) return x_axis ? p_ac.step_x : p_ac.step_y;
        std::vector<double> rn(cands.size());
        for (size_t i = 0; i < cands.size(); ++i) rn[i] = recon_at(x_axis, cands[i]);
        const double rmax = vmax(rn);
        const bool recon_ok = rmax > 0.005;
        const double cap = x_axis ? detail_cap_x : detail_cap_y;
        std::vector<std::pair<double, double>> scored;
        for (size_t i = 0; i < cands.size(); ++i) {
            const double s = cands[i];
            size_t agree = 0;
            for (double v : sources) agree += std::fabs(std::log(std::max(v, 1e-9) / s)) < AGREE_TOL ? 1 : 0;
            double sc = fused_at(x_axis, s) + VC_W * vc_at(s) + AGREE_BONUS * std::max((double)agree - 1.0, 0.0);
            if (recon_ok) sc += 0.6 * (rn[i] / rmax);
            if (s > cap) sc *= 0.35;
            scored.push_back({s, sc});
        }
        double best = -std::numeric_limits<double>::max();
        for (const auto& p : scored) best = std::max(best, p.second);
        double out = std::numeric_limits<double>::infinity();
        for (const auto& p : scored) {
            if (p.second >= SMALLEST_QUALIFIED * best) out = std::min(out, p.first);
        }
        return out;
    };
    double sx = pick_axis(true, pre.ex.cands, w), sy = pick_axis(false, pre.ey.cands, h);
    if (std::fabs(std::log(sx / sy)) > 0.45) { // aspect guard: the finer step when it has real fused support
        const bool fine_x = sx < sy;
        const double s_fine = fine_x ? sx : sy;
        if (fused_at(fine_x, s_fine) >= 0.35) sx = sy = s_fine;
    }
    if (std::fabs(std::log(sx / sy)) > 0.45) {
        auto both = [&](double s) {
            const double rx = recon_at(true, s), ry = recon_at(false, s);
            const double r_term = std::max(rx, ry) > 0.005 ? 0.5 * (rx + ry) / std::max(std::max(rx, ry), 1e-9) : 0.0;
            double v = r_term + fused_at(true, s) + fused_at(false, s) + 2.0 * VC_W * vc_at(s);
            if (s > std::min(detail_cap_x, detail_cap_y)) v *= 0.35;
            return v;
        };
        if (both(sx) >= both(sy)) sy = sx;
        else sx = sy;
    }
    sx = refine_step_acf(pre.ex.acf, sx, 24);
    sy = refine_step_acf(pre.ey.acf, sy, 24);
    if (std::fabs(std::log(sx / sy)) < 0.08 && std::fabs(sx - sy) > 1e-6) {
        const double s = 2.0 * sx * sy / (sx + sy);
        sx = refine_step_acf(pre.ex.acf, s, 24);
        sy = refine_step_acf(pre.ey.acf, s, 24);
    }
    auto fr = std::async(std::launch::async, [&]() { return local_count(pre.maps_y, h, sy); });
    const double n_cols = local_count(pre.maps_x, w, sx);
    const double n_rows = fr.get();
    return Result((double)w / n_cols, (double)h / n_rows, (long long)std::max(1.0, rte(n_cols)), (long long)std::max(1.0, rte(n_rows)), "arbitrated");
}

} // namespace
} // namespace pf

// ---------------------------------------------------------------------------------------------------------------
bool PixelFixDetect(const uint8_t* rgba, int width, int height, bool full, PixelFixDetection& out) {
    if (!rgba || std::min(width, height) < 16 || (long long)width * height > 4000000LL) return false;
    out = full ? pf::detect_full(rgba, (size_t)width, (size_t)height) : pf::detect_fast(rgba, (size_t)width, (size_t)height);
    const std::string& c = out.consensus;
    if (c.rfind("fast:", 0) == 0) out.confidence = "high";
    else if (c == "arbitrated" || (c.rfind("fastmode:", 0) == 0 && c.find('+') != std::string::npos)) out.confidence = "medium";
    else out.confidence = "low"; // as the reference labels it (a 3-of-4 supermajority included)
    return out.cols > 0 && out.rows > 0;
}

std::vector<uint8_t> PixelFixPack(const uint8_t* rgba, int width, int height, int cols, int rows, int kColors) {
    using namespace pf;
    const size_t w = (size_t)width, h = (size_t)height;
    cols = std::max(1, cols);
    rows = std::max(1, rows);
    const size_t k_req = kColors > 0 ? (size_t)kColors : adaptive_k(rgba, w, h, 16, 48, 0.003);
    std::vector<uint32_t> labels;
    size_t kc = 1;
    kmeans_labels(rgba, w, h, k_req, labels, kc);
    kc = std::max<size_t>(kc, 1);
    const size_t n = (size_t)cols * rows;
    const double cw = (double)w / cols, ch = (double)h / rows;
    std::vector<size_t> ixs(w), iys(h);
    std::vector<double> wxs(w), wys(h);
    for (size_t x = 0; x < w; ++x) {
        ixs[x] = std::min((x * (size_t)cols) / w, (size_t)cols - 1);
        const double fx = ((double)x + 0.5 - (double)ixs[x] * cw) / cw;
        wxs[x] = 1.0 - 2.0 * std::fabs(fx - 0.5);
    }
    for (size_t y = 0; y < h; ++y) {
        iys[y] = std::min((y * (size_t)rows) / h, (size_t)rows - 1);
        const double fy = ((double)y + 0.5 - (double)iys[y] * ch) / ch;
        wys[y] = 1.0 - 2.0 * std::fabs(fy - 0.5);
    }
    // stage 1: the winning label of every cell (a centre-weighted vote)
    std::vector<double> wsum(n * kc, 0.0);
    for (size_t y = 0; y < h; ++y) {
        for (size_t x = 0; x < w; ++x) wsum[(iys[y] * cols + ixs[x]) * kc + labels[y * w + x]] += wys[y] * wxs[x] + 1e-4;
    }
    std::vector<uint32_t> win(n, 0);
    for (size_t c = 0; c < n; ++c) {
        size_t bi = 0;
        double bv = wsum[c * kc];
        for (size_t l = 1; l < kc; ++l) {
            if (wsum[c * kc + l] > bv) {
                bv = wsum[c * kc + l];
                bi = l;
            }
        }
        win[c] = (uint32_t)bi;
    }
    // stage 2: the colour of the original pixels that carry the winning label
    std::vector<std::array<double, 3>> csum(n, {0, 0, 0}), msum(n, {0, 0, 0});
    std::vector<double> wden(n, 0.0), selcnt(n, 0.0), cnt(n, 0.0), asum(n, 0.0);
    for (size_t y = 0; y < h; ++y) {
        for (size_t x = 0; x < w; ++x) {
            const size_t i = y * w + x, p = i * 4, cell = iys[y] * cols + ixs[x];
            const double wgt = wys[y] * wxs[x] + 1e-4;
            const double rgb[3] = {rgba[p] / 255.0, rgba[p + 1] / 255.0, rgba[p + 2] / 255.0};
            cnt[cell] += 1.0;
            for (int c = 0; c < 3; ++c) msum[cell][c] += rgb[c];
            if (rgba[p + 3] > 127) asum[cell] += 1.0;
            if (labels[i] == win[cell]) {
                selcnt[cell] += 1.0;
                wden[cell] += wgt;
                for (int c = 0; c < 3; ++c) csum[cell][c] += rgb[c] * wgt;
            }
        }
    }
    std::vector<uint8_t> out(n * 4);
    for (size_t c = 0; c < n; ++c) {
        double color[3];
        for (int k = 0; k < 3; ++k) color[k] = selcnt[c] >= 0.5 && wden[c] > 1e-9 ? csum[c][k] / wden[c] : msum[c][k] / std::max(cnt[c], 1.0);
        for (int k = 0; k < 3; ++k) out[c * 4 + k] = (uint8_t)std::clamp(rte(color[k] * 255.0), 0.0, 255.0);
        out[c * 4 + 3] = asum[c] / std::max(cnt[c], 1.0) > 0.5 ? 255 : 0;
    }
    return out;
}
