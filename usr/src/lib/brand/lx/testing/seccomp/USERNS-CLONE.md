# Experimental user-namespace clone

The amd64 Linux clone syscall can prepare a new user namespace and install
its credentials in the child before the child becomes runnable. The existing
lab-only lx_userns_experimental flag controls both clone and unshare entry.
Its default remains zero. This does not yet run Codex's complete sandbox.

Supported clone requests contain CLONE_NEWUSER, an exit signal of SIGCHLD or
zero, and optional parent/child TID flags. The child may use a new stack or
the copied Linux stack. Other namespace flags, shared-memory/thread flags,
TLS changes and reserved high bits are rejected before namespace allocation.
The parent must currently be single-threaded, outside a shared filesystem
clone group, and not chrooted. i386 and alternate entry modes are unsupported.
These are implementation limits, not claims about Linux clone semantics.

## Native fork integration

Namespace construction shares unshare's credential preparation routine.
It uses the actual caller credential, validates namespace maps, depth and
quota, then allocates the namespace and reduced native credential outside
native process locks. Preparation does not change the parent credential,
namespace, no-new-privileges state or execution guard.

One kernel-only pending credential pointer is appended to lx LWP data.
It is never stored in user-controlled syscall arguments or passed through
the Linux emulation library. The existing native fork hook installs the
prepared process/task credentials and LX_PROC_USERNS guard in the SIDL child,
before publication. Native real-UID accounting keeps the same backing UID.
The pending reference is cleared and released after either fork success or
failure; LWP destruction also releases it on a nonreturning error path.
The child does not inherit the pending pointer.

The child returns through the existing kernel Linux fork path. It inherits
NNP, seccomp and the copied address space's older owner namespace. Native
executable transitions and alternate syscall entry are blocked independently
of seccomp. The parent keeps its original native execution access. There are
no new common brand operations, ZFS pool features or on-disk namespace IDs.

## Validation

Static and dynamic probes call clone directly, without ktest credential
attachment. Each lx executable has 12 cases covering first child credentials,
the unchanged parent, raw clone with the copied stack, repeated clones,
parent/child TID writes in separate address spaces, ordinary-user mapping
brokered by the parent, namespace descent, subsequent unshare, NNP/filter
inheritance across exec, old address-space ownership, native-entry rejection
and atomic rejection of unsupported combinations.

The Linux controls pass 10 cases per executable. Ubuntu's existing AppArmor
policy permits an ordinary user's namespace creation with zero effective
capabilities; that policy-specific case is recorded explicitly. lx requires
the full namespace capability set and successful parent-brokered mapping.
The Linux host policy is unchanged. Linux source references are listed in
[USERNS-ENTRY.md](USERNS-ENTRY.md).

The offline driver checks default-off and zero-budget failures for both
clone and unshare, including native execution after rejection. A read-only
native test waits for actual namespace budget reclamation between probes.
The 64-object quota remains unchanged.

VM70 passes entry 16+16, clone 12+12, default-off, zero-budget and budget-idle
controls. Its full userns regression run stopped earlier because the dynamic
ownership-race trace did not observe a stale grant. The unchanged standalone
race passed on retry and observed stale grants rejected with EPERM. The probe
now pauses 1 ms between ownership calls to span native writer phases while
retaining the required stale-grant/rejection assertion. Final canonical image
acceptance with the paced probe is pending.

Common/unix/genunix/brand/proc/elf source 5bcd036dac and native ktest source
4ba3707890 pass clean release/debug builds. Runtime uses release objects over
the retained mixed May platform; debug is compile-only. Evidence and build
provenance are recorded in /build/RUNNING_TESTS.md on the builder and locally
in lx-sandbox/lab/evidence/boot-70-userns-clone-diagnostic.log.

Mount, PID, IPC and network isolation remain incomplete. Bubblewrap's combined
namespace clone request is still rejected. Full platform validation and an
actual current Codex sandboxed coding workflow remain required. No physical
host or production Debian zone has been changed.
