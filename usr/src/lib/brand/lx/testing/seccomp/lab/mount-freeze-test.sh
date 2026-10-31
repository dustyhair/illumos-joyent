#!/bin/bash
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
test "$(zonename)" = global
zonepath=$(zonecfg -z lx-probe info zonepath | awk '{print $2}')
test "$(digest -a sha256 "$zonepath/root/test/mntns_freeze" | awk '{print $NF}')" = 61ec1043b16935c5d7a86ca64f77be9dabe4001239853bb7b88c7badeb54c90f
test "$(digest -a sha256 "$zonepath/root/test/dynamic/mntns_freeze" | awk '{print $NF}')" = 61f5b4debbff945ede5067b91713bf6e386042cc5dae4f093f52f09c478d941c
mkdir -p "$zonepath/root/test/mnt-freeze"
chmod 1777 "$zonepath/root/test/mnt-freeze"
printf '%s\n' 'lx_brand`lx_seccomp_experimental/W 1' \
    'lx_brand`lx_userns_experimental/W 1' \
    'lx_brand`lx_mntns_experimental/W 1' | mdb -kw
zoneadm -z lx-probe boot
for variant in static dynamic; do
    probe=/test/mntns_freeze
    test "$variant" != dynamic || probe=/test/dynamic/mntns_freeze
    for uid in 0 65534; do
        for mode in allow deny; do
            options=
            test "$uid" = 0 || options=--drop
            test "$mode" != deny || options="$options --deny"
            log=/lab/mnt-freeze-$variant-$uid-$mode.log
            echo "MOUNT_FREEZE_CASE_START variant=$variant uid=$uid mode=$mode"
            zlogin -S lx-probe "$probe $options" </dev/null > "$log" 2>&1
            cat "$log"
            grep -q '^MNF_PASS .*failures=0 ' "$log"
            ktest run lx_namespace:mount:process_idle
            ktest run lx_namespace:user:zone_budget_idle
            echo "MOUNT_FREEZE_CASE_PASS variant=$variant uid=$uid mode=$mode"
        done
    done
done
printf '%s\n' 'lx_brand`lx_seccomp_experimental/W 0' \
    'lx_brand`lx_userns_experimental/W 0' \
    'lx_brand`lx_mntns_experimental/W 0' | mdb -kw
zoneadm -z lx-probe halt
echo LX_MOUNT_FREEZE_PASS
