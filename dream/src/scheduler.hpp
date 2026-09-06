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
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    void start();
    /// Ask the workers to stop and join them.
    void stop();

    /// Register a new process. It is not runnable until `enqueue`.
    std::shared_ptr<Process> create_process();
    void enqueue(const std::shared_ptr<Process>& p);

    /// Block until every process has finished, or until deadlock is detected.
    /// Returns false when nothing could run but processes were still alive.
    bool wait_for_all();

    // --- process-facing operations ---
    uint64_t spawn_from(Process& parent, Value work);
    bool send(Process& sender, uint64_t target, Value message);
    bool receive(Process& p, Value* out);
    JoinState join(Process& p, uint64_t target, Value* out);

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
    void run_slice(const std::shared_ptr<Process>& p);
    void finish(const std::shared_ptr<Process>& p);
    void wake(uint64_t pid);
    void note_idle(bool idle);

    Runtime& rt_;
    std::vector<std::unique_ptr<Worker>> workers_;
    std::atomic<unsigned> next_worker_{0};
    std::atomic<bool> running_{false};

    /// Processes that exist and have not finished.
    std::atomic<size_t> live_{0};
    /// Processes sitting on a run queue or currently executing.
    std::atomic<size_t> runnable_{0};
    std::atomic<unsigned> idle_workers_{0};
    std::atomic<uint64_t> total_reductions_{0};
    std::atomic<bool> deadlocked_{false};

    std::mutex failures_mutex_;
    std::unordered_map<uint64_t, std::string> failures_;

    std::mutex idle_mutex_;
    std::condition_variable work_cv_;
    std::condition_variable done_cv_;
};

}  // namespace dream
