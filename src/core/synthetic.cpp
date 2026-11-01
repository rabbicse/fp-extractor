// SPDX-License-Identifier: MIT
#include "fpx/synthetic.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace fpx {
namespace {

constexpr double kPi = 3.14159265358979323846;

// Portable deterministic RNG (std distributions differ between STL vendors).
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull) {}
    uint64_t next() {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double uniform() { return double(next() >> 11) * (1.0 / 9007199254740992.0); }
    double uniform(double a, double b) { return a + (b - a) * uniform(); }
    int range(int a, int b) { return a + int(next() % uint64_t(b - a + 1)); }
    double normal() {
        double u1 = std::max(1e-12, uniform()), u2 = uniform();
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(2 * kPi * u2);
    }
};

uint64_t mix(uint64_t a, uint64_t b) { return Rng(a ^ (b * 0xD1B54A32D192ED03ull)).next(); }

// Orientation field (ridge direction, radians, y-down image coordinates)
// following the zero-pole model of Sherlock & Monro.
struct OrientationModel {
    std::vector<std::pair<double, double>> cores, deltas;
    double base = 0;
    bool arch = false;
    double cx = 0, cy = 0, w = 1, h = 1;

    OrientationModel(Rng& r, double width, double height) : cx(width / 2), cy(height / 2), w(width), h(height) {
        double kind = r.uniform();
        if (kind < 0.55) {  // loop
            double x = cx + r.uniform(-0.12, 0.12) * width, y = cy + r.uniform(-0.15, 0.05) * height;
            cores.push_back({x, y});
            double side = r.uniform() < 0.5 ? -1 : 1;
            deltas.push_back({x + side * r.uniform(0.25, 0.4) * width, y + r.uniform(0.25, 0.4) * height});
        } else if (kind < 0.85) {  // whorl
            double x = cx + r.uniform(-0.1, 0.1) * width, y = cy + r.uniform(-0.1, 0.05) * height;
            double dy = r.uniform(0.03, 0.08) * height;
            cores.push_back({x - r.uniform(0, 0.04) * width, y - dy});
            cores.push_back({x + r.uniform(0, 0.04) * width, y + dy});
            deltas.push_back({x - r.uniform(0.3, 0.42) * width, y + r.uniform(0.3, 0.42) * height});
            deltas.push_back({x + r.uniform(0.3, 0.42) * width, y + r.uniform(0.3, 0.42) * height});
        } else {
            arch = true;
        }
        base = r.uniform(-0.15, 0.15);
    }

    double operator()(double x, double y) const {
        if (arch) {
            double t = std::atan(1.8 * (x - cx) / w) * std::max(0.0, 1.0 - (y - cy + h * 0.4) / h);
            return t + base;
        }
        double a = 0;
        for (auto& c : cores) a += std::atan2(y - c.second, x - c.first);
        for (auto& d : deltas) a -= std::atan2(y - d.second, x - d.first);
        return 0.5 * a + base;
    }
};

// Grow a ridge pattern with iterated oriented Gabor filtering.
std::vector<float> master_pattern(uint32_t finger, int W, int H, double& period_out) {
    Rng r(mix(finger, 0xF1A9E7));
    OrientationModel om(r, W, H);
    const double period = r.uniform(8.5, 10.5);
    period_out = period;
    constexpr int kOri = 24, K = 6, KS = 2 * K + 1;
    const double sigma = 3.2, f = 1.0 / period;
    std::vector<float> bank(size_t(kOri) * KS * KS);
    for (int o = 0; o < kOri; ++o) {
        double th = o * kPi / kOri;  // ridge direction
        double nx = -std::sin(th), ny = std::cos(th);  // across ridges
        double sum = 0;
        float* k = &bank[size_t(o) * KS * KS];
        for (int y = -K; y <= K; ++y)
            for (int x = -K; x <= K; ++x) {
                double xr = x * nx + y * ny;
                double g = std::exp(-(x * x + y * y) / (2 * sigma * sigma)) * std::cos(2 * kPi * f * xr);
                k[(y + K) * KS + x + K] = float(g);
                sum += g;
            }
        double mean = sum / (KS * KS);  // zero-mean filter
        for (int i = 0; i < KS * KS; ++i) k[i] -= float(mean);
    }
    std::vector<uint8_t> ori(size_t(W) * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            double t = std::fmod(om(x, y), kPi);
            if (t < 0) t += kPi;
            ori[size_t(y) * W + x] = uint8_t(int(std::lround(t / kPi * kOri)) % kOri);
        }
    std::vector<float> img(size_t(W) * H, 0.f), out(img.size());
    for (auto& v : img) v = r.uniform() < 0.04 ? float(r.uniform(-1, 1)) : 0.f;
    for (int it = 0; it < 14; ++it) {
        double ss = 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const float* k = &bank[size_t(ori[size_t(y) * W + x]) * KS * KS];
                float acc = 0;
                for (int j = -K; j <= K; ++j) {
                    int yy = std::clamp(y + j, 0, H - 1);
                    const float* row = &img[size_t(yy) * W];
                    const float* kr = k + (j + K) * KS + K;
                    if (x >= K && x < W - K) {
                        for (int i = -K; i <= K; ++i) acc += kr[i] * row[x + i];
                    } else {
                        for (int i = -K; i <= K; ++i) acc += kr[i] * row[std::clamp(x + i, 0, W - 1)];
                    }
                }
                out[size_t(y) * W + x] = acc;
                ss += double(acc) * acc;
            }
        const float norm = float(1.0 / std::sqrt(ss / double(out.size()) + 1e-12));
        for (size_t i = 0; i < img.size(); ++i) img[i] = std::clamp(out[i] * norm * 1.6f, -1.f, 1.f);
    }
    return img;
}

float bilinear(const std::vector<float>& m, int W, int H, double x, double y) {
    if (x < 0 || y < 0 || x > W - 2 || y > H - 2) return 1.f;  // outside master: "valley"
    int x0 = int(x), y0 = int(y);
    float fx = float(x - x0), fy = float(y - y0);
    const float* p = &m[size_t(y0) * W + x0];
    return (p[0] * (1 - fx) + p[1] * fx) * (1 - fy) + (p[W] * (1 - fx) + p[W + 1] * fx) * fy;
}

}  // namespace

GrayImage synthesize_image(uint32_t finger_id, uint32_t impression, const SynthImageOptions& opt0) {
    SynthImageOptions opt = opt0;
    Rng ir(mix(mix(finger_id, impression), 0x1A9));
    double contact = 1.0;  // fraction of the finger touching the sensor
    if (opt.random_placement && impression > 0) {
        opt.rotation_deg = float(ir.uniform(-25, 25));
        opt.dx = ir.range(-40, 40);
        opt.dy = ir.range(-40, 40);
        opt.distortion = float(ir.uniform(0.2, 1.0));
        opt.noise = float(ir.uniform(0.04, 0.2));
        contact = ir.uniform(0.75, 1.0);
    }
    // Master pattern in 500 dpi units, larger than the sensor.
    const double scale = 500.0 / opt.dpi;  // sensor px -> master px
    const int MW = int(opt.width * scale) + 140, MH = int(opt.height * scale) + 160;
    double period;
    const std::vector<float> master = master_pattern(finger_id, MW, MH, period);
    Rng fr(mix(finger_id, 0xE11));
    const double ex = MW * fr.uniform(0.30, 0.36), ey = MH * fr.uniform(0.36, 0.42);  // finger ellipse radii

    GrayImage img(opt.width, opt.height, opt.dpi, 255);
    const double th = opt.rotation_deg * kPi / 180.0, c = std::cos(th), s = std::sin(th);
    const double ph1 = ir.uniform(0, 2 * kPi), ph2 = ir.uniform(0, 2 * kPi);
    const double amp = opt.distortion * 9.0;
    for (int v = 0; v < opt.height; ++v)
        for (int u = 0; u < opt.width; ++u) {
            // sensor -> master coordinates (rotation about the sensor centre)
            double x = (u - opt.width / 2.0) * scale - opt.dx, y = (v - opt.height / 2.0) * scale - opt.dy;
            double mx = c * x - s * y, my = s * x + c * y;
            mx += amp * std::sin(my / 90.0 + ph1);
            my += amp * std::sin(mx / 110.0 + ph2);
            mx += MW / 2.0;
            my += MH / 2.0;
            double ux = (mx - MW / 2.0) / (ex * contact), uy = (my - MH / 2.0) / (ey * contact);
            double rr = ux * ux + uy * uy;
            double cover = std::clamp((1.0 - rr) * 6.0, 0.0, 1.0);  // soft finger contour
            float val = bilinear(master, MW, MH, mx, my);           // -1 valley .. +1 ridge
            double ridge = 0.5 + 0.5 * val;
            double g = 235.0 - cover * ridge * 190.0 + opt.noise * 120.0 * ir.normal();
            img.at(u, v) = uint8_t(std::clamp(g, 0.0, 255.0));
        }
    return img;
}

FingerTemplate synthesize_template(uint64_t finger_id, const SynthTemplateOptions& opt) {
    Rng r(mix(finger_id, 0x7E3));
    OrientationModel om(r, opt.width, opt.height);
    FingerTemplate t;
    t.width = uint16_t(opt.width);
    t.height = uint16_t(opt.height);
    t.xres = t.yres = 197;
    t.finger_quality = 60;
    const int n = r.range(opt.min_minutiae, opt.max_minutiae);
    const double rx = opt.width * 0.45, ry = opt.height * 0.46, cx = opt.width / 2.0, cy = opt.height / 2.0;
    int guard = 0;
    while (int(t.minutiae.size()) < n && guard++ < 20000) {
        double x = r.uniform(cx - rx, cx + rx), y = r.uniform(cy - ry, cy + ry);
        if (((x - cx) / rx) * ((x - cx) / rx) + ((y - cy) / ry) * ((y - cy) / ry) > 1.0) continue;
        bool ok = true;
        for (auto& m : t.minutiae)
            if ((m.x - x) * (m.x - x) + (m.y - y) * (m.y - y) < 14 * 14) {
                ok = false;
                break;
            }
        if (!ok) continue;
        double ridge = om(x, y);  // y-down radians, ridge orientation (mod pi)
        // Convert to CCW angle with y up, choose a direction along the ridge.
        double ang = -ridge + (r.uniform() < 0.5 ? 0 : kPi);
        Minutia m;
        m.x = uint16_t(std::lround(x));
        m.y = uint16_t(std::lround(y));
        m.angle = uint8_t(int(std::lround(ang * 128.0 / kPi)) & 255);
        m.type = r.uniform() < 0.55 ? MinutiaType::Ending : MinutiaType::Bifurcation;
        m.quality = uint8_t(r.range(40, 100));
        t.minutiae.push_back(m);
    }
    return t;
}

FingerTemplate perturb_template(const FingerTemplate& src, uint64_t seed, const PerturbOptions& opt) {
    Rng r(mix(seed, 0x5EED));
    FingerTemplate t = src;
    t.minutiae.clear();
    const double th = r.uniform(-opt.max_rotation_deg, opt.max_rotation_deg) * kPi / 180.0;
    const double c = std::cos(th), s = std::sin(th);
    const double tx = r.uniform(-opt.max_shift, opt.max_shift), ty = r.uniform(-opt.max_shift, opt.max_shift);
    const double cx = src.width / 2.0, cy = src.height / 2.0;
    const int rot_units = int(std::lround(th * 128.0 / kPi));
    for (const auto& m : src.minutiae) {
        if (r.uniform() < opt.drop_rate) continue;
        double x = m.x - cx, y = m.y - cy;
        // CCW rotation by th in y-down coordinates.
        double nx = c * x + s * y + cx + tx + opt.position_jitter * r.normal();
        double ny = -s * x + c * y + cy + ty + opt.position_jitter * r.normal();
        if (nx < 0 || ny < 0 || nx >= src.width || ny >= src.height) continue;  // fell off the sensor
        Minutia q = m;
        q.x = uint16_t(std::lround(nx));
        q.y = uint16_t(std::lround(ny));
        q.angle = uint8_t((m.angle + rot_units + int(std::lround(opt.angle_jitter * r.normal()))) & 255);
        t.minutiae.push_back(q);
    }
    const int spurious = int(std::lround(src.minutiae.size() * opt.spurious_rate));
    for (int k = 0; k < spurious; ++k) {
        Minutia q;
        q.x = uint16_t(r.range(10, src.width - 10));
        q.y = uint16_t(r.range(10, src.height - 10));
        q.angle = uint8_t(r.range(0, 255));
        q.type = MinutiaType::Ending;
        q.quality = uint8_t(r.range(20, 60));
        t.minutiae.push_back(q);
    }
    // Shuffle so order carries no information.
    for (size_t i = t.minutiae.size(); i > 1; --i) std::swap(t.minutiae[i - 1], t.minutiae[r.next() % i]);
    return t;
}

}  // namespace fpx
