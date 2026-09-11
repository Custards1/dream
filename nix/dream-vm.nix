# The virtual machine: C++, built by CMake.
#
# LLVM and libffi are both optional to the build. They are on by default here
# because without them you lose the JIT tier and `std.ffi`, and a build without
# them is the unusual case rather than the expected one -- but `withJit = false`
# is a real configuration the VM supports, and the tests run under it.
#
# The compiler is `dreams`, built from the seed in its own derivation; the VM's
# build does not see it.
{ lib
, stdenv
, cmake
, pkg-config
, llvmPackages_21
, libffi
, zlib
, withJit ? true
, withFfi ? true
, doCheck ? true
}:

stdenv.mkDerivation {
  pname = "dream-vm";
  version = "0.1.0";

  src = lib.cleanSourceWith {
    src = ../.;
    # The build output directories change constantly and none of them affect
    # this build, so they are dropped by name -- but only *directories*: a
    # source file whose name begins with `build` (build.sh, mind/tool/build.dr)
    # is part of the tree and must stay.
    filter = path: type:
      let base = baseNameOf path;
      in type != "directory"
         || (!(lib.hasPrefix "build" base)
             && base != "target" && base != ".git" && base != "result");
  };

  nativeBuildInputs = [ cmake pkg-config ]
    ++ lib.optional withJit llvmPackages_21.llvm.dev;

  buildInputs = lib.optional withFfi libffi
    ++ lib.optional withJit zlib;   # LLVM links against it

  cmakeFlags = [
    "-DDREAM_ENABLE_JIT=${if withJit then "ON" else "OFF"}"
    "-DDREAM_ENABLE_FFI=${if withFfi then "ON" else "OFF"}"
    "-DCMAKE_BUILD_TYPE=RelWithDebInfo"
  ];

  inherit doCheck;

  # The VM's own tests: values, the collector, cross-heap copying, image
  # validation, maps. They need no compiler, which is why they can run here
  # while the end-to-end tests -- which do -- run in `nix flake check`.
  checkPhase = ''
    runHook preCheck
    ./bin/dream_tests
    runHook postCheck
  '';

  meta = {
    description = "The Dream virtual machine: lazy graph reduction, green processes, an LLVM JIT";
    longDescription = ''
      A lazy graph-reduction interpreter written as an explicit state machine,
      BEAM-style green processes with isolated heaps and copied messages, and
      an optional LLVM JIT for the strict numeric spine of hot functions.
      Installs the `dream` binary, `libdream`, and the C embedding headers.
    '';
    homepage = "https://github.com/Custards1/dream";
    license = lib.licenses.mit;
    mainProgram = "dream";
    platforms = lib.platforms.unix;
  };
}
