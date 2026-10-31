#!/bin/bash
set -euo pipefail
cd "${PIDNS_CONTROL_DIR:-/var/tmp/lx-pidns-controls}"
cc -std=c11 -Wall -Wextra -Werror -O2 -pthread -static pidns_lifecycle.c -o static
cc -std=c11 -Wall -Wextra -Werror -O2 -pthread pidns_lifecycle.c -o dynamic
for variant in static dynamic; do
    for uid in 0 65534; do
        for death in 0 1; do
            options=
            test "$uid" = 0 || options="$options --drop"
            test "$death" = 0 || options="$options --init-death"
            log="$variant-$uid-$death.log"
            aa-exec -p lxc-unshare -- "$PWD/$variant" $options > "$log" 2>&1
            grep -q "^PIDNS_PASS uid=$uid death=$death$" "$log"
            echo "LINUX_PIDNS_PASS variant=$variant uid=$uid death=$death"
        done
    done
done
echo LINUX_PIDNS_CONTROLS_PASS
