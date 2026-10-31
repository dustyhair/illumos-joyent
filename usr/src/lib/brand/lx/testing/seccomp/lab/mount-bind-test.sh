#!/bin/bash
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
test "$(zonename)" = global
zonepath=$(zonecfg -z lx-probe info zonepath | awk '{print $2}')
test "$(digest -a sha256 "$zonepath/root/test/mntns_bind" | awk '{print $NF}')" = f99feeb5fdd3d7cf34b41161100a2bcfdc3e7d57026cc08ae6f7271461ab8535
test "$(digest -a sha256 "$zonepath/root/test/dynamic/mntns_bind" | awk '{print $NF}')" = 67f6f2b5b322cb2871750c74fc09d50642933a1da6433a3a6c7cc6c88434f6e8
mkdir -p "$zonepath/root/test/mnt-bind/denied"
chmod 1777 "$zonepath/root/test/mnt-bind"
chmod 0700 "$zonepath/root/test/mnt-bind/denied"
touch "$zonepath/root/test/mnt-bind/denied/"{source,target}
printf '%s\n' 'lx_brand`lx_seccomp_experimental/W 1' \
    'lx_brand`lx_userns_experimental/W 1' \
    'lx_brand`lx_mntns_experimental/W 1' | mdb -kw
zoneadm -z lx-probe boot
for variant in static dynamic; do
    probe=/test/mntns_bind
    test "$variant" != dynamic || probe=/test/dynamic/mntns_bind
    for mode in combined mount-only denied mount-denied nobody nobody-denied; do
        options=
        case "$mode" in
            combined) ;;
            mount-only) options=--mount-only;;
            denied) options=--drop-admin;;
            mount-denied) options='--mount-only --drop-admin';;
            nobody) options=--drop;;
            nobody-denied) options='--drop --drop-admin';;
        esac
        log=/lab/mnt-bind-$variant-$mode.log
        echo "MOUNT_BIND_CASE_START variant=$variant mode=$mode"
        zlogin -S lx-probe "$probe $options" </dev/null > "$log" 2>&1
        cat "$log"
        grep -q '^MNB_PASS .*failures=0 ' "$log"
        ktest run lx_namespace:mount:process_idle
        ktest run lx_namespace:user:zone_budget_idle
        echo "MOUNT_BIND_CASE_PASS variant=$variant mode=$mode"
    done
done
for mode in root nobody; do
    options=
    test "$mode" != nobody || options=--drop
    log=/lab/mnt-bind-fill-$mode.log
    echo "MOUNT_BIND_FILL_START mode=$mode"
    zlogin -S lx-probe "/test/mntns_bind --fill $options" </dev/null > "$log" 2>&1
    cat "$log"
    grep -q '^MNB_FILL mounts=[0-9]* failures=0$' "$log"
    grep -q '^MNB_PASS .*failures=0 ' "$log"
    ktest run lx_namespace:mount:process_idle
    ktest run lx_namespace:user:zone_budget_idle
    echo "MOUNT_BIND_FILL_PASS mode=$mode"
done
printf '%s\n' 'lx_brand`lx_seccomp_experimental/W 0' \
    'lx_brand`lx_userns_experimental/W 0' \
    'lx_brand`lx_mntns_experimental/W 0' | mdb -kw
zoneadm -z lx-probe halt
echo LX_MOUNT_BIND_PASS
