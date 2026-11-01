/*
 * CDDL version 1.0, http://www.illumos.org/license/CDDL.
 */
#include <sys/types.h>
#include <sys/atomic.h>
#include <sys/avl.h>
#include <sys/brand.h>
#include <sys/debug.h>
#include <sys/errno.h>
#include <sys/id_space.h>
#include <sys/kmem.h>
#include <sys/proc.h>
#include <sys/session.h>
#include <sys/signal.h>
#include <sys/sysmacros.h>
#include <sys/zone.h>
#include <sys/lx_brand.h>
#include <sys/lx_cap.h>
#include <sys/lx_pidns.h>
#include <sys/lx_userns.h>

/* Maps and PID1 state use native pidlock; parent links are immutable. */
typedef enum lx_pidns_state {
	LX_PIDNS_NEW, LX_PIDNS_RESERVED, LX_PIDNS_LIVE, LX_PIDNS_DEAD
} lx_pidns_state_t;

typedef struct lx_pidns_slot {
	avl_node_t ps_number_node;
	avl_node_t ps_backing_node;
	lx_pidns_t *ps_namespace;
	lx_pidns_map_t *ps_map;
	pid_t ps_number;
	pid_t ps_backing;
} lx_pidns_slot_t;

struct lx_pidns {
	uint_t pn_refs;
	uint_t pn_depth;
	cred_t *pn_owner;
	lx_userns_t *pn_userns;
	lx_pidns_t *pn_parent;
	id_space_t *pn_ids;
	avl_tree_t pn_numbers;
	avl_tree_t pn_backing;
	lx_pidns_state_t pn_state;
	boolean_t pn_draining;	/* PID1 is synchronously reaping children */
};

struct lx_pidns_map {
	uint_t pm_refs;
	uint_t pm_count;
	lx_pidns_t *pm_namespace;
	struct pid *pm_identity;
	boolean_t pm_init;
	boolean_t pm_committed;
	lx_pidns_slot_t pm_slots[];
};

static uint_t lx_pidns_nspaces;

static int
lx_pidns_number_cmp(const void *a, const void *b)
{
	const lx_pidns_slot_t *x = a, *y = b;

	return ((x->ps_number > y->ps_number) -
	    (x->ps_number < y->ps_number));
}

static int
lx_pidns_backing_cmp(const void *a, const void *b)
{
	const lx_pidns_slot_t *x = a, *y = b;

	return ((x->ps_backing > y->ps_backing) -
	    (x->ps_backing < y->ps_backing));
}

boolean_t
lx_pidns_busy(void)
{
	return (lx_pidns_nspaces != 0);
}

void
lx_pidns_hold(lx_pidns_t *ns)
{
	VERIFY3U(ns->pn_refs, !=, 0);
	atomic_inc_uint(&ns->pn_refs);
}

void
lx_pidns_rele(lx_pidns_t *ns)
{
	boolean_t held = MUTEX_HELD(&pidlock);

	while (ns != NULL && atomic_dec_uint_nv(&ns->pn_refs) == 0) {
		lx_pidns_t *parent = ns->pn_parent;

		if (!held)
			mutex_enter(&pidlock);
		VERIFY(avl_is_empty(&ns->pn_numbers));
		VERIFY(avl_is_empty(&ns->pn_backing));
		avl_destroy(&ns->pn_numbers);
		avl_destroy(&ns->pn_backing);
		if (!held)
			mutex_exit(&pidlock);
		/* Native ID spaces and credentials permit release here. */
		id_space_destroy(ns->pn_ids);
		lx_userns_pidns_uncharge(ns->pn_userns);
		lx_userns_rele(ns->pn_userns);
		crfree(ns->pn_owner);
		kmem_free(ns, sizeof (*ns));
		atomic_dec_uint(&lx_pidns_nspaces);
		ns = parent;
	}
}

int
lx_pidns_create(cred_t *owner, lx_pidns_t *parent, uint_t limit,
    lx_pidns_t **out)
{
	lx_userns_cred_t actor;
	zone_t *zone = crgetzone(owner);
	lx_pidns_t *ns;
	int error;

	*out = NULL;
	ASSERT(MUTEX_NOT_HELD(&pidlock));
	if (zone->zone_brand != &lx_brand || limit < 2 ||
	    limit > LX_PIDNS_PID_LIMIT ||
	    (parent != NULL && crgetzone(parent->pn_owner) != zone))
		return (EINVAL);
	if (parent != NULL && parent->pn_depth == LX_PIDNS_MAX_DEPTH)
		return (ENOSPC);
	if ((error = lx_userns_map_actor(owner,
	    ztolxzd(zone)->lxzd_userns, &actor)) != 0)
		return (error);
	if (!lx_userns_has_cap(&actor, actor.luc_ns, LX_CAP_SYS_ADMIN))
		return (EPERM);
	if ((error = lx_userns_pidns_charge(actor.luc_ns)) != 0)
		return (error);
	ns = kmem_zalloc(sizeof (*ns), KM_SLEEP);
	ns->pn_ids = id_space_create("lx_pidns", 1, limit);
	ns->pn_refs = 1;
	ns->pn_depth = parent == NULL ? 1 : parent->pn_depth + 1;
	crhold(ns->pn_owner = owner);
	ns->pn_userns = actor.luc_ns;
	lx_userns_hold(ns->pn_userns);
	ns->pn_parent = parent;
	if (parent != NULL)
		lx_pidns_hold(parent);
	avl_create(&ns->pn_numbers, lx_pidns_number_cmp,
	    sizeof (lx_pidns_slot_t),
	    offsetof(lx_pidns_slot_t, ps_number_node));
	avl_create(&ns->pn_backing, lx_pidns_backing_cmp,
	    sizeof (lx_pidns_slot_t),
	    offsetof(lx_pidns_slot_t, ps_backing_node));
	atomic_inc_uint(&lx_pidns_nspaces);
	*out = ns;
	return (0);
}

void
lx_pidns_shutdown(lx_pidns_t *ns)
{
	boolean_t held = MUTEX_HELD(&pidlock);

	if (!held)
		mutex_enter(&pidlock);
	/* Keep maps for native wait, proc descriptors and zombie reporting. */
	ns->pn_state = LX_PIDNS_DEAD;
	if (!held)
		mutex_exit(&pidlock);
}

/*
 * Gate pending construction and signal every committed process represented
 * in this namespace, including descendant namespaces. Native pidlock keeps
 * both the map indices and process identities stable throughout the walk.
 * Uncommitted children observe DEAD at commit and die before user execution.
 */
void
lx_pidns_init_exit(proc_t *init)
{
	lx_pidns_t *ns = lx_pidns_active(init);
	lx_pidns_slot_t *slot;
	zoneid_t zoneid = init->p_zone->zone_id;

	ASSERT(init == curproc);
	VERIFY(ns != NULL &&
	    lx_pidns_map_id(ptolxproc(init)->l_pidns_map, ns) == 1);
	mutex_enter(&pidlock);
	ns->pn_state = LX_PIDNS_DEAD;
	ns->pn_draining = B_TRUE;
	for (slot = avl_first(&ns->pn_numbers); slot != NULL;
	    slot = AVL_NEXT(&ns->pn_numbers, slot)) {
		lx_pidns_map_t *map = slot->ps_map;
		proc_t *p;

		if (!map->pm_committed ||
		    (p = prfind_zone(slot->ps_backing, zoneid)) == NULL ||
		    p == init)
			continue;
		mutex_enter(&p->p_lock);
		/* Retained task identities confer no process authority. */
		if (p->p_brand == &lx_brand && ptolxproc(p) != NULL &&
		    ptolxproc(p)->l_pidns_map == map)
			sigtoproc(p, NULL, SIGKILL);
		mutex_exit(&p->p_lock);
	}
	mutex_exit(&pidlock);

	/* Native child links define completion, not retained identities. */
	proc_exit_wait();
	mutex_enter(&pidlock);
	VERIFY(init->p_child == NULL);
	ns->pn_draining = B_FALSE;
	mutex_exit(&pidlock);
}

/* Native reparenting retains ownership of the child lists and notifications. */
proc_t *
lx_pidns_child_reaper(proc_t *parent)
{
	lx_pidns_t *ns;
	zoneid_t zoneid = parent->p_zone->zone_id;

	ASSERT(MUTEX_HELD(&pidlock));
	for (ns = lx_pidns_active(parent); ns != NULL; ns = ns->pn_parent) {
		lx_pidns_slot_t key = { .ps_number = 1 }, *slot;
		proc_t *init;
		boolean_t alive;

		if ((ns->pn_state != LX_PIDNS_LIVE && !ns->pn_draining) ||
		    (slot = avl_find(&ns->pn_numbers, &key, NULL)) == NULL ||
		    !slot->ps_map->pm_committed ||
		    (init = prfind_zone(slot->ps_backing, zoneid)) == NULL ||
		    init == parent)
			continue;
		mutex_enter(&init->p_lock);
		/*
		 * A LIVE init may already be in exitlwps(), before it can mark
		 * DEAD and drain. Keep its children rooted there across that
		 * interval too. It cannot reach native reparenting until its
		 * synchronous drain has completed.
		 */
		alive = init->p_stat == SRUN &&
		    init->p_brand == &lx_brand && ptolxproc(init) != NULL &&
		    ptolxproc(init)->l_pidns_map == slot->ps_map;
		mutex_exit(&init->p_lock);
		if (alive)
			return (init);
	}
	return (NULL);
}

int
lx_pidns_reserve(lx_pidns_t *ns, lx_pidns_map_t **out)
{
	lx_pidns_map_t *map;
	lx_pidns_t *at;
	uint_t i = 0;
	int error = 0;

	*out = NULL;
	ASSERT(MUTEX_NOT_HELD(&pidlock));
	map = kmem_zalloc(sizeof (*map) +
	    ns->pn_depth * sizeof (map->pm_slots[0]), KM_SLEEP);
	map->pm_refs = 1;
	map->pm_namespace = ns;
	lx_pidns_hold(ns);
	mutex_enter(&pidlock);
	for (at = ns; at != NULL; at = at->pn_parent) {
		lx_pidns_slot_t *slot = &map->pm_slots[i];

		if (at->pn_state == LX_PIDNS_DEAD) {
			error = ENOMEM;
			break;
		}
		if (at->pn_state != LX_PIDNS_LIVE &&
		    (at != ns || at->pn_state != LX_PIDNS_NEW)) {
			error = EAGAIN;
			break;
		}
		slot->ps_number = at->pn_state == LX_PIDNS_NEW ?
		    id_alloc_specific_nosleep(at->pn_ids, 1) :
		    id_alloc_nosleep(at->pn_ids);
		if (slot->ps_number == -1) {
			error = EAGAIN;
			break;
		}
		if (at->pn_state == LX_PIDNS_NEW) {
			at->pn_state = LX_PIDNS_RESERVED;
			map->pm_init = B_TRUE;
		}
		slot->ps_namespace = at;
		slot->ps_map = map;
		i++;
	}
	map->pm_count = i;
	mutex_exit(&pidlock);
	if (error != 0) {
		lx_pidns_map_rele(map);
		return (error);
	}
	*out = map;
	return (0);
}

/* Consume a held native identity, including on failure. */
static int
lx_pidns_bind_identity(lx_pidns_map_t *map, struct pid *identity,
    boolean_t commit)
{
	lx_pidns_t *ns = map->pm_namespace;
	pid_t backing = identity->pid_id;
	uint_t i;
	int error = 0;

	ASSERT(MUTEX_NOT_HELD(&pidlock));
	mutex_enter(&pidlock);
	if (map->pm_identity != NULL) {
		error = EALREADY;
		goto out;
	}
	for (i = 0; i < map->pm_count; i++) {
		lx_pidns_slot_t *slot = &map->pm_slots[i];
		lx_pidns_slot_t key = { .ps_backing = backing };
		lx_pidns_t *at = slot->ps_namespace;

		if (at->pn_state == LX_PIDNS_DEAD) {
			error = ENOMEM;
			goto out;
		}
		if (at->pn_state != LX_PIDNS_LIVE &&
		    !(i == 0 && map->pm_init &&
		    at->pn_state == LX_PIDNS_RESERVED)) {
			error = EAGAIN;
			goto out;
		}
		if (avl_find(&at->pn_backing, &key, NULL) != NULL) {
			error = EEXIST;
			goto out;
		}
	}
	for (i = 0; i < map->pm_count; i++) {
		lx_pidns_slot_t *slot = &map->pm_slots[i];

		slot->ps_backing = backing;
		avl_add(&slot->ps_namespace->pn_numbers, slot);
		avl_add(&slot->ps_namespace->pn_backing, slot);
	}
	map->pm_identity = identity;
	identity = NULL;
	if (commit) {
		map->pm_committed = B_TRUE;
		if (map->pm_init)
			ns->pn_state = LX_PIDNS_LIVE;
	}
out:
	mutex_exit(&pidlock);
	if (identity != NULL)
		lx_pid_identity_rele(identity);
	return (error);
}

/* Bind before lx_pid_assign, while the caller owns the native reservation. */
int
lx_pidns_bind_native(lx_pidns_map_t *map, zone_t *zone, struct pid *identity)
{
	int error = 0;

	ASSERT(MUTEX_NOT_HELD(&pidlock));
	if (zone != crgetzone(map->pm_namespace->pn_owner))
		return (EXDEV);
	mutex_enter(&pidlock);
	if (pid_find(identity->pid_id) != identity || identity->pid_id <= 1) {
		error = ESRCH;
	} else if (identity->pid_prinactive) {
		/* Unpublished LWP reservations have no native proc slot. */
		if (identity->pid_prslot != 0)
			error = ESRCH;
	} else if (prfind_zone(identity->pid_id, zone->zone_id) == NULL) {
		error = EXDEV;
	}
	if (error == 0) {
		PID_HOLD(identity);
	}
	mutex_exit(&pidlock);
	if (error != 0)
		return (error);
	return (lx_pidns_bind_identity(map, identity, B_FALSE));
}

/* Called at native fork's successful b_lwp_setrval, before the child runs. */
boolean_t
lx_pidns_commit(lx_pidns_map_t *map)
{
	lx_pidns_t *ns = map->pm_namespace;
	boolean_t held = MUTEX_HELD(&pidlock), alive = B_TRUE;
	uint_t i;

	if (!held)
		mutex_enter(&pidlock);
	VERIFY(map->pm_identity != NULL);
	for (i = 0; i < map->pm_count; i++) {
		if (map->pm_slots[i].ps_namespace->pn_state == LX_PIDNS_DEAD)
			alive = B_FALSE;
	}
	map->pm_committed = B_TRUE;
	if (map->pm_init && ns->pn_state != LX_PIDNS_DEAD)
		ns->pn_state = alive ? LX_PIDNS_LIVE : LX_PIDNS_DEAD;
	if (!held)
		mutex_exit(&pidlock);
	return (alive);
}

/* Atomic registered-task adapter, used by the private map fixtures. */
int
lx_pidns_publish(lx_pidns_map_t *map, pid_t backing)
{
	struct pid *identity;

	ASSERT(MUTEX_NOT_HELD(&pidlock));
	identity = lx_pid_identity_hold(backing,
	    crgetzone(map->pm_namespace->pn_owner));
	if (identity == NULL)
		return (ESRCH);
	return (lx_pidns_bind_identity(map, identity, B_TRUE));
}

void
lx_pidns_map_hold(lx_pidns_map_t *map)
{
	VERIFY3U(map->pm_refs, !=, 0);
	atomic_inc_uint(&map->pm_refs);
}

void
lx_pidns_map_rele(lx_pidns_map_t *map)
{
	lx_pidns_t *ns = map->pm_namespace;
	boolean_t held = MUTEX_HELD(&pidlock);
	uint_t refs;
	uint_t i;
	size_t size;

	/*
	 * Nonfinal releases need no lock. The final transition is serialized
	 * with pn_backing readers so an indexed map can never be revived from
	 * zero while its releaser waits for pidlock.
	 */
	for (;;) {
		for (;;) {
			VERIFY3U((refs = map->pm_refs), !=, 0);
			if (refs == 1)
				break;
			if (atomic_cas_uint(&map->pm_refs, refs,
			    refs - 1) == refs)
				return;
		}
		if (!held)
			mutex_enter(&pidlock);
		if (atomic_cas_uint(&map->pm_refs, 1, 0) == 1)
			break;
		if (!held)
			mutex_exit(&pidlock);
	}
	for (i = 0; i < map->pm_count; i++) {
		lx_pidns_slot_t *slot = &map->pm_slots[i];
		lx_pidns_t *at = slot->ps_namespace;

		if (map->pm_identity != NULL) {
			avl_remove(&at->pn_numbers, slot);
			avl_remove(&at->pn_backing, slot);
		}
		id_free(at->pn_ids, slot->ps_number);
	}
	if (map->pm_init && ns->pn_state != LX_PIDNS_DEAD)
		ns->pn_state = map->pm_committed ?
		    LX_PIDNS_DEAD : LX_PIDNS_NEW;
	if (map->pm_identity != NULL) {
		PID_RELE(map->pm_identity);
	}
	if (!held)
		mutex_exit(&pidlock);
	size = sizeof (*map) + ns->pn_depth * sizeof (map->pm_slots[0]);
	kmem_free(map, size);
	lx_pidns_rele(ns);
}

/* Return a held committed map already indexed in active ancestry. */
lx_pidns_map_t *
lx_pidns_role_hold(lx_pidns_t *ns, struct pid *identity)
{
	lx_pidns_t *at;
	lx_pidns_slot_t key = { .ps_backing = identity->pid_id };
	lx_pidns_slot_t *slot;

	ASSERT(MUTEX_HELD(&pidlock));
	for (at = ns; at != NULL; at = at->pn_parent) {
		if ((slot = avl_find(&at->pn_backing, &key, NULL)) == NULL ||
		    !slot->ps_map->pm_committed)
			continue;
		/* Final 1 -> 0 transitions also require pidlock. */
		VERIFY3U(slot->ps_map->pm_refs, !=, 0);
		atomic_inc_uint(&slot->ps_map->pm_refs);
		return (slot->ps_map);
	}
	return (NULL);
}

void
lx_pidns_roles_update(proc_t *p)
{
	lx_proc_data_t *pd = ptolxproc(p);
	lx_pidns_t *ns;
	lx_pidns_map_t *pgid, *sid, *old_pgid, *old_sid;

	ASSERT(MUTEX_HELD(&pidlock));
	if (pd == NULL || (ns = lx_pidns_active(p)) == NULL)
		return;

	pgid = lx_pidns_role_hold(ns, p->p_pgidp);
	sid = lx_pidns_role_hold(ns, p->p_sessp->s_sidp);
	old_pgid = pd->l_pidns_pgid;
	old_sid = pd->l_pidns_sid;
	pd->l_pidns_pgid = pgid;
	pd->l_pidns_sid = sid;
	if (old_pgid != NULL)
		lx_pidns_map_rele(old_pgid);
	if (old_sid != NULL)
		lx_pidns_map_rele(old_sid);
}

pid_t
lx_pidns_map_id(lx_pidns_map_t *map, lx_pidns_t *viewer)
{
	uint_t i;

	for (i = 0; i < map->pm_count; i++) {
		if (map->pm_slots[i].ps_namespace == viewer)
			return (map->pm_slots[i].ps_number);
	}
	return (0);
}

pid_t
lx_pidns_visible(lx_pidns_t *ns, pid_t backing)
{
	lx_pidns_slot_t key = { .ps_backing = backing }, *slot;
	boolean_t held = MUTEX_HELD(&pidlock);
	pid_t number;

	if (!held)
		mutex_enter(&pidlock);
	slot = avl_find(&ns->pn_backing, &key, NULL);
	number = slot == NULL || !slot->ps_map->pm_committed ?
	    0 : slot->ps_number;
	if (!held)
		mutex_exit(&pidlock);
	return (number);
}

struct pid *
lx_pidns_lookup(lx_pidns_t *ns, pid_t number)
{
	lx_pidns_slot_t key = { .ps_number = number }, *slot;
	boolean_t held = MUTEX_HELD(&pidlock);
	struct pid *identity = NULL;

	if (!held)
		mutex_enter(&pidlock);
	slot = avl_find(&ns->pn_numbers, &key, NULL);
	if (slot != NULL && slot->ps_map->pm_committed) {
		identity = slot->ps_map->pm_identity;
		PID_HOLD(identity);
	}
	if (!held)
		mutex_exit(&pidlock);
	return (identity);
}

/* Process membership is held by its PID map through native zombie/reap. */
lx_pidns_t *
lx_pidns_map_namespace(lx_pidns_map_t *map)
{
	return (map == NULL ? NULL : map->pm_namespace);
}

lx_pidns_t *
lx_pidns_active(proc_t *p)
{
	lx_proc_data_t *pd = ptolxproc(p);

	return (pd == NULL ? NULL : lx_pidns_map_namespace(pd->l_pidns_map));
}

lx_pidns_t *
lx_pidns_children(proc_t *p)
{
	lx_proc_data_t *pd = ptolxproc(p);

	if (pd == NULL)
		return (NULL);
	return (pd->l_pidns_children != NULL ? pd->l_pidns_children :
	    lx_pidns_map_namespace(pd->l_pidns_map));
}
