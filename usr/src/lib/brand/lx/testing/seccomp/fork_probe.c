/* CDDL version 1.0, http://www.illumos.org/license/CDDL. */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static __thread volatile unsigned long tls_value;
static volatile sig_atomic_t handler_ok;
static volatile pid_t handler_child;

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

static int
reap(pid_t pid)
{
    int status;
    return (pid <= 0 || waitpid(pid, &status, 0) != pid ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0);
}

static int
fork_case(int libc)
{
    pid_t pid;
    tls_value = 0xfeed1234UL;
    if (deny(SYS_getppid))
        return (1);
    pid = libc ? fork() : syscall(SYS_fork);
    if (pid == 0)
        _exit(!restricted() || tls_value != 0xfeed1234UL);
    return (reap(pid) || !restricted());
}
static int raw_fork(void) { return (fork_case(0)); }
static int libc_fork(void) { return (fork_case(1)); }

static int
clone_tids(void)
{
    int parent_tid = -1, child_tid = -1;
    pid_t pid;
    if (deny(SYS_getppid))
        return (1);
    pid = syscall(SYS_clone, SIGCHLD | CLONE_PARENT_SETTID |
        CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID,
        NULL, &parent_tid, &child_tid, NULL);
    if (pid == 0)
        _exit(child_tid != syscall(SYS_gettid) || !restricted() ||
            parent_tid != -1);
    return (parent_tid != pid || reap(pid) || child_tid != -1);
}

static int
clear_tid(void)
{
    int *shared = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    pid_t pid;
    if (shared == MAP_FAILED || deny(SYS_getppid))
        return (1);
    *shared = -1;
    pid = syscall(SYS_clone, SIGCHLD | CLONE_CHILD_SETTID |
        CLONE_CHILD_CLEARTID, NULL, NULL, shared, NULL);
    if (pid == 0)
        _exit(*shared != syscall(SYS_gettid) || !restricted());
    return (reap(pid) || *shared != pid);
}

static int
child_stacking(void)
{
    pid_t pid;
    if (deny(SYS_getppid))
        return (1);
    pid = syscall(SYS_fork);
    if (pid == 0) {
        if (deny(SYS_getpid) || !restricted())
            _exit(1);
        errno = 0;
        _exit(syscall(SYS_getpid) != -1 || errno != EACCES);
    }
    return (reap(pid) || syscall(SYS_getpid) <= 0 || !restricted());
}

static int
memory_fds(void)
{
    volatile int value = 17;
    int fds[2];
    pid_t pid;
    char c;
    if (pipe(fds) || deny(SYS_getppid))
        return (1);
    pid = syscall(SYS_fork);
    if (pid == 0) {
        value = 99;
        close(fds[0]);
        _exit(write(fds[1], "x", 1) != 1 || !restricted());
    }
    return (reap(pid) || value != 17 || read(fds[0], &c, 1) != 1 ||
        c != 'x');
}

static int
fork_policy(void)
{
    pid_t pid;
    if (deny(SYS_fork))
        return (1);
    errno = 0;
    pid = syscall(SYS_fork);
    if (pid == 0)
        _exit(3);
    return (pid != -1 || errno != EACCES);
}

static int
bad_tid(int parent)
{
    unsigned long flags = SIGCHLD |
        (parent ? CLONE_PARENT_SETTID : CLONE_CHILD_SETTID);
    if (deny(SYS_getppid))
        return (1);
    pid_t pid = syscall(SYS_clone, flags, NULL, (void *)1, (void *)1,
        NULL);
    if (pid == 0)
        _exit(!restricted());
    return (reap(pid));
}
static int bad_parent_tid(void) { return (bad_tid(1)); }
static int bad_child_tid(void) { return (bad_tid(0)); }

static int
readonly_child_tid(void)
{
    int *tid = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    pid_t pid;
    if (tid == MAP_FAILED)
        return (1);
    *tid = 1234;
    if (mprotect(tid, 4096, PROT_READ) || deny(SYS_getppid))
        return (1);
    pid = syscall(SYS_clone, SIGCHLD | CLONE_CHILD_SETTID,
        NULL, NULL, tid, NULL);
    if (pid == 0)
        _exit(*tid != 1234 || !restricted());
    return (reap(pid) || *tid != 1234);
}

static int
stack_child(void *arg)
{
    return (arg != (void *)0x1234 || !restricted());
}

static int
clone_stack(void)
{
    void *stack = mmap(NULL, 65536, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED || deny(SYS_getppid))
        return (1);
    return (reap(clone(stack_child, (char *)stack + 65536, SIGCHLD,
        (void *)0x1234)));
}

static int
clone_no_signal(void)
{
    pid_t pid;
    int status;
    if (deny(SYS_getppid))
        return (1);
    pid = syscall(SYS_clone, 0, NULL, NULL, NULL, NULL);
    if (pid == 0)
        _exit(!restricted());
    return (pid < 0 || waitpid(pid, &status, __WCLONE) != pid ||
        status != 0);
}

static int
child_native_escape(void)
{
    pid_t pid;
    int status;
    if (deny(SYS_getppid))
        return (1);
    pid = syscall(SYS_fork);
    if (pid == 0) {
        __asm__ volatile("int $0x91" ::: "memory", "cc");
        _exit(1);
    }
    return (pid < 0 || waitpid(pid, &status, 0) != pid ||
        !WIFSIGNALED(status) || WTERMSIG(status) != SIGSYS);
}

static int
unsupported_flags(void)
{
    if (deny(SYS_getppid))
        return (1);
    errno = 0;
    return (syscall(SYS_clone, SIGCHLD | CLONE_NEWUSER, NULL, NULL,
        NULL, NULL) != -1 || errno != EINVAL);
}

static void
fork_handler(int sig)
{
    pid_t pid;
    (void)sig;
    pid = syscall(SYS_fork);
    handler_child = pid;
    handler_ok = pid >= 0 && restricted();
}

static int
handler_fork(void)
{
    struct sigaction sa = { .sa_handler = fork_handler };
    pid_t pid = syscall(SYS_getpid);
    if (sigaction(SIGUSR1, &sa, NULL) || deny(SYS_getppid))
        return (1);
    if (syscall(SYS_kill, pid, SIGUSR1))
        return (1);
    /* The child must return through its independently copied signal record. */
    if (handler_child == 0)
        _exit(!handler_ok || !restricted());
    return (!handler_ok || reap(handler_child) || !restricted());
}

static int
run(const char *name, int (*fn)(void))
{
    int status;
    pid_t pid = fork();
    if (pid < 0)
        exit(2);
    if (pid == 0) {
        alarm(8);
        _exit(fn());
    }
    if (waitpid(pid, &status, 0) != pid)
        exit(2);
    int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    printf("%s: %s status=0x%x\n", name, ok ? "PASS" : "FAIL", status);
    return (!ok);
}

int
main(void)
{
    int failed = 0, count = 0;
#define RUN(fn) do { count++; failed += run(#fn, fn); } while (0)
    RUN(raw_fork); RUN(libc_fork); RUN(clone_tids); RUN(clear_tid);
    RUN(child_stacking); RUN(memory_fds); RUN(fork_policy); RUN(handler_fork);
    RUN(bad_parent_tid); RUN(bad_child_tid); RUN(readonly_child_tid);
    RUN(clone_stack); RUN(clone_no_signal);
    if (access("/native/usr/bin/uname", X_OK) == 0) {
        RUN(unsupported_flags);
        RUN(child_native_escape);
    }
#undef RUN
    printf("%d fork checks, %d failed\n", count, failed);
    return (failed != 0);
}
