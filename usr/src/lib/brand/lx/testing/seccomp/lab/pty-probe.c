typedef unsigned long ulong_t;
typedef long ssize_t;

#define	SYS_READ	0
#define	SYS_WRITE	1
#define	SYS_CLOSE	3
#define	SYS_IOCTL	16
#define	SYS_OPENAT	257
#define	SYS_EXIT	60
#define	AT_FDCWD	-100
#define	O_RDWR		2
#define	O_NOCTTY	0400
#define	TIOCSPTLCK	0x40045431
#define	TIOCGPTN	0x80045430

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
	static const char msg[] = "PTY_PROBE_FAIL\n";

	(void) syscall3(SYS_WRITE, 2, (long)msg, sizeof (msg) - 1);
	(void) syscall1(SYS_EXIT, 1);
}

void
_start(void)
{
	static const char ptmx[] = "/dev/ptmx";
	static const char prefix[] = "/dev/pts/";
	static const char input[] = "pty-pass\n";
	static const char pass[] = "PTY_PROBE_PASS\n";
	char path[32];
	char buf[32];
	unsigned int ptn = 0;
	unsigned int zero = 0;
	unsigned int value;
	int master;
	int slave;
	int i = 0;
	int j;
	long n;

	master = (int)syscall4(SYS_OPENAT, AT_FDCWD, (long)ptmx,
	    O_RDWR | O_NOCTTY, 0);
	if (master < 0 || syscall3(SYS_IOCTL, master, TIOCSPTLCK,
	    (long)&zero) < 0 || syscall3(SYS_IOCTL, master, TIOCGPTN,
	    (long)&ptn) < 0)
		fail();

	for (j = 0; prefix[j] != '\0'; j++)
		path[i++] = prefix[j];
	value = ptn;
	if (value >= 100)
		path[i++] = (char)('0' + (value / 100) % 10);
	if (value >= 10)
		path[i++] = (char)('0' + (value / 10) % 10);
	path[i++] = (char)('0' + value % 10);
	path[i] = '\0';

	slave = (int)syscall4(SYS_OPENAT, AT_FDCWD, (long)path,
	    O_RDWR | O_NOCTTY, 0);
	if (slave < 0 || syscall3(SYS_WRITE, master, (long)input,
	    sizeof (input) - 1) != sizeof (input) - 1)
		fail();
	n = syscall3(SYS_READ, slave, (long)buf, sizeof (buf));
	if (n < 8 || buf[0] != 'p' || buf[7] != 's')
		fail();

	(void) syscall1(SYS_CLOSE, slave);
	(void) syscall1(SYS_CLOSE, master);
	(void) syscall3(SYS_WRITE, 1, (long)pass, sizeof (pass) - 1);
	(void) syscall1(SYS_EXIT, 0);
}
