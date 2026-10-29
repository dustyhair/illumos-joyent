#!/bin/bash
# Only for this thread's disposable, networkless SmartOS VM.
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
test "$(zonename)" = global
echo LX_ENFORCEMENT_START
# The normal driver expects filtering to remain disabled by default.
/lab/guest-test.sh
/lab/bpf-test.sh
printf '%s\n' 'lx_brand`lx_seccomp_experimental/W 1' | mdb -kw
zoneadm -z lx-probe boot
zoneadm list -cv
set +e
zlogin -S lx-probe '/test/seccomp_probe'
probe_rc=$?
zlogin -S lx-probe '/test/enforcement_probe'
enforce_rc=$?
set -e
echo "LX_FILTER_PROBE_EXIT=$probe_rc"
echo "LX_ENFORCEMENT_PROBE_EXIT=$enforce_rc"
zoneadm -z lx-probe halt
test "$probe_rc" -eq 0
test "$enforce_rc" -eq 0
echo LX_ENFORCEMENT_PASS
