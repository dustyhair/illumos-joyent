# PID1 cohort signal fanout

VM119 validates signaling committed namespace members when their internal
PID1 exits. It also repeats all 24 process/LWP ownership cases accepted in
VM118. Linux PID values remain global and `CLONE_NEWPID` is still rejected.
This is not complete PID namespace teardown or bubblewrap acceptance.

Kernel and KTEST source are recorded in `pid-cohort-results.json`. The new
production path walks the existing PID map AVL under native `pidlock`, marks
the namespace dead before the walk, and uses native `sigtoproc(SIGKILL)` after
matching the held map to the live process under `p_lock`. No process list,
worker thread, taskq, generic brand callback, or native ABI was added.
Uncommitted children still use the existing commit/death gate; racing native
construction needs further integration tests.

Eight targeted cases cover static/dynamic, root/nobody, and ordinary/userns
guarded credentials. Each creates a child and grandchild in separate sessions
with no parent-death signal, plus an outside stopped peer. KTEST validates the
map memberships and absence of parent-death signals. Killing only the mapped
PID1 must kill its two descendants, preserve the outside peer, gate later PID
reservations, and release every map and budget charge after native reap.
Native FBT exit probes require CLD_KILLED/SIGKILL for the three mapped processes
and the peer's later explicit cleanup. Runner exit status is not evidence of
the terminating signal: native noninteractive zlogin returns WEXITSTATUS
without first checking WIFSIGNALED.

Initial harness failures are retained. The networkless fixture's native
lxinit was waiting for routeinfo and never reached its Linux reaper. The
cohort controller uses the existing zone-specific `init-name=/sbin/init`
attribute to run the test root's Linux init directly. DTrace uses the same
`-x nolibs` mode and handled SIGTERM shutdown as the earlier lab controllers.
These changes affect only the offline test setup. Initial cleanup failures
were not counted as passes.

The accepted targeted run uses an uploaded, hash-verified controller from the
recorded final source. The original 52 artifact hashes, controller replacement
hashes, initial failures, positive rerun, and raw console hashes are retained.
The boot image on the builder was updated with that controller. Production
modules did not change during VM119. The previous full filesystem, namespace
clone, PID identity, and map regression run passed in VM118; it was not
repeated in VM119. This change only adds the private mapped-PID1 exit branch.
VM119 repeats the affected 24 ownership cases and the eight new cohort cases.

Clean release/debug lx_brand and namespace KTEST builds passed. Runtime uses
release modules with retained VM118 lx_proc/lx_cgroup and VM114 genunix and
surrounding platform/LOFS/tmpfs. Debug was compile-only. Acceptance requires
flags/views/PID namespaces zero, zone stopped, healthy pool, export zero,
filesystem sync, and QEMU exit. No physical host was deployed.

Remaining PID1 work includes synchronized death/reap ordering, adoption of
orphans by a live inner namespace init, and Linux init signal rules. Public
PID namespace entry and coherent signal/wait/proc/group/session IDs remain
unfinished. IPC/network namespaces and a fresh current Codex installation and
complete sandboxed coding/PTY workflow also remain unaccepted.
