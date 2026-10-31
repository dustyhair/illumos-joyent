Current root-switch controls are mntns_pivot.c, source d8c7786885,
Linux controls passed eight cases, SHA256 6fe56d4a82c88ab88663771795b5b73169f8b1ddffaabb41d0f256262dd2059a.
Eight static/dynamic root/nobody allow/deny controls pass. The kernel does
not yet implement pivot_root. VM106 retirement full suite passed and the VM shut down cleanly.

Read native vfs_add/vfs_detach and Linux /build/linux/fs/namespace.c
path_pivot_root4590-4738 during this turn. Relevant implementation details:

- Linux resolves new_root and put_old as directories before may_mount.
  It rejects a new root or put_old in the old root mount with EBUSY,
  validates mounted new root, and checks both graph reachability relations.
  put_old may be in a child mount under new_root. Equal operands work.
- lx's conservative first implementation must require one participant,
  current process root==view root, one LWP, no CLONE_FS/ptrace/SVFORK.
  lx_mntns_current supplies one hold in addition to the process hold.
  Before acquiring the prepared-state extra hold, lv_refs==2 is a safe
  conservative gate; refuse other members/temporary owners. Revalidate
  under p_lock before handoff. Do not silently leave other roots stale.
- Existing lx_mntns_proc_swap validates expected old root/cwd and transfers
  prepared references atomically under p_lock. A kernel-restricted process
  already satisfies its activation guard. Preserve cwd unless it is the
  exact old root, which Linux updates to new root. Release returned old
  references outside process locks and after the graph transaction.
- Resolve both operands with actor credentials and held vnodes. New root
  must be an exact active tracked mount root. put_old must be owned by an
  actual descendant mount of new root. Cached/path-prefix equality alone
  does not establish ancestry. Allocate all rebased names before commit.
- Acquire covered-vnode write locks for new root's current covered vnode
  and put_old before mutation. Acquire needed VFS locks before proc_swap,
  so no fallible operation remains after publication. Lock failure must
  leave root/cwd and graph unchanged. Do not sleep under p_lock.
- Native vfs_detach(new) transfers splice ownership to an unspliced hold.
  Native vfs_add(put_old, old) acquires a new splice hold. Release the old
  unspliced ownership hold, as ns_reinsert already does. vfs_add resets
  RDONLY/NOSUID/NOMNTTAB flag subsets, so restore exact prior flags while
  clearing VFS_DETACHED. Preserve zone ownership/reference and timestamps.
  New current root clears DETACHED and remains an unspliced private root.
- Promote new root's existing record to lv_root/lv_vfs, reuse that record
  for old root with its original constructor order. Reinsert by order.
  Record count remains constant. Retirement now includes current root in
  reverse constructor order, preventing a bind-root/source cleanup cycle.
- Rebase active new-root descendants by stripping the new root prefix;
  all remaining old active graph paths move beneath the new put_old path.
  Use native ancestry to distinguish hidden older stacks from descendants.
  Detached record names do not become active or enter namespace copies.
- Native vfs_mntpt is currently an immutable constructor prefix for private
  trees. Existing path helper strips that prefix then adds lm_path. Avoid
  changing it casually: cached vnode paths may retain the old prefix.
  Candidate original/current names must be actor-forward-looked-up and
  require exact vnode identity. Do not accept backing-vnode equality.
- Existing direct private LOFS root dotdot and synchronous retirement
  changes must be accepted before runtime root switching. Public busy
  view_destroy leaves its graph usable, as ns_views/ns_clone tests require.

After root-switch acceptance, actual Codex's combined clone0x78020011 still
needs NEWNS/NEWUSER plus PID/IPC/network support and private proc/devpts.
Last actual trace VM100 returned EINVAL twice. Do not claim full bubblewrap
or bypass inner isolation. No physical host deployment has been performed.
