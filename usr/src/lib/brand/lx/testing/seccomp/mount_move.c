/*
 * CDDL version 1.0, http://www.illumos.org/license/CDDL.
 */
#define	_GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define	STACK_SIZE	(1024 * 1024)

typedef struct move_paths {
	char workspace[128];
	char target[128];
} move_paths_t;

static char child_stack[STACK_SIZE];

static int
mount_child(void *arg)
{
	move_paths_t *paths = arg;
	char fd_path[64], marker_file[160], source_file[160], target_file[160];
	char content[16] = { 0 };
	int fd, target_fd;

	if (mount(NULL, "/", NULL, MS_REC | MS_SLAVE, NULL) != 0) {
		perror("mount propagation");
		return (1);
	}
	if (mount("tmpfs", paths->workspace, "tmpfs", MS_NOSUID | MS_NODEV,
	    "mode=0700,size=1m,nr_inodes=64") != 0) {
		perror("mount tmpfs");
		return (1);
	}
	(void) snprintf(source_file, sizeof (source_file), "%s/credential",
	    paths->workspace);
	fd = open(source_file, O_CREAT | O_WRONLY | O_EXCL, 0600);
	if (fd < 0 || write(fd, "retained\n", 9) != 9) {
		perror("create source file");
		if (fd >= 0)
			(void) close(fd);
		return (1);
	}
	(void) close(fd);
	if (mount("tmpfs", paths->target, "tmpfs", MS_NOSUID | MS_NODEV,
	    "mode=0700,size=1m,nr_inodes=64") != 0) {
		perror("mount target tmpfs");
		return (1);
	}
	(void) snprintf(marker_file, sizeof (marker_file), "%s/underlay",
	    paths->target);
	fd = open(marker_file, O_CREAT | O_WRONLY | O_EXCL, 0600);
	if (fd < 0) {
		perror("create target marker");
		return (1);
	}
	(void) close(fd);
	if (mount(paths->workspace, paths->target, NULL, MS_MOVE, NULL) != 0) {
		perror("move mount");
		return (1);
	}
	if (access(source_file, F_OK) == 0 || errno != ENOENT) {
		(void) fprintf(stderr,
		    "source still exposes moved filesystem\n");
		return (1);
	}
	(void) snprintf(target_file, sizeof (target_file), "%s/credential",
	    paths->target);
	fd = open(target_file, O_RDONLY);
	if (fd < 0 || read(fd, content, sizeof (content)) != 9 ||
	    memcmp(content, "retained\n", 9) != 0) {
		perror("read moved file");
		if (fd >= 0)
			(void) close(fd);
		return (1);
	}
	(void) close(fd);
	if (access(marker_file, F_OK) == 0 || errno != ENOENT) {
		(void) fprintf(stderr, "move did not cover target mount\n");
		return (1);
	}
	if (umount2(paths->target, MNT_DETACH | UMOUNT_NOFOLLOW) != 0) {
		perror("unmount moved tmpfs");
		return (1);
	}
	if (access(marker_file, F_OK) != 0) {
		perror("target mount was not revealed");
		return (1);
	}
	if (umount2(paths->target, MNT_DETACH | UMOUNT_NOFOLLOW) != 0) {
		perror("unmount target tmpfs");
		return (1);
	}
	target_fd = open(paths->workspace, O_PATH | O_DIRECTORY | O_CLOEXEC);
	if (target_fd < 0) {
		perror("open recursive bind target");
		return (1);
	}
	(void) snprintf(fd_path, sizeof (fd_path), "/proc/self/fd/%d",
	    target_fd);
	if (mount("/", fd_path, NULL, MS_BIND | MS_REC, NULL) != 0) {
		perror("recursive root bind through fd");
		(void) close(target_fd);
		return (1);
	}
	(void) close(target_fd);
	(void) snprintf(source_file, sizeof (source_file), "%s/etc",
	    paths->workspace);
	target_fd = open(source_file, O_PATH | O_DIRECTORY | O_CLOEXEC);
	if (target_fd < 0) {
		perror("open nested bind target");
		return (1);
	}
	(void) snprintf(fd_path, sizeof (fd_path), "/proc/self/fd/%d",
	    target_fd);
	if (mount(source_file, fd_path, NULL, MS_BIND | MS_REC, NULL) != 0) {
		perror("nested bind through fd");
		(void) close(target_fd);
		return (1);
	}
	(void) close(target_fd);
	if (umount2(source_file, MNT_DETACH | UMOUNT_NOFOLLOW) != 0) {
		perror("unmount nested bind");
		return (1);
	}
	if (umount2(paths->workspace, MNT_DETACH | UMOUNT_NOFOLLOW) != 0) {
		perror("unmount recursive root bind");
		return (1);
	}
	(void) snprintf(source_file, sizeof (source_file), "%s/inaccessible",
	    paths->target);
	fd = open(source_file, O_CREAT | O_WRONLY | O_EXCL, 0000);
	if (fd < 0) {
		perror("create bind source");
		return (1);
	}
	(void) close(fd);
	if (mount("proc", paths->workspace, "proc",
	    MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) != 0) {
		perror("mount proc");
		return (1);
	}
	if (mount(paths->workspace, "/proc", NULL, MS_MOVE, NULL) != 0) {
		perror("move proc");
		return (1);
	}
	target_fd = open("/proc/kmsg", O_PATH | O_CLOEXEC);
	if (target_fd < 0) {
		perror("open proc target");
		return (1);
	}
	(void) snprintf(fd_path, sizeof (fd_path), "/proc/self/fd/%d",
	    target_fd);
	if (mount(source_file, fd_path, NULL, MS_BIND | MS_REC, NULL) != 0) {
		perror("bind proc fd target");
		(void) close(target_fd);
		return (1);
	}
	(void) close(target_fd);
	if (umount2("/proc/kmsg", MNT_DETACH) != 0) {
		perror("unmount proc bind");
		return (1);
	}
	if (umount2("/proc", MNT_DETACH) != 0) {
		perror("unmount moved proc");
		return (1);
	}
	if (unlink(source_file) != 0) {
		perror("unlink bind source");
		return (1);
	}
	if (mount(NULL, "/", NULL, MS_REC | MS_SHARED, NULL) != 0) {
		perror("mount shared propagation");
		return (1);
	}
	return (0);
}

int
main(void)
{
	move_paths_t paths = {
		"/var/tmp/lx-mount-move-source-XXXXXX",
		"/var/tmp/lx-mount-move-target-XXXXXX"
	};
	pid_t child;
	int status, result = 1;

	if (mkdtemp(paths.workspace) == NULL || mkdtemp(paths.target) == NULL) {
		perror("mkdtemp");
		goto out;
	}
	child = clone(mount_child, child_stack + sizeof (child_stack),
	    CLONE_NEWNS | SIGCHLD, &paths);
	if (child < 0) {
		perror("clone");
		goto out;
	}
	if (waitpid(child, &status, 0) != child) {
		perror("waitpid");
		goto out;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		(void) fprintf(stderr, "child status %#x\n", status);
		goto out;
	}
	(void) puts("PASS: mount subtree moved");
	result = 0;
out:
	if (paths.target[0] == '/')
		(void) rmdir(paths.target);
	if (paths.workspace[0] == '/')
		(void) rmdir(paths.workspace);
	return (result);
}
