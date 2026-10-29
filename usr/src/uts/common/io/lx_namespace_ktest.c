/*
 * CDDL version 1.0, http://www.illumos.org/license/CDDL.
 */
#include <sys/ktest.h>
#include <sys/errno.h>
#include <sys/pathname.h>
#include <sys/vnode.h>
#include <sys/lx_mntns.h>

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

	if (error == 0)
		ktest_unregister_module("lx_namespace");
	return (error);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&ns_modlinkage, modinfop));
}
