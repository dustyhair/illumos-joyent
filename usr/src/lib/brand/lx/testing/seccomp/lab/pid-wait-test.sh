#!/bin/bash
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
test "$(zonename)" = global
# Controller-level deadline; timeout is failure and preserves the logs.
controller=$$
( sleep 55; echo PID_WAIT_TIMEOUT >&2; kill -TERM "$controller" ) </dev/null &
watchdog=$!
trap 'kill -TERM "$watchdog" 2>/dev/null || true' EXIT
trap 'exit 124' TERM
/lab/native-wait-regress
zoneadm -z lx-probe boot
for variant in static dynamic; do
    zlogin -S lx-probe "/test/wait/$variant/wait_regress" </dev/null
    zlogin -S lx-probe "/test/wait/$variant/wait_regress --clone-filter" </dev/null
done
zoneadm -z lx-probe halt
echo LX_PID_WAIT_PASS
