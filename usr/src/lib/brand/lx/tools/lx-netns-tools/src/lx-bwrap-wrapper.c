/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2026 Jonathan Wagner.
 */

/*
 * Minimal Linux x86-64 exec wrapper.  It is intentionally libc-free so the
 * compatibility layer adds no runtime dependency inside the lx zone.
 */

typedef unsigned long lx_size_t;

typedef struct lx_option {
	const char *lo_name;
	lx_size_t lo_operands;
} lx_option_t;

#define	LX_SYS_WRITE	1
#define	LX_SYS_EXECVE	59
#define	LX_SYS_EXIT	60
#define	LX_MAX_ARGV	4096

static char *lx_argv[LX_MAX_ARGV];

/*
 * Operand counts from bubblewrap 0.11.2 parse_args_recurse().  Unknown
 * options are rejected instead of guessing where the command begins.
 */
static const lx_option_t lx_options[] = {
	{ "--help", 0 },
	{ "--version", 0 },
	{ "--level-prefix", 0 },
	{ "--unshare-all", 0 },
	{ "--unshare-user", 0 },
	{ "--unshare-user-try", 0 },
	{ "--unshare-ipc", 0 },
	{ "--unshare-pid", 0 },
	{ "--unshare-net", 0 },
	{ "--unshare-uts", 0 },
	{ "--unshare-cgroup", 0 },
	{ "--unshare-cgroup-try", 0 },
	{ "--disable-userns", 0 },
	{ "--assert-userns-disabled", 0 },
	{ "--clearenv", 0 },
	{ "--new-session", 0 },
	{ "--die-with-parent", 0 },
	{ "--as-pid-1", 0 },
	{ "--argv0", 1 },
	{ "--chdir", 1 },
	{ "--remount-ro", 1 },
	{ "--overlay-src", 1 },
	{ "--tmp-overlay", 1 },
	{ "--ro-overlay", 1 },
	{ "--proc", 1 },
	{ "--exec-label", 1 },
	{ "--file-label", 1 },
	{ "--dev", 1 },
	{ "--tmpfs", 1 },
	{ "--mqueue", 1 },
	{ "--dir", 1 },
	{ "--lock-file", 1 },
	{ "--sync-fd", 1 },
	{ "--block-fd", 1 },
	{ "--userns-block-fd", 1 },
	{ "--info-fd", 1 },
	{ "--json-status-fd", 1 },
	{ "--seccomp", 1 },
	{ "--add-seccomp-fd", 1 },
	{ "--userns", 1 },
	{ "--userns2", 1 },
	{ "--pidns", 1 },
	{ "--unsetenv", 1 },
	{ "--uid", 1 },
	{ "--gid", 1 },
	{ "--hostname", 1 },
	{ "--cap-add", 1 },
	{ "--cap-drop", 1 },
	{ "--perms", 1 },
	{ "--size", 1 },
	{ "--bind", 2 },
	{ "--bind-try", 2 },
	{ "--ro-bind", 2 },
	{ "--ro-bind-try", 2 },
	{ "--dev-bind", 2 },
	{ "--dev-bind-try", 2 },
	{ "--bind-fd", 2 },
	{ "--ro-bind-fd", 2 },
	{ "--file", 2 },
	{ "--bind-data", 2 },
	{ "--ro-bind-data", 2 },
	{ "--symlink", 2 },
	{ "--setenv", 2 },
	{ "--chmod", 2 },
	{ "--overlay", 3 }
};

static long
lx_syscall1(long number, long first)
{
	long result;

	__asm__ volatile("syscall"
	    : "=a" (result)
	    : "a" (number), "D" (first)
	    : "rcx", "r11", "memory");
	return (result);
}

static long
lx_syscall3(long number, long first, long second, long third)
{
	long result;

	__asm__ volatile("syscall"
	    : "=a" (result)
	    : "a" (number), "D" (first), "S" (second), "d" (third)
	    : "rcx", "r11", "memory");
	return (result);
}

static lx_size_t
string_length(const char *value)
{
	lx_size_t length = 0;

	while (value[length] != '\0')
		length++;
	return (length);
}

static int
string_equal(const char *left, const char *right)
{
	while (*left != '\0' && *left == *right) {
		left++;
		right++;
	}
	return (*left == *right);
}

static int
string_prefix(const char *value, const char *prefix)
{
	while (*prefix != '\0') {
		if (*value++ != *prefix++)
			return (0);
	}
	return (1);
}

static long
option_operands(const char *name)
{
	lx_size_t i;

	for (i = 0; i < sizeof (lx_options) / sizeof (lx_options[0]); i++) {
		if (string_equal(name, lx_options[i].lo_name))
			return ((long)lx_options[i].lo_operands);
	}
	return (-1);
}

static void fail(const char *) __attribute__((noreturn));

static void
fail(const char *message)
{
	static const char prefix[] = "lx-bwrap: ";
	static const char newline[] = "\n";

	(void) lx_syscall3(LX_SYS_WRITE, 2, (long)prefix,
	    sizeof (prefix) - 1);
	(void) lx_syscall3(LX_SYS_WRITE, 2, (long)message,
	    string_length(message));
	(void) lx_syscall3(LX_SYS_WRITE, 2, (long)newline, 1);
	(void) lx_syscall1(LX_SYS_EXIT, 125);
	__builtin_unreachable();
}

void start_c(unsigned long *) __attribute__((noreturn, used));

void
start_c(unsigned long *stack)
{
	static const char helper_name[] = "LX_NETNS_HELPER=";
	static const char links_name[] = "LX_PRIVATE_NETWORK_LINKS=";
	static const char gateway_name[] = "LX_PRIVATE_NETWORK_GATEWAY=";
	static const char real_name[] = "LX_NETNS_REAL_BWRAP=";
	static const char net[] = "--unshare-net";
	static const char cap_add[] = "--cap-add";
	static const char cap_net_admin[] = "CAP_NET_ADMIN";
	static const char cap_setpcap[] = "CAP_SETPCAP";
	static const char argv0_option[] = "--argv0";
	static const char separator[] = "--";
	static const char payload_argv0_option[] = "--payload-argv0";
	static const char links_option[] = "--links";
	static const char gateway_option[] = "--gateway";
	char **argv = (char **)(stack + 1);
	char **envp = argv + stack[0] + 1;
	const char *real = (void *)0;
	const char *helper = (void *)0;
	const char *links = (void *)0;
	const char *gateway = (void *)0;
	const char *payload_argv0 = (void *)0;
	lx_size_t argc = stack[0];
	lx_size_t out = 0;
	lx_size_t i, j;
	int private_net = 0;
	int found_separator = 0;

	for (i = 0; envp[i] != (void *)0; i++) {
		if (string_prefix(envp[i], real_name))
			real = envp[i] + sizeof (real_name) - 1;
		if (string_prefix(envp[i], helper_name))
			helper = envp[i] + sizeof (helper_name) - 1;
		if (string_prefix(envp[i], links_name))
			links = envp[i] + sizeof (links_name) - 1;
		if (string_prefix(envp[i], gateway_name))
			gateway = envp[i] + sizeof (gateway_name) - 1;
	}
	if (real == (void *)0 || real[0] != '/')
		fail("LX_NETNS_REAL_BWRAP must be an absolute path");
	if (helper == (void *)0 || helper[0] != '/')
		fail("LX_NETNS_HELPER must be an absolute path");
	if (links == (void *)0 || links[0] == '\0')
		fail("LX_PRIVATE_NETWORK_LINKS is empty");
	if (gateway == (void *)0 || gateway[0] == '\0')
		fail("LX_PRIVATE_NETWORK_GATEWAY is empty");
	if (argc + 13 >= LX_MAX_ARGV)
		fail("bubblewrap argument list is too long");

	lx_argv[out++] = (char *)real;
	for (i = 1; i < argc; ) {
		long operands;

		if (string_equal(argv[i], separator)) {
			found_separator = 1;
			if (++i >= argc)
				fail("bubblewrap command is missing");
			if (payload_argv0 == (void *)0)
				payload_argv0 = argv[i];
			if (!private_net)
				lx_argv[out++] = (char *)net;
			lx_argv[out++] = (char *)cap_add;
			lx_argv[out++] = (char *)cap_net_admin;
			lx_argv[out++] = (char *)cap_add;
			lx_argv[out++] = (char *)cap_setpcap;
			lx_argv[out++] = (char *)separator;
			lx_argv[out++] = (char *)helper;
			lx_argv[out++] = (char *)payload_argv0_option;
			lx_argv[out++] = (char *)payload_argv0;
			lx_argv[out++] = (char *)links_option;
			lx_argv[out++] = (char *)links;
			lx_argv[out++] = (char *)gateway_option;
			lx_argv[out++] = (char *)gateway;
			lx_argv[out++] = (char *)separator;
			for (; i < argc; i++)
				lx_argv[out++] = argv[i];
			break;
		}
		if (string_equal(argv[i], "--args"))
			fail("--args is not supported by lx-bwrap");
		if (string_equal(argv[i], "--share-net"))
			fail("--share-net conflicts with lx-bwrap");
		operands = option_operands(argv[i]);
		if (operands < 0)
			fail("unsupported bubblewrap option before command");
		if (i + (lx_size_t)operands >= argc)
			fail("bubblewrap option is missing an operand");
		if (string_equal(argv[i], argv0_option)) {
			if (payload_argv0 != (void *)0)
				fail("expected exactly one --argv0");
			payload_argv0 = argv[i + 1];
		}
		if (string_equal(argv[i], net) ||
		    string_equal(argv[i], "--unshare-all"))
			private_net = 1;
		for (j = 0; j <= (lx_size_t)operands; j++)
			lx_argv[out++] = argv[i + j];
		i += (lx_size_t)operands + 1;
	}
	lx_argv[out] = (void *)0;
	if (!found_separator)
		fail("expected an explicit bubblewrap command separator");

	(void) lx_syscall3(LX_SYS_EXECVE, (long)real, (long)lx_argv,
	    (long)envp);
	fail("cannot execute bwrap.real");
}

__asm__(
	".global _start\n"
	"_start:\n"
	"mov %rsp, %rdi\n"
	"andq $-16, %rsp\n"
	"call start_c\n"
	"ud2\n");
