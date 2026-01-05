#!/bin/sh
set -eu

# Prefer cachemgr if squidclient is available, otherwise fall back to checking
# that the proxy port is listening.

if command -v squidclient >/dev/null 2>&1; then
  squidclient -h 127.0.0.1 -p 3128 mgr:info >/dev/null 2>&1
  exit 0
fi

# 3128 in hex.
port_hex="0C38"

# /proc/net/tcp columns: local_address rem_address st ...
# Listening sockets have state 0A and remote address 00000000:0000.
if grep -Eq ":${port_hex}[[:space:]]+00000000:0000[[:space:]]+0A" /proc/net/tcp; then
  exit 0
fi

# /proc/net/tcp6 uses 32-hex-digit addresses.
if grep -Eq ":${port_hex}[[:space:]]+00000000000000000000000000000000:0000[[:space:]]+0A" /proc/net/tcp6; then
  exit 0
fi

exit 1
