#!/bin/bash
set -u
for variant in static dynamic; do
    probe=/test/mntns_freeze
    test "$variant" != dynamic || probe=/test/dynamic/mntns_freeze
    for uid in 0 65534; do
        for mode in allow deny; do
            options=
            test "$uid" = 0 || options=--drop
            test "$mode" != deny || options="$options --deny"
            log=/lab/mnt-freeze-diag-$variant-$uid-$mode.log
            zlogin -S lx-probe "$probe $options" </dev/null > "$log" 2>&1
            result=$?
            echo "FREEZE_DIAGNOSTIC variant=$variant uid=$uid mode=$mode exit=$result"
            cat "$log"
        done
    done
done
echo FREEZE_DIAGNOSTIC_DONE
