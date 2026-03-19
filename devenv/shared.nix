{
  pkgs,
  config,
  inputs,
  ...
}: let
  common = import ./lib.nix {inherit pkgs config inputs;};
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
      settings.global.excludes = common.treefmtExcludesFromGitignore;
    };
  };

  difftastic.enable = true;

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
      package = common.python;
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
      package = common.pkgsWithOverlay.sqlfluff;
      language = "system";
      files = "\\.sql$";
    };

    ty = {
      enable = true;
      entry = "ty check --no-progress";
      package = common.pkgsWithOverlay.ty;
      language = "system";
      types = ["python"];
      pass_filenames = false;
    };
  };
}
