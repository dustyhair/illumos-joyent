/*
 * CDDL version 1.0, http://www.illumos.org/license/CDDL.
 */
#define	__EXTENSIONS__
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <grp.h>
#include <priv.h>
#include <string.h>
#include <unistd.h>

/* Prove the native ACL grant with no native ownership override privilege. */
static int
acl_control(const char *path)
{
	priv_set_t *basic;
	int fd;

	if ((fd = open(path, O_RDONLY)) < 0) {
		perror("ACL control open");
		return (1);
	}
	if ((basic = priv_allocset()) == NULL) {
		perror("priv_allocset");
		return (1);
	}
	priv_basicset(basic);
	if (setgroups(0, NULL) != 0 || setgid(1000) != 0 ||
	    setuid(1000) != 0 ||
	    setppriv(PRIV_SET, PRIV_EFFECTIVE, basic) != 0 ||
	    setppriv(PRIV_SET, PRIV_PERMITTED, basic) != 0) {
		perror("ACL control credential");
		return (1);
	}
	priv_freeset(basic);
	if (fchown(fd, 1000, 1000) != 0) {
		perror("ACL control fchown");
		return (1);
	}
	(void) close(fd);
	(void) puts("NATIVE_ACL_TAKE_OWNER_PASS");
	return (0);
}

/* Change only the private probe inode, using native global-zone authority. */
int
main(int argc, char **argv)
{
	int fd;
	uint_t cycles = 0;

	if (argc != 3)
		return (2);
	if (strcmp(argv[1], "--acl-control") == 0)
		return (acl_control(argv[2]));
	if ((fd = open(argv[1], O_RDONLY)) < 0) {
		perror("open");
		return (1);
	}
	(void) alarm(25);
	while (access(argv[2], F_OK) != 0) {
		/* The test namespace maps 1001, but does not map 2000. */
		if (fchown(fd, 1001, 1001) != 0) {
			perror("mapped fchown");
			return (1);
		}
		(void) usleep(1000);
		if (fchown(fd, 2000, 2000) != 0) {
			perror("unmapped fchown");
			return (1);
		}
		(void) usleep(3000);
		cycles++;
	}
	(void) close(fd);
	(void) printf("NATIVE_CHOWN_RACER_PASS cycles=%u\n", cycles);
	return (0);
}
