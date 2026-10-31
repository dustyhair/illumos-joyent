Lazy detach implementation prepared for VM101; not yet accepted.

Only private-view MNT_DETACH is added. Normal private unmount, unsupported
flag combinations and detaching the original namespace root stay rejected.
The Linux actor must retain CAP_SYS_ADMIN in the owning user namespace.
Lookup uses actor credentials and requires the exact tracked mount root.
All selected covered-vnode locks are acquired before any graph edge changes.
Selection follows actual mount ancestry, so an older tree hidden by a newer
mount is preserved when the top mount is detached.

The native VFS helper records VFS_DETACHED, exchanges its splice reference for
an unspliced lifetime reference and removes the existing graph edges. It keeps
native zone accounting and existing vnode references. The common pathname
walker treats a retained detached root as its own parent for dotdot. Ordinary
and forcibly unmounted native roots retain their existing behavior. No vnode
or VFS structure layout changed. No ZFS feature or pool-format change is made.

The lx backend retains its bounded mount records and charges, skips detached
records in namespace copies and allows dounmount cleanup without a covered
vnode. Bind sources must belong to the actor's active private graph. Native
VFS_DETACHED identifies removed targets after namespace replacement without a
cached-path or zone-pointer guess. Propagation and readonly changes reject
removed mounts. Open file and directory references remain usable.

Linux8 controls pass across static/dynamic root/nobody allow/deny. They cover
hidden stacked mounts, recursive child unlink, held file/directory access,
self-parent detached roots, fd statistics, cwd ENOENT, source/target rejection,
namespace copies and denied capability. Retained records remain charged until
view retirement, as hidden overmount records already do. Prompt per-mount
reclamation during an active view is not implemented.

Separate Linux8 root controls follow Codex's two-pivot sequence, including
equal new_root/put_old, old cwd references and lazy detach. They pass as Linux
references only. No lx pivot implementation exists yet. Native reattachment
must preserve vi_zone_ref instead of reinitializing a live zone reference.
The accepted readonly checkpoint is fd26c8caa7, validated in VM100. Full Codex
combined namespace clone remains unsupported. No physical host deployment.
