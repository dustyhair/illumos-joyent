#!/bin/bash
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
test "$(zonename)" = global
/lab/namespace-test.sh
for suite in maps capabilities native lifetime; do
    /lab/ktest run "lx_namespace:user:$suite"
done
/lab/ktest run lx_namespace:user:cap_syscalls
/lab/ktest run lx_namespace:user:credentials
cleanup() {
    zoneadm -z lx-probe halt
}
trap cleanup EXIT
(cd /zones/lx-probe-zfs/root &&
    tar xf /lab/lx-root.tar ./test/proc_maps_baseline \
        ./test/cap_syscalls_probe ./test/cap_syscalls_probe32)
zoneadm -z lx-probe boot
/lab/ktest run lx_namespace:user:access
/lab/ktest run lx_namespace:user:proc_maps
zlogin -S lx-probe /test/proc_maps_baseline
zlogin -S lx-probe /test/cap_syscalls_probe
zlogin -S lx-probe '/test/cap_syscalls_probe --filter'
zlogin -S lx-probe /test/cap_syscalls_probe32
echo LX_CAPABILITIES32_EXIT=0
cleanup
trap - EXIT
/lab/ktest run lx_namespace:user:credentials
echo LX_USERNS_BACKEND_PASS
