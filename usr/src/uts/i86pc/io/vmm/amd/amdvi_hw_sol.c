
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/pci_cap.h>
#include <sys/bus.h>
#include "amdvi_priv.h"
uint32_t
amdvi_pci_read_sol(dev_info_t *dip, int off,uint16_t cap_base1)
{

uint32_t id;
//struct amdvi_softc *softc;
//softc = device_get_softc(dip);
// = ddi_get_driver_private(dip);
uint16_t cap_base;
uint32_t caphdr;
ddi_acc_handle_t handle;

//printf("  [cap_base 0x%lx]", cap_base);

if (pci_config_setup(dip, &handle) != DDI_SUCCESS) {
		cmn_err(CE_WARN, "PCI config setup failed");
		return (DDI_FAILURE);
	}

pci_cap_probe(handle, 0, &id, &cap_base);

caphdr = PCI_CAP_GET32(handle, 0, cap_base,
		    0);
return caphdr;
}
