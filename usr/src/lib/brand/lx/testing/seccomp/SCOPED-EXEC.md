# Scoped ordinary exec checkpoint

AMD64 scoped tasks can now execute ordinary Linux programs through the existing
illumos kernel ELF loader. Public namespace entry is still disabled. This is
one prerequisite for Codex's normal sandbox, not full sandbox acceptance.
Later ordinary traced-exec checks are recorded in
[TRACED-EXEC.md](TRACED-EXEC.md); the VM36 results below are retained.

The common exec path uses the calling task's credential for access checks and
credential preparation. An appended brand callback validates each executable
and prepares an unpublished credential at level zero. The lx callback applies
the existing Linux capability model, keeps native privileges limited to basic,
and replaces the candidate's immutable brand payload. Successful publication
updates the remaining task and process under the scoped credential locks.
Previous credential references are released after unlocking. Failed exec never
publishes the candidate. No Linux namespace fields were added to common exec
or the credential structure, and no new per-LWP state was added.

Ordinary exec handles root, bounding and ambient capabilities, NOROOT,
no_new_privs and clearing KEEPCAPS while retaining its lock. Set-id files are
allowed when NNP or the filesystem suppresses set-id effects. Other privileged
file transitions return EOPNOTSUPP. Native Solaris ELF execution still returns
ENOEXEC, and native entry remains prohibited after exec. UID/GID and group
mutations were unsupported at VM36; the later AMD64 implementation and its
limits are documented in [SCOPED-IDS.md](SCOPED-IDS.md).

Scoped security.capability writes now return EOPNOTSUPP. They cannot use the
legacy initial-namespace root path that reports success without storing the
attribute. Actual file capability storage and execution remain unimplemented.

## Validation

Common kernel source is `2676283bced58c5abaabf8cd12277e723ca5bcbf`.
The additional scoped xattr boundary and driver are `5edff52308`.
Final probe source is `5f83644b0a`; `ccec1e3552` corrects its Codex path.
Clean release/debug unix, genunix, lx_brand, lx_proc, elfexec and namespace
ktest builds pass. Linux probes compile with GCC warnings as errors.

All 13 common scenarios pass on Linux for both static and dynamic binaries.
The control uses the existing bwrap AppArmor profile without changing policy:

```
aa-exec -p bwrap -- unshare --user --map-root-user --pid --fork --mount-proc \
    ./scope_caps --linux ./scope-setid/scope_caps
```

Prepare scope-setid/scope_caps as a copy of the static probe with mode 4755.
In this environment, direct bwrap execution inherits NNP1 and cannot establish
the non-NNP exec controls. The aa-exec/unshare control starts with NNP0. No sudo access or
AppArmor policy modification was used.

VM35 passed the older regressions and 18 of 19 scoped static cases. Its Codex
case incorrectly executed the directory /test/codex-current. The corrected
path is /test/codex-current/bin/codex. No kernel change was needed for that
failure. VM35 exported its pool and powered off normally. Optional DTrace
inventory failed with a scsi.d translator/CTF error in the mixed image; no
tracing result is claimed.

VM36 passes all 19 scoped cases for each of the static and dynamic binaries,
with no skips. The common cases cover capability operations, user-fault
preservation, task-local state, weak-caller clone/fork, ordinary exec capability
transitions, failed exec, and exec from a weak thread while a sibling retains
full capabilities. The lx cases check atomic privileged/native rejection,
retained native-entry denial, explicit unsupported ID/file-capability behavior,
and execution of actual Codex 0.166.0 under a filter denying IPv4/IPv6 sockets.
The helper verifies zero E/P/I capabilities, NNP1 and filter mode after exec
before launching Codex, which prints its version and exits zero. This is not
an authenticated or normal sandboxed coding workflow test.

The canonical userns driver now extracts and runs both probe variants. The
previous eight backend suites, three map-file baseline scenarios, 11 capability
ABI cases ordinary/filtered, raw i386 boundary and prior 17 NNP/67179 BPF/
29 filter/31 signal/15 fork/7 exec/5 thread checks also pass. Credential lifetime
checks repeat successfully after zone halt. Runtime hashes match staging.
The final pool export and synced poweroff complete normally.

Runtime SHA256:

```
unix       319c47af2199b54272a9ef1e07886a39ee8dc359b417691e33235c0960b2ade6
genunix    6a20312937fbda56e57323f8ac97626d1a02567876b6ce15be0e4bec5fb2fb33
lx_brand   c60f8beebb882f5ccc1b9e84663950fb4870481ebe43137c9f19bab0aa6f715a
lx_proc    0778682f47e3fc9a019037fad10fcb94889f4678001483eb030cf0f3d1260feb
elfexec    91ba0e910697d4c919ece7a1bc30dd060d958bb88039d646c1e5e8536678b687
ktest      7b5c98a8a352c5a0100b2e71d128378fc098d1d9c281c3be61fc9dd291923068
lib32      2bfe10d818552080ae4bd231888d916e4daee297acf40942e3b65d6c9523d38a
lib64      20cdf12237041cc84cead937066816fe33ff0bec7d7e960a89445c45d0bc70c0
scope      140d0c43c98bac7cdf1dc9fd4da6e81c9158a0e8844610b2170d7d7cbb167d69
dynamic    5437d80b075f7517abb9e169b2d9bdb24f3e2205ba184eaca2ae4703b4cbaa37
codex      b8a11fedf24b2dfb59a70c9e52b030f9a508dcd49cd2dd0d41956f4c785156b8
```

The runtime is release modules over the mixed May platform base, retaining
LOFS a8b and the VM30 rebuilt lx libraries. Debug was compiled, not runtime
validated. SPARC was not built. Scoped exec remains AMD64 only; static and
dynamic tests do not mean separate instruction-set ABIs are supported.
The launcher is lx-sandbox/launch-lab, SHA256
`ece2f8445e5745dd320cc1f837a51ca7dc206f9798292dfdb41f71ddffc16739`.
Local evidence is lab/evidence/boot-36-scoped-exec-acceptance.log; builder
provenance is /build/RUNNING_TESTS.md. Production hosts and zones are unchanged.

## Remaining work

Public namespace entry, remaining ID-bearing interfaces and filesystem IDs,
remaining procfs/lifecycle checks, privileged exec, file capabilities,
and complete mount/PID/IPC/network isolation remain unfinished. Acceptance
here covers untraced, identity-mapped lab fixtures. The normal Codex sandbox
still cannot create the namespaces it requests. A current stable install and
a real sandboxed coding workflow, including denied operations, plus a complete
platform validation are required before host deployment.
