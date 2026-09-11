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
        dreams = final.callPackage ./nix/dreams.nix {
          dreamVm = final.dream-vm;
        };
        mind = final.callPackage ./nix/mind.nix {
          dreamVm = final.dream-vm;
          dreams = final.dreams;
        };
        lucid = final.callPackage ./nix/lucid.nix {
          dreamVm = final.dream-vm;
          dreams = final.dreams;
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
          inherit (pkgs) dream dreams mind lucid dream-vm dream-stdlib;
          default = pkgs.dream;

          # The interpreter-only build. Worth having as a package rather than
          # only a flag: it is the configuration that proves the VM does not
          # quietly depend on the JIT, and it is the one to reach for on a
          # platform LLVM is awkward on.
          dream-vm-no-jit = pkgs.dream-vm.override { withJit = false; };
        };

        apps = {
          default = flake-utils.lib.mkApp { drv = pkgs.dream; name = "dream"; };
          dream = flake-utils.lib.mkApp { drv = pkgs.dream; name = "dream"; };
        };

        checks = {
          inherit (pkgs) dreams mind lucid dream-vm;
          no-jit = pkgs.packages.dream-vm-no-jit or (pkgs.dream-vm.override { withJit = false; });

          # The end-to-end suite needs both halves at once, so it cannot live in
          # either derivation's own `checkPhase`. It runs every example program
          # under the interpreter and the JIT and requires identical output.
          e2e = pkgs.runCommand "dream-e2e"
            {
              nativeBuildInputs = [ pkgs.dreams pkgs.dream-vm pkgs.bash ];
            }
            ''
              cp -r ${self}/dream/tests tests
              chmod -R +w tests
              export MINDV2_PATH=${pkgs.dreams}/lib/dream:${pkgs.dream-stdlib}/lib/dream/packages
              export DREAMS=${pkgs.dreams}/lib/dream/dreams.dream
              export DREAM=${pkgs.dream-vm}/bin/dream
              bash tests/e2e.sh
              touch $out
            '';

          # The standard library's own tests, compiled with `--test`.
          stdlib = pkgs.runCommand "dream-stdlib-tests"
            {
              nativeBuildInputs = [ pkgs.dreams pkgs.dream-vm pkgs.bash ];
            }
            ''
              export DREAM=${pkgs.dream-vm}/bin/dream
              export MINDV2_PATH=${pkgs.dreams}/lib/dream:${pkgs.dream-stdlib}/lib/dream/packages
              $DREAM ${pkgs.dreams}/lib/dream/dreams.dream \
                ${pkgs.dream-stdlib}/lib/dream/packages/std/all.dr --test \
                -L ${pkgs.dream-stdlib}/lib/dream/packages -o tests.dream
              $DREAM tests.dream
              touch $out
            '';
        };

        devShells.default = pkgs.mkShell {
          name = "dream";

          inputsFrom = [ pkgs.dream-vm ];

          nativeBuildInputs = with pkgs; [
            just
            ninja
            gdb
          ];

          shellHook = ''
            echo "dream: just --list, or ./build.sh"
          '';
        };
      });
}