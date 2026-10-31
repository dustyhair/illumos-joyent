#!/bin/bash
set -euo pipefail
test -f /lab/offline-lx-guest
bash /lab/pid-orphan-full-test.sh </dev/null > /lab/pid-drain-full-prior.log 2>&1
bash /lab/pid-drain-test.sh </dev/null > /lab/pid-drain-full-cases.log 2>&1
bash /lab/pid-zombie-test.sh </dev/null > /lab/pid-drain-full-zombie.log 2>&1
bash /lab/pid-wait-test.sh </dev/null > /lab/pid-drain-full-wait.log 2>&1
echo LX_PID_DRAIN_FULL_PASS
