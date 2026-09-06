# default.nix
let
  pkgs = import <nixpkgs> {};
in
pkgs.callPackage ./hello.nix {}