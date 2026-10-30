# Native authority for initial UID0 maps

Initial-namespace CAP_SETFCAP now projects the existing native FILE_SETID
privilege, reusing the file-execution metadata authority already used for
CAP_FSETID. This projects authority; Linux on-disk file capability attributes
and privileged file-capability execution remain unsupported.

The mapping does not itself authorize initial UID0 delegation. When decoding
an initial-namespace map actor, lx also calls the existing native
secpolicy_vnode_setid_retain(cred, B_TRUE) policy. That policy requires the
zone's complete effective privilege envelope before allowing setuid-root
metadata to be retained. A limited credential with FILE_SETID can have the
metadata capability but is not permitted to map initial UID0. This is an
additional native restriction on initial-root mappings. Scoped actors retain
the existing independent immutable Linux capability payload.

No native privilege is granted, no UID map root check is removed and no
common structure field or brand callback is added. The existing map model's
creator/opening-credential SETFCAP checks still apply. The policy call is
made only after FILE_SETID is present in the actual effective native set.

The private attachment fixture now decodes the stopped lx process's actual
native credential for namespace creation and map writes. It no longer
constructs a synthetic full-capability parent actor. Only after creating the
child namespace does it assign the new namespace's full scoped capability
set, as in ordinary Linux user-namespace creation.

## Native capability constraint

Initial native CAP_FSETID and CAP_SETFCAP share FILE_SETID. Requests to split
those permissions cannot be represented by native privilege sets and are
rejected atomically with EPERM. This follows the existing native SETUID/SETGID
coupling rule. Scoped capability sets remain independent; their native
privileges stay at the basic floor.

## Validation

Clean release/debug brand and namespace test builds pass. cstyle and
`git diff --check` pass. Source is57427c7bcd.

New native controls verify that full privileged-file authority maps initial
UID0, FILE_SETID plus PROC_SETID without the zone's full privilege envelope
cannot map initial UID0, rejection leaves the map writable for an authorized
non-root mapping and backing IDs remain correct. Native capset tests verify
that a rejected split of metadata authority leaves the credential unchanged.
Existing map security cases still pass.

VM54 full canonical drivers pass, including the new map_authority control,
actual-credential stopped fixtures, capability19+19, scoped52 static+52 dynamic,
capability ABI ordinary/filtered/i386 controls and prior map/mount/budget/NNP/
BPF/filter/signal/fork/exec/thread/credential lifetime regressions. Native
stream and native rmdir controls pass. Linux probe sources are unchanged;
their Linux52 controls remain from VM53. Actual Codex0.166 prints its version
under the scoped filter; no normal coding sandbox is claimed.

Runtime hashes match staging:

```
lx_brand 3072e915e6671edfd0e98c38eaee04b55ff094d8c69b6e06e25b5da77a9f472b
namespace_ktest fe3f2213f73286768966d158970fe95a25a341681b799405f396592586d11c95
```

Pool export0 and synced poweroff complete acceptance. Evidence is
lab/evidence/boot-54-native-map-authority-acceptance.log. Builder provenance
is /build/RUNNING_TESTS.md. Common7c59b0f249, Native tl600d5200ad, LOFSa8b,
VM30 emulation libraries and VM46 support remain over the mixed May platform.
Debug is compile-only. Launcher lx-sandbox/launch-lab SHA256
ece2f8445e5745dd320cc1f837a51ca7dc206f9798292dfdb41f71ddffc16739.

## Remaining work

Public user namespace entry remains disabled. Resource-scoped filesystem
capability permissions, separate filesystem IDs, mount/PID/IPC/network
isolation and other remaining syscall behavior are unfinished. On-disk file
capabilities remain unsupported. A newest stable installation and actual
normal sandboxed coding workflow and complete platform validation remain
required before physical host deployment. Production hosts and the unrelated
GPU tree remain unchanged.
