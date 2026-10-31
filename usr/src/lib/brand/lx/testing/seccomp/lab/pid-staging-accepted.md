VM117 accepts native PID binding and commit staging. This is a backend
checkpoint. The process hooks are not connected, and CLONE_NEWPID remains
rejected. Codex's complete bubblewrap request still needs PID, IPC and
network namespace support.

ID reservation, native identity binding and successful commit are separate
operations. Native binding accepts an owned PID reservation before an lx
task hash entry exists, checks its zone context, and retains a PID reference.
Bound entries stay hidden. Releasing an uncommitted initial binding permits
PID1 to be retried; releasing a committed init cannot replace it. The atomic
registered-task publication adapter keeps its earlier behavior.

Commit reports whether every ancestor namespace is still alive. If one died
between binding and commit, the identity remains available for ancestor wait
and zombie reporting, but commit returns false. The future native caller must
kill that child before user execution. That caller behavior is not implemented
or accepted by this checkpoint.

The native source supplies the needed commit point. b_lwpdata_alloc runs
before p_lock, so binding can fail through normal native allocation unwind.
b_initlwp_post enters with p_lock held and may temporarily drop it.
b_forklwp precedes a final contract_process_fork failure point. After all
fork failures are impossible, common/os/fork.c calls b_lwp_setrval before the
child can run, with no pidlock or process lock held. lx_setrval already
implements that callback. No additional native fork callback is needed.

All 16 smoke and 16 full paired-process cases passed, each including the
new staging checks: an owned native token with no lx hash entry, wrong-zone
rejection, hidden binding, abort/retry of PID1, successful commit, committed
init replacement denial, death after binding and before commit, retained
identity for ancestor wait, and final release of the original native token.
The prior map tests and all earlier regressions also pass, including all
80 namespace-clone and 16 native PID identity cases. Both idle assertions
passed after every map case. The 64 primary/peer fixture processes exited
normally; final PID namespace and mount-view counts were zero.

Clean release/debug lx_brand and namespace-ktest builds passed without
warnings/errors. cstyle and diff checks passed. The VM verified 44 artifacts
before execution. Release modules retain VM114 genunix and the surrounding
platform/LOFS/tmpfs; debug is compile-only, not whole-platform acceptance.
The zone halted, the guest pool was healthy and exported with status0,
filesystems synced and QEMU exited. No physical host changed.

Kernel source 915a6d5386ca02bccaf4b8052c9cb9268d7f352e.
Runtime/test source 8f9655f4383e0a3677d1142dca2d517737db2e9d.
Console boot-117-pid-binding-staging-acceptance.log.
Console SHA256 268724301c463a777c97ad04fcb6432516605cc459f6179ef14cd4b7d6237f72.
See pid-staging-results.json for exact artifacts, counts and teardown.
