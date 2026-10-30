/* CDDL version 1.0, http://www.illumos.org/license/CDDL. */
#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <linux/capability.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "line %d: %s errno=%d\n", __LINE__, #x, errno); \
    return 1; } } while (0)

static ssize_t
read_text(int fd, char *text, size_t size)
{
    if (lseek(fd, 0, SEEK_SET) != 0)
        return -1;
    ssize_t n = read(fd, text, size - 1);
    if (n >= 0)
        text[n] = '\0';
    return n;
}

static int
initial_maps(void)
{
    char text[256], task[128];
    unsigned first, outside, count;
    struct stat a, b;
    int fd = open("/proc/self/uid_map", O_RDWR);
    int other = open("/proc/self/uid_map", O_RDONLY);
    CHECK(fd >= 0 && other >= 0 && fstat(fd, &a) == 0 && fstat(other, &b) == 0 &&
        a.st_ino == b.st_ino && (a.st_mode & 0777) == 0644);
    CHECK(read_text(fd, text, sizeof text) > 0 &&
        sscanf(text, "%u %u %u", &first, &outside, &count) == 3);
    CHECK(count > 0 && lseek(fd, 0, SEEK_SET) == 0);
    CHECK(write(fd, "0 0 1", 5) == -1 && errno == EPERM);
    CHECK(lseek(fd, 0, SEEK_CUR) == 0);
    close(fd); close(other);
    snprintf(task, sizeof task, "/proc/self/task/%ld/uid_map", syscall(SYS_gettid));
    fd = open(task, O_RDONLY);
    CHECK(fd >= 0 && read_text(fd, text, sizeof text) > 0);
    close(fd);
    fd = open("/proc/self/gid_map", O_RDONLY);
    CHECK(fd >= 0 && read_text(fd, text, sizeof text) > 0);
    close(fd);
    return 0;
}

static int
captured_native_credential(void)
{
    char text[32];
    int fd = open("/proc/self/setgroups", O_RDWR);
    CHECK(fd >= 0 && read_text(fd, text, sizeof text) > 0);
    int allowed = strcmp(text, "allow\n") == 0;
    CHECK(allowed || strcmp(text, "deny\n") == 0);
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    struct __user_cap_header_struct h = { _LINUX_CAPABILITY_VERSION_3, 0 };
    struct __user_cap_data_struct d[2] = {{0}};
    CHECK(syscall(SYS_capget, &h, d) == 0 && (d[0].effective & (1U << 21)) != 0);
    d[0].effective &= ~(1U << 21);
    CHECK(syscall(SYS_capset, &h, d) == 0);
    int fresh = open("/proc/self/setgroups", O_WRONLY);
    CHECK(fresh == -1 && errno == EACCES);
    ssize_t result = write(fd, "allow", 5);
    CHECK(allowed ? result == 5 : result == -1 && errno == EPERM);
    close(fd);
    return 0;
}

static int
retained_target(void)
{
    int ready[2], finish[2], status;
    char byte, path[100], before[256], after[256];
    struct stat a, b;
    CHECK(pipe(ready) == 0 && pipe(finish) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        close(ready[0]); close(finish[1]);
        if (write(ready[1], "x", 1) != 1 || read(finish[0], &byte, 1) != 1)
            _exit(2);
        _exit(0);
    }
    close(ready[1]); close(finish[0]);
    CHECK(read(ready[0], &byte, 1) == 1);
    snprintf(path, sizeof path, "/proc/%d/uid_map", child);
    int fd = open(path, O_RDONLY);
    CHECK(fd >= 0 && read_text(fd, before, sizeof before) > 0 && fstat(fd, &a) == 0);
    CHECK(write(finish[1], "x", 1) == 1);
    close(ready[0]); close(finish[1]);
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0);
    CHECK(read_text(fd, after, sizeof after) > 0 && strcmp(before, after) == 0 &&
        fstat(fd, &b) == 0 && a.st_ino == b.st_ino);
    int copy = dup(fd);
    CHECK(copy >= 0 && read_text(copy, after, sizeof after) > 0 &&
        strcmp(before, after) == 0);
    close(copy); close(fd);
    return 0;
}

int
main(void)
{
    int (*tests[])(void) = { initial_maps, captured_native_credential, retained_target };
    unsigned failed = 0;
    for (unsigned i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        int status;
        pid_t child = fork();
        if (child == 0)
            _exit(tests[i]());
        if (child < 0 || waitpid(child, &status, 0) != child ||
            !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "FAIL baseline scenario %u\n", i);
            failed++;
        }
    }
    printf("%zu map-file baseline scenarios, %u failed, no skips\n",
        sizeof tests / sizeof tests[0], failed);
    return failed != 0;
}
