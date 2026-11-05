#!/usr/bin/bash
#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or http://www.opensolaris.org/os/licensing.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#
# Copyright 2026 Jonathan Wagner.
#
set -o errexit
set -o nounset
set -o pipefail

controller=${1:-$(cd "$(dirname "$0")/../../../zone" && pwd)/lx_private_network.ksh}
work=$(mktemp -d)
lock_owner=

cleanup()
{
	status=$?
	trap - EXIT HUP INT TERM
	if [[ -n "$lock_owner" ]]; then
		kill -9 "$lock_owner" 2>/dev/null || true
		wait "$lock_owner" 2>/dev/null || true
	fi
	rm -rf "$work"
	exit "$status"
}

trap cleanup EXIT HUP INT TERM

lock_helper=$(cd "$(dirname "$controller")" && pwd)/lx_private_network_lock
if [[ -x "$lock_helper" ]]; then
	"$lock_helper" -t 1 "$work/host.lock" /usr/bin/sleep 30 &
	lock_owner=$!
	/usr/bin/sleep 1
	set +o errexit
	"$lock_helper" -t 0 "$work/host.lock" /usr/bin/true \
	    > "$work/lock.out" 2>&1
	lock_status=$?
	set -o errexit
	[[ $lock_status != 0 ]]
	kill -9 "$lock_owner"
	wait "$lock_owner" 2>/dev/null || true
	lock_owner=
	"$lock_helper" -t 1 "$work/host.lock" /usr/bin/true
fi

mkdir "$work/bin" "$work/zone"
for command in dladm ipadm ipf ipnat logger zoneadm zonecfg mdb; do
	cat > "$work/bin/$command" <<'EOF'
#!/usr/bin/bash
name=${0##*/}
printf '%s:%s\n' "$name" "$*" >> "$MOCK_LOG"
case "$name:$*" in
ipadm:show-prop*)
	printf '%s\n' on
	;;
ipf:-V*)
	printf '%s\n' 'Running: yes'
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
		    "${MOCK_GATE_VALUE:-1}"
		;;
	esac
	;;
ipnat:-f*)
	[[ ${MOCK_FAIL_IPNAT:-0} == 0 ]] || exit 1
	;;
dladm:delete-vnic*)
	[[ ${MOCK_FAIL_DELETE_VNIC:-0} == 0 ]] || exit 1
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
export LX_PRIVATE_NETWORK_PATH=$work/bin:/usr/bin:/bin
export LX_PRIVATE_NETWORK_STATE_DIR=$work/state
export LX_PRIVATE_NETWORK_CONTROL_DIR=$work/control
export LX_PRIVATE_NETWORK_LOCKED=1
mkdir -p "$work/control/$MOCK_ZONE"
: > "$MOCK_LOG"

"$controller" prepare "$MOCK_ZONE" "$MOCK_ZONEPATH"
prefix=lx85b8918f
config=$work/control/$MOCK_ZONE/lx-private-network.conf
state=$work/state/85b8918fb66f47409f8ca1a75aeaa3e6
[[ -f "$state/state" ]]
[[ -f "$state/ipf-in-host.rules" ]]
[[ -f "$state/ipf-out-egress.rules" ]]
grep -q "LX_PRIVATE_NETWORK_LINKS='${prefix}n0=10.88.0.2/24,${prefix}n1=10.88.0.3/24'" \
    "$config"
grep -q "LX_PRIVATE_NETWORK_GATEWAY='10.88.0.1'" "$config"
grep -q "^phase='ready'$" "$state/state"
[[ ! -e "$work/state/base.state" ]]
if grep -q 'mdb-input:.*/W' "$MOCK_LOG"; then
	printf '%s\n' 'controller wrote an experimental kernel gate' >&2
	exit 1
fi
if grep -q '^ipnat:-F' "$MOCK_LOG"; then
	printf '%s\n' 'controller flushed the global NAT session table' >&2
	exit 1
fi

# A repeated ready transition must not duplicate links or rules.
before=$(wc -l < "$MOCK_LOG")
"$controller" prepare "$MOCK_ZONE" "$MOCK_ZONEPATH"
after=$(wc -l < "$MOCK_LOG")
(( after > before ))
[[ $(grep -c '^dladm:create-etherstub' "$MOCK_LOG") == 1 ]]

# Runtime ownership uses the full UUID. Datalink names grow past a colliding
# eight-digit prefix instead of allowing one zone to clean up another.
first_zone=$MOCK_ZONE
second_zone=85b8918f-aaaa-4740-9f8c-a1a75aeaa3e6
export MOCK_ZONE=$second_zone
export MOCK_SUBNET=10.89.0.0/24
mkdir -p "$work/control/$second_zone"
"$controller" prepare "$MOCK_ZONE" "$MOCK_ZONEPATH"
second_state=$work/state/85b8918faaaa47409f8ca1a75aeaa3e6
grep -q "^prefix='lx85b8918faaaa'$" "$second_state/state"
"$controller" cleanup "$MOCK_ZONE" "$MOCK_ZONEPATH"
[[ -f "$state/state" ]]
export MOCK_ZONE=$first_zone
export MOCK_SUBNET=10.88.0.0/24

MOCK_ZONE_STATUS=running "$controller" status "$MOCK_ZONE" |
    grep -q 'health=ready'
export MOCK_ZONE_STATUS=installed
"$controller" cleanup "$MOCK_ZONE" "$MOCK_ZONEPATH"
[[ ! -e "$state" ]]
[[ ! -e "$config" ]]

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

# A failed NAT load rolls back links, rules, and the zone-control file.
export MOCK_SUBNET=10.89.0.0/24
set +o errexit
MOCK_FAIL_IPNAT=1 "$controller" prepare "$MOCK_ZONE" "$MOCK_ZONEPATH" \
    > "$work/rollback.out" 2>&1
rollback_status=$?
set -o errexit
[[ $rollback_status != 0 ]]
[[ ! -e "$state" ]]
[[ ! -e "$config" ]]

# Cleanup failures retain ownership state so a later transition can retry.
export MOCK_SUBNET=10.90.0.0/24
"$controller" prepare "$MOCK_ZONE" "$MOCK_ZONEPATH"
set +o errexit
MOCK_FAIL_DELETE_VNIC=1 "$controller" cleanup "$MOCK_ZONE" "$MOCK_ZONEPATH" \
    > "$work/cleanup-failure.out" 2>&1
cleanup_status=$?
set -o errexit
[[ $cleanup_status != 0 ]]
[[ -f "$state/state" ]]
grep -q "phase='cleanup_pending'" "$state/state"
"$controller" cleanup "$MOCK_ZONE" "$MOCK_ZONEPATH"
[[ ! -e "$state" ]]

printf '%s\n' LX_PRIVATE_NETWORK_UNIT_PASS
