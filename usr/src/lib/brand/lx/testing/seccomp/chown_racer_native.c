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
acl_control(const char *path, int mode)
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
	if ((mode ? fchmod(fd, 0644) : fchown(fd, 1000, 1000)) != 0) {
		perror("ACL control setattr");
		return (1);
	}
	(void) close(fd);
	(void) puts(mode ? "NATIVE_ACL_MODE_PASS" :
	    "NATIVE_ACL_TAKE_OWNER_PASS");
	return (0);
}

/* Change only the private probe inode, using native global-zone authority. */
int
main(int argc, char **argv)
{
	int fd, gid_race;
	const char *path, *stop;
	uint_t cycles = 0;

	if (argc == 3 && strcmp(argv[1], "--acl-control") == 0)
		return (acl_control(argv[2], 0));
	if (argc == 3 && strcmp(argv[1], "--acl-mode-control") == 0)
		return (acl_control(argv[2], 1));
	gid_race = argc == 4 && strcmp(argv[1], "--gid-race") == 0;
	if (argc != 3 && !gid_race)
		return (2);
	path = argv[gid_race ? 2 : 1];
	stop = argv[gid_race ? 3 : 2];
	if ((fd = open(path, O_RDONLY)) < 0) {
		perror("open");
		return (1);
	}
	(void) alarm(25);
	while (access(stop, F_OK) != 0) {
		/* The test namespace maps 1001, but does not map 2000. */
		if (fchown(fd, 1001, 1001) != 0) {
			perror("mapped fchown");
			return (1);
		}
		(void) usleep(1000);
		if (fchown(fd, gid_race ? 1001 : 2000, 2000) != 0) {
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
