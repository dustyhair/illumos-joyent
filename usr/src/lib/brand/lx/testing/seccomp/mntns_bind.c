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
        printf("MNB_FAIL name=%s errno=%d\n", name, errno);
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

static void expect_mount(const char *source, const char *target,
    unsigned long flags, int want, const char *name)
{
    int rv;
    errno = 0;
    /* Initial bind ignores optional contents after copying user arguments. */
    rv = syscall(SYS_mount, source, target, NULL, flags, NULL);
    check(want ? rv == -1 && errno == want : rv == 0, name);
}

static void private_check(void)
{
    char p[PATH_MAX];
    check(contents(path("target", p), "source\n"), "private-file");
    check(contents(path("dir-target/file", p), "directory\n"), "private-dir");
    check(contents(path("fd-target", p), "source\n"), "private-fd-target");
    check(contents(path("link-target", p), "source\n"), "private-link-target");
    errno = 0;
    check(syscall(SYS_getppid) == -1 && errno == EACCES,
        "retained-seccomp");
}

int main(int argc, char **argv)
{
    char source[PATH_MAX], target[PATH_MAX], p[PATH_MAX];
    char data[64] = { 0 }, fdpath[64];
    int i, drop = 0, linux_control = 0, child_mode = 0, mount_only = 0;
    int denied = 0, fill = 0, pipes[2], held, targetfd, status;
    uid_t uid;
    gid_t gid;
    pid_t outside, exec_child;
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(180);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--drop")) drop = 1;
        else if (!strcmp(argv[i], "--linux")) linux_control = 1;
        else if (!strcmp(argv[i], "--mount-only")) mount_only = 1;
        else if (!strcmp(argv[i], "--drop-admin")) denied = 1;
        else if (!strcmp(argv[i], "--fill")) fill = 1;
        else if (!strcmp(argv[i], "--exec") && i + 1 < argc) {
            child_mode = 1;
            snprintf(base, sizeof(base), "%s", argv[++i]);
        } else return 2;
    }
    if (child_mode) {
        private_check();
        printf("MNB_EXEC_PASS failures=%d\n", failures);
        return failures != 0;
    }
    if (drop && (setgroups(0, NULL) || setgid(65534) || setuid(65534)))
        return 2;
    uid = getuid();
    gid = getgid();
    if (prctl(PR_SET_DUMPABLE, 1UL, 0UL, 0UL, 0UL)) return 2;
    snprintf(base, sizeof(base), "/test/mnt-bind/run.XXXXXX");
    if (!mkdtemp(base)) { perror("mkdtemp"); return 2; }
    if (write_text(path("source", p), "source\n", 1) ||
        write_text(path("target", p), "target\n", 1) ||
        write_text(path("fd-target", p), "fd-old\n", 1) ||
        write_text(path("link-target", p), "link-old\n", 1) ||
        mkdir(path("dir-source", p), 0755) ||
        mkdir(path("dir-target", p), 0755) ||
        write_text(path("dir-source/file", p), "directory\n", 1) ||
        write_text(path("dir-target/file", p), "outside\n", 1) ||
        symlink("link-target", path("target-link", p)) ||
        pipe(pipes)) return 2;
    held = open(path("target", p), O_RDONLY);
    targetfd = open(path("fd-target", p), O_PATH);
    if (held < 0 || targetfd < 0) return 2;
    outside = fork();
    if (outside < 0) return 2;
    if (outside == 0) {
        close(pipes[1]);
        if (read(pipes[0], data, 1) != 1) _exit(92);
        check(contents(path("target", p), "target\n"), "outside-file");
        check(contents(path("dir-target/file", p), "outside\n"), "outside-dir");
        check(contents(path("fd-target", p), "fd-old\n"), "outside-fd");
        check(contents(path("link-target", p), "link-old\n"), "outside-link");
        printf("MNB_OUTSIDE_PASS failures=%d\n", failures);
        _exit(failures != 0);
    }
    close(pipes[0]);
    if (unshare(mount_only ? CLONE_NEWNS : CLONE_NEWUSER | CLONE_NEWNS) ||
        (!mount_only && maps(uid, gid)) || filter()) {
        perror("namespace setup"); return 2;
    }
    if (linux_control && mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL))
        return 2;
    if (denied) {
        struct __user_cap_header_struct h = {
            .version = _LINUX_CAPABILITY_VERSION_3
        };
        struct __user_cap_data_struct caps[2] = { 0 };
        if (syscall(SYS_capget, &h, caps)) return 2;
        caps[0].effective &= ~(1U << CAP_SYS_ADMIN);
        if (syscall(SYS_capset, &h, caps)) return 2;
    }
    path("source", source);
    path("target", target);
    if (!denied) {
        snprintf(fdpath, sizeof(fdpath), "/proc/self/fd/%d", targetfd);
        expect_mount(source, fdpath, MS_BIND, EINVAL, "foreign-fd-target");
        close(targetfd);
        targetfd = open(path("fd-target", p), O_PATH);
        if (targetfd < 0) return 2;
    }
    expect_mount(source, target, MS_BIND, denied ? EPERM : 0, "bind-file");
    if (denied) {
        check(contents(target, "target\n"), "denied-file-unchanged");
        expect_mount((void *)1, (void *)1, MS_BIND, EFAULT, "bad-pointer-before-authority");
        goto finish;
    }
    errno = 0;
    check(syscall(SYS_mount, source, target, (void *)1, MS_BIND, NULL) == -1 &&
        errno == EFAULT, "bad-ignored-type");
    errno = 0;
    check(syscall(SYS_mount, source, target, NULL, MS_BIND, (void *)1) == -1 &&
        errno == EFAULT, "bad-ignored-data");
    check(syscall(SYS_mount, source, target, "ignored", MS_BIND, "ignored") == 0,
        "valid-ignored-type-data");
    /* The pre-existing file descriptor must keep the covered object. */
    check(read(held, data, sizeof(data) - 1) == 7 && !strcmp(data, "target\n"),
        "held-covered-file");
    check(contents(target, "source\n"), "bound-file-contents");
    check(chdir(base) == 0, "relative-cwd");
    expect_mount("dir-source", "dir-target", MS_BIND, 0, "relative-dir-bind");
    snprintf(fdpath, sizeof(fdpath), "/proc/self/fd/%d", targetfd);
    expect_mount(source, fdpath, MS_BIND, 0, "fd-bind");
    expect_mount(source, path("target-link", p), MS_BIND, 0, "symlink-bind");
    /* Initial bind ignores policy flags; read-only needs a later remount. */
    expect_mount(source, target, MS_BIND | MS_RDONLY | MS_NOSUID |
        MS_NODEV | MS_NOEXEC, 0, "initial-policy-flags");
    i = open(target, O_WRONLY);
    check(i >= 0, "initial-readonly-ignored");
    if (i >= 0) close(i);
    expect_mount(source, path("dir-source", p), MS_BIND, ENOTDIR,
        "file-over-directory");
    expect_mount(path("dir-source", p), target, MS_BIND, ENOTDIR,
        "directory-over-file");
    expect_mount("/does-not-exist", target, MS_BIND, ENOENT, "missing-source");
    expect_mount(source, "/does-not-exist", MS_BIND, ENOENT, "missing-target");
    expect_mount((void *)1, target, MS_BIND, EFAULT, "bad-source");
    expect_mount(source, (void *)1, MS_BIND, EFAULT, "bad-target");
    if (drop) {
        expect_mount("/test/mnt-bind/denied/source", target, MS_BIND,
            EACCES, "actor-source-search");
        expect_mount(source, "/test/mnt-bind/denied/target", MS_BIND,
            EACCES, "actor-target-search");
    }
    private_check();
    /* Copying the view must replay the mounts added through Linux mount. */
    check(unshare(CLONE_NEWNS) == 0, "copy-mutated-namespace");
    private_check();
    exec_child = fork();
    if (exec_child == 0) {
        execl(argv[0], argv[0], "--exec", base, (char *)NULL);
        _exit(93);
    }
    check(exec_child > 0 && waitpid(exec_child, &status, 0) == exec_child &&
        WIFEXITED(status) && WEXITSTATUS(status) == 0, "fork-exec-inheritance");
    if (fill) {
        unsigned n;
        int rv = 0;
        check(mkdir(path("fill", p), 0755) == 0, "fill-directory");
        for (n = 0; n < 4097; n++) {
            snprintf(p, sizeof(p), "fill/%u", n);
            path(p, target);
            if (write_text(target, "uncovered\n", 1)) {
                check(0, "fill-target");
                break;
            }
            errno = 0;
            rv = mount(source, target, NULL, MS_BIND, NULL);
            if (rv != 0) break;
        }
        if (linux_control) {
            check(n == 4097 && rv == 0, "linux-fill-control");
        } else {
            check(n > 0 && n <= 4096 && rv == -1 && errno == ENOSPC,
                "mount-record-limit");
            check(contents(target, "uncovered\n"), "limit-target-unchanged");
            expect_mount(source, target, MS_BIND, ENOSPC,
                "limit-remains-charged");
        }
        printf("MNB_FILL mounts=%u failures=%d\n", n, failures);
    }
finish:
    close(held);
    close(targetfd);
    check(write(pipes[1], "x", 1) == 1, "release-outside");
    close(pipes[1]);
    check(waitpid(outside, &status, 0) == outside && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0, "outside-namespace-unchanged");
    /* The native controller also waits for all retained views to retire. */
    printf("MNB_PASS uid=%lu mount_only=%d denied=%d failures=%d base=%s\n",
        (unsigned long)uid, mount_only, denied, failures, base);
    return failures != 0;
}
