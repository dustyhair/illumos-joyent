# Codex lx shared-network deployment

This checkpoint runs Codex 0.166.0 with bubblewrap filesystem, user, PID,
mount, IPC and seccomp isolation. Network namespace creation is deliberately
disabled: sandboxed commands share the lx zone's network stack.

## Kernel payload

Build and deploy the matched release `lx_brand`, `lx_proc`, `lofs` and `tmpfs`
modules from branch `lx/codex-sandbox`. Reboot into the matched image before
installing the userland package. The experimental entry switches default to
zero; the selected deployment profile must set these module variables to one:

```
lx_brand`lx_seccomp_experimental
lx_brand`lx_userns_experimental
lx_brand`lx_mntns_experimental
lx_brand`lx_pidns_experimental
lx_brand`lx_ipcns_experimental
```

Do not enable a network-namespace switch for this checkpoint.

Build both release and debug variants before deployment:

```sh
cd usr/src/uts/intel/lx_brand && dmake clobber all
cd ../lx_proc && dmake clobber all
cd ../lofs && dmake clobber all
cd ../tmpfs && dmake clobber all
```

## Userland package

Artifact:

```
codex-lx-0.166.0-shared-network.tar.zst
SHA-256 533e35b32d03969cdd9c6320bfbd6e097a1d975bc657dafcf3e63b49628e9852
```

Extract the archive into the lx zone. It contains one top-level
`lab-codex-lx` directory. The integrity manifest records the original and
patched Codex and bubblewrap hashes. Start Codex through
`lab-codex-lx/codex-lx`; do not invoke `vendor/bin/codex` directly.

Run the launcher from the project directory that should be writable. The
current directory is the `:workspace` policy root, so launching from `/`
would intentionally make the entire zone root writable to the sandbox.

The launcher sets `sandbox_workspace_write.network_access=true` and enables
the integrity-checked wrapper. The wrapper removes exactly one option-position
`--unshare-net` and rejects missing or duplicate occurrences. It has no mode
that shares the zone's complete `/dev`; the sandbox gets a private device tree
with a zone-scoped devpts graft and a narrow `ptmx` alias.

To reproduce the userland package, compile the wrapper without a C runtime and
then run the package builder against the unmodified Codex 0.166.0 vendor tree:

```sh
gcc -nostdlib -static -fno-pie -no-pie \
    -o codex-lx-bwrap codex-lx-bwrap.c
./build-codex-lx-package.py \
    --vendor /path/to/x86_64-unknown-linux-musl \
    --wrapper ./codex-lx-bwrap \
    --launcher ./codex-lx \
    --output ./lab-codex-lx
tar --zstd -cf codex-lx-0.166.0-shared-network.tar.zst lab-codex-lx
sha256sum codex-lx-0.166.0-shared-network.tar.zst
```

The acceptance helpers are intentionally static so the installed zone needs
no compiler or runtime libraries:

```sh
for probe in pty-probe sandbox-write-probe sandbox-readonly-probe; do
    gcc -nostdlib -static -Os -fno-stack-protector -no-pie \
        -Wl,--build-id=none -o "$probe" "$probe.c"
done
```

Copy the package and probes under `/test` in a disposable lx zone, then run
`codex-lx-acceptance.sh` from the global zone. Its `CODEX_LX_TEST_ZONE`,
`CODEX_LX_ZONE_ROOT`, `CODEX_LX_TEST_PACKAGE`, and `CODEX_LX_TEST_WORKDIR`
variables override the defaults. The driver enables the experimental switches
only for the test and resets all five on exit.

## Accepted result

Fresh offline VM168 passed the production baseline, restricted IPC regression,
package hash verification and complete Codex/bubblewrap workflow. The clone
flags were `0x38020011` (no `CLONE_NEWNET`). Identity isolation, workspace
writes, LOFS and tmpfs read-only enforcement, PTY master/slave operation and
the absence of `/dev/pts/ptmx` outside the sandbox all passed. Every
experimental switch was reset to zero after the run.

Network isolation remains a separate follow-up milestone.
