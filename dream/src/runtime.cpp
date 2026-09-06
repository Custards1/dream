#include "runtime.hpp"

#include "io.hpp"
#include "os.hpp"

#include <algorithm>
#include <cstdio>
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

    register_module(make_console_module());
    register_module(make_math_module());
    register_module(make_core_module());
    register_module(make_vm_module());
    register_module(make_ffi_module());
    register_module(make_io_module());
    register_module(make_net_module());
    register_module(make_os_module());
    // The poller thread has to exist before any process can wait on a
    // descriptor, and it costs nothing when nothing does IO.
    io_init();
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
    intern_image_atoms();
    return true;
}

void Runtime::intern_image_atoms() {
    for (uint32_t i = 0; i < image_->atom_count(); ++i) {
        intern_atom(image_->atom_name(i).str());
    }
}

void Runtime::register_module(ModuleDef module) { modules_.push_back(std::move(module)); }

const ModuleDef* Runtime::find_module(const std::string& name) const {
    for (const auto& m : modules_) {
        if (m.name == name) return &m;
    }
    return nullptr;
}

uint32_t Runtime::intern_atom(const std::string& name) {
    std::lock_guard<std::mutex> g(atoms_mutex_);
    auto it = atom_ids_.find(name);
    if (it != atom_ids_.end()) return it->second;
    uint32_t id = uint32_t(atom_names_.size());
    atom_names_.push_back(name);
    atom_ids_.emplace(name, id);
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
