/* Ordinary pthread/LWP creation baseline, shared by Linux and native illumos. */
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

static void *
worker(void *arg)
{
	return (arg);
}

int
main(void)
{
	unsigned i;
	pthread_t thread;
	void *value;
	int error;

	(void) alarm(20);
	for (i = 1; i <= 256; i++) {
		error = pthread_create(&thread, NULL, worker, (void *)(uintptr_t)i);
		if (error != 0) {
			(void) fprintf(stderr, "create %u error=%d\n", i, error);
			return (1);
		}
		error = pthread_join(thread, &value);
		if (error != 0 || value != (void *)(uintptr_t)i) {
			(void) fprintf(stderr, "join %u error=%d\n", i, error);
			return (1);
		}
	}
	(void) puts("ordinary LWP baseline PASS count=256");
	return (0);
}
