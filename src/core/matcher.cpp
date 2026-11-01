// SPDX-License-Identifier: MIT
#include "fpx/matcher.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numeric>
#include <string>

#include "fpx/image.hpp"  // fpx::Error

namespace fpx {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kMaxCorr = 8192;  // dense templates (100+ minutiae) need several thousand
constexpr int kMaxPairs = 4096;
constexpr int kRotWindow = 14;  // 1/256 turn: correspondences consistent with the consensus rotation

inline int angdiff(int a, int b) {  // circular distance in 1/256 turn
    int d = (a - b) & 255;
    return d > 128 ? 256 - d : d;
}

// Direction of the vector (dx, dy) in image coordinates (y down) expressed as
// a counter-clockwise angle in 1/256 turn, matching ISO 19794-2 minutia angles.
inline uint8_t vec_angle(double dx, double dy) {
    double a = std::atan2(-dy, dx);  // y up
    int u = int(std::lround(a * 128.0 / kPi));
    return uint8_t(u & 255);
}

struct Tables {
    float cos_t[256], sin_t[256];
    Tables() {
        for (int i = 0; i < 256; ++i) {
            cos_t[i] = float(std::cos(i * kPi / 128.0));
            sin_t[i] = float(std::sin(i * kPi / 128.0));
        }
    }
};
const Tables g_tables;
inline const Tables& tables() { return g_tables; }

struct Corr {
    uint8_t pi, ci, pj, cj;
    uint8_t rot;  // candidate -> probe
};

struct Transform {
    // Maps a candidate point into the probe frame:
    //   x' = c*x + s*y + tx,  y' = -s*x + c*y + ty     (rotation CCW by `rot` in y-down coords)
    float c = 1, s = 0, tx = 0, ty = 0;
    int rot = 0;  // 1/256 turn, added to candidate angles
};

}  // namespace

// ---------------------------------------------------------------------------
// Config parsing
// ---------------------------------------------------------------------------
#define FPX_CFG_FIELDS(X)                                                                     \
    X(neighbors) X(max_minutiae) X(edge_dist_tol) X(edge_dist_tol_rel) X(edge_angle_tol)      \
    X(pair_dist_tol) X(pair_dist_growth) X(pair_angle_tol) X(max_seeds) X(min_correspondences) \
    X(min_overlap_minutiae) X(min_pair_support)

MatcherConfig parse_matcher_config(const std::string& spec, MatcherConfig c) {
    size_t pos = 0;
    while (pos < spec.size()) {
        size_t end = spec.find(',', pos);
        if (end == std::string::npos) end = spec.size();
        std::string kv = spec.substr(pos, end - pos);
        pos = end + 1;
        if (kv.empty()) continue;
        auto eq = kv.find('=');
        if (eq == std::string::npos) throw Error("matcher config: expected key=value, got '" + kv + "'");
        std::string k = kv.substr(0, eq);
        double v = std::stod(kv.substr(eq + 1));
        bool found = false;
#define X(name)                                       \
    if (k == #name) {                                 \
        c.name = static_cast<decltype(c.name)>(v);    \
        found = true;                                 \
    }
        FPX_CFG_FIELDS(X)
#undef X
        if (!found) throw Error("matcher config: unknown key '" + k + "'");
    }
    return c;
}

std::string to_string(const MatcherConfig& c) {
    std::string s;
#define X(name) s += std::string(s.empty() ? "" : ",") + #name "=" + std::to_string(c.name);
    FPX_CFG_FIELDS(X)
#undef X
    return s;
}

// ---------------------------------------------------------------------------
// Compilation
// ---------------------------------------------------------------------------
CompiledTemplate compile(const FingerTemplate& t, const MatcherConfig& cfg) {
    CompiledTemplate ct;
    const double sx = 197.0 / (t.xres ? t.xres : 197), sy = 197.0 / (t.yres ? t.yres : 197);

    std::vector<const Minutia*> src;
    src.reserve(t.minutiae.size());
    for (const auto& m : t.minutiae) src.push_back(&m);
    const size_t limit = std::min<size_t>(size_t(std::max(1, cfg.max_minutiae)), 255);
    if (src.size() > limit) {
        std::stable_sort(src.begin(), src.end(), [](const Minutia* a, const Minutia* b) { return a->quality > b->quality; });
        src.resize(limit);
    }
    ct.minutiae.reserve(src.size());
    for (const Minutia* m : src) {
        CMinutia c;
        c.x = int16_t(std::lround(m->x * sx));
        c.y = int16_t(std::lround(m->y * sy));
        c.angle = m->angle;
        c.type = uint8_t(m->type);
        ct.minutiae.push_back(c);
    }

    const int n = int(ct.minutiae.size());
    const int k = std::max(1, std::min(cfg.neighbors, n - 1));
    ct.edges.reserve(size_t(n) * k);
    std::vector<std::pair<int, int>> dist(n);
    for (int i = 0; i < n; ++i) {
        const CMinutia& a = ct.minutiae[i];
        int cnt = 0;
        for (int j = 0; j < n; ++j) {
            if (j == i) continue;
            int dx = ct.minutiae[j].x - a.x, dy = ct.minutiae[j].y - a.y;
            dist[cnt++] = {dx * dx + dy * dy, j};
        }
        int kk = std::min(k, cnt);
        std::partial_sort(dist.begin(), dist.begin() + kk, dist.begin() + cnt);
        for (int q = 0; q < kk; ++q) {
            const int j = dist[q].second;
            const CMinutia& b = ct.minutiae[j];
            const double d = std::sqrt(double(dist[q].first));
            if (d < 4.0 || d > 510.0) continue;
            CEdge e;
            e.d = uint8_t(std::min(255L, std::lround(d / 2.0)));
            e.a = uint8_t((vec_angle(b.x - a.x, b.y - a.y) - a.angle) & 255);
            e.b = uint8_t((b.angle - a.angle) & 255);
            e.i = uint8_t(i);
            e.j = uint8_t(j);
            ct.edges.push_back(e);
        }
    }
    return ct;
}

// ---------------------------------------------------------------------------
// Probe index
// ---------------------------------------------------------------------------
Probe::Probe(const CompiledTemplate& t, const MatcherConfig& cfg) : t_(t) { build(cfg); }
Probe::Probe(const FingerTemplate& t, const MatcherConfig& cfg) : t_(compile(t, cfg)) { build(cfg); }

void Probe::build(const MatcherConfig& cfg) {
    offsets_.assign(kBuckets + 1, 0);
    const int at = cfg.edge_angle_tol;
    // Each probe edge is registered in every bucket its tolerance box overlaps,
    // so a candidate edge needs a single bucket look-up.
    auto for_each_bucket = [&](const CEdge& e, auto&& fn) {
        const double tol_px = cfg.edge_dist_tol + cfg.edge_dist_tol_rel * e.d * 2.0;
        const int dt = int(std::ceil(tol_px / 2.0));
        const int d0 = std::max(0, e.d - dt) >> 2, d1 = std::min(255, e.d + dt) >> 2;
        const int a0 = e.a - at, b0 = e.b - at;
        const int na = ((at * 2) >> 4) + 2, nb = na;
        for (int db = d0; db <= d1; ++db) {
            int last_ab = -1;
            for (int ia = 0; ia < na; ++ia) {
                int av = std::min(a0 + ia * 16, e.a + at);
                int ab = (av & 255) >> 4;
                if (ab == last_ab) continue;
                last_ab = ab;
                int last_bb = -1;
                for (int ib = 0; ib < nb; ++ib) {
                    int bv = std::min(b0 + ib * 16, e.b + at);
                    int bb = (bv & 255) >> 4;
                    if (bb == last_bb) continue;
                    last_bb = bb;
                    fn((db * kABins + ab) * kBBins + bb);
                }
            }
        }
    };
    for (const CEdge& e : t_.edges) for_each_bucket(e, [&](int b) { ++offsets_[b + 1]; });
    for (int b = 0; b < kBuckets; ++b) offsets_[b + 1] += offsets_[b];
    items_.assign(offsets_[kBuckets], CEdge{});
    std::vector<uint32_t> fill(offsets_.begin(), offsets_.end() - 1);
    for (size_t idx = 0; idx < t_.edges.size(); ++idx)
        for_each_bucket(t_.edges[idx], [&](int b) { items_[fill[b]++] = t_.edges[idx]; });
}

// ---------------------------------------------------------------------------
// Matching
// ---------------------------------------------------------------------------
namespace {

struct PairCand {
    float cost;
    uint8_t p, c;
};

// Pair minutiae under transform T. Returns number of pairs; fills `pp`/`pc`.
int pair_minutiae(const CompiledTemplate& P, const TemplateView& C, const Transform& T, float seed_x, float seed_y,
                  const MatcherConfig& cfg, uint8_t* pp, uint8_t* pc, float tol_scale) {
    const int nP = int(P.minutiae.size()), nC = C.n_minutiae;
    float cx[256], cy[256];
    uint8_t ca[256];
    for (int j = 0; j < nC; ++j) {
        const CMinutia& m = C.minutiae[j];
        cx[j] = T.c * m.x + T.s * m.y + T.tx;
        cy[j] = -T.s * m.x + T.c * m.y + T.ty;
        ca[j] = uint8_t((m.angle + T.rot) & 255);
    }
    PairCand cand[kMaxPairs];
    int nc = 0;
    const float base = cfg.pair_dist_tol * tol_scale;
    for (int i = 0; i < nP && nc < kMaxPairs; ++i) {
        const CMinutia& p = P.minutiae[i];
        const float sdx = p.x - seed_x, sdy = p.y - seed_y;
        const float tol = base + cfg.pair_dist_growth * std::sqrt(sdx * sdx + sdy * sdy);
        const float tol2 = tol * tol;
        for (int j = 0; j < nC; ++j) {
            const float dx = cx[j] - p.x, dy = cy[j] - p.y;
            const float d2 = dx * dx + dy * dy;
            if (d2 > tol2) continue;
            const int da = angdiff(p.angle, ca[j]);
            if (da > cfg.pair_angle_tol) continue;
            cand[nc++] = {d2 / tol2 + float(da) / cfg.pair_angle_tol, uint8_t(i), uint8_t(j)};
            if (nc == kMaxPairs) break;
        }
    }
    std::sort(cand, cand + nc, [](const PairCand& a, const PairCand& b) { return a.cost < b.cost; });
    uint8_t usedP[256] = {0}, usedC[256] = {0};
    int m = 0;
    for (int k = 0; k < nc; ++k) {
        if (usedP[cand[k].p] || usedC[cand[k].c]) continue;
        usedP[cand[k].p] = usedC[cand[k].c] = 1;
        pp[m] = cand[k].p;
        pc[m] = cand[k].c;
        ++m;
    }
    return m;
}

// Cheap upper-bound proxy for pair_minutiae(): number of probe minutiae that
// have at least one compatible candidate minutia. No sorting, no assignment.
int count_compatible(const CompiledTemplate& P, const TemplateView& C, const Transform& T, float seed_x, float seed_y,
                     const MatcherConfig& cfg) {
    const int nP = int(P.minutiae.size()), nC = C.n_minutiae;
    float cx[256], cy[256];
    uint8_t ca[256];
    for (int j = 0; j < nC; ++j) {
        const CMinutia& m = C.minutiae[j];
        cx[j] = T.c * m.x + T.s * m.y + T.tx;
        cy[j] = -T.s * m.x + T.c * m.y + T.ty;
        ca[j] = uint8_t((m.angle + T.rot) & 255);
    }
    int count = 0;
    for (int i = 0; i < nP; ++i) {
        const CMinutia& p = P.minutiae[i];
        const float sdx = p.x - seed_x, sdy = p.y - seed_y;
        const float tol = cfg.pair_dist_tol + cfg.pair_dist_growth * std::sqrt(sdx * sdx + sdy * sdy);
        const float tol2 = tol * tol;
        for (int j = 0; j < nC; ++j) {
            const float dx = cx[j] - p.x, dy = cy[j] - p.y;
            if (dx * dx + dy * dy <= tol2 && angdiff(p.angle, ca[j]) <= cfg.pair_angle_tol) {
                ++count;
                break;
            }
        }
    }
    return count;
}

Transform transform_from_seed(const CMinutia& p, const CMinutia& c, int rot) {
    const Tables& tb = tables();
    Transform T;
    T.rot = rot & 255;
    T.c = tb.cos_t[T.rot];
    T.s = tb.sin_t[T.rot];
    T.tx = p.x - (T.c * c.x + T.s * c.y);
    T.ty = p.y - (-T.s * c.x + T.c * c.y);
    return T;
}

// Least-squares rigid transform from matched pairs.
Transform fit_transform(const CompiledTemplate& P, const TemplateView& C, const uint8_t* pp, const uint8_t* pc, int m) {
    double mpx = 0, mpy = 0, mcx = 0, mcy = 0;
    for (int k = 0; k < m; ++k) {
        mpx += P.minutiae[pp[k]].x;
        mpy += P.minutiae[pp[k]].y;
        mcx += C.minutiae[pc[k]].x;
        mcy += C.minutiae[pc[k]].y;
    }
    mpx /= m;
    mpy /= m;
    mcx /= m;
    mcy /= m;
    // x' = c*x + s*y ; y' = -s*x + c*y  ->  maximise sum p·(R c)
    double sc = 0, ss = 0;
    for (int k = 0; k < m; ++k) {
        double px = P.minutiae[pp[k]].x - mpx, py = P.minutiae[pp[k]].y - mpy;
        double cx = C.minutiae[pc[k]].x - mcx, cy = C.minutiae[pc[k]].y - mcy;
        sc += px * cx + py * cy;
        ss += px * cy - py * cx;
    }
    double th = std::atan2(ss, sc);
    Transform T;
    T.c = float(std::cos(th));
    T.s = float(std::sin(th));
    T.rot = int(std::lround(th * 128.0 / kPi)) & 255;
    T.tx = float(mpx - (T.c * mcx + T.s * mcy));
    T.ty = float(mpy - (-T.s * mcx + T.c * mcy));
    return T;
}

void bbox(const CMinutia* m, int n, float& x0, float& y0, float& x1, float& y1) {
    x0 = y0 = 1e9f;
    x1 = y1 = -1e9f;
    for (int i = 0; i < n; ++i) {
        x0 = std::min(x0, float(m[i].x));
        y0 = std::min(y0, float(m[i].y));
        x1 = std::max(x1, float(m[i].x));
        y1 = std::max(y1, float(m[i].y));
    }
}

}  // namespace

float Matcher::match(const Probe& probe, const TemplateView& C, MatchDetails* det, bool want_pairs) const {
    const CompiledTemplate& P = probe.tmpl();
    const int nP = int(P.minutiae.size()), nC = C.n_minutiae;
    if (det) *det = MatchDetails{};
    if (nP < 3 || nC < 3) return 0.f;

    // 1. Local structure correspondences via the probe's bucket index.
    Corr corr[kMaxCorr];
    int ncorr = 0;
    const uint32_t* off = probe.offsets();
    const CEdge* items = probe.items();
    const int at = cfg_.edge_angle_tol;
    const float dtol_abs = cfg_.edge_dist_tol * 0.5f, dtol_rel = cfg_.edge_dist_tol_rel;
    for (int k = 0; k < C.n_edges && ncorr < kMaxCorr; ++k) {
        const CEdge& ce = C.edges[k];
        const int b = Probe::bucket(ce);
        for (uint32_t q = off[b], qe = off[b + 1]; q < qe; ++q) {
            const CEdge& pe = items[q];
            const int dd = std::abs(int(pe.d) - int(ce.d));
            if (float(dd) > dtol_abs + dtol_rel * pe.d) continue;
            if (angdiff(pe.a, ce.a) > at || angdiff(pe.b, ce.b) > at) continue;
            corr[ncorr++] = {pe.i, ce.i, pe.j, ce.j,
                             uint8_t((P.minutiae[pe.i].angle - C.minutiae[ce.i].angle) & 255)};
            if (ncorr == kMaxCorr) break;
        }
    }
    if (det) det->correspondences = ncorr;
    if (ncorr < cfg_.min_correspondences) return 0.f;

    // 2. Global rotation consensus (32 bins of 11.25°, smoothed).
    int hist[32] = {0};
    for (int k = 0; k < ncorr; ++k) ++hist[corr[k].rot >> 3];
    int best_bin = 0, best_val = -1;
    for (int b = 0; b < 32; ++b) {
        int v = 2 * hist[b] + hist[(b + 31) & 31] + hist[(b + 1) & 31];
        if (v > best_val) {
            best_val = v;
            best_bin = b;
        }
    }
    const int rot_center = best_bin * 8 + 4;
    // Circular mean rotation of consistent correspondences.
    double sx = 0, sy = 0;
    int nconsistent = 0;
    uint16_t keys[2 * kMaxCorr];
    int nkeys = 0;
    const Tables& tb = tables();
    for (int k = 0; k < ncorr; ++k) {
        if (angdiff(corr[k].rot, rot_center) > kRotWindow) continue;
        sx += tb.cos_t[corr[k].rot];
        sy += tb.sin_t[corr[k].rot];
        ++nconsistent;
        keys[nkeys++] = uint16_t((corr[k].pi << 8) | corr[k].ci);
        keys[nkeys++] = uint16_t((corr[k].pj << 8) | corr[k].cj);
    }
    if (nconsistent < cfg_.min_correspondences) return 0.f;
    const int rot_mean = int(std::lround(std::atan2(sy, sx) * 128.0 / kPi)) & 255;

    // 3. Seeds = minutia pairs with most consistent local support.
    std::sort(keys, keys + nkeys);
    struct Seed {
        int support;
        uint16_t key;
    };
    std::array<Seed, 16> seeds{};
    int nseeds = 0, strong_keys = 0;
    const int max_seeds = std::min<int>(cfg_.max_seeds, int(seeds.size()));
    for (int k = 0; k < nkeys;) {
        int e = k;
        while (e < nkeys && keys[e] == keys[k]) ++e;
        Seed s{e - k, keys[k]};
        if (s.support >= cfg_.min_pair_support) ++strong_keys;
        // insertion into small top-N list
        if (nseeds < max_seeds || s.support > seeds[nseeds - 1].support) {
            int pos = std::min(nseeds, max_seeds - 1);
            while (pos > 0 && seeds[pos - 1].support < s.support) {
                seeds[pos] = seeds[pos - 1];
                --pos;
            }
            seeds[pos] = s;
            if (nseeds < max_seeds) ++nseeds;
        }
        k = e;
    }

    // Exact early exit: a pair's graph support (step 5) can never exceed its
    // local support counted here, so if no seed reaches the minimum, the
    // final score is necessarily 0. Most impostors stop here.
    // Likewise, the number of minutia pairs with enough local support bounds
    // the number of scoring pairs m, and score <= 100 m^2 / floor^2.
    if (nseeds == 0 || seeds[0].support < cfg_.min_pair_support || strong_keys < 3) {
        if (det) det->correspondences = ncorr;
        return 0.f;
    }

    // 4. Rank seeds by a cheap compatibility count, then run the full greedy
    //    assignment only for the best one (sorting dominated the profile).
    uint8_t pp[256], pc[256], bpp[256], bpc[256];
    int best_m = 0, best_seed = -1, best_count = -1;
    Transform bestT;
    for (int s = 0; s < nseeds; ++s) {
        if (seeds[s].support < cfg_.min_pair_support) break;  // sorted by support
        const int pi = seeds[s].key >> 8, ci = seeds[s].key & 255;
        const CMinutia& p = P.minutiae[pi];
        Transform T = transform_from_seed(p, C.minutiae[ci], rot_mean);
        int c = count_compatible(P, C, T, p.x, p.y, cfg_);
        if (c > best_count) {
            best_count = c;
            best_seed = s;
            bestT = T;
        }
    }
    if (best_seed >= 0) {
        const CMinutia& p = P.minutiae[seeds[best_seed].key >> 8];
        best_m = pair_minutiae(P, C, bestT, p.x, p.y, cfg_, bpp, bpc, 1.0f);
    }
    if (best_m >= 3) {
        Transform T = fit_transform(P, C, bpp, bpc, best_m);
        // Centre of the matched region as the reference for distortion tolerance.
        float mx = 0, my = 0;
        for (int k = 0; k < best_m; ++k) {
            mx += P.minutiae[bpp[k]].x;
            my += P.minutiae[bpp[k]].y;
        }
        mx /= best_m;
        my /= best_m;
        int m = pair_minutiae(P, C, T, mx, my, cfg_, pp, pc, 1.0f);
        if (m > best_m) {
            best_m = m;
            bestT = T;
            std::memcpy(bpp, pp, m);
            std::memcpy(bpc, pc, m);
        }
    }

    // 5. Graph consistency: a paired minutia counts only if at least
    //    `min_pair_support` of its local structures (edges) lead to neighbours
    //    that are *also* paired with the corresponding candidate neighbour.
    //    Chance alignments of impostors pair many minutiae, but these pairs
    //    are rarely connected by matching edges - this keeps the impostor
    //    tail low enough for million-scale 1:N search.
    int16_t c_of[256];
    std::fill(c_of, c_of + 256, int16_t(-1));
    for (int k = 0; k < best_m; ++k) c_of[bpp[k]] = bpc[k];
    uint8_t sup[256] = {0};
    for (int k = 0; k < ncorr; ++k) {
        const Corr& q = corr[k];
        if (c_of[q.pi] == q.ci && c_of[q.pj] == q.cj && angdiff(q.rot, rot_center) <= kRotWindow) {
            if (sup[q.pi] < 255) ++sup[q.pi];
            if (sup[q.pj] < 255) ++sup[q.pj];
        }
    }
    int supported = 0;
    for (int k = 0; k < best_m; ++k) {
        if (sup[bpp[k]] >= cfg_.min_pair_support) {
            bpp[supported] = bpp[k];
            bpc[supported] = bpc[k];
            ++supported;
        }
    }
    const int paired_total = best_m;
    best_m = supported;

    // 6. Overlap-normalised score.
    float px0, py0, px1, py1, cx0, cy0, cx1, cy1;
    bbox(P.minutiae.data(), nP, px0, py0, px1, py1);
    bbox(C.minutiae, nC, cx0, cy0, cx1, cy1);
    const float margin = 12.f;
    int c_in = 0, p_in = 0;
    for (int j = 0; j < nC; ++j) {  // candidate minutiae inside probe bbox
        const CMinutia& m = C.minutiae[j];
        float x = bestT.c * m.x + bestT.s * m.y + bestT.tx, y = -bestT.s * m.x + bestT.c * m.y + bestT.ty;
        c_in += (x >= px0 - margin && x <= px1 + margin && y >= py0 - margin && y <= py1 + margin);
    }
    for (int i = 0; i < nP; ++i) {  // probe minutiae inside candidate bbox (inverse transform)
        float x = P.minutiae[i].x - bestT.tx, y = P.minutiae[i].y - bestT.ty;
        float u = bestT.c * x - bestT.s * y, v = bestT.s * x + bestT.c * y;
        p_in += (u >= cx0 - margin && u <= cx1 + margin && v >= cy0 - margin && v <= cy1 + margin);
    }
    const float floor_n = float(cfg_.min_overlap_minutiae);
    const float np_eff = std::max(float(std::max(p_in, best_m)), floor_n);
    const float nc_eff = std::max(float(std::max(c_in, best_m)), floor_n);
    float score = 100.f * float(best_m) * float(best_m) / (np_eff * nc_eff);
    score = std::min(score, 100.f);

    if (det) {
        det->matched = best_m;
        det->paired = paired_total;
        det->probe_overlap = p_in;
        det->candidate_overlap = c_in;
        det->rotation_deg = float(int8_t(uint8_t(bestT.rot))) * 360.f / 256.f;
        det->dx = bestT.tx;
        det->dy = bestT.ty;
        if (want_pairs)
            for (int k = 0; k < best_m; ++k) det->pairs.emplace_back(bpp[k], bpc[k]);
    }
    return score;
}

float Matcher::match(const FingerTemplate& a, const FingerTemplate& b, MatchDetails* details) const {
    Probe p(a, cfg_);
    CompiledTemplate cb = compile(b, cfg_);
    // Symmetrise: the algorithm is probe-driven; averaging both directions
    // makes 1:1 scores independent of argument order.
    float s1 = match(p, cb.view(), details, details != nullptr);
    Probe q(cb, cfg_);
    CompiledTemplate ca = p.tmpl();
    float s2 = match(q, ca.view());
    return 0.5f * (s1 + s2);
}

}  // namespace fpx
