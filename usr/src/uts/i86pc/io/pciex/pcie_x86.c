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
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#include <sys/types.h>
#include <sys/ddi.h>
#include <sys/kmem.h>
#include <sys/sysmacros.h>
#include <sys/sunddi.h>
#include <sys/sunpm.h>
#include <sys/epm.h>
#include <sys/sunndi.h>
#include <sys/ddi_impldefs.h>
#include <sys/ddi_implfuncs.h>
#include <sys/pcie.h>
#include <sys/pcie_impl.h>
#include <sys/pcie_pwr.h>
#include <sys/pcie_acpi.h>	/* pcie_x86_priv_t */

/*
 * Preserve firmware-established PCIe config space until SmartOS has explicit
 * ownership for the relevant native service.
 */
int pcie_x86_preserve_firmware_handoff = 1;

static pcie_x86_priv_t *
pcie_x86_bus_priv(dev_info_t *dip)
{
	pcie_bus_t *bus_p = PCIE_DIP2BUS(dip);

	if (bus_p == NULL)
		return (NULL);

	return ((pcie_x86_priv_t *)bus_p->bus_plat_private);
}

static pcie_x86_priv_t *
pcie_x86_find_osc_owner(dev_info_t *dip)
{
	dev_info_t *cur;

	for (cur = dip; cur != NULL; cur = ddi_get_parent(cur)) {
		pcie_x86_priv_t *osc_p = pcie_x86_bus_priv(cur);

		if (osc_p != NULL && osc_p->bus_osc)
			return (osc_p);
	}

	return (NULL);
}

static void
pcie_x86_init_osc(dev_info_t *dip)
{
	pcie_bus_t *bus_p = PCIE_DIP2BUS(dip);
	uint32_t osc_flags = OSC_CONTROL_PCIE_NAT_HP | OSC_CONTROL_PCIE_PME;

	if (bus_p == NULL || !PCIE_IS_RP(bus_p) || pcie_is_osc(dip))
		return;

	(void) pcie_acpi_osc(dip, &osc_flags);
}

void
pcie_apply_plat_props(dev_info_t *dip)
{
	(void) pcie_acpi_program_hp_params(dip);
}

void
pcie_init_plat(dev_info_t *dip)
{
	pcie_bus_t	*bus_p = PCIE_DIP2BUS(dip);
	bus_p->bus_plat_private =
	    (pcie_x86_priv_t *)kmem_zalloc(sizeof (pcie_x86_priv_t), KM_SLEEP);
	pcie_x86_init_osc(dip);
}

void
pcie_fini_plat(dev_info_t *dip)
{
	pcie_bus_t	*bus_p = PCIE_DIP2BUS(dip);

	kmem_free(bus_p->bus_plat_private, sizeof (pcie_x86_priv_t));
}

boolean_t
pcie_plat_preserve_config(dev_info_t *dip)
{
	if (!pcie_x86_preserve_firmware_handoff)
		return (B_FALSE);

	return (pcie_x86_find_osc_owner(dip) != NULL);
}

boolean_t
pcie_plat_owns_pcie_caps(dev_info_t *dip)
{
	pcie_x86_priv_t *osc_p;

	if (!pcie_x86_preserve_firmware_handoff)
		return (B_TRUE);

	osc_p = pcie_x86_find_osc_owner(dip);
	if (osc_p == NULL)
		return (B_TRUE);

	if (!osc_p->bus_osc_valid)
		return (B_FALSE);

	return ((osc_p->bus_osc_ctrl & OSC_CONTROL_PCIE_CAPS) != 0);
}

boolean_t
pcie_plat_owns_aer(dev_info_t *dip)
{
	pcie_x86_priv_t *osc_p;

	if (!pcie_x86_preserve_firmware_handoff)
		return (B_TRUE);

	osc_p = pcie_x86_find_osc_owner(dip);
	if (osc_p == NULL)
		return (B_TRUE);

	if (!osc_p->bus_osc_valid)
		return (B_FALSE);

	return ((osc_p->bus_osc_ctrl & (OSC_CONTROL_PCIE_CAPS |
	    OSC_CONTROL_PCIE_ADV_ERR)) ==
	    (OSC_CONTROL_PCIE_CAPS | OSC_CONTROL_PCIE_ADV_ERR));
}

/* ARGSUSED */
int
pcie_plat_pwr_setup(dev_info_t *dip)
{
	return (DDI_SUCCESS);
}

/*
 * Undo whatever is done in pcie_plat_pwr_common_setup
 */
/* ARGSUSED */
void
pcie_plat_pwr_teardown(dev_info_t *dip)
{
}
