# Native process and LWP PID map ownership

VM118 validates internal PID map ownership through native process creation,
thread exit, nonleader exec, zombie state, wait, and reap. Linux PID values
remain global. `CLONE_NEWPID` is still rejected. This is not acceptance of
bubblewrap or a complete Codex workflow.

The kernel source is `da82a5b27be4ffcccbc8d5534e2bb164f6b9ecb2`, the fixture
and KTEST source is `65a2759bffbb07aa47f50fb2b19017b93e65b5d2`, and the
controller source is `b6fbab28e73af20aca746f8602b554ab71a58dc2`. Exact artifacts, source hashes, case counts,
and evidence hashes are in `pid-process-results.json`.

The implementation uses the existing native fork success callback, speculative
LWP allocation and unwind, brand exec and exit callbacks, and native reap.
It introduces no taskq, worker thread, second process list, or replacement
wait engine. A process retains its PID map until native reap. Worker maps
retire at thread exit or exec, while explicit observers can retain their
native PID identities. PID 1 exit gates later reservations. Cohort termination
and orphan reaping remain unfinished.

The 24 Linux reference cases and both 24-case LX smoke and full runs cover
static and dynamic binaries, root and nobody, ordinary and user-namespace
guarded credentials, and exit, exec, and fork. The global-zone test module
attaches private bookkeeping only to a validated, stopped, single-threaded
fixture in the offline `lx-probe` zone. This trusted test attachment is not
a Linux namespace entry interface.

Each LX run checks 128 PID lifecycle assertions and 48 mount/user namespace
idle assertions. The fork cases inspect a stopped child, retain its map as a
native zombie, then inspect its retired identity after the parent waits.
The thread cases inspect the worker map and its retirement after join or
nonleader exec. Dropping the final observer releases the retired native PID.
Owner reap must return the PID namespace count and budget charge to zero.

The full run also repeats the earlier user, mount, combined clone, native PID
identity, and PID map suites. All experimental switches, private views, and
PID namespaces must be zero at completion. Acceptance requires the zone
stopped, healthy disposable pool, export status zero, filesystem sync, and
QEMU exit. The unmodified raw console is retained as
`boot-118-pid-process-ownership-acceptance.log` locally and on the builder.

Clean release and debug builds cover `lx_brand`, `lx_proc`, and `lx_cgroup`
at the kernel commit, plus `lx_namespace_ktest` at the test commit. The
runtime uses release modules with retained VM114 genunix and surrounding
platform, LOFS, and tmpfs. Debug was compile-only. Whole-platform acceptance
and physical deployment are not claimed.

The next work is coherent Linux PID namespace behavior, including PID 1,
signals, wait results, proc, process groups, and sessions. IPC and network
namespaces and current Codex installation and complete sandboxed coding/PTY
workflows still need validation. No physical host was changed.
