/*
 * CDDL version 1.0, http://www.illumos.org/license/CDDL.
 * Compile for Linux/lx with -pthread. See README.md for root-only controls.
 */
#define	_GNU_SOURCE
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define	SKIP 77
#define	UNPRIV 65534

static char self[PATH_MAX];
static char fixture[128];
static int inherited;
static int native_available;
static const char *native_priv_helper;

static int
get_nnp(void)
{
	return (prctl(PR_GET_NO_NEW_PRIVS, 0UL, 0UL, 0UL, 0UL));
}

static int
set_nnp(void)
{
	return (prctl(PR_SET_NO_NEW_PRIVS, 1UL, 0UL, 0UL, 0UL));
}

static int
proc_nnp(void)
{
	char path[128], line[256];
	FILE *fp;
	int result = -1;

	(void) snprintf(path, sizeof (path), "/proc/self/task/%ld/status",
	    syscall(SYS_gettid));
	if ((fp = fopen(path, "r")) == NULL)
		return (-1);
	while (fgets(line, sizeof (line), fp) != NULL) {
		if (sscanf(line, "NoNewPrivs: %d", &result) == 1)
			break;
	}
	(void) fclose(fp);
	return (result);
}

static int
proc_status(void)
{
	if (proc_nnp() != inherited || set_nnp() != 0)
		return (1);
	return (proc_nnp() != 1);
}

static int
wait_child(pid_t pid)
{
	int status;

	if (pid == -1 || waitpid(pid, &status, 0) != pid)
		return (1);
	return (WIFEXITED(status) ? WEXITSTATUS(status) : 1);
}

static int
initial(void)
{
	int mode = get_nnp();

	/* This baseline starts from a fresh, unrestricted lx login. */
	if (access("/native/usr/bin/uname", X_OK) == 0 && mode != 0)
		return (1);
	return (mode != 0 && mode != 1);
}

static int
arguments(void)
{
	unsigned long args[4];
	unsigned int i;
	int opt;

	for (opt = PR_SET_NO_NEW_PRIVS; opt <= PR_GET_NO_NEW_PRIVS; opt++) {
		for (i = 0; i < 4; i++) {
			(void) memset(args, 0, sizeof (args));
			if (opt == PR_SET_NO_NEW_PRIVS) {
				args[0] = 1;
				args[i] = i == 0 ? 0 : 1;
			} else {
				args[i] = 1;
			}
			errno = 0;
			if (prctl(opt, args[0], args[1], args[2], args[3]) !=
			    -1 ||
			    errno != EINVAL)
				return (1);
		}
	}
	errno = 0;
	if (prctl(PR_SET_NO_NEW_PRIVS, 2UL, 0UL, 0UL, 0UL) != -1 ||
	    errno != EINVAL)
		return (1);
	return (get_nnp() != inherited);
}

static int
irreversible(void)
{
	if (set_nnp() != 0 || get_nnp() != 1 || set_nnp() != 0)
		return (1);
	errno = 0;
	return (prctl(PR_SET_NO_NEW_PRIVS, 0UL, 0UL, 0UL, 0UL) != -1 ||
	    errno != EINVAL || get_nnp() != 1);
}

static void *
thread_get(void *arg)
{
	*(int *)arg = get_nnp();
	return (NULL);
}

static void *
thread_set(void *arg)
{
	pthread_t thread;
	int child = -1;
	int *result = arg;

	*result = 1;
	if (set_nnp() != 0 || get_nnp() != 1 || proc_nnp() != 1)
		return (NULL);
	if (pthread_create(&thread, NULL, thread_get, &child) != 0)
		return (NULL);
	if (pthread_join(thread, NULL) != 0)
		return (NULL);
	*result = (child != 1);
	return (NULL);
}

static int
threads(void)
{
	pthread_t thread;
	int result = 1;

	if (pthread_create(&thread, NULL, thread_set, &result) != 0)
		return (1);
	if (pthread_join(thread, NULL) != 0)
		return (1);
	return (result != 0 || get_nnp() != inherited ||
	    proc_nnp() != inherited);
}

static int
fork_exec(void)
{
	pid_t pid;

	if (set_nnp() != 0)
		return (1);
	pid = fork();
	if (pid == 0) {
		if (get_nnp() != 1)
			_exit(1);
		execl(self, self, "--expect-nnp", (char *)NULL);
		_exit(1);
	}
	return (wait_child(pid));
}

static int
vfork_exec(void)
{
	pid_t pid;

	if (set_nnp() != 0)
		return (1);
	pid = vfork();
	if (pid == 0) {
		execl(self, self, "--expect-nnp", (char *)NULL);
		_exit(1);
	}
	return (wait_child(pid));
}

static int
failed_exec(void)
{
	if (set_nnp() != 0)
		return (1);
	execl("/this-path-does-not-exist-lx-nnp", "missing", (char *)NULL);
	return (errno != ENOENT || get_nnp() != 1);
}

static int
native_bridge(void)
{
	if (access("/native/usr/bin/sh", X_OK) != 0)
		return (SKIP);
	if (set_nnp() != 0)
		return (1);
	execl("/native/usr/bin/sh", "sh", "-c",
	    "exec \"$1\" --expect-nnp", "sh", self, (char *)NULL);
	return (1);
}

static int
identity(int native, int nnp)
{
	static const char *names[] = { "suid", "sgid", "both" };
	unsigned int i;
	int failed = 0;

	if (fixture[0] == '\0' || (native && !native_available))
		return (SKIP);
	for (i = 0; i < 3; i++) {
		char path[PATH_MAX];
		pid_t pid;
		const char *uid = !nnp && i != 1 ? "0" : "65534";
		const char *gid = !nnp && i != 0 ? "0" : "65534";

		(void) snprintf(path, sizeof (path), "%s/%s-%s", fixture,
		    native ? "native" : "linux", names[i]);
		pid = fork();
		if (pid == 0) {
			if (setgroups(0, NULL) != 0 || setgid(UNPRIV) != 0 ||
			    setuid(UNPRIV) != 0 || (nnp && set_nnp() != 0))
				_exit(1);
			if (native == 2) {
				execl("/native/usr/bin/sh", "sh", "-c",
				    "exec \"$1\" \"$2\" \"$3\"", "sh", path,
				    uid, gid, (char *)NULL);
			} else {
				execl(path, path, uid, gid, (char *)NULL);
			}
			perror("identity exec");
			_exit(1);
		}
		if (wait_child(pid) != 0) {
			fprintf(stderr, "%s identity check failed\n", names[i]);
			failed = 1;
		}
	}
	return (failed);
}

static int linux_control(void) { return (identity(0, 0)); }
static int linux_nnp(void) { return (identity(0, 1)); }
static int native_control(void) { return (identity(1, 0)); }
static int native_nnp(void) { return (identity(1, 1)); }
static int bridge_control(void) { return (identity(2, 0)); }
static int bridge_nnp(void) { return (identity(2, 1)); }

static int
native_privilege(int nnp)
{
	if (native_priv_helper == NULL || fixture[0] == '\0')
		return (SKIP);
	if (nnp && set_nnp() != 0)
		return (1);
	execl(native_priv_helper, native_priv_helper, "prepare",
	    nnp ? "0" : "1", (char *)NULL);
	return (1);
}

static int privilege_control(void) { return (native_privilege(0)); }
static int privilege_nnp(void) { return (native_privilege(1)); }

static int
copy_helper(const char *source, const char *prefix)
{
	static const char *names[] = { "suid", "sgid", "both" };
	static const mode_t modes[] = { 04755, 02755, 06755 };
	unsigned int i;

	for (i = 0; i < 3; i++) {
		char path[PATH_MAX], buf[8192];
		int in, out, error = 0;
		ssize_t len;

		(void) snprintf(path, sizeof (path), "%s/%s-%s", fixture,
		    prefix, names[i]);
		in = open(source, O_RDONLY);
		if (in == -1)
			return (1);
		out = open(path, O_WRONLY | O_CREAT | O_EXCL, 0700);
		if (out == -1) {
			(void) close(in);
			return (1);
		}
		while ((len = read(in, buf, sizeof (buf))) > 0) {
			ssize_t off = 0;
			while (off < len) {
				ssize_t n = write(out, buf + off, len - off);
				if (n <= 0) {
				error = 1;
					break;
				}
				off += n;
			}
			if (error)
				break;
		}
		if (len < 0 || fchown(out, 0, 0) != 0 ||
		    fchmod(out, modes[i]) != 0)
			error = 1;
		(void) close(in);
		if (close(out) != 0 || error)
			return (1);
	}
	return (0);
}

static void
cleanup(void)
{
	static const char *names[] = { "suid", "sgid", "both" };
	unsigned int i, j;
	char path[PATH_MAX];

	if (fixture[0] == '\0')
		return;
	for (i = 0; i < 2; i++) {
		for (j = 0; j < 3; j++) {
			(void) snprintf(path, sizeof (path), "%s/%s-%s",
			    fixture,
			    i == 0 ? "linux" : "native", names[j]);
			(void) unlink(path);
		}
	}
	(void) rmdir(fixture);
}

int
main(int argc, char **argv)
{
	struct {
		const char *name;
		int (*run)(void);
	} tests[] = {
		{ "initial state", initial },
		{ "argument validation", arguments },
		{ "proc task status reflects flag", proc_status },
		{ "irreversible and repeatable set", irreversible },
		{ "thread-local state and thread inheritance", threads },
		{ "fork and exec inheritance", fork_exec },
		{ "vfork and exec inheritance", vfork_exec },
		{ "failed exec retains restriction", failed_exec },
		{ "native shell bridge retains restriction", native_bridge },
		{ "Linux set-id controls", linux_control },
		{ "Linux set-id suppression", linux_nnp },
		{ "native set-id controls", native_control },
		{ "native set-id suppression", native_nnp },
		{ "native shell set-id controls", bridge_control },
		{ "native shell set-id suppression", bridge_nnp },
		{ "native inheritable privilege control", privilege_control },
		{ "native inheritable privilege cap", privilege_nnp }
	};
	unsigned int i;
	int failed = 0, skipped = 0;

	if (argc == 2 && strcmp(argv[1], "--expect-nnp") == 0)
		return (get_nnp() != 1);
	if (argc < 2 || argc > 4) {
		fprintf(stderr, "usage: %s linux-identity-helper "
		    "[native-identity-helper [native-privilege-helper]]\n",
		    argv[0]);
		return (2);
	}
	if (realpath(argv[0], self) == NULL)
		return (1);
	inherited = get_nnp();
	if (getuid() == 0 && geteuid() == 0 && (inherited == 0 ||
	    access("/native/usr/bin/uname", X_OK) == 0)) {
		(void) snprintf(fixture, sizeof (fixture),
		    "/var/tmp/lx-nnp-XXXXXX");
		if (mkdtemp(fixture) == NULL) {
			fixture[0] = '\0';
			return (1);
		}
		(void) atexit(cleanup);
		if (chmod(fixture, 0755) != 0 ||
		    copy_helper(argv[1], "linux") != 0)
			return (1);
		if (argc >= 3) {
			if (copy_helper(argv[2], "native") != 0)
				return (1);
			native_available = 1;
		}
		if (argc == 4)
			native_priv_helper = argv[3];
	}
	(void) setvbuf(stdout, NULL, _IONBF, 0);
	for (i = 0; i < sizeof (tests) / sizeof (tests[0]); i++) {
		int rc;
		pid_t pid = fork();
		if (pid == 0)
			_exit(tests[i].run());
		rc = wait_child(pid);
		printf("%s: %s\n", tests[i].name,
		    rc == 0 ? "PASS" : rc == SKIP ? "SKIP" : "FAIL");
		failed += (rc != 0 && rc != SKIP);
		skipped += (rc == SKIP);
	}
	printf("%d failed, %d skipped\n", failed, skipped);
	return (failed != 0);
}
