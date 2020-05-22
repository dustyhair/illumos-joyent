/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2016, Anish Gupta (anish@freebsd.org)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
#include <sys/sunddi.h>  /* used by all entry points for this driver */
#include <contrib/dev/acpica/include/acpi.h>                    /* also used by ddi_create_minor_node, */
#include <sys/acpi/acpi.h>
#include <sys/acpica.h>
__FBSDID("$FreeBSD$");
#include <sys/ddi.h>
//#include "opt_acpi.h"
#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/malloc.h>

#include <sys/cmn_err.h>

#include <sys/modctl.h>  /* used by _init, _info, _fini */
#include <sys/types.h>   /* used by prop_op, ddi_prop_op */
#include <sys/stat.h>    /* defines S_IFCHR used by ddi_create_minor_node */
#include <sys/cmn_err.h> /* used by all entry points for this driver */
#include <sys/ddi.h>     /* used by all entry points for this driver */
                         /* also used by ddi_get_instance, ddi_prop_op */
                         /* ddi_get_instance, and ddi_prop_op */

#include <sys/conf.h>

#include <machine/vmparam.h>

#include <vm/vm.h>
#include <vm/pmap.h>

#include <sys/acpi/accommon.h>
#include <sys/pci_cap.h>
//#include <dev/acpica/acpivar.h>
//#include "io/pci/pci_common.h"
#include "io/iommu.h"
#include "amdvi_priv.h"
#include <sys/sunndi.h>
#define	device_printf(dev, fmt,...) \
	cmn_err(CE_WARN, fmt,## __VA_ARGS__)

static major_t ivrs_major;

extern void *device_arena_alloc(size_t size, int vm_flag);
//extern void pci_common_set_parent_private_data(dev_info_t *dip);
/*
	cmn_err(CE_WARN,
			    "rn_delete: inconsistent annotation\n");*/
static int ivhd_getinfo(device_t dip, ddi_info_cmd_t cmd, void *arg, void **result);
static int ivhd_attach(device_t dip, ddi_attach_cmd_t cmd);
static int ivhd_detach(device_t dip, ddi_detach_cmd_t cmd);
//static int ivhd_identify(device_tdip, ddi_detach_cmd_t cmd);
//static int ivhd_detach(device_t dip, ddi_detach_cmd_t cmd);
static int ivhd_open(dev_t *devp, int flag, int otyp, cred_t *credp);
static int ivhd_close(dev_t dev, int flag, int otyp, cred_t *credp);
 
static void		*ivrs_statep;

const char *ivrs_modname = "amd_ivrs";
device_t *ivhd_devs;			/* IVHD or AMD-Vi device list. */
int	ivhd_count;			/* Number of IVHD header. */
/* 
 * Cached IVHD header list.
 * Single entry for each IVHD, filtered the legacy one.
 */
ACPI_IVRS_HARDWARE *ivhd_hdrs[10];	

#ifndef __FreeBSD__
/*
 * This lives in vtd_sol.c for license reasons.
 */
extern dev_info_t *ivrs_get_dip(ACPI_IVRS_HARDWARE *, int);

extern int amdvi_setup_hw(dev_info_t *,struct amdvi_softc *, device_t *, int );

#endif

//XXX hardcoded for now it actually resides in amdvi_hw.c
int amdvi_ptp_level = 4;		/* Page table levels. */

typedef int (*ivhd_iter_t)(ACPI_IVRS_HEADER *ptr, void *arg);
/*
 * Iterate IVRS table for IVHD and IVMD device type.
 */


static void
ivrs_hdr_iterate_tbl(ivhd_iter_t iter, void *arg)
{
	ACPI_TABLE_IVRS *ivrs;
	ACPI_IVRS_HEADER *ivrs_hdr, *end;
	ACPI_STATUS status;

	status = AcpiGetTable(ACPI_SIG_IVRS, 1, (ACPI_TABLE_HEADER **)&ivrs);
	if (ACPI_FAILURE(status))
		return;

	if (ivrs->Header.Length == 0) {
		return;
	}

	ivrs_hdr = (ACPI_IVRS_HEADER *)(ivrs + 1);
	end = (ACPI_IVRS_HEADER *)((char *)ivrs + ivrs->Header.Length);

	while (ivrs_hdr < end) {
		if ((uint8_t *)ivrs_hdr + ivrs_hdr->Length > (uint8_t *)end) {
		
			printf("AMD-Vi:IVHD/IVMD is corrupted, length : %d\n",
			    ivrs_hdr->Length);
			break;
		}

		switch (ivrs_hdr->Type) {
		case IVRS_TYPE_HARDWARE_LEGACY:	/* Legacy */
		case IVRS_TYPE_HARDWARE_EFR:
		case IVRS_TYPE_HARDWARE_MIXED:
			if (!iter(ivrs_hdr, arg))
				return;
			break;

		case ACPI_IVRS_TYPE_MEMORY1:
		case ACPI_IVRS_TYPE_MEMORY2:
		case ACPI_IVRS_TYPE_MEMORY3:
			if (!iter(ivrs_hdr, arg))
				return;

			break;

		default:
			printf("AMD-Vi:Not IVHD/IVMD type(%d)", ivrs_hdr->Type);

		}

		ivrs_hdr = (ACPI_IVRS_HEADER *)((uint8_t *)ivrs_hdr +
			ivrs_hdr->Length);
	}
}



static bool
ivrs_is_ivhd(UINT8 type)
{

	switch(type) {
	case IVRS_TYPE_HARDWARE_LEGACY:
	case IVRS_TYPE_HARDWARE_EFR:
	case IVRS_TYPE_HARDWARE_MIXED:
		return (true);

	default:
		return (false);
	}
}
 
/* Count the number of AMD-Vi devices in the system. */

static int
ivhd_count_iter(ACPI_IVRS_HEADER * ivrs_he, void *arg)
{

	if (ivrs_is_ivhd(ivrs_he->Type))
		ivhd_count++;

	return (1);
}

struct find_ivrs_hdr_args {
	int	i;
	ACPI_IVRS_HEADER *ptr;
};



static int
ivrs_hdr_find_iter(ACPI_IVRS_HEADER * ivrs_hdr, void *args)
{
	struct find_ivrs_hdr_args *fi;

	fi = (struct find_ivrs_hdr_args *)args;
	if (ivrs_is_ivhd(ivrs_hdr->Type)) {
		if (fi->i == 0) {
			fi->ptr = ivrs_hdr;
			return (0);
		}
		fi->i--;
	}

	return (1);
}

static ACPI_IVRS_HARDWARE *
ivhd_find_by_index(int idx)
{
	struct find_ivrs_hdr_args fi;

	fi.i = idx;
	fi.ptr = NULL;

	ivrs_hdr_iterate_tbl(ivrs_hdr_find_iter, &fi);

	return ((ACPI_IVRS_HARDWARE *)fi.ptr);
}

static void
ivhd_dev_add_entry(struct amdvi_softc *softc, uint32_t start_id,uint32_t end_id, uint8_t cfg, bool ats)
{
	struct ivhd_dev_cfg *dev_cfg;

	/* If device doesn't have special data, don't add it. */
	if (!cfg)
		return;

	dev_cfg = &softc->dev_cfg[softc->dev_cfg_cnt++];
	dev_cfg->start_id = start_id;
	dev_cfg->end_id = end_id;
	dev_cfg->data = cfg;
	dev_cfg->enable_ats = ats;
}

/*
 * Record device attributes as suggested by BIOS.
 */
static int
ivhd_dev_parse(ACPI_IVRS_HARDWARE* ivhd, struct amdvi_softc *softc)
{
	ACPI_IVRS_DE_HEADER *de;
	uint8_t *p, *end;
	int range_start_id = 0, range_end_id = 0;
	uint32_t *extended;
	uint8_t all_data = 0, range_data = 0;
	bool range_enable_ats = false, enable_ats;

	softc->start_dev_rid = ~0;
	softc->end_dev_rid = 0;

	switch (ivhd->Header.Type) {
		case IVRS_TYPE_HARDWARE_LEGACY:
			p = (uint8_t *)ivhd + sizeof(ACPI_IVRS_HARDWARE);
			break;

		case IVRS_TYPE_HARDWARE_EFR:
		case IVRS_TYPE_HARDWARE_MIXED:
			p = (uint8_t *)ivhd + sizeof(ACPI_IVRS_HARDWARE_EFRSUP);
			break;

		default:
			device_printf(softc->dev, 
				"unknown type: 0x%x\n", ivhd->Header.Type);
			return (-1);
	}

	end = (uint8_t *)ivhd + ivhd->Header.Length;

	while (p < end) {
		de = (ACPI_IVRS_DE_HEADER *)p;
		softc->start_dev_rid = MIN(softc->start_dev_rid, de->Id);
		softc->end_dev_rid = MAX(softc->end_dev_rid, de->Id);
		switch (de->Type) {
		case ACPI_IVRS_TYPE_ALL:
			all_data = de->DataSetting;
			break;

		case ACPI_IVRS_TYPE_SELECT:
		case ACPI_IVRS_TYPE_ALIAS_SELECT:
		case ACPI_IVRS_TYPE_EXT_SELECT:
			enable_ats = false;
			if (de->Type == ACPI_IVRS_TYPE_EXT_SELECT) {
				extended = (uint32_t *)(de + 1);
				enable_ats =
				    (*extended & IVHD_DEV_EXT_ATS_DISABLE) ?
					false : true;
			}
			ivhd_dev_add_entry(softc, de->Id, de->Id,
			    de->DataSetting | all_data, enable_ats);
			break;

		case ACPI_IVRS_TYPE_START:
		case ACPI_IVRS_TYPE_ALIAS_START:
		case ACPI_IVRS_TYPE_EXT_START:
			range_start_id = de->Id;
			range_data = de->DataSetting;
			if (de->Type == ACPI_IVRS_TYPE_EXT_START) {
				extended = (uint32_t *)(de + 1);
				range_enable_ats =
				    (*extended & IVHD_DEV_EXT_ATS_DISABLE) ?
					false : true;
			}
			break;

		case ACPI_IVRS_TYPE_END:
			range_end_id = de->Id;
			ivhd_dev_add_entry(softc, range_start_id, range_end_id,
				range_data | all_data, range_enable_ats);
			range_start_id = range_end_id = 0;
			range_data = 0;
			all_data = 0;
			break;

		case ACPI_IVRS_TYPE_PAD4:
			break;

		case ACPI_IVRS_TYPE_SPECIAL:
			/* HPET or IOAPIC */
			break;
		default:
			
			
			if ((de->Type < 5) ||
			    (de->Type >= ACPI_IVRS_TYPE_PAD8))
				device_printf(softc->dev,
				    "Unknown dev entry:0x%x\n", de->Type);
		}

		if (softc->dev_cfg_cnt >
			(sizeof(softc->dev_cfg) / sizeof(softc->dev_cfg[0]))) {
			device_printf(softc->dev,
			    "WARN Too many device entries.\n");
			return (EINVAL);
		}
		if (de->Type < 0x40)
			p += sizeof(ACPI_IVRS_DEVICE4);
		else if (de->Type < 0x80)
			p += sizeof(ACPI_IVRS_DEVICE8A);
		else {
			printf("Variable size IVHD type 0x%x not supported\n",
			    de->Type);
			break;
		}
	}

	KASSERT((softc->end_dev_rid >= softc->start_dev_rid),
	    ("Device end[0x%x] < start[0x%x.\n",
	    softc->end_dev_rid, softc->start_dev_rid));

	return (0);
}

static bool
ivhd_is_newer(ACPI_IVRS_HEADER *old, ACPI_IVRS_HEADER  *new)
{
	/*
	 * Newer IVRS header type take precedence.
	 */
	if ((old->DeviceId == new->DeviceId) &&
		(old->Type == IVRS_TYPE_HARDWARE_LEGACY) &&
		((new->Type == IVRS_TYPE_HARDWARE_EFR) ||
		(new->Type == IVRS_TYPE_HARDWARE_MIXED))) {
		return (true);
	}

	return (false);
}
	
//ivhd_identify(driver_t *driver, device_t parent)
static void 
ivhd_identify()
{
	//ddi_acc_handle_t handle;
	
	ACPI_TABLE_IVRS *ivrs;
	ACPI_IVRS_HARDWARE *ivhd;
	ACPI_STATUS status;
	int i, count = 0;
	uint32_t ivrs_ivinfo;

	#ifdef XXX
	if (acpi_disabled("ivhd"))
		return;
	#endif 

	status = AcpiGetTable(ACPI_SIG_IVRS, 1, (ACPI_TABLE_HEADER **)&ivrs);
	if (ACPI_FAILURE(status))
		return;

	if (ivrs->Header.Length == 0) {
		return;
	}

	ivrs_ivinfo = ivrs->Info;
	printf("AMD-Vi: IVRS Info VAsize = %d PAsize = %d GVAsize = %d"
	       " flags:%b\n",
		REG_BITS(ivrs_ivinfo, 21, 15), REG_BITS(ivrs_ivinfo, 14, 8), 
		REG_BITS(ivrs_ivinfo, 7, 5), REG_BITS(ivrs_ivinfo, 22, 22),
		"\020\001EFRSup");

	ivrs_hdr_iterate_tbl(ivhd_count_iter, NULL);
	if (!ivhd_count)
		return;

	for (i = 0; i < ivhd_count; i++) {
		ivhd = ivhd_find_by_index(i);
		KASSERT(ivhd, ("ivhd%d is NULL\n", i));
		ivhd_hdrs[i] = ivhd;
	}

        /* 
	 * Scan for presence of legacy and non-legacy device type
	 * for same AMD-Vi device and override the old one.
	 */
	for (i = ivhd_count - 1 ; i > 0 ; i--){
       		if (ivhd_is_newer(&ivhd_hdrs[i-1]->Header, 
			&ivhd_hdrs[i]->Header)) {
			ivhd_hdrs[i-1] = ivhd_hdrs[i];
			ivhd_count--;
		}
       }	       


	ivhd_devs = kmem_zalloc(sizeof(struct ddi_parent_private_data) * ivhd_count, KM_SLEEP);
	#ifdef XXX
	ivhd_devs = malloc(sizeof(device_t) * ivhd_count, M_DEVBUF,
		M_WAITOK | M_ZERO);
	#endif
	
	


	
	for (i = 0; i < ivhd_count; i++) {
		ivhd = ivhd_hdrs[i];
		KASSERT(ivhd, ("ivhd%d is NULL\n", i));

		/*
		 * Use a high order to ensure that this driver is probed after
		 * the Host-PCI bridge and the root PCI bus.
		 */
		#ifdef __FreeBSD__ 
		//disabled for not as I do not know how to do this 
		ivhd_devs[i] = BUS_ADD_CHILD(parent,
		    ACPI_DEV_BASE_ORDER + 10 * 10, "ivhd", i);
		#endif
		/*
		 * XXX: In case device was not destroyed before, add will fail.
		 * locate the old device instance.
		 */
//		#ifdef XXX //probably can disable this for the moment until we can compile with minimal errors
		
		if (ivhd_devs[i] == NULL) {
			#ifdef __FreeBSD__ 
			ivhd_devs[i] = device_find_child(parent, "ivhd", i);
			#endif 
			
			
			#ifdef XXX		
			ivhd_devs[i] = (device_t)ivrs_get_dip(ivhd, i);
		
			if (ivhd_devs[i] == NULL) {
				printf("AMD-Vi: cant find ivhd%d\n", i);
				break;
			}
			#endif
		}
//		#endif 
		count++;
	}

	/*
	 * Update device count in case failed to attach.
	 */
	printf("Finished in Identify() AMD_IVRS");
	printf("ivhdcount = %d",ivhd_count); 
	ivhd_count = count;
}

int device_get_unit(device_t dev){
	return ddi_get_instance(dev);
}
#define BUS_PROBE_NOWILDCARD 0
static int
ivhd_probe(device_t dev)
{
	device_printf(dev, "Probe started");
	ACPI_IVRS_HARDWARE *ivhd;
	ddi_acc_handle_t handle;
	int unit, bus,device,func;
	
	#ifdef XXX
	if (acpi_get_handle(dev) != NULL)
		return (ENXIO);
	#endif


	
	if (acpica_get_bdf(dev, &bus, &device, &func)
	    != DDI_SUCCESS) {
		//cmn_err(CE_WARN, "%s: %s%d: Failed to get BDF"
		//    "Unable to use IOMMU unit idx=%d - skipping ...",
		//    f, driver, instance, idx);
		cmn_err(CE_WARN, "ivrs: Failed to get BDF");
		//    "Unable to use IOMMU unit idx=%d - skipping ...",
		//    f, driver, instance, idx);
		return (ENXIO);
	}

	unit = device_get_unit(dev);
	
	if (pci_config_setup(dev, &handle) != DDI_SUCCESS) {
		cmn_err(CE_WARN, "PCI config setup failed 111, %d", unit);
		return (ENXIO);
	}
	ivhd_devs[unit] = dev;
	
	cmn_err(CE_WARN, "PCI config setup passed, %d",unit);
	
	KASSERT((unit <= ivhd_count), 
		("ivhd unit %d > count %d", unit, ivhd_count));
	ivhd = ivhd_hdrs[unit];
	KASSERT(ivhd, ("ivhd is NULL"));
	
	switch (ivhd->Header.Type) {
	case IVRS_TYPE_HARDWARE_EFR:
		//XXX I do not know how to set the description so I am just displaying it
		device_printf(dev, "AMD-Vi/IOMMU ivhd with EFR");
		//device_set_desc(dev, "AMD-Vi/IOMMU ivhd with EFR");
		
		break;
	
	case IVRS_TYPE_HARDWARE_MIXED:
		device_printf(dev, "AMD-Vi/IOMMU ivhd in mixed format");
		//device_set_desc(dev, "AMD-Vi/IOMMU ivhd in mixed format");
		break;

	case IVRS_TYPE_HARDWARE_LEGACY:
        default:
		device_printf(dev, "AMD-Vi/IOMMU ivhd");
		//device_set_desc(dev, "AMD-Vi/IOMMU ivhd");
		break;
	}
	device_printf(dev, "Probe complete");
	return (BUS_PROBE_NOWILDCARD);

	
}

static void
ivhd_print_flag(device_t dev, enum IvrsType ivhd_type, uint8_t flag)
{
	/*
	 * IVHD lgeacy type has two extra high bits in flag which has
	 * been moved to EFR for non-legacy device.
	 */
	switch (ivhd_type) {
	case IVRS_TYPE_HARDWARE_LEGACY:
		device_printf(dev, "Flag:%b\n", flag,
			"\020"
			"\001HtTunEn"
			"\002PassPW"
			"\003ResPassPW"
			"\004Isoc"
			"\005IotlbSup"
			"\006Coherent"
			"\007PreFSup"
			"\008PPRSup");
		break;

	case IVRS_TYPE_HARDWARE_EFR:
	case IVRS_TYPE_HARDWARE_MIXED:
		device_printf(dev, "Flag:%b\n", flag,
			"\020"
			"\001HtTunEn"
			"\002PassPW"
			"\003ResPassPW"
			"\004Isoc"
			"\005IotlbSup"
			"\006Coherent");
		break;
	default:
		device_printf(dev, "Can't decode flag of ivhd type :0x%x\n",
			ivhd_type);
		break;
	}
}

/*
 * Feature in legacy IVHD type(0x10) and attribute in newer type(0x11 and 0x40).
 */
static void
ivhd_print_feature(device_t dev, enum IvrsType ivhd_type, uint32_t feature) 
{
	switch (ivhd_type) {
	case IVRS_TYPE_HARDWARE_LEGACY:
		device_printf(dev, "Features(type:0x%x) HATS = %d GATS = %d"
			" MsiNumPPR = %d PNBanks= %d PNCounters= %d\n",
			ivhd_type,
			REG_BITS(feature, 31, 30),
			REG_BITS(feature, 29, 28),
			REG_BITS(feature, 27, 23),
			REG_BITS(feature, 22, 17),
			REG_BITS(feature, 16, 13));
		device_printf(dev, "max PASID = %d GLXSup = %d Feature:%b\n",
			REG_BITS(feature, 12, 8),
			REG_BITS(feature, 4, 3),
			feature,
			"\020"
			"\002NXSup"
			"\003GTSup"
			"\004<b4>"
			"\005IASup"
			"\006GASup"
			"\007HESup");
		break;

	/* Fewer features or attributes are reported in non-legacy type. */
	case IVRS_TYPE_HARDWARE_EFR:
	case IVRS_TYPE_HARDWARE_MIXED:
		device_printf(dev, "Features(type:0x%x) MsiNumPPR = %d"
			" PNBanks= %d PNCounters= %d\n",
			ivhd_type,
			REG_BITS(feature, 27, 23),
			REG_BITS(feature, 22, 17),
			REG_BITS(feature, 16, 13));
		break;

	default: /* Other ivhd type features are not decoded. */
		
		device_printf(dev, "Can't decode ivhd type :0x%x\n", ivhd_type);
	}
}

/* Print extended features of IOMMU. */
static void
ivhd_print_ext_feature(device_t dev, uint64_t ext_feature)
{
	uint32_t ext_low, ext_high;

	if (!ext_feature)
		return;

	ext_low = ext_feature;

	device_printf(dev, "Extended features[31:0]:%b "
		"HATS = 0x%x GATS = 0x%x "
		"GLXSup = 0x%x SmiFSup = 0x%x SmiFRC = 0x%x "
		"GAMSup = 0x%x DualPortLogSup = 0x%x DualEventLogSup = 0x%x\n",
		(int)ext_low,
		"\020"
		"\001PreFSup"
		"\002PPRSup"
		"\003<b2>"
		"\004NXSup"
		"\005GTSup"
		"\006<b5>"
		"\007IASup"
		"\008GASup"
		"\009HESup"
		"\010PCSup",
		REG_BITS(ext_low, 11, 10),
		REG_BITS(ext_low, 13, 12),
		REG_BITS(ext_low, 15, 14),
		REG_BITS(ext_low, 17, 16),
		REG_BITS(ext_low, 20, 18),
		REG_BITS(ext_low, 23, 21),
		REG_BITS(ext_low, 25, 24),
		REG_BITS(ext_low, 29, 28));

	ext_high = ext_feature >> 32;
	device_printf(dev, "Extended features[62:32]:%b "
		"Max PASID: 0x%x DevTblSegSup = 0x%x "
		"MarcSup = 0x%x\n",
		(int)(ext_high),
		"\020"
		"\006USSup"
		"\009PprOvrflwEarlySup"
		"\010PPRAutoRspSup"
		"\013BlKStopMrkSup"
		"\014PerfOptSup"
		"\015MsiCapMmioSup"
		"\017GIOSup"
		"\018HASup"
		"\019EPHSup"
		"\020AttrFWSup"
		"\021HDSup"
		"\023InvIotlbSup",
	    	REG_BITS(ext_high, 5, 0),
	    	REG_BITS(ext_high, 8, 7),
	    	REG_BITS(ext_high, 11, 10));
}

static int
ivhd_print_cap(struct amdvi_softc *softc, ACPI_IVRS_HARDWARE * ivhd)
{
	device_t dev;
	int max_ptp_level;

	dev = softc->dev;
	
	ivhd_print_flag(dev, softc->ivhd_type, softc->ivhd_flag);
	ivhd_print_feature(dev, softc->ivhd_type, softc->ivhd_feature);
	ivhd_print_ext_feature(dev, softc->ext_feature);
	max_ptp_level = 7;
	/* Make sure device support minimum page level as requested by user. */
	//int amdvi_ptp_level = 1; //XXX get rid of this 

	if (max_ptp_level < amdvi_ptp_level) {
		
		device_printf(dev, "insufficient PTP level:%d\n",
			max_ptp_level);
		//	#endif
		return (EINVAL);
	} else {

		device_printf(softc->dev, "supported paging level:%d, will use only: %d\n",
	    		max_ptp_level, amdvi_ptp_level);
	}

	device_printf(softc->dev, "device range: 0x%x - 0x%x\n",
			softc->start_dev_rid, softc->end_dev_rid);

	return (0);
}

static void *
ivhd_map(dev_info_t *dip )
{
	caddr_t regs;
	struct amdvi_softc *softc;

	softc = device_get_softc(dip);
	int error;
	//ddi_acc_handle_t hdl;
	static ddi_device_acc_attr_t regs_attr = {
		DDI_DEVICE_ATTR_V0,
		DDI_NEVERSWAP_ACC,
		DDI_STRICTORDER_ACC,
	};

	error = ddi_regs_map_setup(dip, 0, &regs, 0, 0, &regs_attr,
	    &softc->hdl);

	if (error != DDI_SUCCESS)
		return (NULL);
	
	//ddi_set_driver_private(dip, hdl);
	printf("IVRS ivhd base address %x",regs);
	return (regs);
}

static int 
ivhd_attach(device_t dev, ddi_attach_cmd_t cmd)
{
	device_printf(dev,
		    "ATTACH ENTERED");
	ACPI_IVRS_HARDWARE *ivhd;
	ACPI_IVRS_HARDWARE_EFRSUP *ivhd_efr;
	struct amdvi_softc *softc;
	int status, unit;
	dev_info_t *dip;
	dip = dev;
	uint64_t pgoffset; 
	const char *driver = ddi_driver_name(dip);
	//int  unit;

	//#ifdef XXX
	unit = device_get_unit(dev);
	device_printf(dev,
		    "ATTACH ENTERED 1");
	device_printf(dev,
		    "ATTACH ENTERED Unit %d",unit);
	KASSERT((unit <= ivhd_count), 
		("ivhd unit %d > count %d", unit, ivhd_count));
	/* Make sure its same device for which attach is called. */
	KASSERT((ivhd_devs[unit] == dev),
		("Not same device old %p new %p", ivhd_devs[unit], dev));

	if (ddi_soft_state_zalloc(ivrs_statep, unit) != DDI_SUCCESS) {
		goto fail;
	}
	
	device_printf(dev,
		    "ATTACH UNIT %x ", unit);
	 
	VERIFY(softc = ddi_get_soft_state(ivrs_statep, unit));
	//softc = device_get_softc(dev);
	

	if (ddi_create_minor_node(dev, "amd-ivrs", S_IFCHR,
	    IVHD_IOMMU_INST2MINOR(unit), "ddi_iommu",
	    0) != DDI_SUCCESS) {
		cmn_err(CE_WARN, "Unable to create minor node for "
		    "%s%d", driver, unit);
		ddi_remove_minor_node(dip, NULL);
		ddi_soft_state_free(ivrs_statep, unit);
		return (DDI_FAILURE);
	}


	softc->dev = dev;
	ddi_set_driver_private(dev, softc);
	//pci_common_set_parent_private_data(dev);
	ivhd = ivhd_hdrs[unit];
	
	KASSERT(ivhd, ("ivhd is NULL"));

	softc->ivhd_type = ivhd->Header.Type;
	softc->pci_seg = ivhd->PciSegmentGroup;
	softc->pci_rid = ivhd->Header.DeviceId;
	softc->ivhd_flag = ivhd->Header.Flags;
	
	device_printf(dev,
		    "ATTACH RID 1 0x%x", softc->pci_rid);
	/* 
	 * On lgeacy IVHD type(0x10), it is documented as feature
	 * but in newer type it is attribute.
	 */
	softc->ivhd_feature = ivhd->Reserved;
	
	/* 
	 * PCI capability has more capabilities that are not part of IVRS.
	 */
	softc->cap_off = ivhd->CapabilityOffset;

	//device_printf(dev,
	//	    "ATTACH ENTERED 3");

#ifdef notyet
	/* IVHD Info bit[4:0] is event MSI/X number. */
	//softc->event_msix = ivhd->Info & 0x1F;
#endif
	switch (ivhd->Header.Type) {
		case IVRS_TYPE_HARDWARE_EFR:
		case IVRS_TYPE_HARDWARE_MIXED:
			ivhd_efr = (ACPI_IVRS_HARDWARE_EFRSUP *)ivhd;
			softc->ext_feature = ivhd_efr->ExtFR;
			break;

	}

	//device_printf(dev,
	//	    "ATTACH ENTERED 4");
	
	//printf("IVRS %x",(uintptr_t)(struct amdvi_ctrl *) PHYS_TO_DMAP(ivhd->BaseAddress));
	softc->aiomt_reg_pa = ivhd->BaseAddress;
	pgoffset = softc->aiomt_reg_pa & MMU_PAGEOFFSET;
	ASSERT(pgoffset == 0);

	softc->aiomt_reg_pages = mmu_btopr(AMD_IOMMU_REG_SIZE + pgoffset);
	softc->aiomt_reg_size = mmu_ptob(softc->aiomt_reg_pages);

	softc->aiomt_va = (uintptr_t)device_arena_alloc(
	ptob(softc->aiomt_reg_pages), VM_SLEEP);

	hat_devload(kas.a_hat, (void *)(uintptr_t)softc->aiomt_va,
	    softc->aiomt_reg_size,
	    mmu_btop(softc->aiomt_reg_pa), PROT_READ | PROT_WRITE
	    | HAT_STRICTORDER, HAT_LOAD_LOCK);

	softc->aiomt_reg_va = softc->aiomt_va + pgoffset;
	softc->ctrl = (struct amdvi_ctrl *)softc->aiomt_reg_va;

	//softc->ctrl = (struct amdvi_ctrl *) PHYS_TO_DMAP(ivhd->BaseAddress);
	printf("IVRS ctrl %x",softc->ctrl);
	status = ivhd_dev_parse(ivhd, softc);
	
	//device_printf(dev,
	//	    "ATTACH ENTERED 5");
	if (status != 0) {
		device_printf(dev,
		    "endpoint device parsing error=%d\n", status);
	
	}
	status = ivhd_print_cap(softc, ivhd);
	if (status != 0) {
		return (status);
	}
	
	//#ifdef XXX
	status = amdvi_setup_hw(dip, softc,ivhd_devs,ivhd_count);
	//#endif
	if (status != 0) {
	
		device_printf(dev, "couldn't be initialised, error=%d\n", 
		    status);
	
		return (status);
	}

	//#endif

	ddi_report_dev(dev);
	return (0);

	fail:
	cmn_err(CE_WARN,"ERRRORRRRRRRR not able to zaloc");
	return (-1);
}

static int
ivhd_detach(device_t dip, ddi_detach_cmd_t cmd)
{
	//struct amdvi_softc *softc;

	//softc = device_get_softc(dev);

	//amdvi_teardown_hw(softc);

	/*
	 * XXX: delete the device.
	 * don't allow detach, return EBUSY.
	 */
	return (0);
}

#ifdef __FreeBSD__

static device_method_t ivhd_methods[] = {
	DEVMETHOD(device_identify, ivhd_identify),
	DEVMETHOD(device_probe, ivhd_probe),
	DEVMETHOD(device_attach, ivhd_attach),
	DEVMETHOD(device_detach, ivhd_detach),
	DEVMETHOD(device_suspend, ivhd_suspend),
	DEVMETHOD(device_resume, ivhd_resume),
	DEVMETHOD_END
};


static driver_t ivhd_driver = {
	"ivhd",
	ivhd_methods,
	sizeof(struct amdvi_softc),
};


static devclass_t ivhd_devclass;

/*
 * Load this module at the end after PCI re-probing to configure interrupt.
 */
DRIVER_MODULE_ORDERED(ivhd, acpi, ivhd_driver, ivhd_devclass, 0, 0,
		      SI_ORDER_ANY);
MODULE_DEPEND(ivhd, acpi, 1, 1, 1);
MODULE_DEPEND(ivhd, pci, 1, 1, 1);
#endif

static struct cb_ops ivhd_cb_ops = {
	ivhd_open,		/* cb_open */
	ivhd_close,	/* cb_close */
	nodev,			/* cb_strategy */
	nodev,			/* cb_print */
	nodev,			/* cb_dump */
	nodev,			/* cb_read */
	nodev,			/* cb_write */
	nodev,			/* cb_ioctl */
	nodev,			/* cb_devmap */
	nodev,			/* cb_mmap */
	nodev,			/* cb_segmap */
	nochpoll,		/* cb_chpoll */
	ddi_prop_op,		/* cb_prop_op XXX?*/
	NULL,			/* cb_str */
	D_NEW | D_MP,		/* cb_flag XXX?*/
	CB_REV,			/* cb_rev XXX?*/
	nodev,			/* cb_aread */
	nodev			/* cb_awrite */
};


static struct dev_ops ivhd_dev_ops = {
	DEVO_REV,		/* devo_rev */
	0,			/* devo_refcnt */
	ivhd_getinfo,	/* devo_getinfo */
	nodev,		/* devo_identify */
	ivhd_probe,		/* devo_probe */
	ivhd_attach,	/* devo_attach */
	ivhd_detach,	/* devo_detach */
	nodev,			/* devo_reset */
	&ivhd_cb_ops,	/* devo_cb_ops */
	NULL,			/* devo_bus_ops */
	nulldev,		/* devo_power */
	nulldev,	/* devo_quiesce */
};

static struct modldrv modldrv = {
	&mod_driverops,
	"AMD IVRS 0.1",
	&ivhd_dev_ops
};

static struct modlinkage modlinkage = {
	MODREV_1,
	(void *)&modldrv,
	NULL
};


static int
ivhd_getinfo(device_t dip, ddi_info_cmd_t cmd, void *arg, void **result)
{
	struct amdvi_softc *statep;

	ASSERT(result);

	*result = NULL;

	switch (cmd) {
	case DDI_INFO_DEVT2DEVINFO:
		statep = ddi_get_soft_state(ivrs_statep,
		    IVHD_IOMMU_MINOR2INST(getminor((dev_t)arg)));
		if (statep) {
			*result = statep->dev;
			return (DDI_SUCCESS);
		}
		break;
	case DDI_INFO_DEVT2INSTANCE:
		*result = (void *)(uintptr_t)
		    IVHD_IOMMU_MINOR2INST(getminor((dev_t)arg));
		return (DDI_SUCCESS);
	}

	return (DDI_FAILURE);
}

static int 
ivhd_open(dev_t *devp, int flag, int otyp, cred_t *credp)
{
return 0;
}
static int 
ivhd_close(dev_t dev, int flag, int otyp, cred_t *credp)
{
return 0;
}

int
_init(void)
{
	int error = ENOTSUP; 
//Check that were running AMD
#if defined(__amd64) && !defined(__xpv)
	cmn_err(CE_WARN,"Entering the INIT");

	//Are we running on bare metal
	//if (get_hwenv() != HW_NATIVE)
	//	return (ENOTSUP);

	//Init state this may notbe right XXX
	error = ddi_soft_state_init(&ivrs_statep,
	    sizeof (struct amdvi_softc), 0);
	if (error) {
		cmn_err(CE_WARN, "%s: _init: failed to init soft state.",
		    ivrs_modname);
	}

	ivrs_major = ddi_name_to_major("amd_ivrs");

	if (error) {
		goto fail;
	}

	ivhd_identify();
	error = mod_install(&modlinkage);

fail:
	if (error) {
		ddi_soft_state_fini(&ivrs_statep);
	}
	
#endif
	return (error);
}

int
_fini(void)
{
	int error;

	cmn_err(CE_WARN,"Entering the FINI");
	error = mod_remove(&modlinkage);
	if (error)
		return (error);
	ddi_soft_state_fini(&ivrs_statep);

	return (0);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&modlinkage, modinfop));
}

