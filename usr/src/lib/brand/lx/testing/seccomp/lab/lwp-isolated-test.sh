#!/bin/bash
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test "$(zonename)" = global
test -f /lab/offline-lx-guest
bootparams | grep -qx 'lx_lab=true'
# Exclusive run directory preserves failures and prevents a second run in this VM.
mkdir /lab/lwp-isolated-results
out=/lab/lwp-isolated-results
exec >"$out/controller.log" 2>&1
runner=
armed=0
phase=preflight
on_exit() {
    local rc=$?
    trap - EXIT
    if test "$rc" != 0; then
        echo "LWP_ISOLATED_FAIL rc=$rc phase=$phase runner=$runner armed=$armed"
        ps -ef >"$out/failure-ps.log" 2>&1 || true
        # Abort only through the held identity. Keep bookkeeping for diagnosis.
        if test -n "$runner" && test "$armed" = 1; then
            if ktest run lx_namespace:pid:lwp_abort >"$out/abort.log" 2>&1; then
                if wait_owned >"$out/abort-wait.log" 2>&1; then
                    echo 'Owned direct zlogin reaped after abort.'
                else
                    echo 'Abort wait failed or timed out; preserve VM.'
                fi
            fi
        fi
        echo 'Preserve VM and console. Follow HANDOFF.md recovery; do not retry.'
    fi
    exit "$rc"
}
trap on_exit EXIT
zonepath=$(zonecfg -z lx-probe info zonepath | awk '{print $2}')
test "$(digest -a sha256 "$zonepath/root/test/pid_ownership" | awk '{print $NF}')" = 4a40378f8c65c2457cc96f8468c9931fecbf781bdc428e2143635bc299b36e32
test "$(digest -a sha256 "$zonepath/root/test/dynamic/pid_ownership" | awk '{print $NF}')" = 33ee0aa8d8132efe239cc1cf3c872cc01e2dc7d96852a1620f89bca6e404ca5d
wait_marker() {
    local marker=$1 i
    for i in {1..15}; do
        grep -q "$marker" "$log" && return 0
        sleep 1
    done
    cat "$log"
    return 1
}
wait_stopped() {
    local i
    for i in {1..15}; do
        test "$(ps -p "$pid" -o s | awk 'NR == 2 {print $1}')" = T && return 0
        sleep 1
    done
    return 1
}
kt() { ktest run "lx_namespace:pid:$1"; }
# Bounded wait in this shell, so wait below reaps its own direct zlogin child.
wait_owned() {
    local i state
    for ((i = 0; i < 30; i++)); do
        state=$(ps -p "$runner" -o s | awk 'NR == 2 {print $1}')
        if test -z "$state" || test "$state" = Z; then
            wait "$runner" || return "$?"
            runner=
            return 0
        fi
        sleep 1
    done
    return 1
}
printf '%s\n' 'lx_brand`lx_seccomp_experimental/W 1' \
    'lx_brand`lx_userns_experimental/W 1' \
    'lx_brand`lx_mntns_experimental/W 1' | mdb -kw
zoneadm -z lx-probe boot
for variant in static dynamic; do
    probe=/test/pid_ownership
    test "$variant" != dynamic || probe=/test/dynamic/pid_ownership
    for uid in 0 65534; do
        for guard in 0 1; do
            caseid=$variant-$uid-$guard
            log=$out/$caseid.fixture.log
            options=
            test "$uid" = 0 || options="$options --drop"
            test "$guard" = 0 || options="$options --guard"
            echo "LWP_ISOLATED_START case=$caseid"
            phase=$caseid-launch
            zlogin -S lx-probe "$probe exit $options" </dev/null >"$log" 2>&1 &
            runner=$!
            wait_marker '^PID_PROCESS '
            pid=$(awk '/^PID_PROCESS / {sub("pid=", "", $2); print $2; exit}' "$log")
            [[ "$pid" =~ ^[0-9]+$ ]] && test "$pid" -gt 1
            wait_stopped
            printf '%s\0' "$pid" >"$out/$caseid.owner.bin"
            phase=$caseid-attach
            ktest run -i "$out/$caseid.owner.bin" lx_namespace:pid:process_attach
            ktest run -i "$out/$caseid.owner.bin" lx_namespace:pid:lwp_owner_hold
            armed=1
            phase=$caseid-inject
            ktest run -i "$out/$caseid.owner.bin" lx_namespace:pid:lwp_failure
            kt lwp_continue
            phase=$caseid-worker
            wait_marker '^PID_WORKER '
            tid=$(awk '/^PID_WORKER / {sub("tid=", "", $3); print $3; exit}' "$log")
            [[ "$tid" =~ ^[0-9]+$ ]] && test "$tid" -gt 1
            wait_stopped
            printf '%s\0' "$tid" >"$out/$caseid.worker.bin"
            ktest run -i "$out/$caseid.worker.bin" lx_namespace:pid:lwp_worker_hold
            kt lwp_continue
            phase=$caseid-retirement
            wait_marker '^PID_RETIRED '
            wait_stopped
            kt lwp_continue
            phase=$caseid-direct-wait
            wait_marker '^PID_PASS$'
            wait_owned
            # Single cleanup attempt. A post-wait race is a failure to preserve.
            phase=$caseid-cleanup
            kt lwp_cleanup
            armed=0
            kt process_idle
            ktest run lx_namespace:mount:process_idle
            ktest run lx_namespace:user:zone_budget_idle
            cat "$log"
            echo "LWP_ISOLATED_CASE_PASS case=$caseid"
        done
    done
done
printf '%s\n' 'lx_brand`lx_seccomp_experimental/W 0' \
    'lx_brand`lx_userns_experimental/W 0' \
    'lx_brand`lx_mntns_experimental/W 0' | mdb -kw
zoneadm -z lx-probe halt
phase="done"
echo LX_LWP_ISOLATED_PASS
