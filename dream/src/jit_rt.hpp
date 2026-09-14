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

}  // extern "C"
