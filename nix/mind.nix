{ lib
, stdenvNoCC
, dreamVm   # to run the compiler image
, dreams    # the compiler, an image, built from the seed in its own derivation
}:

stdenvNoCC.mkDerivation {
  pname = "mind";
  version = "0.1.0";

  src = lib.cleanSourceWith {
    src = ../.;
    filter = path: type:
      let base = baseNameOf path;
      in !(lib.hasPrefix "build" base)
         && base != "target"
         && base != ".git"
         && base != "result";
  };

  nativeBuildInputs = [ dreamVm ];

  # The build writes straight into $out; the stdenv default `make install`
  # would otherwise run the CMake-generated Makefile at the repository root
  # (which wants cmake that is not here).
  dontInstall = true;

  # The build tool is a Dream program, compiled with `--shebang` so the
  # artifact carries a `#!` line and the execute bit. It is installed under
  # its image name in `$MINDV2_PATH`; the VM skips the shebang line when it
  # loads it, which is exactly how `dream mind` picks it up by name.
  buildPhase = ''
    runHook preBuild
    mkdir -p $out/lib/dream
    ${dreamVm}/bin/dream ${dreams}/lib/dream/dreams.dream -L $src/mind/std \
      $src/mind/tool/main.dr --shebang -o $out/lib/dream/mind.dream
    runHook postBuild
  '';

  meta = {
    description = "The Dream build tool";
    homepage = "https://github.com/Custards1/dream";
    license = lib.licenses.mit;
    platforms = lib.platforms.unix;
  };
}