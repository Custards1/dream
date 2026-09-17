// The LLVM JIT tier.
//
// Scope, and why it is drawn where it is: this compiles the strict numeric
// spine of a function -- arithmetic, comparisons, branches, `let`, calls to
// itself and to other functions it can compile, and calls to the host natives
// of `std.native` -- and leaves everything else to the interpreter. The limit is
// not laziness in general but a soundness requirement. Compiled code evaluates a
// call's arguments eagerly, and doing that to an argument the callee would never
// have forced can raise an error in a program that was going to terminate
// quietly. So a function is only compiled when a strictness analysis proves
// every parameter is forced on every path, and a call is only compiled when the
// same is true of the callee -- which is what `PeerSet` asks. Anything else
// stays interpreted, where laziness is explicit and free.
//
// Two things sit deliberately *outside* that rule, and both are there because
// they are not evaluations at all. A parameter every self call hands straight
// back to itself is moved rather than computed, so it needs no strictness --
// `Analyzer::carried`, and it is what lets a loop carry the string, array or map
// it is walking. And an argument in a position a native's strict mask does not
// claim is suspended rather than evaluated, against a snapshot of the current
// slots -- `Emitter::suspend`. Both do exactly what `thunk_for` does with the
// same expression, which is what makes them free of the question above.
//
// What that rule does *not* promise is that an argument is forced at the same
// point the interpreter would have forced it, only that it is forced. Where two
// errors are reachable that can decide which one a program gets; the repro and
// why it is not fixed are under "The tier's eager arguments can change *which*
// error a program raises" in CLAUDE.md.
//
// A `let` has nowhere to live here -- no thunks, and the frame's slots are
// registers -- so its value is emitted where its name is read, which is where
// the interpreter would have forced the thunk. `Analyzer::check_bind` is the
// rule and what it costs.
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
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

#include "builtins.hpp"
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

/// How many *other* functions one compilation may pull into its module.
///
/// A call to another global function is compiled by emitting that function's
/// body into the same module and calling it, so saying yes to one costs the
/// compile time of writing it. That time is real and already visible: the
/// first run of `mapfilter` spends about 19 ms in LLVM, which is most of what
/// the benchmark reports for it. A small closure buys the shapes that matter
/// -- a loop that calls a helper, a helper that calls one more -- and a large
/// one buys a compile nobody asked for.
constexpr uint32_t kMaxPeers = 8;
/// How deep the closure may be followed. A guard against a chain of calls,
/// not a budget: `kMaxPeers` is what bounds the size.
constexpr int kMaxPeerDepth = 8;
/// "Not a function index", for the answer to "which function does this call
/// name?" when it does not name one.
constexpr uint32_t kNoFunc = ~uint32_t(0);

/// How many arguments a native compiled code calls may take.
///
/// The call is one helper with a fixed signature and the arguments in
/// registers, so the widest native decides the signature. Four covers every one
/// the VM provides -- `ffi.bind!` and `ffi.load!` are the only four, and the
/// pure ones stop at three -- and a host module that registers a wider one is
/// simply not called from compiled code.
constexpr uint32_t kMaxNativeArgs = 4;

/// How big a `let` may be for its value to be written out more than once.
///
/// A binding read twice has no thunk to remember its answer in, so writing it
/// where each name is read runs it twice. Counted in nodes, after every `let`
/// inside it has itself been written out -- which is what stops
/// `let a = ..; let b = a + a; let c = b + b` from doubling at every step.
constexpr uint32_t kMaxBindDuplication = 8;

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

// ---------------------------------------------------------------------------
// Natives compiled code calls
//
// The five above are natives compiled code *makes*: each is a few instructions,
// so writing them out beats calling them. Everything else a program reaches
// through `std.native` -- `str_byte`, `map_get`, `array_get`, `compare`, `len`
// -- is a call, and until this existed a single one of them refused the whole
// function. That refusal was most of what kept the tier to hand-shaped numeric
// loops: a lexer that reads a byte, a resolver that looks a name up in a map and
// a comparison written over `core.compare` are all arithmetic with one call in
// the middle of them.
//
// A call is made the way the interpreter makes one -- same callee value, same
// arguments on the same stack, same native -- so the two tiers cannot disagree
// about what a native means. What decides whether a call may be *compiled* is
// below, and it is three questions:
//
//   * **Is it pure?** Purity is spelling in this language, so this is one look
//     at the last character of the name. An impure native is refused, and that
//     one rule is what makes the rest of this simple: nothing here has to order
//     an effect against anything, and nothing here can park -- every native that
//     answers `NativeOutcome::Block` is impure, and so is everything reachable
//     from the Dream work a pure one runs underneath itself.
//   * **Is it saturated, and narrow enough?** A partial application has no
//     callee to enter, and a variadic native has no fixed argument positions for
//     a strict mask to describe. `kMaxNativeArgs` is the rest.
//   * **May every argument be produced here?** A position the native's strict
//     mask claims is evaluated, which is what the interpreter does a step later
//     anyway. A position it does not claim is *suspended* -- `Emitter::suspend`
//     -- so it is not an eager evaluation at all, and so is never an event.
//     See jit_rt.hpp and the note under "A compiled function may call a host
//     native" in CLAUDE.md for why the tier admits the function regardless.
//
// What is deliberately *not* asked is whether the native allocates, or forces,
// or runs Dream code underneath itself. Allocation never collects, so compiled
// code has always been allowed to allocate -- `dream_rt_float` does -- and
// forcing is what `dream_rt_force` already does on every slot read. Both are the
// runtime's to get right, and `run_native` in jit_rt.cpp is where it does.
// ---------------------------------------------------------------------------

/// A native a compiled body may call, and everything needed to call it.
///
/// `builtin` says which of the two shapes it is: a builtin is an immediate
/// carrying an index into a static table, and a member is one function of one
/// host module, named by where it sits. Both are constants at compile time --
/// the module registry is fixed before a program runs -- so the call site is a
/// helper call with two literals in it and nothing to look up.
struct NativeSite {
    bool ok = false;
    bool builtin = false;
    /// Index into `rt.modules()`, when this is a member.
    uint32_t module = 0;
    /// Index within that module's members, or the builtin's table index.
    uint32_t member = 0;
    uint32_t strict_mask = 0;
};

/// Purity, the way the language spells it: a name ending in `!` is impure.
bool native_is_pure(const char* name) {
    const size_t n = std::strlen(name);
    return n > 0 && name[n - 1] != '!';
}

/// Which native this call names, when it names one this tier may call.
///
/// The callee has to be a `Builtin` node or `Field(Global module, name)` -- a
/// member of a module named at the top level, which is what every `std` call
/// looks like. `module_for_import` is the same lookup the interpreter does, so a
/// member of the same name from anywhere else resolves to a different
/// `ModuleDef`, or to none, and is not recognised.
NativeSite native_site(Runtime& rt, const Image& img, uint32_t callee_node, uint32_t argc) {
    NativeSite site;
    const Node& c = img.node(callee_node);
    const char* name = nullptr;
    uint32_t arity = 0;
    bool vouches = false;

    if (Op(c.op) == Op::Builtin) {
        if (c.a >= builtin_count()) return site;
        const BuiltinDef& bd = builtin_def(c.a);
        site.builtin = true;
        site.member = c.a;
        site.strict_mask = bd.strict_mask;
        name = bd.name;
        arity = bd.arity;
        vouches = bd.vouches;
    } else if (Op(c.op) == Op::Field) {
        const Node& m = img.node(c.a);
        if (Op(m.op) != Op::Global) return site;
        const GlobalRec& g = img.global(m.a);
        if (g.kind != GLOBAL_MODULE) return site;
        const ModuleDef* def = rt.module_for_import(g.target);
        if (!def) return site;
        const NativeDef* nd = def->find(img.str(c.b));
        if (!nd) return site;
        // Two indices rather than either pointer: a module is found again at run
        // time by where it sits, which is fixed once a program is running and so
        // needs nothing to stay valid. `modules()` is filled while the runtime
        // is being built and never afterwards.
        site.module = uint32_t(def - rt.modules().data());
        site.member = uint32_t(nd - def->members.data());
        site.strict_mask = nd->strict_mask;
        name = nd->name;
        arity = nd->arity;
        vouches = nd->vouches;
    } else {
        return site;
    }

    if (!native_is_pure(name)) return site;
    if (arity == NATIVE_VARIADIC || arity != argc || arity > kMaxNativeArgs) return site;
    // A native that vouches for the collector is one this tier must not call:
    // it walks a lazy structure underneath itself and is written so a
    // collection may happen while it does, and a call from compiled code pins
    // the heap instead. `NativeDef::vouches` has the measurement. Declining
    // costs nothing that was not already being paid -- such a function was not
    // compiled at all before this -- and it keeps the widening from being a
    // trade of memory for speed.
    if (vouches) return site;
    site.ok = true;
    return site;
}

/// The ops the emitter can write.
///
/// The list and the emitter's own switch have to say the same thing, and the
/// reason is `PeerSet`: an emitter that gives up on a peer gives up on the whole
/// closure, including a root that had nothing wrong with it. While a compile was
/// one function, refusing in the emitter cost nothing.
///
/// What is still absent is everything that names something the tier has no way
/// to enter: `Op::Global` and `Op::Field` as *values* rather than as the callee
/// of a call, `Op::Try`, and `Op::Bind` outside a block. Note that this list is
/// asked only of a node compiled code *emits* -- a node it suspends is run by
/// the interpreter, where every op is supported, so a `Try` inside a list
/// element or an unclaimed argument refuses nothing.
bool op_is_supported(Op op) {
    static const bool no_get = std::getenv("DREAM_JIT_NO_GET") != nullptr;
    static const bool no_set = std::getenv("DREAM_JIT_NO_SET") != nullptr;
    static const bool no_list = std::getenv("DREAM_JIT_NO_LIST") != nullptr;
    static const bool no_array = std::getenv("DREAM_JIT_NO_ARRAY") != nullptr;
    static const bool no_map = std::getenv("DREAM_JIT_NO_MAP") != nullptr;
    static const bool no_closure = std::getenv("DREAM_JIT_NO_CLOSURE") != nullptr;
    static const bool no_cap = std::getenv("DREAM_JIT_NO_CAP") != nullptr;
    static const bool no_thunk = std::getenv("DREAM_JIT_NO_THUNK") != nullptr;
    static const bool no_str = std::getenv("DREAM_JIT_NO_STR") != nullptr;
    static const bool no_atom = std::getenv("DREAM_JIT_NO_ATOM") != nullptr;
    if (no_get && op == Op::Get) return false;
    if (no_set && op == Op::Set) return false;
    if (no_list && (op == Op::MakeList || op == Op::MakeArray)) return false;
    if (no_array && op == Op::MakeArray) return false;
    if (no_map && op == Op::MakeMap) return false;
    if (no_closure && op == Op::MakeClosure) return false;
    if (no_cap && op == Op::Capture) return false;
    if (no_thunk && op == Op::MakeThunk) return false;
    if (no_str && op == Op::ConstStr) return false;
    if (no_atom && op == Op::ConstAtom) return false;
    switch (op) {
        case Op::ConstInt: case Op::ConstFloat: case Op::ConstBool:
        case Op::ConstChar: case Op::ConstStr: case Op::ConstAtom: case Op::Unit:
        case Op::Local: case Op::Capture:
        case Op::If: case Op::Block: case Op::Force:
        case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod:
        case Op::Eq: case Op::Ne: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge:
        case Op::And: case Op::Or: case Op::Neg: case Op::Not:
        case Op::MakeList: case Op::MakeArray: case Op::MakeMap:
        case Op::MakeClosure: case Op::MakeThunk:
        case Op::Get: case Op::Set:
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Analysis
// ---------------------------------------------------------------------------

/// An image function, by index, named -- for the refusal reasons.
std::string func_name(const Image& img, uint32_t fi) {
    const FuncRec& f = img.func(fi);
    return f.name != NO_NODE ? img.str(f.name).str() : "fn#" + std::to_string(fi);
}

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
    /// The other global functions this body calls. Every one of them is a
    /// member of the compile's `PeerSet` and is emitted beside it.
    std::vector<uint32_t> calls;
    /// The body calls itself in tail position, so it is a loop. That is the
    /// best shape there is for the root of a compile and the one shape a peer
    /// may not have; `PeerSet::admit` says why.
    bool tail_self = false;
    /// Parameters every self call hands straight back to itself, one bit per
    /// slot. Such an argument is moved rather than evaluated, which is what
    /// lets it be neither forced nor proved strict. See `Analyzer::carried`.
    SlotSet carried = 0;
    /// Parameters a self call *suspends* rather than evaluates, one bit per
    /// slot. The interpreter suspends every argument; this tier evaluates the
    /// ones the callee is proved to force and suspends the rest, which is what
    /// lets a function be compiled without proving anything about the others.
    /// See the fixpoint in `Analyzer::run`.
    SlotSet lazy = 0;
    /// For each slot, the node a `let` bound it to, or `NO_NODE`. A bound slot
    /// is never read: the expression is written where the name is. See
    /// `Analyzer::check_bind`.
    std::vector<uint32_t> binds;
    /// Why the function was refused, when it was. The first refusal found,
    /// which is good enough to aim a widening at -- chasing reasons is what
    /// `DREAM_JIT_TRACE` and `--dump-jit` are for. Empty when `compilable`.
    /// See `Analyzer::refuse`.
    std::string refuse;
};

class PeerSet;
/// `peers.admit(gi, depth)`, spelled as a free function because `Analyzer` and
/// `PeerSet` each have to name the other: deciding whether a call may be
/// compiled means analysing the callee, and analysing the callee means asking
/// the same question of everything *it* calls. `refuse`, if non-null, receives
/// why the callee was refused, which `PeerSet` is complete enough to say by the
/// time it is implemented.
bool peer_admits(PeerSet& peers, uint32_t gi, int depth, std::string* refuse = nullptr);

class Analyzer {
public:
    Analyzer(Runtime& rt, const Image& img, uint32_t func_index, PeerSet* peers = nullptr,
             int peer_depth = 0)
        : img_(img), fi_(func_index), f_(img.func(func_index)), rt_(rt), peers_(peers),
          peer_depth_(peer_depth) {}

    Analysis run() {
        Analysis a;
        auto finish = [&] { a.refuse = refuse_; return a; };
        // A reason is only recorded when someone asks, so refusal has no reason of its own to be lazy about.
        if (f_.slots > 64) {
            refuse("more than 64 slots");
            return finish();
        }
        if (f_.arity == 0) {
            refuse("arity 0");
            return finish();
        }
        binds_.assign(f_.slots, NO_NODE);
        bound_yet_.assign(f_.slots, false);
        reads_.assign(f_.slots, 0);

        // Every `let` first, before anything is decided. The three answers
        // below -- what the body forces, what it carries, what it suspends --
        // each read a binding through its slot, and none of them can wait for
        // the validating walk, because that walk needs *them* to know which
        // subtrees it has to validate at all.
        collect_binds(f_.body, 0);
        if (rebound_) {
            refuse("a slot is bound twice");
            return finish();
        }
        collect_calls(f_.body, 0);
        carried_ = carried();

        const SlotSet params = f_.arity >= 64 ? ~SlotSet(0) : ((SlotSet(1) << f_.arity) - 1);

        // What a parameter has to be, and why the two answers differ.
        //
        // The rule the tier rests on is about *eager evaluation*: compiled code
        // evaluates a call's arguments before making the call, and doing that to
        // an argument the callee would never have forced can raise in a program
        // that was going to finish quietly. So wherever an argument is
        // evaluated, the parameter it lands in must be one the callee forces
        // anyway.
        //
        // There are now three ways for a parameter to satisfy that, and only the
        // first is a proof about the body:
        //
        //   * **forced on every path** (`strict_of`), so evaluating the argument
        //     at the call raises exactly what the callee would have raised;
        //   * **carried** -- handed straight back, slot `i` to slot `i`, by every
        //     self call, which moves a word and evaluates nothing;
        //   * **suspended**, which is what the interpreter does with every
        //     argument there is and is therefore always available.
        //
        // The third is what turns the rule from a gate into a choice: a function
        // is no longer refused for a parameter it does not force, it merely gets
        // a thunk there. Eagerness survives exactly where it pays -- a numeric
        // loop forces everything it carries, so nothing about `sum`'s fused loop
        // changes -- and everywhere else the tier does what the interpreter does.
        //
        // The three sets are circular, because whether an argument's own
        // subexpression forces a slot depends on whether that argument is
        // evaluated at all. So `strict` is computed against a growing set of
        // suspended positions until it stops shrinking. It terminates because
        // `lazy_` only ever grows and is bounded by the parameters.
        //
        // A **peer** takes none of this. It is entered by a caller that
        // evaluates its arguments, and that caller has no way to know which of
        // the callee's positions were meant to be suspended -- so a peer still
        // has to force every parameter on every path. `peer_call` is what cites
        // that, and it cannot be weakened from here.
        SlotSet strict = 0;
        for (int round = 0; round <= int(f_.arity) + 1; ++round) {
            strict = strict_of(f_.body, 0);
            if (peer_depth_ > 0) break;
            const SlotSet next = params & ~(strict | carried_);
            if (next == lazy_) break;
            lazy_ = next;
        }

        const SlotSet licensed = peer_depth_ > 0 ? strict : (strict | carried_ | lazy_);
        if ((licensed & params) != params) {
            std::string missing;
            for (uint32_t i = 0; i < f_.arity; ++i) {
                if (!((licensed >> i) & 1)) {
                    char buf[32];
                    std::snprintf(buf, sizeof buf, "#%u ", i);
                    missing += buf;
                }
            }
            refuse((peer_depth_ > 0 ? "a peer must force every parameter; "
                                    : "a parameter is neither forced, carried nor suspended: ") +
                   missing);
            return finish();
        }

        // The validating walk, which needs `lazy_`: an argument in a suspended
        // position is not emitted, so nothing about it has to be emittable.
        if (!check(f_.body, 0)) {
            if (refuse_.empty()) refuse_ = "this tier's analysis refused the body "
                                          "but recorded no specific reason";
            return finish();
        }

        // A slot no parameter owns and no `let` bound holds state that lives in
        // the frame and nowhere this can reach. Nothing should produce one --
        // `Op::Bind` is the only thing that writes such a slot, and every bind
        // in the body has just been accounted for -- so this is checked rather
        // than argued, because the argument is about the compiler and this file
        // is not.
        if (reads_beyond_params_) {
            refuse("reads a slot no parameter owns, or reads one before its `let`");
            return finish();
        }
        if (!bindings_substitutable()) return finish();

        // A body emitted as a function of its arguments is handed a frame that
        // is not its own: the outermost invocation's in the recursive shape,
        // because a self call passes the same one down, and the *caller's* in a
        // peer. Its closure is the only thing about it still true of this
        // invocation, and only because a function reached by name has no
        // captures. So a function that has captures of its own -- one that reads
        // a capture, builds a closure over one, or suspends an expression that
        // might do either -- is compiled in the loop shape or not at all, since
        // there the frame really is this invocation's.
        if (f_.n_captures > 0 && (recurses_ || peer_depth_ > 0)) {
            refuse("a function with captures is only compiled as a loop");
            return finish();
        }

        a.binds = binds_;
        a.carried = carried_;
        a.lazy = lazy_;
        a.compilable = true;
        a.strict_params = strict;
        a.recurses = recurses_;
        a.tail_self = tail_self_;
        a.calls = calls_;
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
        a.refuse = refuse_;
        return a;
    }

private:
    /// Is this node, and everything under it, something we can emit?
    bool check(uint32_t node, int depth) {
        if (depth > 256) {
            refuse("body deeper than 256 nodes");
            return false;
        }
        const Node& n = img_.node(node);
        Op op = Op(n.op);

        if (op == Op::Apply) return check_apply(n, depth);
        if (!op_is_supported(op)) {
            refuse(std::string("unsupported op `") + op_name(op) + "`");
            return false;
        }
        // An integer too big for a fixnum is a boxed constant the emitter has
        // no way to name, so it is refused here rather than there -- same
        // reason as the two ops missing from the list above.
        if (op == Op::ConstInt && !fixnum_fits(img_.integer(n.a))) {
            refuse("an integer constant does not fit a fixnum");
            return false;
        }
        if (op == Op::Local) {
            if (n.a >= f_.slots) {
                refuse("a local names a slot past the end of the frame");
                return false;
            }
            ++reads_[n.a];
            // A read of a slot that is neither a parameter nor bound *yet*.
            // "Yet" is what makes a binding that names itself, or one that names
            // a later binding, refuse the function rather than expand for ever:
            // `collect_binds` knows every slot a `let` writes, so the ordering
            // has to be said here, where the body is walked in the order it runs.
            if (n.a >= f_.arity && !bound_yet_[n.a]) reads_beyond_params_ = true;
        }
        if (op == Op::Capture && n.a >= f_.n_captures) {
            refuse("a capture names a slot past the end of the closure");
            return false;
        }

        switch (op) {
            case Op::If:
                return check(n.a, depth + 1) && check(n.b, depth + 1) &&
                       (n.c == NO_NODE || check(n.c, depth + 1));
            case Op::Block:
                for (uint32_t i = 0; i < n.b; ++i) {
                    uint32_t stmt = img_.kid(n.a + i);
                    if (Op(img_.node(stmt).op) == Op::Bind) {
                        if (!check_bind(img_.node(stmt), depth + 1)) return false;
                        continue;
                    }
                    if (!check(stmt, depth + 1)) return false;
                }
                return true;
            case Op::Force: case Op::Neg: case Op::Not:
                return check(n.a, depth + 1);
            case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod:
            case Op::Eq: case Op::Ne: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge:
            case Op::And: case Op::Or:
                return check(n.a, depth + 1) && check(n.b, depth + 1);
            // A list's and an array's elements are suspended, exactly as the
            // interpreter suspends them, so nothing about them has to be
            // emittable. A map's *keys* are not: it has to hash them, so the
            // machine forces each one where it stands and so does this.
            case Op::MakeList: case Op::MakeArray:
                return true;
            case Op::MakeMap:
                for (uint32_t i = 0; i < n.b; ++i) {
                    if (!check(img_.kid(n.a + i * 2), depth + 1)) return false;
                }
                return true;
            // The container and the key are forced; a `get`'s fallback is
            // suspended where a missing key would reach it, and a `set`'s value
            // is stored unforced -- so neither has to be emittable, any more
            // than a list element does.
            case Op::Get:
                return check(n.a, depth + 1) && check(n.b, depth + 1);
            case Op::Set:
                return check(n.a, depth + 1) && check(n.b, depth + 1);
            // The function a closure is built over is entered by the
            // interpreter, so it is not this tier's to have an opinion about.
            case Op::MakeClosure: case Op::MakeThunk:
                if (n.a >= img_.func_count()) {
                    refuse("a closure names a function that is not in the image");
                    return false;
                }
                return true;
            default:
                return true;
        }
    }

    /// This function calling itself with a full argument list. In tail position
    /// it becomes a loop back-edge; anywhere else a machine call, which is what
    /// `recurses_` records: the two need different shapes of function around
    /// them. A call to some *other* global function is a peer, below.
    bool is_self_call(const Node& n) const {
        if (Op(n.op) != Op::Apply || n.c != f_.arity) return false;
        const Node& callee = img_.node(n.a);
        if (Op(callee.op) != Op::Global) return false;
        const GlobalRec& g = img_.global(callee.a);
        return g.kind == GLOBAL_FUNCTION && g.target == fi_;
    }

    /// A call. Four of them are compiled: this function calling itself, one of
    /// the numeric natives compiled code makes rather than calls, a host native
    /// it calls, and a saturated call to another global function this tier can
    /// write. Anything else -- a closure, a partial application, a call through
    /// a local -- refuses the whole function, because there is no way to enter a
    /// callee that is not named.
    bool check_apply(const Node& n, int depth) {
        if (is_self_call(n)) {
            for (uint32_t i = 0; i < n.c; ++i) {
                // A carried argument is moved and a lazy one is suspended;
                // neither is emitted, so neither has to be emittable.
                if (i < 64 && (((carried_ | lazy_) >> i) & 1)) continue;
                if (!check(img_.kid(n.b + i), depth + 1)) return false;
            }
            if (n.flags & F_TAIL) {
                tail_self_ = true;
            } else {
                recurses_ = true;
            }
            return true;
        }
        if (known_native(rt_, img_, n.a, n.c) != KnownNative::None) {
            for (uint32_t i = 0; i < n.c; ++i) {
                if (!check(img_.kid(n.b + i), depth + 1)) return false;
            }
            return true;
        }
        // A host native. An argument its strict mask claims is evaluated here,
        // which is exactly what the interpreter does a step later; one the mask
        // does not claim is suspended, which is exactly what the interpreter
        // does with that. So only the first kind has to be emittable -- and that
        // is the whole of why `core.cons`, whose mask claims nothing, no longer
        // refuses everything it appears in.
        const NativeSite site = native_site(rt_, img_, n.a, n.c);
        if (site.ok) {
            for (uint32_t i = 0; i < n.c; ++i) {
                if (!(i < 32 && ((site.strict_mask >> i) & 1))) continue;
                if (!check(img_.kid(n.b + i), depth + 1)) return false;
            }
            return true;
        }
        // A saturated call to another global function, when that function is
        // itself something this tier can write. It is compiled by emitting the
        // callee into the same module and calling it -- so the whole closure of
        // callees is decided here, before a line of either is written.
        const uint32_t gi = peer_target(n);
        if (gi == kNoFunc) {
            refuse("a call whose callee is not a name (a closure, a partial "
                   "application, or a call through a local)");
            return false;
        }
        if (!peers_) {
            refuse("a call to a peer from an analysis with no peer set");
            return false;
        }
        std::string peer_refuse;
        if (!peer_admits(*peers_, gi, peer_depth_, &peer_refuse)) {
            refuse("peer `" + func_name(img_, gi) + "` refused: " +
                   (peer_refuse.empty() ? "not compilable as a peer" : peer_refuse));
            return false;
        }
        for (uint32_t i = 0; i < n.c; ++i) {
            if (!check(img_.kid(n.b + i), depth + 1)) return false;
        }
        calls_.push_back(gi);
        return true;
    }

    /// The function this call names, when it is a saturated call to a global
    /// function other than this one. `kNoFunc` otherwise -- which covers a
    /// partial application, a call through a local or a closure, and a call of
    /// a module member.
    ///
    /// A self call is deliberately excluded: it has its own two shapes, and a
    /// function is not its own peer.
    uint32_t peer_target(const Node& n) const {
        if (Op(n.op) != Op::Apply) return kNoFunc;
        const Node& callee = img_.node(n.a);
        if (Op(callee.op) != Op::Global) return kNoFunc;
        const GlobalRec& g = img_.global(callee.a);
        if (g.kind != GLOBAL_FUNCTION || g.target == fi_) return kNoFunc;
        if (g.target >= img_.func_count()) return kNoFunc;
        if (n.c != img_.func(g.target).arity) return kNoFunc;
        return g.target;
    }

    /// Does compiled code evaluate every one of this call's arguments before
    /// making it? True of a self call and of a peer, false of a native, which
    /// evaluates only the ones its strict mask claims.
    ///
    /// For a peer the licence is the same one the self call rests on: a
    /// function is only admitted when it forces every parameter on every path,
    /// so an argument evaluated here is an argument the callee was going to
    /// force anyway.
    bool evaluates_all_args(const Node& n) const {
        return is_self_call(n) || peer_target(n) != kNoFunc;
    }

    /// Which of this call's arguments the native it names forces. Zero when it
    /// names no native, which is the answer that claims nothing.
    ///
    /// The natives compiled code *makes* rather than calls are recognised here
    /// too, and answer the same mask: they are ordinary members of an ordinary
    /// module, and which arguments they force is a fact about them rather than
    /// about how this tier chooses to emit them.
    uint32_t native_mask(const Node& n) const {
        const NativeSite site = native_site(rt_, img_, n.a, n.c);
        return site.ok ? site.strict_mask : 0;
    }

    // --- bindings ---------------------------------------------------------
    //
    // A `let` in a compiled body has nowhere to put its value. The interpreter
    // suspends it in a thunk against the frame and forces that thunk the first
    // time the name is read; a compiled body has no thunks, and its frame slots
    // are registers the collector cannot see.
    //
    // So the value is written where the name is *read* instead. That is exactly
    // where the interpreter would have forced the thunk, so nothing moves: a
    // binding nobody reads is never evaluated, which is what an unforced thunk
    // comes to, and a binding read once is evaluated at the read. What is lost
    // is the thunk's memory of its own answer -- read twice, the expression runs
    // twice -- which is why reading twice is only allowed for something small
    // that calls nothing.

    /// A `let` statement. Answers false to refuse the whole function.
    bool check_bind(const Node& n, int depth) {
        // An impure `let` is a statement, not a binding: the block runs it where
        // it stands because the effect has to happen there, and writing it
        // somewhere else would move the effect. Its value would have to be a
        // call this tier refuses anyway -- but the reason to refuse is the
        // order, which is worth saying where the order is decided.
        if (n.flags & F_STRICT) return false;
        // Into a parameter's slot, or past the end of the frame: neither is
        // something the compiler emits, and neither has a meaning here.
        if (n.a < f_.arity) {
            refuse("a `let` binds into a parameter's slot");
            return false;
        }
        if (n.a >= f_.slots) {
            refuse("a `let` binds past the end of the frame");
            return false;
        }
        // One slot, one binding. Bound twice, a read means whichever came
        // before it, and this has no way to say which that was. `collect_binds`
        // already refused the function on `rebound_`; this is the same check in
        // the walk that has to say "yet", because `bound_yet_` is what licenses
        // a read that comes after the binding.
        if (bound_yet_[n.a]) {
            refuse("a slot is bound twice");
            return false;
        }
        if (!check(n.b, depth + 1)) return false;
        binds_[n.a] = n.b;
        // The binding is passed: a read of this slot from here on is a read of
        // the value bound now, so `check`'s `Local` case stops flagging it.
        bound_yet_[n.a] = true;
        return true;
    }

    /// May every `let` in the body be written where its name is read?
    ///
    /// Read once or not at all, always. Read more than once, only when the
    /// value is small and contains no call: "once per read" then costs a couple
    /// of instructions, where a duplicated call could be a duplicated
    /// recursion, and nested duplication is how a linear body becomes an
    /// exponential one.
    bool bindings_substitutable() {
        for (uint32_t slot = f_.arity; slot < f_.slots; ++slot) {
            if (binds_[slot] == NO_NODE || reads_[slot] <= 1) continue;
            bool calls = false;
            const uint32_t cost = expand_cost(binds_[slot], 0, &calls);
            if (calls || cost > kMaxBindDuplication) {
                char buf[32];
                std::snprintf(buf, sizeof buf, "slot #%u ", slot);
                refuse("a `let` bound in " + std::string(buf) + "is read more than once and " +
                       (calls ? "its value contains a call"
                              : "its value is bigger than kMaxBindDuplication"));
                return false;
            }
        }
        return true;
    }

    /// How many nodes `idx` becomes once every `let` inside it has been written
    /// out, and whether any of it is a call. Only asked of a body `check` has
    /// already accepted, so every op it meets is one this tier emits.
    uint32_t expand_cost(uint32_t idx, int depth, bool* calls) {
        if (depth > 64) {
            // Too deep to measure. Say the one thing that refuses it either way.
            *calls = true;
            return kMaxBindDuplication + 1;
        }
        const Node& n = img_.node(idx);
        switch (Op(n.op)) {
            case Op::Local:
                if (n.a >= f_.arity && binds_[n.a] != NO_NODE) {
                    return expand_cost(binds_[n.a], depth + 1, calls);
                }
                return 1;
            case Op::Apply:
                *calls = true;
                return 1;
            case Op::If: {
                uint32_t c = 1 + expand_cost(n.a, depth + 1, calls) +
                             expand_cost(n.b, depth + 1, calls);
                if (n.c != NO_NODE) c += expand_cost(n.c, depth + 1, calls);
                return c;
            }
            case Op::Block: {
                uint32_t c = 1;
                for (uint32_t i = 0; i < n.b; ++i) {
                    const uint32_t stmt = img_.kid(n.a + i);
                    const Node& sn = img_.node(stmt);
                    c += expand_cost(Op(sn.op) == Op::Bind ? sn.b : stmt, depth + 1, calls);
                }
                return c;
            }
            case Op::Force: case Op::Neg: case Op::Not:
                return 1 + expand_cost(n.a, depth + 1, calls);
            case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod:
            case Op::Eq: case Op::Ne: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge:
            case Op::And: case Op::Or:
                return 1 + expand_cost(n.a, depth + 1, calls) +
                       expand_cost(n.b, depth + 1, calls);
            default:
                return 1;
        }
    }

    /// Parameters every self call hands straight back to itself.
    ///
    /// `loop s (i + 1) n acc` in a function whose first parameter is `s`: the
    /// argument is slot 0 and the position is slot 0, so the call moves nothing
    /// and evaluates nothing. Every site has to agree -- one that computes a new
    /// value there is an evaluation like any other, and the parameter is back to
    /// needing the strictness the rest of the tier asks for.
    ///
    /// Asked of the self call sites only, which is what `collect_calls`
    /// gathers: a peer's arguments are evaluated at the call, so nothing is
    /// carried across one.
    SlotSet carried() const {
        if (call_sites_.empty()) return 0;
        SlotSet bits = f_.arity >= 64 ? ~SlotSet(0) : ((SlotSet(1) << f_.arity) - 1);
        for (uint32_t site : call_sites_) {
            const Node& n = img_.node(site);
            for (uint32_t i = 0; i < f_.arity; ++i) {
                const Node& arg = img_.node(img_.kid(n.b + i));
                if (Op(arg.op) != Op::Local || arg.a != i) bits &= ~(SlotSet(1) << i);
            }
        }
        return bits;
    }

    /// Slots definitely forced when this node is reduced to WHNF.
    SlotSet strict_of(uint32_t node, int depth) {
        if (depth > 256) return 0;
        const Node& n = img_.node(node);
        switch (Op(n.op)) {
            case Op::Local:
                // A bound slot is the expression it was bound to, written here.
                if (n.a >= f_.arity && binds_[n.a] != NO_NODE) {
                    return strict_of(binds_[n.a], depth + 1);
                }
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
                const bool eager = evaluates_all_args(n);
                const uint32_t mask = eager ? ~uint32_t(0) : native_mask(n);
                const bool self = is_self_call(n);
                for (uint32_t i = 0; i < n.c; ++i) {
                    if (!eager && !((mask >> i) & 1)) continue;
                    // A carried argument is moved, not evaluated, so it forces
                    // nothing. Counting it would be this set proving a parameter
                    // strict on the strength of a read that only happens because
                    // the set said the parameter was strict.
                    if (self && i < 64 && ((carried_ >> i) & 1)) continue;
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
        // The sites were gathered in `run`, which needs them before this does.

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
                if (n.a >= f_.arity && binds_[n.a] != NO_NODE) {
                    return force_order(binds_[n.a], depth + 1, out);
                }
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
                const uint32_t mask = evaluates_all_args(n) ? ~uint32_t(0) : native_mask(n);
                const bool self = is_self_call(n);
                for (uint32_t i = 0; i < n.c; ++i) {
                    if (!((mask >> i) & 1)) continue;
                    // Moved, not evaluated -- so nothing is forced here and the
                    // order carries on past it. See `carried`.
                    if (self && i < 64 && ((carried_ >> i) & 1)) continue;
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
                // Through a `let` to its value: the call inside one is emitted
                // where the name is read, and the fixpoint has to see it.
                for (uint32_t i = 0; i < n.b; ++i) {
                    const uint32_t stmt = img_.kid(n.a + i);
                    const Node& sn = img_.node(stmt);
                    collect_calls(Op(sn.op) == Op::Bind ? sn.b : stmt, depth + 1);
                }
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

    /// Every `let` in the body, before anything is decided.
    ///
    /// The three answers below -- what the body forces, what it carries, what it
    /// suspends -- each read a binding through its slot, and none of them can
    /// wait for the validating walk, because that walk needs *them* to know
    /// which subtrees it has to validate at all. So `binds_` is filled here,
    /// over the whole body including the subtrees `check` will skip because
    /// they are suspended: those can read a `let` of their own when a snapshot
    /// gives them a frame, and the binding has to be there for them.
    ///
    /// A slot a second `let` writes has no single meaning for either the
    /// fixpoint or the substituting emitter, so that is refused here, up front,
    /// where `run` can see it before a line of the analysis has run.
    void collect_binds(uint32_t idx, int depth) {
        if (depth > 256) return;
        const Node& n = img_.node(idx);
        switch (Op(n.op)) {
            case Op::Bind:
                if (n.a >= f_.arity && n.a < f_.slots) {
                    if (binds_[n.a] != NO_NODE) {
                        rebound_ = true;
                    } else {
                        binds_[n.a] = n.b;
                    }
                }
                collect_binds(n.b, depth + 1);
                return;
            case Op::If:
                collect_binds(n.a, depth + 1);
                collect_binds(n.b, depth + 1);
                if (n.c != NO_NODE) collect_binds(n.c, depth + 1);
                return;
            case Op::Block:
                for (uint32_t i = 0; i < n.b; ++i) {
                    collect_binds(img_.kid(n.a + i), depth + 1);
                }
                return;
            case Op::Force: case Op::Neg: case Op::Not:
                collect_binds(n.a, depth + 1);
                return;
            case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod:
            case Op::Eq: case Op::Ne: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge:
            case Op::And: case Op::Or:
            case Op::Get:
                collect_binds(n.a, depth + 1);
                collect_binds(n.b, depth + 1);
                if (Op(n.op) == Op::Get && n.c != NO_NODE) collect_binds(n.c, depth + 1);
                return;
            case Op::Set:
                collect_binds(n.a, depth + 1);
                collect_binds(n.b, depth + 1);
                collect_binds(n.c, depth + 1);
                return;
            case Op::MakeList: case Op::MakeArray:
                for (uint32_t i = 0; i < n.b; ++i) {
                    collect_binds(img_.kid(n.a + i), depth + 1);
                }
                return;
            case Op::MakeMap:
                for (uint32_t i = 0; i < 2 * n.b; ++i) {
                    collect_binds(img_.kid(n.a + i), depth + 1);
                }
                return;
            case Op::Apply:
                for (uint32_t i = 0; i < n.c; ++i) {
                    collect_binds(img_.kid(n.b + i), depth + 1);
                }
                return;
            default:
                return;
        }
    }

    const Image& img_;
    uint32_t fi_;
    const FuncRec& f_;
    Runtime& rt_;
    PeerSet* peers_ = nullptr;
    int peer_depth_ = 0;
    std::vector<Ty> slot_ty_;
    std::vector<uint32_t> call_sites_;
    std::vector<uint32_t> calls_;
    /// Per slot: the node a `let` bound it to, or `NO_NODE`; and how many times
    /// the body reads it. Both are filled in by `check` as it walks.
    std::vector<uint32_t> binds_;
    std::vector<uint32_t> reads_;
    /// Per slot: whether the validating walk has passed the `let` that binds it.
    /// A read of a slot not yet bound here is a binding that names itself or a
    /// later one, which refuses the function rather than expand for ever. See
    /// the `Local` case of `check`.
    std::vector<bool> bound_yet_;
    /// A slot a second `let` wrote. Set by `collect_binds`, which is the walk
    /// that has the whole body before anything else runs; `run` refuses on it
    /// immediately, before the fixpoint reads through the bindings.
    bool rebound_ = false;
    /// Parameters every self call moves rather than evaluates. Computed in
    /// `run` before `strict_of`, which has to know.
    SlotSet carried_ = 0;
    /// Parameters every self call suspends rather than evaluates, one bit per
    /// slot. The fixpoint in `run` grows it round by round; an argument in a
    /// suspended position is thunked at the call, which is exactly what the
    /// interpreter does with every argument. See the third bullet of `run`.
    SlotSet lazy_ = 0;
    bool recurses_ = false;
    bool tail_self_ = false;
    bool reads_beyond_params_ = false;
    /// The first reason `refuse` has been given, kept so `run` can hand it to
    /// the analysis and from there to whoever reports a refusal.
    std::string refuse_;

    /// Record why this function cannot be compiled. Only the first reason is
    /// kept -- a report is a pointer at a widening, not an inventory, and the
    /// first refusal is the one a walk order would have to change.
    void refuse(const std::string& why) {
        if (refuse_.empty()) refuse_ = why;
    }

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
            case Op::Local:
                if (n.a >= f_.arity && n.a < binds_.size() && binds_[n.a] != NO_NODE) {
                    return ty(binds_[n.a], depth + 1);
                }
                return n.a < slot_ty_.size() ? slot_ty_[n.a] : Ty::Any;
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
// The functions one compile reaches
// ---------------------------------------------------------------------------

/// The other global functions a compilation may call, and the analysis of each.
///
/// The tier used to compile exactly one Dream-level call: this function calling
/// itself. Everything else refused the whole function, which is why a fused
/// `collatz` -- a loop whose body is `acc + collatz_steps x` -- ran interpreted
/// while `collatz_steps` beside it was compiled. A call to another global
/// function is now compiled the way a non-tail self call already was: the
/// callee is emitted as a function of its arguments, into the same module, and
/// called. There is no frame, no thunk and no trip through the interpreter.
///
/// The soundness argument is the one the tier already rests on. Compiled code
/// evaluates a call's arguments before making it, and doing that to an argument
/// the callee would never have forced can raise in a program that was going to
/// finish quietly -- so a function is only compiled when it forces every
/// parameter on every path. Asking that of the callee is asking exactly what
/// the tier asks of itself, which is why the test below is `Analyzer::run` and
/// not a second set of rules.
///
/// Mutual recursion makes the question circular: `f` is admissible if `g` is,
/// and `g` is admissible if `f` is. What is being asked is "nothing anywhere in
/// this closure is unsupported", which is a greatest fixpoint, so a function
/// whose own decision is still running is assumed admissible. That assumption
/// can be wrong -- the decision underneath it may come back no -- and `sound()`
/// is what catches it, rather than an argument about which cases can happen.
class PeerSet {
  public:
    PeerSet(Runtime& rt, const Image& img) : rt_(rt), img_(img) {}

    bool admit(uint32_t gi, int depth);

    /// The first reason the last `admit` failed for, threaded back to the call
    /// site that asked so it can name the peer rather than just the failure.
    const std::string& refuse() const { return refuse_; }

    /// Every call every member makes, and every call the root makes, is to a
    /// member. False means the optimistic answer above was taken somewhere and
    /// turned out wrong, and the whole compile is given up.
    bool sound(const Analysis& root) const {
        for (uint32_t gi : root.calls) {
            if (!members.count(gi)) return false;
        }
        for (const auto& entry : members) {
            for (uint32_t gi : entry.second.calls) {
                if (!members.count(gi)) return false;
            }
        }
        return true;
    }

    /// Admitted functions, by image function index.
    std::unordered_map<uint32_t, Analysis> members;

  private:
    enum class State : uint8_t { Deciding, Yes, No };
    Runtime& rt_;
    const Image& img_;
    std::unordered_map<uint32_t, State> state_;
    std::string refuse_;
};

bool PeerSet::admit(uint32_t gi, int depth) {
    if (depth > kMaxPeerDepth) {
        refuse_ = "the peer chain is deeper than kMaxPeerDepth";
        return false;
    }
    auto seen = state_.find(gi);
    if (seen != state_.end()) {
        if (seen->second == State::No) refuse_ = "the peer was refused when it was analysed";
        return seen->second != State::No;
    }
    if (members.size() >= kMaxPeers) {
        refuse_ = "the peer closure already holds kMaxPeers members";
        return false;
    }
    if (gi >= img_.func_count()) {
        refuse_ = "the callee is not an image function";
        return false;
    }

    state_[gi] = State::Deciding;
    Analysis a = Analyzer(rt_, img_, gi, this, depth + 1).run();

    // A peer is emitted as a function of its arguments and is given no frame of
    // its own. Reading a slot no parameter owns is already refused for every
    // function, peer or not, so what is left to refuse here is one thing:
    //
    // A peer may not be a loop. A compiled loop stays preemptible by writing its
    // loop-carried state back to its frame and handing control to the
    // interpreter, and a peer has no frame to write to -- so a peer that looped
    // would hold its worker for as long as the loop ran, which for a loop that
    // does not terminate is for ever. Recursion is allowed, because the machine
    // stack bounds it: past `kStackBudget` the body answers `JIT_DEEP` and the
    // interpreter takes the call over. A loop has no such bound, and that is the
    // whole of the difference. A function refused here is still compiled in its
    // own right when something enters it directly; it is only refused as
    // somebody else's callee.
    if (!a.compilable) {
        refuse_ = a.refuse.empty() ? "not compilable by this tier" : a.refuse;
    } else if (a.tail_self) {
        refuse_ = "is a loop (a peer may not be a loop)";
    }
    const bool ok = a.compilable && !a.tail_self;
    state_[gi] = ok ? State::Yes : State::No;
    if (!ok) return false;

    // Emitted with no float specialization. A peer's signature has to be the
    // same whoever calls it, and a slot carried as a raw double is a decision
    // about one function's own loop -- keeping it would mean compiling the
    // callee once per shape of argument its callers happened to have.
    a.float_slots = 0;
    a.force_first.clear();
    members.emplace(gi, std::move(a));
    return true;
}

bool peer_admits(PeerSet& peers, uint32_t gi, int depth, std::string* refuse) {
    const bool ok = peers.admit(gi, depth);
    if (!ok && refuse) *refuse = peers.refuse();
    return ok;
}


// ---------------------------------------------------------------------------
// Code generation
// ---------------------------------------------------------------------------

/// The signature of a body emitted as a function of its arguments:
/// `(proc, depth, frame, a0..an, status)`. A peer's is always all-tagged, which
/// is what lets one declaration serve every caller of it.
llvm::FunctionType* body_signature(llvm::LLVMContext& ctx, uint32_t arity, SlotSet float_slots) {
    auto* i64t = llvm::Type::getInt64Ty(ctx);
    auto* i32t = llvm::Type::getInt32Ty(ctx);
    auto* ptrt = llvm::PointerType::getUnqual(ctx);
    auto* dblt = llvm::Type::getDoubleTy(ctx);
    std::vector<llvm::Type*> params{ptrt, i32t, i64t};
    for (uint32_t i = 0; i < arity; ++i) {
        params.push_back(((float_slots >> i) & 1) ? dblt : i64t);
    }
    params.push_back(ptrt);
    return llvm::FunctionType::get(i64t, params, false);
}

/// The LLVM function emitted for each peer of one compile, by image function
/// index. Every one of them is declared before any body is written, because a
/// call to a peer may be emitted before that peer's own body is -- which is
/// what mutual recursion needs.
using PeerFns = std::unordered_map<uint32_t, llvm::Function*>;

class Emitter {
public:
    /// `peers` is the compile's declarations, shared by every emitter in it.
    /// `preset` is non-null when this emitter is writing a peer: the function
    /// already exists, because somebody may already have called it.
    Emitter(llvm::LLVMContext& ctx, llvm::Module& mod, Runtime& rt, const Image& img, uint32_t fi,
            const Analysis& a, const PeerFns* peers = nullptr, llvm::Function* preset = nullptr)
        : ctx_(ctx), mod_(mod), rt_(rt), img_(img), fi_(fi), f_(img.func(fi)),
          recurses_(a.recurses), float_slots_(a.float_slots), carried_(a.carried),
          lazy_(a.lazy), force_first_(a.force_first), binds_(a.binds), peers_(peers),
          preset_(preset), b_(ctx) {}

    llvm::Function* emit(const std::string& name);

    /// Why `emit` gave up, when it did. `failed_` records the first `fail`,
    /// so a root that refused in the analyzer and then a peer that refused in
    /// the emitter reports the peer -- but that is honest: the last body is
    /// the reason the whole closure is refused.
    const std::string& refusal() const { return failure_; }

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
    /// A call to a host native, made the way the interpreter makes one.
    JV host_call(const NativeSite& site, const Node& n);
    JV self_call(const Node& n);
    /// A saturated call to another global function, emitted beside this one.
    JV peer_call(uint32_t gi, const Node& n);
    /// What a callee is handed as its depth. One more than ours in a shape
    /// that has one, and one in the loop shape, which has no depth of its own
    /// because it makes no machine call.
    llvm::Value* callee_depth() { return depth_ ? b_.CreateAdd(depth_, i32c(1)) : i32c(1); }
    /// Take the callee's status: anything but `JIT_OK` is this invocation's
    /// answer too, and the value in hand is already the right one to return.
    JV take_call_status(llvm::Value* r, const char* what);
    JV tail_call(const Node& n, const std::vector<JV>& args);
    JV recursive_call(const std::vector<JV>& args);
    /// Spend one reduction on a call. Answers what is left, so the tail path
    /// can test it; the recursive path ignores it.
    llvm::Value* spend_reduction();
    /// Write the loop-carried parameters back to the heap frame and return
    /// `JIT_YIELD`, so the interpreter resumes the body from the top.
    void emit_yield();
    JV load_slot(uint32_t slot);
    /// A slot as it stands, unforced. What an argument the callee may never
    /// look at is made of, and the one read in this file that is not a demand.
    JV load_slot_raw(uint32_t slot);
    /// Is this parameter one every self call moves rather than evaluates?
    bool slot_is_carried(uint32_t i) const { return i < 64 && ((carried_ >> i) & 1); }
    llvm::Value* force(llvm::Value* v);
    /// `v` as a value the enclosing operation demands in normal form: a thunk
    /// is forced, a float register is already one. This is what the interpreter
    /// does when `operand_value` declines and it evaluates the operand instead,
    /// and it is needed wherever a suspension may reach a demanding site --
    /// `node` hands back a thunk for `MakeThunk` and for a `get`'s fallback.
    JV demand(JV v);

    // --- suspending: a frame of the current values, then thunks against it ---

    /// The value of `idx`, not evaluated: a thunk of the node against a
    /// snapshot of the slots it reads (already in hand for a node that is
    /// constant or one read, exactly as `thunk_for` answers the interpreter).
    JV suspend(uint32_t idx);
    /// A snapshot frame holding the current value of every slot `idx`'s whole
    /// subtree reads -- the slots a thunk of it will need when forced. The
    /// caller's own frame when none are read, which is indistinguishable.
    llvm::Value* snapshot_for(uint32_t idx);
    /// `slots`: one bit per slot to fill with the current value into a fresh
    /// frame carrying this function's closure.
    llvm::Value* make_snapshot(uint64_t slots);
    /// OR the frame slots `idx`'s subtree reads into `slots`, following a bound
    /// slot's value so the fills a thunk needs are there too.
    void collect_reachable_slots(uint32_t idx, uint64_t& slots, int depth);

    // --- the article ops: building and indexing, as the interpreter does ---

    JV make_list(const Node& n);
    JV make_array(const Node& n);
    JV make_map(const Node& n);
    JV make_closure(const Node& n);
    JV container_get(const Node& n);
    JV container_set(const Node& n);

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
    /// One bit per parameter every self call moves rather than evaluates.
    const SlotSet carried_;
    /// One bit per parameter a self call may suspend rather than evaluate:
    /// the analyzer could not prove it is forced on every path, so eager
    /// evaluation of it would be a point where the tier might raise early.
    const SlotSet lazy_;
    /// Slots to force on entry, in the order the body would have forced them.
    const std::vector<uint32_t> force_first_;
    /// Per slot, the node a `let` bound it to, or `NO_NODE`.
    const std::vector<uint32_t> binds_;
    /// The compile's peer declarations, or null when it has none.
    const PeerFns* peers_ = nullptr;
    /// Non-null when this emitter is writing a peer, into a function that was
    /// declared before any body in this module was written.
    llvm::Function* preset_ = nullptr;
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
    /// Why `failed_` was set, the first time. `failed_` guards the whole
    /// downstream walk, so the reason has to be captured where the flag is
    /// raised rather than reconstructed later.
    std::string failure_;

    /// Give up on writing this function, naming the place that did it.
    void fail(const std::string& why) {
        failed_ = true;
        if (failure_.empty()) failure_ = why;
    }

    // Declarations of the runtime helpers.
    llvm::FunctionCallee rt_force_, rt_arith_, rt_compare_, rt_float_, rt_arith_f_, rt_to_int_,
        rt_abs_, rt_floor_, rt_int_of_double_, rt_type_error_, rt_reduction_slot_,
        rt_frame_slots_, rt_frame_store_, rt_native_, rt_builtin_;
    /// Suspending and building, from a body with no frame of its own. See
    /// jit_rt.hpp; the snapshot is filled by the caller (compiled code) before
    /// anything is suspended against it.
    llvm::FunctionCallee rt_snapshot_, rt_suspend_, rt_capture_, rt_closure_, rt_string_,
        rt_cons_, rt_array_, rt_array_items_, rt_map_new_, rt_map_insert_, rt_get_, rt_set_;
};

llvm::Function* Emitter::emit(const std::string& name) {
    declare_helpers();
    // A peer is always a function of its arguments, recursive or not: it has no
    // frame, so there is nothing for the loop shape to read its slots out of.
    if (preset_) return emit_body(name);
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
    // `(proc, module, member, argc, a0..a3, out)` and the same without the
    // module, which a builtin does not have. Four argument words whatever the
    // native's arity: the unused ones are never read, and a fixed signature
    // keeps the call site free of anything to lay out in memory.
    rt_native_ = mod_.getOrInsertFunction(
        "dream_rt_native",
        llvm::FunctionType::get(
            i32_, {ptr_, i32_, i32_, i32_, i64_, i64_, i64_, i64_, ptr_}, false));
    rt_builtin_ = mod_.getOrInsertFunction(
        "dream_rt_builtin",
        llvm::FunctionType::get(i32_, {ptr_, i32_, i32_, i64_, i64_, i64_, i64_, ptr_}, false));
    rt_snapshot_ = mod_.getOrInsertFunction(
        "dream_rt_snapshot", llvm::FunctionType::get(i64_, {ptr_, i64_, i32_}, false));
    rt_suspend_ = mod_.getOrInsertFunction(
        "dream_rt_suspend", llvm::FunctionType::get(i64_, {ptr_, i32_, i64_}, false));
    rt_capture_ = mod_.getOrInsertFunction(
        "dream_rt_capture", llvm::FunctionType::get(i64_, {i64_, i32_}, false));
    rt_closure_ = mod_.getOrInsertFunction(
        "dream_rt_closure", llvm::FunctionType::get(i32_, {ptr_, i32_, i64_, ptr_}, false));
    rt_string_ = mod_.getOrInsertFunction(
        "dream_rt_string", llvm::FunctionType::get(i64_, {ptr_, i32_}, false));
    rt_cons_ = mod_.getOrInsertFunction(
        "dream_rt_cons", llvm::FunctionType::get(i64_, {ptr_, i64_, i64_}, false));
    rt_array_ = mod_.getOrInsertFunction(
        "dream_rt_array", llvm::FunctionType::get(i64_, {ptr_, i32_}, false));
    rt_array_items_ = mod_.getOrInsertFunction(
        "dream_rt_array_items", llvm::FunctionType::get(ptr_, {i64_}, false));
    rt_map_new_ = mod_.getOrInsertFunction(
        "dream_rt_map_new", llvm::FunctionType::get(i64_, {ptr_}, false));
    rt_map_insert_ = mod_.getOrInsertFunction(
        "dream_rt_map_insert",
        llvm::FunctionType::get(i64_, {ptr_, i64_, i64_, i64_}, false));
    rt_get_ = mod_.getOrInsertFunction(
        "dream_rt_get", llvm::FunctionType::get(i32_, {ptr_, i64_, i64_, i32_, ptr_}, false));
    rt_set_ = mod_.getOrInsertFunction(
        "dream_rt_set", llvm::FunctionType::get(i32_, {ptr_, i64_, i64_, i64_, ptr_}, false));
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
    // Internal, so LLVM is free to inline the first level or two of the
    // recursion into itself and to pick its own calling convention for it. A
    // peer's declaration was made before any body in this module was written,
    // so that a call to it could be emitted first.
    fn_ = preset_ ? preset_
                  : llvm::Function::Create(body_signature(ctx_, f_.arity, float_slots_),
                                           llvm::Function::InternalLinkage, name, mod_);
    proc_ = fn_->getArg(0);
    depth_ = fn_->getArg(1);
    frame_ = fn_->getArg(2);
    status_ = fn_->getArg(fn_->arg_size() - 1);
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
    // Slots past the parameters belong to `let` bindings, and a bound slot is
    // never read here: its value is written where its name is. So they are
    // allocated to keep the slot indices lining up, filled with something
    // legible, and never touched again. A slot that is neither a parameter nor
    // bound refuses the whole function (`Analyzer::run`), which is what makes
    // that true.
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
    // A peer is not erased on failure: another peer may already have emitted a
    // call to it, and a function with uses cannot be removed. Nothing is leaked
    // by leaving it -- every caller of this drops the whole module.
    if (failed_) {
        if (!preset_) fn_->eraseFromParent();
        return nullptr;
    }
    if (result.v) {
        b_.CreateStore(i32c(JIT_OK), status_);
        b_.CreateRet(box(result));
    } else if (!b_.GetInsertBlock()->getTerminator()) {
        b_.CreateUnreachable();
    }

    if (llvm::verifyFunction(*fn_, &llvm::errs())) {
        if (!preset_) fn_->eraseFromParent();
        fail("the body did not verify");
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

Emitter::JV Emitter::demand(JV v) {
    // A value already in a register as a double is a number and in normal form;
    // a tagged one may still be a suspension, so it goes through `force`, whose
    // own fast path is a compare for anything that is not a heap pointer.
    if (!v.v || v.dbl) return v;
    return tag(force(v.v));
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

Emitter::JV Emitter::load_slot_raw(uint32_t slot) {
    // No force, and nothing written back. A slot the loop carries as a double
    // is already a number and there is nothing to force about it; every other
    // slot is handed on exactly as it arrived, which may be a suspension the
    // program never demands.
    if (slot_is_dbl(slot)) return flt(b_.CreateLoad(dbl_, slots_[slot]));
    return tag(b_.CreateLoad(i64_, slots_[slot]));
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
    if (idx >= img_.node_count()) {
        std::fprintf(stderr, "JIT BAD NODE idx=%u count=%u\n", idx, img_.node_count());
        fail("bad node index");
        return none();
    }
    const Node& n = img_.node(idx);
    switch (Op(n.op)) {
        case Op::ConstInt: {
            int64_t v = img_.integer(n.a);
            if (!fixnum_fits(v)) {
                fail("an integer constant does not fit a fixnum");
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
        case Op::ConstStr:
            // The process's one copy of the constant, exactly what the
            // interpreter hands back for the same index. The same object for
            // the life of the process, which is why ConstStr suspends less
            // often than it looks as though it should.
            return tag(b_.CreateCall(rt_string_, {proc_, i32c(int(n.a))}));
        case Op::ConstAtom:
            // The atom's id is remapped once, at load, and an atom is an
            // immediate -- a constant `make_atom` of a runtime-chosen id. No
            // helper and nothing to suspend: it is a literal here the way
            // `ConstInt` is.
            return tag(i64(make_atom(rt_.image_atom(n.a))));
        case Op::Local:
            // A `let` has no thunk and no slot here: its value is written where
            // its name is read, which is here. See `Analyzer::check_bind`.
            if (n.a < binds_.size() && binds_[n.a] != NO_NODE) return node(binds_[n.a]);
            return load_slot(n.a);
        case Op::Capture:
            // A capture lives in this function's closure, and both shapes keep
            // the closure the interpreter made for the call -- the outer frame
            // in the recursive shape owns it, and captures do not change with
            // recursion depth. So the current closure is wherever `frame_`
            // points. The machine `enter`s a capture it reads, which forces it,
            // and a capture may well hold a suspension -- so this is a read and
            // then a demand, not a bare read.
            return tag(force(b_.CreateCall(rt_capture_, {frame_, i32c(int(n.a))})));
        case Op::Force: return demand(node(n.a));
        case Op::If: return conditional(n);
        case Op::Block: return block(n);
        case Op::And: case Op::Or: return logic(n);
        case Op::Neg: case Op::Not: return unary(n);
        case Op::MakeList: return make_list(n);
        case Op::MakeArray: return make_array(n);
        case Op::MakeMap: return make_map(n);
        case Op::Get: return container_get(n);
        case Op::Set: return container_set(n);
        case Op::MakeClosure: return make_closure(n);
        case Op::MakeThunk: return suspend(n.a);
        case Op::Apply: return apply(n);
        default: return binary(n);
    }
}

Emitter::JV Emitter::block(const Node& n) {
    JV last = tag(i64(UNIT));
    for (uint32_t i = 0; i < n.b; ++i) {
        uint32_t stmt = img_.kid(n.a + i);
        bool is_last = (i + 1 == n.b);
        // A `let` is not a statement here -- its value is written where its
        // name is read -- so there is nothing to emit where it stands. A block
        // whose *last* statement is one has the value a block of no statements
        // has, which is what the interpreter's `advance_block` ends with too.
        if (Op(img_.node(stmt).op) == Op::Bind) continue;
        if (!is_last && !(img_.node(stmt).flags & F_STRICT)) continue;
        JV v = node(stmt);
        if (failed_) return none();
        if (!v.v) return none();  // control transferred
        if (is_last) last = v;
    }
    return last;
}

// ---------------------------------------------------------------------------
// Suspending, and the article ops
//
// A compiled body keeps its slots in registers, so when it says "not yet" it
// has to make a frame of its own to say it against -- a snapshot of the slots
// as they stand, filled with the current register values, which is exactly
// what the interpreter's frame already is but built once per site instead of
// once per call. Everything below builds that frame and suspends against it;
// see jit_rt.hpp for what the helpers do and why allocation cannot collect.
// ---------------------------------------------------------------------------

void Emitter::collect_reachable_slots(uint32_t idx, uint64_t& slots, int depth) {
    if (depth > 256 || failed_) return;
    const Node& n = img_.node(idx);
    switch (Op(n.op)) {
        case Op::Local:
            // The slot, and anything a `let` bound to it reads: a thunk of
            // that value runs against the same snapshot, so its slots have to
            // be there too. A self-referential bind cannot appear (the
            // analyzer refuses a read before its bind), so following the chain
            // terminates of its own accord; the depth cap is belt and braces.
            if (n.a < f_.slots) slots |= uint64_t(1) << n.a;
            if (n.a >= f_.arity && n.a < binds_.size() && binds_[n.a] != NO_NODE)
                collect_reachable_slots(binds_[n.a], slots, depth + 1);
            return;
        case Op::Force: case Op::Neg: case Op::Not: case Op::MakeThunk:
            collect_reachable_slots(n.a, slots, depth + 1);
            return;
        case Op::If:
            collect_reachable_slots(n.a, slots, depth + 1);
            collect_reachable_slots(n.b, slots, depth + 1);
            if (n.c != NO_NODE) collect_reachable_slots(n.c, slots, depth + 1);
            return;
        case Op::Block:
            for (uint32_t i = 0; i < n.b; ++i)
                collect_reachable_slots(img_.kid(n.a + i), slots, depth + 1);
            return;
        case Op::And: case Op::Or:
            collect_reachable_slots(n.a, slots, depth + 1);
            collect_reachable_slots(n.b, slots, depth + 1);
            return;
        case Op::Get:
            collect_reachable_slots(n.a, slots, depth + 1);
            collect_reachable_slots(n.b, slots, depth + 1);
            if (n.c != NO_NODE) collect_reachable_slots(n.c, slots, depth + 1);
            return;
        case Op::Set:
            // A set's value is stored unforced and runs against the frame when
            // demanded, so its reads have to be covered whether or not the
            // container side read anything.
            collect_reachable_slots(n.a, slots, depth + 1);
            collect_reachable_slots(n.b, slots, depth + 1);
            collect_reachable_slots(n.c, slots, depth + 1);
            return;
        case Op::MakeClosure: {
            // What a new closure captures of *this* frame: each slot capture
            // descriptor reads that slot when the closure is built against the
            // snapshot, and a captured `let`'s value reads the same snapshot.
            if (n.a < img_.func_count()) {
                const FuncRec& f = img_.func(n.a);
                for (uint32_t i = 0; i < f.n_captures; ++i) {
                    uint32_t desc = img_.kid(f.captures_off + i);
                    if (desc & CAP_FROM_CAPTURE) continue;  // the snapshot's closure carries it
                    if (desc < f_.slots) slots |= uint64_t(1) << desc;
                    if (desc >= f_.arity && desc < binds_.size() && binds_[desc] != NO_NODE)
                        collect_reachable_slots(binds_[desc], slots, depth + 1);
                }
            }
            return;
        }
        case Op::MakeList: case Op::MakeArray:
            for (uint32_t i = 0; i < n.b; ++i)
                collect_reachable_slots(img_.kid(n.a + i), slots, depth + 1);
            return;
        case Op::MakeMap:
            for (uint32_t i = 0; i < n.b * 2; ++i)
                collect_reachable_slots(img_.kid(n.a + i), slots, depth + 1);
            return;
        case Op::Apply:
            // The callee is at `n.a` and the arguments are the kids from
            // `n.b`; a callee is usually a name (a `Global`, which reads no
            // slot) but a dynamic call's callee may be any expression, so it
            // is walked too.
            collect_reachable_slots(n.a, slots, depth + 1);
            for (uint32_t i = 0; i < n.c; ++i)
                collect_reachable_slots(img_.kid(n.b + i), slots, depth + 1);
            return;
        case Op::Bind:
            // `let slot = value` as a statement: the value is what a read of
            // the slot follows, and `binds_` already holds it.
            collect_reachable_slots(n.b, slots, depth + 1);
            return;
        case Op::Try:
            // A `try!`'s body and its handler both run against this frame --
            // the handler is entered with the error written into `n.c` and the
            // same frame. A `Try` inside a suspended subtree is run by the
            // interpreter, which supports it; the emitter never emits one.
            collect_reachable_slots(n.a, slots, depth + 1);
            collect_reachable_slots(n.b, slots, depth + 1);
            return;
        case Op::Field:
            // `mod.member`: the base is a module name in practice (no slot),
            // but `Field` may in principle index any expression.
            collect_reachable_slots(n.a, slots, depth + 1);
            return;
        case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod:
        case Op::Eq: case Op::Ne: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge:
            collect_reachable_slots(n.a, slots, depth + 1);
            collect_reachable_slots(n.b, slots, depth + 1);
            return;
        case Op::Global: case Op::Builtin:
        case Op::ConstInt: case Op::ConstFloat: case Op::ConstStr:
        case Op::ConstChar: case Op::ConstBool: case Op::ConstAtom:
        case Op::Unit: case Op::Capture: case Op::Nop: case Op::Count:
            // A name, a literal or a capture: no frame slot is read.
            return;
        default:
            // Nothing else names a frame slot. Kept so that adding an op makes
            // the omission visible here rather than a stale value at run time.
            return;
    }
}

llvm::Value* Emitter::make_snapshot(uint64_t slots) {
    if (slots == 0) return frame_;
    // The fresh frame carries this function's closure (it is built from the
    // caller's frame), and is a brand-new allocation, so the fills below need
    // no write barrier: a young frame cannot already be pointed at by an old
    // object. See `dream_rt_snapshot` and the note in jit_rt.hpp.
    llvm::Value* snap = b_.CreateCall(rt_snapshot_, {proc_, frame_, i32c(int(f_.slots))}, "snap");
    llvm::Value* base = b_.CreateCall(rt_frame_slots_, {snap}, "snap.base");
    for (uint32_t s = 0; s < f_.slots; ++s) {
        if (!((slots >> s) & 1)) continue;
        llvm::Value* v;
        if (s < f_.arity) {
            // The register this slot is carried in, boxed if it is unboxed:
            // the interpreter's frame speaks tagged values. Read, not forced,
            // exactly as `load_slot_raw` hands an argument on.
            v = box(load_slot_raw(s));
        } else if (s < binds_.size() && binds_[s] != NO_NODE) {
            // A `let`: the slot holds a suspension of the bound value, which
            // is what the interpreter's own `Bind` would have put there.
            v = b_.CreateCall(rt_suspend_, {proc_, i32c(int(binds_[s])), snap});
        } else {
            continue;  // nothing bound it; nothing legitimate reads it
        }
        b_.CreateStore(v, b_.CreateGEP(i64_, base, {i64(s)}));
    }
    return snap;
}

Emitter::JV Emitter::suspend(uint32_t idx) {
    uint64_t slots = 0;
    collect_reachable_slots(idx, slots, 0);
    llvm::Value* snap = make_snapshot(slots);
    return tag(b_.CreateCall(rt_suspend_, {proc_, i32c(int(idx)), snap}));
}

Emitter::JV Emitter::make_list(const Node& n) {
    // The interpreter builds a list literal from the last element to the
    // first, each element suspended against the frame; the elements are not
    // evaluated by the building, so the direction is unobservable, but it is
    // kept for the sake of matching the reference.
    uint64_t slots = 0;
    for (uint32_t i = 0; i < n.b; ++i)
        collect_reachable_slots(img_.kid(n.a + i), slots, 0);
    llvm::Value* snap = make_snapshot(slots);
    llvm::Value* list = i64(NIL);
    for (uint32_t i = n.b; i > 0; --i) {
        llvm::Value* head = b_.CreateCall(rt_suspend_, {proc_, i32c(int(img_.kid(n.a + i - 1))), snap});
        list = b_.CreateCall(rt_cons_, {proc_, head, list});
    }
    return tag(list);
}

Emitter::JV Emitter::make_array(const Node& n) {
    uint64_t slots = 0;
    for (uint32_t i = 0; i < n.b; ++i)
        collect_reachable_slots(img_.kid(n.a + i), slots, 0);
    llvm::Value* snap = make_snapshot(slots);
    llvm::Value* arr = b_.CreateCall(rt_array_, {proc_, i32c(int(n.b))});
    llvm::Value* items = b_.CreateCall(rt_array_items_, {arr});
    for (uint32_t i = 0; i < n.b; ++i) {
        llvm::Value* v = b_.CreateCall(rt_suspend_, {proc_, i32c(int(img_.kid(n.a + i))), snap});
        b_.CreateStore(v, b_.CreateGEP(i64_, items, {i64(i)}));
    }
    return tag(arr);
}

Emitter::JV Emitter::make_map(const Node& n) {
    // Keys are evaluated where the machine evaluates them (it has to hash
    // one), values are stored as suspensions, exactly as a map literal's
    // values are stored.
    llvm::Value* m = b_.CreateCall(rt_map_new_, {proc_});
    uint64_t slots = 0;
    for (uint32_t i = 0; i < n.b * 2; ++i)
        collect_reachable_slots(img_.kid(n.a + i), slots, 0);
    llvm::Value* snap = make_snapshot(slots);
    for (uint32_t i = 0; i < n.b; ++i) {
        JV key = demand(node(img_.kid(n.a + i * 2)));
        if (failed_ || !key.v) return none();
        llvm::Value* val =
            b_.CreateCall(rt_suspend_, {proc_, i32c(int(img_.kid(n.a + i * 2 + 1))), snap});
        m = b_.CreateCall(rt_map_insert_, {proc_, m, box(key), val});
    }
    return tag(m);
}

Emitter::JV Emitter::make_closure(const Node& n) {
    // The captures come from the current invocation's values, which only a
    // snapshot holds: the recursive shape's frame belongs to the outermost
    // invocation. A `CAP_FROM_CAPTURE` descriptor names this function's own
    // closure, which the snapshot carries from `frame_` unchanged.
    if (n.a >= img_.func_count()) {
        fail("a closure names a function that is not in the image");
        return none();
    }
    uint64_t slots = 0;
    const FuncRec& f = img_.func(n.a);
    for (uint32_t i = 0; i < f.n_captures; ++i) {
        uint32_t desc = img_.kid(f.captures_off + i);
        if (!(desc & CAP_FROM_CAPTURE) && desc < f_.slots) slots |= uint64_t(1) << desc;
    }
    llvm::Value* snap = make_snapshot(slots);
    auto* out = b_.CreateAlloca(i64_);
    llvm::Value* ok = b_.CreateCall(rt_closure_, {proc_, i32c(int(n.a)), snap, out});
    llvm::Value* v = b_.CreateLoad(i64_, out);
    auto* cont = bb("closure.ok");
    auto* raise_bb = bb("closure.raise");
    b_.CreateCondBr(b_.CreateICmpNE(ok, i32c(0)), cont, raise_bb);
    b_.SetInsertPoint(raise_bb);
    emit_raise(v);
    b_.SetInsertPoint(cont);
    return tag(v);
}

Emitter::JV Emitter::container_get(const Node& n) {
    // The container then the key, each evaluated where the machine evaluates
    // it; the fallback is the one part that runs only on a miss, and runs
    // there exactly as the machine runs it. Three answers: 1 is the value,
    // 2 is "use the `else`", 0 is an error raised in `*out`.
    JV c = demand(node(n.a));
    if (failed_ || !c.v) return none();
    JV k = demand(node(n.b));
    if (failed_ || !k.v) return none();
    auto* out = b_.CreateAlloca(i64_);
    llvm::Value* ok = b_.CreateCall(
        rt_get_, {proc_, box(c), box(k), i32c(n.c != NO_NODE ? 1 : 0), out});
    llvm::Value* v = b_.CreateLoad(i64_, out);

    auto* found_bb = bb("get.found");
    auto* missing_bb = bb("get.missing");
    auto* join_bb = bb("get.end");
    b_.CreateCondBr(b_.CreateICmpEQ(ok, i32c(1)), found_bb, missing_bb);

    // A found value is what the machine would have entered, so it is in hand
    // and tagged. The fallback path below may produce a float in a register;
    // with nothing to say the container holds one, the two paths meet as
    // tagged values and a float is boxed in its own arm.
    b_.SetInsertPoint(found_bb);
    llvm::Value* found = v;
    auto* found_end = b_.GetInsertBlock();
    b_.CreateBr(join_bb);

    b_.SetInsertPoint(missing_bb);
    llvm::Value* missv = nullptr;
    llvm::BasicBlock* miss_end = nullptr;
    if (n.c != NO_NODE) {
        auto* else_bb = bb("get.else");
        auto* raise_bb = bb("get.raise");
        b_.CreateCondBr(b_.CreateICmpEQ(ok, i32c(2)), else_bb, raise_bb);
        b_.SetInsertPoint(raise_bb);
        emit_raise(v);
        b_.SetInsertPoint(else_bb);
        JV ev = suspend(n.c);
        if (failed_ || !ev.v) return none();
        missv = box(ev);
        miss_end = b_.GetInsertBlock();
        b_.CreateBr(join_bb);
    } else {
        // Without a fallback the runtime never answers 2, so this is the
        // error path and nothing else.
        emit_raise(v);
    }
    b_.SetInsertPoint(join_bb);
    // One incoming per predecessor edge, which is `found` plus the `else` arm
    // when it fell through. The fallback may instead have raised or tail
    // called, in which case only `found` reaches the join and the phi has the
    // single incoming its single predecessor calls for.
    auto* phi = b_.CreatePHI(i64_, miss_end ? 2 : 1);
    phi->addIncoming(found, found_end);
    if (miss_end) phi->addIncoming(missv, miss_end);
    return tag(phi);
}

Emitter::JV Emitter::container_set(const Node& n) {
    // `c.[k => v]`: the container and the key are evaluated, the value is
    // stored unforced, as a map literal's values are. One answer: 1 with the
    // updated container, 0 with an error raised.
    JV c = demand(node(n.a));
    if (failed_ || !c.v) return none();
    JV k = demand(node(n.b));
    if (failed_ || !k.v) return none();
    JV val = suspend(n.c);
    if (failed_ || !val.v) return none();
    auto* out = b_.CreateAlloca(i64_);
    llvm::Value* ok = b_.CreateCall(rt_set_, {proc_, box(c), box(k), box(val), out});
    llvm::Value* v = b_.CreateLoad(i64_, out);
    auto* cont = bb("set.ok");
    auto* raise_bb = bb("set.raise");
    b_.CreateCondBr(b_.CreateICmpNE(ok, i32c(0)), cont, raise_bb);
    b_.SetInsertPoint(raise_bb);
    emit_raise(v);
    b_.SetInsertPoint(cont);
    return tag(v);
}

Emitter::JV Emitter::conditional(const Node& n) {
    JV cond = demand(node(n.a));
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
    JV lhs = demand(node(n.a));
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
    JV rhs = demand(node(n.b));
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
    JV x = demand(node(n.a));
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
    if (std::getenv("DREAM_JIT_TRACE") &&
        (n.a >= img_.node_count() || n.b >= img_.node_count()))
        std::fprintf(stderr, "binary bad op=%d a=%u b=%u c=%u count=%u\n", int(op), n.a, n.b, n.c,
                     img_.node_count());
    JV av = demand(node(n.a));
    if (failed_ || !av.v) return none();
    JV bv = demand(node(n.b));
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
    } else if (op == Op::Div || op == Op::Mod) {
        // Two fixnums and a divisor that is not zero is one machine
        // instruction. Everything else -- a zero divisor, a float on either
        // side, something that is not a number at all -- is `arith`'s, and
        // goes down the slow path below to be given the error or the float the
        // interpreter would have given.
        //
        // These two used to be the only arithmetic that did not have a fast
        // path here, on the grounds that the zero check and the integer/float
        // split were not worth duplicating. Measured, they were: a
        // ten-million-iteration loop takes 92 ms dividing where the same loop
        // multiplying takes 48, and that difference is a helper call and
        // nothing else. `collatz` pays it on every step for `n % 2` and again
        // on half of them for `n / 2`.
        llvm::Value* x = b_.CreateAShr(a, 1);
        llvm::Value* y = b_.CreateAShr(bb_, 1);
        auto* nonzero = bb("bin.divmod.nz");
        b_.CreateCondBr(b_.CreateICmpEQ(y, i64(0)), slow_bb, nonzero);
        b_.SetInsertPoint(nonzero);
        if (op == Op::Mod) {
            // A remainder is smaller than its divisor, so it is a fixnum
            // whenever the divisor was one and there is nothing to check.
            // `srem` is C's `%`, which is what `arith` computes, so the sign
            // of the answer follows the dividend in both tiers.
            fastv = b_.CreateOr(b_.CreateShl(b_.CreateSRem(x, y), 1), i64(1));
            fast_end = b_.GetInsertBlock();
            b_.CreateBr(join_bb);
        } else {
            // A quotient leaves the fixnum range in exactly one place:
            // `-2^62 / -1` is `2^62`, one past the largest. Retagging is a
            // shift left, so the overflow check the multiply path already uses
            // decides it -- `n` is a fixnum exactly when `n * 2` does not
            // overflow -- and that one case goes to `arith`, which answers the
            // float the interpreter answers.
            llvm::Value* q = b_.CreateSDiv(x, y);
            auto* shl = llvm::Intrinsic::getOrInsertDeclaration(
                &mod_, llvm::Intrinsic::smul_with_overflow, {i64_});
            llvm::Value* pair = b_.CreateCall(shl, {q, i64(2)});
            llvm::Value* shifted = b_.CreateExtractValue(pair, 0);
            llvm::Value* ovf = b_.CreateExtractValue(pair, 1);
            auto* ok_bb = bb("bin.div.ok");
            b_.CreateCondBr(ovf, slow_bb, ok_bb);
            b_.SetInsertPoint(ok_bb);
            fastv = b_.CreateOr(shifted, i64(1));
            fast_end = b_.GetInsertBlock();
            b_.CreateBr(join_bb);
        }
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
    // Asked in the same order the analyzer asked it, which is what keeps the
    // two from disagreeing about which of the two native paths a call takes.
    const NativeSite site = native_site(rt_, img_, n.a, n.c);
    if (site.ok) return host_call(site, n);
    // The analyzer admitted this call, so it is one of the two Dream-level
    // calls this tier makes: this function calling itself, or a peer. A peer's
    // callee is a `Global` naming some other function; a self call's names this
    // one, and `peers_` never holds it.
    const Node& callee = img_.node(n.a);
    if (Op(callee.op) == Op::Global && peers_) {
        const GlobalRec& g = img_.global(callee.a);
        if (g.kind == GLOBAL_FUNCTION && g.target != fi_) return peer_call(g.target, n);
    }
    return self_call(n);
}

/// The tail every call in this file shares: anything but `JIT_OK` is this
/// invocation's answer too.
///
/// `JIT_RAISED` carries the error, `JIT_DEEP` carries the placeholder a deopt
/// returns -- either way the value in hand is the one to give back, and there
/// is nothing to unwind, because a compiled body has no effects and has written
/// nothing down but its own registers.
///
/// A `JIT_DEEP` from a *peer* is worth one note. It travels up to
/// `enter_function`, which deoptimizes the function it entered -- the root of
/// this compile, not the peer that ran out of stack. That is a misdirected
/// heuristic and not a wrong answer: the root is run by the interpreter, which
/// enters the peer through `enter_function` in its own right, and *that* entry
/// deoptimizes the peer. It corrects itself after one call.
Emitter::JV Emitter::take_call_status(llvm::Value* r, const char* what) {
    auto* ok_bb = bb((std::string(what) + ".ok").c_str());
    auto* out_bb = bb((std::string(what) + ".out").c_str());
    llvm::Value* st = b_.CreateLoad(i32_, status_);
    b_.CreateCondBr(b_.CreateICmpEQ(st, i32c(JIT_OK)), ok_bb, out_bb);

    b_.SetInsertPoint(out_bb);
    b_.CreateRet(r);

    b_.SetInsertPoint(ok_bb);
    return tag(r);
}

/// A saturated call to another global function.
///
/// The callee was emitted into this module by the same code that emitted this
/// function, so the call is a machine call with the arguments in registers: no
/// frame, no thunk, and no trip through the interpreter. What licenses
/// evaluating the arguments here rather than suspending them is the callee's
/// own admission test -- it forces every parameter on every path -- and
/// `PeerSet` is where that is written down.
///
/// The frame passed along is this function's, and the callee never touches it:
/// a peer has no tail self call, so it never reaches `emit_yield`, which is the
/// only thing in a compiled body that writes to a frame. It is passed because
/// the shape takes one, not because it means anything here.
Emitter::JV Emitter::peer_call(uint32_t gi, const Node& n) {
    auto found = peers_->find(gi);
    if (found == peers_->end()) {
        fail("a peer this function was supposed to call was not emitted");
        return none();
    }
    // Every argument before the call, and left to right, which is the order the
    // interpreter's own argument list is built in.
    std::vector<JV> args(n.c);
    for (uint32_t i = 0; i < n.c; ++i) {
        args[i] = node(img_.kid(n.b + i));
        if (failed_ || !args[i].v) return none();
    }
    spend_reduction();

    std::vector<llvm::Value*> call{proc_, callee_depth(), frame_};
    // A peer's parameters are all tagged -- see `PeerSet::admit` -- so a float
    // in a register is boxed here. That is the fourth of the four crossings
    // listed under "Representation", and the only one a peer adds.
    for (JV& a : args) call.push_back(box(a));
    call.push_back(status_);
    return take_call_status(b_.CreateCall(found->second, call), "peer");
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

/// A call to a host native.
///
/// One helper call with the arguments in registers. The helper does what the
/// interpreter's `resume_native` does -- put them on the process's value stack,
/// call the native, take its answer -- so there is one implementation of what a
/// native call means and both tiers reach it.
///
/// A reduction is spent, because the interpreter spends one evaluating the
/// `Apply` node this stands for. That keeps `--stats` honest, and it is also
/// what keeps a compiled loop whose body is mostly native calls preemptible:
/// the budget falls at the same rate either tier runs it.
///
/// Every argument is evaluated before the call and left to right, which is the
/// order the interpreter builds its own argument list in. A float in a register
/// is boxed here: a native speaks tagged values and knows nothing about the
/// representation a compiled body chose.
Emitter::JV Emitter::host_call(const NativeSite& site, const Node& n) {
    std::vector<JV> args(n.c);
    for (uint32_t i = 0; i < n.c; ++i) {
        const uint32_t arg = img_.kid(n.b + i);
        // A position the native's mask does not claim is one it may never look
        // at, so what goes there is suspended, not evaluated -- exactly what
        // the interpreter does with every argument it does not force, and what
        // a position of any shape is allowed to be.
        const bool strict = i < 32 && ((site.strict_mask >> i) & 1);
        args[i] = strict ? node(arg) : suspend(arg);
        if (failed_ || !args[i].v) return none();
    }
    spend_reduction();

    std::vector<llvm::Value*> call{proc_};
    if (!site.builtin) call.push_back(i32c(int(site.module)));
    call.push_back(i32c(int(site.member)));
    call.push_back(i32c(int(n.c)));
    for (uint32_t i = 0; i < kMaxNativeArgs; ++i) {
        call.push_back(i < n.c ? box(args[i]) : i64(UNIT));
    }
    auto* out = b_.CreateAlloca(i64_);
    call.push_back(out);

    llvm::Value* ok = b_.CreateCall(site.builtin ? rt_builtin_ : rt_native_, call);
    llvm::Value* v = b_.CreateLoad(i64_, out);
    auto* cont = bb("host.ok");
    auto* raise_bb = bb("host.raise");
    b_.CreateCondBr(b_.CreateICmpNE(ok, i32c(0)), cont, raise_bb);

    b_.SetInsertPoint(raise_bb);
    emit_raise(v);

    b_.SetInsertPoint(cont);
    // In weak head normal form, which is the invariant every value in a
    // compiled body has: the helper forces a result the native handed back
    // unforced, at the point the machine would have forced it.
    return tag(v);
}

Emitter::JV Emitter::self_call(const Node& n) {
    // Evaluate every argument before touching any slot: an argument may read a
    // parameter this call is about to overwrite.
    std::vector<JV> args(n.c);
    for (uint32_t i = 0; i < n.c; ++i) {
        // A parameter the analyzer could not prove forced on every path is
        // suspended, not evaluated: evaluating it here could raise or diverge
        // where the interpreter's unforced argument never would. See
        // `Analyzer::lazy` and what it licenses.
        if (i < 64 && ((lazy_ >> i) & 1)) {
            args[i] = suspend(img_.kid(n.b + i));
            if (failed_ || !args[i].v) return none();
            continue;
        }
        // A carried parameter is moved, not evaluated: slot `i` to slot `i`,
        // whatever is in it. That is what the interpreter does with it, and it
        // is why such a parameter needs no strictness -- see `Analyzer::carried`
        // and the note in `Analyzer::run` about who has to force what.
        if (i < f_.arity && slot_is_carried(i)) {
            args[i] = load_slot_raw(i);
            continue;
        }
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
        // A slot a `let` bound holds nothing worth carrying: the interpreter
        // resumes at the top of the body and runs the `let` again.
        if (i < binds_.size() && binds_[i] != NO_NODE) continue;
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

/// A self call anywhere but tail position is a machine call, one frame deeper.
/// Its status is the callee's -- see `take_call_status`.
Emitter::JV Emitter::recursive_call(const std::vector<JV>& args) {
    spend_reduction();

    std::vector<llvm::Value*> call{proc_, callee_depth(), frame_};
    for (uint32_t i = 0; i < args.size(); ++i) {
        call.push_back(slot_is_dbl(i)
                           ? as_double(args[i], "a call carrying a float was given something else")
                           : box(args[i]));
    }
    call.push_back(status_);
    // Whatever the body computes comes back through the return register as a
    // tagged `Value`. That is why a non-tail self call is `Ty::Any`: the status
    // has to travel with it, and a `double` return would leave the error
    // nowhere to sit.
    return take_call_status(b_.CreateCall(fn_, call), "call");
}

/// Analyze `func_index` and write it, and everything it calls, into `mod`.
///
/// One module holds the whole closure: the root under `name`, and one internal
/// function per peer. They are all declared before any of them is written,
/// because a call to a peer may be emitted before that peer's own body is --
/// which is what makes mutual recursion between two compiled functions work.
///
/// Answers the root's function, or null if anything in the closure could not be
/// written, in which case the caller drops the module whole. `refuse` receives
/// the first reason the closure could not be written, for the report.
llvm::Function* emit_closure(llvm::LLVMContext& ctx, llvm::Module& mod, Runtime& rt,
                             const Image& img, uint32_t func_index, const std::string& name,
                             std::string* refuse = nullptr) {
    auto fail = [&](const std::string& why) {
        if (refuse) *refuse = why;
        return nullptr;
    };
    PeerSet peers(rt, img);
    Analysis root = Analyzer(rt, img, func_index, &peers, 0).run();
    if (!root.compilable)
        return fail(root.refuse.empty() ? "not compilable by this tier" : root.refuse);
    if (!peers.sound(root))
        return fail("a call names a global outside the peer closure (`PeerSet::sound`)");
    PeerFns fns;
    for (const auto& member : peers.members) {
        fns[member.first] = llvm::Function::Create(
            body_signature(ctx, img.func(member.first).arity, 0),
            llvm::Function::InternalLinkage, "dream_peer_" + std::to_string(member.first), &mod);
    }
    for (const auto& member : peers.members) {
        llvm::Function* decl = fns[member.first];
        Emitter peer(ctx, mod, rt, img, member.first, member.second, &fns, decl);
        if (!peer.emit(decl->getName().str()))
            return fail("the emitter could not write a body for peer `" +
                        func_name(img, member.first) + "`");
    }
    Emitter e(ctx, mod, rt, img, func_index, root, &fns);
    if (!(fns[func_index] = e.emit(name))) return fail(e.refusal());
    return fns[func_index];
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
    add("dream_rt_native", reinterpret_cast<void*>(&dream_rt_native));
    add("dream_rt_builtin", reinterpret_cast<void*>(&dream_rt_builtin));
    add("dream_rt_snapshot", reinterpret_cast<void*>(&dream_rt_snapshot));
    add("dream_rt_suspend", reinterpret_cast<void*>(&dream_rt_suspend));
    add("dream_rt_capture", reinterpret_cast<void*>(&dream_rt_capture));
    add("dream_rt_closure", reinterpret_cast<void*>(&dream_rt_closure));
    add("dream_rt_string", reinterpret_cast<void*>(&dream_rt_string));
    add("dream_rt_cons", reinterpret_cast<void*>(&dream_rt_cons));
    add("dream_rt_array", reinterpret_cast<void*>(&dream_rt_array));
    add("dream_rt_array_items", reinterpret_cast<void*>(&dream_rt_array_items));
    add("dream_rt_map_new", reinterpret_cast<void*>(&dream_rt_map_new));
    add("dream_rt_map_insert", reinterpret_cast<void*>(&dream_rt_map_insert));
    add("dream_rt_get", reinterpret_cast<void*>(&dream_rt_get));
    add("dream_rt_set", reinterpret_cast<void*>(&dream_rt_set));
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
    if (!impl_->ensure_jit(error)) return nullptr;

    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto mod = std::make_unique<llvm::Module>("dream.jit", *ctx);
    mod->setDataLayout(impl_->lljit->getDataLayout());

    std::string name = "dream_fn_" + std::to_string(func_index);
    std::string refuse;
    if (!emit_closure(*ctx, *mod, impl_->rt, img, func_index, name, &refuse)) {
        // One line per refused function, once (this is the entry that crossed
        // the threshold). `DREAM_JIT_TRACE` turns that line on, which is how
        // "what does the tier refuse and why" is asked of a real run.
        static const bool trace = std::getenv("DREAM_JIT_TRACE") != nullptr;
        if (trace) {
            std::fprintf(stderr, "; jit refused fn#%u %s: %s\n", func_index,
                         func_name(img, func_index).c_str(),
                         (refuse.empty() ? "not compilable by this tier" : refuse).c_str());
        }
        if (error) *error = refuse.empty() ? "not compilable by this tier" : refuse;
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
    if (std::getenv("DREAM_JIT_TRACE")) {
        std::fprintf(stderr, "; jit compiled fn#%u %s\n", func_index,
                     func_name(img, func_index).c_str());
    }
    if (const char* want = std::getenv("DREAM_JIT_FN")) {
        if ((uint32_t)std::atoi(want) == func_index) {
            std::string ir;
            llvm::raw_string_ostream os(ir);
            // Re-emit for a clean dump; the optimized module is already moved.
            auto c2 = std::make_unique<llvm::LLVMContext>();
            auto m2 = std::make_unique<llvm::Module>("dump", *c2);
            std::string ref;
            emit_closure(*c2, *m2, impl_->rt, img, func_index, name, &ref);
            m2->print(os, nullptr);
            std::fprintf(stderr, "%s\n", os.str().c_str());
        }
    }
    return fn;
}

std::string Jit::dump_ir(uint32_t func_index) {
    std::lock_guard<std::mutex> g(impl_->mutex);
    const Image& img = impl_->rt.image();
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto mod = std::make_unique<llvm::Module>("dream.jit", *ctx);
    std::string refuse;
    if (!emit_closure(*ctx, *mod, impl_->rt, img, func_index,
                      "dream_fn_" + std::to_string(func_index), &refuse)) {
        return "; fn#" + std::to_string(func_index) +
               " is not compilable by this tier: " +
               (refuse.empty() ? std::string("not compilable by this tier") : refuse) + "\n";
    }
    std::string out;
    llvm::raw_string_ostream os(out);
    mod->print(os, nullptr);
    return out;
}

}  // namespace dream
