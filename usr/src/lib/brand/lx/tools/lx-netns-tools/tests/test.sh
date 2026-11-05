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

here=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)

cleanup()
{
	result=$?
	trap - EXIT HUP INT TERM
	rm -rf "$work"
	exit "$result"
}

trap cleanup EXIT HUP INT TERM

wrapper=$here/build/lx-bwrap-wrapper
helper=$here/build/lx-netns-helper
launcher=$here/src/lx-bwrap
links='lx12345678n0=10.88.0.2/24,lx12345678n1=10.88.0.3/24'
gateway=10.88.0.1

wait_for_path()
{
	local path=$1
	local attempt

	for ((attempt = 0; attempt < 500; attempt++)); do
		[[ -e "$path" ]] && return
		/usr/bin/sleep 0.01
	done
	printf 'timed out waiting for %s\n' "$path" >&2
	return 1
}

private=$(env -i LX_NETNS_REAL_BWRAP=/bin/echo \
    LX_NETNS_HELPER=/usr/libexec/lx-netns-tools/lx-netns-helper \
    LX_PRIVATE_NETWORK_LINKS=$links LX_PRIVATE_NETWORK_GATEWAY=$gateway \
    "$wrapper" --unshare-user -- /bin/true argument)
expected='--unshare-user --unshare-net --cap-add CAP_NET_ADMIN '
expected+='--cap-add CAP_SETPCAP -- '
expected+='/usr/libexec/lx-netns-tools/lx-netns-helper '
expected+='--payload-argv0 /bin/true '
expected+="--links $links --gateway $gateway -- /bin/true argument"
[[ "$private" == "$expected" ]]

with_argv0=$(env -i LX_NETNS_REAL_BWRAP=/bin/echo \
    LX_NETNS_HELPER=/helper LX_PRIVATE_NETWORK_LINKS=$links \
    LX_PRIVATE_NETWORK_GATEWAY=$gateway "$wrapper" --unshare-net \
    --argv0 payload -- /bin/true)
expected_argv0='--unshare-net --argv0 payload --cap-add CAP_NET_ADMIN '
expected_argv0+='--cap-add CAP_SETPCAP -- /helper '
expected_argv0+='--payload-argv0 payload '
expected_argv0+="--links $links --gateway $gateway -- /bin/true"
[[ "$with_argv0" == "$expected_argv0" ]]

separator_argv0=$(env -i LX_NETNS_REAL_BWRAP=/bin/echo \
    LX_NETNS_HELPER=/helper LX_PRIVATE_NETWORK_LINKS=$links \
    LX_PRIVATE_NETWORK_GATEWAY=$gateway "$wrapper" \
    --argv0 -- -- /bin/true)
expected_separator_argv0='--argv0 -- --unshare-net '
expected_separator_argv0+='--cap-add CAP_NET_ADMIN --cap-add CAP_SETPCAP '
expected_separator_argv0+='-- /helper --payload-argv0 -- '
expected_separator_argv0+="--links $links --gateway $gateway -- /bin/true"
[[ "$separator_argv0" == "$expected_separator_argv0" ]]

setenv_option=$(env -i LX_NETNS_REAL_BWRAP=/bin/echo \
    LX_NETNS_HELPER=/helper LX_PRIVATE_NETWORK_LINKS=$links \
    LX_PRIVATE_NETWORK_GATEWAY=$gateway "$wrapper" \
    --setenv TEST --unshare-net -- /bin/true)
expected_setenv='--setenv TEST --unshare-net --unshare-net '
expected_setenv+='--cap-add CAP_NET_ADMIN --cap-add CAP_SETPCAP -- '
expected_setenv+='/helper --payload-argv0 /bin/true '
expected_setenv+="--links $links --gateway $gateway -- /bin/true"
[[ "$setenv_option" == "$expected_setenv" ]]

separator_operand=$(env -i LX_NETNS_REAL_BWRAP=/bin/echo \
    LX_NETNS_HELPER=/helper LX_PRIVATE_NETWORK_LINKS=$links \
    LX_PRIVATE_NETWORK_GATEWAY=$gateway "$wrapper" \
    --setenv TEST -- -- /bin/true)
expected_separator_operand='--setenv TEST -- --unshare-net '
expected_separator_operand+='--cap-add CAP_NET_ADMIN '
expected_separator_operand+='--cap-add CAP_SETPCAP -- '
expected_separator_operand+='/helper --payload-argv0 /bin/true '
expected_separator_operand+="--links $links --gateway $gateway -- /bin/true"
[[ "$separator_operand" == "$expected_separator_operand" ]]

cat > "$work/config" <<EOF
LX_PRIVATE_NETWORK_LINKS='$links'
LX_PRIVATE_NETWORK_GATEWAY='$gateway'
EOF
mkdir "$work/run.lock"
launcher_output=$(LX_NETNS_CONFIG=$work/config \
    LX_NETNS_REAL_BWRAP=/bin/echo LX_NETNS_HELPER=$helper \
    LX_NETNS_WRAPPER=$wrapper LX_NETNS_RUN_LOCK=$work/run.lock \
    "$launcher" -- /bin/true)
[[ "$launcher_output" == *"--links $links --gateway $gateway"* ]]

cat > "$work/probe-wrapper" <<'EOF'
#!/usr/bin/bash
set -o errexit
prefix=$LX_NETNS_TEST_DIR/$LX_NETNS_TEST_ID
active=$LX_NETNS_TEST_DIR/active
owned=false
sleep_pid=

finish()
{
	if $owned; then
		rmdir "$active"
		owned=false
	fi
	: > "$prefix-done"
}

on_signal()
{
	local status=$1

	if [[ -n "$sleep_pid" ]]; then
		kill -TERM "$sleep_pid" 2>/dev/null || true
		wait "$sleep_pid" 2>/dev/null || true
	fi
	finish
	exit "$status"
}

if [[ -e /proc/self/fd/9 ]]; then
	: > "$prefix-fd9"
fi
if ! mkdir "$active" 2>/dev/null; then
	: > "$LX_NETNS_TEST_DIR/overlap"
	exit 99
fi
owned=true
trap 'on_signal 129' HUP
trap 'on_signal 130' INT
trap 'on_signal 143' TERM
: > "$prefix-entered"
/usr/bin/sleep "${LX_NETNS_TEST_SLEEP:-0.2}" &
sleep_pid=$!
wait "$sleep_pid"
sleep_pid=
finish
exit "${LX_NETNS_TEST_STATUS:-0}"
EOF
chmod 755 "$work/probe-wrapper"

LX_NETNS_TEST_DIR=$work LX_NETNS_TEST_ID=first \
    LX_NETNS_CONFIG=$work/config LX_NETNS_REAL_BWRAP=/bin/true \
    LX_NETNS_HELPER=/bin/true LX_NETNS_WRAPPER=$work/probe-wrapper \
    LX_NETNS_RUN_LOCK=$work/run.lock "$launcher" -- /bin/true &
first_pid=$!
wait_for_path "$work/first-entered"
LX_NETNS_TEST_DIR=$work LX_NETNS_TEST_ID=second \
    LX_NETNS_CONFIG=$work/config LX_NETNS_REAL_BWRAP=/bin/true \
    LX_NETNS_HELPER=/bin/true LX_NETNS_WRAPPER=$work/probe-wrapper \
    LX_NETNS_RUN_LOCK=$work/run.lock "$launcher" -- /bin/true &
second_pid=$!
wait_for_path "$work/first-done"
/usr/bin/sleep 0.1
[[ ! -e "$work/second-entered" ]]
if /usr/bin/flock -n "$work/run.lock" /bin/true; then
	printf '%s\n' 'launcher released its lock before teardown grace' >&2
	exit 1
fi
wait "$first_pid"
wait "$second_pid"
[[ ! -e "$work/overlap" ]]
[[ ! -e "$work/first-fd9" ]]
[[ ! -e "$work/second-fd9" ]]
/usr/bin/flock -n "$work/run.lock" /bin/true

set +o errexit
LX_NETNS_TEST_DIR=$work LX_NETNS_TEST_ID=failure \
    LX_NETNS_TEST_STATUS=42 LX_NETNS_CONFIG=$work/config \
    LX_NETNS_REAL_BWRAP=/bin/true LX_NETNS_HELPER=/bin/true \
    LX_NETNS_WRAPPER=$work/probe-wrapper \
    LX_NETNS_RUN_LOCK=$work/run.lock "$launcher" -- /bin/true
failure_status=$?
set -o errexit
[[ $failure_status == 42 ]]

LX_NETNS_TEST_DIR=$work LX_NETNS_TEST_ID=signal LX_NETNS_TEST_SLEEP=30 \
    LX_NETNS_CONFIG=$work/config LX_NETNS_REAL_BWRAP=/bin/true \
    LX_NETNS_HELPER=/bin/true LX_NETNS_WRAPPER=$work/probe-wrapper \
    LX_NETNS_RUN_LOCK=$work/run.lock "$launcher" -- /bin/true &
signal_pid=$!
wait_for_path "$work/signal-entered"
kill -TERM "$signal_pid"
wait_for_path "$work/signal-done"
kill -TERM "$signal_pid"
/usr/bin/sleep 0.1
kill -0 "$signal_pid"
if /usr/bin/flock -n "$work/run.lock" /bin/true; then
	printf '%s\n' 'signal interrupted the teardown grace period' >&2
	exit 1
fi
set +o errexit
wait "$signal_pid"
signal_status=$?
set -o errexit
[[ $signal_status == 143 ]]
/usr/bin/flock -n "$work/run.lock" /bin/true

set +o errexit
env -i LX_NETNS_REAL_BWRAP=/bin/echo LX_NETNS_HELPER=/helper \
    LX_PRIVATE_NETWORK_LINKS=$links LX_PRIVATE_NETWORK_GATEWAY=$gateway \
    "$wrapper" --args 7 -- /bin/true > "$work/args.out" 2>&1
args_status=$?
env -i LX_NETNS_REAL_BWRAP=/bin/echo LX_NETNS_HELPER=/helper \
    LX_PRIVATE_NETWORK_LINKS=$links LX_PRIVATE_NETWORK_GATEWAY=$gateway \
    "$wrapper" --share-net -- /bin/true > "$work/share.out" 2>&1
share_status=$?
env -i LX_NETNS_REAL_BWRAP=/bin/echo LX_NETNS_HELPER=/helper \
    LX_PRIVATE_NETWORK_LINKS=$links LX_PRIVATE_NETWORK_GATEWAY=$gateway \
    "$wrapper" --future-option -- /bin/true > "$work/unknown.out" 2>&1
unknown_status=$?
"$helper" --payload-argv0 test --links bad --gateway "$gateway" -- \
    /bin/true > "$work/helper.out" 2>&1
helper_status=$?
set -o errexit
[[ $args_status == 125 ]]
grep -q -- '--args is not supported' "$work/args.out"
[[ $share_status == 125 ]]
grep -q -- '--share-net conflicts' "$work/share.out"
[[ $unknown_status == 125 ]]
grep -q 'unsupported bubblewrap option' "$work/unknown.out"
[[ $helper_status == 125 ]]
grep -q 'name=address/prefix' "$work/helper.out"

if command -v shellcheck >/dev/null 2>&1; then
	shellcheck "$launcher" "$0"
fi
if grep -R -i -n codex "$here" --exclude-dir=build --exclude=test.sh; then
	printf '%s\n' 'application-specific name found in package source' >&2
	exit 1
fi

printf '%s\n' LX_NETNS_TOOLS_TEST_PASS
