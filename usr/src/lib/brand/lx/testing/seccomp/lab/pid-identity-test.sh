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
held=0
cleanup() {
    if test -n "$pid"; then
        kill -KILL "$pid" 2>/dev/null || true
        kill -CONT "$pid" 2>/dev/null || true
    fi
    if test -n "$runner"; then wait "$runner" || true; fi
    if test "$held" = 1; then
        ktest run lx_namespace:pid:release >/lab/pid-identity-cleanup.log 2>&1 || true
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
for variant in static dynamic; do
    probe=/test/pid_identity
    test "$variant" != dynamic || probe=/test/dynamic/pid_identity
    for uid in 0 65534; do
        for guard in 0 1; do
            for mode in exit exec; do
                options=
                test "$uid" = 0 || options="$options --drop"
                test "$guard" = 0 || options="$options --guard"
                log=/lab/pid-identity-$variant-$uid-$guard-$mode.log
                echo "PID_IDENTITY_CASE_START variant=$variant uid=$uid guard=$guard mode=$mode"
                zlogin -S lx-probe "$probe $mode $options" </dev/null > "$log" 2>&1 &
                runner=$!
                wait_marker "$log" '^PID_WORKER '
                pid=$(awk '/^PID_WORKER / {sub("pid=", "", $2); print $2; exit}' "$log")
                tid=$(awk '/^PID_WORKER / {sub("tid=", "", $3); print $3; exit}' "$log")
                wait_stopped "$pid"
                printf '%s\0' "$tid" > /lab/pid-identity-worker.bin
                ktest run -i /lab/pid-identity-worker.bin lx_namespace:pid:hold
                held=1
                kill -CONT "$pid"
                wait_marker "$log" '^PID_RETIRED '
                wait_stopped "$pid"
                ktest run lx_namespace:pid:retired
                ktest run lx_namespace:pid:release
                held=0
                kill -CONT "$pid"
                wait "$runner"
                runner=
                pid=
                cat "$log"
                grep -q '^PID_PASS$' "$log"
                ktest run lx_namespace:mount:process_idle
                for ((native_exit = 0; native_exit < 32; native_exit++)); do
                    /usr/bin/true
                done
                ktest run lx_namespace:user:zone_budget_idle
                echo "PID_IDENTITY_CASE_PASS variant=$variant uid=$uid guard=$guard mode=$mode"
            done
        done
    done
done
printf '%s\n' 'lx_brand`lx_seccomp_experimental/W 0' \
    'lx_brand`lx_userns_experimental/W 0' \
    'lx_brand`lx_mntns_experimental/W 0' | mdb -kw
zoneadm -z lx-probe halt
echo LX_PID_IDENTITY_PASS
