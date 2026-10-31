#!/bin/bash
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
test "$(zonename)" = global
zonepath=$(zonecfg -z lx-probe info zonepath | awk '{print $2}')
# build-fixtures.sh writes hashes beside each isolated fixture.
for variant in static dynamic; do
    dir="$zonepath/root/test/drain/$variant"
    test "$(digest -a sha256 "$dir/pid_orphan" | awk '{print $NF}')" = "$(cat "$dir/sha256")"
done
pid=
child=
descendant=
peer=
runner=
peer_runner=
held=0
tracer=
watchdog=
cleanup() {
    test -z "$watchdog" || kill -TERM "$watchdog" 2>/dev/null || true
    # Failure retains held maps and stopped tasks for parent inspection.
    # Never kill numeric PIDs recovered from an old log.
    if test -n "$tracer"; then
        kill -TERM "$tracer" 2>/dev/null || true
        wait "$tracer" || true
    fi
    test "$held" = 0 || echo "PID_DRAIN_FAILURE_HELD_FIXTURE retained"
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
wait_runner() {
    local target=$1 status
    if wait "$target"; then status=0; else status=$?; fi
    echo "PID_DRAIN_ZLOGIN_STATUS=$status"
    # Native noninteractive zlogin returns WEXITSTATUS without WIFSIGNALED.
    # Kernel exit probes below validate CLD_KILLED/SIGKILL independently.
    test "$status" = 0
}
printf '%s\n' 'lx_brand`lx_seccomp_experimental/W 1' \
    'lx_brand`lx_userns_experimental/W 1' \
    'lx_brand`lx_mntns_experimental/W 1' | mdb -kw
# The offline root needs its Linux reaper, without metadata/network startup.
if ! zonecfg -z lx-probe info attr name=init-name | grep -q 'name: init-name'; then
    zonecfg -z lx-probe 'add attr; set name=init-name; set type=string; set value=/sbin/init; end;'
else
    zonecfg -z lx-probe 'select attr name=init-name; set type=string; set value=/sbin/init; end;'
fi
zoneadm -z lx-probe boot
trace=/lab/pid-drain-exits.log
dtrace -x nolibs -q -n 'BEGIN { printf("PID_DRAIN_TRACE_READY\n"); }
fbt::proc_exit:entry /execname == "pid_orphan"/ {
    printf("PID_DRAIN_EXIT pid=%d why=%d signal=%d\n", pid, arg0, arg1);
}' </dev/null > "$trace" 2>&1 &
tracer=$!
wait_marker "$trace" '^PID_DRAIN_TRACE_READY$'
for variant in static dynamic; do
    for none in 0 1; do
        for killed in 0 1; do
            for extra in normal ignore race; do
                options=
                test "$none" = 0 || options="$options --clone-none"
                test "$killed" = 0 || options="$options --kill"
                test "$extra" != ignore || options="$options --ignore"
                test "$extra" != race || options="$options --race"
                log=/lab/pid-drain-$variant-$none-$killed-$extra.log
                echo "PID_DRAIN_CASE_START variant=$variant clone=$none kill=$killed extra=$extra"
                zlogin -S lx-probe "/test/drain/$variant/pid_orphan $options" </dev/null > "$log" 2>&1 &
                runner=$!
                controller=$$
                ( sleep 55; echo PID_DRAIN_TIMEOUT >&2; kill -TERM "$controller" ) </dev/null &
                watchdog=$!
                wait_marker "$log" '^PID_PROCESS '
                pid=$(awk '/^PID_PROCESS / {sub("pid=", "", $2); print $2; exit}' "$log")
                wait_stopped "$pid"
                printf '%s\0' "$pid" > /lab/pid-drain-owner.bin
                ktest run -i /lab/pid-drain-owner.bin lx_namespace:pid:process_attach
                held=1
                kill -CONT "$pid"
                wait_marker "$log" '^PID_DRAIN_READY '
                ancestor=$(awk '/^PID_DRAIN_READY / {sub("ancestor=", "", $2); print $2; exit}' "$log")
                child=$(awk '/^PID_CHILD / {sub("pid=", "", $2); print $2; exit}' "$log")
                descendant=$(awk '/^PID_DESCENDANT / {sub("pid=", "", $2); print $2; exit}' "$log")
                for target in "$ancestor" "$pid" "$child" "$descendant"; do
                    wait_stopped "$target"
                done
                printf '%s\0' "$child" > /lab/pid-drain-target.bin
                ktest run -i /lab/pid-drain-target.bin lx_namespace:pid:process_child
                printf '%s\0' "$descendant" > /lab/pid-drain-target.bin
                ktest run -i /lab/pid-drain-target.bin lx_namespace:pid:process_descendant
                killed_owner=$pid
                killed_child=$child
                killed_descendant=$descendant
                if test "$extra" = race; then
                    kill -CONT "$child"
                    wait_marker "$log" '^PID_DRAIN_RACE_READY$'
                fi
                # The outside ancestor performs waitpid, then exits. No grace
                # period or process_dead poll may precede process_drained.
                kill -CONT "$ancestor"
                wait_runner "$runner"
                ktest run lx_namespace:pid:process_drained
                runner=
                pid=
                child=
                descendant=
                grep -q '^PID_DRAIN_PASS ancestor_wait_all_gone=2$' "$log"
                for target in "$killed_child" "$killed_descendant"; do
                    wait_marker "$trace" "^PID_DRAIN_EXIT pid=$target why=2 signal=9$"
                done
                if test "$killed" = 1; then
                    wait_marker "$trace" "^PID_DRAIN_EXIT pid=$killed_owner why=2 signal=9$"
                else
                    wait_marker "$trace" "^PID_DRAIN_EXIT pid=$killed_owner why=1 signal=0$"
                fi
                ktest run lx_namespace:pid:process_release
                held=0
                ktest run lx_namespace:pid:process_idle
                ktest run lx_namespace:mount:process_idle
                for ((native_exit = 0; native_exit < 32; native_exit++)); do /usr/bin/true; done
                ktest run lx_namespace:user:zone_budget_idle
                kill -TERM "$watchdog"
                wait "$watchdog" || true
                watchdog=
                cat "$log"
                echo "PID_DRAIN_CASE_PASS variant=$variant clone=$none kill=$killed extra=$extra"
            done
        done
    done
done
kill -TERM "$tracer"
wait "$tracer"
tracer=
printf '%s\n' 'lx_brand`lx_seccomp_experimental/W 0' \
    'lx_brand`lx_userns_experimental/W 0' \
    'lx_brand`lx_mntns_experimental/W 0' | mdb -kw
zoneadm -z lx-probe halt
echo LX_PID_DRAIN_PASS
