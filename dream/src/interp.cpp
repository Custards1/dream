#include "interp.hpp"

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "builtins.hpp"
#include "jit.hpp"

namespace dream {

namespace {

inline const Image& img_of(Process& p) { return p.runtime().image(); }

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
    Value fa, fb;
    if (!force_whnf(p, a, &fa) || !force_whnf(p, b, &fb)) {
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
    if (oa->type != ob->type) return false;

    switch (oa->type) {
        case ObjType::Str: {
            auto* x = static_cast<StrObj*>(oa);
            auto* y = static_cast<StrObj*>(ob);
            return x->len == y->len && std::memcmp(x->data(), y->data(), x->len) == 0;
        }
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
            for (uint32_t i = 0; i < x->cap; ++i) {
                Value k = x->entries()[i * 2];
                if (k == NIL_SLOT) continue;
                Value found;
                if (!map_lookup(p, fb, k, &found)) return false;
                if (!values_equal(p, x->entries()[i * 2 + 1], found, raised, depth + 1)) return false;
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

/// Enter a function body, taking the compiled tier when one is available.
void enter_function(Process& p, uint32_t func_index, const FuncRec& f, Value frame) {
    Jit* jit = p.runtime().jit();
    if (jit) {
        if (CompiledFn fn = jit->on_enter(func_index)) {
            int status = 0;
            // Compiled code spends the same budget the interpreter does, so
            // fold what it used into the process's total.
            const int64_t before = p.reductions;
            Value r = fn(&p, frame, &status);
            p.total_reductions += uint64_t(before - p.reductions);
            if (status == 0) { ret(p, r); return; }
            if (status == 1) { do_raise(p, r); return; }
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

        Value fr = p.heap().make_frame(callee, f.slots);
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
        uint32_t arity, mask;
        if (is_builtin(callee)) {
            const BuiltinDef& bd = builtin_def(uint32_t(imm_payload(callee)));
            arity = bd.arity;
            mask = bd.strict_mask;
        } else {
            auto* nat = static_cast<NativeObj*>(as_obj(callee));
            arity = nat->arity;
            mask = nat->strict_mask;
        }
        (void)mask;
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
            // so move them out of the way first.
            std::vector<Value> extra(p.stack.begin() + long(base + arity), p.stack.end());
            p.stack.resize(base + arity);
            resume_native(p, callee, uint32_t(base), arity, 0);
            // Re-push the leftovers underneath the pending ApplyTo.
            p.stack.insert(p.stack.end(), extra.begin(), extra.end());
            return;
        }
        resume_native(p, callee, uint32_t(base), arity, 0);
        return;
    }

    //p.stack.resize(base);
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
            as_obj(thunk)->type = ObjType::Blackhole;
            push_cont(p, ContKind::UpdateThunk, 0, 0, 0, thunk);
            eval_node(p, t->node, t->frame);
            return;
        }
    }

    NativeResult r = fn(p, callee, p.stack.data() + base, argc);
    switch (r.outcome) {
        case NativeOutcome::Value:
            p.stack.resize(base);
            ret(p, r.value);
            break;
        case NativeOutcome::Raise:
            p.stack.resize(base);
            do_raise(p, r.value);
            break;
        case NativeOutcome::Block:
            // The native parked the process. Leave the arguments in place and
            // arrange to call it again when the scheduler wakes us.
            push_cont(p, ContKind::NativeRetry, base, argc, 0, callee);
            p.mode = Mode::Return;
            p.result = UNIT;
            break;
    }
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
        as_obj(v)->type = ObjType::Blackhole;
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
            fo->slots()[sn.a] = bound;
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
    const size_t base = p.stack.size();
    p.stack.push_back(b);
    p.stack.push_back(a);
    for (;;) {
        Value w;
        if (!force_whnf(p, p.stack[base + 1], &w)) {
            *out = p.result;
            p.stack.resize(base);
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
    } else if (is_obj(a, ObjType::Str) && is_obj(b, ObjType::Str)) {
        auto* x = static_cast<StrObj*>(as_obj(a));
        auto* y = static_cast<StrObj*>(as_obj(b));
        uint32_t n = x->len < y->len ? x->len : y->len;
        cmp = std::memcmp(x->data(), y->data(), n);
        if (cmp == 0) cmp = x->len < y->len ? -1 : (x->len > y->len ? 1 : 0);
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

void resolve_field(Process& p, Value obj, uint32_t name_index) {
    const Image& img = img_of(p);
    StringRef member = img.str(name_index);

    if (!is_obj(obj, ObjType::Module)) {
        do_raise(p, type_error(p, describe(p, obj) + " has no member `" + member.str() + "`"));
        return;
    }
    auto* mod = static_cast<ModuleObj*>(as_obj(obj));
    const ImportRec& ir = img.import(mod->import_index);
    std::string path = img.str(ir.path).str();

    const ModuleDef* def = p.runtime().find_module(path);
    if (!def) {
        do_raise(p, raise_error(p, well_known(p.runtime()).no_such_member,
                                "module `" + path + "` is not provided by this runtime"));
        return;
    }
    const NativeDef* m = def->find(member);
    if (!m) {
        do_raise(p, raise_error(p, well_known(p.runtime()).no_such_member,
                                "module `" + path + "` has no member `" + member.str() + "`"));
        return;
    }
    Value name = p.heap().make_string(member.data, member.len);
    ret(p, make_native(p, m->fn, name, m->arity, m->strict_mask, m->user));
}

// ---------------------------------------------------------------------------
// One step
// ---------------------------------------------------------------------------

void step_eval(Process& p) {
    const Image& img = img_of(p);
    const Node& n = img.node(p.node);
    Value frame = p.frame;

    switch (Op(n.op)) {
        case Op::ConstInt: ret(p, make_integer(p, img.integer(n.a))); return;
        case Op::ConstFloat: ret(p, p.heap().make_float(img.real(n.a))); return;
        case Op::ConstStr: {
            StringRef s = img.str(n.a);
            ret(p, p.heap().make_string(s.data, s.len));
            return;
        }
        case Op::ConstChar: ret(p, make_char(uint32_t(n.a))); return;
        case Op::ConstBool: ret(p, make_bool(n.a != 0)); return;
        case Op::ConstAtom: {
            // Image atom indices are remapped onto the runtime's global table,
            // so atoms from different modules with the same name compare equal.
            ret(p, make_atom(p.runtime().intern_atom(img.atom_name(n.a).str())));
            return;
        }
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

        case Op::Field:
            push_cont(p, ContKind::FieldOf, n.b, 0, 0, UNIT);
            eval_node(p, n.a, frame);
            return;

        case Op::Apply: {
            const uint32_t* kids = img.kids_at(n.b);
            for (uint32_t i = 0; i < n.c; ++i) {
                p.stack.push_back(thunk_for(p, kids[i], frame));
            }
            push_cont(p, ContKind::ApplyTo, n.c, 0, 0, UNIT);
            eval_node(p, n.a, frame);
            return;
        }

        case Op::If:
            push_cont(p, ContKind::IfBranch, n.b, n.c, 0, frame);
            eval_node(p, n.a, frame);
            return;

        case Op::Block: advance_block(p, n.a, n.b, 0, frame); return;

        case Op::Bind: {
            auto* fo = static_cast<FrameObj*>(as_obj(frame));
            fo->slots()[n.a] = thunk_for(p, n.b, frame);
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
        case Op::Eq: case Op::Ne:
            push_cont(p, ContKind::BinRight, n.op, n.b, 0, frame);
            eval_node(p, n.a, frame);
            return;

        case Op::And: case Op::Or:
            push_cont(p, ContKind::LogicRight, n.op, n.b, 0, frame);
            eval_node(p, n.a, frame);
            return;

        case Op::Neg: case Op::Not:
            push_cont(p, ContKind::UnaryFinish, n.op, 0, 0, UNIT);
            eval_node(p, n.a, frame);
            return;

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
            uint32_t cap = 8;
            while (cap < n.b * 2) cap *= 2;
            p.stack.push_back(p.heap().make_map(cap));
            advance_map(p, n.a, n.b, 0, frame);
            return;
        }

        case Op::Nop: ret(p, UNIT); return;
        default:
            do_raise(p, type_error(p, "unimplemented opcode"));
            return;
    }
}

void step_return(Process& p) {
    if (p.conts.empty()) {
        p.mode = Mode::Halted;
        return;
    }
    Cont c = p.conts.back();
    p.conts.pop_back();

    switch (c.kind) {
        case ContKind::Halt:
            p.mode = Mode::Halted;
            return;

        case ContKind::UpdateThunk: {
            // Overwrite the thunk in place so every sharer sees the result.
            Obj* o = as_obj(c.v1);
            o->type = ObjType::Indirect;
            static_cast<IndirectObj*>(o)->target = p.result;
            return;  // stay in Return: the value flows to the next continuation
        }

        case ContKind::ApplyTo:
            do_apply(p, p.result, c.a);
            return;

        case ContKind::IfBranch: {
            Value cond = p.result;
            if (!is_bool(cond)) {
                do_raise(p, type_error(p, "`if` needs a bool, got " + describe(p, cond)));
                return;
            }
            if (truthy(cond)) {
                eval_node(p, c.a, c.v1);
            } else if (c.b == NO_NODE) {
                ret(p, UNIT);
            } else {
                eval_node(p, c.b, c.v1);
            }
            return;
        }

        case ContKind::BinRight:
            push_cont(p, ContKind::BinFinish, c.a, 0, 0, p.result);
            eval_node(p, c.b, c.v1);
            return;

        case ContKind::BinFinish: {
            Op op = Op(c.a);
            Value out;
            bool ok = (op == Op::Eq || op == Op::Ne || op == Op::Lt || op == Op::Le ||
                       op == Op::Gt || op == Op::Ge)
                          ? compare(p, op, c.v1, p.result, &out)
                          : arith(p, op, c.v1, p.result, &out);
            if (ok) ret(p, out); else do_raise(p, out);
            return;
        }

        case ContKind::LogicRight: {
            Value lhs = p.result;
            if (!is_bool(lhs)) {
                do_raise(p, type_error(p, std::string("`") + op_name(Op(c.a)) +
                                              "` needs a bool, got " + describe(p, lhs)));
                return;
            }
            // Short-circuit: the right operand is never even entered when the
            // left already decides the answer.
            if (Op(c.a) == Op::And && !truthy(lhs)) { ret(p, FALSE_V); return; }
            if (Op(c.a) == Op::Or && truthy(lhs)) { ret(p, TRUE_V); return; }
            eval_node(p, c.b, c.v1);
            return;
        }

        case ContKind::UnaryFinish: {
            Value v = p.result;
            if (Op(c.a) == Op::Neg) {
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
            return;
        }

        case ContKind::BlockNext:
            advance_block(p, c.a, c.b, c.c, c.v1);
            return;

        case ContKind::Catch:
            // The protected body finished normally; the handler is not needed.
            ret(p, p.result);
            return;

        case ContKind::FieldOf:
            resolve_field(p, p.result, c.a);
            return;

        case ContKind::NativeArg:
            p.stack[c.a + c.c] = p.result;
            resume_native(p, c.v1, c.a, c.b, c.c + 1);
            return;

        case ContKind::NativeRetry:
            resume_native(p, c.v1, c.a, c.b, 0);
            return;

        case ContKind::MapEntry: {
            Value key = p.result;
            Value valth = thunk_for(p, img_of(p).kid(c.a + c.c * 2 + 1), c.v1);
            p.stack.back() = resolve(p.stack.back());
            map_insert(p, p.stack.back(), key, valth);
            advance_map(p, c.a, c.b, c.c + 1, c.v1);
            return;
        }
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
            if (o->type == ObjType::Blackhole) o->type = ObjType::Thunk;
            continue;
        }
        if (c.kind == ContKind::Catch) {
            p.stack.resize(c.c);
            auto* fo = static_cast<FrameObj*>(as_obj(c.v1));
            fo->slots()[c.b] = p.result;
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

Value thunk_for(Process& p, uint32_t node, Value frame) {
    const Image& img = img_of(p);
    const Node& n = img.node(node);
    // Wrapping something already in normal form in a thunk would cost an
    // allocation and, for a variable, would break sharing with the binding.
    switch (Op(n.op)) {
        case Op::ConstInt: return make_integer(p, img.integer(n.a));
        case Op::ConstChar: return make_char(uint32_t(n.a));
        case Op::ConstBool: return make_bool(n.a != 0);
        case Op::ConstAtom: return make_atom(p.runtime().intern_atom(img.atom_name(n.a).str()));
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
        default: break;
    }
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

void run_process(Process& p, int64_t budget) {
    p.reductions = budget;
    while (p.reductions > 0) {
        // Safepoint. Every live value is reachable from the process's stacks,
        // its frame and its result -- nothing is stranded in a C++ local.
        if (p.heap().should_collect()) p.maybe_collect();

        switch (p.mode) {
            case Mode::Eval:
                --p.reductions;
                ++p.total_reductions;
                step_eval(p);
                break;
            case Mode::Return:
                step_return(p);
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

        // A blocking builtin asked to be parked; stop here and let the
        // scheduler complete the handshake.
        if (p.park_requested) return;
    }
}

bool force_whnf(Process& p, Value v, Value* out) {
    v = resolve(v);
    if (is_whnf(v)) {
        *out = v;
        return true;
    }
    // Run a nested machine loop down to the current continuation depth. The
    // process's own stacks still hold every root, so a collection during this
    // is as safe as one at the outer level.
    const size_t floor = p.conts.size();
    const Mode saved_mode = p.mode;
    const uint32_t saved_node = p.node;
    const Value saved_frame = p.frame;
    const Value saved_result = p.result;

    enter(p, v);
    bool ok = true;
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
            step_eval(p);
        } else {
            step_return(p);
        }
    }

    *out = p.result;
    Value error = p.result;
    p.mode = saved_mode;
    p.node = saved_node;
    p.frame = saved_frame;
    if (ok) {
        p.result = saved_result;
    } else {
        p.result = error;
    }
    return ok;
}

bool force_deep(Process& p, Value v, Value* out) {
    Value head;
    if (!force_whnf(p, v, &head)) { *out = p.result; return false; }
    if (!is_ptr(head)) { *out = head; return true; }

    switch (as_obj(head)->type) {
        case ObjType::Cons: {
            // Keep the cell on the value stack: forcing the tail can collect,
            // and a raw C++ pointer would not survive it.
            p.stack.push_back(head);
            Value tmp;
            auto* c = static_cast<ConsObj*>(as_obj(p.stack.back()));
            if (!force_deep(p, c->head, &tmp)) { p.stack.pop_back(); *out = p.result; return false; }
            static_cast<ConsObj*>(as_obj(p.stack.back()))->head = tmp;
            c = static_cast<ConsObj*>(as_obj(p.stack.back()));
            if (!force_deep(p, c->tail, &tmp)) { p.stack.pop_back(); *out = p.result; return false; }
            static_cast<ConsObj*>(as_obj(p.stack.back()))->tail = tmp;
            *out = p.stack.back();
            p.stack.pop_back();
            return true;
        }
        case ObjType::Array: {
            p.stack.push_back(head);
            uint32_t len = static_cast<ArrayObj*>(as_obj(head))->len;
            for (uint32_t i = 0; i < len; ++i) {
                Value item = static_cast<ArrayObj*>(as_obj(p.stack.back()))->items()[i];
                Value tmp;
                if (!force_deep(p, item, &tmp)) { p.stack.pop_back(); *out = p.result; return false; }
                static_cast<ArrayObj*>(as_obj(p.stack.back()))->items()[i] = tmp;
            }
            *out = p.stack.back();
            p.stack.pop_back();
            return true;
        }
        case ObjType::Map: {
            p.stack.push_back(head);
            uint32_t cap = static_cast<MapObj*>(as_obj(head))->cap;
            for (uint32_t i = 0; i < cap; ++i) {
                if (static_cast<MapObj*>(as_obj(p.stack.back()))->entries()[i * 2] == NIL_SLOT) continue;
                Value item = static_cast<MapObj*>(as_obj(p.stack.back()))->entries()[i * 2 + 1];
                Value tmp;
                if (!force_deep(p, item, &tmp)) { p.stack.pop_back(); *out = p.result; return false; }
                static_cast<MapObj*>(as_obj(p.stack.back()))->entries()[i * 2 + 1] = tmp;
            }
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
