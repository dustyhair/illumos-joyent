#!/bin/bash
# Only for a later disposable offline guest. No kernel policy writes.
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
base=/lab/pgrp-zone
peer=lx-pgrp-peer
primary=lx-probe
dataset=zones/lx-pgrp-peer
peerpath=/zones/lx-pgrp-peer
fail() { echo "FAIL $*" >&2; exit 1; }
guards() {
    test "$(uname -s)" = SunOS
    test "$(id -u)" = 0
    test "$(zonename)" = global
    test -f /lab/offline-lx-guest
    bootparams | grep -qx 'lx_lab=true'
    test -f /lab/lx-root.tar
    test "${PGRP_FRESH_DISPOSABLE_VM:-}" = reviewed-pgid-image
}
state() { zoneadm -z "$1" list -p | awk -F: '{print $3}'; }
config() { zonecfg -z "$1" export; }
health() {
    zpool status -x zones | tee "$run/$1.pool"
    grep -qx "pool 'zones' is healthy" "$run/$1.pool"
    printf '%s\n' 'lx_brand`lx_pidns_nspaces/D' | mdb -k > "$run/$1.pidns"
    cat "$run/$1.pidns"
    test "$(awk 'NF {v=$NF} END {print v}' "$run/$1.pidns")" = 0
    zoneadm list -cp > "$run/$1.zones"
    ps -eo zone,pid,ppid,args > "$run/$1.processes"
}
verify_primary() {
    config "$primary" > "$run/primary.current"
    cmp "$run/primary.cfg" "$run/primary.current"
    test "$(zonecfg -z "$primary" info zonepath | awk '{print $2}')" = "$primarypath"
}
verify_peer() {
    config "$peer" > "$run/peer.current"
    cmp "$run/peer.cfg" "$run/peer.current"
    test "$(zfs get -H -o value mountpoint "$dataset")" = "$peerpath"
    test "$(zfs get -H -o value guid "$dataset")" = "$(cat "$run/dataset.guid")"
}
verify_bins() {
    local root=$1 v file expected
    for v in pgrp-zone.static pgrp-zone.dynamic ld-linux-x86-64.so.2 libc.so.6; do
        file="$root/test/pgrp-zone/$v"
        expected=$(cat "$base/lx/$v.sha256")
        test "$(digest -a sha256 "$file" | awk '{print $NF}')" = "$expected"
        printf '%s %s\n' "$expected" "$file"
    done
}
setup_phase() {
    test "$(state "$primary")" = running
    test "$(zonecfg -z "$primary" info brand | awk '{print $2}')" = lx
    zonecfg -z "$primary" info attr name=init-name > "$run/primary.init"
    grep -q 'value: /sbin/init' "$run/primary.init"
    zoneadm list -cp > "$run/zones.precreate"
    if awk -F: '{print $2}' "$run/zones.precreate" | grep -qx "$peer"; then fail "peer exists"; fi
    if zonecfg -z "$peer" info >/dev/null 2>&1; then fail "peer config exists"; fi
    if zfs list "$dataset" >/dev/null 2>&1; then fail "peer dataset exists"; fi
    test ! -e "$peerpath"
    test ! -e "$primarypath/root/test/pgrp-zone"
    config "$primary" > "$run/primary.cfg"
    zoneadm -z "$primary" list -p > "$run/primary.identity"
    health before
    # The root archive is the same known offline root used by lx-probe.
    digest -a sha256 /lab/lx-root.tar > "$run/root.sha256"
    echo "create dataset=$dataset mountpoint=$peerpath" >> "$run/created"
    zfs create -o mountpoint="$peerpath" "$dataset"
    zfs get -H -o value guid "$dataset" > "$run/dataset.guid"
    echo "create zone=$peer" >> "$run/created"
    zonecfg -z "$peer" 'create -b; set brand=lx; set zonepath=/zones/lx-pgrp-peer; set autoboot=false; set ip-type=exclusive; add attr; set name=kernel-version; set type=string; set value=5.10.0; end; add attr; set name=init-name; set type=string; set value=/sbin/init; end;'
    config "$peer" > "$run/peer.cfg"
    mkdir "$peerpath/root"
    chmod 0700 "$peerpath"
    (cd "$peerpath/root"; tar xf /lab/lx-root.tar)
    # Record owned paths before the first copy; never overwrite an old suite.
    for root in "$primarypath/root" "$peerpath/root"; do
        test ! -e "$root/test/pgrp-zone"
        echo "create fixtures=$root/test/pgrp-zone" >> "$run/created"
        mkdir "$root/test/pgrp-zone"
        cp "$base"/lx/* "$root/test/pgrp-zone/"
        verify_bins "$root"
    done
    zoneadm -z "$peer" mark -F installed
    touch "$run/SETUP_OK"
}
read_peer() {
    local wanted=$1 line
    while IFS= read -r -t 10 line <&4; do
        printf '%s\n' "$line" >> "$run/$pv.peer.stdout"
        case "$line" in *FAIL*) fail "peer assertion";; esac
        case "$line" in "$wanted"*) event=$line; return;; esac
    done
    fail "peer deadline/EOF, expected $wanted"
}
fixture_command() {
    fixture="/test/pgrp-zone/pgrp-zone.$1"
    if test "$1" = dynamic; then
        fixture="/test/pgrp-zone/ld-linux-x86-64.so.2 --library-path /test/pgrp-zone $fixture"
    fi
}
run_phase() {
    test -f "$run/SETUP_OK"
    test ! -f "$run/RUN_STARTED"
    verify_primary; verify_peer
    test "$(state "$primary")" = running
    test "$(state "$peer")" = installed
    # Do not take ownership of a replacement primary zone instance.
    zoneadm -z "$primary" list -p > "$run/primary.now"
    cmp "$run/primary.identity" "$run/primary.now"
    verify_bins "$primarypath/root"; verify_bins "$peerpath/root"
    touch "$run/RUN_STARTED"
    zoneadm -z "$peer" boot
    zoneadm -z "$peer" list -p > "$run/peer.identity"
    for pv in static dynamic; do
        mkfifo "$run/$pv.in" "$run/$pv.out"
        exec 3<>"$run/$pv.in"
        exec 4<>"$run/$pv.out"
        fixture_command "$pv"
        zlogin -S "$peer" "$fixture peer" < "$run/$pv.in" > "$run/$pv.out" 2> "$run/$pv.peer.stderr" &
        peer_child=$!
        echo "held direct zlogin child=$peer_child variant=$pv" >> "$run/owned"
        for mode in live leaderless; do
            test "$mode" != leaderless || printf R >&3
            read_peer "READY $mode "
            pgid=${event##* }
            [[ "$pgid" =~ ^[0-9]+$ ]] && test "$pgid" -gt 1
            for cv in static dynamic; do
                log="$run/$pv.$mode.$cv.caller.raw"
                fixture_command "$cv"
                zlogin -S "$primary" "$fixture caller $pgid" </dev/null > "$log" 2>&1
                cat "$log"
                grep -qx 'RESULT failures=0' "$log"
                test "$(grep -c ' PASS$' "$log")" = 14
                if grep -q FAIL "$log"; then fail "caller assertion"; fi
                printf C >&3
                read_peer "INTACT "
                test "$event" = "INTACT $pgid"
            done
        done
        printf X >&3
        read_peer 'RESULT failures=0'
        test "$event" = 'RESULT failures=0'
        # Peer reports after reaping all children. Phase deadline also bounds wait.
        wait "$peer_child"
        exec 3>&- 4>&-
    done
    health after_run
    touch "$run/RUN_OK"
    echo PGRP_ZONE_RUNTIME_PASS
}
cleanup_phase() {
    test -f "$run/SETUP_OK" || fail 'partial setup: inspect created journal manually'
    verify_primary; verify_peer
    # Halt by zone identity, never signal historical numeric fixture IDs.
    for z in "$peer" "$primary"; do
        s=$(state "$z")
        if test "$s" = running; then
            identity=primary
            test "$z" != "$peer" || identity=peer
            zoneadm -z "$z" list -p > "$run/$identity.cleanup-identity"
            cmp "$run/$identity.identity" "$run/$identity.cleanup-identity"
            zoneadm -z "$z" halt
        fi
        test "$(state "$z")" = installed
    done
    verify_primary; verify_peer
    health after_halt
    ps -eo zone,args > "$run/tasks.after-halt"
    awk '$1 == "lx-probe" || $1 == "lx-pgrp-peer"' "$run/tasks.after-halt" > "$run/remaining-zone-tasks"
    test ! -s "$run/remaining-zone-tasks"
    touch "$run/CLEANUP_OK"
    echo PGRP_ZONE_HALTED_CONFIGS_PRESERVED
}
guards
phase=${1:-}
run=${2:-}
case "$run" in "$base"/run.*) ;; *) fail "run path must be $base/run.UNIQUE";; esac
case "$run" in *..*|*[!a-zA-Z0-9_./-]*) fail 'unsafe run path';; esac
case "$phase" in setup|run|cleanup) ;; *) fail 'usage: controller setup|run|cleanup /lab/pgrp-zone/run.UNIQUE';; esac
if test "$phase" = setup; then mkdir "$run"; else test -d "$run"; fi
primarypath=$(zonecfg -z "$primary" info zonepath | awk '{print $2}')
case "$primarypath" in /zones/lx-probe|/zones/lx-probe-zfs) ;; *) fail "unexpected primary zonepath";; esac
completion="$run/$phase.completion"
mkfifo "$completion"
exec 9<>"$completion"
(
    trap 'rc=$?; printf "%s\n" "$rc" >&9' EXIT
    "${phase}_phase"
) </dev/null > "$run/$phase.raw" 2>&1 &
worker=$!
echo "phase=$phase held_worker=$worker run=$run deadline=180s"
if IFS= read -r -t 180 rc <&9; then
    wait "$worker" || true
    cat "$run/$phase.raw"
    printf '%s\n' "$rc" > "$run/$phase.result"
    test "$rc" = 0
else
    echo "FAIL timeout phase=$phase; preserve state and inspect live worker before cleanup" | tee "$run/$phase.TIMEOUT"
    exit 124
fi
