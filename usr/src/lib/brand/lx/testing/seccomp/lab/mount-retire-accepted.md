VM106 accepts synchronous mount namespace retirement and the private LOFS
root boundary fix. Source and artifact hashes are in mount-retire-results.json.

When the final view reference is released, all old mount graph edges are
removed before that release returns. Independently held directories and
files retain access to their own filesystem, including writes shared with the
new copy. Real native unmount remains deferred until those references close.
The current root becomes a retained record in reverse constructor order so a
newer bind root can release its backing source before the older mount retires.
The public view_destroy busy-retry contract remains intact.

VM105 exposed a native LOFS dotdot assumption during getcwd. Native reverse
lookup invokes VOP_LOOKUP directly, bypassing the common pathname boundary.
Private unspliced LOFS roots now check execute access and return themselves for
dotdot. Backing-root ascent pins each covered parent under the VFS read lock
and checks for a missing parent before use. A common return helper releases the
last parent reference. The corrected clean release/debug LOFS build passes.

Four Linux retirement controls, four lx retirement cases and four extra lx
smoke cases pass for static/dynamic binaries and root/nobody. They verify old
child graph removal, held files and child filesystems, self-parent roots,
ENOENT for old cwd, exact cwd in the active copied bind, another namespace
copy, and persistent seccomp filtering. The full earlier regression suite also
passes. Native reinsert tests pass six times with four cycles each.

The first smoke attempt hit the user-namespace idle deadline. DTrace proves
that the last reference was the native dead thread's t_cred. Native thread
reaping is batched and wakes when queued exits exceed reaplimit, default 16.
One credential survived 48 seconds on this quiet VM after its mount views were
already gone. The retirement test now generates 32 native exits to wake that
existing reaper before the original zero-charge assertion and 10-second bound.
No production workaround, accounting exception or reaper tunable was added.
Initial failed logs and the release stack trace remain in the evidence.

Every final retirement case passes both idle checks before zone halt. All
experimental switches and private views are zero, the zone is stopped, pool
export returns zero, filesystems sync and QEMU exits. Runtime uses rebuilt
release lx_brand and LOFS with retained platform modules; debug builds are
compile-only. No physical host or ZFS pool feature changed.

This is preparation for pivot_root. The lx syscall is still unimplemented.
Eight expanded Linux root-switch controls pass the two-pivot bubblewrap
sequence and validation failures that must leave root/cwd intact. Actual
Codex's combined namespace clone still fails EINVAL, last traced in VM100.
PID/IPC/network namespaces, combined creation, private proc/devpts and full
Codex coding/PTY acceptance remain.
