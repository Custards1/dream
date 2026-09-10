# Dream: a lazily evaluated functional language.
#
#   dream/   the VM (dream), in C++ -- interpreter, green processes, LLVM JIT
#
# The compiler is `dreams`, written in Dream and built from the checked-in seed,
# so `cmake --build .` builds the VM only; `just dreams` builds the compiler
# once the VM exists.

cmake_minimum_required(VERSION 3.20)
project(dream LANGUAGES CXX)

add_subdirectory(dream)

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