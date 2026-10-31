#!/bin/bash
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
test "$(zonename)" = global
zonepath=$(zonecfg -z lx-probe info zonepath | awk '{print $2}')
for variant in static dynamic; do
    dir="$zonepath/root/test/zombie/$variant"
    test "$(digest -a sha256 "$dir/pid_orphan" | awk '{print $NF}')" = "$(cat "$dir/sha256")"
done
pid=
child=
descendant=
runner=
held=0
log=
tracer=
watchdog=
cleanup() {
    test -z "$watchdog" || kill -TERM "$watchdog" 2>/dev/null || true
    if test -n "$tracer"; then
        kill -TERM "$tracer" 2>/dev/null || true
        wait "$tracer" || true
    fi
    # Keep stopped tasks and held maps on failure for parent inspection.
    test "$held" = 0 || echo PID_ZOMBIE_FAILURE_HELD_FIXTURE

}
trap cleanup EXIT
trap 'exit 124' TERM
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
trace=/lab/pid-zombie-sigcld.log
dtrace -x nolibs -s /lab/zombie-sigcld.d </dev/null > "$trace" 2>&1 &
tracer=$!
wait_marker "$trace" '^PID_ZOMBIE_TRACE_READY$' 
for variant in static dynamic; do
    probe=/test/zombie/static/pid_orphan
    test "$variant" != dynamic || probe=/test/zombie/dynamic/pid_orphan
    for uid in 0 65534; do
        for guard in 0 1; do
            for group in 0 1; do
                for clone in 0 1; do
                    options=--zombie
                    test "$uid" = 0 || options="$options --drop"
                    test "$guard" = 0 || options="$options --guard"
                    test "$group" = 0 || options="$options --group"
                    test "$clone" = 0 || options="$options --clone-none"
                    log=/lab/pid-zombie-$variant-$uid-$guard-$group-$clone.log
                    echo "PID_ZOMBIE_CASE_START variant=$variant uid=$uid guard=$guard group=$group clone_none=$clone"
                    zlogin -S lx-probe "$probe $options" </dev/null > "$log" 2>&1 &
                    runner=$!
                    controller=$$
                    ( sleep 55; echo PID_ZOMBIE_TIMEOUT >&2; kill -TERM "$controller" ) </dev/null &
                    watchdog=$!
                    wait_marker "$log" '^PID_PROCESS '
                    pid=$(awk '/^PID_PROCESS / {sub("pid=", "", $2); print $2; exit}' "$log")
                    wait_stopped "$pid"
                    printf '%s\0' "$pid" > /lab/pid-zombie-owner.bin
                    ktest run -i /lab/pid-zombie-owner.bin lx_namespace:pid:process_attach
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
                    printf '%s\0' "$child" > /lab/pid-zombie-target.bin
                    ktest run -i /lab/pid-zombie-target.bin lx_namespace:pid:process_child
                    printf '%s\0' "$descendant" > /lab/pid-zombie-target.bin
                    ktest run -i /lab/pid-zombie-target.bin lx_namespace:pid:process_descendant
                    # Original parent and namespace init remain stopped.
                    kill -CONT "$descendant"
                    ktest run lx_namespace:pid:orphan_zombie
                    kill -CONT "$child"
                    ktest run lx_namespace:pid:process_zombie
                    ktest run lx_namespace:pid:orphan_zombie_adopted
                    wait_marker "$trace" "^PID_ZOMBIE_SIGCLD_NULL child=$descendant parent=$pid$"
                    wait_marker "$trace" "^PID_ZOMBIE_QUEUE_ALLOC child=$descendant parent=$pid kmflag="
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
                    echo "PID_ZOMBIE_CASE_PASS variant=$variant uid=$uid guard=$guard group=$group clone_none=$clone"
                    kill -TERM "$watchdog"
                    wait "$watchdog" || true
                    watchdog=
                    log=
                done
            done
        done
    done
done
printf '%s\n' 'lx_brand`lx_seccomp_experimental/W 0' \
    'lx_brand`lx_userns_experimental/W 0' \
    'lx_brand`lx_mntns_experimental/W 0' | mdb -kw
kill -TERM "$tracer"
wait "$tracer"
tracer=
zoneadm -z lx-probe halt
echo LX_PID_ZOMBIE_PASS
