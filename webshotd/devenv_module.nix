{
  pkgs,
  config,
  inputs,
  ...
}: let
  common = import ../devenv/lib.nix {inherit pkgs config inputs;};

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

  mkBuildTaskForMode = mode: let
    cfg = modeConfigs.${mode};
  in
    common.mkBuildTask cfg.buildDir cfg.clangdConfig cfg.buildVariant;

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

  mkUpTask = mode: let
    buildTask = mkBuildTaskForMode mode;
  in {
    cwd = config.devenv.root;
    exec = ''
      ${buildTask.exec}
      ${mkRuntimeCommand "up" mode}
    '';
  };

  mkTestTask = mode: let
    cfg = modeConfigs.${mode};
    buildTask = mkBuildTaskForMode mode;
    checkCmd = mkRuntimeCommand "check" mode;
    upCmd = mkRuntimeCommand "up" mode;
    downCmd = mkRuntimeCommand "down" mode;
  in {
    cwd = config.devenv.root;
    exec = ''
      set -euo pipefail
      ${buildTask.exec}
      if ${checkCmd} >/dev/null 2>&1; then
        echo "webshot:devTest cannot run while webshot:devUp owns localhost:8080/8081; run devenv tasks run webshot:devDown first" >&2
        exit 1
      fi
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
  outputs.webshot = common.mkWebshotOutput {
    userverPkg = common.userverPkgs.userver-debug-addr-ub;
  };

  packages =
    common.buildDeps.native
    ++ common.buildDeps.runtime
    ++ [
      common.chaoticPython
      common.toolchain.cc
      common.llvm21.llvm
      common.llvm21.clang-tools
      common.userverPkgs.userver-debug-addr-ub
      common.uniAlgoPkgs.default
      common.yttsPkgs.default
    ]
    ++ common.userverDeps
    ++ [common.webshotTestSan common.webshotTestCov]
    ++ (with common.pkgsWithOverlay; [git gdb nssTools]);

  env.CMAKE_PREFIX_PATH = common.lib.makeSearchPath "lib/cmake" [
    common.userverPkgs.userver-debug-addr-ub
    common.pkgsWithOverlay.boost183.dev
    common.pkgsWithOverlay.fmt.dev
    common.pkgsWithOverlay.zstd.dev
    common.pkgsWithOverlay.cctz
    common.pkgsWithOverlay.yaml-cpp
  ];

  env.PKG_CONFIG_PATH = common.lib.makeSearchPath "lib/pkgconfig" [
    common.pkgsWithOverlay.cryptopp.dev
  ];

  env.USERVER_PYTHON = "${common.chaoticPython}/bin/python3";
  env.USERVER_PYTHON_PATH = "${common.chaoticPython}/bin/python3";
  env.USERVER_DIR = "${common.userverPkgs.userver-debug-addr-ub}/lib/cmake/userver";

  # Expose the yandex-taxi-testsuite Python package so pytest_userver
  # can import `testsuite` (for chaos, pgsql helpers, etc.).
  env.PYTHONPATH =
    "${config.devenv.root}:"
    + (common.lib.makeSearchPath common.python.sitePackages [
      common.yttsPkgs.default
    ]);

  tasks."webshot:devBuild" = mkBuildTaskForMode "dev";

  tasks."webshot:devUp" = mkUpTask "dev";

  tasks."webshot:devDown" = mkRuntimeTask "down" "dev";

  tasks."webshot:devStatus" = mkRuntimeTask "status" "dev";

  tasks."webshot:devLogs" = mkRuntimeTask "logs" "dev";

  tasks."webshot:devTest" = mkTestTask "dev";

  tasks."webshot:prodlikeBuild" = mkBuildTaskForMode "prodlike";

  tasks."webshot:prodlikeUp" = mkUpTask "prodlike";

  tasks."webshot:prodlikeDown" = mkRuntimeTask "down" "prodlike";

  tasks."webshot:prodlikeStatus" = mkRuntimeTask "status" "prodlike";

  tasks."webshot:prodlikeLogs" = mkRuntimeTask "logs" "prodlike";
}
