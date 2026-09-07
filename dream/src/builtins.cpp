#include "builtins.hpp"

#include "io.hpp"

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <stdexcept>

#include "interp.hpp"
#include "process.hpp"
#include "scheduler.hpp"

namespace dream {

// ---------------------------------------------------------------------------
// Maps
// ---------------------------------------------------------------------------

namespace {

uint64_t key_hash(Value v) {
    v = resolve(v);
    if (is_ptr(v) && as_obj(v)->type == ObjType::Str) {
        auto* s = static_cast<StrObj*>(as_obj(v));
        uint64_t h = 1469598103934665603ull;
        for (uint32_t i = 0; i < s->len; ++i) {
            h ^= uint8_t(s->data()[i]);
            h *= 1099511628211ull;
        }
        return h ? h : 1;
    }
    uint64_t h = v;
    if (is_ptr(v) && as_obj(v)->type == ObjType::Float) {
        double d = static_cast<FloatObj*>(as_obj(v))->value;
        std::memcpy(&h, &d, 8);
    }
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return h ? h : 1;
}

/// Keys are compared by identity or by contents for the flat types. Anything
/// deeper is compared by identity, which is what a hash map can promise
/// without forcing arbitrary structure at lookup time.
bool key_equal(Value a, Value b) {
    a = resolve(a);
    b = resolve(b);
    if (a == b) return true;
    if (!is_ptr(a) || !is_ptr(b)) return false;
    Obj* x = as_obj(a);
    Obj* y = as_obj(b);
    if (x->type != y->type) return false;
    if (x->type == ObjType::Str) {
        auto* s = static_cast<StrObj*>(x);
        auto* t = static_cast<StrObj*>(y);
        return s->len == t->len && std::memcmp(s->data(), t->data(), s->len) == 0;
    }
    if (x->type == ObjType::Float) {
        return static_cast<FloatObj*>(x)->value == static_cast<FloatObj*>(y)->value;
    }
    if (x->type == ObjType::Pid) {
        return static_cast<PidObj*>(x)->id == static_cast<PidObj*>(y)->id;
    }
    return false;
}

void map_put_raw(MapObj* m, Value key, Value value) {
    uint32_t mask = m->cap - 1;
    uint32_t i = uint32_t(key_hash(key)) & mask;
    for (;;) {
        Value existing = m->entries()[i * 2];
        if (existing == NIL_SLOT) {
            m->entries()[i * 2] = key;
            m->entries()[i * 2 + 1] = value;
            ++m->count;
            return;
        }
        if (key_equal(existing, key)) {
            m->entries()[i * 2 + 1] = value;
            return;
        }
        i = (i + 1) & mask;
    }
}

}  // namespace

void map_insert(Process& p, Value map, Value key, Value value) {
    // Growing replaces the object, leaving an indirection behind, so a caller
    // holding the old handle must be followed before anything else. Map
    // literals never reach this -- they are sized to twice their entry count --
    // but anything that inserts in a loop does.
    map = resolve(map);
    auto* m = static_cast<MapObj*>(as_obj(map));

    // Keep the load factor under 3/4 so probe chains stay short.
    if ((m->count + 1) * 4 >= m->cap * 3) {
        Value grown = p.heap().make_map(m->cap * 2);
        auto* g = static_cast<MapObj*>(as_obj(grown));
        m = static_cast<MapObj*>(as_obj(map));
        for (uint32_t i = 0; i < m->cap; ++i) {
            Value k = m->entries()[i * 2];
            if (k != NIL_SLOT) map_put_raw(g, k, m->entries()[i * 2 + 1]);
        }
        map_put_raw(g, key, value);
        // The entries live inline after the header, so the object cannot grow
        // in place. Turn the old one into an indirection to the new one, which
        // is the same mechanism a forced thunk uses: every existing reference
        // keeps working, and the collector collapses the hop.
        as_obj(map)->type = ObjType::Indirect;
        static_cast<IndirectObj*>(as_obj(map))->target = grown;
        return;
    }
    map_put_raw(m, key, value);
}

bool map_lookup(Process& p, Value map, Value key, Value* out) {
    map = resolve(map);
    if (!is_obj(map, ObjType::Map)) return false;
    auto* m = static_cast<MapObj*>(as_obj(map));
    if (m->cap == 0) return false;
    uint32_t mask = m->cap - 1;
    uint32_t i = uint32_t(key_hash(key)) & mask;
    for (uint32_t probe = 0; probe < m->cap; ++probe) {
        Value k = m->entries()[i * 2];
        if (k == NIL_SLOT) return false;
        if (key_equal(k, key)) {
            *out = m->entries()[i * 2 + 1];
            return true;
        }
        i = (i + 1) & mask;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

namespace {

bool stringify_into(Process& p, Value v, std::string* out, bool quoted, int depth);

bool stringify_list(Process& p, Value v, std::string* out, int depth) {
    out->push_back('[');
    bool first = true;
    Value cur = v;
    for (;;) {
        Value w;
        if (!force_whnf(p, cur, &w)) return false;
        if (is_nil(w)) break;
        if (!is_obj(w, ObjType::Cons)) {
            // An improper tail: show it rather than pretend the list ended.
            out->append(" | ");
            if (!stringify_into(p, w, out, true, depth + 1)) return false;
            break;
        }
        if (!first) out->append(", ");
        first = false;
        auto* c = static_cast<ConsObj*>(as_obj(w));
        Value head = c->head;
        Value tail = c->tail;
        if (!stringify_into(p, head, out, true, depth + 1)) return false;
        cur = tail;
    }
    out->push_back(']');
    return true;
}

bool stringify_into(Process& p, Value v, std::string* out, bool quoted, int depth) {
    if (depth > 128) {
        out->append("...");
        return true;
    }
    Value w;
    if (!force_whnf(p, v, &w)) return false;

    if (is_fixnum(w)) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%" PRId64, fixnum_value(w));
        out->append(buf);
        return true;
    }
    if (is_imm(w)) {
        switch (imm_kind(w)) {
            case IMM_UNIT: out->append("()"); return true;
            case IMM_BOOL: out->append(truthy(w) ? "true" : "false"); return true;
            case IMM_NIL: out->append("[]"); return true;
            case IMM_ATOM:
                out->push_back(':');
                out->append(p.runtime().atom_name(uint32_t(imm_payload(w))));
                return true;
            case IMM_CHAR: {
                uint32_t cp = uint32_t(imm_payload(w));
                if (quoted) out->push_back('\'');
                // Encode as UTF-8.
                if (cp < 0x80) {
                    out->push_back(char(cp));
                } else if (cp < 0x800) {
                    out->push_back(char(0xC0 | (cp >> 6)));
                    out->push_back(char(0x80 | (cp & 0x3F)));
                } else if (cp < 0x10000) {
                    out->push_back(char(0xE0 | (cp >> 12)));
                    out->push_back(char(0x80 | ((cp >> 6) & 0x3F)));
                    out->push_back(char(0x80 | (cp & 0x3F)));
                } else {
                    out->push_back(char(0xF0 | (cp >> 18)));
                    out->push_back(char(0x80 | ((cp >> 12) & 0x3F)));
                    out->push_back(char(0x80 | ((cp >> 6) & 0x3F)));
                    out->push_back(char(0x80 | (cp & 0x3F)));
                }
                if (quoted) out->push_back('\'');
                return true;
            }
            case IMM_BUILTIN:
                out->append("<builtin ");
                out->append(builtin_def(uint32_t(imm_payload(w))).name);
                out->push_back('>');
                return true;
            default: out->append("<value>"); return true;
        }
    }
    if (!is_ptr(w)) { out->append("<value>"); return true; }

    switch (as_obj(w)->type) {
        case ObjType::Float: {
            double d = static_cast<FloatObj*>(as_obj(w))->value;
            char buf[40];
            // %.17g round-trips but is ugly; try shorter forms first.
            for (int prec = 1; prec <= 17; ++prec) {
                std::snprintf(buf, sizeof buf, "%.*g", prec, d);
                if (std::strtod(buf, nullptr) == d) break;
            }
            out->append(buf);
            return true;
        }
        case ObjType::Str: {
            auto* s = static_cast<StrObj*>(as_obj(w));
            if (!quoted) {
                out->append(s->data(), s->len);
                return true;
            }
            out->push_back('"');
            for (uint32_t i = 0; i < s->len; ++i) {
                char ch = s->data()[i];
                switch (ch) {
                    case '"': out->append("\\\""); break;
                    case '\\': out->append("\\\\"); break;
                    case '\n': out->append("\\n"); break;
                    case '\t': out->append("\\t"); break;
                    default: out->push_back(ch);
                }
            }
            out->push_back('"');
            return true;
        }
        case ObjType::Cons: return stringify_list(p, w, out, depth);
        case ObjType::Array: {
            out->append("#[");
            p.stack.push_back(w);
            uint32_t len = static_cast<ArrayObj*>(as_obj(w))->len;
            for (uint32_t i = 0; i < len; ++i) {
                if (i) out->append(", ");
                Value item = static_cast<ArrayObj*>(as_obj(p.stack.back()))->items()[i];
                if (!stringify_into(p, item, out, true, depth + 1)) {
                    p.stack.pop_back();
                    return false;
                }
            }
            p.stack.pop_back();
            out->push_back(']');
            return true;
        }
        case ObjType::Map: {
            out->append("%{");
            p.stack.push_back(w);
            uint32_t cap = static_cast<MapObj*>(as_obj(w))->cap;
            bool first = true;
            for (uint32_t i = 0; i < cap; ++i) {
                auto* m = static_cast<MapObj*>(as_obj(p.stack.back()));
                if (m->entries()[i * 2] == NIL_SLOT) continue;
                if (!first) out->append(", ");
                first = false;
                Value k = m->entries()[i * 2];
                if (!stringify_into(p, k, out, true, depth + 1)) { p.stack.pop_back(); return false; }
                out->append(" => ");
                Value val = static_cast<MapObj*>(as_obj(p.stack.back()))->entries()[i * 2 + 1];
                if (!stringify_into(p, val, out, true, depth + 1)) { p.stack.pop_back(); return false; }
            }
            p.stack.pop_back();
            out->push_back('}');
            return true;
        }
        case ObjType::Closure: {
            auto* c = static_cast<ClosureObj*>(as_obj(w));
            const FuncRec& f = p.runtime().image().func(c->func);
            out->append("<fn ");
            out->append(p.runtime().image().str(f.name).str());
            out->push_back('>');
            return true;
        }
        case ObjType::Pap: out->append("<partial application>"); return true;
        case ObjType::Native: {
            auto* nat = static_cast<NativeObj*>(as_obj(w));
            out->append("<fn ");
            std::string tmp;
            stringify_into(p, nat->name, &tmp, false, depth + 1);
            out->append(tmp);
            out->push_back('>');
            return true;
        }
        case ObjType::Module: {
            out->append("<module ");
            std::string tmp;
            stringify_into(p, static_cast<ModuleObj*>(as_obj(w))->name, &tmp, false, depth + 1);
            out->append(tmp);
            out->push_back('>');
            return true;
        }
        case ObjType::ErrorBox: {
            auto* e = static_cast<ErrorObj*>(as_obj(w));
            out->append("<error ");
            std::string tmp;
            stringify_into(p, e->kind, &tmp, true, depth + 1);
            out->append(tmp);
            out->push_back(' ');
            tmp.clear();
            stringify_into(p, e->payload, &tmp, false, depth + 1);
            out->append(tmp);
            out->push_back('>');
            return true;
        }
        case ObjType::Pid: {
            char buf[48];
            std::snprintf(buf, sizeof buf, "<process %" PRIu64 ">",
                          static_cast<PidObj*>(as_obj(w))->id);
            out->append(buf);
            return true;
        }
        default: out->append("<value>"); return true;
    }
}

}  // namespace

bool stringify(Process& p, Value v, std::string* out) {
    return stringify_into(p, v, out, false, 0);
}

// ---------------------------------------------------------------------------
// Builtins
// ---------------------------------------------------------------------------

namespace {

NativeResult bi_spawn(Process& p, Value, Value* args, uint32_t) {
    // args[0] is deliberately unforced: it is the work the new process is
    // meant to do, and forcing it here would do that work on this process.
    uint64_t pid = p.runtime().scheduler()->spawn_from(p, args[0]);
    if (pid == 0) {
        return NativeResult::raise(
            raise_error(p, well_known(p.runtime()).error, "cannot spawn: the runtime is shutting down"));
    }
    return NativeResult::ok(p.heap().make_pid(pid));
}

NativeResult bi_join(Process& p, Value, Value* args, uint32_t) {
    if (!is_obj(args[0], ObjType::Pid)) {
        return NativeResult::raise(
            raise_error(p, well_known(p.runtime()).type_error, "join! needs a process"));
    }
    uint64_t target = static_cast<PidObj*>(as_obj(args[0]))->id;
    Value out;
    Scheduler::JoinState st = p.runtime().scheduler()->join(p, target, &out);
    switch (st) {
        case Scheduler::JoinState::Ready: return NativeResult::ok(out);
        case Scheduler::JoinState::Failed: return NativeResult::raise(out);
        case Scheduler::JoinState::Blocked: return NativeResult::block();
    }
    return NativeResult::ok(UNIT);
}

NativeResult bi_send(Process& p, Value, Value* args, uint32_t) {
    if (!is_obj(args[0], ObjType::Pid)) {
        return NativeResult::raise(
            raise_error(p, well_known(p.runtime()).type_error, "send! needs a process"));
    }
    uint64_t target = static_cast<PidObj*>(as_obj(args[0]))->id;
    // The message must be fully evaluated before it leaves: a thunk carries a
    // frame that points into this process's heap, which the receiver cannot see.
    Value forced;
    if (!force_deep(p, args[1], &forced)) return NativeResult::raise(p.result);
    if (!p.runtime().scheduler()->send(p, target, forced)) {
        return NativeResult::raise(raise_error(p, well_known(p.runtime()).error,
                                               "no such thread"));
    }
    return NativeResult::ok(UNIT);
}

NativeResult bi_recv(Process& p, Value, Value*, uint32_t) {
    Value out;
    if (p.runtime().scheduler()->receive(p, &out)) return NativeResult::ok(out);
    return NativeResult::block();
}

NativeResult bi_self(Process& p, Value, Value*, uint32_t) {
    return NativeResult::ok(p.heap().make_pid(p.id()));
}

NativeResult bi_raise(Process& p, Value, Value* args, uint32_t) {
    Value v = resolve(args[0]);
    if (is_obj(v, ObjType::ErrorBox)) return NativeResult::raise(v);
    return NativeResult::raise(p.heap().make_error(make_atom(well_known(p.runtime()).error), v));
}

NativeResult bi_type_of(Process& p, Value, Value* args, uint32_t) {
    const char* name = "unknown";
    switch (surface_type(args[0])) {
        case DREAM_TYPE_INTEGER: name = "integer"; break;
        case DREAM_TYPE_FLOAT: name = "float"; break;
        case DREAM_TYPE_CHAR: name = "char"; break;
        case DREAM_TYPE_BOOL: name = "bool"; break;
        case DREAM_TYPE_UNIT: name = "unit"; break;
        case DREAM_TYPE_STRING: name = "string"; break;
        case DREAM_TYPE_ATOM: name = "atom"; break;
        case DREAM_TYPE_LIST: name = "list"; break;
        case DREAM_TYPE_ARRAY: name = "array"; break;
        case DREAM_TYPE_MAP: name = "map"; break;
        case DREAM_TYPE_MODULE: name = "module"; break;
        case DREAM_TYPE_ERROR: name = "error"; break;
        case DREAM_TYPE_PROCESS: name = "process"; break;
        case DREAM_TYPE_PURE_FN: {
            // Tell the two function types apart via the function record, which
            // is where the compiler recorded the `!` on the name.
            Value v = resolve(args[0]);
            bool impure = false;
            if (is_obj(v, ObjType::Closure)) {
                auto* c = static_cast<ClosureObj*>(as_obj(v));
                impure = (p.runtime().image().func(c->func).flags & FN_IMPURE) != 0;
            } else {
                impure = true;  // builtins and host natives perform effects
            }
            name = impure ? "impure_fn" : "pure_fn";
            break;
        }
        default: break;
    }
    return NativeResult::ok(make_atom(p.runtime().intern_atom(name)));
}

NativeResult bi_to_string(Process& p, Value, Value* args, uint32_t) {
    std::string s;
    if (!stringify(p, args[0], &s)) return NativeResult::raise(p.result);
    return NativeResult::ok(p.heap().make_string(s.data(), uint32_t(s.size())));
}

NativeResult bi_len(Process& p, Value, Value* args, uint32_t) {
    Value v = resolve(args[0]);
    int64_t n = 0;
    if (is_nil(v)) {
        n = 0;
    } else if (is_obj(v, ObjType::Str)) {
        n = static_cast<StrObj*>(as_obj(v))->len;
    } else if (is_obj(v, ObjType::Array)) {
        n = static_cast<ArrayObj*>(as_obj(v))->len;
    } else if (is_obj(v, ObjType::Map)) {
        n = static_cast<MapObj*>(as_obj(v))->count;
    } else if (is_obj(v, ObjType::Cons)) {
        Value cur = v;
        for (;;) {
            Value w;
            if (!force_whnf(p, cur, &w)) return NativeResult::raise(p.result);
            if (!is_obj(w, ObjType::Cons)) break;
            ++n;
            cur = static_cast<ConsObj*>(as_obj(w))->tail;
        }
    } else {
        return NativeResult::raise(
            raise_error(p, well_known(p.runtime()).type_error, "len needs a sized value"));
    }
    return NativeResult::ok(make_fixnum(n));
}

// Order defines the builtin id and must match the compiler's table.
/// `strict! e` -- evaluate `e` all the way down, then hand it back.
///
/// Laziness is the default, and usually right, but it has one sharp edge:
/// an effect in a lazy position does not happen until something forces it, and
/// "something" may be much later or never. `list.map (fn x -> spawn! ..) xs`
/// builds a list of *thunks*, and the spawns only run as each element is
/// forced -- which serializes the very thing that was meant to run at once.
///
/// `strict!` is the way to say "now". It forces deeply rather than to weak
/// head normal form, because forcing the list without forcing its elements
/// would leave the effects exactly where they were.
///
/// It is impure by name, which is right: forcing is when effects happen.
NativeResult bi_strict(Process& p, Value, Value* args, uint32_t) {
    Value out;
    if (!force_deep(p, args[0], &out)) {
        // Either the value raised, or a blocking operation inside it gave up
        // and asked to be parked. `resume_native` turns the second case into a
        // Block; here it only has to not swallow the first.
        return NativeResult::raise(p.result);
    }
    return NativeResult::ok(out);
}

// --- what `match` compiles to ------------------------------------------------
//
// These exist so pattern matching needs no new opcodes and no import: a
// builtin is resolved by id, so `match` works in a module that imports
// nothing. Each returns the piece *unforced*, because a pattern must force
// only as much as deciding takes -- `[x, ..rest]` looks at the first cell and
// neither `x` nor `rest`.

NativeResult bi_match_is_cons(Process& p, Value, Value* args, uint32_t) {
    Value v;
    if (!force_whnf(p, args[0], &v)) return NativeResult::raise(p.result);
    return NativeResult::ok(make_bool(is_obj(v, ObjType::Cons)));
}

NativeResult bi_match_head(Process& p, Value, Value* args, uint32_t) {
    Value v;
    if (!force_whnf(p, args[0], &v)) return NativeResult::raise(p.result);
    if (!is_obj(v, ObjType::Cons)) {
        return NativeResult::raise(
            raise_error(p, well_known(p.runtime()).type_error, "match_head needs a list cell"));
    }
    // Forced: a native's return value goes straight into the machine's
    // result, which every continuation takes to be already reduced. Handing
    // back the raw slot leaks a thunk -- `type_of` on one answers `:unknown`.
    Value head;
    if (!force_whnf(p, static_cast<ConsObj*>(as_obj(v))->head, &head)) {
        return NativeResult::raise(p.result);
    }
    return NativeResult::ok(head);
}

NativeResult bi_match_tail(Process& p, Value, Value* args, uint32_t) {
    Value v;
    if (!force_whnf(p, args[0], &v)) return NativeResult::raise(p.result);
    if (!is_obj(v, ObjType::Cons)) {
        return NativeResult::raise(
            raise_error(p, well_known(p.runtime()).type_error, "match_tail needs a list cell"));
    }
    Value tail;
    if (!force_whnf(p, static_cast<ConsObj*>(as_obj(v))->tail, &tail)) {
        return NativeResult::raise(p.result);
    }
    return NativeResult::ok(tail);
}

NativeResult bi_match_at(Process& p, Value, Value* args, uint32_t) {
    Value v;
    if (!force_whnf(p, args[0], &v)) return NativeResult::raise(p.result);
    Value i = resolve(args[1]);
    if (!is_obj(v, ObjType::Array) || !is_fixnum(i)) {
        return NativeResult::raise(
            raise_error(p, well_known(p.runtime()).type_error, "match_at needs an array"));
    }
    auto* a = static_cast<ArrayObj*>(as_obj(v));
    int64_t k = fixnum_value(i);
    if (k < 0 || k >= a->len) {
        return NativeResult::raise(raise_error(p, p.runtime().intern_atom("out_of_bounds"),
                                               "match_at is outside the array"));
    }
    Value element;
    if (!force_whnf(p, a->items()[k], &element)) return NativeResult::raise(p.result);
    return NativeResult::ok(element);
}

/// `[value]` when the key is there, `[]` when it is not -- one call rather
/// than a `has` followed by a `get`, so the map is looked up once and there is
/// no sentinel that a real value could collide with.
NativeResult bi_match_key(Process& p, Value, Value* args, uint32_t) {
    Value m;
    if (!force_whnf(p, args[0], &m)) return NativeResult::raise(p.result);
    if (!is_obj(m, ObjType::Map)) {
        return NativeResult::raise(
            raise_error(p, well_known(p.runtime()).type_error, "match_key needs a map"));
    }
    Value found;
    if (!map_lookup(p, m, args[1], &found)) return NativeResult::ok(NIL);
    Value held;
    if (!force_whnf(p, found, &held)) return NativeResult::raise(p.result);
    return NativeResult::ok(p.heap().make_cons(held, NIL));
}

const BuiltinDef BUILTINS[] = {
    {"spawn!", 1, 0b0, bi_spawn},
    {"join!", 1, 0b1, bi_join},
    {"send!", 2, 0b11, bi_send},
    {"recv!", 1, 0b1, bi_recv},
    {"self!", 1, 0b1, bi_self},
    {"raise!", 1, 0b1, bi_raise},
    {"type_of", 1, 0b1, bi_type_of},
    {"to_string", 1, 0b1, bi_to_string},
    {"len", 1, 0b1, bi_len},
    // Mask 0: the argument must arrive unforced, or the deep force below
    // would be handed something already reduced to weak head normal form by
    // the caller and could not report a raise from inside it.
    {"strict!", 1, 0b0, bi_strict},
    // Mask 0 throughout: these force what they must themselves, and handing
    // back an unforced piece is the point.
    {"match_is_cons", 1, 0b0, bi_match_is_cons},
    {"match_head", 1, 0b0, bi_match_head},
    {"match_tail", 1, 0b0, bi_match_tail},
    {"match_at", 2, 0b0, bi_match_at},
    {"match_key", 2, 0b10, bi_match_key},
};

}  // namespace

const BuiltinDef& builtin_def(uint32_t id) { return BUILTINS[id]; }
uint32_t builtin_count() { return uint32_t(sizeof(BUILTINS) / sizeof(BUILTINS[0])); }

// ---------------------------------------------------------------------------
// std.console
// ---------------------------------------------------------------------------

namespace {

NativeResult write_values(Process& p, Value* args, uint32_t argc, bool newline, std::FILE* out) {
    std::string text;
    for (uint32_t i = 0; i < argc; ++i) {
        if (!stringify(p, args[i], &text)) return NativeResult::raise(p.result);
    }
    if (newline) text.push_back('\n');
    // One write, so interleaved output from concurrent processes stays legible.
    std::fwrite(text.data(), 1, text.size(), out);
    std::fflush(out);
    return NativeResult::ok(UNIT);
}

NativeResult con_print(Process& p, Value, Value* args, uint32_t argc) {
    return write_values(p, args, argc, true, stdout);
}
NativeResult con_write(Process& p, Value, Value* args, uint32_t argc) {
    return write_values(p, args, argc, false, stdout);
}
NativeResult con_line(Process& p, Value, Value* args, uint32_t argc) {
    return write_values(p, args, argc, true, stdout);
}
NativeResult con_error(Process& p, Value, Value* args, uint32_t argc) {
    return write_values(p, args, argc, true, stderr);
}

NativeResult math_sqrt(Process& p, Value, Value* args, uint32_t) {
    Value v = resolve(args[0]);
    double d = is_fixnum(v) ? double(fixnum_value(v))
                            : (is_obj(v, ObjType::Float)
                                   ? static_cast<FloatObj*>(as_obj(v))->value
                                   : NAN);
    if (std::isnan(d) && !is_obj(v, ObjType::Float)) {
        return NativeResult::raise(
            raise_error(p, well_known(p.runtime()).type_error, "sqrt needs a number"));
    }
    return NativeResult::ok(p.heap().make_float(std::sqrt(d)));
}

NativeResult math_abs(Process& p, Value, Value* args, uint32_t) {
    Value v = resolve(args[0]);
    if (is_fixnum(v)) {
        int64_t n = fixnum_value(v);
        return NativeResult::ok(make_integer(p, n < 0 ? -n : n));
    }
    if (is_obj(v, ObjType::Float)) {
        return NativeResult::ok(
            p.heap().make_float(std::fabs(static_cast<FloatObj*>(as_obj(v))->value)));
    }
    return NativeResult::raise(
        raise_error(p, well_known(p.runtime()).type_error, "abs needs a number"));
}

NativeResult math_floor(Process& p, Value, Value* args, uint32_t) {
    Value v = resolve(args[0]);
    if (is_fixnum(v)) return NativeResult::ok(v);
    if (is_obj(v, ObjType::Float)) {
        return NativeResult::ok(
            make_integer(p, int64_t(std::floor(static_cast<FloatObj*>(as_obj(v))->value))));
    }
    return NativeResult::raise(
        raise_error(p, well_known(p.runtime()).type_error, "floor needs a number"));
}

NativeResult list_head(Process& p, Value, Value* args, uint32_t) {
    Value v = resolve(args[0]);
    if (!is_obj(v, ObjType::Cons)) {
        return NativeResult::raise(
            raise_error(p, well_known(p.runtime()).type_error, "head needs a non-empty list"));
    }
    Value out;
    if (!force_whnf(p, static_cast<ConsObj*>(as_obj(v))->head, &out)) {
        return NativeResult::raise(p.result);
    }
    return NativeResult::ok(out);
}

NativeResult list_tail(Process& p, Value, Value* args, uint32_t) {
    Value v = resolve(args[0]);
    if (!is_obj(v, ObjType::Cons)) {
        return NativeResult::raise(
            raise_error(p, well_known(p.runtime()).type_error, "tail needs a non-empty list"));
    }
    Value out;
    if (!force_whnf(p, static_cast<ConsObj*>(as_obj(v))->tail, &out)) {
        return NativeResult::raise(p.result);
    }
    return NativeResult::ok(out);
}

NativeResult list_cons(Process& p, Value, Value* args, uint32_t) {
    return NativeResult::ok(p.heap().make_cons(args[0], args[1]));
}

NativeResult list_is_empty(Process& p, Value, Value* args, uint32_t) {
    return NativeResult::ok(make_bool(is_nil(resolve(args[0]))));
}

}  // namespace

const NativeDef* ModuleDef::find(const StringRef& member) const {
    for (const auto& m : members) {
        if (member.equals(m.name)) return &m;
    }
    return nullptr;
}

namespace {

NativeResult vm_processes(Process& p, Value, Value*, uint32_t) {
    return NativeResult::ok(make_fixnum(int64_t(p.runtime().live_process_count())));
}

NativeResult vm_reductions(Process& p, Value, Value*, uint32_t) {
    return NativeResult::ok(make_integer(p, int64_t(p.total_reductions)));
}

NativeResult vm_collections(Process& p, Value, Value*, uint32_t) {
    return NativeResult::ok(make_integer(p, int64_t(p.heap().collections())));
}

NativeResult vm_heap_bytes(Process& p, Value, Value*, uint32_t) {
    return NativeResult::ok(make_integer(p, int64_t(p.heap().bytes_allocated())));
}

NativeResult vm_modules(Process& p, Value, Value*, uint32_t) {
    // Built back to front so the list comes out in image order.
    const Image& img = p.runtime().image();
    Value list = NIL;
    for (uint32_t i = img.module_count(); i-- > 0;) {
        StringRef name = img.str(img.module(i).name);
        list = p.heap().make_cons(p.heap().make_string(name.data, name.len), list);
    }
    return NativeResult::ok(list);
}

NativeResult vm_has_ffi(Process&, Value, Value*, uint32_t) {
    return NativeResult::ok(make_bool(ffi_available()));
}

// --- introspection ----------------------------------------------------------
//
// What the VM is doing, as ordinary Dream values. This exists because the
// alternative is a debugger and a print statement: when a program stops making
// progress, the question is always "which processes are stuck, and on what",
// and nothing in the language could answer it before.

/// Build a map from key/value pairs, in one place so every report below has
/// the same shape.
Value info_map(Process& p, std::initializer_list<std::pair<const char*, Value>> pairs) {
    // A map's capacity must be a power of two: lookup masks with `cap - 1`,
    // and anything else turns probing into a loop that never terminates.
    // Twice the entry count keeps the table from filling up.
    uint32_t cap = 8;
    while (cap < pairs.size() * 2) cap *= 2;
    Value m = p.heap().make_map(cap);
    for (auto& [key, value] : pairs) {
        map_insert(p, resolve(m), make_atom(p.runtime().intern_atom(key)), value);
        m = resolve(m);
    }
    return m;
}

Value atom_of(Process& p, const char* name) {
    return make_atom(p.runtime().intern_atom(name));
}

Value text_of(Process& p, const std::string& s) {
    return p.heap().make_string(s.data(), uint32_t(s.size()));
}

const char* status_name(ProcStatus s) {
    switch (s) {
        case ProcStatus::Runnable: return "runnable";
        case ProcStatus::Running: return "running";
        case ProcStatus::Waiting: return "waiting";
        case ProcStatus::Finished: return "finished";
        case ProcStatus::Failed: return "failed";
    }
    return "unknown";
}

/// One process, as a map. `waiting_on` is the part worth having: a process
/// parked on a message and one parked on a socket look identical without it.
Value process_info(Process& p, Process& about) {
    ProcStatus st = about.status.load(std::memory_order_relaxed);
    WaitReason reason = about.wait_reason.load(std::memory_order_relaxed);
    int fd = about.wait_fd.load(std::memory_order_relaxed);
    return info_map(
        p, {
               {"id", make_integer(p, int64_t(about.id()))},
               {"status", atom_of(p, status_name(st))},
               {"waiting_on", st == ProcStatus::Waiting ? atom_of(p, wait_reason_name(reason))
                                                        : atom_of(p, "none")},
               {"fd", make_integer(p, int64_t(fd))},
               {"reductions", make_integer(p, int64_t(about.total_reductions))},
               {"heap_bytes", make_integer(p, int64_t(about.heap().bytes_allocated()))},
               {"collections", make_integer(p, int64_t(about.heap().collections()))},
               {"mailbox", make_integer(p, int64_t(about.mailbox.size()))},
               {"failed", make_bool(about.failed)},
           });
}

NativeResult vm_process_list(Process& p, Value, Value*, uint32_t) {
    auto all = p.runtime().all_processes();
    Value list = NIL;
    for (size_t i = all.size(); i-- > 0;) {
        list = p.heap().make_cons(process_info(p, *all[i]), list);
    }
    return NativeResult::ok(list);
}

NativeResult vm_process_info(Process& p, Value, Value* args, uint32_t) {
    Value v = resolve(args[0]);
    uint64_t id = 0;
    if (is_obj(v, ObjType::Pid)) {
        id = static_cast<PidObj*>(as_obj(v))->id;
    } else if (is_fixnum(v)) {
        id = uint64_t(fixnum_value(v));
    } else {
        return NativeResult::raise(raise_error(p, well_known(p.runtime()).type_error,
                                               "process_info! needs a process or an id"));
    }
    auto proc = p.runtime().find_process(id);
    if (!proc) return NativeResult::ok(UNIT);
    return NativeResult::ok(process_info(p, *proc));
}

NativeResult vm_scheduler(Process& p, Value, Value*, uint32_t) {
    Scheduler* s = p.runtime().scheduler();
    if (!s) return NativeResult::ok(UNIT);
    return NativeResult::ok(info_map(
        p, {
               {"workers", make_integer(p, int64_t(s->worker_count()))},
               {"idle", make_integer(p, int64_t(s->idle_workers()))},
               {"live", make_integer(p, int64_t(s->live()))},
               {"runnable", make_integer(p, int64_t(s->runnable()))},
               {"queued", make_integer(p, int64_t(s->queued()))},
               // Processes waiting on a descriptor. While this is above zero
               // the system is not deadlocked however idle it looks: the
               // kernel still owes somebody an answer.
               {"io_waiters", make_integer(p, int64_t(s->io_waiters()))},
               {"reductions", make_integer(p, int64_t(s->total_reductions()))},
               {"deadlocked", make_bool(s->deadlocked())},
           }));
}

NativeResult vm_io(Process& p, Value, Value*, uint32_t) {
    auto handles = io_snapshot();
    Value list = NIL;
    for (size_t i = handles.size(); i-- > 0;) {
        const IoHandleInfo& h = handles[i];
        const char* kind = h.kind == HandleKind::File     ? "file"
                         : h.kind == HandleKind::Listener ? "listener"
                                                          : "stream";
        list = p.heap().make_cons(
            info_map(p, {
                            {"handle", make_integer(p, h.handle)},
                            {"fd", make_integer(p, int64_t(h.fd))},
                            {"kind", atom_of(p, kind)},
                            {"busy", make_integer(p, int64_t(h.busy))},
                            {"waiting", h.waiting_pid == 0
                                            ? UNIT
                                            : make_integer(p, int64_t(h.waiting_pid))},
                        }),
            list);
    }
    return NativeResult::ok(list);
}

NativeResult vm_async_io(Process&, Value, Value*, uint32_t) {
    return NativeResult::ok(make_bool(io_async_available()));
}

/// A readable snapshot on stderr, for when a program has stopped making
/// progress and the question is why.
NativeResult vm_dump(Process& p, Value, Value*, uint32_t) {
    std::string out = "--- vm ---\n";
    if (Scheduler* s = p.runtime().scheduler()) {
        out += "scheduler: workers=" + std::to_string(s->worker_count())
             + " idle=" + std::to_string(s->idle_workers())
             + " live=" + std::to_string(s->live())
             + " runnable=" + std::to_string(s->runnable())
             + " queued=" + std::to_string(s->queued())
             + " io_waiters=" + std::to_string(s->io_waiters())
             + (s->deadlocked() ? " DEADLOCKED" : "") + "\n";
    }
    for (auto& proc : p.runtime().all_processes()) {
        ProcStatus st = proc->status.load(std::memory_order_relaxed);
        out += "  process " + std::to_string(proc->id()) + ": " + status_name(st);
        if (st == ProcStatus::Waiting) {
            WaitReason r = proc->wait_reason.load(std::memory_order_relaxed);
            out += " on " + std::string(wait_reason_name(r));
            int fd = proc->wait_fd.load(std::memory_order_relaxed);
            if (r == WaitReason::Io && fd >= 0) out += " fd=" + std::to_string(fd);
        }
        out += " reductions=" + std::to_string(proc->total_reductions)
             + " mailbox=" + std::to_string(proc->mailbox.size()) + "\n";
    }
    for (const IoHandleInfo& h : io_snapshot()) {
        const char* kind = h.kind == HandleKind::File     ? "file"
                         : h.kind == HandleKind::Listener ? "listener"
                                                          : "stream";
        out += "  handle " + std::to_string(h.handle) + ": fd=" + std::to_string(h.fd) + " "
             + kind + " busy=" + std::to_string(h.busy);
        if (h.waiting_pid) out += " waiting=" + std::to_string(h.waiting_pid);
        out += "\n";
    }
    std::fwrite(out.data(), 1, out.size(), stderr);
    std::fflush(stderr);
    return NativeResult::ok(UNIT);
}

}  // namespace

ModuleDef make_vm_module() {
    return ModuleDef{"std.vm",
                     {
                         {"processes!", 1, 0b1, vm_processes},
                         {"reductions!", 1, 0b1, vm_reductions},
                         {"collections!", 1, 0b1, vm_collections},
                         {"heap_bytes!", 1, 0b1, vm_heap_bytes},
                         {"modules!", 1, 0b1, vm_modules},
                         {"has_ffi", 1, 0b1, vm_has_ffi},
                         {"async_io", 1, 0b1, vm_async_io},
                         {"processes_info!", 1, 0b1, vm_process_list},
                         {"process_info!", 1, 0b1, vm_process_info},
                         {"scheduler!", 1, 0b1, vm_scheduler},
                         {"io!", 1, 0b1, vm_io},
                         {"dump!", 1, 0b1, vm_dump},
                     }};
}

ModuleDef make_console_module() {
    // One argument each. `print!`, `line!` and `error!` add a newline;
    // `write!` does not.
    //
    // These used to be variadic, printing every argument in turn. It read
    // well -- `print! "x = " x " y = " y` -- but a variadic function can never
    // be passed too many arguments, so nothing was ever an error and a stray
    // value on the end of a line was silently printed instead of being the
    // thing the line evaluated to:
    //
    //     if n < 0 { 1 } else { console.print! total 0 }   // prints "...0"
    //
    // That block means to answer `0`. Variadic makes it print `0` and answer
    // unit, and no arity check can ever catch it. With one argument the same
    // line is `(print! total) 0`, which raises `:not_a_function` -- loudly,
    // where the mistake is.
    //
    // Several values are joined by the caller, which is what `+` is for.
    // `to_string` is the identity on a string, so it is always safe to reach
    // for: `print! ("x = " + to_string x)`.
    return ModuleDef{"std.console",
                     {
                         {"print!", 1, 0b1, con_print},
                         {"write!", 1, 0b1, con_write},
                         {"line!", 1, 0b1, con_line},
                         {"error!", 1, 0b1, con_error},
                     }};
}

ModuleDef make_math_module() {
    return ModuleDef{"std.math",
                     {
                         {"sqrt", 1, 0b1, math_sqrt},
                         {"abs", 1, 0b1, math_abs},
                         {"floor", 1, 0b1, math_floor},
                     }};
}


// ---------------------------------------------------------------------------
// std.core -- the operations the language cannot express in itself
//
// Everything here is either a primitive the representation hides (a string's
// bytes, a map's buckets) or something that must be a single machine step for
// the rest of the library to be worth writing. Anything that can be written in
// Dream is written in Dream, in mind/std.
// ---------------------------------------------------------------------------

namespace {

StrObj* as_string(Value v) {
    v = resolve(v);
    return is_obj(v, ObjType::Str) ? static_cast<StrObj*>(as_obj(v)) : nullptr;
}

NativeResult type_fail(Process& p, const char* what) {
    return NativeResult::raise(raise_error(p, well_known(p.runtime()).type_error, what));
}

/// Decode one UTF-8 scalar starting at `i`, advancing it. Invalid bytes are
/// returned as U+FFFD and consume one byte, so decoding always terminates.
uint32_t utf8_next(const char* data, uint32_t len, uint32_t* i) {
    auto byte = [&](uint32_t k) { return uint8_t(data[k]); };
    uint32_t c = byte(*i);
    uint32_t need = c < 0x80 ? 0 : (c >> 5) == 0x6 ? 1 : (c >> 4) == 0xE ? 2 : (c >> 3) == 0x1E ? 3 : 0xFF;
    if (need == 0xFF || *i + need >= len + (need ? 0 : 1)) {
        ++*i;
        return 0xFFFD;
    }
    if (need == 0) {
        ++*i;
        return c;
    }
    uint32_t cp = c & (0x3F >> need);
    for (uint32_t k = 1; k <= need; ++k) {
        uint32_t cc = byte(*i + k);
        if ((cc & 0xC0) != 0x80) {
            ++*i;
            return 0xFFFD;
        }
        cp = (cp << 6) | (cc & 0x3F);
    }
    *i += need + 1;
    return cp;
}

void utf8_encode(uint32_t cp, std::string* out) {
    if (cp < 0x80) {
        out->push_back(char(cp));
    } else if (cp < 0x800) {
        out->push_back(char(0xC0 | (cp >> 6)));
        out->push_back(char(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out->push_back(char(0xE0 | (cp >> 12)));
        out->push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(char(0x80 | (cp & 0x3F)));
    } else {
        out->push_back(char(0xF0 | (cp >> 18)));
        out->push_back(char(0x80 | ((cp >> 12) & 0x3F)));
        out->push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(char(0x80 | (cp & 0x3F)));
    }
}

// --- strings ---

NativeResult core_str_len(Process& p, Value, Value* args, uint32_t) {
    StrObj* s = as_string(args[0]);
    if (!s) return type_fail(p, "str_len needs a string");
    return NativeResult::ok(make_fixnum(s->len));
}

NativeResult core_str_chars(Process& p, Value, Value* args, uint32_t) {
    StrObj* s = as_string(args[0]);
    if (!s) return type_fail(p, "str_chars needs a string");
    // Decoded back to front so the list comes out in order without reversing.
    std::vector<uint32_t> cps;
    for (uint32_t i = 0; i < s->len;) cps.push_back(utf8_next(s->data(), s->len, &i));
    Value list = NIL;
    for (size_t k = cps.size(); k-- > 0;) {
        list = p.heap().make_cons(make_char(cps[k]), list);
    }
    return NativeResult::ok(list);
}

NativeResult core_str_of_chars(Process& p, Value, Value* args, uint32_t) {
    std::string out;
    Value cur = args[0];
    for (;;) {
        Value w;
        if (!force_whnf(p, cur, &w)) return NativeResult::raise(p.result);
        if (is_nil(w)) break;
        if (!is_obj(w, ObjType::Cons)) return type_fail(p, "str_of_chars needs a list of chars");
        auto* c = static_cast<ConsObj*>(as_obj(w));
        Value head;
        if (!force_whnf(p, c->head, &head)) return NativeResult::raise(p.result);
        if (!is_char(head)) return type_fail(p, "str_of_chars needs a list of chars");
        utf8_encode(uint32_t(imm_payload(head)), &out);
        cur = c->tail;
    }
    return NativeResult::ok(p.heap().make_string(out.data(), uint32_t(out.size())));
}

/// The inverse of `str_byte`: a list of byte values becomes a string holding
/// exactly those bytes.
///
/// `str_of_chars` cannot do this. It takes Unicode scalars and UTF-8-encodes
/// them, so byte 0x80 comes back out as the two bytes 0xC2 0x80 -- which is
/// right for text and wrong for everything else. A string is a length and a
/// byte buffer, `io.write!` puts those bytes out untouched, and `str_byte`
/// reads them back, so this is the one piece missing before a Dream program
/// can produce binary output rather than only consume it.
NativeResult core_str_of_bytes(Process& p, Value, Value* args, uint32_t) {
    std::string out;
    Value cur = args[0];
    for (;;) {
        Value w;
        if (!force_whnf(p, cur, &w)) return NativeResult::raise(p.result);
        if (is_nil(w)) break;
        if (!is_obj(w, ObjType::Cons)) return type_fail(p, "str_of_bytes needs a list of integers");
        auto* c = static_cast<ConsObj*>(as_obj(w));
        Value head;
        if (!force_whnf(p, c->head, &head)) return NativeResult::raise(p.result);
        if (!is_fixnum(head)) return type_fail(p, "str_of_bytes needs a list of integers");
        int64_t b = fixnum_value(head);
        if (b < 0 || b > 255) {
            return NativeResult::raise(raise_error(
                p, well_known(p.runtime()).type_error,
                "str_of_bytes needs bytes in 0..255, got " + std::to_string(b)));
        }
        out.push_back(char(uint8_t(b)));
        cur = c->tail;
    }
    return NativeResult::ok(p.heap().make_string(out.data(), uint32_t(out.size())));
}

NativeResult core_str_slice(Process& p, Value, Value* args, uint32_t) {
    StrObj* s = as_string(args[0]);
    Value from = resolve(args[1]);
    Value count = resolve(args[2]);
    if (!s || !is_fixnum(from) || !is_fixnum(count)) {
        return type_fail(p, "str_slice needs a string, a start and a length");
    }
    // Clamped rather than raising: slicing past the end is how every loop that
    // walks a string finishes, and an error there would be noise.
    int64_t start = fixnum_value(from);
    int64_t n = fixnum_value(count);
    if (start < 0) start = 0;
    if (start > s->len) start = s->len;
    if (n < 0) n = 0;
    if (start + n > s->len) n = s->len - start;
    return NativeResult::ok(p.heap().make_string(s->data() + start, uint32_t(n)));
}

NativeResult core_str_find(Process& p, Value, Value* args, uint32_t) {
    StrObj* hay = as_string(args[0]);
    StrObj* needle = as_string(args[1]);
    Value from = resolve(args[2]);
    if (!hay || !needle || !is_fixnum(from)) {
        return type_fail(p, "str_find needs two strings and a start offset");
    }
    int64_t start = fixnum_value(from);
    if (start < 0) start = 0;
    if (needle->len == 0) {
        return NativeResult::ok(make_fixnum(start <= hay->len ? start : -1));
    }
    if (start + needle->len > hay->len) return NativeResult::ok(make_fixnum(-1));
    for (uint32_t i = uint32_t(start); i + needle->len <= hay->len; ++i) {
        if (std::memcmp(hay->data() + i, needle->data(), needle->len) == 0) {
            return NativeResult::ok(make_fixnum(i));
        }
    }
    return NativeResult::ok(make_fixnum(-1));
}

NativeResult core_str_byte(Process& p, Value, Value* args, uint32_t) {
    StrObj* s = as_string(args[0]);
    Value i = resolve(args[1]);
    if (!s || !is_fixnum(i)) return type_fail(p, "str_byte needs a string and an index");
    int64_t k = fixnum_value(i);
    if (k < 0 || k >= s->len) return NativeResult::ok(make_fixnum(-1));
    return NativeResult::ok(make_fixnum(uint8_t(s->data()[k])));
}

// --- chars ---

NativeResult core_char_code(Process& p, Value, Value* args, uint32_t) {
    Value v = resolve(args[0]);
    if (!is_char(v)) return type_fail(p, "char_code needs a char");
    return NativeResult::ok(make_fixnum(int64_t(imm_payload(v))));
}

NativeResult core_char_of_code(Process& p, Value, Value* args, uint32_t) {
    Value v = resolve(args[0]);
    if (!is_fixnum(v)) return type_fail(p, "char_of_code needs an integer");
    int64_t c = fixnum_value(v);
    if (c < 0 || c > 0x10FFFF) return type_fail(p, "that is not a Unicode scalar value");
    return NativeResult::ok(make_char(uint32_t(c)));
}

// --- numbers ---

NativeResult core_to_float(Process& p, Value, Value* args, uint32_t) {
    Value v = resolve(args[0]);
    if (is_fixnum(v)) return NativeResult::ok(p.heap().make_float(double(fixnum_value(v))));
    if (is_obj(v, ObjType::Float)) return NativeResult::ok(v);
    return type_fail(p, "to_float needs a number");
}

NativeResult core_to_int(Process& p, Value, Value* args, uint32_t) {
    Value v = resolve(args[0]);
    if (is_fixnum(v)) return NativeResult::ok(v);
    if (is_obj(v, ObjType::Float)) {
        double d = static_cast<FloatObj*>(as_obj(v))->value;
        // Truncates toward zero, like C. `math.floor` is there for the other
        // rounding, so this one does not have to guess.
        return NativeResult::ok(make_integer(p, int64_t(d)));
    }
    return type_fail(p, "to_int needs a number");
}

NativeResult core_parse_int(Process& p, Value, Value* args, uint32_t) {
    StrObj* s = as_string(args[0]);
    if (!s) return type_fail(p, "parse_int needs a string");
    std::string text(s->data(), s->len);
    size_t used = 0;
    long long v = 0;
    try {
        v = std::stoll(text, &used, 10);
    } catch (...) {
        return NativeResult::ok(UNIT);
    }
    // The whole string has to be a number, or "12abc" would parse as 12.
    while (used < text.size() && std::isspace(uint8_t(text[used]))) ++used;
    if (used != text.size()) return NativeResult::ok(UNIT);
    return NativeResult::ok(make_integer(p, int64_t(v)));
}

NativeResult core_parse_float(Process& p, Value, Value* args, uint32_t) {
    StrObj* s = as_string(args[0]);
    if (!s) return type_fail(p, "parse_float needs a string");
    std::string text(s->data(), s->len);
    size_t used = 0;
    double v = 0;
    try {
        v = std::stod(text, &used);
    } catch (...) {
        return NativeResult::ok(UNIT);
    }
    while (used < text.size() && std::isspace(uint8_t(text[used]))) ++used;
    if (used != text.size()) return NativeResult::ok(UNIT);
    return NativeResult::ok(p.heap().make_float(v));
}

// --- arrays ---

NativeResult core_array_new(Process& p, Value, Value* args, uint32_t) {
    Value n = resolve(args[0]);
    if (!is_fixnum(n) || fixnum_value(n) < 0) return type_fail(p, "array_new needs a length");
    Value arr = p.heap().make_array(uint32_t(fixnum_value(n)));
    auto* a = static_cast<ArrayObj*>(as_obj(arr));
    for (uint32_t i = 0; i < a->len; ++i) a->items()[i] = args[1];
    return NativeResult::ok(arr);
}

NativeResult core_array_get(Process& p, Value, Value* args, uint32_t) {
    Value v = resolve(args[0]);
    Value i = resolve(args[1]);
    if (!is_obj(v, ObjType::Array) || !is_fixnum(i)) {
        return type_fail(p, "array_get needs an array and an index");
    }
    auto* a = static_cast<ArrayObj*>(as_obj(v));
    int64_t k = fixnum_value(i);
    if (k < 0 || k >= a->len) {
        return NativeResult::raise(raise_error(
            p, p.runtime().intern_atom("out_of_bounds"),
            "index " + std::to_string(k) + " is outside an array of " + std::to_string(a->len)));
    }
    // Forced, like `head` and `tail`: an array's elements are stored as thunks,
    // and a native's return value goes straight into `p.result`, which the
    // machine takes to be in weak head normal form. Handing back the raw slot
    // would leak a thunk into an operator's operand.
    Value out;
    if (!force_whnf(p, a->items()[k], &out)) return NativeResult::raise(p.result);
    return NativeResult::ok(out);
}

NativeResult core_array_set(Process& p, Value, Value* args, uint32_t) {
    Value v = resolve(args[0]);
    Value i = resolve(args[1]);
    if (!is_obj(v, ObjType::Array) || !is_fixnum(i)) {
        return type_fail(p, "array_set needs an array and an index");
    }
    auto* src = static_cast<ArrayObj*>(as_obj(v));
    int64_t k = fixnum_value(i);
    if (k < 0 || k >= src->len) {
        return NativeResult::raise(raise_error(
            p, p.runtime().intern_atom("out_of_bounds"),
            "index " + std::to_string(k) + " is outside an array of " + std::to_string(src->len)));
    }
    // Copies: values are immutable, so updating in place would be visible to
    // whoever else is holding this array.
    Value out = p.heap().make_array(src->len);
    src = static_cast<ArrayObj*>(as_obj(v));
    auto* dst = static_cast<ArrayObj*>(as_obj(out));
    for (uint32_t j = 0; j < src->len; ++j) dst->items()[j] = src->items()[j];
    dst->items()[k] = args[2];
    return NativeResult::ok(out);
}

NativeResult core_array_of_list(Process& p, Value, Value* args, uint32_t) {
    // Two passes: count, then fill. The elements stay lazy.
    const size_t base = p.stack.size();
    Value cur = args[0];
    for (;;) {
        Value w;
        if (!force_whnf(p, cur, &w)) {
            // A suspended force keeps its working stack above ours; cutting it
            // back would destroy the work that is waiting to be resumed.
            if (!p.force_blocked) p.stack.resize(base);
            return NativeResult::raise(p.result);
        }
        if (is_nil(w)) break;
        if (!is_obj(w, ObjType::Cons)) {
            p.stack.resize(base);
            return type_fail(p, "array_of_list needs a list");
        }
        auto* c = static_cast<ConsObj*>(as_obj(w));
        p.stack.push_back(c->head);
        cur = c->tail;
    }
    uint32_t n = uint32_t(p.stack.size() - base);
    Value arr = p.heap().make_array(n);
    auto* a = static_cast<ArrayObj*>(as_obj(arr));
    for (uint32_t i = 0; i < n; ++i) a->items()[i] = p.stack[base + i];
    p.stack.resize(base);
    return NativeResult::ok(arr);
}

NativeResult core_array_to_list(Process& p, Value, Value* args, uint32_t) {
    Value v = resolve(args[0]);
    if (!is_obj(v, ObjType::Array)) return type_fail(p, "array_to_list needs an array");
    uint32_t n = static_cast<ArrayObj*>(as_obj(v))->len;
    Value list = NIL;
    for (uint32_t i = n; i-- > 0;) {
        Value item = static_cast<ArrayObj*>(as_obj(v))->items()[i];
        list = p.heap().make_cons(item, list);
    }
    return NativeResult::ok(list);
}

// --- maps ---

NativeResult core_map_new(Process& p, Value, Value*, uint32_t) {
    return NativeResult::ok(p.heap().make_map(8));
}

NativeResult core_map_get(Process& p, Value, Value* args, uint32_t) {
    Value m = resolve(args[0]);
    if (!is_obj(m, ObjType::Map)) return type_fail(p, "map_get needs a map");
    Value found;
    // A map's values stay lazy, and so does the default -- an unused default
    // should cost nothing -- so whichever one is returned is forced here rather
    // than handed back as a thunk. See the note in `core_array_get`.
    Value out;
    if (!map_lookup(p, m, args[1], &found)) found = args[2];
    if (!force_whnf(p, found, &out)) return NativeResult::raise(p.result);
    return NativeResult::ok(out);
}

NativeResult core_map_has(Process& p, Value, Value* args, uint32_t) {
    Value m = resolve(args[0]);
    if (!is_obj(m, ObjType::Map)) return type_fail(p, "map_has needs a map");
    Value found;
    return NativeResult::ok(make_bool(map_lookup(p, m, args[1], &found)));
}

NativeResult core_map_put(Process& p, Value, Value* args, uint32_t) {
    Value m = resolve(args[0]);
    if (!is_obj(m, ObjType::Map)) return type_fail(p, "map_put needs a map");
    auto* src = static_cast<MapObj*>(as_obj(m));
    // Copy, for the same reason arrays copy.
    Value out = p.heap().make_map(src->cap);
    src = static_cast<MapObj*>(as_obj(m));
    for (uint32_t i = 0; i < src->cap; ++i) {
        Value k = src->entries()[i * 2];
        if (k == NIL_SLOT) continue;
        Value val = src->entries()[i * 2 + 1];
        map_insert(p, resolve(out), k, val);
        out = resolve(out);
        src = static_cast<MapObj*>(as_obj(m));
    }
    map_insert(p, resolve(out), args[1], args[2]);
    return NativeResult::ok(resolve(out));
}

NativeResult core_map_remove(Process& p, Value, Value* args, uint32_t) {
    Value m = resolve(args[0]);
    if (!is_obj(m, ObjType::Map)) return type_fail(p, "map_remove needs a map");
    auto* src = static_cast<MapObj*>(as_obj(m));
    Value out = p.heap().make_map(src->cap);
    src = static_cast<MapObj*>(as_obj(m));
    for (uint32_t i = 0; i < src->cap; ++i) {
        Value k = src->entries()[i * 2];
        if (k == NIL_SLOT) continue;
        Value found;
        // Rebuilt without the key rather than tombstoned: probe chains stay
        // short and nothing has to know about deleted slots.
        Value one = p.heap().make_map(8);
        map_insert(p, one, k, UNIT);
        bool same = map_lookup(p, one, args[1], &found);
        if (same) {
            src = static_cast<MapObj*>(as_obj(m));
            continue;
        }
        Value val = static_cast<MapObj*>(as_obj(m))->entries()[i * 2 + 1];
        map_insert(p, resolve(out), k, val);
        out = resolve(out);
        src = static_cast<MapObj*>(as_obj(m));
    }
    return NativeResult::ok(resolve(out));
}

NativeResult core_map_pairs(Process& p, Value, Value* args, uint32_t) {
    Value m = resolve(args[0]);
    if (!is_obj(m, ObjType::Map)) return type_fail(p, "map_pairs needs a map");
    uint32_t cap = static_cast<MapObj*>(as_obj(m))->cap;
    Value list = NIL;
    for (uint32_t i = cap; i-- > 0;) {
        auto* mo = static_cast<MapObj*>(as_obj(m));
        Value k = mo->entries()[i * 2];
        if (k == NIL_SLOT) continue;
        Value v = mo->entries()[i * 2 + 1];
        Value pair = p.heap().make_cons(k, p.heap().make_cons(v, NIL));
        list = p.heap().make_cons(pair, list);
    }
    return NativeResult::ok(list);
}

// --- ordering ---

/// A total order over the flat types, so sorting can be written in Dawn.
/// Values of different types order by type, which keeps it total without
/// pretending an integer and a string are comparable.
NativeResult core_compare(Process& p, Value, Value* args, uint32_t) {
    Value a = resolve(args[0]);
    Value b = resolve(args[1]);
    auto rank = [](Value v) -> int {
        if (is_fixnum(v) || is_obj(v, ObjType::Float)) return 0;
        if (is_char(v)) return 1;
        if (is_bool(v)) return 2;
        if (is_atom(v)) return 3;
        if (is_obj(v, ObjType::Str)) return 4;
        if (is_unit(v)) return 5;
        return 6;
    };
    int ra = rank(a), rb = rank(b);
    if (ra != rb) return NativeResult::ok(make_fixnum(ra < rb ? -1 : 1));

    int cmp = 0;
    if (ra == 0) {
        double x = is_fixnum(a) ? double(fixnum_value(a)) : static_cast<FloatObj*>(as_obj(a))->value;
        double y = is_fixnum(b) ? double(fixnum_value(b)) : static_cast<FloatObj*>(as_obj(b))->value;
        cmp = x < y ? -1 : (x > y ? 1 : 0);
    } else if (ra == 1 || ra == 2) {
        uint64_t x = imm_payload(a), y = imm_payload(b);
        cmp = x < y ? -1 : (x > y ? 1 : 0);
    } else if (ra == 3) {
        const std::string& x = p.runtime().atom_name(uint32_t(imm_payload(a)));
        const std::string& y = p.runtime().atom_name(uint32_t(imm_payload(b)));
        cmp = x < y ? -1 : (x > y ? 1 : 0);
    } else if (ra == 4) {
        auto* x = static_cast<StrObj*>(as_obj(a));
        auto* y = static_cast<StrObj*>(as_obj(b));
        uint32_t n = x->len < y->len ? x->len : y->len;
        cmp = std::memcmp(x->data(), y->data(), n);
        if (cmp == 0) cmp = x->len < y->len ? -1 : (x->len > y->len ? 1 : 0);
        cmp = cmp < 0 ? -1 : (cmp > 0 ? 1 : 0);
    }
    return NativeResult::ok(make_fixnum(cmp));
}

}  // namespace

ModuleDef make_core_module() {
    return ModuleDef{
        "std.core",
        {
            // lists
            {"head", 1, 0b1, list_head},
            {"tail", 1, 0b1, list_tail},
            {"cons", 2, 0b0, list_cons},
            {"is_empty", 1, 0b1, list_is_empty},
            // strings
            {"str_len", 1, 0b1, core_str_len},
            {"str_chars", 1, 0b1, core_str_chars},
            {"str_of_chars", 1, 0b1, core_str_of_chars},
            {"str_of_bytes", 1, 0b1, core_str_of_bytes},
            {"str_slice", 3, 0b111, core_str_slice},
            {"str_find", 3, 0b111, core_str_find},
            {"str_byte", 2, 0b11, core_str_byte},
            // chars
            {"char_code", 1, 0b1, core_char_code},
            {"char_of_code", 1, 0b1, core_char_of_code},
            // numbers
            {"to_float", 1, 0b1, core_to_float},
            {"to_int", 1, 0b1, core_to_int},
            {"parse_int", 1, 0b1, core_parse_int},
            {"parse_float", 1, 0b1, core_parse_float},
            // arrays
            {"array_new", 2, 0b01, core_array_new},
            {"array_get", 2, 0b11, core_array_get},
            {"array_set", 3, 0b011, core_array_set},
            {"array_of_list", 1, 0b0, core_array_of_list},
            {"array_to_list", 1, 0b1, core_array_to_list},
            // maps
            {"map_new", 1, 0b1, core_map_new},
            {"map_get", 3, 0b011, core_map_get},
            {"map_has", 2, 0b11, core_map_has},
            {"map_put", 3, 0b011, core_map_put},
            {"map_remove", 2, 0b11, core_map_remove},
            {"map_pairs", 1, 0b1, core_map_pairs},
            // ordering
            {"compare", 2, 0b11, core_compare},
        }};
}

}  // namespace dream
