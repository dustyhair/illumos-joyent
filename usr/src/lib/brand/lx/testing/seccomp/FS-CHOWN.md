# Scoped filesystem ownership permission

The scoped CAP_CHOWN path keeps native credential privileges at the existing
basic floor. One appended brand_ops callback evaluates the requested ownership
attributes against the filesystem's supplied owner/group snapshot. Existing
brand-credential references pin its namespace and module. It does not create
a temporary privileged credential or authorize a path before a later lookup.

Ordinary owner/supplementary-group operations defer to native policy. An
operation requiring CAP_CHOWN requires effective scoped authority and mapped
old inode UID/GID. Requested ownership IDs must also be mapped. Grants are
currently limited to ZFS and tmpfs. Other filesystem grants, CAP_FOWNER and
DAC override are not implemented here.

Native setattr policy checks this ownership permission. Tmpfs holds its node
lock across policy and update. ZFS additionally checks current owner/group
under its ACL writer lock and znode lock immediately before applying owner
updates, including its ACL take-ownership fast path. A late denial releases
the locks and commits the already-assigned transaction without applying the
requested ownership update. Existing cleanup frees prepared ACL/FUID state.
FUID allocation state is synchronized if needed. Other filesystem protections
and setid-clearing behavior remain in place.

## Tests

The57-case identity probes pass on Linux as static and dynamic binaries with
-Wall -Wextra -Werror. Five added cases check ownership changes on regular
files and tmpfs, clearing executable setid bits, mapped target validation,
effective versus permitted capability, ordinary group rights, rejection of
unmapped inode owners/groups and thread-local versus cached-file authority.
The initial new assertions incorrectly treated owner_is as an error-code
return; those assertions were corrected before the successful controls.

The native VM driver configures a private16MiB tmpfs through zonecfg,
passes its path to probes and lets zone lifecycle manage mounting/unmounting.
This zoneadm verifies special paths; /dev/null supplies a valid path, while
tmpfs ignores it and reports its resource as swap. Option values are quoted
for the native zonecfg lexer. It never
mounts or changes production hosts. Native kernel, brand, procfs, ELF, ZFS and
namespace tests compile cleanly in release/debug configurations. Common and
module source is af19067878; final driver is10b6f7b3fb. VM55 failed before
kernel entry because the external ISO unix319c differed from archive unix438b.
The canonical updater now exports the matching unix plus SHA256; local helper
lx-sandbox/refresh-lab-image downloads/verifies the pair and rebuilds the ISO.
VM56 booted the same kernel and passed enforcement, then exposed the tmpfs
setup issues described above. Its corrected full namespace rerun passed.

VM57 final canonical drivers PASS without serial overrides: scoped57 static
and57 dynamic, capability19+19, prior map/mount/budget/ABI/i386/NNP/BPF/filter/
signal/fork/exec/thread/native-control/credential lifetime regressions, and
new ZFS/tmpfs ownership controls. Its tmpfs resource was created by the final
driver from an absent resource. Runtime hashes match staging; pool export0
and synced poweroff complete acceptance. Evidence is
lab/evidence/boot-57-scoped-chown-acceptance.log. Builder provenance is
/build/RUNNING_TESTS.md. Runtime SHA256:

```
unix 438bbb5955f52e5ea8ae7c1b46873f135ceb945ca733858df50fb974e870e603
genunix bfdc3e37738b7cfce5c42afabc83b940da763f4685fb3b27ac37f82b0e059680
lx_brand 0fbaa2435e4a0139c9b04e05009a2a48206f09eed7a0d06447021fe8c279915a
zfs f53c8fdd0e4bbd5fc981aff158f85176adbc7c6b2860b57c5edd515a6cd2cfdc
namespace_ktest 90883253561790711000948d84ff5a639dae0f323ad08e5624b22f06b8614d17
```

Native tl600d5200ad, LOFSa8b, VM30 emulation libraries and VM46 support remain
retained over the mixed May base. Debug is compile-only, not runtime tested.
Actual Codex0.166 version passes under the scoped filter; the normal sandbox
is still unavailable. Launcher lx-sandbox/launch-lab remains SHA256
ece2f8445e5745dd320cc1f837a51ca7dc206f9798292dfdb41f71ddffc16739.
The isolated launcher arguments and physical hosts remain unchanged.

## Remaining work

Forced ownership-change races, nontrivial native ACLs and additional
filesystem behavior require more controls before public namespace entry.
CAP_FOWNER/DAC permissions, separate FSUID/FSGID, mount/PID/IPC/network
isolation, remaining syscall behavior and public entry remain incomplete.
On-disk file capabilities remain unsupported. A newest stable installation
and an actual normal sandboxed Codex coding workflow and complete platform
validation remain required before host deployment.
