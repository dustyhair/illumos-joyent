#!/bin/bash
# Trusted attachment fixture only; no Linux namespace syscall is enabled.
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
test "$(zonename)" = global
if ! zpool list zones >/dev/null 2>&1; then
    zpool import zones
fi
/lab/enforcement-test.sh > /lab/mount-process-enforcement.log 2>&1
echo MOUNT_PROCESS_ENFORCEMENT_EXIT=0
/lab/namespace-test.sh > /lab/mount-process-backend.log 2>&1
echo MOUNT_PROCESS_BACKEND_EXIT=0
zonepath=$(zonecfg -z lx-probe info zonepath | awk '{print $2}')
fixture=$zonepath/root/test/mnt-proc
mkdir -p "$fixture"/{cwd,covered}
chmod 0755 "$fixture" "$fixture/cwd" "$fixture/covered"
zoneadm -z lx-probe boot
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
    local pid=$1 i
    for i in {1..15}; do
        test "$(ps -p "$pid" -o s | awk 'NR == 2 {print $1}')" = T && return 0
        sleep 1
    done
    return 1
}
ktest_pid() {
    printf '%s\0' "$2" > /lab/mount-process-pid.bin
    ktest run -o RESULT,LINE,REASON -i /lab/mount-process-pid.bin \
        "lx_namespace:mount:$1"
}
for variant in static dynamic; do
    probe=/test/mount_proc
    test "$variant" != dynamic || probe=/test/dynamic/mount_proc
    for uid in 0 65534; do
        drop=
        test "$uid" = 0 || drop=--drop
        for mode in reject attach; do
            printf 'original\n' > "$fixture/covered/original"
            printf 'source\n' > "$fixture/source"
            printf 'target\n' > "$fixture/bind-target"
            : > "$fixture/result"
            chown "$uid:$uid" "$fixture/result"
            chmod 0600 "$fixture/result"
            chmod 0644 "$fixture/covered/original"
            chmod 0666 "$fixture/source" "$fixture/bind-target"
            log=/lab/mount-process-$variant-$uid-$mode.log
            option=
            test "$mode" != reject || option=--unrestricted
            echo "MOUNT_PROCESS_CASE_START variant=$variant uid=$uid mode=$mode"
            zlogin -S lx-probe "$probe $drop $option" > "$log" 2>&1 &
            job=$!
            wait_marker "$log" '^MP_READY_PID='
            target=$(sed -n 's/^MP_READY_PID=//p' "$log")
            wait_stopped "$target"
            ktest_pid "process_$mode" "$target"
            zlogin -S lx-probe "$probe $drop --outside"
            kill -CONT "$target"
            if test "$mode" = attach; then
                wait_marker "$log" '^MP_ZOMBIE_PID='
                wait_stopped "$target"
                zombie=$(sed -n 's/^MP_ZOMBIE_PID=//p' "$log")
                ktest_pid process_zombie "$zombie"
                kill -CONT "$target"
                wait_marker "$log" '^MP_ORPHAN_PID='
                orphan=$(sed -n 's/^MP_ORPHAN_PID=//p' "$log")
                wait "$job"
                wait_stopped "$orphan"
                ktest_pid process_member "$orphan"
                kill -CONT "$orphan"
                wait_marker "$fixture/result" '^MP_CHILD_PASS mode=2 .*failures=0$'
                grep -qx "MP_PARENT failures=0 uid=$uid" "$log"
                grep -qx "MP_CHILD_SIGNAL_READY uid=$uid failures=0" "$fixture/result"
                cp "$fixture/result" /lab/mount-process-$variant-$uid-result.log
            else
                wait "$job"
            fi
            ktest run -o RESULT,LINE,REASON lx_namespace:mount:process_idle
            zlogin -S lx-probe "$probe $drop --outside"
            test ! -e "$fixture/covered/private"
            echo "MOUNT_PROCESS_CASE_PASS variant=$variant uid=$uid mode=$mode"
        done
    done
done
zoneadm -z lx-probe halt
/lab/userns-test.sh > /lab/mount-process-userns.log 2>&1
echo MOUNT_PROCESS_USERNS_EXIT=0
printf '%s\n' 'lx_brand`lx_seccomp_experimental/W 0' | mdb -kw
echo LX_MOUNT_PROCESS_PASS
