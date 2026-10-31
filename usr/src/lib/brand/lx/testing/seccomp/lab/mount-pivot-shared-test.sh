#!/bin/bash
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
test "$(zonename)" = global
zonepath=$(zonecfg -z lx-probe info zonepath | awk '{print $2}')
test "$(digest -a sha256 /lab/mntns_pivot_shared | awk '{print $NF}')" = d9f08cdf3df0381788e8d1f428a71806000043e3f0d2b23cb50c151215d55f18
cp /lab/mntns_pivot_shared "$zonepath/root/test/dynamic/mntns_pivot_shared"
chmod 0555 "$zonepath/root/test/dynamic/mntns_pivot_shared"
mkdir -p "$zonepath/root/test/mnt-pivot"
chmod 1777 "$zonepath/root/test/mnt-pivot"
printf '%s\n' 'lx_brand`lx_seccomp_experimental/W 1' \
    'lx_brand`lx_userns_experimental/W 1' \
    'lx_brand`lx_mntns_experimental/W 1' | mdb -kw
zoneadm -z lx-probe boot
for variant in dynamic; do
    probe=/test/mntns_pivot
    test "$variant" != dynamic || probe=/test/dynamic/mntns_pivot_shared
    for uid in 0 65534; do
        for mode in shared; do
            options="--lx-shared --bind-root"
            test "$uid" = 0 || options="--drop --lx-shared --bind-root"
            test "$mode" != deny || options="$options --deny"
            log=/lab/mnt-pivot-guard-$variant-$uid-$mode.log
            echo "MOUNT_PIVOT_SHARED_CASE_START variant=$variant uid=$uid mode=$mode"
            zlogin -S lx-probe "$probe $options" </dev/null > "$log" 2>&1
            cat "$log"
            grep -q '^MNP_PASS .*failures=0 ' "$log"
            ktest run lx_namespace:mount:process_idle
            # Native dead threads retain t_cred until the batched reaper
            # runs. Generate more than its default reaplimit of 16 exits;
            # keep the existing zero-charge assertion and deadline intact.
            for ((native_exit = 0; native_exit < 32; native_exit++)); do
                /usr/bin/true
            done
            ktest run lx_namespace:user:zone_budget_idle
            echo "MOUNT_PIVOT_SHARED_CASE_PASS variant=$variant uid=$uid mode=$mode"
        done
    done
done
printf '%s\n' 'lx_brand`lx_seccomp_experimental/W 0' \
    'lx_brand`lx_userns_experimental/W 0' \
    'lx_brand`lx_mntns_experimental/W 0' | mdb -kw
zoneadm -z lx-probe halt
echo LX_MOUNT_PIVOT_SHARED_PASS
