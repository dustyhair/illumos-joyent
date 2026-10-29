/* CDDL version 1.0, http://www.illumos.org/license/CDDL. */
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Minimal Linux PID 1 for the offline lx regression-test root. */
int
main(void)
{
	for (;;) {
		int status;
		while (waitpid(-1, &status, WNOHANG) > 0)
			;
		(void) sleep(1);
	}
}
