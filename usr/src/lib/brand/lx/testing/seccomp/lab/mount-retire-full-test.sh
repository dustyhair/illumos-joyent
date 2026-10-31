#!/bin/bash
set -euo pipefail
test -f /lab/offline-lx-guest
bash /lab/mount-detach-full-test.sh </dev/null > /lab/mnt-retire-regressions.log 2>&1
echo MOUNT_RETIRE_REGRESSIONS_EXIT=0
bash /lab/mount-retire-test.sh </dev/null > /lab/mnt-retire-public.log 2>&1
echo MOUNT_RETIRE_PUBLIC_EXIT=0
echo LX_MOUNT_RETIRE_FULL_PASS
