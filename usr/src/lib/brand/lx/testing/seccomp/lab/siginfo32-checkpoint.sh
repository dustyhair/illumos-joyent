#!/bin/bash
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
test "$(zonename)" = global
bash /lab/pid-cohort-smoke-test.sh </dev/null > /lab/siginfo32-baseline.log 2>&1
zoneadm -z lx-probe boot
tracer=
cleanup() {
    if test -n "$tracer"; then
        kill -TERM "$tracer" 2>/dev/null || true
        wait "$tracer" || true
    fi
}
trap cleanup EXIT
dtrace -x nolibs -s /lab/queued-zero-alloc.d </dev/null > /lab/queued-zero-alloc.log 2>&1 &
tracer=$!
ready=0
for ((n=0; n<15; n++)); do
    if grep -q '^QUEUED_ZERO_TRACE_READY$' /lab/queued-zero-alloc.log; then
        ready=1
        break
    fi
    sleep 1
done
test "$ready" = 1
bash /lab/siginfo32-test.sh </dev/null > /lab/siginfo32-controller.log 2>&1
kill -TERM "$tracer"
wait "$tracer"
tracer=
grep -qx 'QUEUED_ZERO_ALLOC_SEEN=0' /lab/queued-zero-alloc.log
awk '/^QUEUED_HELPER_CALLS zero=1 count=/ {split($3,a,"="); if (a[2] >= 16384) pass=1} END {exit !pass}' /lab/queued-zero-alloc.log
zoneadm -z lx-probe halt
echo LX_SIGINFO32_SMOKE_PASS
