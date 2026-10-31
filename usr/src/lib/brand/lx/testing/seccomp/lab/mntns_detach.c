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
		(void) printf("MND_FAIL name=%s errno=%d\n", name, errno);
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
	int drop = 0, deny = 0, rootfd, childfd, filefd, fd, rv, foreignfd;
	uid_t uid;
	gid_t gid;
	char source[PATH_MAX], target[PATH_MAX], other[PATH_MAX], p[PATH_MAX];
	char rootpath[80], childpath[80], foreignpath[80], data[16] = { 0 };
	struct statfs stats;
	struct stat before, after;

	(void) setvbuf(stdout, NULL, _IONBF, 0);
	(void) alarm(60);
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--drop") == 0)
			drop = 1;
		else if (strcmp(argv[i], "--deny") == 0)
			deny = 1;
		else
			return (2);
	}
	if (drop && (setgroups(0, NULL) || setgid(65534) || setuid(65534)))
		return (2);
	uid = getuid();
	gid = getgid();
	if (prctl(PR_SET_DUMPABLE, 1UL, 0UL, 0UL, 0UL))
		return (2);
	(void) snprintf(base, sizeof (base), "/test/mnt-detach/run.XXXXXX");
	if (mkdtemp(base) == NULL || mkdir(path("source", source), 0755) ||
	    mkdir(path("target", target), 0755) || mkdir(path("other", other), 0755) ||
	    mkdir(path("source/subdir", p), 0755) ||
	    write_text(path("source/file", p), "source\n", 1) ||
	    write_text(path("target/file", p), "covered\n", 1))
		return (2);
	foreignfd = open(source, O_PATH | O_DIRECTORY);
	if (foreignfd < 0)
		return (2);
	(void) snprintf(foreignpath, sizeof (foreignpath),
	    "/proc/self/fd/%d", foreignfd);
	if (unshare(CLONE_NEWUSER | CLONE_NEWNS) || maps(uid, gid) || filter() ||
	    mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) ||
	    mount("tmpfs", path("source/subdir", p), "tmpfs", 0, "mode=0755") ||
	    write_text(path("source/subdir/data", p), "child\n", 1) ||
	    mount(source, target, NULL, MS_BIND | MS_REC, NULL))
		return (2);
	if (deny) {
		struct __user_cap_header_struct h = { _LINUX_CAPABILITY_VERSION_3, 0 };
		struct __user_cap_data_struct caps[2];

		if (syscall(SYS_capget, &h, caps) != 0)
			return (2);
		caps[0].effective &= ~(1U << CAP_SYS_ADMIN);
		caps[0].permitted &= ~(1U << CAP_SYS_ADMIN);
		if (syscall(SYS_capset, &h, caps) != 0)
			return (2);
		errno = 0;
		check(umount2(target, MNT_DETACH) == -1 && errno == EPERM,
		    "detach-requires-owner-capability");
		check(contents(path("target/subdir/data", p), "child\n"),
		    "denied-detach-tree-unchanged");
		goto done;
	}
	errno = 0;
	check(umount2((void *)1, MNT_DETACH) == -1 && errno == EFAULT,
	    "bad-target-pointer");
	errno = 0;
	check(umount2(path("source/file", p), MNT_DETACH) == -1 &&
	    errno == EINVAL, "non-mount-target");
	errno = 0;
	check(mount(foreignpath, other, NULL, MS_BIND, NULL) == -1 &&
	    errno == EINVAL, "foreign-source-not-a-member");
	errno = 0;
	check(umount2(foreignpath, MNT_DETACH) == -1 && errno == EINVAL,
	    "foreign-detach-target");
	check(write_text(path("other/file", p), "top\n", 1) == 0,
	    "prepare-covering-mount");
	check(mount(other, target, NULL, MS_BIND, NULL) == 0,
	    "cover-older-mount-tree");
	check(contents(path("target/file", p), "top\n"), "top-mount-visible");
	check(umount2(target, MNT_DETACH) == 0, "detach-only-top-mount");
	check(contents(path("target/file", p), "source\n"),
	    "older-mount-visible-again");
	check(contents(path("target/subdir/data", p), "child\n"),
	    "older-submount-survives");
	rootfd = open(target, O_PATH | O_DIRECTORY);
	childfd = open(path("target/subdir", p), O_PATH | O_DIRECTORY);
	filefd = open(path("target/file", p), O_RDWR);
	check(rootfd >= 0 && childfd >= 0 && filefd >= 0, "hold-descriptors");
	(void) snprintf(rootpath, sizeof (rootpath), "/proc/self/fd/%d", rootfd);
	(void) snprintf(childpath, sizeof (childpath), "/proc/self/fd/%d", childfd);
	check(chdir(path("target/subdir", p)) == 0, "enter-child-cwd");
	check(umount2(target, MNT_DETACH) == 0, "detach-busy-subtree");
	check(contents(path("target/file", p), "covered\n"), "covered-target-visible");
	check(read(filefd, data, sizeof (data) - 1) == 7 &&
	    strcmp(data, "source\n") == 0, "detached-file-readable");
	check(pwrite(filefd, "x", 1, 0) == 1, "detached-file-writable");
	fd = openat(rootfd, "file", O_RDONLY);
	check(fd >= 0 && read(fd, data, 1) == 1 && data[0] == 'x',
	    "detached-root-directory-access");
	(void) close(fd);
	fd = openat(rootfd, "subdir/data", O_RDONLY);
	check(fd == -1 && errno == ENOENT, "detached-child-unlinked");
	fd = openat(childfd, "data", O_RDONLY);
	check(fd >= 0 && read(fd, data, 1) == 1 && data[0] == 'c',
	    "detached-child-descriptor-access");
	(void) close(fd);
	check(fstatfs(childfd, &stats) == 0 && stats.f_type == 0x01021994,
	    "detached-child-statistics");
	fd = openat(rootfd, "..", O_PATH | O_DIRECTORY);
	check(fd >= 0 && fstat(rootfd, &before) == 0 && fstat(fd, &after) == 0 &&
	    before.st_dev == after.st_dev && before.st_ino == after.st_ino,
	    "detached-root-parent-is-self");
	(void) close(fd);
	fd = openat(childfd, "..", O_PATH | O_DIRECTORY);
	check(fd >= 0 && fstat(childfd, &before) == 0 && fstat(fd, &after) == 0 &&
	    before.st_dev == after.st_dev && before.st_ino == after.st_ino,
	    "detached-child-parent-is-self");
	(void) close(fd);

	errno = 0;
	check(getcwd(p, sizeof (p)) == NULL && errno == ENOENT,
	    "detached-cwd-no-root-path");
	check(chdir("/") == 0, "return-to-root");
	errno = 0;
	rv = mount(rootpath, other, NULL, MS_BIND, NULL);
	check(rv == -1 && errno == EINVAL, "detached-source-not-a-member");
	errno = 0;
	check(mount(source, rootpath, NULL, MS_BIND, NULL) == -1 && errno == ENOENT,
	    "detached-target-not-a-member");
	errno = 0;
	check(mount(NULL, rootpath, NULL, MS_PRIVATE, NULL) == -1 && errno == EINVAL,
	    "detached-propagation-not-a-member");
	errno = 0;
	check(mount(NULL, childpath, NULL, MS_PRIVATE, NULL) == -1 && errno == EINVAL,
	    "detached-descendant-not-a-member");
	errno = 0;
	check(umount2(rootpath, MNT_DETACH) == -1 && errno == EINVAL,
	    "detach-already-detached");
	check(unshare(CLONE_NEWNS) == 0, "clone-active-tree-only");
	check(contents(path("target/file", p), "covered\n"), "clone-covered-target");
	fd = openat(rootfd, "subdir/data", O_RDONLY);
	check(fd == -1 && errno == ENOENT, "clone-detached-child-unlinked");
	fd = openat(childfd, "data", O_RDONLY);
	check(fd >= 0 && read(fd, data, 1) == 1 && data[0] == 'c',
	    "clone-keeps-detached-fd-capability");
	(void) close(fd);
	errno = 0;
	check(mount(rootpath, other, NULL, MS_BIND, NULL) == -1 &&
	    errno == EINVAL, "clone-detached-source-rejected");
	errno = 0;
	check(mount(source, rootpath, NULL, MS_BIND, NULL) == -1 &&
	    errno == ENOENT, "clone-detached-target-rejected");
	(void) close(filefd);
	(void) close(childfd);
	(void) close(rootfd);
done:
	(void) close(foreignfd);
	errno = 0;
	check(syscall(SYS_getppid) == -1 && errno == EACCES, "filter-retained");
	(void) printf("MND_PASS uid=%lu deny=%d failures=%d base=%s\n",
	    (unsigned long)uid, deny, failures, base);
	return (failures != 0);
}
