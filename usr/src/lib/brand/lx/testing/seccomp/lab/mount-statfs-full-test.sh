#!/bin/bash
set -euo pipefail
test -f /lab/offline-lx-guest
bash /lab/mount-tmpfs-full-test.sh </dev/null > /lab/mnt-statfs-regressions.log 2>&1
echo MOUNT_STATFS_REGRESSIONS_EXIT=0
bash /lab/mount-statfs-test.sh </dev/null > /lab/mnt-statfs-public.log 2>&1
echo MOUNT_STATFS_PUBLIC_EXIT=0
echo LX_MOUNT_STATFS_FULL_PASS
