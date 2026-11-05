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
# Prepare private networking for an lx zone.  The persistent configuration is
# stored in zone attributes managed by vmadm.  This file owns runtime state
# only; zone boot and halt remain under zoneadm/vmadm control.
#

set -o errexit
set -o nounset
set -o pipefail

PATH=${LX_PRIVATE_NETWORK_PATH:-/usr/sbin:/usr/bin:/sbin}
export PATH

state_root=${LX_PRIVATE_NETWORK_STATE_DIR:-/var/run/lx-private-network}
lock_dir=${LX_PRIVATE_NETWORK_LOCK_DIR:-/var/run/lx-private-network.lock}
rollback_directory=
gate_names=(seccomp userns mntns pidns ipcns netns)

usage()
{
	cat >&2 <<EOF
usage:
  lx_private_network prepare ZONE ZONEPATH
  lx_private_network cleanup ZONE ZONEPATH
  lx_private_network status [ZONE]

These commands are lx brand lifecycle hooks.  Configure the feature through
the lx_private_network vmadm properties instead of invoking this file directly.
EOF
	exit 2
}

die()
{
	printf 'lx private network: %s\n' "$*" >&2
	exit 1
}

acquire_lock()
{
	local -i attempt=0

	mkdir -p "$state_root"
	chmod 700 "$state_root"
	while ! mkdir "$lock_dir" 2>/dev/null; do
		(( attempt += 1 ))
		if (( attempt == 10 )); then
			die "could not acquire $lock_dir"
		fi
		sleep 1
	done
}

release_lock()
{
	rmdir "$lock_dir" 2>/dev/null || true
}

zone_attr()
{
	local zone=$1 name=$2

	zonecfg -z "$zone" info attr "name=$name" 2>/dev/null |
	    awk '$1 == "value:" { print $2; exit }'
}

zone_prefix()
{
	local compact=${1//-/}

	[[ "$compact" =~ ^[0-9A-Fa-f]{32}$ ]] ||
	    die "zone name is not a UUID: $1"
	printf 'lx%s\n' "${compact:0:8}"
}

deployment_count()
{
	local -i count=0
	local path

	for path in "$state_root"/*/state; do
		[[ -f "$path" ]] || continue
		(( count += 1 ))
	done
	printf '%s\n' "$count"
}

read_gate()
{
	local gate=$1 value

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

set_gate()
{
	local gate=$1 value=$2

	printf 'lx_brand`lx_%s_experimental/W %s\n' "$gate" "$value" |
	    mdb -kw >/dev/null
}

set_all_gates()
{
	local value=$1 gate

	for gate in "${gate_names[@]}"; do
		set_gate "$gate" "$value"
	done
}

nat_counts()
{
	ipnat -l | awk '
	    /^List of active MAP\/Redirect filters:/ { section = "rules"; next }
	    /^List of active sessions:/ { section = "sessions"; next }
	    NF != 0 && section == "rules" { rules++ }
	    NF != 0 && section == "sessions" { sessions++ }
	    END { print rules + 0, sessions + 0 }
	'
}

flush_owned_nat_sessions()
{
	local nat_rules nat_sessions

	[[ -f "$state_root/base.state" ]] || return
	. "$state_root/base.state"
	read -r nat_rules nat_sessions <<< "$(nat_counts)"
	if [[ "${nat_rules_were_empty:-no}" == yes &&
	    "${nat_sessions_were_empty:-no}" == yes &&
	    "$nat_sessions" != 0 ]]; then
		ipnat -F
	fi
}

write_base_state()
{
	local forwarding ipf_running gate temporary nat_rules nat_sessions
	local -a values
	local -i index

	forwarding=$(ipadm show-prop -c -o current -p forwarding ipv4)
	ipf_running=$(ipf -V | awk '/^Running:/ { print $2 }')
	[[ "$forwarding" == on || "$forwarding" == off ]] ||
	    die "cannot read the IPv4 forwarding property"
	[[ "$ipf_running" == yes || "$ipf_running" == no ]] ||
	    die "cannot read the IP Filter state"
	read -r nat_rules nat_sessions <<< "$(nat_counts)"
	for gate in "${gate_names[@]}"; do
		values+=("$(read_gate "$gate")")
	done
	temporary=$state_root/base.state.$$
	{
		printf 'forwarding_was=%s\n' "$forwarding"
		printf 'ipf_was=%s\n' "$ipf_running"
		printf 'nat_rules_were_empty=%s\n' \
		    "$([[ "$nat_rules" == 0 ]] && printf yes || printf no)"
		printf 'nat_sessions_were_empty=%s\n' \
		    "$([[ "$nat_sessions" == 0 ]] && printf yes || printf no)"
		for ((index = 0; index < ${#gate_names[@]}; index++)); do
			printf '%s_was=%s\n' "${gate_names[index]}" \
			    "${values[index]}"
		done
	} > "$temporary"
	chmod 600 "$temporary"
	mv "$temporary" "$state_root/base.state"
}

restore_base_state()
{
	local gate variable value

	[[ -f "$state_root/base.state" ]] || return
	. "$state_root/base.state"
	ipadm set-prop -t -p "forwarding=$forwarding_was" ipv4
	for gate in "${gate_names[@]}"; do
		variable=${gate}_was
		value=${!variable}
		[[ "$value" == 0 || "$value" == 1 ]] ||
		    die "invalid saved gate value: $gate"
		set_gate "$gate" "$value"
	done
	flush_owned_nat_sessions
	if [[ "$ipf_was" == no ]]; then
		ipf -D 2>/dev/null || true
	fi
	rm -f "$state_root/base.state"
}

remove_deployment()
{
	local directory=$1
	local link rules
	local -i last=0

	[[ -f "$directory/state" ]] || return
	. "$directory/state"
	if [[ -f "$directory/ipnat.rules" ]]; then
		ipnat -r -f "$directory/ipnat.rules" 2>/dev/null || true
	fi
	flush_owned_nat_sessions
	if (( $(deployment_count) == 1 )); then
		last=1
	fi
	for rules in "$directory"/ipf-*.rules; do
		[[ -f "$rules" ]] || continue
		ipf -r -f "$rules" 2>/dev/null || true
	done
	[[ ! -f "$config" ]] || rm -f "$config"
	ipadm delete-addr "$host/v4" 2>/dev/null || true
	ipadm delete-if "$host" 2>/dev/null || true
	for link in ${links//,/ }; do
		link=${link%%=*}
		dladm reset-linkprop -t -p zone "$link" 2>/dev/null || true
		dladm delete-vnic "$link" 2>/dev/null || true
	done
	dladm delete-vnic "$host" 2>/dev/null || true
	dladm delete-etherstub "$stub" 2>/dev/null || true
	rm -f "$directory/ipnat.rules" "$directory"/ipf-*.rules \
	    "$directory/state"
	rmdir "$directory"
	if (( last != 0 )); then
		restore_base_state
	fi
}

rollback_prepare()
{
	local result=$?

	trap - EXIT HUP INT TERM
	set +o errexit
	remove_deployment "$rollback_directory"
	release_lock
	exit "$result"
}

validate_subnet()
{
	local cidr=$1 first second third value

	[[ "$cidr" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)\.0/24$ ]] ||
	    die "lx_private_network_subnet must be an IPv4 /24 network"
	first=${BASH_REMATCH[1]}
	second=${BASH_REMATCH[2]}
	third=${BASH_REMATCH[3]}
	for value in "$first" "$second" "$third"; do
		[[ "$value" == 0 || "$value" != 0* ]] ||
		    die "private network octets must not contain leading zeroes"
		(( 10#$value <= 255 )) || die "invalid private network octet"
	done
	if ! (( first == 10 || (first == 172 && second >= 16 && second <= 31) ||
	    (first == 192 && second == 168) )); then
		die "lx_private_network_subnet must use an RFC 1918 network"
	fi
	printf '%s.%s.%s\n' "$first" "$second" "$third"
}

prepare()
{
	(( $# == 2 )) || usage
	local zone=$1 zonepath=$2 enabled egress cidr network count prefix
	local directory stub host gateway subnet zone_root config
	local links='' link rules address status first=0 state
	local -i index

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
	[[ "$count" =~ ^[0-9]+$ ]] || count=4
	(( count >= 1 && count <= 32 )) ||
	    die "lx_private_network_links must be from 1 to 32"
	dladm show-link -p -o link "$egress" >/dev/null 2>&1 ||
	    die "egress link does not exist: $egress"
	status=$(zoneadm -z "$zone" list -p 2>/dev/null |
	    awk -F: 'NR == 1 { print $3 }')
	[[ "$status" == ready || "$status" == running ]] ||
	    die "zone must be ready or running, not ${status:-missing}"

	prefix=$(zone_prefix "$zone")
	directory=$state_root/$prefix
	zone_root=$zonepath/root
	config=$zone_root/etc/lx-private-network.conf
	if [[ -f "$directory/state" ]]; then
		. "$directory/state"
		[[ "$saved_zone" == "$zone" ]] ||
		    die "runtime prefix collision for $prefix"
		return 0
	fi
	for state in "$state_root"/*/state; do
		[[ -f "$state" ]] || continue
		if grep -q "^subnet='$cidr'$" "$state"; then
			die "private subnet is already active: $cidr"
		fi
	done

	stub=${prefix}s0
	host=${prefix}h0
	gateway=$network.1
	subnet=$cidr
	for ((index = 0; index < count; index++)); do
		link=${prefix}n$index
		address=$network.$((index + 2))/24
		[[ -z "$links" ]] || links+=,
		links+=$link=$address
	done

	if (( $(deployment_count) == 0 )); then
		write_base_state
		first=1
	fi
	if ! mkdir "$directory"; then
		(( first == 0 )) || restore_base_state
		die "cannot create runtime state: $directory"
	fi
	chmod 700 "$directory"
	cat > "$directory/state.new" <<EOF
saved_zone='$zone'
prefix='$prefix'
egress='$egress'
network='$network'
count='$count'
stub='$stub'
host='$host'
gateway='$gateway'
subnet='$subnet'
zone_root='$zone_root'
config='$config'
links='$links'
EOF
	chmod 600 "$directory/state.new"
	mv "$directory/state.new" "$directory/state"

	rollback_directory=$directory
	trap rollback_prepare EXIT
	trap 'exit 125' HUP INT TERM
	ipadm set-prop -t -p forwarding=on ipv4
	set_all_gates 1
	ipf -E 2>/dev/null || true
	dladm create-etherstub -t "$stub"
	dladm create-vnic -t -l "$stub" "$host"
	for link in ${links//,/ }; do
		link=${link%%=*}
		dladm create-vnic -t -l "$stub" "$link"
	done
	ipadm create-if -t "$host"
	ipadm create-addr -t -T static -a "$gateway/24" "$host/v4"
	cat > "$directory/ipf-in-host.rules" <<EOF
pass in quick on $host from $subnet to any
EOF
	cat > "$directory/ipf-in-egress.rules" <<EOF
pass in quick on $egress from any to $subnet
EOF
	cat > "$directory/ipf-out-host.rules" <<EOF
pass out quick on $host from any to $subnet
EOF
	cat > "$directory/ipf-out-egress.rules" <<EOF
pass out quick on $egress from $subnet to any
EOF
	cat > "$directory/ipnat.rules" <<EOF
map $egress $subnet -> 0/32 portmap tcp/udp auto
map $egress $subnet -> 0/32
EOF
	for rules in "$directory"/ipf-*.rules; do
		ipf -f "$rules"
	done
	ipnat -f "$directory/ipnat.rules"
	for link in ${links//,/ }; do
		link=${link%%=*}
		dladm set-linkprop -t -p "zone=$zone" "$link"
	done
	cat > "$config.new" <<EOF
LX_PRIVATE_NETWORK_LINKS='$links'
LX_PRIVATE_NETWORK_GATEWAY='$gateway'
EOF
	chmod 644 "$config.new"
	mv "$config.new" "$config"
	trap - EXIT HUP INT TERM
	rollback_directory=
	printf 'LX_PRIVATE_NETWORK_READY zone=%s links=%s gateway=%s\n' \
	    "$zone" "$count" "$gateway"
}

cleanup()
{
	(( $# == 2 )) || usage
	local zone=$1 zonepath=$2 prefix directory config

	[[ "$zonepath" =~ ^/[A-Za-z0-9_./-]+$ ]] ||
	    die "cannot determine a safe zone path"
	prefix=$(zone_prefix "$zone")
	directory=$state_root/$prefix
	config=$zonepath/root/etc/lx-private-network.conf
	if [[ -f "$directory/state" ]]; then
		remove_deployment "$directory"
	else
		rm -f "$config"
	fi
	printf 'LX_PRIVATE_NETWORK_REMOVED zone=%s\n' "$zone"
}

status_command()
{
	(( $# <= 1 )) || usage
	local requested=${1:-} path found=0 status health

	for path in "$state_root"/*/state; do
		[[ -f "$path" ]] || continue
		. "$path"
		[[ -z "$requested" || "$saved_zone" == "$requested" ]] || continue
		status=$(zoneadm -z "$saved_zone" list -p 2>/dev/null |
		    awk -F: 'NR == 1 { print $3 }')
		health=stale
		if [[ "$status" == running && -f "$config" ]] &&
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
command=$1
shift
case "$command" in
prepare|cleanup|status)
	acquire_lock
	trap release_lock EXIT HUP INT TERM
	case "$command" in
	prepare)
		prepare "$@"
		;;
	cleanup)
		cleanup "$@"
		;;
	status)
		status_command "$@"
		;;
	esac
	trap - EXIT HUP INT TERM
	release_lock
	;;
*)
	usage
	;;
esac
