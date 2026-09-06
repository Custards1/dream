# hello.nix
{ stdenv, fetchFromGitHub }:

stdenv.mkDerivation rec {
  pname = "hello-custom";
  version = "1.0.0";

  # Fetch the source code (e.g., from GitHub)
  src = fetchFromGitHub {
    owner = "Custards1";
    repo = "dream";
    rev = "v0.1";
    sha256 = "sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="; 
    # Tip: Leave the hash blank or wrong initially; Nix will error out and give you the correct hash.
  };

  # Build-time dependencies (e.g., compilers, tools)
  buildInputs = [ cargo cmake llvm ];

  # Custom install phase if your app doesn't use standard `make install`
  installPhase = ''
    mkdir -p $out/bin
    cp hello-binary $out/bin/hello
  '';

  meta = {
    description = "Dream programming language";
    homepage = "https://github.com";
  };
}