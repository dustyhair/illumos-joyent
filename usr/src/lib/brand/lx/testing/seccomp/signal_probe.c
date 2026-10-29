/*
 * CDDL version 1.0, http://www.illumos.org/license/CDDL.
 */
#define	_GNU_SOURCE
#include <errno.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/ucontext.h>
#include <sys/wait.h>
#include <unistd.h>

#if !defined(__x86_64__)
#error This signal probe requires Linux x86-64
#endif

static volatile sig_atomic_t hits, inner_hits, problem, kind;
static pid_t self_pid, self_tid;
static uid_t self_uid;
static pid_t expected_sender;
static int ack_fd;
static uintptr_t saved_frame;
static volatile uintptr_t fault_resume;
static unsigned char altstack_mem[65536] __attribute__((aligned(16)));
static int is_lx, skipped;

enum {
	BASIC, MASK, PERSIST_MASK, NESTED, NODEFER, ALTSTACK, CHANGE_RAX,
	POISON_FLAGS, BAD_CS, BAD_SS, BAD_IOPL, BAD_IP, BAD_SP, BAD_FP,
	BAD_MXCSR, NATIVE_ESCAPE, CLOBBER_SSE, CLOBBER_AVX, WAIT_READ
};

static long
raw3(long nr, long a, long b, long c)
{
	long result;
	__asm__ volatile("syscall" : "=a" (result) : "0" (nr),
	    "D" (a), "S" (b), "d" (c) : "rcx", "r11", "memory", "cc");
	return (result);
}

static long
send_self(int sig)
{
	return (raw3(SYS_tgkill, self_pid, self_tid, sig));
}

static int
deny_rule(int nr, uint32_t action)
{
	struct sock_filter p[] = {
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (uint32_t)nr, 0, 1),
		BPF_STMT(BPF_RET | BPF_K, action),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)
	};
	struct sock_fprog fp = { .len = 4, .filter = p };

	if (prctl(PR_SET_NO_NEW_PRIVS, 1UL, 0UL, 0UL, 0UL) != 0)
		return (-1);
	return (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &fp));
}

static int
filter(void)
{
	return (deny_rule(SYS_getppid, SECCOMP_RET_ERRNO | EACCES));
}

static __attribute__((target("avx"), noinline)) void
clobber_avx(void)
{
	__asm__ volatile("vpxor %%ymm7, %%ymm7, %%ymm7" : : : "ymm7");
}

static void
handler(int sig, siginfo_t *info, void *context)
{
	ucontext_t *uc = context;
	sigset_t mask;
	int local;

	if (sig == SIGUSR2) {
		inner_hits++;
		if (raw3(SYS_getppid, 0, 0, 0) != -EACCES)
			problem = 1;
		return;
	}
	hits++;
	if (kind == CLOBBER_SSE)
		__asm__ volatile("pxor %%xmm7, %%xmm7" : : : "xmm7");
	if (kind == CLOBBER_AVX)
		clobber_avx();
	if (sig != SIGUSR1 || info == NULL || uc == NULL ||
	    info->si_signo != sig || info->si_pid != expected_sender ||
	    info->si_uid != self_uid)
		problem = 2;
	if (raw3(SYS_getppid, 0, 0, 0) != -EACCES)
		problem = 3;
	saved_frame = (uintptr_t)uc - 8;
	switch (kind) {
	case MASK:
		if (sigprocmask(SIG_SETMASK, NULL, &mask) != 0 ||
		    !sigismember(&mask, SIGUSR1) ||
		    !sigismember(&mask, SIGUSR2))
			problem = 4;
		break;
	case PERSIST_MASK:
		(void) sigaddset(&uc->uc_sigmask, SIGUSR2);
		break;
	case WAIT_READ: {
		char ack = 'A';
		if (raw3(SYS_write, ack_fd, (uintptr_t)&ack, 1) != 1)
			problem = 9;
		break;
	}
	case NESTED:
		if (send_self(SIGUSR2) != 0 || inner_hits != 1)
			problem = 5;
		break;
	case NODEFER:
		if (hits == 1 && send_self(SIGUSR1) != 0)
			problem = 6;
		break;
	case ALTSTACK: {
		stack_t ss;
		if ((uintptr_t)&local < (uintptr_t)altstack_mem ||
		    (uintptr_t)&local >= (uintptr_t)altstack_mem +
		    sizeof (altstack_mem) ||
		    sigaltstack(NULL, &ss) != 0 || !(ss.ss_flags & SS_ONSTACK))
			problem = 7;
		break;
	}
	case CHANGE_RAX:
		uc->uc_mcontext.gregs[REG_RAX] = 0x12345678;
		break;
	case POISON_FLAGS:
		uc->uc_flags |= UINT64_C(1) << 63;
		uc->uc_link = (void *)(uintptr_t)0xffff800000000000ULL;
		uc->uc_mcontext.__reserved1[0] = UINT64_MAX;
		break;
	case BAD_CS:
		uc->uc_mcontext.gregs[REG_CSGSFS] &= ~UINT64_C(0xffff);
		uc->uc_mcontext.gregs[REG_CSGSFS] |= 8;
		break;
	case BAD_SS:
		uc->uc_mcontext.gregs[REG_CSGSFS] &=
		    UINT64_C(0x0000ffffffffffff);
		uc->uc_mcontext.gregs[REG_CSGSFS] |= UINT64_C(0x10) << 48;
		uc->uc_flags |= 6;
		break;
	case BAD_IOPL:
		uc->uc_mcontext.gregs[REG_EFL] |= 0x3000;
		break;
	case BAD_IP:
		uc->uc_mcontext.gregs[REG_RIP] = (greg_t)0xffff800000000000ULL;
		break;
	case BAD_SP:
		uc->uc_mcontext.gregs[REG_RSP] = (greg_t)0xffff800000000000ULL;
		break;
	case BAD_FP:
		uc->uc_mcontext.fpregs =
		    (void *)(uintptr_t)0xffff800000000000ULL;
		break;
	case BAD_MXCSR:
		uc->uc_mcontext.fpregs->mxcsr |= 0x80000000U;
		break;
	case NATIVE_ESCAPE: {
		long nr = 230;
		__asm__ volatile("int $0x91" : "+a" (nr) : : "cc", "memory");
		problem = 8;
		break;
	}
	default:
		break;
	}
}

static int
setup(int mode, int flags)
{
	struct sigaction sa;
	sigset_t empty;

	self_pid = getpid();
	self_tid = syscall(SYS_gettid);
	self_uid = getuid();
	expected_sender = self_pid;
	kind = mode;
	memset(&sa, 0, sizeof (sa));
	sa.sa_sigaction = handler;
	sa.sa_flags = SA_SIGINFO | flags;
	(void) sigemptyset(&sa.sa_mask);
	(void) sigemptyset(&empty);
	if (sigprocmask(SIG_SETMASK, &empty, NULL) != 0)
		return (1);
	if (mode == MASK)
		(void) sigaddset(&sa.sa_mask, SIGUSR2);
	if (mode == NESTED && sigaction(SIGUSR2, &sa, NULL) != 0)
		return (1);
	return (sigaction(SIGUSR1, &sa, NULL) != 0);
}

static int
exercise(int mode, int flags)
{
	if (setup(mode, flags) != 0 || filter() != 0)
		return (1);
	if (send_self(SIGUSR1) != (mode == CHANGE_RAX ? 0x12345678 : 0))
		return (2);
	if (problem || hits != (mode == NODEFER ? 2 : 1) ||
	    raw3(SYS_getppid, 0, 0, 0) != -EACCES)
		return (3);
	return (0);
}

static int basic(void) { return (exercise(BASIC, 0)); }
static int nested(void) { return (exercise(NESTED, 0)); }
static int nodefer(void) { return (exercise(NODEFER, SA_NODEFER)); }
static int context_rax(void) { return (exercise(CHANGE_RAX, 0)); }
static int flags_ignored(void) { return (exercise(POISON_FLAGS, 0)); }
static int bad_cs(void) { return (exercise(BAD_CS, 0)); }
static int bad_ss(void) { return (exercise(BAD_SS, 0)); }
static int bad_iopl(void) { return (exercise(BAD_IOPL, 0)); }
static int bad_ip(void) { return (exercise(BAD_IP, 0)); }
static int bad_sp(void) { return (exercise(BAD_SP, 0)); }
static int bad_fp(void) { return (exercise(BAD_FP, 0)); }
static int bad_mxcsr(void) { return (exercise(BAD_MXCSR, 0)); }
static int native_escape(void) { return (exercise(NATIVE_ESCAPE, 0)); }

static int
registration_after_filter(void)
{
	/* Replace the action after installing the filter. */
	struct sigaction sa;
	if (setup(BASIC, 0) != 0 || filter() != 0)
		return (1);
	memset(&sa, 0, sizeof (sa));
	sa.sa_sigaction = handler;
	sa.sa_flags = SA_SIGINFO;
	if (sigaction(SIGUSR1, &sa, NULL) != 0)
		return (2);
	return (send_self(SIGUSR1) != 0 || hits != 1 || problem);
}

static int
mask_restore(void)
{
	sigset_t mask;
	if (exercise(MASK, 0) != 0 ||
	    sigprocmask(SIG_SETMASK, NULL, &mask) != 0)
		return (1);
	return (sigismember(&mask, SIGUSR1) || sigismember(&mask, SIGUSR2));
}

static int
mask_context_edit(void)
{
	sigset_t mask;
	if (exercise(PERSIST_MASK, 0) != 0 ||
	    sigprocmask(SIG_SETMASK, NULL, &mask) != 0)
		return (1);
	return (!sigismember(&mask, SIGUSR2));
}

static int
altstack_case(void)
{
	stack_t ss = { .ss_sp = altstack_mem,
	    .ss_size = sizeof (altstack_mem) };
	if (sigaltstack(&ss, NULL) != 0 ||
	    exercise(ALTSTACK, SA_ONSTACK) != 0 ||
	    sigaltstack(NULL, &ss) != 0)
		return (1);
	return ((ss.ss_flags & SS_ONSTACK) != 0 || ss.ss_sp != altstack_mem);
}

static int
altstack_after_filter(void)
{
	stack_t ss = { .ss_sp = altstack_mem,
	    .ss_size = sizeof (altstack_mem) };
	if (setup(ALTSTACK, SA_ONSTACK) != 0 || filter() != 0 ||
	    sigaltstack(&ss, NULL) != 0)
		return (1);
	return (send_self(SIGUSR1) != 0 || hits != 1 || problem);
}

static int
reset_hand(void)
{
	if (exercise(BASIC, SA_RESETHAND) != 0)
		return (1);
	(void) send_self(SIGUSR1);
	return (2);
}

static int
return_policy(void)
{
	if (setup(BASIC, 0) != 0 || filter() != 0 ||
	    deny_rule(SYS_rt_sigreturn, SECCOMP_RET_KILL_PROCESS) != 0)
		return (1);
	(void) send_self(SIGUSR1);
	return (2);
}

static int
handler_policy(void)
{
	struct sigaction sa;
	if (setup(BASIC, 0) != 0 || filter() != 0 ||
	    deny_rule(SYS_rt_sigaction, SECCOMP_RET_ERRNO | EPERM) != 0)
		return (1);
	memset(&sa, 0, sizeof (sa));
	sa.sa_handler = SIG_IGN;
	errno = 0;
	return (sigaction(SIGUSR1, &sa, NULL) != -1 || errno != EPERM);
}

static int
raw_bad_frame(void)
{
	if (filter() != 0)
		return (1);
	(void) raw3(SYS_rt_sigreturn, 0, 0, 0);
	return (2);
}

static int
replay_frame(void)
{
	if (exercise(BASIC, 0) != 0)
		return (1);
	__asm__ volatile("mov %[sp], %%rsp; mov $15, %%rax; syscall; ud2"
	    : : [sp] "r" (saved_frame + 8) : "rax", "rcx", "r11", "memory");
	__builtin_unreachable();
}

static int
sse_preservation(void)
{
	uint64_t input[2] = { UINT64_C(0x123456789abcdef0),
	    UINT64_C(0xfedcba9876543210) };
	uint64_t output[2];
	long result;
	if (setup(CLOBBER_SSE, 0) != 0 || filter() != 0)
		return (1);
	__asm__ volatile("movdqu %[in], %%xmm7; syscall; movdqu %%xmm7, %[out]"
	    : "=a" (result), [out] "=m" (output)
	    : "0" ((long)SYS_tgkill), "D" ((long)self_pid),
	    "S" ((long)self_tid), "d" ((long)SIGUSR1), [in] "m" (input)
	    : "rcx", "r11", "xmm7", "memory");
	return (result != 0 || problem || hits != 1 ||
	    memcmp(input, output, 16));
}


static __attribute__((target("avx"))) int
avx_preservation(void)
{
	uint64_t input[4] = { UINT64_C(0x123456789abcdef0),
	    UINT64_C(0xfedcba9876543210), UINT64_C(0xaabbccdd11223344),
	    UINT64_C(0x9988776655443322) };
	uint64_t output[4];
	long result;

	if (!__builtin_cpu_supports("avx"))
		return (77);
	if (setup(CLOBBER_AVX, 0) != 0 || filter() != 0)
		return (1);
	__asm__ volatile("vmovdqu %[in], %%ymm7; syscall; "
	    "vmovdqu %%ymm7, %[out]"
	    : "=a" (result), [out] "=m" (output)
	    : "0" ((long)SYS_tgkill), "D" ((long)self_pid),
	    "S" ((long)self_tid), "d" ((long)SIGUSR1), [in] "m" (input)
	    : "rcx", "r11", "ymm7", "memory");
	return (result != 0 || problem || hits != 1 ||
	    memcmp(input, output, 32));
}

static int
wait_sleeping(pid_t pid)
{
	char path[80], line[128];
	unsigned int attempt;

	(void) snprintf(path, sizeof (path), "/proc/%ld/status", (long)pid);
	for (attempt = 0; attempt < 1000; attempt++) {
		FILE *f = fopen(path, "r");
		if (f == NULL)
			return (1);
		while (fgets(line, sizeof (line), f) != NULL) {
			if (strncmp(line, "State:", 6) == 0) {
				char *value = strchr(line, ':');
				int sleeping = value != NULL &&
				    strchr(value, 'S') != NULL;
				(void) fclose(f);
				if (sleeping)
					return (0);
				f = NULL;
				break;
			}
		}
		if (f != NULL)
			(void) fclose(f);
		(void) usleep(1000);
	}
	return (1);
}

static int
interrupted_read(int restart)
{
	int ready[2], ack[2], data[2], status;
	pid_t sender, target;
	char byte = 'D';
	ssize_t n;
	int error;

	if (setup(WAIT_READ, restart ? SA_RESTART : 0) != 0 ||
	    pipe(ready) != 0 || pipe(ack) != 0 || pipe(data) != 0)
		return (1);
	target = self_pid;
	sender = fork();
	if (sender == -1)
		return (1);
	if (sender == 0) {
		(void) alarm(4);
		(void) close(ready[1]);
		(void) close(ack[1]);
		(void) close(data[0]);
		if (read(ready[0], &byte, 1) != 1 || wait_sleeping(target) != 0)
			_exit(2);
		if (syscall(SYS_tgkill, target, target, SIGUSR1) != 0 ||
		    read(ack[0], &byte, 1) != 1 || write(data[1], &byte, 1) != 1)
			_exit(3);
		_exit(0);
	}
	expected_sender = sender;
	ack_fd = ack[1];
	(void) close(ready[0]);
	(void) close(ack[0]);
	(void) close(data[1]);
	if (filter() != 0 || write(ready[1], &byte, 1) != 1)
		return (4);
	errno = 0;
	n = read(data[0], &byte, 1);
	error = errno;
	if (restart) {
		if (n != 1)
			return (5);
	} else {
		if (n != -1 || error != EINTR || read(data[0], &byte, 1) != 1)
			return (6);
	}
	if (waitpid(sender, &status, 0) != sender ||
	    !WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return (7);
	return (hits != 1 || problem || raw3(SYS_getppid, 0, 0, 0) != -EACCES);
}

static int read_eintr(void) { return (interrupted_read(0)); }
static int read_restart(void) { return (interrupted_read(1)); }

static void
pwr_handler(int sig)
{
	if (sig == SIGPWR)
		hits++;
}

static int
pwr_reset(void)
{
	struct sigaction sa;

	self_pid = getpid();
	self_tid = syscall(SYS_gettid);
	memset(&sa, 0, sizeof (sa));
	sa.sa_handler = pwr_handler;
	sa.sa_flags = SA_RESETHAND;
	if (sigaction(SIGPWR, &sa, NULL) != 0 || filter() != 0 ||
	    send_self(SIGPWR) != 0 || hits != 1)
		return (1);
	(void) send_self(SIGPWR);
	return (2);
}

static void
segv_handler(int sig, siginfo_t *info, void *context)
{
	ucontext_t *uc = context;

	hits++;
	if (sig != SIGSEGV || info == NULL || info->si_signo != SIGSEGV ||
	    raw3(SYS_getppid, 0, 0, 0) != -EACCES)
		problem = 10;
	if (fault_resume != 0) {
		if (info->si_code != SEGV_MAPERR || info->si_addr != (void *)1)
			problem = 11;
		uc->uc_mcontext.gregs[REG_RIP] = fault_resume;
	} else if (info->si_code != SI_TKILL || info->si_pid != self_pid ||
	    info->si_uid != self_uid) {
		problem = 12;
	}
}

static int
segv_setup(void)
{
	struct sigaction sa;

	self_pid = getpid();
	self_tid = syscall(SYS_gettid);
	self_uid = getuid();
	memset(&sa, 0, sizeof (sa));
	sa.sa_sigaction = segv_handler;
	sa.sa_flags = SA_SIGINFO;
	return (sigaction(SIGSEGV, &sa, NULL) != 0 || filter() != 0);
}

static int
user_segv_info(void)
{
	if (segv_setup() != 0 || send_self(SIGSEGV) != 0)
		return (1);
	return (hits != 1 || problem || raw3(SYS_getppid, 0, 0, 0) != -EACCES);
}

static int
fault_recovery(void)
{
	if (segv_setup() != 0)
		return (1);
	__asm__ volatile("leaq 1f(%%rip), %%rax; movq %%rax, %[resume]; "
	    "movq (%%rcx), %%rax; 1:"
	    : [resume] "=m" (fault_resume) : "c" ((uintptr_t)1)
	    : "rax", "memory");
	return (hits != 1 || problem || raw3(SYS_getppid, 0, 0, 0) != -EACCES);
}

static int
boundary_syscalls(void)
{
	if (!is_lx)
		return (0);
	errno = 0;
	if (syscall(323, 0, 0, 0, 0, 0, 0) != -1 || errno != ENOSYS)
		return (1);
	errno = 0;
	return (syscall(324, 0, 0, 0, 0, 0, 0) != -1 || errno != ENOSYS);
}

static int
run(const char *name, int (*fn)(void), int sig)
{
	pid_t child = fork();
	int status;
	if (child == -1)
		exit(2);
	if (child == 0) {
		(void) alarm(5);
		_exit(fn());
	}
	if (waitpid(child, &status, 0) != child)
		exit(2);
	if (sig == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 77) {
		printf("%s: SKIP (CPU feature unavailable)\n", name);
		skipped++;
		return (0);
	}
	if ((sig != 0 && WIFSIGNALED(status) && WTERMSIG(status) == sig) ||
	    (sig == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
		printf("%s: PASS\n", name);
		return (0);
	}
	printf("%s: FAIL status=0x%x\n", name, status);
	return (1);
}

int
main(void)
{
	struct rlimit core = { 0, 0 };
	int count = 0, failed = 0;
	is_lx = access("/native/usr/bin/uname", X_OK) == 0;
	(void) setrlimit(RLIMIT_CORE, &core);
#define	RUN(fn, sig) do { \
	count++; \
	failed += run(#fn, fn, sig); \
} while (0)
	RUN(basic, 0);
	RUN(registration_after_filter, 0);
	RUN(mask_restore, 0);
	RUN(mask_context_edit, 0);
	RUN(nested, 0);
	RUN(nodefer, 0);
	RUN(altstack_case, 0);
	RUN(altstack_after_filter, 0);
	RUN(context_rax, 0);
	RUN(flags_ignored, 0);
	RUN(reset_hand, SIGUSR1);
	RUN(return_policy, SIGSYS);
	RUN(handler_policy, 0);
	RUN(sse_preservation, 0);
	RUN(avx_preservation, 0);
	RUN(read_eintr, 0); RUN(read_restart, 0); RUN(pwr_reset, SIGPWR);
	RUN(user_segv_info, 0); RUN(fault_recovery, 0);
	if (is_lx) {
		RUN(bad_cs, SIGSEGV); RUN(bad_ss, SIGSEGV);
		RUN(bad_iopl, SIGSEGV); RUN(bad_ip, SIGSEGV);
		RUN(bad_sp, SIGSEGV); RUN(bad_fp, SIGSEGV);
		RUN(bad_mxcsr, SIGSEGV); RUN(raw_bad_frame, SIGSEGV);
		RUN(replay_frame, SIGSEGV); RUN(native_escape, SIGSYS);
		RUN(boundary_syscalls, 0);
	}
#undef RUN
	printf("%d signal checks, %d failed, %d skipped\n",
	    count, failed, skipped);
	return (failed != 0);
}
