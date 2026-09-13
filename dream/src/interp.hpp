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
/// machine loop. Returns false if the evaluation raised; the error is left in
/// `p.result`.
///
/// Whether that loop may *collect* is the caller's to say: see `VouchesForGc`.
bool force_whnf(Process& p, Value v, Value* out);

/// "My locals survive a collection" -- the claim that lets a nested force
/// collect.
///
/// A native that walks a lazy structure runs unbounded Dream work underneath
/// itself, and with the heap pinned that work has nowhere to put its garbage:
/// one `str.concat_all` over the compiler's own image was measured allocating
/// 354 MB of nursery, 87% of it dead, with no safepoint able to reach any of
/// it. Letting the nested loop collect is what fixes that, and the price is
/// this: a frame that holds a `Value` or an `Obj*` across a force must keep it
/// somewhere the collector rewrites -- `p.stack` or `p.pins` -- and re-read it
/// afterwards, never in a C++ local.
///
/// A vouch covers the frame that makes it and nothing below: `force_whnf`
/// consumes the flag and clears it, so a native reached through the forced
/// expression starts out unvouched like any other. A force nobody vouched for
/// raises `force_pins`, and a collection needs it at zero -- which is what
/// makes the guarantee hold for a *chain* of frames rather than just the
/// nearest one. Two unvouched frames deep, the vouched frame between them
/// still cannot collect, and that is the right answer.
///
/// Declare it at the top of the region that forces, not around each call: the
/// flag is restored on destruction, so a native that forces in a loop vouches
/// once.
struct VouchesForGc {
    Process& p;
    const bool saved;
    explicit VouchesForGc(Process& proc) : p(proc), saved(proc.force_vouched) {
        p.force_vouched = true;
    }
    ~VouchesForGc() { p.force_vouched = saved; }
    VouchesForGc(const VouchesForGc&) = delete;
    VouchesForGc& operator=(const VouchesForGc&) = delete;
};

/// One `Value` held somewhere the collector can rewrite it, for as long as the
/// C++ frame that owns it lives. The counterpart of the promise `VouchesForGc`
/// makes: what cannot stay in a local goes here and is read back with `get`.
struct Pin {
    Process& p;
    const size_t at;
    Pin(Process& proc, Value v) : p(proc), at(proc.pins.size()) { p.pins.push_back(v); }
    ~Pin() { p.pins.resize(at); }
    Value get() const { return p.pins[at]; }
    Pin(const Pin&) = delete;
    Pin& operator=(const Pin&) = delete;
};

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
