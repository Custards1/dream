# Dream: a lazily evaluated functional language.
#
#   dream/   the VM (dream), in C++ -- interpreter, green processes, LLVM JIT
#
# The compiler is `dreams`, written in Dream and built from the checked-in seed,
# so `cmake --build .` builds the VM only; `just dreams` builds the compiler
# once the VM exists.

cmake_minimum_required(VERSION 3.20)
project(dream LANGUAGES CXX)

# Register subdirectory tests when configuring from the repository root.
include(CTest)
add_subdirectory(dream)

set(CPACK_PACKAGE_NAME "dream")
set(CPACK_PACKAGE_VERSION "0.1.0")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Dream language virtual machine")
if(WIN32)
  set(CPACK_GENERATOR "ZIP")
else()
  set(CPACK_GENERATOR "TGZ")
endif()
include(CPack)
