// The reduction machine.
//
// `run_process` advances a process by at most `budget` reductions and then
// returns, whatever the process is in the middle of. All of its state lives in
// the Process, so "the middle of" is a legal place to stop.

#pragma once

#include <string>

#include "process.hpp"
#include "runtime.hpp"
#include "value.hpp"

namespace dream {

/// Advance `p` until its budget runs out, it blocks, or it finishes.
void run_process(Process& p, int64_t budget);

/// Set up a fresh process to apply `callee` to `argc` values already pushed on
/// its value stack, and to halt when that returns.
void prime_apply(Process& p, Value callee, uint32_t argc);
/// Set up a fresh process to force `v` and halt.
void prime_force(Process& p, Value v);

/// Build a value for `node` without evaluating it. Constants and variable
/// references are returned directly -- allocating a thunk to wrap a value that
/// is already in normal form would cost an allocation and lose sharing.
Value thunk_for(Process& p, uint32_t node, Value frame);

/// The value of image global `index`, created on first use in this process.
Value global_value(Process& p, uint32_t index);

/// Force to weak head normal form from inside a native, by running a nested
/// machine loop. Returns false either when the evaluation raised (error in
/// `p.result`) or when a blocking native inside the forced value set
/// `p.park_requested`. Callers that return NativeResult should use
/// `force_failed(p)` rather than `NativeResult::raise(p.result)` so that a
/// park is correctly propagated as a Block, not a spurious error.
bool force_whnf(Process& p, Value v, Value* out);

/// Return the right NativeResult after force_whnf (or force_deep / stringify)
/// returned false: Block if a blocking native set park_requested, Raise
/// otherwise.
inline NativeResult force_failed(Process& p) {
    if (p.park_requested) return NativeResult::block();
    return NativeResult::raise(p.result);
}

/// Force a value and everything reachable through it. Used by printing.
bool force_deep(Process& p, Value v, Value* out);

/// Render a value the way `console.print!` and `to_string` do. Forces as it
/// goes, so it can raise; returns false when it does.
bool stringify(Process& p, Value v, std::string* out);

/// A short, non-forcing description, for error messages.
std::string describe(Process& p, Value v);

Value make_integer(Process& p, int64_t v);

/// Build a callable host function. `user` is host state the native can read
/// back from its own object, which is how FFI bindings distinguish themselves.
Value make_native(Process& p, NativeFn fn, Value name, uint32_t arity,
                  uint32_t strict_mask, uint64_t user);

/// Arithmetic and comparison, exposed so JIT-compiled code can share exactly
/// the interpreter's slow paths rather than reimplementing them.
bool jit_arith(Process& p, Op op, Value a, Value b, Value* out);
bool jit_compare(Process& p, Op op, Value a, Value b, Value* out);
Value raise_error(Process& p, uint32_t kind_atom, const std::string& message);

}  // namespace dream
