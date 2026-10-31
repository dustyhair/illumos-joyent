#!/bin/bash
# Exercise new detach cases before the longer regression wrapper.
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
test "$(zonename)" = global
bash /lab/guest-test.sh </dev/null > /lab/mnt-detach-smoke-guest.log 2>&1
if ! test -c /dev/ktest; then
    add_drv ktest
    devfsadm -i ktest
fi
bash /lab/namespace-test.sh </dev/null > /lab/mnt-detach-smoke-backend.log 2>&1
for attr in max-user-namespaces max-mount-namespaces; do
    if zonecfg -z lx-probe info attr name="$attr" | grep -q "name: $attr"; then
        zonecfg -z lx-probe "select attr name=$attr; set type=uint; set value=64; end;"
    else
        zonecfg -z lx-probe "add attr; set name=$attr; set type=uint; set value=64; end;"
    fi
done
bash /lab/mount-detach-test.sh </dev/null > /lab/mnt-detach-smoke-public.log 2>&1
echo LX_MOUNT_DETACH_SMOKE_PASS
