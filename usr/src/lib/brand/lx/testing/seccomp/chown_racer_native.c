/*
 * CDDL version 1.0, http://www.illumos.org/license/CDDL.
 */
#define	__EXTENSIONS__
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

/* Change only the private probe inode, using native global-zone authority. */
int
main(int argc, char **argv)
{
	int fd;
	uint_t cycles = 0;

	if (argc != 3)
		return (2);
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
