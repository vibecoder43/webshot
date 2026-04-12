{
  pkgs,
  config,
  inputs,
  ...
}: let
  common = import ./lib.nix {inherit pkgs config inputs;};

  modeConfigs = {
    dev = {
      buildDir = common.buildDirs.san;
      clangdConfig = common.clangdConfigs.san;
      buildVariant = common.buildVariants.san;
      infraMode = "dev";
      configVarsSource = "${config.devenv.root}/webshotd/config/config_vars.dev.yaml";
    };

    prodlike = {
      buildDir = common.buildDirs.san;
      clangdConfig = common.clangdConfigs.san;
      buildVariant = common.buildVariants.san;
      infraMode = "prodlike";
      configVarsSource = "${config.devenv.root}/webshotd/config/config_vars.prodlike.yaml";
    };
  };

  runtimeLdLibraryPath = common.lib.makeLibraryPath common.testLibs;

  mkWebshotBuildCommandForMode = mode: let
    cfg = modeConfigs.${mode};
  in ''
    build_dir=${common.lib.escapeShellArg cfg.buildDir}
    before_log=$(mktemp /tmp/webshot_build_times_before.XXXXXX)
    started_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    started_ms=$(date +%s%3N)
    if [[ -f "$build_dir/.ninja_log" ]]; then
      cp "$build_dir/.ninja_log" "$before_log"
    else
      : > "$before_log"
    fi

    configure_started_ms=$(date +%s%3N)
    configure_exit_code=0
    { ${common.mkConfigureTaskCommands cfg.buildDir cfg.clangdConfig cfg.buildVariant} } || configure_exit_code=$?
    configure_finished_ms=$(date +%s%3N)
    configure_time_ms=$((configure_finished_ms - configure_started_ms))

    build_started_ms=$(date +%s%3N)
    build_exit_code=0
    if [[ "$configure_exit_code" -eq 0 ]]; then
      cmake --build "$build_dir" || build_exit_code=$?
    fi
    build_finished_ms=$(date +%s%3N)
    build_time_ms=$((build_finished_ms - build_started_ms))

    build_exit_code_final=$build_exit_code
    if [[ "$configure_exit_code" -ne 0 ]]; then
      build_exit_code_final=$configure_exit_code
    fi

    build_status=success
    if [[ "$build_exit_code_final" -ne 0 ]]; then
      build_status=failure
    fi

    finished_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    finished_ms=$(date +%s%3N)
    wall_time_ms=$((finished_ms - started_ms))
    python3 webshotd/collect_build_times.py \
      --build-dir "$build_dir" \
      --before-log "$before_log" \
      --after-log "$build_dir/.ninja_log" \
      --output "$build_dir/latest_build_times.json" \
      --status "$build_status" \
      --started-at "$started_at" \
      --finished-at "$finished_at" \
      --wall-time-ms "$wall_time_ms" \
      --configure-time-ms "$configure_time_ms" \
      --build-time-ms "$build_time_ms"
    rm -f "$before_log"

    if [[ "$build_exit_code_final" -ne 0 ]]; then
      exit "$build_exit_code_final"
    fi
  '';

  mkBuildCommandsForMode = mode: mkWebshotBuildCommandForMode mode;

  mkBuildTaskForMode = mode: {
    cwd = config.devenv.root;
    exec = ''
      set -euo pipefail
      ${mkWebshotBuildCommandForMode mode}
    '';
  };

  mkRuntimeCommand = action: mode: serviceProfile: let
    cfg = modeConfigs.${mode};
    serviceProfileArg =
      if serviceProfile == null
      then ""
      else " \\\n        --service-profile ${common.lib.escapeShellArg serviceProfile}";
  in
    if action == "up"
    then ''
      python3 -m s6.runtime up \
        --mode ${common.lib.escapeShellArg cfg.infraMode}${serviceProfileArg} \
        --binary-path ${common.lib.escapeShellArg "${cfg.buildDir}/runtime_root/webshotd/webshotd"} \
        --config-vars-source ${common.lib.escapeShellArg cfg.configVarsSource} \
        --runtime-ld-library-path ${common.lib.escapeShellArg runtimeLdLibraryPath}
    ''
    else if action == "down"
    then ''
      python3 -m s6.runtime down \
        --mode ${common.lib.escapeShellArg cfg.infraMode}
    ''
    else ''
      python3 -m s6.runtime ${common.lib.escapeShellArg action} \
        --mode ${common.lib.escapeShellArg cfg.infraMode}${serviceProfileArg}
    '';

  mkRuntimeTask = action: mode: {
    cwd = config.devenv.root;
    exec = mkRuntimeCommand action mode null;
  };

  mkUpTask = mode: {
    cwd = config.devenv.root;
    exec = ''
      set -euo pipefail
      ${mkBuildCommandsForMode mode}
      ${mkRuntimeCommand "up" mode null}
    '';
  };

  mkTestTask = mode: let
    cfg = modeConfigs.${mode};
    upCmd = mkRuntimeCommand "up" mode "test_infra";
    downCmd = mkRuntimeCommand "down" mode null;
  in {
    cwd = config.devenv.root;
    exec = ''
      set -euo pipefail
      ${mkWebshotBuildCommandForMode mode}
      cleanup() {
        ${downCmd}
      }
      trap cleanup EXIT
      ${upCmd}
      ${common.testSan}/bin/test_san
    '';
  };

  mkPgmigrateTask = mode: cmd: let
    cfg = modeConfigs.${mode};
  in {
    cwd = config.devenv.root;
    exec = ''
      set -euo pipefail
      python3 devenv/pgmigrate_task.py \
        --config-vars-source ${common.lib.escapeShellArg cfg.configVarsSource} \
        --cmd ${common.lib.escapeShellArg cmd}
    '';
  };
in {
  tasks."proj:devBuild" = mkBuildTaskForMode "dev";

  tasks."proj:tidyBuild" = {
    cwd = config.devenv.root;
    exec = ''
      set -euo pipefail
      ${common.mkConfigureTaskCommands
        common.buildDirs.tidy
        common.clangdConfigs.tidy
        common.buildVariants.tidy}
      cmake --build ${common.lib.escapeShellArg common.buildDirs.tidy} -- -k 0
    '';
  };

  tasks."proj:devUp" = mkUpTask "dev";

  tasks."proj:devDown" = mkRuntimeTask "down" "dev";

  tasks."proj:devStatus" =
    (mkRuntimeTask "status" "dev")
    // {
      showOutput = true;
    };

  tasks."proj:devLogs" =
    (mkRuntimeTask "logs" "dev")
    // {
      showOutput = true;
    };

  tasks."proj:devTest" = mkTestTask "dev";
  tasks."proj:devDbMigrate" = mkPgmigrateTask "dev" "migrate";
  tasks."proj:devDbBaseline" = mkPgmigrateTask "dev" "baseline";

  tasks."proj:prodlikeBuild" = mkBuildTaskForMode "prodlike";

  tasks."proj:prodlikeUp" = mkUpTask "prodlike";

  tasks."proj:prodlikeDown" = mkRuntimeTask "down" "prodlike";

  tasks."proj:prodlikeStatus" =
    (mkRuntimeTask "status" "prodlike")
    // {
      showOutput = true;
    };

  tasks."proj:prodlikeLogs" =
    (mkRuntimeTask "logs" "prodlike")
    // {
      showOutput = true;
    };

  tasks."proj:prodlikeDbMigrate" = mkPgmigrateTask "prodlike" "migrate";
  tasks."proj:prodlikeDbBaseline" = mkPgmigrateTask "prodlike" "baseline";

  tasks."proj:ty" = {
    cwd = config.devenv.root;
    exec = ''
      set -euo pipefail
      ty check --no-progress
    '';
  };
}
