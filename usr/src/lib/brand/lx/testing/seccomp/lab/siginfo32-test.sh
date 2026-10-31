#!/bin/bash
# Parent-only candidate runtime. Run from VM125 global zone, never on builder.
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test "$(zonename)" = global
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
zoneadm -z lx-probe list -p | grep -q ':running:'
zonepath=$(zonecfg -z lx-probe info zonepath | awk '{print $2}')
base=/lab
run="$base/run-vm125.$(date -u +%Y%m%dT%H%M%SZ).$$"
mkdir "$run"
mkfifo "$run/completion"
exec 3<>"$run/completion"
(
    trap 'rc=$?; printf "%s\n" "$rc" >&3' EXIT
    check_hash() {
        test "$(digest -a sha256 "$1" | awk '{print $NF}')" = "$(cat "${2:-$1.sha256}")"
    }
    # These are SunOS executables. They must execute in the native global zone.
    for bits in 32 64; do
        b="$base/native_errno$bits"
        check_hash "$b"
        "$b" </dev/null >"$run/native$bits.raw.log" 2>&1
        cat "$run/native$bits.raw.log"
        grep -qx "PASS native_errno bits=$bits queue=128 kill=128 empty=257" "$run/native$bits.raw.log"
    done
    # Linux executables must enter the LX zone. No nativeexec substitution.
    for b in queued_signal32 queued_threads-dynamic queued_threads-static sigcont_matrix-dynamic sigcont_matrix-static; do
        case "$b" in
        queued_signal32) path=/test/queued-signal-abi/i386/queued_signal32; hash="$zonepath/root$path.sha256" ;;
        queued_threads-*) variant=${b#queued_threads-}; path="/test/queued-thread-signals/$variant/queued_threads"; hash="$zonepath/root/test/queued-thread-signals/$variant/sha256" ;;
        sigcont_matrix-*) variant=${b#sigcont_matrix-}; path="/test/queued-signal-abi/$variant/sigcont_matrix"; hash="$zonepath/root$path.sha256" ;;
        esac
        check_hash "$zonepath/root$path" "$hash"
        zlogin -S lx-probe "$path" </dev/null >"$run/$b.raw.log" 2>&1
        cat "$run/$b.raw.log"
        case "$b" in
        queued_signal32) expected='PASS queued_signal32 layout=128 union=12 value=fedcba98' ;;
        queued_threads-*) expected='PASS queued_threads main=8 worker=8 zero=8192 reaped=5' ;;
        sigcont_matrix-*) expected='PASS sigcont_matrix cases=9 reaped=18' ;;
        esac
        grep -qx "$expected" "$run/$b.raw.log"
    done
    echo SIGINFO32_ERRNO_VM125_PASS
) </dev/null >"$run/controller.raw.log" 2>&1 &
worker=$!
echo "worker=$worker logdir=$run deadline=240s"
# No detached stale-PID killer. Timeout is failure, retained for parent inspection.
if IFS= read -r -t 240 result <&3; then
    wait "$worker" || true
    cat "$run/controller.raw.log"
    printf '%s\n' "$result" >"$run/result"
    test "$result" = 0
else
    echo 'FAIL deadline: inspect owned worker/zlogin/fixture in offline VM before shutdown' | tee "$run/TIMEOUT"
    exit 124
fi
