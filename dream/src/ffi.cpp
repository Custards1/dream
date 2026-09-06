// std.ffi -- calling C from Dream.
//
// The design deliberately adds no syntax. A binding is made by calling a
// function with the signature as data:
//
//     let sqrt = ffi.load! "libm.so.6" "sqrt" [:f64] :f64;
//     sqrt 2.0
//
// Types are atoms, argument lists are lists, and the result is an ordinary
// Dream function. That keeps the FFI honest about what it is -- a dynamically
// checked bridge -- and means the standard library can bind C in Dream source
// rather than needing a C++ shim written for every call.
//
// Nothing here can make a wrong signature safe: describing `sqrt` as taking an
// integer will corrupt the call, exactly as it would in C. What it does do is
// check everything it can before jumping -- that the handle is one it issued,
// that the type atoms are real, that the arity matches -- so the failures that
// can be caught are caught.

#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "builtins.hpp"
#include "interp.hpp"
#include "process.hpp"
#include "runtime.hpp"

#if DREAM_HAVE_FFI
#include <dlfcn.h>
#include <ffi.h>
#endif

namespace dream {

namespace {

enum Kind : uint8_t {
    K_VOID, K_BOOL,
    K_I8, K_I16, K_I32, K_I64,
    K_U8, K_U16, K_U32, K_U64,
    K_F32, K_F64,
    K_PTR, K_CSTR,
    K_INVALID,
};

struct KindName {
    const char* name;
    Kind kind;
    size_t size;
};

/// The type vocabulary. `ptr` is an address carried as an integer: Dream has no
/// pointer type, and inventing one would put a value in the language that only
/// the FFI could produce or consume.
constexpr KindName KINDS[] = {
    {"void", K_VOID, 0},   {"bool", K_BOOL, sizeof(bool)},
    {"i8", K_I8, 1},       {"i16", K_I16, 2},
    {"i32", K_I32, 4},     {"i64", K_I64, 8},
    {"u8", K_U8, 1},       {"u16", K_U16, 2},
    {"u32", K_U32, 4},     {"u64", K_U64, 8},
    {"f32", K_F32, 4},     {"f64", K_F64, 8},
    {"ptr", K_PTR, sizeof(void*)},
    {"cstr", K_CSTR, sizeof(void*)},
};

Kind kind_of(const std::string& name) {
    for (const auto& k : KINDS) {
        if (name == k.name) return k.kind;
    }
    return K_INVALID;
}

size_t size_of(Kind k) {
    for (const auto& e : KINDS) {
        if (e.kind == k) return e.size;
    }
    return 0;
}

constexpr uint32_t MAX_ARGS = 16;

[[maybe_unused]] NativeResult fail(Process& p, const std::string& msg) {
    return NativeResult::raise(raise_error(p, p.runtime().intern_atom("ffi_error"), msg));
}

#if DREAM_HAVE_FFI

ffi_type* ffi_type_for(Kind k) {
    switch (k) {
        case K_VOID: return &ffi_type_void;
        case K_BOOL: return &ffi_type_uint8;
        case K_I8: return &ffi_type_sint8;
        case K_I16: return &ffi_type_sint16;
        case K_I32: return &ffi_type_sint32;
        case K_I64: return &ffi_type_sint64;
        case K_U8: return &ffi_type_uint8;
        case K_U16: return &ffi_type_uint16;
        case K_U32: return &ffi_type_uint32;
        case K_U64: return &ffi_type_uint64;
        case K_F32: return &ffi_type_float;
        case K_F64: return &ffi_type_double;
        case K_PTR:
        case K_CSTR: return &ffi_type_pointer;
        default: return nullptr;
    }
}

struct Binding {
    void* fn = nullptr;
    ffi_cif cif{};
    std::vector<ffi_type*> arg_ffi;
    std::vector<Kind> arg_kinds;
    Kind ret_kind = K_VOID;
    std::string name;
};

/// Libraries and bindings live for the life of the process and are shared by
/// every Dream process, so they are handed out as small integer ids rather than
/// raw pointers: an id the registry did not issue is rejected instead of
/// jumped to.
struct Registry {
    std::mutex mutex;
    std::vector<void*> libs;
    std::vector<std::unique_ptr<Binding>> bindings;

    static Registry& get() {
        static Registry r;
        return r;
    }
};

#endif  // DREAM_HAVE_FFI

/// Read a Dream list of type atoms into kinds.
bool read_kinds(Process& p, Value list, std::vector<Kind>* out, std::string* error) {
    Value cur = list;
    for (;;) {
        Value w;
        if (!force_whnf(p, cur, &w)) {
            *error = "the argument type list could not be evaluated";
            return false;
        }
        if (is_nil(w)) return true;
        if (!is_obj(w, ObjType::Cons)) {
            *error = "the argument types must be a list of atoms";
            return false;
        }
        auto* c = static_cast<ConsObj*>(as_obj(w));
        Value head;
        if (!force_whnf(p, c->head, &head)) {
            *error = "the argument type list could not be evaluated";
            return false;
        }
        if (!is_atom(head)) {
            *error = "each argument type must be an atom, such as :i32";
            return false;
        }
        std::string name = p.runtime().atom_name(uint32_t(imm_payload(head)));
        Kind k = kind_of(name);
        if (k == K_INVALID || k == K_VOID) {
            *error = "`:" + name + "` is not an argument type";
            return false;
        }
        out->push_back(k);
        if (out->size() > MAX_ARGS) {
            *error = "more than " + std::to_string(MAX_ARGS) + " arguments";
            return false;
        }
        cur = c->tail;
    }
}

std::string string_arg(Value v) {
    v = resolve(v);
    if (!is_obj(v, ObjType::Str)) return std::string();
    auto* s = static_cast<StrObj*>(as_obj(v));
    return std::string(s->data(), s->len);
}

// ---------------------------------------------------------------------------
// Members
// ---------------------------------------------------------------------------

NativeResult ffi_sizeof(Process& p, Value, Value* args, uint32_t) {
    Value v = resolve(args[0]);
    if (!is_atom(v)) return fail(p, "sizeof needs a type atom");
    Kind k = kind_of(p.runtime().atom_name(uint32_t(imm_payload(v))));
    if (k == K_INVALID) {
        return fail(p, "`:" + p.runtime().atom_name(uint32_t(imm_payload(v))) +
                           "` is not an FFI type");
    }
    return NativeResult::ok(make_fixnum(int64_t(size_of(k))));
}

#if !DREAM_HAVE_FFI

NativeResult ffi_unavailable(Process& p, Value, Value*, uint32_t) {
    return fail(p, "this build has no FFI: libffi was not found when the VM was configured");
}

#else

NativeResult ffi_open(Process& p, Value, Value* args, uint32_t) {
    Value v = resolve(args[0]);
    // An empty name means the running program: everything already linked into
    // the VM, which is the whole C library on any normal system. It saves
    // every caller from guessing at a versioned filename.
    bool self = is_unit(v) || (is_obj(v, ObjType::Str) &&
                               static_cast<StrObj*>(as_obj(v))->len == 0);
    std::string name = self ? std::string() : string_arg(v);
    if (!self && name.empty()) return fail(p, "open! needs a library name, or \"\" for this program");

    void* handle = ::dlopen(self ? nullptr : name.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char* err = ::dlerror();
        return fail(p, "cannot open `" + (self ? std::string("<this program>") : name) +
                           "`: " + (err ? err : "unknown error"));
    }
    auto& reg = Registry::get();
    std::lock_guard<std::mutex> g(reg.mutex);
    reg.libs.push_back(handle);
    return NativeResult::ok(make_fixnum(int64_t(reg.libs.size())));
}

/// Resolve a handle to a library, or null.
void* lib_of(int64_t handle) {
    auto& reg = Registry::get();
    std::lock_guard<std::mutex> g(reg.mutex);
    if (handle < 1 || size_t(handle) > reg.libs.size()) return nullptr;
    return reg.libs[size_t(handle) - 1];
}

NativeResult ffi_bind(Process& p, Value, Value* args, uint32_t) {
    Value h = resolve(args[0]);
    if (!is_fixnum(h)) return fail(p, "bind! needs a library handle");
    void* lib = lib_of(fixnum_value(h));
    if (!lib) return fail(p, "bind! was given a handle this runtime did not issue");

    std::string symbol = string_arg(args[1]);
    if (symbol.empty()) return fail(p, "bind! needs a symbol name");

    std::vector<Kind> kinds;
    std::string error;
    if (!read_kinds(p, args[2], &kinds, &error)) return fail(p, error);

    Value ret = resolve(args[3]);
    if (!is_atom(ret)) return fail(p, "the return type must be an atom");
    Kind ret_kind = kind_of(p.runtime().atom_name(uint32_t(imm_payload(ret))));
    if (ret_kind == K_INVALID) {
        return fail(p, "`:" + p.runtime().atom_name(uint32_t(imm_payload(ret))) +
                           "` is not a return type");
    }

    ::dlerror();
    void* fn = ::dlsym(lib, symbol.c_str());
    const char* dl_err = ::dlerror();
    if (dl_err) return fail(p, "cannot find `" + symbol + "`: " + dl_err);

    auto binding = std::make_unique<Binding>();
    binding->fn = fn;
    binding->arg_kinds = kinds;
    binding->ret_kind = ret_kind;
    binding->name = symbol;
    for (Kind k : kinds) binding->arg_ffi.push_back(ffi_type_for(k));

    if (ffi_prep_cif(&binding->cif, FFI_DEFAULT_ABI, unsigned(kinds.size()),
                     ffi_type_for(ret_kind), binding->arg_ffi.data()) != FFI_OK) {
        return fail(p, "cannot prepare a call to `" + symbol + "`");
    }

    uint64_t id;
    {
        auto& reg = Registry::get();
        std::lock_guard<std::mutex> g(reg.mutex);
        reg.bindings.push_back(std::move(binding));
        id = reg.bindings.size();
    }

    // A zero-argument C function still has to be callable, and Dream applies a
    // nullary function to unit, so give it arity 1 in that case.
    uint32_t arity = kinds.empty() ? 1 : uint32_t(kinds.size());
    Value name = p.heap().make_string(symbol.data(), uint32_t(symbol.size()));
    extern NativeResult ffi_invoke(Process&, Value, Value*, uint32_t);
    return NativeResult::ok(
        make_native(p, ffi_invoke, name, arity, 0xFFFFFFFFu, id));
}

NativeResult ffi_close(Process& p, Value, Value* args, uint32_t) {
    Value h = resolve(args[0]);
    if (!is_fixnum(h)) return fail(p, "close! needs a library handle");
    auto& reg = Registry::get();
    std::lock_guard<std::mutex> g(reg.mutex);
    int64_t i = fixnum_value(h);
    if (i < 1 || size_t(i) > reg.libs.size() || !reg.libs[size_t(i) - 1]) {
        return fail(p, "close! was given a handle this runtime did not issue");
    }
    // Bindings hold raw pointers into the library, so leave it mapped and just
    // drop our handle: unmapping under a live binding would be a crash waiting
    // to happen, and a program that closed a library is not asking for that.
    reg.libs[size_t(i) - 1] = nullptr;
    return NativeResult::ok(UNIT);
}

/// Storage for one outgoing argument, big enough for any scalar we pass.
union ArgSlot {
    int8_t i8;
    int16_t i16;
    int32_t i32;
    int64_t i64;
    uint8_t u8;
    uint16_t u16;
    uint32_t u32;
    uint64_t u64;
    float f32;
    double f64;
    void* ptr;
};

bool marshal_in(Process& p, Kind k, Value v, ArgSlot* slot, std::string* error) {
    v = resolve(v);
    auto need_int = [&](int64_t* out) {
        if (is_fixnum(v)) {
            *out = fixnum_value(v);
            return true;
        }
        if (is_obj(v, ObjType::Float)) {
            *out = int64_t(static_cast<FloatObj*>(as_obj(v))->value);
            return true;
        }
        *error = "expected a number";
        return false;
    };
    auto need_float = [&](double* out) {
        if (is_obj(v, ObjType::Float)) {
            *out = static_cast<FloatObj*>(as_obj(v))->value;
            return true;
        }
        if (is_fixnum(v)) {
            *out = double(fixnum_value(v));
            return true;
        }
        *error = "expected a number";
        return false;
    };

    int64_t i = 0;
    double d = 0;
    switch (k) {
        case K_BOOL: slot->u8 = truthy(v) ? 1 : 0; return true;
        case K_I8: if (!need_int(&i)) return false; slot->i8 = int8_t(i); return true;
        case K_I16: if (!need_int(&i)) return false; slot->i16 = int16_t(i); return true;
        case K_I32: if (!need_int(&i)) return false; slot->i32 = int32_t(i); return true;
        case K_I64: if (!need_int(&i)) return false; slot->i64 = i; return true;
        case K_U8: if (!need_int(&i)) return false; slot->u8 = uint8_t(i); return true;
        case K_U16: if (!need_int(&i)) return false; slot->u16 = uint16_t(i); return true;
        case K_U32: if (!need_int(&i)) return false; slot->u32 = uint32_t(i); return true;
        case K_U64: if (!need_int(&i)) return false; slot->u64 = uint64_t(i); return true;
        case K_F32: if (!need_float(&d)) return false; slot->f32 = float(d); return true;
        case K_F64: if (!need_float(&d)) return false; slot->f64 = d; return true;
        case K_PTR:
            if (!is_fixnum(v)) {
                *error = "expected an address, which is carried as an integer";
                return false;
            }
            slot->ptr = reinterpret_cast<void*>(uintptr_t(fixnum_value(v)));
            return true;
        case K_CSTR: {
            if (is_nil(v) || is_unit(v)) {
                slot->ptr = nullptr;
                return true;
            }
            if (!is_obj(v, ObjType::Str)) {
                *error = "expected a string";
                return false;
            }
            // Dream strings are stored with a trailing NUL for exactly this.
            slot->ptr = static_cast<StrObj*>(as_obj(v))->data();
            return true;
        }
        default:
            *error = "unsupported argument type";
            return false;
    }
}

Value marshal_out(Process& p, Kind k, const ArgSlot& slot) {
    switch (k) {
        case K_VOID: return UNIT;
        case K_BOOL: return make_bool(slot.u8 != 0);
        case K_I8: return make_fixnum(slot.i8);
        case K_I16: return make_fixnum(slot.i16);
        case K_I32: return make_fixnum(slot.i32);
        case K_I64: return make_integer(p, slot.i64);
        case K_U8: return make_fixnum(slot.u8);
        case K_U16: return make_fixnum(slot.u16);
        case K_U32: return make_fixnum(slot.u32);
        case K_U64: return make_integer(p, int64_t(slot.u64));
        case K_F32: return p.heap().make_float(slot.f32);
        case K_F64: return p.heap().make_float(slot.f64);
        case K_PTR: return make_integer(p, int64_t(uintptr_t(slot.ptr)));
        case K_CSTR: {
            auto* s = static_cast<const char*>(slot.ptr);
            if (!s) return UNIT;
            return p.heap().make_string(s, uint32_t(std::strlen(s)));
        }
        default: return UNIT;
    }
}

NativeResult ffi_invoke(Process& p, Value callee, Value* args, uint32_t argc) {
    auto* nat = static_cast<NativeObj*>(as_obj(callee));
    Binding* b = nullptr;
    {
        auto& reg = Registry::get();
        std::lock_guard<std::mutex> g(reg.mutex);
        uint64_t id = nat->user;
        if (id < 1 || id > reg.bindings.size()) {
            return fail(p, "this foreign function is no longer bound");
        }
        b = reg.bindings[id - 1].get();
    }

    const uint32_t want = uint32_t(b->arg_kinds.size());
    if (argc < want) {
        return fail(p, "`" + b->name + "` needs " + std::to_string(want) + " arguments");
    }

    ArgSlot slots[MAX_ARGS];
    void* pointers[MAX_ARGS];
    for (uint32_t i = 0; i < want; ++i) {
        std::string error;
        if (!marshal_in(p, b->arg_kinds[i], args[i], &slots[i], &error)) {
            return fail(p, "argument " + std::to_string(i + 1) + " of `" + b->name +
                               "`: " + error);
        }
        pointers[i] = &slots[i];
    }

    // libffi writes at least a full word, so the return slot must be one.
    ArgSlot ret{};
    ffi_call(&b->cif, FFI_FN(b->fn), &ret, pointers);
    return NativeResult::ok(marshal_out(p, b->ret_kind, ret));
}

NativeResult ffi_load(Process& p, Value callee, Value* args, uint32_t) {
    // open! then bind!, which is what almost every use wants.
    Value opened;
    {
        NativeResult r = ffi_open(p, callee, args, 1);
        if (r.outcome != NativeOutcome::Value) return r;
        opened = r.value;
    }
    Value bind_args[4] = {opened, args[1], args[2], args[3]};
    return ffi_bind(p, callee, bind_args, 4);
}

NativeResult ffi_alloc(Process& p, Value, Value* args, uint32_t) {
    Value n = resolve(args[0]);
    if (!is_fixnum(n) || fixnum_value(n) < 0) return fail(p, "alloc! needs a size");
    void* mem = std::calloc(size_t(fixnum_value(n)), 1);
    if (!mem) return fail(p, "out of memory");
    return NativeResult::ok(make_integer(p, int64_t(uintptr_t(mem))));
}

NativeResult ffi_free(Process& p, Value, Value* args, uint32_t) {
    Value v = resolve(args[0]);
    if (!is_fixnum(v)) return fail(p, "free! needs an address");
    std::free(reinterpret_cast<void*>(uintptr_t(fixnum_value(v))));
    return NativeResult::ok(UNIT);
}

NativeResult ffi_read_cstr(Process& p, Value, Value* args, uint32_t) {
    Value v = resolve(args[0]);
    if (!is_fixnum(v)) return fail(p, "read_cstr! needs an address");
    auto* s = reinterpret_cast<const char*>(uintptr_t(fixnum_value(v)));
    if (!s) return NativeResult::ok(UNIT);
    return NativeResult::ok(p.heap().make_string(s, uint32_t(std::strlen(s))));
}

NativeResult ffi_read_u8(Process& p, Value, Value* args, uint32_t) {
    Value base = resolve(args[0]);
    Value off = resolve(args[1]);
    if (!is_fixnum(base) || !is_fixnum(off)) return fail(p, "read_u8! needs an address and an offset");
    auto* q = reinterpret_cast<uint8_t*>(uintptr_t(fixnum_value(base))) + fixnum_value(off);
    return NativeResult::ok(make_fixnum(*q));
}

NativeResult ffi_write_u8(Process& p, Value, Value* args, uint32_t) {
    Value base = resolve(args[0]);
    Value off = resolve(args[1]);
    Value val = resolve(args[2]);
    if (!is_fixnum(base) || !is_fixnum(off) || !is_fixnum(val)) {
        return fail(p, "write_u8! needs an address, an offset and a byte");
    }
    auto* q = reinterpret_cast<uint8_t*>(uintptr_t(fixnum_value(base))) + fixnum_value(off);
    *q = uint8_t(fixnum_value(val));
    return NativeResult::ok(UNIT);
}

#endif  // DREAM_HAVE_FFI

}  // namespace

ModuleDef make_ffi_module() {
#if DREAM_HAVE_FFI
    return ModuleDef{"std.ffi",
                     {
                         {"open!", 1, 0b1, ffi_open},
                         {"close!", 1, 0b1, ffi_close},
                         {"bind!", 4, 0b1111, ffi_bind},
                         {"load!", 4, 0b1111, ffi_load},
                         {"sizeof", 1, 0b1, ffi_sizeof},
                         {"alloc!", 1, 0b1, ffi_alloc},
                         {"free!", 1, 0b1, ffi_free},
                         {"read_cstr!", 1, 0b1, ffi_read_cstr},
                         {"read_u8!", 2, 0b11, ffi_read_u8},
                         {"write_u8!", 3, 0b111, ffi_write_u8},
                     }};
#else
    // The module still exists so that `import std.ffi` compiles everywhere and
    // the failure is a clear message at the call, not a missing module.
    return ModuleDef{"std.ffi",
                     {
                         {"open!", 1, 0b1, ffi_unavailable},
                         {"close!", 1, 0b1, ffi_unavailable},
                         {"bind!", 4, 0b1111, ffi_unavailable},
                         {"load!", 4, 0b1111, ffi_unavailable},
                         {"sizeof", 1, 0b1, ffi_sizeof},
                         {"alloc!", 1, 0b1, ffi_unavailable},
                         {"free!", 1, 0b1, ffi_unavailable},
                         {"read_cstr!", 1, 0b1, ffi_unavailable},
                         {"read_u8!", 2, 0b11, ffi_unavailable},
                         {"write_u8!", 3, 0b111, ffi_unavailable},
                     }};
#endif
}

bool ffi_available() {
#if DREAM_HAVE_FFI
    return true;
#else
    return false;
#endif
}

}  // namespace dream
