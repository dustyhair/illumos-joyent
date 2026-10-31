#!/bin/bash
set -euo pipefail
test -f /lab/offline-lx-guest
bash /lab/pid-cohort-smoke-test.sh </dev/null > /lab/pid-orphan-cohort-smoke.log 2>&1
bash /lab/pid-orphan-test.sh </dev/null > /lab/pid-orphan-smoke-public.log 2>&1
echo LX_PID_ORPHAN_SMOKE_PASS
