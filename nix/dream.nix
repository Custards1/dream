# The whole toolchain: the VM, the compiler as an image, and the standard
# library the compiler needs to find.
#
# This is what `nix profile install` and `environment.systemPackages` should
# get. Installing `dream-vm` alone gives you a runtime with nothing to run, and
# the compiler alone is an image no one can run -- so the piece that ties them
# together wraps the `dream` binary with the paths that let a bare `dreams`
# resolve to the installed image and let that image find `std.list`.
{ lib, stdenvNoCC, makeWrapper, dreams, lucid, mind, dream-vm, dream-stdlib }:

stdenvNoCC.mkDerivation {
  pname = "dream";
  version = "0.1.0";

  src = null;
  dontConfigure = true;
  dontBuild = true;
  dontUnpack = true;

  nativeBuildInputs = [ makeWrapper ];

  installPhase = ''
    runHook preInstall
    mkdir -p $out/bin $out/lib/dream $out/include
    # `$MINDV2_PATH` is where the VM looks for installed images and where the
    # compiler looks for installed packages, in that order.
    makeWrapper ${dream-vm}/bin/dream $out/bin/dream \
      --suffix MINDV2_PATH : $out/lib/dream:${dream-stdlib}/lib/dream/packages
    ln -s ${dreams}/lib/dream/dreams.dream $out/lib/dream/dreams.dream
    ln -s ${mind}/lib/dream/mind.dream $out/lib/dream/mind.dream
    ln -s ${lucid}/lib/dream/lucid.dream $out/lib/dream/lucid.dream
    ln -s ${dream-stdlib}/lib/dream/packages $out/lib/dream/packages
    ln -s ${dream-vm}/lib/* $out/lib/
    ln -s ${dream-vm}/include/* $out/include/
    runHook postInstall
  '';

  meta = {
    description = "The Dream language: compiler, virtual machine and standard library";
    homepage = "https://github.com/Custards1/dream";
    license = lib.licenses.mit;
    mainProgram = "dream";
    platforms = lib.platforms.unix;
  };
}