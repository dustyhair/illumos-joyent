#!/bin/bash
set -euo pipefail
cd /var/tmp/lx-mnt-pivot-controls
mkdir -p /test/mnt-pivot
chmod 1777 /test/mnt-pivot
for variant in static dynamic; do
    for uid in 0 65534; do
        for mode in allow deny; do
            options=()
            test "$uid" = 0 || options+=(--drop)
            test "$mode" != deny || options+=(--deny)
            log="$variant-$uid-$mode.log"
            aa-exec -p lxc-unshare -- "$PWD/$variant" "${options[@]}" > "$log" 2>&1
            grep -q '^MNP_PASS .*failures=0 ' "$log"
            echo "LINUX_PIVOT_PASS variant=$variant uid=$uid mode=$mode"
        done
    done
done
echo LINUX_PIVOT_CONTROLS_PASS
