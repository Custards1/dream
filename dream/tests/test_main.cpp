// Unit tests for the VM's building blocks. End-to-end behaviour is covered by
// tests/e2e.sh, which compiles real Dream programs and checks their output.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "builtins.hpp"
#include "heap.hpp"
#include "image.hpp"
#include "process.hpp"
#include "runtime.hpp"
#include "value.hpp"

using namespace dream;

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++checks;                                                              \
        if (!(cond)) {                                                         \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

#define CHECK_EQ(a, b)                                                         \
    do {                                                                       \
        ++checks;                                                              \
        auto va = (a);                                                         \
        auto vb = (b);                                                         \
        if (!(va == vb)) {                                                     \
            std::printf("  FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b); \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

static void test_value_tagging() {
    std::printf("value tagging\n");
    for (int64_t n : {int64_t(0), int64_t(1), int64_t(-1), int64_t(42), int64_t(-99999),
                      (int64_t(1) << 61), -(int64_t(1) << 61)}) {
        Value v = make_fixnum(n);
        CHECK(is_fixnum(v));
        CHECK(!is_ptr(v));
        CHECK(!is_imm(v));
        CHECK_EQ(fixnum_value(v), n);
    }

    CHECK(is_unit(UNIT));
    CHECK(is_bool(TRUE_V));
    CHECK(is_bool(FALSE_V));
    CHECK(truthy(TRUE_V));
    CHECK(!truthy(FALSE_V));
    CHECK(is_nil(NIL));
    CHECK(!is_fixnum(UNIT));
    CHECK(!is_ptr(UNIT));

    CHECK(is_atom(make_atom(7)));
    CHECK_EQ(imm_payload(make_atom(7)), 7u);
    CHECK(is_char(make_char(0x1F600)));
    CHECK_EQ(imm_payload(make_char(0x1F600)), 0x1F600u);
    CHECK(is_builtin(make_builtin(3)));

    // Distinct immediates must never collide.
    CHECK(UNIT != NIL);
    CHECK(TRUE_V != FALSE_V);
    CHECK(make_atom(0) != make_char(0));
    CHECK(make_atom(0) != UNIT);
}

static void test_heap_alloc() {
    std::printf("heap allocation\n");
    Heap h(4096);
    Value f = h.make_float(2.5);
    CHECK(is_ptr(f));
    CHECK(is_obj(f, ObjType::Float));
    CHECK_EQ(static_cast<FloatObj*>(as_obj(f))->value, 2.5);

    Value s = h.make_string("hello", 5);
    CHECK(is_obj(s, ObjType::Str));
    CHECK_EQ(static_cast<StrObj*>(as_obj(s))->len, 5u);
    CHECK(std::memcmp(static_cast<StrObj*>(as_obj(s))->data(), "hello", 5) == 0);

    // A null pointer reserves space for the caller to fill; concatenation
    // depends on it, and getting it wrong reads from address zero.
    Value blank = h.make_string(nullptr, 4);
    CHECK_EQ(static_cast<StrObj*>(as_obj(blank))->len, 4u);
    std::memcpy(static_cast<StrObj*>(as_obj(blank))->data(), "abcd", 4);
    CHECK(std::memcmp(static_cast<StrObj*>(as_obj(blank))->data(), "abcd", 4) == 0);

    // Allocation past the initial block must chain, not corrupt.
    std::vector<Value> many;
    for (int i = 0; i < 2000; ++i) many.push_back(h.make_float(double(i)));
    for (int i = 0; i < 2000; ++i) {
        CHECK_EQ(static_cast<FloatObj*>(as_obj(many[size_t(i)]))->value, double(i));
    }
}

/// A root source that holds a fixed list of values, so the collector can be
/// tested without a whole process.
struct VectorRoots : RootSource {
    std::vector<Value> values;
    void visit_roots(Heap& h) override {
        for (Value& v : values) h.forward(&v);
    }
};

static void test_gc_keeps_live_and_drops_dead() {
    std::printf("garbage collection\n");
    Heap h(4096);
    VectorRoots roots;

    // A list of 100 floats, held live.
    Value list = NIL;
    for (int i = 0; i < 100; ++i) list = h.make_cons(h.make_float(double(i)), list);
    roots.values.push_back(list);

    // Ten thousand unreachable floats.
    for (int i = 0; i < 10000; ++i) (void)h.make_float(double(i));

    size_t before = h.bytes_allocated();
    h.collect(roots);
    CHECK(h.bytes_live() < before / 2);
    CHECK_EQ(h.collections(), uint64_t(1));

    // The live list must have survived intact, with its contents in order.
    Value cur = roots.values[0];
    for (int i = 99; i >= 0; --i) {
        CHECK(is_obj(cur, ObjType::Cons));
        auto* c = static_cast<ConsObj*>(as_obj(cur));
        CHECK_EQ(static_cast<FloatObj*>(as_obj(c->head))->value, double(i));
        cur = c->tail;
    }
    CHECK(is_nil(cur));
}

static void test_gc_preserves_sharing() {
    std::printf("garbage collection preserves sharing\n");
    Heap h(4096);
    VectorRoots roots;
    Value shared = h.make_float(1.25);
    // The same object referenced twice must still be one object afterwards,
    // or thunk update would stop being observable to every sharer.
    roots.values.push_back(h.make_cons(shared, shared));
    h.collect(roots);

    auto* c = static_cast<ConsObj*>(as_obj(roots.values[0]));
    CHECK_EQ(c->head, c->tail);
    CHECK_EQ(static_cast<FloatObj*>(as_obj(c->head))->value, 1.25);
}

static void test_gc_collapses_indirections() {
    std::printf("garbage collection collapses indirections\n");
    Heap h(4096);
    VectorRoots roots;
    Value target = h.make_float(7.0);
    Value ind = h.make_thunk(0, UNIT);
    as_obj(ind)->type = ObjType::Indirect;
    static_cast<IndirectObj*>(as_obj(ind))->target = target;
    roots.values.push_back(h.make_cons(ind, NIL));

    h.collect(roots);
    auto* c = static_cast<ConsObj*>(as_obj(roots.values[0]));
    CHECK(is_obj(c->head, ObjType::Float));  // the hop is gone, not just followed
}

static void test_gc_handles_cycles() {
    std::printf("garbage collection handles cycles\n");
    Heap h(4096);
    VectorRoots roots;
    Value a = h.make_cons(UNIT, NIL);
    Value b = h.make_cons(UNIT, a);
    static_cast<ConsObj*>(as_obj(a))->tail = b;  // a -> b -> a
    roots.values.push_back(a);
    h.collect(roots);

    Value ra = roots.values[0];
    Value rb = static_cast<ConsObj*>(as_obj(ra))->tail;
    CHECK_EQ(static_cast<ConsObj*>(as_obj(rb))->tail, ra);
}

static void test_heap_verifier_accepts_a_healthy_heap() {
    std::printf("heap verifier accepts a healthy heap\n");
    Heap h(4096);
    VectorRoots roots;
    Value list = NIL;
    for (int i = 0; i < 20; ++i) {
        list = h.make_cons(h.make_string("item", 4), list);
    }
    Value arr = h.make_array(3);
    for (uint32_t i = 0; i < 3; ++i) static_cast<ArrayObj*>(as_obj(arr))->items()[i] = list;
    Value map = h.make_map(8);
    roots.values = {list, arr, map, h.make_float(1.0)};
    CHECK_EQ(h.verify(roots), std::string());

    // And it must still be happy after a collection.
    h.collect(roots);
    CHECK_EQ(h.verify(roots), std::string());
}

static void test_heap_verifier_catches_corruption() {
    std::printf("heap verifier catches corruption\n");

    {   // A pointer that does not land in this heap.
        Heap h(4096);
        VectorRoots roots;
        Value cell = h.make_cons(UNIT, NIL);
        roots.values = {cell};
        static_cast<ConsObj*>(as_obj(cell))->head = 0x1000;  // aligned but foreign
        CHECK(h.verify(roots).find("does not land in this heap") != std::string::npos);
    }
    {   // A header claiming a size that runs past its block.
        Heap h(4096);
        VectorRoots roots;
        Value f = h.make_float(1.0);
        roots.values = {f};
        as_obj(f)->bytes = 1u << 20;
        CHECK(h.verify(roots).find("runs past the end") != std::string::npos);
    }
    {   // An unknown type tag.
        Heap h(4096);
        VectorRoots roots;
        Value f = h.make_float(1.0);
        roots.values = {f};
        as_obj(f)->type = static_cast<ObjType>(99);
        CHECK(h.verify(roots).find("unknown type") != std::string::npos);
    }
    {   // An array whose length does not fit its allocation. This is the shape
        // the map-growth bug took: a header describing more than it owns.
        Heap h(4096);
        VectorRoots roots;
        Value a = h.make_array(2);
        roots.values = {a};
        static_cast<ArrayObj*>(as_obj(a))->len = 1000;
        CHECK(h.verify(roots).find("too small") != std::string::npos);
    }
    {   // Tagging already rules out a misaligned pointer: such a word is not
        // classified as a pointer at all, so it is never dereferenced. The
        // verifier's alignment check is a backstop for a hand-built object.
        Heap h(4096);
        Value cell = h.make_cons(UNIT, NIL);
        CHECK(is_ptr(cell));
        CHECK(!is_ptr(cell + 4));
        VectorRoots roots;
        roots.values = {cell};
        static_cast<ConsObj*>(as_obj(cell))->head = cell + 4;
        CHECK_EQ(h.verify(roots), std::string());
    }
}

static void test_heap_verifier_follows_every_object_kind() {
    std::printf("heap verifier follows every object kind\n");
    // Each kind has its own traversal; a kind the walker forgets would hide
    // corruption underneath it, so plant a bad pointer under each in turn.
    Heap h(8192);

    {
        VectorRoots roots;
        Value cl = h.make_closure(0, 2);
        roots.values = {cl};
        static_cast<ClosureObj*>(as_obj(cl))->caps()[1] = 0x2000;
        CHECK(!h.verify(roots).empty());
    }
    {
        VectorRoots roots;
        Value fr = h.make_frame(h.make_closure(0, 0), 3);
        roots.values = {fr};
        static_cast<FrameObj*>(as_obj(fr))->slots()[2] = 0x2000;
        CHECK(!h.verify(roots).empty());
    }
    {
        VectorRoots roots;
        Value th = h.make_thunk(0, UNIT);
        roots.values = {th};
        static_cast<ThunkObj*>(as_obj(th))->frame = 0x2000;
        CHECK(!h.verify(roots).empty());
    }
    {
        VectorRoots roots;
        Value e = h.make_error(make_atom(1), UNIT);
        roots.values = {e};
        static_cast<ErrorObj*>(as_obj(e))->payload = 0x2000;
        CHECK(!h.verify(roots).empty());
    }
    {
        // A branch whose child is not a pointer to anything.
        VectorRoots roots;
        Value m = h.make_map_branch(1);
        auto* branch = static_cast<MapObj*>(as_obj(m));
        branch->bitmap = 1;
        branch->count = 1;
        branch->slots()[0] = 0x2000;
        roots.values = {m};
        CHECK(!h.verify(roots).empty());
    }
    {
        // And a leaf with a bad key, which is where a map's own values live.
        VectorRoots roots;
        Value leaf = h.make_map_leaf(1, UNIT, UNIT, NIL_SLOT);
        Value m = h.make_map_branch(1);
        auto* branch = static_cast<MapObj*>(as_obj(m));
        branch->bitmap = 1;
        branch->count = 1;
        branch->slots()[0] = leaf;
        static_cast<MapLeafObj*>(as_obj(leaf))->key = 0x2000;
        roots.values = {m};
        CHECK(!h.verify(roots).empty());
    }
    {
        VectorRoots roots;
        Value pap = h.make_pap(h.make_closure(0, 0), 1);
        roots.values = {pap};
        static_cast<PapObj*>(as_obj(pap))->args()[0] = 0x2000;
        CHECK(!h.verify(roots).empty());
    }
}

static void test_cross_heap_copy() {
    std::printf("cross-heap copying\n");
    Heap a(4096), b(4096);
    Value list = a.make_cons(a.make_float(1.0),
                             a.make_cons(a.make_string("two", 3), NIL));
    Value copied = Heap::copy_between(b, list);

    CHECK(copied != list);
    auto* c = static_cast<ConsObj*>(as_obj(copied));
    CHECK_EQ(static_cast<FloatObj*>(as_obj(c->head))->value, 1.0);
    auto* c2 = static_cast<ConsObj*>(as_obj(c->tail));
    CHECK_EQ(static_cast<StrObj*>(as_obj(c2->head))->len, 3u);

    // A cycle must not send the copier into a loop.
    Value x = a.make_cons(UNIT, NIL);
    static_cast<ConsObj*>(as_obj(x))->tail = x;
    Value cx = Heap::copy_between(b, x);
    CHECK_EQ(static_cast<ConsObj*>(as_obj(cx))->tail, cx);
}

static void test_image_rejects_bad_input() {
    std::printf("image validation\n");
    std::string err;

    Image img;
    CHECK(!img.load_bytes(reinterpret_cast<const uint8_t*>("short"), 5, err));

    uint8_t header[32] = {};
    std::memcpy(header, "NOTMAGIC", 8);
    Image img2;
    CHECK(!img2.load_bytes(header, sizeof header, err));
    CHECK(err.find("magic") != std::string::npos);

    // Right magic, impossible version.
    uint8_t good[32] = {};
    std::memcpy(good, "DAGNCAAF", 8);
    good[8] = 99;
    Image img3;
    CHECK(!img3.load_bytes(good, sizeof good, err));
}

static void test_atom_interning() {
    std::printf("atom interning\n");
    Runtime rt;
    uint32_t a = rt.intern_atom("hello");
    uint32_t b = rt.intern_atom("hello");
    uint32_t c = rt.intern_atom("world");
    CHECK_EQ(a, b);
    CHECK(a != c);
    CHECK_EQ(rt.atom_name(a), std::string("hello"));

    // The well-known atoms must exist and be distinct.
    const WellKnownAtoms& wk = well_known(rt);
    CHECK(wk.divide_by_zero != wk.type_error);
    CHECK_EQ(rt.atom_name(wk.divide_by_zero), std::string("divide_by_zero"));
}

static void test_builtin_table_matches_compiler() {
    std::printf("builtin table\n");
    // The ids are baked into every image, so this order is a wire format.
    const char* expected[] = {"spawn!",  "join!",   "send!",     "recv!", "self!",
                              "raise!",  "type_of", "to_string", "len",   "strict!",
                              "match_is_cons", "match_head", "match_tail",
                              "match_at", "match_key"};
    const uint32_t n = uint32_t(sizeof(expected) / sizeof(expected[0]));
    CHECK_EQ(builtin_count(), n);
    // Bounded by both, so a table that has grown past this list reports the
    // mismatch above rather than reading off the end of it.
    for (uint32_t i = 0; i < builtin_count() && i < n; ++i) {
        CHECK_EQ(std::string(builtin_def(i).name), std::string(expected[i]));
    }
    // `spawn!` must not force its argument, or spawning would run the work here.
    CHECK_EQ(builtin_def(0).strict_mask, 0u);
}

static void test_maps() {
    std::printf("maps\n");
    Runtime rt;
    auto proc = rt.spawn_process();
    Process& p = *proc;

    // A put answers a new map and leaves the one it was given alone. The
    // caller keeps what comes back -- that is the whole contract, and the
    // reason a map can be shared without being copied.
    Value m = p.heap().make_map(0);
    for (int i = 0; i < 200; ++i) {
        m = map_insert(p, m, make_fixnum(i), make_fixnum(i * 2));
    }
    CHECK_EQ(static_cast<MapObj*>(as_obj(m))->count, 200u);
    for (int i = 0; i < 200; ++i) {
        Value out;
        CHECK(map_lookup(p, m, make_fixnum(i), &out));
        CHECK_EQ(fixnum_value(out), int64_t(i * 2));
    }
    Value missing;
    CHECK(!map_lookup(p, m, make_fixnum(9999), &missing));

    // The map put into is untouched: the older version still answers as it did,
    // and does not see the newer one's entry.
    Value grown = map_insert(p, m, make_fixnum(1000), make_fixnum(7));
    CHECK_EQ(static_cast<MapObj*>(as_obj(m))->count, 200u);
    CHECK_EQ(static_cast<MapObj*>(as_obj(grown))->count, 201u);
    Value peek;
    CHECK(!map_lookup(p, m, make_fixnum(1000), &peek));
    CHECK(map_lookup(p, grown, make_fixnum(1000), &peek));

    // Replacing a key changes the value without changing the size.
    Value replaced = map_insert(p, m, make_fixnum(5), make_fixnum(99));
    CHECK_EQ(static_cast<MapObj*>(as_obj(replaced))->count, 200u);
    CHECK(map_lookup(p, replaced, make_fixnum(5), &peek));
    CHECK_EQ(fixnum_value(peek), int64_t(99));
    CHECK(map_lookup(p, m, make_fixnum(5), &peek));
    CHECK_EQ(fixnum_value(peek), int64_t(10));

    // Removing likewise leaves the original standing.
    Value without = map_erase(p, m, make_fixnum(5));
    CHECK_EQ(static_cast<MapObj*>(as_obj(without))->count, 199u);
    CHECK(!map_lookup(p, without, make_fixnum(5), &peek));
    CHECK(map_lookup(p, m, make_fixnum(5), &peek));
    // Removing something that was never there changes nothing.
    Value same = map_erase(p, m, make_fixnum(9999));
    CHECK_EQ(static_cast<MapObj*>(as_obj(same))->count, 200u);

    // Emptying a map leaves a map, not nothing.
    Value one = map_insert(p, p.heap().make_map(0), make_fixnum(1), UNIT);
    Value none = map_erase(p, one, make_fixnum(1));
    CHECK(is_obj(none, ObjType::Map));
    CHECK_EQ(static_cast<MapObj*>(as_obj(none))->count, 0u);

    // Every entry comes back exactly once, however the trie is shaped.
    std::vector<std::pair<Value, Value>> entries;
    map_collect(m, entries);
    CHECK_EQ(entries.size(), size_t(200));

    // String keys compare by contents, not identity.
    Value m2 = map_insert(p, p.heap().make_map(0), p.heap().make_string("key", 3),
                          make_fixnum(1));
    Value found;
    CHECK(map_lookup(p, m2, p.heap().make_string("key", 3), &found));
    CHECK_EQ(fixnum_value(found), int64_t(1));
}

int main() {
    test_value_tagging();
    test_heap_alloc();
    test_gc_keeps_live_and_drops_dead();
    test_gc_preserves_sharing();
    test_gc_collapses_indirections();
    test_gc_handles_cycles();
    test_heap_verifier_accepts_a_healthy_heap();
    test_heap_verifier_catches_corruption();
    test_heap_verifier_follows_every_object_kind();
    test_cross_heap_copy();
    test_image_rejects_bad_input();
    test_atom_interning();
    test_builtin_table_matches_compiler();
    test_maps();

    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
