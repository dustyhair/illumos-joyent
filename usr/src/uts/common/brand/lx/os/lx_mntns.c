/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * A copy is available at http://www.illumos.org/license/CDDL.
 */
#include <sys/types.h>
#include <sys/atomic.h>
#include <sys/brand.h>
#include <sys/disp.h>
#include <sys/dnlc.h>
#include <sys/dtrace.h>
#include <sys/errno.h>
#include <sys/debug.h>
#include <sys/kmem.h>
#include <sys/fs/lofs_info.h>
#include <sys/fs/tmp.h>
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
#include <sys/machbrand.h>
#include <sys/lx_cap.h>
#include <sys/lx_userns.h>
#include <sys/lx_mntns.h>
#include <lx_syscall.h>

extern brand_t lx_brand;

/* Public mount-namespace entry remains disabled outside disposable tests. */
int lx_mntns_experimental = 0;

/* Bound retained mount records, including mounts hidden by an overmount. */
#define	LX_MNTNS_MAX_MOUNTS	4096

typedef struct lx_mntns_mount {
	vfs_t *lm_vfs;
	vnode_t *lm_root;
	refstr_t *lm_path;
	struct lx_mntns_mount *lm_next;
} lx_mntns_mount_t;

typedef struct lx_mntns_clone_mount {
	vnode_t *lc_source;
	refstr_t *lc_path;
	struct lx_mntns_clone_mount *lc_next;
} lx_mntns_clone_mount_t;

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
	uint_t lv_nmounts;
};

static kmutex_t lx_mntns_lock;
static taskq_t *lx_mntns_taskq;
static volatile uint_t lx_mntns_nviews;

static int lx_mntns_destroy(lx_mntns_view_t *, cred_t *);
static int lx_mntns_tree_fini(lx_mntns_view_t *, cred_t *);
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
    const tmpfs_mount_args_t *tmpfsargs, cred_t *cr, vfs_t **out)
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
	} else if (tmpfsargs != NULL) {
		args.flags |= MS_DATA;
		args.dataptr = (char *)tmpfsargs;
		args.datalen = sizeof (*tmpfsargs);
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
	    "nosub,noexec" : "nosub", MS_NOSPLICE, source, source, NULL, cr,
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

/* A private view's canonical root is distinct from the zone's vnode. */
boolean_t
lx_mntns_proc_at_root(proc_t *p)
{
	lx_proc_data_t *pd = ptolxproc(p);

	ASSERT(MUTEX_HELD(&p->p_lock));
	return (pd != NULL && pd->l_mntns != NULL &&
	    PTOU(p)->u_rdir == pd->l_mntns->lv_root);
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
	if (p->p_brand != &lx_brand || pd == NULL)
		return (EPERM);
	/* Only current amd64 unshare may activate the guard. */
	if (!lx_kernel_restricted(p) && (p != curproc || task != curthread ||
	    get_udatamodel() != DATAMODEL_NATIVE ||
	    ttolxlwp(curthread)->br_syscall_entry != BRAND_ENTRY_AMD64 ||
	    ttolxlwp(curthread)->br_syscall_num != 272 ||
	    ttolxlwp(curthread)->br_stack_mode != LX_STACK_MODE_BRAND))
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

static boolean_t
lx_mntns_bindable(vtype_t type)
{
	switch (type) {
	case VDIR:
	case VREG:
	case VCHR:
	case VBLK:
	case VFIFO:
	case VSOCK:
		return (B_TRUE);
	default:
		return (B_FALSE);
	}
}

/* lv_lock serializes publication and keeps every graft in the private tree. */
static int
lx_mntns_bind_locked(lx_mntns_view_t *view, vnode_t *source,
    vnode_t *covered, const char *path, boolean_t readonly, boolean_t copy)
{
	lx_mntns_mount_t *mount;
	const char *options;
	int error;

	ASSERT(MUTEX_HELD(&view->lv_lock));
	if (view->lv_nmounts >= LX_MNTNS_MAX_MOUNTS)
		return (ENOSPC);
	if (!lx_mntns_owns(view, covered) ||
	    (copy ? (covered->v_type == VNON || covered->v_type == VBAD) :
	    ((covered->v_type == VDIR) != (source->v_type == VDIR))))
		return (EINVAL);
	mount = kmem_zalloc(sizeof (*mount), KM_SLEEP);
	/* Keep noexec when binding a source from another view. */
	options = (source->v_vfsp->vfs_flag & VFS_NOEXEC) != 0 ?
	    "nosub,noexec" : "nosub";
	error = lx_mntns_mount("lx-private-bind", path, "lofs", options,
	    readonly ? MS_RDONLY : 0, covered, source, NULL, view->lv_cred,
	    &mount->lm_vfs);
	if (error == 0)
		error = lx_mntns_pin(mount, view->lv_cred);
	if (error != 0) {
		kmem_free(mount, sizeof (*mount));
		return (error);
	}
	mount->lm_path = refstr_alloc(path);
	mount->lm_next = view->lv_mounts;
	view->lv_mounts = mount;
	view->lv_nmounts++;
	return (0);
}

static int
lx_mntns_bind(lx_mntns_view_t *view, vnode_t *source,
    const char *path, boolean_t readonly, boolean_t copy, cred_t *cr)
{
	vnode_t *covered;
	char *resolved;
	int error;

	if (path == NULL || path[0] != '/' || strcmp(path, "/") == 0 ||
	    (copy ? (source->v_type == VNON || source->v_type == VBAD) :
	    !lx_mntns_bindable(source->v_type)))
		return (EINVAL);
	resolved = kmem_alloc(MAXPATHLEN, KM_SLEEP);
	mutex_enter(&view->lv_lock);
	if ((error = lx_mntns_lookup_path(view->lv_root, path, cr,
	    &covered, resolved)) == 0) {
		error = lx_mntns_bind_locked(view, source, covered, resolved,
		    readonly, copy);
		VN_RELE(covered);
	}
	mutex_exit(&view->lv_lock);
	kmem_free(resolved, MAXPATHLEN);
	return (error);
}

/*
 * Unspliced mounts have no global pathname. Translate their cached vnode path
 * using the recorded mountpoint, then validate it against this exact view.
 * A cached name is only a candidate; backing-vnode equality is insufficient.
 */
static int
lx_mntns_path_locked(lx_mntns_view_t *view, vnode_t *vp, cred_t *actor,
    char *resolved)
{
	lx_mntns_mount_t *mount;
	vnode_t *check = NULL;
	const char *base = NULL, *prefix, *suffix;
	char *cached;
	size_t len;
	int error;

	ASSERT(MUTEX_HELD(&view->lv_lock));
	if (vp->v_vfsp == view->lv_vfs)
		base = "/";
	else {
		for (mount = view->lv_mounts; mount != NULL;
		    mount = mount->lm_next) {
			if (vp->v_vfsp == mount->lm_vfs) {
				base = refstr_value(mount->lm_path);
				break;
			}
		}
	}
	if (base == NULL)
		return (EINVAL);
	cached = kmem_alloc(MAXPATHLEN, KM_SLEEP);
	mutex_enter(&vp->v_lock);
	(void) strlcpy(cached, vp->v_path, MAXPATHLEN);
	mutex_exit(&vp->v_lock);
	prefix = refstr_value(vp->v_vfsp->vfs_mntpt);
	len = strlen(prefix);
	if (strncmp(cached, prefix, len) != 0 ||
	    (cached[len] != '\0' && cached[len] != '/')) {
		error = EAGAIN;
		goto out;
	}
	suffix = cached + len;
	if (*suffix == '/')
		suffix++;
	if (snprintf(resolved, MAXPATHLEN, "%s%s%s", base,
	    strcmp(base, "/") == 0 || *suffix == '\0' ? "" : "/", suffix) >=
	    MAXPATHLEN) {
		error = ENAMETOOLONG;
		goto out;
	}
	error = lx_mntns_lookup_at(view->lv_root, resolved, actor, &check);
	if (error == 0 && check != vp)
		error = EAGAIN;
	if (check != NULL)
		VN_RELE(check);
out:
	kmem_free(cached, MAXPATHLEN);
	return (error);
}

/* Process-link display names also need the private, validated path. */
int
lx_mntns_vnodetopath(vnode_t *vp, char *buffer, size_t size, cred_t *actor)
{
	lx_mntns_view_t *view = lx_mntns_current();
	char *path;
	int error;

	if (view == NULL)
		return (ENOENT);
	path = kmem_alloc(MAXPATHLEN, KM_SLEEP);
	mutex_enter(&view->lv_lock);
	if (!lx_mntns_owns(view, vp))
		error = ENOENT;
	else if ((error = lx_mntns_path_locked(view, vp, actor, path)) == 0) {
		if (strlcpy(buffer, path, size) >= size)
			error = ENAMETOOLONG;
	}
	mutex_exit(&view->lv_lock);
	lx_mntns_view_rele(view);
	kmem_free(path, MAXPATHLEN);
	return (error);
}

/* Mount ancestry retains covered stacks as well as visible mounts. */
static boolean_t
lx_mntns_descendant(lx_mntns_view_t *view, vfs_t *vfs, vfs_t *ancestor)
{
	lx_mntns_mount_t *mount;
	uint_t depth;

	for (depth = 0; depth <= view->lv_nmounts; depth++) {
		if (vfs == ancestor)
			return (B_TRUE);
		for (mount = view->lv_mounts; mount != NULL;
		    mount = mount->lm_next) {
			if (mount->lm_vfs == vfs)
				break;
		}
		if (mount == NULL || vfs->vfs_vnodecovered == NULL)
			return (B_FALSE);
		vfs = vfs->vfs_vnodecovered->v_vfsp;
	}
	return (B_FALSE);
}

/*
 * Publish an already constructed private tree at one held target. Authority
 * and target membership were checked by the caller. Preserve its prepared
 * mount policy while exchanging the unspliced reference for a namespace one.
 */
static int
lx_mntns_attach(vnode_t *covered, vfs_t *vfs)
{
	ulong_t saved, mask = VFS_RDONLY | VFS_NOSETUID | VFS_NODEVICES |
	    VFS_NOMNTTAB;
	int error, flags = MS_NOMNTTAB;

	zone_hold(curzone);
	mount_in_progress(curzone);
	if ((error = vn_vfswlock(covered)) != 0)
		goto out;
	if (vn_mountedvfs(covered) != NULL || (covered->v_flag & VNOMOUNT)) {
		error = (covered->v_flag & VNOMOUNT) ? EINVAL : EBUSY;
		vn_vfsunlock(covered);
		goto out;
	}
	vfs_lock_wait(vfs);
	ASSERT(vfs->vfs_vnodecovered == NULL && vfs->vfs_zone == NULL);
	saved = vfs->vfs_flag;
	if (saved & VFS_RDONLY)
		flags |= MS_RDONLY;
	if ((saved & (VFS_NOSETUID | VFS_NODEVICES)) ==
	    (VFS_NOSETUID | VFS_NODEVICES))
		flags |= MS_NOSUID;
	dnlc_purge_vp(covered);
	vfs_add(covered, vfs, flags);
	vfs->vfs_flag = (vfs->vfs_flag & ~mask) | (saved & mask);
	vfs_unlock(vfs);
	VFS_RELE(vfs);
	vn_vfsunlock(covered);
	error = 0;
out:
	mount_completed(curzone);
	zone_rele(curzone);
	return (error);
}

/* Prepare off-tree so a failed recursive copy publishes nothing. */
static int
lx_mntns_rbind_locked(lx_mntns_view_t *view, vnode_t *from,
    vnode_t *covered, const char *destination, cred_t *actor)
{
	lx_mntns_clone_mount_t *list = NULL, *entry;
	lx_mntns_mount_t *mount, *base = NULL, *tail;
	lx_mntns_view_t *stage;
	char *source, *full;
	const char *name, *options;
	size_t len;
	uint_t count = 1;
	int error;

	ASSERT(MUTEX_HELD(&view->lv_lock));
	if (!lx_mntns_owns(view, from))
		return (EOPNOTSUPP);
	source = kmem_alloc(MAXPATHLEN, KM_SLEEP);
	full = kmem_alloc(MAXPATHLEN, KM_SLEEP);
	stage = kmem_zalloc(sizeof (*stage), KM_SLEEP);
	mutex_init(&stage->lv_lock, NULL, MUTEX_DEFAULT, NULL);
	stage->lv_cred = view->lv_cred;
	error = lx_mntns_path_locked(view, from, actor, source);
	if (error != 0)
		goto out;
	len = strcmp(source, "/") == 0 ? 0 : strlen(source);
	for (mount = view->lv_mounts; mount != NULL;
	    mount = mount->lm_next) {
		name = refstr_value(mount->lm_path);
		if (strncmp(name, source, len) != 0 || name[len] != '/' ||
		    name[len + 1] == '\0' || mount->lm_vfs == from->v_vfsp ||
		    !lx_mntns_descendant(view, mount->lm_vfs, from->v_vfsp))
			continue;
		if (++count > LX_MNTNS_MAX_MOUNTS - view->lv_nmounts) {
			error = ENOSPC;
			goto out;
		}
		entry = kmem_zalloc(sizeof (*entry), KM_SLEEP);
		VN_HOLD(entry->lc_source = mount->lm_root);
		entry->lc_path = refstr_alloc(name + len);
		entry->lc_next = list;
		list = entry;
	}
	if (count > LX_MNTNS_MAX_MOUNTS - view->lv_nmounts) {
		error = ENOSPC;
		goto out;
	}
	if (strlen(destination) + curzone->zone_rootpathlen - 1 > MAXPATHLEN) {
		error = ENAMETOOLONG;
		goto out;
	}
	options = (from->v_vfsp->vfs_flag & VFS_NOEXEC) != 0 ?
	    "nosub,noexec" : "nosub";
	error = lx_mntns_mount("lx-private-recursive", destination, "lofs",
	    options, MS_NOSPLICE, from, from, NULL, view->lv_cred,
	    &stage->lv_vfs);
	if (error != 0)
		goto out;
	vfs_lock_wait(stage->lv_vfs);
	vfs_setmntpoint(stage->lv_vfs, destination, 0);
	vfs_unlock(stage->lv_vfs);
	if ((error = VFS_ROOT(stage->lv_vfs, &stage->lv_root)) != 0) {
		VERIFY0(dounmount(stage->lv_vfs, 0, view->lv_cred));
		VFS_RELE(stage->lv_vfs);
		stage->lv_vfs = NULL;
		goto out;
	}
	for (entry = list; entry != NULL; entry = entry->lc_next) {
		error = lx_mntns_bind(stage, entry->lc_source,
		    refstr_value(entry->lc_path), B_FALSE, B_TRUE,
		    view->lv_cred);
		if (error != 0)
			goto out;
	}
	/* Allocate and validate all destination metadata before publication. */
	for (mount = stage->lv_mounts; mount != NULL;
	    mount = mount->lm_next) {
		if (snprintf(full, MAXPATHLEN, "%s%s", destination,
		    refstr_value(mount->lm_path)) >= MAXPATHLEN) {
			error = ENAMETOOLONG;
			goto out;
		}
		refstr_rele(mount->lm_path);
		mount->lm_path = refstr_alloc(full);
	}
	base = kmem_zalloc(sizeof (*base), KM_SLEEP);
	base->lm_vfs = stage->lv_vfs;
	base->lm_root = stage->lv_root;
	base->lm_path = refstr_alloc(destination);
	if ((error = lx_mntns_attach(covered, stage->lv_vfs)) != 0)
		goto out;
	base->lm_next = view->lv_mounts;
	if (stage->lv_mounts != NULL) {
		for (tail = stage->lv_mounts; tail->lm_next != NULL;
		    tail = tail->lm_next)
			;
		tail->lm_next = base;
		view->lv_mounts = stage->lv_mounts;
	} else {
		view->lv_mounts = base;
	}
	view->lv_nmounts += count;
	base = NULL;
	stage->lv_root = NULL;
	stage->lv_vfs = NULL;
	stage->lv_mounts = NULL;
	stage->lv_nmounts = 0;
out:
	while ((entry = list) != NULL) {
		list = entry->lc_next;
		VN_RELE(entry->lc_source);
		refstr_rele(entry->lc_path);
		kmem_free(entry, sizeof (*entry));
	}
	if (stage->lv_vfs != NULL) {
		mutex_enter(&stage->lv_lock);
		VERIFY0(lx_mntns_tree_fini(stage, view->lv_cred));
		mutex_exit(&stage->lv_lock);
	}
	if (base != NULL) {
		refstr_rele(base->lm_path);
		kmem_free(base, sizeof (*base));
	}
	mutex_destroy(&stage->lv_lock);
	kmem_free(stage, sizeof (*stage));
	kmem_free(source, MAXPATHLEN);
	kmem_free(full, MAXPATHLEN);
	return (error);
}

/*
 * Every allowed constructor creates private mounts: LOFS grafts use nosub,
 * tmpfs is a new instance, and copies replay into distinct mount objects.
 * Shared/unbindable creation is rejected. There is no peer or master link.
 * Both PRIVATE and SLAVE therefore preserve the existing private state,
 * recursively as well. Validate the authority and exact mountpoint anyway.
 */
int
lx_mntns_mount_private(lx_mntns_view_t *view, const char *target, cred_t *actor)
{
	vnode_t *vp = NULL;
	int error;

	if (!lx_mntns_entry_enabled() ||
	    !lx_mntns_view_capable(view, actor))
		return (EPERM);
	mutex_enter(&view->lv_lock);
	error = lookupname((char *)target, UIO_SYSSPACE, FOLLOW, NULL, &vp);
	if (error == 0 &&
	    (!lx_mntns_owns(view, vp) || !(vp->v_flag & VROOT)))
		error = EINVAL;
	if (vp != NULL)
		VN_RELE(vp);
	mutex_exit(&view->lv_lock);
	return (error);
}

/* Seal a tracked private LOFS constructor, without changing its source. */
int
lx_mntns_mount_readonly(lx_mntns_view_t *view, const char *target,
    uint_t restrictions, cred_t *actor)
{
	lofs_freeze_args_t args = { LOFS_FREEZE_ARGS_VERSION, restrictions };
	struct mounta ma = { 0 };
	lx_mntns_mount_t *mount;
	vnode_t *vp = NULL;
	vfs_t *vfs = NULL;
	boolean_t lofs;
	int error;

	if (!lx_mntns_entry_enabled() ||
	    !lx_mntns_view_capable(view, actor))
		return (EPERM);
	mutex_enter(&view->lv_lock);
	if ((error = lookupname((char *)target, UIO_SYSSPACE, FOLLOW,
	    NULL, &vp)) != 0)
		goto out;
	if (vp == view->lv_root)
		vfs = view->lv_vfs;
	for (mount = view->lv_mounts; vfs == NULL && mount != NULL;
	    mount = mount->lm_next) {
		if (vp == mount->lm_root)
			vfs = mount->lm_vfs;
	}
	if (vfs == NULL || !(vp->v_flag & VROOT)) {
		error = EINVAL;
		goto out;
	}
	RLOCK_VFSSW();
	lofs = strcmp(vfssw[vfs->vfs_fstype].vsw_name, "lofs") == 0;
	RUNLOCK_VFSSW();
	if (!lofs || (vp->v_type != VDIR && vp->v_type != VREG)) {
		error = EOPNOTSUPP;
		goto out;
	}
	ma.spec = "lx-private-freeze";
	ma.dir = (char *)target;
	ma.fstype = "lofs";
	ma.flags = MS_SYSSPACE | MS_DATA | MS_REMOUNT;
	ma.dataptr = (char *)&args;
	ma.datalen = sizeof (args);
	zone_hold(curzone);
	mount_in_progress(curzone);
	vfs_lock_wait(vfs);
	error = VFS_MOUNT(vfs, vp, &ma, view->lv_cred);
	vfs_unlock(vfs);
	mount_completed(curzone);
	zone_rele(curzone);
out:
	if (vp != NULL)
		VN_RELE(vp);
	mutex_exit(&view->lv_lock);
	return (error);
}

/*
 * Public bind. Resolve both operands with the actor's root,
 * cwd and credentials, then pass held vnodes to domount. The kernel mount
 * credential must never do a second, more privileged pathname lookup.
 */
int
lx_mntns_mount_bind(lx_mntns_view_t *view, const char *source,
    const char *target, boolean_t recursive, cred_t *actor)
{
	vnode_t *from = NULL, *covered = NULL;
	char *resolved;
	int error;

	if (!lx_mntns_entry_enabled() ||
	    !lx_mntns_view_capable(view, actor))
		return (EPERM);
	resolved = kmem_alloc(MAXPATHLEN, KM_SLEEP);
	mutex_enter(&view->lv_lock);
	error = lookupname((char *)source, UIO_SYSSPACE, FOLLOW, NULL, &from);
	if (error != 0)
		goto out;
	if (!lx_mntns_bindable(from->v_type)) {
		error = EINVAL;
		goto out;
	}
	error = lookupname((char *)target, UIO_SYSSPACE, FOLLOW, NULL,
	    &covered);
	if (error != 0)
		goto out;
	if (!lx_mntns_owns(view, covered)) {
		error = EINVAL;
		goto out;
	}
	if ((from->v_type == VDIR) != (covered->v_type == VDIR)) {
		error = ENOTDIR;
		goto out;
	}
	/* Root overmount requires a separate process-root handoff. */
	if (covered == view->lv_root) {
		error = EOPNOTSUPP;
		goto out;
	}
	error = lx_mntns_path_locked(view, covered, actor, resolved);
	if (error == 0) {
		if (recursive && from->v_type == VDIR)
			error = lx_mntns_rbind_locked(view, from, covered,
			    resolved, actor);
		else
			error = lx_mntns_bind_locked(view, from, covered,
			    resolved, B_FALSE, B_FALSE);
	}
out:
	if (covered != NULL)
		VN_RELE(covered);
	if (from != NULL)
		VN_RELE(from);
	mutex_exit(&view->lv_lock);
	kmem_free(resolved, MAXPATHLEN);
	return (error);
}

/* Native tmpfs initializes ownership and mode before domount publishes it. */
int
lx_mntns_mount_tmpfs(lx_mntns_view_t *view, const char *target,
    const char *options, uid_t uid, gid_t gid, cred_t *actor)
{
	tmpfs_mount_args_t args = { TMPFS_MOUNT_ARGS_VERSION, uid, gid };
	lx_mntns_mount_t *mount = NULL;
	vnode_t *covered = NULL;
	char *resolved;
	int error;

	if (!lx_mntns_entry_enabled() ||
	    !lx_mntns_view_capable(view, actor))
		return (EPERM);
	resolved = kmem_alloc(MAXPATHLEN, KM_SLEEP);
	mutex_enter(&view->lv_lock);
	if (view->lv_nmounts >= LX_MNTNS_MAX_MOUNTS) {
		error = ENOSPC;
		goto out;
	}
	error = lookupname((char *)target, UIO_SYSSPACE, FOLLOW, NULL,
	    &covered);
	if (error != 0)
		goto out;
	if (!lx_mntns_owns(view, covered)) {
		error = EINVAL;
		goto out;
	}
	if (covered->v_type != VDIR) {
		error = ENOTDIR;
		goto out;
	}
	if (covered == view->lv_root) {
		error = EOPNOTSUPP;
		goto out;
	}
	if ((error = lx_mntns_path_locked(view, covered, actor, resolved)) != 0)
		goto out;
	mount = kmem_zalloc(sizeof (*mount), KM_SLEEP);
	mount->lm_path = refstr_alloc(resolved);
	error = lx_mntns_mount("swap", resolved, "tmpfs", options, 0, covered,
	    NULL, &args, view->lv_cred, &mount->lm_vfs);
	if (error == 0)
		error = lx_mntns_pin(mount, view->lv_cred);
	if (error == 0) {
		mount->lm_next = view->lv_mounts;
		view->lv_mounts = mount;
		view->lv_nmounts++;
		mount = NULL;
	}
out:
	if (covered != NULL)
		VN_RELE(covered);
	if (mount != NULL) {
		refstr_rele(mount->lm_path);
		kmem_free(mount, sizeof (*mount));
	}
	mutex_exit(&view->lv_lock);
	kmem_free(resolved, MAXPATHLEN);
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

/* Consumes a reservation; the caller holds root and the global prefix. */
static int
lx_mntns_snapshot(vnode_t *root, const char *path, cred_t *cr, cred_t *owner,
    lx_mntns_budget_t *budget, lx_mntns_view_t **out)
{
	lx_mntns_snapshot_t *list = NULL, *entry, **link;
	lx_mntns_view_t *view = NULL;
	vfs_t *vfsp;
	vnode_t *vp;
	size_t len, count = 0;
	timespec_t before, after;
	int error = 0;

	*out = NULL;
	if (root->v_type != VDIR) {
		error = ENOTDIR;
		goto out;
	}
	len = strlen(path);
	while (len != 0 && path[len - 1] == '/')
		len--;

	/* Collect names without doing pathname lookup under the VFS lock. */
	vfs_list_read_lock();
	vfs_mnttab_modtime(&before);
	vfsp = rootvfs;
	do {
		const char *name = refstr_value(vfsp->vfs_mntpt);

		if (strncmp(name, path, len) != 0 || name[len] != '/' ||
		    name[len + 1] == '\0')
			continue;
		if (++count > LX_MNTNS_MAX_MOUNTS) {
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
	/* No references to a failed, unpublished tree have escaped. */
	if (view != NULL)
		VERIFY0(lx_mntns_view_destroy(view, cr));
	lx_mntns_uncharge(budget);
	return (error);
}

int
lx_mntns_view_snapshot(const char *source, cred_t *cr, cred_t *owner,
    lx_mntns_budget_t *budget, lx_mntns_view_t **out)
{
	vnode_t *root = NULL;
	char *path;
	int error;

	*out = NULL;
	if (source == NULL || source[0] != '/' || owner == NULL)
		return (EINVAL);
	if ((error = lx_mntns_charge(budget)) != 0)
		return (error);
	path = kmem_alloc(MAXPATHLEN, KM_SLEEP);
	if ((error = lookupname(source, UIO_SYSSPACE, FOLLOW, NULL,
	    &root)) == 0 && (error = vnodetopath(rootdir, root, path,
	    MAXPATHLEN, cr)) == 0) {
		error = lx_mntns_snapshot(root, path, cr, owner, budget, out);
		budget = NULL;
	}
	if (root != NULL)
		VN_RELE(root);
	lx_mntns_uncharge(budget);
	kmem_free(path, MAXPATHLEN);
	return (error);
}



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
		if (++count > LX_MNTNS_MAX_MOUNTS) {
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

boolean_t
lx_mntns_entry_enabled(void)
{
	return (lx_mntns_experimental != 0);
}

/*
 * The actor remains unprivileged natively. This kernel-only credential is
 * used solely to build the authorized private tree and later reclaim it.
 * Root/cwd and source are held snapshots, revalidated at publication.
 */
int
lx_mntns_entry_prepare(cred_t *owner, vnode_t *root, vnode_t *cwd,
    lx_mntns_view_t *source, lx_mntns_state_t *state)
{
	lx_mntns_view_t *view = NULL;
	lx_zone_data_t *zone = ztolxzd(curzone);
	lx_mntns_budget_t *budget;
	cred_t *native;
	char *path;
	int error;

	bzero(state, sizeof (*state));
	if (zone == NULL || !lx_mntns_entry_enabled() ||
	    crgetzone(owner) != curzone ||
	    !lx_userns_cred_capable(owner, owner, LX_CAP_SYS_ADMIN))
		return (EPERM);
	budget = zone->lxzd_mntns_budget;
	if (root == NULL)
		root = curzone->zone_rootvp;
	path = kmem_alloc(MAXPATHLEN, KM_SLEEP);
	if ((error = vnodetopath(root, cwd, path, MAXPATHLEN, owner)) != 0)
		goto out_path;
	native = crdup(curzone->zone_kcred);
	if (source != NULL) {
		error = lx_mntns_view_clone(source, native, owner, &view);
	} else if ((error = lx_mntns_charge(budget)) == 0) {
		error = lx_mntns_snapshot(root, curzone->zone_rootpath,
		    native, owner, budget, &view);
	}
	if (error == 0)
		error = lx_mntns_state_prepare(view, path, owner, state);
	if (view != NULL)
		lx_mntns_view_rele(view);
	crfree(native);
out_path:
	kmem_free(path, MAXPATHLEN);
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
	if (view->lv_nmounts >= LX_MNTNS_MAX_MOUNTS) {
		error = ENOSPC;
		goto out;
	}
	if ((error = lx_mntns_lookup_path(view->lv_root, path, cr,
	    &covered, resolved)) != 0)
		goto out;
	/* Only graft onto a mount belonging to this view. */
	if (!lx_mntns_owns(view, covered) || covered->v_type != VDIR) {
		error = EINVAL;
	} else {
		error = lx_mntns_mount("swap", path, "tmpfs", "mode=0700",
		    MS_NOSUID, covered, NULL, NULL, cr, &mount->lm_vfs);
		if (error == 0)
			error = lx_mntns_pin(mount, cr);
	}
	VN_RELE(covered);
	if (error == 0) {
		mount->lm_path = refstr_alloc(resolved);
		mount->lm_next = view->lv_mounts;
		view->lv_mounts = mount;
		view->lv_nmounts++;
		mount = NULL;
	}
out:
	mutex_exit(&view->lv_lock);
	kmem_free(resolved, MAXPATHLEN);
	if (mount != NULL)
		kmem_free(mount, sizeof (*mount));
	return (error);
}

/* Caller holds lv_lock; also used for unpublished subtree rollback. */
static int
lx_mntns_tree_fini(lx_mntns_view_t *view, cred_t *cr)
{
	lx_mntns_mount_t *mount;
	vnode_t *covered;
	int error;

	ASSERT(MUTEX_HELD(&view->lv_lock));
	while ((mount = view->lv_mounts) != NULL) {
		covered = mount->lm_vfs->vfs_vnodecovered;
		if ((error = vn_vfswlock(covered)) != 0)
			return (error);
		VN_RELE(mount->lm_root);
		mount->lm_root = NULL;
		/* dounmount consumes the covered-vnode lock. */
		if ((error = dounmount(mount->lm_vfs, 0, cr)) != 0) {
			VERIFY0(VFS_ROOT(mount->lm_vfs, &mount->lm_root));
			return (error);
		}
		view->lv_mounts = mount->lm_next;
		VERIFY3U(view->lv_nmounts, >, 0);
		view->lv_nmounts--;
		VFS_RELE(mount->lm_vfs);
		refstr_rele(mount->lm_path);
		kmem_free(mount, sizeof (*mount));
	}
	VN_RELE(view->lv_root);
	view->lv_root = NULL;
	error = dounmount(view->lv_vfs, 0, cr);
	if (error != 0) {
		VERIFY0(VFS_ROOT(view->lv_vfs, &view->lv_root));
		return (error);
	}
	VFS_RELE(view->lv_vfs);
	view->lv_vfs = NULL;
	return (0);
}

static int
lx_mntns_destroy(lx_mntns_view_t *view, cred_t *cr)
{
	int error;

	mutex_enter(&view->lv_lock);
	error = lx_mntns_tree_fini(view, cr);
	mutex_exit(&view->lv_lock);
	if (error != 0)
		return (error);
	crfree(view->lv_cred);
	crfree(view->lv_owner);
	mutex_destroy(&view->lv_lock);
	lx_mntns_uncharge(view->lv_budget);
	kmem_free(view, sizeof (*view));
	atomic_dec_uint(&lx_mntns_nviews);
	return (0);
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
