/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Verify that a restricted MAP_32BIT search does not constrain a later,
 * unrestricted address-space search.
 */

#include <sys/mman.h>
#include <sys/types.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define	LOW_MAP_SIZE	((size_t)64 << 20)
#define	LOW_MAP_COUNT	64

int
main(void)
{
	void *low[LOW_MAP_COUNT];
	void *unrestricted;
	size_t nlow;

	for (nlow = 0; nlow < LOW_MAP_COUNT; nlow++) {
		low[nlow] = mmap(NULL, LOW_MAP_SIZE, PROT_NONE,
		    MAP_PRIVATE | MAP_ANON | MAP_NORESERVE | MAP_32BIT, -1, 0);
		if (low[nlow] == MAP_FAILED)
			break;
	}

	if (nlow == 0 || nlow == LOW_MAP_COUNT) {
		(void) fprintf(stderr,
		    "could not establish a full restricted mapping range\n");
		return (EXIT_FAILURE);
	}

	unrestricted = mmap(NULL, LOW_MAP_SIZE, PROT_NONE,
	    MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
	if (unrestricted == MAP_FAILED) {
		(void) fprintf(stderr,
		    "unrestricted mmap after MAP_32BIT failed: %s\n",
		    strerror(errno));
		return (EXIT_FAILURE);
	}

	(void) printf("filled MAP_32BIT with %zu mappings; unrestricted %p\n",
	    nlow, unrestricted);

	if (munmap(unrestricted, LOW_MAP_SIZE) != 0) {
		(void) fprintf(stderr, "munmap failed: %s\n", strerror(errno));
		return (EXIT_FAILURE);
	}
	while (nlow != 0)
		(void) munmap(low[--nlow], LOW_MAP_SIZE);

	return (EXIT_SUCCESS);
}
