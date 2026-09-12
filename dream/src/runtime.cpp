#include "runtime.hpp"

#include "io.hpp"
#include "os.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "builtins.hpp"
#include "process.hpp"
#include "scheduler.hpp"

namespace dream {

Runtime::Runtime() : wk_(std::make_unique<WellKnownAtoms>()) {
    // Intern the atoms the runtime itself names, before any image atoms, so
    // their indices are stable no matter what is loaded.
    wk_->error = intern_atom("error");
    wk_->divide_by_zero = intern_atom("divide_by_zero");
    wk_->type_error = intern_atom("type_error");
    wk_->not_a_function = intern_atom("not_a_function");
    wk_->no_such_member = intern_atom("no_such_member");
    wk_->loop = intern_atom("loop");
    wk_->stack_overflow = intern_atom("stack_overflow");
    wk_->out_of_memory = intern_atom("out_of_memory");
    wk_->killed = intern_atom("killed");
    wk_->normal = intern_atom("normal");
    wk_->timeout = intern_atom("timeout");
    wk_->ok = intern_atom("ok");
    wk_->out_of_bounds = intern_atom("out_of_bounds");
    wk_->no_such_key = intern_atom("no_such_key");
    // The names `type_of` answers with, in `dream_type` order. Interned once
    // here rather than at every call: a fold that asks the type of its
    // accumulator asks once per element.
    static const char* const kTypeNames[] = {
        "integer", "float",  "char",   "bool",   "unit",    "string",
        "atom",    "list",   "array",  "map",    "pure_fn", "impure_fn",
        "module",  "error",  "process", "unknown", "bigstr",
    };
    static_assert(std::size(kTypeNames) == DREAM_TYPE_BIGSTR + 1,
                  "every surface type needs a name");
    for (size_t i = 0; i < std::size(kTypeNames); ++i) {
        wk_->types[i] = intern_atom(kTypeNames[i]);
    }

    register_module(make_console_module());
    register_module(make_math_module());
    // `std.core` is Dream source now (mind/std/core.dr), and what it cannot
    // say for itself it reaches through `std.native`. The host keeps answering
    // to the old name as well: every image built before the move imports the
    // module by it, the bootstrap seed among them, and so does a program
    // compiled where no standard library can be found.
    {
        ModuleDef core = make_core_module();
        register_module(core);
        core.name = "std.native";
        register_module(std::move(core));
    }
    register_module(make_vm_module());
    register_module(make_ffi_module());
    register_module(make_io_module());
    register_module(make_net_module());
    register_module(make_os_module());
    // The poller thread has to exist before any process can wait on a
    // descriptor, and it costs nothing when nothing does IO.
    io_init();
}

void Runtime::enable_profile(size_t top) {
    profiling_ = true;
    profile_top_ = top;
    // Sized from the image, so a function index is a slot and no lookup is
    // needed on the hot path. An image loaded after this call is not profiled,
    // which cannot happen: the image is loaded before anything runs.
    std::vector<std::atomic<uint64_t>> fresh(image_ ? image_->func_count() : 0);
    profile_.swap(fresh);
}

void Runtime::print_profile() const {
    const size_t top = profile_top_;
    if (!profiling_ || !image_) return;
    std::vector<std::pair<uint64_t, uint32_t>> rows;
    rows.reserve(profile_.size());
    uint64_t total = 0;
    for (uint32_t i = 0; i < profile_.size(); ++i) {
        uint64_t n = profile_[i].load(std::memory_order_relaxed);
        total += n;
        if (n) rows.push_back({n, i});
    }
    std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::fprintf(stderr, "; profile: %llu reductions in %zu functions\n",
                 static_cast<unsigned long long>(total), rows.size());
    for (size_t i = 0; i < rows.size() && i < top; ++i) {
        StringRef name = image_->str(image_->func(rows[i].second).name);
        double pct = total ? 100.0 * double(rows[i].first) / double(total) : 0.0;
        std::fprintf(stderr, ";  %5.1f%%  %12llu  %.*s\n", pct,
                     static_cast<unsigned long long>(rows[i].first),
                     int(name.len), name.data);
    }
}

void Runtime::print_stats() const {
    if (!stats_) return;
    uint64_t reductions = scheduler_ ? scheduler_->total_reductions() : 0;
    uint64_t major = 0, minor = 0, promoted = 0, allocated = 0;
    uint64_t minor_ns = 0, major_ns = 0, rounds = 0;
    size_t peak = 0;
    // Every process, not just the root: a program that does its work in
    // children -- which is what green processes are for -- would otherwise
    // report a heap that never did anything.
    //
    // A counter belongs to the worker running its process, and this may be
    // called from `os.exit!` while other workers are still running theirs, so
    // a number here can be one collection out of date. That is the same
    // bargain `std.vm.process_info!` makes for the same reason: a measurement
    // is worth having slightly stale, and worth nothing if taking it needs an
    // atomic on the allocation path.
    for (const auto& p : all_processes()) {
        const Heap& h = p->heap();
        major += h.major_collections();
        minor += h.minor_collections();
        promoted += h.bytes_promoted();
        minor_ns += h.nanos_minor();
        major_ns += h.nanos_major();
        rounds += h.parallel_rounds();
        allocated += h.bytes_total();
        peak += h.bytes_peak();
    }
    std::fprintf(stderr,
                 "; %llu reductions, %llu major + %llu minor collections\n"
                 "; %llu bytes allocated, %llu promoted, %zu live at each heap's peak\n"
                 "; %.0f ms stopped in collection (%.0f major, %.0f minor),"
                 " %llu rounds divided across threads\n",
                 static_cast<unsigned long long>(reductions),
                 static_cast<unsigned long long>(major),
                 static_cast<unsigned long long>(minor),
                 static_cast<unsigned long long>(allocated),
                 static_cast<unsigned long long>(promoted), peak,
                 double(major_ns + minor_ns) / 1e6, double(major_ns) / 1e6,
                 double(minor_ns) / 1e6, static_cast<unsigned long long>(rounds));
}

Runtime::~Runtime() {
    // Stop the poller before the scheduler it wakes into can go away, and
    // close whatever descriptors the program left open.
    io_shutdown();
    os_shutdown();
}

bool Runtime::load_image_file(const std::string& path, std::string& error) {
    auto img = std::make_unique<Image>();
    if (!img->load_file(path, error)) return false;
    image_ = std::move(img);
    size_caches();
    intern_image_atoms();

    // Best-effort: keep the source around so runtime errors can quote a line.
    std::ifstream in(image_->source_name().str());
    if (in) {
        std::ostringstream ss;
        ss << in.rdbuf();
        source_text_ = ss.str();
    }
    return true;
}

bool Runtime::load_image_bytes(const uint8_t* data, size_t size, std::string& error) {
    auto img = std::make_unique<Image>();
    if (!img->load_bytes(data, size, error)) return false;
    image_ = std::move(img);
    size_caches();
    intern_image_atoms();
    return true;
}

/// The lookup caches are sized from the image, so a cache slot is an index and
/// the fast path has nothing to search. Both are cleared by being rebuilt: an
/// image replaced at run time is not something the runtime supports, but a
/// second `load_image_*` on a fresh Runtime must not inherit a stale table.
void Runtime::size_caches() {
    std::vector<std::atomic<const ModuleDef*>> imports(image_ ? image_->import_count() : 0);
    import_defs_.swap(imports);
    std::vector<std::atomic<uint64_t>> fields(image_ ? image_->node_count() : 0);
    field_cache_.swap(fields);
}

/// Intern every atom the image names, and remember the answer per index.
///
/// The map is the point: after this, evaluating a `:name` constant is one
/// array read. Doing it the other way -- interning at each use -- meant a
/// `std::string`, a mutex and a hash on a path that runs once per reduction.
void Runtime::intern_image_atoms() {
    const uint32_t n = image_->atom_count();
    image_atom_ids_.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        StringRef name = image_->atom_name(i);
        image_atom_ids_[i] = intern_atom(std::string_view(name.data, name.len));
    }
}

void Runtime::register_module(ModuleDef module) {
    // Number this module's members into the runtime-wide sequence, so a
    // process can cache the function value for each in a flat array.
    module.member_base = native_member_count_;
    native_member_count_ += uint32_t(module.members.size());
    modules_.push_back(std::move(module));
}

const ModuleDef* Runtime::module_for_import(uint32_t import_index) {
    if (import_index < import_defs_.size()) {
        const ModuleDef* hit = import_defs_[import_index].load(std::memory_order_relaxed);
        if (hit) return hit;
    }
    if (!image_ || import_index >= image_->import_count()) return nullptr;
    StringRef path = image_->str(image_->import(import_index).path);
    const ModuleDef* found = nullptr;
    for (const auto& m : modules_) {
        if (m.name.size() == path.len && std::memcmp(m.name.data(), path.data, path.len) == 0) {
            found = &m;
            break;
        }
    }
    if (found && import_index < import_defs_.size()) {
        import_defs_[import_index].store(found, std::memory_order_relaxed);
    }
    return found;
}

const ModuleDef* Runtime::find_module(const std::string& name) const {
    for (const auto& m : modules_) {
        if (m.name == name) return &m;
    }
    return nullptr;
}

uint32_t Runtime::intern_atom(std::string_view name) {
    std::lock_guard<std::mutex> g(atoms_mutex_);
    // Transparent lookup: no `std::string` is built unless the name is new.
    auto it = atom_ids_.find(name);
    if (it != atom_ids_.end()) return it->second;
    uint32_t id = uint32_t(atom_names_.size());
    atom_names_.emplace_back(name);
    atom_ids_.emplace(atom_names_.back(), id);
    return id;
}

const std::string& Runtime::atom_name(uint32_t index) const {
    static const std::string unknown = "<unknown-atom>";
    std::lock_guard<std::mutex> g(atoms_mutex_);
    if (index >= atom_names_.size()) return unknown;
    return atom_names_[index];
}

uint32_t Runtime::atom_count() const {
    std::lock_guard<std::mutex> g(atoms_mutex_);
    return uint32_t(atom_names_.size());
}

std::shared_ptr<Process> Runtime::spawn_process() {
    std::unique_lock<std::shared_mutex> g(processes_mutex_);
    uint64_t id = next_pid_++;
    auto p = std::make_shared<Process>(*this, id);
    processes_.emplace(id, p);
    return p;
}

std::shared_ptr<Process> Runtime::find_process(uint64_t id) const {
    std::shared_lock<std::shared_mutex> g(processes_mutex_);
    auto it = processes_.find(id);
    return it == processes_.end() ? nullptr : it->second;
}

void Runtime::retire_process(uint64_t id) {
    std::unique_lock<std::shared_mutex> g(processes_mutex_);
    processes_.erase(id);
}

size_t Runtime::live_process_count() const {
    std::shared_lock<std::shared_mutex> g(processes_mutex_);
    size_t n = 0;
    for (const auto& [id, p] : processes_) {
        if (!p->is_done()) ++n;
    }
    return n;
}

std::vector<std::shared_ptr<Process>> Runtime::all_processes() const {
    std::shared_lock<std::shared_mutex> g(processes_mutex_);
    std::vector<std::shared_ptr<Process>> out;
    out.reserve(processes_.size());
    for (auto& [id, p] : processes_) out.push_back(p);
    std::sort(out.begin(), out.end(),
              [](const std::shared_ptr<Process>& a, const std::shared_ptr<Process>& b) {
                  return a->id() < b->id();
              });
    return out;
}

const WellKnownAtoms& well_known(Runtime& rt) { return rt.well_known_atoms(); }

}  // namespace dream
