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
        case ContKind::NativeRetry: return "native_retry";
        case ContKind::MapEntry: return "map_entry";
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
}

Process::~Process() = default;

void Process::maybe_collect() {
    // An old space past its threshold wants the full collection; a nursery
    // past its mark just wants the minor one. A major handles the minor's job
    // too (it promotes everything reachable), so when both are due the full
    // collection wins.
    if (heap_.major_due())
        heap_.major_collect(*this);
    else
        heap_.minor_collect(*this);
}

void Process::visit_roots(Heap& heap) {
    // Everything the machine can still reach lives in these five places. That
    // is the whole payoff of keeping process state out of the C++ stack: the
    // root set is a list, not a stack walk.
    heap.forward(&frame);
    heap.forward(&result);
    heap.forward(&exit_value);
    for (Value& v : stack) heap.forward(&v);
    for (Cont& c : conts) heap.forward(&c.v1);
    for (Value& g : globals) {
        if (g != NIL_SLOT) heap.forward(&g);
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
