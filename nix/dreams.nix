# The compiler: `dreams`, written in Dream, built from the checked-in seed.
#
# It is a package of its own rather than a phase of the VM's build because the
# two have nothing in common at build time. The seed is an image and needs
# nothing but the VM to compile this source into the next image; the stage
# that comes out is still an image, installed as data -- running it is handing
# it to `dream`, which is what the wrapping toolchain derivation arranges.
{ lib
, stdenvNoCC
, dreamVm   # to run the seed; the build never touches a native compiler
}:

stdenvNoCC.mkDerivation {
  pname = "dreams";
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

  # The compiler must still reproduce itself from this source: the image the
  # seed builds and the image that image builds must be byte-identical. That
  # equality is what says the compiler in the tree and the compiler in the
  # store agree.
  doCheck = true;

  buildPhase = ''
    runHook preBuild
    mkdir -p $out/lib/dream
    dream $src/dreams/bootstrap/dreams.dream -L $src/mind -L $src \
      -o $out/lib/dream/dreams.dream $src/dreams/main.dr
    runHook postBuild
  '';

  checkPhase = ''
    runHook preCheck
    dream $out/lib/dream/dreams.dream -L $src/mind -L $src \
      -o $TMPDIR/stage3.dream $src/dreams/main.dr
    cmp $out/lib/dream/dreams.dream $TMPDIR/stage3.dream
    runHook postCheck
  '';

  meta = {
    description = "Compiler for Dream, a lazily evaluated functional language";
    longDescription = ''
      Compiles `.dr` source to a `.dream` bytecode image: a flat arena of
      16-byte execution-tree nodes that the VM maps and starts reducing without
      rebuilding a tree. Written in Dream and built from the checked-in seed.
    '';
    homepage = "https://github.com/Custards1/dream";
    license = lib.licenses.mit;
    platforms = lib.platforms.unix;
  };
}