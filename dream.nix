# dream.nix — builds the full Dream toolchain: dreamc (Rust) + dream VM (C++/LLVM).
#
# Called by default.nix and the flake.  Not meant to be used directly.
#
# Arguments:
#   lib, stdenv, cmake, ninja, pkg-config   — standard
#   rustPlatform                            — for buildRustPackage
#   llvmPackages                            — pass llvmPackages_21 (or whichever)
#   libffi, zlib                            — runtime deps for the VM
#   just                                    — optional, only needed for the dev shell
{
  lib,
  stdenv,
  cmake,
  ninja,
  pkg-config,
  rustPlatform,
  llvmPackages,
  libffi,
  zlib,
}:

let
  # --------------------------------------------------------------------------
  # dreamc — the compiler (Rust)
  # --------------------------------------------------------------------------
  dreamc = rustPlatform.buildRustPackage {
    pname = "dreamc";
    version = "0.1.0";

    src = lib.cleanSourceWith {
      src = ./.;
      filter = path: _type:
        let rel = lib.removePrefix (toString ./. + "/") path;
        in lib.hasPrefix "dreamc/" rel
        || rel == "Cargo.toml"
        || rel == "Cargo.lock";
    };

    cargoLock = {
      lockFile = ./Cargo.lock;
    };

    # Only build/install the binary, not the library crate.
    cargoBuildFlags = [ "--bin" "dreamc" ];

    meta = {
      description = "The Dream language compiler — source to .dream bytecode images";
      mainProgram = "dreamc";
    };
  };

  # --------------------------------------------------------------------------
  # dream VM — the runtime (C++, optional LLVM JIT)
  # --------------------------------------------------------------------------
  dream-vm = stdenv.mkDerivation {
    pname = "dream";
    version = "0.1.0";

    src = lib.cleanSourceWith {
      src = ./.;
      filter = path: _type:
        let rel = lib.removePrefix (toString ./. + "/") path;
        in lib.hasPrefix "dream/" rel
        || rel == "CMakeLists.txt";
    };

    nativeBuildInputs = [
      cmake
      ninja
      pkg-config
      llvmPackages.llvm.dev   # provides llvm-config
    ];

    buildInputs = [
      llvmPackages.llvm
      libffi
      zlib
    ];

    cmakeFlags = [
      "-DCMAKE_BUILD_TYPE=Release"
      "-DDREAM_BUILD_COMPILER=OFF"   # dreamc is built separately above
      "-DDREAM_BUILD_TESTS=OFF"      # tests require the compiler at build time
      "-DDREAM_ENABLE_JIT=ON"
      "-DDREAM_ENABLE_FFI=ON"
      # Point cmake straight at the llvm-config we have, so the Nix-store glob
      # fallback in CMakeLists.txt is never needed.
      "-DDREAM_LLVM_CONFIG=${llvmPackages.llvm.dev}/bin/llvm-config"
    ];

    meta = {
      description = "The Dream language VM — interpreter and LLVM JIT";
      mainProgram = "dream";
    };
  };

in
# The top-level package merges both outputs into one bin/.
# dreamc goes in $out/bin/dreamc, dream in $out/bin/dream,
# and the VM's shared library + headers land in lib/ and include/.
stdenv.mkDerivation {
  pname = "dream-toolchain";
  version = "0.1.0";

  # No source to compile here — just assemble the two derivations.
  dontUnpack = true;
  dontBuild = true;

  installPhase = ''
    runHook preInstall
    mkdir -p $out/bin $out/lib $out/include

    # Compiler
    cp ${dreamc}/bin/dreamc $out/bin/dreamc

    # VM binary and shared library
    cp ${dream-vm}/bin/dream     $out/bin/dream
    cp -r ${dream-vm}/lib/.      $out/lib/
    cp -r ${dream-vm}/include/.  $out/include/
    runHook postInstall
  '';

  meta = {
    description = "Dream programming language — compiler and VM";
    homepage    = "https://github.com/Custards1/dream";
    license     = lib.licenses.mit;
    platforms   = lib.platforms.linux;
    mainProgram = "dream";
  };
}
