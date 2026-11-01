#!/usr/bin/bash
set -o errexit
set -o nounset
set -o pipefail

reset_switch()
{
	printf '%s\n' 'lx_brand`lx_ipcns_experimental/W 0' | mdb -kw
}
trap reset_switch EXIT

test "$(zonename)" = global
zoneadm -z lx-probe list -p | grep -q ':running:'
reset_switch
zlogin -S lx-probe /test/ipcns_restricted disabled
printf '%s\n' 'lx_brand`lx_ipcns_experimental/W 1' | mdb -kw
zlogin -S lx-probe /test/ipcns_restricted enabled
reset_switch
printf '%s\n' 'lx_brand`lx_ipcns_experimental/D' | mdb -k
echo IPCNS_TEST_PASS
