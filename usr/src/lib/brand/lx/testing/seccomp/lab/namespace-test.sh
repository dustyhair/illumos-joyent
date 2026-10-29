#!/bin/bash
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
test "$(zonename)" = global
zpool list zones >/dev/null
mkdir -p /zones/lx-ns-kernel/src/sub
printf '%s\n' original > /zones/lx-ns-kernel/src/sub/original
if ! test -L /zones/lx-ns-kernel/src/escape; then
    ln -s /etc/passwd /zones/lx-ns-kernel/src/escape
fi
modload /lab/lx_namespace_ktest
/lab/ktest run -i /lab/namespace-source.bin lx_namespace:mount:views
test -f /zones/lx-ns-kernel/src/sub/original
test ! -e /zones/lx-ns-kernel/src/sub/private
echo LX_NAMESPACE_VIEW_PASS
