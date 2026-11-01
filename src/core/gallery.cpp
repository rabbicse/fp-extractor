// SPDX-License-Identifier: MIT
#include "fpx/gallery.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

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
        std::lock_guard<std::mutex> job_lock(job_mu_);  // one job at a time on this pool
        n = std::max(1, std::min(n, size()));
        {
            std::lock_guard<std::mutex> lk(mu_);
            fn_ = &fn;
            job_n_ = n;
            active_ = n;
            ++generation_;
        }
        cv_.notify_all();
        fn(0);
        std::unique_lock<std::mutex> lk(mu_);
        if (--active_ > 0) done_cv_.wait(lk, [&] { return active_ == 0; });
        fn_ = nullptr;
    }

private:
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
            (*fn)(idx);
            std::lock_guard<std::mutex> lk(mu_);
            if (--active_ == 0) done_cv_.notify_all();
        }
    }

    std::vector<std::thread> workers_;
    std::mutex mu_, job_mu_;
    std::condition_variable cv_, done_cv_;
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

TemplateView Gallery::view_of(const Entry& e) const {
    return {minutiae_.data() + e.m_off, edges_.data() + e.e_off, e.n_m, e.n_e};
}

bool Gallery::insert_locked(const std::string& id, const CompiledTemplate& t) {
    if (t.minutiae.size() < 3) return false;
    auto it = index_.find(id);
    if (it != index_.end()) {
        entries_[it->second].alive = false;
        --live_;
    }
    Entry e{minutiae_.size(), edges_.size(), uint16_t(t.minutiae.size()), uint16_t(t.edges.size()), true};
    minutiae_.insert(minutiae_.end(), t.minutiae.begin(), t.minutiae.end());
    edges_.insert(edges_.end(), t.edges.begin(), t.edges.end());
    const uint32_t idx = uint32_t(entries_.size());
    entries_.push_back(e);
    if (it != index_.end()) {
        ids_.push_back(id);
        it->second = idx;
    } else {
        ids_.push_back(id);
        index_.emplace(id, idx);
    }
    ++live_;
    return true;
}

bool Gallery::insert(const std::string& id, const FingerTemplate& t) {
    CompiledTemplate ct = compile(t, matcher_.config());  // outside the lock
    std::unique_lock<std::shared_mutex> lk(mu_);
    return insert_locked(id, ct);
}

bool Gallery::insert(const std::string& id, const CompiledTemplate& t) {
    std::unique_lock<std::shared_mutex> lk(mu_);
    return insert_locked(id, t);
}

bool Gallery::remove(const std::string& id) {
    std::unique_lock<std::shared_mutex> lk(mu_);
    auto it = index_.find(id);
    if (it == index_.end()) return false;
    entries_[it->second].alive = false;
    index_.erase(it);
    --live_;
    return true;
}

bool Gallery::contains(const std::string& id) const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    return index_.count(id) != 0;
}

size_t Gallery::size() const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    return live_;
}

size_t Gallery::memory_bytes() const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    size_t ids = 0;
    for (const auto& s : ids_) ids += s.capacity() + sizeof(std::string);
    return minutiae_.capacity() * sizeof(CMinutia) + edges_.capacity() * sizeof(CEdge) +
           entries_.capacity() * sizeof(Entry) + ids + index_.size() * (sizeof(std::string) + 32);
}

void Gallery::reserve(size_t templates, size_t avg_minutiae) {
    std::unique_lock<std::shared_mutex> lk(mu_);
    minutiae_.reserve(templates * avg_minutiae);
    edges_.reserve(templates * avg_minutiae * size_t(std::max(1, matcher_.config().neighbors)));
    entries_.reserve(templates);
    ids_.reserve(templates);
    index_.reserve(templates);
}

void Gallery::compact() {
    std::unique_lock<std::shared_mutex> lk(mu_);
    std::vector<CMinutia> m;
    std::vector<CEdge> e;
    std::vector<Entry> en;
    std::vector<std::string> ids;
    m.reserve(minutiae_.size());
    e.reserve(edges_.size());
    en.reserve(live_);
    ids.reserve(live_);
    index_.clear();
    for (size_t i = 0; i < entries_.size(); ++i) {
        const Entry& x = entries_[i];
        if (!x.alive) continue;
        Entry y{m.size(), e.size(), x.n_m, x.n_e, true};
        m.insert(m.end(), minutiae_.begin() + x.m_off, minutiae_.begin() + x.m_off + x.n_m);
        e.insert(e.end(), edges_.begin() + x.e_off, edges_.begin() + x.e_off + x.n_e);
        index_.emplace(ids_[i], uint32_t(en.size()));
        en.push_back(y);
        ids.push_back(std::move(ids_[i]));
    }
    minutiae_.swap(m);
    edges_.swap(e);
    entries_.swap(en);
    ids_.swap(ids);
}

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
    auto worse = [](const Hit& a, const Hit& b) { return a.score > b.score; };  // min-heap on score
    std::vector<std::vector<Hit>> local(threads);
    std::vector<uint64_t> counts(threads, 0);
    // Dynamic chunking: good load balance even with uneven template sizes.
    const size_t chunk = 4096;
    std::atomic<size_t> next{0};

    pool_->run(threads, [&](int w) {
        std::vector<Hit>& heap = local[w];
        heap.reserve(top_k + 1);
        uint64_t cnt = 0;
        for (;;) {
            size_t begin = next.fetch_add(chunk);
            if (begin >= n) break;
            size_t end = std::min(n, begin + chunk);
            for (size_t i = begin; i < end; ++i) {
                const Entry& e = entries_[i];
                if (!e.alive) continue;
                ++cnt;
                float s = matcher_.match(probe, view_of(e));
                if (s < opt.min_score || s <= 0.f) continue;
                if (int(heap.size()) < top_k) {
                    heap.push_back({s, uint32_t(i)});
                    std::push_heap(heap.begin(), heap.end(), worse);
                } else if (s > heap.front().score) {
                    std::pop_heap(heap.begin(), heap.end(), worse);
                    heap.back() = {s, uint32_t(i)};
                    std::push_heap(heap.begin(), heap.end(), worse);
                }
            }
        }
        counts[w] = cnt;
    });

    std::vector<Hit> all;
    for (auto& h : local) all.insert(all.end(), h.begin(), h.end());
    std::sort(all.begin(), all.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
    if (int(all.size()) > top_k) all.resize(top_k);
    for (const Hit& h : all) res.candidates.push_back({ids_[h.idx], h.score});
    for (uint64_t c : counts) res.compared += c;
    res.elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return res;
}

std::optional<float> Gallery::verify(const std::string& id, const FingerTemplate& probe) const {
    Probe p(probe, matcher_.config());
    std::shared_lock<std::shared_mutex> lk(mu_);
    auto it = index_.find(id);
    if (it == index_.end()) return std::nullopt;
    return matcher_.match(p, view_of(entries_[it->second]));
}

}  // namespace fpx
