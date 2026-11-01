// SPDX-License-Identifier: MIT
// In-memory 1:N identification index.
//
// Templates are compiled once and packed into contiguous arenas (≈ 6 B per
// minutia + 5 B per local structure, ~1.3 KB for a 40-minutiae template), so
// 1 M templates need ≈ 1.3-1.6 GB of RAM and a full scan is a streaming,
// cache-friendly pass split across all cores.
#pragma once

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

struct Candidate {
    std::string id;
    float score = 0;
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

class ThreadPool;

class Gallery {
public:
    explicit Gallery(MatcherConfig cfg = {});
    ~Gallery();
    Gallery(const Gallery&) = delete;
    Gallery& operator=(const Gallery&) = delete;

    /// Insert or replace. Returns false if the template has no usable minutiae.
    bool insert(const std::string& id, const FingerTemplate& t);
    bool insert(const std::string& id, const CompiledTemplate& t);
    bool remove(const std::string& id);
    bool contains(const std::string& id) const;

    size_t size() const;           ///< live templates
    size_t memory_bytes() const;   ///< approximate arena usage
    void reserve(size_t templates, size_t avg_minutiae = 40);
    /// Reclaim space left by removed/replaced templates.
    void compact();

    IdentifyResult identify(const FingerTemplate& probe, const IdentifyOptions& opt = {}) const;
    IdentifyResult identify(const Probe& probe, const IdentifyOptions& opt = {}) const;

    /// 1:1 against an enrolled id. nullopt if id unknown.
    std::optional<float> verify(const std::string& id, const FingerTemplate& probe) const;

    const Matcher& matcher() const { return matcher_; }

private:
    struct Entry {
        uint64_t m_off;
        uint64_t e_off;
        uint16_t n_m;
        uint16_t n_e;
        bool alive;
    };
    TemplateView view_of(const Entry& e) const;
    bool insert_locked(const std::string& id, const CompiledTemplate& t);

    Matcher matcher_;
    mutable std::shared_mutex mu_;
    std::vector<CMinutia> minutiae_;
    std::vector<CEdge> edges_;
    std::vector<Entry> entries_;
    std::vector<std::string> ids_;
    std::unordered_map<std::string, uint32_t> index_;
    size_t live_ = 0;
    std::unique_ptr<ThreadPool> pool_;
};

}  // namespace fpx
