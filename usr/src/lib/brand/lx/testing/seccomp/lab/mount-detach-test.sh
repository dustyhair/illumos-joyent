#!/bin/bash
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
test "$(zonename)" = global
zonepath=$(zonecfg -z lx-probe info zonepath | awk '{print $2}')
test "$(digest -a sha256 "$zonepath/root/test/mntns_detach" | awk '{print $NF}')" = 257d4ac11b749991247a6a27bfd860384e99723cb2069913240f18d652b9df7b
test "$(digest -a sha256 "$zonepath/root/test/dynamic/mntns_detach" | awk '{print $NF}')" = 538c044fecbbd8e5678ff9b24c8a3a6c0a3706cf60c296e9b68b849f8250e612
mkdir -p "$zonepath/root/test/mnt-detach"
chmod 1777 "$zonepath/root/test/mnt-detach"
printf '%s\n' 'lx_brand`lx_seccomp_experimental/W 1' \
    'lx_brand`lx_userns_experimental/W 1' \
    'lx_brand`lx_mntns_experimental/W 1' | mdb -kw
zoneadm -z lx-probe boot
for variant in static dynamic; do
    probe=/test/mntns_detach
    test "$variant" != dynamic || probe=/test/dynamic/mntns_detach
    for uid in 0 65534; do
        for mode in allow deny; do
            options=
            test "$uid" = 0 || options=--drop
            test "$mode" != deny || options="$options --deny"
            log=/lab/mnt-detach-$variant-$uid-$mode.log
            echo "MOUNT_DETACH_CASE_START variant=$variant uid=$uid mode=$mode"
            zlogin -S lx-probe "$probe $options" </dev/null > "$log" 2>&1
            cat "$log"
            grep -q '^MND_PASS .*failures=0 ' "$log"
            ktest run lx_namespace:mount:process_idle
            ktest run lx_namespace:user:zone_budget_idle
            echo "MOUNT_DETACH_CASE_PASS variant=$variant uid=$uid mode=$mode"
        done
    done
done
printf '%s\n' 'lx_brand`lx_seccomp_experimental/W 0' \
    'lx_brand`lx_userns_experimental/W 0' \
    'lx_brand`lx_mntns_experimental/W 0' | mdb -kw
zoneadm -z lx-probe halt
echo LX_MOUNT_DETACH_PASS
