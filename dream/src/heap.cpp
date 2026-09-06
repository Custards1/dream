#include "heap.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace dream {

namespace {
constexpr size_t ALIGN = 8;
inline size_t align_up(size_t n) { return (n + ALIGN - 1) & ~(ALIGN - 1); }

/// Marker written into an object's header once it has been evacuated; the
/// first payload word then holds the to-space address.
constexpr uint8_t GC_FORWARDED = 1;

inline Value& forward_slot(Obj* o) { return *reinterpret_cast<Value*>(o + 1); }
}  // namespace

Heap::Heap(size_t initial_bytes)
    : gc_threshold_(initial_bytes), initial_bytes_(initial_bytes) {
    blocks_ = new_block(initial_bytes);
}

Heap::~Heap() {
    free_blocks(blocks_);
    free_blocks(to_blocks_);
}

Heap::Block* Heap::new_block(size_t bytes) {
    size_t size = bytes < initial_bytes_ ? initial_bytes_ : bytes;
    auto* b = static_cast<Block*>(std::malloc(sizeof(Block)));
    if (!b) throw std::bad_alloc();
    b->data = static_cast<uint8_t*>(std::malloc(size));
    if (!b->data) {
        std::free(b);
        throw std::bad_alloc();
    }
    b->next = nullptr;
    b->size = size;
    b->used = 0;
    return b;
}

void Heap::free_blocks(Block* b) {
    while (b) {
        Block* next = b->next;
        std::free(b->data);
        std::free(b);
        b = next;
    }
}

void* Heap::bump(size_t bytes) {
    Block*& head = collecting_ ? to_blocks_ : blocks_;
    if (head->used + bytes > head->size) {
        // The current block is full. Chain on a new one rather than collecting:
        // the caller may hold raw pointers that a collection would invalidate.
        Block* b = new_block(bytes);
        b->next = head;
        head = b;
    }
    void* p = head->data + head->used;
    head->used += bytes;
    if (!collecting_) allocated_ += bytes;
    return p;
}

Obj* Heap::alloc(ObjType type, size_t extra) {
    size_t bytes = align_up(sizeof(Obj) + extra);
    auto* o = static_cast<Obj*>(bump(bytes));
    std::memset(o, 0, bytes);
    o->type = type;
    o->gc = 0;
    o->aux = 0;
    o->bytes = static_cast<uint32_t>(bytes);
    return o;
}

// ---------------------------------------------------------------------------
// Constructors
// ---------------------------------------------------------------------------

Value Heap::make_float(double v) {
    auto* o = static_cast<FloatObj*>(alloc(ObjType::Float, sizeof(double)));
    o->value = v;
    return from_obj(o);
}

Value Heap::make_string(const char* data, uint32_t len) {
    auto* o = static_cast<StrObj*>(alloc(ObjType::Str, 8 + size_t(len) + 1));
    o->len = len;
    o->hash = 0;
    // A null `data` reserves room to be filled by the caller, which is how
    // concatenation builds its result in one allocation.
    if (len && data) std::memcpy(o->data(), data, len);
    o->data()[len] = '\0';
    return from_obj(o);
}

Value Heap::make_cons(Value head, Value tail) {
    auto* o = static_cast<ConsObj*>(alloc(ObjType::Cons, 2 * sizeof(Value)));
    o->head = head;
    o->tail = tail;
    return from_obj(o);
}

Value Heap::make_array(uint32_t len) {
    auto* o = static_cast<ArrayObj*>(alloc(ObjType::Array, 8 + size_t(len) * sizeof(Value)));
    o->len = len;
    return from_obj(o);
}

Value Heap::make_map(uint32_t capacity) {
    auto* o = static_cast<MapObj*>(alloc(ObjType::Map, 8 + size_t(capacity) * 2 * sizeof(Value)));
    o->count = 0;
    o->cap = capacity;
    return from_obj(o);
}

Value Heap::make_closure(uint32_t func, uint32_t ncaps) {
    auto* o = static_cast<ClosureObj*>(alloc(ObjType::Closure, 8 + size_t(ncaps) * sizeof(Value)));
    o->func = func;
    o->ncaps = ncaps;
    return from_obj(o);
}

Value Heap::make_thunk(uint32_t node, Value frame) {
    auto* o = static_cast<ThunkObj*>(alloc(ObjType::Thunk, 8 + sizeof(Value)));
    o->node = node;
    o->frame = frame;
    return from_obj(o);
}

Value Heap::make_frame(Value closure, uint32_t nslots) {
    auto* o = static_cast<FrameObj*>(
        alloc(ObjType::Frame, sizeof(Value) + 8 + size_t(nslots) * sizeof(Value)));
    o->closure = closure;
    o->nslots = nslots;
    return from_obj(o);
}

Value Heap::make_pap(Value fn, uint32_t nargs) {
    auto* o = static_cast<PapObj*>(
        alloc(ObjType::Pap, sizeof(Value) + 8 + size_t(nargs) * sizeof(Value)));
    o->fn = fn;
    o->nargs = nargs;
    return from_obj(o);
}

Value Heap::make_error(Value kind, Value payload) {
    auto* o = static_cast<ErrorObj*>(alloc(ObjType::ErrorBox, 2 * sizeof(Value)));
    o->kind = kind;
    o->payload = payload;
    return from_obj(o);
}

Value Heap::make_pid(uint64_t id) {
    auto* o = static_cast<PidObj*>(alloc(ObjType::Pid, sizeof(uint64_t)));
    o->id = id;
    return from_obj(o);
}

Value Heap::make_module(uint32_t import_index, Value name) {
    auto* o = static_cast<ModuleObj*>(alloc(ObjType::Module, 8 + sizeof(Value)));
    o->import_index = import_index;
    o->name = name;
    return from_obj(o);
}

// ---------------------------------------------------------------------------
// Collection
// ---------------------------------------------------------------------------

Value Heap::evacuate(Value v) {
    if (!is_ptr(v)) return v;
    Obj* o = as_obj(v);

    if (o->gc == GC_FORWARDED) return forward_slot(o);

    // Collapse indirection chains while copying. A long-running loop that
    // repeatedly updates thunks would otherwise accumulate hops that cost
    // time on every read; collection is the natural place to shorten them.
    if (o->type == ObjType::Indirect) {
        Value target = evacuate(static_cast<IndirectObj*>(o)->target);
        o->gc = GC_FORWARDED;
        forward_slot(o) = target;
        return target;
    }

    size_t bytes = object_size(o);
    auto* copy = static_cast<Obj*>(bump(bytes));
    std::memcpy(copy, o, bytes);
    copy->gc = 0;

    o->gc = GC_FORWARDED;
    forward_slot(o) = from_obj(copy);

    scan_queue_.push_back(copy);
    return from_obj(copy);
}

void Heap::forward(Value* slot) {
    // While verifying, `forward` records roots instead of moving them.
    if (recording_) {
        recording_->push_back(*slot);
        return;
    }
    *slot = evacuate(*slot);
}

void Heap::scan_object(Obj* o) {
    switch (o->type) {
        case ObjType::Cons: {
            auto* c = static_cast<ConsObj*>(o);
            forward(&c->head);
            forward(&c->tail);
            break;
        }
        case ObjType::Array: {
            auto* a = static_cast<ArrayObj*>(o);
            for (uint32_t i = 0; i < a->len; ++i) forward(&a->items()[i]);
            break;
        }
        case ObjType::Map: {
            auto* m = static_cast<MapObj*>(o);
            for (uint32_t i = 0; i < m->cap * 2; ++i) forward(&m->entries()[i]);
            break;
        }
        case ObjType::Closure: {
            auto* c = static_cast<ClosureObj*>(o);
            for (uint32_t i = 0; i < c->ncaps; ++i) forward(&c->caps()[i]);
            break;
        }
        case ObjType::Thunk:
        case ObjType::Blackhole: {
            auto* t = static_cast<ThunkObj*>(o);
            forward(&t->frame);
            break;
        }
        case ObjType::Indirect: {
            auto* ind = static_cast<IndirectObj*>(o);
            forward(&ind->target);
            break;
        }
        case ObjType::Pap: {
            auto* p = static_cast<PapObj*>(o);
            forward(&p->fn);
            for (uint32_t i = 0; i < p->nargs; ++i) forward(&p->args()[i]);
            break;
        }
        case ObjType::Frame: {
            auto* f = static_cast<FrameObj*>(o);
            forward(&f->closure);
            for (uint32_t i = 0; i < f->nslots; ++i) forward(&f->slots()[i]);
            break;
        }
        case ObjType::ErrorBox: {
            auto* e = static_cast<ErrorObj*>(o);
            forward(&e->kind);
            forward(&e->payload);
            break;
        }
        case ObjType::Module: {
            forward(&static_cast<ModuleObj*>(o)->name);
            break;
        }
        case ObjType::Native: {
            forward(&static_cast<NativeObj*>(o)->name);
            break;
        }
        // Float, Str and Pid hold no references.
        default:
            break;
    }
}

void Heap::collect(RootSource& roots) {
    collecting_ = true;
    to_blocks_ = new_block(initial_bytes_);
    scan_queue_.clear();

    roots.visit_roots(*this);

    // Cheney's algorithm, with an explicit queue instead of a scan pointer:
    // our to-space is a chain of blocks rather than one contiguous region, so
    // there is no single pointer to walk.
    while (!scan_queue_.empty()) {
        Obj* o = scan_queue_.back();
        scan_queue_.pop_back();
        scan_object(o);
    }

    free_blocks(blocks_);
    blocks_ = to_blocks_;
    to_blocks_ = nullptr;
    collecting_ = false;

    size_t live = 0;
    for (Block* b = blocks_; b; b = b->next) live += b->used;
    live_after_gc_ = live;
    allocated_ = live;
    ++collections_;

    // Collect again once the heap has grown well past what survived, so that
    // programs with a large live set do not collect continuously.
    gc_threshold_ = live * 2 + initial_bytes_;

    if (verify_after_gc()) {
        std::string problem = verify(roots);
        if (!problem.empty()) {
            std::fprintf(stderr, "mindv2: heap corrupted after collection %llu: %s\n",
                         static_cast<unsigned long long>(collections_), problem.c_str());
            std::abort();
        }
    }
}

// ---------------------------------------------------------------------------
// Verification
// ---------------------------------------------------------------------------

bool Heap::owns(const void* p, size_t bytes) const {
    auto addr = reinterpret_cast<uintptr_t>(p);
    for (Block* b = blocks_; b; b = b->next) {
        auto start = reinterpret_cast<uintptr_t>(b->data);
        // Only the used part counts: an object cannot legitimately live in the
        // unallocated tail of a block.
        if (addr >= start && addr + bytes <= start + b->used) return true;
    }
    return false;
}

bool Heap::verify_after_gc() {
    static const bool on = [] {
        const char* v = std::getenv("DREAM_VERIFY_HEAP");
        return v && *v && std::strcmp(v, "0") != 0;
    }();
    return on;
}

namespace {

struct VerifyWalk {
    const Heap& heap;
    std::string error;
    std::vector<Value> work;
    std::vector<const Obj*> seen;

    bool problem(const std::string& what) {
        if (error.empty()) error = what;
        return false;
    }

    static std::string addr(const void* p) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%p", p);
        return buf;
    }

    void push(Value v) {
        if (is_ptr(v)) work.push_back(v);
    }

    bool check_object(Value v) {
        Obj* o = as_obj(v);
        if (reinterpret_cast<uintptr_t>(o) % 8 != 0) {
            return problem("object at " + addr(o) + " is not 8-byte aligned");
        }
        if (!heap.owns(o, sizeof(Obj))) {
            return problem("pointer " + addr(o) + " does not land in this heap");
        }
        if (o->bytes < sizeof(Obj) || o->bytes > (1u << 30)) {
            return problem("object at " + addr(o) + " claims an implausible size " +
                           std::to_string(o->bytes));
        }
        if (!heap.owns(o, o->bytes)) {
            return problem("object at " + addr(o) + " of " + std::to_string(o->bytes) +
                           " bytes runs past the end of its block");
        }
        if (static_cast<uint8_t>(o->type) >= static_cast<uint8_t>(ObjType::Count)) {
            return problem("object at " + addr(o) + " has unknown type " +
                           std::to_string(static_cast<int>(o->type)));
        }
        return true;
    }

    void run() {
        while (!work.empty() && error.empty()) {
            Value v = work.back();
            work.pop_back();
            if (!is_ptr(v)) continue;
            Obj* o = as_obj(v);
            if (std::find(seen.begin(), seen.end(), o) != seen.end()) continue;
            if (!check_object(v)) return;
            seen.push_back(o);

            switch (o->type) {
                case ObjType::Cons: {
                    auto* c = static_cast<ConsObj*>(o);
                    push(c->head);
                    push(c->tail);
                    break;
                }
                case ObjType::Array: {
                    auto* a = static_cast<ArrayObj*>(o);
                    size_t need = sizeof(Obj) + 8 + size_t(a->len) * sizeof(Value);
                    if (a->bytes < need) {
                        problem("array at " + addr(o) + " says it holds " +
                                std::to_string(a->len) + " items but is too small");
                        return;
                    }
                    for (uint32_t i = 0; i < a->len; ++i) push(a->items()[i]);
                    break;
                }
                case ObjType::Map: {
                    auto* m = static_cast<MapObj*>(o);
                    size_t need = sizeof(Obj) + 8 + size_t(m->cap) * 2 * sizeof(Value);
                    if (m->bytes < need) {
                        problem("map at " + addr(o) + " says it has capacity " +
                                std::to_string(m->cap) + " but is too small");
                        return;
                    }
                    if (m->count > m->cap) {
                        problem("map at " + addr(o) + " holds more entries than it has slots");
                        return;
                    }
                    for (uint32_t i = 0; i < m->cap * 2; ++i) push(m->entries()[i]);
                    break;
                }
                case ObjType::Closure: {
                    auto* c = static_cast<ClosureObj*>(o);
                    if (c->bytes < sizeof(Obj) + 8 + size_t(c->ncaps) * sizeof(Value)) {
                        problem("closure at " + addr(o) + " is too small for its captures");
                        return;
                    }
                    for (uint32_t i = 0; i < c->ncaps; ++i) push(c->caps()[i]);
                    break;
                }
                case ObjType::Frame: {
                    auto* f = static_cast<FrameObj*>(o);
                    if (f->bytes < sizeof(Obj) + sizeof(Value) + 8 +
                                       size_t(f->nslots) * sizeof(Value)) {
                        problem("frame at " + addr(o) + " is too small for its slots");
                        return;
                    }
                    push(f->closure);
                    for (uint32_t i = 0; i < f->nslots; ++i) {
                        // An unbound slot is legitimate: a `let` later in the
                        // block has not run yet.
                        if (f->slots()[i] != NIL_SLOT) push(f->slots()[i]);
                    }
                    break;
                }
                case ObjType::Pap: {
                    auto* p = static_cast<PapObj*>(o);
                    push(p->fn);
                    for (uint32_t i = 0; i < p->nargs; ++i) push(p->args()[i]);
                    break;
                }
                case ObjType::Thunk:
                case ObjType::Blackhole:
                    push(static_cast<ThunkObj*>(o)->frame);
                    break;
                case ObjType::Indirect:
                    push(static_cast<IndirectObj*>(o)->target);
                    break;
                case ObjType::ErrorBox: {
                    auto* e = static_cast<ErrorObj*>(o);
                    push(e->kind);
                    push(e->payload);
                    break;
                }
                case ObjType::Module:
                    push(static_cast<ModuleObj*>(o)->name);
                    break;
                case ObjType::Native:
                    push(static_cast<NativeObj*>(o)->name);
                    break;
                case ObjType::Str: {
                    auto* s = static_cast<StrObj*>(o);
                    if (s->bytes < sizeof(Obj) + 8 + size_t(s->len) + 1) {
                        problem("string at " + addr(o) + " is shorter than its length claims");
                        return;
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }
};

}  // namespace

std::string Heap::verify(RootSource& roots) {
    // Gather the roots without moving anything. `forward` is the only way a
    // root source hands over its references, so it doubles as a recorder while
    // `recording_` is set.
    std::vector<Value> collected;
    recording_ = &collected;
    roots.visit_roots(*this);
    recording_ = nullptr;

    VerifyWalk walk{*this, {}, {}, {}};
    for (Value v : collected) walk.push(v);
    walk.run();
    return walk.error;
}

// ---------------------------------------------------------------------------
// Cross-heap copying
// ---------------------------------------------------------------------------

namespace {

/// Deep-copy with cycle handling. Values crossing a heap boundary are copied
/// eagerly, which is why forcing must happen before a send: a thunk carries a
/// frame that points back into the sender's world.
Value copy_value(Heap& dest, Value v, std::vector<std::pair<Value, Value>>& seen) {
    if (!is_ptr(v)) return v;
    v = resolve(v);
    if (!is_ptr(v)) return v;

    for (auto& [from, to] : seen) {
        if (from == v) return to;
    }

    Obj* o = as_obj(v);
    switch (o->type) {
        case ObjType::Float:
            return dest.make_float(static_cast<FloatObj*>(o)->value);
        case ObjType::Str: {
            auto* s = static_cast<StrObj*>(o);
            return dest.make_string(s->data(), s->len);
        }
        case ObjType::Pid:
            return dest.make_pid(static_cast<PidObj*>(o)->id);
        case ObjType::Cons: {
            Value cell = dest.make_cons(UNIT, NIL);
            seen.emplace_back(v, cell);
            auto* src = static_cast<ConsObj*>(o);
            Value head = copy_value(dest, src->head, seen);
            Value tail = copy_value(dest, src->tail, seen);
            auto* c = static_cast<ConsObj*>(as_obj(cell));
            c->head = head;
            c->tail = tail;
            return cell;
        }
        case ObjType::Array: {
            auto* src = static_cast<ArrayObj*>(o);
            Value arr = dest.make_array(src->len);
            seen.emplace_back(v, arr);
            for (uint32_t i = 0; i < src->len; ++i) {
                Value item = copy_value(dest, src->items()[i], seen);
                static_cast<ArrayObj*>(as_obj(arr))->items()[i] = item;
            }
            return arr;
        }
        case ObjType::Map: {
            auto* src = static_cast<MapObj*>(o);
            Value map = dest.make_map(src->cap);
            seen.emplace_back(v, map);
            static_cast<MapObj*>(as_obj(map))->count = src->count;
            for (uint32_t i = 0; i < src->cap * 2; ++i) {
                Value item = copy_value(dest, src->entries()[i], seen);
                static_cast<MapObj*>(as_obj(map))->entries()[i] = item;
            }
            return map;
        }
        case ObjType::ErrorBox: {
            auto* src = static_cast<ErrorObj*>(o);
            Value kind = copy_value(dest, src->kind, seen);
            Value payload = copy_value(dest, src->payload, seen);
            return dest.make_error(kind, payload);
        }
        case ObjType::Module: {
            auto* src = static_cast<ModuleObj*>(o);
            Value name = copy_value(dest, src->name, seen);
            return dest.make_module(src->import_index, name);
        }
        case ObjType::Closure: {
            auto* src = static_cast<ClosureObj*>(o);
            Value cl = dest.make_closure(src->func, src->ncaps);
            seen.emplace_back(v, cl);
            for (uint32_t i = 0; i < src->ncaps; ++i) {
                Value cap = copy_value(dest, src->caps()[i], seen);
                static_cast<ClosureObj*>(as_obj(cl))->caps()[i] = cap;
            }
            return cl;
        }
        case ObjType::Pap: {
            auto* src = static_cast<PapObj*>(o);
            Value fn = copy_value(dest, src->fn, seen);
            Value pap = dest.make_pap(fn, src->nargs);
            seen.emplace_back(v, pap);
            for (uint32_t i = 0; i < src->nargs; ++i) {
                Value arg = copy_value(dest, src->args()[i], seen);
                static_cast<PapObj*>(as_obj(pap))->args()[i] = arg;
            }
            return pap;
        }
        case ObjType::Frame: {
            auto* src = static_cast<FrameObj*>(o);
            Value fr = dest.make_frame(UNIT, src->nslots);
            seen.emplace_back(v, fr);
            Value closure = copy_value(dest, src->closure, seen);
            static_cast<FrameObj*>(as_obj(fr))->closure = closure;
            for (uint32_t i = 0; i < src->nslots; ++i) {
                Value slot = src->slots()[i];
                Value copied = slot == NIL_SLOT ? NIL_SLOT : copy_value(dest, slot, seen);
                static_cast<FrameObj*>(as_obj(fr))->slots()[i] = copied;
            }
            return fr;
        }
        case ObjType::Thunk: {
            // Thunks travel with their frame, so a spawned computation carries
            // everything it needs and evaluates independently in its new home.
            auto* src = static_cast<ThunkObj*>(o);
            Value th = dest.make_thunk(src->node, UNIT);
            seen.emplace_back(v, th);
            Value frame = copy_value(dest, src->frame, seen);
            static_cast<ThunkObj*>(as_obj(th))->frame = frame;
            return th;
        }
        case ObjType::Blackhole:
            // A thunk under evaluation belongs to the sender's stack; the
            // receiver cannot finish it, so hand over an error instead.
            return dest.make_error(make_atom(0), UNIT);
        default:
            return UNIT;
    }
}

}  // namespace

Value Heap::copy_between(Heap& dest, Value v) {
    std::vector<std::pair<Value, Value>> seen;
    return copy_value(dest, v, seen);
}

}  // namespace dream
