# DDT-absent / canonical-empty-ZIL supported profile (v14 review hold)

`-Q new.jsonl -e -p /explicit/path pool` is an explicit opt-in diagnostic mode.
It includes the same paired allocation, pending-free, async-destroy and
original-BP verification evidence as `-Y`, plus a separate metadata absence
section. Existing `-J`, `-K` and `-Y` behavior and schemas are unchanged.

A successful native exit means capture and metadata checks completed. It is
**not** profile acceptance. Run `profile_intervals.py` with the stream hash,
external exit-status file and exclusive output path to evaluate the complete
supported-profile policy. No planner or mover is implemented. This milestone
has targeted build and pool-free fixture validation only; real profile runtime
validation remains on hold for independent review.

## Bounded absence evidence

The native collector checks all 42 cached DDT checksum/type/class slots, their
owning spa/MOS identities, cached statistics object pointers and in-core DDT
entry/repair trees. All ownership state must be absent. It independently walks the selected MOS directory with no prefetch,
rejecting every DDT-prefixed key, including unknown future names. It then scans
all allocated MOS objects from zero birth cutoff, with monotonically increasing
object IDs, rejecting DDT ZAP/statistics types and invalid types even if no
matching directory key exists. Historical empty DDT tables/statistics are
conservatively refused. `dedup=off`, lack of observed dedup BPs, and cached
histograms alone are not absence evidence. DDT contents are never enumerated.

Every DSL_DATASET bonus encountered in that independent MOS scan has its exact
pinned bonus layout checked and copied, allowing only the legacy DSL_DATASET
or extensible ZAP-metadata object type. A dataset-shaped object with another bonus
layout is refused. Snapshot status follows the pinned `ds_num_children != 0`
rule and is emitted explicitly as `snapshot_not_applicable`; snapshots do not
own active ZILs. Every materialized head, including clones and heads whose tree references
would be birth-pruned by ordinary traversal, has its original objset root read
without prefetch. The complete 192-byte ZIL header must be canonical zero.
Nonempty log BPs, claim/replay fields, flags, padding, unreadable roots and
ambiguous dataset states fail closed. This is intentionally stricter than
BP_IS_HOLE(zh_log). No SLOG and a clean export do not imply this condition.

Directory names, object IDs/types, per-dataset dispositions, root BP words,
zero ZIL header words, counters and completion status are retained. Known
inconsistent/deferred-destroy states, unknown dataset flags, future dataset
TXGs and inconsistent snapshot/head fields are refused. No ZIL parsing,
claiming, replay, clearing, transactions or mutation is used.

Limits: 4096 directory entries, 100000 MOS objects, 4096 datasets, 250000 explicit
read/lookup operations and 32 MiB of requested/result-structure byte charges.
Budgets are reserved before operations; dataset limits precede bonus/root reads.
A bounded extra iterator step identifies end-of-scan at exact entry limits.
The outer time/memory limits, FREAD checks and final same-root/TXG cut check
remain mandatory. Byte charges include result structures and logical ARC bytes;
they do not measure physical I/O, indirect DMU reads, retry amplification,
CPU work or import. This is not a hostile-image sandbox or proof of quiescence.

## Acceptance policy and limits

The analyzer validates the profile record order, unique directory keys,
monotonic complete-declared MOS scan, matching dataset dispositions, pinned DDT
slot count, counters, budgets and successful terminal. Every materialized head root must
also match a successfully verified original BP in the paired capture.
Missing/incomplete profile evidence, hash mismatch, nonzero external exit,
changed cut, unsupported descriptors or incomplete original-BP verification
are rejected. It additionally requires:

- no pending-free owner entries and no active async-destroy queue entries,
  including completed-sentinel entries;
- exact original allocation reconciliation with no unknowns, descriptor
  conflicts or partial overlaps;
- at least one relevant head and one measured original-BP verification.

`ddt_absence_verified` and `zil_empty_heads_verified` describe these restricted
checks. `supported_profile_passed` means only suitable input for a future
read-only dry-run planner. `ddt_enumerated`, `zil_enumerated`,
`movable_candidate`, `coverage_complete`, `payload_validation_performed` and
`evacuation_authorized` remain false. Schema/provenance validation trusts the
bounded native producer; it is not independent proof that an arbitrary JSON
producer reported every MOS object. Snapshot/shared references still require
careful pointer-update and crash-recovery design before any actual movement.

## Tests and next gate

Pool-free native fixtures intercept DMU, ZAP and ARC, using real pinned layouts.
They cover cached/on-disk DDT discrepancies, final-slot DDT presence, orphan DDT
objects, unknown formats, head/snapshot/clone classifications, birth-pruned
heads, nonempty/ambiguous ZIL headers, read failures, malformed layouts,
read-only mode, nonmonotonic scans, exact directory/object limits and budget
boundaries before I/O. Saved metadata is checked unchanged and holds/buffers
balanced; write/free/data-payload calls abort. Native records feed the actual
profile analyzer alongside explicitly synthetic verified allocation fixtures.
Negative policy tests cover nonempty queues, missing evidence, unsupported
states, missing verified roots, incomplete verification, reconciliation errors,
changed cuts and failed exits. CLI refusals execute before discovery.

Stop for independent source/bundle review, then separately authorized disposable
runtime validation. No dry-run planner, pool operation or push is part of this
milestone. The qualified v11 bptree runtime results are documented separately
in BPTREE.md and do not validate this new gate.

## V13 snapshot-exemption correction

Snapshot classification cannot rely on ds_num_children alone. Before exempting
a dataset from ZIL inspection, the native collector requires a nonzero, nonself
ds_next_snap_obj and zero ds_snapnames_zapobj; heads require a zero next link.
The checked next-link and snapshot-name-map object IDs are emitted and required
by the analyzer. No exception for missing snapshot links is supported. This
validates local classification consistency, not the entire snapshot-chain graph.
Corrected positive fixtures point the snapshot to its head. Negative native
fixtures present contradictory snapshot fields together with a nonzero ZIL and
verify refusal before any root ARC read; analyzer mutations independently reject
missing/self links, unexpected name maps, head links and omitted evidence fields.
V12 captures lack the required new fields and are not accepted by this corrected
pre-runtime profile schema. Runtime remains on hold for corrected review.


### V14: unallocated pool-origin head

The TXG 330 runtime inspection stopped at the internal origin head because its
root BP is canonical zero. This is legitimate in the pinned source:
`dsl_pool_create_origin()` creates a head with no origin then snapshots it;
`dsl_dataset_create_sync_dd()` zeroes the bonus and leaves the root unallocated
when there is no origin. The special snapshot creation TXG is 1.
`dmu_objset_open_impl()` handles a hole root by creating a zeroed in-memory
objset, but the diagnostic never calls that path or materializes an objset.

The narrow `unallocated_pool_origin_head` disposition requires the loaded
`dp_origin_snap` identity, matching pool/MOS/directory identity, the directory's
head link, reciprocal head/snapshot links in the same directory, creation TXG 1,
exact UNIQUE_ACCURATE flags, canonical all-zero roots and all four zero space
counters. It also rejects encryption, an origin directory clone origin, head
clone links, user references and dataset property objects. The scanned origin
snapshot bonus must exactly match the loaded bonus. Every relevant field is
emitted for analyzer validation; pool/directory cache identity checks remain
native producer checks, as do the cached DDT checks. This is not an independent
reconstruction of the entire DSL graph. Names and specific object numbers do
not grant an exemption. Ordinary missing roots and noncanonical hole BPs still
fail closed.

This disposition has no persisted objset/ZIL header: `zil_header_words` is null,
not a fabricated zero header. It adds no ARC read; operation accounting counts
only materialized head reads. Every materialized head still requires the whole
canonical-zero ZIL header and original-BP verification. Snapshot records remain
explicitly not applicable. The analyzer requires the scanned origin pair and
allows at most one unallocated origin head. Older profile streams lacking the
new structural evidence are rejected.

Pool-free fixtures test the positive pair and identity, root, space, creation,
link, encryption, ancillary-state and cache mismatch negatives. Runtime evidence
establishes the cause of the previous refusal, not a successful run of V14.
The separately collected DDT-negative runtime case stops at an earlier type-43
rejection; it does not exercise the DDT-absence collector. No pool/VM operation
or push was performed for this correction. All global safety flags remain false;
this gate only qualifies inputs for future read-only planning.
