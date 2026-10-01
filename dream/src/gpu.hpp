// The GPU behind `tensor.gpu`: device memory and the kernels that run on it.
//
// One device per runtime, found on first use and kept for the life of the VM.
// It is reached through OpenCL, loaded with `dlopen` rather than linked: the VM
// gains no build dependency, a machine with no OpenCL runs every program that
// does not ask for a GPU, and one that asks gets an error naming what is
// missing. OpenCL because it is the one API every vendor's GPU answers --
// NVIDIA, AMD, Intel and Apple's -- and because a CPU implementation of it
// (POCL) exists, which is how the tests run this code on a machine without a
// GPU. A CUDA or Metal backend would sit behind the same functions.
//
// Everything is asynchronous except `download`. Operations are enqueued in
// order on one queue and return at once, so a chain of products and sums runs
// on the device back to back without the host waiting on any of them; only
// reading an answer back stops to wait. That is the whole of the speed model,
// and docs/notes/tensors.md has the rest.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace dream::gpu {

/// A block of device memory, reference counted. A GPU tensor holds one
/// reference; a reshape shares the buffer and takes another. The last release
/// frees the memory, which OpenCL defers until every queued command reading it
/// has run.
struct Buffer;

/// Element types a device buffer can hold. A float on a GPU is the fast case
/// -- most consumer GPUs run doubles at a thirty-second of the rate, or not at
/// all -- so `tensor.gpu` makes `F32` and `tensor.gpu64` asks for `F64`.
enum Dtype : int { F64 = 0, F32 = 1 };

/// Whether a device is usable, finding one on the first call. When it is not,
/// `why` says why: no OpenCL library, no device, a kernel that would not build.
bool available(std::string* why);
/// The device's name, or "" when there is none.
std::string device_name();
/// Whether the device can hold `F64` buffers.
bool has_f64();

size_t dtype_size(int dtype);

Buffer* alloc(size_t bytes, std::string* err);
void retain(Buffer* b);
void release(Buffer* b);

/// Host doubles into a device buffer of `dtype`, converting on the way.
bool upload(Buffer* dst, int dtype, const double* src, size_t n, std::string* err);
/// A device buffer of `dtype` back into host doubles. Waits for every queued
/// operation that writes it.
bool download(Buffer* src, int dtype, double* dst, size_t n, std::string* err);
/// `n` elements from `src` at element `offset` into the start of `dst`.
bool copy(Buffer* src, int dtype, size_t offset, Buffer* dst, size_t n, std::string* err);

/// `out[i] = x[i % nx] op y[i % ny]`: equal shapes, and every broadcast a
/// tensor allows (a trailing shape repeated), in one kernel.
bool binary(int op, int dtype, Buffer* x, size_t nx, Buffer* y, size_t ny, Buffer* out, size_t n,
            std::string* err);
bool scalar(int op, int dtype, Buffer* x, double s, bool scalar_left, Buffer* out, size_t n,
            std::string* err);
bool unary(int fn, int dtype, Buffer* x, Buffer* out, size_t n, std::string* err);
/// `C = A x B`, row-major, M x K by K x N.
bool matmul(int dtype, Buffer* a, Buffer* b, Buffer* c, size_t M, size_t K, size_t N,
            std::string* err);
bool transpose(int dtype, Buffer* in, Buffer* out, size_t rows, size_t cols, std::string* err);
/// Sum, minimum or maximum (`KRED_*`) of `n >= 1` elements, finished on the
/// host in double precision.
bool reduce(int op, int dtype, Buffer* x, size_t n, double* out, std::string* err);

}  // namespace dream::gpu
