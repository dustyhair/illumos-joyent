/* CDDL version 1.0, http://www.illumos.org/license/CDDL. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <linux/capability.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
static char base[PATH_MAX];

static void check(int ok, const char *name)
{
    if (!ok) {
        printf("MNR_FAIL name=%s errno=%d\n", name, errno);
        failures++;
    }
}

static const char *path(const char *name, char *out)
{
    if (snprintf(out, PATH_MAX, "%s/%s", base, name) >= PATH_MAX)
        _exit(98);
    return out;
}

static int contents(const char *name, const char *want)
{
    char data[64] = { 0 };
    int fd = open(name, O_RDONLY);
    ssize_t n;
    if (fd < 0) return 0;
    n = read(fd, data, sizeof(data) - 1);
    close(fd);
    return n == (ssize_t)strlen(want) && !strcmp(data, want);
}

static int write_text(const char *name, const char *text, int create)
{
    int fd = open(name, O_WRONLY | (create ? O_CREAT | O_TRUNC : 0), 0644);
    ssize_t n;
    if (fd < 0) return -1;
    n = write(fd, text, strlen(text));
    close(fd);
    return n == (ssize_t)strlen(text) ? 0 : -1;
}

static int maps(uid_t uid, gid_t gid)
{
    char text[96];
    if (write_text("/proc/self/setgroups", "deny\n", 0)) return -1;
    snprintf(text, sizeof(text), "0 %lu 1\n", (unsigned long)uid);
    if (write_text("/proc/self/uid_map", text, 0)) return -1;
    snprintf(text, sizeof(text), "0 %lu 1\n", (unsigned long)gid);
    return write_text("/proc/self/gid_map", text, 0);
}

static int filter(void)
{
    struct sock_filter code[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_getppid, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EACCES),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)
    };
    struct sock_fprog fp = { sizeof(code) / sizeof(code[0]), code };
    return prctl(PR_SET_NO_NEW_PRIVS, 1UL, 0UL, 0UL, 0UL) ||
        syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &fp);
}

static void bind_path(const char *from, const char *to, unsigned long flags,
    int want, const char *what)
{
    int rv;
    errno = 0;
    rv = mount(from, to, NULL, flags, NULL);
    check(want ? rv == -1 && errno == want : rv == 0, what);
}

static int read_at(int dir, const char *name, char *data, size_t size)
{
    int fd = openat(dir, name, O_RDONLY);
    ssize_t n;
    if (fd < 0) return -1;
    n = read(fd, data, size - 1);
    close(fd);
    if (n < 0) return -1;
    data[n] = 0;
    return 0;
}

static void setup(void)
{
    const char *dirs[] = { "src", "src/sub", "src/sub/deep", "a", "a/deep",
        "b", "b/deep", "c", "c/deep", "plain", "rec", "rec/sub",
        "rec/sub/deep", "rec2", "top" };
    char p[PATH_MAX];
    size_t i;
    for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
        if (mkdir(path(dirs[i], p), 0755)) _exit(2);
    if (write_text(path("src/root-marker", p), "new\n", 1) ||
        write_text(path("src/sub/file", p), "base\n", 1) ||
        write_text(path("src/sub/deep/file", p), "base-deep\n", 1) ||
        write_text(path("a/file", p), "A\n", 1) ||
        write_text(path("a/deep/file", p), "Adeep\n", 1) ||
        write_text(path("b/file", p), "B\n", 1) ||
        write_text(path("b/deep/file", p), "Bdeep\n", 1) ||
        write_text(path("c/file", p), "C\n", 1) ||
        write_text(path("c/deep/file", p), "Cdeep\n", 1) ||
        write_text(path("rec/root-marker", p), "old\n", 1) ||
        write_text(path("rec/sub/deep/file", p), "old\n", 1) ||
        write_text(path("regular", p), "regular\n", 1) ||
        write_text(path("fifo-target", p), "outside\n", 1) ||
        mkfifo(path("fifo", p), 0600)) _exit(2);
    for (i = 0; i < 6; i++) {
        char name[32];
        snprintf(name, sizeof(name), "dev-%zu", i);
        if (write_text(path(name, p), "outside\n", 1)) _exit(2);
    }
}

static void outside_check(void)
{
    char p[PATH_MAX];
    check(contents(path("src/sub/file", p), "base\n"), "outside-source");
    check(contents(path("rec/root-marker", p), "old\n"), "outside-root");
    check(contents(path("rec/sub/deep/file", p), "old\n"), "outside-subtree");
    check(contents(path("fifo-target", p), "outside\n"), "outside-fifo");
    check(contents(path("dev-0", p), "outside\n"), "outside-device");
}

static void types(void)
{
    const char *devices[] = { "/dev/null", "/dev/zero", "/dev/full",
        "/dev/random", "/dev/urandom", "/dev/tty" };
    char source[PATH_MAX], target[PATH_MAX], name[32], data[16];
    struct stat st, original;
    size_t i;
    int fd;
    for (i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
        snprintf(name, sizeof(name), "dev-%zu", i);
        path(name, target);
        check(stat(devices[i], &original) == 0 && S_ISCHR(original.st_mode),
            "source-device");
        bind_path(devices[i], target, MS_BIND | (i % 2 ? MS_REC : 0), 0,
            "device-over-file");
        check(stat(target, &st) == 0 && S_ISCHR(st.st_mode) &&
            st.st_rdev == original.st_rdev, "device-identity");
        if (i == 5) continue;
        fd = open(target, O_RDWR | O_NONBLOCK);
        check(fd >= 0, "bound-device-open");
        if (fd < 0) continue;
        if (i == 0) {
            check(write(fd, "x", 1) == 1 && read(fd, data, 1) == 0,
                "bound-null-io");
        } else if (i == 1) {
            memset(data, 1, sizeof(data));
            check(read(fd, data, sizeof(data)) == (ssize_t)sizeof(data), "bound-zero-read");
            check(!memcmp(data, (char[16]){0}, sizeof(data)), "bound-zero-data");
        } else if (i == 2) {
            errno = 0;
            check(write(fd, "x", 1) == -1 && errno == ENOSPC, "bound-full-io");
        } else {
            check(read(fd, data, 1) == 1, "bound-random-read");
        }
        close(fd);
    }
    path("regular", source);
    bind_path(source, path("dev-0", target), MS_BIND | MS_REC, 0,
        "file-over-device");
    check(contents(target, "regular\n"), "file-over-device-contents");
    path("fifo", source);
    path("fifo-target", target);
    bind_path(source, target, MS_BIND | MS_REC, 0, "fifo-over-file");
    check(stat(target, &st) == 0 && S_ISFIFO(st.st_mode), "fifo-type");
    fd = open(target, O_RDWR | O_NONBLOCK);
    check(fd >= 0, "bound-fifo-open");
    if (fd >= 0) {
        memset(data, 0, sizeof(data));
        check(write(fd, "fifo", 4) == 4 && read(fd, data, 4) == 4 &&
            !strcmp(data, "fifo"), "bound-fifo-io");
        close(fd);
    }
    path("regular", source);
    bind_path(source, target, MS_BIND, 0, "file-over-fifo");
    check(contents(target, "regular\n"), "file-over-fifo-contents");
}

static void observer(int ready)
{
    char p[PATH_MAX], root[32], sub[32];
    int fd, old = 0, current = 0, loops;
    int notified = 0;
    for (loops = 0; loops < 200000; loops++) {
        fd = open(path("rec", p), O_PATH | O_DIRECTORY);
        if (fd < 0 || read_at(fd, "root-marker", root, sizeof(root)) ||
            read_at(fd, "sub/deep/file", sub, sizeof(sub))) _exit(92);
        close(fd);
        if (!strcmp(root, "old\n") && !strcmp(sub, "old\n")) old++;
        else if (!strcmp(root, "new\n") && !strcmp(sub, "B\n")) current++;
        else {
            printf("MNR_PARTIAL root=%s sub=%s\n", root, sub);
            _exit(93);
        }
        if (old && !notified) {
            if (write(ready, "r", 1) != 1) _exit(94);
            close(ready);
            notified = 1;
        }
        if (current && old) {
            printf("MNR_OBSERVER_PASS old=%d new=%d\n", old, current);
            _exit(0);
        }
    }
    _exit(95);
}

static void recursive(void)
{
    char source[PATH_MAX], target[PATH_MAX], p[PATH_MAX], token;
    int ready[2], status;
    pid_t child;
    path("a", source);
    bind_path(source, path("src/sub", target), MS_BIND, 0, "source-submount");
    path("b", source);
    bind_path(source, path("src/sub/deep", target), MS_BIND, 0,
        "source-nested-submount");
    path("src", source);
    bind_path(source, path("plain", target), MS_BIND, 0, "plain-directory");
    check(contents(path("plain/sub/file", p), "base\n"), "plain-omits-submount");
    check(contents(path("plain/sub/deep/file", p), "base-deep\n"),
        "plain-omits-nested-submount");
    check(pipe(ready) == 0, "observer-pipe");
    child = fork();
    if (child == 0) {
        close(ready[0]);
        observer(ready[1]);
    }
    close(ready[1]);
    if (child < 0) { check(0, "observer-fork"); close(ready[0]); return; }
    check(read(ready[0], &token, 1) == 1, "observer-ready");
    close(ready[0]);
    path("src", source);
    bind_path(source, path("rec", target),
        0xc0ed0000UL | MS_BIND | MS_REC | MS_SILENT, 0, "recursive-tree");
    check(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0, "atomic-observer");
    check(contents(path("rec/sub/file", p), "A\n"), "recursive-submount");
    check(contents(path("rec/sub/deep/file", p), "B\n"), "recursive-nested");
    path("c", source);
    bind_path(source, path("rec/sub/deep", target), MS_BIND, 0, "copy-mutation");
    check(contents(path("src/sub/deep/file", p), "B\n"), "source-not-mutated");
    check(contents(path("rec/sub/deep/file", p), "C\n"), "copy-mutated");
    path("rec", source);
    bind_path(source, path("rec2", target), MS_BIND | MS_REC, 0,
        "recursive-stacked-copy");
    check(contents(path("rec2/sub/deep/file", p), "C\n"), "copy-stack-visible");
    /* Selecting the top mount must exclude the hidden older mount's children. */
    path("c", source);
    bind_path(source, path("src/sub", target), MS_BIND, 0, "source-overmount");
    path("src/sub", source);
    bind_path(source, path("top", target), MS_BIND | MS_REC, 0,
        "recursive-selected-top");
    check(contents(path("top/deep/file", p), "Cdeep\n"), "hidden-child-excluded");
    check(unshare(CLONE_NEWNS) == 0, "copy-recursive-namespace");
    check(contents(path("rec/sub/deep/file", p), "C\n"), "namespace-copy-recursive");
    check(contents(path("rec2/sub/deep/file", p), "C\n"), "namespace-copy-stack");
    check(contents(path("top/deep/file", p), "Cdeep\n"), "namespace-copy-top");
}

int main(int argc, char **argv)
{
    int i, drop = 0, linux_control = 0, type_mode = 0, deny = 0;
    int pipes[2], status;
    pid_t child;
    uid_t uid;
    gid_t gid;
    char p[PATH_MAX], q[PATH_MAX], token;
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(180);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--drop")) drop = 1;
        else if (!strcmp(argv[i], "--linux")) linux_control = 1;
        else if (!strcmp(argv[i], "--types")) type_mode = 1;
        else if (!strcmp(argv[i], "--deny")) deny = 1;
        else return 2;
    }
    if (drop && (setgroups(0, NULL) || setgid(65534) || setuid(65534))) return 2;
    uid = getuid(); gid = getgid();
    if (prctl(PR_SET_DUMPABLE, 1UL, 0UL, 0UL, 0UL)) return 2;
    snprintf(base, sizeof(base), "/test/mnt-recursive/run.XXXXXX");
    if (!mkdtemp(base)) return 2;
    setup();
    if (pipe(pipes)) return 2;
    child = fork();
    if (child == 0) {
        close(pipes[1]);
        if (read(pipes[0], &token, 1) != 1) _exit(90);
        outside_check();
        printf("MNR_OUTSIDE_PASS failures=%d\n", failures);
        _exit(failures != 0);
    }
    close(pipes[0]);
    if (child < 0 || unshare(CLONE_NEWUSER | CLONE_NEWNS) || maps(uid, gid) ||
        filter()) { perror("setup"); return 2; }
    if (linux_control && mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL))
        return 2;
    if (deny) {
        struct __user_cap_header_struct h = { .version = _LINUX_CAPABILITY_VERSION_3 };
        struct __user_cap_data_struct caps[2] = { 0 };
        if (syscall(SYS_capget, &h, caps)) return 2;
        caps[0].effective &= ~(1U << CAP_SYS_ADMIN);
        if (syscall(SYS_capset, &h, caps)) return 2;
        bind_path(path("src", p), path("rec", q), MS_BIND | MS_REC, EPERM,
            "recursive-authority");
        bind_path("/dev/null", path("dev-0", p), MS_BIND | MS_REC, EPERM,
            "device-authority");
    } else if (type_mode) types();
    else recursive();
    errno = 0;
    check(syscall(SYS_getppid) == -1 && errno == EACCES, "retained-filter");
    check(write(pipes[1], "x", 1) == 1, "outside-release");
    close(pipes[1]);
    check(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0, "outside-namespace-unchanged");
    printf("MNR_PASS uid=%lu types=%d denied=%d failures=%d base=%s\n",
        (unsigned long)uid, type_mode, deny, failures, base);
    return failures != 0;
}

