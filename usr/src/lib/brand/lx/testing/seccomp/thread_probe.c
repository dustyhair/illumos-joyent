/* CDDL version 1.0, http://www.illumos.org/license/CDDL. */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static __thread unsigned long tls_value;
static pid_t parent_pid, parent_tid;
static atomic_int count, ready, release_worker, signal_seen;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static int
install(void)
{
    struct sock_filter code[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
            offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_getppid, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EACCES),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)
    };
    struct sock_fprog fp = { sizeof (code) / sizeof (code[0]), code };
    parent_pid = syscall(SYS_getpid);
    parent_tid = syscall(SYS_gettid);
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

static void *
worker(void *arg)
{
    unsigned long value = (uintptr_t)arg;
    if (tls_value != 0 || !restricted() ||
        syscall(SYS_getpid) != parent_pid ||
        syscall(SYS_gettid) == parent_tid)
        return ((void *)1);
    tls_value = value;
    for (int i = 0; i < 50; i++) {
        if (pthread_mutex_lock(&mutex))
            return ((void *)2);
        count++;
        if (pthread_mutex_unlock(&mutex) || tls_value != value)
            return ((void *)3);
    }
    return ((void *)0x1234);
}

static int
pthread_tls_join(void)
{
    pthread_t threads[4];
    void *result;
    tls_value = 0xbeef;
    if (install())
        return (1);
    for (unsigned int i = 0; i < 4; i++) {
        int error = pthread_create(&threads[i], NULL, worker,
            (void *)(uintptr_t)(i + 1));
        if (error) {
            printf("pthread_create error=%d\n", error);
            return (2);
        }
    }
    for (int i = 0; i < 4; i++) {
        if (pthread_join(threads[i], &result) || result != (void *)0x1234)
            return (3);
    }
    return (count != 200 || tls_value != 0xbeef || !restricted());
}

static void
signal_handler(int sig)
{
    (void)sig;
    if (restricted())
        signal_seen = 1;
}

static void *
signalled_worker(void *arg)
{
    (void)arg;
    ready = 1;
    while (!release_worker)
        syscall(SYS_sched_yield);
    return ((void *)(uintptr_t)(!signal_seen || !restricted()));
}

static int
thread_signal(void)
{
    struct sigaction sa = { .sa_handler = signal_handler };
    pthread_t thread;
    void *result;
    if (sigaction(SIGUSR1, &sa, NULL) || install() ||
        pthread_create(&thread, NULL, signalled_worker, NULL))
        return (1);
    while (!ready)
        syscall(SYS_sched_yield);
    if (pthread_kill(thread, SIGUSR1))
        return (2);
    while (!signal_seen)
        syscall(SYS_sched_yield);
    release_worker = 1;
    return (pthread_join(thread, &result) || result != NULL || !restricted());
}

static void *
last_worker(void *arg)
{
    struct timespec delay = { .tv_nsec = 100000000 };
    (void)arg;
    ready = 1;
    while (!release_worker)
        syscall(SYS_sched_yield);
    syscall(SYS_nanosleep, &delay, NULL);
    syscall(SYS_exit, 42);
    return ((void *)1);
}

static int
last_thread_status(void)
{
    pthread_t thread;
    if (install() || pthread_create(&thread, NULL, last_worker, NULL))
        return (1);
    while (!ready)
        syscall(SYS_sched_yield);
    release_worker = 1;
    syscall(SYS_exit, 0);
    return (2);
}

static int
exit_group_status(void)
{
    pthread_t thread;
    if (install() || pthread_create(&thread, NULL, signalled_worker, NULL))
        return (1);
    while (!ready)
        syscall(SYS_sched_yield);
    syscall(SYS_exit_group, 37);
    return (2);
}

static void *
escaping_worker(void *arg)
{
    (void)arg;
    __asm__ volatile("int $0x91" ::: "memory", "cc");
    return ((void *)1);
}

static int
thread_native_escape(void)
{
    pthread_t thread;
    if (install() || pthread_create(&thread, NULL, escaping_worker, NULL))
        return (1);
    pthread_join(thread, NULL);
    return (2);
}

static int
run(const char *name, int (*fn)(void), int status_code, int sig)
{
    int status;
    pid_t pid = fork();
    if (pid < 0)
        exit(2);
    if (pid == 0) {
        alarm(10);
        _exit(fn());
    }
    if (waitpid(pid, &status, 0) != pid)
        exit(2);
    int ok = sig ? WIFSIGNALED(status) && WTERMSIG(status) == sig :
        WIFEXITED(status) && WEXITSTATUS(status) == status_code;
    printf("%s: %s status=0x%x\n", name, ok ? "PASS" : "FAIL", status);
    return (!ok);
}

int
main(void)
{
    int failed = 0, total = 0;
#define RUN(fn, code, sig) do { total++; failed += run(#fn, fn, code, sig); } while (0)
    RUN(pthread_tls_join, 0, 0); RUN(thread_signal, 0, 0);
    RUN(last_thread_status, 42, 0); RUN(exit_group_status, 37, 0);
    if (access("/native/usr/bin/uname", X_OK) == 0)
        RUN(thread_native_escape, 0, SIGSYS);
#undef RUN
    printf("%d thread checks, %d failed\n", total, failed);
    return (failed != 0);
}
