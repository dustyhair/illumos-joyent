# lx-netns-tools

`lx-netns-tools` connects stock bubblewrap to the private VNIC pool assigned
by the SmartOS LX brand. It contains no global-zone controller and does not
create host links, forwarding rules, or NAT rules.

Configure private networking while the zone is stopped:

```sh
vmadm update UUID \
    lx_private_network=true \
    lx_private_network_egress=e1000g0 \
    lx_private_network_subnet=10.88.0.0/24 \
    lx_private_network_links=2
vmadm start UUID
```

The brand writes `/etc/lx-private-network.conf` inside the zone. Install this
package and the distribution's normal `bubblewrap` package, then call
`lx-bwrap` with ordinary bubblewrap arguments:

```sh
lx-bwrap \
    --unshare-user \
    --ro-bind /usr /usr \
    --ro-bind /etc /etc \
    --dev-bind /dev /dev \
    --proc /proc \
    -- /usr/bin/curl -I https://example.com/
```

`lx-bwrap` adds `--unshare-net` if needed. The helper claims the first
available assigned VNIC, brings it up, adds its IPv4 address and default
route, waits for duplicate-address detection, and drops `CAP_NET_ADMIN` and
`CAP_SETPCAP` from every capability set before it executes the command.
Networked `lx-bwrap` invocations are serialized for their complete lifetime,
including a one-second teardown grace period. Current SmartOS LX kernels can
panic in the host MAC polling path if one VNIC-backed network namespace is
created while another is being torn down. Serialization makes the packaged
interface safe at the cost of allowing only one active networked sandbox per
LX zone. Additional configured links are reserved for future concurrent use
after the host race is fixed.

The package currently supports x86-64 LX guests. Build both package formats
with:

```sh
make deb
make xbps
```

The package depends on `util-linux` for `flock`. The `deb` target requires
`dpkg-deb`. The `xbps` target requires `xbps-create` and should run on Void
Linux so the static helper uses musl.
