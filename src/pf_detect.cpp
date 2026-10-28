// Pixel Art Fixer port: the three cheap, phase-free detectors (autocorr.rs, runlengths.rs, selfsim.rs).
#include <future>
#include <map>

#include "pf_internal.h"

namespace pf {

// ================================================================ autocorr.rs
namespace {

AxisEstimate axis_estimate(const std::vector<FeatMap>& maps, size_t extent) {
    const std::vector<double> steps = arange(MIN_STEP, std::min(MAX_STEP, (double)extent / 4.0), STEP_GRID);
    const size_t ns = steps.size();
    std::vector<double> raw(ns, 0.0), ac_sum(extent, 0.0);
    struct Task {
        size_t mi, band;
        double bw;
    };
    std::vector<Task> tasks;
    for (size_t mi = 0; mi < maps.size(); ++mi) {
        const size_t bands[3] = {12, BAND, maps[mi].lines};
        const double bws[3] = {0.5, 1.0, 1.0};
        for (int b = 0; b < 3; ++b) tasks.push_back({mi, bands[b], bws[b]});
    }
    std::vector<std::vector<double>> acs(tasks.size()), combs(tasks.size());
    ParFor(tasks.size(), [&](size_t t) {
        const FeatMap& m = maps[tasks[t].mi];
        acs[t] = band_acf(band_profiles(m.data, m.lines, m.extent, tasks[t].band));
        combs[t].resize(ns);
        for (size_t i = 0; i < ns; ++i) combs[t][i] = comb_score(acs[t], steps[i], 24, 12.0);
    });
    for (size_t t = 0; t < tasks.size(); ++t) {
        const double wgt = maps[tasks[t].mi].weight * tasks[t].bw;
        for (size_t i = 0; i < extent; ++i) ac_sum[i] += wgt * acs[t][i];
        for (size_t i = 0; i < ns; ++i) raw[i] += wgt * combs[t][i];
    }
    // cepstrum vote on the primary feature map
    const std::vector<double> c = band_cepstrum(band_profiles(maps[0].data, maps[0].lines, maps[0].extent, BAND));
    const double raw_max = std::max(vmax(raw), 1e-9);
    for (size_t i = 0; i < ns; ++i) {
        const double cz = std::max(ceps_score(c, steps[i]), 0.0);
        raw[i] += 0.1 * cz * std::max(raw[i], 0.0) / raw_max;
    }
    // subharmonic suppression (selection only)
    std::vector<double> pen(ns, 0.0);
    for (double m : {2.0, 3.0, 4.0, 5.0}) {
        for (size_t i = 0; i < ns; ++i) {
            const double s_sub = steps[i] / m;
            if (s_sub < steps[0]) continue;
            const double pos = std::max((s_sub - steps[0]) / STEP_GRID, 0.0);
            pen[i] = std::max(pen[i], std::max(interp_at(raw, pos), 0.0));
        }
    }
    std::vector<double> sel(ns);
    for (size_t i = 0; i < ns; ++i) sel[i] = raw[i] - 0.5 * pen[i];
    std::vector<size_t> loc;
    for (size_t i = 1; i + 1 < ns; ++i) {
        if (sel[i] > sel[i - 1] && sel[i] >= sel[i + 1]) loc.push_back(i);
    }
    std::stable_sort(loc.begin(), loc.end(), [&](size_t a, size_t b) { return sel[a] > sel[b]; });
    if (loc.size() > 8) loc.resize(8);
    std::vector<std::pair<double, double>> cands;
    for (size_t i : loc) {
        double s = steps[i];
        if (i > 0 && i < ns - 1) {
            const double d = (raw[i - 1] - raw[i + 1]) / (2.0 * (raw[i - 1] - 2.0 * raw[i] + raw[i + 1]) + 1e-12);
            s = steps[i] + std::clamp(d, -1.0, 1.0) * STEP_GRID;
        }
        cands.push_back({s, sel[i]});
    }
    if (cands.empty()) {
        size_t i = 0;
        for (size_t j = 0; j < ns; ++j) {
            if (sel[j] > sel[i]) i = j;
        }
        cands.push_back({ns ? steps[i] : 8.0, ns ? sel[i] : 0.0});
    }
    // near-tie disambiguation via peak-train quality on the pooled ACF
    if (cands.size() >= 2 && cands[1].second >= 0.85 * cands[0].second) {
        const double a0 = std::max(ac_sum[0], 1e-12);
        std::vector<double> ac_norm(ac_sum.size());
        for (size_t i = 0; i < ac_sum.size(); ++i) ac_norm[i] = ac_sum[i] / a0;
        std::vector<std::pair<double, double>> top;
        for (const auto& cc : cands) {
            if (cc.second >= 0.85 * cands[0].second && top.size() < 3) top.push_back(cc);
        }
        size_t best_j = 0;
        double best_v = -std::numeric_limits<double>::max();
        for (size_t j = 0; j < top.size(); ++j) {
            const double v = top[j].second * (0.1 + train_quality(ac_norm, top[j].first, 20));
            if (v > best_v) {
                best_v = v;
                best_j = j;
            }
        }
        if (best_j != 0) {
            const auto chosen = top[best_j];
            std::vector<std::pair<double, double>> next{chosen};
            for (const auto& cc : cands) {
                if (cc != chosen) next.push_back(cc);
            }
            cands = next;
        }
    }
    for (auto& cc : cands) cc.first = refine_step_acf(ac_sum, cc.first, 24);
    AxisEstimate e;
    e.step = cands[0].first;
    e.cands = cands;
    e.acf = std::move(ac_sum);
    return e;
}

std::vector<FeatMap> build_maps(const std::vector<float>& g, const std::vector<float>& gq, size_t w, size_t h, bool y_axis) {
    std::vector<FeatMap> out(2);
    if (!y_axis) {
        out[0] = {d2x(g, w, h), h, w, 1.0};
        out[1] = {d1x(gq, w, h), h, w, 0.7};
    } else {
        const std::vector<float> gt = transpose(g, w, h), gqt = transpose(gq, w, h);
        out[0] = {d2x(gt, h, w), w, h, 1.0};
        out[1] = {d1x(gqt, h, w), w, h, 0.7};
    }
    return out;
}

} // namespace

double local_count(const std::vector<FeatMap>& maps, size_t extent, double s0) {
    const double uniform = (double)extent / s0;
    const size_t nw = (size_t)std::clamp((double)extent / (14.0 * s0), 1.0, 8.0);
    if (nw < 2 || uniform < 96.0) return uniform;
    std::vector<size_t> edges(nw + 1);
    for (size_t i = 0; i <= nw; ++i) edges[i] = (size_t)((double)extent * (double)i / (double)nw);
    std::vector<double> counts(nw, 0.0);
    ParFor(nw, [&](size_t wdw) {
        const size_t a = edges[wdw], b = edges[wdw + 1], wlen = b - a;
        std::vector<double> ac_loc(wlen, 0.0);
        for (const FeatMap& m : maps) {
            std::vector<float> sl(m.lines * wlen);
            for (size_t y = 0; y < m.lines; ++y) std::copy(m.data.begin() + y * m.extent + a, m.data.begin() + y * m.extent + a + wlen, sl.begin() + y * wlen);
            for (size_t band : {BAND, m.lines}) {
                const std::vector<double> ac = band_acf(band_profiles(sl, m.lines, wlen, band));
                for (size_t i = 0; i < wlen; ++i) ac_loc[i] += m.weight * 1.0 * ac[i];
            }
        }
        const std::vector<double> fr = arange(0.85, 1.18, 0.01);
        std::vector<double> scan(fr.size()), sc(fr.size());
        for (size_t i = 0; i < fr.size(); ++i) {
            scan[i] = s0 * fr[i];
            sc[i] = comb_score(ac_loc, scan[i], 12, 8.0);
        }
        const double s_glob = comb_score(ac_loc, s0, 12, 8.0);
        double s_i;
        if (vmax(sc) > std::max(1.6 * std::max(s_glob, 0.0), 0.02)) {
            size_t i = 0;
            for (size_t j = 0; j < sc.size(); ++j) {
                if (sc[j] > sc[i]) i = j;
            }
            s_i = refine_step_acf(ac_loc, scan[i], 12);
        } else {
            s_i = refine_step_acf(ac_loc, s0, 10);
        }
        s_i = std::clamp(s_i, 0.82 * s0, 1.2 * s0);
        counts[wdw] = (double)wlen / s_i;
    });
    double total = 0;
    for (double c : counts) total += c;
    return std::fabs(total - uniform) >= 2.5 ? total : uniform;
}

AcPre ac_prepare(const uint8_t* rgba, size_t w, size_t h) {
    const std::vector<float> g = to_gray(rgba, w, h);
    const std::vector<float> gq = median_quant(g, w, h);
    AcPre pre;
    pre.maps_x = build_maps(g, gq, w, h, false);
    pre.maps_y = build_maps(g, gq, w, h, true);
    auto fy = std::async(std::launch::async, [&]() { return axis_estimate(pre.maps_y, h); });
    pre.ex = axis_estimate(pre.maps_x, w);
    pre.ey = fy.get();
    return pre;
}

AcDetection ac_detect_pre(const AcPre& pre, size_t w, size_t h) {
    const AxisEstimate& ex = pre.ex;
    const AxisEstimate& ey = pre.ey;
    double sx = ex.step, sy = ey.step;
    const auto& cx = ex.cands;
    const auto& cy = ey.cands;
    // cross-axis harmonic reconciliation (dither anti-comb suppression)
    auto reconcile = [](double s_small, double s_big, const std::vector<double>& ac_small) {
        for (double m : {2.0, 3.0}) {
            if (std::fabs(s_big / s_small - m) <= 0.06 * m) {
                const double pc_big = plain_comb(ac_small, s_big, 16, 8.0);
                const double pc_small = plain_comb(ac_small, s_small, 16, 8.0);
                if (pc_big >= 0.55 * pc_small && pc_big > 0.0) return refine_step_acf(ac_small, s_big, 24);
            }
        }
        return s_small;
    };
    if (sx < sy) sx = reconcile(sx, sy, ex.acf);
    else if (sy < sx) sy = reconcile(sy, sx, ey.acf);

    auto zmax = [](const std::vector<std::pair<double, double>>& c) {
        double m = -std::numeric_limits<double>::max();
        for (const auto& p : c) m = std::max(m, p.second);
        return m;
    };
    // joint square-ish pairing on strong runner-up agreement
    if (std::fabs(std::log(sx / sy)) > 0.08) {
        const double zx0 = zmax(cx), zy0 = zmax(cy);
        bool have = false;
        double p1 = 0, p2 = 0, best_sum = 0;
        for (size_t i = 0; i < cx.size() && i < 6; ++i) {
            for (size_t j = 0; j < cy.size() && j < 6; ++j) {
                const double s1 = cx[i].first, z1 = cx[i].second, s2 = cy[j].first, z2 = cy[j].second;
                if (std::fabs(std::log(s1 / s2)) <= 0.08 && z1 >= 0.6 * zx0 && z2 >= 0.6 * zy0 && z1 + z2 > best_sum) {
                    best_sum = z1 + z2;
                    p1 = s1;
                    p2 = s2;
                    have = true;
                }
            }
        }
        if (have && (std::fabs(std::log(p1 / sx)) > 1e-6 || std::fabs(std::log(p2 / sy)) > 1e-6)) {
            sx = refine_step_acf(ex.acf, p1, 24);
            sy = refine_step_acf(ey.acf, p2, 24);
        }
    }
    // looser second pass for mild disagreement (5-13%)
    const double dis = std::fabs(std::log(sx / sy));
    if (dis > 0.05 && dis <= 0.13) {
        const double zx0 = zmax(cx), zy0 = zmax(cy);
        auto pull = [](const std::vector<std::pair<double, double>>& cands, double z0, double s_other, bool& found) {
            std::pair<double, double> best{0, 0};
            found = false;
            for (size_t i = 0; i < cands.size() && i < 6; ++i) {
                const double s = cands[i].first, z = cands[i].second;
                if (std::fabs(std::log(s / s_other)) <= 0.08 && z >= 0.3 * z0 && (!found || z > best.second)) {
                    best = {s, z};
                    found = true;
                }
            }
            return best;
        };
        bool fy = false, fx = false;
        const auto move_y = pull(cy, zy0, sx, fy);
        const auto move_x = pull(cx, zx0, sy, fx);
        const double score_a = zx0 + (fy ? move_y.second : -1e9);
        const double score_b = zy0 + (fx ? move_x.second : -1e9);
        if (fy && score_a >= score_b) {
            const double s_new = refine_step_acf(ey.acf, move_y.first, 24);
            sy = std::fabs(std::log(s_new / move_y.first)) < 0.04 ? s_new : move_y.first;
        } else if (fx) {
            const double s_new = refine_step_acf(ex.acf, move_x.first, 24);
            sx = std::fabs(std::log(s_new / move_x.first)) < 0.04 ? s_new : move_x.first;
        }
    }
    auto fr = std::async(std::launch::async, [&]() { return local_count(pre.maps_y, h, sy); });
    const double n_cols = local_count(pre.maps_x, w, sx);
    const double n_rows = fr.get();
    AcDetection d;
    d.step_x = (double)w / n_cols;
    d.step_y = (double)h / n_rows;
    d.cols = (size_t)std::max(1.0, rte(n_cols));
    d.rows = (size_t)std::max(1.0, rte(n_rows));
    return d;
}

// ================================================================ runlengths.rs
namespace {
constexpr double RL_S_MIN = 2.05, RL_S_MAX = 26.0, RUN_MIN = 2.0, RUN_MAX = 64.0, RL_BIN = 0.25;
constexpr size_t COHERENCE = 7, MAX_LAG = 4;
constexpr float THR_FRAC = 0.10f;

std::vector<uint8_t> median3_channel(const std::vector<uint8_t>& src, size_t w, size_t h) {
    std::vector<uint8_t> out(w * h);
    ParFor(h, [&](size_t y) {
        uint8_t buf[9];
        for (size_t x = 0; x < w; ++x) {
            int k = 0;
            for (long long dy = -1; dy <= 1; ++dy) {
                const size_t yy = (size_t)std::clamp((long long)y + dy, 0LL, (long long)h - 1);
                for (long long dx = -1; dx <= 1; ++dx) buf[k++] = src[yy * w + (size_t)std::clamp((long long)x + dx, 0LL, (long long)w - 1)];
            }
            std::sort(buf, buf + 9);
            out[y * w + x] = buf[4];
        }
    });
    return out;
}
} // namespace

std::vector<uint8_t> prep_u8(const uint8_t* rgba, size_t w, size_t h) {
    std::vector<std::vector<uint8_t>> chans(4);
    for (int c = 0; c < 4; ++c) {
        std::vector<uint8_t> plane(w * h);
        for (size_t i = 0; i < w * h; ++i) plane[i] = rgba[i * 4 + c];
        chans[c] = median3_channel(plane, w, h);
    }
    std::vector<uint8_t> out(w * h * 4);
    for (size_t i = 0; i < w * h; ++i) {
        for (int c = 0; c < 4; ++c) out[i * 4 + c] = chans[c][i];
    }
    return out;
}

namespace {
std::vector<float> transpose4(const std::vector<float>& img, size_t w, size_t h) {
    std::vector<float> out(w * h * 4);
    for (size_t y = 0; y < h; ++y) {
        for (size_t x = 0; x < w; ++x) std::copy(img.begin() + (y * w + x) * 4, img.begin() + (y * w + x) * 4 + 4, out.begin() + (x * h + y) * 4);
    }
    return out;
}

float percentile95_positive(const std::vector<float>& d) {
    std::vector<float> v;
    for (float x : d) {
        if (x > 0) v.push_back(x);
    }
    if (v.empty()) return 0.0f;
    std::sort(v.begin(), v.end());
    const double pos = 0.95 * ((double)v.size() - 1.0);
    const size_t lo = (size_t)std::floor(pos), hi = (size_t)std::ceil(pos);
    const float f = (float)(pos - (double)lo);
    return v[lo] + (v[hi] - v[lo]) * f;
}

void boundaries(const std::vector<float>& img4, size_t rows, size_t cols, std::vector<long long>& ys, std::vector<double>& pos) {
    ys.clear();
    pos.clear();
    if (cols < 2) return;
    const size_t dw = cols - 1;
    std::vector<float> d(rows * dw);
    ParFor(rows, [&](size_t y) {
        for (size_t x = 0; x < dw; ++x) {
            const size_t a = (y * cols + x) * 4, b = a + 4;
            float s = 0;
            for (int c = 0; c < 4; ++c) s += std::fabs(img4[b + c] - img4[a + c]);
            d[y * dw + x] = s;
        }
    });
    std::vector<float> sm(rows * dw);
    const long long half = (long long)COHERENCE / 2;
    ParFor(rows, [&](size_t y) {
        for (size_t x = 0; x < dw; ++x) {
            double acc = 0;
            for (long long k = -half; k <= half; ++k) acc += d[(size_t)std::clamp((long long)y + k, 0LL, (long long)rows - 1) * dw + x];
            sm[y * dw + x] = (float)(acc / (double)COHERENCE);
        }
    });
    const float p95 = percentile95_positive(sm);
    const float thr = std::max(20.0f, THR_FRAC * p95);
    for (size_t y = 0; y < rows; ++y) {
        const size_t r = y * dw;
        for (size_t x = 0; x < dw; ++x) {
            const float dc = sm[r + x];
            const float left = x == 0 ? 0.0f : sm[r + x - 1];
            const float right = x == dw - 1 ? 0.0f : sm[r + x + 1];
            if (dc > thr && dc > left && dc >= right) {
                const float dl = sm[r + (x == 0 ? 0 : x - 1)];
                const float dr = sm[r + std::min(x + 1, dw - 1)];
                const float denom = dl - 2.0f * dc + dr;
                const float off = std::fabs(denom) > 1e-6f ? std::clamp(0.5f * (dl - dr) / denom, -0.5f, 0.5f) : 0.0f;
                ys.push_back((long long)y);
                pos.push_back((double)x + (double)off);
            }
        }
    }
    if (pos.size() < 4) {
        ys.clear();
        pos.clear();
    }
}

std::vector<float> lag_diffs(const std::vector<long long>& ys, const std::vector<double>& pos, size_t max_lag) {
    std::vector<float> out;
    const size_t n = pos.size();
    for (size_t lag = 1; lag <= max_lag; ++lag) {
        if (n <= lag) break;
        for (size_t i = 0; i + lag < n; ++i) {
            if (ys[i + lag] == ys[i]) {
                const double d = pos[i + lag] - pos[i];
                if (d >= RUN_MIN && d <= RUN_MAX) out.push_back((float)d);
            }
        }
    }
    return out;
}

void hist(const std::vector<float>& runs, double bin_w, std::vector<double>& hv, std::vector<double>& cv) {
    const size_t nb = (size_t)(RUN_MAX / bin_w) + 1;
    std::vector<uint64_t> counts(nb, 0);
    auto edge = [&](size_t j) { return RUN_MAX * (double)j / (double)nb; };
    for (float r : runs) {
        const double v = r;
        if (!(v >= 0.0 && v <= RUN_MAX)) continue;
        size_t i = std::min((size_t)(v / RUN_MAX * (double)nb), nb - 1);
        if (v < edge(i) && i > 0) --i;
        else if (i + 1 < nb && v >= edge(i + 1)) ++i;
        ++counts[i];
    }
    hv.clear();
    cv.clear();
    for (size_t i = 0; i < nb; ++i) {
        if (counts[i] > 0) {
            hv.push_back((double)counts[i]);
            cv.push_back(0.5 * (edge(i) + edge(i + 1)));
        }
    }
}

struct PickResult {
    bool has = false;
    double s = 0, v = 0;
};

PickResult pick_step(const std::vector<float>& runs, const std::vector<double>& s_grid) {
    PickResult none;
    if (runs.empty()) return none;
    std::vector<double> hv, cv;
    hist(runs, RL_BIN, hv, cv);
    double total = 0;
    for (double v : hv) total += v;
    if (total < 50.0) return none;
    const size_t ns = s_grid.size();
    std::vector<double> sc(ns);
    ParFor(ns, [&](size_t si) {
        double acc = 0;
        for (size_t j = 0; j < cv.size(); ++j) acc += hv[j] * std::cos(2.0 * kPi * cv[j] / s_grid[si]);
        sc[si] = acc / total;
    });
    std::vector<size_t> idx;
    for (size_t i = 1; i + 1 < ns; ++i) {
        if (sc[i] > sc[i - 1] && sc[i] >= sc[i + 1]) idx.push_back(i);
    }
    if (idx.empty()) return none;
    std::vector<size_t> order = idx;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return sc[a] > sc[b]; });
    const double smax = sc[order[0]];
    if (smax <= 0.0) return none;
    auto fund = [&](double s) {
        const double tol = std::max(0.6, 0.18 * s);
        size_t cnt = 0;
        for (float r : runs) {
            if (std::fabs((double)r - s) < tol) ++cnt;
        }
        return (double)cnt / total;
    };
    std::vector<std::pair<double, double>> tied;
    for (size_t i : order) {
        if (sc[i] >= 0.70 * smax) tied.push_back({s_grid[i], sc[i]});
    }
    std::stable_sort(tied.begin(), tied.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    PickResult r;
    r.has = true;
    r.s = s_grid[order[0]];
    r.v = smax;
    for (const auto& t : tied) {
        if (fund(t.first) >= 0.04) {
            r.s = t.first;
            r.v = t.second;
            break;
        }
    }
    return r;
}

double refine_rl(const std::vector<float>& runs, double s0) {
    if (runs.empty()) return s0;
    std::vector<double> hv, cv;
    hist(runs, 0.05, hv, cv);
    const std::vector<double> fine = arange(0.94 * s0, 1.06 * s0, 0.002);
    std::vector<double> sf(fine.size());
    for (size_t fi = 0; fi < fine.size(); ++fi) {
        double acc = 0;
        for (size_t j = 0; j < cv.size(); ++j) acc += hv[j] * (cv[j] / s0) * std::cos(2.0 * kPi * cv[j] / fine[fi]);
        sf[fi] = acc;
    }
    size_t best_i = 0;
    for (size_t fi = 0; fi < sf.size(); ++fi) {
        if (sf[fi] > sf[best_i]) best_i = fi;
    }
    double s = fine.empty() ? s0 : fine[best_i];
    for (int it = 0; it < 2; ++it) {
        double num = 0, den = 0;
        for (float r : runs) {
            const double rf = r;
            const double k = rte(rf / s);
            if (k < 1.0) continue;
            const double res = std::fabs(rf - k * s);
            const double w = std::clamp(1.0 - res / (0.30 * s), 0.0, 1.0) * k;
            num += w * k * rf;
            den += w * k * k;
        }
        if (den <= 0.0) break;
        s = num / den;
    }
    return s;
}

bool tile_peak(const std::vector<float>& diffs, double s0, double& out) {
    if (diffs.size() < 350) return false;
    const std::vector<double> fine = arange(0.87 * s0, 1.13 * s0, 0.005);
    std::vector<double> hv, cv;
    hist(diffs, 0.05, hv, cv);
    double total = 0;
    for (double v : hv) total += v;
    std::vector<double> sf(fine.size());
    for (size_t fi = 0; fi < fine.size(); ++fi) {
        double acc = 0;
        for (size_t j = 0; j < cv.size(); ++j) acc += hv[j] * std::cos(2.0 * kPi * cv[j] / fine[fi]);
        sf[fi] = acc / total;
    }
    size_t best_i = 0;
    for (size_t fi = 0; fi < sf.size(); ++fi) {
        if (sf[fi] > sf[best_i]) best_i = fi;
    }
    if (fine.empty() || best_i == 0 || best_i == fine.size() - 1 || sf[best_i] < 0.12) return false;
    out = fine[best_i];
    return true;
}

double integrate_step(const std::vector<long long>& ys, const std::vector<double>& pos, size_t n_perp, size_t n_scan, double s0) {
    struct T {
        double y0, y1, x0, x1;
    };
    std::vector<T> tiles;
    const std::pair<size_t, size_t> tilings[3] = {{3, 3}, {5, 5}, {1, 8}};
    for (const auto& tl : tilings) {
        const double dy = (double)n_perp / (double)tl.first, dx = (double)n_scan / (double)tl.second;
        for (size_t i = 0; i < tl.first; ++i) {
            const double y0 = (double)i * dy, y1 = i + 1 == tl.first ? (double)n_perp : (double)(i + 1) * dy;
            for (size_t j = 0; j < tl.second; ++j) {
                const double x0 = (double)j * dx, x1 = j + 1 == tl.second ? (double)n_scan : (double)(j + 1) * dx;
                tiles.push_back({y0, y1, x0, x1});
            }
        }
    }
    std::vector<double> peak(tiles.size(), 0.0);
    std::vector<char> ok(tiles.size(), 0);
    ParFor(tiles.size(), [&](size_t t) {
        std::vector<long long> tys;
        std::vector<double> tpos;
        for (size_t k = 0; k < pos.size(); ++k) {
            const double yf = (double)ys[k];
            if (yf >= tiles[t].y0 && yf < tiles[t].y1 && pos[k] >= tiles[t].x0 && pos[k] < tiles[t].x1) {
                tys.push_back(ys[k]);
                tpos.push_back(pos[k]);
            }
        }
        ok[t] = tile_peak(lag_diffs(tys, tpos, MAX_LAG), s0, peak[t]) ? 1 : 0;
    });
    double sum = 0;
    size_t n = 0;
    for (size_t t = 0; t < tiles.size(); ++t) {
        if (ok[t]) {
            sum += 1.0 / peak[t];
            ++n;
        }
    }
    if (n == 0) return s0;
    return 1.0 / (sum / (double)n);
}

struct RlAxis {
    bool has = false;
    double s = 0, v = 0;
    std::vector<float> runs;
    std::vector<long long> ys;
    std::vector<double> pos;
};

RlAxis axis_pass(const std::vector<float>& img4, size_t rows, size_t cols, const std::vector<double>& s_grid) {
    RlAxis a;
    boundaries(img4, rows, cols, a.ys, a.pos);
    a.runs = lag_diffs(a.ys, a.pos, MAX_LAG);
    const PickResult p = pick_step(a.runs, s_grid);
    a.v = p.v;
    if (p.has) {
        a.has = true;
        a.s = refine_rl(a.runs, p.s);
    }
    return a;
}
} // namespace

RlDetection rl_detect(const uint8_t* rgba, size_t w, size_t h) {
    const std::vector<uint8_t> pu = prep_u8(rgba, w, h);
    std::vector<float> img4(pu.begin(), pu.end());
    const std::vector<double> s_grid = arange(RL_S_MIN, RL_S_MAX, 0.01);
    const std::vector<float> img4t = transpose4(img4, w, h);
    auto fy = std::async(std::launch::async, [&]() { return axis_pass(img4t, w, h, s_grid); });
    const RlAxis ax = axis_pass(img4, h, w, s_grid);
    const RlAxis ay = fy.get();
    RlDetection d;
    d.score_x = ax.v;
    d.score_y = ay.v;
    if (!ax.has && !ay.has) {
        d.step_x = d.step_y = 8.0;
        d.cols = (long long)rte((double)w / 8.0);
        d.rows = (long long)rte((double)h / 8.0);
        return d;
    }
    bool hx = ax.has, hy = ay.has;
    double sx = ax.s, sy = ay.s;
    const double vx = ax.v, vy = ay.v;
    // cross-axis reconciliation: a weak axis borrows the strong axis's step
    const bool borrow_x = (!hx && hy) || (hx && hy && vx < 0.5 * vy && std::fabs(sx - sy) > 0.15 * sy);
    if (borrow_x) {
        const double sx2 = !ax.runs.empty() ? refine_rl(ax.runs, sy) : sy;
        if (std::fabs(sx2 - sy) < 0.15 * sy) {
            sx = sx2;
            hx = true;
        }
    }
    const bool borrow_y = (!hy && hx) || (hy && hx && vy < 0.5 * vx && std::fabs(sy - sx) > 0.15 * sx);
    if (borrow_y) {
        const double sy2 = !ay.runs.empty() ? refine_rl(ay.runs, sx) : sx;
        if (std::fabs(sy2 - sx) < 0.15 * sx) {
            sy = sy2;
            hy = true;
        }
    }
    if (!hx) sx = sy;
    if (!hy) sy = sx;
    auto fiy = std::async(std::launch::async, [&]() { return integrate_step(ay.ys, ay.pos, w, h, sy); });
    sx = integrate_step(ax.ys, ax.pos, h, w, sx);
    sy = fiy.get();
    const double rel = std::fabs(sx - sy) / (0.5 * (sx + sy));
    if (rel < 0.085 || (rel < 0.15 && std::max(vx, vy) < 0.15)) {
        const double hm = 2.0 / (1.0 / sx + 1.0 / sy);
        sx = sy = hm;
    }
    d.step_x = sx;
    d.step_y = sy;
    d.cols = (long long)rte((double)w / sx);
    d.rows = (long long)rte((double)h / sy);
    return d;
}

// ================================================================ selfsim.rs
namespace {
constexpr size_t TMAX = 72, KCAP = 24, PIX_BUDGET = 1200000;
constexpr double PMIN = 1.7, PMAX = 24.0, PSTEP = 0.02, TILE_TARGET = 144.0, QUALIFY_FRAC = 0.40, QUALIFY_ABS = 4.0, VOTE_WIN = 0.15, VOTE_MIN = 1.5,
                 DISP_UNIFORM = 0.015;

struct Plane {
    std::vector<float> data;
    size_t w = 0, h = 0;
    float at(size_t y, size_t x) const { return data[y * w + x]; }
};

size_t refl101(long long i, long long n) {
    for (;;) {
        if (i < 0) i = -i;
        else if (i >= n) i = 2 * n - 2 - i;
        else return (size_t)i;
    }
}

Plane gaussian_blur_1(const Plane& src) {
    const size_t w = src.w, h = src.h;
    double k64[9], sum = 0;
    for (int i = 0; i < 9; ++i) {
        const double x = i - 4.0;
        k64[i] = std::exp(-0.5 * x * x);
        sum += k64[i];
    }
    float k[9];
    for (int i = 0; i < 9; ++i) k[i] = (float)(k64[i] / sum);
    std::vector<float> tmp(w * h), out(w * h);
    ParFor(h, [&](size_t y) {
        for (size_t x = 0; x < w; ++x) {
            float acc = 0;
            for (int t = 0; t < 9; ++t) acc += k[t] * src.at(y, refl101((long long)x + t - 4, (long long)w));
            tmp[y * w + x] = acc;
        }
    });
    ParFor(h, [&](size_t y) {
        for (size_t x = 0; x < w; ++x) {
            float acc = 0;
            for (int t = 0; t < 9; ++t) acc += k[t] * tmp[refl101((long long)y + t - 4, (long long)h) * w + x];
            out[y * w + x] = acc;
        }
    });
    return {out, w, h};
}

Plane abs_laplacian(const Plane& s) {
    const size_t w = s.w, h = s.h;
    std::vector<float> out(w * h);
    ParFor(h, [&](size_t y) {
        for (size_t x = 0; x < w; ++x) {
            const float up = s.at(refl101((long long)y - 1, (long long)h), x), dn = s.at(refl101((long long)y + 1, (long long)h), x);
            const float lf = s.at(y, refl101((long long)x - 1, (long long)w)), rt = s.at(y, refl101((long long)x + 1, (long long)w));
            out[y * w + x] = std::fabs(up + dn + lf + rt - 4.0f * s.at(y, x));
        }
    });
    return {out, w, h};
}

Plane gradmag(const Plane& y) {
    std::vector<float> out(y.w * y.h);
    for (size_t r = 0; r < y.h; ++r) {
        for (size_t c = 0; c < y.w; ++c) {
            const float gx = c == 0 ? 0.0f : std::fabs(y.at(r, c) - y.at(r, c - 1));
            const float gy = r == 0 ? 0.0f : std::fabs(y.at(r, c) - y.at(r - 1, c));
            out[r * y.w + c] = gx + gy;
        }
    }
    return {out, y.w, y.h};
}

Plane transpose_plane(const Plane& p) {
    Plane t;
    t.w = p.h;
    t.h = p.w;
    t.data.resize(p.w * p.h);
    for (size_t y = 0; y < p.h; ++y) {
        for (size_t x = 0; x < p.w; ++x) t.data[x * p.h + y] = p.data[y * p.w + x];
    }
    return t;
}

std::vector<size_t> tile_starts(size_t n, size_t parts) {
    const double step = (double)n / (double)parts;
    std::vector<size_t> v;
    for (size_t i = 0; i < parts; ++i) {
        const size_t s = (size_t)((double)i * step);
        if (v.empty() || v.back() != s) v.push_back(s);
    }
    return v;
}

using Grid3 = std::vector<std::vector<std::vector<double>>>;

void dcurves_tiled(const Plane& arr0, const Plane& wt0, size_t tmax, size_t ny_in, size_t nx_in, Grid3& num, Grid3& den) {
    const size_t full_h = arr0.h, w = arr0.w;
    const size_t stride = std::max<size_t>(1, (full_h * w + PIX_BUDGET - 1) / PIX_BUDGET);
    std::vector<size_t> rows_idx;
    for (size_t y = 0; y < full_h; y += stride) rows_idx.push_back(y);
    const size_t h = rows_idx.size();
    const size_t t_cap = std::max<size_t>(std::min(tmax, w / 3), 1);
    const std::vector<size_t> rs = tile_starts(h, ny_in), cs0 = tile_starts(w, nx_in);
    const size_t ny = rs.size(), nx = cs0.size();
    num.assign(ny, std::vector<std::vector<double>>(nx, std::vector<double>(t_cap, 0.0)));
    den = num;
    struct R {
        std::vector<std::vector<double>> nu, de;
        bool degenerate = false;
    };
    std::vector<R> res(t_cap);
    ParFor(t_cap, [&](size_t ti) {
        const size_t t = ti + 1;
        const size_t clamp_at = w >= t + 1 ? w - t - 1 : 0;
        std::vector<size_t> cs(cs0.size());
        for (size_t i = 0; i < cs0.size(); ++i) cs[i] = std::min(cs0[i], clamp_at);
        std::vector<size_t> cs_u = cs;
        cs_u.erase(std::unique(cs_u.begin(), cs_u.end()), cs_u.end());
        const bool degenerate = cs_u.size() != nx;
        const std::vector<size_t> use_cs = degenerate ? std::vector<size_t>{0} : cs;
        const size_t ncols = use_cs.size(), ext = w - t;
        R r;
        r.degenerate = degenerate;
        r.nu.assign(ny, std::vector<double>(ncols, 0.0));
        r.de = r.nu;
        std::vector<double> coln(ext), cold(ext);
        for (size_t ri = 0; ri < ny; ++ri) {
            const size_t r0 = rs[ri], r1 = ri + 1 < ny ? rs[ri + 1] : h;
            std::fill(coln.begin(), coln.end(), 0.0);
            std::fill(cold.begin(), cold.end(), 0.0);
            for (size_t rr = r0; rr < r1; ++rr) {
                const size_t y = rows_idx[rr];
                const float* arow = &arr0.data[y * w];
                const float* wrow = &wt0.data[y * w];
                for (size_t x = 0; x < ext; ++x) {
                    const float diff = std::fabs(arow[x + t] - arow[x]);
                    const float wp = wrow[x + t] * wrow[x];
                    coln[x] += (double)(diff * wp);
                    cold[x] += (double)wp;
                }
            }
            for (size_t ci = 0; ci < ncols; ++ci) {
                const size_t c0 = use_cs[ci], c1 = ci + 1 < ncols ? use_cs[ci + 1] : ext;
                double anum = 0, aden = 0;
                for (size_t x = c0; x < std::max(c1, c0); ++x) {
                    anum += f32r(coln[x]);
                    aden += f32r(cold[x]);
                }
                r.nu[ri][ci] = f32r(anum);
                r.de[ri][ci] = f32r(aden);
            }
        }
        res[ti] = std::move(r);
    });
    for (size_t ti = 0; ti < t_cap; ++ti) {
        const R& r = res[ti];
        if (r.degenerate && nx > 1) {
            double nsum = 0, dsum = 0;
            for (const auto& row : r.nu) {
                for (double v : row) nsum += v;
            }
            for (const auto& row : r.de) {
                for (double v : row) dsum += v;
            }
            for (size_t a = 0; a < ny; ++a) {
                for (size_t b = 0; b < nx; ++b) {
                    num[a][b][ti] += nsum / (double)(ny * nx);
                    den[a][b][ti] += dsum / (double)(ny * nx);
                }
            }
        } else {
            for (size_t a = 0; a < ny; ++a) {
                for (size_t b = 0; b < std::min(nx, r.nu[a].size()); ++b) {
                    num[a][b][ti] = r.nu[a][b];
                    den[a][b][ti] = r.de[a][b];
                }
            }
        }
    }
}

std::vector<std::vector<double>> group(const Grid3& num, const Grid3& den, size_t gy, size_t gx) {
    const size_t ny = num.size(), nx = num[0].size(), t_len = num[0][0].size();
    std::vector<size_t> ys(gy + 1), xs(gx + 1);
    for (size_t i = 0; i <= gy; ++i) ys[i] = (size_t)((double)ny * (double)i / (double)gy);
    for (size_t i = 0; i <= gx; ++i) xs[i] = (size_t)((double)nx * (double)i / (double)gx);
    std::vector<std::vector<double>> out;
    for (size_t a = 0; a < gy; ++a) {
        for (size_t b = 0; b < gx; ++b) {
            std::vector<double> n(t_len, 0.0), d(t_len, 0.0);
            for (size_t yy = ys[a]; yy < ys[a + 1]; ++yy) {
                for (size_t xx = xs[b]; xx < xs[b + 1]; ++xx) {
                    for (size_t t = 0; t < t_len; ++t) {
                        n[t] += num[yy][xx][t];
                        d[t] += den[yy][xx][t];
                    }
                }
            }
            for (size_t t = 0; t < t_len; ++t) n[t] /= std::max(d[t], 1e-9);
            out.push_back(n);
        }
    }
    return out;
}

double interp1(double xs_start, const std::vector<double>& dm, double q) {
    const size_t t_len = dm.size();
    const double pos = q - xs_start;
    if (pos <= 0.0) return dm[0];
    if (pos >= (double)(t_len - 1)) return dm[t_len - 1];
    const size_t i = (size_t)std::floor(pos);
    const double f = pos - (double)i;
    return dm[i] * (1.0 - f) + dm[i + 1] * f;
}

std::vector<double> comb_tstat(const std::vector<double>& dm, const std::vector<double>& p_grid, size_t kcap) {
    const size_t t_len = dm.size();
    std::vector<double> s_out(p_grid.size(), -1e9);
    if (t_len < 8) return s_out;
    std::vector<double> cum(t_len, 0.0);
    for (size_t i = 1; i < t_len; ++i) cum[i] = cum[i - 1] + 0.5 * (dm[i] + dm[i - 1]);
    std::vector<double> d2;
    for (size_t i = 2; i < t_len; ++i) d2.push_back(std::fabs(dm[i] - 2.0 * dm[i - 1] + dm[i - 2]));
    const double noise = d2.empty() ? 1e-9 : median(d2) + 1e-9;
    auto boxm = [&](double q, double p) {
        const double lo = std::max(q - p / 2.0, 1.0), hi = std::min(q + p / 2.0, (double)t_len);
        return (interp1(1.0, cum, hi) - interp1(1.0, cum, lo)) / std::max(hi - lo, 1e-9);
    };
    for (size_t pi = 0; pi < p_grid.size(); ++pi) {
        const double p = p_grid[pi];
        const long long kf = (long long)std::floor((double)t_len / p - 0.5);
        const size_t k = std::min((size_t)std::max(kf, 0LL), kcap);
        if (k < 2) continue;
        std::vector<double> c(k);
        for (size_t kk = 1; kk <= k; ++kk) {
            const double q = p * (double)kk;
            const double rm = interp1(1.0, dm, q) - boxm(q, p);
            const double rh1 = interp1(1.0, dm, q - p / 2.0) - boxm(q - p / 2.0, p);
            const double rh2 = interp1(1.0, dm, q + p / 2.0) - boxm(q + p / 2.0, p);
            c[kk - 1] = 0.5 * (rh1 + rh2) - rm;
        }
        double mean_c = 0;
        for (double v : c) mean_c += v;
        mean_c /= (double)k;
        double var = 0;
        for (double v : c) var += (v - mean_c) * (v - mean_c);
        var /= (double)k;
        const double kpen = std::min(1.0, ((double)k - 1.0) / 3.0);
        s_out[pi] = kpen * mean_c * std::sqrt((double)k) / (std::sqrt(var) + 0.5 * noise + 1e-9);
    }
    return s_out;
}

std::vector<size_t> local_maxima(const std::vector<double>& z) {
    std::vector<size_t> idx;
    for (size_t i = 1; i + 1 < z.size(); ++i) {
        if (z[i] >= z[i - 1] && z[i] > z[i + 1]) idx.push_back(i);
    }
    std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return z[a] > z[b]; });
    return idx;
}

double parabolic(double x0, double dx, const std::vector<double>& y, size_t i) {
    if (i > 0 && i + 1 < y.size()) {
        const double denom = y[i - 1] - 2.0 * y[i] + y[i + 1];
        if (std::fabs(denom) > 1e-12) return x0 + (double)i * dx + std::clamp(0.5 * (y[i - 1] - y[i + 1]) / denom, -1.0, 1.0) * dx;
    }
    return x0 + (double)i * dx;
}

double refine_step_ss(const std::vector<double>& r, double s0) {
    const size_t t_len = r.size();
    const size_t kmax = std::min(KCAP, to_usize(((double)t_len - 1.0) / s0));
    struct Pr {
        double k, t, w;
    };
    std::vector<Pr> pairs;
    for (size_t k = 1; k <= kmax; ++k) {
        const double t0 = (double)k * s0, half = std::max(1.5, 0.3 * s0);
        const size_t lo = (size_t)std::max((long long)std::floor(t0 - half), 1LL);
        const size_t hi = std::min(to_usize(std::ceil(t0 + half)), t_len);
        if (hi < lo + 2) continue;
        const std::vector<double> seg(r.begin() + (lo - 1), r.begin() + hi);
        size_t j = 0;
        for (size_t jj = 0; jj < seg.size(); ++jj) {
            if (seg[jj] < seg[j]) j = jj;
        }
        pairs.push_back({(double)k, parabolic((double)lo, 1.0, seg, j), std::pow(0.85, (int)k - 1)});
    }
    if (pairs.empty()) return s0;
    double s = s0;
    for (int it = 0; it < 2; ++it) {
        double num = 0, den = 0;
        for (const Pr& p : pairs) {
            num += p.w * p.k * p.t;
            den += p.w * p.k * p.k;
        }
        s = num / den;
        const double tol = std::max(0.2 * s0, 1.0);
        std::vector<Pr> keep;
        for (const Pr& p : pairs) {
            if (std::fabs(p.t - p.k * s) <= tol) keep.push_back(p);
        }
        if (keep.size() == pairs.size() || keep.empty()) break;
        pairs = keep;
    }
    return 0.6 * s0 < s && s < 1.4 * s0 ? s : s0;
}

std::vector<double> residual_curve(const std::vector<double>& d) {
    const size_t n = d.size();
    double mean = 0;
    for (double v : d) mean += v;
    mean /= (double)std::max<size_t>(n, 1);
    if (mean < 1e-9) return std::vector<double>(n, 0.0);
    std::vector<double> dm(n), out(n);
    for (size_t i = 0; i < n; ++i) dm[i] = d[i] / mean;
    for (size_t i = 0; i < n; ++i) {
        double acc = 0;
        for (long long k = -4; k <= 4; ++k) acc += dm[(size_t)std::clamp((long long)i + k, 0LL, (long long)n - 1)];
        out[i] = dm[i] - acc / 9.0;
    }
    return out;
}

std::vector<std::vector<double>> score_curves(const std::vector<std::vector<double>>& curves, const std::vector<double>& p_grid, double fw, size_t kcap) {
    std::vector<std::vector<double>> out(curves.size());
    ParFor(curves.size(), [&](size_t ci) {
        const std::vector<double>& d = curves[ci];
        double m = 0;
        for (double v : d) m += v;
        m /= (double)std::max<size_t>(d.size(), 1);
        if (m > 1e-9) {
            std::vector<double> dn(d.size());
            for (size_t i = 0; i < d.size(); ++i) dn[i] = d[i] / m;
            std::vector<double> ct = comb_tstat(dn, p_grid, kcap);
            for (double& v : ct) v = fw * std::clamp(v, -10.0, 30.0);
            out[ci] = ct;
        } else {
            out[ci].assign(p_grid.size(), 0.0);
        }
    });
    return out;
}

struct SsAxis {
    double s = 8, conf = 0;
};

SsAxis detect_axis(const Plane* feats[3], const Plane& wt, size_t size) {
    const std::vector<double> p_grid = arange(PMIN, std::min(PMAX, (double)size / 4.0), PSTEP);
    const size_t np_ = p_grid.size();
    if (np_ < 3) return {};
    const size_t other = wt.h, shift = wt.w;
    const size_t ny = (size_t)std::clamp(rte((double)other / 128.0), 1.0, 8.0);
    const size_t nx = (size_t)std::clamp(rte((double)shift / TILE_TARGET), 1.0, 6.0);
    const size_t gy = std::min<size_t>(ny, 4);
    std::vector<std::vector<double>> s_fine, s_coarse;
    std::vector<double> s_glob(np_, 0.0), agg_res, w_fine, w_coarse;
    bool first = true;
    for (int fi = 0; fi < 3; ++fi) {
        Grid3 num, den;
        dcurves_tiled(*feats[fi], wt, TMAX, ny, nx, num, den);
        const auto d_fine = group(num, den, num.size(), num[0].size());
        const auto d_coarse = group(num, den, gy, 1);
        const auto d_glob = group(num, den, 1, 1);
        if (first) {
            s_fine.assign(d_fine.size(), std::vector<double>(np_, 0.0));
            s_coarse.assign(d_coarse.size(), std::vector<double>(np_, 0.0));
            agg_res.assign(d_glob[0].size(), 0.0);
            first = false;
        }
        const auto sf = score_curves(d_fine, p_grid, 1.0, 6);
        for (size_t i = 0; i < d_fine.size(); ++i) {
            for (size_t j = 0; j < np_; ++j) s_fine[i][j] += sf[i][j];
        }
        const auto sc = score_curves(d_coarse, p_grid, 1.0, KCAP);
        for (size_t i = 0; i < d_coarse.size(); ++i) {
            for (size_t j = 0; j < np_; ++j) s_coarse[i][j] += sc[i][j];
        }
        const auto sg = score_curves(d_glob, p_grid, 1.0, KCAP);
        for (size_t j = 0; j < np_; ++j) s_glob[j] += sg[0][j];
        const std::vector<double> res = residual_curve(d_glob[0]);
        for (size_t j = 0; j < agg_res.size(); ++j) agg_res[j] += res[j];
        if (fi == 0) {
            for (const auto& c : d_fine) {
                double s = 0;
                for (double v : c) s += v;
                w_fine.push_back(s / (double)std::max<size_t>(c.size(), 1));
            }
            for (const auto& c : d_coarse) {
                double s = 0;
                for (double v : c) s += v;
                w_coarse.push_back(s / (double)std::max<size_t>(c.size(), 1));
            }
        }
    }
    double wc_sum = 0;
    for (double v : w_coarse) wc_sum += v;
    if (s_fine.empty() || wc_sum <= 1e-9) return {};
    std::vector<double> stot(np_);
    for (size_t j = 0; j < np_; ++j) {
        double acc = 0;
        for (size_t i = 0; i < s_coarse.size(); ++i) acc += (w_coarse[i] / wc_sum) * s_coarse[i][j];
        stot[j] = (acc + 0.5 * s_glob[j]) / 1.5;
    }
    const std::vector<size_t> peaks = local_maxima(stot);
    if (peaks.empty() || stot[peaks[0]] <= 0.0) return {};
    const double smax = stot[peaks[0]];
    const double floor_ = std::max(QUALIFY_FRAC * smax, QUALIFY_ABS);
    size_t best_i = peaks[0];
    bool anyQ = false;
    for (size_t i : peaks) {
        if (stot[i] >= floor_ && (!anyQ || p_grid[i] < p_grid[best_i])) {
            best_i = i;
            anyQ = true;
        }
    }
    // explicit divisor walk
    for (bool changed = true; changed;) {
        changed = false;
        for (double m : {6.0, 5.0, 4.0, 3.0, 2.0}) {
            const double pf = p_grid[best_i] / m;
            if (pf < p_grid[0]) continue;
            const long long j0 = (long long)rte((pf - p_grid[0]) / PSTEP);
            const long long rad = std::max((long long)rte(0.05 * pf / PSTEP), 4LL);
            const size_t lo = (size_t)std::max(j0 - rad, 0LL);
            const size_t hi = std::min((size_t)std::max(j0 + rad + 1, 0LL), np_);
            if (hi > lo) {
                size_t j = lo;
                for (size_t jj = lo; jj < hi; ++jj) {
                    if (stot[jj] > stot[j]) j = jj;
                }
                if (stot[j] >= std::max(0.45 * stot[best_i], 0.8 * QUALIFY_ABS)) {
                    best_i = j;
                    changed = true;
                    break;
                }
            }
        }
    }
    const double p_star = p_grid[best_i];
    auto tile_votes = [&](double center, std::vector<double>& vs, std::vector<double>& ws) {
        vs.clear();
        ws.clear();
        const double win = std::max(VOTE_WIN * center, 6.0 * PSTEP);
        const size_t lo = (size_t)std::max((long long)rte((center - win - p_grid[0]) / PSTEP), 0LL);
        const size_t hi = (size_t)std::max(0LL, std::min((long long)rte((center + win - p_grid[0]) / PSTEP) + 1, (long long)np_));
        for (size_t i = 0; i < s_fine.size(); ++i) {
            if (w_fine[i] <= 1e-9 || hi <= lo || hi - lo < 3) continue;
            const std::vector<double> seg(s_fine[i].begin() + lo, s_fine[i].begin() + hi);
            size_t j = 0;
            for (size_t jj = 0; jj < seg.size(); ++jj) {
                if (seg[jj] > seg[j]) j = jj;
            }
            if (seg[j] < VOTE_MIN) continue;
            vs.push_back(parabolic(p_grid[lo], PSTEP, seg, j));
            ws.push_back(std::min(seg[j], 8.0));
        }
    };
    auto wmedian = [](const std::vector<double>& v, const std::vector<double>& w) {
        std::vector<size_t> order(v.size());
        for (size_t i = 0; i < v.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return v[a] < v[b]; });
        double total = 0;
        for (double x : w) total += x;
        double cw = 0;
        for (size_t o : order) {
            cw += w[o];
            if (cw >= 0.5 * total) return v[o];
        }
        return v[order.back()];
    };
    std::vector<double> votes, vw;
    tile_votes(p_star, votes, vw);
    if (!votes.empty()) tile_votes(wmedian(votes, vw), votes, vw);
    double s0;
    if (!votes.empty()) {
        const double med = wmedian(votes, vw);
        std::vector<double> devs(votes.size());
        for (size_t i = 0; i < votes.size(); ++i) devs[i] = std::fabs(votes[i] - med);
        const double disp = wmedian(devs, vw) / std::max(med, 1e-9);
        if (disp < DISP_UNIFORM) {
            s0 = med;
            const double s_ls = refine_step_ss(agg_res, s0);
            if (std::fabs(s_ls - s0) <= std::max(0.03 * s0, 0.04)) s0 = s_ls;
        } else {
            double wsum = 0, hsum = 0;
            for (size_t i = 0; i < votes.size(); ++i) {
                wsum += vw[i];
                hsum += vw[i] / votes[i];
            }
            s0 = wsum / hsum;
        }
    } else {
        s0 = parabolic(p_grid[0], PSTEP, stot, best_i);
        const double s_ls = refine_step_ss(agg_res, s0);
        if (std::fabs(s_ls - s0) <= std::max(0.03 * s0, 0.04)) s0 = s_ls;
    }
    return {s0, stot[best_i]};
}
} // namespace

SsDetection ss_detect(const uint8_t* rgba, size_t w, size_t h) {
    Plane y;
    y.w = w;
    y.h = h;
    y.data.resize(w * h);
    for (size_t i = 0; i < w * h; ++i) y.data[i] = rgba[i * 4] * 0.299f + rgba[i * 4 + 1] * 0.587f + rgba[i * 4 + 2] * 0.114f;
    const Plane yb = gaussian_blur_1(y);
    const Plane grad0 = gradmag(y), gradb = gradmag(yb), lapb = abs_laplacian(yb);
    Plane wt;
    wt.w = w;
    wt.h = h;
    wt.data.resize(w * h);
    for (size_t i = 0; i < w * h; ++i) wt.data[i] = std::max(rgba[i * 4 + 3] / 255.0f, 0.05f);
    const Plane g0t = transpose_plane(grad0), gbt = transpose_plane(gradb), lbt = transpose_plane(lapb), wtt = transpose_plane(wt);
    const Plane* fx[3] = {&grad0, &gradb, &lapb};
    const Plane* fyp[3] = {&g0t, &gbt, &lbt};
    auto fy = std::async(std::launch::async, [&]() { return detect_axis(fyp, wtt, h); });
    const SsAxis ax = detect_axis(fx, wt, w);
    const SsAxis ay = fy.get();
    SsDetection d;
    d.step_x = ax.s;
    d.step_y = ay.s;
    d.cols = std::max(1LL, (long long)rte((double)w / ax.s));
    d.rows = std::max(1LL, (long long)rte((double)h / ay.s));
    d.conf = std::min(ax.conf, ay.conf);
    return d;
}

} // namespace pf
