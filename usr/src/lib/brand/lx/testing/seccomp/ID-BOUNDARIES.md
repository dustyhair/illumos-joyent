# Scoped ownership and peer identity translation

Scoped chown/fchown/lchown/fchownat arguments now translate through the calling
task's user-namespace maps before native UID/GID validation and VOP_SETATTR.
The unchanged-ID sentinel stays unchanged. Unmapped arguments return EINVAL.
Path/descriptor lookup still occurs first, preserving errors such as ENOENT
and EBADF. The existing wrappers retain their symlink, O_PATH and empty-path
behavior. Initial-namespace calls retain their existing numeric IDs.

SO_PEERCRED now translates the connection's cached backing UID/GID into the
reader's namespace. An unmapped identity returns overflow ID 65534. Values are
copied while holding the socket lock and translated after releasing it, so
namespace locking adds no socket-lock nesting. Reading does not refresh the
peer snapshot. The zone's native init PID is reported as Linux PID one.
This retains the existing zone-level PID model, not Linux PID namespaces.

The change adds one symmetric ID-map helper and no structure fields. It does
not grant native privileges or replace native filesystem permission checks.
In particular, Linux CAP_CHOWN authority for arbitrary mapped ownership
changes remains unfinished. The new ownership controls cover native owner and
supplementary-group permissions with CAP_CHOWN dropped, plus rejection for an
unmapped inode while scoped capabilities remain full. Passing those tests
must not be described as complete namespace capability enforcement.

Future capability authorization belongs in the filesystem policy path using
the inode identity for that operation. Checking a vnode's owner first and then
passing a broadly privileged credential to setattr would race ownership
changes. This checkpoint makes no such privilege substitution.

## Validation

Implementation starts at 26c062fb6e. Final clean release/debug lx_brand source
is 5cb2ef22fb, with probe/driver source e2ee2802d5. The initial build referenced
a ptrace-private PID macro; the final code uses the zone-init check directly.
Changed kernel files pass cstyle and git diff --check.

Static and dynamic Linux controls each pass all 42 scoped scenarios and the
separate inherited-tracer detach control. The 11 new cases cover:

- Socket credentials captured before namespace entry, including unmapped IDs.
- Newly mapped peer identities and snapshots retained after ID changes.
- A departed worker's effective IDs versus the main task's process identity.
- Real Unix connect/accept credentials in both directions.
- Same-owner/group-member chown with capabilities dropped.
- Paths, symlinks, empty paths and O_PATH descriptors.
- Unmapped-ID errors and descriptor/path error precedence.
- Native permission denials, unmapped inode ownership and task-local checks.

The probe prepares sockets and an unlinked file with native UID/GID 2000
before entering the disposable namespace. That identity is outside its map.
It restores real/effective/saved IDs to zero before the normal fixture starts.
The LX trusted fixture maps visible 0..2 to backing 1000..1002 and visible 42
to backing zero. Linux uses the same map via its root-created child namespace.
No host policy or sysctl changes are needed. All credentials and files here
belong to the disposable test process or private VM.

VM44 passes both full drivers. Each static/dynamic variant passes all 42
scoped cases, the inherited-tracer detach control, 19 capability/exec cases
and the seeded SID check. The post-zonehalt credential lifetime check passes.
All earlier backend/mount, map-file, capability ABI/i386 and enforcement,
signal/fork/exec/thread regressions also pass. Codex 0.166.0 prints its version
under the scoped zero-capability filter twice, plus once in the earlier exec
regression because this run retains the installed test fixture. A version
command is not a normal sandboxed coding workflow test.

Runtime readback matches staged module/probe hashes. The pool exports and
normal poweroff completes with filesystems synced. Local console evidence is
lab/evidence/boot-44-ownership-peer-identities.log. Build/run provenance is
/build/RUNNING_TESTS.md on the builder.

Relevant SHA256:

```
lx_brand   488e5ae099d45ed5acdd2604fbf2c77b060c953c295e92a072189ae6a056fca9
ids        b76d6f444d8dd0c3e18eb377da21384e431e696b0c205d5fe29e6a961b8bd3c7
ids_dyn    b7e9c8c8d9adbae2a232547d8886f528eb43591564f27e8cbe848a2bdf37cc13
```

The runtime retains common kernel2676283bce, proc65b024e439,
ktestdbc2210580, LOFSa8b and the VM30 rebuilt lx libraries over the mixed May
platform base. Debug was compiled, not runtime tested. SPARC and the full
platform were not rebuilt. No LWP offsets or user-emulation interfaces change.
The launcher is lx-sandbox/launch-lab, SHA256
ece2f8445e5745dd320cc1f837a51ca7dc206f9798292dfdb41f71ddffc16739.
Production hosts and the unrelated GPU worktree remain unchanged.

## Remaining work

Complete resource-scoped CAP_CHOWN/FOWNER/DAC and other permission checks,
SCM_CREDENTIALS input/output translation and validation, filesystem UID/GID
splits, remaining UID-bearing interfaces, and namespace resource/lifetime
limits before public namespace entry. Mount, PID, IPC and network isolation
still need implementation and acceptance. The known non-native-child ptrace
exit-routing and ignored-signal wait-restart gaps remain documented in
TRACED-EXEC.md. No normal sandboxed Codex coding workflow or production host
deployment is claimed here.
