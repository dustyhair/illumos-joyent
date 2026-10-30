#!/bin/bash
# Diagnostic only: a zero DTrace status is not sandbox acceptance.
set -euo pipefail
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
test "$(zonename)" = global
zoneadm -z lx-probe list -p | grep -q ':running:'
dtrace -x nolibs -qn '
lx-syscall::clone:entry
/zonename == "lx-probe" && (arg0 & 0x7c020000) != 0/
{
    self->namespace_clone = 1;
    printf("BWRAP_CLONE pid=%d flags=0x%lx\n", pid, arg0);
}
lx-syscall::clone:return
/self->namespace_clone/
{
    printf("BWRAP_CLONE_RETURN pid=%d result=%d\n", pid, (int64_t)arg0);
    self->namespace_clone = 0;
}
lx-syscall::unshare:entry
/zonename == "lx-probe"/
{
    printf("BWRAP_UNSHARE pid=%d flags=0x%lx\n", pid, arg0);
}' -c 'zlogin -S lx-probe /test/codex-current/bin/codex sandbox -P :workspace -- /test/identity_probe'
