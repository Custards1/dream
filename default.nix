# For `nix-build`, and for anyone not using flakes.
#
#   nix-build                 the whole toolchain
#   nix-build -A dreamc       just the compiler
#   nix-build -A dream-vm     just the VM
#
# The flake is the same set of derivations with a pinned nixpkgs; this entry
# point takes whatever nixpkgs the caller has.
{ pkgs ? import <nixpkgs> { } }:

let
  packages = rec {
    dream-stdlib = pkgs.callPackage ./nix/dream-stdlib.nix { };
    dream-vm = pkgs.callPackage ./nix/dream-vm.nix { };
    dreamc = pkgs.callPackage ./nix/dreamc.nix { dreamStdlib = dream-stdlib; };
    dream = pkgs.callPackage ./nix/dream.nix { inherit dreamc dream-vm dream-stdlib; };
  };
in
packages.dream // packages
