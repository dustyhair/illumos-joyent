# Observed BP exporter: research only

This is a read-only observation stream, not an inventory completeness certificate,
checksum validator, relocation implementation, or evacuation admission decision.
It does not change on-disk formats or enable removal.

## Invocation and isolation

After independent review, the lab owner may invoke:
    zdb-inventory -J /absolute/new-output.jsonl -e -p /explicit/lab/device-directory pool

Only -J, one -e, and one or more absolute -p directories are accepted. Pool names
cannot name a dataset/snapshot. No rewind, missing-log relaxation, arbitrary
tunables, checkpoint substitution, raw I/O options or mixed legacy dump modes.
Discovery must identify an EXPORTED pool; active-pool acceptance is disabled.
The operator must independently ensure no other system can write these devices.
Exported labels/read-only opening do not prove quiescence across hosts.

Use runtime/launch.sh (via /bin/bash) from the verified review bundle, not the bare binary. No Python installation is needed for this native launcher. The
traverse_no_zil_supported symbol deliberately prevents an old library silently
ignoring the new traversal flag. Other userland dependencies come from the
verified baseline build. No pool was opened on builder during implementation.

## Stream contract

The output file is opened with O_EXCL, O_NOFOLLOW, mode 0600. Existing files and
symlinks are refused. Record limit: 1,000,000 BP/dnode events. Alarm: 300 seconds
starting before kernel_init/discovery. ARC limit remains 256 MiB. These are
bounded research defaults, not a hostile-pool resource-exhaustion guarantee.

Require a successful process exit AND a valid final end record with
export_complete=true/error=0. Reject truncated JSON, missing begin/end, write
failure, count mismatch, timeout, signal/crash or any nonzero error. A failed
write may prevent an end record. Partial output is retained for diagnosis.
export_complete only means the attempted stream terminated without a reported
error. Unsupported records can deliberately prune subtrees and are counted.
It NEVER means complete pool traversal/ownership/validation.

Every production record explicitly has coverage_complete=false,
payload_validation_performed=false, evacuation_authorized=false.

64-bit numeric values use decimal strings (signed bookmark level likewise).
All 128 bytes of a BP are retained as sixteen fixed-width hex words in host
normalized order: these are NOT literal disk bytes/endian ordering. Repeated
references remain separate records. String bytes outside printable ASCII and
quotes/backslashes are escaped as U+00xx; consumers can reverse via Latin-1
bytes rather than infer original UTF-8 normalization.

BP records include bookmarks, birth/size/type/checksum/compression fields,
dnode context, metadata flag and DVA slots. No payload checksums are verified.
Embedded payload size is distinct from ordinary PSIZE; embedded and protected
BP payload words are not treated as ordinary DVAs. Gang raw data is retained
but no gang ASIZE interval or contiguous-header assumption is constructed.
P, Q and A are reported independently only for ordinary valid RAID-Z geometry.
No intervals are emitted by this exporter. Geometry mismatch, unsupported
topology/class, gang, protected, dedup or unknown type prunes children and is
explicitly marked children_skipped/unsupported.

Pool provenance includes GUID, selected synced uberblock TXG/root BP and
top-level vdev GUID/ID/type/ashift/children/parity. Bookmark provenance does not
claim resolution of each dataset's head/snapshot/clone owner class.

## Coverage and error behavior

Uses traverse_pool with PRE, NO_DECRYPT and explicit NO_ZIL; never HARD.
Retains MOS/dataset metadata and the revision's snapshot birth pruning.
ZIL is deliberately not attempted: existing ZIL error/intentional termination
semantics are not changed. The added gate is opt-in; the legacy traversal code
is otherwise byte-identical, tested by source comparison.

No DDT ownership walk, bpobj/deferred-free queue, async-destroy tree,
checkpoint-only ownership, removal/obsolete accounting or allocation map /
unflushed log-space-map reconciliation is attempted. MOS container observations
do not imply BP-shaped payload records were followed. These remain explicit
coverage gaps. A callback/metadata traversal error propagates to end/error and
the process exit status; unsupported pruning is separately visible.

The existing Python inventory_certificate model remains synthetic-only. Do NOT
label these observations synthetic or fabricate validation/ownership assertions
to feed that model. No real-input certificate adapter has been implemented.

## Build and test evidence

Targeted native GCC 10 build uses recorded baseline compiler/linker commands.
Only zdb, zdb_il, exporter and dmu_traverse are compiled; libzpool is relinked
with private copies of unchanged baseline objects and regenerated DTrace
metadata. Neither baseline source nor baseline object files are modified.
See /build/workspaces/raidz/inventory-build/build.log and build-provenance.json.
Build script and baseline command manifest are retained alongside those files.

Pool-free C harness uses real pinned BP macros and vdev_psize_to_asize as the
geometry oracle for 256 payload sizes. Python tests cover raw decoding, exact
uint64, JSON escaping, metadata, duplicate provenance, unsupported layouts,
misalignment/overflow/unknown-vdev geometry, exclusive output, incompatible
options, injected traversal error, write failure, count limit and old-library
refusal. The traversal error is an executable test stub, not real corrupt media.
Legacy regression proves the opt-in source gate leaves the old path unchanged;
legacy real-pool behavior and actual ZIL parsing were NOT exercised.

No source pool reads, no lab execution, no feature changes, no remote pushes.

## Independent-review fixes (second review bundle)

- Validate DMU object type before all type-dependent macros. Unknown type raw
  words survive; metadata is null and no DVAs are interpreted. All crypt-bit
  layouts, including indirect MAC layouts, are explicitly unsupported.
- Per-DVA geometry uses immutable BP classification; invalid earlier slots no
  longer suppress later valid slot observations. Whole-BP pruning aggregates
  slot results only after each independent evaluation.
- Report allocation_class, raw alloc_bias and is_log on vdev and DVA records.
  Only VDEV_BIAS_NONE with is_log false qualifies for ordinary geometry.
  Special, dedup, log and unknown classes are rejected, not silently ordinary.
- Discovery and import are separate testable boundaries. Stubs verify explicit
  paths, can_be_active=false, discovery error propagation, missing/active-state
  refusal before import, exported-state admission with zero import relaxation
  flags, and propagation of an import error.
- Flush/close failure tests assert a nonzero completion status. A terminal
  record can already contain export_complete=true when a later fclose fails;
  consumers MUST require successful process exit as well as a complete stream.
- The capability symbol is only an ABI safety guard, NOT library identity.

The runtime directory contains the exact executable, private ld.so.1 and 40
hashed library files. Both native launch.sh and optional Python launch.py pin
their generated manifests, hash every file, clear inherited target settings,
disable loader configuration, invoke the private loader directly and verify
its 38-library resolved startup dependency closure before execution. This
replaces reliance on a caller-supplied LD_LIBRARY_PATH or the capability marker.
A valid ELF alternative that still exports the true capability is refused by
hash. Changed executable, loader, dependency and manifest tests also fail closed.

The host kernel and launcher interpreter/utilities remain trusted. Keep the
reviewed bundle owner-protected and unchanged during verification/execution;
this is reproducible runtime selection, not protection against a hostile
same-user process racing file replacement. Startup dependency closure is
verified; future optional dlopen/plugin paths are not a sandbox or separately
attested by this prototype. Lab execution remains on HOLD for independent
review. No pool was opened during these tests.

Baseline tracked source and ISO hashes remain unchanged. Its filesystem is NOT
pristine: these two untracked build/header-test setup files were observed and
preserved without modification:
- usr/src/test/header-tests/tests/c-symbols/setup
- usr/src/test/header-tests/tests/cxx-symbols/setup
