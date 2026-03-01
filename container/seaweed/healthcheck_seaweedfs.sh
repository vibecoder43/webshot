#!/bin/sh
set -eu

/bin/busybox wget -q -T 2 -O /dev/null http://127.0.0.1:9333/cluster/healthz
/bin/busybox wget -S -T 2 -O /dev/null http://127.0.0.1:8333/ 2>&1 | /bin/busybox grep -Eq 'HTTP/[0-9.]+ [234][0-9]{2}'
