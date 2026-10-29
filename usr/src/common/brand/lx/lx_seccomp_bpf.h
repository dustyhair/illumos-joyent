/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * A copy is available at http://www.illumos.org/license/CDDL.
 */
#ifndef _LX_SECCOMP_BPF_H
#define	_LX_SECCOMP_BPF_H

#ifdef _KERNEL
#include <sys/types.h>
#else
#include <stdint.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define	LX_SCBPF_MAXINSNS	4096
#define	LX_SCBPF_MEMWORDS	16
#define	LX_SCBPF_DATASIZE	64

/* Linux classic BPF instruction ABI; not a packet-filter load ABI. */
typedef struct lx_scbpf_insn {
	uint16_t code;
	uint8_t jt;
	uint8_t jf;
	uint32_t k;
} lx_scbpf_insn_t;

/* x86 Linux seccomp_data ABI, shared by i386 and amd64. */
typedef struct lx_seccomp_data {
	int32_t nr;
	uint32_t arch;
	uint64_t instruction_pointer;
	uint64_t args[6];
} lx_seccomp_data_t;

/*
 * Returns 1 for valid seccomp cBPF and 0 otherwise.  The caller supplies
 * ninsns workspace words, distinct from the immutable instruction array.
 * Kernel callers must allocate this workspace off-stack.  All control-flow
 * paths are forward-only and scratch reads require definite initialization.
 */
extern int lx_seccomp_bpf_validate(const lx_scbpf_insn_t *, uint32_t,
    uint32_t *);

/*
 * Evaluate a previously verified, immutable program over copied metadata.
 * Returns the raw action/data word, without interpreting seccomp actions.
 * Defensive failures return 0, the Linux KILL_THREAD action.  Allocation,
 * filter lifetime, syscall copyin and enforcement are the caller's concern.
 */
extern uint32_t lx_seccomp_bpf_eval(const lx_scbpf_insn_t *, uint32_t,
    const lx_seccomp_data_t *);

#ifdef __cplusplus
}
#endif
#endif /* _LX_SECCOMP_BPF_H */
