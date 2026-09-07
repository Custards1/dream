// The operating system: arguments, environment, directories, subprocesses.
//
// The interesting one is `exec!`. Running a child to completion means waiting
// for it, and a Dream process must never wait on a worker thread -- that would
// block every other process queued behind it. So `exec!` hands the whole job
// (spawn, read both pipes, reap) to a helper thread and parks the calling
// process through the same handshake `recv!` and the IO natives use. The
// helper wakes it when the child has exited.
//
// A helper thread rather than the epoll poller because a child process is not
// one descriptor but three things at once -- two pipes and an exit status --
// and driving that from a native that is re-entered from the top on every
// wake-up would mean keeping the partially-read output somewhere. One thread
// per running child is the honest cost, and a build tool runs a handful.

#include "os.hpp"

#include <algorithm>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

#include "builtins.hpp"
#include "interp.hpp"
#include "process.hpp"
#include "scheduler.hpp"

extern char** environ;

namespace dream {
namespace {

Value os_error(Process& p, const char* kind, const std::string& message) {
    return raise_error(p, p.runtime().intern_atom(kind), message);
}

NativeResult fail(Process& p, const char* kind, const std::string& message) {
    return NativeResult::raise(os_error(p, kind, message));
}

std::string string_arg(Value v) {
    v = resolve(v);
    if (!is_obj(v, ObjType::Str)) return std::string();
    auto* s = static_cast<StrObj*>(as_obj(v));
    return std::string(s->data(), s->len);
}

bool is_string(Value v) { return is_obj(resolve(v), ObjType::Str); }

Value text(Process& p, const std::string& s) {
    return p.heap().make_string(s.data(), uint32_t(s.size()));
}

/// A list of strings, built back to front so it comes out in order.
Value string_list(Process& p, const std::vector<std::string>& items) {
    Value list = NIL;
    for (size_t i = items.size(); i-- > 0;) {
        list = p.heap().make_cons(text(p, items[i]), list);
    }
    return list;
}

/// Read a Dream list of strings into a vector. Returns false if any element is
/// not a string, which is the only way `exec!` can be misused.
bool read_string_list(Process& p, Value list, std::vector<std::string>* out) {
    Value cur = list;
    for (;;) {
        Value w;
        if (!force_whnf(p, cur, &w)) return false;
        if (is_nil(w)) return true;
        if (!is_obj(w, ObjType::Cons)) return false;
        auto* c = static_cast<ConsObj*>(as_obj(w));
        Value head;
        if (!force_whnf(p, c->head, &head)) return false;
        if (!is_obj(head, ObjType::Str)) return false;
        auto* s = static_cast<StrObj*>(as_obj(head));
        out->emplace_back(s->data(), s->len);
        cur = c->tail;
    }
}

// ---------------------------------------------------------------------------
// Subprocesses
// ---------------------------------------------------------------------------

/// One `exec!` in flight. The Dream process holds only the id; the job itself
/// outlives the native call, because the native returns as soon as it has
/// parked and is entered again from the top when it is woken.
struct Job {
    std::atomic<bool> done{false};
    int code = -1;
    bool started = false;
    bool timed_out = false;
    std::string error;   // non-empty when the child could not be started
    std::string out;
    std::string err;
    std::thread worker;

    /// 0 means "wait as long as it takes". Anything else is a deadline after
    /// which the child is killed -- worth having because a build tool spends
    /// its life running other people's programs, and `git clone` against an
    /// unreachable host will otherwise wait for ever.
    int64_t timeout_ms = 0;
    std::mutex finish_mutex;
    std::condition_variable finished;
    bool child_done = false;
};

class Jobs {
public:
    static Jobs& get() {
        static Jobs j;
        return j;
    }

    int64_t add(std::shared_ptr<Job> job) {
        std::lock_guard<std::mutex> g(mutex_);
        int64_t id = next_++;
        jobs_[id] = std::move(job);
        return id;
    }

    std::shared_ptr<Job> find(int64_t id) {
        std::lock_guard<std::mutex> g(mutex_);
        auto it = jobs_.find(id);
        return it == jobs_.end() ? nullptr : it->second;
    }

    void erase(int64_t id) {
        std::shared_ptr<Job> job;
        {
            std::lock_guard<std::mutex> g(mutex_);
            auto it = jobs_.find(id);
            if (it == jobs_.end()) return;
            job = it->second;
            jobs_.erase(it);
        }
        if (job->worker.joinable()) job->worker.join();
    }

    /// Wait for every child still running, at shutdown.
    void drain() {
        std::vector<std::shared_ptr<Job>> all;
        {
            std::lock_guard<std::mutex> g(mutex_);
            for (auto& [id, job] : jobs_) all.push_back(job);
            jobs_.clear();
        }
        for (auto& job : all) {
            if (job->worker.joinable()) job->worker.join();
        }
    }

private:
    std::mutex mutex_;
    std::unordered_map<int64_t, std::shared_ptr<Job>> jobs_;
    int64_t next_ = 1;
};

/// Read everything from `fd` until end of file, then close it.
std::string slurp(int fd) {
    std::string out;
    char buf[4096];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof buf);
        if (n > 0) { out.append(buf, size_t(n)); continue; }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    ::close(fd);
    return out;
}

/// Spawn `argv`, collect both streams, reap, and wake `pid`.
void run_child(std::shared_ptr<Job> job, std::vector<std::string> argv,
               uint64_t pid, Scheduler* sched) {
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    auto finish = [&] {
        job->done.store(true, std::memory_order_release);
        if (sched) {
            // Wake first, then stop counting this process as an IO waiter.
            //
            // The order matters. `io_waiters` is what tells the deadlock check
            // that a parked process is owed a wake-up from outside the
            // scheduler. Clearing it first opens a window in which the process
            // is neither counted as waiting nor yet runnable, and an idle
            // worker looking in that window sees no runnable process, an empty
            // queue and no IO waiters -- and declares a deadlock that is not
            // one. With one worker the check runs every 500us and hit that
            // window every time.
            //
            // This way round the count is merely released a moment late, which
            // can only delay a real deadlock report, never invent one.
            sched->wake(pid);
            sched->note_io_wait(false);
        }
    };

    if (::pipe(out_pipe) != 0 || ::pipe(err_pipe) != 0) {
        job->error = std::string("cannot create a pipe: ") + std::strerror(errno);
        for (int fd : {out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1]}) {
            if (fd >= 0) ::close(fd);
        }
        finish();
        return;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], 1);
    posix_spawn_file_actions_adddup2(&actions, err_pipe[1], 2);
    posix_spawn_file_actions_addclose(&actions, out_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, err_pipe[0]);

    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (std::string& a : argv) args.push_back(a.data());
    args.push_back(nullptr);

    pid_t child = 0;
    // `posix_spawnp`, so a bare program name is found on PATH -- `git` should
    // mean what it means in a shell.
    int rc = ::posix_spawnp(&child, args[0], &actions, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(out_pipe[1]);
    ::close(err_pipe[1]);

    if (rc != 0) {
        job->error = std::string("cannot run `") + argv[0] + "`: " + std::strerror(rc);
        ::close(out_pipe[0]);
        ::close(err_pipe[0]);
        finish();
        return;
    }
    job->started = true;

    // A watchdog, when a deadline was asked for. It waits to be told the child
    // finished and kills it if that never comes; the reads below then see end
    // of file and `waitpid` reaps as usual, so the normal path needs no
    // special case for having been killed.
    std::thread watchdog;
    if (job->timeout_ms > 0) {
        watchdog = std::thread([job, child] {
            std::unique_lock<std::mutex> lk(job->finish_mutex);
            if (!job->finished.wait_for(lk, std::chrono::milliseconds(job->timeout_ms),
                                        [&] { return job->child_done; })) {
                job->timed_out = true;
                ::kill(child, SIGKILL);
            }
        });
    }

    // stderr on its own thread: a child that fills one pipe while nothing
    // drains the other would deadlock against a reader that takes them in
    // turn, and "it hung when the output got long" is a miserable bug.
    std::string err_text;
    std::thread err_reader([&] { err_text = slurp(err_pipe[0]); });
    job->out = slurp(out_pipe[0]);
    err_reader.join();
    job->err = std::move(err_text);

    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    job->code = WIFEXITED(status) ? WEXITSTATUS(status)
                                  : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);

    if (watchdog.joinable()) {
        {
            std::lock_guard<std::mutex> lk(job->finish_mutex);
            job->child_done = true;
        }
        job->finished.notify_all();
        watchdog.join();
    }
    finish();
}

NativeResult os_exec_with(Process& p, Value* args, int64_t timeout_ms);

NativeResult os_exec(Process& p, Value, Value* args, uint32_t) {
    return os_exec_with(p, args, 0);
}

/// `exec_for! program args milliseconds` -- the same, with a deadline. A child
/// that outlives it is killed, and the result says `timed_out` so the caller
/// can tell that from an ordinary non-zero exit.
NativeResult os_exec_for(Process& p, Value, Value* args, uint32_t) {
    Value ms = resolve(args[2]);
    if (!is_fixnum(ms) || fixnum_value(ms) < 0) {
        return fail(p, "type_error", "exec_for! needs a timeout in milliseconds");
    }
    return os_exec_with(p, args, fixnum_value(ms));
}

NativeResult os_exec_with(Process& p, Value* args, int64_t timeout_ms) {
    // Entered again after the helper thread woke us: the child has exited.
    if (p.os_pending >= 0) {
        int64_t id = p.os_pending;
        p.os_pending = -1;
        auto job = Jobs::get().find(id);
        if (!job) return fail(p, "os_error", "the subprocess result was lost");

        NativeResult result = [&]() -> NativeResult {
            if (!job->error.empty()) return fail(p, "not_found", job->error);
            Value m = p.heap().make_map(8);
            m = map_insert(p, m, make_atom(p.runtime().intern_atom("code")),
                       make_integer(p, int64_t(job->code)));
            m = resolve(m);
            m = map_insert(p, m, make_atom(p.runtime().intern_atom("out")),
                       text(p, job->out));
            m = resolve(m);
            m = map_insert(p, m, make_atom(p.runtime().intern_atom("err")),
                       text(p, job->err));
            m = resolve(m);
            m = map_insert(p, m, make_atom(p.runtime().intern_atom("timed_out")),
                       make_bool(job->timed_out));
            return NativeResult::ok(resolve(m));
        }();
        Jobs::get().erase(id);
        return result;
    }

    if (!is_string(args[0])) return fail(p, "type_error", "exec! needs a program name");
    std::vector<std::string> argv{string_arg(args[0])};
    if (!read_string_list(p, args[1], &argv)) {
        if (p.park_requested) return NativeResult::block();
        return fail(p, "type_error", "exec! needs a list of string arguments");
    }

    Scheduler* sched = p.runtime().scheduler();
    if (!sched) return fail(p, "os_error", "no scheduler is running to wake this process");

    auto job = std::make_shared<Job>();
    job->timeout_ms = timeout_ms;
    int64_t id = Jobs::get().add(job);
    p.os_pending = id;

    // Ask to be parked before the child can finish, so a wake-up that arrives
    // immediately is not lost: `wake` finds the process still running and
    // leaves a note the worker checks before it commits to parking.
    {
        std::lock_guard<std::mutex> g(p.sched_mutex);
        p.park_requested = true;
        p.wait_reason.store(WaitReason::Io, std::memory_order_relaxed);
    }
    sched->note_io_wait(true);
    job->worker = std::thread(run_child, job, std::move(argv), p.id(), sched);
    return NativeResult::block();
}

// ---------------------------------------------------------------------------
// Everything else
// ---------------------------------------------------------------------------

NativeResult os_args(Process& p, Value, Value*, uint32_t) {
    return NativeResult::ok(string_list(p, p.runtime().program_args()));
}

NativeResult os_env(Process& p, Value, Value* args, uint32_t) {
    if (!is_string(args[0])) return fail(p, "type_error", "env! needs a name");
    const char* v = ::getenv(string_arg(args[0]).c_str());
    return NativeResult::ok(v ? text(p, v) : UNIT);
}

NativeResult os_set_env(Process& p, Value, Value* args, uint32_t) {
    if (!is_string(args[0]) || !is_string(args[1])) {
        return fail(p, "type_error", "set_env! needs a name and a value");
    }
    ::setenv(string_arg(args[0]).c_str(), string_arg(args[1]).c_str(), 1);
    return NativeResult::ok(UNIT);
}

NativeResult os_cwd(Process& p, Value, Value*, uint32_t) {
    char buf[4096];
    if (!::getcwd(buf, sizeof buf)) return fail(p, "os_error", std::strerror(errno));
    return NativeResult::ok(text(p, buf));
}

NativeResult os_chdir(Process& p, Value, Value* args, uint32_t) {
    if (!is_string(args[0])) return fail(p, "type_error", "chdir! needs a path");
    std::string path = string_arg(args[0]);
    if (::chdir(path.c_str()) != 0) {
        return fail(p, errno == ENOENT ? "not_found" : "os_error",
                    "cannot enter " + path + ": " + std::strerror(errno));
    }
    return NativeResult::ok(UNIT);
}

/// The names in a directory, without `.` and `..`. Sorted, so a build that
/// walks a tree does the same thing twice -- readdir order is whatever the
/// file system feels like.
NativeResult os_list_dir(Process& p, Value, Value* args, uint32_t) {
    if (!is_string(args[0])) return fail(p, "type_error", "list_dir! needs a path");
    std::string path = string_arg(args[0]);
    DIR* dir = ::opendir(path.c_str());
    if (!dir) {
        return fail(p, errno == ENOENT ? "not_found" : "os_error",
                    "cannot read " + path + ": " + std::strerror(errno));
    }
    std::vector<std::string> names;
    while (dirent* e = ::readdir(dir)) {
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        names.push_back(std::move(name));
    }
    ::closedir(dir);
    std::sort(names.begin(), names.end());
    return NativeResult::ok(string_list(p, names));
}

NativeResult os_pid(Process& p, Value, Value*, uint32_t) {
    return NativeResult::ok(make_integer(p, int64_t(::getpid())));
}

NativeResult os_platform(Process& p, Value, Value*, uint32_t) {
#if defined(__linux__)
    const char* name = "linux";
#elif defined(__APPLE__)
    const char* name = "macos";
#elif defined(_WIN32)
    const char* name = "windows";
#else
    const char* name = "unknown";
#endif
    return NativeResult::ok(make_atom(p.runtime().intern_atom(name)));
}

/// Stop the whole program. Every other process is abandoned where it stands,
/// which is why this is worth reaching for only from the top of a program.
NativeResult os_exit(Process& p, Value, Value* args, uint32_t) {
    Value v = resolve(args[0]);
    if (!is_fixnum(v)) return fail(p, "type_error", "exit! needs an exit code");
    std::fflush(nullptr);
    std::_Exit(int(fixnum_value(v)));
}

}  // namespace

void os_shutdown() { Jobs::get().drain(); }

ModuleDef make_os_module() {
    return ModuleDef{"std.os",
                     {
                         {"args!", 1, 0b1, os_args},
                         {"env!", 1, 0b1, os_env},
                         {"set_env!", 2, 0b11, os_set_env},
                         {"cwd!", 1, 0b1, os_cwd},
                         {"chdir!", 1, 0b1, os_chdir},
                         {"list_dir!", 1, 0b1, os_list_dir},
                         {"exec!", 2, 0b01, os_exec},
                         {"exec_for!", 3, 0b101, os_exec_for},
                         {"pid!", 1, 0b1, os_pid},
                         {"platform", 1, 0b1, os_platform},
                         {"exit!", 1, 0b1, os_exit},
                     }};
}

}  // namespace dream
