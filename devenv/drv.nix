{
  inputs,
  nix,
  paths,
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

  userverDeps = callPkg ./pkgs/userver/deps.nix {
    python = nix.python3;
  };
  inherit (userverDeps) userverBuildPython userverLibs;

  repoPython = callPkg ./pkgs/repo_python.nix {
    python = nix.python3;
  };

  userverPkgs = callSrcPkg ./pkgs/userver.nix "userver" {
    inherit toolchain userverBuildPython userverLibs;
  };
  pgmigrate = callSrcPkg ./pkgs/pgmigrate.nix "pgmigrate" {};
  testsuite = callSrcPkg ./pkgs/testsuite.nix "testsuite" {
    inherit pgmigrate;
  };
  seaweedfs = callPkg ./pkgs/seaweedfs.nix {
    inherit inputs;
  };
  includeWhatYouUse = callPkg ./pkgs/include-what-you-use.nix {};
  unialgo = callSrcPkg ./pkgs/uni-algo.nix "unialgo" {
    inherit toolchain;
  };

  asanUcontextWarningSedScript = "/WARNING: ASan doesn't fully support makecontext\\/swapcontext functions and may produce false positives in some cases!/d";

  ctestVerboseWithAsanUcontextWarningFiltered = ''
    set +e
    ctest --progress --output-on-failure -V "$@" 2>&1 \
      | ${nix.gnused}/bin/sed -e "${asanUcontextWarningSedScript}"
    ctest_exit_code=$?
    set -e

    # CTest logs are written before stdout filtering; scrub them too so CI
    # artifacts (Testing/Temporary/LastTest.log) are clean.
    shopt -s nullglob
    for log in Testing/Last*.log* Testing/Temporary/Last*.log*; do
      if [[ -f "$log" ]]; then
        ${nix.gnused}/bin/sed -i -e "${asanUcontextWarningSedScript}" "$log"
      fi
    done
    shopt -u nullglob

    exit "$ctest_exit_code"
  '';

  rapidocVersion = "9.3.8";
  rapidoc = nix.stdenvNoCC.mkDerivation {
    pname = "rapidoc-assets";
    version = rapidocVersion;
    src = nix.fetchurl {
      url = "https://registry.npmjs.org/rapidoc/-/rapidoc-${rapidocVersion}.tgz";
      hash = "sha512-eCYEbr1Xr8OJZvVCw8SXl9zBCRoLJbhNGuG5IZTHq/RWAOq/O4MafUCuFEyZHsrhLrlUcGZMa64pyhpib8fQKQ==";
    };
    nativeBuildInputs = with nix; [gnutar gzip];
    dontConfigure = true;
    dontBuild = true;
    unpackPhase = ''
      tar -xzf "$src"
    '';
    installPhase = ''
      mkdir -p "$out"
      cp package/dist/rapidoc-min.js "$out/rapidoc-min.js"
    '';
  };

  webUi = nix.stdenvNoCC.mkDerivation {
    pname = "web-ui-vendor";
    version = "0";

    nativeBuildInputs = with nix; [gnutar gzip];
    dontUnpack = true;
    dontConfigure = true;
    dontBuild = true;

    installPhase = let
      htmxVersion = "2.0.8";
      htmxSrc = nix.fetchurl {
        url = "https://registry.npmjs.org/htmx.org/-/htmx.org-${htmxVersion}.tgz";
        hash = "sha512-fm297iru0iWsNJlBrjvtN7V9zjaxd+69Oqjh4F/Vq9Wwi2kFisLcrLCiv5oBX0KLfOX/zG8AUo9ROMU5XUB44Q==";
      };

      jsonEncVersion = "2.0.3";
      jsonEncSrc = nix.fetchurl {
        url = "https://registry.npmjs.org/htmx-ext-json-enc/-/htmx-ext-json-enc-${jsonEncVersion}.tgz";
        hash = "sha512-8Oc6MNOvhSXR78dY7CoiYU3ZgaCPNfE9UHXbHg4g6pd+8fqbZIVFxg9XVijxtGYoF/irf3eTOnEgIPV0KVX7Iw==";
      };

      clientSideTemplatesVersion = "2.0.2";
      clientSideTemplatesSrc = nix.fetchurl {
        url = "https://registry.npmjs.org/htmx-ext-client-side-templates/-/htmx-ext-client-side-templates-${clientSideTemplatesVersion}.tgz";
        hash = "sha512-d2dA5HOJuMPN+QmAa4MtriTc1OZaaidG/UK/uwDsKJzstMFae2M7tFgykFXOjuFoV2XNCcexr3p+a9p5Jb54Dg==";
      };

      responseTargetsVersion = "2.0.4";
      responseTargetsSrc = nix.fetchurl {
        url = "https://registry.npmjs.org/htmx-ext-response-targets/-/htmx-ext-response-targets-${responseTargetsVersion}.tgz";
        hash = "sha512-Q/yfH0N2A40j903mr6ldGV3qLWMQeufROylYIbYQLBGrCpmydflxXmQwDOEhEWdPnBTgiHofkAo0gQ3S1BmyxQ==";
      };

      nunjucksVersion = "3.2.4";
      nunjucksSrc = nix.fetchurl {
        url = "https://registry.npmjs.org/nunjucks/-/nunjucks-${nunjucksVersion}.tgz";
        hash = "sha512-26XRV6BhkgK0VOxfbU5cQI+ICFUtMLixv1noZn1tGU38kQH5A5nmmbk/O45xdyBhD1esk47nKrY0mvQpZIhRjQ==";
      };

      replaywebpageVersion = "2.4.4";
      replaywebpageSrc = nix.fetchurl {
        url = "https://registry.npmjs.org/replaywebpage/-/replaywebpage-${replaywebpageVersion}.tgz";
        hash = "sha512-LgkOOO4Mrvdq77Ly7nzMZKxAlESGKJkwb919Mq8b35TyqQJiZo3fs35QYiwZLCFGj4w41BVM/SlfseJusCwspQ==";
      };
    in ''
      set -euo pipefail
      mkdir -p "$out" "$out/replaywebpage"

      tmp=$(mktemp -d)
      mkdir -p "$tmp/htmx" "$tmp/json_enc" "$tmp/cst" "$tmp/rt" "$tmp/nunjucks" "$tmp/replaywebpage"

      tar -xzf "${htmxSrc}" -C "$tmp/htmx"
      tar -xzf "${jsonEncSrc}" -C "$tmp/json_enc"
      tar -xzf "${clientSideTemplatesSrc}" -C "$tmp/cst"
      tar -xzf "${responseTargetsSrc}" -C "$tmp/rt"
      tar -xzf "${nunjucksSrc}" -C "$tmp/nunjucks"
      tar -xzf "${replaywebpageSrc}" -C "$tmp/replaywebpage"

      cp "$tmp/htmx/package/dist/htmx.min.js" "$out/htmx.min.js"
      cp "$tmp/json_enc/package/dist/json-enc.min.js" "$out/json-enc.min.js"
      cp "$tmp/cst/package/dist/client-side-templates.min.js" "$out/client-side-templates.min.js"
      cp "$tmp/rt/package/dist/response-targets.min.js" "$out/response-targets.min.js"
      cp "$tmp/nunjucks/package/browser/nunjucks.min.js" "$out/nunjucks.min.js"
      cp "$tmp/replaywebpage/package/ui.js" "$out/replaywebpage/ui.js"
      cp "$tmp/replaywebpage/package/sw.js" "$out/replaywebpage/sw.js"

      ${nix.perl}/bin/perl -0pi -e 's@let f=function\(t\)\{const e=\["\\.warc","\\.warc\\.gz","\\.cdx","\\.cdxj","\\.har","\\.json","\\.wacz"\];for\(const r of e\)if\(t\.endsWith\(r\)\)return r;if\(t\.endsWith\("\\.wacz\\.zip"\)\)return"\\.wacz"\}\(n\.sourceName\);@let f="wacz"===n.extraConfig?.sourceType?".wacz":function(t){const e=[".warc",".warc.gz",".cdx",".cdxj",".har",".json",".wacz"];for(const r of e)if(t.endsWith(r))return r;if(t.endsWith(".wacz.zip"))return".wacz"}(n.sourceName);@' "$out/replaywebpage/sw.js"

      # Optional source maps reduce noisy devtools warnings when serving the UI locally.
      for f in \
        "$tmp/htmx/package/dist/htmx.min.js.map" \
        "$tmp/json_enc/package/dist/json-enc.min.js.map" \
        "$tmp/cst/package/dist/client-side-templates.min.js.map" \
        "$tmp/rt/package/dist/response-targets.min.js.map" \
        "$tmp/nunjucks/package/browser/nunjucks.min.js.map"; do
        if [[ -f "$f" ]]; then
          cp "$f" "$out/$(basename "$f")"
        fi
      done

      rm -rf "$tmp"
    '';
  };
in {
  includeWhatYouUse = includeWhatYouUse;
  pgmigrate = pgmigrate;
  rapidoc = rapidoc;
  seaweedfs = seaweedfs;
  testCov = nix.writeShellScriptBin "test_cov" ''
    set -euo pipefail
    export LD_LIBRARY_PATH='${lib.makeLibraryPath sets.testLibs}'
    cmake --build ${paths.build.cov} --target coverage-html
  '';
  testSan = nix.writeShellScriptBin "test_san" ''
    set -euo pipefail
    export LD_LIBRARY_PATH='${lib.makeLibraryPath sets.testLibs}'
    cd ${paths.build.san}
    ${ctestVerboseWithAsanUcontextWarningFiltered}
  '';
  testsuite = testsuite;
  unialgo = unialgo;
  inherit userverBuildPython userverLibs;
  userver = userverPkgs.userver-release;
  userverDbg = userverPkgs.userver-debug-addr-ub;
  repoPy = repoPython;
  webUi = webUi;
}
