#!/bin/bash
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
test "$(zonename)" = global
zonepath=$(zonecfg -z lx-probe info zonepath | awk '{print $2}')
test "$(digest -a sha256 "$zonepath/root/test/mntns_private" | awk '{print $NF}')" = a2c6ae050dcdaf732650ad631af58c160d7f9ffa386b4d84bffb0b96466c2a8c
test "$(digest -a sha256 "$zonepath/root/test/dynamic/mntns_private" | awk '{print $NF}')" = 138b998d3ec698e5f56f6f31c2b292552d33d5091ecf4d9ba74d0654ea0f1287
mkdir -p "$zonepath/root/test/mnt-private"
chmod 1777 "$zonepath/root/test/mnt-private"
printf '%s\n' 'lx_brand`lx_seccomp_experimental/W 1' \
    'lx_brand`lx_userns_experimental/W 1' \
    'lx_brand`lx_mntns_experimental/W 1' | mdb -kw
zoneadm -z lx-probe boot
for variant in static dynamic; do
    probe=/test/mntns_private
    test "$variant" != dynamic || probe=/test/dynamic/mntns_private
    for uid in 0 65534; do
        for mode in allow deny; do
            options=
            test "$uid" = 0 || options=--drop
            test "$mode" != deny || options="$options --deny"
            log=/lab/mnt-private-$variant-$uid-$mode.log
            echo "MOUNT_PRIVATE_CASE_START variant=$variant uid=$uid mode=$mode"
            zlogin -S lx-probe "$probe $options" </dev/null > "$log" 2>&1
            cat "$log"
            grep -q '^MNP_PASS .*failures=0 ' "$log"
            test "$mode" != allow || grep -q '^MNP_PEER_PASS failures=0$' "$log"
            ktest run lx_namespace:mount:process_idle
            ktest run lx_namespace:user:zone_budget_idle
            echo "MOUNT_PRIVATE_CASE_PASS variant=$variant uid=$uid mode=$mode"
        done
    done
done
printf '%s\n' 'lx_brand`lx_seccomp_experimental/W 0' \
    'lx_brand`lx_userns_experimental/W 0' \
    'lx_brand`lx_mntns_experimental/W 0' | mdb -kw
zoneadm -z lx-probe halt
echo LX_MOUNT_PRIVATE_PASS
