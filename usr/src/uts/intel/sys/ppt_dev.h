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
#define	PPT_RESET_DEVICE	(PPT_IOC | 0x06)

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

typedef enum {
	PPT_RESET_FLR = 0,
	PPT_RESET_PM,
	PPT_RESET_BUS,
	PPT_RESET_VENDOR,
	PPT_RESET_NONE
} ppt_reset_type_t;

typedef enum {
	PPT_RESET_F_NONE = 0x0,
	PPT_RESET_F_ALLOW_FALLBACK = 0x1,
	PPT_RESET_F_FORCE = 0x2,
	PPT_RESET_F_VM_STOP = 0x4,
	PPT_RESET_F_GUEST_TRIGGER = 0x8,
	PPT_RESET_F_ASSIGN_STARTUP = 0x10
} ppt_reset_flags_t;

struct ppt_reset_req {
	uint32_t prr_method;
	uint32_t prr_flags;
	uint32_t prr_result_method;
	int32_t prr_result_error;
};

#ifdef __cplusplus
}
#endif

#endif /* _PPT_DEV_H */
