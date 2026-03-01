{
  pkgs,
  tag,
}: let
  squid = pkgs.squid;

  securityFileCertgen = pkgs.writeShellScript "security_file_certgen" ''
    set -euo pipefail
    for p in \
      "${squid}/libexec/security_file_certgen" \
      "${squid}/libexec/security_file_certgen64" \
      "${squid}/libexec/squid/security_file_certgen" \
      "${squid}/lib/squid/security_file_certgen" \
      "${squid}/libexec/squid/security_file_certgen64" \
      "${squid}/lib/squid/security_file_certgen64"
    do
      if [[ -x "$p" ]]; then
        exec "$p" "$@"
      fi
    done
    echo "Missing security_file_certgen in ${squid}" >&2
    exit 2
  '';

  updateCaCertificates = pkgs.writeShellScript "update-ca-certificates" ''
    set -euo pipefail
    out="/etc/ssl/certs/ca-certificates.crt"
    base="${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt"
    if [[ ! -r "$base" ]]; then
      echo "Missing base CA bundle: $base" >&2
      exit 2
    fi
    tmp="''${out}.new"
    cat "$base" >"$tmp"

    if [[ -d /usr/local/share/ca-certificates ]]; then
      shopt -s nullglob
      for f in /usr/local/share/ca-certificates/*.crt; do
        cat "$f" >>"$tmp"
        printf '\n' >>"$tmp"
      done
    fi

    mv -f "$tmp" "$out"
  '';
in
  pkgs.dockerTools.buildLayeredImage {
    name = "localhost/squid";
    inherit tag;

    contents = [
      pkgs.bash
      pkgs.coreutils
      pkgs.gnugrep
      pkgs.cacert
      squid
    ];

    extraCommands = ''
                set -euo pipefail

                mkdir -p bin etc etc/ssl/certs usr/lib/squid usr/libexec/squid usr/local/bin usr/local/share/ca-certificates usr/sbin var/lib/squid etc/squid tmp var/log
                chmod 1777 tmp

                ln -sf ${pkgs.bash}/bin/bash bin/bash
                ln -sf ${pkgs.bash}/bin/bash bin/sh

                ln -sf ${securityFileCertgen} usr/libexec/squid/security_file_certgen
                ln -sf /usr/libexec/squid/security_file_certgen usr/lib/squid/security_file_certgen

                ln -sf ${updateCaCertificates} usr/sbin/update-ca-certificates

                found=""
                for p in \
                  "${squid}/libexec/security_file_certgen" \
                  "${squid}/libexec/security_file_certgen64" \
                  "${squid}/libexec/squid/security_file_certgen" \
                  "${squid}/lib/squid/security_file_certgen" \
                  "${squid}/libexec/squid/security_file_certgen64" \
                  "${squid}/lib/squid/security_file_certgen64"
                do
                  if [ -x "$p" ]; then
                    found=1
                    break
                  fi
                done
                if [ -z "$found" ]; then
                  echo "Missing security_file_certgen in ${squid}" >&2
                  exit 2
                fi

                cp ${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt etc/ssl/certs/ca-certificates.crt

                cat >etc/passwd <<'EOF'
      root:x:0:0:root:/root:/bin/sh
      proxy:x:13:13:proxy:/var/lib/squid:/bin/sh
      squid:x:23:23:squid:/var/lib/squid:/bin/sh
      EOF

                cat >etc/group <<'EOF'
      root:x:0:
      proxy:x:13:
      squid:x:23:
      EOF

                cat >etc/nsswitch.conf <<'EOF'
      passwd: files
      group: files
      shadow: files

      hosts: files dns
      networks: files
      EOF

                cat >etc/os-release <<'EOF'
      NAME="NixOS"
      ID=nixos
      PRETTY_NAME="NixOS (dockerTools)"
      EOF
    '';

    config = {
      Env = [
        "PATH=/usr/sbin:${pkgs.coreutils}/bin:${pkgs.gnugrep}/bin:${pkgs.bash}/bin:${squid}/sbin:${squid}/bin"
        "SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt"
      ];
      Cmd = ["/bin/sh"];
    };
  }
