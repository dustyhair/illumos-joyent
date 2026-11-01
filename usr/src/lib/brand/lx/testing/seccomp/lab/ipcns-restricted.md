# Restricted IPC namespace checkpoint

This checkpoint adds an experimental, default-off `CLONE_NEWIPC` entry path
for combined namespace creation and an `unshare(CLONE_NEWIPC)` path for the
lx brand.  A process that enters the namespace cannot access the zone's
existing System V IPC objects.  An IPC-only `clone()` is rejected with
`EOPNOTSUPP`; this avoids routing an otherwise unrestricted lx process onto
the guarded kernel clone path.  Bubblewrap requests IPC together with user,
mount, and PID namespaces, which is the supported clone shape.

The namespace is intentionally restricted rather than complete.  System V
shared-memory, semaphore, and message-queue syscalls return `ENOSYS` inside
it, including the 32-bit `ipc(2)` multiplexer.  Private IPC object creation
is therefore unavailable.  POSIX message queues remain unsupported by the
brand.  This is sufficient for bubblewrap's namespace construction while
preventing access to the surrounding zone's System V IPC namespace.

The `lx_ipcns_experimental` kernel switch must be set to `1` to permit entry.
It defaults to zero and must remain disabled outside an explicitly selected
deployment profile.

`ipcns-restricted-test.sh` verifies the default-off gate, the safe IPC-only
clone rejection, isolation from an outer shared-memory object, denial of new
SysV IPC objects, parent-view preservation, `unshare(2)`, and inheritance
across `fork(2)`.  The combined clone shape is exercised by the real
bubblewrap/Codex acceptance test.

Network namespaces are not part of this checkpoint.  The companion Codex lx
launcher uses an integrity-checked bubblewrap wrapper that removes exactly
one `--unshare-net` option only when `CODEX_LX_SHARED_NETWORK=1` is set.  The
deployment therefore has shared networking and must not be described as
network isolated.
