# default.nix — entry point for nix-build.
#
#   nix-build           builds the full Dream toolchain (compiler + VM)
{ pkgs ? import <nixpkgs> {} }:

pkgs.callPackage ./dream.nix {
  inherit (pkgs) lib stdenv cmake ninja pkg-config libffi zlib rustPlatform;
  llvmPackages = pkgs.llvmPackages_21;
}
