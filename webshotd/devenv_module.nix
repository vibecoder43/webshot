{
  pkgs,
  config,
  inputs,
  ...
}: let
  common = import ../devenv/lib.nix {inherit pkgs config inputs;};
in {
  outputs.webshot = common.mkWebshotOutput {
    userverPkg = common.userverPkgs.userver-debug-addr-ub;
  };

  packages =
    common.buildDeps.native
    ++ common.buildDeps.runtime
    ++ [
      common.userverHelperPython
      common.toolchain.cc
      common.llvm21.llvm
      common.llvm21.clang-tools
      common.pkgsWithOverlay.include-what-you-use
      common.userverPkgs.userver-debug-addr-ub
      common.uniAlgoPkgs.default
      common.yttsPkgs.default
    ]
    ++ common.userverDeps
    ++ [common.webshotTestSan common.webshotTestCov]
    ++ (with common.pkgsWithOverlay; [
      git
      gdb
      ty
      ungoogled-chromium
      bubblewrap
    ]);

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

  env.USERVER_PYTHON = "${common.userverHelperPython}/bin/python3";
  env.USERVER_PYTHON_PATH = "${common.userverHelperPython}/bin/python3";
  env.USERVER_DIR = "${common.userverPkgs.userver-debug-addr-ub}/lib/cmake/userver";

  # Expose the yandex-taxi-testsuite Python package so pytest_userver
  # can import `testsuite` (for chaos, pgsql helpers, etc.).
  env.PYTHONPATH =
    "${config.devenv.root}:"
    + (common.lib.makeSearchPath common.python.sitePackages [
      common.yttsPkgs.default
    ]);
}
