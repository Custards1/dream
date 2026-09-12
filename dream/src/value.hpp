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
    BigStr,    // a view into the image's payload; see BigStrObj
    Cons,      // lazy list cell
    Array,
    Map,       // a branch of the hash trie; every map value is one of these
    MapLeaf,   // one entry of a map, or a chain of them sharing a hash
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
    uint16_t aux;  // flags that survive collection; see AUX_DEEP_FORCED
    uint32_t bytes;
};

/// Everything under this object has been forced.
///
/// A value never changes once it is built, so this is true forever once it is
/// true at all -- which is what lets a deep force skip a structure it has
/// already walked. Without it, forcing walks *paths* rather than data, and a
/// value shared n levels deep is walked 2^n times: the compiler's own tables,
/// which share a syntax tree between every table that mentions it, took seconds
/// to walk and no time at all to build.
///
/// It rides in `aux`, which the collector copies with the rest of the header,
/// so it survives a collection. A host that reaches in and replaces a field of
/// an object it did not just make must clear it.
constexpr uint16_t AUX_DEEP_FORCED = 1;
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

/// A window onto the program image's payload region.
///
/// A `StrObj` counts its bytes in a `uint32_t` and carries them in its own
/// allocation, which caps a string at 4 GiB and costs a copy at every
/// materialization, every promotion and every `spawn!`. Neither is acceptable
/// for the datum a program is *about* -- a corpus, a model, an asset -- so a
/// big string is not a string at all. It is a length and a pointer into the
/// mapped image, and building one is O(1).
///
/// Sharing the pointer is safe precisely because it does not point into a
/// heap: the bytes belong to the `Runtime`'s image, which is immutable and
/// outlives every process in it. A copy between heaps therefore copies the
/// view, not the bytes. What it may *not* cross is a runtime boundary
/// (`std.vm`), where the pointer would name someone else's mapping.
///
/// `hash` is 0 until something asks, because a map key is the only thing that
/// needs it and walking a gigabyte to answer a question nobody asked is the
/// cost this type exists to refuse.
struct BigStrObj : Obj {
    uint64_t len;
    uint64_t hash;
    const char* data;
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

/// A persistent map: a hash array mapped trie. Keys are forced (we have to hash
/// them); values stay lazy.
///
/// This was an open-addressed table, which is the right shape for a language
/// that mutates and the wrong one for a language that does not. Every `map_put`
/// had to copy the whole table to leave the original standing, so building a
/// map an entry at a time -- the only way to build one here -- cost O(n^2) time
/// and allocated O(n^2) bytes. A program accumulating ten thousand entries did
/// fifty million copies to do it.
///
/// A trie shares structure instead. Putting rebuilds only the path from the
/// root to the leaf that changed, about log32(n) nodes, and every other node is
/// shared with the map it came from. Both maps stay valid, which is what a
/// persistent value has to promise, and neither pays for the other.
///
/// A branch holds up to 32 children chosen by five bits of the key's hash.
/// `bitmap` says which of the 32 are present and the children are packed
/// against each other, so a sparse node costs only what it actually holds.
/// The root of a map is always a branch, so that every map value has the same
/// type however few entries it has.
struct MapObj : Obj {
    /// Entries in this whole subtree. The root's is the map's size, which is
    /// what makes `len` on a map constant time.
    uint32_t count;
    uint32_t bitmap;
    Value* slots() { return reinterpret_cast<Value*>(this + 1); }
};

/// One entry, or a chain of them when two keys hash to the same 64 bits.
///
/// Keys with different hashes always separate somewhere in the trie, so a chain
/// is only ever a real collision rather than a probe sequence -- which is why
/// there is no load factor here and nothing to rehash.
struct MapLeafObj : Obj {
    uint64_t hash;
    Value key;
    Value value;
    Value next;  // NIL, or another leaf with the same hash
};

/// Five bits of hash per level: 32 children, and a 64-bit hash runs out after
/// thirteen of them, by which point any two distinct hashes have separated.
constexpr uint32_t MAP_BITS = 5;
constexpr uint32_t MAP_WIDTH = 1u << MAP_BITS;
constexpr uint32_t MAP_MASK = MAP_WIDTH - 1;

inline uint32_t map_bit_count(uint32_t x) {
#if defined(__GNUC__) || defined(__clang__)
    return uint32_t(__builtin_popcount(x));
#else
    uint32_t n = 0;
    while (x) { x &= x - 1; ++n; }
    return n;
#endif
}

/// Which child a hash selects at this depth, and where that child sits in the
/// packed array.
inline uint32_t map_index(uint64_t hash, uint32_t shift) {
    return uint32_t(hash >> shift) & MAP_MASK;
}
inline uint32_t map_slot_of(uint32_t bitmap, uint32_t bit) {
    return map_bit_count(bitmap & (bit - 1));
}

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

// ---------------------------------------------------------------------------
// Strings, in either representation
// ---------------------------------------------------------------------------

/// The bytes behind a string value, and how many of them there are.
///
/// A `Str` carries its bytes in its own allocation; a `BigStr` points at the
/// image's payload. Every operation that only *reads* a string -- compare,
/// hash, index, slice, write -- wants the pointer and the length and has no
/// business knowing which of the two it was handed, so it asks here once
/// instead of switching on the type at each site. The length is 64-bit
/// because one of the two kinds can be.
struct Bytes {
    const char* data = nullptr;
    uint64_t len = 0;
};

/// True when the value is a string of either representation.
inline bool is_stringish(Value v) {
    v = resolve(v);
    if (!is_ptr(v)) return false;
    ObjType t = as_obj(v)->type;
    return t == ObjType::Str || t == ObjType::BigStr;
}

/// The bytes of a string value, or false when it is not one.
inline bool string_bytes(Value v, Bytes* out) {
    v = resolve(v);
    if (!is_ptr(v)) return false;
    Obj* o = as_obj(v);
    if (o->type == ObjType::Str) {
        auto* s = static_cast<StrObj*>(o);
        *out = Bytes{s->data(), s->len};
        return true;
    }
    if (o->type == ObjType::BigStr) {
        auto* b = static_cast<BigStrObj*>(o);
        *out = Bytes{b->data, b->len};
        return true;
    }
    return false;
}

/// Lexicographic byte order, shorter first on a shared prefix. -1, 0 or 1.
inline int bytes_compare(const Bytes& a, const Bytes& b) {
    uint64_t n = a.len < b.len ? a.len : b.len;
    int c = n ? std::memcmp(a.data, b.data, size_t(n)) : 0;
    if (c != 0) return c < 0 ? -1 : 1;
    return a.len < b.len ? -1 : (a.len > b.len ? 1 : 0);
}

inline bool bytes_equal(const Bytes& a, const Bytes& b) {
    return a.len == b.len && (a.len == 0 || std::memcmp(a.data, b.data, size_t(a.len)) == 0);
}

/// FNV-1a over the bytes, never zero -- zero is what a cached hash uses to mean
/// "not worked out yet". Both string representations hash through this, so a
/// `BigStr` slice and the literal it matches land in the same map bucket.
inline uint64_t bytes_hash(const Bytes& b) {
    uint64_t h = 1469598103934665603ull;
    for (uint64_t i = 0; i < b.len; ++i) {
        h ^= uint8_t(b.data[i]);
        h *= 1099511628211ull;
    }
    return h ? h : 1;
}

dream_type surface_type(Value v);

}  // namespace dream
