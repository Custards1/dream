# A development environment with everything build.sh looks for.
#
#   nix-shell        then ./build.sh
#
# LLVM and libffi are both optional to the build; they are here because with
# them you get the JIT and `std.ffi`, and reproducing a build without them is
# the unusual case rather than the default.
{ pkgs ? import <nixpkgs> { } }:

pkgs.mkShell {
  name = "dream";

  nativeBuildInputs = with pkgs; [
    cmake
    ninja
    pkg-config
    cargo
    rustc
    clang
    llvmPackages_21.llvm.dev   # llvm-config, for the JIT
    just
  ];

  buildInputs = with pkgs; [
    libffi                     # std.ffi
    zlib                       # LLVM links against it
  ];

  shellHook = ''
    echo "dream: run ./build.sh, or just --list"
    # So `just run` and the tests find the standard library without setup.
    export DREAM_PACKAGES="$PWD/mind''${DREAM_PACKAGES:+:$DREAM_PACKAGES}"
  '';
}
