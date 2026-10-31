#!/bin/bash
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
test "$(zonename)" = global
zonepath=$(zonecfg -z lx-probe info zonepath | awk '{print $2}')
test "$(digest -a sha256 "$zonepath/root/test/mntns_retire" | awk '{print $NF}')" = 8f4911fb277a11f47472b45fea234c6f2c7adb12e4178d862146989ff7fdce1d
test "$(digest -a sha256 "$zonepath/root/test/dynamic/mntns_retire" | awk '{print $NF}')" = a8b1c84d3fde99f22934b850eccf30f510aa091a931afe46ed406db819cdcb42
mkdir -p "$zonepath/root/test/mnt-retire"
chmod 1777 "$zonepath/root/test/mnt-retire"
printf '%s\n' 'lx_brand`lx_seccomp_experimental/W 1' \
    'lx_brand`lx_userns_experimental/W 1' \
    'lx_brand`lx_mntns_experimental/W 1' | mdb -kw
zoneadm -z lx-probe boot
for variant in static dynamic; do
    probe=/test/mntns_retire
    test "$variant" != dynamic || probe=/test/dynamic/mntns_retire
    for uid in 0 65534; do
            options=
            test "$uid" = 0 || options=--drop
            log=/lab/mnt-retire-$variant-$uid.log
            echo "MOUNT_RETIRE_CASE_START variant=$variant uid=$uid"
            zlogin -S lx-probe "$probe $options" </dev/null > "$log" 2>&1
            cat "$log"
            grep -q '^MNX_PASS .*failures=0 ' "$log"
            ktest run lx_namespace:mount:process_idle
            ktest run lx_namespace:user:zone_budget_idle
            echo "MOUNT_RETIRE_CASE_PASS variant=$variant uid=$uid"
    done
done
printf '%s\n' 'lx_brand`lx_seccomp_experimental/W 0' \
    'lx_brand`lx_userns_experimental/W 0' \
    'lx_brand`lx_mntns_experimental/W 0' | mdb -kw
zoneadm -z lx-probe halt
echo LX_MOUNT_RETIRE_PASS
