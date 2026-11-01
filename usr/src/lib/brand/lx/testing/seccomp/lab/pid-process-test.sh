#!/bin/bash
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
test "$(zonename)" = global
zonepath=$(zonecfg -z lx-probe info zonepath | awk '{print $2}')
test "$(digest -a sha256 "$zonepath/root/test/pid_ownership" | awk '{print $NF}')" = 0919bd5336eaef99de83a6f664e9155b8c7a40fcf6904585e94c6754c5df099b
test "$(digest -a sha256 "$zonepath/root/test/dynamic/pid_ownership" | awk '{print $NF}')" = ce642e50c69c36095187528f86e4a2ef3ac5741415bb85d5211aeff6efd8f4a0
pid=
child=
leader=
runner=
held=0
cleanup() {
    if test -n "$child"; then
        kill -KILL "$child" 2>/dev/null || true
        kill -CONT "$child" 2>/dev/null || true
    fi
    if test -n "$leader"; then
        kill -KILL "$leader" 2>/dev/null || true
        kill -CONT "$leader" 2>/dev/null || true
    fi
    if test -n "$pid"; then
        kill -KILL "$pid" 2>/dev/null || true
        kill -CONT "$pid" 2>/dev/null || true
    fi
    if test -n "$runner"; then wait "$runner" || true; fi
    if test "$held" = 1; then
        ktest run lx_namespace:pid:process_release >/lab/pid-process-cleanup.log 2>&1 || true
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
wait_zombie() {
    local target=$1 i
    for i in {1..15}; do
        test "$(ps -p "$target" -o s | awk 'NR == 2 {print $1}')" = Z && return 0
        sleep 1
    done
    return 1
}
printf '%s\n' 'lx_brand`lx_seccomp_experimental/W 1' \
    'lx_brand`lx_userns_experimental/W 1' \
    'lx_brand`lx_mntns_experimental/W 1' | mdb -kw
zoneadm -z lx-probe boot
for variant in static dynamic; do
    probe=/test/pid_ownership
    test "$variant" != dynamic || probe=/test/dynamic/pid_ownership
    for uid in 0 65534; do
        for guard in 0 1; do
            for mode in exit exec fork; do
                options=
                test "$uid" = 0 || options="$options --drop"
                test "$guard" = 0 || options="$options --guard"
                log=/lab/pid-process-$variant-$uid-$guard-$mode.log
                echo "PID_PROCESS_CASE_START variant=$variant uid=$uid guard=$guard mode=$mode"
                zlogin -S lx-probe "$probe $mode $options" </dev/null > "$log" 2>&1 &
                runner=$!
                wait_marker "$log" '^PID_PROCESS '
                pid=$(awk '/^PID_PROCESS / {sub("pid=", "", $2); print $2; exit}' "$log")
                wait_stopped "$pid"
                printf '%s\0' "$pid" > /lab/pid-process-owner.bin
                ktest run -i /lab/pid-process-owner.bin lx_namespace:pid:process_attach
                held=1
                kill -CONT "$pid"
                if test "$mode" = fork; then
                    wait_marker "$log" '^PID_PARENT '
                    child=$(awk '/^PID_PARENT / {sub("child=", "", $3); print $3; exit}' "$log")
                    wait_marker "$log" '^PID_CHILD '
                    wait_stopped "$pid"
                    wait_stopped "$child"
                    printf '%s\0' "$child" > /lab/pid-process-target.bin
                    ktest run -i /lab/pid-process-target.bin lx_namespace:pid:process_child
                    kill -CONT "$child"
                    wait_marker "$log" '^PID_CHILD_EXIT '
                    ktest run lx_namespace:pid:process_zombie
                    kill -CONT "$pid"
                else
                    wait_marker "$log" '^PID_WORKER '
                    tid=$(awk '/^PID_WORKER / {sub("tid=", "", $3); print $3; exit}' "$log")
                    wait_stopped "$pid"
                    printf '%s\0' "$tid" > /lab/pid-process-target.bin
                    ktest run -i /lab/pid-process-target.bin lx_namespace:pid:process_thread
                    kill -CONT "$pid"
                fi
                wait_marker "$log" '^PID_RETIRED '
                wait_stopped "$pid"
                ktest run lx_namespace:pid:process_retired
                ktest run lx_namespace:pid:process_release
                held=0
                child=
                kill -CONT "$pid"
                wait "$runner"
                runner=
                pid=
                cat "$log"
                grep -q '^PID_PASS$' "$log"
                if test "$mode" = fork; then grep -q '^PID_WAIT_PASS ' "$log"; fi
                ktest run lx_namespace:pid:process_idle
                ktest run lx_namespace:mount:process_idle
                for ((native_exit = 0; native_exit < 32; native_exit++)); do
                    /usr/bin/true
                done
                ktest run lx_namespace:user:zone_budget_idle
                echo "PID_PROCESS_CASE_PASS variant=$variant uid=$uid guard=$guard mode=$mode"
            done
            options=
            test "$uid" = 0 || options="$options --drop"
            test "$guard" = 0 || options="$options --guard"
            log=/lab/pid-process-$variant-$uid-$guard-roles.log
            echo "PID_ROLE_CASE_START variant=$variant uid=$uid guard=$guard"
            zlogin -S lx-probe "$probe roles $options" </dev/null > "$log" 2>&1 &
            runner=$!
            wait_marker "$log" '^PID_PROCESS '
            pid=$(awk '/^PID_PROCESS / {sub("pid=", "", $2); print $2; exit}' "$log")
            wait_stopped "$pid"
            printf '%s\0' "$pid" > /lab/pid-process-owner.bin
            ktest run -i /lab/pid-process-owner.bin lx_namespace:pid:process_attach
            held=1
            ktest run lx_namespace:pid:process_roles
            kill -CONT "$pid"
            wait_marker "$log" '^PID_ROLE_OWNER '
            wait_marker "$log" '^PID_ROLE_LEADER '
            wait_marker "$log" '^PID_ROLE_MEMBER '
            leader=$(awk '/^PID_ROLE_OWNER / {sub("leader=", "", $3); print $3; exit}' "$log")
            child=$(awk '/^PID_ROLE_LEADER / {sub("member=", "", $3); print $3; exit}' "$log")
            wait_stopped "$pid"
            wait_stopped "$leader"
            wait_stopped "$child"
            printf '%s %s\0' "$leader" "$child" > /lab/pid-role-pair.bin
            ktest run -i /lab/pid-role-pair.bin lx_namespace:pid:role_pair
            kill -CONT "$leader"
            kill -CONT "$pid"
            wait_marker "$log" '^PID_ROLE_LEADER_REAPED '
            wait_stopped "$pid"
            ktest run lx_namespace:pid:role_survivor
            leader=
            kill -CONT "$child"
            wait_marker "$log" '^PID_ROLE_SPLIT '
            wait_stopped "$child"
            ktest run lx_namespace:pid:role_split
            kill -CONT "$child"
            wait_marker "$log" '^PID_ROLE_MEMBER_EXIT '
            wait_zombie "$child"
            ktest run lx_namespace:pid:role_zombie
            kill -CONT "$pid"
            wait_marker "$log" '^PID_ROLE_MEMBER_REAPED '
            wait_stopped "$pid"
            ktest run lx_namespace:pid:role_reaped
            child=
            ktest run lx_namespace:pid:role_race
            kill -CONT "$pid"
            wait_marker "$log" '^PID_RETIRED '
            wait_stopped "$pid"
            kill -CONT "$pid"
            wait "$runner"
            runner=
            ktest run lx_namespace:pid:role_release
            held=0
            pid=
            cat "$log"
            grep -q '^PID_PASS$' "$log"
            ktest run lx_namespace:pid:process_idle
            ktest run lx_namespace:mount:process_idle
            ktest run lx_namespace:user:zone_budget_idle
            echo "PID_ROLE_CASE_PASS variant=$variant uid=$uid guard=$guard"
        done
    done
done
printf '%s\n' 'lx_brand`lx_seccomp_experimental/W 0' \
    'lx_brand`lx_userns_experimental/W 0' \
    'lx_brand`lx_mntns_experimental/W 0' | mdb -kw
zoneadm -z lx-probe halt
echo LX_PID_PROCESS_PASS
