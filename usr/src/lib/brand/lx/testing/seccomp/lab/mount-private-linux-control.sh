#!/bin/bash
set -euo pipefail
cd /var/tmp/lx-mnt-private-controls
mkdir -p /test/mnt-private
chmod 1777 /test/mnt-private
for variant in static dynamic; do
    for uid in 0 65534; do
        for mode in allow deny; do
            options=()
            test "$uid" = 0 || options+=(--drop)
            test "$mode" != deny || options+=(--deny)
            log="$variant-$uid-$mode.log"
            aa-exec -p lxc-unshare -- "$PWD/$variant" --linux "${options[@]}" > "$log" 2>&1
            grep -q '^MNP_PASS .*failures=0 ' "$log"
            test "$mode" != allow || grep -q '^MNP_PEER_PASS failures=0$' "$log"
            echo "LINUX_PRIVATE_PASS variant=$variant uid=$uid mode=$mode"
        done
    done
done
echo LINUX_PRIVATE_CONTROLS_PASS
