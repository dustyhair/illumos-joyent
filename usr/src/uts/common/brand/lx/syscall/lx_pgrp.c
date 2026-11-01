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
 * Copyright 2016 Joyent, Inc.
 */

#include <sys/lx_misc.h>
#include <sys/lx_pidns.h>

#define	LX_INIT_PGID	1
#define	LX_INIT_SID	1

/* From uts/common/syscall/pgrpsys.c */
extern int setpgrp(int, int, int);

long
lx_getpgrp(void)
{
	int pg;
	lx_pidns_t *ns = lx_pidns_active(curproc);

	/* getpgrp() */
	pg = setpgrp(0, 0, 0);
	if (ns != NULL)
		return (lx_pidns_visible(ns, pg));

	/*
	 * If the pgrp is that of the init process, return the value Linux
	 * expects.
	 */
	if (pg == curzone->zone_proc_initpid)
		return (LX_INIT_PGID);

	return (pg);
}

long
lx_getpgid(int pid)
{
	pid_t spid;
	int tid;
	int pg;
	lx_pidns_t *ns = lx_pidns_active(curproc);

	if (pid < 0)
		return (set_errno(ESRCH));

	/*
	 * If the supplied pid matches that of the init process, return the pgid
	 * Linux expects.
	 */
	if (ns == NULL && pid == curzone->zone_proc_initpid)
		return (LX_INIT_PGID);

	if (pid == 0) {
		spid = curproc->p_pid;
	} else if (ns != NULL) {
		if ((spid = lx_pidns_map_backing(ns, pid)) == 0)
			return (set_errno(ESRCH));
	} else if (lx_lpid_to_spair(pid, &spid, &tid) < 0) {
		return (set_errno(ESRCH));
	}

	/* getpgid() */
	ttolwp(curthread)->lwp_errno = 0;
	pg = setpgrp(4, spid, 0);
	if (ttolwp(curthread)->lwp_errno != 0)
		return (ttolwp(curthread)->lwp_errno);
	if (ns != NULL)
		return (lx_pidns_visible(ns, pg));

	/*
	 * If the pgid is that of the init process, return the value Linux
	 * expects.
	 */
	if (pg == curzone->zone_proc_initpid)
		return (LX_INIT_PGID);

	return (pg);
}

long
lx_setpgid(pid_t pid, pid_t pgid)
{
	pid_t spid, spgid;
	int tid;
	lx_pidns_t *ns = lx_pidns_active(curproc);

	if (pid < 0)
		return (set_errno(ESRCH));

	if (pgid < 0)
		return (set_errno(EINVAL));

	if (pid == 0) {
		spid = curproc->p_pid;
	} else if (ns != NULL) {
		if ((spid = lx_pidns_map_backing(ns, pid)) == 0)
			return (set_errno(ESRCH));
	} else if (lx_lpid_to_spair(pid, &spid, &tid) < 0) {
		return (set_errno(ESRCH));
	}

	/*
	 * Process group IDs are native PID identities, except for init.
	 * A populated group can outlive its leader, so do not require a
	 * live task or translate a synthetic LWP ID here.  setpgrp() looks
	 * up the group under pidlock and checks the zone and session.
	 */
	if (pgid == 0)
		spgid = spid;
	else if (ns != NULL) {
		if ((spgid = lx_pidns_map_backing(ns, pgid)) == 0)
			return (set_errno(ESRCH));
	} else if (pgid == LX_INIT_PGID)
		spgid = curzone->zone_proc_initpid;
	else
		spgid = pgid;

	/* setpgid() */
	return (setpgrp(5, spid, spgid));
}

long
lx_getsid(int pid)
{
	pid_t spid;
	int tid;
	int sid;
	lx_pidns_t *ns = lx_pidns_active(curproc);

	if (pid < 0)
		return (set_errno(ESRCH));

	/*
	 * If the supplied pid matches that of the init process, return the sid
	 * Linux expects.
	 */
	if (ns == NULL && pid == curzone->zone_proc_initpid)
		return (LX_INIT_SID);

	if (pid == 0) {
		spid = curproc->p_pid;
	} else if (ns != NULL) {
		if ((spid = lx_pidns_map_backing(ns, pid)) == 0)
			return (set_errno(ESRCH));
	} else if (lx_lpid_to_spair(pid, &spid, &tid) < 0) {
		return (set_errno(ESRCH));
	}

	/* getsid() */
	ttolwp(curthread)->lwp_errno = 0;
	sid = setpgrp(2, spid, 0);
	if (ttolwp(curthread)->lwp_errno != 0)
		return (ttolwp(curthread)->lwp_errno);
	if (ns != NULL)
		return (lx_pidns_visible(ns, sid));


	/*
	 * If the sid is that of the init process, return the value Linux
	 * expects.
	 */
	if (sid == curzone->zone_proc_initpid)
		return (LX_INIT_SID);

	return (sid);
}

long
lx_setsid(void)
{
	int sid;
	lx_pidns_t *ns = lx_pidns_active(curproc);

	/* setsid() */
	ttolwp(curthread)->lwp_errno = 0;
	sid = setpgrp(3, 0, 0);
	if (ttolwp(curthread)->lwp_errno != 0)
		return (ttolwp(curthread)->lwp_errno);
	if (ns != NULL)
		return (lx_pidns_visible(ns, sid));

	/*
	 * If the sid is that of the init process, return the value Linux
	 * expects.
	 */
	if (sid == curzone->zone_proc_initpid)
		return (LX_INIT_SID);

	return (sid);
}
