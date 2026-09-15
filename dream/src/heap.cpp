#include "heap.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_set>

#include "env.hpp"
#include "gc_pool.hpp"

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

/// Every class size is a multiple of 8 and the largest is 4096, so the whole
/// table inverts into 512 bytes: `kClassOf[bytes / 8]` is the class index of
/// anything that size, rounded up. Built once, at load.
///
/// This replaces a binary search. Allocation is the single most frequent thing
/// the VM does -- three hundred million times in one fold -- and six dependent
/// branches to answer a question with 512 possible inputs is the wrong shape.
struct ClassTable {
    uint8_t of[(kMaxClassSize / 8) + 1];
    constexpr ClassTable() : of{} {
        size_t cls = 0;
        for (size_t words = 0; words <= kMaxClassSize / 8; ++words) {
            while (kClassSizes[cls] < words * 8) ++cls;
            of[words] = uint8_t(cls);
        }
    }
};
constexpr ClassTable kClassOf{};
static_assert(kHeapClassCount <= 256, "a class index has to fit in the table's byte");

/// A monotonic clock read, in nanoseconds. Two of these per collection is
/// nothing against the collection itself, and a pause nobody measured is a
/// pause nobody has diagnosed.
inline uint64_t now_nanos() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}

/// The index of the smallest class that holds `bytes`. `bytes` must be a
/// multiple of 8 no larger than the top class -- every caller has already
/// rounded up, and sweep only ever asks about a chunk it found in the heap.
inline size_t class_index(uint32_t bytes) {
    return kClassOf.of[bytes >> 3];
}

/// The class size that holds `bytes`: the class, rounding up.
inline uint32_t class_size(uint32_t bytes) {
    return kClassSizes[class_index(bytes)];
}
}  // namespace

Heap::Heap(size_t initial_bytes)
    : nursery_hi_(initial_bytes),
      nursery_max_(g_env.nursery_max),
      gc_threshold_(initial_bytes),
      initial_bytes_(initial_bytes) {
    // The nursery starts with one block; old space grows its own as objects
    // survive into it. The constructor's block is the nursery's, so a process
    // that dies young -- most programs -- never grows an old space at all.
    nursery_.push_back(new_block(initial_bytes));
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
    b->dead = false;
    block_bytes_ += size;
    if (block_bytes_ > peak_block_bytes_) {
        peak_block_bytes_ = block_bytes_;
        peak_allocated_ = allocated_;
    }
    return b;
}

void Heap::free_block(Block* b) {
    block_bytes_ -= b->size;
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
    nursery_at_ = 0;
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
/// what the *next* safepoint check does. When the current block is full the
/// next one is taken, and when there is no next one a fresh block is added --
/// never a collection, and never a failure.
Obj* Heap::alloc_nursery(uint32_t sz) {
    if (nursery_at_ < nursery_.size()) {
        Block* b = nursery_[nursery_at_];
        if (b->used + sz <= b->size) {
            auto* o = reinterpret_cast<Obj*>(b->data + b->used);
            b->used += sz;
            nursery_bytes_ += sz;
            allocated_ += sz;
            return o;
        }
    }
    return grow_nursery(sz);
}

Obj* Heap::grow_nursery(uint32_t sz) {
    // Step past the blocks with no room. A minor collection empties every
    // block and rewinds to the first, so the ones behind us have already been
    // refilled; a major frees them all, and then there are none at all.
    while (nursery_at_ < nursery_.size()) {
        Block* b = nursery_[nursery_at_];
        if (b->used + sz <= b->size) break;
        ++nursery_at_;
    }
    if (nursery_at_ == nursery_.size()) {
        Block* b = new_block(sz);
        b->next = nullptr;
        nursery_.push_back(b);
    }
    Block* b = nursery_[nursery_at_];
    auto* o = reinterpret_cast<Obj*>(b->data + b->used);
    b->used += sz;
    nursery_bytes_ += sz;
    allocated_ += sz;
    return o;
}

Obj* Heap::alloc_bare(ObjType type, size_t extra) {
    size_t bytes = align_up(sizeof(Obj) + extra);
    uint32_t sz;
    Obj* o;
    uint8_t gen;
    if (bytes > kMaxClassSize) {
        // Large objects tenure immediately: they are rare, often long-lived,
        // and copying one once to save the next copy is a bad swap.
        sz = static_cast<uint32_t>(bytes);
        o = carve_big(sz);
        gen = GC_OLD;
    } else {
        sz = class_size(static_cast<uint32_t>(bytes));
        o = alloc_nursery(sz);
        gen = GC_YOUNG;
    }
    // The header is written whole rather than cleared and then filled: these
    // four fields are the whole of it. `gc` is published last, because it is
    // what a concurrent mark's helper looks at first -- the other bytes must
    // all be there by the time the byte that says "reachable" is.
    //
    // A big object allocated during a concurrent mark is born *marked*: born
    // grey, it could be scanned by a helper before its caller has filled the
    // payload in. So it carries GC_MARK, and is logged for the finalize to
    // scan when the fills are done. The young objects it is filled with can
    // wait for that scan; a helper would have skipped them anyway, because
    // young objects are precisely what a concurrent mark leaves to the final.
    //
    // With no mark running the same object has the same problem for a simpler
    // reason: it is born *old*, and what its caller fills it with is young.
    // That is an old-to-young edge made by no store the barrier ever sees --
    // `make_array` hands back an array and the caller writes the elements
    // straight into it -- so the next minor collection would not scan the
    // array, and the elements it cannot see are elements it frees. A 3000-cell
    // array of unforced thunks loses every one of them, and what the program
    // gets back is whatever the nursery has since put there.
    //
    // So an object born old is remembered at birth. One entry covers the whole
    // object however many slots it has, it is paid only by allocations over
    // `kMaxClassSize` -- which tenure precisely because they are rare -- and it
    // is what makes filling one safe without a barrier at every fill site.
    // Nothing can collect between the allocation and the fill: a collection
    // runs only at a safepoint, and there is none inside a reduction.
    by_type_[size_t(type) & 31] += sz;
    o->type = type;
    o->aux = 0;
    o->bytes = sz;
    if (marking_ && bytes > kMaxClassSize) {
        std::atomic_ref<uint8_t>(o->gc).store(GC_OLD | GC_MARK, std::memory_order_release);
        mark_born_.push_back(o);
    } else {
        std::atomic_ref<uint8_t>(o->gc).store(gen, std::memory_order_relaxed);
        if (gen == GC_OLD) remembered_.push_back(o);
    }
    total_allocated_ += sz;
    return o;
}

Obj* Heap::alloc(ObjType type, size_t extra) {
    Obj* o = alloc_bare(type, extra);
    std::memset(o + 1, 0, size_t(o->bytes) - sizeof(Obj));
    return o;
}

// ---------------------------------------------------------------------------
// Constructors
// ---------------------------------------------------------------------------

Value Heap::make_float(double v) {
    auto* o = static_cast<FloatObj*>(alloc_bare(ObjType::Float, sizeof(double)));
    o->value = v;
    return from_obj(o);
}

Value Heap::make_string(const char* data, uint32_t len) {
    auto* o = static_cast<StrObj*>(alloc_bare(ObjType::Str, 8 + size_t(len) + 1));
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
    auto* o = static_cast<BigStrObj*>(alloc_bare(ObjType::BigStr, sizeof(BigStrObj) - sizeof(Obj)));
    o->len = len;
    o->hash = 0;
    o->data = data;
    return from_obj(o);
}

Value Heap::make_cons(Value head, Value tail) {
    auto* o = static_cast<ConsObj*>(alloc_bare(ObjType::Cons, 2 * sizeof(Value)));
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
    auto* o = static_cast<MapLeafObj*>(alloc_bare(ObjType::MapLeaf, 8 + 3 * sizeof(Value)));
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
    auto* o = static_cast<ThunkObj*>(alloc_bare(ObjType::Thunk, 8 + sizeof(Value)));
    o->node = node;
    o->pad = 0;
    o->frame = frame;
    return from_obj(o);
}

Value Heap::make_frame(Value closure, uint32_t nslots) {
    return make_frame_filling(closure, nslots, 0);
}

Value Heap::make_frame_filling(Value closure, uint32_t nslots, uint32_t filled) {
    auto* o = static_cast<FrameObj*>(
        alloc_bare(ObjType::Frame, sizeof(Value) + 8 + size_t(nslots) * sizeof(Value)));
    o->closure = closure;
    o->nslots = nslots;
    o->pad = 0;
    // The slots the caller is about to write need no clearing; the rest must
    // read as `NIL_SLOT`, and so must the padding to the size class, which the
    // collector walks as part of the object.
    Value* slots = o->slots();
    auto* from = reinterpret_cast<uint8_t*>(slots + filled);
    auto* end = reinterpret_cast<uint8_t*>(o) + o->bytes;
    if (end > from) std::memset(from, 0, size_t(end - from));
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
    auto* o = static_cast<ErrorObj*>(alloc_bare(ObjType::ErrorBox, 2 * sizeof(Value)));
    o->kind = kind;
    o->payload = payload;
    return from_obj(o);
}

Value Heap::make_pid(uint64_t id) {
    auto* o = static_cast<PidObj*>(alloc_bare(ObjType::Pid, sizeof(uint64_t)));
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

/// One collector thread's private state, for the length of one collection.
///
/// Everything a tracing thread touches often lives here rather than in the
/// heap: the grey set it is working through, the old-space chunks it promotes
/// into, and the counters it will add to the heap's when it is finished. That
/// is what lets a parallel round take a lock only when a thread runs out of
/// something -- a batch of free chunks, a block to carve, work to do -- rather
/// than once per object.
struct Heap::GcCtx {
    /// The grey set: marked or promoted, not yet scanned. A serial collection
    /// points this at the heap's own queue, which keeps its capacity from one
    /// collection to the next; a parallel one gives every thread its own and
    /// balances them through the round's shared stack.
    std::vector<Obj*>* q = nullptr;
    std::vector<Obj*> own;
    /// Objects still to scan before this thread next asks whether anyone else
    /// has run out of work. Checking on every object would put a shared
    /// counter on the hottest loop in the collector to answer a question whose
    /// answer changes a few thousand times a second.
    int check_in = 0;

    /// Old space to promote into, private to this thread: chunks split off the
    /// heap's free lists a batch at a time, and a block of its own to carve
    /// from when they run out. Both are handed back when the round ends.
    Block* carve_block = nullptr;
    Obj* free_head[kHeapClassCount] = {};
    Obj* free_tail[kHeapClassCount] = {};
    /// Size classes whose shared list was found empty. Nothing puts a chunk
    /// back on a list until the round is over, so "empty" stays true once it
    /// is true -- and without remembering it, every promotion of a shape the
    /// heap has none of spare would take the allocation lock to be told so
    /// again, which is a lock per object and the whole cost of the round.
    bool free_dry[kHeapClassCount] = {};

    /// Added to the heap's counters when the round ends. Kept apart until then
    /// so that no collector thread ever writes a heap-wide number.
    size_t allocated = 0;
    uint64_t promoted = 0;
    uint64_t total = 0;
    size_t live = 0;
    /// Objects this thread took off a queue and scanned. Counted only in a
    /// parallel round, where the question it answers -- did the work divide?
    /// -- is the only one worth asking about a collector thread.
    uint64_t scanned = 0;
};

/// What a parallel round shares. It lives on the collecting thread's stack for
/// the length of the round and nowhere else.
struct Heap::GcRound {
    /// Guards the shared stack below, and the counters that decide when the
    /// trace is over.
    std::mutex mutex;
    std::condition_variable cv;
    /// Guards the heap's free lists and its block list. A separate lock from
    /// the one above, and not for tidiness: a thread refills its chunk lists
    /// while promoting, which is exactly when the other threads are handing
    /// work around, and one lock for both would have promotion waiting on the
    /// load balancer thousands of times a collection.
    std::mutex alloc_mutex;

    /// Work no thread has claimed. A thread with a long queue gives half of it
    /// away and a thread with none takes a batch back, which is the whole of
    /// the load balancing: the shape of a heap is not known before it is
    /// traced, so the only honest way to divide the trace is to divide it as
    /// it is found.
    std::vector<Obj*> stack;
    /// `stack.size()`, readable without the lock.
    std::atomic<size_t> shared{0};
    /// Threads that have run out of work and not yet found any. A tracing
    /// thread reads this far more often than it changes, which is why it is an
    /// atomic beside the count under the lock rather than the count itself.
    ///
    /// This, and not the size of the local queue, is the question worth
    /// asking before giving work away: handing over a hundred objects costs a
    /// lock, a copy and possibly a wake-up, and is worth it exactly when
    /// somebody would otherwise be doing nothing.
    std::atomic<unsigned> idle{0};

    /// Threads that have reached the body, and threads that still have work.
    /// Both are guarded by `mutex`, and it matters that they are: a thread
    /// claiming work from the stack and a thread declaring itself out of work
    /// have to be ordered against each other, or the second could conclude
    /// that everyone is idle in the moment before the first says otherwise.
    ///
    /// The trace is over when every thread has arrived and every one of them
    /// has run out at once -- a grey object could only be on a thread's own
    /// queue or on the shared stack, and then neither holds one. Counting
    /// arrivals is what makes a thread that was slow to wake safe: until it
    /// has said it is here, the others may not conclude anything.
    unsigned size = 0;
    unsigned arrived = 0;
    unsigned active = 0;
    /// Threads asleep waiting for work. A thread giving work away wakes the
    /// others only when there is someone to wake: a `notify_all` nobody is
    /// listening for is a system call for nothing, and sharing happens often
    /// enough that the nothings add up to more than the collection.
    unsigned waiting = 0;
    /// Read while spinning, so it cannot be guarded by the lock alone.
    std::atomic<bool> done{false};

    std::vector<GcCtx> ctxs;

    /// The old blocks, snapshotted so that a parallel sweep can hand them out
    /// by index, and the list can be rebuilt afterwards by the one thread that
    /// owns it.
    std::vector<Block*> blocks;
    std::atomic<size_t> next_block{0};
};

namespace {

/// Read an object's collector byte. In a parallel round another thread may be
/// claiming the same object at this moment, so the read has to be spelled as
/// an atomic one -- on the machines this runs on it is the same instruction,
/// and saying so is what makes the claim protocol below mean anything.
template <bool Par>
inline uint8_t read_gc(Obj* o) {
    if constexpr (Par)
        return std::atomic_ref<uint8_t>(o->gc).load(std::memory_order_acquire);
    else
        return o->gc;
}

/// Read an object's type byte. In a parallel round another thread may be
/// claiming the object's `gc` at this moment -- but that is not the byte
/// being read here, and the mutator, which is stopped in a parallel round,
/// writes no headers then. What needs saying is the *concurrent* mark, where
/// the mutator is very much running and can flip a live thunk's type (to
/// Blackhole, to Indirect, back) while a helper scans: a plain read against
/// that atomic store is a data race, and reading a type mid-store is reading
/// a byte that is whole one way or the other anyway.
template <bool Par>
inline ObjType read_type(Obj* o) {
    if constexpr (Par)
        return static_cast<ObjType>(
            std::atomic_ref<uint8_t>(reinterpret_cast<uint8_t&>(o->type))
                .load(std::memory_order_acquire));
    else
        return o->type;
}

/// The same for a reference the collector is about to rewrite.
///
/// Two threads can reach one slot: an object logged twice in the remembered
/// set is scanned twice, and both scans write the same answer into it. That
/// much would be benign on every machine here. The ordering is not: the value
/// written is usually a copy the writing thread has *just* made, and a slot is
/// how the rest of the collection finds it. Without the release the reader can
/// see the pointer before it sees the bytes -- and then read a header that is
/// still whatever the chunk held before it was reused. The pair below is the
/// second way a promoted object is published; the forwarding stub in `promote`
/// is the first, and both need the same edge for the same reason.
///
/// On x86-64 these are the same two instructions a plain load and store
/// compile to. Saying which is not a cost; it is the difference between a
/// collector that works and one that works on this machine.
template <bool Par>
inline Value read_slot(Value* slot) {
    if constexpr (Par)
        return std::atomic_ref<Value>(*slot).load(std::memory_order_acquire);
    else
        return *slot;
}

template <bool Par>
inline void write_slot(Value* slot, Value v) {
    if constexpr (Par)
        std::atomic_ref<Value>(*slot).store(v, std::memory_order_release);
    else
        *slot = v;
}

/// `resolve`, for a collector thread.
///
/// The chain it walks is made of ordinary object fields, and in a parallel
/// round another thread may have written the last link a moment ago -- so
/// every hop is an acquiring read, which is what makes the object behind it
/// safe to look at at all. The mutator's `resolve` in value.hpp is the same
/// walk without that, which is right for the mutator: it is stopped.
template <bool Par>
inline Value gc_resolve(Value v) {
    if constexpr (!Par) {
        return resolve(v);
    } else {
        while (is_ptr(v) && as_obj(v)->type == ObjType::Indirect) {
            v = std::atomic_ref<Value>(static_cast<IndirectObj*>(as_obj(v))->target)
                    .load(std::memory_order_acquire);
        }
        return v;
    }
}

/// One spin of a wait that is expected to be over almost immediately.
inline void spin_once() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#endif
}

/// How long a local queue may get before half of it is offered to the others
/// whatever they are doing, how short it may be and still be worth splitting
/// for a thread that has nothing, and how much an idle thread takes back at a
/// time. All three are in objects.
///
/// The floor is low because the cost it guards against is not the copy but the
/// wake-up, and `idle` above already answers whether anyone needs waking. What
/// no floor can help with is a trace with no parallelism in it: a list is the
/// honest example -- scanning one cell yields exactly one more, so the queue
/// stays at length one however many threads are watching it, and the right
/// thing for the others to do is go back to sleep.
constexpr size_t kShareAbove = 4096;
constexpr size_t kShareMin = 32;
constexpr size_t kTakeBatch = 512;
/// Objects scanned between two looks at whether anyone else is idle.
constexpr int kCheckEvery = 64;
/// How long a thread with nothing to do waits before it sleeps, in spins.
///
/// The thread that is about to hand work over is running *now*, so the wait is
/// usually a fraction of a microsecond -- and a futex round trip is tens of
/// them. Sleeping immediately is what made the smaller collections run on one
/// thread while seven others were still being woken: by the time they were up
/// the trace was over. Sleeping eventually is what keeps a long serial tail
/// -- a list, whose trace has no parallelism in it at all -- from burning
/// seven cores to watch one.
constexpr unsigned kIdleSpins = 2000;
/// Chunks split off a shared free list in one go.
constexpr unsigned kFreeBatch = 256;

}  // namespace

/// Keep the actual destructor here: it joins a concurrent mark in flight --
/// the helpers may be picking their way through an object in this heap at
/// this very moment, so they must stand down before a single block below is
/// freed -- and `mark_round_`'s `unique_ptr` needs the round's shape complete,
/// which it only is once `GcRound` has been defined above.
Heap::~Heap() {
    if (marking_) {
        marking_ = false;
        GcPool::instance().join();
    }
    free_blocks(blocks_);
    for (Block* b : nursery_) free_block(b);
}

/// How much work makes the handshake worth it. Waking a pool of threads and
/// joining them again costs tens of microseconds, so a collection that would
/// have finished inside that is better done alone -- and most collections in
/// most processes are exactly that. A minor is measured by the nursery it is
/// about to empty, a major by the whole heap it is about to walk, and the
/// major's floor is four times the minor's because a major also sweeps.
///
/// `DREAM_GC_PAR_MIN` moves the floor. Zero puts every collection through the
/// parallel path however small it is, which is how the race detector gets to
/// see the parallel collector on a program small enough to run under it.
static size_t parallel_floor(bool minor) {
    return minor ? g_env.gc_par_min : g_env.gc_par_min * 4;
}

Value Heap::await_forward(Obj* o) {
    std::atomic_ref<uint8_t> gc(o->gc);
    unsigned spins = 0;
    while ((gc.load(std::memory_order_acquire) & GC_FORWARDED) == 0) {
        if (++spins < 64) {
            spin_once();
        } else {
            // The claim covers one memcpy of at most four kilobytes, so
            // getting this far means the claiming thread lost its core rather
            // than that the copy is slow. Stand aside for it.
            std::this_thread::yield();
            spins = 0;
        }
    }
    return forward_target(o);
}

template <bool Par>
void Heap::mark_object(GcCtx& c, Value v) {
    if (!is_ptr(v)) return;
    Obj* o = as_obj(v);
    if constexpr (Par) {
        // The mark is the claim: whoever sets the bit owns the scan, so an
        // object two threads reach at once is still scanned exactly once.
        uint8_t prev = std::atomic_ref<uint8_t>(o->gc).fetch_or(
            GC_MARK, std::memory_order_acq_rel);
        if (prev & GC_MARK) return;
    } else {
        if (o->gc & GC_MARK) return;
        o->gc |= GC_MARK;
    }

    // Collapse indirection chains while marking. A long-running loop that
    // repeatedly updates thunks would otherwise accumulate hops that cost
    // time on every read; a collection is the natural place to shorten them.
    // Only the thread that claimed the mark writes here, and the mutator is
    // stopped, so this is still the single writer it was.
    if (o->type == ObjType::Indirect) {
        auto* ind = static_cast<IndirectObj*>(o);
        write_slot<Par>(&ind->target, gc_resolve<Par>(read_slot<Par>(&ind->target)));
    }

    // An atom object (Float, Str, Pid) has no references to forward, so it is
    // grey only in name; it never needs the worklist.
    if (!is_atom_object(o->type)) c.q->push_back(o);
}

bool Heap::claim_mark(Obj* o) {
    // The concurrent mark's claim, and its rule in one line: old objects only.
    // Young ones move at the finalize, so a mark on one would name a corpse;
    // they are reached again there, from the live roots. The mark is the claim
    // exactly as in `mark_object`, except there is no `Par` shape to speak for:
    // the seed and the helpers may as well all use the atomic, since it is the
    // same instruction and only turns up during a mark.
    auto gc = std::atomic_ref<uint8_t>(o->gc);
    uint8_t g = gc.load(std::memory_order_acquire);
    if (g & GC_YOUNG) return false;
    if (g & GC_MARK) return false;
    uint8_t prev = gc.fetch_or(GC_MARK, std::memory_order_acq_rel);
    return (prev & GC_MARK) == 0 && !(prev & GC_YOUNG);
}

bool Heap::mark_ref(GcCtx& c, Value v) {
    if (!is_ptr(v)) return false;
    Obj* o = as_obj(v);
    if (!claim_mark(o)) return false;
    // The type is read atomically because the mutator is running: it may be
    // flipping a live thunk's type (to Blackhole, to Indirect, back) at this
    // very moment, and a helper reading the byte plainly against that store is
    // a data race. The byte is whole one way or the other either way, but the
    // says-so is what ThreadSanitizer needs.
    //
    // Atom objects have nothing to scan; the mark bit alone keeps them.
    if (!is_atom_object(read_type<true>(o))) c.q->push_back(o);
    return true;
}

/// Hand one store's object to the concurrent mark's helpers and note it for
/// the finalize. The caller has already established that a mark is running and
/// the object carries the mark, so the work is purely the bookkeeping that
/// turns "the helpers may have finished with this neighbourhood" into "it will
/// be walked again": `mark_log_` is the finalize's memory of it, and the
/// round's shared stack gets it at once, so a helper that is still awake takes
/// it now instead of the finalize taking it later.
void Heap::log_mutation(Obj* dst) {
    mark_log_.push_back(dst);
    GcRound* r = round_;
    if (!r) return;
    bool wake;
    {
        std::lock_guard<std::mutex> g(r->mutex);
        // The trace may have finished a moment ago, with every helper asleep;
        // work pushed now would sit there until the finalize's join, useless.
        // The finalize re-walks `mark_log_` either way, so it is safe to let
        // the object wait for it.
        if (r->done.load(std::memory_order_relaxed)) return;
        r->stack.push_back(dst);
        r->shared.store(r->stack.size(), std::memory_order_relaxed);
        wake = r->waiting > 0;
    }
    if (wake) r->cv.notify_all();
}

template <bool Par, bool Con>
void Heap::forward_in(GcCtx& c, Value* slot) {
    // A concurrent mark does not promote -- young objects are the finalize's
    // job -- so its "forwarding" is marking, and nothing more.
    if constexpr (Con) {
        mark_ref(c, read_slot<Par>(slot));
        return;
    }
    Value v = read_slot<Par>(slot);
    if (!is_ptr(v)) return;
    Obj* o = as_obj(v);
    uint8_t gc = read_gc<Par>(o);
    if (gc & (GC_YOUNG | GC_FORWARDED | GC_BUSY)) {
        forward_slow<Par>(c, slot);
        return;
    }
    // An Indirect is folded away by the slow path, which rewrites the slot
    // with what it resolves to.
    if (o->type == ObjType::Indirect) {
        forward_slow<Par>(c, slot);
        return;
    }
    if (full_trace_ && !(gc & GC_MARK)) mark_object<Par>(c, v);
}

template <bool Par>
void Heap::forward_slow(GcCtx& c, Value* slot) {
    // A slot may hold an Indirect whose work is done. The copying collector
    // folded these by copying through them into the slot, so every reachable
    // slot came out of a collection holding the real value; readers rely on
    // that, not on resolving at the point of use. Nothing moves here, so the
    // way to keep the promise is to write the resolved value into the slot.
    Value v = read_slot<Par>(slot);
    if (is_ptr(v) && as_obj(v)->type == ObjType::Indirect) {
        v = gc_resolve<Par>(v);
        write_slot<Par>(slot, v);
    }
    if (!is_ptr(v)) return;
    Obj* o = as_obj(v);
    uint8_t gc = read_gc<Par>(o);
    // A young object is copied to old space and the slot rewritten to the
    // copy. A forwarded one is a young object already copied this cycle; its
    // first payload word holds the copy, so sharing survives promotion.
    if (gc & GC_YOUNG) {
        if constexpr (Par) {
            // Claim it before copying it. Two copies of one object would be
            // two objects, and everything the language says about sharing --
            // that forcing a thunk is visible to everyone holding it -- would
            // stop being true across a collection.
            uint8_t expect = GC_YOUNG;
            if (!std::atomic_ref<uint8_t>(o->gc).compare_exchange_strong(
                    expect, uint8_t(GC_YOUNG | GC_BUSY),
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                write_slot<Par>(slot, await_forward(o));
                return;
            }
        }
        write_slot<Par>(slot, from_obj(promote<Par>(c, o)));
        return;
    }
    if (gc & GC_FORWARDED) {
        write_slot<Par>(slot, forward_target(o));
        return;
    }
    if constexpr (Par) {
        if (gc & GC_BUSY) {
            write_slot<Par>(slot, await_forward(o));
            return;
        }
    }
    // Old object: only a major collection wants it marked; a minor scans the
    // remembered set and promotes, and has no need to touch the rest.
    if (full_trace_) mark_object<Par>(c, v);
}

/// Copy a nursery object to old space. The from-space chunk becomes a
/// forwarding stub -- its generation bit flips to GC_FORWARDED and its first
/// payload word points at the copy -- so the second reference to it is
/// rewritten to the same copy and sharing survives. Promoted objects are
/// marked too during a major collection, so the sweep keeps them.
///
/// In a parallel round the caller has already claimed `o`, and the flip to
/// GC_FORWARDED is what publishes the copy: every write below it is ordered
/// before the release, so a thread that sees the bit sees a finished object.
template <bool Par>
Obj* Heap::promote(GcCtx& c, Obj* o) {
    uint32_t sz = o->bytes;
    Obj* copy = gc_carve<Par>(c, sz);
    // The header travels field by field rather than inside the payload copy,
    // because `o->gc` is the one byte of the object that is not this thread's
    // to read plainly: another thread may be attempting the claim on it at
    // this very moment, and a plain read against an atomic write is a race
    // whatever the bytes turn out to be. Everything else -- `aux`, the cached
    // hash in the first payload word, the bytes themselves -- travels exactly
    // as it was, which is what `AUX_DEEP_FORCED` and the string hash rely on.
    copy->type = o->type;
    copy->aux = o->aux;
    copy->bytes = sz;
    copy->gc = full_trace_ ? uint8_t(GC_MARK | GC_OLD) : uint8_t(GC_OLD);
    std::memcpy(copy + 1, o + 1, size_t(sz) - sizeof(Obj));
    // Every class size is at least 16 bytes, so every chunk has a first
    // payload word to park the forwarding pointer in. The from-space chunk is
    // dead once the nursery is emptied, so the overwrite costs nothing.
    *reinterpret_cast<Value*>(o + 1) = from_obj(copy);
    if constexpr (Par) {
        std::atomic_ref<uint8_t>(o->gc).store(GC_FORWARDED, std::memory_order_release);
    } else {
        o->gc = GC_FORWARDED;
    }
    c.promoted += sz;
    c.total += sz;
    if (!is_atom_object(o->type)) c.q->push_back(copy);
    return copy;
}

template <bool Par, bool Con>
void Heap::scan_object(GcCtx& c, Obj* o) {
    // Each reference is either forwarded (the copying collector: promote a
    // young one, mark an old one, fold an indirection) or, when `Con`, merely
    // marked -- the concurrent mark's whole scan. Everything else is shared,
    // which is why the switch below is the same list of fields either way.
    auto forward = [&](Value* slot) {
        if constexpr (Con)
            mark_ref(c, read_slot<Par>(slot));
        else
            forward_in<Par, Con>(c, slot);
    };
    switch (read_type<Par>(o)) {
        case ObjType::Cons: {
            auto* x = static_cast<ConsObj*>(o);
            forward(&x->head);
            forward(&x->tail);
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
            auto* x = static_cast<ClosureObj*>(o);
            for (uint32_t i = 0; i < x->ncaps; ++i) forward(&x->caps()[i]);
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

void Heap::forward(Value* slot) {
    // While verifying, `forward` records roots instead of marking them.
    if (recording_) {
        recording_->push_back(*slot);
        return;
    }
    if (parallel_)
        forward_in<true, false>(*root_ctx_, slot);
    else
        forward_in<false, false>(*root_ctx_, slot);
}

// ---------------------------------------------------------------------------
// Promoting into old space from several threads at once
// ---------------------------------------------------------------------------

bool Heap::refill_free(GcCtx& c, size_t cls) {
    if (c.free_dry[cls]) return false;
    std::lock_guard<std::mutex> g(round_->alloc_mutex);
    Obj* head = free_lists_[cls];
    if (!head) {
        c.free_dry[cls] = true;
        return false;
    }
    // Take a bounded batch rather than the whole chain: the whole chain would
    // leave every other thread carving fresh blocks while the space it was
    // holding went unused.
    Obj* tail = head;
    for (unsigned n = 1; n < kFreeBatch && free_next(tail); ++n) tail = free_next(tail);
    free_lists_[cls] = free_next(tail);
    set_free_next(tail, nullptr);
    c.free_head[cls] = head;
    c.free_tail[cls] = tail;
    return true;
}

Heap::Block* Heap::thread_block(uint32_t sz) {
    // A block per thread, not a cursor shared between them. The waste is the
    // tail of each thread's last block, which the next serial carve finds and
    // fills; the alternative is a lock on every object promoted.
    std::lock_guard<std::mutex> g(round_->alloc_mutex);
    Block* b = new_block(sz);
    b->next = blocks_;
    blocks_ = b;
    return b;
}

void Heap::merge_free_lists(Obj** head, Obj** tail) {
    for (size_t i = 0; i < kHeapClassCount; ++i) {
        if (!head[i]) continue;
        set_free_next(tail[i], free_lists_[i]);
        free_lists_[i] = head[i];
        head[i] = nullptr;
        tail[i] = nullptr;
    }
}

template <bool Par>
Obj* Heap::gc_carve(GcCtx& c, uint32_t sz) {
    if constexpr (!Par) {
        return carve(sz);
    } else {
        size_t i = class_index(sz);
        Obj* o = c.free_head[i];
        if (!o && refill_free(c, i)) o = c.free_head[i];
        if (o) {
            c.free_head[i] = free_next(o);
            if (!c.free_head[i]) c.free_tail[i] = nullptr;
            c.allocated += sz;
            return o;
        }
        Block* b = c.carve_block;
        if (!b || b->used + sz > b->size) {
            b = thread_block(sz);
            c.carve_block = b;
        }
        o = reinterpret_cast<Obj*>(b->data + b->used);
        b->used += sz;
        c.allocated += sz;
        return o;
    }
}

// ---------------------------------------------------------------------------
// Dividing the trace
// ---------------------------------------------------------------------------

void Heap::share_work(GcCtx& c) {
    std::vector<Obj*>& q = *c.q;
    size_t give = q.size() / 2;
    if (give == 0) return;
    bool wake;
    {
        std::lock_guard<std::mutex> g(round_->mutex);
        // The *bottom* half. Work found early is nearer the roots, so it
        // stands for a larger subgraph than the leaves on top, and a thread
        // that takes it has something to do for a while.
        round_->stack.insert(round_->stack.end(), q.begin(), q.begin() + ptrdiff_t(give));
        round_->shared.store(round_->stack.size(), std::memory_order_relaxed);
        wake = round_->waiting > 0;
    }
    q.erase(q.begin(), q.begin() + ptrdiff_t(give));
    if (wake) round_->cv.notify_all();
}

/// Move a batch off the shared stack into `c`. The caller holds the lock.
static void claim_batch(std::vector<Obj*>& stack, std::atomic<size_t>& shared,
                        std::vector<Obj*>& into, size_t batch) {
    size_t take = std::min(stack.size(), batch);
    into.insert(into.end(), stack.end() - ptrdiff_t(take), stack.end());
    stack.resize(stack.size() - take);
    shared.store(stack.size(), std::memory_order_relaxed);
}

bool Heap::take_work(GcCtx& c) {
    GcRound& r = *round_;
    // `idle` is this thread's half of `r.active`: it has said it has nothing
    // to do and has not taken it back. Every change of it happens under the
    // lock, because it is what the end of the trace is decided from.
    bool idle = false;
    unsigned spins = 0;
    for (;;) {
        if (r.shared.load(std::memory_order_acquire) != 0) {
            std::lock_guard<std::mutex> g(r.mutex);
            if (!r.stack.empty()) {
                if (idle) {
                    ++r.active;
                    r.idle.fetch_sub(1, std::memory_order_relaxed);
                    idle = false;
                }
                claim_batch(r.stack, r.shared, *c.q, kTakeBatch);
                return true;
            }
        }
        if (!idle) {
            // Out of work, and so is the shared stack. Say so; when the last
            // thread that has arrived says so too, there is nowhere a grey
            // object could still be and the trace is over.
            std::unique_lock<std::mutex> lk(r.mutex);
            idle = true;
            r.idle.fetch_add(1, std::memory_order_relaxed);
            if (--r.active == 0 && r.arrived == r.size) {
                r.done.store(true, std::memory_order_release);
                lk.unlock();
                r.cv.notify_all();
                return false;
            }
            continue;
        }
        if (r.done.load(std::memory_order_acquire)) return false;
        if (++spins < kIdleSpins) {
            spin_once();
            continue;
        }
        // Long enough. Sleep, and let whoever finds work wake us.
        std::unique_lock<std::mutex> lk(r.mutex);
        ++r.waiting;
        r.cv.wait(lk, [&] {
            return r.done.load(std::memory_order_relaxed) || !r.stack.empty();
        });
        --r.waiting;
        if (r.done.load(std::memory_order_relaxed)) return false;
        ++r.active;
        r.idle.fetch_sub(1, std::memory_order_relaxed);
        claim_batch(r.stack, r.shared, *c.q, kTakeBatch);
        return true;
    }
}

template <bool Par, bool Con>
void Heap::drain(GcCtx& c) {
    std::vector<Obj*>& q = *c.q;
    for (;;) {
        while (!q.empty()) {
            Obj* o = q.back();
            q.pop_back();
            scan_object<Par, Con>(c, o);
            if constexpr (Par) {
                ++c.scanned;
                if (--c.check_in <= 0) {
                    c.check_in = kCheckEvery;
                    // Give work away when somebody is waiting for it, or when
                    // the local queue has grown past what one thread can be
                    // expected to get through on its own.
                    if (q.size() >= kShareAbove ||
                        (q.size() >= kShareMin &&
                         round_->idle.load(std::memory_order_relaxed) != 0)) {
                        share_work(c);
                    }
                }
            }
        }
        if constexpr (!Par) {
            return;
        } else {
            if (!take_work(c)) return;
        }
    }
}

bool Heap::trace_in_parallel(RootSource& roots, bool minor) {
    size_t work = minor ? nursery_bytes_ : allocated_;
    if (work < parallel_floor(minor)) return false;
    GcPool& pool = GcPool::instance();
    if (pool.capacity() < 2) return false;

    GcRound round;
    round.ctxs.resize(pool.capacity());
    round_ = &round;
    parallel_ = true;

    bool ran = pool.run(pool.capacity(), [&](unsigned index, unsigned count) {
        GcCtx& c = round.ctxs[index];
        c.q = &c.own;
        c.check_in = kCheckEvery;
        {
            std::lock_guard<std::mutex> g(round.mutex);
            round.size = count;
            ++round.arrived;
            ++round.active;
        }
        // The roots are the collecting thread's own business: they are few,
        // and they are the only part of the heap that lives outside it.
        if (index == 0) {
            root_ctx_ = &c;
            roots.visit_roots(*this);
            root_ctx_ = nullptr;
        }
        if (minor) {
            // The remembered set divides by index. An object logged twice is
            // scanned twice, possibly by two threads at once -- which is why
            // a slot the collector rewrites is read and written atomically.
            size_t n = remembered_.size();
            size_t lo = n * index / count;
            size_t hi = n * (index + 1) / count;
            for (size_t k = lo; k < hi; ++k) scan_object<true, false>(c, remembered_[k]);
        }
        drain<true, false>(c);
    });

    parallel_ = false;
    round_ = nullptr;
    if (ran && g_env.gc_trace) {
        std::fprintf(stderr, ";   traced by");
        for (GcCtx& c : round.ctxs) {
            if (c.scanned) std::fprintf(stderr, " %llu", (unsigned long long)c.scanned);
        }
        std::fprintf(stderr, " objects per thread\n");
    }
    if (ran) {
        for (GcCtx& c : round.ctxs) {
            merge_free_lists(c.free_head, c.free_tail);
            allocated_ += c.allocated;
            promoted_bytes_ += c.promoted;
            total_allocated_ += c.total;
        }
        ++parallel_collections_;
    }
    return ran;
}

void Heap::trace_alone(RootSource& roots, bool minor) {
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

    GcCtx c;
    c.q = &scan_queue_;
    root_ctx_ = &c;
    roots.visit_roots(*this);
    root_ctx_ = nullptr;
    if (minor) {
        for (Obj* o : remembered_) scan_object<false, false>(c, o);
    }
    drain<false, false>(c);
    promoted_bytes_ += c.promoted;
    total_allocated_ += c.total;
}

void Heap::collect(RootSource& roots) {
    major_collect(roots);
}

/// A minor collection's cost is what it promoted, not what it looked at: the
/// nursery is emptied for nothing whatever is in it. So a collection that
/// copied out a large share of the nursery was simply too early -- most of
/// what it copied would have died had it waited -- and the answer is a bigger
/// nursery, not a cleverer collection. A quarter is the line: above it the
/// nursery doubles, below it nothing changes, and a process whose garbage
/// really is long-lived stops at the cap rather than growing for ever.
void Heap::grow_nursery_if_crowded(size_t promoted, size_t looked_at) {
    if (nursery_hi_ >= nursery_max_) return;
    if (promoted * 4 <= looked_at) return;
    nursery_hi_ *= 2;
    if (nursery_hi_ > nursery_max_) nursery_hi_ = nursery_max_;
}

void Heap::major_collect(RootSource& roots) {
    const uint64_t started = now_nanos();

    // Mark. `forward` promotes every reachable young object -- copying it to
    // old space -- and marks the old ones. The roots are grey before anything
    // else; scanning turns them black and enqueues everything they point at,
    // until the worklist drains and the grey set is empty.
    full_trace_ = true;
    if (!trace_in_parallel(roots, /*minor=*/false)) trace_alone(roots, /*minor=*/false);
    full_trace_ = false;
    const uint64_t marked = now_nanos();

    // Sweep. Every old block's chunk headers tile the block, so one walk over
    // each block sees every object: the marked ones stay put (and lose their
    // mark, for the next cycle), the rest go on their size class's free list.
    // Everything reachable in the nursery was promoted, so the whole nursery
    // is dead and is handed back.
    if (!sweep_in_parallel()) sweep();
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
    // wants a bigger nursery before it pays for a minor collection. The cap
    // is the same one the survival rate grows towards, so a process cannot
    // reach a nursery through this that it could not have earned.
    nursery_hi_ = live_after_gc_ / 4 + initial_bytes_;
    if (nursery_hi_ > nursery_max_) nursery_hi_ = nursery_max_;
    // A major has just emptied the nursery and refitted both thresholds, so
    // whatever a minor last measured is about a heap that no longer exists.
    // Give the generational bet its odds back and let the next minor judge.
    minor_pays_ = true;

    major_nanos_ += now_nanos() - started;
    if (g_env.gc_trace)
        std::fprintf(stderr,
                     "; major: %zu live, %zu held, %.2f ms mark, %.2f ms sweep\n",
                     live_after_gc_, block_bytes_, double(marked - started) / 1e6,
                     double(now_nanos() - marked) / 1e6);
    verify_collect(roots, false);
}

void Heap::minor_collect(RootSource& roots) {
    const uint64_t started = now_nanos();
    const uint64_t promoted_before = promoted_bytes_;
    const size_t looked_at = nursery_bytes_;
    const size_t remembered_n = remembered_.size();

    // Trace. Roots have their young values promoted directly; each remembered
    // old object is scanned so a young value it was made to point at is
    // promoted too. Promotion copies reach every other young object through
    // the promoted one, so when the worklist drains, everything reachable is
    // in old space and the nursery is empty by construction.
    if (!trace_in_parallel(roots, /*minor=*/true)) trace_alone(roots, /*minor=*/true);

    // Every nursery block is now dead space; reset them all and rewind to the
    // first, so the next bump allocation refills the blocks we already have.
    // (They are kept, not freed -- the process will fill them again at once.)
    for (Block* b : nursery_) b->used = 0;
    nursery_at_ = 0;
    allocated_ -= nursery_bytes_;
    nursery_bytes_ = 0;
    remembered_.clear();

    ++collections_;
    ++minor_collections_;
    if (allocated_ > peak_live_) peak_live_ = allocated_;
    // What this minor cost against what it recovered. A nursery that was
    // nearly all survivors is one a minor cannot help with -- it copies the
    // lot into old space and the lot dies there -- and `major_due` reads this
    // to stop choosing minors in that case. The same ratio, at a different
    // threshold, is what decides the nursery should grow.
    const size_t promoted_now = size_t(promoted_bytes_ - promoted_before);
    minor_pays_ = promoted_now * 4 <= looked_at * 3;
    grow_nursery_if_crowded(promoted_now, looked_at);
    minor_nanos_ += now_nanos() - started;
    if (g_env.gc_trace)
        std::fprintf(stderr,
                     "; minor: %zu nursery, %llu promoted, %zu remembered, %zu held, %.2f ms\n",
                     looked_at, (unsigned long long)(promoted_bytes_ - promoted_before),
                     remembered_n, block_bytes_, double(now_nanos() - started) / 1e6);

    // A minor never touches old space, so `live_after_gc_` still says what the
    // last full collection measured; `bytes_allocated` reports the whole live
    // picture. The check a minor adds is that nothing reachable is young.
    verify_collect(roots, true);
}

// ---------------------------------------------------------------------------
// A major whose marking overlaps the mutator
// ---------------------------------------------------------------------------
//
// Phase 2's step 3: the pool that a parallel collection borrows to shorten its
// pause also works *during* the pause on a major's marking. A heap that is due
// for a full collection snapshots its roots, hands the snapshot to the helper
// threads, and keeps running -- the helpers walk old objects; the mutator
// keeps allocating and storing; `should_collect` insists the only collection
// the mutator may do meanwhile is the finalize of the one in flight. What the
// helpers grey is a picture of the old graph as it was at the safepoint; the
// finalize re-walks exactly what changed after it (see below), so the two
// together divide the marking, with the younger, hotter half on the helpers
// and the delta -- usually small -- at the finalize.

bool Heap::start_concurrent_mark(RootSource& roots) {
    // Overlapping the mark costs a pool round and, for every store into an old
    // object while the helpers run, a couple of atomic loads and a log. A heap
    // that could have finished its marking inside the handshake pays all of
    // that for nothing, so size is the gate -- the same per-arg idea as
    // `parallel_floor`, with a heavier thumb on the scale. `DREAM_GC_CONCURRENT=1`
    // forces every major through this path (the race detector's door in), and
    // `=0` switches it off regardless of size.
    const int mode = g_env.gc_concurrent;
    if (mode == 0) return false;
    if (mode < 0 && allocated_ < (size_t(1) << 24)) return false;
    GcPool& pool = GcPool::instance();
    if (pool.capacity() < 2) return false;
    const unsigned want = pool.capacity();

    // The round must outlive this call: the helpers work on it after the
    // function has returned, and the barrier feeds it until the finalize. It
    // lives on the heap, not the stack, for exactly that reason.
    if (!mark_round_) mark_round_ = std::make_unique<GcRound>();
    GcRound& round = *mark_round_;
    // `GcRound` is a stack-shaped object (see `trace_in_parallel`), so a roll
    // like this has to reset every field one by one rather than start fresh.
    round.stack.clear();
    round.shared.store(0, std::memory_order_relaxed);
    round.idle.store(0, std::memory_order_relaxed);
    round.arrived = 0;
    round.active = 0;
    round.waiting = 0;
    round.done.store(false, std::memory_order_relaxed);
    round.ctxs.assign(want, GcCtx());
    // The caller does not run the body, so the round belongs to the helpers
    // alone; each announces the count it was given when it arrives.
    round.size = 0;
    round_ = &round;
    marking_ = true;
    mark_done_.store(false, std::memory_order_relaxed);
    concurrent_started_ = now_nanos();

    // Snapshot the roots at this safepoint, while they are still the
    // mutator's. `forward` doubles as a recorder when `recording_` is set -- the
    // same trick `verify_internal` uses, this time collecting entry points
    // rather than walking anything.
    mark_roots_.clear();
    recording_ = &mark_roots_;
    roots.visit_roots(*this);
    recording_ = nullptr;

    // Seed the grey set: every old root is claimed and handed to the shared
    // stack, so the first helper to wake finds something to draw from. Nothing
    // races here -- the helpers have not been told the round exists yet.
    for (Value v : mark_roots_) {
        if (!is_ptr(v)) continue;
        Obj* o = as_obj(v);
        if (claim_mark(o) && !is_atom_object(o->type)) round.stack.push_back(o);
    }
    round.shared.store(round.stack.size(), std::memory_order_relaxed);

    // The helpers' job for this round: arrive, drain the graph, say done. A
    // member -- not a stack object -- for the same reason the round itself is:
    // the pool invokes it from another thread after this function has gone.
    mark_body_ = [this](unsigned index, unsigned count) {
        GcCtx& c = mark_round_->ctxs[index];
        c.q = &c.own;
        c.check_in = kCheckEvery;
        {
            std::lock_guard<std::mutex> g(mark_round_->mutex);
            mark_round_->size = count - 1;  // the caller is not among us
            ++mark_round_->arrived;
            ++mark_round_->active;
        }
        drain<true, true>(c);
        mark_done_.store(true, std::memory_order_relaxed);
    };
    if (!pool.start(want, mark_body_)) {
        // The pool declined between our check and the launch -- another heap
        // took it, or the scheduler's idle hint moved. Back the sealed round
        // out so the caller can do the usual thing, and leave no trace.
        marking_ = false;
        round_ = nullptr;
        mark_round_.reset();
        mark_roots_.clear();
        mark_log_.clear();
        mark_born_.clear();
        return false;
    }
    return true;
}

void Heap::finalize_concurrent_mark(RootSource& roots) {
    const uint64_t started = now_nanos();
    const uint64_t overlap_ns = now_nanos() - concurrent_started_;

    // The mutator may not collect while the helpers have old space open, so the
    // finalize's first act is to take the helpers away. They work on this
    // heap's memory, and the sweep below is about to reconsider all of it.
    marking_ = false;
    GcPool::instance().join();

    // The snapshot was read while the helpers were still running, so it may
    // name young objects this finalize is about to move. Let it go before the
    // nursery is emptied.
    mark_roots_.clear();

    // Re-walk what the overlap could not have seen: the old objects the
    // mutator stored into while the helpers were working (a child can arrive
    // after its parent was scanned), and the old objects born during the mark,
    // whose fresh payloads were filled in after their headers were published.
    // Both go on the remembered set, which the minor-shaped trace below scans
    // first -- and which is sitting ready for exactly that walk.
    remembered_.insert(remembered_.end(), mark_log_.begin(), mark_log_.end());
    mark_log_.clear();
    remembered_.insert(remembered_.end(), mark_born_.begin(), mark_born_.end());
    mark_born_.clear();
    mark_round_.reset();
    round_ = nullptr;

    // Everything the helpers greyed lay in the seeded graph; the mutator has
    // moved on since, so what is left for the trace is the delta, and the
    // trace skips whatever already carries the mark. This is the rest of a
    // major -- promote the young through the remembered set, sweep, empty the
    // nursery, call it done -- with the minor's tail shape on top of the
    // major's mark.
    full_trace_ = true;
    if (!trace_in_parallel(roots, /*minor=*/true)) trace_alone(roots, /*minor=*/true);
    full_trace_ = false;
    if (!sweep_in_parallel()) sweep();
    free_nursery();
    remembered_.clear();
    ++collections_;
    ++major_collections_;
    ++concurrent_marks_;

    gc_threshold_ = live_after_gc_ * 3 + initial_bytes_;
    nursery_hi_ = live_after_gc_ / 4 + initial_bytes_;
    if (nursery_hi_ > nursery_max_) nursery_hi_ = nursery_max_;
    // As in `major_collect`: the nursery is empty and both thresholds are
    // newly fitted, so the last minor's verdict is about a heap that is gone.
    minor_pays_ = true;

    const uint64_t final_ns = now_nanos() - started;
    major_nanos_ += final_ns;
    concurrent_nanos_ += overlap_ns;
    if (g_env.gc_trace)
        std::fprintf(stderr,
                     "; concurrent major: %zu live, %zu held, "
                     "%.2f ms overlap + %.2f ms stop\n",
                     live_after_gc_, block_bytes_, double(overlap_ns) / 1e6,
                     double(final_ns) / 1e6);
    verify_collect(roots, false);
}

// ---------------------------------------------------------------------------
// Sweeping
// ---------------------------------------------------------------------------

void Heap::sweep_block(Block* b, Obj** head, Obj** tail, size_t& live) {
    if (b->big) {
        // A dedicated block carries exactly one big object. It never joins
        // a size class (its size is past the table), so the choice is
        // keep it or hand the whole block back. Nothing else was ever
        // carved into it, so freeing it cannot strand a live chunk.
        Obj* o = reinterpret_cast<Obj*>(b->data);
        if (o->gc & GC_MARK) {
            o->gc &= ~GC_MARK;
            live += o->bytes;
        } else {
            b->dead = true;
        }
        return;
    }

    // A shared block: its chunks tile it exactly, so one walk sees every
    // object and always ends precisely at `used`.
    size_t pos = 0;
    while (pos < b->used) {
        auto* o = reinterpret_cast<Obj*>(b->data + pos);
        uint32_t sz = o->bytes;
        pos += sz;
        if (o->gc & GC_MARK) {
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
            set_free_next(o, head[i]);
            if (!head[i]) tail[i] = o;
            head[i] = o;
        }
    }
}

void Heap::sweep() {
    Obj* head[kHeapClassCount] = {};
    Obj* tail[kHeapClassCount] = {};
    size_t live = 0;
    Block* prev = nullptr;
    for (Block* b = blocks_; b;) {
        Block* next = b->next;
        sweep_block(b, head, tail, live);
        if (b->dead) {
            if (prev) prev->next = next;
            else blocks_ = next;
            if (carve_block_ == b) carve_block_ = nullptr;
            free_block(b);
        } else {
            prev = b;
        }
        b = next;
    }
    merge_free_lists(head, tail);
    Block* cb = blocks_;
    while (cb && cb->big) cb = cb->next;
    carve_block_ = cb;
    live_after_gc_ = live;
    if (live > peak_live_) peak_live_ = live;
    allocated_ = live;
}

bool Heap::sweep_in_parallel() {
    if (allocated_ < parallel_floor(/*minor=*/false)) return false;
    GcPool& pool = GcPool::instance();
    if (pool.capacity() < 2) return false;

    // A sweep divides itself: the blocks are independent, a thread that
    // finishes one takes the next, and nothing needs a lock until the lists
    // are put back together. The one thing a thread may not do is touch the
    // block list, which is why a dead big block is flagged and unlinked
    // afterwards rather than where it is found.
    GcRound round;
    round.ctxs.resize(pool.capacity());
    for (Block* b = blocks_; b; b = b->next) round.blocks.push_back(b);
    round_ = &round;

    bool ran = pool.run(pool.capacity(), [&](unsigned index, unsigned) {
        GcCtx& c = round.ctxs[index];
        for (;;) {
            size_t i = round.next_block.fetch_add(1, std::memory_order_relaxed);
            if (i >= round.blocks.size()) break;
            sweep_block(round.blocks[i], c.free_head, c.free_tail, c.live);
        }
    });
    round_ = nullptr;
    if (!ran) return false;

    size_t live = 0;
    for (GcCtx& c : round.ctxs) {
        merge_free_lists(c.free_head, c.free_tail);
        live += c.live;
    }

    Block* head = nullptr;
    Block* tail = nullptr;
    for (Block* b : round.blocks) {
        if (b->dead) {
            free_block(b);
            continue;
        }
        b->next = nullptr;
        if (tail) tail->next = b;
        else head = b;
        tail = b;
    }
    blocks_ = head;
    Block* cb = blocks_;
    while (cb && cb->big) cb = cb->next;
    carve_block_ = cb;
    live_after_gc_ = live;
    if (live > peak_live_) peak_live_ = live;
    allocated_ = live;
    ++parallel_collections_;
    return true;
}

void Heap::verify_collect(RootSource& roots, bool expect_no_young) {
    if (!g_env.verify_heap) return;
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
            std::string via = "root";
            if (is_ptr(parent)) {
                Obj* pp = as_obj(parent);
                via = addr(pp) + " type " + std::to_string(static_cast<int>(pp->type)) +
                      " gc " + std::to_string(static_cast<int>(pp->gc));
            }
            return problem("a minor collection left a young object reachable at " +
                           addr(o) + " type " + std::to_string(static_cast<int>(o->type)) +
                           " (reached from " + via + ")");
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
