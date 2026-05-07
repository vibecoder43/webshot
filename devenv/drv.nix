{
  inputs,
  nix,
  paths,
  s6Src,
  sets,
  srcs,
  toolchain,
}: let
  lib = nix.lib;

  callPkg = path: args: import path ({pkgs = nix;} // args);
  callSrcPkg = path: name: args:
    callPkg path ({
        src = srcs.${name};
      }
      // args);

  userverDeps = callPkg ./pkg/userver/deps.nix {
    python = nix.python3;
    inherit srcs;
  };
  inherit (userverDeps) userverBuildPython userverLibs;

  repoPython = callPkg ./pkg/repo_python.nix {
    inherit s6Src srcs;
    python = nix.python3;
  };

  userverPkgs = callSrcPkg ./pkg/userver.nix "userver" {
    inherit toolchain userverBuildPython userverLibs;
  };
  pgmigrate = callSrcPkg ./pkg/pgmigrate.nix "pgmigrate" {};
  testsuite = callSrcPkg ./pkg/testsuite.nix "testsuite" {
    inherit pgmigrate;
  };
  seaweedfs = callPkg ./pkg/seaweedfs.nix {
    inherit inputs;
  };
  includeWhatYouUse = callPkg ./pkg/include-what-you-use.nix {};
  boostSml = callSrcPkg ./pkg/boost-sml.nix "boostsml" {};
  unialgo = callSrcPkg ./pkg/uni-algo.nix "unialgo" {
    inherit toolchain;
  };
  rapidoc = nix.stdenvNoCC.mkDerivation {
    name = "rapidoc-assets";
    src = srcs.rapidoc;
    dontUnpack = true;
    dontConfigure = true;
    dontBuild = true;
    installPhase = ''
      mkdir -p "$out"
      cp "$src/dist/rapidoc-min.js" "$out/rapidoc-min.js"
    '';
  };

  webUi = nix.stdenvNoCC.mkDerivation {
    name = "web-ui-vendor";

    dontUnpack = true;
    dontConfigure = true;
    dontBuild = true;

    installPhase = let
      htmxSrc = srcs.htmx;
      jsonEncSrc = srcs.htmxJsonEnc;
      clientSideTemplatesSrc = srcs.htmxClientSideTemplates;
      responseTargetsSrc = srcs.htmxResponseTargets;
      nunjucksSrc = srcs.nunjucks;
      replaywebpageSrc = srcs.replaywebpage;
    in ''
      set -euo pipefail
      mkdir -p "$out" "$out/replaywebpage"

      cp "${htmxSrc}/dist/htmx.min.js" "$out/htmx.min.js"
      cp "${jsonEncSrc}/dist/json-enc.min.js" "$out/json-enc.min.js"
      cp "${clientSideTemplatesSrc}/dist/client-side-templates.min.js" "$out/client-side-templates.min.js"
      cp "${responseTargetsSrc}/dist/response-targets.min.js" "$out/response-targets.min.js"
      cp "${nunjucksSrc}/browser/nunjucks.min.js" "$out/nunjucks.min.js"
      cp "${replaywebpageSrc}/index.html" "$out/replaywebpage/index.html"
      cp "${replaywebpageSrc}/ui.js" "$out/replaywebpage/ui.js"
      cp "${replaywebpageSrc}/sw.js" "$out/replaywebpage/sw.js"

      # Optional source maps reduce noisy devtools warnings when serving the UI locally.
      for f in \
        "${htmxSrc}/dist/htmx.min.js.map" \
        "${jsonEncSrc}/dist/json-enc.min.js.map" \
        "${clientSideTemplatesSrc}/dist/client-side-templates.min.js.map" \
        "${responseTargetsSrc}/dist/response-targets.min.js.map" \
        "${nunjucksSrc}/browser/nunjucks.min.js.map"; do
        if [[ -f "$f" ]]; then
          cp "$f" "$out/$(basename "$f")"
        fi
      done
    '';
  };
in {
  boostSml = boostSml;
  includeWhatYouUse = includeWhatYouUse;
  pgmigrate = pgmigrate;
  rapidoc = rapidoc;
  seaweedfs = seaweedfs;
  testCov = nix.writeShellScriptBin "test_cov" ''
    set -euo pipefail
    export LD_LIBRARY_PATH='${lib.makeLibraryPath sets.testLibs}'
    cmake --build ${paths.build.cov} --target coverage-html
  '';
  testsuite = testsuite;
  unialgo = unialgo;
  inherit userverBuildPython userverLibs;
  userver = userverPkgs.userver-release;
  userverDbg = userverPkgs.userver-debug-addr-ub;
  inherit (repoPython) repoPy repoToolPy;
  webUi = webUi;
}
