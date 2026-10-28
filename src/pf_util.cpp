// Pixel Art Fixer port: the shared layer (sigproc.rs, gray.rs, acf.rs, kmeans.rs) plus an FFT and a parallel loop standing in for rustfft and rayon.
#include <array>
#include <atomic>

#include "pf_internal.h"

namespace pf {

std::vector<double> arange(double start, double stop, double step) {
    const size_t n = to_usize(std::max(0.0, std::ceil((stop - start) / step)));
    std::vector<double> out(n);
    for (size_t i = 0; i < n; ++i) out[i] = start + (double)i * step;
    return out;
}

void ParFor(size_t n, const std::function<void(size_t)>& f) {
    const size_t hw = std::max<size_t>(1, std::thread::hardware_concurrency());
    const size_t nt = std::min(hw, n);
    if (nt <= 1) {
        for (size_t i = 0; i < n; ++i) f(i);
        return;
    }
    std::atomic<size_t> next{0};
    std::vector<std::thread> th;
    th.reserve(nt);
    for (size_t t = 0; t < nt; ++t) {
        th.emplace_back([&]() {
            for (size_t i = next++; i < n; i = next++) f(i);
        });
    }
    for (std::thread& t : th) t.join();
}

// ---------------------------------------------------------------- FFT
namespace {
bool IsPow2(size_t n) { return n && !(n & (n - 1)); }

void FftPow2(std::vector<cd>& a, bool inverse) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = 2 * kPi / (double)len * (inverse ? 1 : -1);
        const cd wl(std::cos(ang), std::sin(ang));
        // twiddles computed directly per stage (accurate for long transforms)
        std::vector<cd> tw(len / 2);
        for (size_t k = 0; k < len / 2; ++k) tw[k] = cd(std::cos(ang * (double)k), std::sin(ang * (double)k));
        (void)wl;
        for (size_t i = 0; i < n; i += len) {
            for (size_t k = 0; k < len / 2; ++k) {
                const cd u = a[i + k], v = a[i + k + len / 2] * tw[k];
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
            }
        }
    }
}

// Bluestein's algorithm for any length, through power-of-two transforms.
void FftBluestein(std::vector<cd>& a, bool inverse) {
    const size_t n = a.size();
    size_t m = 1;
    while (m < 2 * n - 1) m <<= 1;
    const double sgn = inverse ? 1.0 : -1.0;
    std::vector<cd> w(n);
    for (size_t k = 0; k < n; ++k) {
        const unsigned long long k2 = ((unsigned long long)k * k) % (2ULL * n);
        const double ang = sgn * kPi * (double)k2 / (double)n;
        w[k] = cd(std::cos(ang), std::sin(ang));
    }
    std::vector<cd> A(m), B(m);
    for (size_t k = 0; k < n; ++k) A[k] = a[k] * w[k];
    B[0] = std::conj(w[0]);
    for (size_t k = 1; k < n; ++k) B[k] = B[m - k] = std::conj(w[k]);
    FftPow2(A, false);
    FftPow2(B, false);
    for (size_t k = 0; k < m; ++k) A[k] *= B[k];
    FftPow2(A, true);
    for (size_t k = 0; k < n; ++k) a[k] = A[k] / (double)m * w[k];
}
} // namespace

void Fft(std::vector<cd>& a, bool inverse) {
    if (a.size() <= 1) return;
    if (IsPow2(a.size())) FftPow2(a, inverse);
    else FftBluestein(a, inverse);
}

// ---------------------------------------------------------------- sigproc.rs
double percentile(const std::vector<double>& values, double q) {
    if (values.empty()) return 0.0;
    std::vector<double> v = values;
    std::sort(v.begin(), v.end());
    const double pos = q / 100.0 * ((double)v.size() - 1.0);
    const size_t lo = (size_t)std::floor(pos), hi = (size_t)std::ceil(pos);
    const double f = pos - (double)lo;
    return v[lo] + (v[hi] - v[lo]) * f;
}

std::vector<double> normalise(const std::vector<double>& profile) {
    const size_t n = profile.size();
    if (n <= 2) return profile;
    const std::vector<double> interior(profile.begin() + 1, profile.end() - 1);
    double scale = percentile(interior, 95.0);
    if (scale <= 0.0) {
        const double m = vmax(interior);
        scale = m > 0.0 ? m : 1.0;
    }
    std::vector<double> out(n);
    for (size_t i = 0; i < n; ++i) out[i] = std::clamp(profile[i] / (scale + 1e-9), 0.0, 1.5);
    return out;
}

std::vector<double> gaussian_filter1d(const std::vector<double>& x, double sigma) {
    const long long radius = (long long)(4.0 * sigma + 0.5);
    std::vector<double> kern;
    for (long long i = -radius; i <= radius; ++i) kern.push_back(std::exp(-0.5 * std::pow((double)i / sigma, 2)));
    double s = 0;
    for (double k : kern) s += k;
    for (double& k : kern) k /= s;
    const long long n = (long long)x.size();
    auto refl = [n](long long i) -> size_t {
        for (;;) {
            if (i < 0) i = -i - 1;
            else if (i >= n) i = 2 * n - 1 - i;
            else return (size_t)i;
        }
    };
    std::vector<double> out((size_t)n);
    for (long long i = 0; i < n; ++i) {
        double acc = 0;
        for (size_t j = 0; j < kern.size(); ++j) acc += kern[j] * x[refl(i + (long long)j - radius)];
        out[(size_t)i] = acc;
    }
    return out;
}

void find_peaks(const std::vector<double>& x, double height, double distance, std::vector<size_t>& outPos, std::vector<double>& outHeights) {
    const size_t n = x.size();
    std::vector<size_t> peaks;
    size_t i = 1;
    while (n >= 3 && i < n - 1) {
        if (x[i - 1] < x[i]) {
            size_t ahead = i + 1;
            while (ahead < n - 1 && x[ahead] == x[i]) ++ahead;
            if (x[ahead] < x[i]) {
                peaks.push_back((i + ahead - 1) / 2);
                i = ahead;
                continue;
            }
        }
        ++i;
    }
    std::vector<size_t> kept;
    for (size_t p : peaks) {
        if (x[p] >= height) kept.push_back(p);
    }
    if (distance > 1.0 && kept.size() > 1) {
        const size_t dmin = (size_t)std::ceil(distance);
        const size_t m = kept.size();
        std::vector<char> keep(m, 1);
        std::vector<size_t> priority(m);
        for (size_t k = 0; k < m; ++k) priority[k] = k;
        std::stable_sort(priority.begin(), priority.end(), [&](size_t a, size_t b) { return x[kept[a]] < x[kept[b]]; });
        for (size_t pi = m; pi-- > 0;) {
            const size_t j = priority[pi];
            if (!keep[j]) continue;
            long long k = (long long)j - 1;
            while (k >= 0 && kept[j] - kept[(size_t)k] < dmin) keep[(size_t)k--] = 0;
            size_t k2 = j + 1;
            while (k2 < m && kept[k2] - kept[j] < dmin) keep[k2++] = 0;
        }
        std::vector<size_t> next;
        for (size_t k = 0; k < m; ++k) {
            if (keep[k]) next.push_back(kept[k]);
        }
        kept.swap(next);
    }
    outPos = kept;
    outHeights.resize(kept.size());
    for (size_t k = 0; k < kept.size(); ++k) outHeights[k] = x[kept[k]];
}

std::vector<double> median_filter1d(const std::vector<double>& x, size_t size) {
    const size_t n = x.size();
    const long long half = (long long)size / 2;
    std::vector<double> buf(size), out(n);
    for (long long i = 0; i < (long long)n; ++i) {
        size_t bi = 0;
        for (long long k = -half; k <= half; ++k) buf[bi++] = x[(size_t)std::clamp(i + k, 0LL, (long long)n - 1)];
        std::sort(buf.begin(), buf.end());
        out[(size_t)i] = buf[size / 2];
    }
    return out;
}

double median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

double interp(double x, const std::vector<double>& xp, const std::vector<double>& fp) {
    const size_t n = xp.size();
    if (x <= xp[0]) return fp[0];
    if (x >= xp[n - 1]) return fp[n - 1];
    size_t lo = 0, hi = n - 1;
    while (hi - lo > 1) {
        const size_t mid = (lo + hi) / 2;
        if (xp[mid] <= x) lo = mid;
        else hi = mid;
    }
    const double f = (x - xp[lo]) / (xp[hi] - xp[lo]);
    return fp[lo] * (1.0 - f) + fp[hi] * f;
}

// ---------------------------------------------------------------- gray.rs
std::vector<float> to_gray(const uint8_t* rgba, size_t w, size_t h) {
    std::vector<float> g(w * h);
    for (size_t i = 0; i < w * h; ++i) {
        const size_t p = i * 4;
        const float a = rgba[p + 3] / 255.0f;
        const float lum = 0.299f * rgba[p] + 0.587f * rgba[p + 1] + 0.114f * rgba[p + 2];
        g[i] = lum * a + 127.5f * (1.0f - a);
    }
    return g;
}

std::vector<float> median_quant(const std::vector<float>& g, size_t w, size_t h) {
    std::vector<uint8_t> u(w * h);
    for (size_t i = 0; i < w * h; ++i) u[i] = (uint8_t)std::clamp(g[i], 0.0f, 255.0f);
    std::vector<float> out(w * h);
    uint8_t buf[9];
    for (size_t y = 0; y < h; ++y) {
        for (size_t x = 0; x < w; ++x) {
            int k = 0;
            for (long long dy = -1; dy <= 1; ++dy) {
                const size_t yy = (size_t)std::clamp((long long)y + dy, 0LL, (long long)h - 1);
                for (long long dx = -1; dx <= 1; ++dx) buf[k++] = u[yy * w + (size_t)std::clamp((long long)x + dx, 0LL, (long long)w - 1)];
            }
            std::sort(buf, buf + 9);
            out[y * w + x] = (float)(rte(buf[4] / 12.0f) * 12.0);
        }
    }
    return out;
}

std::vector<float> d1x(const std::vector<float>& g, size_t w, size_t h) {
    std::vector<float> out(w * h, 0.0f);
    for (size_t y = 0; y < h; ++y) {
        const size_t r = y * w;
        for (size_t x = 0; x + 1 < w; ++x) out[r + x] = std::fabs(g[r + x + 1] - g[r + x]);
    }
    return out;
}

std::vector<float> d2x(const std::vector<float>& g, size_t w, size_t h) {
    std::vector<float> out(w * h, 0.0f);
    for (size_t y = 0; y < h; ++y) {
        const size_t r = y * w;
        for (size_t x = 1; x + 1 < w; ++x) out[r + x] = std::fabs(g[r + x + 1] - 2.0f * g[r + x] + g[r + x - 1]);
    }
    return out;
}

std::vector<float> transpose(const std::vector<float>& a, size_t w, size_t h) {
    std::vector<float> out(w * h);
    for (size_t y = 0; y < h; ++y) {
        for (size_t x = 0; x < w; ++x) out[x * h + y] = a[y * w + x];
    }
    return out;
}

// ---------------------------------------------------------------- acf.rs
std::vector<std::vector<double>> band_profiles(const std::vector<float>& feat, size_t lines, size_t extent, size_t band) {
    const size_t nb = std::max<size_t>(1, lines / band);
    const size_t group = lines >= band ? band : lines;
    std::vector<std::vector<double>> out(nb, std::vector<double>(extent, 0.0));
    for (size_t b = 0; b < nb; ++b) {
        std::vector<double>& dst = out[b];
        for (size_t r = 0; r < group; ++r) {
            const size_t row = (b * group + r) * extent;
            for (size_t x = 0; x < extent; ++x) dst[x] += feat[row + x];
        }
        for (double& v : dst) v = f32r(v);
    }
    return out;
}

namespace {
size_t NextPow2(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}
} // namespace

std::vector<double> band_acf(const std::vector<std::vector<double>>& prof) {
    const size_t n = prof[0].size();
    const size_t nfft = NextPow2(2 * n), half = nfft / 2;
    std::vector<double> psum(nfft, 0.0);
    std::vector<cd> buf(nfft);
    for (const std::vector<double>& row : prof) {
        double s = 0;
        for (double v : row) s += v;
        const float mean = (float)(s / (double)n);
        std::fill(buf.begin(), buf.end(), cd(0, 0));
        for (size_t i = 0; i < n; ++i) buf[i] = cd((double)((float)row[i] - mean), 0.0);
        Fft(buf, false);
        double ssum = 0;
        for (size_t k = 0; k <= half; ++k) ssum += std::norm(buf[k]);
        ssum = std::max(ssum, 1e-12);
        for (size_t k = 0; k <= half; ++k) {
            const double p = std::norm(buf[k]) / ssum;
            psum[k] += p;
            if (k != 0 && k != half) psum[nfft - k] += p;
        }
    }
    for (size_t k = 0; k < nfft; ++k) buf[k] = cd(psum[k], 0.0);
    Fft(buf, true);
    std::vector<double> ac(n);
    for (size_t lag = 0; lag < n; ++lag) {
        const double unbias = (double)n / (double)std::max<size_t>(n - lag, 1);
        ac[lag] = buf[lag].real() / (double)nfft * unbias;
    }
    const double a0 = std::max(ac[0], 1e-12);
    for (double& v : ac) v /= a0;
    return ac;
}

std::vector<double> band_cepstrum(const std::vector<std::vector<double>>& prof) {
    const size_t n = prof[0].size();
    const size_t nfft = NextPow2(2 * n), half = nfft / 2;
    std::vector<double> pmean(half + 1, 0.0);
    std::vector<cd> buf(nfft);
    for (const std::vector<double>& row : prof) {
        double s = 0;
        for (double v : row) s += v;
        const float mean = (float)(s / (double)n);
        std::fill(buf.begin(), buf.end(), cd(0, 0));
        for (size_t i = 0; i < n; ++i) buf[i] = cd((double)((float)row[i] - mean), 0.0);
        Fft(buf, false);
        for (size_t k = 0; k <= half; ++k) pmean[k] += std::norm(buf[k]);
    }
    const double nb = (double)prof.size();
    std::vector<double> logp(half + 1);
    double lmean = 0;
    for (size_t k = 0; k <= half; ++k) {
        logp[k] = std::log(pmean[k] / nb + 1e-6);
        lmean += logp[k];
    }
    lmean /= (double)logp.size();
    for (double& v : logp) v -= lmean;
    for (size_t k = 0; k < nfft; ++k) buf[k] = cd(k <= half ? logp[k] : logp[nfft - k], 0.0);
    Fft(buf, true);
    const size_t m = n / 2;
    std::vector<double> c(m);
    for (size_t i = 0; i < m; ++i) c[i] = buf[i].real() / (double)nfft;
    const double med = median(c);
    double cm = 0;
    for (double v : c) cm += v;
    cm /= (double)std::max<size_t>(m, 1);
    double var = 0;
    for (double v : c) var += (v - cm) * (v - cm);
    var /= (double)std::max<size_t>(m, 1);
    const double sd = std::sqrt(var);
    for (double& v : c) v = (v - med) / (sd + 1e-12);
    return c;
}

double interp_at(const std::vector<double>& arr, double pos) {
    const size_t n = arr.size();
    const size_t i = (size_t)std::clamp(to_i64(pos), 0LL, (long long)n - 2);
    const double f = pos - (double)i;
    return arr[i] * (1.0 - f) + arr[i + 1] * f;
}

double comb_score(const std::vector<double>& ac, double s, size_t k_max, double k0) {
    const double n = (double)ac.size();
    const long long kf = (long long)std::floor((n - 2.0) / s);
    const size_t kcap = std::min((size_t)std::max(kf, 0LL), k_max);
    if (kcap < 2) return -1.0;
    double num = 0, den = 0;
    for (size_t k = 1; k <= kcap; ++k) {
        const double kd = (double)k;
        const double w = std::exp(-(kd - 1.0) / k0);
        const double pk = interp_at(ac, kd * s);
        const double tr = 0.5 * (interp_at(ac, (kd - 0.5) * s) + interp_at(ac, (kd + 0.5) * s));
        num += w * (pk - tr);
        den += w;
    }
    return num / den;
}

double plain_comb(const std::vector<double>& ac, double s, size_t k_max, double k0) {
    const double n = (double)ac.size();
    const long long kf = (long long)std::floor((n - 2.0) / s);
    const size_t kcap = std::min((size_t)std::max(kf, 0LL), k_max);
    if (kcap < 1) return 0.0;
    double num = 0, den = 0;
    for (size_t k = 1; k <= kcap; ++k) {
        const double kd = (double)k, w = std::exp(-(kd - 1.0) / k0);
        num += w * interp_at(ac, kd * s);
        den += w;
    }
    return num / den;
}

double train_quality(const std::vector<double>& ac, double s0, size_t k_max) {
    const size_t n = ac.size();
    const long long kf = (long long)std::floor(((double)n - 3.0) / s0);
    const size_t kcap = std::min((size_t)std::max(kf, 0LL), k_max);
    if (kcap < 2) return 0.0;
    std::vector<double> res;
    double mass = 0;
    size_t tot = 0;
    for (size_t k = 1; k <= kcap; ++k) {
        const double c0 = (double)k * s0;
        const double r = (double)std::max(to_i64(0.35 * s0), 2LL);
        const size_t lo = (size_t)std::max(to_i64(c0 - r), 1LL);
        const size_t hi = std::min(n - 2, to_usize(std::ceil(c0 + r)));
        if (hi <= lo) continue;
        ++tot;
        size_t i = lo;
        for (size_t j = lo; j <= hi; ++j) {
            if (ac[j] > ac[i]) i = j;
        }
        if (i <= lo || i >= hi || ac[i] <= 0.0) continue;
        const double d = (ac[i - 1] - ac[i + 1]) / (2.0 * (ac[i - 1] - 2.0 * ac[i] + ac[i + 1]) + 1e-12);
        const double p = (double)i + std::clamp(d, -1.0, 1.0);
        res.push_back((p - c0) / s0);
        mass += ac[i];
    }
    if (res.empty() || tot == 0) return 0.0;
    double ss = 0;
    for (double r : res) ss += r * r;
    const double rms = std::sqrt(ss / (double)res.size());
    return (mass / (double)tot) * std::exp(-4.0 * rms);
}

double refine_step_acf(const std::vector<double>& ac, double s0, size_t k_max) {
    const size_t n = ac.size();
    const long long kf = (long long)std::floor(((double)n - 3.0) / s0);
    const size_t kcap = std::min((size_t)std::max(kf, 0LL), k_max);
    if (kcap < 2) return s0;
    std::vector<double> pos, ks, wts;
    for (size_t k = 1; k <= kcap; ++k) {
        const double c0 = (double)k * s0;
        const double r = (double)std::max(to_i64(0.3 * s0), 2LL);
        const size_t lo = (size_t)std::max(to_i64(c0 - r), 1LL);
        const size_t hi = std::min(n - 2, to_usize(std::ceil(c0 + r)));
        if (hi <= lo) continue;
        size_t i = lo;
        for (size_t j = lo; j <= hi; ++j) {
            if (ac[j] > ac[i]) i = j;
        }
        if (i <= lo || i >= hi) continue;
        const double d = (ac[i - 1] - ac[i + 1]) / (2.0 * (ac[i - 1] - 2.0 * ac[i] + ac[i + 1]) + 1e-12);
        pos.push_back((double)i + std::clamp(d, -1.0, 1.0));
        ks.push_back((double)k);
        wts.push_back(std::max(ac[i], 1e-6));
    }
    if (pos.size() < 2) return s0;
    double s = s0;
    for (int it = 0; it < 3; ++it) {
        const double tol = std::max(0.3 * s0, 1.5);
        double num = 0, den = 0;
        size_t kept = 0;
        for (size_t j = 0; j < pos.size(); ++j) {
            if (std::fabs(pos[j] - ks[j] * s) <= tol) {
                num += wts[j] * pos[j] * ks[j];
                den += wts[j] * ks[j] * ks[j];
                ++kept;
            }
        }
        if (kept < 2) break;
        s = num / den;
    }
    if (!(0.8 * s0 <= s && s <= 1.25 * s0)) return s0;
    return s;
}

double ceps_score(const std::vector<double>& c, double s) {
    const double n = (double)c.size();
    const long long kf = (long long)std::floor((n - 2.0) / s);
    const size_t kcap = std::min((size_t)std::max(kf, 0LL), (size_t)6);
    if (kcap < 1) return 0.0;
    double num = 0, den = 0;
    for (size_t k = 1; k <= kcap; ++k) {
        const double kd = (double)k, w = std::exp(-(kd - 1.0) / 3.0);
        num += w * interp_at(c, kd * s);
        den += w;
    }
    return num / den;
}

// ---------------------------------------------------------------- kmeans.rs
std::vector<size_t> even_sample(size_t n, size_t max_n) {
    std::vector<size_t> out;
    if (n <= max_n) {
        out.resize(n);
        for (size_t i = 0; i < n; ++i) out[i] = i;
    } else {
        out.resize(max_n);
        for (size_t i = 0; i < max_n; ++i) out[i] = (size_t)((double)i * ((double)n - 1.0) / ((double)max_n - 1.0));
    }
    return out;
}

namespace {
double Dist2(const P3& a, const P3& b) {
    double s = 0;
    for (int c = 0; c < 3; ++c) {
        const double d = (double)(a[c] - b[c]);
        s += d * d;
    }
    return s;
}

std::vector<P3> KmeansOnce(const std::vector<P3>& points, size_t k, size_t max_iter, double eps, Rng& rng, double& inertiaOut) {
    const size_t n = points.size();
    std::vector<P3> centers;
    centers.reserve(k);
    centers.push_back(points[rng.below(n)]);
    std::vector<double> d2(n);
    for (size_t i = 0; i < n; ++i) d2[i] = Dist2(points[i], centers[0]);
    while (centers.size() < k) {
        double total = 0;
        for (double v : d2) total += v;
        size_t pick = 0;
        if (total > 0.0) {
            const double target = rng.next_f64() * total;
            double acc = 0;
            for (size_t i = 0; i < n; ++i) {
                acc += d2[i];
                if (acc >= target) {
                    pick = i;
                    break;
                }
            }
        } else {
            pick = rng.below(n);
        }
        const P3 c = points[pick];
        centers.push_back(c);
        for (size_t i = 0; i < n; ++i) d2[i] = std::min(d2[i], Dist2(points[i], c));
    }
    std::vector<uint32_t> labels(n, 0);
    for (size_t it = 0; it < max_iter; ++it) {
        for (size_t i = 0; i < n; ++i) {
            size_t best = 0;
            double bd = std::numeric_limits<double>::infinity();
            for (size_t ci = 0; ci < centers.size(); ++ci) {
                const double d = Dist2(points[i], centers[ci]);
                if (d < bd) {
                    bd = d;
                    best = ci;
                }
            }
            labels[i] = (uint32_t)best;
        }
        std::vector<std::array<double, 3>> sums(k, {0, 0, 0});
        std::vector<size_t> cnts(k, 0);
        for (size_t i = 0; i < n; ++i) {
            const size_t l = labels[i];
            ++cnts[l];
            for (int c = 0; c < 3; ++c) sums[l][c] += points[i][c];
        }
        double max_shift = 0;
        for (size_t ci = 0; ci < k; ++ci) {
            if (cnts[ci] == 0) {
                size_t far = 0;
                double fd = -1;
                for (size_t i = 0; i < n; ++i) {
                    const double d = Dist2(points[i], centers[labels[i]]);
                    if (d > fd) {
                        fd = d;
                        far = i;
                    }
                }
                centers[ci] = points[far];
                max_shift = std::numeric_limits<double>::infinity();
                continue;
            }
            P3 nc;
            for (int c = 0; c < 3; ++c) nc[c] = (float)(sums[ci][c] / (double)cnts[ci]);
            max_shift = std::max(max_shift, Dist2(nc, centers[ci]));
            centers[ci] = nc;
        }
        if (max_shift <= eps * eps) break;
    }
    double inertia = 0;
    for (size_t i = 0; i < n; ++i) inertia += Dist2(points[i], centers[labels[i]]);
    inertiaOut = inertia;
    return centers;
}
} // namespace

std::vector<P3> kmeans(const std::vector<P3>& points, size_t k, size_t max_iter, double eps, size_t attempts, uint64_t seed) {
    Rng rng(seed);
    std::vector<P3> best;
    double bestI = std::numeric_limits<double>::infinity();
    bool have = false;
    for (size_t a = 0; a < attempts; ++a) {
        double inertia = 0;
        std::vector<P3> c = KmeansOnce(points, k, max_iter, eps, rng, inertia);
        if (!have || inertia < bestI) {
            best = std::move(c);
            bestI = inertia;
            have = true;
        }
    }
    return best;
}

std::vector<uint8_t> kmeans_quantize_rgba(const uint8_t* rgba, size_t w, size_t h, size_t k) {
    const size_t n = w * h;
    const std::vector<size_t> idx = even_sample(n, 60000);
    std::vector<P3> sample(idx.size());
    std::vector<std::array<uint8_t, 3>> uniq(idx.size());
    for (size_t s = 0; s < idx.size(); ++s) {
        const size_t p = idx[s] * 4;
        sample[s] = {(float)rgba[p], (float)rgba[p + 1], (float)rgba[p + 2]};
        uniq[s] = {rgba[p], rgba[p + 1], rgba[p + 2]};
    }
    std::sort(uniq.begin(), uniq.end());
    uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
    const size_t k_eff = std::min(k, uniq.size());
    std::vector<uint8_t> out(rgba, rgba + n * 4);
    if (k_eff <= 1) return out;
    const std::vector<P3> centers = kmeans(sample, k_eff, 12, 0.5, 2, 42);
    for (size_t i = 0; i < n; ++i) {
        const size_t p = i * 4;
        const P3 px = {(float)rgba[p], (float)rgba[p + 1], (float)rgba[p + 2]};
        size_t best = 0;
        double bd = std::numeric_limits<double>::infinity();
        for (size_t ci = 0; ci < centers.size(); ++ci) {
            const double d = Dist2(px, centers[ci]);
            if (d < bd) {
                bd = d;
                best = ci;
            }
        }
        for (int c = 0; c < 3; ++c) out[p + c] = (uint8_t)std::clamp(rte(centers[best][c]), 0.0, 255.0);
    }
    return out;
}

size_t adaptive_k(const uint8_t* rgba, size_t w, size_t h, size_t lo, size_t hi, double share) {
    std::vector<uint32_t> cnt(4096, 0);
    uint64_t total = 0;
    for (size_t i = 0; i < w * h; ++i) {
        if (rgba[i * 4 + 3] > 0) {
            const size_t key = ((size_t)(rgba[i * 4] >> 4) << 8) | ((size_t)(rgba[i * 4 + 1] >> 4) << 4) | (size_t)(rgba[i * 4 + 2] >> 4);
            ++cnt[key];
            ++total;
        }
    }
    if (total == 0) return lo;
    size_t k = 0;
    for (uint32_t c : cnt) {
        if ((double)c / (double)total >= share) ++k;
    }
    return std::clamp(k, lo, hi);
}

void kmeans_labels(const uint8_t* rgba, size_t w, size_t h, size_t k, std::vector<uint32_t>& labels, size_t& kc) {
    const size_t n = w * h;
    std::vector<size_t> src;
    for (size_t i = 0; i < n; ++i) {
        if (rgba[i * 4 + 3] > 0) src.push_back(i);
    }
    if (src.empty()) {
        src.resize(n);
        for (size_t i = 0; i < n; ++i) src[i] = i;
    }
    const std::vector<size_t> idx = even_sample(src.size(), 60000);
    std::vector<P3> sample(idx.size());
    for (size_t s = 0; s < idx.size(); ++s) {
        const size_t i = src[idx[s]];
        sample[s] = {(float)rgba[i * 4], (float)rgba[i * 4 + 1], (float)rgba[i * 4 + 2]};
    }
    const size_t k_eff = std::max<size_t>(1, std::min(k, sample.size()));
    const std::vector<P3> centers = kmeans(sample, k_eff, 15, 0.5, 1, 42);
    kc = centers.size();
    labels.assign(n, 0);
    ParFor((n + 65535) / 65536, [&](size_t chunk) {
        const size_t a = chunk * 65536, b = std::min(n, a + 65536);
        for (size_t i = a; i < b; ++i) {
            const float p0 = rgba[i * 4], p1 = rgba[i * 4 + 1], p2 = rgba[i * 4 + 2];
            uint32_t best = 0;
            float bd = std::numeric_limits<float>::infinity();
            for (size_t ci = 0; ci < centers.size(); ++ci) {
                const float d = (p0 - centers[ci][0]) * (p0 - centers[ci][0]) + (p1 - centers[ci][1]) * (p1 - centers[ci][1]) + (p2 - centers[ci][2]) * (p2 - centers[ci][2]);
                if (d < bd) {
                    bd = d;
                    best = (uint32_t)ci;
                }
            }
            labels[i] = best;
        }
    });
}

} // namespace pf
