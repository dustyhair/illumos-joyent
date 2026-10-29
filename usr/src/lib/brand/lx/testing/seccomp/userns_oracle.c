#define _GNU_SOURCE
/* CDDL version 1.0, http://www.illumos.org/license/CDDL. */
#include "common/lx_userns_model.h"
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <linux/capability.h>
#include <sched.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static unsigned int checks, failures;
static uid_t native_uid;
static gid_t native_gid;
static uint64_t initial_effective;
static int initial_groups;
static lx_idmap_t initial_uids, initial_gids;

static void
read_initial_map(const char *path, lx_idmap_t *out)
{
	char buffer[4096];
	int fd = open(path, O_RDONLY);
	ssize_t n = fd < 0 ? -1 : read(fd, buffer, sizeof (buffer));
	if (fd >= 0)
		close(fd);
	if (n <= 0 || lx_idmap_parse(buffer, n, out) != 0)
		abort();
	/* This test boundary uses the bootstrap namespace's visible IDs. */
	for (uint32_t i = 0; i < out->lim_count; i++)
		out->lim_extents[i].lie_outside = out->lim_extents[i].lie_inside;
}

#define CHECK(expr, label) do { \
	checks++; \
	if (!(expr)) { \
		fprintf(stderr, "FAIL %s at %d\n", label, __LINE__); \
		failures++; \
	} \
} while (0)

static int
put_map(int gid, const char *text, size_t size, off_t offset)
{
	int fd = open(gid ? "/proc/self/gid_map" : "/proc/self/uid_map", O_WRONLY);
	int result;

	if (fd < 0)
		return errno;
	if (offset != 0 && lseek(fd, offset, SEEK_SET) != offset) {
		result = errno;
		close(fd);
		return result;
	}
	result = write(fd, text, size) == (ssize_t)size ? 0 : errno;
	close(fd);
	return result;
}

static int
put_groups(const char *text)
{
	int fd = open("/proc/self/setgroups", O_WRONLY);
	int result;

	if (fd < 0)
		return errno;
	result = write(fd, text, strlen(text)) == (ssize_t)strlen(text) ? 0 : errno;
	close(fd);
	return result;
}

static int
real_setup(unsigned int inside)
{
	char text[100];
	int error;

	if (unshare(CLONE_NEWUSER) != 0)
		return errno;
	snprintf(text, sizeof (text), "%u %u 1\n", inside, native_uid);
	if ((error = put_map(0, text, strlen(text), 0)) != 0)
		return error;
	if ((error = put_groups("deny\n")) != 0)
		return error;
	snprintf(text, sizeof (text), "%u %u 1\n", inside, native_gid);
	return put_map(1, text, strlen(text), 0);
}

static lx_caps_t
real_caps(void)
{
	struct __user_cap_header_struct header = { _LINUX_CAPABILITY_VERSION_3, 0 };
	struct __user_cap_data_struct data[2];
	lx_caps_t caps = { 0 };

	if (syscall(SYS_capget, &header, data) != 0)
		_exit(110);
	caps.lc_effective = data[0].effective | (uint64_t)data[1].effective << 32;
	caps.lc_permitted = data[0].permitted | (uint64_t)data[1].permitted << 32;
	caps.lc_inheritable = data[0].inheritable | (uint64_t)data[1].inheritable << 32;
	caps.lc_securebits = prctl(PR_GET_SECUREBITS, 0, 0, 0, 0);
	for (unsigned int i = 0; i <= LX_CAP_LAST; i++) {
		if (prctl(PR_CAPBSET_READ, i, 0, 0, 0) == 1)
			caps.lc_bounding |= LX_CAP_BIT(i);
		if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_IS_SET, i, 0, 0) == 1)
			caps.lc_ambient |= LX_CAP_BIT(i);
	}
	return caps;
}

static int
same_caps(lx_caps_t a, lx_caps_t b)
{
	return a.lc_effective == b.lc_effective && a.lc_permitted == b.lc_permitted &&
	    a.lc_inheritable == b.lc_inheritable && a.lc_bounding == b.lc_bounding &&
	    a.lc_ambient == b.lc_ambient && a.lc_securebits == b.lc_securebits;
}

static int
real_set(uint64_t effective, uint64_t permitted, uint64_t inheritable)
{
	struct __user_cap_header_struct header = { _LINUX_CAPABILITY_VERSION_3, 0 };
	struct __user_cap_data_struct data[2] = {
		{ effective, permitted, inheritable },
		{ effective >> 32, permitted >> 32, inheritable >> 32 }
	};
	return syscall(SYS_capset, &header, data) == 0 ? 0 : errno;
}

static int
result_prctl(int option, unsigned long a, unsigned long b)
{
	return prctl(option, a, b, 0, 0) == 0 ? 0 : errno;
}

static lx_userns_actor_t
model_setup(lx_userns_model_t *root, lx_userns_model_t *child)
{
	lx_userns_actor_t actor = { root, native_uid, native_gid, initial_effective };
	lx_userns_model_root(root);
	root->lum_uids = initial_uids;
	root->lum_gids = initial_gids;
	root->lum_setgroups = initial_groups;
	if (lx_userns_model_child(child, &actor) != 0)
		abort();
	actor.lua_ns = child;
	actor.lua_effective = LX_CAP_ALL;
	return actor;
}

static void
map_case(const char *name, const char *text, size_t size, int gid,
    int groups_deny, int rewrite, off_t offset)
{
	lx_userns_model_t *root = calloc(1, sizeof (*root));
	lx_userns_model_t *child = calloc(1, sizeof (*child));
	lx_idmap_t *map = calloc(1, sizeof (*map));
	lx_userns_actor_t actor = model_setup(root, child);
	int expected, status;
	pid_t pid;

	if (groups_deny)
		CHECK(lx_userns_model_groups(child, &actor, 0) == 0, name);
	if (rewrite) {
		char first[100];
		snprintf(first, sizeof (first), "0 %u 1\n", native_uid);
		CHECK(lx_idmap_parse(first, strlen(first), map) == 0, name);
		CHECK(lx_userns_model_write(child, 0, &actor, &actor, map) == 0, name);
	}
	if (offset != 0 || size >= 4096) {
		expected = EINVAL;
	} else {
		expected = lx_userns_model_write_check(child, gid, &actor);
		if (expected == 0)
			expected = lx_idmap_parse(text, size, map);
		if (expected == 0)
			expected = lx_userns_model_write(child, gid, &actor, &actor, map);
	}
	pid = fork();
	if (pid == 0) {
		int actual;
		char first[100];
		if (unshare(CLONE_NEWUSER) != 0)
			_exit(111);
		if (groups_deny && put_groups("deny\n") != 0)
			_exit(112);
		if (rewrite) {
			snprintf(first, sizeof (first), "0 %u 1\n", native_uid);
			if (put_map(0, first, strlen(first), 0) != 0)
				_exit(113);
		}
		actual = put_map(gid, text, size, offset);
		if (actual != expected) {
			dprintf(2, "%s Linux=%d model=%d\n", name, actual, expected);
			_exit(1);
		}
		_exit(0);
	}
	CHECK(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
	    WEXITSTATUS(status) == 0, name);
	free(root); free(child); free(map);
}

static void
map_cases(void)
{
	char text[4096];
	static const char *formats[] = {
		"0 %u 1\n", "1234 %u 1", " \t0 %u 1\t\r\n", "0 %u 0\n",
		"4294967295 %u 1\n", "4294967294 %u 1\n", "4294967294 %u 2\n",
		"4294967296 %u 1\n", "0 %u 4294967297\n", "-1 %u 1\n",
		"+0 %u 1\n", "0x0 %u 1\n", "0 %u 1 junk\n", "0 %u\n",
		"0 %u 1\n\n", "0 %u 1\n \n", "0 %u 1\v\f\n"
	};
	for (size_t i = 0; i < sizeof (formats) / sizeof (formats[0]); i++) {
		char name[40];
		snprintf(name, sizeof (name), "uid format %zu", i);
		snprintf(text, sizeof (text), formats[i], native_uid);
		map_case(name, text, strlen(text), 0, 0, 0, 0);
	}
	snprintf(text, sizeof (text), "0 %u 1\n", native_uid + 1);
	map_case("foreign parent uid", text, strlen(text), 0, 0, 0, 0);
	snprintf(text, sizeof (text), "0 %llu 1\n", (unsigned long long)native_uid +
	    UINT64_C(4294967296));
	map_case("wrapped parent decimal", text, strlen(text), 0, 0, 0, 0);
	snprintf(text, sizeof (text), "0 %u 1\n", native_uid);
	size_t len = strlen(text);
	text[len++] = '\0';
	memcpy(text + len, "junk", 4);
	map_case("embedded NUL", text, len + 4, 0, 0, 0, 0);
	map_case("empty map", "", 0, 0, 0, 0, 0);
	memset(text, ' ', sizeof (text));
	map_case("page-sized write", text, sizeof (text), 0, 0, 0, 0);
	map_case("second malformed write", "bad", 3, 0, 0, 1, 0);
	snprintf(text, sizeof (text), "0 %u 1\n", native_uid);
	map_case("nonzero offset", text, strlen(text), 0, 0, 0, 1);
	snprintf(text, sizeof (text), "0 %u 1\n0 %u 1\n", native_uid, native_uid + 1);
	map_case("overlapping inside", text, strlen(text), 0, 0, 0, 0);
	snprintf(text, sizeof (text), "0 %u 1\n1 %u 1\n", native_uid, native_uid);
	map_case("overlapping outside", text, strlen(text), 0, 0, 0, 0);
	snprintf(text, sizeof (text), "0 %u 1\n1 %u 1\n", native_uid, native_uid + 1);
	map_case("unprivileged multiple ranges", text, strlen(text), 0, 0, 0, 0);
	size_t used = 0;
	for (unsigned int i = 0; i < 341; i++) {
		used += snprintf(text + used, sizeof (text) - used,
		    "%u %u 1\n", i, i);
		if (i == 339)
			map_case("340 extents", text, used, 0, 0, 0, 0);
	}
	map_case("341 extents", text, used, 0, 0, 0, 0);
	snprintf(text, sizeof (text), "0 %u 1\n", native_gid);
	map_case("gid without deny", text, strlen(text), 1, 0, 0, 0);
	map_case("gid after deny", text, strlen(text), 1, 1, 0, 0);
	snprintf(text, sizeof (text), "0 %u 1\n", native_gid + 1);
	map_case("foreign parent gid", text, strlen(text), 1, 1, 0, 0);
}

enum op { DROP, SET, SECURE, KEEP, AMBIENT };
struct action { enum op op; uint64_t a, b, c; };

static int
cap_action(lx_caps_t *model, const struct action *action)
{
	int expected, actual;
	switch (action->op) {
	case DROP:
		expected = lx_caps_drop(model, action->a);
		actual = result_prctl(PR_CAPBSET_DROP, action->a, 0);
		break;
	case SET:
		expected = lx_caps_set(model, action->a, action->b, action->c);
		actual = real_set(action->a, action->b, action->c);
		break;
	case SECURE:
		expected = lx_caps_securebits(model, action->a);
		actual = result_prctl(PR_SET_SECUREBITS, action->a, 0);
		break;
	case KEEP:
		expected = lx_caps_keep(model, action->a);
		actual = result_prctl(PR_SET_KEEPCAPS, action->a, 0);
		break;
	case AMBIENT:
		expected = lx_caps_ambient(model, action->a, action->b);
		if (action->a == 1) {
			actual = prctl(PR_CAP_AMBIENT, action->a, action->b, 0, 0);
			if (actual < 0)
				actual = errno;
		} else {
			actual = result_prctl(PR_CAP_AMBIENT, action->a, action->b);
		}
		break;
	default:
		abort();
	}
	if (actual != expected || !same_caps(*model, real_caps())) {
		dprintf(2, "caps op%d a=%llx actual=%d expected=%d state-match=%d\n",
		    action->op, (unsigned long long)action->a, actual, expected,
		    same_caps(*model, real_caps()));
		return 1;
	}
	return 0;
}

static void
cap_case(const char *name, const struct action *actions, size_t count)
{
	pid_t pid = fork();
	int status;

	if (pid == 0) {
		lx_caps_t model;
		if (real_setup(0) != 0)
			_exit(111);
		lx_caps_new_namespace(&model);
		if (!same_caps(model, real_caps()))
			_exit(112);
		for (size_t i = 0; i < count; i++) {
			if (cap_action(&model, &actions[i]) != 0)
				_exit(1);
		}
		_exit(0);
	}
	CHECK(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
	    WEXITSTATUS(status) == 0, name);
}

#define COUNT(a) (sizeof (a) / sizeof ((a)[0]))
static void
cap_cases(void)
{
	const struct action drops[] = {
		{ DROP, 39, 0, 0 }, { DROP, 39, 0, 0 }, { DROP, 64, 0, 0 },
		{ DROP, 8, 0, 0 }, { DROP, 1, 0, 0 },
		{ SET, LX_CAP_ALL & ~LX_CAP_BIT(8), LX_CAP_ALL, 0 },
		{ DROP, 64, 0, 0 }, { DROP, 2, 0, 0 }
	};
	const struct action inheritable[] = {
		{ DROP, 2, 0, 0 }, { SET, LX_CAP_ALL, LX_CAP_ALL, 4 },
		{ SET, LX_CAP_ALL, LX_CAP_ALL, 1 },
		{ SET, LX_CAP_ALL & ~2ULL, LX_CAP_ALL & ~2ULL, 1 },
		{ SET, LX_CAP_ALL, LX_CAP_ALL, 1 },
		{ SET, LX_CAP_ALL & ~2ULL, LX_CAP_ALL & ~2ULL, 3 },
		{ SET, LX_CAP_ALL & ~(2ULL | LX_CAP_BIT(8)),
		    LX_CAP_ALL & ~2ULL, 3 },
		{ SET, LX_CAP_ALL & ~(2ULL | LX_CAP_BIT(8)),
		    LX_CAP_ALL & ~2ULL, 7 },
		{ SET, UINT64_MAX, UINT64_MAX, UINT64_MAX }
	};
	const struct action locks[] = {
		{ SECURE, 3, 0, 0 }, { SECURE, 0, 0, 0 }, { SECURE, 2, 0, 0 },
		{ KEEP, 1, 0, 0 }, { KEEP, 2, 0, 0 }, { SECURE, 0x33, 0, 0 },
		{ KEEP, 0, 0, 0 }, { SECURE, 0x33, 0, 0 },
		{ SET, 0, 0, 0 }, { SECURE, 0x33, 0, 0 }, { SECURE, 0x80, 0, 0 }
	};
	const struct action ambient[] = {
		{ AMBIENT, 2, 0, 0 }, { SET, LX_CAP_ALL, LX_CAP_ALL, 3 },
		{ AMBIENT, 2, 0, 0 }, { AMBIENT, 1, 0, 0 }, { DROP, 0, 0, 0 },
		{ AMBIENT, 3, 0, 0 }, { AMBIENT, 2, 0, 0 },
		{ SET, LX_CAP_ALL, LX_CAP_ALL, 2 }, { AMBIENT, 1, 0, 0 },
		{ AMBIENT, 2, 1, 0 }, { SECURE, 0xc0, 0, 0 },
		{ AMBIENT, 2, 1, 0 }, { AMBIENT, 4, 0, 0 }, { AMBIENT, 4, 1, 0 },
		{ AMBIENT, 2, 41, 0 }
	};
	cap_case("bounding authority", drops, COUNT(drops));
	cap_case("permitted/inheritable limits", inheritable, COUNT(inheritable));
	cap_case("securebits locks", locks, COUNT(locks));
	cap_case("ambient transitions", ambient, COUNT(ambient));
}

static void
exec_case(const char *name, unsigned int inside, int drop_all, int nnp,
    int noroot, int ambient)
{
	int pipefd[2], status;
	pid_t pid;
	lx_caps_t model, actual;
	CHECK(pipe(pipefd) == 0, "exec pipe");
	lx_caps_new_namespace(&model);
	if (ambient) {
		CHECK(lx_caps_set(&model, LX_CAP_ALL, LX_CAP_ALL, 1) == 0, name);
		CHECK(lx_caps_ambient(&model, 2, 0) == 0, name);
	}
	if (drop_all) {
		for (unsigned int i = 0; i <= LX_CAP_LAST; i++)
			CHECK(lx_caps_drop(&model, i) == 0, name);
		CHECK(lx_caps_set(&model, 0, 0, 0) == 0, name);
	} else if (nnp) {
		CHECK(lx_caps_set(&model, LX_CAP_ALL & ~1ULL,
		    LX_CAP_ALL & ~1ULL, 0) == 0, name);
	}
	if (noroot)
		CHECK(lx_caps_securebits(&model, 3) == 0, name);
	CHECK(lx_caps_keep(&model, 1) == 0, name);
	lx_caps_exec_plain(&model, inside == 0, inside == 0, nnp);
	pid = fork();
	if (pid == 0) {
		char fd[20];
		close(pipefd[0]);
		if (real_setup(inside) != 0)
			_exit(111);
		if (ambient && (real_set(LX_CAP_ALL, LX_CAP_ALL, 1) != 0 ||
		    result_prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, 0) != 0))
			_exit(112);
		if (drop_all) {
			for (unsigned int i = 0; i <= LX_CAP_LAST; i++) {
				if (result_prctl(PR_CAPBSET_DROP, i, 0) != 0)
					_exit(113);
			}
			if (real_set(0, 0, 0) != 0)
				_exit(114);
		} else if (nnp) {
			if (real_set(LX_CAP_ALL & ~1ULL, LX_CAP_ALL & ~1ULL, 0) != 0)
				_exit(115);
		}
		if (noroot && result_prctl(PR_SET_SECUREBITS, 3, 0) != 0)
			_exit(116);
		if (result_prctl(PR_SET_KEEPCAPS, 1, 0) != 0)
			_exit(117);
		if (nnp && result_prctl(PR_SET_NO_NEW_PRIVS, 1, 0) != 0)
			_exit(118);
		snprintf(fd, sizeof (fd), "%d", pipefd[1]);
		execl("/proc/self/exe", "userns_oracle", "--report", fd, NULL);
		_exit(119);
	}
	close(pipefd[1]);
	ssize_t n = read(pipefd[0], &actual, sizeof (actual));
	close(pipefd[0]);
	CHECK(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
	    WEXITSTATUS(status) == 0 && n == sizeof (actual) &&
	    same_caps(model, actual), name);
}

static void
nested_case(void)
{
	pid_t pid = fork();
	int status;
	if (pid == 0) {
		lx_userns_model_t *root = calloc(1, sizeof (*root));
		lx_userns_model_t *first = calloc(1, sizeof (*first));
		lx_userns_model_t *second = calloc(1, sizeof (*second));
		lx_userns_actor_t actor = model_setup(root, first);
		lx_idmap_t *map = calloc(1, sizeof (*map));
		char text[100];
		if (real_setup(0) != 0)
			_exit(111);
		snprintf(text, sizeof (text), "0 %u 1\n", native_uid);
		if (lx_idmap_parse(text, strlen(text), map) != 0 ||
		    lx_userns_model_write(first, 0, &actor, &actor, map) != 0 ||
		    lx_userns_model_groups(first, &actor, 0) != 0)
			_exit(112);
		snprintf(text, sizeof (text), "0 %u 1\n", native_gid);
		if (lx_idmap_parse(text, strlen(text), map) != 0 ||
		    lx_userns_model_write(first, 1, &actor, &actor, map) != 0 ||
		    lx_userns_model_child(second, &actor) != 0 ||
		    unshare(CLONE_NEWUSER) != 0)
			_exit(113);
		actor.lua_ns = second;
		if (put_groups("allow\n") != EPERM ||
		    lx_userns_model_groups(second, &actor, 1) != EPERM)
			_exit(114);
		snprintf(text, sizeof (text), "%u 0 1\n", native_uid);
		if (put_map(0, text, strlen(text), 0) != 0 ||
		    lx_idmap_parse(text, strlen(text), map) != 0 ||
		    lx_userns_model_write(second, 0, &actor, &actor, map) != 0)
			_exit(115);
		snprintf(text, sizeof (text), "%u 0 1\n", native_gid);
		if (put_map(1, text, strlen(text), 0) != 0 ||
		    lx_idmap_parse(text, strlen(text), map) != 0 ||
		    lx_userns_model_write(second, 1, &actor, &actor, map) != 0)
			_exit(116);
		if (geteuid() != lx_idmap_up(&second->lum_uids, native_uid) ||
		    getegid() != lx_idmap_up(&second->lum_gids, native_gid) ||
		    lx_idmap_down(&second->lum_uids, native_uid) != native_uid)
			_exit(117);
		_exit(0);
	}
	CHECK(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
	    WEXITSTATUS(status) == 0, "nested bubblewrap identity");
}

int
main(int argc, char **argv)
{
	if (argc == 3 && strcmp(argv[1], "--report") == 0) {
		lx_caps_t caps = real_caps();
		return write(atoi(argv[2]), &caps, sizeof (caps)) == sizeof (caps) ? 0 : 1;
	}
	native_uid = geteuid(); native_gid = getegid();
	initial_effective = real_caps().lc_effective;
	read_initial_map("/proc/self/uid_map", &initial_uids);
	read_initial_map("/proc/self/gid_map", &initial_gids);
	char groups[10] = { 0 };
	int groupfd = open("/proc/self/setgroups", O_RDONLY);
	if (groupfd < 0 || read(groupfd, groups, sizeof (groups) - 1) <= 0)
		abort();
	close(groupfd);
	initial_groups = strncmp(groups, "allow", 5) == 0;
	map_cases();
	cap_cases();
	exec_case("exec root bounding drop", 0, 1, 0, 0, 0);
	exec_case("exec root nnp", 0, 0, 1, 0, 0);
	exec_case("exec root noroot", 0, 0, 0, 1, 0);
	exec_case("exec nonroot", 1234, 0, 0, 0, 0);
	exec_case("exec ambient", 1234, 0, 0, 0, 1);
	nested_case();
	printf("%u user namespace/capability control assertions, %u failed, no skips\n",
	    checks, failures);
	return failures != 0;
}
