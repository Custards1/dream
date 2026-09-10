// The LLVM JIT tier.
//
// Scope, and why it is drawn where it is: this compiles the strict numeric
// spine of a function -- arithmetic, comparisons, branches, and self tail
// recursion -- and leaves everything else to the interpreter. The limit is not
// laziness in general but a soundness requirement. Compiled code evaluates a
// self tail call's arguments eagerly, and doing that to an argument the callee
// would never have forced can raise an error in a program that was going to
// terminate quietly. So a function is only compiled when a strictness analysis
// proves every parameter is forced on every path. Anything else stays
// interpreted, where laziness is explicit and free.
//
// The fast paths are inline; every slow path calls the interpreter's own
// helper, so the two tiers cannot drift apart on what an operation means.

#include "jit.hpp"

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

/// Status codes a compiled body returns through its out-parameter.
constexpr int JIT_OK = 0;
constexpr int JIT_RAISED = 1;
/// The reduction budget ran out mid-loop. The loop-carried values have been
/// written back to the frame, so the interpreter can pick the iteration up
/// from the top of the body -- an OSR exit that keeps JIT code preemptible.
constexpr int JIT_YIELD = 2;

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
};

class Analyzer {
public:
    Analyzer(const Image& img, uint32_t func_index)
        : img_(img), fi_(func_index), f_(img.func(func_index)) {}

    Analysis run() {
        Analysis a;
        if (f_.slots > 64 || f_.arity == 0) return a;
        if (!check(f_.body, 0)) return a;

        SlotSet strict = strict_of(f_.body, 0);
        SlotSet params = f_.arity >= 64 ? ~SlotSet(0) : ((SlotSet(1) << f_.arity) - 1);
        // Every parameter must be forced on every path, or compiling the
        // self tail call would evaluate something the interpreter never would.
        if ((strict & params) != params) return a;

        a.compilable = true;
        a.strict_params = strict;
        return a;
    }

private:
    /// Is this node, and everything under it, something we can emit?
    bool check(uint32_t node, int depth) {
        if (depth > 256) return false;
        const Node& n = img_.node(node);
        Op op = Op(n.op);

        if (op == Op::Apply) return check_self_call(n, depth);
        if (!op_is_supported(op)) return false;

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

    /// The only call we compile is this function calling itself in tail
    /// position with a full argument list, which becomes a loop back-edge.
    bool check_self_call(const Node& n, int depth) {
        if (!(n.flags & F_TAIL)) return false;
        if (n.c != f_.arity) return false;
        const Node& callee = img_.node(n.a);
        if (Op(callee.op) != Op::Global) return false;
        const GlobalRec& g = img_.global(callee.a);
        if (g.kind != GLOBAL_FUNCTION || g.target != fi_) return false;
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
                // A self tail call forces whatever its argument expressions do.
                SlotSet s = 0;
                for (uint32_t i = 0; i < n.c; ++i) s |= strict_of(img_.kid(n.b + i), depth + 1);
                return s;
            }
            default:
                return 0;
        }
    }

    const Image& img_;
    uint32_t fi_;
    const FuncRec& f_;
};

// ---------------------------------------------------------------------------
// Code generation
// ---------------------------------------------------------------------------

class Emitter {
public:
    Emitter(llvm::LLVMContext& ctx, llvm::Module& mod, const Image& img, uint32_t fi)
        : ctx_(ctx), mod_(mod), img_(img), fi_(fi), f_(img.func(fi)), b_(ctx) {}

    llvm::Function* emit(const std::string& name);

private:
    llvm::Value* node(uint32_t idx);
    llvm::Value* binary(const Node& n);
    llvm::Value* logic(const Node& n);
    llvm::Value* unary(const Node& n);
    llvm::Value* conditional(const Node& n);
    llvm::Value* block(const Node& n);
    llvm::Value* self_call(const Node& n);
    llvm::Value* load_slot(uint32_t slot);
    llvm::Value* force(llvm::Value* v);

    /// Emit the raise path: store the error and return with JIT_RAISED.
    void emit_raise(llvm::Value* error);
    void emit_type_error(const char* message);

    llvm::Value* i64(uint64_t v) { return llvm::ConstantInt::get(i64_, v); }
    llvm::Value* is_fixnum(llvm::Value* v) {
        return b_.CreateICmpNE(b_.CreateAnd(v, i64(1)), i64(0));
    }

    llvm::LLVMContext& ctx_;
    llvm::Module& mod_;
    const Image& img_;
    uint32_t fi_;
    const FuncRec& f_;
    llvm::IRBuilder<> b_;

    llvm::Type* i64_ = nullptr;
    llvm::Type* i32_ = nullptr;
    llvm::Type* i1_ = nullptr;
    llvm::Type* ptr_ = nullptr;

    llvm::Function* fn_ = nullptr;
    llvm::Argument* proc_ = nullptr;
    llvm::Argument* frame_ = nullptr;
    llvm::Argument* status_ = nullptr;

    llvm::BasicBlock* loop_header_ = nullptr;
    std::vector<llvm::Value*> slots_;   // allocas, one per frame slot
    llvm::Value* reduction_slot_ = nullptr;
    bool failed_ = false;

    // Declarations of the runtime helpers.
    llvm::FunctionCallee rt_force_, rt_arith_, rt_compare_, rt_float_, rt_type_error_,
        rt_reduction_slot_, rt_frame_slots_, rt_frame_store_;
};

llvm::Function* Emitter::emit(const std::string& name) {
    i64_ = llvm::Type::getInt64Ty(ctx_);
    i32_ = llvm::Type::getInt32Ty(ctx_);
    i1_ = llvm::Type::getInt1Ty(ctx_);
    ptr_ = llvm::PointerType::getUnqual(ctx_);

    rt_force_ = mod_.getOrInsertFunction(
        "dream_rt_force", llvm::FunctionType::get(i32_, {ptr_, i64_, ptr_}, false));
    rt_arith_ = mod_.getOrInsertFunction(
        "dream_rt_arith", llvm::FunctionType::get(i32_, {ptr_, i32_, i64_, i64_, ptr_}, false));
    rt_compare_ = mod_.getOrInsertFunction(
        "dream_rt_compare", llvm::FunctionType::get(i32_, {ptr_, i32_, i64_, i64_, ptr_}, false));
    rt_float_ = mod_.getOrInsertFunction(
        "dream_rt_float",
        llvm::FunctionType::get(i64_, {ptr_, llvm::Type::getDoubleTy(ctx_)}, false));
    rt_type_error_ = mod_.getOrInsertFunction(
        "dream_rt_type_error", llvm::FunctionType::get(i64_, {ptr_, ptr_}, false));
    rt_reduction_slot_ = mod_.getOrInsertFunction(
        "dream_rt_reduction_slot", llvm::FunctionType::get(ptr_, {ptr_}, false));
    rt_frame_slots_ = mod_.getOrInsertFunction(
        "dream_rt_frame_slots", llvm::FunctionType::get(ptr_, {i64_}, false));
    rt_frame_store_ = mod_.getOrInsertFunction(
        "dream_rt_frame_store",
        llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), {ptr_, i64_, i32_, i64_}, false));

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
    // they are written back only when yielding.
    llvm::Value* slot_base = b_.CreateCall(rt_frame_slots_, {frame_}, "slots");
    slots_.resize(f_.slots);
    for (uint32_t i = 0; i < f_.slots; ++i) {
        slots_[i] = b_.CreateAlloca(i64_, nullptr, "slot" + std::to_string(i));
        llvm::Value* src = b_.CreateGEP(i64_, slot_base, {i64(i)});
        b_.CreateStore(b_.CreateLoad(i64_, src), slots_[i]);
    }
    reduction_slot_ = b_.CreateCall(rt_reduction_slot_, {proc_}, "reductions");

    loop_header_ = llvm::BasicBlock::Create(ctx_, "loop", fn_);
    b_.CreateBr(loop_header_);
    b_.SetInsertPoint(loop_header_);

    llvm::Value* result = node(f_.body);
    if (failed_) {
        fn_->eraseFromParent();
        return nullptr;
    }
    if (result) {
        b_.CreateStore(llvm::ConstantInt::get(i32_, JIT_OK), status_);
        b_.CreateRet(result);
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

void Emitter::emit_raise(llvm::Value* error) {
    b_.CreateStore(llvm::ConstantInt::get(i32_, JIT_RAISED), status_);
    b_.CreateRet(error);
}

void Emitter::emit_type_error(const char* message) {
    llvm::Value* msg = b_.CreateGlobalString(message);
    emit_raise(b_.CreateCall(rt_type_error_, {proc_, msg}));
}

llvm::Value* Emitter::force(llvm::Value* v) {
    // Anything that is not a heap pointer is already in normal form, and in a
    // numeric function that is the overwhelmingly common case.
    auto* need = llvm::BasicBlock::Create(ctx_, "force.slow", fn_);
    auto* done = llvm::BasicBlock::Create(ctx_, "force.done", fn_);
    auto* raise = llvm::BasicBlock::Create(ctx_, "force.raise", fn_);

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
    b_.CreateCondBr(b_.CreateICmpNE(ok, llvm::ConstantInt::get(i32_, 0)), done, raise);

    b_.SetInsertPoint(raise);
    emit_raise(forced);

    b_.SetInsertPoint(done);
    auto* phi = b_.CreatePHI(i64_, 2);
    phi->addIncoming(v, fast);
    phi->addIncoming(forced, slow);
    return phi;
}

llvm::Value* Emitter::load_slot(uint32_t slot) {
    llvm::Value* v = b_.CreateLoad(i64_, slots_[slot]);
    llvm::Value* forced = force(v);
    // Write the forced value back so a second read in the same iteration is
    // free; the thunk itself was already updated by the runtime.
    b_.CreateStore(forced, slots_[slot]);
    return forced;
}

llvm::Value* Emitter::node(uint32_t idx) {
    if (failed_) return nullptr;
    const Node& n = img_.node(idx);
    switch (Op(n.op)) {
        case Op::ConstInt: {
            int64_t v = img_.integer(n.a);
            if (!fixnum_fits(v)) {
                failed_ = true;
                return nullptr;
            }
            return i64(make_fixnum(v));
        }
        case Op::ConstBool: return i64(n.a ? TRUE_V : FALSE_V);
        case Op::ConstChar: return i64(make_char(n.a));
        case Op::Unit: return i64(UNIT);
        case Op::ConstFloat:
            return b_.CreateCall(
                rt_float_, {proc_, llvm::ConstantFP::get(llvm::Type::getDoubleTy(ctx_),
                                                         img_.real(n.a))});
        case Op::ConstAtom:
            // Atom indices are remapped through the runtime, so this needs a
            // call; the analyzer allows it because it is cheap and total.
            failed_ = true;
            return nullptr;
        case Op::Local: return load_slot(n.a);
        case Op::Capture: {
            // Captures are immutable for the life of the call.
            failed_ = true;
            return nullptr;
        }
        case Op::Force: return node(n.a);
        case Op::If: return conditional(n);
        case Op::Block: return block(n);
        case Op::And: case Op::Or: return logic(n);
        case Op::Neg: case Op::Not: return unary(n);
        case Op::Apply: return self_call(n);
        default: return binary(n);
    }
}

llvm::Value* Emitter::block(const Node& n) {
    llvm::Value* last = i64(UNIT);
    for (uint32_t i = 0; i < n.b; ++i) {
        uint32_t stmt = img_.kid(n.a + i);
        bool is_last = (i + 1 == n.b);
        if (!is_last && !(img_.node(stmt).flags & F_STRICT)) continue;
        llvm::Value* v = node(stmt);
        if (failed_) return nullptr;
        if (!v) return nullptr;  // control transferred
        if (is_last) last = v;
    }
    return last;
}

llvm::Value* Emitter::conditional(const Node& n) {
    llvm::Value* cond = node(n.a);
    if (failed_ || !cond) return nullptr;

    auto* then_bb = llvm::BasicBlock::Create(ctx_, "then", fn_);
    auto* else_bb = llvm::BasicBlock::Create(ctx_, "else", fn_);
    auto* bad_bb = llvm::BasicBlock::Create(ctx_, "if.notbool", fn_);
    auto* join_bb = llvm::BasicBlock::Create(ctx_, "endif", fn_);

    llvm::Value* is_true = b_.CreateICmpEQ(cond, i64(TRUE_V));
    llvm::Value* is_false = b_.CreateICmpEQ(cond, i64(FALSE_V));
    auto* pick = llvm::BasicBlock::Create(ctx_, "if.pick", fn_);
    b_.CreateCondBr(b_.CreateOr(is_true, is_false), pick, bad_bb);

    b_.SetInsertPoint(bad_bb);
    emit_type_error("`if` needs a bool");

    b_.SetInsertPoint(pick);
    b_.CreateCondBr(is_true, then_bb, else_bb);

    b_.SetInsertPoint(then_bb);
    llvm::Value* tv = node(n.b);
    if (failed_) return nullptr;
    llvm::BasicBlock* tb = b_.GetInsertBlock();
    if (tv) b_.CreateBr(join_bb);

    b_.SetInsertPoint(else_bb);
    llvm::Value* ev = n.c == NO_NODE ? i64(UNIT) : node(n.c);
    if (failed_) return nullptr;
    llvm::BasicBlock* eb = b_.GetInsertBlock();
    if (ev) b_.CreateBr(join_bb);

    if (!tv && !ev) {
        // Both arms tail-called; nothing reaches the join.
        join_bb->eraseFromParent();
        return nullptr;
    }
    b_.SetInsertPoint(join_bb);
    if (tv && ev) {
        auto* phi = b_.CreatePHI(i64_, 2);
        phi->addIncoming(tv, tb);
        phi->addIncoming(ev, eb);
        return phi;
    }
    return tv ? tv : ev;
}

llvm::Value* Emitter::logic(const Node& n) {
    llvm::Value* lhs = node(n.a);
    if (failed_ || !lhs) return nullptr;
    bool is_and = Op(n.b == 0 ? n.op : n.op) == Op::And;
    is_and = (Op(n.op) == Op::And);

    auto* rhs_bb = llvm::BasicBlock::Create(ctx_, "logic.rhs", fn_);
    auto* short_bb = llvm::BasicBlock::Create(ctx_, "logic.short", fn_);
    auto* bad_bb = llvm::BasicBlock::Create(ctx_, "logic.notbool", fn_);
    auto* join_bb = llvm::BasicBlock::Create(ctx_, "logic.end", fn_);

    llvm::Value* is_true = b_.CreateICmpEQ(lhs, i64(TRUE_V));
    llvm::Value* is_false = b_.CreateICmpEQ(lhs, i64(FALSE_V));
    auto* pick = llvm::BasicBlock::Create(ctx_, "logic.pick", fn_);
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
    llvm::Value* rhs = node(n.b);
    if (failed_) return nullptr;
    llvm::BasicBlock* rb = b_.GetInsertBlock();
    if (rhs) b_.CreateBr(join_bb);

    b_.SetInsertPoint(join_bb);
    auto* phi = b_.CreatePHI(i64_, 2);
    phi->addIncoming(shortcut, short_bb);
    if (rhs) phi->addIncoming(rhs, rb);
    return phi;
}

llvm::Value* Emitter::unary(const Node& n) {
    llvm::Value* v = node(n.a);
    if (failed_ || !v) return nullptr;

    if (Op(n.op) == Op::Not) {
        auto* bad_bb = llvm::BasicBlock::Create(ctx_, "not.notbool", fn_);
        auto* ok_bb = llvm::BasicBlock::Create(ctx_, "not.ok", fn_);
        llvm::Value* is_true = b_.CreateICmpEQ(v, i64(TRUE_V));
        llvm::Value* is_false = b_.CreateICmpEQ(v, i64(FALSE_V));
        b_.CreateCondBr(b_.CreateOr(is_true, is_false), ok_bb, bad_bb);
        b_.SetInsertPoint(bad_bb);
        emit_type_error("`not` needs a bool");
        b_.SetInsertPoint(ok_bb);
        return b_.CreateSelect(is_true, i64(FALSE_V), i64(TRUE_V));
    }

    // Negation: -(2x+1) as a tagged value is 2 - v, with an overflow check.
    auto* fast_bb = llvm::BasicBlock::Create(ctx_, "neg.fast", fn_);
    auto* slow_bb = llvm::BasicBlock::Create(ctx_, "neg.slow", fn_);
    auto* join_bb = llvm::BasicBlock::Create(ctx_, "neg.end", fn_);
    b_.CreateCondBr(is_fixnum(v), fast_bb, slow_bb);

    b_.SetInsertPoint(fast_bb);
    auto* ssub = llvm::Intrinsic::getOrInsertDeclaration(
        &mod_, llvm::Intrinsic::ssub_with_overflow, {i64_});
    llvm::Value* pair = b_.CreateCall(ssub, {i64(2), v});
    llvm::Value* fastv = b_.CreateExtractValue(pair, 0);
    llvm::Value* ovf = b_.CreateExtractValue(pair, 1);
    auto* fast_ok = llvm::BasicBlock::Create(ctx_, "neg.fast.ok", fn_);
    b_.CreateCondBr(ovf, slow_bb, fast_ok);
    b_.SetInsertPoint(fast_ok);
    b_.CreateBr(join_bb);

    b_.SetInsertPoint(slow_bb);
    llvm::Value* out = b_.CreateAlloca(i64_);
    llvm::Value* ok = b_.CreateCall(
        rt_arith_, {proc_, llvm::ConstantInt::get(i32_, int(Op::Sub)), i64(make_fixnum(0)), v, out});
    llvm::Value* slowv = b_.CreateLoad(i64_, out);
    auto* slow_end = b_.GetInsertBlock();
    auto* raise_bb = llvm::BasicBlock::Create(ctx_, "neg.raise", fn_);
    b_.CreateCondBr(b_.CreateICmpNE(ok, llvm::ConstantInt::get(i32_, 0)), join_bb, raise_bb);
    b_.SetInsertPoint(raise_bb);
    emit_raise(slowv);

    b_.SetInsertPoint(join_bb);
    auto* phi = b_.CreatePHI(i64_, 2);
    phi->addIncoming(fastv, fast_ok);
    phi->addIncoming(slowv, slow_end);
    return phi;
}

llvm::Value* Emitter::binary(const Node& n) {
    Op op = Op(n.op);
    llvm::Value* a = node(n.a);
    if (failed_ || !a) return nullptr;
    llvm::Value* bb = node(n.b);
    if (failed_ || !bb) return nullptr;

    const bool is_cmp = (op == Op::Lt || op == Op::Le || op == Op::Gt || op == Op::Ge ||
                         op == Op::Eq || op == Op::Ne);
    const bool inline_arith = (op == Op::Add || op == Op::Sub || op == Op::Mul);
    if (!is_cmp && !inline_arith) {
        // Division and remainder go straight to the runtime: the zero checks
        // and the integer/float split are not worth duplicating here.
        auto* out = b_.CreateAlloca(i64_);
        llvm::Value* ok = b_.CreateCall(
            rt_arith_, {proc_, llvm::ConstantInt::get(i32_, int(op)), a, bb, out});
        llvm::Value* v = b_.CreateLoad(i64_, out);
        auto* cont = llvm::BasicBlock::Create(ctx_, "arith.ok", fn_);
        auto* raise_bb = llvm::BasicBlock::Create(ctx_, "arith.raise", fn_);
        b_.CreateCondBr(b_.CreateICmpNE(ok, llvm::ConstantInt::get(i32_, 0)), cont, raise_bb);
        b_.SetInsertPoint(raise_bb);
        emit_raise(v);
        b_.SetInsertPoint(cont);
        return v;
    }

    auto* fast_bb = llvm::BasicBlock::Create(ctx_, "bin.fast", fn_);
    auto* slow_bb = llvm::BasicBlock::Create(ctx_, "bin.slow", fn_);
    auto* join_bb = llvm::BasicBlock::Create(ctx_, "bin.end", fn_);
    b_.CreateCondBr(b_.CreateAnd(is_fixnum(a), is_fixnum(bb)), fast_bb, slow_bb);

    b_.SetInsertPoint(fast_bb);
    llvm::Value* fastv = nullptr;
    llvm::BasicBlock* fast_end = nullptr;
    if (is_cmp) {
        // The tag is a left shift plus one, so tagged fixnums compare in the
        // same order as the integers they stand for -- no untagging needed.
        llvm::Value* c = nullptr;
        switch (op) {
            case Op::Lt: c = b_.CreateICmpSLT(a, bb); break;
            case Op::Le: c = b_.CreateICmpSLE(a, bb); break;
            case Op::Gt: c = b_.CreateICmpSGT(a, bb); break;
            case Op::Ge: c = b_.CreateICmpSGE(a, bb); break;
            case Op::Eq: c = b_.CreateICmpEQ(a, bb); break;
            default: c = b_.CreateICmpNE(a, bb); break;
        }
        fastv = b_.CreateSelect(c, i64(TRUE_V), i64(FALSE_V));
        fast_end = b_.GetInsertBlock();
        b_.CreateBr(join_bb);
    } else {
        llvm::Intrinsic::ID iid = op == Op::Add   ? llvm::Intrinsic::sadd_with_overflow
                                  : op == Op::Sub ? llvm::Intrinsic::ssub_with_overflow
                                                  : llvm::Intrinsic::smul_with_overflow;
        llvm::Value* lhs = a;
        llvm::Value* rhs = bb;
        if (op == Op::Mul) {
            // Untag both, multiply, then retag: 2xy+1.
            lhs = b_.CreateAShr(a, 1);
            rhs = b_.CreateAShr(bb, 1);
        } else if (op == Op::Add) {
            rhs = b_.CreateSub(bb, i64(1));   // (2x+1) + (2y+1) - 1
        } else {
            rhs = b_.CreateSub(bb, i64(1));   // (2x+1) - (2y+1) + 1 == a - (b-1)
        }
        auto* fn = llvm::Intrinsic::getOrInsertDeclaration(&mod_, iid, {i64_});
        llvm::Value* pair = b_.CreateCall(fn, {lhs, rhs});
        llvm::Value* raw = b_.CreateExtractValue(pair, 0);
        llvm::Value* ovf = b_.CreateExtractValue(pair, 1);

        auto* ok_bb = llvm::BasicBlock::Create(ctx_, "bin.fast.ok", fn_);
        if (op == Op::Mul) {
            // Retagging shifts left by one, which can overflow independently.
            auto* shl = llvm::Intrinsic::getOrInsertDeclaration(
                &mod_, llvm::Intrinsic::smul_with_overflow, {i64_});
            auto* nofast = llvm::BasicBlock::Create(ctx_, "bin.mul.retag", fn_);
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
    llvm::Value* ok = b_.CreateCall(
        helper, {proc_, llvm::ConstantInt::get(i32_, int(op)), a, bb, out});
    llvm::Value* slowv = b_.CreateLoad(i64_, out);
    auto* slow_end = b_.GetInsertBlock();
    auto* raise_bb = llvm::BasicBlock::Create(ctx_, "bin.raise", fn_);
    b_.CreateCondBr(b_.CreateICmpNE(ok, llvm::ConstantInt::get(i32_, 0)), join_bb, raise_bb);
    b_.SetInsertPoint(raise_bb);
    emit_raise(slowv);

    b_.SetInsertPoint(join_bb);
    auto* phi = b_.CreatePHI(i64_, 2);
    phi->addIncoming(fastv, fast_end);
    phi->addIncoming(slowv, slow_end);
    return phi;
}

llvm::Value* Emitter::self_call(const Node& n) {
    // Evaluate every argument before touching any slot: an argument may read a
    // parameter this call is about to overwrite.
    std::vector<llvm::Value*> args(n.c);
    for (uint32_t i = 0; i < n.c; ++i) {
        args[i] = node(img_.kid(n.b + i));
        if (failed_ || !args[i]) return nullptr;
    }
    for (uint32_t i = 0; i < n.c; ++i) b_.CreateStore(args[i], slots_[i]);

    // Spend a reduction and check the budget, so a compiled loop is just as
    // preemptible as an interpreted one.
    llvm::Value* left = b_.CreateLoad(i64_, reduction_slot_);
    llvm::Value* next = b_.CreateSub(left, i64(1));
    b_.CreateStore(next, reduction_slot_);

    auto* yield_bb = llvm::BasicBlock::Create(ctx_, "yield", fn_);
    auto* cont_bb = llvm::BasicBlock::Create(ctx_, "iterate", fn_);
    b_.CreateCondBr(b_.CreateICmpSGT(next, i64(0)), cont_bb, yield_bb);

    b_.SetInsertPoint(yield_bb);
    // Publish the loop-carried values back to the frame and hand control to
    // the interpreter, which resumes at the top of this body. The store runs
    // the write barrier: the frame may be old, the value young, and the next
    // minor collection has to know the edge exists.
    for (uint32_t i = 0; i < f_.slots; ++i) {
        b_.CreateCall(rt_frame_store_,
                      {proc_, frame_, llvm::ConstantInt::get(i32_, i),
                       b_.CreateLoad(i64_, slots_[i])});
    }
    b_.CreateStore(llvm::ConstantInt::get(i32_, JIT_YIELD), status_);
    b_.CreateRet(i64(UNIT));

    b_.SetInsertPoint(cont_bb);
    b_.CreateBr(loop_header_);
    return nullptr;  // control transferred
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
    rt.set_jit(this);
}
Jit::~Jit() { impl_->rt.set_jit(nullptr); }

void Jit::set_threshold(uint32_t calls) { impl_->threshold = calls; }
uint32_t Jit::threshold() const { return impl_->threshold; }
uint64_t Jit::compiled_count() const { return impl_->compiled_count; }

CompiledFn Jit::on_enter(uint32_t func_index) {
    // Count towards the threshold, then compile the first time it is crossed.
    // This is the slow path -- `cached_compiled` has already been consulted by
    // the interpreter on the fast path and missed, so a lock here is confined
    // to one miss per function, not one per entry.
    std::lock_guard<std::mutex> g(impl_->mutex);
    auto it = impl_->compiled.find(func_index);
    if (it != impl_->compiled.end()) return it->second;
    if (impl_->rejected.count(func_index)) return nullptr;

    if (func_index < counts_.size()) {
        uint32_t n = counts_[func_index].fetch_add(1, std::memory_order_relaxed);
        if (n + 1 < impl_->threshold) return nullptr;
    }

    CompiledFn fn = compile_locked(func_index, nullptr);
    if (!fn) {
        impl_->rejected.insert(func_index);
        return nullptr;
    }
    cached_[func_index].store(fn, std::memory_order_release);
    return fn;
}

CompiledFn Jit::compile(uint32_t func_index, std::string* error) {
    std::lock_guard<std::mutex> g(impl_->mutex);
    return compile_locked(func_index, error);
}

CompiledFn Jit::compile_locked(uint32_t func_index, std::string* error) {
    auto it = impl_->compiled.find(func_index);
    if (it != impl_->compiled.end()) return it->second;

    const Image& img = impl_->rt.image();
    Analysis a = Analyzer(img, func_index).run();
    if (!a.compilable) {
        if (error) *error = "not compilable by this tier";
        return nullptr;
    }
    if (!impl_->ensure_jit(error)) return nullptr;

    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto mod = std::make_unique<llvm::Module>("dream.jit", *ctx);
    mod->setDataLayout(impl_->lljit->getDataLayout());

    std::string name = "dream_fn_" + std::to_string(func_index);
    Emitter em(*ctx, *mod, img, func_index);
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
    Analysis a = Analyzer(img, func_index).run();
    if (!a.compilable) {
        return "; fn#" + std::to_string(func_index) +
               " is not compilable by this tier (it is interpreted)\n";
    }
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto mod = std::make_unique<llvm::Module>("dream.jit", *ctx);
    Emitter em(*ctx, *mod, img, func_index);
    if (!em.emit("dream_fn_" + std::to_string(func_index))) {
        return "; code generation failed\n";
    }
    std::string out;
    llvm::raw_string_ostream os(out);
    mod->print(os, nullptr);
    return out;
}

}  // namespace dream
