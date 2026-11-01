typedef long ssize_t;

#define	SYS_WRITE	1
#define	SYS_OPENAT	257
#define	SYS_EXIT	60
#define	AT_FDCWD	-100
#define	O_WRONLY	1
#define	O_CREAT		0100
#define	O_TRUNC		01000
#define	LINUX_EROFS	30

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
fail(const char *message, unsigned long length)
{
	(void) syscall3(SYS_WRITE, 2, (long)message, length);
	(void) syscall1(SYS_EXIT, 1);
}

static void
expect_readonly(const char *path, const char *writable, unsigned long length)
{
	static const char eacces[] = "READONLY_PROBE_EACCES\n";
	static const char enoent[] = "READONLY_PROBE_ENOENT\n";
	static const char other[] = "READONLY_PROBE_OTHER_ERROR\n";
	long fd = syscall4(SYS_OPENAT, AT_FDCWD, (long)path,
	    O_WRONLY | O_CREAT | O_TRUNC, 0600);

	if (fd == -LINUX_EROFS)
		return;
	if (fd >= 0)
		fail(writable, length);
	if (fd == -13)
		fail(eacces, sizeof (eacces) - 1);
	if (fd == -2)
		fail(enoent, sizeof (enoent) - 1);
	fail(other, sizeof (other) - 1);
}

void
_start(void)
{
	static const char lofs_path[] = "/usr/codex-readonly-probe";
	static const char tmpfs_path[] = "/tmp/.codex/readonly-probe";
	static const char lofs_writable[] = "LOFS_READONLY_PROBE_WRITABLE\n";
	static const char tmpfs_writable[] = "TMPFS_READONLY_PROBE_WRITABLE\n";
	static const char pass[] = "SANDBOX_READONLY_PROBE_PASS\n";

	expect_readonly(lofs_path, lofs_writable, sizeof (lofs_writable) - 1);
	expect_readonly(tmpfs_path, tmpfs_writable,
	    sizeof (tmpfs_writable) - 1);
	(void) syscall3(SYS_WRITE, 1, (long)pass, sizeof (pass) - 1);
	(void) syscall1(SYS_EXIT, 0);
}
