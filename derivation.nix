
# https://unix.stackexchange.com/a/717169
{ stdenv }:
stdenv.mkDerivation rec {
  name = "helo-${version}";
  version = "0.1.0";
  src = ./.;
  nativeBuildInputs = [ ];
  buildInputs = [ ];

  buildPhase = ''
    gcc server.c string_replace.c -lm -o helo
  '';

  installPhase = ''
    mkdir -p $out/bin
    cp helo $out/bin
  '';
}
