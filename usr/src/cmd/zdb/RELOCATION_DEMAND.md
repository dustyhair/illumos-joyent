# Offline single-destination relocation demand

`relocation_demand.py` reanalyzes an original supported-profile capture, its
SHA-256 and external exit status. It also requires an expected cut digest
(SHA-256 of `paired_intervals.canonical(cut)`) and explicit source/destination
IDs and GUIDs. Both vdevs must be distinct normal RAID-Z members of that exact
captured topology. It never opens a pool or device, allocates space, returns
destination offsets, rewrites pointers, publishes a map, or invokes a mover.

```
python3 relocation_demand.py capture.jsonl --sha256 CAPTURE_SHA \
  --exit-status capture.exit --cut-sha256 EXPECTED_CUT_SHA \
  --source-id SOURCE_ID --source-guid SOURCE_GUID \
  --destination-id DEST_ID --destination-guid DEST_GUID --output demand.json
```

The output is created exclusively. Exit 0 means the limited single-destination
contract has no identified refusal; it does not establish capacity or movement
safety. Exit 3 writes a complete demand report with explicit contract refusal
reasons. Invalid evidence, identities, unsupported geometry or file errors exit
2. Inputs must be regular nonsymlink files; capture input is bounded to 128 MiB.

Physical extents are grouped by source top-level vdev identity and exact byte
range. Full BP words and descriptors must agree; partial overlaps and conflicts
are refused. Every observed reference retains its sequence, bookmark, DVA slot,
complete original BP record and verification result. Multiple distinct DVAs
remain separate physical allocations even if BP words match. Shared references
to the same compatible extent count once for destination demand. This is an
inventory grouping rule, not ZFS deduplication.

Sizing uses the compressed physical payload P and actual destination ashift,
child count and parity, following pinned `vdev_raidz_asize()`: sector-round P,
add parity sectors per data row, then round to parity+1 sectors. A compiled
pool-free harness calls the actual libzpool vdev operation across all supported child counts through 255.
Supported bounds are ashift 9..16, parity 1..3, children 2..255 and P <=16 MiB.
All aggregate byte totals must fit uint64. Tests include a destination geometry
that changes required bytes; source allocation size is never reused blindly.

A destination already containing another DVA of any selected BP refuses the
whole single-destination contract. No alternate destination is silently chosen.
An empty source is explicitly reported as no work/refusal. Demand greater than
captured gross free space, or an extent larger than destination asize, is also
refused. Gross free bytes use captured metaslab lengths minus reconciled
allocation bytes. They exclude address space outside captured metaslabs but
are not allocator-available capacity: fragmentation, reservations, slop and
allocation policy are unproven.

This is base demand only. Rewriting ancestors, accounting metadata and
transaction/crash-recovery overhead can require additional space. Hole and
embedded references have no standalone extent; the structurally checked empty
origin head has none either. Other-vdev replicas remain visible in each BP.

Proven evidence includes original BP words, observed logical bookmarks/DVA
slots, same-cut allocation reconciliation and checksum-valid logical recovery.
It does not include exhaustive reverse references (traversal is birth-pruned),
authenticated parent byte offsets/slots, shared ancestor graphs, update order,
per-DVA readability, destination reservations, fresh-state exclusion or recovery
protocols. All existing certification flags, `pointer_rewrite_ready`,
`placement_performed`, `movable_candidate` and `capacity_proven` remain false.

Accepted TXG330 regression results and per-pair refusal evidence are included
in the review bundle. The accepted capture is bound to SHA-256
`fdc62399d14973a6cfc3f3b6e22ce039144f4601a06e423e482f5d5295f626c4`.
Its independent review confirmed only the restricted supported profile, not
full DDT/ZIL enumeration or removal safety. No new runtime/pool test is part
of this planner change.


## TXG330 offline result

All 12 ordered pairs among captured top-level vdevs 0..3 refuse the
single-destination contract because selected BPs have other replicas on the
destination (9..36 conflicting observed references per pair). Gross free bytes
exceed base demand for every pair; that does not override replica separation.

| Source vdev ID | Physical extents | Base demand bytes |
| --- | ---: | ---: |
| 0 | 160 | 24,715,264 |
| 1 | 175 | 24,838,144 |
| 2 | 171 | 25,894,912 |
| 3 | 178 | 26,714,112 |

All four captured geometries match, so destination demand equals source unique
bytes here. This fixture has no repeated source-extent observations; synthetic
regressions separately exercise retained reference multiplicity. No
multi-destination fallback is implemented. Run the bundled `run_capture.py`
with the accepted-capture directory and a new output directory to reproduce
all 12 full reanalyses; `summary.json` records the exact IDs, GUIDs and cut hash.


## V1 P2 correction

Distinct source extents in different DVA slots of the same original BP cannot
be assigned to one destination group, even when that group holds no existing
replica. `planned_replica_colocation` refuses this case while retaining each
physical extent and its demand. Repeated observations of the same exact extent
are not additional replicas and remain grouped once. The native geometry sweep
now includes every supported child count through 255 (sizes capped at 16 MiB).

## Group-only extension

`--all-destinations` replaces `--destination-id` and `--destination-guid`.
The same raw profile is revalidated once, then all other captured normal RAID-Z
groups are evaluated using the same single-destination sizing and grouping.
Every source extent lists eligible groups with their geometry-specific demand
and excluded groups with explicit reasons. Unsupported topology classes and
the source group are also recorded separately. No eligible groups is a refusal;
multiple distinct source replicas of one BP are conservatively refused before
assignment, while their separate physical demand and observed references remain.

The deterministic accounting heuristic orders extents by fewest eligible
groups, then largest possible destination demand, then numeric source address.
It chooses a group that stays within captured gross free bytes, minimizing
resulting assigned bytes and breaking ties by numeric group ID. Every assignment
contains only destination group ID/GUID and byte demand, never a destination
byte offset. Each group's used and remaining gross-byte bound is emitted.

`group_assignment_complete` means this accounting heuristic assigned every
extent within those gross bounds. It is not physical placement, an allocator
reservation, capacity proof, or rewrite readiness. `eligibility_complete` means
all captured source extents were evaluated, not that reverse references are
complete. Empty sources and missing eligible groups refuse. Greedy exhaustion
is `greedy_capacity_assignment_unresolved`, not a proof of infeasibility;
partial accounting assignments remain explicitly incomplete. Tests include a
known feasible alternative that the greedy heuristic misses.

Group mode returns exit 0 for complete group accounting, exit 3 for an explicit
refusal or unresolved assignment, and exit 2 for invalid inputs. All safety
flags remain false. The v1 single-destination mode remains available with its
P2 replica co-location correction. Neither mode broadens provenance: authentic
parent locations, shared ancestor relationships, exhaustive reverse references,
reservations and crash-recovery design remain missing.

The accepted TXG330 group replay evaluates four independent removal choices,
not simultaneous removal of four groups. Every source extent has an eligible
group and each independent group-only assignment fits the captured gross bound.
The bundle contains per-source eligibility, exclusions, full references and
assignment records. The original 12 single-destination refusals remain valid.
