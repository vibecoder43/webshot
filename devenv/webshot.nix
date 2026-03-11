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

  mkCrawlerdTaskCommand = command: ''
    cd ${common.lib.escapeShellArg common.crawlerd.root}
    ${command}
    cd ${common.lib.escapeShellArg config.devenv.root}
  '';

  mkWebshotBuildCommandForMode = mode: let
    cfg = modeConfigs.${mode};
  in ''
    ${common.mkConfigureTaskCommands cfg.buildDir cfg.clangdConfig cfg.buildVariant}
    cmake --build ${common.lib.escapeShellArg cfg.buildDir}
  '';

  mkBuildCommandsForMode = mode: ''
    ${mkCrawlerdTaskCommand common.crawlerd.buildCommand}
    ${mkWebshotBuildCommandForMode mode}
  '';

  mkBuildTaskForMode = mode: {
    cwd = config.devenv.root;
    exec = ''
      set -euo pipefail
      ${mkBuildCommandsForMode mode}
    '';
  };

  mkRuntimeCommand = action: mode: let
    cfg = modeConfigs.${mode};
  in ''
    python3 -m s6.runtime ${common.lib.escapeShellArg action} \
      --mode ${common.lib.escapeShellArg cfg.infraMode} \
      --binary-path ${common.lib.escapeShellArg "${cfg.buildDir}/webshotd"} \
      --config-vars-source ${common.lib.escapeShellArg cfg.configVarsSource} \
      --runtime-ld-library-path ${common.lib.escapeShellArg runtimeLdLibraryPath}
  '';

  mkRuntimeTask = action: mode: {
    cwd = config.devenv.root;
    exec = mkRuntimeCommand action mode;
  };

  mkUpTask = mode: {
    cwd = config.devenv.root;
    exec = ''
      set -euo pipefail
      ${mkBuildCommandsForMode mode}
      ${mkRuntimeCommand "up" mode}
    '';
  };

  mkTestTask = mode: let
    cfg = modeConfigs.${mode};
    upCmd = mkRuntimeCommand "up" mode;
    downCmd = mkRuntimeCommand "down" mode;
  in {
    cwd = config.devenv.root;
    exec = ''
      set -euo pipefail
      ${mkCrawlerdTaskCommand common.crawlerd.buildCommand}
      ${mkCrawlerdTaskCommand common.crawlerd.testCommand}
      ${mkWebshotBuildCommandForMode mode}
      cleanup() {
        ${downCmd}
      }
      trap cleanup EXIT
      ${upCmd}
      # The testsuite starts its own webshotd and crawlerd instances.
      s6-svc -d /tmp/webshot/dev/s6-scan/crawlerd
      s6-svc -d /tmp/webshot/dev/s6-scan/webshotd
      s6-svwait -d /tmp/webshot/dev/s6-scan/crawlerd
      s6-svwait -d /tmp/webshot/dev/s6-scan/webshotd
      export LD_LIBRARY_PATH=${common.lib.escapeShellArg runtimeLdLibraryPath}
      cd ${common.lib.escapeShellArg cfg.buildDir}
      ctest --progress --output-on-failure -V
    '';
  };
in {
  tasks."webshot:devBuild" = mkBuildTaskForMode "dev";

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
}
