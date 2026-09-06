#include "process.hpp"

#include "interp.hpp"
#include "runtime.hpp"

namespace dream {

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

void Process::maybe_collect() { heap_.collect(*this); }

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
    std::fprintf(stderr, "mindv2: internal error: %s\n", what);
    std::abort();
}

}  // namespace dream
