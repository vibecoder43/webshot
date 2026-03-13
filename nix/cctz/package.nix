{pkgs}: let
  cctzSrc = pkgs.fetchFromGitHub {
    owner = "google";
    repo = "cctz";
    rev = "d2f2abda066d74c6e110b6be959d50bfb365917a";
    hash = "sha256-YCE0DXuOT5tCOfLlemMH7I2F8c7HEK1NEUJvtfqnCg8=";
  };
in
  pkgs.stdenv.mkDerivation {
    pname = "cctz";
    version = "unstable-d2f2abda";

    src = cctzSrc;

    nativeBuildInputs = [
      pkgs.cmake
    ];

    cmakeFlags = [
      "-DBUILD_TESTING=OFF"
      "-DBUILD_EXAMPLES=OFF"
      "-DBUILD_BENCHMARK=OFF"
      "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
    ];

    doCheck = false;

    meta = with pkgs.lib; {
      description = "C++ library for Civil Time and Time Zone (CCTZ)";
      homepage = "https://github.com/google/cctz";
      license = licenses.asl20;
      platforms = platforms.unix;
    };
  }
