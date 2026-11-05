/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/* Copyright 2026 Jonathan Wagner. */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void
usage(const char *name)
{
	(void) fprintf(stderr, "usage: %s -t seconds lock command [arg ...]\n",
	    name);
	exit(2);
}

int
main(int argc, char **argv)
{
	struct flock lock = { 0 };
	struct timespec pause = { 0, 100000000 };
	char *end;
	long timeout;
	int fd, attempts;

	if (argc < 5 || strcmp(argv[1], "-t") != 0)
		usage(argv[0]);
	errno = 0;
	timeout = strtol(argv[2], &end, 10);
	if (errno != 0 || *end != '\0' || timeout < 0 || timeout > 3600)
		usage(argv[0]);

	if ((fd = open(argv[3], O_RDWR | O_CREAT, 0600)) == -1) {
		(void) fprintf(stderr, "%s: cannot open %s: %s\n", argv[0],
		    argv[3], strerror(errno));
		return (1);
	}
	lock.l_type = F_WRLCK;
	lock.l_whence = SEEK_SET;
	attempts = (int)timeout * 10;
	for (;;) {
		if (fcntl(fd, F_SETLK, &lock) == 0)
			break;
		if (errno != EACCES && errno != EAGAIN) {
			(void) fprintf(stderr, "%s: cannot lock %s: %s\n",
			    argv[0], argv[3], strerror(errno));
			return (1);
		}
		if (attempts-- == 0) {
			(void) fprintf(stderr, "%s: timed out locking %s\n",
			    argv[0], argv[3]);
			return (1);
		}
		(void) nanosleep(&pause, NULL);
	}

	(void) ftruncate(fd, 0);
	(void) dprintf(fd, "%ld\n", (long)getpid());
	if (execvp(argv[4], &argv[4]) == -1) {
		(void) fprintf(stderr, "%s: cannot execute %s: %s\n", argv[0],
		    argv[4], strerror(errno));
	}
	return (1);
}
