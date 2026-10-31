VM97 accepted private tmpfs construction and restricted filesystem statistics.

All 12 tmpfs cases and all four statfs/fstatfs cases passed across static and
dynamic binaries as root and nobody. Matching Linux controls passed. Initial
root ownership is supplied through small kernel-only tmpfs mount arguments.
Native tmpfs initializes the owner before normal mount publication, preserving
native zone memory accounting. No generic VFS accounting or ZFS change was
needed. Default and explicit modes, mapped IDs, relative and descriptor targets,
invalid input, size limits, released pages, zero size, percentage rounding,
noexec and denied-capability behavior passed. A 1 MiB native cap allows1044480
file-data bytes because native tmpfs metadata shares that cap. No size check
was waived. Four peer cases verified copied namespace sharing and isolation.

Restricted amd64 statfs/fstatfs now use actor lookup or held descriptor vnodes
and native VFS statistics in the kernel. Mount flags come from the observed
vnode, including private LOFS restrictions. Tests cover covered descriptors,
magic paths, pipe/socket filesystems, errors, namespace copies, no-admin reads
and seccomp filters preceding lookup. Unguarded legacy emulation and the native
ABI guard remain unchanged. VM96 diagnosed the previously missing kernel
statfs handler; no guard exception was added. VM95's native boot regression
was fixed by preserving empty legacy kernel tmpfs argument blocks.

All prior suites also passed: 70 namespace entry cases, 12 bind/fd cases, two
mount-record limits, 12 recursive/device cases and eight private/slave cases.
Both final idle checks passed. All switches and retained views were zero, the
zone halted, pool export returned zero, filesystems synced and QEMU exited.
Source, artifact and evidence hashes are in mount-statfs-results.json. Runtime
uses rebuilt release modules with retained platform components; debug was
compiled only. No physical host was changed.

The npm latest tag was checked on 2026-10-31 and remains Codex0.166.0. That
fixture still gets EINVAL twice on combined clone0x78020011 with all three
experiments enabled. Readonly bind setup, root rearrangement, private proc and
devpts, mount metadata, detach, remaining namespace creation and complete
Codex coding/PTY acceptance remain unfinished. Readonly tmpfs creation, inode
limits and some other Linux mount options also remain unsupported. Overflowing
size arithmetic is rejected explicitly, even where Linux accepts a wrapped
maximum-size argument.
