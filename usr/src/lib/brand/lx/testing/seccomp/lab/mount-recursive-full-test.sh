#!/bin/bash
set -euo pipefail
test -f /lab/offline-lx-guest
bash /lab/mount-bind-full-test.sh </dev/null > /lab/mnt-recursive-regressions.log 2>&1
echo MOUNT_RECURSIVE_REGRESSIONS_EXIT=0
bash /lab/mount-recursive-test.sh </dev/null > /lab/mnt-recursive-public.log 2>&1
echo MOUNT_RECURSIVE_PUBLIC_EXIT=0
echo LX_MOUNT_RECURSIVE_FULL_PASS
