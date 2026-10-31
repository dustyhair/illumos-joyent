VM104 accepts native retained-mount reattachment ownership.

Reinserting a retained VFS preserves its existing zone reference and creation
timestamp. Reattachment clears VFS_DETACHED. Fresh publication still acquires
its first zone reference. The VFS private-state allocator initializes that
reference before it can be inspected. VM103 caught a boot regression when the
new decision read the kmem_alloc field before initialization; the fix moved
initialization to allocation. The corrected image boots normally.

Six native reinsert tests passed, each repeating four detach/reattach cycles
while holding a tmpfs file. They verify the same file vnode and exactly one
occurrence of the embedded ownership reference in the actual owner zone list.
The tests restore graph links before reporting assertions and verify cleanup.
The full earlier regression wrapper and all public mount cases also passed,
including eight detach cases and an additional eight-case smoke run.

Both idle checks passed before halt after each detach case. All switches and
retained views were zero, the zone was installed and stopped, pool export
returned zero, filesystems synced and QEMU exited. Source, artifact, controls
and console hashes are in mount-reinsert-results.json. Clean release/debug
genunix and namespace-test-module builds passed; runtime uses release modules
with retained platform components. No physical host or ZFS pool feature changed.

This is a root-switch prerequisite. lx pivot_root is not implemented yet.
Namespace retirement, current-root/source dependency cleanup, root/cwd handoff
and exact path rebasing still need work. Separate Linux controls verify the
two-pivot bubblewrap sequence. An additional Linux reference confirms that
leaving the last namespace member unlinks old child mount edges while held
file references remain usable. The npm stable tag was rechecked and remains
Codex0.166.0. Last actual combined clone trace remains VM100 EINVAL twice.
Private proc/devpts, remaining namespaces and full Codex coding acceptance
are still pending.
