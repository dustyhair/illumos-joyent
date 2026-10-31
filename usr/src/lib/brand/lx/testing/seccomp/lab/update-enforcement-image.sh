#!/bin/bash
set -euo pipefail
export PATH=/opt/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
lab=/build/workspaces/lx/lab
ws=/build/workspaces/lx/illumos
if ! test -f "$lab/boot_archive.before-enforce"; then
    cp "$lab/boot_archive" "$lab/boot_archive.before-enforce"
fi
lo=$(pfexec lofiadm -a "$lab/boot_archive")
usr_lo=
usr_mounted=0
cleanup() {
    if test "$usr_mounted" = 1; then
        pfexec umount "$lab/mnt-usr" || true
    fi
    if test -n "$usr_lo"; then
        pfexec lofiadm -d "$usr_lo" || true
    fi
    pfexec umount "$lab/mnt-root" || true
    pfexec lofiadm -d "$lo"
}
trap cleanup EXIT
pfexec mount -F ufs "$lo" "$lab/mnt-root"
test -f "$lab/mnt-root/lab/offline-lx-guest"
pfexec cp "$ws/usr/src/uts/i86pc/unix/obj64/unix" "$lab/mnt-root/platform/i86pc/kernel/amd64/unix"
# The bootloader payload must match the archive copy used by krtld.
pfexec cp "$lab/mnt-root/platform/i86pc/kernel/amd64/unix" "$lab/kernel-unix"
digest -a sha256 "$lab/kernel-unix" | awk '{print $NF}' > "$lab/boot-unix.sha256"
pfexec cp "$ws/usr/src/uts/intel/genunix/obj64/genunix" "$lab/mnt-root/kernel/amd64/genunix"
pfexec cp "$ws/usr/src/uts/intel/lofs/obj64/lofs" "$lab/mnt-root/kernel/fs/amd64/lofs"
pfexec cp "$ws/usr/src/uts/intel/tmpfs/obj64/tmpfs" "$lab/mnt-root/kernel/fs/amd64/tmpfs"
pfexec cp "$ws/usr/src/uts/intel/lx_brand/obj64/lx_brand" "$lab/mnt-root/kernel/brand/amd64/lx_brand"
pfexec cp "$ws/usr/src/uts/intel/tl/obj64/tl" "$lab/mnt-root/kernel/drv/amd64/tl"
pfexec cp "$lab/stream-cred-native" "$lab/mnt-root/lab/stream-cred-native"
pfexec chmod 0555 "$lab/mnt-root/lab/stream-cred-native"
pfexec cp "$lab/chown-racer-native" "$lab/mnt-root/lab/chown-racer-native"
pfexec cp "$lab/chown-race-test.sh" "$lab/mnt-root/lab/chown-race-test.sh"
pfexec cp "$lab/chown-acl-test.sh" "$lab/mnt-root/lab/chown-acl-test.sh"
pfexec cp "$lab/chmod-race-test.sh" "$lab/mnt-root/lab/chmod-race-test.sh"
pfexec cp "$lab/chmod-acl-test.sh" "$lab/mnt-root/lab/chmod-acl-test.sh"
pfexec cp "$lab/chmod-gid-race-test.sh" "$lab/mnt-root/lab/chmod-gid-race-test.sh"
pfexec chmod 0555 "$lab/mnt-root/lab/chmod-acl-test.sh" "$lab/mnt-root/lab/chmod-gid-race-test.sh"
pfexec chmod 0555 "$lab/mnt-root/lab/chown-racer-native" "$lab/mnt-root/lab/chown-race-test.sh" "$lab/mnt-root/lab/chown-acl-test.sh" "$lab/mnt-root/lab/chmod-race-test.sh"
pfexec cp "$ws/usr/src/uts/intel/lx_proc/obj64/lx_proc" "$lab/mnt-root/kernel/fs/amd64/lx_proc"
pfexec cp "$ws/usr/src/uts/intel/zfs/obj64/zfs" "$lab/mnt-root/kernel/fs/amd64/zfs"
pfexec mkdir -p "$lab/mnt-root/kernel/exec/amd64"
pfexec cp "$ws/usr/src/uts/intel/elfexec/obj64/elfexec" "$lab/mnt-root/kernel/exec/amd64/elfexec"
if test "${LX_LAB_UPDATE_USERLIBS:-0}" = 1 ||
    test "${LX_LAB_UPDATE_SUPPORT:-0}" = 1; then
    # SmartOS mounts /usr from this compressed UFS image during boot.
    # Follow tools/build_live: update the detached image, then compress it.
    usr_image="$lab/usr-lx.work"
    pfexec cp "$lab/mnt-root/usr.lgz" "$usr_image"
    pfexec lofiadm -U "$usr_image"
    usr_lo=$(pfexec lofiadm -a "$usr_image")
    mkdir -p "$lab/mnt-usr"
    pfexec mount -F ufs "$usr_lo" "$lab/mnt-usr"
    usr_mounted=1
    if test "${LX_LAB_UPDATE_USERLIBS:-0}" = 1; then
        test -f "$lab/mnt-usr/lib/lx_brand.so.1"
        test ! -L "$lab/mnt-usr/lib/lx_brand.so.1"
        test ! -L "$lab/mnt-usr/lib/amd64/lx_brand.so.1"
        pfexec cp "$ws/usr/src/lib/brand/lx/lx_brand/i386/lx_brand.so.1" \
            "$lab/mnt-usr/lib/lx_brand.so.1"
        pfexec cp "$ws/usr/src/lib/brand/lx/lx_brand/amd64/lx_brand.so.1" \
            "$lab/mnt-usr/lib/amd64/lx_brand.so.1"
        digest -a sha256 "$lab/mnt-usr/lib/lx_brand.so.1" \
            "$lab/mnt-usr/lib/amd64/lx_brand.so.1"
    fi
    if test "${LX_LAB_UPDATE_SUPPORT:-0}" = 1; then
        pfexec cp "$ws/usr/src/lib/brand/lx/lx_support/lx_support" \
            "$lab/mnt-usr/lib/brand/lx/lx_support"
        digest -a sha256 "$lab/mnt-usr/lib/brand/lx/lx_support"
    fi
    pfexec umount "$lab/mnt-usr"
    usr_mounted=0
    pfexec lofiadm -d "$usr_lo"
    usr_lo=
    pfexec lofiadm -C "$usr_image"
    # Release the old UFS allocation before writing its replacement.
    pfexec rm "$lab/mnt-root/usr.lgz"
    sync
    pfexec cp "$usr_image" "$lab/mnt-root/usr.lgz"
    pfexec digest -a sha256 "$usr_image" "$lab/mnt-root/usr.lgz"
fi
if test -f "$lab/userns-budget-native"; then
    pfexec cp "$lab/userns-budget-native" "$lab/mnt-root/lab/userns-budget-native"
    pfexec chmod 0555 "$lab/mnt-root/lab/userns-budget-native"
fi
pfexec cp "$lab/userns-entry-test.sh" "$lab/mnt-root/lab/userns-entry-test.sh"
pfexec chmod 0555 "$lab/mnt-root/lab/userns-entry-test.sh"
# Keep current mount controllers with the image instead of serial-uploading
# them for every boot. Each controller verifies its fixture binary hashes.
for controller in mntns-budget-native mount-owner-test.sh \
    mount-entry-test.sh mount-entry-full-test.sh \
    mount-bind-test.sh mount-bind-full-test.sh \
    mount-recursive-test.sh mount-recursive-full-test.sh \
    mount-private-test.sh mount-private-full-test.sh \
    mount-tmpfs-test.sh mount-tmpfs-full-test.sh \
    mount-statfs-test.sh mount-statfs-full-test.sh \
    mount-freeze-test.sh mount-freeze-full-test.sh \
    mount-detach-test.sh mount-detach-full-test.sh \
    mount-detach-smoke-test.sh \
    mount-retire-test.sh mount-retire-full-test.sh \
    mount-retire-smoke-test.sh \
    mount-pivot-test.sh mount-pivot-full-test.sh \
    mount-pivot-smoke-test.sh bubblewrap-trace.sh; do
    test -f "$lab/$controller"
    pfexec cp "$lab/$controller" "$lab/mnt-root/lab/$controller"
    pfexec chmod 0555 "$lab/mnt-root/lab/$controller"
done
pfexec cp "$lab/lx-root.tar" "$lab/mnt-root/lab/lx-root.tar"
pfexec cp "$lab/enforcement-test.sh" "$lab/mnt-root/lab/enforcement-test.sh"
pfexec chmod 0555 "$lab/mnt-root/lab/enforcement-test.sh"
if test -f "$ws/usr/src/uts/intel/lx_namespace_ktest/obj64/lx_namespace_ktest"; then
    pfexec cp "$ws/usr/src/uts/intel/lx_namespace_ktest/obj64/lx_namespace_ktest" "$lab/mnt-root/lab/lx_namespace_ktest"
    pfexec cp "$lab/namespace-test.sh" "$lab/mnt-root/lab/namespace-test.sh"
    pfexec cp "$lab/namespace-source.bin" "$lab/mnt-root/lab/namespace-source.bin"
    pfexec chmod 0555 "$lab/mnt-root/lab/namespace-test.sh"
    if test -f "$lab/userns-test.sh"; then
        pfexec cp "$lab/userns-test.sh" "$lab/mnt-root/lab/userns-test.sh"
        pfexec chmod 0555 "$lab/mnt-root/lab/userns-test.sh"
    fi
fi

digest -a sha256 "$lab/mnt-root/platform/i86pc/kernel/amd64/unix" "$lab/mnt-root/kernel/amd64/genunix" "$lab/mnt-root/kernel/brand/amd64/lx_brand" "$lab/mnt-root/kernel/fs/amd64/lx_proc" "$lab/mnt-root/kernel/fs/amd64/lofs" "$lab/mnt-root/kernel/fs/amd64/tmpfs" "$lab/mnt-root/kernel/exec/amd64/elfexec" "$lab/lx-root/test/enforcement_probe"
df -h "$lab/mnt-root"
