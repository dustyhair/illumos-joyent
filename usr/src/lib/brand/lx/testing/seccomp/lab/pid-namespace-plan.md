PID namespace integration after native PID lifetime acceptance

This is an implementation plan, not implemented namespace support. Keep
CLONE_NEWPID rejected until the syscall and lifecycle paths agree.

Use the existing global lx task IDs as backing identities. A namespace
mapping holds the native struct pid through exit and reap; it must never
make a retired synthetic ID visible as a native process. Use illumos
id_space_t for namespace-local numbers rather than another global PID
allocator. Ancestor-visible numbers and the active namespace of a process
remain fixed throughout its lifetime. unshare(CLONE_NEWPID) changes only
the namespace used for future children.

Reserve map storage and IDs before a child can run. lx_lwpdata_alloc is
outside p_lock and already reserves synthetic thread IDs. lx_initlwp and
lx_pid_assign run under p_lock, so they cannot acquire pidlock or perform
sleeping map preparation. lx_copy_procdata runs in getproc and copies the
parent's process state. lx_kernel_fork_child runs before the new SIDL child
can run and already commits prepared user/mount state. Audit failure
cleanup and nonleader exec alongside the successful fork path. Preserve
native p_pid and process relationships; translate at the Linux boundary.

The process exit hook runs before the last LWP's free hook. The native
exit code explicitly keeps process brand data through the zombie/reap
interval. Keep process PID mappings for wait and proc reporting through
that interval. Retire synthetic worker mappings when their LWP disappears.
The current lx_clearbrand hook is distinct from lx_free_brand_data, which
frees zone state. Audit actual brand_clearbrand callers and lock ownership
before attaching PID map releases there.

Native orphan adoption is in common/os/exit.c under pidlock and currently
selects the zone init. PID namespaces need an explicit brand-aware reaper
selection here, not a second wait/reap engine. Do not set zone_proc_initpid
or SZONETOP for inner PID1; these have outer-zone shutdown semantics.
Namespace init exit must reject future forks and terminate its remaining
processes, including nested namespaces. The namespace PID maps already
identify those processes; avoid adding a parallel process registry.

Update getpid/gettid/getppid, clone return and TID stores, kill/tkill/tgkill,
wait results and SIGCHLD/siginfo together. Process-group/session IDs and
PID1 signal handling also need namespace semantics. lx_getpid.c and
lx_kill.c still special-case only zone_proc_initpid.

A proc mount must retain the PID namespace selected at mount time. Path
lookup and enumeration must use that mount's namespace, rather than the
current caller's namespace. The current zone-wide lx_proc mount cannot be
silently reused as an inner private proc mount. Check task directories,
self/thread-self, status/stat, parent IDs, held proc descriptors, hidden
siblings, zombie reporting and teardown against Linux.

Before enabling the flag, test child PID1/parent PID0, siblings and nested
views, pthread creation, leader exit, nonleader exec, orphan adoption,
wait/signal IDs, init death, fork failure and repeated teardown. Repeat the
actual Codex bubblewrap request after this; NEWIPC and NEWNET remain
separate required work.
