{
  pkgs,
  config,
  inputs,
  ...
}: let
  pkgsWithOverlay = pkgs.extend (import ../nix/overlay/boost_stacktrace_backtrace.nix);

  lib = pkgsWithOverlay.lib;
  buildDeps = import ../nix/common_deps.nix {pkgs = pkgsWithOverlay;};
  toolchain = import ../nix/toolchain.nix {pkgs = pkgsWithOverlay;};
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

  userverDeps = import ../nix/userver/deps.nix {
    pkgs = pkgsWithOverlay;
    inherit chaoticPython;
  };

  testLibs = userverDeps ++ [pkgsWithOverlay.libarchive pkgsWithOverlay.stdenv.cc.cc];

  webshotTestSan = pkgsWithOverlay.writeShellScriptBin "webshot_test_san" ''
    set -euo pipefail
    export LD_LIBRARY_PATH='${lib.makeLibraryPath testLibs}'
    cd ${buildDirs.san}
    ctest --progress --output-on-failure -V
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
    san = "${config.devenv.root}/build/webshotd/san";
    tidy = "${config.devenv.root}/build/webshotd/tidy";
    cov = "${config.devenv.root}/build/webshotd/cov";
    release = "${config.devenv.root}/build/webshotd/release";
  };

  nixGitignore = pkgsWithOverlay.nix-gitignore;

  webshotSrc = nixGitignore.gitignoreSource [] ../.;

  gitignoreLines = lib.splitString "\n" (builtins.readFile ../.gitignore);
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
    "-DWEBSHOT_SCALAR_ASSETS_DIR=${scalarAssets}"
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
      "./webshotd"
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
      "-DCMAKE_INSTALL_RPATH=${lib.makeLibraryPath ([userverPkg uniAlgoPkgs.default pkgsWithOverlay.libarchive pkgsWithOverlay.stdenv.cc.cc.lib] ++ userverDeps)}"
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
      sourceRoot = "source/webshotd";

      dontStrip = true;

      nativeBuildInputs = buildDeps.native ++ [toolchain.cc];
      buildInputs =
        [
          userverPkg
          uniAlgoPkgs.default
          pkgsWithOverlay.libarchive
        ]
        ++ userverDeps;

      cmakeFlags = mkOutputCmakeFlags {
        inherit userverPkg variant;
      };
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

  scalarAssetsVersion = "1.48.2";
  scalarAssets = pkgsWithOverlay.stdenvNoCC.mkDerivation {
    pname = "scalar-assets";
    version = scalarAssetsVersion;
    src = pkgsWithOverlay.fetchurl {
      url = "https://registry.npmjs.org/@scalar/api-reference/-/api-reference-${scalarAssetsVersion}.tgz";
      hash = "sha512-9u+SXr1aNK0Rfv/hdOWU3ACACn/L472jo5aZGR1NidyIc9eeDlXPGYSfGmOiDDKM1VIs53PjLzojLvo/o4mNfg==";
    };
    nativeBuildInputs = with pkgsWithOverlay; [gnutar gzip];
    dontConfigure = true;
    dontBuild = true;
    unpackPhase = ''
      tar -xzf "$src"
    '';
    installPhase = ''
      mkdir -p "$out"
      cp package/dist/browser/standalone.js "$out/api-reference.js"
      cp package/dist/style.css "$out/style.css"
    '';
  };
in {
  inherit
    buildDeps
    buildVariants
    buildDirs
    chaoticPython
    clangdConfigs
    lib
    llvm21
    mkBuildTask
    mkConfigureTask
    mkWebshotOutput
    pkgsWithOverlay
    python
    scalarAssets
    testLibs
    toolchain
    treefmtExcludesFromGitignore
    uniAlgoPkgs
    userverDeps
    userverPkgs
    webshotTestCov
    webshotTestSan
    yttsPkgs
    ;
}
