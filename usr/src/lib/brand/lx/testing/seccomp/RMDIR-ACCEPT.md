# Kernel rmdir and accepted-stream credentials

Filtered lx tasks now handle rmdir in the kernel through the existing vnode
removal path. Both syscall tables select lx_rmdir. It preserves native path
lookup, filesystem permissions, mount-root protection and error handling,
with EEXIST translated to Linux ENOTEMPTY. No user-mode emulation or cwd
mutation is involved.

The appended RMDIRECTORY_CWD kernel removal mode permits removing the caller's
current directory. vn_removeat normalizes it to ordinary directory removal,
then supplies the parent vnode as the filesystem cwd discriminator. This is
also used by existing lx autofs code. The actual cwd remains held and intact.
All pre-existing enum values and native callers retain their behavior. No
common structure fields are added. Ordinary native rmdir still rejects its
current directory with EINVAL; a native control verifies that behavior.

Native Unix acceptors also inherit the listener's SO_RECVUCRED setting. Live
senders' queued messages flow through the existing serialized data path after
accept. A closing sender attaches the native credentials already held by the
message before it is queued with the pending connection. Updating the local
header pointer/length preserves the newly created protocol header. Receiver
credentials do not depend on the sender remaining alive.

## Validation

Linux static and dynamic controls each pass52 top-level cases, compiled with
-Wall -Wextra -Werror. New controls cover live and exited senders queued before
accept, inherited option getters, two sender identities and a descriptor
queued by an exited sender. Descriptor ownership, contents and CLOEXEC are
checked. rmdir controls check empty/missing/null/file/symlink/dot/non-empty
paths, removal of cwd, unchanged cwd inode and ENOENT from getcwd afterwards.

VM52 passed prior regressions but both new accept cases were killed with
SIGSYS during rmdir cleanup. The syscall table confirmed rmdir still selected
user-mode emulation. The filter was not relaxed. Common/module source
7c59b0f249 adds kernel rmdir; Native tl600d5200ad implements accept handling;
probef14b64bc5e plus7c59 and native-control3cc2fd8d56 supply runtime controls.

VM53 passes both full canonical drivers, Native stream and legacy rmdir
controls, scoped52 static+52 dynamic, capability19+19, existing backend/map/
mount/ABI/i386/NNP/BPF/filter/signal/fork/exec/thread regressions and post-zonehalt
credential lifetime. Actual Codex0.166 version passes under the filter; its
normal sandbox remains unavailable. Runtime hashes match staging, followed
by pool export0 and synced poweroff. Console evidence is
lab/evidence/boot-53-rmdir-accept-acceptance.log. Builder provenance is
/build/RUNNING_TESTS.md.

Clean release/debug unix/genunix/lx_brand/lx_proc/elfexec builds pass. Native tl
was separately clean-built in release/debug. Runtime SHA256:

```
genunix 5b3f917c367f1dbbd6f264d7df8739994329a677037ab2df260db07eb26bac3b
lx_brand 5b1095890d6cfeb508f9cb30aafc7a5437d5750bb866a056e0a9f314001f65b3
tl 313d425a11f6957b72c1412fd80a81e3f1b1a18091427c2789ec114b8c05747d
native_control 1438f0cec5565f363c817af74a56f65882cc3614c58b14d982ef611bb28a8227
```

unix/proc/elfexec rebuilt from7c59 are byte-identical to prior runtime modules.
LOFSa8b, ktestd6149b4432, VM30 emulation libraries and VM46 support remain
retained over the mixed May platform base. Debug is compile-only; no complete
platform or SPARC build is claimed. Launcher lx-sandbox/launch-lab SHA256
ece2f8445e5745dd320cc1f837a51ca7dc206f9798292dfdb41f71ddffc16739.

## Remaining work

The existing unlinkat AT_REMOVEDIR path still retains native cwd restrictions.
Changes to SO_RECVUCRED while data is queued, stream coalescing/PEEK/WAITALL,
flow-controlled identity changes, Native ancillary layout validation and
SCM-specific32-bit controls remain. Administrative PID targets/reference
lifetime, filesystem capability permissions and initial UID0 map authority
are unfinished. Public namespace entry and mount/PID/IPC/network isolation
remain disabled or incomplete. A newest stable installation and actual
normal sandboxed coding workflow, plus full platform validation, are required
before host deployment. Production hosts and the unrelated GPU tree are
unchanged.
