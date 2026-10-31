Experimental amd64 `unshare(CLONE_NEWUSER)` and `clone(CLONE_NEWUSER)` entry
are implemented and disabled by default. See [USERNS-ENTRY.md](USERNS-ENTRY.md)
and [USERNS-CLONE.md](USERNS-CLONE.md). Mount/PID/IPC/network isolation and the
complete Codex sandboxed workflow remain incomplete.

Kernel libc sleep support and its controls are described in
[CLOCK-SLEEP.md](CLOCK-SLEEP.md).

Scoped CPU-affinity and scheduling authority are described in
[Scheduling and affinity](#scheduling-and-affinity) below.

Emulated resource-limit authority and inheritance are described in
[Emulated resource limits](#emulated-resource-limits) below.

Initial-namespace administration guards are described in
[Initial-namespace administration](#initial-namespace-administration) below.
Public namespace entry remains disabled.

Scoped filesystem attribute permission is documented in
[FS-CHOWN.md](FS-CHOWN.md).

Initial UID0 map authority is documented in
[UID0-MAP-AUTHORITY.md](UID0-MAP-AUTHORITY.md).

Kernel rmdir and accepted-stream credentials are documented in
[RMDIR-ACCEPT.md](RMDIR-ACCEPT.md).

Native per-message stream credentials are documented in
[STREAM-CREDENTIALS.md](STREAM-CREDENTIALS.md).

# lx seccomp enforcement probe

The native user-namespace allocation budget is documented in
[USERNS-BUDGET.md](USERNS-BUDGET.md). Ownership and peer-ID translation is in
[ID-BOUNDARIES.md](ID-BOUNDARIES.md). Traced exec is documented in
[TRACED-EXEC.md](TRACED-EXEC.md). Native-backed UID/GID/groups are documented in
[SCOPED-IDS.md](SCOPED-IDS.md), and ordinary exec in
[SCOPED-EXEC.md](SCOPED-EXEC.md). Public namespace entry remains disabled.

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
paths and traverses them with the caller's credentials. The separate copy
backend described below handles stable detached views. Inherited inaccessible
submounts, concurrent directory-rename handling, root overlays, recursive
binds, remount policy changes and process namespace integration still need
implementation before enabling a Linux mount namespace interface. No
Linux namespace request succeeds merely because this backend exists.

## Internal user namespace backend

lx_userns_model.c implements ID-map parsing/composition and Linux capability
state independently of native privileges. lx_userns.c adds reference-counted
namespace objects, serialized one-time map publication, captured map-file
opener credentials, parent/child capability scope, irreversible setgroups
denial and a native-credential reduction helper. These objects are internal.
No process enters one through unshare/clone yet. Kernel capget can read the
scoped payload, while scoped capset still awaits per-LWP publication. Real
PR_CAPBSET_DROP behavior has not been connected to this backend.

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
memory-file snapshots, UID/GID translation at every ABI boundary,
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


## Captured procfs identity maps

UID/GID map and setgroups files now retain the target namespace at VOP_OPEN.
Each open receives a private lx procfs vnode. The vnode holds only a namespace
reference; file_t already holds the immutable opener credential and supplies
it to VOP_READ/VOP_WRITE. Writes use that credential separately from CRED(),
which identifies the current writer. The vnode releases its namespace at
inactive. Each lx zone owns an initial namespace object through its existing
brand-data lifetime. No common-kernel structure changed in this step.

These files use the real namespace maps and irreversible setgroups state.
Map reads use the opener's namespace for parent-relative IDs and continue
working after target exit. Writes apply scope and one-shot publication rules,
reject oversized/nonzero-offset requests, and advance the uio only on success.
Scope rejection precedes copying map data. A setgroups writable open requires
SYS_ADMIN authority; a later privilege drop does not erase the opener's rights.
The map-file native actor projection uses the existing SYS_ADMIN/MOUNT and
PROC_SETID mappings. It does not provide a full initial Linux capability set;
SETFCAP remains absent pending that work. No namespace entry syscall is enabled.

Linux controls can be compiled independently:

    cc -std=gnu11 -O2 -Wall -Wextra -Werror -o proc_maps_linux proc_maps_linux.c
    cc -static -std=gnu11 -O2 -Wall -Wextra -Werror \
        -o proc_maps_baseline proc_maps_baseline.c
    bwrap --unshare-user --unshare-pid --uid 0 --gid 0 --ro-bind / / \
        --proc /proc --dev-bind /dev /dev --cap-add ALL -- ./proc_maps_linux
    bwrap --unshare-user --unshare-pid --uid 0 --gid 0 --ro-bind / / \
        --proc /proc --dev-bind /dev /dev --cap-add ALL -- ./proc_maps_baseline

On codex-testbox all six map-file namespace scenarios and all three baseline
scenarios pass without skips. The controls cover target/opener capture across
namespace changes, privilege changes, target exit, write offsets and setgroups
open authority. Baseline controls retain the bootstrap's actual allow/deny
state, rather than assuming that installed bwrap left setgroups enabled.

Offline VM boot27 passes the proc_maps kernel test and the same three-scenario
static Linux baseline binary inside lx. The kernel test substitutes namespace
objects only on private open vnodes, so it can exercise scoped UID/GID writes,
reader-relative maps, opener/current authority and lifetime through real VOPs
without publishing an incompletely confined process credential. The Linux ABI
probe tests ordinary opens, task paths, file metadata, target exit and an
already-open setgroups file after a real capset drops SYS_ADMIN. A new open is
denied while the original file retains its captured authority.

All seven user suites pass, with credentials repeated after zone halt. Mount
views/snapshots and the existing NNP/BPF/filter/signal/fork/exec/thread probes
also pass. Runtime kernel source is eef956594c, tests c089cc3cd4, driver
f6c5632cbe. Clean release/debug lx_brand/lx_proc and namespace ktest builds pass.
Common kernel1d087dfc71 and LOFSa8b remain in a mixed May platform/userland VM.
This is a release-runtime checkpoint; the full Codex sandbox remains pending.


## Kernel capability syscalls

capget/capset now have kernel handlers for amd64 and i386. The obsolete
516-line userland implementation and its dispatch entries were removed.
The existing capability-to-native-privilege mapping has one implementation,
which also supplies the initial actor for procfs identity maps.

Native-backed capset prepares all three sets on an unpublished credential,
then replaces p_cred once under p_crlock and uses crset for the native thread
notification. Validation failure leaves the original credential unchanged.
This removes the old three-setppriv partial-commit window and fixes an
inheritable-only change being skipped. Inheritable capabilities may remain
after being removed from permitted. Basic native privileges already held by
the caller remain available when Linux sets are zero. Requests that cannot be
represented by coupled native privileges, such as separate SETUID/SETGID
rights, fail instead of reporting a capability drop that did not occur.

The native-backed path retains illumos process-wide credential semantics.
It does not implement Linux per-task capability changes. Scoped payloads can
be queried, but scoped capset returns EOPNOTSUPP until per-LWP publication and
inheritance are implemented. Bounding/securebits/ambient prctl transitions
also remain unfinished. The native procfs bounding value remains explicitly
a legacy approximation. No namespace entry syscall was enabled by this work.

ABI controls cover v1/v2/v3 widths, preferred-version negotiation, NULL data,
a version field at a protected-page boundary, PID/error precedence, unknown
high bits, E/P/I changes, and failure preservation. Remote capget observes
256 snapshots across an E/P/I drop and must see a complete old or new state.
A procfs test compares CapInh/CapPrm/CapEff with capget after a real change;
those fields now use the held process credential rather than constant zeros.
The shared Linux PID lookup also rejects a global-hash hit from another zone.
Its negative test uses a live lx PID and a separate synthetic zone view.

Build the controls on Linux:

    cc -static -std=gnu11 -O2 -Wall -Wextra -Werror \
        -o cap_syscalls_probe cap_syscalls_probe.c
    as --32 -o cap_syscalls_probe32.o cap_syscalls_probe32.S
    ld -m elf_i386 -o cap_syscalls_probe32 cap_syscalls_probe32.o

Run the first program normally and with --filter inside the same installed
bwrap bootstrap documented above. The i386 executable is freestanding and
returns the failed stage number, or zero after all seven stages pass.
Codex-testbox passes all 11 scenarios both normally and with filtering, and
the i386 control returns zero, without skips.

The private image updater can include rebuilt emulation libraries with
LX_LAB_UPDATE_USERLIBS=1. SmartOS mounts /usr from compressed usr.lgz, so the
updater follows tools/build_live: decompress the detached UFS image, update
its libraries, unmount/detach, compress and replace it. The old UFS allocation
must be released before writing the replacement. SHA256 readback checks cover
the inner libraries and compressed image. No library update is inferred from
copies under the root image's shadowed /usr directory.


Final offline VM boot30 passes all eight user suites, the 11 capability ABI
scenarios normally and under filtering, all seven i386 stages, three procfs
map baseline scenarios and every existing regression. Both runtime emulation
library hashes match the rebuilt objects. Kernel source is 7edbda5c50,
procfs status source 488b9388e7, library source d768aa8856, and image staging
source a095ee9c21. Release/debug kernel/test builds and both library builds
pass, using seeded SDK dependencies. Earlier boot28 used old libraries despite
copies in the root image's shadowed /usr; boot29 verified the corrected nested
image before the final PID-boundary change. The remaining platform/userland
and SDK dependencies are not a full source rebuild, so this remains a mixed
platform release-runtime checkpoint. No production host deployment occurred.

## Initial-namespace administration

Scoped credentials must not administer the surrounding native zone merely
because their backing effective UID is 0. lx_cap_initial evaluates native
Linux-capability projection only when the credential belongs to the initial
user namespace. It checks actual effective native privilege sets and does
not require UID0. Descendant credentials cannot acquire initial authority
from their scoped capability bitmap or even from native privilege bits.
The helper adds no credential or process fields.

The zone-wide emulated swapon/swapoff visibility and current UTS hostname/
domain require initial-scope CAP_SYS_ADMIN. Hostname/domain changes retain
native systeminfo checks as well. Reboot, privileged empty-buffer syslog and
vhangup remain legacy initial-root stubs; descendant credentials are rejected.
No private PID-namespace reboot target is implemented. These stubs are not
full Linux CAP_SYS_BOOT/SYSLOG/SYS_TTY_CONFIG emulation. Unprivileged empty-
buffer syslog actions 3/10 retain their existing behavior.

Swapoff checks authority before copying the path. Swapon rejects invalid
flags before checking authority, then copies the path. Reboot checks initial
root authority before validating magic. These denial precedences match the
Linux controls for the supported scope. Linux references:
https://github.com/torvalds/linux/blob/master/mm/swapfile.c
https://github.com/torvalds/linux/blob/master/kernel/sys.c
https://github.com/torvalds/linux/blob/master/kernel/reboot.c
https://github.com/torvalds/linux/blob/master/kernel/printk/printk.c

VM63 final canonical drivers PASS without console overrides: identity 64+64,
capability 19+19 and prior ABI/i386/NNP/BPF/filter/signal/fork/exec/thread/
map/mount/budget/lifetime/native-control and ownership/mode ACL/race suites.
The new scoped case uses backing native UID0 with all scoped capabilities,
then removes effective SYS_ADMIN and repeats the denials. Both times it
checks swap visibility and UTS names remain unchanged. Static and dynamic
initial-lx controls toggle and restore only the disposable zone's emulated
swap display, check legacy no-op stubs, then fork a native initial-root child
that removes effective SYS_ADMIN while keeping it permitted and verifies
that swap/UTS calls are denied. Test reboot uses only ignored CAD_ON; it never
requests restart, halt or poweroff. No real swap device is disabled or enabled.
The explicit initial-control mode refuses to run outside lx.

The native ktest verifies a non-root UID with genuine native authority,
rejection after removing either required native mount or admin privilege,
rejection of an invalid capability index, and a scoped credential's rejection
independent of its native privilege bits. Private unpublished credential
fixtures do not modify the source credential or run privileged operations.
Native credential lifetime checks pass after the probes exit. Linux 64 static/
dynamic controls pass with GCC -Wall -Wextra -Werror. Swap visibility is read
from /proc/meminfo; the existing sysinfo implementation does not consult that
emulation flag and is not used as evidence of its state.

Kernel brand source 00e8179776 and native ktest b756a76b32 pass clean release/
debug builds. Runtime uses release objects; debug is compile-only. Common/
unix a2f98e1dd2, ZFS/proc/elf 89c4ea1aa5 and Native tl600d/LOFSa8b/VM30
emulation libraries/VM46 support remain over the mixed May base. There is no
new ZFS, common-kernel, pool format or feature change in this milestone.
Evidence is lab/evidence/boot-63-initial-admin-acceptance.log; provenance is
/build/RUNNING_TESTS.md. Runtime hashes and synced shutdown are recorded there.
No physical host, boot tree or production Debian zone was changed.

This closes one prerequisite for public namespace entry. Scheduling and
other remaining privileged ABI checks, filesystem permissions, independent
FSIDs and complete mount/PID/IPC/network isolation still need implementation
or validation. An actual newest-Codex sandboxed coding workflow and complete
platform validation remain required; no sandbox acceptance is claimed here.

## Emulated resource limits

The four existing per-process limits without native rctls, LOCKS, NICE,
RTPRIO and RTTIME, now serialize reads and updates with p_lock. Raising a
hard limit requires effective initial-namespace CAP_SYS_RESOURCE through
lx_cap_initial. Namespace-local capability bits and backing native UID0 do
not provide that authority. Lowering hard limits and adjusting soft limits
within the hard limit remain permitted. The change adds no process fields.

Fork copies these limits under the parent's existing p_lock and retains
them rather than resetting them to defaults. Defaults are initialized by
lx_setbrand when brand data is created, before a fork child receives its
parent's copy. Ordinary lx exec retains the inherited values. The existing
initial defaults are preserved.

For these four limits, prlimit64 obtains the old value and installs the new
value in one p_lock critical section. Input is copied before mutation and
output after releasing the lock. Rejected hard-limit increases leave both
the stored limits and the caller's old-value buffer unchanged. An aliased
input/output buffer returns the value from before the update. Existing
foreign-process prlimit64 support remains incomplete. This work does not
add enforcement for the emulated limits or complete scheduling policy
compatibility.

Linux controls pass 66 identity cases per static/dynamic executable and
three initial-namespace authority cases per executable. New controls cover
all four emulated resources through raw setrlimit, getrlimit and prlimit64:
soft/hard bounds, invalid pairs, finite and infinite denied increases,
effective versus permitted authority, ordinary users, backing native UID0,
fork and exec inheritance. A separate two-thread case exchanges 2,000
unique soft limits and requires every preceding value to be returned
exactly once, including the initial and final values. Initial authority
controls change only per-process limits in short-lived children.

Linux reference: https://github.com/torvalds/linux/blob/master/kernel/sys.c

VM64 passes both canonical drivers without serial overrides: 66+66 identity
cases, 19+19 capability cases, 3+3 initial-limit authority cases, and the
existing ABI/i386/NNP/BPF/filter/signal/fork/exec/thread/map/mount/budget/
lifetime/native-control and filesystem ACL/race suites. Runtime hashes match
the staged modules and probes. The pool exported successfully and the VM
completed synced poweroff. Evidence is
lab/evidence/boot-64-emulated-rlimits-acceptance.log.

Kernel brand dc6c025b53 passes clean release/debug builds. Probe/driver source
is a0d0f82a89. Runtime uses the release object; debug remains compile-only.
Common/unix a2f98e1dd2, ZFS/proc/elf89c4ea1aa5, ktestb756a76b32 and retained
Native tl600d/LOFSa8b/VM30 libraries/VM46 support remain over the mixed May
platform. This is not full-platform or actual Codex-workflow acceptance.
No ZFS, common-kernel, pool format, physical host, boot tree or production
Debian zone change is part of this checkpoint. Scheduling policy checks and
public namespace creation remain incomplete.

## Scheduling and affinity

Linux affinity ownership compares the caller's effective backing UID with
the target task's real or effective backing UID. It does not require group
identity or treat the caller's real UID as authority. A different owner
requires effective CAP_SYS_NICE in the target credential's user namespace.
The shared lx target lookup now distinguishes affinity from scheduling
policy writes, whose privileged override needs initial-namespace authority.
Scoped target checks use the actual task credential under p_lock rather
than the representative process credential. Native credentials continue to
use p_crlock. Affinity rechecks ownership after acquiring cpu_lock/p_lock.

Actual CPU binding has a second native permission check. A new affinity
mode on the existing brand credential process-permission callback performs
a pure credential check there under thread_lock. It does not allocate,
block, consult ptrace/dumpability or add a brand_ops pointer. Native-only
credentials defer to existing hasprocperm policy. System-class rejection,
CPU partition checks and binding mechanics remain native. Cross-zone native
administration also defers to existing native policy. Namespace capability
checks follow the immutable namespace ancestry and the target credential;
scoped authority cannot reach the initial namespace or a sibling namespace.

FSS scheduling emulation replaces backing-UID0 shortcuts with initial
CAP_SYS_NICE or the target's existing RTPRIO/NICE limits, as applicable.
Deadline changes require initial authority. Ordinary callers cannot clear
an existing reset-on-fork flag. Emulated parameter validation precedes
capability checks: normal policies require zero priority, FIFO/RR require
1..99, and deadline attributes must describe a valid runtime/deadline/period.
Legacy setscheduler/setparam cannot describe deadline parameters. The
supported old sched_attr ABI requires 48 bytes, treating size zero as 48.
The change does not implement newer attribute extensions.

Native class priority is read and adjusted to the existing policy hint's
absolute value, instead of adding the hint on every request. Native return
errors propagate to Linux. Linux policy/priority/flags/deadline metadata is
published only after a successful native update. Repeating the same policy
therefore neither accumulates priority nor falsely publishes failed changes.
Native class ceilings still apply. setpriority's EPERM/EACCES selection now
uses actual initial CAP_SYS_NICE rather than a backing-UID0 shortcut.

The native ktest covers effective versus permitted capability authority,
ancestor/descendant/sibling/initial boundaries, invalid capability indices,
namespace ownership, ordinary native fallback, real-UID ownership without
GID equality, and rejection despite ptrace authority or a same-process
pointer. Private credential fixtures never publish privileged credentials.
The Linux controls pass 70 identity/affinity cases and seven scheduling
cases per static/dynamic executable. Actual affinity tests bind a stopped
peer to one CPU, read back the mask and restore it, with effective
CAP_SYS_PTRACE removed. Other cases check missing effective SYS_NICE,
per-thread target credentials and caller real UID not granting authority.

This remains scheduling emulation constrained by native class policy. It
does not add a Linux deadline scheduler, complete nice/reset-on-fork
semantics, the direct TS/RT ABI, or arbitrary multi-CPU mask enforcement.
Public namespace entry and the actual newest-Codex sandboxed workflow are
still incomplete. These tests use only the private ktest attachment in the
offline VM. Linux reference:
https://github.com/torvalds/linux/blob/master/kernel/sched/syscalls.c

VM66 final canonical drivers pass without serial overrides: 70+70 identity/
affinity cases in the ordinary class, 8+8 scoped scheduling/affinity cases
under FSS, 19+19 capability cases, initial-limit/admin controls and all prior
ABI/i386/NNP/BPF/filter/signal/fork/exec/thread/map/mount/budget/lifetime/
filesystem ACL/race suites. Both initial-lx probes repeat setscheduler,
setparam and setattr, with native priocntl observations requiring FSS user
priority and its limit to be exactly 60 for FIFO, 59 for RR and 0 for OTHER.
The public Linux getters agree. Removing effective initial SYS_NICE while
retaining native UID0 and a zero RTPRIO limit denies RT entry; denied nice
increases return EACCES. A scoped native-UID0 case with a positive RTPRIO
limit reaches the native class denial and retains all reported metadata.

The new native hierarchy test initially failed in VM65 because its nested
creator's parent ID maps were absent. The corrected fixture maps the parent
before creating the descendant. VM65 native credential lifetime checks and
synced shutdown passed; VM66 is the final acceptance run. Runtime hashes of
the rebuilt modules and probes match staging. The optional /kernel FSS hash
lookup names no file; FSS is installed under /usr/kernel and remains part of
the retained base. No FSS object hash is inferred from that failed lookup.
The pool exported successfully and the VM completed synced poweroff.
Evidence is lab/evidence/boot-66-scheduling-affinity-acceptance.log.

Common/unix/genunix/brand/proc/elf source92390bf918 passes clean release/debug
builds. Probe source is b159be75ea, driver1f7809be93 and native ktest50fd826793,
whose clean release/debug builds pass. Runtime uses release objects; debug
is compile-only. ZFS89c4ea1aa5, Native tl600d/LOFSa8b/VM30 emulation libraries/
VM46 support and the native FSS class remain over the mixed May platform.
No ZFS format/feature, physical host, production Debian zone or host boot
installation changed. Full-platform and actual Codex sandboxed workflow
validation remain required. Public namespace creation remains disabled.


## Copying private mount views

`lx_mntns_view_clone` copies an existing detached view using new LOFS mount
instances over the same backing vnodes. It replays the private mounts in
creation order, including mounts hidden by later overmounts. File contents
remain shared; later mount operations stay within their respective views.
Mount targets retain the paths resolved by `lookuppnvp`, using `refstr_t`.
This also supports regular-file binds without reverse vnode path recovery.

This is an internal backend prototype. Its recorded paths require a stable
directory hierarchy for the source and copy lifetimes. Copies pin source
vnodes, so synchronous destruction returns EBUSY while copies retain them.
Final reference release instead defers cleanup as described below. Process
root/cwd ownership and concurrent rename handling remain unfinished. No Linux
mount-namespace syscall publishes these views yet.

The release VM78 passed the mount-copy test twice directly and twice through
the canonical user-namespace driver. Checks cover imported/private/nested and
hidden mounts, copies of copies, distinct mount instances, shared filesystem
updates, private subsequent overmounts, normalized target paths, regular-file
binds, read-only/noexec/nodev/nosetuid inheritance, root boundaries and busy
source retention followed by cleanup. Existing mount-view/snapshot tests,
BPF corpus/malformed controls, enforcement/signal/fork/exec/thread/clock/i386
and user-namespace/capability/identity/FSS/ACL/race regressions also passed.

Both experimental switches were reset to zero. The zone halted, the pool
exported successfully and the VM completed filesystem sync before poweroff.
The touched brand and namespace-test modules were clean-built in release
and debug; runtime used release on the retained mixed VM73 platform.
This is not a complete platform build or Codex sandbox acceptance.

The explicit `codex sandbox -P :workspace` diagnostic still reaches two
bubblewrap clone requests with flags `0x78020011`, both returning EINVAL.
Mount/PID/IPC/network namespace integration and the remaining bubblewrap
mount operations are still required. `lab/bubblewrap-trace.sh` records that
request; its DTrace exit status is not the sandbox result.


## Mount-view ownership and retirement

Views now carry atomic references. `lx_mntns_view_hold` retains an existing
reference, and `lx_mntns_view_rele` consumes one. The last release queues
cleanup, allowing a source to be released before its surviving copies or
before the last open vnode. `lx_mntns_view_destroy` remains a synchronous
operation: it consumes a sole reference only on success and returns EBUSY
when the view is shared or its mounts remain busy.

Each graft holds its filesystem root so native unmount cannot dismantle a
live view. Controlled cleanup releases that hold before normal unmount and
reacquires it on failure. Native filesystem unmount policy is unchanged.
A single lx task queue is created when the first view is created. Each view
has a preallocated queue entry; a busy view schedules a one-second retry,
allowing other queued views to proceed. The saved creation credential stays
held through cleanup. The worker fences fired timeout callbacks with
`untimeout` before reclaiming their argument. Active views prevent brand
unload, and successful unload drains the task queue before removing code.

VM81 covered backend ownership. The process root/cwd work is described below.
Namespace budgets, stable handling of directory renames, mount syscall routing
and the remaining bubblewrap namespaces are still required. Per-mount lazy
detach also remains separate from retiring an entire view.


VM81 passed the ownership/lifetime case eight times: twice directly, twice
through the canonical user-namespace driver, and twice on each side of the
idle unload/reload check. Native unmount rejected owned tmpfs and LOFS grafts.
Held file/root vnodes remained usable after final owner release, and the
active-view count returned to zero after the last vnode closed. SDT recorded
only native EBUSY retries. After an idle unload, a two-second pause and reload,
the queue/refcount/experimental switches were zero and the mount tests passed
again. The two canonical enforcement/user-namespace drivers and BPF controls
also passed. Uploaded scripts and kernel/test modules matched their manifests.

The zone halted, the pool exported successfully, filesystem sync completed
and QEMU exited. Brand and namespace-test modules were clean-built for
release and debug; runtime used release on the retained mixed VM73 platform.
The actual Codex 0.166.0 workspace sandbox still requests clone 0x78020011 and
receives EINVAL. See lab/mount-lifetime-results.json for exact source and
artifact hashes. No production host deployment occurred.


## Process mount-view roots

The internal root/cwd handoff prepares vnode references outside process locks,
then exchanges the prepared state under `p_lock` after checking the expected
old root/cwd. It invalidates the cached cwd. It requires a guarded lx process,
one LWP, private filesystem state and a stopped/current/pre-runnable target;
tracing, vfork and shared-FS groups are rejected. The old references are
returned to the caller for release outside process locks.

A process owns one view reference. Fork retains it; Linux exec preserves it.
Process exit clears it before zombie reaping, while native root/cwd and open
vnode references keep busy filesystems alive until they can be reclaimed.
Clearbrand handles references from failed creation or brand teardown. View
membership itself participates in the native-entry guard.

Topology copying preserves native filesystem root kinds, including mntfs
regular-file roots over directory mountpoints and namefs door roots. These
copy operations are distinct from new directory/regular-file binds, which
retain their type-matching checks. The real-zone fixture snapshots and clones
the zone tree before assigning its root/cwd, covering these native mounts.

No Linux mount-namespace syscall invokes the handoff yet. Namespace creation
authority, budgets, mount syscall routing, namespace descriptors, rename
handling, pivot/remount/proc/devpts behavior and remaining PID/IPC/network
isolation are still required for bubblewrap.


VM84 passed all eight process cases: static/dynamic programs, UID0/65534,
and guarded attachment versus unguarded rejection. The fixture verified
stale-cwd rejection without publication, private tmpfs and read-only file
binds, cached and raw getcwd, root boundaries, retained filtering, Linux
fork/exec inheritance, SIGKILL wait status and reference release before
zombie reaping. An orphan retained its view after the original parent exited,
then released it on exit. Outside processes retained the original mount tree,
and the active-view count returned to zero after every case. The same probe
passed four Linux controls covering static/dynamic programs and both UIDs.

The full enforcement, mount-backend and user-namespace regression drivers
passed. The runner now inherits /dev/null on stdin, including its zlogin
children. This fixes the serial-console SIGTTIN interruption in VM83. The
actual Codex 0.166.0 workspace sandbox still requested clone 0x78020011 twice
and received EINVAL with both experiments enabled. No Linux mount-namespace
creation syscall is enabled by this checkpoint.

Brand, lx_proc and namespace-test modules were clean-built for release and
debug. Runtime used release on the retained mixed VM73 common kernel, LOFS,
ZFS and user libraries. Both experimental switches were reset to zero and
all views retired. The zone halted, the pool exported, filesystem sync
completed and QEMU exited. See lab/mount-process-results.json and
lab/mount-process-linux-results.json for source/artifact hashes and results.
This does not establish full-platform or production-host acceptance.


## Mount-view resource accounting

Each lx zone owns a reference-counted mount-view account. The optional
`max-mount-namespaces` zonecfg attribute has type `uint` and defaults to 64.
It bounds private views, including copies and views still waiting for busy
mounts to retire. It does not charge the zone's original mount tree. This is
a zone resource limit, rather than Linux's complete per-user ucounts policy.

Creation, snapshot and clone reserve a charge before lookup, allocation or
mount construction. A failed constructor returns its reservation. Clones
inherit the source account, and additional references to the same view do
not consume another charge. Final release retains the charge through busy
cleanup; successful reclamation returns it. The account itself stays alive
through outstanding reservations even after its zone/creator reference is
released. Lowering the maximum preserves existing views and denies new
reservations until usage falls below the limit. A maximum of zero disables
new private views.

`LX_ATTR_MNTNS_MAX` and `LX_ATTR_MNTNS_USED` expose uint32_t maximum and usage
through the existing native zone attribute interface. USED is read-only.
Configuration follows the native zone lifecycle and privilege checks.
No new filesystem privilege is granted by accounting. Constructors still
require native mount authority.

The trusted backend permits NULL accounts only in the global zone, for
fixtures that do not belong to an lx zone. Process root/cwd publication
requires the view's account to match the target zone's account. This prevents
attaching an unaccounted view or charging an attached view to another account.
The process fixture now charges both its real-zone snapshot and its clone.

The kernel quota case exercises all three constructor paths, sharing,
exhaustion, failed native mount refunds, lowering below current usage, zero
quota, a clone retaining its retired source, open-file retention, reuse after
cleanup and account-owner release before cleanup. Native tests verify zone
attribute validation, read-only usage, privilege and lifecycle checks, usage
observation while lowering the maximum, and zonecfg boot configuration.

This is resource accounting and process-zone binding. User-namespace creator
and mount-mutation authority, namespace descriptors, public Linux syscall
entry, per-mount limits, directory rename handling and the remaining
bubblewrap mount/PID/IPC/network operations are still required.


VM85 passed the quota lifecycle case four times across the mount-backend and
canonical user-namespace drivers. Both native zone-quota cases passed, along
with native attribute validation, privilege/lifecycle checks and booting with
max-mount-namespaces=7. All eight static/dynamic UID0/65534 process-view cases
passed with zone accounting; the four guarded attachment cases also rejected
a foreign account without changing root/cwd. The full enforcement and
user-namespace regression drivers passed. All views retired and each process
idle check observed zero zone charges.

The actual Codex 0.166.0 workspace sandbox still issued clone 0x78020011 twice
and received EINVAL with both experiments enabled. Both switches were reset
to zero, the zone halted, the pool exported, filesystem sync completed and
QEMU exited. Brand, lx_proc and namespace-test modules were clean-built for
release/debug and lx_support was rebuilt. Runtime used release on the
retained mixed VM73 common kernel, LOFS, ZFS and user libraries. No production
host deployment occurred. See lab/mount-budget-results.json for exact source,
artifact and evidence hashes. Linux namespace creation remains unfinished.


## Mount-view user-namespace ownership

A private view holds a separate immutable owner credential in addition to
its native mount/cleanup credential. Constructor callers supply both.
Capability checks use the calling task's current effective CAP_SYS_ADMIN
relative to the owner's user namespace through lx_userns_cred_capable.
The owner's captured capability mask does not grant authority to callers.
A clone takes an explicit new owner, allowing a newly created mount namespace
to belong to the calling task's current user namespace.

The current-process view accessor holds the view under p_lock and returns a
reference. It lets mount syscall routing inspect the view without borrowing a
process-owned reference across blocking work. Root/cwd publication now checks
both the target zone's account and the owner's zone. A matching account alone
cannot authorize installing a view owned by a different zone.

Until tracked private mount operations are implemented, mount, umount and
umount2 reject calls on a private view. A task lacking scoped authority gets
EPERM; an authorized task gets EOPNOTSUPP. The check precedes legacy dispatch,
including the NFS user-emulation path, so these operations cannot create
untracked grafts or enter legacy NFS emulation. Calls without a private view
retain their previous behavior.

The owner test constructs initial, sibling and descendant user namespaces.
It checks effective-capability drops, parent-owner authority, denial toward
ancestors and siblings, explicit clone ownership and owner-credential lifetime.
The owned snapshot deliberately captures an owner credential with SYS_ADMIN
removed, proving that a different authorized task can still administer its
user namespace. Native mount credentials remain separate throughout.

The process fixture separately rejects a foreign account and a foreign owner
without changing root/cwd. Its Linux probe attempts tmpfs, bind, remount, NFS,
normal unmount, force unmount and lazy detach, then repeats the view/filter
checks across fork/exec, signalled-child reaping and orphaned-child exit.
The corresponding Linux controls skip the lx-specific unsupported-operation
checks while exercising the same updated process, exec and lifetime code.

No Linux namespace creation syscall is enabled by this checkpoint. Public
creation checks and tracked mount operations, per-mount limits, namespace
descriptors, rename handling and PID/IPC/network isolation remain incomplete.


VM86 passed the owner case four times across the backend and canonical
user-namespace drivers. The quota case passed four times and both native
zone-quota cases passed. All eight static/dynamic UID0/65534 process cases
passed, including four foreign-account and four foreign-owner rejections.
Sixteen probe checkpoints exercised 112 blocked operations across parents,
exec children and orphan lifetime checks. Authorized UID0 calls returned
EOPNOTSUPP and UID65534 calls returned EPERM; each group reported zero
failures, then repeated the private-view and filter checks. All four updated
Linux process controls also passed.

The full enforcement and user-namespace regression drivers passed. All views
retired, process idle checks observed zero charges, both experimental switches
were reset to zero and the zone halted. Pool export and filesystem sync
completed before QEMU exited. Brand and namespace-test modules were
clean-built for release/debug; runtime used release on the retained mixed
VM73 platform with VM85 lx_proc/lx_support. No production host was changed.

The actual Codex 0.166.0 sandbox still requested clone 0x78020011 twice and
received EINVAL with both experiments enabled. Public namespace creation and
tracked private mount operations remain unfinished. See
lab/mount-owner-results.json and lab/mount-owner-linux-results.json for exact
source, artifact, controls and evidence hashes.
## Experimental public mount-namespace entry

With lx_mntns_experimental enabled, amd64 unshare accepts CLONE_NEWNS alone
or with CLONE_NEWUSER. Mount-only entry requires CAP_SYS_ADMIN in the caller's
current user namespace. A combined request prepares the new user namespace
first and checks authority there. The actor's native privileges remain
unchanged for mount-only entry and reduced for user-namespace entry; private
mount construction uses a separate kernel-only zone credential.

The first private tree imports the held zone root and its visible mount
instances using the zone's known global root-path prefix. It does not try to
resolve a global path through the caller's zone root. Trailing slash handling
preserves the native portal, proc, mntfs and device mount paths. A subsequent
private copy uses the current view's recorded mounts and takes a new explicit
owner credential.

Entry holds the old root, cwd, view and credentials, then prepares all new
references outside process locks. It revalidates membership, filesystem state,
credentials and feature switches under p_lock/p_crlock before changing the
process. Prepared root/cwd publication precedes an allocation-free credential
exchange, so a failed combined request cannot leave only USERNS published.
Old references are released outside the locks. Native-entry guard activation
for a previously unguarded task is limited to its current amd64 unshare call.
Remote unguarded attachment remains rejected.

Both features remain disabled by default. The previous USERNS-only behavior
and disabled-combination error codes are preserved. Entry currently requires
one LWP, no vfork, no CLONE_FS sharing and an unchanged root boundary;
the initial zone root or the current private view's canonical root is accepted.
A different chroot remains rejected. Mount entry also rejects tracing. Other namespace flags remain unsupported.
Private mount mutation still returns the explicit authority/unsupported errors
from the owner checkpoint. Tracked operations, per-mount limits, descriptors,
mountinfo, root rearrangement, rename handling and PID/IPC/network integration
remain work toward the full bubblewrap workflow.

The entry probe covers static/dynamic binaries, UID0/65534, mount-only,
combined and user-first entry, dropped SYS_ADMIN, seccomp retention, zero
mount/user quota rollback, inherited exec/cwd and native ABI rejection.
The native controller inspects stopped membership, private root/cwd, owner
capability and absence of native mount privilege after USERNS entry. It waits
for process exit and verifies both mount and user namespace counts return to
zero. The Linux controls use the existing lxc-unshare AppArmor profile and
restore dumpability after their explicit UID drop. No system-wide AppArmor
setting is changed.


VM89 passed the full prior enforcement, user-namespace, owner, quota and
process-lifetime drivers, followed by all 70 public entry cases. Ten cases
copied an existing private namespace again. The root mount-only cases also
forked a child with a different chroot and verified combined USER/MNT entry
still fails with EPERM. All 34 Linux controls passed with the same final probes.
Each public case waited for process exit and checked zero mount/user charges.
The actor retained its seccomp filter and private view through fork/exec, and
raw native entry received SIGSYS. USERNS entry granted no native SYS_MOUNT.

The first nested run in VM88 exposed a root-identity error. VN_CMP does not
compare vnodes with different vnode operation tables, so a valid private LOFS
root failed the initial-zone-root test. Existing private views now require
exact equality with their own canonical root. The initial zone boundary test
is unchanged. VM89 covers both valid nested copies and the chroot rejection.

The current brand and namespace tests clean-built in release and debug modes.
Runtime used release on the retained mixed VM73 platform with VM85 proc/support;
this does not establish acceptance of a complete rebuilt SmartOS platform.
All three experimental flags were reset, active views reached zero, the zone
halted, the pool exported and filesystems synced before QEMU exited. No physical
host was changed. See lab/mount-entry-results.json and
lab/mount-entry-linux-results.json for exact sources, hashes and evidence.

The Codex 0.166.0 sandbox still requests clone 0x78020011 twice and receives
EINVAL with all three experiments enabled. Public unshare acceptance is a
checkpoint toward the full sandbox workflow, not Codex sandbox acceptance.
