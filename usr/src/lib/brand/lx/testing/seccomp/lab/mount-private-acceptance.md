Private and masterless slave mount operations passed VM94.

Every permitted constructor produces private mounts. There are no peer or
master links, so PRIVATE and SLAVE preserve that state, including recursive
requests. The implementation validates namespace owner capability and exact
mountpoint. Shared and unbindable modes remain unsupported.

All eight static/dynamic root/nobody allow/deny cases passed on Linux and lx.
Four peer observers confirmed that a mount in a copied namespace does not
propagate into the original view. Exact mountpoint validation, invalid or
ignored optional arguments, copied-view changes, effective capability removal,
filter retention and cleanup passed.

The full prior suite, 70 entry cases, 12 bind/fd cases, two mount-record limits,
12 recursive/device cases and four recursive observers also passed. Both final
idle checks passed. All three experimental switches were reset, retained views
were zero, the zone halted, the pool exported with status zero, filesystems
synced and QEMU exited. Module and evidence hashes are in
mount-private-results.json. Runtime uses a rebuilt release brand with retained
platform components; debug was compiled only. No physical host was changed.

The Codex 0.166.0 fixture still receives EINVAL twice for combined clone flags
0x78020011. Full current Codex sandbox acceptance remains outstanding. Next are
public tmpfs creation and ownership, bind remounts and the remaining bubblewrap
filesystem and namespace setup.
