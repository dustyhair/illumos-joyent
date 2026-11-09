/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * CDDL HEADER END
 */

/*
 * Copyright 2026 Jonathan Wagner.
 */

#include <sys/types.h>
#include <sys/atomic.h>
#include <sys/debug.h>
#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/systm.h>
#include <sys/cred.h>
#include <sys/netstack.h>
#include <sys/proc.h>
#include <sys/zone.h>
#include <sys/lx_brand.h>
#include <sys/lx_cap.h>
#include <sys/lx_netns.h>
#include <sys/lx_userns.h>
#include <inet/ip.h>
#include <inet/ip_if.h>
#include <net/if.h>

#define	LX_NETNS_TUN_ROUTE_MAX	4096U

/* /etc/system: set lx_brand:lx_netns_experimental = 1 (lab only). */
int lx_netns_experimental = 0;

struct lx_netns {
	uint_t ln_refs;
	lx_userns_t *ln_userns;
	netstack_t *ln_stack;
};

typedef struct lx_tun_route {
	struct lx_tun_route *ltr_next;
	char ltr_name[IFNAMSIZ];
	uint32_t ltr_dst;
	uint32_t ltr_mask;
} lx_tun_route_t;

/* Linux creates every network namespace with one loopback device. */
static int
lx_netns_loopback_create(netstack_t *stack)
{
	ill_t *ill;
	boolean_t allocated = B_FALSE;

	VERIFY(stack->netstack_ip != NULL);
	ill = ill_lookup_on_name(ipif_loopback_name, B_TRUE, B_FALSE,
	    &allocated, stack->netstack_ip);
	if (ill == NULL)
		return (ENOMEM);
	ill_refrele(ill);

	allocated = B_FALSE;
	ill = ill_lookup_on_name(ipif_loopback_name, B_TRUE, B_TRUE,
	    &allocated, stack->netstack_ip);
	if (ill == NULL)
		return (ENOMEM);
	ill_refrele(ill);
	return (0);
}

boolean_t
lx_netns_entry_enabled(void)
{
	return (lx_netns_experimental != 0);
}

int
lx_netns_create(const cred_t *owner, lx_netns_t **result)
{
	lx_userns_cred_t actor;
	lx_userns_t *root;
	lx_netns_t *ns;
	int error;

	*result = NULL;
	if (!lx_userns_cred_capable(owner, owner, LX_CAP_SYS_ADMIN))
		return (EPERM);
	root = ztolxzd(crgetzone(owner))->lxzd_userns;
	if ((error = lx_userns_map_actor(owner, root, &actor)) != 0)
		return (error);
	if ((error = lx_userns_netns_charge(actor.luc_ns)) != 0)
		return (error);

	ns = kmem_zalloc(sizeof (*ns), KM_SLEEP);
	ns->ln_refs = 1;
	ns->ln_userns = actor.luc_ns;
	lx_userns_hold(ns->ln_userns);
	ns->ln_stack = netstack_create_detached(crgetzoneid(owner));
	VERIFY(ns->ln_stack != NULL);
	if ((error = lx_netns_loopback_create(ns->ln_stack)) != 0) {
		netstack_destroy_detached(ns->ln_stack);
		lx_userns_netns_uncharge(ns->ln_userns);
		lx_userns_rele(ns->ln_userns);
		kmem_free(ns, sizeof (*ns));
		return (error);
	}
	*result = ns;
	return (0);
}

void
lx_netns_hold(lx_netns_t *ns)
{
	VERIFY(ns != NULL);
	VERIFY3U(ns->ln_refs, !=, 0);
	atomic_inc_uint(&ns->ln_refs);
}

void
lx_netns_rele(lx_netns_t *ns)
{
	if (ns == NULL || atomic_dec_uint_nv(&ns->ln_refs) != 0)
		return;

	netstack_destroy_detached(ns->ln_stack);
	lx_userns_netns_uncharge(ns->ln_userns);
	lx_userns_rele(ns->ln_userns);
	kmem_free(ns, sizeof (*ns));
}

netstack_t *
lx_netns_stack(lx_netns_t *ns)
{
	return (ns != NULL ? ns->ln_stack : NULL);
}

netstack_t *
lx_netns_current(proc_t *p)
{
	lx_proc_data_t *pd = ptolxproc(p);

	return (pd != NULL ? lx_netns_stack(pd->l_netns) : NULL);
}

/*
 * Network configuration in a detached stack is governed by the user
 * namespace that owns it.  Native PRIV_SYS_IP_CONFIG is deliberately not
 * required: Linux permits a task with CAP_NET_ADMIN in the owning user
 * namespace to configure that namespace's devices.
 */
boolean_t
lx_netns_configure_allowed(const cred_t *cred)
{
	lx_proc_data_t *pd = ptolxproc(curproc);
	lx_userns_cred_t actor;
	lx_netns_t *ns;

	if (pd == NULL || (ns = pd->l_netns) == NULL)
		return (B_FALSE);
	if (lx_userns_map_actor(cred, ns->ln_userns, &actor) != 0)
		return (B_FALSE);
	return (lx_userns_has_cap(&actor, ns->ln_userns,
	    LX_CAP_NET_ADMIN));
}

/*
 * TUN plumbing currently targets the zone's initial network stack.  Do not
 * let a task in a detached network namespace escape back into that stack.
 * Detached-stack TUN support will require passing the target netstack through
 * the private driver interface rather than identifying it by zone alone.
 */
boolean_t
lx_netns_tun_configure_allowed(const cred_t *cred)
{
	lx_proc_data_t *pd = ptolxproc(curproc);
	lx_userns_cred_t actor;
	lx_userns_t *owner;

	if (pd == NULL)
		return (B_FALSE);
	if (pd->l_netns != NULL)
		return (B_FALSE);

	owner = ztolxzd(crgetzone(cred))->lxzd_userns;
	if (lx_userns_map_actor(cred, owner, &actor) != 0)
		return (B_FALSE);
	return (lx_userns_has_cap(&actor, owner, LX_CAP_NET_ADMIN));
}

/*
 * Linux permits the first SO_BINDTODEVICE on a socket without a capability,
 * but requires CAP_NET_RAW in the network namespace's owning user namespace
 * before an existing binding can be changed or removed.
 */
boolean_t
lx_netns_bind_allowed(const cred_t *cred)
{
	lx_proc_data_t *pd = ptolxproc(curproc);
	lx_userns_cred_t actor;
	lx_userns_t *owner;

	if (pd == NULL)
		return (B_FALSE);
	if (pd->l_netns != NULL)
		owner = pd->l_netns->ln_userns;
	else
		owner = ztolxzd(crgetzone(cred))->lxzd_userns;
	if (lx_userns_map_actor(cred, owner, &actor) != 0)
		return (B_FALSE);
	return (lx_userns_has_cap(&actor, owner, LX_CAP_NET_RAW));
}

/*
 * illumos cannot raise an addressless IPv4 point-to-point interface, while
 * Linux permits TUN interfaces to be administratively up before they have an
 * address.  Preserve that Linux-visible state until address configuration can
 * apply it to the native interface.  The TUN driver currently limits PPAs to
 * 0 through 19; a 32-bit bitmap leaves room without coupling BrandZ to the
 * driver's private limit.
 */
static int
lx_netns_tun_id(const char *name)
{
	const char *p;
	uint_t id = 0;

	if (strncmp(name, "tun", 3) != 0 || name[3] == '\0')
		return (-1);
	for (p = name + 3; *p != '\0'; p++) {
		if (*p < '0' || *p > '9')
			return (-1);
		id = id * 10 + (*p - '0');
		if (id >= 32)
			return (-1);
	}
	return ((int)id);
}

void
lx_netns_tun_link_set(const char *name, boolean_t up)
{
	lx_proc_data_t *pd = ptolxproc(curproc);
	lx_zone_data_t *zd;
	uint32_t bit;
	int id;

	if (pd == NULL || pd->l_netns != NULL ||
	    (id = lx_netns_tun_id(name)) < 0)
		return;
	zd = ztolxzd(curproc->p_zone);
	VERIFY(zd != NULL);
	bit = 1U << id;
	mutex_enter(&zd->lxzd_lock);
	if (up)
		zd->lxzd_tun_link_up |= bit;
	else
		zd->lxzd_tun_link_up &= ~bit;
	mutex_exit(&zd->lxzd_lock);
}

void
lx_netns_tun_link_reset(const char *name)
{
	lx_proc_data_t *pd = ptolxproc(curproc);
	lx_tun_route_t **prev, *route;
	lx_zone_data_t *zd;
	uint32_t bit;
	int id;

	if (pd == NULL || pd->l_netns != NULL ||
	    (id = lx_netns_tun_id(name)) < 0)
		return;
	zd = ztolxzd(curproc->p_zone);
	VERIFY(zd != NULL);
	bit = 1U << id;
	mutex_enter(&zd->lxzd_lock);
	zd->lxzd_tun_link_up &= ~bit;
	prev = &zd->lxzd_tun_routes;
	while ((route = *prev) != NULL) {
		if (strcmp(route->ltr_name, name) != 0) {
			prev = &route->ltr_next;
			continue;
		}
		*prev = route->ltr_next;
		zd->lxzd_tun_route_count--;
		kmem_free(route, sizeof (*route));
	}
	mutex_exit(&zd->lxzd_lock);
}

boolean_t
lx_netns_tun_link_is_up(const char *name)
{
	lx_proc_data_t *pd = ptolxproc(curproc);
	lx_zone_data_t *zd;
	boolean_t up;
	uint32_t bit;
	int id;

	if (pd == NULL || pd->l_netns != NULL ||
	    (id = lx_netns_tun_id(name)) < 0)
		return (B_FALSE);
	zd = ztolxzd(curproc->p_zone);
	VERIFY(zd != NULL);
	bit = 1U << id;
	mutex_enter(&zd->lxzd_lock);
	up = (zd->lxzd_tun_link_up & bit) != 0;
	mutex_exit(&zd->lxzd_lock);
	return (up);
}

int
lx_netns_tun_route_defer(const char *name, uint32_t dst, uint32_t mask)
{
	lx_proc_data_t *pd = ptolxproc(curproc);
	lx_tun_route_t *new, *route;
	lx_zone_data_t *zd;

	if (pd == NULL || pd->l_netns != NULL ||
	    lx_netns_tun_id(name) < 0)
		return (EINVAL);
	zd = ztolxzd(curproc->p_zone);
	VERIFY(zd != NULL);
	new = kmem_alloc(sizeof (*new), KM_SLEEP);
	(void) strlcpy(new->ltr_name, name, sizeof (new->ltr_name));
	new->ltr_dst = dst;
	new->ltr_mask = mask;

	mutex_enter(&zd->lxzd_lock);
	for (route = zd->lxzd_tun_routes; route != NULL;
	    route = route->ltr_next) {
		if (strcmp(route->ltr_name, name) == 0 &&
		    route->ltr_dst == dst && route->ltr_mask == mask) {
			mutex_exit(&zd->lxzd_lock);
			kmem_free(new, sizeof (*new));
			return (0);
		}
	}
	if (zd->lxzd_tun_route_count >= LX_NETNS_TUN_ROUTE_MAX) {
		mutex_exit(&zd->lxzd_lock);
		kmem_free(new, sizeof (*new));
		return (ENOSPC);
	}
	new->ltr_next = zd->lxzd_tun_routes;
	zd->lxzd_tun_routes = new;
	zd->lxzd_tun_route_count++;
	mutex_exit(&zd->lxzd_lock);
	return (0);
}

boolean_t
lx_netns_tun_route_take(const char *name, uint32_t *dst, uint32_t *mask)
{
	lx_proc_data_t *pd = ptolxproc(curproc);
	lx_tun_route_t **prev, *route;
	lx_zone_data_t *zd;

	if (pd == NULL || pd->l_netns != NULL ||
	    lx_netns_tun_id(name) < 0)
		return (B_FALSE);
	zd = ztolxzd(curproc->p_zone);
	VERIFY(zd != NULL);
	mutex_enter(&zd->lxzd_lock);
	prev = &zd->lxzd_tun_routes;
	while ((route = *prev) != NULL &&
	    strcmp(route->ltr_name, name) != 0)
		prev = &route->ltr_next;
	if (route == NULL) {
		mutex_exit(&zd->lxzd_lock);
		return (B_FALSE);
	}
	*prev = route->ltr_next;
	zd->lxzd_tun_route_count--;
	mutex_exit(&zd->lxzd_lock);
	*dst = route->ltr_dst;
	*mask = route->ltr_mask;
	kmem_free(route, sizeof (*route));
	return (B_TRUE);
}

boolean_t
lx_netns_tun_route_cancel(const char *name, uint32_t dst, uint32_t mask)
{
	lx_proc_data_t *pd = ptolxproc(curproc);
	lx_tun_route_t **prev, *route;
	lx_zone_data_t *zd;

	if (pd == NULL || pd->l_netns != NULL ||
	    lx_netns_tun_id(name) < 0)
		return (B_FALSE);
	zd = ztolxzd(curproc->p_zone);
	VERIFY(zd != NULL);
	mutex_enter(&zd->lxzd_lock);
	prev = &zd->lxzd_tun_routes;
	while ((route = *prev) != NULL) {
		if (strcmp(route->ltr_name, name) == 0 &&
		    route->ltr_dst == dst && route->ltr_mask == mask)
			break;
		prev = &route->ltr_next;
	}
	if (route == NULL) {
		mutex_exit(&zd->lxzd_lock);
		return (B_FALSE);
	}
	*prev = route->ltr_next;
	zd->lxzd_tun_route_count--;
	mutex_exit(&zd->lxzd_lock);
	kmem_free(route, sizeof (*route));
	return (B_TRUE);
}

void
lx_netns_tun_fini(zone_t *zone)
{
	lx_tun_route_t *routes, *route;
	lx_zone_data_t *zd = ztolxzd(zone);

	VERIFY(zd != NULL);
	mutex_enter(&zd->lxzd_lock);
	routes = zd->lxzd_tun_routes;
	zd->lxzd_tun_routes = NULL;
	zd->lxzd_tun_route_count = 0;
	mutex_exit(&zd->lxzd_lock);
	while ((route = routes) != NULL) {
		routes = route->ltr_next;
		kmem_free(route, sizeof (*route));
	}
}
