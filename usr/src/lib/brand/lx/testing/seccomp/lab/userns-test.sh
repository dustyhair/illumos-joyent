#!/bin/bash
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
test "$(zonename)" = global
/lab/namespace-test.sh
for suite in maps capabilities native lifetime budget budget_race; do
    /lab/ktest run "lx_namespace:user:$suite"
done
/lab/ktest run lx_namespace:user:cap_syscalls
/lab/ktest run lx_namespace:user:credentials
cleanup() {
    zoneadm -z lx-probe halt
}
trap cleanup EXIT
(cd /zones/lx-probe-zfs/root &&
    tar xf /lab/lx-root.tar ./test/proc_maps_baseline \
        ./test/cap_syscalls_probe ./test/cap_syscalls_probe32 ./test/scope_caps \
        ./test/dynamic/scope_caps ./test/scope_ids ./test/dynamic/scope_ids)
# The vendor bundle lives on read-only ISO media, outside the small archive.
if test ! -x /zones/lx-probe-zfs/root/test/codex-current/bin/codex; then
    mkdir -p /mnt/lx-media
    if test ! -x /mnt/lx-media/lab-codex/x86_64-unknown-linux-musl/bin/codex; then
        mount -F hsfs -o ro /dev/dsk/c2t0d0s2 /mnt/lx-media
    fi
    cp -Rp /mnt/lx-media/lab-codex/x86_64-unknown-linux-musl \
        /zones/lx-probe-zfs/root/test/codex-current
fi
codex_hash=$(digest -a sha256 \
    /zones/lx-probe-zfs/root/test/codex-current/bin/codex | awk '{print $NF}')
test "$codex_hash" = b8a11fedf24b2dfb59a70c9e52b030f9a508dcd49cd2dd0d41956f4c785156b8
mkdir -p /zones/lx-probe-zfs/root/test/scope-setid
cp /zones/lx-probe-zfs/root/test/scope_caps /zones/lx-probe-zfs/root/test/scope-setid/scope_caps
chmod 4755 /zones/lx-probe-zfs/root/test/scope-setid/scope_caps
if zonecfg -z lx-probe info attr name=max-user-namespaces | grep -q 'name: max-user-namespaces'; then
    zonecfg -z lx-probe 'select attr name=max-user-namespaces; set type=uint; set value=64; end;'
else
    zonecfg -z lx-probe 'add attr; set name=max-user-namespaces; set type=uint; set value=64; end;'
fi
zoneadm -z lx-probe ready
/lab/userns-budget-native --ready lx-probe
zoneadm -z lx-probe boot
/lab/userns-budget-native --running lx-probe
/lab/ktest run lx_namespace:user:access
/lab/ktest run lx_namespace:user:proc_maps
zlogin -S lx-probe /test/proc_maps_baseline
zlogin -S lx-probe /test/cap_syscalls_probe
zlogin -S lx-probe '/test/cap_syscalls_probe --filter'
zlogin -S lx-probe /test/cap_syscalls_probe32
echo LX_CAPABILITIES32_EXIT=0
zlogin -S lx-probe '/test/scope_ids --unscoped-trace'
zlogin -S lx-probe '/test/dynamic/scope_ids --unscoped-trace'
for probe in /test/scope_caps /test/dynamic/scope_caps \
    /test/scope_ids /test/dynamic/scope_ids; do
    scope_log=/lab/$(echo "$probe" | tr / _).log
    if [[ "$probe" = */scope_ids ]]; then
        expected='SCOPE_IDS cases=49 failures=0'
    else
        expected='SCOPE_CAPS cases=19 failures=0'
    fi
    zlogin -S lx-probe "$probe" > "$scope_log" 2>&1 &
    scope_job=$!
    scope_pid=
    for attempt in {1..10}; do
        scope_pid=$(sed -n 's/^SCOPE_READY_PID=//p' "$scope_log")
        test -z "$scope_pid" || break
        sleep 1
    done
    [[ "$scope_pid" =~ ^[0-9]+$ ]]
    printf '%s\0' "$scope_pid" > /lab/scope-target.bin
    /lab/ktest run -i /lab/scope-target.bin lx_namespace:user:scope_attach
    kill -CONT "$scope_pid"
    scope_done=0
    sid_checked=0
    for attempt in {1..30}; do
        sid_pid=$(sed -n 's/^SID_READY_PID=//p' "$scope_log")
        if test "$sid_checked" = 0 && test -n "$sid_pid"; then
            [[ "$sid_pid" = "$scope_pid" ]]
            printf '%s\0' "$sid_pid" > /lab/scope-target.bin
            /lab/ktest run -i /lab/scope-target.bin lx_namespace:user:scope_sids
            sid_checked=1
            kill -CONT "$sid_pid"
        fi
        if ! kill -0 "$scope_job" 2>/dev/null; then
            scope_done=1
            break
        fi
        sleep 1
    done
    if test "$scope_done" != 1; then
        cat "$scope_log"
        echo "LX_SCOPE_TIMEOUT=$probe"
        exit 1
    fi
    if wait "$scope_job"; then
        cat "$scope_log"
    else
        cat "$scope_log"
        exit 1
    fi
    grep -qx "$expected" "$scope_log"
    if [[ "$probe" = */scope_ids ]]; then
        test "$sid_checked" = 1
    fi
done
cleanup
trap - EXIT
/lab/ktest run lx_namespace:user:credentials
echo LX_USERNS_BACKEND_PASS
