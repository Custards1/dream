// The GPU backend: OpenCL, loaded at run time. gpu.hpp says why it is shaped
// this way; this file is the plumbing.
//
// The handful of OpenCL types and constants used here are declared below
// rather than taken from <CL/cl.h>, so that building the VM needs no OpenCL
// headers either. They are fixed by the specification and by every ICD loader's
// ABI, which is what a dlopen'd library has to agree with in any case.

#include "gpu.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "tensor_kernels.hpp"

#if defined(_WIN32)
#include "windows.hpp"
#else
#include <dlfcn.h>
#endif

namespace dream::gpu {
namespace {

// --- the slice of OpenCL 1.2 this uses -----------------------------------------

using cl_int = int32_t;
using cl_uint = uint32_t;
using cl_ulong = uint64_t;
using cl_bitfield = cl_ulong;
using cl_platform_id = struct _cl_platform_id*;
using cl_device_id = struct _cl_device_id*;
using cl_context = struct _cl_context*;
using cl_command_queue = struct _cl_command_queue*;
using cl_mem = struct _cl_mem*;
using cl_program = struct _cl_program*;
using cl_kernel = struct _cl_kernel*;
using cl_event = struct _cl_event*;

constexpr cl_int CL_SUCCESS = 0;
constexpr cl_bitfield CL_DEVICE_TYPE_GPU = 1 << 2;
constexpr cl_bitfield CL_DEVICE_TYPE_ALL = 0xFFFFFFFF;
constexpr cl_uint CL_DEVICE_MAX_WORK_GROUP_SIZE = 0x1004;
constexpr cl_uint CL_DEVICE_NAME = 0x102B;
constexpr cl_uint CL_DEVICE_EXTENSIONS = 0x1030;
constexpr cl_bitfield CL_MEM_READ_WRITE = 1 << 0;
constexpr cl_uint CL_TRUE = 1;
constexpr cl_uint CL_PROGRAM_BUILD_LOG = 0x1183;

#if defined(_WIN32)
#define DREAM_CL_API __stdcall
#else
#define DREAM_CL_API
#endif

struct Api {
    cl_int(DREAM_CL_API* GetPlatformIDs)(cl_uint, cl_platform_id*, cl_uint*);
    cl_int(DREAM_CL_API* GetDeviceIDs)(cl_platform_id, cl_bitfield, cl_uint, cl_device_id*, cl_uint*);
    cl_int(DREAM_CL_API* GetDeviceInfo)(cl_device_id, cl_uint, size_t, void*, size_t*);
    cl_context(DREAM_CL_API* CreateContext)(const intptr_t*, cl_uint, const cl_device_id*, void*,
                                            void*, cl_int*);
    cl_command_queue(DREAM_CL_API* CreateCommandQueue)(cl_context, cl_device_id, cl_bitfield, cl_int*);
    cl_mem(DREAM_CL_API* CreateBuffer)(cl_context, cl_bitfield, size_t, void*, cl_int*);
    cl_int(DREAM_CL_API* ReleaseMemObject)(cl_mem);
    cl_int(DREAM_CL_API* EnqueueWriteBuffer)(cl_command_queue, cl_mem, cl_uint, size_t, size_t,
                                             const void*, cl_uint, const cl_event*, cl_event*);
    cl_int(DREAM_CL_API* EnqueueReadBuffer)(cl_command_queue, cl_mem, cl_uint, size_t, size_t,
                                            void*, cl_uint, const cl_event*, cl_event*);
    cl_int(DREAM_CL_API* EnqueueCopyBuffer)(cl_command_queue, cl_mem, cl_mem, size_t, size_t, size_t,
                                            cl_uint, const cl_event*, cl_event*);
    cl_program(DREAM_CL_API* CreateProgramWithSource)(cl_context, cl_uint, const char**,
                                                      const size_t*, cl_int*);
    cl_int(DREAM_CL_API* BuildProgram)(cl_program, cl_uint, const cl_device_id*, const char*,
                                       void*, void*);
    cl_int(DREAM_CL_API* GetProgramBuildInfo)(cl_program, cl_device_id, cl_uint, size_t, void*,
                                              size_t*);
    cl_kernel(DREAM_CL_API* CreateKernel)(cl_program, const char*, cl_int*);
    cl_int(DREAM_CL_API* SetKernelArg)(cl_kernel, cl_uint, size_t, const void*);
    cl_int(DREAM_CL_API* EnqueueNDRangeKernel)(cl_command_queue, cl_kernel, cl_uint, const size_t*,
                                               const size_t*, const size_t*, cl_uint,
                                               const cl_event*, cl_event*);
    cl_int(DREAM_CL_API* Finish)(cl_command_queue);
    cl_int(DREAM_CL_API* Flush)(cl_command_queue);
    cl_int(DREAM_CL_API* EnqueueMarkerWithWaitList)(cl_command_queue, cl_uint, const cl_event*,
                                                    cl_event*);
    cl_int(DREAM_CL_API* WaitForEvents)(cl_uint, const cl_event*);
    cl_int(DREAM_CL_API* ReleaseEvent)(cl_event);
};

// --- the kernels ---------------------------------------------------------------
//
// One source, built once per element type with `T` defined to it. The
// operation codes are tensor_kernels.hpp's, so the host and the device agree on
// what `op == 3` means. Every constant is written `(T)n`: a bare `1.0` is a
// double, and a device without doubles refuses the whole program over it.

const char* const kSource = R"CL(
#ifdef FP64
#pragma OPENCL EXTENSION cl_khr_fp64 : enable
#endif

inline T apply(int op, T a, T b) {
    switch (op) {
        case 0: return a + b;
        case 1: return a - b;
        case 2: return a * b;
        case 3: return a / b;
        default: return fmod(a, b);
    }
}

__kernel void binary(int op, __global const T* x, ulong nx, __global const T* y, ulong ny,
                     __global T* out, ulong n) {
    ulong i = get_global_id(0);
    if (i >= n) return;
    out[i] = apply(op, x[nx == n ? i : i % nx], y[ny == n ? i : i % ny]);
}

__kernel void scalar(int op, __global const T* x, T s, int left, __global T* out, ulong n) {
    ulong i = get_global_id(0);
    if (i >= n) return;
    out[i] = left ? apply(op, s, x[i]) : apply(op, x[i], s);
}

__kernel void unary(int fn, __global const T* x, __global T* out, ulong n) {
    ulong i = get_global_id(0);
    if (i >= n) return;
    T a = x[i];
    T r;
    switch (fn) {
        case 0: r = -a; break;
        case 1: r = sqrt(a); break;
        case 2: r = exp(a); break;
        case 3: r = log(a); break;
        case 4: r = fabs(a); break;
        case 5: r = tanh(a); break;
        case 6: r = sin(a); break;
        case 7: r = cos(a); break;
        case 8: r = a > (T)0 ? a : (T)0; break;
        default: r = (T)1 / ((T)1 + exp(-a)); break;
    }
    out[i] = r;
}

// The textbook tiled product: each work-group computes a TS x TS tile of C,
// stepping along K a tile at a time through local memory, so every element of
// A and B is read from global memory once per tile rather than once per
// product term.
__kernel void matmul(__global const T* A, __global const T* B, __global T* C,
                     int M, int K, int N) {
    __local T As[TS][TS];
    __local T Bs[TS][TS];
    int col = get_global_id(0), row = get_global_id(1);
    int lc = get_local_id(0), lr = get_local_id(1);
    T acc = (T)0;
    for (int t = 0; t < K; t += TS) {
        As[lr][lc] = (row < M && t + lc < K) ? A[row * K + t + lc] : (T)0;
        Bs[lr][lc] = (t + lr < K && col < N) ? B[(t + lr) * N + col] : (T)0;
        barrier(CLK_LOCAL_MEM_FENCE);
        for (int k = 0; k < TS; ++k) acc += As[lr][k] * Bs[k][lc];
        barrier(CLK_LOCAL_MEM_FENCE);
    }
    if (row < M && col < N) C[row * N + col] = acc;
}

// A product with one column: one work-item per row, a dot product each. The
// tiled kernel would run sixteen work-items per row and throw fifteen away.
__kernel void matvec(__global const T* A, __global const T* x, __global T* y, int M, int K) {
    int i = get_global_id(0);
    if (i >= M) return;
    __global const T* a = A + (ulong)i * K;
    T acc = (T)0;
    for (int k = 0; k < K; ++k) acc += a[k] * x[k];
    y[i] = acc;
}

__kernel void transpose(__global const T* in, __global T* out, int R, int C) {
    int j = get_global_id(0), i = get_global_id(1);
    if (i < R && j < C) out[(ulong)j * R + i] = in[(ulong)i * C + j];
}

inline T combine(int op, T a, T b) {
    return op == 0 ? a + b : (op == 1 ? fmin(a, b) : fmax(a, b));
}

// Each work-group folds a stride of the input and then its own scratch; the
// host finishes the partials, in double precision.
__kernel void reduce(int op, __global const T* x, ulong n, __global T* partial,
                     __local T* scratch) {
    ulong gid = get_global_id(0), gsz = get_global_size(0);
    uint lid = get_local_id(0), lsz = get_local_size(0);
    T acc = op == 0 ? (T)0 : x[0];
    for (ulong i = gid; i < n; i += gsz) acc = combine(op, acc, x[i]);
    scratch[lid] = acc;
    barrier(CLK_LOCAL_MEM_FENCE);
    for (uint s = lsz / 2; s > 0; s >>= 1) {
        if (lid < s) scratch[lid] = combine(op, scratch[lid], scratch[lid + s]);
        barrier(CLK_LOCAL_MEM_FENCE);
    }
    if (lid == 0) partial[get_group_id(0)] = scratch[0];
}
)CL";

/// The kernels built for one element type.
struct Program {
    cl_program program = nullptr;
    cl_kernel binary = nullptr, scalar = nullptr, unary = nullptr, matmul = nullptr,
              matvec = nullptr, transpose = nullptr, reduce = nullptr;
};

struct Device {
    Api cl{};
    cl_device_id device = nullptr;
    cl_context context = nullptr;
    cl_command_queue queue = nullptr;
    Program programs[2];  // by Dtype
    bool f64 = false;
    size_t tile = 16;
    size_t group = 64;    // the reduction's work-group size, a power of two
    std::string name;
    std::string why;      // empty when usable
    /// How far ahead of the device the host may run. See `Launch::run`.
    unsigned launches = 0;
    cl_event previous_marker = nullptr;
    /// clSetKernelArg is not safe on one kernel from two threads, and every
    /// process that computes on the GPU shares these kernels. One lock around
    /// set-and-enqueue is all the ordering the queue needs.
    std::mutex lock;
};

Device& dev() {
    static Device d;
    return d;
}

std::string cl_failure(const char* what, cl_int code) {
    return std::string("OpenCL failed in ") + what + " (error " + std::to_string(code) + ")";
}

void* open_library() {
    const char* forced = std::getenv("DREAM_OPENCL_LIB");
#if defined(_WIN32)
    if (forced) return reinterpret_cast<void*>(LoadLibraryA(forced));
    return reinterpret_cast<void*>(LoadLibraryA("OpenCL.dll"));
#else
    if (forced) return dlopen(forced, RTLD_NOW | RTLD_LOCAL);
    const char* names[] = {
#if defined(__APPLE__)
        "/System/Library/Frameworks/OpenCL.framework/OpenCL",
#endif
        "libOpenCL.so.1", "libOpenCL.so",
    };
    for (const char* n : names)
        if (void* h = dlopen(n, RTLD_NOW | RTLD_LOCAL)) return h;
    return nullptr;
#endif
}

void* symbol(void* lib, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(lib), name));
#else
    return dlsym(lib, name);
#endif
}

bool load_api(Api& api, std::string* why) {
    void* lib = open_library();
    if (!lib) {
        *why = "no OpenCL library was found (install your GPU's OpenCL driver, or set "
               "DREAM_OPENCL_LIB to the library's path)";
        return false;
    }
#define DREAM_CL_LOAD(field, name)                                         \
    api.field = reinterpret_cast<decltype(api.field)>(symbol(lib, name));  \
    if (!api.field) {                                                      \
        *why = std::string("the OpenCL library has no ") + name;           \
        return false;                                                      \
    }
    DREAM_CL_LOAD(GetPlatformIDs, "clGetPlatformIDs")
    DREAM_CL_LOAD(GetDeviceIDs, "clGetDeviceIDs")
    DREAM_CL_LOAD(GetDeviceInfo, "clGetDeviceInfo")
    DREAM_CL_LOAD(CreateContext, "clCreateContext")
    DREAM_CL_LOAD(CreateCommandQueue, "clCreateCommandQueue")
    DREAM_CL_LOAD(CreateBuffer, "clCreateBuffer")
    DREAM_CL_LOAD(ReleaseMemObject, "clReleaseMemObject")
    DREAM_CL_LOAD(EnqueueWriteBuffer, "clEnqueueWriteBuffer")
    DREAM_CL_LOAD(EnqueueReadBuffer, "clEnqueueReadBuffer")
    DREAM_CL_LOAD(EnqueueCopyBuffer, "clEnqueueCopyBuffer")
    DREAM_CL_LOAD(CreateProgramWithSource, "clCreateProgramWithSource")
    DREAM_CL_LOAD(BuildProgram, "clBuildProgram")
    DREAM_CL_LOAD(GetProgramBuildInfo, "clGetProgramBuildInfo")
    DREAM_CL_LOAD(CreateKernel, "clCreateKernel")
    DREAM_CL_LOAD(SetKernelArg, "clSetKernelArg")
    DREAM_CL_LOAD(EnqueueNDRangeKernel, "clEnqueueNDRangeKernel")
    DREAM_CL_LOAD(Finish, "clFinish")
    DREAM_CL_LOAD(Flush, "clFlush")
    DREAM_CL_LOAD(EnqueueMarkerWithWaitList, "clEnqueueMarkerWithWaitList")
    DREAM_CL_LOAD(WaitForEvents, "clWaitForEvents")
    DREAM_CL_LOAD(ReleaseEvent, "clReleaseEvent")
#undef DREAM_CL_LOAD
    return true;
}

/// A GPU if there is one, and otherwise whatever device OpenCL offers. The
/// second half is what lets a CPU implementation stand in, which is how these
/// paths are tested on machines with no GPU; a real one always wins.
bool pick_device(Device& d, std::string* why) {
    cl_uint nplat = 0;
    if (d.cl.GetPlatformIDs(0, nullptr, &nplat) != CL_SUCCESS || nplat == 0) {
        *why = "OpenCL is installed but reports no platforms";
        return false;
    }
    std::vector<cl_platform_id> plats(nplat);
    d.cl.GetPlatformIDs(nplat, plats.data(), nullptr);
    for (cl_bitfield type : {CL_DEVICE_TYPE_GPU, CL_DEVICE_TYPE_ALL}) {
        for (cl_platform_id p : plats) {
            cl_device_id id = nullptr;
            cl_uint n = 0;
            if (d.cl.GetDeviceIDs(p, type, 1, &id, &n) == CL_SUCCESS && n > 0) {
                d.device = id;
                return true;
            }
        }
    }
    *why = "OpenCL reports no devices";
    return false;
}

bool build(Device& d, Program& prog, const std::string& options, std::string* why) {
    cl_int err = 0;
    const char* src = kSource;
    prog.program = d.cl.CreateProgramWithSource(d.context, 1, &src, nullptr, &err);
    if (err != CL_SUCCESS) {
        *why = cl_failure("clCreateProgramWithSource", err);
        return false;
    }
    err = d.cl.BuildProgram(prog.program, 1, &d.device, options.c_str(), nullptr, nullptr);
    if (err != CL_SUCCESS) {
        size_t len = 0;
        d.cl.GetProgramBuildInfo(prog.program, d.device, CL_PROGRAM_BUILD_LOG, 0, nullptr, &len);
        std::string log(len, '\0');
        d.cl.GetProgramBuildInfo(prog.program, d.device, CL_PROGRAM_BUILD_LOG, len, log.data(),
                                 nullptr);
        *why = "the GPU kernels did not build: " + log;
        return false;
    }
    struct Named { cl_kernel* k; const char* name; } kernels[] = {
        {&prog.binary, "binary"}, {&prog.scalar, "scalar"}, {&prog.unary, "unary"},
        {&prog.matmul, "matmul"}, {&prog.matvec, "matvec"}, {&prog.transpose, "transpose"},
        {&prog.reduce, "reduce"},
    };
    for (auto& k : kernels) {
        *k.k = d.cl.CreateKernel(prog.program, k.name, &err);
        if (err != CL_SUCCESS) {
            *why = cl_failure(k.name, err);
            return false;
        }
    }
    return true;
}

void init(Device& d) {
    std::string why;
    if (!load_api(d.cl, &why) || !pick_device(d, &why)) {
        d.why = why;
        return;
    }
    char name[256] = {0};
    d.cl.GetDeviceInfo(d.device, CL_DEVICE_NAME, sizeof name - 1, name, nullptr);
    d.name = name;
    size_t ext_len = 0;
    d.cl.GetDeviceInfo(d.device, CL_DEVICE_EXTENSIONS, 0, nullptr, &ext_len);
    std::string ext(ext_len, '\0');
    d.cl.GetDeviceInfo(d.device, CL_DEVICE_EXTENSIONS, ext_len, ext.data(), nullptr);
    d.f64 = ext.find("cl_khr_fp64") != std::string::npos;
    size_t max_group = 0;
    d.cl.GetDeviceInfo(d.device, CL_DEVICE_MAX_WORK_GROUP_SIZE, sizeof max_group, &max_group,
                       nullptr);
    // A 16 x 16 tile wants 256 work-items in a group, which every desktop GPU
    // allows; a smaller device gets 8 x 8.
    d.tile = max_group >= 256 ? 16 : 8;
    d.group = 64;
    while (d.group > max_group && d.group > 1) d.group /= 2;

    cl_int err = 0;
    d.context = d.cl.CreateContext(nullptr, 1, &d.device, nullptr, nullptr, &err);
    if (err != CL_SUCCESS) {
        d.why = cl_failure("clCreateContext", err);
        return;
    }
    d.queue = d.cl.CreateCommandQueue(d.context, d.device, 0, &err);
    if (err != CL_SUCCESS) {
        d.why = cl_failure("clCreateCommandQueue", err);
        return;
    }
    const std::string ts = " -DTS=" + std::to_string(d.tile);
    if (!build(d, d.programs[F32], "-DT=float" + ts, &why)) {
        d.why = why;
        return;
    }
    if (d.f64 && !build(d, d.programs[F64], "-DT=double -DFP64" + ts, &why)) d.f64 = false;
}

Device* ready(std::string* err) {
    static std::once_flag once;
    Device& d = dev();
    std::call_once(once, [&] { init(d); });
    if (!d.why.empty()) {
        *err = d.why;
        return nullptr;
    }
    return &d;
}

Program* program_for(Device& d, int dtype, std::string* err) {
    if (dtype == F64 && !d.f64) {
        *err = "this GPU (" + d.name + ") has no double precision; use tensor.gpu for float32";
        return nullptr;
    }
    return &d.programs[dtype];
}

size_t round_up(size_t n, size_t m) { return (n + m - 1) / m * m; }

}  // namespace

struct Buffer {
    cl_mem mem;
    size_t bytes;
    std::atomic<int> refs;
};

bool available(std::string* why) { return ready(why) != nullptr; }

std::string device_name() {
    std::string ignored;
    Device* d = ready(&ignored);
    return d ? d->name : std::string();
}

bool has_f64() {
    std::string ignored;
    Device* d = ready(&ignored);
    return d && d->f64;
}

size_t dtype_size(int dtype) { return dtype == F32 ? sizeof(float) : sizeof(double); }

Buffer* alloc(size_t bytes, std::string* err) {
    Device* d = ready(err);
    if (!d) return nullptr;
    cl_int code = 0;
    // OpenCL refuses a zero-sized buffer, and a tensor is never empty, but a
    // partial result can be: one byte is a buffer nothing reads.
    cl_mem mem = d->cl.CreateBuffer(d->context, CL_MEM_READ_WRITE, bytes ? bytes : 1, nullptr, &code);
    if (code != CL_SUCCESS) {
        *err = cl_failure("clCreateBuffer", code) + " allocating " + std::to_string(bytes) +
               " bytes of device memory";
        return nullptr;
    }
    auto* b = new Buffer{mem, bytes, {1}};
    return b;
}

void retain(Buffer* b) { b->refs.fetch_add(1, std::memory_order_relaxed); }

void release(Buffer* b) {
    if (b->refs.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    dev().cl.ReleaseMemObject(b->mem);
    delete b;
}

bool upload(Buffer* dst, int dtype, const double* src, size_t n, std::string* err) {
    Device* d = ready(err);
    if (!d) return false;
    std::lock_guard<std::mutex> g(d->lock);
    cl_int code;
    if (dtype == F32) {
        std::vector<float> tmp(src, src + n);
        code = d->cl.EnqueueWriteBuffer(d->queue, dst->mem, CL_TRUE, 0, n * sizeof(float),
                                        tmp.data(), 0, nullptr, nullptr);
    } else {
        code = d->cl.EnqueueWriteBuffer(d->queue, dst->mem, CL_TRUE, 0, n * sizeof(double), src, 0,
                                        nullptr, nullptr);
    }
    if (code != CL_SUCCESS) *err = cl_failure("clEnqueueWriteBuffer", code);
    return code == CL_SUCCESS;
}

bool download(Buffer* src, int dtype, double* dst, size_t n, std::string* err) {
    Device* d = ready(err);
    if (!d) return false;
    std::lock_guard<std::mutex> g(d->lock);
    cl_int code;
    if (dtype == F32) {
        std::vector<float> tmp(n);
        code = d->cl.EnqueueReadBuffer(d->queue, src->mem, CL_TRUE, 0, n * sizeof(float),
                                       tmp.data(), 0, nullptr, nullptr);
        for (size_t i = 0; i < n; ++i) dst[i] = tmp[i];
    } else {
        code = d->cl.EnqueueReadBuffer(d->queue, src->mem, CL_TRUE, 0, n * sizeof(double), dst, 0,
                                       nullptr, nullptr);
    }
    if (code != CL_SUCCESS) *err = cl_failure("clEnqueueReadBuffer", code);
    return code == CL_SUCCESS;
}

bool copy(Buffer* src, int dtype, size_t offset, Buffer* dst, size_t n, std::string* err) {
    Device* d = ready(err);
    if (!d) return false;
    std::lock_guard<std::mutex> g(d->lock);
    const size_t w = dtype_size(dtype);
    cl_int code = d->cl.EnqueueCopyBuffer(d->queue, src->mem, dst->mem, offset * w, 0, n * w, 0,
                                          nullptr, nullptr);
    if (code != CL_SUCCESS) *err = cl_failure("clEnqueueCopyBuffer", code);
    return code == CL_SUCCESS;
}

namespace {

/// Arguments in order, then the launch. Everything under the device's lock.
struct Launch {
    Device& d;
    cl_kernel k;
    cl_uint at = 0;
    cl_int code = CL_SUCCESS;
    template <class X>
    Launch& arg(const X& x) {
        if (code == CL_SUCCESS) code = d.cl.SetKernelArg(k, at, sizeof(X), &x);
        ++at;
        return *this;
    }
    Launch& local(size_t bytes) {
        if (code == CL_SUCCESS) code = d.cl.SetKernelArg(k, at, bytes, nullptr);
        ++at;
        return *this;
    }
    /// The element type's scalar: a float for an F32 program, a double for F64.
    Launch& scalar(int dtype, double s) {
        if (dtype == F32) return arg(float(s));
        return arg(s);
    }
    bool run(cl_uint dims, const size_t* global, const size_t* group, std::string* err) {
        if (code == CL_SUCCESS)
            code = d.cl.EnqueueNDRangeKernel(d.queue, k, dims, nullptr, global, group, 0, nullptr,
                                             nullptr);
        if (code != CL_SUCCESS) {
            *err = cl_failure("a GPU kernel launch", code);
            return false;
        }
        throttle();
        return true;
    }

    /// Keep at most two windows of launches in flight.
    ///
    /// A released buffer is freed only once every queued command that reads it
    /// has run, and the host enqueues far faster than any device computes: a
    /// loop of a thousand products queued a thousand results' worth of memory
    /// before the first was freed, and measured eight gigabytes for a program
    /// whose live set was eight megabytes. So every `kWindow` launches drop a
    /// marker, and wait for the marker before it. The device is never idle on
    /// our account -- a whole window is always queued behind the one waited
    /// for -- and what is in flight, with the memory it pins, is bounded.
    void throttle() {
        constexpr unsigned kWindow = 32;
        if (++d.launches % kWindow != 0) return;
        cl_event marker = nullptr;
        if (d.cl.EnqueueMarkerWithWaitList(d.queue, 0, nullptr, &marker) != CL_SUCCESS) return;
        d.cl.Flush(d.queue);
        if (d.previous_marker) {
            d.cl.WaitForEvents(1, &d.previous_marker);
            d.cl.ReleaseEvent(d.previous_marker);
        }
        d.previous_marker = marker;
    }
};

}  // namespace

bool binary(int op, int dtype, Buffer* x, size_t nx, Buffer* y, size_t ny, Buffer* out, size_t n,
            std::string* err) {
    Device* d = ready(err);
    if (!d) return false;
    Program* p = program_for(*d, dtype, err);
    if (!p) return false;
    std::lock_guard<std::mutex> g(d->lock);
    size_t global = round_up(n, 64);
    return Launch{*d, p->binary}
        .arg(cl_int(op)).arg(x->mem).arg(cl_ulong(nx)).arg(y->mem).arg(cl_ulong(ny))
        .arg(out->mem).arg(cl_ulong(n))
        .run(1, &global, nullptr, err);
}

bool scalar(int op, int dtype, Buffer* x, double s, bool scalar_left, Buffer* out, size_t n,
            std::string* err) {
    Device* d = ready(err);
    if (!d) return false;
    Program* p = program_for(*d, dtype, err);
    if (!p) return false;
    std::lock_guard<std::mutex> g(d->lock);
    size_t global = round_up(n, 64);
    return Launch{*d, p->scalar}
        .arg(cl_int(op)).arg(x->mem).scalar(dtype, s).arg(cl_int(scalar_left ? 1 : 0))
        .arg(out->mem).arg(cl_ulong(n))
        .run(1, &global, nullptr, err);
}

bool unary(int fn, int dtype, Buffer* x, Buffer* out, size_t n, std::string* err) {
    Device* d = ready(err);
    if (!d) return false;
    Program* p = program_for(*d, dtype, err);
    if (!p) return false;
    std::lock_guard<std::mutex> g(d->lock);
    size_t global = round_up(n, 64);
    return Launch{*d, p->unary}
        .arg(cl_int(fn)).arg(x->mem).arg(out->mem).arg(cl_ulong(n))
        .run(1, &global, nullptr, err);
}

bool matmul(int dtype, Buffer* a, Buffer* b, Buffer* c, size_t M, size_t K, size_t N,
            std::string* err) {
    Device* d = ready(err);
    if (!d) return false;
    Program* p = program_for(*d, dtype, err);
    if (!p) return false;
    std::lock_guard<std::mutex> g(d->lock);
    if (N == 1) {
        size_t global = round_up(M, 64);
        return Launch{*d, p->matvec}
            .arg(a->mem).arg(b->mem).arg(c->mem).arg(cl_int(M)).arg(cl_int(K))
            .run(1, &global, nullptr, err);
    }
    size_t global[2] = {round_up(N, d->tile), round_up(M, d->tile)};
    size_t group[2] = {d->tile, d->tile};
    return Launch{*d, p->matmul}
        .arg(a->mem).arg(b->mem).arg(c->mem).arg(cl_int(M)).arg(cl_int(K)).arg(cl_int(N))
        .run(2, global, group, err);
}

bool transpose(int dtype, Buffer* in, Buffer* out, size_t rows, size_t cols, std::string* err) {
    Device* d = ready(err);
    if (!d) return false;
    Program* p = program_for(*d, dtype, err);
    if (!p) return false;
    std::lock_guard<std::mutex> g(d->lock);
    size_t global[2] = {round_up(cols, 16), round_up(rows, 16)};
    return Launch{*d, p->transpose}
        .arg(in->mem).arg(out->mem).arg(cl_int(rows)).arg(cl_int(cols))
        .run(2, global, nullptr, err);
}

bool reduce(int op, int dtype, Buffer* x, size_t n, double* out, std::string* err) {
    Device* d = ready(err);
    if (!d) return false;
    Program* p = program_for(*d, dtype, err);
    if (!p) return false;
    // Enough groups to fill a GPU, few enough that the host's share is nothing.
    const size_t groups = std::min<size_t>(256, (n + d->group - 1) / d->group);
    const size_t w = dtype_size(dtype);
    Buffer* partial = alloc(groups * w, err);
    if (!partial) return false;
    bool ok;
    {
        std::lock_guard<std::mutex> g(d->lock);
        size_t global = groups * d->group, group = d->group;
        ok = Launch{*d, p->reduce}
                 .arg(cl_int(op)).arg(x->mem).arg(cl_ulong(n)).arg(partial->mem)
                 .local(group * w)
                 .run(1, &global, &group, err);
    }
    std::vector<double> parts(groups);
    if (ok) ok = download(partial, dtype, parts.data(), groups, err);
    release(partial);
    if (!ok) return false;
    double r = parts[0];
    for (size_t i = 1; i < groups; ++i) {
        switch (op) {
            case KRED_SUM: r += parts[i]; break;
            case KRED_MIN: r = parts[i] < r ? parts[i] : r; break;
            default: r = parts[i] > r ? parts[i] : r; break;
        }
    }
    *out = r;
    return true;
}

}  // namespace dream::gpu
