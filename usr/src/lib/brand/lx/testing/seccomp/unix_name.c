/*
 * CDDL version 1.0, http://www.illumos.org/license/CDDL.
 */
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define	ARRAY_SIZE(a)	(sizeof (a) / sizeof ((a)[0]))

static int
check_name(const char *operation, int fd, socklen_t supplied)
{
	struct sockaddr_un addr;
	sa_family_t family = AF_UNIX;
	socklen_t len = supplied;
	unsigned char *bytes = (unsigned char *)&addr;
	unsigned char *family_bytes = (unsigned char *)&family;
	size_t copied;
	size_t i;
	int rc;

	(void) memset(&addr, 0xa5, sizeof (addr));
	if (strcmp(operation, "getpeername") == 0) {
		rc = getpeername(fd, (struct sockaddr *)&addr, &len);
	} else {
		rc = getsockname(fd, (struct sockaddr *)&addr, &len);
	}

	if (rc != 0) {
		(void) fprintf(stderr, "%s(%u) failed: %s\n", operation,
		    (unsigned)supplied, strerror(errno));
		return (1);
	}
	if (len != sizeof (sa_family_t)) {
		(void) fprintf(stderr, "%s(%u) returned length %u, expected %u\n",
		    operation, (unsigned)supplied, (unsigned)len,
		    (unsigned)sizeof (sa_family_t));
		return (1);
	}

	copied = supplied < len ? supplied : len;
	for (i = 0; i < copied; i++) {
		if (bytes[i] != family_bytes[i]) {
			(void) fprintf(stderr,
			    "%s(%u) returned bad family byte %zu\n", operation,
			    (unsigned)supplied, i);
			return (1);
		}
	}
	for (i = copied; i < sizeof (addr); i++) {
		if (bytes[i] != 0xa5) {
			(void) fprintf(stderr,
			    "%s(%u) wrote byte %zu past the Linux address\n",
			    operation, (unsigned)supplied, i);
			return (1);
		}
	}
	return (0);
}

static int
check_type(int type)
{
	static const socklen_t sizes[] = {
	    0,
	    1,
	    sizeof (sa_family_t),
	    sizeof (struct sockaddr_un)
	};
	int pair[2];
	int failures = 0;
	size_t i;

	if (socketpair(AF_UNIX, type, 0, pair) != 0) {
		(void) fprintf(stderr, "socketpair failed: %s\n", strerror(errno));
		return (1);
	}

	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		failures += check_name("getsockname", pair[0], sizes[i]);
		failures += check_name("getpeername", pair[0], sizes[i]);
	}

	(void) close(pair[0]);
	(void) close(pair[1]);
	return (failures);
}

int
main(void)
{
	int failures = 0;

	failures += check_type(SOCK_STREAM);
	failures += check_type(SOCK_DGRAM);
	if (failures != 0) {
		(void) fprintf(stderr, "FAIL: %d AF_UNIX name checks failed\n",
		    failures);
		return (1);
	}

	(void) printf("PASS: unnamed AF_UNIX socket names match Linux\n");
	return (0);
}
