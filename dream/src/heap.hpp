// Per-process heaps.
//
// Every process owns its heap outright: nothing in it is reachable from
// another process, because messages are copied on send. That is what BEAM buys
// with copying semantics, and it buys the same things here -- collection never
// stops the world, never takes a lock, and never has to consider another
// thread's view of an object. It is also what makes lazy thunk update safe
// without atomics: only one process can ever force a given thunk.
//
// The heap is generational: fresh objects bump-allocate in the nursery, and a
// minor collection promotes everything still reachable into old space by
// copying. Old space is a non-moving mark-sweep, which is why collection is
// safe at an interpreter safepoint with no native-stack scanning. Nothing the
// interpreter holds in a C++ local ever moves: `alloc` never collects, and a
// major collection moves nothing at all.
//
// The design this implements is written down in docs/gc.md ("Phase 1").

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "value.hpp"

namespace dream {

/// One free list per size class; the class table lives in heap.cpp.
inline constexpr size_t kHeapClassCount = 55;

/// The collector bits, all in the one-byte `gc` header field. The low two are
/// the mark-sweep cycle's; the generation bits are permanent once set and must
/// survive a collection untouched.
constexpr uint8_t GC_MARK = 1;       ///< in the live set of a full collection
constexpr uint8_t GC_FREE = 2;       ///< already threaded onto a free list
constexpr uint8_t GC_YOUNG = 4;      ///< a nursery object
constexpr uint8_t GC_OLD = 8;        ///< a tenured object
/// A nursery object whose copy already lives in old space: its first payload
/// word holds the forwarding pointer. Only valid while a collection is running.
constexpr uint8_t GC_FORWARDED = 16;
/// A nursery object one collector thread has claimed and is copying right now.
/// Only a parallel collection ever sets it, and only for the length of one
/// `memcpy`: another thread that finds it waits for `GC_FORWARDED` rather than
/// making a second copy, because two copies of one object would be two objects
/// and sharing would not survive the collection.
constexpr uint8_t GC_BUSY = 32;

class Heap;

/// Supplies the roots for a collection. Implemented by Process.
struct RootSource {
    virtual ~RootSource() = default;
    /// Call `heap.forward(&value)` for every live reference.
    virtual void visit_roots(Heap& heap) = 0;
};

class Heap {
public:
    explicit Heap(size_t initial_bytes = 64 * 1024);
    ~Heap();
    Heap(const Heap&) = delete;
    Heap& operator=(const Heap&) = delete;

    /// Allocate a zeroed object with `extra` payload bytes after the header.
    /// Routes to the nursery, except for objects past the top size class,
    /// which tenure immediately in old space. Never collects: growing here
    /// would invalidate references held in C++ locals by the caller.
    /// Collection happens at safepoints instead.
    Obj* alloc(ObjType type, size_t extra);

    /// The same, without clearing the payload. For the constructors below that
    /// write every field they own.
    ///
    /// The clear is pure waste for those: a thunk is twenty-four bytes and all
    /// twenty-four are written immediately afterwards. Anything whose payload a
    /// *caller* fills in later -- a frame's unbound slots, an array handed to a
    /// host -- must still arrive zeroed, because until it is filled the
    /// collector may walk it, and an unwritten slot has to read as `NIL_SLOT`.
    Obj* alloc_bare(ObjType type, size_t extra);

    /// A frame whose first `filled` slots the caller is about to write. Only
    /// the rest is cleared -- an unwritten slot must read as `NIL_SLOT`,
    /// because that is what "not bound yet" is -- and the caller's own stores
    /// do the work the clear would have done twice.
    Value make_frame_filling(Value closure, uint32_t nslots, uint32_t filled);

    // Convenience constructors for the common shapes.
    Value make_float(double v);
    Value make_string(const char* data, uint32_t len);
    /// A view onto bytes the heap does not own -- the image's payload region.
    /// Nothing is copied, so `data` must outlive every heap that can reach it;
    /// today the only such bytes are the `Runtime`'s mapped image.
    Value make_bigstr(const char* data, uint64_t len);
    Value make_cons(Value head, Value tail);
    Value make_array(uint32_t len);
    /// An empty map: a branch with no children. The capacity argument is a
    /// leftover of the open-addressed table and is ignored -- a trie sizes
    /// itself -- but it is kept so that callers with a sensible hint need not
    /// all change.
    Value make_map(uint32_t capacity);
    /// A branch with room for `nslots` packed children. The caller fills in the
    /// bitmap and the slots.
    Value make_map_branch(uint32_t nslots);
    Value make_map_leaf(uint64_t hash, Value key, Value value, Value next);
    Value make_closure(uint32_t func, uint32_t ncaps);
    Value make_thunk(uint32_t node, Value frame);
    Value make_frame(Value closure, uint32_t nslots);
    Value make_pap(Value fn, uint32_t nargs);
    Value make_error(Value kind, Value payload);
    Value make_pid(uint64_t id);
    Value make_module(uint32_t import_index, Value name);

    /// True when the process should collect at its next safepoint: old space
    /// past its threshold, or the nursery past its high-water mark.
    bool should_collect() const { return major_due() || minor_due(); }
    bool major_due() const { return allocated_ >= gc_threshold_; }
    bool minor_due() const { return nursery_bytes_ >= nursery_hi_; }

    /// Full collection: promote every reachable young object and mark-sweep
    /// old space. Safe only at an interpreter safepoint.
    void collect(RootSource& roots);
    /// The same, under its proper name.
    void major_collect(RootSource& roots);
    /// Minor collection: promote every reachable young object out of the
    /// nursery into old space, leaving it empty. The trace starts from the
    /// roots and the remembered set; old space is neither marked nor swept.
    void minor_collect(RootSource& roots);

    /// Hand one root to the collector: mark what `*slot` holds, and rewrite
    /// the slot when promotion moves it. This is the entry point a
    /// `RootSource` uses, and the only one; the scanner reaches the same code
    /// through the templates in heap.cpp, where the collector knows whether it
    /// is running on one thread or several and can afford to say so at compile
    /// time.
    ///
    /// A collection asks the templated form ninety million times in one
    /// self-compile, so it is worth an inlined fast path and two compiled
    /// shapes. The roots are a few thousand, so this one can be a call.
    void forward(Value* slot);

    /// The write barrier. The mutator calls this on an in-place store into an
    /// existing object, so that a minor collection can still find every
    /// old-to-young reference. Cheap, and never needs a lock: only the owning
    /// process writes its own heap.
    void remember_if_old(Obj* dst, Value value) {
        if ((dst->gc & GC_OLD) && is_ptr(value) && (as_obj(value)->gc & GC_YOUNG))
            remembered_.push_back(dst);
    }

    size_t bytes_allocated() const { return allocated_; }
    size_t bytes_live() const { return live_after_gc_; }
    uint64_t collections() const { return collections_; }
    uint64_t major_collections() const { return major_collections_; }
    uint64_t minor_collections() const { return minor_collections_; }
    /// Rounds that were divided across the helper threads rather than run on
    /// the collecting thread alone. A mark and a sweep count separately: they
    /// are separate rounds, and either may fall back on its own.
    uint64_t parallel_rounds() const { return parallel_collections_; }
    /// Bytes of nursery space copied into old space by promotion.
    uint64_t bytes_promoted() const { return promoted_bytes_; }
    /// Nanoseconds this process spent stopped in a collection, split by kind.
    /// Wall time, not CPU time: what a collection costs is what the process
    /// could not do while it ran, and a parallel collector's whole claim is
    /// that those two numbers come apart.
    uint64_t nanos_minor() const { return minor_nanos_; }
    uint64_t nanos_major() const { return major_nanos_; }
    /// Every byte this heap has ever handed out, collections included. What
    /// `bytes_allocated` reports is reset by a collection, so it says how full
    /// the heap is rather than how much work has gone through it -- and the
    /// second question is the one that finds a program allocating quadratically.
    uint64_t bytes_total() const { return total_allocated_; }
    /// The largest the live set has ever been after a collection: the memory
    /// the program actually needs, as opposed to the garbage it made getting
    /// there.
    size_t bytes_peak() const { return peak_live_; }

    /// Deep-copy `v` out of this heap into `dest`. Used for message sends and
    /// for spawning, which are the only two places a value crosses heaps.
    static Value copy_between(Heap& dest, Value v);

    /// Walk everything reachable from `roots` and check the object graph holds
    /// together: every pointer lands on an object this heap owns, every header
    /// is plausible, and no object straddles the end of its block. Returns an
    /// empty string when all is well.
    ///
    /// This is the check that catches a collector or allocator bug at the
    /// moment it happens rather than as a crash somewhere unrelated later.
    std::string verify(RootSource& roots);

    /// True when this heap owns `bytes` starting at `p`.
    bool owns(const void* p, size_t bytes) const;

    /// Set from the DREAM_VERIFY_HEAP environment variable: verify after every
    /// collection. Slow, and meant for chasing exactly this class of bug.
    static bool verify_after_gc();

private:
    struct Block {
        Block* next;
        size_t size;
        size_t used;
        uint8_t* data;
        /// True when the whole block is one big object's. Nothing else may be
        /// carved into it, and sweep may then free the whole block without
        /// stranding any live chunk.
        bool big;
        /// Set by the sweep of a big block whose object died. The block list
        /// is rebuilt afterwards rather than unlinked from as it is walked,
        /// because in a parallel sweep the thread that finds a dead block is
        /// not the one that owns the list.
        bool dead;
    };

    /// One collector thread's private state for the length of one collection.
    /// Defined in heap.cpp: nothing outside the collector has any business
    /// with it, but a parallel round needs an array of them and the array has
    /// to live somewhere.
    struct GcCtx;
    /// The state a parallel round shares: the overflow work stack that
    /// balances the trace, the block snapshot the sweep divides, and the lock
    /// that guards the free lists while several threads carve from them.
    struct GcRound;

    Block* new_block(size_t bytes);
    void free_block(Block* b);
    void free_blocks(Block* b);
    /// Free every nursery block: after a major collection every reachable
    /// young object has been promoted, so the whole nursery is dead.
    void free_nursery();

    /// Take `sz` bytes of new space in old space: a fresh chunk carved off the
    /// tail of the carve block (or a new block, or a split from the class
    /// lists). Used by promotion; normal allocation goes to the nursery.
    Obj* carve(size_t sz);
    /// An entire block dedicated to one large object.
    Obj* carve_big(size_t sz);
    /// Bump-allocate `sz` bytes from the nursery blocks.
    Obj* alloc_nursery(uint32_t sz);
    /// Add a block to the nursery and allocate from it. The cold half of
    /// `alloc_nursery`, reached only when no existing block has room.
    Obj* grow_nursery(uint32_t sz);

    /// Widen the nursery when a collection promoted a large share of what it
    /// looked at. Called at the end of a minor collection.
    void grow_nursery_if_crowded(size_t promoted, size_t looked_at);

    // The collector proper, in two compiled shapes. `Par` is what the code
    // would otherwise have had to ask at runtime, ninety million times a
    // collection: whether another thread can be looking at the same object.
    // With it false these are exactly the single-threaded collector, plain
    // loads and stores throughout; with it true every read of a `gc` byte is
    // an atomic, the mark is a claim, and promotion is a claim followed by a
    // copy that other threads wait for. Both are instantiated in heap.cpp.

    /// Mark what `*slot` holds, rewriting the slot if promotion moves it.
    template <bool Par> void forward_in(GcCtx& c, Value* slot);
    /// The half of `forward_in` that has something to do: a young object to
    /// promote, one already promoted this cycle, or an indirection to fold
    /// away.
    template <bool Par> void forward_slow(GcCtx& c, Value* slot);
    /// Place one reference onto the live set.
    template <bool Par> void mark_object(GcCtx& c, Value v);
    /// Copy a nursery object to old space and leave a forwarding pointer
    /// behind, so that the second reference to it finds the same copy.
    template <bool Par> Obj* promote(GcCtx& c, Obj* o);
    /// Update the references inside one object, pushing each further out.
    template <bool Par> void scan_object(GcCtx& c, Obj* o);
    /// Take `sz` bytes of old space to promote into. Serially that is the
    /// heap's own `carve`; in parallel it is the thread's private chunk lists
    /// and its private carve block, so that promotion needs no lock per
    /// object.
    template <bool Par> Obj* gc_carve(GcCtx& c, uint32_t sz);
    /// Drain the thread's own queue, taking from and giving to the round's
    /// shared stack, until every thread has run out of work at once.
    template <bool Par> void drain(GcCtx& c);

    /// The old-space copy a forwarded nursery object points at, read back out
    /// of its first payload word.
    static Value forward_target(Obj* o) {
        return *reinterpret_cast<Value*>(o + 1);
    }
    /// Wait for the thread that claimed `o` to finish copying it, and answer
    /// with the copy. A claim covers one `memcpy` of at most four kilobytes,
    /// so this spins rather than sleeps.
    static Value await_forward(Obj* o);

    /// Trace the live set, in parallel when the pool can spare the threads and
    /// the collection is large enough to be worth the handshake. Returns false
    /// when it did not run, and the caller must trace on its own thread.
    bool trace_in_parallel(RootSource& roots, bool minor);
    /// The same trace on the collecting thread alone: what every collection
    /// did before there was a pool, and what most of them still do.
    void trace_alone(RootSource& roots, bool minor);
    /// Sweep old space across several threads, the blocks divided between
    /// them. Returns false when it did not run.
    bool sweep_in_parallel();
    /// Split up to a batch of chunks off the shared free list for `cls` into
    /// `c`. False when the list is empty and the caller must carve.
    bool refill_free(GcCtx& c, size_t cls);
    /// A block for one collector thread to carve promoted objects out of.
    Block* thread_block(uint32_t sz);
    /// Give half of this thread's queue to the round's shared stack.
    void share_work(GcCtx& c);
    /// Take a batch back when the local queue has run dry, waiting for one to
    /// appear. False when the trace is over and no batch is coming.
    bool take_work(GcCtx& c);

    /// Sweep one block into `head`/`tail`, a chain per size class, counting
    /// what survives. Shared by the serial sweep and the parallel one, which
    /// differ only in whose chains they fill and who rebuilds the block list.
    void sweep_block(Block* b, Obj** head, Obj** tail, size_t& live);
    /// Prepend one thread's swept chains to the heap's free lists.
    void merge_free_lists(Obj** head, Obj** tail);

    /// Thread every dead object in every old block onto the size-class free
    /// lists. Also clears the mark bits of what survives and recounts the
    /// live set. Called only after a major collection.
    void sweep();

    /// Run the graph walk after a collection when `DREAM_VERIFY_HEAP` asks
    /// for it. `expect_no_young` is the promise a minor collection makes:
    /// everything reachable was promoted, so nothing may still be young.
    void verify_collect(RootSource& roots, bool expect_no_young);
    /// The walk itself, shared by `verify` and `verify_collect`.
    std::string verify_internal(RootSource& roots, bool no_young);

    std::array<Obj*, kHeapClassCount> free_lists_{};
    /// Old-space blocks. Everything that has survived a collection lives here.
    Block* blocks_ = nullptr;
    Block* carve_block_ = nullptr;
    /// The nursery: fresh allocations bump through these and every reachable
    /// one is copied to old space at the next collection, after which the
    /// blocks are reset empty (or freed whole by a major).
    std::vector<Block*> nursery_;
    /// Which block is being bumped in. An *index*, not a pointer, because a
    /// minor collection empties every block and allocation has to start over
    /// at the first one: a cursor left at the end would add a block per
    /// collection and never touch the ones it had just emptied, so the nursery
    /// grew by its whole size at every minor until the next major freed it.
    size_t nursery_at_ = 0;
    size_t nursery_bytes_ = 0;
    /// The nursery's high-water mark: past it, a minor collection is due.
    size_t nursery_hi_;
    /// How large the nursery may grow. See `grow_nursery_if_crowded`.
    size_t nursery_max_;
    /// Old objects the mutator has been made to point at young ones. Drained
    /// (not deduplicated) by scanning each entry at the next minor collection.
    std::vector<Obj*> remembered_;
    size_t allocated_ = 0;
    uint64_t total_allocated_ = 0;
    size_t peak_live_ = 0;
    size_t gc_threshold_;
    size_t initial_bytes_;
    size_t live_after_gc_ = 0;
    uint64_t collections_ = 0;
    uint64_t major_collections_ = 0;
    uint64_t minor_collections_ = 0;
    uint64_t promoted_bytes_ = 0;
    uint64_t parallel_collections_ = 0;
    uint64_t minor_nanos_ = 0;
    uint64_t major_nanos_ = 0;
    /// True while a major collection is running: `forward` marks the old
    /// objects it reaches as well as promoting the young ones.
    bool full_trace_ = false;
    /// True while a parallel round is running, which is what tells `forward`
    /// -- the root entry point, and the only part of the collector that is not
    /// compiled twice -- which shape of the collector to call into.
    bool parallel_ = false;
    /// The context the roots are handed to: the collecting thread's own.
    GcCtx* root_ctx_ = nullptr;
    /// Non-null only while a parallel round is running.
    GcRound* round_ = nullptr;
    /// The grey set while collecting: objects marked but not yet scanned.
    std::vector<Obj*> scan_queue_;
    /// Non-null while verifying: `forward` records roots rather than marking.
    std::vector<Value>* recording_ = nullptr;
};

/// The size in bytes of an object, from its header.
inline size_t object_size(const Obj* o) { return o->bytes; }

}  // namespace dream