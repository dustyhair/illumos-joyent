/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * A copy is available at http://www.illumos.org/license/CDDL.
 */
#include <sys/types.h>
#include <sys/errno.h>
#include <sys/debug.h>
#include <sys/kmem.h>
#include <sys/fs/lofs_info.h>
#include <sys/refstr.h>
#include <sys/mount.h>
#include <sys/pathname.h>
#include <sys/systm.h>
#include <sys/vfs.h>
#include <sys/vnode.h>
#include <sys/lx_mntns.h>

typedef struct lx_mntns_mount {
	vfs_t *lm_vfs;
	refstr_t *lm_path;
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

static int
lx_mntns_create(vnode_t *source, cred_t *cr, lx_mntns_view_t **out)
{
	lx_mntns_view_t *view;
	int error;

	*out = NULL;
	if (source->v_type != VDIR)
		return (ENOTDIR);
	view = kmem_zalloc(sizeof (*view), KM_SLEEP);
	mutex_init(&view->lv_lock, NULL, MUTEX_DEFAULT, NULL);
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
	mutex_destroy(&view->lv_lock);
	kmem_free(view, sizeof (*view));
	return (error);
}

int
lx_mntns_view_create(const char *source, cred_t *cr, lx_mntns_view_t **out)
{
	vnode_t *vp;
	int error;

	*out = NULL;
	if (source == NULL || source[0] != '/')
		return (EINVAL);
	if ((error = lookupname(source, UIO_SYSSPACE, FOLLOW, NULL, &vp)) != 0)
		return (error);
	error = lx_mntns_create(vp, cr, out);
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

int
lx_mntns_view_bind(lx_mntns_view_t *view, vnode_t *source,
    const char *path, boolean_t readonly, cred_t *cr)
{
	lx_mntns_mount_t *mount;
	vnode_t *covered;
	const char *options;
	char *resolved;
	int error;

	if (path == NULL || path[0] != '/' || strcmp(path, "/") == 0 ||
	    (source->v_type != VDIR && source->v_type != VREG))
		return (EINVAL);
	mount = kmem_zalloc(sizeof (*mount), KM_SLEEP);
	resolved = kmem_alloc(MAXPATHLEN, KM_SLEEP);
	mutex_enter(&view->lv_lock);
	if ((error = lx_mntns_lookup_path(view->lv_root, path, cr,
	    &covered, resolved)) != 0)
		goto out;
	if (!lx_mntns_owns(view, covered) ||
	    covered->v_type != source->v_type) {
		error = EINVAL;
	} else {
		/* Keep noexec when binding a source from another view. */
		options = (source->v_vfsp->vfs_flag & VFS_NOEXEC) != 0 ?
		    "nosub,noexec" : "nosub";
		error = lx_mntns_mount("lx-private-bind", path, "lofs", options,
		    readonly ? MS_RDONLY : 0, covered, source, cr,
		    &mount->lm_vfs);
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

/* A held VFS identifies the mount instance; it does not prevent unmount. */
typedef struct lx_mntns_snapshot {
	vfs_t *ls_vfs;
	refstr_t *ls_path;
	vnode_t *ls_root;
	struct lx_mntns_snapshot *ls_next;
} lx_mntns_snapshot_t;

int
lx_mntns_view_snapshot(const char *source, cred_t *cr, lx_mntns_view_t **out)
{
	lx_mntns_snapshot_t *list = NULL, *entry, **link;
	lx_mntns_view_t *view = NULL;
	vfs_t *vfsp;
	vnode_t *root = NULL, *vp;
	char *path = kmem_alloc(MAXPATHLEN, KM_SLEEP);
	size_t len, count = 0;
	timespec_t before, after;
	int error;

	*out = NULL;
	if (source == NULL || source[0] != '/') {
		error = EINVAL;
		goto out;
	}
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
	if ((error = lx_mntns_create(root, cr, &view)) != 0)
		goto out;
	for (entry = list; entry != NULL; entry = entry->ls_next) {
		if (entry->ls_root == NULL)
			continue;
		error = lx_mntns_view_bind(view, entry->ls_root,
		    refstr_value(entry->ls_path) + len, B_FALSE, cr);
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
 * of the source and copy. This backend does not publish a process namespace
 * or provide deferred unmount. In particular,
 * the caller must retain a source view while a clone pins any of its vnodes.
 */
int
lx_mntns_view_clone(lx_mntns_view_t *source, cred_t *cr,
    lx_mntns_view_t **out)
{
	lx_mntns_clone_mount_t *list = NULL, *entry;
	lx_mntns_mount_t *mount;
	lx_mntns_view_t *view = NULL;
	size_t count = 0;
	int error = 0;

	*out = NULL;
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
	if ((error = lx_mntns_create(source->lv_root, cr, &view)) != 0)
		goto out;
	for (entry = list; entry != NULL; entry = entry->lc_next) {
		error = lx_mntns_view_bind(view, entry->lc_source,
		    refstr_value(entry->lc_path), B_FALSE, cr);
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
	mutex_destroy(&view->lv_lock);
	kmem_free(view, sizeof (*view));
	return (0);
busy:
	mutex_exit(&view->lv_lock);
	return (error);
}
