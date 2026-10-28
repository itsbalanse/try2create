#pragma once
// Internals of the Pixel Art Fixer port (pixel_fixer.h). Each block mirrors one module of the reference Rust core; comments name it.
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace pf {

using cd = std::complex<double>;
constexpr double kPi = 3.14159265358979323846;

// ---- numeric helpers matching Rust semantics
inline double rte(double v) { return std::nearbyint(v); } // round half to even (the default rounding mode)
inline size_t to_usize(double v) { return v <= 0 || std::isnan(v) ? 0 : (size_t)v; }
inline long long to_i64(double v) { return std::isnan(v) ? 0 : (long long)v; } // truncation toward zero
inline double rem_euclid(double a, double b) {
    double r = std::fmod(a, b);
    return r < 0 ? r + std::fabs(b) : r;
}
inline double f32r(double v) { return (double)(float)v; } // a value rounded through float32, as the reference does
inline double vmax(const std::vector<double>& v, double init = -std::numeric_limits<double>::max()) {
    double m = init;
    for (double x : v) m = std::max(m, x);
    return m;
}
std::vector<double> arange(double start, double stop, double step);

// Parallel loop over [0, n): f(i) per index, results kept per index so sums stay in reference order.
void ParFor(size_t n, const std::function<void(size_t)>& f);

// ---- FFT (rustfft semantics: unnormalised forward and inverse)
void Fft(std::vector<cd>& a, bool inverse);

// ---- sigproc.rs
double percentile(const std::vector<double>& values, double q);
std::vector<double> normalise(const std::vector<double>& profile);
std::vector<double> gaussian_filter1d(const std::vector<double>& x, double sigma);
void find_peaks(const std::vector<double>& x, double height, double distance, std::vector<size_t>& pos, std::vector<double>& heights);
std::vector<double> median_filter1d(const std::vector<double>& x, size_t size);
double median(std::vector<double> values);
double interp(double x, const std::vector<double>& xp, const std::vector<double>& fp);

// ---- gray.rs
std::vector<float> to_gray(const uint8_t* rgba, size_t w, size_t h);
std::vector<float> median_quant(const std::vector<float>& g, size_t w, size_t h);
std::vector<float> d1x(const std::vector<float>& g, size_t w, size_t h);
std::vector<float> d2x(const std::vector<float>& g, size_t w, size_t h);
std::vector<float> transpose(const std::vector<float>& a, size_t w, size_t h);

// ---- acf.rs
constexpr double MIN_STEP = 2.0, MAX_STEP = 24.0, STEP_GRID = 0.02;
constexpr size_t BAND = 24;
std::vector<std::vector<double>> band_profiles(const std::vector<float>& feat, size_t lines, size_t extent, size_t band);
std::vector<double> band_acf(const std::vector<std::vector<double>>& prof);
std::vector<double> band_cepstrum(const std::vector<std::vector<double>>& prof);
double interp_at(const std::vector<double>& arr, double pos);
double comb_score(const std::vector<double>& ac, double s, size_t k_max, double k0);
double plain_comb(const std::vector<double>& ac, double s, size_t k_max, double k0);
double train_quality(const std::vector<double>& ac, double s0, size_t k_max);
double refine_step_acf(const std::vector<double>& ac, double s0, size_t k_max);
double ceps_score(const std::vector<double>& c, double s);

// ---- kmeans.rs
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(std::max<uint64_t>(seed, 1)) {}
    uint64_t next_u64() {
        uint64_t x = s;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        s = x;
        return x * 0x2545F4914F6CDD1DULL;
    }
    double next_f64() { return (double)(next_u64() >> 11) / (double)(1ULL << 53); }
    size_t below(size_t n) { return (size_t)(next_f64() * (double)n) % std::max<size_t>(n, 1); }
};
using P3 = std::array<float, 3>;
std::vector<size_t> even_sample(size_t n, size_t max_n);
std::vector<P3> kmeans(const std::vector<P3>& points, size_t k, size_t max_iter, double eps, size_t attempts, uint64_t seed);
std::vector<uint8_t> kmeans_quantize_rgba(const uint8_t* rgba, size_t w, size_t h, size_t k);
size_t adaptive_k(const uint8_t* rgba, size_t w, size_t h, size_t lo, size_t hi, double share);
void kmeans_labels(const uint8_t* rgba, size_t w, size_t h, size_t k, std::vector<uint32_t>& labels, size_t& kc);

// ---- autocorr.rs
struct FeatMap {
    std::vector<float> data;
    size_t lines = 0, extent = 0;
    double weight = 1;
};
struct AxisEstimate {
    double step = 8;
    std::vector<std::pair<double, double>> cands;
    std::vector<double> acf;
};
struct AcPre {
    std::vector<FeatMap> maps_x, maps_y;
    AxisEstimate ex, ey;
};
struct AcDetection {
    double step_x = 8, step_y = 8;
    size_t cols = 1, rows = 1;
};
AcPre ac_prepare(const uint8_t* rgba, size_t w, size_t h);
AcDetection ac_detect_pre(const AcPre& pre, size_t w, size_t h);
double local_count(const std::vector<FeatMap>& maps, size_t extent, double s0);

// ---- runlengths.rs
std::vector<uint8_t> prep_u8(const uint8_t* rgba, size_t w, size_t h);
struct RlDetection {
    double step_x = 8, step_y = 8;
    long long cols = 1, rows = 1;
    double score_x = 0, score_y = 0;
};
RlDetection rl_detect(const uint8_t* rgba, size_t w, size_t h);

// ---- selfsim.rs
struct SsDetection {
    double step_x = 8, step_y = 8;
    long long cols = 1, rows = 1;
    double conf = 0;
};
SsDetection ss_detect(const uint8_t* rgba, size_t w, size_t h);

} // namespace pf
