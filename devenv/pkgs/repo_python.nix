{
  pkgs,
  python ? pkgs.python3,
}: let
  pyPkgs = python.pkgs;

  transliterate = pyPkgs.buildPythonPackage rec {
    pname = "transliterate";
    version = "1.10.2";

    pyproject = true;
    "build-system" = with pyPkgs; [setuptools wheel];

    src = pkgs.fetchPypi {
      inherit pname version;
      hash = "sha256-vGCODUjmh9ucKx1+p8OBr+DRhJytIWCH2OA9jQalfIU=";
    };

    propagatedBuildInputs = with pyPkgs; [six];

    doCheck = false;
  };

  websocketsCompatible = pyPkgs.websockets.overridePythonAttrs (old: rec {
    version = "12.0";

    src = pkgs.fetchPypi {
      pname = old.pname;
      inherit version;
      hash = "sha256-gd+cvLtsJg3h4AfljAEb/r4tr8hDUQewU385PdOMixs=";
    };

    doCheck = false;
  });
in
  python.withPackages (_: [
    # Repo build and test helpers still need the userver generator deps.
    pyPkgs.jinja2
    pyPkgs.pyyaml
    pyPkgs.pydantic
    transliterate

    # Repo-enabled local tests and helper flows.
    pyPkgs.minio
    pyPkgs.playwright
    pyPkgs.py
    pyPkgs.psycopg2
    pyPkgs.pytest
    pyPkgs.requests
    # userver testsuite currently requires websockets < 13.
    websocketsCompatible
    pyPkgs.zstd
  ])
