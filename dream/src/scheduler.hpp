// Scheduling.
//
// Processes are multiplexed onto a small pool of OS threads. Each worker owns
// a run queue and steals from its neighbours when it goes hungry. A process
// runs for a fixed number of reductions and is then put back on the queue,
// whatever it was in the middle of -- the same preemption model as BEAM, and
// the reason a tight loop in one process cannot starve the others.

#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <vector>

#include "process.hpp"
#include "runtime.hpp"

namespace dream {

class Scheduler {
public:
    enum class JoinState { Ready, Failed, Blocked };

    /// The reduction budget one process gets before it is rescheduled.
    static constexpr int64_t REDUCTIONS_PER_SLICE = 4000;

    Scheduler(Runtime& rt, unsigned worker_count);
    ~Scheduler();

    /// How many workers to run when nobody said, which is not one per core.
    ///
    /// A worker with nothing to do is not free: it wakes every 500us to look
    /// for work, and the looking costs atomics on lines the one worker that
    /// *is* running the program keeps writing. A program with a single process
    /// -- which is every build, every compile and most scripts -- therefore
    /// pays for every core the machine happens to have. Measured on a 24-core
    /// machine, a self-compile takes 2.75 s with four workers and 3.03 s with
    /// twenty-four; the curve is flat between two and eight and climbs after.
    ///
    /// Eight is the same compromise `GcPool` makes, for the same reason and
    /// with the same escape hatch: `-j` is how a program that really does have
    /// twenty-four runnable processes asks for twenty-four workers.
    static unsigned default_workers();
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    void start();
    /// Ask the workers to stop and join them.
    void stop();

    /// Register a new process. It is not runnable until `enqueue`.
    std::shared_ptr<Process> create_process();
    void enqueue(const std::shared_ptr<Process>& p);

    /// Write a snapshot of every process and open handle to stderr.
    ///
    /// The same report `vm.dump!` produces, reachable from C++ so that a
    /// program which has stopped making progress can still be asked what it is
    /// doing -- at that point no Dream code can run to ask on its own.
    void dump(const char* why) const;

    /// Block until every process has finished, or until deadlock is detected.
    /// Returns false when nothing could run but processes were still alive.
    bool wait_for_all();

    // --- process-facing operations ---
    uint64_t spawn_from(Process& parent, Value work);
    bool send(Process& sender, uint64_t target, Value message);
    bool receive(Process& p, Value* out);
    JoinState join(Process& p, uint64_t target, Value* out);

    /// Make a parked process runnable again. Public because the IO poller
    /// calls it from its own thread when a descriptor becomes ready, which is
    /// the same handshake a message arriving uses.
    void wake(uint64_t pid);

    /// A process is waiting on something outside the scheduler -- a descriptor
    /// the poller is watching. Such a process is not deadlocked even though
    /// nothing in the system can run: the kernel still owes it an answer.
    void note_io_wait(bool waiting) {
        if (waiting) {
            io_waiters_.fetch_add(1, std::memory_order_relaxed);
        } else {
            io_waiters_.fetch_sub(1, std::memory_order_relaxed);
        }
    }
    size_t io_waiters() const { return io_waiters_.load(std::memory_order_relaxed); }

    // --- introspection, for `std.vm` ---
    size_t live() const { return live_.load(std::memory_order_relaxed); }
    size_t runnable() const { return runnable_.load(std::memory_order_relaxed); }
    unsigned idle_workers() const { return idle_workers_.load(std::memory_order_relaxed); }
    /// How many processes are sitting on run queues right now.
    size_t queued() const;

    unsigned worker_count() const { return unsigned(workers_.size()); }
    /// Errors raised by processes that nobody was waiting on. A failure that
    /// reaches a joiner is that joiner's business, not the runtime's, so it is
    /// dropped from this list when it is delivered.
    std::vector<std::string> take_failures();
    uint64_t total_reductions() const { return total_reductions_.load(); }
    bool deadlocked() const { return deadlocked_.load(); }

private:
    struct Worker {
        std::mutex mutex;
        std::deque<std::shared_ptr<Process>> queue;
        std::thread thread;
    };

    void worker_loop(unsigned index);
    std::shared_ptr<Process> take_local(unsigned index);
    std::shared_ptr<Process> steal(unsigned thief);
    void run_slice(const std::shared_ptr<Process>& p, unsigned index);
    void requeue(unsigned index, const std::shared_ptr<Process>& p);
    void finish(const std::shared_ptr<Process>& p);
    void note_idle(bool idle);

    Runtime& rt_;
    std::vector<std::unique_ptr<Worker>> workers_;
    std::atomic<unsigned> next_worker_{0};
    std::atomic<bool> running_{false};

    /// Processes that exist and have not finished.
    std::atomic<size_t> live_{0};
    /// Processes sitting on a run queue or currently executing.
    std::atomic<size_t> runnable_{0};
    /// Processes that are queued **or running** -- one counter, deliberately.
    ///
    /// This is what the deadlock check asks about, and it is one atomic rather
    /// than two because the question has to be answered without observing a
    /// half-finished transition. The previous answer was "walk every queue,
    /// then read `runnable_`", and it has a race that predates the counter:
    /// a worker re-enqueueing its process pushes it onto a queue and only then
    /// decrements `runnable_`, so a walk that has already passed that queue can
    /// go on to read `runnable_` after the decrement and conclude that a
    /// perfectly healthy program is deadlocked. It needs all workers idle to
    /// fire, which is why it stayed hidden while the default was one worker per
    /// core and surfaced as soon as that became eight.
    ///
    /// A process joins this count when it is enqueued and leaves it when its
    /// slice ends without re-enqueueing -- so a slice that hands the process
    /// straight back increments before it decrements and the count never dips.
    /// Parking is a decrement, which is correct: a parked process is waiting
    /// for a `send!` from someone still counted here, or for the poller, which
    /// `io_waiters_` covers.
    std::atomic<size_t> active_{0};
    /// Processes sitting on some worker's queue, waiting to be picked up.
    ///
    /// A hint for `steal`, and deliberately nothing more. It lets an idle
    /// worker skip the walk over every other worker's queue -- one mutex
    /// acquisition apiece, paid by every idle worker every 500us, which on a
    /// machine with many cores running a program with one process is
    /// O(workers^2) lock operations per poll, all of it contending with the
    /// single worker actually running the program.
    ///
    /// It carries no correctness: reading it stale costs at most a missed
    /// steal, which the 500us retry and `enqueue`'s notify both cover, and
    /// which the walk itself could already do by passing a victim just before
    /// it was given work. The deadlock check next to it needs a stronger
    /// answer than this and keeps its walk -- see `worker_loop` for why.
    std::atomic<size_t> queued_{0};
    std::atomic<unsigned> idle_workers_{0};
    /// Processes parked on a descriptor rather than on another process.
    std::atomic<size_t> io_waiters_{0};
    std::atomic<uint64_t> total_reductions_{0};
    std::atomic<bool> deadlocked_{false};

    std::mutex failures_mutex_;
    std::unordered_map<uint64_t, std::string> failures_;

    std::mutex idle_mutex_;
    std::condition_variable work_cv_;
    std::condition_variable done_cv_;
    /// Wake `wait_for_all` after changing what it waits on. See the definition.
    void notify_done();
};

}  // namespace dream
