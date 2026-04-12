{...}: {
  imports = let
    # Work around devenv eval caching: ensure changes in imported modules invalidate evaluation.
    import_files = [
      ./devenv/shared.nix
      ./devenv/packaging.nix
      ./devenv/webshot.nix
      ./webshotd/devenv_module.nix
    ];
    # Include non-module nix files that affect imported modules (but must not be imported themselves).
    deps_files =
      import_files
      ++ [
        ./devenv/lib.nix
      ];
    deps = builtins.concatStringsSep "" (map builtins.readFile deps_files);
  in
    builtins.seq deps import_files;
}
