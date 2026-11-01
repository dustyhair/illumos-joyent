# Experimental public PID namespace acceptance

VM151 accepted the experimental LX public PID namespace entry on 2026-11-01
UTC. The tested source ends at
`c1eb26f66f86f686a701024d535777f946964eb3`.

The public entry is guarded by the separate, default-zero
`lx_pidns_experimental` switch. It supports `unshare()` and `clone()` PID
namespace creation together with user and mount namespaces, translates
process, thread, parent, group, session, wait, signal, and queued-signal
identities, applies PID 1 signal semantics, and binds private proc mounts to
their mount-time PID namespace. Ordinary private unmount now destroys a
visible leaf mount while retaining busy-child and failed-unmount safety.

Clean release and DEBUG builds passed for `lx_brand` and
`lx_namespace_ktest`. VM151 ran these release hashes:

- Unix: `44d14d14367016d6f243344e55ca745d295af9134ae33145f0935c8738c31db8`
- genunix: `bb42efda16a9b50edb74ec64a86629c083d564a692ee7a3afe11b1d12f6c6647`
- `lx_brand`: `6ed1b4ba50d738d3a920a2f97cf7d8fd50932bfa660b23fd2683ceedafbab2cd`
- `lx_proc`: `2de0c08c6dfd36176b106500988dda71f15214b6e2d6bf35f5da8c1e25574890`
- `lx_namespace_ktest`: `6c06ed07f64a10d0edfcfd7e0336989865bf44fd912bdb43c4bbdf7422d46114`
- PID lifecycle fixture: `f12bdc01ea39cf5396849b83b7044a4ae6283f06fae9614594ca76650f2aad29`

The production bootstrap passed before enabling the lab-only switch. The
public matrix then passed root and UID 65534 callers in both live-init and
init-death modes. It covered namespace PID 1, child and thread IDs, parent
IDs, sessions and process groups, PID 1 default-signal suppression and signal
handlers, `kill`, `tkill`, `tgkill`, queued signals, non-consuming and
consuming waits, orphan adoption, proc self and task visibility, outer-PID
hiding, and ordinary proc unmount. The controller ended with
`LX_PIDNS_PUBLIC_PASS`; coherent PID and mount idle kernel tests also passed.

The final guest state had all experimental switches at zero, mount views
zero, PID spaces zero, the LX zone installed, a healthy pool, completed sync,
and an exited QEMU process. The raw VM151 console SHA-256 is
`6d670f942c78eba16be423c44275127fbb88da2a132bd3ae8cd95af55fbb987b`.

This accepts the implementation only behind its default-disabled
experimental switch. It does not enable public PID namespaces in production,
and it does not expand the accepted scope to IPC or network namespaces.
