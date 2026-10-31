#!/bin/bash
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
test "$(zonename)" = global
zonepath=$(zonecfg -z lx-probe info zonepath | awk '{print $2}')
test "$(digest -a sha256 "$zonepath/root/test/mntns_pivot" | awk '{print $NF}')" = 9955a76a365b3449cce78a18f88374678099a23f70f6b5ec1cd33e1cf72cf20f
test "$(digest -a sha256 "$zonepath/root/test/dynamic/mntns_pivot" | awk '{print $NF}')" = 8a8538d32872730b0da68c77a8e07497c24815b176a8303a497e0277258fa96c
mkdir -p "$zonepath/root/test/mnt-pivot"
chmod 1777 "$zonepath/root/test/mnt-pivot"
printf '%s\n' 'lx_brand`lx_seccomp_experimental/W 1' \
    'lx_brand`lx_userns_experimental/W 1' \
    'lx_brand`lx_mntns_experimental/W 1' | mdb -kw
zoneadm -z lx-probe boot
for variant in static dynamic; do
    probe=/test/mntns_pivot
    test "$variant" != dynamic || probe=/test/dynamic/mntns_pivot
    for uid in 0 65534; do
        for mode in allow deny; do
            options=
            test "$uid" = 0 || options=--drop
            test "$mode" != deny || options="$options --deny"
            log=/lab/mnt-pivot-$variant-$uid-$mode.log
            echo "MOUNT_PIVOT_CASE_START variant=$variant uid=$uid mode=$mode"
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
            echo "MOUNT_PIVOT_CASE_PASS variant=$variant uid=$uid mode=$mode"
        done
    done
done
printf '%s\n' 'lx_brand`lx_seccomp_experimental/W 0' \
    'lx_brand`lx_userns_experimental/W 0' \
    'lx_brand`lx_mntns_experimental/W 0' | mdb -kw
zoneadm -z lx-probe halt
echo LX_MOUNT_PIVOT_PASS
