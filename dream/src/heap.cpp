#include "heap.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <unordered_set>

namespace dream {

namespace {
constexpr size_t ALIGN = 8;
inline size_t align_up(size_t n) { return (n + ALIGN - 1) & ~(ALIGN - 1); }

/// A free chunk is an object-shaped slot whose header still answers the chunk
/// size (so the sweep can walk a block as a run of chunks) and whose first
/// payload word links to the next free chunk of the same size class.
inline Obj* free_next(Obj* o) {
    return reinterpret_cast<Obj*>(reinterpret_cast<Value*>(o + 1)[0]);
}
inline void set_free_next(Obj* o, Obj* next) {
    reinterpret_cast<Value*>(o + 1)[0] = reinterpret_cast<Value>(next);
}

/// True for the object kinds whose payload holds no references -- Float, Str,
/// Pid and BigStr carry only raw bytes, a host id, or a pointer into the
/// image, which is not heap memory and is never collected. Marking one is a
/// no-op, so the collector need not put it on the worklist; the mark bit alone
/// keeps it.
inline bool is_atom_object(ObjType t) {
    return t == ObjType::Float || t == ObjType::Str || t == ObjType::Pid ||
           t == ObjType::BigStr;
}

/// Allocation rounds every object up to one of these sizes, and every chunk in
/// the heap is exactly one of them -- which is what makes the free lists
/// uniform and O(1) to fill and drain. Sizes below 128 are every multiple of 8
/// (objects all the way up to a ~8-slot frame are some $24+8k$); above that the
/// classes spread, first by 16, then geometrically, so a large object wastes at
/// most ~7% of its chunk.
constexpr uint32_t kClassSizes[] = {
    16,  24,  32,  40,  48,  56,  64,  72,  80,  88,  96,  104, 112, 120, 128,
    144, 160, 176, 192, 208, 224, 240, 256,
    288, 320, 352, 384, 416, 448, 480, 512,
    576, 640, 704, 768, 832, 896, 960, 1024,
    1152, 1280, 1408, 1536, 1664, 1792, 1920, 2048,
    2304, 2560, 2816, 3072, 3328, 3584, 3840, 4096,
};
static_assert(std::size(kClassSizes) == kHeapClassCount,
              "the free list array must match the size-class table");
constexpr uint32_t kMaxClassSize = kClassSizes[kHeapClassCount - 1];

/// The index of the smallest class that holds `bytes`. `bytes` must be a class
/// size itself for the exact lookup that sweep does; for allocation the caller
/// rounds up first.
size_t class_index(uint32_t bytes) {
    const uint32_t* it = std::lower_bound(kClassSizes, kClassSizes + kHeapClassCount, bytes);
    return static_cast<size_t>(it - kClassSizes);
}

/// The class size that holds `bytes`: the class, rounding up.
uint32_t class_size(uint32_t bytes) {
    return kClassSizes[class_index(bytes)];
}
}  // namespace

Heap::Heap(size_t initial_bytes)
    : nursery_hi_(initial_bytes),
      gc_threshold_(initial_bytes),
      initial_bytes_(initial_bytes) {
    // The nursery starts with one block; old space grows its own as objects
    // survive into it. The constructor's block is the nursery's, so a process
    // that dies young -- most programs -- never grows an old space at all.
    nursery_.push_back(new_block(initial_bytes));
    nursery_cursor_ = nursery_.back();
}

Heap::~Heap() {
    free_blocks(blocks_);
    for (Block* b : nursery_) free_block(b);
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
    b->big = false;
    return b;
}

void Heap::free_block(Block* b) {
    std::free(b->data);
    std::free(b);
}

void Heap::free_blocks(Block* b) {
    while (b) {
        Block* next = b->next;
        free_block(b);
        b = next;
    }
}

void Heap::free_nursery() {
    for (Block* b : nursery_) free_block(b);
    nursery_.clear();
    nursery_cursor_ = nullptr;
    nursery_bytes_ = 0;
}

Obj* Heap::carve(size_t sz) {
    // A same-sized chunk is already free: pop it. The free list holds only
    // chunks this size class knows, so the header is a valid run member; the
    // `GC_FREE` bit tells sweep that a dead chunk is already on a list and must
    // not be pushed a second time. Only the collector's promotes carve here,
    // so a popped chunk is always about to become a tenured object.
    size_t i = class_index(static_cast<uint32_t>(sz));
    if (Obj* o = free_lists_[i]) {
        free_lists_[i] = free_next(o);
        allocated_ += sz;
        return o;
    }

    // None free. Carve a fresh chunk off the tail of a block that has room --
    // the current carve block first, then any other shared old block (so the
    // strands of space left when a run ends and a new one begins are still
    // used), then a brand-new block. Dedicated big-object blocks are never a
    // target: their headroom is reserved, and a chunk cut into one would be
    // orphaned the day the big object dies and its whole block is handed back.
    // The carve is why collection can never run here: the block's tail belongs
    // to no object yet, but the caller may hold raw pointers that a collection
    // would have to treat as roots or lose.
    Block* b = carve_block_;
    if (b && (b->big || b->used + sz > b->size)) b = nullptr;
    for (Block* c = blocks_; b == nullptr && c; c = c->next) {
        if (!c->big && c->used + sz <= c->size) b = c;
    }
    if (!b) {
        b = new_block(sz);
        b->next = blocks_;
        blocks_ = b;
    }
    carve_block_ = b;
    auto* o = reinterpret_cast<Obj*>(b->data + b->used);
    b->used += sz;
    allocated_ += sz;
    return o;
}

Obj* Heap::carve_big(size_t sz) {
    // Anything past the top class gets a block to itself: a run of even a few
    // percent slack is one large object's worth of waste, and big objects are
    // rare enough that dedicating a block never fragments anything smaller.
    Block* b = new_block(sz);
    b->big = true;
    b->used = sz;
    b->next = blocks_;
    blocks_ = b;
    allocated_ += sz;
    return reinterpret_cast<Obj*>(b->data);
}

/// Bump-allocate from the nursery. Collection can never run mid-reduction, so
/// no allocation-time safepoint is needed: the high-water mark only decides
/// what the *next* safepoint check does. When the current block is full a
/// fresh one is added -- never a collection, and never a failure.
Obj* Heap::alloc_nursery(uint32_t sz) {
    Block* b = nursery_cursor_;
    if (b && (b->big || b->used + sz > b->size)) b = nullptr;
    if (!b) {
        b = new_block(sz);
        b->next = nullptr;
        nursery_.push_back(b);
        nursery_cursor_ = b;
    }
    auto* o = reinterpret_cast<Obj*>(b->data + b->used);
    b->used += sz;
    nursery_bytes_ += sz;
    allocated_ += sz;
    return o;
}

Obj* Heap::alloc(ObjType type, size_t extra) {
    size_t bytes = align_up(sizeof(Obj) + extra);
    uint32_t sz;
    Obj* o;
    if (bytes > kMaxClassSize) {
        // Large objects tenure immediately: they are rare, often long-lived,
        // and copying one once to save the next copy is a bad swap.
        sz = static_cast<uint32_t>(bytes);
        o = carve_big(sz);
    } else {
        sz = class_size(static_cast<uint32_t>(bytes));
        o = alloc_nursery(sz);
    }
    std::memset(o, 0, sz);
    o->type = type;
    o->gc = bytes > kMaxClassSize ? GC_OLD : GC_YOUNG;
    o->aux = 0;
    o->bytes = sz;
    total_allocated_ += sz;
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

Value Heap::make_bigstr(const char* data, uint64_t len) {
    // Sized from the struct rather than counted by hand: unlike a StrObj there
    // is no trailing payload here, so the whole object is its fields.
    auto* o = static_cast<BigStrObj*>(alloc(ObjType::BigStr, sizeof(BigStrObj) - sizeof(Obj)));
    o->len = len;
    o->hash = 0;
    o->data = data;
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

Value Heap::make_map(uint32_t) {
    return make_map_branch(0);
}

Value Heap::make_map_branch(uint32_t nslots) {
    auto* o = static_cast<MapObj*>(alloc(ObjType::Map, 8 + size_t(nslots) * sizeof(Value)));
    o->count = 0;
    o->bitmap = 0;
    return from_obj(o);
}

Value Heap::make_map_leaf(uint64_t hash, Value key, Value value, Value next) {
    auto* o = static_cast<MapLeafObj*>(alloc(ObjType::MapLeaf, 8 + 3 * sizeof(Value)));
    o->hash = hash;
    o->key = key;
    o->value = value;
    o->next = next;
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

void Heap::mark_object(Value v) {
    if (!is_ptr(v)) return;
    Obj* o = as_obj(v);
    if (o->gc & GC_MARK) return;

    // Collapse indirection chains while marking. A long-running loop that
    // repeatedly updates thunks would otherwise accumulate hops that cost
    // time on every read; a collection is the natural place to shorten them.
    // No one else observes the heap in the middle of a collection, so writing
    // through one Indirect from here cannot race a reader.
    if (o->type == ObjType::Indirect) {
        static_cast<IndirectObj*>(o)->target = resolve(static_cast<IndirectObj*>(o)->target);
    }

    // The mark rides alongside the generation bit: an old object stays old.
    o->gc |= GC_MARK;
    // An atom object (Float, Str, Pid) has no references to forward, so it is
    // grey only in name; it never needs the worklist.
    if (!is_atom_object(o->type)) scan_queue_.push_back(o);
}

void Heap::forward(Value* slot) {
    // While verifying, `forward` records roots instead of marking them.
    if (recording_) {
        recording_->push_back(*slot);
        return;
    }
    // A slot may hold an Indirect whose work is done. The copying collector
    // folded these by copying through them into the slot, so every reachable
    // slot came out of a collection holding the real value; readers rely on
    // that, not on resolving at the point of use. Nothing moves here, so the
    // way to keep the promise is to write the resolved value into the slot.
    Value v = *slot;
    if (is_ptr(v) && as_obj(v)->type == ObjType::Indirect) {
        v = resolve(v);
        *slot = v;
    }
    if (!is_ptr(v)) return;
    Obj* o = as_obj(v);
    // A young object is copied to old space and the slot rewritten to the
    // copy. A forwarded one is a young object already copied this cycle; its
    // first payload word holds the copy, so sharing survives promotion.
    if (o->gc & GC_YOUNG) {
        *slot = from_obj(promote(o));
        return;
    }
    if (o->gc & GC_FORWARDED) {
        *slot = forward_target(o);
        return;
    }
    // Old object: only a major collection wants it marked; a minor scans the
    // remembered set and promotes, and has no need to touch the rest.
    if (full_trace_) mark_object(v);
}

/// Copy a nursery object to old space. The from-space chunk becomes a
/// forwarding stub -- its generation bit flips to GC_FORWARDED and its first
/// payload word points at the copy -- so the second reference to it is
/// rewritten to the same copy and sharing survives. Promoted objects are
/// marked too during a major collection, so the sweep keeps them.
Obj* Heap::promote(Obj* o) {
    uint32_t sz = o->bytes;
    Obj* copy = carve(sz);
    std::memcpy(copy, o, sz);
    copy->gc = full_trace_ ? (GC_MARK | GC_OLD) : GC_OLD;
    // Every class size is at least 16 bytes, so every chunk has a first
    // payload word to park the forwarding pointer in. The from-space chunk is
    // dead once the nursery is emptied, so the overwrite costs nothing.
    *reinterpret_cast<Value*>(o + 1) = from_obj(copy);
    o->gc = GC_FORWARDED;
    promoted_bytes_ += sz;
    total_allocated_ += sz;
    if (!is_atom_object(o->type)) scan_queue_.push_back(copy);
    return copy;
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
            uint32_t n = map_bit_count(m->bitmap);
            for (uint32_t i = 0; i < n; ++i) forward(&m->slots()[i]);
            break;
        }
        case ObjType::MapLeaf: {
            auto* l = static_cast<MapLeafObj*>(o);
            forward(&l->key);
            forward(&l->value);
            forward(&l->next);
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
    major_collect(roots);
}

void Heap::major_collect(RootSource& roots) {
    // Every object that survives lands on the worklist at some point, so it
    // wants room for a big fraction of everything that was alive. The queue
    // retains its capacity across collects, but the live set grows, so a
    // fresh peak collection reallocates it once -- then again at the next
    // peak. Giving it a generous head start floors the size of the span it
    // holds, so each collection amortizes the growth instead of reallocating
    // a handful of times mid-collect. Nothing here is exact; it only has to
    // be in the right decade.
    if (scan_queue_.capacity() < allocated_ / size_t(16) + 1)
        scan_queue_.reserve(allocated_ / size_t(16) + 1);
    scan_queue_.clear();
    full_trace_ = true;

    // Mark. `forward` promotes every reachable young object -- copying it to
    // old space -- and marks the old ones. The roots are grey before anything
    // else; scanning turns them black and enqueues everything they point at,
    // until the worklist drains and the grey set is empty.
    roots.visit_roots(*this);
    while (!scan_queue_.empty()) {
        Obj* o = scan_queue_.back();
        scan_queue_.pop_back();
        scan_object(o);
    }
    full_trace_ = false;

    // Sweep. Every old block's chunk headers tile the block, so one walk over
    // each block sees every object: the marked ones stay put (and lose their
    // mark, for the next cycle), the rest go on their size class's free list.
    // Everything reachable in the nursery was promoted, so the whole nursery
    // is dead and is handed back.
    sweep();
    free_nursery();
    remembered_.clear();

    ++collections_;
    ++major_collections_;

    // Collect again once the heap has grown well past what survived, so that
    // programs with a large live set do not collect continuously. The `live * 2`
    // floor means a steady state with a modest live set (a program chewing
    // through transient lists) never pays to collect more than a couple of
    // times before the picture settles -- an allocate-heavy program that makes
    // little that survives wastes a copy of everything it ever builds if it
    // collects on every doubling.
    gc_threshold_ = live_after_gc_ * 3 + initial_bytes_;
    // The nursery scales with the live set too: a process that holds a lot
    // wants a bigger nursery before it pays for a minor collection.
    nursery_hi_ = live_after_gc_ / 4 + initial_bytes_;

    verify_collect(roots, false);
}

void Heap::minor_collect(RootSource& roots) {
    if (scan_queue_.capacity() < allocated_ / size_t(16) + 1)
        scan_queue_.reserve(allocated_ / size_t(16) + 1);
    scan_queue_.clear();

    // Trace. Roots have their young values promoted directly; each remembered
    // old object is scanned so a young value it was made to point at is
    // promoted too. Promotion copies reach every other young object through
    // the promoted one, so when the worklist drains, everything reachable is
    // in old space and the nursery is empty by construction.
    roots.visit_roots(*this);
    for (Obj* o : remembered_) scan_object(o);
    while (!scan_queue_.empty()) {
        Obj* o = scan_queue_.back();
        scan_queue_.pop_back();
        scan_object(o);
    }

    // Every nursery block is now dead space; reset them all so the next bump
    // allocation starts fresh. (Blocks with headroom are kept, not freed --
    // the process will fill them again immediately.)
    for (Block* b : nursery_) b->used = 0;
    allocated_ -= nursery_bytes_;
    nursery_bytes_ = 0;
    remembered_.clear();

    ++collections_;
    ++minor_collections_;
    if (allocated_ > peak_live_) peak_live_ = allocated_;

    // A minor never touches old space, so `live_after_gc_` still says what the
    // last full collection measured; `bytes_allocated` reports the whole live
    // picture. The check a minor adds is that nothing reachable is young.
    verify_collect(roots, true);
}

void Heap::sweep() {
    size_t live = 0;
    Block* prev = nullptr;
    for (Block* b = blocks_; b;) {
        Block* next = b->next;

        if (b->big) {
            // A dedicated block carries exactly one big object. It never joins
            // a size class (its size is past the table), so the choice is
            // keep it or hand the whole block back. Nothing else was ever
            // carved into it, so freeing it cannot strand a live chunk.
            Obj* o = reinterpret_cast<Obj*>(b->data);
            if (o->gc & GC_MARK) {
                o->gc &= ~GC_MARK;
                live += o->bytes;
                prev = b;
            } else {
                if (prev) prev->next = next;
                else blocks_ = next;
                if (carve_block_ == b) carve_block_ = nullptr;
                free_block(b);
            }
            b = next;
            continue;
        }

        // A shared block: its chunks tile it exactly, so one walk sees every
        // object and always ends precisely at `used`.
        size_t pos = 0;
        while (pos < b->used) {
            auto* o = reinterpret_cast<Obj*>(b->data + pos);
            uint32_t sz = o->bytes;
            pos += sz;
            bool marked = (o->gc & GC_MARK) != 0;
            if (marked) {
                // Tenured objects keep their generation bit; only the cycle's
                // mark is cleared.
                o->gc &= ~GC_MARK;
                live += sz;
            } else if (o->gc & GC_FREE) {
                // Alive on a list from an earlier cycle, untouched since. The
                // free bit is what keeps us from pushing it a second time; it
                // still points off its list head. Leave the header alone.
            } else {
                o->gc = GC_FREE;
                size_t i = class_index(sz);
                set_free_next(o, free_lists_[i]);
                free_lists_[i] = o;
            }
        }
        prev = b;
        b = next;
    }
    Block* cb = blocks_;
    while (cb && cb->big) cb = cb->next;
    carve_block_ = cb;
    live_after_gc_ = live;
    if (live > peak_live_) peak_live_ = live;
    allocated_ = live;
}

void Heap::verify_collect(RootSource& roots, bool expect_no_young) {
    if (!verify_after_gc()) return;
    std::string problem = verify_internal(roots, expect_no_young);
    if (!problem.empty()) {
        std::fprintf(stderr, "dream: heap corrupted after collection %llu: %s\n",
                     static_cast<unsigned long long>(collections_), problem.c_str());
        std::abort();
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
    for (Block* b : nursery_) {
        auto start = reinterpret_cast<uintptr_t>(b->data);
        if (addr >= start && addr + bytes <= start + b->used) return true;
    }
    return false;
}

bool Heap::verify_after_gc() {
    #if defined(DREAM_DEBUG) || defined(DREAM_VERIFY_HEAP)
    static const bool on = [] {
        const char* v = std::getenv("DREAM_VERIFY_HEAP");
        return v && *v && std::strcmp(v, "0") != 0;
    }();
    return on;
    #else
        return false;
    #endif
    
}

namespace {

struct VerifyWalk {
    const Heap& heap;
    bool no_young;
    std::string error;
    std::vector<Value> work;
    std::vector<Value> parents;


    bool problem(const std::string& what) {
        if (error.empty()) error = what;
        return false;
    }

    static std::string addr(const void* p) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%p", p);
        return buf;
    }

    void push(Value v, Value parent) {
        if (is_ptr(v)) {
            work.push_back(v);
            parents.push_back(parent);
        }
    }

    bool check_object(Value v, Value parent) {
        Obj* o = as_obj(v);
        if (no_young && (o->gc & GC_YOUNG)) {
            return problem("a minor collection left a young object reachable at " +
                           addr(o));
        }
        if (reinterpret_cast<uintptr_t>(o) % 8 != 0) {
            return problem("object at " + addr(o) + " is not 8-byte aligned");
        }
        if (!heap.owns(o, sizeof(Obj))) {
            std::string via = "root";
            if (is_ptr(parent)) {
                Obj* p = as_obj(parent);
                via = addr(p) + " type " + std::to_string(static_cast<int>(p->type));
            }
            return problem("pointer " + addr(o) + " does not land in this heap (reached from " +
                           via + ")");
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
        // `seen` needs O(1) membership: the walk meets every live object once,
        // and a vector + linear find would make verification itself
        // quadratic -- survivable when a collection happens a handful of times,
        // not when a generational collector verifies after every minor.
        std::unordered_set<const Obj*> seen;
        while (!work.empty() && error.empty()) {
            Value v = work.back();
            work.pop_back();
            Value parent = parents.empty() ? 0 : parents.back();
            parents.pop_back();
            if (!is_ptr(v)) continue;
            Obj* o = as_obj(v);
            if (!seen.insert(o).second) continue;
            if (!check_object(v, parent)) return;

            switch (o->type) {
                case ObjType::Cons: {
                    auto* c = static_cast<ConsObj*>(o);
                    push(c->head, v);
                    push(c->tail, v);
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
                    for (uint32_t i = 0; i < a->len; ++i) push(a->items()[i], v);
                    break;
                }
                case ObjType::MapLeaf: {
                    auto* l = static_cast<MapLeafObj*>(o);
                    if (l->bytes < sizeof(Obj) + 8 + 3 * sizeof(Value)) {
                        problem("map entry at " + addr(o) + " is too small");
                        return;
                    }
                    push(l->key, v);
                    push(l->value, v);
                    push(l->next, v);
                    break;
                }
                case ObjType::Map: {
                    auto* m = static_cast<MapObj*>(o);
                    uint32_t slots = map_bit_count(m->bitmap);
                    size_t need = sizeof(Obj) + 8 + size_t(slots) * sizeof(Value);
                    if (m->bytes < need) {
                        problem("map at " + addr(o) + " holds " +
                                std::to_string(slots) + " children but is too small");
                        return;
                    }
                    // A branch with children must hold at least as many
                    // entries as it has children, since every child holds one.
                    if (m->count < slots) {
                        problem("map at " + addr(o) + " counts fewer entries than it has children");
                        return;
                    }
                    for (uint32_t i = 0; i < slots; ++i) push(m->slots()[i], v);
                    break;
                }
                case ObjType::Closure: {
                    auto* c = static_cast<ClosureObj*>(o);
                    if (c->bytes < sizeof(Obj) + 8 + size_t(c->ncaps) * sizeof(Value)) {
                        problem("closure at " + addr(o) + " is too small for its captures");
                        return;
                    }
                    for (uint32_t i = 0; i < c->ncaps; ++i) push(c->caps()[i], v);
                    break;
                }
                case ObjType::Frame: {
                    auto* f = static_cast<FrameObj*>(o);
                    if (f->bytes < sizeof(Obj) + sizeof(Value) + 8 +
                                       size_t(f->nslots) * sizeof(Value)) {
                        problem("frame at " + addr(o) + " is too small for its slots");
                        return;
                    }
                    push(f->closure, v);
                    for (uint32_t i = 0; i < f->nslots; ++i) {
                        // An unbound slot is legitimate: a `let` later in the
                        // block has not run yet.
                        if (f->slots()[i] != NIL_SLOT) push(f->slots()[i], v);
                    }
                    break;
                }
                case ObjType::Pap: {
                    auto* p = static_cast<PapObj*>(o);
                    push(p->fn, v);
                    for (uint32_t i = 0; i < p->nargs; ++i) push(p->args()[i], v);
                    break;
                }
                case ObjType::Thunk:
                case ObjType::Blackhole:
                    push(static_cast<ThunkObj*>(o)->frame, v);
                    break;
                case ObjType::Indirect:
                    push(static_cast<IndirectObj*>(o)->target, v);
                    break;
                case ObjType::ErrorBox: {
                    auto* e = static_cast<ErrorObj*>(o);
                    push(e->kind, v);
                    push(e->payload, v);
                    break;
                }
                case ObjType::Module:
                    push(static_cast<ModuleObj*>(o)->name, v);
                    break;
                case ObjType::Native:
                    push(static_cast<NativeObj*>(o)->name, v);
                    break;
                case ObjType::Str: {
                    auto* s = static_cast<StrObj*>(o);
                    if (s->bytes < sizeof(Obj) + 8 + size_t(s->len) + 1) {
                        problem("string at " + addr(o) + " is shorter than its length claims");
                        return;
                    }
                    break;
                }
                case ObjType::BigStr: {
                    // Its bytes are not in the heap, so there is nothing here
                    // to measure against its length -- only that it points
                    // somewhere at all. A null view would be read as an empty
                    // string forever rather than failing where it was made.
                    if (static_cast<BigStrObj*>(o)->data == nullptr) {
                        problem("big string at " + addr(o) + " points nowhere");
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
    return verify_internal(roots, false);
}

std::string Heap::verify_internal(RootSource& roots, bool no_young) {
    // Gather the roots without moving anything. `forward` is the only way a
    // root source hands over its references, so it doubles as a recorder while
    // `recording_` is set.
    std::vector<Value> collected;
    recording_ = &collected;
    roots.visit_roots(*this);
    recording_ = nullptr;

    VerifyWalk walk{*this, no_young, {}, {}, {}};
    for (Value v : collected) walk.push(v, 0);
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
        case ObjType::BigStr: {
            // The view crosses, not the bytes. They live in the image, which
            // every heap of this runtime shares and none of them owns.
            auto* b = static_cast<BigStrObj*>(o);
            return dest.make_bigstr(b->data, b->len);
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
            // Copied branch for branch, so the shape -- and with it the
            // lookups -- comes out identical on the other side.
            auto* src = static_cast<MapObj*>(o);
            uint32_t slots = map_bit_count(src->bitmap);
            Value map = dest.make_map_branch(slots);
            seen.emplace_back(v, map);
            auto* out = static_cast<MapObj*>(as_obj(map));
            out->count = src->count;
            out->bitmap = src->bitmap;
            for (uint32_t i = 0; i < slots; ++i) {
                Value item = copy_value(dest, src->slots()[i], seen);
                static_cast<MapObj*>(as_obj(map))->slots()[i] = item;
            }
            return map;
        }
        case ObjType::MapLeaf: {
            auto* src = static_cast<MapLeafObj*>(o);
            Value leaf = dest.make_map_leaf(src->hash, NIL_SLOT, NIL_SLOT, NIL_SLOT);
            seen.emplace_back(v, leaf);
            Value key = copy_value(dest, src->key, seen);
            Value val = copy_value(dest, src->value, seen);
            Value next = copy_value(dest, src->next, seen);
            auto* out = static_cast<MapLeafObj*>(as_obj(leaf));
            out->key = key;
            out->value = val;
            out->next = next;
            return leaf;
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
