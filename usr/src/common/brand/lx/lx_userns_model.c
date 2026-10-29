/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * A copy is available at http://www.illumos.org/license/CDDL.
 */
#include "lx_userns_model.h"
#ifdef _KERNEL
#include <sys/systm.h>
#include <sys/errno.h>
#else
#include <string.h>
#include <errno.h>
#endif

static int
lx_id_space(unsigned char c)
{
	return (c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f');
}

void
lx_idmap_identity(lx_idmap_t *map)
{
	(void) memset(map, 0, sizeof (*map));
	map->lim_count = 1;
	map->lim_extents[0].lie_count = UINT32_MAX;
}

/* Linux decimal map fields are converted to u32 before range validation. */
static int
lx_id_number(const char **cursor, const char *end, uint32_t *value)
{
	const char *p = *cursor;
	uint32_t n = 0;

	while (p != end && lx_id_space(*p))
		p++;
	if (p == end || *p < '0' || *p > '9')
		return (0);
	do {
		n = n * 10U + (unsigned char)*p++ - '0';
	} while (p != end && *p >= '0' && *p <= '9');
	*cursor = p;
	*value = n;
	return (1);
}

static int
lx_id_overlap(uint32_t a, uint32_t acount, uint32_t b, uint32_t bcount)
{
	return (a < b + bcount && b < a + acount);
}

int
lx_idmap_parse(const char *input, size_t size, lx_idmap_t *map)
{
	const char *end, *p, *line;
	lx_id_extent_t extent;
	uint32_t i;

	(void) memset(map, 0, sizeof (*map));
	if (size == 0 || size >= 4096)
		return (EINVAL);
	/* Linux's text parser stops at the first embedded NUL. */
	for (end = input; end != input + size && *end != '\0'; end++)
		continue;
	for (p = input; p != end; p = line == end ? end : line + 1) {
		for (line = p; line != end && *line != '\n'; line++)
			continue;
		if (map->lim_count == LX_IDMAP_EXTENTS ||
		    !lx_id_number(&p, line, &extent.lie_inside) || p == line ||
		    !lx_id_space(*p) ||
		    !lx_id_number(&p, line, &extent.lie_outside) || p == line ||
		    !lx_id_space(*p) ||
		    !lx_id_number(&p, line, &extent.lie_count))
			return (EINVAL);
		while (p != line && lx_id_space(*p))
			p++;
		if (p != line || extent.lie_count == 0 ||
		    extent.lie_count > UINT32_MAX - extent.lie_inside ||
		    extent.lie_count > UINT32_MAX - extent.lie_outside)
			return (EINVAL);
		for (i = 0; i < map->lim_count; i++) {
			const lx_id_extent_t *old = &map->lim_extents[i];

			if (lx_id_overlap(extent.lie_inside, extent.lie_count,
			    old->lie_inside, old->lie_count) ||
			    lx_id_overlap(extent.lie_outside, extent.lie_count,
			    old->lie_outside, old->lie_count))
				return (EINVAL);
		}
		map->lim_extents[map->lim_count++] = extent;
	}
	return (map->lim_count == 0 ? EINVAL : 0);
}

static uint32_t
lx_idmap_range(const lx_idmap_t *map, uint32_t id, uint32_t count, int up)
{
	uint32_t i;

	if (count == 0 || count > UINT32_MAX - id)
		return (LX_ID_INVALID);
	for (i = 0; i < map->lim_count; i++) {
		const lx_id_extent_t *extent = &map->lim_extents[i];
		uint32_t base = up ? extent->lie_outside : extent->lie_inside;
		uint32_t target = up ? extent->lie_inside : extent->lie_outside;

		if (id >= base && id - base < extent->lie_count &&
		    count <= extent->lie_count - (id - base))
			return (target + id - base);
	}
	return (LX_ID_INVALID);
}

uint32_t
lx_idmap_down(const lx_idmap_t *map, uint32_t id)
{
	return (lx_idmap_range(map, id, 1, 0));
}

uint32_t
lx_idmap_up(const lx_idmap_t *map, uint32_t id)
{
	return (lx_idmap_range(map, id, 1, 1));
}

int
lx_idmap_compose(lx_idmap_t *map, const lx_idmap_t *parent)
{
	uint32_t i, j, outside;

	/* Validate every range before changing the caller's temporary map. */
	for (i = 0; i < map->lim_count; i++) {
		lx_id_extent_t *extent = &map->lim_extents[i];

		if (lx_idmap_range(parent, extent->lie_outside,
		    extent->lie_count, 0) == LX_ID_INVALID)
			return (EPERM);
	}
	for (i = 0; i < map->lim_count; i++) {
		lx_id_extent_t *extent = &map->lim_extents[i];

		outside = lx_idmap_range(parent, extent->lie_outside,
		    extent->lie_count, 0);
		extent->lie_outside = outside;
	}
	/* Linux preserves short input order, sorting maps larger than five. */
	if (map->lim_count > 5) {
		for (i = 1; i < map->lim_count; i++) {
			lx_id_extent_t extent = map->lim_extents[i];

			for (j = i; j != 0; j--) {
				if (map->lim_extents[j - 1].lie_inside <=
				    extent.lie_inside)
					break;
				map->lim_extents[j] = map->lim_extents[j - 1];
			}
			map->lim_extents[j] = extent;
		}
	}
	return (0);
}

void
lx_caps_new_namespace(lx_caps_t *caps)
{
	(void) memset(caps, 0, sizeof (*caps));
	caps->lc_effective = caps->lc_permitted = LX_CAP_ALL;
	caps->lc_bounding = LX_CAP_ALL;
}

int
lx_caps_set(lx_caps_t *caps, uint64_t effective, uint64_t permitted,
    uint64_t inheritable)
{
	effective &= LX_CAP_ALL;
	permitted &= LX_CAP_ALL;
	inheritable &= LX_CAP_ALL;
	if ((permitted & ~caps->lc_permitted) != 0 ||
	    (effective & ~permitted) != 0 ||
	    (inheritable & ~(caps->lc_inheritable | caps->lc_bounding)) != 0 ||
	    ((caps->lc_effective & LX_CAP_BIT(LX_CAP_SETPCAP)) == 0 &&
	    (inheritable & ~(caps->lc_inheritable | caps->lc_permitted)) != 0))
		return (EPERM);
	caps->lc_effective = effective;
	caps->lc_permitted = permitted;
	caps->lc_inheritable = inheritable;
	caps->lc_ambient &= permitted & inheritable;
	return (0);
}

int
lx_caps_drop(lx_caps_t *caps, unsigned int capability)
{
	/* Linux checks authority before validating the requested cap number. */
	if ((caps->lc_effective & LX_CAP_BIT(LX_CAP_SETPCAP)) == 0)
		return (EPERM);
	if (capability > LX_CAP_LAST)
		return (EINVAL);
	caps->lc_bounding &= ~LX_CAP_BIT(capability);
	return (0);
}

int
lx_caps_securebits(lx_caps_t *caps, uint32_t bits)
{
	unsigned int bit;

	if ((bits & ~LX_SEC_SUPPORTED) != 0 ||
	    (caps->lc_effective & LX_CAP_BIT(LX_CAP_SETPCAP)) == 0 ||
	    (caps->lc_securebits & LX_SEC_LOCKS & ~bits) != 0)
		return (EPERM);
	for (bit = 1; bit < 8; bit += 2) {
		if ((caps->lc_securebits & (1U << bit)) != 0 &&
		    ((caps->lc_securebits ^ bits) & (1U << (bit - 1))) != 0)
			return (EPERM);
	}
	caps->lc_securebits = bits;
	return (0);
}

int
lx_caps_keep(lx_caps_t *caps, unsigned int keep)
{
	if (keep > 1)
		return (EINVAL);
	if ((caps->lc_securebits & LX_SEC_KEEP_CAPS_LOCKED) != 0)
		return (EPERM);
	caps->lc_securebits &= ~LX_SEC_KEEP_CAPS;
	if (keep != 0)
		caps->lc_securebits |= LX_SEC_KEEP_CAPS;
	return (0);
}

/* Ambient subcommands 1=read, 2=raise, 3=lower, 4=clear all. */
int
lx_caps_ambient(lx_caps_t *caps, unsigned int operation, unsigned int cap)
{
	uint64_t bit;

	if (operation == 4) {
		if (cap != 0)
			return (EINVAL);
		caps->lc_ambient = 0;
		return (0);
	}
	if (cap > LX_CAP_LAST || operation < 1 || operation > 3)
		return (EINVAL);
	bit = LX_CAP_BIT(cap);
	if (operation == 1)
		return ((caps->lc_ambient & bit) != 0);
	if (operation == 2) {
		if ((caps->lc_securebits & LX_SEC_NO_AMBIENT_RAISE) != 0 ||
		    (caps->lc_permitted & caps->lc_inheritable & bit) == 0)
			return (EPERM);
		caps->lc_ambient |= bit;
	} else {
		caps->lc_ambient &= ~bit;
	}
	return (0);
}

/* Ordinary exec only: setid and file-capability exec need separate checks. */
void
lx_caps_exec_plain(lx_caps_t *caps, int real_root, int effective_root, int nnp)
{
	uint64_t permitted = 0;

	if ((caps->lc_securebits & LX_SEC_NOROOT) == 0 &&
	    (real_root || effective_root))
		permitted = caps->lc_inheritable | caps->lc_bounding;
	if (nnp)
		permitted &= caps->lc_permitted;
	caps->lc_permitted = permitted | caps->lc_ambient;
	caps->lc_effective = effective_root &&
	    (caps->lc_securebits & LX_SEC_NOROOT) == 0 ?
	    caps->lc_permitted : caps->lc_ambient;
	caps->lc_securebits &= ~LX_SEC_KEEP_CAPS;
}

/* Root membership masks: bit0 real, bit1 effective, bit2 saved. */
void
lx_caps_uid_change(lx_caps_t *caps, unsigned int old, unsigned int new)
{
	if ((caps->lc_securebits & LX_SEC_NO_SETUID_FIXUP) != 0)
		return;
	if (old != 0 && new == 0) {
		if ((caps->lc_securebits & LX_SEC_KEEP_CAPS) == 0)
			caps->lc_permitted = 0;
		caps->lc_effective = caps->lc_ambient = 0;
	}
	if ((old & 2) != 0 && (new & 2) == 0)
		caps->lc_effective = 0;
	if ((old & 2) == 0 && (new & 2) != 0)
		caps->lc_effective = caps->lc_permitted;
}

int
lx_userns_capable(const lx_userns_actor_t *actor,
    const lx_userns_model_t *target, unsigned int cap)
{
	const lx_userns_model_t *ns;

	if (cap > LX_CAP_LAST || actor->lua_ns == NULL)
		return (0);
	for (ns = target; ns != NULL; ns = ns->lum_parent) {
		if (actor->lua_ns == ns)
			return ((actor->lua_effective & LX_CAP_BIT(cap)) != 0);
		if (actor->lua_ns == ns->lum_parent &&
		    actor->lua_uid == ns->lum_owner)
			return (1);
	}
	return (0);
}

void
lx_userns_model_root(lx_userns_model_t *ns)
{
	(void) memset(ns, 0, sizeof (*ns));
	lx_idmap_identity(&ns->lum_uids);
	lx_idmap_identity(&ns->lum_gids);
	ns->lum_setgroups = 1;
}

int
lx_userns_model_child(lx_userns_model_t *ns, const lx_userns_actor_t *creator)
{
	const lx_userns_model_t *parent = creator->lua_ns;

	if (parent == NULL ||
	    lx_idmap_up(&parent->lum_uids, creator->lua_uid) == LX_ID_INVALID ||
	    lx_idmap_up(&parent->lum_gids, creator->lua_gid) == LX_ID_INVALID)
		return (EPERM);
	if (parent->lum_level > 32)
		return (ENOSPC);
	(void) memset(ns, 0, sizeof (*ns));
	ns->lum_parent = parent;
	ns->lum_level = parent->lum_level + 1;
	ns->lum_owner = creator->lua_uid;
	ns->lum_setgroups = parent->lum_setgroups;
	ns->lum_creator_setfcap =
	    (creator->lua_effective & LX_CAP_BIT(LX_CAP_SETFCAP)) != 0;
	return (0);
}

int
lx_userns_model_write_check(const lx_userns_model_t *ns, int gid,
    const lx_userns_actor_t *opener)
{
	const lx_idmap_t *map = gid ? &ns->lum_gids : &ns->lum_uids;

	if (ns->lum_parent == NULL || (opener->lua_ns != ns &&
	    opener->lua_ns != ns->lum_parent) || map->lim_count != 0 ||
	    !lx_userns_capable(opener, ns, LX_CAP_SYS_ADMIN))
		return (EPERM);
	return (0);
}

/* Input map is temporary and still names IDs relative to the parent. */
int
lx_userns_model_write(lx_userns_model_t *ns, int gid,
    const lx_userns_actor_t *opener, const lx_userns_actor_t *current,
    lx_idmap_t *map)
{
	const lx_idmap_t *parent;
	lx_idmap_t *target;
	uint32_t i, identity;
	unsigned int cap = gid ? LX_CAP_SETGID : LX_CAP_SETUID;
	int permitted = 0, error;

	if ((error = lx_userns_model_write_check(ns, gid, opener)) != 0)
		return (error);
	target = gid ? &ns->lum_gids : &ns->lum_uids;
	parent = gid ? &ns->lum_parent->lum_gids : &ns->lum_parent->lum_uids;
	if (map->lim_count == 0 || map->lim_count > LX_IDMAP_EXTENTS)
		return (EINVAL);
	if (!gid) {
		for (i = 0; i < map->lim_count; i++) {
			if (map->lim_extents[i].lie_outside != 0)
				continue;
			if (opener->lua_ns == ns ? !ns->lum_creator_setfcap :
			    !lx_userns_capable(opener, ns->lum_parent,
			    LX_CAP_SETFCAP))
				return (EPERM);
		}
	}
	if (map->lim_count == 1 && map->lim_extents[0].lie_count == 1 &&
	    ns->lum_owner == opener->lua_uid) {
		identity = lx_idmap_down(parent,
		    map->lim_extents[0].lie_outside);
		permitted = gid ? !ns->lum_setgroups &&
		    identity == opener->lua_gid : identity == opener->lua_uid;
	}
	if (!permitted &&
	    !(lx_userns_capable(opener, ns->lum_parent, cap) &&
	    lx_userns_capable(current, ns->lum_parent, cap)))
		return (EPERM);
	if ((error = lx_idmap_compose(map, parent)) != 0)
		return (error);
	(void) memcpy(target, map, sizeof (*map));
	return (0);
}

int
lx_userns_model_groups(lx_userns_model_t *ns,
    const lx_userns_actor_t *opener, int allow)
{
	if (!lx_userns_capable(opener, ns, LX_CAP_SYS_ADMIN))
		return (EPERM);
	if (allow) {
		if (!ns->lum_setgroups)
			return (EPERM);
	} else {
		if (ns->lum_gids.lim_count != 0)
			return (EPERM);
		ns->lum_setgroups = 0;
	}
	return (0);
}
