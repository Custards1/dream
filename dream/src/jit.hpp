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

    /// The tier to enter `func_index` with: a compiled body, or null for the
    /// interpreter.
    ///
    /// This runs on *every function entry*, so it is inline, takes no lock,
    /// and crosses into the compiler only on the one entry that makes a
    /// function hot. What makes that possible is that `cached_` holds three
    /// states rather than two -- a compiled body, the rejected marker, or null
    /// while the function is still cold -- so one atomic load answers the
    /// question. With only "compiled" and "everything else", a function LLVM
    /// had refused took the JIT's global mutex and two hash lookups on every
    /// entry, for ever, to be told again that it could not be compiled; in a
    /// program that enters a hundred and eighty million functions that is not
    /// a slow path, it is the program.
    CompiledFn tier(uint32_t func_index) {
        if (func_index >= cached_.size()) return nullptr;
        CompiledFn fn = cached_[func_index].load(std::memory_order_acquire);
        if (fn) return reinterpret_cast<uintptr_t>(fn) == kRejectedBits ? nullptr : fn;
        // Counting is a heuristic, so it is a relaxed load and store rather
        // than a read-modify-write: two racing workers may lose a count
        // between them, and a function compiled one entry late is not an
        // observable difference.
        const uint32_t n = counts_[func_index].load(std::memory_order_relaxed) + 1;
        counts_[func_index].store(n, std::memory_order_relaxed);
        if (n < threshold_.load(std::memory_order_relaxed)) return nullptr;
        return on_enter(func_index);
    }

    /// The entry that made `func_index` hot: compile it, or mark it rejected
    /// so `tier` never asks again. The slow path, and the only one that locks.
    CompiledFn on_enter(uint32_t func_index);

    /// Compile now, regardless of temperature. Returns nullptr on failure.
    CompiledFn compile(uint32_t func_index, std::string* error);

    void set_threshold(uint32_t calls);
    uint32_t threshold() const;
    uint64_t compiled_count() const;
    /// Human-readable IR for one function, for `--dump-jit`.
    std::string dump_ir(uint32_t func_index);

private:
    /// What `cached_` holds for a function compilation has been tried on and
    /// failed. Any value that cannot be a function pointer would do; one is
    /// the cheapest to compare against.
    static constexpr uintptr_t kRejectedBits = 1;
    static CompiledFn rejected() { return reinterpret_cast<CompiledFn>(kRejectedBits); }

    /// Dense per-function entry counters and compiled-body cache, indexed by
    /// image function index. They live on `Jit` rather than in `Impl` so
    /// `cached_compiled`, which runs on every function entry, can read the
    /// cache with a single load and no PIMPL indirection. Both are sized once,
    /// in the constructor, after the image has loaded.
    std::vector<std::atomic<uint32_t>> counts_;
    std::vector<std::atomic<CompiledFn>> cached_;
    /// Mirrors the threshold inside `Impl`, so `tier` can read it without
    /// reaching through the PIMPL.
    std::atomic<uint32_t> threshold_{0};

    /// Caller must hold the JIT lock.
    CompiledFn compile_locked(uint32_t func_index, std::string* error);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace dream
