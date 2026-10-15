# Paired BP/allocation observation — independent review required

Research-only implementation. Execution HOLD: no actual pool has been opened
with this extension. Review code, tests and runtime bundle before the coordinator
runs it on a newly exported, externally quiesced disposable fixture. No mover,
claims, allocation, frees, checkpoint subtraction, or production access.

## Capture and identity

Use the verified native runtime launcher, after independent authorization:

    /bin/bash runtime/launch.sh -K /absolute/new-paired.jsonl -e -p /explicit/lab/devices pool

-K and -J are mutually exclusive. -K inherits -J's strict pre-discovery CLI
checks, EXPORTED-only import with zero relaxation flags, FREAD initialization,
exclusive output creation, 300-second deadline and 256 MiB ARC setting. -J
continues to produce BP-only schema v1; -K produces zdb-observed-allocation-v1.
The read-only gate additionally rejects a spa_mode containing FWRITE even when
spa_writeable() is false due to an untrusted configuration.

One opened spa supplies BP traversal and allocation-map enumeration. Pool GUID,
selected synced uberblock TXG, full normalized root BP and vdev geometry identify
the cut. Vdev records additionally include asize and metaslab_count. cut_end
records TXG/root again; a change fails the capture. This is an in-process root
consistency check, NOT a fresh label read or proof that another host is not
writing. External lab quiescence/immutable media remains required.

The stream is bound by an externally recorded SHA256 and process exit status.
Runtime identity is bound by the reviewed runtime hashes and the coordinator's
execution evidence. No self-referential source commit or signed attestation is
claimed by JSON. A successful process exit AND complete, error-free terminal
record AND count/hash/root validation are required. Close can fail after a
complete terminal was buffered; nonzero exit always invalidates the capture.

Never compare the existing TXG-600 BP evidence with new allocation data. The
fixture was subsequently imported writable. Capture both populations at a new
selected cut. paired_intervals.same_cut rejects different GUID/TXG/root/topology;
--same-cut-as applies this check to a previously validated analysis result.

## Allocation set

zdb_allocation.c is included by the exporter so its helpers remain private.
It does not call zdb_leak_init/space_map_load, modify ms_allocatable/ms_loaded,
change allocator callbacks, call zio_claim, or subtract checkpoint/owner ranges.
Each concrete normal RAIDZ metaslab gets a separate scratch RANGE_SEG64 tree.

For each selected-MOS base map, record metaslab bounds, map object, recorded
length/shift and unflushed TXG. Replay chronological alloc/free entries into the
scratch allocated tree and compare base allocated bytes against map metadata.
A genuinely absent base map is explicit and may still acquire log allocations.

Read-only spa loading does not replay log map data. Enumerate the loaded log
metadata AVL in ascending TXG, explicitly open/read each log map from the same
MOS, and apply each entry iff log TXG >= that metaslab's unflushed TXG. Emit all
entries, including already-flushed skipped entries, with source map object,
TXG, ordinal, vdev/metaslab, interval, operation and applied flag. Do not also
apply ms_unflushed_allocs/frees. Future/zero/nonincreasing log TXGs, bad IDs,
unsupported vdevs/classes, arithmetic overflow, misalignment and cross-metaslab
entries fail. Duplicate/overlapping allocation or free outside the current
allocated tree fails instead of unioning away an inconsistency.

The checked parser uses pinned space-map bit macros, bounds the second word
BEFORE access, rejects reserved two-word padding and shift/address overflow,
and never lets a two-word entry cross a block. It validates map length alignment,
object extent/block size, allocated block presence using dmu_offset_next, and
propagates DMU read/callback errors. It deliberately skips space-map debug
entries as the pinned parser does. Native word decoding relies on normal DMU
byteswapping for the space-map object type. Full malformed-pool/import safety
is NOT established: shared import/metadata code still contains assertions and
is not a hostile-image sandbox. Nonzero/crashed/timed-out capture is invalid.

Caps: 4096 metaslabs, 64 MiB total recorded map bytes, 1,000,000 map words,
16 MiB maximum map block buffer, 1,000,000 allocation records (including
provenance/operations), plus the existing separate 1,000,000 BP/dnode events.
The word cap also bounds scratch-tree growth; allocator/library overhead is
not a measured hard RSS cap. Early limit failures leave diagnostic partial data.

Emit all final allocated intervals BEFORE any ownership claims. allocation_end
contains record/interval/byte/map-word/map-byte counts. The analyzer independently
replays all exported operations, requires exact agreement with these intervals,
and produces SHA256 digests of canonical operations and interval sets.

## Scope and exclusions

Active checkpoint feature/state, checkpoint-held bytes, active removal feature
or in-progress removal are refused. Non-normal classes, log vdevs and indirect
vdevs are refused. No subtraction is used to create an apparently clean audit.
DDT, deferred frees, async destroy and ZIL OWNERSHIP remain explicitly
not_enumerated. Their allocations, if present in admitted concrete maps, remain
in A. These statuses do not assert that the queues/logs are empty. Labels,
uberblocks and boot/reserved non-metaslab areas are outside A's definition.

All records retain coverage_complete=false, payload_validation_performed=false,
evacuation_authorized=false. Complete export is only successful attempted
serialization. Even exact address equality does not complete ownership or
payload validation and cannot authorize relocation.

## Offline comparison

    python3 paired_intervals.py paired.jsonl --sha256 <captured-sha256> \
        --exit-status exit-status.txt --output new-analysis.json

The output must not already exist. Analysis performs strict JSON duplicate-key,
number/terminal/count/order/root checks and verifies the expected file hash.
It independently decodes ordinary BP DVAs/P/Q/A, retains all reference records,
metadata, births, bookmarks and raw words, and retains allocation records.
It checks metaslab coverage and independently replays base/log operations.

Per-vdev interval sweep emits EVERY A-minus-observed and observed-minus-A span,
with unknown owner explanation, without silently excluding any class. Original
records and their hashed input retain detailed attribution. Exact compatible
repeats retain reference multiplicity; incompatible descriptors and partial
overlaps remain findings. address_sets_equal is only mathematical set equality.
reconciliation_passed additionally requires no descriptor/overlap/unsupported
observations; neither flag changes any certification flag. Missing metadata
ownership explanations remain unresolved even when this diagnostic passes.

observed_intervals.py is the real-input observation analyzer, not the existing
synthetic inventory_certificate model. No observations are relabeled synthetic.
Synthetic test fixtures only exercise this diagnostic and never invoke that
certificate. Canonical JSON hashes are evidence digests, not ZFS payload checksums.

## Validation and limits

Targeted GCC10 build with warnings as errors. Native tests exercise production
checked parser, private-tree operations, and full allocation driver with stubbed
DMU/map reads. Stubbed cases include base/log replay, thresholds, cancellation,
future log TXG, invalid vdevs/alignment/bounds, malformed/truncated entries,
limits, read/open/output errors, checkpoint/removal and writeable-mode refusal.
No kernel_init, discovery, pool import, or real device read is used by this harness;
only the in-memory B-tree cache is initialized. Python tests cover interval
arithmetic against an independent point-set oracle, strict paired records,
replay agreement, cut mismatch and explicit missing/outside ranges. Existing
BP/CLI/runtime identity regressions are rerun with the new private build.

The first native test attempt lacked B-tree cache initialization and crashed;
the harness now calls zfs_btree_init/fini, without initializing a spa or pool.
This was a fixture setup error. No actual pool was accessed in that attempt.

After independent review, run a new paired disposable-lab capture and stock
cross-check. A separate approved fixture must demonstrate nonempty unflushed
log operations; a zero-log pass cannot validate real log replay. Real corrupted
maps, import failure paths, and all excluded ownership classes remain untested.

## v3 independent-review fixes

- Validate raw BP classification BEFORE any nonallocated/unsupported filtering.
  The pinned DMU type bounds, embedded/crypt bits, first-DVA hole identity,
  gang bits and dedup bit determine the class. All reported type/level/size/
  checksum/compression/birth fields must match raw words, including embedded
  encodings. Hole records cannot conceal secondary DVA words. Layout/class
  contradictions fail parsing, never produce a successful empty comparison.
- A central schema pass validates canonical uint64 strings for every GUID,
  bookmark, birth, topology, map/operation/counter field before cut construction;
  bookmark level uses canonical int64. Small numeric enums reject booleans;
  boolean fields require actual booleans. Missing required and unknown fields
  fail; GUIDs must be nonzero and top-vdev GUIDs unique. Cut comparisons also
  validate their identities before comparing them.
- Base-map entry ownership is the enclosing metaslab, matching the pinned
  loader. SM_NO_VDEVID is truncated to zero by the writer in the 24-bit
  two-word vdev field. It is not an owner ID. Log entries continue to require
  and validate their encoded vdev ID. test_pinned_writer.inc is the VERBATIM
  space_map_write_seg helper from pinned space_map.c; tests call it against
  in-memory dbufs to generate long two-word ALLOC/FREE entries for nonzero
  enclosing vdevs 1, 3 and 19. No pool or actual DMU write is involved.
- Preflight all base/log map objects before ANY replay: nonzero object IDs
  must have exactly one owner, including across log TXGs. Absent base maps
  retain object zero; log object zero is invalid. Registry cap: 8192 map
  objects in addition to existing resource limits. Analyzer independently
  enforces global object ownership, including zero-length aliased maps.

Current verification: 70 native checks; 39 exporter/runtime regressions;
24 analyzer tests with 1000 interval oracle cases and review reproducers.
Actual prior TXG-600 data passes strict raw/schema validation unchanged:
850 ordinary / 13719 hole / 58 embedded BPs, 987 interval references. No new
allocation data or comparison against a different cut was produced.
Review HOLD remains. No pools, pushes, baseline changes or production access.

Pinned writer helper SHA256: 94a1420fd8ff4a277a7805813eebebe08f35d68a7b63c4de1b8d2659f7910010

## v4 lab failure investigation and narrow boundary fix

The real paired TXG-677 stream remains FAILED and unchanged (SHA256
fed1036d66f13e3c7aab6720caff94c30423cc6ea8416ceb529580da87c953ec).
Stock -mmm output reports map 21 length 240 and allocated bytes 0x2c000,
but does not print individual entries: dump_spacemap requires -mmmm or -dddddd.
The stream preserves six successful ALLOC entries before EEXIST, not the
rejected seventh operation. No original missing operation has been invented.

Confirmed defect: pinned range_tree_find_in returns B_TRUE with *osize == 0
when the successor begins exactly at start+size (its comparison uses > rather
than >=). za_apply incorrectly treated this zero-length boundary intersection
as an overlap. The native reproducer uses the exact six recorded operations,
then a clearly labeled SYNTHETIC left-adjacent probe. Before the fix, it fails
with EEXIST; with the fix, the probe is accepted and correctly coalesced.
Actual overlapping ranges (including a one-byte intersection), duplicates and
frees over holes continue to fail. The production change only requires a
positive intersection length when rejecting an allocation. Shared range-tree
or space-map writer/loader behavior is unchanged.

Pinned writer/loader interpretation: map entries describe half-open allocations
and frees; range_tree_add permits adjacency and coalesces neighboring segments.
The sorted-tree comparator treats touching endpoints as nonoverlapping. The
existing find_in API's zero-size successor result therefore is not evidence of
a duplicate allocation. No genuine overlap check was removed or weakened.

This verified defect is CONSISTENT with the lab failure, not definitive proof
of its specific seventh entry: the saved evidence lacks that entry. Exact
confirmation requires the coordinator to capture decoded entries at verbosity
4. On an authorized, exported, externally quiesced disposable lab cut:

    /usr/sbin/zdb -e -p /explicit/lab/data-only-directory -u labtank
    /usr/sbin/zdb -e -p /explicit/lab/data-only-directory -mmmm labtank 0 7

The trailing arguments select top-vdev 0, metaslab 7; record full stdout/stderr
and exit status. Existing -mmm output is insufficient. The pool has since been
reimported/scrubbed, so a new dump must be labeled with its NEW selected cut.
Do not rewind, pretend it proves TXG 677, or compare old BPs with new allocation
maps. An already-preserved immutable TXG-677 image would be needed to establish
that historic entry exactly. No new pool reads were performed during this fix.

The fixture JSON/inc contain only the six original emitted operations with
source hash/TXG and explicit missing-entry caveat. Native and Python tests test
that exact prefix and independent boundary probes. Failed-stream analyzer
rejection and unavailable differences remain unchanged. Independent review is
required before another coordinated paired capture; the historic root-cause
attribution remains limited until additional entry evidence is available.
