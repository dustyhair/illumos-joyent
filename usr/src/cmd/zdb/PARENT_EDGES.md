# Bounded parent-edge capture (runtime reviewed)

`-W new.jsonl -e -p /explicit/path pool` is a new opt-in diagnostic mode.
It runs the existing verified/profile capture, then a separate bounded walker
beside the existing collectors. Existing modes and traversal callers retain
their behavior; no libzpool ABI or persistent mapping format is added.

Analyze with `parent_edges.py CAPTURE --sha256 SHA --exit-status EXIT_FILE
--output NEW_ANALYSIS`. Native success alone is not acceptance. The analyzer
projects the profile evidence without losing records and requires the existing
supported-profile policy, successful external exit, exact stream hash and cut.

## Evidence and anchors

The walker starts at the selected imported uberblock's MOS root, discovers
DSL_DATASET bonuses in authenticated MOS dnode buffers, and walks every
discovered dataset root. It applies no snapshot birth cutoff and does not
coalesce shared tree visits. A head and snapshot/clone sharing a physical tree
retain separate logical bookmark paths and parent observation IDs.

Each parent_edge contains original child BP words, parent BP/bookmark, parent
container observation ID, decoded byte offset, dnode base offset where relevant,
slot and child bookmark. The selected-uberblock anchor has no parent buffer and
is qualified as selected import evidence, not independent label proof.
Dataset-root anchors locate ds_bp inside actual MOS bonus bytes. The analyzer
cross-checks dataset-root coverage and profile dataset fields against those
bytes, including links, flags, creation TXG and space counters.

Pointer-bearing parent blocks are read by ARC using their original BP with
WAIT/CANFAIL and no prefetch. Fletcher-4 or SHA-256 checksums are required.
Successful ARC reads supply native checksum-authentication evidence. The entire
decoded AMD64 little-endian parent buffer is retained as hex. Decoded offsets
are NOT disk offsets, including when stored blocks are compressed.

The analyzer checks child byte membership, structural slot offsets/bookmarks,
all expected outgoing tree slots, parent identities, dataset root coverage,
completion counters and accepted original-BP verification for each allocated
edge BP. It does not independently reproduce ARC checksums from compressed raw
P bytes or authenticate label selection. The stream and analysis trust the
bounded producer for the claimed read and root-selection operations.

Supported containers are indirect BP arrays, legacy 512-byte dnodes with spill
pointers, objset meta/user/group/project-used dnodes, and MOS dataset bonus
roots. Large dnodes, embedded pointer-bearing metadata, unknown flags/types,
crypt/dedup/DDT states, malformed lengths/levels, cycles and read failures refuse.
Ordinary embedded leaf payloads and holes require no additional container read.

## Bounds and scope

The additional walker limits visits and metadata reads to 100,000 each,
requested decoded bytes to 64 MiB, dataset roots to 4,096, recursion depth to 64,
and conservative output charges to 512 MiB. Checks precede metadata ARC reads;
dnode layout checks precede pointer/bonus interpretation. Existing outer process
limits and final root/TXG equality remain mandatory. These budgets do not bound
earlier import/profile traversal or physical I/O amplification. The analyzer
caps the entire input file at 512 MiB, so it may refuse a native-complete stream
whose combined profile and parent sections exceed that input bound.

Completion describes current MOS/dataset TREE POINTER SLOT evidence only. It
excludes BP-bearing semantic payloads such as deadlists, historical uberblocks
and other historical roots. No reference count proves a block has no other
owners. Records retain reverse_references_complete=false,
pointer_rewrite_ready=false, placement_performed=false and previous false
certification flags. No source allocation, transaction, pointer or pool state
is changed; no topology removal is attempted.

## Validation and next step

Pool-free fixtures cover shared head/snapshot roots born before the previous
snapshot TXG, indirect/dnode/spill slots, MOS bonus anchors, hole roots,
malformed layouts, checksum/read failures, cycles and pre-read budgets. A full
synthetic profile/edge fixture exercises projection and acceptance. Mutations
cover missing slots/containers, altered bytes, changed cuts and mismatched
profile bonuses. Synthetic checksum-success stubs are not disk checksum proof.

Independent source/bundle review passed for commit
1ef884f67adff5c27d7e737ddfeb0591a53dd5b7. Exact runtime validation against the
sealed TXG 330 fixture and final independent evidence review passed. Raw-parent
comparison corroborated decoded membership and snapshot/clone aliases within
the captured tree-slot scope; independent label election and per-replica health
remain unproven. See [RESULTS.md](RESULTS.md). Actual movement still requires relevant reverse-reference
semantics, COW ancestors/shared roots, allocation reservations,
snapshot/deadlist accounting, atomic publication, old-root retention and crash
recovery. Unmodified stock-reader reopen/scrub and update compatibility remain
unproven. No direct rewrite or indirection architecture is selected here.
