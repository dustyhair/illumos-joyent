#!/bin/bash
set -euo pipefail
test -f /lab/offline-lx-guest
bash /lab/pid-identity-full-test.sh </dev/null > /lab/pid-maps-regressions.log 2>&1
echo PID_MAPS_REGRESSIONS_EXIT=0
bash /lab/pid-maps-test.sh </dev/null > /lab/pid-maps-public.log 2>&1
echo LX_PID_MAPS_FULL_PASS
