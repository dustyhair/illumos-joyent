#!/bin/bash
set -euo pipefail
cd /var/tmp/lx-mnt-freeze-controls
mkdir -p /test/mnt-freeze
chmod 1777 /test/mnt-freeze
for variant in static dynamic; do
    for uid in 0 65534; do
        for mode in allow deny; do
            options=()
            test "$uid" = 0 || options+=(--drop)
            test "$mode" != deny || options+=(--deny)
            log="$variant-$uid-$mode.log"
            aa-exec -p lxc-unshare -- "$PWD/$variant" --linux "${options[@]}" > "$log" 2>&1
            grep -q '^MNF_PASS .*failures=0 ' "$log"
            echo "LINUX_FREEZE_PASS variant=$variant uid=$uid mode=$mode"
        done
    done
done
echo LINUX_FREEZE_CONTROLS_PASS
