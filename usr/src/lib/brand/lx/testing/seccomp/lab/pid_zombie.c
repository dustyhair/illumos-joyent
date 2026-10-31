/*
 * CDDL version 1.0, http://www.illumos.org/license/CDDL.
 */
#define	_GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define	CHECK(x) do { \
	if (!(x)) { \
		(void) printf("PID_ORPHAN_FAIL line=%d errno=%d\n", \
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

static int
put(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY);
	ssize_t n;

	if (fd < 0)
		return (-1);
	n = write(fd, text, strlen(text));
	(void) close(fd);
	return (n == (ssize_t)strlen(text) ? 0 : -1);
}

static void
new_user(void)
{
	char text[96];
	uid_t uid = getuid();
	gid_t gid = getgid();

	CHECK(prctl(PR_SET_DUMPABLE, 1UL, 0UL, 0UL, 0UL) == 0);
	CHECK(unshare(CLONE_NEWUSER) == 0);
	CHECK(put("/proc/self/setgroups", "deny\n") == 0);
	(void) snprintf(text, sizeof (text), "0 %lu 1\n", (unsigned long)uid);
	CHECK(put("/proc/self/uid_map", text) == 0);
	(void) snprintf(text, sizeof (text), "0 %lu 1\n", (unsigned long)gid);
	CHECK(put("/proc/self/gid_map", text) == 0);
}

static void
caught(int sig)
{
	(void) sig;
}

int
main(int argc, char **argv)
{
	int linux_ns = 0, guard = 0, drop = 0, group = 0, report[2], status;
	int clone_none = 0, zombie = 0;
	struct sigaction action;
	pid_t bootstrap, owner, child, descendant;

	(void) setvbuf(stdout, NULL, _IONBF, 0);
	CHECK(signal(SIGALRM, timeout) != SIG_ERR);
	(void) alarm(60);
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--linux") == 0)
			linux_ns = 1;
		else if (strcmp(argv[i], "--guard") == 0)
			guard = 1;
		else if (strcmp(argv[i], "--drop") == 0)
			drop = 1;
		else if (strcmp(argv[i], "--group") == 0)
			group = 1;
		else if (strcmp(argv[i], "--clone-none") == 0)
			clone_none = 1;
		else if (strcmp(argv[i], "--zombie") == 0)
			zombie = 1;
		else
			return (2);
	}
	if (drop)
		CHECK(setgroups(0, NULL) == 0 && setgid(65534) == 0 &&
		    setuid(65534) == 0);
	if (guard || linux_ns)
		new_user();
	if (linux_ns)
		CHECK(unshare(CLONE_NEWPID) == 0);
	/* A fresh child can become a session leader even under zlogin. */
	CHECK((bootstrap = fork()) >= 0);
	if (bootstrap != 0) {
		CHECK(waitpid(bootstrap, &status, 0) == bootstrap);
		CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
		return (0);
	}
	(void) alarm(60);
	owner = getpid();
	CHECK(setsid() == owner && getsid(0) == owner && getpgrp() == owner);
	(void) printf("PID_PROCESS pid=%d tid=%d mode=orphan group=%d "
	    "clone_none=%d\n",
	    owner, owner, group, clone_none);
	if (!linux_ns)
		CHECK(raise(SIGSTOP) == 0);
	if (zombie) {
		(void) memset(&action, 0, sizeof (action));
		action.sa_handler = caught;
		action.sa_flags = SA_NOCLDSTOP;
		CHECK(sigemptyset(&action.sa_mask) == 0);
		CHECK(sigaction(SIGCHLD, &action, NULL) == 0);
	}
	CHECK(pipe(report) == 0 && (child = fork()) >= 0);
	if (child == 0) {
		pid_t parent = getpid();

		(void) alarm(60);
		if (group)
			CHECK(setpgid(0, 0) == 0);
		descendant = clone_none ?
		    syscall(SYS_clone, 0UL, NULL, NULL, NULL, 0UL) : fork();
		CHECK(descendant >= 0);
		if (descendant == 0) {
			(void) alarm(60);
			if (!group)
				CHECK(setpgid(0, 0) == 0);
			CHECK(getsid(0) == owner && getpgrp() != owner);
			(void) printf("PID_DESCENDANT pid=%d parent=%d\n",
			    getpid(), getppid());
			CHECK(raise(SIGSTOP) == 0);
			CHECK(zombie || getppid() == owner);
			(void) printf("PID_ORPHAN_EXIT pid=%d parent=%d\n",
			    getpid(), getppid());
			_exit(43);
		}
		CHECK(waitpid(descendant, &status, WUNTRACED | __WALL) ==
		    descendant &&
		    WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP);
		(void) printf("PID_CHILD pid=%d parent=%d descendant=%d\n",
		    parent, getppid(), descendant);
		CHECK(write(report[1], &descendant, sizeof (descendant)) ==
		    sizeof (descendant));
		CHECK(raise(SIGSTOP) == 0);
		if (linux_ns && zombie) {
			siginfo_t info;

			(void) memset(&info, 0, sizeof (info));
			CHECK(waitid(P_PID, descendant, &info,
			    WEXITED | WNOWAIT | __WALL) == 0);
			CHECK(info.si_pid == descendant &&
			    info.si_code == CLD_EXITED && info.si_status == 43);
		}
		_exit(31);
	}
	CHECK(read(report[0], &descendant, sizeof (descendant)) ==
	    sizeof (descendant));
	(void) printf("PID_PARENT pid=%d child=%d descendant=%d\n",
	    owner, child, descendant);
	if (linux_ns) {
		CHECK(waitpid(child, &status, WUNTRACED) == child &&
		    WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP);
		if (zombie)
			CHECK(kill(descendant, SIGCONT) == 0);
		CHECK(kill(child, SIGCONT) == 0);
	} else {
		CHECK(raise(SIGSTOP) == 0);
	}
	CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
	    WEXITSTATUS(status) == 31);
	if (linux_ns && !zombie) {
		CHECK(waitpid(descendant, &status, WNOHANG) == 0);
		CHECK(kill(descendant, SIGCONT) == 0);
	}
	CHECK(waitpid(descendant, &status, 0) == descendant &&
	    WIFEXITED(status) && WEXITSTATUS(status) == 43);
	(void) printf("PID_ADOPT_PASS owner=%d child=%d descendant=%d\n",
	    owner, child, descendant);
	(void) printf("PID_RETIRED pid=%d tid=%d mode=orphan\n", owner, owner);
	if (!linux_ns)
		CHECK(raise(SIGSTOP) == 0);
	(void) printf("PID_PASS\n");
	return (0);
}
