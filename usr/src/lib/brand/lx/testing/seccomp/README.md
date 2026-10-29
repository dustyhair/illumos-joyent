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
to start. It does not enable Codex's sandbox. `PR_SET_NO_NEW_PRIVS` and
`PR_GET_NO_NEW_PRIVS` also remain placeholders in this tree and need real
credential enforcement before they can satisfy sandbox requirements.

## Remaining implementation

1. Implement no-new-privileges semantics across exec and task creation, including
   native execution paths and credential-changing operations. State must be
   inherited and irreversible with Linux-compatible task scope.
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
