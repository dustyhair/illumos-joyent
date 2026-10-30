/* CDDL version 1.0, http://www.illumos.org/license/CDDL. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/capability.h>
#include <linux/securebits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s errno=%d\n", __LINE__, #x, errno); return 1; } } while (0)
static const char *self;
static int cases, failures;
static atomic_int ready, release_worker, worker_error;
static uint64_t capbits(int permitted)
{
    struct __user_cap_header_struct h = { _LINUX_CAPABILITY_VERSION_3, 0 };
    struct __user_cap_data_struct d[2];
    if (syscall(SYS_capget, &h, d)) return UINT64_MAX;
    return permitted ? d[0].permitted | (uint64_t)d[1].permitted << 32 :
        d[0].effective | (uint64_t)d[1].effective << 32;
}
static int drop_caps(void)
{
    struct __user_cap_header_struct h = { _LINUX_CAPABILITY_VERSION_3, 0 };
    struct __user_cap_data_struct d[2] = {{0}, {0}};
    return syscall(SYS_capset, &h, d);
}
static int ids(int gid, unsigned r, unsigned e, unsigned s)
{
    unsigned got[3];
    if (syscall(gid ? SYS_getresgid : SYS_getresuid, &got[0], &got[1], &got[2])) return 0;
    return got[0] == r && got[1] == e && got[2] == s;
}
static int root_ids(void)
{
    CHECK(syscall(SYS_setgroups, 0, NULL) == 0);
    CHECK(syscall(SYS_setresgid, 0, 0, 0) == 0);
    CHECK(syscall(SYS_setresuid, 0, 0, 0) == 0);
    CHECK(ids(0, 0, 0, 0) && ids(1, 0, 0, 0));
    return 0;
}
static int mapped_ids(void)
{
    CHECK(ids(0, 42, 42, 42) && ids(1, 42, 42, 42));
    CHECK(getuid() == 42 && geteuid() == 42 && getgid() == 42 && getegid() == 42);
    CHECK(root_ids() == 0);
    CHECK(getuid() == 0 && geteuid() == 0 && getgid() == 0 && getegid() == 0);
    CHECK(syscall(SYS_setresgid, 1, 2, 42) == 0 && ids(1, 1, 2, 42));
    CHECK(syscall(SYS_setresuid, 1, 2, 42) == 0 && ids(0, 1, 2, 42));
    CHECK(capbits(0) == 0 && capbits(1) == 0);
    CHECK(prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) != 1);
    return 0;
}
static int validation(void)
{
    errno = 0;
    CHECK(syscall(SYS_setresuid, -1, 999, -1) == -1 && errno == EINVAL);
    CHECK(ids(0, 42, 42, 42));
    CHECK(drop_caps() == 0);
    errno = 0;
    CHECK(syscall(SYS_setresuid, -1, 0, -1) == -1 && errno == EPERM);
    errno = 0;
    CHECK(syscall(SYS_setresgid, -1, 0, -1) == -1 && errno == EPERM);
    CHECK(syscall(SYS_setresuid, -1, -1, -1) == 0);
    CHECK(syscall(SYS_setresgid, -1, -1, -1) == 0);
    errno = 0;
    CHECK(syscall(SYS_setuid, -1) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(syscall(SYS_getresuid, NULL, NULL, NULL) == -1 && errno == EFAULT);
    CHECK(ids(0, 42, 42, 42));
    return 0;
}
static int saved_ids(void)
{
    CHECK(root_ids() == 0);
    CHECK(syscall(SYS_setresgid, 1, 2, 0) == 0);
    CHECK(syscall(SYS_setresuid, 1, 2, 0) == 0);
    CHECK(capbits(0) == 0 && capbits(1) != 0);
    CHECK(syscall(SYS_setuid, 0) == 0 && ids(0, 1, 0, 0));
    CHECK(capbits(0) == capbits(1));
    CHECK(syscall(SYS_setgid, 1) == 0 && ids(1, 1, 1, 1));
    CHECK(syscall(SYS_setuid, 1) == 0 && ids(0, 1, 1, 1));
    CHECK(capbits(0) == 0 && capbits(1) == 0);
    errno = 0;
    CHECK(syscall(SYS_setuid, 0) == -1 && errno == EPERM);
    return 0;
}
static int re_ids(void)
{
    CHECK(root_ids() == 0);
    CHECK(syscall(SYS_setresgid, 1, 2, 0) == 0);
    CHECK(syscall(SYS_setresuid, 1, 2, 0) == 0);
    CHECK(drop_caps() == 0);
    errno = 0;
    CHECK(syscall(SYS_setreuid, 0, -1) == -1 && errno == EPERM);
    errno = 0;
    CHECK(syscall(SYS_setregid, 0, -1) == -1 && errno == EPERM);
    CHECK(syscall(SYS_setregid, -1, 0) == 0 && ids(1, 1, 0, 0));
    CHECK(syscall(SYS_setreuid, -1, 0) == 0 && ids(0, 1, 0, 0));
    return 0;
}
static int keep_caps(void)
{
    CHECK(root_ids() == 0);
    CHECK(prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0) == 0);
    uint64_t before = capbits(1);
    CHECK(syscall(SYS_setresuid, 1, 1, 1) == 0);
    CHECK(capbits(0) == 0 && capbits(1) == before);
    return 0;
}
static int no_fixup(void)
{
    CHECK(root_ids() == 0);
    CHECK(prctl(PR_SET_SECUREBITS, SECBIT_NO_SETUID_FIXUP, 0, 0, 0) == 0);
    uint64_t before = capbits(0);
    CHECK(syscall(SYS_setresuid, 1, 1, 1) == 0);
    CHECK(capbits(0) == before && capbits(1) == before);
    return 0;
}
static int groups_case(void)
{
    unsigned in[] = {2, 42, 0, 1}, out[4];
    CHECK(syscall(SYS_setgroups, 4, in) == 0);
    CHECK(syscall(SYS_getgroups, 0, (void *)1) == 4);
    CHECK(syscall(SYS_getgroups, 4, out) == 4);
    CHECK(out[0] == 42 && out[1] == 0 && out[2] == 1 && out[3] == 2);
    errno = 0;
    CHECK(syscall(SYS_getgroups, 3, out) == -1 && errno == EINVAL);
    unsigned bad = 999;
    errno = 0;
    CHECK(syscall(SYS_setgroups, 1, &bad) == -1 && errno == EINVAL);
    CHECK(syscall(SYS_getgroups, 0, NULL) == 4);
    CHECK(drop_caps() == 0);
    errno = 0;
    CHECK(syscall(SYS_setgroups, -1, (void *)1) == -1 && errno == EPERM);
    return 0;
}
static void *identity_worker(void *unused)
{
    (void)unused;
    unsigned g = 1;
    int error = syscall(SYS_setgroups, 1, &g) ||
        syscall(SYS_setresgid, 1, 1, 1) || syscall(SYS_setresuid, 1, 1, 1);
    if (drop_caps()) error = 1;
    if (!ids(0, 1, 1, 1) || !ids(1, 1, 1, 1)) error = 1;
    atomic_store(&worker_error, error);
    atomic_store(&ready, 1);
    while (!atomic_load(&release_worker)) sched_yield();
    return NULL;
}
static int thread_ids(void)
{
    pthread_t t;
    unsigned g = 42, out;
    CHECK(syscall(SYS_setgroups, 1, &g) == 0);
    CHECK(pthread_create(&t, NULL, identity_worker, NULL) == 0);
    while (!atomic_load(&ready)) sched_yield();
    CHECK(atomic_load(&worker_error) == 0);
    CHECK(ids(0, 42, 42, 42) && ids(1, 42, 42, 42));
    CHECK(syscall(SYS_getgroups, 1, &out) == 1 && out == 42);
    CHECK(capbits(0) != 0);
    atomic_store(&release_worker, 1);
    CHECK(pthread_join(t, NULL) == 0);
    return 0;
}
static int accounting(void)
{
    CHECK(root_ids() == 0);
    for (int i = 0; i < 64; i++) {
        unsigned uid = i & 1;
        CHECK(syscall(SYS_setresuid, uid, 0, 0) == 0);
        CHECK(ids(0, uid, 0, 0));
        pid_t p = fork(); int status;
        if (p == 0) _exit(ids(0, uid, 0, 0) ? 0 : 1);
        CHECK(p > 0 && waitpid(p, &status, 0) == p && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    return 0;
}
static int self_proc(void)
{
    CHECK(root_ids() == 0);
    CHECK(syscall(SYS_setresgid, 1, 1, 1) == 0);
    CHECK(syscall(SYS_setresuid, 1, 1, 1) == 0);
    CHECK(prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) != 1);
    FILE *f = fopen("/proc/self/status", "r");
    CHECK(f != NULL);
    char line[256]; int seen = 0;
    while (fgets(line, sizeof(line), f)) {
        unsigned a,b,c,d;
        if (sscanf(line, "Uid: %u %u %u %u", &a,&b,&c,&d) == 4 ||
            sscanf(line, "Gid: %u %u %u %u", &a,&b,&c,&d) == 4) {
            CHECK(a == 1 && b == 1 && c == 1 && d == 1); seen++;
        }
    }
    CHECK(fclose(f) == 0 && seen == 2);
    CHECK(prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) == 0);
    CHECK(prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) == 1);
    return 0;
}
static int file_access(void)
{
    char path[128];
    snprintf(path, sizeof(path), "/tmp/lx-ids-%d", getpid());
    CHECK(syscall(SYS_setgroups, 0, NULL) == 0);
    CHECK(syscall(SYS_setresgid, 1, 1, 1) == 0);
    CHECK(syscall(SYS_setresuid, 1, 1, 2) == 0);
    CHECK(drop_caps() == 0);
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(fd >= 0);
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && st.st_uid == 1 && st.st_gid == 1);
    CHECK(close(fd) == 0);
    CHECK(syscall(SYS_setresuid, -1, 2, -1) == 0);
    errno = 0;
    CHECK(open(path, O_RDONLY) == -1 && errno == EACCES);
    CHECK(syscall(SYS_setresuid, -1, 1, -1) == 0);
    fd = open(path, O_RDONLY);
    CHECK(fd >= 0 && close(fd) == 0);
    CHECK(unlink(path) == 0);
    return 0;
}
static atomic_int broadcast_done, broadcast_error;
static void *broadcast_observer(void *unused)
{
    (void)unused;
    while (!atomic_load(&broadcast_done)) sched_yield();
    unsigned g;
    if (!ids(0,1,1,1) || !ids(1,1,1,1) || syscall(SYS_getgroups,1,&g) != 1 || g != 1)
        atomic_store(&broadcast_error, 1);
    return NULL;
}
static void *broadcast_writer(void *unused)
{
    (void)unused;
    unsigned g = 1;
    if (setgroups(1, &g) || setresgid(1,1,1) || setresuid(1,1,1))
        atomic_store(&broadcast_error, 1);
    atomic_store(&broadcast_done, 1);
    return NULL;
}
static int libc_broadcast(void)
{
    CHECK(root_ids() == 0);
    pthread_t reader, writer;
    CHECK(pthread_create(&reader, NULL, broadcast_observer, NULL) == 0);
    CHECK(pthread_create(&writer, NULL, broadcast_writer, NULL) == 0);
    CHECK(pthread_join(writer, NULL) == 0 && pthread_join(reader, NULL) == 0);
    CHECK(atomic_load(&broadcast_error) == 0 && ids(0,1,1,1) && ids(1,1,1,1));
    return 0;
}
static int image_check(void)
{
    CHECK(ids(0, 1, 1, 1) && ids(1, 1, 1, 1));
    CHECK(getauxval(AT_UID) == 1 && getauxval(AT_EUID) == 1);
    CHECK(getauxval(AT_GID) == 1 && getauxval(AT_EGID) == 1);
    CHECK(capbits(0) == 0 && capbits(1) == 0);
    return 0;
}
static int exec_ids(void)
{
    CHECK(root_ids() == 0);
    CHECK(syscall(SYS_setresgid, 1, 1, 1) == 0);
    CHECK(syscall(SYS_setresuid, 1, 1, 1) == 0);
    char *const args[] = {(char *)self, "--image", NULL};
    syscall(SYS_execve, self, args, NULL);
    fprintf(stderr, "identity exec failed errno=%d\n", errno);
    return 1;
}
static void run(const char *name, int (*fn)(void))
{
    fflush(NULL); int status = 0; pid_t p = fork();
    if (p == 0) _exit(fn());
    int ok = p > 0 && waitpid(p, &status, 0) == p && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    printf("%s %s status=%#x\n", ok ? "PASS" : "FAIL", name, status);
    cases++; failures += !ok;
}
static int map_file(pid_t p, const char *name)
{
    char path[128]; snprintf(path, sizeof(path), "/proc/%d/%s", p, name);
    int fd = open(path, O_WRONLY); if (fd < 0) return -1;
    const char text[] = "0 1000 3\n42 0 1\n";
    int ok = write(fd, text, sizeof(text)-1) == sizeof(text)-1;
    close(fd); return ok ? 0 : -1;
}
int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--image")) return image_check();
    self = argv[0];
    if (argc == 2 && !strcmp(argv[1], "--linux")) {
        pid_t p = fork(); int status;
        CHECK(p >= 0);
        if (p == 0) {
            CHECK(unshare(CLONE_NEWUSER) == 0);
            raise(SIGSTOP);
        } else {
            CHECK(waitpid(p, &status, WUNTRACED) == p && WIFSTOPPED(status));
            CHECK(map_file(p, "uid_map") == 0 && map_file(p, "gid_map") == 0);
            CHECK(kill(p, SIGCONT) == 0);
            CHECK(waitpid(p, &status, 0) == p && WIFEXITED(status));
            return WEXITSTATUS(status);
        }
    } else if (argc == 1) {
        printf("SCOPE_READY_PID=%d\n", getpid()); fflush(stdout);
        raise(SIGSTOP);
    } else return 2;
    setvbuf(stdout, NULL, _IOLBF, 0);
    run("mapped getters and privileged setters", mapped_ids);
    run("mapping validation and failed-update atomicity", validation);
    run("saved UID restoration and capability fixups", saved_ids);
    run("setreuid and setregid permission rules", re_ids);
    run("KEEPCAPS UID transition", keep_caps);
    run("NO_SETUID_FIXUP UID transition", no_fixup);
    run("groups sorting, mapping and permission ordering", groups_case);
    run("per-thread IDs and groups", thread_ids);
    run("native real-UID accounting across fork", accounting);
    run("self procfs and dumpability", self_proc);
    run("mapped identities and auxv across exec", exec_ids);
    run("mapped file ownership and native access", file_access);
    run("glibc broadcasts identity changes from a worker", libc_broadcast);
    printf("SCOPE_IDS cases=%d failures=%d\n", cases, failures);
    return failures != 0;
}
