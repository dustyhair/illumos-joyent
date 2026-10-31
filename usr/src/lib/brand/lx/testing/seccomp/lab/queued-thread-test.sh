#!/bin/bash
# Run in the fresh offline lab global zone after lx-probe is booted.
set -eu
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test "$(zonename)" = global
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
zoneadm -z lx-probe list -p | grep -q ':running:'
zonepath=$(zonecfg -z lx-probe info zonepath | awk '{print $2}')
for variant in dynamic static; do
    dir="$zonepath/root/test/queued-thread-signals/$variant"
    actual=$(digest -a sha256 "$dir/queued_threads" | awk '{print $NF}')
    test "$actual" = "$(cat "$dir/sha256")"
    log="/lab/queued-thread-signals-$variant.log"
    # The fixture bounds pipe waits, supervises numeric-UID children, and reaps.
    # A failed phase is fatal; retain the raw zlogin output.
    zlogin -S lx-probe "/test/queued-thread-signals/$variant/queued_threads" \
        </dev/null >"$log" 2>&1
    cat "$log"
    grep -q '^PASS queued_threads main=8 worker=8 zero=8192 reaped=5$' "$log"
done
echo QUEUED_THREAD_SIGNALS_PASS
