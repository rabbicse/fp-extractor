// SPDX-License-Identifier: MIT
#include "fpx/gallery.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include "fpx/image.hpp"  // fpx::Error

namespace fpx {

// Minimal fixed-size pool: run(n, fn) executes fn(worker_index) on n workers
// (the calling thread participates) and waits for completion.
class ThreadPool {
public:
    explicit ThreadPool(int n) {
        for (int i = 1; i < n; ++i) workers_.emplace_back([this, i] { loop(i); });
    }
    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_) t.join();
    }
    int size() const { return int(workers_.size()) + 1; }

    void run(int n, const std::function<void(int)>& fn) {
        // One job at a time on this pool, admitted in arrival order. A plain
        // mutex is not fair: under concurrent searches some requests waited
        // many times longer than others (p95 several times p50).
        Turn turn(*this);
        n = std::max(1, std::min(n, size()));
        {
            std::lock_guard<std::mutex> lk(mu_);
            fn_ = &fn;
            job_n_ = n;
            active_ = n;
            error_ = nullptr;
            ++generation_;
        }
        cv_.notify_all();
        call(fn, 0);
        // Always wait for the workers: fn refers to the caller's stack.
        std::unique_lock<std::mutex> lk(mu_);
        if (--active_ > 0) done_cv_.wait(lk, [&] { return active_ == 0; });
        fn_ = nullptr;
        if (error_) std::rethrow_exception(std::exchange(error_, nullptr));
    }

private:
    // FIFO ticket: waits for this caller's turn, releases it on scope exit.
    struct Turn {
        ThreadPool& p;
        explicit Turn(ThreadPool& pool) : p(pool) {
            std::unique_lock<std::mutex> lk(p.queue_mu_);
            const uint64_t ticket = p.next_ticket_++;
            p.queue_cv_.wait(lk, [&] { return p.serving_ == ticket; });
        }
        ~Turn() {
            {
                std::lock_guard<std::mutex> lk(p.queue_mu_);
                ++p.serving_;
            }
            p.queue_cv_.notify_all();
        }
    };

    // Keeps the first exception of a job for the caller to rethrow; an
    // exception escaping a worker thread would terminate the process.
    void call(const std::function<void(int)>& fn, int idx) {
        try {
            fn(idx);
        } catch (...) {
            std::lock_guard<std::mutex> lk(mu_);
            if (!error_) error_ = std::current_exception();
        }
    }

    void loop(int idx) {
        uint64_t seen = 0;
        for (;;) {
            const std::function<void(int)>* fn;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [&] { return stop_ || generation_ != seen; });
                if (stop_) return;
                seen = generation_;
                if (idx >= job_n_) continue;  // not needed for this job
                fn = fn_;
            }
            call(*fn, idx);
            std::lock_guard<std::mutex> lk(mu_);
            if (--active_ == 0) done_cv_.notify_all();
        }
    }

    std::vector<std::thread> workers_;
    std::mutex mu_, queue_mu_;
    std::condition_variable cv_, done_cv_, queue_cv_;
    uint64_t next_ticket_ = 0, serving_ = 0;
    std::exception_ptr error_;
    const std::function<void(int)>* fn_ = nullptr;
    int active_ = 0, job_n_ = 0;
    uint64_t generation_ = 0;
    bool stop_ = false;
};

Gallery::Gallery(MatcherConfig cfg) : matcher_(cfg) {
    int hw = int(std::thread::hardware_concurrency());
    pool_ = std::make_unique<ThreadPool>(std::max(1, hw));
}

Gallery::~Gallery() = default;

void Gallery::check_finger(uint8_t finger) {
    if (finger > kMaxFingerPosition)
        throw Error("finger position must be 0..10 (got " + std::to_string(int(finger)) + ")");
}

TemplateView Gallery::view_of(const Entry& e) const {
    return {minutiae_.data() + e.m_off, edges_.data() + e.e_off, e.n_m, e.n_e};
}

int Gallery::subject_index(const std::string& subject) const {
    auto it = subject_index_.find(subject);
    return it == subject_index_.end() ? -1 : int(it->second);
}

bool Gallery::insert_locked(const std::string& subject, uint8_t finger, const CompiledTemplate& t) {
    if (t.minutiae.size() < 3) return false;
    uint32_t sidx;
    auto it = subject_index_.find(subject);
    if (it == subject_index_.end()) {
        sidx = uint32_t(subject_names_.size());
        subject_names_.push_back(subject);
        Slots empty;
        empty.fill(-1);
        slots_.push_back(empty);
        subject_index_.emplace(subject, sidx);
    } else {
        sidx = it->second;
    }
    int32_t& slot = slots_[sidx][finger];
    if (slot >= 0) {  // replace
        entries_[size_t(slot)].alive = false;
        --live_;
    }
    Entry e{minutiae_.size(), edges_.size(), sidx, uint16_t(t.minutiae.size()), uint16_t(t.edges.size()), finger, true};
    minutiae_.insert(minutiae_.end(), t.minutiae.begin(), t.minutiae.end());
    edges_.insert(edges_.end(), t.edges.begin(), t.edges.end());
    slot = int32_t(entries_.size());
    entries_.push_back(e);
    ++live_;
    return true;
}

bool Gallery::insert(const std::string& subject, uint8_t finger, const FingerTemplate& t) {
    check_finger(finger);
    CompiledTemplate ct = compile(t, matcher_.config());  // outside the lock
    std::unique_lock<std::shared_mutex> lk(mu_);
    return insert_locked(subject, finger, ct);
}

bool Gallery::insert(const std::string& subject, uint8_t finger, const CompiledTemplate& t) {
    check_finger(finger);
    std::unique_lock<std::shared_mutex> lk(mu_);
    return insert_locked(subject, finger, t);
}

bool Gallery::remove(const std::string& subject, uint8_t finger) {
    check_finger(finger);
    std::unique_lock<std::shared_mutex> lk(mu_);
    int s = subject_index(subject);
    if (s < 0) return false;
    int32_t& slot = slots_[size_t(s)][finger];
    if (slot < 0) return false;
    entries_[size_t(slot)].alive = false;
    slot = -1;
    --live_;
    return true;
}

size_t Gallery::remove_subject(const std::string& subject) {
    std::unique_lock<std::shared_mutex> lk(mu_);
    int s = subject_index(subject);
    if (s < 0) return 0;
    size_t n = 0;
    for (int32_t& slot : slots_[size_t(s)]) {
        if (slot < 0) continue;
        entries_[size_t(slot)].alive = false;
        slot = -1;
        --live_;
        ++n;
    }
    return n;
}

bool Gallery::contains(const std::string& subject) const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    int s = subject_index(subject);
    if (s < 0) return false;
    for (int32_t slot : slots_[size_t(s)])
        if (slot >= 0) return true;
    return false;
}

bool Gallery::contains(const std::string& subject, uint8_t finger) const {
    if (finger > kMaxFingerPosition) return false;
    std::shared_lock<std::shared_mutex> lk(mu_);
    int s = subject_index(subject);
    return s >= 0 && slots_[size_t(s)][finger] >= 0;
}

std::vector<std::pair<uint8_t, int>> Gallery::fingers_of(const std::string& subject) const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    std::vector<std::pair<uint8_t, int>> out;
    int s = subject_index(subject);
    if (s < 0) return out;
    for (int f = 0; f <= kMaxFingerPosition; ++f) {
        int32_t slot = slots_[size_t(s)][size_t(f)];
        if (slot >= 0) out.emplace_back(uint8_t(f), int(entries_[size_t(slot)].n_m));
    }
    return out;
}

size_t Gallery::size() const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    return live_;
}

size_t Gallery::subjects() const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    size_t n = 0;
    for (const Slots& sl : slots_)
        for (int32_t slot : sl)
            if (slot >= 0) {
                ++n;
                break;
            }
    return n;
}

size_t Gallery::memory_bytes() const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    size_t names = 0;
    for (const auto& s : subject_names_) names += s.capacity() + sizeof(std::string);
    return minutiae_.capacity() * sizeof(CMinutia) + edges_.capacity() * sizeof(CEdge) +
           entries_.capacity() * sizeof(Entry) + slots_.capacity() * sizeof(Slots) + names +
           subject_index_.size() * (sizeof(std::string) + 32);
}

void Gallery::reserve(size_t templates, size_t avg_minutiae) {
    std::unique_lock<std::shared_mutex> lk(mu_);
    minutiae_.reserve(templates * avg_minutiae);
    edges_.reserve(templates * avg_minutiae * size_t(std::max(1, matcher_.config().neighbors)));
    entries_.reserve(templates);
    subject_names_.reserve(templates);
    slots_.reserve(templates);
    subject_index_.reserve(templates);
}

void Gallery::compact() {
    std::unique_lock<std::shared_mutex> lk(mu_);
    std::vector<CMinutia> m;
    std::vector<CEdge> e;
    std::vector<Entry> en;
    std::vector<std::string> names;
    std::vector<Slots> slots;
    m.reserve(minutiae_.size());
    e.reserve(edges_.size());
    en.reserve(live_);
    subject_index_.clear();
    for (size_t s = 0; s < subject_names_.size(); ++s) {
        Slots ns;
        ns.fill(-1);
        bool any = false;
        const uint32_t new_sidx = uint32_t(names.size());
        for (size_t f = 0; f < ns.size(); ++f) {
            int32_t slot = slots_[s][f];
            if (slot < 0) continue;
            const Entry& x = entries_[size_t(slot)];
            Entry y{m.size(), e.size(), new_sidx, x.n_m, x.n_e, x.finger, true};
            m.insert(m.end(), minutiae_.begin() + long(x.m_off), minutiae_.begin() + long(x.m_off + x.n_m));
            e.insert(e.end(), edges_.begin() + long(x.e_off), edges_.begin() + long(x.e_off + x.n_e));
            ns[f] = int32_t(en.size());
            en.push_back(y);
            any = true;
        }
        if (!any) continue;  // subject without fingers disappears
        subject_index_.emplace(subject_names_[s], new_sidx);
        names.push_back(std::move(subject_names_[s]));
        slots.push_back(ns);
    }
    minutiae_.swap(m);
    edges_.swap(e);
    entries_.swap(en);
    subject_names_.swap(names);
    slots_.swap(slots);
}

// ---------------------------------------------------------------------------
// Single-template identification
// ---------------------------------------------------------------------------
IdentifyResult Gallery::identify(const FingerTemplate& probe, const IdentifyOptions& opt) const {
    Probe p(probe, matcher_.config());
    return identify(p, opt);
}

IdentifyResult Gallery::identify(const Probe& probe, const IdentifyOptions& opt) const {
    const auto t0 = std::chrono::steady_clock::now();
    IdentifyResult res;
    const int top_k = std::max(1, opt.top_k);
    std::shared_lock<std::shared_mutex> lk(mu_);
    const size_t n = entries_.size();
    const int threads = std::max(1, opt.threads > 0 ? std::min(opt.threads, pool_->size()) : pool_->size());

    struct Hit {
        float score;
        uint32_t idx;
    };
    // Total order: higher score first, then lower gallery index. Without the
    // tie-break, which of several equal scores made the top-k depended on how
    // threads happened to split the gallery, so results changed between runs.
    auto better = [](const Hit& a, const Hit& b) { return a.score > b.score || (a.score == b.score && a.idx < b.idx); };
    auto worse = better;  // as heap "less": front() is the worst kept hit
    // Scores must be >= min_score: anything <= the next float below it is unused.
    const float min_need = opt.min_score > 0.f ? std::nextafter(opt.min_score, 0.f) : 0.f;
    std::vector<std::vector<Hit>> local{size_t(threads)};
    std::vector<uint64_t> counts(size_t(threads), 0);
    const size_t chunk = 4096;  // dynamic chunking -> good load balance
    std::atomic<size_t> next{0};

    pool_->run(threads, [&](int w) {
        std::vector<Hit>& heap = local[size_t(w)];
        heap.reserve(size_t(top_k) + 1);
        uint64_t cnt = 0;
        for (;;) {
            size_t begin = next.fetch_add(chunk);
            if (begin >= n) break;
            size_t end = std::min(n, begin + chunk);
            for (size_t i = begin; i < end; ++i) {
                const Entry& e = entries_[i];
                if (!e.alive) continue;
                ++cnt;
                // Only scores that could enter this thread's top-k matter (an
                // equal score can still win on the index tie-break).
                const float need =
                    std::max(min_need, int(heap.size()) < top_k ? 0.f : std::nextafter(heap.front().score, 0.f));
                float s = matcher_.match(probe, view_of(e), nullptr, false, need);
                if (s < opt.min_score || s <= 0.f) continue;
                if (int(heap.size()) < top_k) {
                    heap.push_back({s, uint32_t(i)});
                    std::push_heap(heap.begin(), heap.end(), worse);
                } else if (better({s, uint32_t(i)}, heap.front())) {
                    std::pop_heap(heap.begin(), heap.end(), worse);
                    heap.back() = {s, uint32_t(i)};
                    std::push_heap(heap.begin(), heap.end(), worse);
                }
            }
        }
        counts[size_t(w)] = cnt;
    });

    std::vector<Hit> all;
    for (auto& h : local) all.insert(all.end(), h.begin(), h.end());
    std::sort(all.begin(), all.end(), better);
    if (int(all.size()) > top_k) all.resize(size_t(top_k));
    for (const Hit& h : all) {
        const Entry& e = entries_[h.idx];
        res.candidates.push_back({subject_names_[e.subject], h.score, e.finger});
    }
    for (uint64_t c : counts) res.compared += c;
    res.elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return res;
}

// ---------------------------------------------------------------------------
// Multi-finger identification
// ---------------------------------------------------------------------------
void apply_policy(SubjectCandidate& c, size_t presented, const FusionPolicy& p) {
    c.score = 0;
    c.fingers_matched = 0;
    float best = 0;
    for (const FingerScore& f : c.fingers) {
        c.score += f.score;
        best = std::max(best, f.score);
        if (f.score >= p.finger_threshold) ++c.fingers_matched;
    }
    if (presented <= 1) {
        c.match = best >= p.single_threshold;
    } else {
        const int need = std::max(1, std::min(p.min_fingers, int(presented)));
        c.match = c.fingers_matched >= need || (p.strong_single_threshold > 0 && best >= p.strong_single_threshold);
    }
}

namespace {

inline bool positions_compatible(uint8_t probe_finger, uint8_t gallery_finger) {
    return probe_finger == 0 || gallery_finger == 0 || probe_finger == gallery_finger;
}

struct SubjectHit {
    uint32_t subject;
    float score;
    uint8_t probe;           // index into probes
    uint8_t gallery_finger;
};

// Best one-to-one assignment of probe fingers to gallery fingers for one
// subject (greedy by score; at most 10 x 11 candidates).
void assign(std::vector<SubjectHit>::iterator b, std::vector<SubjectHit>::iterator e,
            const std::vector<FingerProbe>& probes, std::vector<FingerScore>& out) {
    std::sort(b, e, [](const SubjectHit& x, const SubjectHit& y) { return x.score > y.score; });
    uint32_t used_probe = 0, used_gallery = 0;
    for (auto it = b; it != e; ++it) {
        if ((used_probe >> it->probe) & 1u) continue;
        if ((used_gallery >> it->gallery_finger) & 1u) continue;
        used_probe |= 1u << it->probe;
        used_gallery |= 1u << it->gallery_finger;
        out.push_back({probes[it->probe].finger, it->gallery_finger, it->score});
    }
}

}  // namespace

SubjectIdentifyResult Gallery::identify_subjects(const std::vector<FingerProbe>& probes,
                                                 const SubjectIdentifyOptions& opt) const {
    const auto t0 = std::chrono::steady_clock::now();
    SubjectIdentifyResult res;
    if (probes.empty()) throw Error("identify: at least one probe finger is required");
    if (probes.size() > size_t(kMaxFingerPosition)) throw Error("identify: at most 10 probe fingers");
    for (const auto& p : probes) check_finger(p.finger);

    std::vector<Probe> compiled;
    compiled.reserve(probes.size());
    for (const auto& p : probes) compiled.emplace_back(p.tmpl, matcher_.config());

    // Scores below this cannot influence any decision; dropping them keeps
    // the per-thread hit lists tiny (impostor scores are almost always ~0).
    const float floor = std::max(0.5f, 0.25f * std::min(opt.policy.finger_threshold, opt.policy.single_threshold));
    const float need = std::nextafter(floor, 0.f);  // scores <= this are below floor

    std::shared_lock<std::shared_mutex> lk(mu_);
    const size_t n = entries_.size();
    const int threads = std::max(1, opt.threads > 0 ? std::min(opt.threads, pool_->size()) : pool_->size());
    std::vector<std::vector<SubjectHit>> local{size_t(threads)};
    std::vector<uint64_t> counts(size_t(threads), 0);
    const size_t chunk = 4096;
    std::atomic<size_t> next{0};

    // One pass over the gallery; every entry is compared with each probe
    // finger whose position is compatible.
    pool_->run(threads, [&](int w) {
        auto& hits = local[size_t(w)];
        uint64_t cnt = 0;
        for (;;) {
            size_t begin = next.fetch_add(chunk);
            if (begin >= n) break;
            size_t end = std::min(n, begin + chunk);
            for (size_t i = begin; i < end; ++i) {
                const Entry& e = entries_[i];
                if (!e.alive) continue;
                const TemplateView v = view_of(e);
                for (size_t p = 0; p < probes.size(); ++p) {
                    if (!positions_compatible(probes[p].finger, e.finger)) continue;
                    ++cnt;
                    float s = matcher_.match(compiled[p], v, nullptr, false, need);
                    if (s >= floor) hits.push_back({e.subject, s, uint8_t(p), e.finger});
                }
            }
        }
        counts[size_t(w)] = cnt;
    });

    std::vector<SubjectHit> all;
    for (auto& h : local) all.insert(all.end(), h.begin(), h.end());
    // Total order (thread-local lists arrive in scheduling order), so the
    // per-subject finger assignment is the same on every run.
    std::sort(all.begin(), all.end(), [](const SubjectHit& a, const SubjectHit& b) {
        if (a.subject != b.subject) return a.subject < b.subject;
        if (a.probe != b.probe) return a.probe < b.probe;
        if (a.gallery_finger != b.gallery_finger) return a.gallery_finger < b.gallery_finger;
        return a.score > b.score;
    });
    std::vector<SubjectCandidate> cands;
    for (size_t i = 0; i < all.size();) {
        size_t j = i;
        while (j < all.size() && all[j].subject == all[i].subject) ++j;
        SubjectCandidate c;
        c.id = subject_names_[all[i].subject];
        assign(all.begin() + long(i), all.begin() + long(j), probes, c.fingers);
        apply_policy(c, probes.size(), opt.policy);
        cands.push_back(std::move(c));
        i = j;
    }
    // Stable: equal candidates keep gallery (subject index) order.
    std::stable_sort(cands.begin(), cands.end(), [](const SubjectCandidate& a, const SubjectCandidate& b) {
        if (a.match != b.match) return a.match;
        return a.score > b.score;
    });
    if (cands.size() > size_t(std::max(1, opt.top_k))) cands.resize(size_t(std::max(1, opt.top_k)));
    res.candidates = std::move(cands);
    for (uint64_t c : counts) res.compared += c;
    res.elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return res;
}

// ---------------------------------------------------------------------------
// Verification
// ---------------------------------------------------------------------------
std::optional<float> Gallery::verify(const std::string& subject, const FingerTemplate& probe) const {
    Probe p(probe, matcher_.config());
    std::shared_lock<std::shared_mutex> lk(mu_);
    int s = subject_index(subject);
    if (s < 0) return std::nullopt;
    std::optional<float> best;
    for (int32_t slot : slots_[size_t(s)]) {
        if (slot < 0) continue;
        float sc = matcher_.match(p, view_of(entries_[size_t(slot)]));
        if (!best || sc > *best) best = sc;
    }
    return best;
}

std::optional<SubjectCandidate> Gallery::verify_subject(const std::string& subject,
                                                        const std::vector<FingerProbe>& probes,
                                                        const FusionPolicy& policy) const {
    if (probes.empty()) throw Error("verify: at least one probe finger is required");
    if (probes.size() > size_t(kMaxFingerPosition)) throw Error("verify: at most 10 probe fingers");
    for (const auto& p : probes) check_finger(p.finger);
    std::vector<Probe> compiled;
    for (const auto& p : probes) compiled.emplace_back(p.tmpl, matcher_.config());
    std::shared_lock<std::shared_mutex> lk(mu_);
    int s = subject_index(subject);
    if (s < 0) return std::nullopt;
    std::vector<SubjectHit> hits;
    bool any = false;
    for (int32_t slot : slots_[size_t(s)]) {
        if (slot < 0) continue;
        any = true;
        const Entry& e = entries_[size_t(slot)];
        for (size_t p = 0; p < probes.size(); ++p) {
            if (!positions_compatible(probes[p].finger, e.finger)) continue;
            hits.push_back({uint32_t(s), matcher_.match(compiled[p], view_of(e)), uint8_t(p), e.finger});
        }
    }
    if (!any) return std::nullopt;
    SubjectCandidate c;
    c.id = subject;
    assign(hits.begin(), hits.end(), probes, c.fingers);
    apply_policy(c, probes.size(), policy);
    return c;
}

}  // namespace fpx
