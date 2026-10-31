#!/bin/bash
set -euo pipefail
test -f /lab/offline-lx-guest
bash /lab/mount-retire-full-test.sh </dev/null > /lab/mnt-pivot-regressions.log 2>&1
echo MOUNT_PIVOT_REGRESSIONS_EXIT=0
bash /lab/mount-pivot-test.sh </dev/null > /lab/mnt-pivot-public.log 2>&1
echo MOUNT_PIVOT_PUBLIC_EXIT=0
echo LX_MOUNT_PIVOT_FULL_PASS
