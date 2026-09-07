// Builtins, host modules, and the map primitives shared with the interpreter.

#pragma once

#include <cstdint>
#include <utility>
#include <vector>
#include <string>

#include "runtime.hpp"
#include "value.hpp"

namespace dream {

class Process;

struct BuiltinDef {
    const char* name;
    uint32_t arity;
    /// Bit i set means argument i is forced before the call. `spawn!` clears
    /// bit 0 deliberately: forcing the thunk would run the new process's work
    /// on the caller's stack, which is exactly what spawning must not do.
    uint32_t strict_mask;
    NativeFn fn;
};

const BuiltinDef& builtin_def(uint32_t id);
uint32_t builtin_count();

/// Insert into an open-addressed map, growing it when it gets crowded.
/// `map` with `key` set to `value`. The map handed in is unchanged and stays
/// valid: what comes back shares all of it but the path to the new entry.
Value map_insert(Process& p, Value map, Value key, Value value);
/// `map` without `key`, on the same terms.
Value map_erase(Process& p, Value map, Value key);
/// Every entry of a map, in the trie's order.
void map_collect(Value node, std::vector<std::pair<Value, Value>>& out);
/// Look up a key. Returns false when it is absent.
bool map_lookup(Process& p, Value map, Value key, Value* out);

/// Modules this runtime provides to `import`.
ModuleDef make_console_module();
ModuleDef make_math_module();
ModuleDef make_core_module();
ModuleDef make_vm_module();
ModuleDef make_ffi_module();

/// True when this build can call C.
bool ffi_available();

}  // namespace dream
