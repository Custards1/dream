# What `sleep` needs from the system, beyond the VM:
#
#   nix-shell sleep/shell.nix --run 'mind run -- serve'
#
# SQLite, which `std.sql.sqlite` loads when the store is opened, and an
# OpenCL loader, through which `std.tensor` finds the GPU's own driver (the
# vendor's ICD, which NixOS keeps under /run/opengl-driver). Without a GPU,
# or without this shell, everything still runs: training and staging stay on
# the host.
{ pkgs ? import <nixpkgs> { } }:

pkgs.mkShell {
  name = "sleep";
  buildInputs = with pkgs; [ sqlite ocl-icd ];
  shellHook = ''
    export DREAM_LIBRARY_PATH=${pkgs.sqlite.out}/lib''${DREAM_LIBRARY_PATH:+:$DREAM_LIBRARY_PATH}
    export DREAM_OPENCL_LIB=${pkgs.ocl-icd}/lib/libOpenCL.so.1
    if [ -d /run/opengl-driver/etc/OpenCL/vendors ]; then
      export OCL_ICD_VENDORS=/run/opengl-driver/etc/OpenCL/vendors
    fi
  '';
}
