// The runtime interface JIT-compiled code calls back into.
//
// Compiled code handles the fixnum fast paths inline and calls these for
// everything else, so the two tiers agree on semantics by construction: the
// slow paths are literally the interpreter's.

#pragma once

#include <cstdint>

#include "value.hpp"

namespace dream {
class Process;
}

extern "C" {

/// Force to weak head normal form. Returns 1 on success, 0 if it raised (in
/// which case *out holds the error).
int dream_rt_force(dream::Process* p, dream::Value v, dream::Value* out);

/// Arithmetic and comparison slow paths: floats, strings, overflow, and the
/// type errors. `op` is the image opcode.
int dream_rt_arith(dream::Process* p, int32_t op, dream::Value a, dream::Value b, dream::Value* out);
int dream_rt_compare(dream::Process* p, int32_t op, dream::Value a, dream::Value b, dream::Value* out);

/// Allocate a float box. Allocation itself never collects -- a collection runs
/// only at a safepoint -- so this is safe to call with the compiled frame's
/// values in machine registers, which is what makes an unboxed loop able to box
/// its answer on the way out.
dream::Value dream_rt_float(dream::Process* p, double d);

/// Arithmetic whose result is known to be a float, answered as a raw double.
///
/// The unboxed path takes this when one of its operands is not statically a
/// float and turns out at runtime not to be a number in hand either. `arith`
/// then either raises -- which is the usual outcome, and the error is the same
/// one the interpreter would have produced -- or answers a number, because an
/// operation with a float box on one side of it and a number on the other is
/// the double path in `arith` and nothing else. So the result unboxes, and the
/// caller stays in doubles rather than having to rejoin two representations.
///
/// Returns 1 with the double in `*out`, or 0 with the error in `*err`.
int dream_rt_arith_f(dream::Process* p, int32_t op, dream::Value a, dream::Value b, double* out,
                     dream::Value* err);

/// The three numeric natives whose answer is an integer or keeps the shape of
/// its argument, for the case compiled code did not specialize: `to_int`,
/// `math.floor` and `math.abs`. Each is the native's own body. Returns 1 with
/// the value in `*out`, or 0 with the error there instead.
int dream_rt_to_int(dream::Process* p, dream::Value v, dream::Value* out);
int dream_rt_floor(dream::Process* p, dream::Value v, dream::Value* out);
int dream_rt_abs(dream::Process* p, dream::Value v, dream::Value* out);

/// `make_integer` of a double already in a register: the tail of `to_int` and
/// of `math.floor` when compiled code has the number unboxed. Cannot raise.
dream::Value dream_rt_int_of_double(dream::Process* p, double d);

/// Raise a type error with a fixed message, for the shapes compiled code
/// rejects inline (a non-bool condition, say).
dream::Value dream_rt_type_error(dream::Process* p, const char* message);

/// The address of the process's reduction counter, fetched once on entry so
/// the loop back-edge can decrement it without a call.
int64_t* dream_rt_reduction_slot(dream::Process* p);

/// Slots of the frame, for writing loop-carried values back when yielding.
dream::Value* dream_rt_frame_slots(dream::Value frame);

/// Store into a frame slot with the write barrier: the yield spill is the one
/// place compiled code writes a possibly-young value into a possibly-old
/// frame, and the next minor collection has to know the edge exists.
void dream_rt_frame_store(dream::Process* p, dream::Value frame, uint32_t index, dream::Value v);

/// Call a host native the way the interpreter calls one.
///
/// The two shapes a native comes in are the two entry points: a *builtin* is an
/// immediate carrying an index into a static table, and a *member* is one
/// function of one host module -- `std.native`'s `str_byte`, `std.math`'s
/// `sqrt` -- named by where it sits rather than by a pointer, so that the
/// callee value this process already built for it is what gets called.
///
/// Arguments arrive in registers and are moved onto the process's value stack,
/// which is where a native expects to find them and where the collector can see
/// them. Four is the whole of it: a native taking more is not admitted, so the
/// signature is fixed and nothing has to be spilled through memory at the call
/// site. Unused positions are `UNIT` and are never read, because `argc` says
/// how many there are.
///
/// Returns 1 with the value in `*out`, or 0 with the error there instead.
/// Whichever it is, the value is in weak head normal form: a native that hands
/// back something it found without forcing it (`NativeOutcome::Enter`) is
/// forced here, which is exactly where the machine forces one too.
int dream_rt_native(dream::Process* p, uint32_t module_index, uint32_t member_index, uint32_t argc,
                    dream::Value a0, dream::Value a1, dream::Value a2, dream::Value a3,
                    dream::Value* out);
int dream_rt_builtin(dream::Process* p, uint32_t builtin_id, uint32_t argc, dream::Value a0,
                     dream::Value a1, dream::Value a2, dream::Value a3, dream::Value* out);

// ---------------------------------------------------------------------------
// Suspending, from code that has no frame
//
// A compiled body keeps its slots in machine registers, which is what compiling
// it is for -- and it is also the reason it could not, until these existed, say
// "not yet". A thunk is a node and a *frame*, and the frame the interpreter made
// for this call holds whatever it held when the call began: in the loop shape
// the loop has since overwritten its registers, and in the recursive shape the
// frame belongs to the outermost invocation and not to this one.
//
// So compiled code makes a frame of its own when it needs one -- a snapshot of
// the slots as they stand -- and suspends against that. Which is exactly what
// the interpreter does, because a snapshot filled with this invocation's values
// *is* this invocation's frame; it is only built later and built once per site
// rather than once per call.
// ---------------------------------------------------------------------------

/// A frame with this function's closure and `nslots` empty slots, for the caller
/// to fill. Unfilled slots read as "not bound yet", which is what a slot no
/// `let` reached means.
dream::Value dream_rt_snapshot(dream::Process* p, dream::Value frame, uint32_t nslots);

/// `thunk_for`: the value of `node` against `frame` without evaluating it. A
/// node already in normal form answers itself, so a constant or a slot costs no
/// allocation here any more than it does in the interpreter.
dream::Value dream_rt_suspend(dream::Process* p, uint32_t node, dream::Value frame);

/// A capture of the closure this frame belongs to, unforced.
dream::Value dream_rt_capture(dream::Value frame, uint32_t index);

/// A closure for image function `func_index`, capturing from `frame`. Returns 1
/// with the closure in `*out`, or 0 with the error there -- which happens only
/// for a capture descriptor the image should never have contained.
int dream_rt_closure(dream::Process* p, uint32_t func_index, dream::Value frame,
                     dream::Value* out);

// ---------------------------------------------------------------------------
// Building
//
// Allocation never collects -- a collection runs only at a safepoint, and an
// allocation passes through none -- so a compiled body may build. What it may
// not do is *forget*: every object below is filled by the caller immediately
// after it is made, with nothing in between that could move it.
// ---------------------------------------------------------------------------

/// The string constant at `index`, from this process's cache.
dream::Value dream_rt_string(dream::Process* p, uint32_t index);

dream::Value dream_rt_cons(dream::Process* p, dream::Value head, dream::Value tail);
/// An array of `len` slots, all "not bound yet" until the caller writes them.
dream::Value dream_rt_array(dream::Process* p, uint32_t len);
/// The elements of an array, for the caller to fill. No write barrier: an array
/// big enough to be born old is remembered whole at birth (`alloc_bare`), and a
/// young one needs none.
dream::Value* dream_rt_array_items(dream::Value array);
dream::Value dream_rt_map_new(dream::Process* p);
dream::Value dream_rt_map_insert(dream::Process* p, dream::Value map, dream::Value key,
                                 dream::Value value);

/// `c.[k]`, `c.[k else d]` and `c.[k => v]`. See `jit_container_get` in
/// interp.hpp for what the three answers mean; 2 is "use the `else`", which the
/// caller emits where the machine would have evaluated it.
int dream_rt_get(dream::Process* p, dream::Value container, dream::Value key, int32_t has_else,
                 dream::Value* out);
int dream_rt_set(dream::Process* p, dream::Value container, dream::Value key, dream::Value value,
                 dream::Value* out);

}  // extern "C"
