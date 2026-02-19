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
 * Copyright 2018 Joyent, Inc
 */

#ifndef _PPT_DEV_H
#define	_PPT_DEV_H

#ifdef __cplusplus
extern "C" {
#endif

#define	PPT_IOC			(('P' << 16)|('T' << 8))

#define	PPT_CFG_READ		(PPT_IOC | 0x01)
#define	PPT_CFG_WRITE		(PPT_IOC | 0x02)
#define	PPT_BAR_QUERY		(PPT_IOC | 0x03)
#define	PPT_BAR_READ		(PPT_IOC | 0x04)
#define	PPT_BAR_WRITE		(PPT_IOC | 0x05)

#define	PPT_MAXNAMELEN	32

struct ppt_cfg_io {
	uint64_t pci_off;
	uint32_t pci_width;
	uint32_t pci_data;
};
struct ppt_bar_io {
	uint32_t pbi_bar;
	uint32_t pbi_off;
	uint32_t pbi_width;
	uint32_t pbi_data;
};

struct ppt_bar_query {
	uint32_t pbq_baridx;
	uint32_t pbq_type;
	uint64_t pbq_base;
	uint64_t pbq_size;
};

/*
 * Enhanced ppt+ ioctls (safe passthrough extensions)
 */
#define PPT_CAP_IOMMU       (1 << 0)
#define PPT_CAP_IRQ_REMAP   (1 << 1)
#define PPT_CAP_BAR_INFO    (1 << 2)
#define PPT_CAP_RESET       (1 << 3)
#define PPT_CAP_MSI         (1 << 4)
#define PPT_CAP_INTX        (1 << 5)

struct ppt_caps {
        uint32_t version;   /* API version */
        uint32_t caps;      /* Bitmask of supported features */
};

#define PPT_GET_CAPS        (PPT_IOC | 0x10)

struct ppt_region_info {
        uint32_t index;       /* BAR index (0-5) */
        uint64_t phys_addr;   /* Physical address of BAR */
        uint64_t size;        /* Size of BAR */
        uint32_t flags;       /* Prefetchable, MMIO/IO, etc. */
};

#define PPT_GET_REGION_INFO (PPT_IOC | 0x11)

struct ppt_iommu_map {
	uint64_t gpa;
	uint64_t hpa;
	uint64_t size;
	uint32_t prot;   /* future: IOMMU_PROT_READ / IOMMU_PROT_WRITE */
};

/* flexible array for batch mapping */
struct ppt_iommu_map_batch {
	uint32_t count;              /* number of entries in maps[] */
	uint32_t pad;                /* explicit padding for 8-byte align */
	struct ppt_iommu_map maps[]; /* array of maps */
};

#define PPT_IOMMU_MAP     (PPT_IOC | 0x20)
#define PPT_IOMMU_UNMAP   (PPT_IOC | 0x21)
#define PPT_IOMMU_MAP_BATCH   (PPT_IOC | 0x22)
#define PPT_IOMMU_UNMAP_BATCH (PPT_IOC | 0x23)

/* Define prot mask bits */
#define IOMMU_PROT_READ   0x1
#define IOMMU_PROT_WRITE  0x2
#define IOMMU_PROT_RW     (IOMMU_PROT_READ|IOMMU_PROT_WRITE)


#ifdef __cplusplus
}
#endif

#endif /* _PPT_DEV_H */
