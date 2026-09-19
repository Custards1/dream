// A green process: the unit of concurrency, scheduling, and garbage collection.
//
// A process is a small heap plus two explicit stacks. Nothing about its state
// lives on the C++ stack, which is what lets a scheduler stop it between any
// two reductions, resume it on a different OS thread, collect its heap with
// precise roots, and keep hundreds of thousands of them alive at once. That is
// the same bargain BEAM makes, and it is why the interpreter is written as a
// state machine rather than as a recursive `eval`.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "heap.hpp"
#include "image.hpp"
#include "value.hpp"

namespace dream {

class Runtime;

// ---------------------------------------------------------------------------
// Continuations
// ---------------------------------------------------------------------------

enum class ContKind : uint8_t {
    Halt,        // nothing left to do; the result is the process's answer
    UpdateThunk, // v1 = the thunk being forced; overwrite it with the result
    ApplyTo,     // a = argc; the arguments are the top `argc` of the value stack
    IfBranch,    // a = then node, b = else node, v1 = frame
    BinRight,    // a = opcode, b = right-hand node, v1 = frame
    BinFinish,   // a = opcode, v1 = the already-forced left-hand value
    LogicRight,  // a = opcode (And/Or), b = right-hand node, v1 = frame
    UnaryFinish, // a = opcode
    BlockNext,   // a = kids offset, b = count, c = next index, v1 = frame
    Catch,       // a = handler node, b = slot, c = value-stack depth, v1 = frame
    FieldOf,     // a = string index of the member name
    NativeArg,   // a = stack base, b = argc, c = index just forced, v1 = callee
    NativeArgs,  // a = the call node, b = stack base, c = index just evaluated,
                 // v1 = frame; the arguments filled in so far sit at the base
    NativeRetry, // a = stack base, b = argc, v1 = callee; re-invoke after a block
    MapEntry,    // a = stack base, b = pair count, c = index, v1 = the map
    IndexKey,    // b = the get/set node, v1 = frame; the container is the result
    IndexApply,  // b = the get/set node, v1 = frame; the container is on the value stack
    GetWalk,     // a = cells still to step over, b = the get node, c = the index, v1 = frame
    SetWalk,     // a = cells still to step over, b = the set node, c = the index,
                 // v1 = frame; the cells stepped over so far are on the value stack
};

struct Cont {
    ContKind kind;
    uint8_t pad0;
    uint16_t pad1;
    uint32_t a;
    uint32_t b;
    uint32_t c;
    Value v1;
};

/// The continuation stack: the machine's "what to do when this part is done".
///
/// This was a `std::vector<Cont>`, and pushing one was **6.5% of a
/// self-compile** -- more than `step_return`, and second only to the dispatch
/// switch itself. Not the growth, which amortizes to nothing against a stack
/// that is pushed and popped rather than grown: the *call*. `step_eval`
/// pushes a continuation from some thirty places, and at that many sites GCC
/// stops inlining `emplace_back` and emits an outlined clone, so every
/// continuation the machine pushed paid a function call and a capacity check
/// that no caller could hoist. A `Cont` is twenty-four bytes of POD; pushing
/// one is four stores, and it should cost four stores.
///
/// So the fast path is written out and nothing else is: `grow` is the only
/// thing out of line, and it is reached once per doubling. The type is
/// deliberately not a general container -- it has exactly the operations the
/// machine performs, which is why `insert_at` exists instead of iterators.
class ContStack {
public:
    ContStack() = default;
    ~ContStack() { std::free(data_); }
    ContStack(const ContStack&) = delete;
    ContStack& operator=(const ContStack&) = delete;

    /// `always_inline` because the heuristics say no and they are wrong here.
    /// `step_eval` is one switch over every opcode and pushes a continuation
    /// from some thirty of its arms, which puts it far past the inliner's
    /// growth ceiling -- so GCC outlines even this, and a push stayed a call
    /// costing 6% of a self-compile after it stopped being a `std::vector`.
    /// The body is a compare and two stores; it is smaller than its own call
    /// sequence.
    __attribute__((always_inline)) inline void push_back(const Cont& c) {
        if (size_ == cap_) grow();
        data_[size_++] = c;
    }

    /// Splice one in below the work already on the stack. Rare -- only a retry
    /// under a suspended nested force -- and O(n) on purpose, because making
    /// the common push cheaper is what this class is for.
    void insert_at(size_t i, const Cont& c) {
        if (size_ == cap_) grow();
        std::memmove(data_ + i + 1, data_ + i, (size_ - i) * sizeof(Cont));
        data_[i] = c;
        ++size_;
    }

    void pop_back() { --size_; }
    void clear() { size_ = 0; }
    Cont& back() { return data_[size_ - 1]; }
    const Cont& back() const { return data_[size_ - 1]; }
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    void reserve(size_t n) { if (n > cap_) regrow(n); }

    Cont* begin() { return data_; }
    Cont* end() { return data_ + size_; }

private:
    void grow() { regrow(cap_ ? cap_ * 2 : 64); }
    void regrow(size_t want);

    Cont* data_ = nullptr;
    size_t size_ = 0;
    size_t cap_ = 0;
};

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

/// A message in flight. It carries its own heap: the sender deep-copies the
/// value into it, and the receiver copies it out into its own heap. Neither
/// side ever touches the other's heap, so a send needs no heap lock -- only
/// the mailbox queue is synchronized.
struct Message {
    std::unique_ptr<Heap> heap;
    Value value = UNIT;
    uint64_t from_pid = 0;
};

class Mailbox {
public:
    void push(std::unique_ptr<Message> m);
    std::unique_ptr<Message> pop();
    bool empty() const;
    size_t size() const;

private:
    mutable std::mutex mutex_;
    std::deque<std::unique_ptr<Message>> queue_;
};

// ---------------------------------------------------------------------------
// Process
// ---------------------------------------------------------------------------

enum class Mode : uint8_t {
    Eval,    // reduce `node` in `frame` to weak head normal form
    Return,  // `result` is in WHNF; hand it to the top continuation
    Raise,   // `result` is an error; unwind to the nearest catch
    Halted,
};

enum class ProcStatus : uint8_t {
    Runnable,
    Running,
    Waiting,   // blocked in `recv!`, `join!`, or on a descriptor
    Finished,
    Failed,
};

/// What a parked process is parked on.
///
/// The status alone says a process is stuck but not why, and "waiting for a
/// message that will never come" and "waiting for a socket the kernel has not
/// answered" look identical from outside. `std.vm` reports this so the
/// difference is visible without a debugger.
enum class WaitReason : uint8_t {
    None,
    Message,   // `recv!`
    Join,      // `join!`
    Io,        // a descriptor the poller is watching
};

const char* wait_reason_name(WaitReason r);
const char* mode_name(Mode m);
const char* cont_kind_name(ContKind k);

class Process final : public RootSource {
public:
    Process(Runtime& rt, uint64_t id);
    ~Process() override;

    uint64_t id() const { return id_; }

    /// The handle of a `net.connect!` that has been issued but not yet
    /// finished, or -1. A blocking native is re-entered from the top when its
    /// process is woken, and `connect` is the one operation where the second
    /// call must ask how the first went rather than start again.
    int64_t io_pending = -1;

    /// The `std.os.exec!` whose child is still running, or -1. Same reason as
    /// `io_pending`: a blocking native is re-entered from the top, and this is
    /// how the second entry knows which child it was waiting for.
    int64_t os_pending = -1;

    /// Set when a nested force (`force_whnf`) was suspended because a blocking
    /// native inside it asked to park. The force's continuations are left in
    /// place to be resumed; `force_resume_at` is the index in `conts` where
    /// the outer operation's retry must be spliced in, *below* them.
    bool force_blocked = false;
    size_t force_resume_at = 0;

    /// Whether a nested force may collect, and what it costs to say so.
    ///
    /// `force_whnf` runs the machine underneath a native, on the C++ stack, and
    /// for most of the VM's life it could not collect there: a native holding a
    /// raw `Value` across the call would have had its object promoted out from
    /// under it. The cost of that rule was not a bug but a *size* -- a native
    /// that walks a long lazy list runs unbounded Dream work with the heap
    /// pinned, and one `str.concat_all` over the image was measured allocating
    /// 354 MB of nursery that no safepoint could reach. 87% of it was garbage.
    ///
    /// So the rule is now earned rather than assumed. Collection is legal in a
    /// nested force exactly when every C++ frame between it and `run_process`
    /// has said its locals can survive one -- which means keeping them on
    /// `stack` or in `pins` and re-reading, never in a C++ local. A frame says
    /// so with `VouchesForGc` (interp.hpp); `force_pins` counts the frames that
    /// have *not*, and a collection needs it at zero.
    ///
    /// `force_vouched` is how the claim reaches `force_whnf`: the guard sets
    /// it, the force consumes it, and clears it for everything it goes on to
    /// reach -- because a vouch is about the frame that made it and says
    /// nothing about the natives that run below.
    bool force_vouched = false;
    uint32_t force_pins = 0;

    /// Nested `force_whnf` loops currently on the process's machine stack.
    ///

    /// Interpreted code never nests them deeply -- each force is a heap
    /// continuation and the loop returns between links -- so a deep count is a
    /// *compiled* signature: a compiled body forces a slot by calling, and
    /// `load_slot -> force -> dream_rt_force -> force_whnf` runs a whole nested
    /// machine, which re-enters another compiled body, which forces again. One
    /// real C++ frame per link of a lazy chain, against a fixed machine stack.
    /// That was the SIGSEGV under "Fixed: compiled code forcing a long thunk
    /// chain crashed" in CLAUDE.md; `enter_function` declines the compiled tier
    /// past `kMaxForceNestForCompiled`, and the interpreter, whose recursion is
    /// heap continuations and so has a limit it can check, finishes the chain.
    uint32_t force_nest = 0;

    /// Values a C++ frame is holding across a call that can collect.
    ///
    /// The interpreter keeps no machine state on the C++ stack, which is the
    /// whole reason the root set is a list rather than a stack walk. The
    /// exceptions are few and all of them are here: the callee a native call
    /// must still name when the native parks, and the spare arguments
    /// `do_apply` sets aside when a native is over-applied. The collector
    /// visits this like any other root and rewrites what it finds, so the
    /// frame reads its value back out rather than trusting the copy it kept.
    std::vector<Value> pins;

    /// Bytes already charged to some function by the allocation profile.
    ///
    /// A step's allocation is measured as the heap's total before and after,
    /// and a step that enters a native which forces runs whole machine loops
    /// underneath itself -- every one of which measures and charges the same
    /// bytes again. So each level subtracts what the levels below it have
    /// already claimed, and this is the running tally they claim into. Only
    /// touched while profiling.
    uint64_t alloc_attributed = 0;

    Runtime& runtime() { return rt_; }
    Heap& heap() { return heap_; }
    const Heap& heap() const { return heap_; }

    /// The image this process runs, cached at construction.
    ///
    /// Reaching it the honest way -- `p.runtime().image()` -- is two dependent
    /// loads before the one that reads the node, and the machine asks for a
    /// node on every step. An image is loaded before any process exists and is
    /// never replaced, so the indirection buys nothing.
    const Image* code = nullptr;

    // --- machine state ---
    Mode mode = Mode::Halted;
    uint32_t node = NO_NODE;
    Value frame = UNIT;
    Value result = UNIT;

    std::vector<Value> stack;
    ContStack conts;

    // --- scheduling ---
    std::atomic<ProcStatus> status{ProcStatus::Runnable};
    int64_t reductions = 0;
    uint64_t total_reductions = 0;
    /// The budget `run_process` was given, so a nested force can re-arm it.
    int64_t slice = 4000;
    /// The slice ran out while a nested force was running.
    ///
    /// A nested force cannot hand the process back -- the natives above it are
    /// waiting on the C++ stack for a value -- so the budget is not something
    /// it can spend down to zero and stop at. Left alone it goes arbitrarily
    /// negative, and then everything that asks "is there budget left?" is told
    /// "no" for ever. That is not a counter being untidy: a JIT-compiled loop
    /// reached through `strict!` yielded on *every* iteration for it, giving
    /// back the heap frame the compilation exists to remove and re-entering
    /// the interpreter once per element. So the nested loop re-arms the budget
    /// and sets this instead, and `run_process` ends the slice the moment the
    /// force it was under returns.
    bool slice_spent = false;

    Mailbox mailbox;

    /// One cache slot per image global. Globals are per-process, not shared:
    /// a memoized top-level value lives in whichever heap forced it, and heaps
    /// are private. Code is shared; data is not -- as in BEAM.
    std::vector<Value> globals;

    /// One slot per host module member, indexed by `ModuleDef::member_base + i`.
    ///
    /// `native.tail xs` used to build two objects before it could call
    /// anything: a string for the member's name and a `NativeObj` around the
    /// function pointer. Both are decided by which member it is and nothing
    /// else, so the same pair was allocated again at every call -- in a fold
    /// over ten million elements, forty million objects that were all equal.
    /// The value belongs to this heap, so the cache does too, and it is a root.
    std::vector<Value> native_cache;

    /// One slot per image string constant: the `StrObj` this process hands out
    /// for it. A literal is immutable and has no identity a program can
    /// observe, so one copy per process does what a fresh copy per evaluation
    /// did, without the allocation. A root, like the others here.
    ///
    /// Per-process, and sized by the whole program's string table, which is
    /// the same bargain `globals` above makes: a table this process may only
    /// use a corner of, in exchange for the lookup being an index. It is
    /// allocated on the first literal a process reaches, so a process that
    /// evaluates none pays nothing, and the table is small even for a large
    /// program -- the compiler's own image has 1752 strings against 1118
    /// globals. If a workload ever wants hundreds of thousands of processes
    /// each touching a corner of a huge string table, this and `globals` want
    /// bounding together.
    std::vector<Value> string_cache;

    /// The same for float constants, which are boxed and so cost an
    /// allocation each: a loop over `1.0 / x` allocated one per iteration for
    /// a value the image already held.
    std::vector<Value> float_cache;

    /// Set once the process stops; `failed` distinguishes a raised error.
    Value exit_value = UNIT;
    bool failed = false;
    std::string failure_text;

    /// Processes blocked in `join!` on this one. The mutex also guards the
    /// transition to a finished status, so a joiner cannot register itself
    /// against a process that has already exited.
    std::mutex waiters_mutex;
    std::vector<uint64_t> waiters;

    /// Guards the parking handshake below. Only the worker currently running a
    /// process may change its machine state, so a blocking builtin does not
    /// park the process itself -- it asks to be parked, and the worker does it
    /// once it has stopped touching the process. Anyone waking a process that
    /// is still running just leaves a note.
    std::mutex sched_mutex;
    /// Set by a blocking builtin: "park me when this slice ends".
    bool park_requested = false;
    /// Set by a waker that found the process still running.
    bool wake_pending = false;
    /// Why this process asked to be parked. Written by the blocking native
    /// alongside `park_requested`, and read by `std.vm` from another thread.
    std::atomic<WaitReason> wait_reason{WaitReason::None};
    /// The descriptor being waited on, when `wait_reason` is `Io`.
    std::atomic<int> wait_fd{-1};

    bool is_done() const {
        ProcStatus s = status.load(std::memory_order_relaxed);
        return s == ProcStatus::Finished || s == ProcStatus::Failed;
    }

    /// Collect if the heap has grown past its threshold. Only legal at a
    /// safepoint, where every live value is reachable from the stacks.
    void maybe_collect();

    void visit_roots(Heap& heap) override;

    // Convenience for natives and the interpreter.
    Value make_error_value(const char* kind, const char* message);
    [[noreturn]] void unreachable(const char* what);

private:
    Runtime& rt_;
    uint64_t id_;
    Heap heap_;
};

}  // namespace dream
