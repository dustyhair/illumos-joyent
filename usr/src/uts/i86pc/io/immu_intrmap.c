/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2010, Oracle and/or its affiliates. All rights reserved.
 */

/*
 * Copyright (c) 2009, Intel Corporation.
 * All rights reserved.
 */


#include <sys/apic.h>
#include <vm/hat_i86.h>
#include <sys/sysmacros.h>
#include <sys/smp_impldefs.h>
#include <sys/immu.h>


typedef struct intrmap_private {
	immu_t		*ir_immu;
	dev_info_t	*ir_dip;
	immu_inv_wait_t	ir_inv_wait;
	uint16_t	ir_idx;
	uint32_t	ir_sid_svt_sq;
} intrmap_private_t;

#define	INTRMAP_PRIVATE(intrmap) ((intrmap_private_t *)intrmap)

/* interrupt remapping table entry */
typedef struct intrmap_rte {
	uint64_t	lo;
	uint64_t	hi;
} intrmap_rte_t;

#define	IRTE_HIGH(sid_svt_sq) (sid_svt_sq)
#define	IRTE_LOW(dst, vector, dlm, tm, rh, dm, fpd, p)	\
	    (((uint64_t)(dst) << 32) |  \
	    ((uint64_t)(vector) << 16) | \
	    ((uint64_t)(dlm) << 5) | \
	    ((uint64_t)(tm) << 4) | \
	    ((uint64_t)(rh) << 3) | \
	    ((uint64_t)(dm) << 2) | \
	    ((uint64_t)(fpd) << 1) | \
	    (p))

typedef enum {
	SVT_NO_VERIFY = 0,	/* no verification */
	SVT_ALL_VERIFY,		/* using sid and sq to verify */
	SVT_BUS_VERIFY,		/* verify #startbus and #endbus */
	SVT_RSVD
} intrmap_svt_t;

typedef enum {
	SQ_VERIFY_ALL = 0,	/* verify all 16 bits */
	SQ_VERIFY_IGR_1,	/* ignore bit 3 */
	SQ_VERIFY_IGR_2,	/* ignore bit 2-3 */
	SQ_VERIFY_IGR_3		/* ignore bit 1-3 */
} intrmap_sq_t;

/*
 * S field of the Interrupt Remapping Table Address Register
 * the size of the interrupt remapping table is 1 << (immu_intrmap_irta_s + 1)
 */
static uint_t intrmap_irta_s = INTRMAP_MAX_IRTA_SIZE;

/*
 * If true, arrange to suppress broadcast EOI by setting edge-triggered mode
 * even for level-triggered interrupts in the interrupt-remapping engine.
 * If false, broadcast EOI can still be suppressed if the CPU supports the
 * APIC_SVR_SUPPRESS_BROADCAST_EOI bit.  In both cases, the IOAPIC is still
 * programmed with the correct trigger mode, and pcplusmp must send an EOI
 * to the IOAPIC by writing to the IOAPIC's EOI register to make up for the
 * missing broadcast EOI.
 */
static int intrmap_suppress_brdcst_eoi = 0;

/*
 * Verify the source ID of interrupt requests.  This was part of the earlier
 * APIC-stable passthru line and keeps the IRTE requester-ID check honest for
 * native PCIe endpoints.
 */
static int intrmap_enable_sid_verify = 1;
/*
 * Keep fragile host drivers on the legacy interrupt path until the IR-on
 * passthru regression is narrowed down.
 */
static char *immu_intrmap_exclude_drivers = "xhci,nvme,igb,ahci,e1000g";
static int immu_intrmap_two_phase_irte_update = 1;
static uint32_t immu_intrmap_drhd_transition_mask = 0;
static uint32_t immu_intrmap_drhd_transition_warn_mask = 0;
#define	IMMU_INTRMAP_DRHD_GATE_MAX	32
static kmutex_t immu_intrmap_drhd_gate_lock[IMMU_INTRMAP_DRHD_GATE_MAX];
static kcondvar_t immu_intrmap_drhd_gate_cv[IMMU_INTRMAP_DRHD_GATE_MAX];
static uint32_t immu_intrmap_drhd_gate_active[IMMU_INTRMAP_DRHD_GATE_MAX];
static uint32_t immu_intrmap_drhd_transition_depth[IMMU_INTRMAP_DRHD_GATE_MAX];
static boolean_t immu_intrmap_drhd_gate_inited = B_FALSE;

#define	IMMU_INTRMAP_IRTE_TRACE_RING_SZ	128
typedef struct immu_intrmap_irte_trace {
	uint32_t	it_seq;
	uint32_t	it_idx;
	uint32_t	it_sid;
	uint32_t	it_dst;
	uint16_t	it_type;
	uint16_t	it_count;
	uint8_t		it_unit;
	uint8_t		it_vector;
	uint8_t		it_dlm;
	uint8_t		it_tm;
	uint8_t		it_rh;
	uint8_t		it_dm;
	uint8_t		it_pad[2];
	uintptr_t	it_immu;
	uintptr_t	it_dip;
	uint64_t	it_lo;
	uint64_t	it_hi;
} immu_intrmap_irte_trace_t;

static int immu_intrmap_trace_recent_irte = 0;
static int immu_intrmap_trace_recent_irte_dump = 0;
static int immu_intrmap_trace_recent_irte_dump_count = 16;
static uint32_t immu_intrmap_irte_trace_seq = 0;
static uint32_t immu_intrmap_irte_trace_dumped = 0;
static immu_intrmap_irte_trace_t
    immu_intrmap_irte_trace_ring[IMMU_INTRMAP_IRTE_TRACE_RING_SZ];

/* fault types for DVMA remapping */
static char *immu_dvma_faults[] = {
	"Reserved",
	"The present field in root-entry is Clear",
	"The present field in context-entry is Clear",
	"Hardware detected invalid programming of a context-entry",
	"The DMA request attempted to access an address beyond max support",
	"The Write field in a page-table entry is Clear when DMA write",
	"The Read field in a page-table entry is Clear when DMA read",
	"Access the next level page table resulted in error",
	"Access the root-entry table resulted in error",
	"Access the context-entry table resulted in error",
	"Reserved field not initialized to zero in a present root-entry",
	"Reserved field not initialized to zero in a present context-entry",
	"Reserved field not initialized to zero in a present page-table entry",
	"DMA blocked due to the Translation Type field in context-entry",
	"Incorrect fault event reason number",
};
#define	DVMA_MAX_FAULTS (sizeof (immu_dvma_faults)/(sizeof (char *))) - 1

/* fault types for interrupt remapping */
static char *immu_intrmap_faults[] = {
	"reserved field set in IRTE",
	"interrupt_index exceed the intr-remap table size",
	"present field in IRTE is clear",
	"hardware access intr-remap table address resulted in error",
	"reserved field set in IRTE, include various conditional",
	"hardware blocked an interrupt request in Compatibility format",
	"remappable interrupt request blocked due to verification failure"
};
#define	INTRMAP_MAX_FAULTS \
	(sizeof (immu_intrmap_faults) / (sizeof (char *))) - 1

/* Function prototypes */
static int immu_intrmap_init(int apic_mode);
static void immu_intrmap_switchon(int suppress_brdcst_eoi);
static void immu_intrmap_alloc(void **intrmap_private_tbl, dev_info_t *dip,
    uint16_t type, int count, uchar_t ioapic_index);
static void immu_intrmap_map(void *intrmap_private, void *intrmap_data,
    uint16_t type, int count);
static void immu_intrmap_free(void **intrmap_privatep);
static void immu_intrmap_rdt(void *intrmap_private, ioapic_rdt_t *irdt);
static void immu_intrmap_msi(void *intrmap_private, msi_regs_t *mregs);
static int immu_intrmap_unit_index(immu_t *target);
static void immu_intrmap_drhd_gate_init(void);
static void immu_intrmap_drhd_map_enter(int unit);
static void immu_intrmap_drhd_map_exit(int unit);
static void immu_intrmap_irte_trace_record(intrmap_private_t *priv, immu_t *immu,
    uint_t unit, uint_t idx, uint16_t type, int count, uchar_t vector, uint_t dlm,
    uint_t tm, uint_t rh, uint_t dm, uint_t dst, uint_t sid, intrmap_rte_t *irte);

static struct apic_intrmap_ops intrmap_ops = {
	immu_intrmap_init,
	immu_intrmap_switchon,
	immu_intrmap_alloc,
	immu_intrmap_map,
	immu_intrmap_free,
	immu_intrmap_rdt,
	immu_intrmap_msi,
};

/* apic mode, APIC/X2APIC */
static int intrmap_apic_mode = LOCAL_APIC;

static void
immu_intrmap_drhd_gate_init(void)
{
	int i;

	if (immu_intrmap_drhd_gate_inited)
		return;

	for (i = 0; i < IMMU_INTRMAP_DRHD_GATE_MAX; i++) {
		mutex_init(&immu_intrmap_drhd_gate_lock[i], NULL, MUTEX_DRIVER,
		    NULL);
		cv_init(&immu_intrmap_drhd_gate_cv[i], NULL, CV_DRIVER, NULL);
		immu_intrmap_drhd_gate_active[i] = 0;
		immu_intrmap_drhd_transition_depth[i] = 0;
	}

	immu_intrmap_drhd_gate_inited = B_TRUE;
}

static void
immu_intrmap_drhd_map_enter(int unit)
{
	if (unit < 0 || unit >= IMMU_INTRMAP_DRHD_GATE_MAX)
		return;

	ASSERT(immu_intrmap_drhd_gate_inited);
	mutex_enter(&immu_intrmap_drhd_gate_lock[unit]);
	immu_intrmap_drhd_gate_active[unit]++;
	mutex_exit(&immu_intrmap_drhd_gate_lock[unit]);
}

static void
immu_intrmap_drhd_map_exit(int unit)
{
	if (unit < 0 || unit >= IMMU_INTRMAP_DRHD_GATE_MAX)
		return;

	ASSERT(immu_intrmap_drhd_gate_inited);
	mutex_enter(&immu_intrmap_drhd_gate_lock[unit]);
	ASSERT(immu_intrmap_drhd_gate_active[unit] != 0);
	if (--immu_intrmap_drhd_gate_active[unit] == 0 &&
	    immu_intrmap_drhd_transition_active(unit)) {
		cv_broadcast(&immu_intrmap_drhd_gate_cv[unit]);
	}
	mutex_exit(&immu_intrmap_drhd_gate_lock[unit]);
}

void
immu_intrmap_drhd_transition_set(int unit, boolean_t enter)
{
	uint32_t bit;
	uint32_t depth;

	if (unit < 0 || unit >= IMMU_INTRMAP_DRHD_GATE_MAX)
		return;

	if (!immu_intrmap_drhd_gate_inited)
		immu_intrmap_drhd_gate_init();

	bit = (1u << unit);
	if (enter) {
		mutex_enter(&immu_intrmap_drhd_gate_lock[unit]);
		depth = ++immu_intrmap_drhd_transition_depth[unit];
		if (depth == 1) {
			atomic_or_32(&immu_intrmap_drhd_transition_mask, bit);
			atomic_and_32(&immu_intrmap_drhd_transition_warn_mask, ~bit);
			while (immu_intrmap_drhd_gate_active[unit] != 0) {
				cv_wait(&immu_intrmap_drhd_gate_cv[unit],
				    &immu_intrmap_drhd_gate_lock[unit]);
			}
		}
		mutex_exit(&immu_intrmap_drhd_gate_lock[unit]);
		cmn_err(CE_NOTE, "immu_intrmap: drhd=%d transition enter depth=%u",
		    unit, depth);
	} else {
		mutex_enter(&immu_intrmap_drhd_gate_lock[unit]);
		if (immu_intrmap_drhd_transition_depth[unit] == 0) {
			depth = 0;
		} else {
			depth = --immu_intrmap_drhd_transition_depth[unit];
			if (depth == 0) {
				atomic_and_32(&immu_intrmap_drhd_transition_mask,
				    ~bit);
				atomic_and_32(&immu_intrmap_drhd_transition_warn_mask,
				    ~bit);
			}
		}
		mutex_exit(&immu_intrmap_drhd_gate_lock[unit]);
		cmn_err(CE_NOTE, "immu_intrmap: drhd=%d transition exit depth=%u",
		    unit, depth);
	}
}

boolean_t
immu_intrmap_drhd_transition_active(int unit)
{
	if (unit < 0 || unit >= IMMU_INTRMAP_DRHD_GATE_MAX)
		return (B_FALSE);

	return ((immu_intrmap_drhd_transition_mask & (1u << unit)) != 0);
}

static void
immu_intrmap_irte_trace_record(intrmap_private_t *priv, immu_t *immu, uint_t unit,
    uint_t idx, uint16_t type, int count, uchar_t vector, uint_t dlm, uint_t tm,
    uint_t rh, uint_t dm, uint_t dst, uint_t sid, intrmap_rte_t *irte)
{
	immu_intrmap_irte_trace_t *ent;
	uint32_t seq;

	if (!immu_intrmap_trace_recent_irte)
		return;

	seq = atomic_inc_32_nv(&immu_intrmap_irte_trace_seq);
	ent = &immu_intrmap_irte_trace_ring[
	    (seq - 1) % IMMU_INTRMAP_IRTE_TRACE_RING_SZ];

	ent->it_idx = idx;
	ent->it_sid = sid;
	ent->it_dst = dst;
	ent->it_type = type;
	ent->it_count = (count < 0) ? 0 : (uint16_t)count;
	ent->it_unit = (unit > 0xffu) ? 0xffu : (uint8_t)unit;
	ent->it_vector = vector;
	ent->it_dlm = (uint8_t)dlm;
	ent->it_tm = (uint8_t)tm;
	ent->it_rh = (uint8_t)rh;
	ent->it_dm = (uint8_t)dm;
	ent->it_immu = (uintptr_t)immu;
	ent->it_dip = (uintptr_t)(priv != NULL ? priv->ir_dip : NULL);
	ent->it_lo = irte->lo;
	ent->it_hi = irte->hi;
	membar_producer();
	ent->it_seq = seq;
}

void
immu_intrmap_debug_dump_recent_irte(const char *reason)
{
	immu_intrmap_irte_trace_t ent;
	uint32_t seq;
	uint32_t start;
	uint32_t end;
	uint_t i;
	uint_t n;
	const char *why = (reason != NULL) ? reason : "<null>";

	if (!immu_intrmap_trace_recent_irte || !immu_intrmap_trace_recent_irte_dump)
		return;

	if (atomic_cas_32(&immu_intrmap_irte_trace_dumped, 0, 1) != 0)
		return;

	seq = immu_intrmap_irte_trace_seq;
	n = immu_intrmap_trace_recent_irte_dump_count;
	if (n == 0 || n > IMMU_INTRMAP_IRTE_TRACE_RING_SZ)
		n = 8;
	if (n > IMMU_INTRMAP_IRTE_TRACE_RING_SZ)
		n = IMMU_INTRMAP_IRTE_TRACE_RING_SZ;

	end = seq;
	start = (end > n) ? (end - n + 1) : 1;

	prom_printf("IMMU IRTE trace dump reason=%s seq=%u start=%u end=%u\n",
	    why, seq, start, end);

	for (i = start; i <= end; i++) {
		ent = immu_intrmap_irte_trace_ring[
		    (i - 1) % IMMU_INTRMAP_IRTE_TRACE_RING_SZ];
		membar_consumer();
		if (ent.it_seq != i)
			continue;
		prom_printf("IMMU IRTE[%u] unit=%u idx=%u sid=0x%x "
		    "bdf=%02x:%02x.%u type=0x%x cnt=%u vec=0x%x dst=0x%x "
		    "tm=%u dm=%u rh=%u dlm=%u lo=0x%llx hi=0x%llx dip=%p\n",
		    ent.it_seq, ent.it_unit, ent.it_idx, ent.it_sid,
		    (ent.it_sid >> 8) & 0xff, (ent.it_sid >> 3) & 0x1f,
		    ent.it_sid & 0x7, ent.it_type, ent.it_count, ent.it_vector,
		    ent.it_dst, ent.it_tm, ent.it_dm, ent.it_rh, ent.it_dlm,
		    (u_longlong_t)ent.it_lo, (u_longlong_t)ent.it_hi,
		    (void *)ent.it_dip);
	}
}


/*
 * helper functions
 */
static uint_t
bitset_find_free(bitset_t *b, uint_t post)
{
	uint_t	i;
	uint_t	cap = bitset_capacity(b);

	if (post == cap)
		post = 0;

	ASSERT(post < cap);

	for (i = post; i < cap; i++) {
		if (!bitset_in_set(b, i))
			return (i);
	}

	for (i = 0; i < post; i++) {
		if (!bitset_in_set(b, i))
			return (i);
	}

	return (INTRMAP_IDX_FULL);	/* no free index */
}

static boolean_t
immu_intrmap_driver_excluded(const char *driver)
{
	const char *p, *end;
	size_t len;

	if (driver == NULL || *driver == '\0' ||
	    immu_intrmap_exclude_drivers == NULL ||
	    *immu_intrmap_exclude_drivers == '\0') {
		return (B_FALSE);
	}

	for (p = immu_intrmap_exclude_drivers; *p != '\0'; p = end) {
		while (*p == ' ' || *p == '\t' || *p == ',')
			p++;
		if (*p == '\0')
			break;

		for (end = p; *end != '\0' && *end != ','; end++)
			continue;

		len = end - p;
		while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t'))
			len--;

		if (len == strlen(driver) && strncmp(p, driver, len) == 0)
			return (B_TRUE);

		if (*end == '\0')
			break;
	}

	return (B_FALSE);
}

static int
immu_intrmap_unit_index(immu_t *target)
{
	immu_t *immu;
	int idx = 0;

	for (immu = list_head(&immu_list); immu != NULL;
	    immu = list_next(&immu_list, immu), idx++) {
		if (immu == target)
			return (idx);
	}

	return (-1);
}

/*
 * helper function to find 'count' contigous free
 * interrupt remapping table entries
 */
static uint_t
bitset_find_multi_free(bitset_t *b, uint_t post, uint_t count)
{
	uint_t  i, j;
	uint_t	cap = bitset_capacity(b);

	if (post == INTRMAP_IDX_FULL) {
		return (INTRMAP_IDX_FULL);
	}

	if (count > cap)
		return (INTRMAP_IDX_FULL);

	ASSERT(post < cap);

	for (i = post; (i + count) <= cap; i++) {
		for (j = 0; j < count; j++) {
			if (bitset_in_set(b, (i + j))) {
				i = i + j;
				break;
			}
			if (j == count - 1)
				return (i);
		}
	}

	for (i = 0; (i < post) && ((i + count) <= cap); i++) {
		for (j = 0; j < count; j++) {
			if (bitset_in_set(b, (i + j))) {
				i = i + j;
				break;
			}
			if (j == count - 1)
				return (i);
		}
	}

	return (INTRMAP_IDX_FULL);		/* no free index */
}

/* alloc one interrupt remapping table entry */
static int
alloc_tbl_entry(intrmap_t *intrmap)
{
	uint32_t idx;

	for (;;) {
		mutex_enter(&intrmap->intrmap_lock);
		idx = intrmap->intrmap_free;
		if (idx != INTRMAP_IDX_FULL) {
			bitset_add(&intrmap->intrmap_map, idx);
			intrmap->intrmap_free =
			    bitset_find_free(&intrmap->intrmap_map, idx + 1);
			mutex_exit(&intrmap->intrmap_lock);
			break;
		}

		/* no free intr entry, use compatible format intr */
		mutex_exit(&intrmap->intrmap_lock);

		if (intrmap_apic_mode != LOCAL_X2APIC) {
			break;
		}

		/*
		 * x2apic mode not allowed compatible
		 * interrupt
		 */
		delay(IMMU_ALLOC_RESOURCE_DELAY);
	}

	return (idx);
}

/* alloc 'cnt' contigous interrupt remapping table entries */
static int
alloc_tbl_multi_entries(intrmap_t *intrmap, uint_t cnt)
{
	uint_t idx, pos, i;

	for (; ; ) {
		mutex_enter(&intrmap->intrmap_lock);
		pos = intrmap->intrmap_free;
		idx = bitset_find_multi_free(&intrmap->intrmap_map, pos, cnt);

		if (idx != INTRMAP_IDX_FULL) {
			if (idx <= pos && pos < (idx + cnt)) {
				intrmap->intrmap_free = bitset_find_free(
				    &intrmap->intrmap_map, idx + cnt);
			}
			for (i = 0; i < cnt; i++) {
				bitset_add(&intrmap->intrmap_map, idx + i);
			}
			mutex_exit(&intrmap->intrmap_lock);
			break;
		}

		mutex_exit(&intrmap->intrmap_lock);

		if (intrmap_apic_mode != LOCAL_X2APIC) {
			break;
		}

		/* x2apic mode not allowed comapitible interrupt */
		delay(IMMU_ALLOC_RESOURCE_DELAY);
	}

	return (idx);
}

/* init interrupt remapping table */
static int
init_unit(immu_t *immu)
{
	intrmap_t	*intrmap;
	size_t		size;

	ddi_dma_attr_t intrmap_dma_attr = {
		DMA_ATTR_V0,
		0U,
		0xffffffffffffffffULL,
		0xffffffffU,
		MMU_PAGESIZE,	/* page aligned */
		0x1,
		0x1,
		0xffffffffU,
		0xffffffffffffffffULL,
		1,
		4,
		0
	};

	ddi_device_acc_attr_t intrmap_acc_attr = {
		DDI_DEVICE_ATTR_V0,
		DDI_NEVERSWAP_ACC,
		DDI_STRICTORDER_ACC
	};

	/* Must support queued invalidation if using interrupt remapping */
	if (!IMMU_ECAP_GET_QI(immu->immu_regs_excap)) {
		cmn_err(CE_WARN,
		    "!init_unit: immu %s has no QI support, cannot enable IR",
		    immu->immu_name ? immu->immu_name : "<noname>");
		return (DDI_FAILURE);
	}

	if (intrmap_apic_mode == LOCAL_X2APIC) {
		if (!IMMU_ECAP_GET_EIM(immu->immu_regs_excap)) {
			cmn_err(CE_WARN,
			    "!init_unit: immu %s x2APIC but no EIM support",
			    immu->immu_name ? immu->immu_name : "<noname>");
			return (DDI_FAILURE);
		}
	}

	if (intrmap_irta_s > INTRMAP_MAX_IRTA_SIZE) {
		cmn_err(CE_WARN,
		    "!init_unit: IRTA size %u > max %u, clamping",
		    intrmap_irta_s, INTRMAP_MAX_IRTA_SIZE);
		intrmap_irta_s = INTRMAP_MAX_IRTA_SIZE;
	}

	intrmap = kmem_zalloc(sizeof (intrmap_t), KM_SLEEP);

	if (ddi_dma_alloc_handle(immu->immu_dip,
	    &intrmap_dma_attr,
	    DDI_DMA_SLEEP,
	    NULL,
	    &(intrmap->intrmap_dma_hdl)) != DDI_SUCCESS) {
		cmn_err(CE_WARN, "!init_unit: DMA handle alloc failed immu %s",
		    immu->immu_name ? immu->immu_name : "<noname>");
		kmem_free(intrmap, sizeof (intrmap_t));
		return (DDI_FAILURE);
	}

	intrmap->intrmap_size = 1 << (intrmap_irta_s + 1);
	size = intrmap->intrmap_size * INTRMAP_RTE_SIZE;
	if (ddi_dma_mem_alloc(intrmap->intrmap_dma_hdl,
	    size,
	    &intrmap_acc_attr,
	    DDI_DMA_CONSISTENT | IOMEM_DATA_UNCACHED,
	    DDI_DMA_SLEEP,
	    NULL,
	    &(intrmap->intrmap_vaddr),
	    &size,
	    &(intrmap->intrmap_acc_hdl)) != DDI_SUCCESS) {
		cmn_err(CE_WARN, "!init_unit: DMA memory alloc failed immu %s",
		    immu->immu_name ? immu->immu_name : "<noname>");
		ddi_dma_free_handle(&(intrmap->intrmap_dma_hdl));
		kmem_free(intrmap, sizeof (intrmap_t));
		return (DDI_FAILURE);
	}

	ASSERT(!((uintptr_t)intrmap->intrmap_vaddr & MMU_PAGEOFFSET));
	bzero(intrmap->intrmap_vaddr, size);
	intrmap->intrmap_paddr = pfn_to_pa(
	    hat_getpfnum(kas.a_hat, intrmap->intrmap_vaddr));

	mutex_init(&(intrmap->intrmap_lock), NULL, MUTEX_DRIVER, NULL);
	bitset_init(&intrmap->intrmap_map);
	bitset_resize(&intrmap->intrmap_map, intrmap->intrmap_size);
	intrmap->intrmap_free = 0;

	immu->immu_intrmap = intrmap;

	cmn_err(CE_CONT, "!init_unit: immu %s IR table initialized, size=%u",
	    immu->immu_name ? immu->immu_name : "<noname>",
	    intrmap->intrmap_size);

	return (DDI_SUCCESS);
}

/* init interrupt remapping table */
// static int
// init_unit(immu_t *immu)
// {
// 	intrmap_t *intrmap;
// 	size_t size;
// 
// 	ddi_dma_attr_t intrmap_dma_attr = {
// 		DMA_ATTR_V0,
// 		0U,
// 		0xffffffffffffffffULL,
// 		0xffffffffU,
// 		MMU_PAGESIZE,	/* page aligned */
// 		0x1,
// 		0x1,
// 		0xffffffffU,
// 		0xffffffffffffffffULL,
// 		1,
// 		4,
// 		0
// 	};
// 
// 	ddi_device_acc_attr_t intrmap_acc_attr = {
// 		DDI_DEVICE_ATTR_V0,
// 		DDI_NEVERSWAP_ACC,
// 		DDI_STRICTORDER_ACC
// 	};
// 
// 	/*
// 	 * Using interrupt remapping implies using the queue
// 	 * invalidation interface. According to Intel,
// 	 * hardware that supports interrupt remapping should
// 	 * also support QI.
// 	 */
// 	ASSERT(IMMU_ECAP_GET_QI(immu->immu_regs_excap));
// 
// 	if (intrmap_apic_mode == LOCAL_X2APIC) {
// 		if (!IMMU_ECAP_GET_EIM(immu->immu_regs_excap)) {
// 			return (DDI_FAILURE);
// 		}
// 	}
// 
// 	if (intrmap_irta_s > INTRMAP_MAX_IRTA_SIZE) {
// 		intrmap_irta_s = INTRMAP_MAX_IRTA_SIZE;
// 	}
// 
// 	intrmap =  kmem_zalloc(sizeof (intrmap_t), KM_SLEEP);
// 
// 	if (ddi_dma_alloc_handle(immu->immu_dip,
// 	    &intrmap_dma_attr,
// 	    DDI_DMA_SLEEP,
// 	    NULL,
// 	    &(intrmap->intrmap_dma_hdl)) != DDI_SUCCESS) {
// 		kmem_free(intrmap, sizeof (intrmap_t));
// 		return (DDI_FAILURE);
// 	}
// 
// 	intrmap->intrmap_size = 1 << (intrmap_irta_s + 1);
// 	size = intrmap->intrmap_size * INTRMAP_RTE_SIZE;
// 	if (ddi_dma_mem_alloc(intrmap->intrmap_dma_hdl,
// 	    size,
// 	    &intrmap_acc_attr,
// 	    DDI_DMA_CONSISTENT | IOMEM_DATA_UNCACHED,
// 	    DDI_DMA_SLEEP,
// 	    NULL,
// 	    &(intrmap->intrmap_vaddr),
// 	    &size,
// 	    &(intrmap->intrmap_acc_hdl)) != DDI_SUCCESS) {
// 		ddi_dma_free_handle(&(intrmap->intrmap_dma_hdl));
// 		kmem_free(intrmap, sizeof (intrmap_t));
// 		return (DDI_FAILURE);
// 	}
// 
// 	ASSERT(!((uintptr_t)intrmap->intrmap_vaddr & MMU_PAGEOFFSET));
// 	bzero(intrmap->intrmap_vaddr, size);
// 	intrmap->intrmap_paddr = pfn_to_pa(
// 	    hat_getpfnum(kas.a_hat, intrmap->intrmap_vaddr));
// 
// 	mutex_init(&(intrmap->intrmap_lock), NULL, MUTEX_DRIVER, NULL);
// 	bitset_init(&intrmap->intrmap_map);
// 	bitset_resize(&intrmap->intrmap_map, intrmap->intrmap_size);
// 	intrmap->intrmap_free = 0;
// 
// 	immu->immu_intrmap = intrmap;
// 
// 	return (DDI_SUCCESS);
// }

static immu_t *
get_immu(dev_info_t *dip, uint16_t type, uchar_t ioapic_index)
{
	immu_t	*immu = NULL;

	if (!DDI_INTR_IS_MSI_OR_MSIX(type)) {
		immu = immu_dmar_ioapic_immu(ioapic_index);
	} else {
		if (dip != NULL)
			immu = immu_dmar_get_immu(dip);
	}

	return (immu);
}

static int
get_top_pcibridge(dev_info_t *dip, void *arg)
{
	dev_info_t **topdipp = arg;
	immu_devi_t *immu_devi;

	mutex_enter(&(DEVI(dip)->devi_lock));
	immu_devi = DEVI(dip)->devi_iommu;
	mutex_exit(&(DEVI(dip)->devi_lock));

	if (immu_devi == NULL || immu_devi->imd_pcib_type == IMMU_PCIB_BAD ||
	    immu_devi->imd_pcib_type == IMMU_PCIB_ENDPOINT) {
		return (DDI_WALK_CONTINUE);
	}

	*topdipp = dip;

	return (DDI_WALK_CONTINUE);
}

static dev_info_t *
intrmap_top_pcibridge(dev_info_t *rdip)
{
	dev_info_t *top_pcibridge = NULL;

	if (immu_walk_ancestor(rdip, NULL, get_top_pcibridge,
	    &top_pcibridge, NULL, 0) != DDI_SUCCESS) {
		return (NULL);
	}

	return (top_pcibridge);
}

/* function to get interrupt request source id */
static uint32_t
get_sid(dev_info_t *dip, uint16_t type, uchar_t ioapic_index)
{
	dev_info_t	*pdip;
	immu_devi_t	*immu_devi;
	uint16_t	sid;
	uchar_t		svt, sq;

	if (!intrmap_enable_sid_verify) {
		return (0);
	}

	if (!DDI_INTR_IS_MSI_OR_MSIX(type)) {
		/* for interrupt through I/O APIC */
		sid = immu_dmar_ioapic_sid(ioapic_index);
		svt = SVT_ALL_VERIFY;
		sq = SQ_VERIFY_ALL;
	} else {
		/* MSI/MSI-X interrupt */
		ASSERT(dip);
		pdip = intrmap_top_pcibridge(dip);
		ASSERT(pdip);
		immu_devi = DEVI(pdip)->devi_iommu;
		ASSERT(immu_devi);
		if (immu_devi->imd_pcib_type == IMMU_PCIB_PCIE_PCI) {
			/* device behind pcie to pci bridge */
			sid = (immu_devi->imd_bus << 8) | immu_devi->imd_sec;
			svt = SVT_BUS_VERIFY;
			sq = SQ_VERIFY_ALL;
		} else {
			/* pcie device or device behind pci to pci bridge */
			sid = (immu_devi->imd_bus << 8) |
			    immu_devi->imd_devfunc;
			svt = SVT_ALL_VERIFY;
			sq = SQ_VERIFY_ALL;
		}
	}

	return (sid | (svt << 18) | (sq << 16));
}

static void
intrmap_enable(immu_t *immu)
{
	intrmap_t *intrmap;
	uint64_t irta_reg;

	intrmap = immu->immu_intrmap;

	irta_reg = intrmap->intrmap_paddr | intrmap_irta_s;
	if (intrmap_apic_mode == LOCAL_X2APIC) {
		irta_reg |= (0x1 << 11);
	}

	immu_regs_intrmap_enable(immu, irta_reg);
}

/* ####################################################################### */

/*
 * immu_intr_handler()
 *	the fault event handler for a single immu unit
 */
uint_t
immu_intr_handler(caddr_t arg, caddr_t arg1 __unused)
{
	immu_t *immu = (immu_t *)arg;
	uint32_t status;
	int index, fault_reg_offset;
	int max_fault_index;
	boolean_t found_fault;
	dev_info_t *idip;

	mutex_enter(&(immu->immu_intr_lock));
	mutex_enter(&(immu->immu_regs_lock));

	/* read the fault status */
	status = immu_regs_get32(immu, IMMU_REG_FAULT_STS);

	idip = immu->immu_dip;
	ASSERT(idip);

	/* check if we have a pending fault for this immu unit */
	if ((status & IMMU_FAULT_STS_PPF) == 0) {
		mutex_exit(&(immu->immu_regs_lock));
		mutex_exit(&(immu->immu_intr_lock));
		return (DDI_INTR_UNCLAIMED);
	}

	/*
	 * handle all primary pending faults
	 */
	index = IMMU_FAULT_GET_INDEX(status);
	max_fault_index =  IMMU_CAP_GET_NFR(immu->immu_regs_cap) - 1;
	fault_reg_offset = IMMU_CAP_GET_FRO(immu->immu_regs_cap);

	found_fault = B_FALSE;
	_NOTE(CONSTCOND)
	while (1) {
		uint64_t val;
		uint8_t fault_reason;
		uint8_t fault_type;
		uint16_t sid;
		uint64_t pg_addr;
		uint64_t idx;

		/* read the higher 64bits */
		val = immu_regs_get64(immu, fault_reg_offset + index * 16 + 8);

		/* check if this fault register has pending fault */
		if (!IMMU_FRR_GET_F(val)) {
			break;
		}

		found_fault = B_TRUE;

		/* get the fault reason, fault type and sid */
		fault_reason = IMMU_FRR_GET_FR(val);
		fault_type = IMMU_FRR_GET_FT(val);
		sid = IMMU_FRR_GET_SID(val);

		/* read the first 64bits */
		val = immu_regs_get64(immu, fault_reg_offset + index * 16);
		pg_addr = val & IMMU_PAGEMASK;
		idx = val >> 48;

		/* clear the fault */
		immu_regs_put32(immu, fault_reg_offset + index * 16 + 12,
		    (((uint32_t)1) << 31));

		/* report the fault info */
		if (fault_reason < 0x20) {
			/* immu-remapping fault */
			ddi_err(DER_WARN, idip,
			    "generated a fault event when translating DMA %s\n"
			    "\t on address 0x%" PRIx64 " for PCI(%d, %d, %d), "
			    "the reason is:\n\t %s",
			    fault_type ? "read" : "write", pg_addr,
			    (sid >> 8) & 0xff, (sid >> 3) & 0x1f, sid & 0x7,
			    immu_dvma_faults[MIN(fault_reason,
			    DVMA_MAX_FAULTS)]);
			immu_print_fault_info(sid, pg_addr);
		} else if (fault_reason < 0x27) {
			/* intr-remapping fault */
			ddi_err(DER_WARN, idip,
			    "generated a fault event when translating "
			    "interrupt request\n"
			    "\t on index 0x%" PRIx64 " for PCI(%d, %d, %d), "
			    "the reason is:\n\t %s",
			    idx,
			    (sid >> 8) & 0xff, (sid >> 3) & 0x1f, sid & 0x7,
			    immu_intrmap_faults[MIN((fault_reason - 0x20),
			    INTRMAP_MAX_FAULTS)]);
		} else {
			ddi_err(DER_WARN, idip, "Unknown fault reason: 0x%x",
			    fault_reason);
		}

		index++;
		if (index > max_fault_index)
			index = 0;
	}

	/* Clear the fault */
	if (!found_fault) {
		ddi_err(DER_MODE, idip,
		    "Fault register set but no fault present");
	}
	immu_regs_put32(immu, IMMU_REG_FAULT_STS, 1);
	mutex_exit(&(immu->immu_regs_lock));
	mutex_exit(&(immu->immu_intr_lock));
	return (DDI_INTR_CLAIMED);
}
/* ######################################################################### */

/*
 * Interrupt remap entry points
 */

/* initialize interrupt remapping */
static int
immu_intrmap_init(int apic_mode)
{
	immu_t *immu;
	int error = DDI_FAILURE;

	if (immu_intrmap_enable == B_FALSE) {
		return (DDI_SUCCESS);
	}

	intrmap_apic_mode = apic_mode;
	immu_intrmap_drhd_gate_init();

	immu = list_head(&immu_list);
	for (; immu; immu = list_next(&immu_list, immu)) {
		if ((immu->immu_intrmap_running == B_TRUE) &&
		    IMMU_ECAP_GET_IR(immu->immu_regs_excap)) {
			if (init_unit(immu) == DDI_SUCCESS) {
				error = DDI_SUCCESS;
			}
		}
	}

	/*
	 * if all IOMMU units disable intr remapping,
	 * return FAILURE
	 */
	return (error);
}



/* enable interrupt remapping */
static void
immu_intrmap_switchon(int suppress_brdcst_eoi)
{
	immu_t *immu;


	intrmap_suppress_brdcst_eoi = suppress_brdcst_eoi;

	immu = list_head(&immu_list);
	for (; immu; immu = list_next(&immu_list, immu)) {
		if (immu->immu_intrmap_setup == B_TRUE) {
			intrmap_enable(immu);
		}
	}
}

/* alloc remapping entry for the interrupt */
static void
immu_intrmap_alloc(void **intrmap_private_tbl, dev_info_t *dip,
    uint16_t type, int count, uchar_t ioapic_index)
{
	immu_t	*immu;
	intrmap_t *intrmap;
	immu_inv_wait_t *iwp;
	uint32_t	idx, i;
	uint32_t	sid_svt_sq;
	intrmap_private_t *intrmap_private;
	const char	*driver;

	if (intrmap_private_tbl[0] == INTRMAP_DISABLE ||
	    intrmap_private_tbl[0] != NULL) {
		return;
	}

	driver = (dip != NULL) ? ddi_driver_name(dip) : NULL;
	if (immu_intrmap_driver_excluded(driver)) {
		intrmap_private_tbl[0] = INTRMAP_DISABLE;
		return;
	}

	intrmap_private_tbl[0] =
	    kmem_zalloc(sizeof (intrmap_private_t), KM_SLEEP);
	intrmap_private = INTRMAP_PRIVATE(intrmap_private_tbl[0]);

	immu = get_immu(dip, type, ioapic_index);
	if ((immu != NULL) && (immu->immu_intrmap_running == B_TRUE)) {
		intrmap_private->ir_immu = immu;
		intrmap_private->ir_dip = dip;
	} else {
		prom_printf("IRMAP: alloc failed for dip=%p type=0x%x "
		    "(immu=%p, running=%d)\n",
		    (void *)dip, type, (void *)immu,
		    immu ? immu->immu_intrmap_running : -1);
		goto intrmap_disable;
	}

	intrmap = immu->immu_intrmap;

	if (count == 1) {
		idx = alloc_tbl_entry(intrmap);
	} else {
		idx = alloc_tbl_multi_entries(intrmap, count);
	}

	if (idx == INTRMAP_IDX_FULL) {
		prom_printf("IRMAP: allocation FULL for dip=%p type=0x%x count=%d\n",
		    (void *)dip, type, count);
		goto intrmap_disable;
	}

	intrmap_private->ir_idx = idx;

	sid_svt_sq = intrmap_private->ir_sid_svt_sq =
	    get_sid(dip, type, ioapic_index);
	iwp = &intrmap_private->ir_inv_wait;
	immu_init_inv_wait(iwp, "intrmaplocal", B_TRUE);

	/* 🔎 Log the allocation */
	prom_printf("IRMAP: dip=%p type=%s count=%d idx=%u (sid=0x%x) immu=%s\n",
	    (void *)dip,
	    (DDI_INTR_IS_MSI_OR_MSIX(type) ? "MSI/MSI-X" : "IOAPIC"),
	    count, idx, sid_svt_sq,
	    immu->immu_name ? immu->immu_name : "<noname>");

	if (count == 1) {
		if (IMMU_CAP_GET_CM(immu->immu_regs_cap)) {
			immu_qinv_intr_one_cache(immu, idx, iwp);
		} else {
			immu_regs_wbf_flush(immu);
		}
		return;
	}

	for (i = 1; i < count; i++) {
		intrmap_private_tbl[i] =
		    kmem_zalloc(sizeof (intrmap_private_t), KM_SLEEP);

		INTRMAP_PRIVATE(intrmap_private_tbl[i])->ir_immu = immu;
		INTRMAP_PRIVATE(intrmap_private_tbl[i])->ir_dip = dip;
		INTRMAP_PRIVATE(intrmap_private_tbl[i])->ir_sid_svt_sq =
		    sid_svt_sq;
		INTRMAP_PRIVATE(intrmap_private_tbl[i])->ir_idx = idx + i;

		prom_printf("IRMAP:   multi-idx=%u for dip=%p\n", idx+i, (void*)dip);
	}

	if (IMMU_CAP_GET_CM(immu->immu_regs_cap)) {
		immu_qinv_intr_caches(immu, idx, count, iwp);
	} else {
		immu_regs_wbf_flush(immu);
	}

	return;

intrmap_disable:
	kmem_free(intrmap_private_tbl[0], sizeof (intrmap_private_t));
	intrmap_private_tbl[0] = INTRMAP_DISABLE;
}

/* remapping the interrupt */
static void
immu_intrmap_map(void *intrmap_private, void *intrmap_data,
	uint16_t type, int count)
{
	immu_t          *immu;
	immu_inv_wait_t *iwp;
	intrmap_t       *intrmap;
	ioapic_rdt_t    *irdt = (ioapic_rdt_t *)intrmap_data;
	msi_regs_t      *mregs = (msi_regs_t *)intrmap_data;
	intrmap_rte_t    irte;
	intrmap_rte_t    irte_np;
	uint_t           idx, i;
	uint_t           last_vector;
	uint_t           sid;
	uint32_t         dst, sid_svt_sq;
	uint64_t         raw_dst, shifted_dst;
	uchar_t          vector, dlm, tm, rh, dm;
	int              unit = -1;
	boolean_t        unit_entered = B_FALSE;

	if (intrmap_private == INTRMAP_DISABLE)
		return;

	idx = INTRMAP_PRIVATE(intrmap_private)->ir_idx;
	immu = INTRMAP_PRIVATE(intrmap_private)->ir_immu;
	iwp = &INTRMAP_PRIVATE(intrmap_private)->ir_inv_wait;
	intrmap = immu->immu_intrmap;
	sid_svt_sq = INTRMAP_PRIVATE(intrmap_private)->ir_sid_svt_sq;
	sid = sid_svt_sq & 0xffff;
	unit = immu_intrmap_unit_index(immu);
	immu_intrmap_drhd_map_enter(unit);
	unit_entered = B_TRUE;

	{
		if (unit >= 0 && immu_intrmap_drhd_transition_active(unit)) {
			uint32_t bit = (1u << unit);
			char dip_path[1024];
			const char *dip_name = "<null>";
			dev_info_t *dip = INTRMAP_PRIVATE(intrmap_private)->ir_dip;

			if (dip != NULL) {
				if (ddi_pathname(dip, dip_path) == NULL)
					(void) strlcpy(dip_path, "<pathname-failed>",
					    sizeof (dip_path));
				dip_name = dip_path;
			}

			if ((atomic_or_32_nv(&immu_intrmap_drhd_transition_warn_mask,
			    bit) & bit) == 0) {
				cmn_err(CE_WARN, "!immu_intrmap_map: deferring IRTE "
				    "update during DRHD transition dip=%s unit=%d "
				    "sid=0x%x bdf=%02x:%02x.%u idx=%u type=0x%x count=%d",
				    dip_name, unit, sid, (sid >> 8) & 0xff,
				    (sid >> 3) & 0x1f, sid & 0x7, idx, type, count);
			}
			goto out;
		}
	}

	if (!DDI_INTR_IS_MSI_OR_MSIX(type)) {
		dm = RDT_DM(irdt->ir_lo);
		rh = 0;
		tm = RDT_TM(irdt->ir_lo);
		dlm = RDT_DLM(irdt->ir_lo);
		dst = irdt->ir_hi;

		/*
		* Mark the IRTE's TM as Edge to suppress broadcast EOI.
		*/
		if (intrmap_suppress_brdcst_eoi) {
			tm = TRIGGER_MODE_EDGE;
		}

		vector = RDT_VECTOR(irdt->ir_lo);
	} else {
		/* MSI/MSI-X interrupt */
		dm = MSI_ADDR_DM_PHYSICAL;
		rh = MSI_ADDR_RH_FIXED;
		tm = TRIGGER_MODE_EDGE;
		dlm = 0;
		dst = mregs->mr_addr;

		vector = mregs->mr_data & 0xff;

		/* Diagnostic logging before shifting dst */
		raw_dst = (uint64_t)mregs->mr_addr;
		shifted_dst = (intrmap_apic_mode == LOCAL_APIC) ?
		    ((raw_dst & 0xFFULL) << 8) : raw_dst;

		if (intrmap_apic_mode == LOCAL_APIC && shifted_dst > 0xFF00) {
			cmn_err(CE_WARN,
				"!immu_intrmap_map: suspicious APIC dest (shifted=0x%" PRIx64
				") raw 0x%" PRIx64, shifted_dst, raw_dst);
		}

		cmn_err(CE_CONT,
			"!immu_intrmap_map: MSI setup idx=%u vector=0x%x "
			"raw_dst=0x%" PRIx64 " shifted_dst=0x%" PRIx64 " "
			"msi_addr=0x%" PRIx64 " msi_data=0x%" PRIx32 " (count=%d)",
			idx, (unsigned int)vector,
			raw_dst, shifted_dst,
			(uint64_t)mregs->mr_addr, (uint32_t)mregs->mr_data,
			count);
	}

	if (intrmap_apic_mode == LOCAL_APIC)
		dst = (dst & 0xFF) << 8;

	if (count <= 0) {
		cmn_err(CE_WARN, "!immu_intrmap_map: invalid interrupt count=%d "
		    "idx=%u type=0x%x", count, idx, type);
		goto out;
	}

	last_vector = (uint_t)vector + (uint_t)count - 1;
	if (vector < APIC_BASE_VECT || last_vector > 0xffu) {
		char dip_path[1024];
		const char *dip_name = "<null>";
		dev_info_t *dip = INTRMAP_PRIVATE(intrmap_private)->ir_dip;

		if (dip != NULL) {
			if (ddi_pathname(dip, dip_path) == NULL)
				(void) strlcpy(dip_path, "<pathname-failed>",
				    sizeof (dip_path));
			dip_name = dip_path;
		}
		cmn_err(CE_WARN, "!immu_intrmap_map: refusing illegal vector "
		    "programming dip=%s sid=0x%x bdf=%02x:%02x.%u idx=%u "
		    "type=0x%x vector=0x%x count=%d (last=0x%x, base=0x%x)",
		    dip_name, sid, (sid >> 8) & 0xff, (sid >> 3) & 0x1f,
		    sid & 0x7, idx, type, (uint_t)vector, count, last_vector,
		    APIC_BASE_VECT);
		bzero(intrmap->intrmap_vaddr + idx * INTRMAP_RTE_SIZE,
		    (size_t)count * INTRMAP_RTE_SIZE);
		if (count == 1) {
			immu_qinv_intr_one_cache(immu, idx, iwp);
		} else {
			immu_qinv_intr_caches(immu, idx, count, iwp);
		}
		goto out;
		}

	if (count == 1) {
		irte.lo = IRTE_LOW(dst, vector, dlm, tm, rh, dm, 0, 1);
		irte.hi = IRTE_HIGH(sid_svt_sq);
		if (immu_intrmap_two_phase_irte_update) {
			irte_np = irte;
			irte_np.lo &= ~1ULL;
			bcopy(&irte_np, intrmap->intrmap_vaddr + idx * INTRMAP_RTE_SIZE,
			    INTRMAP_RTE_SIZE);
			membar_producer();
			immu_qinv_intr_one_cache(immu, idx, iwp);
		}
		immu_intrmap_irte_trace_record(
		    INTRMAP_PRIVATE(intrmap_private), immu, unit, idx, type,
		    count, vector, dlm, tm, rh, dm, dst, sid, &irte);

		cmn_err(CE_CONT,
		    "!IRTE[%u]: lo=0x%" PRIx64 " hi=0x%" PRIx64,
		    idx, (uint64_t)irte.lo, (uint64_t)irte.hi);

		/* set interrupt remapping table entry */
		bcopy(&irte, intrmap->intrmap_vaddr + idx * INTRMAP_RTE_SIZE,
		    INTRMAP_RTE_SIZE);

		membar_producer();
		immu_qinv_intr_one_cache(immu, idx, iwp);

	} else {
		if (immu_intrmap_two_phase_irte_update) {
			uchar_t pvec = vector;

			for (i = 0; i < count; i++) {
				irte.lo = IRTE_LOW(dst, pvec, dlm, tm, rh, dm, 0, 1);
				irte.hi = IRTE_HIGH(sid_svt_sq);
				irte_np = irte;
				irte_np.lo &= ~1ULL;
				bcopy(&irte_np, intrmap->intrmap_vaddr +
				    (idx + i) * INTRMAP_RTE_SIZE,
				    INTRMAP_RTE_SIZE);
				pvec++;
			}
			membar_producer();
			immu_qinv_intr_caches(immu, idx, count, iwp);
		}
		for (i = 0; i < count; i++) {
			irte.lo = IRTE_LOW(dst, vector, dlm, tm, rh, dm, 0, 1);
			irte.hi = IRTE_HIGH(sid_svt_sq);
			immu_intrmap_irte_trace_record(
			    INTRMAP_PRIVATE(intrmap_private), immu, unit,
			    idx + i, type, count, vector, dlm, tm, rh, dm,
			    dst, sid, &irte);

			cmn_err(CE_CONT,
			    "!IRTE[%u]: lo=0x%" PRIx64 " hi=0x%" PRIx64,
			    idx + i, (uint64_t)irte.lo, (uint64_t)irte.hi);

			/* set interrupt remapping table entry */
			bcopy(&irte, intrmap->intrmap_vaddr +
			    (idx + i) * INTRMAP_RTE_SIZE,
			    INTRMAP_RTE_SIZE);
			vector++;
		}

		membar_producer();
		immu_qinv_intr_caches(immu, idx, count, iwp);
	}
out:
	if (unit_entered)
		immu_intrmap_drhd_map_exit(unit);
}
/* remapping the interrupt */
// static void
// immu_intrmap_map(void *intrmap_private, void *intrmap_data, uint16_t type,
//     int count)
// {
// 	immu_t	*immu;
// 	immu_inv_wait_t	*iwp;
// 	intrmap_t	*intrmap;
// 	ioapic_rdt_t	*irdt = (ioapic_rdt_t *)intrmap_data;
// 	msi_regs_t	*mregs = (msi_regs_t *)intrmap_data;
// 	intrmap_rte_t	irte;
// 	uint_t		idx, i;
// 	uint32_t	dst, sid_svt_sq;
// 	uchar_t		vector, dlm, tm, rh, dm;
// 
// 	if (intrmap_private == INTRMAP_DISABLE)
// 		return;
// 
// 	idx = INTRMAP_PRIVATE(intrmap_private)->ir_idx;
// 	immu = INTRMAP_PRIVATE(intrmap_private)->ir_immu;
// 	iwp = &INTRMAP_PRIVATE(intrmap_private)->ir_inv_wait;
// 	intrmap = immu->immu_intrmap;
// 	sid_svt_sq = INTRMAP_PRIVATE(intrmap_private)->ir_sid_svt_sq;
// 
// 	if (!DDI_INTR_IS_MSI_OR_MSIX(type)) {
// 		dm = RDT_DM(irdt->ir_lo);
// 		rh = 0;
// 		tm = RDT_TM(irdt->ir_lo);
// 		dlm = RDT_DLM(irdt->ir_lo);
// 		dst = irdt->ir_hi;
// 
// 		/*
// 		 * Mark the IRTE's TM as Edge to suppress broadcast EOI.
// 		 */
// 		if (intrmap_suppress_brdcst_eoi) {
// 			tm = TRIGGER_MODE_EDGE;
// 		}
// 
// 		vector = RDT_VECTOR(irdt->ir_lo);
// 	} else {
// 		dm = MSI_ADDR_DM_PHYSICAL;
// 		rh = MSI_ADDR_RH_FIXED;
// 		tm = TRIGGER_MODE_EDGE;
// 		dlm = 0;
// 		dst = mregs->mr_addr;
// 
// 		vector = mregs->mr_data & 0xff;
// 	}
// 
// 	if (intrmap_apic_mode == LOCAL_APIC)
// 		dst = (dst & 0xFF) << 8;
// 
// 	if (count == 1) {
// 		irte.lo = IRTE_LOW(dst, vector, dlm, tm, rh, dm, 0, 1);
// 		irte.hi = IRTE_HIGH(sid_svt_sq);
// 
// 		/* set interrupt remapping table entry */
// 		bcopy(&irte, intrmap->intrmap_vaddr +
// 		    idx * INTRMAP_RTE_SIZE,
// 		    INTRMAP_RTE_SIZE);
// 
// 		immu_qinv_intr_one_cache(immu, idx, iwp);
// 
// 	} else {
// 		for (i = 0; i < count; i++) {
// 			irte.lo = IRTE_LOW(dst, vector, dlm, tm, rh, dm, 0, 1);
// 			irte.hi = IRTE_HIGH(sid_svt_sq);
// 
// 			/* set interrupt remapping table entry */
// 			bcopy(&irte, intrmap->intrmap_vaddr +
// 			    idx * INTRMAP_RTE_SIZE,
// 			    INTRMAP_RTE_SIZE);
// 			vector++;
// 			idx++;
// 		}
// 
// 		immu_qinv_intr_caches(immu, idx, count, iwp);
// 	}
// }

/* free the remapping entry */
static void
immu_intrmap_free(void **intrmap_privatep)
{
	immu_t *immu;
	immu_inv_wait_t *iwp;
	intrmap_t *intrmap;
	uint32_t idx;

	if (*intrmap_privatep == INTRMAP_DISABLE || *intrmap_privatep == NULL) {
		*intrmap_privatep = NULL;
		return;
	}

	immu = INTRMAP_PRIVATE(*intrmap_privatep)->ir_immu;
	iwp = &INTRMAP_PRIVATE(*intrmap_privatep)->ir_inv_wait;
	intrmap = immu->immu_intrmap;
	idx = INTRMAP_PRIVATE(*intrmap_privatep)->ir_idx;

	bzero(intrmap->intrmap_vaddr + idx * INTRMAP_RTE_SIZE,
	    INTRMAP_RTE_SIZE);

	immu_qinv_intr_one_cache(immu, idx, iwp);

	mutex_enter(&intrmap->intrmap_lock);
	bitset_del(&intrmap->intrmap_map, idx);
	if (intrmap->intrmap_free == INTRMAP_IDX_FULL) {
		intrmap->intrmap_free = idx;
	}
	mutex_exit(&intrmap->intrmap_lock);

	kmem_free(*intrmap_privatep, sizeof (intrmap_private_t));
	*intrmap_privatep = NULL;
}

/* record the ioapic rdt entry */
static void
immu_intrmap_rdt(void *intrmap_private, ioapic_rdt_t *irdt)
{
	uint32_t rdt_entry, tm, pol, idx, vector;

	rdt_entry = irdt->ir_lo;

	if (intrmap_private != INTRMAP_DISABLE && intrmap_private != NULL) {
		idx = INTRMAP_PRIVATE(intrmap_private)->ir_idx;
		tm = RDT_TM(rdt_entry);
		pol = RDT_POL(rdt_entry);
		vector = RDT_VECTOR(rdt_entry);
		irdt->ir_lo = (tm << INTRMAP_IOAPIC_TM_SHIFT) |
		    (pol << INTRMAP_IOAPIC_POL_SHIFT) |
		    ((idx >> 15) << INTRMAP_IOAPIC_IDX15_SHIFT) |
		    vector;
		irdt->ir_hi = (idx << INTRMAP_IOAPIC_IDX_SHIFT) |
		    (1 << INTRMAP_IOAPIC_FORMAT_SHIFT);
	} else {
		irdt->ir_hi <<= APIC_ID_BIT_OFFSET;
	}
}


static void
immu_intrmap_msi(void *intrmap_private, msi_regs_t *mregs)
{
	uint_t	idx;

	if (intrmap_private != INTRMAP_DISABLE && intrmap_private != NULL) {
		/* Interrupt Remapping case: use IR-format MSI address encoding */
		idx = INTRMAP_PRIVATE(intrmap_private)->ir_idx;

		mregs->mr_data = 0;
		mregs->mr_addr = MSI_ADDR_HDR |
		    ((idx & 0x7fff) << INTRMAP_MSI_IDX_SHIFT) |
		    (1 << INTRMAP_MSI_FORMAT_SHIFT) |
		    (1 << INTRMAP_MSI_SHV_SHIFT) |
		    ((idx >> 15) << INTRMAP_MSI_IDX15_SHIFT);

		cmn_err(CE_CONT,
		    "!immu_intrmap_msi(IR): idx=%u mr_addr=0x%" PRIx64
		    " mr_data=0x%x (IR-format encoding)",
		    idx, (uint64_t)mregs->mr_addr, mregs->mr_data);
	} else {
		/* Compatibility / non-remapped format */
		mregs->mr_addr = MSI_ADDR_HDR |
		    (MSI_ADDR_RH_FIXED << MSI_ADDR_RH_SHIFT) |
		    (MSI_ADDR_DM_PHYSICAL << MSI_ADDR_DM_SHIFT) |
		    (mregs->mr_addr << MSI_ADDR_DEST_SHIFT);
		mregs->mr_data = (MSI_DATA_TM_EDGE << MSI_DATA_TM_SHIFT) |
		    mregs->mr_data;

		cmn_err(CE_CONT,
		    "!immu_intrmap_msi(compat): mr_addr=0x%" PRIx64
		    " mr_data=0x%x",
		    (uint64_t)mregs->mr_addr, mregs->mr_data);
	}
}

/*ARGSUSED*/
// static void
// immu_intrmap_msi(void *intrmap_private, msi_regs_t *mregs)
// {
// 	uint_t	idx;
// 
// 	if (intrmap_private != INTRMAP_DISABLE && intrmap_private != NULL) {
// 		idx = INTRMAP_PRIVATE(intrmap_private)->ir_idx;
// 
// 		mregs->mr_data = 0;
// 		mregs->mr_addr = MSI_ADDR_HDR |
// 		    ((idx & 0x7fff) << INTRMAP_MSI_IDX_SHIFT) |
// 		    (1 << INTRMAP_MSI_FORMAT_SHIFT) |
// 		    (1 << INTRMAP_MSI_SHV_SHIFT) |
// 		    ((idx >> 15) << INTRMAP_MSI_IDX15_SHIFT);
// 	} else {
// 		mregs->mr_addr = MSI_ADDR_HDR |
// 		    (MSI_ADDR_RH_FIXED << MSI_ADDR_RH_SHIFT) |
// 		    (MSI_ADDR_DM_PHYSICAL << MSI_ADDR_DM_SHIFT) |
// 		    (mregs->mr_addr << MSI_ADDR_DEST_SHIFT);
// 		mregs->mr_data = (MSI_DATA_TM_EDGE << MSI_DATA_TM_SHIFT) |
// 		    mregs->mr_data;
// 	}
// }

/* ######################################################################### */
/*
 * Functions exported by immu_intr.c
 */
void
immu_intrmap_setup(list_t *listp)
{
	immu_t *immu;

	/*
	 * Check if ACPI DMAR tables say that
	 * interrupt remapping is supported
	 */
	if (immu_dmar_intrmap_supported() == B_FALSE) {
		return;
	}

	/*
	 * Check if interrupt remapping is disabled.
	 */
	if (immu_intrmap_enable == B_FALSE) {
		return;
	}

	psm_vt_ops = &intrmap_ops;

	immu = list_head(listp);
	for (; immu; immu = list_next(listp, immu)) {
		mutex_init(&(immu->immu_intrmap_lock), NULL,
		    MUTEX_DEFAULT, NULL);
		mutex_enter(&(immu->immu_intrmap_lock));
		immu_init_inv_wait(&immu->immu_intrmap_inv_wait,
		    "intrmapglobal", B_TRUE);
		immu->immu_intrmap_setup = B_TRUE;
		mutex_exit(&(immu->immu_intrmap_lock));
	}
}

void
immu_intrmap_startup(immu_t *immu)
{
	/* do nothing */
	mutex_enter(&(immu->immu_intrmap_lock));
	if (immu->immu_intrmap_setup == B_TRUE) {
		immu->immu_intrmap_running = B_TRUE;
	}
	mutex_exit(&(immu->immu_intrmap_lock));
}

/*
 * Register a Intel IOMMU unit (i.e. DMAR unit's)
 * interrupt handler
 */
void
immu_intr_register(immu_t *immu)
{
	int irq, vect;
	char intr_handler_name[IMMU_MAXNAMELEN];
	uint32_t msi_data;
	uint32_t uaddr;
	uint32_t msi_addr;
	uint32_t localapic_id = 0;

	if (psm_get_localapicid)
		localapic_id = psm_get_localapicid(0);

	msi_addr = (MSI_ADDR_HDR |
	    ((localapic_id & 0xFF) << MSI_ADDR_DEST_SHIFT) |
	    (MSI_ADDR_RH_FIXED << MSI_ADDR_RH_SHIFT) |
	    (MSI_ADDR_DM_PHYSICAL << MSI_ADDR_DM_SHIFT));

	if (intrmap_apic_mode == LOCAL_X2APIC) {
		uaddr = localapic_id & 0xFFFFFF00;
	} else {
		uaddr = 0;
	}

	/* Dont need to hold immu_intr_lock since we are in boot */
	irq = vect = psm_get_ipivect(IMMU_INTR_IPL, -1);
	if (psm_xlate_vector_by_irq != NULL)
		vect = psm_xlate_vector_by_irq(irq);

	msi_data = ((MSI_DATA_DELIVERY_FIXED <<
	    MSI_DATA_DELIVERY_SHIFT) | vect);

	(void) snprintf(intr_handler_name, sizeof (intr_handler_name),
	    "%s-intr-handler", immu->immu_name);

	(void) add_avintr((void *)NULL, IMMU_INTR_IPL,
	    immu_intr_handler, intr_handler_name, irq,
	    (caddr_t)immu, NULL, NULL, NULL);

	immu_regs_intr_enable(immu, msi_addr, msi_data, uaddr);

	(void) immu_intr_handler((caddr_t)immu, NULL);
}


// /*
//  * Return MSI message address/data programmed for this handle.
//  * For passthru drivers like ppt that must reprogram hardware.
//  */
// int
// ddi_intr_get_msi_info(ddi_intr_handle_t hdl, uint32_t *addrlo,
//     uint32_t *addrhi, uint16_t *data)
// {
// 	ddi_intr_handle_impl_t *ihp =
// 	    (ddi_intr_handle_impl_t *)hdl;
// 
// 	if (ihp == NULL || ihp->ih_type != DDI_INTR_TYPE_MSI)
// 		return (DDI_FAILURE);
// 
// 	if (addrlo != NULL)
// 		*addrlo = (uint32_t)(ihp->ih_msi_addr & 0xffffffffUL);
// 	if (addrhi != NULL)
// 		*addrhi = (uint32_t)(ihp->ih_msi_addr >> 32);
// 	if (data != NULL)
// 		*data = (uint16_t)ihp->ih_msi_data;
// 
// 	return (DDI_SUCCESS);
// }
