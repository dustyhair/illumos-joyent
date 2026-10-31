#!/bin/bash
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test "$(zonename)" = global
test -f /lab/offline-lx-guest
bootparams | grep -qx 'lx_lab=true'
bash /lab/pid-cohort-smoke-test.sh </dev/null >/lab/lwp-production-baseline.log 2>&1
for bits in 32 64; do
    /lab/lwp-native/ordinary-native"$bits"
done >/lab/lwp-production-native.log 2>&1
zoneadm -z lx-probe boot
for variant in static dynamic; do
    zlogin -S lx-probe /test/lwp-ordinary/"$variant" >"/lab/lwp-production-$variant.log" 2>&1
    grep -qx 'ordinary LWP baseline PASS count=256' "/lab/lwp-production-$variant.log"
done
bash /lab/leaderless-pgrp/guest-controller.sh </dev/null >/lab/lwp-production-pgrp.log 2>&1
bash /lab/siginfo32-test.sh </dev/null >/lab/lwp-production-signals.log 2>&1
zoneadm -z lx-probe halt
bash /lab/pid-orphan-test.sh </dev/null >/lab/lwp-production-orphan.log 2>&1
bash /lab/pid-nested-test.sh </dev/null >/lab/lwp-production-nested.log 2>&1
echo LX_LWP_PRODUCTION_CHECKPOINT_PASS
