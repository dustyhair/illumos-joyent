Namespace retirement is prepared for VM105; not accepted yet.

A Linux reference confirmed that the old namespace's child mounts disappear
when its last member leaves, while file and independently held child mounts
remain usable. Four static/dynamic root/nobody controls pass. They exercise
old namespace-root and target descriptors, self-parent roots, data sharing,
active copied graphs, cwd ENOENT, repeated copying and the existing filter.

The last view release now synchronously unlinks its graph outside process
locks. It retains real filesystem references and defers native unmount work.
All covered-vnode locks are acquired before mutation. A transient reader is
retried before any edge changes. Process exit, clearbrand and unshare release
sites were checked to drop their final view references outside process locks.
The owner's credential supplies zone accounting, including global cleanup of
an lx zone. Namespace graph retirement also marks the unspliced root detached.

Private constructor-order metadata keeps newer bind references before their
sources. The current namespace root becomes a retained mount record in that
order during final release. This prepares cleanup for a future root that pins
an older mount in the same view. Existing public view_destroy still leaves a
busy view usable; only final reference retirement changes graph membership.
No structure layout outside the private lx backend changes.

The accepted native reattachment checkpoint is5071b5dd0e, VM104. lx pivot_root
is still unimplemented, and combined namespace creation still fails. No pool
feature or physical host change is involved.
