/*
 * CDDL version 1.0, http://www.illumos.org/license/CDDL.
 */
#define	_GNU_SOURCE
#include <errno.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#if !defined(__x86_64__)
#error This enforcement probe requires x86-64
#endif

#define	ALLOW SECCOMP_RET_ALLOW
#define	DENY(e) (SECCOMP_RET_ERRNO | (e))
#define	COUNT(a) (sizeof (a) / sizeof ((a)[0]))

static int
install(struct sock_filter *code, unsigned int n, int use_syscall)
{
	struct sock_fprog fp = { .len = n, .filter = code };

	if (prctl(PR_SET_NO_NEW_PRIVS, 1UL, 0UL, 0UL, 0UL) != 0)
		return (-1);
	return (use_syscall ? syscall(SYS_seccomp, 1, 0, &fp) :
	    prctl(PR_SET_SECCOMP, 2, &fp, 0UL, 0UL));
}

static int
allow_all(void)
{
	struct sock_filter p[] = { BPF_STMT(BPF_RET | BPF_K, ALLOW) };

	return (install(p, COUNT(p), 1));
}

static int
rule(int nr, uint32_t value, int use_syscall)
{
	struct sock_filter p[] = {
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (uint32_t)nr, 0, 1),
		BPF_STMT(BPF_RET | BPF_K, value),
		BPF_STMT(BPF_RET | BPF_K, ALLOW)
	};

	return (install(p, COUNT(p), use_syscall));
}

static int
check_errno(int nr, int value, int use_syscall)
{
	long r;

	if (rule(nr, DENY(value), use_syscall) != 0)
		return (1);
	errno = 0;
	r = syscall(nr);
	return (value == 0 ? r != 0 : r != -1 || errno != value);
}

static int deny_prctl(void) { return (check_errno(SYS_getppid, 13, 0)); }
static int deny_syscall(void) { return (check_errno(SYS_getppid, 123, 1)); }
static int deny_unknown(void) { return (check_errno(9999, 17, 1)); }
static int deny_negative(void) { return (check_errno(-1, 18, 1)); }
static int errno_zero(void) { return (check_errno(SYS_getppid, 0, 1)); }

static int
errno_clamp(void)
{
	if (rule(SYS_getppid, DENY(65535), 1) != 0)
		return (1);
	errno = 0;
	return (syscall(SYS_getppid) != -1 || errno != 4095);
}

static int
immutable(void)
{
	struct sock_filter p[] = {
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_getppid, 0, 1),
		BPF_STMT(BPF_RET | BPF_K, DENY(19)),
		BPF_STMT(BPF_RET | BPF_K, ALLOW)
	};

	if (install(p, COUNT(p), 1) != 0)
		return (1);
	((volatile struct sock_filter *)p)[2].k = ALLOW;
	errno = 0;
	return (syscall(SYS_getppid) != -1 || errno != 19);
}

static int
stack_tie(void)
{
	if (rule(SYS_getppid, DENY(21), 1) != 0 ||
	    rule(SYS_getppid, DENY(22), 0) != 0)
		return (1);
	errno = 0;
	return (syscall(SYS_getppid) != -1 || errno != 22);
}

static int
stack_allow(void)
{
	if (rule(SYS_getppid, DENY(23), 1) != 0 || allow_all() != 0)
		return (1);
	errno = 0;
	return (syscall(SYS_getppid) != -1 || errno != 23);
}

static int
mode(void)
{
	char buf[4096];
	int fd;
	ssize_t n;
	size_t carry = 0;
	unsigned int reads;

	if (allow_all() != 0 ||
	    prctl(PR_GET_SECCOMP, 0UL, 0UL, 0UL, 0UL) != 2)
		return (2);
	errno = 0;
	if (prctl(PR_SET_SECCOMP, 0UL, 0UL, 0UL, 0UL) != -1 ||
	    errno != EINVAL)
		return (3);
	fd = syscall(SYS_open, "/proc/self/status", 0, 0);
	if (fd < 0)
		return (4);
	for (reads = 0; reads < 256; reads++) {
		n = syscall(SYS_read, fd, buf + carry,
		    sizeof (buf) - carry - 1);
		if (n <= 0)
			break;
		buf[carry + n] = 0;
		if (strstr(buf, "Seccomp:\t2\n") != NULL) {
			(void) syscall(SYS_close, fd);
			return (0);
		}
		carry += n;
		if (carry > 32) {
			memmove(buf, buf + carry - 32, 32);
			carry = 32;
		}
	}
	(void) syscall(SYS_close, fd);
	return (5);
}

static int
invalid_arguments(void)
{
	struct sock_filter p[] = { BPF_STMT(BPF_RET | BPF_K, ALLOW) };
	struct sock_fprog fp = { .len = 1, .filter = p };

	if (prctl(PR_SET_NO_NEW_PRIVS, 1UL, 0UL, 0UL, 0UL) != 0)
		return (1);
#define	BAD(call, err) do { \
	errno = 0; \
	if ((call) != -1 || errno != (err)) \
		return (1); \
} while (0)
	BAD(syscall(SYS_seccomp, 99, 0, &fp), EINVAL);
	BAD(syscall(SYS_seccomp, 1, 0x80000000U, &fp), EINVAL);
	BAD(prctl(PR_SET_SECCOMP, 2UL, 0UL, 0UL, 0UL), EFAULT);
	BAD(syscall(SYS_seccomp, 1, 0, (void *)1), EFAULT);
	fp.len = 0;
	BAD(syscall(SYS_seccomp, 1, 0, &fp), EINVAL);
	fp.len = 4097;
	BAD(syscall(SYS_seccomp, 1, 0, &fp), EINVAL);
	fp.len = 1;
	fp.filter = (void *)1;
	BAD(syscall(SYS_seccomp, 1, 0, &fp), EFAULT);
	fp.filter = p;
	p[0].code = 0xffff;
	BAD(syscall(SYS_seccomp, 1, 0, &fp), EINVAL);
#undef BAD
	return (0);
}

static int
no_privilege(void)
{
	struct sock_filter p[] = { BPF_STMT(BPF_RET | BPF_K, ALLOW) };
	struct sock_fprog fp = { .len = 1, .filter = p };

	errno = 0;
	return (syscall(SYS_seccomp, 1, 0, &fp) != -1 || errno != EACCES);
}

static int
before_copyin(void)
{
	if (rule(SYS_read, DENY(24), 1) != 0)
		return (1);
	errno = 0;
	return (syscall(SYS_read, -1, (void *)1, (size_t)-1) != -1 ||
	    errno != 24);
}

/* Reuse one exact instruction site for full-width metadata checks. */
static __attribute__((noinline, noclone)) long
raw_target(uint64_t args[6], uint64_t *pc, int run)
{
	register uint64_t r10 __asm__("r10") = args[3];
	register uint64_t r8 __asm__("r8") = args[4];
	register uint64_t r9 __asm__("r9") = args[5];
	long result = SYS_getppid;
	uint64_t address;

	__asm__ volatile(
	    "leaq 1f(%%rip), %[pc]\n\t"
	    "test %[run], %[run]\n\t"
	    "jz 1f\n\t"
	    "syscall\n1:"
	    : "+a" (result), [pc] "=&r" (address)
	    : "D" (args[0]), "S" (args[1]), "d" (args[2]),
	    "r" (r10), "r" (r8), "r" (r9), [run] "r" (run)
	    : "rcx", "r11", "cc", "memory");
	*pc = address;
	return (result);
}

static int
metadata(void)
{
	uint64_t args[6] = { UINT64_C(0x123456789abcdef0),
	    UINT64_C(0xfedcba9876543210), UINT64_C(0xaabbccdd11223344),
	    UINT64_C(0xdeadbeef159a371d), UINT64_C(0x3344556677889900),
	    UINT64_C(0x9988776655443322) };
	uint64_t pc;
	uint32_t words[16];
	struct sock_filter p[64];
	unsigned int i, n = 0;

	(void) raw_target(args, &pc, 0);
	words[0] = SYS_getppid;
	words[1] = 0xc000003e;
	words[2] = pc;
	words[3] = pc >> 32;
	for (i = 0; i < 6; i++) {
		words[4 + i * 2] = args[i];
		words[5 + i * 2] = args[i] >> 32;
	}
	p[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0);
	p[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
	    SYS_getppid, 1, 0);
	p[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, ALLOW);
	for (i = 1; i < 16; i++) {
		p[n++] = (struct sock_filter)BPF_STMT(
		    BPF_LD | BPF_W | BPF_ABS, i * 4);
		p[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
		    words[i], 1, 0);
		p[n++] = (struct sock_filter)BPF_STMT(
		    BPF_RET | BPF_K, DENY(71));
	}
	p[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, DENY(72));
	if (install(p, n, 1) != 0 || raw_target(args, &pc, 1) != -72)
		return (1);
	args[5] ^= UINT64_C(0x100000000);
	return (raw_target(args, &pc, 1) != -71);
}

static int
kill_thread(void)
{
	if (rule(SYS_getppid, SECCOMP_RET_KILL, 1) != 0)
		return (1);
	(void) syscall(SYS_getppid);
	return (2);
}
static int
kill_process(void)
{
	if (rule(SYS_getppid, SECCOMP_RET_KILL_PROCESS, 1) != 0)
		return (1);
	(void) syscall(SYS_getppid);
	return (2);
}
static int
kill_over_errno(void)
{
	if (rule(SYS_getppid, SECCOMP_RET_KILL_PROCESS, 1) != 0 ||
	    rule(SYS_getppid, DENY(55), 1) != 0)
		return (1);
	(void) syscall(SYS_getppid);
	return (2);
}

static int
escape_int80(void)
{
	long r = 110;

	if (allow_all() != 0)
		return (1);
	__asm__ volatile("int $0x80" : "+a" (r) : : "cc", "memory");
	return (2);
}

static int
escape_int91(void)
{
	long r = 230;

	if (allow_all() != 0)
		return (1);
	__asm__ volatile("int $0x91" : "+a" (r) : : "cc", "memory");
	return (2);
}

static int
escape_fork(void)
{
	if (allow_all() != 0)
		return (1);
	(void) syscall(SYS_fork);
	return (2);
}

static int
escape_clone(void)
{
	if (allow_all() != 0)
		return (1);
	(void) syscall(SYS_clone, 0, 0, 0, 0, 0);
	return (2);
}

static int
escape_exec(void)
{
	if (allow_all() != 0)
		return (1);
	(void) syscall(SYS_execve, "/native/usr/bin/true", 0, 0);
	return (2);
}

static int
invalid_sigreturn(void)
{
	if (allow_all() != 0)
		return (1);
	(void) syscall(SYS_rt_sigreturn);
	return (2);
}

static int
escape_vsyscall(void)
{
	if (allow_all() != 0)
		return (1);
	(void) ((long (*)(void *))(uintptr_t)0xffffffffff600400ULL)(NULL);
	return (2);
}

static volatile sig_atomic_t signal_seen;

static void
empty_handler(int sig)
{
	signal_seen = sig == SIGUSR1;
}

static int
caught_signal(void)
{
	pid_t pid = getpid(), tid = syscall(SYS_gettid);

	if (signal(SIGUSR1, empty_handler) == SIG_ERR || allow_all() != 0)
		return (1);
	if (syscall(SYS_tgkill, pid, tid, SIGUSR1) != 0)
		return (2);
	return (!signal_seen);
}

static atomic_int release_worker;

static void *
worker(void *arg)
{
	(void) arg;
	while (!atomic_load(&release_worker))
		(void) sched_yield();
	return (NULL);
}

static int
multiple_threads(void)
{
	pthread_t th;
	int rc, error;

	if (pthread_create(&th, NULL, worker, NULL) != 0)
		return (1);
	rc = allow_all();
	error = errno;
	atomic_store(&release_worker, 1);
	if (pthread_join(th, NULL) != 0)
		return (1);
	return (rc != -1 || error != EOPNOTSUPP ||
	    prctl(PR_GET_SECCOMP, 0UL, 0UL, 0UL, 0UL) != 0);
}

static int
unsupported_actions(void)
{
	uint32_t actions[] = { SECCOMP_RET_TRAP, SECCOMP_RET_TRACE,
	    SECCOMP_RET_LOG, SECCOMP_RET_USER_NOTIF };
	struct sock_filter p[] = { BPF_STMT(BPF_RET | BPF_K, ALLOW) };
	unsigned int i;

	for (i = 0; i < COUNT(actions); i++) {
		p[0].k = actions[i];
		errno = 0;
		if (install(p, 1, 1) != -1 || errno != EINVAL)
			return (1);
	}
	p[0].code = BPF_RET | BPF_A;
	errno = 0;
	return (install(p, 1, 1) != -1 || errno != EINVAL ||
	    prctl(PR_GET_SECCOMP, 0UL, 0UL, 0UL, 0UL) != 0);
}

static int
chain_limit(void)
{
	unsigned int i;

	for (i = 0; i < 64; i++) {
		if (allow_all() != 0)
			return (1);
	}
	errno = 0;
	return (allow_all() != -1 || errno != ENOMEM ||
	    prctl(PR_GET_SECCOMP, 0UL, 0UL, 0UL, 0UL) != 2);
}

static int
path_limit(void)
{
	static struct sock_filter p[4096];
	unsigned int i;

	for (i = 0; i < COUNT(p) - 1; i++)
		p[i] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_IMM, 0);
	p[COUNT(p) - 1] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, ALLOW);
	for (i = 0; i < 7; i++) {
		if (install(p, COUNT(p), 1) != 0)
			return (1);
	}
	errno = 0;
	return (install(p, COUNT(p), 1) != -1 || errno != ENOMEM ||
	    prctl(PR_GET_SECCOMP, 0UL, 0UL, 0UL, 0UL) != 2);
}

static int
run(const char *name, int (*fn)(void), int sig)
{
	int status;
	pid_t child = fork();

	if (child == -1) {
		perror("fork");
		exit(2);
	}
	if (child == 0) {
		(void) alarm(5);
		_exit(fn());
	}
	if (waitpid(child, &status, 0) != child)
		exit(2);
	if ((sig && WIFSIGNALED(status) && WTERMSIG(status) == sig) ||
	    (!sig && WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
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
	int failed = 0, count = 0;
	int lx = access("/native/usr/bin/uname", X_OK) == 0;

	(void) setrlimit(RLIMIT_CORE, &core);
#define	RUN(fn, sig) do { \
	count++; \
	failed += run(#fn, fn, sig); \
} while (0)
	RUN(deny_prctl, 0); RUN(deny_syscall, 0); RUN(errno_zero, 0);
	RUN(errno_clamp, 0); RUN(deny_unknown, 0); RUN(deny_negative, 0);
	RUN(immutable, 0); RUN(stack_tie, 0); RUN(stack_allow, 0);
	RUN(mode, 0); RUN(invalid_arguments, 0); RUN(no_privilege, 0);
	RUN(before_copyin, 0); RUN(metadata, 0);
	RUN(kill_thread, SIGSYS); RUN(kill_process, SIGSYS);
	RUN(kill_over_errno, SIGSYS);
	if (lx) {
		RUN(escape_int80, SIGSYS); RUN(escape_int91, SIGSYS);
		RUN(escape_fork, SIGSYS); RUN(escape_clone, SIGSYS);
		RUN(escape_exec, SIGSYS); RUN(invalid_sigreturn, SIGSEGV);
		RUN(escape_vsyscall, SIGSYS); RUN(caught_signal, 0);
		RUN(multiple_threads, 0); RUN(unsupported_actions, 0);
		RUN(chain_limit, 0); RUN(path_limit, 0);
	}
#undef RUN
	printf("%d checks, %d failed; experimental subset only\n",
	    count, failed);
	return (failed != 0);
}
