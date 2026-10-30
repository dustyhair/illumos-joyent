/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * A copy is available at http://www.illumos.org/license/CDDL.
 */
#include <sys/types.h>
#include <sys/atomic.h>
#include <sys/brand.h>
#include <sys/disp.h>
#include <sys/dtrace.h>
#include <sys/errno.h>
#include <sys/debug.h>
#include <sys/kmem.h>
#include <sys/fs/lofs_info.h>
#include <sys/refstr.h>
#include <sys/mount.h>
#include <sys/pathname.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/taskq_impl.h>
#include <sys/vfs.h>
#include <sys/vnode.h>
#include <sys/user.h>
#include <sys/zone.h>
#include <sys/lx_brand.h>
#include <sys/lx_misc.h>
#include <sys/lx_cap.h>
#include <sys/lx_userns.h>
#include <sys/lx_mntns.h>
#include <lx_syscall.h>

extern brand_t lx_brand;

typedef struct lx_mntns_mount {
	vfs_t *lm_vfs;
	vnode_t *lm_root;
	refstr_t *lm_path;
	struct lx_mntns_mount *lm_next;
} lx_mntns_mount_t;

/* The zone and each reserved view own a reference to this account. */
struct lx_mntns_budget {
	kmutex_t lb_lock;
	volatile uint_t lb_refs;
	uint32_t lb_max;
	uint32_t lb_used;
};

struct lx_mntns_view {
	kmutex_t lv_lock;
	volatile uint_t lv_refs;
	cred_t *lv_cred;
	cred_t *lv_owner;
	lx_mntns_budget_t *lv_budget;
	taskq_ent_t lv_reap_ent;
	timeout_id_t lv_timeout;
	vnode_t *lv_root;
	vfs_t *lv_vfs;
	lx_mntns_mount_t *lv_mounts;
};

static kmutex_t lx_mntns_lock;
static taskq_t *lx_mntns_taskq;
static volatile uint_t lx_mntns_nviews;

static int lx_mntns_destroy(lx_mntns_view_t *, cred_t *);
static void lx_mntns_reap(void *);

lx_mntns_budget_t *
lx_mntns_budget_create(uint32_t maximum)
{
	lx_mntns_budget_t *budget = kmem_zalloc(sizeof (*budget), KM_SLEEP);

	mutex_init(&budget->lb_lock, NULL, MUTEX_DEFAULT, NULL);
	budget->lb_refs = 1;
	budget->lb_max = maximum;
	return (budget);
}

void
lx_mntns_budget_rele(lx_mntns_budget_t *budget)
{
	if (atomic_dec_uint_nv(&budget->lb_refs) == 0) {
		VERIFY0(budget->lb_used);
		mutex_destroy(&budget->lb_lock);
		kmem_free(budget, sizeof (*budget));
	}
}

void
lx_mntns_budget_set(lx_mntns_budget_t *budget, uint32_t maximum)
{
	mutex_enter(&budget->lb_lock);
	budget->lb_max = maximum;
	mutex_exit(&budget->lb_lock);
}

void
lx_mntns_budget_get(lx_mntns_budget_t *budget, uint32_t *maximum,
    uint32_t *used)
{
	mutex_enter(&budget->lb_lock);
	*maximum = budget->lb_max;
	*used = budget->lb_used;
	mutex_exit(&budget->lb_lock);
}

/* A reservation holds its account through failed or deferred cleanup. */
static int
lx_mntns_charge(lx_mntns_budget_t *budget)
{
	if (budget == NULL)
		return (curzone == global_zone ? 0 : EPERM);
	mutex_enter(&budget->lb_lock);
	if (budget->lb_used >= budget->lb_max) {
		mutex_exit(&budget->lb_lock);
		return (ENOSPC);
	}
	budget->lb_used++;
	atomic_inc_uint(&budget->lb_refs);
	mutex_exit(&budget->lb_lock);
	return (0);
}

static void
lx_mntns_uncharge(lx_mntns_budget_t *budget)
{
	if (budget == NULL)
		return;
	mutex_enter(&budget->lb_lock);
	VERIFY3U(budget->lb_used, !=, 0);
	budget->lb_used--;
	mutex_exit(&budget->lb_lock);
	lx_mntns_budget_rele(budget);
}

void
lx_mntns_init(void)
{
	mutex_init(&lx_mntns_lock, NULL, MUTEX_DEFAULT, NULL);
}

boolean_t
lx_mntns_busy(void)
{
	return (lx_mntns_nviews != 0);
}

void
lx_mntns_fini(void)
{
	VERIFY(!lx_mntns_busy());
	/* Wait for the last reaper to return before unloading module text. */
	if (lx_mntns_taskq != NULL) {
		taskq_destroy(lx_mntns_taskq);
		lx_mntns_taskq = NULL;
	}
	mutex_destroy(&lx_mntns_lock);
}

static void
lx_mntns_dispatch(void *arg)
{
	lx_mntns_view_t *view = arg;

	/* A preallocated entry can be queued from a timeout without failing. */
	taskq_dispatch_ent(lx_mntns_taskq, lx_mntns_reap, view, TQ_NOSLEEP,
	    &view->lv_reap_ent);
}

static void
lx_mntns_reap(void *arg)
{
	lx_mntns_view_t *view = arg;
	timeout_id_t id;
	int error;

	VERIFY3U(view->lv_refs, ==, 0);
	mutex_enter(&view->lv_lock);
	id = view->lv_timeout;
	view->lv_timeout = 0;
	mutex_exit(&view->lv_lock);
	/* Fence the timeout callback before freeing its argument or module. */
	if (id != 0)
		(void) untimeout(id);
	if ((error = lx_mntns_destroy(view, view->lv_cred)) == 0)
		return;
	/* Keep busy roots intact and let the taskq service other views. */
	DTRACE_PROBE1(lx__mntns__reap__retry, int, error);
	mutex_enter(&view->lv_lock);
	view->lv_timeout = timeout(lx_mntns_dispatch, view, hz);
	mutex_exit(&view->lv_lock);
}

/* The caller owns the returned reference, independent of process lifetime. */
lx_mntns_view_t *
lx_mntns_current(void)
{
	lx_proc_data_t *pd;
	lx_mntns_view_t *view = NULL;

	mutex_enter(&curproc->p_lock);
	if ((pd = ptolxproc(curproc)) != NULL && pd->l_mntns != NULL) {
		view = pd->l_mntns;
		lx_mntns_view_hold(view);
	}
	mutex_exit(&curproc->p_lock);
	return (view);
}

/* Ownership is immutable; holding the view also holds its owner credential. */
boolean_t
lx_mntns_view_capable(lx_mntns_view_t *view, const cred_t *actor)
{
	return (lx_userns_cred_capable(actor, view->lv_owner,
	    LX_CAP_SYS_ADMIN));
}

void
lx_mntns_view_hold(lx_mntns_view_t *view)
{
	VERIFY3U(atomic_inc_uint_nv(&view->lv_refs), >, 1);
}

void
lx_mntns_view_rele(lx_mntns_view_t *view)
{
	if (atomic_dec_uint_nv(&view->lv_refs) == 0)
		lx_mntns_dispatch(view);
}

static int
lx_mntns_mount(const char *source, const char *path, const char *type,
    const char *options, int flags, vnode_t *covered, vnode_t *sourcevp,
    cred_t *cr, vfs_t **out)
{
	struct mounta args;
	lofs_mount_args_t loargs;
	char *opts = kmem_zalloc(MAX_MNTOPT_STR, KM_SLEEP);
	int error;

	(void) strlcpy(opts, options, MAX_MNTOPT_STR);
	bzero(&args, sizeof (args));
	args.spec = (char *)source;
	args.dir = (char *)path;
	args.flags = MS_SYSSPACE | MS_OPTIONSTR | MS_OVERLAY |
	    MS_NOMNTTAB | flags;
	args.fstype = (char *)type;
	args.optptr = opts;
	args.optlen = MAX_MNTOPT_STR;
	if (sourcevp != NULL) {
		loargs.lma_version = LOFS_MOUNT_ARGS_VERSION;
		loargs.lma_source = sourcevp;
		args.flags |= MS_DATA;
		args.dataptr = (char *)&loargs;
		args.datalen = sizeof (loargs);
	}
	*out = NULL;
	error = domount((char *)type, &args, covered, cr, out);
	kmem_free(opts, MAX_MNTOPT_STR);
	return (error);
}

static int
lx_mntns_lookup_path(vnode_t *root, const char *path, cred_t *cr,
    vnode_t **out, char *resolved)
{
	struct pathname pn, rpn;
	int error;

	*out = NULL;
	if (path == NULL || path[0] != '/')
		return (EINVAL);
	if ((error = pn_get(path, UIO_SYSSPACE, &pn)) != 0)
		return (error);
	if (resolved != NULL)
		pn_alloc(&rpn);
	pn_skipslash(&pn);
	/* lookuppnvp consumes the start and non-global root references. */
	VN_HOLD(root);
	if (root != rootdir)
		VN_HOLD(root);
	error = lookuppnvp(&pn, resolved != NULL ? &rpn : NULL,
	    FOLLOW, NULL, out, root, root, cr);
	if (resolved != NULL) {
		if (error == 0)
			(void) strlcpy(resolved, rpn.pn_path, MAXPATHLEN);
		pn_free(&rpn);
	}
	pn_free(&pn);
	return (error);
}

static int
lx_mntns_lookup_at(vnode_t *root, const char *path, cred_t *cr,
    vnode_t **out)
{
	return (lx_mntns_lookup_path(root, path, cr, out, NULL));
}

/* Consumes one reserved charge on both success and failure. */
static int
lx_mntns_create(vnode_t *source, cred_t *cr, cred_t *owner,
    lx_mntns_budget_t *budget, lx_mntns_view_t **out)
{
	lx_mntns_view_t *view;
	int error;

	*out = NULL;
	if (source->v_type != VDIR) {
		lx_mntns_uncharge(budget);
		return (ENOTDIR);
	}
	view = kmem_zalloc(sizeof (*view), KM_SLEEP);
	mutex_init(&view->lv_lock, NULL, MUTEX_DEFAULT, NULL);
	view->lv_refs = 1;
	view->lv_cred = cr;
	view->lv_budget = budget;
	crhold(view->lv_owner = owner);
	crhold(cr);
	atomic_inc_uint(&lx_mntns_nviews);
	mutex_enter(&lx_mntns_lock);
	if (lx_mntns_taskq == NULL) {
		lx_mntns_taskq = taskq_create("lx_mntns_reap", 1,
		    minclsyspri, 1, 1, 0);
	}
	mutex_exit(&lx_mntns_lock);
	/* Source is pinned; domount must not resolve its pathname again. */
	error = lx_mntns_mount("lx-private-root", "/", "lofs",
	    (source->v_vfsp->vfs_flag & VFS_NOEXEC) != 0 ?
	    "nosub,noexec" : "nosub", MS_NOSPLICE, source, source, cr,
	    &view->lv_vfs);
	if (error != 0)
		goto fail;
	if ((error = VFS_ROOT(view->lv_vfs, &view->lv_root)) != 0) {
		VERIFY0(dounmount(view->lv_vfs, 0, cr));
		VFS_RELE(view->lv_vfs);
		goto fail;
	}
	*out = view;
	return (0);
fail:
	crfree(view->lv_cred);
	crfree(view->lv_owner);
	mutex_destroy(&view->lv_lock);
	lx_mntns_uncharge(view->lv_budget);
	kmem_free(view, sizeof (*view));
	atomic_dec_uint(&lx_mntns_nviews);
	return (error);
}

int
lx_mntns_view_create(const char *source, cred_t *cr, cred_t *owner,
    lx_mntns_budget_t *budget, lx_mntns_view_t **out)
{
	vnode_t *vp;
	int error;

	*out = NULL;
	if (source == NULL || source[0] != '/' || owner == NULL)
		return (EINVAL);
	if ((error = lx_mntns_charge(budget)) != 0)
		return (error);
	if ((error = lookupname(source, UIO_SYSSPACE, FOLLOW, NULL,
	    &vp)) != 0) {
		lx_mntns_uncharge(budget);
		return (error);
	}
	error = lx_mntns_create(vp, cr, owner, budget, out);
	VN_RELE(vp);
	return (error);
}

/* The caller holds lv_lock; a covered vnode must belong to this view. */
static boolean_t
lx_mntns_owns(lx_mntns_view_t *view, vnode_t *vp)
{
	lx_mntns_mount_t *mount;

	if (vp->v_vfsp == view->lv_vfs)
		return (B_TRUE);
	for (mount = view->lv_mounts; mount != NULL; mount = mount->lm_next) {
		if (vp->v_vfsp == mount->lm_vfs)
			return (B_TRUE);
	}
	return (B_FALSE);
}

/* Preparation may block; never call it while holding a process lock. */
int
lx_mntns_state_prepare(lx_mntns_view_t *view, const char *cwd, cred_t *cr,
    lx_mntns_state_t *state)
{
	int error;

	bzero(state, sizeof (*state));
	lx_mntns_view_hold(view);
	state->lms_view = view;
	mutex_enter(&view->lv_lock);
	VN_HOLD(state->lms_root = view->lv_root);
	error = lx_mntns_lookup_at(view->lv_root, cwd, cr, &state->lms_cwd);
	if (error == 0 && state->lms_cwd->v_type != VDIR)
		error = ENOTDIR;
	if (error == 0 && !lx_mntns_owns(view, state->lms_cwd))
		error = EOPNOTSUPP;
	if (error == 0)
		error = VOP_ACCESS(state->lms_cwd, VEXEC, 0, cr, NULL);
	mutex_exit(&view->lv_lock);
	if (error != 0)
		lx_mntns_state_fini(state);
	return (error);
}

void
lx_mntns_state_fini(lx_mntns_state_t *state)
{
	if (state->lms_root != NULL)
		VN_RELE(state->lms_root);
	if (state->lms_cwd != NULL)
		VN_RELE(state->lms_cwd);
	if (state->lms_cwdname != NULL)
		refstr_rele(state->lms_cwdname);
	if (state->lms_view != NULL)
		lx_mntns_view_rele(state->lms_view);
	bzero(state, sizeof (*state));
}

/*
 * Publish prepared references without sleeping under p_lock. The caller
 * keeps the expected old vnodes held while preparing and revalidating the
 * target. Success returns the old process references in state, to release
 * outside p_lock. No Linux syscall or namespace authority is supplied here.
 */
int
lx_mntns_proc_swap(proc_t *p, vnode_t *expected_root, vnode_t *expected_cwd,
    lx_mntns_state_t *state)
{
	lx_mntns_state_t old;
	lx_proc_data_t *pd = ptolxproc(p);
	kthread_t *task = p->p_tlist;
	user_t *up = PTOU(p);

	ASSERT(MUTEX_HELD(&p->p_lock));
	if (p->p_brand != &lx_brand || pd == NULL || !lx_kernel_restricted(p))
		return (EPERM);
	if (state->lms_view != NULL && (ztolxzd(p->p_zone) == NULL ||
	    state->lms_view->lv_budget !=
	    ztolxzd(p->p_zone)->lxzd_mntns_budget ||
	    crgetzone(state->lms_view->lv_owner) != p->p_zone))
		return (EXDEV);
	if (state->lms_view == NULL || state->lms_root == NULL ||
	    state->lms_cwd == NULL || p->p_lwpcnt != 1 || task == NULL ||
	    (task->t_proc_flag & TP_KTHREAD) != 0)
		return (EINVAL);
	if ((p->p_flag & SVFORK) != 0 || pd->l_ptrace != 0 ||
	    lx_clone_grp_member(pd, LX_CLONE_FS))
		return (EOPNOTSUPP);
	if ((p != curproc && p->p_stat != SIDL &&
	    task->t_state != TS_STOPPED) ||
	    lwptolxlwp(ttolwp(task))->br_stack_mode != LX_STACK_MODE_BRAND)
		return (EBUSY);
	if (up->u_rdir != expected_root || up->u_cdir != expected_cwd)
		return (EAGAIN);
	old.lms_view = pd->l_mntns;
	old.lms_root = up->u_rdir;
	old.lms_cwd = up->u_cdir;
	old.lms_cwdname = up->u_cwd;
	pd->l_mntns = state->lms_view;
	up->u_rdir = state->lms_root;
	up->u_cdir = state->lms_cwd;
	up->u_cwd = state->lms_cwdname;
	*state = old;
	return (0);
}

/* A VFS reference alone does not prevent native unmount of a live graft. */
static int
lx_mntns_pin(lx_mntns_mount_t *mount, cred_t *cr)
{
	int error;

	if ((error = VFS_ROOT(mount->lm_vfs, &mount->lm_root)) != 0) {
		VERIFY0(vn_vfswlock(mount->lm_vfs->vfs_vnodecovered));
		VERIFY0(dounmount(mount->lm_vfs, 0, cr));
		VFS_RELE(mount->lm_vfs);
	}
	return (error);
}

static int
lx_mntns_bind(lx_mntns_view_t *view, vnode_t *source,
    const char *path, boolean_t readonly, boolean_t copy, cred_t *cr)
{
	lx_mntns_mount_t *mount;
	vnode_t *covered;
	const char *options;
	char *resolved;
	int error;

	if (path == NULL || path[0] != '/' || strcmp(path, "/") == 0 ||
	    (copy ? (source->v_type == VNON || source->v_type == VBAD) :
	    (source->v_type != VDIR && source->v_type != VREG)))
		return (EINVAL);
	mount = kmem_zalloc(sizeof (*mount), KM_SLEEP);
	resolved = kmem_alloc(MAXPATHLEN, KM_SLEEP);
	mutex_enter(&view->lv_lock);
	if ((error = lx_mntns_lookup_path(view->lv_root, path, cr,
	    &covered, resolved)) != 0)
		goto out;
	if (!lx_mntns_owns(view, covered) ||
	    (copy ? (covered->v_type == VNON || covered->v_type == VBAD) :
	    covered->v_type != source->v_type)) {
		error = EINVAL;
	} else {
		/* Keep noexec when binding a source from another view. */
		options = (source->v_vfsp->vfs_flag & VFS_NOEXEC) != 0 ?
		    "nosub,noexec" : "nosub";
		error = lx_mntns_mount("lx-private-bind", path, "lofs", options,
		    readonly ? MS_RDONLY : 0, covered, source, cr,
		    &mount->lm_vfs);
		if (error == 0)
			error = lx_mntns_pin(mount, cr);
	}
	VN_RELE(covered);
	if (error == 0) {
		mount->lm_path = refstr_alloc(resolved);
		mount->lm_next = view->lv_mounts;
		view->lv_mounts = mount;
		mount = NULL;
	}
out:
	mutex_exit(&view->lv_lock);
	kmem_free(resolved, MAXPATHLEN);
	if (mount != NULL)
		kmem_free(mount, sizeof (*mount));
	return (error);
}

int
lx_mntns_view_bind(lx_mntns_view_t *view, vnode_t *source,
    const char *path, boolean_t readonly, cred_t *cr)
{
	return (lx_mntns_bind(view, source, path, readonly, B_FALSE, cr));
}

/* A held VFS identifies the mount instance; it does not prevent unmount. */
typedef struct lx_mntns_snapshot {
	vfs_t *ls_vfs;
	refstr_t *ls_path;
	vnode_t *ls_root;
	struct lx_mntns_snapshot *ls_next;
} lx_mntns_snapshot_t;

int
lx_mntns_view_snapshot(const char *source, cred_t *cr, cred_t *owner,
    lx_mntns_budget_t *budget, lx_mntns_view_t **out)
{
	lx_mntns_snapshot_t *list = NULL, *entry, **link;
	lx_mntns_view_t *view = NULL;
	vfs_t *vfsp;
	vnode_t *root = NULL, *vp;
	char *path;
	size_t len, count = 0;
	timespec_t before, after;
	int error;

	*out = NULL;
	if (source == NULL || source[0] != '/' || owner == NULL)
		return (EINVAL);
	if ((error = lx_mntns_charge(budget)) != 0)
		return (error);
	path = kmem_alloc(MAXPATHLEN, KM_SLEEP);
	if ((error = lookupname(source, UIO_SYSSPACE, FOLLOW, NULL,
	    &root)) != 0)
		goto out;
	if (root->v_type != VDIR) {
		error = ENOTDIR;
		goto out;
	}
	if ((error = vnodetopath(rootdir, root, path, MAXPATHLEN, cr)) != 0)
		goto out;
	len = strcmp(path, "/") == 0 ? 0 : strlen(path);

	/* Collect names without doing pathname lookup under the VFS lock. */
	vfs_list_read_lock();
	vfs_mnttab_modtime(&before);
	vfsp = rootvfs;
	do {
		const char *name = refstr_value(vfsp->vfs_mntpt);

		if (strncmp(name, path, len) != 0 || name[len] != '/' ||
		    name[len + 1] == '\0')
			continue;
		if (++count > 4096) {
			error = E2BIG;
			break;
		}
		entry = kmem_zalloc(sizeof (*entry), KM_NOSLEEP);
		if (entry == NULL) {
			error = ENOMEM;
			break;
		}
		entry->ls_vfs = vfsp;
		VFS_HOLD(vfsp);
		entry->ls_path = vfsp->vfs_mntpt;
		refstr_hold(entry->ls_path);
		/* Parents must be grafted before their nested submounts. */
		for (link = &list; *link != NULL; link = &(*link)->ls_next) {
			if (strlen(refstr_value((*link)->ls_path)) >
			    strlen(name))
				break;
		}
		entry->ls_next = *link;
		*link = entry;
	} while ((vfsp = vfsp->vfs_next) != rootvfs);
	vfs_list_unlock();
	if (error != 0)
		goto out;

	/*
	 * Resolve visible roots through the captured source, never by a
	 * second global pathname. Obscured/stacked mounts are not copied.
	 * A vnode pins each source after lookup. If any mount changed while
	 * collecting, return EAGAIN before creating the new tree.
	 */
	for (entry = list; entry != NULL; entry = entry->ls_next) {
		error = lx_mntns_lookup_at(root,
		    refstr_value(entry->ls_path) + len, cr, &vp);
		if (error == ENOENT || error == ENOTDIR) {
			error = 0;
			continue;
		}
		if (error != 0)
			goto out;
		if (vp->v_vfsp == entry->ls_vfs && (vp->v_flag & VROOT))
			entry->ls_root = vp;
		else
			VN_RELE(vp);
	}
	vfs_list_read_lock();
	vfs_mnttab_modtime(&after);
	vfs_list_unlock();
	if (before.tv_sec != after.tv_sec || before.tv_nsec != after.tv_nsec) {
		error = EAGAIN;
		goto out;
	}
	error = lx_mntns_create(root, cr, owner, budget, &view);
	budget = NULL;
	if (error != 0)
		goto out;
	for (entry = list; entry != NULL; entry = entry->ls_next) {
		if (entry->ls_root == NULL)
			continue;
		/* Native roots need not have the type of their mountpoint. */
		error = lx_mntns_bind(view, entry->ls_root,
		    refstr_value(entry->ls_path) + len, B_FALSE, B_TRUE, cr);
		if (error != 0)
			break;
	}
	if (error == 0) {
		*out = view;
		view = NULL;
	}
out:
	while ((entry = list) != NULL) {
		list = entry->ls_next;
		if (entry->ls_root != NULL)
			VN_RELE(entry->ls_root);
		VFS_RELE(entry->ls_vfs);
		refstr_rele(entry->ls_path);
		kmem_free(entry, sizeof (*entry));
	}
	if (root != NULL)
		VN_RELE(root);
	/* No references to a failed, unpublished tree have escaped. */
	if (view != NULL)
		VERIFY0(lx_mntns_view_destroy(view, cr));
	lx_mntns_uncharge(budget);
	kmem_free(path, MAXPATHLEN);
	return (error);
}

typedef struct lx_mntns_clone_mount {
	vnode_t *lc_source;
	refstr_t *lc_path;
	struct lx_mntns_clone_mount *lc_next;
} lx_mntns_clone_mount_t;

/*
 * Copy the private tree without rediscovering its mounts through the global
 * mount list. Replay in creation order, including mounts hidden by a later
 * overmount. The new LOFS instances pin the original backing vnodes: file
 * contents remain shared, while subsequent mount operations stay private.
 *
 * The recorded paths require a stable directory hierarchy for the lifetime
 * of the source and copy. This backend does not publish a process namespace.
 * Released sources remain queued for cleanup while copies pin their vnodes.
 */
int
lx_mntns_view_clone(lx_mntns_view_t *source, cred_t *cr, cred_t *owner,
    lx_mntns_view_t **out)
{
	lx_mntns_clone_mount_t *list = NULL, *entry;
	lx_mntns_mount_t *mount;
	lx_mntns_view_t *view = NULL;
	lx_mntns_budget_t *budget = source->lv_budget;
	size_t count = 0;
	int error = 0;

	*out = NULL;
	if (owner == NULL)
		return (EINVAL);
	if ((error = lx_mntns_charge(budget)) != 0)
		return (error);
	mutex_enter(&source->lv_lock);
	for (mount = source->lv_mounts; mount != NULL;
	    mount = mount->lm_next) {
		if (++count > 4096) {
			error = E2BIG;
			goto out;
		}
		entry = kmem_zalloc(sizeof (*entry), KM_SLEEP);
		entry->lc_path = mount->lm_path;
		refstr_hold(entry->lc_path);
		entry->lc_next = list;
		list = entry;
		error = VFS_ROOT(mount->lm_vfs, &entry->lc_source);
		if (error != 0)
			goto out;
	}
	error = lx_mntns_create(source->lv_root, cr, owner, budget, &view);
	budget = NULL;
	if (error != 0)
		goto out;
	for (entry = list; entry != NULL; entry = entry->lc_next) {
		error = lx_mntns_bind(view, entry->lc_source,
		    refstr_value(entry->lc_path), B_FALSE, B_TRUE, cr);
		if (error != 0)
			goto out;
	}
	*out = view;
	view = NULL;
out:
	while ((entry = list) != NULL) {
		list = entry->lc_next;
		if (entry->lc_source != NULL)
			VN_RELE(entry->lc_source);
		refstr_rele(entry->lc_path);
		kmem_free(entry, sizeof (*entry));
	}
	if (view != NULL)
		VERIFY0(lx_mntns_view_destroy(view, cr));
	lx_mntns_uncharge(budget);
	mutex_exit(&source->lv_lock);
	return (error);
}

int
lx_mntns_view_lookup(lx_mntns_view_t *view, const char *path, cred_t *cr,
    vnode_t **out)
{
	int error;

	mutex_enter(&view->lv_lock);
	error = lx_mntns_lookup_at(view->lv_root, path, cr, out);
	mutex_exit(&view->lv_lock);
	return (error);
}

int
lx_mntns_view_tmpfs(lx_mntns_view_t *view, const char *path, cred_t *cr)
{
	lx_mntns_mount_t *mount;
	vnode_t *covered;
	char *resolved;
	int error;

	if (path == NULL || path[0] != '/' || strcmp(path, "/") == 0)
		return (EINVAL);
	mount = kmem_zalloc(sizeof (*mount), KM_SLEEP);
	resolved = kmem_alloc(MAXPATHLEN, KM_SLEEP);
	mutex_enter(&view->lv_lock);
	if ((error = lx_mntns_lookup_path(view->lv_root, path, cr,
	    &covered, resolved)) != 0)
		goto out;
	/* Only graft onto a mount belonging to this view. */
	if (!lx_mntns_owns(view, covered) || covered->v_type != VDIR) {
		error = EINVAL;
	} else {
		error = lx_mntns_mount("swap", path, "tmpfs", "mode=0700",
		    MS_NOSUID, covered, NULL, cr, &mount->lm_vfs);
		if (error == 0)
			error = lx_mntns_pin(mount, cr);
	}
	VN_RELE(covered);
	if (error == 0) {
		mount->lm_path = refstr_alloc(resolved);
		mount->lm_next = view->lv_mounts;
		view->lv_mounts = mount;
		mount = NULL;
	}
out:
	mutex_exit(&view->lv_lock);
	kmem_free(resolved, MAXPATHLEN);
	if (mount != NULL)
		kmem_free(mount, sizeof (*mount));
	return (error);
}

static int
lx_mntns_destroy(lx_mntns_view_t *view, cred_t *cr)
{
	lx_mntns_mount_t *mount;
	vnode_t *covered;
	int error;

	mutex_enter(&view->lv_lock);
	while ((mount = view->lv_mounts) != NULL) {
		covered = mount->lm_vfs->vfs_vnodecovered;
		if ((error = vn_vfswlock(covered)) != 0)
			goto busy;
		VN_RELE(mount->lm_root);
		mount->lm_root = NULL;
		/* dounmount consumes the covered-vnode lock. */
		if ((error = dounmount(mount->lm_vfs, 0, cr)) != 0) {
			VERIFY0(VFS_ROOT(mount->lm_vfs, &mount->lm_root));
			goto busy;
		}
		view->lv_mounts = mount->lm_next;
		VFS_RELE(mount->lm_vfs);
		refstr_rele(mount->lm_path);
		kmem_free(mount, sizeof (*mount));
	}
	VN_RELE(view->lv_root);
	view->lv_root = NULL;
	error = dounmount(view->lv_vfs, 0, cr);
	if (error != 0) {
		VERIFY0(VFS_ROOT(view->lv_vfs, &view->lv_root));
		goto busy;
	}
	VFS_RELE(view->lv_vfs);
	mutex_exit(&view->lv_lock);
	crfree(view->lv_cred);
	crfree(view->lv_owner);
	mutex_destroy(&view->lv_lock);
	lx_mntns_uncharge(view->lv_budget);
	kmem_free(view, sizeof (*view));
	atomic_dec_uint(&lx_mntns_nviews);
	return (0);
busy:
	mutex_exit(&view->lv_lock);
	return (error);
}

int
lx_mntns_view_destroy(lx_mntns_view_t *view, cred_t *cr)
{
	int error;

	if (atomic_cas_uint(&view->lv_refs, 1, 0) != 1)
		return (EBUSY);
	if ((error = lx_mntns_destroy(view, cr)) != 0)
		VERIFY3U(atomic_swap_uint(&view->lv_refs, 1), ==, 0);
	return (error);
}
