#!/bin/bash
set -euo pipefail
cd /var/tmp/lx-namespace-clone-controls
for variant in static dynamic; do
    for uid in 0 65534; do
        for raw in libc raw; do
            for mode in combined filter mount-only nested deny initial-mount; do
                options=()
                test "$uid" = 0 || options+=(--drop)
                test "$raw" = libc || options+=(--raw)
                test "$mode" = combined || options+=(--"$mode")
                log="$variant-$uid-$raw-$mode.log"
                aa-exec -p lxc-unshare -- "$PWD/$variant" "${options[@]}" > "$log" 2>&1
                grep -q '^MCL_PASS ' "$log"
                echo "LINUX_NSCLONE_PASS variant=$variant uid=$uid raw=$raw mode=$mode"
            done
        done
    done
done
echo LINUX_NSCLONE_CONTROLS_PASS
