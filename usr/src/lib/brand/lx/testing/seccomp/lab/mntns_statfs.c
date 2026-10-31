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
#include <sys/socket.h>
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
		(void) printf("MNFS_FAIL name=%s errno=%d\n", name, errno);
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

static int
block_stats(void)
{
	struct sock_filter code[] = {
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_statfs, 1, 0),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_fstatfs, 0, 1),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EIO),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)
	};
	struct sock_fprog fp = { sizeof (code) / sizeof (code[0]), code };

	return (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &fp));
}

int
main(int argc, char **argv)
{
	int drop = 0, linux_control = 0, oldfd, fd, pipefd[2], sockets[2];
	uid_t uid;
	gid_t gid;
	char target[PATH_MAX], p[PATH_MAX], alias[80];
	struct statfs original, a, b;
	struct __user_cap_header_struct h = {
		.version = _LINUX_CAPABILITY_VERSION_3 };
	struct __user_cap_data_struct caps[2] = { 0 };

	(void) setvbuf(stdout, NULL, _IONBF, 0);
	(void) alarm(60);
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--drop") == 0)
			drop = 1;
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
	(void) snprintf(base, sizeof (base), "/test/mnt-statfs/run.XXXXXX");
	if (mkdtemp(base) == NULL || mkdir(path("target", target), 0700) ||
	    (oldfd = open(target, O_PATH | O_DIRECTORY)) < 0 ||
	    fstatfs(oldfd, &original))
		return (2);
	if (unshare(CLONE_NEWUSER | CLONE_NEWNS) || maps(uid, gid) || filter())
		return (2);
	if (linux_control && mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL))
		return (2);
	if (mount("tmpfs", target, "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC,
	    "size=2M,mode=0755"))
		return (2);
	check(statfs(target, &a) == 0, "path-statfs");
	check(a.f_type == 0x01021994 && a.f_bsize == 4096 && a.f_frsize == 4096,
	    "tmpfs-type-and-block-size");
	check(a.f_blocks == 512 && a.f_namelen == 255, "limit-and-name-size");
	check((a.f_flags & 0xe) == 0xe, "path-mount-restrictions");
	fd = open(target, O_PATH | O_DIRECTORY);
	check(fd >= 0 && fstatfs(fd, &b) == 0 && b.f_type == a.f_type &&
	    b.f_blocks == a.f_blocks && (b.f_flags & 0xe) == 0xe,
	    "held-private-descriptor");
	(void) snprintf(alias, sizeof (alias), "/proc/self/fd/%d", fd);
	check(statfs(alias, &b) == 0 && b.f_type == a.f_type &&
	    b.f_blocks == a.f_blocks && (b.f_flags & 0xe) == 0xe,
	    "magic-descriptor-path");
	check(fstatfs(oldfd, &b) == 0 && b.f_type == original.f_type &&
	    b.f_type != a.f_type, "covered-descriptor-keeps-old-filesystem");
	check(chdir(target) == 0 && statfs(".", &b) == 0 &&
	    b.f_type == a.f_type, "relative-path");
	check(chdir("/") == 0, "leave-private-cwd");
	errno = 0;
	check(syscall(SYS_statfs, (void *)1, &b) == -1 && errno == EFAULT,
	    "invalid-path-pointer");
	errno = 0;
	check(syscall(SYS_statfs, target, (void *)1) == -1 && errno == EFAULT,
	    "invalid-path-output");
	errno = 0;
	check(syscall(SYS_fstatfs, fd, (void *)1) == -1 && errno == EFAULT,
	    "invalid-descriptor-output");
	errno = 0;
	check(fstatfs(-1, &b) == -1 && errno == EBADF, "bad-descriptor");
	errno = 0;
	check(statfs(path("missing", p), &b) == -1 && errno == ENOENT,
	    "missing-path");
	check(pipe(pipefd) == 0, "pipe-setup");
	check(fstatfs(pipefd[0], &b) == 0 && b.f_type == 0x50495045 &&
	    b.f_blocks == 0 && b.f_frsize == 4096, "pipe-filesystem");
	(void) close(pipefd[0]);
	(void) close(pipefd[1]);
	check(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "socket-setup");
	check(fstatfs(sockets[0], &b) == 0 && b.f_type == 0x534f434b &&
	    b.f_blocks == 0, "socket-filesystem");
	(void) close(sockets[0]);
	(void) close(sockets[1]);
	check(unshare(CLONE_NEWNS) == 0, "copy-private-view");
	check(statfs(target, &b) == 0 && b.f_type == a.f_type &&
	    b.f_blocks == a.f_blocks && (b.f_flags & 0xe) == 0xe,
	    "copied-view-retains-restrictions");
	check(syscall(SYS_capget, &h, caps) == 0, "capget");
	caps[0].effective &= ~(1U << CAP_SYS_ADMIN);
	check(syscall(SYS_capset, &h, caps) == 0, "drop-admin");
	check(statfs(target, &b) == 0 && fstatfs(fd, &b) == 0,
	    "statistics-require-no-admin-capability");
	check(block_stats() == 0, "stack-statfs-filter");
	errno = 0;
	check(statfs(target, &b) == -1 && errno == EIO, "filter-path-statfs");
	errno = 0;
	check(fstatfs(fd, &b) == -1 && errno == EIO, "filter-descriptor-statfs");
	errno = 0;
	check(syscall(SYS_statfs, (void *)1, &b) == -1 && errno == EIO,
	    "filter-precedes-path-lookup");
	errno = 0;
	check(syscall(SYS_getppid) == -1 && errno == EACCES,
	    "previous-filter-retained");
	(void) close(fd);
	(void) close(oldfd);
	(void) printf("MNFS_PASS uid=%lu failures=%d base=%s\n",
	    (unsigned long)uid, failures, base);
	return (failures != 0);
}
