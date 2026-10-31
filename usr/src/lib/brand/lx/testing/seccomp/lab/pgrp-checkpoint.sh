#!/bin/bash
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test "$(zonename)" = global
test -f /lab/offline-lx-guest
bootparams | grep -qx 'lx_lab=true'
bash /lab/pid-cohort-smoke-test.sh </dev/null > /lab/pgrp-baseline.log 2>&1
zoneadm -z lx-probe boot
bash /lab/leaderless-pgrp/guest-controller.sh </dev/null > /lab/pgrp-ordinary.log 2>&1
export PGRP_FRESH_DISPOSABLE_VM=reviewed-pgid-image
run=/lab/pgrp-zone/run.vm126.$$
printf '%s\n' "$run" > /lab/pgrp-zone-run-path.txt
bash /lab/pgrp-zone/lab/pgrp-zone-controller.sh setup "$run" </dev/null > /lab/pgrp-zone-setup.log 2>&1
bash /lab/pgrp-zone/lab/pgrp-zone-controller.sh run "$run" </dev/null > /lab/pgrp-zone-run.log 2>&1
for variant in static dynamic; do
    echo "PEER $variant"
    cat "$run/$variant.peer.stdout"
    cat "$run/$variant.peer.stderr"
done > /lab/pgrp-zone-peers.log
bash /lab/pgrp-zone/lab/pgrp-zone-controller.sh cleanup "$run" </dev/null > /lab/pgrp-zone-cleanup.log 2>&1
bash /lab/pid-orphan-test.sh </dev/null > /lab/pgrp-orphan.log 2>&1
bash /lab/pid-nested-test.sh </dev/null > /lab/pgrp-nested.log 2>&1
zoneadm -z lx-probe boot
bash /lab/siginfo32-test.sh </dev/null > /lab/pgrp-signal-regressions.log 2>&1
zoneadm -z lx-probe halt
echo LX_PGRP_CHECKPOINT_PASS
