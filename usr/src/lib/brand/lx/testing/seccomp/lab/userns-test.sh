#!/bin/bash
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
test "$(zonename)" = global
/lab/namespace-test.sh
for suite in maps capabilities native lifetime; do
    /lab/ktest run "lx_namespace:user:$suite"
done
/lab/ktest run lx_namespace:user:cap_syscalls
/lab/ktest run lx_namespace:user:credentials
cleanup() {
    zoneadm -z lx-probe halt
}
trap cleanup EXIT
(cd /zones/lx-probe-zfs/root &&
    tar xf /lab/lx-root.tar ./test/proc_maps_baseline \
        ./test/cap_syscalls_probe ./test/cap_syscalls_probe32 ./test/scope_caps \
        ./test/dynamic/scope_caps)
mkdir -p /zones/lx-probe-zfs/root/test/scope-setid
cp /zones/lx-probe-zfs/root/test/scope_caps /zones/lx-probe-zfs/root/test/scope-setid/scope_caps
chmod 4755 /zones/lx-probe-zfs/root/test/scope-setid/scope_caps
zoneadm -z lx-probe boot
/lab/ktest run lx_namespace:user:access
/lab/ktest run lx_namespace:user:proc_maps
zlogin -S lx-probe /test/proc_maps_baseline
zlogin -S lx-probe /test/cap_syscalls_probe
zlogin -S lx-probe '/test/cap_syscalls_probe --filter'
zlogin -S lx-probe /test/cap_syscalls_probe32
echo LX_CAPABILITIES32_EXIT=0
for probe in /test/scope_caps /test/dynamic/scope_caps; do
    if test "$probe" = /test/scope_caps; then
        scope_log=/lab/scope-caps.log
    else
        scope_log=/lab/scope-caps-dynamic.log
    fi
    zlogin -S lx-probe "$probe" > "$scope_log" 2>&1 &
    scope_job=$!
    scope_pid=
    for attempt in {1..10}; do
        scope_pid=$(sed -n 's/^SCOPE_READY_PID=//p' "$scope_log")
        test -z "$scope_pid" || break
        sleep 1
    done
    [[ "$scope_pid" =~ ^[0-9]+$ ]]
    printf '%s\0' "$scope_pid" > /lab/scope-target.bin
    /lab/ktest run -i /lab/scope-target.bin lx_namespace:user:scope_attach
    kill -CONT "$scope_pid"
    scope_done=0
    for attempt in {1..30}; do
        if ! kill -0 "$scope_job" 2>/dev/null; then
            scope_done=1
            break
        fi
        sleep 1
    done
    if test "$scope_done" != 1; then
        cat "$scope_log"
        echo "LX_SCOPE_TIMEOUT=$probe"
        exit 1
    fi
    if wait "$scope_job"; then
        cat "$scope_log"
    else
        cat "$scope_log"
        exit 1
    fi
    grep -q '^SCOPE_CAPS cases=19 failures=0$' "$scope_log"
done
cleanup
trap - EXIT
/lab/ktest run lx_namespace:user:credentials
echo LX_USERNS_BACKEND_PASS
