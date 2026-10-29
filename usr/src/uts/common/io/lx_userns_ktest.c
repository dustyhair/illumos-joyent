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
#include <sys/lx_userns.h>
#include <sys/lx_mntns.h>

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
