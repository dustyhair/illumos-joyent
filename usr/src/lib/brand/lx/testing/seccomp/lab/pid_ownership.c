/*
 * CDDL version 1.0, http://www.illumos.org/license/CDDL.
 */
#define	_GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *self, *mode;
static int guard, drop;

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

static int
prepare(void)
{
	uid_t uid;
	gid_t gid;
	char text[96];

	if (drop && (setgroups(0, NULL) || setgid(65534) || setuid(65534)))
		return (1);
	if (!guard)
		return (0);
	uid = getuid();
	gid = getgid();
	if (prctl(PR_SET_DUMPABLE, 1UL, 0UL, 0UL, 0UL) ||
	    unshare(CLONE_NEWUSER) || put("/proc/self/setgroups", "deny\n"))
		return (1);
	(void) snprintf(text, sizeof (text), "0 %lu 1\n", (unsigned long)uid);
	if (put("/proc/self/uid_map", text))
		return (1);
	(void) snprintf(text, sizeof (text), "0 %lu 1\n", (unsigned long)gid);
	return (put("/proc/self/gid_map", text) != 0);
}

static void *
worker(void *unused)
{
	(void) unused;
	(void) printf("PID_WORKER pid=%d tid=%ld mode=%s\n",
	    getpid(), syscall(SYS_gettid), mode);
	if (raise(SIGSTOP))
		_exit(3);
	if (strcmp(mode, "exec") == 0) {
		char *const args[] = { (char *)self, "image", NULL };

		execv(self, args);
		perror("worker exec");
		_exit(4);
	}
	return (NULL);
}

int
main(int argc, char **argv)
{
	pthread_t thread;
	int error;

	(void) setvbuf(stdout, NULL, _IONBF, 0);
	(void) alarm(120);
	if (argc < 2)
		return (2);
	self = argv[0];
	mode = argv[1];
	if (strcmp(mode, "image") == 0)
		goto retired;
	if (strcmp(mode, "exit") != 0 && strcmp(mode, "exec") != 0 &&
	    strcmp(mode, "fork") != 0 && strcmp(mode, "death") != 0 &&
	    strcmp(mode, "roles") != 0)
		return (2);
	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--guard") == 0)
			guard = 1;
		else if (strcmp(argv[i], "--drop") == 0)
			drop = 1;
		else
			return (2);
	}
	if (prepare()) {
		perror("prepare");
		return (3);
	}
	(void) printf("PID_PROCESS pid=%d tid=%ld mode=%s\n",
	    getpid(), syscall(SYS_gettid), mode);
	if (raise(SIGSTOP))
		return (1);
	if (strcmp(mode, "roles") == 0) {
		pid_t leader = fork();
		int status;

		if (leader < 0) {
			perror("role leader fork");
			return (1);
		}
		if (leader == 0) {
			pid_t member;

			if (setsid() < 0) {
				perror("leader setsid");
				_exit(4);
			}
			member = fork();
			if (member < 0) {
				perror("role member fork");
				_exit(5);
			}
			if (member == 0) {
				(void) printf(
				    "PID_ROLE_MEMBER pid=%d leader=%d "
				    "pgid=%d sid=%d\n", getpid(), getppid(),
				    getpgrp(), getsid(0));
				if (raise(SIGSTOP) || setpgid(0, 0) != 0)
					_exit(6);
				(void) printf("PID_ROLE_SPLIT pid=%d pgid=%d "
				    "sid=%d\n", getpid(), getpgrp(), getsid(0));
				if (raise(SIGSTOP))
					_exit(7);
				(void) printf("PID_ROLE_MEMBER_EXIT pid=%d\n",
				    getpid());
				_exit(29);
			}
			(void) printf("PID_ROLE_LEADER pid=%d member=%d "
			    "pgid=%d sid=%d\n", getpid(), member, getpgrp(),
			    getsid(0));
			if (raise(SIGSTOP))
				_exit(8);
			_exit(31);
		}
		(void) printf("PID_ROLE_OWNER pid=%d leader=%d\n", getpid(),
		    leader);
		if (raise(SIGSTOP) || waitpid(leader, &status, 0) != leader ||
		    !WIFEXITED(status) || WEXITSTATUS(status) != 31)
			return (1);
		(void) printf("PID_ROLE_LEADER_REAPED pid=%d\n", leader);
		if (raise(SIGSTOP))
			return (1);
		leader = waitpid(-1, &status, 0);
		if (leader < 0 || !WIFEXITED(status) ||
		    WEXITSTATUS(status) != 29)
			return (1);
		(void) printf("PID_ROLE_MEMBER_REAPED pid=%d\n", leader);
		if (raise(SIGSTOP))
			return (1);
		goto retired;
	}
	if (strcmp(mode, "fork") == 0 || strcmp(mode, "death") == 0) {
		pid_t parent = getpid(), child = fork();
		int status;

		if (child < 0) {
			perror("fork");
			return (1);
		}
		if (child == 0) {
			if (strcmp(mode, "death") == 0) {
				pid_t descendant;

				if (setsid() < 0 || (descendant = fork()) < 0)
					_exit(1);
				if (descendant == 0) {
					if (setsid() < 0)
						_exit(1);
					(void) printf("PID_DESCENDANT pid=%d "
					    "parent=%d\n", getpid(), getppid());
				} else {
					(void) printf("PID_CHILD pid=%d "
					    "parent=%d descendant=%d\n",
					    getpid(),
					    getppid(), descendant);
				}
				/* No parent-death signal is configured. */
				if (raise(SIGSTOP))
					_exit(1);
				_exit(99);
			}
			if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 ||
			    getppid() != parent)
				_exit(1);
			(void) printf("PID_CHILD pid=%d parent=%d\n",
			    getpid(), getppid());
			if (raise(SIGSTOP))
				_exit(1);
			(void) printf("PID_CHILD_EXIT pid=%d\n", getpid());
			_exit(29);
		}
		(void) printf("PID_PARENT pid=%d child=%d\n", getpid(), child);
		if (raise(SIGSTOP) || waitpid(child, &status, 0) != child ||
		    !WIFEXITED(status) || WEXITSTATUS(status) != 29)
			return (1);
		(void) printf("PID_WAIT_PASS child=%d\n", child);
		goto retired;
	}
	if ((error = pthread_create(&thread, NULL, worker, NULL)) != 0 ||
	    (error = pthread_join(thread, NULL)) != 0) {
		(void) printf("PID_FAIL pthread=%d\n", error);
		return (1);
	}
retired:
	(void) printf("PID_RETIRED pid=%d tid=%ld mode=%s\n",
	    getpid(), syscall(SYS_gettid), mode);
	if (getpid() != syscall(SYS_gettid) || raise(SIGSTOP))
		return (1);
	(void) printf("PID_PASS\n");
	return (0);
}
