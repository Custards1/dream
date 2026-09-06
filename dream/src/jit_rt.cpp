// Slow paths for JIT-compiled code. These are the interpreter's own helpers,
// so a compiled function and an interpreted one cannot disagree about what an
// operation means.

#include "jit_rt.hpp"

#include "interp.hpp"
#include "process.hpp"
#include "runtime.hpp"

using namespace dream;

extern "C" {

int dream_rt_force(Process* p, Value v, Value* out) {
    Value result;
    if (!force_whnf(*p, v, &result)) {
        *out = p->result;
        return 0;
    }
    *out = result;
    return 1;
}

int dream_rt_arith(Process* p, int32_t op, Value a, Value b, Value* out) {
    return jit_arith(*p, Op(op), a, b, out) ? 1 : 0;
}

int dream_rt_compare(Process* p, int32_t op, Value a, Value b, Value* out) {
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

}  // extern "C"
