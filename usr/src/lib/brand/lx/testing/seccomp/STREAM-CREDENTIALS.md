# Native stream credentials

This change extends the existing native SO_RECVUCRED option to connected
Unix stream sockets. It uses the credential and PID already held by each
STREAMS message, the existing connected-endpoint serializer and the existing
SCM_UCRED format. It adds no common structure fields or separate lx credential
store. The lx SO_PASSCRED mapping now uses the native option for streams and
datagrams; the cached-peer fallback and its private flag are removed.

When credentials are requested, the M_DATA fast path queues through the
ordinary serialized data path. That path adds a native option header without
copying payload blocks. Protocol options and MORE flags are retained. The
allocation failure path uses existing tl_memrecover queueing. Invalid native
option bounds fail before copying.

Descriptor messages keep their existing preallocated data block and ownership
callback. This follows the existing datagram path. kstrputmsg holds a duplicate
reference, so requiring db_ref1 is incorrect. VM50 DTrace with -x nolibs showed
ref2, len80, capacity352 and EPROTO71 at the rejected guard. The initial test
failure returned zero bytes, not CTRUNC. The corrected path retains the shared
native header instead of reallocating it and losing descriptor ownership.
Ordinary messages use allocb_tmpl/reallocb, preserving native credential holds.

## Validation

Native and lx modules build cleanly in release/debug configurations; cstyle
and git diff --check pass. The native control uses the XPG socket interface
and ucred APIs and checks child PIDs and per-message real/effective UIDs before
and after setuid. It is bounded by alarm20. The initial helper build exposed
missing XPG declarations; _XOPEN_SOURCE600 and ucred_geteuid corrected it.

Linux static/dynamic controls each pass49 top-level cases. New tests exercise
fork, write, writev, changing sender IDs on a shared stream, the option getter,
and descriptor passing with automatic credentials and MSG_CMSG_CLOEXEC.
VM50 passed the native control and48/49 static cases. Its descriptor failure
was diagnosed with a small unscoped control and DTrace; default DTrace libraries
failed scsi.d compilation against the mixed platform, while -x nolibs worked.

VM51 full canonical acceptance passes, with Native tl97060e5a00, brand5e7d4b1d73,
native control97060e5a00 and probe5e7d4b1d73. The Native control, both full drivers, scoped49 static+49 dynamic,
capability19+19, existing backend/map/mount/ABI/i386/enforcement/filter/signal/
fork/exec/thread suites and post-zonehalt credential lifetime pass. Actual
Codex0.166 version passes under the filter; no normal sandbox workflow is
claimed. Runtime tl/native-control hashes match staging; pool export0 and
synced poweroff complete acceptance. Evidence is
lab/evidence/boot-51-native-stream-acceptance.log. Retained common2676283bce,
proc65b024e439, ktestd6149b4432, LOFSa8b, VM30 emulation libraries and VM46
support remain over the mixed May base. Debug is compile-only. Launcher
lx-sandbox/launch-lab SHA256
ece2f8445e5745dd320cc1f837a51ca7dc206f9798292dfdb41f71ddffc16739.

Runtime SHA256:

```
tl a129e6007cf75e6cddeebe6c8d6d442a68dc8f8a4e75aa0f2ed47635c77d60dc
lx_brand 1adbffc8ad970d608b8f4ba3d2c6cc0c09e58658f1ec7dec617a4b9f9a93ef01
native_control 14924d8abaa6476e777c40eeea6d26668e05fa65fbf128fbbfc4481a0d09da27
```

## Remaining work

Accept/listener inheritance and live/exited preaccept senders are now
validated in RMDIR-ACCEPT.md. Option changes while data is queued still need
separate controls and implementation review. Stream coalescing, MSG_PEEK/WAITALL,
credential changes while flow-controlled, option changes on queued messages,
Native ancillary layout assumptions and SCM-specific 32-bit controls need
further coverage. Administrative PID target/lifetime semantics are still open.
No public user namespace is enabled. Filesystem permission prerequisites,
mount/PID/IPC/network isolation, a current stable Codex installation and an
actual normal sandboxed coding workflow remain required. This is still a
mixed test platform, not a complete platform rebuild or host deployment.
