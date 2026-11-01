# Native `/proc` agent acceptance

VM140 accepted the LX native `/proc` agent syscall-mode correction and the
existing synthetic PID ownership correction on 2026-11-01 UTC. The tested
production changes are `5f86540a7d4fc92f1ddf34d6a634b3ec3035f4af` and
`fa95ffb81a0da2ff06e197fbb8b740955b1473a1`.

`Psyscall()` injects illumos syscall numbers at the target's native syscall
instruction. A kernel-created `/proc` agent begins in `LX_STACK_MODE_PREINIT`,
which selects native dispatch. The process restriction used to terminate that
entry before the mode check. The correction permits native dispatch only when
the running thread equals the kernel-maintained `p_agenttp`. Ordinary Linux
LWPs cannot select this state. Existing `/proc` authorization and the native
agent creation and destruction lifecycle remain unchanged.

The private controller exercised native `getpid`, `_lwp_self`, and `_lwp_exit`
through real libproc `Psyscall()` operations. Both an unattached LX target and
a target with a private PID map survived `Pdestroy_agent()`. The mapped case
proved that the prepared identity differed from the leader, stayed in the
native numeric hash, became the agent's durable `br_pid`, and matched the
actual namespace map. After agent exit, one inactive native observer remained.
Its final native release removed the numeric hash entry.

The first two fresh attempts, VM138 and VM139, found private-test assumptions
that namespace PID 2 would be reused after the agent reservation. The
allocator uses next-fit numbering. The corrected private checker recorded the
actual committed namespace number and used it for retirement and release.
Neither failure indicated a production defect, and both diagnostic images and
logs were retained without an in-VM retry.

VM140 also passed the established PID cohort prerequisite, 15 fork checks,
six exec checks, five thread checks, native 32-bit and 64-bit ordinary-LWP
controls, 512 ordinary LX LWP creations, leaderless process-group and queued
signal controls, and the orphan and nested PID-map suites. The final state had
all three experimental flags at zero, mount views zero, PID spaces zero, the
LX zone installed, a healthy pool, export status zero, completed sync, and an
exited QEMU process.

Runtime release hashes were:

- `lx_brand`: `06f27ae72d58ed5bb03a300a03db1d854bc357147deaac10e9b16129079c9226`
- private `lx_namespace_ktest`: `80b528df0935e0a434ea9c08896bda1191c82b289e51563273bc56289d659a11`
- controller: `a1a7f3c08fe9ea82a42bc825561e0540ab7a53bad9e2dc4ac20add471fd62592`
- controller script: `5e44093befec1c31d6d4eb1163934bdb650d1a0c2126c7250b96831d2870fdc0`
- boot archive: `dfcce8b9b4e5b1306b9fd5ff7915d925b169258d163a313f006d36a5bf3b9992`
- ISO: `42adb658051e430858adb509163d8eb8a17f63dc3dc80cd2e2348368eea5f7b6`
- console evidence: `08254487719138a71b392de6ed09b0467e34748cb99613de94c841e50004241c`
- accepted disk image: `fb05e6d8489f8e12cb57ac9712e5789f8c3cc464df6019ccdc2bb75bebe539de`

After removing the private test overlay, clean release and DEBUG builds passed.
The production hashes were `20ab5c990c632ff853b2d75bff080aa12170ebc9409dee534f9a4b6d871ec32f`
and `72dcb0e75339e6ea7697ab0e99d7fc2a188c7a9a6ae02ffa1b1735469175e18b`
for `lx_brand`, and `ac1c19a028b07426a58608cac583774e6ce0f5bebb6cb2055bab2412e657045b`
and `a8f35050612ebcc42dadd0e58db72ae81896b4413584e97919ee0a10decfb3a4`
for `lx_namespace_ktest`.

This accepts only native agent execution and PID ownership. Public PID
namespaces, PGID and SID roles, general map lookup and removal, PID1 policy,
IPC, networking, and the complete Codex bubblewrap workflow remain out of
scope and unaccepted.
