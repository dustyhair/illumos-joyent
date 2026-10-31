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
		(void) printf("MNP_FAIL name=%s errno=%d\n", name, errno);
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
	int drop = 0, deny = 0, shared = 0, outsidefd, tempfd, fd;
	uid_t uid;
	gid_t gid;
	char candidate[PATH_MAX], p[PATH_MAX], oldpath[PATH_MAX];
	char data[16] = { 0 };
	struct statfs stats;

	(void) setvbuf(stdout, NULL, _IONBF, 0);
	(void) alarm(60);
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--drop") == 0)
			drop = 1;
		else if (strcmp(argv[i], "--lx-shared") == 0)
			shared = 1;
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
	(void) snprintf(base, sizeof (base), "/test/mnt-pivot/run.XXXXXX");
	if (mkdtemp(base) == NULL ||
	    mkdir(path("candidate", candidate), 0755) ||
	    mkdir(path("plain", p), 0755) ||
	    write_text(path("outside", p), "outside\n", 1))
		return (2);
	if (unshare(CLONE_NEWUSER | CLONE_NEWNS) ||
	    maps(uid, gid) || filter() ||
	    mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) ||
	    mount("tmpfs", candidate, "tmpfs", MS_NOSUID | MS_NODEV,
	    "mode=0755"))
		return (2);
	if (chdir(candidate) || mkdir("oldroot", 0755) ||
	    mkdir("newroot", 0755) ||
	    write_text("temporary", "temporary\n", 1) ||
	    mount("newroot", "newroot", NULL, MS_BIND | MS_REC, NULL))
		return (2);
	if (deny) {
		struct __user_cap_header_struct h = {
		    _LINUX_CAPABILITY_VERSION_3, 0 };
		struct __user_cap_data_struct caps[2];

		if (syscall(SYS_capget, &h, caps) != 0)
			return (2);
		caps[0].effective &= ~(1U << CAP_SYS_ADMIN);
		caps[0].permitted &= ~(1U << CAP_SYS_ADMIN);
		if (syscall(SYS_capset, &h, caps) != 0)
			return (2);
		errno = 0;
		check(syscall(SYS_pivot_root, candidate, "oldroot") == -1 &&
		    errno == EPERM, "pivot-requires-owner-capability");
		check(contents(path("outside", p), "outside\n"),
		    "denied-pivot-root-unchanged");
		goto done;
	}
	errno = 0;
	check(syscall(SYS_pivot_root, (void *)1, "oldroot") == -1 &&
	    errno == EFAULT, "bad-new-root-pointer");
	errno = 0;
	check(syscall(SYS_pivot_root, path("plain", p), "oldroot") == -1 &&
	    errno == EINVAL, "new-root-must-be-mounted");
	errno = 0;
	check(syscall(SYS_pivot_root, candidate, (void *)1) == -1 &&
	    errno == EFAULT, "bad-put-old-pointer");
	errno = 0;
	check(syscall(SYS_pivot_root, "temporary", "oldroot") == -1 &&
	    errno == ENOTDIR, "new-root-must-be-directory");
	errno = 0;
	check(syscall(SYS_pivot_root, candidate, "temporary") == -1 &&
	    errno == ENOTDIR, "put-old-must-be-directory");
	errno = 0;
	check(syscall(SYS_pivot_root, candidate, base) == -1 &&
	    errno == EBUSY, "put-old-outside-new-root");
	errno = 0;
	check(syscall(SYS_pivot_root, candidate, "missing") == -1 &&
	    errno == ENOENT, "put-old-must-exist");
	check(contents("temporary", "temporary\n") &&
	    contents(path("outside", p), "outside\n"),
	    "invalid-pivots-leave-root-and-cwd-unchanged");
	if (shared) {
		int channel[2], status;
		pid_t child;

		if (pipe(channel) != 0 || (child = fork()) < 0)
			return (2);
		if (child == 0) {
			char byte;

			(void) close(channel[1]);
			_exit(read(channel[0], &byte, 1) == 0 ? 0 : 2);
		}
		(void) close(channel[0]);
		errno = 0;
		check(syscall(SYS_pivot_root, candidate, "oldroot") == -1 &&
		    errno == EOPNOTSUPP, "shared-view-explicitly-unsupported");
		check(contents("temporary", "temporary\n") &&
		    contents(path("outside", p), "outside\n"),
		    "shared-rejection-preserves-root-and-cwd");
		(void) close(channel[1]);
		check(waitpid(child, &status, 0) == child &&
		    WIFEXITED(status) && WEXITSTATUS(status) == 0,
		    "shared-child-released");
	}
	outsidefd = open(base, O_PATH | O_DIRECTORY);
	tempfd = open(candidate, O_PATH | O_DIRECTORY);
	check(outsidefd >= 0 && tempfd >= 0, "save-root-descriptors");
	check(syscall(SYS_pivot_root, candidate, "oldroot") == 0,
	    "pivot-to-temporary-tmpfs");
	if (failures != 0)
		return (1);
	check(chdir("/") == 0 && contents("/temporary", "temporary\n"),
	    "temporary-root-visible");
	check(statfs("/", &stats) == 0 && stats.f_type == 0x01021994,
	    "temporary-root-tmpfs-statistics");
	check(snprintf(oldpath, sizeof (oldpath), "/oldroot%s/outside", base) <
	    (int)sizeof (oldpath) && contents(oldpath, "outside\n"),
	    "old-root-reachable");
	check(mount("tmpfs", "/newroot", "tmpfs", MS_NOSUID | MS_NODEV,
	    "mode=0755") == 0 &&
	    write_text("/newroot/final", "final\n", 1) == 0,
	    "prepare-final-root");
	check(mount(NULL, "/oldroot", NULL, MS_REC | MS_PRIVATE, NULL) == 0,
	    "old-root-private");
	check(umount2("/oldroot", MNT_DETACH) == 0, "detach-original-root");
	fd = openat(outsidefd, "outside", O_RDONLY);
	check(fd >= 0 && read(fd, data, sizeof (data) - 1) == 8 &&
	    strcmp(data, "outside\n") == 0, "detached-original-descriptor");
	(void) close(fd);
	check(chdir("/newroot") == 0, "enter-final-root");
	check(syscall(SYS_pivot_root, ".", ".") == 0,
	    "equal-new-root-and-put-old");
	if (failures != 0)
		return (1);
	check(fchdir(tempfd) == 0, "return-to-covered-temporary-root");
	check(umount2(".", MNT_DETACH) == 0, "detach-temporary-root-via-cwd");
	check(chdir("/") == 0 && contents("/final", "final\n"),
	    "final-root-visible");
	check(access("/temporary", F_OK) == -1 && errno == ENOENT,
	    "temporary-root-hidden");
	check(fchdir(outsidefd) == 0, "held-cwd-outside-new-root");
	errno = 0;
	check(getcwd(p, sizeof (p)) == NULL && errno == ENOENT,
	    "detached-cwd-unreachable");
	check(chdir("/") == 0 && unshare(CLONE_NEWNS) == 0 &&
	    contents("/final", "final\n"), "clone-final-active-root");
	(void) close(tempfd);
	(void) close(outsidefd);
done:
	errno = 0;
	check(syscall(SYS_getppid) == -1 && errno == EACCES, "filter-retained");
	(void) printf("MNP_PASS uid=%lu deny=%d failures=%d base=%s\n",
	    (unsigned long)uid, deny, failures, base);
	return (failures != 0);
}
