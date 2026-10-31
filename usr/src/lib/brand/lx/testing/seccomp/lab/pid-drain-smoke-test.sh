#!/bin/bash
set -euo pipefail
test -f /lab/offline-lx-guest
bash /lab/pid-cohort-test.sh </dev/null > /lab/pid-drain-smoke-cohort.log 2>&1
bash /lab/pid-drain-test.sh </dev/null > /lab/pid-drain-smoke-cases.log 2>&1
bash /lab/pid-zombie-test.sh </dev/null > /lab/pid-drain-smoke-zombie.log 2>&1
bash /lab/pid-wait-test.sh </dev/null > /lab/pid-drain-smoke-wait.log 2>&1
echo LX_PID_DRAIN_SMOKE_PASS
