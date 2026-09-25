// Slow paths for JIT-compiled code. These are the interpreter's own helpers,
// so a compiled function and an interpreted one cannot disagree about what an
// operation means.

#include "jit_rt.hpp"

#include <cmath>
#include <cstring>

#include "builtins.hpp"
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

int dream_rt_switch_head(Process* p, Value v, Value* out) {
    v = resolve(v);
    if (!is_obj(v, ObjType::Cons)) {
        *out = UNIT;
        return 1;
    }
    return dream_rt_force(p, static_cast<ConsObj*>(as_obj(v))->head, out);
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

// ---------------------------------------------------------------------------
// Calling a host native
//
// What licenses this is written where the decision is made, in `native_site` in
// jit.cpp: only a *pure* native of fixed arity is admitted, which is what makes
// the shape below enough. A pure native performs no effect, so nothing here has
// to be ordered against anything; and it cannot park, because the natives that
// park are every one of them impure and the Dream work a pure one runs
// underneath itself is pure too. `Block` is answered all the same, in the one
// way that is honest about not having handled it.
//
// The heap is pinned for the duration, for the reason every helper in this file
// that can run Dream code pins it: the compiled frame above holds its values in
// machine registers, and a collection would move what they name. That is the
// same bargain `dream_rt_force` makes, and it has the same cost -- a native that
// walks a long lazy structure allocates for the length of the walk with nothing
// able to collect any of it. See `PinsTheHeap`, and "A JIT that can allocate" in
// CLAUDE.md, whose de-pinning stage is what would remove it.
// ---------------------------------------------------------------------------

namespace {

/// Run `fn` as the native `callee`, with `argc` arguments already in hand.
///
/// The arguments are pushed onto the process's value stack rather than passed
/// from an array of this frame's own, because that is where a native looks for
/// them: the interpreter hands one `p.stack.data() + base`, and a native that
/// grows the stack underneath itself -- which several do, to hold a list across
/// a force -- is written against exactly that. Doing it the same way here means
/// no native has to know which tier called it.
int run_native(Process& p, Value callee, NativeFn fn, uint32_t argc, const Value* args,
               Value* out) {
    PinsTheHeap pinned(p);
    const size_t base = p.stack.size();
    for (uint32_t i = 0; i < argc; ++i) p.stack.push_back(args[i]);

    NativeResult r = fn(p, callee, p.stack.data() + base, argc);
    p.stack.resize(base);

    switch (r.outcome) {
        case NativeOutcome::Value:
            *out = r.value;
            return 1;
        case NativeOutcome::Raise:
            *out = r.value;
            return 0;
        case NativeOutcome::Enter: {
            // The native found something and did not force it. The machine
            // forces such a value as the continuation of the call, so forcing
            // it here is the same point in the program -- what differs is that
            // this is a nested loop on the C++ stack where the machine's is a
            // heap continuation. `Process::force_nest` is what bounds that, and
            // `kMaxForceNestForCompiled` is where the interpreter takes over.
            //
            // Pinned rather than held in a local on principle: nothing can
            // collect underneath `PinsTheHeap`, but a value that crosses a
            // force belongs somewhere the collector rewrites, and the cost of
            // saying so is a push and a pop.
            Pin keep(p, r.value);
            Value w;
            if (!force_whnf(p, keep.get(), &w)) {
                *out = p.result;
                return 0;
            }
            *out = w;
            return 1;
        }
        case NativeOutcome::Block:
            break;
    }

    // The native parked the process, which compiled code has no way to be
    // resumed into: there is no continuation to push, because the body it would
    // resume is machine code half way through a register allocation. It is
    // unreachable for an admitted native -- only an impure one blocks -- and the
    // answer here is the one `dream_rt_force` gives for the same situation,
    // which is to report what the process is holding as a raise rather than to
    // pretend the call produced a value.
    *out = p.result;
    return 0;
}

}  // namespace

int dream_rt_native(Process* p, uint32_t module_index, uint32_t member_index, uint32_t argc,
                    Value a0, Value a1, Value a2, Value a3, Value* out) {
    const ModuleDef& def = p->runtime().modules()[module_index];
    const NativeDef& m = def.members[member_index];

    // The callee value this process hands out for this member, which is the one
    // `Field` would have produced: built once and cached, because an FFI binding
    // reads its own `user` field back out of it and because rebuilding it would
    // allocate a name and an object per call. The cache is indexed by the
    // runtime-wide member id, exactly as `member_value_slow` indexes it.
    const uint32_t id = def.member_base + member_index;
    if (p->native_cache.empty()) {
        p->native_cache.assign(p->runtime().native_member_count(), NIL_SLOT);
    }
    Value callee = id < p->native_cache.size() ? p->native_cache[id] : NIL_SLOT;
    if (callee == NIL_SLOT) {
        Value name = p->heap().make_string(m.name, uint32_t(std::strlen(m.name)));
        callee = make_native(*p, m.fn, name, m.arity, m.strict_mask, m.user);
        if (id < p->native_cache.size()) p->native_cache[id] = callee;
    }

    const Value args[4] = {a0, a1, a2, a3};
    return run_native(*p, callee, m.fn, argc, args, out);
}

int dream_rt_builtin(Process* p, uint32_t builtin_id, uint32_t argc, Value a0, Value a1, Value a2,
                     Value a3, Value* out) {
    // A builtin is an immediate: it names itself, and there is nothing to build
    // or to cache.
    const BuiltinDef& bd = builtin_def(builtin_id);
    const Value args[4] = {a0, a1, a2, a3};
    return run_native(*p, make_builtin(builtin_id), bd.fn, argc, args, out);
}

}  // extern "C"
