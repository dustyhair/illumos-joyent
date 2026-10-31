#!/bin/bash
set -euo pipefail
test -f /lab/offline-lx-guest
bash /lab/pid-maps-full-test.sh </dev/null > /lab/pid-process-regressions.log 2>&1
echo PID_PROCESS_REGRESSIONS_EXIT=0
bash /lab/pid-process-test.sh </dev/null > /lab/pid-process-public.log 2>&1
echo LX_PID_PROCESS_FULL_PASS
