{
  pkgs,
  config,
  inputs,
  ...
}: let
  nix = pkgs.extend (import ./overlay/backtrace.nix);
  lib = nix.lib;

  srcs = import ./srcs.nix {
    inherit inputs lib;
    lockFile = ../devenv.lock;
  };

  toolchain = import ./toolchain.nix {pkgs = nix;};

  variants = rec {
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
        enableIncludeWhatYouUse = true;
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

  mkClangd = name: buildDir:
    nix.writeText "webshot-clangd-${name}" ''
      CompileFlags:
        CompilationDatabase: ${buildDir}
    '';

  paths = rec {
    build = {
      san = "${config.devenv.root}/build/webshotd/san";
      tidy = "${config.devenv.root}/build/webshotd/tidy";
      cov = "${config.devenv.root}/build/webshotd/cov";
      release = "${config.devenv.root}/build/webshotd/release";
    };

    clangd = {
      san = mkClangd "san" build.san;
      tidy = mkClangd "tidy" build.tidy;
      cov = mkClangd "cov" build.cov;
      release = mkClangd "release" build.release;
    };

    cmakePrefix = lib.makeSearchPath "lib/cmake" sets.cmakePrefix;
  };

  drv = import ./drv.nix {
    inherit inputs nix paths sets srcs toolchain;
  };

  sets = import ./sets.nix {
    inherit drv nix;
  };

  webshotSrc = nix.nix-gitignore.gitignoreSource [] ../.;

  gitignoreLines = lib.splitString "\n" (builtins.readFile ../.gitignore);
  gitignorePatterns =
    builtins.filter (
      line:
        line
        != ""
        && !(lib.hasPrefix "#" line)
    )
    (map (line: lib.removeSuffix "\r" line) gitignoreLines);

  treefmtExcludes =
    lib.concatMap (
      pattern:
        if lib.hasSuffix "/" pattern
        then ["${lib.removeSuffix "/" pattern}/**"]
        else [pattern "${pattern}/**"]
    )
    gitignorePatterns;

  cmakeBool = value:
    if value
    then "ON"
    else "OFF";

  mkVariantFlags = variant:
    [
      "-DCMAKE_BUILD_TYPE=${variant.buildType}"
      "-DCMAKE_EXPORT_COMPILE_COMMANDS=${cmakeBool variant.exportCompileCommands}"
      "-DUSE_SANITIZERS=${cmakeBool variant.useSanitizers}"
      "-DBUILD_TESTING=${cmakeBool variant.buildTesting}"
      "-DWEBSHOT_ENABLE_COVERAGE=${cmakeBool variant.enableCoverage}"
    ]
    ++ lib.optional (variant ? clangTidy) "-DCMAKE_CXX_CLANG_TIDY=${variant.clangTidy}"
    ++ lib.optional
    (variant ? enableIncludeWhatYouUse && variant.enableIncludeWhatYouUse)
    "-DCMAKE_CXX_INCLUDE_WHAT_YOU_USE=${drv.includeWhatYouUse}/bin/include-what-you-use"
    ++ lib.optional
    (variant ? disablePrecompileHeaders && variant.disablePrecompileHeaders)
    "-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON";

  mkCommonFlags = {
    userverDir,
    pythonPath,
  }: [
    "-Duserver_DIR=${userverDir}"
    "-DUSERVER_PYTHON_PATH=${pythonPath}"
    "-DUSERVER_DEBUG_INFO_COMPRESSION=z"
    "-DWEBSHOT_ENABLE_SQL_COVERAGE=OFF"
    "-DWEBSHOT_RAPIDOC_ASSETS_DIR=${drv.rapidoc}"
    "-DWEBSHOT_WEB_UI_VENDOR_DIR=${drv.webUi}"
  ];

  mkOutputFlags = {
    userver,
    variant,
  }:
    mkVariantFlags variant
    ++ [
      "-DUSERVER_USE_CCACHE=OFF"
      "-DCMAKE_C_COMPILER_LAUNCHER="
      "-DCMAKE_CXX_COMPILER_LAUNCHER="
      "-DCMAKE_BUILD_WITH_INSTALL_RPATH=ON"
      "-DCMAKE_INSTALL_RPATH=${lib.makeLibraryPath ([userver drv.unialgo nix.libarchive nix.stdenv.cc.cc.lib] ++ sets.userver)}"
      "-DWEBSHOT_RUNTIME_EXTRA_PATH=${lib.makeBinPath sets.crawlerRuntime}"
    ]
    ++ mkCommonFlags {
      userverDir = "${userver}/lib/cmake/userver";
      pythonPath = "${drv.userverPy}/bin/python3";
    }
    ++ [
      "-DCMAKE_CXX_COMPILER=${toolchain.cc}/bin/clang++"
      "-DCMAKE_C_COMPILER=${toolchain.cc}/bin/clang"
    ];
in {
  inherit drv nix paths sets srcs toolchain variants;
  treefmtExcludes = treefmtExcludes;

  mkConfigure = buildDir: clangdFile: variant: ''
    ${lib.escapeShellArgs (["cmake" "-B" buildDir]
      ++ [
        "-S"
        "./webshotd"
        "-G"
        "Ninja"
        "-DCMAKE_CXX_COMPILER=clang++"
        "-DCMAKE_C_COMPILER_LAUNCHER=ccache"
        "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache"
        "-DCMAKE_INSTALL_PREFIX=${buildDir}/runtime_root"
      ]
      ++ mkCommonFlags {
        userverDir = "${drv.userverDbg}/lib/cmake/userver";
        pythonPath = "${drv.userverPy}/bin/python3";
      }
      ++ mkVariantFlags variant
      ++ [
        "-DUSERVER_FEATURE_TESTSUITE=ON"
        "-DUSERVER_TESTSUITE_USE_VENV=OFF"
        "-DUSERVER_SQL_USE_VENV=OFF"
        "-DUSERVER_CHAOTIC_USE_VENV=OFF"
        "-DTESTSUITE_PYTHON_BINARY=${drv.userverPy}/bin/python3"
        "-Wno-dev"
      ])}
    ln -sf "${clangdFile}" .clangd
  '';

  mkWebshot = {
    suffix ? "",
    userver,
    variant ?
      variants.san
      // {
        buildTesting = false;
        exportCompileCommands = false;
      },
  }:
    toolchain.stdenv.mkDerivation {
      pname = "webshot${suffix}";
      version = "0.1.0";
      src = webshotSrc;
      # gitignoreSource preserves the repo basename in the unpack tree, so pick
      # the service subdir dynamically instead of assuming `source/`.
      setSourceRoot = ''
        matches=(*/webshotd)
        if [[ "''${#matches[@]}" -ne 1 || ! -d "''${matches[0]}" ]]; then
          printf 'expected one unpacked repo root with webshotd/, got: %s\n' "''${matches[*]}" >&2
          exit 1
        fi
        sourceRoot="''${matches[0]}"
      '';

      dontStrip = true;

      nativeBuildInputs = sets.buildNative ++ [toolchain.cc];
      buildInputs =
        [
          userver
          drv.unialgo
          nix.libarchive
        ]
        ++ sets.userver;

      cmakeFlags = mkOutputFlags {
        inherit userver variant;
      };
    };
}
