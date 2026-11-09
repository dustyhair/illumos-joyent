/*
 * CDDL version 1.0, http://www.illumos.org/license/CDDL.
 */
#define	_GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define	CHECK(x, name) do { \
	if (!(x)) { \
		(void) printf("PIDNS_FAIL name=%s errno=%d line=%d\n", \
		    name, errno, __LINE__); \
		return (1); \
	} \
} while (0)

static volatile sig_atomic_t caught;
static volatile sig_atomic_t vfork_shared;

static void
handler(int sig)
{
	if (sig == SIGALRM)
		_exit(124);
	caught = sig;
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

static void *
worker(void *unused)
{
	(void) unused;
	return ((void *)(uintptr_t)(getpid() != 1 ||
	    syscall(SYS_gettid) <= 1 || getpgrp() != 1 || getsid(0) != 1));
}

static int
inside(const char *procpath, pid_t outer)
{
	char path[PATH_MAX], link[64], byte;
	pthread_t thread;
	void *result;
	pid_t child, orphan;
	int status, release[2];
	siginfo_t info;

	CHECK(signal(SIGALRM, handler) != SIG_ERR, "init-alarm-handler");
	(void) alarm(15);
	CHECK(prctl(PR_SET_PDEATHSIG, SIGKILL) == 0, "init-parent-death");
	CHECK(getpid() == 1 && syscall(SYS_gettid) == 1 && getppid() == 0,
	    "init-identities");
	CHECK(raise(SIGTERM) == 0 && raise(SIGUSR1) == 0,
	    "init-default-signals-ignored");
	CHECK(setsid() == 1 && getpgrp() == 1 && getsid(0) == 1,
	    "init-session-identities");
	CHECK(mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) == 0,
	    "private-mount-propagation");
	CHECK(mount("proc", procpath, "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC,
	    NULL) == 0, "mount-namespace-proc");
	CHECK(snprintf(path, sizeof (path), "%s/self", procpath) <
	    (int)sizeof (path), "proc-self-path");
	CHECK(readlink(path, link, sizeof (link)) == 1 && link[0] == '1',
	    "proc-self-id");
	CHECK(snprintf(path, sizeof (path), "%s/%d", procpath, outer) <
	    (int)sizeof (path), "proc-outer-path");
	errno = 0;
	CHECK(access(path, F_OK) == -1 && errno == ENOENT,
	    "proc-hides-outer-parent");
	CHECK(signal(SIGUSR1, handler) != SIG_ERR, "init-caught-signal");
	CHECK((child = fork()) >= 0, "fork-worker");
	if (child == 0) {
		CHECK(getpid() == 2 && getppid() == 1 && getpgrp() == 1 &&
		    getsid(0) == 1, "worker-identities");
		CHECK(kill(1, SIGUSR1) == 0, "worker-signals-init-handler");
		_exit(29);
	}
	CHECK(child == 2, "parent-visible-child-id");
	while (waitid(P_PID, child, &info, WEXITED | WNOWAIT) != 0)
		CHECK(errno == EINTR, "waitid-worker");
	CHECK(info.si_pid == 2 && info.si_code == CLD_EXITED &&
	    info.si_status == 29 && caught == SIGUSR1, "waitid-and-signal");
	CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
	    WEXITSTATUS(status) == 29, "waitpid-worker");
	CHECK(pthread_create(&thread, NULL, worker, NULL) == 0 &&
	    pthread_join(thread, &result) == 0 && result == NULL,
	    "pthread-identities");
	vfork_shared = 0;
	CHECK((child = vfork()) >= 0, "vfork-exec");
	if (child == 0) {
		vfork_shared = 1;
		(void) execl("/bin/true", "true", (char *)NULL);
		_exit(127);
	}
	CHECK(vfork_shared == 1 && waitpid(child, &status, 0) == child &&
	    WIFEXITED(status) && WEXITSTATUS(status) == 0,
	    "vfork-exec-and-shared-address-space");
	vfork_shared = 0;
	CHECK((child = vfork()) >= 0, "vfork-exit");
	if (child == 0) {
		vfork_shared = 2;
		_exit(41);
	}
	CHECK(vfork_shared == 2 && waitpid(child, &status, 0) == child &&
	    WIFEXITED(status) && WEXITSTATUS(status) == 41,
	    "vfork-exit-and-shared-address-space");
	CHECK(pipe(release) == 0 && (child = fork()) >= 0,
	    "fork-orphan-parent");
	if (child == 0) {
		CHECK((orphan = fork()) >= 0, "fork-orphan");
		if (orphan != 0)
			_exit(31);
		CHECK(read(release[0], &byte, 1) == 1 && getppid() == 1,
		    "orphan-adopted-by-namespace-init");
		_exit(43);
	}
	CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
	    WEXITSTATUS(status) == 31, "wait-orphan-parent");
	CHECK(write(release[1], "G", 1) == 1, "release-orphan");
	CHECK((orphan = waitpid(-1, &status, 0)) > 1 && orphan != child &&
	    WIFEXITED(status) && WEXITSTATUS(status) == 43,
	    "namespace-init-reaps-orphan");
	(void) close(release[0]);
	(void) close(release[1]);
	CHECK(umount(procpath) == 0, "unmount-namespace-proc");
	(void) printf("PIDNS_INSIDE_PASS init=1 firstchild=2 orphan=%d\n",
	    orphan);
	return (0);
}

/* Keep a worker alive until PID1 exits, then test namespace shutdown. */
static int
init_death(int release, int report)
{
	FILE *status;
	pid_t child, outer;
	char byte;

	CHECK(signal(SIGALRM, handler) != SIG_ERR, "death-alarm-handler");
	(void) alarm(15);
	CHECK(prctl(PR_SET_PDEATHSIG, SIGKILL) == 0, "death-parent-signal");
	CHECK(getpid() == 1 && getppid() == 0, "death-init-identities");
	CHECK((child = fork()) >= 0, "death-fork-worker");
	if (child == 0) {
		CHECK(getpid() == 2 && getppid() == 1,
		    "death-worker-identities");
		/* The inherited outer proc mount reports the outer PID. */
		CHECK((status = fopen("/proc/self/stat", "r")) != NULL &&
		    fscanf(status, "%d", &outer) == 1, "death-outer-worker-id");
		(void) fclose(status);
		CHECK(write(report, &outer, sizeof (outer)) == sizeof (outer),
		    "death-report-worker");
		for (;;)
			(void) pause();
	}
	CHECK(read(release, &byte, 1) == 1, "death-parent-release");
	_exit(37);
}

int
main(int argc, char **argv)
{
	char text[96], procpath[] = "/tmp/lx-pidns.XXXXXX";
	uid_t uid;
	gid_t gid;
	pid_t before, child;
	int status, death = 0, drop = 0, release[2], report[2];
	pid_t outer_worker;

	(void) setvbuf(stdout, NULL, _IONBF, 0);
	(void) alarm(30);
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--drop") == 0)
			drop = 1;
		else if (strcmp(argv[i], "--init-death") == 0)
			death = 1;
		else
			return (2);
	}
	if (drop)
		CHECK(setgroups(0, NULL) == 0 && setgid(65534) == 0 &&
		    setuid(65534) == 0, "drop-identity");
	uid = getuid();
	gid = getgid();
	before = getpid();
	CHECK(prctl(PR_SET_DUMPABLE, 1UL, 0UL, 0UL, 0UL) == 0,
	    "mapping-dumpable");
	CHECK(mkdtemp(procpath) != NULL, "create-proc-target");
	CHECK(unshare(CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWPID) == 0,
	    "unshare-namespaces-for-child");
	CHECK(put("/proc/self/setgroups", "deny\n") == 0, "deny-setgroups");
	(void) snprintf(text, sizeof (text), "0 %lu 1\n", (unsigned long)uid);
	CHECK(put("/proc/self/uid_map", text) == 0, "uid-map");
	(void) snprintf(text, sizeof (text), "0 %lu 1\n", (unsigned long)gid);
	CHECK(put("/proc/self/gid_map", text) == 0, "gid-map");
	CHECK(getpid() == before && getuid() == 0 && getgid() == 0,
	    "unshare-retains-active-pid-namespace");
	CHECK(pipe(release) == 0 && pipe(report) == 0, "create-death-pipes");
	CHECK((child = fork()) >= 0, "fork-namespace-init");
	if (child == 0)
		_exit(death ? init_death(release[0], report[1]) :
		    inside(procpath, before));
	if (death) {
		CHECK(read(report[0], &outer_worker, sizeof (outer_worker)) ==
		    sizeof (outer_worker) && outer_worker > 1 &&
		    outer_worker != child && kill(outer_worker, 0) == 0,
		    "outer-parent-finds-live-worker");
		CHECK(write(release[1], "G", 1) == 1, "outer-release-init");
	}
	CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
	    WEXITSTATUS(status) == (death ? 37 : 0),
	    "namespace-init-completes");
	if (death) {
		errno = 0;
		CHECK(kill(outer_worker, 0) == -1 && errno == ESRCH,
		    "init-death-removes-worker");
		errno = 0;
		CHECK(fork() == -1 && errno == ENOMEM,
		    "dead-namespace-rejects-fork");
	}
	(void) close(release[0]);
	(void) close(release[1]);
	(void) close(report[0]);
	(void) close(report[1]);
	CHECK(getpid() == before && rmdir(procpath) == 0,
	    "parent-identity-and-mount-retained");
	(void) printf("PIDNS_PASS uid=%lu death=%d\n",
	    (unsigned long)uid, death);
	return (0);
}
