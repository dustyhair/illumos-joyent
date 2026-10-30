/*
 * CDDL version 1.0, http://www.illumos.org/license/CDDL.
 */
#ifndef _SYS_LX_USERNS_H
#define	_SYS_LX_USERNS_H
#include <sys/types.h>
#include <sys/cred.h>
#include <lx_userns_model.h>

struct kthread;
struct klwp;

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
extern cred_t *lx_userns_native_for(cred_t *, const lx_userns_cred_t *);
extern void lx_userns_cred_free(struct brand_cred *);
extern int lx_userns_cred_perm(const cred_t *, struct proc *,
    const cred_t *, int);
extern int lx_userns_caps_get(const cred_t *, lx_caps_t *);
extern void lx_userns_publish(struct kthread *, cred_t *);
extern void lx_userns_forklwp(struct klwp *, struct klwp *);
extern void lx_userns_exitlwp(struct klwp *);
extern int lx_userns_capset(uint64_t, uint64_t, uint64_t);
extern boolean_t lx_userns_prctl(int, uintptr_t, uintptr_t, uintptr_t,
    uintptr_t, long *);
extern void lx_userns_exec(struct proc *, const cred_t *);
extern int lx_userns_map_actor(const cred_t *, lx_userns_t *,
    lx_userns_cred_t *);
extern int lx_userns_groups_allowed(lx_userns_t *);
extern int lx_userns_map_write_scope(lx_userns_t *, lx_userns_t *);

#endif /* _SYS_LX_USERNS_H */
