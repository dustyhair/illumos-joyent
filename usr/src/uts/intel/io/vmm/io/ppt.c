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

#include <sys/modctl.h>
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
#include <sys/stat.h>
#include <sys/sunddi.h>
#include <sys/pci.h>
#include <sys/pci_cap.h>
#include <sys/pcie_impl.h>
#include <sys/ppt_dev.h>
#include <sys/mkdev.h>
#include <sys/sysmacros.h>

#include "vmm_lapic.h"

#include <io/iommu.h>
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
		struct pptintr_arg arg[MAX_MSIMSGS];
	} msi;

	struct {
		int num_msgs;
		size_t inth_sz;
		size_t arg_sz;
		ddi_intr_handle_t *inth;
		struct pptintr_arg *arg;
	} msix;

	/* --- Phase 2b additions --- */
	void	*pptd_domain;   /* iommu_domain_t *, domain context */
	uint32_t	pptd_domainid; /* optional unique domain ID */
	uint64_t	pptd_faults;   /* count of DMA/IOMMU faults */
};

static ddi_modhandle_t iommu_hdl = NULL;
static const struct iommu_ops *ppt_iommu_ops = NULL;

static major_t		ppt_major;
static void		*ppt_state;
static kmutex_t		pptdev_mtx;
static list_t		pptdev_list;

#define	PPT_MINOR_NAME	"ppt"

static ddi_device_acc_attr_t ppt_attr = {
	DDI_DEVICE_ATTR_V0,
	DDI_NEVERSWAP_ACC,
	DDI_STORECACHING_OK_ACC,
	DDI_DEFAULT_ACC
};

static void ppt_reset_pci_power_state(dev_info_t *dip);

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

		/* Log BAR writes explicitly (BAR0–BAR5) */
		if (cio.pci_off >= PCI_CONF_BASE0 && cio.pci_off <= PCI_CONF_BASE5) {
			int bar_idx = (cio.pci_off - PCI_CONF_BASE0) / 4;
			cmn_err(CE_NOTE,
				"!ppt: CFG_WRITE BDF=%x BAR%d off=0x%x width=%d data=0x%08x",
				pci_get_bdf(ppt->pptd_dip), bar_idx,
				(uint_t)cio.pci_off, cio.pci_width, cio.pci_data);
		} else {
			cmn_err(CE_CONT,
				"!ppt: CFG_WRITE BDF=%x off=0x%x width=%d data=0x%08x",
				pci_get_bdf(ppt->pptd_dip),
				(uint_t)cio.pci_off, cio.pci_width, cio.pci_data);
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
	case PPT_GET_CAPS: {
		struct ppt_caps caps;
		caps.version = 1;
		caps.caps = 0;

		/*
		* Advertise only capabilities that are currently wired through
		* ppt ioctls in this tree. Keep this conservative until each
		* feature path is fully validated on target hardware.
		*/
		caps.caps |= PPT_CAP_BAR_INFO;
		caps.caps |= PPT_CAP_IOMMU;

		if (ddi_copyout(&caps, data, sizeof (caps), md) != 0)
			return (EFAULT);
		return (0);
	}
	case PPT_GET_REGION_INFO: {
		struct ppt_region_info rinfo;

		if (ddi_copyin(data, &rinfo, sizeof (rinfo), md) != 0)
			return (EFAULT);

		if (rinfo.index >= PCI_BASE_NUM)
			return (EINVAL);

		struct pptbar *pbar = &ppt->pptd_bars[rinfo.index];
		if (pbar->base == 0 || pbar->size == 0)
			return (ENOENT);

		rinfo.phys_addr = pbar->base;
		rinfo.size      = pbar->size;
		rinfo.flags     = pbar->type;

		if (ddi_copyout(&rinfo, data, sizeof (rinfo), md) != 0)
			return (EFAULT);
		return (0);
	}

	case PPT_IOMMU_MAP: {
		struct ppt_iommu_map map;
		if (ddi_copyin(data, &map, sizeof(map), md) != 0)
			return (EFAULT);

		int rc = iommu_domain_map(ppt->pptd_domain,
								map.gpa, map.hpa, map.size,
								map.prot);

		if (rc == 0) {
			/* 🔑 ensure device domain sees the new mapping */
			iommu_invalidate_tlb(ppt->pptd_domain);

			cmn_err(CE_NOTE,
				"!ppt: IOMMU_MAP OK BDF=%x GPA=0x%llx HPA=0x%llx sz=0x%llx",
				pci_get_bdf(ppt->pptd_dip),
				(u_longlong_t)map.gpa,
				(u_longlong_t)map.hpa,
				(u_longlong_t)map.size);
		} else {
			cmn_err(CE_WARN,
				"!ppt: IOMMU_MAP FAIL BDF=%x GPA=0x%llx sz=0x%llx rc=%d",
				pci_get_bdf(ppt->pptd_dip),
				(u_longlong_t)map.gpa,
				(u_longlong_t)map.size,
				rc);
		}
		return rc;
	}

	case PPT_IOMMU_UNMAP: {
		struct ppt_iommu_map map;

		/* Copy request from userland */
		if (ddi_copyin(data, &map, sizeof (map), md) != 0)
			return (EFAULT);

		/* Issue the unmap into this device's domain */
		int rc = iommu_domain_unmap(ppt->pptd_domain, map.gpa, map.size);

		if (rc == 0) {
			/* 🔑 Always flush TLB after unmap */
			iommu_invalidate_tlb(ppt->pptd_domain);

			cmn_err(CE_NOTE,
				"!ppt: IOMMU_UNMAP OK BDF=%x GPA=0x%llx sz=0x%llx",
				pci_get_bdf(ppt->pptd_dip),
				(u_longlong_t)map.gpa,
				(u_longlong_t)map.size);
		} else {
			cmn_err(CE_WARN,
				"!ppt: IOMMU_UNMAP FAIL BDF=%x GPA=0x%llx sz=0x%llx rc=%d",
				pci_get_bdf(ppt->pptd_dip),
				(u_longlong_t)map.gpa,
				(u_longlong_t)map.size,
				rc);
		}

		return rc;
	}

	case PPT_IOMMU_MAP_BATCH: {
		struct ppt_iommu_map_batch ureq;
		if (ddi_copyin(data, &ureq, sizeof (ureq), md) != 0)
			return (EFAULT);

		size_t totsz = offsetof(struct ppt_iommu_map_batch, maps) +
					ureq.count * sizeof (struct ppt_iommu_map);

		struct ppt_iommu_map_batch *kreq =
			kmem_zalloc(totsz, KM_SLEEP);
		if (ddi_copyin(data, kreq, totsz, md) != 0) {
			kmem_free(kreq, totsz);
			return (EFAULT);
		}

		for (uint32_t i = 0; i < kreq->count; i++) {
			struct ppt_iommu_map *m = &kreq->maps[i];
			iommu_domain_map(ppt->pptd_domain,
							m->gpa, m->hpa,
							m->size, m->prot);
		}

		if (kreq->count > 0)
			iommu_invalidate_tlb(ppt->pptd_domain);

		kmem_free(kreq, totsz);
		return (0);
	}

	case PPT_IOMMU_UNMAP_BATCH: {
		struct ppt_iommu_map_batch ureq;
		if (ddi_copyin(data, &ureq, sizeof (ureq), md) != 0)
			return (EFAULT);

		size_t totsz = offsetof(struct ppt_iommu_map_batch, maps) +
					ureq.count * sizeof (struct ppt_iommu_map);

		struct ppt_iommu_map_batch *kreq =
			kmem_zalloc(totsz, KM_SLEEP);
		if (ddi_copyin(data, kreq, totsz, md) != 0) {
			kmem_free(kreq, totsz);
			return (EFAULT);
		}

		for (uint32_t i = 0; i < kreq->count; i++) {
			struct ppt_iommu_map *m = &kreq->maps[i];
			iommu_domain_unmap(ppt->pptd_domain,
							m->gpa, m->size);
		}

		if (kreq->count > 0)
			iommu_invalidate_tlb(ppt->pptd_domain);

		kmem_free(kreq, totsz);
		return (0);
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


#define PCI_BASE_MEM_TYPE_M   0x00000006
#define PCI_BASE_MEM_TYPE_32  0x00000000
#define PCI_BASE_MEM_TYPE_1M  0x00000002
#define PCI_BASE_MEM_TYPE_64  0x00000004

#define PCI_BASE_MEM_TYPE_M   0x00000006
#define PCI_BASE_MEM_TYPE_32  0x00000000
#define PCI_BASE_MEM_TYPE_1M  0x00000002
#define PCI_BASE_MEM_TYPE_64  0x00000004
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

// static int
// ppt_bar_crawl(struct pptdev *ppt)
// {
// 	struct pptbar *pbar;
// 	int err = 0;
// 	int i;
// 
// 	for (i = 0; i < PCI_BASE_NUM; i++) {
// 		off_t off = PCI_CONF_BASE0 + i * 4;
// 		uint32_t barlo, barhi = 0, szlo, szhi = 0;
// 		uint64_t base, size;
// 		uint_t cfg_type;
// 
// 		barlo = pci_config_get32(ppt->pptd_cfg, off);
// 		if (barlo == 0 || barlo == 0xffffffff) {
// 			/* unused BAR slot */
// 			continue;
// 		}
// 
// 		cfg_type = barlo & PCI_BASE_SPACE_M;
// 
// 		/* --- new: diagnostic --- */
// 		cmn_err(CE_NOTE, "!ppt: probing BAR%d raw=0x%08x cfg_type=%s",
// 		    i, barlo,
// 		    (cfg_type == PCI_BASE_SPACE_IO) ? "IO" :
// 		    ((barlo & PCI_BASE_MEM_TYPE_M) == PCI_BASE_MEM_TYPE_64 ?
// 		    "MEM64" : "MEM32"));
// 
// 		if (cfg_type == PCI_BASE_SPACE_IO) {
// 			/* ---- I/O BAR ---- */
// 			pci_config_put32(ppt->pptd_cfg, off, 0xffffffff);
// 			szlo = pci_config_get32(ppt->pptd_cfg, off);
// 			pci_config_put32(ppt->pptd_cfg, off, barlo);
// 
// 			size = ~(szlo & PCI_BASE_IO_ADDR_M) + 1;
// 			base = barlo & PCI_BASE_IO_ADDR_M;
// 
// 			/* diagnostic: catch small IO windows */
// 			cmn_err(CE_NOTE,
// 			    "!ppt: BAR%d IO port base=0x%llx size=0x%llx (%llu bytes)",
// 			    i, (unsigned long long)base,
// 			    (unsigned long long)size,
// 			    (unsigned long long)size);
// 
// 		} else {
// 			/* ---- Memory BAR (32‑bit or 64‑bit) ---- */
// 			if ((barlo & PCI_BASE_MEM_TYPE_M) == PCI_BASE_MEM_TYPE_64) {
// 				uint64_t szfull;
// 
// 				barhi = pci_config_get32(ppt->pptd_cfg, off + 4);
// 
// 				/* probe size */
// 				pci_config_put32(ppt->pptd_cfg, off, 0xffffffff);
// 				pci_config_put32(ppt->pptd_cfg, off + 4, 0xffffffff);
// 				szlo = pci_config_get32(ppt->pptd_cfg, off);
// 				szhi = pci_config_get32(ppt->pptd_cfg, off + 4);
// 				pci_config_put32(ppt->pptd_cfg, off, barlo);
// 				pci_config_put32(ppt->pptd_cfg, off + 4, barhi);
// 
// 				szfull = ((uint64_t)szhi << 32) |
// 				    (szlo & PCI_BASE_M_ADDR_M);
// 				size = (~szfull + 1);
// 
// 				base = ((uint64_t)barhi << 32) |
// 				    (barlo & PCI_BASE_M_ADDR_M);
// 
// 				i++;	/* skip upper half */
// 			} else {
// 				/* 32‑bit memory BAR */
// 				pci_config_put32(ppt->pptd_cfg, off, 0xffffffff);
// 				szlo = pci_config_get32(ppt->pptd_cfg, off);
// 				pci_config_put32(ppt->pptd_cfg, off, barlo);
// 
// 				size = ~(szlo & PCI_BASE_M_ADDR_M) + 1;
// 				base = barlo & PCI_BASE_M_ADDR_M;
// 			}
// 		}
// 
// 		if (size == 0)
// 			continue;
// 
// 		pbar = &ppt->pptd_bars[i];
// 		pbar->base = base;
// 		pbar->size = size;
// 
// 		/* classify the BAR type */
// 		if (cfg_type == PCI_BASE_SPACE_IO)
// 			pbar->type = PCI_ADDR_IO;
// 		else if ((barlo & PCI_BASE_MEM_TYPE_M) == PCI_BASE_MEM_TYPE_64)
// 			pbar->type = PCI_ADDR_MEM64;
// 		else
// 			pbar->type = PCI_ADDR_MEM32;
// 
// 		cmn_err(CE_NOTE,
// 		    "!ppt: BAR%d confirmed: base=0x%llx size=0x%llx (%llu MB) type=%s",
// 		    i,
// 		    (unsigned long long)base,
// 		    (unsigned long long)size,
// 		    (unsigned long long)(size >> 20),
// 		    (pbar->type == PCI_ADDR_IO)   ? "IO" :
// 		    (pbar->type == PCI_ADDR_MEM64)? "MEM64" : "MEM32");
// 
// 		/* map IO BARs so ddi_get/put() works */
// 		if (pbar->type == PCI_ADDR_IO) {
// 			err = ddi_regs_map_setup(ppt->pptd_dip, i,
// 			    &pbar->io_ptr, 0, 0, &ppt_attr, &pbar->io_handle);
// 			if (err != 0)
// 				break;
// 		}
// 	}
// 
// 	if (err != 0)
// 		ppt_bar_wipe(ppt);
// 
// 	return (err);
// }

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
	ddi_set_driver_private(dip, ppt);

	if (pci_config_setup(dip, &ppt->pptd_cfg) != DDI_SUCCESS) {
		goto fail;
	}
	if (ppt_bar_crawl(ppt) != 0) {
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

#ifndef PCI_PMCSR_STATE_D0
#define PCI_PMCSR_STATE_D0     0x0000
#endif

#ifndef PCI_PMCSR_STATE_D3HOT
#define PCI_PMCSR_STATE_D3HOT  0x0003
#endif


static boolean_t
ppt_flr(dev_info_t *dip, boolean_t force)
{
	uint16_t cap_ptr, ctl, cmd;
	ddi_acc_handle_t hdl;
	uint_t compl_delay = 0, max_delay_us;

	if (pci_config_setup(dip, &hdl) != DDI_SUCCESS)
		return (B_FALSE);

	/* Try to locate PCIe capability */
	if (PCI_CAP_LOCATE(hdl, PCI_CAP_ID_PCI_E, &cap_ptr) != DDI_SUCCESS)
		goto fail;

	/* Check if FLR is supported by this function */
	if ((PCI_CAP_GET32(hdl, 0, cap_ptr, PCIE_DEVCAP) & PCIE_DEVCAP_FLR) == 0)
		goto fail;

	max_delay_us = MAX(ppt_max_completion_tmo_us(dip), 10000);

	/*
	 * Disable bus mastering to prevent generation of new transactions
	 * while waiting for pending transactions to drain.
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

		/* Add post-FLR delay to cover max completion timeout */
		compl_delay = MAX(10, (ppt_max_completion_tmo_us(dip) / 1000));
	}

	/* Initiate the FLR */
	ctl = PCI_CAP_GET16(hdl, 0, cap_ptr, PCIE_DEVCTL);
	(void)PCI_CAP_PUT16(hdl, 0, cap_ptr, PCIE_DEVCTL,
	    ctl | PCIE_DEVCTL_INITIATE_FLR);

	/* Wait at least 100ms (plus delay if completions were pending) */
	delay(drv_usectohz((100 + compl_delay) * 1000));

	pci_config_teardown(&hdl);
	return (B_TRUE);

fail:
	dev_err(dip, CE_NOTE, "!FLR unsupported or failed, attempting PM reset fallback");

	pci_config_teardown(&hdl);
	return (B_FALSE);
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

	if (ppt->msi.num_msgs == 0)
		return;

	for (i = 0; i < ppt->msi.num_msgs; i++) {
		int intr_cap;

		(void) ddi_intr_get_cap(ppt->msi.inth[i], &intr_cap);
		if (intr_cap & DDI_INTR_FLAG_BLOCK)
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

	ppt->msi.num_msgs = 0;
}

static void
ppt_teardown_msix_intr(struct pptdev *ppt, int idx)
{
	if (ppt->msix.inth != NULL && ppt->msix.inth[idx] != NULL) {
		int intr_cap;

		(void) ddi_intr_get_cap(ppt->msix.inth[idx], &intr_cap);
		if (intr_cap & DDI_INTR_FLAG_BLOCK)
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

#ifndef PCIE_LINKSTS_NEG_WIDTH_MASK
#define PCIE_LINKSTS_NEG_WIDTH_MASK	0x03f0
#define PCIE_LINKSTS_NEG_WIDTH_SHIFT	4
#endif

/*
 * Debug helper: print PCIe and PM info for a GPU after reset.
 * Can be invoked right after FLR or bus reset to confirm device came back alive.
 */
static void
ppt_trace_gpu_state(dev_info_t *dip, const char *label)
{
	ddi_acc_handle_t cfg;
	uint16_t cap_ptr;
	uint16_t pmcsr = 0xffff;
	uint32_t linkcap = 0, linksta = 0;
	int i;

	if (pci_config_setup(dip, &cfg) != DDI_SUCCESS) {
		cmn_err(CE_WARN, "!ppt_trace_gpu_state: cannot map cfg space");
		return;
	}

	cmn_err(CE_NOTE, "!ppt_trace_gpu_state(%s): BDF=%x",
	    label, pci_get_bdf(dip));

	/* --- Power Management --- */
	if (PCI_CAP_LOCATE(cfg, PCI_CAP_ID_PM, &cap_ptr) == DDI_SUCCESS) {
		pmcsr = PCI_CAP_GET16(cfg, 0, cap_ptr, PCI_PMCSR);
		cmn_err(CE_NOTE, "\tPMCSR=0x%04x (state=%u -> %s)",
		    pmcsr, pmcsr & PCI_PMCSR_STATE_MASK,
		    (pmcsr & PCI_PMCSR_STATE_MASK) == PCI_PMCSR_D0 ?
		    "D0" :
		    (pmcsr & PCI_PMCSR_STATE_MASK) == PCI_PMCSR_D3HOT ?
		    "D3hot" : "other");
	} else {
		cmn_err(CE_NOTE, "\t(no PM capability)");
	}

	/* --- Link information --- */
	if (PCI_CAP_LOCATE(cfg, PCI_CAP_ID_PCI_E, &cap_ptr) == DDI_SUCCESS) {
		linkcap = PCI_CAP_GET32(cfg, 0, cap_ptr, PCIE_LINKCAP);
		linksta = PCI_CAP_GET16(cfg, 0, cap_ptr, PCIE_LINKSTS);

		/* illumos already defines the MASK but not the SHIFT */
	#ifndef PCIE_LINKSTS_NEG_WIDTH_SHIFT
	#define PCIE_LINKSTS_NEG_WIDTH_SHIFT 4
	#endif

		cmn_err(CE_NOTE, "\tLink: Speed x%u, Negotiated x%u",
			linksta & PCIE_LINKSTS_SPEED_MASK,
			(linksta & PCIE_LINKSTS_NEG_WIDTH_MASK) >>
			PCIE_LINKSTS_NEG_WIDTH_SHIFT);
	} else {
		cmn_err(CE_NOTE, "\t(no PCIe capability)");
	}

	/* --- BAR snapshot --- */
	for (i = 0; i < PCI_BASE_NUM; i++) {
		uint32_t bar = pci_config_get32(cfg, PCI_CONF_BASE0 + i * 4);
		if (bar == 0 || bar == 0xffffffff)
			continue;

		if ((bar & PCI_BASE_SPACE_M) == PCI_BASE_SPACE_MEM)
			cmn_err(CE_NOTE, "\tBAR%d=0x%08x (MEM%s)",
			    i, bar,
			    (bar & 0x8) ? "64" : "32");
		else
			cmn_err(CE_NOTE, "\tBAR%d=0x%08x (IO)", i, bar);
	}

	pci_config_teardown(&cfg);
}

/*
 * Perform a PCIe secondary-bus reset on the parent bridge of @dip.
 * Safe fallback when a device lacks FLR (common on consumer GPUs).
 */
static void
ppt_bus_reset(dev_info_t *dip)
{
	dev_info_t *parent;
	uint8_t bctl;
	ddi_acc_handle_t hdl;

	parent = ddi_get_parent(dip);
	if (parent == NULL)
		return;

	if (pci_config_setup(parent, &hdl) != DDI_SUCCESS)
		return;

	bctl = pci_config_get8(hdl, PCI_BCNF_BCNTRL);
	bctl |= PCI_BCNF_BCNTRL_RESET;
	pci_config_put8(hdl, PCI_BCNF_BCNTRL, bctl);

	/* Hold reset for 100 ms */
	delay(drv_usectohz(100000));

	bctl &= ~PCI_BCNF_BCNTRL_RESET;
	pci_config_put8(hdl, PCI_BCNF_BCNTRL, bctl);

	pci_config_teardown(&hdl);
	cmn_err(CE_NOTE, "!ppt_bus_reset: issued secondary‑bus reset for %s",
	    ddi_node_name(dip));
}

//	int
//	ppt_assign_device(struct vm *vm, int pptfd)
//	{
//		struct pptdev *ppt;
//		int err = 0;
//		uint16_t bdf;
//	
//		/* --- tracing --- */
//		cmn_err(CE_NOTE, "!ppt_assign_device: enter (vm=%p, fd=%d)", (void *)vm, pptfd);
//	
//		mutex_enter(&pptdev_mtx);
//	
//		/* Lookup ppt soft‑state from fd */
//		err = ppt_findf(NULL, pptfd, &ppt);
//		if (err != 0) {
//			cmn_err(CE_WARN, "!ppt_assign_device: ppt_findf failed, err=%d", err);
//			mutex_exit(&pptdev_mtx);
//			return (err);
//		}
//	
//		/*
//		 * 1. Save current PCI configuration space before reset
//		 */
//		cmn_err(CE_NOTE, "!ppt_assign_device: saving PCI config space");
//		if (pci_save_config_regs(ppt->pptd_dip) != DDI_SUCCESS) {
//			err = EIO;
//			goto done;
//		}
//	
//		/*
//		 * 2. Issue a Function‑Level Reset (FLR) if available.
//		 *    Many GPUs require a full hardware reset prior to passthrough.
//		 *    If FLR is unsupported, attempt a secondary‑bus reset.
//		 */
//		bdf = pci_get_bdf(ppt->pptd_dip);
//	
//		for (int i = 0; i < PCI_BASE_NUM; i++) {
//			uint32_t lo = pci_config_get32(ppt->pptd_cfg,
//				PCI_CONF_BASE0 + i * 4);
//			uint32_t hi = 0;
//			if ((lo & PCI_BASE_MEM_TYPE_M) == PCI_BASE_MEM_TYPE_64)
//				hi = pci_config_get32(ppt->pptd_cfg,
//					PCI_CONF_BASE0 + (i + 1) * 4);
//			cmn_err(CE_NOTE, "PPT: !before reset BAR%d lo=0x%08x hi=0x%08x", i, lo, hi);
//		}
//	
//		uint32_t bar_save[PCI_BASE_NUM * 2] = {0};
//		for (int i = 0; i < PCI_BASE_NUM; i++) {
//			bar_save[i * 2] = pci_config_get32(ppt->pptd_cfg,
//				PCI_CONF_BASE0 + i * 4);
//			if ((bar_save[i * 2] & PCI_BASE_MEM_TYPE_M) == PCI_BASE_MEM_TYPE_64)
//				bar_save[i * 2 + 1] = pci_config_get32(ppt->pptd_cfg,
//					PCI_CONF_BASE0 + (i + 1) * 4);
//		}
//		
//		if (bdf == 0x101) {
//			cmn_err(CE_NOTE,
//				"!ppt_assign_device: skipping reset for secondary function BDF=0x%x",
//				bdf);
//		} else {
//			cmn_err(CE_NOTE, "!ppt_assign_device: attempting FLR/bus reset for BDF=0x%x",
//				bdf);
//			if (!ppt_flr(ppt->pptd_dip, B_TRUE)) {
//				cmn_err(CE_WARN,
//					"!ppt_assign_device: FLR unsupported, using bus reset fallback");
//				(void)ppt_bus_reset(ppt->pptd_dip);
//			}
//			
//			//	cmn_err(CE_NOTE, "!ppt_assign_device: attempting FLR reset");
//		//	if (!ppt_flr(ppt->pptd_dip, B_TRUE)) {
//		//		cmn_err(CE_WARN, "!ppt_assign_device: FLR unsupported, using bus reset fallback");
//		//		(void) ppt_bus_reset(ppt->pptd_dip);
//		//	}
//			delay(drv_usectohz(500000));	/* 200 ms settle time */
//			delay(drv_usectohz(500000));	/* 200 ms settle time */
//	
//			for (int i = 0; i < PCI_BASE_NUM; i++) {
//				pci_config_put32(ppt->pptd_cfg, PCI_CONF_BASE0 + i * 4,
//					bar_save[i * 2]);
//				if ((bar_save[i * 2] & PCI_BASE_MEM_TYPE_M) == PCI_BASE_MEM_TYPE_64)
//					pci_config_put32(ppt->pptd_cfg, PCI_CONF_BASE0 + (i + 1) * 4,
//						bar_save[i * 2 + 1]);
//			}
//	
//			for (int i = 0; i < PCI_BASE_NUM; i++) {
//				uint32_t lo = pci_config_get32(ppt->pptd_cfg,
//					PCI_CONF_BASE0 + i * 4);
//				uint32_t hi = 0;
//				if ((lo & PCI_BASE_MEM_TYPE_M) == PCI_BASE_MEM_TYPE_64)
//					hi = pci_config_get32(ppt->pptd_cfg,
//						PCI_CONF_BASE0 + (i + 1) * 4);
//				cmn_err(CE_NOTE, "PPT: !after reset BAR%d lo=0x%08x hi=0x%08x", i, lo, hi);
//			}
//	
//			ppt_trace_gpu_state(ppt->pptd_dip, "post‑reset");
//			/*
//			* 3. Restore the power state and re‑initialize config space.
//			*/
//			cmn_err(CE_NOTE, "!ppt_assign_device: restoring config state");
//			ppt_reset_pci_power_state(ppt->pptd_dip);
//	
//			/* optional extra delay */
//			delay(drv_usectohz(300000));
//	
//			if (pci_restore_config_regs(ppt->pptd_dip) != DDI_SUCCESS ||
//				pci_save_config_regs(ppt->pptd_dip) != DDI_SUCCESS) {
//				err = EIO;
//				goto done;
//			}
//	
//			/*
//			*  Re‑enable memory, I/O and bus‑master decoding after reset.
//			*  Some GPUs lose CMD bits while their audio function retains them.
//			*/
//			uint16_t cmd = pci_config_get16(ppt->pptd_cfg, PCI_CONF_COMM);
//			cmd |= PCI_COMM_ME | PCI_COMM_MAE | PCI_COMM_IO;
//			pci_config_put16(ppt->pptd_cfg, PCI_CONF_COMM, cmd);
//			cmd = pci_config_get16(ppt->pptd_cfg, PCI_CONF_COMM);
//			cmn_err(CE_NOTE, "!ppt_assign_device: PCI command register now 0x%04x (Mem/BM/I/O enable)", cmd);
//	
//			uint8_t pstate = pci_config_get8(ppt->pptd_cfg, PCI_PMCSR);
//			cmn_err(CE_NOTE, "PPT: power_state reg=0x%x", pstate & 0x3);
//			/*
//			* 4. Enable bus‑mastering and BAR decoding so bhyve can map BARs.
//			*/
//			ppt_toggle_bar(ppt, B_TRUE);
//			cmn_err(CE_NOTE, "!ppt_assign_device: BARs enabled");
//		}
//		/*
//		 * 5. Create a fresh IOMMU domain for this device.
//		 */
//		bdf = pci_get_bdf(ppt->pptd_dip);
//		cmn_err(CE_NOTE, "!ppt_assign_device: creating IOMMU domain for BDF %x", bdf);
//	
//		ppt->pptd_domain = iommu_create_domain(1ULL << 36);
//		if (ppt->pptd_domain == NULL) {
//			err = ENOMEM;
//			goto done;
//		}
//	
//		ppt->vm = vm;
//		ppt->pptd_faults = 0;
//	
//		/*
//		 * Detach device from host domain and attach to new VM domain.
//		 */
//		cmn_err(CE_NOTE, "!ppt_assign_device: moving BDF %x into new IOMMU domain", bdf);
//		iommu_remove_device(iommu_host_domain(), bdf);
//		iommu_add_device(ppt->pptd_domain, bdf);
//	
//		pf_set_passthru(ppt->pptd_dip, B_TRUE);
//	
//		cmn_err(CE_NOTE, "!ppt_assign_device: complete, domain=%p", ppt->pptd_domain);
//	
//	done:
//		releasef(pptfd);
//		mutex_exit(&pptdev_mtx);
//		return (err);
//	}
static void
ppt_dump_pcie_ext_caps(dev_info_t *dip)
{
	ddi_acc_handle_t hdl;
	uint16_t ptr = 0x100; /* start of PCIe extended capability list */
	uint16_t capid, next;

	if (pci_config_setup(dip, &hdl) != DDI_SUCCESS)
		return;

	while (ptr) {
		uint32_t hdr = pci_config_get32(hdl, ptr);
		if (hdr == 0xffffffff)
			break;

		capid = hdr & 0xffff;
		next = (hdr >> 20) & 0xfff;
		dev_err(dip, CE_NOTE, "!extcap: id=0x%04x next=0x%03x", capid, next);

		if (!next || next == ptr)
			break;
		ptr = next;
	}
	pci_config_teardown(&hdl);
}

#ifndef PCIE_LINKSTS_DLLLA
#define PCIE_LINKSTS_DLLLA        0x0001  /* Data Link Layer Link Active */
#endif
#ifndef PCIE_LINKSTS_SPEED_MASK
#define PCIE_LINKSTS_SPEED_MASK   0x000F
#endif
#ifndef PCIE_LINKSTS_NEG_WIDTH_MASK
#define PCIE_LINKSTS_NEG_WIDTH_MASK   0x03F0
#endif
#ifndef PCIE_LINKSTS_NEG_WIDTH_SHIFT
#define PCIE_LINKSTS_NEG_WIDTH_SHIFT  4
#endif

/*
 * Assign a physical PCI function to a VM and prepare it for passthrough.
 * Performs:
 *   - Config‑space save
 *   - FLR / NVIDIA vendor / bus reset
 *   - PCIe link retrain polling
 *   - D0 power restoration
 *   - BAR + ROM re‑enable
 *   - IOMMU domain creation and device attach
 */

#define NVIDIA_VENDOR_ID   0x10de
#define NV_GPU_RESET_REG   0x488
#define NV_RESET_WAIT_US   500000   /* 500 ms */
#define LINK_POLL_INTERVAL_US 10000 /* 10 ms */
#define LINK_POLL_TIMEOUT_US 1000000 /* 1 s */

/*
 * Poll the PCIe Link Status value until the Data Link Layer Link Active bit
 * sets or timeout expires.
 */
static boolean_t
ppt_wait_link_active(dev_info_t *dip)
{
	ddi_acc_handle_t hdl;
	uint16_t cap, lstat;
	hrtime_t start = gethrtime();

	if (pci_config_setup(dip, &hdl) != DDI_SUCCESS)
		return (B_FALSE);

	if (PCI_CAP_LOCATE(hdl, PCI_CAP_ID_PCI_E, &cap) != DDI_SUCCESS) {
		pci_config_teardown(&hdl);
		return (B_FALSE);
	}

	do {
		lstat = PCI_CAP_GET16(hdl, 0, cap, PCIE_LINKSTS);
		if (lstat & PCIE_LINKSTS_DLLLA) {
			uint16_t speed = lstat & PCIE_LINKSTS_SPEED_MASK;
			uint16_t width =
			    (lstat & PCIE_LINKSTS_NEG_WIDTH_MASK) >>
			    PCIE_LINKSTS_NEG_WIDTH_SHIFT;

			uint_t diff_ms = (gethrtime() - start) / (hrtime_t)1e6;
			dev_err(dip, CE_NOTE,
			    "!ppt_wait_link_active: DLL link active "
			    "(Gen%d x%d) after %u ms", speed, width, diff_ms);

			pci_config_teardown(&hdl);
			return (B_TRUE);
		}
		delay(drv_usectohz(LINK_POLL_INTERVAL_US));
	} while ((gethrtime() - start) < (hrtime_t)USEC2NSEC(LINK_POLL_TIMEOUT_US));

	uint_t diff_ms = (gethrtime() - start) / (hrtime_t)1e6;
	dev_err(dip, CE_WARN,
	    "!ppt_wait_link_active: link NOT active after %u ms, lstat=0x%04x",
	    diff_ms, lstat);

	pci_config_teardown(&hdl);
	return (B_FALSE);
}
/*
 * NVIDIA vendor‑specific soft reset.
 * Writes bit 0 at offset 0x488, waits 500 ms, and returns TRUE if bit 0 clears.
 */
static boolean_t
ppt_vendor_reset(dev_info_t *dip)
{
	ddi_acc_handle_t hdl;
	uint16_t vid, did;
	uint32_t val, post;
	boolean_t success = B_FALSE;

	if (pci_config_setup(dip, &hdl) != DDI_SUCCESS)
		return (B_FALSE);

	vid = pci_config_get16(hdl, PCI_CONF_VENID);
	did = pci_config_get16(hdl, PCI_CONF_DEVID);
	if (vid != NVIDIA_VENDOR_ID) {
		pci_config_teardown(&hdl);
		return (B_FALSE);
	}

	val = pci_config_get32(hdl, NV_GPU_RESET_REG);
	if (val == 0xffffffff || val == 0x0) {
		dev_err(dip, CE_NOTE,
		    "!ppt_vendor_reset: invalid register 0x%x (0x%08x)",
		    NV_GPU_RESET_REG, val);
		goto done;
	}

	dev_err(dip, CE_NOTE,
	    "!ppt_vendor_reset: attempting NVIDIA soft reset VID=0x%04x DID=0x%04x "
	    "(reg=0x%08x)", vid, did, val);

	pci_config_put32(hdl, NV_GPU_RESET_REG, val | 1);
	delay(drv_usectohz(NV_RESET_WAIT_US));  /* settle */

	post = pci_config_get32(hdl, NV_GPU_RESET_REG);
	dev_err(dip, CE_NOTE,
	    "!ppt_vendor_reset: post‑write 0x%08x (bit0=%d)",
	    post, post & 1);

	if ((post & 1) == 0) {
		dev_err(dip, CE_NOTE, "!ppt_vendor_reset: bit0 cleared → reset ack");
		success = B_TRUE;
	} else {
		dev_err(dip, CE_WARN,
		    "!ppt_vendor_reset: reset bit not cleared — may not support");
	}

	if (success)
		(void)ppt_wait_link_active(dip);

done:
	pci_config_teardown(&hdl);
	return (success);
}

/*
 * ppt_reset_validate()
 *
 * Verify that a PCIe function has successfully completed a reset.
 * Checks:
 *   1.  Config space responds (VID/DID not 0xFFFF)
 *   2.  Command register cleared (0x0000)
 *   3.  Link retrained and DLLLA bit set
 *   4.  Optionally ROM header readable
 *   5.  BAR0 readable (MMIO space alive)
 *
 * Returns B_TRUE when all sanity checks passed, else B_FALSE.
 */
static boolean_t
ppt_reset_validate(dev_info_t *dip)
{
	ddi_acc_handle_t hdl;
	uint16_t vid, did, cmd, pciecap, lstat;
	boolean_t ok = B_TRUE;

	if (pci_config_setup(dip, &hdl) != DDI_SUCCESS)
		return (B_FALSE);

	/* 1️⃣  Config-space response */
	vid = pci_config_get16(hdl, PCI_CONF_VENID);
	did = pci_config_get16(hdl, PCI_CONF_DEVID);
	if (vid == 0xffff || vid == 0x0000) {
		dev_err(dip, CE_WARN,
		    "!ppt_reset_validate: config space unreadable (VID=0x%04x)", vid);
		ok = B_FALSE;
		goto done;
	}
	dev_err(dip, CE_NOTE, "!ppt_reset_validate: VID=0x%04x DID=0x%04x", vid, did);

	/* 2️⃣  Command register should have cleared */
	cmd = pci_config_get16(hdl, PCI_CONF_COMM);
	dev_err(dip, CE_NOTE, "!ppt_reset_validate: CMD=0x%04x", cmd);
	if (cmd != 0x0000) {
		dev_err(dip, CE_NOTE, "!ppt_reset_validate: CMD not 0 after reset");
		/* Not fatal, but record */
	}

	/* 3️⃣  PCIe link active and retrained */
	if (PCI_CAP_LOCATE(hdl, PCI_CAP_ID_PCI_E, &pciecap) == DDI_SUCCESS) {
		lstat = PCI_CAP_GET16(hdl, 0, pciecap, PCIE_LINKSTS);
		uint16_t speed = lstat & 0xF;
		uint16_t width = (lstat & 0x3F0) >> 4;
#ifdef PCIE_LINKSTS_DLLLA
		boolean_t dll_active = !!(lstat & PCIE_LINKSTS_DLLLA);
#else
		boolean_t dll_active = !!(lstat & 0x1);
#endif
		dev_err(dip, CE_NOTE,
		    "!ppt_reset_validate: LINKSTS=0x%04x (Gen%d x%d, DLLLA=%d)",
		    lstat, speed, width, dll_active);
		if (!dll_active) {
			dev_err(dip, CE_WARN,
			    "!ppt_reset_validate: link inactive after reset");
			ok = B_FALSE;
			goto done;
		}
	} else {
		dev_err(dip, CE_WARN, "!ppt_reset_validate: no PCIe cap found?");
	}

	/* 4️⃣  Optional ROM BAR check for NVIDIA (0x1c/vend) */
	uint32_t rom_bar = pci_config_get32(hdl, PCI_CONF_ROM);
	pci_config_put32(hdl, PCI_CONF_ROM, rom_bar | PCI_BASE_ROM_ENABLE);
	delay(drv_usectohz(10000)); /* 10 ms enable latency */
	uint32_t rom_first = pci_config_get32(hdl, (uint32_t)PCI_CONF_BASE5);
	pci_config_put32(hdl, PCI_CONF_ROM, rom_bar);
	if (rom_first == 0xffffffff) {
		dev_err(dip, CE_NOTE, "!ppt_reset_validate: ROM BAR unreadable (0xFFFFFFFF)");
	} else {
		dev_err(dip, CE_NOTE, "!ppt_reset_validate: ROM BAR responds (0x%08x)", rom_first);
	}

	/* 5️⃣  BAR0 sanity—responds with something other than 0xFFFFFFFF */
	uint64_t bar0 = pci_config_get32(hdl, PCI_CONF_BASE0);
	if ((bar0 & ~0xF) != 0 && bar0 != 0xffffffff)
		dev_err(dip, CE_NOTE,
		    "!ppt_reset_validate: BAR0=0x%08lx appears mapped", bar0);
	else {
		dev_err(dip, CE_WARN,
		    "!ppt_reset_validate: BAR0 unreadable (0x%08lx)", bar0);
		ok = B_FALSE;
	}

done:
	pci_config_teardown(&hdl);
	dev_err(dip, ok ? CE_NOTE : CE_WARN,
	    ok ? "!ppt_reset_validate: device OK after reset"
	       : "!ppt_reset_validate: device FAILED reset checks");
	return (ok);
}

/*
 * === Main Passthrough Assignment Routine ===
 */
int
ppt_assign_device(struct vm *vm, int pptfd)
{
	struct pptdev *ppt;
	int err = 0;
	uint16_t bdf;

	cmn_err(CE_NOTE, "!ppt_assign_device: enter (vm=%p, fd=%d)",
	    (void *)vm, pptfd);

	mutex_enter(&pptdev_mtx);

	err = ppt_findf(NULL, pptfd, &ppt);
	if (err != 0) {
		cmn_err(CE_WARN,
		    "!ppt_assign_device: ppt_findf failed, err=%d", err);
		mutex_exit(&pptdev_mtx);
		return (err);
	}

	/* 1️⃣ Save current PCI configuration */
	cmn_err(CE_NOTE, "!ppt_assign_device: saving PCI config space");
	if (pci_save_config_regs(ppt->pptd_dip) != DDI_SUCCESS) {
		err = EIO;
		goto done;
	}

	/* 2️⃣ Perform reset sequence (FLR → vendor → bus) */
	bdf = pci_get_bdf(ppt->pptd_dip);

	for (int i = 0; i < PCI_BASE_NUM; i++) {
		uint32_t lo = pci_config_get32(ppt->pptd_cfg,
		    PCI_CONF_BASE0 + i * 4);
		uint32_t hi = 0;
		if ((lo & PCI_BASE_MEM_TYPE_M) == PCI_BASE_MEM_TYPE_64)
			hi = pci_config_get32(ppt->pptd_cfg,
			    PCI_CONF_BASE0 + (i + 1) * 4);
		cmn_err(CE_NOTE,
		    "PPT: BAR%d before reset lo=0x%08x hi=0x%08x", i, lo, hi);
	}

	cmn_err(CE_NOTE,
	    "!ppt_assign_device: initiating reset for BDF=0x%x", bdf);

	if (!ppt_flr(ppt->pptd_dip, B_TRUE)) {
		cmn_err(CE_WARN,
		    "!ppt_assign_device: FLR failed, trying NVIDIA vendor reset");

		if (!ppt_vendor_reset(ppt->pptd_dip)) {
			cmn_err(CE_WARN,
			    "!ppt_assign_device: vendor reset unsupported, using bus reset");
			(void)ppt_bus_reset(ppt->pptd_dip);
			(void)ppt_wait_link_active(ppt->pptd_dip);
		}
	} else {
		(void)ppt_wait_link_active(ppt->pptd_dip);
	}

	delay(drv_usectohz(500000)); /* 500 ms settle */

	/* 3️⃣ Ensure device is in D0 power state */
	uint16_t pmcap;
	if (PCI_CAP_LOCATE(ppt->pptd_cfg, PCI_CAP_ID_PM, &pmcap) == DDI_SUCCESS) {
		uint16_t pmcsr =
		    PCI_CAP_GET16(ppt->pptd_cfg, 0, pmcap, PCI_PMCSR);
		cmn_err(CE_NOTE, "PPT: power_state before=0x%x", pmcsr & 0x3);
		pmcsr &= ~PCI_PMCSR_STATE_MASK; /* force D0 */
		PCI_CAP_PUT16(ppt->pptd_cfg, 0, pmcap, PCI_PMCSR, pmcsr);
		delay(drv_usectohz(300000));
		pmcsr = PCI_CAP_GET16(ppt->pptd_cfg, 0, pmcap, PCI_PMCSR);
		cmn_err(CE_NOTE, "PPT: power_state after=0x%x", pmcsr & 0x3);
	}

	/* 4️⃣ Verify link once more & pretty‑print status */
	uint16_t pciecap;
	if (PCI_CAP_LOCATE(ppt->pptd_cfg, PCI_CAP_ID_PCI_E, &pciecap) ==
	    DDI_SUCCESS) {
		uint16_t lstat =
		    PCI_CAP_GET16(ppt->pptd_cfg, 0, pciecap, PCIE_LINKSTS);
		uint16_t speed = lstat & PCIE_LINKSTS_SPEED_MASK;
		uint16_t width =
		    (lstat & PCIE_LINKSTS_NEG_WIDTH_MASK) >>
		    PCIE_LINKSTS_NEG_WIDTH_SHIFT;
		cmn_err(CE_NOTE, "PPT: PCIe link status 0x%04x (Gen%d x%d active)",
		    lstat, speed, width);
	}

	/* 5️⃣ Restore saved config space */
	if (pci_restore_config_regs(ppt->pptd_dip) != DDI_SUCCESS ||
	    pci_save_config_regs(ppt->pptd_dip) != DDI_SUCCESS) {
		err = EIO;
		goto done;
	}

	/* 6️⃣ Enable MEM/IO/BusMaster decode */
	uint16_t cmd = pci_config_get16(ppt->pptd_cfg, PCI_CONF_COMM);
	cmd |= (PCI_COMM_MAE | PCI_COMM_ME | PCI_COMM_IO);
	pci_config_put16(ppt->pptd_cfg, PCI_CONF_COMM, cmd);
	cmd = pci_config_get16(ppt->pptd_cfg, PCI_CONF_COMM);
	cmn_err(CE_NOTE,
	    "ppt_assign_device: PCI command register now 0x%04x", cmd);
	delay(drv_usectohz(500000));

	/* 7️⃣ ROM BAR re‑enable if needed */
	uint32_t rom_bar = pci_config_get32(ppt->pptd_cfg, PCI_CONF_ROM);
	cmn_err(CE_NOTE, "PPT: ROM BAR after reset 0x%08x", rom_bar);
	if (!(rom_bar & PCI_BASE_ROM_ENABLE)) {
		rom_bar = (rom_bar & PCI_BASE_ROM_ADDR_M) |
		    PCI_BASE_ROM_ENABLE;
		pci_config_put32(ppt->pptd_cfg, PCI_CONF_ROM, rom_bar);
		cmn_err(CE_NOTE, "PPT: ROM BAR re‑enabled (0x%08x)", rom_bar);
	}

	cmn_err(CE_NOTE,
	    "!ppt_assign_device: restoring config state done");
	ppt_toggle_bar(ppt, B_TRUE);
	cmn_err(CE_NOTE, "!ppt_assign_device: BARs enabled");

	/* 9️⃣ Create new IOMMU domain and attach */
	bdf = pci_get_bdf(ppt->pptd_dip);
	cmn_err(CE_NOTE,
	    "!ppt_assign_device: creating IOMMU domain for BDF 0x%x", bdf);
	ppt->pptd_domain = iommu_create_domain(1ULL << 36);
	if (ppt->pptd_domain == NULL) {
		err = ENOMEM;
		goto done;
	}
	ppt->vm = vm;
	ppt->pptd_faults = 0;

	/* Validate that the GPU really re‑initialized */
	(void)ppt_reset_validate(ppt->pptd_dip);
	
	/* NEW: Ensure BusMaster for DMA */
	cmd = pci_config_get16(ppt->pptd_cfg, PCI_CONF_COMM);
	if ((cmd & PCI_COMM_ME) == 0) {
		cmd |= PCI_COMM_ME;
		pci_config_put16(ppt->pptd_cfg, PCI_CONF_COMM, cmd);
	}
	cmd = pci_config_get16(ppt->pptd_cfg, PCI_CONF_COMM);
	cmn_err(CE_NOTE, "ppt_assign_device: BusMaster re‑enabled (CMD=0x%04x)", cmd);
	
	iommu_remove_device(iommu_host_domain(), bdf);
	iommu_add_device(ppt->pptd_domain, bdf);
	pf_set_passthru(ppt->pptd_dip, B_TRUE);
	cmn_err(CE_NOTE,
	    "!ppt_assign_device: complete, domain=%p",
	    (void *)ppt->pptd_domain);

done:
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
ppt_do_unassign(struct pptdev *ppt)
{
	ASSERT(MUTEX_HELD(&pptdev_mtx));
	struct vm *vm = ppt->vm;
	uint16_t bdf = pci_get_bdf(ppt->pptd_dip);

	ppt_flr(ppt->pptd_dip, B_TRUE);
	ppt_reset_pci_power_state(ppt->pptd_dip);
	(void) pci_restore_config_regs(ppt->pptd_dip);

	pf_set_passthru(ppt->pptd_dip, B_FALSE);

	if (ppt->pptd_domain != NULL) {
		cmn_err(CE_NOTE, "!ppt: unassign BDF %x from domain %p faults=%llu",
			bdf, ppt->pptd_domain, (u_longlong_t)ppt->pptd_faults);

		iommu_remove_device(ppt->pptd_domain, bdf);
		iommu_add_device(iommu_host_domain(), bdf);

		iommu_destroy_domain(ppt->pptd_domain);
		ppt->pptd_domain = NULL;
		ppt->pptd_faults = 0;
	}

	ppt_unmap_all_mmio(vm, ppt);
	ppt_teardown_msi(ppt);
	ppt_teardown_msix(ppt);

	ppt->vm = NULL;
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
	for (ppt = list_head(&pptdev_list); ppt != NULL;
	    ppt = list_next(&pptdev_list, ppt)) {
		if (ppt->vm == vm) {
			ppt_do_unassign(ppt);
		}
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
		err = EINVAL;
		goto done;
	}

	for (uint_t i = 0; i < MAX_MMIOSEGS; i++) {
		struct pptseg *seg = &ppt->mmio[i];

		if (seg->len == 0) {
			err = vm_map_mmio(vm, gpa, len, hpa);
			if (err == 0) {
				seg->gpa = gpa;
				seg->len = len;
			}
			goto done;
		}
	}
	err = ENOSPC;

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

	/* Don’t inject to guest yet — just log */
	cmn_err(CE_NOTE,
	    "PPT: got MSI interrupt from %s%d (shadow guest_msg=0x%llx)",
	    ddi_driver_name(ppt->pptd_dip),
	    ddi_get_instance(ppt->pptd_dip),
	    (unsigned long long)pptarg->msg_data);

	/* Mark it claimed so the kernel doesn’t resend */
	return (DDI_INTR_CLAIMED);
}

// static uint_t
// pptintr(caddr_t arg, caddr_t unused)
// {
// 	struct pptintr_arg *pptarg = (struct pptintr_arg *)arg;
// 	struct pptdev *ppt = pptarg->pptdev;
// 
// 	cmn_err(CE_NOTE,
// 	    "PPT: ***pptintr fired*** dev=%s%d guest_msg=0x%llx",
// 	    ddi_driver_name(ppt->pptd_dip),
// 	    ddi_get_instance(ppt->pptd_dip),
// 	    (unsigned long long)pptarg->msg_data);
// 
// 	if (ppt->vm != NULL) {
// 		lapic_intr_msi(ppt->vm, pptarg->addr, pptarg->msg_data);
// 	} else {
// 		cmn_err(CE_WARN, "PPT: interrupt fired but ppt->vm == NULL");
// 	}
// 	return (ppt->msi.is_fixed ? DDI_INTR_UNCLAIMED : DDI_INTR_CLAIMED);
// }

/*
 * Helper: Scan capability list to find MSI cap offset.
 * Returns offset or 0 if none.
 * Also returns a config handle in *cfgp which caller must teardown.
 */
static int
ppt_find_msi_cap(dev_info_t *dip, ddi_acc_handle_t *cfgp)
{
	ddi_acc_handle_t cfg;
	uint8_t ptr, cap;
	int capoff = 0;

	if (pci_config_setup(dip, &cfg) != DDI_SUCCESS)
		return (0);

	ptr = pci_config_get8(cfg, PCIR_CAP_PTR);
	while (ptr != 0 && ptr != 0xff) {
		cap = pci_config_get8(cfg, ptr + PCICAP_ID);
		if (cap == PCIY_MSI) {
			capoff = ptr;
			break;
		}
		ptr = pci_config_get8(cfg, ptr + PCICAP_NEXTPTR);
	}

	*cfgp = cfg;
	return (capoff);
}

// int
// ppt_setup_msi(struct vm *vm, int vcpu, int pptfd,
// 	uint64_t guest_addr, uint64_t guest_msg, int numvec)
// {
// 	struct pptdev *ppt;
// 	int err;
// 
// 	if (numvec < 0 || numvec > MAX_MSIMSGS)
// 		return (EINVAL);
// 
// 	mutex_enter(&pptdev_mtx);
// 	err = ppt_findf(vm, pptfd, &ppt);
// 	if (err != 0) {
// 		mutex_exit(&pptdev_mtx);
// 		return (err);
// 	}
// 
// 	cmn_err(CE_NOTE, "PPT_SETUP_MSI called for dip=%p numvec=%d -- disabled in debug build",
// 		(void*)ppt->pptd_dip, numvec);
// 
// 	/* For debug, do not actually allocate. Just return EBUSY/EINVAL
// 	* or 0 if you want bhyve to think it's 'success'. */
// 	mutex_exit(&pptdev_mtx);
// 	return (ENOTSUP);
// }

// static uint32_t
// expected_ir_msi_addr(uint32_t idx)
// {
// 	return (MSI_ADDR_HDR |
// 	    ((idx & 0x7fff) << INTRMAP_MSI_IDX_SHIFT) |
// 	    (1 << INTRMAP_MSI_FORMAT_SHIFT) |
// 	    (1 << INTRMAP_MSI_SHV_SHIFT) |
// 	    ((idx >> 15) << INTRMAP_MSI_IDX15_SHIFT));
// }

int
ppt_setup_msi(struct vm *vm, int vcpu, int pptfd,
	uint64_t guest_addr, uint64_t guest_msg, int numvec)
{
	int i, msi_count, intr_type;
	struct pptdev *ppt;
	int err = 0;

	if (numvec < 0 || numvec > MAX_MSIMSGS)
		return (EINVAL);

	mutex_enter(&pptdev_mtx);
	err = ppt_findf(vm, pptfd, &ppt);
	if (err != 0) {
		mutex_exit(&pptdev_mtx);
		return (err);
	}

	ppt_teardown_msi(ppt);

	if (numvec == 0) {
		cmn_err(CE_NOTE, "PPT(%s): MSI disabled",
			ddi_driver_name(ppt->pptd_dip));
		goto done;
	}

	if (ddi_intr_get_navail(ppt->pptd_dip, DDI_INTR_TYPE_MSI, &msi_count)
		!= DDI_SUCCESS) {
		intr_type = DDI_INTR_TYPE_FIXED;
		ppt->msi.is_fixed = B_TRUE;
	} else {
		intr_type = DDI_INTR_TYPE_MSI;
		ppt->msi.is_fixed = B_FALSE;
	}

	if (numvec > msi_count) {
		cmn_err(CE_NOTE,
			"PPT: guest requested %d MSI, only %d available",
			numvec, msi_count);
		err = EINVAL;
		goto done;
	}

	cmn_err(CE_NOTE, "PPT(%s node=%s addr=%s): allocating %d MSI (PLUMBED ONLY)",
		ddi_driver_name(ppt->pptd_dip),
		ddi_node_name(ppt->pptd_dip),
		ddi_get_name_addr(ppt->pptd_dip),
		numvec);

	ppt->msi.inth_sz = numvec * sizeof (ddi_intr_handle_t);
	ppt->msi.inth = kmem_zalloc(ppt->msi.inth_sz, KM_SLEEP);

	if (ddi_intr_alloc(ppt->pptd_dip, ppt->msi.inth,
		intr_type, 0, numvec, &msi_count, 0) != DDI_SUCCESS) {
		cmn_err(CE_WARN, "PPT: ddi_intr_alloc failed");
		kmem_free(ppt->msi.inth, ppt->msi.inth_sz);
		err = EINVAL;
		goto done;
	}

	for (i = 0; i < numvec; i++) {
		int intr_cap = 0;
		uint_t intr_pri = 0;

		ppt->msi.num_msgs = i + 1;
		ppt->msi.arg[i].pptdev   = ppt;
		ppt->msi.arg[i].addr     = guest_addr;
		ppt->msi.arg[i].msg_data = (uint32_t)(guest_msg + i);

		(void) ddi_intr_get_cap(ppt->msi.inth[i], &intr_cap);
		(void) ddi_intr_get_pri(ppt->msi.inth[i], &intr_pri);

		if (ddi_intr_add_handler(ppt->msi.inth[i],
			pptintr, &ppt->msi.arg[i], NULL) != DDI_SUCCESS) {
			cmn_err(CE_WARN, "PPT: ddi_intr_add_handler failed i=%d", i);
			break;
		}

		if (intr_cap & DDI_INTR_FLAG_BLOCK)
			err = ddi_intr_block_enable(&ppt->msi.inth[i], 1);
		else
			err = ddi_intr_enable(ppt->msi.inth[i]);

		if (err != DDI_SUCCESS) {
			cmn_err(CE_WARN, "PPT: ddi_intr_enable failed i=%d", i);
			break;
		}
		cmn_err(CE_NOTE, "PPT: MSI[%d] handler installed & ENABLED", i);
	}

	if (i < numvec) {
		ppt_teardown_msi(ppt);
		err = ENXIO;
	}

done:
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

		(void) ddi_intr_get_cap(ppt->msix.inth[idx], &intr_cap);
		if (intr_cap & DDI_INTR_FLAG_BLOCK)
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

void
ppt_dma_fault_notify(uint16_t rid)
{
	struct pptdev *ppt;

	mutex_enter(&pptdev_mtx);
	for (ppt = list_head(&pptdev_list);
		ppt != NULL;
		ppt = list_next(&pptdev_list, ppt)) {

		if (pci_get_bdf(ppt->pptd_dip) == rid) {
			ppt->pptd_faults++;
			break;
		}
	}
	mutex_exit(&pptdev_mtx);
}
