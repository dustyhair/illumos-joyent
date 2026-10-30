#!/bin/bash
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
test "$(zonename)" = global
cleanup() {
    zoneadm -z lx-probe halt || true
    printf '%s\n' 'lx_brand`lx_userns_experimental/W 0' | mdb -kw
    zonecfg -z lx-probe 'select attr name=max-user-namespaces; set value=64; end;'
}
trap cleanup EXIT
printf '%s\n' 'lx_brand`lx_userns_experimental/D' | mdb -k > /lab/userns-entry-default.log
cat /lab/userns-entry-default.log
awk '$NF == "0" { found=1 } END { exit !found }' /lab/userns-entry-default.log
zoneadm -z lx-probe boot
for probe in /test/userns_entry /test/dynamic/userns_entry; do
    zlogin -S lx-probe "$probe --disabled"
done
printf '%s\n' 'lx_brand`lx_userns_experimental/W 1' | mdb -kw
for probe in /test/userns_entry /test/dynamic/userns_entry; do
    log=/lab/$(echo "$probe" | tr / _).log
    if zlogin -S lx-probe "$probe --enabled" > "$log" 2>&1; then
        cat "$log"
    else
        cat "$log"
        exit 1
    fi
    grep -qx 'USERNS_ENTRY cases=15 failures=0' "$log"
done
zoneadm -z lx-probe halt
zonecfg -z lx-probe 'select attr name=max-user-namespaces; set value=0; end;'
zoneadm -z lx-probe boot
for probe in /test/userns_entry /test/dynamic/userns_entry; do
    zlogin -S lx-probe "$probe --budget-zero"
done
cleanup
trap - EXIT
echo LX_USERNS_ENTRY_PASS
