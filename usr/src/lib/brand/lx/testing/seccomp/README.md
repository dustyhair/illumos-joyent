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
no syscall enforcement caller. Both `prctl` and `seccomp` continue rejecting
filter installation. This change does not enable Codex's sandbox.

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

## Remaining implementation

1. Extend no-new-privileges validation to i386 and the remaining native
   privilege paths, and validate a complete platform build before deployment.
2. Add seccomp program copy-in, immutable filter lifetime, inheritance,
   stacking, action precedence, signals, Linux errno mapping and thread
   synchronization. The BPF core alone provides none of these.
3. Close native execution and context-restoration escapes before accepting
   any filter. `lx_syscall_enter` currently hands some calls to user-space
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
