// The LLVM JIT tier.
//
// Scope, and why it is drawn where it is: this compiles the strict numeric
// spine of a function -- arithmetic, comparisons, branches, and self recursion
// -- and leaves everything else to the interpreter. The limit is not laziness
// in general but a soundness requirement. Compiled code evaluates a self call's
// arguments eagerly, and doing that to an argument the callee would never have
// forced can raise an error in a program that was going to terminate quietly.
// So a function is only compiled when a strictness analysis proves every
// parameter is forced on every path. Anything else stays interpreted, where
// laziness is explicit and free.
//
// Self recursion comes in two shapes and they are compiled differently. A tail
// call is a loop back-edge: the parameters are overwritten and control jumps to
// the top, which is what lets a compiled loop run in constant space and yield
// to the scheduler at every iteration. A *non-tail* call -- `fib (n - 1) +
// fib (n - 2)` -- is a machine call, which means the body has to be a function
// of its arguments rather than of a heap frame. So such a function is emitted
// twice over: an inner body taking its parameters by value, and an outer entry
// matching `CompiledFn` that unpacks the frame once and calls it. The frame the
// interpreter would have allocated per call disappears entirely, which on `fib`
// is the whole program -- 100% of what it allocates is frames.
//
// What that costs is written down in `kStackBudget`: a machine call uses
// machine stack, and machine stack is a fixed resource where a heap
// continuation is not. Compiled recursion therefore carries its depth and
// gives up -- `JIT_DEEP` -- rather than overflow, and the interpreter, whose
// recursion is on the heap and bounded by `DREAM_MAX_DEPTH`, takes the call
// over. That is a fallback, not an error: the program sees what it would have
// seen with no JIT at all.
//
// The fast paths are inline; every slow path calls the interpreter's own
// helper, so the two tiers cannot drift apart on what an operation means.

#include "jit.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

#include "image.hpp"
#include "jit_rt.hpp"
#include "process.hpp"
#include "runtime.hpp"

namespace dream {

namespace {

/// Status codes a compiled body returns through its out-parameter. The same
/// four as `JitStatus` in jit.hpp, spelled again here because the emitter needs
/// them as constants it can put in the IR.
constexpr int JIT_OK = 0;
constexpr int JIT_RAISED = 1;
/// The reduction budget ran out mid-loop. The loop-carried values have been
/// written back to the frame, so the interpreter can pick the iteration up
/// from the top of the body -- an OSR exit that keeps JIT code preemptible.
constexpr int JIT_YIELD = 2;
/// Compiled self recursion ran out of machine stack. Nothing has been written
/// anywhere and no effect has happened -- the compiled body is arithmetic and
/// nothing else -- so the caller may simply run the call interpreted instead.
constexpr int JIT_DEEP = 3;
/// An entry guard did not hold: a parameter the body carries as an unboxed
/// double arrived as something that is not a float in hand. Like `JIT_DEEP`,
/// nothing has happened yet and the interpreter can simply run the call --
/// but unlike it, the function is not given up on, because this is a fact
/// about one call rather than about the function.
constexpr int JIT_BAIL = 4;

/// How much machine stack one chain of compiled self calls may use.
///
/// The interpreter's recursion is heap continuations and its limit is
/// `DREAM_MAX_DEPTH` -- four million of them. A compiled non-tail call is a
/// machine call, and a machine stack is typically eight megabytes for the
/// whole thread, shared with every interpreter loop and native above and below
/// this one. So compiled recursion gets a budget rather than the limit, and
/// hands the call back when it is spent. A megabyte is deep enough that no
/// ordinary program reaches it -- `fib 32` goes 32 deep -- and shallow enough
/// that several nested chains still fit.
constexpr uint32_t kStackBudget = 1u << 20;
/// What one compiled frame is assumed to cost, on top of eight bytes per slot,
/// for turning that budget into a count of calls.
///
/// It is a guess, because how much stack LLVM gives a function is decided after
/// this code is written and there is no cheap way to ask. So it is a guess made
/// in the direction where being wrong is harmless: too high and the recursion
/// hands itself back to the interpreter earlier than it had to, which costs a
/// little speed on a recursion nobody writes; too low and it overflows the
/// machine stack, which is a crash. `fib` recurses 32 deep and an ordinary
/// tree walk tens; a function that needs thousands is one the interpreter
/// should have anyway.
constexpr uint32_t kFrameOverhead = 128;

// ---------------------------------------------------------------------------
// Representation
//
// A compiled function carries its values in one of two shapes: a tagged
// `Value`, which is what every other part of the runtime speaks, and a raw
// `double`, which is what a float *is* once nobody has to be able to point at
// it. The second exists because a boxed float is an allocation, and a loop that
// allocates one per operation is a loop the collector runs rather than the CPU:
// `pi` in the benchmark allocated 866 MB to add two million terms, 44% of it
// float boxes, and spent its time in the nursery instead of the FPU.
//
// So the emitter decides, before it writes a line, which values are floats, and
// keeps those in registers. Boxing happens at exactly four places -- the value
// returned to the interpreter, the spill a yield makes into the heap frame, an
// operand handed to a slow-path helper, and a join where the two arms disagree
// -- and nowhere in between. A fused `pi` loop reaches all four zero times per
// iteration.
//
// `Ty` is what makes that safe. `Float` is a claim about the *value*, not about
// the expression: it means "if this reduces without raising, what it reduces to
// is a float". That is exactly what `arith` guarantees -- an operator with a
// float on one side of it answers a float or raises, and has no third case --
// which is why `Unknown + Float` is `Float` even though the left side could
// turn out to be a list. If it is a list, the operation raises, and a raise is
// a return, not a value of the wrong shape.
//
// A parameter is the one place the claim cannot be proved, because its value
// comes from the caller. So it is *guarded* instead: a slot the loop carries as
// a double is checked on entry, and a function entered with something else in
// it answers `JIT_BAIL` and is run by the interpreter, which is what would have
// happened had it never been compiled.
// ---------------------------------------------------------------------------

enum class Ty : uint8_t {
    /// Nothing is known yet: the fixpoint has not reached this, or the
    /// expression transfers control and produces no value at all.
    Unknown = 0,
    Float = 1,
    /// A tagged `Value` of any kind, floats included. The top of the lattice.
    Any = 2,
};

inline Ty join(Ty a, Ty b) {
    if (a == Ty::Unknown) return b;
    if (b == Ty::Unknown) return a;
    return a == b ? a : Ty::Any;
}

/// The type of `a op b` for `+ - * / %`.
///
/// A float on either side makes the answer a float, whatever the other side is
/// -- see the note above. Otherwise nothing is known until both sides are.
inline Ty arith_ty(Ty a, Ty b) {
    if (a == Ty::Float || b == Ty::Float) return Ty::Float;
    if (a == Ty::Unknown || b == Ty::Unknown) return Ty::Unknown;
    return Ty::Any;
}

/// The host natives compiled code knows how to make, rather than call.
///
/// Every one of them is a pure function of one number: no effect, no Dream
/// code underneath it, and nothing it can do that the machine cannot do in a
/// few instructions. That is the whole admission test, and it is a narrow one
/// on purpose -- a native that runs Dream work underneath itself would do so
/// with this frame's values in registers the collector cannot see.
///
/// They matter out of proportion to their size because a call to *any* of them
/// used to refuse the whole function: the tier compiles self calls and nothing
/// else, so a fused `pi` loop, whose body is arithmetic and one `to_float`, was
/// never compiled at all.
enum class KnownNative : uint8_t { None, ToFloat, ToInt, Sqrt, Abs, Floor };

/// Which native `Apply`'s callee names, when it names one of the above.
///
/// The callee has to be `Field(Global module, name)` -- a member of a module
/// named at the top level, which is what every `std` call looks like -- and the
/// module has to be one this runtime provides. `module_for_import` is what says
/// so, and it is the same lookup the interpreter does; a module of the same
/// member name from anywhere else resolves to a different `ModuleDef`, or to
/// none, and is not recognised.
KnownNative known_native(Runtime& rt, const Image& img, uint32_t callee_node, uint32_t argc) {
    const Node& c = img.node(callee_node);
    if (Op(c.op) != Op::Field) return KnownNative::None;
    const Node& m = img.node(c.a);
    if (Op(m.op) != Op::Global) return KnownNative::None;
    const GlobalRec& g = img.global(m.a);
    if (g.kind != GLOBAL_MODULE) return KnownNative::None;
    const ModuleDef* def = rt.module_for_import(g.target);
    if (!def || argc != 1) return KnownNative::None;
    StringRef name = img.str(c.b);
    // The host module answers to both names: `std.core` is Dream source now and
    // reaches the host through `std.native`, but images built before that move
    // -- the bootstrap seed among them -- still import it by the old one, and
    // the runtime registers it twice for exactly that reason.
    if (def->name == "std.native" || def->name == "std.core") {
        if (name.equals("to_float")) return KnownNative::ToFloat;
        if (name.equals("to_int")) return KnownNative::ToInt;
    } else if (def->name == "std.math") {
        if (name.equals("sqrt")) return KnownNative::Sqrt;
        if (name.equals("abs")) return KnownNative::Abs;
        if (name.equals("floor")) return KnownNative::Floor;
    }
    return KnownNative::None;
}

/// The bit of the strict mask a recognised native has. All of them force their
/// one argument, but the mask is what says so, and reading it is what keeps
/// this honest if one is ever admitted that does not.
uint32_t known_native_strict_mask(Runtime& rt, const Image& img, uint32_t callee_node) {
    const Node& c = img.node(callee_node);
    const GlobalRec& g = img.global(img.node(c.a).a);
    const ModuleDef* def = rt.module_for_import(g.target);
    if (!def) return 0;
    const NativeDef* nd = def->find(img.str(c.b));
    return nd ? nd->strict_mask : 0;
}

bool op_is_supported(Op op) {
    switch (op) {
        case Op::ConstInt: case Op::ConstFloat: case Op::ConstBool:
        case Op::ConstChar: case Op::ConstAtom: case Op::Unit:
        case Op::Local: case Op::Capture:
        case Op::If: case Op::Block: case Op::Force:
        case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod:
        case Op::Eq: case Op::Ne: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge:
        case Op::And: case Op::Or: case Op::Neg: case Op::Not:
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Analysis
// ---------------------------------------------------------------------------

using SlotSet = uint64_t;  // one bit per slot; functions with >64 slots are skipped

struct Analysis {
    bool compilable = false;
    SlotSet strict_params = 0;
    /// The body calls itself somewhere other than tail position, so it is
    /// emitted as a function of its arguments rather than of a frame. See the
    /// note at the top of this file.
    bool recurses = false;
    /// Slots the function carries as raw doubles: one bit per slot. Decided by
    /// the fixpoint in `Analyzer::float_slots`, guarded on entry, and boxed
    /// again at every boundary. See "Representation" above.
    SlotSet float_slots = 0;
    /// Slots to force on entry, in this order, before the float ones are
    /// unboxed. Empty unless `float_slots` is. See `Analyzer::force_order`.
    std::vector<uint32_t> force_first;
};

class Analyzer {
public:
    Analyzer(Runtime& rt, const Image& img, uint32_t func_index)
        : img_(img), fi_(func_index), f_(img.func(func_index)), rt_(rt) {}

    Analysis run() {
        Analysis a;
        if (f_.slots > 64 || f_.arity == 0) return a;
        if (!check(f_.body, 0)) return a;

        SlotSet strict = strict_of(f_.body, 0);
        SlotSet params = f_.arity >= 64 ? ~SlotSet(0) : ((SlotSet(1) << f_.arity) - 1);
        // Every parameter must be forced on every path, or compiling the
        // self tail call would evaluate something the interpreter never would.
        if ((strict & params) != params) return a;

        // A body that reads a slot no parameter owns has state the frame
        // holds and the argument list does not, so it cannot be emitted as a
        // function of its arguments. In practice nothing reaches this: the
        // only thing that writes such a slot is `Op::Bind`, and a block
        // containing one is refused above. It is checked rather than argued
        // because the argument is about the compiler, not about this file.
        if (recurses_ && reads_beyond_params_) return a;

        a.compilable = true;
        a.strict_params = strict;
        a.recurses = recurses_;
        a.float_slots = float_slots();
        if (a.float_slots) {
            if (!entry_forces(a.float_slots, &a.force_first)) {
                // The body forces its parameters in an order this cannot pin
                // down, so unboxing one would mean forcing it out of turn.
                // Give the specialization up rather than the order.
                a.float_slots = 0;
                a.force_first.clear();
            }
        }
        return a;
    }

private:
    /// Is this node, and everything under it, something we can emit?
    bool check(uint32_t node, int depth) {
        if (depth > 256) return false;
        const Node& n = img_.node(node);
        Op op = Op(n.op);

        if (op == Op::Apply) return check_apply(n, depth);
        if (!op_is_supported(op)) return false;
        if (op == Op::Local && n.a >= f_.arity) reads_beyond_params_ = true;

        switch (op) {
            case Op::If:
                return check(n.a, depth + 1) && check(n.b, depth + 1) &&
                       (n.c == NO_NODE || check(n.c, depth + 1));
            case Op::Block:
                for (uint32_t i = 0; i < n.b; ++i) {
                    uint32_t stmt = img_.kid(n.a + i);
                    // A `let` inside the body would need a thunk built against
                    // the frame, but the frame's slots live in registers here.
                    if (Op(img_.node(stmt).op) == Op::Bind) return false;
                    if (!check(stmt, depth + 1)) return false;
                }
                return true;
            case Op::Force: case Op::Neg: case Op::Not:
                return check(n.a, depth + 1);
            case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod:
            case Op::Eq: case Op::Ne: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge:
            case Op::And: case Op::Or:
                return check(n.a, depth + 1) && check(n.b, depth + 1);
            default:
                return true;
        }
    }

    /// This function calling itself with a full argument list -- the only
    /// Dream-level call the tier compiles. In tail position it becomes a loop
    /// back-edge; anywhere else a machine call, which is what `recurses_`
    /// records: the two need different shapes of function around them.
    bool is_self_call(const Node& n) const {
        if (Op(n.op) != Op::Apply || n.c != f_.arity) return false;
        const Node& callee = img_.node(n.a);
        if (Op(callee.op) != Op::Global) return false;
        const GlobalRec& g = img_.global(callee.a);
        return g.kind == GLOBAL_FUNCTION && g.target == fi_;
    }

    /// A call: this function calling itself, or one of the numeric natives
    /// compiled code makes rather than calls. Anything else refuses the whole
    /// function, because the tier has no way to enter an arbitrary callee.
    bool check_apply(const Node& n, int depth) {
        if (is_self_call(n)) {
            for (uint32_t i = 0; i < n.c; ++i) {
                if (!check(img_.kid(n.b + i), depth + 1)) return false;
            }
            if (!(n.flags & F_TAIL)) recurses_ = true;
            return true;
        }
        if (known_native(rt_, img_, n.a, n.c) == KnownNative::None) return false;
        for (uint32_t i = 0; i < n.c; ++i) {
            if (!check(img_.kid(n.b + i), depth + 1)) return false;
        }
        return true;
    }

    /// Slots definitely forced when this node is reduced to WHNF.
    SlotSet strict_of(uint32_t node, int depth) {
        if (depth > 256) return 0;
        const Node& n = img_.node(node);
        switch (Op(n.op)) {
            case Op::Local:
                return SlotSet(1) << n.a;
            case Op::If:
                // The condition always runs; a branch only counts when both
                // arms force it.
                return strict_of(n.a, depth + 1) |
                       (n.c == NO_NODE
                            ? 0
                            : (strict_of(n.b, depth + 1) & strict_of(n.c, depth + 1)));
            case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod:
            case Op::Eq: case Op::Ne: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge:
                return strict_of(n.a, depth + 1) | strict_of(n.b, depth + 1);
            case Op::And: case Op::Or:
                // Short-circuiting: only the left operand is certain.
                return strict_of(n.a, depth + 1);
            case Op::Force: case Op::Neg: case Op::Not:
                return strict_of(n.a, depth + 1);
            case Op::Block: {
                SlotSet s = 0;
                for (uint32_t i = 0; i < n.b; ++i) {
                    uint32_t stmt = img_.kid(n.a + i);
                    bool last = (i + 1 == n.b);
                    if (last || (img_.node(stmt).flags & F_STRICT)) {
                        s |= strict_of(stmt, depth + 1);
                    }
                }
                return s;
            }
            case Op::Apply: {
                // A self call forces whatever its argument expressions do,
                // because compiled code evaluates them all. A native forces
                // only the arguments its mask claims -- taking the union there
                // would be claiming a parameter is strict on the strength of an
                // argument nobody looks at, and this set is what licenses
                // compiling the call in the first place.
                SlotSet s = 0;
                const bool self = is_self_call(n);
                const uint32_t mask =
                    self ? ~uint32_t(0) : known_native_strict_mask(rt_, img_, n.a);
                for (uint32_t i = 0; i < n.c; ++i) {
                    if (!self && !((mask >> i) & 1)) continue;
                    s |= strict_of(img_.kid(n.b + i), depth + 1);
                }
                return s;
            }
            default:
                return 0;
        }
    }

    // --- which values are floats ------------------------------------------
    //
    // A fixpoint over the parameters, ascending the lattice in "Representation"
    // above. A slot's type is the join of what every self call puts in it; the
    // value it arrives with contributes nothing, because that one is guarded
    // rather than inferred. Each of the arity slots can rise at most twice
    // (Unknown to Float to Any), so the loop is bounded by that and terminates
    // on the round that changes nothing.

    /// One bit per slot the function can carry as a raw double.
    SlotSet float_slots() {
        slot_ty_.assign(f_.slots, Ty::Unknown);
        for (uint32_t i = f_.arity; i < f_.slots; ++i) slot_ty_[i] = Ty::Any;
        collect_calls(f_.body, 0);

        const int rounds = int(2 * f_.arity) + 2;
        for (int round = 0; round < rounds; ++round) {
            bool changed = false;
            for (uint32_t site : call_sites_) {
                const Node& n = img_.node(site);
                for (uint32_t i = 0; i < n.c && i < f_.arity; ++i) {
                    Ty t = join(slot_ty_[i], ty(img_.kid(n.b + i), 0));
                    if (t != slot_ty_[i]) {
                        slot_ty_[i] = t;
                        changed = true;
                    }
                }
            }
            if (!changed) break;
        }

        SlotSet bits = 0;
        for (uint32_t i = 0; i < f_.slots; ++i) {
            if (slot_ty_[i] == Ty::Float) bits |= SlotSet(1) << i;
        }
        return bits;
    }

    // --- what the body forces, and in what order ---------------------------
    //
    // A slot carried as a double has to be a double before the loop starts, and
    // the value the caller left in the frame is usually a thunk -- the argument
    // of a fused fold is `acc + f x`, which `thunk_for` suspends because it is
    // not already a value. So the entry has to force it, and forcing is the one
    // thing the tier has been careful never to do out of turn: a parameter that
    // raises must raise where the interpreter would have raised it, which is
    // wherever the body first looks at it.
    //
    // So the entry forces a *prefix* of the body's own order and no more. What
    // is below computes that order for as far as it is the same on every path;
    // where two branches disagree the sequence stops, and if a float slot is
    // not inside the part that is definite, the specialization is dropped
    // rather than the order. Slots after the prefix are left to `load_slot`,
    // which forces each on first read exactly as before.

    /// Append the slots `idx` definitely forces, in order. Answers false once
    /// the order stops being the same on every path, after which the caller
    /// must stop appending.
    bool force_order(uint32_t idx, int depth, std::vector<uint32_t>& out) {
        if (depth > 128) return false;
        const Node& n = img_.node(idx);
        switch (Op(n.op)) {
            case Op::Local:
                out.push_back(n.a);
                return true;
            case Op::Force: case Op::Neg: case Op::Not:
                return force_order(n.a, depth + 1, out);
            case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod:
            case Op::Eq: case Op::Ne: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge:
                return force_order(n.a, depth + 1, out) && force_order(n.b, depth + 1, out);
            case Op::And: case Op::Or:
                // Short-circuiting: whether the right side runs at all depends
                // on the left, so the order is only definite up to here.
                force_order(n.a, depth + 1, out);
                return false;
            case Op::If: {
                if (!force_order(n.a, depth + 1, out)) return false;
                if (n.c == NO_NODE) return false;
                std::vector<uint32_t> t, e;
                force_order(n.b, depth + 1, t);
                force_order(n.c, depth + 1, e);
                size_t i = 0;
                while (i < t.size() && i < e.size() && t[i] == e[i]) out.push_back(t[i++]);
                return false;
            }
            case Op::Block: {
                for (uint32_t i = 0; i < n.b; ++i) {
                    uint32_t stmt = img_.kid(n.a + i);
                    bool last = (i + 1 == n.b);
                    if (!last && !(img_.node(stmt).flags & F_STRICT)) continue;
                    if (!force_order(stmt, depth + 1, out)) return false;
                }
                return true;
            }
            case Op::Apply: {
                const bool self = is_self_call(n);
                const uint32_t mask =
                    self ? ~uint32_t(0) : known_native_strict_mask(rt_, img_, n.a);
                for (uint32_t i = 0; i < n.c; ++i) {
                    if (!((mask >> i) & 1)) continue;
                    if (!force_order(img_.kid(n.b + i), depth + 1, out)) return false;
                }
                // A tail call transfers control and a machine call comes back
                // with a value, not with a slot; either way nothing follows.
                return false;
            }
            default:
                return true;
        }
    }

    /// The slots to force on entry, in order, so that every float slot has been
    /// reached. False when one of them is not inside the definite part of the
    /// order, which is when the specialization has to be given up.
    bool entry_forces(SlotSet floats, std::vector<uint32_t>* out) {
        std::vector<uint32_t> seq;
        force_order(f_.body, 0, seq);

        SlotSet seen = 0;
        SlotSet want = floats;
        for (uint32_t slot : seq) {
            if (slot >= 64 || ((seen >> slot) & 1)) continue;
            seen |= SlotSet(1) << slot;
            out->push_back(slot);
            want &= ~(SlotSet(1) << slot);
            if (!want) return true;
        }
        return false;
    }

    void collect_calls(uint32_t idx, int depth) {
        if (depth > 256) return;
        const Node& n = img_.node(idx);
        if (Op(n.op) == Op::Apply) {
            if (is_self_call(n)) call_sites_.push_back(idx);
            for (uint32_t i = 0; i < n.c; ++i) collect_calls(img_.kid(n.b + i), depth + 1);
            return;
        }
        switch (Op(n.op)) {
            case Op::If:
                collect_calls(n.a, depth + 1);
                collect_calls(n.b, depth + 1);
                if (n.c != NO_NODE) collect_calls(n.c, depth + 1);
                return;
            case Op::Block:
                for (uint32_t i = 0; i < n.b; ++i) collect_calls(img_.kid(n.a + i), depth + 1);
                return;
            case Op::Force: case Op::Neg: case Op::Not:
                collect_calls(n.a, depth + 1);
                return;
            case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod:
            case Op::Eq: case Op::Ne: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge:
            case Op::And: case Op::Or:
                collect_calls(n.a, depth + 1);
                collect_calls(n.b, depth + 1);
                return;
            default:
                return;
        }
    }

    const Image& img_;
    uint32_t fi_;
    const FuncRec& f_;
    Runtime& rt_;
    std::vector<Ty> slot_ty_;
    std::vector<uint32_t> call_sites_;
    bool recurses_ = false;
    bool reads_beyond_params_ = false;

public:
    /// The type of one node, given what is known about the slots. Public
    /// because the emitter asks it the same question while it writes code, and
    /// the two must give the same answer at every node or a value would be
    /// carried in one representation and read in the other.
    Ty ty(uint32_t idx, int depth) {
        if (depth > 256) return Ty::Any;
        const Node& n = img_.node(idx);
        switch (Op(n.op)) {
            case Op::ConstFloat: return Ty::Float;
            case Op::Local: return n.a < slot_ty_.size() ? slot_ty_[n.a] : Ty::Any;
            case Op::Force: case Op::Neg: return ty(n.a, depth + 1);
            case Op::If: {
                // An arm that transfers control -- a tail call -- contributes
                // nothing, which is what makes the type of a loop body the type
                // of its base case.
                Ty t = ty(n.b, depth + 1);
                Ty e = n.c == NO_NODE ? Ty::Any : ty(n.c, depth + 1);
                return join(t, e);
            }
            case Op::Block:
                return n.b == 0 ? Ty::Any : ty(img_.kid(n.a + n.b - 1), depth + 1);
            case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod:
                return arith_ty(ty(n.a, depth + 1), ty(n.b, depth + 1));
            case Op::Apply: {
                // A tail self call produces no value; a non-tail one comes back
                // through the return register, which is a tagged `Value`
                // whatever the body computed.
                if (is_self_call(n)) return (n.flags & F_TAIL) ? Ty::Unknown : Ty::Any;
                switch (known_native(rt_, img_, n.a, n.c)) {
                    case KnownNative::ToFloat:
                    case KnownNative::Sqrt:
                        return Ty::Float;
                    case KnownNative::Abs:
                        // `abs` keeps the shape it was given: an integer stays
                        // an integer and a float stays a float.
                        return ty(img_.kid(n.b), depth + 1) == Ty::Float ? Ty::Float : Ty::Any;
                    default:
                        return Ty::Any;
                }
            }
            default: return Ty::Any;
        }
    }
};

// ---------------------------------------------------------------------------
// Code generation
// ---------------------------------------------------------------------------

class Emitter {
public:
    Emitter(llvm::LLVMContext& ctx, llvm::Module& mod, Runtime& rt, const Image& img, uint32_t fi,
            const Analysis& a)
        : ctx_(ctx), mod_(mod), rt_(rt), img_(img), fi_(fi), f_(img.func(fi)),
          recurses_(a.recurses), float_slots_(a.float_slots), force_first_(a.force_first),
          b_(ctx) {}

    llvm::Function* emit(const std::string& name);

private:
    /// A value in hand, in one of the two representations of "Representation"
    /// above: a tagged `Value`, or a raw `double` nobody has boxed. A null `v`
    /// means control was transferred and there is no value at all -- what a
    /// tail call leaves behind.
    struct JV {
        llvm::Value* v = nullptr;
        bool dbl = false;
    };
    static JV tag(llvm::Value* v) { return JV{v, false}; }
    static JV flt(llvm::Value* v) { return JV{v, true}; }
    static JV none() { return JV{nullptr, false}; }

    /// Does the function carry this slot as a raw double? Decided by the
    /// fixpoint in the analyzer and guarded on entry.
    bool slot_is_dbl(uint32_t i) const { return (float_slots_ >> i) & 1; }

    void declare_helpers();
    /// The body, as a function of `(proc, depth, frame, a0..an, status)`. Only
    /// emitted for a body that calls itself outside tail position; the frame
    /// comes along for the yield at depth zero and is untouched otherwise.
    llvm::Function* emit_body(const std::string& name);
    /// The `CompiledFn` the interpreter enters: unpack the frame once, and
    /// call `body` with depth zero.
    llvm::Function* emit_entry(const std::string& name, llvm::Function* body);
    /// Today's shape, for a body whose only self calls are tail calls: one
    /// function, slots read out of the frame, a loop and nothing else.
    llvm::Function* emit_loop(const std::string& name);
    JV node(uint32_t idx);
    JV binary(const Node& n);
    JV logic(const Node& n);
    JV unary(const Node& n);
    JV conditional(const Node& n);
    JV block(const Node& n);
    JV apply(const Node& n);
    JV native_call(KnownNative which, const Node& n);
    JV self_call(const Node& n);
    JV tail_call(const Node& n, const std::vector<JV>& args);
    JV recursive_call(const std::vector<JV>& args);
    /// Spend one reduction on a call. Answers what is left, so the tail path
    /// can test it; the recursive path ignores it.
    llvm::Value* spend_reduction();
    /// Write the loop-carried parameters back to the heap frame and return
    /// `JIT_YIELD`, so the interpreter resumes the body from the top.
    void emit_yield();
    JV load_slot(uint32_t slot);
    llvm::Value* force(llvm::Value* v);

    // --- the two representations, and the crossings between them -----------

    /// `v` as a tagged `Value`, allocating a float box if it is not one yet.
    /// This is the only thing in the file that can allocate, and the four
    /// places it is reached from are listed under "Representation".
    llvm::Value* box(JV v);
    /// Arithmetic and ordering whose answer is a float, in doubles.
    JV float_arith(Op op, JV a, JV b);
    JV float_order(Op op, JV a, JV b);
    llvm::Value* fop(Op op, llvm::Value* x, llvm::Value* y);
    llvm::Value* order_bool(Op op, llvm::Value* x, llvm::Value* y);
    /// The double `v` stands for, raising `message` if it is not a number.
    /// Emits nothing when `v` is already a double, which is the case that pays.
    llvm::Value* as_double(JV v, const char* message);
    /// The double `v` stands for, branching to `slow` if it is not a number in
    /// hand. Leaves the insert point on the path where it is.
    llvm::Value* try_double(llvm::Value* v, llvm::BasicBlock* slow);
    /// Read a frame slot that the loop carries as a double, or leave for
    /// `bail`. The check is deliberately not a *force*: forcing here would move
    /// where a raising argument raises, and an argument that arrives suspended
    /// is rare enough to be worth handing back rather than reordering for.
    llvm::Value* guard_float(llvm::Value* v, llvm::BasicBlock* bail);
    /// Store `JIT_BAIL` and return: this one call is the interpreter's.
    void emit_bail();
    llvm::Value* unary_intrinsic(llvm::Intrinsic::ID id, llvm::Value* x);

    /// Emit the raise path: store the error and return with JIT_RAISED.
    void emit_raise(llvm::Value* error);
    void emit_type_error(const char* message);

    llvm::Value* i64(uint64_t v) { return llvm::ConstantInt::get(i64_, v); }
    llvm::Value* i32c(int v) { return llvm::ConstantInt::get(i32_, v); }
    llvm::Value* is_fixnum(llvm::Value* v) {
        return b_.CreateICmpNE(b_.CreateAnd(v, i64(1)), i64(0));
    }
    /// Eight-byte aligned and not the "no value" sentinel: a heap object.
    llvm::Value* is_heap_ptr(llvm::Value* v) {
        return b_.CreateAnd(b_.CreateICmpEQ(b_.CreateAnd(v, i64(TAG_MASK)), i64(0)),
                            b_.CreateICmpNE(v, i64(0)));
    }
    /// The `double` inside a float box. Its header is one word, so the payload
    /// is at offset eight and is as aligned as the object.
    llvm::Value* load_float(llvm::Value* v) {
        llvm::Value* at = b_.CreateIntToPtr(b_.CreateAdd(v, i64(8)), ptr_);
        auto* load = b_.CreateLoad(dbl_, at);
        load->setAlignment(llvm::Align(8));
        return load;
    }
    llvm::BasicBlock* bb(const char* name) { return llvm::BasicBlock::Create(ctx_, name, fn_); }

    llvm::LLVMContext& ctx_;
    llvm::Module& mod_;
    Runtime& rt_;
    const Image& img_;
    uint32_t fi_;
    const FuncRec& f_;
    /// Emitted as a function of its arguments rather than of a frame.
    const bool recurses_;
    /// One bit per slot carried as a raw double.
    const SlotSet float_slots_;
    /// Slots to force on entry, in the order the body would have forced them.
    const std::vector<uint32_t> force_first_;
    llvm::IRBuilder<> b_;

    llvm::Type* i64_ = nullptr;
    llvm::Type* i32_ = nullptr;
    llvm::Type* i8_ = nullptr;
    llvm::Type* i1_ = nullptr;
    llvm::Type* dbl_ = nullptr;
    llvm::Type* ptr_ = nullptr;

    llvm::Function* fn_ = nullptr;
    llvm::Argument* proc_ = nullptr;
    llvm::Argument* frame_ = nullptr;
    llvm::Argument* status_ = nullptr;
    /// How many compiled frames of this function are already on the machine
    /// stack. Null in the loop shape, which has none. It is an argument rather
    /// than a counter in the process because an argument needs no restoring on
    /// the way out: a deopt unwinds through every frame at once, and a counter
    /// would have to be put back by each of them.
    llvm::Argument* depth_ = nullptr;

    llvm::BasicBlock* loop_header_ = nullptr;
    std::vector<llvm::Value*> slots_;   // allocas, one per frame slot
    llvm::Value* reduction_slot_ = nullptr;
    bool failed_ = false;

    // Declarations of the runtime helpers.
    llvm::FunctionCallee rt_force_, rt_arith_, rt_compare_, rt_float_, rt_arith_f_, rt_to_int_,
        rt_abs_, rt_floor_, rt_int_of_double_, rt_type_error_, rt_reduction_slot_,
        rt_frame_slots_, rt_frame_store_;
};

llvm::Function* Emitter::emit(const std::string& name) {
    declare_helpers();
    if (!recurses_) return emit_loop(name);
    llvm::Function* body = emit_body(name + ".rec");
    if (!body) return nullptr;
    llvm::Function* entry = emit_entry(name, body);
    if (!entry) {
        body->eraseFromParent();
        return nullptr;
    }
    return entry;
}

void Emitter::declare_helpers() {
    i64_ = llvm::Type::getInt64Ty(ctx_);
    i32_ = llvm::Type::getInt32Ty(ctx_);
    i8_ = llvm::Type::getInt8Ty(ctx_);
    i1_ = llvm::Type::getInt1Ty(ctx_);
    dbl_ = llvm::Type::getDoubleTy(ctx_);
    ptr_ = llvm::PointerType::getUnqual(ctx_);

    rt_force_ = mod_.getOrInsertFunction(
        "dream_rt_force", llvm::FunctionType::get(i32_, {ptr_, i64_, ptr_}, false));
    rt_arith_ = mod_.getOrInsertFunction(
        "dream_rt_arith", llvm::FunctionType::get(i32_, {ptr_, i32_, i64_, i64_, ptr_}, false));
    rt_compare_ = mod_.getOrInsertFunction(
        "dream_rt_compare", llvm::FunctionType::get(i32_, {ptr_, i32_, i64_, i64_, ptr_}, false));
    rt_float_ = mod_.getOrInsertFunction(
        "dream_rt_float", llvm::FunctionType::get(i64_, {ptr_, dbl_}, false));
    rt_arith_f_ = mod_.getOrInsertFunction(
        "dream_rt_arith_f",
        llvm::FunctionType::get(i32_, {ptr_, i32_, i64_, i64_, ptr_, ptr_}, false));
    rt_to_int_ = mod_.getOrInsertFunction(
        "dream_rt_to_int", llvm::FunctionType::get(i32_, {ptr_, i64_, ptr_}, false));
    rt_abs_ = mod_.getOrInsertFunction(
        "dream_rt_abs", llvm::FunctionType::get(i32_, {ptr_, i64_, ptr_}, false));
    rt_floor_ = mod_.getOrInsertFunction(
        "dream_rt_floor", llvm::FunctionType::get(i32_, {ptr_, i64_, ptr_}, false));
    rt_int_of_double_ = mod_.getOrInsertFunction(
        "dream_rt_int_of_double", llvm::FunctionType::get(i64_, {ptr_, dbl_}, false));
    rt_type_error_ = mod_.getOrInsertFunction(
        "dream_rt_type_error", llvm::FunctionType::get(i64_, {ptr_, ptr_}, false));
    rt_reduction_slot_ = mod_.getOrInsertFunction(
        "dream_rt_reduction_slot", llvm::FunctionType::get(ptr_, {ptr_}, false));
    rt_frame_slots_ = mod_.getOrInsertFunction(
        "dream_rt_frame_slots", llvm::FunctionType::get(ptr_, {i64_}, false));
    rt_frame_store_ = mod_.getOrInsertFunction(
        "dream_rt_frame_store",
        llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), {ptr_, i64_, i32_, i64_}, false));
}

llvm::Function* Emitter::emit_loop(const std::string& name) {
    auto* fty = llvm::FunctionType::get(i64_, {ptr_, i64_, ptr_}, false);
    fn_ = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, name, mod_);
    proc_ = fn_->getArg(0);
    frame_ = fn_->getArg(1);
    status_ = fn_->getArg(2);
    proc_->setName("proc");
    frame_->setName("frame");
    status_->setName("status");

    auto* entry = llvm::BasicBlock::Create(ctx_, "entry", fn_);
    b_.SetInsertPoint(entry);

    // Frame slots become allocas so the loop can carry them in registers;
    // they are written back only when yielding. Every one of them is created
    // here, in the entry block, which is where LLVM looks for the allocas it
    // can promote -- and before any guard splits the block underneath us.
    slots_.resize(f_.slots);
    for (uint32_t i = 0; i < f_.slots; ++i) {
        slots_[i] = b_.CreateAlloca(slot_is_dbl(i) ? dbl_ : i64_, nullptr,
                                    "slot" + std::to_string(i));
    }
    llvm::BasicBlock* bail = float_slots_ ? llvm::BasicBlock::Create(ctx_, "bail", fn_) : nullptr;
    llvm::Value* slot_base = b_.CreateCall(rt_frame_slots_, {frame_}, "slots");
    std::vector<llvm::Value*> raw(f_.slots);
    for (uint32_t i = 0; i < f_.slots; ++i) {
        raw[i] = b_.CreateLoad(i64_, b_.CreateGEP(i64_, slot_base, {i64(i)}));
    }
    // Force what the body would have forced, in the order it would have forced
    // it. Empty unless a slot is carried as a double; see `Analyzer::force_order`.
    for (uint32_t slot : force_first_) raw[slot] = force(raw[slot]);
    for (uint32_t i = 0; i < f_.slots; ++i) {
        b_.CreateStore(slot_is_dbl(i) ? guard_float(raw[i], bail) : raw[i], slots_[i]);
    }
    reduction_slot_ = b_.CreateCall(rt_reduction_slot_, {proc_}, "reductions");

    loop_header_ = llvm::BasicBlock::Create(ctx_, "loop", fn_);
    b_.CreateBr(loop_header_);

    if (bail) {
        b_.SetInsertPoint(bail);
        emit_bail();
    }

    b_.SetInsertPoint(loop_header_);
    JV result = node(f_.body);
    if (failed_) {
        fn_->eraseFromParent();
        return nullptr;
    }
    if (result.v) {
        b_.CreateStore(i32c(JIT_OK), status_);
        b_.CreateRet(box(result));
    } else if (!b_.GetInsertBlock()->getTerminator()) {
        // Every path ended in a tail call; nothing falls through here.
        b_.CreateUnreachable();
    }

    if (llvm::verifyFunction(*fn_, &llvm::errs())) {
        fn_->eraseFromParent();
        return nullptr;
    }
    return fn_;
}

llvm::Function* Emitter::emit_body(const std::string& name) {
    // `(proc, depth, frame, a0..an, status)`. The parameters arrive as values
    // rather than through the frame, which is the whole point: a self call
    // allocates nothing, and the only frame in the picture is the one the
    // interpreter already made for the outermost call. A parameter the
    // function carries as a float arrives as a `double`; the entry below is
    // where that is checked, once.
    std::vector<llvm::Type*> params{ptr_, i32_, i64_};
    for (uint32_t i = 0; i < f_.arity; ++i) params.push_back(slot_is_dbl(i) ? dbl_ : i64_);
    params.push_back(ptr_);
    auto* fty = llvm::FunctionType::get(i64_, params, false);
    // Internal, so LLVM is free to inline the first level or two of the
    // recursion into itself and to pick its own calling convention for it.
    fn_ = llvm::Function::Create(fty, llvm::Function::InternalLinkage, name, mod_);
    proc_ = fn_->getArg(0);
    depth_ = fn_->getArg(1);
    frame_ = fn_->getArg(2);
    status_ = fn_->getArg(uint32_t(params.size()) - 1);
    proc_->setName("proc");
    depth_->setName("depth");
    frame_->setName("frame");
    status_->setName("status");

    auto* entry = llvm::BasicBlock::Create(ctx_, "entry", fn_);
    b_.SetInsertPoint(entry);

    // Allocas first and in the entry block, which is where LLVM looks for the
    // ones it can promote to registers.
    slots_.resize(f_.slots);
    for (uint32_t i = 0; i < f_.slots; ++i) {
        slots_[i] = b_.CreateAlloca(slot_is_dbl(i) ? dbl_ : i64_, nullptr,
                                    "slot" + std::to_string(i));
    }
    for (uint32_t i = 0; i < f_.arity; ++i) {
        b_.CreateStore(fn_->getArg(3 + i), slots_[i]);
    }
    // Slots past the parameters belong to `let` bindings, and a body with one
    // is not compiled at all -- the analysis refuses a block containing a
    // `Bind`, and refuses outright to use this shape if any slot beyond the
    // parameters is even read. They are allocated so slot indices line up and
    // left alone.
    for (uint32_t i = f_.arity; i < f_.slots; ++i) b_.CreateStore(i64(UNIT), slots_[i]);
    reduction_slot_ = b_.CreateCall(rt_reduction_slot_, {proc_}, "reductions");

    // Out of machine stack: hand the call back. See `kStackBudget`.
    const uint32_t limit =
        std::max(64u, kStackBudget / (kFrameOverhead + 8 * std::max(1u, uint32_t(f_.slots))));
    auto* deep = llvm::BasicBlock::Create(ctx_, "deep", fn_);
    loop_header_ = llvm::BasicBlock::Create(ctx_, "loop", fn_);
    b_.CreateCondBr(b_.CreateICmpUGE(depth_, llvm::ConstantInt::get(i32_, limit)), deep,
                    loop_header_);

    b_.SetInsertPoint(deep);
    b_.CreateStore(i32c(JIT_DEEP), status_);
    b_.CreateRet(i64(UNIT));

    b_.SetInsertPoint(loop_header_);
    JV result = node(f_.body);
    if (failed_) {
        fn_->eraseFromParent();
        return nullptr;
    }
    if (result.v) {
        b_.CreateStore(i32c(JIT_OK), status_);
        b_.CreateRet(box(result));
    } else if (!b_.GetInsertBlock()->getTerminator()) {
        b_.CreateUnreachable();
    }

    if (llvm::verifyFunction(*fn_, &llvm::errs())) {
        fn_->eraseFromParent();
        return nullptr;
    }
    return fn_;
}

llvm::Function* Emitter::emit_entry(const std::string& name, llvm::Function* body) {
    auto* fty = llvm::FunctionType::get(i64_, {ptr_, i64_, ptr_}, false);
    auto* fn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, name, mod_);
    // The helpers below write into whatever `fn_` names, so the entry takes
    // the emitter over for as long as it is being written. The body is already
    // finished and nothing reaches back into it.
    fn_ = fn;
    depth_ = nullptr;
    slots_.clear();
    proc_ = fn->getArg(0);
    frame_ = fn->getArg(1);
    status_ = fn->getArg(2);
    proc_->setName("proc");
    frame_->setName("frame");
    status_->setName("status");

    b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", fn));
    llvm::BasicBlock* bail = float_slots_ ? llvm::BasicBlock::Create(ctx_, "bail", fn) : nullptr;
    llvm::Value* slot_base = b_.CreateCall(rt_frame_slots_, {frame_}, "slots");
    std::vector<llvm::Value*> raw(f_.arity);
    for (uint32_t i = 0; i < f_.arity; ++i) {
        raw[i] = b_.CreateLoad(i64_, b_.CreateGEP(i64_, slot_base, {i64(i)}));
    }
    // The arguments are handed over unforced, and the body forces each slot on
    // first read exactly as the loop shape does -- so an argument the body
    // never looks at is never touched here either. The exception is the prefix
    // a double-carrying slot obliges: it has to be a double before the body
    // starts, so the body's own force order is replayed up to it and no
    // further.
    for (uint32_t slot : force_first_) {
        if (slot < f_.arity) raw[slot] = force(raw[slot]);
    }
    std::vector<llvm::Value*> args{proc_, i32c(0), frame_};
    for (uint32_t i = 0; i < f_.arity; ++i) {
        args.push_back(slot_is_dbl(i) ? guard_float(raw[i], bail) : raw[i]);
    }
    args.push_back(status_);
    b_.CreateRet(b_.CreateCall(body, args));

    if (bail) {
        b_.SetInsertPoint(bail);
        emit_bail();
    }

    if (llvm::verifyFunction(*fn, &llvm::errs())) {
        fn->eraseFromParent();
        return nullptr;
    }
    return fn;
}

void Emitter::emit_raise(llvm::Value* error) {
    b_.CreateStore(i32c(JIT_RAISED), status_);
    b_.CreateRet(error);
}

void Emitter::emit_type_error(const char* message) {
    llvm::Value* msg = b_.CreateGlobalString(message);
    emit_raise(b_.CreateCall(rt_type_error_, {proc_, msg}));
}

void Emitter::emit_bail() {
    // Nothing has happened yet: no reduction spent, no slot written, no effect.
    // So the interpreter can simply run the call, which is what it would have
    // done had this function never been compiled.
    b_.CreateStore(i32c(JIT_BAIL), status_);
    b_.CreateRet(i64(UNIT));
}

llvm::Value* Emitter::force(llvm::Value* v) {
    // Anything that is not a heap pointer is already in normal form, and in a
    // numeric function that is the overwhelmingly common case.
    auto* need = bb("force.slow");
    auto* done = bb("force.done");
    auto* raise = bb("force.raise");

    llvm::Value* tagged = b_.CreateICmpNE(b_.CreateAnd(v, i64(7)), i64(0));
    llvm::Value* null = b_.CreateICmpEQ(v, i64(0));
    llvm::Value* whnf = b_.CreateOr(tagged, null);
    auto* fast = b_.GetInsertBlock();
    b_.CreateCondBr(whnf, done, need);

    b_.SetInsertPoint(need);
    llvm::Value* out = b_.CreateAlloca(i64_, nullptr, "forced");
    llvm::Value* ok = b_.CreateCall(rt_force_, {proc_, v, out});
    llvm::Value* forced = b_.CreateLoad(i64_, out);
    auto* slow = b_.GetInsertBlock();
    b_.CreateCondBr(b_.CreateICmpNE(ok, i32c(0)), done, raise);

    b_.SetInsertPoint(raise);
    emit_raise(forced);

    b_.SetInsertPoint(done);
    auto* phi = b_.CreatePHI(i64_, 2);
    phi->addIncoming(v, fast);
    phi->addIncoming(forced, slow);
    return phi;
}

Emitter::JV Emitter::load_slot(uint32_t slot) {
    if (slot_is_dbl(slot)) {
        // Already a double: the entry guard saw a float box and nothing since
        // has written anything else here.
        return flt(b_.CreateLoad(dbl_, slots_[slot]));
    }
    llvm::Value* v = b_.CreateLoad(i64_, slots_[slot]);
    llvm::Value* forced = force(v);
    // Write the forced value back so a second read in the same iteration is
    // free; the thunk itself was already updated by the runtime.
    b_.CreateStore(forced, slots_[slot]);
    return tag(forced);
}

// ---------------------------------------------------------------------------
// Crossing between the two representations
// ---------------------------------------------------------------------------

llvm::Value* Emitter::box(JV v) {
    if (!v.dbl) return v.v;
    return b_.CreateCall(rt_float_, {proc_, v.v});
}

llvm::Value* Emitter::try_double(llvm::Value* v, llvm::BasicBlock* slow) {
    auto* fix = bb("num.fix");
    auto* notfix = bb("num.notfix");
    auto* obj = bb("num.obj");
    auto* load = bb("num.load");
    auto* done = bb("num.done");

    b_.CreateCondBr(is_fixnum(v), fix, notfix);

    b_.SetInsertPoint(fix);
    llvm::Value* from_int = b_.CreateSIToFP(b_.CreateAShr(v, 1), dbl_);
    b_.CreateBr(done);

    // Reading the type byte is only legal once the value is known to be a
    // pointer, which is why this is two tests and not one.
    b_.SetInsertPoint(notfix);
    b_.CreateCondBr(is_heap_ptr(v), obj, slow);

    b_.SetInsertPoint(obj);
    llvm::Value* type = b_.CreateLoad(i8_, b_.CreateIntToPtr(v, ptr_));
    b_.CreateCondBr(
        b_.CreateICmpEQ(type, llvm::ConstantInt::get(i8_, uint8_t(ObjType::Float))), load, slow);

    b_.SetInsertPoint(load);
    llvm::Value* from_box = load_float(v);
    b_.CreateBr(done);

    b_.SetInsertPoint(done);
    auto* phi = b_.CreatePHI(dbl_, 2);
    phi->addIncoming(from_int, fix);
    phi->addIncoming(from_box, load);
    return phi;
}

llvm::Value* Emitter::as_double(JV v, const char* message) {
    if (v.dbl) return v.v;
    auto* bad = bb("num.bad");
    llvm::Value* d = try_double(v.v, bad);
    auto* here = b_.GetInsertBlock();
    b_.SetInsertPoint(bad);
    emit_type_error(message);
    b_.SetInsertPoint(here);
    return d;
}

llvm::Value* Emitter::guard_float(llvm::Value* v, llvm::BasicBlock* bail) {
    auto* obj = bb("guard.obj");
    auto* ok = bb("guard.ok");
    b_.CreateCondBr(is_heap_ptr(v), obj, bail);

    b_.SetInsertPoint(obj);
    llvm::Value* type = b_.CreateLoad(i8_, b_.CreateIntToPtr(v, ptr_));
    b_.CreateCondBr(
        b_.CreateICmpEQ(type, llvm::ConstantInt::get(i8_, uint8_t(ObjType::Float))), ok, bail);

    b_.SetInsertPoint(ok);
    return load_float(v);
}

llvm::Value* Emitter::unary_intrinsic(llvm::Intrinsic::ID id, llvm::Value* x) {
    auto* fn = llvm::Intrinsic::getOrInsertDeclaration(&mod_, id, {dbl_});
    return b_.CreateCall(fn, {x});
}

llvm::Value* Emitter::fop(Op op, llvm::Value* x, llvm::Value* y) {
    switch (op) {
        case Op::Add: return b_.CreateFAdd(x, y);
        case Op::Sub: return b_.CreateFSub(x, y);
        case Op::Mul: return b_.CreateFMul(x, y);
        case Op::Div: return b_.CreateFDiv(x, y);
        default: return b_.CreateFRem(x, y);   // Mod, which `arith` does with fmod
    }
}

llvm::Value* Emitter::order_bool(Op op, llvm::Value* x, llvm::Value* y) {
    // `compare` turns two numbers into -1, 0 or 1 using `x < y` and `x > y` and
    // nothing else, so a NaN on either side falls to 0 -- which makes
    // `nan <= nan` *true* in this language. An ordered `fcmp ole` would say
    // false. The two predicates are written here exactly as `compare` writes
    // them, so that the tiers cannot disagree about it.
    llvm::Value* lt = b_.CreateFCmpOLT(x, y);
    llvm::Value* gt = b_.CreateFCmpOGT(x, y);
    llvm::Value* c = nullptr;
    switch (op) {
        case Op::Lt: c = lt; break;
        case Op::Gt: c = gt; break;
        case Op::Le: c = b_.CreateNot(gt); break;
        default: c = b_.CreateNot(lt); break;   // Ge
    }
    return b_.CreateSelect(c, i64(TRUE_V), i64(FALSE_V));
}

Emitter::JV Emitter::float_arith(Op op, JV a, JV bv) {
    // The case the whole thing is for: both sides in registers, and the answer
    // is one instruction with nothing around it.
    if (a.dbl && bv.dbl) return flt(fop(op, a.v, bv.v));

    auto* slow_bb = bb("fbin.slow");
    auto* join_bb = bb("fbin.end");
    llvm::Value* da = a.dbl ? a.v : try_double(a.v, slow_bb);
    llvm::Value* db = bv.dbl ? bv.v : try_double(bv.v, slow_bb);
    llvm::Value* fast = fop(op, da, db);
    auto* fast_end = b_.GetInsertBlock();
    b_.CreateBr(join_bb);

    // Not a number in hand. `arith` decides, and either raises -- which is
    // what all but always happens, with the error the interpreter would have
    // given -- or answers a number, because an operation with a float on one
    // side of it has no third case. So the answer comes back as a double and
    // the caller never learns there were two paths.
    b_.SetInsertPoint(slow_bb);
    llvm::Value* out = b_.CreateAlloca(dbl_);
    llvm::Value* err = b_.CreateAlloca(i64_);
    llvm::Value* ok =
        b_.CreateCall(rt_arith_f_, {proc_, i32c(int(op)), box(a), box(bv), out, err});
    llvm::Value* slowv = b_.CreateLoad(dbl_, out);
    auto* slow_end = b_.GetInsertBlock();
    auto* raise_bb = bb("fbin.raise");
    b_.CreateCondBr(b_.CreateICmpNE(ok, i32c(0)), join_bb, raise_bb);

    b_.SetInsertPoint(raise_bb);
    emit_raise(b_.CreateLoad(i64_, err));

    b_.SetInsertPoint(join_bb);
    auto* phi = b_.CreatePHI(dbl_, 2);
    phi->addIncoming(fast, fast_end);
    phi->addIncoming(slowv, slow_end);
    return flt(phi);
}

Emitter::JV Emitter::float_order(Op op, JV a, JV bv) {
    if (a.dbl && bv.dbl) return tag(order_bool(op, a.v, bv.v));

    auto* slow_bb = bb("fcmp.slow");
    auto* join_bb = bb("fcmp.end");
    llvm::Value* da = a.dbl ? a.v : try_double(a.v, slow_bb);
    llvm::Value* db = bv.dbl ? bv.v : try_double(bv.v, slow_bb);
    llvm::Value* fast = order_bool(op, da, db);
    auto* fast_end = b_.GetInsertBlock();
    b_.CreateBr(join_bb);

    b_.SetInsertPoint(slow_bb);
    llvm::Value* out = b_.CreateAlloca(i64_);
    llvm::Value* ok = b_.CreateCall(rt_compare_, {proc_, i32c(int(op)), box(a), box(bv), out});
    llvm::Value* slowv = b_.CreateLoad(i64_, out);
    auto* slow_end = b_.GetInsertBlock();
    auto* raise_bb = bb("fcmp.raise");
    b_.CreateCondBr(b_.CreateICmpNE(ok, i32c(0)), join_bb, raise_bb);

    b_.SetInsertPoint(raise_bb);
    emit_raise(slowv);

    b_.SetInsertPoint(join_bb);
    auto* phi = b_.CreatePHI(i64_, 2);
    phi->addIncoming(fast, fast_end);
    phi->addIncoming(slowv, slow_end);
    return tag(phi);
}

// ---------------------------------------------------------------------------
// The tree
// ---------------------------------------------------------------------------

Emitter::JV Emitter::node(uint32_t idx) {
    if (failed_) return none();
    const Node& n = img_.node(idx);
    switch (Op(n.op)) {
        case Op::ConstInt: {
            int64_t v = img_.integer(n.a);
            if (!fixnum_fits(v)) {
                failed_ = true;
                return none();
            }
            return tag(i64(make_fixnum(v)));
        }
        case Op::ConstBool: return tag(i64(n.a ? TRUE_V : FALSE_V));
        case Op::ConstChar: return tag(i64(make_char(n.a)));
        case Op::Unit: return tag(i64(UNIT));
        case Op::ConstFloat:
            // The constant *is* the double. Boxing one was the largest single
            // source of garbage in a float loop: `pi` allocated a float box per
            // literal per iteration, four million of them, to hold two numbers
            // that never left the loop.
            return flt(llvm::ConstantFP::get(dbl_, img_.real(n.a)));
        case Op::ConstAtom:
            // Atom indices are remapped through the runtime, so this needs a
            // call; the analyzer allows it because it is cheap and total.
            failed_ = true;
            return none();
        case Op::Local: return load_slot(n.a);
        case Op::Capture: {
            // Captures are immutable for the life of the call.
            failed_ = true;
            return none();
        }
        case Op::Force: return node(n.a);
        case Op::If: return conditional(n);
        case Op::Block: return block(n);
        case Op::And: case Op::Or: return logic(n);
        case Op::Neg: case Op::Not: return unary(n);
        case Op::Apply: return apply(n);
        default: return binary(n);
    }
}

Emitter::JV Emitter::block(const Node& n) {
    JV last = tag(i64(UNIT));
    for (uint32_t i = 0; i < n.b; ++i) {
        uint32_t stmt = img_.kid(n.a + i);
        bool is_last = (i + 1 == n.b);
        if (!is_last && !(img_.node(stmt).flags & F_STRICT)) continue;
        JV v = node(stmt);
        if (failed_) return none();
        if (!v.v) return none();  // control transferred
        if (is_last) last = v;
    }
    return last;
}

Emitter::JV Emitter::conditional(const Node& n) {
    JV cond = node(n.a);
    if (failed_ || !cond.v) return none();
    // A condition that is a float is not a bool, and the tagged path below
    // says so with the message the interpreter would use.
    llvm::Value* c = box(cond);

    auto* then_bb = bb("then");
    auto* else_bb = bb("else");
    auto* bad_bb = bb("if.notbool");
    auto* join_bb = bb("endif");

    llvm::Value* is_true = b_.CreateICmpEQ(c, i64(TRUE_V));
    llvm::Value* is_false = b_.CreateICmpEQ(c, i64(FALSE_V));
    auto* pick = bb("if.pick");
    b_.CreateCondBr(b_.CreateOr(is_true, is_false), pick, bad_bb);

    b_.SetInsertPoint(bad_bb);
    emit_type_error("`if` needs a bool");

    b_.SetInsertPoint(pick);
    b_.CreateCondBr(is_true, then_bb, else_bb);

    // Both arms are written before either is closed, because whether they
    // agree about representation decides what each of them has to leave in the
    // register -- and a box belongs at the end of the arm that needs one, not
    // at the join where it would be paid on both paths.
    b_.SetInsertPoint(then_bb);
    JV tv = node(n.b);
    if (failed_) return none();
    llvm::BasicBlock* tb = b_.GetInsertBlock();

    b_.SetInsertPoint(else_bb);
    JV ev = n.c == NO_NODE ? tag(i64(UNIT)) : node(n.c);
    if (failed_) return none();
    llvm::BasicBlock* eb = b_.GetInsertBlock();

    if (!tv.v && !ev.v) {
        // Both arms tail-called; nothing reaches the join.
        join_bb->eraseFromParent();
        return none();
    }
    const bool as_dbl = tv.v && ev.v ? (tv.dbl && ev.dbl) : (tv.v ? tv.dbl : ev.dbl);
    llvm::Value* tval = nullptr;
    llvm::Value* eval = nullptr;
    if (tv.v) {
        b_.SetInsertPoint(tb);
        tval = as_dbl ? tv.v : box(tv);
        b_.CreateBr(join_bb);
        tb = b_.GetInsertBlock();
    }
    if (ev.v) {
        b_.SetInsertPoint(eb);
        eval = as_dbl ? ev.v : box(ev);
        b_.CreateBr(join_bb);
        eb = b_.GetInsertBlock();
    }

    b_.SetInsertPoint(join_bb);
    if (tval && eval) {
        auto* phi = b_.CreatePHI(as_dbl ? dbl_ : i64_, 2);
        phi->addIncoming(tval, tb);
        phi->addIncoming(eval, eb);
        return JV{phi, as_dbl};
    }
    return JV{tval ? tval : eval, as_dbl};
}

Emitter::JV Emitter::logic(const Node& n) {
    JV lhs = node(n.a);
    if (failed_ || !lhs.v) return none();
    llvm::Value* l = box(lhs);
    const bool is_and = (Op(n.op) == Op::And);

    auto* rhs_bb = bb("logic.rhs");
    auto* short_bb = bb("logic.short");
    auto* bad_bb = bb("logic.notbool");
    auto* join_bb = bb("logic.end");

    llvm::Value* is_true = b_.CreateICmpEQ(l, i64(TRUE_V));
    llvm::Value* is_false = b_.CreateICmpEQ(l, i64(FALSE_V));
    auto* pick = bb("logic.pick");
    b_.CreateCondBr(b_.CreateOr(is_true, is_false), pick, bad_bb);

    b_.SetInsertPoint(bad_bb);
    emit_type_error(is_and ? "`and` needs a bool" : "`or` needs a bool");

    b_.SetInsertPoint(pick);
    // `and` continues on true, `or` continues on false.
    b_.CreateCondBr(is_true, is_and ? rhs_bb : short_bb, is_and ? short_bb : rhs_bb);

    b_.SetInsertPoint(short_bb);
    llvm::Value* shortcut = i64(is_and ? FALSE_V : TRUE_V);
    b_.CreateBr(join_bb);

    b_.SetInsertPoint(rhs_bb);
    JV rhs = node(n.b);
    if (failed_) return none();
    // The short-circuit answer is a bool, so the two paths only meet as tagged
    // values; a float right-hand side is boxed here rather than at the join.
    llvm::Value* rv = rhs.v ? box(rhs) : nullptr;
    llvm::BasicBlock* rb = b_.GetInsertBlock();
    if (rv) b_.CreateBr(join_bb);

    b_.SetInsertPoint(join_bb);
    auto* phi = b_.CreatePHI(i64_, 2);
    phi->addIncoming(shortcut, short_bb);
    if (rv) phi->addIncoming(rv, rb);
    return tag(phi);
}

Emitter::JV Emitter::unary(const Node& n) {
    JV x = node(n.a);
    if (failed_ || !x.v) return none();

    if (Op(n.op) == Op::Not) {
        llvm::Value* v = box(x);
        auto* bad_bb = bb("not.notbool");
        auto* ok_bb = bb("not.ok");
        llvm::Value* is_true = b_.CreateICmpEQ(v, i64(TRUE_V));
        llvm::Value* is_false = b_.CreateICmpEQ(v, i64(FALSE_V));
        b_.CreateCondBr(b_.CreateOr(is_true, is_false), ok_bb, bad_bb);
        b_.SetInsertPoint(bad_bb);
        emit_type_error("`not` needs a bool");
        b_.SetInsertPoint(ok_bb);
        return tag(b_.CreateSelect(is_true, i64(FALSE_V), i64(TRUE_V)));
    }

    // Negation is `0 - x`, which is what the interpreter reaches for too --
    // and it is a subtraction rather than a sign flip because the two differ
    // on zero: `0 - 0.0` is `0.0` where `fneg 0.0` is `-0.0`.
    if (x.dbl) return flt(b_.CreateFSub(llvm::ConstantFP::get(dbl_, 0.0), x.v));

    // The integer case: -(2x+1) as a tagged value is 2 - v, with an overflow
    // check.
    llvm::Value* v = x.v;
    auto* fast_bb = bb("neg.fast");
    auto* slow_bb = bb("neg.slow");
    auto* join_bb = bb("neg.end");
    b_.CreateCondBr(is_fixnum(v), fast_bb, slow_bb);

    b_.SetInsertPoint(fast_bb);
    auto* ssub = llvm::Intrinsic::getOrInsertDeclaration(
        &mod_, llvm::Intrinsic::ssub_with_overflow, {i64_});
    llvm::Value* pair = b_.CreateCall(ssub, {i64(2), v});
    llvm::Value* fastv = b_.CreateExtractValue(pair, 0);
    llvm::Value* ovf = b_.CreateExtractValue(pair, 1);
    auto* fast_ok = bb("neg.fast.ok");
    b_.CreateCondBr(ovf, slow_bb, fast_ok);
    b_.SetInsertPoint(fast_ok);
    b_.CreateBr(join_bb);

    b_.SetInsertPoint(slow_bb);
    llvm::Value* out = b_.CreateAlloca(i64_);
    llvm::Value* ok = b_.CreateCall(
        rt_arith_, {proc_, i32c(int(Op::Sub)), i64(make_fixnum(0)), v, out});
    llvm::Value* slowv = b_.CreateLoad(i64_, out);
    auto* slow_end = b_.GetInsertBlock();
    auto* raise_bb = bb("neg.raise");
    b_.CreateCondBr(b_.CreateICmpNE(ok, i32c(0)), join_bb, raise_bb);
    b_.SetInsertPoint(raise_bb);
    emit_raise(slowv);

    b_.SetInsertPoint(join_bb);
    auto* phi = b_.CreatePHI(i64_, 2);
    phi->addIncoming(fastv, fast_ok);
    phi->addIncoming(slowv, slow_end);
    return tag(phi);
}

Emitter::JV Emitter::binary(const Node& n) {
    Op op = Op(n.op);
    JV av = node(n.a);
    if (failed_ || !av.v) return none();
    JV bv = node(n.b);
    if (failed_ || !bv.v) return none();

    const bool is_order = (op == Op::Lt || op == Op::Le || op == Op::Gt || op == Op::Ge);
    const bool is_cmp = is_order || op == Op::Eq || op == Op::Ne;
    const bool is_arith = !is_cmp && op != Op::And && op != Op::Or;

    if (av.dbl || bv.dbl) {
        if (is_arith) return float_arith(op, av, bv);
        if (is_order) return float_order(op, av, bv);
        // `==` and `!=` fall through to the tagged path on purpose. Two float
        // boxes that are the *same object* are equal in this language whatever
        // they hold, NaN included -- `values_equal` compares the words before
        // it compares the numbers -- and once a float is in a register there is
        // no object left to ask about.
    }

    llvm::Value* a = box(av);
    llvm::Value* bb_ = box(bv);

    const bool inline_arith = (op == Op::Add || op == Op::Sub || op == Op::Mul);
    if (!is_cmp && !inline_arith) {
        // Division and remainder go straight to the runtime: the zero checks
        // and the integer/float split are not worth duplicating here.
        auto* out = b_.CreateAlloca(i64_);
        llvm::Value* ok = b_.CreateCall(rt_arith_, {proc_, i32c(int(op)), a, bb_, out});
        llvm::Value* v = b_.CreateLoad(i64_, out);
        auto* cont = bb("arith.ok");
        auto* raise_bb = bb("arith.raise");
        b_.CreateCondBr(b_.CreateICmpNE(ok, i32c(0)), cont, raise_bb);
        b_.SetInsertPoint(raise_bb);
        emit_raise(v);
        b_.SetInsertPoint(cont);
        return tag(v);
    }

    auto* fast_bb = bb("bin.fast");
    auto* slow_bb = bb("bin.slow");
    auto* join_bb = bb("bin.end");
    b_.CreateCondBr(b_.CreateAnd(is_fixnum(a), is_fixnum(bb_)), fast_bb, slow_bb);

    b_.SetInsertPoint(fast_bb);
    llvm::Value* fastv = nullptr;
    llvm::BasicBlock* fast_end = nullptr;
    if (is_cmp) {
        // The tag is a left shift plus one, so tagged fixnums compare in the
        // same order as the integers they stand for -- no untagging needed.
        llvm::Value* c = nullptr;
        switch (op) {
            case Op::Lt: c = b_.CreateICmpSLT(a, bb_); break;
            case Op::Le: c = b_.CreateICmpSLE(a, bb_); break;
            case Op::Gt: c = b_.CreateICmpSGT(a, bb_); break;
            case Op::Ge: c = b_.CreateICmpSGE(a, bb_); break;
            case Op::Eq: c = b_.CreateICmpEQ(a, bb_); break;
            default: c = b_.CreateICmpNE(a, bb_); break;
        }
        fastv = b_.CreateSelect(c, i64(TRUE_V), i64(FALSE_V));
        fast_end = b_.GetInsertBlock();
        b_.CreateBr(join_bb);
    } else {
        llvm::Intrinsic::ID iid = op == Op::Add   ? llvm::Intrinsic::sadd_with_overflow
                                  : op == Op::Sub ? llvm::Intrinsic::ssub_with_overflow
                                                  : llvm::Intrinsic::smul_with_overflow;
        llvm::Value* lhs = a;
        llvm::Value* rhs = bb_;
        if (op == Op::Mul) {
            // Untag both, multiply, then retag: 2xy+1.
            lhs = b_.CreateAShr(a, 1);
            rhs = b_.CreateAShr(bb_, 1);
        } else if (op == Op::Add) {
            rhs = b_.CreateSub(bb_, i64(1));   // (2x+1) + (2y+1) - 1
        } else {
            rhs = b_.CreateSub(bb_, i64(1));   // (2x+1) - (2y+1) + 1 == a - (b-1)
        }
        auto* fn = llvm::Intrinsic::getOrInsertDeclaration(&mod_, iid, {i64_});
        llvm::Value* pair = b_.CreateCall(fn, {lhs, rhs});
        llvm::Value* raw = b_.CreateExtractValue(pair, 0);
        llvm::Value* ovf = b_.CreateExtractValue(pair, 1);

        auto* ok_bb = bb("bin.fast.ok");
        if (op == Op::Mul) {
            // Retagging shifts left by one, which can overflow independently.
            auto* shl = llvm::Intrinsic::getOrInsertDeclaration(
                &mod_, llvm::Intrinsic::smul_with_overflow, {i64_});
            auto* nofast = bb("bin.mul.retag");
            b_.CreateCondBr(ovf, slow_bb, nofast);
            b_.SetInsertPoint(nofast);
            llvm::Value* p2 = b_.CreateCall(shl, {raw, i64(2)});
            llvm::Value* shifted = b_.CreateExtractValue(p2, 0);
            llvm::Value* ovf2 = b_.CreateExtractValue(p2, 1);
            b_.CreateCondBr(ovf2, slow_bb, ok_bb);
            b_.SetInsertPoint(ok_bb);
            fastv = b_.CreateOr(shifted, i64(1));
        } else {
            b_.CreateCondBr(ovf, slow_bb, ok_bb);
            b_.SetInsertPoint(ok_bb);
            fastv = raw;
        }
        fast_end = b_.GetInsertBlock();
        b_.CreateBr(join_bb);
    }

    b_.SetInsertPoint(slow_bb);
    auto* out = b_.CreateAlloca(i64_);
    llvm::FunctionCallee helper = is_cmp ? rt_compare_ : rt_arith_;
    llvm::Value* ok = b_.CreateCall(helper, {proc_, i32c(int(op)), a, bb_, out});
    llvm::Value* slowv = b_.CreateLoad(i64_, out);
    auto* slow_end = b_.GetInsertBlock();
    auto* raise_bb = bb("bin.raise");
    b_.CreateCondBr(b_.CreateICmpNE(ok, i32c(0)), join_bb, raise_bb);
    b_.SetInsertPoint(raise_bb);
    emit_raise(slowv);

    b_.SetInsertPoint(join_bb);
    auto* phi = b_.CreatePHI(i64_, 2);
    phi->addIncoming(fastv, fast_end);
    phi->addIncoming(slowv, slow_end);
    return tag(phi);
}

// ---------------------------------------------------------------------------
// Calls
// ---------------------------------------------------------------------------

Emitter::JV Emitter::apply(const Node& n) {
    KnownNative which = known_native(rt_, img_, n.a, n.c);
    if (which != KnownNative::None) return native_call(which, n);
    return self_call(n);
}

/// One of the numeric natives, made rather than called.
///
/// Each is the same few instructions the native runs, with the same error where
/// it raises one. What they replace is not the call -- a native call is cheap
/// -- but the *refusal*: a function containing one was not compiled at all.
Emitter::JV Emitter::native_call(KnownNative which, const Node& n) {
    JV x = node(img_.kid(n.b));
    if (failed_ || !x.v) return none();

    switch (which) {
        case KnownNative::ToFloat:
            return flt(as_double(x, "to_float needs a number"));
        case KnownNative::Sqrt:
            return flt(unary_intrinsic(llvm::Intrinsic::sqrt,
                                       as_double(x, "sqrt needs a number")));
        case KnownNative::Abs:
            // `abs` keeps the shape it was given, so only the float case is
            // written here; an integer one goes to the native's own code, where
            // the fixnum range makes negating the smallest value a special
            // case not worth repeating.
            if (x.dbl) return flt(unary_intrinsic(llvm::Intrinsic::fabs, x.v));
            break;
        case KnownNative::Floor:
            if (x.dbl) {
                llvm::Value* d = unary_intrinsic(llvm::Intrinsic::floor, x.v);
                return tag(b_.CreateCall(rt_int_of_double_, {proc_, d}));
            }
            break;
        case KnownNative::ToInt:
            if (x.dbl) return tag(b_.CreateCall(rt_int_of_double_, {proc_, x.v}));
            break;
        default:
            break;
    }

    // The tagged cases: hand the value to the runtime, which is the native's
    // own body, and take its answer or its error.
    llvm::FunctionCallee helper = which == KnownNative::Abs     ? rt_abs_
                                  : which == KnownNative::Floor ? rt_floor_
                                                                : rt_to_int_;
    auto* out = b_.CreateAlloca(i64_);
    llvm::Value* ok = b_.CreateCall(helper, {proc_, x.v, out});
    llvm::Value* v = b_.CreateLoad(i64_, out);
    auto* cont = bb("native.ok");
    auto* raise_bb = bb("native.raise");
    b_.CreateCondBr(b_.CreateICmpNE(ok, i32c(0)), cont, raise_bb);
    b_.SetInsertPoint(raise_bb);
    emit_raise(v);
    b_.SetInsertPoint(cont);
    return tag(v);
}

Emitter::JV Emitter::self_call(const Node& n) {
    // Evaluate every argument before touching any slot: an argument may read a
    // parameter this call is about to overwrite.
    std::vector<JV> args(n.c);
    for (uint32_t i = 0; i < n.c; ++i) {
        args[i] = node(img_.kid(n.b + i));
        if (failed_ || !args[i].v) return none();
    }
    if (n.flags & F_TAIL) return tail_call(n, args);
    return recursive_call(args);
}

llvm::Value* Emitter::spend_reduction() {
    llvm::Value* left = b_.CreateLoad(i64_, reduction_slot_);
    llvm::Value* next = b_.CreateSub(left, i64(1));
    b_.CreateStore(next, reduction_slot_);
    return next;
}

void Emitter::emit_yield() {
    // Publish the loop-carried values back to the frame and hand control to
    // the interpreter, which resumes at the top of this body. The store runs
    // the write barrier: the frame may be old, the value young, and the next
    // minor collection has to know the edge exists.
    //
    // A slot the loop carried as a double is boxed here, which is the one
    // allocation a fused float loop makes -- once every few thousand
    // iterations, when its slice runs out, rather than once per operation.
    //
    // Only the parameters in the recursive shape: the slots past them are
    // never read there, and the frame already holds whatever the interpreter
    // put in them.
    const uint32_t n = recurses_ ? f_.arity : f_.slots;
    for (uint32_t i = 0; i < n; ++i) {
        llvm::Value* v = slot_is_dbl(i) ? box(flt(b_.CreateLoad(dbl_, slots_[i])))
                                        : b_.CreateLoad(i64_, slots_[i]);
        b_.CreateCall(rt_frame_store_, {proc_, frame_, i32c(int(i)), v});
    }
    b_.CreateStore(i32c(JIT_YIELD), status_);
    b_.CreateRet(i64(UNIT));
}

/// A tail call is the loop back-edge: overwrite the parameters and jump.
Emitter::JV Emitter::tail_call(const Node& n, const std::vector<JV>& args) {
    for (uint32_t i = 0; i < n.c; ++i) {
        // The coercions here should both be no-ops: the analyzer decided a slot
        // was a double by looking at exactly these expressions. They are
        // written anyway, so that a disagreement between the two would be a
        // conversion rather than a value read in the wrong shape.
        b_.CreateStore(slot_is_dbl(i)
                           ? as_double(args[i], "a loop carrying a float was given something else")
                           : box(args[i]),
                       slots_[i]);
    }

    // Spend a reduction and check the budget, so a compiled loop is just as
    // preemptible as an interpreted one.
    llvm::Value* next = spend_reduction();

    auto* yield_bb = bb("yield");
    auto* cont_bb = bb("iterate");
    llvm::Value* carry_on = b_.CreateICmpSGT(next, i64(0));
    if (recurses_) {
        // A yield leaves the interpreter to resume *this* invocation from the
        // top of the body -- which it can only do for the outermost one. Below
        // that there are compiled frames on the machine stack waiting for a
        // value, and returning to the interpreter would abandon them. So the
        // loop keeps going instead, and the slice ends when the whole chain
        // comes back. The budget still falls, so nothing runs unaccounted.
        carry_on = b_.CreateOr(carry_on, b_.CreateICmpNE(depth_, i32c(0)));
    }
    b_.CreateCondBr(carry_on, cont_bb, yield_bb);

    b_.SetInsertPoint(yield_bb);
    emit_yield();

    b_.SetInsertPoint(cont_bb);
    b_.CreateBr(loop_header_);
    return none();  // control transferred
}

/// A call anywhere else is a machine call, one frame deeper.
///
/// Its status is the callee's: `JIT_RAISED` and `JIT_DEEP` both mean this
/// invocation has nothing left to say, and the value in hand is the callee's
/// -- an error, or the placeholder a deopt returns -- so it is returned as it
/// stands. Only `JIT_OK` carries on. That is the same shape every slow-path
/// call in this file already has, which is why there is no unwinding to do:
/// a compiled body has no effects to undo and nothing written down but its own
/// registers.
Emitter::JV Emitter::recursive_call(const std::vector<JV>& args) {
    spend_reduction();

    std::vector<llvm::Value*> call{proc_, b_.CreateAdd(depth_, i32c(1)), frame_};
    for (uint32_t i = 0; i < args.size(); ++i) {
        call.push_back(slot_is_dbl(i)
                           ? as_double(args[i], "a call carrying a float was given something else")
                           : box(args[i]));
    }
    call.push_back(status_);
    llvm::Value* r = b_.CreateCall(fn_, call);

    auto* ok_bb = bb("call.ok");
    auto* out_bb = bb("call.out");
    llvm::Value* st = b_.CreateLoad(i32_, status_);
    b_.CreateCondBr(b_.CreateICmpEQ(st, i32c(JIT_OK)), ok_bb, out_bb);

    b_.SetInsertPoint(out_bb);
    b_.CreateRet(r);

    b_.SetInsertPoint(ok_bb);
    // Whatever the body computed, it comes back through the return register as
    // a tagged `Value`. That is why a non-tail self call is `Ty::Any`: the
    // status has to travel with it, and a `double` return would leave the
    // error nowhere to sit.
    return tag(r);
}

}  // namespace

// ---------------------------------------------------------------------------
// Jit
// ---------------------------------------------------------------------------

struct Jit::Impl {
    explicit Impl(Runtime& r) : rt(r) {}

    Runtime& rt;
    std::unique_ptr<llvm::orc::LLJIT> lljit;
    std::mutex mutex;
    std::unordered_map<uint32_t, CompiledFn> compiled;
    std::unordered_set<uint32_t> rejected;
    uint32_t threshold = 32;
    uint64_t compiled_count = 0;
    bool initialized = false;

    bool ensure_jit(std::string* error);
};

bool Jit::Impl::ensure_jit(std::string* error) {
    if (lljit) return true;
    if (!initialized) {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        initialized = true;
    }
    auto built = llvm::orc::LLJITBuilder().create();
    if (!built) {
        if (error) *error = llvm::toString(built.takeError());
        return false;
    }
    lljit = std::move(*built);

    // Expose the runtime helpers to compiled code by absolute address; they
    // are in this shared object, not something the JIT can look up by name.
    auto& jd = lljit->getMainJITDylib();
    llvm::orc::SymbolMap syms;
    auto add = [&](const char* name, void* addr) {
        syms[lljit->mangleAndIntern(name)] = llvm::orc::ExecutorSymbolDef(
            llvm::orc::ExecutorAddr::fromPtr(addr), llvm::JITSymbolFlags::Exported);
    };
    add("dream_rt_force", reinterpret_cast<void*>(&dream_rt_force));
    add("dream_rt_arith", reinterpret_cast<void*>(&dream_rt_arith));
    add("dream_rt_compare", reinterpret_cast<void*>(&dream_rt_compare));
    add("dream_rt_float", reinterpret_cast<void*>(&dream_rt_float));
    add("dream_rt_arith_f", reinterpret_cast<void*>(&dream_rt_arith_f));
    add("dream_rt_to_int", reinterpret_cast<void*>(&dream_rt_to_int));
    add("dream_rt_abs", reinterpret_cast<void*>(&dream_rt_abs));
    add("dream_rt_floor", reinterpret_cast<void*>(&dream_rt_floor));
    add("dream_rt_int_of_double", reinterpret_cast<void*>(&dream_rt_int_of_double));
    add("dream_rt_type_error", reinterpret_cast<void*>(&dream_rt_type_error));
    add("dream_rt_reduction_slot", reinterpret_cast<void*>(&dream_rt_reduction_slot));
    add("dream_rt_frame_slots", reinterpret_cast<void*>(&dream_rt_frame_slots));
    add("dream_rt_frame_store", reinterpret_cast<void*>(&dream_rt_frame_store));
    if (auto err = jd.define(llvm::orc::absoluteSymbols(std::move(syms)))) {
        if (error) *error = llvm::toString(std::move(err));
        lljit.reset();
        return false;
    }
    return true;
}

bool Jit::available() { return true; }

Jit::Jit(Runtime& rt) : impl_(std::make_unique<Impl>(rt)) {
    const size_t funcs = impl_->rt.image().func_count();
    counts_ = std::vector<std::atomic<uint32_t>>(funcs);
    cached_ = std::vector<std::atomic<CompiledFn>>(funcs);
    threshold_.store(impl_->threshold, std::memory_order_relaxed);
    rt.set_jit(this);
}
Jit::~Jit() { impl_->rt.set_jit(nullptr); }

void Jit::set_threshold(uint32_t calls) {
    impl_->threshold = calls;
    threshold_.store(calls, std::memory_order_relaxed);
}
uint32_t Jit::threshold() const { return impl_->threshold; }
uint64_t Jit::compiled_count() const { return impl_->compiled_count; }

CompiledFn Jit::on_enter(uint32_t func_index) {
    // Reached once per function: `tier` has counted the entries inline and
    // this is the one that crossed the threshold. The answer is written back
    // into `cached_` either way -- a compiled body, or the rejected marker --
    // so no later entry comes anywhere near this lock.
    std::lock_guard<std::mutex> g(impl_->mutex);
    auto it = impl_->compiled.find(func_index);
    if (it != impl_->compiled.end()) return it->second;
    if (impl_->rejected.count(func_index)) return nullptr;

    CompiledFn fn = compile_locked(func_index, nullptr);
    if (!fn) {
        impl_->rejected.insert(func_index);
        cached_[func_index].store(rejected(), std::memory_order_release);
        return nullptr;
    }
    cached_[func_index].store(fn, std::memory_order_release);
    return fn;
}

void Jit::deoptimize(uint32_t func_index) {
    if (func_index >= cached_.size()) return;
    // Only the tier table is touched. The compiled body stays in `compiled`
    // and stays valid -- `--dump-jit` and a second runtime over the same image
    // still want it -- but no entry will reach it again, which is the point:
    // the recursion that ran out of machine stack would run out of it every
    // time, and each attempt would throw away the work it did before finding
    // out.
    cached_[func_index].store(rejected(), std::memory_order_release);
}

CompiledFn Jit::compile(uint32_t func_index, std::string* error) {
    std::lock_guard<std::mutex> g(impl_->mutex);
    return compile_locked(func_index, error);
}

CompiledFn Jit::compile_locked(uint32_t func_index, std::string* error) {
    auto it = impl_->compiled.find(func_index);
    if (it != impl_->compiled.end()) return it->second;

    const Image& img = impl_->rt.image();
    Analysis a = Analyzer(impl_->rt, img, func_index).run();
    if (!a.compilable) {
        if (error) *error = "not compilable by this tier";
        return nullptr;
    }
    if (!impl_->ensure_jit(error)) return nullptr;

    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto mod = std::make_unique<llvm::Module>("dream.jit", *ctx);
    mod->setDataLayout(impl_->lljit->getDataLayout());

    std::string name = "dream_fn_" + std::to_string(func_index);
    Emitter em(*ctx, *mod, impl_->rt, img, func_index, a);
    if (!em.emit(name)) {
        if (error) *error = "code generation failed";
        return nullptr;
    }

    // Standard optimization pipeline: without it the tag arithmetic and the
    // repeated slot loads dominate, and the compiled loop is barely faster
    // than the interpreter.
    llvm::PassBuilder pb;
    llvm::LoopAnalysisManager lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager cgam;
    llvm::ModuleAnalysisManager mam;
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);
    auto mpm = pb.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
    mpm.run(*mod, mam);

    if (auto err = impl_->lljit->addIRModule(
            llvm::orc::ThreadSafeModule(std::move(mod), std::move(ctx)))) {
        if (error) *error = llvm::toString(std::move(err));
        return nullptr;
    }
    auto sym = impl_->lljit->lookup(name);
    if (!sym) {
        if (error) *error = llvm::toString(sym.takeError());
        return nullptr;
    }
    auto fn = sym->toPtr<CompiledFn>();
    impl_->compiled[func_index] = fn;
    ++impl_->compiled_count;
    return fn;
}

std::string Jit::dump_ir(uint32_t func_index) {
    std::lock_guard<std::mutex> g(impl_->mutex);
    const Image& img = impl_->rt.image();
    Analysis a = Analyzer(impl_->rt, img, func_index).run();
    if (!a.compilable) {
        return "; fn#" + std::to_string(func_index) +
               " is not compilable by this tier (it is interpreted)\n";
    }
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto mod = std::make_unique<llvm::Module>("dream.jit", *ctx);
    Emitter em(*ctx, *mod, impl_->rt, img, func_index, a);
    if (!em.emit("dream_fn_" + std::to_string(func_index))) {
        return "; code generation failed\n";
    }
    std::string out;
    llvm::raw_string_ostream os(out);
    mod->print(os, nullptr);
    return out;
}

}  // namespace dream
