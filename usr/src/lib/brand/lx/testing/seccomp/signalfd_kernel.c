#define	_GNU_SOURCE

#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <sys/types.h>

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void
fail(const char *what)
{
	perror(what);
	exit(EXIT_FAILURE);
}

static void
consume(int fd, int signo)
{
	struct signalfd_siginfo info;
	ssize_t n;

	if (kill(getpid(), signo) != 0)
		fail("kill");
	n = read(fd, &info, sizeof (info));
	if (n != sizeof (info))
		fail("read signalfd");
	if (info.ssi_signo != (uint32_t)signo) {
		(void) fprintf(stderr, "wrong signal: got %u expected %d\n",
		    info.ssi_signo, signo);
		exit(EXIT_FAILURE);
	}
}

int
main(void)
{
	sigset_t mask;
	uint64_t bits = 0;
	int fd, flags;

	(void) sigemptyset(&mask);
	(void) sigaddset(&mask, SIGUSR1);
	(void) sigaddset(&mask, SIGUSR2);
	if (sigprocmask(SIG_BLOCK, &mask, NULL) != 0)
		fail("sigprocmask");
	if (unshare(CLONE_NEWNS) != 0)
		fail("unshare mount namespace");

	(void) sigemptyset(&mask);
	(void) sigaddset(&mask, SIGUSR1);
	fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
	if (fd == -1)
		fail("signalfd4 create");
	flags = fcntl(fd, F_GETFL);
	if (flags == -1 || (flags & O_NONBLOCK) == 0) {
		errno = EINVAL;
		fail("signalfd4 nonblock");
	}
	flags = fcntl(fd, F_GETFD);
	if (flags == -1 || (flags & FD_CLOEXEC) == 0) {
		errno = EINVAL;
		fail("signalfd4 cloexec");
	}
	consume(fd, SIGUSR1);

	(void) sigemptyset(&mask);
	(void) sigaddset(&mask, SIGUSR2);
	if (signalfd(fd, &mask, 0) != fd)
		fail("signalfd4 update");
	consume(fd, SIGUSR2);

	errno = 0;
	if (signalfd(-1, &mask, 0x40000000) != -1 || errno != EINVAL) {
		errno = EINVAL;
		fail("signalfd4 bad flags");
	}
	errno = 0;
	if (syscall(SYS_signalfd4, -1, &bits, sizeof (bits) - 1, 0) != -1 ||
	    errno != EINVAL) {
		errno = EINVAL;
		fail("signalfd4 bad size");
	}

	if (close(fd) != 0)
		fail("close");
	(void) puts("PASS: kernel signalfd4 in mount namespace");
	return (EXIT_SUCCESS);
}
