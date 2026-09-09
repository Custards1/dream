// Per-process heaps.
//
// Every process owns its heap outright: nothing in it is reachable from
// another process, because messages are copied on send. That is what BEAM buys
// with copying semantics, and it buys the same things here -- collection never
// stops the world, never takes a lock, and never has to consider another
// thread's view of an object. It is also what makes lazy thunk update safe
// without atomics: only one process can ever force a given thunk.
//
// The collector is an atomic mark-sweep: tri-color mark from the roots into a
// worklist, then a sweep that threads every dead object's space onto a
// segregated free list. Nothing moves, which is why collection is safe at an
// interpreter safepoint with no native-stack scanning -- the live set is
// exactly the process's explicit stacks, and every address stays valid across
// a collection.

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
    /// Never collects: growing here would invalidate references held in C++
    /// locals by the caller. Collection happens at safepoints instead.
    Obj* alloc(ObjType type, size_t extra);

    // Convenience constructors for the common shapes.
    Value make_float(double v);
    Value make_string(const char* data, uint32_t len);
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

    /// True when the process should collect at its next safepoint.
    bool should_collect() const { return allocated_ >= gc_threshold_; }

    /// Collect, tracing from `roots`. Safe only at an interpreter safepoint.
    void collect(RootSource& roots);

    /// Mark the reference `*slot` holds. `forward` is the name a root source
    /// and the scanner use to hand a reference to the collector; under
    /// mark-sweep nothing moves, so the reference itself is never rewritten.
    void forward(Value* slot);

    size_t bytes_allocated() const { return allocated_; }
    size_t bytes_live() const { return live_after_gc_; }
    uint64_t collections() const { return collections_; }
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
    };

    Block* new_block(size_t bytes);
    void free_blocks(Block* b);

    /// Take `sz` bytes of new space: a fresh chunk carved off the tail of the
    /// carve block (or a new block, or a split from the class lists).
    Obj* carve(size_t sz);
    /// An entire block dedicated to one large object.
    Obj* carve_big(size_t sz);

    /// Place one reference onto the live set. Shared by `forward` and the
    /// collector itself; `out` stays null for a value already marked.
    void mark_object(Value v);

    /// Thread every dead object in every block onto the size-class free lists.
    /// Also clears the mark bits of what survives and recounts the live set.
    void sweep();

    /// Update the references inside a marked object. Called from the sweep of
    /// the worklist; `forward` below is what pushes each reference further out.
    void scan_object(Obj* o);

    std::array<Obj*, kHeapClassCount> free_lists_{};
    Block* blocks_ = nullptr;
    Block* carve_block_ = nullptr;
    size_t allocated_ = 0;
    uint64_t total_allocated_ = 0;
    size_t peak_live_ = 0;
    size_t gc_threshold_;
    size_t initial_bytes_;
    size_t live_after_gc_ = 0;
    uint64_t collections_ = 0;
    /// The grey set while collecting: objects marked but not yet scanned.
    std::vector<Obj*> scan_queue_;
    /// Non-null while verifying: `forward` records roots rather than marking.
    std::vector<Value>* recording_ = nullptr;
};

/// The size in bytes of an object, from its header.
inline size_t object_size(const Obj* o) { return o->bytes; }

}  // namespace dream