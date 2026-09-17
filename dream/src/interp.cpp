#include <cstdlib>

#include "interp.hpp"

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "builtins.hpp"
#include "jit.hpp"

namespace dream {

std::atomic<uint64_t> g_thunk_counts[64];
const bool g_probe_thunk = std::getenv("DREAM_PROBE_THUNK") != nullptr;

namespace {

inline const Image& img_of(Process& p) { return *p.code; }

/// A nested `force_whnf` loop is in progress on this process's machine stack.
/// See `Process::force_nest` and `kMaxForceNestForCompiled`: the depth is what
/// `enter_function` reads before offering the compiled tier, because each link
/// of a lazy chain a *compiled* body forces is a C++ frame.
struct ForceNest {
    Process& p;
    explicit ForceNest(Process& proc) : p(proc) { ++p.force_nest; }
    ~ForceNest() { --p.force_nest; }
    ForceNest(const ForceNest&) = delete;
    ForceNest& operator=(const ForceNest&) = delete;
};

inline void eval_node(Process& p, uint32_t node, Value frame) {
    p.mode = Mode::Eval;
    p.node = node;
    p.frame = frame;
}

inline void ret(Process& p, Value v) {
    p.mode = Mode::Return;
    p.result = v;
}

inline void do_raise(Process& p, Value err) {
    p.mode = Mode::Raise;
    p.result = err;
}

inline void push_cont(Process& p, ContKind k, uint32_t a, uint32_t b, uint32_t c, Value v1) {
    p.conts.push_back(Cont{k, 0, 0, a, b, c, v1});
}

/// Arrange for an operation to be run again when the process is woken.
///
/// Normally that means pushing its continuation on top. But when a nested
/// force inside the operation was *suspended* rather than abandoned (see
/// `force_whnf`), the work it had already done is still on the continuation
/// stack and has to finish first -- so the retry is spliced in underneath it.
/// The operation is then re-run against a value that is already forced, which
/// is what stops it repeating any effect the suspended work performed.
inline void push_retry(Process& p, ContKind k, uint32_t a, uint32_t b, uint32_t c, Value v1) {
    Cont retry{k, 0, 0, a, b, c, v1};
    if (p.force_blocked) {
        p.conts.insert(p.conts.begin() + std::ptrdiff_t(p.force_resume_at), retry);
        p.force_blocked = false;
    } else {
        p.conts.push_back(retry);
    }
}

// ---------------------------------------------------------------------------
// Numbers
// ---------------------------------------------------------------------------

inline bool is_number(Value v) { return is_fixnum(v) || is_obj(v, ObjType::Float); }
inline double to_double(Value v) {
    return is_fixnum(v) ? double(fixnum_value(v)) : static_cast<FloatObj*>(as_obj(v))->value;
}

}  // namespace

Value make_native(Process& p, NativeFn fn, Value name, uint32_t arity,
                  uint32_t strict_mask, uint64_t user) {
    auto* nat = static_cast<NativeObj*>(p.heap().alloc(
        ObjType::Native, sizeof(void*) + sizeof(Value) + 8 + sizeof(uint64_t)));
    nat->fn = reinterpret_cast<void*>(fn);
    nat->name = name;
    nat->arity = arity;
    nat->strict_mask = strict_mask;
    nat->user = user;
    return from_obj(nat);
}

Value make_integer(Process& p, int64_t v) {
    if (fixnum_fits(v)) return make_fixnum(v);
    // Beyond 63 bits we fall back to a float rather than growing a bignum
    // type; the compiler only ever emits values that fit.
    return p.heap().make_float(double(v));
}

Value raise_error(Process& p, uint32_t kind_atom, const std::string& message) {
    Value msg = p.heap().make_string(message.data(), uint32_t(message.size()));
    return p.heap().make_error(make_atom(kind_atom), msg);
}

namespace {

[[nodiscard]] Value type_error(Process& p, const std::string& msg) {
    return raise_error(p, well_known(p.runtime()).type_error, msg);
}

/// The `StrObj` for image string constant `index`, made once per process.
///
/// A literal is immutable and a program cannot observe which copy of it it
/// holds -- strings compare and hash by their bytes -- so evaluating `"("` in
/// a loop has no business allocating. Nothing in the VM ever writes into a
/// string it did not just make (concatenation and slicing both build a fresh
/// one), which is what makes sharing safe rather than merely cheap.
///
/// The cache is a process root, so a shared literal survives collection and
/// keeps its identity across one.
inline Value literal_string(Process& p, uint32_t index) {
    if (index < p.string_cache.size()) {
        Value hit = p.string_cache[index];
        if (hit != NIL_SLOT) return hit;
    } else {
        p.string_cache.assign(img_of(p).string_count(), NIL_SLOT);
    }
    StringRef s = img_of(p).str(index);
    Value v = p.heap().make_string(s.data, s.len);
    if (index < p.string_cache.size()) p.string_cache[index] = v;
    return v;
}

/// The boxed float for image float constant `index`, made once per process.
/// Same argument as `literal_string`: immutable, no observable identity, and
/// otherwise one allocation every time the constant is reached.
inline Value literal_float(Process& p, uint32_t index) {
    if (index < p.float_cache.size()) {
        Value hit = p.float_cache[index];
        if (hit != NIL_SLOT) return hit;
    } else {
        p.float_cache.assign(img_of(p).float_count(), NIL_SLOT);
    }
    Value v = p.heap().make_float(img_of(p).real(index));
    if (index < p.float_cache.size()) p.float_cache[index] = v;
    return v;
}

// ---------------------------------------------------------------------------
// Structural equality
//
// Forces as it descends, so `[1,2] == [1,2]` compares element by element even
// though both lists arrive as unevaluated thunks.
// ---------------------------------------------------------------------------

bool values_equal(Process& p, Value a, Value b, bool* raised, int depth);

bool equal_children(Process& p, Value* xs, Value* ys, uint32_t n, bool* raised, int depth) {
    for (uint32_t i = 0; i < n; ++i) {
        if (!values_equal(p, xs[i], ys[i], raised, depth + 1)) return false;
        if (*raised) return false;
    }
    return true;
}

bool values_equal(Process& p, Value a, Value b, bool* raised, int depth) {
    if (depth > 512) {
        p.result = type_error(p, "structure is too deeply nested to compare");
        *raised = true;
        return false;
    }
    // `resolve` and the WHNF test are inline; `force_whnf` is a call into a
    // nested machine loop. Both operands of a comparison have usually been
    // forced by the machine already, so ask before calling.
    Value fa = resolve(a), fb = resolve(b);
    if ((!is_whnf(fa) && !force_whnf(p, fa, &fa)) ||
        (!is_whnf(fb) && !force_whnf(p, fb, &fb))) {
        *raised = true;
        return false;
    }
    if (fa == fb) return true;

    if (is_number(fa) && is_number(fb)) {
        if (is_fixnum(fa) && is_fixnum(fb)) return fa == fb;
        return to_double(fa) == to_double(fb);
    }
    if (!is_ptr(fa) || !is_ptr(fb)) return false;
    Obj* oa = as_obj(fa);
    Obj* ob = as_obj(fb);

    // Strings compare by their bytes, and a big string is a string whose bytes
    // happen to live in the image. Deciding otherwise would make
    // `str.slice 0 4 payload == "%PDF"` -- the first thing anyone does with a
    // payload -- quietly false, which is a worse surprise than the two
    // representations being visible to `type_of`.
    Bytes xs, ys;
    if (string_bytes(fa, &xs) && string_bytes(fb, &ys)) return bytes_equal(xs, ys);

    if (oa->type != ob->type) return false;

    switch (oa->type) {
        case ObjType::Cons: {
            auto* x = static_cast<ConsObj*>(oa);
            auto* y = static_cast<ConsObj*>(ob);
            if (!values_equal(p, x->head, y->head, raised, depth + 1) || *raised) return false;
            return values_equal(p, x->tail, y->tail, raised, depth + 1);
        }
        case ObjType::Array: {
            auto* x = static_cast<ArrayObj*>(oa);
            auto* y = static_cast<ArrayObj*>(ob);
            if (x->len != y->len) return false;
            return equal_children(p, x->items(), y->items(), x->len, raised, depth);
        }
        case ObjType::Pid:
            return static_cast<PidObj*>(oa)->id == static_cast<PidObj*>(ob)->id;
        case ObjType::Map: {
            auto* x = static_cast<MapObj*>(oa);
            auto* y = static_cast<MapObj*>(ob);
            if (x->count != y->count) return false;
            // Two maps holding the same entries may have different shapes only
            // if their hashes differ, which they cannot -- but comparing by
            // lookup rather than by shape is what makes that not something to
            // rely on.
            std::vector<std::pair<Value, Value>> entries;
            map_collect(fa, entries);
            for (auto& [k, v] : entries) {
                Value found;
                if (!map_lookup(p, fb, k, &found)) return false;
                if (!values_equal(p, v, found, raised, depth + 1)) return false;
                if (*raised) return false;
            }
            return true;
        }
        default:
            return false;  // functions, modules: identity only, already checked
    }
}

// ---------------------------------------------------------------------------
// Applying
// ---------------------------------------------------------------------------

void resume_native(Process& p, Value callee, uint32_t base, uint32_t argc, uint32_t from);
void enter(Process& p, Value v);
bool callee_operand(Process& p, const Image& img, uint32_t node, Value frame, Value* out);

/// The kinds of node `thunk_for` can answer with a value instead of a
/// suspension. A strict argument shaped like one of these is left to it: those
/// are the cheap cases it exists for, and most of them cost no allocation at
/// all. Everything else -- a call, a block, a member, a conditional -- it
/// always suspends, and a suspension the native is about to force on the very
/// next step is what `fill_native_args` is about.
///
/// The two lists are allowed to drift. A kind named here that `thunk_for`
/// suspends anyway is a thunk built where one need not have been, which is
/// what every call did before this existed; a kind missing from here is an
/// argument the machine evaluates rather than computes, which is what the
/// general path does. Neither is a change in meaning, which is why this can
/// stay a plain list rather than a second copy of the rules.
inline bool thunk_for_may_answer(Op op) {
    switch (op) {
        case Op::ConstInt: case Op::ConstStr: case Op::ConstFloat:
        case Op::ConstChar: case Op::ConstBool: case Op::ConstAtom:
        case Op::Unit: case Op::Builtin: case Op::Local: case Op::Capture:
        case Op::MakeClosure: case Op::MakeThunk:
        case Op::Add: case Op::Sub: case Op::Mul: case Op::Get:
            return true;
        default:
            return false;
    }
}

/// A builtin is an immediate carrying an index into a static table; a host
/// native is an object carrying the same two numbers. Every caller wants one
/// or both of them and none of them care which kind it has.
inline uint32_t native_arity(Value callee) {
    if (is_builtin(callee)) return builtin_def(uint32_t(imm_payload(callee))).arity;
    return static_cast<NativeObj*>(as_obj(callee))->arity;
}
inline uint32_t native_strict_mask(Value callee) {
    if (is_builtin(callee)) return builtin_def(uint32_t(imm_payload(callee))).strict_mask;
    return static_cast<NativeObj*>(as_obj(callee))->strict_mask;
}


/// The depth of nested `force_whnf` loops past which `enter_function` stops
/// offering the compiled tier. See `Process::force_nest`: interpreted code
/// nests forces only as deep as natives on the same C++ chain actually sit one
/// inside another (a `strict!` over nested natives, tens at most), while a
/// *compiled* chain of lazy links spends one C++ frame per link and would blow
/// the machine stack long before `DREAM_MAX_DEPTH`. Small enough to keep the
/// interpreter in charge of anything pathological, large enough that every
/// healthy program nests well under it.
constexpr uint32_t kMaxForceNestForCompiled = 256;
/// Enter a function body, taking the compiled tier when one is available.
void enter_function(Process& p, uint32_t func_index, const FuncRec& f, Value frame) {
    Jit* jit = p.runtime().jit();
    // When a nested force is already deep on the process's machine stack, the
    // compiled tier is not offered at all. A compiled body forces a slot by
    // *calling*, so a chain of lazy links under a force is a C++ frame per link
    // -- one compiled body, one `dream_rt_force`, one `force_whnf` -- and a
    // long enough chain overflows the machine stack before `DREAM_MAX_DEPTH` is
    // anywhere in sight. The interpreter makes those frames on the heap and can
    // reach its limit, so it takes the chain over. Interpreted code never nests
    // this deep (each of its forces returns before the next), which is what
    // lets a small bound here be free for every healthy program. See

    // `Process::force_nest`, `dream/tests/programs/force_chain.dr`, and "Fixed:
    // compiled code forcing a long thunk chain crashed" in CLAUDE.md.
    if (jit && p.force_nest <= kMaxForceNestForCompiled) {
        // One inlined, lock-free read of the tier table (see `Jit::tier`), so
        // a process whose functions never grow hot -- most of a compile --
        // pays a load and a branch for the JIT being present.
        CompiledFn fn = jit->tier(func_index);
        if (fn) {
            int status = 0;
            // Compiled code spends the same budget the interpreter does, so
            // fold what it used into the process's total -- but only what *it*
            // spent. A compiled body re-enters the interpreter for anything it
            // cannot do itself, and those reductions have already counted
            // themselves; charging the whole fall in the budget counted them
            // twice, and twice again for every compiled frame above them. On a
            // self-compile that reported 194M reductions where the same work
            // without the JIT reports 61M, which made `--stats` and every
            // number derived from it meaningless whenever the JIT was on.
            const int64_t before = p.reductions;
            const uint64_t before_total = p.total_reductions;
            Value r = fn(&p, frame, &status);
            // Clamped, because a nested force underneath this call may have
            // re-armed the budget (`Process::slice_spent`), which leaves more
            // in it than there was to start with.
            const int64_t fell = before - p.reductions;
            const uint64_t spent = fell > 0 ? uint64_t(fell) : 0;
            const uint64_t nested = p.total_reductions - before_total;
            const uint64_t own = spent > nested ? spent - nested : 0;
            p.total_reductions += own;
            // And attributed, which nothing did before: a function the JIT
            // compiled was invisible to `--profile` however hot it was.
            if (own && p.runtime().profiling()) p.runtime().note_reductions(func_index, own);
            if (status == JitOk) { ret(p, r); return; }
            if (status == JitRaised) { do_raise(p, r); return; }
            if (status == JitTooDeep) {
                // Compiled self recursion ran out of machine stack. The body
                // is arithmetic, so nothing has happened that has to be undone
                // -- the call simply has not been made yet, and the
                // interpreter, whose recursion is heap continuations bounded
                // by `DREAM_MAX_DEPTH`, makes it instead.
                //
                // And it takes the function over for good. A function that
                // recurses deeper than the machine stack allows does it on
                // every call, and each attempt would run a megabyte of frames
                // before finding out; worse, the interpreted recursion would
                // re-enter the compiled tier at every level and do it again
                // per level. One store here is what keeps the fallback linear.
                jit->deoptimize(func_index);
                eval_node(p, f.body, frame);
                return;
            }
            if (status == JitBailed) {
                // An entry guard did not hold: a slot the compiled body carries
                // as an unboxed double was handed something that is not a float
                // in hand. Nothing has been spent and nothing written, so the
                // interpreter runs this one call -- and the tier stays, because
                // unlike running out of machine stack this is a property of the
                // call rather than of the function. The guard costs a load and
                // two branches, so a function that bails every time is no
                // slower than one that was never compiled.
                eval_node(p, f.body, frame);
                return;
            }
            // Yielded: the compiled loop spent its budget and wrote its
            // loop-carried state back to the frame. Fall through to the
            // interpreter, which resumes the body and lets the scheduler
            // preempt at the next safepoint.
        }
    }
    eval_node(p, f.body, frame);
}
inline size_t max(size_t a, size_t b) { return a > b ? a : b; }

void do_apply(Process& p, Value callee, uint32_t argc) {
    callee = resolve(callee);
    const Image& img = img_of(p);
    const size_t base = max(p.stack.size() - argc, 0);

    if (!is_whnf(callee)) {
        // Reached through a path that did not evaluate the callee node, such
        // as priming a process with a value read out of another heap.
        push_cont(p, ContKind::ApplyTo, argc, 0, 0, UNIT);
        enter(p, callee);
        return;
    }

    if (is_obj(callee, ObjType::Pap)) {
        // Fold the earlier arguments in ahead of the new ones and retry.
        auto* pap = static_cast<PapObj*>(as_obj(callee));
        uint32_t have = pap->nargs;
        p.stack.insert(p.stack.begin() + long(base), pap->args(), pap->args() + have);
        do_apply(p, pap->fn, argc + have);
        return;
    }

    if (is_obj(callee, ObjType::Closure)) {
        auto* cl = static_cast<ClosureObj*>(as_obj(callee));
        const FuncRec& f = img.func(cl->func);
        uint32_t arity = f.arity;

        if (arity == 0) {
            // A zero-arity function -- a thunk object or an impure action.
            // It consumes no arguments; any that were supplied apply to its
            // result, so `main! ()` and `go!` both do the right thing.
            Value fr = p.heap().make_frame(callee, f.slots);
            if (argc > 0) push_cont(p, ContKind::ApplyTo, argc, 0, 0, UNIT);
            enter_function(p, cl->func, f, fr);
            return;
        }
        if (argc < arity) {
            Value pap = p.heap().make_pap(callee, argc);
            auto* po = static_cast<PapObj*>(as_obj(pap));
            for (uint32_t i = 0; i < argc; ++i) po->args()[i] = p.stack[base + i];
            p.stack.resize(base);
            ret(p, pap);
            return;
        }

        Value fr = p.heap().make_frame_filling(callee, f.slots, arity);
        auto* fo = static_cast<FrameObj*>(as_obj(fr));
        for (uint32_t i = 0; i < arity; ++i) fo->slots()[i] = p.stack[base + i];

        // Over-application: the extra arguments apply to whatever comes back.
        uint32_t extra = argc - arity;
        p.stack.erase(p.stack.begin() + long(base), p.stack.begin() + long(base + arity));
        if (extra > 0) push_cont(p, ContKind::ApplyTo, extra, 0, 0, UNIT);
        enter_function(p, cl->func, f, fr);
        return;
    }

    if (is_builtin(callee) || is_obj(callee, ObjType::Native)) {
        const uint32_t arity = native_arity(callee);
        if (arity == NATIVE_VARIADIC) {
            // Takes the whole application. There is nothing to under- or
            // over-apply: `print! a b c` hands the native all three.
            resume_native(p, callee, uint32_t(base), argc, 0);
            return;
        }
        if (argc < arity) {
            Value pap = p.heap().make_pap(callee, argc);
            auto* po = static_cast<PapObj*>(as_obj(pap));
            for (uint32_t i = 0; i < argc; ++i) po->args()[i] = p.stack[base + i];
            p.stack.resize(base);
            ret(p, pap);
            return;
        }
        if (argc > arity) {
            // Apply exactly `arity`, then apply the result to the rest.
            push_cont(p, ContKind::ApplyTo, argc - arity, 0, 0, UNIT);
            // The extra arguments sit above the ones we are about to consume,
            // so move them out of the way first -- into `pins` rather than a
            // C++ vector, because the native is about to run Dream code that
            // may collect and these are live references like any other.
            const size_t spilled = p.pins.size();
            p.pins.insert(p.pins.end(), p.stack.begin() + long(base + arity), p.stack.end());
            p.stack.resize(base + arity);
            resume_native(p, callee, uint32_t(base), arity, 0);
            // Re-push the leftovers underneath the pending ApplyTo.
            p.stack.insert(p.stack.end(), p.pins.begin() + long(spilled), p.pins.end());
            p.pins.resize(spilled);
            return;
        }
        resume_native(p, callee, uint32_t(base), arity, 0);
        return;
    }

    do_raise(p, raise_error(p, well_known(p.runtime()).not_a_function,
                            describe(p, callee) + " is not a function"));
}

/// Force the strict arguments of a native one at a time, then call it.
void resume_native(Process& p, Value callee, uint32_t base, uint32_t argc, uint32_t from) {
    uint32_t mask, arity;
    NativeFn fn;
    if (is_builtin(callee)) {
        const BuiltinDef& bd = builtin_def(uint32_t(imm_payload(callee)));
        mask = bd.strict_mask;
        arity = bd.arity;
        fn = bd.fn;
    } else {
        auto* nat = static_cast<NativeObj*>(as_obj(callee));
        mask = nat->strict_mask;
        arity = nat->arity;
        fn = reinterpret_cast<NativeFn>(nat->fn);
    }
    // A variadic native forces every argument: `strict_mask` is a 32-bit map of
    // argument positions, and an unbounded call has no fixed positions to map.
    // The `i < 32` guard is the same point from the other side -- it keeps a
    // 33rd argument to a fixed-arity native from shifting out of range.
    const bool all_strict = arity == NATIVE_VARIADIC;

    for (uint32_t i = from; i < argc; ++i) {
        Value v = resolve(p.stack[base + i]);
        p.stack[base + i] = v;
        const bool strict = all_strict || (i < 32 && (mask & (1u << i)));
        if (strict && !is_whnf(v)) {
            push_cont(p, ContKind::NativeArg, base, argc, i, callee);
            // `enter` sets up the machine to force it; the NativeArg
            // continuation writes the result back into the argument slot.
            p.mode = Mode::Return;
            p.result = v;
            // Fall through to the forcing path below by re-entering the value.
            Value thunk = v;
            auto* t = static_cast<ThunkObj*>(as_obj(thunk));
            if (as_obj(thunk)->type == ObjType::Blackhole) {
                p.conts.pop_back();
                do_raise(p, raise_error(p, well_known(p.runtime()).loop,
                                        "value depends on itself"));
                return;
            }
            obj_type_store(as_obj(thunk), ObjType::Blackhole);
            push_cont(p, ContKind::UpdateThunk, 0, 0, 0, thunk);
            eval_node(p, t->node, t->frame);
            return;
        }
    }

    // The callee is named again after the call -- `NativeRetry` has to record
    // which native to re-enter -- so it has to survive one. A builtin is an
    // immediate and cannot move; a host native is an object, and although it
    // is reachable from `native_cache` and so can never be *freed* under us,
    // a minor collection would promote it and leave this copy pointing at the
    // forwarded husk. Pinning is a push and a pop, and only on the host path.
    const bool pinned = !is_builtin(callee);
    Pin callee_pin(p, pinned ? callee : UNIT);

    NativeResult r = fn(p, callee, p.stack.data() + base, argc);
    if (pinned) callee = callee_pin.get();

    // A nested force inside the native hit a blocking operation and gave up
    // (see `force_whnf`). Whatever the native decided to return is built on a
    // value it never actually got, so the answer is not "this failed" -- it is
    // "not yet". Park and call it again, rather than trusting the result.
    //
    // Catching it here rather than in each native means a native cannot forget
    // to: the ones that force a value deeply are exactly the ones that would
    // not think to check.
    if (p.park_requested && r.outcome != NativeOutcome::Block) {
        r = NativeResult::block();
    }

    switch (r.outcome) {
        case NativeOutcome::Value:
            p.stack.resize(base);
            ret(p, r.value);
            break;
        case NativeOutcome::Raise:
            p.stack.resize(base);
            do_raise(p, r.value);
            break;
        case NativeOutcome::Enter:
            // Something the native found and did not force. Forced here, as the
            // continuation of the call, however deep its evaluation goes.
            p.stack.resize(base);
            enter(p, r.value);
            break;
        case NativeOutcome::Block:
            // The native parked the process. Leave the arguments in place and
            // arrange to call it again when the scheduler wakes us.
            push_retry(p, ContKind::NativeRetry, base, argc, 0, callee);
            p.mode = Mode::Return;
            p.result = UNIT;
            break;
    }
}

/// Evaluate a native's arguments where they stand, suspending only the lazy
/// ones, and then call it.
///
/// A native declares which of its arguments it forces, and `resume_native`
/// forces exactly those -- but by then each one is already a Thunk, because the
/// call site builds a Thunk for every argument it passes. For a strict argument
/// that Thunk is a temporary and nothing else: `resume_native` immediately
/// blackholes it, evaluates it, and overwrites it with an Indirect, so an
/// allocation, two writes and a continuation have been spent carrying `(node,
/// frame)` four steps. It is never shared, either, because the native forces it
/// before it can hand it to anyone. On a self-compile three million of the ten
/// million thunks a compile makes are exactly this.
///
/// So the machine's own continuation carries what the Thunk carried: the call
/// node says which arguments are left and where to find them, and the frame is
/// what they are evaluated in. Nothing is forced here that `resume_native`
/// would not have forced, and in the same order, so an argument that raises
/// raises in the same place and with the same error -- this is a change of
/// representation, not of strictness. A lazy argument is suspended exactly as
/// before.
///
/// The callee is read again on the way back rather than carried across, because
/// a `Cont` has one value field and the frame is in it. Reading it is cheap and
/// it is the same callee: naming one is a read, not an evaluation (see
/// `callee_operand`), and a frame's slots do not change while one of its own
/// sub-expressions is being evaluated -- the same assumption `BinRight` makes
/// when it reads its right operand after forcing its left. It is also what
/// keeps this safe across a collection, which a carried copy would not be.
void fill_native_args(Process& p, uint32_t call_node, Value callee, uint32_t base,
                      uint32_t from, Value frame) {
    const Image& img = img_of(p);
    const Node& n = img.node(call_node);
    const uint32_t* kids = img.kids_at(n.b);
    const uint32_t mask = native_strict_mask(callee);
    // The same rule `resume_native` states, for the same reason: a variadic
    // native has no fixed argument positions for a 32-bit mask to describe, so
    // it forces all of them.
    const bool all_strict = native_arity(callee) == NATIVE_VARIADIC;

    for (uint32_t i = from; i < n.c; ++i) {
        // A lazy argument is suspended exactly as before, and so is a strict
        // one of a shape `thunk_for` answers without allocating -- computing
        // those is cheaper than evaluating them, and taking them away from it
        // would undo its work rather than add to it.
        const bool strict = all_strict || (i < 32 && (mask & (1u << i)));
        if (!strict || thunk_for_may_answer(Op(img.node(kids[i]).op))) {
            p.stack.push_back(thunk_for(p, kids[i], frame));
            continue;
        }
        push_cont(p, ContKind::NativeArgs, call_node, base, i, frame);
        eval_node(p, kids[i], frame);
        return;
    }
    resume_native(p, callee, base, n.c, 0);
}

// ---------------------------------------------------------------------------
// Forcing
// ---------------------------------------------------------------------------

/// Build a closure for `func_index`, capturing from `frame`. Creating a
/// closure allocates but cannot fail at the language level and has no effects,
/// which is what lets `thunk_for` do it eagerly instead of suspending it.
bool build_closure(Process& p, uint32_t func_index, Value frame, Value* out) {
    const Image& img = img_of(p);
    const FuncRec& f = img.func(func_index);
    Value cl = p.heap().make_closure(func_index, f.n_captures);
    auto* co = static_cast<ClosureObj*>(as_obj(cl));
    auto* fo = static_cast<FrameObj*>(as_obj(frame));
    auto* parent = is_obj(fo->closure, ObjType::Closure)
                       ? static_cast<ClosureObj*>(as_obj(fo->closure))
                       : nullptr;
    for (uint32_t i = 0; i < f.n_captures; ++i) {
        uint32_t desc = img.kid(f.captures_off + i);
        uint32_t idx = desc & ~CAP_FROM_CAPTURE;
        if (desc & CAP_FROM_CAPTURE) {
            if (!parent || idx >= parent->ncaps) {
                *out = type_error(p, "malformed capture descriptor");
                return false;
            }
            co->caps()[i] = parent->caps()[idx];
        } else {
            if (idx >= fo->nslots) {
                *out = type_error(p, "malformed capture descriptor");
                return false;
            }
            // Capture the slot's current contents -- normally a thunk. Sharing
            // that thunk is what lets a `let rec` closure see itself: the slot
            // is filled in before anyone forces it.
            co->caps()[i] = fo->slots()[idx];
        }
    }
    *out = cl;
    return true;
}

void enter(Process& p, Value v) {
    v = resolve(v);
    if (!is_ptr(v)) {
        ret(p, v);
        return;
    }
    ObjType t = as_obj(v)->type;
    if (t == ObjType::Thunk) {
        auto* th = static_cast<ThunkObj*>(as_obj(v));
        uint32_t node = th->node;
        Value fr = th->frame;
        // Blackhole first: if evaluating this thunk reaches it again, we have
        // a value defined in terms of itself and must say so rather than spin.
        obj_type_store(as_obj(v), ObjType::Blackhole);
        push_cont(p, ContKind::UpdateThunk, 0, 0, 0, v);
        eval_node(p, node, fr);
        return;
    }
    if (t == ObjType::Blackhole) {
        do_raise(p, raise_error(p, well_known(p.runtime()).loop, "value depends on itself"));
        return;
    }
    ret(p, v);
}

// ---------------------------------------------------------------------------
// Blocks
// ---------------------------------------------------------------------------

void advance_block(Process& p, uint32_t kids_off, uint32_t count, uint32_t index, Value frame) {
    const Image& img = img_of(p);
    while (index < count) {
        uint32_t stmt = img.kid(kids_off + index);
        const Node& sn = img.node(stmt);
        const bool last = (index + 1 == count);

        if (Op(sn.op) == Op::Bind) {
            auto* fo = static_cast<FrameObj*>(as_obj(frame));
            const Node& vn = img.node(sn.b);
            Value bound;
            if (Op(vn.op) == Op::MakeClosure || Op(vn.op) == Op::MakeThunk) {
                // A local binding may refer to itself -- `let rec go x = .. go ..`
                // -- and a closure built right now would capture the slot while
                // it is still empty. Suspending it instead means the slot
                // already holds this thunk by the time the closure is made, so
                // the capture sees something that resolves to the closure.
                bound = p.heap().make_thunk(sn.b, frame);
            } else {
                bound = thunk_for(p, sn.b, frame);
            }
            // A frame may have outlived its call -- one held by a long-lived
            // thunk -- so the bind can point an old object at a young one.
            value_slot_store(&fo->slots()[sn.a], bound);
            p.heap().remember_if_old(fo, bound);
            // A binding is lazy unless the compiler marked it strict, which it
            // does exactly when the bound expression has effects that must
            // happen where they were written.
            if (sn.flags & F_STRICT) {
                push_cont(p, ContKind::BlockNext, kids_off, count, index + 1, frame);
                enter(p, bound);
                return;
            }
            ++index;
            continue;
        }

        if (last) {
            eval_node(p, stmt, frame);
            return;
        }
        // A non-final statement exists only for its effects. The compiler marks
        // the ones that have any; the rest are dead and forcing them could
        // raise an error the program never asked for.
        if (sn.flags & F_STRICT) {
            push_cont(p, ContKind::BlockNext, kids_off, count, index + 1, frame);
            eval_node(p, stmt, frame);
            return;
        }
        ++index;
    }
    ret(p, UNIT);
}

void advance_map(Process& p, uint32_t kids_off, uint32_t pairs, uint32_t index, Value frame) {
    if (index >= pairs) {
        // Follow any indirection an insert left behind before handing the map
        // out; callers expect a value in normal form.
        Value map = resolve(p.stack.back());
        p.stack.pop_back();
        ret(p, map);
        return;
    }
    push_cont(p, ContKind::MapEntry, kids_off, pairs, index, frame);
    eval_node(p, img_of(p).kid(kids_off + index * 2), frame);
}

// ---------------------------------------------------------------------------
// Arithmetic and comparison
// ---------------------------------------------------------------------------

bool arith(Process& p, Op op, Value a, Value b, Value* out);
bool compare(Process& p, Op op, Value a, Value b, Value* out);

/// Is this the head of a list -- a cell, or the empty list?
inline bool is_listish(Value v) { return is_nil(v) || is_obj(v, ObjType::Cons); }

/// `xs + ys`. The spine of `xs` is copied and its end pointed at `ys`;
/// elements are never forced, so concatenating lists of unevaluated work stays
/// lazy in the elements.
bool concat_lists(Process& p, Value a, Value b, Value* out) {
    if (is_nil(a)) {
        *out = b;
        return true;
    }
    // Forcing the spine can collect, so the working set lives on the process
    // stack rather than in C++ locals: slot 0 holds the tail to append, slot 1
    // is the cursor, and the heads pile up above them.
    //
    // It does *not* vouch (`VouchesForGc`), though everything below it is
    // written for one. This is not reached through `resume_native` the way a
    // native is -- it is the `+` operator, reached from `arith`, which
    // `finish_binop` calls while holding both operands in C++ locals and which
    // JIT-compiled code calls while holding its entire frame in machine
    // registers. Neither of those survives a collection, and the frame that
    // cannot survive one is the frame that decides. Vouching here made
    // `effects_once` hang about one run in twelve.
    const size_t base = p.stack.size();
    p.stack.push_back(b);
    p.stack.push_back(a);
    for (;;) {
        Value w;
        if (!force_whnf(p, p.stack[base + 1], &w)) {
            *out = p.result;
            // Not when the force was only suspended: its stack is above ours.
            if (!p.force_blocked) p.stack.resize(base);
            return false;
        }
        if (is_nil(w)) break;
        if (!is_obj(w, ObjType::Cons)) {
            *out = type_error(p, "the left side of a list `+` is not a proper list");
            p.stack.resize(base);
            return false;
        }
        auto* c = static_cast<ConsObj*>(as_obj(w));
        Value head = c->head;
        p.stack[base + 1] = c->tail;
        p.stack.push_back(head);
    }

    Value result = p.stack[base];
    for (size_t i = p.stack.size(); i-- > base + 2;) {
        result = p.heap().make_cons(p.stack[i], result);
    }
    p.stack.resize(base);
    *out = result;
    return true;
}

bool arith(Process& p, Op op, Value a, Value b, Value* out) {
    const auto& wk = well_known(p.runtime());

    if (op == Op::Add && is_listish(a) && is_listish(b)) {
        return concat_lists(p, a, b, out);
    }

    if (op == Op::Add && is_obj(a, ObjType::Str) && is_obj(b, ObjType::Str)) {
        auto* x = static_cast<StrObj*>(as_obj(a));
        auto* y = static_cast<StrObj*>(as_obj(b));
        Value s = p.heap().make_string(nullptr, x->len + y->len);
        auto* z = static_cast<StrObj*>(as_obj(s));
        std::memcpy(z->data(), static_cast<StrObj*>(as_obj(a))->data(), x->len);
        std::memcpy(z->data() + x->len, static_cast<StrObj*>(as_obj(b))->data(), y->len);
        *out = s;
        return true;
    }

    // A big string is deliberately not concatenable. Joining one would have to
    // produce a `StrObj`, which copies the bytes into the heap and caps them at
    // 4 GiB -- undoing both halves of what the payload is for. The error names
    // the two ways out rather than leaving the caller to guess.
    if (is_obj(a, ObjType::BigStr) || is_obj(b, ObjType::BigStr)) {
        *out = type_error(p, std::string("cannot apply `") + op_name(op) +
                                 "` to a big string: it is a view into the image, not heap "
                                 "memory. Take a `str.slice` of it, or hand the whole of it "
                                 "to `io.write!`.");
        return false;
    }

    if (!is_number(a) || !is_number(b)) {
        *out = type_error(p, std::string("cannot apply `") + op_name(op) + "` to " +
                                 describe(p, a) + " and " + describe(p, b));
        return false;
    }

    if (is_fixnum(a) && is_fixnum(b)) {
        int64_t x = fixnum_value(a), y = fixnum_value(b), r = 0;
        bool overflow = false;
        switch (op) {
            case Op::Add: overflow = __builtin_add_overflow(x, y, &r); break;
            case Op::Sub: overflow = __builtin_sub_overflow(x, y, &r); break;
            case Op::Mul: overflow = __builtin_mul_overflow(x, y, &r); break;
            case Op::Div:
                if (y == 0) {
                    *out = raise_error(p, wk.divide_by_zero, "division by zero");
                    return false;
                }
                r = x / y;
                break;
            case Op::Mod:
                if (y == 0) {
                    *out = raise_error(p, wk.divide_by_zero, "remainder by zero");
                    return false;
                }
                r = x % y;
                break;
            default: break;
        }
        if (!overflow && fixnum_fits(r)) {
            *out = make_fixnum(r);
            return true;
        }
        // Fall through to double on overflow rather than silently wrapping.
    }

    double x = to_double(a), y = to_double(b), r = 0;
    switch (op) {
        case Op::Add: r = x + y; break;
        case Op::Sub: r = x - y; break;
        case Op::Mul: r = x * y; break;
        case Op::Div:
            if (y == 0.0 && is_fixnum(b)) {
                *out = raise_error(p, wk.divide_by_zero, "division by zero");
                return false;
            }
            r = x / y;
            break;
        case Op::Mod: r = std::fmod(x, y); break;
        default: break;
    }
    *out = p.heap().make_float(r);
    return true;
}

bool compare(Process& p, Op op, Value a, Value b, Value* out) {
    if (op == Op::Eq || op == Op::Ne) {
        bool raised = false;
        bool eq = values_equal(p, a, b, &raised, 0);
        if (raised) {
            *out = p.result;
            return false;
        }
        *out = make_bool(op == Op::Eq ? eq : !eq);
        return true;
    }

    int cmp;
    if (is_number(a) && is_number(b)) {
        if (is_fixnum(a) && is_fixnum(b)) {
            int64_t x = fixnum_value(a), y = fixnum_value(b);
            cmp = x < y ? -1 : (x > y ? 1 : 0);
        } else {
            double x = to_double(a), y = to_double(b);
            cmp = x < y ? -1 : (x > y ? 1 : 0);
        }
    } else if (is_stringish(a) && is_stringish(b)) {
        Bytes x, y;
        string_bytes(a, &x);
        string_bytes(b, &y);
        cmp = bytes_compare(x, y);
    } else if (is_char(a) && is_char(b)) {
        uint64_t x = imm_payload(a), y = imm_payload(b);
        cmp = x < y ? -1 : (x > y ? 1 : 0);
    } else {
        *out = type_error(p, std::string("cannot order ") + describe(p, a) + " against " +
                                 describe(p, b));
        return false;
    }

    bool r = false;
    switch (op) {
        case Op::Lt: r = cmp < 0; break;
        case Op::Le: r = cmp <= 0; break;
        case Op::Gt: r = cmp > 0; break;
        case Op::Ge: r = cmp >= 0; break;
        default: break;
    }
    *out = make_bool(r);
    return true;
}

// ---------------------------------------------------------------------------
// Module member lookup
// ---------------------------------------------------------------------------

/// `obj.member`, where `obj` is a host module.
///
/// This runs on every call of every `std.core` member, which in a program of
/// any size means millions of times, so what it does *not* do matters. The
/// module is found through a cache indexed by the import record, and which
/// member was wanted is remembered against the node that asked -- packed as
/// the import index and the member's position in one word, so a node that
/// somehow meets a different module falls back to the search rather than
/// reading the wrong member out of the right one.
/// The slow half: find the member, number it, and build its function value.
/// Reached once per call site, and then never again.
[[gnu::noinline]] bool member_value_slow(Process& p, uint32_t imp, uint32_t name_index,
                                         uint32_t node_index, Value* out) {
    const Image& img = img_of(p);
    StringRef member = img.str(name_index);
    const ModuleDef* def = p.runtime().module_for_import(imp);
    if (!def) {
        *out = raise_error(p, well_known(p.runtime()).no_such_member,
                           "module `" + img.str(img.import(imp).path).str() +
                               "` is not provided by this runtime");
        return false;
    }
    uint32_t at = UINT32_MAX;
    for (uint32_t i = 0; i < def->members.size(); ++i) {
        if (member.equals(def->members[i].name)) {
            at = i;
            break;
        }
    }
    if (at == UINT32_MAX) {
        *out = raise_error(p, well_known(p.runtime()).no_such_member,
                           "module `" + img.str(img.import(imp).path).str() +
                               "` has no member `" + member.str() + "`");
        return false;
    }

    // The value handed back is decided by which member this is and nothing
    // else, so this process builds it once. What that replaces is two
    // allocations -- a copy of the name and a `NativeObj` -- at every call of
    // every `std` member, which in a program of any size is most of what the
    // allocator does.
    const uint32_t id = def->member_base + at;
    if (p.native_cache.empty()) {
        p.native_cache.assign(p.runtime().native_member_count(), NIL_SLOT);
    }
    // The site remembers the *cache slot*, not the member's position in its
    // module: that is what lets the fast path answer without asking which
    // module this import names. The import index rides along, so a node that
    // somehow meets a different module falls back here rather than reading
    // the wrong member out of the right one.
    p.runtime().set_field_cache(node_index, (uint64_t(imp + 1) << 32) | id);

    if (id < p.native_cache.size() && p.native_cache[id] != NIL_SLOT) {
        *out = p.native_cache[id];
        return true;
    }
    const NativeDef& m = def->members[at];
    Value name = p.heap().make_string(member.data, member.len);
    Value fn = make_native(p, m.fn, name, m.arity, m.strict_mask, m.user);
    if (id < p.native_cache.size()) p.native_cache[id] = fn;
    *out = fn;
    return true;
}

/// `obj.member`, where `obj` is a host module.
///
/// This runs on every call of every `std.core` member, which in a program of
/// any size means millions of times, so what it does *not* do matters. Warm,
/// it is three loads and a compare: the module's import index out of the
/// object, the cache slot out of the site that asked, and the function value
/// out of this process's table. No name is copied, no module is searched, and
/// nothing is allocated.
inline bool member_value(Process& p, Value obj, uint32_t name_index, uint32_t node_index,
                         Value* out) {
    if (!is_obj(obj, ObjType::Module)) {
        *out = type_error(p, describe(p, obj) + " has no member `" +
                                 img_of(p).str(name_index).str() + "`");
        return false;
    }
    const uint32_t imp = static_cast<ModuleObj*>(as_obj(obj))->import_index;
    const uint64_t cached = p.runtime().field_cache(node_index);
    if (uint32_t(cached >> 32) == imp + 1) {
        const uint32_t id = uint32_t(cached);
        if (id < p.native_cache.size()) {
            Value hit = p.native_cache[id];
            if (hit != NIL_SLOT) {
                *out = hit;
                return true;
            }
        }
    }
    return member_value_slow(p, imp, name_index, node_index, out);
}

void resolve_field(Process& p, Value obj, uint32_t name_index, uint32_t node_index) {
    Value v;
    if (member_value(p, obj, name_index, node_index, &v)) ret(p, v);
    else do_raise(p, v);
}

// ---------------------------------------------------------------------------
// Operands that need no evaluation
//
// The machine's general shape is "push a continuation, evaluate the part,
// come back" -- which is what makes it interruptible and what keeps its depth
// on the heap. But a great many operands are not expressions at all. A global,
// a builtin, a parameter, a member of an imported module: each of these is a
// *read*, and running it through the machine costs a continuation, a reduction
// and a return trip to learn what a load would have said. These two answer
// "can I just read it?", and the sites that ask fall back to the general path
// whenever the answer is no.
// ---------------------------------------------------------------------------

/// The module a node names, when it names one already in normal form.
bool module_operand(Process& p, const Image& img, uint32_t node, Value frame, Value* out) {
    const Node& n = img.node(node);
    if (Op(n.op) == Op::Global) {
        const GlobalRec& g = img.global(n.a);
        if (g.kind != GLOBAL_MODULE) return false;
        *out = global_value(p, n.a);
        return true;
    }
    // A module can also sit in a slot -- `import a.{b}` binds one, and so does
    // passing a module to a function.
    Value v;
    if (Op(n.op) == Op::Local) {
        v = static_cast<FrameObj*>(as_obj(frame))->slots()[n.a];
    } else if (Op(n.op) == Op::Capture) {
        auto* fo = static_cast<FrameObj*>(as_obj(frame));
        v = static_cast<ClosureObj*>(as_obj(fo->closure))->caps()[n.a];
    } else {
        return false;
    }
    if (v == NIL_SLOT) return false;
    v = resolve(v);
    if (!is_obj(v, ObjType::Module)) return false;
    *out = v;
    return true;
}

/// The value of a node that already is one: a constant, or a binding whose
/// contents are in weak head normal form.
///
/// This is what lets an operator finish where it starts. `acc + x` over two
/// bound locals is the whole of a fold's inner loop, and running each side
/// through the machine costs a continuation, an Eval and a Return to discover
/// that the slot held a number all along -- five steps to add two integers.
/// A local still holding a thunk answers false, and the general path forces it
/// exactly as before, so nothing is evaluated here that was not evaluated
/// there.
bool operand_value(Process& p, const Image& img, uint32_t node, Value frame, Value* out) {
    const Node& n = img.node(node);
    switch (Op(n.op)) {
        case Op::ConstInt: *out = make_integer(p, img.integer(n.a)); return true;
        case Op::ConstFloat: *out = literal_float(p, n.a); return true;
        case Op::ConstStr: *out = literal_string(p, n.a); return true;
        case Op::ConstChar: *out = make_char(uint32_t(n.a)); return true;
        case Op::ConstBool: *out = make_bool(n.a != 0); return true;
        case Op::ConstAtom: *out = make_atom(p.runtime().image_atom(n.a)); return true;
        case Op::Unit: *out = UNIT; return true;
        case Op::Builtin: *out = make_builtin(n.a); return true;
        case Op::Local: {
            Value v = static_cast<FrameObj*>(as_obj(frame))->slots()[n.a];
            // An unbound slot is an error the general path reports; saying so
            // in two places would be saying it differently in two places.
            if (v == NIL_SLOT) return false;
            v = resolve(v);
            if (!is_whnf(v)) return false;
            *out = v;
            return true;
        }
        case Op::Capture: {
            auto* fo = static_cast<FrameObj*>(as_obj(frame));
            Value v = resolve(static_cast<ClosureObj*>(as_obj(fo->closure))->caps()[n.a]);
            if (!is_whnf(v)) return false;
            *out = v;
            return true;
        }
        default:
            return false;
    }
}

/// The callee a node names, when naming it needs no evaluation. `*out` is
/// `NIL_SLOT` for a local that has not been bound yet, which is the caller's
/// error to report.
bool callee_operand(Process& p, const Image& img, uint32_t node, Value frame, Value* out) {
    const Node& n = img.node(node);
    switch (Op(n.op)) {
        case Op::Local:
            *out = static_cast<FrameObj*>(as_obj(frame))->slots()[n.a];
            return true;
        case Op::Capture: {
            auto* fo = static_cast<FrameObj*>(as_obj(frame));
            *out = static_cast<ClosureObj*>(as_obj(fo->closure))->caps()[n.a];
            return true;
        }
        case Op::Builtin:
            *out = make_builtin(n.a);
            return true;
        case Op::Global: {
            const GlobalRec& g = img.global(n.a);
            // A parameterless global is a memoized value or an action to
            // perform, and both have rules of their own; only a function with
            // parameters is a closure that is simply there.
            if (g.kind != GLOBAL_FUNCTION || img.func(g.target).arity == 0) return false;
            *out = global_value(p, n.a);
            return true;
        }
        case Op::Field: {
            Value obj;
            if (!module_operand(p, img, n.a, frame, &obj)) return false;
            // A member that cannot be resolved is left to the general path,
            // which raises the same error where the machine can see it.
            return member_value(p, obj, n.b, node, out);
        }
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// One step
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Containers: `get` and `set`
//
// One pair of operations over every container a program builds: a map by key,
// an array or a list by position. They are opcodes rather than natives because
// of what a native must do with the element it hands back. A native's result
// has to be a value, and an element is usually still a thunk, so a native
// forces it underneath itself -- in a nested machine loop, on the C++ stack. A
// map field updated twenty thousand times and never read is twenty thousand of
// those nested inside each other the first time it is read, which was deep
// enough to run the 8 MiB stack of a Nix builder out while the compiler compiled
// itself. Here the element is entered like any other value, so its depth is
// continuations: heap, with a limit that raises rather than a guard page.
// ---------------------------------------------------------------------------

/// Nothing at the key. When the `get` has a fallback, evaluate it and say so.
bool get_fallback(Process& p, uint32_t at, Value frame) {
    const Node& n = img_of(p).node(at);
    if (n.c == NO_NODE) return false;
    eval_node(p, n.c, frame);
    return true;
}

inline uint32_t out_of_bounds_atom(Process& p) { return well_known(p.runtime()).out_of_bounds; }

/// Step `remaining` more cells down a list and answer the head found there. A
/// tail that is not a value yet is entered with a continuation to come back to,
/// so a long lazy list is walked by the machine rather than by recursion.
void list_get(Process& p, uint32_t at, Value cur, uint32_t remaining, uint32_t index, Value frame) {
    for (;;) {
        cur = resolve(cur);
        if (!is_obj(cur, ObjType::Cons)) {
            if (get_fallback(p, at, frame)) return;
            do_raise(p, raise_error(p, out_of_bounds_atom(p),
                                    "index " + std::to_string(index) +
                                        " is past the end of a list of " +
                                        std::to_string(index - remaining)));
            return;
        }
        auto* cell = static_cast<ConsObj*>(as_obj(cur));
        if (remaining == 0) {
            enter(p, cell->head);
            return;
        }
        --remaining;
        Value tail = resolve(cell->tail);
        if (!is_whnf(tail)) {
            push_cont(p, ContKind::GetWalk, remaining, at, index, frame);
            enter(p, tail);
            return;
        }
        cur = tail;
    }
}

/// Step `remaining` more cells down a list, keeping every cell passed on the
/// value stack, and answer the list with the element at `index` replaced. The
/// cells in front of it are new; everything behind it is shared, and so is
/// every element but the one replaced.
void list_set(Process& p, uint32_t at, Value cur, uint32_t remaining, uint32_t index, Value frame) {
    for (;;) {
        cur = resolve(cur);
        const uint32_t walked = index - remaining;
        if (!is_obj(cur, ObjType::Cons)) {
            p.stack.resize(p.stack.size() - walked);
            do_raise(p, raise_error(p, out_of_bounds_atom(p),
                                    "index " + std::to_string(index) +
                                        " is past the end of a list of " + std::to_string(walked)));
            return;
        }
        if (remaining == 0) {
            Value value = thunk_for(p, img_of(p).node(at).c, frame);
            Value out = p.heap().make_cons(value, static_cast<ConsObj*>(as_obj(cur))->tail);
            for (uint32_t i = 0; i < index; ++i) {
                Value head = static_cast<ConsObj*>(as_obj(p.stack.back()))->head;
                out = p.heap().make_cons(head, out);
                p.stack.pop_back();
            }
            ret(p, out);
            return;
        }
        p.stack.push_back(cur);
        --remaining;
        Value tail = resolve(static_cast<ConsObj*>(as_obj(cur))->tail);
        if (!is_whnf(tail)) {
            push_cont(p, ContKind::SetWalk, remaining, at, index, frame);
            enter(p, tail);
            return;
        }
        cur = tail;
    }
}

inline bool is_sequence(Value v) {
    return is_obj(v, ObjType::Array) || is_nil(v) || is_obj(v, ObjType::Cons);
}

/// `c.[k]` and `c.[k else d]`, with the container and the key both in hand.
void container_get(Process& p, uint32_t at, Value container, Value key, Value frame) {
    container = resolve(container);
    key = resolve(key);
    if (is_obj(container, ObjType::Map)) {
        Value found;
        if (map_lookup(p, container, key, &found)) {
            enter(p, found);
            return;
        }
        if (get_fallback(p, at, frame)) return;
        do_raise(p, raise_error(p, well_known(p.runtime()).no_such_key,
                                "the map has no key " + describe(p, key)));
        return;
    }
    if (!is_sequence(container)) {
        do_raise(p, type_error(p, "`.[ ]` reads a map, an array or a list, not " +
                                      describe(p, container)));
        return;
    }
    if (!is_fixnum(key)) {
        do_raise(p, type_error(p, "a position is an integer, not " + describe(p, key)));
        return;
    }
    const int64_t k = fixnum_value(key);
    if (is_obj(container, ObjType::Array)) {
        auto* a = static_cast<ArrayObj*>(as_obj(container));
        if (k >= 0 && k < int64_t(a->len)) {
            enter(p, a->items()[k]);
            return;
        }
        if (get_fallback(p, at, frame)) return;
        do_raise(p, raise_error(p, out_of_bounds_atom(p),
                                "index " + std::to_string(k) + " is outside an array of " +
                                    std::to_string(a->len)));
        return;
    }
    if (k < 0 || k > int64_t(0xFFFFFFFFu)) {
        if (get_fallback(p, at, frame)) return;
        do_raise(p, raise_error(p, out_of_bounds_atom(p),
                                "index " + std::to_string(k) + " is not a position in a list"));
        return;
    }
    list_get(p, at, container, uint32_t(k), uint32_t(k), frame);
}

/// `c.[k => v]`, with the container and the key in hand. The value is stored
/// unforced, as a map literal's values are.
void container_set(Process& p, uint32_t at, Value container, Value key, Value frame) {
    container = resolve(container);
    key = resolve(key);
    const uint32_t value_node = img_of(p).node(at).c;
    if (is_obj(container, ObjType::Map)) {
        Value value = thunk_for(p, value_node, frame);
        ret(p, map_insert(p, container, key, value));
        return;
    }
    if (!is_sequence(container)) {
        do_raise(p, type_error(p, "`.[ => ]` changes a map, an array or a list, not " +
                                      describe(p, container)));
        return;
    }
    if (!is_fixnum(key)) {
        do_raise(p, type_error(p, "a position is an integer, not " + describe(p, key)));
        return;
    }
    const int64_t k = fixnum_value(key);
    if (is_obj(container, ObjType::Array)) {
        const uint32_t n = static_cast<ArrayObj*>(as_obj(container))->len;
        if (k < 0 || k >= int64_t(n)) {
            do_raise(p, raise_error(p, out_of_bounds_atom(p),
                                    "index " + std::to_string(k) + " is outside an array of " +
                                        std::to_string(n)));
            return;
        }
        // A copy: the array given is a value, and someone else may hold it.
        // A large array is allocated straight into old space, so every store
        // into it goes through the barrier.
        Value value = thunk_for(p, value_node, frame);
        Value out = p.heap().make_array(n);
        auto* src = static_cast<ArrayObj*>(as_obj(container));
        auto* dst = static_cast<ArrayObj*>(as_obj(out));
        for (uint32_t i = 0; i < n; ++i) {
            Value item = uint32_t(k) == i ? value : src->items()[i];
            dst->items()[i] = item;
            p.heap().remember_if_old(dst, item);
        }
        ret(p, out);
        return;
    }
    if (k < 0 || k > int64_t(0xFFFFFFFFu)) {
        do_raise(p, raise_error(p, out_of_bounds_atom(p),
                                "index " + std::to_string(k) + " is not a position in a list"));
        return;
    }
    list_set(p, at, container, uint32_t(k), uint32_t(k), frame);
}

// ---------------------------------------------------------------------------
// Finishing an operator
//
// Each of these is the tail of a continuation, factored out so that the same
// code runs whether the operands arrived through the machine or were read
// straight out of the frame (see `operand_value`).
// ---------------------------------------------------------------------------

void finish_binary(Process& p, Op op, Value lhs, Value rhs) {
    // Two fixnums is what an operator almost always has, and answering it here
    // skips the whole of `arith` and `compare`: the list, string and big-string
    // cases each of them tries first, and the `values_equal` recursion behind
    // `==`. Overflow and division by zero fall through to the general code
    // rather than being handled twice.
    if (is_fixnum(lhs) && is_fixnum(rhs)) {
        const int64_t x = fixnum_value(lhs), y = fixnum_value(rhs);
        int64_t r;
        switch (op) {
            case Op::Add:
                if (!__builtin_add_overflow(x, y, &r) && fixnum_fits(r)) {
                    ret(p, make_fixnum(r));
                    return;
                }
                break;
            case Op::Sub:
                if (!__builtin_sub_overflow(x, y, &r) && fixnum_fits(r)) {
                    ret(p, make_fixnum(r));
                    return;
                }
                break;
            case Op::Mul:
                if (!__builtin_mul_overflow(x, y, &r) && fixnum_fits(r)) {
                    ret(p, make_fixnum(r));
                    return;
                }
                break;
            case Op::Div:
                // A quotient leaves the fixnum range in exactly one place:
                // `-2^62 / -1` is `2^62`, one past the largest. `arith` hands
                // that to the double path and answers a float; this one used
                // to build the fixnum anyway, which wraps it to the *smallest*
                // fixnum. It was the only number the two paths disagreed
                // about, and which one a program got depended on whether both
                // operands happened to be in hand -- so `12 / n` and a `12 / n`
                // whose `n` arrived as a thunk gave different answers.
                if (y != 0) {
                    r = x / y;
                    if (fixnum_fits(r)) {
                        ret(p, make_fixnum(r));
                        return;
                    }
                }
                break;
            case Op::Mod:
                // A remainder is smaller than its divisor, so it is a fixnum
                // whenever the divisor was one: nothing to check.
                if (y != 0) {
                    ret(p, make_fixnum(x % y));
                    return;
                }
                break;
            case Op::Eq: ret(p, make_bool(x == y)); return;
            case Op::Ne: ret(p, make_bool(x != y)); return;
            case Op::Lt: ret(p, make_bool(x < y)); return;
            case Op::Le: ret(p, make_bool(x <= y)); return;
            case Op::Gt: ret(p, make_bool(x > y)); return;
            case Op::Ge: ret(p, make_bool(x >= y)); return;
            default: break;
        }
    }
    Value out;
    bool ok = (op == Op::Eq || op == Op::Ne || op == Op::Lt || op == Op::Le ||
               op == Op::Gt || op == Op::Ge)
                  ? compare(p, op, lhs, rhs, &out)
                  : arith(p, op, lhs, rhs, &out);

    // A nested force inside the operator hit a blocking operation and
    // gave up (see `force_whnf`) -- comparing two lists whose elements
    // are still `join!`s, say. Both operands are ordinary values and
    // both operators force, so this is reachable without any effect
    // being written at the comparison itself.
    //
    // The answer is not "this failed" but "not yet", so park and redo
    // the whole operation when the scheduler wakes us. Forcing is
    // memoised, so the retry pays only for what had not been forced
    // yet. This is the same handshake `apply_native` performs for a
    // native whose nested force gave up; the operators need their own
    // because they are the machine rather than a native call.
    //
    // Without it `ok` is false with a `p.result` that was never an
    // error, and the process dies with an error that has no kind and
    // no message.
    if (p.park_requested) {
        // Both operands travel in a cell: this retry may be spliced in
        // below a suspended force, and that work overwrites `p.result`
        // before the retry is reached.
        Value pair = p.heap().make_cons(lhs, rhs);
        push_retry(p, ContKind::BinFinish, uint32_t(op), 1, 0, pair);
        return;  // the mode is already Return
    }
    if (ok) ret(p, out); else do_raise(p, out);
}

/// `if cond { a } else { b }`, with the condition in hand.
void take_branch(Process& p, Value cond, uint32_t then_node, uint32_t else_node, Value frame) {
    if (!is_bool(cond)) {
        do_raise(p, type_error(p, "`if` needs a bool, got " + describe(p, cond)));
        return;
    }
    if (truthy(cond)) {
        eval_node(p, then_node, frame);
    } else if (else_node == NO_NODE) {
        ret(p, UNIT);
    } else {
        eval_node(p, else_node, frame);
    }
}

/// `a && b` and `a || b`, with the left operand in hand. The right is entered
/// only when the left has not already decided the answer.
void finish_logic(Process& p, Op op, Value lhs, uint32_t right_node, Value frame) {
    if (!is_bool(lhs)) {
        do_raise(p, type_error(p, std::string("`") + op_name(op) + "` needs a bool, got " +
                                      describe(p, lhs)));
        return;
    }
    if (op == Op::And && !truthy(lhs)) { ret(p, FALSE_V); return; }
    if (op == Op::Or && truthy(lhs)) { ret(p, TRUE_V); return; }
    eval_node(p, right_node, frame);
}

/// `-x` and `not x`, with the operand in hand.
void finish_unary(Process& p, Op op, Value v) {
    if (op == Op::Neg) {
        if (is_fixnum(v)) { ret(p, make_integer(p, -fixnum_value(v))); return; }
        if (is_obj(v, ObjType::Float)) {
            ret(p, p.heap().make_float(-static_cast<FloatObj*>(as_obj(v))->value));
            return;
        }
        do_raise(p, type_error(p, "cannot negate " + describe(p, v)));
        return;
    }
    if (!is_bool(v)) {
        do_raise(p, type_error(p, "`not` needs a bool, got " + describe(p, v)));
        return;
    }
    ret(p, make_bool(!truthy(v)));
}

void step_eval(Process& p) {
    const Image& img = img_of(p);
    const Node& n = img.node(p.node);
    Value frame = p.frame;

    switch (Op(n.op)) {
        case Op::ConstInt: ret(p, make_integer(p, img.integer(n.a))); return;
        case Op::ConstFloat: ret(p, literal_float(p, n.a)); return;
        case Op::ConstStr: ret(p, literal_string(p, n.a)); return;
        case Op::ConstChar: ret(p, make_char(uint32_t(n.a))); return;
        case Op::ConstBool: ret(p, make_bool(n.a != 0)); return;
        case Op::ConstAtom:
            // Image atom indices are remapped onto the runtime's global table,
            // so atoms from different modules with the same name compare equal.
            // The map is built at load; here it is one array read.
            ret(p, make_atom(p.runtime().image_atom(n.a)));
            return;
        case Op::Unit: ret(p, UNIT); return;

        case Op::Local: {
            auto* fo = static_cast<FrameObj*>(as_obj(frame));
            Value v = fo->slots()[n.a];
            if (v == NIL_SLOT) {
                do_raise(p, type_error(p, "binding used before it was bound"));
                return;
            }
            enter(p, v);
            return;
        }
        case Op::Capture: {
            auto* fo = static_cast<FrameObj*>(as_obj(frame));
            auto* cl = static_cast<ClosureObj*>(as_obj(fo->closure));
            enter(p, cl->caps()[n.a]);
            return;
        }
        case Op::Global: {
            const GlobalRec& g = img.global(n.a);
            if (g.kind == GLOBAL_FUNCTION) {
                const FuncRec& f = img.func(g.target);
                // A parameterless impure binding is an action, not a value:
                // `let go! = { .. }` is performed by naming it, and is not
                // memoized, so each mention runs it again. That is the same
                // rule the entry point `main!` follows.
                if (f.arity == 0 && (f.flags & FN_IMPURE) && !(f.flags & FN_GLOBAL_VALUE)) {
                    Value cl = global_value(p, n.a);
                    Value fr = p.heap().make_frame(cl, f.slots);
                    eval_node(p, f.body, fr);
                    return;
                }
            }
            enter(p, global_value(p, n.a));
            return;
        }
        case Op::Builtin: ret(p, make_builtin(n.a)); return;

        case Op::Field: {
            // `mod.member`, where `mod` is a module named at the top level, is
            // what every call of a `std` function looks like -- and a module
            // value is a per-process constant, so there is nothing here to
            // evaluate. Resolving it in place saves the pair of steps the
            // general path costs: one Eval to reach the module, one Return to
            // come back with it.
            Value obj;
            if (!module_operand(p, img, n.a, frame, &obj)) {
                // The node index rides along so the member lookup can be
                // cached against the site that asked for it.
                push_cont(p, ContKind::FieldOf, n.b, p.node, 0, UNIT);
                eval_node(p, n.a, frame);
                return;
            }
            resolve_field(p, obj, n.b, p.node);
            return;
        }

        case Op::Apply: {
            const uint32_t* kids = img.kids_at(n.b);
            // Almost every call names its callee -- a global, a builtin, a
            // parameter, a member of an imported module -- and naming one is
            // a read, not an evaluation. Reading it here goes straight to the
            // application; the general path would push a continuation, spend a
            // reduction evaluating the name, and come back to do this anyway.
            Value callee;
            if (!callee_operand(p, img, n.a, frame, &callee)) {
                for (uint32_t i = 0; i < n.c; ++i) {
                    p.stack.push_back(thunk_for(p, kids[i], frame));
                }
                push_cont(p, ContKind::ApplyTo, n.c, 0, 0, UNIT);
                eval_node(p, n.a, frame);
                return;
            }
            if (callee == NIL_SLOT) {
                do_raise(p, type_error(p, "binding used before it was bound"));
                return;
            }
            // A saturated call to a function we already have: the arguments
            // are the frame's first slots, so build the frame and write them
            // there. The general path pushes each one onto the value stack for
            // `do_apply` to copy out again and erase -- three passes over the
            // arguments where a call needs one.
            Value fn = resolve(callee);
            if (is_obj(fn, ObjType::Closure)) {
                auto* cl = static_cast<ClosureObj*>(as_obj(fn));
                const FuncRec& f = img.func(cl->func);
                if (f.arity == n.c && n.c > 0) {
                    Value fr = p.heap().make_frame_filling(fn, f.slots, f.arity);
                    auto* fo = static_cast<FrameObj*>(as_obj(fr));
                    // Safe to hold `fo` across these: allocation never
                    // collects, which is the rule the whole machine rests on.
                    for (uint32_t i = 0; i < n.c; ++i) {
                        fo->slots()[i] = thunk_for(p, kids[i], frame);
                    }
                    enter_function(p, cl->func, f, fr);
                    return;
                }
            }
            // A saturated call to a native: its strict arguments are
            // evaluated where they stand rather than suspended and forced one
            // step later. See `fill_native_args`.
            if (is_builtin(fn) || is_obj(fn, ObjType::Native)) {
                const uint32_t arity = native_arity(fn);
                if (arity == n.c || arity == NATIVE_VARIADIC) {
                    fill_native_args(p, p.node, fn, uint32_t(p.stack.size()), 0, frame);
                    return;
                }
            }
            for (uint32_t i = 0; i < n.c; ++i) {
                p.stack.push_back(thunk_for(p, kids[i], frame));
            }
            do_apply(p, fn, n.c);
            return;
        }

        case Op::If: {
            Value cond;
            if (!operand_value(p, img, n.a, frame, &cond)) {
                push_cont(p, ContKind::IfBranch, n.b, n.c, 0, frame);
                eval_node(p, n.a, frame);
                return;
            }
            take_branch(p, cond, n.b, n.c, frame);
            return;
        }

        case Op::Block: advance_block(p, n.a, n.b, 0, frame); return;

        case Op::Bind: {
            auto* fo = static_cast<FrameObj*>(as_obj(frame));
            Value bound = thunk_for(p, n.b, frame);
            value_slot_store(&fo->slots()[n.a], bound);
            p.heap().remember_if_old(fo, bound);
            ret(p, UNIT);
            return;
        }

        case Op::MakeClosure:
        case Op::MakeThunk: {
            Value cl;
            if (!build_closure(p, n.a, frame, &cl)) {
                do_raise(p, cl);
                return;
            }
            ret(p, cl);
            return;
        }

        case Op::Try:
            push_cont(p, ContKind::Catch, n.b, n.c, uint32_t(p.stack.size()), frame);
            eval_node(p, n.a, frame);
            return;

        case Op::Force: eval_node(p, n.a, frame); return;

        case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod:
        case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge:
        case Op::Eq: case Op::Ne: {
            Value lhs;
            if (!operand_value(p, img, n.a, frame, &lhs)) {
                push_cont(p, ContKind::BinRight, n.op, n.b, 0, frame);
                eval_node(p, n.a, frame);
                return;
            }
            Value rhs;
            if (!operand_value(p, img, n.b, frame, &rhs)) {
                push_cont(p, ContKind::BinFinish, n.op, 0, 0, lhs);
                eval_node(p, n.b, frame);
                return;
            }
            finish_binary(p, Op(n.op), lhs, rhs);
            return;
        }

        case Op::And: case Op::Or: {
            Value lhs;
            if (!operand_value(p, img, n.a, frame, &lhs)) {
                push_cont(p, ContKind::LogicRight, n.op, n.b, 0, frame);
                eval_node(p, n.a, frame);
                return;
            }
            finish_logic(p, Op(n.op), lhs, n.b, frame);
            return;
        }

        case Op::Neg: case Op::Not: {
            Value v;
            if (!operand_value(p, img, n.a, frame, &v)) {
                push_cont(p, ContKind::UnaryFinish, n.op, 0, 0, UNIT);
                eval_node(p, n.a, frame);
                return;
            }
            finish_unary(p, Op(n.op), v);
            return;
        }

        case Op::MakeList: {
            const uint32_t* kids = img.kids_at(n.a);
            Value list = NIL;
            for (uint32_t i = n.b; i > 0; --i) {
                list = p.heap().make_cons(thunk_for(p, kids[i - 1], frame), list);
            }
            ret(p, list);
            return;
        }
        case Op::MakeArray: {
            const uint32_t* kids = img.kids_at(n.a);
            Value arr = p.heap().make_array(n.b);
            for (uint32_t i = 0; i < n.b; ++i) {
                Value item = thunk_for(p, kids[i], frame);
                static_cast<ArrayObj*>(as_obj(arr))->items()[i] = item;
            }
            ret(p, arr);
            return;
        }
        case Op::MakeMap: {
            Value m = p.heap().make_map(0);
            p.stack.push_back(m);
            advance_map(p, n.a, n.b, 0, frame);
            return;
        }

        // The container and then the key, each forced where everything else
        // is: by the machine, with a continuation, and not underneath a native.
        // Neither is usually an expression, though -- `xs.[0]` is a binding and
        // a literal -- so both are read directly when they can be.
        case Op::Get: case Op::Set: {
            Value container;
            if (!operand_value(p, img, n.a, frame, &container)) {
                push_cont(p, ContKind::IndexKey, 0, p.node, 0, frame);
                eval_node(p, n.a, frame);
                return;
            }
            Value key;
            if (!operand_value(p, img, n.b, frame, &key)) {
                // The container waits on the value stack, where the collector
                // can see it, while the key is forced.
                p.stack.push_back(container);
                push_cont(p, ContKind::IndexApply, 0, p.node, 0, frame);
                eval_node(p, n.b, frame);
                return;
            }
            if (Op(n.op) == Op::Get) container_get(p, p.node, container, key, frame);
            else container_set(p, p.node, container, key, frame);
            return;
        }

        case Op::Nop: ret(p, UNIT); return;
        default:
            do_raise(p, type_error(p, "unimplemented opcode"));
            return;
    }
}

/// Hand `p.result` to the top continuation.
///
/// `floor` is the depth this loop belongs to: `run_process` owns the whole
/// stack and passes zero, and a nested force (`force_whnf`) owns only what it
/// pushed and passes the depth it found. It matters because of the thunk
/// updates below, which are the one continuation this runs several of in a
/// row -- and running one that belongs to the machine *outside* would be
/// finishing someone else's work under them.
void step_return(Process& p, size_t floor) {
  // Thunk updates are handled here rather than by returning to the machine for
  // each one. Forcing a value nested n deep leaves n of them stacked, and each
  // is a single store -- so what the machine would be dispatching between is
  // one write and the next.
  for (;;) {
    if (p.conts.size() <= floor) {
        if (p.conts.empty()) p.mode = Mode::Halted;
        return;
    }
    Cont c = p.conts.back();
    p.conts.pop_back();

    if (c.kind == ContKind::UpdateThunk) {
        // Overwrite the thunk in place so every sharer sees the result.
        Obj* o = as_obj(c.v1);
        // A thunk that has survived into old space now gains a young target,
        // so the write barrier notes the edge for the next minor collection.
        p.heap().remember_if_old(o, p.result);
        // The target goes in before the type flips, so the flip -- which is
        // what a concurrent mark's helper reads first -- cannot be seen with
        // the target still unset. The flip store is the one that releases.
        std::atomic_ref<Value>(static_cast<IndirectObj*>(o)->target)
            .store(p.result, std::memory_order_relaxed);
        obj_type_store_release(o, ObjType::Indirect);
        continue;  // the value flows on to the next continuation
    }

    switch (c.kind) {
        case ContKind::Halt:
            p.mode = Mode::Halted;
            return;

        case ContKind::UpdateThunk:
            p.unreachable("thunk update is handled above");

        case ContKind::ApplyTo:
            do_apply(p, p.result, c.a);
            return;

        case ContKind::IfBranch:
            take_branch(p, p.result, c.a, c.b, c.v1);
            return;

        case ContKind::BinRight: {
            // The right operand is often a constant or a bound local, in which
            // case the operator finishes here rather than after another round
            // trip through the machine.
            Value lhs = p.result;
            Value rhs;
            if (!operand_value(p, img_of(p), c.b, c.v1, &rhs)) {
                push_cont(p, ContKind::BinFinish, c.a, 0, 0, lhs);
                eval_node(p, c.b, c.v1);
                return;
            }
            finish_binary(p, Op(c.a), lhs, rhs);
            return;
        }

        case ContKind::BinFinish: {
            Op op = Op(c.a);
            // Held across the call: comparing forces, and `force_whnf` leaves
            // its own value in `p.result` on the way out.
            Value lhs = c.v1;
            Value rhs = p.result;
            // `b == 1` marks a retry that was spliced in below a suspended
            // force (see below). That work finishes first and overwrites
            // `p.result` on its way out, so both operands travelled here in a
            // cell of their own instead.
            if (c.b == 1) {
                auto* pair = static_cast<ConsObj*>(as_obj(c.v1));
                lhs = pair->head;
                rhs = pair->tail;
            }
            finish_binary(p, op, lhs, rhs);
            return;
        }

        case ContKind::LogicRight:
            finish_logic(p, Op(c.a), p.result, c.b, c.v1);
            return;

        case ContKind::UnaryFinish:
            finish_unary(p, Op(c.a), p.result);
            return;

        case ContKind::BlockNext:
            advance_block(p, c.a, c.b, c.c, c.v1);
            return;

        case ContKind::Catch:
            // The protected body finished normally; the handler is not needed.
            ret(p, p.result);
            return;

        case ContKind::FieldOf:
            resolve_field(p, p.result, c.a, c.b);
            return;

        case ContKind::NativeArg:
            p.stack[c.a + c.c] = p.result;
            resume_native(p, c.v1, c.a, c.b, c.c + 1);
            return;

        case ContKind::NativeArgs: {
            // The argument just evaluated joins the ones already at the base,
            // and the rest carry on from there.
            p.stack.push_back(p.result);
            const Image& img = img_of(p);
            Value callee;
            if (!callee_operand(p, img, img.node(c.a).a, c.v1, &callee)) {
                p.unreachable("the callee of a native call stopped being a name");
            }
            fill_native_args(p, c.a, resolve(callee), c.b, c.c + 1, c.v1);
            return;
        }

        case ContKind::NativeRetry:
            resume_native(p, c.v1, c.a, c.b, 0);
            return;

        case ContKind::MapEntry: {
            Value key = p.result;
            Value valth = thunk_for(p, img_of(p).kid(c.a + c.c * 2 + 1), c.v1);
            p.stack.back() = map_insert(p, resolve(p.stack.back()), key, valth);
            advance_map(p, c.a, c.b, c.c + 1, c.v1);
            return;
        }

        case ContKind::IndexKey: {
            Value container = p.result;
            Value key;
            const Node& idx = img_of(p).node(c.b);
            if (!operand_value(p, img_of(p), idx.b, c.v1, &key)) {
                // The container waits on the value stack, where the collector
                // can see it, while the key is forced.
                p.stack.push_back(container);
                push_cont(p, ContKind::IndexApply, 0, c.b, 0, c.v1);
                eval_node(p, idx.b, c.v1);
                return;
            }
            if (Op(idx.op) == Op::Get) container_get(p, c.b, container, key, c.v1);
            else container_set(p, c.b, container, key, c.v1);
            return;
        }

        case ContKind::IndexApply: {
            Value container = p.stack.back();
            p.stack.pop_back();
            if (Op(img_of(p).node(c.b).op) == Op::Get) {
                container_get(p, c.b, container, p.result, c.v1);
            } else {
                container_set(p, c.b, container, p.result, c.v1);
            }
            return;
        }

        case ContKind::GetWalk:
            list_get(p, c.b, p.result, c.a, c.c, c.v1);
            return;

        case ContKind::SetWalk:
            list_set(p, c.b, p.result, c.a, c.c, c.v1);
            return;
    }
    return;
  }
}

/// Unwind to the nearest catch at or above `floor`. Returns false when the
/// error escapes it.
bool unwind(Process& p, size_t floor) {
    while (p.conts.size() > floor) {
        Cont c = p.conts.back();
        p.conts.pop_back();

        if (c.kind == ContKind::UpdateThunk) {
            // The thunk did not produce a value. Put it back the way it was so
            // a later `try!` can attempt it again and see the same error,
            // rather than finding a blackhole and reporting a false loop.
            Obj* o = as_obj(c.v1);
            if (obj_type(o) == ObjType::Blackhole) obj_type_store(o, ObjType::Thunk);
            continue;
        }
        if (c.kind == ContKind::Catch) {
            p.stack.resize(c.c);
            auto* fo = static_cast<FrameObj*>(as_obj(c.v1));
            value_slot_store(&fo->slots()[c.b], p.result);
            p.heap().remember_if_old(fo, p.result);
            eval_node(p, c.a, c.v1);
            return true;
        }
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

/// `DREAM_PROBE_THUNK`: count the suspensions a run makes, by the kind of
/// expression suspended, and report them with `--stats`.
///
/// `--stats` says how much of a program's allocation is thunks; this says what
/// they are *of*, which is the question that decides what to do about it. On a
/// self-compile it is what showed that two node kinds -- `apply` and `get` --
/// were 90% of every thunk made, and so that an in-range array read and an
/// already-built list prefix were worth handling in `thunk_for` while the rest
/// waits for a strictness analysis. One predictable branch per suspension, and
/// only on the path that was about to allocate anyway.

void dbg_dump_thunks() {
    if (!g_probe_thunk) return;
    for (size_t i = 0; i < 64; ++i) {
        uint64_t c = g_thunk_counts[i].load();
        if (c > 50000)
            std::fprintf(stderr, "; thunked %-14s %llu\n", op_name(Op(i)),
                         (unsigned long long)c);
    }
}

Value thunk_for(Process& p, uint32_t node, Value frame) {
    const Image& img = img_of(p);
    const Node& n = img.node(node);
    // Wrapping something already in normal form in a thunk would cost an
    // allocation and, for a variable, would break sharing with the binding.
    switch (Op(n.op)) {
        case Op::ConstInt: return make_integer(p, img.integer(n.a));
        case Op::ConstStr: return literal_string(p, n.a);
        case Op::ConstFloat: return literal_float(p, n.a);
        case Op::ConstChar: return make_char(uint32_t(n.a));
        case Op::ConstBool: return make_bool(n.a != 0);
        case Op::ConstAtom: return make_atom(p.runtime().image_atom(n.a));
        case Op::Unit: return UNIT;
        case Op::Builtin: return make_builtin(n.a);
        case Op::Local: {
            Value v = static_cast<FrameObj*>(as_obj(frame))->slots()[n.a];
            if (v != NIL_SLOT) return v;
            break;
        }
        case Op::Capture: {
            auto* fo = static_cast<FrameObj*>(as_obj(frame));
            return static_cast<ClosureObj*>(as_obj(fo->closure))->caps()[n.a];
        }
        case Op::MakeClosure:
        case Op::MakeThunk: {
            // A closure is already a value. Wrapping it in a thunk would mean
            // `spawn! $( .. )` hands the child a suspension that evaluates to
            // the work rather than the work itself.
            Value cl;
            if (build_closure(p, n.a, frame, &cl)) return cl;
            break;
        }
        case Op::Add:
        case Op::Sub:
        case Op::Mul: {
            // `f (n - 1)`: one arithmetic step on two numbers already in hand.
            //
            // Suspending it costs a Thunk now and, when the callee looks at it,
            // a Blackhole write, an Indirect write, a continuation and a
            // reduction -- to save an add. On a self-compile thunks are 31% of
            // everything allocated and frames another 34%, and on `fib` the two
            // of them are the entire program, so this is the cheapest shape of
            // the biggest cost there is.
            //
            // It is not a strictness analysis and needs none, because nothing
            // here can be observed. `operand_value` reads an operand only when
            // it is *already* in weak head normal form -- it never forces, so
            // no evaluation is brought forward. Two fixnums added, subtracted
            // or multiplied cannot raise, cannot diverge and cannot depend on
            // anything; overflow and every other shape fall through to the
            // thunk. So the only difference is that an argument the callee
            // never looks at has been computed, which is one machine
            // instruction and no change in meaning.
            //
            // Division is deliberately absent: `x / 0` raises, and a raise the
            // program would not otherwise have reached is exactly the
            // observable difference this must not make.
            Value a, b;
            if (operand_value(p, img, n.a, frame, &a) && is_fixnum(a) &&
                operand_value(p, img, n.b, frame, &b) && is_fixnum(b)) {
                const int64_t x = fixnum_value(a), y = fixnum_value(b);
                int64_t r;
                bool over = true;
                switch (Op(n.op)) {
                    case Op::Add: over = __builtin_add_overflow(x, y, &r); break;
                    case Op::Sub: over = __builtin_sub_overflow(x, y, &r); break;
                    default: over = __builtin_mul_overflow(x, y, &r); break;
                }
                if (!over && fixnum_fits(r)) return make_fixnum(r);
            }
            break;
        }
        case Op::Get: {
            // `a.[i]` on an array already in hand, at an index inside it, is a
            // load. Suspending one is the second most common thunk a compile
            // makes -- 2.6M of them -- because a record here is an array and
            // reading a field of one is how everything downstream of the lexer
            // talks about a token.
            //
            // Safe for the same reason the arithmetic above is: `operand_value`
            // reads an operand only when it is *already* in normal form, so
            // nothing is forced early, and an in-range read of an array cannot
            // raise. Out of range falls through to the thunk, which is what
            // knows about `else d` and about raising.
            //
            // Only an array. A *list* index walks the spine, and walking forces
            // it -- on a lazy or infinite list that is a different program, not
            // a cheaper one. A map lookup hashes and compares the key, and for
            // a compound key that forces too. Neither belongs here.
            //
            // What comes back is the element as it is stored, thunk and all,
            // exactly as `Op::Local` hands back a slot: it is the same value
            // the suspension would have produced, and whoever forces it updates
            // the array's own copy, so sharing improves rather than suffers.
            Value c, k;
            if (!operand_value(p, img, n.a, frame, &c) ||
                !operand_value(p, img, n.b, frame, &k) || !is_fixnum(k)) {
                break;
            }
            const int64_t i = fixnum_value(k);
            if (i < 0) break;
            if (is_obj(c, ObjType::Array)) {
                auto* a = static_cast<ArrayObj*>(as_obj(c));
                if (i < int64_t(a->len)) {
                    Value v = a->items()[i];
                    if (v != NIL_SLOT) return v;
                }
                break;
            }
            // A list, walked only as far as it has already been built.
            //
            // Indexing a list is a walk, and a walk normally *forces* -- which
            // is why this cannot simply do what the array case does: on a lazy
            // or infinite list, forcing the spine early is a different program.
            // So the walk follows only cells that are already in normal form
            // and gives up the moment it would have to force one, which makes
            // it a pointer chase and nothing more. `list.nth` is
            // `xs.[n else ()]` and is how the compiler reads every record that
            // is still a list, so most of these are index 0, 1 or 2 into a
            // spine that was built several stages ago.
            //
            // The depth is capped because this runs whether or not the callee
            // ever looks at the argument. A short walk is cheaper than the
            // Thunk it replaces; a long one would be work done on spec.
            if (is_obj(c, ObjType::Cons) && i <= 8) {
                Value cur = c;
                for (int64_t step = 0; step < i; ++step) {
                    Value tail = resolve(static_cast<ConsObj*>(as_obj(cur))->tail);
                    if (!is_obj(tail, ObjType::Cons)) { cur = UNIT; break; }
                    cur = tail;
                }
                if (is_obj(cur, ObjType::Cons)) {
                    Value v = static_cast<ConsObj*>(as_obj(cur))->head;
                    if (v != NIL_SLOT) return v;
                }
            }
            break;
        }
        default: break;
    }
    if (g_probe_thunk) g_thunk_counts[size_t(n.op) & 63].fetch_add(1, std::memory_order_relaxed);
    return p.heap().make_thunk(node, frame);
}

Value global_value(Process& p, uint32_t index) {
    if (p.globals.empty()) p.globals.assign(img_of(p).global_count(), NIL_SLOT);
    if (p.globals[index] != NIL_SLOT) return p.globals[index];

    const Image& img = img_of(p);
    const GlobalRec& g = img.global(index);
    Value v;
    if (g.kind == GLOBAL_MODULE) {
        const ImportRec& ir = img.import(g.target);
        StringRef alias = img.str(ir.alias);
        v = p.heap().make_module(g.target, p.heap().make_string(alias.data, alias.len));
    } else {
        const FuncRec& f = img.func(g.target);
        Value cl = p.heap().make_closure(g.target, 0);
        if (f.arity == 0 && (f.flags & FN_GLOBAL_VALUE)) {
            // A pure top-level value: force once, then memoize. The thunk is
            // the memo, so every later reference sees the computed result.
            Value fr = p.heap().make_frame(cl, f.slots);
            v = p.heap().make_thunk(f.body, fr);
        } else {
            v = cl;
        }
    }
    p.globals[index] = v;
    return v;
}

void prime_apply(Process& p, Value callee, uint32_t argc) {
    p.conts.clear();
    push_cont(p, ContKind::Halt, 0, 0, 0, UNIT);
    push_cont(p, ContKind::ApplyTo, argc, 0, 0, UNIT);
    // `enter` returns it immediately when it is already a function, and forces
    // it first when it is not, so both spawn shapes reach the same place.
    enter(p, callee);
}

void prime_force(Process& p, Value v) {
    p.conts.clear();
    push_cont(p, ContKind::Halt, 0, 0, 0, UNIT);
    enter(p, v);
}

/// The limits that keep one runaway process from taking the runtime with it.
///
/// A process is the unit of failure here, so exhausting memory has to kill the
/// process that did it and nothing else. Without these, `std::bad_alloc`
/// escapes the allocator and calls `terminate`, which loses every other
/// process, the scheduler, and any work already done.
///
/// Each is generous enough that ordinary programs never approach it -- a tail
/// call pops its continuation, so a loop runs in constant space, and
/// `sum_to 100000` needs 100k frames against a limit of four million -- and
/// each can be moved for a program that genuinely needs more.
struct Limits {
    size_t conts;
    size_t stack;
    size_t heap_bytes;

    static const Limits& get() {
        static Limits l = [] {
            auto from_env = [](const char* name, size_t fallback) {
                if (const char* env = std::getenv(name)) {
                    long long n = std::atoll(env);
                    if (n > 0) return size_t(n);
                }
                return fallback;
            };
            return Limits{
                from_env("DREAM_MAX_DEPTH", size_t(4u) << 20),      // ~96 MB of continuations
                from_env("DREAM_MAX_STACK", size_t(4u) << 20),      // ~32 MB of values
                from_env("DREAM_MAX_HEAP", size_t(1u) << 30),       // 1 GB per process
            };
        }();
        return l;
    }
};

/// The raises behind the checks below, kept here rather than in the loops so
/// the per-reduction path stays a couple of compares and the error-string
/// construction stays in `.cold` clones.
void raise_depth_limit(Process& p, const WellKnownAtoms& wk, size_t depth) {
    do_raise(p, raise_error(p, wk.stack_overflow,
                            "recursion too deep: " + std::to_string(depth) + " pending frames"));
}
void raise_stack_limit(Process& p, const WellKnownAtoms& wk, size_t depth) {
    do_raise(p, raise_error(p, wk.stack_overflow,
                            "value stack too deep: " + std::to_string(depth) + " entries"));
}
void raise_heap_limit(Process& p, const WellKnownAtoms& wk, size_t max_heap) {
    do_raise(p, raise_error(p, wk.out_of_memory,
                            "process heap grew past " + std::to_string(max_heap) + " bytes"));
}

/// Raise in `p` if it has outgrown what one process may use. Returns true when
/// it did, so the caller can stop what it was doing.
///
/// Called at safepoints rather than from `push_cont` or `alloc`: here the
/// process is already in a consistent state, and safepoints are one reduction
/// apart, so a limit can only be overshot by a bounded amount. Making the
/// allocator itself fail would mean every caller of `alloc` -- most of which
/// hold raw object pointers -- had to cope with a null.
///
/// The limits and well-known atoms are passed in because this runs once a
/// reduction: reading them here would pay the thread-safe static initializer
/// guard and a handful of loads on every step of every loop. The callers hoist
/// them into local registers instead, which is what keeps this cheap enough to
/// run unconditionally.
static inline bool check_limits(Process& p, const WellKnownAtoms& wk,
                                size_t max_conts, size_t max_stack, size_t max_heap) {
    if (p.mode == Mode::Raise) return false;  // already unwinding; let it finish
    if (p.conts.size() > max_conts) {
        raise_depth_limit(p, wk, p.conts.size());
        return true;
    }
    if (p.stack.size() > max_stack) {
        raise_stack_limit(p, wk, p.stack.size());
        return true;
    }
    // Checked after a collection has had its chance, so this fires only for a
    // process whose *live* data is too big, not one that merely allocates fast.
    if (p.heap().bytes_allocated() > max_heap && p.heap().bytes_live() > max_heap / 2) {
        raise_heap_limit(p, wk, max_heap);
        return true;
    }
    return false;
}

/// The function whose frame is current, or `UINT32_MAX` when there is none --
/// at the very start of a process, or inside a native.
/// Charge one step's *own* allocation to `fi`.
///
/// The bytes a step allocates are the heap's total before and after -- but a
/// step that enters a native which forces runs whole machine loops underneath
/// itself, and each of those has already charged its own bytes. Subtract what
/// they claimed, and claim the rest. Without this a self-compile's allocation
/// profile totalled 36 GB against a heap that handed out 1.5 GB.
inline void note_own_alloc(Process& p, uint32_t fi, uint64_t a0, uint64_t n0) {
    const uint64_t delta = p.heap().bytes_total() - a0;
    const uint64_t nested = p.alloc_attributed - n0;
    const uint64_t own = delta > nested ? delta - nested : 0;
    if (own) {
        p.runtime().note_alloc(fi, own);
        p.alloc_attributed += own;
    }
}

uint32_t current_func(Process& p) {
    if (!is_ptr(p.frame)) return UINT32_MAX;
    Value c = static_cast<FrameObj*>(as_obj(p.frame))->closure;
    if (!is_obj(c, ObjType::Closure)) return UINT32_MAX;
    return static_cast<ClosureObj*>(as_obj(c))->func;
}

/// `step_return` with its allocation charged to whoever is returning.
///
/// A Return step is not a reduction -- it spends no budget, and the profile's
/// first table is right not to count it -- but it does allocate: it is where a
/// thunk's result is published and where the list builder conses. Leaving it
/// out put a third of a compile's bytes in no function at all.
inline void step_return_counted(Process& p, size_t floor) {
    const uint32_t fi = current_func(p);
    const uint64_t a0 = p.heap().bytes_total();
    const uint64_t n0 = p.alloc_attributed;
    step_return(p, floor);
    note_own_alloc(p, fi, a0, n0);
}

void run_process(Process& p, int64_t budget) {
    p.reductions = budget;
    p.slice = budget;
    p.slice_spent = false;
    // The limits are loop-invariant over the whole run; read them once so the
    // per-reduction check stays a couple of compares against registers, not a
    // thread-safe static init guarded call.
    const Limits lim = Limits::get();
    const size_t max_conts = lim.conts;
    const size_t max_stack = lim.stack;
    const size_t max_heap = lim.heap_bytes;
    const WellKnownAtoms& wk = well_known(p.runtime());
    // Profiling never turns on mid-run, so this is a register for the whole
    // loop rather than a load per step. That matters most for the Return run
    // below, which is more than half of all the steps a program takes.
    const bool prof = p.runtime().profiling();
    // `slice_spent` is how a nested force says the budget ran out while it was
    // running: it re-armed the counter so that what reads it keeps working,
    // and this is where the slice it could not end actually ends.
    while (p.reductions > 0 && !p.slice_spent) {
        // Safepoint. Every live value is reachable from the process's stacks,
        // its frame and its result -- nothing is stranded in a C++ local.
        if (p.heap().should_collect()) p.maybe_collect();

        // A raised limit unwinds at the next dispatch, below.
        if (check_limits(p, wk, max_conts, max_stack, max_heap)) continue;

        switch (p.mode) {
            case Mode::Eval:
                --p.reductions;
                ++p.total_reductions;
                // Attributed to whichever function's frame is current, which
                // is what makes a profile read like the source: a reduction
                // belongs to the code that asked for it.
                if (prof) {
                    // The frame is read before the step, because the step is
                    // what changes it, and the bytes after, because they are
                    // what the step allocated.
                    const uint32_t fi = current_func(p);
                    const uint64_t a0 = p.heap().bytes_total();
                    const uint64_t n0 = p.alloc_attributed;
                    p.runtime().note_reduction(fi);
                    step_eval(p);
                    note_own_alloc(p, fi, a0, n0);
                } else {
                    step_eval(p);
                }
                break;
            case Mode::Return:
                if (prof) step_return_counted(p, 0);
                else step_return(p, 0);
                break;
            case Mode::Raise:
                if (!unwind(p, 0)) {
                    p.failed = true;
                    p.exit_value = p.result;
                    p.mode = Mode::Halted;
                    return;
                }
                break;
            case Mode::Halted:
                p.exit_value = p.result;
                return;
        }

        // Hand the value that came back to whoever was waiting for it, and
        // keep doing so while there is nothing else to do.
        //
        // A Return step is bookkeeping, not work: it pops the continuation
        // stack it walks, so a run of them is bounded by the depth already
        // there and cannot spin. Leaving the safepoint check out of that run
        // costs at most a slightly late collection -- a safepoint is a place
        // collection *may* happen, not one where it must -- and saves the
        // check on more than half of all the steps a program takes.
        if (prof) {
            while (p.mode == Mode::Return && !p.park_requested) step_return_counted(p, 0);
        } else {
            while (p.mode == Mode::Return && !p.park_requested) step_return(p, 0);
        }

        // A blocking builtin asked to be parked; stop here and let the
        // scheduler complete the handshake.
        if (p.park_requested) return;
    }
}

bool force_whnf(Process& p, Value v, Value* out) {
    ForceNest nest(p);
    v = resolve(v);
    if (is_whnf(v)) {
        *out = v;
        return true;
    }
    // Run a nested machine loop down to the current continuation depth.
    //
    // Whether it may collect is decided here and nowhere else. The caller
    // either vouched for its own locals or did not (`VouchesForGc`); a force
    // nobody vouched for raises `force_pins` for as long as it runs, and a
    // collection needs every frame in the chain to have vouched, which is
    // exactly `force_pins == 0`. The flag is consumed rather than inherited:
    // what this force goes on to reach starts unvouched, because a vouch is a
    // claim about the frame that made it.
    const bool vouched = p.force_vouched;
    p.force_vouched = false;
    if (!vouched) ++p.force_pins;

    const Limits lim = Limits::get();
    const size_t max_conts = lim.conts;
    const size_t max_stack = lim.stack;
    const size_t max_heap = lim.heap_bytes;
    const WellKnownAtoms& wk = well_known(p.runtime());
    const bool prof = p.runtime().profiling();
    const size_t floor = p.conts.size();
    const Mode saved_mode = p.mode;
    const uint32_t saved_node = p.node;

    // The machine state this loop is about to overwrite. It is two live
    // references, and a collection inside the loop would move both -- so they
    // are pinned rather than kept in a local, and read back below. Cheap, and
    // only on the path that actually runs a loop: a value already in WHNF
    // returned above without touching any of this.
    Pin saved_frame(p, p.frame);
    Pin saved_result(p, p.result);

    enter(p, v);
    bool ok = true;
    bool blocked = false;
    for (;;) {
        if (p.mode == Mode::Return && p.conts.size() <= floor) break;
        if (p.mode == Mode::Halted) break;
        if (p.mode == Mode::Raise) {
            if (!unwind(p, floor)) { ok = false; break; }
            continue;
        }
        if (p.mode == Mode::Eval) {
            --p.reductions;
            ++p.total_reductions;
            // Attributed like any other reduction. Leaving this out made the
            // profiler blind to most of a compile: `dreams` forces at every
            // stage boundary, so the machine spends the bulk of its life in
            // *this* loop rather than the outer one -- 179M of a self-compile's
            // 214M reductions -- and a profile that cannot see them names the
            // wrong functions.
            if (prof) {
                const uint32_t fi = current_func(p);
                const uint64_t a0 = p.heap().bytes_total();
                const uint64_t n0 = p.alloc_attributed;
                p.runtime().note_reduction(fi);
                step_eval(p);
                note_own_alloc(p, fi, a0, n0);
            } else {
                step_eval(p);
            }
        } else if (prof) {
            step_return_counted(p, floor);
        } else {
            step_return(p, floor);
        }

        // The outer loop's limit check, and -- when the whole chain of frames
        // below `run_process` has vouched -- its safepoint too. Without the
        // limit check a runaway recursion reached through a native
        // (`console.print!` forcing a value that recurses for ever) grows until
        // the allocator throws, because this loop is where that forcing
        // happens. Without the safepoint a native that walks a long lazy
        // structure allocates for as long as the walk takes with nothing able
        // to collect any of it, which is the same failure a size away.
        if (check_limits(p, wk, max_conts, max_stack, max_heap)) continue;
        if (p.force_pins == 0 && p.heap().should_collect()) p.maybe_collect();
        // The slice ran out somewhere this loop cannot stop. See
        // `Process::slice_spent`: re-arm it, and let `run_process` end the
        // slice when this force finally returns.
        if (p.reductions <= 0) {
            p.slice_spent = true;
            p.reductions = p.slice;
        }

        // A blocking native inside the value being forced -- `join!`, `recv!`,
        // a read on a socket -- asked to be parked. This loop cannot park: it
        // is running underneath a native, whose C++ frame has to return before
        // the scheduler can touch the process. Carrying on would re-enter that
        // native for ever, which is a livelock, not a hang: the loop spins
        // without spending a reduction, so it never even yields its slice.
        //
        // So abandon the force and let the *outer* machine park instead. The
        // native above us returns Block, and is called again once the thing it
        // was waiting for has arrived.
        if (p.park_requested) {
            blocked = true;
            ok = false;
            break;
        }
    }

    // Both counters back as they were. The vouch is restored rather than left
    // consumed because a native that forces in a loop vouches once: taking it
    // away after the first force would quietly pin the rest of the walk, which
    // is the bug this whole mechanism exists to remove.
    if (!vouched) --p.force_pins;
    p.force_vouched = vouched;

    if (blocked) {
        // The force is *suspended*, not abandoned. Everything it pushed -- its
        // continuations, its half-forced blackholed thunks, its stack -- stays
        // exactly as it is, and the process resumes into it when the scheduler
        // wakes it. `force_resume_at` records where the outer operation's
        // retry belongs: underneath all of it.
        //
        // This used to unwind instead: un-blackhole the thunks, drop the
        // continuations, and let the outer native re-force from scratch. That
        // is only sound while forcing is pure, and it is not. A computation
        // that parks may already have performed effects, and re-forcing
        // performs them again -- `strict! { print! "x"; join! p }` printed
        // twice. Worse, `exec!` records its pending child in a single slot on
        // the process, so a re-force made the *first* call take the result the
        // *second* had started and begin another; that one never terminated.
        //
        // The machine state is left as the park set it (Return, on the
        // blocking native's own retry), so nothing below restores it.
        p.force_blocked = true;
        p.force_resume_at = floor;
        *out = p.result;
        return false;
    }

    *out = p.result;
    Value error = p.result;
    p.mode = saved_mode;
    p.node = saved_node;
    p.frame = saved_frame.get();
    if (ok) {
        p.result = saved_result.get();
    } else {
        p.result = error;
    }
    return ok;
}

bool force_deep(Process& p, Value v, Value* out) {
    Value head;
    if (!force_whnf(p, v, &head)) { *out = p.result; return false; }
    if (!is_ptr(head)) { *out = head; return true; }
    // Walked already, by this call or by an earlier one. See AUX_DEEP_FORCED:
    // this is what keeps a deep force linear in the data rather than in the
    // number of paths through it.
    if (as_obj(head)->aux & AUX_DEEP_FORCED) { *out = head; return true; }

    const size_t base = p.stack.size();
    auto unwind = [&](Value* out_) {
        if (!p.force_blocked) p.stack.resize(base);
        *out_ = p.result;
        return false;
    };

    switch (as_obj(head)->type) {
        case ObjType::Cons: {
            // A list is a chain and is walked as one. Recursing on the tail
            // would cost one C++ frame per element, and a list long enough to
            // be worth forcing -- the bytes of an image, say -- runs out of
            // stack long before it runs out of list.
            //
            // Every cell is kept on the value stack, both so the collector can
            // see them while the heads are being forced and so they can be
            // marked afterwards: a cell is deeply forced only once everything
            // after it is, so the marking runs backwards, from the end.
            p.stack.push_back(head);
            // The cell being walked waits on the value stack so that a
            // collection can find it and rewrite the reference -- which means
            // no C++ pointer to it may outlive a force. The object moves, and
            // a pointer taken before the force still names the block it moved
            // out of. So the cell is re-read from the stack at every single
            // use rather than once per turn round the loop, and this is the
            // only way to touch it.
            //
            // Storing through a stale one does not fault where it happens,
            // which is what made this expensive to find. It writes into
            // whatever that block has since been recycled into, and since
            // `ConsObj::head` and `ThunkObj::node` share an offset, the shape
            // the damage usually takes is a thunk whose node is the low half
            // of a pointer. The machine jumps to it some thousands of
            // reductions later and dies a long way from here.
            auto cell = [&] { return static_cast<ConsObj*>(as_obj(p.stack.back())); };
            for (;;) {
                Value tmp;
                if (!force_deep(p, cell()->head, &tmp)) {
                    return unwind(out);
                }
                value_slot_store(&cell()->head, tmp);
                p.heap().remember_if_old(cell(), tmp);

                Value tail;
                if (!force_whnf(p, cell()->tail, &tail)) {
                    return unwind(out);
                }
                value_slot_store(&cell()->tail, tail);
                p.heap().remember_if_old(cell(), tail);

                const bool more = is_ptr(tail) && as_obj(tail)->type == ObjType::Cons
                                  && !(as_obj(tail)->aux & AUX_DEEP_FORCED);
                if (more) {
                    p.stack.push_back(tail);
                    continue;
                }
                // What ends the chain is not always `[]`: a tail may hold any
                // value, and one that is not a cell still has to be forced.
                if (!is_ptr(tail) || as_obj(tail)->type != ObjType::Cons) {
                    Value done;
                    if (!force_deep(p, tail, &done)) return unwind(out);
                    auto* last = static_cast<ConsObj*>(as_obj(p.stack.back()));
                    value_slot_store(&last->tail, done);
                    p.heap().remember_if_old(last, done);
                }
                break;
            }
            // Nothing below allocates, so nothing moves: the cell at the
            // bottom of this run is still the one that was pushed.
            Value first = p.stack[base];
            while (p.stack.size() > base) {
                as_obj(p.stack.back())->aux |= AUX_DEEP_FORCED;
                p.stack.pop_back();
            }
            *out = first;
            return true;
        }
        case ObjType::Array: {
            p.stack.push_back(head);
            // Re-read on both sides of the force, for the reason the list
            // above is: the array is on the value stack precisely so that a
            // collection may move it, and a pointer that spans the force is a
            // pointer to where it used to be. The length is safe to keep --
            // it is a number, and an array's does not change.
            auto arr = [&] { return static_cast<ArrayObj*>(as_obj(p.stack.back())); };
            const uint32_t len = arr()->len;
            for (uint32_t i = 0; i < len; ++i) {
                Value tmp;
                if (!force_deep(p, arr()->items()[i], &tmp)) return unwind(out);
                value_slot_store(&arr()->items()[i], tmp);
                p.heap().remember_if_old(arr(), tmp);
            }
            as_obj(p.stack.back())->aux |= AUX_DEEP_FORCED;
            *out = p.stack.back();
            p.stack.pop_back();
            return true;
        }
        case ObjType::Map: {
            p.stack.push_back(head);
            // The leaves are collected once and then forced in place. A leaf
            // may be shared with another version of the map, which is fine:
            // forcing a thunk yields the same value to everyone holding it.
            //
            // The values wait on the value stack rather than in a C++ vector,
            // for the reason every list above is walked there: forcing one of
            // them can collect, and a collection rewrites what it can see. A
            // vector of `Value` is not something it can see, so the entries
            // after the one being forced would be husks of objects that had
            // moved. That was safe only while a deep force could not collect,
            // and `strict!` vouching (see `bi_strict`) is exactly what makes
            // it collect.
            std::vector<std::pair<Value, Value>> entries;
            map_collect(p.stack.back(), entries);
            const size_t first = p.stack.size();
            for (auto& [k, v] : entries) {
                (void)k;
                p.stack.push_back(v);
            }
            const size_t count = entries.size();
            entries.clear();
            for (size_t i = 0; i < count; ++i) {
                Value tmp;
                if (!force_deep(p, p.stack[first + i], &tmp)) return unwind(out);
            }
            p.stack.resize(first);
            as_obj(p.stack.back())->aux |= AUX_DEEP_FORCED;
            *out = p.stack.back();
            p.stack.pop_back();
            return true;
        }
        default:
            *out = head;
            return true;
    }
}

bool jit_arith(Process& p, Op op, Value a, Value b, Value* out) {
    return arith(p, op, a, b, out);
}
bool jit_compare(Process& p, Op op, Value a, Value b, Value* out) {
    return compare(p, op, a, b, out);
}

Value jit_literal_string(Process& p, uint32_t index) { return literal_string(p, index); }

bool jit_build_closure(Process& p, uint32_t func_index, Value frame, Value* out) {
    return build_closure(p, func_index, frame, out);
}

// ---------------------------------------------------------------------------
// Containers, for a caller with no continuation
//
// The two below are `container_get` and `container_set` said as functions. Every
// decision in them is the same one, taken in the same order and with the same
// error, because a disagreement between the tiers about what `.[ ]` means would
// be a disagreement about a thing programs do constantly. What differs is only
// how the answer leaves: the machine `enter`s the element it found and lets the
// next reduction force it, and these force it here.
//
// Both are called with the heap pinned -- `PinsTheHeap` in the caller -- so a
// value may wait in a C++ local across a force. The list walks still put their
// working set where the collector can see it, because the cost is a push and a
// pop and the alternative is a rule that has to be remembered.
// ---------------------------------------------------------------------------

namespace {

/// The element `container_get` would have entered, forced. 1 with the value, 0
/// with the error.
int jit_deliver(Process& p, Value v, Value* out) {
    Value w;
    if (!force_whnf(p, v, &w)) {
        *out = p.result;
        return 0;
    }
    *out = w;
    return 1;
}

}  // namespace

int jit_container_get(Process& p, Value container, Value key, bool has_else, Value* out) {
    container = resolve(container);
    key = resolve(key);
    // Nothing at the key. With an `else` the caller answers it; without one the
    // error is this container's to name.
    auto missing = [&](uint32_t kind, const std::string& why) {
        if (has_else) return 2;
        *out = raise_error(p, kind, why);
        return 0;
    };

    if (is_obj(container, ObjType::Map)) {
        Value found;
        if (map_lookup(p, container, key, &found)) return jit_deliver(p, found, out);
        return missing(well_known(p.runtime()).no_such_key,
                       "the map has no key " + describe(p, key));
    }
    if (!is_sequence(container)) {
        *out = type_error(p, "`.[ ]` reads a map, an array or a list, not " +
                                 describe(p, container));
        return 0;
    }
    if (!is_fixnum(key)) {
        *out = type_error(p, "a position is an integer, not " + describe(p, key));
        return 0;
    }
    const int64_t k = fixnum_value(key);
    if (is_obj(container, ObjType::Array)) {
        auto* a = static_cast<ArrayObj*>(as_obj(container));
        if (k >= 0 && k < int64_t(a->len)) return jit_deliver(p, a->items()[k], out);
        return missing(out_of_bounds_atom(p), "index " + std::to_string(k) +
                                                  " is outside an array of " +
                                                  std::to_string(a->len));
    }
    if (k < 0 || k > int64_t(0xFFFFFFFFu)) {
        return missing(out_of_bounds_atom(p),
                       "index " + std::to_string(k) + " is not a position in a list");
    }
    // The walk. Where `list_get` pushes a `GetWalk` continuation for a tail that
    // is not in normal form, this forces it and carries on round the loop -- so
    // the depth is one nested force, not one per cell.
    Pin cur(p, container);
    uint32_t remaining = uint32_t(k);
    for (;;) {
        Value c = resolve(cur.get());
        if (!is_obj(c, ObjType::Cons)) {
            return missing(out_of_bounds_atom(p),
                           "index " + std::to_string(k) + " is past the end of a list of " +
                               std::to_string(uint32_t(k) - remaining));
        }
        if (remaining == 0) return jit_deliver(p, static_cast<ConsObj*>(as_obj(c))->head, out);
        --remaining;
        Value tail;
        if (!force_whnf(p, static_cast<ConsObj*>(as_obj(c))->tail, &tail)) {
            *out = p.result;
            return 0;
        }
        cur.set(tail);
    }
}

int jit_container_set(Process& p, Value container, Value key, Value value, Value* out) {
    container = resolve(container);
    key = resolve(key);
    if (is_obj(container, ObjType::Map)) {
        *out = map_insert(p, container, key, value);
        return 1;
    }
    if (!is_sequence(container)) {
        *out = type_error(p, "`.[ => ]` changes a map, an array or a list, not " +
                                 describe(p, container));
        return 0;
    }
    if (!is_fixnum(key)) {
        *out = type_error(p, "a position is an integer, not " + describe(p, key));
        return 0;
    }
    const int64_t k = fixnum_value(key);
    if (is_obj(container, ObjType::Array)) {
        const uint32_t n = static_cast<ArrayObj*>(as_obj(container))->len;
        if (k < 0 || k >= int64_t(n)) {
            *out = raise_error(p, out_of_bounds_atom(p),
                               "index " + std::to_string(k) + " is outside an array of " +
                                   std::to_string(n));
            return 0;
        }
        // A copy, for the reason `container_set` gives: the array given is a
        // value and someone else may hold it.
        Value arr = p.heap().make_array(n);
        auto* src = static_cast<ArrayObj*>(as_obj(container));
        auto* dst = static_cast<ArrayObj*>(as_obj(arr));
        for (uint32_t i = 0; i < n; ++i) {
            Value item = uint32_t(k) == i ? value : src->items()[i];
            dst->items()[i] = item;
            p.heap().remember_if_old(dst, item);
        }
        *out = arr;
        return 1;
    }
    if (k < 0 || k > int64_t(0xFFFFFFFFu)) {
        *out = raise_error(p, out_of_bounds_atom(p),
                           "index " + std::to_string(k) + " is not a position in a list");
        return 0;
    }
    // The cells walked past wait on the value stack, exactly as `list_set`
    // leaves them: the new spine is built back out of them once the position is
    // reached, and everything behind it is shared.
    const size_t base = p.stack.size();
    Pin cur(p, container);
    Pin val(p, value);
    uint32_t remaining = uint32_t(k);
    for (;;) {
        Value c = resolve(cur.get());
        if (!is_obj(c, ObjType::Cons)) {
            const uint32_t walked = uint32_t(k) - remaining;
            p.stack.resize(base);
            *out = raise_error(p, out_of_bounds_atom(p),
                               "index " + std::to_string(k) + " is past the end of a list of " +
                                   std::to_string(walked));
            return 0;
        }
        if (remaining == 0) {
            Value built = p.heap().make_cons(val.get(), static_cast<ConsObj*>(as_obj(c))->tail);
            while (p.stack.size() > base) {
                Value head = static_cast<ConsObj*>(as_obj(p.stack.back()))->head;
                built = p.heap().make_cons(head, built);
                p.stack.pop_back();
            }
            *out = built;
            return 1;
        }
        p.stack.push_back(c);
        --remaining;
        Value tail;
        if (!force_whnf(p, static_cast<ConsObj*>(as_obj(c))->tail, &tail)) {
            p.stack.resize(base);
            *out = p.result;
            return 0;
        }
        cur.set(tail);
    }
}

std::string describe(Process& p, Value v) {
    v = resolve(v);
    if (is_fixnum(v)) return "integer " + std::to_string(fixnum_value(v));
    if (is_imm(v)) {
        switch (imm_kind(v)) {
            case IMM_UNIT: return "unit";
            case IMM_BOOL: return truthy(v) ? "bool true" : "bool false";
            case IMM_CHAR: return "a char";
            case IMM_ATOM: return ":" + p.runtime().atom_name(uint32_t(imm_payload(v)));
            case IMM_NIL: return "the empty list";
            case IMM_BUILTIN: return "a builtin function";
            default: return "a value";
        }
    }
    if (!is_ptr(v)) return "a value";
    return std::string("a ") + obj_type_name(as_obj(v)->type);
}

}  // namespace dream
