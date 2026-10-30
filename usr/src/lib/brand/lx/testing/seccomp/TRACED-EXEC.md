# Scoped traced exec

Scoped AMD64 ordinary exec now constrains capability gains using the credential
recorded when tracing began. This closes an exec privilege gap before public
namespace entry can be enabled. It does not implement privileged file exec,
file capabilities, or the complete ptrace ABI.

The brand holds an ordinary illumos cred_t reference in the tracee's private
LWP data. PTRACE_ATTACH records the attaching caller. PTRACE_TRACEME records
the requesting child, after checking its proposed parent's authority. This
subtle difference follows Linux ptrace_link, which records current_cred for
both operations. The parent permission check uses its actual task credential,
not its process capability union. A scoped descendant cannot authorize tracing
an initial-namespace task. Existing unscoped TRACEME behavior is retained.

Fork inheritance holds the same snapshot. Detach, tracer exit, tracee exit and
failed attachment clear and release it. Tracer exit also suppresses a pending
ptrace SIGSTOP before restarting the tracee. Otherwise it becomes a permanent
job-control stop after the tracing relationship disappears. Credential release occurs outside
process and accord locks. A cleanup assertion checks that no tracer credential
remains when the brand frees an LWP. The appended pointer changes private LWP
size from 448 to 456 bytes, with old member offsets unchanged. No common kernel
credential field or ptrace framework was added.

The scoped exec callback checks CAP_SYS_PTRACE in the target user namespace,
including ancestor/owner rules and existing native administrative authority.
A weak attach credential constrains new permitted capabilities to the caller's
old permitted set, as no_new_privs does. Changed tracer credentials cannot
retroactively authorize the old attachment. An absent or detached tracer adds
no restriction. Ordinary exec can reset effective IDs to real IDs when Linux's
unsafe-gain rules require it; the candidate uses native SID helpers for those
changes. Successful capability gains or effective-ID changes keep core dumping
restricted through the normal post-exec brand callback. Failed exec does not
publish the candidate or change the old image's dumpability state.

## Validation

Implementation commits are 65b024e439, 17c409ae7a, 36bd646c0d and b044ab9763.
The final kernel fixture cleanup is dbc2210580. Tracer-exit SIGSTOP handling
is d7b6c1ee2c; the later 8bd739e84f changes only its comment. Linux reference source is
/build/linux at b4f0dd314b39: kernel/ptrace.c, kernel/capability.c and
security/commoncap.c. Live controls run on codex-testbox's Linux 7.0.0-38.

Clean release/debug lx_brand, lx_proc and namespace ktest builds pass. Static
and dynamic Linux probes compile with warnings as errors. Both pass all 31
identity/exec cases, plus the unscoped PTRACE_O_TRACEFORK inheritance control.
The initial control incorrectly assumed TRACEME records the parent's credential;
actual Linux behavior and source inspection established that it records the
requesting child. The implementation and controls were corrected together.

The added checks cover full and weak ATTACH credentials, effective capability
drops/restoration after attach, both TRACEME snapshot cases, detach and tracer
exit, weaker-parent rejection, task credentials versus process union, and
mixed-ID exec with and without a potential no_new_privs capability gain.
The inheritance control traces a fork, observes the inherited stop, detaches
the grandchild and lets its real parent reap exit status seven. The tracer
then reaps its own child. This validates credential hold/release at inheritance
and detach, not final-status routing for a non-native-child tracee. It runs outside the scoped fixture,
because guarded traced fork remains explicitly unsupported.

Build scope_ids.c with -std=c11 -Wall -Wextra -Werror -O2 -pthread and optionally
-static. The --linux mode uses the root-created disposable user namespace and
mapped backing identities documented in SCOPED-IDS.md. The --unscoped-trace
mode separately checks inherited ptrace attachment. Run both modes for both
binary variants. No host policy or sysctl changes are needed.

VM40 passed the previous regressions and the first 27 scoped cases, then
exposed the tracer-exit SIGSTOP bug described above. Its failed serial upload
left the console unresponsive. ACPI shutdown did not complete, so that
private VM was stopped uncleanly and its disk/console were preserved. VM41
started with a fresh disk. All 31 scoped cases then passed for static and
dynamic probes, as did both capability/exec suites and post-halt lifetime.
The unscoped fork control exposed EINTR during wait; b90a99183f retries it.
VM42 then exposed missing final-status routing for a traced process that is
not the tracer's native child. That remains unresolved. The d34deaf750 lifetime
control explicitly detaches the inherited grandchild before exit and passed
on Linux and in a hash-verified dynamic VM42 diagnostic run.
An explicit ISO-media copy and SHA256 check now prepares the actual Codex
fixture on fresh disks. VM41 exported its pool and powered off with filesystems
synced. VM43 is the final full-driver run on another fresh 8 GiB disk. Both drivers
exit zero without diagnostic bypasses. The ISO fixture preparation succeeds
and checks the actual Codex binary's SHA256. Static and dynamic variants each
pass all 31 scoped cases, their inherited-credential detach control, all 19
capability/exec cases and the seeded native SID check. The post-zonehalt
credential lifetime check passes. Actual Codex 0.166.0 still prints its version
under filtering after the helper verifies zero E/P/I, NNP1 and filter mode.
This is not a normal sandboxed coding workflow test.

The earlier eight backend suites, three procfs map cases, 11 capability ABI
cases ordinary/filtered, raw i386 boundary, detached mount views/snapshots and
17 NNP/67179 BPF/29 filter/31 signal/15 fork/7 exec/5 thread regressions pass.
No test skips are claimed as acceptance. The ISO filesystem unmounts, the pool
exports successfully and poweroff completes with filesystems synced.

VM43 module/probe provenance is brand d7b6c1ee2c, proc65b024e439,
ktestdbc2210580, probe d34deaf750 and driver a61523d350. Common kernel2676283bce,
LOFSa8b and the VM30 rebuilt lx libraries are retained over the mixed May
platform base. Debug was compiled, not runtime tested. SPARC was not built.
The user emulation libraries contain no lx_lwp_data references; the appended
kernel-only pointer does not change their interface.

New/relevant runtime SHA256:

```
lx_brand   4701eb37f80721e5a5a8c5c9fb319643fc8940507a4e9fe9669e56e52ec4c02a
lx_proc    2a498675b1f8373c1eca122b77c7bfa203833e34b7096df9845c957d144ec904
ktest      be2e30d01853527d02bdf434cd79e35a159ca9cbccd98aa302bbf1498856e8af
ids        55cc224c38be4b040e8c35da8067a327017de337acf7583f0ec2b0b9ec64b569
ids_dyn    8f9f12d2191b534669ed5e4b94335fafcfafeeda169c072c320984838d8b3685
codex      b8a11fedf24b2dfb59a70c9e52b030f9a508dcd49cd2dd0d41956f4c785156b8
```

Local evidence is lab/evidence/boot-43-traced-exec-acceptance.log. Builder
provenance is /build/RUNNING_TESTS.md. The launcher is lx-sandbox/launch-lab,
SHA256 ece2f8445e5745dd320cc1f837a51ca7dc206f9798292dfdb41f71ddffc16739.
Production hosts and the unrelated GPU workspace are unchanged.

## Remaining work

Public namespaces remain disabled. Guarded traced fork/shared-mm clone, native
procfs tracing policy, non-native-child ptrace final-status routing, privileged file exec, file capabilities, filesystem-ID
splits and other UID-bearing interfaces still need work. This checkpoint does
not establish full ptrace or exec compatibility. More lifecycle and namespace
reader tests are needed before enabling user/mount namespace entry and resource
controls. The unscoped inheritance control retries EINTR; ignored-signal
wait restart behavior remains to be audited separately. Full mount/PID/IPC/network isolation and a normal current-stable Codex
sandboxed coding workflow remain unvalidated. No production host is changed.
