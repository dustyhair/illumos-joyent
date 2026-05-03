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
 * Copyright 2019 Joyent, Inc.
 * Copyright 2022 OmniOS Community Edition (OmniOSce) Association.
 */

#include <sys/cdefs.h>

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/kmem.h>
#include <sys/module.h>
#include <sys/bus.h>
#include <sys/pciio.h>
#include <sys/sysctl.h>

#include <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>

#include <machine/vmm.h>
#include <machine/vmm_dev.h>

#include <sys/conf.h>
#include <sys/ddi.h>
#include <sys/ddi_intr_impl.h>
#include <sys/stat.h>
#include <sys/sunddi.h>
#include <sys/pci.h>
#include <sys/pci_cap.h>
#include <sys/pcie_impl.h>
#include <sys/ppt_dev.h>
#include <sys/mkdev.h>
#include <sys/sysmacros.h>
#include <sys/promif.h>

#include "vmm_lapic.h"
#include "vioapic.h"

#include "iommu.h"
#include "ppt.h"

#define	MAX_MSIMSGS	32

/*
 * If the MSI-X table is located in the middle of a BAR then that MMIO
 * region gets split into two segments - one segment above the MSI-X table
 * and the other segment below the MSI-X table - with a hole in place of
 * the MSI-X table so accesses to it can be trapped and emulated.
 *
 * So, allocate a MMIO segment for each BAR register + 1 additional segment.
 */
#define	MAX_MMIOSEGS	((PCIR_MAX_BAR_0 + 1) + 1)

struct pptintr_arg {
	struct pptdev	*pptdev;
	uint64_t	addr;
	uint64_t	msg_data;
};

struct pptintx_arg {
	struct pptdev	*pptdev;
	int		ioapic_irq;
};

struct pptseg {
	vm_paddr_t	gpa;
	size_t		len;
	int		wired;
};

struct pptbar {
	uint64_t base;
	uint64_t size;
	uint_t type;
	ddi_acc_handle_t io_handle;
	caddr_t io_ptr;
	uint_t ddireg;
};

struct pptdev {
	dev_info_t		*pptd_dip;
	list_node_t		pptd_node;
	ddi_acc_handle_t	pptd_cfg;
	struct pptbar		pptd_bars[PCI_BASE_NUM];
	struct vm		*vm;
	struct pptseg mmio[MAX_MMIOSEGS];
	struct {
		int	num_msgs;		/* guest state */
		boolean_t is_fixed;
		size_t	inth_sz;
		ddi_intr_handle_t *inth;
		uint64_t intr_count;
		uint64_t setup_count;
		struct pptintr_arg arg[MAX_MSIMSGS];
	} msi;

	struct {
		int num_msgs;
		size_t inth_sz;
		size_t arg_sz;
		ddi_intr_handle_t *inth;
		struct pptintr_arg *arg;
	} msix;
	struct {
		boolean_t enabled;
		int ioapic_irq;
		ddi_intr_handle_t inth;
		struct pptintx_arg arg;
	} intx;
};


static major_t		ppt_major;
static void		*ppt_state;
static kmutex_t		pptdev_mtx;
static list_t		pptdev_list;
int			ppt_diag_enable = 0;
int			ppt_unassign_diag_enable = 0;
int			ppt_unassign_flr_quiesce = 0;
int			ppt_unassign_preremove_diag_enable = 0;
int			ppt_unassign_allfunc_quiesce = 0;

#ifndef PCI_PMCSR_STATE_D0
#define	PCI_PMCSR_STATE_D0	0x0000
#endif

#ifndef PCI_PMCSR_STATE_D3HOT
#define	PCI_PMCSR_STATE_D3HOT	0x0003
#endif

#ifndef PCI_PMCSR_NO_SOFT_RESET
#define	PCI_PMCSR_NO_SOFT_RESET	0x0008
#endif

#ifndef PCIE_LINKSTS_NEG_WIDTH_MASK
#define	PCIE_LINKSTS_NEG_WIDTH_MASK	0x03f0
#define	PCIE_LINKSTS_NEG_WIDTH_SHIFT	4
#endif

#define	LINK_POLL_INTERVAL_US	10000
#define	LINK_POLL_TIMEOUT_US	1000000
#define	DEVICE_PRESENT_TIMEOUT_US	5000000

struct ppt_reset_state {
	uint16_t	prs_bdf;
	uint16_t	prs_venid;
	uint16_t	prs_devid;
	uint16_t	prs_cmd;
	uint16_t	prs_stat;
	uint16_t	prs_pmcsr;
	uint16_t	prs_msictl;
	uint16_t	prs_msidata;
	uint16_t	prs_msixctl;
	uint32_t	prs_bar0;
	uint32_t	prs_bar1;
	uint32_t	prs_bar3;
	uint32_t	prs_msiaddr_lo;
	uint32_t	prs_msiaddr_hi;
	boolean_t	prs_present;
	boolean_t	prs_has_pm;
	boolean_t	prs_has_msi;
	boolean_t	prs_has_msix;
};

static boolean_t ppt_wait_device_present_locked(struct pptdev *);
static boolean_t ppt_wait_link_active(dev_info_t *);
static boolean_t ppt_pm_reset(dev_info_t *, boolean_t);
static void ppt_bus_reset(dev_info_t *);
static void ppt_reset_pci_power_state(dev_info_t *);
static void ppt_reset_log_state_locked(struct pptdev *, ppt_reset_type_t,
    const char *);
static int ppt_save_reset_config_locked(struct pptdev *, ppt_reset_type_t);
static int ppt_restore_reset_config_locked(struct pptdev *, ppt_reset_type_t);
static int ppt_reset_device_method_locked(struct pptdev *, ppt_reset_flags_t,
    ppt_reset_type_t, ppt_reset_type_t *);
static int ppt_setup_intx_locked(struct pptdev *, int, boolean_t);
static uint_t pptintr_intx(caddr_t arg, caddr_t unused);
static void ppt_teardown_intx(struct pptdev *ppt);

#define	PPT_MINOR_NAME	"ppt"

static ddi_device_acc_attr_t ppt_attr = {
	DDI_DEVICE_ATTR_V0,
	DDI_NEVERSWAP_ACC,
	DDI_STORECACHING_OK_ACC,
	DDI_DEFAULT_ACC
};

/*
 * ddi_intr_alloc() can hand back a single MSI/MSI-X handle that still
 * advertises block semantics.  ppt only ever wants the single-vector enable
 * path in that case, so normalize the handle once up front.
 */
static void
ppt_intr_normalize_single(ddi_intr_handle_t h)
{
	ddi_intr_handle_impl_t *hdlp = (ddi_intr_handle_impl_t *)h;

	if (hdlp == NULL)
		return;

	/*
	 * A single MSI/MSI-X vector should use the plain enable path.  Leaving
	 * BLOCK set makes ddi treat it like a block enable/disable request,
	 * which does not match the passthrough interrupt lifecycle.
	 */
	if (DDI_INTR_IS_MSI_OR_MSIX(hdlp->ih_type))
		hdlp->ih_cap &= ~DDI_INTR_FLAG_BLOCK;
}

static int
ppt_open(dev_t *devp, int flag, int otyp, cred_t *cr)
{
	/* XXX: require extra privs? */
	return (0);
}

#define	BAR_TO_IDX(bar)	(((bar) - PCI_CONF_BASE0) / PCI_BAR_SZ_32)
#define	BAR_VALID(b)	(			\
		(b) >= PCI_CONF_BASE0 &&	\
		(b) <= PCI_CONF_BASE5 &&	\
		((b) & (PCI_BAR_SZ_32-1)) == 0)

static int
ppt_ioctl(dev_t dev, int cmd, intptr_t arg, int md, cred_t *cr, int *rv)
{
	minor_t minor = getminor(dev);
	struct pptdev *ppt;
	void *data = (void *)arg;

	if ((ppt = ddi_get_soft_state(ppt_state, minor)) == NULL) {
		return (ENOENT);
	}

	switch (cmd) {
	case PPT_CFG_READ: {
		struct ppt_cfg_io cio;
		ddi_acc_handle_t cfg = ppt->pptd_cfg;

		if (ddi_copyin(data, &cio, sizeof (cio), md) != 0) {
			return (EFAULT);
		}
		switch (cio.pci_width) {
		case 4:
			cio.pci_data = pci_config_get32(cfg, cio.pci_off);
			break;
		case 2:
			cio.pci_data = pci_config_get16(cfg, cio.pci_off);
			break;
		case 1:
			cio.pci_data = pci_config_get8(cfg, cio.pci_off);
			break;
		default:
			return (EINVAL);
		}

		if (ddi_copyout(&cio, data, sizeof (cio), md) != 0) {
			return (EFAULT);
		}
		return (0);
	}
	case PPT_CFG_WRITE: {
		struct ppt_cfg_io cio;
		ddi_acc_handle_t cfg = ppt->pptd_cfg;

		if (ddi_copyin(data, &cio, sizeof (cio), md) != 0) {
			return (EFAULT);
		}
		switch (cio.pci_width) {
		case 4:
			pci_config_put32(cfg, cio.pci_off, cio.pci_data);
			break;
		case 2:
			pci_config_put16(cfg, cio.pci_off, cio.pci_data);
			break;
		case 1:
			pci_config_put8(cfg, cio.pci_off, cio.pci_data);
			break;
		default:
			return (EINVAL);
		}

		return (0);
	}
	case PPT_BAR_QUERY: {
		struct ppt_bar_query barg;
		struct pptbar *pbar;

		if (ddi_copyin(data, &barg, sizeof (barg), md) != 0) {
			return (EFAULT);
		}
		if (barg.pbq_baridx >= PCI_BASE_NUM) {
			return (EINVAL);
		}
		pbar = &ppt->pptd_bars[barg.pbq_baridx];

		if (pbar->base == 0 || pbar->size == 0) {
			return (ENOENT);
		}
		barg.pbq_type = pbar->type;
		barg.pbq_base = pbar->base;
		barg.pbq_size = pbar->size;

		if (ddi_copyout(&barg, data, sizeof (barg), md) != 0) {
			return (EFAULT);
		}
		return (0);
	}
	case PPT_BAR_READ: {
		struct ppt_bar_io bio;
		struct pptbar *pbar;
		void *addr;
		uint_t rnum;
		ddi_acc_handle_t cfg;

		if (ddi_copyin(data, &bio, sizeof (bio), md) != 0) {
			return (EFAULT);
		}
		rnum = bio.pbi_bar;
		if (rnum >= PCI_BASE_NUM) {
			return (EINVAL);
		}
		pbar = &ppt->pptd_bars[rnum];
		if (pbar->type != PCI_ADDR_IO || pbar->io_handle == NULL) {
			return (EINVAL);
		}
		addr = pbar->io_ptr + bio.pbi_off;

		switch (bio.pbi_width) {
		case 4:
			bio.pbi_data = ddi_get32(pbar->io_handle, addr);
			break;
		case 2:
			bio.pbi_data = ddi_get16(pbar->io_handle, addr);
			break;
		case 1:
			bio.pbi_data = ddi_get8(pbar->io_handle, addr);
			break;
		default:
			return (EINVAL);
		}

		if (ddi_copyout(&bio, data, sizeof (bio), md) != 0) {
			return (EFAULT);
		}
		return (0);
	}
	case PPT_BAR_WRITE: {
		struct ppt_bar_io bio;
		struct pptbar *pbar;
		void *addr;
		uint_t rnum;
		ddi_acc_handle_t cfg;

		if (ddi_copyin(data, &bio, sizeof (bio), md) != 0) {
			return (EFAULT);
		}
		rnum = bio.pbi_bar;
		if (rnum >= PCI_BASE_NUM) {
			return (EINVAL);
		}
		pbar = &ppt->pptd_bars[rnum];
		if (pbar->type != PCI_ADDR_IO || pbar->io_handle == NULL) {
			return (EINVAL);
		}
		addr = pbar->io_ptr + bio.pbi_off;

		switch (bio.pbi_width) {
		case 4:
			ddi_put32(pbar->io_handle, addr, bio.pbi_data);
			break;
		case 2:
			ddi_put16(pbar->io_handle, addr, bio.pbi_data);
			break;
		case 1:
			ddi_put8(pbar->io_handle, addr, bio.pbi_data);
			break;
		default:
			return (EINVAL);
		}

		return (0);
	}
	case PPT_RESET_DEVICE: {
		struct ppt_reset_req req;
		ppt_reset_type_t want_method;
		ppt_reset_type_t actual_method = PPT_RESET_NONE;
		ppt_reset_flags_t flags;
		int err;

		if (ddi_copyin(data, &req, sizeof (req), md) != 0) {
			return (EFAULT);
		}

		want_method = (ppt_reset_type_t)req.prr_method;
		flags = (ppt_reset_flags_t)req.prr_flags;
		req.prr_result_method = PPT_RESET_NONE;
		req.prr_result_error = 0;

		switch (want_method) {
		case PPT_RESET_NONE:
		case PPT_RESET_FLR:
		case PPT_RESET_PM:
		case PPT_RESET_BUS:
			break;
		default:
			return (EINVAL);
		}

		if (!mutex_tryenter(&pptdev_mtx)) {
			return (EBUSY);
		}
		if (ppt->vm != NULL) {
			mutex_exit(&pptdev_mtx);
			return (EBUSY);
		}

		flags |= PPT_RESET_F_FORCE;
		if (want_method == PPT_RESET_NONE) {
			flags |= PPT_RESET_F_ALLOW_FALLBACK;
		}
		err = ppt_reset_device_method_locked(ppt, flags, want_method,
		    &actual_method);
		mutex_exit(&pptdev_mtx);

		req.prr_result_method = actual_method;
		req.prr_result_error = err;
		if (ddi_copyout(&req, data, sizeof (req), md) != 0) {
			return (EFAULT);
		}
		return (err);
	}
	case PPT_INTX_SETUP: {
		struct ppt_intx_req req;
		int err;

		if (ddi_copyin(data, &req, sizeof (req), md) != 0) {
			return (EFAULT);
		}

		mutex_enter(&pptdev_mtx);
		if (ppt->vm == NULL) {
			mutex_exit(&pptdev_mtx);
			return (ENXIO);
		}

		err = ppt_setup_intx_locked(ppt, req.pir_ioapic_irq,
		    req.pir_enable != 0);
		mutex_exit(&pptdev_mtx);
		return (err);
	}

	default:
		return (ENOTTY);
	}

	return (0);
}

static int
ppt_find_msix_table_bar(struct pptdev *ppt)
{
	uint16_t base;
	uint32_t off;

	if (PCI_CAP_LOCATE(ppt->pptd_cfg, PCI_CAP_ID_MSI_X, &base) !=
	    DDI_SUCCESS)
		return (-1);

	off = pci_config_get32(ppt->pptd_cfg, base + PCI_MSIX_TBL_OFFSET);

	if (off == PCI_EINVAL32)
		return (-1);

	return (off & PCI_MSIX_TBL_BIR_MASK);
}

static int
ppt_devmap(dev_t dev, devmap_cookie_t dhp, offset_t off, size_t len,
    size_t *maplen, uint_t model)
{
	minor_t minor;
	struct pptdev *ppt;
	int err, bar;
	uint_t ddireg;

	minor = getminor(dev);

	if ((ppt = ddi_get_soft_state(ppt_state, minor)) == NULL)
		return (ENXIO);

#ifdef _MULTI_DATAMODEL
	if (ddi_model_convert_from(model) != DDI_MODEL_NONE)
		return (ENXIO);
#endif

	if (off < 0 || off != P2ALIGN(off, PAGESIZE))
		return (EINVAL);

	if ((bar = ppt_find_msix_table_bar(ppt)) == -1)
		return (EINVAL);

	ddireg = ppt->pptd_bars[bar].ddireg;

	if (ddireg == 0)
		return (EINVAL);

	err = devmap_devmem_setup(dhp, ppt->pptd_dip, NULL, ddireg, off, len,
	    PROT_USER | PROT_READ | PROT_WRITE, IOMEM_DATA_CACHED, &ppt_attr);

	if (err == DDI_SUCCESS)
		*maplen = len;

	return (err);
}

static void
ppt_bar_wipe(struct pptdev *ppt)
{
	uint_t i;

	for (i = 0; i < PCI_BASE_NUM; i++) {
		struct pptbar *pbar = &ppt->pptd_bars[i];
		if (pbar->type == PCI_ADDR_IO && pbar->io_handle != NULL) {
			ddi_regs_map_free(&pbar->io_handle);
		}
	}
	bzero(&ppt->pptd_bars, sizeof (ppt->pptd_bars));
}

static int
ppt_bar_crawl(struct pptdev *ppt)
{
	pci_regspec_t *regs;
	uint_t rcount, i;
	int err = 0, rlen;

	if (ddi_getlongprop(DDI_DEV_T_ANY, ppt->pptd_dip, DDI_PROP_DONTPASS,
	    "assigned-addresses", (caddr_t)&regs, &rlen) != DDI_PROP_SUCCESS) {
		return (EIO);
	}

	VERIFY3S(rlen, >, 0);
	rcount = rlen / sizeof (pci_regspec_t);
	for (i = 0; i < rcount; i++) {
		pci_regspec_t *reg = &regs[i];
		struct pptbar *pbar;
		uint_t bar, rnum;

		DTRACE_PROBE1(ppt__crawl__reg, pci_regspec_t *, reg);
		bar = PCI_REG_REG_G(reg->pci_phys_hi);
		if (!BAR_VALID(bar)) {
			continue;
		}

		rnum = BAR_TO_IDX(bar);
		pbar = &ppt->pptd_bars[rnum];
		/* is this somehow already populated? */
		if (pbar->base != 0 || pbar->size != 0) {
			err = EEXIST;
			break;
		}

		/*
		 * Register 0 corresponds to the PCI config space.
		 * The registers which match the assigned-addresses list are
		 * offset by 1.
		 */
		pbar->ddireg = i + 1;

		pbar->type = reg->pci_phys_hi & PCI_ADDR_MASK;
		pbar->base = ((uint64_t)reg->pci_phys_mid << 32) |
		    (uint64_t)reg->pci_phys_low;
		pbar->size = ((uint64_t)reg->pci_size_hi << 32) |
		    (uint64_t)reg->pci_size_low;
		if (pbar->type == PCI_ADDR_IO) {
			err = ddi_regs_map_setup(ppt->pptd_dip, rnum,
			    &pbar->io_ptr, 0, 0, &ppt_attr, &pbar->io_handle);
			if (err != 0) {
				break;
			}
		}
	}
	kmem_free(regs, rlen);

	if (err != 0) {
		ppt_bar_wipe(ppt);
	}
	return (err);
}

static boolean_t
ppt_bar_verify_mmio(struct pptdev *ppt, uint64_t base, uint64_t size)
{
	const uint64_t map_end = base + size;

	/* Zero-length or overflow mappings are not valid */
	if (map_end <= base) {
		return (B_FALSE);
	}
	/* MMIO bounds should be page-aligned */
	if ((base & PAGEOFFSET) != 0 || (size & PAGEOFFSET) != 0) {
		return (B_FALSE);
	}

	for (uint_t i = 0; i < PCI_BASE_NUM; i++) {
		const struct pptbar *bar = &ppt->pptd_bars[i];
		const uint64_t bar_end = bar->base + bar->size;

		/* Only memory BARs can be mapped */
		if (bar->type != PCI_ADDR_MEM32 &&
		    bar->type != PCI_ADDR_MEM64) {
			continue;
		}

		/* Does the mapping fit within this BAR? */
		if (base < bar->base || base >= bar_end ||
		    map_end < bar->base || map_end > bar_end) {
			continue;
		}

		/* This BAR satisfies the provided map */
		return (B_TRUE);
	}
	return (B_FALSE);
}

static boolean_t
ppt_toggle_bar(struct pptdev *ppt, boolean_t enable)
{
	/*
	 * Enable/disable bus mastering and BAR decoding based on the BAR
	 * configuration. Bhyve emulates the COMMAND register so we won't see
	 * the bits changing there.
	 */
	ddi_acc_handle_t hdl;
	uint16_t cmd;

	if (pci_config_setup(ppt->pptd_dip, &hdl) != DDI_SUCCESS)
		return (B_FALSE);
	cmd = pci_config_get16(hdl, PCI_CONF_COMM);

	if (enable) {
		cmd |= PCI_COMM_ME;

		for (uint_t i = 0; i < PCI_BASE_NUM; i++) {
			const struct pptbar *bar = &ppt->pptd_bars[i];

			switch (bar->type) {
			case PCI_ADDR_MEM32:
			case PCI_ADDR_MEM64:
				cmd |= PCI_COMM_MAE;
				break;
			case PCI_ADDR_IO:
				cmd |= PCI_COMM_IO;
				break;
			}
		}
	} else {
		cmd &= ~(PCI_COMM_ME | PCI_COMM_MAE | PCI_COMM_IO);
	}

	pci_config_put16(hdl, PCI_CONF_COMM, cmd);
	pci_config_teardown(&hdl);

	return (B_TRUE);
}

static int
ppt_ddi_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
	struct pptdev *ppt = NULL;
	char name[PPT_MAXNAMELEN];
	int inst;

	if (cmd != DDI_ATTACH)
		return (DDI_FAILURE);

	inst = ddi_get_instance(dip);

	if (ddi_soft_state_zalloc(ppt_state, inst) != DDI_SUCCESS) {
		goto fail;
	}
	VERIFY(ppt = ddi_get_soft_state(ppt_state, inst));
	ppt->pptd_dip = dip;
	ppt->intx.ioapic_irq = -1;
	ppt->intx.arg.ioapic_irq = -1;
	ddi_set_driver_private(dip, ppt);

	if (pci_config_setup(dip, &ppt->pptd_cfg) != DDI_SUCCESS) {
		goto fail;
	}
	if (ppt_bar_crawl(ppt) != 0) {
		goto fail;
	}
	if (pci_save_config_regs(dip) != DDI_SUCCESS) {
		goto fail;
	}
	if (ddi_create_minor_node(dip, PPT_MINOR_NAME, S_IFCHR, inst,
	    DDI_PSEUDO, 0) != DDI_SUCCESS) {
		goto fail;
	}

	ppt_toggle_bar(ppt, B_FALSE);

	mutex_enter(&pptdev_mtx);
	list_insert_tail(&pptdev_list, ppt);
	mutex_exit(&pptdev_mtx);

	return (DDI_SUCCESS);

fail:
	if (ppt != NULL) {
		ddi_remove_minor_node(dip, NULL);
		if (ppt->pptd_cfg != NULL) {
			pci_config_teardown(&ppt->pptd_cfg);
		}
		ppt_bar_wipe(ppt);
		ddi_soft_state_free(ppt_state, inst);
	}
	return (DDI_FAILURE);
}

static int
ppt_ddi_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
	struct pptdev *ppt;
	int inst;

	if (cmd != DDI_DETACH)
		return (DDI_FAILURE);

	ppt = ddi_get_driver_private(dip);
	inst = ddi_get_instance(dip);

	ASSERT3P(ddi_get_soft_state(ppt_state, inst), ==, ppt);

	mutex_enter(&pptdev_mtx);
	if (ppt->vm != NULL) {
		mutex_exit(&pptdev_mtx);
		return (DDI_FAILURE);
	}
	list_remove(&pptdev_list, ppt);
	mutex_exit(&pptdev_mtx);

	ddi_remove_minor_node(dip, PPT_MINOR_NAME);
	ppt_bar_wipe(ppt);
	pci_config_teardown(&ppt->pptd_cfg);
	ddi_set_driver_private(dip, NULL);
	ddi_soft_state_free(ppt_state, inst);

	return (DDI_SUCCESS);
}

static int
ppt_ddi_info(dev_info_t *dip, ddi_info_cmd_t cmd, void *arg, void **result)
{
	int error = DDI_FAILURE;
	int inst = getminor((dev_t)arg);

	switch (cmd) {
	case DDI_INFO_DEVT2DEVINFO: {
		struct pptdev *ppt = ddi_get_soft_state(ppt_state, inst);

		if (ppt != NULL) {
			*result = (void *)ppt->pptd_dip;
			error = DDI_SUCCESS;
		}
		break;
	}
	case DDI_INFO_DEVT2INSTANCE: {
		*result = (void *)(uintptr_t)inst;
		error = DDI_SUCCESS;
		break;
	}
	default:
		break;
	}
	return (error);
}

static struct cb_ops ppt_cb_ops = {
	ppt_open,
	nulldev,	/* close */
	nodev,		/* strategy */
	nodev,		/* print */
	nodev,		/* dump */
	nodev,		/* read */
	nodev,		/* write */
	ppt_ioctl,
	ppt_devmap,	/* devmap */
	NULL,		/* mmap */
	NULL,		/* segmap */
	nochpoll,	/* poll */
	ddi_prop_op,
	NULL,
	D_NEW | D_MP | D_64BIT | D_DEVMAP,
	CB_REV
};

static struct dev_ops ppt_ops = {
	DEVO_REV,
	0,
	ppt_ddi_info,
	nulldev,	/* identify */
	nulldev,	/* probe */
	ppt_ddi_attach,
	ppt_ddi_detach,
	nodev,		/* reset */
	&ppt_cb_ops,
	(struct bus_ops *)NULL
};

static struct modldrv modldrv = {
	&mod_driverops,
	"bhyve pci pass-thru",
	&ppt_ops
};

static struct modlinkage modlinkage = {
	MODREV_1,
	&modldrv,
	NULL
};

int
_init(void)
{
	int error;

	mutex_init(&pptdev_mtx, NULL, MUTEX_DRIVER, NULL);
	list_create(&pptdev_list, sizeof (struct pptdev),
	    offsetof(struct pptdev, pptd_node));

	error = ddi_soft_state_init(&ppt_state, sizeof (struct pptdev), 0);
	if (error) {
		goto fail;
	}

	error = mod_install(&modlinkage);

	ppt_major = ddi_name_to_major("ppt");
fail:
	if (error) {
		ddi_soft_state_fini(&ppt_state);
	}
	return (error);
}

int
_fini(void)
{
	int error;

	error = mod_remove(&modlinkage);
	if (error)
		return (error);
	ddi_soft_state_fini(&ppt_state);

	return (0);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&modlinkage, modinfop));
}

static boolean_t
ppt_wait_for_pending_txn(dev_info_t *dip, uint_t max_delay_us)
{
	uint16_t cap_ptr, devsts;
	ddi_acc_handle_t hdl;

	if (pci_config_setup(dip, &hdl) != DDI_SUCCESS)
		return (B_FALSE);

	if (PCI_CAP_LOCATE(hdl, PCI_CAP_ID_PCI_E, &cap_ptr) != DDI_SUCCESS) {
		pci_config_teardown(&hdl);
		return (B_FALSE);
	}

	devsts = PCI_CAP_GET16(hdl, 0, cap_ptr, PCIE_DEVSTS);
	while ((devsts & PCIE_DEVSTS_TRANS_PENDING) != 0) {
		if (max_delay_us == 0) {
			pci_config_teardown(&hdl);
			return (B_FALSE);
		}

		/* Poll once every 100 milliseconds up to the timeout. */
		if (max_delay_us > 100000) {
			delay(drv_usectohz(100000));
			max_delay_us -= 100000;
		} else {
			delay(drv_usectohz(max_delay_us));
			max_delay_us = 0;
		}
		devsts = PCI_CAP_GET16(hdl, 0, cap_ptr, PCIE_DEVSTS);
	}

	pci_config_teardown(&hdl);
	return (B_TRUE);
}

static uint_t
ppt_max_completion_tmo_us(dev_info_t *dip)
{
	uint_t timo = 0;
	uint16_t cap_ptr;
	ddi_acc_handle_t hdl;
	uint_t timo_ranges[] = {	/* timeout ranges */
		50000,		/* 50ms */
		100,		/* 100us */
		10000,		/* 10ms */
		0,
		0,
		55000,		/* 55ms */
		210000,		/* 210ms */
		0,
		0,
		900000,		/* 900ms */
		3500000,	/* 3.5s */
		0,
		0,
		13000000,	/* 13s */
		64000000,	/* 64s */
		0
	};

	if (pci_config_setup(dip, &hdl) != DDI_SUCCESS)
		return (50000); /* default 50ms */

	if (PCI_CAP_LOCATE(hdl, PCI_CAP_ID_PCI_E, &cap_ptr) != DDI_SUCCESS)
		goto out;

	if ((PCI_CAP_GET16(hdl, 0, cap_ptr, PCIE_PCIECAP) &
	    PCIE_PCIECAP_VER_MASK) < PCIE_PCIECAP_VER_2_0)
		goto out;

	if ((PCI_CAP_GET32(hdl, 0, cap_ptr, PCIE_DEVCAP2) &
	    PCIE_DEVCTL2_COM_TO_RANGE_MASK) == 0)
		goto out;

	timo = timo_ranges[PCI_CAP_GET16(hdl, 0, cap_ptr, PCIE_DEVCTL2) &
	    PCIE_DEVCAP2_COM_TO_RANGE_MASK];

out:
	if (timo == 0)
		timo = 50000; /* default 50ms */

	pci_config_teardown(&hdl);
	return (timo);
}

static boolean_t
ppt_wait_device_present_locked(struct pptdev *ppt)
{
	hrtime_t start;

	ASSERT(MUTEX_HELD(&pptdev_mtx));

	start = gethrtime();
	do {
		uint16_t vid = pci_config_get16(ppt->pptd_cfg, PCI_CONF_VENID);
		uint16_t did = pci_config_get16(ppt->pptd_cfg, PCI_CONF_DEVID);

		if (vid != 0xffff && did != 0xffff && vid != 0 && did != 0) {
			return (B_TRUE);
		}
		delay(drv_usectohz(LINK_POLL_INTERVAL_US));
	} while ((gethrtime() - start) <
	    (hrtime_t)USEC2NSEC(DEVICE_PRESENT_TIMEOUT_US));

	return (B_FALSE);
}

static boolean_t
ppt_wait_link_active(dev_info_t *dip)
{
	ddi_acc_handle_t hdl;
	uint16_t cap, lstat;
	hrtime_t start;

	if (pci_config_setup(dip, &hdl) != DDI_SUCCESS)
		return (B_FALSE);

	if (PCI_CAP_LOCATE(hdl, PCI_CAP_ID_PCI_E, &cap) != DDI_SUCCESS) {
		pci_config_teardown(&hdl);
		return (B_FALSE);
	}

	start = gethrtime();
	do {
		lstat = PCI_CAP_GET16(hdl, 0, cap, PCIE_LINKSTS);
		if (lstat & PCIE_LINKSTS_DLL_LINK_ACTIVE) {
			pci_config_teardown(&hdl);
			return (B_TRUE);
		}
		delay(drv_usectohz(LINK_POLL_INTERVAL_US));
	} while ((gethrtime() - start) < (hrtime_t)USEC2NSEC(LINK_POLL_TIMEOUT_US));

	pci_config_teardown(&hdl);
	return (B_FALSE);
}

static boolean_t
ppt_pm_reset(dev_info_t *dip, boolean_t force)
{
	uint16_t cap_ptr, csr, csr_before, csr_d3, csr_after;
	uint16_t cmd;
	uint16_t bdf;
	ddi_acc_handle_t hdl;

	if (pci_config_setup(dip, &hdl) != DDI_SUCCESS)
		return (B_FALSE);

	if (PCI_CAP_LOCATE(hdl, PCI_CAP_ID_PM, &cap_ptr) != DDI_SUCCESS) {
		pci_config_teardown(&hdl);
		return (B_FALSE);
	}

	cmd = pci_config_get16(hdl, PCI_CONF_COMM);
	cmd &= ~(PCI_COMM_ME | PCI_COMM_MAE | PCI_COMM_IO);
	pci_config_put16(hdl, PCI_CONF_COMM, cmd);

	csr = PCI_CAP_GET16(hdl, 0, cap_ptr, PCI_PMCSR);
	csr_before = csr;
	if ((csr & PCI_PMCSR_NO_SOFT_RESET) != 0 && !force) {
		pci_config_teardown(&hdl);
		return (B_FALSE);
	}

	if ((csr & PCI_PMCSR_STATE_MASK) != PCI_PMCSR_STATE_D0) {
		csr = (csr & ~PCI_PMCSR_STATE_MASK) | PCI_PMCSR_STATE_D0;
		(void) PCI_CAP_PUT16(hdl, 0, cap_ptr, PCI_PMCSR, csr);
		delay(drv_usectohz(10000));
		csr = PCI_CAP_GET16(hdl, 0, cap_ptr, PCI_PMCSR);
		if ((csr & PCI_PMCSR_STATE_MASK) != PCI_PMCSR_STATE_D0) {
			pci_config_teardown(&hdl);
			return (B_FALSE);
		}
	}

	csr = (csr & ~PCI_PMCSR_STATE_MASK) | PCI_PMCSR_STATE_D3HOT;
	(void) PCI_CAP_PUT16(hdl, 0, cap_ptr, PCI_PMCSR, csr);
	delay(drv_usectohz(100000));
	csr_d3 = PCI_CAP_GET16(hdl, 0, cap_ptr, PCI_PMCSR);

	csr = (csr & ~PCI_PMCSR_STATE_MASK) | PCI_PMCSR_STATE_D0;
	(void) PCI_CAP_PUT16(hdl, 0, cap_ptr, PCI_PMCSR, csr);
	delay(drv_usectohz(50000));
	csr_after = PCI_CAP_GET16(hdl, 0, cap_ptr, PCI_PMCSR);

	if (ppt_diag_enable != 0) {
		bdf = pci_get_bdf(dip);
		cmn_err(CE_NOTE, "ppt-reset-pm: bdf=0x%x force=%u "
		    "no_soft_reset=%u pmcsr_before=0x%x pmcsr_d3=0x%x "
		    "pmcsr_after=0x%x", bdf, force,
		    (csr_before & PCI_PMCSR_NO_SOFT_RESET) != 0,
		    csr_before, csr_d3, csr_after);
	}

	pci_config_teardown(&hdl);
	return (B_TRUE);
}

static void
ppt_bus_reset(dev_info_t *dip)
{
	dev_info_t *parent;
	uint16_t bctl;
	ddi_acc_handle_t hdl;

	parent = ddi_get_parent(dip);
	if (parent == NULL)
		return;

	if (pci_config_setup(parent, &hdl) != DDI_SUCCESS)
		return;

	bctl = pci_config_get16(hdl, PCI_BCNF_BCNTRL);
	bctl |= PCI_BCNF_BCNTRL_RESET;
	pci_config_put16(hdl, PCI_BCNF_BCNTRL, bctl);
	delay(drv_usectohz(100000));
	bctl &= ~PCI_BCNF_BCNTRL_RESET;
	pci_config_put16(hdl, PCI_BCNF_BCNTRL, bctl);

	pci_config_teardown(&hdl);
}

static boolean_t
ppt_flr(dev_info_t *dip, boolean_t force)
{
	uint16_t cap_ptr, ctl, cmd;
	ddi_acc_handle_t hdl;
	uint_t compl_delay = 0, max_delay_us;

	if (pci_config_setup(dip, &hdl) != DDI_SUCCESS)
		return (B_FALSE);

	if (PCI_CAP_LOCATE(hdl, PCI_CAP_ID_PCI_E, &cap_ptr) != DDI_SUCCESS)
		goto fail;

	if ((PCI_CAP_GET32(hdl, 0, cap_ptr, PCIE_DEVCAP) & PCIE_DEVCAP_FLR)
	    == 0)
		goto fail;

	max_delay_us = MAX(ppt_max_completion_tmo_us(dip), 10000);

	/*
	 * Disable busmastering to prevent generation of new transactions while
	 * waiting for the device to go idle.  If the idle timeout fails, the
	 * command register is restored which will re-enable busmastering.
	 */
	cmd = pci_config_get16(hdl, PCI_CONF_COMM);
	pci_config_put16(hdl, PCI_CONF_COMM, cmd & ~PCI_COMM_ME);
	if (!ppt_wait_for_pending_txn(dip, max_delay_us)) {
		if (!force) {
			pci_config_put16(hdl, PCI_CONF_COMM, cmd);
			goto fail;
		}
		dev_err(dip, CE_WARN,
		    "?Resetting with transactions pending after %u us\n",
		    max_delay_us);

		/*
		 * Extend the post-FLR delay to cover the maximum Completion
		 * Timeout delay of anything in flight during the FLR delay.
		 * Enforce a minimum delay of at least 10ms.
		 */
		compl_delay = MAX(10, (ppt_max_completion_tmo_us(dip) / 1000));
	}

	/* Initiate the reset. */
	ctl = PCI_CAP_GET16(hdl, 0, cap_ptr, PCIE_DEVCTL);
	(void) PCI_CAP_PUT16(hdl, 0, cap_ptr, PCIE_DEVCTL,
	    ctl | PCIE_DEVCTL_INITIATE_FLR);

	/* Wait for at least 100ms */
	delay(drv_usectohz((100 + compl_delay) * 1000));

	pci_config_teardown(&hdl);
	return (B_TRUE);

fail:
	/*
	 * Higher-level reset selection is responsible for trying an alternate
	 * reset method when FLR is unavailable or fails.
	 */
	pci_config_teardown(&hdl);
	return (B_FALSE);
}

static boolean_t
ppt_nvidia_gpu_bus_reset_quirk_active(struct pptdev *ppt)
{
	uint16_t bdf;
	uint16_t vid;
	uint16_t did;

	bdf = pci_get_bdf(ppt->pptd_dip);
	if ((bdf & 0x7u) != 0)
		return (B_FALSE);

	vid = pci_config_get16(ppt->pptd_cfg, PCI_CONF_VENID);
	did = pci_config_get16(ppt->pptd_cfg, PCI_CONF_DEVID);
	return (vid == 0x10de && did == 0x1e07);
}

static void
ppt_reset_capture_state(struct pptdev *ppt, struct ppt_reset_state *st)
{
	uint16_t cap;

	bzero(st, sizeof (*st));
	st->prs_bdf = pci_get_bdf(ppt->pptd_dip);
	st->prs_venid = pci_config_get16(ppt->pptd_cfg, PCI_CONF_VENID);
	st->prs_devid = pci_config_get16(ppt->pptd_cfg, PCI_CONF_DEVID);
	st->prs_present = (st->prs_venid != 0xffff);
	st->prs_cmd = pci_config_get16(ppt->pptd_cfg, PCI_CONF_COMM);
	st->prs_stat = pci_config_get16(ppt->pptd_cfg, PCI_CONF_STAT);
	st->prs_bar0 = pci_config_get32(ppt->pptd_cfg, PCI_CONF_BASE0);
	st->prs_bar1 = pci_config_get32(ppt->pptd_cfg, PCI_CONF_BASE1);
	st->prs_bar3 = pci_config_get32(ppt->pptd_cfg, PCI_CONF_BASE3);

	if (!st->prs_present)
		return;

	if (PCI_CAP_LOCATE(ppt->pptd_cfg, PCI_CAP_ID_PM, &cap) == DDI_SUCCESS) {
		st->prs_has_pm = B_TRUE;
		st->prs_pmcsr = PCI_CAP_GET16(ppt->pptd_cfg, 0, cap,
		    PCI_PMCSR);
	}

	if (PCI_CAP_LOCATE(ppt->pptd_cfg, PCI_CAP_ID_MSI, &cap) == DDI_SUCCESS) {
		st->prs_has_msi = B_TRUE;
		st->prs_msictl = PCI_CAP_GET16(ppt->pptd_cfg, 0, cap,
		    PCI_MSI_CTRL);
		st->prs_msiaddr_lo = PCI_CAP_GET32(ppt->pptd_cfg, 0, cap,
		    PCI_MSI_ADDR_OFFSET);
		if ((st->prs_msictl & PCI_MSI_64BIT_MASK) != 0) {
			st->prs_msiaddr_hi = PCI_CAP_GET32(ppt->pptd_cfg, 0,
			    cap, PCI_MSI_64BIT_ADDR);
			st->prs_msidata = PCI_CAP_GET16(ppt->pptd_cfg, 0,
			    cap, PCI_MSI_64BIT_DATA);
		} else {
			st->prs_msidata = PCI_CAP_GET16(ppt->pptd_cfg, 0,
			    cap, PCI_MSI_32BIT_DATA);
		}
	}

	if (PCI_CAP_LOCATE(ppt->pptd_cfg, PCI_CAP_ID_MSI_X, &cap) ==
	    DDI_SUCCESS) {
		st->prs_has_msix = B_TRUE;
		st->prs_msixctl = PCI_CAP_GET16(ppt->pptd_cfg, 0, cap,
		    PCI_MSIX_CTRL);
	}
}

static void
ppt_reset_log_one_state(struct pptdev *ppt, ppt_reset_type_t method,
    const char *phase)
{
	struct ppt_reset_state st;

	ppt_reset_capture_state(ppt, &st);
	cmn_err(CE_NOTE, "ppt-reset-state: phase=%s method=%u bdf=0x%x "
	    "present=%u ven=0x%x dev=0x%x cmd=0x%x stat=0x%x "
	    "bar0=0x%x bar1=0x%x bar3=0x%x pm=%u pmcsr=0x%x "
	    "msi=%u msictl=0x%x msiaddr=0x%x:%x msidata=0x%x "
	    "msix=%u msixctl=0x%x",
	    phase, method, st.prs_bdf, st.prs_present, st.prs_venid,
	    st.prs_devid, st.prs_cmd, st.prs_stat, st.prs_bar0,
	    st.prs_bar1, st.prs_bar3, st.prs_has_pm, st.prs_pmcsr,
	    st.prs_has_msi, st.prs_msictl, st.prs_msiaddr_hi,
	    st.prs_msiaddr_lo, st.prs_msidata, st.prs_has_msix,
	    st.prs_msixctl);
}

static void
ppt_reset_log_state_locked(struct pptdev *ppt, ppt_reset_type_t method,
    const char *phase)
{
	struct pptdev *peer;
	uint16_t base_bdf;

	ASSERT(MUTEX_HELD(&pptdev_mtx));

	if (ppt_diag_enable == 0)
		return;

	base_bdf = pci_get_bdf(ppt->pptd_dip) & ~0x7u;
	for (peer = list_head(&pptdev_list); peer != NULL;
	    peer = list_next(&pptdev_list, peer)) {
		if (method != PPT_RESET_BUS && peer != ppt)
			continue;
		if (method == PPT_RESET_BUS &&
		    ((pci_get_bdf(peer->pptd_dip) & ~0x7u) != base_bdf)) {
			continue;
		}
		ppt_reset_log_one_state(peer, method, phase);
	}
}

static int
ppt_save_reset_config_locked(struct pptdev *ppt, ppt_reset_type_t method)
{
	struct pptdev *peer;
	uint16_t base_bdf;

	ASSERT(MUTEX_HELD(&pptdev_mtx));

	base_bdf = pci_get_bdf(ppt->pptd_dip) & ~0x7u;

	for (peer = list_head(&pptdev_list); peer != NULL;
	    peer = list_next(&pptdev_list, peer)) {
		if (method != PPT_RESET_BUS && peer != ppt) {
			continue;
		}
		if (method == PPT_RESET_BUS &&
		    ((pci_get_bdf(peer->pptd_dip) & ~0x7u) != base_bdf)) {
			continue;
		}

		if (pci_save_config_regs(peer->pptd_dip) != DDI_SUCCESS) {
			cmn_err(CE_WARN, "ppt: reset save failed for "
			    "bdf=0x%x method=%u",
			    pci_get_bdf(peer->pptd_dip), method);
			return (EIO);
		}
	}

	return (0);
}

static int
ppt_restore_reset_config_locked(struct pptdev *ppt, ppt_reset_type_t method)
{
	struct pptdev *peer;
	uint16_t base_bdf;
	int err = 0;

	ASSERT(MUTEX_HELD(&pptdev_mtx));

	base_bdf = pci_get_bdf(ppt->pptd_dip) & ~0x7u;

	for (peer = list_head(&pptdev_list); peer != NULL;
	    peer = list_next(&pptdev_list, peer)) {
		if (method != PPT_RESET_BUS && peer != ppt) {
			continue;
		}
		if (method == PPT_RESET_BUS &&
		    ((pci_get_bdf(peer->pptd_dip) & ~0x7u) != base_bdf)) {
			continue;
		}

		if (!ppt_wait_device_present_locked(peer)) {
			cmn_err(CE_WARN, "ppt: reset restore timed out waiting "
			    "for bdf=0x%x method=%u",
			    pci_get_bdf(peer->pptd_dip), method);
			err = EIO;
		}
		ppt_reset_pci_power_state(peer->pptd_dip);
		if (pci_restore_config_regs(peer->pptd_dip) != DDI_SUCCESS) {
			cmn_err(CE_WARN, "ppt: reset restore failed for "
			    "bdf=0x%x method=%u",
			    pci_get_bdf(peer->pptd_dip), method);
			err = EIO;
			continue;
		}
		/*
		 * pci_restore_config_regs() consumes the saved config property.
		 * Keep a fresh host-state image for later manual resets and for
		 * the assignment path.
		 */
		if (pci_save_config_regs(peer->pptd_dip) != DDI_SUCCESS) {
			cmn_err(CE_WARN, "ppt: reset resave failed for "
			    "bdf=0x%x method=%u",
			    pci_get_bdf(peer->pptd_dip), method);
			err = EIO;
		}
		(void) ppt_wait_link_active(peer->pptd_dip);
	}

	return (err);
}

static int
ppt_reset_run_locked(struct pptdev *ppt, ppt_reset_type_t want_method,
    ppt_reset_flags_t flags, ppt_reset_type_t *actual_methodp)
{
	ppt_reset_type_t method = PPT_RESET_NONE;
	boolean_t ok = B_FALSE;
	boolean_t force = (flags & PPT_RESET_F_FORCE) != 0;
	uint_t func;

	func = pci_get_bdf(ppt->pptd_dip) & 0x7u;

	ppt_reset_log_state_locked(ppt, want_method, "before");

	/*
	 * Save all sibling functions before any manual reset.  A requested FLR
	 * or auto reset can fall back to a secondary-bus reset, and the bus
	 * reset path must have valid saved config for every affected function.
	 */
	if (ppt_save_reset_config_locked(ppt, PPT_RESET_BUS) != 0)
		return (EIO);

	switch (want_method) {
	case PPT_RESET_FLR:
		ok = ppt_flr(ppt->pptd_dip, B_TRUE);
		method = PPT_RESET_FLR;
		if (!ok && (flags & PPT_RESET_F_ALLOW_FALLBACK) != 0) {
			ok = ppt_pm_reset(ppt->pptd_dip, force);
			method = ok ? PPT_RESET_PM : method;
			if (!ok && func == 0) {
				ppt_bus_reset(ppt->pptd_dip);
				ok = B_TRUE;
				method = PPT_RESET_BUS;
			}
		}
		break;
	case PPT_RESET_PM:
		ok = ppt_pm_reset(ppt->pptd_dip, force);
		method = PPT_RESET_PM;
		break;
	case PPT_RESET_BUS:
		ppt_bus_reset(ppt->pptd_dip);
		ok = B_TRUE;
		method = PPT_RESET_BUS;
		break;
	case PPT_RESET_NONE:
		if (ppt_nvidia_gpu_bus_reset_quirk_active(ppt)) {
			ppt_bus_reset(ppt->pptd_dip);
			ok = B_TRUE;
			method = PPT_RESET_BUS;
			break;
		}
		ok = ppt_flr(ppt->pptd_dip, B_TRUE);
		method = PPT_RESET_FLR;
		if (!ok && (flags & PPT_RESET_F_ALLOW_FALLBACK) != 0) {
			ok = ppt_pm_reset(ppt->pptd_dip, force);
			method = ok ? PPT_RESET_PM : method;
			if (!ok && func == 0) {
				ppt_bus_reset(ppt->pptd_dip);
				ok = B_TRUE;
				method = PPT_RESET_BUS;
			}
		}
		break;
	default:
		return (EINVAL);
	}

	if (!ok) {
		ppt_reset_log_state_locked(ppt, method, "failed");
		if (actual_methodp != NULL)
			*actual_methodp = method;
		return (EIO);
	}

	ppt_reset_log_state_locked(ppt, method, "after-reset");

	/*
	 * A manual reset can run before assignment.  Leave the affected
	 * function(s) in the saved host configuration so ppt_assign_device()
	 * does not snapshot a reset-default config image as its pristine state.
	 * A secondary-bus reset affects every function in the device.
	 */
	if (ppt_restore_reset_config_locked(ppt, method) != 0) {
		if (actual_methodp != NULL)
			*actual_methodp = method;
		return (EIO);
	}
	ppt_reset_log_state_locked(ppt, method, "after-restore");

	if (actual_methodp != NULL)
		*actual_methodp = method;
	return (0);
}

static int
ppt_reset_device_method_locked(struct pptdev *ppt, ppt_reset_flags_t flags,
    ppt_reset_type_t want_method, ppt_reset_type_t *actual_methodp)
{
	int err;

	err = ppt_reset_run_locked(ppt, want_method, flags, actual_methodp);
	if (err != 0) {
		cmn_err(CE_WARN, "ppt_reset_device: bdf=0x%x failed method=%u err=%d",
		    pci_get_bdf(ppt->pptd_dip),
		    actual_methodp != NULL ? *actual_methodp : PPT_RESET_NONE, err);
	} else if (ppt_diag_enable != 0) {
		cmn_err(CE_NOTE, "ppt_reset_device: bdf=0x%x complete method=%u",
		    pci_get_bdf(ppt->pptd_dip),
		    actual_methodp != NULL ? *actual_methodp : PPT_RESET_NONE);
	}
	return (err);
}

static int
ppt_findf(struct vm *vm, int fd, struct pptdev **pptp)
{
	struct pptdev *ppt = NULL;
	file_t *fp;
	vattr_t va;
	int err = 0;

	ASSERT(MUTEX_HELD(&pptdev_mtx));

	if ((fp = getf(fd)) == NULL)
		return (EBADF);

	va.va_mask = AT_RDEV;
	if (VOP_GETATTR(fp->f_vnode, &va, NO_FOLLOW, fp->f_cred, NULL) != 0 ||
	    getmajor(va.va_rdev) != ppt_major) {
		err = EBADF;
		goto fail;
	}

	ppt = ddi_get_soft_state(ppt_state, getminor(va.va_rdev));

	if (ppt == NULL) {
		err = EBADF;
		goto fail;
	}

	if (ppt->vm != vm) {
		err = EBUSY;
		goto fail;
	}

	*pptp = ppt;
	return (0);

fail:
	releasef(fd);
	return (err);
}

static void
ppt_unmap_all_mmio(struct vm *vm, struct pptdev *ppt)
{
	int i;
	struct pptseg *seg;

	for (i = 0; i < MAX_MMIOSEGS; i++) {
		seg = &ppt->mmio[i];
		if (seg->len == 0)
			continue;
		(void) vm_unmap_mmio(vm, seg->gpa, seg->len);
		bzero(seg, sizeof (struct pptseg));
	}
}

static void
ppt_teardown_msi(struct pptdev *ppt)
{
	int i;
	uint16_t bdf;

	if (ppt->msi.num_msgs == 0)
		return;

	bdf = pci_get_bdf(ppt->pptd_dip);
	if (ppt_diag_enable != 0) {
		cmn_err(CE_NOTE, "ppt: teardown_msi bdf=0x%x msgs=%d "
		    "is_fixed=%d intr_count=%llu setup_count=%llu", bdf,
		    ppt->msi.num_msgs, ppt->msi.is_fixed ? 1 : 0,
		    (u_longlong_t)ppt->msi.intr_count,
		    (u_longlong_t)ppt->msi.setup_count);
	}

	for (i = 0; i < ppt->msi.num_msgs; i++) {
		int intr_cap;

		(void) ddi_intr_get_cap(ppt->msi.inth[i], &intr_cap);
		if ((intr_cap & DDI_INTR_FLAG_BLOCK) &&
		    ppt->msi.num_msgs > 1)
			ddi_intr_block_disable(&ppt->msi.inth[i], 1);
		else
			ddi_intr_disable(ppt->msi.inth[i]);

		ddi_intr_remove_handler(ppt->msi.inth[i]);
		ddi_intr_free(ppt->msi.inth[i]);

		ppt->msi.inth[i] = NULL;
	}

	kmem_free(ppt->msi.inth, ppt->msi.inth_sz);
	ppt->msi.inth = NULL;
	ppt->msi.inth_sz = 0;
	ppt->msi.is_fixed = B_FALSE;
	ppt->msi.intr_count = 0;

	ppt->msi.num_msgs = 0;
}

static void
ppt_teardown_msix_intr(struct pptdev *ppt, int idx)
{
	if (ppt->msix.inth != NULL && ppt->msix.inth[idx] != NULL) {
		int intr_cap;

		(void) ddi_intr_get_cap(ppt->msix.inth[idx], &intr_cap);
		if ((intr_cap & DDI_INTR_FLAG_BLOCK) &&
		    ppt->msix.num_msgs > 1)
			ddi_intr_block_disable(&ppt->msix.inth[idx], 1);
		else
			ddi_intr_disable(ppt->msix.inth[idx]);

		ddi_intr_remove_handler(ppt->msix.inth[idx]);
	}
}

static void
ppt_teardown_msix(struct pptdev *ppt)
{
	uint_t i;

	if (ppt->msix.num_msgs == 0)
		return;

	for (i = 0; i < ppt->msix.num_msgs; i++)
		ppt_teardown_msix_intr(ppt, i);

	if (ppt->msix.inth) {
		for (i = 0; i < ppt->msix.num_msgs; i++)
			ddi_intr_free(ppt->msix.inth[i]);
		kmem_free(ppt->msix.inth, ppt->msix.inth_sz);
		ppt->msix.inth = NULL;
		ppt->msix.inth_sz = 0;
		kmem_free(ppt->msix.arg, ppt->msix.arg_sz);
		ppt->msix.arg = NULL;
		ppt->msix.arg_sz = 0;
	}

	ppt->msix.num_msgs = 0;
}

static void
ppt_teardown_intx(struct pptdev *ppt)
{
	int intr_cap;

	if (ppt->intx.inth != NULL) {
		(void) ddi_intr_get_cap(ppt->intx.inth, &intr_cap);
		if (intr_cap & DDI_INTR_FLAG_BLOCK)
			(void) ddi_intr_block_disable(&ppt->intx.inth, 1);
		else
			(void) ddi_intr_disable(ppt->intx.inth);
		(void) ddi_intr_remove_handler(ppt->intx.inth);
		(void) ddi_intr_free(ppt->intx.inth);
		ppt->intx.inth = NULL;
	}
	ppt->intx.enabled = B_FALSE;
	ppt->intx.ioapic_irq = -1;
	ppt->intx.arg.pptdev = NULL;
	ppt->intx.arg.ioapic_irq = -1;
}

int
ppt_assigned_devices(struct vm *vm)
{
	struct pptdev *ppt;
	uint_t num = 0;

	mutex_enter(&pptdev_mtx);
	for (ppt = list_head(&pptdev_list); ppt != NULL;
	    ppt = list_next(&pptdev_list, ppt)) {
		if (ppt->vm == vm) {
			num++;
		}
	}
	mutex_exit(&pptdev_mtx);
	return (num);
}

boolean_t
ppt_is_mmio(struct vm *vm, vm_paddr_t gpa)
{
	struct pptdev *ppt = list_head(&pptdev_list);

	/* XXX: this should probably be restructured to avoid the lock */
	mutex_enter(&pptdev_mtx);
	for (ppt = list_head(&pptdev_list); ppt != NULL;
	    ppt = list_next(&pptdev_list, ppt)) {
		if (ppt->vm != vm) {
			continue;
		}

		for (uint_t i = 0; i < MAX_MMIOSEGS; i++) {
			struct pptseg *seg = &ppt->mmio[i];

			if (seg->len == 0)
				continue;
			if (gpa >= seg->gpa && gpa < seg->gpa + seg->len) {
				mutex_exit(&pptdev_mtx);
				return (B_TRUE);
			}
		}
	}

	mutex_exit(&pptdev_mtx);
	return (B_FALSE);
}

int
ppt_assign_device(struct vm *vm, int pptfd)
{
	struct pptdev *ppt = NULL;
	int err = 0;

	mutex_enter(&pptdev_mtx);
	/* Passing NULL requires the device to be unowned. */
	err = ppt_findf(NULL, pptfd, &ppt);
	if (err != 0) {
		mutex_exit(&pptdev_mtx);
		return (err);
	}

	if (pci_save_config_regs(ppt->pptd_dip) != DDI_SUCCESS) {
		err = EIO;
		goto done;
	}
	if (ppt_diag_enable != 0) {
		cmn_err(CE_NOTE, "ppt: assign enter bdf=0x%x vm=%p pptfd=%d",
		    pci_get_bdf(ppt->pptd_dip), (void *)vm, pptfd);
	}
	ppt_flr(ppt->pptd_dip, B_TRUE);

	/*
	 * Restore the device state after reset and then perform another save
	 * so the "pristine" state can be restored when the device is removed
	 * from the guest.
	 */
	if (pci_restore_config_regs(ppt->pptd_dip) != DDI_SUCCESS ||
	    pci_save_config_regs(ppt->pptd_dip) != DDI_SUCCESS) {
		err = EIO;
		goto done;
	}

	ppt_toggle_bar(ppt, B_TRUE);

	ppt->vm = vm;
	iommu_remove_device(iommu_host_domain(), pci_get_bdf(ppt->pptd_dip));
	iommu_add_device(vm_iommu_domain(vm), pci_get_bdf(ppt->pptd_dip));
	pf_set_passthru(ppt->pptd_dip, B_TRUE);

done:
	if (ppt_diag_enable != 0 || err != 0) {
		cmn_err(err == 0 ? CE_NOTE : CE_WARN, "ppt: assign done "
		    "bdf=0x%x vm=%p pptfd=%d err=%d",
		    pci_get_bdf(ppt->pptd_dip), (void *)vm, pptfd, err);
	}
	releasef(pptfd);
	mutex_exit(&pptdev_mtx);
	return (err);
}

static void
ppt_reset_pci_power_state(dev_info_t *dip)
{
	ddi_acc_handle_t cfg;
	uint16_t cap_ptr;

	if (pci_config_setup(dip, &cfg) != DDI_SUCCESS)
		return;

	if (PCI_CAP_LOCATE(cfg, PCI_CAP_ID_PM, &cap_ptr) == DDI_SUCCESS) {
		uint16_t val;

		val = PCI_CAP_GET16(cfg, 0, cap_ptr, PCI_PMCSR);
		if ((val & PCI_PMCSR_STATE_MASK) != PCI_PMCSR_D0) {
			val = (val & ~PCI_PMCSR_STATE_MASK) | PCI_PMCSR_D0;
			(void) PCI_CAP_PUT16(cfg, 0, cap_ptr, PCI_PMCSR,
			    val);
		}
	}

	pci_config_teardown(&cfg);
}

static void
ppt_unassign_log_one_state(struct pptdev *ppt, const char *phase)
{
	struct ppt_reset_state st;

	ppt_reset_capture_state(ppt, &st);
	cmn_err(CE_NOTE, "ppt-unassign-state: phase=%s bdf=0x%x vm=%p "
	    "present=%u ven=0x%x dev=0x%x cmd=0x%x stat=0x%x "
	    "bar0=0x%x bar1=0x%x bar3=0x%x pm=%u pmcsr=0x%x "
	    "msi=%u msictl=0x%x msiaddr=0x%x:%x msidata=0x%x "
	    "msix=%u msixctl=0x%x host_msi_msgs=%d host_msix_msgs=%d "
	    "intx=%u",
	    phase, st.prs_bdf, (void *)ppt->vm, st.prs_present,
	    st.prs_venid, st.prs_devid, st.prs_cmd, st.prs_stat,
	    st.prs_bar0, st.prs_bar1, st.prs_bar3, st.prs_has_pm,
	    st.prs_pmcsr, st.prs_has_msi, st.prs_msictl,
	    st.prs_msiaddr_hi, st.prs_msiaddr_lo, st.prs_msidata,
	    st.prs_has_msix, st.prs_msixctl, ppt->msi.num_msgs,
	    ppt->msix.num_msgs, ppt->intx.enabled ? 1 : 0);
}

static void
ppt_unassign_log_vm_state_locked(struct vm *vm, const char *phase)
{
	struct pptdev *peer;

	ASSERT(MUTEX_HELD(&pptdev_mtx));

	if (ppt_unassign_preremove_diag_enable == 0)
		return;

	for (peer = list_head(&pptdev_list); peer != NULL;
	    peer = list_next(&pptdev_list, peer)) {
		if (peer->vm == vm)
			ppt_unassign_log_one_state(peer, phase);
	}
}

static void
ppt_unassign_disable_config_intrs(struct pptdev *ppt)
{
	uint16_t cap;

	if (PCI_CAP_LOCATE(ppt->pptd_cfg, PCI_CAP_ID_MSI, &cap) ==
	    DDI_SUCCESS) {
		uint16_t ctl = PCI_CAP_GET16(ppt->pptd_cfg, 0, cap,
		    PCI_MSI_CTRL);

		if ((ctl & PCI_MSI_ENABLE_BIT) != 0) {
			(void) PCI_CAP_PUT16(ppt->pptd_cfg, 0, cap,
			    PCI_MSI_CTRL, ctl & ~PCI_MSI_ENABLE_BIT);
		}
	}

	if (PCI_CAP_LOCATE(ppt->pptd_cfg, PCI_CAP_ID_MSI_X, &cap) ==
	    DDI_SUCCESS) {
		uint16_t ctl = PCI_CAP_GET16(ppt->pptd_cfg, 0, cap,
		    PCI_MSIX_CTRL);

		if ((ctl & PCI_MSIX_ENABLE_BIT) != 0) {
			(void) PCI_CAP_PUT16(ppt->pptd_cfg, 0, cap,
			    PCI_MSIX_CTRL, ctl & ~PCI_MSIX_ENABLE_BIT);
		}
	}
}

static void
ppt_unassign_quiesce_one_locked(struct pptdev *ppt, const char *phase)
{
	uint16_t cmd;
	uint16_t bdf = pci_get_bdf(ppt->pptd_dip);

	ASSERT(MUTEX_HELD(&pptdev_mtx));

	if (ppt_unassign_preremove_diag_enable != 0)
		ppt_unassign_log_one_state(ppt, phase);

	cmd = pci_config_get16(ppt->pptd_cfg, PCI_CONF_COMM);
	cmd &= ~(PCI_COMM_ME | PCI_COMM_MAE | PCI_COMM_IO);
	pci_config_put16(ppt->pptd_cfg, PCI_CONF_COMM, cmd);
	ppt_unassign_disable_config_intrs(ppt);

	ppt_teardown_msi(ppt);
	ppt_teardown_msix(ppt);
	ppt_teardown_intx(ppt);

	if (ppt_unassign_preremove_diag_enable != 0) {
		cmn_err(CE_NOTE, "ppt-unassign-state: phase=%s-done "
		    "bdf=0x%x", phase, bdf);
		ppt_unassign_log_one_state(ppt, "post-quiesce");
	}
}

static void
ppt_unassign_quiesce_vm_locked(struct vm *vm)
{
	struct pptdev *peer;

	ASSERT(MUTEX_HELD(&pptdev_mtx));

	for (peer = list_head(&pptdev_list); peer != NULL;
	    peer = list_next(&pptdev_list, peer)) {
		if (peer->vm == vm)
			ppt_unassign_quiesce_one_locked(peer, "allfunc-quiesce");
	}
}

static void
ppt_do_unassign(struct pptdev *ppt)
{
	struct vm *vm = ppt->vm;
	uint16_t cmd;
	uint16_t bdf = pci_get_bdf(ppt->pptd_dip);

	ASSERT3P(vm, !=, NULL);
	ASSERT(MUTEX_HELD(&pptdev_mtx));

	if (ppt_unassign_diag_enable != 0) {
		cmn_err(CE_NOTE, "ppt: unassign step=start bdf=0x%x vm=%p",
		    bdf, (void *)vm);
	}

	/*
	 * VM destruction must not block behind a device reset while holding the
	 * global ppt mutex.  Quiesce host-visible decode and interrupts here;
	 * the next assignment path performs the selected reset method before the
	 * device is handed to another guest.
	 */
	cmd = pci_config_get16(ppt->pptd_cfg, PCI_CONF_COMM);
	cmd &= ~(PCI_COMM_ME | PCI_COMM_MAE | PCI_COMM_IO);
	pci_config_put16(ppt->pptd_cfg, PCI_CONF_COMM, cmd);
	if (ppt_unassign_diag_enable != 0) {
		cmn_err(CE_NOTE, "ppt: unassign step=decode-off bdf=0x%x",
		    bdf);
	}

	ppt_teardown_msi(ppt);
	if (ppt_unassign_diag_enable != 0) {
		cmn_err(CE_NOTE, "ppt: unassign step=msi-done bdf=0x%x",
		    bdf);
	}
	ppt_teardown_msix(ppt);
	if (ppt_unassign_diag_enable != 0) {
		cmn_err(CE_NOTE, "ppt: unassign step=msix-done bdf=0x%x",
		    bdf);
	}
	ppt_teardown_intx(ppt);
	if (ppt_unassign_diag_enable != 0) {
		cmn_err(CE_NOTE, "ppt: unassign step=intx-done bdf=0x%x",
		    bdf);
	}

	if (ppt_unassign_allfunc_quiesce != 0) {
		ppt_unassign_quiesce_vm_locked(vm);
		if (ppt_unassign_diag_enable != 0) {
			cmn_err(CE_NOTE, "ppt: unassign step=allfunc-quiesce "
			    "bdf=0x%x", bdf);
		}
	}

	/*
	 * Optional teardown quiesce path: after guest-visible decode and
	 * interrupts are quiesced, issue an FLR on function 0 before removing
	 * the device from the guest IOMMU domain.
	 */
	if (ppt_unassign_flr_quiesce != 0 && PCI_RID2FUNC(bdf) == 0) {
		ppt_flr(ppt->pptd_dip, B_TRUE);
		if (ppt_unassign_diag_enable != 0) {
			cmn_err(CE_NOTE,
			    "ppt: unassign step=flr-quiesce bdf=0x%x", bdf);
		}
	}

	ppt_unmap_all_mmio(vm, ppt);
	if (ppt_unassign_diag_enable != 0) {
		cmn_err(CE_NOTE, "ppt: unassign step=mmio-unmapped bdf=0x%x",
		    bdf);
	}

	ppt_unassign_log_vm_state_locked(vm, "pre-iommu-remove");
	iommu_remove_device(vm_iommu_domain(vm), pci_get_bdf(ppt->pptd_dip));
	if (ppt_unassign_diag_enable != 0) {
		cmn_err(CE_NOTE, "ppt: unassign step=iommu-vm-removed "
		    "bdf=0x%x", bdf);
	}
	iommu_add_device(iommu_host_domain(), pci_get_bdf(ppt->pptd_dip));
	if (ppt_unassign_diag_enable != 0) {
		cmn_err(CE_NOTE, "ppt: unassign step=iommu-host-added "
		    "bdf=0x%x", bdf);
	}
	pf_set_passthru(ppt->pptd_dip, B_FALSE);
	if (ppt_unassign_diag_enable != 0) {
		cmn_err(CE_NOTE, "ppt: unassign step=passthru-off bdf=0x%x",
		    bdf);
	}

	/*
	 * Restore from the state saved during device assignment.  If the device
	 * power state has been altered, bring it back to D0 first because the
	 * transition itself may reset config state.
	 */
	ppt_reset_pci_power_state(ppt->pptd_dip);
	if (ppt_unassign_diag_enable != 0) {
		cmn_err(CE_NOTE, "ppt: unassign step=d0 bdf=0x%x", bdf);
	}
	(void) pci_restore_config_regs(ppt->pptd_dip);
	if (ppt_unassign_diag_enable != 0) {
		cmn_err(CE_NOTE, "ppt: unassign step=config-restored "
		    "bdf=0x%x", bdf);
	}
	ppt->vm = NULL;
	if (ppt_unassign_diag_enable != 0) {
		cmn_err(CE_NOTE, "ppt: unassign step=done bdf=0x%x", bdf);
	}
}

int
ppt_unassign_device(struct vm *vm, int pptfd)
{
	struct pptdev *ppt;
	int err = 0;

	mutex_enter(&pptdev_mtx);
	err = ppt_findf(vm, pptfd, &ppt);
	if (err != 0) {
		mutex_exit(&pptdev_mtx);
		return (err);
	}

	ppt_do_unassign(ppt);

	releasef(pptfd);
	mutex_exit(&pptdev_mtx);
	return (err);
}

void
ppt_unassign_all(struct vm *vm)
{
	struct pptdev *ppt;

	mutex_enter(&pptdev_mtx);
	if (ppt_unassign_diag_enable != 0) {
		cmn_err(CE_NOTE, "ppt: unassign_all enter vm=%p",
		    (void *)vm);
	}
	for (ppt = list_head(&pptdev_list); ppt != NULL;
	    ppt = list_next(&pptdev_list, ppt)) {
		if (ppt->vm == vm) {
			ppt_do_unassign(ppt);
		}
	}
	if (ppt_unassign_diag_enable != 0) {
		cmn_err(CE_NOTE, "ppt: unassign_all exit vm=%p",
		    (void *)vm);
	}
	mutex_exit(&pptdev_mtx);
}

int
ppt_map_mmio(struct vm *vm, int pptfd, vm_paddr_t gpa, size_t len,
    vm_paddr_t hpa)
{
	struct pptdev *ppt;
	int err = 0;

	if ((len & PAGEOFFSET) != 0 || len == 0 || (gpa & PAGEOFFSET) != 0 ||
	    (hpa & PAGEOFFSET) != 0 || gpa + len < gpa || hpa + len < hpa) {
		cmn_err(CE_NOTE, "ppt: map_mmio reject vm=%p pptfd=%d "
		    "gpa=0x%llx len=0x%llx hpa=0x%llx err=%d", (void *)vm,
		    pptfd, (u_longlong_t)gpa, (u_longlong_t)len,
		    (u_longlong_t)hpa, EINVAL);
		return (EINVAL);
	}

	mutex_enter(&pptdev_mtx);
	err = ppt_findf(vm, pptfd, &ppt);
	if (err != 0) {
		mutex_exit(&pptdev_mtx);
		return (err);
	}

	/*
	 * Ensure that the host-physical range of the requested mapping fits
	 * within one of the MMIO BARs of the device.
	 */
	if (!ppt_bar_verify_mmio(ppt, hpa, len)) {
		cmn_err(CE_NOTE, "ppt: map_mmio verify_fail bdf=0x%x vm=%p "
		    "pptfd=%d gpa=0x%llx len=0x%llx hpa=0x%llx",
		    pci_get_bdf(ppt->pptd_dip), (void *)vm, pptfd,
		    (u_longlong_t)gpa, (u_longlong_t)len, (u_longlong_t)hpa);
		err = EINVAL;
		goto done;
	}

	for (uint_t i = 0; i < MAX_MMIOSEGS; i++) {
		struct pptseg *seg = &ppt->mmio[i];

		if (seg->len == 0) {
			if (ppt_diag_enable != 0) {
				cmn_err(CE_NOTE, "ppt: map_mmio enter "
				    "bdf=0x%x vm=%p pptfd=%d gpa=0x%llx "
				    "len=0x%llx hpa=0x%llx seg=%u",
				    pci_get_bdf(ppt->pptd_dip), (void *)vm,
				    pptfd, (u_longlong_t)gpa,
				    (u_longlong_t)len, (u_longlong_t)hpa, i);
			}
			err = vm_map_mmio(vm, gpa, len, hpa);
			if (err == 0) {
				seg->gpa = gpa;
				seg->len = len;
			}
			if (ppt_diag_enable != 0 || err != 0) {
				cmn_err(err == 0 ? CE_NOTE : CE_WARN,
				    "ppt: map_mmio done bdf=0x%x vm=%p "
				    "pptfd=%d seg=%u err=%d",
				    pci_get_bdf(ppt->pptd_dip), (void *)vm,
				    pptfd, i, err);
			}
			goto done;
		}
	}
	err = ENOSPC;
	cmn_err(CE_NOTE, "ppt: map_mmio nospc bdf=0x%x vm=%p pptfd=%d "
	    "gpa=0x%llx len=0x%llx hpa=0x%llx", pci_get_bdf(ppt->pptd_dip),
	    (void *)vm, pptfd, (u_longlong_t)gpa, (u_longlong_t)len,
	    (u_longlong_t)hpa);

done:
	releasef(pptfd);
	mutex_exit(&pptdev_mtx);
	return (err);
}

int
ppt_unmap_mmio(struct vm *vm, int pptfd, vm_paddr_t gpa, size_t len)
{
	struct pptdev *ppt;
	int err = 0;
	uint_t i;

	mutex_enter(&pptdev_mtx);
	err = ppt_findf(vm, pptfd, &ppt);
	if (err != 0) {
		mutex_exit(&pptdev_mtx);
		return (err);
	}

	for (i = 0; i < MAX_MMIOSEGS; i++) {
		struct pptseg *seg = &ppt->mmio[i];

		if (seg->gpa == gpa && seg->len == len) {
			err = vm_unmap_mmio(vm, seg->gpa, seg->len);
			if (err == 0) {
				seg->gpa = 0;
				seg->len = 0;
			}
			goto out;
		}
	}
	err = ENOENT;
out:
	releasef(pptfd);
	mutex_exit(&pptdev_mtx);
	return (err);
}

static uint_t
pptintr(caddr_t arg, caddr_t unused)
{
	struct pptintr_arg *pptarg = (struct pptintr_arg *)arg;
	struct pptdev *ppt = pptarg->pptdev;
	uint64_t intr_count;
	uint16_t bdf;

	intr_count = ++ppt->msi.intr_count;
	bdf = pci_get_bdf(ppt->pptd_dip);
	if (ppt_diag_enable != 0 &&
	    (intr_count <= 8 || (intr_count & (intr_count - 1)) == 0)) {
		cmn_err(CE_NOTE, "ppt: intr bdf=0x%x count=%llu is_fixed=%d "
		    "addr=0x%llx msg=0x%llx vm=%p", bdf,
		    (u_longlong_t)intr_count, ppt->msi.is_fixed ? 1 : 0,
		    (u_longlong_t)pptarg->addr, (u_longlong_t)pptarg->msg_data,
		    (void *)ppt->vm);
	}

	if (ppt->vm != NULL) {
		lapic_intr_msi(ppt->vm, pptarg->addr, pptarg->msg_data);
	} else {
		/*
		 * XXX
		 * This is not expected to happen - panic?
		 */
	}

	/*
	 * For legacy interrupts give other filters a chance in case
	 * the interrupt was not generated by the passthrough device.
	 */
	return (ppt->msi.is_fixed ? DDI_INTR_UNCLAIMED : DDI_INTR_CLAIMED);
}

static uint_t
pptintr_intx(caddr_t arg, caddr_t unused)
{
	struct pptintx_arg *intxarg = (struct pptintx_arg *)arg;
	struct pptdev *ppt = intxarg->pptdev;

	if (ppt->vm != NULL && ppt->intx.enabled)
		(void) vioapic_pulse_irq(ppt->vm, intxarg->ioapic_irq);
	return (DDI_INTR_UNCLAIMED);
}

int
ppt_setup_msi(struct vm *vm, int vcpu, int pptfd, uint64_t addr, uint64_t msg,
    int numvec)
{
	int i, msi_count, intr_type;
	struct pptdev *ppt;
	int err = 0;
	boolean_t same_request = B_TRUE;
	uint16_t bdf = 0;

	if (numvec < 0 || numvec > MAX_MSIMSGS)
		return (EINVAL);

	mutex_enter(&pptdev_mtx);
	err = ppt_findf(vm, pptfd, &ppt);
	if (err != 0) {
		mutex_exit(&pptdev_mtx);
		return (err);
	}

	bdf = pci_get_bdf(ppt->pptd_dip);
	if (ppt_diag_enable != 0) {
		cmn_err(CE_NOTE, "ppt: setup_msi enter bdf=0x%x vcpu=%d "
		    "numvec=%d addr=0x%llx msg=0x%llx existing_msgs=%d "
		    "is_fixed=%d setup_count=%llu intr_count=%llu",
		    bdf, vcpu, numvec, (u_longlong_t)addr, (u_longlong_t)msg,
		    ppt->msi.num_msgs, ppt->msi.is_fixed ? 1 : 0,
		    (u_longlong_t)ppt->msi.setup_count,
		    (u_longlong_t)ppt->msi.intr_count);
	}

	/* Reject attempts to enable MSI while MSI-X is active. */
	if (ppt->msix.num_msgs != 0 && numvec != 0) {
		err = EBUSY;
		goto done;
	}

	/*
	 * Avoid tearing down and reallocating host MSI state when the guest is
	 * reissuing the same single-vector request.  The guest does this during
	 * normal setup, and churning the host handle there makes passthrough
	 * interrupt state much harder to keep stable.
	 */
	if (numvec != 0 && !ppt->msi.is_fixed && ppt->msi.num_msgs == numvec &&
	    ppt->msi.inth != NULL) {
		for (i = 0; i < numvec; i++) {
			if (ppt->msi.arg[i].addr != addr ||
			    ppt->msi.arg[i].msg_data != msg + i) {
				same_request = B_FALSE;
				break;
			}
		}
		if (same_request) {
			if (ppt_diag_enable != 0) {
				cmn_err(CE_NOTE, "ppt: setup_msi reuse "
				    "bdf=0x%x numvec=%d addr=0x%llx "
				    "msg=0x%llx", bdf, numvec,
				    (u_longlong_t)addr, (u_longlong_t)msg);
			}
			goto done;
		}
	}

	/* Free any allocated resources */
	ppt_teardown_msi(ppt);

	if (numvec == 0) {
		if (ppt_diag_enable != 0)
			cmn_err(CE_NOTE, "ppt: setup_msi disable bdf=0x%x",
			    bdf);
		goto done;
	}

	if (ddi_intr_get_navail(ppt->pptd_dip, DDI_INTR_TYPE_MSI,
	    &msi_count) != DDI_SUCCESS) {
		if (ddi_intr_get_navail(ppt->pptd_dip, DDI_INTR_TYPE_FIXED,
		    &msi_count) != DDI_SUCCESS) {
			err = EINVAL;
			goto done;
		}

		intr_type = DDI_INTR_TYPE_FIXED;
		ppt->msi.is_fixed = B_TRUE;
	} else {
		intr_type = DDI_INTR_TYPE_MSI;
	}

	if (ppt_diag_enable != 0) {
		cmn_err(CE_NOTE, "ppt: setup_msi alloc bdf=0x%x intr_type=%s "
		    "avail=%d requested=%d", bdf,
		    intr_type == DDI_INTR_TYPE_MSI ? "msi" : "fixed",
		    msi_count, numvec);
	}

	/*
	 * The device must be capable of supporting the number of vectors
	 * the guest wants to allocate.
	 */
	if (numvec > msi_count) {
		err = EINVAL;
		goto done;
	}

	ppt->msi.inth_sz = numvec * sizeof (ddi_intr_handle_t);
	ppt->msi.inth = kmem_zalloc(ppt->msi.inth_sz, KM_SLEEP);
	err = ddi_intr_alloc(ppt->pptd_dip, ppt->msi.inth, intr_type, 0,
	    numvec, &msi_count, 0);
	if (ppt_diag_enable != 0 || err != DDI_SUCCESS) {
		cmn_err(err == DDI_SUCCESS ? CE_NOTE : CE_WARN,
		    "ppt: setup_msi ddi_intr_alloc bdf=0x%x intr_type=%s "
		    "requested=%d got=%d err=%d", bdf,
		    intr_type == DDI_INTR_TYPE_MSI ? "msi" : "fixed",
		    numvec, msi_count, err);
	}
	if (err != DDI_SUCCESS) {
		kmem_free(ppt->msi.inth, ppt->msi.inth_sz);
		err = EINVAL;
		goto done;
	}

	/* Verify that we got as many vectors as the guest requested */
	if (numvec != msi_count) {
		ppt_teardown_msi(ppt);
		err = EINVAL;
		goto done;
	}

	/* Set up & enable interrupt handler for each vector. */
	for (i = 0; i < numvec; i++) {
		int res, intr_cap = 0;

		ppt->msi.num_msgs = i + 1;
		ppt->msi.arg[i].pptdev = ppt;
		ppt->msi.arg[i].addr = addr;
		ppt->msi.arg[i].msg_data = msg + i;

		res = ddi_intr_add_handler(ppt->msi.inth[i], pptintr,
		    &ppt->msi.arg[i], NULL);
		if (ppt_diag_enable != 0 || res != DDI_SUCCESS) {
			cmn_err(res == DDI_SUCCESS ? CE_NOTE : CE_WARN,
			    "ppt: setup_msi add_handler bdf=0x%x vec=%d "
			    "guest_msg=0x%llx handle=%p err=%d", bdf, i,
			    (u_longlong_t)(msg + i),
			    (void *)ppt->msi.inth[i], res);
		}
		if (res != DDI_SUCCESS)
			break;

		if (numvec == 1)
			ppt_intr_normalize_single(ppt->msi.inth[i]);

		(void) ddi_intr_get_cap(ppt->msi.inth[i], &intr_cap);
		if ((intr_cap & DDI_INTR_FLAG_BLOCK) && numvec > 1)
			res = ddi_intr_block_enable(&ppt->msi.inth[i], 1);
		else {
			res = ddi_intr_enable(ppt->msi.inth[i]);
		}
		if (ppt_diag_enable != 0 || res != DDI_SUCCESS) {
			cmn_err(res == DDI_SUCCESS ? CE_NOTE : CE_WARN,
			    "ppt: setup_msi enable bdf=0x%x vec=%d "
			    "guest_msg=0x%llx handle=%p caps=0x%x err=%d",
			    bdf, i, (u_longlong_t)(msg + i),
			    (void *)ppt->msi.inth[i], intr_cap, res);
		}

		if (res != DDI_SUCCESS)
			break;
	}
	if (i < numvec) {
		ppt_teardown_msi(ppt);
		err = ENXIO;
	} else {
		ppt->msi.setup_count++;
		if (ppt_diag_enable != 0) {
			cmn_err(CE_NOTE, "ppt: setup_msi armed bdf=0x%x "
			    "numvec=%d intr_type=%s setup_count=%llu", bdf,
			    numvec, ppt->msi.is_fixed ? "fixed" : "msi",
			    (u_longlong_t)ppt->msi.setup_count);
		}
	}

done:
	if (ppt_diag_enable != 0 || err != 0) {
		cmn_err(err == 0 ? CE_NOTE : CE_WARN, "ppt: setup_msi exit "
		    "bdf=0x%x err=%d msgs=%d is_fixed=%d intr_count=%llu "
		    "setup_count=%llu", bdf, err, ppt->msi.num_msgs,
		    ppt->msi.is_fixed ? 1 : 0,
		    (u_longlong_t)ppt->msi.intr_count,
		    (u_longlong_t)ppt->msi.setup_count);
	}
	releasef(pptfd);
	mutex_exit(&pptdev_mtx);
	return (err);
}

int
ppt_setup_msix(struct vm *vm, int vcpu, int pptfd, int idx, uint64_t addr,
    uint64_t msg, uint32_t vector_control)
{
	struct pptdev *ppt;
	int numvec, alloced;
	int err = 0;

	mutex_enter(&pptdev_mtx);
	err = ppt_findf(vm, pptfd, &ppt);
	if (err != 0) {
		mutex_exit(&pptdev_mtx);
		return (err);
	}

	/* Reject attempts to enable MSI-X while MSI is active. */
	if (ppt->msi.num_msgs != 0) {
		err = EBUSY;
		goto done;
	}

	/*
	 * First-time configuration:
	 *	Allocate the MSI-X table
	 *	Allocate the IRQ resources
	 *	Set up some variables in ppt->msix
	 */
	if (ppt->msix.num_msgs == 0) {
		dev_info_t *dip = ppt->pptd_dip;

		if (ddi_intr_get_navail(dip, DDI_INTR_TYPE_MSIX,
		    &numvec) != DDI_SUCCESS) {
			err = EINVAL;
			goto done;
		}

		ppt->msix.num_msgs = numvec;

		ppt->msix.arg_sz = numvec * sizeof (ppt->msix.arg[0]);
		ppt->msix.arg = kmem_zalloc(ppt->msix.arg_sz, KM_SLEEP);
		ppt->msix.inth_sz = numvec * sizeof (ddi_intr_handle_t);
		ppt->msix.inth = kmem_zalloc(ppt->msix.inth_sz, KM_SLEEP);

		if (ddi_intr_alloc(dip, ppt->msix.inth, DDI_INTR_TYPE_MSIX, 0,
		    numvec, &alloced, 0) != DDI_SUCCESS) {
			kmem_free(ppt->msix.arg, ppt->msix.arg_sz);
			kmem_free(ppt->msix.inth, ppt->msix.inth_sz);
			ppt->msix.arg = NULL;
			ppt->msix.inth = NULL;
			ppt->msix.arg_sz = ppt->msix.inth_sz = 0;
			err = EINVAL;
			goto done;
		}

		if (numvec != alloced) {
			ppt_teardown_msix(ppt);
			err = EINVAL;
			goto done;
		}
	}

	if (idx >= ppt->msix.num_msgs) {
		err = EINVAL;
		goto done;
	}

	if ((vector_control & PCIM_MSIX_VCTRL_MASK) == 0) {
		int intr_cap, res;

		/* Tear down the IRQ if it's already set up */
		ppt_teardown_msix_intr(ppt, idx);

		ppt->msix.arg[idx].pptdev = ppt;
		ppt->msix.arg[idx].addr = addr;
		ppt->msix.arg[idx].msg_data = msg;

		/* Setup the MSI-X interrupt */
		if (ddi_intr_add_handler(ppt->msix.inth[idx], pptintr,
		    &ppt->msix.arg[idx], NULL) != DDI_SUCCESS) {
			err = ENXIO;
			goto done;
		}

		if (ppt->msix.num_msgs == 1)
			ppt_intr_normalize_single(ppt->msix.inth[idx]);

		(void) ddi_intr_get_cap(ppt->msix.inth[idx], &intr_cap);
		if ((intr_cap & DDI_INTR_FLAG_BLOCK) &&
		    ppt->msix.num_msgs > 1)
			res = ddi_intr_block_enable(&ppt->msix.inth[idx], 1);
		else
			res = ddi_intr_enable(ppt->msix.inth[idx]);

		if (res != DDI_SUCCESS) {
			ddi_intr_remove_handler(ppt->msix.inth[idx]);
			err = ENXIO;
			goto done;
		}
	} else {
		/* Masked, tear it down if it's already been set up */
		ppt_teardown_msix_intr(ppt, idx);
	}

done:
	releasef(pptfd);
	mutex_exit(&pptdev_mtx);
	return (err);
}

int
ppt_setup_intx(struct vm *vm, int pptfd, int ioapic_irq, boolean_t enable)
{
	struct pptdev *ppt;
	int err = 0;

	mutex_enter(&pptdev_mtx);
	err = ppt_findf(vm, pptfd, &ppt);
	if (err != 0) {
		mutex_exit(&pptdev_mtx);
		return (err);
	}

	err = ppt_setup_intx_locked(ppt, ioapic_irq, enable);
	releasef(pptfd);
	mutex_exit(&pptdev_mtx);
	return (err);
}

static int
ppt_setup_intx_locked(struct pptdev *ppt, int ioapic_irq, boolean_t enable)
{
	int err = 0;
	struct vm *vm = ppt->vm;

	ASSERT(MUTEX_HELD(&pptdev_mtx));

	if (vm == NULL)
		return (ENXIO);

	if (!enable) {
		ppt_teardown_intx(ppt);
		return (0);
	}

	if (ioapic_irq < 0 || ioapic_irq >= vioapic_pincount(vm))
		return (EINVAL);

	if (ppt->intx.inth != NULL && ppt->intx.enabled &&
	    ppt->intx.ioapic_irq == ioapic_irq)
		return (0);

	ppt_teardown_intx(ppt);

	{
		int nalloc = 0;
		int navail = 0;
		int intr_cap = 0;
		int res;
		dev_info_t *dip = ppt->pptd_dip;

		if (ddi_intr_get_navail(dip, DDI_INTR_TYPE_FIXED, &navail) !=
		    DDI_SUCCESS || navail < 1) {
			err = ENOTSUP;
			goto done;
		}

		if (ddi_intr_alloc(dip, &ppt->intx.inth, DDI_INTR_TYPE_FIXED, 0, 1,
		    &nalloc, 0) != DDI_SUCCESS || nalloc != 1) {
			err = ENXIO;
			goto done;
		}

		ppt->intx.arg.pptdev = ppt;
		ppt->intx.arg.ioapic_irq = ioapic_irq;

		if (ddi_intr_add_handler(ppt->intx.inth, pptintr_intx,
		    &ppt->intx.arg, NULL) != DDI_SUCCESS) {
			(void) ddi_intr_free(ppt->intx.inth);
			ppt->intx.inth = NULL;
			err = ENXIO;
			goto done;
		}

		(void) ddi_intr_get_cap(ppt->intx.inth, &intr_cap);
		if (intr_cap & DDI_INTR_FLAG_BLOCK)
			res = ddi_intr_block_enable(&ppt->intx.inth, 1);
		else
			res = ddi_intr_enable(ppt->intx.inth);

		if (res != DDI_SUCCESS) {
			(void) ddi_intr_remove_handler(ppt->intx.inth);
			(void) ddi_intr_free(ppt->intx.inth);
			ppt->intx.inth = NULL;
			err = ENXIO;
			goto done;
		}
	}

	ppt->intx.enabled = B_TRUE;
	ppt->intx.ioapic_irq = ioapic_irq;

done:
	return (err);
}

int
ppt_get_limits(struct vm *vm, int pptfd, int *msilimit, int *msixlimit)
{
	struct pptdev *ppt;
	int err = 0;

	mutex_enter(&pptdev_mtx);
	err = ppt_findf(vm, pptfd, &ppt);
	if (err != 0) {
		mutex_exit(&pptdev_mtx);
		return (err);
	}

	if (ddi_intr_get_navail(ppt->pptd_dip, DDI_INTR_TYPE_MSI,
	    msilimit) != DDI_SUCCESS) {
		*msilimit = -1;
	}
	if (ddi_intr_get_navail(ppt->pptd_dip, DDI_INTR_TYPE_MSIX,
	    msixlimit) != DDI_SUCCESS) {
		*msixlimit = -1;
	}

	releasef(pptfd);
	mutex_exit(&pptdev_mtx);
	return (err);
}

int
ppt_disable_msix(struct vm *vm, int pptfd)
{
	struct pptdev *ppt;
	int err = 0;

	mutex_enter(&pptdev_mtx);
	err = ppt_findf(vm, pptfd, &ppt);
	if (err != 0) {
		mutex_exit(&pptdev_mtx);
		return (err);
	}

	ppt_teardown_msix(ppt);

	releasef(pptfd);
	mutex_exit(&pptdev_mtx);
	return (err);
}
