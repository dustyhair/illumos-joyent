# Codex lx private-network deployment

This profile gives each bubblewrap sandbox its own lx network namespace and
IPv4 address. A pool of vNICs bounds concurrency. The global zone routes the
pool's `/24` through an operator-selected egress link and IP Filter applies
source NAT.

`codex-lx-network` owns the host lifecycle. It creates one etherstub, a host
gateway vNIC and one sandbox vNIC per pool slot. It also enables IPv4
forwarding, enables `lx_netns_experimental`, loads narrow forwarding rules and
loads two NAT rules. The script records the previous shared settings and
restores them after removing the last managed deployment.

The prefix in the example produces `clxstub0`, `clxhost0` and sandbox links
`clxnet0` through `clxnet3`. The numeric suffixes satisfy SmartOS datalink
naming rules.

The setup command requires an installed lx zone and boots it after assigning
the links. The teardown command halts that zone. Both operations hold a global
lock. A failed setup runs the same removal path as an explicit teardown.

For a four-slot pool on `10.88.0.0/24` using `external0` as the egress link:

```sh
codex-lx-network setup lx-codex clx external0 10.88.0 4
```

The command writes `/etc/codex-lx-network.conf` inside the zone. Start Codex
through the packaged `codex-lx` launcher. Private mode is the default:

```sh
/opt/codex-lx/codex-lx
```

The launcher selects a workspace-derived Codex permission profile with network
syscalls enabled. In private mode, the bubblewrap integrity wrapper injects
the single `--unshare-net` option that this profile omits; it also accepts and
retains one supplied by older Codex builds. The wrapper temporarily adds
`CAP_NET_ADMIN` and `CAP_SETPCAP`, then inserts `codex-lx-net` as the sandbox
command. The helper races only for links listed in the
root-owned config, claims one assigned link, brings it up, adds its address,
waits for DAD to finish and installs the default route. Before it execs the
requested command, it removes both setup capabilities from the ambient,
bounding, permitted, effective and inheritable sets and verifies the removal.
It also preserves bubblewrap's requested payload `argv[0]`, which Codex uses
to select its internal Linux sandbox runner.
Exiting the namespace returns the link to the zone, so there is no per-command
host cleanup process.

At most four sandbox commands can hold a link in this example. A fifth command
fails with exit status 125 instead of sharing another command's network
namespace. Increase the pool count, up to 32, if Codex needs more concurrent
sandboxes.

Remove the deployment with:

```sh
codex-lx-network teardown clx
```

The controller keeps its deployment description under
`/var/codex-lx-network`; datalinks, addresses and rules remain transient. After
a global-zone reboot, restore a deployment before using Codex:

```sh
codex-lx-network reapply clx
```

`reapply` removes any partial runtime state and rebuilds the deployment from
the saved description. A later packaging step can call it from an SMF method;
the controller keeps that service free of link, rule and rollback logic.

Shared networking remains an explicit compatibility mode:

```sh
CODEX_LX_NETWORK_MODE=shared /opt/codex-lx/codex-lx
```

Shared mode removes a supplied `--unshare-net` exactly as the VM168 package
did and accepts its absence from network-enabled Codex profiles. Private mode
never falls back to shared networking when the config or link pool is missing.

## Accepted data path

VM201 exercised this packaged launcher, wrapper, helper and controller with
real Codex 0.166.0. Two sequential sandbox commands each claimed `clxnet0`,
configured `10.88.0.2/24`, removed the temporary setup capabilities and
completed a UDP exchange through the controller's forwarding and NAT rules.
The peer, running in a separate private netstack behind the selected egress
link, observed `10.99.0.1`; IPNAT recorded the live mapping from
`10.88.0.2`. The second command's clone flags were `0x78020011`, including
`CLONE_NEWNET`. Both detached lifecycle KTESTs passed, and teardown left only
the global netstack.
