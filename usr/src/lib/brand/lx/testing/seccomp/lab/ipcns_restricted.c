#define	_GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define	STACK_SIZE (1024 * 1024)

static int outer_shmid;

static void
fail(const char *what)
{
	fprintf(stderr, "FAIL %s: %s\n", what, strerror(errno));
	exit(1);
}

static void
check_isolated(const char *where)
{
	struct shmid_ds ds;

	errno = 0;
	if (shmctl(outer_shmid, IPC_STAT, &ds) != -1 || errno != ENOSYS)
		fail(where);
	errno = 0;
	if (shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600) != -1 ||
	    errno != ENOSYS)
		fail("isolated shmget");
	errno = 0;
	if (semget(IPC_PRIVATE, 1, IPC_CREAT | 0600) != -1 ||
	    errno != ENOSYS)
		fail("isolated semget");
	errno = 0;
	if (msgget(IPC_PRIVATE, IPC_CREAT | 0600) != -1 || errno != ENOSYS)
		fail("isolated msgget");
}

static int
clone_child(void *unused)
{
	(void) unused;
	return (0);
}

static void
wait_ok(pid_t child, const char *what)
{
	int status;

	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0)
		fail(what);
}

static void
run_enabled(void)
{
	struct shmid_ds ds;
	char *stack;
	pid_t child;

	stack = malloc(STACK_SIZE);
	if (stack == NULL)
		fail("malloc clone stack");
	errno = 0;
	child = clone(clone_child, stack + STACK_SIZE,
	    CLONE_NEWIPC | SIGCHLD, NULL);
	if (child != -1 || errno != EOPNOTSUPP)
		fail("IPC-only clone must fail safely");
	free(stack);

	if (shmctl(outer_shmid, IPC_STAT, &ds) != 0)
		fail("failed clone changed parent IPC view");
	if (unshare(CLONE_NEWIPC) != 0)
		fail("unshare CLONE_NEWIPC");
	check_isolated("unshare cannot see outer shm");

	child = fork();
	if (child < 0)
		fail("fork after IPC unshare");
	if (child == 0) {
		check_isolated("fork inherited IPC isolation");
		_exit(0);
	}
	wait_ok(child, "wait inherited IPC isolation");
}

int
main(int argc, char **argv)
{
	struct shmid_ds ds;
	pid_t worker;

	if (argc != 2 || (strcmp(argv[1], "disabled") != 0 &&
	    strcmp(argv[1], "enabled") != 0)) {
		fprintf(stderr, "usage: %s disabled|enabled\n", argv[0]);
		return (2);
	}

	if (strcmp(argv[1], "disabled") == 0) {
		errno = 0;
		if (unshare(CLONE_NEWIPC) != -1 || errno != EPERM)
			fail("default-off CLONE_NEWIPC");
		puts("IPCNS_DISABLED_PASS");
		return (0);
	}

	outer_shmid = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
	if (outer_shmid < 0)
		fail("create outer shm");
	worker = fork();
	if (worker < 0)
		fail("fork worker");
	if (worker == 0) {
		run_enabled();
		_exit(0);
	}
	wait_ok(worker, "wait IPC namespace worker");
	if (shmctl(outer_shmid, IPC_STAT, &ds) != 0)
		fail("outer shm survived worker");
	if (shmctl(outer_shmid, IPC_RMID, NULL) != 0)
		fail("remove outer shm");
	puts("IPCNS_RESTRICTED_PASS");
	return (0);
}
