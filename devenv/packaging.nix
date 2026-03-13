{
  pkgs,
  config,
  inputs,
  ...
}: let
  common = import ./lib.nix {inherit pkgs config inputs;};
in {
  outputs.packaging = {
    cctz = common.cctzPkgs.default;
    pgmigrate = common.pgmigratePkgs.default;
    uniAlgo = common.uniAlgoPkgs.default;
    userverRelease = common.userverPkgs.userver-release;
    userverDebugAddrUb = common.userverPkgs.userver-debug-addr-ub;
    yandexTaxiTestsuite = common.yttsPkgs.default;
  };
}
