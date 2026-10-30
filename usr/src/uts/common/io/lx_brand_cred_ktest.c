/*
 * CDDL version 1.0, http://www.illumos.org/license/CDDL.
 */
#include <sys/types.h>
#include <sys/cred_impl.h>
#include <sys/atomic.h>
#include <sys/sid.h>
#include <sys/zone.h>
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
	bzero(&actor.luc_caps, sizeof (actor.luc_caps));
	copy = lx_userns_native_for(CRED(), &actor);
	BC_CHECK(!lx_userns_traceme(copy, original) &&
	    lx_userns_traceme(original, copy) &&
	    !lx_userns_traceme(original, CRED()) &&
	    lx_userns_traceme(CRED(), original),
	    "TRACEME ignored scoped capability or ancestor authority");
	crfree(copy);
	copy = NULL;
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

/* A global hash hit must not bypass the requested zone's PID view. */
static boolean_t
bc_zone_lookup(void)
{
	zone_t *zone = zone_find_by_name("lx-probe");
	zone_t *other;
	proc_t *p;
	pid_t pid;
	int error;
	boolean_t passed = B_FALSE;

	if (zone == NULL)
		return (B_FALSE);
	pid = zone->zone_proc_initpid;
	if (pid <= 1 || lx_lpid_lock(pid, zone, 0, &p, NULL) != 0)
		goto out;
	passed = p->p_zone == zone;
	mutex_exit(&p->p_lock);
	if (!passed)
		goto out;
	other = kmem_zalloc(sizeof (*other), KM_SLEEP);
	other->zone_brand = &lx_brand;
	other->zone_id = zone->zone_id == 1 ? 2 : 1;
	error = lx_lpid_lock(pid, other, 0, &p, NULL);
	if (error == 0)
		mutex_exit(&p->p_lock);
	passed = error != 0;
	kmem_free(other, sizeof (*other));
out:
	zone_rele(zone);
	return (passed);
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
	BC_CHECK(bc_zone_lookup(),
	    "Linux PID lookup crossed the requested zone");
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

/* Retain only native SID metadata, never a zone or branded credential. */
static credsid_t *lx_scope_seed;

void
lx_userns_test_scope_sids_fini(void)
{
	credsid_t *seed = atomic_swap_ptr(&lx_scope_seed, NULL);

	if (seed != NULL)
		kcrsid_rele(seed);
}

static void
lx_scope_seed_sids(cred_t *cr)
{
	ksid_t sid = { 0 };
	ksidlist_t *list = kmem_zalloc(KSIDLIST_MEM(1), KM_SLEEP);

	sid.ks_domain = ksid_lookupdomain("S-1-5-21-110-220-330");
	sid.ks_rid = 100;
	crsetsid(cr, &sid, KSID_USER);
	sid.ks_domain = ksid_lookupdomain("S-1-5-21-110-220-330");
	sid.ks_rid = 101;
	crsetsid(cr, &sid, KSID_GROUP);
	list->ksl_ref = list->ksl_nsid = 1;
	list->ksl_sids[0].ks_domain =
	    ksid_lookupdomain("S-1-5-21-110-220-330");
	list->ksl_sids[0].ks_rid = 102;
	crsetsidlist(cr, list);
}

void
lx_userns_test_scope_sids(ktest_ctx_hdl_t *ctx)
{
	uchar_t *input;
	size_t size, i;
	pid_t pid = 0;
	zone_t *zone = NULL;
	proc_t *p = NULL;
	cred_t *task = NULL, *summary = NULL;
	credsid_t *seed = atomic_swap_ptr(&lx_scope_seed, NULL);
	boolean_t passed = B_FALSE;

	ktest_get_input(ctx, &input, &size);
	BC_CHECK(size >= 2 && size <= 10 && input[size - 1] == 0,
	    "invalid SID fixture PID input");
	for (i = 0; i + 1 < size; i++) {
		BC_CHECK(input[i] >= '0' && input[i] <= '9' &&
		    pid < 10000000, "invalid SID fixture PID");
		pid = pid * 10 + input[i] - '0';
	}
	zone = zone_find_by_name("lx-probe");
	BC_CHECK(zone != NULL && pid > 1 &&
	    lx_lpid_lock(pid, zone, 0, &p, NULL) == 0,
	    "cannot find stopped SID fixture");
	BC_CHECK(p->p_lwpcnt == 1 && p->p_tlist != NULL &&
	    p->p_tlist->t_state == TS_STOPPED &&
	    strcmp(p->p_user.u_comm, "scope_ids") == 0 &&
	    (ptolxproc(p)->l_flags & LX_PROC_USERNS) != 0,
	    "target is not the stopped scoped SID fixture");
	mutex_enter(&p->p_crlock);
	crhold(task = p->p_tlist->t_cred);
	crhold(summary = p->p_cred);
	mutex_exit(&p->p_crlock);
	mutex_exit(&p->p_lock);
	p = NULL;
	BC_CHECK(seed != NULL &&
	    seed->kr_sidx[KSID_USER].ks_domain != NULL &&
	    seed->kr_sidx[KSID_GROUP].ks_domain != NULL &&
	    seed->kr_sidlist != NULL,
	    "identity changes mutated the shared original SID payload");
	BC_CHECK(crgetuid(task) == 1001 && crgetgid(task) == 1001 &&
	    crgetuid(summary) == 1001 && crgetgid(summary) == 1001,
	    "fixture did not complete mapped effective ID changes");
	BC_CHECK(crgetsid(task, KSID_USER) == NULL &&
	    crgetsid(task, KSID_GROUP) == NULL &&
	    crgetsidlist(task) == NULL,
	    "task retained stale native SID metadata");
	BC_CHECK(crgetsid(summary, KSID_USER) == NULL &&
	    crgetsid(summary, KSID_GROUP) == NULL &&
	    crgetsidlist(summary) == NULL,
	    "process summary retained stale native SID metadata");
	passed = B_TRUE;
out:
	if (p != NULL)
		mutex_exit(&p->p_lock);
	if (task != NULL)
		crfree(task);
	if (summary != NULL)
		crfree(summary);
	if (seed != NULL)
		kcrsid_rele(seed);
	if (zone != NULL)
		zone_rele(zone);
	if (passed)
		KT_PASS(ctx);
}

/* Trusted lab-only entry, never reachable through a Linux namespace syscall. */
void
lx_userns_test_scope_attach(ktest_ctx_hdl_t *ctx)
{
	uchar_t *input;
	size_t size, i;
	pid_t pid = 0;
	zone_t *zone = NULL;
	proc_t *p = NULL;
	kthread_t *task;
	cred_t *base = NULL, *scoped = NULL, *old_process, *old_task;
	lx_userns_t *ns = NULL;
	lx_userns_cred_t actor = { 0 };
	boolean_t passed = B_FALSE;
	const char *map;
	boolean_t identities;

	ktest_get_input(ctx, &input, &size);
	BC_CHECK(size >= 2 && size <= 10 && input[size - 1] == 0,
	    "invalid target PID input");
	for (i = 0; i + 1 < size; i++) {
		BC_CHECK(input[i] >= '0' && input[i] <= '9' &&
		    pid < 10000000, "invalid target PID");
		pid = pid * 10 + input[i] - '0';
	}
	zone = zone_find_by_name("lx-probe");
	BC_CHECK(zone != NULL && pid > 1 &&
	    lx_lpid_lock(pid, zone, 0, &p, NULL) == 0,
	    "cannot find stopped lab process");
	identities = strcmp(p->p_user.u_comm, "scope_ids") == 0;
	BC_CHECK(p->p_lwpcnt == 1 && (identities ||
	    strcmp(p->p_user.u_comm, "scope_caps") == 0) &&
	    p->p_tlist != NULL && p->p_tlist->t_state == TS_STOPPED &&
	    ptolxproc(p)->l_flags == 0 && crgetbrand(p->p_cred) == NULL,
	    "target is not the fresh stopped fixture");
	map = identities ? "0 1000 3\n42 0 1" : "0 0 1";
	mutex_enter(&p->p_crlock);
	crhold(base = p->p_cred);
	mutex_exit(&p->p_crlock);
	mutex_exit(&p->p_lock);
	p = NULL;
	BC_CHECK(crgetuid(base) == 0 && crgetgid(base) == 0,
	    "fixture requires backing root identities");
	actor.luc_ns = ztolxzd(zone)->lxzd_userns;
	lx_caps_new_namespace(&actor.luc_caps);
	BC_CHECK(lx_userns_child_create(&actor, &ns) == 0 &&
	    lx_userns_map_write(ns, 0, &actor, &actor, map,
	    strlen(map), 0) == 0 &&
	    lx_userns_map_write(ns, 1, &actor, &actor, map,
	    strlen(map), 0) == 0,
	    "cannot prepare private fixture namespace");
	actor.luc_ns = ns;
	scoped = lx_userns_native_for(base, &actor);
	if (identities)
		lx_scope_seed_sids(scoped);
	BC_CHECK(lx_lpid_lock(pid, zone, 0, &p, NULL) == 0,
	    "fixture disappeared before attachment");
	task = p->p_tlist;
	BC_CHECK(p->p_lwpcnt == 1 && task != NULL &&
	    task->t_state == TS_STOPPED && strcmp(p->p_user.u_comm,
	    identities ? "scope_ids" : "scope_caps") == 0 &&
	    ptolxproc(p)->l_flags == 0 &&
	    lwptolxlwp(ttolwp(task))->br_stack_mode == LX_STACK_MODE_BRAND,
	    "fixture changed before attachment");
	mutex_enter(&p->p_crlock);
	old_process = p->p_cred;
	old_task = task->t_cred;
	crhold(scoped);
	crhold(scoped);
	p->p_cred = task->t_cred = scoped;
	ptolxproc(p)->l_flags |= LX_PROC_USERNS;
	mutex_exit(&p->p_crlock);
	mutex_exit(&p->p_lock);
	p = NULL;
	crfree(old_process);
	crfree(old_task);
	if (identities) {
		credsid_t *old;

		kcrsid_hold(scoped->cr_ksid);
		old = atomic_swap_ptr(&lx_scope_seed, scoped->cr_ksid);
		if (old != NULL)
			kcrsid_rele(old);
	}
	passed = B_TRUE;
out:
	if (p != NULL)
		mutex_exit(&p->p_lock);
	if (scoped != NULL)
		crfree(scoped);
	if (base != NULL)
		crfree(base);
	if (ns != NULL)
		lx_userns_rele(ns);
	if (zone != NULL)
		zone_rele(zone);
	if (passed)
		KT_PASS(ctx);
}
