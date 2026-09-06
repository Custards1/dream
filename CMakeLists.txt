# Dream: a lazily evaluated functional language.
#
#   dreamc/  the compiler (dreamc), in Rust -- source to `.dream` images
#   dream/  the VM (mindv2), in C++      -- interpreter, green processes, LLVM JIT
#
# The compiler is built by Cargo. This drives it as a custom target so that
# `cmake --build .` produces both halves, and so the VM's end-to-end tests have
# a compiler to call. Configure with -DDREAM_BUILD_COMPILER=OFF to skip it.

cmake_minimum_required(VERSION 3.20)
project(dream LANGUAGES CXX)

option(DREAM_BUILD_COMPILER "Build the dreamc compiler with Cargo" ON)

add_subdirectory(dream)

if(DREAM_BUILD_COMPILER)
  find_program(DREAM_CARGO cargo)
  if(DREAM_CARGO)
    set(DREAM_CARGO_PROFILE $<IF:$<CONFIG:Debug>,dev,release>)
    set(DREAM_CARGO_DIR $<IF:$<CONFIG:Debug>,debug,release>)
    add_custom_target(dreamc ALL
      COMMAND ${DREAM_CARGO} build --profile ${DREAM_CARGO_PROFILE}
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/dreamc
      COMMENT "Building the dreamc compiler"
      USES_TERMINAL)
    # The VM's e2e tests shell out to the compiler, so make the ordering explicit.
    if(TARGET mindv2_cli)
      add_dependencies(mindv2_cli dreamc)
    endif()
  else()
    message(WARNING "Dream: cargo not found; the compiler will not be built. "
                    "The VM still builds, but it has nothing to compile for it.")
  endif()
endif()
# 3. Configure CPack for Debian generation
set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "my-awesome-app")
set(CPACK_PACKAGE_VENDOR "My Company or Name")
set(CPACK_PACKAGE_CONTACT "maintainer@example.com")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A brief summary of my awesome application")

# Optional: Automatically calculate runtime dependencies (like libc6, etc.)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)

# 4. Include CPack (this must be at the very end)
include(CPack)