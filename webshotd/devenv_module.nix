{
  pkgs,
  config,
  inputs,
  ...
}: let
  ctx = import ../devenv/ctx.nix {inherit pkgs config inputs;};
  lib = ctx.nix.lib;
in {
  outputs.webshot = ctx.mkWebshot {
    userver = ctx.drv.userverDbg;
  };

  packages =
    ctx.sets.buildNative
    ++ ctx.sets.runtime
    ++ [
      ctx.drv.repoPy
      ctx.toolchain.cc
      ctx.nix.llvmPackages_22.llvm
      ctx.nix.llvmPackages_22.clang-tools
      ctx.drv.includeWhatYouUse
      ctx.drv.userverDbg
      ctx.drv.unialgo
      ctx.drv.testsuite
      ctx.drv.pgmigrate
    ]
    ++ ctx.sets.userverLibs
    ++ [ctx.drv.testSan ctx.drv.testCov]
    ++ (with ctx.nix; [
      git
      gdb
      ty
      ungoogled-chromium
      bubblewrap
    ]);

  env.CMAKE_PREFIX_PATH = ctx.paths.cmakePrefix;

  env.PKG_CONFIG_PATH = lib.makeSearchPath "lib/pkgconfig" [
    ctx.nix.cryptopp.dev
  ];

  env.USERVER_PYTHON = "${ctx.drv.repoPy}/bin/python3";
  env.USERVER_PYTHON_PATH = "${ctx.drv.repoPy}/bin/python3";
  env.USERVER_DIR = "${ctx.drv.userverDbg}/lib/cmake/userver";

  # Expose the yandex-taxi-testsuite Python package so pytest_userver
  # can import `testsuite` (for chaos, pgsql helpers, etc.).
  env.PYTHONPATH =
    "${config.devenv.root}:"
    + (lib.makeSearchPath ctx.nix.python3.sitePackages [
      ctx.drv.testsuite
    ]);
}
