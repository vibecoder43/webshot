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

  mkClangUml = name: relativeBuildDir:
    nix.writeText "webshot-clang-uml-${name}" ''
      compilation_database_dir: ${relativeBuildDir}
      output_directory: diagrams
      relative_to: ${config.devenv.root}
      diagrams:
        class_overview:
          type: class
          glob:
            - webshotd/src/**.cpp
            - webshotd/include/**.hpp
          generate_method_arguments: none
          include:
            namespaces:
              - ws
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

    clangUml = {
      san = mkClangUml "san" "build/webshotd/san";
      tidy = mkClangUml "tidy" "build/webshotd/tidy";
    };

    cmakePrefix = lib.concatStringsSep ";" sets.cmakePrefix;
  };

  drv = import ./drv.nix {
    inherit inputs nix paths s6Src sets srcs toolchain;
  };

  repoToolPythonPath = "${drv.repoToolPy}/bin/python3";

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
    "-DCMAKE_PREFIX_PATH=${paths.cmakePrefix}"
    "-Duserver_DIR=${userverDir}"
    "-DUSERVER_PYTHON_PATH=${pythonPath}"
    "-DUSERVER_DEBUG_INFO_COMPRESSION=z"
    "-DWEBSHOT_BROWSER_SANDBOX_CLOSURE_PATHS_FILE=${sets.browserSandboxClosure}/store-paths"
    "-DWEBSHOT_BROWSER_SANDBOX_FONTCONFIG_FILE=${sets.browserSandboxFontconfigFile}"
    "-DWEBSHOT_BROWSER_SANDBOX_PATH=${sets.browserSandboxPath}"
  ];

  mkToolchainFlags = [
    "-DCMAKE_CXX_COMPILER=${toolchain.cc}/bin/clang++"
    "-DCMAKE_C_COMPILER=${toolchain.cc}/bin/clang"
    "-DCMAKE_LINKER_TYPE=webshot_mold"
    "-DCMAKE_CXX_USING_LINKER_webshot_mold=-B${nix.mold}/bin;-fuse-ld=mold"
    "-DCMAKE_C_USING_LINKER_webshot_mold=-B${nix.mold}/bin;-fuse-ld=mold"
  ];

  runtimeAssetDirs = [
    {
      source = drv.webUi;
      destination = "web_ui/vendor";
    }
    {
      source = drv.rapidoc;
      destination = "rapidoc-assets";
    }
  ];

  mkStageRuntimeAssets = {
    layoutRoot,
    shellLayoutRoot ? false,
  }: let
    layoutRootExpr =
      if shellLayoutRoot
      then layoutRoot
      else lib.escapeShellArg layoutRoot;
    stageDir = entry: ''
      stage_runtime_dir ${lib.escapeShellArg entry.source} "$layout_root/${entry.destination}"
    '';
  in ''
    layout_root=${layoutRootExpr}
    stage_runtime_dir() {
      local source="$1"
      local destination="$2"

      mkdir -p "$(dirname "$destination")"
      rsync -a --delete "$source"/ "$destination"/
    }
    ${lib.concatMapStrings stageDir runtimeAssetDirs}
  '';

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
    ++ mkUserverNoVenvFlags repoToolPythonPath
    ++ mkCommonFlags {
      userverDir = "${userver}/lib/cmake/userver";
      pythonPath = repoToolPythonPath;
    }
    ++ mkToolchainFlags;

  mkConfigure = {
    buildDir,
    variant,
  }:
    ["cmake"]
    ++ ["-U" "*_DIR"]
    ++ ["-B" buildDir]
    ++ [
      "-S"
      "./webshotd"
      "-G"
      "Ninja"
      "-DCMAKE_C_COMPILER_LAUNCHER=ccache"
      "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache"
      "-DCMAKE_INSTALL_PREFIX=${buildDir}/runtime_root"
    ]
    ++ mkToolchainFlags
    ++ mkCommonFlags {
      userverDir = "${drv.userverDbg}/lib/cmake/userver";
      pythonPath = repoToolPythonPath;
    }
    ++ mkVariantFlags variant
    ++ [
      "-DUSERVER_FEATURE_TESTSUITE=ON"
      "-Wno-dev"
    ]
    ++ mkUserverNoVenvFlags repoToolPythonPath;
in {
  inherit drv mkStageRuntimeAssets nix paths sets srcs toolchain variants;
  treefmtExcludes = treefmtExcludes;
  mkConfigure = mkConfigure;

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

      nativeBuildInputs = sets.buildNative ++ [toolchain.cc nix.makeWrapper nix.rsync];
      buildInputs = sets.buildInputsFor userver;

      cmakeFlags = mkOutputFlags {
        inherit userver variant;
      };

      postInstall = ''
                ${mkStageRuntimeAssets {
          layoutRoot = "\"$out\"";
          shellLayoutRoot = true;
        }}

                patchShebangs "$out/webshotd/webshotd_wrapper"
                wrapProgram "$out/webshotd/webshotd_wrapper" \
                  --set PATH "${lib.makeBinPath sets.runtimeTools}"

                # NixOS may generate a drop-in that overrides PATH. Avoid relying on unit
                # environment by routing through a package entrypoint that sets PATH to
                # the runtime toolchain.
                install -Dm0755 /dev/null "$out/bin/webshot-systemd-runtime"
                cat >"$out/bin/webshot-systemd-runtime" <<'EOF'
        #!${nix.bash}/bin/bash
        set -euo pipefail
        export PATH='${sets.systemdRuntimePath}'
        exec '${drv.repoPy}/bin/python3' -m s6.runtime "$@"
        EOF

                install_systemd_unit() {
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
            --replace-fail '@execStartExtra@' "$exec_start_extra"
        }

                install_systemd_unit \
                  webshot.service \
                  "webshot stack" \
                  webshot-local-s3.service \
                  "" \
                  ""
                install_systemd_unit \
                  webshot-local-s3.service \
                  "webshot stack (local S3)" \
                  webshot.service \
                  "" \
                  ' --seaweedfs-s3-config ${"$"}{CREDENTIALS_DIRECTORY}/seaweedfs_s3_config.json'
      '';
    };
}
