# Head-only symbolic allocation and retention ledger

`cow_ledger.py` derives one selected head's logical route from the revalidated
raw `-W` capture and authenticated semantic leaf evidence. A reported route
length or saved graph is never accepted as source evidence. The target must
be a plain file-data leaf, selected by dataset/object/block and an exact
captured DVA selector.

Every public invocation runs the full graph/profile/original-BP verification
validator once, then the same bounded semantic authentication/ownership helpers
as `semantic_references.py`. These helpers are an internal version-coupled
contract. Raw leaf hashes and original BP checksums are rechecked; saved
semantic reports are not trusted.

## The selected route is not the full stock transaction

Starting at the selected logical observation, the ledger follows recorded
parent-node links through exactly one selected dataset bonus anchor to the
selected imported MOS root. Snapshots, ambiguous observations, missing parents,
cycles and routes crossing another dataset refuse. Only that route's child
pointers receive symbolic update intents. One replacement is modeled per
original BP in this scope. Every distinct logical observation and canonical
slot remains visible; sharing never causes all aliases to be updated.
All other dataset roots are frozen. The root-publication intent is not executable.

This enumerates the reference-copy route, not all blocks an ordinary stock COW
would dirty. The captured logical births and previous-snapshot TXG select a
conditional ordinary-stock kill-path classification for each route block:
dataset deadlist path, dataset free path, or MOS free path. No kill operation or
its side effects are executed/simulated. Deadlisting, BPOBJ entry/header changes,
snapshot/unique counters, MOS bookkeeping and deferred frees may expand the
transaction into additional branches. The full transaction replacement count
is null, and `full_stock_transaction_enumerated` remains false.

At sealed TXG 330 the selected route has 12 blocks: the data leaf/file indirect
births 299/306 precede previous snapshot 309 and select the deadlist path;
seven head blocks born at 318 select the free path; three MOS blocks select
the MOS free path. These counts are derived from evidence. They are not an
assertion that a stock transaction writes only 12 replacement blocks.

The selected-route size model has 26 symbolic replica allocations and a
3,309,568-byte conditional bound. All 70 captured allocation intervals
(102,162,432 bytes) remain in the model; its tagged union bound is 105,472,000
bytes. These are selected-route model quantities, excluding transaction
expansion. Forty-five counter/birth/retention obligations remain unresolved.

## Explicit size and replica assumptions

No output buffer, BP checksum, physical pointer or uberblock is materialized.
For the data leaf, the model assumes a byte-preserving copy with original
compression and PSIZE; raw target-payload acquisition/copy verification remains
unresolved. For changed metadata, compression OFF and PSIZE=LSIZE are explicit
size assumptions, not encoded output. Original compression, sizes, DVA replicas
and logical/physical births are retained. New births stay null with mandatory
unresolved obligations.

The new replica count equals the original BP's DVA count. For each non-source
normal RAID-Z group, QSIZE/ASIZE are calculated from modeled PSIZE with the
pinned geometry helper. Each symbolic extent is charged the largest ASIZE
among these cases. Distinct groups are required for one BP's replicas, but
no assignment, capacity proof or reservation is supplied. Insufficient distinct
candidate groups refuse. The demand bound covers the selected route only;
unresolved transaction expansion is excluded.

## Tagged allocations and frozen model images

The entire captured allocated set is retained verbatim in `source-vdev:GUID`
address spaces. Each modeled new DVA has a unique identity and an interval in
`fresh-symbolic-only-never-a-vdev-address`. Coordinates index only the model's
staging arena. Destination vdev and physical offset remain null. Repacking
symbolic intervals is allowed if identities, lengths and disjointness survive.

The invariant is:

`tagged_allocated_after = captured_allocated_before union fresh_symbolic_allocations`

Source intervals cannot disappear, change, be reused or be freed. New symbolic
intervals cannot overlap each other or enter a source address space. The
captured source intervals are checked for bounds/non-overlap, and every
original route DVA must be covered by the retained set. The
after-set must contain exactly that union, without hidden/duplicate extents.
This is a hypothetical tagged-model invariant, not an updated pool bitmap or
physical non-overlap proof. Source bytes, fresh conditional bounds and their
tagged union bound are reported separately.

Decoded BPOBJ leaf bytes, physical/decoded SHA256, the 48-byte header, owner
chains and existing stored descriptors are frozen as captured before-images
and fixed-total images in this hypothetical model. They are not the proposed
after-images of a stock transaction. Actual stock new-entry counts and header
totals stay null: existing descriptors may remain unchanged while new entries
are appended and totals change. A normalized candidate's membership in the
selected route is recorded without changing its stored descriptor. Semantic
rewrites are unsupported; lossy matches never establish historical identity
or authorize rewrites/frees.

The no-free/fixed-header policy intentionally exposes unresolved retention and
accounting work. Each replacement requires old-block ownership, new-birth and
counter-delta obligations. Global obligations cover pool/deflation counters;
MOS retention/deferred frees; selected dataset referenced/compressed/
uncompressed/unique and directory counters; snapshot/clone/deadlist lifetimes;
stock deadlist/BPOBJ/MOS expansion; metaslab/log-spacemap TXGs; replica/capacity
reservation; target-payload acquisition; and checksum/publication semantics.
Captured selected dataset counters and previous-snapshot TXG are before-values.
Unknown after-values/deltas remain null, never guessed as zero. Missing or
claimed-resolved obligations refuse.

All semantic unresolved obligations persist. Keeping allocation bits set or
writing an external retention list does not establish stock-consistent owners.
Dataset and MOS lifetimes need source-backed rules before image publication.
All readiness flags remain false, including transaction accounting, retention,
capacity, placement, rewriting, publication and write-schedule availability.
This ledger neither authorizes nor predicts a writable stock import.

The sealed TXG 330 fixture is distinct from any later disposable checkpoint
baseline. Such a baseline must be bound to its own evidence; it must not be
silently substituted for this cut. Active checkpoint support is outside the
current source capture's supported profile.

## Invocation, bounds and tests

Use the semantic analyzer's capture/status/hash, cut hash, exact DVA selector
and raw-leaf evidence/hash arguments, plus:

```text
--dataset-object 184 --object 2 --blkid 0 --output NEW_OUTPUT
```

The default creates and validates a deterministic symbolic draft. To audit an
untrusted bare `plan` JSON, use `--proposal PATH --proposal-sha256 SHA`. Source
facts/obligations are rederived before checking it. Outputs are created
exclusively after validation. Inputs must be regular non-symlink files.

Upstream limits remain: 512 MiB raw capture, 16 MiB leaf JSON and bounded
semantic decoding. Ledger limits independently cap 64 route observations,
100,000 tagged intervals, 4,096 modeled replicas, two million charged scans/
validation operations and 64 MiB output/proposal. Charges precede records.
JSON parsing, upstream validation, hashing and bounded sort comparisons are
outside the ledger work counter; this is not a hostile-input CPU sandbox.

`test_cow_ledger.py` uses a complete synthetic public-validator-valid capture
with shared MOS buffers, four dataset aliases and seven stored BPs. It covers
alias updates, missing/free/reused source intervals, overlapping/duplicate
replacements, invented physical destinations, missing/resolved counter/birth
obligations, modified deadlist bytes/header/entries, lossy rewrites, incorrect
stock-transaction claims, input bindings, budgets and exclusive CLI output.
Synthetic captures exercise validation contracts, not disk authentication.
