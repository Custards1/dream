# The compiler: Rust, built by Cargo.
#
# It is a package of its own rather than a phase of the VM's build because the
# two have nothing in common at build time -- and because the VM's CMake build
# drives Cargo itself when asked to, which cannot work inside the sandbox
# (Cargo would want the network). `-DDREAM_BUILD_COMPILER=OFF` in the VM's
# derivation is the other half of that arrangement.
{ lib
, rustPlatform
, dreamStdlib   # the `mind` tree, so an installed compiler finds `std.*`
, makeWrapper
}:

rustPlatform.buildRustPackage {
  pname = "dreamc";
  version = "0.1.0";

  src = lib.cleanSourceWith {
    src = ../.;
    # The C++ half, build outputs and editor droppings change constantly and
    # none of them affect this build; excluding them keeps the store path
    # stable across VM edits.
    filter = path: type:
      let base = baseNameOf path;
      in !(lib.hasPrefix "build" base)
         && base != "target"
         && base != ".git"
         && base != "result";
  };

  cargoLock.lockFile = ../Cargo.lock;

  nativeBuildInputs = [ makeWrapper ];

  # `cargo test` needs the whole workspace, and the compiler's own tests are
  # hermetic -- they write to a temporary directory and clean up after
  # themselves -- so they run here rather than only in `nix flake check`.
  doCheck = true;

  postInstall = ''
    # Without this an installed `dreamc` cannot find `std.list`: packages are
    # searched relative to the file being compiled, and nothing about a store
    # path says where the standard library went. `--suffix` rather than
    # `--set`, so a caller can still point at their own copy first.
    wrapProgram $out/bin/dreamc \
      --suffix DREAM_PACKAGES : ${dreamStdlib}/lib/dream/packages
  '';

  meta = {
    description = "Compiler for Dream, a lazily evaluated functional language";
    longDescription = ''
      Compiles `.dr` source to a `.dream` bytecode image: a flat arena of
      16-byte execution-tree nodes that the VM maps and starts reducing without
      rebuilding a tree.
    '';
    homepage = "https://github.com/Custards1/dream";
    license = lib.licenses.mit;
    mainProgram = "dreamc";
    platforms = lib.platforms.unix;
  };
}
