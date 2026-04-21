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
      variant = ctx.variants.san;
      infra = "dev";
      configVars = "${config.devenv.root}/webshotd/config/config_vars.dev.yaml";
    };

    prodlike = {
      buildDir = ctx.paths.build.san;
      clangd = ctx.paths.clangd.san;
      variant = ctx.variants.san;
      infra = "prodlike";
      configVars = "${config.devenv.root}/webshotd/config/config_vars.prodlike.yaml";
    };
  };

  runtimeLdPath = lib.makeLibraryPath ctx.sets.testLibs;

  mkRepeatedFlagArgs = flag: args:
    lib.concatMapStringsSep " \\\n      " (arg: "${flag}=${lib.escapeShellArg arg}") args;

  mkBuild = {
    buildDir,
    variant,
    forceConfigureFresh ? false,
    timingsOutput ? null,
    buildArgs ? [],
  }: let
    configureArgv = ctx.mkConfigureArgv {
      inherit buildDir variant;
      fresh = false;
    };
    configureFingerprint = ctx.mkConfigureFingerprint {
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
    forceConfigureFreshArg =
      if forceConfigureFresh
      then " \\\n      --force-configure-fresh"
      else "";
  in ''
    python3 devenv/build_task.py \
      --build-dir ${lib.escapeShellArg buildDir} \
      --configure-fingerprint ${lib.escapeShellArg configureFingerprint} \
      ${mkRepeatedFlagArgs "--configure-arg" configureArgv}${forceConfigureFreshArg}${buildArgFlags}${timingsArgs}
  '';

  mkTask = exec: {
    cwd = config.devenv.root;
    inherit exec;
  };

  mkRuntime = action: mode: profile: let
    cfg = modes.${mode};
    profileArg =
      if profile == null
      then ""
      else " \\\n        --service-profile ${lib.escapeShellArg profile}";
  in
    if action == "up"
    then ''
      python3 -m s6.runtime up \
        --mode ${lib.escapeShellArg cfg.infra}${profileArg} \
        --binary-path ${lib.escapeShellArg "${cfg.buildDir}/runtime_root/webshotd/webshotd_wrapper"} \
        --config-vars-source ${lib.escapeShellArg cfg.configVars} \
        --runtime-ld-library-path ${lib.escapeShellArg runtimeLdPath}
    ''
    else if action == "down"
    then ''
      python3 -m s6.runtime down \
        --mode ${lib.escapeShellArg cfg.infra}
    ''
    else ''
      python3 -m s6.runtime ${lib.escapeShellArg action} \
        --mode ${lib.escapeShellArg cfg.infra}${profileArg}
    '';

  mkBuildTask = mode: let
    cfg = modes.${mode};
  in
    mkTask ''
      set -euo pipefail
      ${mkBuild {
        inherit (cfg) buildDir variant;
        timingsOutput = "${cfg.buildDir}/latest_build_times.json";
      }}
    '';

  mkUpTask = mode: let
    cfg = modes.${mode};
  in
    mkTask ''
      set -euo pipefail
      if [[ -f .devenv/remote_compile.json ]]; then
        echo "proj:${mode}Up: remote compile is enabled; local runtime requires build/webshotd outputs, while remote outputs are mirrored under build/remote" >&2
        exit 1
      fi
      ${mkBuild {
        inherit (cfg) buildDir variant;
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
    testScript = ''
      set -euo pipefail
      ${mkBuild {
        inherit (modes.${mode}) buildDir variant;
        timingsOutput = "${modes.${mode}.buildDir}/latest_build_times.json";
      }}
      cleanup() {
        ${down}
      }
      trap cleanup EXIT
      ${up}
      python3 devenv/run_unit_tests.py --mode ${lib.escapeShellArg mode}${failFastArg}
      python3 devenv/run_testsuite_tests.py --mode ${lib.escapeShellArg mode}${failFastArg}
    '';
    remoteTestScript = ''
      set -euo pipefail
      cd ${lib.escapeShellArg config.devenv.root}
      ${mkBuild {
        inherit (modes.${mode}) buildDir variant;
        timingsOutput = "${modes.${mode}.buildDir}/latest_build_times.json";
      }}
      cleanup() {
        ${down}
      }
      trap cleanup EXIT
      ${up}
      build_dir=${lib.escapeShellArg modes.${mode}.buildDir}
      log_dir="$build_dir/Testing/Temporary"
      mkdir -p "$log_dir"
      ctest \
        --progress \
        --output-on-failure \
        -V \
        -E '^testsuite-testsuite-tests$' \
        --no-tests=error \
        --output-log "$log_dir/unit_tests.log" \
        --test-dir "$build_dir"${lib.optionalString failFast " --stop-on-failure"}
      testsuite_dir="$build_dir/test"
      python3 "$testsuite_dir/runtests-testsuite-tests" \
        --service-logs-pretty \
        -vv${lib.optionalString failFast " -x"} \
        >"$log_dir/testsuite.log" \
        2>&1
    '';
  in
    mkTask ''
      set -euo pipefail
      if [[ -f .devenv/remote_compile.json ]]; then
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
