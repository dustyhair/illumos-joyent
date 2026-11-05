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

The brand writes a host-owned configuration file and exposes it read-only at
`/native/.zonecontrol/lx-private-network.conf`. Install this package and the
distribution's normal `bubblewrap` package, then call `lx-bwrap` with ordinary
bubblewrap arguments:

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
created while another is being torn down. The launcher keeps its lock file
descriptor out of the payload, forwards HUP, INT, and TERM to bubblewrap, and
retains the lock through an uninterrupted grace period. Do not send SIGKILL to
the launcher because no process can run teardown handling after SIGKILL.
This is an experimental guest-side mitigation, not a host safety boundary. A
guest can bypass the launcher. Serialization allows only one active networked
sandbox per LX zone.
Additional configured links are reserved for future concurrent use after the
host race is fixed.

Stop all `lx-bwrap` processes before installing or upgrading the package.
Version 0.1.4 locks the package directory so later package upgrades keep the
same lock inode, but an upgrade from 0.1.2 changes the lock location.

The argument rewriter follows the bubblewrap 0.11.2 option table. It rejects
unknown options rather than risk interpreting an option operand as the
command separator. `--args` is unsupported because it can hide options in a
file descriptor, and `--share-net` conflicts with this command's purpose.

The package currently supports x86-64 LX guests. `make test` builds both
helpers and checks version consistency, argument rewriting, lock descriptor
ownership, serialized launches, signal forwarding, exit status, and teardown
grace. Build the package formats with:

```sh
make deb
make xbps
```

The package depends on `util-linux` for `flock`. The `deb` target requires
`dpkg-deb`, and the `xbps` target requires `xbps-create`. The helper is linked
statically with the libc selected by `CC`; building on Void does not by itself
select musl. The libc and target architecture policy remains packaging work
for a future release.
