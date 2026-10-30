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
 * Copyright 2019 Joyent, Inc.
 */

/* Kernel capget/capset using the existing native privilege mapping. */
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/cred_impl.h>
#include <sys/priv_impl.h>
#include <sys/lx_brand.h>
#include <sys/lx_syscalls.h>
#include <sys/lx_userns.h>
#include <sys/lx_cap.h>

#define	LX_CAP_VERSION_1	0x19980330U
#define	LX_CAP_VERSION_2	0x20071026U
#define	LX_CAP_VERSION_3	0x20080522U

typedef struct lx_cap_data {
	uint32_t lcd_effective;
	uint32_t lcd_permitted;
	uint32_t lcd_inheritable;
} lx_cap_data_t;

static const priv_t lx_cap_map_chown[] = {
	PRIV_FILE_CHOWN,
	PRIV_FILE_CHOWN_SELF,
	-1
};
static const priv_t lx_cap_map_dac_override[] = {
	PRIV_FILE_DAC_READ,
	PRIV_FILE_DAC_WRITE,
	PRIV_FILE_DAC_EXECUTE,
	-1
};
static const priv_t lx_cap_map_dac_read_search[] = {
	PRIV_FILE_DAC_SEARCH,
	PRIV_FILE_DAC_READ,
	-1
};
static const priv_t lx_cap_map_fowner[] = { PRIV_FILE_OWNER, -1 };
static const priv_t lx_cap_map_fsetid[] = { PRIV_FILE_SETID, -1 };
static const priv_t lx_cap_map_kill[] = { PRIV_PROC_OWNER, -1 };
/*
 * One way that Linux capabilities(7) differs from Illumos privileges(5) is
 * that it distinguishes between setuid and setgroups rights.  This will be a
 * problem if an lx-branded process requests to drop only CAP_SETUID but not
 * CAP_SETGID.
 *
 * The native path rejects a request that cannot be represented exactly.
 */
static const priv_t lx_cap_map_setgid[] = { PRIV_PROC_SETID, -1 };
static const priv_t lx_cap_map_setuid[] = { PRIV_PROC_SETID, -1 };
static const priv_t lx_cap_map_linux_immutable[] = { PRIV_FILE_FLAG_SET, -1 };
static const priv_t lx_cap_map_bind_service[] = { PRIV_NET_PRIVADDR, -1 };
static const priv_t lx_cap_map_net_admin[] = {
	PRIV_SYS_IP_CONFIG,
	-1
	/*
	 * It would probably make sense to include PRIV_SYS_DL_CONFIG, but that
	 * privilege is not extended to non-global zones by default.  A more
	 * sophisticated capabilities translation layer could make it optional.
	 */
};
static const priv_t lx_cap_map_net_raw[] = {
	PRIV_NET_RAWACCESS,
	PRIV_NET_ICMPACCESS,
	-1
};
static const priv_t lx_cap_map_ipc_lock[] = { PRIV_PROC_LOCK_MEMORY, -1 };
static const priv_t lx_cap_map_ipc_owner[] = {
	PRIV_IPC_DAC_READ,
	PRIV_IPC_DAC_WRITE,
	PRIV_IPC_OWNER,
	-1
};
static const priv_t lx_cap_map_sys_chroot[] = { PRIV_PROC_CHROOT, -1 };
static const priv_t lx_cap_map_sys_admin[] = {
	PRIV_SYS_MOUNT,
	PRIV_SYS_ADMIN,
	-1
};
static const priv_t lx_cap_map_sys_nice[] = { PRIV_PROC_PRIOUP, -1 };
static const priv_t lx_cap_map_sys_resource[] = { PRIV_SYS_RESOURCE, -1 };
static const priv_t lx_cap_map_audit_write[] = { PRIV_PROC_AUDIT, -1 };
static const priv_t lx_cap_map_audit_control[] = { PRIV_SYS_AUDIT, -1 };

/*
 * Mapping of Linux capabilities -> Illumos privileges
 * The ID definitions can be found in the Linux sources here:
 * include/uapi/linux/capability.h
 *
 * Order is critical.
 */
static const priv_t *lx_cap_mapping[LX_CAP_LAST + 1] = {
	lx_cap_map_chown,		/* CAP_CHOWN */
	lx_cap_map_dac_override,	/* CAP_DAC_OVERRIDE */
	lx_cap_map_dac_read_search,	/* CAP_DAC_READ_SEARCH */
	lx_cap_map_fowner,		/* CAP_FOWNER */
	lx_cap_map_fsetid,		/* CAP_FSETID */
	lx_cap_map_kill,		/* CAP_KILL */
	lx_cap_map_setgid,		/* CAP_SETGID */
	lx_cap_map_setuid,		/* CAP_SETUID */
	NULL,				/* CAP_SETPCAP */
	lx_cap_map_linux_immutable,	/* CAP_LINUX_IMMUTABLE */
	lx_cap_map_bind_service,	/* CAP_BIND_SERVICE */
	NULL,				/* CAP_BROADCAST */
	lx_cap_map_net_admin,		/* CAP_NET_ADMIN */
	lx_cap_map_net_raw,		/* CAP_NET_RAW */
	lx_cap_map_ipc_lock,		/* CAP_IPC_LOCK */
	lx_cap_map_ipc_owner,		/* CAP_IPC_OWNER */
	NULL,				/* CAP_MODULE */
	NULL,				/* CAP_RAWIO */
	lx_cap_map_sys_chroot,		/* CAP_SYS_CHROOT */
	NULL,				/* CAP_PTRACE */
	NULL,				/* CAP_PACCT */
	lx_cap_map_sys_admin,		/* CAP_SYS_ADMIN */
	NULL,				/* CAP_BOOT */
	lx_cap_map_sys_nice,		/* CAP_SYS_NICE */
	lx_cap_map_sys_resource,	/* CAP_SYS_RESOURCE */
	NULL,				/* CAP_SYS_TIME */
	NULL,				/* CAP_SYS_TTY_CONFIG */
	NULL,				/* CAP_MKNOD */
	NULL,				/* CAP_LEASE */
	lx_cap_map_audit_write,		/* CAP_AUDIT_WRITE */
	lx_cap_map_audit_control,	/* CAP_AUDIT_CONTROL */
	NULL,				/* CAP_SETFCAP */
	NULL,				/* CAP_MAC_OVERRIDE */
	NULL,				/* CAP_MAC_ADMIN */
	NULL,				/* CAP_SYSLOG */
	NULL,				/* CAP_WAKE_ALARM */
	NULL				/* CAP_BLOCK_SUSPEND */
};


static uint64_t
lx_cap_from_priv(const priv_set_t *priv)
{
	uint64_t caps = 0;
	uint_t i, j;

	for (i = 0; i <= LX_CAP_LAST; i++) {
		if (lx_cap_mapping[i] == NULL)
			continue;
		for (j = 0; lx_cap_mapping[i][j] != -1; j++) {
			if (!priv_ismember(priv, lx_cap_mapping[i][j]))
				break;
		}
		if (lx_cap_mapping[i][j] == -1)
			caps |= LX_CAP_BIT(i);
	}
	return (caps);
}

void
lx_cap_native_get(const cred_t *cred, lx_caps_t *caps)
{
	bzero(caps, sizeof (*caps));
	caps->lc_effective = lx_cap_from_priv(&CR_OEPRIV(cred));
	caps->lc_permitted = lx_cap_from_priv(&CR_OPPRIV(cred));
	caps->lc_inheritable = lx_cap_from_priv(&CR_IPRIV(cred));
	caps->lc_bounding = lx_cap_from_priv(&CR_LPRIV(cred));
}

static int
lx_cap_to_priv(const priv_set_t *old, uint64_t wanted, priv_set_t *out)
{
	uint64_t changes = lx_cap_from_priv(old) ^ wanted;
	priv_set_t basic = *old;
	uint_t i, j;

	*out = *old;
	for (i = 0; i <= LX_CAP_LAST; i++) {
		if ((changes & LX_CAP_BIT(i)) == 0 ||
		    (wanted & LX_CAP_BIT(i)) != 0 || lx_cap_mapping[i] == NULL)
			continue;
		for (j = 0; lx_cap_mapping[i][j] != -1; j++)
			priv_delset(out, lx_cap_mapping[i][j]);
	}
	/* Retain shared privileges required by any requested capability. */
	for (i = 0; i <= LX_CAP_LAST; i++) {
		if ((wanted & LX_CAP_BIT(i)) == 0 || lx_cap_mapping[i] == NULL)
			continue;
		for (j = 0; lx_cap_mapping[i][j] != -1; j++)
			priv_addset(out, lx_cap_mapping[i][j]);
	}
	/* E.g. file_chown_self remains useful without Linux CAP_CHOWN. */
	priv_intersect(priv_basic, &basic);
	priv_union(&basic, out);
	return (lx_cap_from_priv(out) == wanted ? 0 : EPERM);
}

/* Prepare an unpublished credential; failures leave it unchanged. */
int
lx_cap_native_update(cred_t *cred, const lx_caps_t *wanted)
{
	lx_caps_t caps;
	priv_set_t effective, permitted, inheritable;
	int error;

	if (crgetbrand(cred) != NULL)
		return (EOPNOTSUPP);
	lx_cap_native_get(cred, &caps);
	if ((error = lx_caps_set(&caps, wanted->lc_effective,
	    wanted->lc_permitted, wanted->lc_inheritable)) != 0)
		return (error);
	if ((error = lx_cap_to_priv(&CR_OEPRIV(cred), caps.lc_effective,
	    &effective)) != 0 ||
	    (error = lx_cap_to_priv(&CR_OPPRIV(cred), caps.lc_permitted,
	    &permitted)) != 0 ||
	    (error = lx_cap_to_priv(&CR_IPRIV(cred), caps.lc_inheritable,
	    &inheritable)) != 0)
		return (error);
	if (!priv_issubset(&effective, &permitted))
		return (EPERM);
	priv_set_PA(cred);
	CR_EPRIV(cred) = effective;
	CR_PPRIV(cred) = permitted;
	CR_IPRIV(cred) = inheritable;
	return (0);
}

/* Linux validates only the version field before the capget NULL-data case. */
static int
lx_cap_version(uintptr_t header, uint_t *count)
{
	uint32_t version;

	if (copyin((void *)header, &version, sizeof (version)) != 0)
		return (EFAULT);
	switch (version) {
	case LX_CAP_VERSION_1:
		*count = 1;
		return (0);
	case LX_CAP_VERSION_2:
	case LX_CAP_VERSION_3:
		*count = 2;
		return (0);
	default:
		version = LX_CAP_VERSION_3;
		if (copyout(&version, (void *)header, sizeof (version)) != 0)
			return (EFAULT);
		return (EINVAL);
	}
}

long
lx_capget(uintptr_t header, uintptr_t address)
{
	lx_cap_data_t data[2];
	lx_caps_t caps;
	cred_t *cred;
	proc_t *target;
	uint_t count, i;
	int32_t pid;
	int error = lx_cap_version(header, &count);

	if (address == 0 && error == EINVAL)
		return (0);
	if (error != 0)
		return (set_errno(error));
	if (address == 0)
		return (0);
	if (copyin((void *)(header + sizeof (uint32_t)), &pid,
	    sizeof (pid)) != 0)
		return (set_errno(EFAULT));
	if (pid < 0)
		return (set_errno(EINVAL));
	if (pid == 0 || pid == lx_gettid()) {
		crhold(cred = CRED());
	} else {
		if (lx_lpid_lock(pid, curproc->p_zone, LXP_ZOMBOK,
		    &target, NULL) != 0)
			return (set_errno(ESRCH));
		mutex_enter(&target->p_crlock);
		crhold(cred = target->p_cred);
		mutex_exit(&target->p_crlock);
		mutex_exit(&target->p_lock);
	}
	if (crgetbrand(cred) != NULL) {
		error = lx_userns_caps_get(cred, &caps);
	} else {
		lx_cap_native_get(cred, &caps);
	}
	crfree(cred);
	if (error != 0)
		return (set_errno(error));
	for (i = 0; i < count; i++) {
		data[i].lcd_effective = caps.lc_effective >> (i * 32);
		data[i].lcd_permitted = caps.lc_permitted >> (i * 32);
		data[i].lcd_inheritable = caps.lc_inheritable >> (i * 32);
	}
	if (copyout(data, (void *)address, count * sizeof (data[0])) != 0)
		return (set_errno(EFAULT));
	return (0);
}

long
lx_capset(uintptr_t header, uintptr_t address)
{
	lx_cap_data_t data[2] = { 0 };
	lx_caps_t wanted = { 0 };
	priv_set_t removed;
	cred_t *candidate, *old;
	proc_t *p = curproc;
	boolean_t nondump;
	uint_t count;
	int32_t pid;
	int error = lx_cap_version(header, &count);

	if (error != 0)
		return (set_errno(error));
	if (copyin((void *)(header + sizeof (uint32_t)), &pid,
	    sizeof (pid)) != 0)
		return (set_errno(EFAULT));
	if (pid != 0 && pid != lx_gettid())
		return (set_errno(EPERM));
	if (copyin((void *)address, data, count * sizeof (data[0])) != 0)
		return (set_errno(EFAULT));
	wanted.lc_effective = data[0].lcd_effective |
	    ((uint64_t)data[1].lcd_effective << 32);
	wanted.lc_permitted = data[0].lcd_permitted |
	    ((uint64_t)data[1].lcd_permitted << 32);
	wanted.lc_inheritable = data[0].lcd_inheritable |
	    ((uint64_t)data[1].lcd_inheritable << 32);

	candidate = cralloc();
	mutex_enter(&p->p_crlock);
	old = p->p_cred;
	crdup_to(old, candidate);
	error = lx_cap_native_update(candidate, &wanted);
	if (error != 0) {
		mutex_exit(&p->p_crlock);
		crfree(candidate);
		return (set_errno(error));
	}
	/* Retain the native ppriv rule for non-root privilege reductions. */
	removed = CR_OPPRIV(candidate);
	priv_inverse(&removed);
	priv_intersect(&CR_OPPRIV(old), &removed);
	nondump = crgetuid(candidate) != 0 && crgetruid(candidate) != 0 &&
	    crgetsuid(candidate) != 0 &&
	    !priv_issubset(&removed, &CR_IPRIV(candidate));
	crhold(candidate); /* One reference for p_cred, one for this thread. */
	p->p_cred = candidate;
	mutex_exit(&p->p_crlock);
	crfree(old);
	if (nondump) {
		mutex_enter(&p->p_lock);
		p->p_flag |= SNOCD;
		mutex_exit(&p->p_lock);
	}
	crset(p, candidate);
	return (0);
}
