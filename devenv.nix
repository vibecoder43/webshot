{...}: {
  imports = [
    ./devenv/qa.nix
    ./devenv/outputs.nix
    ./devenv/tasks.nix
    ./webshotd/devenv_module.nix
  ];
}
