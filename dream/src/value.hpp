// Value representation.
//
// A Value is one 64-bit word, tagged in its low bits:
//
//   ....1   fixnum          63-bit signed integer, value = (int64)v >> 1
//   ...000  heap pointer    8-byte aligned; 0 is the "no value" sentinel
//   ...010  immediate       subtype in bits 3..7, payload in bits 8..63
//
// Fixnums get the one-bit tag because integer arithmetic is the hot path:
// testing and untagging cost a shift each. Everything else is either an
// immediate (unit, bool, char, atom, nil) or a pointer into the owning
// process's heap.
//
// Heap objects are only ever reachable from the process that allocated them.
// Nothing here is shared between processes -- messages are copied -- so no
// object needs a lock or an atomic field.

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>

#include "dream/core.h"

namespace dream {

using Value = uint64_t;

// ---------------------------------------------------------------------------
// Tagging
// ---------------------------------------------------------------------------

inline constexpr uint64_t TAG_MASK = 7;
inline constexpr uint64_t TAG_PTR = 0;
inline constexpr uint64_t TAG_IMM = 2;

enum ImmKind : uint64_t {
    IMM_UNIT = 0,
    IMM_BOOL = 1,
    IMM_CHAR = 2,
    IMM_ATOM = 3,
    IMM_NIL = 4,   // the empty list
    IMM_BUILTIN = 5,  // payload is the builtin id from the image
};

/// The sentinel for "no value here". Never a legal Dream value.
inline constexpr Value NIL_SLOT = 0;

inline constexpr bool is_fixnum(Value v) { return (v & 1) != 0; }
inline constexpr int64_t fixnum_value(Value v) { return static_cast<int64_t>(v) >> 1; }
inline constexpr Value make_fixnum(int64_t n) {
    return (static_cast<uint64_t>(n) << 1) | 1;
}
/// Integers outside 63 bits must be boxed; the compiler emits i64 constants.
inline constexpr bool fixnum_fits(int64_t n) {
    return n >= -(int64_t(1) << 62) && n < (int64_t(1) << 62);
}

inline constexpr bool is_ptr(Value v) { return v != 0 && (v & TAG_MASK) == TAG_PTR; }
inline constexpr bool is_imm(Value v) { return (v & TAG_MASK) == TAG_IMM; }
inline constexpr uint64_t imm_kind(Value v) { return (v >> 3) & 0x1F; }
inline constexpr uint64_t imm_payload(Value v) { return v >> 8; }
inline constexpr Value make_imm(ImmKind kind, uint64_t payload) {
    return (payload << 8) | (static_cast<uint64_t>(kind) << 3) | TAG_IMM;
}

inline constexpr Value UNIT = make_imm(IMM_UNIT, 0);
inline constexpr Value NIL = make_imm(IMM_NIL, 0);
inline constexpr Value TRUE_V = make_imm(IMM_BOOL, 1);
inline constexpr Value FALSE_V = make_imm(IMM_BOOL, 0);

inline constexpr Value make_bool(bool b) { return b ? TRUE_V : FALSE_V; }
inline constexpr Value make_char(uint32_t cp) { return make_imm(IMM_CHAR, cp); }
inline constexpr Value make_atom(uint32_t idx) { return make_imm(IMM_ATOM, idx); }
inline constexpr Value make_builtin(uint32_t id) { return make_imm(IMM_BUILTIN, id); }

inline constexpr bool is_imm_kind(Value v, ImmKind k) {
    return is_imm(v) && imm_kind(v) == static_cast<uint64_t>(k);
}
inline constexpr bool is_bool(Value v) { return is_imm_kind(v, IMM_BOOL); }
inline constexpr bool is_unit(Value v) { return is_imm_kind(v, IMM_UNIT); }
inline constexpr bool is_char(Value v) { return is_imm_kind(v, IMM_CHAR); }
inline constexpr bool is_atom(Value v) { return is_imm_kind(v, IMM_ATOM); }
inline constexpr bool is_nil(Value v) { return is_imm_kind(v, IMM_NIL); }
inline constexpr bool is_builtin(Value v) { return is_imm_kind(v, IMM_BUILTIN); }
inline constexpr bool truthy(Value v) { return v == TRUE_V; }

// ---------------------------------------------------------------------------
// Heap objects
// ---------------------------------------------------------------------------

enum class ObjType : uint8_t {
    Float = 0,
    Str,
    Cons,      // lazy list cell
    Array,
    Map,
    Closure,
    Thunk,     // an unevaluated (node, frame) pair
    Blackhole, // a thunk currently being forced
    Indirect,  // an updated thunk, pointing at its result
    Pap,       // partial application: too few arguments so far
    Frame,     // an activation: slots for parameters and block bindings
    Module,
    ErrorBox,
    Pid,
    Native,    // a host function registered through the C API
    Count
};

const char* obj_type_name(ObjType t);

/// Every heap object starts with this. `bytes` is the whole allocation,
/// which is what lets the collector walk and copy without a type switch.
struct Obj {
    ObjType type;
    uint8_t gc;   // scratch for the collector: mark bit / forwarded flag
    uint16_t aux;
    uint32_t bytes;
};
static_assert(sizeof(Obj) == 8, "object headers must stay one word");

inline Obj* as_obj(Value v) { return reinterpret_cast<Obj*>(v); }
inline Value from_obj(const Obj* o) { return reinterpret_cast<Value>(o); }
inline bool is_obj(Value v, ObjType t) { return is_ptr(v) && as_obj(v)->type == t; }

struct FloatObj : Obj {
    double value;
};

struct StrObj : Obj {
    uint32_t len;
    uint32_t hash;
    char* data() { return reinterpret_cast<char*>(this + 1); }
    const char* data() const { return reinterpret_cast<const char*>(this + 1); }
};

/// A cons cell. Both fields may be thunks: `[expensive_a, expensive_b]`
/// allocates two thunks and forces neither.
struct ConsObj : Obj {
    Value head;
    Value tail;
};

struct ArrayObj : Obj {
    uint32_t len;
    uint32_t pad;
    Value* items() { return reinterpret_cast<Value*>(this + 1); }
};

/// Open-addressed map. Keys are forced (we have to hash them); values stay
/// lazy. `cap` is always a power of two.
struct MapObj : Obj {
    uint32_t count;
    uint32_t cap;
    Value* entries() { return reinterpret_cast<Value*>(this + 1); }  // k,v,k,v...
};

struct ClosureObj : Obj {
    uint32_t func;
    uint32_t ncaps;
    Value* caps() { return reinterpret_cast<Value*>(this + 1); }
};

/// A suspension. Forcing overwrites it in place with an Indirect, so every
/// sharer of the pointer sees the result -- this is what makes laziness
/// share work rather than repeat it.
struct ThunkObj : Obj {
    uint32_t node;
    uint32_t pad;
    Value frame;
};

struct IndirectObj : Obj {
    Value target;
};
static_assert(sizeof(IndirectObj) <= sizeof(ThunkObj),
              "a forced thunk is overwritten in place, so it must not grow");

struct PapObj : Obj {
    Value fn;
    uint32_t nargs;
    uint32_t pad;
    Value* args() { return reinterpret_cast<Value*>(this + 1); }
};

struct FrameObj : Obj {
    Value closure;
    uint32_t nslots;
    uint32_t pad;
    Value* slots() { return reinterpret_cast<Value*>(this + 1); }
};

struct ModuleObj : Obj {
    uint32_t import_index;
    uint32_t pad;
    Value name;  // StrObj
};

struct ErrorObj : Obj {
    Value kind;     // atom
    Value payload;  // arbitrary value
};

struct PidObj : Obj {
    uint64_t id;
};

/// A host function: a builtin, a module member, or something registered
/// through the C API. `strict_mask` bit i set means argument i is forced
/// before the call -- `spawn!` clears bit 0, because forcing the thunk it is
/// handed would run the new process's work on the caller.
struct NativeObj : Obj {
    void* fn;
    Value name;
    uint32_t arity;
    uint32_t strict_mask;
    /// Host state the function is bound to -- an FFI binding id, say. A native
    /// receives its own object, so this is how one C function can back many
    /// distinct Dream functions.
    uint64_t user;
};

// ---------------------------------------------------------------------------
// Following indirections
// ---------------------------------------------------------------------------

/// Chase Indirect nodes left behind by thunk updates. Every read of a value
/// that might have been a thunk goes through this.
inline Value resolve(Value v) {
    while (is_ptr(v) && as_obj(v)->type == ObjType::Indirect) {
        v = static_cast<IndirectObj*>(as_obj(v))->target;
    }
    return v;
}

/// True when the value is already in weak head normal form.
inline bool is_whnf(Value v) {
    if (!is_ptr(v)) return true;
    switch (as_obj(v)->type) {
        case ObjType::Thunk:
        case ObjType::Blackhole:
        case ObjType::Indirect:
            return false;
        default:
            return true;
    }
}

dream_type surface_type(Value v);

}  // namespace dream
