VM111 accepts amd64 clone entry for private user and mount namespaces.
The implementation prepares credentials and the filesystem view before
native forksys can create a runnable child. The native fork hook exchanges
the child's inherited references for the prepared root/cwd and namespace
credential. Parent cleanup releases the returned old references, or a failed
preparation, outside process locks. The parent retains its own identity,
root/cwd, mount view and dispatch policy.

First mount-only entry activates the child's native-entry guard only from
a pending kernel-owned state during a fresh SIDL amd64 clone. Namespace
ownership, capabilities and budgets reuse the existing lx backends. Shared
address spaces, CLONE_FS, ptraced parents, multithreaded entry and unsupported
namespace flags retain explicit errors. Direct kernel fork also clears stale
tracing requests from the older user-emulation path. No common native fork
or VFS implementation change was needed for this milestone.

All 80 cases pass: 56 enabled combinations and eight each with mount entry
disabled, mount quota zero and user quota zero. These include 36 successful
namespace children and 44 expected rejections. Both idle assertions pass
after every case. Static/dynamic binaries, raw/libc clone entry, root/nobody,
first mount-only entry, combined/nested views, capability denial, invalid
shared-FS flags, private child mounts, parent identity/cwd isolation and
inherited seccomp filtering are covered. All 56 Linux reference cases pass
under the existing lxc-unshare profile without changing global policy.

The complete earlier mount/root-switch regression suite and the additional
readonly/shared-root controls pass, as do four native reinsert tests. VM110's
initial driver stopped before clone because native Bash rejected an empty
array under nounset; the corrected string-based driver passed all 56 enabled
cases. VM110 evidence is preserved separately. VM111 uses the rebuilt kernel
with the tracing-state cleanup and the corrected driver.

Codex 0.166.0 is still the stable npm release and reports its version in lx.
Its actual sandbox request still fails twice with EINVAL for flags0x78020011.
The unsupported bits are PID, IPC and network namespace creation,0x68000000.
Bubblewrap's generic user-namespace error is misleading for this request;
the user/mount subset now passes. Full Codex sandbox acceptance is not claimed.

Final switches and private views are zero, the zone is stopped, pool export
returns zero, filesystems sync and QEMU exits. Exact source, image, controller
and evidence hashes are in namespace-clone-results.json. Clean release/debug
lx_brand builds pass; runtime uses release binaries with retained platform
modules. Whole-platform acceptance is not claimed. No physical host was
changed. PID/IPC/network namespaces, private proc/devpts and full coding/PTY
acceptance remain.
