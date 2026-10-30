# Scoped filesystem attribute permissions

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

The 57-case identity probes pass on Linux as static and dynamic binaries with
-Wall -Wextra -Werror. Five added cases check ownership changes on regular
files and tmpfs, clearing executable setid bits, mapped target validation,
effective versus permitted capability, ordinary group rights, rejection of
unmapped inode owners/groups and thread-local versus cached-file authority.
The initial new assertions incorrectly treated owner_is as an error-code
return; those assertions were corrected before the successful controls.

The native VM driver configures a private 16 MiB tmpfs through zonecfg,
passes its path to probes and lets zone lifecycle manage mounting/unmounting.
This zoneadm verifies special paths; /dev/null supplies a valid path, while
tmpfs ignores it and reports its resource as swap. Option values are quoted
for the native zonecfg lexer. It never
mounts or changes production hosts. Native kernel, brand, procfs, ELF, ZFS and
namespace tests compile cleanly in release/debug configurations. Common and
module source is af19067878; final driver is 10b6f7b3fb. VM55 failed before
kernel entry because the external ISO unix319c differed from archive unix438b.
The canonical updater now exports the matching unix plus SHA256; local helper
lx-sandbox/refresh-lab-image downloads/verifies the pair and rebuilds the ISO.
VM56 booted the same kernel and passed enforcement, then exposed the tmpfs
setup issues described above. Its corrected full namespace rerun passed.

VM57 final canonical drivers PASS without serial overrides: scoped57 static
and57 dynamic, capability 19+19, prior map/mount/budget/ABI/i386/NNP/BPF/filter/
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
Actual Codex 0.166 version passes under the scoped filter; the normal sandbox
is still unavailable. Launcher lx-sandbox/launch-lab remains SHA256
ece2f8445e5745dd320cc1f837a51ca7dc206f9798292dfdb41f71ddffc16739.
The isolated launcher arguments and physical hosts remain unchanged.

## Forced ownership races

VM59 runs the final race driver from the rebuilt archive without console
changes. A native global-zone helper alternates a private probe inode between
mapped ownership 1001:1001 and unmapped ownership 2000:2000. DTrace delays a
successful early native policy return, outside the ZFS update locks, and
requires an observed locked check against the unmapped owner. It also verifies
that zfs_setattr returns EPERM for that same operation. Either an unexpected
return or any DTrace diagnostic fails the driver. The test uses existing FBT
probes and adds no production kernel instrumentation.

Both canonical drivers pass. The static probe observed nine stale denials and
the dynamic probe observed eight. Each completed 1000 ownership attempts,
accepting only success or EPERM. The regular 57+57 identity, 19+19 capability
and earlier regression suites also pass. Credential lifetime checks pass after
both probes exit. Runtime kernel hashes remain the VM57 values above. The
native helper passes cstyle and GCC -Wall -Wextra -Werror. Test source is
b08568fa5e; kernel source remains af19067878.

During VM58 diagnostic development, the driver targeted genunix instead of
unix for the brand helper, then inherited an ignored SIGINT in its background
tracer. The final driver targets unix and stops DTrace using SIGTERM. The
initial fixed delay also missed races on one run; the final delay uses the
early policy return and varies its length. VM58 is diagnostic evidence only.
Final evidence is lab/evidence/boot-59-chown-race-acceptance.log.

## Native ACL take-ownership path

VM60 adds a nontrivial native ZFS ACL with an explicit write_owner grant to
UID1000. A native process with UID/GID1000 and only the basic effective and
permitted privilege sets successfully takes ownership of the private inode.
That positive control proves the ACL works without a native ownership
privilege override.

The scoped static and dynamic probes each verify three cases through that
same ACL: an unmapped owner remains protected with full scoped CAP_CHOWN;
a mapped owner can be changed with effective CAP_CHOWN; and removing the
effective capability prevents the ownership change. DTrace requires zero
calls to the generic setattr policy in all three operations and verifies
both the mandatory brand result and the final ZFS return. The probes also
verify the resulting file ownership. Existing FBT probes provide all tracing;
there are no new production kernel hooks or instrumentation.

VM60 final canonical drivers PASS without serial overrides, including the
new ACL cases, forced races, 57+57 identity and 19+19 capability cases, prior
regressions and credential lifetime cleanup. Native control helper cstyle and
GCC -Wall -Wextra -Werror PASS, as do Linux57 static/dynamic controls. Test
source is fb7749d1ef; kernel source remains af19067878 with the runtime hashes
above. Evidence is lab/evidence/boot-60-chown-acl-acceptance.log. The mixed
platform and debug-runtime limitations above still apply.

## Scoped file modes

The existing brand credential callback also handles explicit mode requests.
Its requested vattr argument is mutable so it can normalize S_ISGID; the
implementation changes no other requested mode bits. There is no additional
brand_ops pointer and no credential, LWP, zone or inode structure growth.
Native effective/permitted privilege sets stay at the existing basic floor.
Unbranded callers continue through native policy.

A scoped caller may chmod an inode it owns, or use effective CAP_FOWNER when
the inode UID is mapped. The GID need not be mapped for CAP_FOWNER. Keeping
S_ISGID additionally requires group membership, or effective CAP_FSETID with
both inode UID and GID mapped. Linux setuid/sticky rules apply to non-root
backing owners. Native UID0 setuid files retain the illumos requirement for
full native zone authority via secpolicy_vnode_setid_retain(root), for both
an owner and a caller using FOWNER. A scoped capability cannot bypass that
native privilege gate. A namespace-visible UID0 backed by a nonzero native
UID is not a native-root file.

Tmpfs checks and publishes modes under its existing node lock. ZFS checks
both its generic native-policy path and its explicit native ACL fast path,
then repeats authorization under the existing ACL writer lock and znode lock.
It records the original attribute mask so chown's internal setid clearing
does not acquire a new FOWNER requirement. At the final check, an explicit
chmod's original setgid request is reevaluated against the current GID.
Only S_ISGID is reconciled in the prepared mode; these special bits are not
encoded in the prepared ACL. Existing transaction cleanup and commit-on-late-
denial behavior are retained.

OpenZFS on Linux invokes Linux setattr_prepare through zpl_setattr_prepare
before its ZFS update. Linux chmod/chown take the inode lock around the
filesystem update. The illumos integration uses its existing native policy
and locking mechanisms instead. References:
https://github.com/openzfs/zfs/blob/master/module/os/linux/zfs/zpl_inode.c
https://github.com/openzfs/zfs/blob/master/include/os/linux/kernel/linux/vfs_compat.h
https://github.com/torvalds/linux/blob/master/fs/attr.c
https://github.com/torvalds/linux/blob/master/fs/open.c

No namespace state is stored in a pool. Ownership and modes use their existing
backing UID/GID and mode fields. No pool feature, filesystem format or pool
upgrade is introduced; existing feature-flag interoperability limits remain.

### Mode acceptance

VM62 final canonical drivers PASS without serial overrides: identity 63+63,
capability 19+19, prior ABI/i386/NNP/BPF/filter/signal/fork/exec/thread/native-
control/map/mount/budget/lifetime checks, existing ownership ACL/race checks,
and all new mode controls. Linux 63 static/dynamic controls pass with GCC
-Wall -Wextra -Werror. The six added cases cover ZFS/tmpfs setid/sticky modes,
effective capability, ordinary owner/group rights, mapped UID versus mapped
GID requirements, task versus cached-file authority and native-root setid
restrictions. The native-root case intentionally expects Linux to allow its
namespace capability and lx to preserve the stricter native privilege gate.

Native UID/GID1000 with basic E/P and an explicit write_acl ACE can chmod the
private inode. Scoped static/dynamic probes each reject an unmapped owner,
allow the mapped owner with FOWNER and reject the same change without effective
FOWNER. DTrace verifies these use the native ACL path with zero generic policy
calls. Forced owner changes produce eight locked chmod denials per variant,
and ZFS returns EPERM for each. Forced group changes exercise both directions:
static clears 4/restores 4, dynamic clears 5/restores 6. The trace checks both the
normalized request and actual znode mode. There are no tracer diagnostics or
violation markers. Each race variant completes 1000 bounded attempts.

The first new Linux control reused an inode changed by the ownership suite;
it now has an independent initial fixture. Initial a2f98e1dd2 compilation
found a missing sys/stat.h include in lx_userns.c; 89c4ea1aa5 fixes it. Clean
release/debug common/unix source is a2f98e1dd2; clean ZFS/proc/elf/ktest is
89c4ea1aa5 with identical underlying changes. Brand and final probe source is
91e026e659. VM61 passed 62+62 and the new ACL/races, but review identified the
missing native setuid-root gate before acceptance or physical deployment.
The guard and owner/FOWNER regression pass in VM62. Native helper cstyle and
GCC Werror pass. Evidence is lab/evidence/boot-62-mode-acceptance.log;
VM61 is pre-acceptance evidence only. Runtime SHA256:

```
unix 0e53c529302c573d3e11ffa1481234de4c5325d5487b646d871335751ec9ff4c
genunix 34c0478a0c08f47a7de439c7a86bc84b29475bd4a1147e051adb336d22211335
lx_brand 378e06cf1d258b7f07864256640741047038111e38cdad240033785dc5b13000
zfs f9ee00c2a95d1c20325bc231e87611f276d94ae7e3ca357697b0bf4ba4d21a0a
namespace_ktest 391f7cf8edb51808a68d066a54bdf0b8c146b920f44c31b5a7fd6ca8e2cd539f
```

The mixed-platform, retained-component and compile-only debug limitations
above still apply. No physical host was changed. No full-platform build or
normal sandboxed Codex workflow is claimed.

## Remaining work

Additional filesystem behavior still requires controls before public
namespace entry. Native ACL take-ownership has the controls above; other
ACL operations are not covered by that acceptance.
Other CAP_FOWNER operations and DAC permissions, separate FSUID/FSGID,
mount/PID/IPC/network
isolation, remaining syscall behavior and public entry remain incomplete.
On-disk file capabilities remain unsupported. A newest stable installation
and an actual normal sandboxed Codex coding workflow and complete platform
validation remain required before host deployment.
