/* Ordinary POSIX queue/kill control. Does not test supplied Linux errno. */
#define	_POSIX_C_SOURCE	200809L
#include <sys/types.h>
#include <signal.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
fail(const char *what)
{
	(void) fprintf(stderr, "FAIL %s errno=%d\n", what, errno);
	exit(1);
}

static void
empty(sigset_t *set)
{
	struct timespec zero = { 0, 0 };
	siginfo_t si;
	sigset_t pending;

	if (sigtimedwait(set, &si, &zero) != -1 || errno != EAGAIN)
		fail("extra signal");
	if (sigpending(&pending) != 0 || sigismember(&pending, SIGUSR1) != 0)
		fail("extra pending signal");
}

int
main(void)
{
	sigset_t set, old;
	struct sigaction sa;
	struct timespec limit = { 2, 0 };
	union sigval value;
	siginfo_t si;
	unsigned int i;

	/* Request native queue metadata even for blocked kill. */
	(void) memset(&sa, 0, sizeof (sa));
	sa.sa_handler = SIG_DFL;
	sa.sa_flags = SA_SIGINFO;
	if (sigemptyset(&sa.sa_mask) != 0 ||
	    sigaction(SIGUSR1, &sa, NULL) != 0 || sigemptyset(&set) != 0 ||
	    sigaddset(&set, SIGUSR1) != 0 ||
	    sigprocmask(SIG_BLOCK, &set, &old) != 0)
		fail("setup");
	(void) alarm(15);
	empty(&set);
	/* Exercise repeated native pool allocation and reuse. */
	for (i = 0; i < 128; i++) {
		value.sival_ptr = (void *)(uintptr_t)UINT32_C(0xfedcba98);
		if (sigqueue(getpid(), SIGUSR1, value) != 0)
			fail("sigqueue");
		(void) memset(&si, 0xa5, sizeof (si));
		if (sigtimedwait(&set, &si, &limit) != SIGUSR1 ||
		    si.si_signo != SIGUSR1 || si.si_code != SI_QUEUE ||
		    si.si_errno != 0 || si.si_pid != getpid() ||
		    si.si_uid != getuid() ||
		    si.si_value.sival_ptr != value.sival_ptr)
			fail("queued metadata");
		empty(&set);
		if (kill(getpid(), SIGUSR1) != 0)
			fail("kill");
		(void) memset(&si, 0xa5, sizeof (si));
		if (sigtimedwait(&set, &si, &limit) != SIGUSR1 ||
		    si.si_signo != SIGUSR1 || si.si_code != SI_USER ||
		    si.si_errno != 0 || si.si_pid != getpid() ||
		    si.si_uid != getuid())
			fail("kill metadata");
		empty(&set);
	}
	(void) alarm(0);
	if (sigprocmask(SIG_SETMASK, &old, NULL) != 0)
		fail("restore mask");
	(void) printf("PASS native_errno bits=%u "
	    "queue=128 kill=128 empty=257\n",
	    (unsigned int)(sizeof (void *) * 8));
	return (0);
}
