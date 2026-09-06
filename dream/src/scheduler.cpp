#include "scheduler.hpp"

#include <chrono>

#include "builtins.hpp"
#include "interp.hpp"

namespace dream {

using namespace std::chrono_literals;

Scheduler::Scheduler(Runtime& rt, unsigned worker_count) : rt_(rt) {
    if (worker_count == 0) worker_count = 1;
    for (unsigned i = 0; i < worker_count; ++i) {
        workers_.push_back(std::make_unique<Worker>());
    }
    rt_.set_scheduler(this);
}

Scheduler::~Scheduler() { stop(); }

void Scheduler::start() {
    if (running_.exchange(true)) return;
    for (unsigned i = 0; i < workers_.size(); ++i) {
        workers_[i]->thread = std::thread([this, i] { worker_loop(i); });
    }
}

void Scheduler::stop() {
    if (!running_.exchange(false)) return;
    work_cv_.notify_all();
    for (auto& w : workers_) {
        if (w->thread.joinable()) w->thread.join();
    }
}

std::shared_ptr<Process> Scheduler::create_process() {
    auto p = rt_.spawn_process();
    live_.fetch_add(1, std::memory_order_relaxed);
    return p;
}

void Scheduler::enqueue(const std::shared_ptr<Process>& p) {
    p->status.store(ProcStatus::Runnable, std::memory_order_relaxed);
    // Round-robin placement. A process that blocks and wakes may land on a
    // different worker than it started on, which is fine: nothing in a process
    // is tied to a thread.
    unsigned i = next_worker_.fetch_add(1, std::memory_order_relaxed) % workers_.size();
    {
        std::lock_guard<std::mutex> g(workers_[i]->mutex);
        workers_[i]->queue.push_back(p);
    }
    work_cv_.notify_one();
}

std::shared_ptr<Process> Scheduler::take_local(unsigned index) {
    Worker& w = *workers_[index];
    std::lock_guard<std::mutex> g(w.mutex);
    if (w.queue.empty()) return nullptr;
    auto p = std::move(w.queue.front());
    w.queue.pop_front();
    return p;
}

std::shared_ptr<Process> Scheduler::steal(unsigned thief) {
    // Take from the back of a victim's queue: the front is the work it is
    // about to run, and leaving that alone keeps its cache warm.
    for (unsigned n = 1; n < workers_.size(); ++n) {
        unsigned victim = (thief + n) % workers_.size();
        Worker& w = *workers_[victim];
        std::lock_guard<std::mutex> g(w.mutex);
        if (w.queue.empty()) continue;
        auto p = std::move(w.queue.back());
        w.queue.pop_back();
        return p;
    }
    return nullptr;
}

void Scheduler::note_idle(bool idle) {
    if (idle) {
        idle_workers_.fetch_add(1, std::memory_order_relaxed);
    } else {
        idle_workers_.fetch_sub(1, std::memory_order_relaxed);
    }
}

void Scheduler::worker_loop(unsigned index) {
    while (running_.load(std::memory_order_relaxed)) {
        std::shared_ptr<Process> p = take_local(index);
        if (!p) p = steal(index);

        if (!p) {
            note_idle(true);
            {
                std::unique_lock<std::mutex> lk(idle_mutex_);
                work_cv_.wait_for(lk, 500us);
            }
            note_idle(false);

            // Nothing runnable anywhere, every worker asleep, yet processes
            // remain: they are all parked on a message that will never come.
            if (live_.load() > 0 && idle_workers_.load() + 1 >= workers_.size()) {
                bool any_queued = false;
                for (auto& w : workers_) {
                    std::lock_guard<std::mutex> g(w->mutex);
                    if (!w->queue.empty()) { any_queued = true; break; }
                }
                if (!any_queued && runnable_.load() == 0) {
                    deadlocked_.store(true);
                    done_cv_.notify_all();
                }
            }
            continue;
        }

        runnable_.fetch_add(1, std::memory_order_relaxed);
        run_slice(p);
        runnable_.fetch_sub(1, std::memory_order_relaxed);
    }
}

void Scheduler::run_slice(const std::shared_ptr<Process>& p) {
    p->status.store(ProcStatus::Running, std::memory_order_relaxed);
    uint64_t before = p->total_reductions;

    run_process(*p, REDUCTIONS_PER_SLICE);

    total_reductions_.fetch_add(p->total_reductions - before, std::memory_order_relaxed);

    if (p->mode == Mode::Halted) {
        finish(p);
        return;
    }

    {
        // The parking handshake. This worker has stopped touching the machine
        // state, so it is now safe to publish that the process is parked -- and
        // to notice a wake that arrived while it was still running.
        std::lock_guard<std::mutex> g(p->sched_mutex);
        if (p->park_requested) {
            p->park_requested = false;
            if (!p->wake_pending) {
                p->status.store(ProcStatus::Waiting, std::memory_order_release);
                return;
            }
            // Woken before we could park: fall through and run again. The
            // blocking builtin re-checks its condition, so a spurious wake
            // costs one reduction and nothing else.
            p->wake_pending = false;
        }
    }

    // Budget spent mid-computation, or a wake beat the park. Put it back and
    // let something else run; the whole state is in the Process, so there is
    // nothing to save.
    enqueue(p);
}

std::vector<std::string> Scheduler::take_failures() {
    std::lock_guard<std::mutex> g(failures_mutex_);
    std::vector<std::string> out;
    out.reserve(failures_.size());
    for (auto& [pid, text] : failures_) out.push_back(text);
    failures_.clear();
    return out;
}

void Scheduler::finish(const std::shared_ptr<Process>& p) {
    std::vector<uint64_t> waiters;
    bool had_waiters = false;
    {
        std::lock_guard<std::mutex> g(p->waiters_mutex);
        p->exit_value = p->result;
        had_waiters = !p->waiters.empty();
        p->status.store(p->failed ? ProcStatus::Failed : ProcStatus::Finished,
                        std::memory_order_release);
        waiters.swap(p->waiters);
    }

    // Record the failure only when nobody is waiting for it. A joiner receives
    // the error as a value and decides what it means; reporting it as well
    // would turn a handled failure into a spurious complaint.
    if (p->failed && !had_waiters) {
        std::string text;
        stringify(*p, p->result, &text);
        std::lock_guard<std::mutex> g(failures_mutex_);
        failures_.emplace(p->id(), "process " + std::to_string(p->id()) + ": " + text);
    }

    live_.fetch_sub(1, std::memory_order_acq_rel);
    for (uint64_t id : waiters) wake(id);
    done_cv_.notify_all();
}

void Scheduler::wake(uint64_t pid) {
    auto proc = rt_.find_process(pid);
    if (!proc) return;
    std::lock_guard<std::mutex> g(proc->sched_mutex);
    if (proc->status.load(std::memory_order_relaxed) == ProcStatus::Waiting) {
        // It is parked and nobody is touching its machine state, so it is safe
        // to make it runnable. It resumes in Return mode on the NativeRetry
        // continuation the blocking builtin left behind, which calls that
        // builtin again -- this time it finds what it was waiting for.
        enqueue(proc);
        return;
    }
    // Still running, or between running and parking. Leave a note; the worker
    // checks for it before it commits to parking, so the wake cannot be lost.
    proc->wake_pending = true;
}

bool Scheduler::wait_for_all() {
    std::unique_lock<std::mutex> lk(idle_mutex_);
    done_cv_.wait(lk, [this] {
        return live_.load() == 0 || deadlocked_.load() || !running_.load();
    });
    return live_.load() == 0;
}

// ---------------------------------------------------------------------------
// Process-facing operations
// ---------------------------------------------------------------------------

uint64_t Scheduler::spawn_from(Process& parent, Value work) {
    if (!running_.load()) return 0;
    auto child = create_process();

    // The work crosses a heap boundary, so it is copied. A closure copies with
    // its captured values, which is exactly the isolation we want: the child
    // shares no mutable state with its parent.
    Value copied = Heap::copy_between(child->heap(), work);
    prime_apply(*child, copied, 0);
    enqueue(child);
    return child->id();
}

bool Scheduler::send(Process& sender, uint64_t target, Value message) {
    auto proc = rt_.find_process(target);
    if (!proc) return false;

    auto msg = std::make_unique<Message>();
    msg->heap = std::make_unique<Heap>(1024);
    msg->value = Heap::copy_between(*msg->heap, message);
    msg->from_pid = sender.id();
    proc->mailbox.push(std::move(msg));

    // Push first, then wake: a receiver that parks between those two steps is
    // still found by `wake`, because parking takes the same lock.
    wake(target);
    return true;
}

bool Scheduler::receive(Process& p, Value* out) {
    std::lock_guard<std::mutex> g(p.sched_mutex);
    auto msg = p.mailbox.pop();
    if (!msg) {
        p.park_requested = true;
        return false;
    }
    *out = Heap::copy_between(p.heap(), msg->value);
    return true;
}

Scheduler::JoinState Scheduler::join(Process& p, uint64_t target, Value* out) {
    auto proc = rt_.find_process(target);
    if (!proc) {
        // Unknown pid: either it never existed or it has been reaped.
        *out = UNIT;
        return JoinState::Ready;
    }
    if (proc.get() == &p) {
        *out = raise_error(p, well_known(rt_).error, "a process cannot join itself");
        return JoinState::Failed;
    }

    std::lock_guard<std::mutex> g(proc->waiters_mutex);
    if (proc->is_done()) {
        *out = Heap::copy_between(p.heap(), proc->exit_value);
        if (proc->failed) {
            // Delivered to a joiner, so it is handled; drop it from the list
            // the runtime reports at shutdown.
            std::lock_guard<std::mutex> gf(failures_mutex_);
            failures_.erase(target);
            return JoinState::Failed;
        }
        return JoinState::Ready;
    }
    // Register before parking, under the same lock the exiting process takes,
    // so an exit that happens right now cannot slip past us.
    proc->waiters.push_back(p.id());
    {
        std::lock_guard<std::mutex> g2(p.sched_mutex);
        p.park_requested = true;
    }
    return JoinState::Blocked;
}

}  // namespace dream
