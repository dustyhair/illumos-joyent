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
		(void) printf("MNRM_FAIL name=%s errno=%d\n", name, errno);
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

static void
remount(const char *target, int readonly, int want, const char *name)
{
	int rv;
	unsigned long flags = MS_BIND | MS_REMOUNT | MS_NOSUID | MS_NODEV |
	    MS_SILENT | MS_RELATIME;

	if (readonly)
		flags |= MS_RDONLY;
	errno = 0;
	rv = mount("none", target, NULL, flags, NULL);
	check(want ? rv == -1 && errno == want : rv == 0, name);
}

int
main(int argc, char **argv)
{
	int drop = 0, denied = 0, fd, sourcefd;
	uid_t uid;
	gid_t gid;
	char source[PATH_MAX], target[PATH_MAX], p[PATH_MAX];
	char *mapping;
	const long page = sysconf(_SC_PAGESIZE);

	(void) setvbuf(stdout, NULL, _IONBF, 0);
	(void) alarm(60);
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--drop") == 0)
			drop = 1;
		else if (strcmp(argv[i], "--deny") == 0)
			denied = 1;
		else
			return (2);
	}
	if (drop && (setgroups(0, NULL) || setgid(65534) || setuid(65534)))
		return (2);
	uid = getuid();
	gid = getgid();
	if (prctl(PR_SET_DUMPABLE, 1UL, 0UL, 0UL, 0UL))
		return (2);
	(void) snprintf(base, sizeof (base), "/test/mnt-remount/run.XXXXXX");
	if (mkdtemp(base) == NULL || mkdir(path("source", source), 0755) ||
	    mkdir(path("target", target), 0755) ||
	    write_text(path("source/file", p), "source\n", 1))
		return (2);
	if (unshare(CLONE_NEWUSER | CLONE_NEWNS) || maps(uid, gid) || filter() ||
	    mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) ||
	    mount(source, target, NULL, MS_BIND, NULL))
		return (2);
	if (denied) {
		struct __user_cap_header_struct h = {
			.version = _LINUX_CAPABILITY_VERSION_3 };
		struct __user_cap_data_struct caps[2] = { 0 };

		if (syscall(SYS_capget, &h, caps))
			return (2);
		caps[0].effective &= ~(1U << CAP_SYS_ADMIN);
		if (syscall(SYS_capset, &h, caps))
			return (2);
		remount(target, 1, EPERM, "denied-remount");
		goto out;
	}
	path("target/file", p);
	fd = open(p, O_RDWR);
	check(fd >= 0, "open-target-writer");
	remount(target, 1, EBUSY, "writer-prevents-readonly");
	(void) close(fd);
	sourcefd = open(path("source/file", p), O_RDWR);
	check(sourcefd >= 0, "open-source-writer");
	remount(target, 1, 0, "source-writer-does-not-block-target");
	check(write(sourcefd, "x", 1) == 1, "source-write-still-works");
	(void) close(sourcefd);
	errno = 0;
	check(open(path("target/file", p), O_WRONLY) == -1 && errno == EROFS,
	    "readonly-open");
	errno = 0;
	check(chmod(p, 0600) == -1 && errno == EROFS, "readonly-chmod");
	errno = 0;
	check(unlink(p) == -1 && errno == EROFS, "readonly-unlink");
	fd = open(p, O_RDONLY);
	check(fd >= 0, "open-readonly-descriptor");
	errno = 0;
	check(fchmod(fd, 0600) == -1 && errno == EROFS, "readonly-fchmod");
	mapping = mmap(NULL, (size_t)page, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE, fd, 0);
	check(mapping != MAP_FAILED, "private-writable-map-on-readonly-mount");
	if (mapping != MAP_FAILED) {
		mapping[0] = 'p';
		(void) munmap(mapping, (size_t)page);
	}
	check(contents(p, "xource\n"), "private-map-leaves-file-unchanged");
	(void) close(fd);
	remount(target, 0, 0, "restore-writable-mount");
	fd = open(p, O_RDWR);
	check(fd >= 0, "open-for-shared-map");
	mapping = mmap(NULL, (size_t)page, PROT_READ | PROT_WRITE,
	    MAP_SHARED, fd, 0);
	check(mapping != MAP_FAILED, "shared-writable-map");
	(void) close(fd);
	if (mapping != MAP_FAILED) {
		remount(target, 1, EBUSY, "shared-map-outlives-writer-fd");
		mapping[0] = 's';
		(void) munmap(mapping, (size_t)page);
	}
	remount(target, 1, 0, "readonly-after-shared-map-unmapped");
	fd = open(p, O_RDONLY);
	check(fd >= 0, "open-readonly-hold");
	remount(target, 1, 0, "readonly-hold-does-not-block");
	(void) close(fd);
	check(unshare(CLONE_NEWNS) == 0, "copy-readonly-namespace");
	errno = 0;
	check(open(p, O_WRONLY) == -1 && errno == EROFS,
	    "copied-mount-retains-readonly");
out:
	errno = 0;
	check(syscall(SYS_getppid) == -1 && errno == EACCES, "retained-seccomp");
	(void) printf("MNRM_PASS uid=%lu denied=%d failures=%d base=%s\n",
	    (unsigned long)uid, denied, failures, base);
	return (failures != 0);
}
