# SmartOS lab results transfer

Small source checkpoint for a disposable SmartOS ZFS research lab. This directory
contains the qualified one-file SSH transfer helper and offline tests. It does
not include or launch a VM, create a pool, run the capture, or replace the lab's
lifecycle and free-space controls. No keys, private host paths, machine identities,
raw logs, captures, VM images, or compiled binaries are included.

## Recorded results

The lab topology was four top-level three-disk RAIDZ1 vdevs (`ashift=12`). The
healthy and degraded captures used the same on-disk cut (TXG 1043). The degraded
run omitted child 0 of top-level vdev 0; its eleven remaining data disks were
attached read-only. This was a logical-correctness experiment, not a performance
benchmark or production storage test.

- Healthy run: capture and strict analysis completed. 1,013/1,013 ordinary block
  pointer reads verified, totaling 117,931,008 physical bytes, with none outstanding.
  Observed address sets matched allocation intervals (178,839,552 bytes).
- Degraded run: the recovered capture passed strict same-cut analysis and 22
  conservative reconstruction witnesses totaling 2,883,584 physical bytes.
  These witnesses infer reconstruction from an absent data child, a single DVA,
  full-row mapping, and checksum-valid original-block-pointer reads; they do not
  directly instrument the parity path.
- Capture: 38,410,900 bytes; SHA256
  `3d271356b85b5fcf5b41e036029df1395de8e48fd613344e5b181a3f36f5160a`.
  Both runs produced this digest; degraded bytes were actually transferred and
  validated, not replaced with healthy output.
- SSH transport qualification: a separate system-only VM transferred the recovered
  capture in **3.23848 seconds** (11.86 MB/s decimal), with exact size/SHA256 and
  SSH exit 0. This includes the client's transfer and verification path, excludes
  VM boot/setup, and is one local measurement rather than a throughput guarantee.
  An arbitrary `id` request was rejected with exit 1 and no output.
- Qualification cleanup: dedicated daemon stopped, temporary private credentials
  removed, VM exited 0, loopback listener disappeared. The existing 55 GiB
  free-space guard stayed healthy; 183 prior recovery artifact pins were unchanged.

### Limits of the evidence

The degraded result is `RECOVERED_WITH_QMP_POSTCHECK_GAP`: fresh QMP graph/device
properties and post-capture counters were unavailable. No zero-write-attempt
claim is made. Serial postchecks, read-only file-descriptor/OFD checks, offline
image hashes, and 72 protected-history records passed, but do not replace QMP
counter evidence. Coverage completeness, payload validation, and evacuation
authorization remain **false**.

An earlier serial command lost its trailing frame at a deadline. A bounded
receive-only drain returned no tail. A harmless nonce barrier and a fresh transfer
of its already completed staging file recovered the chunk without repeating the
source read or capture. All 560 complete original DATA rows and the partial row
matched; the whole capture hash subsequently matched. The original command's exit
status remains unknown, and original failed/UNKNOWN histories were preserved.

## Reproduce the transport test

Use a disposable system-only VM when testing transport alone. Preserve existing
capture evidence and enforce the lab's storage headroom guard. Provide a result
file through trusted console/media; qualification used a read-only ISO carrying
the recovered capture. Do not attach production disks or enable host bridging.

1. Add exactly this restricted user-network configuration to the next VM launch,
   after checking that host port 22222 is free:

   ```text
   -netdev user,id=labssh,restrict=on,hostfwd=tcp:127.0.0.1:22222-10.0.2.15:2222
   -device e1000,netdev=labssh,id=labsshdev
   ```

   Through the trusted guest console, inspect `dladm show-link` and
   `ipadm show-addr`. Qualification obtained `10.0.2.15/24` automatically. If
   absent, configure only the discovered lab NIC temporarily; do not overwrite
   existing interfaces or add external routing. Verify the host listener with
   `ss -ltnp 'sport = :22222'` and guest listener with `netstat -an -f inet`.

2. Create a private host session directory and dedicated client key. Set `SESSION`
   to that directory, and bind the actual capture's `SOURCE`, `SIZE`, and `SHA256`
   from trusted console observations. The helper accepts only
   `/var/tmp/prelaunch-<32 lowercase hex digits>/verified.jsonl`.

   ```sh
   umask 077
   ssh-keygen -q -t ed25519 -N '' -f "$SESSION/client"
   python3 transfer.py bundle --output "$SESSION/bundle" \
     --client-public-key "$SESSION/client.pub" \
     --source "$SOURCE" --size "$SIZE" --sha256 "$SHA256"
   ```

3. Through trusted console/media, install the bundle's `sshd_config`,
   `authorized_keys`, and `read-result.sh`, root-owned mode 0600, into a new
   mode-0700 `/root/codex-result-ssh`. Refuse an existing directory. Keep `/root`
   root-owned and not group/world writable. Generate a dedicated guest host key:

   ```sh
   ssh-keygen -q -t ed25519 -N '' -f /root/codex-result-ssh/host_ed25519
   /usr/lib/ssh/sshd -t -f /root/codex-result-ssh/sshd_config
   /usr/lib/ssh/sshd -T -f /root/codex-result-ssh/sshd_config
   ```

   Qualification used SmartOS OpenSSH 10.5p1. SmartOS requires PAM to remain on:
   `UsePAM no` is ignored with a warning. This helper omits that setting and
   explicitly requires `AuthenticationMethods publickey`, disables password and
   keyboard-interactive login, PTY, user RC, and forwarding, and forces the key
   to the single reader. Do not silently remove these restrictions for another
   SSH implementation. The system SSH service is not modified.

4. Obtain **only** `host_ed25519.pub` through the trusted console and save it as
   `$SESSION/host-public-key`; never use unverified keyscan as the trust source.

   ```sh
   python3 transfer.py pin-host \
     --trusted-console-public-key "$SESSION/host-public-key" \
     --output "$SESSION/known_hosts"
   ```

   Explicitly start the separate guest daemon after validating the configuration:

   ```sh
   /usr/lib/ssh/sshd -f /root/codex-result-ssh/sshd_config \
     -E /root/codex-result-ssh/sshd.log
   ```

5. Measure the host pull using a monotonic clock around `transfer.receive`, or
   use the CLI and an external timer:

   ```sh
   python3 transfer.py pull --key "$SESSION/client" \
     --known-hosts "$SESSION/known_hosts" --output "$SESSION/downloaded.jsonl" \
     --size "$SIZE" --sha256 "$SHA256" --timeout 180
   ```

   The reader accepts only `read-result`, checks size/hash before and after
   streaming, and does not grant a shell or SCP/SFTP access. The client pins the
   host key, bounds bytes/deadline, requires exit 0 and exact size/hash, and never
   overwrites or automatically retries. Failure preserves `.partial` evidence.
   Local host users can reach the listener but need the dedicated key.

6. Verify the dedicated daemon's PID and command before stopping it. Remove its
   temporary private key/authorization and the host client private key. Preserve
   public pins and measurements. Cleanly shut down the disposable VM and verify
   the host listener is gone. Never stop the system SSH daemon.

## Offline tests

From this directory, with Python 3 and Bash on Linux:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest -v test_transfer
PYTHONDONTWRITEBYTECODE=1 python3 -O -m unittest -v test_transfer
```

Ten tests cover configuration restrictions, the forced reader, public-key parsing,
exclusive host-key pinning, client options, exact-size/hash/exit validation,
overwrite refusal, bounded failure, timeout, and symlink rejection. Tests use
synthetic public-key parser fixtures and local subprocesses, not live keys or VMs.
