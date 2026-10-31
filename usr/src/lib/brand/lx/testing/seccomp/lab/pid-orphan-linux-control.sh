#!/bin/bash
set -euo pipefail
cd "${PIDNS_CONTROL_DIR:-/var/tmp/lx-pidns-controls}"
cc -std=c11 -Wall -Wextra -Werror -O2 -static pid_orphan.c -o orphan-static
cc -std=c11 -Wall -Wextra -Werror -O2 pid_orphan.c -o orphan-dynamic
for variant in static dynamic; do
    for uid in 0 65534; do
        for group in 0 1; do
            for clone in 0 1; do
                options=
                test "$uid" = 0 || options="$options --drop"
                test "$group" = 0 || options="$options --group"
                test "$clone" = 0 || options="$options --clone-none"
                log="orphan-$variant-$uid-$group-$clone.log"
                aa-exec -p lxc-unshare -- "$PWD/orphan-$variant" --linux $options > "$log" 2>&1
                grep -q '^PID_ADOPT_PASS owner=1 child=2 descendant=3$' "$log"
                grep -q '^PID_ORPHAN_EXIT pid=3 parent=1$' "$log"
                grep -q '^PID_PASS$' "$log"
                echo "LINUX_ORPHAN_PASS variant=$variant uid=$uid group=$group clone_none=$clone"
            done
        done
    done
done
