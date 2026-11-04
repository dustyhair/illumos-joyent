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
 * CDDL HEADER END
 */

/*
 * Copyright 2026 Jonathan Wagner.
 */

#ifndef _SYS_LX_NETNS_H
#define	_SYS_LX_NETNS_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define	LX_NETNS_DEFAULT_MAX	64U

typedef struct lx_netns lx_netns_t;
struct cred;
struct netstack;
struct proc;

extern boolean_t lx_netns_entry_enabled(void);
extern int lx_netns_create(const struct cred *, lx_netns_t **);
extern void lx_netns_hold(lx_netns_t *);
extern void lx_netns_rele(lx_netns_t *);
extern struct netstack *lx_netns_stack(lx_netns_t *);
extern struct netstack *lx_netns_current(struct proc *);
extern boolean_t lx_netns_configure_allowed(const struct cred *);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_LX_NETNS_H */
