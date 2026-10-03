# The virtual machine: C++, built by CMake.
#
# libffi is required: `std.ffi` is part of every VM, and one that cannot call C
# is not a valid build. So is OpenSSL, for `std.tls`. LLVM is optional. It is on by default here because
# without it you lose the JIT tier -- but `withJit = false` is a real
# configuration the VM supports, and the tests run under it.
#
# The compiler is `dreams`, built from the seed in its own derivation; the VM's
# build does not see it.
{ lib
, stdenv
, cmake
, pkg-config
, llvmPackages_21
, libffi
, openssl
, zlib
, withJit ? true
, doCheck ? true
}:

let
  # Linked into libdream, so the VM this builds needs neither libffi.so nor
  # libLLVM.so at run time: see DREAM_LINK_DEPS in dream/CMakeLists.txt.
  libffiStatic = import ./libffi-static.nix { inherit libffi; };
in
stdenv.mkDerivation {
  pname = "dream-vm";
  version = "0.1.0";

  src = lib.cleanSourceWith {
    src = ../.;
    # The build output directories change constantly and none of them affect
    # this build, so they are dropped by name -- but only *directories*: a
    # source file whose name begins with `build` (build.sh, mind/tool/build.dr)
    # is part of the tree and must stay. And only at the root, which is where
    # `build`, `build-dream` and `build-pgo` are: `mind/std/build` is the
    # `std.build` package, and dropping every `build*` directory wherever it
    # was dropped that too, so nothing that imports it would compile.
    filter = path: type:
      let base = baseNameOf path;
          atRoot = dirOf path == toString ../.;
      in type != "directory"
         || (!(atRoot && lib.hasPrefix "build" base)
             && base != "target" && base != ".git" && base != "result");
  };

  nativeBuildInputs = [ cmake pkg-config ]
    ++ lib.optional withJit llvmPackages_21.llvm.dev;

  buildInputs = [ libffiStatic openssl ]
    ++ lib.optional withJit zlib;   # LLVM links against it

  cmakeFlags = [
    "-DDREAM_ENABLE_JIT=${if withJit then "ON" else "OFF"}"
    "-DCMAKE_BUILD_TYPE=RelWithDebInfo"
    # STATIC rather than the default PREFER_STATIC: a package that quietly
    # fell back on the shared libraries would be the build this exists to avoid.
    "-DDREAM_LINK_DEPS=STATIC"
    "-DDREAM_FFI_LIB=${libffiStatic.out}/lib/libffi.a"
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
    description = "The Dream virtual machine";
    longDescription = ''
      A lazy graph-reduction interpreter/compiler written as an explicit state machine,
      Installs the `dream` binary, `libdream`, and the C embedding headers.
    '';
    homepage = "https://github.com/Custards1/dream";
    license = lib.licenses.mit;
    mainProgram = "dream";
    platforms = lib.platforms.unix;
  };
}
