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

  mkBuild = mode: let
    cfg = modes.${mode};
    configureFingerprint = ctx.mkConfigureFingerprint {
      inherit (cfg) buildDir variant;
    };
  in ''
    build_dir=${lib.escapeShellArg cfg.buildDir}
    configure_fingerprint=${lib.escapeShellArg configureFingerprint}
    configure_fingerprint_file="$build_dir/.configure-fingerprint"
    configure_fresh_flag=
    configure_mode=normal
    if [[ -f "$configure_fingerprint_file" ]]; then
      previous_configure_fingerprint=$(<"$configure_fingerprint_file")
      if [[ "$previous_configure_fingerprint" != "$configure_fingerprint" ]]; then
        configure_fresh_flag=--fresh
        configure_mode=fresh
        printf 'CMake configure spec changed; reconfiguring with --fresh\n'
      fi
    elif [[ -f "$build_dir/CMakeCache.txt" || -d "$build_dir/CMakeFiles" ]]; then
      configure_fresh_flag=--fresh
      configure_mode=fresh
      printf 'Existing build dir has no configure fingerprint; reconfiguring with --fresh\n'
    fi

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
    if [[ -n "$configure_fresh_flag" ]]; then
      set +e
      ${ctx.mkConfigure {
      buildDir = cfg.buildDir;
      clangdFile = cfg.clangd;
      variant = cfg.variant;
      fresh = true;
    }}
      configure_exit_code=$?
      set -e
    else
      set +e
      ${ctx.mkConfigure {
      buildDir = cfg.buildDir;
      clangdFile = cfg.clangd;
      variant = cfg.variant;
      fresh = false;
    }}
      configure_exit_code=$?
      set -e
    fi
    configure_finished_ms=$(date +%s%3N)
    configure_time_ms=$((configure_finished_ms - configure_started_ms))
    if [[ "$configure_exit_code" -eq 0 ]]; then
      printf '%s\n' "$configure_fingerprint" > "$configure_fingerprint_file"
    fi

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

  mkBuildTask = mode:
    mkTask ''
      set -euo pipefail
      ${mkBuild mode}
    '';

  mkUpTask = mode:
    mkTask ''
      set -euo pipefail
      ${mkBuild mode}
      exec ${mkRuntime "up" mode null}
    '';

  mkRuntimeTask = action: mode: mkTask (mkRuntime action mode null);

  mkTestTask = mode: let
    up = mkRuntime "up" mode "test_infra";
    down = mkRuntime "down" mode null;
  in
    mkTask ''
      set -euo pipefail
      ${mkBuild mode}
      cleanup() {
        ${down}
      }
      trap cleanup EXIT
      ${up}
      ${ctx.drv.testSan}/bin/test_san
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
    ${ctx.mkConfigure {
      buildDir = ctx.paths.build.tidy;
      clangdFile = ctx.paths.clangd.tidy;
      variant = ctx.variants.tidy;
      fresh = false;
    }}
    cmake --build ${lib.escapeShellArg ctx.paths.build.tidy} -- -k 0
  '';

  tasks."proj:devUp" = mkUpTask "dev";
  tasks."proj:devDown" = mkRuntimeTask "down" "dev";
  tasks."proj:devStatus" = (mkRuntimeTask "status" "dev") // {showOutput = true;};
  tasks."proj:devLogs" = (mkRuntimeTask "logs" "dev") // {showOutput = true;};
  tasks."proj:devTest" = mkTestTask "dev";
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
