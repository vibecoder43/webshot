{
  pkgs,
  config,
  inputs,
  ...
}: let
  ctx = import ../devenv/ctx.nix {inherit pkgs config inputs;};
  lib = ctx.nix.lib;
in {
  outputs.webshot = ctx.mkProjPkg {
    userver = ctx.drv.userver;
    variant = ctx.variants.release;
  };

  packages =
    ctx.sets.buildNative
    ++ ctx.sets.devTools
    ++ ctx.sets.runtime
    ++ ctx.sets.runtimeTools
    ++ [
      ctx.drv.repoPy
      ctx.toolchain.cc
      ctx.nix.llvmPackages_22.llvm
      ctx.nix.llvmPackages_22.clang-tools
      ctx.nix.clang-uml
      ctx.drv.includeWhatYouUse
      ctx.drv.userverDbg
      ctx.drv.unialgo
      ctx.drv.testsuite
      ctx.drv.pgmigrate
    ]
    ++ ctx.sets.userverLibs
    ++ [ctx.drv.testCov];

  # Expose the yandex-taxi-testsuite Python package so pytest_userver
  # can import `testsuite` (for chaos, pgsql helpers, etc.).
  env.PYTHONPATH =
    "${config.devenv.root}:"
    + (lib.makeSearchPath ctx.nix.python3.sitePackages [
      ctx.drv.testsuite
    ]);
}
