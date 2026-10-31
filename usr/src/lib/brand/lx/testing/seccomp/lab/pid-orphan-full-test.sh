#!/bin/bash
set -euo pipefail
test -f /lab/offline-lx-guest
bash /lab/pid-cohort-full-test.sh </dev/null > /lab/pid-orphan-regressions.log 2>&1
echo PID_ORPHAN_REGRESSIONS_EXIT=0
bash /lab/pid-orphan-test.sh </dev/null > /lab/pid-orphan-public.log 2>&1
echo LX_PID_ORPHAN_FULL_PASS
