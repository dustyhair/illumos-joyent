#!/bin/bash
set -euo pipefail
test -f /lab/offline-lx-guest
bash /lab/mount-private-full-test.sh </dev/null > /lab/mnt-tmpfs-regressions.log 2>&1
echo MOUNT_TMPFS_REGRESSIONS_EXIT=0
bash /lab/mount-tmpfs-test.sh </dev/null > /lab/mnt-tmpfs-public.log 2>&1
echo MOUNT_TMPFS_PUBLIC_EXIT=0
echo LX_MOUNT_TMPFS_FULL_PASS
