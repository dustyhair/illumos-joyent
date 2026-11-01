/*
 * CDDL version 1.0, http://www.illumos.org/license/CDDL.
 */
#include <sys/ktest.h>
#include <sys/kmem.h>
#include <sys/cred_impl.h>
#include <sys/priv_impl.h>
#include <sys/errno.h>
#include <sys/pathname.h>
#include <sys/vnode.h>
#include <sys/vfs.h>
#include <sys/mount.h>
#include <sys/systm.h>
#include <sys/sysmacros.h>
#include <sys/brand.h>
#include <sys/proc.h>
#include <sys/zone.h>
#include <sys/lx_brand.h>
#include <sys/machbrand.h>
#include <sys/lx_mntns.h>
#include <sys/lx_pidns.h>
#include <sys/lx_userns.h>
#include <sys/lx_cap.h>

extern brand_t lx_brand;
extern void lx_pcagent_lab_prepared(struct pid **, pid_t *);

extern void lx_userns_test_maps(ktest_ctx_hdl_t *);
extern void lx_userns_test_caps(ktest_ctx_hdl_t *);
extern void lx_userns_test_affinity(ktest_ctx_hdl_t *);
extern void lx_userns_test_native(ktest_ctx_hdl_t *);
extern void lx_userns_test_map_authority(ktest_ctx_hdl_t *);
extern void lx_userns_test_lifetime(ktest_ctx_hdl_t *);
extern void lx_userns_test_budget(ktest_ctx_hdl_t *);
extern void lx_userns_test_budget_race(ktest_ctx_hdl_t *);
extern void lx_userns_test_zone_budget_idle(ktest_ctx_hdl_t *);
extern void lx_userns_test_zone_budget_disabled(ktest_ctx_hdl_t *);
extern void lx_userns_test_zone_budget_hold(ktest_ctx_hdl_t *);
extern void lx_brand_cred_test_copies(ktest_ctx_hdl_t *);
extern void lx_brand_cred_test_access(ktest_ctx_hdl_t *);
extern void lx_proc_maps_test(ktest_ctx_hdl_t *);
extern void lx_userns_test_scope_attach(ktest_ctx_hdl_t *);
extern void lx_userns_test_scope_sids(ktest_ctx_hdl_t *);
extern void lx_userns_test_scope_sids_fini(void);
extern void lx_cap_native_test(ktest_ctx_hdl_t *);

/* Only the disposable lab driver prepares this private source tree. */
static void
ns_views(ktest_ctx_hdl_t *ctx)
{
	uchar_t *input;
	size_t size;
	lx_mntns_view_t *a = NULL, *b = NULL;
	vnode_t *held = NULL, *vp = NULL, *other = NULL;
	cred_t *cr = CRED();
	vattr_t attr;
	int error;
	boolean_t passed = B_FALSE;

	ktest_get_input(ctx, &input, &size);
	if (size < 2 || size > MAXPATHLEN || input[size - 1] != 0 ||
	    strlen((char *)input) != size - 1 ||
	    strncmp((char *)input, "/zones/lx-ns-kernel/", 20) != 0) {
		KT_ERROR(ctx, "invalid private namespace fixture path");
		return;
	}
#define	NS_CHECK(expr, why) do { \
	if (!(expr)) { \
		KT_FAIL(ctx, "%s", why); \
		goto out; \
	} \
} while (0)
	error = lx_mntns_view_create((char *)input, cr, cr, NULL, &a);
	NS_CHECK(error == 0, "cannot create detached view A");
	error = lx_mntns_view_create((char *)input, cr, cr, NULL, &b);
	NS_CHECK(error == 0, "cannot create detached view B");
	error = lx_mntns_view_lookup(a, "/sub/original", cr, &vp);
	NS_CHECK(error == 0, "initial file missing from A");
	error = lx_mntns_view_lookup(b, "/sub/original", cr, &other);
	NS_CHECK(error == 0, "initial file missing from B");
	NS_CHECK(vp->v_vfsp != other->v_vfsp,
	    "views share the same mount-instance vnode");
	VN_RELE(vp);
	vp = NULL;
	VN_RELE(other);
	other = NULL;
	error = lx_mntns_view_tmpfs(a, "/sub", cr);
	NS_CHECK(error == 0, "cannot graft tmpfs onto A");
	error = lx_mntns_view_lookup(a, "/sub/original", cr, &vp);
	NS_CHECK(error == ENOENT, "A still sees the covered file");
	error = lx_mntns_view_lookup(b, "/sub/original", cr, &vp);
	NS_CHECK(error == 0, "private mount changed sibling B");
	VN_RELE(vp);
	vp = NULL;
	error = lx_mntns_view_lookup(a, "/sub", cr, &held);
	NS_CHECK(error == 0, "A tmpfs root missing");
	bzero(&attr, sizeof (attr));
	attr.va_type = VREG;
	attr.va_mode = 0600;
	attr.va_mask = AT_TYPE | AT_MODE;
	error = VOP_CREATE(held, "private", &attr, EXCL, VWRITE,
	    &vp, cr, 0, NULL, NULL);
	NS_CHECK(error == 0, "cannot create A-private file");
	VN_RELE(vp);
	vp = NULL;
	error = lx_mntns_view_lookup(b, "/sub/private", cr, &vp);
	NS_CHECK(error == ENOENT, "A-private file leaked into B");
	error = lx_mntns_view_lookup(a, "/escape", cr, &vp);
	NS_CHECK(error == ENOENT, "absolute symlink escaped private root");
	error = lx_mntns_view_lookup(a, "/../escape", cr, &vp);
	NS_CHECK(error == ENOENT, "dot-dot escaped private root");
	error = lx_mntns_view_destroy(a, cr);
	if (error == 0)
		a = NULL;
	NS_CHECK(error == EBUSY, "destroy did not retain a busy mount tree");
	error = lx_mntns_view_lookup(a, "/sub/private", cr, &vp);
	NS_CHECK(error == 0, "busy view became unusable after failed destroy");
	VN_RELE(vp);
	vp = NULL;
	VN_RELE(held);
	held = NULL;
	error = lx_mntns_view_destroy(a, cr);
	if (error == 0)
		a = NULL;
	NS_CHECK(error == 0, "view A did not release after closing vnode");
	passed = B_TRUE;
out:
	if (vp != NULL)
		VN_RELE(vp);
	if (other != NULL)
		VN_RELE(other);
	if (held != NULL)
		VN_RELE(held);
	if (a != NULL && lx_mntns_view_destroy(a, cr) != 0) {
		KT_ERROR(ctx, "view A cleanup failed");
		passed = B_FALSE;
	}
	if (b != NULL && lx_mntns_view_destroy(b, cr) != 0) {
		KT_ERROR(ctx, "view B cleanup failed");
		passed = B_FALSE;
	}
	if (passed)
		KT_PASS(ctx);
#undef NS_CHECK
}

/* Check the actual reference object, without assuming other mounts are idle. */
static boolean_t
ns_vfs_zone_ref_once(vfs_t *vfs)
{
	zone_t *zone = vfs->vfs_zone;
	zone_ref_t *ref, *wanted = &vfs->vfs_implp->vi_zone_ref;
	uint_t matches = 0;
	uint64_t remaining;
	boolean_t once;

	mutex_enter(&zone->zone_lock);
	remaining = (uint64_t)zone->zone_ref + 1;
	for (ref = list_head(&zone->zone_ref_list); ref != NULL &&
	    remaining-- != 0; ref = list_next(&zone->zone_ref_list, ref)) {
		if (ref == wanted && ++matches == 2)
			break;
	}
	once = ref == NULL && matches == 1 && wanted->zref_zone == zone;
	mutex_exit(&zone->zone_lock);
	return (once);
}

static void
ns_reinsert(ktest_ctx_hdl_t *ctx)
{
	uchar_t *input;
	size_t size;
	lx_mntns_view_t *view = NULL;
	vnode_t *root = NULL, *file = NULL, *check = NULL, *covered = NULL;
	vfs_t *vfs;
	zone_t *owner;
	vattr_t attr = { 0 };
	boolean_t detached, passed = B_FALSE;
	int error, flags;
	uint_t i;

	ktest_get_input(ctx, &input, &size);
	if (size < 2 || size > MAXPATHLEN || input[size - 1] != 0 ||
	    strlen((char *)input) != size - 1 ||
	    strncmp((char *)input, "/zones/lx-ns-kernel/", 20) != 0) {
		KT_ERROR(ctx, "invalid private namespace fixture path");
		return;
	}
#define	NS_REINSERT_CHECK(expr, why) do { \
	if (!(expr)) { \
		KT_FAIL(ctx, "%s", why); \
		goto out; \
	} \
} while (0)
	error = lx_mntns_view_create((char *)input, CRED(), CRED(), NULL,
	    &view);
	NS_REINSERT_CHECK(error == 0, "cannot create private view");
	error = lx_mntns_view_tmpfs(view, "/pin", CRED());
	NS_REINSERT_CHECK(error == 0, "cannot create private tmpfs");
	error = lx_mntns_view_lookup(view, "/pin", CRED(), &root);
	NS_REINSERT_CHECK(error == 0, "cannot find tmpfs root");
	vfs = root->v_vfsp;
	owner = vfs->vfs_zone;
	VN_HOLD(covered = vfs->vfs_vnodecovered);
	attr.va_type = VREG;
	attr.va_mode = 0600;
	attr.va_mask = AT_TYPE | AT_MODE;
	error = VOP_CREATE(root, "retained", &attr, EXCL, VWRITE,
	    &file, CRED(), 0, NULL, NULL);
	NS_REINSERT_CHECK(error == 0, "cannot create retained file");
	for (i = 0; i < 4; i++) {
		NS_REINSERT_CHECK(ns_vfs_zone_ref_once(vfs),
		    "mount does not own exactly one zone reference");
		error = vn_vfswlock(covered);
		NS_REINSERT_CHECK(error == 0, "cannot lock covered vnode");
		vfs_lock_wait(vfs);
		vfs_detach(vfs);
		detached = vfs->vfs_vnodecovered == NULL &&
		    (vfs->vfs_flag & VFS_DETACHED) != 0 &&
		    vfs->vfs_zone == owner;
		flags = MS_NOMNTTAB;
		if (vfs->vfs_flag & VFS_RDONLY)
			flags |= MS_RDONLY;
		if ((vfs->vfs_flag & (VFS_NOSETUID | VFS_NODEVICES)) ==
		    (VFS_NOSETUID | VFS_NODEVICES))
			flags |= MS_NOSUID;
		vfs_lock_wait(vfs);
		vfs_add(covered, vfs, flags);
		vfs_unlock(vfs);
		/* Exchange the retained ownership hold for the splice hold. */
		VFS_RELE(vfs);
		vn_vfsunlock(covered);
		NS_REINSERT_CHECK(detached,
		    "detach lost native ownership state");
		NS_REINSERT_CHECK(vfs->vfs_vnodecovered == covered &&
		    !(vfs->vfs_flag & VFS_DETACHED) &&
		    vfs->vfs_zone == owner && ns_vfs_zone_ref_once(vfs),
		    "reattachment duplicated or replaced zone ownership");
		error = lx_mntns_view_lookup(view, "/pin/retained", CRED(),
		    &check);
		NS_REINSERT_CHECK(error == 0 && check == file,
		    "reattachment replaced the retained filesystem");
		VN_RELE(check);
		check = NULL;
	}
	passed = B_TRUE;
out:
	if (check != NULL)
		VN_RELE(check);
	if (file != NULL)
		VN_RELE(file);
	if (root != NULL)
		VN_RELE(root);
	if (covered != NULL)
		VN_RELE(covered);
	if (view != NULL && lx_mntns_view_destroy(view, CRED()) != 0) {
		KT_ERROR(ctx, "reinsert view cleanup failed");
		passed = B_FALSE;
	}
	if (passed)
		KT_PASS(ctx);
#undef NS_REINSERT_CHECK
}

static int
ns_cover(const char *path, cred_t *cr, vfs_t **out)
{
	struct mounta args;
	vnode_t *vp;
	char options[MAX_MNTOPT_STR] = "mode=0700";
	int error;

	*out = NULL;
	if ((error = lookupname((char *)path, UIO_SYSSPACE, FOLLOW, NULL,
	    &vp)) != 0)
		return (error);
	bzero(&args, sizeof (args));
	args.spec = "swap";
	args.dir = (char *)path;
	args.fstype = "tmpfs";
	args.flags = MS_SYSSPACE | MS_OPTIONSTR | MS_OVERLAY | MS_NOMNTTAB;
	args.optptr = options;
	args.optlen = sizeof (options);
	error = domount("tmpfs", &args, vp, cr, out);
	VN_RELE(vp);
	return (error);
}

static int
ns_uncover(vfs_t *vfsp, cred_t *cr)
{
	int error;

	if ((error = vn_vfswlock(vfsp->vfs_vnodecovered)) != 0)
		return (error);
	if ((error = dounmount(vfsp, 0, cr)) == 0)
		VFS_RELE(vfsp);
	return (error);
}

static void
ns_snapshot(ktest_ctx_hdl_t *ctx)
{
	uchar_t *input;
	size_t size;
	lx_mntns_view_t *a = NULL, *b = NULL;
	vnode_t *vp = NULL, *source = NULL;
	vfs_t *later = NULL, *cover = NULL;
	cred_t *cr = CRED();
	char *path = kmem_alloc(MAXPATHLEN, KM_SLEEP);
	int error = 0;
	boolean_t passed = B_FALSE;

	ktest_get_input(ctx, &input, &size);
	if (size < 2 || size > MAXPATHLEN || input[size - 1] != 0 ||
	    strlen((char *)input) != size - 1 ||
	    strncmp((char *)input, "/zones/lx-ns-kernel/", 20) != 0) {
		KT_ERROR(ctx, "invalid private namespace fixture path");
		kmem_free(path, MAXPATHLEN);
		return;
	}
#define	NS_CHECK(expr, why) do { \
	if (!(expr)) { \
		KT_FAIL(ctx, "%s, error=%d", why, error); \
		goto out; \
	} \
} while (0)
	error = lx_mntns_view_snapshot((char *)input, cr, cr, NULL, &a);
	NS_CHECK(error == 0, "cannot snapshot source into A");
	error = lx_mntns_view_snapshot((char *)input, cr, cr, NULL, &b);
	NS_CHECK(error == 0, "cannot snapshot source into B");
	error = lx_mntns_view_lookup(a, "/existing/mounted", cr, &vp);
	NS_CHECK(error == 0, "pre-existing tmpfs missing in snapshot");
	VN_RELE(vp);
	vp = NULL;
	error = lx_mntns_view_lookup(a, "/existing/deep/nested", cr, &vp);
	NS_CHECK(error == 0, "nested source mount missing in snapshot");
	VN_RELE(vp);
	vp = NULL;
	error = lx_mntns_view_lookup(a, "/existing/underlying", cr, &vp);
	NS_CHECK(error == ENOENT, "snapshot exposed covered physical file");
	error = lx_mntns_view_lookup(a, "/stacked/visible", cr, &vp);
	NS_CHECK(error == 0, "top stacked source mount missing");
	VN_RELE(vp);
	vp = NULL;
	error = lx_mntns_view_lookup(a, "/stacked/hidden", cr, &vp);
	NS_CHECK(error == ENOENT, "snapshot exposed lower stacked mount");
	error = lx_mntns_view_lookup(a, "/readonly/file", cr, &vp);
	NS_CHECK(error == 0, "source read-only bind missing");
	NS_CHECK(vn_is_readonly(vp), "source read-only restriction lost");
	NS_CHECK((vp->v_vfsp->vfs_flag & VFS_NOEXEC) != 0,
	    "source noexec restriction lost");
	error = VOP_ACCESS(vp, VWRITE, 0, cr, NULL);
	NS_CHECK(error == EROFS, "source read-only bind allows write access");
	VN_RELE(vp);
	vp = NULL;

	(void) snprintf(path, MAXPATHLEN, "%s/existing", (char *)input);
	error = lookupname(path, UIO_SYSSPACE, FOLLOW, NULL, &source);
	NS_CHECK(error == 0, "cannot pin original source mount");
	error = ns_cover(path, cr, &cover);
	NS_CHECK(error == 0, "cannot overmount original source");
	error = lx_mntns_view_bind(a, source, "/pin", B_FALSE, cr);
	NS_CHECK(error == 0, "cannot bind a pinned overmounted source");
	VN_RELE(source);
	source = NULL;
	error = lx_mntns_view_lookup(a, "/pin/mounted", cr, &vp);
	NS_CHECK(error == 0, "pinned bind followed a later source overmount");
	VN_RELE(vp);
	vp = NULL;
	error = lx_mntns_view_lookup(a, "/existing/deep/nested", cr, &vp);
	NS_CHECK(error == 0, "source overmount changed nested snapshot");
	VN_RELE(vp);
	vp = NULL;

	(void) snprintf(path, MAXPATHLEN, "%s/later", (char *)input);
	error = ns_cover(path, cr, &later);
	NS_CHECK(error == 0, "cannot mount later source submount");
	error = lx_mntns_view_lookup(a, "/later/original", cr, &vp);
	NS_CHECK(error == 0, "later source mount propagated into snapshot");
	VN_RELE(vp);
	vp = NULL;
	error = lx_mntns_view_tmpfs(a, "/existing/deep", cr);
	NS_CHECK(error == 0, "cannot cover imported nested mount privately");
	error = lx_mntns_view_lookup(a, "/existing/deep/nested", cr, &vp);
	NS_CHECK(error == ENOENT, "private nested cover failed");
	error = lx_mntns_view_lookup(b, "/existing/deep/nested", cr, &vp);
	NS_CHECK(error == 0, "private nested cover changed sibling snapshot");
	VN_RELE(vp);
	vp = NULL;

	error = lx_mntns_view_lookup(b, "/existing/mounted", cr, &source);
	NS_CHECK(error == 0, "cannot find regular file bind source");
	error = lx_mntns_view_bind(a, source, "/bind-target", B_TRUE, cr);
	NS_CHECK(error == 0, "cannot bind a regular file read-only");
	error = lx_mntns_view_lookup(a, "/bind-target", cr, &vp);
	NS_CHECK(error == 0 && vp->v_type == VREG,
	    "regular file bind missing");
	error = VOP_ACCESS(vp, VWRITE, 0, cr, NULL);
	NS_CHECK(error == EROFS, "regular file bind allows write access");
	VN_RELE(vp);
	vp = NULL;
	error = lx_mntns_view_bind(a, source, "/sub", B_FALSE, cr);
	NS_CHECK(error == EINVAL, "bind accepted mismatched file types");
	VN_RELE(source);
	source = NULL;
	passed = B_TRUE;
out:
	if (vp != NULL)
		VN_RELE(vp);
	if (source != NULL)
		VN_RELE(source);
	if (a != NULL && lx_mntns_view_destroy(a, cr) != 0) {
		KT_ERROR(ctx, "snapshot A cleanup failed");
		passed = B_FALSE;
	}
	if (b != NULL && lx_mntns_view_destroy(b, cr) != 0) {
		KT_ERROR(ctx, "snapshot B cleanup failed");
		passed = B_FALSE;
	}
	if (cover != NULL && ns_uncover(cover, cr) != 0) {
		KT_ERROR(ctx, "source overmount cleanup failed");
		passed = B_FALSE;
	}
	if (later != NULL && ns_uncover(later, cr) != 0) {
		KT_ERROR(ctx, "later source mount cleanup failed");
		passed = B_FALSE;
	}
	kmem_free(path, MAXPATHLEN);
	if (passed)
		KT_PASS(ctx);
#undef NS_CHECK
}

static int
ns_make(lx_mntns_view_t *view, const char *parent, const char *name,
    vtype_t type, cred_t *cr)
{
	vnode_t *dir, *vp = NULL;
	vattr_t attr;
	int error;

	if ((error = lx_mntns_view_lookup(view, parent, cr, &dir)) != 0)
		return (error);
	bzero(&attr, sizeof (attr));
	attr.va_type = type;
	attr.va_mode = type == VDIR ? 0700 : 0600;
	attr.va_mask = AT_TYPE | AT_MODE;
	if (type == VDIR) {
		error = VOP_MKDIR(dir, (char *)name, &attr, &vp, cr, NULL, 0,
		    NULL);
	} else {
		error = VOP_CREATE(dir, (char *)name, &attr, EXCL, VWRITE,
		    &vp, cr, 0, NULL, NULL);
	}
	if (vp != NULL)
		VN_RELE(vp);
	VN_RELE(dir);
	return (error);
}

static void
ns_clone(ktest_ctx_hdl_t *ctx)
{
	uchar_t *input;
	size_t size;
	lx_mntns_view_t *a = NULL, *b = NULL, *c = NULL;
	vnode_t *vp = NULL, *other = NULL;
	cred_t *cr = CRED();
	int error = 0;
	boolean_t passed = B_FALSE;

	ktest_get_input(ctx, &input, &size);
	if (size < 2 || size > MAXPATHLEN || input[size - 1] != 0 ||
	    strlen((char *)input) != size - 1 ||
	    strncmp((char *)input, "/zones/lx-ns-kernel/", 20) != 0) {
		KT_ERROR(ctx, "invalid private namespace fixture path");
		return;
	}
#define	NS_CHECK(expr, why) do { \
	if (!(expr)) { \
		KT_FAIL(ctx, "%s, error=%d", why, error); \
		goto out; \
	} \
} while (0)
#define	NS_PRESENT(view, path, why) do { \
	error = lx_mntns_view_lookup(view, path, cr, &vp); \
	NS_CHECK(error == 0, why); \
	VN_RELE(vp); \
	vp = NULL; \
} while (0)
	error = lx_mntns_view_snapshot((char *)input, cr, cr, NULL, &a);
	NS_CHECK(error == 0, "cannot snapshot clone source");
	error = lx_mntns_view_tmpfs(a, "/sub", cr);
	NS_CHECK(error == 0, "cannot create private source mount");
	error = ns_make(a, "/sub", "private", VREG, cr);
	NS_CHECK(error == 0, "cannot create private source file");
	error = ns_make(a, "/sub", "deep", VDIR, cr);
	NS_CHECK(error == 0, "cannot create nested mountpoint");
	error = lx_mntns_view_tmpfs(a, "/sub/../sub/deep", cr);
	NS_CHECK(error == 0, "cannot create private nested mount");
	error = ns_make(a, "/sub/deep", "nested", VREG, cr);
	NS_CHECK(error == 0, "cannot create nested source file");
	error = lx_mntns_view_lookup(a, "/readonly/file", cr, &other);
	NS_CHECK(error == 0, "cannot find file bind source");
	error = lx_mntns_view_bind(a, other, "/bind-target", B_TRUE, cr);
	NS_CHECK(error == 0, "cannot create private read-only file bind");
	VN_RELE(other);
	other = NULL;
	error = lx_mntns_view_clone(a, cr, cr, &b);
	NS_CHECK(error == 0, "cannot clone private source");
	NS_PRESENT(b, "/sub/private", "private mount missing in clone");
	NS_PRESENT(b, "/sub/deep/nested", "nested mount missing in clone");
	NS_PRESENT(b, "/existing/deep/nested", "imported mount missing");
	NS_PRESENT(b, "/stacked/visible", "stacked source mount missing");
	error = lx_mntns_view_lookup(b, "/sub/original", cr, &vp);
	NS_CHECK(error == ENOENT, "clone exposed covered physical file");
	error = lx_mntns_view_lookup(a, "/sub/private", cr, &other);
	NS_CHECK(error == 0, "private source file disappeared");
	error = lx_mntns_view_lookup(b, "/sub/private", cr, &vp);
	NS_CHECK(error == 0, "cloned file disappeared");
	NS_CHECK(vp->v_vfsp != other->v_vfsp,
	    "clone shares source mount instance");
	VN_RELE(vp);
	vp = NULL;
	VN_RELE(other);
	other = NULL;
	error = ns_make(a, "/sub", "shared", VREG, cr);
	NS_CHECK(error == 0, "cannot update shared filesystem");
	NS_PRESENT(b, "/sub/shared", "clone does not share file contents");
	error = lx_mntns_view_tmpfs(b, "/sub/deep", cr);
	NS_CHECK(error == 0, "cannot overmount nested clone mount");
	error = lx_mntns_view_lookup(b, "/sub/deep/nested", cr, &vp);
	NS_CHECK(error == ENOENT, "private clone overmount failed");
	NS_PRESENT(a, "/sub/deep/nested", "clone mount changed source");
	error = lx_mntns_view_tmpfs(a, "/sub", cr);
	NS_CHECK(error == 0, "cannot overmount source after clone");
	NS_PRESENT(b, "/sub/private", "source mount propagated to clone");
	error = lx_mntns_view_clone(b, cr, cr, &c);
	NS_CHECK(error == 0, "cannot clone a clone with a hidden mount");
	NS_PRESENT(c, "/sub/private", "second clone lost private mount");
	error = lx_mntns_view_lookup(c, "/sub/deep/nested", cr, &vp);
	NS_CHECK(error == ENOENT, "second clone exposed hidden mount");
	error = lx_mntns_view_lookup(c, "/bind-target", cr, &vp);
	NS_CHECK(error == 0 && vp->v_type == VREG,
	    "second clone lost regular file bind");
	NS_CHECK(vn_is_readonly(vp), "clone lost read-only bind policy");
	NS_CHECK((vp->v_vfsp->vfs_flag & VFS_NOEXEC) != 0,
	    "clone lost noexec bind policy");
	NS_CHECK((vp->v_vfsp->vfs_flag &
	    (VFS_NODEVICES | VFS_NOSETUID)) ==
	    (VFS_NODEVICES | VFS_NOSETUID),
	    "clone lost nodev or nosetuid bind policy");
	error = VOP_ACCESS(vp, VWRITE, 0, cr, NULL);
	NS_CHECK(error == EROFS, "cloned read-only bind permits write");
	VN_RELE(vp);
	vp = NULL;
	error = lx_mntns_view_lookup(c, "/../escape", cr, &vp);
	NS_CHECK(error == ENOENT, "second clone escaped private root");
	error = lx_mntns_view_destroy(a, cr);
	if (error == 0)
		a = NULL;
	NS_CHECK(error == EBUSY, "source was destroyed despite pinned clone");
	NS_PRESENT(b, "/sub/private", "failed source destroy broke clone");
	passed = B_TRUE;
out:
	if (vp != NULL)
		VN_RELE(vp);
	if (other != NULL)
		VN_RELE(other);
	if (c != NULL && lx_mntns_view_destroy(c, cr) != 0) {
		KT_ERROR(ctx, "second clone cleanup failed");
		passed = B_FALSE;
	}
	if (b != NULL && lx_mntns_view_destroy(b, cr) != 0) {
		KT_ERROR(ctx, "first clone cleanup failed");
		passed = B_FALSE;
	}
	if (a != NULL && lx_mntns_view_destroy(a, cr) != 0) {
		KT_ERROR(ctx, "clone source cleanup failed");
		passed = B_FALSE;
	}
	if (passed)
		KT_PASS(ctx);
#undef NS_PRESENT
#undef NS_CHECK
}

static boolean_t
ns_wait_idle(void)
{
	int i;

	for (i = 0; i < 100; i++) {
		if (!lx_mntns_busy())
			return (B_TRUE);
		delay(MAX(1, hz / 10));
	}
	return (!lx_mntns_busy());
}

static int
ns_native_unmount(lx_mntns_view_t *view, const char *path, cred_t *cr)
{
	vnode_t *vp;
	vfs_t *vfsp;
	int error;

	if ((error = lx_mntns_view_lookup(view, path, cr, &vp)) != 0)
		return (error);
	vfsp = vp->v_vfsp;
	VFS_HOLD(vfsp);
	VN_RELE(vp);
	/* Only the view's own root hold should keep this graft busy. */
	error = ns_uncover(vfsp, cr);
	if (error != 0)
		VFS_RELE(vfsp);
	return (error);
}

static void
ns_lifetime(ktest_ctx_hdl_t *ctx)
{
	uchar_t *input;
	size_t size;
	lx_mntns_view_t *a = NULL, *b = NULL;
	vnode_t *held = NULL, *vp = NULL;
	cred_t *cr = crdup(CRED());
	vattr_t attr;
	int error = 0;
	boolean_t passed = B_FALSE;

	ktest_get_input(ctx, &input, &size);
	if (size < 2 || size > MAXPATHLEN || input[size - 1] != 0 ||
	    strlen((char *)input) != size - 1 ||
	    strncmp((char *)input, "/zones/lx-ns-kernel/", 20) != 0) {
		KT_ERROR(ctx, "invalid private namespace fixture path");
		crfree(cr);
		return;
	}
#define	NS_CHECK(expr, why) do { \
	if (!(expr)) { \
		KT_FAIL(ctx, "%s, error=%d", why, error); \
		goto out; \
	} \
} while (0)
	NS_CHECK(!lx_mntns_busy(), "unrelated mount views are active");
	error = lx_mntns_view_create((char *)input, cr, cr, NULL, &a);
	NS_CHECK(error == 0, "cannot create lifetime source view");
	lx_mntns_view_hold(a);
	error = lx_mntns_view_destroy(a, cr);
	if (error == 0)
		a = NULL;
	NS_CHECK(error == EBUSY, "destroy consumed a shared view");
	lx_mntns_view_rele(a);
	error = lx_mntns_view_tmpfs(a, "/sub", cr);
	NS_CHECK(error == 0, "shared view lost its remaining reference");
	error = ns_native_unmount(a, "/sub", cr);
	/* A broken kernel may have dismantled the graft; do not revisit it. */
	if (error == 0)
		a = NULL;
	NS_CHECK(error == EBUSY, "native unmount dismantled owned tmpfs");
	error = ns_make(a, "/sub", "retained", VREG, cr);
	NS_CHECK(error == 0, "cannot create retained file");
	error = lx_mntns_view_lookup(a, "/sub/retained", cr, &held);
	NS_CHECK(error == 0, "cannot hold retained file");
	error = lx_mntns_view_clone(a, cr, cr, &b);
	NS_CHECK(error == 0, "cannot clone lifetime source");
	error = ns_native_unmount(b, "/sub", cr);
	if (error == 0)
		b = NULL;
	NS_CHECK(error == EBUSY, "native unmount dismantled owned LOFS graft");
	/* The views keep the creation credential after the caller drops it. */
	crfree(cr);
	cr = NULL;
	lx_mntns_view_rele(a);
	a = NULL;
	delay(hz * 2);
	NS_CHECK(lx_mntns_busy(), "pinned source was freed too early");
	error = lx_mntns_view_lookup(b, "/sub/retained", CRED(), &vp);
	NS_CHECK(error == 0, "source release broke its surviving clone");
	VN_RELE(vp);
	vp = NULL;
	lx_mntns_view_rele(b);
	b = NULL;
	delay(hz * 2);
	NS_CHECK(lx_mntns_busy(), "open vnode did not retain busy mount");
	bzero(&attr, sizeof (attr));
	attr.va_mask = AT_TYPE;
	error = VOP_GETATTR(held, &attr, 0, CRED(), NULL);
	NS_CHECK(error == 0 && attr.va_type == VREG,
	    "held vnode became invalid after final view release");
	VN_RELE(held);
	held = NULL;
	NS_CHECK(ns_wait_idle(), "mount views leaked after closing last vnode");
	/* Exercise retirement with no grafts and a held detached-root vnode. */
	error = lx_mntns_view_create((char *)input, CRED(), CRED(), NULL, &a);
	NS_CHECK(error == 0, "cannot create root-only lifetime view");
	error = lx_mntns_view_lookup(a, "/", CRED(), &held);
	NS_CHECK(error == 0, "cannot hold detached root");
	lx_mntns_view_rele(a);
	a = NULL;
	delay(hz * 2);
	NS_CHECK(lx_mntns_busy(), "busy detached root was freed too early");
	VN_RELE(held);
	held = NULL;
	NS_CHECK(ns_wait_idle(), "detached root leaked after vnode release");
	passed = B_TRUE;
out:
	if (vp != NULL)
		VN_RELE(vp);
	if (held != NULL)
		VN_RELE(held);
	if (b != NULL)
		lx_mntns_view_rele(b);
	if (a != NULL)
		lx_mntns_view_rele(a);
	if (cr != NULL)
		crfree(cr);
	if (!ns_wait_idle()) {
		KT_ERROR(ctx, "lifetime cleanup left active mount views");
		passed = B_FALSE;
	}
	if (passed)
		KT_PASS(ctx);
#undef NS_CHECK
}

static void
ns_quota(ktest_ctx_hdl_t *ctx)
{
	uchar_t *input;
	size_t size;
	lx_mntns_budget_t *budget = NULL;
	lx_mntns_view_t *a = NULL, *b = NULL, *c = NULL;
	vnode_t *held = NULL;
	cred_t *reduced = NULL;
	uint32_t maximum, used;
	int error = 0;
	boolean_t passed = B_FALSE;

	ktest_get_input(ctx, &input, &size);
	if (size < 2 || size > MAXPATHLEN || input[size - 1] != 0 ||
	    strlen((char *)input) != size - 1 ||
	    strncmp((char *)input, "/zones/lx-ns-kernel/", 20) != 0) {
		KT_ERROR(ctx, "invalid private namespace fixture path");
		return;
	}
#define	NS_CHECK(expr, why) do { \
	if (!(expr)) { \
		KT_FAIL(ctx, "%s, error=%d", why, error); \
		goto out; \
	} \
} while (0)
	NS_CHECK(!lx_mntns_busy(), "unrelated mount views are active");
	budget = lx_mntns_budget_create(1);
	error = lx_mntns_view_create((char *)input, CRED(), CRED(), budget, &a);
	NS_CHECK(error == 0, "cannot create charged view");
	lx_mntns_budget_get(budget, &maximum, &used);
	NS_CHECK(maximum == 1 && used == 1, "new view was not charged");
	error = lx_mntns_view_create((char *)input, CRED(), CRED(), budget, &c);
	NS_CHECK(error == ENOSPC && c == NULL, "create bypassed quota");
	error = lx_mntns_view_snapshot((char *)input, CRED(), CRED(),
	    budget, &c);
	NS_CHECK(error == ENOSPC && c == NULL, "snapshot bypassed quota");
	error = lx_mntns_view_clone(a, CRED(), CRED(), &c);
	NS_CHECK(error == ENOSPC && c == NULL, "clone bypassed source quota");
	lx_mntns_view_hold(a);
	lx_mntns_view_rele(a);
	lx_mntns_budget_get(budget, &maximum, &used);
	NS_CHECK(used == 1, "sharing a view changed its charge");
	lx_mntns_budget_set(budget, 3);
	reduced = lx_userns_native_cred(CRED());
	error = lx_mntns_view_create((char *)input, reduced, reduced,
	    budget, &c);
	NS_CHECK(error == EPERM && c == NULL, "native mount denial changed");
	error = lx_mntns_view_snapshot((char *)input, reduced, reduced,
	    budget, &c);
	NS_CHECK(error == EPERM && c == NULL,
	    "snapshot gained native authority");
	error = lx_mntns_view_clone(a, reduced, reduced, &c);
	NS_CHECK(error == EPERM && c == NULL, "clone gained native authority");
	lx_mntns_budget_get(budget, &maximum, &used);
	NS_CHECK(used == 1, "failed construction leaked reservations");
	error = lx_mntns_view_tmpfs(a, "/sub", CRED());
	NS_CHECK(error == 0, "cannot mount quota fixture tmpfs");
	error = ns_make(a, "/sub", "quota-retained", VREG, CRED());
	NS_CHECK(error == 0, "cannot create retained quota file");
	error = lx_mntns_view_lookup(a, "/sub/quota-retained", CRED(), &held);
	NS_CHECK(error == 0, "cannot hold retained quota file");
	lx_mntns_budget_set(budget, 2);
	error = lx_mntns_view_clone(a, CRED(), CRED(), &b);
	NS_CHECK(error == 0, "cannot create charged clone");
	lx_mntns_view_rele(a);
	a = NULL;
	delay(hz * 2);
	lx_mntns_budget_get(budget, &maximum, &used);
	NS_CHECK(used == 2, "retired pinned source returned quota early");
	lx_mntns_budget_set(budget, 1);
	error = lx_mntns_view_clone(b, CRED(), CRED(), &c);
	NS_CHECK(error == ENOSPC && c == NULL,
	    "lowered quota admitted a clone");
	error = lx_mntns_view_destroy(b, CRED());
	if (error == 0)
		b = NULL;
	NS_CHECK(error == 0, "cannot destroy quota clone");
	lx_mntns_budget_get(budget, &maximum, &used);
	NS_CHECK(maximum == 1 && used == 1,
	    "destroy did not return one charge");
	lx_mntns_budget_set(budget, 0);
	error = lx_mntns_view_create((char *)input, CRED(), CRED(), budget, &c);
	NS_CHECK(error == ENOSPC && c == NULL, "zero quota admitted a view");
	VN_RELE(held);
	held = NULL;
	NS_CHECK(ns_wait_idle(), "quota source did not retire");
	lx_mntns_budget_get(budget, &maximum, &used);
	NS_CHECK(maximum == 0 && used == 0, "retirement leaked a charge");
	lx_mntns_budget_set(budget, 1);
	error = lx_mntns_view_snapshot((char *)input, CRED(), CRED(),
	    budget, &a);
	NS_CHECK(error == 0, "returned quota cannot be reused");
	error = lx_mntns_view_lookup(a, "/", CRED(), &held);
	NS_CHECK(error == 0, "cannot retain charged snapshot root");
	/* The account survives release of its zone/creator reference. */
	lx_mntns_budget_rele(budget);
	budget = NULL;
	lx_mntns_view_rele(a);
	a = NULL;
	delay(hz * 2);
	NS_CHECK(lx_mntns_busy(), "snapshot root lost deferred ownership");
	VN_RELE(held);
	held = NULL;
	NS_CHECK(ns_wait_idle(), "account-owner release broke retirement");
	passed = B_TRUE;
out:
	if (held != NULL)
		VN_RELE(held);
	if (c != NULL)
		lx_mntns_view_rele(c);
	if (b != NULL)
		lx_mntns_view_rele(b);
	if (a != NULL)
		lx_mntns_view_rele(a);
	if (reduced != NULL)
		crfree(reduced);
	if (!ns_wait_idle()) {
		KT_ERROR(ctx, "quota cleanup left active mount views");
		passed = B_FALSE;
	}
	if (budget != NULL)
		lx_mntns_budget_rele(budget);
	if (passed)
		KT_PASS(ctx);
#undef NS_CHECK
}

static void
ns_owner(ktest_ctx_hdl_t *ctx)
{
	uchar_t *input;
	size_t size;
	lx_userns_t *root = lx_userns_root_create(), *a = NULL, *b = NULL;
	lx_userns_t *sibling = NULL;
	lx_userns_cred_t actor = { 0 }, child, admin;
	cred_t *base = crdup(CRED()), *parent = NULL, *full = NULL;
	cred_t *weak = NULL, *nested = NULL, *peer = NULL, *saved = NULL;
	lx_mntns_view_t *initial = NULL, *owned = NULL, *copy = NULL;
	boolean_t passed = B_FALSE;
	int error = 0;

	ktest_get_input(ctx, &input, &size);
	if (size < 2 || size > MAXPATHLEN || input[size - 1] != 0 ||
	    strlen((char *)input) != size - 1 ||
	    strncmp((char *)input, "/zones/lx-ns-kernel/", 20) != 0) {
		KT_ERROR(ctx, "invalid private namespace fixture path");
		goto out;
	}
#define	NS_CHECK(expr, why) do { \
	if (!(expr)) { \
		KT_FAIL(ctx, "%s, error=%d", why, error); \
		goto out; \
	} \
} while (0)
	VERIFY0(crsetresuid(base, 1000, 1000, 1000));
	parent = lx_userns_native_cred(base);
	actor.luc_ns = root;
	actor.luc_uid = actor.luc_gid = 1000;
	error = lx_userns_child_create(&actor, &a);
	NS_CHECK(error == 0, "cannot create owner user namespace");
	error = lx_userns_child_create(&actor, &sibling);
	NS_CHECK(error == 0, "cannot create sibling user namespace");
	NS_CHECK(lx_userns_map_actor(CRED(), root, &admin) == 0 &&
	    lx_userns_map_write(a, 0, &admin, &admin, "0 1000 2", 8, 0) == 0 &&
	    lx_userns_map_write(a, 1, &admin, &admin, "0 1000 1", 8, 0) == 0,
	    "cannot map mount-owner namespace IDs");
	actor.luc_ns = a;
	lx_caps_new_namespace(&actor.luc_caps);
	full = lx_userns_native_for(base, &actor);
	actor.luc_caps.lc_effective &= ~LX_CAP_BIT(LX_CAP_SYS_ADMIN);
	weak = lx_userns_native_for(base, &actor);
	child = actor;
	child.luc_uid = 1001;
	error = lx_userns_child_create(&child, &b);
	NS_CHECK(error == 0, "cannot create descendant user namespace");
	child.luc_ns = b;
	lx_caps_new_namespace(&child.luc_caps);
	nested = lx_userns_native_for(base, &child);
	child.luc_ns = sibling;
	peer = lx_userns_native_for(base, &child);
	error = lx_mntns_view_create((char *)input, CRED(), parent, NULL,
	    &initial);
	NS_CHECK(error == 0, "cannot create initial-scope view");
	error = lx_mntns_view_snapshot((char *)input, CRED(), weak, NULL,
	    &owned);
	NS_CHECK(error == 0, "cannot create owned snapshot");
	error = lx_mntns_view_clone(owned, CRED(), nested, &copy);
	NS_CHECK(error == 0, "cannot clone into new owner namespace");
	NS_CHECK(lx_mntns_view_capable(owned, full) &&
	    !lx_mntns_view_capable(owned, weak),
	    "owner authority ignored current effective capability");
	NS_CHECK(!lx_mntns_view_capable(owned, nested) &&
	    !lx_mntns_view_capable(owned, peer),
	    "descendant or sibling acquired ancestor mount authority");
	NS_CHECK(lx_mntns_view_capable(copy, nested) &&
	    lx_mntns_view_capable(copy, full) &&
	    !lx_mntns_view_capable(copy, weak) &&
	    !lx_mntns_view_capable(copy, peer),
	    "clone did not use its explicit owner user namespace");
	NS_CHECK(lx_mntns_view_capable(owned, parent) &&
	    !lx_mntns_view_capable(initial, parent) &&
	    !lx_mntns_view_capable(initial, full) &&
	    lx_mntns_view_capable(initial, CRED()),
	    "scoped authority escaped or lost parent owner authority");
	saved = crdup(full);
	crfree(weak);
	weak = NULL;
	NS_CHECK(lx_mntns_view_capable(owned, saved),
	    "view did not retain its immutable owner credential");
	passed = B_TRUE;
out:
	if (copy != NULL)
		lx_mntns_view_rele(copy);
	if (owned != NULL)
		lx_mntns_view_rele(owned);
	if (initial != NULL)
		lx_mntns_view_rele(initial);
	if (saved != NULL)
		crfree(saved);
	if (peer != NULL)
		crfree(peer);
	if (nested != NULL)
		crfree(nested);
	if (weak != NULL)
		crfree(weak);
	if (full != NULL)
		crfree(full);
	if (parent != NULL)
		crfree(parent);
	crfree(base);
	lx_userns_rele(b);
	lx_userns_rele(sibling);
	lx_userns_rele(a);
	lx_userns_rele(root);
	if (!ns_wait_idle()) {
		KT_ERROR(ctx, "owner cleanup left active mount views");
		passed = B_FALSE;
	}
	if (passed)
		KT_PASS(ctx);
#undef NS_CHECK
}

static void
ns_zone_quota(ktest_ctx_hdl_t *ctx)
{
	zone_t *zone = zone_find_by_name("lx-probe");
	lx_mntns_budget_t *budget;
	lx_mntns_view_t *a = NULL, *b = NULL, *c = NULL;
	uint32_t maximum, used;
	int error = 0, i;
	boolean_t passed = B_FALSE;

	if (zone == NULL || ztolxzd(zone) == NULL) {
		if (zone != NULL)
			zone_rele(zone);
		KT_ERROR(ctx, "mount quota fixture zone is not ready");
		return;
	}
#define	NS_CHECK(expr, why) do { \
	if (!(expr)) { \
		KT_FAIL(ctx, "%s, error=%d", why, error); \
		goto out; \
	} \
} while (0)
	budget = ztolxzd(zone)->lxzd_mntns_budget;
	lx_mntns_budget_get(budget, &maximum, &used);
	NS_CHECK(used == 0 && (maximum == 0 || maximum == 2),
	    "unexpected native mount quota fixture configuration");
	error = lx_mntns_view_create("/zones/lx-ns-kernel/src", CRED(), CRED(),
	    budget, &a);
	if (maximum == 0) {
		NS_CHECK(error == ENOSPC && a == NULL,
		    "zero zone mount quota admitted a view");
	} else {
		NS_CHECK(error == 0, "cannot create zone-charged view");
		error = lx_mntns_view_clone(a, CRED(), CRED(), &b);
		NS_CHECK(error == 0, "cannot clone zone-charged view");
		/* Native controller observes used=2 and lowers the limit. */
		for (i = 0; i < 100; i++) {
			lx_mntns_budget_get(budget, &maximum, &used);
			if (maximum == 1)
				break;
			delay(MAX(1, hz / 10));
		}
		NS_CHECK(maximum == 1 && used == 2,
		    "native controller did not lower the live quota");
		error = lx_mntns_view_clone(b, CRED(), CRED(), &c);
		NS_CHECK(error == ENOSPC && c == NULL,
		    "lowered live zone quota admitted a view");
	}
	passed = B_TRUE;
out:
	if (c != NULL)
		lx_mntns_view_rele(c);
	if (b != NULL)
		lx_mntns_view_rele(b);
	if (a != NULL)
		lx_mntns_view_rele(a);
	if (!ns_wait_idle()) {
		KT_ERROR(ctx, "native quota fixture views did not retire");
		passed = B_FALSE;
	}
	lx_mntns_budget_get(budget, &maximum, &used);
	if (used != 0) {
		KT_FAIL(ctx, "native quota fixture leaked a charge");
		passed = B_FALSE;
	}
	zone_rele(zone);
	if (passed)
		KT_PASS(ctx);
#undef NS_CHECK
}

static boolean_t
ns_process_pid(ktest_ctx_hdl_t *ctx, pid_t *pid)
{
	uchar_t *input;
	size_t size, i;

	*pid = 0;
	ktest_get_input(ctx, &input, &size);
	if (size < 2 || size > 10 || input[size - 1] != 0)
		return (B_FALSE);
	for (i = 0; i + 1 < size; i++) {
		if (input[i] < '0' || input[i] > '9' || *pid >= 10000000)
			return (B_FALSE);
		*pid = *pid * 10 + input[i] - '0';
	}
	return (*pid > 1);
}

static void
ns_entry_inspect(ktest_ctx_hdl_t *ctx, boolean_t user, boolean_t mount)
{
	zone_t *zone = NULL;
	proc_t *p = NULL;
	pid_t pid;
	cred_t *cr = NULL;
	lx_mntns_view_t *view = NULL;
	vnode_t *root = NULL, *cwd = NULL, *vp = NULL;
	lx_caps_t caps;
	int error = 0;
	boolean_t passed = B_FALSE;

#define	NS_CHECK(expr, why) do { \
	if (!(expr)) { \
		KT_FAIL(ctx, "%s, error=%d", why, error); \
		goto out; \
	} \
} while (0)
	NS_CHECK(ns_process_pid(ctx, &pid), "invalid entry fixture PID");
	zone = zone_find_by_name("lx-probe");
	NS_CHECK(zone != NULL && lx_lpid_lock(pid, zone, 0, &p, NULL) == 0,
	    "cannot find entry fixture process");
	NS_CHECK(p->p_brand == &lx_brand && p->p_lwpcnt == 1 &&
	    p->p_tlist != NULL &&
	    p->p_tlist->t_state == TS_STOPPED &&
	    strcmp(p->p_user.u_comm, "mntns_entry") == 0 &&
	    ((ptolxproc(p)->l_flags & LX_PROC_USERNS) != 0) == user &&
	    (ptolxproc(p)->l_mntns != NULL) == mount,
	    "entry published unexpected namespace membership");
	NS_CHECK(!(user || mount) || lx_kernel_restricted(p),
	    "new namespace membership did not enforce native-entry guard");
	mutex_enter(&p->p_crlock);
	crhold(cr = p->p_cred);
	mutex_exit(&p->p_crlock);
	if ((root = PTOU(p)->u_rdir) != NULL)
		VN_HOLD(root);
	VN_HOLD(cwd = PTOU(p)->u_cdir);
	if ((view = ptolxproc(p)->l_mntns) != NULL)
		lx_mntns_view_hold(view);
	mutex_exit(&p->p_lock);
	p = NULL;
	if (user) {
		NS_CHECK(lx_userns_caps_get(cr, &caps) == 0 &&
		    !priv_ismember(&CR_OEPRIV(cr), PRIV_SYS_MOUNT),
		    "entry installed privileged native mount credentials");
	} else {
		NS_CHECK(crgetbrand(cr) == NULL,
		    "failed or mount-only entry changed credential namespace");
	}
	if (mount) {
		NS_CHECK(root != NULL && lx_mntns_view_capable(view, cr) &&
		    !lx_mntns_view_capable(view, CRED()),
		    "published view does not belong to the calling actor");
		error = lx_mntns_view_lookup(view, "/", cr, &vp);
		NS_CHECK(error == 0 && vp == root &&
		    root->v_vfsp != zone->zone_rootvp->v_vfsp,
		    "entry did not install a distinct private root mount");
		VN_RELE(vp);
		vp = NULL;
		error = lx_mntns_view_lookup(view, "/test/mnt-entry/cwd",
		    cr, &vp);
		NS_CHECK(error == 0 && vp == cwd,
		    "entry cwd does not refer to the private tree");
	} else {
		NS_CHECK(root == NULL || VN_CMP(root, zone->zone_rootvp),
		    "failed entry changed root boundary");
	}
	passed = B_TRUE;
out:
	if (p != NULL)
		mutex_exit(&p->p_lock);
	if (vp != NULL)
		VN_RELE(vp);
	if (root != NULL)
		VN_RELE(root);
	if (cwd != NULL)
		VN_RELE(cwd);
	if (view != NULL)
		lx_mntns_view_rele(view);
	if (cr != NULL)
		crfree(cr);
	if (zone != NULL)
		zone_rele(zone);
	if (passed)
		KT_PASS(ctx);
#undef NS_CHECK
}

static void ns_entry_mount(ktest_ctx_hdl_t *ctx)
{
	ns_entry_inspect(ctx, B_FALSE, B_TRUE);
}
static void ns_entry_user_mount(ktest_ctx_hdl_t *ctx)
{
	ns_entry_inspect(ctx, B_TRUE, B_TRUE);
}
static void ns_entry_reject(ktest_ctx_hdl_t *ctx)
{
	ns_entry_inspect(ctx, B_FALSE, B_FALSE);
}
static void ns_entry_user_only(ktest_ctx_hdl_t *ctx)
{
	ns_entry_inspect(ctx, B_TRUE, B_FALSE);
}

static void
ns_process_enter(ktest_ctx_hdl_t *ctx, boolean_t reject)
{
	zone_t *zone = NULL;
	proc_t *p = NULL;
	pid_t pid;
	cred_t *base = NULL;
	struct lx_seccomp_filter *filter = NULL;
	vnode_t *root = NULL, *cwd = NULL, *vp = NULL;
	lx_mntns_view_t *view = NULL, *copy = NULL, *foreign = NULL;
	lx_mntns_view_t *foreign_owner = NULL;
	lx_mntns_state_t state = { 0 }, wrong = { 0 }, wrong_owner = { 0 };
	uint32_t maximum, used;
	vattr_t attr;
	char *path = kmem_alloc(MAXPATHLEN, KM_SLEEP);
	int error = 0;
	boolean_t passed = B_FALSE;

#define	MP_CHECK(expr, why) do { \
	if (!(expr)) { \
		KT_FAIL(ctx, "%s, error=%d", why, error); \
		goto out; \
	} \
} while (0)
	MP_CHECK(ns_process_pid(ctx, &pid), "invalid process PID");
	zone = zone_find_by_name("lx-probe");
	MP_CHECK(zone != NULL && lx_lpid_lock(pid, zone, 0, &p, NULL) == 0,
	    "cannot find stopped process fixture");
	MP_CHECK(p->p_brand == &lx_brand && p->p_lwpcnt == 1 &&
	    p->p_tlist != NULL && p->p_tlist->t_state == TS_STOPPED &&
	    p->p_user.u_cdir != NULL &&
	    strcmp(p->p_user.u_comm, "mount_proc") == 0 &&
	    ptolxproc(p)->l_mntns == NULL &&
	    lx_kernel_restricted(p) == !reject,
	    "target is not the stopped root/cwd fixture");
	mutex_enter(&p->p_crlock);
	crhold(base = p->p_cred);
	mutex_exit(&p->p_crlock);
	root = p->p_user.u_rdir;
	if (root != NULL)
		VN_HOLD(root);
	VN_HOLD(cwd = p->p_user.u_cdir);
	filter = ptolxproc(p)->l_seccomp;
	lx_seccomp_hold(filter);
	mutex_exit(&p->p_lock);
	p = NULL;
	error = vnodetopath(root != NULL ? root : zone->zone_rootvp,
	    cwd, path, MAXPATHLEN, base);
	MP_CHECK(error == 0 && strcmp(path, "/test/mnt-proc/cwd") == 0,
	    "fixture cwd differs from the expected namespace path");
	lx_mntns_budget_get(ztolxzd(zone)->lxzd_mntns_budget,
	    &maximum, &used);
	MP_CHECK(maximum >= 2 && used == 0, "fixture quota is not idle");
	error = lx_mntns_view_snapshot(zone->zone_rootpath, CRED(), base,
	    ztolxzd(zone)->lxzd_mntns_budget, &view);
	MP_CHECK(error == 0, "cannot snapshot process root");
	error = lx_mntns_view_tmpfs(view, "/test/mnt-proc/covered", CRED());
	MP_CHECK(error == 0, "cannot create process-private mount");
	error = lx_mntns_view_lookup(view, "/test/mnt-proc/covered",
	    CRED(), &vp);
	MP_CHECK(error == 0, "cannot find private mount root");
	bzero(&attr, sizeof (attr));
	attr.va_mask = AT_UID | AT_GID;
	attr.va_uid = crgetuid(base);
	attr.va_gid = crgetgid(base);
	error = VOP_SETATTR(vp, &attr, 0, CRED(), NULL);
	MP_CHECK(error == 0, "cannot assign private filesystem ownership");
	VN_RELE(vp);
	vp = NULL;
	error = ns_make(view, "/test/mnt-proc/covered", "private", VREG, base);
	MP_CHECK(error == 0, "cannot create process-private file");
	error = lx_mntns_view_lookup(view, "/test/mnt-proc/source", base, &vp);
	MP_CHECK(error == 0, "cannot find read-only bind source");
	error = lx_mntns_view_bind(view, vp, "/test/mnt-proc/bind-target",
	    B_TRUE, CRED());
	MP_CHECK(error == 0, "cannot create process read-only bind");
	VN_RELE(vp);
	vp = NULL;
	error = lx_mntns_view_clone(view, CRED(), base, &copy);
	MP_CHECK(error == 0, "cannot clone real process mount topology");
	MP_CHECK(lx_mntns_view_capable(copy, base) ==
	    lx_cap_initial(base, LX_CAP_SYS_ADMIN) &&
	    !lx_mntns_view_capable(copy, CRED()),
	    "mount authority used native cleanup credential or another zone");
	lx_mntns_budget_get(ztolxzd(zone)->lxzd_mntns_budget,
	    &maximum, &used);
	MP_CHECK(used == 2, "process snapshot and clone lost their charges");
	lx_mntns_view_rele(view);
	view = copy;
	copy = NULL;
	error = lx_mntns_state_prepare(view, "/test/mnt-proc/source", base,
	    &state);
	MP_CHECK(error == ENOTDIR && state.lms_view == NULL &&
	    state.lms_root == NULL && state.lms_cwd == NULL,
	    "failed cwd preparation retained references");
	error = lx_mntns_state_prepare(view, "/test/mnt-proc/cwd", base,
	    &state);
	MP_CHECK(error == 0, "cannot prepare process root/cwd references");
	if (!reject) {
		error = lx_mntns_view_create(zone->zone_rootpath, CRED(), base,
		    NULL, &foreign);
		MP_CHECK(error == 0, "cannot create foreign-account view");
		error = lx_mntns_state_prepare(foreign, "/test/mnt-proc/cwd",
		    base, &wrong);
		MP_CHECK(error == 0, "cannot prepare foreign-account state");
		error = lx_mntns_view_create(zone->zone_rootpath, CRED(),
		    CRED(), ztolxzd(zone)->lxzd_mntns_budget, &foreign_owner);
		MP_CHECK(error == 0, "cannot create foreign-owner view");
		error = lx_mntns_state_prepare(foreign_owner,
		    "/test/mnt-proc/cwd", base, &wrong_owner);
		MP_CHECK(error == 0, "cannot prepare foreign-owner state");
	}
	MP_CHECK(lx_lpid_lock(pid, zone, 0, &p, NULL) == 0,
	    "process fixture disappeared during preparation");
	MP_CHECK(p->p_brand == &lx_brand && p->p_cred == base &&
	    ptolxproc(p)->l_seccomp == filter &&
	    strcmp(p->p_user.u_comm, "mount_proc") == 0,
	    "process identity changed during preparation");
	if (!reject) {
		error = lx_mntns_proc_swap(p, root, cwd, &wrong);
		MP_CHECK(error == EXDEV && ptolxproc(p)->l_mntns == NULL &&
		    p->p_user.u_rdir == root && p->p_user.u_cdir == cwd,
		    "foreign mount account changed process state");
		error = lx_mntns_proc_swap(p, root, cwd, &wrong_owner);
		MP_CHECK(error == EXDEV && ptolxproc(p)->l_mntns == NULL &&
		    p->p_user.u_rdir == root && p->p_user.u_cdir == cwd,
		    "foreign mount owner changed process state");
		error = lx_mntns_proc_swap(p, root, NULL, &state);
		MP_CHECK(error == EAGAIN && ptolxproc(p)->l_mntns == NULL &&
		    p->p_user.u_rdir == root && p->p_user.u_cdir == cwd,
		    "stale expected cwd changed process state");
	}
	error = lx_mntns_proc_swap(p, root, cwd, &state);
	if (reject) {
		MP_CHECK(error == EPERM && ptolxproc(p)->l_mntns == NULL &&
		    p->p_user.u_rdir == root && p->p_user.u_cdir == cwd,
		    "unguarded process acquired private root/cwd");
	} else {
		MP_CHECK(error == 0 && ptolxproc(p)->l_mntns == view &&
		    p->p_user.u_rdir != root && p->p_user.u_cdir != cwd &&
		    p->p_user.u_cwd == NULL,
		    "private root/cwd publication failed");
	}
	passed = B_TRUE;
out:
	if (p != NULL)
		mutex_exit(&p->p_lock);
	lx_mntns_state_fini(&state);
	lx_mntns_state_fini(&wrong);
	lx_mntns_state_fini(&wrong_owner);
	if (foreign_owner != NULL)
		lx_mntns_view_rele(foreign_owner);
	if (foreign != NULL)
		lx_mntns_view_rele(foreign);
	if (vp != NULL)
		VN_RELE(vp);
	if (view != NULL)
		lx_mntns_view_rele(view);
	if (copy != NULL)
		lx_mntns_view_rele(copy);
	if (root != NULL)
		VN_RELE(root);
	if (cwd != NULL)
		VN_RELE(cwd);
	lx_seccomp_rele(filter);
	if (base != NULL)
		crfree(base);
	if (zone != NULL)
		zone_rele(zone);
	kmem_free(path, MAXPATHLEN);
	if (reject && !ns_wait_idle()) {
		KT_ERROR(ctx, "rejected process entry leaked a mount view");
		passed = B_FALSE;
	}
	if (passed)
		KT_PASS(ctx);
#undef MP_CHECK
}

static void
ns_process_attach(ktest_ctx_hdl_t *ctx)
{
	ns_process_enter(ctx, B_FALSE);
}

static void
ns_process_reject(ktest_ctx_hdl_t *ctx)
{
	ns_process_enter(ctx, B_TRUE);
}

static void
ns_process_inspect(ktest_ctx_hdl_t *ctx, boolean_t zombie)
{
	zone_t *zone = NULL;
	proc_t *p = NULL;
	pid_t pid;
	lx_mntns_view_t *view = NULL;
	vnode_t *root = NULL, *cwd = NULL, *vp = NULL;
	cred_t *cr = NULL;
	int error = 0;
	boolean_t passed = B_FALSE;

#define	MP_CHECK(expr, why) do { \
	if (!(expr)) { \
		KT_FAIL(ctx, "%s, error=%d", why, error); \
		goto out; \
	} \
} while (0)
	MP_CHECK(ns_process_pid(ctx, &pid), "invalid inspection PID");
	zone = zone_find_by_name("lx-probe");
	MP_CHECK(zone != NULL && lx_lpid_lock(pid, zone,
	    zombie ? LXP_ZOMBOK : 0, &p, NULL) == 0,
	    "cannot find process to inspect");
	MP_CHECK(p->p_brand == &lx_brand &&
	    strcmp(p->p_user.u_comm, "mount_proc") == 0,
	    "inspection target is not the fixture");
	if (zombie) {
		MP_CHECK(p->p_stat == SZOMB && p->p_tlist == NULL &&
		    ptolxproc(p)->l_mntns == NULL,
		    "unreaped zombie retained its mount-view reference");
		passed = B_TRUE;
		goto out;
	}
	MP_CHECK(ptolxproc(p)->l_mntns != NULL && p->p_lwpcnt == 1 &&
	    p->p_tlist != NULL && p->p_tlist->t_state == TS_STOPPED &&
	    lx_kernel_restricted(p), "orphan lost its inherited mount view");
	view = ptolxproc(p)->l_mntns;
	lx_mntns_view_hold(view);
	VN_HOLD(root = p->p_user.u_rdir);
	VN_HOLD(cwd = p->p_user.u_cdir);
	crhold(cr = p->p_cred);
	mutex_exit(&p->p_lock);
	p = NULL;
	error = lx_mntns_view_lookup(view, "/", cr, &vp);
	MP_CHECK(error == 0 && vp == root, "orphan has a different root vnode");
	VN_RELE(vp);
	vp = NULL;
	error = lx_mntns_view_lookup(view, "/test/mnt-proc/cwd", cr, &vp);
	MP_CHECK(error == 0 && vp == cwd, "orphan has a different cwd vnode");
	VN_RELE(vp);
	vp = NULL;
	error = lx_mntns_view_destroy(view, CRED());
	if (error == 0)
		view = NULL;
	MP_CHECK(error == EBUSY, "orphan did not own its inherited reference");
	passed = B_TRUE;
out:
	if (p != NULL)
		mutex_exit(&p->p_lock);
	if (vp != NULL)
		VN_RELE(vp);
	if (root != NULL)
		VN_RELE(root);
	if (cwd != NULL)
		VN_RELE(cwd);
	if (view != NULL)
		lx_mntns_view_rele(view);
	if (cr != NULL)
		crfree(cr);
	if (zone != NULL)
		zone_rele(zone);
	if (passed)
		KT_PASS(ctx);
#undef MP_CHECK
}

static void
ns_process_member(ktest_ctx_hdl_t *ctx)
{
	ns_process_inspect(ctx, B_FALSE);
}

static void
ns_process_zombie(ktest_ctx_hdl_t *ctx)
{
	ns_process_inspect(ctx, B_TRUE);
}

static void
ns_process_idle(ktest_ctx_hdl_t *ctx)
{
	zone_t *zone;
	uint32_t maximum, used;

	if (!ns_wait_idle()) {
		KT_FAIL(ctx, "process exit leaked private mount views");
		return;
	}
	zone = zone_find_by_name("lx-probe");
	if (zone == NULL) {
		KT_ERROR(ctx, "cannot find process fixture zone");
		return;
	}
	lx_mntns_budget_get(ztolxzd(zone)->lxzd_mntns_budget, &maximum, &used);
	zone_rele(zone);
	if (used != 0) {
		KT_FAIL(ctx, "process exit leaked mount account charges");
		return;
	}
	KT_PASS(ctx);
}

/* A real stopped LX worker supplies the synthetic thread PID. */
static kmutex_t ns_pid_lock;
static struct pid *ns_pid_identity;
static pid_t ns_pid_number;

static void
ns_pid_drop(void)
{
	ASSERT(MUTEX_HELD(&ns_pid_lock));
	if (ns_pid_identity != NULL) {
		mutex_enter(&pidlock);
		/* Do not release an identity a regression already freed. */
		if (pid_find(ns_pid_number) == ns_pid_identity) {
			PID_RELE(ns_pid_identity);
		}
		mutex_exit(&pidlock);
		ns_pid_identity = NULL;
		ns_pid_number = 0;
	}
}

/* Native reservations must never resolve to proc slot zero. */
static void
ns_pid_reservation(ktest_ctx_hdl_t *ctx)
{
	struct pid *identity;
	pid_t number;
	boolean_t hidden;

	if ((number = pid_allocate(curproc, 0, 0)) < 0) {
		KT_ERROR(ctx, "cannot allocate native PID reservation");
		return;
	}
	mutex_enter(&pidlock);
	identity = pid_find(number);
	hidden = prfind_zone(number, ALL_ZONES) == NULL;
	PID_RELE(identity);
	mutex_exit(&pidlock);
	if (hidden)
		KT_PASS(ctx);
	else
		KT_FAIL(ctx, "native PID reservation resolved to a process");
}

static void
ns_pid_hold(ktest_ctx_hdl_t *ctx)
{
	zone_t *zone = NULL;
	struct pid *identity = NULL, *foreign;
	proc_t *p = NULL;
	kthread_t *t;
	pid_t number;
	boolean_t passed = B_FALSE;

	if (!ns_process_pid(ctx, &number)) {
		KT_ERROR(ctx, "invalid real worker TID");
		return;
	}
	zone = zone_find_by_name("lx-probe");
	if (zone == NULL || zone->zone_brand != &lx_brand ||
	    (identity = lx_pid_identity_hold(number, zone)) == NULL) {
		KT_FAIL(ctx, "worker has no held lx identity");
		goto out;
	}
	if (lx_lpid_lock(number, zone, 0, &p, &t) != 0) {
		KT_FAIL(ctx, "cannot find held worker");
		goto out;
	}
	if (t == NULL || p->p_pid == number || t->t_state != TS_STOPPED ||
	    strcmp(PTOU(p)->u_comm, "pid_identity") != 0) {
		KT_FAIL(ctx, "fixture is not a stopped synthetic worker PID");
		goto out;
	}
	mutex_exit(&p->p_lock);
	p = NULL;
	mutex_enter(&pidlock);
	passed = identity->pid_ref >= 2 && identity->pid_prinactive &&
	    prfind_zone(number, ALL_ZONES) == NULL;
	mutex_exit(&pidlock);
	if (!passed) {
		KT_FAIL(ctx, "synthetic thread ID became a native process");
		goto out;
	}
	passed = B_FALSE;
	foreign = lx_pid_identity_hold(number, global_zone);
	if (foreign != NULL) {
		lx_pid_identity_rele(foreign);
		KT_FAIL(ctx, "identity lookup crossed zone ownership");
		goto out;
	}
	mutex_enter(&ns_pid_lock);
	if (ns_pid_identity != NULL) {
		mutex_exit(&ns_pid_lock);
		KT_ERROR(ctx, "previous identity fixture still held");
		goto out;
	}
	ns_pid_identity = identity;
	ns_pid_number = number;
	identity = NULL;
	mutex_exit(&ns_pid_lock);
	passed = B_TRUE;
out:
	if (p != NULL)
		mutex_exit(&p->p_lock);
	if (identity != NULL)
		lx_pid_identity_rele(identity);
	if (zone != NULL)
		zone_rele(zone);
	if (passed)
		KT_PASS(ctx);
}

static void
ns_pid_retired(ktest_ctx_hdl_t *ctx)
{
	struct pid *actual;
	boolean_t retained = B_FALSE;
	uint_t attempt;

	mutex_enter(&ns_pid_lock);
	if (ns_pid_identity == NULL) {
		mutex_exit(&ns_pid_lock);
		KT_ERROR(ctx, "no held worker identity");
		return;
	}
	for (attempt = 0; attempt < 100; attempt++) {
		mutex_enter(&pidlock);
		actual = pid_find(ns_pid_number);
		retained = actual == ns_pid_identity && actual->pid_ref == 1 &&
		    actual->pid_prinactive &&
		    prfind_zone(ns_pid_number, ALL_ZONES) == NULL;
		mutex_exit(&pidlock);
		if (retained || actual == NULL)
			break;
		delay(MAX(1, hz / 10));
	}
	mutex_exit(&ns_pid_lock);
	if (retained)
		KT_PASS(ctx);
	else
		KT_FAIL(ctx,
		    "retired PID lost identity or remained a native task");
}

static void
ns_pid_release(ktest_ctx_hdl_t *ctx)
{
	pid_t number;
	boolean_t gone;

	mutex_enter(&ns_pid_lock);
	number = ns_pid_number;
	ns_pid_drop();
	mutex_enter(&pidlock);
	gone = number > 1 && pid_find(number) == NULL;
	mutex_exit(&pidlock);
	mutex_exit(&ns_pid_lock);
	if (gone)
		KT_PASS(ctx);
	else
		KT_FAIL(ctx,
		    "final reference did not release the PID reservation");
}

/* These maps describe stopped fixtures; they do not change their namespaces. */
static lx_pidns_map_t *ns_pid_maps[3];
static lx_pidns_t *ns_pid_spaces[3];
static zone_t *ns_pid_maps_zone;
static pid_t ns_pid_maps_worker;

static void
ns_pid_maps_drop(void)
{
	int i;

	ASSERT(MUTEX_HELD(&ns_pid_lock));
	/* Match freeproc: final map/namespace release may hold pidlock. */
	mutex_enter(&pidlock);
	for (i = 2; i >= 0; i--) {
		if (ns_pid_maps[i] != NULL)
			lx_pidns_map_rele(ns_pid_maps[i]);
		ns_pid_maps[i] = NULL;
		ns_pid_spaces[i] = NULL;
	}
	mutex_exit(&pidlock);
	if (ns_pid_maps_zone != NULL)
		zone_rele(ns_pid_maps_zone);
	ns_pid_maps_zone = NULL;
	ns_pid_maps_worker = 0;
}

static boolean_t
ns_pid_pair(ktest_ctx_hdl_t *ctx, pid_t *worker, pid_t *peer)
{
	uchar_t *input;
	size_t size, i;
	pid_t numbers[2] = { 0, 0 };
	uint_t at = 0;

	ktest_get_input(ctx, &input, &size);
	if (size < 4 || size > 22 || input[size - 1] != 0)
		return (B_FALSE);
	for (i = 0; i + 1 < size; i++) {
		if (input[i] == ' ' && at == 0 && numbers[0] > 1) {
			at = 1;
			continue;
		}
		if (input[i] < '0' || input[i] > '9' || numbers[at] >= 10000000)
			return (B_FALSE);
		numbers[at] = numbers[at] * 10 + input[i] - '0';
	}
	*worker = numbers[0];
	*peer = numbers[1];
	return (at == 1 && *peer > 1 && *worker != *peer);
}

static boolean_t
ns_pid_map_absent(lx_pidns_t *ns, pid_t number)
{
	struct pid *identity = lx_pidns_lookup(ns, number);

	if (identity == NULL)
		return (B_TRUE);
	lx_pid_identity_rele(identity);
	return (B_FALSE);
}

static void
ns_pid_maps_hold(ktest_ctx_hdl_t *ctx)
{
	lx_pidns_t *spaces[3] = { NULL, NULL, NULL }, *temp = NULL;
	lx_pidns_t *chain[LX_PIDNS_MAX_DEPTH] = { NULL };
	lx_pidns_map_t *maps[3] = { NULL, NULL, NULL };
	lx_pidns_map_t *pending = NULL, *extra = NULL;
	lx_userns_t *root = NULL, *user = NULL;
	lx_userns_cred_t actor;
	zone_t *zone = NULL;
	cred_t *base = NULL, *scope = NULL, *weak = NULL;
	proc_t *p = NULL;
	kthread_t *t;
	struct pid *identity = NULL, *original = NULL;
	pid_t worker, peer, mainpid, fresh;
	boolean_t token_gone;
	uint32_t maximum = 0, used, after;
	boolean_t changed = B_FALSE, passed = B_FALSE;
	int i;

#define	PM_CHECK(expr, why) do { \
	if (!(expr)) { \
		KT_FAIL(ctx, "%s", why); \
		goto out; \
	} \
} while (0)
	mutex_enter(&ns_pid_lock);
	PM_CHECK(ns_pid_identity == NULL && ns_pid_maps[0] == NULL &&
	    !lx_pidns_busy(), "another PID fixture is still held");
	PM_CHECK(ns_pid_pair(ctx, &worker, &peer), "invalid worker PID pair");
	zone = zone_find_by_name("lx-probe");
	PM_CHECK(zone != NULL && lx_lpid_lock(worker, zone, 0, &p, &t) == 0,
	    "cannot find primary worker");
	PM_CHECK(t != NULL && t->t_state == TS_STOPPED &&
	    p->p_pid != worker && strcmp(PTOU(p)->u_comm, "pid_identity") == 0,
	    "primary fixture is not a stopped worker");
	mainpid = p->p_pid;
	mutex_enter(&p->p_crlock);
	crhold(base = p->p_cred);
	mutex_exit(&p->p_crlock);
	mutex_exit(&p->p_lock);
	p = NULL;
	PM_CHECK(lx_lpid_lock(peer, zone, 0, &p, &t) == 0,
	    "cannot find peer worker");
	PM_CHECK(t != NULL && t->t_state == TS_STOPPED && p->p_pid != peer &&
	    p->p_pid != mainpid && strcmp(PTOU(p)->u_comm, "pid_identity") == 0,
	    "peer fixture is not a different stopped process");
	mutex_exit(&p->p_lock);
	p = NULL;
	root = ztolxzd(zone)->lxzd_userns;
	PM_CHECK(lx_userns_map_actor(base, root, &actor) == 0 &&
	    lx_userns_child_create(&actor, &user) == 0,
	    "cannot prepare private test user namespace");
	actor.luc_ns = user;
	lx_caps_new_namespace(&actor.luc_caps);
	scope = lx_userns_native_for(base, &actor);
	actor.luc_caps.lc_effective &= ~LX_CAP_BIT(LX_CAP_SYS_ADMIN);
	weak = lx_userns_native_for(base, &actor);
	lx_userns_pidns_budget_get(root, &maximum, &used);
	PM_CHECK(used == 0 && maximum >= LX_PIDNS_MAX_DEPTH + 4,
	    "PID namespace budget is not initially idle");
	PM_CHECK(lx_pidns_create(weak, NULL, 5, &temp) == EPERM && temp == NULL,
	    "missing owner capability was accepted");
	PM_CHECK(lx_pidns_create(scope, NULL, 5, &spaces[0]) == 0 &&
	    lx_pidns_reserve(spaces[0], &maps[0]) == 0 &&
	    lx_pidns_map_id(maps[0], spaces[0]) == 1,
	    "cannot reserve outer init ID");
	PM_CHECK(ns_pid_map_absent(spaces[0], 1),
	    "unpublished init reservation became visible");
	PM_CHECK(lx_pidns_reserve(spaces[0], &pending) == EAGAIN &&
	    pending == NULL, "second init reservation was accepted");
	PM_CHECK(lx_pidns_publish(maps[0], mainpid) == 0 &&
	    lx_pidns_create(scope, spaces[0], 3, &spaces[1]) == 0 &&
	    lx_pidns_reserve(spaces[1], &maps[1]) == 0 &&
	    lx_pidns_publish(maps[1], worker) == 0 &&
	    lx_pidns_create(scope, spaces[0], 3, &spaces[2]) == 0 &&
	    lx_pidns_reserve(spaces[2], &maps[2]) == 0 &&
	    lx_pidns_publish(maps[2], peer) == 0,
	    "cannot publish nested and sibling mappings");
	PM_CHECK(lx_pidns_visible(spaces[0], mainpid) == 1 &&
	    lx_pidns_visible(spaces[0], worker) == 2 &&
	    lx_pidns_visible(spaces[0], peer) == 3 &&
	    lx_pidns_visible(spaces[1], worker) == 1 &&
	    lx_pidns_visible(spaces[1], peer) == 0 &&
	    lx_pidns_visible(spaces[2], peer) == 1 &&
	    lx_pidns_visible(spaces[2], worker) == 0,
	    "ancestor or sibling PID views are wrong");
	PM_CHECK(lx_pidns_map_id(maps[1], spaces[0]) == 2 &&
	    lx_pidns_map_id(maps[1], spaces[1]) == 1 &&
	    lx_pidns_map_id(maps[1], spaces[2]) == 0,
	    "map IDs crossed a sibling namespace");
	identity = lx_pidns_lookup(spaces[1], 1);
	PM_CHECK(identity != NULL && identity->pid_id == worker,
	    "namespace lookup lost the native identity");
	lx_pid_identity_rele(identity);
	identity = NULL;
	PM_CHECK(lx_pidns_reserve(spaces[0], &pending) == 0 &&
	    lx_pidns_map_id(pending, spaces[0]) == 4 &&
	    lx_pidns_reserve(spaces[1], &extra) == EAGAIN && extra == NULL,
	    "ancestor exhaustion did not reject a partial reservation");
	lx_pidns_map_rele(pending);
	pending = NULL;
	PM_CHECK(lx_pidns_reserve(spaces[1], &pending) == 0 &&
	    lx_pidns_map_id(pending, spaces[1]) == 2 &&
	    lx_pidns_map_id(pending, spaces[0]) == 4,
	    "failed reservation leaked a descendant or ancestor ID");
	PM_CHECK(lx_pidns_publish(pending, curproc->p_pid) == ESRCH &&
	    lx_pidns_publish(pending, worker) == EEXIST &&
	    ns_pid_map_absent(spaces[0], 4) &&
	    ns_pid_map_absent(spaces[1], 2) &&
	    lx_pidns_publish(maps[1], worker) == EALREADY,
	    "failed or duplicate publication changed the visible maps");
	lx_pidns_map_rele(pending);
	pending = NULL;
	PM_CHECK(lx_pidns_create(CRED(), spaces[0], 5, &temp) == EINVAL &&
	    temp == NULL, "foreign-zone owner was accepted");
	lx_userns_pidns_budget_get(root, &maximum, &used);
	PM_CHECK(used == 3 && lx_userns_pidns_budget_set(root, used) == 0,
	    "PID namespace accounting is wrong");
	changed = B_TRUE;
	PM_CHECK(lx_pidns_create(scope, NULL, 5, &temp) == ENOSPC &&
	    temp == NULL,
	    "exhausted namespace budget allowed creation");
	VERIFY0(lx_userns_pidns_budget_set(root, maximum));
	changed = B_FALSE;
	/* Model native LWP preparation before any lx task hash entry exists. */
	PM_CHECK(lx_pidns_create(scope, NULL, 5, &temp) == 0 &&
	    lx_pidns_reserve(temp, &pending) == 0,
	    "cannot reserve native-binding init");
	PM_CHECK((fresh = pid_allocate(curproc, 0, 0)) > 1,
	    "cannot allocate unpublished native PID");
	mutex_enter(&pidlock);
	original = pid_find(fresh);
	mutex_exit(&pidlock);
	PM_CHECK(original != NULL, "native PID reservation disappeared");
	identity = lx_pid_identity_hold(fresh, zone);
	PM_CHECK(identity == NULL, "fresh native PID already has an lx task");
	PM_CHECK(lx_pidns_bind_native(pending, global_zone, original) ==
	    EXDEV &&
	    lx_pidns_bind_native(pending, zone, original) == 0 &&
	    ns_pid_map_absent(temp, 1) && lx_pidns_visible(temp, fresh) == 0 &&
	    lx_pidns_reserve(temp, &extra) == EAGAIN && extra == NULL,
	    "uncommitted native binding became visible or crossed zones");
	lx_pidns_map_rele(pending);
	pending = NULL;
	PM_CHECK(lx_pidns_reserve(temp, &pending) == 0 &&
	    lx_pidns_map_id(pending, temp) == 1 &&
	    lx_pidns_bind_native(pending, zone, original) == 0 &&
	    lx_pidns_commit(pending) && lx_pidns_visible(temp, fresh) == 1,
	    "failed native construction made PID1 impossible to retry");
	identity = lx_pidns_lookup(temp, 1);
	PM_CHECK(identity == original, "native commit changed its identity");
	lx_pid_identity_rele(identity);
	identity = NULL;
	lx_pidns_map_rele(pending);
	pending = NULL;
	PM_CHECK(lx_pidns_reserve(temp, &pending) == ENOMEM && pending == NULL,
	    "committed PID1 could be replaced");
	lx_pidns_rele(temp);
	temp = NULL;
	PM_CHECK(lx_pidns_create(scope, NULL, 5, &temp) == 0 &&
	    lx_pidns_reserve(temp, &pending) == 0 &&
	    lx_pidns_bind_native(pending, zone, original) == 0,
	    "cannot bind shutdown-race init");
	lx_pidns_shutdown(temp);
	PM_CHECK(!lx_pidns_commit(pending) &&
	    lx_pidns_visible(temp, fresh) == 1,
	    "namespace death before commit did not report a doomed child");
	identity = lx_pidns_lookup(temp, 1);
	PM_CHECK(identity == original,
	    "doomed child lost retained identity for ancestor wait");
	lx_pid_identity_rele(identity);
	identity = NULL;
	lx_pidns_map_rele(pending);
	pending = NULL;
	lx_pidns_rele(temp);
	temp = NULL;
	mutex_enter(&pidlock);
	PID_RELE(original);
	original = NULL;
	token_gone = pid_find(fresh) == NULL;
	mutex_exit(&pidlock);
	PM_CHECK(token_gone, "native binding retained the original PID token");
	PM_CHECK(lx_pidns_create(scope, NULL, 5, &temp) == 0 &&
	    lx_pidns_reserve(temp, &pending) == 0,
	    "cannot reserve rollback init");
	lx_pidns_map_rele(pending);
	pending = NULL;
	PM_CHECK(lx_pidns_reserve(temp, &pending) == 0 &&
	    lx_pidns_map_id(pending, temp) == 1,
	    "aborted init reservation could not be retried");
	lx_pidns_shutdown(temp);
	PM_CHECK(lx_pidns_publish(pending, worker) == ENOMEM,
	    "shutdown allowed a pending task to publish");
	lx_pidns_map_rele(pending);
	pending = NULL;
	PM_CHECK(lx_pidns_reserve(temp, &pending) == ENOMEM && pending == NULL,
	    "shutdown was undone by reservation cleanup");
	lx_pidns_rele(temp);
	temp = NULL;
	for (i = 0; i < LX_PIDNS_MAX_DEPTH; i++) {
		PM_CHECK(lx_pidns_create(scope, i == 0 ? NULL : chain[i - 1],
		    3, &chain[i]) == 0, "allowed namespace depth failed");
	}
	PM_CHECK(lx_pidns_create(scope, chain[LX_PIDNS_MAX_DEPTH - 1], 3,
	    &temp) == ENOSPC && temp == NULL, "namespace depth exceeded 32");
	for (i = LX_PIDNS_MAX_DEPTH - 1; i >= 0; i--) {
		lx_pidns_rele(chain[i]);
		chain[i] = NULL;
	}
	lx_userns_pidns_budget_get(root, &maximum, &after);
	PM_CHECK(after == used, "temporary namespaces leaked a budget charge");
	for (i = 0; i < 3; i++) {
		ns_pid_maps[i] = maps[i];
		ns_pid_spaces[i] = spaces[i];
		maps[i] = NULL;
		/* The map owns the namespace; test final release at reap. */
		lx_pidns_rele(spaces[i]);
		spaces[i] = NULL;
	}
	ns_pid_maps_zone = zone;
	zone = NULL;
	ns_pid_maps_worker = worker;
	passed = B_TRUE;
out:
	if (p != NULL)
		mutex_exit(&p->p_lock);
	if (identity != NULL)
		lx_pid_identity_rele(identity);
	if (changed)
		VERIFY0(lx_userns_pidns_budget_set(root, maximum));
	if (pending != NULL)
		lx_pidns_map_rele(pending);
	if (extra != NULL)
		lx_pidns_map_rele(extra);
	lx_pidns_rele(temp);
	if (original != NULL) {
		mutex_enter(&pidlock);
		PID_RELE(original);
		mutex_exit(&pidlock);
	}
	for (i = LX_PIDNS_MAX_DEPTH - 1; i >= 0; i--)
		lx_pidns_rele(chain[i]);
	for (i = 2; i >= 0; i--) {
		if (maps[i] != NULL)
			lx_pidns_map_rele(maps[i]);
		lx_pidns_rele(spaces[i]);
	}
	if (weak != NULL)
		crfree(weak);
	if (scope != NULL)
		crfree(scope);
	lx_userns_rele(user);
	if (base != NULL)
		crfree(base);
	if (zone != NULL)
		zone_rele(zone);
	mutex_exit(&ns_pid_lock);
	if (passed)
		KT_PASS(ctx);
#undef PM_CHECK
}

static void
ns_pid_maps_retired(ktest_ctx_hdl_t *ctx)
{
	struct pid *identity;
	proc_t *p = NULL;
	boolean_t retained = B_FALSE;
	uint_t attempt;
	lx_pidns_map_t *pending = NULL;

	mutex_enter(&ns_pid_lock);
	if (ns_pid_maps[1] == NULL) {
		KT_ERROR(ctx, "no PID map fixture held");
		goto out;
	}
	for (attempt = 0; attempt < 100; attempt++) {
		identity = lx_pidns_lookup(ns_pid_spaces[1], 1);
		if (identity == NULL)
			break;
		mutex_enter(&pidlock);
		retained = identity->pid_id == ns_pid_maps_worker &&
		    identity->pid_ref == 2 && identity->pid_prinactive &&
		    pid_find(ns_pid_maps_worker) == identity &&
		    prfind_zone(ns_pid_maps_worker, ALL_ZONES) == NULL;
		PID_RELE(identity);
		mutex_exit(&pidlock);
		if (retained)
			break;
		delay(MAX(1, hz / 10));
	}
	if (lx_lpid_lock(ns_pid_maps_worker, ns_pid_maps_zone, 0,
	    &p, NULL) == 0) {
		mutex_exit(&p->p_lock);
		retained = B_FALSE;
	}
	lx_pidns_shutdown(ns_pid_spaces[1]);
	if (lx_pidns_reserve(ns_pid_spaces[1], &pending) != ENOMEM) {
		if (pending != NULL)
			lx_pidns_map_rele(pending);
		retained = B_FALSE;
	}
	if (retained)
		KT_PASS(ctx);
	else
		KT_FAIL(ctx,
		    "retired namespace identity is lost or still a task");
out:
	mutex_exit(&ns_pid_lock);
}

static void
ns_pid_maps_release(ktest_ctx_hdl_t *ctx)
{
	pid_t worker;
	boolean_t gone;
	lx_userns_t *root = NULL;
	uint32_t maximum, used = 1;

	mutex_enter(&ns_pid_lock);
	worker = ns_pid_maps_worker;
	if (ns_pid_maps_zone != NULL) {
		root = ztolxzd(ns_pid_maps_zone)->lxzd_userns;
		lx_userns_hold(root);
	}
	ns_pid_maps_drop();
	if (root != NULL) {
		lx_userns_pidns_budget_get(root, &maximum, &used);
		lx_userns_rele(root);
	}
	mutex_enter(&pidlock);
	gone = worker > 1 && pid_find(worker) == NULL;
	mutex_exit(&pidlock);
	mutex_exit(&ns_pid_lock);
	if (gone && used == 0 && !lx_pidns_busy())
		KT_PASS(ctx);
	else
		KT_FAIL(ctx, "map release retained an identity or namespace");
}

/* Private bookkeeping attachment only; Linux PID syscall values stay global. */
static lx_pidns_t *ns_pid_process_space;
static zone_t *ns_pid_process_zone;
static lx_pidns_map_t *ns_pid_process_observer;
static lx_pidns_map_t *ns_pid_process_descendant_map;
static pid_t ns_pid_process_descendant_pid;
static pid_t ns_pid_process_owner, ns_pid_process_target;
static struct pid *ns_pcagent_observer;
static pid_t ns_pcagent_number;

static void
ns_pid_pcagent_live(ktest_ctx_hdl_t *ctx)
{
	proc_t *p = NULL;
	kthread_t *t;
	lx_lwp_data_t *ld;
	lx_pidns_map_t *map;
	struct pid *prepared = NULL;
	pid_t pid, number = 0, mapped;
	boolean_t passed = B_FALSE;

	mutex_enter(&ns_pid_lock);
	if (ns_pcagent_observer != NULL || !ns_process_pid(ctx, &pid) ||
	    ns_pid_process_zone == NULL || pid != ns_pid_process_owner ||
	    (p = sprlock_zone(pid, ns_pid_process_zone->zone_id)) == NULL ||
	    (t = p->p_agenttp) == NULL || (ld = ttolxlwp(t)) == NULL ||
	    (map = ld->br_pidns_map) == NULL || ld->br_lpid == NULL) {
		KT_FAIL(ctx, "missing live native /proc agent bookkeeping");
		goto out;
	}
	membar_consumer();
	lx_pcagent_lab_prepared(&prepared, &number);
	mapped = lx_pidns_map_id(map, ns_pid_process_space);
	if (prepared == NULL || number == 0 || prepared == p->p_pidp ||
	    ld->br_lpid->lxp_pidp != prepared || ld->br_pid != number ||
	    mapped == 0 || mapped != lx_pidns_visible(ns_pid_process_space,
	    number)) {
		KT_FAIL(ctx,
		    "agent did not retain its prepared mapped identity");
		goto out;
	}
	mutex_exit(&p->p_lock);
	mutex_enter(&pidlock);
	if (pid_find(number) != prepared) {
		mutex_exit(&pidlock);
		mutex_enter(&p->p_lock);
		KT_FAIL(ctx,
		    "prepared agent identity is absent from native hash");
		goto out;
	}
	PID_HOLD(prepared);
	mutex_exit(&pidlock);
	mutex_enter(&p->p_lock);
	ns_pcagent_observer = prepared;
	ns_pcagent_number = number;
	passed = B_TRUE;
out:
	if (p != NULL)
		sprunlock(p);
	mutex_exit(&ns_pid_lock);
	if (passed)
		KT_PASS(ctx);
}

static void
ns_pid_pcagent_dead(ktest_ctx_hdl_t *ctx)
{
	proc_t *p = NULL;
	struct pid *identity;
	pid_t pid, number;
	boolean_t passed = B_FALSE;

	mutex_enter(&ns_pid_lock);
	identity = ns_pcagent_observer;
	number = ns_pcagent_number;
	if (identity == NULL || !ns_process_pid(ctx, &pid) ||
	    ns_pid_process_zone == NULL || pid != ns_pid_process_owner ||
	    (p = sprlock_zone(pid, ns_pid_process_zone->zone_id)) == NULL ||
	    p->p_agenttp != NULL || p->p_lwpcnt != 1) {
		KT_FAIL(ctx, "native /proc agent did not leave cleanly");
		goto out;
	}
	sprunlock(p);
	p = NULL;
	mutex_enter(&pidlock);
	if (pid_find(number) != identity || !identity->pid_prinactive ||
	    identity->pid_ref != 1) {
		mutex_exit(&pidlock);
		KT_FAIL(ctx, "agent teardown did not leave only its observer");
		goto out;
	}
	PID_RELE(identity);
	if (pid_find(number) != NULL) {
		mutex_exit(&pidlock);
		KT_FAIL(ctx, "agent identity remained after final release");
		goto out;
	}
	mutex_exit(&pidlock);
	ns_pcagent_observer = NULL;
	ns_pcagent_number = 0;
	passed = B_TRUE;
out:
	if (p != NULL)
		sprunlock(p);
	mutex_exit(&ns_pid_lock);
	if (passed)
		KT_PASS(ctx);
}

static void
ns_pid_process_drop(void)
{
	ASSERT(MUTEX_HELD(&ns_pid_lock));
	if (ns_pid_process_observer != NULL)
		lx_pidns_map_rele(ns_pid_process_observer);
	if (ns_pid_process_descendant_map != NULL)
		lx_pidns_map_rele(ns_pid_process_descendant_map);
	ns_pid_process_descendant_map = NULL;
	ns_pid_process_descendant_pid = 0;
	lx_pidns_rele(ns_pid_process_space);
	if (ns_pid_process_zone != NULL)
		zone_rele(ns_pid_process_zone);
	ns_pid_process_observer = NULL;
	ns_pid_process_space = NULL;
	ns_pid_process_zone = NULL;
	ns_pid_process_owner = ns_pid_process_target = 0;
}

static boolean_t
ns_pid_process_childless(pid_t pid, zone_t *zone)
{
	proc_t *p;
	boolean_t childless;

	mutex_enter(&pidlock);
	p = prfind_zone(pid, zone->zone_id);
	childless = p != NULL && p->p_child == NULL;
	mutex_exit(&pidlock);
	return (childless);
}

static void
ns_pid_process_attach(ktest_ctx_hdl_t *ctx)
{
	zone_t *zone = NULL;
	proc_t *p = NULL;
	kthread_t *t;
	lx_lwp_data_t *ld;
	cred_t *base = NULL, *scope = NULL;
	lx_userns_t *user = NULL;
	lx_userns_cred_t actor;
	lx_pidns_t *space = NULL;
	lx_pidns_map_t *map = NULL;
	struct pid *identity = NULL;
	pid_t pid;
	boolean_t installed = B_FALSE, passed = B_FALSE;

#define	PO_CHECK(expr, why) do { \
	if (!(expr)) { \
		KT_FAIL(ctx, "%s", why); \
		goto out; \
	} \
} while (0)
	mutex_enter(&ns_pid_lock);
	PO_CHECK(ns_pid_process_space == NULL && !lx_pidns_busy() &&
	    ns_pid_identity == NULL, "another PID fixture is active");
	PO_CHECK(ns_process_pid(ctx, &pid), "invalid process fixture PID");
	zone = zone_find_by_name("lx-probe");
	PO_CHECK(zone != NULL && lx_lpid_lock(pid, zone, 0, &p, &t) == 0,
	    "cannot find stopped process fixture");
	ld = t == NULL ? NULL : ttolxlwp(t);
	PO_CHECK(p->p_brand == &lx_brand && p->p_lwpcnt == 1 &&
	    !(p->p_flag & (SEXITING | SEXITLWPS | SHOLDFORK | SHOLDFORK1)) &&
	    t != NULL && t->t_state == TS_STOPPED &&
	    (strcmp(PTOU(p)->u_comm, "pid_ownership") == 0 ||
	    strcmp(PTOU(p)->u_comm, "pid_orphan") == 0 ||
	    strcmp(PTOU(p)->u_comm, "pid_nested") == 0) &&
	    ld != NULL && ld->br_stack_mode == LX_STACK_MODE_BRAND &&
	    ld->br_syscall_entry == BRAND_ENTRY_AMD64 &&
	    ptolxproc(p)->l_ptrace == 0 && ptolxproc(p)->l_pidns_map == NULL &&
	    ptolxproc(p)->l_pidns_children == NULL && ld->br_pidns_map == NULL,
	    "fixture is not a sole Linux-mode LWP without PID bookkeeping");
	mutex_enter(&p->p_crlock);
	crhold(base = p->p_cred);
	mutex_exit(&p->p_crlock);
	mutex_exit(&p->p_lock);
	p = NULL;
	PO_CHECK(ns_pid_process_childless(pid, zone),
	    "PID1 fixture must initially have no native children");
	PO_CHECK(lx_userns_map_actor(base, ztolxzd(zone)->lxzd_userns,
	    &actor) == 0 && lx_userns_child_create(&actor, &user) == 0,
	    "cannot prepare immutable test namespace owner");
	actor.luc_ns = user;
	lx_caps_new_namespace(&actor.luc_caps);
	scope = lx_userns_native_for(base, &actor);
	identity = lx_pid_identity_hold(pid, zone);
	PO_CHECK(identity != NULL && lx_pidns_create(scope, NULL,
	    LX_PIDNS_PID_LIMIT, &space) == 0 &&
	    lx_pidns_reserve(space, &map) == 0 &&
	    lx_pidns_bind_native(map, zone, identity) == 0,
	    "cannot prepare the initial process PID map");
	PO_CHECK(lx_lpid_lock(pid, zone, 0, &p, &t) == 0,
	    "process disappeared during preparation");
	ld = t == NULL ? NULL : ttolxlwp(t);
	PO_CHECK(p->p_lwpcnt == 1 && p->p_cred == base &&
	    t != NULL && t->t_state == TS_STOPPED &&
	    ld != NULL && ld->br_stack_mode == LX_STACK_MODE_BRAND &&
	    ld->br_syscall_entry == BRAND_ENTRY_AMD64 &&
	    ptolxproc(p)->l_pidns_map == NULL && ld->br_pidns_map == NULL,
	    "process state changed during preparation");
	ptolxproc(p)->l_pidns_map = map;
	lx_pidns_map_hold(map);
	ld->br_pidns_map = map;
	lx_pidns_map_hold(map);
	installed = B_TRUE;
	mutex_exit(&p->p_lock);
	p = NULL;
	PO_CHECK(lx_pidns_commit(map), "namespace died during test attachment");
	ns_pid_process_space = space;
	space = NULL;
	ns_pid_process_zone = zone;
	zone = NULL;
	ns_pid_process_owner = pid;
	passed = B_TRUE;
out:
	if (p != NULL)
		mutex_exit(&p->p_lock);
	if (!passed && installed) {
		mutex_enter(&pidlock);
		if ((p = prfind_zone(pid, zone->zone_id)) != NULL)
			psignal(p, SIGKILL);
		mutex_exit(&pidlock);
	}
	if (identity != NULL)
		lx_pid_identity_rele(identity);
	if (map != NULL)
		lx_pidns_map_rele(map);
	lx_pidns_rele(space);
	if (scope != NULL)
		crfree(scope);
	lx_userns_rele(user);
	if (base != NULL)
		crfree(base);
	if (zone != NULL)
		zone_rele(zone);
	mutex_exit(&ns_pid_lock);
	if (passed)
		KT_PASS(ctx);
#undef PO_CHECK
}

static void
ns_pid_process_observe(ktest_ctx_hdl_t *ctx, uint_t kind)
{
	proc_t *p = NULL;
	kthread_t *t;
	lx_pidns_map_t *map = NULL;
	pid_t target, number = kind == 2 ? 3 : 2;
	boolean_t child = kind != 0, descendant = kind == 2;
	boolean_t passed = B_FALSE;
	lx_pidns_map_t **observer = descendant ?
	    &ns_pid_process_descendant_map : &ns_pid_process_observer;

	mutex_enter(&ns_pid_lock);
	if (ns_pid_process_space == NULL || *observer != NULL ||
	    (descendant && ns_pid_process_observer == NULL) ||
	    !ns_process_pid(ctx, &target) ||
	    lx_lpid_lock(target, ns_pid_process_zone, 0, &p, &t) != 0) {
		KT_ERROR(ctx,
		    "missing process fixture or invalid observed target");
		goto out;
	}
	if (t == NULL || t->t_state != TS_STOPPED ||
	    p->p_brand != &lx_brand || !lx_kernel_restricted(p) ||
	    lx_pidns_active(p) != ns_pid_process_space ||
	    ptolxproc(p)->l_pidns_children != NULL ||
	    (strcmp(PTOU(p)->u_comm, "pid_ownership") != 0 &&
	    strcmp(PTOU(p)->u_comm, "pid_orphan") != 0)) {
		KT_FAIL(ctx,
		    "observed process did not inherit private bookkeeping");
		goto out;
	}
	map = child ? ptolxproc(p)->l_pidns_map : ttolxlwp(t)->br_pidns_map;
	if ((child && (p->p_ppid != (descendant ? ns_pid_process_target :
	    ns_pid_process_owner) ||
	    p->p_pid != target || ttolxlwp(t)->br_pidns_map != map)) ||
	    (!child && (p->p_pid != ns_pid_process_owner ||
	    target == p->p_pid ||
	    ttolxlwp(t)->br_pidns_map == ptolxproc(p)->l_pidns_map)) ||
	    map == NULL ||
	    lx_pidns_map_id(map, ns_pid_process_space) != number ||
	    (descendant && ptolxproc(p)->l_parent_deathsig != 0)) {
		KT_FAIL(ctx, "fork or thread did not receive the next PID map");
		goto out;
	}
	lx_pidns_map_hold(map);
	*observer = map;
	if (descendant)
		ns_pid_process_descendant_pid = target;
	else
		ns_pid_process_target = target;
	mutex_exit(&p->p_lock);
	p = NULL;
	if (lx_pidns_visible(ns_pid_process_space, target) != number) {
		KT_FAIL(ctx, "native success did not commit the observed PID");
		goto out;
	}
	if (descendant) {
		if (lx_lpid_lock(ns_pid_process_target, ns_pid_process_zone,
		    0, &p, &t) != 0 ||
		    ptolxproc(p)->l_parent_deathsig != 0) {
			KT_FAIL(ctx, "cohort child has a parent-death signal");
			goto out;
		}
		mutex_exit(&p->p_lock);
		p = NULL;
	}
	passed = B_TRUE;
out:
	if (p != NULL)
		mutex_exit(&p->p_lock);
	mutex_exit(&ns_pid_lock);
	if (passed)
		KT_PASS(ctx);
}

static void
ns_pid_process_thread(ktest_ctx_hdl_t *ctx)
{
	ns_pid_process_observe(ctx, 0);
}

static void
ns_pid_process_child(ktest_ctx_hdl_t *ctx)
{
	ns_pid_process_observe(ctx, 1);
}

static void
ns_pid_process_descendant(ktest_ctx_hdl_t *ctx)
{
	ns_pid_process_observe(ctx, 2);
}

/* Native reap may finish after the exiting PID1 has queued cohort signals. */
static void
ns_pid_process_dead(ktest_ctx_hdl_t *ctx)
{
	uint_t attempt;
	boolean_t passed = B_FALSE;
	lx_pidns_map_t *reserved = NULL;

	mutex_enter(&ns_pid_lock);
	if (ns_pid_process_observer == NULL ||
	    ns_pid_process_descendant_map == NULL) {
		KT_ERROR(ctx, "no observed cohort");
		goto out;
	}
	for (attempt = 0; attempt < 100; attempt++) {
		struct pid *child = lx_pidns_lookup(ns_pid_process_space, 2);
		struct pid *descendant =
		    lx_pidns_lookup(ns_pid_process_space, 3);

		mutex_enter(&pidlock);
		passed = child != NULL && descendant != NULL &&
		    child->pid_prinactive && descendant->pid_prinactive &&
		    child->pid_ref == 2 && descendant->pid_ref == 2 &&
		    prfind_zone(ns_pid_process_owner, ALL_ZONES) == NULL &&
		    prfind_zone(ns_pid_process_target, ALL_ZONES) == NULL &&
		    prfind_zone(ns_pid_process_descendant_pid,
		    ALL_ZONES) == NULL;
		if (child != NULL) {
			PID_RELE(child);
		}
		if (descendant != NULL) {
			PID_RELE(descendant);
		}
		mutex_exit(&pidlock);
		if (passed)
			break;
		delay(MAX(1, hz / 10));
	}
	if (passed && lx_pidns_reserve(ns_pid_process_space, &reserved) ==
	    ENOMEM) {
		KT_PASS(ctx);
	} else {
		KT_FAIL(ctx, "PID1 did not kill the cohort and gate new tasks");
	}
out:
	if (reserved != NULL)
		lx_pidns_map_rele(reserved);
	mutex_exit(&ns_pid_lock);
}

/* Run after ancestor wait; held maps must not defer native reap. */
static void
ns_pid_process_drained(ktest_ctx_hdl_t *ctx)
{
	boolean_t passed = B_FALSE;
	lx_pidns_map_t *reserved = NULL;

	mutex_enter(&ns_pid_lock);
	if (ns_pid_process_observer == NULL ||
	    ns_pid_process_descendant_map == NULL) {
		KT_ERROR(ctx, "no observed cohort");
		goto out;
	}
	{
		struct pid *child = lx_pidns_lookup(ns_pid_process_space, 2);
		struct pid *descendant =
		    lx_pidns_lookup(ns_pid_process_space, 3);

		mutex_enter(&pidlock);
		passed = child != NULL && descendant != NULL &&
		    child->pid_prinactive && descendant->pid_prinactive &&
		    child->pid_ref == 2 && descendant->pid_ref == 2 &&
		    prfind_zone(ns_pid_process_owner, ALL_ZONES) == NULL &&
		    prfind_zone(ns_pid_process_target, ALL_ZONES) == NULL &&
		    prfind_zone(ns_pid_process_descendant_pid,
		    ALL_ZONES) == NULL;
		if (child != NULL) {
			PID_RELE(child);
		}
		if (descendant != NULL) {
			PID_RELE(descendant);
		}
		mutex_exit(&pidlock);
	}
	if (passed && lx_pidns_reserve(ns_pid_process_space, &reserved) ==
	    ENOMEM) {
		KT_PASS(ctx);
	} else {
		KT_FAIL(ctx, "ancestor wait returned before cohort reap");
	}
out:
	if (reserved != NULL)
		lx_pidns_map_rele(reserved);
	mutex_exit(&ns_pid_lock);
}

static void
ns_pid_process_zombie_check(ktest_ctx_hdl_t *ctx, boolean_t orphan)
{
	proc_t *p = NULL;
	boolean_t passed = B_FALSE;
	uint_t attempt;
	lx_pidns_map_t *map;
	pid_t target;

	mutex_enter(&ns_pid_lock);
	map = orphan ? ns_pid_process_descendant_map : ns_pid_process_observer;
	target = orphan ? ns_pid_process_descendant_pid : ns_pid_process_target;
	if (map == NULL) {
		KT_ERROR(ctx, "no child map held");
		goto out;
	}
	for (attempt = 0; attempt < 100; attempt++) {
		if (lx_lpid_lock(target, ns_pid_process_zone,
		    LXP_ZOMBOK, &p, NULL) == 0) {
			passed = p->p_stat == SZOMB &&
			    ptolxproc(p)->l_pidns_map == map &&
			    lx_pidns_active(p) == ns_pid_process_space;
			mutex_exit(&p->p_lock);
			p = NULL;
		}
		if (passed)
			break;
		delay(MAX(1, hz / 10));
	}
	if (passed)
		KT_PASS(ctx);
	else
		KT_FAIL(ctx,
		    "child did not retain its PID map as a native zombie");
out:
	mutex_exit(&ns_pid_lock);
}

static void
ns_pid_process_zombie(ktest_ctx_hdl_t *ctx)
{
	ns_pid_process_zombie_check(ctx, B_FALSE);
}

static void
ns_pid_orphan_zombie(ktest_ctx_hdl_t *ctx)
{
	ns_pid_process_zombie_check(ctx, B_TRUE);
}

static void
ns_pid_orphan_adopted(ktest_ctx_hdl_t *ctx)
{
	proc_t *owner, *orphan;
	boolean_t passed = B_FALSE;

	mutex_enter(&ns_pid_lock);
	mutex_enter(&pidlock);
	owner = prfind_zone(ns_pid_process_owner, ALL_ZONES);
	orphan = prfind_zone(ns_pid_process_descendant_pid, ALL_ZONES);
	if (owner != NULL && orphan != NULL &&
	    ns_pid_process_descendant_map != NULL) {
		mutex_enter(&orphan->p_lock);
		passed = orphan->p_brand == &lx_brand &&
		    ptolxproc(orphan) != NULL &&
		    ptolxproc(orphan)->l_pidns_map ==
		    ns_pid_process_descendant_map &&
		    orphan->p_parent == owner &&
		    orphan->p_ppid == owner->p_pid &&
		    orphan->p_sessp == owner->p_sessp &&
		    orphan->p_pgidp != owner->p_pgidp &&
		    !orphan->p_pgidp->pid_pgorphaned &&
		    orphan->p_tlist != NULL &&
		    orphan->p_tlist->t_state == TS_STOPPED;
		mutex_exit(&orphan->p_lock);
	}
	mutex_exit(&pidlock);
	mutex_exit(&ns_pid_lock);
	if (passed)
		KT_PASS(ctx);
	else
		KT_FAIL(ctx,
		    "stopped orphan lost its namespace init or group link");
}

static void
ns_pid_orphan_zombie_adopted(ktest_ctx_hdl_t *ctx)
{
	proc_t *owner, *orphan;
	boolean_t passed = B_FALSE;

	mutex_enter(&ns_pid_lock);
	mutex_enter(&pidlock);
	owner = prfind_zone(ns_pid_process_owner, ALL_ZONES);
	orphan = prfind_zone(ns_pid_process_descendant_pid, ALL_ZONES);
	if (owner != NULL && orphan != NULL &&
	    ns_pid_process_descendant_map != NULL) {
		mutex_enter(&orphan->p_lock);
		passed = orphan->p_brand == &lx_brand &&
		    ptolxproc(orphan) != NULL &&
		    ptolxproc(orphan)->l_pidns_map ==
		    ns_pid_process_descendant_map &&
		    orphan->p_parent == owner &&
		    orphan->p_ppid == owner->p_pid &&
		    orphan->p_sessp == owner->p_sessp &&
		    orphan->p_pgidp != owner->p_pgidp &&
		    !orphan->p_pgidp->pid_pgorphaned &&
		    orphan->p_stat == SZOMB;
		mutex_exit(&orphan->p_lock);
	}
	mutex_exit(&pidlock);
	mutex_exit(&ns_pid_lock);
	if (passed)
		KT_PASS(ctx);
	else
		KT_FAIL(ctx,
		    "zombie orphan lost its namespace init or group link");
}

static void
ns_pid_orphan_retired(ktest_ctx_hdl_t *ctx)
{
	uint_t attempt;
	boolean_t passed = B_FALSE;

	mutex_enter(&ns_pid_lock);
	if (ns_pid_process_descendant_map == NULL) {
		KT_ERROR(ctx, "no observed orphan map");
		goto out;
	}
	for (attempt = 0; attempt < 100; attempt++) {
		struct pid *identity = lx_pidns_lookup(ns_pid_process_space, 3);

		if (identity == NULL)
			break;
		mutex_enter(&pidlock);
		passed = identity->pid_prinactive && identity->pid_ref == 2 &&
		    identity->pid_id == ns_pid_process_descendant_pid &&
		    prfind_zone(ns_pid_process_descendant_pid,
		    ALL_ZONES) == NULL;
		PID_RELE(identity);
		mutex_exit(&pidlock);
		if (passed)
			break;
		delay(MAX(1, hz / 10));
	}
	if (passed)
		KT_PASS(ctx);
	else
		KT_FAIL(ctx,
		    "adopted orphan was not reaped with held identity");
out:
	mutex_exit(&ns_pid_lock);
}

static void
ns_pid_process_retired(ktest_ctx_hdl_t *ctx)
{
	struct pid *identity;
	proc_t *p = NULL;
	kthread_t *t;
	boolean_t passed = B_FALSE, retired = B_FALSE;
	uint_t attempt;

	mutex_enter(&ns_pid_lock);
	if (ns_pid_process_observer == NULL) {
		KT_ERROR(ctx, "no observed PID map held");
		goto out;
	}
	for (attempt = 0; attempt < 100; attempt++) {
		identity = lx_pidns_lookup(ns_pid_process_space, 2);
		if (identity == NULL)
			break;
		mutex_enter(&pidlock);
		retired = identity->pid_prinactive && identity->pid_ref == 2 &&
		    identity->pid_id == ns_pid_process_target &&
		    prfind_zone(ns_pid_process_target, ALL_ZONES) == NULL;
		PID_RELE(identity);
		mutex_exit(&pidlock);
		if (retired)
			break;
		delay(MAX(1, hz / 10));
	}
	if (!retired || lx_lpid_lock(ns_pid_process_owner, ns_pid_process_zone,
	    0, &p, &t) != 0) {
		KT_FAIL(ctx,
		    "retired child/thread lost identity or owner exited");
		goto out;
	}
	passed = t != NULL && t->t_state == TS_STOPPED &&
	    ttolxlwp(t)->br_pidns_map == ptolxproc(p)->l_pidns_map &&
	    lx_pidns_active(p) == ns_pid_process_space &&
	    lx_pidns_map_id(ptolxproc(p)->l_pidns_map,
	    ns_pid_process_space) == 1;
	mutex_exit(&p->p_lock);
	p = NULL;
	if (passed)
		KT_PASS(ctx);
	else
		KT_FAIL(ctx,
		    "exec or join did not preserve the process identity map");
out:
	mutex_exit(&ns_pid_lock);
}

static void
ns_pid_process_release(ktest_ctx_hdl_t *ctx)
{
	pid_t target, descendant;
	boolean_t gone;

	mutex_enter(&ns_pid_lock);
	target = ns_pid_process_target;
	descendant = ns_pid_process_descendant_pid;
	ns_pid_process_drop();
	mutex_enter(&pidlock);
	gone = target > 1 && pid_find(target) == NULL &&
	    (descendant == 0 || pid_find(descendant) == NULL);
	mutex_exit(&pidlock);
	mutex_exit(&ns_pid_lock);
	if (gone)
		KT_PASS(ctx);
	else
		KT_FAIL(ctx, "observed PID outlived its last map reference");
}

static void
ns_pid_process_idle(ktest_ctx_hdl_t *ctx)
{
	zone_t *zone = zone_find_by_name("lx-probe");
	uint32_t maximum, used;

	if (zone == NULL) {
		KT_ERROR(ctx, "no lx process fixture zone");
		return;
	}
	lx_userns_pidns_budget_get(ztolxzd(zone)->lxzd_userns, &maximum, &used);
	zone_rele(zone);
	if (used == 0 && !lx_pidns_busy())
		KT_PASS(ctx);
	else
		KT_FAIL(ctx,
		    "native reap retained a PID namespace or budget charge");
}

/*
 * Private nested drain fixture. Never replace an inherited published map.
 * ns_pid_lock serializes test state; process pointers never outlive p_lock.
 */
static lx_pidns_t *ns_nested_space;
static lx_pidns_map_t *ns_nested_maps[4];
static pid_t ns_nested_pids[4];

static void
ns_nested_drop(void)
{
	uint_t i;

	ASSERT(MUTEX_HELD(&ns_pid_lock));
	for (i = 0; i < 4; i++) {
		if (ns_nested_maps[i] != NULL)
			lx_pidns_map_rele(ns_nested_maps[i]);
		ns_nested_maps[i] = NULL;
		ns_nested_pids[i] = 0;
	}
	lx_pidns_rele(ns_nested_space);
	ns_nested_space = NULL;
}

static void
ns_nested_arm(ktest_ctx_hdl_t *ctx)
{
	proc_t *p = NULL;
	kthread_t *t;
	cred_t *base = NULL, *scope = NULL;
	lx_userns_t *user = NULL;
	lx_userns_cred_t actor;
	lx_pidns_t *space = NULL;
	lx_pidns_map_t *map = NULL;
	pid_t pid;
	boolean_t passed = B_FALSE;

#define	ND_CHECK(expr, why) do { \
	if (!(expr)) { \
		KT_FAIL(ctx, "%s", why); \
		goto out; \
	} \
} while (0)
	mutex_enter(&ns_pid_lock);
	ND_CHECK(ns_pid_process_space != NULL && ns_nested_space == NULL &&
	    ns_pid_process_observer == NULL &&
	    ns_pid_process_descendant_map == NULL &&
	    ns_process_pid(ctx, &pid), "invalid nested fixture state");
	ND_CHECK(lx_lpid_lock(pid, ns_pid_process_zone, 0, &p, &t) == 0,
	    "missing nested helper");
	ND_CHECK(p->p_lwpcnt == 1 && t != NULL &&
	    t->t_state == TS_STOPPED && p->p_brand == &lx_brand &&
	    strcmp(PTOU(p)->u_comm, "pid_nested") == 0 &&
	    !(p->p_flag & (SEXITING | SEXITLWPS | SHOLDFORK | SHOLDFORK1)) &&
	    p->p_ppid == ns_pid_process_owner && ptolxproc(p)->l_ptrace == 0 &&
	    lx_kernel_restricted(p) &&
	    lx_pidns_active(p) == ns_pid_process_space &&
	    ptolxproc(p)->l_pidns_children == NULL &&
	    ttolxlwp(t)->br_clone_pidns_map == NULL &&
	    ttolxlwp(t)->br_clone_pidns_error == 0 &&
	    ttolxlwp(t)->br_clone_cred == NULL &&
	    ttolxlwp(t)->br_clone_mntns == NULL &&
	    ttolxlwp(t)->br_pidns_map == ptolxproc(p)->l_pidns_map &&
	    ttolxlwp(t)->br_stack_mode == LX_STACK_MODE_BRAND &&
	    ttolxlwp(t)->br_syscall_entry == BRAND_ENTRY_AMD64 &&
	    lx_pidns_map_id(ptolxproc(p)->l_pidns_map,
	    ns_pid_process_space) == 2, "helper is not stopped outer child");
	map = ptolxproc(p)->l_pidns_map;
	lx_pidns_map_hold(map);
	mutex_enter(&p->p_crlock);
	crhold(base = p->p_cred);
	mutex_exit(&p->p_crlock);
	mutex_exit(&p->p_lock);
	p = NULL;
	ND_CHECK(ns_pid_process_childless(pid, ns_pid_process_zone),
	    "helper already has children");
	ND_CHECK(lx_userns_map_actor(base,
	    ztolxzd(ns_pid_process_zone)->lxzd_userns, &actor) == 0 &&
	    lx_userns_child_create(&actor, &user) == 0,
	    "cannot prepare nested owner");
	actor.luc_ns = user;
	lx_caps_new_namespace(&actor.luc_caps);
	scope = lx_userns_native_for(base, &actor);
	ND_CHECK(lx_pidns_create(scope, ns_pid_process_space,
	    LX_PIDNS_PID_LIMIT, &space) == 0, "cannot create nested space");
	ND_CHECK(lx_lpid_lock(pid, ns_pid_process_zone, 0, &p, &t) == 0,
	    "helper disappeared");
	ND_CHECK(p->p_lwpcnt == 1 && t != NULL &&
	    t->t_state == TS_STOPPED && p->p_cred == base &&
	    !(p->p_flag & (SEXITING | SEXITLWPS | SHOLDFORK | SHOLDFORK1)) &&
	    ptolxproc(p)->l_pidns_map == map &&
	    ptolxproc(p)->l_pidns_children == NULL &&
	    ttolxlwp(t)->br_clone_pidns_map == NULL &&
	    ttolxlwp(t)->br_clone_pidns_error == 0 &&
	    ttolxlwp(t)->br_clone_cred == NULL &&
	    ttolxlwp(t)->br_clone_mntns == NULL,
	    "helper changed during preparation");
	/* Process owns one hold; test owns the creation hold until release. */
	lx_pidns_hold(space);
	ptolxproc(p)->l_pidns_children = space;
	ns_nested_space = space;
	space = NULL;
	ns_nested_maps[0] = map;
	map = NULL;
	ns_nested_pids[0] = pid;
	passed = B_TRUE;
out:
	if (p != NULL)
		mutex_exit(&p->p_lock);
	if (map != NULL)
		lx_pidns_map_rele(map);
	lx_pidns_rele(space);
	if (scope != NULL)
		crfree(scope);
	lx_userns_rele(user);
	if (base != NULL)
		crfree(base);
	mutex_exit(&ns_pid_lock);
	if (passed)
		KT_PASS(ctx);
#undef ND_CHECK
}

static void
ns_nested_observe(ktest_ctx_hdl_t *ctx, uint_t slot)
{
	proc_t *p = NULL;
	kthread_t *t;
	lx_pidns_map_t *map;
	pid_t pid, inner = slot == 1 ? 1 : slot + 1;
	pid_t outer = slot == 1 ? 3 : slot + 3;
	boolean_t passed = B_FALSE;

	mutex_enter(&ns_pid_lock);
	if (ns_nested_space == NULL || ns_nested_maps[slot] != NULL ||
	    !ns_process_pid(ctx, &pid) ||
	    lx_lpid_lock(pid, ns_pid_process_zone,
	    slot == 3 ? LXP_ZOMBOK : 0, &p, &t) != 0) {
		KT_ERROR(ctx, "missing nested observation target");
		goto out;
	}
	if (p->p_brand != &lx_brand) {
		KT_FAIL(ctx, "nested target is not lx");
		goto out;
	}
	map = ptolxproc(p)->l_pidns_map;
	if (p->p_pid != pid ||
	    ptolxproc(p)->l_parent_deathsig != 0 ||
	    strcmp(PTOU(p)->u_comm, "pid_nested") != 0 ||
	    lx_pidns_active(p) != ns_nested_space || map == NULL ||
	    ptolxproc(p)->l_pidns_children != NULL ||
	    p->p_ppid != ns_nested_pids[slot == 1 ? 0 : 1] ||
	    lx_pidns_map_id(map, ns_nested_space) != inner ||
	    lx_pidns_map_id(map, ns_pid_process_space) != outer ||
	    (slot == 3 ? p->p_stat != SZOMB :
	    (t == NULL || t->t_state != TS_STOPPED ||
	    ttolxlwp(t)->br_pidns_map != map))) {
		KT_FAIL(ctx,
		    "nested maps, adoption or stop/zombie state differ");
		goto out;
	}
	lx_pidns_map_hold(map);
	ns_nested_maps[slot] = map;
	ns_nested_pids[slot] = pid;
	passed = B_TRUE;
out:
	if (p != NULL)
		mutex_exit(&p->p_lock);
	mutex_exit(&ns_pid_lock);
	if (passed)
		KT_PASS(ctx);
}

static void
ns_nested_inner(ktest_ctx_hdl_t *ctx)
{
	ns_nested_observe(ctx, 1);
}

static void
ns_nested_live(ktest_ctx_hdl_t *ctx)
{
	ns_nested_observe(ctx, 2);
}

static void
ns_nested_zombie(ktest_ctx_hdl_t *ctx)
{
	ns_nested_observe(ctx, 3);
}

/* Lookup holds only an identity. A held retired map must not delay wait. */
static boolean_t
ns_nested_retired(uint_t slot)
{
	struct pid *id;
	boolean_t passed;
	pid_t inner = slot == 1 ? 1 : slot + 1;
	pid_t outer = slot == 0 ? 2 : slot == 1 ? 3 : slot + 3;

	id = lx_pidns_lookup(ns_pid_process_space, outer);
	mutex_enter(&pidlock);
	passed = id != NULL && id->pid_id == ns_nested_pids[slot] &&
	    id->pid_prinactive && id->pid_ref == 2 &&
	    prfind_zone(ns_nested_pids[slot], ALL_ZONES) == NULL;
	if (id != NULL) {
		PID_RELE(id);
	}
	mutex_exit(&pidlock);
	if (!passed || slot == 0)
		return (passed);
	id = lx_pidns_lookup(ns_nested_space, inner);
	mutex_enter(&pidlock);
	passed = id != NULL && id->pid_id == ns_nested_pids[slot] &&
	    id->pid_prinactive && id->pid_ref == 2;
	if (id != NULL) {
		PID_RELE(id);
	}
	mutex_exit(&pidlock);
	return (passed);
}

static void
ns_nested_check(ktest_ctx_hdl_t *ctx, boolean_t final)
{
	uint_t i;
	proc_t *p;
	boolean_t passed = B_TRUE;
	lx_pidns_map_t *pending = NULL;

	mutex_enter(&ns_pid_lock);
	for (i = 0; i < 4; i++) {
		if (ns_nested_maps[i] == NULL)
			passed = B_FALSE;
	}
	if (!passed) {
		KT_ERROR(ctx, "incomplete nested observations");
		goto out;
	}
	for (i = final ? 0 : 1; i < 4; i++)
		passed = ns_nested_retired(i) && passed;
	mutex_enter(&pidlock);
	p = prfind_zone(ns_pid_process_owner, ALL_ZONES);
	passed = passed && (final ? p == NULL :
	    p != NULL && p->p_stat == SRUN);
	p = prfind_zone(ns_nested_pids[0], ALL_ZONES);
	passed = passed && (final ? p == NULL :
	    p != NULL && p->p_stat == SRUN);
	mutex_exit(&pidlock);
	passed = (lx_pidns_reserve(ns_nested_space, &pending) == ENOMEM) &&
	    passed;
	if (pending != NULL) {
		lx_pidns_map_rele(pending);
		pending = NULL;
	}
	if (final)
		passed = (lx_pidns_reserve(ns_pid_process_space, &pending) ==
		    ENOMEM) && passed;
	if (passed)
		KT_PASS(ctx);
	else
		KT_FAIL(ctx, "wait returned before nested native reap");
out:
	if (pending != NULL)
		lx_pidns_map_rele(pending);
	mutex_exit(&ns_pid_lock);
}

static void
ns_nested_inner_drained(ktest_ctx_hdl_t *ctx)
{
	ns_nested_check(ctx, B_FALSE);
}

static void
ns_nested_drained(ktest_ctx_hdl_t *ctx)
{
	ns_nested_check(ctx, B_TRUE);
}

static void
ns_nested_release(ktest_ctx_hdl_t *ctx)
{
	uint_t i;
	pid_t pids[4];
	boolean_t gone = B_TRUE;

	mutex_enter(&ns_pid_lock);
	bcopy(ns_nested_pids, pids, sizeof (pids));
	ns_nested_drop();
	ns_pid_process_drop();
	mutex_enter(&pidlock);
	for (i = 0; i < 4; i++)
		gone = pids[i] > 1 && pid_find(pids[i]) == NULL && gone;
	mutex_exit(&pidlock);
	mutex_exit(&ns_pid_lock);
	if (gone)
		KT_PASS(ctx);
	else
		KT_FAIL(ctx, "nested release retained native identities");
}

static struct modlmisc ns_modlmisc = {
	.misc_modops = &mod_miscops,
	.misc_linkinfo = "lx namespace tests"
};
static struct modlinkage ns_modlinkage = {
	.ml_rev = MODREV_1,
	.ml_linkage = { &ns_modlmisc, NULL }
};

int
_init(void)
{
	ktest_module_hdl_t *km = NULL;
	ktest_suite_hdl_t *ks = NULL;
	int error;

	mutex_init(&ns_pid_lock, NULL, MUTEX_DEFAULT, NULL);
	if ((error = ktest_create_module("lx_namespace", &km)) != 0) {
		mutex_destroy(&ns_pid_lock);
		return (error);
	}
	if ((error = ktest_add_suite(km, "mount", &ks)) != 0 ||
	    (error = ktest_add_test(ks, "views", ns_views,
	    KTEST_FLAG_INPUT)) != 0 ||
	    (error = ktest_add_test(ks, "reinsert", ns_reinsert,
	    KTEST_FLAG_INPUT)) != 0 ||
	    (error = ktest_add_test(ks, "snapshot", ns_snapshot,
	    KTEST_FLAG_INPUT)) != 0 ||
	    (error = ktest_add_test(ks, "clone", ns_clone,
	    KTEST_FLAG_INPUT)) != 0 ||
	    (error = ktest_add_test(ks, "lifetime", ns_lifetime,
	    KTEST_FLAG_INPUT)) != 0 ||
	    (error = ktest_add_test(ks, "quota", ns_quota,
	    KTEST_FLAG_INPUT)) != 0 ||
	    (error = ktest_add_test(ks, "owner", ns_owner,
	    KTEST_FLAG_INPUT)) != 0 ||
	    (error = ktest_add_test(ks, "zone_quota", ns_zone_quota,
	    0)) != 0 ||
	    (error = ktest_add_test(ks, "entry_mount", ns_entry_mount,
	    KTEST_FLAG_INPUT)) != 0 ||
	    (error = ktest_add_test(ks, "entry_user_mount", ns_entry_user_mount,
	    KTEST_FLAG_INPUT)) != 0 ||
	    (error = ktest_add_test(ks, "entry_reject", ns_entry_reject,
	    KTEST_FLAG_INPUT)) != 0 ||
	    (error = ktest_add_test(ks, "entry_user_only", ns_entry_user_only,
	    KTEST_FLAG_INPUT)) != 0 ||
	    (error = ktest_add_test(ks, "process_attach", ns_process_attach,
	    KTEST_FLAG_INPUT)) != 0 ||
	    (error = ktest_add_test(ks, "process_reject", ns_process_reject,
	    KTEST_FLAG_INPUT)) != 0 ||
	    (error = ktest_add_test(ks, "process_member", ns_process_member,
	    KTEST_FLAG_INPUT)) != 0 ||
	    (error = ktest_add_test(ks, "process_zombie", ns_process_zombie,
	    KTEST_FLAG_INPUT)) != 0 ||
	    (error = ktest_add_test(ks, "process_idle", ns_process_idle,
	    0)) != 0 ||
	    (error = ktest_add_suite(km, "user", &ks)) != 0 ||
	    (error = ktest_add_test(ks, "maps", lx_userns_test_maps, 0)) != 0 ||
	    (error = ktest_add_test(ks, "capabilities", lx_userns_test_caps,
	    0)) != 0 ||
	    (error = ktest_add_test(ks, "affinity", lx_userns_test_affinity,
	    0)) != 0 ||
	    (error = ktest_add_test(ks, "native", lx_userns_test_native,
	    0)) != 0 ||
	    (error = ktest_add_test(ks, "map_authority",
	    lx_userns_test_map_authority, 0)) != 0 ||
	    (error = ktest_add_test(ks, "lifetime", lx_userns_test_lifetime,
	    0)) != 0 ||
	    (error = ktest_add_test(ks, "budget", lx_userns_test_budget,
	    0)) != 0 ||
	    (error = ktest_add_test(ks, "budget_race",
	    lx_userns_test_budget_race, 0)) != 0 ||
	    (error = ktest_add_test(ks, "zone_budget_idle",
	    lx_userns_test_zone_budget_idle, 0)) != 0 ||
	    (error = ktest_add_test(ks, "zone_budget_disabled",
	    lx_userns_test_zone_budget_disabled, 0)) != 0 ||
	    (error = ktest_add_test(ks, "zone_budget_hold",
	    lx_userns_test_zone_budget_hold, 0)) != 0 ||
	    (error = ktest_add_test(ks, "credentials",
	    lx_brand_cred_test_copies, 0)) != 0 ||
	    (error = ktest_add_test(ks, "access", lx_brand_cred_test_access,
	    0)) != 0 ||
	    (error = ktest_add_test(ks, "proc_maps", lx_proc_maps_test,
	    0)) != 0 ||
	    (error = ktest_add_test(ks, "cap_syscalls", lx_cap_native_test,
	    0)) != 0 ||
	    (error = ktest_add_test(ks, "scope_attach",
	    lx_userns_test_scope_attach, KTEST_FLAG_INPUT)) != 0 ||
	    (error = ktest_add_test(ks, "scope_sids",
	    lx_userns_test_scope_sids, KTEST_FLAG_INPUT)) != 0 ||
	    (error = ktest_add_suite(km, "pid", &ks)) != 0 ||
	    (error = ktest_add_test(ks, "reservation", ns_pid_reservation,
	    0)) != 0 ||
	    (error = ktest_add_test(ks, "hold", ns_pid_hold,
	    KTEST_FLAG_INPUT)) != 0 ||
	    (error = ktest_add_test(ks, "retired", ns_pid_retired, 0)) != 0 ||
	    (error = ktest_add_test(ks, "release", ns_pid_release, 0)) != 0 ||
	    (error = ktest_add_test(ks, "maps_hold", ns_pid_maps_hold,
	    KTEST_FLAG_INPUT)) != 0 ||
	    (error = ktest_add_test(ks, "maps_retired", ns_pid_maps_retired,
	    0)) != 0 ||
	    (error = ktest_add_test(ks, "maps_release", ns_pid_maps_release,
	    0)) != 0 ||
	    (error = ktest_add_test(ks, "nested_arm",
	    ns_nested_arm, KTEST_FLAG_INPUT)) != 0 ||
	    (error = ktest_add_test(ks, "nested_inner",
	    ns_nested_inner, KTEST_FLAG_INPUT)) != 0 ||
	    (error = ktest_add_test(ks, "nested_live",
	    ns_nested_live, KTEST_FLAG_INPUT)) != 0 ||
	    (error = ktest_add_test(ks, "nested_zombie",
	    ns_nested_zombie, KTEST_FLAG_INPUT)) != 0 ||
	    (error = ktest_add_test(ks, "nested_inner_drained",
	    ns_nested_inner_drained, 0)) != 0 ||
	    (error = ktest_add_test(ks, "nested_drained",
	    ns_nested_drained, 0)) != 0 ||
	    (error = ktest_add_test(ks, "nested_release",
	    ns_nested_release, 0)) != 0 ||
	    (error = ktest_add_test(ks, "process_attach", ns_pid_process_attach,
	    KTEST_FLAG_INPUT)) != 0 ||
	    (error = ktest_add_test(ks, "pcagent_live", ns_pid_pcagent_live,
	    KTEST_FLAG_INPUT)) != 0 ||
	    (error = ktest_add_test(ks, "pcagent_dead", ns_pid_pcagent_dead,
	    KTEST_FLAG_INPUT)) != 0 ||
	    (error = ktest_add_test(ks, "process_thread", ns_pid_process_thread,
	    KTEST_FLAG_INPUT)) != 0 ||
	    (error = ktest_add_test(ks, "process_child", ns_pid_process_child,
	    KTEST_FLAG_INPUT)) != 0 ||
	    (error = ktest_add_test(ks, "process_descendant",
	    ns_pid_process_descendant, KTEST_FLAG_INPUT)) != 0 ||
	    (error = ktest_add_test(ks, "process_drained",
	    ns_pid_process_drained, 0)) != 0 ||
	    (error = ktest_add_test(ks, "process_dead", ns_pid_process_dead,
	    0)) != 0 ||
	    (error = ktest_add_test(ks, "process_zombie", ns_pid_process_zombie,
	    0)) != 0 ||
	    (error = ktest_add_test(ks, "orphan_adopted", ns_pid_orphan_adopted,
	    0)) != 0 ||
	    (error = ktest_add_test(ks, "orphan_zombie_adopted",
	    ns_pid_orphan_zombie_adopted, 0)) != 0 ||
	    (error = ktest_add_test(ks, "orphan_zombie", ns_pid_orphan_zombie,
	    0)) != 0 ||
	    (error = ktest_add_test(ks, "orphan_retired", ns_pid_orphan_retired,
	    0)) != 0 ||
	    (error = ktest_add_test(ks, "process_retired",
	    ns_pid_process_retired, 0)) != 0 ||
	    (error = ktest_add_test(ks, "process_release",
	    ns_pid_process_release, 0)) != 0 ||
	    (error = ktest_add_test(ks, "process_idle", ns_pid_process_idle,
	    0)) != 0 ||
	    (error = ktest_register_module(km)) != 0) {
		ktest_free_module(km);
		mutex_destroy(&ns_pid_lock);
		return (error);
	}
	if ((error = mod_install(&ns_modlinkage)) != 0) {
		ktest_unregister_module("lx_namespace");
		mutex_destroy(&ns_pid_lock);
	}
	return (error);
}

int
_fini(void)
{
	int error = mod_remove(&ns_modlinkage);

	if (error == 0) {
		ktest_unregister_module("lx_namespace");
		lx_userns_test_scope_sids_fini();
		mutex_enter(&ns_pid_lock);
		ns_pid_drop();
		ns_pid_maps_drop();
		ns_nested_drop();
		ns_pid_process_drop();
		mutex_exit(&ns_pid_lock);
		mutex_destroy(&ns_pid_lock);
	}
	return (error);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&ns_modlinkage, modinfop));
}
