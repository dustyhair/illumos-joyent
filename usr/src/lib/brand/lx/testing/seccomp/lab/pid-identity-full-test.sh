#!/bin/bash
set -euo pipefail
test -f /lab/offline-lx-guest
bash /lab/namespace-clone-full-test.sh </dev/null > /lab/pid-identity-regressions.log 2>&1
echo PID_IDENTITY_REGRESSIONS_EXIT=0
bash /lab/pid-identity-test.sh </dev/null > /lab/pid-identity-public.log 2>&1
echo LX_PID_IDENTITY_FULL_PASS
