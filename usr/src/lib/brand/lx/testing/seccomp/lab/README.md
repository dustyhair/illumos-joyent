# Offline lx kernel acceptance lab

The 2026-10-29 x86-64 test passed all 17 `no_new_privs` checks with no skips.
The seccomp probe reported disabled mode and rejected filter installation,
exiting 77 as expected. Seccomp filtering and namespace support remain absent.

The test ran in a disposable local QEMU/KVM VM with two CPUs, 2 GiB RAM,
no network interfaces, and one new 8 GiB qcow2 disk. No host devices or
directories were shared with the guest. The existing SmartOS host kernel,
its zones, and the other project checkout were unchanged.

## Image contents

The base was a copy of the existing May 8 `platform-TESTING` output. Only
the copy was mounted on the builder using its own lofi mapping. It contains
rebuilt release modules at:

- `/kernel/amd64/genunix`
- `/kernel/brand/amd64/lx_brand`
- `/kernel/fs/amd64/lx_proc`

The latter two paths override the shipped `/usr/kernel` copies. Runtime
behavior and module listings confirmed the new modules were active.
The builder's other compiled objects were not copied into these modules.
The surrounding platform and bootloader were reused, so this is explicitly
a mixed-image development result. It does not certify a full platform build.

The common exec and LWP changes live in `genunix`. Proc reporting lives in
`lx_proc`, which must be built and included separately from `lx_brand`.
The first test image omitted `lx_proc` and correctly failed the proc-dependent
checks; rebuilding and including it resolved those failures.

The archive also contains `/lab/lx-root.tar`, `/lab/guest-test.sh`, and an
empty `/lab/offline-lx-guest` marker. Its single-user `sulogin` is replaced
with a direct console shell for this networkless test image only. No password
or SSH setup is needed. This image is intended only for the private lab.

## Minimal lx root

Compile `lab_init.c`, `no_new_privs_probe.c`, `identity_probe.c`, and
`seccomp_probe.c` statically for Debian 12 x86-64, using `-static -O2` and
adding `-pthread` for the no-new-privileges probe. Compile the native helpers
on SmartOS as described in the parent README.

The root needs `etc/init.d`, `etc/default`, `etc/network`, `sbin`, `bin`,
`usr/bin`, `test`, `proc`, `sys`, `dev`, `native`, `var/tmp`, `tmp`, and `root`
directories. Give `tmp` and `var/tmp` mode 1777. Install:

- The Linux `lab_init` binary as `/sbin/init`.
- The three Linux probes and two native helpers under `/test`, with their
  standard names from the parent README.
- `/bin/sh` and `/sbin/sh` as symlinks to `/native/usr/bin/sh`.
- `/usr/bin/getent` as a symlink to `/native/usr/bin/getent`.
- `/etc/passwd` and `/etc/group` with root and UID/GID 65534 entries.
- `/etc/alpine-release` containing `3.20`, plus empty `inittab` and `fstab`.

Pack the tree with root ownership using GNU tar's `--owner=0 --group=0`.
The lx platform supplies `/native`, device filesystems, and proc mounts.
The shell links are required for safe-mode zlogin. The driver creates a
dedicated ZFS zone dataset and halts the zone after the checks.

## Boot and run

Use a BIOS ISO containing the normal SmartOS loader, `boot/cdboot`, and
`platform/i86pc/kernel/amd64/unix` plus `platform/i86pc/amd64/boot_archive`.
Generate `boot_archive.hash` as the archive's SHA-1 hex digest and newline.
Copy the matching external `unix` from the base image. The lab's loader config
uses serial console `ttya`, a one-second autoboot delay, and:

```
smartos="true"
boot-args="-s -B smartos=true,standalone=true,noimport=true,lx_lab=true"
```

Create the ISO with `xorriso -as mkisofs -R -b boot/cdboot -no-emul-boot
-boot-info-table`, using the assembled image tree. Place it at
`lab/lx-lab.iso` alongside `lab/zones.qcow2` relative to the checked-in
`launch-lab` and `console.py` scripts. Create the blank disk with
`qemu-img create -f qcow2 lab/zones.qcow2 8G` only for the initial run.

Run `bash launch-lab`. Once single-user boot completes, invoke:

```
python3 console.py '/lab/guest-test.sh'
```

The driver requires both the image marker and `lx_lab=true` boot property
before it can initialize the disposable disk. It selects the sole non-removable
disk from `diskinfo`; the CD-ROM is not a candidate. Expected output includes
`LX_NNP_EXIT=0`, `LX_SECCOMP_EXIT=77`, and `LX_LAB_PASS`.

Direct QEMU multiboot loading failed to mount the RAM filesystem even with
the original `genunix`. Booting through the normal SmartOS loader succeeded.

Save the console log and module hashes, halt any remaining test zone, then
use the private console to run `cd /; zpool export zones; poweroff`. The
launcher removes its PID/socket files on exit. Keep the ISO, qcow2 disk and
logs for subsequent development, but do not leave the VM consuming RAM.

i386, configured profile/forced-privilege paths, tracing, and concurrent
credential transitions remain outside the demonstrated acceptance result.

## Seccomp BPF core acceptance

The BPF milestone adds a stateless core to `lx_brand` and a separate test
module. It does not install filters or enforce syscall restrictions.
The kernel source is commit `d3bb62568aa2f39eaa8db903a93201fc5707a472`;
the final guest driver is `4d75812602`. Clean release/debug builds of
`lx_brand` and `lx_seccomp_ktest` passed. Existing `genunix` and `lx_proc`
from the no-new-privileges milestone were retained in the private archive.

Generate the corpus using the parent README's Linux oracle. The acceptance
corpus contains 67179 records, including 67163 native Linux comparisons and
16 literal ABI cases. Its SHA-256 is
`202bea0c2d04d55ad8ab0a371180e8e11f7a13dd0db7143b5bd28c2b334d8f25`.
Put it at `/lab/linux-vectors.bin`, the test module at
`/lab/lx_seccomp_ktest`, and `bpf-test.sh` under `/lab` in the private archive.

The base image lacks the ktest command and driver. This lab reused the
existing build proto's command, library and driver. Install the command
at `/lab/ktest`, `libktest.so.1` under `/lib/amd64`, the driver at
`/kernel/drv/amd64/ktest`, and its configuration at `/kernel/drv/ktest.conf`.
SmartOS mounts a separate compressed `/usr` during boot, so writing a command
under `/usr/bin` in the RAM root does not make it available after boot.
The guest driver creates the ktest device if necessary.

Prepare three small negative fixtures from one corpus record. Set the
header count to one, then omit the last instruction byte for the truncated
case, append one byte for the trailing case, or use an invalid header magic.
Name them `/lab/bpf-bad-truncated.bin`, `/lab/bpf-bad-trailing.bin` and
`/lab/bpf-bad-header.bin`. Full copies of the corpus are unnecessary and can
exhaust the private archive's free space.

Run `/lab/guest-test.sh && /lab/bpf-test.sh` from the private serial console.
The final clean boot passed the 17 privilege regressions and all 67179 BPF
records. Each negative fixture produced an expected ktest ERROR and exit 1;
the combined driver returned 0 and printed `LX_BPF_PASS`. The unchanged
seccomp probe returned 77 because syscall filtering remains unsupported.
Save the console output before exporting the disposable pool and powering
off. The first BPF boot established the privilege regression result but
failed to find the test command under `/usr`; the acceptance boot used the
corrected image and a fresh disposable disk, with no serial binary upload.

Release module SHA-256 values:

- `lx_brand`: `ff732834ee17169bb65fb021b20ac489e33d421f663a7d1dab24896c896f2429`
- `lx_seccomp_ktest`: `f236b903f542908cc08d7f94db3b1cc334b11f58f4f6d998d12db37f57edcd4c`

This remains a mixed-image development test. No physical host kernel,
boot configuration or existing Debian zone was changed.
