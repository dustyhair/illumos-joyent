VM102 accepts private lazy unmount with retained native mount lifetimes.

All eight lx detach cases and matching Linux controls passed across static
and dynamic binaries as root and nobody with allowed/denied capabilities.
The same eight cases passed in the early smoke run. The full regression
wrapper passed, including 70 entry cases, 12 binds, two record limits,
12 recursive/device cases, eight private/slave cases, 12 tmpfs cases,
four filesystem-statistics cases and eight readonly-bind cases.

The native vfs_detach helper marks VFS_DETACHED, exchanges its splice hold
for an unspliced lifetime hold and removes existing mount graph links.
Native zone accounting and vnode references remain live. The pathname walker
treats a retained detached root as its own parent for dotdot. Ordinary and
forcibly unmounted native roots retain their existing behavior. No VFS or
vnode structure layout changed; no ZFS pool format or feature changed.

The lx backend validates actor authority and the exact mount root, selects
actual descendants and locks all covered vnodes before mutation. A hidden
older stack is preserved when only its covering mount is removed. Open file
and directory references remain usable, child mount edges disappear, covered
targets reappear and namespace copies omit removed mounts. Detached sources,
targets and propagation changes are rejected, including after namespace
replacement. Bounded records stay charged until view retirement. Normal
private unmount, other flag combinations and original-root detach remain
unsupported. Native reattachment and pivot_root are not implemented yet.

VM101 found changed error returns in the older disabled-experiment guard.
Those errors and guard-before-lookup behavior were restored. Existing
assertions were preserved. The smoke fixture initially omitted ktest driver
initialization; its corrected run and the complete wrapper then passed.

Both idle checks passed before halt after every full and smoke detach case.
The controller reset all switches and halted the zone. A redundant later idle
query could not find the halted zone, as those checks require a live fixture.
That invocation is recorded as invalid, not counted as a pass. Retained views
were zero, pool export returned zero, filesystems synced and QEMU exited.
Exact sources, artifacts and evidence hashes are in mount-detach-results.json.
Release/debug genunix and brand builds passed; runtime retains platform
components and debug was compile-only. No physical host changed.

The last actual Codex0.166.0 trace in VM100 still failed combined namespace
creation twice with EINVAL. Lazy detach is a prerequisite, not full bubblewrap
acceptance. Root switching, private proc/devpts, namespace/mount metadata,
PID/IPC/network namespaces and complete coding/PTY acceptance remain.
