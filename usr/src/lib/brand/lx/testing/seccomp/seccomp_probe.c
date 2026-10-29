/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * A copy is available at http://www.illumos.org/license/CDDL.
 *
 * Compile inside Linux or an lx zone, not as a native illumos program:
 * cc -std=c99 -Wall -Wextra -Werror -O2 seccomp_probe.c -o seccomp_probe
 */
#define	_GNU_SOURCE
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#if defined(__x86_64__)
#define	PROBE_ARCH AUDIT_ARCH_X86_64
#elif defined(__i386__)
#define	PROBE_ARCH AUDIT_ARCH_I386
#else
#error Add the Linux audit architecture before running on this platform
#endif

#define	SKIP 77

static int
initial_mode(void)
{
	int mode = prctl(PR_GET_SECCOMP, 0UL, 0UL, 0UL, 0UL);

	/* A Linux container may inherit a host filter. lx has none today. */
	if (access("/native/usr/bin/uname", X_OK) == 0 && mode != 0) {
		fprintf(stderr, "lx claims initial seccomp mode %d "
		    "without enforcement\n",
		    mode);
		return (1);
	}
	return (mode < 0 || mode > SECCOMP_MODE_FILTER);
}

static int
null_filter(void)
{
	errno = 0;
	return (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER,
	    0UL, 0UL, 0UL) != -1 ||
	    errno != EFAULT);
}

static int
invalid_mode(void)
{
	errno = 0;
	return (prctl(PR_SET_SECCOMP, 99UL, 0UL, 0UL, 0UL) != -1 ||
	    errno != EINVAL);
}

static int
enforcement(int use_syscall)
{
	struct sock_filter insns[] = {
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
		    offsetof(struct seccomp_data, arch)),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, PROBE_ARCH, 1, 0),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL),
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
		    offsetof(struct seccomp_data, nr)),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_getppid, 0, 1),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EACCES),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)
	};
	struct sock_fprog prog = {
		.len = sizeof (insns) / sizeof (insns[0]),
		.filter = insns
	};
	int before, rc, error;
	long result;

	before = prctl(PR_GET_SECCOMP, 0UL, 0UL, 0UL, 0UL);
	if (prctl(PR_SET_NO_NEW_PRIVS, 1UL, 0UL, 0UL, 0UL) != 0) {
		perror("PR_SET_NO_NEW_PRIVS");
		return (1);
	}
	if (use_syscall) {
		rc = syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog);
	} else {
		rc = prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER,
		    &prog, 0UL, 0UL);
	}
	error = errno;
	if (rc == -1) {
		if (prctl(PR_GET_SECCOMP, 0UL, 0UL, 0UL, 0UL) != before) {
			fprintf(stderr,
			    "mode changed after failed installation\n");
			return (1);
		}
		if (error == EINVAL || error == ENOSYS ||
		    error == EOPNOTSUPP) {
			printf("  filter unavailable, errno=%d; "
			    "installation rejected\n",
			    error);
			return (SKIP);
		}
		errno = error;
		perror("filter installation");
		return (1);
	}
	errno = 0;
	result = syscall(SYS_getppid);
	if (result != -1 || errno != EACCES) {
		fprintf(stderr, "filter accepted but getppid returned %ld "
		    "errno=%d\n",
		    result, errno);
		return (1);
	}
	return (prctl(PR_GET_SECCOMP, 0UL, 0UL, 0UL, 0UL) !=
	    SECCOMP_MODE_FILTER);
}

static int
prctl_filter(void)
{
	return (enforcement(0));
}

static int
syscall_filter(void)
{
	return (enforcement(1));
}

int
main(void)
{
	struct {
		const char *name;
		int (*run)(void);
	} tests[] = {
		{ "initial mode", initial_mode },
		{ "null prctl filter", null_filter },
		{ "invalid prctl mode", invalid_mode },
		{ "prctl filter enforces EACCES", prctl_filter },
		{ "seccomp syscall filter enforces EACCES", syscall_filter }
	};
	unsigned int i;
	int failed = 0, missing = 0;

	(void) setvbuf(stdout, NULL, _IONBF, 0);
	for (i = 0; i < sizeof (tests) / sizeof (tests[0]); i++) {
		int status, rc;
		pid_t pid = fork();
		if (pid == -1) {
			perror("fork");
			return (1);
		}
		if (pid == 0)
			_exit(tests[i].run());
		if (waitpid(pid, &status, 0) != pid) {
			perror("waitpid");
			return (1);
		}
		rc = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
		printf("%s: %s\n", tests[i].name,
		    rc == 0 ? "PASS" : rc == SKIP ? "UNSUPPORTED" : "FAIL");
		failed += (rc != 0 && rc != SKIP);
		missing += (rc == SKIP);
	}
	printf("%d failed, %d unsupported; sandbox filtering %s\n", failed,
	    missing, failed == 0 && missing == 0 ? "available" : "NOT ready");
	return (failed != 0 ? 1 : missing != 0 ? SKIP : 0);
}
