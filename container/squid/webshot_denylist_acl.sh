#!/bin/bash
set -euo pipefail

[[ $# -eq 2 ]] || { echo "usage: $0 <webshot-host> <webshot-port>" >&2; exit 2; }

webshotHost="$1"
webshotPort="$2"
webshotPath="/v1/denylist/check"

while IFS= read -r line; do
  url="${line%% *}"
  if [[ -z "${url}" ]]; then
    echo "OK"
    continue
  fi

  if { exec 3<>"/dev/tcp/${webshotHost}/${webshotPort}"; } 2>/dev/null; then
    body="${url}"
    bodyLen="${#body}"
    printf 'POST %s HTTP/1.1\r\nHost: %s\r\nContent-Type: text/plain\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s' \
      "${webshotPath}" "${webshotHost}" "${bodyLen}" "${body}" >&3

    statusLine=""
    IFS= read -r statusLine <&3 || true
    exec 3<&- 3>&-

    if [[ "${statusLine}" == HTTP/*\ 204\ * || "${statusLine}" == HTTP/*\ 204$ ]]; then
      echo "ERR"
    else
      echo "OK"
    fi
  else
    echo "OK"
  fi
done
