/* CDDL version 1.0, http://www.illumos.org/license/CDDL. */
#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *self;
static unsigned char alternate[32768];
static volatile sig_atomic_t caught_count;

static int
deny(int nr)
{
    struct sock_filter code[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
            offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, nr, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EACCES),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)
    };
    struct sock_fprog fp = { sizeof (code) / sizeof (code[0]), code };
    return (prctl(PR_SET_NO_NEW_PRIVS, 1UL, 0UL, 0UL, 0UL) != 0 ||
        syscall(SYS_seccomp, 1, 0, &fp) != 0);
}

static int
restricted(void)
{
    errno = 0;
    return (syscall(SYS_getppid) == -1 && errno == EACCES &&
        prctl(PR_GET_NO_NEW_PRIVS, 0UL, 0UL, 0UL, 0UL) == 1 &&
        prctl(PR_GET_SECCOMP, 0UL, 0UL, 0UL, 0UL) == 2);
}

static void caught(int sig) { (void)sig; caught_count++; }

static int
prepare(void)
{
    struct sigaction sa = { .sa_handler = caught };
    sigset_t mask;
    stack_t ss = { .ss_sp = alternate, .ss_size = sizeof (alternate) };
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    if (sigaction(SIGUSR1, &sa, NULL))
        return (1);
    sa.sa_handler = SIG_IGN;
    return (sigaction(SIGUSR2, &sa, NULL) ||
        sigprocmask(SIG_BLOCK, &mask, NULL) || sigaltstack(&ss, NULL) ||
        deny(SYS_getppid));
}

static int
helper(int argc, char **argv)
{
    struct sigaction sa;
    sigset_t mask;
    stack_t ss;
    unsigned long phdr = getauxval(AT_PHDR);
    unsigned long random = getauxval(AT_RANDOM);
    volatile unsigned char byte;
    if (argc != 3 || strcmp(argv[2], "payload") ||
        !getenv("LX_EXEC_MARKER") ||
        strcmp(getenv("LX_EXEC_MARKER"), "preserved") || !restricted())
        return (1);
    if (!phdr || getauxval(AT_PHENT) != sizeof (Elf64_Phdr) ||
        !getauxval(AT_PHNUM) || !getauxval(AT_ENTRY) ||
        getauxval(AT_PAGESZ) != 4096 || getauxval(AT_CLKTCK) != 100 ||
        getauxval(AT_UID) != getuid() || getauxval(AT_EUID) != geteuid() ||
        getauxval(AT_GID) != getgid() || getauxval(AT_EGID) != getegid() ||
        !random)
        return (2);
    byte = *(unsigned char *)phdr;
    byte = *(unsigned char *)random;
    (void)byte;
    if (sigaction(SIGUSR1, NULL, &sa) || sa.sa_handler != SIG_DFL ||
        sigaction(SIGUSR2, NULL, &sa) || sa.sa_handler != SIG_IGN ||
        sigprocmask(SIG_SETMASK, NULL, &mask) ||
        !sigismember(&mask, SIGTERM) || sigaltstack(NULL, &ss) ||
        !(ss.ss_flags & SS_DISABLE))
        return (3);
    sa.sa_handler = caught;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR1, &sa, NULL) ||
        syscall(SYS_kill, syscall(SYS_getpid), SIGUSR1) ||
        caught_count != 1 || !restricted())
        return (4);
    return (0);
}

static void
do_exec(const char *path, const char *mode)
{
    char *args[] = { (char *)path, (char *)mode, "payload", NULL };
    char *env[] = { "LX_EXEC_MARKER=preserved", NULL };
    syscall(SYS_execve, path, args, env);
}

static int
static_exec(void)
{
    if (prepare())
        return (1);
    do_exec(self, "helper");
    return (9);
}

static int
dynamic_exec(void)
{
    if (prepare())
        return (1);
    do_exec(getenv("LX_EXEC_DYNAMIC") ? getenv("LX_EXEC_DYNAMIC") :
        "/test/exec_dynamic", "helper");
    return (9);
}

static int
failed_exec(void)
{
    char *args[] = { "missing", NULL };
    if (deny(SYS_getppid))
        return (1);
    errno = 0;
    return (syscall(SYS_execve, "/test/does-not-exist", args, NULL) != -1 ||
        errno != ENOENT || !restricted());
}

static int
exec_policy(void)
{
    char *args[] = { (char *)self, "helper", "payload", NULL };
    if (deny(SYS_execve))
        return (1);
    errno = 0;
    return (syscall(SYS_execve, self, args, NULL) != -1 || errno != EACCES);
}

static int
native_exec(void)
{
    if (deny(SYS_getppid))
        return (1);
    errno = 0;
    do_exec("/native/usr/bin/true", "unused");
    return (errno != ENOEXEC || !restricted());
}

static int
pwr_default(void)
{
    if (deny(SYS_getppid))
        return (1);
    do_exec(self, "pwr");
    return (9);
}

static int
codex_version(void)
{
    char *args[] = { "/test/codex-current/bin/codex", "--version", NULL };
    /* Deny Internet sockets while retaining local IPC. */
    struct sock_filter code[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
            offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_socket, 0, 4),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
            offsetof(struct seccomp_data, args[0])),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 2, 1, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 10, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EACCES),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)
    };
    struct sock_fprog fp = { sizeof (code) / sizeof (code[0]), code };
    if (prctl(PR_SET_NO_NEW_PRIVS, 1UL, 0UL, 0UL, 0UL) != 0 ||
        syscall(SYS_seccomp, 1, 0, &fp) != 0)
        return (1);
    syscall(SYS_execve, args[0], args, NULL);
    return (9);
}

static int
run(const char *name, int (*fn)(void), int sig)
{
    int status;
    pid_t pid = fork();
    if (pid < 0)
        exit(2);
    if (pid == 0) {
        alarm(12);
        _exit(fn());
    }
    if (waitpid(pid, &status, 0) != pid)
        exit(2);
    int ok = sig ? WIFSIGNALED(status) && WTERMSIG(status) == sig :
        WIFEXITED(status) && WEXITSTATUS(status) == 0;
    printf("%s: %s status=0x%x\n", name, ok ? "PASS" : "FAIL", status);
    return (!ok);
}

int
main(int argc, char **argv)
{
    int failed = 0, count = 0;
    self = argv[0];
    if (argc > 1 && !strcmp(argv[1], "helper"))
        return (helper(argc, argv));
    if (argc > 1 && !strcmp(argv[1], "pwr")) {
        syscall(SYS_kill, syscall(SYS_getpid), SIGPWR);
        return (9);
    }
#define RUN(fn, sig) do { count++; failed += run(#fn, fn, sig); } while (0)
    RUN(static_exec, 0); RUN(failed_exec, 0); RUN(exec_policy, 0);
    RUN(pwr_default, SIGPWR);
    if (access(getenv("LX_EXEC_DYNAMIC") ? getenv("LX_EXEC_DYNAMIC") :
        "/test/exec_dynamic", X_OK) == 0)
        RUN(dynamic_exec, 0);
    if (access("/native/usr/bin/uname", X_OK) == 0) {
        RUN(native_exec, 0);
        if (access("/test/codex-current/bin/codex", X_OK) == 0)
            RUN(codex_version, 0);
    }
#undef RUN
    printf("%d exec checks, %d failed\n", count, failed);
    return (failed != 0);
}
