# Platform and image portability

Dream targets native 64-bit Linux, Windows, and macOS. A compiled `.dream`
image is portable bytecode: copy the **same file**, without recompiling or
rewriting it, to a compatible Dream VM on any of these systems. This includes
the compiler image, `dreams.dream`.

The on-disk format uses fixed-width little-endian integers, IEEE 754 binary64
floats, and indices rather than host pointers. The current mapped reader
requires a 64-bit little-endian host (including x86-64 and ARM64). The VM checks
these host properties at build time. LLVM machine code is generated locally
at runtime and is not part of the portable image. The bytecode format and VM
version must be compatible; portability is not a promise of compatibility
with arbitrary future format versions.

Ordinary language semantics, binary strings, concurrency, and standard file,
process, and network operations belong to the portable interface. Native OS
calls live in the VM. Windows paths and arguments are converted between
Dream's UTF-8 and Windows' UTF-16, and binary file I/O does not translate CRLF
or treat byte 26 as end of file. Windows sockets are kept distinct from CRT
file descriptors. Linux uses epoll; macOS and Windows use a readiness poller.
The portable poller checks readiness every 5 ms and is a correctness baseline,
not a claim of identical throughput or latency to epoll.

Platform-specific work remains platform-specific:

- FFI library names, exported symbols, and C ABIs require matching libraries
  on the receiving system. An optional feature such as FFI must be enabled in
  that VM when a program uses it.
- Executable names, shell commands, environment variables, permissions, and
  filesystem case sensitivity depend on the host. `mind` dependency fetching
  requires the external programs it invokes, such as Git, curl, and tar.
- A shebang is optional metadata for Unix launchers. Use `dream program.dream`
  on every OS; Windows does not execute a `.dream` file as a native executable.
- `os.platform ()` reports the **running** VM's OS. Use it for runtime selection
  when one image needs several implementations. `when os == "..."` selects
  code at **compile time** and therefore intentionally specializes an image.
- Windows `os.replace!` launches a child with the terminal attached, waits,
  and exits with its status. Windows has no POSIX process-image replacement,
  so the PID is not preserved.

## Building

Use a C++20 compiler and CMake 3.20 or newer. GCC, Clang, and MSVC are supported
by the build configuration. Start with the interpreter to avoid optional
LLVM and libffi dependencies:

```text
cmake -S . -B build -DDREAM_ENABLE_JIT=OFF -DDREAM_ENABLE_FFI=OFF
cmake --build build --config Release --parallel
ctest --test-dir build -C Release -R "^dream_tests$" --output-on-failure
```

Single-configuration builds put the VM in `build/bin/dream` (or `dream.exe`).
Visual Studio puts it in `build/bin/Release/dream.exe`. Keep the Windows VM DLL
beside the executable. Enable LLVM and libffi with `DREAM_ENABLE_JIT` and
`DREAM_ENABLE_FFI`; missing dependencies produce an interpreter or a VM without
FFI, respectively. On Windows, use libraries built for the same compiler ABI.

## Verification

`.github/workflows/portable.yml` compiles a bundle on Linux and sends those exact
artifacts to Linux, Windows, and macOS runners. Each runner verifies SHA-256
hashes before execution. The bundle covers language operations, binary I/O,
Unicode filenames, subprocess quoting, deadlines, TCP I/O with one worker, and
using the same compiler image to compile a new program on the receiving OS.
The initial matrix exercises the interpreter; optional JIT/FFI configurations
need separate validation on each host.

To reproduce locally:

```text
python3 dream/tests/portable.py compile --vm build/bin/dream --images build/images
python3 dream/tests/portable.py run --vm build/bin/dream --images build/images
```

Use `python` and the appropriate `.exe` path on Windows. Copy `build/images`
from the compiling machine to test another OS; do not regenerate it there.
`DREAM_PORTABLE_POLLER=ON` also exercises the non-epoll readiness backend on
Linux. Cross-compilation verifies Windows compilation/linking, but native CI
results remain necessary to establish platform support.
