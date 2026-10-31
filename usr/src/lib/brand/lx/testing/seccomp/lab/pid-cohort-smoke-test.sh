#!/bin/bash
set -euo pipefail
test -f /lab/offline-lx-guest
bash /lab/pid-process-smoke-test.sh </dev/null > /lab/pid-cohort-process-smoke.log 2>&1
bash /lab/pid-cohort-test.sh </dev/null > /lab/pid-cohort-smoke-public.log 2>&1
echo LX_PID_COHORT_SMOKE_PASS
