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
        printf("MNP_FAIL name=%s errno=%d\n", name, errno);
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

static void change(const char *target, unsigned long flags, int want,
    const char *name)
{
    int rv;
    errno = 0;
    rv = mount(NULL, target, NULL, flags, NULL);
    check(want ? rv == -1 && errno == want : rv == 0, name);
}

int main(int argc, char **argv)
{
    int i, drop = 0, denied = 0, linux_control = 0, pipefd[2], status;
    uid_t uid;
    gid_t gid;
    pid_t peer;
    char source[PATH_MAX], target[PATH_MAX], p[PATH_MAX], token;
    const unsigned long modes[] = { MS_PRIVATE, MS_SLAVE,
        MS_PRIVATE | MS_REC, MS_SLAVE | MS_REC | MS_SILENT };
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(60);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--drop")) drop = 1;
        else if (!strcmp(argv[i], "--deny")) denied = 1;
        else if (!strcmp(argv[i], "--linux")) linux_control = 1;
        else return 2;
    }
    if (drop && (setgroups(0, NULL) || setgid(65534) || setuid(65534))) return 2;
    uid = getuid(); gid = getgid();
    if (prctl(PR_SET_DUMPABLE, 1UL, 0UL, 0UL, 0UL)) return 2;
    snprintf(base, sizeof(base), "/test/mnt-private/run.XXXXXX");
    if (!mkdtemp(base) || mkdir(path("source", p), 0755) ||
        mkdir(path("other", p), 0755) || mkdir(path("target", p), 0755) ||
        write_text(path("source/file", p), "source\n", 1) ||
        write_text(path("other/file", p), "other\n", 1) ||
        write_text(path("target/file", p), "original\n", 1)) return 2;
    if (unshare(CLONE_NEWUSER | CLONE_NEWNS) || maps(uid, gid) || filter())
        return 2;
    /* Establish the same all-private initial state on Linux. */
    if (linux_control && mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL))
        return 2;
    if (denied) {
        struct __user_cap_header_struct h = { .version = _LINUX_CAPABILITY_VERSION_3 };
        struct __user_cap_data_struct caps[2] = { 0 };
        if (syscall(SYS_capget, &h, caps)) return 2;
        caps[0].effective &= ~(1U << CAP_SYS_ADMIN);
        if (syscall(SYS_capset, &h, caps)) return 2;
    }
    for (i = 0; i < 4; i++)
        change("/", modes[i], denied ? EPERM : 0, "root-private-slave");
    if (denied) {
        check(contents(path("target/file", p), "original\n"),
            "denied-unchanged");
        goto out;
    }
    change(path("target", target), MS_PRIVATE | MS_REC, EINVAL,
        "nonmountpoint");
    change("/", MS_PRIVATE | MS_SLAVE, EINVAL, "multiple-types");
    change("/does-not-exist", MS_PRIVATE, ENOENT, "missing-target");
    errno = 0;
    check(syscall(SYS_mount, (void *)1, "/", NULL, MS_PRIVATE, NULL) == -1 &&
        errno == EFAULT, "bad-ignored-source");
    check(mount("ignored", "/", "ignored", MS_SLAVE | MS_REC,
        "ignored") == 0, "ignored-optional-contents");
    path("source", source);
    path("target", target);
    check(mount(source, target, NULL, MS_BIND, NULL) == 0, "make-mountpoint");
    for (i = 0; i < 4; i++)
        change(target, modes[i], 0, "bind-private-slave");
    check(pipe(pipefd) == 0, "peer-pipe");
    peer = fork();
    if (peer == 0) {
        close(pipefd[1]);
        if (read(pipefd[0], &token, 1) != 1) _exit(90);
        check(contents(path("target/file", p), "source\n"),
            "peer-namespace-not-propagated");
        printf("MNP_PEER_PASS failures=%d\n", failures);
        _exit(failures != 0);
    }
    close(pipefd[0]);
    if (peer < 0) return 2;
    check(unshare(CLONE_NEWNS) == 0, "copy-private-namespace");
    change("/", MS_SLAVE | MS_REC | MS_SILENT, 0, "copied-root-slave");
    path("other", source);
    check(mount(source, target, NULL, MS_BIND, NULL) == 0, "copied-view-mount");
    change(target, MS_PRIVATE | MS_REC, 0, "copied-mount-private");
    check(contents(path("target/file", p), "other\n"), "copied-view-change");
    check(write(pipefd[1], "x", 1) == 1, "peer-release");
    close(pipefd[1]);
    check(waitpid(peer, &status, 0) == peer && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0, "peer-exit");
out:
    errno = 0;
    check(syscall(SYS_getppid) == -1 && errno == EACCES, "retained-seccomp");
    printf("MNP_PASS uid=%lu denied=%d failures=%d base=%s\n",
        (unsigned long)uid, denied, failures, base);
    return failures != 0;
}

