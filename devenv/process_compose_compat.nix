{
  pkgs,
  lib,
  config,
  ...
}:
lib.mkIf (config.process.manager.implementation == "process-compose" && config.processes == {}) {
  # Upstream devenv v1.11.2 can read configFile before the process-compose
  # module populates it. Provide an empty config only for repos with no
  # devenv-managed processes so shell evaluation still succeeds.
  process.managers.process-compose.configFile = lib.mkDefault (
    (pkgs.formats.yaml {}).generate "process-compose-empty.yaml" {
      version = "0.5";
      processes = {};
    }
  );
}
