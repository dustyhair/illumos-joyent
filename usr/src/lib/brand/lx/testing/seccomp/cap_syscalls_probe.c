/* CDDL version 1.0, http://www.illumos.org/license/CDDL. */
#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <linux/capability.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <limits.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "line %d: %s errno=%d\n", __LINE__, #x, errno); \
    return 1; } } while (0)
#define V1 _LINUX_CAPABILITY_VERSION_1
#define V2 _LINUX_CAPABILITY_VERSION_2
#define V3 _LINUX_CAPABILITY_VERSION_3
#define BIND (1U << CAP_NET_BIND_SERVICE)

typedef struct __user_cap_data_struct caps_t[2];

static int
get(caps_t data)
{
    struct __user_cap_header_struct h = { V3, 0 };
    return syscall(SYS_capget, &h, data);
}

static int
set(caps_t data)
{
    struct __user_cap_header_struct h = { V3, 0 };
    return syscall(SYS_capset, &h, data);
}

static int
versions(void)
{
    caps_t expected, actual;
    CHECK(get(expected) == 0);
    uint32_t versions[] = { V1, V2, V3 };
    for (unsigned i = 0; i < 3; i++) {
        struct __user_cap_header_struct h = { versions[i], 0 };
        memset(actual, 0xa5, sizeof actual);
        CHECK(syscall(SYS_capget, &h, actual) == 0);
        CHECK(memcmp(actual, expected, i == 0 ? sizeof actual[0] : sizeof actual) == 0);
        if (i == 0)
            CHECK(actual[1].effective == 0xa5a5a5a5U);
    }
    return 0;
}

static int
negotiation(void)
{
    struct __user_cap_header_struct h = { 0, 12345 };
    caps_t data;
    CHECK(syscall(SYS_capget, &h, NULL) == 0 && h.version == V3 && h.pid == 12345);
    h.version = 0;
    CHECK(syscall(SYS_capget, &h, data) == -1 && errno == EINVAL && h.version == V3);
    h.version = 0;
    CHECK(syscall(SYS_capset, &h, data) == -1 && errno == EINVAL && h.version == V3);
    CHECK(syscall(SYS_capget, NULL, NULL) == -1 && errno == EFAULT);
    return 0;
}

static int
version_boundary(void)
{
    size_t page = sysconf(_SC_PAGESIZE);
    char *area = mmap(NULL, page * 2, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(area != MAP_FAILED && mprotect(area + page, page, PROT_NONE) == 0);
    uint32_t *version = (void *)(area + page - sizeof *version);
    *version = V3;
    CHECK(syscall(SYS_capget, version, NULL) == 0);
    *version = 0;
    CHECK(syscall(SYS_capget, version, NULL) == 0 && *version == V3);
    caps_t data;
    CHECK(syscall(SYS_capget, version, data) == -1 && errno == EFAULT);
    CHECK(munmap(area, page * 2) == 0);
    return 0;
}

static int
pid_errors(void)
{
    caps_t data;
    struct __user_cap_header_struct h = { V3, -1 };
    CHECK(syscall(SYS_capget, &h, NULL) == 0);
    CHECK(syscall(SYS_capget, &h, data) == -1 && errno == EINVAL);
    CHECK(syscall(SYS_capset, &h, NULL) == -1 && errno == EPERM);
    h.pid = INT_MAX;
    CHECK(syscall(SYS_capget, &h, data) == -1 && errno == ESRCH);
    h.pid = 0;
    CHECK(syscall(SYS_capget, &h, (void *)1) == -1 && errno == EFAULT);
    CHECK(syscall(SYS_capset, &h, (void *)1) == -1 && errno == EFAULT);
    return 0;
}

static int
inheritable_only(void)
{
    caps_t original, desired, actual;
    CHECK(get(original) == 0 && (original[0].permitted & BIND) != 0);
    memcpy(desired, original, sizeof desired);
    desired[0].inheritable &= ~BIND;
    CHECK(set(desired) == 0 && get(actual) == 0 &&
        memcmp(actual, desired, sizeof actual) == 0);
    desired[0].inheritable |= BIND;
    CHECK(set(desired) == 0 && get(actual) == 0 &&
        memcmp(actual, desired, sizeof actual) == 0);
    return 0;
}

static int
inheritable_outside_permitted(void)
{
    caps_t desired, actual;
    CHECK(get(desired) == 0 && (desired[0].permitted & BIND) != 0);
    desired[0].inheritable |= BIND;
    CHECK(set(desired) == 0);
    desired[0].effective &= ~BIND;
    desired[0].permitted &= ~BIND;
    CHECK(set(desired) == 0 && get(actual) == 0 &&
        memcmp(actual, desired, sizeof actual) == 0);
    return 0;
}

static int
failed_atomic(void)
{
    caps_t original, desired, actual;
    CHECK(get(original) == 0 && (original[0].effective & BIND) != 0);
    memcpy(desired, original, sizeof desired);
    desired[0].permitted &= ~BIND;
    CHECK(set(desired) == -1 && errno == EPERM);
    CHECK(get(actual) == 0 && memcmp(actual, original, sizeof actual) == 0);
    return 0;
}

static int
unknown_high_bits(void)
{
    caps_t original, desired, actual;
    CHECK(get(original) == 0);
    memcpy(desired, original, sizeof desired);
    desired[1].effective |= 0x80000000U;
    desired[1].permitted |= 0x80000000U;
    desired[1].inheritable |= 0x80000000U;
    CHECK(set(desired) == 0 && get(actual) == 0 &&
        memcmp(actual, original, sizeof actual) == 0);
    return 0;
}

static int
effective_and_zero(void)
{
    caps_t desired, actual;
    CHECK(get(desired) == 0);
    desired[0].effective = desired[1].effective = 0;
    CHECK(set(desired) == 0 && get(actual) == 0 &&
        memcmp(actual, desired, sizeof actual) == 0);
    memset(desired, 0, sizeof desired);
    CHECK(set(desired) == 0 && get(actual) == 0 &&
        memcmp(actual, desired, sizeof actual) == 0);
    return 0;
}

static int
remote_atomic(void)
{
    int ready[2], go[2], finish[2], status;
    char byte;
    CHECK(pipe(ready) == 0 && pipe(go) == 0 && pipe(finish) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        caps_t data;
        close(ready[0]); close(go[1]); close(finish[1]);
        if (get(data) != 0 || (data[0].effective & BIND) == 0)
            _exit(2);
        data[0].inheritable |= BIND;
        if (set(data) != 0 || write(ready[1], "x", 1) != 1 ||
            read(go[0], &byte, 1) != 1)
            _exit(2);
        data[0].effective &= ~BIND;
        data[0].permitted &= ~BIND;
        data[0].inheritable &= ~BIND;
        if (set(data) != 0 || write(ready[1], "x", 1) != 1 ||
            read(finish[0], &byte, 1) != 1)
            _exit(2);
        _exit(0);
    }
    close(ready[1]); close(go[0]); close(finish[0]);
    CHECK(read(ready[0], &byte, 1) == 1);
    struct __user_cap_header_struct h = { V3, child };
    caps_t data;
    CHECK(syscall(SYS_capget, &h, data) == 0 &&
        (data[0].effective & BIND) != 0 &&
        (data[0].permitted & BIND) != 0 &&
        (data[0].inheritable & BIND) != 0);
    CHECK(write(go[1], "x", 1) == 1);
    for (unsigned i = 0; i < 256; i++) {
        CHECK(syscall(SYS_capget, &h, data) == 0);
        unsigned e = !!(data[0].effective & BIND);
        unsigned p = !!(data[0].permitted & BIND);
        unsigned inherited = !!(data[0].inheritable & BIND);
        CHECK(e == p && p == inherited);
    }
    CHECK(read(ready[0], &byte, 1) == 1 &&
        syscall(SYS_capget, &h, data) == 0 &&
        ((data[0].effective | data[0].permitted | data[0].inheritable) & BIND) == 0);
    CHECK(write(finish[1], "x", 1) == 1);
    close(ready[0]); close(go[1]); close(finish[1]);
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0);
    CHECK(syscall(SYS_capget, &h, data) == -1 && errno == ESRCH);
    return 0;
}

int
main(int argc, char **argv)
{
    int (*tests[])(void) = { versions, negotiation, version_boundary, pid_errors,
        inheritable_only, inheritable_outside_permitted, failed_atomic,
        unknown_high_bits, effective_and_zero, remote_atomic };
    unsigned failed = 0;
    int filter = argc == 2 && strcmp(argv[1], "--filter") == 0;
    for (unsigned i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        int status;
        pid_t child = fork();
        if (child == 0) {
            if (filter) {
                struct sock_filter insn[] = { BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW) };
                struct sock_fprog program = { 1, insn };
                if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
                    prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0)
                    _exit(2);
            }
            _exit(tests[i]());
        }
        if (child < 0 || waitpid(child, &status, 0) != child ||
            !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "FAIL capability ABI scenario %u\n", i);
            failed++;
        }
    }
    printf("%zu capability ABI scenarios, %u failed, no skips, filter=%d\n",
        sizeof tests / sizeof tests[0], failed, filter);
    return failed != 0;
}
