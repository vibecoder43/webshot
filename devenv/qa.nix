{
  pkgs,
  config,
  inputs,
  ...
}: let
  ctx = import ./ctx.nix {inherit pkgs config inputs;};
in {
  cachix.enable = true;

  treefmt = {
    enable = true;
    config = {
      programs = {
        alejandra.enable = true;
        clang-format.enable = true;
        cmake-format.enable = true;
        ruff-format.enable = true;
        sqlfluff.enable = true;
        yamlfmt.enable = true;
      };
      settings = {
        global.excludes = ctx.treefmtExcludes;
        formatter."cmake-format".includes = [
          "*.cmake"
          "CMakeLists.txt"
          "**/CMakeLists.txt"
        ];
      };
    };
  };

  tasks."git:blame-ignore-revs" = {
    cwd = config.devenv.root;
    before = ["devenv:enterShell"];
    status = ''
      set -euo pipefail
      git rev-parse --is-inside-work-tree >/dev/null 2>&1 || exit 0
      git config --local --get-all blame.ignoreRevsFile 2>/dev/null | grep -Fxq .git-blame-ignore-revs
    '';
    exec = ''
      set -euo pipefail
      git rev-parse --is-inside-work-tree >/dev/null 2>&1 || exit 0
      git config --local --add blame.ignoreRevsFile .git-blame-ignore-revs
    '';
  };

  git-hooks.hooks = {
    treefmt = {
      enable = true;
      settings.formatters = builtins.attrValues config.treefmt.config.build.programs;
    };
    ruff.enable = true;
    shellcheck = {
      enable = true;
      args = ["-x"];
    };
    unicode_hygiene = {
      enable = true;
      entry = "python3 check_unicode_hygiene.py";
      package = ctx.nix.python3;
      language = "system";
      files = "";
    };
    yamllint = {
      enable = true;
      settings.configuration = ''
        extends: relaxed
        rules:
          line-length: disable
      '';
    };
    sqlfluff-lint = {
      enable = true;
      entry = "sqlfluff lint";
      package = ctx.nix.sqlfluff;
      language = "system";
      files = "\\.sql$";
    };

    ty = {
      enable = true;
      entry = "ty check --no-progress";
      package = ctx.nix.ty;
      language = "system";
      types = ["python"];
      pass_filenames = false;
    };
  };
}
