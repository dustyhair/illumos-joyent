#!/bin/bash
set -euo pipefail
test -f /lab/offline-lx-guest
bash /lab/mount-pivot-full-test.sh </dev/null > /lab/nsclone-regressions.log 2>&1
echo NSCLONE_REGRESSIONS_EXIT=0
bash /lab/mount-pivot-shared-test.sh </dev/null > /lab/nsclone-pivot-shared.log 2>&1
for phase in enabled disabled mount-zero user-zero; do
    bash /lab/namespace-clone-test.sh "$phase" </dev/null > "/lab/nsclone-$phase.log" 2>&1
    echo "NSCLONE_PHASE_EXIT phase=$phase result=0"
done
for attr in max-user-namespaces max-mount-namespaces; do
    zonecfg -z lx-probe "select attr name=$attr; set value=64; end;"
done
echo LX_NSCLONE_FULL_PASS
