// Built when LLVM is unavailable. The VM runs interpreter-only; every entry
// point answers "not compiled", which is a legal answer for a tiered runtime.

#include "jit.hpp"

namespace dream {

struct Jit::Impl {};

bool Jit::available() { return false; }
Jit::Jit(Runtime&) : impl_(nullptr) {}
Jit::~Jit() = default;
CompiledFn Jit::on_enter(uint32_t) { return nullptr; }
CompiledFn Jit::compile_locked(uint32_t, std::string* error) {
    if (error) *error = "this build has no JIT: LLVM was not found at configure time";
    return nullptr;
}
CompiledFn Jit::compile(uint32_t, std::string* error) {
    if (error) *error = "this build has no JIT: LLVM was not found at configure time";
    return nullptr;
}
void Jit::set_threshold(uint32_t) {}
uint32_t Jit::threshold() const { return 0; }
uint64_t Jit::compiled_count() const { return 0; }
std::string Jit::dump_ir(uint32_t) { return "<no JIT in this build>"; }

}  // namespace dream
