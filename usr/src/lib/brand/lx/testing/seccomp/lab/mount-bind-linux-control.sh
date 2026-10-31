#!/bin/bash
set -euo pipefail
cd /var/tmp/lx-mnt-bind-controls
mkdir -p /test/mnt-bind/denied
chmod 1777 /test/mnt-bind
chmod 0700 /test/mnt-bind/denied
touch /test/mnt-bind/denied/{source,target}
for variant in static dynamic; do
    for mode in combined mount-only denied mount-denied nobody nobody-denied; do
        options=()
        case "$mode" in
            combined) ;;
            mount-only) options=(--mount-only);;
            denied) options=(--drop-admin);;
            mount-denied) options=(--mount-only --drop-admin);;
            nobody) options=(--drop);;
            nobody-denied) options=(--drop --drop-admin);;
        esac
        aa-exec -p lxc-unshare -- "$PWD/$variant" --linux "${options[@]}" > "$variant-$mode.log" 2>&1
        grep -q '^MNB_PASS .*failures=0 ' "$variant-$mode.log"
        echo "LINUX_BIND_PASS variant=$variant mode=$mode"
    done
done
for mode in root nobody; do
    options=()
    test "$mode" != nobody || options=(--drop)
    aa-exec -p lxc-unshare -- "$PWD/static" --linux --fill "${options[@]}" > "static-fill-$mode.log" 2>&1
    grep -q '^MNB_FILL mounts=4097 failures=0$' "static-fill-$mode.log"
    grep -q '^MNB_PASS .*failures=0 ' "static-fill-$mode.log"
    echo "LINUX_BIND_FILL_PASS mode=$mode"
done
echo LINUX_BIND_CONTROLS_PASS
