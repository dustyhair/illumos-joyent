/* CDDL version 1.0, http://www.illumos.org/license/CDDL. */
#include <sys/types.h>
#include <sys/procfs.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static volatile sig_atomic_t interrupted;

static void
interrupt(int sig)
{
	interrupted = sig;
}

int
main(int argc, char **argv)
{
	char path[64], *end;
	psinfo_t info;
	pid_t pid, previous;
	int tty, fd, i, result = 1;

	if (argc != 2)
		return (2);
	pid = (pid_t)strtol(argv[1], &end, 10);
	if (*end != 0 || pid <= 1)
		return (2);
	(void) snprintf(path, sizeof (path), "/proc/%ld/psinfo", (long)pid);
	fd = open(path, O_RDONLY);
	if (fd < 0 || read(fd, &info, sizeof (info)) != sizeof (info) ||
	    info.pr_pgid != pid || info.pr_ppid != getppid())
		return (2);
	(void) close(fd);
	if ((tty = open("/dev/tty", O_RDWR)) < 0 ||
	    (previous = tcgetpgrp(tty)) < 0)
		return (2);
	(void) signal(SIGTTOU, SIG_IGN);
	(void) signal(SIGTTIN, SIG_IGN);
	(void) signal(SIGINT, interrupt);
	(void) signal(SIGTERM, interrupt);
	if (tcsetpgrp(tty, pid) == 0 && kill(-pid, SIGCONT) == 0) {
		for (i = 0; i < 300 && !interrupted; i++) {
			fd = open(path, O_RDONLY);
			if (fd < 0) {
				result = errno == ENOENT ? 0 : 1;
				break;
			}
			if (read(fd, &info, sizeof (info)) != sizeof (info)) {
				(void) close(fd);
				break;
			}
			(void) close(fd);
			if (info.pr_lwp.pr_sname == 'Z') {
				result = 0;
				break;
			}
			(void) sleep(1);
		}
	}
	if (tcsetpgrp(tty, previous) != 0)
		result = 1;
	(void) close(tty);
	return (result);
}
