#!/bin/bash
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
test "$(zonename)" = global
zonepath=$(zonecfg -z lx-probe info zonepath | awk '{print $2}')
test "$(digest -a sha256 "$zonepath/root/test/mntns_statfs" | awk '{print $NF}')" = 27f94f3b07d0ca4b5b587e38095eda15942384dea6e14734b43e4f10d6341208
test "$(digest -a sha256 "$zonepath/root/test/dynamic/mntns_statfs" | awk '{print $NF}')" = 715f423562f9c9f6070d361f20b679e40c0d1e231138da12ff4c82d757294046
mkdir -p "$zonepath/root/test/mnt-statfs"
chmod 1777 "$zonepath/root/test/mnt-statfs"
printf '%s\n' 'lx_brand`lx_seccomp_experimental/W 1' \
    'lx_brand`lx_userns_experimental/W 1' \
    'lx_brand`lx_mntns_experimental/W 1' | mdb -kw
zoneadm -z lx-probe boot
for variant in static dynamic; do
    probe=/test/mntns_statfs
    test "$variant" != dynamic || probe=/test/dynamic/mntns_statfs
    for uid in 0 65534; do
            options=
            test "$uid" = 0 || options=--drop
            log=/lab/mnt-statfs-$variant-$uid.log
            echo "MOUNT_STATFS_CASE_START variant=$variant uid=$uid"
            zlogin -S lx-probe "$probe $options" </dev/null > "$log" 2>&1
            cat "$log"
            grep -q '^MNFS_PASS .*failures=0 ' "$log"
            ktest run lx_namespace:mount:process_idle
            ktest run lx_namespace:user:zone_budget_idle
            echo "MOUNT_STATFS_CASE_PASS variant=$variant uid=$uid"
    done
done
printf '%s\n' 'lx_brand`lx_seccomp_experimental/W 0' \
    'lx_brand`lx_userns_experimental/W 0' \
    'lx_brand`lx_mntns_experimental/W 0' | mdb -kw
zoneadm -z lx-probe halt
echo LX_MOUNT_STATFS_PASS
