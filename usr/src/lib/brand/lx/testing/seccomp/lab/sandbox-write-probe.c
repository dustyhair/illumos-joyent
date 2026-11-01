typedef long ssize_t;

#define	SYS_READ	0
#define	SYS_WRITE	1
#define	SYS_CLOSE	3
#define	SYS_OPENAT	257
#define	SYS_EXIT	60
#define	AT_FDCWD	-100
#define	O_RDONLY	0
#define	O_WRONLY	1
#define	O_CREAT		0100
#define	O_TRUNC		01000

static long
syscall1(long nr, long a0)
{
	register long rax __asm__("rax") = nr;
	register long rdi __asm__("rdi") = a0;

	__asm__ volatile("syscall" : "+a" (rax) : "D" (rdi) :
	    "rcx", "r11", "memory");
	return (rax);
}

static long
syscall3(long nr, long a0, long a1, long a2)
{
	register long rax __asm__("rax") = nr;
	register long rdi __asm__("rdi") = a0;
	register long rsi __asm__("rsi") = a1;
	register long rdx __asm__("rdx") = a2;

	__asm__ volatile("syscall" : "+a" (rax) : "D" (rdi), "S" (rsi),
	    "d" (rdx) : "rcx", "r11", "memory");
	return (rax);
}

static long
syscall4(long nr, long a0, long a1, long a2, long a3)
{
	register long rax __asm__("rax") = nr;
	register long rdi __asm__("rdi") = a0;
	register long rsi __asm__("rsi") = a1;
	register long rdx __asm__("rdx") = a2;
	register long r10 __asm__("r10") = a3;

	__asm__ volatile("syscall" : "+a" (rax) : "D" (rdi), "S" (rsi),
	    "d" (rdx), "r" (r10) : "rcx", "r11", "memory");
	return (rax);
}

static void
fail(void)
{
	static const char message[] = "SANDBOX_WRITE_PROBE_FAIL\n";

	(void) syscall3(SYS_WRITE, 2, (long)message, sizeof (message) - 1);
	(void) syscall1(SYS_EXIT, 1);
}

void
_start(void)
{
	static const char path[] = "/test/codex-sandbox-write";
	static const char marker[] = "sandbox-write-pass\n";
	static const char pass[] = "SANDBOX_WRITE_PROBE_PASS\n";
	char buffer[sizeof (marker)];
	int fd;
	ssize_t nread;
	unsigned long i;

	fd = (int)syscall4(SYS_OPENAT, AT_FDCWD, (long)path,
	    O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0 || syscall3(SYS_WRITE, fd, (long)marker,
	    sizeof (marker) - 1) != sizeof (marker) - 1 ||
	    syscall1(SYS_CLOSE, fd) != 0)
		fail();
	fd = (int)syscall4(SYS_OPENAT, AT_FDCWD, (long)path, O_RDONLY, 0);
	if (fd < 0)
		fail();
	nread = syscall3(SYS_READ, fd, (long)buffer, sizeof (buffer));
	if (syscall1(SYS_CLOSE, fd) != 0 || nread != sizeof (marker) - 1)
		fail();
	for (i = 0; i < sizeof (marker) - 1; i++) {
		if (buffer[i] != marker[i])
			fail();
	}
	(void) syscall3(SYS_WRITE, 1, (long)pass, sizeof (pass) - 1);
	(void) syscall1(SYS_EXIT, 0);
}
