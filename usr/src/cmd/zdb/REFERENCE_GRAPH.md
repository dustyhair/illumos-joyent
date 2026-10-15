# Offline observed reference graph and ancestor closure

The additive [semantic-reference analyzer](SEMANTIC_REFERENCES.md) can authenticate
bounded deadlist payload evidence and report references into this structural
closure. It preserves the graph's unresolved obligations and readiness flags;
its matches do not change this graph into a write schedule.

`reference_graph.py` consumes the original `-W` stream, SHA-256, external exit
status and expected cut digest. It reruns `parent_edges.analyze()` and the
underlying supported-profile validation. Saved analysis summaries are not an
input. The graph does not open devices, reserve space, supply destination
offsets, publish mappings or perform writes to a pool.

```
python3 reference_graph.py CAPTURE --sha256 SHA --exit-status EXIT_FILE \
  --cut-sha256 CUT_SHA --target-vdev-id ID --target-vdev-guid GUID \
  --target-offset OFFSET --target-asize ASIZE --target-dva-slot SLOT \
  --output NEW_GRAPH
```

The target is one exact captured DVA: top-level vdev ID/GUID, source byte offset,
allocation size and original slot 0..2. Coordinates describe top-level DVA
space, not child-disk offsets. Missing, ambiguous, mismatched and noncanonical
selectors refuse. Exit 0 means the observed graph and structural closure were
validated; mandatory unresolved obligations and all false readiness flags remain.
Exit 2 reports invalid input, conflicts, unsupported graph shapes or budget
exhaustion. Output is an exclusive, compact canonical JSON file.

## Canonical blocks, slots and aliases

A block identity includes every original BP word. Each DVA replica retains its
original slot, top-level identity, offset and allocation size. Shared observations
of one descriptor merge; distinct replicas remain distinct. Overlapping distinct
allocations, incompatible descriptors using the same extent and duplicate DVA
replicas refuse. Pointer-bearing parents with the same descriptor must have
byte-identical decoded buffers; matching checksums or addresses alone do not
permit a merge. The output retains the decoded digest and original observation
IDs; the input hash binds the full decoded bytes retained in the capture.

Canonical slot identity is parent block plus decoded byte offset. Kind,
dnode base, slot number and original child must also agree. Distinct slots
referencing one child remain distinct. Each slot retains every logical
observation, including its original node/parent-node IDs and both bookmarks.
Holes and embedded values remain slot-local terminals, not physical block nodes.

Dataset bonus anchors retain dataset identity, snapshot state, directory and
previous-snapshot evidence. A previous-snapshot link alone does not prove clone
origin; the graph preserves clone aliases without inventing that relationship.
The selected imported uberblock is an explicitly qualified publication boundary.
Dataset-root closure continues through its MOS bonus slot and ancestors rather
than stopping at the dataset objset.

## Observed dependencies and unresolved semantics

For an explicit DVA target, reverse slot links identify observed ancestors.
A deterministic DAG order places each observed child before its parents and
emits each shared ancestor once. Cycles after canonicalization, missing blocks,
and an ancestor frontier other than the selected MOS root refuse. This is
structural dependency order, not a COW allocation/write schedule, copy budget,
atomic publication protocol or proof of absent external owners.

The semantic watchlist identifies BPOBJ, deadlist, subobject, intent-log and
generic uint64 metadata leaf payloads without interpreting their ownership.
Pointer-array containers themselves are already structurally decoded. This
watchlist is not exhaustive. Output always lists unresolved non-tree semantic
references, other semantic fields, historical roots/uberblocks, independent label
election, COW/accounting/recovery and stock-reader/update compatibility.
`missing_global_reference_count` is unknown, never inferred to be zero from a
closed observed graph. The output keeps coverage_complete,
reverse_references_complete, pointer_rewrite_ready, ancestor_rewrite_ready,
placement_performed, capacity_proven and write_schedule_available false.

## Bounds and validation

Input is a regular nonsymlink file capped at 512 MiB. The parent validator's
existing capture limits apply. Additional construction limits are 100,000 BP
blocks, 100,000 canonical slots, 100,000 logical slot observations, 2,000,000
charged construction/closure operations and 256 MiB of conservative output
charges. Containers are bounded by the input parent evidence. Charges precede
new entries; serialized output including its newline must fit the output cap.
These graph charges exclude input parsing/revalidation and bounded-sort internal
comparisons; they are not a hostile-input CPU/memory sandbox.

Pool-free tests cover shared roots/aliases, separate slots and replicas,
conflicting bytes/descriptors, overlapping extents, cycles, diamonds, missing
parents/evidence, incorrect cuts/selectors, output exclusivity and all budgets.
The replay script revalidates the exact accepted TXG 330 capture:
`eb8b80b66ee9734a3a5370215252300df4f4b617c0d0bea79d1fb828394c68e9`.

It produces 580 allocated BP blocks, 33 canonical parent blocks from 44 parent
observations, 17,695 canonical slots and all 24,907 logical slot observations.
For the shared object-2 block 0, aliases 184/275/281 remain intact. Its observed
closure has the data block plus 19 ancestors, reaches all three dataset bonus
anchors and terminates at the selected MOS root. Five semantic leaf blocks
remain explicitly opaque. These are captured structural facts, not exhaustive
reverse-reference or relocation-safety evidence.

The implementation adds no native read path and selects neither direct pointer
rewriting nor persistent indirection. Both still require architecture-specific
accounting, crash recovery and stock-format/reader compatibility evidence before
a disposable write prototype. See [RESULTS.md](RESULTS.md) for the independently
accepted parent-edge runtime and its qualified stock-display caveat.
