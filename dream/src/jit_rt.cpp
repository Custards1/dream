// Slow paths for JIT-compiled code. These are the interpreter's own helpers,
// so a compiled function and an interpreted one cannot disagree about what an
// operation means.

#include "jit_rt.hpp"

#include <cmath>

#include "interp.hpp"
#include "process.hpp"
#include "runtime.hpp"

using namespace dream;

extern "C" {

// A compiled frame holds its slots in registers the collector cannot see, so
// nothing reached from one may collect -- see `PinsTheHeap` in interp.hpp. Each
// helper below that can run the machine says so. `force_whnf` would pin anyway
// for want of a vouch, but saying it here is what keeps the guarantee if
// someone later vouches for something `arith` or `compare` reaches.
int dream_rt_force(Process* p, Value v, Value* out) {
    PinsTheHeap pinned(*p);
    Value result;
    if (!force_whnf(*p, v, &result)) {
        *out = p->result;
        return 0;
    }
    *out = result;
    return 1;
}

int dream_rt_arith(Process* p, int32_t op, Value a, Value b, Value* out) {
    PinsTheHeap pinned(*p);
    return jit_arith(*p, Op(op), a, b, out) ? 1 : 0;
}

int dream_rt_compare(Process* p, int32_t op, Value a, Value b, Value* out) {
    PinsTheHeap pinned(*p);
    return jit_compare(*p, Op(op), a, b, out) ? 1 : 0;
}

Value dream_rt_float(Process* p, double d) { return p->heap().make_float(d); }

namespace {
/// The double a number in hand stands for. Only ever asked of a value `arith`
/// has just answered with, which is a fixnum or a float box and nothing else.
inline bool number_as_double(Value v, double* out) {
    if (is_fixnum(v)) {
        *out = double(fixnum_value(v));
        return true;
    }
    if (is_obj(v, ObjType::Float)) {
        *out = static_cast<FloatObj*>(as_obj(v))->value;
        return true;
    }
    return false;
}
}  // namespace

int dream_rt_arith_f(Process* p, int32_t op, Value a, Value b, double* out, Value* err) {
    PinsTheHeap pinned(*p);
    Value r;
    if (!jit_arith(*p, Op(op), a, b, &r)) {
        *err = r;
        return 0;
    }
    // An operation with a float on one side of it answers a float or raises;
    // there is no third case, and the header says why. Checked rather than
    // assumed, because the assumption is about `arith` and this file is not.
    if (!number_as_double(r, out)) {
        *err = raise_error(*p, well_known(p->runtime()).type_error,
                           "arithmetic on a float did not answer a number");
        return 0;
    }
    return 1;
}

int dream_rt_to_int(Process* p, Value v, Value* out) {
    if (is_fixnum(v)) {
        *out = v;
        return 1;
    }
    if (is_obj(v, ObjType::Float)) {
        *out = make_integer(*p, int64_t(static_cast<FloatObj*>(as_obj(v))->value));
        return 1;
    }
    *out = raise_error(*p, well_known(p->runtime()).type_error, "to_int needs a number");
    return 0;
}

int dream_rt_floor(Process* p, Value v, Value* out) {
    if (is_fixnum(v)) {
        *out = v;
        return 1;
    }
    if (is_obj(v, ObjType::Float)) {
        *out = make_integer(*p, int64_t(std::floor(static_cast<FloatObj*>(as_obj(v))->value)));
        return 1;
    }
    *out = raise_error(*p, well_known(p->runtime()).type_error, "floor needs a number");
    return 0;
}

int dream_rt_abs(Process* p, Value v, Value* out) {
    if (is_fixnum(v)) {
        int64_t n = fixnum_value(v);
        *out = make_integer(*p, n < 0 ? -n : n);
        return 1;
    }
    if (is_obj(v, ObjType::Float)) {
        *out = p->heap().make_float(std::fabs(static_cast<FloatObj*>(as_obj(v))->value));
        return 1;
    }
    *out = raise_error(*p, well_known(p->runtime()).type_error, "abs needs a number");
    return 0;
}

Value dream_rt_int_of_double(Process* p, double d) { return make_integer(*p, int64_t(d)); }

Value dream_rt_type_error(Process* p, const char* message) {
    return raise_error(*p, well_known(p->runtime()).type_error, message);
}

int64_t* dream_rt_reduction_slot(Process* p) { return &p->reductions; }

Value* dream_rt_frame_slots(Value frame) {
    return static_cast<FrameObj*>(as_obj(frame))->slots();
}

/// Store `v` into slot `index` of a frame, running the write barrier. A
/// compiled function's own slots live in registers, so the interpreter's
/// per-store barrier in Op::Bind never sees them; the spill back to the heap
/// frame at a yield is where an old-to-young edge can appear, and where the
/// next minor collection has to be told about it.
void dream_rt_frame_store(Process* p, Value frame, uint32_t index, Value v) {
    auto* f = static_cast<FrameObj*>(as_obj(frame));
    p->heap().remember_if_old(f, v);
    value_slot_store(&f->slots()[index], v);
}

}  // extern "C"
