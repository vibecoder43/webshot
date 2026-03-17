{
  pkgs,
  config,
  inputs,
  ...
}: let
  pkgsWithOverlay = pkgs.extend (import ../nix/overlay/boost_stacktrace_backtrace.nix);

  lib = pkgsWithOverlay.lib;
  devenvLock = builtins.fromJSON (builtins.readFile ../devenv.lock);
  lockValidation = let
    validateNode = name: let
      node = devenvLock.nodes.${name};
      locked = node.locked or null;
      nodeType =
        if locked == null
        then null
        else locked.type or null;
    in
      if name == devenvLock.root || locked == null || nodeType == "path"
      then []
      else if nodeType == "github"
      then lib.optional (!(builtins.hasAttr "narHash" locked)) "${name} (github)"
      else [
        "${name} (${
          if nodeType == null
          then "missing type"
          else nodeType
        })"
      ];

    invalidNodes = lib.concatMap validateNode (builtins.attrNames devenvLock.nodes);
  in
    if invalidNodes == []
    then true
    else
      throw ''
        devenv.lock is missing locked.narHash for remote inputs or contains unsupported remote input types: ${lib.concatStringsSep ", " invalidNodes}
        Update devenv.lock to include locked.narHash for each remote node.
      '';
  buildDeps = import ../nix/common_deps.nix {pkgs = pkgsWithOverlay;};
  toolchain = import ../nix/toolchain.nix {pkgs = pkgsWithOverlay;};
  llvm21 = pkgsWithOverlay.llvmPackages_21;
  system = pkgsWithOverlay.stdenv.system;
  crawlerRuntimeBins = with pkgsWithOverlay; [
    ungoogled-chromium
    bubblewrap
    socat
  ];

  python = pkgsWithOverlay.python3;
  userverPython = import ../nix/userver/deps.nix {
    pkgs = pkgsWithOverlay;
    inherit python;
  };
  inherit (userverPython) userverDeps userverHelperPython;

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

  userverPkgs = import ../nix/userver/packages.nix {
    pkgs = pkgsWithOverlay;
    inherit (inputs) userverSrc;
  };
  pgmigratePkgs = {
    default = import ../nix/pgmigrate/package.nix {
      pkgs = pkgsWithOverlay;
      inherit (inputs) pgmigrateSrc;
    };
  };
  uniAlgoPkgs = {
    default = import ../nix/uni-algo/package.nix {
      pkgs = pkgsWithOverlay;
      inherit (inputs) uniAlgoSrc;
    };
  };
  yttsPkgs = {
    default = import ../nix/yandex-taxi-testsuite/package.nix {
      pkgs = pkgsWithOverlay;
      inherit (inputs) pgmigrateSrc yandexTaxiTestsuiteSrc;
    };
  };
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
    "-DWEBSHOT_RAPIDOC_ASSETS_DIR=${rapidocAssets}"
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
        disablePrecompileHeaders = true;
        cmakeCxxIncludeWhatYouUse = "include-what-you-use";
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
    ++ lib.optional (variant ? clangTidy) "-DCMAKE_CXX_CLANG_TIDY=${variant.clangTidy}"
    ++ lib.optional
    (variant ? cmakeCxxIncludeWhatYouUse)
    "-DCMAKE_CXX_INCLUDE_WHAT_YOU_USE=${variant.cmakeCxxIncludeWhatYouUse}"
    ++ lib.optional
    (variant ? disablePrecompileHeaders && variant.disablePrecompileHeaders)
    "-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON";

  mkTaskCmakeFlags = buildDir: variant:
    [
      "-S"
      "./webshotd"
      "-G"
      "Ninja"
      "-DCMAKE_CXX_COMPILER=clang++"
      "-DCMAKE_C_COMPILER_LAUNCHER=ccache"
      "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache"
      "-DCMAKE_INSTALL_PREFIX=${buildDir}/runtime_root"
    ]
    ++ mkCmakeCommonFlags {
      userverDir = "${userverPkgs.userver-debug-addr-ub}/lib/cmake/userver";
      pythonPath = "${userverHelperPython}/bin/python3";
    }
    ++ mkCmakeVariantFlags variant
    ++ [
      "-DUSERVER_FEATURE_TESTSUITE=ON"
      "-DUSERVER_TESTSUITE_USE_VENV=OFF"
      "-DUSERVER_SQL_USE_VENV=OFF"
      "-DUSERVER_CHAOTIC_USE_VENV=OFF"
      "-DTESTSUITE_PYTHON_BINARY=${userverHelperPython}/bin/python3"
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
      pythonPath = "${userverHelperPython}/bin/python3";
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

      nativeBuildInputs = buildDeps.native ++ [toolchain.cc pkgsWithOverlay.makeWrapper];
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

      postFixup = ''
        wrapProgram "$out/webshotd/webshotd" \
          --prefix PATH : "${lib.makeBinPath crawlerRuntimeBins}"
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
    ${lib.escapeShellArgs (["cmake" "-B" buildDir] ++ mkTaskCmakeFlags buildDir variant)}
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

  rapidocAssetsVersion = "9.3.8";
  rapidocAssets = pkgsWithOverlay.stdenvNoCC.mkDerivation {
    pname = "rapidoc-assets";
    version = rapidocAssetsVersion;
    src = pkgsWithOverlay.fetchurl {
      url = "https://registry.npmjs.org/rapidoc/-/rapidoc-${rapidocAssetsVersion}.tgz";
      hash = "sha512-eCYEbr1Xr8OJZvVCw8SXl9zBCRoLJbhNGuG5IZTHq/RWAOq/O4MafUCuFEyZHsrhLrlUcGZMa64pyhpib8fQKQ==";
    };
    nativeBuildInputs = with pkgsWithOverlay; [gnutar gzip];
    dontConfigure = true;
    dontBuild = true;
    unpackPhase = ''
      tar -xzf "$src"
    '';
    installPhase = ''
      mkdir -p "$out"
      cp package/dist/rapidoc-min.js "$out/rapidoc-min.js"
    '';
  };
in
  assert lockValidation; {
    inherit
      buildDeps
      buildVariants
      buildDirs
      crawlerRuntimeBins
      clangdConfigs
      lib
      llvm21
      mkBuildTask
      mkConfigureTaskCommands
      mkConfigureTask
      mkWebshotOutput
      pkgsWithOverlay
      pgmigratePkgs
      python
      rapidocAssets
      testLibs
      toolchain
      treefmtExcludesFromGitignore
      uniAlgoPkgs
      userverDeps
      userverHelperPython
      userverPkgs
      webshotTestCov
      webshotTestSan
      yttsPkgs
      ;
  }
