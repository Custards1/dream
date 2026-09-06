{
  description = "Dream: a dynamically typed, lazily evaluated functional language with green processes";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    # The overlay is system-independent, so it sits outside eachDefaultSystem.
    # It is what lets a NixOS configuration say `pkgs.dream` after adding this
    # flake, rather than reaching into `inputs.dream.packages.<system>`.
    {
      overlays.default = final: prev: {
        dream-stdlib = final.callPackage ./nix/dream-stdlib.nix { };
        dream-vm = final.callPackage ./nix/dream-vm.nix { };
        dreamc = final.callPackage ./nix/dreamc.nix {
          dreamStdlib = final.dream-stdlib;
        };
        dream = final.callPackage ./nix/dream.nix { };
      };
    }
    // flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs {
          inherit system;
          overlays = [ self.overlays.default ];
        };
      in
      {
        packages = {
          inherit (pkgs) dream dreamc dream-vm dream-stdlib;
          default = pkgs.dream;

          # The interpreter-only build. Worth having as a package rather than
          # only a flag: it is the configuration that proves the VM does not
          # quietly depend on the JIT, and it is the one to reach for on a
          # platform LLVM is awkward on.
          dream-vm-no-jit = pkgs.dream-vm.override { withJit = false; };
        };

        apps = {
          default = flake-utils.lib.mkApp { drv = pkgs.dream; name = "dreamc"; };
          dreamc = flake-utils.lib.mkApp { drv = pkgs.dream; name = "dreamc"; };
          dream = flake-utils.lib.mkApp { drv = pkgs.dream; name = "dream"; };
        };

        checks = {
          inherit (pkgs) dreamc dream-vm;
          no-jit = pkgs.packages.dream-vm-no-jit or (pkgs.dream-vm.override { withJit = false; });

          # The end-to-end suite needs both halves at once, so it cannot live in
          # either derivation's own `checkPhase`. It runs every example program
          # under the interpreter and the JIT and requires identical output.
          e2e = pkgs.runCommand "dream-e2e"
            {
              nativeBuildInputs = [ pkgs.dreamc pkgs.dream-vm pkgs.bash ];
            }
            ''
              cp -r ${self}/dream/tests tests
              chmod -R +w tests
              export DREAMC=${pkgs.dreamc}/bin/dreamc
              export DREAM=${pkgs.dream-vm}/bin/dream
              export DREAM_PACKAGES=${pkgs.dream-stdlib}/lib/dream/packages
              bash tests/e2e.sh
              touch $out
            '';

          # The standard library's own tests, compiled with `--test`.
          stdlib = pkgs.runCommand "dream-stdlib-tests"
            {
              nativeBuildInputs = [ pkgs.dreamc pkgs.dream-vm ];
            }
            ''
              dreamc ${pkgs.dream-stdlib}/lib/dream/packages/std/all.dr --test \
                -L ${pkgs.dream-stdlib}/lib/dream/packages -o tests.dream
              dream tests.dream
              touch $out
            '';
        };

        devShells.default = pkgs.mkShell {
          name = "dream";

          inputsFrom = [ pkgs.dream-vm ];

          nativeBuildInputs = with pkgs; [
            cargo
            rustc
            clippy
            rustfmt
            just
            ninja
            gdb
          ];

          shellHook = ''
            # So `just run` and the tests find the standard library from the
            # working tree rather than from the store: in a dev shell the
            # source you are editing is the one that should win.
            export DREAM_PACKAGES="$PWD/mind''${DREAM_PACKAGES:+:$DREAM_PACKAGES}"
            echo "dream: just --list, or ./build.sh"
          '';
        };
      });
}
