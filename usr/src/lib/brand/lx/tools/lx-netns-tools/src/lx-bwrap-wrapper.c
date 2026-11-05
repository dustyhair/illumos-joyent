/*
 * Minimal Linux x86-64 exec wrapper.  It is intentionally libc-free so the
 * compatibility layer adds no runtime dependency inside the lx zone.
 */

typedef unsigned long lx_size_t;

#define	LX_SYS_WRITE	1
#define	LX_SYS_EXECVE	59
#define	LX_SYS_EXIT	60
#define	LX_MAX_ARGV	4096

static char *lx_argv[LX_MAX_ARGV];

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
	lx_size_t found_net = 0;
	lx_size_t found_separator = 0;
	lx_size_t out = 0;
	lx_size_t i;
	int command = 0;

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
	for (i = 1; i < argc; i++) {
		if (!command && string_equal(argv[i], argv0_option)) {
			if (i + 1 >= argc)
				fail("--argv0 is missing its value");
			if (payload_argv0 != (void *)0)
				fail("expected exactly one --argv0");
			payload_argv0 = argv[i + 1];
		}
		if (!command && string_equal(argv[i], separator)) {
			found_separator++;
			if (i + 1 >= argc)
				fail("bubblewrap command is missing");
			if (payload_argv0 == (void *)0)
				payload_argv0 = argv[i + 1];
			if (found_net == 0)
				lx_argv[out++] = (char *)net;
			lx_argv[out++] = (char *)cap_add;
			lx_argv[out++] = (char *)cap_net_admin;
			lx_argv[out++] = (char *)cap_add;
			lx_argv[out++] = (char *)cap_setpcap;
			lx_argv[out++] = argv[i];
			lx_argv[out++] = (char *)helper;
			lx_argv[out++] = (char *)payload_argv0_option;
			lx_argv[out++] = (char *)payload_argv0;
			lx_argv[out++] = (char *)links_option;
			lx_argv[out++] = (char *)links;
			lx_argv[out++] = (char *)gateway_option;
			lx_argv[out++] = (char *)gateway;
			lx_argv[out++] = (char *)separator;
			command = 1;
			continue;
		}
		if (!command && string_equal(argv[i], net))
			found_net++;
		lx_argv[out++] = argv[i];
	}
	lx_argv[out] = (void *)0;
	if (found_net > 1)
		fail("expected at most one --unshare-net");
	if (found_separator != 1)
		fail("expected one bubblewrap command separator");

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
