#!/bin/bash
# Runs only inside the disposable, networkless QEMU guest.
set -euo pipefail
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
test "$(zonename)" = global
echo LX_LAB_START
uname -a
digest -a sha256 /kernel/amd64/genunix /kernel/brand/amd64/lx_brand
diskinfo
if ! zpool list zones >/dev/null 2>&1; then
    zpool import -f zones >/dev/null 2>&1 || true
fi
if ! zpool list zones >/dev/null 2>&1; then
    # The launcher supplies one blank, disposable 8 GiB SATA test disk.
    set -- $(diskinfo -Hp | awk '$6 == "no" {print $2}')
    test "$#" = 1
    disk=$1
    test -c "/dev/rdsk/${disk}p0"
    zpool create -f zones "$disk"
fi
if ! zonecfg -z lx-probe info >/dev/null 2>&1; then
    zonecfg -z lx-probe 'create -b; set brand=lx; set zonepath=/zones/lx-probe; set autoboot=false; set ip-type=exclusive; add attr; set name=kernel-version; set type=string; set value=5.10.0; end;'
fi
test "$(zoneadm -z lx-probe list -p | cut -d: -f3)" != running
mkdir -p /zones/lx-probe/root
chmod 0700 /zones/lx-probe
cd /zones/lx-probe/root
tar xf /lab/lx-root.tar
zoneadm -z lx-probe mark -F installed
zoneadm -z lx-probe boot
zoneadm list -cv
modinfo | egrep 'genunix|lx_brand'
set +e
zlogin -S lx-probe /test/no_new_privs_probe /test/identity_probe /test/native_identity_probe /test/native_privilege_probe
nnp_result=$?
echo "LX_NNP_EXIT=$nnp_result"
zlogin -S lx-probe /test/seccomp_probe
seccomp_result=$?
echo "LX_SECCOMP_EXIT=$seccomp_result"
set -e
zoneadm -z lx-probe halt
if [[ $nnp_result != 0 || $seccomp_result != 77 ]]; then
    echo LX_LAB_FAIL
    exit 1
fi
echo LX_LAB_PASS
