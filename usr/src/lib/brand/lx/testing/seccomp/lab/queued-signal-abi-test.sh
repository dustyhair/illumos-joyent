#!/bin/bash
# Fresh VM124 only. Standalone: does not overwrite parent queued-thread logs.
set -eu
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test "$(zonename)" = global
test -f /lab/offline-lx-guest
run="/lab/queued-signal-abi-vm124.$(date -u +%Y%m%dT%H%M%SZ).$$"
mkdir "$run"
mkfifo "$run/completion"
exec 3<>"$run/completion"
(
    trap 'rc=$?; printf "%s\n" "$rc" >&3' EXIT
    set -eu
    bootparams | grep -q '^lx_lab=true$'
    zoneadm -z lx-probe list -p | grep -q ':running:'
    zonepath=$(zonecfg -z lx-probe info zonepath | awk '{print $2}')
    test -n "$zonepath"
    for item in i386/queued_signal32 dynamic/sigcont_matrix static/sigcont_matrix; do
        binary="$zonepath/root/test/queued-signal-abi/$item"
        actual=$(digest -a sha256 "$binary" | awk '{print $NF}')
        test "$actual" = "$(cat "$binary.sha256")"
        printf 'HASH %s %s\n' "$item" "$actual"
        variant=${item%%/*}
        zlogin -S lx-probe "/test/queued-signal-abi/$item" \
            </dev/null >"$run/$variant.raw.log" 2>&1
        cat "$run/$variant.raw.log"
        case "$variant" in
        i386) grep -qx 'PASS queued_signal32 layout=128 union=12 value=fedcba98' "$run/$variant.raw.log" ;;
        *) grep -qx 'PASS sigcont_matrix cases=9 reaped=18' "$run/$variant.raw.log" ;;
        esac
    done
    echo QUEUED_SIGNAL_ABI_VM124_PASS
) </dev/null >"$run/controller.raw.log" 2>&1 &
worker=$!
printf 'worker=%s logdir=%s deadline=75s\n' "$worker" "$run"
# Bash read supplies the controller deadline without a stale-PID killer.
# On timeout leave worker/zlogin/fixture state and logs for offline inspection.
if IFS= read -r -t 75 result <&3; then
    if wait "$worker"; then :; else :; fi
    cat "$run/controller.raw.log"
    printf '%s\n' "$result" >"$run/result"
    test "$result" = 0
else
    echo 'FAIL controller deadline; owned worker/fixture state retained' | tee "$run/TIMEOUT"
    exit 124
fi
