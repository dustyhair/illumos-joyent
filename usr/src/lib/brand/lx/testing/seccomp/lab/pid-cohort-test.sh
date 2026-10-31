#!/bin/bash
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
test "$(zonename)" = global
zonepath=$(zonecfg -z lx-probe info zonepath | awk '{print $2}')
test "$(digest -a sha256 "$zonepath/root/test/pid_ownership" | awk '{print $NF}')" = 4a40378f8c65c2457cc96f8468c9931fecbf781bdc428e2143635bc299b36e32
test "$(digest -a sha256 "$zonepath/root/test/dynamic/pid_ownership" | awk '{print $NF}')" = 33ee0aa8d8132efe239cc1cf3c872cc01e2dc7d96852a1620f89bca6e404ca5d
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
    test "$held" = 0 || echo "PID_COHORT_FAILURE_HELD_FIXTURE retained"
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
    echo "PID_COHORT_ZLOGIN_STATUS=$status"
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
trace=/lab/pid-cohort-exits.log
dtrace -x nolibs -q -n 'BEGIN { printf("PID_COHORT_TRACE_READY\n"); }
fbt::proc_exit:entry /execname == "pid_ownership"/ {
    printf("PID_COHORT_EXIT pid=%d why=%d signal=%d\n", pid, arg0, arg1);
}' </dev/null > "$trace" 2>&1 &
tracer=$!
wait_marker "$trace" '^PID_COHORT_TRACE_READY$'
for variant in static dynamic; do
    probe=/test/pid_ownership
    test "$variant" != dynamic || probe=/test/dynamic/pid_ownership
    for uid in 0 65534; do
        for guard in 0 1; do
            options=
            test "$uid" = 0 || options="$options --drop"
            peer_log=/lab/pid-cohort-peer-$variant-$uid-$guard.log
            log=/lab/pid-cohort-$variant-$uid-$guard.log
            echo "PID_COHORT_CASE_START variant=$variant uid=$uid guard=$guard"
            zlogin -S lx-probe "$probe exit $options" </dev/null > "$peer_log" 2>&1 &
            peer_runner=$!
            wait_marker "$peer_log" '^PID_PROCESS '
            peer=$(awk '/^PID_PROCESS / {sub("pid=", "", $2); print $2; exit}' "$peer_log")
            wait_stopped "$peer"
            test "$guard" = 0 || options="$options --guard"
            zlogin -S lx-probe "$probe death $options" </dev/null > "$log" 2>&1 &
            runner=$!
            controller=$$
            ( sleep 55; echo PID_COHORT_TIMEOUT >&2; kill -TERM "$controller" ) </dev/null &
            watchdog=$!
            wait_marker "$log" '^PID_PROCESS '
            pid=$(awk '/^PID_PROCESS / {sub("pid=", "", $2); print $2; exit}' "$log")
            wait_stopped "$pid"
            printf '%s\0' "$pid" > /lab/pid-cohort-owner.bin
            ktest run -i /lab/pid-cohort-owner.bin lx_namespace:pid:process_attach
            held=1
            kill -CONT "$pid"
            wait_marker "$log" '^PID_CHILD '
            wait_marker "$log" '^PID_DESCENDANT '
            child=$(awk '/^PID_CHILD / {sub("pid=", "", $2); print $2; exit}' "$log")
            descendant=$(awk '/^PID_DESCENDANT / {sub("pid=", "", $2); print $2; exit}' "$log")
            wait_stopped "$pid"
            wait_stopped "$child"
            wait_stopped "$descendant"
            printf '%s\0' "$child" > /lab/pid-cohort-target.bin
            ktest run -i /lab/pid-cohort-target.bin lx_namespace:pid:process_child
            printf '%s\0' "$descendant" > /lab/pid-cohort-target.bin
            ktest run -i /lab/pid-cohort-target.bin lx_namespace:pid:process_descendant
            killed_owner=$pid
            killed_peer=$peer
            killed_child=$child
            killed_descendant=$descendant
            kill -KILL "$pid"
            wait_runner "$runner"
            ktest run lx_namespace:pid:process_drained
            runner=
            pid=
            child=
            descendant=
            wait_stopped "$peer"
            echo PID_COHORT_OUTSIDE_PEER_ALIVE
            kill -KILL "$peer"
            wait_runner "$peer_runner"
            peer_runner=
            peer=
            for killed in "$killed_owner" "$killed_child" "$killed_descendant" "$killed_peer"; do
                wait_marker "$trace" "^PID_COHORT_EXIT pid=$killed why=2 signal=9$"
                grep "^PID_COHORT_EXIT pid=$killed why=2 signal=9$" "$trace"
            done
            ktest run lx_namespace:pid:process_release
            held=0
            child=
            descendant=
            cat "$log"
            cat "$peer_log"
            ktest run lx_namespace:pid:process_idle
            ktest run lx_namespace:mount:process_idle
            for ((native_exit = 0; native_exit < 32; native_exit++)); do
                /usr/bin/true
            done
            ktest run lx_namespace:user:zone_budget_idle
            kill -TERM "$watchdog"
            wait "$watchdog" || true
            watchdog=
            echo "PID_COHORT_CASE_PASS variant=$variant uid=$uid guard=$guard"
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
echo LX_PID_COHORT_PASS
