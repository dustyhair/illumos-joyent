#!/bin/bash
set -euo pipefail
lab=$(cd "$(dirname "$0")/lab" && pwd)
builder=jwagner@192.168.1.210
remote_lab=/build/workspaces/lx/lab
rsync --inplace -a "$builder:$remote_lab/boot_archive" "$lab/boot_archive"
rsync --inplace -a "$builder:$remote_lab/kernel-unix" "$lab/iso/platform/i86pc/kernel/amd64/unix"
rsync -a "$builder:$remote_lab/boot-unix.sha256" "$lab/boot-unix.sha256"
expected=$(cat "$lab/boot-unix.sha256")
actual=$(sha256sum "$lab/iso/platform/i86pc/kernel/amd64/unix" | cut -d' ' -f1)
if [[ "$expected" != "$actual" ]]; then
    echo "ISO unix does not match the boot archive manifest" >&2
    exit 1
fi
sha1sum "$lab/boot_archive" | cut -d' ' -f1 > "$lab/iso/platform/i86pc/amd64/boot_archive.hash"
xorriso -as mkisofs -quiet -R -b boot/cdboot -no-emul-boot -boot-info-table \
    -o "$lab/lx-lab.iso" "$lab/iso"
