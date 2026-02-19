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
    cd ${buildDirs.san}
    ctest --output-on-failure
  '';

  webshotTestCov = pkgsWithOverlay.writeShellScriptBin "webshot-test-cov" ''
    set -euo pipefail
    export LD_LIBRARY_PATH='${lib.makeLibraryPath testLibs}'
    cmake --build ${buildDirs.cov} --target webshot-coverage-html
  '';

  userverPkgs = inputs.userver.packages.${system};
  uniAlgoPkgs = inputs."uni-algo".packages.${system};
  yttsPkgs = inputs."yandex-taxi-testsuite".packages.${system};
  buildDirs = {
    san = "${config.devenv.root}/build/san";
    tidy = "${config.devenv.root}/build/tidy";
    cov = "${config.devenv.root}/build/cov";
    release = "${config.devenv.root}/build/release";
  };

  cmakeBaseFlags = [
    "-S ."
    "-G Ninja"
    "-D CMAKE_CXX_COMPILER=clang++"
    "-D CMAKE_C_COMPILER_LAUNCHER=ccache"
    "-D CMAKE_CXX_COMPILER_LAUNCHER=ccache"
    "-D USERVER_PYTHON_PATH=$USERVER_PYTHON_PATH"
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
    "-D CMAKE_BUILD_TYPE=Debug"
    "-D CMAKE_EXPORT_COMPILE_COMMANDS=ON"
    "-D USE_SANITIZERS=ON"
    "-D BUILD_TESTING=ON"
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
    ];

  releaseFlags = [
    "-D CMAKE_BUILD_TYPE=Release"
    "-D CMAKE_EXPORT_COMPILE_COMMANDS=ON"
    "-D USE_SANITIZERS=OFF"
    "-D BUILD_TESTING=OFF"
  ];

  mkClangdConfig = name: buildDir:
    pkgsWithOverlay.writeText "webshot-clangd-${name}" ''
      CompileFlags:
        CompilationDatabase: ${buildDir}
    '';

  clangdConfigs = {
    san = mkClangdConfig "san" buildDirs.san;
    tidy = mkClangdConfig "tidy" buildDirs.tidy;
    cov = mkClangdConfig "cov" buildDirs.cov;
    release = mkClangdConfig "release" buildDirs.release;
  };

  mkConfigureTask = buildDir: clangdConfig: extraFlags: {
    cwd = config.devenv.root;
    exec =
      lib.concatStringsSep " " (
        ["cmake" "-B" buildDir] ++ cmakeBaseFlags ++ extraFlags
      )
      + ''&& ln -sf ${clangdConfig} .clangd'';
  };

  mkBuildTask = buildDir: {
    cwd = config.devenv.root;
    exec = "cmake --build ${buildDir}";
  };
in {
  cachix.enable = true;
  packages =
    buildDeps.native
    ++ buildDeps.runtime
    ++ [
      toolchain.cc
      llvm21.llvm
      llvm21.clang-tools
      userverPkgs.userver-debug-addr-ub
      uniAlgoPkgs.default
      yttsPkgs.default
    ]
    ++ userverDeps
    ++ [webshotTestSan webshotTestCov]
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
        yamlfmt.enable = true;
      };
      settings.global.excludes = [
        ".git/**"
        ".devenv/**"
        ".direnv/**"
        ".cache/**"
        ".pytest_cache/**"
        "secret/**"
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
    shellcheck = {
      enable = true;
      args = ["-x"];
    };
    unicode-hygiene = {
      enable = true;
      entry = "python3 check_unicode_hygiene.py";
      package = python;
      language = "system";
      files = "";
    };
    yamllint = {
      enable = true;
      settings.configuration = ''
        extends: relaxed
        rules:
          line-length: disable
      '';
    };
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

  env.WEBSHOT_RUNTIME_LD_LIBRARY_PATH = lib.makeLibraryPath testLibs;
  env.WEBSHOT_BUILD_DIR = buildDirs.san;
  env.WEBSHOT_STATE_DIR = "${config.devenv.root}/.cache/webshot";

  tasks."webshot:infraDevUp" = {
    exec = "bash container/compose/infra_dev_up.sh";
    cwd = config.devenv.root;
  };

  tasks."webshot:infraDevDown" = {
    exec = "bash container/compose/infra_dev_down.sh";
    cwd = config.devenv.root;
  };

  tasks."webshot:devUp" = {
    exec = "bash container/compose/webshot_ctl.sh dev up";
    cwd = config.devenv.root;
  };

  tasks."webshot:devDown" = {
    exec = "bash container/compose/webshot_ctl.sh dev down";
    cwd = config.devenv.root;
  };

  tasks."webshot:devStatus" = {
    exec = "bash container/compose/webshot_ctl.sh dev status";
    cwd = config.devenv.root;
  };

  tasks."webshot:devLogs" = {
    exec = "bash container/compose/webshot_ctl.sh dev logs";
    cwd = config.devenv.root;
  };

  tasks."webshot:infraProdlikeUp" = {
    exec = "bash container/compose/infra_prodlike_up.sh";
    cwd = config.devenv.root;
  };

  tasks."webshot:infraProdlikeDown" = {
    exec = "bash container/compose/infra_prodlike_down.sh";
    cwd = config.devenv.root;
  };

  tasks."webshot:prodlikeUp" = {
    exec = "bash container/compose/webshot_ctl.sh prodlike up";
    cwd = config.devenv.root;
  };

  tasks."webshot:prodlikeDown" = {
    exec = "bash container/compose/webshot_ctl.sh prodlike down";
    cwd = config.devenv.root;
  };

  tasks."webshot:prodlikeStatus" = {
    exec = "bash container/compose/webshot_ctl.sh prodlike status";
    cwd = config.devenv.root;
  };

  tasks."webshot:prodlikeLogs" = {
    exec = "bash container/compose/webshot_ctl.sh prodlike logs";
    cwd = config.devenv.root;
  };

  tasks."webshot:configureSan" =
    mkConfigureTask buildDirs.san clangdConfigs.san sanFlags;

  tasks."webshot:configureTidy" =
    mkConfigureTask buildDirs.tidy clangdConfigs.tidy tidyFlags;

  tasks."webshot:configureCov" =
    mkConfigureTask buildDirs.cov clangdConfigs.cov covFlags;

  tasks."webshot:configureRelease" =
    mkConfigureTask buildDirs.release clangdConfigs.release releaseFlags;

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

  tasks."webshot:testCov" = {
    package = webshotTestCov;
    exec = "webshot-test-cov";
  };
}
