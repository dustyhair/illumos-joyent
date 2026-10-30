/*
 * CDDL version 1.0, http://www.illumos.org/license/CDDL.
 */
#include <sys/types.h>
#include <sys/param.h>
#include <sys/ktest.h>
#include <sys/kmem.h>
#include <sys/errno.h>
#include <sys/brand.h>
#include <sys/cred.h>
#include <sys/priv.h>
#include <sys/proc.h>
#include <sys/file.h>
#include <sys/vnode.h>
#include <sys/lx_userns.h>
#include <sys/lx_cap.h>
#include <sys/priv_impl.h>
#include <sys/lx_brand.h>

extern brand_t lx_brand;

#define	BC_CHECK(expr, why) do { \
	if (!(expr)) { \
		KT_FAIL(ctx, "%s", why); \
		goto out; \
	} \
} while (0)

void
lx_brand_cred_test_copies(ktest_ctx_hdl_t *ctx)
{
	lx_userns_t *root = lx_userns_root_create(), *ns = NULL;
	lx_userns_cred_t actor = { 0 };
	cred_t *original = NULL, *copy = NULL, *keep = NULL, *temporary;
	boolean_t two_refs = B_FALSE, passed = B_FALSE;
	proc_t *target = kmem_zalloc(sizeof (*target), KM_SLEEP);
	struct plock lock = { 0 };
	uint_t variant;

	target->p_lockp = &lock;
	mutex_init(&target->p_crlock, NULL, MUTEX_DEFAULT, NULL);
	mutex_init(&target->p_lock, NULL, MUTEX_DEFAULT, NULL);
	target->p_cred = CRED();
	BC_CHECK(!brand_in_use(&lx_brand), "test requires all lx zones halted");
	BC_CHECK(crgetbrand(CRED()) == NULL,
	    "test caller already has brand data");
	actor.luc_ns = root;
	actor.luc_uid = crgetuid(CRED());
	actor.luc_gid = crgetgid(CRED());
	BC_CHECK(lx_userns_child_create(&actor, &ns) == 0,
	    "cannot create credential namespace");
	actor.luc_ns = ns;
	lx_caps_new_namespace(&actor.luc_caps);
	original = lx_userns_native_for(CRED(), &actor);
	BC_CHECK(crgetbrand(original) != NULL && brand_in_use(&lx_brand),
	    "brand payload did not prevent unload");
	for (variant = 0; variant < 4; variant++) {
		two_refs = variant >= 2;
		switch (variant) {
		case 0:
			copy = crdup(original);
			break;
		case 1:
			copy = cralloc();
			crdup_to(original, copy);
			break;
		case 2:
			temporary = crdup(original);
			copy = crcopy(temporary);
			break;
		default:
			temporary = crdup(original);
			copy = cralloc();
			crcopy_to(temporary, copy);
			break;
		}
		BC_CHECK(crgetbrand(copy) == crgetbrand(original) &&
		    crcmp(copy, original) == 0,
		    "credential copy lost brand state");
		crfree(copy);
		if (two_refs)
			crfree(copy);
		copy = NULL;
	}
	keep = crdup(original);
	crfree(original);
	original = NULL;
	lx_userns_rele(ns);
	ns = NULL;
	lx_userns_rele(root);
	root = NULL;
	BC_CHECK(brand_in_use(&lx_brand) &&
	    priv_proc_cred_perm(keep, target, NULL, VREAD) == EACCES,
	    "copied credential outlived its namespace or lost confinement");
	crfree(keep);
	keep = NULL;
	BC_CHECK(!brand_in_use(&lx_brand), "brand credential reference leaked");
	passed = B_TRUE;
out:
	if (copy != NULL) {
		crfree(copy);
		if (two_refs)
			crfree(copy);
	}
	if (original != NULL)
		crfree(original);
	if (keep != NULL)
		crfree(keep);
	lx_userns_rele(ns);
	lx_userns_rele(root);
	mutex_destroy(&target->p_lock);
	mutex_destroy(&target->p_crlock);
	kmem_free(target, sizeof (*target));
	if (passed)
		KT_PASS(ctx);
}

/* Vnode opens use the actual caller credential, including its brand payload. */
static int
bc_open(cred_t *cred, char *path)
{
	cred_t *saved = CRED();
	vnode_t *vp = NULL;
	int error;

	crhold(cred);
	curthread->t_cred = cred;
	error = vn_open(path, UIO_SYSSPACE, FREAD, 0, &vp, CRCREAT, 0);
	if (error == 0) {
		error = VOP_CLOSE(vp, FREAD, 1, (offset_t)0, cred, NULL);
		VN_RELE(vp);
	}
	curthread->t_cred = saved;
	crfree(cred);
	return (error);
}

void
lx_brand_cred_test_access(ktest_ctx_hdl_t *ctx)
{
	lx_userns_t *root = lx_userns_root_create(), *a = NULL, *b = NULL;
	lx_userns_t *child = NULL;
	lx_userns_cred_t creator = { 0 }, actor;
	cred_t *full = NULL, *zero = NULL, *permitted = NULL;
	cred_t *sibling = NULL, *descendant = NULL, *unclamped = NULL;
	cred_t *basic = lx_userns_native_cred(CRED()), *real = NULL;
	cred_t *captured = NULL, *initial_real = NULL;
	proc_t *target = kmem_zalloc(sizeof (*target), KM_SLEEP);
	struct plock lock = { 0 };
	lx_proc_data_t *pd = kmem_zalloc(sizeof (*pd), KM_SLEEP);
	char path[MAXPATHLEN], uidmap[64], gidmap[64];
	boolean_t passed = B_FALSE;

	target->p_lockp = &lock;
	mutex_init(&target->p_crlock, NULL, MUTEX_DEFAULT, NULL);
	mutex_init(&target->p_lock, NULL, MUTEX_DEFAULT, NULL);
	target->p_brand = &lx_brand;
	target->p_brand_data = pd;
	creator.luc_ns = root;
	creator.luc_uid = crgetuid(CRED());
	creator.luc_gid = crgetgid(CRED());
	lx_caps_new_namespace(&creator.luc_caps);
	BC_CHECK(lx_userns_child_create(&creator, &a) == 0 &&
	    lx_userns_child_create(&creator, &b) == 0,
	    "create sibling namespaces");
	(void) snprintf(uidmap, sizeof (uidmap), "0 %u 1", creator.luc_uid);
	(void) snprintf(gidmap, sizeof (gidmap), "0 %u 1", creator.luc_gid);
	BC_CHECK(lx_userns_map_write(a, 0, &creator, &creator,
	    uidmap, strlen(uidmap), 0) == 0 &&
	    lx_userns_map_write(a, 1, &creator, &creator,
	    gidmap, strlen(gidmap), 0) == 0, "map parent identities");
	actor = creator;
	actor.luc_ns = a;
	full = lx_userns_native_for(CRED(), &actor);
	BC_CHECK(lx_userns_child_create(&actor, &child) == 0,
	    "create descendant namespace");
	actor.luc_ns = child;
	descendant = lx_userns_native_for(CRED(), &actor);
	actor.luc_ns = a;
	actor.luc_caps.lc_effective = 0;
	permitted = lx_userns_native_for(CRED(), &actor);
	actor.luc_caps.lc_permitted = 0;
	zero = lx_userns_native_for(CRED(), &actor);
	actor.luc_ns = b;
	sibling = lx_userns_native_for(CRED(), &actor);
	unclamped = crdup(CRED());
	crsetbrand(unclamped, crgetbrand(full));

	target->p_cred = CRED();
	BC_CHECK(priv_proc_cred_perm(full, target, NULL, VREAD) == EACCES &&
	    priv_proc_cred_perm(unclamped, target, NULL, VWRITE) == EACCES,
	    "namespace credential can access initial namespace process");
	target->p_cred = sibling;
	BC_CHECK(priv_proc_cred_perm(full, target, NULL, VREAD) == EACCES,
	    "namespace credential can access sibling namespace process");
	target->p_cred = full;
	BC_CHECK(priv_proc_cred_perm(zero, target, NULL, VREAD) == EACCES &&
	    priv_proc_cred_perm(permitted, target, NULL, VREAD) == EACCES &&
	    priv_proc_cred_perm(permitted, target, NULL,
	    VWRITE | PRIV_PROC_REALCREDS) == 0,
	    "effective/permitted process-access masks ignored");
	target->p_cred = zero;
	BC_CHECK(priv_proc_cred_perm(zero, target, &captured, VREAD) == 0 &&
	    captured == zero, "matching unprivileged namespace access failed");
	crfree(captured);
	captured = NULL;
	target->p_flag = SNOCD;
	BC_CHECK(priv_proc_cred_perm(full, target, NULL, VREAD) == EACCES,
	    "new namespace exposed non-dumpable initial address space");
	lx_userns_exec(target, full);
	BC_CHECK(priv_proc_cred_perm(permitted, target, NULL,
	    VREAD) == EACCES &&
	    priv_proc_cred_perm(full, target, NULL, VREAD) == 0,
	    "dumpability or scoped ptrace authority ignored");
	target->p_cred = descendant;
	BC_CHECK(priv_proc_cred_perm(zero, target, NULL, VREAD) == EACCES,
	    "namespace owner exposed older non-dumpable address space");
	lx_userns_exec(target, descendant);
	BC_CHECK(priv_proc_cred_perm(zero, target, NULL, VREAD) == 0 &&
	    priv_proc_cred_perm(basic, target, NULL, VREAD) == 0,
	    "namespace ownership did not authorize descendant access");
	target->p_flag = 0;
	initial_real = crdup(basic);
	BC_CHECK(crsetresuid(initial_real, crgetruid(basic),
	    crgetuid(basic) + 1, crgetsuid(basic)) == 0,
	    "cannot prepare initial real-ID credential");
	BC_CHECK(priv_proc_cred_perm(initial_real, target, NULL,
	    VWRITE | PRIV_PROC_REALCREDS) == EACCES,
	    "matching real IDs bypassed namespace ownership");
	target->p_cred = zero;
	target->p_flag = 0;
	real = crdup(zero);
	BC_CHECK(crsetresuid(real, crgetruid(zero), crgetuid(zero) + 1,
	    crgetsuid(zero)) == 0, "cannot prepare real-ID credential");
	BC_CHECK(priv_proc_cred_perm(real, target, NULL, VREAD) == EACCES &&
	    priv_proc_cred_perm(real, target, NULL,
	    VWRITE | PRIV_PROC_REALCREDS) == 0,
	    "real-ID process access used effective IDs");
	BC_CHECK(crcmp(full, zero) != 0,
	    "credential comparison erased authority");
	lx_userns_exec(target, zero);
	mutex_enter(&target->p_lock);
	pd->l_flags |= LX_PROC_NO_DUMP;
	mutex_exit(&target->p_lock);
	BC_CHECK(priv_proc_cred_perm(zero, target, NULL, VREAD) == EACCES &&
	    priv_proc_cred_perm(full, target, NULL, VREAD) == 0,
	    "Linux dumpability state did not constrain scoped access");
	lx_userns_exec(target, zero);
	BC_CHECK((pd->l_flags & LX_PROC_NO_DUMP) == 0 &&
	    priv_proc_cred_perm(zero, target, NULL, VREAD) == 0,
	    "ordinary exec did not reset scoped dumpability");

	(void) snprintf(path, sizeof (path), "/proc/%d/as", curproc->p_ppid);
	BC_CHECK(bc_open(CRED(), path) == 0,
	    "native parent memory fixture missing");
	BC_CHECK(bc_open(unclamped, path) == EACCES,
	    "native procfs bypassed namespace boundary with root credentials");
	BC_CHECK(bc_open(unclamped, "/proc/self/as") == 0,
	    "native procfs denied self access");
	(void) snprintf(path, sizeof (path), "/proc/%d/path/root",
	    curproc->p_ppid);
	BC_CHECK(bc_open(CRED(), path) == 0 &&
	    bc_open(unclamped, path) == EACCES,
	    "native procfs root path bypassed namespace boundary");
	(void) strlcpy(path, "/zones/lx-probe-zfs/root/proc/1/maps",
	    sizeof (path));
	BC_CHECK(bc_open(CRED(), path) == 0, "lx procfs fixture not running");
	BC_CHECK(bc_open(unclamped, path) == EACCES,
	    "lx procfs root shortcut bypassed namespace boundary");
	passed = B_TRUE;
out:
	if (captured != NULL)
		crfree(captured);
	if (real != NULL)
		crfree(real);
	if (initial_real != NULL)
		crfree(initial_real);
	crfree(basic);
	if (unclamped != NULL)
		crfree(unclamped);
	if (descendant != NULL)
		crfree(descendant);
	if (sibling != NULL)
		crfree(sibling);
	if (permitted != NULL)
		crfree(permitted);
	if (zero != NULL)
		crfree(zero);
	if (full != NULL)
		crfree(full);
	lx_userns_exec(target, CRED());
	kmem_free(pd, sizeof (*pd));
	lx_userns_rele(child);
	lx_userns_rele(b);
	lx_userns_rele(a);
	lx_userns_rele(root);
	mutex_destroy(&target->p_lock);
	mutex_destroy(&target->p_crlock);
	kmem_free(target, sizeof (*target));
	if (passed)
		KT_PASS(ctx);
}

void
lx_cap_native_test(ktest_ctx_hdl_t *ctx)
{
	cred_t *candidate = crdup(CRED()), *scoped = NULL;
	lx_userns_t *root = lx_userns_root_create(), *ns = NULL;
	lx_userns_cred_t actor = { 0 };
	lx_caps_t original, wanted, actual, unchanged;
	cred_priv_t saved;
	boolean_t passed = B_FALSE;

	lx_cap_native_get(CRED(), &original);
	wanted = original;
	BC_CHECK((wanted.lc_permitted & LX_CAP_BIT(10)) != 0,
	    "native fixture lacks net_privaddr");
	wanted.lc_inheritable &= ~LX_CAP_BIT(10);
	BC_CHECK(lx_cap_native_update(candidate, &wanted) == 0,
	    "inheritable-only capability drop failed");
	lx_cap_native_get(candidate, &actual);
	BC_CHECK(actual.lc_effective == wanted.lc_effective &&
	    actual.lc_permitted == wanted.lc_permitted &&
	    actual.lc_inheritable == wanted.lc_inheritable,
	    "inheritable-only update changed the wrong sets");
	wanted.lc_inheritable |= LX_CAP_BIT(10);
	BC_CHECK(lx_cap_native_update(candidate, &wanted) == 0,
	    "inheritable-only capability addition failed");
	wanted.lc_effective &= ~LX_CAP_BIT(10);
	wanted.lc_permitted &= ~LX_CAP_BIT(10);
	BC_CHECK(lx_cap_native_update(candidate, &wanted) == 0,
	    "inheritable capability could not outlive permitted capability");
	lx_cap_native_get(candidate, &actual);
	BC_CHECK((actual.lc_inheritable & LX_CAP_BIT(10)) != 0 &&
	    (actual.lc_permitted & LX_CAP_BIT(10)) == 0,
	    "native permitted drop erased the inheritable set");
	saved = *CR_PRIVS(candidate);
	wanted.lc_effective |= LX_CAP_BIT(10);
	BC_CHECK(lx_cap_native_update(candidate, &wanted) == EPERM &&
	    bcmp(&saved, CR_PRIVS(candidate), sizeof (saved)) == 0,
	    "rejected capability update partially changed its credential");
	wanted = actual;
	wanted.lc_effective &= ~LX_CAP_BIT(LX_CAP_SETUID);
	BC_CHECK(lx_cap_native_update(candidate, &wanted) == EPERM &&
	    bcmp(&saved, CR_PRIVS(candidate), sizeof (saved)) == 0,
	    "coupled native SETUID/SETGID privileges pretended to separate");
	bzero(&wanted, sizeof (wanted));
	BC_CHECK(lx_cap_native_update(candidate, &wanted) == 0,
	    "native zero capability sets failed");
	lx_cap_native_get(candidate, &actual);
	BC_CHECK(actual.lc_effective == 0 && actual.lc_permitted == 0 &&
	    actual.lc_inheritable == 0 &&
	    priv_issubset(priv_basic, &CR_EPRIV(candidate)),
	    "zero Linux capabilities erased basic native privileges");
	lx_cap_native_get(CRED(), &unchanged);
	BC_CHECK(bcmp(&original, &unchanged, sizeof (original)) == 0,
	    "copy-on-write candidate changed its source credential");

	actor.luc_ns = root;
	actor.luc_uid = crgetuid(CRED());
	actor.luc_gid = crgetgid(CRED());
	BC_CHECK(lx_userns_child_create(&actor, &ns) == 0,
	    "cannot create scoped capability fixture");
	actor.luc_ns = ns;
	lx_caps_new_namespace(&actor.luc_caps);
	scoped = lx_userns_native_for(CRED(), &actor);
	BC_CHECK(lx_userns_caps_get(scoped, &actual) == 0 &&
	    actual.lc_effective == LX_CAP_ALL,
	    "scoped capability query fell back to native privilege mapping");
	BC_CHECK(lx_cap_native_update(scoped, &wanted) == EOPNOTSUPP &&
	    lx_userns_caps_get(scoped, &actual) == 0 &&
	    actual.lc_effective == LX_CAP_ALL,
	    "native capability update overwrote a scoped immutable payload");
	passed = B_TRUE;
out:
	if (scoped != NULL)
		crfree(scoped);
	crfree(candidate);
	lx_userns_rele(ns);
	lx_userns_rele(root);
	if (passed)
		KT_PASS(ctx);
}
