{
  pkgs,
  config,
  inputs,
  ...
}: let
  common = import ../devenv/lib.nix {inherit pkgs config inputs;};
  nodejs = common.nodejs;
  npmDeps = pkgs.importNpmLock.buildNodeModules {
    npmRoot = ./.;
    inherit nodejs;
  };
  crawlerdPkg = pkgs.buildNpmPackage {
    pname = "crawlerd";
    version = "0.1.0";
    src = pkgs.lib.cleanSource ./.;

    inherit nodejs;
    npmDepsHash = "sha256-HIzKBQZ80EHLXfU/LPY9K68r3U47rw5WmebMAUzkIWI=";
    npmBuildScript = "build";
    npmInstallFlags = ["--include=dev"];
    npm_config_package_lock_only = "false";

    nativeBuildInputs = [pkgs.makeWrapper];

    installPhase = ''
      runHook preInstall

      npm prune --omit=dev --no-save

      mkdir -p $out/bin $out/libexec/crawlerd
      cp -r dist node_modules package.json $out/libexec/crawlerd/

      makeWrapper ${nodejs}/bin/node $out/bin/crawlerd \
        --prefix PATH : "${pkgs.lib.makeBinPath [pkgs.bubblewrap pkgs.chromium]}" \
        --add-flags $out/libexec/crawlerd/dist/src/server.js

      runHook postInstall
    '';

    meta = {
      description = "Internal crawler daemon for webshot";
      mainProgram = "crawlerd";
      platforms = pkgs.lib.platforms.linux;
    };
  };
in {
  outputs.crawlerd = crawlerdPkg;

  packages = [
    crawlerdPkg
    nodejs
    pkgs.chromium
    pkgs.bubblewrap
    pkgs.importNpmLock.hooks.linkNodeModulesHook
  ];

  enterShell = ''
    crawlerdNodeModules="${config.devenv.root}/crawlerd/node_modules"
    if [ -e "$crawlerdNodeModules" ] && [ ! -L "$crawlerdNodeModules" ]; then
      echo "crawlerd/node_modules must be managed by Nix" >&2
      return 1
    fi
    ln -sfn ${npmDeps}/node_modules "$crawlerdNodeModules"
  '';

  tasks."crawlerd:check" = {
    cwd = common.crawlerd.root;
    exec = common.crawlerd.checkCommand;
  };

  tasks."crawlerd:build" = {
    cwd = common.crawlerd.root;
    exec = common.crawlerd.buildCommand;
  };

  tasks."crawlerd:test" = {
    cwd = common.crawlerd.root;
    exec = common.crawlerd.testCommand;
  };

  tasks."crawlerd:openapi" = {
    cwd = common.crawlerd.root;
    exec = common.crawlerd.openapiCommand;
  };
}
