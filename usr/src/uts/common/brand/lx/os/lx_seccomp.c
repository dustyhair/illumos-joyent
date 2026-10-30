/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * A copy is available at http://www.illumos.org/license/CDDL.
 */
/*
 * EXPERIMENTAL: a process-wide, kernel-emulation-only enforcement path.
 * Native execution and alternate syscall entries terminate the process.
 * This is deliberately disabled until explicitly enabled by the lab host.
 */
#include <sys/types.h>
#include <sys/atomic.h>
#include <sys/brand.h>
#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/privregs.h>
#include <sys/machbrand.h>
#include <sys/lx_brand.h>
#include <sys/lx_impl.h>
#include <lx_seccomp_bpf.h>

#define	SC_ALLOW 0x7fff0000U
#define	SC_ERRNO 0x00050000U
#define	SC_KILL_PROCESS 0x80000000U
#define	SC_ACTION 0xffff0000U
#define	SC_MAXPATH 32768U
#define	SC_MAXDEPTH 64U

/* /etc/system: set lx_brand:lx_seccomp_experimental = 1 (lab only). */
int lx_seccomp_experimental = 0;

struct lx_seccomp_filter {
	uint_t sc_refs;
	uint32_t sc_len;
	uint32_t sc_total;
	uint32_t sc_depth;
	lx_scbpf_insn_t *sc_prog;
	struct lx_seccomp_filter *sc_prev;
};

void
lx_seccomp_hold(struct lx_seccomp_filter *f)
{
	if (f != NULL)
		atomic_inc_uint(&f->sc_refs);
}

void
lx_seccomp_rele(struct lx_seccomp_filter *f)
{
	while (f != NULL && atomic_dec_uint_nv(&f->sc_refs) == 0) {
		struct lx_seccomp_filter *prev = f->sc_prev;

		kmem_free(f->sc_prog, f->sc_len * sizeof (*f->sc_prog));
		kmem_free(f, sizeof (*f));
		f = prev;
	}
}

boolean_t
lx_seccomp_active(proc_t *p)
{
	lx_proc_data_t *pd = ptolxproc(p);

	return (pd != NULL && pd->l_seccomp != NULL);
}

boolean_t
lx_kernel_restricted(proc_t *p)
{
	lx_proc_data_t *pd = ptolxproc(p);

	return (pd != NULL && (pd->l_seccomp != NULL || pd->l_mntns != NULL ||
	    (pd->l_flags & LX_PROC_USERNS) != 0));
}

/* Called before any syscall entry can fall back to native dispatch. */
void
lx_seccomp_enter(klwp_t *lwp, int entry)
{
	if (lx_kernel_restricted(lwptoproc(lwp)) &&
	    (entry != BRAND_ENTRY_AMD64 ||
	    lwptolxlwp(lwp)->br_stack_mode != LX_STACK_MODE_BRAND))
		exit(CLD_KILLED, SIGSYS);
}

long
lx_seccomp_install(uintptr_t address)
{
	struct {
		uint16_t len;
		uint16_t pad[3];
		uint64_t filter;
	} fp;
	proc_t *p = curproc;
	klwp_t *lwp = ttolwp(curthread);
	lx_proc_data_t *pd = ptolxproc(p);
	struct lx_seccomp_filter *f, *old;
	uint32_t *flow, i;
	int error = 0;

	if (address == 0)
		return (set_errno(EFAULT));
	if (!lx_seccomp_experimental ||
	    lwp_getdatamodel(lwp) != DATAMODEL_NATIVE ||
	    lwptolxlwp(lwp)->br_syscall_entry != BRAND_ENTRY_AMD64)
		return (set_errno(EINVAL));
	if (copyin((void *)address, &fp, sizeof (fp)) != 0)
		return (set_errno(EFAULT));
	if (fp.len == 0 || fp.len > LX_SCBPF_MAXINSNS)
		return (set_errno(EINVAL));
	if (!lwp->lwp_no_new_privs)
		return (set_errno(EACCES));

	f = kmem_zalloc(sizeof (*f), KM_SLEEP);
	f->sc_refs = 1;
	f->sc_len = fp.len;
	f->sc_prog = kmem_alloc(fp.len * sizeof (*f->sc_prog), KM_SLEEP);
	if (copyin((void *)(uintptr_t)fp.filter, f->sc_prog,
	    fp.len * sizeof (*f->sc_prog)) != 0) {
		error = EFAULT;
		goto fail;
	}
	flow = kmem_alloc(fp.len * sizeof (*flow), KM_SLEEP);
	if (!lx_seccomp_bpf_validate(f->sc_prog, fp.len, flow))
		error = EINVAL;
	kmem_free(flow, fp.len * sizeof (*flow));
	if (error != 0)
		goto fail;

	/* Other actions and computed returns need a separate implementation. */
	for (i = 0; i < fp.len; i++) {
		uint32_t action = f->sc_prog[i].k & SC_ACTION;

		if (f->sc_prog[i].code == 0x16 ||
		    (f->sc_prog[i].code == 0x06 && action != SC_ALLOW &&
		    action != SC_ERRNO && action != 0 &&
		    action != SC_KILL_PROCESS)) {
			error = EINVAL;
			goto fail;
		}
	}

	/*
	 * The experimental guard is process-wide. Installation requires one
	 * LWP; later LWPs see the same guard. Restricted kernel paths handle
	 * fork, exec and pthread clones without native usermode emulation.
	 */
	mutex_enter(&p->p_lock);
	old = pd->l_seccomp;
	if (p->p_lwpcnt != 1) {
		error = EOPNOTSUPP;
	} else if (old != NULL && (old->sc_depth >= SC_MAXDEPTH ||
	    old->sc_total + fp.len + 4 > SC_MAXPATH)) {
		error = ENOMEM;
	} else {
		f->sc_prev = old; /* Transfer the proc's old reference. */
		f->sc_depth = old == NULL ? 1 : old->sc_depth + 1;
		f->sc_total = fp.len + (old == NULL ? 0 : old->sc_total + 4);
		pd->l_seccomp = f;
	}
	mutex_exit(&p->p_lock);
	if (error != 0)
		goto fail;
	return (0);
fail:
	lx_seccomp_rele(f);
	return (set_errno(error));
}

long
lx_seccomp(uintptr_t operation, uintptr_t flags, uintptr_t address)
{
	if (operation != 1 || flags != 0)
		return (set_errno(EINVAL));
	return (lx_seccomp_install(address));
}

/* Return false if a filtered Linux errno result has already been returned. */
boolean_t
lx_seccomp_check(klwp_t *lwp, int nr)
{
	proc_t *p = lwptoproc(lwp);
	lx_proc_data_t *pd = ptolxproc(p);
	struct lx_seccomp_filter *head, *f;
	struct regs *rp = lwptoregs(lwp);
	lx_seccomp_data_t data;
	uint32_t result = SC_ALLOW;
	uintptr_t args[6];
	uint_t i;

	if (pd->l_seccomp == NULL)
		return (B_TRUE);
	mutex_enter(&p->p_lock);
	head = pd->l_seccomp;
	lx_seccomp_hold(head);
	mutex_exit(&p->p_lock);

	data.nr = nr;
	data.arch = 0xc000003eU;
	data.instruction_pointer = rp->r_pc;
	data.args[0] = rp->r_rdi;
	data.args[1] = rp->r_rsi;
	data.args[2] = rp->r_rdx;
	data.args[3] = rp->r_r10;
	data.args[4] = rp->r_r8;
	data.args[5] = rp->r_r9;
	for (f = head; f != NULL; f = f->sc_prev) {
		uint32_t value = lx_seccomp_bpf_eval(f->sc_prog, f->sc_len,
		    &data);
		uint32_t action = value & SC_ACTION;

		if (action != SC_ALLOW && action != SC_ERRNO) {
			lx_seccomp_rele(head);
				/* Both KILL actions terminate the process. */
			exit(CLD_KILLED, SIGSYS);
			return (B_FALSE);
		}
		if (action == SC_ERRNO && result == SC_ALLOW)
			result = value; /* Newest ERRNO wins a tie. */
	}
	lx_seccomp_rele(head);
	if (result == SC_ALLOW)
		return (B_TRUE);

	for (i = 0; i < 6; i++)
		args[i] = data.args[i];
	for (i = 0; i < 4; i++)
		lwptolxlwp(lwp)->br_syscall_args[i] = args[i];
	lx_trace_sysenter(nr, args);
	result &= 0xffffU;
	if (result > 4095)
		result = 4095;
	/* This is already a Linux errno; do not translate it again. */
	lwp->lwp_errno = 0;
	lx_syscall_return(lwp, nr, -(long)result);
	return (B_FALSE);
}
