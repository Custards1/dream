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
#include <unordered_map>
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
// The fixed kernels -- the products and transpose -- are one source, built
// once per element type with `T` defined to it. Elementwise work and
// reductions are not here: each fused program gets kernels generated for it
// (`fused_source`), whose operation codes are tensor_kernels.hpp's, so the
// host and the device agree on what `op == 3` means. Every constant is written
// `(T)n`: a bare `1.0` is a double, and a device without doubles refuses the
// whole program over it.

const char* const kSource = R"CL(
#ifdef FP64
#pragma OPENCL EXTENSION cl_khr_fp64 : enable
#endif

// The textbook tiled product: each work-group computes a TS x TS tile of C,
// stepping along K a tile at a time through local memory, so every element of
// A and B is read from global memory once per tile rather than once per
// product term.
//
// Each operand is read through strides, `A[i * ars + k * acs]`: a matrix as it
// is stored, or one read as its transpose with nothing copied.
__kernel void matmul(__global const T* A, ulong ars, ulong acs, __global const T* B, ulong brs,
                     ulong bcs, __global T* C, int M, int K, int N) {
    __local T As[TS][TS];
    __local T Bs[TS][TS];
    int col = get_global_id(0), row = get_global_id(1);
    int lc = get_local_id(0), lr = get_local_id(1);
    T acc = (T)0;
    for (int t = 0; t < K; t += TS) {
        As[lr][lc] = (row < M && t + lc < K) ? A[row * ars + (t + lc) * acs] : (T)0;
        Bs[lr][lc] = (t + lr < K && col < N) ? B[(t + lr) * brs + col * bcs] : (T)0;
        barrier(CLK_LOCAL_MEM_FENCE);
        for (int k = 0; k < TS; ++k) acc += As[lr][k] * Bs[k][lc];
        barrier(CLK_LOCAL_MEM_FENCE);
    }
    if (row < M && col < N) C[row * N + col] = acc;
}

// A product with one column: one work-item per row, a dot product each. The
// tiled kernel would run sixteen work-items per row and throw fifteen away.
__kernel void matvec(__global const T* A, ulong ars, ulong acs, __global const T* x, ulong xrs,
                     __global T* y, int M, int K) {
    int i = get_global_id(0);
    if (i >= M) return;
    T acc = (T)0;
    for (int k = 0; k < K; ++k) acc += A[i * ars + k * acs] * x[k * xrs];
    y[i] = acc;
}

__kernel void transpose(__global const T* in, __global T* out, int R, int C) {
    int j = get_global_id(0), i = get_global_id(1);
    if (i < R && j < C) out[(ulong)j * R + i] = in[(ulong)i * C + j];
}

)CL";

// What every generated elementwise kernel starts with: the operations a
// program's instructions name, as functions, so that a nested `relu` is a call
// and not its operand written out twice at every level.
const char* const kFusedPrelude = R"CL(
#ifdef FP64
#pragma OPENCL EXTENSION cl_khr_fp64 : enable
#endif
inline T relu_(T a) { return a > (T)0 ? a : (T)0; }
inline T sigmoid_(T a) { return (T)1 / ((T)1 + exp(-a)); }
inline T combine_(int op, T a, T b) {
    return op == 0 ? a + b : (op == 1 ? fmin(a, b) : fmax(a, b));
}
)CL";

/// The fixed kernels, built for one element type.
struct Program {
    cl_program program = nullptr;
    cl_kernel matmul = nullptr, matvec = nullptr, transpose = nullptr;
};

/// The two kernels generated for one fused program: the elementwise one, and
/// the reduction that computes the same values and folds them instead of
/// storing them.
///
/// A program that starts from a product gets the product kernels instead,
/// with the program applied to each element as it is stored: `mm` for a
/// product with more than one column, `mv` for one with exactly one.
struct Fused {
    cl_program program = nullptr;
    cl_kernel map = nullptr, reduce = nullptr, axis = nullptr, mm = nullptr, mv = nullptr;
};

struct Device {
    Api cl{};
    cl_device_id device = nullptr;
    cl_context context = nullptr;
    cl_command_queue queue = nullptr;
    Program programs[2];  // by Dtype
    /// Fused kernels by their source, which names the dtype. A program is
    /// built the first time it is met, and a loop meets the same one every
    /// iteration -- its constants are arguments, not part of the source.
    std::unordered_map<std::string, Fused> fused;
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

/// Compile `source` with `options`, saying what went wrong in `why`.
cl_program compile(Device& d, const std::string& source, const std::string& options,
                   std::string* why) {
    cl_int err = 0;
    const char* src = source.c_str();
    cl_program program = d.cl.CreateProgramWithSource(d.context, 1, &src, nullptr, &err);
    if (err != CL_SUCCESS) {
        *why = cl_failure("clCreateProgramWithSource", err);
        return nullptr;
    }
    err = d.cl.BuildProgram(program, 1, &d.device, options.c_str(), nullptr, nullptr);
    if (err != CL_SUCCESS) {
        size_t len = 0;
        d.cl.GetProgramBuildInfo(program, d.device, CL_PROGRAM_BUILD_LOG, 0, nullptr, &len);
        std::string log(len, '\0');
        d.cl.GetProgramBuildInfo(program, d.device, CL_PROGRAM_BUILD_LOG, len, log.data(),
                                 nullptr);
        *why = "the GPU kernels did not build: " + log;
        return nullptr;
    }
    return program;
}

bool kernel(Device& d, cl_program program, const char* name, cl_kernel* out, std::string* why) {
    cl_int err = 0;
    *out = d.cl.CreateKernel(program, name, &err);
    if (err != CL_SUCCESS) *why = cl_failure(name, err);
    return err == CL_SUCCESS;
}

std::string options_for(const Device& d, int dtype) {
    return std::string(dtype == F32 ? "-DT=float" : "-DT=double -DFP64") +
           " -DTS=" + std::to_string(d.tile);
}

bool build(Device& d, Program& prog, int dtype, std::string* why) {
    prog.program = compile(d, kSource, options_for(d, dtype), why);
    return prog.program && kernel(d, prog.program, "matmul", &prog.matmul, why) &&
           kernel(d, prog.program, "matvec", &prog.matvec, why) &&
           kernel(d, prog.program, "transpose", &prog.transpose, why);
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
    if (!build(d, d.programs[F32], F32, &why)) {
        d.why = why;
        return;
    }
    if (d.f64 && !build(d, d.programs[F64], F64, &why)) d.f64 = false;
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

bool matmul(int dtype, Operand a, Operand b, Buffer* c, size_t M, size_t K, size_t N,
            std::string* err) {
    Device* d = ready(err);
    if (!d) return false;
    Program* p = program_for(*d, dtype, err);
    if (!p) return false;
    std::lock_guard<std::mutex> g(d->lock);
    if (N == 1) {
        size_t global = round_up(M, 64);
        return Launch{*d, p->matvec}
            .arg(a.buf->mem).arg(cl_ulong(a.rs)).arg(cl_ulong(a.cs))
            .arg(b.buf->mem).arg(cl_ulong(b.rs))
            .arg(c->mem).arg(cl_int(M)).arg(cl_int(K))
            .run(1, &global, nullptr, err);
    }
    size_t global[2] = {round_up(N, d->tile), round_up(M, d->tile)};
    size_t group[2] = {d->tile, d->tile};
    return Launch{*d, p->matmul}
        .arg(a.buf->mem).arg(cl_ulong(a.rs)).arg(cl_ulong(a.cs))
        .arg(b.buf->mem).arg(cl_ulong(b.rs)).arg(cl_ulong(b.cs))
        .arg(c->mem).arg(cl_int(M)).arg(cl_int(K)).arg(cl_int(N))
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

namespace {

/// One value function of a generated kernel: its source, the parameters it
/// adds to a kernel's list, and the arguments that pass them on.
struct ValueFn {
    std::string text, params, args;
};

/// `name(i, n, ..)`: the value of `prog` at position `i` of an answer of `n`,
/// with its inputs and constants named with `pre` so that several programs
/// can share one kernel's parameter list. A program that starts from a product
/// takes the product's element as `p`.
ValueFn value_function(const FuseProgram& prog, const std::string& name, const std::string& pre,
                       bool product) {
    ValueFn f;
    for (unsigned k = 0; k < prog.ninputs; ++k) {
        const std::string i = pre + std::to_string(k);
        f.params += ", __global const T* in" + i + ", ulong n" + i + ", ulong r" + i;
        f.args += ", in" + i + ", n" + i + ", r" + i;
    }
    for (unsigned k = 0; k < prog.nconsts; ++k) {
        f.params += ", T c" + pre + std::to_string(k);
        f.args += ", c" + pre + std::to_string(k);
    }
    // Each input is loaded the way the program reads it -- as it lies, as
    // its transpose, or both -- once, at the top.
    std::string loads;
    std::vector<bool> plain(prog.ninputs, false), turned(prog.ninputs, false);
    for (unsigned i = 0; i < prog.ncode; ++i) {
        if (prog.code[2 * i] == FUSE_LOAD) plain[prog.code[2 * i + 1]] = true;
        if (prog.code[2 * i] == FUSE_LOADT) turned[prog.code[2 * i + 1]] = true;
    }
    for (unsigned k = 0; k < prog.ninputs; ++k) {
        const std::string i = std::to_string(k), q = pre + i;
        if (plain[k])
            loads += "    T x" + i + " = in" + q + "[n" + q + " == n ? i : i % n" + q + "];\n";
        if (turned[k])
            loads += "    ulong o" + i + " = i % n" + q + ";\n"
                     "    T xt" + i + " = in" + q + "[(o" + i + " % r" + q + ") * (n" + q + " / r" + q +
                     ") + o" + i + " / r" + q + "];\n";
    }
    std::vector<std::string> stack;
    static const char* const binops[] = {" + ", " - ", " * ", " / "};
    static const char* const fns[] = {"-", "sqrt", "exp", "log", "fabs", "tanh",
                                      "sin", "cos", "relu_", "sigmoid_"};
    for (unsigned i = 0; i < prog.ncode; ++i) {
        const unsigned op = prog.code[2 * i], arg = prog.code[2 * i + 1];
        switch (op) {
            case FUSE_LOAD: stack.push_back("x" + std::to_string(arg)); break;
            case FUSE_CONST: stack.push_back("c" + pre + std::to_string(arg)); break;
            case FUSE_PRODUCT: stack.push_back("p"); break;
            case FUSE_LOADT: stack.push_back("xt" + std::to_string(arg)); break;
            case FUSE_BIN: {
                std::string y = stack.back();
                stack.pop_back();
                std::string& x = stack.back();
                x = arg == KOP_MOD ? "fmod(" + x + ", " + y + ")" : "(" + x + binops[arg] + y + ")";
                break;
            }
            default: stack.back() = std::string(fns[arg]) + "(" + stack.back() + ")"; break;
        }
    }
    f.text = "inline T " + name + "(ulong i, ulong n" + (product ? ", T p" : "") + f.params +
             ") {\n" + loads + "    return " + stack.back() + ";\n}\n";
    return f;
}

/// The OpenCL source for a fused program: a `value` function computing one
/// element from the inputs and constants, and the kernels around it. The
/// constants are arguments, so that `x * 0.5` and `x * 0.25` are one kernel
/// and a loop that changes a scalar every iteration builds nothing new.
///
/// A program that starts from a product gets the product kernels instead,
/// and an operand of that product that is itself a deferred program (`pa`,
/// `pb`) is computed by its own value function as the product loads it --
/// into the tile of local memory the kernel was going to copy it into, so a
/// chain feeding the product is never stored.
std::string fused_source(int dtype, const FuseProgram& prog, const FuseProgram* pa,
                         const FuseProgram* pb) {
    bool product = false;
    for (unsigned i = 0; i < prog.ncode; ++i) product = product || prog.code[2 * i] == FUSE_PRODUCT;
    const ValueFn v = value_function(prog, "value", "", product);
    std::string src = std::string("// ") + (dtype == F32 ? "f32" : "f64") + "\n" + kFusedPrelude +
                      v.text;
    if (product) {
        std::string params = v.params, args = v.args;
        std::string load_a = "A[row * ars + (t + lc) * acs]";
        std::string load_b = "B[(t + lr) * brs + col * bcs]";
        std::string load_ma = "A[i * ars + k * acs]", load_x = "x[k * xrs]";
        if (pa) {
            const ValueFn f = value_function(*pa, "va", "a", false);
            src += f.text;
            params += f.params;
            args += f.args;
            load_a = "va((ulong)row * K + (t + lc), (ulong)M * K" + f.args + ")";
            load_ma = "va((ulong)i * K + k, (ulong)M * K" + f.args + ")";
        }
        if (pb) {
            const ValueFn f = value_function(*pb, "vb", "b", false);
            src += f.text;
            params += f.params;
            args += f.args;
            load_b = "vb((ulong)(t + lr) * N + col, (ulong)K * N" + f.args + ")";
            load_x = "vb((ulong)k, (ulong)K" + f.args + ")";
        }
        // The kernels are `kSource`'s, with the loads of a computed operand
        // replaced by its value function and the store replaced by the
        // program applied to what would have been stored.
        src += "__kernel void mm(__global const T* A, ulong ars, ulong acs, __global const T* B,\n"
               "                 ulong brs, ulong bcs, __global T* C,\n"
               "                 int M, int K, int N, ulong n" + params + ") {\n"
               "    __local T As[TS][TS];\n"
               "    __local T Bs[TS][TS];\n"
               "    int col = get_global_id(0), row = get_global_id(1);\n"
               "    int lc = get_local_id(0), lr = get_local_id(1);\n"
               "    T acc = (T)0;\n"
               "    for (int t = 0; t < K; t += TS) {\n"
               "        As[lr][lc] = (row < M && t + lc < K) ? " + load_a + " : (T)0;\n"
               "        Bs[lr][lc] = (t + lr < K && col < N) ? " + load_b + " : (T)0;\n"
               "        barrier(CLK_LOCAL_MEM_FENCE);\n"
               "        for (int k = 0; k < TS; ++k) acc += As[lr][k] * Bs[k][lc];\n"
               "        barrier(CLK_LOCAL_MEM_FENCE);\n"
               "    }\n"
               "    if (row < M && col < N) {\n"
               "        ulong i = (ulong)row * N + col;\n"
               "        C[i] = value(i, n, acc" + v.args + ");\n"
               "    }\n}\n";
        src += "__kernel void mv(__global const T* A, ulong ars, ulong acs, __global const T* x,\n"
               "                 ulong xrs, __global T* y, int M, int K, ulong n" + params + ") {\n"
               "    int i = get_global_id(0);\n"
               "    if (i >= M) return;\n"
               "    T acc = (T)0;\n"
               "    for (int k = 0; k < K; ++k) acc += " + load_ma + " * " + load_x + ";\n"
               "    y[i] = value((ulong)i, n, acc" + v.args + ");\n}\n";
        return src;
    }
    const std::string& params = v.params;
    const std::string& args = v.args;
    src += "__kernel void map(__global T* out, ulong n" + params + ") {\n"
           "    ulong i = get_global_id(0);\n"
           "    if (i < n) out[i] = value(i, n" + args + ");\n}\n";
    src += "__kernel void reduce(__global T* partial, __local T* scratch, int op, ulong n" +
           params + ") {\n"
           "    ulong gid = get_global_id(0), gsz = get_global_size(0);\n"
           "    uint lid = get_local_id(0), lsz = get_local_size(0);\n"
           "    T acc = op == 0 ? (T)0 : value(0, n" + args + ");\n"
           "    for (ulong i = gid; i < n; i += gsz) acc = combine_(op, acc, value(i, n" + args + "));\n"
           "    scratch[lid] = acc;\n"
           "    barrier(CLK_LOCAL_MEM_FENCE);\n"
           "    for (uint s = lsz / 2; s > 0; s >>= 1) {\n"
           "        if (lid < s) scratch[lid] = combine_(op, scratch[lid], scratch[lid + s]);\n"
           "        barrier(CLK_LOCAL_MEM_FENCE);\n"
           "    }\n"
           "    if (lid == 0) partial[get_group_id(0)] = scratch[0];\n}\n";
    // A sum along an axis: one work-item per answer, adding the values along
    // the axis in order.
    src += "__kernel void axis(__global T* out, ulong outer, ulong len, ulong inner, ulong n" +
           params + ") {\n"
           "    ulong g = get_global_id(0);\n"
           "    if (g >= outer * inner) return;\n"
           "    ulong o = g / inner, i = g % inner;\n"
           "    T acc = (T)0;\n"
           "    for (ulong a = 0; a < len; ++a) acc += value((o * len + a) * inner + i, n" + args + ");\n"
           "    out[g] = acc;\n}\n";
    return src;
}

/// The kernels for a program, built the first time it is asked for. Called
/// under the device's lock, which is what guards the cache.
Fused* fused_kernels(Device& d, int dtype, const FuseProgram& prog, std::string* why,
                     const FuseProgram* pa = nullptr, const FuseProgram* pb = nullptr) {
    std::string src = fused_source(dtype, prog, pa, pb);
    auto it = d.fused.find(src);
    if (it != d.fused.end()) return &it->second;
    Fused f;
    f.program = compile(d, src, options_for(d, dtype), why);
    if (!f.program) return nullptr;
    const bool product = src.find("__kernel void mm(") != std::string::npos;
    if (product ? !kernel(d, f.program, "mm", &f.mm, why) || !kernel(d, f.program, "mv", &f.mv, why)
                : !kernel(d, f.program, "map", &f.map, why) ||
                      !kernel(d, f.program, "reduce", &f.reduce, why) ||
                      !kernel(d, f.program, "axis", &f.axis, why))
        return nullptr;
    return &d.fused.emplace(std::move(src), f).first->second;
}

/// The inputs and constants every fused kernel takes after its own arguments.
void bind_program(Launch& l, int dtype, const FuseProgram& prog, Buffer* const* ins,
                  const size_t* counts, const size_t* rows, const double* consts) {
    for (unsigned k = 0; k < prog.ninputs; ++k)
        l.arg(ins[k]->mem).arg(cl_ulong(counts[k])).arg(cl_ulong(rows[k]));
    for (unsigned k = 0; k < prog.nconsts; ++k) l.scalar(dtype, consts[k]);
}

}  // namespace

bool fused(int dtype, const FuseProgram& prog, Buffer* const* ins, const size_t* counts,
           const size_t* rows, const double* consts, Buffer* out, size_t n, std::string* err) {
    Device* d = ready(err);
    if (!d) return false;
    if (!program_for(*d, dtype, err)) return false;
    std::lock_guard<std::mutex> g(d->lock);
    Fused* f = fused_kernels(*d, dtype, prog, err);
    if (!f) return false;
    size_t global = round_up(n, 64);
    Launch l{*d, f->map};
    l.arg(out->mem).arg(cl_ulong(n));
    bind_program(l, dtype, prog, ins, counts, rows, consts);
    return l.run(1, &global, nullptr, err);
}

bool fused_matmul(int dtype, const FuseProgram& prog, Operand a, Operand b, size_t M, size_t K,
                  size_t N, Buffer* const* ins, const size_t* counts, const size_t* rows,
                  const double* consts, Buffer* out, std::string* err) {
    Device* d = ready(err);
    if (!d) return false;
    if (!program_for(*d, dtype, err)) return false;
    std::lock_guard<std::mutex> g(d->lock);
    Fused* f = fused_kernels(*d, dtype, prog, err, a.program ? &a.program->prog : nullptr,
                             b.program ? &b.program->prog : nullptr);
    if (!f) return false;
    const cl_ulong n = cl_ulong(M) * N;
    // An operand computed by its own program has no buffer; the kernel's
    // parameter for one is still there and never read, so any buffer fills it.
    cl_mem amem = a.buf ? a.buf->mem : out->mem;
    cl_mem bmem = b.buf ? b.buf->mem : out->mem;
    auto bind_all = [&](Launch& l) {
        bind_program(l, dtype, prog, ins, counts, rows, consts);
        for (const OperandProgram* op : {a.program, b.program})
            if (op) bind_program(l, dtype, op->prog, op->ins, op->counts, op->rows, op->consts);
    };
    if (N == 1) {
        size_t global = round_up(M, 64);
        Launch l{*d, f->mv};
        l.arg(amem).arg(cl_ulong(a.rs)).arg(cl_ulong(a.cs)).arg(bmem).arg(cl_ulong(b.rs))
            .arg(out->mem).arg(cl_int(M)).arg(cl_int(K)).arg(n);
        bind_all(l);
        return l.run(1, &global, nullptr, err);
    }
    size_t global[2] = {round_up(N, d->tile), round_up(M, d->tile)};
    size_t group[2] = {d->tile, d->tile};
    Launch l{*d, f->mm};
    l.arg(amem).arg(cl_ulong(a.rs)).arg(cl_ulong(a.cs)).arg(bmem).arg(cl_ulong(b.rs))
        .arg(cl_ulong(b.cs)).arg(out->mem).arg(cl_int(M)).arg(cl_int(K)).arg(cl_int(N)).arg(n);
    bind_all(l);
    return l.run(2, global, group, err);
}

bool fused_axis(int dtype, const FuseProgram& prog, Buffer* const* ins, const size_t* counts,
                const size_t* rows, const double* consts, size_t outer, size_t len, size_t inner,
                Buffer* out, std::string* err) {
    Device* d = ready(err);
    if (!d) return false;
    if (!program_for(*d, dtype, err)) return false;
    std::lock_guard<std::mutex> g(d->lock);
    Fused* f = fused_kernels(*d, dtype, prog, err);
    if (!f) return false;
    size_t global = round_up(outer * inner, 64);
    Launch l{*d, f->axis};
    l.arg(out->mem).arg(cl_ulong(outer)).arg(cl_ulong(len)).arg(cl_ulong(inner))
        .arg(cl_ulong(outer * len * inner));
    bind_program(l, dtype, prog, ins, counts, rows, consts);
    return l.run(1, &global, nullptr, err);
}

bool fused_reduce(int op, int dtype, const FuseProgram& prog, Buffer* const* ins,
                  const size_t* counts, const size_t* rows, const double* consts, size_t n,
                  double* out, std::string* err) {
    Device* d = ready(err);
    if (!d) return false;
    if (!program_for(*d, dtype, err)) return false;
    // Enough groups to fill a GPU, few enough that the host's share is nothing.
    const size_t groups = std::min<size_t>(256, (n + d->group - 1) / d->group);
    const size_t w = dtype_size(dtype);
    Buffer* partial = alloc(groups * w, err);
    if (!partial) return false;
    bool ok;
    {
        std::lock_guard<std::mutex> g(d->lock);
        Fused* f = fused_kernels(*d, dtype, prog, err);
        ok = f != nullptr;
        if (ok) {
            size_t global = groups * d->group, group = d->group;
            Launch l{*d, f->reduce};
            l.arg(partial->mem).local(group * w).arg(cl_int(op)).arg(cl_ulong(n));
            bind_program(l, dtype, prog, ins, counts, rows, consts);
            ok = l.run(1, &global, &group, err);
        }
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
