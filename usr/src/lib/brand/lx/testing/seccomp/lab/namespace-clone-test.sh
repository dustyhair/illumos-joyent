#!/bin/bash
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
test "$(zonename)" = global
phase=${1:-enabled}
case "$phase" in enabled|disabled|mount-zero|user-zero) ;; *) exit 2;; esac
zonepath=$(zonecfg -z lx-probe info zonepath | awk '{print $2}')
test "$(digest -a sha256 "$zonepath/root/test/mntns_clone" | awk '{print $NF}')" = 3bbd1ced5ae26de60b07692a16e59f81c56ac8e864947ed1aea2c79762815f5f
test "$(digest -a sha256 "$zonepath/root/test/dynamic/mntns_clone" | awk '{print $NF}')" = e28c943caef1946b32566d21492eb4f91ba74d392cc37d453f50902f6d2e2a6f
mkdir -p "$zonepath/root/test/mnt-clone"
chmod 1777 "$zonepath/root/test/mnt-clone"
for attr in max-user-namespaces max-mount-namespaces; do
    maximum=64
    test "$phase:$attr" != user-zero:max-user-namespaces || maximum=0
    test "$phase:$attr" != mount-zero:max-mount-namespaces || maximum=0
    zonecfg -z lx-probe "select attr name=$attr; set value=$maximum; end;"
done
mount_switch=1
test "$phase" != disabled || mount_switch=0
printf '%s\n' 'lx_brand`lx_seccomp_experimental/W 1' \
    'lx_brand`lx_userns_experimental/W 1' \
    "lx_brand\`lx_mntns_experimental/W $mount_switch" | mdb -kw
zoneadm -z lx-probe boot
for variant in static dynamic; do
    probe=/test/mntns_clone
    test "$variant" != dynamic || probe=/test/dynamic/mntns_clone
    for uid in 0 65534; do
        for entry in libc raw; do
            modes="combined filter mount-only nested deny initial-mount badfs"
            test "$phase" = enabled || modes=combined
            for mode in $modes; do
                options=()
                test "$uid" = 0 || options+=(--drop)
                test "$entry" = libc || options+=(--raw)
                test "$mode" = combined || options+=(--"$mode")
                case "$phase" in
                    disabled) options+=(--expect 22);;
                    mount-zero|user-zero) options+=(--expect 28);;
                esac
                log=/lab/nsclone-$phase-$variant-$uid-$entry-$mode.log
                echo "NSCLONE_CASE_START phase=$phase variant=$variant uid=$uid entry=$entry mode=$mode"
                if ! zlogin -S lx-probe "$probe ${options[*]}" </dev/null > "$log" 2>&1; then
                    cat "$log"
                    exit 1
                fi
                cat "$log"
                grep -q '^MCL_PASS ' "$log"
                ktest run lx_namespace:mount:process_idle
                for ((native_exit = 0; native_exit < 32; native_exit++)); do
                    /usr/bin/true
                done
                ktest run lx_namespace:user:zone_budget_idle
                echo "NSCLONE_CASE_PASS phase=$phase variant=$variant uid=$uid entry=$entry mode=$mode"
            done
        done
    done
done
printf '%s\n' 'lx_brand`lx_seccomp_experimental/W 0' \
    'lx_brand`lx_userns_experimental/W 0' \
    'lx_brand`lx_mntns_experimental/W 0' | mdb -kw
zoneadm -z lx-probe halt
echo "LX_NSCLONE_PASS phase=$phase"
