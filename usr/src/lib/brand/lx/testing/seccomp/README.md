# lx seccomp enforcement probe

Build and run this test inside a Linux environment or an lx zone:

```sh
cc -std=c99 -Wall -Wextra -Werror -O2 seccomp_probe.c -o seccomp_probe
./seccomp_probe
```

The test installs a classic BPF filter that denies the raw `getppid` syscall
with `EACCES`, using both `prctl(PR_SET_SECCOMP)` and `seccomp(2)`. Each test
runs in a child process so its restrictions do not affect the runner. The
architecture check terminates a process if its syscall ABI does not match
the filter. The source supports x86-64 and i386; each ABI needs a separate
build and run.

Exit status:

- 0: all checks passed, including actual denial of the filtered syscall.
- 1: regression or incorrect reporting, including accepting an unenforced filter.
- 77: no regressions found, but filtering remains unsupported.

An unsupported installation is never reported as working sandbox support.
These checks cover only the first seccomp prerequisite. They do not certify
the entire sandbox or test namespaces, Landlock, filter inheritance, filter
stacking, thread synchronization, or escape paths.

The initial-mode check recognizes lx using `/native/usr/bin/uname` and expects
disabled mode there until real filtering exists. Update that expectation when
lx supports inherited filters. On Linux, inherited container filters are
allowed. The probe is intended for an unrestricted development shell; an
outer sandbox may independently prohibit installing filters or calling
`getppid`, which prevents these tests from establishing lx behavior.

## First kernel change

The existing lx `PR_SET_SECCOMP` implementation accepts a non-null filter
without applying it. `PR_GET_SECCOMP` always reports filter mode, even in a
fresh process. The accompanying change rejects unsupported filter mode with
`EINVAL`, preserves the existing null-pointer `EFAULT`, and reports mode 0.
The seccomp DTrace probe remains available.

This intentionally changes compatibility behavior. Software that previously
continued after a fictitious successful filter installation may now refuse
to start. It does not enable Codex's sandbox.

## No-new-privileges implementation and probes

The next change implements an irreversible per-LWP `no_new_privs` flag in
the common kernel, using an existing reserved field without changing the
structure layout. New LWPs inherit the creator's flag; fork copies the
specific parent's LWP state. Common exec retains the flag across Linux
and native programs, ignores set-id files and their forced privileges,
discards profile credentials while preserving profile access decisions,
and limits inheritable privilege promotion to the old permitted set.
`PR_SET_NO_NEW_PRIVS` and `PR_GET_NO_NEW_PRIVS` validate all five arguments.
The lx proc status files expose the target task's `NoNewPrivs` value.

An isolated x86-64 SmartOS VM passed all 17 no-new-privileges checks with
no skips. The test image contains the rebuilt release `genunix`, `lx_brand`
and `lx_proc` modules over an existing platform image. This is a mixed-image
development result, not a clean full-platform validation. See
[the lab notes](lab/README.md) for the boot method and acceptance details.
It is not evidence that lx can safely install seccomp filters yet.

Build the Linux test and Linux identity helper in Linux or lx:

```sh
cc -std=c99 -Wall -Wextra -Werror -O2 -pthread \
    no_new_privs_probe.c -o no_new_privs_probe
cc -std=c99 -Wall -Wextra -Werror -O2 identity_probe.c -o identity_probe
./no_new_privs_probe ./identity_probe
```

On a native SmartOS builder, compile the native helpers:

```sh
cc -std=c99 -Wall -Wextra -Werror -O2 \
    identity_probe.c -o native_identity_probe
cc -std=c99 -Wall -Wextra -Werror -O2 \
    native_privilege_probe.c -o native_privilege_probe
```

Copy those helpers into the lx test directory, then run from a fresh,
unrestricted root login inside the lx zone:

```sh
./no_new_privs_probe ./identity_probe \
    ./native_identity_probe ./native_privilege_probe
```

The 17 checks cover argument validation, proc task status, irreversible
setting, task-local state, thread/fork/vfork inheritance, successful and
failed exec, a native shell bridge, Linux and native set-id binaries,
and native inheritable privilege promotion. The native privilege helper
places `proc_setid` in the inheritable set, drops it from effective and
permitted sets, and execs itself. The control must regain the privilege;
the restricted case must not regain it.

Root-only set-id tests create a root-owned temporary directory under
`/var/tmp`, copy the tiny identity helper into it, and mark the copies
setuid, setgid, or both. The test runner itself is never marked set-id.
Children drop supplementary groups and all user/group IDs to 65534 before
exec. Each suppression case has a positive control without the flag.
Controls must pass to establish that the filesystem and environment
actually permit set-id execution. The parent removes the helper copies
and directory at normal exit, including ordinary test failures.
If forcibly terminated, remove the leftover `/var/tmp/lx-nnp-*` directory
after verifying it belongs to that test.

Exit 0 means no failures among the checks that ran; it does not mean that
skipped checks passed. Missing root privileges or optional native helpers
skip their corresponding checks. Baseline lx is recognized via `/native`
and expected to start unrestricted; do not run the baseline from a task
that deliberately inherited `no_new_privs`.

Profile credential replacement, forced privilege configuration, tracing,
zone entry, and concurrent credential-change scenarios still need dedicated
runtime coverage. The probes do not establish seccomp or namespace support.

## Seccomp BPF core

`usr/src/common/brand/lx/lx_seccomp_bpf.c` implements the seccomp-specific
classic BPF verifier and interpreter. It is linked into `lx_brand` but has
an experimental syscall enforcement caller. Both `prctl` and `seccomp`
reject filter installation by default. The separate enforcement experiment
below does not yet enable Codex's sandbox.

The core accepts at most 4096 instructions, aligned word loads within the
64-byte x86 Linux metadata ABI, initialized scratch memory, forward jumps,
and the ALU/return operations accepted by Linux seccomp. Packet loads,
ancillary data, invalid opcode bits, MOD, constant division by zero and
constant shifts of 32 or more are rejected. Variable shifts use x86's
low-five-bit shift count. Metadata words use Linux x86 byte order, without
packet-filter byte swapping. The evaluator returns the opaque action word;
its caller must interpret and enforce it. The verifier's scratch validation
also matches Linux's conservative handling of instructions following RET.

Callers must copy the program and metadata into kernel-owned storage,
allocate the verifier workspace off-stack, and keep accepted programs
immutable throughout evaluation. The core owns no task state or memory.

Build the differential runner on Linux x86-64, setting `ws` to this checkout:

```sh
cc -std=c11 -Wall -Wextra -Werror -O2 \
    -I"$ws/usr/src/common/brand/lx" \
    "$ws/usr/src/common/brand/lx/lx_seccomp_bpf.c" \
    "$ws/usr/src/lib/brand/lx/testing/seccomp/bpf_oracle.c" \
    -o bpf_oracle
./bpf_oracle linux-vectors.bin --exhaustive
```

Each native comparison installs a candidate filter in a separate child.
A prefix permits unrelated syscalls so the child can report its result.
The runner compares Linux installation decisions and observable action
results, then records the program, captured metadata and expected raw core
result. It exhausts all 65536 opcode encodings and covers branch/scratch
joins, seeded control-flow mutations, ALU edges, invalid metadata accesses,
maximum program length and actions. Sixteen additional literal ABI cases
check every metadata word, including negative syscall numbers and the i386
architecture value. Those literals do not exercise i386 syscall entry.
API guards are checked separately. Any mismatch exits nonzero.

`lx_seccomp_ktest` depends on `lx_brand` and calls the actual kernel core,
using the generated binary corpus as input. Build the release and debug
`uts/intel/lx_brand` and `uts/intel/lx_seccomp_ktest` targets. In the isolated
VM, copy the release test module and corpus under `/lab`, then run:

```sh
modload /lab/lx_seccomp_ktest
ktest run -i /lab/linux-vectors.bin lx_seccomp:bpf:vectors
```

The fixture is little-endian and intended for x86. A 16-byte header contains
magic 0x4c585342, version 1, record count and zero. Each record contains four
32-bit fields: instruction count, expected validity, expected raw result,
and zero. Then come 64 metadata bytes and the 8-byte instructions. Invalid
programs have no evaluation expectation. The kernel test bounds every
record and rejects truncated or trailing input.

The x86-64 acceptance run passed 67163 native Linux comparisons and 16
literal ABI cases. The focused 1643-case run passed with AddressSanitizer
and UndefinedBehaviorSanitizer. In the isolated SmartOS VM, the actual
`lx_brand` core passed all 67179 corpus records through ktest; malformed
header, truncated-instruction and trailing-data inputs produced the expected
ktest errors. All 17 no-new-privileges regressions also passed without skips.
Release and debug modules built cleanly; runtime coverage used release.
Filtering remained unsupported, as required. This was a mixed platform
image and does not establish a complete platform build or i386 entry support.

## Experimental kernel enforcement

`uts/common/brand/lx/os/lx_seccomp.c` adds an enforcement prototype, disabled
by the `lx_seccomp_experimental` tunable's default value of zero. Ordinary
zones still reject installation. Only the disposable VM driver enables it.
This is a restricted experiment, not general Linux seccomp compatibility.

With the switch enabled, an amd64 process with one LWP and `no_new_privs`
may install filters through `prctl` or `seccomp`, with flags zero. Programs
and headers are copied before publication; the programs remain immutable.
Stacking preserves earlier restrictions, KILL takes precedence over ERRNO,
and the newest ERRNO wins equal-action ties. Linux error numbers bypass
illumos errno translation, including errno zero and the 4095 clamp.
The prototype accepts constant ALLOW, ERRNO and KILL returns. It rejects
computed returns, TRAP, TRACE, LOG, USER_NOTIF, TSYNC and other flags.
Chains are limited to 64 filters and 32768 instructions including the
four-instruction overhead between stacked filters.

The guard is process-wide and installation requires one LWP. New kernel
paths allow restricted amd64 fork, clone and exec operations without
entering the native user-space emulation library. Later Linux threads
inherit the process guard and no-new-privileges state. Native brand
commands, compatibility entries and vsyscalls still terminate the process.
Both KILL actions terminate the process; separate per-task filter and kill
semantics are not provided. See the guarded process section for limits.
Codex's full sandbox still fails because user namespaces are missing.

The assembly interposer now receives a kernel-supplied entry ABI. Metadata
contains the six raw amd64 registers and saved post-syscall PC before lx
argument processing or table bounds checks. Native SYSENTER and legacy
LCALL dispatch also consult the interposer in `syscall_entry`. lx rejects
native dispatch while a guard exists. User-space emulation and brandsys
have additional checks. The negative audit-table index check is required
because a filter can now return a result for a negative syscall number.

Filter nodes are reference counted. The proc's previous reference becomes
an immutable chain link when a new head is published under `p_lock`.
Process-data copying holds the copied head, and debranding releases it.
This accounts for kernel process lifecycle paths even though normal Linux
fork and exec are not supported for guarded tasks. Disabling the tunable
only prevents new installations; it does not remove existing guards.

Build the additional probe on Linux x86-64:

```sh
cc -std=c11 -Wall -Wextra -Werror -O2 -static -pthread \
    enforcement_probe.c -o enforcement_probe
```

The Linux control runs 17 checks. The enabled lx experiment adds escape,
thread-count and resource-limit checks. Every test runs in a separate child;
expected SIGSYS termination is success only for the designated kill/escape
cases. The parent remains unrestricted. The offline
`lab/enforcement-test.sh` first runs the default-off privilege and seccomp
regressions and the BPF corpus. It then changes the experiment switch through
MDB inside the guarded private VM and runs both enforcement probes. The
matching `unix`, `genunix`, `lx_brand` and `lx_proc` must all be rebuilt.

The initial enforcement milestone passed all 29 enforcement checks, the original
filter-denial probe through both installation APIs, all 17 privilege
regressions without skips, and all 67179 BPF corpus records. Filtering
was rejected before the experimental switch was enabled. The final LWP
layout also preserves all 54 previous member offsets and the 440-byte size;
the entry tag occupies former tail padding, avoiding a layout mismatch
with other lx modules. These results cover the stated experiment only.

## Experimental amd64 signals

The default-off experiment now handles rt_sigaction, rt_sigprocmask,
sigaltstack and rt_sigreturn in the kernel. Registered amd64 handlers can
run in Linux mode under an installed filter. Delivery builds the Linux
frame directly. Kernel-owned records retain native context bookkeeping,
including the original extended floating-point state. Return checks the
most recent frame, user addresses, segment selectors, privileged flags,
MXCSR and alternate-stack fields before restoring user registers and masks.
Native-mode entry remains prohibited, including from a handler.

The path supports nested handlers up to 32 records, alternate stacks,
mask restoration and edits, SA_NODEFER, SA_RESETHAND, interrupted reads
with and without SA_RESTART, SSE/AVX preservation, source-specific siginfo,
and context edits for hardware-fault recovery. It requires a Linux restorer.
Default SIGPWR termination remains correct after a one-shot handler.

This is a restricted ABI experiment. Untracked handlers installed by the
older user-space bridge cannot be imported. A signal interrupting native
emulation cannot use this path. There is no i386 kernel signal backend.
Nonlocal handler exits such as longjmp and user editing of Linux extended
XSAVE records are not implemented. The LIFO kernel-record requirement is
stricter than Linux signal-return semantics. Fork and exec use the restricted kernel paths described below. Existing unfiltered legacy signal returns still use
the older bridge when no kernel frame exists.

The final isolated VM run passed all 31 signal checks with no skips, all
29 enforcement checks, all 17 no-new-privileges checks and the 67179-record
BPF corpus. The raw ELF32 boundary probe returned zero. The native Linux
control passed all 20 common signal checks without skips. Runtime used
release modules in a mixed platform image, not a clean full-platform build.

Build signal_probe.c on Linux x86-64 with the enforcement probe's compiler
flags. Run it without inherited filters. Each case creates a separate child
and installs its own filter. The Linux control has 20 checks. The lx run
adds invalid selectors, privileged flags, addresses, FP state, missing and
replayed frames, native entry from a handler, and syscall-table boundaries.
The raw ELF32 boundary32_probe.s checks reserved syscall 251 and out-of-range
359/360. It is only for lx, since those numbers identify real Linux calls.

The signal pointer appends to lx_lwp_data. All 54 old field offsets remain
unchanged; its size grows from 440 to 448 bytes. Rebuild lx_brand and lx_proc
together. The shared sendsig hook also requires matching unix and genunix.

## Guarded processes and threads

The kernel handles amd64 fork and process-style clone for a single-LWP,
untraced caller. Supported process clone flags cover a child stack,
SIGCHLD or no termination signal, and parent/child TID stores. The child
inherits immutable filters, no-new-privileges, TLS and copied signal records.
Read-only or invalid TID targets do not gain write access. Child filter
stacking does not change its parent's restrictions. Private address-space
children do not clear a same-mm waiter on exit.

The exec path uses the normal kernel ELF loader, translates its initial
aux vector and enters the Linux executable or interpreter directly.
The mapped native brand library is never entered. Filters and masks survive;
caught dispositions and alternate stacks reset, ignored dispositions remain,
and a new handler can return without a native stack. Solaris ELF execution
returns ENOEXEC with the guard retained. The native-exec classifier's negative
errno result lets the ELF loader release its resources normally. A 32-bit
image is rejected. Raw exit and exit_group have kernel paths.

Thread-style clone supports the full shared-process flags used by glibc
and musl pthreads, optional TLS, TID stores, SYSVSEM, DETACHED and PARENT.
Each new LWP starts in Linux mode with its own stack, TLS, Linux TID and
inherited process guard. The kernel preserves POSIX floating-point control
settings. Thread exit clears its TID and wakes joiners. The last exiting
thread supplies the process status through lwp_exit_status; existing native
callers of lwp_exit retain status zero. Signal contexts only request native
stack restoration when such a stack exists.

This is not complete Linux clone or exec compatibility. vfork and VM-sharing
process clones are absent. Fork from a multithreaded process and ptraced
creation are rejected. Filter installation still requires a single LWP;
per-task filters, TSYNC and thread-only KILL remain unfinished. User-space
emulated syscalls still cannot run under a guard. These limits matter for
coding tools that create subprocesses and for general sandbox compatibility.

The isolated release VM passed 15 fork, 7 exec and 5 thread checks, plus
31 signal checks, 29 enforcement checks, 17 no-new-privileges checks and
the 67179-record BPF corpus. No signal or privilege cases were skipped.
Static and dynamic exec checks cover auxv, credentials, masks, dispositions,
alternate stacks, caught-signal return and retained denial. Thread checks
cover TLS, mutexes, join, floating-point controls, signals, final exit status
and blocked native entry. The actual Codex 0.166.0 binary prints its version
after exec under an Internet-socket-denying filter that permits local IPC.
Its normal sandbox still exits 1 at bubblewrap user-namespace creation.
This is a mixed-platform development result, not a full Codex acceptance.

Build the Linux probes with -std=c11 -Wall -Wextra -Werror -O2 -static
-pthread, adding -lm for thread_probe.c. Build exec_probe.c again without
-static as exec_dynamic. The latter needs the matching Linux loader/libc
inside the test root. Linux controls pass 13 fork, 5 static/dynamic exec and
4 thread cases. The lx-only cases cover its explicit unsupported operations.

## Remaining implementation

1. Extend no-new-privileges validation to i386 and the remaining native
   privilege paths, and validate a complete platform build before deployment.
2. Replace the experimental process-wide guard with full per-task filter
   semantics, complete action handling and thread synchronization. Extend
   the immutable filter lifetime and stacking implementation accordingly.
3. Replace the experiment's native-transition termination with an enforceable
   implementation that permits the Linux operations Codex needs. `lx_syscall_enter` currently hands some calls to user-space
   native emulation; filtering only that dispatcher is insufficient.
   Audit `B_EXEC_NATIVE`, brand-native syscalls, restored native stack modes,
   tracing and signal return. Metadata must capture all six raw arguments
   before `lx_emulate_args`, including arguments that lx later dereferences
   or rewrites. Handle unknown calls, i386, x86-64, x32 and alternate entry
   instructions explicitly, with no unfiltered fallback.
4. Add user and mount namespaces, UID/GID maps, namespace descriptors, mount
   propagation and the remaining namespace operations actually used by the
   installed bubblewrap build. Reject unsupported isolation requests explicitly.
5. Test the installed Codex sandbox end to end, including forbidden filesystem,
   network and process operations. The legacy path additionally needs an audit
   of its filesystem restrictions, including Landlock if that version uses it.

The current source locations are:

- `usr/src/common/brand/lx/lx_seccomp_bpf.c` and `lx_seccomp_bpf.h`
- `usr/src/uts/common/io/lx_seccomp_ktest.c`
- `usr/src/uts/common/brand/lx/syscall/lx_prctl.c`
- `usr/src/uts/common/brand/lx/syscall/lx_miscsys.c`, `lx_unshare`
- `usr/src/uts/common/brand/lx/os/lx_syscall.c`, syscall tables and dispatch
- `usr/src/uts/common/brand/lx/os/lx_brand.c`, process and LWP lifecycle
- `usr/src/uts/common/brand/lx/sys/lx_brand.h`, brand state

Linux filter semantics are documented at
https://docs.kernel.org/userspace-api/seccomp_filter.html.

## Private mount-tree backend

The internal lx_mntns backend creates detached LOFS roots with nosub,
private tmpfs grafts, and directory or regular-file binds. The snapshot entry
point collects visible existing submounts, pins their source vnodes, and
recreates parent mounts before nested mounts. It excludes obscured stacked
mounts and returns EAGAIN if the global mount list changes during capture.
It uses the caller's credentials and does not grant mount privileges.
No Linux namespace syscall enables this backend yet. unshare remains EPERM
and clone namespace requests still fail.

A private VM kernel test creates sibling views, covers one view's subdirectory,
checks that its private file cannot appear in the sibling or physical source,
and checks absolute symlink and dot-dot root boundaries. Holding a vnode makes
destruction return EBUSY; the view remains usable and releases after closing
the vnode. Two consecutive runs pass, followed by pool export0 and poweroff.
Release and debug lx_brand and lx_namespace_ktest builds pass. The existing
17 NNP, 67179 BPF, 29 filter, 31 signal, 15 fork, 7 exec and 5 thread checks
also pass in that VM. This is a mixed-platform development image.

The current Codex 0.166.0 managed workspace profile requires user, mount,
PID, IPC and network namespaces. An explicit Linux control uses
`codex sandbox -P :workspace -- /bin/true`; a legacy sandbox_mode override
can leave an inherited permissions profile unrestricted and is not evidence
of sandbox success. The real lx clone request is 0x78020011 and returns
EINVAL. Private mount views are only one part of the required implementation.
UID/GID maps, scoped credentials, remount, proc views, PID/IPC/network
isolation and process lifecycle integration remain unimplemented. Do not claim normal Codex sandbox acceptance from this test.

The snapshot acceptance VM also passes repeated checks for pre-existing
and nested mounts, covered physical files, stacked mounts, read-only/noexec
inheritance, later source mounts, source overmount after capture, private
nested covers, regular-file read-only binds and mismatched bind types. The
original regression suite passes with the new LOFS module loaded. The kernel
LOFS pinned-source argument requires MS_SYSSPACE and MS_DATA together; the
native mount syscall strips MS_SYSSPACE and userspace cannot supply a vnode.
Pinned sources do not follow later overmounts. Normal pathname LOFS mounts
keep their existing behavior.

These entry points are internal and still require native mount privilege.
Snapshot capture currently discovers submounts from canonical global mount
paths and traverses them with the caller's credentials. It does not implement
namespace cloning from an existing detached view, inherited inaccessible
submounts, concurrent directory-rename handling, root overlays, recursive
binds, remount policy changes or deferred teardown after process exit. Those
must be completed before enabling a Linux mount namespace interface. No
Linux namespace request succeeds merely because this backend exists.

## Internal user namespace backend

lx_userns_model.c implements ID-map parsing/composition and Linux capability
state independently of native privileges. lx_userns.c adds reference-counted
namespace objects, serialized one-time map publication, captured map-file
opener credentials, parent/child capability scope, irreversible setgroups
denial and a native-credential reduction helper. These objects are internal.
No process enters one through unshare/clone yet, and Linux-visible capget,
capset or PR_CAPBSET_DROP behavior has not been replaced by this backend.

UID/GID maps flatten parent-relative ranges to backing IDs and preserve
reader-relative map output. Checks cover overlapping ranges, unmapped parent
ranges, the reserved -1 ID, a maximum of 340 extents, parent-root SETFCAP
requirements, opener/current authority, and publication once only. Namespace
owners use backing IDs. Scoped capability checks never authorize operations
in an ancestor or unrelated namespace. A namespace owner in the parent has
authority over its descendants. setgroups denial passes to children.

The per-task capability value stores effective, permitted, inheritable,
bounding and ambient masks. It models real bounding drops, capset constraints,
the original eight securebits and locks, ambient changes and ordinary exec
without file capabilities or setid transitions. Native privileges remain
separate: the credential helper limits all native sets to the caller's basic
privileges, preserves backing IDs, enables privilege awareness, and removes
external policy and pfexec state. This helper alone is not process isolation.

The Linux control runner builds from the checkout:

    bash usr/src/lib/brand/lx/testing/seccomp/run-userns-linux-controls

It uses an installed bubblewrap to create a capability-enabled bootstrap user
namespace, because the Ubuntu AppArmor policy restricts capabilities in user
namespaces created directly by unconfined helper executables. The runner
changes no host security settings. It compares mapping writes, bounding and
capset changes, securebits/ambient operations, ordinary exec and the nested
identity mapping used by Codex 0.166.0 bubblewrap. 102 control assertions pass
without skips, both normally and under ASan/UBSan on codex-testbox. The current
control boundary inherits the bootstrap namespace's existing ID maps, rather
than assuming it has the initial namespace's complete mapping.

Clean release/debug lx_brand and lx_namespace_ktest builds pass. The two new
brand C files also pass a separate Smatch-enabled compile. In the offline VM,
all four user suites pass: maps, capabilities, native and lifetime. They test
340-range publication, nested translation, ancestor/sibling confinement,
Native credential UID/GID retention, native configuration/mount denial,
32 concurrent pairs of map writers, and reference release through the depth
limit. The mount views/snapshots and all existing regressions also pass.
This remains a release-runtime mixed-platform development result.

Process/LWP credential publication, complete fork/exec transitions, procfs
per-open authority capture, UID/GID translation at every ABI boundary,
native-entry confinement and resource-specific capability checks remain
required before exposing namespace syscalls. Namespace quotas,
setns descriptors, filesystem capabilities, privileged exec and newer
securebits also need implementation or explicit rejection. The current
Linux namespace requests remain unsupported, and normal Codex sandbox
acceptance has not been achieved.


## Immutable brand credentials and process access

Scoped Linux authority is an immutable lx-owned payload attached to a native
credential. The common kernel carries one optional opaque pointer and delegates
process-access policy through brand_ops. Existing crdup/crcopy/crfree paths
hold and release the payload; its lifetime keeps the brand module registered,
including when a copied credential outlives a zone or an open file captures it.
Ordinary native credentials have no payload. No Linux capability mask or
namespace hierarchy is stored in common kernel structures.

The amd64 credential grows from 144 to 152 bytes. The two appended brand
callbacks grow brand_ops from 336 to 352 bytes. CTF checks preserve every old
member offset. lx_proc_data separately gains one eight-byte namespace reference
for the address-space owner, inherited at fork, replaced in the existing exec
hook, and released by clearbrand. This reference is distinct from the current
credential's namespace: Linux unshare does not transfer ownership of older,
non-dumpable memory into the new namespace.

The lx policy enforces ancestor/sibling confinement, Linux permitted/effective
capability dominance, parent namespace ownership and non-dumpable memory
checks before native identity/root shortcuts. Native zone policy still runs
first. Linux ptrace requests use real IDs and permitted capabilities; procfs
reads use effective IDs and capabilities. A scoped capability never expands
native privilege sets. These constructors and checks remain internal; no
user namespace syscall publishes a scoped credential yet.

Linux controls for process access can be built and run separately:

    cd usr/src/lib/brand/lx/testing/seccomp
    cc -std=gnu11 -O2 -Wall -Wextra -Werror -o proc_access_linux proc_access_linux.c
    bwrap --unshare-user --unshare-pid --uid 0 --gid 0 --ro-bind / / \
        --proc /proc --dev-bind /dev /dev --cap-add ALL -- ./proc_access_linux

All 13 control scenarios pass on codex-testbox, without skips. They cover
capability dominance, ancestor/sibling access and old/new address-space
ownership across unshare and exec. The installed bwrap bootstrap gives each
run private user/PID namespaces without changing host policy.

The namespace ktest module adds credential-copy/lifetime tests and scoped
process-access tests. It also opens real native procfs memory/root paths and
lx procfs maps with a deliberately unclamped native-root credential carrying
scoped Linux authority, to check that root shortcuts cannot bypass lx policy.
Run /lab/userns-test.sh only in the disposable offline VM after the existing
enforcement driver has prepared its lx-probe fixture.


At checkpoint 328a2ddfbd, all six user suites pass in offline VM boot26,
including credential copies and actual procfs access. The credential test
passes again after the lx fixture halts, checking brand-reference cleanup.
Mount views/snapshots and all previous enforcement/signal/fork/exec/thread
regressions also pass. The first run found a missing persistent lock in the
synthetic proc fixture, fixed by that test-only commit. Production kernel
source is 1d087dfc71, with clean release/debug core and lx modules. The VM
retains earlier LOFSa8b and May platform/userland, so this is a mixed-platform
release-runtime result, not acceptance of the full Codex sandbox.
