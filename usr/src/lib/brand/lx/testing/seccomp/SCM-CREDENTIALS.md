SCM credential exact-size validation
===================================
Source checkpoint 5bf39b0ba6, branch lx/codex-sandbox.

ltos_conv_ucred now rejects any credential payload whose length differs from
sizeof(lx_ucred_t), during the sizing pass before any UID/GID/PID payload read.
No native transport or credential authority policy is changed.

Regression tests send payload lengths 0 through 16, except the valid 12-byte
credential size. They cover stream and datagram socketpairs, before and after
dropping all capabilities, and verify that no rejected message was queued.
The test also runs within the mapped identity suite.

Linux controls: GCC -std=c11 -Wall -Wextra -Werror -O2 -pthread, both static
and dynamic builds. Full43-case mapped identity controls pass for each build.
Malformed-only controls pass as ordinary user and root. Source reference:
/build/linux/net/core/scm.c SCM_CREDENTIALS exact CMSG_LEN(sizeof(struct ucred)).

SmartOS: clean release/debug lx_brand build and cstyle/git diff --check pass.
Disposable VM47 full drivers PASS: DRIVER_EXIT=0 and USERNS_DRIVER_EXIT=0.
Static and dynamic scoped identity suites each pass43 cases, capability suites
each pass19, plus prior backend/ABI/i386/filter/signal/fork/exec/thread cases.
Runtime lx_brand SHA256 f29e2fbc2d19fdc9bdbfd68d3155ba8114189075325a3b1adb570cf9112f0662.
QEMU paused on local ENOSPC during extraction; resumed the SAME VM after
compressing preserved old diagnostic disks. Pool export0 and synced poweroff.
No production host deployment. Evidence: lab/evidence/boot-47-scm-length-acceptance.log.

Retained common2676283bce/proc65b024e439/ktestd6149b4432/LOFSa8b, VM30
emulation libraries and VM46 support over the mixed May base. Debug compile
only; full-platform rebuild and actual Codex sandbox workflow remain pending.

Still pending: SCM_CREDENTIALS sender authority checks, sender/reader namespace
ID translation, real versus effective ID defaults and per-message stream
credential semantics. This length check does not implement those behaviors.
