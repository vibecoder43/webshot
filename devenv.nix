{
  pkgs,
  config,
  inputs,
  ...
}: let
  pkgsWithOverlay = pkgs.extend (import ./nix/overlay/boost_stacktrace_backtrace.nix);

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

  testLibs = userverDeps ++ [pkgsWithOverlay.stdenv.cc.cc];

  webshotTestSan = pkgsWithOverlay.writeShellScriptBin "webshot_test_san" ''
    set -euo pipefail
    export LD_LIBRARY_PATH='${lib.makeLibraryPath testLibs}'
    cd ${buildDirs.san}
    ctest --output-on-failure
  '';

  webshotTestCov = pkgsWithOverlay.writeShellScriptBin "webshot_test_cov" ''
    set -euo pipefail
    export LD_LIBRARY_PATH='${lib.makeLibraryPath testLibs}'
    cmake --build ${buildDirs.cov} --target coverage-html
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

  nixGitignore = pkgsWithOverlay.nix-gitignore;

  webshotSrc = nixGitignore.gitignoreSource [] ./.;

  gitignoreLines = lib.splitString "\n" (builtins.readFile ./.gitignore);
  gitignorePatterns =
    builtins.filter (
      line:
        line
        != ""
        && !(lib.hasPrefix "#" line)
    )
    (map (line: lib.removeSuffix "\r" line) gitignoreLines);

  treefmtExcludesFromGitignore =
    lib.concatMap (
      pattern:
        if lib.hasSuffix "/" pattern
        then ["${lib.removeSuffix "/" pattern}/**"]
        else [pattern "${pattern}/**"]
    )
    gitignorePatterns;

  mkCmakeCommonFlags = {
    userverDir,
    pythonPath,
  }: [
    "-Duserver_DIR=${userverDir}"
    "-DUSERVER_PYTHON_PATH=${pythonPath}"
    "-DUSERVER_DEBUG_INFO_COMPRESSION=z"
    "-DWEBSHOT_ENABLE_SQL_COVERAGE=OFF"
  ];

  cmakeBool = value:
    if value
    then "ON"
    else "OFF";

  buildVariants = rec {
    san = {
      buildType = "Debug";
      exportCompileCommands = true;
      useSanitizers = true;
      buildTesting = true;
      enableCoverage = false;
    };

    tidy =
      san
      // {
        clangTidy = "clang-tidy";
      };

    cov =
      san
      // {
        enableCoverage = true;
      };

    release = {
      buildType = "Release";
      exportCompileCommands = true;
      useSanitizers = false;
      buildTesting = false;
      enableCoverage = false;
    };
  };

  mkCmakeVariantFlags = variant:
    [
      "-DCMAKE_BUILD_TYPE=${variant.buildType}"
      "-DCMAKE_EXPORT_COMPILE_COMMANDS=${cmakeBool variant.exportCompileCommands}"
      "-DUSE_SANITIZERS=${cmakeBool variant.useSanitizers}"
      "-DBUILD_TESTING=${cmakeBool variant.buildTesting}"
      "-DWEBSHOT_ENABLE_COVERAGE=${cmakeBool variant.enableCoverage}"
    ]
    ++ lib.optional (variant ? clangTidy) "-DCMAKE_CXX_CLANG_TIDY=${variant.clangTidy}";

  mkTaskCmakeFlags = variant:
    [
      "-S"
      "."
      "-G"
      "Ninja"
      "-DCMAKE_CXX_COMPILER=clang++"
      "-DCMAKE_C_COMPILER_LAUNCHER=ccache"
      "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache"
    ]
    ++ mkCmakeCommonFlags {
      userverDir = "${userverPkgs.userver-debug-addr-ub}/lib/cmake/userver";
      pythonPath = "${chaoticPython}/bin/python3";
    }
    ++ mkCmakeVariantFlags variant
    ++ [
      "-DUSERVER_FEATURE_TESTSUITE=ON"
      "-DUSERVER_TESTSUITE_USE_VENV=OFF"
      "-DUSERVER_SQL_USE_VENV=OFF"
      "-DUSERVER_CHAOTIC_USE_VENV=OFF"
      "-DTESTSUITE_PYTHON_BINARY=${chaoticPython}/bin/python3"
      "-Wno-dev"
    ];

  mkOutputCmakeFlags = {
    userverPkg,
    variant,
  }:
    mkCmakeVariantFlags variant
    ++ [
      "-DUSERVER_USE_CCACHE=OFF"
      "-DCMAKE_C_COMPILER_LAUNCHER="
      "-DCMAKE_CXX_COMPILER_LAUNCHER="
      "-DCMAKE_BUILD_WITH_INSTALL_RPATH=ON"
      "-DCMAKE_INSTALL_RPATH=${lib.makeLibraryPath ([userverPkg uniAlgoPkgs.default pkgsWithOverlay.stdenv.cc.cc.lib] ++ userverDeps)}"
    ]
    ++ mkCmakeCommonFlags {
      userverDir = "${userverPkg}/lib/cmake/userver";
      pythonPath = "${chaoticPython}/bin/python3";
    }
    ++ [
      "-DCMAKE_CXX_COMPILER=${toolchain.cc}/bin/clang++"
      "-DCMAKE_C_COMPILER=${toolchain.cc}/bin/clang"
    ];

  mkWebshotOutput = {
    pnameSuffix ? "",
    userverPkg,
    variant ?
      buildVariants.san
      // {
        buildTesting = false;
        exportCompileCommands = false;
      },
  }:
    toolchain.stdenv.mkDerivation {
      pname = "webshot${pnameSuffix}";
      version = "0.1.0";
      src = webshotSrc;

      dontStrip = true;

      nativeBuildInputs = buildDeps.native ++ [toolchain.cc pkgsWithOverlay.makeWrapper];
      buildInputs =
        [
          userverPkg
          uniAlgoPkgs.default
        ]
        ++ userverDeps;

      cmakeFlags = mkOutputCmakeFlags {
        inherit userverPkg variant;
      };

      postFixup = ''
        wrapProgram "$out/bin/webshotd" \
          --prefix PATH : "${lib.makeBinPath [pkgsWithOverlay.podman]}"
      '';
    };

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

  mkConfigureTaskCommands = buildDir: clangdConfig: variant: ''
    ${lib.escapeShellArgs (["cmake" "-B" buildDir] ++ mkTaskCmakeFlags variant)}
    ln -sf "${clangdConfig}" .clangd
  '';

  mkConfigureTask = buildDir: clangdConfig: variant: {
    cwd = config.devenv.root;
    exec = ''
      set -euo pipefail
      ${mkConfigureTaskCommands buildDir clangdConfig variant}
    '';
  };

  mkBuildTask = buildDir: clangdConfig: variant: {
    cwd = config.devenv.root;
    exec = ''
      set -euo pipefail
      ${mkConfigureTaskCommands buildDir clangdConfig variant}
      cmake --build ${buildDir}
    '';
  };

  squidImageDev = import ./container/squid/squid.nix {
    pkgs = pkgsWithOverlay;
    tag = "dev";
  };

  squidImageProdlike = import ./container/squid/squid.nix {
    pkgs = pkgsWithOverlay;
    tag = "prodlike";
  };

  squidLoadPreamble = ''
    set -euo pipefail

    root="$PWD"
    while [[ "$root" != "/" && ! -f "$root/devenv.nix" ]]; do
      root="$(dirname "$root")"
    done
    [[ -f "$root/devenv.nix" ]] || { echo "Failed to find repo root (expected devenv.nix in a parent directory)" >&2; exit 2; }

    # shellcheck source=shell/lib.sh
    . "$root/shell/lib.sh"
    need devenv
    need podman
    cd -- "$root"
  '';

  squidLoadDev = pkgsWithOverlay.writeShellScriptBin "squid_load_dev" ''
    ${squidLoadPreamble}
    img="$(devenv build -q outputs.squidImageDev)"
    [[ -n "$img" ]] || { echo "Failed to build squidImageDev" >&2; exit 2; }
    exec podman load --quiet -i "$img"
  '';

  squidLoadProdlike = pkgsWithOverlay.writeShellScriptBin "squid_load_prodlike" ''
    ${squidLoadPreamble}
    img="$(devenv build -q outputs.squidImageProdlike)"
    [[ -n "$img" ]] || { echo "Failed to build squidImageProdlike" >&2; exit 2; }
    exec podman load --quiet -i "$img"
  '';
in {
  cachix.enable = true;
  outputs.webshot = mkWebshotOutput {userverPkg = userverPkgs.userver-debug-addr-ub;};
  outputs.squidImageDev = squidImageDev;
  outputs.squidImageProdlike = squidImageProdlike;
  packages =
    buildDeps.native
    ++ buildDeps.runtime
    ++ [
      chaoticPython
      toolchain.cc
      llvm21.llvm
      llvm21.clang-tools
      userverPkgs.userver-debug-addr-ub
      uniAlgoPkgs.default
      yttsPkgs.default
      squidLoadDev
      squidLoadProdlike
    ]
    ++ userverDeps
    ++ [webshotTestSan webshotTestCov]
    ++ (with pkgsWithOverlay; [git gdb nssTools]);
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
      settings.global.excludes = treefmtExcludesFromGitignore;
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
    unicode_hygiene = {
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
  env.PYTHONPATH =
    "${config.devenv.root}:"
    + (lib.makeSearchPath python.sitePackages [
      yttsPkgs.default
    ]);

  env.WEBSHOTD_RUNTIME_LD_LIBRARY_PATH = lib.makeLibraryPath testLibs;
  env.WEBSHOTD_BUILD_DIR = buildDirs.san;
  tasks."webshot:infraDevUp" = {
    exec = "python3 container/compose/infra.py dev up";
    cwd = config.devenv.root;
  };

  tasks."webshot:infraDevDown" = {
    exec = "python3 container/compose/infra.py dev down";
    cwd = config.devenv.root;
  };

  tasks."webshot:devUp" = {
    exec = "python3 container/compose/infra.py dev up && python3 container/compose/webshotd.py dev up";
    cwd = config.devenv.root;
  };

  tasks."webshot:devDown" = {
    exec = "python3 container/compose/webshotd.py dev down && python3 container/compose/infra.py dev down";
    cwd = config.devenv.root;
  };

  tasks."webshot:devStatus" = {
    exec = "python3 container/compose/infra.py dev status && python3 container/compose/webshotd.py dev status";
    cwd = config.devenv.root;
  };

  tasks."webshot:devLogs" = {
    exec = "python3 container/compose/webshotd.py dev logs";
    cwd = config.devenv.root;
  };

  tasks."webshot:infraProdlikeUp" = {
    exec = "python3 container/compose/infra.py prodlike up";
    cwd = config.devenv.root;
  };

  tasks."webshot:infraProdlikeDown" = {
    exec = "python3 container/compose/infra.py prodlike down";
    cwd = config.devenv.root;
  };

  tasks."webshot:prodlikeUp" = {
    exec = "python3 container/compose/infra.py prodlike up && python3 container/compose/webshotd.py prodlike up";
    cwd = config.devenv.root;
  };

  tasks."webshot:prodlikeDown" = {
    exec = "python3 container/compose/webshotd.py prodlike down && python3 container/compose/infra.py prodlike down";
    cwd = config.devenv.root;
  };

  tasks."webshot:prodlikeStatus" = {
    exec = "python3 container/compose/infra.py prodlike status && python3 container/compose/webshotd.py prodlike status";
    cwd = config.devenv.root;
  };

  tasks."webshot:prodlikeLogs" = {
    exec = "python3 container/compose/webshotd.py prodlike logs";
    cwd = config.devenv.root;
  };

  tasks."webshot:configureSan" =
    mkConfigureTask buildDirs.san clangdConfigs.san buildVariants.san;

  tasks."webshot:configureTidy" =
    mkConfigureTask buildDirs.tidy clangdConfigs.tidy buildVariants.tidy;

  tasks."webshot:configureCov" =
    mkConfigureTask buildDirs.cov clangdConfigs.cov buildVariants.cov;

  tasks."webshot:configureRelease" =
    mkConfigureTask buildDirs.release clangdConfigs.release buildVariants.release;

  tasks."webshot:buildSan" =
    mkBuildTask buildDirs.san clangdConfigs.san buildVariants.san;

  tasks."webshot:buildTidy" =
    mkBuildTask buildDirs.tidy clangdConfigs.tidy buildVariants.tidy;

  tasks."webshot:buildCov" =
    mkBuildTask buildDirs.cov clangdConfigs.cov buildVariants.cov;

  tasks."webshot:buildRelease" =
    mkBuildTask buildDirs.release clangdConfigs.release buildVariants.release;

  tasks."webshot:testSan" = {
    package = webshotTestSan;
    exec = "webshot_test_san";
  };

  tasks."webshot:testCov" = {
    package = webshotTestCov;
    exec = "webshot_test_cov";
  };
}
