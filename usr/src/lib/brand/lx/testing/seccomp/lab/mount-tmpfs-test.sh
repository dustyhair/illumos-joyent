#!/bin/bash
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
test "$(zonename)" = global
zonepath=$(zonecfg -z lx-probe info zonepath | awk '{print $2}')
test "$(digest -a sha256 "$zonepath/root/test/mntns_tmpfs" | awk '{print $NF}')" = 68e276cfd687fc1b3e02e0ecdb82e23b0a3ec96327213454f328f2b61bc658a1
test "$(digest -a sha256 "$zonepath/root/test/dynamic/mntns_tmpfs" | awk '{print $NF}')" = d0057906a75d550ca4708cef57a8740dda52976e50e2295787cd195e18fd56ec
mkdir -p "$zonepath/root/test/mnt-tmpfs"
chmod 1777 "$zonepath/root/test/mnt-tmpfs"
printf '%s\n' 'lx_brand`lx_seccomp_experimental/W 1' \
    'lx_brand`lx_userns_experimental/W 1' \
    'lx_brand`lx_mntns_experimental/W 1' | mdb -kw
zoneadm -z lx-probe boot
for variant in static dynamic; do
    probe=/test/mntns_tmpfs
    test "$variant" != dynamic || probe=/test/dynamic/mntns_tmpfs
    for uid in 0 65534; do
        for mode in basic options deny; do
            options=
            test "$uid" = 0 || options=--drop
            test "$mode" != deny || options="$options --deny"
            test "$mode" != options || options="$options --options"
            log=/lab/mnt-tmpfs-$variant-$uid-$mode.log
            echo "MOUNT_TMPFS_CASE_START variant=$variant uid=$uid mode=$mode"
            zlogin -S lx-probe "$probe $options" </dev/null > "$log" 2>&1
            cat "$log"
            grep -q '^MNT_PASS .*failures=0 ' "$log"
            test "$mode" != basic || grep -q '^MNT_PEER_PASS failures=0$' "$log"
            test "$mode" != options || grep -q '^MNT_SIZE_PASS ' "$log"
            ktest run lx_namespace:mount:process_idle
            ktest run lx_namespace:user:zone_budget_idle
            echo "MOUNT_TMPFS_CASE_PASS variant=$variant uid=$uid mode=$mode"
        done
    done
done
printf '%s\n' 'lx_brand`lx_seccomp_experimental/W 0' \
    'lx_brand`lx_userns_experimental/W 0' \
    'lx_brand`lx_mntns_experimental/W 0' | mdb -kw
zoneadm -z lx-probe halt
echo LX_MOUNT_TMPFS_PASS
