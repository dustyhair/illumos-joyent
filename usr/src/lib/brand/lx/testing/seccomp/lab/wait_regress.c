/*
 * CDDL version 1.0, http://www.illumos.org/license/CDDL.
 */
#define	_GNU_SOURCE
#define	_POSIX_C_SOURCE 200809L
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif

static int gate[2];
static pid_t child = -1;
static volatile sig_atomic_t caught;

static void
cleanup(void)
{
	int status;

	(void) close(gate[1]);
	if (child > 0) {
		while (waitpid(child, &status, 0) < 0 && errno == EINTR)
			;
	}
}

#define	CHECK(x) do { \
	if (!(x)) { \
		(void) fprintf(stderr, "WAIT_FAIL line=%d errno=%d\n", \
		    __LINE__, errno); \
		cleanup(); \
		exit(1); \
	} \
} while (0)

static void
handler(int sig)
{
	(void) sig;
	caught = 1;
}

#ifdef __linux__
static int
clone_filter(void)
{
	pid_t clone_child;
	int status;
	char byte;

	CHECK(pipe(gate) == 0);
	CHECK((clone_child = syscall(SYS_clone, 0UL, NULL, NULL,
	    NULL, 0UL)) >= 0);
	if (clone_child == 0) {
		(void) close(gate[1]);
		if (read(gate[0], &byte, 1) != 0)
			_exit(2);
		_exit(19);
	}
	CHECK((child = fork()) >= 0);
	if (child == 0) {
		(void) close(gate[1]);
		if (read(gate[0], &byte, 1) != 0)
			_exit(2);
		_exit(23);
	}
	errno = 0;
	CHECK(waitpid(clone_child, &status, WNOHANG) == -1 &&
	    errno == ECHILD);
	errno = 0;
	CHECK(waitpid(child, &status, WNOHANG | __WCLONE) == -1 &&
	    errno == ECHILD);
	CHECK(waitpid(clone_child, &status, WNOHANG | __WALL) == 0);
	CHECK(waitpid(child, &status, WNOHANG | __WALL) == 0);
	CHECK(close(gate[1]) == 0);
	CHECK(waitpid(clone_child, &status, __WCLONE) == clone_child &&
	    WIFEXITED(status) && WEXITSTATUS(status) == 19);
	CHECK(waitpid(child, &status, __WALL) == child &&
	    WIFEXITED(status) && WEXITSTATUS(status) == 23);
	child = -1;
	(void) puts("WAIT_CLONE_FILTER_PASS cases=6");
	return (0);
}
#endif

int
main(int argc, char **argv)
{
	struct sigaction action;
	struct itimerval timer;
	int status;
	char byte;

#ifdef __linux__
	if (argc == 2 && strcmp(argv[1], "--clone-filter") == 0)
		return (clone_filter());
#endif
	(void) argv;
	if (argc != 1)
		return (2);
	CHECK(pipe(gate) == 0);
	CHECK((child = fork()) >= 0);
	if (child == 0) {
		(void) close(gate[1]);
		if (read(gate[0], &byte, 1) != 0)
			_exit(2);
		_exit(37);
	}
	CHECK(waitpid(child, &status, WNOHANG) == 0);
	(void) memset(&action, 0, sizeof (action));
	action.sa_handler = handler;
	CHECK(sigemptyset(&action.sa_mask) == 0);
	CHECK(sigaction(SIGALRM, &action, NULL) == 0);
	(void) memset(&timer, 0, sizeof (timer));
	timer.it_value.tv_usec = 10000;
	timer.it_interval.tv_usec = 10000;
	CHECK(setitimer(ITIMER_REAL, &timer, NULL) == 0);
	errno = 0;
	CHECK(waitpid(child, &status, 0) == -1 && errno == EINTR && caught);
	(void) memset(&timer, 0, sizeof (timer));
	CHECK(setitimer(ITIMER_REAL, &timer, NULL) == 0);
	CHECK(close(gate[1]) == 0);
	CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
	    WEXITSTATUS(status) == 37);
	child = -1;
	(void) puts("WAIT_ORDINARY_PASS cases=3");
	return (0);
}
