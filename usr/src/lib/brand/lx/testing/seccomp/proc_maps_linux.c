/* CDDL version 1.0, http://www.illumos.org/license/CDDL. */
#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <linux/capability.h>
#include <sched.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <stdlib.h>

#define ALL ((UINT64_C(1) << 41) - 1)
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "line %d: %s errno=%d\n", __LINE__, #x, errno); \
    return 1; } } while (0)

static int
caps(uint64_t e)
{
    struct __user_cap_header_struct h = { _LINUX_CAPABILITY_VERSION_3, 0 };
    struct __user_cap_data_struct d[2] = {
        { (uint32_t)e, (uint32_t)ALL, 0 },
        { e >> 32, ALL >> 32, 0 }
    };
    return syscall(SYS_capset, &h, d);
}

static ssize_t
read_map(int fd, char *text, size_t size)
{
    if (lseek(fd, 0, SEEK_SET) != 0)
        return -1;
    ssize_t n = read(fd, text, size - 1);
    if (n >= 0)
        text[n] = '\0';
    return n;
}

static int
capture_target(void)
{
    char before[256], after[256];
    int old = open("/proc/self/uid_map", O_RDWR);
    CHECK(old >= 0 && read_map(old, before, sizeof before) > 0);
    CHECK(unshare(CLONE_NEWUSER) == 0);
    int fresh = open("/proc/self/uid_map", O_RDWR);
    CHECK(fresh >= 0 && read_map(fresh, after, sizeof after) == 0);
    CHECK(read_map(old, after, sizeof after) > 0 && strcmp(before, after) == 0);
    CHECK(write(fresh, "0 0 1", 5) == 5);
    CHECK(read_map(fresh, after, sizeof after) > 0);
    CHECK(read_map(old, after, sizeof after) > 0 && strcmp(before, after) == 0);
    close(old); close(fresh);
    return 0;
}

static int
weak_opener(void)
{
    CHECK(unshare(CLONE_NEWUSER) == 0 && caps(0) == 0);
    int fd = open("/proc/self/uid_map", O_WRONLY);
    CHECK(fd >= 0 && caps(ALL) == 0);
    CHECK(write(fd, "0 0 1", 5) == -1 && errno == EPERM);
    CHECK(lseek(fd, 0, SEEK_CUR) == 0);
    close(fd);
    return 0;
}

static int
strong_opener(void)
{
    CHECK(unshare(CLONE_NEWUSER) == 0);
    int fd = open("/proc/self/uid_map", O_WRONLY);
    CHECK(fd >= 0 && caps(0) == 0);
    CHECK(write(fd, "0 0 1", 5) == 5);
    CHECK(lseek(fd, 0, SEEK_CUR) == 5);
    CHECK(write(fd, "0 0 1", 5) == -1 && errno == EINVAL);
    close(fd);
    return 0;
}

static int
groups_opener(void)
{
    CHECK(unshare(CLONE_NEWUSER) == 0);
    int fd = open("/proc/self/setgroups", O_WRONLY);
    CHECK(fd >= 0 && caps(0) == 0);
    CHECK(write(fd, "deny", 4) == 4);
    close(fd);
    fd = open("/proc/self/setgroups", O_RDONLY);
    char text[32];
    CHECK(fd >= 0 && read_map(fd, text, sizeof text) == 5 &&
        strcmp(text, "deny\n") == 0);
    close(fd);
    return 0;
}

static int
groups_no_authority(void)
{
    CHECK(unshare(CLONE_NEWUSER) == 0 && caps(0) == 0);
    int fd = open("/proc/self/setgroups", O_WRONLY);
    CHECK(fd == -1 && errno == EACCES);
    fd = open("/proc/self/setgroups", O_RDONLY);
    CHECK(fd >= 0);
    close(fd);
    return 0;
}

static int
after_exit(void)
{
    int ready[2], finish[2], error = 0, status;
    char byte, path[80], text[256];
    CHECK(pipe(ready) == 0 && pipe(finish) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        close(ready[0]); close(finish[1]);
        if (unshare(CLONE_NEWUSER) != 0)
            error = errno;
        if (write(ready[1], &error, sizeof error) != sizeof error ||
            read(finish[0], &byte, 1) != 1)
            _exit(2);
        _exit(error != 0);
    }
    close(ready[1]); close(finish[0]);
    CHECK(read(ready[0], &error, sizeof error) == sizeof error && error == 0);
    snprintf(path, sizeof path, "/proc/%d/uid_map", child);
    int fd = open(path, O_RDWR);
    CHECK(fd >= 0 && write(finish[1], "x", 1) == 1);
    close(ready[0]); close(finish[1]);
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0);
    CHECK(read_map(fd, text, sizeof text) == 0);
    CHECK(write(fd, "0 0 1", 5) == 5);
    CHECK(read_map(fd, text, sizeof text) > 0);
    close(fd);
    return 0;
}

int
main(void)
{
    int (*tests[])(void) = { capture_target, weak_opener, strong_opener,
        groups_opener, groups_no_authority, after_exit };
    unsigned failed = 0;
    for (unsigned i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        int status;
        pid_t child = fork();
        if (child == 0)
            _exit(tests[i]());
        if (child < 0 || waitpid(child, &status, 0) != child ||
            !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "FAIL scenario %u\n", i);
            failed++;
        }
    }
    printf("%zu Linux map-file scenarios, %u failed, no skips\n",
        sizeof tests / sizeof tests[0], failed);
    return failed != 0;
}
