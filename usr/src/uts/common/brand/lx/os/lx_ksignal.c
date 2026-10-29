/*
 * CDDL version 1.0, http://www.illumos.org/license/CDDL.
 */
/* Direct amd64 Linux signal delivery for the default-off lx experiment. */
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/brand.h>
#include <sys/archsystm.h>
#include <sys/kmem.h>
#include <sys/errno.h>
#include <sys/user.h>
#include <sys/schedctl.h>
#include <sys/proc.h>
#include <sys/signal.h>
#include <sys/siginfo.h>
#include <sys/ucontext.h>
#include <sys/privregs.h>
#include <sys/psw.h>
#include <sys/segments.h>
#include <sys/fp.h>
#include <sys/debug.h>
#include <sys/stddef.h>
#include <sys/lx_brand.h>
#include <sys/lx_impl.h>
#include <sys/lx_signal.h>
#include <sys/lx_ksignal.h>
#include <vm/as.h>

#define	LXK_MAXDEPTH 32
#define	LXK_SUPPORTED_FLAGS (LXK_SA_NOCLDSTOP | LXK_SA_NOCLDWAIT | \
    LXK_SA_SIGINFO | LXK_SA_RESTORER | LXK_SA_ONSTACK | LXK_SA_RESTART | \
    LXK_SA_NODEFER | LXK_SA_RESETHAND)

CTASSERT(sizeof (lx_ksigaction_t) == 32);
CTASSERT(sizeof (lx_kstack_t) == 24);
CTASSERT(sizeof (lx_ksigcontext_t) == 256);
CTASSERT(sizeof (lx_kucontext_t) == 304);
CTASSERT(sizeof (lx_ksiginfo_t) == 128);
CTASSERT(offsetof(lx_ksigframe_t, fpstate) == 440);
CTASSERT(sizeof (lx_ksigframe_t) == 960);

struct lx_sigrecord {
	struct lx_sigrecord *prev;
	ucontext_t uc;
	uintptr_t frame;
	uint_t depth;
	void *xregs;
	size_t xsize;
};

static const int lxk_regs[LXK_NREG] = {
	REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15,
	REG_RDI, REG_RSI, REG_RBP, REG_RBX, REG_RDX, REG_RAX, REG_RCX,
	REG_RSP, REG_RIP, REG_RFL
};

static void
lxk_free_record(struct lx_sigrecord *r)
{
	if (r->xregs != NULL)
		kmem_free(r->xregs, r->xsize);
	kmem_free(r, sizeof (*r));
}

void
lx_sigframes_free(lx_lwp_data_t *ld)
{
	struct lx_sigrecord *r = ld->br_sigframes;

	ld->br_sigframes = NULL;
	while (r != NULL) {
		struct lx_sigrecord *prev = r->prev;
		lxk_free_record(r);
		r = prev;
	}
}

/* Called during allocation, before lwp_create takes p_lock. */
void
lx_sigframes_clone(lx_lwp_data_t *dst, const lx_lwp_data_t *src)
{
	struct lx_sigrecord **out = &dst->br_sigframes;
	const struct lx_sigrecord *r;

	for (r = src->br_sigframes; r != NULL; r = r->prev) {
		struct lx_sigrecord *copy;

		copy = kmem_alloc(sizeof (*copy), KM_SLEEP);
		*copy = *r;
		copy->prev = NULL;
		if (r->xregs != NULL) {
			copy->xregs = kmem_alloc(r->xsize, KM_SLEEP);
			bcopy(r->xregs, copy->xregs, r->xsize);
		}
		*out = copy;
		out = &copy->prev;
	}
}

static void
lxk_setmask(uint64_t bits, k_sigset_t *set)
{
	lx_sigset_t ls;

	bcopy(&bits, &ls, sizeof (ls));
	lx_ltos_sigset(&ls, set);
}

static uint64_t
lxk_getmask(k_sigset_t *set)
{
	lx_sigset_t ls;
	uint64_t bits;

	lx_stol_sigset(set, &ls);
	bcopy(&ls, &bits, sizeof (bits));
	return (bits);
}

static boolean_t
lxk_onstack(uintptr_t sp, const stack_t *ss)
{
	return ((ss->ss_flags & SS_DISABLE) == 0 &&
	    sp >= (uintptr_t)ss->ss_sp &&
	    sp - (uintptr_t)ss->ss_sp < ss->ss_size);
}

static void
lxk_stack_out(klwp_t *lwp, uintptr_t sp, lx_kstack_t *out)
{
	stack_t *ss = &lwp->lwp_sigaltstack;

	out->sp = (uintptr_t)ss->ss_sp;
	out->size = ss->ss_size;
	out->flags = ss->ss_flags & SS_DISABLE;
	if (lxk_onstack(sp, ss))
		out->flags |= LXK_SS_ONSTACK;
	out->pad = 0;
}

static int
lxk_stack_in(const lx_kstack_t *in, stack_t *out, boolean_t returning)
{
	uintptr_t limit = (uintptr_t)curproc->p_as->a_userlimit;
	uint_t flags = in->flags;

	if ((flags & ~(LXK_SS_ONSTACK | LXK_SS_DISABLE)) != 0 ||
	    (!returning && (flags & LXK_SS_ONSTACK) != 0) ||
	    flags == (LXK_SS_ONSTACK | LXK_SS_DISABLE))
		return (EINVAL);
	bzero(out, sizeof (*out));
	if (flags & LXK_SS_DISABLE) {
		out->ss_flags = SS_DISABLE;
		return (0);
	}
	if (in->size < 2048)
		return (ENOMEM);
	if (in->sp >= limit || in->size > limit - in->sp)
		return (EINVAL);
	out->ss_sp = (void *)(uintptr_t)in->sp;
	out->ss_size = in->size;
	out->ss_flags = flags & LXK_SS_ONSTACK;
	return (0);
}

static long
lxk_sigaction(uintptr_t sig, uintptr_t ap, uintptr_t op, uintptr_t size)
{
	proc_t *p = curproc;
	lx_proc_data_t *pd = ptolxproc(p);
	lx_ksigaction_t act, old;
	k_sigset_t mask;
	int native, flags = SA_SIGINFO;
	uint64_t bit;

	if (size != 8 || sig == 0 || sig > LX_NSIG ||
	    (ap != 0 && (sig == LX_SIGKILL || sig == LX_SIGSTOP)))
		return (set_errno(EINVAL));
	native = ltos_signo[sig];
	if (native <= 0)
		return (set_errno(EOPNOTSUPP));
	bit = 1ULL << (sig - 1);
	if (ap != 0) {
		if (copyin((void *)ap, &act, sizeof (act)) != 0)
			return (set_errno(EFAULT));
		act.flags &= 0xffffffffULL;
		if ((act.flags & ~LXK_SUPPORTED_FLAGS) != 0)
			return (set_errno(EINVAL));
		lxk_setmask(act.mask, &mask);
		if (act.flags & LXK_SA_NOCLDSTOP) flags |= SA_NOCLDSTOP;
		if (act.flags & LXK_SA_NOCLDWAIT) flags |= SA_NOCLDWAIT;
		if (act.flags & LXK_SA_RESTART) flags |= SA_RESTART;
		if (act.flags & LXK_SA_NODEFER) flags |= SA_NODEFER;
		if (act.flags & LXK_SA_RESETHAND) flags |= SA_RESETHAND;
	}
	mutex_enter(&p->p_lock);
	if (pd->l_sigvalid & bit) {
		old = pd->l_sigacts[sig];
	} else {
		if (op != 0 && PTOU(p)->u_signal[native - 1] != SIG_DFL &&
		    PTOU(p)->u_signal[native - 1] != SIG_IGN) {
			mutex_exit(&p->p_lock);
			return (set_errno(EOPNOTSUPP));
		}
		bzero(&old, sizeof (old));
		if (PTOU(p)->u_signal[native - 1] == SIG_IGN)
			old.handler = 1;
	}
	if (ap != 0) {
		pd->l_sigacts[sig] = act;
		pd->l_sigvalid |= bit;
		setsigact(native, (void (*)(int))(uintptr_t)
		    ((sig == LX_SIGPWR && act.handler == 0) ? 2 : act.handler),
		    &mask, flags);
	}
	mutex_exit(&p->p_lock);
	if (native == SIGCLD && ap != 0)
		sigcld_repost();
	if (op != 0 && copyout(&old, (void *)op, sizeof (old)) != 0)
		return (set_errno(EFAULT));
	return (0);
}

static long
lxk_sigprocmask(uintptr_t how, uintptr_t ap, uintptr_t op, uintptr_t size)
{
	uint64_t input, old;
	k_sigset_t set;
	kthread_t *t = curthread;

	if (size != 8)
		return (set_errno(EINVAL));
	if (ap != 0) {
		if (how > 2)
			return (set_errno(EINVAL));
		if (copyin((void *)ap, &input, sizeof (input)) != 0)
			return (set_errno(EFAULT));
		lxk_setmask(input, &set);
	}
	schedctl_finish_sigblock(t);
	old = lxk_getmask(&t->t_hold);
	if (ap != 0) {
		if (how == 0) sigorset(&t->t_hold, &set);
		else if (how == 1) sigdiffset(&t->t_hold, &set);
		else t->t_hold = set;
		t->t_sig_check = 1;
	}
	if (op != 0 && copyout(&old, (void *)op, sizeof (old)) != 0)
		return (set_errno(EFAULT));
	return (0);
}

static long
lxk_sigaltstack(uintptr_t ap, uintptr_t op)
{
	klwp_t *lwp = ttolwp(curthread);
	lx_kstack_t input, old;
	stack_t ss;
	int error;

	if (ap != 0) {
		if (copyin((void *)ap, &input, sizeof (input)) != 0)
			return (set_errno(EFAULT));
		if ((error = lxk_stack_in(&input, &ss, B_FALSE)) != 0)
			return (set_errno(error));
		if (lxk_onstack(lwptoregs(lwp)->r_sp, &lwp->lwp_sigaltstack))
			return (set_errno(EPERM));
	}
	lxk_stack_out(lwp, lwptoregs(lwp)->r_sp, &old);
	if (ap != 0)
		lwp->lwp_sigaltstack = ss;
	if (op != 0 && copyout(&old, (void *)op, sizeof (old)) != 0)
		return (set_errno(EFAULT));
	return (0);
}

/* -1 delegates to the existing native signal bridge; 0 is a bad frame. */
int
lx_sigdeliver_kernel(int sig, k_siginfo_t *sip, void (*handler)())
{
	proc_t *p = curproc;
	klwp_t *lwp = ttolwp(curthread);
	lx_lwp_data_t *ld = lwptolxlwp(lwp);
	lx_proc_data_t *pd = ptolxproc(p);
	struct regs *rp = lwptoregs(lwp);
	lx_ksigaction_t act;
	lx_ksigframe_t *frame;
	struct lx_sigrecord *r;
	uintptr_t top, address, limit = (uintptr_t)p->p_as->a_userlimit;
	uint_t i;
	int ls = stol_signo[sig];
	boolean_t alt;

	if (lwp_getdatamodel(lwp) != DATAMODEL_NATIVE || ls <= 0)
		return (-1);
	mutex_enter(&p->p_lock);
	if ((pd->l_sigvalid & (1ULL << (ls - 1))) == 0) {
		mutex_exit(&p->p_lock);
		return (-1);
	}
	act = pd->l_sigacts[ls];
	if (act.flags & LXK_SA_RESETHAND)
		pd->l_sigacts[ls].handler = 0;
	mutex_exit(&p->p_lock);
	if (ls == LX_SIGPWR && act.handler == 0 && (uintptr_t)handler == 2) {
		exit(CLD_KILLED, SIGPWR);
		return (1);
	}
	if (act.handler <= 1 || act.handler >= limit ||
	    act.handler != (uintptr_t)handler ||
	    ld->br_stack_mode != LX_STACK_MODE_BRAND ||
	    (ld->br_sigframes != NULL &&
	    ld->br_sigframes->depth >= LXK_MAXDEPTH))
		return (0);

	alt = (act.flags & LXK_SA_ONSTACK) != 0 &&
	    (lwp->lwp_sigaltstack.ss_flags & SS_DISABLE) == 0 &&
	    !lxk_onstack(rp->r_sp, &lwp->lwp_sigaltstack);
	if (alt) {
		top = (uintptr_t)lwp->lwp_sigaltstack.ss_sp +
		    lwp->lwp_sigaltstack.ss_size;
	} else {
		if ((uintptr_t)rp->r_sp < 128)
			return (0);
		top = rp->r_sp - 128;
	}
	if (top >= limit || top < sizeof (*frame) + 16)
		return (0);
	address = ((top - sizeof (*frame)) & ~(uintptr_t)15) - 8;
	if ((alt || lxk_onstack(rp->r_sp, &lwp->lwp_sigaltstack)) &&
	    (address < (uintptr_t)lwp->lwp_sigaltstack.ss_sp ||
	    address + sizeof (*frame) > (uintptr_t)lwp->lwp_sigaltstack.ss_sp +
	    lwp->lwp_sigaltstack.ss_size))
		return (0);
	r = kmem_zalloc(sizeof (*r), KM_SLEEP);
	if (savecontext(&r->uc, &lwp->lwp_sigoldmask, SAVECTXT_F_NONE) != 0) {
		lxk_free_record(r);
		return (0);
	}
	/* Preserve initial FP state even if the handler is its first user. */
	getfpregs(lwp, &r->uc.uc_mcontext.fpregs);
	r->uc.uc_flags |= UC_FPU;
	if (fpu_xsave_enabled()) {
		r->xsize = fpu_proc_xregs_max_size();
		r->xregs = kmem_alloc(r->xsize, KM_SLEEP);
		fpu_proc_xregs_get(lwp, r->xregs);
	}
	frame = kmem_zalloc(sizeof (*frame), KM_SLEEP);
	frame->restorer = (act.flags & LXK_SA_RESTORER) ? act.restorer : 0;
	lxk_stack_out(lwp, rp->r_sp, &frame->uc.stack);
	frame->uc.mask = lxk_getmask(&lwp->lwp_sigoldmask);
	for (i = 0; i < LXK_NREG; i++)
		frame->uc.context.g[i] = r->uc.uc_mcontext.gregs[lxk_regs[i]];
	frame->uc.context.cs = UCS_SEL;
	frame->uc.context.ss = UDS_SEL;
	frame->uc.context.err = r->uc.uc_mcontext.gregs[REG_ERR];
	frame->uc.context.trapno = r->uc.uc_mcontext.gregs[REG_TRAPNO];
	if ((act.flags & LXK_SA_RESTART) != 0 &&
	    ((uintptr_t)r->uc.uc_brand_data[0] & LX_UC_RESTART_SYSCALL) != 0 &&
	    (long)frame->uc.context.g[LXK_RAX] == -4 &&
	    frame->uc.context.g[LXK_RIP] >= 2) {
		frame->uc.context.g[LXK_RIP] -= 2;
		frame->uc.context.g[LXK_RAX] =
		    (uintptr_t)r->uc.uc_brand_data[2];
	}
	if (r->uc.uc_flags & UC_FPU) {
		bcopy(&r->uc.uc_mcontext.fpregs, frame->fpstate, 512);
		frame->uc.context.fpstate = address +
		    offsetof(lx_ksigframe_t, fpstate);
	}
	frame->info.signo = ls;
	if (sip != NULL) {
		frame->info.code = lx_stol_sigcode(sip->si_code);
		if (ls == LX_SIGILL || ls == LX_SIGBUS || ls == LX_SIGFPE ||
		    ls == LX_SIGSEGV) {
			uint64_t fault = (uintptr_t)sip->si_addr;
			bcopy(&fault, frame->info.data, sizeof (fault));
			frame->uc.context.cr2 = ls == LX_SIGSEGV ? fault : 0;
		} else {
			int32_t pid = sip->si_pid;
			uint32_t uid = sip->si_uid;
			bcopy(&pid, frame->info.data, sizeof (pid));
			bcopy(&uid, frame->info.data + 4, sizeof (uid));
			if (sip->si_code == SI_QUEUE ||
			    sip->si_code == SI_TIMER) {
				uint64_t value =
				    (uintptr_t)sip->si_value.sival_ptr;
				bcopy(&value, frame->info.data + 8,
				    sizeof (value));
			}
			if (ls == LX_SIGCHLD) {
				int32_t status = sip->si_status;
				uint64_t utime =
				    (uint64_t)sip->si_utime * 100 / hz;
				uint64_t stime =
				    (uint64_t)sip->si_stime * 100 / hz;
				if (sip->si_code != CLD_EXITED && status > 0 &&
				    status < NSIG)
					status = stol_signo[status];
				bcopy(&status, frame->info.data + 8,
				    sizeof (status));
				bcopy(&utime, frame->info.data + 16,
				    sizeof (utime));
				bcopy(&stime, frame->info.data + 24,
				    sizeof (stime));
			}
		}
	}
	if (copyout_nowatch(frame, (void *)address, sizeof (*frame)) != 0) {
		kmem_free(frame, sizeof (*frame));
		lxk_free_record(r);
		return (0);
	}
	kmem_free(frame, sizeof (*frame));
	r->frame = address;
	r->depth = ld->br_sigframes == NULL ? 1 : ld->br_sigframes->depth + 1;
	r->prev = ld->br_sigframes;
	ld->br_sigframes = r;
	if (lxk_onstack(address, &lwp->lwp_sigaltstack))
		lwp->lwp_sigaltstack.ss_flags |= SS_ONSTACK;
	rp->r_pc = act.handler;
	rp->r_sp = address;
	rp->r_rdi = ls;
	rp->r_rsi = address + offsetof(lx_ksigframe_t, info);
	rp->r_rdx = address + offsetof(lx_ksigframe_t, uc);
	rp->r_rax = 0;
	rp->r_cs = UCS_SEL;
	rp->r_ss = UDS_SEL;
	rp->r_ps = PSL_USER;
	return (1);
}

static long
lxk_sigreturn(klwp_t *lwp)
{
	lx_lwp_data_t *ld = lwptolxlwp(lwp);
	struct lx_sigrecord *r = ld->br_sigframes;
	struct regs *rp = lwptoregs(lwp);
	lx_kucontext_t uc;
	stack_t stack;
	k_sigset_t mask;
	uintptr_t limit = (uintptr_t)curproc->p_as->a_userlimit;
	uint_t i;

	if (r == NULL || (uintptr_t)rp->r_sp < 8 ||
	    (uintptr_t)rp->r_sp - 8 != r->frame ||
	    copyin((void *)(r->frame + 8), &uc, sizeof (uc)) != 0)
		goto bad;
	if (uc.context.cs != UCS_SEL || uc.context.ss != UDS_SEL ||
	    uc.context.fs != 0 || uc.context.gs != 0 ||
	    uc.context.g[LXK_RIP] >= limit ||
	    uc.context.g[LXK_RSP] >= limit ||
	    (uc.context.g[LXK_RFL] & (PS_IOPL | PS_NT | PS_VM)) != 0 ||
	    lxk_stack_in(&uc.stack, &stack, B_TRUE) != 0)
		goto bad;
	if (uc.context.fpstate != 0) {
		uint8_t fp[512];
		uint32_t mxcsr;

		if (copyin((void *)(uintptr_t)uc.context.fpstate, fp,
		    sizeof (fp)) != 0)
			goto bad;
		bcopy(fp + 24, &mxcsr, sizeof (mxcsr));
		if ((mxcsr & ~sse_mxcsr_mask) != 0)
			goto bad;
		bcopy(fp, &r->uc.uc_mcontext.fpregs, sizeof (fp));
		r->uc.uc_flags |= UC_FPU;
	}
	for (i = 0; i < LXK_NREG; i++)
		r->uc.uc_mcontext.gregs[lxk_regs[i]] = uc.context.g[i];
	r->uc.uc_mcontext.gregs[REG_CS] = UCS_SEL;
	r->uc.uc_mcontext.gregs[REG_SS] = UDS_SEL;
	lxk_setmask(uc.mask, &mask);
	sigktou(&mask, &r->uc.uc_sigmask);
	/* User flags/link/stack-mode fields never become a native ucontext. */
	r->uc.uc_flags &= ~(UC_STACK | UC_XSAVE);
	r->uc.uc_flags |= UC_CPU | UC_SIGMASK;
	r->uc.uc_brand_data[0] = (void *)
	    (((uintptr_t)r->uc.uc_brand_data[0] & ~LX_UC_STACK_NATIVE) |
	    LX_UC_STACK_BRAND);
	if (r->xregs != NULL && fpu_proc_xregs_set(lwp, r->xregs) != 0)
		goto bad;
	/* Emit the same ptrace syscall-exit stop as the native bridge. */
	rp->r_r0 = 0;
	(void) lx_ptrace_stop(LX_PR_SYSEXIT);
	ld->br_sigframes = r->prev;
	lwp->lwp_sigaltstack = stack;
	restorecontext(&r->uc);
	lxk_free_record(r);
	ld->br_eosys = JUSTRETURN;
	return (0);
bad:
	exit(CLD_KILLED, SIGSEGV);
	return (0);
}

boolean_t
lx_sig_kernel_syscall(klwp_t *lwp, int nr, uintptr_t *args, long *result)
{
	lx_proc_data_t *pd = ptolxproc(curproc);

	if (lwp_getdatamodel(lwp) != DATAMODEL_NATIVE ||
	    (!lx_seccomp_experimental && pd->l_sigvalid == 0 &&
	    lwptolxlwp(lwp)->br_sigframes == NULL &&
	    !lx_seccomp_active(curproc)))
		return (B_FALSE);
	switch (nr) {
	case 13:
		if (args[0] > 0 && args[0] <= LX_NSIG &&
		    ltos_signo[args[0]] < 0 && !lx_seccomp_active(curproc))
			return (B_FALSE);
		*result = lxk_sigaction(args[0], args[1], args[2], args[3]);
		break;
	case 14:
		*result = lxk_sigprocmask(args[0], args[1], args[2], args[3]);
		break;
	case 15:
		*result = lxk_sigreturn(lwp);
		break;
	case 131:
		*result = lxk_sigaltstack(args[0], args[1]);
		break;
	default:
		return (B_FALSE);
	}
	return (B_TRUE);
}
