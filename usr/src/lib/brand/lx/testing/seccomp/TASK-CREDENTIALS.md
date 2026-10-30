# Scoped task credential checkpoint

Work is on the private `lx/codex-sandbox` branch of illumos-joyent. It is
experimental and has not been deployed to a production SmartOS host.
Public namespace entry remains disabled. Normal Codex sandbox operation
has not passed acceptance.

The brand reuses `kthread_t.t_cred` for immutable task credentials. The process
credential contains the union of the live tasks' Linux capability sets for
process-wide access checks. Scoped capability changes replace only the calling
task's credential. Creation copies the creator's credential before the child
runs. Exit removes the departing task's capabilities from the process union.
The native privilege sets remain limited to the existing basic privilege floor.

The kernel-entry guard also applies to scoped processes without a seccomp
filter. Unsupported native and usermode-emulation transitions terminate the
process. Scoped exec and UID/GID mutations currently return EOPNOTSUPP while
their task-specific credential transitions are unfinished.

## Evidence before the trap refresh fix

Clean release and debug builds of lx_brand, lx_proc, and lx_namespace_ktest
passed. The static Linux control built with GCC warnings as errors and passed
all five scenarios in a disposable bwrap user/PID namespace on codex-testbox.

Disposable x86 SmartOS VM31 passed the eight existing user-namespace backend
suites, 11 capability ABI scenarios both ordinary and filtered, raw i386
checks, mount views and snapshots, and previous NNP/BPF/filter/signal/fork/exec/
thread regressions. Codex 0.166.0 printed its version under the filter.
Six of seven new live scoped scenarios passed. The combined thread/fork case
failed. The same failure reproduced with diagnostic static binaries in VM32
and VM33. VM33 narrowed it to a forked child's effective capability set being
nonzero after the creating thread had dropped its capabilities. Weak-caller
pthread creation and the other scoped scenarios passed.

Inspection found that common user trap and DTrace entry replace t_cred with
p_cred. A page fault can therefore restore the aggregate capability set to a
scoped task. The current fix preserves brand-owned task credentials at common
credential refresh points. The regression now forces a private first-write
page fault after dropping capabilities. This fix still needs its clean kernel
build and disposable-VM acceptance. SPARC refresh sites follow the same
contract, but only the x86 target is available for build/runtime validation.

VM31's large serial diagnostic upload overflowed terminal input. VM32's
separate dynamic diagnostic hung and zone halt did not finish. Neither VM is
claimed to have exported its pool cleanly. The disposable QEMU processes were
stopped after unsuccessful orderly shutdown attempts. The static reproduction
remains the acceptance target; the dynamic failure is unresolved.

Runtime images reuse the previously validated common kernel/LOFS modules and
rebuilt VM30 lx libraries as appropriate for each run, over a May platform
base. They are mixed development images, not a complete platform rebuild.
The private launcher is `lx-sandbox/launch-lab`, SHA256
`ece2f8445e5745dd320cc1f837a51ca7dc206f9798292dfdb41f71ddffc16739`.
Build/run provenance is recorded on the builder in `/build/RUNNING_TESTS.md`.

## Remaining work

Validate the trap fix, per-task procfs and concurrent update behavior, and
scoped task lifecycle before enabling namespace entry. Complete scoped exec,
identity translation/transitions, capability enforcement and namespace lifetime
limits. Connect safe user/mount namespaces and complete the mount, PID, IPC,
and network isolation requested by Codex. Finally install the newest stable
Codex and exercise a normal sandboxed coding workflow with actual denied
filesystem/network/process operations. Production deployment follows VM
acceptance, not a successful version command.
