/*
 * CDDL version 1.0, http://www.illumos.org/license/CDDL.
 */
#define	_GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define	STACK_SIZE	(1024 * 1024)

static char child_stack[STACK_SIZE];

static int
mount_child(void *arg)
{
	const char *target = arg;
	struct stat st;

	if (mount(NULL, "/", NULL, MS_REC | MS_SLAVE, NULL) != 0) {
		perror("mount propagation");
		return (1);
	}
	if (mount("tmpfs", target, "tmpfs", MS_NOSUID | MS_NODEV,
	    "mode=01777,size=20%,nr_inodes=800k") != 0) {
		perror("mount tmpfs");
		return (1);
	}
	if (stat(target, &st) != 0) {
		perror("stat tmpfs");
		return (1);
	}
	if ((st.st_mode & 07777) != 01777) {
		(void) fprintf(stderr, "tmpfs mode is %#o\n",
		    st.st_mode & 07777);
		return (1);
	}
	if (umount2(target, MNT_DETACH) != 0) {
		perror("umount tmpfs");
		return (1);
	}
	return (0);
}

int
main(void)
{
	char target[] = "/var/tmp/lx-tmpfs-nr-inodes-XXXXXX";
	pid_t child;
	int status;

	if (mkdtemp(target) == NULL) {
		perror("mkdtemp");
		return (1);
	}
	child = clone(mount_child, child_stack + sizeof (child_stack),
	    CLONE_NEWNS | SIGCHLD, target);
	if (child < 0) {
		perror("clone");
		(void) rmdir(target);
		return (1);
	}
	if (waitpid(child, &status, 0) != child) {
		perror("waitpid");
		(void) rmdir(target);
		return (1);
	}
	if (rmdir(target) != 0) {
		perror("rmdir");
		return (1);
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		(void) fprintf(stderr, "child status %#x\n", status);
		return (1);
	}
	(void) puts("PASS: tmpfs nr_inodes option accepted");
	return (0);
}
