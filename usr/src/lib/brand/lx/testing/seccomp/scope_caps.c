/* CDDL version 1.0, http://www.illumos.org/license/CDDL. */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/capability.h>
#include <linux/securebits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct caps { uint64_t e, p, i; } caps_t;
static int failures, cases;
static int lx;
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "line %d: %s errno=%d\n", __LINE__, #x, errno); \
    return 1; } } while (0)

static int getcaps(int tid, caps_t *c)
{
    struct __user_cap_header_struct h = { _LINUX_CAPABILITY_VERSION_3, tid };
    struct __user_cap_data_struct d[2];
    if (syscall(SYS_capget, &h, d) != 0) return -1;
    c->e = d[0].effective | (uint64_t)d[1].effective << 32;
    c->p = d[0].permitted | (uint64_t)d[1].permitted << 32;
    c->i = d[0].inheritable | (uint64_t)d[1].inheritable << 32;
    return 0;
}
static int setcaps(caps_t c)
{
    struct __user_cap_header_struct h = { _LINUX_CAPABILITY_VERSION_3, 0 };
    struct __user_cap_data_struct d[2] = {
        { c.e, c.p, c.i }, { c.e >> 32, c.p >> 32, c.i >> 32 }
    };
    return syscall(SYS_capset, &h, d);
}
static int same(caps_t a, caps_t b)
{
    return a.e == b.e && a.p == b.p && a.i == b.i;
}
static int basic(void)
{
    caps_t c, after;
    CHECK(getcaps(0, &c) == 0 && (c.e & (1ULL << CAP_SETPCAP)));
    c.i = 1ULL << CAP_CHOWN;
    CHECK(setcaps(c) == 0 && getcaps(0, &after) == 0 && same(c, after));
    c.e &= ~(1ULL << CAP_CHOWN);
    CHECK(setcaps(c) == 0);
    c.e |= 1ULL << CAP_CHOWN;
    CHECK(setcaps(c) == 0);
    c.e &= ~(1ULL << CAP_CHOWN);
    c.p &= ~(1ULL << CAP_CHOWN);
    CHECK(setcaps(c) == 0);
    after = c; after.e |= 1ULL << CAP_CHOWN;
    errno = 0;
    CHECK(setcaps(after) == -1 && errno == EPERM);
    CHECK(getcaps(0, &after) == 0 && same(c, after));
    return 0;
}
static int bounding(void)
{
    caps_t c, after;
    CHECK(getcaps(0, &c) == 0);
    c.i &= ~(1ULL << CAP_NET_RAW);
    CHECK(setcaps(c) == 0);
    CHECK(prctl(PR_CAPBSET_READ, CAP_NET_RAW, 0, 0, 0) == 1);
    CHECK(prctl(PR_CAPBSET_DROP, CAP_NET_RAW, 0, 0, 0) == 0);
    CHECK(prctl(PR_CAPBSET_READ, CAP_NET_RAW, 0, 0, 0) == 0);
    CHECK(getcaps(0, &c) == 0 && (c.p & (1ULL << CAP_NET_RAW)));
    after = c; after.i |= 1ULL << CAP_NET_RAW;
    errno = 0;
    CHECK(setcaps(after) == -1 && errno == EPERM);
    CHECK(getcaps(0, &after) == 0 && same(c, after));
    c.e &= ~(1ULL << CAP_SETPCAP); c.p &= ~(1ULL << CAP_SETPCAP);
    CHECK(setcaps(c) == 0);
    errno = 0;
    CHECK(prctl(PR_CAPBSET_DROP, 999, 0, 0, 0) == -1 && errno == EPERM);
    return 0;
}
static int securebits(void)
{
    CHECK(prctl(PR_GET_KEEPCAPS, 0, 0, 0, 0) == 0);
    CHECK(prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0) == 0);
    CHECK(prctl(PR_GET_KEEPCAPS, 0, 0, 0, 0) == 1);
    unsigned b = SECBIT_KEEP_CAPS | SECBIT_KEEP_CAPS_LOCKED;
    CHECK(prctl(PR_SET_SECUREBITS, b, 0, 0, 0) == 0);
    CHECK(prctl(PR_GET_SECUREBITS, 0, 0, 0, 0) == (int)b);
    errno = 0;
    CHECK(prctl(PR_SET_KEEPCAPS, 0, 0, 0, 0) == -1 && errno == EPERM);
    return 0;
}
static int ambient(void)
{
    caps_t c;
    CHECK(getcaps(0, &c) == 0);
    c.i |= 1ULL << CAP_CHOWN;
    CHECK(setcaps(c) == 0);
    CHECK(prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, CAP_CHOWN, 0, 0) == 0);
    CHECK(prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_IS_SET, CAP_CHOWN, 0, 0) == 1);
    c.i &= ~(1ULL << CAP_CHOWN);
    CHECK(setcaps(c) == 0);
    CHECK(prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_IS_SET, CAP_CHOWN, 0, 0) == 0);
    errno = 0;
    CHECK(prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, CAP_CHOWN, 0, 0) == -1 && errno == EPERM);
    CHECK(prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0) == 0);
    return 0;
}
static atomic_int stage, worker_tid, worker_error;
static caps_t parent_caps;
static void *grandchild(void *unused)
{
    caps_t c;
    (void)unused;
    if (getcaps(0, &c) != 0 || c.e || c.p || c.i) atomic_store(&worker_error, 128);
    return NULL;
}
static void *worker(void *unused)
{
    caps_t c;
    (void)unused;
    int error = getcaps(0, &c) != 0 || !same(c, parent_caps);
    if (setcaps((caps_t){0}) != 0) error = 1;
    atomic_store(&worker_tid, syscall(SYS_gettid));
    atomic_store(&worker_error, error);
    atomic_store(&stage, 1);
    while (atomic_load(&stage) == 1) sched_yield();
    pthread_t t;
    if (pthread_create(&t, NULL, grandchild, NULL) != 0) error |= 4;
    else if (pthread_join(t, NULL) != 0) error |= 8;
    pid_t child = syscall(SYS_fork);
    if (child == 0) {
        caps_t got;
        int ok = getcaps(0, &got) == 0 && got.e == 0 && got.p == 0 && got.i == 0;
        syscall(SYS_exit, ok ? 0 : 1);
        __builtin_unreachable();
    }
    int status;
    if (child < 0) {
        fprintf(stderr, "worker fork failed errno=%d\n", errno); error |= 16;
    } else if (waitpid(child, &status, 0) != child) {
        fprintf(stderr, "worker wait failed errno=%d\n", errno); error |= 32;
    } else if (!WIFEXITED(status) || WEXITSTATUS(status)) {
        fprintf(stderr, "worker child status=%#x\n", status); error |= 64;
    }
    if (error) atomic_store(&worker_error, error);
    return NULL;
}
static int threads(void)
{
    pthread_t t;
    caps_t got;
    CHECK(getcaps(0, &parent_caps) == 0);
    CHECK(pthread_create(&t, NULL, worker, NULL) == 0);
    while (atomic_load(&stage) == 0) sched_yield();
    CHECK(atomic_load(&worker_error) == 0);
    CHECK(getcaps(0, &got) == 0 && same(got, parent_caps));
    CHECK(getcaps(atomic_load(&worker_tid), &got) == 0 && got.e == 0 && got.p == 0 && got.i == 0);
    atomic_store(&stage, 2);
    CHECK(pthread_join(t, NULL) == 0);
    if (atomic_load(&worker_error)) fprintf(stderr, "worker error bits=%d\n", atomic_load(&worker_error));
    CHECK(atomic_load(&worker_error) == 0);
    CHECK(getcaps(0, &got) == 0 && same(got, parent_caps));
    return 0;
}
static int native_entry(void)
{
    /* Solaris getpid via int 0x91 must never refresh the scoped credential. */
    register long nr __asm__("rax") = 20;
    __asm__ volatile("int $0x91" : "+a"(nr) : : "rcx", "r11", "memory");
    return 1;
}
static int interim_exec(void)
{
    char *const argv[] = { "/bin/true", NULL };
    errno = 0;
    CHECK(syscall(SYS_execve, argv[0], argv, NULL) == -1 && errno == EOPNOTSUPP);
    errno = 0;
    CHECK(syscall(SYS_setresuid, 0, 0, 0) == -1 && errno == EOPNOTSUPP);
    return 0;
}
static void run(const char *name, int (*fn)(void), int sig)
{
    int status;
    fflush(NULL);
    pid_t p = fork();
    if (p == 0) _exit(fn());
    int ok = p > 0 && waitpid(p, &status, 0) == p &&
        (sig ? WIFSIGNALED(status) && WTERMSIG(status) == sig :
        WIFEXITED(status) && WEXITSTATUS(status) == 0);
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    cases++; if (!ok) failures++;
}
int main(int argc, char **argv)
{
    lx = argc == 1;
    if (argc > 1 && strcmp(argv[1], "--linux") != 0) return 2;
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (lx) {
        printf("SCOPE_READY_PID=%d\n", getpid());
        if (kill(getpid(), SIGSTOP) != 0) return 2;
    }
    run("capset atomic transitions", basic, 0);
    run("bounding drop and permissions", bounding, 0);
    run("securebits locks", securebits, 0);
    run("ambient transitions", ambient, 0);
    run("per-task caps, clone, and multithreaded fork", threads, 0);
    if (lx) {
        run("native entry denied without a seccomp filter", native_entry, SIGSYS);
        run("unfinished exec and ID transitions reject", interim_exec, 0);
    }
    printf("SCOPE_CAPS cases=%d failures=%d\n", cases, failures);
    return failures != 0;
}
