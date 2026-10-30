/*
 * CDDL version 1.0, http://www.illumos.org/license/CDDL.
 */
#include <sys/ktest.h>
#include <sys/lx_brand.h>
#include "lx_proc.h"

#define	PM_CHECK(expr, why) do { \
	if (!(expr)) { \
		KT_FAIL(ctx, "%s", why); \
		goto out; \
	} \
} while (0)

static int
pm_open(char *name, int flags, cred_t *cred, vnode_t **out)
{
	cred_t *saved = CRED();
	int error;

	crhold(cred);
	curthread->t_cred = cred;
	error = vn_open(name, UIO_SYSSPACE, flags, 0, out, CRCREAT, 0);
	curthread->t_cred = saved;
	crfree(cred);
	return (error);
}

static void
pm_close(vnode_t *vp)
{
	if (vp != NULL) {
		(void) VOP_CLOSE(vp, FREAD | FWRITE, 1, 0, CRED(), NULL);
		VN_RELE(vp);
	}
}

static int
pm_write(vnode_t *vp, cred_t *opener, cred_t *writer, char *text,
    size_t length, offset_t offset, enum uio_seg segment, boolean_t consumed)
{
	iovec_t iov = { text, length };
	uio_t uio = { 0 };
	cred_t *saved = CRED();
	int error;

	uio.uio_iov = &iov;
	uio.uio_iovcnt = 1;
	uio.uio_resid = length;
	uio.uio_loffset = offset;
	uio.uio_segflg = segment;
	crhold(writer);
	curthread->t_cred = writer;
	error = VOP_WRITE(vp, &uio, 0, opener, NULL);
	curthread->t_cred = saved;
	crfree(writer);
	if (uio.uio_resid != (consumed ? 0 : length) ||
	    uio.uio_loffset != offset + (consumed ? length : 0))
		return (EIO);
	return (error);
}

static int
pm_read(vnode_t *vp, cred_t *opener, char *text, size_t size)
{
	iovec_t iov = { text, size - 1 };
	uio_t uio = { 0 };
	int error;

	uio.uio_iov = &iov;
	uio.uio_iovcnt = 1;
	uio.uio_resid = size - 1;
	uio.uio_segflg = UIO_SYSSPACE;
	error = VOP_READ(vp, &uio, 0, opener, NULL);
	text[size - 1 - uio.uio_resid] = '\0';
	return (error);
}

/*
 * Substitute a namespace only on a private, already-open vnode. Public
 * namespace entry is still disabled. This tests the real VOP paths without
 * publishing an incompletely confined credential to a running process.
 */
static void
pm_target(vnode_t *vp, lx_userns_t *ns)
{
	lxpr_node_t *node = VTOLXP(vp);
	lx_userns_t *old = node->lxpr_userns;

	lx_userns_hold(ns);
	node->lxpr_userns = ns;
	lx_userns_rele(old);
}

void
lx_proc_maps_test(ktest_ctx_hdl_t *ctx)
{
	char uidpath[] = "/zones/lx-probe-zfs/root/proc/1/uid_map";
	char gidpath[] = "/zones/lx-probe-zfs/root/proc/1/gid_map";
	char groupspath[] = "/zones/lx-probe-zfs/root/proc/1/setgroups";
	vnode_t *uid = NULL, *second = NULL, *gid = NULL, *groups = NULL;
	lx_userns_t *root = lx_userns_root_create(), *ns = NULL, *other = NULL;
	lx_userns_cred_t creator = { 0 }, actor;
	cred_t *full = NULL, *weak = NULL, *reader = NULL;
	cred_t *basic = lx_userns_native_cred(CRED());
	char text[256];
	uint_t inside, outside, count;
	boolean_t passed = B_FALSE;

	PM_CHECK(pm_open(uidpath, FREAD | FWRITE, CRED(), &uid) == 0 &&
	    pm_open(uidpath, FREAD | FWRITE, CRED(), &second) == 0,
	    "open real uid_map fixtures");
	PM_CHECK(uid != second && VTOLXP(uid)->lxpr_userns != NULL &&
	    VTOLXP(uid)->lxpr_userns == VTOLXP(second)->lxpr_userns,
	    "map opens did not capture independent vnodes for one namespace");
	PM_CHECK(pm_write(uid, CRED(), CRED(), NULL, 5, 0,
	    UIO_USERSPACE, B_FALSE) == EPERM,
	    "initial namespace write copied data before rejecting the scope");
	PM_CHECK(pm_open(groupspath, FREAD | FWRITE, basic, &groups) == EACCES,
	    "setgroups writable open ignored initial native authority");
	PM_CHECK(pm_open(groupspath, FREAD | FWRITE, CRED(), &groups) == 0 &&
	    pm_open(gidpath, FREAD | FWRITE, CRED(), &gid) == 0,
	    "open real gid_map/setgroups fixtures");
	PM_CHECK(pm_read(uid, CRED(), text, sizeof (text)) == 0 &&
	    sscanf(text, "%u %u %u", &inside, &outside, &count) == 3 &&
	    inside == 0 && outside == 0 && count == UINT32_MAX,
	    "initial map was not the initial namespace identity map");

	creator.luc_ns = root;
	creator.luc_uid = crgetuid(CRED());
	creator.luc_gid = crgetgid(CRED());
	lx_caps_new_namespace(&creator.luc_caps);
	PM_CHECK(lx_userns_child_create(&creator, &ns) == 0 &&
	    lx_userns_child_create(&creator, &other) == 0,
	    "create private map namespaces");
	actor = creator;
	actor.luc_ns = ns;
	full = lx_userns_native_for(CRED(), &actor);
	actor.luc_caps.lc_effective = 0;
	weak = lx_userns_native_for(CRED(), &actor);
	actor.luc_ns = other;
	reader = lx_userns_native_for(CRED(), &actor);
	pm_target(uid, ns);
	pm_target(gid, ns);
	pm_target(groups, ns);
	PM_CHECK(pm_read(uid, full, text, sizeof (text)) == 0 && text[0] == 0,
	    "unpublished map was not empty");
	PM_CHECK(pm_write(uid, weak, full, "0 0 1", 5, 0,
	    UIO_SYSSPACE, B_FALSE) == EPERM,
	    "current privilege gain replaced the opener authority");
	PM_CHECK(pm_write(uid, full, weak, NULL, 5, 0,
	    UIO_USERSPACE, B_FALSE) == EFAULT,
	    "bad map pointer was not rejected without consuming data");
	PM_CHECK(pm_write(uid, full, weak, "0 0 1", 5, 0,
	    UIO_SYSSPACE, B_TRUE) == 0,
	    "captured strong opener failed after current privilege drop");
	PM_CHECK(pm_write(uid, full, weak, "0 0 1", 5, 5,
	    UIO_SYSSPACE, B_FALSE) == EINVAL &&
	    pm_write(uid, full, weak, "0 0 1", 5, 0,
	    UIO_SYSSPACE, B_FALSE) == EPERM,
	    "successful map write was not offset-bound and write-once");
	PM_CHECK(pm_read(uid, reader, text, sizeof (text)) == 0 &&
	    sscanf(text, "%u %u %u", &inside, &outside, &count) == 3 &&
	    outside == UINT32_MAX,
	    "map read ignored the file credential's namespace");
	PM_CHECK(pm_write(groups, full, weak, "deny", 4, 0,
	    UIO_SYSSPACE, B_TRUE) == 0 &&
	    pm_read(groups, weak, text, sizeof (text)) == 0 &&
	    strcmp(text, "deny\n") == 0,
	    "setgroups write used current authority or did not persist");
	PM_CHECK(pm_write(gid, full, weak, "0 0 1", 5, 0,
	    UIO_SYSSPACE, B_TRUE) == 0,
	    "GID map did not use captured authority and deny-setgroups state");
	PM_CHECK(pm_read(second, CRED(), text, sizeof (text)) == 0 &&
	    sscanf(text, "%u %u %u", &inside, &outside, &count) == 3 &&
	    count == UINT32_MAX,
	    "one open's target namespace replaced another's");
	crfree(full);
	full = NULL;
	crfree(weak);
	weak = NULL;
	lx_userns_rele(ns);
	ns = NULL;
	lx_userns_rele(root);
	root = NULL;
	PM_CHECK(pm_read(uid, CRED(), text, sizeof (text)) == 0 &&
	    sscanf(text, "%u %u %u", &inside, &outside, &count) == 3 &&
	    count == 1,
	    "open map did not retain its target namespace");
	passed = B_TRUE;
out:
	pm_close(uid);
	pm_close(second);
	pm_close(gid);
	pm_close(groups);
	if (reader != NULL)
		crfree(reader);
	if (weak != NULL)
		crfree(weak);
	if (full != NULL)
		crfree(full);
	crfree(basic);
	lx_userns_rele(other);
	lx_userns_rele(ns);
	lx_userns_rele(root);
	if (passed)
		KT_PASS(ctx);
}
