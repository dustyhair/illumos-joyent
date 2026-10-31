#!/bin/bash
set -euo pipefail
cd /var/tmp/lx-mnt-statfs-controls
mkdir -p /test/mnt-statfs
chmod 1777 /test/mnt-statfs
for variant in static dynamic; do
    for uid in 0 65534; do
        options=()
        test "$uid" = 0 || options+=(--drop)
        log="$variant-$uid.log"
        aa-exec -p lxc-unshare -- "$PWD/$variant" --linux "${options[@]}" > "$log" 2>&1
        grep -q '^MNFS_PASS .*failures=0 ' "$log"
        echo "LINUX_STATFS_PASS variant=$variant uid=$uid"
    done
done
echo LINUX_STATFS_CONTROLS_PASS
