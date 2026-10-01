// The CPU kernels behind tensors, written once and compiled twice.
//
// This file is included by tensor_kernels.cpp, which compiles it for the
// baseline the rest of the VM targets, and by tensor_kernels_avx2.cpp, which
// compiles it again with AVX2 and FMA switched on. `tensor_kernels()` in
// tensor.cpp asks the CPU once which of the two it can run and hands out that
// table. Each copy lives in its own namespace, named by `DREAM_KERNEL_NS`.
//
// Why two translation units rather than `target_clones` on each function: the
// matrix product's inner kernel only reaches its speed when it is inlined into
// the loops around it, and GCC will not inline a function compiled for one
// instruction set into a clone compiled for another. The clone it produced
// spilled its accumulators to memory and ran at 1 GFLOP/s where the same
// source built for the machine ran at 33 -- see "The matrix product" in
// docs/notes/tensors.md. Compiling the whole file per target sidesteps the
// question: everything in it agrees about the instruction set.
//
// Nothing here knows about Dream values. Everything is a pointer, a count and
// an operation code; tensor.cpp has already checked shapes and sizes.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

#include "tensor_kernels.hpp"

namespace dream {
namespace DREAM_KERNEL_NS {
namespace {

using Z = size_t;

inline double apply_binary(int op, double a, double b) {
    switch (op) {
        case KOP_ADD: return a + b;
        case KOP_SUB: return a - b;
        case KOP_MUL: return a * b;
        case KOP_DIV: return a / b;
        default: return std::fmod(a, b);
    }
}

// Each operation is its own loop, with the switch outside it, so that every
// loop is a plain `out[i] = x[i] op y[i]` the vectorizer can take.
void binary(int op, const double* __restrict x, const double* __restrict y,
            double* __restrict out, Z n) {
    switch (op) {
        case KOP_ADD: for (Z i = 0; i < n; ++i) out[i] = x[i] + y[i]; break;
        case KOP_SUB: for (Z i = 0; i < n; ++i) out[i] = x[i] - y[i]; break;
        case KOP_MUL: for (Z i = 0; i < n; ++i) out[i] = x[i] * y[i]; break;
        case KOP_DIV: for (Z i = 0; i < n; ++i) out[i] = x[i] / y[i]; break;
        default: for (Z i = 0; i < n; ++i) out[i] = std::fmod(x[i], y[i]); break;
    }
}

void scalar(int op, const double* __restrict x, double s, bool scalar_left,
            double* __restrict out, Z n) {
    if (scalar_left) {
        switch (op) {
            case KOP_ADD: for (Z i = 0; i < n; ++i) out[i] = s + x[i]; break;
            case KOP_SUB: for (Z i = 0; i < n; ++i) out[i] = s - x[i]; break;
            case KOP_MUL: for (Z i = 0; i < n; ++i) out[i] = s * x[i]; break;
            case KOP_DIV: for (Z i = 0; i < n; ++i) out[i] = s / x[i]; break;
            default: for (Z i = 0; i < n; ++i) out[i] = std::fmod(s, x[i]); break;
        }
        return;
    }
    switch (op) {
        case KOP_ADD: for (Z i = 0; i < n; ++i) out[i] = x[i] + s; break;
        case KOP_SUB: for (Z i = 0; i < n; ++i) out[i] = x[i] - s; break;
        case KOP_MUL: for (Z i = 0; i < n; ++i) out[i] = x[i] * s; break;
        case KOP_DIV: for (Z i = 0; i < n; ++i) out[i] = x[i] / s; break;
        default: for (Z i = 0; i < n; ++i) out[i] = std::fmod(x[i], s); break;
    }
}

void unary(int fn, const double* __restrict x, double* __restrict out, Z n) {
    switch (fn) {
        case KFN_NEG: for (Z i = 0; i < n; ++i) out[i] = -x[i]; break;
        case KFN_SQRT: for (Z i = 0; i < n; ++i) out[i] = std::sqrt(x[i]); break;
        case KFN_EXP: for (Z i = 0; i < n; ++i) out[i] = std::exp(x[i]); break;
        case KFN_LOG: for (Z i = 0; i < n; ++i) out[i] = std::log(x[i]); break;
        case KFN_ABS: for (Z i = 0; i < n; ++i) out[i] = std::fabs(x[i]); break;
        case KFN_TANH: for (Z i = 0; i < n; ++i) out[i] = std::tanh(x[i]); break;
        case KFN_SIN: for (Z i = 0; i < n; ++i) out[i] = std::sin(x[i]); break;
        case KFN_COS: for (Z i = 0; i < n; ++i) out[i] = std::cos(x[i]); break;
        case KFN_RELU: for (Z i = 0; i < n; ++i) out[i] = x[i] > 0.0 ? x[i] : 0.0; break;
        default: for (Z i = 0; i < n; ++i) out[i] = 1.0 / (1.0 + std::exp(-x[i])); break;
    }
}

// Eight running sums rather than one. A single accumulator is a chain of
// dependent additions, one every four cycles, and the compiler may not split
// it -- floating-point addition is not associative, so splitting changes the
// answer. Writing the split out is what licenses the vector unit, at the price
// the comment just named: the rounding is that of eight interleaved sums, not
// of one left-to-right sum. Every BLAS makes the same trade.
double dot(const double* __restrict a, const double* __restrict b, Z n) {
    double acc[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    Z i = 0;
    for (; i + 8 <= n; i += 8)
        for (int l = 0; l < 8; ++l) acc[l] += a[i + l] * b[i + l];
    double tail = 0;
    for (; i < n; ++i) tail += a[i] * b[i];
    return ((acc[0] + acc[1]) + (acc[2] + acc[3])) + ((acc[4] + acc[5]) + (acc[6] + acc[7])) +
           tail;
}

double reduce(int op, const double* __restrict x, Z n) {
    if (op == KRED_SUM) {
        double acc[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        Z i = 0;
        for (; i + 8 <= n; i += 8)
            for (int l = 0; l < 8; ++l) acc[l] += x[i + l];
        double tail = 0;
        for (; i < n; ++i) tail += x[i];
        return ((acc[0] + acc[1]) + (acc[2] + acc[3])) +
               ((acc[4] + acc[5]) + (acc[6] + acc[7])) + tail;
    }
    double r = x[0];
    if (op == KRED_MIN) {
        for (Z i = 1; i < n; ++i) r = x[i] < r ? x[i] : r;
    } else {
        for (Z i = 1; i < n; ++i) r = x[i] > r ? x[i] : r;
    }
    return r;
}

// --- the matrix product ------------------------------------------------------
//
// The shape every fast GEMM has (Goto and van de Geijn): block the product so
// that a panel of B stays in L2 and a sliver of A in L1, copy both into the
// order the inner kernel reads them ("packing"), and make the inner kernel a
// fixed MR x NR tile of C held entirely in registers while it walks k. With
// four-wide vectors a 6 x 8 tile is twelve accumulators, two loads of B and a
// broadcast of A per step -- fifteen of AVX2's sixteen registers.
//
// The vector type is GCC's and Clang's portable one, as wide as the target's
// registers: four doubles with AVX, two on the baseline (SSE2 on x86-64, NEON
// on ARM). The tile is the same 6 x 8 either way; only the number of vectors
// per row of it changes. With AVX2 `c += a * b` contracts to an FMA.

#if defined(__AVX__)
constexpr size_t VW = 4;
#else
constexpr size_t VW = 2;
#endif
typedef double vec __attribute__((vector_size(VW * sizeof(double)), aligned(8)));

constexpr Z MR = 6, NR = 8, NV = NR / VW, KC = 256, MC = 96, NC = 2048;

inline vec loadv(const double* p) {
    vec v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
inline void storev(double* p, vec v) { std::memcpy(p, &v, sizeof v); }

/// B's `kc x nc` block as panels of NR columns, each `kc x NR` and contiguous,
/// the last padded with zeros.
void pack_b(const double* B, Z ldb, Z kc, Z nc, double* Bp) {
    for (Z j = 0; j < nc; j += NR) {
        const Z w = std::min(NR, nc - j);
        double* d = Bp + (j / NR) * kc * NR;
        for (Z k = 0; k < kc; ++k, d += NR) {
            const double* src = B + k * ldb + j;
            Z l = 0;
            for (; l < w; ++l) d[l] = src[l];
            for (; l < NR; ++l) d[l] = 0;
        }
    }
}

/// A's `mc x kc` block as slivers of MR rows, each stored column by column.
void pack_a(const double* A, Z lda, Z mc, Z kc, double* Ap) {
    for (Z i = 0; i < mc; i += MR) {
        const Z h = std::min(MR, mc - i);
        double* d = Ap + (i / MR) * kc * MR;
        for (Z k = 0; k < kc; ++k, d += MR) {
            Z r = 0;
            for (; r < h; ++r) d[r] = A[(i + r) * lda + k];
            for (; r < MR; ++r) d[r] = 0;
        }
    }
}

/// C's `h x w` tile (at most MR x NR) plus the product of an A sliver and a B
/// panel. A full tile is written straight from the registers; an edge tile
/// goes through a scratch copy so that nothing past C's edge is touched.
inline void micro(Z kc, const double* __restrict a, const double* __restrict b, double* C,
                  Z ldc, Z h, Z w) {
    vec c[MR][NV] = {};
    for (Z k = 0; k < kc; ++k, a += MR, b += NR) {
        vec bv[NV];
#pragma GCC unroll 4
        for (Z v = 0; v < NV; ++v) bv[v] = loadv(b + v * VW);
#pragma GCC unroll 8
        for (Z r = 0; r < MR; ++r) {
            const vec ar = vec{} + a[r];
#pragma GCC unroll 4
            for (Z v = 0; v < NV; ++v) c[r][v] += ar * bv[v];
        }
    }
    if (h == MR && w == NR) {
        for (Z r = 0; r < MR; ++r)
            for (Z v = 0; v < NV; ++v) {
                double* at = C + r * ldc + v * VW;
                storev(at, loadv(at) + c[r][v]);
            }
        return;
    }
    double t[MR][NR];
    std::memcpy(t, c, sizeof t);
    for (Z r = 0; r < h; ++r)
        for (Z l = 0; l < w; ++l) C[r * ldc + l] += t[r][l];
}

/// `C = A x B` for row-major `A` (M x K) and `B` (K x N), `C` (M x N)
/// overwritten. Thin products go to the loops that suit them: packing a panel
/// to multiply it by one column is all overhead.
void gemm(const double* A, const double* B, double* C, Z M, Z K, Z N) {
    if (N == 1) {
        for (Z i = 0; i < M; ++i) C[i] = dot(A + i * K, B, K);
        return;
    }
    std::memset(C, 0, M * N * sizeof(double));
    if (M == 1) {
        // A row vector times a matrix: a sum of B's rows, each scaled, which
        // reads B once and in order.
        for (Z k = 0; k < K; ++k) {
            const double a = A[k];
            const double* __restrict b = B + k * N;
            double* __restrict c = C;
            for (Z j = 0; j < N; ++j) c[j] += a * b[j];
        }
        return;
    }
    std::vector<double> Bp(KC * (NC + NR)), Ap((MC + MR) * KC);
    for (Z jc = 0; jc < N; jc += NC) {
        const Z nc = std::min(NC, N - jc);
        for (Z pc = 0; pc < K; pc += KC) {
            const Z kc = std::min(KC, K - pc);
            pack_b(B + pc * N + jc, N, kc, nc, Bp.data());
            for (Z ic = 0; ic < M; ic += MC) {
                const Z mc = std::min(MC, M - ic);
                pack_a(A + ic * K + pc, K, mc, kc, Ap.data());
                for (Z jr = 0; jr < nc; jr += NR)
                    for (Z ir = 0; ir < mc; ir += MR)
                        micro(kc, Ap.data() + (ir / MR) * kc * MR, Bp.data() + (jr / NR) * kc * NR,
                              C + (ic + ir) * N + jc + jr, N, std::min(MR, mc - ir),
                              std::min(NR, nc - jr));
            }
        }
    }
}

/// `out` (C x R) is `in` (R x C) transposed, in 32 x 32 blocks so that both
/// sides are read and written a cache line at a time.
void transpose(const double* __restrict in, double* __restrict out, Z R, Z C) {
    constexpr Z B = 32;
    for (Z i0 = 0; i0 < R; i0 += B)
        for (Z j0 = 0; j0 < C; j0 += B) {
            const Z i1 = std::min(R, i0 + B), j1 = std::min(C, j0 + B);
            for (Z i = i0; i < i1; ++i)
                for (Z j = j0; j < j1; ++j) out[j * R + i] = in[i * C + j];
        }
}

}  // namespace

const TensorKernels kernels = {
    binary, scalar, unary, dot, reduce, gemm, transpose,
};

}  // namespace DREAM_KERNEL_NS
}  // namespace dream
