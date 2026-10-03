# libffi with its static archive, for a VM that does not need libffi.so at run
# time. nixpkgs builds libffi shared-only, and the archive has to be position-
# independent as well, because it is linked into libdream.so rather than into
# an executable -- autotools builds static objects without -fPIC unless told.
# DREAM_LINK_DEPS in dream/CMakeLists.txt is the other half.
{ libffi }:

libffi.overrideAttrs (old: {
  dontDisableStatic = true;
  configureFlags = (old.configureFlags or [ ]) ++ [ "--with-pic" ];
})
