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
#include <sys/lx_mntns.h>

extern void lx_userns_test_maps(ktest_ctx_hdl_t *);
extern void lx_userns_test_caps(ktest_ctx_hdl_t *);
extern void lx_userns_test_native(ktest_ctx_hdl_t *);
extern void lx_userns_test_lifetime(ktest_ctx_hdl_t *);
extern void lx_userns_test_budget(ktest_ctx_hdl_t *);
extern void lx_userns_test_budget_race(ktest_ctx_hdl_t *);
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
	error = lx_mntns_view_create((char *)input, cr, &a);
	NS_CHECK(error == 0, "cannot create detached view A");
	error = lx_mntns_view_create((char *)input, cr, &b);
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
	error = lx_mntns_view_snapshot((char *)input, cr, &a);
	NS_CHECK(error == 0, "cannot snapshot source into A");
	error = lx_mntns_view_snapshot((char *)input, cr, &b);
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
	    (error = ktest_add_suite(km, "user", &ks)) != 0 ||
	    (error = ktest_add_test(ks, "maps", lx_userns_test_maps, 0)) != 0 ||
	    (error = ktest_add_test(ks, "capabilities", lx_userns_test_caps,
	    0)) != 0 ||
	    (error = ktest_add_test(ks, "native", lx_userns_test_native,
	    0)) != 0 ||
	    (error = ktest_add_test(ks, "lifetime", lx_userns_test_lifetime,
	    0)) != 0 ||
	    (error = ktest_add_test(ks, "budget", lx_userns_test_budget,
	    0)) != 0 ||
	    (error = ktest_add_test(ks, "budget_race",
	    lx_userns_test_budget_race, 0)) != 0 ||
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
