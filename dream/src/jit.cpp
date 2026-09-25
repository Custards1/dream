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
// claim is read rather than evaluated -- `Analyzer::eagerly_safe` and
// `Emitter::lazy_operand`. Both do exactly what `thunk_for` does with the same
// expression, which is what makes them free of the question above.
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
#include "llvm/IR/MDBuilder.h"
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
//
// Which parameters are floats has two sources. The fixpoint finds the ones a
// loop's own self calls make floats -- `acc + 1.0 / k` -- and a **signature**
// names the rest: a parameter declared `:float` starts the fixpoint as Float
// rather than as Unknown (the image's `TYPE` section, `Image::float_params`).
// The second is what reaches `go (x + dx) dx (acc + x * dx)`, where no float
// literal appears and the fixpoint alone would never say `x` is one. It is a
// guess and is treated as one, because the types are gradual: a caller with no
// signature may pass an integer, and the guard that makes the fixpoint's own
// answers safe makes this one safe too. A declared float that turns out to be
// an integer costs a bail, and a run of them gives the function up
// (`Jit::note_bail`); it never costs an answer.
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
//   * **May every argument be evaluated here?** A native forces the arguments
//     its mask claims and leaves the others suspended, and compiled code has no
//     thunk to suspend one in -- so a position the mask does not claim is only
//     allowed when evaluating it is not an observable event. See `eagerly_safe`.
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

    // Match failure is a raise, which the helper reports through the ordinary
    // error status. It performs no external effect despite the bang suffix.
    if (!native_is_pure(name) && !(site.builtin && std::strcmp(name, "raise!") == 0)) return site;
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

// The lowerer's catch-all arm is `if true`, followed by a match-error
// fallback. That dead fallback must not weaken strictness or result types.
int constant_condition(const Image& img, const Node& n) {
    const Node& condition = img.node(n.a);
    return Op(condition.op) == Op::ConstBool ? int(condition.a != 0) : -1;
}

/// The ops the emitter can write. `Op::Capture` is deliberately absent:
/// a capture lives in the frame, beyond the parameters this emitter reads. The
/// emitter used to refuse unsupported ops *after* the analysis had said yes, which cost nothing while a
/// compile was one function -- the emitter gave up and the function was marked
/// rejected. It is not free now: an emitter that gives up on a peer gives up on
/// the whole closure, including a root that had nothing wrong with it. So the
/// two lists say the same thing.
bool op_is_supported(Op op) {
    switch (op) {
        case Op::ConstInt: case Op::ConstFloat: case Op::ConstBool:
        case Op::ConstChar: case Op::ConstAtom: case Op::Unit:
        case Op::SwitchAtom: case Op::SwitchHead:
        case Op::Local:
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
    /// The entry cannot force the float slots in the order the body would, so
    /// the first iteration is emitted a second time over tagged slots and only
    /// its tail call crosses into doubles. Loop shape only. See "The first
    /// iteration" above `Emitter::emit_loop`.
    bool peel = false;
    /// A peer only: this body returns its answer as a raw double, its bits in
    /// the return register. Decided by the type of the body, not by a
    /// signature -- see "Typed peers" above `PeerSet`.
    bool ret_dbl = false;
    /// A peer only: the parameters of its *typed* variant, which takes them as
    /// doubles, and whether that variant returns one. Zero when the signature
    /// declares no float parameter, and then there is no typed variant.
    SlotSet typed_slots = 0;
    bool typed_ret_dbl = false;
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
    /// For each slot, the node a `let` bound it to, or `NO_NODE`. A bound slot
    /// is never read: the expression is written where the name is. See
    /// `Analyzer::check_bind`.
    std::vector<uint32_t> binds;
};

class PeerSet;
/// `peers.admit(gi, depth)`, spelled as a free function because `Analyzer` and
/// `PeerSet` each have to name the other: deciding whether a call may be
/// compiled means analysing the callee, and analysing the callee means asking
/// the same question of everything *it* calls.
bool peer_admits(PeerSet& peers, uint32_t gi, int depth);
/// The type of a call to peer `gi` whose arguments in `float_args` are known to
/// be floats. Also a free function, for the same reason.
Ty peer_call_ty(const PeerSet& peers, uint32_t gi, SlotSet float_args);

class Analyzer {
public:
    Analyzer(Runtime& rt, const Image& img, uint32_t func_index, PeerSet* peers = nullptr,
             int peer_depth = 0)
        : img_(img), fi_(func_index), f_(img.func(func_index)), rt_(rt), peers_(peers),
          peer_depth_(peer_depth) {}

    Analysis run() {
        Analysis a;
        if (f_.slots > 64 || f_.arity == 0) return a;
        binds_.assign(f_.slots, NO_NODE);
        reads_.assign(f_.slots, 0);
        if (!check(f_.body, 0)) return a;

        // A slot no parameter owns and no `let` bound holds state that lives in
        // the frame and nowhere this can reach. Nothing should produce one --
        // `Op::Bind` is the only thing that writes such a slot, and every bind
        // in the body has just been accounted for -- so this is checked rather
        // than argued, because the argument is about the compiler and this file
        // is not.
        if (reads_beyond_params_) return a;
        if (!bindings_substitutable()) return a;

        // Before `strict_of`, which needs to know which arguments are moved
        // rather than evaluated in order not to claim them forced.
        collect_calls(f_.body, 0);
        carried_ = carried();

        SlotSet strict = strict_of(f_.body, 0);
        SlotSet params = f_.arity >= 64 ? ~SlotSet(0) : ((SlotSet(1) << f_.arity) - 1);

        // What a parameter has to be, and why the two answers differ.
        //
        // The rule the tier rests on is about *eager evaluation*: compiled code
        // evaluates a call's arguments before making the call, and doing that to
        // an argument the callee would never have forced can raise in a program
        // that was going to finish quietly. So wherever an argument is
        // evaluated, the parameter it lands in must be one the callee forces
        // anyway.
        //
        // A **peer** is entered by a caller that does exactly that, so every one
        // of its parameters must be forced on every path. That is what
        // `peer_call` cites and it cannot be weakened from here.
        //
        // The **root** is entered from a heap frame the interpreter filled, so
        // nothing about its parameters is evaluated on the way in -- the only
        // eager evaluation in the picture is its own self calls'. A parameter
        // every self call hands straight back to itself is not evaluated by one:
        // slot `i` to slot `i` moves a word and forces nothing, which is
        // precisely what the interpreter does with it (`thunk_for` of a `Local`
        // is the slot, unforced). So such a parameter needs no licence, and that
        // is what lets a loop carry a string, an array or a map it reads on one
        // path and not on the other -- `f s (i + 1) n (acc + core.str_byte s i)`
        // is the shape, and before this it was refused for `s`.
        const SlotSet licensed = peer_depth_ > 0 ? strict : (strict | carried_);
        if ((licensed & params) != params) return a;

        a.binds = binds_;
        a.carried = carried_;
        a.compilable = true;
        a.strict_params = strict;
        a.recurses = recurses_;
        a.tail_self = tail_self_;
        a.calls = calls_;
        // A parameter the signature declares `:float` starts the fixpoint as
        // one instead of as nothing. That is a guess about the value the
        // caller hands in, and the entry guards it exactly as it guards a slot
        // the fixpoint found on its own; what it adds is the loop whose self
        // calls never mention a float literal -- `go (x + dx) dx (acc + x * dx)`
        // -- where nothing else would ever say that `x` is one.
        //
        // Only a parameter the body forces on every path. One it merely
        // carries may never be looked at, so the value it arrives with may be
        // a suspension nobody is allowed to force -- and a guard can only hand
        // such a call back, every time.
        const SlotSet hinted = img_.float_params(fi_) & params & strict;
        a.float_slots = float_slots(hinted, 0);
        if (a.float_slots && !entry_forces(a.float_slots, &a.force_first)) {
            // The body forces its parameters in an order the entry cannot pin
            // down, so unboxing one there would mean forcing it out of turn.
            a.force_first.clear();
            if (!recurses_) {
                // A loop runs its first iteration over tagged slots instead,
                // forcing in the body's own order, and crosses into doubles
                // at the back-edge. Nothing is forced early and nothing is
                // given up.
                a.peel = true;
            } else {
                // A recursion has no back-edge to cross at, so it keeps the
                // float slots the entry *can* reach and gives up only the
                // others -- which used to be all of them. Giving one up can
                // demote another (a slot that was a float because this one
                // was), so the fixpoint runs again with the given-up ones
                // pinned, until what is left is inside the prefix.
                std::vector<uint32_t> seq;
                force_order(f_.body, 0, seq);
                SlotSet prefix = 0;
                for (uint32_t slot : seq) {
                    if (slot < 64) prefix |= SlotSet(1) << slot;
                }
                SlotSet pinned = 0;
                SlotSet fs = a.float_slots;
                while (fs & ~prefix) {
                    pinned |= fs & ~prefix;
                    fs = float_slots(hinted & ~pinned, pinned);
                }
                a.float_slots = fs;
                if (fs && !entry_forces(fs, &a.force_first)) {
                    a.float_slots = 0;
                    a.force_first.clear();
                }
            }
        }
        if (peer_depth_ > 0) {
            // The two variants a peer may be emitted as, typed by the slots
            // each one has: every parameter tagged, and the declared floats
            // (plus whatever the fixpoint finds follows from them) as doubles.
            // Each returns a double when its body's type is Float -- a fact
            // about the body, true of the generic variant as much as the typed
            // one: `1.0 / (k * k + 1.0)` is a float whatever `k` is, or raises.
            float_slots(0, params);
            a.ret_dbl = ty(f_.body, 0) == Ty::Float;
            if (hinted) {
                a.typed_slots = float_slots(hinted, 0) & params;
                a.typed_ret_dbl = a.typed_slots && ty(f_.body, 0) == Ty::Float;
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
        // An integer too big for a fixnum is a boxed constant the emitter has
        // no way to name, so it is refused here rather than there -- same
        // reason as the two ops missing from the list above.
        if (op == Op::ConstInt && !fixnum_fits(img_.integer(n.a))) return false;
        if (op == Op::Local) {
            if (n.a >= f_.slots) return false;
            ++reads_[n.a];
            // A read of a slot that is neither a parameter nor already bound.
            // "Already" is what makes a binding that names itself, or one that
            // names a later binding, refuse the function rather than expand for
            // ever -- `check_bind` records a slot only after its value has been
            // walked.
            if (n.a >= f_.arity && binds_[n.a] == NO_NODE) reads_beyond_params_ = true;
        }

        switch (op) {
            case Op::SwitchAtom: case Op::SwitchHead:
                if (!check(n.a, depth + 1)) return false;
                for (uint32_t i = 1; i < n.c; i += 2) {
                    if (!check(img_.kid(n.b + i), depth + 1)) return false;
                }
                return check(img_.kid(n.b + n.c - 1), depth + 1);
            case Op::If: {
                const int c = constant_condition(img_, n);
                if (c >= 0) return c ? check(n.b, depth + 1)
                                    : (n.c == NO_NODE || check(n.c, depth + 1));
                return check(n.a, depth + 1) && check(n.b, depth + 1) &&
                       (n.c == NO_NODE || check(n.c, depth + 1));
            }
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
        // A host native. Its arguments are evaluated here, which for the ones
        // its strict mask claims is exactly what the interpreter does a step
        // later -- and for the ones it does not, is only allowed where
        // evaluating cannot be observed.
        const NativeSite site = native_site(rt_, img_, n.a, n.c);
        if (site.ok) {
            for (uint32_t i = 0; i < n.c; ++i) {
                const uint32_t arg = img_.kid(n.b + i);
                if (!check(arg, depth + 1)) return false;
                const bool strict = i < 32 && ((site.strict_mask >> i) & 1);
                if (!strict && !eagerly_safe(arg, 0)) return false;
            }
            return true;
        }
        // A saturated call to another global function, when that function is
        // itself something this tier can write. It is compiled by emitting the
        // callee into the same module and calling it -- so the whole closure of
        // callees is decided here, before a line of either is written.
        const uint32_t gi = peer_target(n);
        if (gi == kNoFunc || !peers_ || !peer_admits(*peers_, gi, peer_depth_)) return false;
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

    /// May compiled code evaluate this expression where the interpreter would
    /// have suspended it?
    ///
    /// Asked of the arguments a native's strict mask does *not* claim. Such an
    /// argument reaches the native as a thunk, and a compiled body has no thunk
    /// to make -- so either the value can be produced here without that being an
    /// event, or the call is not compiled at all.
    ///
    /// Two things qualify, and neither is an evaluation at all. A literal, which
    /// is a word. And a *slot*, which is read without being forced -- see
    /// `Emitter::lazy_operand`, which is what emits one, and note that this is
    /// exactly what the interpreter passes in the same position: `thunk_for` of
    /// a `Local` is the slot's value, suspended or not, handed over as it
    /// stands.
    ///
    /// A `let`'s name stands for its value, which is written where the name is
    /// read, so the question is asked of the value instead.
    ///
    /// Everything else is refused, arithmetic included: `x + 1` can raise, and
    /// raising from an argument nobody was going to look at is exactly the
    /// change this must not make.
    bool eagerly_safe(uint32_t idx, int depth) const {
        if (depth > 64) return false;
        const Node& n = img_.node(idx);
        switch (Op(n.op)) {
            case Op::ConstInt:
                return fixnum_fits(img_.integer(n.a));
            case Op::ConstFloat: case Op::ConstBool: case Op::ConstChar: case Op::Unit:
                return true;
            case Op::Local:
                if (n.a >= f_.arity) {
                    return n.a < binds_.size() && binds_[n.a] != NO_NODE &&
                           eagerly_safe(binds_[n.a], depth + 1);
                }
                return true;
            case Op::Force:
                return eagerly_safe(n.a, depth + 1);
            default:
                return false;
        }
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
        // A strict binding must run where it stands. Only parameter aliases
        // are substitutable without repeating its work; block() forces those
        // at the binding point and load_slot caches the answer.
        if (n.flags & F_STRICT) {
            // The match lowerer binds its subject strictly. A parameter alias
            // can still be substituted after forcing it at the binding point:
            // load_slot remembers the forced value. Other strict bindings need
            // real stored values and remain outside this tier.
            const Node& value = img_.node(n.b);
            if (Op(value.op) != Op::Local || value.a >= f_.arity) return false;
        }
        // Into a parameter's slot, or past the end of the frame: neither is
        // something the compiler emits, and neither has a meaning here.
        if (n.a < f_.arity || n.a >= f_.slots) return false;
        // One slot, one binding. Bound twice, a read means whichever came
        // before it, and this has no way to say which that was.
        if (binds_[n.a] != NO_NODE) return false;
        if (!check(n.b, depth + 1)) return false;
        binds_[n.a] = n.b;
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
            if (calls || cost > kMaxBindDuplication) return false;
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
            case Op::SwitchAtom: case Op::SwitchHead: {
                uint32_t c = 1 + expand_cost(n.a, depth + 1, calls);
                for (uint32_t i = 1; i < n.c; i += 2)
                    c += expand_cost(img_.kid(n.b + i), depth + 1, calls);
                return c + expand_cost(img_.kid(n.b + n.c - 1), depth + 1, calls);
            }
            case Op::If: {
                if (constant_condition(img_, n) == 1) return expand_cost(n.b, depth + 1, calls);
                if (constant_condition(img_, n) == 0)
                    return n.c == NO_NODE ? 1 : expand_cost(n.c, depth + 1, calls);
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
            case Op::Bind:
                return (n.flags & F_STRICT) ? strict_of(n.b, depth + 1) : 0;
            case Op::Local:
                // A bound slot is the expression it was bound to, written here.
                if (n.a >= f_.arity && binds_[n.a] != NO_NODE) {
                    return strict_of(binds_[n.a], depth + 1);
                }
                return SlotSet(1) << n.a;
            case Op::SwitchAtom: case Op::SwitchHead: {
                SlotSet arms = strict_of(img_.kid(n.b + n.c - 1), depth + 1);
                for (uint32_t i = 1; i < n.c; i += 2)
                    arms &= strict_of(img_.kid(n.b + i), depth + 1);
                return strict_of(n.a, depth + 1) | arms;
            }
            case Op::If:
                if (constant_condition(img_, n) == 1) return strict_of(n.b, depth + 1);
                if (constant_condition(img_, n) == 0)
                    return n.c == NO_NODE ? 0 : strict_of(n.c, depth + 1);
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

    /// One bit per slot the function can carry as a raw double. `seed` is
    /// where the fixpoint starts at Float rather than at Unknown -- the
    /// parameters a signature declared -- and `pinned` is where it starts at
    /// Any and so stays.
    SlotSet float_slots(SlotSet seed, SlotSet pinned) {
        slot_ty_.assign(f_.slots, Ty::Unknown);
        for (uint32_t i = f_.arity; i < f_.slots; ++i) slot_ty_[i] = Ty::Any;
        for (uint32_t i = 0; i < f_.arity && i < 64; ++i) {
            if ((pinned >> i) & 1) slot_ty_[i] = Ty::Any;
            else if ((seed >> i) & 1) slot_ty_[i] = Ty::Float;
        }
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
            case Op::Bind:
                return !(n.flags & F_STRICT) || force_order(n.b, depth + 1, out);
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
            case Op::SwitchAtom: case Op::SwitchHead:
                // The head may itself force work before any arm executes.
                // Keep the entry prefix to the subject; peeling handles the rest.
                force_order(n.a, depth + 1, out);
                return false;
            case Op::If: {
                if (constant_condition(img_, n) == 1) return force_order(n.b, depth + 1, out);
                if (constant_condition(img_, n) == 0)
                    return n.c == NO_NODE || force_order(n.c, depth + 1, out);
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
            case Op::SwitchAtom: case Op::SwitchHead:
                collect_calls(n.a, depth + 1);
                for (uint32_t i = 1; i < n.c; i += 2)
                    collect_calls(img_.kid(n.b + i), depth + 1);
                collect_calls(img_.kid(n.b + n.c - 1), depth + 1);
                return;
            case Op::If:
                if (constant_condition(img_, n) >= 0) {
                    uint32_t arm = constant_condition(img_, n) ? n.b : n.c;
                    if (arm != NO_NODE) collect_calls(arm, depth + 1);
                    return;
                }
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
    /// Parameters every self call moves rather than evaluates. Computed in
    /// `run` before `strict_of`, which has to know.
    SlotSet carried_ = 0;
    bool recurses_ = false;
    bool tail_self_ = false;
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
            case Op::Local:
                if (n.a >= f_.arity && n.a < binds_.size() && binds_[n.a] != NO_NODE) {
                    return ty(binds_[n.a], depth + 1);
                }
                return n.a < slot_ty_.size() ? slot_ty_[n.a] : Ty::Any;
            case Op::Force: case Op::Neg: return ty(n.a, depth + 1);
            case Op::SwitchAtom: case Op::SwitchHead: {
                Ty t = ty(img_.kid(n.b + n.c - 1), depth + 1);
                for (uint32_t i = 1; i < n.c; i += 2)
                    t = join(t, ty(img_.kid(n.b + i), depth + 1));
                return t;
            }
            case Op::If: {
                if (constant_condition(img_, n) == 1) return ty(n.b, depth + 1);
                if (constant_condition(img_, n) == 0)
                    return n.c == NO_NODE ? Ty::Any : ty(n.c, depth + 1);
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
                if (peers_) {
                    const uint32_t gi = peer_target(n);
                    if (gi != kNoFunc) {
                        SlotSet floats = 0;
                        for (uint32_t i = 0; i < n.c && i < 64; ++i) {
                            if (ty(img_.kid(n.b + i), depth + 1) == Ty::Float) floats |= SlotSet(1) << i;
                        }
                        return peer_call_ty(*peers_, gi, floats);
                    }
                }
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
// Typed peers
//
// A peer used to be all-tagged, for a reason that was right: its signature has
// to be the same for every caller, and a double parameter is a decision about
// what callers pass. So a float helper called from a compiled float loop cost
// a box for every argument going in and one for the answer coming out -- three
// allocations an iteration for `acc + term (to_float i)`, which is the whole of
// what that loop allocated.
//
// A signature is a decision that does not depend on the caller, which is what
// the missing piece was. A peer whose signature declares a float parameter is
// emitted twice: the generic variant, every parameter tagged, as before; and a
// typed one, the declared floats (and whatever the fixpoint finds follows from
// them) as doubles. A call site whose arguments are already doubles calls the
// typed variant with them in registers. One holding a tagged value checks it
// -- it is evaluated, so a float is a float box -- and calls the typed variant
// if every such argument is one and the generic variant otherwise, which is
// where an integer the types allowed an unannotated caller to pass ends up,
// and it gets exactly what the interpreter would give it.
//
// Either variant hands back a raw double when its *body* is a float -- a fact
// the analysis establishes, not the signature, and so true of untyped peers
// too: `1.0 / (k * k + 1.0)` is a float whatever `k` is, or it raises. The
// bits travel in the return register the tagged value used, and are only read
// once the status says `JIT_OK`, because on any other status that register
// holds an error or a placeholder. The caller's analysis knows which variant
// answers what (`peer_call_ty`), so a loop accumulating a peer's answer carries
// that accumulator as a double as well.
// ---------------------------------------------------------------------------

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
};

bool PeerSet::admit(uint32_t gi, int depth) {
    if (depth > kMaxPeerDepth) return false;
    auto seen = state_.find(gi);
    if (seen != state_.end()) return seen->second != State::No;
    if (members.size() >= kMaxPeers) return false;
    if (gi >= img_.func_count()) return false;

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
    const bool ok = a.compilable && !a.tail_self;
    state_[gi] = ok ? State::Yes : State::No;
    if (!ok) return false;

    // Emitted with no float specialization. A peer's signature has to be the
    // same whoever calls it, and a slot carried as a raw double is a decision
    // about one function's own loop -- keeping it would mean compiling the
    // callee once per shape of argument its callers happened to have.
    a.float_slots = 0;
    a.force_first.clear();
    a.peel = false;
    members.emplace(gi, std::move(a));
    return true;
}

bool peer_admits(PeerSet& peers, uint32_t gi, int depth) { return peers.admit(gi, depth); }

Ty peer_call_ty(const PeerSet& peers, uint32_t gi, SlotSet float_args) {
    auto found = peers.members.find(gi);
    // Still being decided -- a mutual recursion -- so nothing is known yet.
    if (found == peers.members.end()) return Ty::Any;
    const Analysis& a = found->second;
    // Arguments the analysis already knows are floats are floats at run time
    // too, so the call site's check for them passes and the typed variant is
    // the one that runs. That is what licenses answering for it here.
    if (a.typed_slots && (a.typed_slots & ~float_args) == 0) {
        return a.typed_ret_dbl ? Ty::Float : Ty::Any;
    }
    // Otherwise either variant may run, so the answer is a float only when
    // both of them give one.
    const bool both = a.ret_dbl && (!a.typed_slots || a.typed_ret_dbl);
    return both ? Ty::Float : Ty::Any;
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
struct PeerFn {
    /// Every parameter tagged. What a call gets when it cannot show the typed
    /// variant floats.
    llvm::Function* generic = nullptr;
    /// The declared floats as doubles; null when the signature declares none.
    llvm::Function* typed = nullptr;
    SlotSet typed_slots = 0;
    bool ret_dbl = false;
    bool typed_ret_dbl = false;
};
using PeerFns = std::unordered_map<uint32_t, PeerFn>;

class Emitter {
public:
    /// `peers` is the compile's declarations, shared by every emitter in it.
    /// `preset` is non-null when this emitter is writing a peer: the function
    /// already exists, because somebody may already have called it.
    Emitter(llvm::LLVMContext& ctx, llvm::Module& mod, Runtime& rt, const Image& img, uint32_t fi,
            const Analysis& a, const PeerFns* peers = nullptr, llvm::Function* preset = nullptr)
        : ctx_(ctx), mod_(mod), rt_(rt), img_(img), fi_(fi), f_(img.func(fi)),
          recurses_(a.recurses), float_slots_(a.float_slots), carried_(a.carried),
          force_first_(a.force_first), binds_(a.binds), peel_first_(a.peel),
          ret_dbl_(preset && a.ret_dbl), peers_(peers),
          preset_(preset), b_(ctx) {}

    llvm::Function* emit(const std::string& name);

private:
    /// A value in hand, in one of the two representations of "Representation"
    /// above: a tagged `Value`, or a raw `double` nobody has boxed. A null `v`
    /// means control was transferred and there is no value at all -- what a
    /// tail call leaves behind.
    struct JV {
        llvm::Value* v = nullptr;
        bool dbl = false;
        // A layout hint only: integers can overflow, and gradual callers can
        // pass other types. Every fast path still checks its operands.
        bool integer_hint = false;
    };
    static JV tag(llvm::Value* v, bool integer_hint = false) { return JV{v, false, integer_hint}; }
    static JV flt(llvm::Value* v) { return JV{v, true}; }
    static JV none() { return JV{nullptr, false}; }

    /// Does the function carry this slot as a raw double? Decided by the
    /// fixpoint in the analyzer and guarded on entry.
    /// Always false while the first iteration of a peeled loop is being
    /// written: every slot is tagged there.
    bool slot_is_dbl(uint32_t i) const { return !peeling_ && ((float_slots_ >> i) & 1); }
    /// The same question about the loop proper, whichever is being written.
    bool loop_slot_is_dbl(uint32_t i) const { return (float_slots_ >> i) & 1; }

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
    JV switch_node(const Node& n);
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
    JV take_call_status(llvm::Value* r, const char* what, bool dbl = false);
    /// A call of one variant of a peer, arguments already in hand.
    JV call_peer_variant(llvm::Function* fn, SlotSet dbl_slots, bool ret_dbl,
                         const std::vector<JV>& args);
    /// What a body hands back through the return register.
    llvm::Value* returned(JV v) {
        if (!ret_dbl_) return box(v);
        return b_.CreateBitCast(as_double(v, "a float function answered something else"), i64_);
    }
    JV tail_call(const Node& n, const std::vector<JV>& args);
    JV enter_loop(const Node& n, const std::vector<JV>& args);
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
    /// An argument in a position nobody is obliged to force: a literal, or a
    /// slot read without forcing it. `Analyzer::eagerly_safe` is what has
    /// already said this expression is one of those.
    JV lazy_operand(uint32_t idx);
    /// Is this parameter one every self call moves rather than evaluates?
    bool slot_is_carried(uint32_t i) const { return i < 64 && ((carried_ >> i) & 1); }
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
    /// One bit per parameter every self call moves rather than evaluates.
    const SlotSet carried_;
    /// Slots to force on entry, in the order the body would have forced them.
    const std::vector<uint32_t> force_first_;
    /// Per slot, the node a `let` bound it to, or `NO_NODE`.
    const std::vector<uint32_t> binds_;
    /// The loop's first iteration is written separately, over tagged slots.
    const bool peel_first_;
    /// A peer that answers a raw double, its bits in the return register.
    const bool ret_dbl_;
    /// That first iteration is what is being written right now.
    bool peeling_ = false;
    /// The loop's own slots, typed as `float_slots_` says. `slots_` is these
    /// except while the first iteration is written, when it is a set of tagged
    /// ones and the back-edge stores into these.
    std::vector<llvm::Value*> loop_slots_;
    /// Where a first iteration goes when the value it hands the loop is not
    /// the float the loop carries.
    llvm::BasicBlock* peel_bail_ = nullptr;
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

    // Declarations of the runtime helpers.
    llvm::FunctionCallee rt_force_, rt_arith_, rt_compare_, rt_float_, rt_arith_f_, rt_to_int_,
        rt_abs_, rt_floor_, rt_int_of_double_, rt_type_error_, rt_reduction_slot_,
        rt_frame_slots_, rt_frame_store_, rt_native_, rt_builtin_, rt_switch_head_;
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

    rt_switch_head_ = mod_.getOrInsertFunction(
        "dream_rt_switch_head", llvm::FunctionType::get(i32_, {ptr_, i64_, ptr_}, false));
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
}

// The first iteration.
//
// A float slot has to be a double before the loop starts, and the value the
// interpreter left in the frame is usually a suspension, so the entry forces
// the slots in the order the body would -- but only as far as that order is
// the same on every path (`Analyzer::force_order`). Past that prefix, forcing
// at the entry would be forcing out of turn, and a float slot out there used
// to cost the whole specialization.
//
// So the loop's first iteration is emitted a second time, over tagged slots,
// and runs exactly as an unspecialized body would: every slot forced on first
// read, in the body's own order. Only its back-edge (`enter_loop`) crosses
// into doubles, and it does so with values the iteration has already computed
// -- a guard, not a force. From there the loop is the specialized one.
// `integrate x hi dx acc` is the shape that needs it: the condition forces `x`
// and `hi`, and `acc` is forced by the base case on one side and by the self
// call on the other, so no prefix reaches it.
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
    loop_slots_ = slots_;
    std::vector<llvm::Value*> first_slots;
    if (peel_first_) {
        first_slots.resize(f_.slots);
        for (uint32_t i = 0; i < f_.slots; ++i) {
            first_slots[i] = b_.CreateAlloca(i64_, nullptr, "first" + std::to_string(i));
        }
    }
    llvm::BasicBlock* bail = float_slots_ ? llvm::BasicBlock::Create(ctx_, "bail", fn_) : nullptr;
    peel_bail_ = bail;
    llvm::Value* slot_base = b_.CreateCall(rt_frame_slots_, {frame_}, "slots");
    std::vector<llvm::Value*> raw(f_.slots);
    for (uint32_t i = 0; i < f_.slots; ++i) {
        raw[i] = b_.CreateLoad(i64_, b_.CreateGEP(i64_, slot_base, {i64(i)}));
    }
    if (peel_first_) {
        // Handed over exactly as they arrived; the first iteration forces
        // each on first read, as the interpreter would.
        for (uint32_t i = 0; i < f_.slots; ++i) b_.CreateStore(raw[i], first_slots[i]);
    } else {
        // Force what the body would have forced, in the order it would have
        // forced it. Empty unless a slot is carried as a double; see
        // `Analyzer::force_order`.
        for (uint32_t slot : force_first_) raw[slot] = force(raw[slot]);
        for (uint32_t i = 0; i < f_.slots; ++i) {
            b_.CreateStore(slot_is_dbl(i) ? guard_float(raw[i], bail) : raw[i], slots_[i]);
        }
    }
    reduction_slot_ = b_.CreateCall(rt_reduction_slot_, {proc_}, "reductions");

    loop_header_ = llvm::BasicBlock::Create(ctx_, "loop", fn_);
    if (peel_first_) {
        // The first iteration, over tagged slots. Its base case returns like
        // any other; its tail call stores into the loop's own slots and
        // enters the loop (`tail_call`).
        auto* first = llvm::BasicBlock::Create(ctx_, "first", fn_);
        b_.CreateBr(first);
        b_.SetInsertPoint(first);
        slots_ = first_slots;
        peeling_ = true;
        JV r = node(f_.body);
        peeling_ = false;
        slots_ = loop_slots_;
        if (failed_) {
            fn_->eraseFromParent();
            return nullptr;
        }
        if (r.v) {
            b_.CreateStore(i32c(JIT_OK), status_);
            b_.CreateRet(box(r));
        } else if (!b_.GetInsertBlock()->getTerminator()) {
            b_.CreateUnreachable();
        }
    } else {
        b_.CreateBr(loop_header_);
    }

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
        b_.CreateRet(returned(result));
    } else if (!b_.GetInsertBlock()->getTerminator()) {
        b_.CreateUnreachable();
    }

    if (llvm::verifyFunction(*fn_, &llvm::errs())) {
        if (!preset_) fn_->eraseFromParent();
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
    return tag(forced, (img_.integer_params(fi_) >> slot) & 1);
}

Emitter::JV Emitter::load_slot_raw(uint32_t slot) {
    // No force, and nothing written back. A slot the loop carries as a double
    // is already a number and there is nothing to force about it; every other
    // slot is handed on exactly as it arrived, which may be a suspension the
    // program never demands.
    if (slot_is_dbl(slot)) return flt(b_.CreateLoad(dbl_, slots_[slot]));
    return tag(b_.CreateLoad(i64_, slots_[slot]));
}

Emitter::JV Emitter::lazy_operand(uint32_t idx) {
    const Node& n = img_.node(idx);
    switch (Op(n.op)) {
        case Op::Local:
            // A `let`'s name stands for its value; a parameter stands for its
            // slot, read and not forced.
            if (n.a < binds_.size() && binds_[n.a] != NO_NODE) return lazy_operand(binds_[n.a]);
            return load_slot_raw(n.a);
        case Op::Force:
            // The compiler asked for this to be evaluated where it stands. It is
            // handed over unevaluated instead, which is what the interpreter
            // does too -- it suspends the `Force` in a thunk -- and whoever
            // demands the value gets the same answer either way.
            return lazy_operand(n.a);
        default:
            // A literal. `eagerly_safe` admits nothing else.
            return node(idx);
    }
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
            return tag(i64(make_fixnum(v)), true);
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
            return tag(i64(make_atom(rt_.image_atom(n.a))));
        case Op::SwitchAtom: case Op::SwitchHead: return switch_node(n);
        case Op::Local:
            // A `let` has no thunk and no slot here: its value is written where
            // its name is read, which is here. See `Analyzer::check_bind`.
            if (n.a < binds_.size() && binds_[n.a] != NO_NODE) return node(binds_[n.a]);
            return load_slot(n.a);
        case Op::Capture:
            // A capture lives in the frame, and a compiled body is a function
            // of its arguments. Unreachable: op_is_supported refuses it.
            failed_ = true;
            return none();
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
        // A `let` is not a statement here -- its value is written where its
        // name is read -- so there is nothing to emit where it stands. A block
        // whose *last* statement is one has the value a block of no statements
        // has, which is what the interpreter's `advance_block` ends with too.
        if (Op(img_.node(stmt).op) == Op::Bind) {
            const Node& binding = img_.node(stmt);
            if ((binding.flags & F_STRICT) && !node(binding.b).v) return none();
            continue;
        }
        if (!is_last && !(img_.node(stmt).flags & F_STRICT)) continue;
        JV v = node(stmt);
        if (failed_) return none();
        if (!v.v) return none();  // control transferred
        if (is_last) last = v;
    }
    return last;
}

Emitter::JV Emitter::switch_node(const Node& n) {
    JV subject = node(n.a);
    if (failed_ || !subject.v) return none();
    llvm::Value* key = box(subject);
    if (Op(n.op) == Op::SwitchHead) {
        auto* out = b_.CreateAlloca(i64_);
        auto* ok = b_.CreateCall(rt_switch_head_, {proc_, key, out});
        key = b_.CreateLoad(i64_, out);
        auto* ready = bb("switch.head");
        auto* raised = bb("switch.raise");
        b_.CreateCondBr(b_.CreateICmpNE(ok, i32c(0)), ready, raised);
        b_.SetInsertPoint(raised);
        emit_raise(key);
        b_.SetInsertPoint(ready);
    }
    auto* fallback = bb("switch.default");
    auto* done = bb("switch.end");
    auto* dispatch = b_.CreateSwitch(key, fallback, n.c / 2);
    std::vector<std::pair<JV, llvm::BasicBlock*>> values;
    bool doubles = true;
    // Preserve first-match semantics even for hand-built tables with repeated
    // keys (including distinct image atoms interned to the same runtime id).
    std::unordered_set<Value> keys;
    for (uint32_t i = 0; i <= n.c / 2; ++i) {
        const bool last = i == n.c / 2;
        auto* arm = last ? fallback : bb("switch.arm");
        if (!last) {
            const Node& atom = img_.node(img_.kid(n.b + 2 * i));
            Value key_value = make_atom(rt_.image_atom(atom.a));
            if (keys.insert(key_value).second)
                dispatch->addCase(llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(i64_), key_value), arm);
        }
        b_.SetInsertPoint(arm);
        JV v = node(img_.kid(n.b + (last ? n.c - 1 : 2 * i + 1)));
        if (failed_) return none();
        if (v.v) {
            doubles &= v.dbl;
            values.emplace_back(v, b_.GetInsertBlock());
        }
    }
    if (values.empty()) {
        done->eraseFromParent();
        return none();
    }
    b_.SetInsertPoint(done);
    auto* phi = b_.CreatePHI(doubles ? dbl_ : i64_, values.size());
    for (auto& [v, end] : values) {
        b_.SetInsertPoint(end);
        auto* value = doubles ? v.v : box(v);
        b_.CreateBr(done);
        phi->addIncoming(value, b_.GetInsertBlock());
    }
    b_.SetInsertPoint(done);
    return JV{phi, doubles};
}

Emitter::JV Emitter::conditional(const Node& n) {
    if (constant_condition(img_, n) == 1) return node(n.b);
    if (constant_condition(img_, n) == 0) return n.c == NO_NODE ? tag(i64(UNIT)) : node(n.c);
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

    auto* fast_bb = bb("bin.fast");
    auto* slow_bb = bb("bin.slow");
    auto* join_bb = bb("bin.end");
    auto* branch = b_.CreateCondBr(b_.CreateAnd(is_fixnum(a), is_fixnum(bb_)), fast_bb, slow_bb);
    if (av.integer_hint && bv.integer_hint)
        branch->setMetadata(llvm::LLVMContext::MD_prof,
                            llvm::MDBuilder(ctx_).createBranchWeights(2000, 1));

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
    return tag(phi, is_arith && av.integer_hint && bv.integer_hint);
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
Emitter::JV Emitter::take_call_status(llvm::Value* r, const char* what, bool dbl) {
    auto* ok_bb = bb((std::string(what) + ".ok").c_str());
    auto* out_bb = bb((std::string(what) + ".out").c_str());
    llvm::Value* st = b_.CreateLoad(i32_, status_);
    b_.CreateCondBr(b_.CreateICmpEQ(st, i32c(JIT_OK)), ok_bb, out_bb);

    b_.SetInsertPoint(out_bb);
    b_.CreateRet(r);

    b_.SetInsertPoint(ok_bb);
    // A callee that answers a double hands back its bits; anything but
    // `JIT_OK` is an error or a placeholder in the same register, which is why
    // the status is read before the bits are.
    if (dbl) return flt(b_.CreateBitCast(r, dbl_));
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
        failed_ = true;
        return none();
    }
    const PeerFn& pf = found->second;
    // Every argument before the call, and left to right, which is the order the
    // interpreter's own argument list is built in.
    std::vector<JV> args(n.c);
    for (uint32_t i = 0; i < n.c; ++i) {
        args[i] = node(img_.kid(n.b + i));
        if (failed_ || !args[i].v) return none();
    }
    spend_reduction();

    if (!pf.typed) return call_peer_variant(pf.generic, 0, pf.ret_dbl, args);

    // Which of the typed variant's parameters are not doubles already. None is
    // the case that pays: the caller computed them as floats, and they go
    // across in registers without a box on either side.
    llvm::Value* all_floats = nullptr;
    for (uint32_t i = 0; i < n.c && i < 64; ++i) {
        if (!((pf.typed_slots >> i) & 1) || args[i].dbl) continue;
        // A tagged value in hand -- every argument here is evaluated -- so a
        // float is a float box, and anything else (an integer the types let
        // an unannotated caller pass) goes to the generic variant, which does
        // with it what the interpreter would.
        llvm::Value* v = args[i].v;
        llvm::Value* ptr = is_heap_ptr(v);
        auto* here = b_.GetInsertBlock();
        auto* obj = bb("peer.obj");
        auto* joined = bb("peer.checked");
        b_.CreateCondBr(ptr, obj, joined);
        b_.SetInsertPoint(obj);
        llvm::Value* type = b_.CreateLoad(i8_, b_.CreateIntToPtr(v, ptr_));
        llvm::Value* is_float =
            b_.CreateICmpEQ(type, llvm::ConstantInt::get(i8_, uint8_t(ObjType::Float)));
        auto* obj_end = b_.GetInsertBlock();
        b_.CreateBr(joined);
        b_.SetInsertPoint(joined);
        auto* phi = b_.CreatePHI(i1_, 2);
        phi->addIncoming(b_.getFalse(), here);
        phi->addIncoming(is_float, obj_end);
        all_floats = all_floats ? b_.CreateAnd(all_floats, phi) : phi;
    }
    if (!all_floats) return call_peer_variant(pf.typed, pf.typed_slots, pf.typed_ret_dbl, args);

    auto* typed_bb = bb("peer.typed");
    auto* generic_bb = bb("peer.generic");
    auto* done_bb = bb("peer.done");
    b_.CreateCondBr(all_floats, typed_bb, generic_bb);

    b_.SetInsertPoint(typed_bb);
    std::vector<JV> unboxed = args;
    for (uint32_t i = 0; i < n.c && i < 64; ++i) {
        if (((pf.typed_slots >> i) & 1) && !args[i].dbl) unboxed[i] = flt(load_float(args[i].v));
    }
    JV t = call_peer_variant(pf.typed, pf.typed_slots, pf.typed_ret_dbl, unboxed);

    // The two variants meet here. Both answer a double when both bodies are
    // floats; otherwise the double side is boxed, which is the one allocation
    // this path can make and only when the two variants disagree.
    const bool both_dbl = pf.typed_ret_dbl && pf.ret_dbl;
    llvm::Value* tv = both_dbl ? t.v : box(t);
    auto* typed_end = b_.GetInsertBlock();
    b_.CreateBr(done_bb);

    b_.SetInsertPoint(generic_bb);
    JV g = call_peer_variant(pf.generic, 0, pf.ret_dbl, args);
    llvm::Value* gv = both_dbl ? g.v : box(g);
    auto* generic_end = b_.GetInsertBlock();
    b_.CreateBr(done_bb);

    b_.SetInsertPoint(done_bb);
    auto* phi = b_.CreatePHI(both_dbl ? dbl_ : i64_, 2);
    phi->addIncoming(tv, typed_end);
    phi->addIncoming(gv, generic_end);
    return both_dbl ? flt(phi) : tag(phi);
}

Emitter::JV Emitter::call_peer_variant(llvm::Function* fn, SlotSet dbl_slots, bool ret_dbl,
                                       const std::vector<JV>& args) {
    std::vector<llvm::Value*> call{proc_, callee_depth(), frame_};
    // A tagged parameter is handed a box -- a float in a register is boxed
    // here, the fourth crossing under "Representation" -- and a double one a
    // double. Every double one was either a double already or has just been
    // checked to be a float box and unboxed; nothing converts here.
    for (uint32_t i = 0; i < args.size(); ++i) {
        const bool dbl = i < 64 && ((dbl_slots >> i) & 1);
        call.push_back(dbl ? args[i].v : box(args[i]));
    }
    call.push_back(status_);
    return take_call_status(b_.CreateCall(fn, call), "peer", ret_dbl);
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
        // at, so what goes there is read rather than evaluated. The analyzer has
        // already refused any shape that cannot be (`eagerly_safe`).
        const bool strict = i < 32 && ((site.strict_mask >> i) & 1);
        args[i] = strict ? node(arg) : lazy_operand(arg);
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
    if (peeling_) return enter_loop(n, args);
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

/// The back-edge of a peeled first iteration: into the loop proper.
///
/// The arguments were evaluated over tagged slots, so a slot the loop carries
/// as a double is handed whatever the first iteration computed, and that is
/// only a float if the program's values are. The fixpoint's claim is about the
/// loop *given* floats on entry -- by induction, every iteration after -- so
/// this is where the induction starts, and it is a guard: a float box goes in
/// as its double, anything else hands the call back (`JIT_BAIL`). Nothing has
/// been written but registers, so the interpreter can run the call as though
/// it had never been compiled; what the first iteration forced stays forced,
/// which is the one thing it did and is what the interpreter would have done.
///
/// A carried slot is read again here rather than taken from `args`. `self_call`
/// reads it before evaluating the arguments after it, and those are usually
/// what force it -- `go cr (zr * zr + cr)` hands `cr` over as the caller's
/// thunk and then forces it one argument later -- so the value it read is the
/// suspension and the slot now holds the float. Reading the slot is still a
/// move and forces nothing.
Emitter::JV Emitter::enter_loop(const Node& n, const std::vector<JV>& args) {
    for (uint32_t i = 0; i < n.c; ++i) {
        JV a = (i < f_.arity && slot_is_carried(i)) ? load_slot_raw(i) : args[i];
        llvm::Value* v = !loop_slot_is_dbl(i) ? box(a)
                         : a.dbl              ? a.v
                                              : guard_float(a.v, peel_bail_);
        b_.CreateStore(v, loop_slots_[i]);
    }
    llvm::Value* next = spend_reduction();
    auto* yield_bb = bb("yield");
    auto* cont_bb = bb("iterate");
    b_.CreateCondBr(b_.CreateICmpSGT(next, i64(0)), cont_bb, yield_bb);

    // The yield writes the loop's slots, which are the ones just stored.
    b_.SetInsertPoint(yield_bb);
    std::vector<llvm::Value*> first = slots_;
    slots_ = loop_slots_;
    peeling_ = false;
    emit_yield();
    peeling_ = true;
    slots_ = first;

    b_.SetInsertPoint(cont_bb);
    b_.CreateBr(loop_header_);
    return none();
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
    // Whatever the body computes comes back through the return register -- a
    // tagged `Value`, or a peer's double as its bits. The analysis calls a
    // non-tail self call `Ty::Any` either way, which is the cautious answer:
    // the emitter handles whichever representation arrives.
    return take_call_status(b_.CreateCall(fn_, call), "call", ret_dbl_);
}

/// Analyze `func_index` and write it, and everything it calls, into `mod`.
///
/// One module holds the whole closure: the root under `name`, and one internal
/// function per peer. They are all declared before any of them is written,
/// because a call to a peer may be emitted before that peer's own body is --
/// which is what makes mutual recursion between two compiled functions work.
///
/// Answers the root's function, or null if anything in the closure could not be
/// written, in which case the caller drops the module whole.
llvm::Function* emit_closure(llvm::LLVMContext& ctx, llvm::Module& mod, Runtime& rt,
                             const Image& img, uint32_t func_index, const std::string& name) {
    PeerSet peers(rt, img);
    Analysis root = Analyzer(rt, img, func_index, &peers, 0).run();
    if (!root.compilable || !peers.sound(root)) return nullptr;

    PeerFns fns;
    for (const auto& member : peers.members) {
        const Analysis& a = member.second;
        const uint32_t arity = img.func(member.first).arity;
        PeerFn& pf = fns[member.first];
        pf.generic = llvm::Function::Create(
            body_signature(ctx, arity, 0), llvm::Function::InternalLinkage,
            "dream_peer_" + std::to_string(member.first), &mod);
        pf.ret_dbl = a.ret_dbl;
        if (a.typed_slots) {
            pf.typed = llvm::Function::Create(
                body_signature(ctx, arity, a.typed_slots), llvm::Function::InternalLinkage,
                "dream_peer_" + std::to_string(member.first) + ".typed", &mod);
            pf.typed_slots = a.typed_slots;
            pf.typed_ret_dbl = a.typed_ret_dbl;
        }
    }
    // Both variants are written whether or not anything calls them; the
    // optimizer drops an internal function nobody calls before code is
    // generated for it, which is where the time goes.
    for (const auto& member : peers.members) {
        const PeerFn& pf = fns[member.first];
        Emitter generic(ctx, mod, rt, img, member.first, member.second, &fns, pf.generic);
        if (!generic.emit(pf.generic->getName().str())) return nullptr;
        if (pf.typed) {
            Analysis t = member.second;
            t.float_slots = pf.typed_slots;
            t.ret_dbl = pf.typed_ret_dbl;
            Emitter typed(ctx, mod, rt, img, member.first, t, &fns, pf.typed);
            if (!typed.emit(pf.typed->getName().str())) return nullptr;
        }
    }
    return Emitter(ctx, mod, rt, img, func_index, root, &fns).emit(name);
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
    add("dream_rt_switch_head", reinterpret_cast<void*>(&dream_rt_switch_head));
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
    bails_ = std::vector<std::atomic<uint8_t>>(funcs);
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
    if (!emit_closure(*ctx, *mod, impl_->rt, img, func_index, name)) {
        if (error) *error = "not compilable by this tier";
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
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto mod = std::make_unique<llvm::Module>("dream.jit", *ctx);
    if (!emit_closure(*ctx, *mod, impl_->rt, img, func_index,
                      "dream_fn_" + std::to_string(func_index))) {
        return "; fn#" + std::to_string(func_index) +
               " is not compilable by this tier (it is interpreted)\n";
    }
    std::string out;
    llvm::raw_string_ostream os(out);
    mod->print(os, nullptr);
    return out;
}

}  // namespace dream
