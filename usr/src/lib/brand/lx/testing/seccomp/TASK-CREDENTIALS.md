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
process. At VM34, scoped exec and UID/GID mutations returned EOPNOTSUPP. Ordinary
scoped exec is now implemented and validated in [SCOPED-EXEC.md](SCOPED-EXEC.md).
Scoped UID/GID and supplementary-group transitions are now implemented for
AMD64 and validated in [SCOPED-IDS.md](SCOPED-IDS.md). The earlier VM34 results
below describe the restrictions at that checkpoint.

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
page fault after dropping capabilities. VM34 validates this fix as described below. SPARC refresh sites follow the
same contract, but only the x86 target is available for build/runtime validation.

VM31's large serial diagnostic upload overflowed terminal input. VM32's
separate dynamic diagnostic hung and zone halt did not finish. Neither VM is
claimed to have exported its pool cleanly. The disposable QEMU processes were
stopped after unsuccessful orderly shutdown attempts. VM34 passes both the static and dynamic reproductions after the common
credential-refresh fix.

Runtime images reuse the previously validated common kernel/LOFS modules and
rebuilt VM30 lx libraries as appropriate for each run, over a May platform
base. They are mixed development images, not a complete platform rebuild.
The private launcher is `lx-sandbox/launch-lab`, SHA256
`ece2f8445e5745dd320cc1f837a51ca7dc206f9798292dfdb41f71ddffc16739`.
Build/run provenance is recorded on the builder in `/build/RUNNING_TESTS.md`.

## VM34 acceptance

Kernel source commit `db416b741e4aebef1de828d635ce21c5c7a33915` passed clean
release/debug unix, genunix, lx_brand, lx_proc, elfexec and namespace ktest
builds. Linux controls pass all five common cases for static and dynamic
binaries, with the forced first-write fault included.

VM34 passes all seven live scoped cases with both binaries. These cover atomic
E/P/I capability updates, irreversible bounding drops and permission ordering,
securebits locks, ambient capabilities, preservation across user page faults,
task-local drops, remote task capget, weak-caller thread creation and fork from
a multithreaded process, native-entry denial without a seccomp filter, and
honest EOPNOTSUPP for unfinished scoped exec and ID changes. The prior eight
backend suites, three map-file baseline scenarios, 11 capability ABI cases
ordinary and filtered, raw i386 checks, mount views/snapshots and previous
17 NNP/67179 BPF/29 filter/31 signal/15 fork/7 exec/5 thread regressions pass.
Credentials repeat after each zone halt, including the dynamic test, passes.
Runtime module/library hashes match their staged artifacts. The final zone
halt, pool export and synced poweroff complete normally.

The dynamic check used a separate small driver after the normal userns driver,
with the dynamic binary uploaded compressed and its digest checked. The
committed `lab/scoped-dynamic-test.sh` records that bounded-wait procedure with
the same standard private-lab guards as the other drivers. Build the dynamic
binary from `scope_caps.c` without `-static`, gzip it to `/lab/scope.gz` inside
the disposable VM, then run the helper after the normal userns test driver.
The binary must use the loader/libc installed in the lab root. All tests here
used Ubuntu glibc 2.39. No Codex authentication or network workflow was tested.

Runtime SHA256:

```
unix       3c7e9f420f092298322d3816fc2cef29d160b6ba71d733ad1799fb066191a6d4
genunix    aa6b8f2ea9f4e9b005e19342bf83c495d463d824cf73e7f10768236f3a33d5cf
lx_brand   aa94109e158f7a063adf7804015b2a173dcf5a0397e7f3539ad0d0628986c00d
lx_proc    3dd1ec24548ee0bd80dbb178214e39ccb14ecfbf50c5d3ea2ff8a99713e20d2f
ktest      0e3c907c52196ba464e12a70835826d601e060f34ba074c2d8d9b92e417b2dc3
lib32      2bfe10d818552080ae4bd231888d916e4daee297acf40942e3b65d6c9523d38a
lib64      20cdf12237041cc84cead937066816fe33ff0bec7d7e960a89445c45d0bc70c0
scope      fe9dda4d2c4d8b9732cb4ad511af3cf8361614b993335cca3d8abceb4403708d
dynamic    f7322fb315a610aa892e0c43e7390704026ab100ee5e05699ba64e079f2809ea
```

Local console evidence is `lab/evidence/boot-34-scoped-task-credentials.log`.
These remain release-runtime results on a mixed platform image; debug modules
were compiled. They do not prove full Linux namespace or sandbox semantics.

## Remaining work

Validate per-task procfs and concurrent update behavior and complete scoped
task lifecycle before enabling namespace entry. Complete traced/privileged exec,
identity translation/transitions, capability enforcement and namespace lifetime
limits. Connect safe user/mount namespaces and complete the mount, PID, IPC,
and network isolation requested by Codex. Finally install the newest stable
Codex and exercise a normal sandboxed coding workflow with actual denied
filesystem/network/process operations. Production deployment follows VM
acceptance, not a successful version command.
