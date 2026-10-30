/*
 * CDDL version 1.0, http://www.illumos.org/license/CDDL.
 */
#ifndef _LX_USERNS_MODEL_H
#define	_LX_USERNS_MODEL_H

#ifdef _KERNEL
#include <sys/types.h>
#include <sys/inttypes.h>
#else
#include <stdint.h>
#include <stddef.h>
#endif

#define	LX_IDMAP_EXTENTS	340
#define	LX_ID_INVALID	UINT32_MAX
#define	LX_ID_OVERFLOW	65534U
#define	LX_CAP_LAST	40
#define	LX_CAP_ALL	((UINT64_C(1) << (LX_CAP_LAST + 1)) - 1)
#define	LX_CAP_BIT(n)	(UINT64_C(1) << (n))
#define	LX_CAP_CHOWN	0
#define	LX_CAP_FOWNER	3
#define	LX_CAP_FSETID	4
#define	LX_CAP_SETGID	6
#define	LX_CAP_SETUID	7
#define	LX_CAP_SETPCAP	8
#define	LX_CAP_SYS_ADMIN	21
#define	LX_CAP_SYS_RESOURCE	24
#define	LX_CAP_SETFCAP	31

#define	LX_SEC_NOROOT		0x01U
#define	LX_SEC_NO_SETUID_FIXUP	0x04U
#define	LX_SEC_KEEP_CAPS		0x10U
#define	LX_SEC_KEEP_CAPS_LOCKED	0x20U
#define	LX_SEC_NO_AMBIENT_RAISE	0x40U
#define	LX_SEC_LOCKS		0xaaU
#define	LX_SEC_SUPPORTED	0xffU

typedef struct lx_id_extent {
	uint32_t lie_inside;
	uint32_t lie_outside;
	uint32_t lie_count;
} lx_id_extent_t;

typedef struct lx_idmap {
	uint32_t lim_count;
	lx_id_extent_t lim_extents[LX_IDMAP_EXTENTS];
} lx_idmap_t;

typedef struct lx_caps {
	uint64_t lc_effective;
	uint64_t lc_permitted;
	uint64_t lc_inheritable;
	uint64_t lc_bounding;
	uint64_t lc_ambient;
	uint32_t lc_securebits;
} lx_caps_t;

/* The namespace owner is recorded as a global ID, never a visible ID. */
typedef struct lx_userns_model {
	const struct lx_userns_model *lum_parent;
	uint32_t lum_owner;
	uint32_t lum_level;
	int lum_creator_setfcap;
	int lum_setgroups;
	lx_idmap_t lum_uids;
	lx_idmap_t lum_gids;
} lx_userns_model_t;

/* File-open credentials must be captured separately from the current task. */
typedef struct lx_userns_actor {
	const lx_userns_model_t *lua_ns;
	uint32_t lua_uid;
	uint32_t lua_gid;
	uint64_t lua_effective;
} lx_userns_actor_t;

extern void lx_idmap_identity(lx_idmap_t *);
extern int lx_idmap_parse(const char *, size_t, lx_idmap_t *);
extern int lx_idmap_compose(lx_idmap_t *, const lx_idmap_t *);
extern uint32_t lx_idmap_down(const lx_idmap_t *, uint32_t);
extern uint32_t lx_idmap_up(const lx_idmap_t *, uint32_t);
extern void lx_caps_new_namespace(lx_caps_t *);
extern int lx_caps_set(lx_caps_t *, uint64_t, uint64_t, uint64_t);
extern int lx_caps_drop(lx_caps_t *, unsigned int);
extern int lx_caps_securebits(lx_caps_t *, uint32_t);
extern int lx_caps_keep(lx_caps_t *, unsigned int);
extern int lx_caps_ambient(lx_caps_t *, unsigned int, unsigned int);
extern void lx_caps_exec_plain(lx_caps_t *, int, int, int);
extern void lx_caps_uid_change(lx_caps_t *, unsigned int, unsigned int);
extern int lx_userns_capable(const lx_userns_actor_t *,
    const lx_userns_model_t *, unsigned int);
extern void lx_userns_model_root(lx_userns_model_t *);
extern int lx_userns_model_child(lx_userns_model_t *,
    const lx_userns_actor_t *);
extern int lx_userns_model_write_check(const lx_userns_model_t *, int,
    const lx_userns_actor_t *);
extern int lx_userns_model_write(lx_userns_model_t *, int,
    const lx_userns_actor_t *, const lx_userns_actor_t *, lx_idmap_t *);
extern int lx_userns_model_groups(lx_userns_model_t *,
    const lx_userns_actor_t *, int);

#endif /* _LX_USERNS_MODEL_H */
