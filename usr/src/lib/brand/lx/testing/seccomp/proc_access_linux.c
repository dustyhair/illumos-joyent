#define _GNU_SOURCE
/* CDDL version 1.0, http://www.illumos.org/license/CDDL. */
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <linux/capability.h>
#include <sched.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <stdlib.h>

#define ALL ((UINT64_C(1) << 41) - 1)
#define TRACE (UINT64_C(1) << 19)

struct scenario {
	const char *name;
	unsigned int source_depth, target_depth;
	uint64_t effective, permitted, target_permitted;
	int dumpable, allow, execute;
};

static int
write_text(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return errno;
	int error = write(fd, text, strlen(text)) == (ssize_t)strlen(text) ? 0 : errno;
	close(fd);
	return error;
}

static int
enter_namespaces(unsigned int depth)
{
	for (unsigned int i = 0; i < depth; i++) {
		int error;
		if (unshare(CLONE_NEWUSER) != 0)
			return errno;
		if ((error = write_text("/proc/self/uid_map", "0 0 1\n")) != 0 ||
		    (error = write_text("/proc/self/setgroups", "deny\n")) != 0 ||
		    (error = write_text("/proc/self/gid_map", "0 0 1\n")) != 0)
			return error;
	}
	return 0;
}

static int
set_caps(uint64_t e, uint64_t p)
{
	struct __user_cap_header_struct h = { _LINUX_CAPABILITY_VERSION_3, 0 };
	struct __user_cap_data_struct d[2] = {
		{ e, p, 0 }, { e >> 32, p >> 32, 0 }
	};
	return syscall(SYS_capset, &h, d) == 0 ? 0 : errno;
}

static int
target_ready(int ready, int finish, uint64_t permitted, int dumpable)
{
	char byte;
	int error = set_caps(permitted, permitted);
	if (error == 0 && prctl(PR_SET_DUMPABLE, dumpable, 0, 0, 0) != 0)
		error = errno;
	if (write(ready, &error, sizeof (error)) != sizeof (error))
		return 2;
	if (error == 0 && read(finish, &byte, 1) != 1)
		error = EIO;
	return error == 0 ? 0 : 2;
}

static int
run_case(const struct scenario *s)
{
	int ready[2], finish[2], status, error = 0;
	char byte, path[80];
	if (pipe(ready) != 0 || pipe(finish) != 0)
		return 2;
	pid_t target = fork();
	if (target == 0) {
		close(ready[0]); close(finish[1]);
		error = enter_namespaces(s->target_depth);
		if (error != 0) {
			if (write(ready[1], &error, sizeof (error)) != sizeof (error))
				_exit(2);
			_exit(2);
		}
		if (s->execute) {
			char r[20], f[20], p[30], d[20];
			snprintf(r, sizeof (r), "%d", ready[1]);
			snprintf(f, sizeof (f), "%d", finish[0]);
			snprintf(p, sizeof (p), "%llx",
			    (unsigned long long)s->target_permitted);
			snprintf(d, sizeof (d), "%d", s->dumpable);
			execl("/proc/self/exe", "proc_access_linux", "--target",
			    r, f, p, d, NULL);
			error = errno;
			if (write(ready[1], &error, sizeof (error)) != sizeof (error))
				_exit(2);
			_exit(2);
		}
		_exit(target_ready(ready[1], finish[0], s->target_permitted,
		    s->dumpable));
	}
	close(ready[1]); close(finish[0]);
	if (target < 0 || read(ready[0], &error, sizeof (error)) != sizeof (error))
		error = EIO;
	if (error == 0)
		error = enter_namespaces(s->source_depth);
	if (error == 0)
		error = set_caps(s->effective, s->permitted);
	int failed = error != 0;
	if (error == 0) {
		snprintf(path, sizeof (path), "/proc/%d/maps", target);
		int fd = open(path, O_RDONLY | O_CLOEXEC);
		int allow = fd >= 0;
		if (fd >= 0) {
			if (read(fd, &byte, 1) != 1)
				failed = 1;
			close(fd);
		} else if (errno != EACCES && errno != EPERM) {
			failed = 1;
		}
		if (allow != s->allow) {
			dprintf(2, "%s: allow=%d expected=%d\n", s->name, allow, s->allow);
			failed = 1;
		}
	} else {
		dprintf(2, "%s: setup errno=%d\n", s->name, error);
	}
	if (write(finish[1], "x", 1) != 1)
		failed = 1;
	close(finish[1]); close(ready[0]);
	if (waitpid(target, &status, 0) != target || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0)
		failed = 1;
	return failed;
}

int
main(int argc, char **argv)
{
	if (argc == 6 && strcmp(argv[1], "--target") == 0)
		return target_ready(atoi(argv[2]), atoi(argv[3]),
		    strtoull(argv[4], NULL, 16), atoi(argv[5]));
	const struct scenario cases[] = {
		{ "same namespace no caps", 0, 0, 0, 0, 0, 1, 1, 0 },
		{ "same namespace less permitted", 0, 0, 0, 0, ALL, 1, 0, 0 },
		{ "filesystem check uses effective", 0, 0, 0, ALL, ALL, 1, 0, 0 },
		{ "missing ptrace effective bit", 0, 0, ALL & ~TRACE, ALL, ALL, 1, 0, 0 },
		{ "effective superset", 0, 0, ALL & ~TRACE, ALL, ALL & ~TRACE, 1, 1, 0 },
		{ "nondumpable no authority", 0, 0, 0, 0, 0, 0, 0, 0 },
		{ "nondumpable scoped ptrace", 0, 0, TRACE, TRACE, ALL, 0, 1, 0 },
		{ "child cannot inspect parent", 1, 0, ALL, ALL, 0, 1, 0, 0 },
		{ "sibling cannot inspect sibling", 1, 1, ALL, ALL, 0, 1, 0, 0 },
		{ "owner cannot expose older memory", 0, 1, 0, 0, ALL, 0, 0, 0 },
		{ "ancestor cannot expose older memory", 0, 2, 0, 0, ALL, 0, 0, 0 },
		{ "owner controls new child image", 0, 1, 0, 0, ALL, 0, 1, 1 },
		{ "owner controls new descendant image", 0, 2, 0, 0, ALL, 0, 1, 1 },
	};
	unsigned int failed = 0;
	if (geteuid() != 0) {
		fprintf(stderr, "Run inside the documented bwrap bootstrap\n");
		return 2;
	}
	for (unsigned int i = 0; i < sizeof (cases) / sizeof (cases[0]); i++) {
		int status;
		pid_t child = fork();
		if (child == 0)
			_exit(run_case(&cases[i]));
		if (child < 0 || waitpid(child, &status, 0) != child ||
		    !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
			fprintf(stderr, "FAIL %s\n", cases[i].name);
			failed++;
		}
	}
	printf("%zu Linux process-access scenarios, %u failed, no skips\n",
	    sizeof (cases) / sizeof (cases[0]), failed);
	return failed != 0;
}
