#!/bin/bash
set -euo pipefail
test -f /lab/offline-lx-guest
bash /lab/mount-entry-full-test.sh </dev/null > /lab/mnt-bind-regressions.log 2>&1
echo MOUNT_BIND_REGRESSIONS_EXIT=0
bash /lab/mount-bind-test.sh </dev/null > /lab/mnt-bind-public.log 2>&1
echo MOUNT_BIND_PUBLIC_EXIT=0
echo LX_MOUNT_BIND_FULL_PASS
