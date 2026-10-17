# For `nix-build`, and for anyone not using flakes.
#
#   nix-build                 the whole toolchain
#   nix-build -A dreams       just the compiler
#   nix-build -A dream-vm     just the VM
#   nix-build -A mind         the build tool
#   nix-build -A lucid        the language server
#
# The components are git submodules, so a checkout needs them first
# (`git submodule update --init`, or `just subs`).
#
# The flake is the same set of derivations with a pinned nixpkgs; this entry
# point takes whatever nixpkgs the caller has.
{ pkgs ? import <nixpkgs> { } }:

let
  packages = rec {
    dream-stdlib = pkgs.callPackage ./nix/dream-stdlib.nix { };
    dream-vm = pkgs.callPackage ./nix/dream-vm.nix { };
    dreams = pkgs.callPackage ./nix/dreams.nix { dreamVm = dream-vm; };
    mind = pkgs.callPackage ./nix/mind.nix { dreamVm = dream-vm; inherit dreams; };
    lucid = pkgs.callPackage ./nix/lucid.nix { dreamVm = dream-vm; inherit dreams; };
    dream = pkgs.callPackage ./nix/dream.nix { inherit dreams mind lucid dream-vm dream-stdlib; };
  };
in
packages.dream // packages