#!/usr/bin/bash
set -o errexit
set -o nounset
set -o pipefail

here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM

cc=${CC:-gcc}
"$cc" -nostdlib -static -fno-pie -no-pie -Wall -Wextra -Werror \
    -o "$work/bwrap" "$here/codex-lx-bwrap.c"
"$cc" -static -O2 -Wall -Wextra -Werror \
    -o "$work/net" "$here/codex-lx-net.c"

private=$(env -i CODEX_LX_NETWORK_MODE=private \
    CODEX_LX_REAL_BWRAP=/bin/echo CODEX_LX_NET_HELPER=/opt/codex-lx-net \
    CODEX_LX_NET_LINKS='clx0=10.88.0.2/24,clx1=10.88.0.3/24' \
    CODEX_LX_NET_GATEWAY=10.88.0.1 \
    "$work/bwrap" --unshare-user --unshare-net -- /bin/true argument)
expected_private='--unshare-user --unshare-net -- /opt/codex-lx-net '
expected_private+='--links clx0=10.88.0.2/24,clx1=10.88.0.3/24 '
expected_private+='--gateway 10.88.0.1 -- /bin/true argument'
[[ "$private" == "$expected_private" ]]

shared=$(env -i CODEX_LX_NETWORK_MODE=shared \
    CODEX_LX_REAL_BWRAP=/bin/echo \
    "$work/bwrap" --unshare-user --unshare-net -- /bin/true argument)
[[ "$shared" == '--unshare-user -- /bin/true argument' ]]

set +o errexit
env -i CODEX_LX_NETWORK_MODE=shared CODEX_LX_REAL_BWRAP=/bin/echo \
    "$work/bwrap" --unshare-net --unshare-net -- /bin/true \
    >"$work/duplicate.out" 2>&1
duplicate_status=$?
"$work/net" --links bad --gateway 10.88.0.1 -- /bin/true \
    >"$work/net.out" 2>&1
net_status=$?
set -o errexit
[[ $duplicate_status == 125 ]]
grep -q 'expected exactly one --unshare-net' "$work/duplicate.out"
[[ $net_status == 125 ]]
grep -q 'name=address/prefix' "$work/net.out"

mkdir "$work/bin" "$work/zone"
for command in dladm ipadm ipf ipnat zoneadm zonecfg mdb; do
	cat > "$work/bin/$command" <<'EOF'
#!/usr/bin/bash
name=${0##*/}
case "$name:$*" in
ipadm:show-prop*)
	printf '%s\n' off
	;;
ipf:-V*)
	printf '%s\n' 'Running: no'
	;;
zoneadm:*list\ -p*)
	printf '%s\n' "-:lx-test:installed:$MOCK_ZONEPATH:uuid:lx:excl"
	;;
zonecfg:*)
	printf 'zonepath: %s\n' "$MOCK_ZONEPATH"
	;;
mdb:*)
	cat >/dev/null
	printf '%s\n' 'lx_netns_experimental: 0'
	;;
ipnat:-f*)
	[[ ${MOCK_FAIL_IPNAT:-0} == 0 ]] || exit 1
	;;
esac
exit 0
EOF
	chmod 755 "$work/bin/$command"
done

export MOCK_ZONEPATH=$work/zone
export CODEX_LX_NETWORK_PATH=$work/bin:/usr/bin:/bin
export CODEX_LX_NETWORK_STATE_DIR=$work/state
export CODEX_LX_NETWORK_LOCK_DIR=$work/lock
mkdir -p "$work/zone/root/etc"
"$here/codex-lx-network" setup lx-test clx external0 10.88.0 2
grep -q "CODEX_LX_NET_LINKS='clxnet0=10.88.0.2/24,clxnet1=10.88.0.3/24'" \
    "$work/zone/root/etc/codex-lx-network.conf"
[[ -f "$work/state/clx/state" ]]
"$here/codex-lx-network" reapply clx
[[ -f "$work/state/clx/state" ]]
"$here/codex-lx-network" teardown clx
[[ ! -e "$work/state/clx" ]]
[[ ! -e "$work/zone/root/etc/codex-lx-network.conf" ]]

set +o errexit
MOCK_FAIL_IPNAT=1 "$here/codex-lx-network" setup lx-test fail \
    external0 10.89.0 1 >"$work/rollback.out" 2>&1
rollback_status=$?
set -o errexit
[[ $rollback_status != 0 ]]
if [[ -e "$work/state/fail" ]]; then
	cat "$work/rollback.out" >&2
	find "$work/state/fail" -maxdepth 1 -print >&2
	exit 1
fi
[[ ! -e "$work/zone/root/etc/codex-lx-network.conf" ]]

printf '%s\n' CODEX_LX_NETWORK_UNIT_PASS
