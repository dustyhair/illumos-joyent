/*
 * CDDL version 1.0, http://www.illumos.org/license/CDDL.
 */
#ifndef _SYS_LX_KSIGNAL_H
#define	_SYS_LX_KSIGNAL_H

#include <sys/types.h>

#define	LXK_NSIG 64

#define	LXK_SA_NOCLDSTOP 0x00000001ULL
#define	LXK_SA_NOCLDWAIT 0x00000002ULL
#define	LXK_SA_SIGINFO 0x00000004ULL
#define	LXK_SA_RESTORER 0x04000000ULL
#define	LXK_SA_ONSTACK 0x08000000ULL
#define	LXK_SA_RESTART 0x10000000ULL
#define	LXK_SA_NODEFER 0x40000000ULL
#define	LXK_SA_RESETHAND 0x80000000ULL
#define	LXK_SS_ONSTACK 1
#define	LXK_SS_DISABLE 2
#define	LXK_NREG 18
#define	LXK_RAX 13
#define	LXK_RSP 15
#define	LXK_RIP 16
#define	LXK_RFL 17

typedef struct lx_ksigaction {
	uint64_t handler;
	uint64_t flags;
	uint64_t restorer;
	uint64_t mask;
} lx_ksigaction_t;

typedef struct lx_kstack {
	uint64_t sp;
	uint32_t flags;
	uint32_t pad;
	uint64_t size;
} lx_kstack_t;

typedef struct lx_ksigcontext {
	uint64_t g[LXK_NREG];
	uint16_t cs, gs, fs, ss;
	uint64_t err, trapno, mask, cr2, fpstate;
	uint64_t reserved[8];
} lx_ksigcontext_t;

typedef struct lx_kucontext {
	uint64_t flags, link;
	lx_kstack_t stack;
	lx_ksigcontext_t context;
	uint64_t mask;
} lx_kucontext_t;

typedef struct lx_ksiginfo {
	int32_t signo, error, code, pad;
	uint8_t data[112];
} lx_ksiginfo_t;

typedef struct lx_ksigframe {
	uint64_t restorer;
	lx_kucontext_t uc;
	lx_ksiginfo_t info;
	uint8_t fpstate[512];
	uint64_t pad;
} lx_ksigframe_t;

#endif /* _SYS_LX_KSIGNAL_H */
