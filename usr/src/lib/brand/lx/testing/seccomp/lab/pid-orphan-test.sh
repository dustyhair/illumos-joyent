#!/bin/bash
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
test "$(zonename)" = global
zonepath=$(zonecfg -z lx-probe info zonepath | awk '{print $2}')
test "$(digest -a sha256 "$zonepath/root/test/pid_orphan" | awk '{print $NF}')" = 0693f205a6ee9fc39f2dec45cf431aa370ffd38c7c9e241844486fb45f48f682
test "$(digest -a sha256 "$zonepath/root/test/dynamic/pid_orphan" | awk '{print $NF}')" = f426360fb20d0ccb59327c433cbd1c39030e2ad6a0e8a1db02e08ab3e886f1fc
pid=
child=
descendant=
runner=
held=0
log=
cleanup() {
    if test -n "$log" && test -f "$log"; then
        test -n "$child" || child=$(awk '/^PID_CHILD / {sub("pid=", "", $2); print $2; exit}' "$log")
        test -n "$descendant" || descendant=$(awk '/^PID_DESCENDANT / {sub("pid=", "", $2); print $2; exit}' "$log")
    fi
    for target in "$descendant" "$child" "$pid"; do
        test -z "$target" || kill -KILL "$target" 2>/dev/null || true
    done
    test -z "$runner" || wait "$runner" || true
    if test "$held" = 1; then
        ktest run lx_namespace:pid:process_release >/lab/pid-orphan-cleanup.log 2>&1 || true
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
# Use the Linux test reaper without offline metadata/network startup.
if ! zonecfg -z lx-probe info attr name=init-name | grep -q 'name: init-name'; then
    zonecfg -z lx-probe 'add attr; set name=init-name; set type=string; set value=/sbin/init; end;'
else
    zonecfg -z lx-probe 'select attr name=init-name; set type=string; set value=/sbin/init; end;'
fi
zoneadm -z lx-probe boot
for variant in static dynamic; do
    probe=/test/pid_orphan
    test "$variant" != dynamic || probe=/test/dynamic/pid_orphan
    for uid in 0 65534; do
        for guard in 0 1; do
            for group in 0 1; do
                for clone in 0 1; do
                    options=
                    test "$uid" = 0 || options="$options --drop"
                    test "$guard" = 0 || options="$options --guard"
                    test "$group" = 0 || options="$options --group"
                    test "$clone" = 0 || options="$options --clone-none"
                    log=/lab/pid-orphan-$variant-$uid-$guard-$group-$clone.log
                    echo "PID_ORPHAN_CASE_START variant=$variant uid=$uid guard=$guard group=$group clone_none=$clone"
                    zlogin -S lx-probe "$probe $options" </dev/null > "$log" 2>&1 &
                    runner=$!
                    wait_marker "$log" '^PID_PROCESS '
                    pid=$(awk '/^PID_PROCESS / {sub("pid=", "", $2); print $2; exit}' "$log")
                    wait_stopped "$pid"
                    printf '%s\0' "$pid" > /lab/pid-orphan-owner.bin
                    ktest run -i /lab/pid-orphan-owner.bin lx_namespace:pid:process_attach
                    held=1
                    kill -CONT "$pid"
                    wait_marker "$log" '^PID_CHILD '
                    child=$(awk '/^PID_CHILD / {sub("pid=", "", $2); print $2; exit}' "$log")
                    wait_marker "$log" '^PID_DESCENDANT '
                    descendant=$(awk '/^PID_DESCENDANT / {sub("pid=", "", $2); print $2; exit}' "$log")
                    wait_marker "$log" '^PID_PARENT '
                    wait_stopped "$pid"
                    wait_stopped "$child"
                    wait_stopped "$descendant"
                    printf '%s\0' "$child" > /lab/pid-orphan-target.bin
                    ktest run -i /lab/pid-orphan-target.bin lx_namespace:pid:process_child
                    printf '%s\0' "$descendant" > /lab/pid-orphan-target.bin
                    ktest run -i /lab/pid-orphan-target.bin lx_namespace:pid:process_descendant
                    kill -CONT "$child"
                    ktest run lx_namespace:pid:process_zombie
                    ktest run lx_namespace:pid:orphan_adopted
                    wait_stopped "$descendant"
                    kill -CONT "$descendant"
                    wait_marker "$log" '^PID_ORPHAN_EXIT '
                    ktest run lx_namespace:pid:orphan_zombie
                    kill -CONT "$pid"
                    wait_marker "$log" '^PID_RETIRED '
                    wait_stopped "$pid"
                    ktest run lx_namespace:pid:process_retired
                    ktest run lx_namespace:pid:orphan_retired
                    ktest run lx_namespace:pid:process_release
                    held=0
                    child=
                    descendant=
                    kill -CONT "$pid"
                    wait "$runner"
                    runner=
                    pid=
                    cat "$log"
                    grep -q '^PID_ADOPT_PASS ' "$log"
                    grep -q '^PID_PASS$' "$log"
                    ktest run lx_namespace:pid:process_idle
                    ktest run lx_namespace:mount:process_idle
                    for ((native_exit = 0; native_exit < 32; native_exit++)); do
                        /usr/bin/true
                    done
                    ktest run lx_namespace:user:zone_budget_idle
                    echo "PID_ORPHAN_CASE_PASS variant=$variant uid=$uid guard=$guard group=$group clone_none=$clone"
                    log=
                done
            done
        done
    done
done
printf '%s\n' 'lx_brand`lx_seccomp_experimental/W 0' \
    'lx_brand`lx_userns_experimental/W 0' \
    'lx_brand`lx_mntns_experimental/W 0' | mdb -kw
zoneadm -z lx-probe halt
echo LX_PID_ORPHAN_PASS
