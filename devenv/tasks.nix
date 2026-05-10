{
  pkgs,
  config,
  inputs,
  ...
}: let
  ctx = import ./ctx.nix {inherit pkgs config inputs;};
  lib = ctx.nix.lib;

  modes = {
    dev = {
      buildDir = ctx.paths.build.san;
      clangd = ctx.paths.clangd.san;
      clangUml = ctx.paths.clangUml.san;
      variant = ctx.variants.san;
      infra = "dev";
      configVars = "${config.devenv.root}/webshotd/config/config_vars.dev.yaml";
      seaweedfsS3Config = "${config.devenv.root}/seaweedfs/s3_config.json";
    };

    prodlike = {
      buildDir = ctx.paths.build.san;
      clangd = ctx.paths.clangd.san;
      clangUml = ctx.paths.clangUml.san;
      variant = ctx.variants.san;
      infra = "prodlike";
      configVars = "${config.devenv.root}/webshotd/config/config_vars.prodlike.yaml";
      seaweedfsS3Config = null;
    };
  };

  runtimeLdPath = lib.makeLibraryPath ctx.sets.testLibs;

  mkRepeatedFlagArgs = flag: args:
    lib.concatMapStringsSep " \\\n      " (arg: "${flag}=${lib.escapeShellArg arg}") args;

  mkClangdLink = clangdFile: ''
    if [[ ! -f remote_compile.json ]]; then
      ln -sf ${lib.escapeShellArg clangdFile} .clangd
    fi
  '';

  mkClangUmlLink = clangUmlFile: ''
    if [[ ! -f remote_compile.json ]]; then
      ln -sf ${lib.escapeShellArg clangUmlFile} .clang-uml
    fi
  '';

  mkBuild = {
    buildDir,
    clangdFile,
    clangUmlFile,
    variant,
    manageClangd ? true,
    timingsOutput ? null,
    buildArgs ? [],
  }: let
    configureArgv = ctx.mkConfigure {
      inherit buildDir variant;
    };
    timingsArgs =
      if timingsOutput == null
      then ""
      else ''
        \
          --timings-output ${lib.escapeShellArg timingsOutput} \
          --timings-collector ${lib.escapeShellArg "${config.devenv.root}/devenv/collect_build_times.py"}
      '';
    buildArgFlags =
      if buildArgs == []
      then ""
      else ''
        \
          ${mkRepeatedFlagArgs "--build-arg" buildArgs}
      '';
  in ''
    ${lib.optionalString manageClangd (mkClangdLink clangdFile)}
    ${lib.optionalString manageClangd (mkClangUmlLink clangUmlFile)}
    python3 devenv/build_task.py \
      --build-dir ${lib.escapeShellArg buildDir} \
      ${mkRepeatedFlagArgs "--configure-arg" configureArgv}${buildArgFlags}${timingsArgs}
    ${ctx.mkStageRuntimeAssets {
      layoutRoot = "${buildDir}/runtime_root";
    }}
  '';

  mkTask = exec: {
    cwd = config.devenv.root;
    inherit exec;
  };

  mkRuntime = action: mode: profile: let
    cfg = modes.${mode};
    runtimeDir = "${cfg.buildDir}/runtime_run/${cfg.infra}";
    profileArg =
      if profile == null
      then ""
      else " \\\n        --service-profile ${lib.escapeShellArg profile}";
    seaweedfsS3ConfigArg =
      if cfg.seaweedfsS3Config != null
      then " \\\n        --seaweedfs-s3-config ${lib.escapeShellArg cfg.seaweedfsS3Config}"
      else "";
  in
    if action == "up"
    then ''
      python3 -m s6.runtime up \
        --mode ${lib.escapeShellArg cfg.infra}${profileArg} \
        --runtime-dir ${lib.escapeShellArg runtimeDir} \
        --binary-path ${lib.escapeShellArg "${cfg.buildDir}/runtime_root/webshotd/webshotd_wrapper"} \
        --config-vars-source ${lib.escapeShellArg cfg.configVars} \
        --runtime-ld-library-path ${lib.escapeShellArg runtimeLdPath}${seaweedfsS3ConfigArg}
    ''
    else if action == "down"
    then ''
      python3 -m s6.runtime down \
        --mode ${lib.escapeShellArg cfg.infra} \
        --runtime-dir ${lib.escapeShellArg runtimeDir}
    ''
    else ''
      python3 -m s6.runtime ${lib.escapeShellArg action} \
        --mode ${lib.escapeShellArg cfg.infra}${profileArg} \
        --runtime-dir ${lib.escapeShellArg runtimeDir}
    '';

  mkBuildTask = mode: let
    cfg = modes.${mode};
  in
    mkTask ''
      set -euo pipefail
      ${mkBuild {
        inherit (cfg) buildDir variant;
        clangdFile = cfg.clangd;
        clangUmlFile = cfg.clangUml;
        timingsOutput = "${cfg.buildDir}/latest_build_times.json";
      }}
    '';

  mkUpTask = mode: let
    cfg = modes.${mode};
  in
    mkTask ''
      set -euo pipefail
      if [[ -f remote_compile.json ]]; then
        echo "proj:${mode}Up: remote compile is enabled; local runtime requires build/webshotd outputs, while remote outputs are mirrored under build/remote" >&2
        exit 1
      fi
      ${mkBuild {
        inherit (cfg) buildDir variant;
        clangdFile = cfg.clangd;
        clangUmlFile = cfg.clangUml;
        timingsOutput = "${cfg.buildDir}/latest_build_times.json";
      }}
      exec ${mkRuntime "up" mode null}
    '';

  mkRuntimeTask = action: mode: mkTask (mkRuntime action mode null);

  mkTestTask = mode: failFast: let
    up = mkRuntime "up" mode "test_infra";
    down = mkRuntime "down" mode null;
    failFastArg =
      if failFast
      then " --fail-fast"
      else "";
    runTests = ''
      python3 devenv/run_unit_tests.py --mode ${lib.escapeShellArg mode}${failFastArg}
      python3 devenv/run_testsuite_tests.py --mode ${lib.escapeShellArg mode}${failFastArg}
    '';
    testScript = ''
      set -euo pipefail
      ${mkBuild {
        inherit (modes.${mode}) buildDir variant;
        clangdFile = modes.${mode}.clangd;
        clangUmlFile = modes.${mode}.clangUml;
        timingsOutput = "${modes.${mode}.buildDir}/latest_build_times.json";
      }}
      cleanup() {
        ${down}
      }
      trap cleanup EXIT
      ${up}
      ${runTests}
    '';
    remoteTestScript = ''
      set -euo pipefail
      cd ${lib.escapeShellArg config.devenv.root}
      ${mkBuild {
        inherit (modes.${mode}) buildDir variant;
        clangdFile = modes.${mode}.clangd;
        clangUmlFile = modes.${mode}.clangUml;
        manageClangd = false;
        timingsOutput = "${modes.${mode}.buildDir}/latest_build_times.json";
      }}
      cleanup() {
        ${down}
      }
      trap cleanup EXIT
      ${up}
      ${runTests}
    '';
  in
    mkTask ''
      set -euo pipefail
      if [[ -f remote_compile.json ]]; then
        python3 devenv/remote_compile.py \
          --run-script ${lib.escapeShellArg remoteTestScript} \
          --sync-shadow-build-dir ${lib.escapeShellArg modes.${mode}.buildDir}
      else
        ${testScript}
      fi
    '';

  mkPgmigrate = mode: cmd: let
    cfg = modes.${mode};
  in
    mkTask ''
      set -euo pipefail
      python3 devenv/pgmigrate_task.py \
        --config-vars-source ${lib.escapeShellArg cfg.configVars} \
        --cmd ${lib.escapeShellArg cmd}
    '';
in {
  tasks."proj:devBuild" = mkBuildTask "dev";

  tasks."proj:tidyBuild" = mkTask ''
    set -euo pipefail
    ${mkBuild {
      buildDir = ctx.paths.build.tidy;
      clangdFile = ctx.paths.clangd.tidy;
      clangUmlFile = ctx.paths.clangUml.tidy;
      variant = ctx.variants.tidy;
      buildArgs = ["--" "-k" "0"];
    }}
  '';

  tasks."proj:devUp" = mkUpTask "dev";
  tasks."proj:devDown" = mkRuntimeTask "down" "dev";
  tasks."proj:devStatus" = (mkRuntimeTask "status" "dev") // {showOutput = true;};
  tasks."proj:devLogs" = (mkRuntimeTask "logs" "dev") // {showOutput = true;};
  tasks."proj:devTest" = mkTestTask "dev" false;
  tasks."proj:devTestFailFast" = mkTestTask "dev" true;
  tasks."proj:devDbMigrate" = mkPgmigrate "dev" "migrate";
  tasks."proj:devDbBaseline" = mkPgmigrate "dev" "baseline";

  tasks."proj:prodlikeBuild" = mkBuildTask "prodlike";
  tasks."proj:prodlikeUp" = mkUpTask "prodlike";
  tasks."proj:prodlikeDown" = mkRuntimeTask "down" "prodlike";
  tasks."proj:prodlikeStatus" = (mkRuntimeTask "status" "prodlike") // {showOutput = true;};
  tasks."proj:prodlikeLogs" = (mkRuntimeTask "logs" "prodlike") // {showOutput = true;};
  tasks."proj:prodlikeDbMigrate" = mkPgmigrate "prodlike" "migrate";
  tasks."proj:prodlikeDbBaseline" = mkPgmigrate "prodlike" "baseline";

  tasks."proj:ty" = mkTask ''
    set -euo pipefail
    ty check --no-progress
  '';
}
