{
  pkgs,
  config,
  inputs,
  ...
}: let
  pkgsWithOverlay = pkgs.extend (import ./nix/overlays/boost-stacktrace-backtrace.nix);

  lib = pkgsWithOverlay.lib;
  buildDeps = import ./nix/common_deps.nix {pkgs = pkgsWithOverlay;};
  toolchain = import ./nix/toolchain.nix {pkgs = pkgsWithOverlay;};
  llvm21 = pkgsWithOverlay.llvmPackages_21;
  system = pkgsWithOverlay.stdenv.system;

  python = pkgsWithOverlay.python3;
  chaoticPython = python.withPackages (ps: [
    ps.jinja2
    ps.pyyaml
    ps.pydantic
    ps.psycopg2
    ps.websockets
    ps.minio
  ]);

  userverDeps = import ./nix/userver/deps.nix {
    pkgs = pkgsWithOverlay;
    inherit chaoticPython;
  };

  # Extra libs needed by C++ tests; used only in a dedicated wrapper,
  # not exported globally into the dev shell.
  testLibs = userverDeps ++ [pkgsWithOverlay.stdenv.cc.cc];

  webshotTestSan = pkgsWithOverlay.writeShellScriptBin "webshot-test-san" ''
    set -euo pipefail
    export LD_LIBRARY_PATH='${lib.makeLibraryPath testLibs}'
    cd /tmp/build-webshot-san
    ctest --output-on-failure
  '';

  userverPkgs = inputs.userver.packages.${system};
  uniAlgoPkgs = inputs."uni-algo".packages.${system};
  yttsPkgs = inputs."yandex-taxi-testsuite".packages.${system};
  buildDirs = {
    san = "/tmp/build-webshot-san";
    tidy = "/tmp/build-webshot-tidy";
    cov = "/tmp/build-webshot-cov";
    release = "/tmp/build-webshot-release";
  };

  cmakeBaseFlags = [
    "-S ."
    "-G Ninja"
    "-D CMAKE_CXX_COMPILER=clang++"
    "-D CMAKE_C_COMPILER_LAUNCHER=ccache"
    "-D CMAKE_CXX_COMPILER_LAUNCHER=ccache"
    "-D WEBSHOT_CLANG_FORMAT=clang-format"
    "-D USERVER_PYTHON_PATH=$USERVER_PYTHON_PATH"
    "-D WEBSHOT_USE_MOLD=ON"
    "-D WEBSHOT_ENABLE_LLVM_SYMBOLIZER=ON"
    "-D WEBSHOT_ENABLE_SQL_COVERAGE=OFF"
    "-D userver_DIR=$USERVER_DIR"
    "-D USERVER_FEATURE_TESTSUITE=ON"
    "-D USERVER_TESTSUITE_USE_VENV=OFF"
    "-D USERVER_SQL_USE_VENV=OFF"
    "-D USERVER_CHAOTIC_USE_VENV=OFF"
    "-D TESTSUITE_PYTHON_BINARY=$USERVER_PYTHON_PATH"
    "-Wno-dev"
  ];

  sanFlags = [
    "-D CMAKE_CXX_CLANG_TIDY="
    "-D CMAKE_BUILD_TYPE=Debug"
    "-D CMAKE_EXPORT_COMPILE_COMMANDS=ON"
    "-D USE_SANITIZERS=ON"
    "-D BUILD_TESTING=ON"
    "-D WEBSHOT_LLVM_SYMBOLIZER_BIN=$WEBSHOT_LLVM_SYMBOLIZER_BIN"
  ];

  tidyFlags =
    sanFlags
    ++ [
      "-D CMAKE_CXX_CLANG_TIDY=clang-tidy"
    ];

  covFlags =
    sanFlags
    ++ [
      "-D WEBSHOT_ENABLE_COVERAGE=ON"
      "-D WEBSHOT_LLVM_COV_BIN=llvm-cov"
      "-D WEBSHOT_LLVM_PROFDATA_BIN=llvm-profdata"
    ];

  releaseFlags = [
    "-D CMAKE_CXX_CLANG_TIDY="
    "-D CMAKE_BUILD_TYPE=Release"
    "-D CMAKE_EXPORT_COMPILE_COMMANDS=ON"
    "-D USE_SANITIZERS=OFF"
    "-D BUILD_TESTING=OFF"
  ];

  mkConfigureTask = buildDir: extraFlags: {
    cwd = config.git.root;
    exec = lib.concatStringsSep " " (
      ["cmake" "-B" buildDir] ++ cmakeBaseFlags ++ extraFlags
    );
  };

  mkBuildTask = buildDir: {
    cwd = config.git.root;
    exec = "cmake --build ${buildDir}";
  };
in {
  cachix.enable = false;
  packages =
    buildDeps.native
    ++ buildDeps.runtime
    ++ [
      toolchain.cc
      llvm21.llvm
      userverPkgs.userver-debug-addr-ub
      uniAlgoPkgs.default
      yttsPkgs.default
    ]
    ++ userverDeps
    ++ [webshotTestSan]
    ++ (with pkgsWithOverlay; [git gdb]);

  treefmt = {
    enable = true;
    config = {
      programs = {
        alejandra.enable = true;
        clang-format.enable = true;
        cmake-format.enable = true;
        ruff-format.enable = true;
        sqlfluff.enable = true;
      };
      settings.global.excludes = [
        ".git/**"
        ".devenv/**"
        ".direnv/**"
        ".cache/**"
        ".pytest_cache/**"
        "secrets/**"
      ];
    };
  };
  difftastic.enable = true;

  git-hooks.hooks = {
    treefmt = {
      enable = true;
      settings.formatters = builtins.attrValues config.treefmt.config.build.programs;
    };
    ruff.enable = true;
    sqlfluff-lint = {
      enable = true;
      entry = "sqlfluff lint";
      package = pkgsWithOverlay.sqlfluff;
      language = "system";
      files = "\\.sql$";
    };
  };
  env.CMAKE_PREFIX_PATH = lib.makeSearchPath "lib/cmake" [
    userverPkgs.userver-debug-addr-ub
    pkgsWithOverlay.boost183.dev
    pkgsWithOverlay.fmt.dev
    pkgsWithOverlay.zstd.dev
    pkgsWithOverlay.cctz
    pkgsWithOverlay.yaml-cpp
  ];

  env.PKG_CONFIG_PATH = lib.makeSearchPath "lib/pkgconfig" [
    pkgsWithOverlay.cryptopp.dev
  ];

  env.USERVER_PYTHON = "${chaoticPython}/bin/python3";
  env.USERVER_PYTHON_PATH = "${chaoticPython}/bin/python3";

  env.USERVER_DIR = "${userverPkgs.userver-debug-addr-ub}/lib/cmake/userver";

  # Expose the yandex-taxi-testsuite Python package so pytest_userver
  # can import `testsuite` (for chaos, pgsql helpers, etc.).
  env.PYTHONPATH = lib.makeSearchPath python.sitePackages [
    yttsPkgs.default
  ];

  env.WEBSHOT_LLVM_SYMBOLIZER_BIN = "${llvm21.llvm}/bin/llvm-symbolizer";
  env.WEBSHOT_RUNTIME_LD_LIBRARY_PATH = lib.makeLibraryPath testLibs;

  tasks."webshot:infraUp" = {
    exec = "bash containers/quadlet/install_quadlet.sh && systemctl --user start webshot-stack.target";
    cwd = config.git.root;
  };

  tasks."webshot:infraDown" = {
    exec = "systemctl --user stop webshot-stack.target";
    cwd = config.git.root;
  };

  tasks."webshot:debugUp" = {
    exec = "bash containers/quadlet/install_quadlet.sh && systemctl --user start webshot-debug-stack.target";
    cwd = config.git.root;
  };

  tasks."webshot:debugDown" = {
    exec = "systemctl --user stop webshot-debug-stack.target";
    cwd = config.git.root;
  };

  tasks."webshot:prodInfraUp" = {
    exec = "bash containers/quadlet/install_quadlet.sh && systemctl --user start webshot-prod-stack.target";
    cwd = config.git.root;
  };

  tasks."webshot:prodInfraDown" = {
    exec = "systemctl --user stop webshot-prod-stack.target";
    cwd = config.git.root;
  };

  tasks."webshot:prodDebugUp" = {
    exec = "bash containers/quadlet/install_quadlet.sh && systemctl --user start webshot-prod-debug-stack.target";
    cwd = config.git.root;
  };

  tasks."webshot:prodDebugDown" = {
    exec = "systemctl --user stop webshot-prod-debug-stack.target";
    cwd = config.git.root;
  };

  tasks."webshot:configureSan" =
    mkConfigureTask buildDirs.san sanFlags;

  tasks."webshot:configureTidy" =
    mkConfigureTask buildDirs.tidy tidyFlags;

  tasks."webshot:configureCov" =
    mkConfigureTask buildDirs.cov covFlags;

  tasks."webshot:configureRelease" =
    mkConfigureTask buildDirs.release releaseFlags;

  tasks."webshot:buildSan" =
    mkBuildTask buildDirs.san;

  tasks."webshot:buildTidy" =
    mkBuildTask buildDirs.tidy;

  tasks."webshot:buildCov" =
    mkBuildTask buildDirs.cov;

  tasks."webshot:buildRelease" =
    mkBuildTask buildDirs.release;

  tasks."webshot:testSan" = {
    package = webshotTestSan;
    exec = "webshot-test-san";
  };
}
