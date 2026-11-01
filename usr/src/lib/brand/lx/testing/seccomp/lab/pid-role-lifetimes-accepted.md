# PID role-map lifetime acceptance

VM146 accepted the LX PID role-map lifetime and final-release changes on
2026-11-01 UTC. The tested source ends at
`098c06b4dd1f59b35ac4adeea47cc09ab1f7a627`.

The production changes serialize final PID-map removal under `pidlock`, use a
constant-stack retry for a raced final release, retain committed maps for
process-group and session membership, and add the version-3 brand callback
used to update those roles. The tests cover leader reap, surviving group and
session membership, group departure, zombie retention, final role release,
and a last-release versus indexed-acquisition race. A dedicated owner-release
test also verifies mapped native `/proc` agent teardown without assuming PID
allocator reuse.

Clean release and DEBUG builds passed for the affected Unix, genunix,
`lx_brand`, and `lx_namespace_ktest` modules. VM146 ran these release hashes:

- Unix: `44d14d14367016d6f243344e55ca745d295af9134ae33145f0935c8738c31db8`
- genunix: `bb42efda16a9b50edb74ec64a86629c083d564a692ee7a3afe11b1d12f6c6647`
- `lx_brand`: `8c15d5c06928e0ff2a1ded1b0ac53ca5d8fe886ee867e5970b996c7e916e2390`
- `lx_namespace_ktest`: `5b2328784c8e1495b6469e4c63271e3007f185d9e3785c5f6ac670ac0fb3548d`
- LX root fixture: `dd8d11caafc01892cc70b099acc6dc1e382a69d750249c986f9872c863f917dd`

The production LWP checkpoint passed native 32-bit and 64-bit controls,
ordinary static and dynamic LX LWPs, process and role ownership, cohort
signals, leaderless process groups, 32-bit siginfo, orphan adoption, and
nested PID drain. The mapped native-agent checkpoint passed real libproc
native syscalls, attached-map target survival, owner reap, final PID identity
removal, and namespace, mount-view, and user-budget idle checks. PID1 drain,
queued-signal coverage, and the complete PID, mount, and namespace regression
then passed with `DRAIN=0 QUEUED=0 FULL=0` and
`LX_PID_PROCESS_FULL_PASS`.

The final guest state had all three experimental flags at zero, mount views
zero, PID spaces zero, the LX zone installed, a healthy pool, export status
zero, completed sync, and an exited QEMU process. The raw VM146 console SHA-256
is `d0cf8ada7c71855e6e3e2fde7d7ba93a3aa72f3393bcea4be6b5b409d7170fa1`.

This accepts the private test path and the production role-map and agent
lifetime corrections. Public `CLONE_NEWPID` remains disabled. IPC and network
namespaces and the complete current Codex bubblewrap workflow remain outside
this checkpoint.
