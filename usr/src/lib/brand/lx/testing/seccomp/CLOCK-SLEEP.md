# Kernel clock nanosleep

glibc usleep and clock_nanosleep can now stay in the lx kernel execution
path. Previously syscall 230 dispatched to the Linux emulation library,
which a seccomp-filtered or user-namespace process cannot enter. VM71
exposed this as SIGSYS when the paced ownership-race probe called usleep.
Syscall 267 also has a kernel handler for the i386 time32 timespec layout.

Modern glibc probes the amd64 rseq syscall during process startup. The LX
amd64 table previously ended at syscall 322, so strict processes recorded
syscall 334 as unsupported and delivered SIGSYS after the next successful
syscall. LX now returns ENOSYS for rseq without marking strict mode. This
matches a Linux kernel without restartable-sequence support and lets glibc
use its normal fallback.

The same strict-mode rule applies to two probes bubblewrap and glibc make
before confined payload execution. Legacy, non-user-namespace LX processes
now report their capability bounding set without logging PR_CAPBSET_READ as
unsupported; public user namespaces retain their tracked capability model.
The amd64 faccessat2 probe returns ENOSYS without marking strict mode, so
glibc can fall back to faccessat. Other out-of-range syscalls remain strict
failures.

Restricted processes also keep vfork inside the kernel. The LX path now uses
the native illumos vfork implementation. The child shares the parent's address
space and the parent remains suspended until the child calls exec or exits.
LX consumes the native vfork wait inside the brand dispatcher because the
active syscall is Linux vfork rather than native forksys. An unguarded process
with a pending child PID namespace retains the existing EOPNOTSUPP result.
Native illumos vfork suspends every parent LWP. The LX dispatcher resumes
sibling LWPs before it waits so Linux vfork suspends only the calling thread.
The same path accepts clone with CLONE_VM, CLONE_VFORK and SIGCHLD, which is
the process creation sequence used by current glibc posix_spawn.

The implementation reuses the native LWP delay condition variable and
signal-aware timed waits. It supports relative and absolute REALTIME,
MONOTONIC and BOOTTIME sleeps. BOOTTIME uses lx's existing monotonic clock
model because host suspension is not represented separately. Relative
REALTIME sleeps use monotonic elapsed time. Absolute REALTIME sleeps check
the adjustable wall clock through the native realtime wait path. Huge
monotonic intervals saturate without signed arithmetic overflow; huge
realtime deadlines use bounded native intervals.

Linux ignores flag bits other than TIMER_ABSTIME. Successful sleeps leave
the remainder untouched. Interrupted relative sleeps copy the remaining
interval, reporting EFAULT if that copy fails. Absolute sleeps ignore the
remainder pointer, including when interrupted. Caught signals return EINTR
even with SA_RESTART. Request/remainder pointers may alias.

CPU-clock sleeps have no native implementation and return EOPNOTSUPP.
RAW/COARSE clocks also return EOPNOTSUPP. Other unsupported clock identifiers
return EINVAL. This does not add CPU-clock, alarm-clock, TAI, time namespace,
i386 time64 or Linux restart-block support. Linux CPU-clock sleeps and encoded
process clock identifiers therefore remain compatibility gaps.

## Controls

The static and dynamic clock_sleep probes install NNP and an allow-all
seccomp filter before every case, forcing use of the kernel path. Each has
26 cases covering clock/timespec validation, error ordering, ignored flags,
unused remainder pointers, minimum elapsed time, past/absolute deadlines,
signal interruption, SA_RESTART, bad remainder pointers, very large intervals,
aliased arguments, libc usleep/clock_nanosleep, the rseq fallback and the
capability/faccessat2 startup probes. The vfork cases verify shared private
memory, parent suspension, successful exec, failed exec followed by exit,
child signal death, sibling-thread progress and libc posix_spawn. The
`--vfork-stress N` mode cycles N times through direct exit, successful exec,
failed exec and signal death under the same allow-all seccomp filter.

clock_sleep32.S is a standalone i386 executable. On Linux it installs an
allow-all filter. Its --lx mode requires the existing i386 filter installation
to return EINVAL, then tests the unfiltered time32 ABI. It does not claim
i386 seccomp support.
It checks all three relative clocks against monotonic elapsed time, all
three past absolute deadlines, the time32 remainder with an adjacent canary,
validation, bad pointers, unsupported clocks and ignored flags. It passes
on Linux. It needs no multilib libc or development packages.

The canonical offline enforcement driver runs both confined amd64 executables
and the explicit unfiltered i386 control. The
userns driver additionally exercises libc sleep inside a scoped process in
the ownership-race probe. That probe retains its required observed stale
grant and late rejection checks; a run without the observation still fails.

VM72 passes 17+17 sleep cases and exposes the remaining native errno mapping
error in validation: Native ENOTSUP maps to Linux ENOSYS, so unsupported clock
backends must return native EOPNOTSUPP. It also confirms existing i386 filter
rejection and exposes a driver log-creation race. These are corrected in
source 8271cbb3ef before the VM73 acceptance below.

VM73 now passes both canonical drivers on a fresh image without diagnostic
overrides. Confined amd64 sleep controls pass 18+18, and the explicit i386
unfiltered ABI control passes while confirming the existing filter rejection.
Paced ownership/mode race probes use libc sleep inside scoped processes and
pass all required stale-grant assertions. User namespace entry 16+16, clone
12+12, budget-idle/default-off/zero-budget controls and the previous regression
suites also pass. Runtime module and probe hashes match staging. Local
evidence is lx-sandbox/lab/evidence/boot-73-clone-clock-sleep-acceptance.log.
The private pool exports successfully and the VM completes synced poweroff.

Final brand source 8271cbb3ef passes clean release/debug builds; accepted
driver/probe source is 8a478f3e8b.

Common/kernel source 5bcd036dac, native ktest 4ba3707890 and the
retained platform components remain mixed. Runtime is release-only; debug
is compile-only. No host clock, physical host image or production zone is
changed. Controlled wall-clock adjustment and suspend/resume are not runtime
claims from these controls.

Linux syscall behavior reference:
https://github.com/torvalds/linux/blob/master/kernel/time/posix-timers.c

Native implementation references are usr/src/uts/common/os/condvar.c and
usr/src/uts/common/os/timers.c in this workspace. No Linux implementation
code is copied into the CDDL source.
