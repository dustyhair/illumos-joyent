# TU102 BAR Bring-Up Notes

## Purpose

This note tracks the current TU102 ROM/GOP BAR bring-up work in `bhyve`
without mixing it into the older MSI/runtime notes.

## Current branch state

- Workspace: `/build/smartos-live/projects/illumos`
- Active file: `usr/src/cmd/bhyve/common/pci_passthru.c`
- Test host: `root@192.168.1.201`
- Guest test path: stock UEFI or GPU-diag UEFI, flat topology, TU102 ROM

## Preserved older working shape

From trace bundle:

- `/build/traces/tu102-state-20260411T031538Z-pcie-shadow/launcher.log`
- `/build/traces/tu102-state-20260411T031538Z-pcie-shadow/com1.log`

Observed behavior on that older local path:

- Guest BAR1 was presented at `0x800000000` with size `0x10000000`.
- Guest BAR3 was presented at `0x810000000` with size `0x02000000`.
- A TU102 BAR3 tail alias was mapped at `0x812000000`, size `0x1000`.
- ROM load and start completed successfully on COM1.
- Linux later reported `BAR1 @ 0x800000000 (256MB)`.

This is the strongest preserved local evidence that the old working path did
not depend on the later synthetic `0x160...` BAR1 scheme.

## Current recovery findings

### Before BAR3-tail recovery

The recovery path initially fell into synthetic guest apertures:

- `0x160000000`
- `0x172000000`
- `0x182000000`
- `0x192000000`

Those accesses were guest physical MMIO addresses, not host BAR addresses.

### BAR3-tail recovery

Current recovery work restored a BAR3-tail decode in `pci_passthru.c` and
split BAR3-tail MMIO handling away from the generic BAR3 alias-remap path.

Current observed run:

- Trace: `/zones/build/traces/testvm-trace-BAR3TAIL2-20260413T045207Z`
- Test binary: `/zones/build/bhyve-tu102-bar3tail2`
- sha256: `e5061692ac5177a0971c756dd5444a4276bedf150d4086bd1f79cdba17a6b08d`

Observed behavior change:

- The BAR3 tail is now serviced cleanly at `0x172000000`.
- The guest issues byte writes of `0xff` into the BAR3 tail page.
- The first post-tail BAR1 access now occurs at `0x182001000`.
- That first BAR1 post-tail access is also serviced without an unhandled MMIO
  fault.
- There is still no guest SSH and no useful COM1 output on this path.

This is later progress than the previous recovery runs, which died earlier on
the first post-BAR3 transition.

### Trap-only BAR1 observation

Current higher-visibility run:

- Trace: `/zones/build/traces/testvm-trace-BAR3TAIL3-20260413T050417Z`
- Test binary: `/zones/build/bhyve-tu102-bar3tail3`
- sha256: `c9965ef9e0943af6dce717f9a49f5ca3accf05aca540f3fd68065d85764a17fa`

Observed behavior change:

- The first post-tail BAR1 window was kept trap-only at `0x172001000`.
- The guest then wrote `0xff` byte-by-byte across that BAR1 window.
- Logged accesses advanced contiguously from offset `0x0` through at least
  offset `0x3ff` in the first page.
- There were no unhandled MMIO faults during that observation window.
- COM1 remained silent and the guest was still not reachable.

This is the first direct proof that the post-tail BAR1 region is not just
"touched once"; the guest is walking the first page linearly.

### Repeated first-page fill and promote

Current higher-progress run:

- Trace: `/zones/build/traces/testvm-trace-BAR3TAIL5-20260413T051207Z`
- Test binary: `/zones/build/bhyve-tu102-bar3tail5`
- sha256: `63c315a0eef07288c0cc01e47b369557b0bd94985bf1eaa47fd2b215b990460e`

Observed behavior change:

- The trap-only first-page logic now repeats on successive post-tail BAR1
  windows, not just the first one.
- Confirmed fill-and-promote sequence so far:
  - `0x172001000`
  - `0x182001000`
  - `0x192001000`
  - `0x1a2001000`
  - later slot reuse also reached `0x1b2001000`
- For each of those windows, the guest writes `0xff` byte-by-byte across the
  first 4 KB page and only then promotes the full 256 MB BAR1 slice.
- Early COM1 output is still limited to DXE handoff markers:
  - `DxeIpl: peim-init`
  - `DxeIpl: register-shadow`
  - `DxeIpl: enter-loadcore`
  - `DxeIpl: found-dxecore`
  - `DxeIpl: loadfile-ok`
  - `DxeIpl: handoff-dxecore`
- The guest did not become stably reachable yet, but this is the clearest
  evidence so far that the post-tail BAR1 path is a repeated page-prime +
  promote pattern rather than a single special-case transition.

### Deeper live-window cache

Current follow-up run:

- Trace: `/zones/build/traces/testvm-trace-BAR3TAIL6-20260413T051723Z`
- Test binary: `/zones/build/bhyve-tu102-bar3tail6`
- sha256: `2bb02463d7b6cd9faf70e39d885a0e8b10873d191282d23adbe4fc0b0fa55663`

Observed early behavior:

- The live post-tail BAR1 cache was widened from 4 slots to 8 slots.
- The run still reaches the first post-tail BAR1 promotion at `0x172001000`.
- Host-side ICMP reachability to `192.168.1.222` became visible early on this
  run, but guest SSH was not yet stably accepting connections when checked.

This is still preliminary, but it suggests the wider cache did not regress the
new BAR1 bring-up path and may be helping later guest progress.

### Cache-depth limit and retry follow-up

Later runs clarified the next two boundaries:

- `BAR3TAIL6` (`/zones/build/traces/testvm-trace-BAR3TAIL6-20260413T051723Z`)
  showed that simply widening the live BAR1 cache is not sufficient. The run
  eventually reached `0x1b2001000` and then repeated:
  - `pci_passthru: TU102 postbar3-window-promote map failed bar=1 gpa=0x1b2001000 ...`
- That indicates a real promotion-budget limit for fully mapped BAR1 slices.

To address that, `BAR3TAIL7` added a promotion retry path that:

- tries the full-window BAR1 map once,
- if that fails, evicts the oldest already-promoted BAR1 slice,
- then retries the promotion once.

Current state of that retry path:

- test binary: `/zones/build/bhyve-tu102-bar3tail7`
- trace: `/zones/build/traces/testvm-trace-BAR3TAIL7-20260413T113145Z`
- the run still progresses through at least the first and second post-tail BAR1
  fill-and-promote windows,
- but the captured trace has not yet shown a successful `promote retry` line.

So the current active boundary is no longer "how do we map the first BAR1
window?" but "what is the correct policy once repeated promoted BAR1 windows
start exhausting the mapping budget?"

### Page-only BAR1 recovery and stock Linux bring-up

Two later recovery changes materially changed the stock-UEFI path:

1. Post-BAR3 BAR1 slices stopped promoting the full 256 MB window
   immediately. Instead, each new slice starts trap-only and only the first
   4 KB page is mapped after the observed `0xff` fill completes.
2. Passthru BAR registration in `pci_emul.c` was reordered so `pe_baraddr()`
   runs before userspace MMIO registration. That fixed the stock-UEFI
   `unregister_mem()` assertion where BAR3 had been registered at the wrong
   pre-pin GPA.

Key traces from that transition:

- `/zones/build/traces/testvm-trace-BAR1PAGE-20260413T115328Z`
- `/zones/build/traces/testvm-trace-BAR1PAGE-STOCKFIX-20260413T120018Z`

Observed behavior change:

- Stock UEFI now gets through firmware, GRUB, and Linux init instead of
  crashing in bhyve or stalling at the earlier BAR1 promotion limit.
- COM1 showed Linux boot through `simpledrm`, storage bring-up, and later
  userspace/systemd progress.
- The next recovered issue was not a boot failure but a BAR decode mismatch:
  native BAR1 accesses at `0x800000000` were still being folded into the
  synthetic post-BAR3 window path.

### Restored high-GPA BAR layout and stable stock checkpoint

The preserved older local tree pinned the display function differently than
the mid-recovery code:

- BAR1 native GPA: `0x800000000`
- BAR3 native GPA: `0x810000000`
- BAR0 native GPA: `0xc0000000`

Restoring that older native layout while keeping the low compatibility alias
and BAR3-tail handling produced the first stable stock-UEFI checkpoint on the
current branch.

Current stable run:

- Trace: `/zones/build/traces/testvm-trace-HIGHPIN-STOCK-20260413T121149Z`
- Test binary: `/zones/build/bhyve-tu102-highpin`
- sha256: `fbd8262eb89a5c45bd3f88e9ce818c10d91cf27400f370bd9b6fc97fe3bdc782`

Observed behavior change:

- BAR1 stays natively mapped at `0x800000000`.
- BAR3 stays natively mapped at `0x810000000`.
- The earlier bad native-BAR1 misdecode no longer appears in the launcher
  trace on this run.
- Stock UEFI boots through:
  - `BdsDxe` handoff
  - GRUB menu
  - Linux init
  - systemd userspace
  - serial login prompt on `ttyS0`
- Guest networking comes up:
  - ICMP to `192.168.1.222` succeeds
  - TCP port 22 opens
- Guest NVIDIA runtime succeeds again:
  - `nvidia-smi -L` returns the TU102 device
  - full `nvidia-smi` completes successfully
  - `/proc/interrupts` shows the GPU on MSI IRQ `37`
- Guest `dmesg` reaches:
  - `GPU 0000:00:08.0 is primary UEFI console device`

This is the strongest current stock-UEFI checkpoint in the recovery branch:
the guest is stable enough to boot, accept SSH, and complete `nvidia-smi`
with the restored high-GPA BAR layout.

### Guest modeset follow-up on the stable stock checkpoint

After the stable stock checkpoint was committed, the guest was retested with
the boot-time NVIDIA blacklist removed and `nvidia-drm.modeset=1` added to
the kernel command line.

Current follow-up run:

- Trace: `/zones/build/traces/testvm-trace-MODESETTEST-20260413T131358Z`
- Host binary: `/zones/build/bhyve-tu102-highpin`
- Guest kernel cmdline now includes:
  - `nvidia-drm.modeset=1`
  - no `modprobe.blacklist=` or `rd.driver.blacklist=` NVIDIA entries

Observed behavior change:

- The guest still boots successfully to late userspace.
- COM1 now reaches:
  - `Started gdm.service - GNOME Display Manager.`
  - `Ubuntu 24.04.3 LTS jon-BHYVE ttyS0`
  - later `NVRM: probing 0x10de 0x1e07, class 0x30000`
- Guest SSH still works after the modeset boot.
- `nvidia-smi` and `nvidia-smi -L` still succeed.
- `/proc/interrupts` still shows the GPU on MSI IRQ `37`.

However, the guest still does not load `nvidia_drm`, and that turned out to
be a guest-local policy issue rather than a SmartOS/bhyve issue. The guest
contains multiple local modprobe blacklist files:

- `/etc/modprobe.d/zz-nvidia-block-display.conf`
- `/etc/modprobe.d/nvidia-early-trace-blacklist.conf`
- `/etc/modprobe.d/zz-gpu-test-blacklist.conf`
- `/etc/modprobe.d/zz-nvidia-passthru-blacklist.conf`

Those files blacklist `nvidia_drm` / `nvidia_modeset`, and one of them also
contains:

- `install nvidia_drm /bin/false`
- `install nvidia_modeset /bin/false`

That means the current remaining display-handoff blocker is no longer the
host-side ROM/BAR path. The next step is guest-local cleanup so
`nvidia_drm` can actually load and honor the already-active
`nvidia-drm.modeset=1` kernel argument.

### Clean guest DRM autoload after blacklist removal

After the guest-local blacklist files were moved aside and the guest
`initramfs` was rebuilt, the next stock-UEFI boot successfully brought up
the DRM stack without any live `modprobe` workaround.

Current follow-up run:

- Trace: `/zones/build/traces/testvm-trace-MODESETREBOOT-20260413T134107Z`
- Host binary: `/zones/build/bhyve-tu102-highpin`

Observed result from guest `192.168.1.222`:

- Kernel command line still includes:
  - `nvidia-drm.modeset=1`
- Guest boots back to the normal serial login path.
- `gdm`, `graphical.target`, and `ssh` are all active.
- `nvidia-smi -L` still succeeds.
- `nvidia_drm` now autoloads during boot:
  - `nvidia_drm`
  - `nvidia_modeset`
  - `nvidia`
- Guest DRM nodes now exist for the passed GPU without any manual load:
  - `/dev/dri/card1`
  - `/dev/dri/renderD128`
  - `/dev/dri/by-path/pci-0000:00:08.0-card`
- `dmesg` now shows the full early display handoff sequence:
  - `fbcon: Taking over console`
  - `Initialized simpledrm`
  - `NVRM: probing 0x10de 0x1e07`
  - `[nvidia-drm] Loading driver`
  - `GPU 0000:00:08.0 is primary UEFI console device`
  - `Initialized nvidia-drm ... for 0000:00:08.0 on minor 1`

This is the first preserved checkpoint where the current SmartOS/bhyve
baseline plus guest-local cleanup produces:

- stock UEFI boot,
- working `nvidia-smi`,
- active `gdm`,
- and automatic `nvidia_drm` startup on the passed GPU.

## Timing notes

The current traces do not support the idea that UEFI itself is the major
startup delay anymore.

From `/zones/build/traces/testvm-trace-MODESETREBOOT-20260413T134107Z`:

- launcher starts `bhyve` at `2026-04-13T13:41:11Z`
- COM1 capture attaches at `2026-04-13T13:41:13Z`
- `BdsDxe: starting Boot0003 "Ubuntu"` is already present in that first COM1
  burst
- the first Linux kernel line appears immediately after the `BdsDxe` lines in
  the same initial COM1 burst

So the visible firmware handoff from UEFI boot manager into the Linux kernel
is already down in the low-seconds range, and the expensive part of startup
is later:

- `NVRM: probing 0x10de 0x1e07` appears at about `+54s` in guest kernel time
- `Initialized nvidia-drm ...` appears at about `+68s`
- serial login appears after the later systemd/network/GDM work

That means the current startup improvement target is not “make BDS itself much
faster”; it is reducing or replacing the temporary TU102 compatibility path
without regressing the already-fast firmware handoff.

### Quiet-trace confirmation on the stable stock checkpoint

After the TU102 startup trace points were moved behind the
`BHYVE_PPT_TU102_TRACE=1` runtime gate, the stock-UEFI path was rerun to
confirm that the quieter default still behaved like the earlier stable
checkpoint.

Current follow-up run:

- Trace: `/zones/build/traces/testvm-trace-QUIETTRACE-20260413T135929Z`
- Host binary: `/zones/build/bhyve-tu102-quiettrace`
- `bhyve` sha256:
  - `e642fa9902818e7e146dfaa7e3ec004b463154436c5104437166a5570fab60af`

Observed behavior:

- The host-side TU102 BAR churn logs are quiet by default.
- COM1 still shows the same fast firmware handoff:
  - `BdsDxe: loading Boot0003 "Ubuntu"...`
  - `BdsDxe: starting Boot0003 "Ubuntu"...`
  - immediate Linux boot output afterward
- The guest again reaches late userspace, with the visible delay now in:
  - `NetworkManager.service`
  - `accounts-daemon.service`
  - `snapd.service` / `snapd.seeded.service`
- Representative COM1 timing from this run:
  - `ucsi_ccg ... PPM init failed` around `73s`
  - `NetworkManager.service/start running` near `57s+`
  - `accounts-daemon.service/start running` near `61s+`
  - `snapd.service/start running` near `65s+`

So the quieter default host build preserves the current stock-UEFI path, and
the next startup optimization target is guest userspace policy and service
behavior rather than firmware or early bhyve bring-up.

### Guest memory sizing on the stable stock checkpoint

The current stable stock-UEFI checkpoint was re-tested with larger guest
memory sizes.

Observed results:

- `1G` remains bootable, but it is no longer the preferred default.
- `2G` boots successfully on the same stock-UEFI flat-topology baseline and
  reaches the normal late-userspace / SSH / graphical-target path.
- `4G` currently fails before firmware with:
  - `pci_emul_alloc_rom: failed to create rom segment`
  - `passthru_init_rom: failed to alloc rom segment`
  - `Device emulation initialization error: Not enough space`

The practical conclusion is:

- use `2G` as the default working size for this recovery path
- do not use `4G` until the bhyve guest address-space layout is adjusted so
  the GPU ROM/MMIO placement still fits

## Source research notes

### Linux / VFIO model

Linux VFIO exposes real PCI BAR regions to userspace and lets userspace map
device regions and IRQs through the VFIO device API. The kernel documentation
does not describe any synthetic guest BAR1 alias chain as a normal model.

Primary source:

- `https://docs.kernel.org/driver-api/vfio.html`

### FreeBSD / bhyve model

FreeBSD changed bhyve in 2024 so that, when booting with a boot ROM, it does
not pre-enable and pre-map PCI BARs before guest code runs. The boot ROM is
expected to own BAR initialization in that mode.

Primary source:

- `https://cgit.FreeBSD.org/src/commit/?id=e962b37bf0ffe7f30f5b025b46ea49ba01c71f2f`

This is relevant because current TU102 ROM bring-up work is still close to
that boundary: BAR exposure timing and command/BAR decode state materially
change guest firmware behavior.

### TU102 BAR meaning

External hardware documentation aligns with the local behavior split:

- BAR0: MMIO registers
- BAR1: VRAM aperture
- BAR3: RAMIN aperture
- BAR6: PCI ROM aperture

Primary source:

- `https://envytools.readthedocs.io/`

## Current interpretation

- `0x172000000` is not "first extra BAR1 window"; it is BAR3-tail related.
- The first post-tail BAR1 access starts at `0x172001000`, immediately after
  the BAR3-tail page.
- Early recovery needed the repeated first-page `0xff` fill observation to get
  through the post-BAR3 transition without immediate MMIO faults.
- The native TU102 layout that currently works best is the older high-GPA
  shape:
  - BAR1 `0x800000000`
  - BAR3 `0x810000000`
  - BAR0 `0xc0000000`
- On the current stable stock run, the bad native-BAR1 misdecode has dropped
  out and the guest can boot fully enough for SSH and `nvidia-smi`.

## Next actions

- Preserve this high-GPA stock-UEFI checkpoint before making further cleanup
  changes.
- Compare the remaining local recovery hacks against the older preserved
  working source and remove whatever is no longer needed on top of the stable
  run.
- Check whether the physical passed-GPU display now shows useful output on the
  same stock run, since the guest now reaches `primary UEFI console device`
  and a stable NVIDIA runtime.
