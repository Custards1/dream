{ lib, stdenv, cmake, llvm, libffi }:

stdenv.mkDerivation {
  pname = "dream";
  version = "0.1.0";

  # Points to your local source code directory
  src = ./.; 

  nativeBuildInputs = [ cmake ];
  buildInputs = [ llvm libffi ]; # Put library dependencies here if needed
}