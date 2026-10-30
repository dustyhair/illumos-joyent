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
#include <sys/lx_cap.h>
#include <sys/lx_brand.h>
#include <sys/proc.h>

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

	if ((error = lx_userns_map_write_scope(ns, opener->luc_ns)) != 0)
		return (error);
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

void
lx_userns_exec(proc_t *process, const cred_t *cred)
{
	const lx_userns_native_t *data = (void *)crgetbrand(cred);
	lx_userns_t *old, *owner = NULL;
	lx_proc_data_t *pd = ptolxproc(process);

	VERIFY(process->p_brand == &lx_brand && pd != NULL);
	if (data == NULL && pd->l_userns_mmap == NULL)
		return;
	if (data != NULL && data->un_cred.bc_brand == &lx_brand) {
		owner = data->un_namespace;
		lx_userns_hold(owner);
	}
	mutex_enter(&process->p_lock);
	old = pd->l_userns_mmap;
	pd->l_userns_mmap = owner;
	pd->l_flags &= ~LX_PROC_NO_DUMP;
	mutex_exit(&process->p_lock);
	lx_userns_rele(old);
}

/* The credential snapshot alone cannot identify the address-space owner. */
static boolean_t
lx_userns_nondump(proc_t *process, lx_userns_t **memory)
{
	boolean_t held = MUTEX_HELD(&process->p_lock), nodump;
	lx_proc_data_t *pd;

	*memory = NULL;
	if (!held)
		mutex_enter(&process->p_lock);
	nodump = (process->p_flag & SNOCD) != 0;
	if (process->p_brand == &lx_brand &&
	    (pd = ptolxproc(process)) != NULL) {
		nodump |= (pd->l_flags & LX_PROC_NO_DUMP) != 0;
		if (nodump && pd->l_userns_mmap != NULL) {
			*memory = pd->l_userns_mmap;
			lx_userns_hold(*memory);
		}
	}
	if (!held)
		mutex_exit(&process->p_lock);
	return (nodump);
}

int
lx_userns_cred_perm(const cred_t *source, proc_t *process,
    const cred_t *target, int mode)
{
	const lx_userns_native_t *from = (void *)crgetbrand(source);
	const lx_userns_native_t *to = (void *)crgetbrand(target);
	lx_userns_cred_t actor = { 0 };
	lx_userns_t *target_ns, *root, *memory;
	boolean_t samezone, nodump, allowed;
	uint64_t mask;

	if (from != NULL && from->un_cred.bc_brand != &lx_brand)
		return (EACCES);
	if (to != NULL && to->un_cred.bc_brand != &lx_brand)
		return (EACCES);
	VERIFY(from != NULL || to != NULL);
	samezone = crgetzone(source) == crgetzone(target);
	if (!samezone && from != NULL)
		return (EACCES);
	root = from != NULL ? from->un_namespace : to->un_namespace;
	while (root->un_parent != NULL)
		root = root->un_parent;
	actor.luc_ns = from != NULL ? from->un_namespace : root;
	actor.luc_uid = crgetuid(source);
	actor.luc_gid = crgetgid(source);
	if (from != NULL)
		actor.luc_caps = from->un_caps;
	target_ns = to != NULL ? to->un_namespace : root;

	/*
	 * unshare changes credentials, not ownership of an existing address
	 * space. A non-dumpable image needs authority in that older namespace.
	 */
	nodump = lx_userns_nondump(process, &memory);
	if (nodump) {
		allowed = samezone && lx_userns_has_cap(&actor,
		    memory != NULL ? memory : root, LX_USERNS_PTRACE);
		lx_userns_rele(memory);
		if (!allowed && (from != NULL ||
		    secpolicy_proc_access(source) != 0))
			return (EACCES);
	}
	/* Cross-zone native administration still follows native zone policy. */
	if (!samezone)
		return (BRAND_CRED_DEFER);
	if (lx_userns_has_cap(&actor, target_ns, LX_USERNS_PTRACE))
		return (0);
	if (from == NULL) {
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

/*
 * Decode the immutable file credential or current thread credential. The
 * caller holds both the native credential and the scope used for its root.
 * Initial-namespace authority uses the existing native mappings needed for
 * map files only. SETFCAP has no native mapping yet and remains absent.
 */
int
lx_userns_map_actor(const cred_t *cred, lx_userns_t *scope,
    lx_userns_cred_t *actor)
{
	const lx_userns_native_t *data = (void *)crgetbrand(cred);
	lx_userns_t *root = scope, *other;

	while (root->un_parent != NULL)
		root = root->un_parent;
	bzero(actor, sizeof (*actor));
	actor->luc_uid = crgetuid(cred);
	actor->luc_gid = crgetgid(cred);
	if (data != NULL) {
		if (data->un_cred.bc_brand != &lx_brand)
			return (EACCES);
		other = data->un_namespace;
		while (other->un_parent != NULL)
			other = other->un_parent;
		if (other != root)
			return (EACCES);
		actor->luc_ns = data->un_namespace;
		actor->luc_caps = data->un_caps;
		return (0);
	}
	actor->luc_ns = root;
	lx_cap_native_get(cred, &actor->luc_caps);
	return (0);
}

int
lx_userns_groups_allowed(lx_userns_t *ns)
{
	int allowed;

	mutex_enter(&ns->un_lock);
	allowed = ns->un_model.lum_setgroups;
	mutex_exit(&ns->un_lock);
	return (allowed);
}

/* Linux checks the captured opener's namespace before copying map data. */
int
lx_userns_map_write_scope(lx_userns_t *ns, lx_userns_t *opener)
{
	if (ns->un_parent == NULL || (opener != ns && opener != ns->un_parent))
		return (EPERM);
	return (0);
}

int
lx_userns_caps_get(const cred_t *cred, lx_caps_t *caps)
{
	const lx_userns_native_t *data = (void *)crgetbrand(cred);

	if (data == NULL || data->un_cred.bc_brand != &lx_brand)
		return (EACCES);
	*caps = data->un_caps;
	return (0);
}

/* Caller owns the returned immutable credential, with one reference. */
static cred_t *
lx_userns_with_caps(const cred_t *source, const lx_caps_t *caps)
{
	const lx_userns_native_t *data = (void *)crgetbrand(source);
	lx_userns_cred_t actor;

	VERIFY(data != NULL && data->un_cred.bc_brand == &lx_brand);
	actor.luc_ns = data->un_namespace;
	actor.luc_uid = crgetuid(source);
	actor.luc_gid = crgetgid(source);
	actor.luc_caps = *caps;
	return (lx_userns_native_for((cred_t *)source, &actor));
}

/*
 * Scoped t_cred changes are serialized by p_lock. Native entry cannot refresh
 * them from p_cred. The latter is a conservative union for process-wide
 * access checks, not an authority source for individual Linux tasks.
 */
void
lx_userns_publish(kthread_t *task, cred_t *replacement)
{
	proc_t *p = task->t_procp;
	cred_t *base, *summary, *old_process, *old_task;
	lx_userns_native_t *data;
	lx_caps_t zero = { 0 }, part;
	kthread_t *t;

	VERIFY(MUTEX_NOT_HELD(&p->p_lock));
	VERIFY((ptolxproc(p)->l_flags & LX_PROC_USERNS) != 0);
	mutex_enter(&p->p_crlock);
	crhold(base = p->p_cred);
	mutex_exit(&p->p_crlock);
	summary = lx_userns_with_caps(base, &zero);
	crfree(base);
	data = (void *)crgetbrand(summary);

	mutex_enter(&p->p_lock);
	mutex_enter(&p->p_crlock);
	old_task = task->t_cred;
	crhold(replacement);
	task->t_cred = replacement;
	t = p->p_tlist;
	if (t != NULL) {
		do {
			const lx_userns_native_t *member =
			    (void *)crgetbrand(t->t_cred);

			if ((t->t_proc_flag & TP_KTHREAD) != 0)
				continue;
			VERIFY(member != NULL &&
			    member->un_cred.bc_brand == &lx_brand &&
			    member->un_namespace == data->un_namespace);
			VERIFY0(lx_userns_caps_get(t->t_cred, &part));
			data->un_caps.lc_effective |= part.lc_effective;
			data->un_caps.lc_permitted |= part.lc_permitted;
			data->un_caps.lc_inheritable |= part.lc_inheritable;
			data->un_caps.lc_bounding |= part.lc_bounding;
			data->un_caps.lc_ambient |= part.lc_ambient;
		} while ((t = t->t_forw) != p->p_tlist);
	}
	old_process = p->p_cred;
	p->p_cred = summary;
	mutex_exit(&p->p_crlock);
	mutex_exit(&p->p_lock);
	crfree(old_process);
	crfree(old_task);
}

void
lx_userns_forklwp(klwp_t *parent, klwp_t *child)
{
	proc_t *p = lwptoproc(parent);

	if ((ptolxproc(p)->l_flags & LX_PROC_USERNS) != 0)
		lx_userns_publish(lwptot(child), lwptot(parent)->t_cred);
}

void
lx_userns_exitlwp(klwp_t *lwp)
{
	lx_caps_t zero = { 0 };
	cred_t *empty;

	if ((ptolxproc(lwptoproc(lwp))->l_flags & LX_PROC_USERNS) == 0)
		return;
	/* A departing task cannot retain authority in the process union. */
	empty = lx_userns_with_caps(lwptot(lwp)->t_cred, &zero);
	lx_userns_publish(lwptot(lwp), empty);
	crfree(empty);
}

int
lx_userns_capset(uint64_t effective, uint64_t permitted,
    uint64_t inheritable)
{
	lx_caps_t caps;
	cred_t *replacement;
	int error;

	if ((error = lx_userns_caps_get(CRED(), &caps)) != 0 ||
	    (error = lx_caps_set(&caps, effective, permitted,
	    inheritable)) != 0)
		return (error);
	replacement = lx_userns_with_caps(CRED(), &caps);
	lx_userns_publish(curthread, replacement);
	crfree(replacement);
	return (0);
}

boolean_t
lx_userns_prctl(int option, uintptr_t a2, uintptr_t a3, uintptr_t a4,
    uintptr_t a5, long *result)
{
	lx_caps_t caps;
	cred_t *replacement;
	int error = 0;

	if (ptolxproc(curproc) == NULL ||
	    (ptolxproc(curproc)->l_flags & LX_PROC_USERNS) == 0)
		return (B_FALSE);
	VERIFY0(lx_userns_caps_get(CRED(), &caps));
	switch (option) {
	case 7: /* PR_GET_KEEPCAPS */
		*result = (caps.lc_securebits & LX_SEC_KEEP_CAPS) != 0;
		return (B_TRUE);
	case 8: /* PR_SET_KEEPCAPS */
		error = a2 > 1 ? EINVAL : lx_caps_keep(&caps, a2);
		break;
	case 23: /* PR_CAPBSET_READ */
		if (a2 > LX_CAP_LAST) {
			error = EINVAL;
			break;
		}
		*result = (caps.lc_bounding & LX_CAP_BIT(a2)) != 0;
		return (B_TRUE);
	case 24: /* PR_CAPBSET_DROP: permission precedes range validation. */
		error = lx_caps_drop(&caps, a2 > UINT32_MAX ? UINT32_MAX : a2);
		break;
	case 27: /* PR_GET_SECUREBITS */
		*result = caps.lc_securebits;
		return (B_TRUE);
	case 28: /* PR_SET_SECUREBITS */
		error = a2 > UINT32_MAX ? EPERM : lx_caps_securebits(&caps, a2);
		break;
	case 47: /* PR_CAP_AMBIENT */
		if (a4 != 0 || a5 != 0 || a2 > 4 || a2 == 0 ||
		    (a2 == 4 ? a3 != 0 : a3 > LX_CAP_LAST)) {
			error = EINVAL;
			break;
		}
		if (a2 == 1) {
			*result = (caps.lc_ambient & LX_CAP_BIT(a3)) != 0;
			return (B_TRUE);
		}
		error = lx_caps_ambient(&caps, a2, a3);
		break;
	default:
		return (B_FALSE);
	}
	if (error == 0) {
		replacement = lx_userns_with_caps(CRED(), &caps);
		lx_userns_publish(curthread, replacement);
		crfree(replacement);
	}
	*result = error == 0 ? 0 : set_errno(error);
	return (B_TRUE);
}
