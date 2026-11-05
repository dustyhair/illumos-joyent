#!/usr/bin/bash
#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the Common Development
# and Distribution License (the "License").  You may not use this file except
# in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE or
# http://www.opensolaris.org/os/licensing.
#
# CDDL HEADER END
#
set -o errexit
set -o nounset
set -o pipefail

controller=${1:-$(cd "$(dirname "$0")" && pwd)/lx_private_network.ksh}
work=$(mktemp -d)
trap 'status=$?; rm -rf "$work"; exit $status' EXIT HUP INT TERM

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
	printf '%s\n' "-:$MOCK_ZONE:$MOCK_ZONE_STATUS:$MOCK_ZONEPATH:uuid:lx:excl"
	;;
zonecfg:*name=lx-private-network-egress*)
	printf '%s\n' 'value: e1000g0'
	;;
zonecfg:*name=lx-private-network-subnet*)
	printf 'value: %s\n' "$MOCK_SUBNET"
	;;
zonecfg:*name=lx-private-network-links*)
	printf 'value: %s\n' "$MOCK_LINKS"
	;;
zonecfg:*name=lx-private-network*)
	printf '%s\n' 'value: true'
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
		printf '%s\n' \
		    'MAP 10.88.0.2 1234 <- -> 192.0.2.1 5678 [8.8.8.8 53]'
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

export MOCK_ZONE=85b8918f-b66f-4740-9f8c-a1a75aeaa3e6
export MOCK_ZONEPATH=$work/zone
export MOCK_ZONE_STATUS=ready
export MOCK_SUBNET=10.88.0.0/24
export MOCK_LINKS=2
export MOCK_LOG=$work/mock.log
export MOCK_NAT_SESSIONS=$work/nat.sessions
export LX_PRIVATE_NETWORK_PATH=$work/bin:/usr/bin:/bin
export LX_PRIVATE_NETWORK_STATE_DIR=$work/state
export LX_PRIVATE_NETWORK_LOCK_DIR=$work/lock
mkdir -p "$work/zone/root/etc"
printf '%s\n' 0 > "$MOCK_NAT_SESSIONS"
: > "$MOCK_LOG"

"$controller" prepare "$MOCK_ZONE" "$MOCK_ZONEPATH"
prefix=lx85b8918f
config=$work/zone/root/etc/lx-private-network.conf
state=$work/state/$prefix
[[ -f "$state/state" ]]
[[ -f "$state/ipf-in-host.rules" ]]
[[ -f "$state/ipf-in-egress.rules" ]]
[[ -f "$state/ipf-out-host.rules" ]]
[[ -f "$state/ipf-out-egress.rules" ]]
grep -q "LX_PRIVATE_NETWORK_LINKS='${prefix}n0=10.88.0.2/24,${prefix}n1=10.88.0.3/24'" \
    "$config"
grep -q "LX_PRIVATE_NETWORK_GATEWAY='10.88.0.1'" "$config"
for gate in seccomp userns mntns pidns ipcns netns; do
	grep -q "^${gate}_was=0$" "$work/state/base.state"
	grep -q "mdb-input:lx_brand.*lx_${gate}_experimental/W 1" \
	    "$MOCK_LOG"
done

# A repeated ready transition must not duplicate links or rules.
before=$(wc -l < "$MOCK_LOG")
"$controller" prepare "$MOCK_ZONE" "$MOCK_ZONEPATH"
after=$(wc -l < "$MOCK_LOG")
(( after > before ))
[[ $(grep -c '^dladm:create-etherstub' "$MOCK_LOG") == 1 ]]

MOCK_ZONE_STATUS=running "$controller" status "$MOCK_ZONE" |
    grep -q 'health=ready'
export MOCK_ZONE_STATUS=installed
printf '%s\n' 2 > "$MOCK_NAT_SESSIONS"
"$controller" cleanup "$MOCK_ZONE" "$MOCK_ZONEPATH"
[[ ! -e "$state" ]]
[[ ! -e "$config" ]]
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

# Invalid and non-private networks fail before touching runtime state.
export MOCK_ZONE_STATUS=ready
export MOCK_SUBNET=8.8.8.0/24
set +o errexit
"$controller" prepare "$MOCK_ZONE" "$MOCK_ZONEPATH" \
    > "$work/public.out" 2>&1
public_status=$?
set -o errexit
[[ $public_status != 0 ]]
grep -q 'RFC 1918' "$work/public.out"
[[ ! -e "$state" ]]

# A failed NAT load rolls back links, rules, the zone file and saved gates.
export MOCK_SUBNET=10.89.0.0/24
set +o errexit
MOCK_FAIL_IPNAT=1 "$controller" prepare "$MOCK_ZONE" "$MOCK_ZONEPATH" \
    > "$work/rollback.out" 2>&1
rollback_status=$?
set -o errexit
[[ $rollback_status != 0 ]]
[[ ! -e "$state" ]]
[[ ! -e "$config" ]]
[[ ! -e "$work/state/base.state" ]]

printf '%s\n' LX_PRIVATE_NETWORK_UNIT_PASS
