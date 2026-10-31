Linux PID namespace behavioral controls

These are reference tests for PID namespace work still to be implemented
in lx. They do not claim lx namespace acceptance or enable CLONE_NEWPID.

On codex-testbox, linux-control.sh compiles static and dynamic C11 pthread
binaries with Wall/Wextra/Werror. It runs lifecycle and init-death cases
as root and nobody under the existing lxc-unshare profile. All eight cases
passed without global policy changes. linux-results.json records the exact
source commit, source/binary hashes and output hashes.

The lifecycle case checks that unshare changes the PID namespace for
future children while leaving the caller's PID unchanged. The first child
is PID1 with parent PID0. It establishes a session and process group,
mounts a private proc filesystem, checks its self link and hidden outer
parent, forks PID2, receives a caught signal and checks waitid/WNOWAIT and
waitpid IDs/status. A pthread retains process PID1 with its own TID. An
orphaned grandchild is adopted and reaped by namespace PID1.

The init-death case holds PID2 alive while its namespace init exits.
The outer parent checks that the worker has disappeared before wait for
init completes. A later fork into that same namespace must fail ENOMEM.
The inherited outer proc mount supplies the worker's outer PID solely for
this observer check. This also illustrates why inner proc mounts must
retain their own namespace view.

PID1 installs an alarm handler that exits explicitly; default fatal signals
such as SIGTERM cannot provide a timeout for namespace init. PDEATHSIG
SIGKILL protects cleanup if the outer test process disappears. Temporary
mounts live only in the new mount namespace and are unmounted before exit.

Next, implement the native-backed PID maps and lifecycle integration
specified in pid-namespace-plan.md. Preserve the current explicit rejection
until PID1, signal/wait/proc and orphan/reap paths agree. NEWIPC and NEWNET
remain separate blockers for Codex's full bubblewrap request.
