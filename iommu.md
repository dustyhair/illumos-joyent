# IOMMU Project Log (LLM-Parseable)

## Format Rules
- File is append-only unless explicitly doing a normalization pass.
- Use ISO8601 UTC timestamps (`YYYY-MM-DDTHH:MM:SSZ`).
- Keep IDs stable once created.
- Prefer machine-oriented fields over prose.
- Add new work as a new object in `history`.
- Update `state` and `next_actions` after each meaningful change.
- Source-code change policy: do not introduce non-ASCII characters in source files.

## Helper Scripts
```yaml
helper_scripts:
  - path: ./helper-scripts.sh
    purpose: "Reusable shell helpers for iommu.md maintenance and session handoff"
    tags: [log, handoff, triage]
    key_commands:
      - "./helper-scripts.sh help"
      - "./helper-scripts.sh log-quick-status"
      - "./helper-scripts.sh log-next-actions"
      - "./helper-scripts.sh log-append-template"
      - "./helper-scripts.sh log-touch"
```

## Build Stamp Policy
```yaml
buildstamp_policy:
  default_mode: "force TESTING in smartos-live Makefile"
  makefile_path: "/build/smartos-live/Makefile"
  fallback_mode_for_future_sessions:
    - "export BUILDSTAMP=TESTING"
  note: "If Makefile behavior is reverted upstream, use the fallback export before invoking the build."
```

## Schema Version
`iommu-log/v1`

```yaml
meta:
  schema: iommu-log/v1
  project: smartos-gpu-passthrough-illumos
  repo_path: /build/smartos-live/projects/illumos
  owner: jonathan-wagner
  last_updated_utc: 2026-02-20T19:11:02Z
  baseline_commit: 163766864e9e03150779d67df8d72b9242a84eb7

state:
  objective: "Enable reliable GPU passthrough on SmartOS (illumos side)."
  phase: bringup
  confidence: medium
  reviewed_commit_range: 163766864e9e03150779d67df8d72b9242a84eb7..0075f74b7b24fc52ab431d93e03e34a7f47c996d
  reviewed_commit_count: 3
  major_capabilities_present:
    - ppt_caps_and_region_query
    - ppt_iommu_map_unmap_api
    - vmm_vm_memquery_api
    - iommu_domain_map_unmap_exports
    - interrupt_remap_debug_paths
    - reset_fallback_paths_flr_vendor_bus
  blockers_known:
    - agaw_forced_hack_present
    - heavy_debug_code_in_hot_paths
    - capability_advertisement_incomplete
  deferred_constraints:
    - "Keep temporary AGAW hack in place during current finicky-hardware bringup; revisit only after passthru is sufficiently stable."
  coding_constraints:
    - "ASCII-only source edits: do not add non-ASCII characters to source files."
  smoke_matrix:
    - id: SMK-001
      area: assignment
      title: "vm_assign_pptdev assigns GPU device cleanly"
      status: todo
    - id: SMK-002
      area: bar
      title: "BAR region query and initial mapping"
      status: todo
    - id: SMK-003
      area: dma
      title: "GPU DMA path via IOMMU mapping (guest RAM access)"
      status: todo
    - id: SMK-004
      area: msi
      title: "MSI programming and interrupt delivery"
      status: todo
    - id: SMK-005
      area: msix
      title: "MSI-X table setup and interrupt delivery"
      status: todo
    - id: SMK-006
      area: reset
      title: "FLR/vendor/bus reset and reassign stability"
      status: todo
    - id: SMK-007
      area: rom
      title: "ROM BAR exposure and guest init behavior"
      status: todo
    - id: SMK-008
      area: reboot
      title: "Guest reboot/restart keeps passthru functional"
      status: todo

next_actions:
  - id: NA-001
    priority: P0
    title: "Make PPT_GET_CAPS capability bits match implemented ioctls"
    status: done
    completed_utc: 2026-02-19T03:34:12Z
    completion_note: "PPT_GET_CAPS now advertises BAR_INFO + IOMMU, matching implemented PPT_GET_REGION_INFO and PPT_IOMMU_* ioctl paths."
    owner: jonathan-wagner
  - id: NA-002
    priority: P0
    title: "Remove or gate AGAW hardcode in immu_regs.c"
    status: deferred
    defer_reason: "User decision: keep temporary AGAW hack until passthru is working sufficiently on current finicky hardware."
    owner: jonathan-wagner
  - id: NA-003
    priority: P1
    title: "Consolidate TLB invalidation policy to avoid duplicate flushes"
    status: done
    completed_utc: 2026-02-19T03:47:29Z
    completion_note: "Moved to explicit caller-managed invalidation: iommu_domain_map/unmap no longer flush, and ppt batch map/unmap now performs one flush per batch."
    owner: jonathan-wagner
  - id: NA-004
    priority: P1
    title: "Add passthrough smoke matrix run log (BAR/MSI/MSIX/reset/ROM)"
    status: todo
    unblocked_utc: 2026-02-20T19:11:02Z
    unblocked_reason: "Targeted builder validation now compiles vmm_vtd, ppt, and vmm successfully."
    owner: jonathan-wagner
  - id: NA-005
    priority: P0
    title: "Fix merge regression in vmm.c vm_memquery_list function termination"
    status: done
    completed_utc: 2026-02-20T19:11:02Z
    completion_note: "Issue no longer reproduces in targeted builder runs (`vmm` link + ctfmerge succeed on 192.168.1.210)."
    owner: jonathan-wagner

findings:
  - id: F-001
    severity: high
    summary: "AGAW currently forced to fixed values (debug hack)"
    evidence:
      - usr/src/uts/i86pc/io/immu_regs.c
    detail: "Code forces `agaw = 48` and `nlevels = 4` with explicit dirty-hack comments; high risk on non-target platforms."
  - id: F-002
    severity: medium
    summary: "Capability reporting appears behind implementation"
    evidence:
      - usr/src/uts/intel/io/vmm/io/ppt.c
      - usr/src/uts/intel/sys/ppt_dev.h
      - usr/src/uts/common/sys/ppt_ioctl.h
    detail: "`PPT_GET_CAPS` currently advertises BAR_INFO only, while broader mapping ioctls exist."
  - id: F-003
    severity: medium
    summary: "Potential redundant TLB invalidation"
    evidence:
      - usr/src/uts/intel/io/vmm/io/iommu.c
      - usr/src/uts/intel/io/vmm/io/ppt.c
    detail: "Map/unmap wrappers invalidate TLB and caller paths also invalidate in some flows."
  - id: F-004
    severity: medium
    summary: "Debug instrumentation is extensive in interrupt and passthru paths"
    evidence:
      - usr/src/uts/i86pc/io/immu_intrmap.c
      - usr/src/uts/i86pc/io/pcplusmp/apic_common.c
      - usr/src/uts/intel/io/vmm/io/ppt.c
      - usr/src/cmd/bhyve/common/pci_passthru.c
    detail: "High log volume and debug toggles may affect stability/observability under load."
  - id: F-005
    severity: critical
    summary: "Merge introduced compile-breaking truncation in vm_memquery_list()"
    evidence:
      - usr/src/uts/intel/io/vmm/vmm.c:5391
      - usr/src/uts/intel/io/vmm/vmm.c:5395
    detail: "After merge commit 1b58ea7ae1, vm_memquery_list() no longer ends with `return (0);` and closing brace before vm_mmiohook_init(), which is now parsed as if nested in the previous function."

history:
  - entry_id: H-2026-02-20-002
    timestamp_utc: 2026-02-20T19:11:02Z
    actor: codex
    type: change
    summary: "Hardened DMA map flow and validated targeted kernel module builds on builder host."
    refs:
      commits: []
      files:
        - usr/src/uts/intel/io/vmm/intel/vtd.c
        - usr/src/uts/intel/io/vmm/io/iommu.c
        - usr/src/uts/intel/io/vmm/io/ppt.c
        - iommu.md
      commands:
        - "/build/bin/smartbuild --no-auto --tail 120"
        - "/build/bin/build-targeted-builder (helper script in /build/bin, outside repo)"
    results:
      - "Added DRHD-enable gating/tunable path and converted scope-miss host panic in `vtd_add_device()` into warning+return for stability."
      - "Added VT-d context/IOTLB invalidation immediately after context-entry programming in `vtd_add_device()`."
      - "Added IOMMU map/unmap input validation and no-progress guards in `iommu.c`."
      - "Updated PPT batch map/unmap handling to propagate first error and flush TLB only when work was performed."
      - "Targeted builder runs complete successfully for `vmm_vtd`, `ppt`, and `vmm` on 192.168.1.210."
    risks:
      - "Scope-miss behavior now skips mapping instead of panicking; passthrough runtime validation is still required to ensure guest-visible behavior is acceptable."
    next_actions:
      - "Run SMK-003 DMA smoke with real guest workload and collect `iommu` fault counters/logs."
      - "Run SMK-004/SMK-005 interrupt delivery tests (MSI/MSI-X) with the same kernel."
  - entry_id: H-2026-02-20-001
    timestamp_utc: 2026-02-20T02:43:04Z
    actor: codex
    type: change
    summary: "Committed testing-only smartos-live build identity changes."
    refs:
      commits:
        - 4c4c6f38
      files:
        - /build/smartos-live/Makefile
        - /build/smartos-live/configure
        - iommu.md
      commands: []
    results:
      - "smartos-live Makefile now forces BUILDSTAMP=TESTING for platform naming."
      - "smartos-live configure keeps testing fork default (`git_stem=https://github.com/dustyhair`)."
    risks:
      - "These settings are testing-only and should not be used for release builds."
    next_actions:
      - "Build again and verify output paths and version files use TESTING stamp."
  - entry_id: H-2026-02-19-006
    timestamp_utc: 2026-02-19T23:33:02Z
    actor: codex
    type: review
    summary: "Reviewed merge of origin/master into iommu-testing for passthrough alignment."
    refs:
      commits:
        - 1b58ea7ae1888dc40ac1873d1db01cbddbf8dc0b
      files:
        - usr/src/uts/intel/io/vmm/vmm.c
        - usr/src/cmd/bhyve/common/pci_passthru.c
        - usr/src/cmd/bhyve/common/pci_emul.c
        - usr/src/cmd/bhyve/common/pci_emul.h
        - usr/src/uts/intel/io/vmm/vmm.mapfile
        - iommu.md
      commands: []
    results:
      - "Critical regression found: vm_memquery_list termination lost in merge result."
      - "Passthrough core files (iommu.c, ppt.c, immu_regs.c, immu_intrmap.c, rootnex.conf, ppt_dev.h, ppt_ioctl.h) were not modified by this merge."
      - "Merged bhyve callback semantics (`PE_CFGRW_*`) in pci_emul/pci_passthru are directionally compatible with current passthrough goals."
    risks:
      - "Build/test pipeline blocked until vmm.c compile break is fixed."
    next_actions:
      - "Fix F-005 first, then resume NA-004 smoke matrix execution."
  - entry_id: H-2026-02-19-005
    timestamp_utc: 2026-02-19T04:04:48Z
    actor: codex
    type: change
    summary: "Initialized structured GPU passthrough smoke matrix for NA-004 execution tracking."
    refs:
      commits: []
      files:
        - iommu.md
      commands: []
    results:
      - "Added SMK-001..SMK-008 checklist entries under state.smoke_matrix with todo status."
      - "Matrix now provides stable IDs for pass/fail run logging."
    risks:
      - "No execution results recorded yet."
    next_actions:
      - "Run first matrix item (SMK-001) and append result entry."
  - entry_id: H-2026-02-19-004
    timestamp_utc: 2026-02-19T03:47:29Z
    actor: codex
    type: change
    summary: "Consolidated IOMMU TLB invalidation policy to explicit caller flushes."
    refs:
      commits: []
      files:
        - usr/src/uts/intel/io/vmm/io/iommu.c
        - usr/src/uts/intel/io/vmm/io/ppt.c
        - iommu.md
      commands: []
    results:
      - "Removed implicit TLB invalidation from `iommu_domain_map()` and `iommu_domain_unmap()`."
      - "Kept explicit flush in single `PPT_IOMMU_MAP/UNMAP` paths."
      - "Added one explicit flush after `PPT_IOMMU_MAP_BATCH` and `PPT_IOMMU_UNMAP_BATCH` loops."
    risks:
      - "Any new future caller of iommu_domain_map/unmap must explicitly invalidate TLB."
    next_actions:
      - "Proceed with NA-004 passthru smoke matrix execution and logging."
  - entry_id: H-2026-02-19-003
    timestamp_utc: 2026-02-19T03:34:12Z
    actor: codex
    type: change
    summary: "Aligned PPT_GET_CAPS advertisement with implemented ppt ioctl capability surface."
    refs:
      commits: []
      files:
        - usr/src/uts/intel/io/vmm/io/ppt.c
        - iommu.md
      commands: []
    results:
      - "Updated `PPT_GET_CAPS` to set `PPT_CAP_BAR_INFO | PPT_CAP_IOMMU`."
      - "Did not advertise IRQ_REMAP/RESET/MSI/INTX yet to avoid over-claiming before validation."
    risks:
      - "Capability set remains intentionally conservative; additional bits may be enabled later after validation."
    next_actions:
      - "Proceed to NA-003 (TLB invalidation consolidation) or NA-004 (smoke matrix execution)."
  - entry_id: H-2026-02-19-002
    timestamp_utc: 2026-02-19T03:27:00Z
    actor: jonathan-wagner
    type: decision
    summary: "Deferred AGAW hack cleanup until GPU passthru is sufficiently working on current hardware."
    refs:
      commits: []
      files:
        - usr/src/uts/i86pc/io/immu_regs.c
        - iommu.md
      commands: []
    results:
      - "AGAW hardcode remains intentionally enabled during bringup."
      - "Cleanup remains required later and is tracked as deferred action NA-002."
    risks:
      - "Potential platform-specific side effects remain while hack is active."
    next_actions:
      - "Proceed with non-AGAW passthru bringup tasks first."
  - entry_id: H-2026-02-19-001
    timestamp_utc: 2026-02-19T03:21:19Z
    actor: codex
    type: review
    summary: "Reviewed all commits after baseline and extracted passthrough-relevant changes"
    commits:
      - hash: 3700ed08ec64d5ac609ce5fdc8f1451fb11beeb4
        date_utc: 2025-09-05T17:29:23Z
        title: "Made changs to add ABI for passthru improvements"
        files_changed_count: 5
        insertions: 151
        deletions: 32
        key_changes:
          - "Added `PPT_GET_CAPS` and `PPT_GET_REGION_INFO` ABI/types in `ppt_dev.h`."
          - "Implemented new ioctl handlers in `ppt.c` for capability and BAR region discovery."
          - "Updated bhyve `pci_passthru.c` to probe new ABI and fallback to legacy BAR query."
          - "Enabled IOMMU defaults in `rootnex.conf`; relaxed AGAW validation guard."
        impact:
          - "Establishes compatibility bridge for old/new ppt interfaces."
      - hash: 61715aa2aae707c7a063db7568490b549463d183
        date_utc: 2025-09-05T17:42:48Z
        title: "Moved ppt_get_caps Function before use"
        files_changed_count: 1
        insertions: 12
        deletions: 15
        key_changes:
          - "Refactor in `pci_passthru.c`: helper placement/order cleanup for `ppt_get_caps()`."
        impact:
          - "No intended functional change; readability and call-order cleanup."
      - hash: 0075f74b7b24fc52ab431d93e03e34a7f47c996d
        date_utc: 2026-02-19T03:00:38Z
        title: "Lots of things changed"
        files_changed_count: 21
        insertions: 2824
        deletions: 595
        key_changes:
          - "Added `usr/src/uts/common/sys/ppt_ioctl.h` common ioctl ABI definitions."
          - "Extended `ppt_dev.h` with IOMMU map/unmap structs and batch ioctls."
          - "Implemented `PPT_IOMMU_MAP/UNMAP` (+batch) handlers in kernel `ppt.c`."
          - "Added/exported `iommu_domain_map()` and `iommu_domain_unmap()` plumbing."
          - "Added `VM_MEMQUERY` and `VM_MEMQUERY_LIST` interfaces and implementation."
          - "Modified bhyve passthru BAR flow to use ppt/IOMMU mappings."
          - "Expanded reset paths (FLR/vendor/bus) and interrupt-remap diagnostics."
          - "Enabled interrupt remapping in `rootnex.conf`."
          - "Inserted explicit AGAW/nlevels hardcode in `immu_regs.c` (debug hack)."
        impact:
          - "Introduces end-to-end primitives for explicit GPA->HPA->IOMMU mapping for passthru devices."
          - "Substantial progress, but code still in bringup/debug state."

open_questions:
  - id: Q-001
    question: "Should `PPT_GET_CAPS` advertise IOMMU mapping bits immediately or after additional validation?"
    status: open
  - id: Q-002
    question: "What is the intended long-term AGAW selection strategy per platform/DRHD capabilities?"
    status: open
  - id: Q-003
    question: "Which logs/debug toggles should remain in production kernels vs DEBUG builds only?"
    status: open
```

## Append Template
Use this exact skeleton for future entries.

```yaml
- entry_id: H-YYYY-MM-DD-XXX
  timestamp_utc: YYYY-MM-DDTHH:MM:SSZ
  actor: <name>
  type: <review|change|test|decision|incident>
  summary: "<one line>"
  refs:
    commits: []
    files: []
    commands: []
  results:
    - "<fact 1>"
    - "<fact 2>"
  risks:
    - "<risk or none>"
  next_actions:
    - "<action>"
```
