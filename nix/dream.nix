# The whole toolchain: the compiler, the VM, and the standard library the
# compiler needs to find.
#
# This is what `nix profile install` and `environment.systemPackages` should
# get. Installing `dreamc` alone gives you a compiler that cannot resolve
# `import std.list`, and installing `dream-vm` alone gives you a runtime with
# nothing to run.
{ lib, symlinkJoin, makeWrapper, dreamc, dream-vm, dream-stdlib }:

symlinkJoin {
  name = "dream-0.1.0";
  paths = [ dreamc dream-vm dream-stdlib ];

  nativeBuildInputs = [ makeWrapper ];

  # `dreamc` arrives already wrapped by its own derivation, and symlinkJoin
  # copies that wrapper through, so the standard library is on the path here
  # too. Nothing further is needed -- the VM reads images, not source, and so
  # has no package path of its own.
  meta = {
    description = "The Dream language: compiler, virtual machine and standard library";
    homepage = "https://github.com/Custards1/dream";
    license = lib.licenses.mit;
    mainProgram = "dreamc";
    platforms = lib.platforms.unix;
  };
}
