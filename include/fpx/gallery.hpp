// SPDX-License-Identifier: MIT
// In-memory 1:N identification index with multi-finger subjects.
//
// A *subject* (customer) owns up to 11 finger slots: positions 1..10 per
// ISO/IEC 19794-2 (1 = right thumb, 2 = right index, ... 6 = left thumb,
// 7 = left index, ...) and slot 0 for "position unknown".
//
// Templates are compiled once and packed into contiguous arenas (≈ 6 B per
// minutia + 5 B per local structure, ~1.5 KB for a 40-minutiae template), so
// a full scan is a streaming, cache-friendly pass split across all cores.
//
// Multi-finger search (`identify_subjects`) scores every probe finger in a
// single pass, compares a probe finger only with gallery fingers of the same
// position (when both positions are known), and fuses per subject. Requiring
// two independent fingers to agree drives false matches at 10^6 subjects
// down by orders of magnitude compared with one finger.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "fpx/matcher.hpp"
#include "fpx/template.hpp"

namespace fpx {

constexpr int kMaxFingerPosition = 10;  ///< ISO 19794-2 finger positions 1..10 (0 = unknown)

// ---------------------------------------------------------------------------
// Single-template search (one probe template)
// ---------------------------------------------------------------------------
struct Candidate {
    std::string id;       ///< subject id
    float score = 0;
    uint8_t finger = 0;   ///< gallery finger position that produced the score
};

struct IdentifyOptions {
    int top_k = 10;
    float min_score = 0.0f;   ///< drop candidates below this score
    int threads = 0;          ///< 0 = all hardware threads
};

struct IdentifyResult {
    std::vector<Candidate> candidates;  ///< best first
    uint64_t compared = 0;
    double elapsed_ms = 0;
};

// ---------------------------------------------------------------------------
// Multi-finger search
// ---------------------------------------------------------------------------
struct FingerProbe {
    uint8_t finger = 0;     ///< position 1..10, or 0 = unknown (compared with every finger)
    FingerTemplate tmpl;
};

/// Decision rule for a subject given one score per probe finger.
struct FusionPolicy {
    /// Per-finger threshold when two or more fingers are presented.
    float finger_threshold = kVerifyThreshold;
    /// How many presented fingers must reach `finger_threshold`
    /// (capped at the number of fingers presented).
    int min_fingers = 2;
    /// Threshold when only one finger is presented (single-finger 1:N rule).
    float single_threshold = kIdentifyThreshold;
    /// With several fingers presented, one finger alone this strong is also
    /// accepted (e.g. the second finger was smudged). <= 0 disables.
    float strong_single_threshold = 30.0f;
};

struct FingerScore {
    uint8_t probe_finger = 0;    ///< position given for the probe finger
    uint8_t gallery_finger = 0;  ///< position of the enrolled finger it matched
    float score = 0;
};

struct SubjectCandidate {
    std::string id;
    float score = 0;            ///< fused score = sum of per-finger scores
    bool match = false;         ///< decision under the FusionPolicy
    int fingers_matched = 0;    ///< fingers with score >= finger_threshold
    std::vector<FingerScore> fingers;  ///< one entry per probe finger that scored
};

struct SubjectIdentifyOptions {
    int top_k = 5;
    int threads = 0;
    FusionPolicy policy;
};

struct SubjectIdentifyResult {
    std::vector<SubjectCandidate> candidates;  ///< matches first, then by fused score
    uint64_t compared = 0;      ///< finger-to-finger comparisons performed
    double elapsed_ms = 0;
};

/// Applies the policy to per-probe-finger scores (exposed for tests/tools).
void apply_policy(SubjectCandidate& c, size_t probes_presented, const FusionPolicy& p);

class ThreadPool;

class Gallery {
public:
    explicit Gallery(MatcherConfig cfg = {});
    ~Gallery();
    Gallery(const Gallery&) = delete;
    Gallery& operator=(const Gallery&) = delete;

    // --- enrolment ---------------------------------------------------------
    /// Insert or replace the template of (subject, finger). finger in 0..10.
    /// Returns false if the template has too few usable minutiae.
    bool insert(const std::string& subject, uint8_t finger, const FingerTemplate& t);
    bool insert(const std::string& subject, uint8_t finger, const CompiledTemplate& t);
    /// Single-finger convenience: finger slot 0 ("unknown").
    bool insert(const std::string& id, const FingerTemplate& t) { return insert(id, 0, t); }
    bool insert(const std::string& id, const CompiledTemplate& t) { return insert(id, 0, t); }

    bool remove(const std::string& subject, uint8_t finger);
    /// Remove all fingers of a subject. Returns number of fingers removed.
    size_t remove_subject(const std::string& subject);
    bool remove(const std::string& subject) { return remove_subject(subject) > 0; }

    bool contains(const std::string& subject) const;
    bool contains(const std::string& subject, uint8_t finger) const;
    /// Enrolled fingers of a subject: (position, minutiae count).
    std::vector<std::pair<uint8_t, int>> fingers_of(const std::string& subject) const;

    size_t size() const;           ///< live finger templates
    size_t subjects() const;       ///< subjects with at least one finger
    size_t memory_bytes() const;   ///< approximate arena usage
    void reserve(size_t templates, size_t avg_minutiae = 40);
    /// Reclaim space left by removed/replaced templates.
    void compact();

    // --- search ------------------------------------------------------------
    /// One probe template against every enrolled finger (any position).
    IdentifyResult identify(const FingerTemplate& probe, const IdentifyOptions& opt = {}) const;
    IdentifyResult identify(const Probe& probe, const IdentifyOptions& opt = {}) const;

    /// Several probe fingers of one person, fused per subject.
    SubjectIdentifyResult identify_subjects(const std::vector<FingerProbe>& probes,
                                            const SubjectIdentifyOptions& opt = {}) const;

    /// 1:1: best score of `probe` against any finger of `subject`. nullopt if unknown.
    std::optional<float> verify(const std::string& subject, const FingerTemplate& probe) const;
    /// 1:1 multi-finger with fusion. nullopt if subject unknown.
    std::optional<SubjectCandidate> verify_subject(const std::string& subject, const std::vector<FingerProbe>& probes,
                                                   const FusionPolicy& policy = {}) const;

    const Matcher& matcher() const { return matcher_; }

private:
    struct Entry {
        uint64_t m_off;
        uint64_t e_off;
        uint32_t subject;
        uint16_t n_m;
        uint16_t n_e;
        uint8_t finger;
        bool alive;
    };
    using Slots = std::array<int32_t, kMaxFingerPosition + 1>;  // entry index per position, -1 = none

    TemplateView view_of(const Entry& e) const;
    bool insert_locked(const std::string& subject, uint8_t finger, const CompiledTemplate& t);
    int subject_index(const std::string& subject) const;  // -1 if unknown
    static void check_finger(uint8_t finger);

    Matcher matcher_;
    mutable std::shared_mutex mu_;
    std::vector<CMinutia> minutiae_;
    std::vector<CEdge> edges_;
    std::vector<Entry> entries_;
    std::vector<std::string> subject_names_;
    std::vector<Slots> slots_;
    std::unordered_map<std::string, uint32_t> subject_index_;
    size_t live_ = 0;
    std::unique_ptr<ThreadPool> pool_;
};

}  // namespace fpx
