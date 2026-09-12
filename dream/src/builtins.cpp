#include "builtins.hpp"

#include "io.hpp"

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <chrono>
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
    // A big string hashes by the same walk over the same bytes, so it lands in
    // the bucket its contents earn and a plain string of those bytes finds it.
    // The answer is kept on the object: everything else here is a few machine
    // words, and this one can be a gigabyte.
    if (is_ptr(v) && as_obj(v)->type == ObjType::BigStr) {
        auto* b = static_cast<BigStrObj*>(as_obj(v));
        if (b->hash == 0) b->hash = bytes_hash(Bytes{b->data, b->len});
        return b->hash;
    }
    Bytes bytes;
    if (is_ptr(v) && as_obj(v)->type == ObjType::Str) {
        string_bytes(v, &bytes);
        return bytes_hash(bytes);
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
    // Before the type test, because the two string representations are one
    // kind of key: they hash alike, so they must compare alike.
    Bytes sa, sb;
    if (string_bytes(a, &sa) && string_bytes(b, &sb)) return bytes_equal(sa, sb);
    if (x->type != y->type) return false;
    if (x->type == ObjType::Float) {
        return static_cast<FloatObj*>(x)->value == static_cast<FloatObj*>(y)->value;
    }
    if (x->type == ObjType::Pid) {
        return static_cast<PidObj*>(x)->id == static_cast<PidObj*>(y)->id;
    }
    return false;
}

/// The number of entries a node holds, counting a whole subtree.
uint32_t map_node_count(Value v) {
    if (!is_ptr(v)) return 0;
    Obj* o = as_obj(v);
    if (o->type == ObjType::Map) return static_cast<MapObj*>(o)->count;
    if (o->type != ObjType::MapLeaf) return 0;
    uint32_t n = 0;
    for (Value cur = v; is_obj(cur, ObjType::MapLeaf); cur = static_cast<MapLeafObj*>(as_obj(cur))->next) {
        ++n;
    }
    return n;
}

/// A leaf chain with `key` set to `value`. Replacing an existing key rebuilds
/// the chain above it; adding a new one puts it at the front. Either way the
/// chain the caller had is left untouched, which is the whole contract.
Value map_leaf_assoc(Heap& h, Value leaf, uint64_t hash, Value key, Value value, bool* added) {
    // Is the key already here? Walk first so that the common case -- it is not
    // -- costs one pass and no allocation.
    bool present = false;
    for (Value cur = leaf; is_obj(cur, ObjType::MapLeaf); ) {
        auto* l = static_cast<MapLeafObj*>(as_obj(cur));
        if (key_equal(l->key, key)) { present = true; break; }
        cur = l->next;
    }
    if (!present) {
        *added = true;
        return h.make_map_leaf(hash, key, value, leaf);
    }
    // Rebuild only as far as the entry that changed; the rest of the chain is
    // shared.
    Value rebuilt = NIL_SLOT;
    std::vector<MapLeafObj*> before;
    Value cur = leaf;
    while (is_obj(cur, ObjType::MapLeaf)) {
        auto* l = static_cast<MapLeafObj*>(as_obj(cur));
        if (key_equal(l->key, key)) {
            rebuilt = h.make_map_leaf(hash, key, value, l->next);
            break;
        }
        before.push_back(l);
        cur = l->next;
    }
    for (size_t i = before.size(); i > 0; --i) {
        rebuilt = h.make_map_leaf(hash, before[i - 1]->key, before[i - 1]->value, rebuilt);
    }
    return rebuilt;
}

Value map_assoc(Heap& h, Value node, uint32_t shift, uint64_t hash, Value key, Value value,
                bool* added);

/// Two nodes with different hashes, put under a branch deep enough to tell them
/// apart. They may agree for several levels, so this nests until they do not.
Value map_split(Heap& h, uint32_t shift, uint64_t h1, Value n1, uint64_t h2, Value n2) {
    uint32_t i1 = map_index(h1, shift);
    uint32_t i2 = map_index(h2, shift);
    if (i1 == i2) {
        Value deeper = map_split(h, shift + MAP_BITS, h1, n1, h2, n2);
        Value branch = h.make_map_branch(1);
        auto* b = static_cast<MapObj*>(as_obj(branch));
        b->bitmap = 1u << i1;
        b->count = map_node_count(deeper);
        b->slots()[0] = deeper;
        return branch;
    }
    Value branch = h.make_map_branch(2);
    auto* b = static_cast<MapObj*>(as_obj(branch));
    b->bitmap = (1u << i1) | (1u << i2);
    b->count = map_node_count(n1) + map_node_count(n2);
    b->slots()[i1 < i2 ? 0 : 1] = n1;
    b->slots()[i1 < i2 ? 1 : 0] = n2;
    return branch;
}

/// `node` with `key` set to `value`, sharing everything the change does not
/// touch. `added` says whether the map grew, so counts stay right without
/// recounting a subtree.
Value map_assoc(Heap& h, Value node, uint32_t shift, uint64_t hash, Value key, Value value,
                bool* added) {
    if (is_obj(node, ObjType::MapLeaf)) {
        auto* l = static_cast<MapLeafObj*>(as_obj(node));
        if (l->hash == hash) return map_leaf_assoc(h, node, hash, key, value, added);
        // Different hashes: they belong under a branch, not in one chain.
        *added = true;
        Value fresh = h.make_map_leaf(hash, key, value, NIL_SLOT);
        return map_split(h, shift, l->hash, node, hash, fresh);
    }
    auto* m = static_cast<MapObj*>(as_obj(node));
    uint32_t bit = 1u << map_index(hash, shift);
    uint32_t at = map_slot_of(m->bitmap, bit);
    uint32_t slots = map_bit_count(m->bitmap);

    if (m->bitmap & bit) {
        Value child = m->slots()[at];
        Value grown = map_assoc(h, child, shift + MAP_BITS, hash, key, value, added);
        Value branch = h.make_map_branch(slots);
        auto* b = static_cast<MapObj*>(as_obj(branch));
        m = static_cast<MapObj*>(as_obj(node));
        b->bitmap = m->bitmap;
        b->count = m->count + (*added ? 1 : 0);
        for (uint32_t i = 0; i < slots; ++i) b->slots()[i] = m->slots()[i];
        b->slots()[at] = grown;
        return branch;
    }
    // A slot nothing is using yet: widen by one and keep the children packed.
    *added = true;
    Value fresh = h.make_map_leaf(hash, key, value, NIL_SLOT);
    Value branch = h.make_map_branch(slots + 1);
    auto* b = static_cast<MapObj*>(as_obj(branch));
    m = static_cast<MapObj*>(as_obj(node));
    b->bitmap = m->bitmap | bit;
    b->count = m->count + 1;
    for (uint32_t i = 0; i < at; ++i) b->slots()[i] = m->slots()[i];
    b->slots()[at] = fresh;
    for (uint32_t i = at; i < slots; ++i) b->slots()[i + 1] = m->slots()[i];
    return branch;
}

/// `node` without `key`. Answers NIL_SLOT when the node is left empty, so a
/// parent can drop the slot rather than keep an empty branch forever.
Value map_dissoc(Heap& h, Value node, uint32_t shift, uint64_t hash, Value key, bool* removed) {
    if (is_obj(node, ObjType::MapLeaf)) {
        auto* l = static_cast<MapLeafObj*>(as_obj(node));
        if (l->hash != hash) return node;
        std::vector<MapLeafObj*> keep;
        bool found = false;
        for (Value cur = node; is_obj(cur, ObjType::MapLeaf); ) {
            auto* e = static_cast<MapLeafObj*>(as_obj(cur));
            if (!found && key_equal(e->key, key)) { found = true; }
            else { keep.push_back(e); }
            cur = e->next;
        }
        if (!found) return node;
        *removed = true;
        Value rebuilt = NIL_SLOT;
        for (size_t i = keep.size(); i > 0; --i) {
            rebuilt = h.make_map_leaf(hash, keep[i - 1]->key, keep[i - 1]->value, rebuilt);
        }
        return rebuilt;
    }
    auto* m = static_cast<MapObj*>(as_obj(node));
    uint32_t bit = 1u << map_index(hash, shift);
    if (!(m->bitmap & bit)) return node;
    uint32_t at = map_slot_of(m->bitmap, bit);
    uint32_t slots = map_bit_count(m->bitmap);
    Value shrunk = map_dissoc(h, m->slots()[at], shift + MAP_BITS, hash, key, removed);
    if (!*removed) return node;

    if (shrunk == NIL_SLOT) {
        Value branch = h.make_map_branch(slots - 1);
        auto* b = static_cast<MapObj*>(as_obj(branch));
        m = static_cast<MapObj*>(as_obj(node));
        b->bitmap = m->bitmap & ~bit;
        b->count = m->count - 1;
        for (uint32_t i = 0; i < at; ++i) b->slots()[i] = m->slots()[i];
        for (uint32_t i = at + 1; i < slots; ++i) b->slots()[i - 1] = m->slots()[i];
        // An empty branch below the root is not worth keeping.
        return (b->bitmap == 0 && shift > 0) ? NIL_SLOT : branch;
    }
    Value branch = h.make_map_branch(slots);
    auto* b = static_cast<MapObj*>(as_obj(branch));
    m = static_cast<MapObj*>(as_obj(node));
    b->bitmap = m->bitmap;
    b->count = m->count - 1;
    for (uint32_t i = 0; i < slots; ++i) b->slots()[i] = m->slots()[i];
    b->slots()[at] = shrunk;
    return branch;
}

}  // namespace

Value map_insert(Process& p, Value map, Value key, Value value) {
    map = resolve(map);
    if (!is_obj(map, ObjType::Map)) return map;
    bool added = false;
    return map_assoc(p.heap(), map, 0, key_hash(key), resolve(key), value, &added);
}

Value map_erase(Process& p, Value map, Value key) {
    map = resolve(map);
    if (!is_obj(map, ObjType::Map)) return map;
    bool removed = false;
    Value out = map_dissoc(p.heap(), map, 0, key_hash(key), resolve(key), &removed);
    // The root stays a branch even when the last entry goes.
    return out == NIL_SLOT ? p.heap().make_map(0) : out;
}

/// Every entry, appended to `out`. The order is the trie's, which is to say the
/// hash order -- unspecified, as the language promises.
void map_collect(Value node, std::vector<std::pair<Value, Value>>& out) {
    if (!is_ptr(node)) return;
    Obj* o = as_obj(node);
    if (o->type == ObjType::MapLeaf) {
        for (Value cur = node; is_obj(cur, ObjType::MapLeaf); ) {
            auto* l = static_cast<MapLeafObj*>(as_obj(cur));
            out.emplace_back(l->key, l->value);
            cur = l->next;
        }
        return;
    }
    if (o->type != ObjType::Map) return;
    auto* m = static_cast<MapObj*>(o);
    uint32_t slots = map_bit_count(m->bitmap);
    for (uint32_t i = 0; i < slots; ++i) map_collect(m->slots()[i], out);
}

bool map_lookup(Process&, Value map, Value key, Value* out) {
    uint64_t hash = key_hash(key);
    Value node = resolve(map);
    uint32_t shift = 0;
    // Down five bits of hash at a time until the branch runs out or a leaf
    // answers. Thirteen levels is the most a 64-bit hash can ask for.
    while (is_ptr(node)) {
        Obj* o = as_obj(node);
        if (o->type == ObjType::MapLeaf) {
            for (Value cur = node; is_obj(cur, ObjType::MapLeaf); ) {
                auto* l = static_cast<MapLeafObj*>(as_obj(cur));
                if (l->hash == hash && key_equal(l->key, key)) {
                    if (out) *out = l->value;
                    return true;
                }
                cur = l->next;
            }
            return false;
        }
        if (o->type != ObjType::Map) return false;
        auto* m = static_cast<MapObj*>(o);
        uint32_t bit = 1u << map_index(hash, shift);
        if (!(m->bitmap & bit)) return false;
        node = m->slots()[map_slot_of(m->bitmap, bit)];
        shift += MAP_BITS;
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
    // Rendering an element forces it, which can collect, so the rest of the
    // list is carried on the value stack where the collector will update it.
    const size_t base = p.stack.size();
    p.stack.push_back(v);
    for (;;) {
        Value w;
        if (!force_whnf(p, p.stack[base], &w)) {
            if (!p.force_blocked) p.stack.resize(base);
            return false;
        }
        if (is_nil(w)) break;
        if (!is_obj(w, ObjType::Cons)) {
            // An improper tail: show it rather than pretend the list ended.
            out->append(" | ");
            if (!stringify_into(p, w, out, true, depth + 1)) {
                if (!p.force_blocked) p.stack.resize(base);
                return false;
            }
            break;
        }
        if (!first) out->append(", ");
        first = false;
        p.stack[base] = w;
        if (!stringify_into(p, static_cast<ConsObj*>(as_obj(w))->head, out, true, depth + 1)) {
            if (!p.force_blocked) p.stack.resize(base);
            return false;
        }
        p.stack[base] = static_cast<ConsObj*>(as_obj(p.stack[base]))->tail;
    }
    p.stack.resize(base);
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
        case ObjType::BigStr:
            // `to_string` refuses outright; here the value is being rendered
            // inside something larger, where raising would lose the rest of the
            // structure. A placeholder says which value it was and how big.
            out->append("<big string, " +
                        std::to_string(static_cast<BigStrObj*>(as_obj(w))->len) + " bytes>");
            return true;
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
            std::vector<std::pair<Value, Value>> entries;
            map_collect(p.stack.back(), entries);
            bool first = true;
            for (auto& [k, val] : entries) {
                if (!first) out->append(", ");
                first = false;
                if (!stringify_into(p, k, out, true, depth + 1)) { p.stack.pop_back(); return false; }
                out->append(" => ");
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

/// `error_new kind payload` -- an error as a value, without raising it.
///
/// An error is a kind and a payload, and until these three existed a program
/// could catch one and learn nothing from it: `catch e` bound a box with no way
/// in, so the only thing to do with a failure was print it. With them, a caught
/// error can be asked what went wrong and a program can raise a *typed* failure
/// of its own -- `raise! (core.error_new :not_found path)` -- rather than
/// raising a string and hoping the reader parses it.
///
/// Pure, all three: making and reading an error is not an effect. Only raising
/// one is, which is why `raise!` keeps its `!` and these do not.
NativeResult core_error_new(Process& p, Value, Value* args, uint32_t) {
    Value kind = resolve(args[0]);
    if (!is_atom(kind)) {
        return NativeResult::raise(raise_error(p, well_known(p.runtime()).type_error,
                                               "error_new needs an atom for the kind"));
    }
    return NativeResult::ok(p.heap().make_error(kind, args[1]));
}

/// The kind of an error, or `()` for anything else -- so a `match` on the kind
/// needs no type test first.
NativeResult core_error_kind(Process& p, Value, Value* args, uint32_t) {
    Value v = resolve(args[0]);
    if (!is_obj(v, ObjType::ErrorBox)) return NativeResult::ok(UNIT);
    return NativeResult::ok(static_cast<ErrorObj*>(as_obj(v))->kind);
}

/// The payload of an error, or `()`. Unforced: a payload built lazily by the
/// code that failed stays that way until someone looks.
NativeResult core_error_payload(Process& p, Value, Value* args, uint32_t) {
    Value v = resolve(args[0]);
    if (!is_obj(v, ObjType::ErrorBox)) return NativeResult::ok(UNIT);
    return NativeResult::ok(static_cast<ErrorObj*>(as_obj(v))->payload);
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
        // Deliberately not `:string`. A big string is one only in what its
        // bytes mean; handing it to a `:string` branch means handing that
        // branch a value it may not concatenate, render or decode, and the
        // point of a distinct name is that such a branch is never reached.
        case DREAM_TYPE_BIGSTR: name = "bigstr"; break;
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
    // Its whole point is that it does not fit in a string; rendering it would
    // build the copy the type exists to avoid, and would be capped at 4 GiB
    // while doing so.
    if (is_obj(resolve(args[0]), ObjType::BigStr)) {
        return NativeResult::raise(raise_error(
            p, well_known(p.runtime()).type_error,
            "to_string cannot render a big string: it is a view into the image, larger than a "
            "string may be. Take a `str.slice` of it, or hand the whole of it to `io.write!`."));
    }
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
    } else if (is_obj(v, ObjType::BigStr)) {
        // A fixnum is 63 bits, so a payload would have to be four exabytes
        // before this could not say how long it is.
        n = int64_t(static_cast<BigStrObj*>(as_obj(v))->len);
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
// nothing. A pattern forces only as much as deciding takes -- `[x, ..rest]`
// looks at the first cell and neither `x` nor `rest` -- so each hands back the
// piece it found without forcing it, and the machine forces the piece if and
// when something wants it (`NativeResult::enter`).
//
// They used to force it themselves, which is not the same thing. Forcing inside
// a native runs a nested machine loop on the C++ stack, and a piece whose own
// evaluation reads through another pattern nests again: a list rebuilt a few
// hundred thousand times from the rest of the one before was a segmentation
// fault the first time its end was read. Their arguments arrive forced by the
// same machine, through the strictness mask, rather than by a force of their own.

NativeResult bi_match_is_cons(Process&, Value, Value* args, uint32_t) {
    return NativeResult::ok(make_bool(is_obj(resolve(args[0]), ObjType::Cons)));
}

NativeResult bi_match_head(Process& p, Value, Value* args, uint32_t) {
    Value v = resolve(args[0]);
    if (!is_obj(v, ObjType::Cons)) {
        return NativeResult::raise(
            raise_error(p, well_known(p.runtime()).type_error, "match_head needs a list cell"));
    }
    return NativeResult::enter(static_cast<ConsObj*>(as_obj(v))->head);
}

NativeResult bi_match_tail(Process& p, Value, Value* args, uint32_t) {
    Value v = resolve(args[0]);
    if (!is_obj(v, ObjType::Cons)) {
        return NativeResult::raise(
            raise_error(p, well_known(p.runtime()).type_error, "match_tail needs a list cell"));
    }
    return NativeResult::enter(static_cast<ConsObj*>(as_obj(v))->tail);
}

NativeResult bi_match_at(Process& p, Value, Value* args, uint32_t) {
    Value v = resolve(args[0]);
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
    return NativeResult::enter(a->items()[k]);
}

/// `[value]` when the key is there, `[]` when it is not -- one call rather
/// than a `has` followed by a `get`, so the map is looked up once and there is
/// no sentinel that a real value could collide with. The value goes into the
/// cell as the map holds it, and `match_head` hands it on unforced.
NativeResult bi_match_key(Process& p, Value, Value* args, uint32_t) {
    Value m = resolve(args[0]);
    if (!is_obj(m, ObjType::Map)) {
        return NativeResult::raise(
            raise_error(p, well_known(p.runtime()).type_error, "match_key needs a map"));
    }
    Value found;
    if (!map_lookup(p, m, args[1], &found)) return NativeResult::ok(NIL);
    return NativeResult::ok(p.heap().make_cons(found, NIL));
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
    Value m = p.heap().make_map(0);
    for (auto& [key, value] : pairs) {
        m = map_insert(p, m, make_atom(p.runtime().intern_atom(key)), value);
    }
    return m;
}

Value atom_of(Process& p, const char* name) {
    return make_atom(p.runtime().intern_atom(name));
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

namespace {

// --- measuring -------------------------------------------------------------
//
// A language with no clock can be timed only from outside it, which is no use
// for finding which stage of a long compilation is the slow one. These are the
// smallest set that answers "how long, how much, and how much of it was
// garbage" -- the three questions worth asking before changing anything.

/// Nanoseconds from a monotonic clock. Only differences mean anything: it does
/// not count from any particular moment, and it never goes backwards, which is
/// what makes it the right one to measure with.
NativeResult vm_now_ns(Process& p, Value, Value*, uint32_t) {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    // A fixnum is 63 bits, which is 292 years of nanoseconds.
    return NativeResult::ok(make_integer(p, int64_t(ns)));
}

/// Milliseconds since the epoch, for a log line someone has to read.
NativeResult vm_wall_ms(Process& p, Value, Value*, uint32_t) {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return NativeResult::ok(make_integer(p, int64_t(ms)));
}

/// Every byte this process has ever allocated, collections included.
NativeResult vm_allocated(Process& p, Value, Value*, uint32_t) {
    return NativeResult::ok(make_integer(p, int64_t(p.heap().bytes_total())));
}

/// The largest the live set has been after a collection.
NativeResult vm_peak_bytes(Process& p, Value, Value*, uint32_t) {
    return NativeResult::ok(make_integer(p, int64_t(p.heap().bytes_peak())));
}

/// Everything worth knowing about this process at once, so that a measurement
/// takes one call and cannot get its readings from different moments.
NativeResult vm_stats(Process& p, Value, Value*, uint32_t) {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return NativeResult::ok(info_map(p, {
        {"now_ns", make_integer(p, int64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()))},
        {"reductions", make_integer(p, int64_t(p.total_reductions))},
        {"allocated", make_integer(p, int64_t(p.heap().bytes_total()))},
        {"heap_bytes", make_integer(p, int64_t(p.heap().bytes_allocated()))},
        {"live_bytes", make_integer(p, int64_t(p.heap().bytes_live()))},
        {"peak_bytes", make_integer(p, int64_t(p.heap().bytes_peak()))},
        {"collections", make_integer(p, int64_t(p.heap().collections()))},
        {"minor_collections", make_integer(p, int64_t(p.heap().minor_collections()))},
        {"major_collections", make_integer(p, int64_t(p.heap().major_collections()))},
        {"bytes_promoted", make_integer(p, int64_t(p.heap().bytes_promoted()))},
    }));
}

/// Defined further down, next to the error helpers it needs.
NativeResult vm_eval_image(Process& p, Value self, Value* args, uint32_t n);
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
                         {"eval_image!", 1, 0b1, vm_eval_image},
                         // measuring
                         {"now_ns!", 1, 0b1, vm_now_ns},
                         {"wall_ms!", 1, 0b1, vm_wall_ms},
                         {"allocated!", 1, 0b1, vm_allocated},
                         {"peak_bytes!", 1, 0b1, vm_peak_bytes},
                         {"stats!", 1, 0b1, vm_stats},
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

/// The one refusal a big string earns, worded the same everywhere it is made.
///
/// These are the operations that would have to *build* a string out of one --
/// concatenating, searching, decoding to characters. Each would copy the bytes
/// into the heap and each would be capped at the 4 GiB a `StrObj` can count,
/// which between them is the whole reason the payload is not a string. So the
/// error names what a big string can do instead, because a caller who reached
/// for `str.find` wants a way through, not a diagnosis.
NativeResult bigstr_refused(Process& p, const char* what) {
    return NativeResult::raise(raise_error(
        p, well_known(p.runtime()).type_error,
        std::string(what) +
            " cannot take a big string: it is a view into the image, larger than a string may "
            "be. `len`, `str.byte`, `str.slice`, `==` and `io.write!` all work on one without "
            "copying it."));
}

NativeResult type_fail(Process& p, const char* what) {
    return NativeResult::raise(raise_error(p, well_known(p.runtime()).type_error, what));
}

/// Bring a value across from another runtime's heap.
///
/// Only data comes across, which is not a restriction so much as the definition
/// of what a compile-time value is: the answer has to be something a compiler
/// can write into an image, and a closure or a process is not. Atoms are
/// re-interned by name, because an atom is an index into the runtime that made
/// it and the two runtimes have never met.
bool import_across(Process& dest, Runtime& src_rt, Value v, Value* out, int depth) {
    if (depth > 64) return false;
    v = resolve(v);
    if (is_fixnum(v) || v == UNIT || v == NIL || is_bool(v) || is_char(v)) {
        *out = v;
        return true;
    }
    if (is_atom(v)) {
        *out = make_atom(dest.runtime().intern_atom(src_rt.atom_name(uint32_t(imm_payload(v)))));
        return true;
    }
    if (!is_ptr(v)) return false;
    switch (as_obj(v)->type) {
        case ObjType::Float:
            *out = dest.heap().make_float(static_cast<FloatObj*>(as_obj(v))->value);
            return true;
        case ObjType::Str: {
            auto* s = static_cast<StrObj*>(as_obj(v));
            *out = dest.heap().make_string(s->data(), s->len);
            return true;
        }
        case ObjType::Cons: {
            // Built back to front so the copy is made without recursing down
            // the spine, which for a long list is what would run out of stack.
            std::vector<Value> items;
            for (Value cur = v; is_obj(cur, ObjType::Cons); cur = resolve(static_cast<ConsObj*>(as_obj(cur))->tail)) {
                items.push_back(static_cast<ConsObj*>(as_obj(cur))->head);
                if (items.size() > (1u << 24)) return false;
            }
            Value list = NIL;
            for (size_t i = items.size(); i-- > 0;) {
                Value item;
                if (!import_across(dest, src_rt, items[i], &item, depth + 1)) return false;
                list = dest.heap().make_cons(item, list);
            }
            *out = list;
            return true;
        }
        case ObjType::Array: {
            uint32_t n = static_cast<ArrayObj*>(as_obj(v))->len;
            Value arr = dest.heap().make_array(n);
            for (uint32_t i = 0; i < n; ++i) {
                Value item;
                if (!import_across(dest, src_rt, static_cast<ArrayObj*>(as_obj(v))->items()[i],
                                   &item, depth + 1)) {
                    return false;
                }
                static_cast<ArrayObj*>(as_obj(arr))->items()[i] = item;
            }
            *out = arr;
            return true;
        }
        case ObjType::Map: {
            std::vector<std::pair<Value, Value>> entries;
            map_collect(v, entries);
            Value m = dest.heap().make_map(0);
            for (auto& [k, val] : entries) {
                Value ck, cv;
                if (!import_across(dest, src_rt, k, &ck, depth + 1)) return false;
                if (!import_across(dest, src_rt, val, &cv, depth + 1)) return false;
                m = map_insert(dest, m, ck, cv);
            }
            *out = m;
            return true;
        }
        default:
            return false;
    }
}

/// Run an image and answer the value its entry point produced.
///
/// A compiler written in the language it compiles has a problem with
/// compile-time evaluation: working out what an expression comes to means
/// running it, and the only thing that knows how to run Dream is this. So it
/// runs on a fresh VM -- its own runtime, image, heap and scheduler -- which is
/// exactly what the expression would get at run time, and therefore cannot
/// disagree with it. Writing a second evaluator for compile time is how the two
/// come to disagree.
/// Raise with a named kind, the way `std.os` does for its own failures.
NativeResult comp_fail(Process& p, const char* kind, const std::string& message) {
    return NativeResult::raise(raise_error(p, p.runtime().intern_atom(kind), message));
}

NativeResult vm_eval_image(Process& p, Value, Value* args, uint32_t) {
    Value v = resolve(args[0]);
    if (!is_obj(v, ObjType::Str)) return type_fail(p, "eval_image! needs an image as a string");
    auto* s = static_cast<StrObj*>(as_obj(v));
    std::string bytes(s->data(), s->len);

    Runtime rt;
    std::string message;
    if (!rt.load_image_bytes(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(),
                             message)) {
        return comp_fail(p, "bad_image", message);
    }
    uint32_t func = rt.image().entry();
    if (func == NO_NODE) return comp_fail(p, "bad_image", "the image has no entry point");

    // One worker: this runs inside a worker of the outer scheduler, and the
    // work is one expression rather than a program worth parallelising.
    Scheduler sched(rt, 1);
    auto root = sched.create_process();
    Value cl = root->heap().make_closure(func, 0);
    prime_apply(*root, cl, 0);
    sched.start();
    sched.enqueue(root);
    bool clean = sched.wait_for_all();
    sched.stop();

    if (root->failed || !clean) {
        return comp_fail(p, "comp_failed", "the compile-time expression failed");
    }
    Value deep;
    if (!force_deep(*root, root->exit_value, &deep)) {
        return comp_fail(p, "comp_failed", "the compile-time expression raised");
    }
    Value imported;
    if (!import_across(p, rt, deep, &imported, 0)) {
        return comp_fail(p, "comp_failed",
                    "a compile-time expression must produce data, not a function or a process");
    }
    return NativeResult::ok(imported);
}

/// Decode one UTF-8 scalar starting at `i`, advancing it. Invalid bytes are
/// returned as U+FFFD and consume one byte, so decoding always terminates.
///
/// Every char this produces is a Unicode scalar value, which is what a `char`
/// is and what `char_of_code` insists on. The shape of the bytes is not enough
/// for that: `C0 AF` has the shape of a two-byte sequence and spells `/` in
/// more bytes than it needs, `ED A0 80` spells a surrogate, and `F4 90 80 80`
/// spells a codepoint past U+10FFFF. An overlong form is the dangerous one --
/// it lets a `/`, a quote or a NUL through anything that checked the bytes
/// before decoding them -- and all three are ruled out by the same means: the
/// lead byte narrows the range the *second* byte may take (RFC 3629, section 4).
uint32_t utf8_next(const char* data, uint32_t len, uint32_t* i) {
    uint8_t c = uint8_t(data[*i]);
    if (c < 0x80) {
        ++*i;
        return c;
    }
    uint32_t need;
    uint32_t cp;
    uint8_t lo = 0x80;
    uint8_t hi = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) {
        need = 1;
        cp = c & 0x1F;
    } else if (c >= 0xE0 && c <= 0xEF) {
        need = 2;
        cp = c & 0x0F;
        if (c == 0xE0) lo = 0xA0;  // below is overlong
        if (c == 0xED) hi = 0x9F;  // above is a surrogate
    } else if (c >= 0xF0 && c <= 0xF4) {
        need = 3;
        cp = c & 0x07;
        if (c == 0xF0) lo = 0x90;  // below is overlong
        if (c == 0xF4) hi = 0x8F;  // above is past U+10FFFF
    } else {
        // A continuation byte with no lead, or a lead (C0, C1, F5..FF) that
        // could only begin an overlong form or something past U+10FFFF.
        ++*i;
        return 0xFFFD;
    }
    // Counted as what is left rather than as `*i + need`, which cannot wrap.
    if (len - *i <= need) {
        ++*i;
        return 0xFFFD;
    }
    for (uint32_t k = 1; k <= need; ++k) {
        uint8_t cc = uint8_t(data[*i + k]);
        if (cc < (k == 1 ? lo : 0x80) || cc > (k == 1 ? hi : 0xBF)) {
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
    Bytes b;
    if (!string_bytes(args[0], &b)) return type_fail(p, "str_len needs a string");
    return NativeResult::ok(make_fixnum(int64_t(b.len)));
}

NativeResult core_str_chars(Process& p, Value, Value* args, uint32_t) {
    if (is_obj(resolve(args[0]), ObjType::BigStr)) return bigstr_refused(p, "str_chars");
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
    // The same care as `str_of_bytes`: the rest of the list lives on the value
    // stack, because forcing a character can collect and move the cell.
    const size_t base = p.stack.size();
    p.stack.push_back(args[0]);
    for (;;) {
        Value w;
        if (!force_whnf(p, p.stack[base], &w)) {
            if (!p.force_blocked) p.stack.resize(base);
            return NativeResult::raise(p.result);
        }
        if (is_nil(w)) break;
        if (!is_obj(w, ObjType::Cons)) {
            p.stack.resize(base);
            return type_fail(p, "str_of_chars needs a list of chars");
        }
        p.stack[base] = w;
        Value head;
        if (!force_whnf(p, static_cast<ConsObj*>(as_obj(w))->head, &head)) {
            if (!p.force_blocked) p.stack.resize(base);
            return NativeResult::raise(p.result);
        }
        if (!is_char(head)) {
            p.stack.resize(base);
            return type_fail(p, "str_of_chars needs a list of chars");
        }
        utf8_encode(uint32_t(imm_payload(head)), &out);
        p.stack[base] = static_cast<ConsObj*>(as_obj(p.stack[base]))->tail;
    }
    p.stack.resize(base);
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
    // Forcing a byte runs Dream code, which can collect, and a collection moves
    // every object it keeps. So the walk carries its position on the value
    // stack -- which the collector updates -- rather than in a C++ local, and
    // re-reads the cell after each force. Getting this wrong needs a list long
    // enough to collect part way along, which is why it stayed hidden until a
    // program compiled itself.
    const size_t base = p.stack.size();
    p.stack.push_back(args[0]);
    for (;;) {
        Value w;
        if (!force_whnf(p, p.stack[base], &w)) {
            // A suspended force keeps its working stack above ours; cutting it
            // back would destroy the work that is waiting to be resumed.
            if (!p.force_blocked) p.stack.resize(base);
            return NativeResult::raise(p.result);
        }
        if (is_nil(w)) break;
        if (!is_obj(w, ObjType::Cons)) {
            p.stack.resize(base);
            return type_fail(p, "str_of_bytes needs a list of integers");
        }
        // The cell itself becomes the position, so the tail stays reachable
        // while the head is forced.
        p.stack[base] = w;
        Value head;
        if (!force_whnf(p, static_cast<ConsObj*>(as_obj(w))->head, &head)) {
            if (!p.force_blocked) p.stack.resize(base);
            return NativeResult::raise(p.result);
        }
        if (!is_fixnum(head)) {
            p.stack.resize(base);
            return type_fail(p, "str_of_bytes needs a list of integers");
        }
        int64_t b = fixnum_value(head);
        if (b < 0 || b > 255) {
            p.stack.resize(base);
            return NativeResult::raise(raise_error(
                p, well_known(p.runtime()).type_error,
                "str_of_bytes needs bytes in 0..255, got " + std::to_string(b)));
        }
        out.push_back(char(uint8_t(b)));
        p.stack[base] = static_cast<ConsObj*>(as_obj(p.stack[base]))->tail;
    }
    p.stack.resize(base);
    return NativeResult::ok(p.heap().make_string(out.data(), uint32_t(out.size())));
}

/// `str_concat parts` -- one string from a list of them.
///
/// Concatenating n strings by `+` is n copies of everything written so far,
/// which is quadratic and is why anything that built a large output a piece at
/// a time -- an image, a rendered diagnostic, a dump -- cost more in copying
/// than in the work it was reporting on. This walks the list once and copies
/// each part once.
///
/// The same care with the collector as `str_of_bytes`: forcing a part runs
/// Dream code, which can collect and move every cell, so the position rides on
/// the value stack rather than in a C++ local.
NativeResult core_str_concat(Process& p, Value, Value* args, uint32_t) {
    std::string out;
    const size_t base = p.stack.size();
    p.stack.push_back(args[0]);
    for (;;) {
        Value w;
        if (!force_whnf(p, p.stack[base], &w)) {
            if (!p.force_blocked) p.stack.resize(base);
            return NativeResult::raise(p.result);
        }
        if (is_nil(w)) break;
        if (!is_obj(w, ObjType::Cons)) {
            p.stack.resize(base);
            return type_fail(p, "str_concat needs a list of strings");
        }
        p.stack[base] = w;
        Value head;
        if (!force_whnf(p, static_cast<ConsObj*>(as_obj(w))->head, &head)) {
            if (!p.force_blocked) p.stack.resize(base);
            return NativeResult::raise(p.result);
        }
        if (is_obj(head, ObjType::BigStr)) {
            p.stack.resize(base);
            return bigstr_refused(p, "str_concat");
        }
        if (!is_obj(head, ObjType::Str)) {
            p.stack.resize(base);
            return type_fail(p, "str_concat needs a list of strings");
        }
        auto* part = static_cast<StrObj*>(as_obj(head));
        out.append(part->data(), part->len);
        p.stack[base] = static_cast<ConsObj*>(as_obj(p.stack[base]))->tail;
    }
    p.stack.resize(base);
    return NativeResult::ok(p.heap().make_string(out.data(), uint32_t(out.size())));
}

NativeResult core_str_slice(Process& p, Value, Value* args, uint32_t) {
    Bytes b;
    Value from = resolve(args[1]);
    Value count = resolve(args[2]);
    if (!string_bytes(args[0], &b) || !is_fixnum(from) || !is_fixnum(count)) {
        return type_fail(p, "str_slice needs a string, a start and a length");
    }
    // Clamped rather than raising: slicing past the end is how every loop that
    // walks a string finishes, and an error there would be noise.
    int64_t start = fixnum_value(from);
    int64_t n = fixnum_value(count);
    if (start < 0) start = 0;
    if (uint64_t(start) > b.len) start = int64_t(b.len);
    if (n < 0) n = 0;
    if (uint64_t(start) + uint64_t(n) > b.len) n = int64_t(b.len - uint64_t(start));
    // A slice of a big string is another window on the same bytes, not a copy
    // of them -- which is what lets a program walk a payload it could not hold.
    if (is_obj(resolve(args[0]), ObjType::BigStr)) {
        return NativeResult::ok(p.heap().make_bigstr(b.data + start, uint64_t(n)));
    }
    return NativeResult::ok(p.heap().make_string(b.data + start, uint32_t(n)));
}

NativeResult core_str_find(Process& p, Value, Value* args, uint32_t) {
    if (is_obj(resolve(args[0]), ObjType::BigStr) || is_obj(resolve(args[1]), ObjType::BigStr)) {
        return bigstr_refused(p, "str_find");
    }
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
    Bytes b;
    Value i = resolve(args[1]);
    if (!string_bytes(args[0], &b) || !is_fixnum(i)) {
        return type_fail(p, "str_byte needs a string and an index");
    }
    int64_t k = fixnum_value(i);
    if (k < 0 || uint64_t(k) >= b.len) return NativeResult::ok(make_fixnum(-1));
    return NativeResult::ok(make_fixnum(uint8_t(b.data[k])));
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
    // A surrogate is half of a UTF-16 pair and not a character. Encoded as
    // UTF-8 it is bytes that `str_chars` will not decode back, so letting one
    // through here would break the round trip between the two.
    if (c < 0 || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
        return type_fail(p, "that is not a Unicode scalar value");
    }
    return NativeResult::ok(make_char(uint32_t(c)));
}

// --- numbers ---

NativeResult core_to_float(Process& p, Value, Value* args, uint32_t) {
    Value v = resolve(args[0]);
    if (is_fixnum(v)) return NativeResult::ok(p.heap().make_float(double(fixnum_value(v))));
    if (is_obj(v, ObjType::Float)) return NativeResult::ok(v);
    return type_fail(p, "to_float needs a number");
}

/// A float's eight bytes, little-endian.
///
/// A bit pattern will not fit in an integer here -- a fixnum is 63 bits and a
/// double is 64 -- and rebuilding one out of halves invites exactly the sign
/// and rounding mistakes that would make a written image differ from the value
/// that went into it. Bytes are what a writer wants anyway.
NativeResult core_float_bytes(Process& p, Value, Value* args, uint32_t) {
    Value v = resolve(args[0]);
    double d;
    if (is_fixnum(v)) d = double(fixnum_value(v));
    else if (is_obj(v, ObjType::Float)) d = static_cast<FloatObj*>(as_obj(v))->value;
    else return type_fail(p, "float_bytes needs a number");
    unsigned char b[8];
    std::memcpy(b, &d, 8);
    return NativeResult::ok(p.heap().make_string(reinterpret_cast<const char*>(b), 8));
}

/// The inverse: the float those eight bytes spell.
NativeResult core_float_of_bytes(Process& p, Value, Value* args, uint32_t) {
    Value v = resolve(args[0]);
    if (!is_obj(v, ObjType::Str)) return type_fail(p, "float_of_bytes needs a string");
    auto* s = static_cast<StrObj*>(as_obj(v));
    if (s->len < 8) return type_fail(p, "float_of_bytes needs eight bytes");
    double d;
    std::memcpy(&d, s->data(), 8);
    return NativeResult::ok(p.heap().make_float(d));
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
    // Shares everything the new entry does not sit on: about log32(n) nodes are
    // rebuilt and the rest of the map is the one that came in.
    return NativeResult::ok(map_insert(p, m, args[1], args[2]));
}

NativeResult core_map_remove(Process& p, Value, Value* args, uint32_t) {
    Value m = resolve(args[0]);
    if (!is_obj(m, ObjType::Map)) return type_fail(p, "map_remove needs a map");
    return NativeResult::ok(map_erase(p, m, args[1]));
}

NativeResult core_map_pairs(Process& p, Value, Value* args, uint32_t) {
    Value m = resolve(args[0]);
    if (!is_obj(m, ObjType::Map)) return type_fail(p, "map_pairs needs a map");
    std::vector<std::pair<Value, Value>> entries;
    map_collect(m, entries);
    Value list = NIL;
    for (size_t i = entries.size(); i-- > 0;) {
        Value pair = p.heap().make_cons(entries[i].first,
                                        p.heap().make_cons(entries[i].second, NIL));
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
        if (is_stringish(v)) return 4;
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
        Bytes x, y;
        string_bytes(a, &x);
        string_bytes(b, &y);
        cmp = bytes_compare(x, y);
    }
    return NativeResult::ok(make_fixnum(cmp));
}

// --- large data ---
//
// The payload region is the one part of an image the container addresses in 64
// bits (docs/bytecode-format.md, `LDAT`/`PAYL`). It holds whatever the program
// is *about* rather than what the program is -- a corpus, a model, an asset --
// and it is reached from here and nowhere else: no opcode names it, so an image
// that never asks for its payload is an image any reader can run.
//
// Both of these are pure. The payload is fixed when the image is written and
// nothing can alter it, so asking for datum `i` is a function of `i` in exactly
// the way `str_byte` is a function of its index.

NativeResult core_data_count(Process& p, Value, Value*, uint32_t) {
    return NativeResult::ok(make_fixnum(int64_t(p.runtime().image().data_count())));
}

NativeResult core_data_at(Process& p, Value, Value* args, uint32_t) {
    Value v = resolve(args[0]);
    if (!is_fixnum(v)) return type_fail(p, "data_at needs an index");
    int64_t i = fixnum_value(v);
    const Image& img = p.runtime().image();
    if (i < 0 || uint64_t(i) >= img.data_count()) {
        return NativeResult::raise(raise_error(
            p, well_known(p.runtime()).type_error,
            "data_at " + std::to_string(i) + ": this image carries " +
                std::to_string(img.data_count()) +
                " large data. They are declared at compile time with `dreams --payload`."));
    }
    // O(1) and no copy: the value is a length and a pointer into the mapping.
    uint32_t k = uint32_t(i);
    return NativeResult::ok(p.heap().make_bigstr(img.data_bytes(k), img.data_length(k)));
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
            {"str_concat", 1, 0b1, core_str_concat},
            // An error is a kind and a payload; these are the way in and out.
            // `error_new`'s payload is left lazy, so the mask forces only the
            // kind.
            {"error_new", 2, 0b01, core_error_new},
            {"error_kind", 1, 0b1, core_error_kind},
            {"error_payload", 1, 0b1, core_error_payload},
            {"str_slice", 3, 0b111, core_str_slice},
            {"str_find", 3, 0b111, core_str_find},
            {"str_byte", 2, 0b11, core_str_byte},
            // chars
            {"char_code", 1, 0b1, core_char_code},
            {"char_of_code", 1, 0b1, core_char_of_code},
            // numbers
            {"to_float", 1, 0b1, core_to_float},
            {"float_bytes", 1, 0b1, core_float_bytes},
            {"float_of_bytes", 1, 0b1, core_float_of_bytes},
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
            // large data
            {"data_count", 1, 0b1, core_data_count},
            {"data_at", 1, 0b1, core_data_at},
            // ordering
            {"compare", 2, 0b11, core_compare},
        }};
}

}  // namespace dream
