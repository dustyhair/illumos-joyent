#!/bin/ksh -p
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
# Prepare private networking for an lx zone.  Persistent intent is stored in
# zone attributes.  This program owns temporary host resources only.
#

set -o errexit
set -o nounset
set -o pipefail

unset LD_AUDIT LD_LIBRARY_PATH LD_LIBRARY_PATH_32 LD_LIBRARY_PATH_64
unset LD_PRELOAD LD_PRELOAD_32 LD_PRELOAD_64

PATH=${LX_PRIVATE_NETWORK_PATH:-/usr/sbin:/usr/bin:/sbin}
export PATH

state_root=${LX_PRIVATE_NETWORK_STATE_DIR:-/var/run/lx-private-network}
control_root=${LX_PRIVATE_NETWORK_CONTROL_DIR:-/var/zonecontrol}
lock_path=${LX_PRIVATE_NETWORK_LOCK_PATH:-$state_root/lock}
lock_helper=${LX_PRIVATE_NETWORK_LOCK_HELPER:-/usr/lib/brand/lx/lx_private_network_lock}
rollback_directory=
gate_names=(seccomp userns mntns pidns ipcns netns)

usage()
{
	cat >&2 <<EOF
usage:
  lx_private_network prepare ZONE ZONEPATH
  lx_private_network quiesce ZONE ZONEPATH
  lx_private_network cleanup ZONE ZONEPATH
  lx_private_network status [ZONE]

These commands are lx brand lifecycle hooks.  Configure the experimental
feature through vmadm instead of invoking this file directly.
EOF
	exit 2
}

log_error()
{
	logger -p daemon.err "[zone ${zone_for_log:-unknown}] lx private network: $*" \
	    2>/dev/null || true
	printf 'lx private network: %s\n' "$*" >&2
}

die()
{
	log_error "$*"
	exit 1
}

lock_and_restart()
{
	mkdir -p "$state_root"
	chmod 700 "$state_root"
	[[ ${LX_PRIVATE_NETWORK_LOCKED:-0} == 1 ]] && return
	[[ -x "$lock_helper" ]] || die "lock helper is not executable: $lock_helper"
	exec "$lock_helper" -t 10 "$lock_path" /usr/bin/env \
	    LX_PRIVATE_NETWORK_LOCKED=1 "$0" "$@"
}

zone_attr()
{
	typeset zone=$1 name=$2 variable
	typeset -n value

	variable=_ZONECFG_attr_${name//-/_}
	value=$variable
	if [[ ${value+x} == x ]]; then
		printf '%s\n' "$value"
		return
	fi
	zonecfg -z "$zone" info attr "name=$name" 2>/dev/null |
	    awk '$1 == "value:" { print $2; exit }'
}

zone_id()
{
	typeset compact=${1//-/}

	[[ ${#compact} == 32 && "$compact" != *[!0-9A-Fa-f]* ]] ||
	    die "zone name is not a UUID: $1"
	printf '%s\n' "$compact" | tr '[:upper:]' '[:lower:]'
}

runtime_directory()
{
	printf '%s/%s\n' "$state_root" "$(zone_id "$1")"
}

read_gate()
{
	typeset gate=$1 value

	value=$(printf 'lx_brand`lx_%s_experimental/D\n' "$gate" | mdb -k |
	    awk 'NF != 0 { value = $NF } END { print value }')
	case "$value" in
	0|0x0)
		printf '%s\n' 0
		;;
	1|0x1)
		printf '%s\n' 1
		;;
	*)
		die "cannot read lx_${gate}_experimental"
		;;
	esac
}

check_host_prerequisites()
{
	typeset forwarding ipf_running gate

	forwarding=$(ipadm show-prop -c -o current -p forwarding ipv4)
	ipf_running=$(ipf -V | awk '/^Running:/ { print $2 }')
	[[ "$forwarding" == on ]] ||
	    die "IPv4 forwarding must be enabled by the host administrator"
	[[ "$ipf_running" == yes ]] ||
	    die "IP Filter must be enabled by the host administrator"
	for gate in "${gate_names[@]}"; do
		[[ $(read_gate "$gate") == 1 ]] ||
		    die "lx_${gate}_experimental must be enabled in /etc/system"
	done
}

prefix_in_use()
{
	typeset candidate=$1 owner=$2 path saved_zone prefix link

	for path in "$state_root"/*/state; do
		[[ -f "$path" ]] || continue
		unset saved_zone prefix
		# Runtime files are root-owned and mode 0600.
		# shellcheck disable=SC1090
		. "$path"
		if [[ ${prefix:-} == "$candidate" && ${saved_zone:-} != "$owner" ]]; then
			return 0
		fi
	done
	while IFS= read -r link; do
		case "$link" in
		"$candidate"s0|"$candidate"h0|"$candidate"n[0-9]*)
			return 0
			;;
		esac
	done < <(dladm show-link -p -o link 2>/dev/null || true)
	return 1
}

allocate_prefix()
{
	typeset zone=$1 compact length candidate

	compact=$(zone_id "$zone")
	for length in 8 12 16 20 24 26; do
		candidate=lx${compact:0:length}
		if ! prefix_in_use "$candidate" "$zone"; then
			printf '%s\n' "$candidate"
			return
		fi
	done
	die "cannot allocate a unique datalink prefix for $zone"
}

validate_subnet()
{
	typeset cidr=$1 first second third value

	[[ "$cidr" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)\.0/24$ ]] ||
	    die "lx_private_network_subnet must be an IPv4 /24 network"
	# shellcheck disable=SC2296
	first=${.sh.match[1]}
	# shellcheck disable=SC2296
	second=${.sh.match[2]}
	# shellcheck disable=SC2296
	third=${.sh.match[3]}
	for value in "$first" "$second" "$third"; do
		[[ "$value" == 0 || "$value" != 0* ]] ||
		    die "private network octets must not contain leading zeroes"
		(( value <= 255 )) || die "invalid private network octet"
	done
	if ! (( first == 10 || (first == 172 && second >= 16 && second <= 31) ||
	    (first == 192 && second == 168) )); then
		die "lx_private_network_subnet must use an RFC 1918 network"
	fi
	printf '%s.%s.%s\n' "$first" "$second" "$third"
}

write_state()
{
	typeset directory=$1 phase=$2 temporary

	temporary=$directory/state.$$

	{
		printf "phase='%s'\n" "$phase"
		printf "saved_zone='%s'\n" "$zone"
		printf "prefix='%s'\n" "$prefix"
		printf "egress='%s'\n" "$egress"
		printf "network='%s'\n" "$network"
		printf "count='%s'\n" "$count"
		printf "stub='%s'\n" "$stub"
		printf "host='%s'\n" "$host"
		printf "gateway='%s'\n" "$gateway"
		printf "subnet='%s'\n" "$subnet"
		printf "config='%s'\n" "$config"
		printf "links='%s'\n" "$links"
	} > "$temporary"
	chmod 600 "$temporary"
	mv "$temporary" "$directory/state"
}

deployment_healthy()
{
	typeset link

	[[ ${phase:-} == ready && -f "$config" ]] || return 1
	dladm show-link -p -o link "$stub" >/dev/null 2>&1 || return 1
	dladm show-link -p -o link "$host" >/dev/null 2>&1 || return 1
	for link in ${links//,/ }; do
		link=${link%%=*}
		dladm show-link -p -o link "$link" >/dev/null 2>&1 || return 1
	done
	return 0
}

cleanup_failed()
{
	log_error "$*"
	cleanup_errors=1
}

nat_rules_present()
{
	ipnat -l 2>/dev/null | awk '
	    /^List of active MAP\/Redirect filters:/ { active = 1; next }
	    /^List of active sessions:/ { active = 0 }
	    active { print }
	' | grep -F " $subnet " >/dev/null 2>&1
}

filter_rules_present()
{
	typeset rules=$1

	ipfstat -io 2>/dev/null | grep -F -x -f "$rules" >/dev/null 2>&1
}

remove_deployment()
{
	typeset directory=$1 expected_zone=$2 link rules
	typeset -i cleanup_errors=0

	[[ -f "$directory/state" ]] || return
	# shellcheck disable=SC1090,SC1091
	. "$directory/state"
	[[ "$saved_zone" == "$expected_zone" ]] ||
	    die "runtime state belongs to $saved_zone, not $expected_zone"

	rm -f "$directory"/*.pending
	if nat_rules_present; then
		for rules in "$directory"/ipnat-*.rules; do
			[[ -f "$rules" ]] || continue
			ipnat -r -f "$rules" ||
			    cleanup_failed "cannot remove NAT rule in $rules"
		done
	fi
	if nat_rules_present; then
		cleanup_failed "private-network NAT rules remain active"
	else
		rm -f "$directory"/ipnat-*.rules
	fi
	for rules in "$directory"/ipf-*.rules; do
		[[ -f "$rules" ]] || continue
		if filter_rules_present "$rules"; then
			ipf -r -f "$rules" || cleanup_failed \
			    "cannot remove IP Filter rules in $rules"
		fi
		if filter_rules_present "$rules"; then
			cleanup_failed "IP Filter rule remains active: $rules"
		else
			rm -f "$rules"
		fi
	done
	if [[ -e "$config" ]] && ! rm -f "$config"; then
		cleanup_failed "cannot remove zone-control configuration $config"
	fi
	if ! ipadm delete-addr "$host/v4" 2>/dev/null; then
		if ipadm show-addr "$host/v4" >/dev/null 2>&1; then
			cleanup_failed "cannot remove address $host/v4"
		fi
	fi
	if ! ipadm delete-if "$host" 2>/dev/null; then
		if ipadm show-if "$host" >/dev/null 2>&1; then
			cleanup_failed "cannot remove interface $host"
		fi
	fi
	for link in ${links//,/ }; do
		link=${link%%=*}
		dladm reset-linkprop -t -p zone "$link" 2>/dev/null || true
		if ! dladm delete-vnic "$link" 2>/dev/null; then
			if dladm show-link "$link" >/dev/null 2>&1; then
				cleanup_failed "cannot remove VNIC $link"
			fi
		fi
	done
	if ! dladm delete-vnic "$host" 2>/dev/null; then
		if dladm show-link "$host" >/dev/null 2>&1; then
			cleanup_failed "cannot remove VNIC $host"
		fi
	fi
	if ! dladm delete-etherstub "$stub" 2>/dev/null; then
		if dladm show-link "$stub" >/dev/null 2>&1; then
			cleanup_failed "cannot remove etherstub $stub"
		fi
	fi

	if (( cleanup_errors != 0 )); then
		phase=cleanup_pending
		# Keep the ownership record and any rule files for a retry.
		write_state "$directory" "$phase"
		return 1
	fi
	rm -f "$directory/state"
	rmdir "$directory"
}

quiesce()
{
	(( $# == 2 )) || usage
	typeset zone=$1 zonepath=$2 directory rules

	zone_for_log=$zone
	[[ "$zonepath" =~ ^/[A-Za-z0-9_./-]+$ ]] ||
	    die "cannot determine a safe zone path"
	directory=$(runtime_directory "$zone")
	[[ -f "$directory/state" ]] || return
	# shellcheck disable=SC1090,SC1091
	. "$directory/state"
	[[ "$saved_zone" == "$zone" ]] ||
	    die "runtime state belongs to $saved_zone, not $zone"

	if nat_rules_present; then
		for rules in "$directory"/ipnat-*.rules; do
			[[ -f "$rules" ]] || continue
			ipnat -r -f "$rules" ||
			    die "cannot quiesce NAT rule in $rules"
		done
	fi
	nat_rules_present && die "private-network NAT rules remain active"
	for rules in "$directory"/ipf-*.rules; do
		[[ -f "$rules" ]] || continue
		if filter_rules_present "$rules"; then
			ipf -r -f "$rules" ||
			    die "cannot quiesce IP Filter rule in $rules"
		fi
		filter_rules_present "$rules" &&
		    die "private-network IP Filter rule remains active: $rules"
	done
	write_state "$directory" quiesced
	printf 'LX_PRIVATE_NETWORK_QUIESCED zone=%s\n' "$zone"
}

rollback_prepare()
{
	typeset result=$?

	trap - EXIT HUP INT TERM
	set +o errexit
	remove_deployment "$rollback_directory" "$zone_for_log"
	exit "$result"
}

write_guest_config()
{
	typeset temporary

	[[ -d "$control_root/$zone" ]] ||
	    die "zone-control directory does not exist: $control_root/$zone"
	temporary=$(mktemp "$control_root/$zone/.lx-private-network.XXXXXX")
	{
		printf "LX_PRIVATE_NETWORK_LINKS='%s'\n" "$links"
		printf "LX_PRIVATE_NETWORK_GATEWAY='%s'\n" "$gateway"
	} > "$temporary"
	chmod 644 "$temporary"
	mv "$temporary" "$config"
}

prepare()
{
	(( $# == 2 )) || usage
	typeset zone=$1 zonepath=$2 enabled egress cidr network count prefix
	typeset directory stub host gateway subnet config
	typeset links='' link rules address status state phase saved_zone
	typeset -i index

	zone_for_log=$zone
	[[ "$zonepath" =~ ^/[A-Za-z0-9_./-]+$ ]] ||
	    die "cannot determine a safe zone path"
	enabled=$(zone_attr "$zone" lx-private-network)
	[[ "$enabled" == true ]] || return 0
	egress=$(zone_attr "$zone" lx-private-network-egress)
	cidr=$(zone_attr "$zone" lx-private-network-subnet)
	count=$(zone_attr "$zone" lx-private-network-links)
	[[ "$egress" =~ ^[A-Za-z0-9_.-]+$ ]] ||
	    die "invalid lx_private_network_egress"
	network=$(validate_subnet "$cidr")
	[[ "$count" =~ ^[0-9]+$ ]] || count=1
	(( count >= 1 && count <= 32 )) ||
	    die "lx_private_network_links must be from 1 to 32"
	dladm show-link -p -o link "$egress" >/dev/null 2>&1 ||
	    die "egress link does not exist: $egress"
	status=$(zoneadm -z "$zone" list -p 2>/dev/null |
	    awk -F: 'NR == 1 { print $3 }')
	[[ "$status" == ready || "$status" == running ]] ||
	    die "zone must be ready or running, not ${status:-missing}"
	check_host_prerequisites

	directory=$(runtime_directory "$zone")
	if [[ -f "$directory/state" ]]; then
		# shellcheck disable=SC1090,SC1091
		. "$directory/state"
		[[ "$saved_zone" == "$zone" ]] ||
		    die "runtime state identity mismatch for $zone"
		if deployment_healthy; then
			return 0
		fi
		log_error "repairing incomplete private-network state"
		remove_deployment "$directory" "$zone" ||
		    die "incomplete state could not be removed"
	fi
	for state in "$state_root"/*/state; do
		[[ -f "$state" ]] || continue
		if grep -F -x -q "subnet='$cidr'" "$state"; then
			die "private subnet is already active: $cidr"
		fi
	done

	prefix=$(allocate_prefix "$zone")
	stub=${prefix}s0
	host=${prefix}h0
	gateway=$network.1
	subnet=$cidr
	config=$control_root/$zone/lx-private-network.conf
	for ((index = 0; index < count; index++)); do
		link=${prefix}n$index
		address=$network.$((index + 2))/24
		[[ -z "$links" ]] || links+=,
		links+=$link=$address
	done

	mkdir "$directory" || die "cannot create runtime state: $directory"
	chmod 700 "$directory"
	write_state "$directory" preparing
	rollback_directory=$directory
	trap rollback_prepare EXIT
	trap 'exit 125' HUP INT TERM

	dladm create-etherstub -t "$stub"
	dladm create-vnic -t -l "$stub" "$host"
	for link in ${links//,/ }; do
		link=${link%%=*}
		dladm create-vnic -t -l "$stub" "$link"
	done
	ipadm create-if -t "$host"
	ipadm create-addr -t -T static -a "$gateway/24" "$host/v4"
	cat > "$directory/ipf-in-host.pending" <<EOF
pass in quick on $host from $subnet to any
EOF
	ipf -f "$directory/ipf-in-host.pending"
	mv "$directory/ipf-in-host.pending" "$directory/ipf-in-host.rules"
	cat > "$directory/ipf-out-egress.pending" <<EOF
pass out quick on $egress from $subnet to any
EOF
	ipf -f "$directory/ipf-out-egress.pending"
	mv "$directory/ipf-out-egress.pending" "$directory/ipf-out-egress.rules"
	cat > "$directory/ipnat-portmap.pending" <<EOF
map $egress $subnet -> 0/32 portmap tcp/udp auto
EOF
	ipnat -f "$directory/ipnat-portmap.pending"
	mv "$directory/ipnat-portmap.pending" "$directory/ipnat-portmap.rules"
	cat > "$directory/ipnat-map.pending" <<EOF
map $egress $subnet -> 0/32
EOF
	ipnat -f "$directory/ipnat-map.pending"
	mv "$directory/ipnat-map.pending" "$directory/ipnat-map.rules"
	for link in ${links//,/ }; do
		link=${link%%=*}
		dladm set-linkprop -t -p "zone=$zone" "$link"
	done
	write_guest_config
	write_state "$directory" ready
	trap - EXIT HUP INT TERM
	rollback_directory=
	printf 'LX_PRIVATE_NETWORK_READY zone=%s links=%s gateway=%s\n' \
	    "$zone" "$count" "$gateway"
}

cleanup()
{
	(( $# == 2 )) || usage
	typeset zone=$1 zonepath=$2 directory config

	zone_for_log=$zone
	[[ "$zonepath" =~ ^/[A-Za-z0-9_./-]+$ ]] ||
	    die "cannot determine a safe zone path"
	directory=$(runtime_directory "$zone")
	config=$control_root/$zone/lx-private-network.conf
	if [[ -f "$directory/state" ]]; then
		remove_deployment "$directory" "$zone" ||
		    die "private-network cleanup is incomplete; state retained"
	elif [[ -e "$config" ]]; then
		rm -f "$config" || die "cannot remove zone-control configuration"
	fi
	printf 'LX_PRIVATE_NETWORK_REMOVED zone=%s\n' "$zone"
}

status_command()
{
	(( $# <= 1 )) || usage
	typeset requested=${1:-} path found=0 status health
	typeset saved_zone subnet egress count phase config stub

	for path in "$state_root"/*/state; do
		[[ -f "$path" ]] || continue
		# shellcheck disable=SC1090
		. "$path"
		[[ -z "$requested" || "$saved_zone" == "$requested" ]] || continue
		status=$(zoneadm -z "$saved_zone" list -p 2>/dev/null |
		    awk -F: 'NR == 1 { print $3 }')
		health=${phase:-stale}
		if [[ ${phase:-} == ready && "$status" == running && -f "$config" ]] &&
		    dladm show-link -p -o link "$stub" >/dev/null 2>&1; then
			health=ready
		fi
		printf 'zone=%s subnet=%s egress=%s links=%s status=%s health=%s\n' \
		    "$saved_zone" "$subnet" "$egress" "$count" \
		    "${status:-missing}" "$health"
		found=1
	done
	if (( found == 0 )); then
		[[ -z "$requested" ]] || die "zone has no active private network"
		printf '%s\n' 'no active lx private networks'
	fi
}

(( $# >= 1 )) || usage
lock_and_restart "$@"
command=$1
shift
case "$command" in
prepare)
	prepare "$@"
	;;
quiesce)
	quiesce "$@"
	;;
cleanup)
	cleanup "$@"
	;;
status)
	status_command "$@"
	;;
*)
	usage
	;;
esac
