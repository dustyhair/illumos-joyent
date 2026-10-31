#!/bin/bash
set -euo pipefail
test -f /lab/offline-lx-guest
bash /lab/mount-statfs-full-test.sh </dev/null > /lab/mnt-freeze-regressions.log 2>&1
echo MOUNT_FREEZE_REGRESSIONS_EXIT=0
bash /lab/mount-freeze-test.sh </dev/null > /lab/mnt-freeze-public.log 2>&1
echo MOUNT_FREEZE_PUBLIC_EXIT=0
echo LX_MOUNT_FREEZE_FULL_PASS
