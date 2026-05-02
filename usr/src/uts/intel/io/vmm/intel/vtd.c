/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2011 NetApp, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY NETAPP, INC ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL NETAPP, INC OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */
/* This file is dual-licensed; see usr/src/contrib/bhyve/LICENSE */

/*
 * Copyright 2018 Joyent, Inc.
 * Copyright 2022 Oxide Computer Company
 */

#include <sys/cdefs.h>

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/kmem.h>
#include <sys/synch.h>

#include <dev/pci/pcireg.h>

#include <machine/vmparam.h>
#include <sys/vmm_vm.h>

#include <contrib/dev/acpica/include/acpi.h>

#include <sys/sunndi.h>

#include "io/iommu.h"

/*
 * rootnex/immu_intrmap owns interrupt-remap hardware state on SmartOS.  We
 * coordinate DRHD transitions through these exported hooks without pulling in
 * i86pc private headers from this build path.
 */
extern void immu_intrmap_drhd_transition_set(int, boolean_t);

/*
 * Documented in the "Intel Virtualization Technology for Directed I/O",
 * Architecture Spec, September 2008.
 */

#define	VTD_DRHD_INCLUDE_PCI_ALL(Flags)  (((Flags) >> 0) & 0x1)

/* Section 10.4 "Register Descriptions" */
struct vtdmap {
	volatile uint32_t	version;
	volatile uint32_t	res0;
	volatile uint64_t	cap;
	volatile uint64_t	ext_cap;
	volatile uint32_t	gcr;
	volatile uint32_t	gsr;
	volatile uint64_t	rta;
	volatile uint64_t	ccr;
};

#define	VTD_CAP_SAGAW(cap)	(((cap) >> 8) & 0x1F)
#define	VTD_CAP_ND(cap)		((cap) & 0x7)
#define	VTD_CAP_CM(cap)		(((cap) >> 7) & 0x1)
//XXX this is sus
//#define	VTD_CAP_SPS(cap)	(((cap) >> 34) & 0x7)
#define	VTD_CAP_SPS(cap)	(((cap) >> 34) & 0xF)
#define	VTD_CAP_RWBF(cap)	(((cap) >> 4) & 0x1)

#define	VTD_ECAP_DI(ecap)	(((ecap) >> 2) & 0x1)
#define	VTD_ECAP_IR(ecap)	(((ecap) >> 3) & 0x1)
#define	VTD_ECAP_COHERENCY(ecap) ((ecap) & 0x1)
#define	VTD_ECAP_IRO(ecap)	(((ecap) >> 8) & 0x3FF)

#define	VTD_GCR_WBF		(1 << 27)
#define	VTD_GCR_SRTP		(1 << 30)
#define	VTD_GCR_TE		(1U << 31)
#define	VTD_GCR_IRE		(1U << 25)

#define	VTD_GSR_WBFS		(1 << 27)
#define	VTD_GSR_RTPS		(1 << 30)
#define	VTD_GSR_TES		(1U << 31)

#define	VTD_CCR_ICC		(1UL << 63)	/* invalidate context cache */
#define	VTD_CCR_CIRG_GLOBAL	(1UL << 61)	/* global invalidation */
#define	VTD_CCR_CAIG(_ccr)	(((_ccr) >> 59) & 0x3)	/* actual granularity */

/*
 * Fault event/status registers (Intel VT-d spec, register block section).
 * These are used to mask fault-event interrupts during DRHD bring-up/teardown
 * to avoid delivering a bogus local-APIC vector before fault MSI is valid.
 */
#define	VTD_REG_FSTS_OFF	0x34
#define	VTD_REG_FECTL_OFF	0x38
#define	VTD_REG_FEDATA_OFF	0x3c
#define	VTD_REG_FEADDR_OFF	0x40
#define	VTD_REG_FEUADDR_OFF	0x44

#define	VTD_FECTL_IM		(1U << 31)

#define	VTD_IIR_IVT		(1UL << 63)	/* invalidation IOTLB */
#define	VTD_IIR_IIRG_GLOBAL	(1ULL << 60)	/* global IOTLB invalidation */
#define	VTD_IIR_IIRG_DOMAIN	(2ULL << 60)	/* domain IOTLB invalidation */
#define	VTD_IIR_IIRG_PAGE	(3ULL << 60)	/* page IOTLB invalidation */
#define	VTD_IIR_IAIG(_iir)	(((_iir) >> 57) & 0x3)	/* actual granularity */
#define	VTD_IIR_DRAIN_READS	(1ULL << 49)	/* drain pending DMA reads */
#define	VTD_IIR_DRAIN_WRITES	(1ULL << 48)	/* drain pending DMA writes */
#define	VTD_IIR_DOMAIN_P	32

#define	VTD_ROOT_PRESENT	0x1
#define	VTD_CTX_PRESENT		0x1
#define	VTD_CTX_TT_ALL		(1UL << 2)

#define	VTD_PTE_RD		(1UL << 0)
#define	VTD_PTE_WR		(1UL << 1)
#define	VTD_PTE_SUPERPAGE	(1UL << 7)
#define	VTD_PTE_ADDR_M		(0x000FFFFFFFFFF000UL)

#define	VTD_RID2IDX(rid)	(((rid) & 0xff) * 2)

struct domain {
	uint64_t	*ptp;		/* first level page table page */
	int		pt_levels;	/* number of page table levels */
	int		addrwidth;	/* 'AW' field in context entry */
	int		spsmask;	/* supported super page sizes */
	uint_t		id;		/* domain id */
	vm_paddr_t	maxaddr;	/* highest address to be mapped */
	SLIST_ENTRY(domain) next;
};

static SLIST_HEAD(, domain) domhead;

#define	DRHD_MAX_UNITS	16
static ACPI_DMAR_HARDWARE_UNIT	*drhds[DRHD_MAX_UNITS];
static int			drhd_num;
static struct vtdmap		*vtdmaps[DRHD_MAX_UNITS];
static int			max_domains;
typedef int			(*drhd_ident_func_t)(void);
static dev_info_t		*vtddips[DRHD_MAX_UNITS];
static kmutex_t		vtd_invalidate_locks[DRHD_MAX_UNITS];
static boolean_t		vtd_invalidate_locks_init;
static uint32_t		vtd_invalidate_lock_contention[DRHD_MAX_UNITS];
static uint32_t		vtd_timeout_dumped_mask;
static uint32_t		vtd_bringup_dumped_te_mask;
static uint32_t		vtd_bringup_dumped_hostadd_mask;
static uint32_t		vtd_ctxcmd_first_dumped_mask;
static uint32_t		vtd_iotlbcmd_first_dumped_mask;
static uint32_t		vtd_ctxcmd_prebusy_dumped_mask;
static uint32_t		vtd_iotlbcmd_prebusy_dumped_mask;
static uint32_t		vtd_init_stage_rta_mask;
static uint32_t		vtd_init_stage_srtp_mask;
static uint32_t		vtd_init_stage_te_mask;
static uint32_t		vtd_init_order_warned_mask;
static boolean_t		vtd_host_domain_bootstrap_active;
static uint32_t		vtd_ctxinv_skip_mask;
static uint32_t		vtd_iotlbinv_skip_mask;
static uint32_t		vtd_ctxinv_warned_mask;
static uint32_t		vtd_iotlbinv_warned_mask;
/*
 * Bitmask of DRHD units to ignore. Bit N skips DRHD index N.
 *
 * Example for a broken DRHD0:
 *   set vtd_drhd_ignore_mask=0x1
 */
uint32_t vtd_drhd_ignore_mask = 0x0;
/*
 * Upper bound (in microseconds) for polling hardware completion bits.
 * These are intentionally tunable so hang behavior can be adjusted in
 * field diagnostics without rebuilding.
 */
uint32_t vtd_wait_timeout_us = 2000000;
uint32_t vtd_wait_delay_us = 1;
uint32_t vtd_trace_lifecycle = 0;
uint32_t vtd_trace_invalidate = 0;
uint32_t vtd_trace_invlock = 0;
uint32_t vtd_trace_timeout_dump = 0;
uint32_t vtd_trace_bringup_dump = 0;
uint32_t vtd_trace_inv_precheck = 0;
/*
 * Debug knob: skip invalidate commands when TES is clear.
 *
 * Default is off. Skipping invalidations can hide coherency bugs and should
 * only be used for narrow diagnostics.
 */
uint32_t vtd_skip_invalidate_if_tes_clear = 0;
uint32_t vtd_trace_inv_cmd = 0;
uint32_t vtd_trace_init_order = 0;
uint32_t vtd_trace_map = 0;
uint32_t vtd_trace_map_verbose = 0;
uint32_t vtd_trace_domid = 0xffffffffU;
uint32_t vtd_trace_remove_state = 0;
/*
 * Diagnostic recovery knob: if removing a non-host device from a domain
 * leaves the DRHD invalidate engine wedged, toggle translation on that DRHD
 * under the rootnex interrupt-remap transition gate and retry invalidation.
 */
uint32_t vtd_rearm_on_remove_timeout = 1;
/*
 * Debug/mitigation knob: quiesce host interrupt-remap updates while vmm_vtd is
 * updating or invalidating DMA mappings for a non-host domain. This widens the
 * existing DRHD transition gate beyond vtd_enable() and targets the
 * vm_assign_pptdev/vm_iommu_modify mapping burst where APIC ESR=0x40 has been
 * observed.
 */
uint32_t vtd_intrmap_quiesce_on_mapping = 1;
uint32_t vtd_intrmap_quiesce_on_mapping_host = 0;
/*
 * By default, hold the immu_intrmap quiesce gate at the batched TLB
 * invalidate boundary rather than on every individual page-table update.
 * vm_iommu_modify() performs many vtd_update_mapping() calls and then issues a
 * single invalidate; per-update enter/exit churn can itself trigger the APIC
 * ESR=0x40 failure on this platform.
 */
uint32_t vtd_intrmap_quiesce_on_mapping_updates = 0;
/*
 * Interrupt remapping is host-owned via immu_intrmap on SmartOS.
 * Leave IRE untouched in vmm_vtd by default to avoid conflicting writes.
 *
 * Set to 1 only for controlled experiments with vmm_vtd-managed IRE.
 */
uint32_t vtd_manage_ire = 0;

#define	VTD_HOST_DOMAIN_ID	1U

static uint64_t root_table[PAGE_SIZE / sizeof (uint64_t)] __aligned(4096);
static uint64_t ctx_tables[256][PAGE_SIZE / sizeof (uint64_t)] __aligned(4096);

static boolean_t vtd_ir_unit_ok(int unit, struct vtdmap *vtdmap);
static int vtd_drhd_index(struct vtdmap *vtdmap);
static void vtd_fault_intr_mask(struct vtdmap *vtdmap, boolean_t masked);
static void vtd_fault_status_clear(struct vtdmap *vtdmap, const char *tag);

static boolean_t
vtd_trace_domain_enabled(const struct domain *dom)
{
	if (dom == NULL)
		return (vtd_trace_domid == 0xffffffffU);
	return (vtd_trace_domid == 0xffffffffU || dom->id == vtd_trace_domid);
}

static boolean_t
vtd_drhd_enabled(int idx)
{
	if (idx < 0 || idx >= drhd_num)
		return (B_FALSE);
	if ((vtd_drhd_ignore_mask & (1u << idx)) != 0)
		return (B_FALSE);
	return (vtdmaps[idx] != NULL);
}

static void
vtd_fault_intr_mask(struct vtdmap *vtdmap, boolean_t masked)
{
	volatile uint32_t *fectl;
	uint32_t val, newval;

	fectl = (volatile uint32_t *)((caddr_t)vtdmap + VTD_REG_FECTL_OFF);
	val = *fectl;
	newval = masked ? (val | VTD_FECTL_IM) : (val & ~VTD_FECTL_IM);
	if (newval != val)
		*fectl = newval;
	(void)*fectl; /* flush posted write */

	if (vtd_trace_lifecycle != 0) {
		cmn_err(CE_NOTE, "vtd: fault-event intr %s drhd=%d FECTL=0x%x->0x%x "
		    "FEDATA=0x%x FEADDR=0x%x FEUADDR=0x%x",
		    masked ? "MASK" : "UNMASK", vtd_drhd_index(vtdmap), val, *fectl,
		    *(volatile uint32_t *)((caddr_t)vtdmap + VTD_REG_FEDATA_OFF),
		    *(volatile uint32_t *)((caddr_t)vtdmap + VTD_REG_FEADDR_OFF),
		    *(volatile uint32_t *)((caddr_t)vtdmap + VTD_REG_FEUADDR_OFF));
	}
}

static void
vtd_fault_status_clear(struct vtdmap *vtdmap, const char *tag)
{
	volatile uint32_t *fsts;
	uint32_t val;

	fsts = (volatile uint32_t *)((caddr_t)vtdmap + VTD_REG_FSTS_OFF);
	val = *fsts;
	if (val != 0) {
		/*
		 * Fault status bits are write-1-to-clear. Writing back the observed
		 * status clears pending fault conditions before/after TE transitions.
		 */
		*fsts = val;
		(void)*fsts; /* flush posted write */
	}

	if (vtd_trace_lifecycle != 0) {
		cmn_err(CE_NOTE, "vtd: fault-status clear drhd=%d tag=%s FSTS=0x%x "
		    "after=0x%x", vtd_drhd_index(vtdmap), tag, val, *fsts);
	}
}

static int
vtd_drhd_index(struct vtdmap *vtdmap)
{
	int i;

	for (i = 0; i < drhd_num; i++) {
		if (vtdmaps[i] == vtdmap)
			return (i);
	}

	return (-1);
}

static void
vtd_invalidate_lock_enter(struct vtdmap *vtdmap)
{
	int idx;

	if (!vtd_invalidate_locks_init)
		return;

	idx = vtd_drhd_index(vtdmap);
	if (idx < 0 || idx >= DRHD_MAX_UNITS)
		return;

	if (mutex_tryenter(&vtd_invalidate_locks[idx]) == 0) {
		uint32_t n;

		n = ++vtd_invalidate_lock_contention[idx];
		if (vtd_trace_invlock != 0 && (n <= 8 || (n % 100) == 0)) {
			cmn_err(CE_NOTE, "vtd: invalidate lock contention "
			    "drhd=%d count=%u", idx, n);
		}
		mutex_enter(&vtd_invalidate_locks[idx]);
	}
}

static void
vtd_invalidate_lock_exit(struct vtdmap *vtdmap)
{
	int idx;

	if (!vtd_invalidate_locks_init)
		return;

	idx = vtd_drhd_index(vtdmap);
	if (idx < 0 || idx >= DRHD_MAX_UNITS)
		return;

	mutex_exit(&vtd_invalidate_locks[idx]);
}

static void
vtd_dump_timeout_state_once(struct vtdmap *vtdmap, const char *what)
{
	int idx, iotlb_off;
	uint32_t bit;
	volatile uint64_t *iotlb_reg;
	uint64_t iotlb_val;

	if (vtd_trace_timeout_dump == 0)
		return;

	idx = vtd_drhd_index(vtdmap);
	if (idx < 0 || idx >= 32)
		return;

	bit = (1u << idx);
	if ((vtd_timeout_dumped_mask & bit) != 0)
		return;
	vtd_timeout_dumped_mask |= bit;

	iotlb_off = VTD_ECAP_IRO(vtdmap->ext_cap) * 16;
	iotlb_reg = (volatile uint64_t *)((caddr_t)vtdmap + iotlb_off + 8);
	iotlb_val = *iotlb_reg;

	cmn_err(CE_WARN, "vtd: first timeout state dump drhd=%d what=%s "
	    "VER=0x%x CAP=0x%llx ECAP=0x%llx GCR=0x%x GSR=0x%x CCR=0x%llx "
	    "IOTLB(off=0x%x)=0x%llx", idx, what, vtdmap->version,
	    (unsigned long long)vtdmap->cap,
	    (unsigned long long)vtdmap->ext_cap,
	    vtdmap->gcr, vtdmap->gsr,
	    (unsigned long long)vtdmap->ccr,
	    iotlb_off + 8, (unsigned long long)iotlb_val);
}

static void
vtd_dump_bringup_state_once(struct vtdmap *vtdmap, uint32_t *maskp,
    const char *tag)
{
	int idx, iotlb_off;
	uint32_t bit;
	volatile uint64_t *iotlb_reg;
	uint64_t iotlb_val;

	if (vtd_trace_bringup_dump == 0)
		return;

	idx = vtd_drhd_index(vtdmap);
	if (idx < 0 || idx >= 32)
		return;

	bit = (1u << idx);
	if ((*maskp & bit) != 0)
		return;
	*maskp |= bit;

	iotlb_off = VTD_ECAP_IRO(vtdmap->ext_cap) * 16;
	iotlb_reg = (volatile uint64_t *)((caddr_t)vtdmap + iotlb_off + 8);
	iotlb_val = *iotlb_reg;

	cmn_err(CE_NOTE, "vtd: bringup state drhd=%d tag=%s VER=0x%x CAP=0x%llx "
	    "ECAP=0x%llx GCR=0x%x GSR=0x%x CCR=0x%llx IOTLB(off=0x%x)=0x%llx",
	    idx, tag, vtdmap->version,
	    (unsigned long long)vtdmap->cap,
	    (unsigned long long)vtdmap->ext_cap,
	    vtdmap->gcr, vtdmap->gsr,
	    (unsigned long long)vtdmap->ccr,
	    iotlb_off + 8, (unsigned long long)iotlb_val);
}

static void
vtd_inv_cmd_log_once(struct vtdmap *vtdmap, uint32_t *maskp, const char *which,
    uint64_t before, uint64_t cmd, uint64_t after)
{
	int idx;
	uint32_t bit;

	if (vtd_trace_inv_cmd == 0)
		return;

	idx = vtd_drhd_index(vtdmap);
	if (idx < 0 || idx >= 32)
		return;
	bit = (1u << idx);
	if ((*maskp & bit) != 0)
		return;
	*maskp |= bit;

	cmn_err(CE_NOTE, "vtd: first %s cmd drhd=%d before=0x%llx cmd=0x%llx "
	    "after=0x%llx CAIG=%u IAIG=%u GCR=0x%x GSR=0x%x",
	    which, idx, (unsigned long long)before, (unsigned long long)cmd,
	    (unsigned long long)after, (uint_t)VTD_CCR_CAIG(after),
	    (uint_t)VTD_IIR_IAIG(after), vtdmap->gcr, vtdmap->gsr);
}

static void
vtd_inv_prebusy_log_once(struct vtdmap *vtdmap, uint32_t *maskp, const char *which,
    uint64_t regv)
{
	int idx;
	uint32_t bit;

	if (vtd_trace_inv_cmd == 0)
		return;

	idx = vtd_drhd_index(vtdmap);
	if (idx < 0 || idx >= 32)
		return;
	bit = (1u << idx);
	if ((*maskp & bit) != 0)
		return;
	*maskp |= bit;

	cmn_err(CE_WARN, "vtd: %s prebusy drhd=%d reg=0x%llx CAIG=%u IAIG=%u "
	    "GCR=0x%x GSR=0x%x", which, idx, (unsigned long long)regv,
	    (uint_t)VTD_CCR_CAIG(regv), (uint_t)VTD_IIR_IAIG(regv),
	    vtdmap->gcr, vtdmap->gsr);
}

static int
vtd_mark_mask_for_map(struct vtdmap *vtdmap, uint32_t *maskp)
{
	int idx;
	uint32_t bit;

	idx = vtd_drhd_index(vtdmap);
	if (idx < 0 || idx >= 32)
		return (-1);
	bit = (1u << idx);
	*maskp |= bit;
	return (idx);
}

static void
vtd_log_init_stage(struct vtdmap *vtdmap, const char *stage)
{
	if (vtd_trace_init_order == 0)
		return;

	cmn_err(CE_NOTE, "vtd: init-stage drhd=%d stage=%s GCR=0x%x GSR=0x%x "
	    "RTA=0x%llx CCR=0x%llx", vtd_drhd_index(vtdmap), stage,
	    vtdmap->gcr, vtdmap->gsr, (unsigned long long)vtdmap->rta,
	    (unsigned long long)vtdmap->ccr);
}

static void
vtd_check_host_add_init_order(struct vtdmap *vtdmap, uint16_t rid)
{
	int idx;
	uint32_t bit;
	boolean_t have_rta, have_srtp, have_te;

	if (vtd_trace_init_order == 0)
		return;

	idx = vtd_drhd_index(vtdmap);
	if (idx < 0 || idx >= 32)
		return;
	bit = (1u << idx);

	have_rta = ((vtd_init_stage_rta_mask & bit) != 0);
	have_srtp = ((vtd_init_stage_srtp_mask & bit) != 0);
	have_te = ((vtd_init_stage_te_mask & bit) != 0);

	if (have_rta && have_srtp && have_te)
		return;

	if ((vtd_init_order_warned_mask & bit) == 0) {
		cmn_err(CE_WARN, "vtd: host add before full init drhd=%d rid=0x%x "
		    "stages rta=%u srtp=%u te=%u GCR=0x%x GSR=0x%x",
		    idx, rid, have_rta ? 1 : 0, have_srtp ? 1 : 0, have_te ? 1 : 0,
		    vtdmap->gcr, vtdmap->gsr);
		vtd_init_order_warned_mask |= bit;
	}
}

static boolean_t
vtd_invalidate_precheck(struct vtdmap *vtdmap, const char *which)
{
	uint32_t gsr, gcr;
	int idx;
	boolean_t tes_set, rtps_set;

	gsr = vtdmap->gsr;
	gcr = vtdmap->gcr;
	tes_set = ((gsr & VTD_GSR_TES) != 0);
	rtps_set = ((gsr & VTD_GSR_RTPS) != 0);
	idx = vtd_drhd_index(vtdmap);

	if (tes_set)
		return (B_TRUE);

	if (vtd_trace_inv_precheck != 0) {
		cmn_err(CE_WARN, "vtd: precheck %s invalidate on DRHD %d with "
		    "TES clear (GCR=0x%x GSR=0x%x RTPS=%u TES=%u)", which, idx,
		    gcr, gsr, rtps_set ? 1 : 0, tes_set ? 1 : 0);
	}

	if (vtd_skip_invalidate_if_tes_clear != 0)
		return (B_FALSE);

	return (B_TRUE);
}

static boolean_t
vtd_intrmap_quiesce_domain(const struct domain *dom)
{
	if (vtd_intrmap_quiesce_on_mapping == 0 || dom == NULL)
		return (B_FALSE);

	if (vtd_intrmap_quiesce_on_mapping_host == 0 &&
	    dom->id == VTD_HOST_DOMAIN_ID)
		return (B_FALSE);

	return (B_TRUE);
}

static void
vtd_intrmap_quiesce_all(boolean_t enter)
{
	int i;

	for (i = 0; i < drhd_num; i++) {
		if (!vtd_drhd_enabled(i))
			continue;
		immu_intrmap_drhd_transition_set(i, enter);
	}
}

static boolean_t
vtd_invalidate_skip_check(uint32_t *skip_mask, uint32_t *warned_mask,
    struct vtdmap *vtdmap, const char *which)
{
	int idx;
	uint32_t bit;

	idx = vtd_drhd_index(vtdmap);
	if (idx < 0 || idx >= 32)
		return (B_FALSE);

	bit = (1u << idx);
	if ((*skip_mask & bit) == 0)
		return (B_FALSE);

	if ((*warned_mask & bit) == 0) {
		cmn_err(CE_WARN, "vtd: DRHD %d %s invalidation is in "
		    "degraded mode; skipping invalidate command",
		    idx, which);
		*warned_mask |= bit;
	}

	return (B_TRUE);
}

static void
vtd_invalidate_mark_skip(uint32_t *skip_mask, struct vtdmap *vtdmap,
    const char *which)
{
	int idx;
	uint32_t bit;

	idx = vtd_drhd_index(vtdmap);
	if (idx < 0 || idx >= 32)
		return;

	bit = (1u << idx);
	if ((*skip_mask & bit) == 0) {
		cmn_err(CE_WARN, "vtd: DRHD %d %s invalidate timed out; "
		    "future %s invalidations will be skipped on this DRHD",
		    idx, which, which);
	}
	*skip_mask |= bit;
}

static void
vtd_invalidate_clear_skip(struct vtdmap *vtdmap)
{
	int idx;
	uint32_t bit;

	idx = vtd_drhd_index(vtdmap);
	if (idx < 0 || idx >= 32)
		return;

	bit = (1u << idx);
	vtd_ctxinv_skip_mask &= ~bit;
	vtd_iotlbinv_skip_mask &= ~bit;
	vtd_ctxinv_warned_mask &= ~bit;
	vtd_iotlbinv_warned_mask &= ~bit;
	vtd_timeout_dumped_mask &= ~bit;
	vtd_bringup_dumped_te_mask &= ~bit;
	vtd_bringup_dumped_hostadd_mask &= ~bit;
	vtd_ctxcmd_first_dumped_mask &= ~bit;
	vtd_iotlbcmd_first_dumped_mask &= ~bit;
	vtd_ctxcmd_prebusy_dumped_mask &= ~bit;
	vtd_iotlbcmd_prebusy_dumped_mask &= ~bit;
	vtd_init_order_warned_mask &= ~bit;
}

static boolean_t
vtd_wait_reg32(volatile uint32_t *reg, uint32_t mask, uint32_t expect,
    struct vtdmap *vtdmap, const char *what)
{
	uint32_t val;
	uint32_t wait_us, step;

	step = MAX(vtd_wait_delay_us, 1);
	for (wait_us = 0; wait_us < vtd_wait_timeout_us; wait_us += step) {
		val = *reg;
		if ((val & mask) == expect)
			return (B_TRUE);
		drv_usecwait(step);
	}

	val = *reg;
	cmn_err(CE_WARN, "vtd: timeout waiting for %s on DRHD %d "
	    "(reg=0x%x mask=0x%x expect=0x%x timeout=%uus)",
	    what, vtd_drhd_index(vtdmap), val, mask, expect,
	    vtd_wait_timeout_us);
	vtd_dump_timeout_state_once(vtdmap, what);
	return (B_FALSE);
}

static boolean_t
vtd_wait_reg64(volatile uint64_t *reg, uint64_t mask, uint64_t expect,
    struct vtdmap *vtdmap, const char *what)
{
	uint64_t val;
	uint32_t wait_us, step;

	step = MAX(vtd_wait_delay_us, 1);
	for (wait_us = 0; wait_us < vtd_wait_timeout_us; wait_us += step) {
		val = *reg;
		if ((val & mask) == expect)
			return (B_TRUE);
		drv_usecwait(step);
	}

	val = *reg;
	cmn_err(CE_WARN, "vtd: timeout waiting for %s on DRHD %d "
	    "(reg=0x%llx mask=0x%llx expect=0x%llx timeout=%uus)",
	    what, vtd_drhd_index(vtdmap),
	    (unsigned long long)val, (unsigned long long)mask,
	    (unsigned long long)expect, vtd_wait_timeout_us);
	vtd_dump_timeout_state_once(vtdmap, what);
	return (B_FALSE);
}

static int
vtd_max_domains(struct vtdmap *vtdmap)
{
	int nd;

	nd = VTD_CAP_ND(vtdmap->cap);

	switch (nd) {
	case 0:
		return (16);
	case 1:
		return (64);
	case 2:
		return (256);
	case 3:
		return (1024);
	case 4:
		return (4 * 1024);
	case 5:
		return (16 * 1024);
	case 6:
		return (64 * 1024);
	default:
		panic("vtd_max_domains: invalid value of nd (0x%0x)", nd);
	}
}

static uint_t
domain_id(void)
{
	uint_t id;
	struct domain *dom;

	/* Skip domain id 0 - it is reserved when Caching Mode field is set */
	for (id = 1; id < max_domains; id++) {
		SLIST_FOREACH(dom, &domhead, next) {
			if (dom->id == id)
				break;
		}
		if (dom == NULL)
			break;		/* found it */
	}

	if (id >= max_domains)
		panic("domain ids exhausted");

	return (id);
}

static struct vtdmap *
vtd_device_scope(uint16_t rid)
{
	int i, remaining, pathrem;
	char *end, *pathend;
	struct vtdmap *vtdmap;
	ACPI_DMAR_HARDWARE_UNIT *drhd;
	ACPI_DMAR_DEVICE_SCOPE *device_scope;
	ACPI_DMAR_PCI_PATH *path;
	for (i = 0; i < drhd_num; i++) {
		if (!vtd_drhd_enabled(i))
			continue;
		drhd = drhds[i];

		if (VTD_DRHD_INCLUDE_PCI_ALL(drhd->Flags)) {
			/*
			 * From Intel VT-d arch spec, version 3.0:
			 * If a DRHD structure with INCLUDE_PCI_ALL flag Set is
			 * reported for a Segment, it must be enumerated by BIOS
			 * after all other DRHD structures for the same Segment.
			 */
			vtdmap = vtdmaps[i];
			return (vtdmap);
		}

		end = (char *)drhd + drhd->Header.Length;
		remaining = drhd->Header.Length -
		    sizeof (ACPI_DMAR_HARDWARE_UNIT);
		while (remaining > sizeof (ACPI_DMAR_DEVICE_SCOPE)) {
			device_scope =
			    (ACPI_DMAR_DEVICE_SCOPE *)(end - remaining);
			remaining -= device_scope->Length;

			switch (device_scope->EntryType) {
				/* 0x01 and 0x02 are PCI device entries */
				case 0x01:
				case 0x02:
					break;
				default:
					continue;
			}

			if (PCI_RID2BUS(rid) != device_scope->Bus)
				continue;

			pathend = (char *)device_scope + device_scope->Length;
			pathrem = device_scope->Length -
			    sizeof (ACPI_DMAR_DEVICE_SCOPE);
			while (pathrem >= sizeof (ACPI_DMAR_PCI_PATH)) {
				path = (ACPI_DMAR_PCI_PATH *)
				    (pathend - pathrem);
				pathrem -= sizeof (ACPI_DMAR_PCI_PATH);

				if (PCI_RID2SLOT(rid) != path->Device)
					continue;
				if (PCI_RID2FUNC(rid) != path->Function)
					continue;

				vtdmap = vtdmaps[i];
				return (vtdmap);
			}
		}
	}

	/* No matching scope */
	return (NULL);
}

static void
vtd_wbflush(struct vtdmap *vtdmap)
{

	if (VTD_ECAP_COHERENCY(vtdmap->ext_cap) == 0)
		invalidate_cache_all();

	if (VTD_CAP_RWBF(vtdmap->cap)) {
		vtdmap->gcr = VTD_GCR_WBF;
		(void) vtd_wait_reg32(&vtdmap->gsr, VTD_GSR_WBFS, 0,
		    vtdmap, "write-buffer flush (WBFS clear)");
	}
}

static boolean_t
vtd_ctx_global_invalidate(struct vtdmap *vtdmap)
{
	boolean_t ok;
	uint64_t before, cmdv, after;
	int drhd_idx = vtd_drhd_index(vtdmap);

	if (vtd_trace_invalidate != 0) {
		cmn_err(CE_NOTE, "vtd: ctx invalidate start drhd=%d", drhd_idx);
	}

	if (vtd_invalidate_skip_check(&vtd_ctxinv_skip_mask,
	    &vtd_ctxinv_warned_mask, vtdmap, "context-cache")) {
		return (B_TRUE);
	}
	if (!vtd_invalidate_precheck(vtdmap, "context-cache"))
		return (B_FALSE);

	vtd_invalidate_lock_enter(vtdmap);
	before = vtdmap->ccr;
	if ((before & VTD_CCR_ICC) != 0) {
		vtd_inv_prebusy_log_once(vtdmap, &vtd_ctxcmd_prebusy_dumped_mask,
		    "ctx", before);
	}
	cmdv = VTD_CCR_ICC | VTD_CCR_CIRG_GLOBAL;
	vtdmap->ccr = cmdv;
	after = vtdmap->ccr;
	vtd_inv_cmd_log_once(vtdmap, &vtd_ctxcmd_first_dumped_mask, "ctx",
	    before, cmdv, after);
	ok = vtd_wait_reg64(&vtdmap->ccr, VTD_CCR_ICC, 0, vtdmap,
	    "context cache invalidate (ICC clear)");
	vtd_invalidate_lock_exit(vtdmap);
	if (!ok)
		vtd_invalidate_mark_skip(&vtd_ctxinv_skip_mask, vtdmap,
		    "context-cache");
	else if (vtd_trace_invalidate != 0)
		cmn_err(CE_NOTE, "vtd: ctx invalidate done drhd=%d", drhd_idx);
	return (ok);
}

static boolean_t
vtd_iotlb_global_invalidate(struct vtdmap *vtdmap)
{
	int offset;
	volatile uint64_t *iotlb_reg;
	uint64_t before, cmdv, after, val;
	boolean_t ok;
	int drhd_idx = vtd_drhd_index(vtdmap);

	if (vtd_trace_invalidate != 0) {
		cmn_err(CE_NOTE, "vtd: iotlb invalidate start drhd=%d", drhd_idx);
	}

	if (vtd_invalidate_skip_check(&vtd_iotlbinv_skip_mask,
	    &vtd_iotlbinv_warned_mask, vtdmap, "IOTLB")) {
		return (B_TRUE);
	}
	if (!vtd_invalidate_precheck(vtdmap, "IOTLB"))
		return (B_FALSE);

	vtd_invalidate_lock_enter(vtdmap);
	vtd_wbflush(vtdmap);

	offset = VTD_ECAP_IRO(vtdmap->ext_cap) * 16;
	iotlb_reg = (volatile uint64_t *)((caddr_t)vtdmap + offset + 8);

	before = *iotlb_reg;
	if ((before & VTD_IIR_IVT) != 0) {
		vtd_inv_prebusy_log_once(vtdmap, &vtd_iotlbcmd_prebusy_dumped_mask,
		    "iotlb", before);
	}
	cmdv = VTD_IIR_IVT | VTD_IIR_IIRG_GLOBAL |
	    VTD_IIR_DRAIN_READS | VTD_IIR_DRAIN_WRITES;
	*iotlb_reg = cmdv;
	after = *iotlb_reg;
	vtd_inv_cmd_log_once(vtdmap, &vtd_iotlbcmd_first_dumped_mask, "iotlb",
	    before, cmdv, after);
	ok = vtd_wait_reg64(iotlb_reg, VTD_IIR_IVT, 0, vtdmap,
	    "IOTLB invalidate (IVT clear)");
	vtd_invalidate_lock_exit(vtdmap);
	if (!ok) {
		val = *iotlb_reg;
		cmn_err(CE_WARN, "vtd: IOTLB register after timeout on DRHD %d "
		    "(reg=0x%llx)", drhd_idx,
		    (unsigned long long)val);
		vtd_invalidate_mark_skip(&vtd_iotlbinv_skip_mask, vtdmap,
		    "IOTLB");
		return (B_FALSE);
	}

	if (vtd_trace_invalidate != 0) {
		cmn_err(CE_NOTE, "vtd: iotlb invalidate done drhd=%d", drhd_idx);
	}

	return (B_TRUE);
}

/*
 * Synchronize hardware caches after context-entry ownership changes.
 * If 'scope' is non-NULL, invalidate only that remapping unit; otherwise
 * invalidate all enabled units.
 */
static boolean_t
vtd_context_changed_invalidate(struct vtdmap *scope)
{
	int i;
	boolean_t ok = B_TRUE;

	if (scope != NULL) {
		if (vtd_trace_invalidate != 0) {
			cmn_err(CE_NOTE, "vtd: invalidate scope=drhd%d",
			    vtd_drhd_index(scope));
		}
		if (!vtd_ctx_global_invalidate(scope))
			ok = B_FALSE;
		if (!vtd_iotlb_global_invalidate(scope))
			ok = B_FALSE;
		return (ok);
	}

	if (vtd_trace_invalidate != 0) {
		cmn_err(CE_NOTE, "vtd: invalidate scope=all-enabled");
	}

	for (i = 0; i < drhd_num; i++) {
		struct vtdmap *vtdmap;

		if (!vtd_drhd_enabled(i))
			continue;
		vtdmap = vtdmaps[i];
		if (!vtd_ctx_global_invalidate(vtdmap))
			ok = B_FALSE;
		if (!vtd_iotlb_global_invalidate(vtdmap))
			ok = B_FALSE;
	}

	if (vtd_trace_invalidate != 0) {
		cmn_err(CE_NOTE, "vtd: invalidate scope=all-enabled result=%d",
		    ok ? 1 : 0);
	}

	return (ok);
}

static boolean_t
vtd_translation_enable(struct vtdmap *vtdmap)
{
	boolean_t ok;

	/*
	 * Prevent VT-d fault-event interrupts from firing with an invalid vector
	 * while the unit is transitioning into TE and fault MSI state may not yet
	 * be fully initialized.
	 */
	vtd_fault_intr_mask(vtdmap, B_TRUE);
	vtd_fault_status_clear(vtdmap, "pre-te");
	vtd_dump_bringup_state_once(vtdmap, &vtd_bringup_dumped_te_mask,
	    "pre-translation-enable");
	vtd_log_init_stage(vtdmap, "te-cmd");

	vtdmap->gcr = VTD_GCR_TE;
	ok = vtd_wait_reg32(&vtdmap->gsr, VTD_GSR_TES, VTD_GSR_TES,
	    vtdmap, "translation enable (TES set)");
	if (!ok)
		return (B_FALSE);
	vtd_fault_status_clear(vtdmap, "post-te");

	(void) vtd_mark_mask_for_map(vtdmap, &vtd_init_stage_te_mask);
	vtd_log_init_stage(vtdmap, "te-done");
	return (B_TRUE);
}

static void
vtd_translation_disable(struct vtdmap *vtdmap)
{
	vtd_fault_intr_mask(vtdmap, B_TRUE);
	vtd_fault_status_clear(vtdmap, "pre-td");

	vtdmap->gcr = 0;
	(void) vtd_wait_reg32(&vtdmap->gsr, VTD_GSR_TES, 0,
	    vtdmap, "translation disable (TES clear)");
	vtd_fault_status_clear(vtdmap, "post-td");
}

static boolean_t
vtd_rearm_unit_and_invalidate(struct vtdmap *vtdmap)
{
	int idx;
	boolean_t gate_entered = B_FALSE;
	boolean_t ok = B_FALSE;

	if (vtdmap == NULL)
		return (B_FALSE);

	idx = vtd_drhd_index(vtdmap);
	if (vtd_trace_invalidate != 0) {
		cmn_err(CE_NOTE, "vtd: rearm start drhd=%d", idx);
	}

	if (idx >= 0) {
		immu_intrmap_drhd_transition_set(idx, B_TRUE);
		gate_entered = B_TRUE;
	}

	/*
	 * Recover from a potentially wedged invalidate engine by toggling
	 * translation and re-issuing invalidate commands.
	 */
	vtd_translation_disable(vtdmap);
	vtdmap->rta = vtophys(root_table);
	vtdmap->gcr = VTD_GCR_SRTP;
	if (!vtd_wait_reg32(&vtdmap->gsr, VTD_GSR_RTPS, VTD_GSR_RTPS,
	    vtdmap, "set-root-table (RTPS set)")) {
		goto out;
	}
	if (!vtd_translation_enable(vtdmap))
		goto out;
	vtd_invalidate_clear_skip(vtdmap);
	if (vtd_trace_invalidate != 0) {
		cmn_err(CE_NOTE, "vtd: rearm done drhd=%d", idx);
	}
	ok = vtd_context_changed_invalidate(vtdmap);

out:
	if (gate_entered)
		immu_intrmap_drhd_transition_set(idx, B_FALSE);
	return (ok);
}

static boolean_t
vtd_unit_init_complete(struct vtdmap *vtdmap)
{
	int idx;
	uint32_t bit;

	idx = vtd_drhd_index(vtdmap);
	if (idx < 0 || idx >= 32)
		return (B_FALSE);

	bit = (1u << idx);
	return (((vtd_init_stage_rta_mask & bit) != 0) &&
	    ((vtd_init_stage_srtp_mask & bit) != 0) &&
	    ((vtd_init_stage_te_mask & bit) != 0));
}

static boolean_t
vtd_ensure_unit_enabled(struct vtdmap *vtdmap)
{
	int idx;
	boolean_t gate_entered = B_FALSE;
	boolean_t ok = B_FALSE;

	if (vtdmap == NULL)
		return (B_FALSE);

	idx = vtd_drhd_index(vtdmap);
	if (idx < 0 || !vtd_drhd_enabled(idx))
		return (B_FALSE);

	if (vtd_unit_init_complete(vtdmap))
		return (B_TRUE);

	cmn_err(CE_NOTE, "vtd: ensure-unit-enable drhd=%d begin", idx);
	immu_intrmap_drhd_transition_set(idx, B_TRUE);
	gate_entered = B_TRUE;

	vtd_wbflush(vtdmap);

	vtdmap->rta = vtophys(root_table);
	(void) vtd_mark_mask_for_map(vtdmap, &vtd_init_stage_rta_mask);
	vtd_log_init_stage(vtdmap, "rta-write");
	vtdmap->gcr = VTD_GCR_SRTP;
	if (!vtd_wait_reg32(&vtdmap->gsr, VTD_GSR_RTPS, VTD_GSR_RTPS,
	    vtdmap, "set-root-table (RTPS set)")) {
		cmn_err(CE_WARN, "vtd: ensure-unit-enable drhd=%d SRTP timeout",
		    idx);
		goto out;
	}
	(void) vtd_mark_mask_for_map(vtdmap, &vtd_init_stage_srtp_mask);
	vtd_log_init_stage(vtdmap, "srtp-done");

	if (!vtd_translation_enable(vtdmap)) {
		cmn_err(CE_WARN, "vtd: ensure-unit-enable drhd=%d TE timeout", idx);
		goto out;
	}

	if (!vtd_ctx_global_invalidate(vtdmap) ||
	    !vtd_iotlb_global_invalidate(vtdmap)) {
		cmn_err(CE_WARN, "vtd: ensure-unit-enable drhd=%d invalidate failed",
		    idx);
		goto out;
	}

	if (vtd_ir_unit_ok(idx, vtdmap)) {
		vtdmap->gcr = VTD_GCR_TE | VTD_GCR_IRE;
		cmn_err(CE_NOTE,
		    "vtd_enable: DRHD %d translation=ENABLED, interrupt remap=ENABLED "
		    "(vmm_vtd-managed)", idx);
	} else if (VTD_ECAP_IR(vtdmap->ext_cap) != 0) {
		cmn_err(CE_NOTE,
		    "vtd_enable: DRHD %d translation=ENABLED, interrupt remap=HOST-OWNED "
		    "(immu_intrmap), gsr.ires=%d", idx,
		    ((vtdmap->gsr & VTD_GCR_IRE) != 0) ? 1 : 0);
	} else {
		cmn_err(CE_NOTE,
		    "vtd_enable: DRHD %d translation=ENABLED, interrupt remap=UNSUPPORTED",
		    idx);
	}

	cmn_err(CE_NOTE, "vtd_enable: DRHD %d TE bit %s (GSR=0x%x)",
	    idx, (vtdmap->gsr & VTD_GSR_TES) ? "ENABLED" : "DISABLED",
	    vtdmap->gsr);
	cmn_err(CE_NOTE, "vtd: ensure-unit-enable drhd=%d done", idx);
	ok = B_TRUE;
out:
	if (gate_entered)
		immu_intrmap_drhd_transition_set(idx, B_FALSE);
	return (ok);
}

static void *
vtd_map(dev_info_t *dip)
{
	caddr_t regs;
	ddi_acc_handle_t hdl;
	int error;

	static ddi_device_acc_attr_t regs_attr = {
		DDI_DEVICE_ATTR_V0,
		DDI_NEVERSWAP_ACC,
		DDI_STRICTORDER_ACC,
	};

	error = ddi_regs_map_setup(dip, 0, &regs, 0, PAGE_SIZE, &regs_attr,
	    &hdl);

	if (error != DDI_SUCCESS)
		return (NULL);

	ddi_set_driver_private(dip, hdl);

	return (regs);
}

static void
vtd_unmap(dev_info_t *dip)
{
	ddi_acc_handle_t hdl = ddi_get_driver_private(dip);

	if (hdl != NULL)
		ddi_regs_map_free(&hdl);
}

static dev_info_t *
vtd_get_dip(ACPI_DMAR_HARDWARE_UNIT *drhd, int unit)
{
	dev_info_t *dip;
	struct ddi_parent_private_data *pdptr;
	struct regspec reg;

	/*
	 * Try to find an existing devinfo node for this vtd unit.
	 */
	ndi_devi_enter(ddi_root_node());
	dip = ddi_find_devinfo("vtd", unit, 0);
	ndi_devi_exit(ddi_root_node());

	if (dip != NULL)
		return (dip);

	/*
	 * None found, construct a devinfo node for this vtd unit.
	 */
	dip = ddi_add_child(ddi_root_node(), "vtd",
	    DEVI_SID_NODEID, unit);

	reg.regspec_bustype = 0;
	reg.regspec_addr = drhd->Address;
	reg.regspec_size = PAGE_SIZE;

	/*
	 * update the reg properties
	 *
	 *   reg property will be used for register
	 *   set access
	 *
	 * refer to the bus_map of root nexus driver
	 * I/O or memory mapping:
	 *
	 * <bustype=0, addr=x, len=x>: memory
	 * <bustype=1, addr=x, len=x>: i/o
	 * <bustype>1, addr=0, len=x>: x86-compatibility i/o
	 */
	(void) ndi_prop_update_int_array(DDI_DEV_T_NONE,
	    dip, "reg", (int *)&reg,
	    sizeof (struct regspec) / sizeof (int));

	/*
	 * This is an artificially constructed dev_info, and we
	 * need to set a few more things to be able to use it
	 * for ddi_dma_alloc_handle/free_handle.
	 */
	ddi_set_driver(dip, ddi_get_driver(ddi_root_node()));
	DEVI(dip)->devi_bus_dma_allochdl =
	    DEVI(ddi_get_driver((ddi_root_node())));

	pdptr = kmem_zalloc(sizeof (struct ddi_parent_private_data)
	    + sizeof (struct regspec), KM_SLEEP);
	pdptr->par_nreg = 1;
	pdptr->par_reg = (struct regspec *)(pdptr + 1);
	pdptr->par_reg->regspec_bustype = 0;
	pdptr->par_reg->regspec_addr = drhd->Address;
	pdptr->par_reg->regspec_size = PAGE_SIZE;
	ddi_set_parent_data(dip, pdptr);

	return (dip);
}

static int
vtd_init(void)
{
	int i, units, remaining, tmp;
	struct vtdmap *vtdmap;
	vm_paddr_t ctx_paddr;
	char *end;
#ifdef __FreeBSD__
	char envname[32];
	unsigned long mapaddr;
#endif
	ACPI_STATUS status;
	ACPI_TABLE_DMAR *dmar;
	ACPI_DMAR_HEADER *hdr;
	ACPI_DMAR_HARDWARE_UNIT *drhd;

#ifdef __FreeBSD__
	/*
	 * Allow the user to override the ACPI DMAR table by specifying the
	 * physical address of each remapping unit.
	 *
	 * The following example specifies two remapping units at
	 * physical addresses 0xfed90000 and 0xfeda0000 respectively.
	 * set vtd.regmap.0.addr=0xfed90000
	 * set vtd.regmap.1.addr=0xfeda0000
	 */
	for (units = 0; units < DRHD_MAX_UNITS; units++) {
		snprintf(envname, sizeof (envname), "vtd.regmap.%d.addr",
		    units);
		if (getenv_ulong(envname, &mapaddr) == 0)
			break;
		vtdmaps[units] = (struct vtdmap *)PHYS_TO_DMAP(mapaddr);
	}

	if (units > 0)
		goto skip_dmar;
#else
	units = 0;
#endif
	/* Search for DMAR table. */
	status = AcpiGetTable(ACPI_SIG_DMAR, 0, (ACPI_TABLE_HEADER **)&dmar);
	if (ACPI_FAILURE(status))
		return (ENXIO);

	end = (char *)dmar + dmar->Header.Length;
	remaining = dmar->Header.Length - sizeof (ACPI_TABLE_DMAR);
	while (remaining > sizeof (ACPI_DMAR_HEADER)) {
		hdr = (ACPI_DMAR_HEADER *)(end - remaining);
		if (hdr->Length > remaining)
			break;
		/*
		 * From Intel VT-d arch spec, version 1.3:
		 * BIOS implementations must report mapping structures
		 * in numerical order, i.e. All remapping structures of
		 * type 0 (DRHD) enumerated before remapping structures of
		 * type 1 (RMRR) and so forth.
		 */
		if (hdr->Type != ACPI_DMAR_TYPE_HARDWARE_UNIT)
			break;

		drhd = (ACPI_DMAR_HARDWARE_UNIT *)hdr;
		drhds[units] = drhd;
#ifdef __FreeBSD__
		vtdmaps[units] = (struct vtdmap *)PHYS_TO_DMAP(drhd->Address);
#else
		vtddips[units] = vtd_get_dip(drhd, units);
		vtdmaps[units] = (struct vtdmap *)vtd_map(vtddips[units]);
		if (vtdmaps[units] == NULL)
			goto fail;
#endif
		if (++units >= DRHD_MAX_UNITS)
			break;
		remaining -= hdr->Length;
	}

	if (units <= 0)
		return (ENXIO);

#ifdef __FreeBSD__
skip_dmar:
#endif
	drhd_num = units;
	for (i = 0; i < DRHD_MAX_UNITS; i++) {
		mutex_init(&vtd_invalidate_locks[i], NULL, MUTEX_DRIVER, NULL);
		vtd_invalidate_lock_contention[i] = 0;
	}
	vtd_timeout_dumped_mask = 0;
	vtd_bringup_dumped_te_mask = 0;
	vtd_bringup_dumped_hostadd_mask = 0;
	vtd_ctxcmd_first_dumped_mask = 0;
	vtd_iotlbcmd_first_dumped_mask = 0;
	vtd_ctxcmd_prebusy_dumped_mask = 0;
	vtd_iotlbcmd_prebusy_dumped_mask = 0;
	vtd_init_stage_rta_mask = 0;
	vtd_init_stage_srtp_mask = 0;
	vtd_init_stage_te_mask = 0;
	vtd_init_order_warned_mask = 0;
	vtd_host_domain_bootstrap_active = B_TRUE;
	vtd_invalidate_locks_init = B_TRUE;

	max_domains = 64 * 1024; /* maximum valid value */
	for (i = 0; i < drhd_num; i++) {
		if (!vtd_drhd_enabled(i))
			continue;
		vtdmap = vtdmaps[i];

		if (VTD_CAP_CM(vtdmap->cap) != 0)
			panic("vtd_init: invalid caching mode");

		/* take most compatible (minimum) value */
		if ((tmp = vtd_max_domains(vtdmap)) < max_domains)
			max_domains = tmp;
			
	}

	/*
	 * Set up the root-table to point to the context-entry tables
	 */
	for (i = 0; i < 256; i++) {
		ctx_paddr = vtophys(ctx_tables[i]);
		if (ctx_paddr & PAGE_MASK)
			panic("ctx table (0x%0lx) not page aligned", ctx_paddr);

		root_table[i * 2] = ctx_paddr | VTD_ROOT_PRESENT;
	}

	cmn_err(CE_NOTE, "vtd_init: found %d DRHD units", units);
	cmn_err(CE_NOTE, "vtd_init: max_domains=%d", max_domains);
	
	return (0);

#ifndef __FreeBSD__
fail:
	for (i = 0; i <= units; i++)
		vtd_unmap(vtddips[i]);
	return (ENXIO);
#endif
}

static void
vtd_cleanup(void)
{
#ifndef __FreeBSD__
	int i;

	KASSERT(SLIST_EMPTY(&domhead), ("domain list not empty"));

	bzero(root_table, sizeof (root_table));
	for (i = 0; i < drhd_num; i++) {
		vtdmaps[i] = NULL;
		/*
		 * Unmap the vtd registers. Note that the devinfo nodes
		 * themselves aren't removed, they are considered system state
		 * and can be reused when the module is reloaded.
		 */
		if (vtddips[i] != NULL)
			vtd_unmap(vtddips[i]);
	}
	if (vtd_invalidate_locks_init) {
		for (i = 0; i < DRHD_MAX_UNITS; i++) {
			mutex_destroy(&vtd_invalidate_locks[i]);
		}
		vtd_invalidate_locks_init = B_FALSE;
	}
	vtd_host_domain_bootstrap_active = B_FALSE;
#endif
}

static boolean_t
vtd_ir_unit_ok(int unit, struct vtdmap *vtdmap)
{
	if (vtd_manage_ire == 0)
		return (B_FALSE);

	/*
	 * Temporary policy function in a real tree you'd probably make
	 * this check for quirks, errata, or even a tunable to skip/force
	 * certain units.
	 *
	 * For example, you might blacklist DRHD0 explicitly if you know it
	 * storms when IR is enabled.
	 */
	if (!vtd_drhd_enabled(unit)) {
		/* Explicitly skipped by policy/tunable */
		return (B_FALSE);
	}

	/* Require that ECAP advertises IR support */
	if (VTD_ECAP_IR(vtdmap->ext_cap) == 0)
		return (B_FALSE);

	return (B_TRUE);
}

static void
vtd_enable(void)
{
	int i;

	cmn_err(CE_NOTE, "vtd_enable: enabling VT-d units");

	/*
	 * Coordinate with host interrupt-remap programming across the full VT-d
	 * enable phase, not just individual unit bring-up, to reduce exposure to
	 * cross-DRHD IRTE updates while units are transitioning.
	 */
	for (i = 0; i < drhd_num; i++) {
		if (!vtd_drhd_enabled(i))
			continue;
		immu_intrmap_drhd_transition_set(i, B_TRUE);
	}

	for (i = 0; i < drhd_num; i++) {
		if (!vtd_drhd_enabled(i))
			continue;
		if (!vtd_ensure_unit_enabled(vtdmaps[i])) {
			cmn_err(CE_WARN, "vtd_enable: failed to initialize DRHD %d",
				    i);
		}
	}

	for (i = 0; i < drhd_num; i++) {
		if (!vtd_drhd_enabled(i))
			continue;
		immu_intrmap_drhd_transition_set(i, B_FALSE);
	}

	vtd_host_domain_bootstrap_active = B_FALSE;
}

// static void
// vtd_enable(void)
// {
// 	int i;
// 	struct vtdmap *vtdmap;
// 	cmn_err(CE_NOTE, "vtd_enable: IN ENABLE!");
// 	//HACKING!!!!
// 	for (i = 1; i < drhd_num; i++) {
// 		vtdmap = vtdmaps[i];
// 		vtd_wbflush(vtdmap);
// 
// 		/* Update the root table address */
// 		vtdmap->rta = vtophys(root_table);
// 		vtdmap->gcr = VTD_GCR_SRTP;
// 		while ((vtdmap->gsr & VTD_GSR_RTPS) == 0)
// 			;
// 
// 		vtd_ctx_global_invalidate(vtdmap);
// 		vtd_iotlb_global_invalidate(vtdmap);
// 
// 		vtd_translation_enable(vtdmap);
// 	
// 		cmn_err(CE_NOTE, "vtd_enable: DRHD %d TE bit now %s (GSR=0x%x)",
// 			i,
// 			(vtdmap->gsr & VTD_GSR_TES) ? "ENABLED" : "DISABLED",
// 			vtdmap->gsr);
// 	}
// 
// 	
// }

static void
vtd_disable(void)
{
	int i;
	struct vtdmap *vtdmap;
	for (i = 0; i < drhd_num; i++) {
		if (!vtd_drhd_enabled(i))
			continue;
		vtdmap = vtdmaps[i];
		vtd_translation_disable(vtdmap);
	}
}

/*
 * Program a VT-d context table entry to associate a PCI device RID
 * with a domain (guest or host). Each RID (bus:devfn) corresponds
 * to two 64-bit context entry slots (low + high).
 */
// static void
// vtd_add_device(void *arg, uint16_t rid)
// {
// 	int idx;
// 	uint64_t *ctxp;
// 	struct domain *dom = arg;
// 	vm_paddr_t pt_paddr;
// 	struct vtdmap *vtdmap;
// 	uint8_t bus;
// 
// 	cmn_err(CE_NOTE, "VT-d: entering vtd_add_device for RID=0x%x (bus=%u dev=%u fn=%u) domid=%u",
// 	    rid,
// 	    PCI_RID2BUS(rid), PCI_RID2DEV(rid), PCI_RID2FUNC(rid),
// 	    dom->id);
// 
// 	bus = PCI_RID2BUS(rid);
// 
// 	/* Base pointer for the context-table this bus maps to */
// 	ctxp = ctx_tables[bus];
// 
// 	/* Physical addr of root of the domain’s page table */
// 	pt_paddr = vtophys(dom->ptp);
// 
// 	/* Index into context table (two entries per RID) */
// 	idx = VTD_RID2IDX(rid);
// 
// 	cmn_err(CE_NOTE, "VT-d: domid=%u ptp=%p ptp_phys=0x%llx addrwidth=%d pt_levels=%d",
// 	    dom->id, dom->ptp,
// 	    (unsigned long long)pt_paddr,
// 	    dom->addrwidth, dom->pt_levels);
// 
// 	/* Sanity check: device RID already mapped? */
// 	if (ctxp[idx] & VTD_CTX_PRESENT) {
// 		panic("vtd_add_device: device RID=0x%x already owned by domain %d",
// 		    rid, (uint16_t)(ctxp[idx + 1] >> 8));
// 	}
// 
// 	/* Which DRHD controls this rid? */
// 	if ((vtdmap = vtd_device_scope(rid)) == NULL)
// 		panic("vtd_add_device: RID=0x%x not in scope of any DRHD", rid);
// 
// 	/*
// 	 * Order is important:
// 	 *  hi dword: Domain ID, AGAW bits
// 	 *  lo dword: root page table addr + TT bits + PRESENT
// 	 */
// 	ctxp[idx + 1] = (uint64_t)dom->addrwidth | ((uint64_t)dom->id << 8);
// 
// 	if (VTD_ECAP_DI(vtdmap->ext_cap))
// 		ctxp[idx] = VTD_CTX_TT_ALL;   /* DMA translates all addr spaces */
// 	else
// 		ctxp[idx] = 0;
// 
// 	ctxp[idx] |= pt_paddr | VTD_CTX_PRESENT;
// 
// 	cmn_err(CE_NOTE,
// 	    "VT-d: wrote CTX[%d] for RID=0x%x: LO=0x%llx HI=0x%llx (domid=%u)",
// 	    idx, rid,
// 	    (unsigned long long)ctxp[idx],
// 	    (unsigned long long)ctxp[idx + 1],
// 	    dom->id);
// 
// 	/*
// 	 * Normally: must flush context cache + IOTLB for this domain
// 	 * so that hardware uses updated entries.
// 	 */
// 	// vtd_invalidate_context(vtdmap, rid, dom->id);
// 	// vtd_invalidate_iotlb(vtdmap, dom->id);
// 
// 	cmn_err(CE_NOTE, "VT-d: completed vtd_add_device for RID=0x%x on DRHD=%p",
// 	    rid, (void *)vtdmap);
// }

static void
vtd_add_device(void *arg, uint16_t rid)
{
	int idx;
	uint64_t *ctxp;
	struct domain *dom = arg;
	vm_paddr_t pt_paddr;
	struct vtdmap *vtdmap;
	uint8_t bus;
	uint8_t slot;
	uint8_t func;
	int drhd_idx = -1;

	bus = PCI_RID2BUS(rid);
	slot = PCI_RID2SLOT(rid);
	func = PCI_RID2FUNC(rid);
	if (vtd_trace_lifecycle != 0 && vtd_trace_domain_enabled(dom)) {
		cmn_err(CE_NOTE,
		    "vtd_add_device: entering rid=0x%x bdf=%02x:%02x.%x domid=%u",
		    rid, bus, slot, func, dom->id);
	}
	ctxp = ctx_tables[bus];
	pt_paddr = vtophys(dom->ptp);
	idx = VTD_RID2IDX(rid);

	if (vtd_trace_lifecycle != 0 && vtd_trace_domain_enabled(dom)) {
		cmn_err(CE_NOTE,
		    "vtd_add_device: dom->ptp=%p pt_paddr=0x%llx addrwidth=%d idx=%d",
		    dom->ptp, (unsigned long long)pt_paddr, dom->addrwidth, idx);
	}

	if (ctxp[idx] & VTD_CTX_PRESENT) {
		cmn_err(CE_WARN, "vtd_add_device: RID=0x%x already owned by domain %d",
		    rid, (uint16_t)(ctxp[idx + 1] >> 8));
		return;
	}

	/*
	 * Which DRHD controls this RID?
	 * This trace is the key: it tells you the DRHD index and hardware base.
	 */
	if ((vtdmap = vtd_device_scope(rid)) == NULL)
	{
		cmn_err(CE_WARN, "vtd_add_device: RID=0x%x not in scope for any enabled DRHD (mask=0x%x)",
		    rid, vtd_drhd_ignore_mask);
		return;
	}

	/* --- Diagnostic trace for DRHD correlation --- */
	for (int i = 0; i < drhd_num; i++) {
		if (!vtd_drhd_enabled(i))
			continue;
		if (vtdmaps[i] == vtdmap) {
			drhd_idx = i;
			if (vtd_trace_lifecycle != 0 &&
			    vtd_trace_domain_enabled(dom)) {
				cmn_err(CE_NOTE,
				    "vtd_add_device: RID=0x%x handled by DRHD %d @%p (ECAP.IR=%u)",
				    rid, i, (void *)vtdmap,
				    (uint_t)VTD_ECAP_IR(vtdmap->ext_cap));
			}
			break;
		}
	}

	if (dom->id == VTD_HOST_DOMAIN_ID && !vtd_unit_init_complete(vtdmap)) {
		/*
		 * During initial host-domain seeding, defer TE/invalidate until
		 * iommu_init() has walked the full PCI tree and calls vtd_enable().
		 */
		if (vtd_host_domain_bootstrap_active) {
			vtd_check_host_add_init_order(vtdmap, rid);
		} else if (!vtd_ensure_unit_enabled(vtdmap)) {
			cmn_err(CE_WARN, "vtd_add_device: host-domain ensure failed "
			    "for rid=0x%x domid=%u", rid, dom->id);
			return;
		}
	}

	/*
	 * Normal context‑entry programming.
	 */
	ctxp[idx + 1] = ((uint64_t)dom->id << 8) | (dom->addrwidth & 0x7);

	if (VTD_ECAP_DI(vtdmap->ext_cap))
		ctxp[idx] = VTD_CTX_TT_ALL;
	else
		ctxp[idx] = 0;

	ctxp[idx] |= pt_paddr | VTD_CTX_PRESENT;

	if (vtd_trace_lifecycle != 0 && vtd_trace_domain_enabled(dom)) {
		cmn_err(CE_NOTE, "vtd_add_device: wrote CTX[%d]: LO=0x%llx HI=0x%llx",
		    idx,
		    (unsigned long long)ctxp[idx],
		    (unsigned long long)ctxp[idx + 1]);
	}

	/*
	 * Refresh context-cache and IOTLB for the controlling DRHD so the
	 * updated DID/PT root is observed immediately after domain handoff.
	 */
	if (dom->id != VTD_HOST_DOMAIN_ID) {
		/*
		 * Passthru/device-assignment domains require strict coherency;
		 * do not carry forward degraded skip state from host-domain
		 * attach activity.
		 */
		vtd_invalidate_clear_skip(vtdmap);
	}

	if (dom->id == VTD_HOST_DOMAIN_ID) {
		vtd_check_host_add_init_order(vtdmap, rid);
		vtd_dump_bringup_state_once(vtdmap,
		    &vtd_bringup_dumped_hostadd_mask,
		    "host-add-pre-invalidate");
	}

	if (dom->id == VTD_HOST_DOMAIN_ID && vtd_host_domain_bootstrap_active) {
		if (vtd_trace_lifecycle != 0 && vtd_trace_domain_enabled(dom)) {
			cmn_err(CE_NOTE, "vtd_add_device: deferring host invalidate "
			    "until enable rid=0x%x drhd=%d", rid, drhd_idx);
		}
		goto out;
	}

	if (!vtd_context_changed_invalidate(vtdmap)) {
		if (vtd_rearm_on_remove_timeout != 0 &&
		    dom->id != VTD_HOST_DOMAIN_ID &&
		    vtd_rearm_unit_and_invalidate(vtdmap)) {
			cmn_err(CE_NOTE, "vtd_add_device: invalidate recovered "
			    "after DRHD rearm rid=0x%x domid=%u", rid, dom->id);
		} else {
			cmn_err(CE_WARN, "vtd_add_device: invalidate timed out "
			    "for rid=0x%x domid=%u", rid, dom->id);
		}
	}

out:
	if (vtd_trace_lifecycle != 0 && vtd_trace_domain_enabled(dom)) {
		cmn_err(CE_NOTE,
		    "vtd_add_device: completed rid=0x%x drhd=%d domid=%u",
		    rid, drhd_idx, dom->id);
	}
}

// THIS WORKS GOOD!!!
// static void
// vtd_add_device(void *arg, uint16_t rid)
// {
// 	int idx;
// 	uint64_t *ctxp;
// 	struct domain *dom = arg;
// 	vm_paddr_t pt_paddr;
// 	struct vtdmap *vtdmap;
// 	uint8_t bus;
// 
// 	cmn_err(CE_NOTE, "vtd_add_device: entering for rid=0x%x domid=%u", 
// 		rid, dom->id);
// 
// 	bus = PCI_RID2BUS(rid);
// 	ctxp = ctx_tables[bus];
// 	pt_paddr = vtophys(dom->ptp);
// 	idx = VTD_RID2IDX(rid);
// 
// 	cmn_err(CE_NOTE, "vtd_add_device: dom->ptp=%p pt_paddr=0x%llx addrwidth=%d",
// 		dom->ptp, (unsigned long long)pt_paddr, dom->addrwidth);
// 
// 	if (ctxp[idx] & VTD_CTX_PRESENT) {
// 		panic("vtd_add_device: device %x is already owned by "
// 		    "domain %d", rid, (uint16_t)(ctxp[idx + 1] >> 8));
// 	}
// 
// 	if ((vtdmap = vtd_device_scope(rid)) == NULL)
// 		panic("vtd_add_device: device %x is not in scope for "
// 		    "any DMA remapping unit", rid);
// 
// 	/*
// 	 * Order is important. The 'present' bit is set only after all fields
// 	 * of the context pointer are initialized.
// 	 */
// 	ctxp[idx + 1] = dom->addrwidth | (dom->id << 8);
// 
// 	if (VTD_ECAP_DI(vtdmap->ext_cap))
// 		ctxp[idx] = VTD_CTX_TT_ALL;
// 	else
// 		ctxp[idx] = 0;
// 
// 	ctxp[idx] |= pt_paddr | VTD_CTX_PRESENT;
// 
// 	cmn_err(CE_NOTE, "vtd_add_device: wrote ctx[%d]: low=0x%llx high=0x%llx",
// 			idx,
// 			(unsigned long long)ctxp[idx],
// 			(unsigned long long)ctxp[idx+1]);
// 
// 	cmn_err(CE_NOTE, "vtd_add_device: completed for rid=0x%x", rid);
// 	/*
// 	 * 'Not Present' entries are not cached in either the Context Cache
// 	 * or in the IOTLB, so there is no need to invalidate either of them.
// 	 */
// }

static void
vtd_remove_device(void *arg, uint16_t rid)
{
	int idx;
	uint64_t *ctxp;
	uint8_t bus;
	struct domain *dom = arg;
	struct vtdmap *vtdmap;
	boolean_t ok;

	bus = PCI_RID2BUS(rid);
	ctxp = ctx_tables[bus];
	idx = VTD_RID2IDX(rid);
	vtdmap = vtd_device_scope(rid);

	if (vtd_trace_lifecycle != 0 && vtd_trace_domain_enabled(dom)) {
		cmn_err(CE_NOTE, "vtd_remove_device: rid=0x%x domid=%u",
		    rid, dom != NULL ? dom->id : 0);
	}
	if (vtd_trace_remove_state != 0 && vtd_trace_domain_enabled(dom)) {
		cmn_err(CE_NOTE, "vtd_remove_state: phase=before-clear "
		    "rid=0x%x domid=%u drhd=%d ctxlo=0x%llx ctxhi=0x%llx "
		    "gsr=0x%x ccr=0x%llx rta=0x%llx",
		    rid, dom != NULL ? dom->id : 0,
		    vtdmap != NULL ? vtd_drhd_index(vtdmap) : -1,
		    (unsigned long long)ctxp[idx],
		    (unsigned long long)ctxp[idx + 1],
		    vtdmap != NULL ? vtdmap->gsr : 0,
		    (unsigned long long)(vtdmap != NULL ? vtdmap->ccr : 0),
		    (unsigned long long)(vtdmap != NULL ? vtdmap->rta : 0));
	}

	/*
	 * Order is important. The 'present' bit is must be cleared first.
	 */
	ctxp[idx] = 0;
	ctxp[idx + 1] = 0;
	if (vtd_trace_remove_state != 0 && vtd_trace_domain_enabled(dom)) {
		cmn_err(CE_NOTE, "vtd_remove_state: phase=after-clear "
		    "rid=0x%x domid=%u drhd=%d ctxlo=0x%llx ctxhi=0x%llx "
		    "gsr=0x%x ccr=0x%llx rta=0x%llx",
		    rid, dom != NULL ? dom->id : 0,
		    vtdmap != NULL ? vtd_drhd_index(vtdmap) : -1,
		    (unsigned long long)ctxp[idx],
		    (unsigned long long)ctxp[idx + 1],
		    vtdmap != NULL ? vtdmap->gsr : 0,
		    (unsigned long long)(vtdmap != NULL ? vtdmap->ccr : 0),
		    (unsigned long long)(vtdmap != NULL ? vtdmap->rta : 0));
	}

	/* Keep remove/add invalidation policy in a single helper. */
	ok = vtd_context_changed_invalidate(NULL);
	if (!ok) {
		cmn_err(CE_WARN, "vtd_remove_device: invalidate timed out for "
		    "rid=0x%x", rid);
		if (vtd_rearm_on_remove_timeout != 0 && dom != NULL &&
		    dom->id != VTD_HOST_DOMAIN_ID &&
		    vtdmap != NULL) {
			if (vtd_rearm_unit_and_invalidate(vtdmap)) {
				cmn_err(CE_NOTE, "vtd_remove_device: recovered "
				    "remove invalidate after DRHD rearm "
				    "rid=0x%x domid=%u", rid, dom->id);
			} else {
				cmn_err(CE_WARN, "vtd_remove_device: DRHD "
				    "rearm did not recover remove invalidate "
				    "rid=0x%x domid=%u", rid, dom->id);
			}
		}
	}
	if (vtd_trace_lifecycle != 0 && vtd_trace_domain_enabled(dom)) {
		cmn_err(CE_NOTE, "vtd_remove_device: completed rid=0x%x", rid);
	}
}

#define	CREATE_MAPPING	0
#define	REMOVE_MAPPING	1



/*
* Install or remove an IOMMU mapping for [gpa,hpa,len].
* Tries to use 1G or 2M superpages if allowed and aligned,
* falls back to 4K otherwise.
*
* NOTE: Partial unmap will zap an entire superpage. No demotion yet.
*/
static uint64_t
vtd_update_mapping(void *arg, vm_paddr_t gpa, vm_paddr_t hpa,
				uint64_t len, int remove)
{
	struct domain *dom = arg;
	uint64_t mapped = 0;
	const char *op = remove ? "unmap" : "map";
	boolean_t gate_entered = B_FALSE;

	KASSERT(((gpa | hpa | len) & PAGE_MASK) == 0,
		("%s: unaligned gpa/hpa/len", __func__));
	KASSERT(gpa + len > gpa, ("%s: wraparound gpa", __func__));
	KASSERT(gpa + len <= dom->maxaddr,
		("%s: gpa range %lx/%lx beyond maxaddr %lx", __func__,
		(u_long)gpa, (u_long)len, (u_long)dom->maxaddr));

	if (vtd_trace_map != 0 && vtd_trace_domain_enabled(dom)) {
		cmn_err(CE_NOTE,
		    "vtd_%s: domid=%u gpa=0x%llx hpa=0x%llx len=0x%llx max=0x%llx",
		    op, dom->id, (unsigned long long)gpa, (unsigned long long)hpa,
		    (unsigned long long)len, (unsigned long long)dom->maxaddr);
	}

	if (vtd_intrmap_quiesce_on_mapping_updates != 0 &&
	    vtd_intrmap_quiesce_domain(dom)) {
		vtd_intrmap_quiesce_all(B_TRUE);
		gate_entered = B_TRUE;
	}

	while (len > 0) {
		uint64_t spsize;
		int spshift;

		/* Choose biggest possible page size: 1G -> 2M -> 4K */
		if ((dom->spsmask & 0x2) &&           /* supports 1G */
			(gpa % (1ULL<<30)) == 0 &&
			(hpa % (1ULL<<30)) == 0 &&
			len >= (1ULL<<30)) {
			spshift = 30;
			spsize  = 1ULL << 30;
		} else if ((dom->spsmask & 0x1) &&    /* supports 2M */
			(gpa % (1ULL<<21)) == 0 &&
			(hpa % (1ULL<<21)) == 0 &&
			len >= (1ULL<<21)) {
			spshift = 21;
			spsize  = 1ULL << 21;
		} else {
			spshift = 12;
			spsize  = 1ULL << 12;
		}

		/* Walk down the levels until we’re at or below desired shift */
		uint64_t *ptp = dom->ptp;
		int nlevels   = dom->pt_levels;
		int ptpshift = 0, ptpindex = 0;

		while (--nlevels >= 0) {
			ptpshift = 12 + nlevels * 9;
			ptpindex = (gpa >> ptpshift) & 0x1FF;

			if (spshift >= ptpshift)
				break;

			if (ptp[ptpindex] == 0) {
				void *nlp = vmm_ptp_alloc();
				ptp[ptpindex] = vtophys(nlp) | VTD_PTE_RD | VTD_PTE_WR;
			}
			ptp = (uint64_t *)PHYS_TO_DMAP(ptp[ptpindex] & VTD_PTE_ADDR_M);
		}

		if (remove) {
			ptp[ptpindex] = 0;
		} else {
			uint64_t pte = hpa | VTD_PTE_RD | VTD_PTE_WR;
			if (spshift > 12)
				pte |= VTD_PTE_SUPERPAGE;
			ptp[ptpindex] = pte;
		}

		if (vtd_trace_map_verbose != 0 && vtd_trace_domain_enabled(dom)) {
			cmn_err(CE_NOTE,
			    "vtd_%s: domid=%u chunk=0x%llx shift=%d ptpshift=%d ptidx=%d",
			    op, dom->id, (unsigned long long)spsize, spshift,
			    ptpshift, ptpindex);
		}

		gpa  += spsize;
		hpa  += spsize;
		len  -= spsize;
		mapped += spsize;
	}

	if (vtd_trace_map != 0 && vtd_trace_domain_enabled(dom)) {
		cmn_err(CE_NOTE, "vtd_%s: domid=%u mapped=0x%llx",
		    op, dom->id, (unsigned long long)mapped);
	}

	if (gate_entered)
		vtd_intrmap_quiesce_all(B_FALSE);

	return mapped;
}
static uint64_t
vtd_create_mapping(void *arg, vm_paddr_t gpa, vm_paddr_t hpa, uint64_t len)
{

	return (vtd_update_mapping(arg, gpa, hpa, len, CREATE_MAPPING));
}

static uint64_t
vtd_remove_mapping(void *arg, vm_paddr_t gpa, uint64_t len)
{

	return (vtd_update_mapping(arg, gpa, 0, len, REMOVE_MAPPING));
}

static void
vtd_invalidate_tlb(void *dom)
{
	int i;
	struct vtdmap *vtdmap;
	struct domain *d = dom;
	boolean_t gate_entered = B_FALSE;

	if (vtd_trace_invalidate != 0 && vtd_trace_domain_enabled(d)) {
		cmn_err(CE_NOTE, "vtd_invalidate_tlb: domid=%u",
		    d != NULL ? d->id : 0);
	}

	if (vtd_intrmap_quiesce_domain(d)) {
		vtd_intrmap_quiesce_all(B_TRUE);
		gate_entered = B_TRUE;
	}

	/*
	 * Invalidate the IOTLB.
	 * XXX use domain-selective invalidation for IOTLB
	 */
	for (i = 0; i < drhd_num; i++) {
		if (!vtd_drhd_enabled(i))
			continue;
		vtdmap = vtdmaps[i];
		if (!vtd_iotlb_global_invalidate(vtdmap)) {
			cmn_err(CE_WARN, "vtd_invalidate_tlb: invalidate "
			    "timed out on DRHD %d", i);
			}
	}

	if (gate_entered)
		vtd_intrmap_quiesce_all(B_FALSE);
}

#define VTD_ECAP_DEV_IOTLB(_ecap)	(((_ecap) >> 2) & 0x1)	/* DEV-IOTLB support */
#define VTD_ECAP_MGAW(_ecap)	(((_ecap) >> 16) & 0x7f) /* Max Guest Addr Width */

static void *
vtd_create_domain(vm_paddr_t maxaddr)
{
	struct domain *dom;
	vm_paddr_t addr;
	int i, gaw, agaw, res, pt_levels, addrwidth;
	uint32_t spsmask;

	if (drhd_num <= 0)
		panic("vtd_create_domain: no dma remapping hardware available");

	/*
	* Calculate AGAW.
	* Section 3.4.2 "Adjusted Guest Address Width", Architecture Spec.
	*/
	addr = 0;
	for (gaw = 0; addr < maxaddr; gaw++)
		addr = 1ULL << gaw;

	res = (gaw - 12) % 9;
	if (res == 0)
		agaw = gaw;
	else
		agaw = gaw + 9 - res;

	if (agaw > 64)
		agaw = 64;

	/*
	* Simplest approach: force using computed AGAW and derive page table levels.
	* Page table levels = (agaw - 12)/9 + 1, minimum 2.
	*/
//	pt_levels = ((agaw - 12) / 9) + 1;
//	if (pt_levels < 2)
//		pt_levels = 2;
//	addrwidth = pt_levels - 2;

	/* Force AGAW=39-bit, 3-level page tables */
	agaw = 48;
	pt_levels = 4;
	addrwidth = pt_levels - 2;
	
	dom = kmem_zalloc(sizeof (struct domain), KM_SLEEP);
	dom->pt_levels = pt_levels;
	dom->addrwidth = addrwidth;
	dom->id = domain_id();
	dom->maxaddr = maxaddr;
	dom->ptp = vmm_ptp_alloc();
	if ((uintptr_t)dom->ptp & PAGE_MASK)
		panic("vtd_create_domain: ptp (%p) not page aligned", dom->ptp);

#ifdef __FreeBSD__
#ifdef notyet
	/*
	* XXX superpage mappings for the iommu do not work correctly.
	*
	* By default all physical memory is mapped into the host_domain.
	* ...
	*/
	dom->spsmask = ~0;
	for (i = 0; i < drhd_num; i++) {
		struct vtdmap *vtdmap = vtdmaps[i];
		/* take most compatible value */
		dom->spsmask &= VTD_CAP_SPS(vtdmap->cap);
	}
#endif
#else
	/*
	* On illumos we decidedly do not remove memory mapped to a VM's domain
	* from the host_domain, so we don't have to deal with page demotion and
	* can just use large pages.
	*
	* Since VM memory is currently allocated as 4k pages and mapped into
	* the VM domain page by page, the use of large pages is essentially
	* limited to the host_domain.
	*/
	spsmask = ~0U;
	for (i = 0; i < drhd_num; i++) {
		if (!vtd_drhd_enabled(i))
			continue;
		struct vtdmap *vtdmap = vtdmaps[i];
		uint32_t cap_sps = VTD_CAP_SPS(vtdmap->cap);
		uint64_t ecap    = vtdmap->ext_cap;

		uint_t ir        = VTD_ECAP_IR(ecap);
		uint_t devtlb    = VTD_ECAP_DEV_IOTLB(ecap);
		uint_t mgaw      = VTD_ECAP_MGAW(ecap);

		cmn_err(CE_NOTE,
			"VT-d: DRHD %d CAP.SPS=0x%x ECAP.IR=%u ECAP.DEV-IOTLB=%u ECAP.MGAW=%u",
			i, cap_sps, ir, devtlb, mgaw);

		spsmask &= cap_sps;
	}
	dom->spsmask = spsmask;
#endif

	SLIST_INSERT_HEAD(&domhead, dom, next);
	if (vtd_trace_lifecycle != 0 && vtd_trace_domain_enabled(dom)) {
		cmn_err(CE_NOTE,
		    "VT-d: domain create dom=%p domid=%u levels=%d addrwidth=%d max=0x%llx spsmask=0x%x",
		    (void *)dom, dom->id, dom->pt_levels, dom->addrwidth,
		    (unsigned long long)dom->maxaddr, dom->spsmask);
	}
	return (dom);
}

static void
vtd_free_ptp(uint64_t *ptp, int level)
{
	int i;
	uint64_t *nlp;

	if (level > 1) {
		for (i = 0; i < 512; i++) {
			if ((ptp[i] & (VTD_PTE_RD | VTD_PTE_WR)) == 0)
				continue;
			if ((ptp[i] & VTD_PTE_SUPERPAGE) != 0)
				continue;
			nlp = (uint64_t *)PHYS_TO_DMAP(ptp[i] & VTD_PTE_ADDR_M);
			vtd_free_ptp(nlp, level - 1);
		}
	}

	vmm_ptp_free(ptp);
}

static void
vtd_destroy_domain(void *arg)
{
	struct domain *dom;

	dom = arg;
	if (vtd_trace_lifecycle != 0 && vtd_trace_domain_enabled(dom)) {
		cmn_err(CE_NOTE, "VT-d: domain destroy dom=%p domid=%u",
		    (void *)dom, dom->id);
	}

	SLIST_REMOVE(&domhead, dom, domain, next);
	vtd_free_ptp(dom->ptp, dom->pt_levels);
	kmem_free(dom, sizeof (*dom));
}

const struct iommu_ops vmm_iommu_ops = {
	.init = vtd_init,
	.cleanup = vtd_cleanup,
	.enable = vtd_enable,
	.disable = vtd_disable,
	.create_domain = vtd_create_domain,
	.destroy_domain = vtd_destroy_domain,
	.create_mapping = vtd_create_mapping,
	.remove_mapping = vtd_remove_mapping,
	.add_device = vtd_add_device,
	.remove_device = vtd_remove_device,
	.invalidate_tlb = vtd_invalidate_tlb,
};


static struct modlmisc modlmisc = {
	&mod_miscops,
	"bhyve vmm vtd",
};

static struct modlinkage modlinkage = {
	MODREV_1,
	&modlmisc,
	NULL
};

int
_init(void)
{
	return (mod_install(&modlinkage));
}

int
_fini(void)
{
	return (mod_remove(&modlinkage));
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&modlinkage, modinfop));
}
