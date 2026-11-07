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
 * Copyright 2006 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 * Copyright 2016 Joyent, Inc.
 */

#include <sys/types.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/zone.h>
#include <sys/thread.h>
#include <sys/signal.h>
#include <sys/brand.h>
#include <sys/lx_brand.h>
#include <sys/lx_syscalls.h>
#include <sys/lx_siginfo.h>
#include <sys/lx_pidns.h>
#include <lx_signum.h>
#include <sys/contract/process_impl.h>

extern int kill(pid_t, int);

static pid_t
lx_signal_backing(lx_pidns_t *ns, pid_t id)
{
	return (ns == NULL ? id : lx_pidns_map_backing(ns, id));
}

static pid_t
lx_signal_sender(proc_t *target)
{
	lx_pidns_t *ns = lx_pidns_active(target);

	return (ns == NULL ? curproc->p_pid :
	    lx_pidns_visible(ns, curproc->p_pid));
}

/*
 * Check if it is legal to send this signal to the init process.  Linux
 * kill(2) semantics dictate that no _unhandled_ signal may be sent to pid
 * 1.
 */
static int
lx_init_sig_check(int sig, pid_t pid)
{
	proc_t *p;
	int rv = 0;

	mutex_enter(&pidlock);
	if ((p = prfind(pid)) == NULL || p->p_stat == SIDL) {
		rv = ESRCH;
	} else if (sig != 0) {
		if (sigismember(&cantmask, sig)) {
			rv = EPERM;
		} else {
			mutex_enter(&p->p_lock);
			if (PTOU(p)->u_signal[sig-1] == SIG_DFL ||
			    PTOU(p)->u_signal[sig-1] == SIG_IGN) {
				rv = EPERM;
			}
			mutex_exit(&p->p_lock);
		}
	}
	mutex_exit(&pidlock);

	return (rv);
}

static long
lx_thrkill(pid_t tgid, pid_t pid, int lx_sig, boolean_t tgkill)
{
	kthread_t *t;
	proc_t *pp, *cp = curproc;
	sigqueue_t *sqp;
	int sig, rv;
	lx_pidns_t *ns = lx_pidns_active(cp);
	pid_t backing, group;
	boolean_t nsinit;

	/*
	 * Unlike kill(2), Linux tkill(2) doesn't allow signals to
	 * be sent to process IDs <= 0 as it doesn't overlay any special
	 * semantics on the pid.
	 */
	if ((pid <= 0) || ((lx_sig < 0) || (lx_sig > LX_NSIG)) ||
	    ((sig = ltos_signo[lx_sig]) < 0))
		return (set_errno(EINVAL));

	/*
	 * If the Linux pid is 1, translate the pid to the actual init
	 * pid for the zone.  Note that Linux dictates that no unhandled
	 * signals may be sent to init, so check for that, too.
	 *
	 * Otherwise, extract the tid and real pid from the Linux pid.
	 */
	if ((backing = lx_signal_backing(ns, pid)) == 0)
		return (set_errno(ESRCH));
	nsinit = (ns != NULL && pid == 1) ||
	    (ns == NULL && pid == cp->p_zone->zone_proc_initpid);
	if (nsinit && backing != cp->p_pid) {
		if ((rv = lx_init_sig_check(sig, backing)) != 0) {
			return (set_errno(rv));
		}
	}
	sqp = kmem_zalloc(sizeof (sigqueue_t), KM_SLEEP);
	/*
	 * Find the process for the passed pid...
	 */
	if (lx_lpid_lock(backing, curzone, 0, &pp, &t) != 0) {
		rv = set_errno(ESRCH);
		goto free_and_exit;
	}

	/*
	 * Make sure the thread group matches the thread.
	 */
	if (tgkill) {
		group = lx_signal_backing(ns, tgid);
		if (group == 0 || group != pp->p_pid) {
			mutex_exit(&pp->p_lock);
			rv = set_errno(ESRCH);
			goto free_and_exit;
		}
	}

	/*
	 * SIGCONT permits a same-session sender regardless of credentials.
	 * Other sends, including SIGCONT across sessions, require permission.
	 */
	if (!(sig == SIGCONT && pp->p_sessp == cp->p_sessp) &&
	    !prochasprocperm(pp, cp, CRED())) {
		mutex_exit(&pp->p_lock);
		rv = set_errno(EPERM);
		goto free_and_exit;
	}

	/* a signal of 0 means just check for the existence of the thread */
	if (lx_sig == 0) {
		mutex_exit(&pp->p_lock);
		rv = 0;
		goto free_and_exit;
	}
	/* PID1 may generate a default-disposition signal without dying. */
	if (nsinit && pp == cp && !sigismember(&cantmask, sig) &&
	    (PTOU(pp)->u_signal[sig - 1] == SIG_DFL ||
	    PTOU(pp)->u_signal[sig - 1] == SIG_IGN)) {
		mutex_exit(&pp->p_lock);
		rv = 0;
		goto free_and_exit;
	}

	sqp->sq_info.si_signo = sig;
	sqp->sq_info.si_code = SI_LWP;
	sqp->sq_info.si_pid = lx_signal_sender(pp);
	sqp->sq_info.si_zoneid = getzoneid();
	sqp->sq_info.si_uid = crgetruid(CRED());
	sigaddqa(pp, t, sqp);

	mutex_exit(&pp->p_lock);

	return (0);

free_and_exit:
	kmem_free(sqp, sizeof (sigqueue_t));
	return (rv);
}

long
lx_tgkill(pid_t tgid, pid_t pid, int lx_sig)
{
	return (lx_thrkill(tgid, pid, lx_sig, B_TRUE));
}

long
lx_tkill(pid_t pid, int lx_sig)
{
	return (lx_thrkill(0, pid, lx_sig, B_FALSE));
}

long
lx_kill(pid_t lx_pid, int lx_sig)
{
	pid_t s_pid, initpid;
	sigsend_t v;
	zone_t *zone = curzone;
	struct proc *p;
	int err, sig, nfound;
	lx_pidns_t *ns = lx_pidns_active(curproc);

	if ((lx_sig < 0) || (lx_sig > LX_NSIG) ||
	    ((sig = ltos_signo[lx_sig]) < 0))
		return (set_errno(EINVAL));

	initpid = zone->zone_proc_initpid;
	if (lx_pid == 0 || lx_pid == -1) {
		s_pid = 0;
	} else if (lx_pid > 0) {
		/*
		 * Translations for individual processes (including pid 1) is
		 * all handled by lx_lpid_to_spair.
		 */
		if (ns != NULL) {
			s_pid = lx_pidns_map_backing(ns, lx_pid);
		} else if (lx_lpid_to_spair(lx_pid, &s_pid, NULL) != 0) {
			s_pid = 0;
		}
		if (s_pid == 0) {
			/*
			 * If we didn't find this pid that means it doesn't
			 * exist in this zone.
			 */
			return (set_errno(ESRCH));
		}
	} else {
		ASSERT(lx_pid < 0);
		if (ns != NULL) {
			s_pid = lx_pidns_map_backing(ns, -lx_pid);
			if (s_pid == 0)
				return (set_errno(ESRCH));
		} else if (lx_lpid_to_spair(-lx_pid, &s_pid, NULL) != 0) {
			/*
			 * If we didn't find this pid it means that the
			 * process group leader doesn't exist in this zone.
			 * In this case assuming that the Linux pid is
			 * the same as the Solaris pid will get us the
			 * correct behavior.
			 */
			s_pid = -lx_pid;
		}
	}

	/*
	 * Check that it is legal for this signal to be sent to init
	 */
	if (((ns != NULL && lx_pid == 1) ||
	    (ns == NULL && s_pid == initpid)) &&
	    (err = lx_init_sig_check(sig, s_pid)) != 0)
		return (set_errno(err));

	/*
	 * For individual processes, kill() semantics are the same between
	 * Solaris and Linux.
	 */
	if (lx_pid >= 0)
		return (kill(s_pid, sig));

	/*
	 * In Solaris, sending a signal to -pid means "send a signal to
	 * everyone in process group pid."  In Linux it means "send a
	 * signal to everyone in the group other than init."  Sending a
	 * signal to -1 means "send a signal to every process except init
	 * and myself."
	 */

	bzero(&v, sizeof (v));
	v.sig = sig;
	v.checkperm = 1;
	v.sicode = SI_USER;
	err = 0;

	mutex_enter(&pidlock);

	p = (lx_pid == -1) ? practive : pgfind(s_pid);
	nfound = 0;
	while (err == 0 && p != NULL) {
		pid_t visible = ns == NULL ? p->p_pid :
		    lx_pidns_visible(ns, p->p_pid);

		if ((p->p_zone == zone) && (p->p_stat != SIDL) &&
		    visible != 0 && visible != 1 &&
		    (p->p_pid != initpid) && (lx_pid < -1 || p != curproc)) {
			nfound++;
			err = sigsendproc(p, &v);
		}

		p = (lx_pid == -1) ? p->p_next : p->p_pglink;
	}
	mutex_exit(&pidlock);

	/*
	 * If we found no processes, we'll return ESRCH -- but unlike our
	 * native kill(2), we do not return EPERM if processes are found but
	 * we did not have permission to send any of them a signal.
	 */
	if (nfound == 0)
		err = ESRCH;

	return (err ? set_errno(err) : 0);
}

/*
 * This handles the unusual case where the user sends a non-queueable signal
 * through rt_sigqueueinfo. Signals sent with codes that indicate they are
 * queuable are sent through the sigqueue syscall via the user level function
 * lx_rt_sigqueueinfo().
 */
int
lx_helper_rt_sigqueueinfo(pid_t tgid, int sig, siginfo_t *uinfo)
{
	proc_t *target_proc;
	pid_t s_pid;
	zone_t *zone = curproc->p_zone;
	sigsend_t send;
	int err;
	siginfo_t kinfo;
	lx_pidns_t *ns = lx_pidns_active(curproc);

	if (copyin(uinfo, &kinfo, sizeof (siginfo_t)) != 0)
		return (set_errno(EFAULT));
	/* Unlike in lx_kill, this process id must be exact, no negatives. */
	if (tgid == 0)
		return (set_errno(ESRCH));
	if (tgid < 0)
		return (set_errno(EINVAL));
	/*
	 * Translate init directly, otherwise use the convenient utility
	 * function to translate. Since we're sending to the whole group, we
	 * only need the solaris pid, and not the lwp id.
	 */
	if (ns != NULL) {
		if ((s_pid = lx_pidns_map_backing(ns, tgid)) == 0)
			return (set_errno(ESRCH));
	} else if (tgid == 1) {
		s_pid = zone->zone_proc_initpid;
	} else {
		if (lx_lpid_to_spair(tgid, &s_pid, NULL) != 0) {
			/*
			 * If we didn't find this pid that means it doesn't
			 * exist in this zone.
			 */
			return (set_errno(ESRCH));
		}
	}
	/*
	 * We shouldn't have queuable signals here, those are sent elsewhere by
	 * the usermode handler for this emulated call.
	 */
	if (!SI_CANQUEUE(kinfo.si_code)) {
		return (set_errno(EINVAL));
	}
	/* Since our signal shouldn't queue, we just call sigsendproc(). */
	bzero(&send, sizeof (send));
	send.sig = sig;
	send.checkperm = 1;
	send.sicode = kinfo.si_code;
	send.value = kinfo.si_value;

	mutex_enter(&pidlock);
	target_proc = prfind(s_pid);
	err = 0;
	if (target_proc != NULL) {
		err = sigsendproc(target_proc, &send);
		if (err == 0 && send.perm == 0)
			err = EPERM;
	} else {
		err = ESRCH;
	}
	mutex_exit(&pidlock);

	return (err ? set_errno(err) : 0);
}

/*
 * Unlike the above function, this handles all system calls to rt_tgsigqueue
 * regardless of si_code.
 */
static int
lx_rt_tgsigqueueinfo_common(pid_t tgid, pid_t tid, int sig,
    const siginfo_t *kinfo)
{
	int err;
	proc_t *p = NULL;
	proc_t *cp = curproc;
	kthread_t *t;
	sigqueue_t *sqp = NULL;
	lx_pidns_t *ns = lx_pidns_active(cp);
	pid_t backing_tid, backing_tgid;

	if (tgid <= 0 || tid <= 0)
		return (set_errno(EINVAL));

	/* Reserved codes may only be supplied to the calling thread. */
	if ((kinfo->si_code >= 0 || kinfo->si_code == SI_LWP) &&
	    tid != lx_gettid())
		return (set_errno(EPERM));

	/* Accept only codes supported by the existing usermode conversion. */
	if (kinfo->si_code < 0 && kinfo->si_code != SI_LWP &&
	    kinfo->si_code != SI_QUEUE && kinfo->si_code != SI_TIMER &&
	    kinfo->si_code != SI_ASYNCIO && kinfo->si_code != SI_MESGQ)
		return (set_errno(EINVAL));
	backing_tid = lx_signal_backing(ns, tid);
	backing_tgid = lx_signal_backing(ns, tgid);
	if (backing_tid == 0 || backing_tgid == 0)
		return (set_errno(ESRCH));

	/* Never allocate for signal zero, or sleep with target p_lock held. */
	if (sig != 0)
		sqp = kmem_zalloc(sizeof (sigqueue_t), KM_SLEEP);

	if (lx_lpid_lock(backing_tid, curzone, 0, &p, &t) != 0) {
		err = ESRCH;
		goto errout;
	}

	/* PID 1 names the zone init, including its nonleader threads. */
	if (backing_tgid != p->p_pid) {
		err = ESRCH;
		goto errout;
	}

	/* As in Linux, target lookup precedes signal and credential checks. */
	if (sig < 0 || sig >= NSIG) {
		err = EINVAL;
		goto errout;
	}

	/* SIGCONT has the same-session exemption used by Linux kill. */
	if (!(sig == SIGCONT && p->p_sessp == cp->p_sessp) &&
	    !prochasprocperm(p, cp, CRED())) {
		err = EPERM;
		goto errout;
	}

	if (sig != 0) {
		sqp->sq_info.si_signo = sig;
		sqp->sq_info.si_code = kinfo->si_code;
		sqp->sq_info.si_errno = kinfo->si_errno;
		/* Linux permits supplied sender metadata for queued signals. */
		sqp->sq_info.si_pid = kinfo->si_pid;
		sqp->sq_info.si_uid = kinfo->si_uid;
		sqp->sq_info.si_value = kinfo->si_value;
		sqp->sq_info.si_ctid = PRCTID(cp);
		sqp->sq_info.si_zoneid = getzoneid();
		/* sigaddqa consumes the queue, including ignored signals. */
		sigaddqa(p, t, sqp);
	}
	mutex_exit(&p->p_lock);
	return (0);

errout:
	if (p != NULL)
		mutex_exit(&p->p_lock);
	siginfofree(sqp);
	return (set_errno(err));
}

int
lx_helper_rt_tgsigqueueinfo(pid_t tgid, pid_t tid, int sig, siginfo_t *uinfo)
{
	siginfo_t kinfo;

	bzero(&kinfo, sizeof (kinfo));
#ifdef _SYSCALL32_IMPL
	if (get_udatamodel() == DATAMODEL_ILP32) {
		siginfo32_t info32;

		if (copyin(uinfo, &info32, sizeof (info32)) != 0)
			return (set_errno(EFAULT));
		kinfo.si_code = info32.si_code;
		kinfo.si_errno = info32.si_errno;
		kinfo.si_pid = info32.si_pid;
		kinfo.si_uid = info32.si_uid;
		kinfo.si_value.sival_ptr =
		    (void *)(uintptr_t)(uint32_t)info32.si_value.sival_ptr;
	} else
#endif
	if (copyin(uinfo, &kinfo, sizeof (kinfo)) != 0) {
		return (set_errno(EFAULT));
	}
	return (lx_rt_tgsigqueueinfo_common(tgid, tid, sig, &kinfo));
}

long
lx_rt_tgsigqueueinfo(pid_t tgid, pid_t tid, int lx_sig, lx_siginfo_t *uinfo)
{
	lx_siginfo_t linfo;
	siginfo_t kinfo;
	int sig, code;

	if (copyin(uinfo, &linfo, sizeof (linfo)) != 0)
		return (set_errno(EFAULT));
	if (lx_sig < 0 || lx_sig > LX_NSIG ||
	    (sig = ltos_signo[lx_sig]) < 0)
		sig = -1;
	if (linfo.lsi_code >= 0) {
		code = linfo.lsi_code;
	} else {
		switch (linfo.lsi_code) {
		case LX_SI_TKILL:
			code = SI_LWP;
			break;
		case LX_SI_QUEUE:
			code = SI_QUEUE;
			break;
		case LX_SI_TIMER:
			code = SI_TIMER;
			break;
		case LX_SI_ASYNCIO:
			code = SI_ASYNCIO;
			break;
		case LX_SI_MESGQ:
			code = SI_MESGQ;
			break;
		default:
			return (set_errno(EINVAL));
		}
	}
	bzero(&kinfo, sizeof (kinfo));
	kinfo.si_signo = sig;
	kinfo.si_errno = linfo.lsi_errno;
	kinfo.si_code = code;
	kinfo.si_pid = linfo.lsi_pid;
	kinfo.si_uid = linfo.lsi_uid;
	kinfo.si_value = linfo.lsi_value;
	return (lx_rt_tgsigqueueinfo_common(tgid, tid, sig, &kinfo));
}
