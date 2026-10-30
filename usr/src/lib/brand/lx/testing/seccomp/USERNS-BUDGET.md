# Native lx user-namespace budget

Each lx zone now has a native budget for child user-namespace objects. The
initial namespace owns the shared maximum and usage count. Every descendant
charges that same budget, regardless of nesting or creator ID. The initial
namespace itself is excluded. The default is 1024; zero prevents new objects.
An exhausted reservation returns ENOSPC before allocating the namespace.

Failed construction returns its reservation. Credential copies and other
references do not charge again. An object remains charged until its last
reference goes away and its memory is freed. Parent references keep the
accounting root alive during cascading destruction. No zone pointer is held
in the namespace object, avoiding a zone/root reference cycle.

Allocation, limit changes and usage snapshots use the initial namespace's
existing mutex. Allocation releases that lock before taking a descendant's
map lock or allocating memory. The change adds two private 32-bit fields and
no common zone, credential or LWP fields.

This is an additional native aggregate policy, not Linux per-user ucounts or
an implementation of /proc/sys/user/max_user_namespaces. Public namespace
entry remains disabled.

## Configuration

The optional zonecfg attribute is max-user-namespaces with type uint. For a
zone without the attribute, a configuration example is:

```
add attr
set name=max-user-namespaces
set type=uint
set value=1024
end
```

The boot helper validates the unsigned value fits in 32 bits and applies it
through the existing native zone attribute API. Values outside that range or
with the wrong type fail configuration validation. Native zone configuration
authority controls writes. The normal illumos rule still applies: attributes
can be changed while the zone is ready, before it runs. Configuration edits
take effect at the next boot; no live mutation exception was added.

LX_ATTR_USERNS_MAX reads/writes a uint32_t maximum. LX_ATTR_USERNS_USED reads
a uint32_t usage count and rejects writes. Usage remains readable while the
zone runs. Lowering the maximum does not revoke existing objects; subsequent
reservations fail until usage falls below it. In-flight reservations count
against usage as well as fully constructed objects.

The rctl registration interface does not support module unloading;
os/rctl.c explicitly documents that limitation. Existing lx zone attributes
provide configuration without permanent common-kernel callbacks into a
loadable brand. The limit and accounting state remain in their owning brand.

## Validation

Core source and boot support are 93507d57a5. Kernel test include and controller
build fixes are d6149b4432. The final controller/driver are bfb6bd809b, which
respects the native READY/RUNNING attribute restriction. The original
controller incorrectly tried mutations on a running zone and encountered
EINVAL before lx's callback. No common zone syscall policy was changed.

Clean release/debug lx_brand and namespace ktest builds pass. lx_support and
the native controller build successfully; the controller uses GCC warnings as
errors. The private SDK lacked libzonecfg, so libzonecfg.so.1 was seeded from
the builder runtime into the isolated proto only, SHA256
c4bbf8b55624669b98e80684038b2fa66a67b5beb4ae9757bc9cc5658063a628.
No host library or package was changed. This is not a full platform rebuild.

Kernel tests cover default/zero budgets, nested aggregate charging, attempts
to set a child's budget, failed-model rollback, extra references, cascading
release, lowering below live usage and reuse. A concurrent test runs 32 rounds
of 16 allocation requests with four slots and four taskq workers. Each round
must retain exactly four objects, reject the rest, then return usage to zero.

The native controller tests configuration readback, short/invalid buffers,
read-only usage, and EPERM after a forked native process drops its privileges.
It sets a ready zone's limit to zero and checks actual backend denial. It then
sets two, observes two live objects held by a kernel test, lowers the maximum
to one, and verifies that the objects survive while another allocation fails.
After their release it sees zero usage and restores the original maximum.
After boot it verifies zonecfg's limit of 64 and EINVAL for a running-zone
mutation, leaving the value unchanged.

VM45 passed the revised controller and both full probe variants after
hash-verified controller/script uploads. The new boot helper was correctly
updated inside usr.lgz, which SmartOS mounts over /usr. Its runtime SHA256
matched the build. The pool exported and normal poweroff synced filesystems.
VM46 is the final full-image run, with no serial overrides. Both full drivers
exit zero. All four budget tests pass, including the ready-zone controller
and running-zone readback/rejection checks. Static and dynamic probes each
pass all 42 scoped identity/exec/ownership/peer cases, 19 capability/exec cases,
the inherited-tracer detach control and seeded SID check. The post-zonehalt
credential lifetime check also passes.

The previous backend/mount, map-file, capability ABI/i386 and NNP/BPF/filter/
signal/fork/exec/thread regressions pass. Actual Codex 0.166.0 still prints its
version under the scoped zero-capability filter. No normal sandboxed coding
workflow is claimed. Runtime module, helper and retained emulation-library
hashes match staging. The pool exports and normal poweroff syncs filesystems.

Relevant runtime SHA256:

```
lx_brand   85a2d133b2d7773759cfa715972d8a6c0cb28f4f35a94d823298e1b11ef14221
ktest      75f256b5e1e38c458005f1d3c673acdbd08a0bf9d102c54628f92fa14065dd5d
support    d22d8501f7e577572724dde12f3ff5941c20d2759f5cad7063a0ca5cf6bf7a76
controller 8aba48f010fc954d44fbfe1b9c493ca3c173f32aed09253e3fb5e9682c7d14e4
```

The runtime retains common kernel2676283bce, proc65b024e439, LOFSa8b and the
VM30 lx emulation libraries over the mixed May platform base. Only lx_support
was replaced in the nested /usr image for this checkpoint. Debug was compiled,
not runtime tested. SPARC and the complete platform were not rebuilt.

Local evidence is lab/evidence/boot-46-native-userns-budget-acceptance.log;
builder provenance is /build/RUNNING_TESTS.md. The launcher is
lx-sandbox/launch-lab, SHA256
ece2f8445e5745dd320cc1f837a51ca7dc206f9798292dfdb41f71ddffc16739.
Production hosts and the unrelated GPU workspace remain unchanged.

## Remaining work

Mount/PID/IPC/network namespace objects still need their own resource and
lifetime controls. Linux per-user namespace quota semantics, credential and
resource permission checks, filesystem-ID splits, SCM_CREDENTIALS and other
remaining interfaces are not implemented by this budget. User/mount namespace
entry and Codex's normal namespace-based sandbox remain unavailable. A current
stable installation and a real sandboxed coding workflow are still required,
followed by a complete platform validation before any host deployment.
