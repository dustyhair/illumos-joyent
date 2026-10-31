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
		(void) printf("MNT_FAIL name=%s errno=%d\n", name, errno);
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
root_attributes(const char *target, mode_t mode)
{
	struct stat st;

	if (stat(target, &st) != 0) {
		check(0, "stat-mounted-root");
		return;
	}
	check((st.st_mode & 07777) == mode, "initial-root-mode");
	check(st.st_uid == 0 && st.st_gid == 0, "initial-root-ownership");
}

static void
basic(const char *target)
{
	char p[PATH_MAX], token;
	int pipefd[2], status;
	pid_t peer;

	if (chdir(base) != 0 || mount(NULL, "target", "tmpfs", 0, NULL) != 0) {
		check(0, "default-tmpfs");
		return;
	}
	check(chdir("/") == 0, "leave-relative-mount-cwd");
	root_attributes(target, 01777);
	check(access(path("target/covered", p), F_OK) == -1 && errno == ENOENT,
	    "covered-file-hidden");
	check(write_text(path("target/shared", p), "shared\n", 1) == 0,
	    "write-default-tmpfs");
	check(pipe(pipefd) == 0, "peer-pipe");
	peer = fork();
	if (peer == 0) {
		(void) close(pipefd[1]);
		if (read(pipefd[0], &token, 1) != 1)
			_exit(90);
		check(contents(path("target/shared", p), "edited\n"),
		    "peer-shared-superblock-uncovered");
		(void) printf("MNT_PEER_PASS failures=%d\n", failures);
		_exit(failures != 0);
	}
	(void) close(pipefd[0]);
	if (peer < 0)
		_exit(91);
	check(unshare(CLONE_NEWNS) == 0, "copy-tmpfs-namespace");
	check(write_text(path("target/shared", p), "edited\n", 0) == 0,
	    "copy-shares-existing-superblock");
	check(mount("ignored", target, "tmpfs", MS_NOSUID | MS_NODEV,
	    "mode=0755") == 0, "copied-view-new-superblock");
	root_attributes(target, 0755);
	check(access(path("target/shared", p), F_OK) == -1 && errno == ENOENT,
	    "new-superblock-hides-old");
	check(write(pipefd[1], "x", 1) == 1, "peer-release");
	(void) close(pipefd[1]);
	check(waitpid(peer, &status, 0) == peer && WIFEXITED(status) &&
	    WEXITSTATUS(status) == 0, "peer-exit");
}

static void
options(const char *target, int outsidefd)
{
	const char *bad[] = { "mode=888", "uid=1", "gid=1", "size=1KK",
	    "size=garbage", "uid=4294967295",
	    "gid=4294967295", "mode=755x", "uid=0x" };
	char p[PATH_MAX], bytes[4096] = { 0 }, fdpath[80];
	struct statfs before, after;
	struct sysinfo memory;
	int fd, status;
	size_t total = 0;
	pid_t child;

	(void) snprintf(fdpath, sizeof (fdpath), "/proc/self/fd/%d", outsidefd);
	errno = 0;
	check(mount("tmpfs", fdpath, "tmpfs", 0, NULL) == -1 && errno == EINVAL,
	    "foreign-target-descriptor");
	errno = 0;
	check(syscall(SYS_mount, (void *)1, target, "tmpfs", 0, NULL) == -1 &&
	    errno == EFAULT, "invalid-source-pointer");
	errno = 0;
	check(mount("tmpfs", path("missing", p), "tmpfs", 0, NULL) == -1 &&
	    errno == ENOENT, "missing-target");
	errno = 0;
	check(mount("tmpfs", path("target/covered", p), "tmpfs", 0, NULL) == -1 &&
	    errno == ENOTDIR, "file-target");
	for (size_t i = 0; i < sizeof (bad) / sizeof (bad[0]); i++) {
		errno = 0;
		check(mount("tmpfs", target, "tmpfs", 0, bad[i]) == -1 &&
		    errno == EINVAL, bad[i]);
		check(contents(path("target/covered", p), "covered\n"),
		    "bad-options-leave-target-unchanged");
	}
	fd = open(target, O_RDONLY | O_DIRECTORY);
	check(fd >= 0, "private-target-descriptor");
	(void) snprintf(fdpath, sizeof (fdpath), "/proc/self/fd/%d", fd);
	if (mount("tmpfs", fdpath, "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC,
	    "mode=0700,mode=0750,uid=0,gid=0,size=1M") != 0) {
		check(0, "explicit-options");
		(void) close(fd);
		return;
	}
	(void) close(fd);
	root_attributes(target, 0750);
	check(statfs(target, &before) == 0, "statfs-before-fill");
	fd = open(path("target/large", p), O_WRONLY | O_CREAT, 0600);
	check(fd >= 0, "open-bounded-file");
	if (fd >= 0) {
		ssize_t n;
		while (total < 2 * 1024 * 1024 &&
		    (n = write(fd, bytes, sizeof (bytes))) > 0)
			total += (size_t)n;
		check(total >= 65536 && total <= 1024 * 1024 && errno == ENOSPC,
		    "size-cap-enforced");
		(void) close(fd);
		check(unlink(p) == 0, "unlink-bounded-file");
	}
	check(statfs(target, &after) == 0 && after.f_bfree >= before.f_bfree - 2,
	    "released-file-pages");
	path("target/script", p);
	check(write_text(p, "#!/bin/sh\nexit 0\n", 1) == 0 && chmod(p, 0755) == 0,
	    "prepare-noexec-script");
	child = fork();
	if (child == 0) {
		execl(p, p, (char *)NULL);
		_exit(errno == EACCES ? 0 : 92);
	}
	check(child > 0 && waitpid(child, &status, 0) == child &&
	    WIFEXITED(status) && WEXITSTATUS(status) == 0, "noexec-enforced");
	check(mount(NULL, target, "tmpfs", 0, "size=0,mode=1777") == 0,
	    "zero-size-unlimited");
	root_attributes(target, 01777);
	check(write_text(path("target/zero", p), "zero\n", 1) == 0,
	    "write-zero-size");
	check(mount("tmpfs", target, "tmpfs", 0, "size=1%,mode=0755") == 0,
	    "percentage-size");
	if (sysinfo(&memory) == 0 && statfs(target, &after) == 0) {
		unsigned long long page = (unsigned long long)sysconf(_SC_PAGESIZE);
		unsigned long long pages = (unsigned long long)memory.totalram *
		    memory.mem_unit / page;

		check((unsigned long long)after.f_blocks == (pages + 99) / 100,
		    "percentage-rounds-up-to-page");
	} else {
		check(0, "percentage-statfs");
	}
	(void) printf("MNT_SIZE_PASS bytes=%zu\n", total);
}

int
main(int argc, char **argv)
{
	int drop = 0, denied = 0, linux_control = 0, option_case = 0, outsidefd;
	uid_t uid;
	gid_t gid;
	char p[PATH_MAX], target[PATH_MAX];

	(void) setvbuf(stdout, NULL, _IONBF, 0);
	(void) alarm(60);
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--drop") == 0)
			drop = 1;
		else if (strcmp(argv[i], "--deny") == 0)
			denied = 1;
		else if (strcmp(argv[i], "--linux") == 0)
			linux_control = 1;
		else if (strcmp(argv[i], "--options") == 0)
			option_case = 1;
		else
			return (2);
	}
	if (drop && (setgroups(0, NULL) || setgid(65534) || setuid(65534)))
		return (2);
	uid = getuid();
	gid = getgid();
	if (prctl(PR_SET_DUMPABLE, 1UL, 0UL, 0UL, 0UL))
		return (2);
	(void) snprintf(base, sizeof (base), "/test/mnt-tmpfs/run.XXXXXX");
	if (mkdtemp(base) == NULL || mkdir(path("target", target), 0700) ||
	    write_text(path("target/covered", p), "covered\n", 1))
		return (2);
	if ((outsidefd = open(target, O_RDONLY | O_DIRECTORY)) < 0)
		return (2);
	if (unshare(CLONE_NEWUSER | CLONE_NEWNS) || maps(uid, gid) || filter())
		return (2);
	if (linux_control && mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL))
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
		errno = 0;
		check(mount("tmpfs", target, "tmpfs", MS_NODEV, "mode=0755") == -1 &&
		    errno == EPERM, "denied-tmpfs");
		check(contents(path("target/covered", p), "covered\n"),
		    "denied-target-unchanged");
	} else if (option_case) {
		options(target, outsidefd);
	} else {
		basic(target);
	}
	(void) close(outsidefd);
	errno = 0;
	check(syscall(SYS_getppid) == -1 && errno == EACCES, "retained-seccomp");
	(void) printf("MNT_PASS uid=%lu denied=%d options=%d failures=%d base=%s\n",
	    (unsigned long)uid, denied, option_case, failures, base);
	return (failures != 0);
}
