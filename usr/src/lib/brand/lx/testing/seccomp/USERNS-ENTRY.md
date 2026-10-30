# Experimental user-namespace entry

The amd64 Linux unshare syscall can now create a user namespace when the
lab-only lx_userns_experimental flag is enabled. Its default is zero,
retaining the existing EPERM fallback for unshare. The canonical offline
VM driver verifies that default before enabling entry and restores zero
on cleanup. No production host enables the flag.

Supported requests are CLONE_NEWUSER and CLONE_NEWUSER|CLONE_FS for a
single task whose filesystem state is already private. Other combined
flags return EINVAL. Flags retain their full native argument width, so
reserved high bits cannot be truncated into an accepted request. Requests
without NEWUSER retain EPERM. Chrooted entry returns EPERM. Multiple LWPs
return EINVAL. Existing shared-FS clone groups, vfork children, i386 and
alternate entry modes return EOPNOTSUPP. Experimental user-namespace clone is
described in [USERNS-CLONE.md](USERNS-CLONE.md). Other Linux namespaces and
setns remain unsupported.

## Credential publication

Entry captures the caller's actual immutable credential and process
credential under native locks. The existing namespace constructor checks
parent mappings, depth, quota and creator authority. It allocates an empty
namespace and the existing reduced native credential outside process
locks. New namespace capabilities follow the model's normal full effective,
permitted and bounding sets, empty inheritable/ambient sets and cleared
securebits. Native privilege sets remain bounded by priv_basic, with native
privilege awareness preventing backing UID0 from recovering privileges.

Before publication, entry rechecks task count, private filesystem state,
root directory, source credentials and the experimental flag. It replaces
process and task credentials and sets the irreversible LX_PROC_USERNS guard
under p_lock and p_crlock. Credential release occurs unlocked. Failed entry
publishes neither a namespace credential nor the guard. Incoherent native
real-UID accounting state returns EAGAIN rather than changing its count.
The ordinary path keeps all backing UID/GID/group values and native real-UID
accounting unchanged. There are no new process fields or brand_ops entries.

The guard uses the existing kernel-only Linux emulation path, independently
of seccomp. It blocks native/alternate syscall entry and native executable
transitions. Fork and ordinary Linux exec retain confinement. NNP and an
existing seccomp filter survive entry. The address-space owner's namespace
is retained until exec; entering a newer credential namespace cannot grant
access to an older nondumpable address space.

Existing proc map files provide mapping and setgroups policy. Before a map
is written, unmapped IDs read as overflow IDs. Mapping is parent-relative;
ordinary users can map their own backing identities after denying setgroups.
Initial UID0 mapping retains the existing creator SETFCAP/native authority
requirements. Neither entry nor mapping changes on-disk IDs or pool features.

## Controls

The entry probes call unshare directly. They do not use ktest credential
attachment. Static and dynamic probes cover initial root, an ordinary user
without parent capabilities, empty maps, mapping, explicit FS flag, nested
entry, the depth 33 boundary, NNP/filter retention, fork/exec inheritance,
old address-space ownership, thread/chroot/shared-FS rejection, full-width
flag validation and failure atomicity. lx-only controls verify native exec
rejection and SIGSYS on int91 without an installed seccomp filter. Default-
off and zero-budget cases additionally execute a native child successfully,
proving that rejected entry did not leave a guard installed.

The Linux controls pass 13 cases per executable. Ubuntu's existing AppArmor
unprivileged-userns policy is recorded explicitly: ordinary entry succeeds
with zero effective capabilities and self-mapping is denied. That is a
policy-specific control, not proof of ordinary unrestricted Linux mapping.
The lx ordinary-user case separately requires full namespace capabilities
and successful self-mapping. No Linux host policy or sysctl is changed.
References:
https://github.com/torvalds/linux/blob/master/kernel/user_namespace.c
https://github.com/torvalds/linux/blob/master/kernel/fork.c
https://documentation.ubuntu.com/release-notes/24.04/

The private zone keeps its quota of 64 objects. Native deferred cleanup can hold
namespace references after waitpid returns. Running two depth 33 probes back
to back initially reached that quota in VM67. DTrace showed natural recovery,
and a delayed rerun passed without restarting the zone. A read-only native
ktest now waits up to 10 seconds for the actual zone-root used count to reach
zero after each probe. It neither raises the quota nor forces reclamation.
VM68 passed 15+15 entry cases, both idle checks, zero-budget controls and
both canonical drivers before the final high-bit argument correction.

The newest Codex sandboxed workflow is not yet accepted. Bubblewrap still
needs combined namespace clone entry and complete mount, PID, IPC and
network isolation. Native emulation gaps beyond this syscall remain. This
guarded entry milestone does not substitute the outer zone for Codex's sandbox.

## VM69 acceptance

Both canonical drivers pass without serial overrides on the final image.
Static and dynamic entry probes pass 16 cases each, including reserved high
flags, and the native root-budget idle checks reach used 0 after both probes.
Default-off and zero-budget controls pass in both executable variants.
Identity/affinity 70+70, scoped FSS 8+8, capability 19+19 and the prior ABI/i386/
NNP/BPF/filter/signal/fork/exec/thread/map/mount/budget/lifetime/native-control/
filesystem ACL/race suites pass. Linux entry 13+13 controls pass with the
explicit Ubuntu policy-specific ordinary-user variant described above.

The release module/probe hashes match staging. The private pool exports
successfully and the VM completes synced poweroff. Local evidence is
lx-sandbox/lab/evidence/boot-69-userns-entry-acceptance.log. Build and runtime
provenance is /build/RUNNING_TESTS.md on the builder.

Brand/probe/driver source 5c7e4352f0 passes clean release/debug brand builds;
native ktest source 2ba0b8b4e9 passes clean release/debug builds. Runtime uses
release objects; debug is compile-only. Common/unix 92390bf918 and the VM66
ZFS/proc/elf objects, Native tl600d/LOFSa8b/VM30 emulation libraries/VM46
support/FSS class remain over the mixed May platform. This is not a complete
platform rebuild or a Codex sandboxed workflow acceptance. No physical host,
boot tree, production Debian zone, pool format or pool feature is changed.
