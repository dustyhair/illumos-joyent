#!/bin/bash
set -euo pipefail
test -f /lab/offline-lx-guest
bash /lab/mount-freeze-full-test.sh </dev/null > /lab/mnt-detach-regressions.log 2>&1
echo MOUNT_DETACH_REGRESSIONS_EXIT=0
bash /lab/mount-detach-test.sh </dev/null > /lab/mnt-detach-public.log 2>&1
echo MOUNT_DETACH_PUBLIC_EXIT=0
echo LX_MOUNT_DETACH_FULL_PASS
