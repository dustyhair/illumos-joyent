VM100 accepts readonly setup for fresh private LOFS binds.

All eight lx cases and matching Linux controls passed across static/dynamic
binaries as root and nobody with allowed/denied capabilities. The full prior
regression wrapper also passed, including 70 entry cases, 12 binds, two record
limits, 12 recursive/device cases, eight private/slave cases, 12 tmpfs cases
and four restricted filesystem-statistics cases.

A private bind can be sealed readonly before potential target write exposure.
The existing LOFS instance lock fences that transition against writable opens,
shared writable mappings and mutators. The seal tightens primary and derived
VFS flags and future derived instances. Native mount ownership is checked
without changing options on refusal. Generic public LOFS remount stays disabled.
The kernel-only private constructors preserve NOMNTTAB before publication.

Coverage includes source writers through a different mount, readonly flags,
separately sealed recursive child mounts, write and metadata refusal, noexec,
readable existing O_CREAT files, missing-file refusal, symlinks, DAC checks,
file roots, private COW mappings, copied namespaces and removed capabilities.
Linux directory removal checks readonly parent policy before mounted-child
EBUSY. The existing cwd-preserving native removal mode now supplies that order
to both lx rmdir and unlinkat(AT_REMOVEDIR). Ordinary native RMDIRECTORY behavior
is unchanged. The mounted-child assertion was preserved and expanded with
empty-directory and fd-relative cases.

This accepts immediate bubblewrap setup, not general Linux remount support.
After any potential target writable-open/shared-map/mutator exposure, a new
seal remains EOPNOTSUPP even after references close. Readonly-to-readwrite is
unsupported. The separate full Linux late-remount reference is preserved.
No late writer or mapping lifetime guarantee is claimed for lx.

Both final idle checks passed. Views and experiment flags were zero, the zone
halted, the pool exported, filesystems synced and QEMU exited. Source and
artifact hashes are in mount-freeze-results.json. Clean release/debug builds
passed for genunix, brand and LOFS; runtime uses release modules with retained
platform components. Debug was compiled only. No physical host changed.

Codex0.166.0 still gets EINVAL twice for combined clone0x78020011 with the
experiments enabled. Lazy detach, root switching, private proc/devpts, mount
metadata, remaining namespace creation and complete coding/PTY acceptance
remain unfinished. The lazy-detach draft and Linux root-sequence controls were
kept outside this accepted image.
