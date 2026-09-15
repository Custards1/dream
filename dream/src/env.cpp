#include "env.hpp"

#include <cstdlib>
#include <cstring>

namespace dream {

namespace {

/// A flag that is on when the variable is set to anything but nothing and `0`.
/// Spelling `=0` for "off" matters because these are set by recipes as often as
/// by hand, and a recipe that wants to turn one off should be able to say so.
bool flag(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && std::strcmp(v, "0") != 0;
}

/// A positive count, or the fallback. Zero and negatives are rejected rather
/// than clamped: a limit of nothing is a mistake in the caller's shell, and
/// answering it with the default is friendlier than answering it with a VM
/// that cannot run a program.
size_t count(const char* name, size_t fallback) {
    if (const char* v = std::getenv(name)) {
        long long n = std::atoll(v);
        if (n > 0) return size_t(n);
    }
    return fallback;
}

}  // namespace

EnvConfig g_env = [] {
    EnvConfig c{};

    c.max_conts = count("DREAM_MAX_DEPTH", size_t(4u) << 20);
    c.max_stack = count("DREAM_MAX_STACK", size_t(4u) << 20);
    c.max_heap = count("DREAM_MAX_HEAP", size_t(1u) << 30);

    c.nursery_max = count("DREAM_NURSERY_MAX", size_t(32) << 20);

    // Zero is meaningful here where it is a mistake everywhere else -- it is
    // how the race detector asks for the parallel path on a tiny collection --
    // so this one is parsed rather than counted.
    c.gc_par_min = size_t(1) << 20;
    if (const char* v = std::getenv("DREAM_GC_PAR_MIN")) {
        long long n = std::atoll(v);
        if (n >= 0) c.gc_par_min = size_t(n);
    }

    c.gc_threads = 0;
    if (const char* v = std::getenv("DREAM_GC_THREADS")) {
        long long n = std::atoll(v);
        if (n > 0) c.gc_threads = unsigned(n < 64 ? n : 64);
    }

    c.gc_concurrent = -1;
    if (const char* v = std::getenv("DREAM_GC_CONCURRENT")) {
        if (std::strcmp(v, "1") == 0) c.gc_concurrent = 1;
        else if (std::strcmp(v, "0") == 0) c.gc_concurrent = 0;
    }

    c.gc_trace = flag("DREAM_GC_TRACE");
    c.verify_heap = flag("DREAM_VERIFY_HEAP");
    c.probe_thunk = flag("DREAM_PROBE_THUNK");
    c.io_trace = flag("DREAM_IO_TRACE");

    c.stuck_seconds = 0.0;
    if (const char* v = std::getenv("DREAM_STUCK_SECONDS")) c.stuck_seconds = std::atof(v);

    return c;
}();

}  // namespace dream
