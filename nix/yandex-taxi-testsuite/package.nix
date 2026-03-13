{
  pkgs,
  pgmigrateSrc,
  yandexTaxiTestsuiteSrc,
}: let
  pyPkgs = pkgs.python3Packages;
  pgmigratePkg = import ../pgmigrate/package.nix {
    inherit pgmigrateSrc pkgs;
  };
in
  pyPkgs.buildPythonPackage {
    pname = "yandex-taxi-testsuite";
    version = "0.4.5";

    src = yandexTaxiTestsuiteSrc;

    pyproject = true;
    "build-system" = with pyPkgs; [setuptools wheel];

    postPatch = ''
      substituteInPlace setup.py \
        --replace "    setup_requires=['pytest-runner']," ""
    '';

    propagatedBuildInputs =
      (with pyPkgs; [
        packaging
        pyyaml
        aiohttp
        yarl
        py
        pytest-aiohttp
        pytest-asyncio
        pytest
        python-dateutil
        cached-property
        psycopg2
      ])
      ++ [pgmigratePkg];

    doCheck = false;
  }
