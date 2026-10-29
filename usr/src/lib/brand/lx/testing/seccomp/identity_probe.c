/*
 * CDDL version 1.0, http://www.illumos.org/license/CDDL.
 * Build this small helper for Linux, and also natively for illumos.
 * It performs no command execution or privileged operation.
 */
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	uid_t uid;
	gid_t gid;

	if (argc != 3)
		return (2);
	uid = (uid_t)strtoul(argv[1], NULL, 10);
	gid = (gid_t)strtoul(argv[2], NULL, 10);
	if (geteuid() != uid || getegid() != gid) {
		fprintf(stderr, "identity: euid=%lu egid=%lu, "
		    "expected %lu/%lu\n",
		    (unsigned long)geteuid(), (unsigned long)getegid(),
		    (unsigned long)uid, (unsigned long)gid);
		return (1);
	}
	return (0);
}
