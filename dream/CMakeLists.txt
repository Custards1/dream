# dream, the Dream VM: a lazy graph-reduction interpreter with BEAM-style green
# processes, and an optional LLVM JIT tier.

cmake_minimum_required(VERSION 3.20)
project(dream CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

option(DREAM_ENABLE_JIT "Build the LLVM JIT tier" ON)
option(DREAM_ENABLE_FFI "Build the C foreign function interface" ON)
option(DREAM_BUILD_TESTS "Build the VM test suite" ON)

# ---------------------------------------------------------------------------
# LLVM discovery
#
# The JIT is optional: without LLVM the VM still builds and runs, interpreter
# only. `llvm-config` is preferred over LLVM's CMake package because the
# package's exported targets name transitive dependencies (ZLIB, zstd) that are
# frequently not importable on their own, and a missing one is a hard error
# rather than a graceful "not found". llvm-config just prints flags.
#
# Override either way with -DDREAM_LLVM_CONFIG=<path> or -DLLVM_DIR=<cmakedir>.
# ---------------------------------------------------------------------------
set(DREAM_HAVE_LLVM OFF)

if(DREAM_ENABLE_JIT)
  if(NOT DREAM_LLVM_CONFIG)
    find_program(DREAM_LLVM_CONFIG
      NAMES llvm-config llvm-config-21 llvm-config-20 llvm-config-19 llvm-config-18
            llvm-config-17 llvm-config-16 llvm-config-15)
  endif()

  if(NOT DREAM_LLVM_CONFIG)
    # Nix keeps llvm-config in the -dev output, which is usually not in PATH.
    file(GLOB DREAM_NIX_LLVM_CONFIGS "/nix/store/*llvm-*-dev/bin/llvm-config")
    if(DREAM_NIX_LLVM_CONFIGS)
      list(SORT DREAM_NIX_LLVM_CONFIGS)
      list(REVERSE DREAM_NIX_LLVM_CONFIGS)
      list(GET DREAM_NIX_LLVM_CONFIGS 0 DREAM_LLVM_CONFIG)
    endif()
  endif()

  if(DREAM_LLVM_CONFIG)
    execute_process(COMMAND ${DREAM_LLVM_CONFIG} --version
                    OUTPUT_VARIABLE DREAM_LLVM_VERSION
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(DREAM_LLVM_VERSION VERSION_LESS 15)
      message(WARNING "Dream: llvm-config reports ${DREAM_LLVM_VERSION}; the JIT needs 15 or newer")
    else()
      execute_process(COMMAND ${DREAM_LLVM_CONFIG} --includedir
                      OUTPUT_VARIABLE DREAM_LLVM_INCLUDEDIR
                      OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
      execute_process(COMMAND ${DREAM_LLVM_CONFIG} --libdir
                      OUTPUT_VARIABLE DREAM_LLVM_LIBDIR
                      OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
      execute_process(COMMAND ${DREAM_LLVM_CONFIG} --libs core orcjit native support
                              target analysis passes ipo instcombine scalaropts
                              transformutils
                      OUTPUT_VARIABLE DREAM_LLVM_LIBS_RAW
                      OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
      execute_process(COMMAND ${DREAM_LLVM_CONFIG} --system-libs
                      OUTPUT_VARIABLE DREAM_LLVM_SYSLIBS_RAW
                      OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
      execute_process(COMMAND ${DREAM_LLVM_CONFIG} --has-rtti
                      OUTPUT_VARIABLE DREAM_LLVM_RTTI
                      OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)

      separate_arguments(DREAM_LLVM_LIBS NATIVE_COMMAND "${DREAM_LLVM_LIBS_RAW}")
      separate_arguments(DREAM_LLVM_SYSLIBS NATIVE_COMMAND "${DREAM_LLVM_SYSLIBS_RAW}")
      set(DREAM_HAVE_LLVM ON)
      message(STATUS "Dream: JIT enabled against LLVM ${DREAM_LLVM_VERSION} (${DREAM_LLVM_CONFIG})")
    endif()
  endif()

  if(NOT DREAM_HAVE_LLVM AND LLVM_DIR)
    # Explicit CMake package, for platforms where that is the better route.
    find_package(LLVM CONFIG QUIET)
    if(LLVM_FOUND)
      llvm_map_components_to_libnames(DREAM_LLVM_LIBS core orcjit native support target
                                      analysis passes ipo instcombine scalaropts
                                      transformutils)
      set(DREAM_LLVM_INCLUDEDIR ${LLVM_INCLUDE_DIRS})
      set(DREAM_LLVM_LIBDIR ${LLVM_LIBRARY_DIRS})
      set(DREAM_HAVE_LLVM ON)
      message(STATUS "Dream: JIT enabled against LLVM ${LLVM_PACKAGE_VERSION} (${LLVM_DIR})")
    endif()
  endif()

  if(NOT DREAM_HAVE_LLVM)
    message(WARNING "Dream: LLVM not found; building interpreter-only. "
                    "Pass -DDREAM_LLVM_CONFIG=<path to llvm-config> to enable the JIT.")
  endif()
endif()

# ---------------------------------------------------------------------------
# libffi, for std.ffi
#
# Optional in the same way the JIT is: without it the VM builds and runs, and
# `std.ffi` reports at the call site that this build cannot reach C.
# ---------------------------------------------------------------------------
set(DREAM_HAVE_FFI OFF)
if(DREAM_ENABLE_FFI)
  find_path(DREAM_FFI_INCLUDE ffi.h PATH_SUFFIXES ffi)
  find_library(DREAM_FFI_LIB NAMES ffi)
  if(NOT DREAM_FFI_INCLUDE OR NOT DREAM_FFI_LIB)
    # Nix keeps headers in a separate -dev output that is not on the default
    # search path.
    file(GLOB DREAM_NIX_FFI_INC "/nix/store/*libffi-*-dev/include")
    file(GLOB DREAM_NIX_FFI_LIB "/nix/store/*libffi-*/lib/libffi.so")
    if(DREAM_NIX_FFI_INC AND DREAM_NIX_FFI_LIB)
      list(SORT DREAM_NIX_FFI_INC)
      list(REVERSE DREAM_NIX_FFI_INC)
      list(GET DREAM_NIX_FFI_INC 0 DREAM_FFI_INCLUDE)
      list(SORT DREAM_NIX_FFI_LIB)
      list(REVERSE DREAM_NIX_FFI_LIB)
      list(GET DREAM_NIX_FFI_LIB 0 DREAM_FFI_LIB)
    endif()
  endif()
  if(DREAM_FFI_INCLUDE AND DREAM_FFI_LIB)
    set(DREAM_HAVE_FFI ON)
    message(STATUS "Dream: FFI enabled (${DREAM_FFI_LIB})")
  else()
    message(WARNING "Dream: libffi not found; std.ffi will report that C is unreachable")
  endif()
endif()

# ---------------------------------------------------------------------------
# The VM library
# ---------------------------------------------------------------------------
set(DREAM_SOURCES
  src/value.cpp
  src/image.cpp
  src/heap.cpp
  src/cores.cpp
  src/gc_pool.cpp
  src/process.cpp
  src/interp.cpp
  src/builtins.cpp
  src/ffi.cpp
  src/io.cpp
  src/os.cpp
  src/scheduler.cpp
  src/runtime.cpp
  src/capi.cpp
)

if(DREAM_HAVE_LLVM)
  list(APPEND DREAM_SOURCES src/jit.cpp src/jit_rt.cpp)
else()
  list(APPEND DREAM_SOURCES src/jit_stub.cpp src/jit_rt.cpp)
endif()

add_library(dream SHARED ${DREAM_SOURCES})
set_target_properties(dream PROPERTIES
  OUTPUT_NAME dream
  VERSION 0.1
  SOVERSION 0)

target_include_directories(dream
  PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

find_package(Threads REQUIRED)
target_link_libraries(dream PUBLIC Threads::Threads)
target_link_libraries(dream PRIVATE ${CMAKE_DL_LIBS})

if(DREAM_HAVE_FFI)
  target_compile_definitions(dream PRIVATE DREAM_HAVE_FFI=1)
  target_include_directories(dream SYSTEM PRIVATE ${DREAM_FFI_INCLUDE})
  target_link_libraries(dream PRIVATE ${DREAM_FFI_LIB})
endif()

target_compile_options(dream PRIVATE -Wall -Wextra -Wno-unused-parameter)

# ---------------------------------------------------------------------------
# Codegen for the interpreter
#
# The VM is a shared library, and that costs more than it looks. By default a
# compiler must assume any global function in a `.so` can be interposed at load
# time by a definition from somewhere else, so every cross-TU call inside the
# library goes through the PLT and none of them can be inlined -- and the
# interpreter is nothing but cross-TU calls on its hot path (`force_whnf`,
# `well_known`, `builtin_def`, every `Heap::` constructor). Nothing here is
# meant to be interposed, so say so: `-fno-semantic-interposition` lets the
# calls bind directly and inline.
#
# `-O3` over the `-O2` a RelWithDebInfo build would pick, because the reduction
# loop is exactly the shape -O3's extra inlining and unswitching are for, and
# `just` builds RelWithDebInfo -- so the default build is the one that has to be
# fast.
include(CheckCXXCompilerFlag)
foreach(flag -fno-semantic-interposition -fno-plt)
  string(MAKE_C_IDENTIFIER "HAVE${flag}" flag_var)
  check_cxx_compiler_flag(${flag} ${flag_var})
  if(${flag_var})
    target_compile_options(dream PRIVATE ${flag})
  endif()
endforeach()
target_compile_options(dream PRIVATE
  $<$<CONFIG:RelWithDebInfo>:-O3>
  $<$<CONFIG:Release>:-O3>)

# The inliner's default ceilings are wrong for this interpreter, and knowing
# why is worth more than the numbers.
#
# `step_eval` is one switch over every opcode, so it is enormous -- far past
# `large-function-insns`, after which GCC will not grow a function further.
# Everything it calls is therefore outlined, however small: pushing a
# continuation is a compare and two stores, and it was **6% of a self-compile**
# as an out-of-line call. It is not that the helpers are expensive; it is that
# the one function they serve is too big to be allowed to absorb them, so the
# hot path pays a call per operand read, per continuation push, per operand
# decode. Raising the ceilings is worth about 5% on a self-compile, measured
# against the same source built without them, interleaved, seven rounds.
#
# The one helper that still needed `always_inline` even with these is
# `ContStack::push_back` -- see the note on it in process.hpp.
#
# The `SHELL:` prefix is load-bearing: without it CMake sees five options that
# all begin `--param`, deduplicates them down to one, and passes the other four
# values as bare words -- which the driver reports as unused linker inputs, one
# warning per file per param. Twelve files, four params, forty-eight warnings,
# and the flags not applied.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  foreach(param large-function-growth=400 large-function-insns=10000
                 inline-unit-growth=120 max-inline-insns-auto=120
                 max-inline-insns-single=800)
    target_compile_options(dream PRIVATE "SHELL:--param ${param}")
  endforeach()
endif()

# Train and rebuild in the same build directory. Instrument only the VM;
# ordinary builds have no profiling counters or profile-file dependency.
set(DREAM_PGO "OFF" CACHE STRING "GCC profile-guided optimization: OFF, GENERATE, USE")
set_property(CACHE DREAM_PGO PROPERTY STRINGS OFF GENERATE USE)
set(DREAM_PGO_DIR "${CMAKE_BINARY_DIR}/pgo" CACHE PATH "GCC runtime profile directory")
if(NOT DREAM_PGO STREQUAL "OFF")
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "DREAM_PGO requires GCC")
  endif()
  if(DREAM_PGO STREQUAL "GENERATE")
    target_compile_options(dream PRIVATE "-fprofile-generate=${DREAM_PGO_DIR}" -fprofile-update=atomic)
    target_link_options(dream PRIVATE "-fprofile-generate=${DREAM_PGO_DIR}")
    target_compile_definitions(dream PRIVATE DREAM_PROFILE_GENERATE=1)
  elseif(DREAM_PGO STREQUAL "USE")
    target_compile_options(dream PRIVATE "-fprofile-use=${DREAM_PGO_DIR}" -Werror=missing-profile)
  else()
    message(FATAL_ERROR "DREAM_PGO must be OFF, GENERATE or USE")
  endif()
endif()


if(DREAM_HAVE_LLVM)
  target_compile_definitions(dream PRIVATE DREAM_HAVE_LLVM=1
                             __STDC_CONSTANT_MACROS __STDC_FORMAT_MACROS __STDC_LIMIT_MACROS)
  target_include_directories(dream SYSTEM PRIVATE ${DREAM_LLVM_INCLUDEDIR})
  target_link_directories(dream PRIVATE ${DREAM_LLVM_LIBDIR})
  target_link_libraries(dream PRIVATE ${DREAM_LLVM_LIBS} ${DREAM_LLVM_SYSLIBS})
  # LLVM is normally built without RTTI; matching it avoids undefined vtable
  # references when we inherit from or dynamic_cast its types.
  if(NOT DREAM_LLVM_RTTI STREQUAL "YES")
    set_source_files_properties(src/jit.cpp PROPERTIES COMPILE_OPTIONS "-fno-rtti")
  endif()
endif()

# ---------------------------------------------------------------------------
# The `dream` executable
# ---------------------------------------------------------------------------
add_executable(dream_cli src/cli.cpp)
set_target_properties(dream_cli PROPERTIES OUTPUT_NAME dream)
target_link_libraries(dream_cli PRIVATE dream)
target_include_directories(dream_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(DREAM_BUILD_TESTS)
  enable_testing()
  add_executable(dream_tests tests/test_main.cpp)
  target_link_libraries(dream_tests PRIVATE dream)
  target_include_directories(dream_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME dream_tests COMMAND dream_tests)

  # End-to-end tests need the compiler, so they are skipped when it is absent
  # rather than failing a VM-only build.
  add_test(NAME dream_e2e
           COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e.sh)
  set_tests_properties(dream_e2e PROPERTIES
    ENVIRONMENT "dream=$<TARGET_FILE:dream_cli>"
    SKIP_RETURN_CODE 1)

  # A malformed image must be rejected, never crashed on. Kept short here; run
  # `just fuzz 5000` for a real sweep.
  add_test(NAME dream_fuzz_image
           COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz_image.sh)
  set_tests_properties(dream_fuzz_image PROPERTIES
    ENVIRONMENT "dream=$<TARGET_FILE:dream_cli>;ITERATIONS=120"
    SKIP_RETURN_CODE 1
    TIMEOUT 600)

  # The same programs with the heap checked after every collection.
  add_test(NAME dream_e2e_verified_heap
           COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e.sh)
  set_tests_properties(dream_e2e_verified_heap PROPERTIES
    ENVIRONMENT "dream=$<TARGET_FILE:dream_cli>;DREAM_VERIFY_HEAP=1"
    SKIP_RETURN_CODE 1)
endif()

# A worked example of embedding the VM from C. Building it keeps the public
# header honest: it must compile as C, not just as C++.
add_executable(dream_embed_example examples/embed.c)
target_link_libraries(dream_embed_example PRIVATE dream)
set_target_properties(dream_embed_example PROPERTIES OUTPUT_NAME embed C_STANDARD 11)


include(GNUInstallDirs)



install(TARGETS dream dream_cli
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
