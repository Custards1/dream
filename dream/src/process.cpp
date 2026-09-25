#include "process.hpp"

#include "interp.hpp"
#include "runtime.hpp"

namespace dream {

const char* mode_name(Mode m) {
    switch (m) {
        case Mode::Eval: return "eval";
        case Mode::Return: return "return";
        case Mode::Raise: return "raise";
        case Mode::Halted: return "halted";
    }
    return "?";
}

const char* cont_kind_name(ContKind k) {
    switch (k) {
        case ContKind::Halt: return "halt";
        case ContKind::UpdateThunk: return "update_thunk";
        case ContKind::ApplyTo: return "apply_to";
        case ContKind::IfBranch: return "if_branch";
        case ContKind::BinRight: return "bin_right";
        case ContKind::BinFinish: return "bin_finish";
        case ContKind::LogicRight: return "logic_right";
        case ContKind::UnaryFinish: return "unary_finish";
        case ContKind::BlockNext: return "block_next";
        case ContKind::Catch: return "catch";
        case ContKind::FieldOf: return "field_of";
        case ContKind::NativeArg: return "native_arg";
        case ContKind::NativeArgs: return "native_args";
        case ContKind::NativeRetry: return "native_retry";
        case ContKind::MapEntry: return "map_entry";
        case ContKind::IndexKey: return "index_key";
        case ContKind::IndexApply: return "index_apply";
        case ContKind::GetWalk: return "get_walk";
        case ContKind::SetWalk: return "set_walk";
        case ContKind::SwitchOn: return "switch_on";
        case ContKind::SwitchKey: return "switch_key";
        case ContKind::EnterRetry: return "enter_retry";
    }
    return "?";
}

const char* wait_reason_name(WaitReason r) {
    switch (r) {
        case WaitReason::Message: return "message";
        case WaitReason::Join: return "join";
        case WaitReason::Io: return "io";
        case WaitReason::None: break;
    }
    return "none";
}

// ---------------------------------------------------------------------------
// Mailbox
// ---------------------------------------------------------------------------

/// Grow the continuation stack. Out of line because it runs once per doubling
/// and the push beside it runs tens of millions of times; `Cont` is trivially
/// copyable, so this is a realloc and nothing more.
void ContStack::regrow(size_t want) {
    Cont* next = static_cast<Cont*>(std::realloc(data_, want * sizeof(Cont)));
    if (!next) {
        std::fprintf(stderr, "dream: out of memory growing the continuation stack\n");
        std::abort();
    }
    data_ = next;
    cap_ = want;
}

void Mailbox::push(std::unique_ptr<Message> m) {
    std::lock_guard<std::mutex> g(mutex_);
    queue_.push_back(std::move(m));
}

std::unique_ptr<Message> Mailbox::pop() {
    std::lock_guard<std::mutex> g(mutex_);
    if (queue_.empty()) return nullptr;
    auto m = std::move(queue_.front());
    queue_.pop_front();
    return m;
}

bool Mailbox::empty() const {
    std::lock_guard<std::mutex> g(mutex_);
    return queue_.empty();
}

size_t Mailbox::size() const {
    std::lock_guard<std::mutex> g(mutex_);
    return queue_.size();
}

// ---------------------------------------------------------------------------
// Process
// ---------------------------------------------------------------------------

Process::Process(Runtime& rt, uint64_t id) : rt_(rt), id_(id), heap_(64 * 1024) {
    stack.reserve(64);
    conts.reserve(64);
    code = rt.has_image() ? &rt.image() : nullptr;
}

Process::~Process() = default;

void Process::maybe_collect() {
    // A mark the helpers are still on is not a state to launch anything from:
    // starting a second collection while the first's helpers were walking old
    // space would be two collectors on one heap. The one legal move is to
    // finalize what the helpers did -- which also folds in whatever minor a
    // full collection would have owed, so the nursery emergency resolves here
    // too. An old space past its threshold *starts* the concurrent mark when
    // it can, and falls back to the whole-world major when the helpers are
    // busy elsewhere or the heap is too small to gain from them. A nursery
    // past its mark just gets the minor.
    if (heap_.marking()) {
        heap_.finalize_concurrent_mark(*this);
    } else if (heap_.major_due()) {
        if (!heap_.start_concurrent_mark(*this)) heap_.major_collect(*this);
    } else {
        heap_.minor_collect(*this);
    }
}

void Process::visit_roots(Heap& heap) {
    // Everything the machine can still reach lives in these few places. That
    // is the whole payoff of keeping process state out of the C++ stack: the
    // root set is a list, not a stack walk. `pins` is the one concession, and
    // it exists so that the handful of C++ frames that genuinely do hold a
    // `Value` across a collection can be written down rather than reasoned
    // about -- see `Process::pins`.
    heap.forward(&frame);
    heap.forward(&result);
    heap.forward(&exit_value);
    for (Value& v : stack) heap.forward(&v);
    // Usually empty: a native call pins its callee only when that callee is a
    // host object rather than a builtin immediate, and nothing else pins at all
    // unless a native has been over-applied.
    for (Value& v : pins) heap.forward(&v);
    for (Cont& c : conts) heap.forward(&c.v1);
    for (Value& g : globals) {
        if (g != NIL_SLOT) heap.forward(&g);
    }
    for (Value& v : native_cache) {
        if (v != NIL_SLOT) heap.forward(&v);
    }
    for (Value& v : string_cache) {
        if (v != NIL_SLOT) heap.forward(&v);
    }
    for (Value& v : float_cache) {
        if (v != NIL_SLOT) heap.forward(&v);
    }
}

Value Process::make_error_value(const char* kind, const char* message) {
    uint32_t atom = rt_.intern_atom(kind);
    Value msg = heap_.make_string(message, uint32_t(std::char_traits<char>::length(message)));
    return heap_.make_error(make_atom(atom), msg);
}

void Process::unreachable(const char* what) {
    std::fprintf(stderr, "dream: internal error: %s\n", what);
    std::abort();
}

}  // namespace dream
