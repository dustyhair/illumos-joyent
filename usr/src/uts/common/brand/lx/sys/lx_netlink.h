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

/*
 * Copyright 2026 Jonathan Wagner.
 */

#ifndef _SYS_LX_NETLINK_H
#define	_SYS_LX_NETLINK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

#ifdef _KERNEL
extern void lx_netlink_input_uevent(zoneid_t, const char *, uint_t);
#endif

#ifdef __cplusplus
}
#endif

#endif /* _SYS_LX_NETLINK_H */
