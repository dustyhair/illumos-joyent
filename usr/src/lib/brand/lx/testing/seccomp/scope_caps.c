/* CDDL version 1.0, http://www.illumos.org/license/CDDL. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sys/xattr.h>
#include <linux/capability.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <sys/socket.h>
#include <linux/securebits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct caps { uint64_t e, p, i; } caps_t;
static int failures, cases;
static int lx;
static const char *self_path, *setid_path;
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
    /* A private first write takes a user page fault after the drop. */
    volatile char *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) error |= 2;
    else {
        *page = 1;
        if (getcaps(0, &c) != 0 || c.e || c.p || c.i) error |= 256;
        munmap((void *)page, 4096);
    }
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
        int status = getcaps(0, &got) != 0 ? 32 + errno :
            got.e ? 2 : got.p ? 3 : got.i ? 4 : 0;
        syscall(SYS_exit, status);
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
static uint64_t bounding_set(void)
{
    uint64_t bits = 0;
    for (int cap = 0; cap <= 40; cap++)
        if (prctl(PR_CAPBSET_READ, cap, 0, 0, 0) == 1)
            bits |= 1ULL << cap;
    return bits;
}
static uint64_t ambient_set(void)
{
    uint64_t bits = 0;
    for (int cap = 0; cap <= 40; cap++)
        if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_IS_SET, cap, 0, 0) == 1)
            bits |= 1ULL << cap;
    return bits;
}
static int exec_check(int argc, char **argv)
{
    caps_t got, want;
    CHECK(argc == 9);
    want.e = strtoull(argv[2], NULL, 16);
    want.p = strtoull(argv[3], NULL, 16);
    want.i = strtoull(argv[4], NULL, 16);
    CHECK(getcaps(0, &got) == 0 && same(got, want));
    CHECK(bounding_set() == strtoull(argv[5], NULL, 16));
    CHECK(ambient_set() == strtoull(argv[6], NULL, 16));
    CHECK(prctl(PR_GET_SECUREBITS, 0, 0, 0, 0) == (int)strtoul(argv[7], NULL, 16));
    CHECK(prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == (int)strtoul(argv[8], NULL, 16));
    return 0;
}
static int exec_expect(const char *path, caps_t c, uint64_t b, uint64_t a,
    unsigned secure, unsigned nnp)
{
    uint64_t values[] = { c.e, c.p, c.i, b, a, secure, nnp };
    char text[7][24];
    char *args[10] = { (char *)path, "--exec-check" };
    for (int i = 0; i < 7; i++) {
        snprintf(text[i], sizeof(text[i]), "%llx", (unsigned long long)values[i]);
        args[i + 2] = text[i];
    }
    args[9] = NULL;
    syscall(SYS_execve, path, args, NULL);
    fprintf(stderr, "exec %s failed errno=%d\n", path, errno);
    return 1;
}
static int exec_nnp(void)
{
    uint64_t b = bounding_set();
    CHECK(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0);
    CHECK(setcaps((caps_t){0}) == 0);
    return exec_expect(self_path, (caps_t){0}, b, 0, 0, 1);
}
static int exec_root(void)
{
    uint64_t b = bounding_set();
    int nnp = prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0);
    CHECK(nnp == 0);
    CHECK(setcaps((caps_t){0}) == 0);
    return exec_expect(self_path, (caps_t){b, b, 0}, b, 0, 0, 0);
}
static int exec_bound(void)
{
    CHECK(prctl(PR_CAPBSET_DROP, CAP_NET_RAW, 0, 0, 0) == 0);
    return exec_root();
}
static int exec_ambient(void)
{
    uint64_t b = bounding_set(), bit = 1ULL << CAP_CHOWN;
    CHECK(prctl(PR_SET_SECUREBITS, SECBIT_NOROOT | SECBIT_KEEP_CAPS, 0, 0, 0) == 0);
    CHECK(setcaps((caps_t){bit, bit, bit}) == 0);
    CHECK(prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, CAP_CHOWN, 0, 0) == 0);
    return exec_expect(self_path, (caps_t){bit, bit, bit}, b, bit, SECBIT_NOROOT, 0);
}
static int exec_keep(void)
{
    uint64_t b = bounding_set();
    CHECK(prctl(PR_SET_SECUREBITS, SECBIT_KEEP_CAPS | SECBIT_KEEP_CAPS_LOCKED, 0, 0, 0) == 0);
    CHECK(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0);
    CHECK(setcaps((caps_t){0}) == 0);
    return exec_expect(self_path, (caps_t){0}, b, 0, SECBIT_KEEP_CAPS_LOCKED, 1);
}
static int failed_exec(void)
{
    caps_t c, got;
    uint64_t b = bounding_set(), a;
    CHECK(prctl(PR_SET_SECUREBITS, SECBIT_KEEP_CAPS, 0, 0, 0) == 0);
    CHECK(getcaps(0, &c) == 0);
    c.e = 0; c.i = 1ULL << CAP_CHOWN;
    CHECK(setcaps(c) == 0);
    CHECK(prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, CAP_CHOWN, 0, 0) == 0);
    a = ambient_set();
    char *const args[] = { "/scope-no-such-executable", NULL };
    errno = 0;
    CHECK(syscall(SYS_execve, args[0], args, NULL) == -1 && errno == ENOENT);
    CHECK(getcaps(0, &got) == 0 && same(c, got));
    CHECK(bounding_set() == b && ambient_set() == a);
    CHECK(prctl(PR_GET_SECUREBITS, 0, 0, 0, 0) == SECBIT_KEEP_CAPS);
    return 0;
}
static void *exec_worker(void *unused)
{
    (void)unused;
    return (void *)(uintptr_t)exec_nnp();
}
static int exec_thread(void)
{
    pthread_t t;
    CHECK(pthread_create(&t, NULL, exec_worker, NULL) == 0);
    CHECK(pthread_join(t, NULL) == 0);
    return 1; /* Successful exec replaces this whole process. */
}
static int exec_setid_nnp(void)
{
    uint64_t b = bounding_set(), bit = 1ULL << CAP_CHOWN;
    CHECK(prctl(PR_SET_SECUREBITS, SECBIT_NOROOT, 0, 0, 0) == 0);
    CHECK(setcaps((caps_t){bit, bit, bit}) == 0);
    CHECK(prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, CAP_CHOWN, 0, 0) == 0);
    CHECK(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0);
    return exec_expect(setid_path, (caps_t){bit, bit, bit}, b, bit, SECBIT_NOROOT, 1);
}
static int privileged_exec(void)
{
    caps_t c, got;
    CHECK(getcaps(0, &c) == 0);
    char *args[] = { (char *)setid_path, "--exec-check", NULL };
    errno = 0;
    CHECK(syscall(SYS_execve, args[0], args, NULL) == -1 && errno == EOPNOTSUPP);
    CHECK(getcaps(0, &got) == 0 && same(c, got));
    args[0] = "/native/usr/bin/true";
    errno = 0;
    CHECK(syscall(SYS_execve, args[0], args, NULL) == -1 && errno == ENOEXEC);
    CHECK(getcaps(0, &got) == 0 && same(c, got));
    return 0;
}
static int exec_native_guard(void)
{
    char *const args[] = { (char *)self_path, "--exec-native", NULL };
    CHECK(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0);
    syscall(SYS_execve, self_path, args, NULL);
    return 1;
}
static int codex_image(void)
{
    caps_t c;
    CHECK(getcaps(0, &c) == 0 && c.e == 0 && c.p == 0 && c.i == 0);
    CHECK(prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == 1);
    CHECK(prctl(PR_GET_SECCOMP, 0, 0, 0, 0) == 2);
    char *const args[] = { "/test/codex-current", "--version", NULL };
    syscall(SYS_execve, args[0], args, NULL);
    return 1;
}
static int exec_codex(void)
{
    struct sock_filter code[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_socket, 0, 3),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0])),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_INET, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EACCES),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)
    };
    struct sock_fprog program = { sizeof(code) / sizeof(code[0]), code };
    CHECK(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0);
    CHECK(setcaps((caps_t){0}) == 0);
    CHECK(prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0);
    errno = 0;
    CHECK(socket(AF_INET, SOCK_STREAM, 0) == -1 && errno == EACCES);
    char *const args[] = { (char *)self_path, "--exec-codex", NULL };
    syscall(SYS_execve, self_path, args, NULL);
    return 1;
}
static int file_caps_unsupported(void)
{
    struct vfs_cap_data data = { .magic_etc = VFS_CAP_REVISION_2 | VFS_CAP_FLAGS_EFFECTIVE };
    data.data[0].permitted = 1U << CAP_CHOWN;
    int fd = open(setid_path, O_RDONLY);
    CHECK(fd >= 0);
    errno = 0;
    CHECK(fsetxattr(fd, "security.capability", &data, sizeof(data), 0) == -1 && errno == EOPNOTSUPP);
    CHECK(close(fd) == 0);
    return 0;
}
static int interim_ids(void)
{
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
    if (argc > 1 && strcmp(argv[1], "--exec-check") == 0) return exec_check(argc, argv);
    if (argc == 2 && strcmp(argv[1], "--exec-native") == 0) return native_entry();
    if (argc == 2 && strcmp(argv[1], "--exec-codex") == 0) return codex_image();
    lx = argc == 1;
    if (!lx && (argc != 3 || strcmp(argv[1], "--linux") != 0)) return 2;
    self_path = argv[0];
    setid_path = lx ? "/test/scope-setid/scope_caps" : argv[2];
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
    run("NNP exec preserves dropped capabilities", exec_nnp, 0);
    run("root ordinary exec uses bounding capabilities", exec_root, 0);
    run("bounding drop survives exec", exec_bound, 0);
    run("ambient exec and NOROOT", exec_ambient, 0);
    run("exec clears KEEPCAPS but retains its lock", exec_keep, 0);
    run("failed exec preserves credentials", failed_exec, 0);
    run("weak worker exec ignores process capability union", exec_thread, 0);
    run("NNP ignores setid and preserves ambient caps", exec_setid_nnp, 0);
    if (lx) {
        run("native entry denied without a seccomp filter", native_entry, SIGSYS);
        run("unfinished ID transitions reject", interim_ids, 0);
        run("privileged and native exec reject atomically", privileged_exec, 0);
        run("native entry denied after scoped exec", exec_native_guard, SIGSYS);
        run("Codex exec with scoped zero capabilities and a filter", exec_codex, 0);
        run("unsupported file capabilities do not report success", file_caps_unsupported, 0);
    }
    printf("SCOPE_CAPS cases=%d failures=%d\n", cases, failures);
    return failures != 0;
}
