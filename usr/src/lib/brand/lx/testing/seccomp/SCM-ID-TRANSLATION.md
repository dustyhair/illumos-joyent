# Socket credential ID translation

This checkpoint uses existing native credentials, immutable lx namespace maps
and the native socket ancillary transport. It adds no common kernel structure
fields or privilege grants.

Explicit SCM_CREDENTIALS IDs are translated in the sender's namespace before
native transport. Unmapped IDs fail EINVAL. A claim must match the sender's
real, effective or saved native ID, or have scoped CAP_SETUID/CAP_SETGID.
The check uses the calling task credential. The current PID realm is native
zone-owned; a descendant user namespace's CAP_SYS_ADMIN is not authority to
forge a PID in that realm. Native initial-realm CAP_SYS_ADMIN behavior remains
subject to the limitations below.

Returned IDs use the receiving task's namespace. Automatic credentials use
real UID/GID. Explicit metadata populates both real and effective fields of
the existing native prcred_t, so that it follows the same return path. Native
zone-init PID is translated to Linux PID1.

All explicit credential records are validated. The last explicit claim is
forwarded, matching Linux. The native datagram transport appends default
credentials after explicit ones. Conversion returns the explicit record once
and suppresses those redundant defaults. The intermediate native-layout
buffer is bounded by its converted length during 64-bit header expansion.

## Validation

Linux controls pass47 top-level cases for each static and dynamic binary,
compiled with -Wall -Wextra -Werror. Four new cases cover explicit mapped
round trips on datagram/stream sockets, real/effective/saved IDs without
capabilities, scoped capability claims, repeated claims, rejection of forged
UID/GID and PID claims, unmapped IDs and rejected-message queue atomicity.

The initial VM48 image passed enforcement but failed three new cases because
native default credentials were also returned. The native transport source
confirmed that it appends defaults. Revision81dce15033 fixes that conversion;
probe581743cfa3 adds repeated-claim controls. VM49 passes both full drivers:
DRIVER_EXIT=0 and USERNS_DRIVER_EXIT=0, scoped47+47, capability19+19,
prior enforcement/backend/map/mount/NNP/BPF/filter/signal/fork/exec/thread/
ABI/i386 regressions and post-zonehalt credential lifetime. Actual Codex0.166
prints its version under the scoped filter; a normal coding sandbox is still
unavailable. Pool export0 and synced shutdown are required for acceptance.

Runtime lx_brand SHA256:
9db7d377728ad445f4398c864c1b6f1032ca98d2d829fe0bb0acd7a56da06184.
Clean release/debug brand builds and cstyle pass. Common2676283bce,
proc65b024e439, ktestd6149b4432, LOFSa8b, VM30 emulation libraries and VM46
support are retained over the mixed May platform base. Debug is compile-only;
no full-platform or physical-host deployment claim. Console evidence is
lab/evidence/boot-49-scm-ids-acceptance.log. Builder provenance is
/build/RUNNING_TESTS.md. The launcher remains lx-sandbox/launch-lab SHA256
ece2f8445e5745dd320cc1f837a51ca7dc206f9798292dfdb41f71ddffc16739.

## Remaining work

This is not complete Linux SCM or PID namespace emulation. In particular,
native initial-realm administrative PID overrides still need Linux target
existence, zone-boundary and PID-reference lifetime semantics. The VM49 automatic stream snapshot fallback has since been replaced by
the native per-message implementation in STREAM-CREDENTIALS.md. Cross-namespace queued
credentials, sender task races, ancillary truncation and 32-bit SCM-specific
controls need further coverage before enabling public namespaces. Native
ancillary layouts remain subject to the existing receive-side assumptions.
