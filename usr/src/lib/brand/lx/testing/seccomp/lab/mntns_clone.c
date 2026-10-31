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
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define	CHECK(x, name) do { \
	if (!(x)) { \
		(void) printf("MCL_FAIL name=%s errno=%d line=%d\n", \
		    name, errno, __LINE__); \
		return (1); \
	} \
} while (0)

static struct context {
	int ready[2], release[2], filter;
	char base[PATH_MAX], target[PATH_MAX], file[PATH_MAX];
} ctx;

static int
put(const char *path, const char *text, int create)
{
	int fd = open(path, O_WRONLY | (create ? O_CREAT | O_TRUNC : 0), 0644);
	ssize_t n;

	if (fd < 0)
		return (-1);
	n = write(fd, text, strlen(text));
	(void) close(fd);
	return (n == (ssize_t)strlen(text) ? 0 : -1);
}

static int
maps(pid_t pid, uid_t uid, gid_t gid)
{
	char path[96], text[96];

	(void) snprintf(path, sizeof (path), "/proc/%d/setgroups", pid);
	CHECK(put(path, "deny\n", 0) == 0, "deny-setgroups");
	(void) snprintf(path, sizeof (path), "/proc/%d/uid_map", pid);
	(void) snprintf(text, sizeof (text), "0 %lu 1\n", (unsigned long)uid);
	CHECK(put(path, text, 0) == 0, "map-uid");
	(void) snprintf(path, sizeof (path), "/proc/%d/gid_map", pid);
	(void) snprintf(text, sizeof (text), "0 %lu 1\n", (unsigned long)gid);
	CHECK(put(path, text, 0) == 0, "map-gid");
	return (0);
}

static int
filtered(void)
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
filter_check(void)
{
	if (ctx.filter) {
		errno = 0;
		CHECK(syscall(SYS_getppid) == -1 && errno == EACCES,
		    "inherited-filter");
	}
	return (0);
}

static int
child_main(void *arg)
{
	char byte, path[PATH_MAX], cwd[PATH_MAX];
	struct stat st;
	int fd;

	(void) arg;
	(void) close(ctx.ready[0]);
	(void) close(ctx.release[1]);
	CHECK(write(ctx.ready[1], "S", 1) == 1, "child-ready");
	CHECK(read(ctx.release[0], &byte, 1) == 1, "child-mapped");
	CHECK(geteuid() == 0 && getegid() == 0, "mapped-child-identity");
	CHECK(filter_check() == 0, "child-filter-before-mount");
	CHECK(mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) == 0,
	    "child-private-propagation");
	CHECK(mount("tmpfs", ctx.target, "tmpfs", 0, "mode=0755") == 0,
	    "child-private-tmpfs");
	CHECK(chdir(ctx.target) == 0 && getcwd(cwd, sizeof (cwd)) != NULL &&
	    strcmp(cwd, ctx.target) == 0, "child-private-cwd");
	CHECK(put("child", "private\n", 1) == 0, "child-private-file");
	CHECK(snprintf(path, sizeof (path), "%s/covered", ctx.target) <
	    (int)sizeof (path), "child-covered-path");
	errno = 0;
	CHECK(stat(path, &st) == -1 && errno == ENOENT,
	    "child-covered-file-hidden");
	CHECK(write(ctx.ready[1], "M", 1) == 1, "child-mounted");
	CHECK(read(ctx.release[0], &byte, 1) == 1, "parent-isolation-checked");
	CHECK(chdir("/") == 0 && unshare(CLONE_NEWNS) == 0,
	    "child-can-copy-cloned-namespace");
	CHECK(snprintf(path, sizeof (path), "%s/child", ctx.target) <
	    (int)sizeof (path), "child-copy-path");
	CHECK((fd = open(path, O_RDONLY)) >= 0, "child-copy-retains-mount");
	(void) close(fd);
	CHECK(filter_check() == 0, "child-filter-after-copy");
	return (0);
}

int
main(int argc, char **argv)
{
	int drop = 0, mountonly = 0, nested = 0, raw = 0, deny = 0;
	int expect = 0, flags, status, fd;
	uid_t uid;
	gid_t gid;
	pid_t child;
	char byte, before[PATH_MAX], after[PATH_MAX], data[16] = { 0 };
	void *stack;

	(void) setvbuf(stdout, NULL, _IONBF, 0);
	(void) alarm(60);
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--drop") == 0)
			drop = 1;
		else if (strcmp(argv[i], "--mount-only") == 0)
			mountonly = 1;
		else if (strcmp(argv[i], "--nested") == 0)
			nested = 1;
		else if (strcmp(argv[i], "--raw") == 0)
			raw = 1;
		else if (strcmp(argv[i], "--filter") == 0)
			ctx.filter = 1;
		else if (strcmp(argv[i], "--deny") == 0)
			deny = mountonly = 1;
		else if (strcmp(argv[i], "--expect") == 0 && i + 1 < argc)
			expect = atoi(argv[++i]);
		else
			return (2);
	}
	if (drop)
		CHECK(setgroups(0, NULL) == 0 && setgid(65534) == 0 &&
		    setuid(65534) == 0, "drop-identity");
	CHECK(prctl(PR_SET_DUMPABLE, 1UL, 0UL, 0UL, 0UL) == 0,
	    "self-mapping-dumpable");
	uid = getuid();
	gid = getgid();
	if (mountonly || nested) {
		CHECK(unshare(CLONE_NEWUSER | CLONE_NEWNS) == 0,
		    "prepare-parent-namespace");
		CHECK(maps(getpid(), uid, gid) == 0, "map-parent");
		uid = getuid();
		gid = getgid();
	}
	if (ctx.filter)
		CHECK(filtered() == 0, "install-parent-filter");
	CHECK(getcwd(before, sizeof (before)) != NULL, "save-parent-cwd");
	(void) snprintf(ctx.base, sizeof (ctx.base),
	    "/test/mnt-clone/run.XXXXXX");
	CHECK(mkdtemp(ctx.base) != NULL, "create-base");
	CHECK(snprintf(ctx.target, sizeof (ctx.target), "%s/target", ctx.base) <
	    (int)sizeof (ctx.target), "target-path");
	CHECK(mkdir(ctx.target, 0755) == 0, "target-directory");
	CHECK(snprintf(ctx.file, sizeof (ctx.file), "%s/covered", ctx.target) <
	    (int)sizeof (ctx.file), "covered-path");
	CHECK(put(ctx.file, "parent\n", 1) == 0, "parent-file");
	if (deny) {
		struct __user_cap_header_struct h = {
		    _LINUX_CAPABILITY_VERSION_3, 0 };
		struct __user_cap_data_struct caps[2];

		CHECK(syscall(SYS_capget, &h, caps) == 0, "read-parent-caps");
		caps[0].effective &= ~(1U << CAP_SYS_ADMIN);
		CHECK(syscall(SYS_capset, &h, caps) == 0,
		    "drop-mount-capability");
		expect = EPERM;
	}
	CHECK(pipe(ctx.ready) == 0 && pipe(ctx.release) == 0, "create-pipes");
	CHECK((stack = malloc(65536)) != NULL, "allocate-child-stack");
	flags = CLONE_NEWNS | SIGCHLD | (mountonly ? 0 : CLONE_NEWUSER);
	if (raw) {
		child = syscall(SYS_clone, flags, NULL, NULL, NULL, 0);
		if (child == 0)
			_exit(child_main(NULL));
	} else
		child = clone(child_main, (char *)stack + 65536, flags, NULL);
	if (expect) {
		CHECK(child == -1 && errno == expect,
		    "clone-expected-rejection");
	} else {
		CHECK(child > 0, "create-namespace-child");
		(void) close(ctx.ready[1]);
		(void) close(ctx.release[0]);
		CHECK(read(ctx.ready[0], &byte, 1) == 1 && byte == 'S',
		    "read-child-ready");
		if (!mountonly)
			CHECK(maps(child, uid, gid) == 0, "parent-maps-child");
		CHECK(write(ctx.release[1], "G", 1) == 1,
		    "release-mapped-child");
		CHECK(read(ctx.ready[0], &byte, 1) == 1 && byte == 'M',
		    "read-mounted-child");
		CHECK((fd = open(ctx.file, O_RDONLY)) >= 0 &&
		    read(fd, data, sizeof (data)) == 7 &&
		    strcmp(data, "parent\n") == 0, "parent-mount-unchanged");
		(void) close(fd);
		CHECK(write(ctx.release[1], "G", 1) == 1,
		    "release-checked-child");
		CHECK(waitpid(child, &status, 0) == child &&
		    WIFEXITED(status) && WEXITSTATUS(status) == 0,
		    "child-exits-cleanly");
	}
	CHECK(getcwd(after, sizeof (after)) != NULL &&
	    strcmp(before, after) == 0, "parent-cwd-unchanged");
	CHECK(geteuid() == uid && getegid() == gid,
	    "parent-identity-unchanged");
	CHECK(filter_check() == 0, "parent-filter-retained");
	free(stack);
	(void) printf("MCL_PASS uid=%lu mountonly=%d nested=%d raw=%d "
	    "filter=%d expect=%d\n", (unsigned long)uid,
	    mountonly, nested, raw, ctx.filter, expect);
	return (0);
}
