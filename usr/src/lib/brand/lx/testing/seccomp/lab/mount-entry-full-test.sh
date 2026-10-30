#!/bin/bash
set -euo pipefail
test -f /lab/offline-lx-guest
/lab/mount-owner-test.sh </dev/null > /lab/mnt-entry-regressions.log 2>&1
echo MNT_ENTRY_REGRESSIONS_EXIT=0
/lab/mount-entry-test.sh </dev/null > /lab/mnt-entry-public.log 2>&1
echo MNT_ENTRY_PUBLIC_EXIT=0
echo LX_MOUNT_ENTRY_FULL_PASS
