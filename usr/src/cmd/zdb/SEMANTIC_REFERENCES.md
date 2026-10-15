# Offline semantic references, initial bounded profile

`semantic_references.py` extends the observed graph with dataset deadlist
references and contextual unflushed-TXG array layouts. It revalidates the raw
`-W` capture through `reference_graph.analyze()` on every public invocation.
It does not trust saved graphs, stock display text, caller-selected owners,
or a manifest's assertion that a checksum passed.

All coverage/readiness flags remain false. The structural closure and dependency
order remain unchanged. Semantic matches identify additional references to
consider; they do not supply a COW/write schedule or establish exhaustive
reverse references. Unknown, unmatched, historical-root, accounting,
publication, recovery and stock-reader obligations remain explicit.

## Input evidence and authentication

The second input is a bounded JSON document, pinned by its SHA256:

```json
{
  "schema": "semantic-leaf-evidence-v1",
  "capture_sha256": "<exact raw -W capture SHA256>",
  "cut_sha256": "<canonical validated cut SHA256>",
  "leaves": [{
    "bp_words": ["<16 canonical 64-bit hex words>"],
    "source_replica": {
      "slot": 0,
      "vdev_id": "1",
      "vdev_guid": "<captured GUID>",
      "offset": "<source byte offset>",
      "asize": "<source allocated size>"
    },
    "physical_hex": "<exact BP PSIZE physical bytes, lowercase hex>"
  }]
}
```

The descriptor must be an exact original leaf BP in the revalidated graph.
The replica must match one of its captured DVA slots, including GUID, offset
and ASIZE. The decoder recomputes little-endian Fletcher4 over all PSIZE bytes
and compares all four original checksum words before decompressing. The initial
profile accepts native little-endian, non-dedup, unencrypted type-5/type-195
leaves with checksum 7 and compression OFF or LZ4. It records physical/decoded
SHA256 and source-replica provenance. A supplied location is checked against
the descriptor; the independent acquisition review must establish where those
bytes were actually read. Checksums alone are not location or replica-health
proof.

ZFS LZ4's BE32 packed-length prefix and every literal, match offset, extension
and decoded length are bounded. The implementation uses no external codec,
pool, device or subprocess API. Embedded ownership ZAPs are decoded directly
from authenticated captured dnode BP words, skipping `blk_prop`/`blk_birth`.
Only canonical, unnormalized embedded micro-ZAPs are supported initially.
Fat/non-embedded dataset deadlists explicitly refuse; unrelated non-embedded
ZAPs remain outside the decoded scope.

## Ownership and stored BPs

Owners are derived from MOS dnode bytes: dataset bonus `ds_deadlist_obj`,
deadlist micro-ZAP birth bucket, BPOBJ dnode and BPOBJ bonus. No object number
is hardcoded. Valid empty deadlist maps and canonical empty BPOBJs are accepted.
Nonempty BPOBJs require the exact 48-byte BPOBJ_HDR, zero subobject fields,
one logical leaf, checked `entries * 128`, data-block/count bounds and supplied
authenticated raw leaf bytes. Missing bytes refuse. Older headers, subobject
lists, indirect/multiple data blocks, malformed links and unsupported layouts
refuse rather than skipping references.

Each entry retains its stored 16 words, owner chains/provenance, BPOBJ object,
entry index and decoded byte offset. Matching reproduces `bpobj_enqueue()`:
clear fill; clear non-dedup checksums; for embedded BPs retain only property
and logical birth. The resulting tuple is a matching key, never a new original
BP identity. Multiple captured original descriptors with one normalized key refuse.
Uniqueness is limited to captured descriptors; erased fields leave ambiguity
about other originals outside that capture.
Unmatched entries retain unresolved obligations. Matches are tagged as target,
ancestor, or outside the observed structural closure; the owner itself can be
outside that closure. Entry births must fit the captured cut and the owning
deadlist's exclusive-lower/inclusive-upper birth bucket.

## Contextual TXG-array classification

Type 195 alone never establishes a non-pointer layout. An exact captured key
`com.delphix:ms_unflushed_phys_txgs` in a supported MOS type-196/bonus-NONE
micro-ZAP must point to a type-195/bonus-NONE, 128-KiB, single-leaf object.
That authenticated object-link context selects the pinned source layout:
`metaslab_unflushed_phys_t` has one uint64 TXG field, with no `blkptr_t` slots.
Duplicate contextual owners and incompatible target layouts refuse.

This does not independently validate vdev registration/configuration ownership.
That limitation and the allocation/log-accounting preservation obligation are
reported for every classification. Array raw bytes are optional for this
contextual format classification; absent bytes leave `payload_values_validated`
false. Supplied array bytes must authenticate and their uint64 values must not
exceed the cut TXG. Metaslab counts/index coverage are not independently proved.
No accounting metadata may be discarded based on this classification.

Source contracts at base `da512a7e2e6716980271582e436faef06a575f71`:

* `sys/dsl_dataset.h:133`, `dsl_deadlist.c:65,225` and `zap.c:1140`: owner links
  and hexadecimal birth buckets.
* `sys/bpobj.h:39`, `bpobj.c:148,222,479`: versioned headers, entry traversal
  and the exact stored-BP normalization.
* `metaslab.c:6037`, `sys/metaslab_impl.h:554`,
  `spa_log_spacemap.c:1204`: unflushed-TXG allocation, layout and reader.
* `blkptr.c:93`, `sys/spa.h:340`, `common/lz4/lz4.c:101`,
  `sys/zap_impl.h:47`: embedded, compression and micro-ZAP formats.

## Invocation, bounds and verification

Use the same capture/status/hash/cut/target arguments as `reference_graph.py`,
plus `--leaf-evidence PATH --leaf-sha256 SHA256`. Output is created exclusively
only after successful validation. Inputs must be regular, non-symlink files.
The evidence document is at most 16 MiB; its JSON keys are checked for duplicates.
Raw capture remains bounded at 512 MiB by the graph analyzer.

Semantic budgets independently cap 64 leaves, 8 MiB total decoded bytes,
100,000 MOS objects, 100,000 entries, two million charged work operations,
and 64 MiB serialized output. Individual external leaves are at most 1 MiB.
The work counter charges scans, checksum words, decoded LZ4 bytes and entry
loops; bounded JSON parsing, hashing, graph validation and sort comparisons
are outside that counter and retain their respective byte/count bounds.

Run `python3 test_semantic_references.py` for portable pool-free tests. They
exercise synthetic format/owner cases and the public validation boundary;
synthetic private-helper fixtures do not establish native capture correctness.
Adversarial cases include forged keys, malformed ZAPs/links, missing raw bytes,
checksum/cut/replica mismatches, truncation, overflow, subobjects, unsupported
trees, ambiguous normalized matches, unknown references and outside-closure
owners. Exact accepted-capture replay and independent raw acquisition are
separate review evidence.

## Sealed TXG 330 offline replay

The raw `-W` capture SHA256 is
`eb8b80b66ee9734a3a5370215252300df4f4b617c0d0bea79d1fb828394c68e9`.
The independent acquisition's 4-KiB BPOBJ 288 physical payload has SHA256
`a60466d5679eef265289c3beca80c55c9cd802f1b320ab9a4730f06916687292`;
its 128-KiB decoded buffer has SHA256
`22d05b71555adf3e011966bf42f8df6ddbe052fd91f12638ef2f950c404c708c`.
All three acquired copies are byte-identical. The coordinator confirmed that
independent review of the acquisition passed; this extension awaits its own review.

Exact public-analyzer replay authenticates one BPOBJ, derives the chain
dataset 184 -> deadlist 276 -> birth bucket 1 -> BPOBJ 288, and emits seven
stored references at decoded offsets 0, 128, 256, 384, 512, 640 and 768.
Each has one normalized match among captured originals, to the snapshot 275 /
clone 281 dnode chain L0 through L5 and their shared objset root. The owner
BPOBJ is outside the observed closure, while all seven matches are ancestors
inside it. There are zero target matches and zero unmatched entries in this
bounded enumeration; this is not an exhaustive absence/identity proof.

Objects 86, 90, 93 and 98 receive contextual TXG-array layout classifications.
Their raw array values and vdev registration are not independently validated
by this input. The structural closure stays at 20 blocks; all readiness flags
stay false. Sixteen new test methods pass normally and under optimized Python,
alongside 152 existing regression methods. No new native build is required.

For a current-root COW experiment retaining all old allocations and source
storage, older uberblocks can remain unchanged. Snapshots/clones and these
deadlist references still matter in the current cut. Writable stock reopen
needs consistent allocation/free accounting and crash-safe root publication.
Full removal must address every retained/selectable state or establish a
compatible retirement/mapping policy; it is not equivalent to rewriting every
stale uberblock slot. None of these obligations is discharged here.
