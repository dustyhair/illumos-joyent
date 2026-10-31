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
#include <sys/mman.h>
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
		(void) printf("MNF_FAIL name=%s errno=%d\n", name, errno);
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

static unsigned long seal_flags = MS_BIND | MS_REMOUNT | MS_RDONLY |
    MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_SILENT | MS_RELATIME;

static void
expect_error(int rv, int want, const char *name)
{
	check(rv == -1 && errno == want, name);
}

static void
seal(const char *target, int want, const char *name)
{
	int rv;

	errno = 0;
	rv = mount(NULL, target, NULL, seal_flags, NULL);
	check(want ? rv == -1 && errno == want : rv == 0, name);
}

int
main(int argc, char **argv)
{
	int drop = 0, denied = 0, linux_control = 0, fd, sourcefd, status;
	uid_t uid;
	gid_t gid;
	char source[PATH_MAX], target[PATH_MAX], p[PATH_MAX], q[PATH_MAX];
	char alias[80], *mapping;
	struct statfs stats;
	struct __user_cap_header_struct h = {
		.version = _LINUX_CAPABILITY_VERSION_3 };
	struct __user_cap_data_struct caps[2] = { 0 };
	pid_t child;
	long page = sysconf(_SC_PAGESIZE);

	(void) setvbuf(stdout, NULL, _IONBF, 0);
	(void) alarm(60);
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--drop") == 0)
			drop = 1;
		else if (strcmp(argv[i], "--deny") == 0)
			denied = 1;
		else if (strcmp(argv[i], "--linux") == 0)
			linux_control = 1;
		else
			return (2);
	}
	if (drop && (setgroups(0, NULL) || setgid(65534) || setuid(65534)))
		return (2);
	uid = getuid();
	gid = getgid();
	if (prctl(PR_SET_DUMPABLE, 1UL, 0UL, 0UL, 0UL))
		return (2);
	(void) snprintf(base, sizeof (base), "/test/mnt-freeze/run.XXXXXX");
	if (mkdtemp(base) == NULL || mkdir(path("source", source), 0755) ||
	    mkdir(path("target", target), 0755) ||
	    mkdir(path("late", p), 0755) || mkdir(path("mapped", p), 0755) ||
	    mkdir(path("source/subdir", p), 0755) ||
	    write_text(path("source/file", p), "source\n", 1) ||
	    write_text(path("source/secret", p), "secret\n", 1) ||
	    chmod(p, 0) || symlink("file", path("source/alias", p)) ||
	    symlink("secret", path("source/secret-alias", p)) ||
	    write_text(path("source/script", p), "#!/bin/sh\nexit 0\n", 1) ||
	    chmod(p, 0755) || write_text(path("file-target", p), "cover\n", 1))
		return (2);
	if (unshare(CLONE_NEWUSER | CLONE_NEWNS) || maps(uid, gid) || filter())
		return (2);
	if (linux_control && mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL))
		return (2);
	if (mount("tmpfs", path("source/subdir", p), "tmpfs",
	    MS_NOSUID | MS_NODEV, "mode=0755") ||
	    write_text(path("source/subdir/data", p), "child\n", 1))
		return (2);
	if (mount(source, target, NULL, MS_BIND | MS_REC, NULL))
		return (2);
	if (denied) {
		if (syscall(SYS_capget, &h, caps))
			return (2);
		caps[0].effective &= ~(1U << CAP_SYS_ADMIN);
		if (syscall(SYS_capset, &h, caps))
			return (2);
		seal(target, EPERM, "denied-seal");
		check(write_text(path("target/file", p), "denied\n", 0) == 0,
		    "denied-seal-leaves-writable");
		goto out;
	}
	seal(source, EINVAL, "non-mountpoint");
	errno = 0;
	expect_error(syscall(SYS_mount, (void *)1, target, NULL, seal_flags, NULL),
	    EFAULT, "bad-ignored-source");
	sourcefd = open(path("source/file", p), O_RDWR);
	check(sourcefd >= 0, "source-writer");
	fd = open(target, O_PATH | O_DIRECTORY);
	check(fd >= 0, "target-descriptor");
	(void) snprintf(alias, sizeof (alias), "/proc/self/fd/%d", fd);
	seal(alias, 0, "fresh-bind-seal-via-descriptor");
	(void) close(fd);
	check(statfs(target, &stats) == 0 && (stats.f_flags & 0xf) == 0xf,
	    "sealed-mount-flags");
	check(statfs(path("target/subdir", p), &stats) == 0 &&
	    !(stats.f_flags & 1), "parent-seal-does-not-seal-child-mount");
	seal(p, 0, "separate-child-seal");
	errno = 0;
	expect_error(open(path("target/subdir/data", p), O_WRONLY), EROFS,
	    "child-seal-enforced");
	check(write(sourcefd, "x", 1) == 1, "source-writer-unaffected");
	(void) close(sourcefd);
	check(contents(path("target/file", p), "xource\n"), "read-sealed-file");
	errno = 0;
	expect_error(open(p, O_WRONLY), EROFS, "readonly-open");
	fd = open(p, O_RDONLY | O_CREAT, 0644);
	check(fd >= 0, "readonly-create-existing");
	errno = 0;
	expect_error(fchmod(fd, 0600), EROFS, "readonly-fchmod");
	mapping = mmap(NULL, (size_t)page, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE, fd, 0);
	check(mapping != MAP_FAILED, "private-map-remains-allowed");
	if (mapping != MAP_FAILED) {
		mapping[0] = 'p';
		(void) munmap(mapping, (size_t)page);
	}
	(void) close(fd);
	check(contents(p, "xource\n"), "private-map-did-not-change-file");
	fd = open(path("target/alias", p), O_RDONLY | O_CREAT, 0644);
	check(fd >= 0, "readonly-create-existing-symlink");
	(void) close(fd);
	errno = 0;
	expect_error(open(path("target/new", p), O_RDONLY | O_CREAT, 0644),
	    EROFS, "readonly-create-missing");
	errno = 0;
	expect_error(chmod(path("target/file", p), 0600), EROFS, "readonly-chmod");
	errno = 0;
	expect_error(chown(p, 0, 0), EROFS, "readonly-chown");
	errno = 0;
	expect_error(unlink(p), EROFS, "readonly-unlink");
	errno = 0;
	expect_error(mkdir(path("target/newdir", p), 0755), EROFS, "readonly-mkdir");
	errno = 0;
	expect_error(rmdir(path("target/subdir", p)), EROFS, "readonly-rmdir");
	errno = 0;
	expect_error(symlink("x", path("target/newlink", p)), EROFS,
	    "readonly-symlink");
	errno = 0;
	expect_error(rename(path("target/file", p), path("target/renamed", q)),
	    EROFS, "readonly-rename");
	path("target/script", p);
	child = fork();
	if (child == 0) {
		execl(p, p, (char *)NULL);
		_exit(errno == EACCES ? 0 : 90);
	}
	check(child > 0 && waitpid(child, &status, 0) == child &&
	    WIFEXITED(status) && WEXITSTATUS(status) == 0, "noexec-enforced");
	seal(target, 0, "idempotent-seal");
	if (syscall(SYS_capget, &h, caps))
		return (2);
	caps[0].effective &= ~((1U << CAP_DAC_OVERRIDE) | (1U << CAP_DAC_READ_SEARCH));
	if (syscall(SYS_capset, &h, caps))
		return (2);
	errno = 0;
	expect_error(open(path("target/secret", p), O_RDONLY), EACCES,
	    "ordinary-read-respects-dac");
	errno = 0;
	expect_error(open(p, O_RDONLY | O_CREAT, 0644), EACCES,
	    "readonly-create-respects-dac");
	errno = 0;
	expect_error(open(path("target/secret-alias", p), O_RDONLY | O_CREAT, 0644),
	    EACCES, "readonly-create-symlink-respects-dac");
	path("late", p);
	check(mount(source, p, NULL, MS_BIND, NULL) == 0, "late-bind");
	fd = open(path("late/file", q), O_RDWR);
	check(fd >= 0, "late-writer");
	(void) close(fd);
	seal(p, linux_control ? 0 : EOPNOTSUPP, "post-writer-scope");
	path("mapped", p);
	check(mount(source, p, NULL, MS_BIND, NULL) == 0, "mapped-bind");
	fd = open(path("mapped/file", q), O_RDWR);
	check(fd >= 0, "mapped-writer");
	mapping = mmap(NULL, (size_t)page, PROT_READ | PROT_WRITE,
	    MAP_SHARED, fd, 0);
	check(mapping != MAP_FAILED, "shared-map");
	(void) close(fd);
	if (mapping != MAP_FAILED) {
		seal(p, linux_control ? EBUSY : EOPNOTSUPP, "mapped-seal-refused");
		(void) munmap(mapping, (size_t)page);
	}
	seal(p, linux_control ? 0 : EOPNOTSUPP, "post-map-scope");
	path("source/file", p);
	path("file-target", q);
	check(mount(p, q, NULL, MS_BIND, NULL) == 0, "file-bind");
	seal(q, 0, "file-root-seal");
	fd = open(q, O_RDONLY | O_CREAT, 0644);
	check(fd >= 0, "file-root-readonly-create-existing");
	(void) close(fd);
	check(unshare(CLONE_NEWNS) == 0, "copy-sealed-view");
	check(statfs(target, &stats) == 0 && (stats.f_flags & 0xf) == 0xf,
	    "copied-seal-flags");
	errno = 0;
	expect_error(open(path("target/file", p), O_WRONLY), EROFS,
	    "copied-view-readonly");
out:
	errno = 0;
	expect_error(syscall(SYS_getppid), EACCES, "filter-retained");
	(void) printf("MNF_PASS uid=%lu denied=%d failures=%d base=%s\n",
	    (unsigned long)uid, denied, failures, base);
	return (failures != 0);
}
