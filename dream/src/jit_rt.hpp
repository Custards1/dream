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

/// Allocate a float box.
dream::Value dream_rt_float(dream::Process* p, double d);

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
