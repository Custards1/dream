{
  description = "Dream — a lazily evaluated functional language";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };

        # The LLVM version used across all packages and the dev shell.
        llvmPkgs = pkgs.llvmPackages_21;

        callArgs = {
          inherit (pkgs) lib stdenv cmake ninja pkg-config libffi zlib rustPlatform;
          llvmPackages = llvmPkgs;
        };

        dreamToolchain = pkgs.callPackage ./dream.nix callArgs;

      in {
        # ------------------------------------------------------------------ #
        # Packages                                                            #
        # ------------------------------------------------------------------ #
        packages = {
          default  = dreamToolchain;
          dream    = dreamToolchain;
        };

        # ------------------------------------------------------------------ #
        # Apps  (nix run .# -- <args>)                                       #
        # ------------------------------------------------------------------ #
        apps = {
          default = {
            type    = "app";
            program = "${dreamToolchain}/bin/dream";
          };
          dreamc = {
            type    = "app";
            program = "${dreamToolchain}/bin/dreamc";
          };
        };

        # ------------------------------------------------------------------ #
        # Dev shell  (nix develop)                                           #
        # ------------------------------------------------------------------ #
        devShells.default = pkgs.mkShell {
          name = "dream";

          nativeBuildInputs = with pkgs; [
            cmake
            ninja
            pkg-config
            cargo
            rustc
            clang
            llvmPkgs.llvm.dev     # llvm-config, for the JIT
            just
          ];

          buildInputs = with pkgs; [
            libffi
            zlib
          ];

          shellHook = ''
            echo "dream dev shell — run 'just' to see available recipes"
            export DREAM_PACKAGES="$PWD/mind''${DREAM_PACKAGES:+:$DREAM_PACKAGES}"
          '';
        };

        # ------------------------------------------------------------------ #
        # Checks  (nix flake check)                                          #
        # ------------------------------------------------------------------ #
        checks = {
          build = dreamToolchain;
        };
      }
    );
}
