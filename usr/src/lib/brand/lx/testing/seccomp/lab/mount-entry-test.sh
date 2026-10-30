#!/bin/bash
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
test "$(zonename)" = global
zonepath=$(zonecfg -z lx-probe info zonepath | awk '{print $2}')
mkdir -p "$zonepath/root/test/mnt-entry/cwd"
chmod 0755 "$zonepath/root/test/mnt-entry" "$zonepath/root/test/mnt-entry/cwd"
flags() {
    printf '%s\n' "lx_brand\`lx_seccomp_experimental/W 1" \
        "lx_brand\`lx_userns_experimental/W $1" \
        "lx_brand\`lx_mntns_experimental/W $2" | mdb -kw
}
boot_limits() {
    zonecfg -z lx-probe "select attr name=max-mount-namespaces; set value=$1; end;"
    zonecfg -z lx-probe "select attr name=max-user-namespaces; set value=$2; end;"
    zoneadm -z lx-probe boot
}
wait_ready() {
    local file=$1 i
    for i in {1..20}; do
        grep -q '^MNE_READY_PID=' "$file" && return 0
        sleep 1
    done
    cat "$file"
    return 1
}
run_case() {
    local variant=$1 uid=$2 mode=$3 want=$4 phase=$5 filter=${6:-0}
    local probe=/test/mntns_entry drop= extra= job pid log suite
    test "$variant" != dynamic || probe=/test/dynamic/mntns_entry
    test "$uid" = 0 || drop=--drop
    test "$filter" = 0 || extra=--filter
    log=/lab/mnt-entry-$phase-$variant-$uid-$mode-$filter.log
    echo "MNT_ENTRY_CASE_START phase=$phase variant=$variant uid=$uid mode=$mode errno=$want filter=$filter"
    zlogin -S lx-probe "$probe $drop $extra --mode $mode --errno $want" </dev/null > "$log" 2>&1 &
    job=$!
    wait_ready "$log"
    pid=$(sed -n 's/^MNE_READY_PID=//p' "$log")
    suite=entry_reject
    if test "$want" = 0; then
        suite=entry_user_mount
        test "$mode" != mount || suite=entry_mount
    elif test "$mode" = user-first || test "$mode" = drop-admin; then
        suite=entry_user_only
    fi
    printf '%s\0' "$pid" > /lab/mnt-entry-pid.bin
    ktest run -i /lab/mnt-entry-pid.bin "lx_namespace:mount:$suite"
    kill -CONT "$pid"
    wait "$job"
    cat "$log"
    grep -q '^MNE_PASS .*failures=0$' "$log"
    ktest run lx_namespace:mount:process_idle
    ktest run lx_namespace:user:zone_budget_idle
    echo "MNT_ENTRY_CASE_PASS phase=$phase variant=$variant uid=$uid mode=$mode errno=$want filter=$filter"
}
flags 0 0
boot_limits 7 64
for variant in static dynamic; do
    for uid in 0 65534; do
        run_case "$variant" "$uid" mount 1 disabled
        run_case "$variant" "$uid" combined 1 disabled
    done
done
flags 1 0
for variant in static dynamic; do
    for uid in 0 65534; do
        run_case "$variant" "$uid" combined 22 mount-disabled
    done
done
flags 1 1
for variant in static dynamic; do
    for uid in 0 65534; do
        for mode in mount combined user-first drop-admin; do
            want=0
            test "$mode" != drop-admin || want=1
            if test "$mode" = mount && test "$uid" != 0; then want=1; fi
            run_case "$variant" "$uid" "$mode" "$want" enabled
        done
        for mode in mount combined; do
            want=0
            if test "$mode" = mount && test "$uid" != 0; then want=1; fi
            run_case "$variant" "$uid" "$mode" "$want" enabled 1
        done
    done
done
zoneadm -z lx-probe halt
boot_limits 0 64
for variant in static dynamic; do
    for uid in 0 65534; do
        for mode in mount combined user-first drop-admin; do
            want=28
            if test "$mode" = drop-admin || { test "$mode" = mount && test "$uid" != 0; }; then want=1; fi
            run_case "$variant" "$uid" "$mode" "$want" mount-quota-zero
        done
    done
done
zoneadm -z lx-probe halt
boot_limits 7 0
for variant in static dynamic; do
    for uid in 0 65534; do
        run_case "$variant" "$uid" combined 28 user-quota-zero
        want=0
        test "$uid" = 0 || want=1
        run_case "$variant" "$uid" mount "$want" user-quota-zero
    done
done
zoneadm -z lx-probe halt
zonecfg -z lx-probe 'select attr name=max-user-namespaces; set value=64; end;'
flags 0 0
printf '%s\n' 'lx_brand`lx_seccomp_experimental/W 0' | mdb -kw
echo LX_MOUNT_ENTRY_PASS
