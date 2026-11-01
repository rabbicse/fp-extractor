// SPDX-License-Identifier: MIT
// Minutiae matcher for ANSI-378 / ISO-19794-2 templates.
//
// Algorithm (see docs/MATCHING.md for the full description):
//   1. "Compile" every template once: normalise to 500 dpi and build, for each
//      minutia, rotation/translation-invariant local structures ("edges") to its
//      k nearest neighbours: (length, angle of edge vs. minutia, neighbour angle
//      vs. minutia).
//   2. A probe is indexed once into a small quantised hash grid. Comparing it to
//      a gallery template is then a linear scan of the candidate's edges with
//      O(1) look-ups -> list of local correspondences.
//   3. Correspondences vote for a global rotation; the best supported minutia
//      pairs are used as alignment seeds; each seed aligns the candidate onto
//      the probe and minutiae are paired greedily within distance/angle
//      tolerances (tolerance grows with distance to the seed to absorb skin
//      distortion). The best alignment is refined by least squares.
//   4. Only pairs whose local structures connect to other pairs count
//      (graph consistency). Score = 100 * m^2 / (n_probe_overlap *
//      n_candidate_overlap), floors applied, in [0, 100].
//
// Everything is integer / small-float arithmetic over fixed-size stack buffers:
// no allocation per comparison, so it is thread-safe (≈ 20 µs per comparison
// per core on a 4-vCPU cloud VM, see `fpx bench`).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "fpx/template.hpp"

namespace fpx {

struct MatcherConfig {
    int neighbors = 6;            ///< k nearest neighbours per minutia (local structures)
    int max_minutiae = 128;       ///< keep at most this many (highest quality) minutiae
    int edge_dist_tol = 6;        ///< px @500dpi, absolute edge-length tolerance
    float edge_dist_tol_rel = 0.05f;  ///< + relative tolerance (fraction of length)
    int edge_angle_tol = 10;      ///< 1/256 turn (~14°) for edge angles
    int pair_dist_tol = 14;       ///< px @500dpi, global pairing radius near the seed
    float pair_dist_growth = 0.06f;   ///< radius grows by this fraction of distance to the seed
    int pair_angle_tol = 18;      ///< 1/256 turn (~25°)
    int max_seeds = 6;            ///< alignment hypotheses tried per comparison
    int min_correspondences = 3;  ///< early exit below this many local matches
    int min_overlap_minutiae = 20;///< normalisation floor (prevents tiny-overlap inflation)
    int min_pair_support = 3;     ///< a paired minutia counts only if >= this many local structures agree
};

/// Decision thresholds on the 0..100 score, calibrated on synthetic data
/// (see docs/MATCHING.md#calibration). ALWAYS recalibrate on data from your
/// own scanners with `fpx eval <dir>` before going live.
///  * 1:1 verification: impostor scores observed <= 4 (image level).
///  * 1:N identification against ~1M templates: the best of a million
///    impostor scores must stay below the threshold; observed max 12.2
///    over 5M template-level impostor comparisons, weakest genuine 24.5.
constexpr float kVerifyThreshold = 12.0f;
constexpr float kIdentifyThreshold = 18.0f;
constexpr float kDefaultThreshold = kVerifyThreshold;

#pragma pack(push, 1)
struct CMinutia {        // 6 bytes
    int16_t x, y;        ///< 500 dpi pixels
    uint8_t angle;       ///< 1/256 turn, counter-clockwise
    uint8_t type;
};
struct CEdge {           // 5 bytes: local structure from minutia i to neighbour j
    uint8_t d;           ///< length in 2-px units (max 510 px)
    uint8_t a;           ///< direction of i->j relative to angle(i)
    uint8_t b;           ///< angle(j) relative to angle(i)
    uint8_t i, j;        ///< minutia indices
};
#pragma pack(pop)

/// Non-owning view on a compiled template (gallery entries live in an arena).
struct TemplateView {
    const CMinutia* minutiae = nullptr;
    const CEdge* edges = nullptr;
    uint16_t n_minutiae = 0;
    uint16_t n_edges = 0;
};

struct CompiledTemplate {
    std::vector<CMinutia> minutiae;
    std::vector<CEdge> edges;
    TemplateView view() const {
        return {minutiae.data(), edges.data(), static_cast<uint16_t>(minutiae.size()),
                static_cast<uint16_t>(edges.size())};
    }
};

/// Override fields from "key=value,key=value" (keys = MatcherConfig field names).
/// Throws fpx::Error on unknown keys. Used by `fpx ... --matcher "..."`.
MatcherConfig parse_matcher_config(const std::string& spec, MatcherConfig base = {});
std::string to_string(const MatcherConfig& cfg);

CompiledTemplate compile(const FingerTemplate& t, const MatcherConfig& cfg = {});

/// A probe pre-indexed for fast repeated comparisons (1:N). Build once per search.
class Probe {
public:
    Probe(const CompiledTemplate& t, const MatcherConfig& cfg = {});
    Probe(const FingerTemplate& t, const MatcherConfig& cfg = {});

    const CompiledTemplate& tmpl() const { return t_; }

    // Internal: quantised edge index.
    static constexpr int kDBins = 64, kABins = 16, kBBins = 16;
    static constexpr int kBuckets = kDBins * kABins * kBBins;
    static int bucket(const CEdge& e) {
        return ((e.d >> 2) * kABins + (e.a >> 4)) * kBBins + (e.b >> 4);
    }
    const uint32_t* offsets() const { return offsets_.data(); }
    const CEdge* items() const { return items_.data(); }

private:
    void build(const MatcherConfig& cfg);
    CompiledTemplate t_;
    std::vector<uint32_t> offsets_;  // kBuckets + 1
    std::vector<CEdge> items_;       // copies of probe edges, grouped by bucket
};

struct MatchDetails {
    int correspondences = 0;   ///< local structure matches found
    int matched = 0;           ///< paired minutiae that are also supported by local structures (used in score)
    int paired = 0;            ///< minutiae paired after global alignment (before support filter)
    int probe_overlap = 0;
    int candidate_overlap = 0;
    float rotation_deg = 0;    ///< candidate -> probe
    float dx = 0, dy = 0;
    std::vector<std::pair<int, int>> pairs;  ///< (probe idx, candidate idx), only if requested
};

class Matcher {
public:
    explicit Matcher(MatcherConfig cfg = {}) : cfg_(cfg) {}
    const MatcherConfig& config() const { return cfg_; }

    /// Score in [0, 100]. Thread-safe, allocation-free unless `details->pairs` wanted.
    float match(const Probe& probe, const TemplateView& candidate,
                MatchDetails* details = nullptr, bool want_pairs = false) const;

    /// Convenience 1:1 comparison.
    float match(const FingerTemplate& a, const FingerTemplate& b, MatchDetails* details = nullptr) const;

private:
    MatcherConfig cfg_;
};

}  // namespace fpx
