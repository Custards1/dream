// Input and output. See io.hpp for why this is not a server process.

#include "io.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "builtins.hpp"
#include "interp.hpp"
#include "process.hpp"
#include "scheduler.hpp"
#include "sha256.hpp"

#if defined(__linux__) && !defined(DREAM_PORTABLE_POLLER)
#define DREAM_HAVE_EPOLL 1
#include <sys/epoll.h>
#include <sys/eventfd.h>
#else
#define DREAM_HAVE_EPOLL 0
#endif

#include "platform_io.hpp"
#include <chrono>
#include <condition_variable>

namespace dream {
namespace {

[[maybe_unused]] bool trace_on() { static bool on = ::getenv("DREAM_IO_TRACE") != nullptr; return on; }
#define IOTRACE(...) do { if (trace_on()) { std::fprintf(stderr, "[io] " __VA_ARGS__); std::fprintf(stderr, "\n"); } } while (0)


// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

Value io_error(Process& p, const char* kind, const std::string& message) {
    return raise_error(p, p.runtime().intern_atom(kind), message);
}

NativeResult fail(Process& p, const char* kind, const std::string& message) {
    return NativeResult::raise(io_error(p, kind, message));
}

/// An `errno` turned into the kind a Dream program can match on. The message
/// keeps the system's own wording, which is more specific than anything this
/// file could invent.
NativeResult fail_errno(Process& p, const std::string& what, int err) {
    const char* kind = "io_error";
    switch (err) {
        case ENOENT: kind = "not_found"; break;
        case EACCES:
        case EPERM: kind = "permission_denied"; break;
        case EEXIST: kind = "already_exists"; break;
        case EISDIR:
        case ENOTDIR: kind = "wrong_kind"; break;
        case ECONNREFUSED: kind = "connection_refused"; break;
        case ECONNRESET:
        case EPIPE: kind = "connection_lost"; break;
        case EADDRINUSE: kind = "address_in_use"; break;
        case ETIMEDOUT: kind = "timed_out"; break;
        default: break;
    }
    return fail(p, kind, what + ": " + std::strerror(err));
}

std::string string_arg(Value v) {
    v = resolve(v);
    if (!is_obj(v, ObjType::Str)) return std::string();
    auto* s = static_cast<StrObj*>(as_obj(v));
    return std::string(s->data(), s->len);
}

/// Deliberately false for a big string. Everything that asks goes on to call
/// `string_arg`, which materializes a `std::string` -- a path a payload must
/// never take. `write!` is the one place that wants the bytes rather than a
/// string, and it asks `string_bytes` instead.
bool is_string(Value v) { return is_obj(resolve(v), ObjType::Str); }

// ---------------------------------------------------------------------------
// Handles
//
// A handle is a small integer, not a pointer and not a raw descriptor. It
// carries a generation, so an id left over from a closed handle is rejected
// rather than silently addressing whatever descriptor the kernel has since
// handed out for the same number. That is the whole safety argument: no Dream
// program can reach a descriptor it was not given, or one it has closed.
// ---------------------------------------------------------------------------

struct Handle {
    int fd = -1;
    HandleKind kind = HandleKind::File;
    uint32_t generation = 0;
    bool open = false;
    /// Set between issuing a `connect` and learning whether it succeeded.
    bool connecting = false;
    /// Closing a descriptor the VM does not own would be rude, and closing
    /// stdin twice would be worse.
    bool owned = true;
    /// How many operations are in flight on this handle right now.
    ///
    /// Operations release the table lock before their syscall -- holding it
    /// would serialize every process's IO behind whichever one is slowest --
    /// so `close!` cannot simply close the descriptor: another process may be
    /// inside `read(2)` on it, and the number would be handed straight back
    /// out by the next `open`. Instead `close!` marks the handle closed and
    /// the last operation to finish does the closing.
    int busy = 0;
};

constexpr uint64_t HANDLE_INDEX_BITS = 32;
constexpr uint64_t HANDLE_INDEX_MASK = (uint64_t(1) << HANDLE_INDEX_BITS) - 1;

class HandleTable {
public:
    static HandleTable& get() {
        static HandleTable t;
        return t;
    }

    /// Take ownership of `fd` and return the id that addresses it.
    int64_t add(int fd, HandleKind kind, bool owned = true) {
        std::lock_guard<std::mutex> g(mutex_);
        size_t i = 0;
        for (; i < slots_.size(); ++i) {
            if (!slots_[i].open && slots_[i].busy == 0) break;
        }
        if (i == slots_.size()) slots_.push_back(Handle{});
        Handle& h = slots_[i];
        h.fd = fd;
        h.kind = kind;
        h.open = true;
        h.connecting = false;
        h.owned = owned;
        ++h.generation;
        return int64_t((uint64_t(h.generation) << HANDLE_INDEX_BITS) | uint64_t(i));
    }

    /// The handle behind `id`, or null when the id is stale, closed or was
    /// never issued.
    Handle* find(int64_t id) {
        if (id < 0) return nullptr;
        uint64_t raw = uint64_t(id);
        size_t index = size_t(raw & HANDLE_INDEX_MASK);
        uint32_t generation = uint32_t(raw >> HANDLE_INDEX_BITS);
        if (index >= slots_.size()) return nullptr;
        Handle& h = slots_[index];
        if (!h.open || h.generation != generation) return nullptr;
        return &h;
    }

    std::mutex& mutex() { return mutex_; }

    /// Resolve `id` and mark an operation in flight. Returns false when the id
    /// is stale, closed, or was never issued.
    bool acquire(int64_t id, int* fd, HandleKind* kind) {
        std::lock_guard<std::mutex> g(mutex_);
        Handle* h = find(id);
        if (!h) return false;
        ++h->busy;
        *fd = h->fd;
        *kind = h->kind;
        return true;
    }

    /// Finish an operation, closing the descriptor if `close!` was waiting on
    /// this being the last one.
    void release(int64_t id) {
        int to_close = -1;
        {
            std::lock_guard<std::mutex> g(mutex_);
            size_t index = size_t(uint64_t(id) & HANDLE_INDEX_MASK);
            if (index >= slots_.size()) return;
            Handle& h = slots_[index];
            if (--h.busy == 0 && !h.open && h.owned && h.fd >= 0) {
                to_close = h.fd;
                h.fd = -1;
            }
        }
        if (to_close >= 0) sys::close(to_close);
    }

    /// Every open handle, for `std.vm`.
    std::vector<IoHandleInfo> snapshot() {
        std::vector<IoHandleInfo> out;
        std::lock_guard<std::mutex> g(mutex_);
        for (size_t i = 0; i < slots_.size(); ++i) {
            const Handle& h = slots_[i];
            if (!h.open) continue;
            IoHandleInfo info;
            info.handle = int64_t((uint64_t(h.generation) << HANDLE_INDEX_BITS) | uint64_t(i));
            info.fd = h.fd;
            info.kind = h.kind;
            info.busy = h.busy;
            out.push_back(info);
        }
        return out;
    }

    /// Close every handle still open, for shutdown.
    void close_all() {
        std::lock_guard<std::mutex> g(mutex_);
        for (Handle& h : slots_) {
            if (h.open && h.owned && h.fd >= 0) sys::close(h.fd);
            h.open = false;
        }
    }

private:
    std::mutex mutex_;
    /// A deque, not a vector: `add` may grow the table while a caller holds a
    /// `Handle*` from `find`, and a deque does not move what is already there.
    std::deque<Handle> slots_;
};

/// Holds an operation open on a handle for as long as it is in scope.
class Held {
public:
    Held() = default;
    ~Held() { if (ok_) HandleTable::get().release(id_); }
    Held(const Held&) = delete;
    Held& operator=(const Held&) = delete;

    bool open(Value v) {
        Value h = resolve(v);
        if (!is_fixnum(h)) return false;
        id_ = fixnum_value(h);
        ok_ = HandleTable::get().acquire(id_, &fd_, &kind_);
        return ok_;
    }

    int fd() const { return fd_; }
    HandleKind kind() const { return kind_; }
    int64_t id() const { return id_; }
    /// A regular file is always "ready" to `epoll`, so waiting on one is
    /// meaningless and its calls are issued directly.
    bool pollable() const { return kind_ != HandleKind::File; }

private:
    int64_t id_ = -1;
    int fd_ = -1;
    HandleKind kind_ = HandleKind::File;
    bool ok_ = false;
};


// ---------------------------------------------------------------------------
// The poller
//
// One thread, one `epoll` set, and a table of which process is waiting on
// which descriptor. A native that cannot complete registers interest and parks
// its process; this thread wakes the process when the kernel says the
// descriptor is ready.
//
// Interest is registered EPOLLONESHOT: it fires once and is disarmed, so a
// process that is woken but then loses a race for the data simply arms again.
// Level-triggered would spin, and edge-triggered without oneshot would wake
// every waiter on a shared descriptor.
// ---------------------------------------------------------------------------

class Poller {
public:
    static Poller& get() {
        static Poller p;
        return p;
    }

    bool available() const { return running_.load(); }

    void start() {
#if DREAM_HAVE_EPOLL
        if (running_.exchange(true)) return;
        epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd_ < 0) {
            running_ = false;
            return;
        }
        // A descriptor whose only job is to make `epoll_wait` return, so the
        // thread can notice it has been asked to stop.
        wake_fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (wake_fd_ >= 0) {
            epoll_event ev{};
            ev.events = EPOLLIN;
            ev.data.fd = wake_fd_;
            ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wake_fd_, &ev);
        }
        thread_ = std::thread([this] { loop(); });
#else
        if (running_.exchange(true)) return;
        thread_ = std::thread([this] { loop_portable(); });
#endif
    }

    void stop() {
#if DREAM_HAVE_EPOLL
        if (!running_.exchange(false)) return;
        if (wake_fd_ >= 0) {
            uint64_t one = 1;
            [[maybe_unused]] sys::Count n = sys::write(wake_fd_, &one, sizeof one);
        }
        if (thread_.joinable()) thread_.join();
        if (wake_fd_ >= 0) sys::close(wake_fd_);
        if (epoll_fd_ >= 0) sys::close(epoll_fd_);
        wake_fd_ = -1;
        epoll_fd_ = -1;
#else
        if (!running_.exchange(false)) return;
        changed_.notify_all();
        if (thread_.joinable()) thread_.join();
#endif
    }

    /// Wait for `fd` to become readable (or writable) on behalf of `pid`.
    ///
    /// Returns false when this build cannot wait, so the caller can fall back
    /// to a blocking call rather than parking a process nothing will wake.
    bool arm(int fd, bool writable, uint64_t pid, Scheduler* sched) {
#if DREAM_HAVE_EPOLL
        if (epoll_fd_ < 0) return false;
        {
            std::lock_guard<std::mutex> g(mutex_);
            waiters_[fd] = Waiter{pid, sched};
        }
        epoll_event ev{};
        ev.events = uint32_t((writable ? EPOLLOUT : EPOLLIN) | EPOLLONESHOT | EPOLLERR | EPOLLHUP);
        ev.data.fd = fd;
        // MOD first: the descriptor is usually already in the set from an
        // earlier wait, and ADD would fail with EEXIST.
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
            if (errno != ENOENT || ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
                std::lock_guard<std::mutex> g(mutex_);
                waiters_.erase(fd);
                return false;
            }
        }
        sched->note_io_wait(true);
        IOTRACE("arm fd=%d %s pid=%llu", fd, writable ? "w" : "r", (unsigned long long)pid);
        return true;
#else
        std::lock_guard<std::mutex> g(mutex_);
        if (!running_ || waiters_.count(fd)) return false;
        sched->note_io_wait(true);
        waiters_[fd] = Waiter{pid, sched, writable};
        changed_.notify_all();
        return true;
#endif
    }

    /// The process parked on `fd`, or 0 if none is.
    uint64_t waiter_of(int fd) {
        std::lock_guard<std::mutex> g(mutex_);
        auto it = waiters_.find(fd);
        return it == waiters_.end() ? 0 : it->second.pid;
    }

    /// Stop watching `fd`, because it is being closed.
    void forget(int fd) {
#if DREAM_HAVE_EPOLL
        if (epoll_fd_ < 0) return;
        Waiter w{};
        bool had = false;
        {
            std::lock_guard<std::mutex> g(mutex_);
            auto it = waiters_.find(fd);
            if (it != waiters_.end()) {
                w = it->second;
                had = true;
                waiters_.erase(it);
            }
        }
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        // Whoever was waiting must be released, or closing a socket out from
        // under a reader would park that reader for ever. It wakes, retries,
        // and gets the "closed" error the retry produces.
        if (had && w.sched) {
            // Wake before releasing the IO-waiter count; see `run_child` in
            // os.cpp for why the other order invents a deadlock.
            w.sched->wake(w.pid);
            w.sched->note_io_wait(false);
        }
#else
        Waiter w{};
        {
            std::lock_guard<std::mutex> g(mutex_);
            auto it = waiters_.find(fd);
            if (it == waiters_.end()) return;
            w = it->second;
            waiters_.erase(it);
        }
        w.sched->wake(w.pid);
        w.sched->note_io_wait(false);
#endif
    }

private:
    struct Waiter {
        uint64_t pid = 0;
        Scheduler* sched = nullptr;
        bool writable = false;
    };

#if DREAM_HAVE_EPOLL
    void loop() {
        std::vector<epoll_event> events(64);
        while (running_.load(std::memory_order_relaxed)) {
            int n = ::epoll_wait(epoll_fd_, events.data(), int(events.size()), 100);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            for (int i = 0; i < n; ++i) {
                int fd = events[i].data.fd;
                if (fd == wake_fd_) {
                    uint64_t drain = 0;
                    [[maybe_unused]] sys::Count r = sys::read(wake_fd_, &drain, sizeof drain);
                    continue;
                }
                Waiter w{};
                {
                    std::lock_guard<std::mutex> g(mutex_);
                    auto it = waiters_.find(fd);
                    if (it == waiters_.end()) continue;
                    w = it->second;
                    waiters_.erase(it);
                }
                if (w.sched) {
                    IOTRACE("ready fd=%d -> wake pid=%llu", fd, (unsigned long long)w.pid);
                    // Wake first, then release the count; see `run_child`.
                    w.sched->wake(w.pid);
                    w.sched->note_io_wait(false);
                }
            }
        }
    }
#endif

#if !DREAM_HAVE_EPOLL
    void loop_portable() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (running_) {
            for (auto it = waiters_.begin(); it != waiters_.end();) {
                if (!sys::ready(it->first, it->second.writable)) { ++it; continue; }
                Waiter w = it->second;
                it = waiters_.erase(it);
                w.sched->wake(w.pid);
                w.sched->note_io_wait(false);
            }
            changed_.wait_for(lock, std::chrono::milliseconds(5));
        }
    }
    std::condition_variable changed_;
#endif
    std::atomic<bool> running_{false};
    int epoll_fd_ = -1;
    int wake_fd_ = -1;
    std::thread thread_;
    std::mutex mutex_;
    std::unordered_map<int, Waiter> waiters_;
};

/// Park `p` until `fd` is ready. The caller returns the result unchanged.
///
/// Returning `block()` on its own only says "my result is not ready"; asking to
/// be parked is what stops the worker from running this process again straight
/// away. The request is made before the descriptor is armed, so a readiness
/// notification that arrives in between is not lost: `wake` finds the process
/// still running and leaves a note the worker checks before it commits.
NativeResult wait_for(Process& p, int fd, bool writable) {
    Scheduler* sched = p.runtime().scheduler();
    if (!sched) {
        return fail(p, "io_error", "no scheduler is running, so nothing could wake this process");
    }
    {
        std::lock_guard<std::mutex> g(p.sched_mutex);
        p.park_requested = true;
        p.wait_reason.store(WaitReason::Io, std::memory_order_relaxed);
        p.wait_fd.store(fd, std::memory_order_relaxed);
    }
    if (!Poller::get().arm(fd, writable, p.id(), sched)) {
        std::lock_guard<std::mutex> g(p.sched_mutex);
        p.park_requested = false;
        return fail(p, "io_error",
                    "this build cannot wait on a descriptor without blocking a worker");
    }
    return NativeResult::block();
}

bool set_nonblocking(int fd) {
    return sys::nonblocking(fd);
}

// ---------------------------------------------------------------------------
// std.io
// ---------------------------------------------------------------------------

/// Read at most `want` bytes. Returns "" at end of file, which is what makes
/// a read loop terminate without a separate `eof` call racing the read.
NativeResult io_read(Process& p, Value, Value* args, uint32_t) {
    Held h;
    if (!h.open(args[0])) {
        return fail(p, "io_closed", "this handle is closed, or was never opened");
    }
    Value n = resolve(args[1]);
    if (!is_fixnum(n) || fixnum_value(n) < 0) {
        return fail(p, "type_error", "read! needs a byte count");
    }
    size_t want = size_t(fixnum_value(n));
    if (want == 0) return NativeResult::ok(p.heap().make_string("", 0));
    // One call cannot be asked to fill the heap; a reader loops anyway.
    if (want > (1u << 24)) want = 1u << 24;

    std::vector<char> buf(want);
    for (;;) {
        sys::Count got = sys::read(h.fd(), buf.data(), want);
        if (got >= 0) return NativeResult::ok(p.heap().make_string(buf.data(), uint32_t(got)));
        if (errno == EINTR) continue;
        if ((errno == EAGAIN || errno == EWOULDBLOCK) && h.pollable()) {
            return wait_for(p, h.fd(), false);
        }
        return fail_errno(p, "read", errno);
    }
}

/// Write as much of `data` as the descriptor will take, and answer how much
/// that was. A short write is not an error -- `write_all!` is the loop.
NativeResult io_write(Process& p, Value, Value* args, uint32_t) {
    Held h;
    if (!h.open(args[0])) {
        return fail(p, "io_closed", "this handle is closed, or was never opened");
    }
    Bytes str;
    if (!string_bytes(args[1], &str)) return fail(p, "type_error", "write! needs a string");
    if (str.len == 0) return NativeResult::ok(make_fixnum(0));

    for (;;) {
        // A big string is written from where it lies in the mapped image, with
        // no intermediate buffer -- the kernel reads the pages, so a payload
        // larger than memory costs no memory to write. `write` takes a size_t
        // and answers how much it took, and a short write is the caller's loop
        // either way, so nothing here has to know which kind it was handed.
        sys::Count put = sys::write(h.fd(), str.data, size_t(str.len));
        if (put >= 0) return NativeResult::ok(make_fixnum(int64_t(put)));
        if (errno == EINTR) continue;
        if ((errno == EAGAIN || errno == EWOULDBLOCK) && h.pollable()) {
            return wait_for(p, h.fd(), true);
        }
        return fail_errno(p, "write", errno);
    }
}

NativeResult io_open(Process& p, Value, Value* args, uint32_t) {
    if (!is_string(args[0])) return fail(p, "type_error", "open! needs a path");
    std::string path = string_arg(args[0]);

    Value m = resolve(args[1]);
    if (!is_atom(m)) {
        return fail(p, "type_error", "open! needs a mode: :read, :write, :append or :update");
    }
    std::string mode = p.runtime().atom_name(uint32_t(imm_payload(m)));
    int flags;
    if (mode == "read") flags = O_RDONLY;
    else if (mode == "write") flags = O_WRONLY | O_CREAT | O_TRUNC;
    else if (mode == "append") flags = O_WRONLY | O_CREAT | O_APPEND;
    else if (mode == "update") flags = O_RDWR | O_CREAT;
    else return fail(p, "bad_argument", "`:" + mode + "` is not a mode; use :read, :write, :append or :update");

    int fd = sys::open(path.c_str(), flags | O_CLOEXEC, 0644);
    if (fd < 0) return fail_errno(p, "open " + path, errno);

    // What it actually is decides how it can be waited on: opening a fifo by
    // name gives a descriptor that must be polled, not read straight through.
    sys::FileStat st{};
    HandleKind kind = HandleKind::File;
    if (sys::fstat(fd, &st) == 0 && !S_ISREG(st.st_mode)) {
        kind = HandleKind::Stream;
        set_nonblocking(fd);
    }
    return NativeResult::ok(make_fixnum(HandleTable::get().add(fd, kind)));
}

NativeResult io_close(Process& p, Value, Value* args, uint32_t) {
    Value v = resolve(args[0]);
    if (!is_fixnum(v)) return fail(p, "type_error", "close! needs an io handle");
    int fd = -1;
    bool close_now = false;
    {
        std::lock_guard<std::mutex> g(HandleTable::get().mutex());
        Handle* h = HandleTable::get().find(fixnum_value(v));
        if (!h) return fail(p, "io_closed", "this handle is already closed");
        fd = h->fd;
        h->open = false;
        // Nobody is mid-syscall on it, so it can go now. Otherwise the last
        // operation to finish closes it -- see `HandleTable::release`.
        if (h->busy == 0 && h->owned && fd >= 0) {
            h->fd = -1;
            close_now = true;
        }
    }
    // Release any waiter first: closing a socket out from under a reader would
    // otherwise park that reader for ever. It wakes, retries, and gets the
    // "closed" error.
    Poller::get().forget(fd);
    if (close_now) sys::close(fd);
    return NativeResult::ok(UNIT);
}

NativeResult io_flush(Process& p, Value, Value* args, uint32_t) {
    Held h;
    if (!h.open(args[0])) return fail(p, "io_closed", "this handle is closed");
    // Only a regular file has anything buffered in the kernel worth forcing
    // out; asking a socket to fsync is an error, not a no-op.
    if (h.kind() == HandleKind::File && sys::fsync(h.fd()) < 0) {
        return fail_errno(p, "flush", errno);
    }
    return NativeResult::ok(UNIT);
}

NativeResult io_seek(Process& p, Value, Value* args, uint32_t) {
    Held h;
    if (!h.open(args[0])) return fail(p, "io_closed", "this handle is closed");
    Value off = resolve(args[1]);
    if (!is_fixnum(off)) return fail(p, "type_error", "seek! needs a byte offset");
    sys::Offset where = sys::lseek(h.fd(), sys::Offset(fixnum_value(off)), SEEK_SET);
    if (where < 0) return fail_errno(p, "seek", errno);
    return NativeResult::ok(make_fixnum(int64_t(where)));
}

NativeResult io_size(Process& p, Value, Value* args, uint32_t) {
    Held h;
    if (!h.open(args[0])) return fail(p, "io_closed", "this handle is closed");
    sys::FileStat st{};
    if (sys::fstat(h.fd(), &st) < 0) return fail_errno(p, "size", errno);
    return NativeResult::ok(make_fixnum(int64_t(st.st_size)));
}

NativeResult io_kind(Process& p, Value, Value* args, uint32_t) {
    Held h;
    if (!h.open(args[0])) return fail(p, "io_closed", "this handle is closed");
    const char* name = h.kind() == HandleKind::File     ? "file"
                     : h.kind() == HandleKind::Listener ? "listener"
                                                        : "stream";
    return NativeResult::ok(make_atom(p.runtime().intern_atom(name)));
}

NativeResult io_is_open(Process& p, Value, Value* args, uint32_t) {
    Value v = resolve(args[0]);
    if (!is_fixnum(v)) return NativeResult::ok(make_bool(false));
    std::lock_guard<std::mutex> g(HandleTable::get().mutex());
    return NativeResult::ok(make_bool(HandleTable::get().find(fixnum_value(v)) != nullptr));
}

/// Whether waiting on a descriptor parks the process rather than the worker.
NativeResult io_async(Process&, Value, Value*, uint32_t) {
    return NativeResult::ok(make_bool(io_async_available()));
}

// --- the standard streams ---------------------------------------------------
//
// Handed out as handles the program does not own: closing one would close the
// process's real stdin, which is not this VM's to do.

int64_t std_handle(int fd) {
    static std::mutex mutex;
    static int64_t ids[3] = {-1, -1, -1};
    std::lock_guard<std::mutex> g(mutex);
    if (ids[fd] < 0) {
        sys::FileStat st{};
        bool regular = sys::fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
        ids[fd] = HandleTable::get().add(fd, regular ? HandleKind::File : HandleKind::Stream,
                                         /*owned=*/false);
    }
    return ids[fd];
}

NativeResult io_stdin(Process&, Value, Value*, uint32_t) {
    return NativeResult::ok(make_fixnum(std_handle(0)));
}
NativeResult io_stdout(Process&, Value, Value*, uint32_t) {
    return NativeResult::ok(make_fixnum(std_handle(1)));
}
NativeResult io_stderr(Process&, Value, Value*, uint32_t) {
    return NativeResult::ok(make_fixnum(std_handle(2)));
}

// --- paths ------------------------------------------------------------------

NativeResult io_exists(Process& p, Value, Value* args, uint32_t) {
    if (!is_string(args[0])) return fail(p, "type_error", "exists! needs a path");
    sys::FileStat st{};
    return NativeResult::ok(make_bool(sys::stat(string_arg(args[0]).c_str(), &st) == 0));
}

NativeResult io_is_dir(Process& p, Value, Value* args, uint32_t) {
    if (!is_string(args[0])) return fail(p, "type_error", "is_dir! needs a path");
    sys::FileStat st{};
    if (sys::stat(string_arg(args[0]).c_str(), &st) != 0) return NativeResult::ok(make_bool(false));
    return NativeResult::ok(make_bool(S_ISDIR(st.st_mode)));
}

NativeResult io_remove(Process& p, Value, Value* args, uint32_t) {
    if (!is_string(args[0])) return fail(p, "type_error", "remove! needs a path");
    std::string path = string_arg(args[0]);
    if (sys::remove(path.c_str()) != 0) return fail_errno(p, "remove " + path, errno);
    return NativeResult::ok(UNIT);
}

NativeResult io_rename(Process& p, Value, Value* args, uint32_t) {
    if (!is_string(args[0]) || !is_string(args[1])) {
        return fail(p, "type_error", "rename! needs two paths");
    }
    std::string from = string_arg(args[0]);
    if (sys::rename(from.c_str(), string_arg(args[1]).c_str()) != 0) {
        return fail_errno(p, "rename " + from, errno);
    }
    return NativeResult::ok(UNIT);
}

NativeResult io_mkdir(Process& p, Value, Value* args, uint32_t) {
    if (!is_string(args[0])) return fail(p, "type_error", "mkdir! needs a path");
    std::string path = string_arg(args[0]);
    if (sys::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
        return fail_errno(p, "mkdir " + path, errno);
    }
    return NativeResult::ok(UNIT);
}

// ---------------------------------------------------------------------------
// std.net
//
// Sockets are handles like any other, so `io.read!` and `io.write!` work on
// them unchanged -- that is what "net on top of io" means here. Only the
// operations that have no file equivalent live in this module: listening,
// accepting, and connecting.
// ---------------------------------------------------------------------------

/// Bind and listen on `port`, on every interface.
NativeResult net_listen(Process& p, Value, Value* args, uint32_t) {
    Value v = resolve(args[0]);
    if (!is_fixnum(v) || fixnum_value(v) < 0 || fixnum_value(v) > 65535) {
        return fail(p, "bad_argument", "listen! needs a port between 0 and 65535");
    }
    int fd = sys::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return fail_errno(p, "socket", errno);

    // Without this, a listener that has just exited leaves the port unusable
    // for a minute or two, which makes every restart of a server a coin toss.
    int on = 1;
    sys::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(uint16_t(fixnum_value(v)));
    if (sys::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        int e = errno;
        sys::close(fd);
        return fail_errno(p, "bind", e);
    }
    if (sys::listen(fd, 128) < 0) {
        int e = errno;
        sys::close(fd);
        return fail_errno(p, "listen", e);
    }
    set_nonblocking(fd);
    return NativeResult::ok(make_fixnum(HandleTable::get().add(fd, HandleKind::Listener)));
}

/// Take the next incoming connection, parking this process until one arrives.
NativeResult net_accept(Process& p, Value, Value* args, uint32_t) {
    Held h;
    if (!h.open(args[0])) return fail(p, "io_closed", "this listener is closed");
    if (h.kind() != HandleKind::Listener) {
        return fail(p, "wrong_kind", "accept! needs a listening socket");
    }

    for (;;) {
        int client = sys::accept(h.fd(), nullptr, nullptr);
        if (client >= 0) {
            set_nonblocking(client);
            int on = 1;
            // Small writes should go out now, not wait for more to accumulate.
            sys::setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
            return NativeResult::ok(
                make_fixnum(HandleTable::get().add(client, HandleKind::Stream)));
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return wait_for(p, h.fd(), false);
        return fail_errno(p, "accept", errno);
    }
}

/// Open a connection to `host` on `port`.
///
/// This is the one operation that cannot simply be retried on wake-up: the
/// second `connect` on the same socket does not redo the work, it reports what
/// happened. So the handle remembers that a connection is in flight, and the
/// retry asks the socket how it went.
NativeResult net_connect_finish(Process& p);

NativeResult net_connect(Process& p, Value, Value* args, uint32_t) {
    // Woken from the wait below: the attempt is over, and the socket knows
    // whether it worked.
    if (p.io_pending >= 0) return net_connect_finish(p);

    if (!is_string(args[0])) return fail(p, "type_error", "connect! needs a host name");
    Value pv = resolve(args[1]);
    if (!is_fixnum(pv) || fixnum_value(pv) < 0 || fixnum_value(pv) > 65535) {
        return fail(p, "bad_argument", "connect! needs a port between 0 and 65535");
    }
    std::string host = string_arg(args[0]);
    std::string port = std::to_string(fixnum_value(pv));

    // `getaddrinfo` blocks, and there is no portable non-blocking resolver.
    // It is called once per connection, before the socket exists, so it stalls
    // one worker briefly rather than for the life of the connection.
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    int gai = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &found);
    if (gai != 0 || !found) {
        return fail(p, "not_found", "cannot resolve " + host + ": " + ::gai_strerror(gai));
    }

    int fd = sys::socket(found->ai_family, found->ai_socktype, found->ai_protocol);
    if (fd < 0) {
        int e = errno;
        ::freeaddrinfo(found);
        return fail_errno(p, "socket", e);
    }
    set_nonblocking(fd);
    int rc = sys::connect(fd, found->ai_addr, static_cast<sys::SockLen>(found->ai_addrlen));
    int e = errno;
    ::freeaddrinfo(found);

    if (rc == 0) {
        int on = 1;
        sys::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
        return NativeResult::ok(make_fixnum(HandleTable::get().add(fd, HandleKind::Stream)));
    }
    if (e != EINPROGRESS && e != EINTR) {
        sys::close(fd);
        return fail_errno(p, "connect to " + host, e);
    }

    // In flight. Hand back a handle so the retry has somewhere to keep state,
    // and wait for the socket to become writable, which is how the kernel
    // reports that the attempt has finished one way or the other.
    int64_t id = HandleTable::get().add(fd, HandleKind::Stream);
    {
        std::lock_guard<std::mutex> g(HandleTable::get().mutex());
        if (Handle* h = HandleTable::get().find(id)) h->connecting = true;
    }
    p.io_pending = id;
    return wait_for(p, fd, true);
}

/// The second half of `connect!`, reached when the socket became writable.
NativeResult net_connect_finish(Process& p) {
    int64_t id = p.io_pending;
    p.io_pending = -1;
    std::unique_lock<std::mutex> lock(HandleTable::get().mutex());
    Handle* h = HandleTable::get().find(id);
    if (!h) return fail(p, "io_closed", "the connection was closed while it was being made");
    h->connecting = false;

    int err = 0;
    sys::SockLen len = sizeof err;
    if (sys::getsockopt(h->fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) err = errno;
    if (err != 0) {
        int fd = h->fd;
        h->open = false;
        h->fd = -1;
        lock.unlock();
        sys::close(fd);
        return fail_errno(p, "connect", err);
    }
    int on = 1;
    sys::setsockopt(h->fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
    lock.unlock();
    return NativeResult::ok(make_fixnum(id));
}

/// The address of the other end of a connection, as "host:port".
NativeResult net_peer(Process& p, Value, Value* args, uint32_t) {
    Held h;
    if (!h.open(args[0])) return fail(p, "io_closed", "this socket is closed");
    sockaddr_in addr{};
    sys::SockLen len = sizeof addr;
    if (sys::getpeername(h.fd(), reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
        return fail_errno(p, "peer", errno);
    }
    char text[INET_ADDRSTRLEN] = {0};
    ::inet_ntop(AF_INET, &addr.sin_addr, text, sizeof text);
    std::string out = std::string(text) + ":" + std::to_string(ntohs(addr.sin_port));
    return NativeResult::ok(p.heap().make_string(out.data(), uint32_t(out.size())));
}

/// The port a listener actually got, which is the only way to learn it after
/// asking for port 0.
NativeResult net_port(Process& p, Value, Value* args, uint32_t) {
    Held h;
    if (!h.open(args[0])) return fail(p, "io_closed", "this socket is closed");
    sockaddr_in addr{};
    sys::SockLen len = sizeof addr;
    if (sys::getsockname(h.fd(), reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
        return fail_errno(p, "port", errno);
    }
    return NativeResult::ok(make_fixnum(int64_t(ntohs(addr.sin_port))));
}

/// Stop sending, so the other end sees end of file while this end can still
/// read what is already on the way.
NativeResult net_shutdown(Process& p, Value, Value* args, uint32_t) {
    Held h;
    if (!h.open(args[0])) return fail(p, "io_closed", "this socket is closed");
    sys::shutdown(h.fd(), SHUT_WR);
    return NativeResult::ok(UNIT);
}

// ---------------------------------------------------------------------------
// What a build asks of the file system
//
// Stamps, digests, links, trees. A build that has nothing to do must cost a
// stat per input and nothing more (docs/build.md, "Running a plan"), so the
// stamp is one native, and a digest is read here rather than a file being
// read into a Dream string and hashed there.
//
// These run on the worker. A stat is too quick to be worth parking for; a
// digest of a large file is not, and if a build turns out to spend its time
// here, `digest!` is the one to move to a helper thread as `exec!` was.
// ---------------------------------------------------------------------------

std::filesystem::path fs_path(const std::string& utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string fs_text(const std::filesystem::path& path) {
    auto bytes = path.generic_u8string();
    return std::string(bytes.begin(), bytes.end());
}

NativeResult fail_fs(Process& p, const std::string& what, const std::error_code& ec) {
    const char* kind = "io_error";
    if (ec == std::errc::no_such_file_or_directory) kind = "not_found";
    else if (ec == std::errc::permission_denied || ec == std::errc::operation_not_permitted) kind = "permission_denied";
    else if (ec == std::errc::file_exists) kind = "already_exists";
    else if (ec == std::errc::not_a_directory || ec == std::errc::is_a_directory) kind = "wrong_kind";
    return fail(p, kind, what + ": " + ec.message());
}

/// `stat! path` -- `%{ :kind, :size, :modified }`, or `()` when there is
/// nothing there. `:modified` is in nanoseconds and means nothing but itself:
/// it is a stamp to compare with the last one, never a time to show anyone.
/// A missing file is an answer rather than an error, because "is it still
/// there" is half of what a stamp is asked.
NativeResult io_stat(Process& p, Value, Value* args, uint32_t) {
    if (!is_string(args[0])) return fail(p, "type_error", "stat! needs a path");
    std::string path = string_arg(args[0]);
    const char* kind = "other";
    int64_t size = 0, modified = 0;
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(windows::wide(path).c_str(), GetFileExInfoStandard, &data)) {
        return NativeResult::ok(UNIT);
    }
    kind = data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? "dir" : "file";
    size = int64_t((uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow);
    // 100ns ticks since 1601, made nanoseconds since 1970 so the two platforms
    // agree on what a stamp is.
    uint64_t ticks = (uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
    modified = (int64_t(ticks) - 116444736000000000LL) * 100;
#else
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) return NativeResult::ok(UNIT);
        return fail_errno(p, "stat " + path, errno);
    }
    kind = S_ISREG(st.st_mode) ? "file" : S_ISDIR(st.st_mode) ? "dir" : "other";
    size = int64_t(st.st_size);
#if defined(__APPLE__)
    modified = int64_t(st.st_mtimespec.tv_sec) * 1000000000 + st.st_mtimespec.tv_nsec;
#else
    modified = int64_t(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec;
#endif
#endif
    Value m = p.heap().make_map(4);
    m = resolve(map_insert(p, m, make_atom(p.runtime().intern_atom("kind")),
                           make_atom(p.runtime().intern_atom(kind))));
    m = resolve(map_insert(p, m, make_atom(p.runtime().intern_atom("size")), make_integer(p, size)));
    m = resolve(map_insert(p, m, make_atom(p.runtime().intern_atom("modified")), make_integer(p, modified)));
    return NativeResult::ok(m);
}

/// `digest! path` -- the SHA-256 of a file's contents, as 64 hex digits.
NativeResult io_digest_file(Process& p, Value, Value* args, uint32_t) {
    if (!is_string(args[0])) return fail(p, "type_error", "digest! needs a path");
    std::string path = string_arg(args[0]);
    int fd = sys::open(path.c_str(), O_RDONLY, 0);
    if (fd < 0) return fail_errno(p, "digest " + path, errno);
    Sha256 sha;
    std::vector<char> buf(1 << 16);
    for (;;) {
        auto n = sys::read(fd, buf.data(), buf.size());
        if (n > 0) { sha.update(buf.data(), size_t(n)); continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            int e = errno;
            sys::close(fd);
            return fail_errno(p, "digest " + path, e);
        }
        break;
    }
    sys::close(fd);
    std::string hex = sha.hex();
    return NativeResult::ok(p.heap().make_string(hex.data(), uint32_t(hex.size())));
}

/// `digest text` -- the SHA-256 of a string. Pure: the same text always has
/// the same digest, which is what lets a step's key be worked out in a plan.
NativeResult io_digest_text(Process& p, Value, Value* args, uint32_t) {
    Value v = resolve(args[0]);
    Sha256 sha;
    if (is_obj(v, ObjType::Str)) {
        auto* s = static_cast<StrObj*>(as_obj(v));
        sha.update(s->data(), s->len);
    } else if (is_obj(v, ObjType::BigStr)) {
        auto* s = static_cast<BigStrObj*>(as_obj(v));
        sha.update(s->data, size_t(s->len));
    } else {
        return fail(p, "type_error", "digest needs a string");
    }
    std::string hex = sha.hex();
    return NativeResult::ok(p.heap().make_string(hex.data(), uint32_t(hex.size())));
}

/// `link! from to` -- `to` becomes another name for `from`: a hard link, or a
/// copy where one cannot be made (another volume, a file system without
/// them). Either way `to` is new and must not exist yet. A step's outputs are
/// never written to again once made, so a link is as good as a copy and costs
/// nothing; placing a result is a link beside it and a `rename!` over it.
NativeResult io_link(Process& p, Value, Value* args, uint32_t) {
    if (!is_string(args[0]) || !is_string(args[1])) return fail(p, "type_error", "link! needs two paths");
    auto from = fs_path(string_arg(args[0]));
    auto to = fs_path(string_arg(args[1]));
    std::error_code ec;
    std::filesystem::create_hard_link(from, to, ec);
    if (!ec) return NativeResult::ok(make_atom(p.runtime().intern_atom("linked")));
    if (ec == std::errc::file_exists || ec == std::errc::no_such_file_or_directory) {
        return fail_fs(p, "link " + fs_text(from) + " to " + fs_text(to), ec);
    }
    ec.clear();
    std::filesystem::copy_file(from, to, std::filesystem::copy_options::none, ec);
    if (ec) return fail_fs(p, "copy " + fs_text(from) + " to " + fs_text(to), ec);
    return NativeResult::ok(make_atom(p.runtime().intern_atom("copied")));
}

/// `chmod! path mode` -- set the permission bits, `0o755` being `493`. On
/// Windows only the owner's write bit means anything (it is the read-only
/// attribute), and the rest are accepted and ignored, so a build can mark its
/// outputs executable without asking what it is running on.
NativeResult io_chmod(Process& p, Value, Value* args, uint32_t) {
    Value mode = resolve(args[1]);
    if (!is_string(args[0]) || !is_fixnum(mode) || fixnum_value(mode) < 0 || fixnum_value(mode) > 07777) {
        return fail(p, "type_error", "chmod! needs a path and a mode");
    }
    std::error_code ec;
    std::filesystem::permissions(fs_path(string_arg(args[0])),
                                 std::filesystem::perms(fixnum_value(mode)), ec);
    if (ec) return fail_fs(p, "chmod " + string_arg(args[0]), ec);
    return NativeResult::ok(UNIT);
}

/// `walk! dir` -- every file under `dir`, as paths relative to it with `/`
/// between the parts, sorted. Directories are walked and not listed, and a
/// link to a directory is not followed, so a tree that links back into itself
/// is still finite. Picking files by pattern is left to the caller.
NativeResult io_walk(Process& p, Value, Value* args, uint32_t) {
    if (!is_string(args[0])) return fail(p, "type_error", "walk! needs a directory");
    auto root = fs_path(string_arg(args[0]));
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, ec), end;
    if (ec) return fail_fs(p, "walk " + fs_text(root), ec);
    std::vector<std::string> files;
    for (; it != end; it.increment(ec)) {
        if (ec) return fail_fs(p, "walk " + fs_text(root), ec);
        std::error_code kind_ec;
        if (it->is_regular_file(kind_ec)) files.push_back(fs_text(it->path().lexically_relative(root)));
    }
    std::sort(files.begin(), files.end());
    Value list = NIL;
    for (size_t i = files.size(); i-- > 0;) {
        list = p.heap().make_cons(p.heap().make_string(files[i].data(), uint32_t(files[i].size())), list);
    }
    return NativeResult::ok(list);
}

/// `mkdir_all! path` -- the directory and any parents it needs. Already there
/// is not an error.
NativeResult io_mkdir_all(Process& p, Value, Value* args, uint32_t) {
    if (!is_string(args[0])) return fail(p, "type_error", "mkdir_all! needs a path");
    std::error_code ec;
    std::filesystem::create_directories(fs_path(string_arg(args[0])), ec);
    if (ec) return fail_fs(p, "mkdir " + string_arg(args[0]), ec);
    return NativeResult::ok(UNIT);
}

/// `remove_all! path` -- the path and everything under it, answering how many
/// entries went. Nothing there is not an error and answers 0: a scratch
/// directory is cleared whether or not the last run got as far as making it.
NativeResult io_remove_all(Process& p, Value, Value* args, uint32_t) {
    if (!is_string(args[0])) return fail(p, "type_error", "remove_all! needs a path");
    std::error_code ec;
    auto removed = std::filesystem::remove_all(fs_path(string_arg(args[0])), ec);
    if (ec) return fail_fs(p, "remove " + string_arg(args[0]), ec);
    return NativeResult::ok(make_integer(p, int64_t(removed)));
}

}  // namespace

// ---------------------------------------------------------------------------
// Wiring
// ---------------------------------------------------------------------------

void io_init() { if (sys::init()) Poller::get().start(); }

void io_shutdown() {
    Poller::get().stop();
    HandleTable::get().close_all();
}

bool io_async_available() { return Poller::get().available(); }

std::vector<IoHandleInfo> io_snapshot() {
    // The waiter is asked for outside the table lock: the two are separate
    // locks, and taking them together here would invent an ordering that
    // nothing else obeys.
    std::vector<IoHandleInfo> out = HandleTable::get().snapshot();
    for (IoHandleInfo& info : out) info.waiting_pid = Poller::get().waiter_of(info.fd);
    return out;
}

ModuleDef make_io_module() {
    return ModuleDef{"std.io",
                     {
                         {"open!", 2, 0b11, io_open},
                         {"close!", 1, 0b1, io_close},
                         {"read!", 2, 0b11, io_read},
                         {"write!", 2, 0b11, io_write},
                         {"flush!", 1, 0b1, io_flush},
                         {"seek!", 2, 0b11, io_seek},
                         {"size!", 1, 0b1, io_size},
                         {"kind!", 1, 0b1, io_kind},
                         {"is_open!", 1, 0b1, io_is_open},
                         {"stdin!", 1, 0b1, io_stdin},
                         {"stdout!", 1, 0b1, io_stdout},
                         {"stderr!", 1, 0b1, io_stderr},
                         {"exists!", 1, 0b1, io_exists},
                         {"is_dir!", 1, 0b1, io_is_dir},
                         {"remove!", 1, 0b1, io_remove},
                         {"rename!", 2, 0b11, io_rename},
                         {"mkdir!", 1, 0b1, io_mkdir},
                         {"mkdir_all!", 1, 0b1, io_mkdir_all},
                         {"remove_all!", 1, 0b1, io_remove_all},
                         {"stat!", 1, 0b1, io_stat},
                         {"digest!", 1, 0b1, io_digest_file},
                         {"digest", 1, 0b1, io_digest_text},
                         {"link!", 2, 0b11, io_link},
                         {"chmod!", 2, 0b11, io_chmod},
                         {"walk!", 1, 0b1, io_walk},
                         {"async", 1, 0b1, io_async},
                     }};
}

ModuleDef make_net_module() {
    return ModuleDef{"std.net",
                     {
                         {"listen!", 1, 0b1, net_listen},
                         {"accept!", 1, 0b1, net_accept},
                         {"connect!", 2, 0b11, net_connect},
                         {"peer!", 1, 0b1, net_peer},
                         {"port!", 1, 0b1, net_port},
                         {"shutdown!", 1, 0b1, net_shutdown},
                     }};
}

}  // namespace dream
