#!/bin/bash
set -euo pipefail
cd /var/tmp/lx-mnt-tmpfs-controls
mkdir -p /test/mnt-tmpfs
chmod 1777 /test/mnt-tmpfs
for variant in static dynamic; do
    for uid in 0 65534; do
        for mode in basic options deny; do
            options=()
            test "$uid" = 0 || options+=(--drop)
            test "$mode" != options || options+=(--options)
            test "$mode" != deny || options+=(--deny)
            log="$variant-$uid-$mode.log"
            aa-exec -p lxc-unshare -- "$PWD/$variant" --linux "${options[@]}" > "$log" 2>&1
            grep -q '^MNT_PASS .*failures=0 ' "$log"
            test "$mode" != basic || grep -q '^MNT_PEER_PASS failures=0$' "$log"
            test "$mode" != options || grep -q '^MNT_SIZE_PASS ' "$log"
            echo "LINUX_TMPFS_PASS variant=$variant uid=$uid mode=$mode"
        done
    done
done
echo LINUX_TMPFS_CONTROLS_PASS
