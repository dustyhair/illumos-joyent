/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2026 Jonathan Wagner.
 */

#ifndef _SYS_LX_INPUT_H
#define	_SYS_LX_INPUT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

#define	LX_INPUT_DRV			"lx_input"

#define	LX_INPUT_MINOR_BITS		6
#define	LX_INPUT_MINOR_MASK		((1U << LX_INPUT_MINOR_BITS) - 1)
#define	LX_INPUT_MINOR_UINPUT		0
#define	LX_INPUT_MINOR_EVENT_BASE	1
#define	LX_INPUT_EVENT_COUNT		32
#define	LX_INPUT_MINOR_EVENT_LAST	(LX_INPUT_MINOR_EVENT_BASE + \
	LX_INPUT_EVENT_COUNT - 1)
#define	LX_INPUT_MINOR_TYPE(m) \
	((minor_t)(m) & LX_INPUT_MINOR_MASK)
#define	LX_INPUT_MINOR_CLONE(id, type) \
	(((minor_t)(id) << LX_INPUT_MINOR_BITS) | (minor_t)(type))

#define	LX_INPUT_LINUX_UINPUT_MAJOR	10
#define	LX_INPUT_LINUX_UINPUT_MINOR	223
#define	LX_INPUT_LINUX_EVENT_MAJOR	13
#define	LX_INPUT_LINUX_EVENT_BASE	64

/* Linux input ABI limits. */
#define	LX_EV_SYN		0x00
#define	LX_EV_KEY		0x01
#define	LX_EV_REL		0x02
#define	LX_EV_ABS		0x03
#define	LX_EV_MSC		0x04
#define	LX_EV_SW		0x05
#define	LX_EV_LED		0x11
#define	LX_EV_SND		0x12
#define	LX_EV_REP		0x14
#define	LX_EV_FF		0x15
#define	LX_EV_MAX		0x1f

#define	LX_SYN_REPORT		0
#define	LX_SYN_DROPPED		3

#define	LX_KEY_MAX		0x2ff
#define	LX_REL_MAX		0x0f
#define	LX_ABS_MAX		0x3f
#define	LX_MSC_MAX		0x07
#define	LX_SW_MAX		0x10
#define	LX_LED_MAX		0x0f
#define	LX_SND_MAX		0x07
#define	LX_FF_MAX		0x7f
#define	LX_INPUT_PROP_MAX	0x1f

#define	LX_INPUT_BITS(n)	(((n) + 8) / 8)

#define	LX_INPUT_NAME_SIZE	80
#define	LX_INPUT_PHYS_SIZE	128
#define	LX_INPUT_UNIQ_SIZE	128

typedef struct lx_input_id {
	uint16_t lii_bustype;
	uint16_t lii_vendor;
	uint16_t lii_product;
	uint16_t lii_version;
} lx_input_id_t;

typedef struct lx_input_absinfo {
	int32_t lia_value;
	int32_t lia_minimum;
	int32_t lia_maximum;
	int32_t lia_fuzz;
	int32_t lia_flat;
	int32_t lia_resolution;
} lx_input_absinfo_t;

/* Device description snapshot returned to other LX kernel modules. */
typedef struct lx_input_config {
	lx_input_id_t lic_id;
	char lic_name[LX_INPUT_NAME_SIZE];
	char lic_phys[LX_INPUT_PHYS_SIZE];
	char lic_uniq[LX_INPUT_UNIQ_SIZE];
	uint32_t lic_ff_effects_max;
	uint8_t lic_evbits[LX_INPUT_BITS(LX_EV_MAX)];
	uint8_t lic_keybits[LX_INPUT_BITS(LX_KEY_MAX)];
	uint8_t lic_relbits[LX_INPUT_BITS(LX_REL_MAX)];
	uint8_t lic_absbits[LX_INPUT_BITS(LX_ABS_MAX)];
	uint8_t lic_mscbits[LX_INPUT_BITS(LX_MSC_MAX)];
	uint8_t lic_swbits[LX_INPUT_BITS(LX_SW_MAX)];
	uint8_t lic_ledbits[LX_INPUT_BITS(LX_LED_MAX)];
	uint8_t lic_sndbits[LX_INPUT_BITS(LX_SND_MAX)];
	uint8_t lic_ffbits[LX_INPUT_BITS(LX_FF_MAX)];
	uint8_t lic_propbits[LX_INPUT_BITS(LX_INPUT_PROP_MAX)];
	lx_input_absinfo_t lic_absinfo[LX_ABS_MAX + 1];
} lx_input_config_t;

typedef struct lx_uinput_setup {
	lx_input_id_t lus_id;
	char lus_name[LX_INPUT_NAME_SIZE];
	uint32_t lus_ff_effects_max;
} lx_uinput_setup_t;

typedef struct lx_uinput_abs_setup {
	uint16_t luas_code;
	uint16_t luas_pad;
	lx_input_absinfo_t luas_absinfo;
} lx_uinput_abs_setup_t;

typedef struct lx_uinput_user_dev {
	char luud_name[LX_INPUT_NAME_SIZE];
	lx_input_id_t luud_id;
	uint32_t luud_ff_effects_max;
	int32_t luud_absmax[LX_ABS_MAX + 1];
	int32_t luud_absmin[LX_ABS_MAX + 1];
	int32_t luud_absfuzz[LX_ABS_MAX + 1];
	int32_t luud_absflat[LX_ABS_MAX + 1];
} lx_uinput_user_dev_t;

typedef struct lx_input_event32 {
	int32_t lie_sec;
	int32_t lie_usec;
	uint16_t lie_type;
	uint16_t lie_code;
	int32_t lie_value;
} lx_input_event32_t;

typedef struct lx_input_event64 {
	int64_t lie_sec;
	int64_t lie_usec;
	uint16_t lie_type;
	uint16_t lie_code;
	int32_t lie_value;
} lx_input_event64_t;

/* Linux ioctl encoding, kept local so native ioctl values cannot collide. */
#define	LX_IOC_NRBITS		8
#define	LX_IOC_TYPEBITS		8
#define	LX_IOC_SIZEBITS		14
#define	LX_IOC_NRSHIFT		0
#define	LX_IOC_TYPESHIFT	LX_IOC_NRBITS
#define	LX_IOC_SIZESHIFT	(LX_IOC_TYPESHIFT + LX_IOC_TYPEBITS)
#define	LX_IOC_DIRSHIFT		(LX_IOC_SIZESHIFT + LX_IOC_SIZEBITS)
#define	LX_IOC_NONE		0U
#define	LX_IOC_WRITE		1U
#define	LX_IOC_READ		2U

#define	LX_IOC(dir, type, nr, size) ((uint32_t)( \
	((dir) << LX_IOC_DIRSHIFT) | ((uint32_t)(type) << LX_IOC_TYPESHIFT) | \
	((nr) << LX_IOC_NRSHIFT) | ((uint32_t)(size) << LX_IOC_SIZESHIFT)))
#define	LX_IO(type, nr)		LX_IOC(LX_IOC_NONE, (type), (nr), 0)
#define	LX_IOW(type, nr, data)	LX_IOC(LX_IOC_WRITE, (type), (nr), \
	sizeof (data))
#define	LX_IOR(type, nr, data)	LX_IOC(LX_IOC_READ, (type), (nr), \
	sizeof (data))
#define	LX_IOC_TYPE(cmd)	(((uint32_t)(cmd) >> LX_IOC_TYPESHIFT) & 0xff)
#define	LX_IOC_NR(cmd)		(((uint32_t)(cmd) >> LX_IOC_NRSHIFT) & 0xff)
#define	LX_IOC_SIZE(cmd)	(((uint32_t)(cmd) >> LX_IOC_SIZESHIFT) & \
	((1U << LX_IOC_SIZEBITS) - 1))

#define	LX_UINPUT_IOCTL_BASE	'U'
#define	LX_UI_DEV_CREATE	LX_IO(LX_UINPUT_IOCTL_BASE, 1)
#define	LX_UI_DEV_DESTROY	LX_IO(LX_UINPUT_IOCTL_BASE, 2)
#define	LX_UI_DEV_SETUP		LX_IOW(LX_UINPUT_IOCTL_BASE, 3, \
	lx_uinput_setup_t)
#define	LX_UI_ABS_SETUP		LX_IOW(LX_UINPUT_IOCTL_BASE, 4, \
	lx_uinput_abs_setup_t)
#define	LX_UI_SET_EVBIT		LX_IOW(LX_UINPUT_IOCTL_BASE, 100, int)
#define	LX_UI_SET_KEYBIT	LX_IOW(LX_UINPUT_IOCTL_BASE, 101, int)
#define	LX_UI_SET_RELBIT	LX_IOW(LX_UINPUT_IOCTL_BASE, 102, int)
#define	LX_UI_SET_ABSBIT	LX_IOW(LX_UINPUT_IOCTL_BASE, 103, int)
#define	LX_UI_SET_MSCBIT	LX_IOW(LX_UINPUT_IOCTL_BASE, 104, int)
#define	LX_UI_SET_LEDBIT	LX_IOW(LX_UINPUT_IOCTL_BASE, 105, int)
#define	LX_UI_SET_SNDBIT	LX_IOW(LX_UINPUT_IOCTL_BASE, 106, int)
#define	LX_UI_SET_FFBIT		LX_IOW(LX_UINPUT_IOCTL_BASE, 107, int)
#define	LX_UI_SET_SWBIT		LX_IOW(LX_UINPUT_IOCTL_BASE, 109, int)
#define	LX_UI_SET_PROPBIT	LX_IOW(LX_UINPUT_IOCTL_BASE, 110, int)
#define	LX_UI_GET_VERSION	LX_IOR(LX_UINPUT_IOCTL_BASE, 45, uint32_t)

#define	LX_EVDEV_IOCTL_BASE	'E'
#define	LX_EVIOCGVERSION	LX_IOR(LX_EVDEV_IOCTL_BASE, 0x01, int)
#define	LX_EVIOCGID		LX_IOR(LX_EVDEV_IOCTL_BASE, 0x02, \
	lx_input_id_t)
#define	LX_EVIOCGRAB		LX_IOW(LX_EVDEV_IOCTL_BASE, 0x90, int)
#define	LX_EVIOCSCLOCKID	LX_IOW(LX_EVDEV_IOCTL_BASE, 0xa0, int)

#define	LX_EV_VERSION		0x010001
#define	LX_UINPUT_VERSION	5

#ifdef _KERNEL
extern boolean_t lx_input_get_config(zoneid_t, uint_t, lx_input_config_t *);
extern int lx_input_trigger_uevent(zoneid_t, uint_t, boolean_t,
    const char *);
extern int lx_input_trigger_uinput_uevent(zoneid_t, const char *);
#endif

#ifdef __cplusplus
}
#endif

#endif /* _SYS_LX_INPUT_H */
