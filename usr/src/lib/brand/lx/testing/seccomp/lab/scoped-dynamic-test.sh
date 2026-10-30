#!/bin/bash
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
test "$(zonename)" = global
mkdir -p /zones/lx-probe-zfs/root/test/dynamic
gzip -dc /lab/scope.gz > /zones/lx-probe-zfs/root/test/dynamic/scope_caps
chmod 755 /zones/lx-probe-zfs/root/test/dynamic/scope_caps
zoneadm -z lx-probe boot
zlogin -S lx-probe /test/dynamic/scope_caps > /lab/scope-dynamic.log 2>&1 &
job=$!
for attempt in 1 2 3 4 5 6 7 8 9 10; do
    pid=$(sed -n 's/^SCOPE_READY_PID=//p' /lab/scope-dynamic.log)
    test -z "$pid" || break
    sleep 1
done
[[ "$pid" =~ ^[0-9]+$ ]]
printf '%s\0' "$pid" > /lab/scope-target.bin
/lab/ktest run -i /lab/scope-target.bin lx_namespace:user:scope_attach
kill -CONT "$pid"
for attempt in 1 2 3 4 5 6 7 8 9 10; do
    if ! kill -0 "$job" 2>/dev/null; then
        result=0
        wait "$job" || result=$?
        cat /lab/scope-dynamic.log
        zoneadm -z lx-probe halt
        exit "$result"
    fi
    sleep 1
done
cat /lab/scope-dynamic.log
echo SCOPE_DYNAMIC_TIMEOUT
exit 124
