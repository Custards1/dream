// Slow paths for JIT-compiled code. These are the interpreter's own helpers,
// so a compiled function and an interpreted one cannot disagree about what an
// operation means.

#include "jit_rt.hpp"

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
