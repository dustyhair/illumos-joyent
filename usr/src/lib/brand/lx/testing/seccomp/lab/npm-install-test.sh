#!/bin/bash
# Runs only in this task's private, networkless SmartOS VM.
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
test "$(zonename)" = global
test "$(zoneadm -z lx-probe list -p | cut -d: -f3)" != running
for flag in lx_seccomp_experimental lx_userns_experimental; do
    printf 'lx_brand`%s/D\n' "$flag" | mdb -k > /lab/install-$flag.log
    cat /lab/install-$flag.log
    awk '$NF == "0" { found=1 } END { exit !found }' /lab/install-$flag.log
done
if ! test -f /mnt/lx-media/lab-install/npm-cache.tar; then
    mkdir -p /mnt/lx-media
    mount -F hsfs -o ro /dev/dsk/c2t0d0s2 /mnt/lx-media
fi
media=/mnt/lx-media/lab-install
root=/zones/lx-probe-zfs/root
run=/var/tmp/lx-npm-install-$$
mkdir -p "$root$run" "$root/opt/lx-zone-install/runtime"
chmod 0711 "$root$run"
gzip -dc "$media/node-npm-runtime.tar.gz" |
    (cd "$root/opt/lx-zone-install/runtime"; tar xf -)
cp "$media/npm-install-case.sh" "$root/opt/lx-zone-install/"
chmod 0555 "$root/opt/lx-zone-install/npm-install-case.sh"
cp "$media/npm-install-runner" "$media/zone_codex_audit" "$root/test/"
chmod 0555 "$root/test/npm-install-runner" "$root/test/zone_codex_audit"
# The minimal test root previously carried only libc and a native shell.
# Install the matching Linux fixture runtime before any Linux task starts.
cp "$root/opt/lx-zone-install/runtime/lib/"*.so.* "$root/lib/x86_64-linux-gnu/"
cp "$root/opt/lx-zone-install/runtime/lib/ld-linux-x86-64.so.2" \
    "$root/lib64/ld-linux-x86-64.so.2"
ln -sf /opt/lx-zone-install/runtime/bin/sh "$root/bin/sh"
ln -sf /opt/lx-zone-install/runtime/bin/env "$root/usr/bin/env"
ln -sf /opt/lx-zone-install/runtime/bin/node "$root/usr/bin/node"
for who in root user; do
    work=$root$run/$who
    mkdir -p "$work/cache" "$work/home"
    (cd "$work/cache"; tar xf "$media/npm-cache.tar")
    if test "$who" = user; then
        chown -R 65534:65534 "$work"
    fi
    chmod 0700 "$work" "$work/home"
done
cleanup() { zoneadm -z lx-probe halt; }
trap cleanup EXIT
zoneadm -z lx-probe boot
zlogin -S lx-probe "/test/npm-install-runner $run/root"
zlogin -S lx-probe "/test/npm-install-runner $run/user --drop"
cleanup
trap - EXIT
printf '%s\n' LX_NPM_INSTALL_PASS
