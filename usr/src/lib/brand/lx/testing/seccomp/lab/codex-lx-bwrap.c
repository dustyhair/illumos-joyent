/*
 * Minimal Linux x86-64 exec wrapper.  It is intentionally libc-free so the
 * compatibility layer adds no runtime dependency inside the lx zone.
 */

typedef unsigned long lx_size_t;

#define	LX_SYS_WRITE	1
#define	LX_SYS_EXECVE	59
#define	LX_SYS_EXIT	60
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
	static const char prefix[] = "codex-lx-bwrap: ";
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
	static const char mode[] = "CODEX_LX_SHARED_NETWORK=1";
	static const char real_name[] = "CODEX_LX_REAL_BWRAP=";
	static const char net[] = "--unshare-net";
	static const char separator[] = "--";
	char **argv = (char **)(stack + 1);
	char **envp = argv + stack[0] + 1;
	const char *real = (void *)0;
	lx_size_t argc = stack[0];
	lx_size_t found_net = 0;
	lx_size_t out = 1;
	lx_size_t i;
	int shared = 0;
	int command = 0;

	for (i = 0; envp[i] != (void *)0; i++) {
		if (string_equal(envp[i], mode))
			shared = 1;
		if (string_prefix(envp[i], real_name))
			real = envp[i] + sizeof (real_name) - 1;
	}
	if (real == (void *)0 || real[0] != '/')
		fail("CODEX_LX_REAL_BWRAP must be an absolute path");
	argv[0] = (char *)real;
	if (!shared) {
		(void) lx_syscall3(LX_SYS_EXECVE, (long)real,
		    (long)argv, (long)envp);
		fail("cannot execute bwrap.real");
	}

	for (i = 1; i < argc; i++) {
		if (!command && string_equal(argv[i], separator))
			command = 1;
		if (!command && string_equal(argv[i], net)) {
			found_net++;
			continue;
		}
		argv[out++] = argv[i];
	}
	argv[out] = (void *)0;
	if (found_net != 1)
		fail("shared-network mode expected exactly one --unshare-net");

	(void) lx_syscall3(LX_SYS_EXECVE, (long)real, (long)argv,
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
