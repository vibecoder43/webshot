{
  pkgs,
  config,
  inputs,
  ...
}: let
  nix = pkgs.extend (import ./overlay/backtrace.nix);
  lib = nix.lib;

  rootType = builtins.typeOf config.devenv.root;
  rootIsStoreString =
    rootType
    == "string"
    && lib.hasPrefix "${builtins.storeDir}/" config.devenv.root;
  projRootPath =
    if rootIsStoreString
    then null
    else if rootType == "path"
    then config.devenv.root
    else /. + config.devenv.root;

  projSrc =
    if rootIsStoreString
    then config.devenv.root
    else
      lib.fileset.toSource {
        root = projRootPath;
        fileset = lib.fileset.gitTracked projRootPath;
      };

  s6Src = projSrc + "/s6";

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
      buildType = "RelWithDebInfo";
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
    };

    clangd = {
      san = mkClangd "san" build.san;
      tidy = mkClangd "tidy" build.tidy;
    };

    cmakePrefix = lib.makeSearchPath "lib/cmake" sets.cmakePrefix;
  };

  drv = import ./drv.nix {
    inherit inputs nix paths projSrc s6Src sets srcs toolchain;
  };

  repoPythonPath = "${drv.repoPy}/bin/python3";

  sets = import ./sets.nix {
    inherit drv nix;
  };

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
    "-DWEBSHOT_RAPIDOC_ASSETS_DIR=${drv.rapidoc}"
    "-DWEBSHOT_WEB_UI_VENDOR_DIR=${drv.webUi}"
    "-DWEBSHOT_BROWSER_SANDBOX_CLOSURE_PATHS_FILE=${sets.browserSandboxClosure}/store-paths"
    "-DWEBSHOT_BROWSER_SANDBOX_FONTCONFIG_FILE=${sets.browserSandboxFontconfigFile}"
    "-DWEBSHOT_BROWSER_SANDBOX_PATH=${sets.browserSandboxPath}"
  ];

  mkUserverNoVenvFlags = pythonPath: [
    "-DUSERVER_TESTSUITE_USE_VENV=OFF"
    "-DUSERVER_TESTSUITE_PYTHON_BINARY=${pythonPath}"
    "-DTESTSUITE_PYTHON_BINARY=${pythonPath}"
    "-DUSERVER_SQL_USE_VENV=OFF"
    "-DUSERVER_SQL_PYTHON_BINARY=${pythonPath}"
    "-DUSERVER_CHAOTIC_USE_VENV=OFF"
    "-DUSERVER_CHAOTIC_PYTHON_BINARY=${pythonPath}"
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
      "-DCMAKE_INSTALL_RPATH=${lib.makeLibraryPath (sets.rpathLibsFor userver)}"
      "-DUSERVER_FEATURE_TESTSUITE=OFF"
    ]
    ++ mkUserverNoVenvFlags repoPythonPath
    ++ mkCommonFlags {
      userverDir = "${userver}/lib/cmake/userver";
      pythonPath = repoPythonPath;
    }
    ++ [
      "-DCMAKE_CXX_COMPILER=${toolchain.cc}/bin/clang++"
      "-DCMAKE_C_COMPILER=${toolchain.cc}/bin/clang"
    ];

  mkConfigure = {
    buildDir,
    variant,
    fresh,
  }:
    ["cmake"]
    ++ lib.optional fresh "--fresh"
    ++ ["-B" buildDir]
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
      pythonPath = repoPythonPath;
    }
    ++ mkVariantFlags variant
    ++ [
      "-DUSERVER_FEATURE_TESTSUITE=ON"
      "-Wno-dev"
    ]
    ++ mkUserverNoVenvFlags repoPythonPath;
in {
  inherit drv nix paths sets srcs toolchain variants;
  treefmtExcludes = treefmtExcludes;
  mkConfigure = mkConfigure;

  mkConfigureFingerprint = {
    buildDir,
    variant,
  }:
    builtins.hashString "sha256" (builtins.toJSON (mkConfigure {
      inherit buildDir variant;
      fresh = false;
    }));

  mkProjPkg = {
    suffix ? "",
    userver,
    variant ? variants.release,
  }:
    toolchain.stdenv.mkDerivation {
      name = "webshot${suffix}";
      src = projSrc;
      # The source root name differs between flake and live-root evaluation, so
      # pick the service subdir dynamically instead of assuming `source/`.
      setSourceRoot = ''
        matches=(*/webshotd)
        if [[ "''${#matches[@]}" -ne 1 || ! -d "''${matches[0]}" ]]; then
          printf 'expected one unpacked repo root with webshotd/, got: %s\n' "''${matches[*]}" >&2
          exit 1
        fi
        sourceRoot="''${matches[0]}"
      '';

      dontStrip = true;

      nativeBuildInputs = sets.buildNative ++ [toolchain.cc nix.makeWrapper];
      buildInputs = sets.buildInputsFor userver;

      cmakeFlags = mkOutputFlags {
        inherit userver variant;
      };

      postInstall = ''
        patchShebangs "$out/webshotd/webshotd_wrapper"
        wrapProgram "$out/webshotd/webshotd_wrapper" \
          --set PATH "${lib.makeBinPath sets.runtimeTools}"

        installSystemdUnit() {
          local unit_name="$1"
          local description="$2"
          local conflicts="$3"
          local local_s3_assert="$4"
          local exec_start_extra="$5"
          local unit_path="$out/lib/systemd/system/$unit_name"

          install -Dm0644 ${./systemd/webshot.service.in} "$unit_path"
          substituteInPlace "$unit_path" \
            --replace-fail '@description@' "$description" \
            --replace-fail '@conflicts@' "$conflicts" \
            --replace-fail '@localS3Assert@' "$local_s3_assert" \
            --replace-fail '@out@' "$out" \
            --replace-fail '@repoPython@' '${drv.repoPy}' \
            --replace-fail '@runtimePath@' '${sets.systemdRuntimePath}' \
            --replace-fail '@execStartExtra@' "$exec_start_extra"
        }

        installSystemdUnit \
          webshot.service \
          "webshot stack" \
          webshot-local-s3.service \
          "" \
          ""
        installSystemdUnit \
          webshot-local-s3.service \
          "webshot stack (local S3)" \
          webshot.service \
          "AssertPathExists=/etc/webshot/seaweedfs_s3_config.json" \
          " --seaweedfs-s3-config /etc/webshot/seaweedfs_s3_config.json"
      '';
    };
}
