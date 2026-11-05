#!/usr/bin/bash

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

cat > "$work/config" <<EOF
LX_PRIVATE_NETWORK_LINKS='$links'
LX_PRIVATE_NETWORK_GATEWAY='$gateway'
EOF
: > "$work/run.lock"
launcher_output=$(LX_NETNS_CONFIG=$work/config \
    LX_NETNS_REAL_BWRAP=/bin/echo LX_NETNS_HELPER=$helper \
    LX_NETNS_WRAPPER=$wrapper LX_NETNS_RUN_LOCK=$work/run.lock \
    "$launcher" -- /bin/true)
[[ "$launcher_output" == *"--links $links --gateway $gateway"* ]]

set +o errexit
env -i LX_NETNS_REAL_BWRAP=/bin/echo LX_NETNS_HELPER=/helper \
    LX_PRIVATE_NETWORK_LINKS=$links LX_PRIVATE_NETWORK_GATEWAY=$gateway \
    "$wrapper" --unshare-net --unshare-net -- /bin/true \
    > "$work/duplicate.out" 2>&1
duplicate_status=$?
"$helper" --payload-argv0 test --links bad --gateway "$gateway" -- \
    /bin/true > "$work/helper.out" 2>&1
helper_status=$?
set -o errexit
[[ $duplicate_status == 125 ]]
grep -q 'expected at most one --unshare-net' "$work/duplicate.out"
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
