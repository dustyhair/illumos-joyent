#!/bin/bash
set -euo pipefail
test -f /lab/offline-lx-guest
bash /lab/pid-process-full-test.sh </dev/null > /lab/pid-cohort-regressions.log 2>&1
echo PID_COHORT_REGRESSIONS_EXIT=0
bash /lab/pid-cohort-test.sh </dev/null > /lab/pid-cohort-public.log 2>&1
echo LX_PID_COHORT_FULL_PASS
