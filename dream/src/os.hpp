// The operating system: arguments, environment, directories, subprocesses.
//
// See os.cpp for why `exec!` uses a helper thread rather than the IO poller.
#pragma once

#include "runtime.hpp"

namespace dream {

/// Wait for any subprocess still running. Called when the runtime goes away.
void os_shutdown();

ModuleDef make_os_module();

}  // namespace dream
