#!/bin/bash
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
test "$(zonename)" = global
zonepath=$(zonecfg -z lx-probe info zonepath | awk '{print $2}')
for variant in static dynamic; do
    dir="$zonepath/root/test/nested/$variant"
    test "$(digest -a sha256 "$dir/pid_nested" | awk '{print $NF}')" = "$(cat "$dir/sha256")"
done
wait_marker() {
    local i
    for ((i = 0; i < 15; i++)); do
        grep -q "$2" "$1" && return 0
        sleep 1
    done
    cat "$1"
    return 1
}
wait_stopped() {
    local i
    for ((i = 0; i < 15; i++)); do
        test "$(ps -p "$1" -o s | awk 'NR == 2 {print $1}')" = T && return 0
        sleep 1
    done
    return 1
}
input_test() {
    printf '%s\0' "$2" > /lab/pid-nested-input.bin
    ktest run -i /lab/pid-nested-input.bin "lx_namespace:pid:$1"
}
role() {
    wait_marker "$log" "^NEST_ROLE role=$1 "
    awk -v role="$1" '$1 == "NEST_ROLE" && $2 == "role="role {sub("pid=", "", $3); print $3; exit}' "$log"
}
watchdog=
held=0
cleanup() {
    test -z "$watchdog" || kill -TERM "$watchdog" 2>/dev/null || true
    # Failed VM checkpoint retains stopped tasks and held identities for audit.
    test "$held" = 0 || echo NEST_FAILURE_HELD_FIXTURE
}
trap cleanup EXIT
trap 'exit 124' TERM
printf '%s\n' 'lx_brand`lx_seccomp_experimental/W 1' \
    'lx_brand`lx_userns_experimental/W 1' \
    'lx_brand`lx_mntns_experimental/W 1' | mdb -kw
if ! zonecfg -z lx-probe info attr name=init-name | grep -q 'name: init-name'; then
    zonecfg -z lx-probe 'add attr; set name=init-name; set type=string; set value=/sbin/init; end;'
else
    zonecfg -z lx-probe 'select attr name=init-name; set type=string; set value=/sbin/init; end;'
fi
zoneadm -z lx-probe boot
for variant in static dynamic; do
    for clone in normal clone0; do
        for mode in normal kill-inner kill-outer; do
            opts=()
            test "$clone" != clone0 || opts+=(--clone0)
            test "$mode" = normal || opts+=("--$mode")
            log=/lab/pid-nested-$variant-$clone-$mode.log
            echo "NEST_CASE_START $variant $clone $mode"
            zlogin -S lx-probe "/test/nested/$variant/pid_nested ${opts[*]}" </dev/null > "$log" 2>&1 &
            runner=$!
            controller=$$
            ( sleep 55; echo NEST_TIMEOUT >&2; kill -TERM "$controller" ) </dev/null &
            watchdog=$!
            outer=$(role 0)
            wait_stopped "$outer"
            input_test process_attach "$outer"
            held=1
            kill -CONT "$outer"
            helper=$(role 1)
            wait_stopped "$helper"
            input_test nested_arm "$helper"
            kill -CONT "$helper"
            inner=$(role 2)
            wait_stopped "$inner"
            input_test nested_inner "$inner"
            kill -CONT "$inner"
            wait_marker "$log" '^NEST_READY '
            observer=$(awk '/^NEST_READY / {sub("observer=", "", $2); print $2; exit}' "$log")
            wait_stopped "$observer"
            live=$(role 3)
            zombie=$(role 4)
            input_test nested_live "$live"
            input_test nested_zombie "$zombie"
            kill -CONT "$observer"
            if test "$mode" != kill-outer; then
                wait_marker "$log" '^NEST_INNER_WAIT peers_alive=2$'
                # No completion retry, delay, process_dead or release first.
                ktest run lx_namespace:pid:nested_inner_drained
                wait_stopped "$observer"
                kill -CONT "$observer"
            fi
            wait "$runner"
            ktest run lx_namespace:pid:nested_drained
            grep -q '^NEST_PASS outside_wait_all_gone=5$' "$log"
            if grep -q NEST_FAIL "$log"; then exit 1; fi
            ktest run lx_namespace:pid:nested_release
            held=0
            ktest run lx_namespace:pid:process_idle
            ktest run lx_namespace:mount:process_idle
            for ((n = 0; n < 32; n++)); do /usr/bin/true; done
            ktest run lx_namespace:user:zone_budget_idle
            kill -TERM "$watchdog"
            wait "$watchdog" || true
            watchdog=
            cat "$log"
            echo "NEST_CASE_PASS $variant $clone $mode"
        done
    done
done
printf '%s\n' 'lx_brand`lx_seccomp_experimental/W 0' \
    'lx_brand`lx_userns_experimental/W 0' \
    'lx_brand`lx_mntns_experimental/W 0' | mdb -kw
zoneadm -z lx-probe halt
echo LX_PID_NESTED_PASS
