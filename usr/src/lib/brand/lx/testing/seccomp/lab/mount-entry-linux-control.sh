#!/bin/bash
set -euo pipefail
work=$1
for variant in static dynamic; do
    probe=$work/mntns_entry
    test "$variant" != dynamic || probe=$work/dynamic/mntns_entry
    for uid in 0 65534; do
        drop=()
        test "$uid" = 0 || drop=(--drop)
        for mode in mount combined user-first drop-admin; do
            want=0
            test "$mode" != drop-admin || want=1
            if test "$mode" = mount && test "$uid" != 0; then want=1; fi
            log=$work/$variant-$uid-$mode.log
            "$probe" --linux --cwd "$work/cwd" "${drop[@]}" --mode "$mode" --errno "$want" > "$log" 2>&1
            grep -q '^MNE_PASS .*failures=0$' "$log"
            echo "LINUX_MNT_ENTRY_PASS variant=$variant uid=$uid mode=$mode"
        done
        for mode in mount combined; do
            want=0
            if test "$mode" = mount && test "$uid" != 0; then want=1; fi
            log=$work/$variant-$uid-$mode-filter.log
            "$probe" --linux --filter --cwd "$work/cwd" "${drop[@]}" --mode "$mode" --errno "$want" > "$log" 2>&1
            grep -q '^MNE_PASS .*failures=0$' "$log"
            echo "LINUX_MNT_ENTRY_PASS variant=$variant uid=$uid mode=$mode filter=1"
        done
    done
done
for variant in static dynamic; do
    probe=$work/mntns_entry
    test "$variant" != dynamic || probe=$work/dynamic/mntns_entry
    for mode in mount combined user-first; do
        for uid in 0 65534; do
            if test "$mode" = mount && test "$uid" != 0; then continue; fi
            drop=()
            test "$uid" = 0 || drop=(--drop)
            log=$work/$variant-$uid-$mode-nested.log
            "$probe" --linux --repeat --cwd "$work/cwd" "${drop[@]}" --mode "$mode" > "$log" 2>&1
            grep -q '^MNE_PASS .*failures=0$' "$log"
            echo "LINUX_MNT_ENTRY_PASS variant=$variant uid=$uid mode=$mode nested=1"
        done
    done
done
echo "LINUX_MNT_ENTRY_EVIDENCE=$work"
