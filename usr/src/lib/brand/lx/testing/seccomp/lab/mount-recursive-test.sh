#!/bin/bash
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
test "$(zonename)" = global
zonepath=$(zonecfg -z lx-probe info zonepath | awk '{print $2}')
test "$(digest -a sha256 "$zonepath/root/test/mntns_recursive" | awk '{print $NF}')" = 5854614600e94d0597e762ff945e975aebaffd321eb7722b104dc7183e400490
test "$(digest -a sha256 "$zonepath/root/test/dynamic/mntns_recursive" | awk '{print $NF}')" = cba2d288ef696dd1444c3d4d59d607417c9d59c69c50ddbba43b06545189a045
mkdir -p "$zonepath/root/test/mnt-recursive"
chmod 1777 "$zonepath/root/test/mnt-recursive"
printf '%s\n' 'lx_brand`lx_seccomp_experimental/W 1' \
    'lx_brand`lx_userns_experimental/W 1' \
    'lx_brand`lx_mntns_experimental/W 1' | mdb -kw
zoneadm -z lx-probe boot
for variant in static dynamic; do
    probe=/test/mntns_recursive
    test "$variant" != dynamic || probe=/test/dynamic/mntns_recursive
    for uid in 0 65534; do
        for mode in recursive types deny; do
            options=
            test "$uid" = 0 || options=--drop
            test "$mode" != types || options="$options --types"
            test "$mode" != deny || options="$options --deny"
            log=/lab/mnt-recursive-$variant-$uid-$mode.log
            echo "MOUNT_RECURSIVE_CASE_START variant=$variant uid=$uid mode=$mode"
            zlogin -S lx-probe "$probe $options" </dev/null > "$log" 2>&1
            cat "$log"
            grep -q '^MNR_PASS .*failures=0 ' "$log"
            test "$mode" != recursive || grep -q '^MNR_OBSERVER_PASS ' "$log"
            ktest run lx_namespace:mount:process_idle
            ktest run lx_namespace:user:zone_budget_idle
            echo "MOUNT_RECURSIVE_CASE_PASS variant=$variant uid=$uid mode=$mode"
        done
    done
done
printf '%s\n' 'lx_brand`lx_seccomp_experimental/W 0' \
    'lx_brand`lx_userns_experimental/W 0' \
    'lx_brand`lx_mntns_experimental/W 0' | mdb -kw
zoneadm -z lx-probe halt
echo LX_MOUNT_RECURSIVE_PASS
