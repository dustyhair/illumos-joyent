# Reviewed RAID-Z research checkpoint

This records the publication status of the diagnostic and offline-planning
milestones. The coordinator confirmed the independent review outcomes below;
V14 validation records and the planner/parent-edge review bundles corroborate
the selected commits and reported results.

| Milestone | Commit | Current status |
| --- | --- | --- |
| V14 supported-profile gate | `110353f075131e743b0f1f6cacc5322f88a6111c` | Sealed TXG 330 runtime validation and independent evidence review passed. |
| Group-only planner v2 | `0fb23c783689af8621f8b6d880ceae148bcdd01d` | Independent source/results review passed; all four independent source choices reproduce. |
| Parent-edge `-W` diagnostic | `1ef884f67adff5c27d7e737ddfeb0591a53dd5b7` | Sealed TXG 330 runtime validation and final independent evidence review passed. |

V14 matched 83 MOS objects, six datasets and 16 directory names, completed 580
original-BP verifications, and passed exact allocation reconciliation with empty
pending/async queues. The reviewed runtime evidence reports read-only nodes,
zero observed writes and preserved image hashes. These are bounded observations
of this disposable fixture, not a universal no-write proof.

The accepted V14 capture SHA-256 is
`fdc62399d14973a6cfc3f3b6e22ce039144f4601a06e423e482f5d5295f626c4`.
This proves only the restricted supported profile. DDT/ZIL contents remain
unenumerated; the earlier DDT-negative case stopped before the absence collector.

Planner v2 produces deterministic destination-group eligibility and gross-byte
accounting, preserving observed sharing and excluding existing replica groups.
It supplies no destination offsets, reservations or physical placement, and
its gross-free bounds are not proof of allocator capacity.

The accepted parent-edge capture SHA-256 is
`eb8b80b66ee9734a3a5370215252300df4f4b617c0d0bea79d1fb828394c68e9`.
It records 24,908 edge nodes, 44 parent observations and six dataset anchors.
Independent raw-parent comparison authenticated 33 distinct parent BP
descriptors and checked the recorded slot memberships. Snapshot/clone aliases
were preserved, including expansion of a clone root born before its previous
snapshot TXG. The selected MOS root matches stock observations before and after;
independent label election and per-replica health were not established.

One focused stock MOS display contains an `(inconsistent)` suffix that other
same-cut listings omit. Source inspection supports an uninitialized display-stat
field as the explanation: `dump_dir()` reads `dds_inconsistent`, while the MOS
path through `dmu_objset_fast_stat()` sets only `dds_type`. This remains a
source-based inference; the suffix is preserved in the evidence and establishes
neither on-disk corruption nor pool health. Acceptance uses authenticated bytes
and cut identity rather than that display suffix.

Parent-edge scope remains current captured MOS/dataset tree pointer slots;
decoded offsets are not physical disk addresses. BP-bearing semantic payloads
and historical roots remain excluded. Exhaustive reverse references,
COW/accounting, crash recovery, stock-reader compatibility and
relocation/rewrite safety remain unproven. All certification, placement and
rewrite-readiness flags remain false.
