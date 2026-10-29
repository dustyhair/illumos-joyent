/*
 * CDDL version 1.0, http://www.illumos.org/license/CDDL.
 * Linux differential test and kernel-test fixture writer for the shared core.
 */
#define	_GNU_SOURCE
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "lx_seccomp_bpf.h"

#if !defined(__x86_64__)
#error The native oracle currently requires Linux x86-64
#endif

_Static_assert(sizeof (lx_scbpf_insn_t) ==
    sizeof (struct sock_filter), "instruction ABI");
_Static_assert(sizeof (lx_seccomp_data_t) ==
    sizeof (struct seccomp_data), "metadata ABI");
_Static_assert(offsetof(lx_seccomp_data_t, args) == 16, "argument offset");

/* CSTYLED */
#define	I(c, v)	((lx_scbpf_insn_t) { (c), 0, 0, (v) })
/* CSTYLED */
#define	J(c, v, t, f)	((lx_scbpf_insn_t) { (c), (t), (f), (v) })
#define	ALLOW	SECCOMP_RET_ALLOW
#define	ERRNO	SECCOMP_RET_ERRNO
#define	MAXBUF	(LX_SCBPF_MAXINSNS + 8)

static uint32_t workspace[LX_SCBPF_MAXINSNS];
static FILE *fixture;
static uint32_t ncells;
static uint32_t random_state = 0x159a371d;

static uint32_t
random_word(void)
{
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;
	return (random_state);
}

/* The same non-cloned call site supplies the kernel's actual saved PC. */
static __attribute__((noinline, noclone)) long
target_call(lx_seccomp_data_t *data, int run)
{
	register uint64_t r10 __asm__("r10") = data->args[3];
	register uint64_t r8 __asm__("r8") = data->args[4];
	register uint64_t r9 __asm__("r9") = data->args[5];
	uint64_t pc;
	long result = SYS_getppid;

	__asm__ volatile(
	    "leaq 1f(%%rip), %[pc]\n\t"
	    "test %[run], %[run]\n\t"
	    "jz 1f\n\t"
	    "syscall\n"
	    "1:"
	    : "+a" (result), [pc] "=&r" (pc)
	    : "D" (data->args[0]), "S" (data->args[1]),
	    "d" (data->args[2]), "r" (r10), "r" (r8), "r" (r9),
	    [run] "r" (run)
	    : "rcx", "r11", "cc", "memory");
	data->instruction_pointer = pc;
	return (result);
}

static int
read_exact(int fd, void *buf, size_t size)
{
	size_t pos = 0;

	while (pos < size) {
		ssize_t n = read(fd, (char *)buf + pos, size - pos);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return (0);
		pos += n;
	}
	return (1);
}

static void
put(const void *buf, size_t size)
{
	if (fwrite(buf, 1, size, fixture) != size) {
		perror("fixture write");
		exit(2);
	}
}

/* Wire: 16-byte record header, 64-byte metadata, then 8-byte instructions. */
static void
save_case(const lx_scbpf_insn_t *p, uint32_t n,
    const lx_seccomp_data_t *data, int valid, uint32_t value)
{
	uint32_t header[] = { n, (uint32_t)valid, value, 0 };

	put(header, sizeof (header));
	put(data, sizeof (*data));
	put(p, n * sizeof (*p));
	ncells++;
}

typedef struct installed {
	int ok;
	int error;
	lx_seccomp_data_t data;
} installed_t;

static int
check_case(const char *name, const lx_scbpf_insn_t *p, uint32_t n,
    int wrap)
{
	int fd[2], status, got;
	pid_t pid;
	installed_t result = { 0 };
	int64_t actual = 0, expected;
	uint32_t action, value;
	int valid = lx_seccomp_bpf_validate(p, n, workspace);

	if (pipe(fd) != 0 || (pid = fork()) == -1) {
		perror("fork/pipe");
		exit(2);
	}
	if (pid == 0) {
		lx_scbpf_insn_t code[MAXBUF];
		struct sock_fprog prog;
		long returned;
		uint32_t i, prefix = wrap ? 4 : 0;

		(void) close(fd[0]);
		(void) alarm(3);
		result.data.nr = SYS_getppid;
		result.data.arch = AUDIT_ARCH_X86_64;
		for (i = 0; i < 6; i++)
			result.data.args[i] = UINT64_C(0xa89710234567bcde) ^
			    (UINT64_C(0x159a371ddeadbeef) * (i + 1));
		(void) target_call(&result.data, 0);
		if (n + prefix > MAXBUF)
			_exit(2);
		if (wrap) {
			code[0] = I(BPF_LD | BPF_W | BPF_ABS, 0);
			code[1] = J(BPF_JMP | BPF_JEQ | BPF_K,
			    SYS_getppid, 1, 0);
			code[2] = I(BPF_RET | BPF_K, ALLOW);
			code[3] = I(BPF_LD | BPF_IMM, 0);
		}
		(void) memcpy(code + prefix, p, n * sizeof (*p));
		prog.len = n + prefix;
		prog.filter = (struct sock_filter *)(void *)code;
		if (prctl(PR_SET_NO_NEW_PRIVS, 1UL, 0UL, 0UL, 0UL) != 0)
			_exit(2);
		result.ok = syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER,
		    0, &prog) == 0;
		result.error = result.ok ? 0 : errno;
		if (write(fd[1], &result, sizeof (result)) != sizeof (result))
			_exit(2);
		if (!result.ok)
			_exit(0);
		returned = target_call(&result.data, 1);
		actual = returned;
		if (write(fd[1], &actual, sizeof (actual)) != sizeof (actual))
			_exit(2);
		_exit(0);
	}
	(void) close(fd[1]);
	got = read_exact(fd[0], &result, sizeof (result));
	if (got && result.ok)
		got = read_exact(fd[0], &actual, sizeof (actual)) ? 2 : 1;
	(void) close(fd[0]);
	if (waitpid(pid, &status, 0) != pid || !got) {
		fprintf(stderr, "%s: oracle child did not report\n", name);
		return (1);
	}
	if (!result.ok && result.error != EINVAL) {
		fprintf(stderr, "%s: unexpected install errno %d\n", name,
		    result.error);
		return (1);
	}
	if (valid != result.ok) {
		fprintf(stderr, "%s: validity core=%d Linux=%d errno=%d\n",
		    name, valid, result.ok, result.error);
		return (1);
	}
	value = valid ? lx_seccomp_bpf_eval(p, n, &result.data) : 0;
	if (valid) {
		action = value & SECCOMP_RET_ACTION_FULL;
		switch (action) {
		case SECCOMP_RET_ALLOW:
		case SECCOMP_RET_LOG:
			expected = getpid();
			break;
		case SECCOMP_RET_ERRNO:
			expected = value & SECCOMP_RET_DATA;
			if (expected > 4095)
				expected = 4095;
			expected = -expected;
			break;
		case SECCOMP_RET_TRACE:
		case SECCOMP_RET_USER_NOTIF:
			expected = -ENOSYS;
			break;
		default:
			if (!WIFSIGNALED(status) ||
			    WTERMSIG(status) != SIGSYS || got != 1) {
				fprintf(stderr, "%s: action %08x did not kill "
				    "as Linux SIGSYS\n", name, value);
				return (1);
			}
			goto save;
		}
		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
		    got != 2 || actual != expected) {
			fprintf(stderr, "%s: action=%08x raw=%lld "
			    "expected=%lld "
			    "status=%x\n", name, value, (long long)actual,
			    (long long)expected, status);
			return (1);
		}
	} else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "%s: rejected program child failed\n", name);
		return (1);
	}
save:
	save_case(p, n, &result.data, valid, value);
	return (0);
}

static int
basic_cases(void)
{
	lx_scbpf_insn_t p[MAXBUF];
	uint32_t values[] = { 0, ALLOW, ERRNO, ERRNO | EACCES,
	    ERRNO | 65535, SECCOMP_RET_KILL_PROCESS, SECCOMP_RET_TRAP,
	    SECCOMP_RET_TRACE, SECCOMP_RET_USER_NOTIF, SECCOMP_RET_LOG,
	    0x12340000 };
	uint32_t i, j, op;
	int failed = 0;
	char name[100];

	for (i = 0; i < sizeof (values) / sizeof (values[0]); i++) {
		p[0] = I(BPF_RET | BPF_K, values[i]);
		(void) snprintf(name, sizeof (name), "return %08x", values[i]);
		failed += check_case(name, p, 1, 1);
	}
	p[0] = I(BPF_RET | BPF_A, 0);
	failed += check_case("initial accumulator", p, 1, 1);
	p[0] = I(BPF_MISC | BPF_TXA, 0);
	p[1] = I(BPF_RET | BPF_A, 0);
	failed += check_case("initial X", p, 2, 1);
	for (i = 0; i < 2; i++) {
		p[0] = I(i ? BPF_LDX | BPF_W | BPF_LEN :
		    BPF_LD | BPF_W | BPF_LEN, 0);
		p[1] = I(i ? BPF_MISC | BPF_TXA : BPF_LD | BPF_W | BPF_LEN,
		    0);
		p[2] = I(BPF_ALU | BPF_OR | BPF_K, ERRNO);
		p[3] = I(BPF_RET | BPF_A, 0);
		failed += check_case("metadata length", p, 4, 1);
	}
	for (i = 0; i < 24; i++) {
		p[0] = I(BPF_LD | BPF_IMM, random_word());
		p[1] = I(BPF_ALU | BPF_NEG, 0);
		p[2] = I(BPF_MISC | BPF_TAX, 0);
		p[3] = I(BPF_ALU | BPF_RSH | BPF_K, 16);
		p[4] = I(BPF_ALU | BPF_XOR | BPF_X, 0);
		p[5] = I(BPF_ALU | BPF_AND | BPF_K, 0x7ff);
		p[6] = I(BPF_ALU | BPF_OR | BPF_K, ERRNO);
		p[7] = I(BPF_RET | BPF_A, 0);
		failed += check_case("negation", p, 8, 1);
	}
	for (i = 0; i < 70; i++) {
		p[0] = I(BPF_LD | BPF_W | BPF_ABS, i);
		p[1] = I(BPF_ALU | BPF_AND | BPF_K, 0x7ff);
		p[2] = I(BPF_ALU | BPF_OR | BPF_K, ERRNO);
		p[3] = I(BPF_RET | BPF_A, 0);
		(void) snprintf(name, sizeof (name), "metadata offset %u", i);
		failed += check_case(name, p, 4, 1);
	}
	for (op = 0; op <= 0xa0; op += 0x10) {
		if (op == BPF_NEG)
			continue;
		for (j = 0; j < 2; j++) {
			for (i = 0; i < 32; i++) {
				uint32_t operand = i < 4 ?
				    (uint32_t[]) { 0, 1, 32, 0xffffffff }[i] :
				    random_word();
				p[0] = I(BPF_LD | BPF_IMM, random_word());
				p[1] = I(BPF_LDX | BPF_IMM, operand);
				p[2] = I(BPF_ALU | op | (j ? BPF_X : BPF_K),
				    operand);
				p[3] = I(BPF_MISC | BPF_TAX, 0);
				p[4] = I(BPF_ALU | BPF_RSH | BPF_K, 16);
				p[5] = I(BPF_ALU | BPF_XOR | BPF_X, 0);
				p[6] = I(BPF_ALU | BPF_AND | BPF_K, 0x7ff);
				p[7] = I(BPF_ALU | BPF_OR | BPF_K, ERRNO);
				p[8] = I(BPF_RET | BPF_A, 0);
				(void) snprintf(name, sizeof (name),
				    "ALU op=%x source=%u operand=%x", op, j,
				    operand);
				failed += check_case(name, p, 9, 1);
			}
		}
	}
	for (op = BPF_JEQ; op <= BPF_JSET; op += 0x10) {
		for (j = 0; j < 2; j++) {
			for (i = 0; i < 24; i++) {
				uint32_t a = random_word();
				uint32_t b = (i & 1) ? a : random_word();
				p[0] = I(BPF_LD | BPF_IMM, a);
				p[1] = I(BPF_LDX | BPF_IMM, b);
				p[2] = J(BPF_JMP | op | (j ? BPF_X : BPF_K),
				    b, 0, 1);
				p[3] = I(BPF_RET | BPF_K, ERRNO | 91);
				p[4] = I(BPF_RET | BPF_K, ERRNO | 92);
				(void) snprintf(name, sizeof (name),
				    "jump op=%x source=%u A=%x B=%x",
				    op, j, a, b);
				failed += check_case(name, p, 5, 1);
			}
		}
	}
	for (i = 0; i < 18; i++) {
		p[0] = I(BPF_LD | BPF_IMM, ERRNO | 123);
		p[1] = I(BPF_ST, i);
		p[2] = I(BPF_LDX | BPF_MEM, i);
		p[3] = I(BPF_MISC | BPF_TXA, 0);
		p[4] = I(BPF_RET | BPF_A, 0);
		(void) snprintf(name, sizeof (name), "scratch cell %u", i);
		failed += check_case(name, p, 5, 1);
		p[0] = I(BPF_LDX | BPF_IMM, ERRNO | 124);
		p[1] = I(BPF_STX, i);
		p[2] = I(BPF_LD | BPF_MEM, i);
		p[3] = I(BPF_RET | BPF_A, 0);
		failed += check_case("scratch store X and load A", p, 4, 1);
	}
	p[0] = I(BPF_LD | BPF_MEM, 0);
	p[1] = I(BPF_RET | BPF_A, 0);
	failed += check_case("uninitialized scratch", p, 2, 1);
	p[0] = I(BPF_JMP | BPF_JA, 1);
	p[1] = I(BPF_LD | BPF_MEM, 0);
	p[2] = I(BPF_RET | BPF_K, ALLOW);
	failed += check_case("unreachable scratch", p, 3, 1);
	p[0] = J(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0);
	p[1] = I(BPF_ST, 0);
	p[2] = I(BPF_LD | BPF_MEM, 0);
	p[3] = I(BPF_RET | BPF_A, 0);
	failed += check_case("scratch missing on one path", p, 4, 1);
	for (i = 0; i < 3; i++) {
		p[0] = I(BPF_JMP | BPF_JA,
		    ((uint32_t[]) { 0, 1, UINT32_MAX })[i]);
		p[1] = I(BPF_RET | BPF_K, ALLOW);
		failed += check_case("absolute jump boundary", p, 2, 1);
	}
	p[0] = I(BPF_LD | BPF_IMM, 0);
	failed += check_case("no terminal return", p, 1, 1);
	failed += check_case("empty program", p, 0, 1);
	for (i = 0; i < LX_SCBPF_MAXINSNS + 1; i++)
		p[i] = I(BPF_RET | BPF_K, ALLOW);
	failed += check_case("maximum program", p, LX_SCBPF_MAXINSNS, 0);
	failed += check_case("oversize program", p, LX_SCBPF_MAXINSNS + 1, 0);
	return (failed);
}

static int
extra_cases(void)
{
	lx_scbpf_insn_t p[32];
	lx_seccomp_data_t data = {
		.nr = -1, .arch = 0x40000003,
		.instruction_pointer = UINT64_C(0x1122334455667788),
		.args = {
			UINT64_C(0x0123456789abcdef),
			UINT64_C(0xfedcba9876543210),
			UINT64_C(0xaabbccdd10203040),
			UINT64_C(0x9988776655443322),
			UINT64_C(0x0102030405060708),
			UINT64_C(0xf1f2f3f4f5f6f7f8)
		}
	};
	uint32_t words[] = { 0xffffffff, 0x40000003, 0x55667788, 0x11223344,
	    0x89abcdef, 0x01234567, 0x76543210, 0xfedcba98,
	    0x10203040, 0xaabbccdd, 0x55443322, 0x99887766,
	    0x05060708, 0x01020304, 0xf5f6f7f8, 0xf1f2f3f4 };
	uint32_t i, slot, mask, branch;
	int failed = 0;
	char name[100];

	/* Full-width ABI values have literal expectations, including i386. */
	for (i = 0; i < 16; i++) {
		p[0] = I(BPF_LD | BPF_W | BPF_ABS, i * 4);
		p[1] = I(BPF_RET | BPF_A, 0);
		if (!lx_seccomp_bpf_validate(p, 2, workspace) ||
		    lx_seccomp_bpf_eval(p, 2, &data) != words[i]) {
			fprintf(stderr, "literal metadata word %u failed\n", i);
			failed++;
		} else {
			save_case(p, 2, &data, 1, words[i]);
		}
	}
	p[0] = I(BPF_RET | BPF_K, ALLOW);
	if (lx_seccomp_bpf_validate(NULL, 1, workspace) ||
	    lx_seccomp_bpf_validate(p, 1, NULL) ||
	    lx_seccomp_bpf_validate(p, UINT32_MAX, workspace) ||
	    lx_seccomp_bpf_eval(NULL, 1, &data) != 0 ||
	    lx_seccomp_bpf_eval(p, 1, NULL) != 0 ||
	    lx_seccomp_bpf_eval(p, UINT32_MAX, &data) != 0) {
		fprintf(stderr, "API argument guards failed\n");
		failed++;
	}
	for (slot = 0; slot < 16; slot++) {
		for (mask = 0; mask < 4; mask++) {
			for (branch = 0; branch < 2; branch++) {
				p[0] = I(BPF_LD | BPF_W | BPF_ABS, 0);
				p[1] = J(BPF_JMP | BPF_JEQ | BPF_K,
				    SYS_getppid + branch, 0, 3);
				p[2] = I(BPF_LD | BPF_IMM, 124);
				p[3] = (mask & 1) ? I(BPF_ST, slot) :
				    I(BPF_LD | BPF_IMM, 125);
				p[4] = I(BPF_JMP | BPF_JA, 2);
				p[5] = I(BPF_LD | BPF_IMM, 126);
				p[6] = (mask & 2) ? I(BPF_ST, slot) :
				    I(BPF_LD | BPF_IMM, 127);
				p[7] = I(BPF_LD | BPF_MEM, slot);
				p[8] = I(BPF_ALU | BPF_OR | BPF_K, ERRNO);
				p[9] = I(BPF_RET | BPF_A, 0);
				(void) snprintf(name, sizeof (name),
				    "scratch join cell=%u stores=%u branch=%u",
				    slot, mask, branch);
				failed += check_case(name, p, 10, 1);
			}
		}
	}
	for (i = 0; i < 512; i++) {
		uint32_t j, n = 2 + random_word() % 23;
		for (j = 0; j + 1 < n; j++) {
			uint32_t r = random_word();
			switch (r % 8) {
			case 0: p[j] = I(BPF_ST, random_word() % 18); break;
			case 1:
				p[j] = I(BPF_LD | BPF_MEM, random_word() % 18);
				break;
			case 2:
				p[j] = I(BPF_LD | BPF_IMM, random_word());
				break;
			case 3:
				p[j] = J(BPF_JMP | BPF_JEQ | BPF_K, 0,
				    random_word() % 4, random_word() % 4);
				break;
			case 4: p[j] = I(BPF_JMP | BPF_JA, r % 4); break;
			case 5: p[j] = I(BPF_RET | BPF_K, ERRNO | (r & 127));
				break;
			case 6: p[j] = I(BPF_ALU | BPF_XOR | BPF_K, r); break;
			default: p[j] = I(BPF_LDX | BPF_IMM, r); break;
			}
		}
		p[n - 1] = I(BPF_RET | BPF_K, ERRNO | 44);
		(void) snprintf(name, sizeof (name), "seeded CFG case %u", i);
		failed += check_case(name, p, n, 1);
	}
	return (failed);
}

int
main(int argc, char **argv)
{
	struct rlimit core = { 0, 0 };
	uint32_t header[] = { 0x4c585342, 1, 0, 0 };
	lx_scbpf_insn_t p[3];
	uint32_t opcode;
	int failed;
	char name[80];

	if (argc < 2 || argc > 3)
		return (2);
	(void) setrlimit(RLIMIT_CORE, &core);
	(void) setvbuf(stdout, NULL, _IONBF, 0);
	if ((fixture = fopen(argv[1], "wb")) == NULL)
		return (2);
	put(header, sizeof (header));
	failed = basic_cases();
	failed += extra_cases();
	printf("focused cases: %u, failures: %d\n", ncells, failed);
	if (failed != 0)
		return (1);
	if (argc == 3 && strcmp(argv[2], "--exhaustive") == 0) {
		for (opcode = 0; opcode <= 0xffff; opcode++) {
			uint32_t k = 1;
			if (opcode == 0x20)
				k = 8;
			if (opcode == 0x05 || opcode == 0x60 || opcode == 0x61)
				k = 0;
			p[0] = I(BPF_ST, 0);
			p[1] = I(opcode, k);
			p[2] = I(BPF_RET | BPF_K, ALLOW);
			(void) snprintf(name, sizeof (name),
			    "opcode %04x", opcode);
			failed += check_case(name, p, 3, 1);
			if (failed != 0)
				break;
			if ((opcode & 0x1fff) == 0x1fff)
				printf("opcode checks: %u\n", opcode + 1);
		}
	}
	if (failed != 0)
		return (1);
	if (fseek(fixture, 8, SEEK_SET) != 0)
		return (2);
	put(&ncells, sizeof (ncells));
	if (fclose(fixture) != 0)
		return (2);
	printf("PASS: %u cases (%u Linux comparisons, 16 literal ABI)\n",
	    ncells, ncells - 16);
	return (0);
}
