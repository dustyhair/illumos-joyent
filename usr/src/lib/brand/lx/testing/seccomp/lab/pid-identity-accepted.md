VM114 accepts native PID identity lifetime changes needed for PID namespaces.
CLONE_NEWPID remains rejected. This checkpoint does not make bubblewrap work.

lx_pid_identity_hold looks up an existing global lx task ID in the selected
zone and retains its native struct pid. It prevents ID reuse without keeping
the process or LWP alive or granting lookup authority. Thread exit and
nonleader exec remove the old lx hash entry and drop the owning native PID
reference with PID_RELE. They no longer free that object while mappings can
still hold references. Unpublished speculative allocations retain their
existing cleanup path.

Native pid_allocate now initializes non-process reservations as inactive
under pidlinklock, before another lookup can see them. This is one line in
the common allocator. It replaces four lx lines that set the flag after
publication. The only non-PID_ALLOC_PROC caller is lx; ordinary native fork
continues to allocate a process slot. Native lookups cannot interpret a
reserved LWP ID as proc slot zero.

VM112 found an existing nonleader exec bug: the lx PID hash was reassigned,
but gettid still returned the worker's retired ID. The existing reassignment
path now updates br_pid. VM113 then passed all 16 lifecycle cases. Source
review subsequently moved the inactive initialization into the allocator,
and VM114 reran the smoke tests and full regressions on the final kernel.
The VM112 failure and VM113 smoke evidence remain recorded separately.

VM114 passed 16 smoke and 16 full cases across static/dynamic binaries,
root/nobody, ordinary/userns-guarded processes and worker exit/nonleader
exec. These include 96 native hold/retire/release test calls, two direct
native reservation lookup checks and both idle checks after every case.
The 16 matching Linux controls passed under the existing lxc-unshare
profile without changing global policy.

All earlier regressions pass: 70 mount-entry, 12 bind, two mount-limit,
12 recursive, eight private, 12 tmpfs, four statfs, eight readonly,
eight detach, four retirement, eight pivot and two shared/readonly root
cases, plus all 80 namespace-clone cases. Those clone cases include
36 successful children and 44 expected rejections. Both idle assertions
pass after every clone case. Four native reinsert tests cover 16 cycles.

Clean release/debug genunix, lx_brand and namespace-ktest builds passed
without warnings or errors. Touched-source cstyle and git diff checks pass.
The VM runs release modules with the retained surrounding platform and
LOFS/tmpfs modules. Debug builds are compile-only; this is not a whole
platform acceptance result. The VM's 41 artifact hashes were checked before
execution. The zone stopped, experimental switches and mount views were
zero, the pool exported successfully, filesystems synced and QEMU exited.
No physical host or production zone was changed.

The newest npm stable version was rechecked as 0.166.0 at
2026-10-31T14:09:43.965368+00:00. Codex prints that version in lx.
Its actual bubblewrap clone request 0x78020011 still returns EINVAL twice.
NEWPID, NEWIPC and NEWNET are still unsupported. The generic user-namespace
error is misleading because separate user/mount namespace clone tests pass.
Fresh installation and the full sandboxed coding/PTY workflow remain unaccepted.

Kernel source: c2fd9c840f94112344ed347f635d327dbb8601ac
Runtime/test source: dee78b586837ed898172b9a3e213b4feae19bd8c
Console: boot-114-pid-identity-acceptance.log
Console SHA256: 42aa65c171942ad2ca99b94d00686b10e849af49a0195f1f986595f06adf0e48

pid-identity-results.json records artifacts, case counts, Linux evidence,
earlier diagnostic runs and cleanup. pid-namespace-plan.md records the next
integration points and required semantics; it describes work still to do.
