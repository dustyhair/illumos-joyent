/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * A copy is available at http://www.illumos.org/license/CDDL.
 */
#include <sys/types.h>
#include <sys/errno.h>
#include <sys/debug.h>
#include <sys/kmem.h>
#include <sys/mount.h>
#include <sys/pathname.h>
#include <sys/systm.h>
#include <sys/vfs.h>
#include <sys/vnode.h>
#include <sys/lx_mntns.h>

typedef struct lx_mntns_mount {
	vfs_t *lm_vfs;
	struct lx_mntns_mount *lm_next;
} lx_mntns_mount_t;

struct lx_mntns_view {
	kmutex_t lv_lock;
	vnode_t *lv_root;
	vfs_t *lv_vfs;
	lx_mntns_mount_t *lv_mounts;
};

static int
lx_mntns_mount(const char *source, const char *path, const char *type,
    const char *options, int flags, vnode_t *covered, cred_t *cr, vfs_t **out)
{
	struct mounta args;
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
	*out = NULL;
	error = domount((char *)type, &args, covered, cr, out);
	kmem_free(opts, MAX_MNTOPT_STR);
	return (error);
}

static int
lx_mntns_lookup(lx_mntns_view_t *view, const char *path, cred_t *cr,
    vnode_t **out)
{
	struct pathname pn;
	int error;

	*out = NULL;
	if (path == NULL || path[0] != '/')
		return (EINVAL);
	if ((error = pn_get(path, UIO_SYSSPACE, &pn)) != 0)
		return (error);
	pn_skipslash(&pn);
	/* lookuppnvp consumes the start and non-global root references. */
	VN_HOLD(view->lv_root);
	if (view->lv_root != rootdir)
		VN_HOLD(view->lv_root);
	error = lookuppnvp(&pn, NULL, FOLLOW, NULL, out, view->lv_root,
	    view->lv_root, cr);
	pn_free(&pn);
	return (error);
}

int
lx_mntns_view_create(const char *source, cred_t *cr, lx_mntns_view_t **out)
{
	lx_mntns_view_t *view;
	vnode_t *covered;
	int error;

	*out = NULL;
	if (source == NULL || source[0] != '/')
		return (EINVAL);
	if ((error = lookupname(source, UIO_SYSSPACE, FOLLOW, NULL,
	    &covered)) != 0)
		return (error);
	if (covered->v_type != VDIR) {
		VN_RELE(covered);
		return (ENOTDIR);
	}
	view = kmem_zalloc(sizeof (*view), KM_SLEEP);
	mutex_init(&view->lv_lock, NULL, MUTEX_DEFAULT, NULL);
	/*
	 * A detached root is never mounted over the source. nosub prevents
	 * later source mounts from leaking into this private view. Existing
	 * submounts must be copied explicitly by namespace construction.
	 */
	error = lx_mntns_mount(source, "/", "lofs", "nosub", MS_NOSPLICE,
	    covered, cr, &view->lv_vfs);
	VN_RELE(covered);
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
	mutex_destroy(&view->lv_lock);
	kmem_free(view, sizeof (*view));
	return (error);
}

int
lx_mntns_view_lookup(lx_mntns_view_t *view, const char *path, cred_t *cr,
    vnode_t **out)
{
	int error;

	mutex_enter(&view->lv_lock);
	error = lx_mntns_lookup(view, path, cr, out);
	mutex_exit(&view->lv_lock);
	return (error);
}

int
lx_mntns_view_tmpfs(lx_mntns_view_t *view, const char *path, cred_t *cr)
{
	lx_mntns_mount_t *mount;
	vnode_t *covered;
	int error;

	if (path == NULL || path[0] != '/' || strcmp(path, "/") == 0)
		return (EINVAL);
	mount = kmem_zalloc(sizeof (*mount), KM_SLEEP);
	mutex_enter(&view->lv_lock);
	if ((error = lx_mntns_lookup(view, path, cr, &covered)) != 0)
		goto out;
	/* Only graft onto an alias belonging to this view's detached root. */
	if (covered->v_vfsp != view->lv_vfs || covered->v_type != VDIR) {
		error = EINVAL;
	} else {
		error = lx_mntns_mount("swap", path, "tmpfs", "mode=0700",
		    MS_NOSUID, covered, cr, &mount->lm_vfs);
	}
	VN_RELE(covered);
	if (error == 0) {
		mount->lm_next = view->lv_mounts;
		view->lv_mounts = mount;
		mount = NULL;
	}
out:
	mutex_exit(&view->lv_lock);
	if (mount != NULL)
		kmem_free(mount, sizeof (*mount));
	return (error);
}

int
lx_mntns_view_destroy(lx_mntns_view_t *view, cred_t *cr)
{
	lx_mntns_mount_t *mount;
	vnode_t *covered;
	int error;

	mutex_enter(&view->lv_lock);
	while ((mount = view->lv_mounts) != NULL) {
		covered = mount->lm_vfs->vfs_vnodecovered;
		if ((error = vn_vfswlock(covered)) != 0)
			goto busy;
		/* dounmount consumes the covered-vnode lock. */
		if ((error = dounmount(mount->lm_vfs, 0, cr)) != 0)
			goto busy;
		view->lv_mounts = mount->lm_next;
		VFS_RELE(mount->lm_vfs);
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
	mutex_destroy(&view->lv_lock);
	kmem_free(view, sizeof (*view));
	return (0);
busy:
	mutex_exit(&view->lv_lock);
	return (error);
}
