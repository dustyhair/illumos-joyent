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
#include <sys/sid.h>
#include <sys/priv_impl.h>
#include <sys/policy.h>
#include <sys/lx_userns.h>
#include <sys/lx_cap.h>
#include <sys/lx_misc.h>
#include <sys/lx_brand.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <sys/vfs.h>

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
static void
lx_userns_native_limit(cred_t *cred)
{
	uint_t i;

	for (i = 0; i < PRIV_NSET; i++)
		priv_intersect(priv_basic, &CR_PRIVSETS(cred)[i]);
	CR_FLAGS(cred) |= PRIV_AWARE | PRIV_AWARE_INHERIT;
	CR_FLAGS(cred) &= ~(PRIV_AWARE_RESET | PRIV_PFEXEC | PRIV_XPOLICY);
	crsetcrklpd(cred, NULL);
}

cred_t *
lx_userns_native_cred(cred_t *source)
{
	cred_t *reduced = crdup(source);

	CR_EPRIV(reduced) = CR_OEPRIV(source);
	CR_PPRIV(reduced) = CR_OPPRIV(source);
	lx_userns_native_limit(reduced);
	return (reduced);
}

#define	LX_USERNS_PTRACE	19

/* Only namespace credentials allocate this payload; it is immutable. */
typedef struct lx_userns_native {
	brand_cred_t un_cred;
	lx_userns_t *un_namespace;
	lx_caps_t un_caps;
	boolean_t un_exec_nodump;
} lx_userns_native_t;

extern brand_t lx_brand;

static void
lx_userns_native_attach(cred_t *cred, const lx_userns_cred_t *actor)
{
	lx_userns_native_t *data = kmem_alloc(sizeof (*data), KM_SLEEP);

	/* Entry to the initial namespace must never use this constructor. */
	VERIFY(actor->luc_ns->un_parent != NULL);
	data->un_namespace = actor->luc_ns;
	lx_userns_hold(data->un_namespace);
	data->un_caps = actor->luc_caps;
	data->un_exec_nodump = B_FALSE;
	brand_cred_init(&data->un_cred, &lx_brand);
	crsetbrand(cred, &data->un_cred);
	brand_cred_rele(&data->un_cred);
}

cred_t *
lx_userns_native_for(cred_t *source, const lx_userns_cred_t *actor)
{
	cred_t *cred = lx_userns_native_cred(source);

	lx_userns_native_attach(cred, actor);
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
	if (data != NULL && data->un_exec_nodump) {
		pd->l_flags |= LX_PROC_NO_DUMP;
		process->p_flag |= SNOCD;
	}
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
	if (process == curproc)
		return (0);
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

/* p_lock protects the list; do not inspect LWP brand data during exit. */
static kthread_t *
lx_userns_leader(proc_t *p)
{
	kthread_t *t = p->p_tlist, *first = NULL;

	ASSERT(MUTEX_HELD(&p->p_lock));
	if (t == NULL)
		return (NULL);
	do {
		if ((t->t_proc_flag & TP_KTHREAD) == 0) {
			if (t->t_tid == 1)
				return (t);
			if (first == NULL)
				first = t;
		}
	} while ((t = t->t_forw) != p->p_tlist);
	return (first);
}

/*
 * Each task owns its immutable credential. The native process identity follows
 * a live representative, while Linux capabilities conservatively cover every
 * task. Reserve a changed real UID's process count before publication, exactly
 * as native setreuid does. Allocation and credential release occur unlocked.
 */
void
lx_userns_publish(kthread_t *task, cred_t *replacement)
{
	proc_t *p = task->t_procp;
	cred_t *base, *identity, *summary, *old_process, *old_task;
	kthread_t *leader, *t;
	lx_userns_native_t *data;
	lx_caps_t zero = { 0 }, part;
	uid_t oldruid, newruid;
	boolean_t counted;

	VERIFY(MUTEX_NOT_HELD(&p->p_lock));
	VERIFY((ptolxproc(p)->l_flags & LX_PROC_USERNS) != 0);
retry:
	mutex_enter(&p->p_lock);
	leader = lx_userns_leader(p);
	mutex_enter(&p->p_crlock);
	crhold(base = p->p_cred);
	crhold(identity = leader == task ? replacement :
	    leader != NULL ? leader->t_cred : base);
	mutex_exit(&p->p_crlock);
	mutex_exit(&p->p_lock);
	summary = lx_userns_with_caps(base, &zero);
	VERIFY0(crsetresuid(summary, crgetruid(identity),
	    crgetuid(identity), crgetsuid(identity)));
	VERIFY0(crsetresgid(summary, crgetrgid(identity),
	    crgetgid(identity), crgetsgid(identity)));
	if (summary->cr_grps != identity->cr_grps &&
	    (crgetngroups(summary) != crgetngroups(identity) ||
	    bcmp(crgetgroups(summary), crgetgroups(identity),
	    crgetngroups(identity) * sizeof (gid_t)) != 0))
		VERIFY0(crsetgroups(summary, crgetngroups(identity),
		    (gid_t *)crgetgroups(identity)));
	/* Match the representative's immutable native SID payload as well. */
	if (summary->cr_ksid != identity->cr_ksid) {
		if (identity->cr_ksid != NULL)
			kcrsid_hold(identity->cr_ksid);
		if (summary->cr_ksid != NULL)
			kcrsid_rele(summary->cr_ksid);
		summary->cr_ksid = identity->cr_ksid;
	}
	oldruid = crgetruid(base);
	newruid = crgetruid(summary);
	counted = oldruid != newruid;
	if (counted) {
		mutex_enter(&pidlock);
		upcount_inc(newruid, crgetzoneid(base));
		mutex_exit(&pidlock);
	}
	data = (void *)crgetbrand(summary);
	mutex_enter(&p->p_lock);
	mutex_enter(&p->p_crlock);
	if (p->p_cred != base || lx_userns_leader(p) != leader ||
	    (leader != NULL && leader != task && leader->t_cred != identity)) {
		mutex_exit(&p->p_crlock);
		mutex_exit(&p->p_lock);
		if (counted) {
			mutex_enter(&pidlock);
			upcount_dec(newruid, crgetzoneid(base));
			mutex_exit(&pidlock);
		}
		crfree(summary);
		crfree(identity);
		crfree(base);
		goto retry;
	}
	old_task = task->t_cred;
	if (crgetuid(old_task) != crgetuid(replacement) ||
	    crgetgid(old_task) != crgetgid(replacement)) {
		p->p_flag |= SNOCD;
		ptolxproc(p)->l_flags |= LX_PROC_NO_DUMP;
	}
	crhold(replacement);
	task->t_cred = replacement;
	t = p->p_tlist;
	if (t != NULL) {
		do {
			const lx_userns_native_t *member;

			if ((t->t_proc_flag & TP_KTHREAD) != 0)
				continue;
			member = (void *)crgetbrand(t->t_cred);
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
	if (counted) {
		mutex_enter(&pidlock);
		upcount_dec(oldruid, crgetzoneid(base));
		mutex_exit(&pidlock);
	}
	crfree(old_process);
	crfree(old_task);
	crfree(identity);
	crfree(base);
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
	case 3: /* PR_GET_DUMPABLE */
		mutex_enter(&curproc->p_lock);
		*result = (curproc->p_flag & SNOCD) == 0 &&
		    (ptolxproc(curproc)->l_flags & LX_PROC_NO_DUMP) == 0;
		mutex_exit(&curproc->p_lock);
		return (B_TRUE);
	case 4: /* PR_SET_DUMPABLE */
		if (a2 > 1) {
			*result = set_errno(EINVAL);
			return (B_TRUE);
		}
		mutex_enter(&curproc->p_lock);
		if (a2 == 0) {
			curproc->p_flag |= SNOCD;
			ptolxproc(curproc)->l_flags |= LX_PROC_NO_DUMP;
		} else {
			curproc->p_flag &= ~SNOCD;
			ptolxproc(curproc)->l_flags &= ~LX_PROC_NO_DUMP;
		}
		mutex_exit(&curproc->p_lock);
		*result = 0;
		return (B_TRUE);
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

/* Native administration and namespace-relative ptrace authority. */
static boolean_t
lx_userns_tracer_capable(const cred_t *tracer, const cred_t *tracee)
{
	const lx_userns_native_t *data = (void *)crgetbrand(tracee);
	lx_userns_cred_t actor;

	VERIFY(data != NULL && data->un_cred.bc_brand == &lx_brand);
	if (crgetzone(tracer) != crgetzone(tracee))
		return (B_FALSE);
	if (lx_userns_map_actor(tracer, data->un_namespace, &actor) == 0 &&
	    lx_userns_has_cap(&actor, data->un_namespace, LX_USERNS_PTRACE))
		return (B_TRUE);
	return (crgetbrand(tracer) == NULL &&
	    secpolicy_proc_access(tracer) == 0);
}

boolean_t
lx_userns_traceme(const cred_t *parent, const cred_t *child)
{
	const lx_userns_native_t *to = (void *)crgetbrand(child);
	lx_userns_cred_t actor;

	/* A descendant credential cannot trace an initial-namespace task. */
	if (to == NULL)
		return (crgetbrand(parent) == NULL);
	if (lx_userns_tracer_capable(parent, child))
		return (B_TRUE);
	return (lx_userns_map_actor(parent, to->un_namespace, &actor) == 0 &&
	    actor.luc_ns == to->un_namespace &&
	    (to->un_caps.lc_permitted & ~actor.luc_caps.lc_permitted) == 0);
}

/* Replace an effective identity's native SID, just as native set-id does. */
static int
lx_userns_exec_sid(cred_t *candidate, const cred_t *source, boolean_t gid)
{
	uid_t id = gid ? crgetgid(candidate) : crgetuid(candidate);
	ksid_t sid, *ksp = NULL;

	if (id == (gid ? crgetgid(source) : crgetuid(source)))
		return (0);
	if (id > MAXUID) {
		if ((gid ? ksid_lookupbygid(curproc->p_zone, id, &sid) :
		    ksid_lookupbyuid(curproc->p_zone, id, &sid)) != 0)
			return (EINVAL);
		ksp = &sid;
	}
	crsetsid(candidate, ksp, gid ? KSID_GROUP : KSID_USER);
	return (0);
}

/* Ordinary exec; privileged file transitions remain explicitly unsupported. */
int
lx_userns_cred_exec(const cred_t *source, cred_t *candidate, vnode_t *vp,
    const vattr_t *attr, int level)
{
	const lx_userns_native_t *data = (void *)crgetbrand(source);
	lx_proc_data_t *pd = ptolxproc(curproc);
	lx_userns_cred_t actor;
	boolean_t nnp = ttolwp(curthread)->lwp_no_new_privs != 0;
	boolean_t limited, gain;
	cred_t *tracer;
	lx_caps_t trial;
	int error;

	if (data == NULL || data->un_cred.bc_brand != &lx_brand ||
	    pd == NULL || (pd->l_flags & LX_PROC_USERNS) == 0)
		return (EPERM);
	if (((attr->va_mode & (VSUID | VSGID)) != 0 && !nnp &&
	    (vp->v_vfsp->vfs_flag & VFS_NOSETUID) == 0) ||
	    crgetuid(candidate) != crgetuid(source) ||
	    crgetgid(candidate) != crgetgid(source))
		return (EOPNOTSUPP);
	if (level != 0)
		return (0);
	VERIFY3U(crgetref(candidate), ==, 1);
	actor.luc_ns = data->un_namespace;
	actor.luc_uid = crgetuid(candidate);
	actor.luc_gid = crgetgid(candidate);
	actor.luc_caps = data->un_caps;
	tracer = lx_ptrace_getcred();
	limited = nnp || (tracer != NULL &&
	    !lx_userns_tracer_capable(tracer, source));
	if (tracer != NULL)
		crfree(tracer);
	trial = actor.luc_caps;
	lx_caps_exec_plain(&trial,
	    lx_userns_id(actor.luc_ns, 0, crgetruid(source), 0) == 0,
	    lx_userns_id(actor.luc_ns, 0, crgetuid(source), 0) == 0, 0);
	gain = (trial.lc_permitted & ~data->un_caps.lc_permitted) != 0;
	lx_caps_exec_plain(&actor.luc_caps,
	    lx_userns_id(actor.luc_ns, 0, crgetruid(source), 0) == 0,
	    lx_userns_id(actor.luc_ns, 0, crgetuid(source), 0) == 0, limited);
	if (gain && limited && (nnp ||
	    (data->un_caps.lc_effective & LX_CAP_BIT(7)) == 0)) {
		VERIFY0(crsetresuid(candidate, crgetruid(source),
		    crgetruid(source), crgetsuid(source)));
		VERIFY0(crsetresgid(candidate, crgetrgid(source),
		    crgetrgid(source), crgetsgid(source)));
		error = lx_userns_exec_sid(candidate, source, B_FALSE);
		if (error == 0)
			error = lx_userns_exec_sid(candidate, source, B_TRUE);
		if (error != 0)
			return (error);
		actor.luc_uid = crgetuid(candidate);
		actor.luc_gid = crgetgid(candidate);
	}
	lx_userns_native_limit(candidate);
	lx_userns_native_attach(candidate, &actor);
	((lx_userns_native_t *)crgetbrand(candidate))->un_exec_nodump =
	    (actor.luc_caps.lc_permitted & ~data->un_caps.lc_permitted) != 0 ||
	    crgetuid(candidate) != crgetuid(source) ||
	    crgetgid(candidate) != crgetgid(source);
	return (0);
}

/* Visible IDs are translated before native backing-ID validation. */
uint32_t
lx_userns_id_visible(const cred_t *cred, int gid, uint32_t backing)
{
	const lx_userns_native_t *data = (void *)crgetbrand(cred);
	uint32_t id;

	if (data == NULL)
		return (backing);
	VERIFY(data->un_cred.bc_brand == &lx_brand);
	id = lx_userns_id(data->un_namespace, gid, backing, 0);
	return (id == UINT32_MAX ? 65534 : id);
}

static uint_t
lx_userns_root_ids(lx_userns_t *ns, const uint32_t ids[3])
{
	uint_t mask = 0, i;

	for (i = 0; i < 3; i++) {
		if (lx_userns_id(ns, 0, ids[i], 0) == 0)
			mask |= 1U << i;
	}
	return (mask);
}

static int
lx_userns_setids(int nr, uintptr_t *args)
{
	const lx_userns_native_t *data = (void *)crgetbrand(CRED());
	uint32_t old[3], next[3], input[3] = { UINT32_MAX,
	    UINT32_MAX, UINT32_MAX }, i;
	boolean_t gid = nr == 106 || nr == 114 || nr == 119;
	boolean_t one = nr == 105 || nr == 106;
	boolean_t two = nr == 113 || nr == 114;
	boolean_t capable;
	cred_t *candidate;
	lx_caps_t caps = data->un_caps;
	ksid_t sid, *ksp = NULL;

	old[0] = gid ? crgetrgid(CRED()) : crgetruid(CRED());
	old[1] = gid ? crgetgid(CRED()) : crgetuid(CRED());
	old[2] = gid ? crgetsgid(CRED()) : crgetsuid(CRED());
	bcopy(old, next, sizeof (old));
	for (i = 0; i < (one ? 1 : two ? 2 : 3); i++) {
		uint32_t visible = (uint32_t)args[i];

		if (!one && visible == UINT32_MAX)
			continue;
		input[i] = lx_userns_id(data->un_namespace, gid, visible, 1);
		if (input[i] == UINT32_MAX)
			return (EINVAL);
	}
	capable = (caps.lc_effective & LX_CAP_BIT(gid ? 6 : 7)) != 0;
	if (one) {
		if (capable) {
			next[0] = next[1] = next[2] = input[0];
		} else {
			if (input[0] != old[0] && input[0] != old[2])
				return (EPERM);
			next[1] = input[0];
		}
	} else {
		for (i = 0; i < (two ? 2 : 3); i++) {
			if (input[i] == UINT32_MAX)
				continue;
			if (!capable && input[i] != old[0] &&
			    input[i] != old[1] &&
			    ((!two || i != 0) ? input[i] != old[2] : B_TRUE))
				return (EPERM);
			next[i] = input[i];
		}
		if (two && (input[0] != UINT32_MAX ||
		    (input[1] != UINT32_MAX && input[1] != old[0])))
			next[2] = next[1];
	}
	if (bcmp(next, old, sizeof (next)) == 0)
		return (0);
	if (!gid)
		lx_caps_uid_change(&caps, lx_userns_root_ids(data->un_namespace,
		    old), lx_userns_root_ids(data->un_namespace, next));
	candidate = lx_userns_with_caps(CRED(), &caps);
	if ((gid ? crsetresgid(candidate, next[0], next[1], next[2]) :
	    crsetresuid(candidate, next[0], next[1], next[2])) != 0) {
		crfree(candidate);
		return (EINVAL);
	}
	if (next[1] != old[1]) {
		if (next[1] > MAXUID) {
			if ((gid ? ksid_lookupbygid(curproc->p_zone, next[1],
			    &sid) : ksid_lookupbyuid(curproc->p_zone, next[1],
			    &sid)) != 0) {
				crfree(candidate);
				return (EINVAL);
			}
			ksp = &sid;
		}
		/* crsetsid consumes the lookup's domain reference. */
		crsetsid(candidate, ksp, gid ? KSID_GROUP : KSID_USER);
	}
	lx_userns_publish(curthread, candidate);
	crfree(candidate);
	return (0);
}

static long
lx_userns_groups(int nr, int count, uintptr_t address)
{
	const lx_userns_native_t *data = (void *)crgetbrand(CRED());
	gid_t *groups;
	cred_t *candidate;
	int n = crgetngroups(CRED()), error = 0, i;
	boolean_t ephemeral = B_FALSE;
	ksidlist_t *sidlist = NULL;

	if (nr == 115) {
		if (count < 0 || (count != 0 && count < n))
			return (set_errno(EINVAL));
		if (count == 0 || n == 0)
			return (n);
		groups = kmem_alloc(n * sizeof (*groups), KM_SLEEP);
		for (i = 0; i < n; i++)
			groups[i] = lx_userns_id_visible(CRED(), 1,
			    crgetgroups(CRED())[i]);
		if (copyout(groups, (void *)address, n * sizeof (*groups)) != 0)
			error = EFAULT;
		kmem_free(groups, n * sizeof (*groups));
		return (error == 0 ? n : set_errno(error));
	}
	if ((data->un_caps.lc_effective & LX_CAP_BIT(6)) == 0 ||
	    !lx_userns_may_setgroups(data->un_namespace))
		return (set_errno(EPERM));
	/* Use native group storage; do not silently truncate larger sets. */
	if ((uint_t)count > ngroups_max)
		return (set_errno(EINVAL));
	groups = count == 0 ? NULL :
	    kmem_alloc(count * sizeof (*groups), KM_SLEEP);
	if (count != 0 && copyin((void *)address, groups,
	    count * sizeof (*groups)) != 0) {
		error = EFAULT;
		goto out;
	}
	for (i = 0; i < count; i++) {
		groups[i] = lx_userns_id(data->un_namespace, 1, groups[i], 1);
		if (groups[i] == UINT32_MAX ||
		    !VALID_GID(groups[i], curproc->p_zone)) {
			error = EINVAL;
			goto out;
		}
		if (groups[i] > MAXUID)
			ephemeral = B_TRUE;
	}
	if (ephemeral && (sidlist = kcrsid_gidstosids(curproc->p_zone,
	    count, groups)) == NULL) {
		error = EINVAL;
		goto out;
	}
	candidate = lx_userns_with_caps(CRED(), &data->un_caps);
	if (crsetgroups(candidate, count, groups) != 0)
		error = EINVAL;
	else {
		crsetsidlist(candidate, sidlist);
		sidlist = NULL;
		lx_userns_publish(curthread, candidate);
	}
	crfree(candidate);
out:
	if (sidlist != NULL)
		ksidlist_rele(sidlist);
	if (groups != NULL)
		kmem_free(groups, count * sizeof (*groups));
	return (error == 0 ? 0 : set_errno(error));
}

boolean_t
lx_userns_id_syscall(int nr, uintptr_t *args, long *result)
{
	uint32_t ids[3], i;
	int error;

	if (nr == 102 || nr == 107 || nr == 104 || nr == 108) {
		uint32_t id = nr == 102 ? crgetruid(CRED()) :
		    nr == 107 ? crgetuid(CRED()) :
		    nr == 104 ? crgetrgid(CRED()) : crgetgid(CRED());

		*result = lx_userns_id_visible(CRED(), nr == 104 || nr == 108,
		    id);
	} else if (nr == 118 || nr == 120) {
		boolean_t gid = nr == 120;

		ids[0] = gid ? crgetrgid(CRED()) : crgetruid(CRED());
		ids[1] = gid ? crgetgid(CRED()) : crgetuid(CRED());
		ids[2] = gid ? crgetsgid(CRED()) : crgetsuid(CRED());
		for (i = 0; i < 3; i++) {
			ids[i] = lx_userns_id_visible(CRED(), gid, ids[i]);
			if (copyout(&ids[i], (void *)args[i],
			    sizeof (ids[i])) != 0) {
				*result = set_errno(EFAULT);
				return (B_TRUE);
			}
		}
		*result = 0;
	} else if (nr == 115 || nr == 116) {
		*result = lx_userns_groups(nr, (int)args[0], args[1]);
	} else if (nr == 105 || nr == 106 || nr == 113 || nr == 114 ||
	    nr == 117 || nr == 119) {
		error = lx_userns_setids(nr, args);
		*result = error == 0 ? 0 : set_errno(error);
	} else {
		return (B_FALSE);
	}
	return (B_TRUE);
}
