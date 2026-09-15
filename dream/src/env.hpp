// The `DREAM_*` knobs, read once.
//
// Every one of these is a tuning or tracing switch that cannot change while a
// program runs: a nursery cap, a limit, a trace flag. The obvious way to read
// one is `getenv` behind a function-local `static`, and that is what each site
// used to do -- but a function-local static with a dynamic initializer is not a
// load. It is a guard byte in `.bss`, an acquire load of it, a branch, and on
// the cold side a call into `__cxa_guard_acquire` and libc; the guard lives on
// a different cache line from the answer, so the common path touches two. That
// is nothing per collection and it is not nothing where the runtime actually
// asks: `force_whnf` reads three limits and is the loop a lazy program lives
// in, and `IOTRACE` is the first thing every IO native does.
//
// So the questions are all asked once, at load, and the answers live in one
// struct that the hot paths read as a plain field. The cost of a knob is now a
// load from a line that is warm because the knob beside it was just read.
//
// Two consequences worth knowing. The environment is sampled *before* `main`
// rather than at the first collection or the first IO, so a Dream program that
// calls `os.set_env! "DREAM_GC_TRACE" "1"` on itself no longer turns tracing on
// -- it never reliably did, since whichever site got there first won, but now
// it never does at all. And nothing in this library may read `g_env` from its
// own dynamic initializer, because the order of those across translation units
// is not defined; every field here is for run time.

#pragma once

#include <cstddef>

namespace dream {

struct EnvConfig {
    // --- Limits, per process ---------------------------------------------
    //
    // A process is the unit of failure here, so exhausting memory has to kill
    // the process that did it and nothing else. Without these, `std::bad_alloc`
    // escapes the allocator and calls `terminate`, which loses every other
    // process, the scheduler, and any work already done.
    //
    // Each is generous enough that ordinary programs never approach it -- a
    // tail call pops its continuation, so a loop runs in constant space, and
    // `sum_to 100000` needs 100k frames against a limit of four million -- and
    // each can be moved for a program that genuinely needs more.

    /// DREAM_MAX_DEPTH: pending continuations. ~96 MB of them at the default.
    size_t max_conts;
    /// DREAM_MAX_STACK: value-stack entries. ~32 MB at the default.
    size_t max_stack;
    /// DREAM_MAX_HEAP: heap bytes owned by one process.
    size_t max_heap;

    // --- The collector ----------------------------------------------------

    /// DREAM_NURSERY_MAX: the cap on how far a busy process's nursery grows.
    /// The default is where the self-compile stops improving; the knob is for
    /// measuring the next workload.
    size_t nursery_max;

    /// DREAM_GC_PAR_MIN: bytes of work under which a minor collection is not
    /// divided across the pool; a major's floor is four times this. Zero puts
    /// every collection through the parallel path however small it is, which
    /// is how the race detector gets to see the parallel collector on a
    /// program small enough to run under it.
    size_t gc_par_min;

    /// DREAM_GC_THREADS: the most threads one collection may use, the
    /// collecting one included. Zero means unset -- `GcPool` then picks, and
    /// the reasoning for what it picks lives there rather than here.
    unsigned gc_threads;

    /// DREAM_GC_CONCURRENT: 1 forces every major's marking through the
    /// concurrent path (the race detector's door in), 0 switches it off, and
    /// -1 -- the default -- lets the size of the heap decide.
    int gc_concurrent;

    /// DREAM_GC_TRACE: report every collection on stderr as it happens.
    /// `--stats` gives the totals; this gives the shape of them, which is what
    /// says whether a pause is one long collection or forty short ones.
    bool gc_trace;

    /// DREAM_VERIFY_HEAP: re-walk the whole graph after every collection.
    /// Slow, and meant for chasing a reference the collector could not see or
    /// could not fix -- which is a class of bug that never faults where the
    /// mistake is. It is a runtime branch and not a build option because it
    /// used to be one: the walk was compiled out unless the build defined
    /// `DREAM_DEBUG` or `DREAM_VERIFY_HEAP`, which no build here ever did, so
    /// the variable did nothing and `just test-heap` verified nothing. A branch
    /// per collection is not a cost worth a switch that is off by accident.
    bool verify_heap;

    // --- Tracing and diagnosis -------------------------------------------

    /// DREAM_PROBE_THUNK: count suspensions by the kind of node suspended, and
    /// print the table with `--stats`. That is the question `--stats` alone
    /// cannot answer: it says how much of a program's allocation is thunks,
    /// and this says what they are thunks *of*.
    bool probe_thunk;

    /// DREAM_IO_TRACE: narrate every IO native on stderr.
    bool io_trace;

    /// DREAM_STUCK_SECONDS: report what every process is doing if the whole
    /// system goes this long without spending a single reduction. Zero is off.
    /// See `Scheduler::wait_for_all` for why the runtime cannot just decide
    /// this for itself.
    double stuck_seconds;
};

/// The one copy, filled before `main` runs.
///
/// Not `const`, and for one reason: the unit tests force a knob that no
/// ordinary run would set -- `test_concurrent_mark_period` needs the concurrent
/// path taken on a heap far too small to earn it. Writing the field is what
/// they do, which is both clearer and more reliable than `setenv` was, since
/// that only worked while no collection had yet read the variable. Nothing in
/// the library itself writes it.
extern EnvConfig g_env;

}  // namespace dream
