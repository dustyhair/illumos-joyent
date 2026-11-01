#!/usr/bin/bash
set -o errexit
set -o nounset
set -o pipefail

zone=${CODEX_LX_TEST_ZONE:-lx-probe}
zone_root=${CODEX_LX_ZONE_ROOT:-/zones/lx-probe-zfs/root}
codex=${CODEX_LX_TEST_PACKAGE:-/test/codex-lx/codex-lx}
workdir=${CODEX_LX_TEST_WORKDIR:-/test}

reset_switches()
{
	printf '%s\n' \
	    'lx_brand`lx_seccomp_experimental/W 0' \
	    'lx_brand`lx_userns_experimental/W 0' \
	    'lx_brand`lx_mntns_experimental/W 0' \
	    'lx_brand`lx_pidns_experimental/W 0' \
	    'lx_brand`lx_ipcns_experimental/W 0' | mdb -kw
}
trap reset_switches EXIT

printf '%s\n' \
    'lx_brand`lx_seccomp_experimental/W 1' \
    'lx_brand`lx_userns_experimental/W 1' \
    'lx_brand`lx_mntns_experimental/W 1' \
    'lx_brand`lx_pidns_experimental/W 1' \
    'lx_brand`lx_ipcns_experimental/W 1' | mdb -kw

identity_command="zlogin -S $zone /native/usr/bin/sh -c 'cd $workdir && "
identity_command+="exec $codex sandbox -P :workspace -- "
identity_command+="/test/identity_probe 0 0'"
dtrace -x nolibs -qn '
lx-syscall::clone:entry
/(arg0 & 0x7c020000) != 0/
{
	self->namespace_clone = 1;
	printf("BWRAP_CLONE pid=%d flags=0x%lx\n", pid, arg0);
}
lx-syscall::clone:return
/self->namespace_clone/
{
	printf("BWRAP_CLONE_RETURN pid=%d result=%d\n", pid,
	    (int64_t)arg0);
	self->namespace_clone = 0;
}' -c "$identity_command"

run_sandbox()
{
	zlogin -S "$zone" /native/usr/bin/sh -c \
	    "cd $workdir && exec $codex sandbox -P :workspace -- $*"
}

run_sandbox /test/identity_probe 0 0
run_sandbox /test/sandbox-write-probe
grep -qx sandbox-write-pass "$zone_root/test/codex-sandbox-write"
run_sandbox /test/pty-probe
run_sandbox /test/sandbox-readonly-probe
test ! -e "$zone_root/usr/codex-readonly-probe"
zlogin -S "$zone" /native/usr/bin/test ! -e /dev/pts/ptmx
printf '%s\n' CODEX_LX_SHARED_NETWORK_ACCEPTANCE_PASS
