{ lib
, stdenvNoCC
, dreamVm   # to run the compiler image
, dreams    # the compiler, an image, built from the seed in its own derivation
}:

stdenvNoCC.mkDerivation {
  pname = "lucid";
  version = "0.1.0";

  src = lib.cleanSourceWith {
    src = ../.;
    # The build output directories change constantly and none of them affect
    # this build, so they are dropped by name -- but only *directories*: a
    # source file whose name begins with `build` (build.sh, mind/tool/build.dr)
    # is part of the tree and must stay.
    filter = path: type:
      let base = baseNameOf path;
      in type != "directory"
         || (!(lib.hasPrefix "build" base)
             && base != "target" && base != ".git" && base != "result");
  };

  nativeBuildInputs = [ dreamVm ];

  # The build writes straight into $out; the stdenv default `make install`
  # would otherwise run the CMake-generated Makefile at the repository root
  # (which wants cmake that is not here).
  dontInstall = true;

  # The language server imports `dreams` as a library, so it is the one
  # program here built by the compiler that is also made of it -- the build
  # takes the whole repository as its library path.
  buildPhase = ''
    runHook preBuild
    mkdir -p $out/lib/dream
    ${dreamVm}/bin/dream ${dreams}/lib/dream/dreams.dream $src/lucid/main.dr \
      -L $src/mind -L $src -o $out/lib/dream/lucid.dream
    runHook postBuild
  '';

  meta = {
    description = "The Dream language server";
    homepage = "https://github.com/Custards1/dream";
    license = lib.licenses.mit;
    platforms = lib.platforms.unix;
  };
}