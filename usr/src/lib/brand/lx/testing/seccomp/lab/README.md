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
