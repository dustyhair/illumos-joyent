#!/bin/bash
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test "$(zonename)" = global
test -f /lab/offline-lx-guest
bootparams | grep -qx 'lx_lab=true'
bash /lab/pid-cohort-smoke-test.sh </dev/null >/lab/lwp-baseline.log 2>&1
for bits in 32 64; do
    /lab/lwp-native/ordinary-native"$bits"
done >/lab/lwp-native.log 2>&1
base=/lab/lwp-isolated-run
mkdir "$base"
mkfifo "$base/completion"
exec 3<>"$base/completion"
(
    trap 'rc=$?; printf "%s\n" "$rc" >&3' EXIT
    if bash /lab/lwp-isolated-test.sh; then
        cat /lab/lwp-isolated-results/controller.log
    else
        rc=$?
        cat /lab/lwp-isolated-results/controller.log
        exit "$rc"
    fi
) </dev/null >/lab/lwp-isolated.log 2>&1 &
worker=$!
echo "LWP failure worker=$worker deadline=240s"
if IFS= read -r -t 240 result <&3; then
    wait "$worker" || true
    test "$result" = 0
else
    echo 'FAIL LWP controller deadline; preserve worker and VM for inspection'
    exit 124
fi
zoneadm -z lx-probe boot
zlogin -S lx-probe /test/lwp-ordinary/static >/lab/lwp-linux-static.log 2>&1
zlogin -S lx-probe /test/lwp-ordinary/dynamic >/lab/lwp-linux-dynamic.log 2>&1
grep -qx 'ordinary LWP baseline PASS count=256' /lab/lwp-linux-static.log
grep -qx 'ordinary LWP baseline PASS count=256' /lab/lwp-linux-dynamic.log
bash /lab/leaderless-pgrp/guest-controller.sh </dev/null >/lab/lwp-pgrp.log 2>&1
bash /lab/siginfo32-test.sh </dev/null >/lab/lwp-signals.log 2>&1
zoneadm -z lx-probe halt
echo LX_LWP_ISOLATED_CHECKPOINT_PASS
