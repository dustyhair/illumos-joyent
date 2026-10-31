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

#define	CHECK(x) do { \
	if (!(x)) { \
		(void) printf("PID_DRAIN_FAIL line=%d errno=%d\n", \
		    __LINE__, errno); \
		_exit(1); \
	} \
} while (0)

static void
timeout(int sig)
{
	(void) sig;
	_exit(124);
}

static void
put(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY);

	CHECK(fd >= 0);
	CHECK(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
	CHECK(close(fd) == 0);
}

static void
new_user(void)
{
	char text[80];
	uid_t uid = getuid();
	gid_t gid = getgid();

	CHECK(unshare(CLONE_NEWUSER) == 0);
	put("/proc/self/setgroups", "deny\n");
	(void) snprintf(text, sizeof (text), "0 %u 1\n", uid);
	put("/proc/self/uid_map", text);
	(void) snprintf(text, sizeof (text), "0 %u 1\n", gid);
	put("/proc/self/gid_map", text);
}

/* Linux /proc is still mounted in the outside ancestor's PID namespace. */
static pid_t
outside_pid(int linux_ns)
{
	char text[128];
	int fd;
	ssize_t n;

	if (!linux_ns)
		return (getpid());
	CHECK((fd = open("/proc/self/stat", O_RDONLY)) >= 0);
	CHECK((n = read(fd, text, sizeof (text) - 1)) > 0);
	text[n] = '\0';
	CHECK(close(fd) == 0);
	return ((pid_t)strtol(text, NULL, 10));
}

static void
race_ready(int fd)
{
	CHECK(write(fd, "r", 1) == 1);
	(void) puts("PID_DRAIN_RACE_READY");
}

int
main(int argc, char **argv)
{
	int linux_ns = 0, none = 0, killed = 0, race = 0, ignore = 0;
	int wire[2], ready[2], finish[2], status;
	pid_t init, members[2], done = 0;
	char byte;

	(void) setvbuf(stdout, NULL, _IONBF, 0);
	CHECK(signal(SIGALRM, timeout) != SIG_ERR);
	(void) alarm(60);
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--linux") == 0)
			linux_ns = 1;
		else if (strcmp(argv[i], "--clone-none") == 0)
			none = 1;
		else if (strcmp(argv[i], "--kill") == 0)
			killed = 1;
		else if (strcmp(argv[i], "--race") == 0)
			race = 1;
		else if (strcmp(argv[i], "--ignore") == 0)
			ignore = 1;
		else
			return (2);
	}
	if (linux_ns) {
		new_user();
		CHECK(unshare(CLONE_NEWPID) == 0);
	}
	CHECK(pipe(wire) == 0 && pipe(ready) == 0 &&
	    pipe(finish) == 0);
	CHECK((init = fork()) >= 0);
	if (init == 0) {
		pid_t child;

		(void) alarm(50);
		CHECK(close(wire[0]) == 0);
		(void) printf("PID_PROCESS pid=%d\n", outside_pid(linux_ns));
		if (!linux_ns)
			CHECK(raise(SIGSTOP) == 0);
		if (ignore)
			CHECK(signal(SIGCHLD, SIG_IGN) != SIG_ERR);
		child = none ? syscall(SYS_clone, 0UL, NULL, NULL,
		    NULL, 0UL) : fork();
		CHECK(child >= 0);
		if (child == 0) {
			pid_t descendant, me = outside_pid(linux_ns);

			(void) alarm(45);
			CHECK(write(wire[1], &me, sizeof (me)) == sizeof (me));
			CHECK((descendant = fork()) >= 0);
			if (descendant == 0) {
				me = outside_pid(linux_ns);
				(void) alarm(45);
				CHECK(write(wire[1], &me, sizeof (me)) ==
				    sizeof (me));
				CHECK(write(ready[1], "g", 1) == 1);
				CHECK(raise(SIGSTOP) == 0);
				for (;;)
					(void) pause();
			}
			CHECK(raise(SIGSTOP) == 0);
			if (race) {
				int announced = 0;

				for (;;) {
					pid_t p = fork();

					if (p == 0)
						_exit(0);
					if (p > 0) {
						while (waitpid(p, &status,
						    0) < 0 && errno == EINTR)
							;
						if (!announced) {
							race_ready(ready[1]);
							announced = 1;
						}
					}
				}
			}
			for (;;)
				(void) pause();
		}
		CHECK(read(ready[0], &byte, 1) == 1);
		CHECK(write(wire[1], &done, sizeof (done)) == sizeof (done));
		if (linux_ns)
			CHECK(read(finish[0], &byte, 1) == 1);
		else
			CHECK(raise(SIGSTOP) == 0);
		_exit(0);
	}
	CHECK(close(wire[1]) == 0);
	for (int i = 0; i < 2; i++)
		CHECK(read(wire[0], &members[i], sizeof (members[i])) ==
		    sizeof (members[i]));
	CHECK(read(wire[0], &done, sizeof (done)) == sizeof (done) &&
	    done == 0);
	if (!linux_ns)
		CHECK(waitpid(init, &status, WUNTRACED) == init &&
		    WIFSTOPPED(status));
	(void) printf("PID_CHILD pid=%d\n", members[0]);
	(void) printf("PID_DESCENDANT pid=%d\n", members[1]);
	(void) printf("PID_DRAIN_READY ancestor=%d\n", getpid());
	if (!linux_ns)
		CHECK(raise(SIGSTOP) == 0);
	/* No forks precede the observed maps 2 and 3. */
	if (linux_ns && race)
		CHECK(kill(members[0], SIGCONT) == 0);
	if (race)
		CHECK(read(ready[0], &byte, 1) == 1 && byte == 'r');
	if (linux_ns && !killed)
		CHECK(write(finish[1], "x", 1) == 1);
	else
		CHECK(kill(init, killed ? SIGKILL : SIGCONT) == 0);
	CHECK(waitpid(init, &status, 0) == init);
	CHECK(killed ? WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL :
	    WIFEXITED(status) && WEXITSTATUS(status) == 0);
	/* LX checks held native identities in process_drained. */
	if (linux_ns) {
		for (int i = 0; i < 2; i++) {
			errno = 0;
			CHECK(kill(members[i], 0) == -1 && errno == ESRCH);
		}
	}
	(void) puts("PID_DRAIN_PASS ancestor_wait_all_gone=2");
	return (0);
}
