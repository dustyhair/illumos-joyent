VM109 accepts restricted amd64 pivot_root in a sole-participant private
mount namespace. Both bubblewrap root switches work, including equal "."
operands. Paths resolve with the actor's credentials and require owned,
tracked mounts and actual graph ancestry. Root/cwd handoff uses the existing
process swap helper. Native VFS detach/add preserves ownership and held
filesystem references. Constructor order controls eventual reclamation.
Shared namespaces return EOPNOTSUPP; multithreaded, CLONE_FS, ptraced and
vfork process states remain excluded by the handoff checks.

Native publication needs an absolute mountpoint for zone lookup. NOSPLICE
construction supplies a descriptive marker instead. The implementation
retains the original cached-vnode prefix in optional private metadata while
setting the native publication path through vfs_setmntpoint. Prefixes transfer
with roots and are released during reclamation. Actor forward lookup still
requires exact vnode identity. No common VFS or native ABI change was needed
for this milestone.

Eight static/dynamic root/nobody allow/deny cases pass in the full suite and
in an additional smoke run. Two supplemental dynamic cases verify refusal
while a child shares the namespace, successful switching after child exit,
a readonly bind root retaining an older source, and readonly policy surviving
namespace copying. Existing nosuid/nodev restrictions are preserved when
sealing the bind. Original zero-charge cleanup assertions remain unchanged.
Eight original Linux controls and eight readonly bind-root controls pass.
The full previous regression suite and six native reinsert tests pass.

VM107 exposed the native publication path requirement and VM108 exposed the
constructor-prefix regression. Both diagnostics and initial failed logs are
preserved. Final flags and views are zero, the zone is stopped, pool export
returns zero, filesystems sync and QEMU exits. Exact source/artifact/controller
and evidence hashes are in pivot-root-results.json. Clean release/debug
lx_brand builds pass; runtime retains the earlier platform modules and uses
release binaries. Whole-platform acceptance is not claimed.

The remaining runtime blocker is Codex's combined namespace clone. NEWNS
clone entry, PID/IPC/network namespaces, private proc/devpts, and complete
current Codex sandbox coding/PTY acceptance remain. The last actual Codex
clone trace is VM100 EINVAL, not a post-pivot Codex acceptance test. No physical
host deployment or ZFS pool feature change has been made.
