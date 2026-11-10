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
 * Copyright 2017 Joyent, Inc.
 * Copyright 2026 Jonathan Wagner.
 */

/*
 * lx_sysfs -- a Linux-compatible /sys for the LX brand
 */

#include <vm/seg_vn.h>
#include <sys/sdt.h>
#include <sys/strlog.h>
#include <sys/stropts.h>
#include <sys/cmn_err.h>
#include <sys/lx_brand.h>
#include <sys/x86_archext.h>
#include <sys/archsystm.h>
#include <sys/fp.h>
#include <sys/pool_pset.h>
#include <sys/pset.h>
#include <sys/zone.h>
#include <sys/pghw.h>
#include <sys/vfs_opreg.h>
#include <sys/param.h>
#include <sys/utsname.h>
#include <sys/lx_misc.h>
#include <sys/lx_input.h>
#include <sys/brand.h>
#include <sys/cred_impl.h>
#include <sys/tihdr.h>
#include <sys/sunddi.h>
#include <sys/vnode.h>
#include <sys/netstack.h>
#include <sys/ethernet.h>
#include <inet/ip_arp.h>

#include "lx_sysfs.h"

/*
 * Pointer to the vnode ops vector for this fs.
 * This is instantiated in lxsys_init() in lx_sysvfsops.c
 */
vnodeops_t *lxsys_vnodeops;

static int lxsys_open(vnode_t **, int, cred_t *, caller_context_t *);
static int lxsys_close(vnode_t *, int, int, offset_t, cred_t *,
    caller_context_t *);
static int lxsys_read(vnode_t *, uio_t *, int, cred_t *, caller_context_t *);
static int lxsys_getattr(vnode_t *, vattr_t *, int, cred_t *,
    caller_context_t *);
static int lxsys_access(vnode_t *, int, int, cred_t *, caller_context_t *);
static int lxsys_lookup(vnode_t *, char *, vnode_t **,
    pathname_t *, int, vnode_t *, cred_t *, caller_context_t *, int *,
    pathname_t *);
static int lxsys_readdir(vnode_t *, uio_t *, cred_t *, int *,
    caller_context_t *, int);
static int lxsys_readlink(vnode_t *, uio_t *, cred_t *, caller_context_t *);
static int lxsys_cmp(vnode_t *, vnode_t *, caller_context_t *);
static int lxsys_sync(void);
static void lxsys_inactive(vnode_t *, cred_t *, caller_context_t *);

static vnode_t *lxsys_lookup_static(lxsys_node_t *, char *);
static vnode_t *lxsys_lookup_class_netdir(lxsys_node_t *, char *);
static vnode_t *lxsys_lookup_devices_virtual_netdir(lxsys_node_t *, char *);
static vnode_t *lxsys_lookup_blockdir(lxsys_node_t *, char *);
static vnode_t *lxsys_lookup_devices_zfsdir(lxsys_node_t *, char *);
static vnode_t *lxsys_lookup_devices_syscpu(lxsys_node_t *, char *);
static vnode_t *lxsys_lookup_devices_syscpuinfo(lxsys_node_t *, char *);
static vnode_t *lxsys_lookup_devices_sysnode(lxsys_node_t *, char *);
static vnode_t *lxsys_lookup_class_input(lxsys_node_t *, char *);
static vnode_t *lxsys_lookup_devices_input(lxsys_node_t *, char *);

static int lxsys_read_static(lxsys_node_t *, lxsys_uiobuf_t *);
static int lxsys_read_devices_virtual_net(lxsys_node_t *, lxsys_uiobuf_t *);
static int lxsys_read_devices_zfs_block(lxsys_node_t *, lxsys_uiobuf_t *);
static int lxsys_read_devices_syscpu(lxsys_node_t *, lxsys_uiobuf_t *);
static int lxsys_read_devices_sysnode(lxsys_node_t *, lxsys_uiobuf_t *);
static int lxsys_read_devices_input(lxsys_node_t *, lxsys_uiobuf_t *);

static int lxsys_readdir_devices_syscpu(lxsys_node_t *, uio_t *, int *);
static int lxsys_readdir_devices_syscpuinfo(lxsys_node_t *, uio_t *, int *);
static int lxsys_readdir_devices_sysnode(lxsys_node_t *, uio_t *, int *);
static int lxsys_readdir_static(lxsys_node_t *, uio_t *, int *);
static int lxsys_readdir_class_netdir(lxsys_node_t *, uio_t *, int *);
static int lxsys_readdir_devices_virtual_netdir(lxsys_node_t *, uio_t *, int *);
static int lxsys_readdir_blockdir(lxsys_node_t *, uio_t *, int *);
static int lxsys_readdir_devices_zfsdir(lxsys_node_t *, uio_t *, int *);
static int lxsys_readdir_class_input(lxsys_node_t *, uio_t *, int *);
static int lxsys_readdir_devices_input(lxsys_node_t *, uio_t *, int *);

static int lxsys_readlink_class_net(lxsys_node_t *, char *, size_t);
static int lxsys_readlink_block(lxsys_node_t *, char *, size_t);
static int lxsys_readlink_class_input(lxsys_node_t *, char *, size_t);
static int lxsys_readlink_devices_input(lxsys_node_t *, char *, size_t);

/*
 * The lx /sys vnode operations vector
 */
const fs_operation_def_t lxsys_vnodeops_template[] = {
	VOPNAME_OPEN,		{ .vop_open = lxsys_open },
	VOPNAME_CLOSE,		{ .vop_close = lxsys_close },
	VOPNAME_READ,		{ .vop_read = lxsys_read },
	VOPNAME_GETATTR,	{ .vop_getattr = lxsys_getattr },
	VOPNAME_ACCESS,		{ .vop_access = lxsys_access },
	VOPNAME_LOOKUP,		{ .vop_lookup = lxsys_lookup },
	VOPNAME_READDIR,	{ .vop_readdir = lxsys_readdir },
	VOPNAME_READLINK,	{ .vop_readlink = lxsys_readlink },
	VOPNAME_FSYNC,		{ .error = lxsys_sync },
	VOPNAME_SEEK,		{ .error = lxsys_sync },
	VOPNAME_INACTIVE,	{ .vop_inactive = lxsys_inactive },
	VOPNAME_CMP,		{ .vop_cmp = lxsys_cmp },
	NULL,			NULL
};

typedef enum lxsys_cpu_state {
	LXSYS_CPU_ON,		/* online */
	LXSYS_CPU_OFF,		/* offline */
	LXSYS_CPU_ANY,		/* don't care */
} lxsys_cpu_state_t;

static void lxsys_format_cpu(char *, int, lxsys_cpu_state_t);

/*
 * Sysfs Inode format:
 * 0000AABBBBCC
 *
 * AA - TYPE
 * BBBB - INSTANCE
 * CC - ENDPOINT
 *
 * Where TYPE is one of:
 * 1 - SYS_STATIC
 * 2 - SYS_CLASS_NET
 * 3 - SYS_DEV_NET
 * 4 - SYS_BLOCK
 * 5 - SYS_DEV_ZFS
 * 6 - SYS_DEV_SYS_CPU
 * 7 - SYS_DEV_SYS_CPUINFO
 * 8 - SYS_DEV_SYS_NODE
 *
 * Static entries will have assigned INSTANCE identifiers:
 * - 0x00: /sys
 * - 0x01: /sys/class
 * - 0x02: /sys/devices
 * - 0x03: /sys/fs
 * - 0x04: /sys/class/net
 * - 0x05: /sys/devices/virtual
 * - 0x06: /sys/devices/system
 * - 0x07: /sys/fs/cgroup
 * - 0x08: /sys/devices/virtual/net
 * - 0x09: /sys/block
 * - 0x0a: /sys/devices/zfs
 * - 0x0b: /sys/devices/system/cpu
 * - 0x0c: /sys/devices/system/node
 * - 0x0d: /sys/bus
 * - 0x0e: /sys/class/input
 * - 0x0f: /sys/devices/virtual/input
 *
 * Dynamic /sys/class/net/<interface> symlinks will use an INSTANCE derived
 * from the corresonding ifindex.
 *
 * Dynamic /sys/devices/virtual/net/<interface>/<entries> directories will use
 * an INSTANCE derived from the ifindex and statically assigned ENDPOINT IDs
 * for the contained entries.
 *
 * Dynamic /sys/block/<dev> symlinks will use an INSTANCE derived from the
 * device major and instance from records listed in kstat or zvols.
 *
 * Dynamic /sys/devices/zfs/<dev> directories will use an INSTANCE derived from
 * the emulated minor number.
 *
 * Semi-static/Dynamic /sys/devices/system/cpu contains the fixed 'kernel_max',
 * 'offline', 'online', 'possible', and 'present' files, and a dynamic set of
 * cpuN subdirectories. All of these are dynamic nodes.
 *
 * Static /sys/devices/system/node/node0 currently only contains a
 * static cpulist file, but will likely need future dynamic entries for cpuN
 * symlinks, and perhaps other static files. By only providing 'node0' we
 * pretend that there is only a single NUMA node available to a zone (trying to
 * be NUMA-aware inside a zone is generally not going to work anyway).
 * If dynamic entries are added under node0, it must be converted to the
 * semi-static/dynamic approach as used under /sys/devices/system/cpu.
 *
 * The dyn_ino_type table must be updated whenever a new static instance is
 * defined.
 */

#define	LXSYS_INST_CLASSDIR			0x1
#define	LXSYS_INST_DEVICESDIR			0x2
#define	LXSYS_INST_FSDIR			0x3
#define	LXSYS_INST_CLASS_NETDIR			0x4
#define	LXSYS_INST_DEVICES_VIRTUALDIR		0x5
#define	LXSYS_INST_DEVICES_SYSTEMDIR		0x6
#define	LXSYS_INST_FS_CGROUPDIR			0x7
#define	LXSYS_INST_DEVICES_VIRTUAL_NETDIR	0x8
#define	LXSYS_INST_BLOCKDIR			0x9
#define	LXSYS_INST_DEVICES_ZFSDIR		0xa
#define	LXSYS_INST_DEVICES_SYSCPU		0xb
#define	LXSYS_INST_DEVICES_SYSNODE		0xc
#define	LXSYS_INST_BUSDIR			0xd
#define	LXSYS_INST_CLASS_INPUTDIR		0xe
#define	LXSYS_INST_DEVICES_VIRTUAL_INPUTDIR	0xf
#define	LXSYS_INST_MAX		LXSYS_INST_DEVICES_VIRTUAL_INPUTDIR

/*
 * These are of dynamic type (LXSYS_DEV_SYS_CPU), but essentially fixed
 * instances. Under /sys/devices/system/cpu we have: kernel_max, offline,
 * online, possible and present. We also have a dynamic set of cpuN subdirs.
 * The cpuN subdirs are actually of type LXSYS_DEV_SYS_CPUINFO, so we can use
 * the following instance IDs for the fixed files.
 */
#define	LXSYS_INST_DEV_SYSCPU_KMAX		0x1
#define	LXSYS_INST_DEV_SYSCPU_OFFLINE		0x2
#define	LXSYS_INST_DEV_SYSCPU_ONLINE		0x3
#define	LXSYS_INST_DEV_SYSCPU_POSSIBLE		0x4
#define	LXSYS_INST_DEV_SYSCPU_PRESENT		0x5

/*
 * This array is used for directory inode correction in lxsys_readdir_common
 * when a directory's static-type entry is actually a dynamic-type.
 */
static int dyn_ino_type [] = {
	0,				/* invalid */
	0,				/* LXSYS_INST_CLASSDIR */
	0,				/* LXSYS_INST_DEVICESDIR */
	0,				/* LXSYS_INST_FSDIR */
	LXSYS_CLASS_NET,		/* LXSYS_INST_CLASS_NETDIR */
	0,				/* LXSYS_INST_DEVICES_VIRTUALDIR */
	0,				/* LXSYS_INST_DEVICES_SYSTEMDIR */
	0,				/* LXSYS_INST_FS_CGROUPDIR */
	LXSYS_DEV_NET,			/* LXSYS_INST_DEV_VIRTUAL_NETDIR */
	LXSYS_BLOCK,			/* LXSYS_INST_BLOCKDIR */
	LXSYS_DEV_ZFS,			/* LXSYS_INST_DEVICES_ZFSDIR */
	LXSYS_DEV_SYS_CPU,		/* LXSYS_INST_DEVICES_SYSCPU */
	LXSYS_DEV_SYS_NODE,		/* LXSYS_INST_DEV_SYSNODE */
	0,				/* LXSYS_INST_BUSDIR */
	LXSYS_CLASS_INPUT,		/* LXSYS_INST_CLASS_INPUTDIR */
	LXSYS_DEV_INPUT,		/* LXSYS_INST_DEV_VIRTUAL_INPUTDIR */
};
#define	DYN_INO_LEN \
	(sizeof (dyn_ino_type) / sizeof ((dyn_ino_type)[0]))

/*
 * file contents of an lx /sys directory.
 */
static lxsys_dirent_t dirlist_root[] = {
	{ LXSYS_INST_BLOCKDIR,		"block" },
	{ LXSYS_INST_BUSDIR,		"bus" },
	{ LXSYS_INST_CLASSDIR,		"class" },
	{ LXSYS_INST_DEVICESDIR,	"devices" },
	{ LXSYS_INST_FSDIR,		"fs" }
};
static lxsys_dirent_t dirlist_class[] = {
	{ LXSYS_INST_CLASS_INPUTDIR,	"input" },
	{ LXSYS_INST_CLASS_NETDIR,	"net" }
};
static lxsys_dirent_t dirlist_fs[] = {
	{ LXSYS_INST_FS_CGROUPDIR,	"cgroup" }
};
static lxsys_dirent_t dirlist_devices[] = {
	{ LXSYS_INST_DEVICES_SYSTEMDIR,		"system" },
	{ LXSYS_INST_DEVICES_VIRTUALDIR,	"virtual" },
	{ LXSYS_INST_DEVICES_ZFSDIR,		"zfs" }
};
static lxsys_dirent_t dirlist_devices_virtual[] = {
	{ LXSYS_INST_DEVICES_VIRTUAL_INPUTDIR,	"input" },
	{ LXSYS_INST_DEVICES_VIRTUAL_NETDIR,	"net" }
};

static lxsys_dirent_t dirlist_devices_system[] = {
	{ LXSYS_INST_DEVICES_SYSCPU,	"cpu" },
	{ LXSYS_INST_DEVICES_SYSNODE,	"node" }
};

#define	LXSYS_ENDP_NET_ADDRESS	1
#define	LXSYS_ENDP_NET_ADDRLEN	2
#define	LXSYS_ENDP_NET_FLAGS	3
#define	LXSYS_ENDP_NET_IFINDEX	4
#define	LXSYS_ENDP_NET_MTU	5
#define	LXSYS_ENDP_NET_TXQLEN	6
#define	LXSYS_ENDP_NET_TYPE	7

#define	LXSYS_ENDP_BLOCK_DEVICE	1

#define	LXSYS_ENDP_NODE_CPULIST	1
#define	LXSYS_ENDP_NODE_CPUMAP	2

#define	LXSYS_ENDP_INPUT_NAME		1
#define	LXSYS_ENDP_INPUT_PHYS		2
#define	LXSYS_ENDP_INPUT_UNIQ		3
#define	LXSYS_ENDP_INPUT_PROPERTIES	4
#define	LXSYS_ENDP_INPUT_IDDIR		5
#define	LXSYS_ENDP_INPUT_CAPDIR		6
#define	LXSYS_ENDP_INPUT_EVENTDIR	7
#define	LXSYS_ENDP_INPUT_UEVENT		8
#define	LXSYS_ENDP_INPUT_ID_BUS		10
#define	LXSYS_ENDP_INPUT_ID_VENDOR	11
#define	LXSYS_ENDP_INPUT_ID_PRODUCT	12
#define	LXSYS_ENDP_INPUT_ID_VERSION	13
#define	LXSYS_ENDP_INPUT_CAP_EV		20
#define	LXSYS_ENDP_INPUT_CAP_KEY	21
#define	LXSYS_ENDP_INPUT_CAP_REL	22
#define	LXSYS_ENDP_INPUT_CAP_ABS	23
#define	LXSYS_ENDP_INPUT_CAP_MSC	24
#define	LXSYS_ENDP_INPUT_CAP_SW		25
#define	LXSYS_ENDP_INPUT_CAP_LED	26
#define	LXSYS_ENDP_INPUT_CAP_SND	27
#define	LXSYS_ENDP_INPUT_CAP_FF		28
#define	LXSYS_ENDP_INPUT_EVENT_DEV	40
#define	LXSYS_ENDP_INPUT_EVENT_UEVENT	41
#define	LXSYS_ENDP_INPUT_EVENT_DEVICE	42

#define	LXSYS_CLASS_INPUT_ENDPOINT(slot)	((slot) * 2 + 1)
#define	LXSYS_CLASS_EVENT_ENDPOINT(slot)	((slot) * 2 + 2)

static lxsys_dirent_t dirlist_devices_virtual_net[] = {
	{ LXSYS_ENDP_NET_ADDRESS,	"address" },
	{ LXSYS_ENDP_NET_ADDRLEN,	"addr_len" },
	{ LXSYS_ENDP_NET_FLAGS,		"flags" },
	{ LXSYS_ENDP_NET_IFINDEX,	"ifindex" },
	{ LXSYS_ENDP_NET_MTU,		"mtu" },
	{ LXSYS_ENDP_NET_TXQLEN,	"tx_queue_len" },
	{ LXSYS_ENDP_NET_TYPE,		"type" }
};

static lxsys_dirent_t dirlist_devices_zfs_block[] = {
	{ LXSYS_ENDP_BLOCK_DEVICE,	"device" }
};

static lxsys_dirent_t dirlist_devices_sysnode[] = {
	{ LXSYS_ENDP_NODE_CPULIST,	"cpulist" },
	{ LXSYS_ENDP_NODE_CPUMAP,	"cpumap" }
};

static lxsys_dirent_t dirlist_devices_input[] = {
	{ LXSYS_ENDP_INPUT_CAPDIR,		"capabilities" },
	{ LXSYS_ENDP_INPUT_IDDIR,		"id" },
	{ LXSYS_ENDP_INPUT_NAME,		"name" },
	{ LXSYS_ENDP_INPUT_PHYS,		"phys" },
	{ LXSYS_ENDP_INPUT_PROPERTIES,		"properties" },
	{ LXSYS_ENDP_INPUT_UEVENT,		"uevent" },
	{ LXSYS_ENDP_INPUT_UNIQ,		"uniq" }
};

static lxsys_dirent_t dirlist_devices_input_id[] = {
	{ LXSYS_ENDP_INPUT_ID_BUS,		"bustype" },
	{ LXSYS_ENDP_INPUT_ID_PRODUCT,		"product" },
	{ LXSYS_ENDP_INPUT_ID_VENDOR,		"vendor" },
	{ LXSYS_ENDP_INPUT_ID_VERSION,		"version" }
};

static lxsys_dirent_t dirlist_devices_input_cap[] = {
	{ LXSYS_ENDP_INPUT_CAP_ABS,		"abs" },
	{ LXSYS_ENDP_INPUT_CAP_EV,		"ev" },
	{ LXSYS_ENDP_INPUT_CAP_FF,		"ff" },
	{ LXSYS_ENDP_INPUT_CAP_KEY,		"key" },
	{ LXSYS_ENDP_INPUT_CAP_LED,		"led" },
	{ LXSYS_ENDP_INPUT_CAP_MSC,		"msc" },
	{ LXSYS_ENDP_INPUT_CAP_REL,		"rel" },
	{ LXSYS_ENDP_INPUT_CAP_SND,		"snd" },
	{ LXSYS_ENDP_INPUT_CAP_SW,		"sw" }
};

static lxsys_dirent_t dirlist_devices_input_event[] = {
	{ LXSYS_ENDP_INPUT_EVENT_DEV,		"dev" },
	{ LXSYS_ENDP_INPUT_EVENT_DEVICE,	"device" },
	{ LXSYS_ENDP_INPUT_EVENT_UEVENT,	"uevent" }
};

#define	SYSDIRLISTSZ(l)	(sizeof (l) / sizeof ((l)[0]))

#define	SYSDLENT(i, l)	{ i, l, SYSDIRLISTSZ(l) }
static lxsys_dirlookup_t lxsys_dirlookup[] = {
	SYSDLENT(LXSYS_INST_ROOT, dirlist_root),
	SYSDLENT(LXSYS_INST_CLASSDIR, dirlist_class),
	SYSDLENT(LXSYS_INST_FSDIR, dirlist_fs),
	{ LXSYS_INST_FS_CGROUPDIR, NULL, 0 },
	SYSDLENT(LXSYS_INST_DEVICESDIR, dirlist_devices),
	SYSDLENT(LXSYS_INST_DEVICES_SYSTEMDIR, dirlist_devices_system),
	SYSDLENT(LXSYS_INST_DEVICES_VIRTUALDIR, dirlist_devices_virtual),
	{ LXSYS_INST_CLASS_INPUTDIR, NULL, 0 },
	{ LXSYS_INST_DEVICES_VIRTUAL_INPUTDIR, NULL, 0 },
	SYSDLENT(LXSYS_INST_DEVICES_SYSNODE, dirlist_devices_sysnode),
	{ LXSYS_INST_BUSDIR, NULL, 0 },
};


/*
 * Array of lookup functions, indexed by lx /sys file type.
 */
static vnode_t *(*lxsys_lookup_function[LXSYS_MAXTYPE])() = {
	NULL,					/* LXSYS_NONE		*/
	lxsys_lookup_static,			/* LXSYS_STATIC		*/
	lxsys_lookup_class_netdir,		/* LXSYS_CLASS_NET	*/
	lxsys_lookup_devices_virtual_netdir,	/* LXSYS_DEV_NET	*/
	lxsys_lookup_blockdir,			/* LXSYS_BLOCK		*/
	lxsys_lookup_devices_zfsdir,		/* LXSYS_DEV_ZFS	*/
	lxsys_lookup_devices_syscpu,		/* LXSYS_DEV_SYS_CPU	*/
	lxsys_lookup_devices_syscpuinfo,	/* LXSYS_DEV_SYS_CPUINFO */
	lxsys_lookup_devices_sysnode,		/* LXSYS_DEV_SYS_NODE	*/
	lxsys_lookup_class_input,		/* LXSYS_CLASS_INPUT	*/
	lxsys_lookup_devices_input,		/* LXSYS_DEV_INPUT	*/
};

/*
 * Array of readdir functions, indexed by /sys file type.
 */
static int (*lxsys_readdir_function[LXSYS_MAXTYPE])() = {
	NULL,					/* LXSYS_NONE		*/
	lxsys_readdir_static,			/* LXSYS_STATIC		*/
	lxsys_readdir_class_netdir,		/* LXSYS_CLASS_NET	*/
	lxsys_readdir_devices_virtual_netdir,	/* LXSYS_DEV_NET	*/
	lxsys_readdir_blockdir,			/* LXSYS_BLOCK		*/
	lxsys_readdir_devices_zfsdir,		/* LXSYS_DEV_ZFS	*/
	lxsys_readdir_devices_syscpu,		/* LXSYS_DEV_SYS_CPU	*/
	lxsys_readdir_devices_syscpuinfo,	/* LXSYS_DEV_SYS_CPUINFO */
	lxsys_readdir_devices_sysnode,		/* LXSYS_DEV_SYS_NODE	*/
	lxsys_readdir_class_input,		/* LXSYS_CLASS_INPUT	*/
	lxsys_readdir_devices_input,		/* LXSYS_DEV_INPUT	*/
};

/*
 * Array of read functions, indexed by /sys file type.
 */
static int (*lxsys_read_function[LXSYS_MAXTYPE])() = {
	NULL,					/* LXSYS_NONE		*/
	lxsys_read_static,			/* LXSYS_STATIC		*/
	NULL,					/* LXSYS_CLASS_NET	*/
	lxsys_read_devices_virtual_net,		/* LXSYS_DEV_NET	*/
	NULL,					/* LXSYS_BLOCK		*/
	lxsys_read_devices_zfs_block,		/* LXSYS_DEV_ZFS	*/
	lxsys_read_devices_syscpu,		/* LXSYS_DEV_SYS_CPU	*/
	NULL,					/* LXSYS_DEV_SYS_CPUINFO */
	lxsys_read_devices_sysnode,		/* LXSYS_DEV_SYS_NODE	*/
	NULL,					/* LXSYS_CLASS_INPUT	*/
	lxsys_read_devices_input,		/* LXSYS_DEV_INPUT	*/
};

/*
 * Array of readlink functions, indexed by /sys file type.
 */
static int (*lxsys_readlink_function[LXSYS_MAXTYPE])() = {
	NULL,					/* LXSYS_NONE		*/
	NULL,					/* LXSYS_STATIC		*/
	lxsys_readlink_class_net,		/* LXSYS_CLASS_NET	*/
	NULL,					/* LXSYS_DEV_NET	*/
	lxsys_readlink_block,			/* LXSYS_BLOCK		*/
	NULL,					/* LXSYS_DEV_ZFS	*/
	NULL,					/* LXSYS_DEV_SYS_CPU	*/
	NULL,					/* LXSYS_DEV_SYS_CPUINFO */
	NULL,					/* LXSYS_DEV_SYS_NODE	*/
	lxsys_readlink_class_input,		/* LXSYS_CLASS_INPUT	*/
	lxsys_readlink_devices_input,		/* LXSYS_DEV_INPUT	*/
};

/*
 * Given one of our inodes, return the vnode type.
 *
 * lxsys_getnode will always set the vnode type to VDIR. It expects the
 * caller (normally the lookup functions) to fix the type. Those same rules are
 * encoded here for our inode-to-type translation.
 */
int
lxsys_ino_get_type(ino_t ino)
{
	lxsys_nodetype_t type;
	unsigned int instance;
	unsigned int endpoint;

	type = (ino & 0xff000000) >> 24;
	instance = (ino & 0xffff00) >> 8;
	endpoint = (ino & 0xff);

	if (type == LXSYS_STATIC && instance > LXSYS_INST_MAX)
		return (VNON);

	/* Validate non-static node types */
	if (type != LXSYS_STATIC &&
	    (type <= LXSYS_STATIC || type >= LXSYS_MAXTYPE)) {
		return (VNON);
	}

	if (type != LXSYS_STATIC) {
		/* Non-static node types */
		switch (type) {
		case LXSYS_CLASS_NET:
			if (instance != 0) {
				return (VLNK);
			}
			break;
		case LXSYS_DEV_NET:
			/*
			 * /sys/devices/virtual/net usually has the eth0 and
			 * lo directories. Each network device directory is an
			 * instances with a 0 endpoint. The files within
			 * that directory have a non-0 endpoint.
			 */
			if (endpoint != 0) {
				return (VREG);
			}
			break;
		case LXSYS_BLOCK:
			if (instance != 0) {
				return (VLNK);
			}
			break;
		case LXSYS_DEV_ZFS:
			/*
			 * /sys/devices/zfs usually has the zfsds0 directory
			 * instance with a 0 endpoint. The device file within
			 * that directory has a non-0 endpoint.
			 */
			if (endpoint != 0) {
				return (VREG);
			}
			break;
		case LXSYS_DEV_SYS_CPU:
			if (instance != 0) {
				return (VREG);
			}
			break;
		case LXSYS_DEV_SYS_CPUINFO:
			/*
			 * There is an instance of /sys/devices/system/cpu/cpuN
			 * for each CPU. These have an instance per CPU and
			 * currently the endpoint is 0 since there is nothing
			 * underneath the cpuN subdirectories. Future
			 * regular file entries are likely to be added there.
			 */
			if (endpoint != 0) {
				return (VREG);
			}
			break;
		case LXSYS_DEV_SYS_NODE:
			/*
			 * /sys/devices/system/node has the node0 directory
			 * instance with a 0 endpoint. The cpulist file within
			 * that directory has a non-0 endpoint.
			 */
			if (endpoint != 0) {
				return (VREG);
			}
			break;
		case LXSYS_CLASS_INPUT:
			if (endpoint != 0)
				return (VLNK);
			break;
		case LXSYS_DEV_INPUT:
			if (endpoint == LXSYS_ENDP_INPUT_IDDIR ||
			    endpoint == LXSYS_ENDP_INPUT_CAPDIR ||
			    endpoint == LXSYS_ENDP_INPUT_EVENTDIR)
				break;
			if (endpoint == LXSYS_ENDP_INPUT_EVENT_DEVICE)
				return (VLNK);
			if (endpoint != 0)
				return (VREG);
			break;
		default:
			break;
		}
	}
	return (VDIR);
}

/*
 * lxsys_open(): Vnode operation for VOP_OPEN()
 */
/* ARGSUSED */
static int
lxsys_open(vnode_t **vpp, int flag, cred_t *cr, caller_context_t *ct)
{
	/*
	 * We only allow reading in this file system
	 */
	if (flag & FWRITE)
		return (EROFS);

	return (0);
}


/*
 * lxsys_close(): Vnode operation for VOP_CLOSE()
 */
/* ARGSUSED */
static int
lxsys_close(vnode_t *vp, int flag, int count, offset_t offset, cred_t *cr,
    caller_context_t *ct)
{
	return (0);
}


/*
 * lxsys_read(): Vnode operation for VOP_READ()
 * All we currently have in this fs are directories.
 */
/* ARGSUSED */
static int
lxsys_read(vnode_t *vp, uio_t *uiop, int ioflag, cred_t *cr,
    caller_context_t *ct)
{
	lxsys_node_t *lnp = VTOLXS(vp);
	lxsys_nodetype_t type = lnp->lxsys_type;
	int (*rlfunc)();
	int error;
	lxsys_uiobuf_t *luio;

	VERIFY(type > LXSYS_NONE && type < LXSYS_MAXTYPE);

	if (vp->v_type == VDIR) {
		return (EISDIR);
	}

	rlfunc = lxsys_read_function[type];
	if (rlfunc != NULL) {
		luio = lxsys_uiobuf_new(uiop);
		if ((error = rlfunc(lnp, luio)) == 0) {
			error = lxsys_uiobuf_flush(luio);
		}
		lxsys_uiobuf_free(luio);
	} else {
		error = EIO;
	}

	return (error);
}

/*
 * lxsys_getattr(): Vnode operation for VOP_GETATTR()
 */
/* ARGSUSED */
static int
lxsys_getattr(vnode_t *vp, vattr_t *vap, int flags, cred_t *cr,
    caller_context_t *ct)
{
	register lxsys_node_t *lxsnp = VTOLXS(vp);

	/* Default attributes, that may be overridden below */
	bzero(vap, sizeof (*vap));
	vap->va_atime = vap->va_mtime = vap->va_ctime = lxsnp->lxsys_time;
	vap->va_nlink = 1;
	vap->va_type = vp->v_type;
	vap->va_mode = lxsnp->lxsys_mode;
	vap->va_fsid = vp->v_vfsp->vfs_dev;
	vap->va_blksize = DEV_BSIZE;
	vap->va_uid = lxsnp->lxsys_uid;
	vap->va_gid = lxsnp->lxsys_gid;
	vap->va_nodeid = lxsnp->lxsys_ino;

	vap->va_nblocks = (fsblkcnt64_t)btod(vap->va_size);
	return (0);
}

/*
 * lxsys_access(): Vnode operation for VOP_ACCESS()
 */
/* ARGSUSED */
static int
lxsys_access(vnode_t *vp, int mode, int flags, cred_t *cr, caller_context_t *ct)
{
	lxsys_node_t *lxsnp = VTOLXS(vp);
	int shift = 0;

	/*
	 * Although our lx sysfs is basically a read only file system, Linux
	 * expects it to be writable so we can't just error if (mode & VWRITE).
	 */

	/* If user is root allow access regardless of permission bits */
	if (secpolicy_proc_access(cr) == 0)
		return (0);

	/*
	 * Access check is based on only one of owner, group, public.  If not
	 * owner, then check group.  If not a member of the group, then check
	 * public access.
	 */
	if (crgetuid(cr) != lxsnp->lxsys_uid) {
		shift += 3;
		if (!groupmember((uid_t)lxsnp->lxsys_gid, cr))
			shift += 3;
	}

	mode &= ~(lxsnp->lxsys_mode << shift);

	if (mode == 0)
		return (0);

	return (EACCES);
}

/*
 * lxsys_lookup(): Vnode operation for VOP_LOOKUP()
 */
/* ARGSUSED */
static int
lxsys_lookup(vnode_t *dp, char *comp, vnode_t **vpp, pathname_t *pathp,
    int flags, vnode_t *rdir, cred_t *cr, caller_context_t *ct,
    int *direntflags, pathname_t *realpnp)
{
	lxsys_node_t *lxsnp = VTOLXS(dp);
	lxsys_nodetype_t type = lxsnp->lxsys_type;
	int error;

	VERIFY(dp->v_type == VDIR);
	VERIFY(type > LXSYS_NONE && type < LXSYS_MAXTYPE);

	/*
	 * restrict lookup permission to owner or root
	 */
	if ((error = lxsys_access(dp, VEXEC, 0, cr, ct)) != 0) {
		return (error);
	}

	/*
	 * Just return the parent vnode if that's where we are trying to go.
	 */
	if (strcmp(comp, "..") == 0) {
		VN_HOLD(lxsnp->lxsys_parentvp);
		*vpp = lxsnp->lxsys_parentvp;
		return (0);
	}

	/*
	 * Special handling for directory searches.  Note: null component name
	 * denotes that the current directory is being searched.
	 */
	if ((dp->v_type == VDIR) && (*comp == '\0' || strcmp(comp, ".") == 0)) {
		VN_HOLD(dp);
		*vpp = dp;
		return (0);
	}

	*vpp = (lxsys_lookup_function[type](lxsnp, comp));
	return ((*vpp == NULL) ? ENOENT : 0);
}

static lxsys_node_t *
lxsys_lookup_disk(lxsys_node_t *ldp, char *comp, lxsys_nodetype_t type)
{
	lxsys_node_t *lnp = NULL;
	lx_zone_data_t *lxzdata;
	lx_virt_disk_t *vd;

	lxzdata = ztolxzd(curproc->p_zone);
	if (lxzdata == NULL)
		return (NULL);
	ASSERT(lxzdata->lxzd_vdisks != NULL);

	vd = list_head(lxzdata->lxzd_vdisks);
	while (vd != NULL) {
		int inst = getminor(vd->lxvd_emul_dev) & 0xffff;

		if (strcmp(vd->lxvd_name, comp) == 0 && inst != 0) {
			lnp = lxsys_getnode(ldp->lxsys_vnode, type, inst, 0);
			break;
		}

		vd = list_next(lxzdata->lxzd_vdisks, vd);
	}

	return (lnp);
}

static vnode_t *
lxsys_lookup_static(lxsys_node_t *ldp, char *comp)
{
	lxsys_dirent_t *dirent = NULL;
	int i, len = 0;

	for (i = 0; i < SYSDIRLISTSZ(lxsys_dirlookup); i++) {
		if (ldp->lxsys_instance == lxsys_dirlookup[i].dl_instance) {
			dirent = lxsys_dirlookup[i].dl_list;
			len = lxsys_dirlookup[i].dl_length;
			break;
		}
	}
	if (dirent == NULL) {
		return (NULL);
	}

	for (i = 0; i < len; i++) {
		if (strncmp(comp, dirent[i].d_name, MAXPATHLEN) == 0) {
			lxsys_nodetype_t node_type = ldp->lxsys_type;
			unsigned int node_instance = 0;
			lxsys_node_t *lnp;

			switch (dirent[i].d_idnum) {
			case LXSYS_INST_BLOCKDIR:
				node_type = LXSYS_BLOCK;
				break;
			case LXSYS_INST_CLASS_NETDIR:
				node_type = LXSYS_CLASS_NET;
				break;
			case LXSYS_INST_CLASS_INPUTDIR:
				node_type = LXSYS_CLASS_INPUT;
				break;
			case LXSYS_INST_DEVICES_VIRTUAL_NETDIR:
				node_type = LXSYS_DEV_NET;
				break;
			case LXSYS_INST_DEVICES_VIRTUAL_INPUTDIR:
				node_type = LXSYS_DEV_INPUT;
				break;
			case LXSYS_INST_DEVICES_ZFSDIR:
				node_type = LXSYS_DEV_ZFS;
				break;
			case LXSYS_INST_DEVICES_SYSCPU:
				node_type = LXSYS_DEV_SYS_CPU;
				break;
			case LXSYS_INST_DEVICES_SYSNODE:
				node_type = LXSYS_DEV_SYS_NODE;
				break;
			default:
				/* Another static node */
				node_instance = dirent[i].d_idnum;
			}
			if (node_type == LXSYS_STATIC) {
				lnp = lxsys_getnode_static(ldp->lxsys_vnode,
				    node_instance);
			} else {
				lnp = lxsys_getnode(ldp->lxsys_vnode,
				    node_type, node_instance, 0);
			}
			return (lnp->lxsys_vnode);
		}
	}
	return (NULL);
}

static vnode_t *
lxsys_lookup_class_netdir(lxsys_node_t *ldp, char *comp)
{
	vnode_t *result = NULL;
	lxsys_node_t *lnp;
	netstack_t *ns;
	ip_stack_t *ipst;
	avl_tree_t *phytree;
	phyint_t *phyi;
	char ifname[LIFNAMSIZ];

	if (ldp->lxsys_type != LXSYS_CLASS_NET ||
	    ldp->lxsys_instance != 0) {
		/* Lookups only allowed at directory level */
		return (NULL);
	}

	(void) strncpy(ifname, comp, LIFNAMSIZ);
	lx_ifname_convert(ifname, LX_IF_TONATIVE);

	if ((ns = lxsys_netstack(ldp)) == NULL) {
		return (NULL);
	}
	ipst = ns->netstack_ip;
	rw_enter(&ipst->ips_ill_g_lock, RW_READER);

	phytree = &ipst->ips_phyint_g_list->phyint_list_avl_by_name;
	phyi = avl_find(phytree, ifname, NULL);
	if (phyi != NULL) {
		lnp = lxsys_getnode(ldp->lxsys_vnode, ldp->lxsys_type,
		    phyi->phyint_ifindex, 0);
		result = lnp->lxsys_vnode;
		result->v_type = VLNK;
	}

	rw_exit(&ipst->ips_ill_g_lock);
	netstack_rele(ns);

	return (result);
}

static boolean_t
lxsys_input_slot(lxsys_node_t *ldp, const char *comp, const char *prefix,
    uint_t *slotp)
{
	char *end;
	ulong_t slot;
	size_t length = strlen(prefix);

	if (strncmp(comp, prefix, length) != 0 || comp[length] == '\0' ||
	    ddi_strtoul(comp + length, &end, 10, &slot) != 0 ||
	    *end != '\0' || slot >= LX_INPUT_EVENT_COUNT)
		return (B_FALSE);
	if (!lx_input_get_config(LXSTOZ(ldp)->zone_id, (uint_t)slot, NULL))
		return (B_FALSE);
	*slotp = (uint_t)slot;
	return (B_TRUE);
}

static vnode_t *
lxsys_lookup_class_input(lxsys_node_t *ldp, char *comp)
{
	lxsys_node_t *lnp;
	uint_t endpoint;
	uint_t slot;

	if (ldp->lxsys_instance != 0 || ldp->lxsys_endpoint != 0)
		return (NULL);
	if (lxsys_input_slot(ldp, comp, "input", &slot))
		endpoint = LXSYS_CLASS_INPUT_ENDPOINT(slot);
	else if (lxsys_input_slot(ldp, comp, "event", &slot))
		endpoint = LXSYS_CLASS_EVENT_ENDPOINT(slot);
	else
		return (NULL);

	lnp = lxsys_getnode(ldp->lxsys_vnode, LXSYS_CLASS_INPUT, 0,
	    endpoint);
	lnp->lxsys_vnode->v_type = VLNK;
	lnp->lxsys_mode = 0777;
	return (lnp->lxsys_vnode);
}

static vnode_t *
lxsys_input_child(lxsys_node_t *ldp, uint_t endpoint, vtype_t type)
{
	lxsys_node_t *lnp;

	lnp = lxsys_getnode(ldp->lxsys_vnode, LXSYS_DEV_INPUT,
	    ldp->lxsys_instance, endpoint);
	lnp->lxsys_vnode->v_type = type;
	lnp->lxsys_mode = type == VLNK ? 0777 : 0444;
	return (lnp->lxsys_vnode);
}

static vnode_t *
lxsys_input_named_child(lxsys_node_t *ldp, char *comp,
    lxsys_dirent_t *entries, uint_t count)
{
	uint_t i;

	for (i = 0; i < count; i++) {
		if (strcmp(comp, entries[i].d_name) == 0) {
			vtype_t type = VREG;

			if (entries[i].d_idnum == LXSYS_ENDP_INPUT_IDDIR ||
			    entries[i].d_idnum == LXSYS_ENDP_INPUT_CAPDIR)
				type = VDIR;
			else if (entries[i].d_idnum ==
			    LXSYS_ENDP_INPUT_EVENT_DEVICE)
				type = VLNK;
			return (lxsys_input_child(ldp, entries[i].d_idnum,
			    type));
		}
	}
	return (NULL);
}

static vnode_t *
lxsys_lookup_devices_input(lxsys_node_t *ldp, char *comp)
{
	uint_t slot;

	if (ldp->lxsys_instance == 0) {
		lxsys_node_t *lnp;

		if (!lxsys_input_slot(ldp, comp, "input", &slot))
			return (NULL);
		lnp = lxsys_getnode(ldp->lxsys_vnode, LXSYS_DEV_INPUT,
		    slot + 1, 0);
		return (lnp->lxsys_vnode);
	}

	slot = ldp->lxsys_instance - 1;
	if (slot >= LX_INPUT_EVENT_COUNT ||
	    !lx_input_get_config(LXSTOZ(ldp)->zone_id, slot, NULL))
		return (NULL);

	if (ldp->lxsys_endpoint == 0) {
		char event[16];

		(void) snprintf(event, sizeof (event), "event%u", slot);
		if (strcmp(comp, event) == 0)
			return (lxsys_input_child(ldp,
			    LXSYS_ENDP_INPUT_EVENTDIR, VDIR));
		return (lxsys_input_named_child(ldp, comp,
		    dirlist_devices_input,
		    SYSDIRLISTSZ(dirlist_devices_input)));
	}
	if (ldp->lxsys_endpoint == LXSYS_ENDP_INPUT_IDDIR) {
		return (lxsys_input_named_child(ldp, comp,
		    dirlist_devices_input_id,
		    SYSDIRLISTSZ(dirlist_devices_input_id)));
	}
	if (ldp->lxsys_endpoint == LXSYS_ENDP_INPUT_CAPDIR) {
		return (lxsys_input_named_child(ldp, comp,
		    dirlist_devices_input_cap,
		    SYSDIRLISTSZ(dirlist_devices_input_cap)));
	}
	if (ldp->lxsys_endpoint == LXSYS_ENDP_INPUT_EVENTDIR) {
		return (lxsys_input_named_child(ldp, comp,
		    dirlist_devices_input_event,
		    SYSDIRLISTSZ(dirlist_devices_input_event)));
	}

	return (NULL);
}

static vnode_t *
lxsys_lookup_devices_virtual_netdir(lxsys_node_t *ldp, char *comp)
{
	lxsys_node_t *lnp;

	if (ldp->lxsys_instance == 0) {
		/* top-level interface listing */
		vnode_t *result = NULL;
		netstack_t *ns;
		ip_stack_t *ipst;
		avl_tree_t *phytree;
		phyint_t *phyi;
		char ifname[LIFNAMSIZ];

		(void) strncpy(ifname, comp, LIFNAMSIZ);
		lx_ifname_convert(ifname, LX_IF_TONATIVE);

		if ((ns = lxsys_netstack(ldp)) == NULL) {
			return (NULL);
		}
		ipst = ns->netstack_ip;
		rw_enter(&ipst->ips_ill_g_lock, RW_READER);

		phytree = &ipst->ips_phyint_g_list->phyint_list_avl_by_name;
		phyi = avl_find(phytree, ifname, NULL);
		if (phyi != NULL) {
			lnp = lxsys_getnode(ldp->lxsys_vnode, ldp->lxsys_type,
			    phyi->phyint_ifindex, 0);
			result = lnp->lxsys_vnode;
		}

		rw_exit(&ipst->ips_ill_g_lock);
		netstack_rele(ns);

		return (result);
	} else if (ldp->lxsys_endpoint == 0) {
		/* interface-level sub-item listing */
		int i, size;
		lxsys_dirent_t *dirent;

		size = SYSDIRLISTSZ(dirlist_devices_virtual_net);
		for (i = 0; i < size; i++) {
			dirent = &dirlist_devices_virtual_net[i];
			if (strncmp(comp, dirent->d_name, LXSNSIZ) == 0) {
				lnp = lxsys_getnode(ldp->lxsys_vnode,
				    ldp->lxsys_type, ldp->lxsys_instance,
				    dirent->d_idnum);
				lnp->lxsys_vnode->v_type = VREG;
				lnp->lxsys_mode = 0444;
				return (lnp->lxsys_vnode);
			}
		}
	}

	return (NULL);
}

static vnode_t *
lxsys_lookup_blockdir(lxsys_node_t *ldp, char *comp)
{
	lxsys_node_t *lnp;

	if (ldp->lxsys_instance == 0) {
		/* top-level dev listing */
		lnp = lxsys_lookup_disk(ldp, comp, LXSYS_BLOCK);

		if (lnp != NULL) {
			lnp->lxsys_vnode->v_type = VLNK;
			return (lnp->lxsys_vnode);
		}
	}

	return (NULL);
}

static vnode_t *
lxsys_lookup_devices_zfsdir(lxsys_node_t *ldp, char *comp)
{
	lxsys_node_t *lnp;

	if (ldp->lxsys_instance == 0) {
		/* top-level dev listing */
		lnp = lxsys_lookup_disk(ldp, comp, LXSYS_DEV_ZFS);

		if (lnp != NULL) {
			return (lnp->lxsys_vnode);
		}
	} else if (ldp->lxsys_endpoint == 0) {
		/* disk-level sub-item listing */
		int i, size;
		lxsys_dirent_t *dirent;

		/*
		 * All of these entries currently look like regular files
		 * but on a real Linux system some will be subdirs. This should
		 * be fixed when we populate the directory for real.
		 */
		size = SYSDIRLISTSZ(dirlist_devices_zfs_block);
		for (i = 0; i < size; i++) {
			dirent = &dirlist_devices_zfs_block[i];
			if (strncmp(comp, dirent->d_name, LXSNSIZ) == 0) {
				lnp = lxsys_getnode(ldp->lxsys_vnode,
				    ldp->lxsys_type, ldp->lxsys_instance,
				    dirent->d_idnum);
				lnp->lxsys_vnode->v_type = VREG;
				lnp->lxsys_mode = 0444;
				return (lnp->lxsys_vnode);
			}
		}
	}

	return (NULL);
}

static vnode_t *
lxsys_lookup_devices_syscpu(lxsys_node_t *ldp, char *comp)
{
	lxsys_node_t *lnp = NULL;

	if (ldp->lxsys_instance == 0) {
		/* top-level cpu listing */

		/* If fixed entry */
		if (strcmp(comp, "kernel_max") == 0) {
			lnp = lxsys_getnode(ldp->lxsys_vnode, LXSYS_DEV_SYS_CPU,
			    LXSYS_INST_DEV_SYSCPU_KMAX, 0);
			lnp->lxsys_vnode->v_type = VREG;
			lnp->lxsys_mode = 0444;
		} else if (strcmp(comp, "offline") == 0) {
			lnp = lxsys_getnode(ldp->lxsys_vnode, LXSYS_DEV_SYS_CPU,
			    LXSYS_INST_DEV_SYSCPU_OFFLINE, 0);
			lnp->lxsys_vnode->v_type = VREG;
			lnp->lxsys_mode = 0444;
		} else if (strcmp(comp, "online") == 0) {
			lnp = lxsys_getnode(ldp->lxsys_vnode, LXSYS_DEV_SYS_CPU,
			    LXSYS_INST_DEV_SYSCPU_ONLINE, 0);
			lnp->lxsys_vnode->v_type = VREG;
			lnp->lxsys_mode = 0444;
		} else if (strcmp(comp, "possible") == 0) {
			lnp = lxsys_getnode(ldp->lxsys_vnode, LXSYS_DEV_SYS_CPU,
			    LXSYS_INST_DEV_SYSCPU_POSSIBLE, 0);
			lnp->lxsys_vnode->v_type = VREG;
			lnp->lxsys_mode = 0444;
		} else if (strcmp(comp, "present") == 0) {
			lnp = lxsys_getnode(ldp->lxsys_vnode, LXSYS_DEV_SYS_CPU,
			    LXSYS_INST_DEV_SYSCPU_PRESENT, 0);
			lnp->lxsys_vnode->v_type = VREG;
			lnp->lxsys_mode = 0444;
		} else {
			/* Else dynamic cpuN entry */
			cpuset_t *avail;	/* all installed CPUs */
			uint_t i, avlo, avhi;

			avail = cpuset_alloc(KM_SLEEP);
			cpuset_all(avail);

			/* Take a snapshot of the available set */
			mutex_enter(&cpu_lock);
			cpuset_and(avail, &cpu_available);
			mutex_exit(&cpu_lock);

			cpuset_bounds(avail, &avlo, &avhi);

			for (i = avlo; i <= avhi; i++) {
				char cpunm[16];

				if (!cpu_in_set(avail, i))
					continue;

				(void) snprintf(cpunm, sizeof (cpunm), "cpu%u",
				    i);

				if (strcmp(comp, cpunm) == 0) {
					lnp = lxsys_getnode(ldp->lxsys_vnode,
					    LXSYS_DEV_SYS_CPUINFO, i + 1, 0);
					break;
				}
			}
			cpuset_free(avail);
		}

		if (lnp != NULL) {
			return (lnp->lxsys_vnode);
		}
	} else if (ldp->lxsys_endpoint == 0) {
		/* cpu-level sub-item listing, currently empty */
		/* EMPTY */
	}

	return (NULL);
}

/* ARGSUSED */
static vnode_t *
lxsys_lookup_devices_syscpuinfo(lxsys_node_t *ldp, char *comp)
{
	return (NULL);
}

static vnode_t *
lxsys_lookup_devices_sysnode(lxsys_node_t *ldp, char *comp)
{
	lxsys_node_t *lnp = NULL;

	if (ldp->lxsys_instance == 0) {
		/*
		 * The system is presently represented as a single node,
		 * regardless of any NUMA topology which exists.
		 * The instances are offset by 1 to account for the top level
		 * directory occupying instance 0.
		 */
		if (strcmp(comp, "node0") == 0) {
			lnp = lxsys_getnode(ldp->lxsys_vnode, ldp->lxsys_type,
			    1, 0);
			return (lnp->lxsys_vnode);
		}
	} else {
		/* interface-level sub-item listing */
		int i, size;
		lxsys_dirent_t *dirent;

		size = SYSDIRLISTSZ(dirlist_devices_sysnode);
		for (i = 0; i < size; i++) {
			dirent = &dirlist_devices_sysnode[i];
			if (strncmp(comp, dirent->d_name, LXSNSIZ) == 0) {
				lnp = lxsys_getnode(ldp->lxsys_vnode,
				    ldp->lxsys_type, ldp->lxsys_instance,
				    dirent->d_idnum);
				lnp->lxsys_vnode->v_type = VREG;
				lnp->lxsys_mode = 0444;
				return (lnp->lxsys_vnode);
			}
		}
	}

	return (NULL);
}

static int
lxsys_read_devices_virtual_net(lxsys_node_t *lnp, lxsys_uiobuf_t *luio)
{
	netstack_t *ns;
	ill_t *ill;
	uint_t ifindex = lnp->lxsys_instance;
	uint8_t *addr;
	uint64_t flags;
	int error = 0;

	if (ifindex == 0 || lnp->lxsys_endpoint == 0) {
		return (EISDIR);
	}

	if ((ns = lxsys_netstack(lnp)) == NULL) {
		return (EIO);
	}

	ill = lxsys_find_ill(ns->netstack_ip, ifindex);
	if (ill == NULL) {
		netstack_rele(ns);
		return (EIO);
	}

	switch (lnp->lxsys_endpoint) {
	case LXSYS_ENDP_NET_ADDRESS:
		if (ill->ill_phys_addr_length != ETHERADDRL) {
			lxsys_uiobuf_printf(luio, "00:00:00:00:00:00\n");
			break;
		}
		addr = ill->ill_phys_addr;
		lxsys_uiobuf_printf(luio,
		    "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx\n",
		    addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
		break;
	case LXSYS_ENDP_NET_ADDRLEN:
		lxsys_uiobuf_printf(luio, "%u\n",
		    IS_LOOPBACK(ill) ? ETHERADDRL : ill->ill_phys_addr_length);
		break;
	case LXSYS_ENDP_NET_FLAGS:
		flags = (ill->ill_flags | ill->ill_ipif->ipif_flags |
		    ill->ill_phyint->phyint_flags) & 0xffff;
		lx_ifflags_convert(&flags, LX_IF_FROMNATIVE);
		lxsys_uiobuf_printf(luio, "0x%x\n", flags);
		break;
	case LXSYS_ENDP_NET_IFINDEX:
		lxsys_uiobuf_printf(luio, "%u\n", ifindex);
		break;
	case LXSYS_ENDP_NET_MTU:
		lxsys_uiobuf_printf(luio, "%u\n", ill->ill_mtu);
		break;
	case LXSYS_ENDP_NET_TXQLEN:
		/* perpetuate the txqlen lie */
		if (IS_LOOPBACK(ill)) {
			lxsys_uiobuf_printf(luio, "0\n");
		} else {
			lxsys_uiobuf_printf(luio, "1\n");
		}
		break;
	case LXSYS_ENDP_NET_TYPE:
		lxsys_uiobuf_printf(luio, "%u\n",
		    IS_LOOPBACK(ill) ? LX_ARPHRD_LOOPBACK :
		    arp_hw_type(ill->ill_mactype));
		break;
	default:
		error = EIO;
	}

	ill_refrele(ill);
	netstack_rele(ns);
	return (error);
}

/* ARGSUSED1 */
static int
lxsys_read_devices_zfs_block(lxsys_node_t *lnp, lxsys_uiobuf_t *luio)
{
	uint_t dskindex = lnp->lxsys_instance;

	if (dskindex == 0 || lnp->lxsys_endpoint == 0) {
		return (EISDIR);
	}

	return (EIO);
}

/*
 * In the Linux src tree, see ABI/stable/sysfs-devices-node.
 *
 * For the 'cpumap' file, each CPU is treated as a bit, then those are
 * accumulated and printed as a hex digit, with CPU0 as the rightmost bit.
 * Each set of 8 digits (i.e. 32 CPUs) is then delimited with a comma.
 * Since we are emulating a single NUMA group, all of our CPUs will be listed
 * in this file. For example, a 48 CPU system would look like:
 *     00000000,00000000,00000000,00000000,00000000,00000000,0000ffff,ffffffff
 * It comes out this way because 'kernel_max' is NCPU, which is currently
 * defined to be 256.
 */
static int
lxsys_read_devices_sysnode(lxsys_node_t *lnp, lxsys_uiobuf_t *luio)
{
	if (lnp->lxsys_instance == 1) {
		char outbuf[256];

		if (lnp->lxsys_endpoint == LXSYS_ENDP_NODE_CPULIST) {
			/* Show the range of CPUs */
			lxsys_format_cpu(outbuf, sizeof (outbuf),
			    LXSYS_CPU_ANY);
		} else if (lnp->lxsys_endpoint == LXSYS_ENDP_NODE_CPUMAP) {
			int i;
			uint_t j, ndigits;
			cpuset_t *avail;	/* all installed CPUs */

			avail = cpuset_alloc(KM_SLEEP);
			cpuset_all(avail);

			/* Take a snapshot of the available set */
			mutex_enter(&cpu_lock);
			cpuset_and(avail, &cpu_available);
			mutex_exit(&cpu_lock);

			outbuf[0] = '\0';
			ndigits = 0;
			for (i = NCPU - 1; i >= 0; i -= 4) {
				char buf[8];
				int cnt = 3;
				uint_t digit = 0;

				for (j = i; cnt >= 0; j--, cnt--) {
					if (cpu_in_set(avail, j))
						digit |= 1 << cnt;
				}
				(void) snprintf(buf, sizeof (buf), "%x", digit);
				if (ndigits == 8) {
					(void) strlcat(outbuf, ",",
					    sizeof (outbuf));
					ndigits = 0;
				}
				(void) strlcat(outbuf, buf, sizeof (outbuf));
				ndigits++;
			}

			cpuset_free(avail);
		} else {
			return (EISDIR);
		}

		lxsys_uiobuf_printf(luio, "%s\n", outbuf);
		return (0);
	}
	return (EISDIR);
}

static void
lxsys_input_bitmap(lxsys_uiobuf_t *luio, const uint8_t *bits, size_t size)
{
	int word;
	boolean_t started = B_FALSE;

	for (word = (int)((size + sizeof (uint64_t) - 1) /
	    sizeof (uint64_t)) - 1; word >= 0; word--) {
		uint64_t value = 0;
		uint_t byte;

		for (byte = 0; byte < sizeof (value); byte++) {
			size_t offset = word * sizeof (value) + byte;

			if (offset < size)
				value |= (uint64_t)bits[offset] << (byte * 8);
		}
		if (value == 0 && !started && word != 0)
			continue;
		lxsys_uiobuf_printf(luio, started ? " %llx" : "%llx",
		    (u_longlong_t)value);
		started = B_TRUE;
	}
	lxsys_uiobuf_printf(luio, "\n");
}

static int
lxsys_read_devices_input(lxsys_node_t *lnp, lxsys_uiobuf_t *luio)
{
	lx_input_config_t config;
	const uint8_t *bits = NULL;
	size_t size = 0;
	uint_t slot;

	if (lnp->lxsys_instance == 0)
		return (EISDIR);
	slot = lnp->lxsys_instance - 1;
	if (!lx_input_get_config(LXSTOZ(lnp)->zone_id, slot, &config))
		return (EIO);

	switch (lnp->lxsys_endpoint) {
	case LXSYS_ENDP_INPUT_NAME:
		lxsys_uiobuf_printf(luio, "%s\n", config.lic_name);
		return (0);
	case LXSYS_ENDP_INPUT_PHYS:
		lxsys_uiobuf_printf(luio, "%s\n", config.lic_phys);
		return (0);
	case LXSYS_ENDP_INPUT_UNIQ:
		lxsys_uiobuf_printf(luio, "%s\n", config.lic_uniq);
		return (0);
	case LXSYS_ENDP_INPUT_ID_BUS:
		lxsys_uiobuf_printf(luio, "%04x\n", config.lic_id.lii_bustype);
		return (0);
	case LXSYS_ENDP_INPUT_ID_VENDOR:
		lxsys_uiobuf_printf(luio, "%04x\n", config.lic_id.lii_vendor);
		return (0);
	case LXSYS_ENDP_INPUT_ID_PRODUCT:
		lxsys_uiobuf_printf(luio, "%04x\n", config.lic_id.lii_product);
		return (0);
	case LXSYS_ENDP_INPUT_ID_VERSION:
		lxsys_uiobuf_printf(luio, "%04x\n", config.lic_id.lii_version);
		return (0);
	case LXSYS_ENDP_INPUT_PROPERTIES:
		bits = config.lic_propbits;
		size = sizeof (config.lic_propbits);
		break;
	case LXSYS_ENDP_INPUT_CAP_EV:
		bits = config.lic_evbits;
		size = sizeof (config.lic_evbits);
		break;
	case LXSYS_ENDP_INPUT_CAP_KEY:
		bits = config.lic_keybits;
		size = sizeof (config.lic_keybits);
		break;
	case LXSYS_ENDP_INPUT_CAP_REL:
		bits = config.lic_relbits;
		size = sizeof (config.lic_relbits);
		break;
	case LXSYS_ENDP_INPUT_CAP_ABS:
		bits = config.lic_absbits;
		size = sizeof (config.lic_absbits);
		break;
	case LXSYS_ENDP_INPUT_CAP_MSC:
		bits = config.lic_mscbits;
		size = sizeof (config.lic_mscbits);
		break;
	case LXSYS_ENDP_INPUT_CAP_SW:
		bits = config.lic_swbits;
		size = sizeof (config.lic_swbits);
		break;
	case LXSYS_ENDP_INPUT_CAP_LED:
		bits = config.lic_ledbits;
		size = sizeof (config.lic_ledbits);
		break;
	case LXSYS_ENDP_INPUT_CAP_SND:
		bits = config.lic_sndbits;
		size = sizeof (config.lic_sndbits);
		break;
	case LXSYS_ENDP_INPUT_CAP_FF:
		bits = config.lic_ffbits;
		size = sizeof (config.lic_ffbits);
		break;
	case LXSYS_ENDP_INPUT_UEVENT:
		lxsys_uiobuf_printf(luio, "PRODUCT=%x/%x/%x/%x\n",
		    config.lic_id.lii_bustype, config.lic_id.lii_vendor,
		    config.lic_id.lii_product, config.lic_id.lii_version);
		lxsys_uiobuf_printf(luio, "NAME=\"%s\"\n", config.lic_name);
		if (config.lic_phys[0] != '\0')
			lxsys_uiobuf_printf(luio, "PHYS=\"%s\"\n",
			    config.lic_phys);
		if (config.lic_uniq[0] != '\0')
			lxsys_uiobuf_printf(luio, "UNIQ=\"%s\"\n",
			    config.lic_uniq);
		return (0);
	case LXSYS_ENDP_INPUT_EVENT_DEV:
		lxsys_uiobuf_printf(luio, "%u:%u\n",
		    LX_INPUT_LINUX_EVENT_MAJOR,
		    LX_INPUT_LINUX_EVENT_BASE + slot);
		return (0);
	case LXSYS_ENDP_INPUT_EVENT_UEVENT:
		lxsys_uiobuf_printf(luio, "MAJOR=%u\nMINOR=%u\n",
		    LX_INPUT_LINUX_EVENT_MAJOR,
		    LX_INPUT_LINUX_EVENT_BASE + slot);
		lxsys_uiobuf_printf(luio, "DEVNAME=input/event%u\n", slot);
		return (0);
	default:
		return (EISDIR);
	}

	lxsys_input_bitmap(luio, bits, size);
	return (0);
}

static void
lxsys_format_range(char *buf, int blen, boolean_t *first, uint_t start,
    uint_t cnt)
{
	char tmp[256];
	char *delim;

	if (cnt == 0)
		return;

	if (*first) {
		*first = B_FALSE;
		delim = "";
	} else {
		delim = ",";
	}
	if (cnt > 1) {
		(void) snprintf(tmp, sizeof (tmp), "%s%u-%u", delim, start,
		    start + cnt - 1);
	} else {
		(void) snprintf(tmp, sizeof (tmp), "%s%u", delim, start);
	}
	(void) strlcat(buf, tmp, blen);
}

/*
 * Format a string of which CPUs are online, offline, or don't care (depending
 * on chk_state), and which would be formatted like this:
 *    0-31
 * or
 *    0-12,14,20-31
 */
static void
lxsys_format_cpu(char *buf, int blen, lxsys_cpu_state_t chk_state)
{
	uint_t start, cnt, avlo, avhi;
	boolean_t first = B_TRUE;
	cpuset_t *active;	/* CPUs online */
	cpuset_t *avail;	/* all installed CPUs */

	active = cpuset_alloc(KM_SLEEP);
	avail = cpuset_alloc(KM_SLEEP);
	cpuset_all(active);
	cpuset_all(avail);

	/* Take a snapshot of the available and active sets */
	mutex_enter(&cpu_lock);
	cpuset_and(avail, &cpu_available);
	cpuset_and(active, &cpu_active_set);
	mutex_exit(&cpu_lock);

	cpuset_bounds(avail, &avlo, &avhi);

	buf[0] = '\0';
	if (chk_state == LXSYS_CPU_ANY) {
		start = avlo;
		cnt = avhi + 1;
	} else {
		uint_t i;
		boolean_t incl_cpu = B_TRUE;

		start = 0;
		cnt = 0;
		for (i = avlo; i <= avhi; i++) {
			if (chk_state == LXSYS_CPU_ON) {
				if (!cpu_in_set(active, i))
					incl_cpu = B_FALSE;
			} else {
				if (cpu_in_set(active, i))
					incl_cpu = B_FALSE;
			}

			if (incl_cpu && cpu_in_set(avail, i)) {
				cnt++;
			} else {
				/*
				 * Note: this may print nothing if our 'cnt'
				 * is 0, but we advance 'start' properly so we
				 * handle the next range of elements we're
				 * looking for.
				 */
				lxsys_format_range(buf, blen, &first, start,
				    cnt);
				start += cnt + 1;
				cnt = 0;
				incl_cpu = B_TRUE;
			}
		}
	}

	cpuset_free(avail);
	cpuset_free(active);

	lxsys_format_range(buf, blen, &first, start, cnt);
}

static int
lxsys_read_devices_syscpu(lxsys_node_t *lnp, lxsys_uiobuf_t *luio)
{
	uint_t inst = lnp->lxsys_instance;
	char outbuf[256];

	/*
	 * For 'kernel_max', 'offline', 'online', 'possible', and 'present',
	 * see the Documentaion/cputopology.txt file in the Linux src tree.
	 */
	if (inst == LXSYS_INST_DEV_SYSCPU_KMAX) {
		lxsys_uiobuf_printf(luio, "%d\n", NCPU - 1);
		return (0);
	}

	if (inst == LXSYS_INST_DEV_SYSCPU_OFFLINE) {
		lxsys_format_cpu(outbuf, sizeof (outbuf), LXSYS_CPU_OFF);
		lxsys_uiobuf_printf(luio, "%s\n", outbuf);
		return (0);
	}

	if (inst == LXSYS_INST_DEV_SYSCPU_ONLINE) {
		lxsys_format_cpu(outbuf, sizeof (outbuf), LXSYS_CPU_ON);
		lxsys_uiobuf_printf(luio, "%s\n", outbuf);
		return (0);
	}

	if (inst == LXSYS_INST_DEV_SYSCPU_POSSIBLE ||
	    inst == LXSYS_INST_DEV_SYSCPU_PRESENT) {
		lxsys_format_cpu(outbuf, sizeof (outbuf), LXSYS_CPU_ANY);
		lxsys_uiobuf_printf(luio, "%s\n", outbuf);
		return (0);
	}

	/* All other nodes are directories */
	return (EISDIR);
}

/* ARGSUSED */
static int
lxsys_read_static(lxsys_node_t *lnp, lxsys_uiobuf_t *luio)
{
	/* All static nodes are directories */
	return (EISDIR);
}

/*
 * lxsys_readdir(): Vnode operation for VOP_READDIR()
 */
/* ARGSUSED */
static int
lxsys_readdir(vnode_t *dp, uio_t *uiop, cred_t *cr, int *eofp,
    caller_context_t *ct, int flags)
{
	lxsys_node_t *lxsnp = VTOLXS(dp);
	lxsys_nodetype_t type = lxsnp->lxsys_type;
	ssize_t uresid;
	off_t uoffset;
	int error, leof;

	ASSERT(dp->v_type == VDIR);
	VERIFY(type > LXSYS_NONE && type < LXSYS_MAXTYPE);

	/*
	 * restrict readdir permission to owner or root
	 */
	if ((error = lxsys_access(dp, VREAD, 0, cr, ct)) != 0)
		return (error);

	uoffset = uiop->uio_offset;
	uresid = uiop->uio_resid;

	/* can't do negative reads */
	if (uoffset < 0 || uresid <= 0)
		return (EINVAL);

	/* can't read directory entries that don't exist! */
	if (uoffset % LXSYS_SDSIZE)
		return (ENOENT);

	/* Free lower functions from having to check eofp == NULL */
	if (eofp == NULL) {
		eofp = &leof;
	}

	return (lxsys_readdir_function[lxsnp->lxsys_type](lxsnp, uiop, eofp));
}

static int
lxsys_dirent_out(dirent64_t *d, ushort_t n, struct uio *uio)
{
	int error;
	off_t offset = uio->uio_offset;

	/*
	 * uiomove() updates both uiop->uio_resid and uiop->uio_offset by the
	 * same amount.  But we want uiop->uio_offset to change in increments
	 * of LXSYS_SDSIZE, which is different from the number of bytes being
	 * returned to the user.  To accomplish this, we set uiop->uio_offset
	 * separately on success, overriding what uiomove() does.
	 */
	d->d_off = (off64_t)(offset + LXSYS_SDSIZE);
	d->d_reclen = n;
	if ((error = uiomove(d, n, UIO_READ, uio)) != 0) {
		return (error);
	}
	uio->uio_offset = offset + LXSYS_SDSIZE;
	return (0);
}

/*
 * This has the common logic for returning directory entries
 */
static int
lxsys_readdir_common(lxsys_node_t *lxsnp, uio_t *uiop, int *eofp,
    lxsys_dirent_t *dirtab, int dirtablen)
{
	/* bp holds one dirent64 structure */
	longlong_t bp[DIRENT64_RECLEN(LXSNSIZ) / sizeof (longlong_t)];
	dirent64_t *dirent = (dirent64_t *)bp;
	ssize_t oresid;	/* save a copy for testing later */
	ssize_t uresid;

	oresid = uiop->uio_resid;

	/* clear out the dirent buffer */
	bzero(bp, sizeof (bp));

	/* Satisfy user request */
	while ((uresid = uiop->uio_resid) > 0) {
		int dirindex;
		off_t uoffset;
		int reclen;
		int error;

		uoffset = uiop->uio_offset;
		dirindex  = (uoffset / LXSYS_SDSIZE) - 2;

		if (uoffset == 0) {

			dirent->d_ino = lxsnp->lxsys_ino;
			dirent->d_name[0] = '.';
			dirent->d_name[1] = '\0';
			reclen = DIRENT64_RECLEN(1);

		} else if (uoffset == LXSYS_SDSIZE) {

			dirent->d_ino = lxsys_parentinode(lxsnp);
			dirent->d_name[0] = '.';
			dirent->d_name[1] = '.';
			dirent->d_name[2] = '\0';
			reclen = DIRENT64_RECLEN(2);

		} else if (dirindex >= 0 && dirindex < dirtablen) {

			int slen = strlen(dirtab[dirindex].d_name);
			int idnum, ino_type = 0;

			idnum = dirtab[dirindex].d_idnum;
			if (idnum > 0 && idnum < DYN_INO_LEN)
				ino_type = dyn_ino_type[idnum];

			if (ino_type != 0) {
				/*
				 * Correct the inode for static directories
				 * which contain non-static lxsys_nodetype_t's.
				 */
				dirent->d_ino = lxsys_inode(ino_type, 0, 0);
				DTRACE_PROBE3(lxsys__fix__inode,
				    char *, dirtab[dirindex].d_name,
				    int, ino_type, int, dirent->d_ino);
			} else {
				dirent->d_ino = lxsys_inode(LXSYS_STATIC,
				    idnum, 0);
			}

			(void) strcpy(dirent->d_name, dirtab[dirindex].d_name);
			reclen = DIRENT64_RECLEN(slen);

		} else {
			/* Run out of table entries */
			*eofp = 1;
			return (0);
		}

		/*
		 * If the size of the data to transfer is greater than the
		 * user-provided buffer, we cannot continue.
		 */
		if (reclen > uresid) {
			/* Error if no entries have been returned yet. */
			if (uresid == oresid) {
				return (EINVAL);
			}
			break;
		}

		if ((error = lxsys_dirent_out(dirent, reclen, uiop)) != 0) {
			return (error);
		}
	}

	/* Have run out of space, but could have just done last table entry */
	*eofp = (uiop->uio_offset >= ((dirtablen+2) * LXSYS_SDSIZE)) ?  1 : 0;
	return (0);
}

static int
lxsys_readdir_subdir(lxsys_node_t *lxsnp, uio_t *uiop, int *eofp,
    lxsys_dirent_t *dirtab, int dirtablen)
{
	/* bp holds one dirent64 structure */
	longlong_t bp[DIRENT64_RECLEN(LXSNSIZ) / sizeof (longlong_t)];
	dirent64_t *dirent = (dirent64_t *)bp;
	ssize_t oresid;	/* save a copy for testing later */
	ssize_t uresid;

	VERIFY(dirtab != NULL || dirtablen == 0);

	oresid = uiop->uio_resid;

	/* clear out the dirent buffer */
	bzero(bp, sizeof (bp));

	/* Satisfy user request */
	while ((uresid = uiop->uio_resid) > 0) {
		int dirindex;
		off_t uoffset;
		int reclen;
		int error;

		uoffset = uiop->uio_offset;
		dirindex  = (uoffset / LXSYS_SDSIZE) - 2;

		if (uoffset == 0) {

			dirent->d_ino = lxsnp->lxsys_ino;
			dirent->d_name[0] = '.';
			dirent->d_name[1] = '\0';
			reclen = DIRENT64_RECLEN(1);

		} else if (uoffset == LXSYS_SDSIZE) {

			dirent->d_ino = lxsys_parentinode(lxsnp);
			dirent->d_name[0] = '.';
			dirent->d_name[1] = '.';
			dirent->d_name[2] = '\0';
			reclen = DIRENT64_RECLEN(2);

		} else if (dirindex >= 0 && dirindex < dirtablen) {

			int slen = strlen(dirtab[dirindex].d_name);

			dirent->d_ino = lxsys_inode(lxsnp->lxsys_type,
			    lxsnp->lxsys_instance, dirtab[dirindex].d_idnum);
			(void) strcpy(dirent->d_name, dirtab[dirindex].d_name);
			reclen = DIRENT64_RECLEN(slen);

		} else {
			/* Run out of table entries */
			*eofp = 1;
			return (0);
		}

		/*
		 * If the size of the data to transfer is greater than the
		 * user-provided buffer, we cannot continue.
		 */
		if (reclen > uresid) {
			/* Error if no entries have been returned yet. */
			if (uresid == oresid) {
				return (EINVAL);
			}
			break;
		}

		if ((error = lxsys_dirent_out(dirent, reclen, uiop)) != 0) {
			return (error);
		}
	}

	/* Have run out of space, but could have just done last table entry */
	*eofp = (uiop->uio_offset >= ((dirtablen+2) * LXSYS_SDSIZE)) ?  1 : 0;
	return (0);
}

static int
lxsys_readdir_ifaces(lxsys_node_t *ldp, struct uio *uiop, int *eofp,
    lxsys_nodetype_t type)
{
	longlong_t bp[DIRENT64_RECLEN(LXSNSIZ) / sizeof (longlong_t)];
	dirent64_t *dirent = (dirent64_t *)bp;
	ssize_t oresid, uresid;
	netstack_t *ns;
	ip_stack_t *ipst;
	avl_tree_t *phytree;
	phyint_t *phyi;
	int error, i;


	/* Emit "." and ".." entries */
	oresid = uiop->uio_resid;
	error = lxsys_readdir_common(ldp, uiop, eofp, NULL, 0);
	if (error != 0 || *eofp == 0) {
		return (error);
	}

	if ((ns = lxsys_netstack(ldp)) == NULL) {
		*eofp = 1;
		return (0);
	}
	ipst = ns->netstack_ip;

	rw_enter(&ipst->ips_ill_g_lock, RW_READER);
	phytree = &ipst->ips_phyint_g_list->phyint_list_avl_by_index;
	phyi = avl_first(phytree);
	if (phyi == NULL) {
		*eofp = 1;
	}
	bzero(bp, sizeof (bp));

	/*
	 * Skip records we have already passed with the offset.
	 * This accounts for the two "." and ".." records already seen.
	 */
	for (i = (uiop->uio_offset/LXSYS_SDSIZE) - 2; i > 0; i--) {
		if ((phyi = avl_walk(phytree, phyi, AVL_AFTER)) == NULL) {
			*eofp = 1;
			break;
		}
	}

	while ((uresid = uiop->uio_resid) > 0 && phyi != NULL) {
		uint_t ifindex;
		int reclen;

		ifindex = phyi->phyint_ifindex;
		(void) strncpy(dirent->d_name, phyi->phyint_name, LIFNAMSIZ);
		lx_ifname_convert(dirent->d_name, LX_IF_FROMNATIVE);
		dirent->d_ino = lxsys_inode(type, ifindex, 0);
		reclen = DIRENT64_RECLEN(strlen(dirent->d_name));

		if (reclen > uresid) {
			if (uresid == oresid) {
				/* Not enough space for one record */
				error = EINVAL;
			}
			break;
		}
		if ((error = lxsys_dirent_out(dirent, reclen, uiop)) != 0) {
			break;
		}

		if ((phyi = avl_walk(phytree, phyi, AVL_AFTER)) == NULL) {
			*eofp = 1;
			break;
		}
	}

	rw_exit(&ipst->ips_ill_g_lock);
	netstack_rele(ns);
	return (error);
}

static int
lxsys_readdir_disks(lxsys_node_t *ldp, struct uio *uiop, int *eofp,
    lxsys_nodetype_t type)
{
	longlong_t bp[DIRENT64_RECLEN(LXSNSIZ) / sizeof (longlong_t)];
	dirent64_t *dirent = (dirent64_t *)bp;
	ssize_t oresid, uresid;
	int skip, error;
	int reclen;
	uint_t instance;
	lx_zone_data_t *lxzdata;
	lx_virt_disk_t *vd;

	/* Emit "." and ".." entries */
	oresid = uiop->uio_resid;
	error = lxsys_readdir_common(ldp, uiop, eofp, NULL, 0);
	if (error != 0 || *eofp == 0) {
		return (error);
	}

	skip = (uiop->uio_offset/LXSYS_SDSIZE) - 2;

	lxzdata = ztolxzd(curproc->p_zone);
	if (lxzdata == NULL)
		return (EINVAL);
	ASSERT(lxzdata->lxzd_vdisks != NULL);

	vd = list_head(lxzdata->lxzd_vdisks);
	while (vd != NULL) {
		if (skip > 0) {
			skip--;
			goto next;
		}

		if (strnlen(vd->lxvd_name, sizeof (vd->lxvd_name)) > LXSNSIZ)
			goto next;

		(void) strncpy(dirent->d_name, vd->lxvd_name, LXSNSIZ);

		instance = getminor(vd->lxvd_emul_dev) & 0xffff;
		if (instance == 0)
			goto next;

		dirent->d_ino = lxsys_inode(type, instance, 0);
		reclen = DIRENT64_RECLEN(strlen(dirent->d_name));

		uresid = uiop->uio_resid;
		if (reclen > uresid) {
			if (uresid == oresid) {
				/* Not enough space for one record */
				error = EINVAL;
			}
			break;
		}
		if ((error = lxsys_dirent_out(dirent, reclen, uiop)) != 0) {
			break;
		}

next:
		vd = list_next(lxzdata->lxzd_vdisks, vd);
	}

	/* Indicate EOF if we reached the end of the virtual disks. */
	if (vd == NULL) {
		*eofp = 1;
	}

	return (error);
}


static int
lxsys_readdir_static(lxsys_node_t *lnp, uio_t *uiop, int *eofp)
{
	lxsys_dirent_t *dirent = NULL;
	int i, len = 0;
	boolean_t found = B_FALSE;

	for (i = 0; i < SYSDIRLISTSZ(lxsys_dirlookup); i++) {
		if (lnp->lxsys_instance == lxsys_dirlookup[i].dl_instance) {
			dirent = lxsys_dirlookup[i].dl_list;
			len = lxsys_dirlookup[i].dl_length;
			found = B_TRUE;
			break;
		}
	}

	if (!found) {
		return (ENOTDIR);
	}

	return (lxsys_readdir_common(lnp, uiop, eofp, dirent, len));
}

static int
lxsys_readdir_input_list(lxsys_node_t *lnp, uio_t *uiop, int *eofp,
    boolean_t class_entries)
{
	longlong_t bp[DIRENT64_RECLEN(LXSNSIZ) / sizeof (longlong_t)];
	dirent64_t *dirent = (dirent64_t *)bp;
	ssize_t original;
	int error;
	int skip;
	uint_t slot;

	original = uiop->uio_resid;
	error = lxsys_readdir_common(lnp, uiop, eofp, NULL, 0);
	if (error != 0 || *eofp == 0)
		return (error);

	skip = (uiop->uio_offset / LXSYS_SDSIZE) - 2;
	bzero(bp, sizeof (bp));
	for (slot = 0; slot < LX_INPUT_EVENT_COUNT; slot++) {
		uint_t entries = class_entries ? 2 : 1;
		uint_t entry;

		if (!lx_input_get_config(LXSTOZ(lnp)->zone_id, slot, NULL))
			continue;
		for (entry = 0; entry < entries; entry++) {
			int reclen;

			if (skip > 0) {
				skip--;
				continue;
			}
			if (class_entries) {
				(void) snprintf(dirent->d_name,
				    sizeof (dirent->d_name), "%s%u",
				    entry == 0 ? "input" : "event", slot);
				dirent->d_ino = lxsys_inode(LXSYS_CLASS_INPUT,
				    0, entry == 0 ?
				    LXSYS_CLASS_INPUT_ENDPOINT(slot) :
				    LXSYS_CLASS_EVENT_ENDPOINT(slot));
			} else {
				(void) snprintf(dirent->d_name,
				    sizeof (dirent->d_name), "input%u", slot);
				dirent->d_ino = lxsys_inode(LXSYS_DEV_INPUT,
				    slot + 1, 0);
			}
			reclen = DIRENT64_RECLEN(strlen(dirent->d_name));
			if (reclen > uiop->uio_resid) {
				if (uiop->uio_resid == original)
					return (EINVAL);
				*eofp = 0;
				return (0);
			}
			if ((error = lxsys_dirent_out(dirent, reclen,
			    uiop)) != 0)
				return (error);
		}
	}

	*eofp = 1;
	return (0);
}

static int
lxsys_readdir_class_input(lxsys_node_t *lnp, uio_t *uiop, int *eofp)
{
	if (lnp->lxsys_instance != 0 || lnp->lxsys_endpoint != 0)
		return (ENOTDIR);
	return (lxsys_readdir_input_list(lnp, uiop, eofp, B_TRUE));
}

static int
lxsys_readdir_devices_input(lxsys_node_t *lnp, uio_t *uiop, int *eofp)
{
	if (lnp->lxsys_instance == 0)
		return (lxsys_readdir_input_list(lnp, uiop, eofp, B_FALSE));

	if (lnp->lxsys_endpoint == 0) {
		lxsys_dirent_t entries[
		    SYSDIRLISTSZ(dirlist_devices_input) + 1];
		char event[16];

		bcopy(dirlist_devices_input, entries,
		    sizeof (dirlist_devices_input));
		(void) snprintf(event, sizeof (event), "event%u",
		    lnp->lxsys_instance - 1);
		entries[SYSDIRLISTSZ(dirlist_devices_input)].d_idnum =
		    LXSYS_ENDP_INPUT_EVENTDIR;
		entries[SYSDIRLISTSZ(dirlist_devices_input)].d_name = event;
		return (lxsys_readdir_subdir(lnp, uiop, eofp, entries,
		    SYSDIRLISTSZ(entries)));
	}
	if (lnp->lxsys_endpoint == LXSYS_ENDP_INPUT_IDDIR) {
		return (lxsys_readdir_subdir(lnp, uiop, eofp,
		    dirlist_devices_input_id,
		    SYSDIRLISTSZ(dirlist_devices_input_id)));
	}
	if (lnp->lxsys_endpoint == LXSYS_ENDP_INPUT_CAPDIR) {
		return (lxsys_readdir_subdir(lnp, uiop, eofp,
		    dirlist_devices_input_cap,
		    SYSDIRLISTSZ(dirlist_devices_input_cap)));
	}
	if (lnp->lxsys_endpoint == LXSYS_ENDP_INPUT_EVENTDIR) {
		return (lxsys_readdir_subdir(lnp, uiop, eofp,
		    dirlist_devices_input_event,
		    SYSDIRLISTSZ(dirlist_devices_input_event)));
	}

	return (ENOTDIR);
}

static int
lxsys_readdir_class_netdir(lxsys_node_t *lnp, uio_t *uiop, int *eofp)
{
	if (lnp->lxsys_type != LXSYS_CLASS_NET ||
	    lnp->lxsys_instance != 0) {
		/*
		 * Since /sys/class/net contains only symlinks, readdir
		 * operations should not be performed anywhere except the top
		 * level (instance == 0).
		 */
		return (ENOTDIR);
	}

	return (lxsys_readdir_ifaces(lnp, uiop, eofp, LXSYS_CLASS_NET));
}

static int
lxsys_readdir_devices_virtual_netdir(lxsys_node_t *lnp, uio_t *uiop, int *eofp)
{
	int error;

	if (lnp->lxsys_instance == 0) {
		/* top-level interface listing */
		error = lxsys_readdir_ifaces(lnp, uiop, eofp,
		    LXSYS_DEV_NET);
	} else if (lnp->lxsys_endpoint == 0) {
		/* interface-level sub-item listing */
		error = lxsys_readdir_subdir(lnp, uiop, eofp,
		    dirlist_devices_virtual_net,
		    SYSDIRLISTSZ(dirlist_devices_virtual_net));
	} else {
		/* there shouldn't be subdirs below this */
		error = ENOTDIR;
	}

	return (error);
}

static int
lxsys_readdir_blockdir(lxsys_node_t *lnp, uio_t *uiop, int *eofp)
{
	if (lnp->lxsys_type != LXSYS_BLOCK ||
	    lnp->lxsys_instance != 0) {
		/*
		 * Since /sys/block contains only symlinks, readdir operations
		 * should not be performed anywhere except the top level
		 * (instance == 0).
		 */
		return (ENOTDIR);
	}

	return (lxsys_readdir_disks(lnp, uiop, eofp, LXSYS_BLOCK));
}

static int
lxsys_readdir_devices_zfsdir(lxsys_node_t *lnp, uio_t *uiop, int *eofp)
{
	int error;

	if (lnp->lxsys_instance == 0) {
		/* top-level dev listing */
		error = lxsys_readdir_disks(lnp, uiop, eofp,
		    LXSYS_DEV_ZFS);
	} else if (lnp->lxsys_endpoint == 0) {
		/* disk-level sub-item listing */
		error = lxsys_readdir_subdir(lnp, uiop, eofp,
		    dirlist_devices_zfs_block,
		    SYSDIRLISTSZ(dirlist_devices_zfs_block));
	} else {
		/*
		 * Currently there shouldn't be subdirs below this but
		 * on a real Linux system some will be subdirs. This should
		 * be fixed when we populate the directory for real.
		 */
		error = ENOTDIR;
	}

	return (error);
}

/* Handle fixed entries within the cpu directory. */
static int
lxsys_do_sub_cpu(struct uio *uiop, ssize_t oresid, dirent64_t *dirent,
    char *nm, int inst, int *errp)
{
	int reclen;
	ssize_t uresid;

	(void) strncpy(dirent->d_name, nm, LXSNSIZ);

	dirent->d_ino = lxsys_inode(LXSYS_DEV_SYS_CPU, inst, 0);
	reclen = DIRENT64_RECLEN(strlen(dirent->d_name));

	uresid = uiop->uio_resid;
	if (reclen > uresid) {
		if (uresid == oresid) {
			/* Not enough space for one record */
			*errp = EINVAL;
		}
		return (-1);
	}
	if ((*errp = lxsys_dirent_out(dirent, reclen, uiop)) != 0) {
		return (-1);
	}

	return (0);
}

static int
lxsys_readdir_cpu(lxsys_node_t *ldp, struct uio *uiop, int *eofp)
{
	longlong_t bp[DIRENT64_RECLEN(LXSNSIZ) / sizeof (longlong_t)];
	dirent64_t *dirent = (dirent64_t *)bp;
	ssize_t oresid, uresid;
	int skip, error;
	int reclen;
	cpuset_t *avail;
	uint_t i, avlo, avhi;

	/* Emit "." and ".." entries */
	oresid = uiop->uio_resid;
	error = lxsys_readdir_common(ldp, uiop, eofp, NULL, 0);
	if (error != 0 || *eofp == 0) {
		return (error);
	}

	skip = (uiop->uio_offset/LXSYS_SDSIZE) - 2;

	/* Fixed entries */
	if (skip > 0) {
		skip--;
	} else {
		if (lxsys_do_sub_cpu(uiop, oresid, dirent, "kernel_max",
		    LXSYS_INST_DEV_SYSCPU_KMAX, &error) != 0)
			goto done;

		if (lxsys_do_sub_cpu(uiop, oresid, dirent, "offline",
		    LXSYS_INST_DEV_SYSCPU_OFFLINE, &error) != 0)
			goto done;

		if (lxsys_do_sub_cpu(uiop, oresid, dirent, "online",
		    LXSYS_INST_DEV_SYSCPU_ONLINE, &error) != 0)
			goto done;

		if (lxsys_do_sub_cpu(uiop, oresid, dirent, "possible",
		    LXSYS_INST_DEV_SYSCPU_POSSIBLE, &error) != 0)
			goto done;

		if (lxsys_do_sub_cpu(uiop, oresid, dirent, "present",
		    LXSYS_INST_DEV_SYSCPU_PRESENT, &error) != 0)
			goto done;
	}

	avail = cpuset_alloc(KM_SLEEP);
	cpuset_all(avail);

	/* Take a snapshot of the available set */
	mutex_enter(&cpu_lock);
	cpuset_and(avail, &cpu_available);
	mutex_exit(&cpu_lock);

	cpuset_bounds(avail, &avlo, &avhi);

	/* Output dynamic CPU info */
	for (i = avlo; i <= avhi; i++) {
		char cpunm[16];

		if (skip > 0) {
			skip--;
			continue;
		}

		if (!cpu_in_set(avail, i))
			continue;

		(void) snprintf(cpunm, sizeof (cpunm), "cpu%u", i);
		(void) strncpy(dirent->d_name, cpunm, LXSNSIZ);

		dirent->d_ino = lxsys_inode(LXSYS_DEV_SYS_CPUINFO, i + 1, 0);
		reclen = DIRENT64_RECLEN(strlen(dirent->d_name));

		uresid = uiop->uio_resid;
		if (reclen > uresid) {
			if (uresid == oresid) {
				/* Not enough space for one record */
				error = EINVAL;
			}
			break;
		}
		if ((error = lxsys_dirent_out(dirent, reclen, uiop)) != 0) {
			break;
		}
	}
	cpuset_free(avail);

	/* Indicate EOF if we reached the end of the CPU list. */
	if (i == avhi) {
		*eofp = 1;
	}

done:
	return (error);
}

static int
lxsys_readdir_devices_syscpu(lxsys_node_t *lnp, uio_t *uiop, int *eofp)
{
	int error;

	if (lnp->lxsys_instance == 0) {
		/* top-level cpu listing */
		error = lxsys_readdir_cpu(lnp, uiop, eofp);
	} else if (lnp->lxsys_endpoint == 0) {
		/* cpu-level sub-item listing */
		error = lxsys_readdir_subdir(lnp, uiop, eofp, NULL, 0);
	} else {
		/*
		 * Currently there shouldn't be subdirs below this but
		 * on a real Linux system some will be subdirs. This should
		 * be fixed when we populate the directory for real.
		 */
		error = ENOTDIR;
	}

	return (error);
}

static int
lxsys_readdir_devices_syscpuinfo(lxsys_node_t *lnp, uio_t *uiop, int *eofp)
{
	int error;

	if (lnp->lxsys_type != LXSYS_DEV_SYS_CPUINFO) {
		/*
		 * Since /sys/devices/system/cpu/cpuN is empty, readdir
		 * operations should not be performed anywhere except the top
		 * level.
		 */
		return (ENOTDIR);
	}

	/*
	 * Emit "." and ".." entries
	 * All cpuN directories are currently empty.
	 */
	error = lxsys_readdir_common(lnp, uiop, eofp, NULL, 0);
	if (error != 0 || *eofp == 0) {
		return (error);
	}

	/* Indicate EOF */
	*eofp = 1;

	return (error);
}

static int
lxsys_readdir_devices_sysnode(lxsys_node_t *lnp, uio_t *uiop, int *eofp)
{
	int error;

	if (lnp->lxsys_instance == 0) {
		/* top-level node listing */
		longlong_t bp[DIRENT64_RECLEN(LXSNSIZ) / sizeof (longlong_t)];
		dirent64_t *dirent = (dirent64_t *)bp;
		ssize_t oresid, uresid;
		int reclen, skip;

		/* Emit "." and ".." entries */
		oresid = uiop->uio_resid;
		error = lxsys_readdir_common(lnp, uiop, eofp, NULL, 0);
		if (error != 0 || *eofp == 0) {
			return (error);
		}
		skip = (uiop->uio_offset/LXSYS_SDSIZE) - 2;

		/* Fixed entries */
		if (skip > 0) {
			skip--;
		} else {
			(void) strncpy(dirent->d_name, "node0", LXSNSIZ);

			dirent->d_ino = lxsys_inode(LXSYS_DEV_SYS_NODE,
			    1, 0);
			reclen = DIRENT64_RECLEN(strlen(dirent->d_name));

			uresid = uiop->uio_resid;
			if (reclen > uresid) {
				if (uresid == oresid) {
					/* Not enough space for one record */
					return (EINVAL);
				}
				return (0);
			}
			error = lxsys_dirent_out(dirent, reclen, uiop);
		}
		/* Indicate EOF */
		if (error == 0) {
			*eofp = 1;
		}
	} else if (lnp->lxsys_endpoint == 0) {
		/* node-level sub-item listing */
		error = lxsys_readdir_subdir(lnp, uiop, eofp,
		    dirlist_devices_sysnode,
		    SYSDIRLISTSZ(dirlist_devices_sysnode));
	} else {
		/* there shouldn't be subdirs below this */
		error = ENOTDIR;
	}

	return (error);
}

/*
 * lxsys_readlink(): Vnode operation for VOP_READLINK()
 */
/* ARGSUSED */
static int
lxsys_readlink(vnode_t *vp, uio_t *uiop, cred_t *cr, caller_context_t *ct)
{
	char buf[MAXPATHLEN + 1];
	lxsys_node_t *lnp = VTOLXS(vp);
	lxsys_nodetype_t type = lnp->lxsys_type;
	int (*rlfunc)();
	int error;

	VERIFY(type > LXSYS_NONE && type < LXSYS_MAXTYPE);

	if (vp->v_type != VLNK) {
		return (EINVAL);
	}

	rlfunc = lxsys_readlink_function[lnp->lxsys_type];
	if (rlfunc != NULL) {
		if ((error = rlfunc(lnp, buf, sizeof (buf))) == 0) {
			error = uiomove(buf, strlen(buf), UIO_READ, uiop);
		}
	} else {
		error = EINVAL;
	}

	return (error);
}


static int
lxsys_readlink_class_net(lxsys_node_t *lnp, char *buf, size_t len)
{
	netstack_t *ns;
	ip_stack_t *ipst;
	avl_tree_t *phytree;
	phyint_t *phyi;
	uint_t ifindex;
	char ifname[LIFNAMSIZ];
	int error = EINVAL;

	if ((ifindex = lnp->lxsys_instance) == 0) {
		return (error);
	}

	if ((ns = lxsys_netstack(lnp)) == NULL) {
		return (error);
	}
	ipst = ns->netstack_ip;
	rw_enter(&ipst->ips_ill_g_lock, RW_READER);

	phytree = &ipst->ips_phyint_g_list->phyint_list_avl_by_index;
	phyi = avl_find(phytree, &ifindex, NULL);
	if (phyi != NULL) {
		(void) strncpy(ifname, phyi->phyint_name, LIFNAMSIZ);
		lx_ifname_convert(ifname, LX_IF_FROMNATIVE);
		(void) snprintf(buf, len, "/sys/devices/virtual/net/%s",
		    ifname);
		error = 0;
	}

	rw_exit(&ipst->ips_ill_g_lock);
	netstack_rele(ns);
	return (error);
}

static int
lxsys_readlink_class_input(lxsys_node_t *lnp, char *buf, size_t len)
{
	uint_t endpoint = lnp->lxsys_endpoint;
	uint_t slot;

	if (endpoint == 0)
		return (EINVAL);
	slot = (endpoint - 1) / 2;
	if (slot >= LX_INPUT_EVENT_COUNT ||
	    !lx_input_get_config(LXSTOZ(lnp)->zone_id, slot, NULL))
		return (EINVAL);

	if (endpoint == LXSYS_CLASS_INPUT_ENDPOINT(slot)) {
		(void) snprintf(buf, len,
		    "../../devices/virtual/input/input%u", slot);
	} else if (endpoint == LXSYS_CLASS_EVENT_ENDPOINT(slot)) {
		(void) snprintf(buf, len,
		    "../../devices/virtual/input/input%u/event%u", slot, slot);
	} else {
		return (EINVAL);
	}
	return (0);
}

static int
lxsys_readlink_devices_input(lxsys_node_t *lnp, char *buf, size_t len)
{
	if (lnp->lxsys_endpoint != LXSYS_ENDP_INPUT_EVENT_DEVICE)
		return (EINVAL);
	(void) strlcpy(buf, "..", len);
	return (0);
}

static int
lxsys_readlink_block(lxsys_node_t *lnp, char *buf, size_t len)
{
	int inst, error = EINVAL;
	lx_zone_data_t *lxzdata;
	lx_virt_disk_t *vd;

	if ((inst = lnp->lxsys_instance) == 0) {
		return (error);
	}

	lxzdata = ztolxzd(curproc->p_zone);
	if (lxzdata == NULL)
		return (error);
	ASSERT(lxzdata->lxzd_vdisks != NULL);

	vd = list_head(lxzdata->lxzd_vdisks);
	while (vd != NULL) {
		int vinst = getminor(vd->lxvd_emul_dev) & 0xffff;

		if (vinst == inst) {
			(void) snprintf(buf, len,
			    "../devices/zfs/%s", vd->lxvd_name);
			error = 0;
			break;
		}
		vd = list_next(lxzdata->lxzd_vdisks, vd);
	}

	return (error);
}

/*
 * lxsys_inactive(): Vnode operation for VOP_INACTIVE()
 * Vnode is no longer referenced, deallocate the file
 * and all its resources.
 */
/* ARGSUSED */
static void
lxsys_inactive(vnode_t *vp, cred_t *cr, caller_context_t *ct)
{
	lxsys_freenode(VTOLXS(vp));
}

/*
 * lxsys_sync(): Vnode operation for VOP_SYNC()
 */
static int
lxsys_sync()
{
	/*
	 * Nothing to sync but this function must never fail
	 */
	return (0);
}

/*
 * lxsys_cmp(): Vnode operation for VOP_CMP()
 */
static int
lxsys_cmp(vnode_t *vp1, vnode_t *vp2, caller_context_t *ct)
{
	if (vn_matchops(vp1, lxsys_vnodeops) ||
	    vn_matchops(vp2, lxsys_vnodeops))
		return (vp1 == vp2);
	return (VOP_CMP(vp1, vp2, ct));
}
