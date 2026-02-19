#ifndef _SYS_PPT_IOCTL_H
#define _SYS_PPT_IOCTL_H

#include <sys/types.h>
#include <sys/ioccom.h>

/*
 * Capability bits
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

#define PPT_GET_CAPS _IOR('P', 0x01, struct ppt_caps)

/*
 * BAR region info
 */
struct ppt_region_info {
    uint32_t index;       /* BAR index (0-5) */
    uint64_t phys_addr;   /* Physical address of BAR */
    uint64_t size;        /* Size of BAR */
    uint32_t flags;       /* Prefetchable, MMIO/IO, etc. */
};

#define PPT_GET_REGION_INFO _IOWR('P', 0x02, struct ppt_region_info)

#endif /* _SYS_PPT_IOCTL_H */
