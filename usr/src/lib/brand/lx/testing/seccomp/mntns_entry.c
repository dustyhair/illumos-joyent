/* CDDL version 1.0, http://www.illumos.org/license/CDDL. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/capability.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("MNE_FAIL name=%s errno=%d\n", what, errno);
        failures++;
    }
}
static int read_map(char *data, size_t size)
{
    int fd = open("/proc/self/uid_map", O_RDONLY);
    ssize_t n;
    if (fd < 0) return -1;
    n = read(fd, data, size - 1);
    close(fd);
    if (n < 0) return -1;
    data[n] = 0;
    return 0;
}
static int write_text(const char *path, const char *data)
{
    int fd = open(path, O_WRONLY);
    ssize_t n;
    if (fd < 0) return -1;
    n = write(fd, data, strlen(data));
    close(fd);
    return n == (ssize_t)strlen(data) ? 0 : -1;
}
static int maps(uid_t uid, gid_t gid)
{
    char text[96];
    if (write_text("/proc/self/setgroups", "deny\n")) return -1;
    snprintf(text, sizeof(text), "0 %lu 1\n", (unsigned long)uid);
    if (write_text("/proc/self/uid_map", text)) return -1;
    snprintf(text, sizeof(text), "0 %lu 1\n", (unsigned long)gid);
    return write_text("/proc/self/gid_map", text);
}
static int caps(struct __user_cap_data_struct data[2])
{
    struct __user_cap_header_struct h = { .version = _LINUX_CAPABILITY_VERSION_3 };
    return syscall(SYS_capget, &h, data);
}
static int drop_admin(void)
{
    struct __user_cap_header_struct h = { .version = _LINUX_CAPABILITY_VERSION_3 };
    struct __user_cap_data_struct data[2] = { 0 };
    if (caps(data)) return -1;
    data[0].effective &= ~(1U << CAP_SYS_ADMIN);
    return syscall(SYS_capset, &h, data);
}
static int guard(void)
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
static void filesystem(const char *wanted)
{
    char cwd[4096], map[4096];
    struct stat st;
    int fd;
    check(getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, wanted), "cached-cwd");
    check(syscall(SYS_getcwd, cwd, sizeof(cwd)) == (long)strlen(wanted) + 1 &&
        !strcmp(cwd, wanted), "raw-cwd");
    check(read_map(map, sizeof(map)) == 0, "copied-proc");
    check(stat("/etc/mnttab", &st) == 0 && S_ISREG(st.st_mode), "copied-mntfs");
    check(stat("/native/usr/lib/amd64/libc.so.1", &st) == 0 &&
        S_ISREG(st.st_mode), "copied-native-portal");
    fd = open("/dev/null", O_WRONLY);
    check(fd >= 0 && write(fd, "x", 1) == 1, "copied-device");
    if (fd >= 0) close(fd);
    check(chdir("/") == 0 && chdir("..") == 0 &&
        getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, "/"), "root-floor");
    check(chdir(wanted) == 0, "restore-cwd");
}
int main(int argc, char **argv)
{
    const char *mode = "combined", *cwd = "/test/mnt-entry/cwd";
    int drop = 0, linux_control = 0, expected = 0, filtered = 0, child = 0;
    int repeat = 0;
    int i, status, rc;
    uid_t uid, before_uid;
    gid_t gid, before_gid;
    pid_t pid;
    char before[4096], after[4096], saved_cwd[4096];
    struct __user_cap_data_struct oldcaps[2] = { 0 }, newcaps[2] = { 0 };
    unsigned flags;

    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(45);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--drop")) drop = 1;
        else if (!strcmp(argv[i], "--linux")) linux_control = 1;
        else if (!strcmp(argv[i], "--filter")) filtered = 1;
        else if (!strcmp(argv[i], "--child")) child = 1;
        else if (!strcmp(argv[i], "--repeat")) repeat = 1;
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) mode = argv[++i];
        else if (!strcmp(argv[i], "--cwd") && i + 1 < argc) cwd = argv[++i];
        else if (!strcmp(argv[i], "--errno") && i + 1 < argc) expected = atoi(argv[++i]);
        else return 2;
    }
    if (child) {
        if (!linux_control) filesystem(cwd);
        check(getcwd(saved_cwd, sizeof(saved_cwd)) && !strcmp(saved_cwd, cwd), "exec-cwd");
        printf("MNE_EXEC_PASS failures=%d uid=%ld\n", failures, (long)getuid());
        return failures != 0;
    }
    if (drop && (setgroups(0, NULL) || setgid(65534) || setuid(65534) ||
        prctl(PR_SET_DUMPABLE, 1UL, 0UL, 0UL, 0UL))) return 2;
    uid = getuid(); gid = getgid();
    if (chdir(cwd) || !getcwd(saved_cwd, sizeof(saved_cwd))) return 2;
    if (filtered && guard()) return 2;
    if (!strcmp(mode, "user-first") || !strcmp(mode, "drop-admin")) {
        if (unshare(CLONE_NEWUSER) || maps(uid, gid)) {
            perror("user namespace setup"); return 2;
        }
        if (!strcmp(mode, "drop-admin") && drop_admin()) return 2;
        flags = CLONE_NEWNS;
    } else if (!strcmp(mode, "combined")) flags = CLONE_NEWUSER | CLONE_NEWNS;
    else if (!strcmp(mode, "mount")) flags = CLONE_NEWNS;
    else return 2;
    before_uid = getuid(); before_gid = getgid();
    if (read_map(before, sizeof(before)) || caps(oldcaps)) return 2;
    errno = 0;
    rc = unshare(flags);
    if (expected) {
        check(rc == -1 && errno == expected, "expected-entry-error");
        check(getuid() == before_uid && getgid() == before_gid, "failed-entry-ids");
        check(read_map(after, sizeof(after)) == 0 && !strcmp(before, after), "failed-entry-map");
        check(caps(newcaps) == 0 && !memcmp(oldcaps, newcaps, sizeof(oldcaps)), "failed-entry-caps");
        check(getcwd(after, sizeof(after)) && !strcmp(after, saved_cwd), "failed-entry-cwd");
    } else {
        check(rc == 0, "namespace-entry");
        if (rc != 0) return 1;
        if (!strcmp(mode, "combined")) check(maps(uid, gid) == 0, "combined-maps");
        if (repeat) {
            uid_t parent_uid = getuid();
            gid_t parent_gid = getgid();
            check(unshare(flags) == 0, "nested-namespace-entry");
            if (!strcmp(mode, "combined"))
                check(maps(parent_uid, parent_gid) == 0, "nested-maps");
        }
        if (strcmp(mode, "mount")) check(getuid() == 0 && getgid() == 0, "namespace-root-ids");
        if (!linux_control) filesystem(saved_cwd);
        check(prctl(PR_GET_SECCOMP, 0UL, 0UL, 0UL, 0UL) == (filtered ? 2 : 0), "filter-state");
    }
    printf("MNE_READY_PID=%ld\n", (long)getpid());
    printf("MNE_STATE mode=%s expected=%d uid=%ld failures=%d\n", mode, expected, (long)getuid(), failures);
    if (!linux_control) raise(SIGSTOP);
    if (!expected) {
        pid = fork();
        if (pid == 0) {
            char *args[] = { argv[0], "--child", "--cwd", saved_cwd, NULL, NULL };
            if (linux_control) args[4] = "--linux";
            execv(argv[0], args); _exit(90);
        }
        check(pid > 0 && waitpid(pid, &status, 0) == pid &&
            WIFEXITED(status) && WEXITSTATUS(status) == 0, "fork-exec-wait");
        if (!strcmp(mode, "mount")) {
            pid = fork();
            if (pid == 0) {
                if (chroot(saved_cwd) || chdir("/")) _exit(92);
                errno = 0;
                if (unshare(CLONE_NEWUSER | CLONE_NEWNS) != -1 || errno != EPERM)
                    _exit(93);
                if (!getcwd(after, sizeof(after)) || strcmp(after, "/")) _exit(94);
                _exit(0);
            }
            check(pid > 0 && waitpid(pid, &status, 0) == pid &&
                WIFEXITED(status) && WEXITSTATUS(status) == 0, "chroot-entry-denied");
        }
        if (!linux_control) {
            pid = fork();
            if (pid == 0) {
                __asm__ volatile("int $0x91" : : "a"(20) : "memory");
                _exit(91);
            }
            check(pid > 0 && waitpid(pid, &status, 0) == pid &&
                WIFSIGNALED(status) && WTERMSIG(status) == SIGSYS, "native-entry-rejected");
        }
    }
    if (filtered) {
        errno = 0;
        check(syscall(SYS_getppid) == -1 && errno == EACCES, "filter-retained");
    }
    printf("MNE_PASS mode=%s expected=%d failures=%d\n", mode, expected, failures);
    return failures != 0;
}
