#!/bin/bash
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
test "$(zonename)" = global
zonepath=$(zonecfg -z lx-probe info zonepath | awk '{print $2}')
test "$(digest -a sha256 "$zonepath/root/test/mntns_retire" | awk '{print $NF}')" = e4727eb2b064725edf6b54e920ced43274b050ff4a318f80803446a3649f3464
test "$(digest -a sha256 "$zonepath/root/test/dynamic/mntns_retire" | awk '{print $NF}')" = 233f082758c05d8a61c3628d7d46732eaefc3eab87a7cd8a39c54aee176b15f9
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
            # Native dead threads retain t_cred until the batched reaper
            # runs. Generate more than its default reaplimit of 16 exits;
            # keep the existing zero-charge assertion and deadline intact.
            for ((native_exit = 0; native_exit < 32; native_exit++)); do
                /usr/bin/true
            done
            ktest run lx_namespace:user:zone_budget_idle
            echo "MOUNT_RETIRE_CASE_PASS variant=$variant uid=$uid"
    done
done
printf '%s\n' 'lx_brand`lx_seccomp_experimental/W 0' \
    'lx_brand`lx_userns_experimental/W 0' \
    'lx_brand`lx_mntns_experimental/W 0' | mdb -kw
zoneadm -z lx-probe halt
echo LX_MOUNT_RETIRE_PASS
