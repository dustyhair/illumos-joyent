# Bounded async-destroy ownership (v10 review hold)

Paired `-K` and verified `-Y` now emit schema v3 with a separate async-destroy
owner section after pending-free owners and before the final cut check. `-J`
remains unchanged. This milestone has targeted build and synthetic native-test
coverage only. No new pool capture, pool mutation, full build or push is included.
The independently verified v9 pending-free fixture is documented in DEFERRED.md;
it is not runtime evidence for this new walker.

## Selected queue and traversal

The exporter reads the selected MOS `dp_bptree_obj`, checking consistency with
SPA_FEATURE_ASYNC_DESTROY. Inactive features require a zero object and produce
an explicit empty scope/terminal. The active object's data and bonus types must
be DMU_OTN_UINT64_METADATA, with an exact 40-byte bptree header. Only the active
`[bt_begin, bt_end)` range is read; earlier freed ranges need not exist. End-offset
arithmetic and declared extent bounds are checked. Each 168-byte entry is read
in fragments at DMU data-block boundaries, so an entry can straddle blocks.

`async_queue` preserves the header and accounting counters. `async_entry`
preserves the original absolute queue index, all 16 root BP words, birth cutoff,
original resume bookmark and UINT64_MAX completed sentinel. Sentinels produce
no owners or tree reads. The walker operates on a local bookmark copy and never
calls bptree_iterate, creates a transaction, dirties a buffer, drains a queue,
updates accounting or applies the global zfs_free_leak_on_eio policy.

A diagnostic-only postorder walker follows objset, dnode and indirect metadata.
Resume ordering follows the pinned zbookmark_subtree_completed semantics with
checked arithmetic. A matching resume point emits the node without revisiting
its already-freed children. Blocks born at or before the entry's birth cutoff
are excluded. An unresolved resume bookmark fails the entire capture. Small
512-byte dnodes are supported; large dnodes, spill resume points, malformed
geometry, encrypted/protected, dedup/DDT, gang and unsupported allocation classes
are refused. Conservative refusals can reject otherwise legitimate pools.
Checksummed metadata reads use ARC_FLAG_WAIT and ZIO_FLAG_CANFAIL without
prefetch or HARD; errors remain errors. There are no data-leaf payload reads.

`async_owner` preserves queue object/index, traversal depth, exact destroyed-tree
bookmark and all BP words. Ancestor BP aliases or physical extent overlaps are
rejected before the next metadata read; repeated references outside the current
ancestor chain are preserved for overlap findings. Owners are not passed to the
original-BP payload verifier. Even though bptree roots retain full BPs (unlike
stripped bpobj entries), payload_verifiable and movable_candidate remain false.

## Budgets and completion

Limits are cumulative across the queue: 4096 queue entries, 100000 BP visits,
100000 explicit read/lookup operations, 64 MiB requested bonus/queue bytes plus
logical ARC metadata bytes, depth below 64, and 16 MiB per metadata block.
Object-info, bonus-hold, offset-next, DMU-read and ARC-read calls reserve budget
before execution. Offset-next and object-info consume operation counts but have
no known requested payload byte size. Entry counts are checked before queue
reads; depth/visit limits precede traversal reads. Buffers and bonus holds are
released on failures. The existing outer timeout, memory budget, FREAD checks
and same-cut root/TXG checks still apply.

These are explicit operation and requested-byte budgets, not bounds on physical
I/O, indirect DMU reads, decompression CPU, lower-level retry amplification, or
pool-import work. Import precedes this walker; this is not a hostile-image sandbox.
Header accounting is preserved, not reconciled against physical RAID-Z A bytes.
An error produces an incomplete async_end and fails the enclosing capture.
External process exit status remains mandatory, including buffered-output and
close failures. A successful terminal means enumeration of the supported
selected queue under these bounds, not complete ownership of the pool.

## Analysis and validation

The v3 analyzer requires a contiguous section, active/empty scope consistency,
absolute entry ordering, sentinel/cutoff validity, unique bookmarks within each
entry, owner provenance, raw BP/DVA eligibility, bounded counters and a successful
terminal. It retains all individual DVA references. It reports async ownership
outside allocation, overlap with live and pending-free owners, multiplicity
among async owners, and residual allocation after all three observed classes.
Original reconciliation and pending-free results are unchanged. JSON provenance
validation is not independent proof that an arbitrary producer traversed every
child. Global coverage_complete, payload_validation_performed and
evacuation_authorized remain false. DDT and ZIL remain unenumerated.

Native fixtures execute the production walker against fake DMU and ARC using
real pinned layouts. They cover cross-block entry reads and second-fragment
errors; nonzero active starts; resumed leaves, skipped earlier objects and
already-visited child subtrees; equality cutoffs; sentinels; unresolved resumes;
queue arithmetic/layout errors; ancestor cycles/overlaps; byte, operation, visit
and depth limits before I/O; exact queue/visit limits; read errors and short
buffers. Saved header/entry bytes and hold/buffer balances are checked, with
write/free/data-payload calls intercepted to abort. Native outputs also pass
through paired and verified v3 analyzers; negative parser fixtures corrupt
provenance, counts and terminals. Existing v1/v2 analyzers remain supported.

Next gate: independent source/bundle review, then separately authorized disposable
same-cut runtime fixtures with nonempty and partially resumed async-destroy
queues. Compare roots, cutoffs, original resume bookmarks, remaining owner BPs
and extents with a bounded stock/reference traversal. No runtime success or
relocation safety is claimed here.

## V11 review corrections

Every non-sentinel entry must have an OBJSET root at BP level zero, including
birth-pruned roots. A traversed root must appear exactly once as the last owner
for that entry, with identical BP words and the root bookmark; pruned or hole
roots must have no owners and no unresolved resume. Completed sentinels retain
the native exemption from root validation. Regression fixtures reject zero-word,
wrong-type and wrong-level roots, missing root owners even with adjusted terminal
counts, and invalid root-owner ordering/disposition. The Makefile now lists
zdb_bptree.c as a zdb_inventory.o prerequisite; an isolated make timestamp test
checks that changing this include invalidates the object.
