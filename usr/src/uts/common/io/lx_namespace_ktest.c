/*
 * CDDL version 1.0, http://www.illumos.org/license/CDDL.
 */
#include <sys/ktest.h>
#include <sys/kmem.h>
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
#include <sys/lx_mntns.h>
#include <sys/lx_userns.h>

extern brand_t lx_brand;

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
	error = lx_mntns_view_create((char *)input, cr, NULL, &a);
	NS_CHECK(error == 0, "cannot create detached view A");
	error = lx_mntns_view_create((char *)input, cr, NULL, &b);
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
	error = lx_mntns_view_snapshot((char *)input, cr, NULL, &a);
	NS_CHECK(error == 0, "cannot snapshot source into A");
	error = lx_mntns_view_snapshot((char *)input, cr, NULL, &b);
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
	error = lx_mntns_view_snapshot((char *)input, cr, NULL, &a);
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
	error = lx_mntns_view_clone(a, cr, &b);
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
	error = lx_mntns_view_clone(b, cr, &c);
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
	error = lx_mntns_view_create((char *)input, cr, NULL, &a);
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
	error = lx_mntns_view_clone(a, cr, &b);
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
	error = lx_mntns_view_create((char *)input, CRED(), NULL, &a);
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
	error = lx_mntns_view_create((char *)input, CRED(), budget, &a);
	NS_CHECK(error == 0, "cannot create charged view");
	lx_mntns_budget_get(budget, &maximum, &used);
	NS_CHECK(maximum == 1 && used == 1, "new view was not charged");
	error = lx_mntns_view_create((char *)input, CRED(), budget, &c);
	NS_CHECK(error == ENOSPC && c == NULL, "create bypassed quota");
	error = lx_mntns_view_snapshot((char *)input, CRED(), budget, &c);
	NS_CHECK(error == ENOSPC && c == NULL, "snapshot bypassed quota");
	error = lx_mntns_view_clone(a, CRED(), &c);
	NS_CHECK(error == ENOSPC && c == NULL, "clone bypassed source quota");
	lx_mntns_view_hold(a);
	lx_mntns_view_rele(a);
	lx_mntns_budget_get(budget, &maximum, &used);
	NS_CHECK(used == 1, "sharing a view changed its charge");
	lx_mntns_budget_set(budget, 3);
	reduced = lx_userns_native_cred(CRED());
	error = lx_mntns_view_create((char *)input, reduced, budget, &c);
	NS_CHECK(error == EPERM && c == NULL, "native mount denial changed");
	error = lx_mntns_view_snapshot((char *)input, reduced, budget, &c);
	NS_CHECK(error == EPERM && c == NULL,
	    "snapshot gained native authority");
	error = lx_mntns_view_clone(a, reduced, &c);
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
	error = lx_mntns_view_clone(a, CRED(), &b);
	NS_CHECK(error == 0, "cannot create charged clone");
	lx_mntns_view_rele(a);
	a = NULL;
	delay(hz * 2);
	lx_mntns_budget_get(budget, &maximum, &used);
	NS_CHECK(used == 2, "retired pinned source returned quota early");
	lx_mntns_budget_set(budget, 1);
	error = lx_mntns_view_clone(b, CRED(), &c);
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
	error = lx_mntns_view_create((char *)input, CRED(), budget, &c);
	NS_CHECK(error == ENOSPC && c == NULL, "zero quota admitted a view");
	VN_RELE(held);
	held = NULL;
	NS_CHECK(ns_wait_idle(), "quota source did not retire");
	lx_mntns_budget_get(budget, &maximum, &used);
	NS_CHECK(maximum == 0 && used == 0, "retirement leaked a charge");
	lx_mntns_budget_set(budget, 1);
	error = lx_mntns_view_snapshot((char *)input, CRED(), budget, &a);
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
	error = lx_mntns_view_create("/zones/lx-ns-kernel/src", CRED(),
	    budget, &a);
	if (maximum == 0) {
		NS_CHECK(error == ENOSPC && a == NULL,
		    "zero zone mount quota admitted a view");
	} else {
		NS_CHECK(error == 0, "cannot create zone-charged view");
		error = lx_mntns_view_clone(a, CRED(), &b);
		NS_CHECK(error == 0, "cannot clone zone-charged view");
		/* Native controller observes used=2, then lowers maximum to 1. */
		for (i = 0; i < 100; i++) {
			lx_mntns_budget_get(budget, &maximum, &used);
			if (maximum == 1)
				break;
			delay(MAX(1, hz / 10));
		}
		NS_CHECK(maximum == 1 && used == 2,
		    "native controller did not lower the live quota");
		error = lx_mntns_view_clone(b, CRED(), &c);
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
ns_process_enter(ktest_ctx_hdl_t *ctx, boolean_t reject)
{
	zone_t *zone = NULL;
	proc_t *p = NULL;
	pid_t pid;
	cred_t *base = NULL;
	struct lx_seccomp_filter *filter = NULL;
	vnode_t *root = NULL, *cwd = NULL, *vp = NULL;
	lx_mntns_view_t *view = NULL, *copy = NULL, *foreign = NULL;
	lx_mntns_state_t state = { 0 }, wrong = { 0 };
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
	error = lx_mntns_view_snapshot(zone->zone_rootpath, CRED(),
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
	error = lx_mntns_view_clone(view, CRED(), &copy);
	MP_CHECK(error == 0, "cannot clone real process mount topology");
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
		error = lx_mntns_view_create(zone->zone_rootpath, CRED(), NULL,
		    &foreign);
		MP_CHECK(error == 0, "cannot create foreign-account view");
		error = lx_mntns_state_prepare(foreign, "/test/mnt-proc/cwd",
		    base, &wrong);
		MP_CHECK(error == 0, "cannot prepare foreign-account state");
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

	if ((error = ktest_create_module("lx_namespace", &km)) != 0)
		return (error);
	if ((error = ktest_add_suite(km, "mount", &ks)) != 0 ||
	    (error = ktest_add_test(ks, "views", ns_views,
	    KTEST_FLAG_INPUT)) != 0 ||
	    (error = ktest_add_test(ks, "snapshot", ns_snapshot,
	    KTEST_FLAG_INPUT)) != 0 ||
	    (error = ktest_add_test(ks, "clone", ns_clone,
	    KTEST_FLAG_INPUT)) != 0 ||
	    (error = ktest_add_test(ks, "lifetime", ns_lifetime,
	    KTEST_FLAG_INPUT)) != 0 ||
	    (error = ktest_add_test(ks, "quota", ns_quota,
	    KTEST_FLAG_INPUT)) != 0 ||
	    (error = ktest_add_test(ks, "zone_quota", ns_zone_quota,
	    0)) != 0 ||
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
	    (error = ktest_register_module(km)) != 0) {
		ktest_free_module(km);
		return (error);
	}
	if ((error = mod_install(&ns_modlinkage)) != 0)
		ktest_unregister_module("lx_namespace");
	return (error);
}

int
_fini(void)
{
	int error = mod_remove(&ns_modlinkage);

	if (error == 0) {
		ktest_unregister_module("lx_namespace");
		lx_userns_test_scope_sids_fini();
	}
	return (error);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&ns_modlinkage, modinfop));
}
