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
zlogin -S lx-probe '/test/signal_probe'
signal_rc=$?
zlogin -S lx-probe '/test/fork_probe'
fork_rc=$?
zlogin -S lx-probe '/test/exec_probe'
exec_rc=$?
zlogin -S lx-probe '/test/thread_probe'
thread_rc=$?
boundary_rc=0
if test -f /usr/lib/lx_brand.so.1; then
    zlogin -S lx-probe '/test/boundary32_probe'
    boundary_rc=$?
else
    echo LX_BOUNDARY32_SKIP_NO_NATIVE_EMULATOR
fi
set -e
echo "LX_FILTER_PROBE_EXIT=$probe_rc"
echo "LX_ENFORCEMENT_PROBE_EXIT=$enforce_rc"
echo "LX_SIGNAL_PROBE_EXIT=$signal_rc"
echo "LX_FORK_PROBE_EXIT=$fork_rc"
echo "LX_EXEC_PROBE_EXIT=$exec_rc"
echo "LX_THREAD_PROBE_EXIT=$thread_rc"
echo "LX_BOUNDARY32_EXIT=$boundary_rc"
zoneadm -z lx-probe halt
test "$probe_rc" -eq 0
test "$enforce_rc" -eq 0
test "$signal_rc" -eq 0
test "$fork_rc" -eq 0
test "$exec_rc" -eq 0
test "$thread_rc" -eq 0
test "$boundary_rc" -eq 0
echo LX_ENFORCEMENT_PASS
