#!/bin/bash
# Exercise cleanup after the reaper has run, followed by fresh module state.
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
test "$(zonename)" = global
test "$(zoneadm -z lx-probe list -p | cut -d: -f3)" = installed
run_mount_tests() {
    local output=$1
    dtrace -x nolibs -qs /lab/mount-lifetime-trace.d \
        -c 'bash /lab/namespace-debug.sh' > "$output" 2>&1
    grep -qx LX_NAMESPACE_SNAPSHOT_PASS "$output"
    awk '
        /^RETIRE_RETRY/ {
            if ($2 != "errno=16") bad=1
            split($3, n, "=")
            count += n[2]
        }
        END { exit bad || count < 2 }
    ' "$output"
}
module_id() {
    modinfo | awk -v name="$1" '$6 == name { print $1 }'
}
run_mount_tests /lab/mount-lifetime-before-reload.log
for module in lx_namespace_ktest lx_seccomp_ktest lx_proc lx_devfs lx_systrace; do
    id=$(module_id "$module")
    if test -n "$id"; then
        [[ "$id" =~ ^[0-9]+$ ]]
        modunload -i "$id"
        echo "LIFETIME_UNLOAD_${module}_EXIT=0"
    fi
done
id=$(module_id lx_brand)
[[ "$id" =~ ^[0-9]+$ ]]
modunload -i "$id"
echo LIFETIME_UNLOAD_BRAND_EXIT=0
test -z "$(module_id lx_brand)"
sleep 2
modload /kernel/brand/amd64/lx_brand
echo LIFETIME_RELOAD_BRAND_EXIT=0
printf '%s\n' 'lx_brand`lx_mntns_taskq/J' 'lx_brand`lx_mntns_nviews/D' \
    'lx_brand`lx_userns_experimental/D' \
    'lx_brand`lx_seccomp_experimental/D' | mdb -k > /lab/mount-lifetime-reload-state.log
cat /lab/mount-lifetime-reload-state.log
awk -F: '
    NF == 2 {
        gsub(/[ \t]/, "", $2)
        if ($2 == "0") n++
    }
    END { exit n != 4 }
' /lab/mount-lifetime-reload-state.log
run_mount_tests /lab/mount-lifetime-after-reload.log
echo LX_MOUNT_LIFETIME_RELOAD_PASS
