# A development environment with everything build.sh looks for.
#
#   nix-shell        then ./build.sh
#
# libffi and OpenSSL are required (`std.ffi` and `std.tls` are part of every
# VM). LLVM is optional; it is here because with it you get the JIT, and a
# build without it is the unusual case rather than the default.
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
    libffi                     # std.ffi, required
    openssl                    # std.tls, required
    zlib                       # LLVM links against it
    sqlite                     # std.sql.sqlite, loaded when a program opens a database
  ];

  shellHook = ''
    echo "dream: run ./build.sh, or just --list"
    # So `just run` and the tests find the standard library without setup.
    export DREAM_PACKAGES="$PWD/mind''${DREAM_PACKAGES:+:$DREAM_PACKAGES}"
    # std.sql.sqlite opens libsqlite3 the way the platform's loader finds
    # one, and on Nix that is only by being told where it is.
    export LD_LIBRARY_PATH="${pkgs.sqlite.out}/lib''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  '';
}
