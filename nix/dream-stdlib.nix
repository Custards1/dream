# The standard library: Dream source, not a build artefact.
#
# It is installed as data rather than compiled, because `dreamc` compiles whole
# programs -- there is no separate library image to link against, and a module
# is only lowered as part of the program that imports it.
{ lib, stdenvNoCC }:

stdenvNoCC.mkDerivation {
  pname = "dream-stdlib";
  version = "0.1.0";

  src = ../mind;

  dontConfigure = true;
  dontBuild = true;

  # `mind` is a directory of packages, each with its own `mind.toml`, and that
  # directory is what `DREAM_PACKAGES` wants on its path.
  installPhase = ''
    runHook preInstall
    mkdir -p $out/lib/dream/packages
    cp -r . $out/lib/dream/packages/
    runHook postInstall
  '';

  meta = {
    description = "The Dream standard library (std.list, std.array, std.str, std.seq, std.test)";
    homepage = "https://github.com/Custards1/dream";
    license = lib.licenses.mit;
    platforms = lib.platforms.all;
  };
}
