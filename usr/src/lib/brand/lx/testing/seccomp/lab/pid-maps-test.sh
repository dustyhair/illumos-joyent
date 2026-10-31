#!/bin/bash
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
test "$(zonename)" = global
zonepath=$(zonecfg -z lx-probe info zonepath | awk '{print $2}')
test "$(digest -a sha256 "$zonepath/root/test/pid_identity" | awk '{print $NF}')" = 01381efe3f26ec016fadea74cb7af6c4f9c38f19a5c0a88ce7f7bfd1323db49d
test "$(digest -a sha256 "$zonepath/root/test/dynamic/pid_identity" | awk '{print $NF}')" = 39363f684dcf5f23edd5971a25f4bd71deb70934a517e81d4442c34db2e73966
pid=
runner=
peerpid=
peerrunner=
held=0
cleanup() {
    if test -n "$pid"; then
        kill -KILL "$pid" 2>/dev/null || true
        kill -CONT "$pid" 2>/dev/null || true
    fi
    if test -n "$peerpid"; then
        kill -KILL "$peerpid" 2>/dev/null || true
        kill -CONT "$peerpid" 2>/dev/null || true
    fi
    if test -n "$runner"; then wait "$runner" || true; fi
    if test -n "$peerrunner"; then wait "$peerrunner" || true; fi
    if test "$held" = 1; then
        ktest run lx_namespace:pid:maps_release >/lab/pid-maps-cleanup.log 2>&1 || true
    fi
}
trap cleanup EXIT
wait_marker() {
    local file=$1 marker=$2 i
    for i in {1..15}; do
        grep -q "$marker" "$file" && return 0
        sleep 1
    done
    cat "$file"
    return 1
}
wait_stopped() {
    local target=$1 i
    for i in {1..15}; do
        test "$(ps -p "$target" -o s | awk 'NR == 2 {print $1}')" = T && return 0
        sleep 1
    done
    return 1
}
printf '%s\n' 'lx_brand`lx_seccomp_experimental/W 1' \
    'lx_brand`lx_userns_experimental/W 1' \
    'lx_brand`lx_mntns_experimental/W 1' | mdb -kw
zoneadm -z lx-probe boot
ktest run lx_namespace:pid:reservation
for variant in static dynamic; do
    probe=/test/pid_identity
    test "$variant" != dynamic || probe=/test/dynamic/pid_identity
    for uid in 0 65534; do
        for guard in 0 1; do
            for mode in exit exec; do
                options=
                test "$uid" = 0 || options="$options --drop"
                test "$guard" = 0 || options="$options --guard"
                log=/lab/pid-maps-$variant-$uid-$guard-$mode.log
                echo "PID_MAPS_CASE_START variant=$variant uid=$uid guard=$guard mode=$mode"
                zlogin -S lx-probe "$probe $mode $options" </dev/null > "$log" 2>&1 &
                runner=$!
                peerlog=/lab/pid-maps-peer-$variant-$uid-$guard-$mode.log
                zlogin -S lx-probe "$probe exit $options" </dev/null > "$peerlog" 2>&1 &
                peerrunner=$!
                wait_marker "$log" '^PID_WORKER '
                pid=$(awk '/^PID_WORKER / {sub("pid=", "", $2); print $2; exit}' "$log")
                tid=$(awk '/^PID_WORKER / {sub("tid=", "", $3); print $3; exit}' "$log")
                wait_marker "$peerlog" '^PID_WORKER '
                peerpid=$(awk '/^PID_WORKER / {sub("pid=", "", $2); print $2; exit}' "$peerlog")
                peertid=$(awk '/^PID_WORKER / {sub("tid=", "", $3); print $3; exit}' "$peerlog")
                wait_stopped "$peerpid"
                wait_stopped "$pid"
                printf '%s %s\0' "$tid" "$peertid" > /lab/pid-maps-worker.bin
                ktest run -i /lab/pid-maps-worker.bin lx_namespace:pid:maps_hold
                held=1
                kill -CONT "$pid"
                wait_marker "$log" '^PID_RETIRED '
                wait_stopped "$pid"
                ktest run lx_namespace:pid:maps_retired
                ktest run lx_namespace:pid:maps_release
                held=0
                kill -CONT "$pid"
                wait "$runner"
                runner=
                pid=
                cat "$log"
                grep -q '^PID_PASS$' "$log"
                kill -CONT "$peerpid"
                wait_marker "$peerlog" '^PID_RETIRED '
                wait_stopped "$peerpid"
                kill -CONT "$peerpid"
                wait "$peerrunner"
                peerrunner=
                peerpid=
                cat "$peerlog"
                grep -q '^PID_PASS$' "$peerlog"
                ktest run lx_namespace:mount:process_idle
                for ((native_exit = 0; native_exit < 32; native_exit++)); do
                    /usr/bin/true
                done
                ktest run lx_namespace:user:zone_budget_idle
                echo "PID_MAPS_CASE_PASS variant=$variant uid=$uid guard=$guard mode=$mode"
            done
        done
    done
done
printf '%s\n' 'lx_brand`lx_seccomp_experimental/W 0' \
    'lx_brand`lx_userns_experimental/W 0' \
    'lx_brand`lx_mntns_experimental/W 0' | mdb -kw
zoneadm -z lx-probe halt
echo LX_PID_MAPS_PASS
