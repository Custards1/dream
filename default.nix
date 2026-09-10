# For `nix-build`, and for anyone not using flakes.
#
#   nix-build                 the whole toolchain
#   nix-build -A dreams       just the compiler
#   nix-build -A dream-vm     just the VM
#
# The flake is the same set of derivations with a pinned nixpkgs; this entry
# point takes whatever nixpkgs the caller has.
{ pkgs ? import <nixpkgs> { } }:

let
  packages = rec {
    dream-stdlib = pkgs.callPackage ./nix/dream-stdlib.nix { };
    dream-vm = pkgs.callPackage ./nix/dream-vm.nix { };
    dreams = pkgs.callPackage ./nix/dreams.nix { dreamVm = dream-vm; };
    dream = pkgs.callPackage ./nix/dream.nix { inherit dreams dream-vm dream-stdlib; };
  };
in
packages.dream // packages