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

/* /etc/system: set lx_brand:lx_netns_experimental = 1 (lab only). */
int lx_netns_experimental = 0;

struct lx_netns {
	uint_t ln_refs;
	lx_userns_t *ln_userns;
	netstack_t *ln_stack;
};

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
