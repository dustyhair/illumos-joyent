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

## Remaining implementation

1. Extend the no-new-privileges validation to i386 and the remaining native
   privilege paths, and validate a complete platform build before deployment.
2. Implement seccomp program copy-in and classic BPF validation, then evaluation
   at the syscall boundary. Preserve the Linux ABI's argument byte order,
   architecture, instruction pointer and syscall number. Handle i386 and x86-64
   and reject unsupported entry paths safely. Network-packet BPF evaluation is
   not a drop-in seccomp implementation.
3. Add filter lifetime, inheritance, stacking, action precedence, errno mapping,
   signals and thread synchronization. Ensure brand escape and native syscall
   paths cannot bypass a filter. Test these before reporting filtering as supported.
4. Add user and mount namespaces, UID/GID maps, namespace descriptors, mount
   propagation and the remaining namespace operations actually used by the
   installed bubblewrap build. Reject unsupported isolation requests explicitly.
5. Test the installed Codex sandbox end to end, including forbidden filesystem,
   network and process operations. The legacy path additionally needs an audit
   of its filesystem restrictions, including Landlock if that version uses it.

The current source locations are:

- `usr/src/uts/common/brand/lx/syscall/lx_prctl.c`
- `usr/src/uts/common/brand/lx/syscall/lx_miscsys.c`, `lx_unshare`
- `usr/src/uts/common/brand/lx/os/lx_syscall.c`, syscall tables and dispatch
- `usr/src/uts/common/brand/lx/os/lx_brand.c`, process and LWP lifecycle
- `usr/src/uts/common/brand/lx/sys/lx_brand.h`, brand state

Linux filter semantics are documented at
https://docs.kernel.org/userspace-api/seccomp_filter.html.
