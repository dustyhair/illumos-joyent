#!/bin/bash
set -euo pipefail
test -f /lab/offline-lx-guest
bash /lab/mount-recursive-full-test.sh </dev/null > /lab/mnt-private-regressions.log 2>&1
echo MOUNT_PRIVATE_REGRESSIONS_EXIT=0
bash /lab/mount-private-test.sh </dev/null > /lab/mnt-private-public.log 2>&1
echo MOUNT_PRIVATE_PUBLIC_EXIT=0
echo LX_MOUNT_PRIVATE_FULL_PASS
