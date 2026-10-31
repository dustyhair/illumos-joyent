#!/bin/bash
# Parent-only runtime in a fresh offline VM global zone.
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test "$(zonename)" = global
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
zoneadm -z lx-probe list -p | grep -q ':running:'
zonepath=$(zonecfg -z lx-probe info zonepath | awk '{print $2}')
base=/lab/leaderless-pgrp
run="$base/run.$(date -u +%Y%m%dT%H%M%SZ).$$"
mkdir "$run"
mkfifo "$run/completion"
exec 3<>"$run/completion"
(
    trap 'rc=$?; printf "%s\n" "$rc" >&3' EXIT
    for variant in static dynamic; do
        path="/test/leaderless-pgrp/lx/leaderless-pgrp.$variant"
        binary="$zonepath/root$path"
        test "$(digest -a sha256 "$binary" | awk '{print $NF}')" = "$(cat "$binary.sha256")"
        zlogin -S lx-probe "$path" </dev/null >"$run/$variant.raw.log" 2>&1
        cat "$run/$variant.raw.log"
        grep -qx 'RESULT failures=0' "$run/$variant.raw.log"
        test "$(grep -c ' PASS$' "$run/$variant.raw.log")" = 22
        if grep -q FAIL "$run/$variant.raw.log"; then exit 1; fi
    done
    echo LEADERLESS_PGRP_PASS
) </dev/null >"$run/controller.raw.log" 2>&1 &
worker=$!
echo "worker=$worker logdir=$run deadline=60s"
if IFS= read -r -t 60 result <&3; then
    wait "$worker" || true
    cat "$run/controller.raw.log"
    printf '%s\n' "$result" >"$run/result"
    test "$result" = 0
else
    echo 'FAIL deadline: inspect owned worker/zlogin/fixture before offline shutdown' | tee "$run/TIMEOUT"
    exit 124
fi
