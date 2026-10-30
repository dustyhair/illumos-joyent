#!/bin/bash
# Only for the disposable, networkless lx acceptance VM.
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
test "$(zonename)" = global
if ! zpool list zones >/dev/null 2>&1; then
    zpool import zones
fi
test "$(digest -a sha256 /kernel/brand/amd64/lx_brand |
    awk '{print $NF}')" = \
    2dd36e5117c717aef7c0c33aca3c2cb92ff5786602480f6ea3835aa1404026a5
test "$(digest -a sha256 /lab/lx_namespace_ktest |
    awk '{print $NF}')" = \
    0ab8e4afb2756f65d20b44e5c8ed444ec87cbc2c5575fe5fdf6ab595261e1d1e
/lab/bpf-test.sh > /lab/mount-lifetime-bpf.log 2>&1
echo LIFETIME_BPF_EXIT=0
printf '%s\n' 'lx_brand`lx_mntns_taskq/J' \
    'lx_brand`lx_mntns_nviews/D' | mdb -k
sed 's@/lab/ktest run @/lab/ktest run -o RESULT,LINE,REASON @g' \
    /lab/namespace-test.sh > /lab/namespace-debug.sh
dtrace -x nolibs -qs /lab/mount-lifetime-trace.d \
    -c 'bash /lab/namespace-debug.sh' > /lab/mount-lifetime-direct.log 2>&1
# DTrace itself does not propagate the target command's result.
grep -qx LX_NAMESPACE_SNAPSHOT_PASS /lab/mount-lifetime-direct.log
awk '
    /^RETIRE_RETRY/ {
        if ($2 != "errno=16") bad=1
        split($3, n, "=")
        count += n[2]
    }
    END { exit bad || count < 2 }
' /lab/mount-lifetime-direct.log
echo LIFETIME_DIRECT_EXIT=0
/lab/enforcement-test.sh > /lab/mount-lifetime-enforcement.log 2>&1
echo LIFETIME_ENFORCEMENT_EXIT=0
/lab/userns-test.sh > /lab/mount-lifetime-userns.log 2>&1
echo LIFETIME_USERNS_EXIT=0
zoneadm -z lx-probe boot
printf '%s\n' 'lx_brand`lx_userns_experimental/W 1' | mdb -kw
# This remains a diagnostic of the actual Codex workspace sandbox.
/lab/bubblewrap-trace.sh > /lab/mount-lifetime-bubblewrap.log 2>&1
cat /lab/mount-lifetime-bubblewrap.log
printf '%s\n' 'lx_brand`lx_userns_experimental/W 0' \
    'lx_brand`lx_seccomp_experimental/W 0' | mdb -kw
zoneadm -z lx-probe halt
printf '%s\n' 'lx_brand`lx_mntns_nviews/D' \
    'lx_brand`lx_userns_experimental/D' \
    'lx_brand`lx_seccomp_experimental/D' | mdb -k > /lab/mount-lifetime-end.log
cat /lab/mount-lifetime-end.log
awk '$NF == "0" { n++ } END { exit n != 3 }' /lab/mount-lifetime-end.log
echo LX_MOUNT_LIFETIME_PASS
