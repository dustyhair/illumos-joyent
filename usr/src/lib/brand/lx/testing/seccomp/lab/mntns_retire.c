/*
 * CDDL version 1.0, http://www.illumos.org/license/CDDL.
 */
#define	_GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <linux/capability.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
static char base[PATH_MAX];

static void
check(int ok, const char *name)
{
	if (!ok) {
		(void) printf("MNX_FAIL name=%s errno=%d\n", name, errno);
		failures++;
	}
}

static const char *
path(const char *name, char *out)
{
	if (snprintf(out, PATH_MAX, "%s/%s", base, name) >= PATH_MAX)
		_exit(98);
	return (out);
}

static int
write_text(const char *name, const char *text, int create)
{
	int fd = open(name, O_WRONLY | (create ? O_CREAT | O_TRUNC : 0), 0644);
	ssize_t n;

	if (fd < 0)
		return (-1);
	n = write(fd, text, strlen(text));
	(void) close(fd);
	return (n == (ssize_t)strlen(text) ? 0 : -1);
}

static int
contents(const char *name, const char *want)
{
	char data[64] = { 0 };
	int fd = open(name, O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return (0);
	n = read(fd, data, sizeof (data) - 1);
	(void) close(fd);
	return (n == (ssize_t)strlen(want) && strcmp(data, want) == 0);
}

static int
maps(uid_t uid, gid_t gid)
{
	char text[96];

	if (write_text("/proc/self/setgroups", "deny\n", 0) != 0)
		return (-1);
	(void) snprintf(text, sizeof (text), "0 %lu 1\n", (unsigned long)uid);
	if (write_text("/proc/self/uid_map", text, 0) != 0)
		return (-1);
	(void) snprintf(text, sizeof (text), "0 %lu 1\n", (unsigned long)gid);
	return (write_text("/proc/self/gid_map", text, 0));
}

static int
filter(void)
{
	struct sock_filter code[] = {
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_getppid, 0, 1),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EACCES),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)
	};
	struct sock_fprog fp = { sizeof (code) / sizeof (code[0]), code };

	return (prctl(PR_SET_NO_NEW_PRIVS, 1UL, 0UL, 0UL, 0UL) ||
	    syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &fp));
}

int
main(int argc, char **argv)
{
	int drop = 0, rootfd, childfd, nsfd, filefd, fd;
	uid_t uid;
	gid_t gid;
	char source[PATH_MAX], target[PATH_MAX], p[PATH_MAX], relative[PATH_MAX];
	char data[16] = { 0 };
	struct stat before, after;
	struct statfs stats;

	(void) setvbuf(stdout, NULL, _IONBF, 0);
	(void) alarm(60);
	if (argc == 2 && strcmp(argv[1], "--drop") == 0)
		drop = 1;
	else if (argc != 1)
		return (2);
	if (drop && (setgroups(0, NULL) || setgid(65534) || setuid(65534)))
		return (2);
	uid = getuid();
	gid = getgid();
	if (prctl(PR_SET_DUMPABLE, 1UL, 0UL, 0UL, 0UL))
		return (2);
	(void) snprintf(base, sizeof (base), "/test/mnt-retire/run.XXXXXX");
	if (mkdtemp(base) == NULL || mkdir(path("source", source), 0755) ||
	    mkdir(path("target", target), 0755) ||
	    mkdir(path("source/subdir", p), 0755) ||
	    write_text(path("source/file", p), "source\n", 1) ||
	    write_text(path("target/file", p), "covered\n", 1))
		return (2);
	if (unshare(CLONE_NEWUSER | CLONE_NEWNS) || maps(uid, gid) || filter() ||
	    mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) ||
	    mount("tmpfs", path("source/subdir", p), "tmpfs", 0, "mode=0755") ||
	    write_text(path("source/subdir/data", p), "child\n", 1) ||
	    mount(source, target, NULL, MS_BIND | MS_REC, NULL))
		return (2);
	rootfd = open(target, O_PATH | O_DIRECTORY);
	childfd = open(path("target/subdir", p), O_PATH | O_DIRECTORY);
	nsfd = open("/", O_PATH | O_DIRECTORY);
	filefd = open(path("target/file", p), O_RDWR);
	check(rootfd >= 0 && childfd >= 0 && nsfd >= 0 && filefd >= 0,
	    "hold-old-namespace-descriptors");
	check(unshare(CLONE_NEWNS) == 0, "leave-last-namespace-member");
	fd = openat(rootfd, "subdir/data", O_RDONLY);
	check(fd == -1 && errno == ENOENT, "old-namespace-child-unlinked");
	if (fd >= 0)
		(void) close(fd);
	check(snprintf(relative, sizeof (relative), "%s/file", target + 1) <
	    (int)sizeof (relative), "old-root-relative-path");
	fd = openat(nsfd, relative, O_RDONLY);
	check(fd >= 0 && read(fd, data, sizeof (data) - 1) == 8 &&
	    strcmp(data, "covered\n") == 0, "old-root-covered-target-visible");
	(void) close(fd);
	bzero(data, sizeof (data));
	check(read(filefd, data, sizeof (data) - 1) == 7 &&
	    strcmp(data, "source\n") == 0, "held-old-file-readable");
	check(pwrite(filefd, "x", 1, 0) == 1 &&
	    contents(path("target/file", p), "xource\n"),
	    "held-old-file-shares-data-with-copy");
	fd = openat(childfd, "data", O_RDONLY);
	check(fd >= 0 && read(fd, data, 1) == 1 && data[0] == 'c',
	    "held-old-child-readable");
	(void) close(fd);
	check(fstatfs(childfd, &stats) == 0 && stats.f_type == 0x01021994,
	    "held-old-child-statistics");
	for (int i = 0; i < 3; i++) {
		int dir = i == 0 ? rootfd : i == 1 ? childfd : nsfd;

		fd = openat(dir, "..", O_PATH | O_DIRECTORY);
		check(fd >= 0 && fstat(dir, &before) == 0 && fstat(fd, &after) == 0 &&
		    before.st_dev == after.st_dev && before.st_ino == after.st_ino,
		    "old-root-parent-is-self");
		(void) close(fd);
	}
	check(fchdir(childfd) == 0, "old-child-cwd");
	errno = 0;
	check(getcwd(p, sizeof (p)) == NULL && errno == ENOENT,
	    "old-namespace-cwd-unreachable");
	check(chdir("/") == 0 &&
	    contents(path("target/subdir/data", p), "child\n") &&
	    contents(path("source/subdir/data", p), "child\n"),
	    "new-namespace-child-mounts-still-present");
	check(unshare(CLONE_NEWNS) == 0 &&
	    contents(path("target/subdir/data", p), "child\n"),
	    "copy-remains-valid-after-second-retirement");
	(void) close(filefd);
	(void) close(nsfd);
	(void) close(childfd);
	(void) close(rootfd);
	errno = 0;
	check(syscall(SYS_getppid) == -1 && errno == EACCES, "filter-retained");
	(void) printf("MNX_PASS uid=%lu failures=%d base=%s\n",
	    (unsigned long)uid, failures, base);
	return (failures != 0);
}
