# Kernel clock nanosleep

glibc usleep and clock_nanosleep can now stay in the lx kernel execution
path. Previously syscall 230 dispatched to the Linux emulation library,
which a seccomp-filtered or user-namespace process cannot enter. VM71
exposed this as SIGSYS when the paced ownership-race probe called usleep.
Syscall 267 also has a kernel handler for the i386 time32 timespec layout.

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
18 cases covering clock/timespec validation, error ordering, ignored flags,
unused remainder pointers, minimum elapsed time, past/absolute deadlines,
signal interruption, SA_RESTART, bad remainder pointers, very large intervals,
aliased arguments and libc usleep/clock_nanosleep. Linux controls pass 18+18.

clock_sleep32.S is a standalone i386 executable with an allow-all filter.
It checks all three relative clocks against monotonic elapsed time, all
three past absolute deadlines, the time32 remainder with an adjacent canary,
validation, bad pointers, unsupported clocks and ignored flags. It passes
on Linux. It needs no multilib libc or development packages.

The canonical offline enforcement driver runs all three executables. The
userns driver additionally exercises libc sleep inside a scoped process in
the ownership-race probe. That probe retains its required observed stale
grant and late rejection checks; a run without the observation still fails.

Clean release/debug brand builds pass at source 7f97b706ec. VM72 acceptance
is pending. Common/kernel source 5bcd036dac, native ktest 4ba3707890 and the
retained platform components remain mixed. Runtime is release-only; debug
is compile-only. No host clock, physical host image or production zone is
changed. Controlled wall-clock adjustment and suspend/resume are not runtime
claims from these controls.

Linux syscall behavior reference:
https://github.com/torvalds/linux/blob/master/kernel/time/posix-timers.c

Native implementation references are usr/src/uts/common/os/condvar.c and
usr/src/uts/common/os/timers.c in this workspace. No Linux implementation
code is copied into the CDDL source.
