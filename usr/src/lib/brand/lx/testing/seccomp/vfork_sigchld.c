/*
 * CDDL version 1.0, http://www.illumos.org/license/CDDL.
 */
#define	_GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t sigchld_count;

static void
sigchld_handler(int signal)
{
	(void) signal;
	sigchld_count++;
}

int
main(void)
{
	struct sigaction action = { 0 };
	int iteration;

	action.sa_handler = sigchld_handler;
	(void) sigemptyset(&action.sa_mask);
	if (sigaction(SIGCHLD, &action, NULL) != 0) {
		perror("sigaction");
		return (1);
	}

	for (iteration = 0; iteration < 1000; iteration++) {
		pid_t child;
		int status;

		child = vfork();
		if (child < 0) {
			perror("vfork");
			return (1);
		}
		if (child == 0) {
			execl("/definitely-not-present",
			    "definitely-not-present", (char *)NULL);
			_exit(127);
		}
		if (waitpid(child, &status, 0) != child) {
			perror("waitpid");
			return (1);
		}
		if (!WIFEXITED(status) || WEXITSTATUS(status) != 127) {
			(void) fprintf(stderr,
			    "bad child status at iteration %d\n", iteration);
			return (1);
		}
	}

	(void) printf("PASS: 1000 vfork failures, SIGCHLD count %d\n",
	    (int)sigchld_count);
	return (sigchld_count == 1000 ? 0 : 1);
}
