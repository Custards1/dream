// Process-independent state: the loaded image, the module registry, and the
// process table. One Runtime serves every scheduler thread, so everything here
// is either immutable after load or explicitly synchronized.

#pragma once

#include <cstdint>
#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "image.hpp"
#include "value.hpp"

namespace dream {

class Process;
class Scheduler;

/// What a native call did. The machine, not the native, owns evaluation order,
/// so a native that needs a forced argument asks for one and is re-invoked.
enum class NativeOutcome : uint8_t {
    Value,       // `value` is the result
    Raise,       // `value` is an error to raise
    Block,       // the process parked itself; re-enter when it is woken
    Enter,       // `value` is the result, not yet forced; the machine forces it
};

struct NativeResult {
    NativeOutcome outcome = NativeOutcome::Value;
    Value value = UNIT;

    static NativeResult ok(Value v) { return {NativeOutcome::Value, v}; }
    static NativeResult raise(Value v) { return {NativeOutcome::Raise, v}; }
    static NativeResult block() { return {NativeOutcome::Block, UNIT}; }
    /// The result, unforced, for the machine to force as the continuation of
    /// the call. A native that hands back something it found -- a list's head,
    /// a map's value -- answers this rather than forcing it. Forcing inside a
    /// native runs a nested machine loop on the C++ stack, and a value whose own
    /// evaluation reads through another such native nests again, as deep as the
    /// chain of reads is long; entered by the machine, that depth is heap.
    static NativeResult enter(Value v) { return {NativeOutcome::Enter, v}; }
};

/// A host function. `callee` is the value being applied: for a module member
/// or an FFI binding it is the `NativeObj`, whose `user` field carries whatever
/// the host attached. Builtins are immediates and pass themselves.
using NativeFn = NativeResult (*)(Process& p, Value callee, Value* args, uint32_t argc);

/// An `arity` of this means "however many arguments the call site passed".
///
/// A curried language cannot decide on its own when a variadic call is
/// saturated -- `f a b` and `f a b c` differ only in how many arguments the
/// application node carries -- so a variadic native consumes exactly the
/// arguments of the application that reached it, and is never partially
/// applied. Every argument is forced, since `strict_mask` has no bit to spare
/// for an unbounded list.
constexpr uint32_t NATIVE_VARIADIC = 0xFFFFFFFFu;

struct NativeDef {
    const char* name;
    /// Number of arguments, or `NATIVE_VARIADIC`.
    uint32_t arity;
    uint32_t strict_mask;  // bit i: force argument i before calling
    NativeFn fn;
    /// Attached to the function value, and readable by `fn` from its own
    /// object. Lets one implementation back many members.
    uint64_t user = 0;
};

/// A module the host provides, such as `std.console`.
struct ModuleDef {
    std::string name;
    std::vector<NativeDef> members;
    /// Where this module's members start in the runtime-wide numbering of
    /// host members. Assigned by `register_module`, so `member_base + i` is a
    /// small dense id for one member of one module -- which is what lets a
    /// process cache the function value it hands out for it in a flat array
    /// rather than rebuilding it at every call. See `Process::native_cache`.
    uint32_t member_base = 0;
    const NativeDef* find(const StringRef& member) const;
};

class Runtime {
public:
    Runtime();
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    bool load_image_file(const std::string& path, std::string& error);
    bool load_image_bytes(const uint8_t* data, size_t size, std::string& error);
    const Image& image() const { return *image_; }
    bool has_image() const { return image_ != nullptr; }

    /// Register a module the program can `import`. Must happen before running.
    void register_module(ModuleDef module);
    const ModuleDef* find_module(const std::string& name) const;

    /// The host module an import record names, remembered after the first ask.
    ///
    /// Every `core.head xs` in a program goes through here, and the honest
    /// lookup -- copy the path out of the image into a `std::string`, then walk
    /// the registered modules comparing names -- allocated and scanned on every
    /// single call. Modules cannot be registered once the program is running,
    /// so the answer never changes, and one relaxed word per import record is
    /// the whole cache. A racing pair of threads computes the same pointer.
    const ModuleDef* module_for_import(uint32_t import_index);

    /// Which member of that module a `.field` node named, likewise remembered.
    /// The import index is stored with it, so a node that somehow sees a
    /// different module falls back to the search rather than reading the wrong
    /// member.
    uint64_t field_cache(uint32_t node_index) const {
        return node_index < field_cache_.size()
                   ? field_cache_[node_index].load(std::memory_order_relaxed)
                   : 0;
    }
    void set_field_cache(uint32_t node_index, uint64_t packed) {
        if (node_index < field_cache_.size()) {
            field_cache_[node_index].store(packed, std::memory_order_relaxed);
        }
    }
    const std::vector<ModuleDef>& modules() const { return modules_; }

    /// Atom names are global and stable, so they can be compared by index.
    /// Atoms from the image are interned first, then any created at run time.
    uint32_t intern_atom(std::string_view name);
    const std::string& atom_name(uint32_t index) const;
    uint32_t atom_count() const;

    /// The runtime id of image atom `i`.
    ///
    /// Every `:ok` a program evaluates lands here, so what this must not do is
    /// what it used to: copy the name out of the image into a `std::string`,
    /// take the atom table's mutex, and hash it. Image atoms are all interned
    /// once at load, and the image is immutable, so the answer is decided
    /// before anything runs and this is a single array read. On a fold whose
    /// body mentions one atom that was forty million string constructions.
    uint32_t image_atom(uint32_t i) const { return image_atom_ids_[i]; }

    /// How many host module members exist across every registered module: the
    /// size a process's native cache needs to be.
    uint32_t native_member_count() const { return native_member_count_; }

    // --- process table ---
    std::shared_ptr<Process> spawn_process();
    std::shared_ptr<Process> find_process(uint64_t id) const;
    void retire_process(uint64_t id);
    size_t live_process_count() const;
    /// Every process the runtime still knows about, newest last. Used by
    /// `std.vm` to report what the system is doing.
    std::vector<std::shared_ptr<Process>> all_processes() const;

    /// The arguments the program was run with, not counting the VM's own
    /// options or the image path. `std.os.args!` hands these to Dream.
    const std::vector<std::string>& program_args() const { return program_args_; }
    void set_program_args(std::vector<std::string> args) { program_args_ = std::move(args); }

    Scheduler* scheduler() { return scheduler_; }
    void set_scheduler(Scheduler* s) { scheduler_ = s; }

    // --- the profile ---
    //
    // Where a program's time goes, counted in reductions and attributed to the
    // function whose frame was current. Off unless asked for: the attribution
    // costs two loads per reduction, which is small but not nothing, and a
    // measurement that changes what it measures is worth switching off.
    //
    // The counters are relaxed atomics because every worker thread reduces at
    // once and nothing here needs an order -- a profile is a shape, and a lost
    // increment does not change one.
    void enable_profile(size_t top);
    bool profiling() const { return profiling_; }
    void note_reduction(uint32_t func_index) {
        if (func_index < profile_.size()) {
            profile_[func_index].fetch_add(1, std::memory_order_relaxed);
        }
    }
    /// The hottest functions, most reductions first, on stderr. A no-op unless
    /// profiling was asked for, so the places that have to call it -- the end
    /// of a run, and `exit!`, which never returns to it -- can call it blind.
    void print_profile() const;

    // --- what the run cost ---
    //
    // Reductions and collections, on stderr, for `--stats`. It lives here
    // rather than in the CLI for the same reason the profile does: a program
    // that ends with `os.exit!` -- which is every tool, `dreams` included --
    // never comes back to `main`, so anything only `main` printed was never
    // printed for the runs anyone actually measures.
    void enable_stats() { stats_ = true; }
    bool stats_enabled() const { return stats_; }
    /// A no-op unless `--stats` was asked for.
    void print_stats() const;

    /// The JIT tier, or null when running interpreter-only.
    class Jit* jit() const { return jit_; }
    void set_jit(class Jit* j) { jit_ = j; }

    /// Diagnostics: the source text, if it can still be read, so runtime
    /// errors can point at a line rather than a node index.
    const std::string& source_text() const { return source_text_; }

    const struct WellKnownAtoms& well_known_atoms() const { return *wk_; }

private:
    void intern_image_atoms();
    void size_caches();

    std::unique_ptr<Image> image_;
    std::string source_text_;

    std::vector<ModuleDef> modules_;

    mutable std::mutex atoms_mutex_;
    std::vector<std::string> atom_names_;
    /// Transparent hashing, so a lookup by `string_view` does not have to
    /// build a `std::string` it is going to throw away.
    struct SvHash {
        using is_transparent = void;
        size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
    };
    std::unordered_map<std::string, uint32_t, SvHash, std::equal_to<>> atom_ids_;
    /// Image atom index -> runtime atom id, filled at load. See `image_atom`.
    std::vector<uint32_t> image_atom_ids_;
    uint32_t native_member_count_ = 0;

    mutable std::shared_mutex processes_mutex_;
    std::unordered_map<uint64_t, std::shared_ptr<Process>> processes_;
    uint64_t next_pid_ = 1;

    std::vector<std::string> program_args_;
    Scheduler* scheduler_ = nullptr;
    std::vector<std::atomic<const ModuleDef*>> import_defs_;
    std::vector<std::atomic<uint64_t>> field_cache_;
    bool profiling_ = false;
    bool stats_ = false;
    size_t profile_top_ = 0;
    std::vector<std::atomic<uint64_t>> profile_;
    class Jit* jit_ = nullptr;
    std::unique_ptr<struct WellKnownAtoms> wk_;
};

/// Well-known atoms, interned at startup so the runtime can name them cheaply.
///
/// Anything a hot path would otherwise name with a C string literal belongs
/// here: `intern_atom` takes the atom table's mutex and hashes the name, which
/// is the wrong price to pay per reduction. `type_of` alone asked for one
/// twenty million times in a ten-million-element fold.
struct WellKnownAtoms {
    uint32_t error;
    uint32_t divide_by_zero;
    uint32_t type_error;
    uint32_t not_a_function;
    uint32_t no_such_member;
    uint32_t loop;
    uint32_t stack_overflow;
    uint32_t out_of_memory;
    uint32_t killed;
    uint32_t normal;
    uint32_t timeout;
    uint32_t ok;
    uint32_t out_of_bounds;
    uint32_t no_such_key;
    /// The name `type_of` answers for each surface type, indexed by
    /// `dream_type`. `DREAM_TYPE_PURE_FN` is the pure one; an impure function
    /// reports `impure_fn`, which is the entry after it.
    uint32_t types[DREAM_TYPE_BIGSTR + 1];
};
const WellKnownAtoms& well_known(Runtime& rt);

}  // namespace dream
