# Bounded pending-free ownership (v8 review hold)

This extends paired `-K` and verified `-Y` capture to schema v2. BP-only `-J`
and schema-v1 analyzers remain supported. No new pool read, VM operation, pool
mutation, mover or push is authorized by this implementation stage. Runtime
validation follows independent review on a disposable exported fixture.

## What is enumerated

After the independent allocation-map capture, before the final cut check,
`zdb_pending.c` reads the selected MOS objects named by
`spa_deferred_bpobj.bpo_object` and, on pool version >= 26,
`dp_free_bpobj.bpo_object`. It does not drain either queue. These correspond to
the two no-free bpobj iterations in stock zdb's block-accounting path. The
async-destroy bptree is a distinct obligation and remains unenumerated.

The walker reads object information and bonus data directly, validates
DMU_OT_BPOBJ / DMU_OT_BPOBJ_HDR, exact supported 16/32/48-byte bonus layouts,
block alignment/size, count arithmetic and object extents. It follows explicit
DMU_OT_BPOBJ_SUBOBJ lists using a breadth-first queue. Object IDs (including
list objects) must be nonzero and unique across both roots: cycles, shared child
objects, cross-root aliases and reused list objects fail closed. This conservative
policy may reject a legitimate future/shared layout; it never silently skips it.

Caps: 4096 combined bpobj/list object IDs, depth 64, 100000 stored entries,
32 MiB directly requested bonus/data bytes and a 16 MiB block buffer. The byte
counter does not measure physical I/O amplification or indirect DMU metadata
reads. Object/data reads propagate errors, detect holes in required data blocks,
and release bonus buffers before continuing. No recursive DMU lock/buffer stack,
transactions, dirty/free calls, allocator-tree mutation or payload zio_read is
used. FREAD and !spa_writeable are required before/after enumeration. Shared
pool import still precedes these checks and is not a hostile-image sandbox.

`pending_scope` identifies pool version and both roots, including explicit DSL
queue non-applicability for older versions. `pending_object`, `pending_link`
and `pending_owner` retain root/containing-object/index/parent/depth provenance,
raw stored BP words and header accounting counters. Two `pending_queue_end`
records distinguish empty, enumerated, not_applicable and incomplete queues.
`pending_end` binds counts, direct requested bytes, completion/error and the
active object/index diagnostic context. A failure invalidates the entire capture;
partial records are diagnostic only. External exit status remains mandatory,
including a close error after a terminal record was buffered.

## Stored entries are not original payload BPs

Pinned bpobj_enqueue erases non-dedup checksums and fill, and embedded payloads.
Pending entries never use zi_record/zv_record and cannot be accepted as original
payload reads. Every owner record has payload_verifiable=false and
movable_candidate=false. Stored ordinary unprotected non-dedup/non-DDT nongang
entries yield checked normal-RAIDZ DVA extents; embedded entries have no extents.
Holes, unknown types, protection, dedup/DDT, gang and unsupported/bad geometry
fail closed. Logical/physical births cannot exceed the selected cut.

Header bpo_bytes/comp/uncomp are preserved as accounting evidence. Apart from
requiring zeros on a wholly empty object, they are not reconciled against the
entry stream: bpo_bytes uses RAID-Z-deflated dsize, not the physical DVA A sum,
and subobject aggregate semantics need a separate accounting check. Completion
means bounded enumeration of the declared object graph, not proof of all owner
classes or trustworthy execution against an arbitrary untrusted JSON producer.

## Analyzer behavior

Schema v2 requires one contiguous owner section before cut_end. The analyzer
checks roots/applicability, exact BFS graph/order, entry/link indices, headers,
limits, both queue summaries, full terminal counts, raw BP eligibility and DVA
P/Q/A geometry. Native output fixtures are passed through this parser as well
as independently hand-built negative fixtures. Schema v1 cannot carry owner
records and cannot acquire a pending-free completion claim.

Original A_minus_observed, observed_minus_A, address_sets_equal and
reconciliation_passed are unchanged. Additional outputs are:

- pending_explained_A_minus_observed: intersection of the original missing set
  with the enumerated pending extents;
- pending_residual_A_minus_observed: remaining unexplained spans;
- pending_outside_A, pending_vs_observed_overlaps and pending_owner_overlaps;
- all owner records, graph/link provenance and individual DVA references.

Overlaps are findings, never erased by classifying a queue entry as pending free.
Duplicate references are retained. The overlap sweep reports multiplicity spans,
not a potentially quadratic list of reference pairs. Joining references back to
object/index/slot provides attribution. Pending completion does not make a
block movable. Coverage_complete, payload_validation_performed and
evacuation_authorized remain false globally. DDT, async destroy and ZIL remain
not_enumerated; checkpoint/removal and unsupported allocation classes retain
existing refusal behavior.

## Validation and next runtime gate

Focused native tests use fake DMU only; direct dirty/free and payload-read calls
abort, and every case checks balanced bonus holds/releases. Success cases cover
both queues, empty, nested, duplicate extents, embedded, and legacy bonus/version
layouts. Failure cases cover graph cycles/aliases, missing objects, layout/count/
extent/depth caps, read failures, sparse blocks, writable mode, invalid stored BPs,
bad geometry and output failures. Direct byte/object cap boundaries are checked.
Analyzer fixtures preserve original discrepancies and test explained/residual/
outside/overlap results, owner provenance, counters, cut/exit failure and verified
mode's unchanged live-read accounting. Prior allocation/exporter/checksum tests
are rerun against the targeted build.

After review, obtain a disposable same-cut capture with at least one nonempty
pending queue and compare exact root/child objects, entries and extents against
stock no-free enumeration. An empty-queue run alone is not substantive validation.
No runtime success, complete ownership inventory, relocation, crash recovery or
removal safety is claimed by this milestone.
