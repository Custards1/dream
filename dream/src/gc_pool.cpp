#include "gc_pool.hpp"

#include <algorithm>
#include <cstdlib>

namespace dream {

namespace {

/// How many threads a collection may use at most, the collecting one included.
///
/// Not the whole machine. Tracing is bound by memory, not by arithmetic, so the
/// curve flattens well before the core count -- and the threads it would have
/// used are threads the scheduler wants for processes. On the self-compile the
/// wall time stops improving at about four and the pause keeps falling past
/// sixteen, so eight is a compromise between the two and not a measurement of
/// either. `DREAM_GC_THREADS` is how the next workload gets to disagree, and 1
/// turns the helpers off altogether.
unsigned configured_capacity() {
    if (const char* env = std::getenv("DREAM_GC_THREADS")) {
        long long n = std::atoll(env);
        if (n > 0) return unsigned(std::min<long long>(n, 64));
    }
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 1;
    return std::min(hw, 8u);
}

}  // namespace

GcPool::GcPool() : capacity_(configured_capacity()) {}

GcPool::~GcPool() {
    {
        std::lock_guard<std::mutex> g(mutex_);
        stopping_ = true;
    }
    start_cv_.notify_all();
    for (std::thread& t : threads_) {
        if (t.joinable()) t.join();
    }
}

GcPool& GcPool::instance() {
    static GcPool pool;
    return pool;
}

void GcPool::ensure_threads(unsigned n) {
    while (threads_.size() < n) {
        unsigned index = unsigned(threads_.size()) + 1;  // 0 is the caller
        threads_.emplace_back([this, index] { helper_loop(index); });
    }
}

void GcPool::helper_loop(unsigned index) {
    uint64_t seen = 0;
    for (;;) {
        std::unique_lock<std::mutex> lk(mutex_);
        start_cv_.wait(lk, [&] { return stopping_ || generation_ != seen; });
        if (stopping_) return;
        seen = generation_;
        // A round may want fewer threads than the pool has started. Those
        // outside it take note of the generation and go back to sleep, so the
        // next round still finds them waiting rather than a round behind.
        if (index >= round_size_) continue;
        const auto* body = body_;
        lk.unlock();

        (*body)(index, round_size_);

        lk.lock();
        if (--pending_ == 0) done_cv_.notify_one();
    }
}

bool GcPool::run(unsigned want, const std::function<void(unsigned, unsigned)>& body) {
    if (capacity_ < 2 || want < 2) return false;
    // One heap at a time. The alternative -- queueing -- would make one
    // process's collection wait on another's, which is exactly the stall this
    // is here to shorten.
    if (busy_.exchange(true, std::memory_order_acquire)) return false;

    unsigned size = std::min(want, capacity_);
    if (idle_hint_) {
        // The caller's own thread is running this collection, so it is not one
        // of the idle ones: helpers are capped by how many workers are asleep.
        unsigned idle = idle_hint_->load(std::memory_order_relaxed);
        size = std::min(size, idle + 1);
    }
    if (size < 2) {
        busy_.store(false, std::memory_order_release);
        return false;
    }

    {
        std::lock_guard<std::mutex> g(mutex_);
        ensure_threads(size - 1);
        body_ = &body;
        round_size_ = size;
        pending_ = size - 1;
        ++generation_;
    }
    start_cv_.notify_all();

    body(0, size);

    {
        std::unique_lock<std::mutex> lk(mutex_);
        done_cv_.wait(lk, [&] { return pending_ == 0; });
        body_ = nullptr;
        round_size_ = 0;
    }
    busy_.store(false, std::memory_order_release);
    return true;
}

}  // namespace dream
