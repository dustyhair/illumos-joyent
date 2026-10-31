VM116 accepts the private PID namespace map backend. Linux CLONE_NEWPID
remains rejected; process activation, signal/wait/proc translation and PID1
lifecycle integration are still required before bubblewrap can use it.

The backend uses native id_space_t for private numeric IDs, AVL trees for
visible/backing lookup and native pidlock for atomic ancestor publication.
An immutable owner credential and an explicit user-namespace reference
preserve ownership. Creation checks the owner's CAP_SYS_ADMIN and charges
a separate zone-wide PID namespace budget. The default budget is 1024 and
hierarchy depth is limited to 32. There is no private PID allocator, worker
thread or reaping engine. Native ID-space destruction is documented as
context-independent, and native freeproc already releases credentials under
pidlock. Namespace/map destruction follows those native contracts.

Reservations remain invisible until publication. Failure rolls back every
allocated ancestor ID. An unpublished PID1 reservation can be retried;
shutdown and published PID1 retirement prevent replacement. Each map holds
one native PID identity for its whole ancestor chain. Retaining that object
does not keep a task alive or grant lookup/signal authority. Parent and
sibling maps cannot be used interchangeably.

All 16 smoke and 16 full paired-process cases passed across static/dynamic
binaries, root/nobody, ordinary/userns-guarded credentials and worker exit
or nonleader exec. The 64 primary/peer fixture processes completed normally.
The tests check ancestor/sibling views, invisible reservations, duplicate
and foreign-zone publication, ancestor exhaustion rollback, capability and
budget rejection, depth32, init reservation abort/retry, shutdown gates,
retired native identity retention and final release under native pidlock.
All 96 hold/retired/release test calls and both idle assertions after every
case passed. PID namespace objects and charges returned to zero.

The full earlier suite passed, including all 80 user/mount clone cases,
16 native PID identity cases, 70 mount-entry, 12 bind, two limit,
12 recursive, eight private, 12 tmpfs, four statfs, eight readonly,
eight detach, four retirement, eight pivot and two shared/readonly root
cases. Four native reinsert tests cover 16 cycles.

VM115 faulted on the first rollback because an unbraced if guarded only the
ASSERT in the multi-statement native PID_RELE macro. The actual release then
used a null identity. Disassembly identifies the fault at map_rele+d3,
loading pid_ref from NULL+0x20. The fix puts the whole macro inside braces.
VM116 reran the unchanged failing test and all regressions. VM115's panic,
unclean shutdown and pool state are recorded separately, without acceptance.
VM116 imported the disposable pool successfully; final zpool status reported
all pools healthy, export returned zero, filesystems synced and QEMU exited.

Clean release/debug lx_brand and namespace-ktest builds passed without
warnings/errors; touched-source cstyle and diff checks passed. VM116 checked
44 artifact hashes before running. Release modules use the retained VM114
genunix and surrounding platform/LOFS/tmpfs. Debug builds are compile-only;
this is not a whole-platform acceptance result. No physical host changed.

Initial backend source da001592deff694589d5a002b2941c8d9b1efbaa.
Accepted kernel/runtime source 4a18764dffcabe13c79af302218fe202b02c9b29.
Test source 801637e0fb87ce7df5cd9b89ff007fab3e07d879.
Console boot-116-pid-maps-acceptance.log.
Console SHA256 df66d8717f971cdede5357adc9f86ea58495775a1c77967f778eda9c9534f9f2.
See pid-maps-results.json for artifacts, counts and teardown evidence.
