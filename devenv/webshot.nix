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
    ${common.mkConfigureTaskCommands cfg.buildDir cfg.clangdConfig cfg.buildVariant}
    cmake --build ${common.lib.escapeShellArg cfg.buildDir}
  '';

  mkBuildCommandsForMode = mode: mkWebshotBuildCommandForMode mode;

  mkBuildTaskForMode = mode: {
    cwd = config.devenv.root;
    exec = ''
      set -euo pipefail
      ${mkBuildCommandsForMode mode}
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
      export LD_LIBRARY_PATH=${common.lib.escapeShellArg runtimeLdLibraryPath}
      cd ${common.lib.escapeShellArg cfg.buildDir}
      ctest --progress --output-on-failure -V
    '';
  };
in {
  tasks."webshot:devBuild" = mkBuildTaskForMode "dev";

  tasks."webshot:tidyBuild" = {
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

  tasks."webshot:devUp" = mkUpTask "dev";

  tasks."webshot:devDown" = mkRuntimeTask "down" "dev";

  tasks."webshot:devStatus" =
    (mkRuntimeTask "status" "dev")
    // {
      showOutput = true;
    };

  tasks."webshot:devLogs" =
    (mkRuntimeTask "logs" "dev")
    // {
      showOutput = true;
    };

  tasks."webshot:devTest" = mkTestTask "dev";

  tasks."webshot:prodlikeBuild" = mkBuildTaskForMode "prodlike";

  tasks."webshot:prodlikeUp" = mkUpTask "prodlike";

  tasks."webshot:prodlikeDown" = mkRuntimeTask "down" "prodlike";

  tasks."webshot:prodlikeStatus" =
    (mkRuntimeTask "status" "prodlike")
    // {
      showOutput = true;
    };

  tasks."webshot:prodlikeLogs" =
    (mkRuntimeTask "logs" "prodlike")
    // {
      showOutput = true;
    };

  tasks."webshot:ty" = {
    cwd = config.devenv.root;
    exec = ''
      set -euo pipefail
      ty check --no-progress
    '';
  };
}
