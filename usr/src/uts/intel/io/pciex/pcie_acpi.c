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
#include <sys/sysmacros.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/kmem.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/sunndi.h>
#include <sys/promif.h>
#include <sys/pcie.h>
#include <sys/pci_cap.h>
#include <sys/pcie_impl.h>
#include <sys/pcie_acpi.h>
#include <sys/acpi/acpi.h>
#include <sys/acpica.h>

ACPI_STATUS pcie_acpi_eval_osc(dev_info_t *dip, ACPI_HANDLE osc_hdl,
	uint32_t *osc_flags);
static ACPI_STATUS pcie_acpi_find_method(ACPI_HANDLE busobj,
	const char *method, ACPI_HANDLE *hdlp);
static ACPI_STATUS pcie_acpi_find_osc(ACPI_HANDLE busobj,
	ACPI_HANDLE *osc_hdlp);

typedef struct pcie_acpi_hpx_type0 {
	uint32_t	revision;
	uint8_t		cache_line_size;
	uint8_t		latency_timer;
	uint8_t		enable_serr;
	uint8_t		enable_perr;
} pcie_acpi_hpx_type0_t;

typedef struct pcie_acpi_hpx_type2 {
	uint32_t	revision;
	uint32_t	unc_err_mask_and;
	uint32_t	unc_err_mask_or;
	uint32_t	unc_err_sever_and;
	uint32_t	unc_err_sever_or;
	uint32_t	cor_err_mask_and;
	uint32_t	cor_err_mask_or;
	uint32_t	adv_err_cap_and;
	uint32_t	adv_err_cap_or;
	uint16_t	pci_exp_devctl_and;
	uint16_t	pci_exp_devctl_or;
	uint16_t	pci_exp_lnkctl_and;
	uint16_t	pci_exp_lnkctl_or;
	uint32_t	sec_unc_err_sever_and;
	uint32_t	sec_unc_err_sever_or;
	uint32_t	sec_unc_err_mask_and;
	uint32_t	sec_unc_err_mask_or;
} pcie_acpi_hpx_type2_t;

static ACPI_STATUS pcie_acpi_decode_hpx_type0(ACPI_OBJECT *record,
	pcie_acpi_hpx_type0_t *hpx0);
static ACPI_STATUS pcie_acpi_decode_hpx_type2(ACPI_OBJECT *record,
	pcie_acpi_hpx_type2_t *hpx2);
static void pcie_acpi_program_hpx_type0(ddi_acc_handle_t cfg_hdl,
	const pcie_acpi_hpx_type0_t *hpx0);
static void pcie_acpi_program_hpx_type2(ddi_acc_handle_t cfg_hdl,
	const pcie_acpi_hpx_type2_t *hpx2);
static ACPI_STATUS pcie_acpi_run_hpx(ddi_acc_handle_t cfg_hdl,
	ACPI_HANDLE handle);
static ACPI_STATUS pcie_acpi_run_hpp(ddi_acc_handle_t cfg_hdl,
	ACPI_HANDLE handle);

#ifdef DEBUG
static void pcie_dump_acpi_obj(ACPI_HANDLE pcibus_obj);
static ACPI_STATUS pcie_walk_obj_namespace(ACPI_HANDLE hdl, uint32_t nl,
	void *context, void **ret);
static ACPI_STATUS pcie_print_acpi_name(ACPI_HANDLE hdl, uint32_t nl,
	void *context, void **ret);
#endif /* DEBUG */

int
pcie_acpi_osc(dev_info_t *dip, uint32_t *osc_flags)
{
	ACPI_HANDLE pcibus_obj;
	int status = AE_ERROR;
	ACPI_HANDLE osc_hdl;
	pcie_bus_t *bus_p = PCIE_DIP2BUS(dip);
	pcie_x86_priv_t *osc_p = (pcie_x86_priv_t *)bus_p->bus_plat_private;

	/* Mark this so we know _OSC has been called for this device */
	osc_p->bus_osc = B_TRUE;
	osc_p->bus_osc_valid = B_FALSE;
	osc_p->bus_osc_ctrl = 0;
	osc_p->bus_osc_hp = B_FALSE;
	osc_p->bus_osc_aer = B_FALSE;

	status = acpica_get_handle(dip, &pcibus_obj);
	if (status != AE_OK) {
		PCIE_DBG("No ACPI device found (dip %p)\n", (void *)dip);
		return (DDI_FAILURE);
	}

	if (pcie_acpi_find_osc(pcibus_obj, &osc_hdl) != AE_OK) {
		PCIE_DBG("no _OSC method present for dip %p\n", (void *)dip);
		return (DDI_FAILURE);
	}

	if (pcie_acpi_eval_osc(dip, osc_hdl, osc_flags) != AE_OK) {
		PCIE_DBG("Failed to evaluate _OSC method for dip 0x%p\n",
		    (void *)dip);
		return (DDI_FAILURE);
	}

	osc_p->bus_osc_hp = (*osc_flags & OSC_CONTROL_PCIE_NAT_HP) ?
	    B_TRUE : B_FALSE;
	osc_p->bus_osc_aer = (*osc_flags & OSC_CONTROL_PCIE_ADV_ERR) ?
	    B_TRUE : B_FALSE;
	osc_p->bus_osc_ctrl = *osc_flags;
	osc_p->bus_osc_valid = B_TRUE;

#ifdef DEBUG
	if (pcie_debug_flags > 1)
		pcie_dump_acpi_obj(pcibus_obj);
#endif /* DEBUG */

	return (DDI_SUCCESS);
}

static ACPI_STATUS
pcie_acpi_find_osc(ACPI_HANDLE busobj, ACPI_HANDLE *osc_hdlp)
{
	return (pcie_acpi_find_method(busobj, "_OSC", osc_hdlp));
}

static ACPI_STATUS
pcie_acpi_find_method(ACPI_HANDLE busobj, const char *method,
    ACPI_HANDLE *hdlp)
{
	ACPI_HANDLE parentobj = busobj;
	ACPI_STATUS status = AE_NOT_FOUND;

	*hdlp = NULL;

	do {
		busobj = parentobj;
		if ((status = AcpiGetHandle(busobj, (char *)method, hdlp)) ==
		    AE_OK)
			break;
	} while (AcpiGetParent(busobj, &parentobj) == AE_OK);

	if (*hdlp == NULL)
		status = AE_NOT_FOUND;

	return (status);
}

/* UUID for for PCI/PCI-X/PCI-Exp hierarchy as defined in PCI fw ver 3.0 */
static uint8_t pcie_uuid[16] =
	{0x5b, 0x4d, 0xdb, 0x33, 0xf7, 0x1f, 0x1c, 0x40,
	0x96, 0x57, 0x74, 0x41, 0xc0, 0x3d, 0xd7, 0x66};

ACPI_STATUS
pcie_acpi_eval_osc(dev_info_t *dip, ACPI_HANDLE osc_hdl, uint32_t *osc_flags)
{
	ACPI_STATUS		status;
	ACPI_OBJECT_LIST	arglist;
	ACPI_OBJECT		args[4];
	UINT32			caps_buffer[3];
	ACPI_BUFFER		rb;
	UINT32			*rbuf;

	arglist.Count = 4;
	arglist.Pointer = args;

	args[0].Type = ACPI_TYPE_BUFFER;
	args[0].Buffer.Length = 16;
	args[0].Buffer.Pointer = pcie_uuid;

	args[1].Type = ACPI_TYPE_INTEGER;
	args[1].Integer.Value = PCIE_OSC_REVISION_ID;

	args[2].Type = ACPI_TYPE_INTEGER;
	args[2].Integer.Value = 3;

	args[3].Type = ACPI_TYPE_BUFFER;
	args[3].Buffer.Length = 12;
	args[3].Buffer.Pointer = (void *)caps_buffer;

	caps_buffer[0] = 0;
	caps_buffer[1] = OSC_SUPPORT_FIELD_INIT;
	caps_buffer[2] = *osc_flags | OSC_CONTROL_FIELD_INIT;

	if (caps_buffer[2] & OSC_CONTROL_PCIE_NAT_HP)
		caps_buffer[2] |= (OSC_CONTROL_PCIE_NAT_HP |
		    OSC_CONTROL_PCIE_NAT_PM);

	rb.Length = ACPI_ALLOCATE_BUFFER;
	rb.Pointer = NULL;

	status = AcpiEvaluateObjectTyped(osc_hdl, NULL, &arglist, &rb,
	    ACPI_TYPE_BUFFER);
	if (status != AE_OK) {
		PCIE_DBG("Failed to execute _OSC method (status %d)\n",
		    status);
		return (status);
	}

	/* LINTED pointer alignment */
	rbuf = (UINT32 *)((ACPI_OBJECT *)rb.Pointer)->Buffer.Pointer;

	if (rbuf[0] & OSC_STATUS_ERRORS) {
		PCIE_DBG("_OSC method failed (STATUS %d)\n", rbuf[0]);
		AcpiOsFree(rb.Pointer);
		return (AE_ERROR);
	}

	*osc_flags = rbuf[2];

	PCIE_DBG("_OSC method evaluation completed for 0x%p: "
	    "STATUS 0x%x SUPPORT 0x%x CONTROL req 0x%x, CONTROL ret 0x%x\n",
	    (void *)dip, rbuf[0], rbuf[1], caps_buffer[2], rbuf[2]);

	AcpiOsFree(rb.Pointer);

	return (AE_OK);
}

static ACPI_STATUS
pcie_acpi_decode_hpx_type0(ACPI_OBJECT *record, pcie_acpi_hpx_type0_t *hpx0)
{
	ACPI_OBJECT *fields = record->Package.Elements;
	int i;

	if (record->Type != ACPI_TYPE_PACKAGE || record->Package.Count != 6)
		return (AE_ERROR);

	for (i = 0; i < 6; i++) {
		if (fields[i].Type != ACPI_TYPE_INTEGER)
			return (AE_ERROR);
	}

	if (fields[1].Integer.Value != 1)
		return (AE_SUPPORT);

	hpx0->revision = fields[1].Integer.Value;
	hpx0->cache_line_size = fields[2].Integer.Value;
	hpx0->latency_timer = fields[3].Integer.Value;
	hpx0->enable_serr = fields[4].Integer.Value;
	hpx0->enable_perr = fields[5].Integer.Value;

	return (AE_OK);
}

static ACPI_STATUS
pcie_acpi_decode_hpx_type2(ACPI_OBJECT *record, pcie_acpi_hpx_type2_t *hpx2)
{
	ACPI_OBJECT *fields = record->Package.Elements;
	int i;

	if (record->Type != ACPI_TYPE_PACKAGE || record->Package.Count != 18)
		return (AE_ERROR);

	for (i = 0; i < 18; i++) {
		if (fields[i].Type != ACPI_TYPE_INTEGER)
			return (AE_ERROR);
	}

	if (fields[1].Integer.Value != 1)
		return (AE_SUPPORT);

	hpx2->revision = fields[1].Integer.Value;
	hpx2->unc_err_mask_and = fields[2].Integer.Value;
	hpx2->unc_err_mask_or = fields[3].Integer.Value;
	hpx2->unc_err_sever_and = fields[4].Integer.Value;
	hpx2->unc_err_sever_or = fields[5].Integer.Value;
	hpx2->cor_err_mask_and = fields[6].Integer.Value;
	hpx2->cor_err_mask_or = fields[7].Integer.Value;
	hpx2->adv_err_cap_and = fields[8].Integer.Value;
	hpx2->adv_err_cap_or = fields[9].Integer.Value;
	hpx2->pci_exp_devctl_and = fields[10].Integer.Value;
	hpx2->pci_exp_devctl_or = fields[11].Integer.Value;
	hpx2->pci_exp_lnkctl_and = fields[12].Integer.Value;
	hpx2->pci_exp_lnkctl_or = fields[13].Integer.Value;
	hpx2->sec_unc_err_sever_and = fields[14].Integer.Value;
	hpx2->sec_unc_err_sever_or = fields[15].Integer.Value;
	hpx2->sec_unc_err_mask_and = fields[16].Integer.Value;
	hpx2->sec_unc_err_mask_or = fields[17].Integer.Value;

	return (AE_OK);
}

static void
pcie_acpi_program_hpx_type0(ddi_acc_handle_t cfg_hdl,
    const pcie_acpi_hpx_type0_t *hpx0)
{
	uint8_t header;
	uint16_t cmd;
	uint16_t bctl;

	pci_config_put8(cfg_hdl, PCI_CONF_CACHE_LINESZ, hpx0->cache_line_size);
	pci_config_put8(cfg_hdl, PCI_CONF_LATENCY_TIMER, hpx0->latency_timer);

	cmd = pci_config_get16(cfg_hdl, PCI_CONF_COMM);
	if (hpx0->enable_serr != 0)
		cmd |= PCI_COMM_SERR_ENABLE;
	if (hpx0->enable_perr != 0)
		cmd |= PCI_COMM_PARITY_DETECT;
	pci_config_put16(cfg_hdl, PCI_CONF_COMM, cmd);

	header = pci_config_get8(cfg_hdl, PCI_CONF_HEADER) & PCI_HEADER_TYPE_M;
	if (header != PCI_HEADER_PPB)
		return;

	pci_config_put8(cfg_hdl, PCI_BCNF_LATENCY_TIMER, hpx0->latency_timer);

	bctl = pci_config_get16(cfg_hdl, PCI_BCNF_BCNTRL);
	if (hpx0->enable_perr != 0)
		bctl |= PCI_BCNF_BCNTRL_PARITY_ENABLE;
	pci_config_put16(cfg_hdl, PCI_BCNF_BCNTRL, bctl);
}

static void
pcie_acpi_program_hpx_type2(ddi_acc_handle_t cfg_hdl,
    const pcie_acpi_hpx_type2_t *hpx2)
{
	uint16_t pcie_cap;
	uint16_t aer_cap;
	uint16_t reg16;
	uint32_t reg32;
	uint16_t devctl_and;
	uint16_t devctl_or;

	if (PCI_CAP_LOCATE(cfg_hdl, PCI_CAP_ID_PCI_E, &pcie_cap) !=
	    DDI_SUCCESS) {
		return;
	}

	devctl_and = hpx2->pci_exp_devctl_and | ~PCIE_DEVCTL_ERR_MASK;
	devctl_or = hpx2->pci_exp_devctl_or & PCIE_DEVCTL_ERR_MASK;

	reg16 = PCI_CAP_GET16(cfg_hdl, PCI_CAP_ID_PCI_E, pcie_cap, PCIE_DEVCTL);
	reg16 &= devctl_and;
	reg16 |= devctl_or;
	PCI_CAP_PUT16(cfg_hdl, PCI_CAP_ID_PCI_E, pcie_cap, PCIE_DEVCTL, reg16);

	if (PCI_CAP_LOCATE(cfg_hdl, PCI_CAP_XCFG_SPC(PCIE_EXT_CAP_ID_AER),
	    &aer_cap) != DDI_SUCCESS) {
		return;
	}

	reg32 = PCI_XCAP_GET32(cfg_hdl, PCIE_EXT_CAP_ID_AER, aer_cap,
	    PCIE_AER_UCE_MASK);
	reg32 = (reg32 & hpx2->unc_err_mask_and) | hpx2->unc_err_mask_or;
	PCI_XCAP_PUT32(cfg_hdl, PCIE_EXT_CAP_ID_AER, aer_cap,
	    PCIE_AER_UCE_MASK, reg32);

	reg32 = PCI_XCAP_GET32(cfg_hdl, PCIE_EXT_CAP_ID_AER, aer_cap,
	    PCIE_AER_UCE_SERV);
	reg32 = (reg32 & hpx2->unc_err_sever_and) | hpx2->unc_err_sever_or;
	PCI_XCAP_PUT32(cfg_hdl, PCIE_EXT_CAP_ID_AER, aer_cap,
	    PCIE_AER_UCE_SERV, reg32);

	reg32 = PCI_XCAP_GET32(cfg_hdl, PCIE_EXT_CAP_ID_AER, aer_cap,
	    PCIE_AER_CE_MASK);
	reg32 = (reg32 & hpx2->cor_err_mask_and) | hpx2->cor_err_mask_or;
	PCI_XCAP_PUT32(cfg_hdl, PCIE_EXT_CAP_ID_AER, aer_cap,
	    PCIE_AER_CE_MASK, reg32);

	reg32 = PCI_XCAP_GET32(cfg_hdl, PCIE_EXT_CAP_ID_AER, aer_cap,
	    PCIE_AER_CTL);
	reg32 = (reg32 & hpx2->adv_err_cap_and) | hpx2->adv_err_cap_or;

	if ((reg32 & PCIE_AER_CTL_ECRC_GEN_CAP) == 0)
		reg32 &= ~PCIE_AER_CTL_ECRC_GEN_ENA;
	if ((reg32 & PCIE_AER_CTL_ECRC_CHECK_CAP) == 0)
		reg32 &= ~PCIE_AER_CTL_ECRC_CHECK_ENA;

	PCI_XCAP_PUT32(cfg_hdl, PCIE_EXT_CAP_ID_AER, aer_cap,
	    PCIE_AER_CTL, reg32);
}

static ACPI_STATUS
pcie_acpi_run_hpx(ddi_acc_handle_t cfg_hdl, ACPI_HANDLE handle)
{
	ACPI_STATUS status;
	ACPI_BUFFER rb;
	ACPI_OBJECT *package;
	ACPI_OBJECT *record;
	ACPI_OBJECT *fields;
	int i;
	boolean_t applied = B_FALSE;

	rb.Length = ACPI_ALLOCATE_BUFFER;
	rb.Pointer = NULL;

	status = AcpiEvaluateObjectTyped(handle, "_HPX", NULL, &rb,
	    ACPI_TYPE_PACKAGE);
	if (ACPI_FAILURE(status))
		return (status);

	package = rb.Pointer;
	for (i = 0; i < package->Package.Count; i++) {
		pcie_acpi_hpx_type0_t hpx0;
		pcie_acpi_hpx_type2_t hpx2;

		record = &package->Package.Elements[i];
		if (record->Type != ACPI_TYPE_PACKAGE ||
		    record->Package.Count < 2) {
			status = AE_ERROR;
			goto out;
		}

		fields = record->Package.Elements;
		if (fields[0].Type != ACPI_TYPE_INTEGER ||
		    fields[1].Type != ACPI_TYPE_INTEGER) {
			status = AE_ERROR;
			goto out;
		}

		switch (fields[0].Integer.Value) {
		case 0:
			status = pcie_acpi_decode_hpx_type0(record, &hpx0);
			if (ACPI_FAILURE(status))
				goto out;
			pcie_acpi_program_hpx_type0(cfg_hdl, &hpx0);
			applied = B_TRUE;
			break;
		case 2:
			status = pcie_acpi_decode_hpx_type2(record, &hpx2);
			if (ACPI_FAILURE(status))
				goto out;
			pcie_acpi_program_hpx_type2(cfg_hdl, &hpx2);
			applied = B_TRUE;
			break;
		default:
			break;
		}
	}

	status = applied ? AE_OK : AE_NOT_FOUND;
out:
	AcpiOsFree(rb.Pointer);
	return (status);
}

static ACPI_STATUS
pcie_acpi_run_hpp(ddi_acc_handle_t cfg_hdl, ACPI_HANDLE handle)
{
	ACPI_STATUS status;
	ACPI_BUFFER rb;
	ACPI_OBJECT *package;
	ACPI_OBJECT *fields;
	pcie_acpi_hpx_type0_t hpx0;
	int i;

	rb.Length = ACPI_ALLOCATE_BUFFER;
	rb.Pointer = NULL;

	status = AcpiEvaluateObjectTyped(handle, "_HPP", NULL, &rb,
	    ACPI_TYPE_PACKAGE);
	if (ACPI_FAILURE(status))
		return (status);

	package = rb.Pointer;
	if (package->Package.Count != 4) {
		status = AE_ERROR;
		goto out;
	}

	fields = package->Package.Elements;
	for (i = 0; i < 4; i++) {
		if (fields[i].Type != ACPI_TYPE_INTEGER) {
			status = AE_ERROR;
			goto out;
		}
	}

	hpx0.revision = 1;
	hpx0.cache_line_size = fields[0].Integer.Value;
	hpx0.latency_timer = fields[1].Integer.Value;
	hpx0.enable_serr = fields[2].Integer.Value;
	hpx0.enable_perr = fields[3].Integer.Value;
	pcie_acpi_program_hpx_type0(cfg_hdl, &hpx0);
	status = AE_OK;
out:
	AcpiOsFree(rb.Pointer);
	return (status);
}

int
pcie_acpi_program_hp_params(dev_info_t *dip)
{
	ACPI_HANDLE handle;
	ACPI_HANDLE parent;
	ACPI_STATUS status;
	ddi_acc_handle_t cfg_hdl;

	if (pci_config_setup(dip, &cfg_hdl) != DDI_SUCCESS)
		return (DDI_FAILURE);

	status = acpica_get_handle(dip, &handle);
	if (ACPI_FAILURE(status)) {
		pci_config_teardown(&cfg_hdl);
		return (DDI_FAILURE);
	}

	for (;;) {
		status = pcie_acpi_run_hpx(cfg_hdl, handle);
		if (ACPI_SUCCESS(status)) {
			pci_config_teardown(&cfg_hdl);
			return (DDI_SUCCESS);
		}

		status = pcie_acpi_run_hpp(cfg_hdl, handle);
		if (ACPI_SUCCESS(status)) {
			pci_config_teardown(&cfg_hdl);
			return (DDI_SUCCESS);
		}

		if (AcpiGetParent(handle, &parent) != AE_OK)
			break;
		handle = parent;
	}

	pci_config_teardown(&cfg_hdl);
	return (DDI_FAILURE);
}

int
pcie_acpi_get_osc_ctl(dev_info_t *dip, uint32_t *osc_flags)
{
	pcie_bus_t *bus_p = PCIE_DIP2BUS(dip);
	pcie_x86_priv_t *osc_p;

	if (bus_p == NULL || osc_flags == NULL)
		return (DDI_FAILURE);

	osc_p = (pcie_x86_priv_t *)bus_p->bus_plat_private;
	if (osc_p == NULL || !osc_p->bus_osc_valid)
		return (DDI_FAILURE);

	*osc_flags = osc_p->bus_osc_ctrl;
	return (DDI_SUCCESS);
}

boolean_t
pcie_is_osc(dev_info_t *dip)
{
	pcie_bus_t *bus_p = PCIE_DIP2BUS(dip);
	pcie_x86_priv_t *osc_p = (pcie_x86_priv_t *)bus_p->bus_plat_private;
	return (osc_p->bus_osc);
}

#ifdef DEBUG
static void
pcie_dump_acpi_obj(ACPI_HANDLE pcibus_obj)
{
	int status;
	ACPI_BUFFER retbuf;

	if (pcibus_obj == NULL)
		return;

	retbuf.Pointer = NULL;
	retbuf.Length = ACPI_ALLOCATE_BUFFER;
	status = AcpiGetName(pcibus_obj, ACPI_FULL_PATHNAME, &retbuf);
	if (status != AE_OK)
		return;
	PCIE_DBG("PCIE BUS PATHNAME: %s\n", (char *)retbuf.Pointer);
	AcpiOsFree(retbuf.Pointer);

	PCIE_DBG("  METHODS: \n");
	status = AcpiWalkNamespace(ACPI_TYPE_METHOD, pcibus_obj, 1,
	    pcie_print_acpi_name, NULL, "  ", NULL);
	status = AcpiWalkNamespace(ACPI_TYPE_DEVICE, pcibus_obj, 1,
	    pcie_walk_obj_namespace, NULL, NULL, NULL);
}

/*ARGSUSED*/
static ACPI_STATUS
pcie_walk_obj_namespace(ACPI_HANDLE hdl, uint32_t nl, void *context,
	void **ret)
{
	int status;
	ACPI_BUFFER retbuf;
	char buf[32];

	retbuf.Pointer = NULL;
	retbuf.Length = ACPI_ALLOCATE_BUFFER;
	status = AcpiGetName(hdl, ACPI_FULL_PATHNAME, &retbuf);
	if (status != AE_OK)
		return (status);
	buf[0] = 0;
	while (nl--)
		(void) strcat(buf, "  ");
	PCIE_DBG("%sDEVICE: %s\n", buf, (char *)retbuf.Pointer);
	AcpiOsFree(retbuf.Pointer);

	PCIE_DBG("%s  METHODS: \n", buf);
	status = AcpiWalkNamespace(ACPI_TYPE_METHOD, hdl, 1,
	    pcie_print_acpi_name, NULL, (void *)buf, NULL);
	return (status);
}

/*ARGSUSED*/
static ACPI_STATUS
pcie_print_acpi_name(ACPI_HANDLE hdl, uint32_t nl, void *context, void **ret)
{
	int status;
	ACPI_BUFFER retbuf;
	char name[16];

	retbuf.Pointer = name;
	retbuf.Length = 16;
	status = AcpiGetName(hdl, ACPI_SINGLE_NAME, &retbuf);
	if (status == AE_OK)
		PCIE_DBG("%s    %s \n", (char *)context, name);
	return (AE_OK);
}
#endif /* DEBUG */
