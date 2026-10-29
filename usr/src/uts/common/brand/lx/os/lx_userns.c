/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * A copy is available at http://www.illumos.org/license/CDDL.
 */
#include <sys/types.h>
#include <sys/atomic.h>
#include <sys/brand.h>
#include <sys/debug.h>
#include <sys/kmem.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/cred_impl.h>
#include <sys/priv_impl.h>
#include <sys/policy.h>
#include <sys/lx_userns.h>

struct lx_userns {
	uint_t un_refs;
	kmutex_t un_lock;
	struct lx_userns *un_parent;
	lx_userns_model_t un_model;
};

static lx_userns_actor_t
lx_userns_actor(const lx_userns_cred_t *cred)
{
	lx_userns_actor_t actor;

	actor.lua_ns = &cred->luc_ns->un_model;
	actor.lua_uid = cred->luc_uid;
	actor.lua_gid = cred->luc_gid;
	actor.lua_effective = cred->luc_caps.lc_effective;
	return (actor);
}

lx_userns_t *
lx_userns_root_create(void)
{
	lx_userns_t *ns = kmem_zalloc(sizeof (*ns), KM_SLEEP);

	mutex_init(&ns->un_lock, NULL, MUTEX_DEFAULT, NULL);
	ns->un_refs = 1;
	lx_userns_model_root(&ns->un_model);
	return (ns);
}

void
lx_userns_hold(lx_userns_t *ns)
{
	VERIFY3U(ns->un_refs, !=, 0);
	atomic_inc_uint(&ns->un_refs);
}

void
lx_userns_rele(lx_userns_t *ns)
{
	while (ns != NULL && atomic_dec_uint_nv(&ns->un_refs) == 0) {
		lx_userns_t *parent = ns->un_parent;

		mutex_destroy(&ns->un_lock);
		kmem_free(ns, sizeof (*ns));
		ns = parent;
	}
}

int
lx_userns_child_create(const lx_userns_cred_t *creator, lx_userns_t **out)
{
	lx_userns_t *ns = kmem_zalloc(sizeof (*ns), KM_SLEEP);
	lx_userns_t *parent = creator->luc_ns;
	lx_userns_actor_t actor = lx_userns_actor(creator);
	int error;

	*out = NULL;
	mutex_enter(&parent->un_lock);
	error = lx_userns_model_child(&ns->un_model, &actor);
	mutex_exit(&parent->un_lock);
	if (error != 0) {
		kmem_free(ns, sizeof (*ns));
		return (error);
	}
	mutex_init(&ns->un_lock, NULL, MUTEX_DEFAULT, NULL);
	ns->un_refs = 1;
	ns->un_parent = parent;
	lx_userns_hold(parent);
	*out = ns;
	return (0);
}

int
lx_userns_map_write(lx_userns_t *ns, int gid, const lx_userns_cred_t *opener,
    const lx_userns_cred_t *current, const char *buffer, size_t size,
    offset_t offset)
{
	lx_userns_actor_t open_actor = lx_userns_actor(opener);
	lx_userns_actor_t current_actor = lx_userns_actor(current);
	lx_idmap_t *map;
	int error;

	if (ns->un_parent == NULL || (opener->luc_ns != ns &&
	    opener->luc_ns != ns->un_parent))
		return (EPERM);
	if (offset != 0 || size >= 4096)
		return (EINVAL);
	map = kmem_alloc(sizeof (*map), KM_SLEEP);
	mutex_enter(&ns->un_lock);
	error = lx_userns_model_write_check(&ns->un_model, gid, &open_actor);
	if (error == 0)
		error = lx_idmap_parse(buffer, size, map);
	if (error == 0) {
		/* Hierarchy locking always goes from child to parent. */
		mutex_enter(&ns->un_parent->un_lock);
		error = lx_userns_model_write(&ns->un_model, gid,
		    &open_actor, &current_actor, map);
		mutex_exit(&ns->un_parent->un_lock);
	}
	mutex_exit(&ns->un_lock);
	kmem_free(map, sizeof (*map));
	return (error);
}

void
lx_userns_map_read(lx_userns_t *ns, int gid, lx_userns_t *reader,
    lx_idmap_t *out)
{
	const lx_idmap_t *view;
	uint32_t i;

	mutex_enter(&ns->un_lock);
	(void) memcpy(out, gid ? &ns->un_model.lum_gids :
	    &ns->un_model.lum_uids, sizeof (*out));
	mutex_exit(&ns->un_lock);
	if (reader == ns && ns->un_parent != NULL)
		reader = ns->un_parent;
	mutex_enter(&reader->un_lock);
	view = gid ? &reader->un_model.lum_gids : &reader->un_model.lum_uids;
	for (i = 0; i < out->lim_count; i++)
		out->lim_extents[i].lie_outside = lx_idmap_up(view,
		    out->lim_extents[i].lie_outside);
	mutex_exit(&reader->un_lock);
}

uint32_t
lx_userns_id(lx_userns_t *ns, int gid, uint32_t id, int down)
{
	const lx_idmap_t *map;
	uint32_t result;

	mutex_enter(&ns->un_lock);
	map = gid ? &ns->un_model.lum_gids : &ns->un_model.lum_uids;
	result = down ? lx_idmap_down(map, id) : lx_idmap_up(map, id);
	mutex_exit(&ns->un_lock);
	return (result);
}

int
lx_userns_groups_write(lx_userns_t *ns, const lx_userns_cred_t *opener,
    const char *buffer, size_t size, offset_t offset)
{
	lx_userns_actor_t actor = lx_userns_actor(opener);
	char text[8];
	size_t length, pos;
	int allow, error;

	if (offset != 0 || size >= sizeof (text))
		return (EINVAL);
	(void) memcpy(text, buffer, size);
	text[size] = '\0';
	length = strlen(text);
	if (length >= 5 && strncmp(text, "allow", 5) == 0) {
		allow = 1;
		pos = 5;
	} else if (length >= 4 && strncmp(text, "deny", 4) == 0) {
		allow = 0;
		pos = 4;
	} else {
		return (EINVAL);
	}
	for (; pos < length; pos++) {
		if (text[pos] != ' ' && text[pos] != '\t' &&
		    text[pos] != '\n' && text[pos] != '\r' &&
		    text[pos] != '\v' && text[pos] != '\f')
			return (EINVAL);
	}
	mutex_enter(&ns->un_lock);
	error = lx_userns_model_groups(&ns->un_model, &actor, allow);
	mutex_exit(&ns->un_lock);
	return (error);
}

int
lx_userns_may_setgroups(lx_userns_t *ns)
{
	int allowed;

	mutex_enter(&ns->un_lock);
	allowed = ns->un_model.lum_setgroups &&
	    ns->un_model.lum_gids.lim_count != 0;
	mutex_exit(&ns->un_lock);
	return (allowed);
}

int
lx_userns_has_cap(const lx_userns_cred_t *actor, lx_userns_t *target,
    uint_t cap)
{
	lx_userns_actor_t cred = lx_userns_actor(actor);

	return (lx_userns_capable(&cred, &target->un_model, cap));
}

/*
 * Native privileges must not mirror scoped Linux capabilities. This only
 * constructs the reduced credential. The caller still needs process-scope
 * checks, native-entry confinement, and resource-specific capability checks
 * before publishing a namespace transition.
 */
cred_t *
lx_userns_native_cred(cred_t *source)
{
	cred_t *reduced = crdup(source);
	uint_t i;

	CR_EPRIV(reduced) = CR_OEPRIV(source);
	CR_PPRIV(reduced) = CR_OPPRIV(source);
	for (i = 0; i < PRIV_NSET; i++)
		priv_intersect(priv_basic, &CR_PRIVSETS(reduced)[i]);
	CR_FLAGS(reduced) |= PRIV_AWARE | PRIV_AWARE_INHERIT;
	CR_FLAGS(reduced) &= ~(PRIV_AWARE_RESET | PRIV_PFEXEC | PRIV_XPOLICY);
	crsetcrklpd(reduced, NULL);
	return (reduced);
}

#define	LX_USERNS_PTRACE	19

/* Only namespace credentials allocate this payload; it is immutable. */
typedef struct lx_userns_native {
	brand_cred_t un_cred;
	lx_userns_t *un_namespace;
	lx_caps_t un_caps;
} lx_userns_native_t;

extern brand_t lx_brand;

cred_t *
lx_userns_native_for(cred_t *source, const lx_userns_cred_t *actor)
{
	lx_userns_native_t *data = kmem_alloc(sizeof (*data), KM_SLEEP);
	cred_t *cred = lx_userns_native_cred(source);

	/* Entry to the initial namespace must never use this constructor. */
	VERIFY(actor->luc_ns->un_parent != NULL);
	data->un_namespace = actor->luc_ns;
	lx_userns_hold(data->un_namespace);
	data->un_caps = actor->luc_caps;
	brand_cred_init(&data->un_cred, &lx_brand);
	crsetbrand(cred, &data->un_cred);
	brand_cred_rele(&data->un_cred);
	return (cred);
}

void
lx_userns_cred_free(brand_cred_t *data)
{
	lx_userns_native_t *native = (lx_userns_native_t *)data;

	lx_userns_rele(native->un_namespace);
	kmem_free(native, sizeof (*native));
}

int
lx_userns_cred_perm(const cred_t *source, const cred_t *target, int mode)
{
	const lx_userns_native_t *from = (void *)crgetbrand(source);
	const lx_userns_native_t *to = (void *)crgetbrand(target);
	lx_userns_cred_t actor = { 0 };
	lx_userns_t *target_ns, *root;
	uint64_t mask;

	/* A different brand's credential is outside every lx user namespace. */
	if (from != NULL && from->un_cred.bc_brand != &lx_brand)
		return (EACCES);
	if (to != NULL && to->un_cred.bc_brand != &lx_brand)
		return (EACCES);
	VERIFY(from != NULL || to != NULL);
	root = from != NULL ? from->un_namespace : to->un_namespace;
	while (root->un_parent != NULL)
		root = root->un_parent;
	actor.luc_ns = from != NULL ? from->un_namespace : root;
	actor.luc_uid = crgetuid(source);
	actor.luc_gid = crgetgid(source);
	if (from != NULL)
		actor.luc_caps = from->un_caps;
	target_ns = to != NULL ? to->un_namespace : root;
	/* Namespace ownership grants authority to its owner in the parent. */
	if (lx_userns_has_cap(&actor, target_ns, LX_USERNS_PTRACE))
		return (0);
	if (from == NULL) {
		/* Retain native process authority for native administrators. */
		return (secpolicy_proc_access(source) == 0 ?
		    BRAND_CRED_DEFER : EACCES);
	}
	if (to == NULL || from->un_namespace != to->un_namespace)
		return (EACCES);
	mask = (mode & PRIV_PROC_REALCREDS) ? from->un_caps.lc_permitted :
	    from->un_caps.lc_effective;
	if ((to->un_caps.lc_permitted & ~mask) != 0)
		return (EACCES);
	return (BRAND_CRED_DEFER);
}
