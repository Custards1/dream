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
#include <filesystem>
#include "windows.hpp"
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

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "builtins.hpp"
#include "interp.hpp"
#include "process.hpp"
#include "scheduler.hpp"

#ifndef _WIN32
extern char** environ;
#endif

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

/// How to run a child: what `run!`'s options map says, and what `exec!` and
/// `exec_for!` leave at the defaults.
///
/// A build runs other programs in directories and environments of its own
/// choosing, and running them from here is the only way it can. Changing this
/// VM's own directory or environment around a call would not do: both belong
/// to the whole VM, and a build runs several children at once.
struct RunOptions {
    /// 0 means "wait as long as it takes". Anything else is a deadline after
    /// which the child is killed -- worth having because a build tool spends
    /// its life running other people's programs, and `git clone` against an
    /// unreachable host will otherwise wait for ever.
    int64_t timeout_ms = 0;
    std::string cwd;                                          // empty: this VM's
    std::vector<std::pair<std::string, std::string>> env;     // set over the inherited
    bool clear_env = false;                                   // inherit nothing first
    std::string log;         // both streams to this file instead of `:out`/`:err`
    bool has_stdin = false;  // otherwise the child inherits this VM's stdin
    std::string stdin_text;
};

/// One `exec!` in flight. The Dream process holds only the id; the job itself
/// outlives the native call, because the native returns as soon as it has
/// parked and is entered again from the top when it is woken.
struct Job {
    std::atomic<bool> done{false};
    int code = -1;
    bool started = false;
    bool timed_out = false;
    std::string error;   // non-empty when the child could not be started
    const char* error_kind = "not_found";
    std::string out;
    std::string err;
    std::thread worker;
    RunOptions opts;

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

#ifdef _WIN32
// Pipe reads live on helper threads, just as on POSIX. Only the explicitly
// listed standard handles are inherited by the child.
std::string slurp(HANDLE pipe) {
    std::string out;
    char buf[4096];
    DWORD n;
    while (ReadFile(pipe, buf, sizeof buf, &n, nullptr) && n) out.append(buf, n);
    CloseHandle(pipe);
    return out;
}

/// Write all of `text` to the child's input, then close it. A child that
/// exits without reading is not an error; the write just stops.
void feed(HANDLE pipe, std::string text) {
    size_t at = 0;
    while (at < text.size()) {
        DWORD n = 0;
        DWORD want = DWORD(std::min<size_t>(text.size() - at, 1u << 20));
        if (!WriteFile(pipe, text.data() + at, want, &n, nullptr) || !n) break;
        at += n;
    }
    CloseHandle(pipe);
}

/// The environment block a child starts with: this VM's, or none, with the
/// options' variables set over it. Windows compares names without regard to
/// case, and wants the block sorted the same way.
std::wstring child_environment(const RunOptions& opts) {
    std::vector<std::wstring> entries;
    auto same_name = [](const std::wstring& entry, const std::wstring& name) {
        size_t eq = entry.find(L'=', 1);   // `=C:=C:\` names begin with `=`
        std::wstring have = entry.substr(0, eq);
        return CompareStringOrdinal(have.c_str(), int(have.size()),
                                    name.c_str(), int(name.size()), TRUE) == CSTR_EQUAL;
    };
    std::vector<std::pair<std::wstring, std::wstring>> set;
    for (const auto& [name, value] : opts.env) set.emplace_back(windows::wide(name), windows::wide(value));
    if (!opts.clear_env) {
        wchar_t* block = GetEnvironmentStringsW();
        for (wchar_t* e = block; e && *e; e += wcslen(e) + 1) {
            std::wstring entry(e);
            bool replaced = false;
            for (const auto& [name, value] : set) replaced = replaced || same_name(entry, name);
            if (!replaced) entries.push_back(entry);
        }
        if (block) FreeEnvironmentStringsW(block);
    }
    for (const auto& [name, value] : set) entries.push_back(name + L"=" + value);
    std::sort(entries.begin(), entries.end(), [](const std::wstring& a, const std::wstring& b) {
        return CompareStringOrdinal(a.c_str(), int(a.size()), b.c_str(), int(b.size()), TRUE) == CSTR_LESS_THAN;
    });
    std::wstring out;
    for (const auto& e : entries) { out += e; out.push_back(L'\0'); }
    out.push_back(L'\0');
    return out;
}

void run_child(std::shared_ptr<Job> job, std::vector<std::string> argv,
               uint64_t pid, Scheduler* sched) {
    const RunOptions& opts = job->opts;
    auto finish = [&] {
        job->done.store(true, std::memory_order_release);
        if (sched) { sched->wake(pid); sched->note_io_wait(false); }
    };
    HANDLE out_read = nullptr, out_write = nullptr, err_read = nullptr, err_write = nullptr;
    HANDLE in_read = nullptr, in_write = nullptr, log = nullptr;
    HANDLE input = INVALID_HANDLE_VALUE;
    auto cleanup = [&] {
        for (HANDLE h : {out_read, out_write, err_read, err_write, in_read, in_write, log, input})
            if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    };
    auto give_up = [&](const char* kind, std::string message) {
        job->error_kind = kind;
        job->error = std::move(message);
        cleanup(); finish();
    };
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    if (!opts.log.empty()) {
        log = CreateFileW(windows::wide(opts.log).c_str(), GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, &security, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
        if (log == INVALID_HANDLE_VALUE) {
            log = nullptr;
            return give_up("os_error", "cannot open the log `" + opts.log + "`: " + windows::error());
        }
    } else if (!CreatePipe(&out_read, &out_write, &security, 0) ||
               !CreatePipe(&err_read, &err_write, &security, 0) ||
               !SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0) ||
               !SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0)) {
        return give_up("os_error", "cannot create pipes: " + windows::error());
    }
    if (opts.has_stdin) {
        if (!CreatePipe(&in_read, &in_write, &security, 0) ||
            !SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0)) {
            return give_up("os_error", "cannot create pipes: " + windows::error());
        }
        input = in_read;
        in_read = nullptr;
    } else {
        HANDLE current_input = GetStdHandle(STD_INPUT_HANDLE);
        if (!current_input || current_input == INVALID_HANDLE_VALUE ||
            !DuplicateHandle(GetCurrentProcess(), current_input, GetCurrentProcess(),
                             &input, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
            input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                &security, OPEN_EXISTING, 0, nullptr);
        }
    }
    HANDLE child_out = log ? log : out_write;
    HANDLE child_err = log ? log : err_write;
    STARTUPINFOEXW start{};
    start.StartupInfo.cb = sizeof(start);
    start.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    start.StartupInfo.hStdInput = input;
    start.StartupInfo.hStdOutput = child_out;
    start.StartupInfo.hStdError = child_err;
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> attributes(bytes);
    start.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(start.lpAttributeList, 1, 0, &bytes)) {
        return give_up("os_error", windows::error());
    }
    // A handle listed twice is refused, and a log is both streams.
    HANDLE inherited[] = {input, child_out, child_err};
    DWORD inherited_count = child_out == child_err ? 2 : 3;
    bool ready = UpdateProcThreadAttribute(start.lpAttributeList, 0,
        PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, inherited_count * sizeof(HANDLE), nullptr, nullptr);
    std::wstring command;
    for (const auto& arg : argv) {
        if (!command.empty()) command += L' ';
        command += windows::quote(arg);
    }
    bool own_env = opts.clear_env || !opts.env.empty();
    std::wstring env_block = own_env ? child_environment(opts) : std::wstring();
    std::wstring cwd = opts.cwd.empty() ? std::wstring() : windows::wide(opts.cwd);
    PROCESS_INFORMATION child{};
    bool created = ready && CreateProcessW(nullptr, command.data(), nullptr, nullptr,
        TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
        own_env ? env_block.data() : nullptr, cwd.empty() ? nullptr : cwd.c_str(),
        &start.StartupInfo, &child);
    DWORD error = GetLastError();
    DeleteProcThreadAttributeList(start.lpAttributeList);
    for (HANDLE* h : {&out_write, &err_write, &log}) {
        if (*h) { CloseHandle(*h); *h = nullptr; }
    }
    if (!created) {
        std::string where = opts.cwd.empty() ? "" : " in `" + opts.cwd + "`";
        return give_up(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? "not_found" : "os_error",
                       "cannot run `" + argv[0] + "`" + where + ": " + windows::error(error));
    }
    CloseHandle(input); input = nullptr;
    CloseHandle(child.hThread);
    job->started = true;
    std::thread feeder;
    if (in_write) { feeder = std::thread(feed, in_write, opts.stdin_text); in_write = nullptr; }
    std::thread out_reader, err_reader;
    if (out_read) {
        out_reader = std::thread([&] { job->out = slurp(out_read); });
        err_reader = std::thread([&] { job->err = slurp(err_read); });
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(job->opts.timeout_ms);
    while (WaitForSingleObject(child.hProcess, 50) == WAIT_TIMEOUT) {
        if (job->opts.timeout_ms > 0 && std::chrono::steady_clock::now() >= deadline) {
            job->timed_out = true;
            TerminateProcess(child.hProcess, 124);
            WaitForSingleObject(child.hProcess, INFINITE);
            break;
        }
    }
    DWORD code = 0;
    GetExitCodeProcess(child.hProcess, &code);
    job->code = int(code);
    CloseHandle(child.hProcess);
    if (out_reader.joinable()) { out_reader.join(); err_reader.join(); }
    if (feeder.joinable()) feeder.join();
    finish();
}
#else
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

/// A pipe whose ends are closed on exec.
///
/// Children are started from several helper threads at once, and a pipe end
/// that leaks into a sibling keeps that pipe open for as long as the sibling
/// runs: the reader waiting on the first child does not see end of file until
/// the second has finished too. A build that runs eight compiles would find
/// each one taking as long as the slowest. `dup2` in the spawn's file actions
/// clears the flag on the descriptor the child is meant to have, so only that
/// one survives. Where there is no `pipe2`, setting the flag afterwards leaves
/// a window, which is the best that can be done there.
bool cloexec_pipe(int fds[2]) {
#if defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    return ::pipe2(fds, O_CLOEXEC) == 0;
#else
    if (::pipe(fds) != 0) return false;
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return true;
#endif
}

/// Write all of `text` to `fd`, then close it. A child that exits without
/// reading its input is not an error, so a broken pipe just ends the write.
/// SIGPIPE is blocked on this thread for the write, and a signal raised by a
/// write is directed at the thread that made it, so it is discarded with the
/// thread rather than ending the VM.
void feed(int fd, std::string text) {
    sigset_t pipe_only, old;
    sigemptyset(&pipe_only);
    sigaddset(&pipe_only, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipe_only, &old);
    size_t at = 0;
    while (at < text.size()) {
        ssize_t n = ::write(fd, text.data() + at, text.size() - at);
        if (n > 0) { at += size_t(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    ::close(fd);
}

/// The environment a child starts with: this VM's, or none, with the options'
/// variables set over it.
std::vector<std::string> child_environment(const RunOptions& opts) {
    std::vector<std::string> out;
    auto overridden = [&](const char* entry) {
        const char* eq = std::strchr(entry, '=');
        size_t len = eq ? size_t(eq - entry) : std::strlen(entry);
        for (const auto& [name, value] : opts.env) {
            if (name.size() == len && std::memcmp(name.data(), entry, len) == 0) return true;
        }
        return false;
    };
    if (!opts.clear_env) {
        for (char** e = environ; *e; ++e) {
            if (!overridden(*e)) out.emplace_back(*e);
        }
    }
    for (const auto& [name, value] : opts.env) out.push_back(name + "=" + value);
    return out;
}

// `posix_spawn_file_actions_addchdir_np` is how a spawned child gets a working
// directory without a fork. glibc has had it since 2.29, musl since 1.1.24,
// and macOS since 10.15.
#if defined(__APPLE__) || defined(__GLIBC__) && (__GLIBC__ > 2 || __GLIBC_MINOR__ >= 29)
#define DREAM_SPAWN_CHDIR 1
#endif

/// Spawn `argv`, collect both streams, reap, and wake `pid`.
void run_child(std::shared_ptr<Job> job, std::vector<std::string> argv,
               uint64_t pid, Scheduler* sched) {
    const RunOptions& opts = job->opts;
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    int in_pipe[2] = {-1, -1};
    int log_fd = -1;
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
    auto close_all = [&] {
        for (int fd : {out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1],
                       in_pipe[0], in_pipe[1], log_fd}) {
            if (fd >= 0) ::close(fd);
        }
    };
    auto give_up = [&](const char* kind, std::string message) {
        job->error_kind = kind;
        job->error = std::move(message);
        close_all();
        finish();
    };

#ifndef DREAM_SPAWN_CHDIR
    if (!opts.cwd.empty()) {
        return give_up("os_error", "this platform's posix_spawn cannot set a working directory");
    }
#endif
    if (!opts.log.empty()) {
        log_fd = ::open(opts.log.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (log_fd < 0) {
            return give_up("os_error", "cannot open the log `" + opts.log + "`: " + std::strerror(errno));
        }
    } else if (!cloexec_pipe(out_pipe) || !cloexec_pipe(err_pipe)) {
        return give_up("os_error", std::string("cannot create a pipe: ") + std::strerror(errno));
    }
    if (opts.has_stdin && !cloexec_pipe(in_pipe)) {
        return give_up("os_error", std::string("cannot create a pipe: ") + std::strerror(errno));
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (log_fd >= 0) {
        posix_spawn_file_actions_adddup2(&actions, log_fd, 1);
        posix_spawn_file_actions_adddup2(&actions, log_fd, 2);
    } else {
        posix_spawn_file_actions_adddup2(&actions, out_pipe[1], 1);
        posix_spawn_file_actions_adddup2(&actions, err_pipe[1], 2);
    }
    if (in_pipe[0] >= 0) posix_spawn_file_actions_adddup2(&actions, in_pipe[0], 0);
#ifdef DREAM_SPAWN_CHDIR
    if (!opts.cwd.empty()) posix_spawn_file_actions_addchdir_np(&actions, opts.cwd.c_str());
#endif

    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (std::string& a : argv) args.push_back(a.data());
    args.push_back(nullptr);

    // The inherited environment is passed as it stands; anything else is
    // built here and outlives the spawn.
    bool own_env = opts.clear_env || !opts.env.empty();
    std::vector<std::string> env_text;
    std::vector<char*> env_ptrs;
    if (own_env) {
        env_text = child_environment(opts);
        for (std::string& e : env_text) env_ptrs.push_back(e.data());
        env_ptrs.push_back(nullptr);
    }

    pid_t child = 0;
    // `posix_spawnp`, so a bare program name is found on PATH -- `git` should
    // mean what it means in a shell. The PATH searched is this VM's, whatever
    // the child is given.
    int rc = ::posix_spawnp(&child, args[0], &actions, nullptr, args.data(),
                            own_env ? env_ptrs.data() : environ);
    posix_spawn_file_actions_destroy(&actions);
    for (int* fd : {&out_pipe[1], &err_pipe[1], &in_pipe[0], &log_fd}) {
        if (*fd >= 0) { ::close(*fd); *fd = -1; }
    }

    if (rc != 0) {
        const char* kind = rc == ENOENT || rc == EACCES ? "not_found" : "os_error";
        std::string where = opts.cwd.empty() ? "" : " in `" + opts.cwd + "`";
        return give_up(kind, std::string("cannot run `") + argv[0] + "`" + where + ": " + std::strerror(rc));
    }
    job->started = true;

    // A watchdog, when a deadline was asked for. It waits to be told the child
    // finished and kills it if that never comes; the reads below then see end
    // of file and `waitpid` reaps as usual, so the normal path needs no
    // special case for having been killed.
    std::thread watchdog;
    if (job->opts.timeout_ms > 0) {
        watchdog = std::thread([job, child] {
            std::unique_lock<std::mutex> lk(job->finish_mutex);
            if (!job->finished.wait_for(lk, std::chrono::milliseconds(job->opts.timeout_ms),
                                        [&] { return job->child_done; })) {
                job->timed_out = true;
                ::kill(child, SIGKILL);
            }
        });
    }

    // The input on a thread of its own, for the same reason as stderr below:
    // a child that answers before it has read everything would otherwise be
    // writing into a full pipe while this thread is still writing into its.
    std::thread feeder;
    if (in_pipe[1] >= 0) {
        feeder = std::thread(feed, in_pipe[1], opts.stdin_text);
        in_pipe[1] = -1;
    }

    // stderr on its own thread: a child that fills one pipe while nothing
    // drains the other would deadlock against a reader that takes them in
    // turn, and "it hung when the output got long" is a miserable bug.
    if (out_pipe[0] >= 0) {
        std::string err_text;
        std::thread err_reader([&] { err_text = slurp(err_pipe[0]); });
        job->out = slurp(out_pipe[0]);
        err_reader.join();
        job->err = std::move(err_text);
    }

    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    job->code = WIFEXITED(status) ? WEXITSTATUS(status)
                                  : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
    if (feeder.joinable()) feeder.join();

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

#endif

/// Entered again after the helper thread woke us: the child has exited, and
/// its job holds what it did.
NativeResult child_result(Process& p) {
    int64_t id = p.os_pending;
    p.os_pending = -1;
    auto job = Jobs::get().find(id);
    if (!job) return fail(p, "os_error", "the subprocess result was lost");

    NativeResult result = [&]() -> NativeResult {
        if (!job->error.empty()) return fail(p, job->error_kind, job->error);
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

/// Start `program args` on a helper thread and park until it has finished.
/// `name` is the native's, for its messages.
NativeResult start_child(Process& p, const char* name, Value* args, RunOptions opts) {
    std::string who = name;
    if (!is_string(args[0])) return fail(p, "type_error", who + " needs a program name");
    std::vector<std::string> argv{string_arg(args[0])};
    if (!read_string_list(p, args[1], &argv)) {
        if (p.park_requested) return NativeResult::block();
        return fail(p, "type_error", who + " needs a list of string arguments");
    }

    for (const auto& arg : argv) {
        if (arg.find('\0') != std::string::npos)
            return fail(p, "bad_argument", "subprocess arguments cannot contain NUL bytes");
#ifdef _WIN32
        try { (void)windows::wide(arg); }
        catch (const std::system_error& error) { return fail(p, "bad_argument", error.what()); }
#endif
    }
    if (argv[0].empty()) return fail(p, "bad_argument", who + " needs a non-empty program name");

    // A program named by a path is found from where this VM stands, not from
    // the child's `:cwd`. `run! "build/bin/dream" .. %{ :cwd => job }` means
    // the `dream` beside the caller, which is what anyone writing it meant;
    // only a bare name is looked up on PATH.
    std::filesystem::path program(std::u8string(argv[0].begin(), argv[0].end()));
    if (!opts.cwd.empty() && program.is_relative() && program.has_parent_path()) {
        std::error_code ec;
        auto absolute = std::filesystem::absolute(program, ec);
        if (!ec) {
            auto bytes = absolute.u8string();
            argv[0] = std::string(bytes.begin(), bytes.end());
        }
    }

    // Otherwise a missing directory comes back from the spawn as the program
    // not being found, which sends whoever reads it looking for the wrong file.
    if (!opts.cwd.empty()) {
        std::error_code ec;
        if (!std::filesystem::is_directory(std::filesystem::path(std::u8string(opts.cwd.begin(), opts.cwd.end())), ec))
            return fail(p, "not_found", who + "'s `:cwd` `" + opts.cwd + "` is not a directory");
    }

    Scheduler* sched = p.runtime().scheduler();
    if (!sched) return fail(p, "os_error", "no scheduler is running to wake this process");

    auto job = std::make_shared<Job>();
    job->opts = std::move(opts);
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

NativeResult os_exec(Process& p, Value, Value* args, uint32_t) {
    if (p.os_pending >= 0) return child_result(p);
    return start_child(p, "exec!", args, RunOptions{});
}

/// `exec_for! program args milliseconds` -- the same, with a deadline. A child
/// that outlives it is killed, and the result says `timed_out` so the caller
/// can tell that from an ordinary non-zero exit.
NativeResult os_exec_for(Process& p, Value, Value* args, uint32_t) {
    if (p.os_pending >= 0) return child_result(p);
    Value ms = resolve(args[2]);
    if (!is_fixnum(ms) || fixnum_value(ms) < 0) {
        return fail(p, "type_error", "exec_for! needs a timeout in milliseconds");
    }
    RunOptions opts;
    opts.timeout_ms = fixnum_value(ms);
    return start_child(p, "exec_for!", args, std::move(opts));
}

/// What reading `run!`'s options came to: all of them, a force that has to
/// park first, or a mistake to report.
enum class Read { ok, park, bad };

Read forced(Process& p, Value v, Value* out) {
    if (force_whnf(p, v, out)) return Read::ok;
    return p.park_requested ? Read::park : Read::bad;
}

Read option_string(Process& p, Value v, std::string* out) {
    Value w;
    if (Read r = forced(p, v, &w); r != Read::ok) return r;
    if (!is_obj(w, ObjType::Str)) return Read::bad;
    auto* s = static_cast<StrObj*>(as_obj(w));
    out->assign(s->data(), s->len);
    return out->find('\0') == std::string::npos ? Read::ok : Read::bad;
}

/// `run!`'s options, into `o`. A key it does not know is a mistake rather than
/// something to ignore: `%{ :cdw => dir }` run in the wrong directory would
/// be a far worse answer than an error naming the key.
Read read_run_options(Process& p, Value options, RunOptions* o, std::string* why) {
    Value map;
    if (Read r = forced(p, options, &map); r != Read::ok) {
        *why = "run! needs a map of options";
        return r;
    }
    if (!is_obj(map, ObjType::Map)) {
        *why = "run! needs a map of options, `%{}` for none";
        return Read::bad;
    }
    std::vector<std::pair<Value, Value>> entries;
    map_collect(map, entries);
    for (auto& [key, value] : entries) {
        Value k = resolve(key);
        std::string name = is_atom(k) ? p.runtime().atom_name(uint32_t(imm_payload(k))) : "";
        auto wrong = [&](const char* wanted) {
            *why = "run!'s `:" + name + "` needs " + wanted;
            return Read::bad;
        };
        Read r = Read::ok;
        if (name == "cwd" || name == "log") {
            r = option_string(p, value, name == "cwd" ? &o->cwd : &o->log);
            if (r == Read::bad) return wrong("a path");
        } else if (name == "stdin") {
            r = option_string(p, value, &o->stdin_text);
            if (r == Read::bad) return wrong("a string");
            o->has_stdin = true;
        } else if (name == "timeout") {
            Value w;
            r = forced(p, value, &w);
            if (r == Read::ok && (!is_fixnum(w) || fixnum_value(w) < 0)) return wrong("milliseconds");
            if (r == Read::ok) o->timeout_ms = fixnum_value(w);
        } else if (name == "clear_env") {
            Value w;
            r = forced(p, value, &w);
            if (r == Read::ok && !is_bool(w)) return wrong("true or false");
            if (r == Read::ok) o->clear_env = truthy(w);
        } else if (name == "env") {
            Value vars;
            r = forced(p, value, &vars);
            if (r == Read::ok && !is_obj(vars, ObjType::Map)) return wrong("a map of names to values");
            if (r == Read::ok) {
                std::vector<std::pair<Value, Value>> pairs;
                map_collect(vars, pairs);
                for (auto& [var, text_value] : pairs) {
                    std::string var_name, var_value;
                    r = option_string(p, var, &var_name);
                    if (r == Read::ok) r = option_string(p, text_value, &var_value);
                    if (r == Read::park) break;
                    if (r == Read::bad || var_name.empty() || var_name.find('=', 1) != std::string::npos)
                        return wrong("strings for names and values, and no `=` in a name");
                    o->env.emplace_back(std::move(var_name), std::move(var_value));
                }
            }
        } else {
            *why = is_atom(k) ? "run! has no option `:" + name + "`; it takes "
                                ":cwd, :env, :clear_env, :timeout, :log and :stdin"
                              : "run!'s options are keyed by atoms, as in `%{ :cwd => dir }`";
            return Read::bad;
        }
        if (r == Read::park) return r;
    }
    return Read::ok;
}

/// `run! program args options` -- `exec!` with a say in how the child starts.
///
///   :cwd       the directory it runs in
///   :env       %{ name => value }, set over the inherited environment
///   :clear_env inherit nothing but what `:env` sets
///   :timeout   milliseconds, as for `exec_for!`
///   :log       a file both streams are written to, instead of `:out`/`:err`
///   :stdin     a string to give it as input, which is then closed
///
/// A build is what this is for: every step it runs has a directory and an
/// environment of its own, and a compile's output belongs in a log beside
/// what it made rather than in a string held by the caller.
NativeResult os_run(Process& p, Value, Value* args, uint32_t) {
    if (p.os_pending >= 0) return child_result(p);
    RunOptions opts;
    std::string why;
    switch (read_run_options(p, args[2], &opts, &why)) {
        case Read::park: return NativeResult::block();
        case Read::bad: return fail(p, "bad_argument", why);
        case Read::ok: break;
    }
    return start_child(p, "run!", args, std::move(opts));
}

// ---------------------------------------------------------------------------
// Everything else
// ---------------------------------------------------------------------------

NativeResult os_args(Process& p, Value, Value*, uint32_t) {
    return NativeResult::ok(string_list(p, p.runtime().program_args()));
}

NativeResult os_env(Process& p, Value, Value* args, uint32_t) {
    if (!is_string(args[0])) return fail(p, "type_error", "env! needs a name");
#ifdef _WIN32
    const wchar_t* v = _wgetenv(windows::wide(string_arg(args[0])).c_str());
    return NativeResult::ok(v ? text(p, windows::utf8(v)) : UNIT);
#else
    const char* v = ::getenv(string_arg(args[0]).c_str());
    return NativeResult::ok(v ? text(p, v) : UNIT);
#endif
}

NativeResult os_set_env(Process& p, Value, Value* args, uint32_t) {
    if (!is_string(args[0]) || !is_string(args[1])) {
        return fail(p, "type_error", "set_env! needs a name and a value");
    }
#ifdef _WIN32
    _wputenv_s(windows::wide(string_arg(args[0])).c_str(), windows::wide(string_arg(args[1])).c_str());
#else
    ::setenv(string_arg(args[0]).c_str(), string_arg(args[1]).c_str(), 1);
#endif
    return NativeResult::ok(UNIT);
}

NativeResult os_cwd(Process& p, Value, Value*, uint32_t) {
    std::error_code ec;
    auto path = std::filesystem::current_path(ec);
    if (ec) return fail(p, "os_error", ec.message());
    auto bytes = path.generic_u8string();
    return NativeResult::ok(text(p, std::string(bytes.begin(), bytes.end())));
}

NativeResult os_chdir(Process& p, Value, Value* args, uint32_t) {
    if (!is_string(args[0])) return fail(p, "type_error", "chdir! needs a path");
    std::string path_bytes = string_arg(args[0]);
    std::error_code ec;
    std::filesystem::current_path(std::filesystem::path(std::u8string(path_bytes.begin(), path_bytes.end())), ec);
    if (ec) return fail(p, ec == std::errc::no_such_file_or_directory ? "not_found" : "os_error", ec.message());
    return NativeResult::ok(UNIT);
}

NativeResult os_list_dir(Process& p, Value, Value* args, uint32_t) {
    if (!is_string(args[0])) return fail(p, "type_error", "list_dir! needs a path");
    std::string path_bytes = string_arg(args[0]);
    std::error_code ec;
    std::filesystem::directory_iterator it(std::filesystem::path(std::u8string(path_bytes.begin(), path_bytes.end())), ec), end;
    std::vector<std::string> names;
    while (!ec && it != end) {
        auto bytes = it->path().filename().u8string();
        names.emplace_back(bytes.begin(), bytes.end());
        it.increment(ec);
    }
    if (ec) return fail(p, ec == std::errc::no_such_file_or_directory ? "not_found" : "os_error", ec.message());
    std::sort(names.begin(), names.end());
    return NativeResult::ok(string_list(p, names));
}

NativeResult os_pid(Process& p, Value, Value*, uint32_t) {
    return NativeResult::ok(make_integer(p, int64_t(
#ifdef _WIN32
        ::_getpid()
#else
        ::getpid()
#endif
    )));
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
/// `monotonic!` -- milliseconds from some fixed point in this process's life.
///
/// A steady clock, so it never jumps: the number it answers only ever grows,
/// whatever happens to the wall clock. That is what makes a *difference*
/// between two readings a duration, which is the only thing this is for --
/// where the zero is has no meaning at all.
///
/// Timing anything in a lazy language means forcing it first. A phase that has
/// not been forced has not run, so `let t = monotonic! ()` around an unforced
/// value times the building of a thunk.
NativeResult os_monotonic(Process& p, Value, Value*, uint32_t) {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return NativeResult::ok(
        make_integer(p, int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(now).count())));
}

/// `now!` -- milliseconds since the Unix epoch, from the wall clock.
///
/// This one can jump backwards, because the wall clock can: it is what to
/// stamp a file or a log line with, and never what to measure a duration with.
NativeResult os_now(Process& p, Value, Value*, uint32_t) {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    return NativeResult::ok(
        make_integer(p, int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(now).count())));
}

#ifdef DREAM_PROFILE_GENERATE
extern "C" void __gcov_dump();
#endif

/// End the process now, running nothing on the way out -- POSIX `_Exit`.
///
/// On Windows `_Exit` is not that. It is `ExitProcess`, which kills every other
/// thread and then runs each DLL's static destructors, the VM's among them,
/// against a heap and a collector whose threads vanished mid-step. So Windows
/// ends with `TerminateProcess` on itself, which is what `_Exit` means
/// everywhere else. Callers flush first; nothing after this runs.
[[noreturn]] static void exit_now(int code) {
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
#endif
    std::_Exit(code);
}

// Its training-only flush changes this cold function's control-flow graph;
// exclude it from GCC's profile rather than applying mismatched counters.
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((no_profile_instrument_function))
#endif
NativeResult os_exit(Process& p, Value, Value* args, uint32_t) {
    Value v = resolve(args[0]);
    if (!is_fixnum(v)) return fail(p, "type_error", "exit! needs an exit code");
    // Nothing runs after this, so anything the run owes its caller has to be
    // said here. A measurement that only printed on the way out of `main`
    // would never print for a program that ends by exiting, which is most
    // tools -- `dreams` among them, so `--stats` on a compile said nothing at
    // all until this line existed.
    p.runtime().print_profile();
    p.runtime().print_stats();
    // `_Exit` runs no destructors, and a C library's are often where it
    // writes out what it was holding. The exiting process's are the ones it
    // can run safely: every other process is still running on its own worker.
    release_foreign(p);
#ifdef DREAM_PROFILE_GENERATE
    // _Exit bypasses GCC's normal profile writer. Compiler training runs
    // finish through this native, so flush explicitly in instrumented builds.
    __gcov_dump();
#endif
    std::fflush(nullptr);
    exit_now(int(fixnum_value(v)));
}

/// `replace! program args` -- become another program.
///
/// This is `execvp`: the image, the heap and every thread of this VM are gone,
/// and the program named here takes over the process. Nothing comes back, so
/// there is no result type and no `!`-suffixed thing to do with one -- the only
/// way this returns is by failing.
///
/// `exec!` is the wrong tool for handing over to something *interactive*: it
/// gives the child pipes and reads them to the end, so a program that prompts
/// has nobody typing at it. A tool that runs an editor, a shell or a REPL wants
/// this instead -- the child inherits the terminal, because it inherits
/// everything.
NativeResult os_replace(Process& p, Value, Value* args, uint32_t) {
    if (!is_string(args[0])) return fail(p, "type_error", "replace! needs a program name");
    std::vector<std::string> argv{string_arg(args[0])};
    if (!read_string_list(p, args[1], &argv)) {
        if (p.park_requested) return NativeResult::block();
        return fail(p, "type_error", "replace! needs a list of string arguments");
    }

    std::vector<char*> raw;
    raw.reserve(argv.size() + 1);
    for (std::string& a : argv) raw.push_back(a.data());
    raw.push_back(nullptr);

    // Anything still sitting in a stdio buffer would be lost with the address
    // space, so it goes out first.
    std::fflush(nullptr);
#ifdef _WIN32
    std::wstring command;
    for (const auto& arg : argv) {
        if (!command.empty()) command += L' ';
        command += windows::quote(arg);
    }
    STARTUPINFOW start{};
    start.cb = sizeof(start);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &start, &child))
        return fail(p, "not_found", windows::error());
    CloseHandle(child.hThread);
    WaitForSingleObject(child.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(child.hProcess, &code);
    CloseHandle(child.hProcess);
    std::fflush(nullptr);
    exit_now(int(code));
#else
    ::execvp(raw[0], raw.data());
#endif
    return fail(p, "not_found",
                std::string("cannot run `") + argv[0] + "`: " + std::strerror(errno));
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
                         {"run!", 3, 0b101, os_run},
                         {"replace!", 2, 0b01, os_replace},
                         {"monotonic!", 1, 0b1, os_monotonic},
                         {"now!", 1, 0b1, os_now},
                         {"pid!", 1, 0b1, os_pid},
                         {"platform", 1, 0b1, os_platform},
                         {"exit!", 1, 0b1, os_exit},
                     }};
}

}  // namespace dream
