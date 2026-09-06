# CMake generated Testfile for 
# Source directory: /home/blake/dawn/dawn/dawn
# Build directory: /home/blake/dawn/dawn/dawn
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[dawn_tests]=] "/home/blake/dawn/dawn/bin/dawn_tests")
set_tests_properties([=[dawn_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/blake/dawn/dawn/dawn/CMakeLists.txt;170;add_test;/home/blake/dawn/dawn/dawn/CMakeLists.txt;0;")
add_test([=[dawn_e2e]=] "/home/blake/dawn/dawn/dawn/tests/e2e.sh")
set_tests_properties([=[dawn_e2e]=] PROPERTIES  ENVIRONMENT "DAWN=/home/blake/dawn/dawn/bin/dawn" SKIP_RETURN_CODE "1" _BACKTRACE_TRIPLES "/home/blake/dawn/dawn/dawn/CMakeLists.txt;174;add_test;/home/blake/dawn/dawn/dawn/CMakeLists.txt;0;")
