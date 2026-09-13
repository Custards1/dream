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
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
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
    ///
    /// With a concurrent mark in flight neither triggers a *new* collection --
    /// the helpers may be walking an old object's fields at this very moment,
    /// so the only collection this process may do is the finalize of the one
    /// it already owes. The finalize becomes due when the helpers have drained,
    /// or when the nursery has outgrown twice its high-water mark (the normal
    /// mark, which would have triggered a minor, is not worth the handshake;
    /// twice the normal mark is). The doubling is capped at the nursery's
    /// maximum so it cannot handshake for a grow that was not allowed.
    bool should_collect() const {
        if (marking_)
            return mark_done_.load(std::memory_order_relaxed) ||
                   nursery_bytes_ >= std::min(nursery_max_, nursery_hi_ * 2);
        return major_due() || minor_due();
    }
    /// Bytes held in old space: everything allocated that the nursery is not
    /// still holding. `allocated_` counts both generations, because it is what
    /// says how full the heap is; this is the half a major is about.
    size_t old_bytes() const {
        return allocated_ >= nursery_bytes_ ? allocated_ - nursery_bytes_ : 0;
    }

    /// Old space past the threshold the last full collection fitted to it.
    ///
    /// The nursery is deliberately not in that comparison -- `gc_threshold_`
    /// is fitted to what a major found live in *old* space, so counting the
    /// nursery against it measures a quantity a minor is about to empty
    /// against one only a major can move. On the self-compile, separating them
    /// took the majors from 176 ms to 16 ms: a major that fires while the
    /// nursery is full promotes the whole of it first, so the conflation was
    /// not just miscounting, it was picking the expensive collection.
    ///
    /// The second clause is what the first one costs. A nursery can be
    /// hundreds of megabytes past its high-water mark by the time a safepoint
    /// comes round, because a deep force (`strict!`) runs the interpreter in a
    /// nested loop that may not collect -- a native above it holds raw
    /// pointers, which is the "alloc never collects" rule -- and the safepoint
    /// after it lands at the one instant when everything just allocated is
    /// still live. A minor there is a copy with no collection in it: it
    /// promotes the lot, and the lot dies immediately afterwards. So when the
    /// last minor promoted more than three quarters of what it looked at, the
    /// nursery is treated as old space's problem again and the whole heap is
    /// the trigger. A `strict!` in a loop collected 2.4x faster for it, and
    /// the compile does not notice, because its minors promote about a
    /// quarter.
    bool major_due() const {
        if (old_bytes() >= gc_threshold_) return true;
        return !minor_pays_ && allocated_ >= gc_threshold_;
    }
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

    /// Phase 2's step 3: a major whose *marking* overlaps the mutator.
    ///
    /// `start_concurrent_mark` snapshots the roots at this safepoint, seeds
    /// the helper threads with them, and returns at once -- the helpers mark
    /// the old objects they can reach while the mutator keeps running, and
    /// skip young ones, which move and so cannot be marked ahead of time.
    /// `finalize_concurrent_mark` then does what a major does: finish the
    /// graph with one more pass over what the overlap could have missed, sweep,
    /// and empty the nursery.
    ///
    /// The mutator runs no collection of any kind between the two calls --
    /// `should_collect` says "finalize" when `marking()` is true, and nothing
    /// else. `finalize_concurrent_mark` is the one legal way out.
    ///
    /// `start_concurrent_mark` returns false when the helpers cannot help (the
    /// heap is too small, the pool has no threads, another heap owns it); the
    /// caller then collects in the old whole-world way and the pair is not
    /// entered. `marking()` is the third state, told from the two methods by
    /// who is asking: `should_collect`/`maybe_collect` drive it, and the rest
    /// of the runtime only ever reads `marking()` to pick the legal next move.
    bool start_concurrent_mark(RootSource& roots);
    void finalize_concurrent_mark(RootSource& roots);
    /// True between `start_concurrent_mark` and `finalize_concurrent_mark`.
    bool marking() const { return marking_; }
    /// True once the helpers have drained their last batch: the mark is done
    /// and only the finalize's housekeeping is owed. Read by `should_collect`
    /// to make the finalize prompt when nothing more can be gained from the
    /// overlap, so a heap does not drift on with a finished mark behind it.
    bool mark_done() const { return mark_done_.load(std::memory_order_relaxed); }

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
    ///
    /// The reads are atomics because the one byte being read is the same one a
    /// concurrent mark's helper claims with `fetch_or` at the same moment --
    /// same instruction as a plain load, but a plain load against an atomic
    /// write is a data race whether or not it matters.
    ///
    /// During a concurrent mark the same store can also uncover a subgraph the
    /// helpers have already drawn: an object is logged the first time a store
    /// lands in one that is marked, so the finalize re-walks it -- and the
    /// work is handed to the helpers now, so the re-walk happens concurrently
    /// rather than at the finalize. A store into an object the helpers have
    /// not marked yet needs none of this: their scan, when it comes, sees it.
    void remember_if_old(Obj* dst, Value value) {
        uint8_t dgc = std::atomic_ref<uint8_t>(dst->gc).load(std::memory_order_acquire);
        if ((dgc & GC_OLD) && is_ptr(value) &&
            (std::atomic_ref<uint8_t>(as_obj(value)->gc).load(std::memory_order_acquire) &
             GC_YOUNG))
            remembered_.push_back(dst);
        if (marking_ && (dgc & GC_MARK)) log_mutation(dst);
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
    /// Nanoseconds the mutator spent running *over* a concurrent mark -- the
    /// window in which the helpers were working in the background. Not stopped
    /// time; it is reported beside the stopped time precisely so the two can
    /// be told apart.
    uint64_t nanos_concurrent() const { return concurrent_nanos_; }
    /// Full collections whose marking ran on the helper threads rather than on
    /// the collecting thread alone.
    uint64_t concurrent_marks() const { return concurrent_marks_; }
    /// Every byte this heap has ever handed out, collections included. What
    /// `bytes_allocated` reports is reset by a collection, so it says how full
    /// the heap is rather than how much work has gone through it -- and the
    /// second question is the one that finds a program allocating quadratically.
    uint64_t bytes_total() const { return total_allocated_; }
    /// The largest the live set has ever been after a collection: the memory
    /// the program actually needs, as opposed to the garbage it made getting
    /// there.
    size_t bytes_peak() const { return peak_live_; }
    /// The most this heap has ever held *from the OS*: every block it had
    /// malloc'd at once, nursery and old space together, whether or not the
    /// objects in them were live.
    ///
    /// This is the number a person watching `top` sees, and it is not
    /// `bytes_peak`. The difference between them is everything the collector
    /// is carrying and not using -- headroom above the live set, chunks on the
    /// free lists, and the blocks a sweep emptied but cannot hand back because
    /// something else in them is still alive.
    size_t bytes_peak_held() const { return peak_block_bytes_; }
    /// What was *allocated* at the moment the heap held the most -- objects
    /// that existed, live or merely not yet proven dead. Reported beside the
    /// held figure because the gap between the two is the collector's own
    /// waste (headroom and holes), and the gap between *this* and the live set
    /// is what a different trigger policy could in principle recover. On the
    /// self-compile it is 88% of the peak, which is what says the peak is a
    /// heap full of objects rather than a heap full of holes -- and so why
    /// four different threshold policies all failed to move it. See docs/gc.md.
    size_t bytes_alloc_at_peak() const { return peak_allocated_; }

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
    /// `Con` is the concurrent-mark shape: instead of promoting and absorbing,
    /// it hands each reference to `mark_ref`, which marks what the helpers may
    /// reach and skips the young.
    template <bool Par, bool Con> void forward_in(GcCtx& c, Value* slot);
    /// The half of `forward_in` that has something to do: a young object to
    /// promote, one already promoted this cycle, or an indirection to fold
    /// away.
    template <bool Par> void forward_slow(GcCtx& c, Value* slot);
    /// Place one reference onto the live set.
    template <bool Par> void mark_object(GcCtx& c, Value v);
    /// The concurrent mark's own claim on one reference. Unlike `mark_object`
    /// it will not touch a young object -- those move at the finalize, and a
    /// mark on a soon-to-be-corpse is a mark on nothing -- and it collapses no
    /// indirection chains, because a chain it "fixed" would be a write the
    /// mutator is reading. It is called by the helpers on every reference they
    /// scan and by nothing else.
    bool mark_ref(GcCtx& c, Value v);
    /// Claim an old object for the concurrent mark: set GC_MARK, say whether
    /// this caller won. Shared by `mark_ref` and the seed in
    /// `start_concurrent_mark`.
    static bool claim_mark(Obj* o);
    /// Copy a nursery object to old space and leave a forwarding pointer
    /// behind, so that the second reference to it finds the same copy.
    template <bool Par> Obj* promote(GcCtx& c, Obj* o);
    /// Update the references inside one object, pushing each further out.
    template <bool Par, bool Con> void scan_object(GcCtx& c, Obj* o);
    /// Take `sz` bytes of old space to promote into. Serially that is the
    /// heap's own `carve`; in parallel it is the thread's private chunk lists
    /// and its private carve block, so that promotion needs no lock per
    /// object.
    template <bool Par> Obj* gc_carve(GcCtx& c, uint32_t sz);
    /// Drain the thread's own queue, taking from and giving to the round's
    /// shared stack, until every thread has run out of work at once.
    template <bool Par, bool Con> void drain(GcCtx& c);

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
    /// Bytes currently malloc'd for blocks, and the most there have ever been.
    size_t block_bytes_ = 0;
    size_t peak_block_bytes_ = 0;
    size_t peak_allocated_ = 0;
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
    /// Whether the last minor collection was worth doing: it promoted no more
    /// than three quarters of what it looked at, so the nursery was mostly
    /// garbage and copying the survivors bought something. False after a minor
    /// that promoted nearly everything -- see `major_due`, which then stops
    /// preferring a minor. Starts true: a heap that has never collected has no
    /// evidence against the generational bet.
    bool minor_pays_ = true;

    /// Non-null while verifying: `forward` records roots rather than marking.
    std::vector<Value>* recording_ = nullptr;

    /// Note a store into a marked object during a concurrent mark: the object
    /// goes on the re-walk list (`mark_log_`, drained by the finalize) and on
    /// the helpers' shared stack, so its newly uncovered subgraph is drawn in
    /// the background rather than at the finalize. The caller -- `remember_if_old`
    /// -- has already checked that a mark is running and the object is marked.
    void log_mutation(Obj* dst);

    /// True while a concurrent mark's helpers are out. Nothing else in the
    /// heap may collect then, which is what `should_collect` enforces.
    bool marking_ = false;
    /// Set by a helper the moment its drain returns, meaning the mark's last
    /// batch was the last anywhere: the finalize has nothing further to gain
    /// by waiting. Written by the helpers, read by the process's own thread.
    std::atomic<bool> mark_done_{false};
    /// The roots, snapshotted at the safepoint where `start_concurrent_mark`
    /// was called. The helpers see the heap as it was then, not as it becomes
    /// while they work; young values in the snapshot are skipped and are
    /// re-found by the finalize's walk of the live roots. Kept until the
    /// finalize, because the values answer for objects the helpers still hold.
    std::vector<Value> mark_roots_;
    /// Old objects a store reached while a mark was running and the helpers
    /// had moved past. The finalize scans these once more.
    std::vector<Obj*> mark_log_;
    /// Old objects allocated during a mark. They are born marked -- grey would
    /// let a helper scan a half-built body -- and each carries a fresh young
    /// payload written after its header was published, so marking them while
    /// still building would miss the point. The finalize scans these once, at
    /// the end, when the fills are all done.
    std::vector<Obj*> mark_born_;
    /// The concurrent mark's round, alive from `start_concurrent_mark` until
    /// the finalize's join. It must outlive `start_concurrent_mark`'s stack,
    /// and the finalize may be many reductions later.
    std::unique_ptr<GcRound> mark_round_;
    /// The mark's body, bound to the heap and the round. Same lifetime as
    /// `mark_round_`: the pool calls it on another thread, so it must be a
    /// member rather than a stack object the start function would leave.
    std::function<void(unsigned, unsigned)> mark_body_;
    uint64_t concurrent_marks_ = 0;
    uint64_t concurrent_nanos_ = 0;
    /// The clock reading taken when the helpers were launched; the overlap
    /// window is the difference to the start of the finalize.
    uint64_t concurrent_started_ = 0;
};

/// The size in bytes of an object, from its header.
inline size_t object_size(const Obj* o) { return o->bytes; }

}  // namespace dream