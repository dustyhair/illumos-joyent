#!/bin/bash
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
test "$(zonename)" = global
zpool list zones >/dev/null
src=/zones/lx-ns-kernel/src
mkdir -p "$src"/{sub,existing,ro-source,readonly,later,pin,stacked}
printf '%s\n' original > "$src/sub/original"
printf '%s\n' underlying > "$src/existing/underlying"
printf '%s\n' original > "$src/later/original"
printf '%s\n' readonly > "$src/ro-source/file"
printf '%s\n' target > "$src/bind-target"
if ! test -L "$src/escape"; then
    ln -s /etc/passwd "$src/escape"
fi
printf '%s\0' "$src" > /lab/namespace-source.bin
mounted=()
cleanup() {
    local failed=0 i
    for ((i=${#mounted[@]}-1; i>=0; i--)); do
        umount "${mounted[i]}" || failed=1
    done
    return "$failed"
}
trap cleanup EXIT
mount -F tmpfs swap "$src/existing"
mounted+=("$src/existing")
printf '%s\n' mounted > "$src/existing/mounted"
mkdir -p "$src/existing/deep"
mount -F tmpfs swap "$src/existing/deep"
mounted+=("$src/existing/deep")
printf '%s\n' nested > "$src/existing/deep/nested"
mount -F lofs -o ro,nosuid,noexec,nosub "$src/ro-source" "$src/readonly"
mounted+=("$src/readonly")
mount -F tmpfs swap "$src/stacked"
mounted+=("$src/stacked")
printf '%s\n' hidden > "$src/stacked/hidden"
mount -O -F tmpfs swap "$src/stacked"
mounted+=("$src/stacked")
printf '%s\n' visible > "$src/stacked/visible"
modload /lab/lx_namespace_ktest
/lab/ktest run -i /lab/namespace-source.bin lx_namespace:mount:views
/lab/ktest run -i /lab/namespace-source.bin lx_namespace:mount:snapshot
/lab/ktest run -i /lab/namespace-source.bin lx_namespace:mount:snapshot
/lab/ktest run -i /lab/namespace-source.bin lx_namespace:mount:clone
/lab/ktest run -i /lab/namespace-source.bin lx_namespace:mount:clone
/lab/ktest run -i /lab/namespace-source.bin lx_namespace:mount:lifetime
/lab/ktest run -i /lab/namespace-source.bin lx_namespace:mount:lifetime
/lab/ktest run -i /lab/namespace-source.bin lx_namespace:mount:quota
/lab/ktest run -i /lab/namespace-source.bin lx_namespace:mount:quota
/lab/ktest run -i /lab/namespace-source.bin lx_namespace:mount:owner
/lab/ktest run -i /lab/namespace-source.bin lx_namespace:mount:owner
test -f "$src/sub/original"
test ! -e "$src/sub/private"
cleanup
mounted=()
trap - EXIT
test -f "$src/existing/underlying"
test ! -e "$src/existing/mounted"
echo LX_NAMESPACE_SNAPSHOT_PASS
