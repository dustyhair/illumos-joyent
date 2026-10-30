# Scoped native-backed identities

AMD64 scoped tasks now implement UID/GID getters, setuid/setgid,
setreuid/setregid, setresuid/setresgid and supplementary groups. The numeric
values in native credentials remain real backing identities. Namespace maps
translate syscall arguments, getter results, procfs status, auxiliary vectors
and stat ownership. Public namespace entry remains disabled.

The implementation reuses illumos credentials, native group storage, SID
helpers and real-UID process accounting. It adds no credential or LWP fields.
Each task retains its own immutable credential. A representative live task
supplies the native process identity; the process capability sets still cover
all live tasks. Publication reserves a new real UID's native process count
before swapping credentials, retries changed snapshots and releases the old
count after publication. Allocation and credential release occur outside
process locks.

Linux permission and saved-ID rules apply before preparing a native candidate.
UID transitions apply the existing capability/securebits model. Effective ID
changes clear dumpability. Native validation rejects backing IDs that the zone
cannot represent. Effective UID/GID changes replace or clear the matching SID,
and setgroups replaces or clears the supplementary SID list. The process
summary copies the representative's SID payload through native reference
counting. Shared original SID objects remain unchanged.

Groups use the native configured limit, 16 in the test VM. Larger requests
return EINVAL rather than being truncated. Separate filesystem UID/GID changes
remain EOPNOTSUPP. The current model uses effective IDs for filesystem access.
Scoped PR_GET_DUMPABLE/PR_SET_DUMPABLE support restricted mode zero and ordinary
mode one; mode two is not implemented.

## Validation

The identity implementation starts at 9847ca5122, with build and fixture fixes
in 3b1675676a, 5ad7ea0231 and a2606b318e. SID hygiene and its seeded kernel test
are c911578999. Clean release/debug lx_brand, lx_proc and namespace ktest builds
pass. Common kernel source remains 2676283bce, LOFS a8b and the rebuilt VM30
lx libraries are retained over the mixed May platform base. Debug is compiled,
not runtime tested. SPARC is not built.

The trusted lab fixture maps visible IDs 0..2 to backing IDs 1000..1002 and
visible ID 42 to backing root. This exercises real nonzero native identities,
not just a root-to-root map. The fixture is limited to stopped, fresh,
single-LWP test programs in the private lx-probe zone. It is not a Linux API.

All 13 common scenarios pass on Linux for static and dynamic binaries:

- Mapped getters and privileged setters, rejected updates and atomicity.
- Saved IDs, setreuid/setregid rules, KEEPCAPS and NO_SETUID_FIXUP.
- Supplementary group mapping, sorting, permission and error ordering.
- Task-local identity changes, fork accounting and glibc thread broadcasts.
- Nondumpable self-procfs, mapped exec auxiliary vectors and capability drops.
- Actual file ownership, native read denial after an ID change, and restoration.

Compile scope_ids.c with -std=c11 -Wall -Wextra -Werror -O2 -pthread, with
-static for the static variant. Run --linux using the existing root SSH access
on codex-testbox. The parent creates and maps only a disposable child user
namespace. Put the binaries in a fresh root-owned, world-executable temporary
directory so the mapped nonzero identities can execute them. Remove it after
the run. No host policy or sysctl changes are needed. Linux suid_dumpable is two
on this control host, so the test accepts any restricted result other than one.

VM37 passes all 13 identity and 19 scoped capability/exec cases for each binary
variant, plus the earlier enforcement and backend regressions. Native upcount
buckets covering backing IDs 1000..1002 were empty after zone halt. This checks
these exercised identities, not all possible accounting races.

VM38 passes all 13 identity cases and all 19 scoped capability/exec cases for
both static and dynamic binaries, without skips. Both seeded SID checks pass:
the stopped parent has backing UID/GID 1001, its task and process credentials
have no stale user/group/supplementary SIDs, and the held original SID object
still contains the seeded entries. The final credential lifetime check passes
after zone halt. Both full drivers exit zero. Runtime hashes match staging,
the pool exports successfully and poweroff completes with filesystems synced.

The eight earlier backend suites, three procfs map baseline cases, 11 capability
ABI cases ordinary/filtered, raw i386 boundary and previous 17 NNP/67179 BPF/
29 filter/31 signal/15 fork/7 exec/5 thread checks also pass. Actual Codex
0.166.0 still prints its version after the exec helper verifies zero E/P/I,
NNP1 and filter mode. This is not a normal sandboxed coding workflow test.

Runtime SHA256:

```
unix       319c47af2199b54272a9ef1e07886a39ee8dc359b417691e33235c0960b2ade6
genunix    6a20312937fbda56e57323f8ac97626d1a02567876b6ce15be0e4bec5fb2fb33
lx_brand   03c1c8a2da3af0b01a90ece4fa9af56f579627c9e243aeb997af19df7e465b63
lx_proc    7cf638c9bdd9b10c175239204d8fe24ed93536a787c5012271a6faa758fcf781
elfexec    91ba0e910697d4c919ece7a1bc30dd060d958bb88039d646c1e5e8536678b687
ktest      c49085bbb674fa95dde75056a9d240ad088ca471a14702d7e64dffaee336f5b6
ids        f52eb0a1793227f1a98f9338104eac0b348ba354d190bc3e7c1c9c6d83fa75a5
ids_dyn    6958788d01eabfd873aaedbf0956e80c2335a26ca78df948f616c537859b00b5
caps       d5a7cb8d6b590b3156c9fd82690743bb5d4aedcf9b406dd72856ff3fef988791
caps_dyn   99cc58c8c9c9a1c3188c0103a4c6ac3d562b87550a52af0c6a9ce2d3cfa800aa
lib32      2bfe10d818552080ae4bd231888d916e4daee297acf40942e3b65d6c9523d38a
lib64      20cdf12237041cc84cead937066816fe33ff0bec7d7e960a89445c45d0bc70c0
```

Local evidence is lab/evidence/boot-38-scoped-identities-sids.log. Launcher is
lx-sandbox/launch-lab, SHA256
ece2f8445e5745dd320cc1f837a51ca7dc206f9798292dfdb41f71ddffc16739.
Builder provenance is /build/RUNNING_TESTS.md. Production hosts and zones are
unchanged.

## VM39 concurrent snapshots

Probe commit 5175d4d108 adds a fourteenth scenario. Two workers each perform
128 raw UID/GID/group update cycles while the main task reads all three task
status files for at least 128 rounds. Each UID/GID row must contain one complete
credential's four ID fields. Each worker retains its own identity, and the
main task's IDs and groups stay zero. NO_SETUID_FIXUP preserves the workers'
capabilities during these transitions; glibc broadcasting is tested separately.
Workers remain alive until sampling ends, so disappearing task directories
cannot hide a failure. This covers simultaneous publications and reads, not
all possible lifecycle races or namespace-relative readers.

All 14 cases pass on Linux and VM39 with static and dynamic binaries. VM39
retains the VM38 kernel/modules and reruns both full drivers successfully,
including both seeded SID checks and all prior regressions. No kernel change
was needed for the added test. The final zone halt, credential lifetime check,
pool export and synced poweroff succeed. Runtime probe hashes are:

```
ids        bc1e2c05121bb141b693b6f5b71b8f8f60cf53e62889d66786748a94d40183f0
ids_dyn    e0e3ed7d263b2a5f3b1d59efac4129da5727a3d3fb0c6ad37c9afd72b7cdd9eb
```

Console evidence is lab/evidence/boot-39-concurrent-identities.log. The same
mixed-image, ABI and deployment limits stated above apply.

## Remaining work

This checkpoint covers the listed AMD64 identity interfaces. Separate filesystem
IDs, remaining ownership-changing and UID-bearing interfaces such as peer
credentials and siginfo, dead-leader semantics, broader lifecycle and procfs checks, and traced/privileged exec remain
unfinished. Native ephemeral SID
lookup is reused but was not exercised against an idmap service.

User/mount namespace entry and complete mount, PID, IPC and network isolation
remain disabled or incomplete. Resource limits and namespace lifetime controls
also need acceptance. Codex 0.166.0 prints its version under filtering, but its
normal namespace-based sandbox does not run yet. A current stable installation,
a real sandboxed coding workflow and complete platform validation remain
required before host deployment.
