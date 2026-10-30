/* CDDL version 1.0, http://www.illumos.org/license/CDDL. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
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

static const char *base = "/test/mnt-proc";
static int report = STDOUT_FILENO;
static int failures;

static void
check(int ok, const char *name)
{
    if (!ok) {
        dprintf(report, "MP_FAIL name=%s errno=%d\n", name, errno);
        failures++;
    }
}

static const char *
path(const char *suffix, char *buffer)
{
    if (snprintf(buffer, PATH_MAX, "%s/%s", base, suffix) >= PATH_MAX)
        _exit(99);
    return buffer;
}

static int
contents(const char *name, const char *expected)
{
    char buffer[64] = { 0 };
    int fd = open(name, O_RDONLY);
    ssize_t n;

    if (fd < 0)
        return 0;
    n = read(fd, buffer, sizeof (buffer) - 1);
    close(fd);
    return n == (ssize_t)strlen(expected) && !strcmp(buffer, expected);
}

static int
guard(void)
{
    struct sock_filter code[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_getppid, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EACCES),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)
    };
    struct sock_fprog fp = { sizeof (code) / sizeof (code[0]), code };

    return prctl(PR_SET_NO_NEW_PRIVS, 1UL, 0UL, 0UL, 0UL) ||
        syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &fp);
}

static void
outside(void)
{
    char p[PATH_MAX];
    int fd;

    check(contents(path("covered/original", p), "original\n"),
        "outside-original");
    errno = 0;
    check(access(path("covered/private", p), F_OK) == -1 && errno == ENOENT,
        "outside-private-absent");
    check(contents(path("bind-target", p), "target\n"), "outside-bind-target");
    fd = open(path("bind-target", p), O_WRONLY);
    check(fd >= 0, "outside-bind-writable");
    if (fd >= 0)
        close(fd);
}

static void
inside(void)
{
    char p[PATH_MAX], cwd[PATH_MAX], expected[PATH_MAX];
    int fd;
    long n;

    path("cwd", expected);
    check(getcwd(cwd, sizeof (cwd)) != NULL && !strcmp(cwd, expected),
        "private-cwd");
    n = syscall(SYS_getcwd, cwd, sizeof (cwd));
    check(n == (long)strlen(expected) + 1 && !strcmp(cwd, expected),
        "private-raw-getcwd");
    fd = open("../covered/private", O_RDONLY);
    check(fd >= 0, "private-relative-file");
    if (fd >= 0)
        close(fd);
    errno = 0;
    check(access(path("covered/original", p), F_OK) == -1 && errno == ENOENT,
        "private-covered-original");
    check(contents(path("bind-target", p), "source\n"), "private-file-bind");
    errno = 0;
    fd = open(path("bind-target", p), O_WRONLY);
    check(fd == -1 && errno == EROFS, "private-readonly-bind");
    if (fd >= 0)
        close(fd);
    check(chdir("/") == 0 && chdir("..") == 0 &&
        getcwd(cwd, sizeof (cwd)) != NULL && !strcmp(cwd, "/"),
        "private-root-boundary");
    check(chdir(expected) == 0, "private-restore-cwd");
    errno = 0;
    check(syscall(SYS_getppid) == -1 && errno == EACCES &&
        prctl(PR_GET_SECCOMP, 0UL, 0UL, 0UL, 0UL) == 2,
        "retained-filter");
    errno = 0;
    check(execl("/does-not-exist", "missing", (char *)NULL) == -1 &&
        errno == ENOENT, "failed-exec-preserves-view");
}

static int
linux_setup(uid_t owner)
{
    char covered[PATH_MAX], source[PATH_MAX], target[PATH_MAX], p[PATH_MAX];
    int fd;

    path("covered", covered);
    path("source", source);
    path("bind-target", target);
    if (unshare(CLONE_NEWNS) ||
        mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) ||
        mount("tmpfs", covered, "tmpfs", MS_NOSUID | MS_NODEV, "mode=0700") ||
        chown(covered, owner, owner))
        return -1;
    fd = open(path("covered/private", p), O_CREAT | O_WRONLY, 0600);
    if (fd < 0)
        return -1;
    if (fchown(fd, owner, owner)) {
        close(fd);
        return -1;
    }
    close(fd);
    return mount(source, target, NULL, MS_BIND, NULL) ||
        mount(NULL, target, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY, NULL);
}

int
main(int argc, char **argv)
{
    int i, drop = 0, linux_control = 0, unrestricted = 0, outside_mode = 0;
    int child_mode = 0, result = -1, status;
    char p[PATH_MAX], fdarg[32];
    pid_t child;
    siginfo_t info;

    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(45);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--base") && i + 1 < argc)
            base = argv[++i];
        else if (!strcmp(argv[i], "--drop"))
            drop = 1;
        else if (!strcmp(argv[i], "--linux"))
            linux_control = 1;
        else if (!strcmp(argv[i], "--unrestricted"))
            unrestricted = 1;
        else if (!strcmp(argv[i], "--outside"))
            outside_mode = 1;
        else if ((!strcmp(argv[i], "--inherited") ||
            !strcmp(argv[i], "--exit-child")) && i + 1 < argc) {
            child_mode = !strcmp(argv[i], "--inherited") ? 2 : 1;
            result = atoi(argv[++i]);
        } else
            return 2;
    }
    if (child_mode) {
        report = result;
        check(setsid() == getpid(), "child-private-session");
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        inside();
        if (child_mode == 1) {
            dprintf(report, "MP_CHILD_SIGNAL_READY uid=%ld failures=%d\n",
                (long)getuid(), failures);
            raise(SIGKILL);
            return 92;
        }
        if (child_mode == 2) {
            dprintf(report, "MP_INHERIT_READY pid=%ld\n", (long)getpid());
            raise(SIGSTOP);
            inside();
        }
        dprintf(report, "MP_CHILD_PASS mode=%d uid=%ld failures=%d\n",
            child_mode, (long)getuid(), failures);
        return failures != 0;
    }
    if (linux_control && linux_setup(drop ? 65534 : 0)) {
        perror("linux private mounts");
        return 2;
    }
    if (drop && (setgroups(0, NULL) || setgid(65534) || setuid(65534)))
        return 2;
    if (outside_mode) {
        outside();
        printf("MP_OUTSIDE failures=%d uid=%ld\n", failures, (long)getuid());
        return failures != 0;
    }
    if (chdir(path("cwd", p)) || getcwd(p, sizeof (p)) == NULL)
        return 2;
    result = open(path("result", p), O_WRONLY | O_TRUNC);
    if (result < 3 || (!unrestricted && guard()))
        return 2;
    if (!linux_control) {
        printf("MP_READY_PID=%ld\n", (long)getpid());
        raise(SIGSTOP);
    }
    if (unrestricted) {
        outside();
        return failures != 0;
    }
    inside();
    snprintf(fdarg, sizeof (fdarg), "%d", result);
    child = fork();
    if (child == 0) {
        execl(argv[0], argv[0], "--base", base, "--exit-child", fdarg,
            (char *)NULL);
        _exit(90);
    }
    memset(&info, 0, sizeof (info));
    check(child > 0 && waitid(P_PID, child, &info, WEXITED | WNOWAIT) == 0 &&
        info.si_pid == child && info.si_code == CLD_KILLED &&
        info.si_status == SIGKILL,
        "wait-without-reaping");
    printf("MP_ZOMBIE_PID=%ld\n", (long)child);
    raise(SIGSTOP);
    check(waitpid(child, &status, 0) == child && WIFSIGNALED(status) &&
        WTERMSIG(status) == SIGKILL, "reap-killed-child");
    child = fork();
    if (child == 0) {
        execl(argv[0], argv[0], "--base", base, "--inherited", fdarg,
            (char *)NULL);
        _exit(91);
    }
    check(child > 0 && waitpid(child, &status, WUNTRACED) == child &&
        WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP, "orphan-child-stop");
    printf("MP_ORPHAN_PID=%ld\n", (long)child);
    printf("MP_PARENT failures=%d uid=%ld\n", failures, (long)getuid());
    return failures != 0;
}
