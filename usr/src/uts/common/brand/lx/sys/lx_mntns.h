/*
 * CDDL version 1.0, http://www.illumos.org/license/CDDL.
 */
#ifndef _SYS_LX_MNTNS_H
#define	_SYS_LX_MNTNS_H

#include <sys/types.h>
#include <sys/cred.h>
#include <sys/vnode.h>

/*
 * Private mount-tree backend. Linux namespace syscalls are not enabled by
 * these routines. Each caller owns a reference. destroy consumes a sole
 * reference only on success; rele always consumes a reference and defers
 * cleanup after the last release if mounted vnodes are still busy.
 * All path arguments are kernel strings. Returned vnodes must be released.
 */
typedef struct lx_mntns_view lx_mntns_view_t;
extern void lx_mntns_init(void);
extern boolean_t lx_mntns_busy(void);
extern void lx_mntns_fini(void);
extern void lx_mntns_view_hold(lx_mntns_view_t *);
extern void lx_mntns_view_rele(lx_mntns_view_t *);
extern int lx_mntns_view_create(const char *, cred_t *, lx_mntns_view_t **);
extern int lx_mntns_view_snapshot(const char *, cred_t *, lx_mntns_view_t **);
extern int lx_mntns_view_clone(lx_mntns_view_t *, cred_t *, lx_mntns_view_t **);
extern int lx_mntns_view_bind(lx_mntns_view_t *, vnode_t *, const char *,
    boolean_t, cred_t *);
extern int lx_mntns_view_lookup(lx_mntns_view_t *, const char *, cred_t *,
    vnode_t **);
extern int lx_mntns_view_tmpfs(lx_mntns_view_t *, const char *, cred_t *);
extern int lx_mntns_view_destroy(lx_mntns_view_t *, cred_t *);

#endif /* _SYS_LX_MNTNS_H */
