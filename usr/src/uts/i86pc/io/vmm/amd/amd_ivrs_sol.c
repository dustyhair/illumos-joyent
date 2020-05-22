


#include <sys/sunndi.h>
//#include <contrib/dev/acpica/include/acpi.h>
#include <sys/acpi/acpi.h>
#include <sys/acpica.h>
dev_info_t *
ivrs_get_dip(ACPI_IVRS_HARDWARE *ivrshd, int unit){
	dev_info_t *dip;
	struct ddi_parent_private_data *pdptr;

	int circ,bus,device,func;

	/*
	 * Try to find an existing devinfo node for this ivrs unit.
	 */
	ndi_devi_enter(ddi_root_node(), &circ);
	dip = ddi_find_devinfo("amd_ivrs", unit, 0);
	ndi_devi_exit(ddi_root_node(), circ);

	

	if (dip != NULL)
		return (dip);



	dip = ddi_add_child(ddi_root_node(), "amd_ivrs",
	    DEVI_SID_NODEID, unit);

	pdptr = kmem_zalloc(sizeof (struct ddi_parent_private_data), KM_SLEEP);

	ddi_set_parent_data(dip, pdptr);
	
	if (acpica_get_bdf(dip, &bus, &device, &func)
	    != DDI_SUCCESS) {
		//cmn_err(CE_WARN, "%s: %s%d: Failed to get BDF"
		//    "Unable to use IOMMU unit idx=%d - skipping ...",
		//    f, driver, instance, idx);
		cmn_err(CE_WARN, "ivrs: Failed to get BDF");
		//    "Unable to use IOMMU unit idx=%d - skipping ...",
		//    f, driver, instance, idx);
		return (NULL);
	}
	cmn_err(CE_WARN, "ivrs: BDF FOUND to get BDF");
	
	return (dip);
}	
