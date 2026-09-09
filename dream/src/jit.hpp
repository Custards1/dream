// The JIT tier.
//
// The interpreter is the baseline; the JIT is an optimization that must never
// change observable behaviour. Functions are compiled after they prove hot,
// and a compiled function is entered through the same calling convention the
// interpreter uses, so the two tiers interleave freely.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "value.hpp"

namespace dream {

class Runtime;
class Process;

/// Signature of a JIT-compiled function body.
///
/// `status` reports what happened: 0 the return value is a result, 1 it is an
/// error to raise, 2 the reduction budget ran out mid-loop and the frame holds
/// the loop-carried state, so the caller should resume the body from the top.
/// That last case is what keeps compiled loops preemptible.
using CompiledFn = Value (*)(Process* p, Value frame, int* status);

class Jit {
public:
    static bool available();

    explicit Jit(Runtime& rt);
    ~Jit();
    Jit(const Jit&) = delete;
    Jit& operator=(const Jit&) = delete;

    /// The fast side of `on_enter`: one read of the compiled-body cache.
    /// Returns true and stores the compiled body when this function has
    /// already been compiled. When it returns false the caller must let the
    /// interpreter run and call `on_enter` so the miss can count towards the
    /// threshold. `cached_compiled` is defined inline so the interpreted path
    /// -- where this runs on every function entry -- costs a single load and
    /// no call into the JIT machinery; `on_enter` keeps the slow path cold.
    bool cached_compiled(uint32_t func_index, CompiledFn* out) const {
        if (func_index >= cached_.size()) return false;
        CompiledFn fn = cached_[func_index].load(std::memory_order_acquire);
        if (!fn) return false;
        *out = fn;
        return true;
    }

    /// Count an entry into `func_index`; returns a compiled body once the
    /// function is hot and compilation has succeeded, otherwise nullptr.
    /// The slow path: consult after `cached_compiled` has missed.
    CompiledFn on_enter(uint32_t func_index);

    /// Compile now, regardless of temperature. Returns nullptr on failure.
    CompiledFn compile(uint32_t func_index, std::string* error);

    void set_threshold(uint32_t calls);
    uint32_t threshold() const;
    uint64_t compiled_count() const;
    /// Human-readable IR for one function, for `--dump-jit`.
    std::string dump_ir(uint32_t func_index);

private:
    /// Dense per-function entry counters and compiled-body cache, indexed by
    /// image function index. They live on `Jit` rather than in `Impl` so
    /// `cached_compiled`, which runs on every function entry, can read the
    /// cache with a single load and no PIMPL indirection. Both are sized once,
    /// in the constructor, after the image has loaded.
    std::vector<std::atomic<uint32_t>> counts_;
    std::vector<std::atomic<CompiledFn>> cached_;

    /// Caller must hold the JIT lock.
    CompiledFn compile_locked(uint32_t func_index, std::string* error);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace dream
