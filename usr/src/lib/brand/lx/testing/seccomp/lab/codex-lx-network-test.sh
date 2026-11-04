#!/usr/bin/bash
set -o errexit
set -o nounset
set -o pipefail

here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'status=$?; rm -rf "$work"; exit $status' EXIT HUP INT TERM

if command -v svccfg >/dev/null 2>&1; then
	svccfg validate "$here/codex-lx-network-recovery.xml"
fi

cc=${CC:-gcc}
tar_command=${TAR:-$(command -v gtar || command -v tar)}
gzip_command=${GZIP:-$(command -v gzip)}
"$cc" -nostdlib -static -fno-pie -no-pie -Wall -Wextra -Werror \
    -o "$work/bwrap" "$here/codex-lx-bwrap.c"
"$cc" -static -O2 -Wall -Wextra -Werror \
    -o "$work/net" "$here/codex-lx-net.c"

private=$(env -i CODEX_LX_NETWORK_MODE=private \
    CODEX_LX_REAL_BWRAP=/bin/echo CODEX_LX_NET_HELPER=/opt/codex-lx-net \
    CODEX_LX_NET_LINKS='clx0=10.88.0.2/24,clx1=10.88.0.3/24' \
    CODEX_LX_NET_GATEWAY=10.88.0.1 \
	"$work/bwrap" --unshare-user \
	--argv0 codex-linux-sandbox -- /bin/true argument)
expected_private='--unshare-user --argv0 codex-linux-sandbox --unshare-net '
expected_private+='--cap-add CAP_NET_ADMIN '
expected_private+='--cap-add CAP_SETPCAP -- /opt/codex-lx-net '
expected_private+='--payload-argv0 codex-linux-sandbox '
expected_private+='--links clx0=10.88.0.2/24,clx1=10.88.0.3/24 '
expected_private+='--gateway 10.88.0.1 -- /bin/true argument'
[[ "$private" == "$expected_private" ]]

private_existing=$(env -i CODEX_LX_NETWORK_MODE=private \
    CODEX_LX_REAL_BWRAP=/bin/echo CODEX_LX_NET_HELPER=/opt/codex-lx-net \
    CODEX_LX_NET_LINKS='clx0=10.88.0.2/24,clx1=10.88.0.3/24' \
    CODEX_LX_NET_GATEWAY=10.88.0.1 \
    "$work/bwrap" --unshare-user --unshare-net \
    --argv0 codex-linux-sandbox -- /bin/true argument)
expected_existing='--unshare-user --unshare-net --argv0 codex-linux-sandbox '
expected_existing+='--cap-add CAP_NET_ADMIN '
expected_existing+='--cap-add CAP_SETPCAP -- /opt/codex-lx-net '
expected_existing+='--payload-argv0 codex-linux-sandbox '
expected_existing+='--links clx0=10.88.0.2/24,clx1=10.88.0.3/24 '
expected_existing+='--gateway 10.88.0.1 -- /bin/true argument'
[[ "$private_existing" == "$expected_existing" ]]

shared=$(env -i CODEX_LX_NETWORK_MODE=shared \
    CODEX_LX_REAL_BWRAP=/bin/echo \
    "$work/bwrap" --unshare-user --unshare-net -- /bin/true argument)
[[ "$shared" == '--unshare-user -- /bin/true argument' ]]

shared_existing=$(env -i CODEX_LX_NETWORK_MODE=shared \
    CODEX_LX_REAL_BWRAP=/bin/echo \
    "$work/bwrap" --unshare-user -- /bin/true argument)
[[ "$shared_existing" == '--unshare-user -- /bin/true argument' ]]

set +o errexit
env -i CODEX_LX_NETWORK_MODE=shared CODEX_LX_REAL_BWRAP=/bin/echo \
    "$work/bwrap" --unshare-net --unshare-net -- /bin/true \
    >"$work/duplicate.out" 2>&1
duplicate_status=$?
"$work/net" --payload-argv0 test --links bad --gateway 10.88.0.1 -- \
	/bin/true \
    >"$work/net.out" 2>&1
net_status=$?
set -o errexit
[[ $duplicate_status == 125 ]]
grep -q 'expected at most one --unshare-net' "$work/duplicate.out"
[[ $net_status == 125 ]]
grep -q 'name=address/prefix' "$work/net.out"

mkdir "$work/bin" "$work/zone"
for command in dladm ipadm ipf ipnat zoneadm zonecfg mdb; do
	cat > "$work/bin/$command" <<'EOF'
#!/usr/bin/bash
name=${0##*/}
printf '%s:%s\n' "$name" "$*" >> "$MOCK_LOG"
case "$name:$*" in
ipadm:show-prop*)
	printf '%s\n' off
	;;
ipf:-V*)
	printf '%s\n' 'Running: no'
	;;
zoneadm:*list\ -p*)
	status=$(cat "$MOCK_ZONE_STATUS")
	printf '%s\n' "-:lx-test:$status:$MOCK_ZONEPATH:uuid:lx:excl"
	;;
zoneadm:*boot*)
	printf '%s\n' running > "$MOCK_ZONE_STATUS"
	;;
zoneadm:*halt*)
	printf '%s\n' installed > "$MOCK_ZONE_STATUS"
	;;
zonecfg:*)
	printf 'zonepath: %s\n' "$MOCK_ZONEPATH"
	;;
mdb:*)
	request=$(cat)
	printf 'mdb-input:%s\n' "$request" >> "$MOCK_LOG"
	case "$request" in
	*/D)
		gate=${request##*lx_}
		gate=${gate%%_experimental*}
		printf 'lx_%s_experimental: %s\n' "$gate" \
		    "${MOCK_GATE_VALUE:-0}"
		;;
	esac
	;;
ipnat:-f*)
	[[ ${MOCK_FAIL_IPNAT:-0} == 0 ]] || exit 1
	;;
ipnat:-l*)
	printf '%s\n\n' 'List of active MAP/Redirect filters:'
	printf '%s\n' 'List of active sessions:'
	count=$(cat "$MOCK_NAT_SESSIONS")
	for ((index = 0; index < count; index++)); do
		printf '%s\n' 'MAP 10.88.0.2 1234 <- -> 192.0.2.1 5678 [8.8.8.8 53]'
	done
	;;
ipnat:-F*)
	printf '%s\n' 0 > "$MOCK_NAT_SESSIONS"
	;;
esac
exit 0
EOF
	chmod 755 "$work/bin/$command"
done

export MOCK_ZONEPATH=$work/zone
export MOCK_ZONE_STATUS=$work/zone.status
export MOCK_LOG=$work/mock.log
export MOCK_NAT_SESSIONS=$work/nat.sessions
export CODEX_LX_NETWORK_PATH=$work/bin:/usr/bin:/bin
export CODEX_LX_NETWORK_STATE_DIR=$work/state
export CODEX_LX_NETWORK_LOCK_DIR=$work/lock
export CODEX_LX_TAR=$tar_command
export CODEX_LX_GZIP=$gzip_command
mkdir -p "$work/zone/root/etc"
printf '%s\n' installed > "$MOCK_ZONE_STATUS"
printf '%s\n' 0 > "$MOCK_NAT_SESSIONS"
: > "$MOCK_LOG"

mkdir -p "$work/package/codex-lx-private"
printf '#!/usr/bin/bash\nexit 0\n' > \
    "$work/package/codex-lx-private/codex-lx"
chmod 755 "$work/package/codex-lx-private/codex-lx"
printf '%s\n' '{}' > \
    "$work/package/codex-lx-private/lx-package-manifest.json"
"$tar_command" -cf - -C "$work/package" codex-lx-private |
    "$gzip_command" -c > "$work/codex-lx-private.tar.gz"
"$here/codex-lx-network" install lx-test "$work/codex-lx-private.tar.gz"
[[ -x "$work/zone/root/opt/codex-lx/codex-lx" ]]
[[ -f "$work/zone/root/opt/codex-lx/lx-package-manifest.json" ]]

"$here/codex-lx-network" start lx-test clx external0 10.88.0 2
grep -q "CODEX_LX_NET_LINKS='clxnet0=10.88.0.2/24,clxnet1=10.88.0.3/24'" \
    "$work/zone/root/etc/codex-lx-network.conf"
[[ -f "$work/state/clx/state" ]]
[[ -f "$work/state/clx/ipf-in-host.rules" ]]
[[ -f "$work/state/clx/ipf-in-egress.rules" ]]
[[ -f "$work/state/clx/ipf-out-host.rules" ]]
[[ -f "$work/state/clx/ipf-out-egress.rules" ]]
for gate in seccomp userns mntns pidns ipcns netns; do
	grep -q "^${gate}_was=0$" "$work/state/base.state"
	grep -q "mdb-input:lx_brand.*lx_${gate}_experimental/W 1" \
	    "$MOCK_LOG"
done
"$here/codex-lx-network" status clx | grep -q 'health=ready'
"$here/codex-lx-network" reapply clx
[[ -f "$work/state/clx/state" ]]
printf '%s\n' 2 > "$MOCK_NAT_SESSIONS"
"$here/codex-lx-network" teardown clx
[[ ! -e "$work/state/clx" ]]
[[ ! -e "$work/zone/root/etc/codex-lx-network.conf" ]]
[[ ! -e "$work/state/base.state" ]]
[[ $(cat "$MOCK_NAT_SESSIONS") == 0 ]]
flush_line=$(grep -n '^ipnat:-F' "$MOCK_LOG" | tail -1 | cut -d: -f1)
filter_line=$(grep -n 'ipf:-r -f .*ipf-out-egress.rules' "$MOCK_LOG" |
    tail -1 | cut -d: -f1)
(( flush_line < filter_line ))
for gate in seccomp userns mntns pidns ipcns netns; do
	grep -q "mdb-input:lx_brand.*lx_${gate}_experimental/W 0" \
	    "$MOCK_LOG"
done

"$here/codex-lx-network" setup lx-test stale external0 10.90.0 1
printf '%s\n' installed > "$MOCK_ZONE_STATUS"
"$here/codex-lx-network" status stale | grep -q 'health=stale'
"$here/codex-lx-network" recover | grep -q 'deployments=1'
[[ ! -e "$work/state/stale" ]]
[[ ! -e "$work/state/base.state" ]]
"$here/codex-lx-network" recover | grep -q 'deployments=0'

: > "$MOCK_LOG"
MOCK_GATE_VALUE=1 "$here/codex-lx-network" setup lx-test keep \
    external0 10.91.0 1
for gate in seccomp userns mntns pidns ipcns netns; do
	grep -q "^${gate}_was=1$" "$work/state/base.state"
done
"$here/codex-lx-network" teardown keep
for gate in seccomp userns mntns pidns ipcns netns; do
	count=$(grep -c "mdb-input:lx_brand.*lx_${gate}_experimental/W 1" \
	    "$MOCK_LOG")
	(( count >= 2 ))
done

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
