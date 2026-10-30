/*
 * CDDL version 1.0, http://www.illumos.org/license/CDDL.
 */
#include <sys/types.h>
#include <sys/ktest.h>
#include <sys/kmem.h>
#include <sys/errno.h>
#include <sys/cred_impl.h>
#include <sys/priv_impl.h>
#include <sys/policy.h>
#include <sys/taskq.h>
#include <sys/disp.h>
#include <sys/lx_userns.h>
#include <sys/lx_cap.h>
#include <sys/lx_mntns.h>
#include <sys/brand.h>
#include <sys/lx_brand.h>
#include <sys/zone.h>
#include <sys/ddi.h>

extern brand_t lx_brand;

#define	NS_CHECK(expr, why) do { \
	if (!(expr)) { \
		KT_FAIL(ctx, "%s", why); \
		goto out; \
	} \
} while (0)

void
lx_userns_test_maps(ktest_ctx_hdl_t *ctx)
{
	lx_userns_t *root = lx_userns_root_create(), *a = NULL, *b = NULL;
	lx_userns_t *other = lx_userns_root_create(), *priv = NULL;
	lx_userns_t *no_fcap = NULL, *ranges = NULL, *cross = NULL;
	lx_userns_cred_t creator = { 0 }, actor, nested, admin, foreign;
	lx_idmap_t *readback = kmem_alloc(sizeof (*readback), KM_SLEEP);
	char *many = kmem_zalloc(4096, KM_SLEEP);
	size_t used = 0;
	uint_t i;
	boolean_t passed = B_FALSE;

	creator.luc_ns = root;
	creator.luc_uid = creator.luc_gid = 1000;
	NS_CHECK(lx_userns_child_create(&creator, &a) == 0, "create A failed");
	actor = creator;
	actor.luc_ns = a;
	lx_caps_new_namespace(&actor.luc_caps);
	NS_CHECK(lx_userns_id(a, 0, 1000, 0) == LX_ID_INVALID,
	    "new namespace has an implicit UID mapping");
	NS_CHECK(!lx_userns_may_setgroups(a), "empty GID map allows setgroups");
	NS_CHECK(lx_userns_map_write(a, 0, &actor, &actor,
	    "0 1000 0", 8, 0) == EINVAL, "zero-count map accepted");
	NS_CHECK(lx_userns_map_write(a, 0, &actor, &actor,
	    "0 1001 1", 8, 0) == EPERM,
	    "unprivileged foreign UID map accepted");
	foreign = actor;
	foreign.luc_ns = other;
	NS_CHECK(lx_userns_map_write(a, 0, &foreign, &foreign,
	    "0 1000 1", 8, 0) == EPERM, "unrelated namespace can write map");
	NS_CHECK(lx_userns_map_write(a, 0, &actor, &actor,
	    "0 1000 1", 8, 1) == EINVAL, "nonzero map offset accepted");
	NS_CHECK(lx_userns_map_write(a, 0, &actor, &actor,
	    "0 1000 1", 8, 0) == 0, "self UID map rejected");
	NS_CHECK(lx_userns_map_write(a, 0, &actor, &actor,
	    "bad", 3, 0) == EPERM, "map rewrite did not fail before parsing");
	NS_CHECK(lx_userns_id(a, 0, 1000, 0) == 0 &&
	    lx_userns_id(a, 0, 0, 1) == 1000,
	    "UID translation changed the backing identity");
	lx_userns_map_read(a, 0, a, readback);
	NS_CHECK(readback->lim_count == 1 &&
	    readback->lim_extents[0].lie_outside == 1000,
	    "self map read is not relative to parent");
	NS_CHECK(lx_userns_map_write(a, 1, &actor, &actor,
	    "0 1000 1", 8, 0) == EPERM, "unprivileged GID map bypassed deny");
	NS_CHECK(lx_userns_groups_write(a, &actor, "deny\n", 5, 0) == 0,
	    "cannot disable setgroups");
	NS_CHECK(lx_userns_groups_write(a, &actor, "allow", 5, 0) == EPERM,
	    "setgroups denial was reversible");
	NS_CHECK(lx_userns_map_write(a, 1, &actor, &actor,
	    "0 1000 1", 8, 0) == 0, "GID map rejected after deny");
	NS_CHECK(!lx_userns_may_setgroups(a),
	    "GID map cleared setgroups denial");
	NS_CHECK(lx_userns_child_create(&actor, &b) == 0,
	    "create nested B failed");
	nested = actor;
	nested.luc_ns = b;
	lx_caps_new_namespace(&nested.luc_caps);
	NS_CHECK(lx_userns_groups_write(b, &nested, "allow", 5, 0) == EPERM,
	    "nested namespace lost inherited setgroups denial");
	NS_CHECK(lx_userns_map_write(b, 0, &nested, &nested,
	    "1000 0 1", 8, 0) == 0 &&
	    lx_userns_map_write(b, 1, &nested, &nested,
	    "1000 0 1", 8, 0) == 0, "nested identity map rejected");
	NS_CHECK(lx_userns_id(b, 0, 1000, 0) == 1000 &&
	    lx_userns_id(b, 1, 1000, 0) == 1000,
	    "nested mapping is not flattened to backing IDs");
	lx_userns_map_read(b, 0, b, readback);
	NS_CHECK(readback->lim_extents[0].lie_outside == 0,
	    "nested self map did not report parent ID");
	lx_userns_map_read(b, 0, root, readback);
	NS_CHECK(readback->lim_extents[0].lie_outside == 1000,
	    "ancestor map read did not report ancestor ID");
	NS_CHECK(!lx_userns_has_cap(&nested, a, LX_CAP_SYS_ADMIN) &&
	    !lx_userns_has_cap(&actor, root, LX_CAP_SYS_ADMIN) &&
	    !lx_userns_has_cap(&foreign, a, LX_CAP_SYS_ADMIN),
	    "child or unrelated capabilities escaped their namespace");
	NS_CHECK(lx_userns_has_cap(&creator, b, LX_CAP_SYS_ADMIN),
	    "parent namespace owner lost descendant authority");

	admin = creator;
	lx_caps_new_namespace(&admin.luc_caps);
	NS_CHECK(lx_userns_child_create(&admin, &priv) == 0,
	    "cannot create privileged mapping target");
	NS_CHECK(lx_userns_map_write(priv, 0, &admin, &creator,
	    "0 0 2", 5, 0) == EPERM,
	    "map write ignored current task's missing parent capability");
	NS_CHECK(lx_userns_map_write(priv, 0, &admin, &admin,
	    "50 1000 5\n0 0 1", 15, 0) == 0,
	    "privileged multi-range UID map rejected");
	NS_CHECK(lx_userns_map_write(priv, 1, &admin, &admin,
	    "0 1000 1", 8, 0) == 0 && lx_userns_may_setgroups(priv),
	    "privileged GID mapping failed to enable setgroups");
	NS_CHECK(lx_userns_groups_write(priv, &admin, "deny", 4, 0) == EPERM,
	    "setgroups can be disabled after GID map publication");
	NS_CHECK(lx_userns_groups_write(priv, &admin, "allow", 5, 0) == 0,
	    "idempotent allow rejected");
	NS_CHECK(lx_userns_id(priv, 0, 1002, 0) == 52 &&
	    lx_userns_id(priv, 0, 54, 1) == 1004 &&
	    lx_userns_id(priv, 0, 1005, 0) == LX_ID_INVALID,
	    "range edge or offset translation failed");
	admin.luc_ns = priv;
	NS_CHECK(lx_userns_child_create(&admin, &cross) == 0,
	    "cannot create range-composition target");
	NS_CHECK(lx_userns_map_write(cross, 0, &admin, &admin,
	    "0 50 6", 6, 0) == EPERM,
	    "child map crosses an unmapped parent range");
	NS_CHECK(lx_userns_map_write(cross, 0, &admin, &admin,
	    "0 50 5", 6, 0) == 0 &&
	    lx_userns_id(cross, 0, 4, 1) == 1004,
	    "nested range composition failed");

	admin.luc_ns = root;
	admin.luc_uid = admin.luc_gid = 0;
	admin.luc_caps.lc_effective &= ~LX_CAP_BIT(LX_CAP_SETFCAP);
	NS_CHECK(lx_userns_child_create(&admin, &no_fcap) == 0,
	    "cannot create SETFCAP restriction target");
	foreign = admin;
	foreign.luc_ns = no_fcap;
	lx_caps_new_namespace(&foreign.luc_caps);
	NS_CHECK(lx_userns_map_write(no_fcap, 0, &foreign, &foreign,
	    "0 0 1", 5, 0) == EPERM,
	    "creator without SETFCAP mapped parent root");
	NS_CHECK(lx_userns_map_write(no_fcap, 0, &admin, &admin,
	    "0 0 1", 5, 0) == EPERM,
	    "parent opener without SETFCAP mapped root");
	lx_caps_new_namespace(&admin.luc_caps);
	NS_CHECK(lx_userns_map_write(no_fcap, 0, &admin, &admin,
	    "0 0 1", 5, 0) == 0, "privileged parent root mapping rejected");

	NS_CHECK(lx_userns_child_create(&admin, &ranges) == 0,
	    "cannot create large map target");
	for (i = LX_IDMAP_EXTENTS; i != 0; i--)
		used += snprintf(many + used, 4096 - used,
		    "%u %u 1\n", i - 1, i - 1);
	NS_CHECK(lx_userns_map_write(ranges, 0, &admin, &admin,
	    many, used, 0) == 0, "340-range map rejected");
	lx_userns_map_read(ranges, 0, root, readback);
	NS_CHECK(readback->lim_count == LX_IDMAP_EXTENTS &&
	    readback->lim_extents[0].lie_inside == 0 &&
	    readback->lim_extents[LX_IDMAP_EXTENTS - 1].lie_inside == 339,
	    "large map was not sorted or was truncated");
	NS_CHECK(lx_userns_map_write(root, 0, &admin, &admin,
	    "0 0 1", 5, 0) == EPERM, "initial namespace map was writable");
	passed = B_TRUE;
out:
	lx_userns_rele(cross);
	lx_userns_rele(ranges);
	lx_userns_rele(no_fcap);
	lx_userns_rele(priv);
	lx_userns_rele(b);
	lx_userns_rele(a);
	lx_userns_rele(other);
	lx_userns_rele(root);
	kmem_free(readback, sizeof (*readback));
	kmem_free(many, 4096);
	if (passed)
		KT_PASS(ctx);
}

void
lx_userns_test_caps(ktest_ctx_hdl_t *ctx)
{
	lx_caps_t caps;
	uint_t i;
	boolean_t passed = B_FALSE;

	lx_caps_new_namespace(&caps);
	for (i = 0; i <= LX_CAP_LAST; i++)
		NS_CHECK(lx_caps_drop(&caps, i) == 0, "bounding drop failed");
	NS_CHECK(caps.lc_bounding == 0 && caps.lc_effective == LX_CAP_ALL &&
	    caps.lc_permitted == LX_CAP_ALL,
	    "bounding drop changed current effective/permitted capabilities");
	NS_CHECK(lx_caps_set(&caps, 0, 0, 0) == 0, "drop all caps failed");
	NS_CHECK(lx_caps_set(&caps, 1, 1, 0) == EPERM,
	    "capset regained dropped permitted capability");
	NS_CHECK(lx_caps_drop(&caps, 99) == EPERM,
	    "bounding drop ignored missing SETPCAP");
	lx_caps_exec_plain(&caps, 1, 1, 0);
	NS_CHECK(caps.lc_effective == 0 && caps.lc_permitted == 0,
	    "root exec regained dropped bounding capabilities");
	lx_caps_new_namespace(&caps);
	NS_CHECK(lx_caps_drop(&caps, 99) == EINVAL,
	    "invalid cap index accepted");
	NS_CHECK(lx_caps_set(&caps, LX_CAP_ALL, LX_CAP_ALL, 1) == 0 &&
	    lx_caps_ambient(&caps, 2, 0) == 0,
	    "ambient raise failed with required permitted/inheritable bits");
	lx_caps_exec_plain(&caps, 0, 0, 0);
	NS_CHECK(caps.lc_effective == 1 && caps.lc_permitted == 1 &&
	    caps.lc_ambient == 1, "ordinary nonroot exec lost ambient caps");
	NS_CHECK(lx_caps_set(&caps, 1, 1, 0) == 0 && caps.lc_ambient == 0,
	    "capset did not trim ambient set");
	lx_caps_new_namespace(&caps);
	NS_CHECK(lx_caps_securebits(&caps, 3) == 0 &&
	    lx_caps_securebits(&caps, 0) == EPERM,
	    "locked NOROOT bit was reversible");
	NS_CHECK(lx_caps_keep(&caps, 1) == 0 &&
	    lx_caps_securebits(&caps, 0x33) == 0 &&
	    lx_caps_keep(&caps, 0) == EPERM, "KEEP_CAPS lock failed");
	lx_caps_exec_plain(&caps, 1, 1, 0);
	NS_CHECK((caps.lc_securebits & LX_SEC_KEEP_CAPS) == 0 &&
	    caps.lc_effective == 0, "exec did not clear KEEP_CAPS/NOROOT caps");
	lx_caps_new_namespace(&caps);
	lx_caps_uid_change(&caps, 7, 0);
	NS_CHECK(caps.lc_effective == 0 && caps.lc_permitted == 0,
	    "root UID drop retained capabilities without KEEP_CAPS");
	passed = B_TRUE;
out:
	if (passed)
		KT_PASS(ctx);
}

void
lx_userns_test_native(ktest_ctx_hdl_t *ctx)
{
	cred_t *source = CRED(), *reduced = lx_userns_native_cred(source);
	lx_mntns_view_t *view = NULL;
	uint_t i;
	boolean_t passed = B_FALSE;

	NS_CHECK(crgetuid(reduced) == crgetuid(source) &&
	    crgetruid(reduced) == crgetruid(source) &&
	    crgetsuid(reduced) == crgetsuid(source) &&
	    crgetgid(reduced) == crgetgid(source) &&
	    crgetrgid(reduced) == crgetrgid(source) &&
	    crgetsgid(reduced) == crgetsgid(source),
	    "native privilege reduction changed backing UID/GID");
	for (i = 0; i < PRIV_NSET; i++)
		NS_CHECK(priv_issubset(&CR_PRIVSETS(reduced)[i], priv_basic),
		    "native credential retained non-basic privilege");
	NS_CHECK((CR_FLAGS(reduced) & PRIV_AWARE) != 0 &&
	    (CR_FLAGS(reduced) & (PRIV_AWARE_RESET | PRIV_PFEXEC |
	    PRIV_XPOLICY)) == 0, "native privilege awareness flags unsafe");
	NS_CHECK(secpolicy_sys_config(reduced, B_TRUE) == EPERM,
	    "namespace native credential can configure host");
	NS_CHECK(lx_mntns_view_create("/", reduced, &view) == EPERM &&
	    view == NULL,
	    "reduced native credential can mount outside a scoped operation");
	passed = B_TRUE;
out:
	if (view != NULL && lx_mntns_view_destroy(view, source) != 0)
		passed = B_FALSE;
	crfree(reduced);
	if (passed)
		KT_PASS(ctx);
}

void
lx_userns_test_affinity(ktest_ctx_hdl_t *ctx)
{
	lx_userns_t *root = lx_userns_root_create(), *a = NULL, *b = NULL;
	lx_userns_t *sibling = NULL;
	lx_userns_cred_t actor = { 0 }, child, admin;
	cred_t *base = crdup(CRED()), *full = NULL, *weak = NULL;
	cred_t *nested = NULL, *peer = NULL, *target = NULL;
	boolean_t passed = B_FALSE;

	VERIFY0(crsetresuid(base, 1000, 1000, 1000));
	actor.luc_ns = root;
	actor.luc_uid = actor.luc_gid = 1000;
	NS_CHECK(lx_userns_child_create(&actor, &a) == 0 &&
	    lx_userns_child_create(&actor, &sibling) == 0,
	    "create sibling affinity namespaces failed");
	NS_CHECK(lx_userns_map_actor(CRED(), root, &admin) == 0 &&
	    lx_userns_map_write(a, 0, &admin, &admin, "0 1000 2", 8, 0) == 0 &&
	    lx_userns_map_write(a, 1, &admin, &admin, "0 1000 1", 8, 0) == 0,
	    "map parent affinity namespace failed");
	actor.luc_ns = a;
	lx_caps_new_namespace(&actor.luc_caps);
	full = lx_userns_native_for(base, &actor);
	actor.luc_caps.lc_effective &= ~LX_CAP_BIT(LX_CAP_SYS_NICE);
	weak = lx_userns_native_for(base, &actor);
	child = actor;
	child.luc_uid = 1001;
	NS_CHECK(lx_userns_child_create(&child, &b) == 0,
	    "create descendant affinity namespace failed");
	child.luc_ns = b;
	nested = lx_userns_native_for(base, &child);
	child.luc_ns = sibling;
	peer = lx_userns_native_for(base, &child);
	target = crdup(weak);
	VERIFY0(crsetresuid(target, 2000, 2000, 2000));
	NS_CHECK(lx_userns_cred_capable(full, target, LX_CAP_SYS_NICE) &&
	    !lx_userns_cred_capable(weak, target, LX_CAP_SYS_NICE),
	    "affinity capability ignored effective authority");
	NS_CHECK(lx_userns_cred_capable(full, nested, LX_CAP_SYS_NICE) &&
	    !lx_userns_cred_capable(weak, nested, LX_CAP_SYS_NICE) &&
	    !lx_userns_cred_capable(full, peer, LX_CAP_SYS_NICE),
	    "affinity namespace hierarchy escaped its authority");
	NS_CHECK(!lx_userns_cred_capable(full, base, LX_CAP_SYS_NICE) &&
	    !lx_userns_cred_capable(full, target, LX_CAP_LAST + 1),
	    "affinity capability reached initial namespace or invalid index");
	NS_CHECK(lx_userns_cred_capable(base, full, LX_CAP_SYS_NICE),
	    "parent namespace owner lost child affinity authority");
	NS_CHECK(brand_cred_proc_perm(base, curproc, base,
	    BRAND_CRED_AFFINITY) == BRAND_CRED_DEFER,
	    "native affinity policy did not defer");
	NS_CHECK(brand_cred_proc_perm(full, curproc, target,
	    BRAND_CRED_AFFINITY) == 0 &&
	    brand_cred_proc_perm(weak, curproc, target,
	    BRAND_CRED_AFFINITY) == EPERM,
	    "affinity callback used ptrace or process identity authority");
	VERIFY0(crsetresuid(target, 1000, 2000, 2000));
	VERIFY0(crsetresgid(target, 2000, 2000, 2000));
	NS_CHECK(brand_cred_proc_perm(weak, curproc, target,
	    BRAND_CRED_AFFINITY) == 0,
	    "affinity real UID ownership required group equality");
	passed = B_TRUE;
out:
	if (target != NULL)
		crfree(target);
	if (peer != NULL)
		crfree(peer);
	if (nested != NULL)
		crfree(nested);
	if (weak != NULL)
		crfree(weak);
	if (full != NULL)
		crfree(full);
	crfree(base);
	lx_userns_rele(b);
	lx_userns_rele(sibling);
	lx_userns_rele(a);
	lx_userns_rele(root);
	if (passed)
		KT_PASS(ctx);
}

void
lx_userns_test_map_authority(ktest_ctx_hdl_t *ctx)
{
	lx_userns_t *root = lx_userns_root_create(), *full = NULL, *weak = NULL;
	cred_t *partial = lx_userns_native_cred(CRED());
	lx_userns_cred_t admin, limited;
	lx_caps_t caps;
	uint_t i;
	boolean_t passed = B_FALSE;

	NS_CHECK(secpolicy_vnode_setid_retain(CRED(), B_TRUE) == 0 &&
	    lx_userns_map_actor(CRED(), root, &admin) == 0 &&
	    (admin.luc_caps.lc_effective & LX_CAP_BIT(LX_CAP_SETFCAP)) != 0,
	    "native fixture lacks privileged-file map authority");
	NS_CHECK(lx_userns_child_create(&admin, &full) == 0 &&
	    lx_userns_map_write(full, 0, &admin, &admin, "0 0 1", 5, 0) == 0,
	    "native privileged-file authority cannot map initial UID0");
	NS_CHECK(lx_userns_id(full, 0, 0, 1) == 0,
	    "initial UID0 map changed its backing identity");
	for (i = 0; i < PRIV_NSET; i++) {
		priv_addset(&CR_PRIVSETS(partial)[i], PRIV_FILE_SETID);
		priv_addset(&CR_PRIVSETS(partial)[i], PRIV_PROC_SETID);
	}
	lx_cap_native_get(partial, &caps);
	NS_CHECK((caps.lc_effective & LX_CAP_BIT(LX_CAP_SETFCAP)) != 0 &&
	    secpolicy_vnode_setid_retain(partial, B_TRUE) == EPERM,
	    "limited metadata privilege bypassed native setuid-root policy");
	NS_CHECK(lx_userns_map_actor(partial, root, &limited) == 0 &&
	    (limited.luc_caps.lc_effective & LX_CAP_BIT(LX_CAP_SETFCAP)) == 0,
	    "initial map authority bypassed privileged-file policy");
	NS_CHECK(lx_userns_child_create(&limited, &weak) == 0 &&
	    lx_userns_map_write(weak, 0, &limited, &limited,
	    "0 0 1", 5, 0) == EPERM,
	    "limited native credential mapped initial UID0");
	NS_CHECK(lx_userns_map_write(weak, 0, &limited, &limited,
	    "0 1000 1", 8, 0) == 0 &&
	    lx_userns_id(weak, 0, 0, 1) == 1000,
	    "root-map denial blocked an authorized non-root mapping");
	passed = B_TRUE;
out:
	crfree(partial);
	lx_userns_rele(weak);
	lx_userns_rele(full);
	lx_userns_rele(root);
	if (passed)
		KT_PASS(ctx);
}

typedef struct lx_ns_race {
	kmutex_t nr_lock;
	kcondvar_t nr_cv;
	uint_t nr_ready;
	lx_userns_t *nr_ns;
	lx_userns_cred_t nr_actor;
	int nr_results[2];
} lx_ns_race_t;

typedef struct lx_ns_writer {
	lx_ns_race_t *nw_race;
	uint_t nw_index;
} lx_ns_writer_t;

static void
lx_ns_writer(void *arg)
{
	lx_ns_writer_t *writer = arg;
	lx_ns_race_t *race = writer->nw_race;
	uint_t index = writer->nw_index;

	mutex_enter(&race->nr_lock);
	if (++race->nr_ready == 2)
		cv_broadcast(&race->nr_cv);
	while (race->nr_ready < 2)
		cv_wait(&race->nr_cv, &race->nr_lock);
	mutex_exit(&race->nr_lock);
	race->nr_results[index] = lx_userns_map_write(race->nr_ns, 0,
	    &race->nr_actor, &race->nr_actor,
	    index == 0 ? "0 1000 1" : "1 1000 1", 8, 0);
}

void
lx_userns_test_lifetime(ktest_ctx_hdl_t *ctx)
{
	taskq_t *queue = taskq_create("lx_ns_writers", 2, minclsyspri,
	    2, 2, TASKQ_PREPOPULATE);
	lx_userns_t *root = lx_userns_root_create(), *chain = NULL;
	lx_userns_t *next = NULL;
	lx_userns_cred_t creator = { 0 };
	lx_ns_race_t race = { 0 };
	lx_ns_writer_t writers[2] = { { &race, 0 }, { &race, 1 } };
	uint_t round, winner;
	boolean_t passed = B_FALSE;

	mutex_init(&race.nr_lock, NULL, MUTEX_DEFAULT, NULL);
	cv_init(&race.nr_cv, NULL, CV_DEFAULT, NULL);
	creator.luc_ns = root;
	creator.luc_uid = creator.luc_gid = 1000;
	NS_CHECK(queue != NULL, "cannot create concurrent map writers");
	for (round = 0; round < 32; round++) {
		race.nr_ready = 0;
		NS_CHECK(lx_userns_child_create(&creator, &race.nr_ns) == 0,
		    "cannot create concurrent map target");
		race.nr_actor = creator;
		race.nr_actor.luc_ns = race.nr_ns;
		lx_caps_new_namespace(&race.nr_actor.luc_caps);
		(void) taskq_dispatch(queue, lx_ns_writer, &writers[0],
		    TQ_SLEEP);
		(void) taskq_dispatch(queue, lx_ns_writer, &writers[1],
		    TQ_SLEEP);
		taskq_wait(queue);
		winner = race.nr_results[0] == 0 ? 0 : 1;
		NS_CHECK(race.nr_results[winner] == 0 &&
		    race.nr_results[1 - winner] == EPERM &&
		    lx_userns_id(race.nr_ns, 0, 1000, 0) == winner,
		    "concurrent map writers did not publish exactly one map");
		lx_userns_rele(race.nr_ns);
		race.nr_ns = NULL;
	}
	chain = lx_userns_root_create();
	creator.luc_ns = chain;
	creator.luc_uid = creator.luc_gid = 0;
	lx_caps_new_namespace(&creator.luc_caps);
	for (round = 0; round < 33; round++) {
		NS_CHECK(lx_userns_child_create(&creator, &next) == 0,
		    "namespace depth rejected before limit");
		NS_CHECK(lx_userns_map_write(next, 0, &creator, &creator,
		    "0 0 1", 5, 0) == 0 &&
		    lx_userns_map_write(next, 1, &creator, &creator,
		    "0 0 1", 5, 0) == 0, "deep namespace identity map failed");
		lx_userns_rele(chain);
		chain = next;
		next = NULL;
		creator.luc_ns = chain;
	}
	NS_CHECK(lx_userns_child_create(&creator, &next) == ENOSPC &&
	    next == NULL,
	    "namespace depth limit not enforced");
	passed = B_TRUE;
out:
	if (queue != NULL)
		taskq_destroy(queue);
	lx_userns_rele(race.nr_ns);
	lx_userns_rele(next);
	lx_userns_rele(chain);
	lx_userns_rele(root);
	cv_destroy(&race.nr_cv);
	mutex_destroy(&race.nr_lock);
	if (passed)
		KT_PASS(ctx);
}

/* Native aggregate budget tests; this is not Linux per-user ucounts. */
static boolean_t
lx_budget_is(lx_userns_t *ns, uint32_t maximum, uint32_t used)
{
	uint32_t actual_max, actual_used;

	lx_userns_budget_get(ns, &actual_max, &actual_used);
	return (actual_max == maximum && actual_used == used);
}

void
lx_userns_test_budget(ktest_ctx_hdl_t *ctx)
{
	lx_userns_t *root = lx_userns_root_create(), *a = NULL, *b = NULL;
	lx_userns_cred_t creator = { 0 }, nested;
	boolean_t passed = B_FALSE;

	creator.luc_ns = root;
	creator.luc_uid = creator.luc_gid = 1000;
	lx_caps_new_namespace(&creator.luc_caps);
	NS_CHECK(lx_budget_is(root, LX_USERNS_DEFAULT_MAX, 0),
	    "incorrect initial namespace budget");
	NS_CHECK(lx_userns_budget_set(root, 0) == 0 &&
	    lx_userns_child_create(&creator, &a) == ENOSPC && a == NULL &&
	    lx_budget_is(root, 0, 0), "disabled budget allowed allocation");
	NS_CHECK(lx_userns_budget_set(root, 2) == 0 &&
	    lx_userns_child_create(&creator, &a) == 0,
	    "cannot allocate first budgeted namespace");
	NS_CHECK(lx_userns_budget_set(a, 9) == EINVAL &&
	    lx_budget_is(a, 2, 1), "child changed or escaped the root budget");
	nested = creator;
	nested.luc_ns = a;
	NS_CHECK(lx_userns_child_create(&nested, &b) == EPERM && b == NULL &&
	    lx_budget_is(root, 2, 1), "failed construction leaked a charge");
	NS_CHECK(lx_userns_map_write(a, 0, &creator, &creator,
	    "0 1000 1", 8, 0) == 0 &&
	    lx_userns_map_write(a, 1, &creator, &creator,
	    "0 1000 1", 8, 0) == 0 &&
	    lx_userns_child_create(&nested, &b) == 0 &&
	    lx_budget_is(b, 2, 2), "nested namespace escaped root accounting");
	NS_CHECK(lx_userns_budget_set(root, 1) == 0 &&
	    lx_budget_is(root, 1, 2), "lowering a limit revoked live objects");
	lx_userns_rele(a);
	a = NULL;
	NS_CHECK(lx_budget_is(root, 1, 2),
	    "parent reference was released while its child remained alive");
	lx_userns_rele(b);
	b = NULL;
	NS_CHECK(lx_budget_is(root, 1, 0), "cascade release leaked charges");
	NS_CHECK(lx_userns_child_create(&creator, &a) == 0 &&
	    lx_userns_child_create(&creator, &b) == ENOSPC && b == NULL,
	    "limit did not apply after reuse");
	lx_userns_hold(a);
	lx_userns_rele(a);
	NS_CHECK(lx_budget_is(root, 1, 1), "extra reference lost its charge");
	lx_userns_rele(a);
	a = NULL;
	NS_CHECK(lx_budget_is(root, 1, 0),
	    "last reference retained its charge");
	passed = B_TRUE;
out:
	lx_userns_rele(b);
	lx_userns_rele(a);
	lx_userns_rele(root);
	if (passed)
		KT_PASS(ctx);
}

typedef struct lx_budget_job {
	const lx_userns_cred_t *bj_creator;
	lx_userns_t *bj_ns;
	int bj_error;
} lx_budget_job_t;

static void
lx_budget_worker(void *arg)
{
	lx_budget_job_t *job = arg;

	job->bj_error = lx_userns_child_create(job->bj_creator, &job->bj_ns);
}

void
lx_userns_test_budget_race(ktest_ctx_hdl_t *ctx)
{
	lx_userns_t *root = lx_userns_root_create();
	lx_userns_cred_t creator = { 0 };
	lx_budget_job_t jobs[16] = { 0 };
	taskq_t *queue = taskq_create("lx_ns_budget", 4, minclsyspri,
	    16, 16, TASKQ_PREPOPULATE);
	uint_t i, round, successes;
	boolean_t passed = B_FALSE;

	creator.luc_ns = root;
	creator.luc_uid = creator.luc_gid = 1000;
	NS_CHECK(queue != NULL && lx_userns_budget_set(root, 4) == 0,
	    "cannot prepare concurrent namespace allocation");
	for (round = 0; round < 32; round++) {
		for (i = 0; i < 16; i++) {
			jobs[i].bj_creator = &creator;
			(void) taskq_dispatch(queue, lx_budget_worker, &jobs[i],
			    TQ_SLEEP);
		}
		taskq_wait(queue);
		successes = 0;
		for (i = 0; i < 16; i++) {
			if (jobs[i].bj_error == 0) {
				NS_CHECK(jobs[i].bj_ns != NULL,
				    "successful creation returned no object");
				successes++;
			} else {
				NS_CHECK(jobs[i].bj_error == ENOSPC &&
				    jobs[i].bj_ns == NULL,
				    "failed reservation returned an object");
			}
		}
		NS_CHECK(successes == 4 && lx_budget_is(root, 4, 4),
		    "concurrent allocation oversubscribed or lost the budget");
		for (i = 0; i < 16; i++) {
			lx_userns_rele(jobs[i].bj_ns);
			jobs[i].bj_ns = NULL;
		}
		NS_CHECK(lx_budget_is(root, 4, 0),
		    "concurrent allocation/release leaked a charge");
	}
	passed = B_TRUE;
out:
	if (queue != NULL)
		taskq_destroy(queue);
	for (i = 0; i < 16; i++)
		lx_userns_rele(jobs[i].bj_ns);
	lx_userns_rele(root);
	if (passed)
		KT_PASS(ctx);
}

static void
lx_userns_zone_budget(ktest_ctx_hdl_t *ctx, boolean_t disabled)
{
	zone_t *zone = zone_find_by_name("lx-probe");
	lx_userns_t *root, *a = NULL, *b = NULL, *extra = NULL;
	lx_userns_cred_t creator = { 0 };
	uint32_t maximum, used;
	uint_t attempt;
	boolean_t passed = B_FALSE;

	NS_CHECK(zone != NULL && zone->zone_brand == &lx_brand,
	    "private lx-probe zone missing");
	root = ztolxzd(zone)->lxzd_userns;
	creator.luc_ns = root;
	creator.luc_uid = creator.luc_gid = 1000;
	NS_CHECK(lx_budget_is(root, disabled ? 0 : 2, 0),
	    "native zone attribute did not set the expected budget");
	if (disabled) {
		NS_CHECK(lx_userns_child_create(&creator, &a) == ENOSPC &&
		    a == NULL && lx_budget_is(root, 0, 0),
		    "native zero budget did not prevent allocation");
	} else {
		NS_CHECK(lx_userns_child_create(&creator, &a) == 0 &&
		    lx_userns_child_create(&creator, &b) == 0 &&
		    lx_userns_child_create(&creator, &extra) == ENOSPC &&
		    extra == NULL, "native budget did not enforce two objects");
		/* Wait for the native controller to lower the limit. */
		for (attempt = 0; attempt < 200; attempt++) {
			lx_userns_budget_get(root, &maximum, &used);
			if (maximum == 1)
				break;
			delay(drv_usectohz(50000));
		}
		NS_CHECK(lx_budget_is(root, 1, 2),
		    "native controller failed to observe/lower a live budget");
		NS_CHECK(lx_userns_child_create(&creator, &extra) == ENOSPC &&
		    extra == NULL, "lowering the budget allowed more objects");
	}
	passed = B_TRUE;
out:
	lx_userns_rele(extra);
	lx_userns_rele(b);
	lx_userns_rele(a);
	if (zone != NULL)
		zone_rele(zone);
	if (passed)
		KT_PASS(ctx);
}

void
lx_userns_test_zone_budget_disabled(ktest_ctx_hdl_t *ctx)
{
	lx_userns_zone_budget(ctx, B_TRUE);
}

void
lx_userns_test_zone_budget_hold(ktest_ctx_hdl_t *ctx)
{
	lx_userns_zone_budget(ctx, B_FALSE);
}
