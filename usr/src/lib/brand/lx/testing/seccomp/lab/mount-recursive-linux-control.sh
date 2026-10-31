#!/bin/bash
set -euo pipefail
cd /var/tmp/lx-mnt-recursive-controls
mkdir -p /test/mnt-recursive
chmod 1777 /test/mnt-recursive
for variant in static dynamic; do
    for uid in 0 65534; do
        for mode in recursive types deny; do
            options=()
            test "$uid" = 0 || options+=(--drop)
            test "$mode" != types || options+=(--types)
            test "$mode" != deny || options+=(--deny)
            log="$variant-$uid-$mode.log"
            aa-exec -p lxc-unshare -- "$PWD/$variant" --linux "${options[@]}" > "$log" 2>&1
            grep -q '^MNR_PASS .*failures=0 ' "$log"
            test "$mode" != recursive || grep -q '^MNR_OBSERVER_PASS ' "$log"
            echo "LINUX_RECURSIVE_PASS variant=$variant uid=$uid mode=$mode"
        done
    done
done
echo LINUX_RECURSIVE_CONTROLS_PASS
