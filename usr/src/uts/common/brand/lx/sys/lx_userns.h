/*
 * CDDL version 1.0, http://www.illumos.org/license/CDDL.
 */
#ifndef _SYS_LX_USERNS_H
#define	_SYS_LX_USERNS_H
#include <sys/types.h>
#include <sys/cred.h>
#include <lx_userns_model.h>

/* Internal objects only; no Linux namespace syscall is enabled here. */
typedef struct lx_userns lx_userns_t;
typedef struct lx_userns_cred {
	lx_userns_t *luc_ns;
	uint32_t luc_uid;
	uint32_t luc_gid;
	lx_caps_t luc_caps;
} lx_userns_cred_t;

extern lx_userns_t *lx_userns_root_create(void);
extern int lx_userns_child_create(const lx_userns_cred_t *, lx_userns_t **);
extern void lx_userns_hold(lx_userns_t *);
extern void lx_userns_rele(lx_userns_t *);
extern int lx_userns_map_write(lx_userns_t *, int, const lx_userns_cred_t *,
    const lx_userns_cred_t *, const char *, size_t, offset_t);
extern void lx_userns_map_read(lx_userns_t *, int, lx_userns_t *, lx_idmap_t *);
extern uint32_t lx_userns_id(lx_userns_t *, int, uint32_t, int);
extern int lx_userns_groups_write(lx_userns_t *, const lx_userns_cred_t *,
    const char *, size_t, offset_t);
extern int lx_userns_may_setgroups(lx_userns_t *);
extern int lx_userns_has_cap(const lx_userns_cred_t *, lx_userns_t *, uint_t);
extern cred_t *lx_userns_native_cred(cred_t *);

#endif /* _SYS_LX_USERNS_H */
