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
    NativeRetry, // a = stack base, b = argc, v1 = callee; re-invoke after a block
    MapEntry,    // a = stack base, b = pair count, c = index, v1 = the map
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

    Runtime& runtime() { return rt_; }
    Heap& heap() { return heap_; }

    // --- machine state ---
    Mode mode = Mode::Halted;
    uint32_t node = NO_NODE;
    Value frame = UNIT;
    Value result = UNIT;

    std::vector<Value> stack;
    std::vector<Cont> conts;

    // --- scheduling ---
    std::atomic<ProcStatus> status{ProcStatus::Runnable};
    int64_t reductions = 0;
    uint64_t total_reductions = 0;

    Mailbox mailbox;

    /// One cache slot per image global. Globals are per-process, not shared:
    /// a memoized top-level value lives in whichever heap forced it, and heaps
    /// are private. Code is shared; data is not -- as in BEAM.
    std::vector<Value> globals;

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
