#define	_GNU_SOURCE

#include <sys/timerfd.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static void
fail(const char *what)
{
	perror(what);
	exit(EXIT_FAILURE);
}

int
main(void)
{
	struct itimerspec value, old, current;
	struct pollfd pfd;
	uint64_t expirations;
	int fd, flags, nullfd;

	if (unshare(CLONE_NEWNS) != 0)
		fail("unshare mount namespace");
	fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (fd == -1)
		fail("timerfd_create");
	flags = fcntl(fd, F_GETFL);
	if (flags == -1 || (flags & O_NONBLOCK) == 0) {
		errno = EINVAL;
		fail("timerfd nonblock");
	}
	flags = fcntl(fd, F_GETFD);
	if (flags == -1 || (flags & FD_CLOEXEC) == 0) {
		errno = EINVAL;
		fail("timerfd cloexec");
	}
	if (timerfd_gettime(fd, &current) != 0)
		fail("timerfd_gettime initial");

	value.it_interval.tv_sec = 0;
	value.it_interval.tv_nsec = 0;
	value.it_value.tv_sec = 0;
	value.it_value.tv_nsec = 50000000;
	if (timerfd_settime(fd, 0, &value, &old) != 0)
		fail("timerfd_settime");
	if (old.it_value.tv_sec != 0 || old.it_value.tv_nsec != 0) {
		errno = EINVAL;
		fail("timerfd old value");
	}
	pfd.fd = fd;
	pfd.events = POLLIN;
	pfd.revents = 0;
	if (poll(&pfd, 1, 2000) != 1 || (pfd.revents & POLLIN) == 0) {
		errno = ETIMEDOUT;
		fail("timerfd poll");
	}
	if (read(fd, &expirations, sizeof (expirations)) !=
	    sizeof (expirations) || expirations == 0)
		fail("timerfd read");
	if (timerfd_gettime(fd, &current) != 0)
		fail("timerfd_gettime final");

	errno = 0;
	if (timerfd_create(CLOCK_PROCESS_CPUTIME_ID, 0) != -1 ||
	    errno != EINVAL) {
		errno = EINVAL;
		fail("timerfd bad clock");
	}
	errno = 0;
	if (timerfd_create(CLOCK_MONOTONIC, 0x40000000) != -1 ||
	    errno != EINVAL) {
		errno = EINVAL;
		fail("timerfd bad flags");
	}
	nullfd = open("/dev/null", O_RDONLY);
	if (nullfd == -1)
		fail("open /dev/null");
	errno = 0;
	flags = timerfd_gettime(nullfd, &current);
	if (flags != -1 || errno != EINVAL) {
		(void) fprintf(stderr, "non-timer result=%d errno=%d\n",
		    flags, errno);
		fail("timerfd non-timer descriptor");
	}
	(void) close(nullfd);
	if (close(fd) != 0)
		fail("close");
	(void) puts("PASS: kernel timerfd in mount namespace");
	return (EXIT_SUCCESS);
}
