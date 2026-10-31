/*
 * CDDL version 1.0, http://www.illumos.org/license/CDDL.
 */
#define	_GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define	CHECK(x) do { if (!(x)) { \
	(void) printf("NEST_FAIL line=%d errno=%d\n", __LINE__, errno); \
	_exit(1); } } while (0)
static int linux_ns, clone0, mode;
static int events[2], inner_go[2], outer_go[2];
struct event { int role; pid_t pid; };
static pid_t ids[6];
/* Outside proc mount deliberately remains visible in both PID namespaces. */
static pid_t
global_pid(void)
{
	char buf[128];
	int fd;
	ssize_t n;

	if (!linux_ns)
		return (getpid());
	CHECK((fd = open("/proc/self/stat", O_RDONLY)) >= 0);
	CHECK((n = read(fd, buf, sizeof (buf) - 1)) > 0);
	buf[n] = 0;
	CHECK(close(fd) == 0);
	return ((pid_t)strtol(buf, NULL, 10));
}
static void
emit(int role)
{
	struct event e = { role, global_pid() };
	CHECK(write(events[1], &e, sizeof (e)) == sizeof (e));
}
static void
put(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY);
	CHECK(fd >= 0);
	CHECK(write(fd, s, strlen(s)) == (ssize_t)strlen(s));
	CHECK(close(fd) == 0);
}
static void
userns(void)
{
	char buf[80];
	uid_t u = getuid();
	gid_t g = getgid();
	CHECK(unshare(CLONE_NEWUSER) == 0);
	put("/proc/self/setgroups", "deny\n");
	(void) snprintf(buf, sizeof (buf), "0 %u 1\n", u);
	put("/proc/self/uid_map", buf);
	(void) snprintf(buf, sizeof (buf), "0 %u 1\n", g);
	put("/proc/self/gid_map", buf);
}
static void
park(void)
{
	for (;;)
		(void) pause();
}
static void
gone(int first, int last)
{
	for (int i = first; i <= last; i++) {
		errno = 0;
		CHECK(kill(ids[i], 0) == -1 && errno == ESRCH);
	}
}
static void
inner(void)
{
	pid_t parent;
	int status;
	char c;

	emit(2);
	if (!linux_ns)
		CHECK(raise(SIGSTOP) == 0);
	CHECK((parent = fork()) >= 0);
	if (parent == 0) {
		pid_t live, zombie;
		siginfo_t info;

		live = clone0 ? syscall(SYS_clone, 0UL, NULL, NULL,
		    NULL, 0UL) : fork();
		CHECK(live >= 0);
		if (live == 0) {
			emit(3);
			CHECK(raise(SIGSTOP) == 0);
			park();
		}
		CHECK(waitpid(live, &status, WUNTRACED | __WALL) == live &&
		    WIFSTOPPED(status));
		zombie = clone0 ? syscall(SYS_clone, 0UL, NULL, NULL,
		    NULL, 0UL) : fork();
		CHECK(zombie >= 0);
		if (zombie == 0) {
			emit(4);
			_exit(23);
		}
		CHECK(waitid(P_PID, zombie, &info,
		    WEXITED | WNOWAIT | __WALL) == 0);
		CHECK(info.si_pid == zombie && info.si_code == CLD_EXITED &&
		    info.si_status == 23);
		/* Exiting now gives inner PID1 live and zombie adoptees. */
		_exit(0);
	}
	CHECK(waitpid(parent, &status, 0) == parent &&
	    WIFEXITED(status) && WEXITSTATUS(status) == 0);
	emit(5);
	CHECK(read(inner_go[0], &c, 1) == 1);
	_exit(0);
}
static void
outer(void)
{
	pid_t helper, child;
	int status;
	char c;

	emit(0);
	if (!linux_ns)
		CHECK(raise(SIGSTOP) == 0);
	CHECK((helper = fork()) >= 0);
	if (helper == 0) {
		emit(1);
		if (linux_ns)
			CHECK(unshare(CLONE_NEWPID) == 0);
		else
			CHECK(raise(SIGSTOP) == 0);
		CHECK((child = fork()) >= 0);
		if (child == 0)
			inner();
		CHECK(waitpid(child, &status, 0) == child);
		CHECK(mode == 1 ? WIFSIGNALED(status) &&
		    WTERMSIG(status) == SIGKILL : WIFEXITED(status) &&
		    WEXITSTATUS(status) == 0);
		emit(6);
		park();
	}
	CHECK(read(outer_go[0], &c, 1) == 1);
	_exit(0);
}
int
main(int argc, char **argv)
{
	int status;
	pid_t init;
	struct event e;

	(void) setvbuf(stdout, NULL, _IONBF, 0);
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--linux") == 0)
			linux_ns = 1;
		else if (strcmp(argv[i], "--clone0") == 0)
			clone0 = 1;
		else if (strcmp(argv[i], "--kill-inner") == 0)
			mode = 1;
		else if (strcmp(argv[i], "--kill-outer") == 0)
			mode = 2;
		else
			return (2);
	}
	CHECK(pipe(events) == 0 && pipe(inner_go) == 0 &&
	    pipe(outer_go) == 0);
	if (linux_ns) {
		userns();
		CHECK(unshare(CLONE_NEWPID) == 0);
	}
	CHECK((init = fork()) >= 0);
	if (init == 0)
		outer();
	for (;;) {
		CHECK(read(events[0], &e, sizeof (e)) ==
		    sizeof (e));
		CHECK(e.role >= 0 && e.role <= 6);
		if (e.role <= 4) {
			ids[e.role] = e.pid;
			(void) printf("NEST_ROLE role=%d pid=%d\n",
			    e.role, e.pid);
			continue;
		}
		if (e.role == 5)
			break;
		CHECK(0);
	}
	(void) printf("NEST_READY observer=%d\n", getpid());
	if (!linux_ns)
		CHECK(raise(SIGSTOP) == 0);
	if (mode == 2) {
		CHECK(kill(init, SIGKILL) == 0);
	} else {
		if (mode == 1)
			CHECK(kill(ids[2], SIGKILL) == 0);
		else
			CHECK(write(inner_go[1], "x", 1) == 1);
		CHECK(read(events[0], &e, sizeof (e)) == sizeof (e) &&
		    e.role == 6);
		/* Exactly one immediate check after helper's native wait. */
		gone(2, 4);
		CHECK(kill(ids[0], 0) == 0 && kill(ids[1], 0) == 0);
		(void) puts("NEST_INNER_WAIT peers_alive=2");
		if (!linux_ns)
			CHECK(raise(SIGSTOP) == 0);
		CHECK(write(outer_go[1], "x", 1) == 1);
	}
	CHECK(waitpid(init, &status, 0) == init);
	CHECK(mode == 2 ? WIFSIGNALED(status) &&
	    WTERMSIG(status) == SIGKILL :
	    WIFEXITED(status) && WEXITSTATUS(status) == 0);
	gone(0, 4);
	(void) puts("NEST_PASS outside_wait_all_gone=5");
	return (0);
}
