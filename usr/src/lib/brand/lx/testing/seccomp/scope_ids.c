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
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s errno=%d\n", __LINE__, #x, errno); return 1; } } while (0)
static const char *self;
static int cases, failures;
static int pre_root_pair[2], pre_unmapped_pair[2], pre_unmapped_fd;
static pid_t pre_peer_pid;
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
static atomic_int snapshot_tid[2], snapshot_go, snapshot_done;
static atomic_int snapshot_stop, snapshot_error;
static void *snapshot_writer(void *arg)
{
    unsigned index = (unsigned)(uintptr_t)arg;
    atomic_store(&snapshot_tid[index], (int)syscall(SYS_gettid));
    while (!atomic_load(&snapshot_go)) sched_yield();
    for (unsigned i = 0; i < 128; i++) {
        unsigned id = (i + index) & 1;
        if (syscall(SYS_setgroups, 1, &id) ||
            syscall(SYS_setresgid, id, id, id) ||
            syscall(SYS_setresuid, id, id, id) ||
            !ids(0, id, id, id) || !ids(1, id, id, id)) {
            atomic_store(&snapshot_error, 1);
            break;
        }
        sched_yield();
    }
    atomic_fetch_add(&snapshot_done, 1);
    while (!atomic_load(&snapshot_stop)) sched_yield();
    return NULL;
}
static int snapshot_status(int tid, int leader)
{
    char path[128], line[256];
    snprintf(path, sizeof(path), "/proc/self/task/%d/status", tid);
    FILE *f = fopen(path, "r");
    CHECK(f != NULL);
    int seen = 0, invalid = 0;
    while (fgets(line, sizeof(line), f)) {
        unsigned a,b,c,d;
        if (sscanf(line, "Uid: %u %u %u %u", &a,&b,&c,&d) == 4 ||
            sscanf(line, "Gid: %u %u %u %u", &a,&b,&c,&d) == 4) {
            if (a > (leader ? 0U : 1U) || a != b || a != c || a != d)
                invalid = 1;
            seen++;
        } else if (!strncmp(line, "Groups:", 7)) {
            if (sscanf(line + 7, "%u %u", &a,&b) != 1 ||
                a > (leader ? 0U : 1U)) invalid = 1;
            seen++;
        }
    }
    CHECK(fclose(f) == 0 && seen == 3 && !invalid);
    return 0;
}
static int concurrent_snapshots(void)
{
    CHECK(root_ids() == 0);
    CHECK(prctl(PR_SET_SECUREBITS, SECBIT_NO_SETUID_FIXUP, 0, 0, 0) == 0);
    unsigned group = 0;
    CHECK(syscall(SYS_setgroups, 1, &group) == 0);
    pthread_t threads[2];
    for (unsigned i = 0; i < 2; i++)
        CHECK(pthread_create(&threads[i], NULL, snapshot_writer,
            (void *)(uintptr_t)i) == 0);
    while (!atomic_load(&snapshot_tid[0]) || !atomic_load(&snapshot_tid[1]))
        sched_yield();
    atomic_store(&snapshot_go, 1);
    unsigned samples = 0;
    int error = 0, leader = (int)syscall(SYS_gettid);
    do {
        for (unsigned i = 0; i < 2; i++)
            error |= snapshot_status(atomic_load(&snapshot_tid[i]), 0);
        error |= snapshot_status(leader, 1);
        samples++;
        if (error || samples == 20000) break;
    } while (atomic_load(&snapshot_done) != 2 || samples < 128);
    atomic_store(&snapshot_stop, 1);
    for (unsigned i = 0; i < 2; i++) CHECK(pthread_join(threads[i], NULL) == 0);
    CHECK(!error && atomic_load(&snapshot_done) == 2 &&
        atomic_load(&snapshot_error) == 0 && samples >= 128);
    CHECK(ids(0, 0, 0, 0) && ids(1, 0, 0, 0));
    return 0;
}
static int set_effective(uint64_t effective)
{
    struct __user_cap_header_struct h = { _LINUX_CAPABILITY_VERSION_3, 0 };
    struct __user_cap_data_struct d[2];
    if (syscall(SYS_capget, &h, d)) return -1;
    d[0].effective = (uint32_t)effective;
    d[1].effective = effective >> 32;
    return (int)syscall(SYS_capset, &h, d);
}
static int trace_image(int argc, char **argv)
{
    uint64_t expected = strtoull(argv[2], NULL, 16);
    int error = !(ids(0, 0, 0, 0) && ids(1, 0, 0, 0) &&
        capbits(0) == expected && capbits(1) == expected);
    if (expected != 0 && prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) == 1)
        error = 1;
    if (error) fprintf(stderr, "traced exec E=%llx P=%llx expected=%llx dump=%d\n",
        (unsigned long long)capbits(0), (unsigned long long)capbits(1),
        (unsigned long long)expected, prctl(PR_GET_DUMPABLE, 0, 0, 0, 0));
    if (argc == 4) {
        int fd = atoi(argv[3]);
        if (write(fd, &error, sizeof(error)) != sizeof(error)) return 1;
    }
    return error;
}
static void trace_exec(uint64_t expected, int report)
{
    char caps[32], fd[32];
    snprintf(caps, sizeof(caps), "%llx", (unsigned long long)expected);
    snprintf(fd, sizeof(fd), "%d", report);
    char *args[] = {(char *)self, "--trace-image", caps, report < 0 ? NULL : fd, NULL};
    syscall(SYS_execve, self, args, NULL);
    _exit(90);
}
/* mode0 full, mode1 weak, mode2 full then weak, mode3 weak then full. */
static int trace_case(int attach, int mode, int detach)
{
    CHECK(root_ids() == 0);
    uint64_t full = capbits(0), weak = full & ~(UINT64_C(1) << CAP_SYS_PTRACE);
    CHECK(set_effective(mode & 1 ? weak : full) == 0);
    int ready_pipe[2], gate[2];
    CHECK(pipe(ready_pipe) == 0 && pipe(gate) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        close(ready_pipe[0]); close(gate[1]);
        if ((mode < 4 && drop_caps()) ||
            (mode >= 4 && set_effective(mode & 1 ? weak : full)) ||
            prctl(PR_SET_DUMPABLE, 1, 0, 0, 0)) _exit(91);
        if (!attach && ptrace(PTRACE_TRACEME, 0, NULL, NULL)) _exit(92);
        if (write(ready_pipe[1], "r", 1) != 1) _exit(93);
        if (!attach) raise(SIGSTOP);
        char byte;
        if (read(gate[0], &byte, 1) != 1) _exit(94);
        if (mode >= 4 && drop_caps()) _exit(101);
        int gain = detach || (attach && !(mode & 1)) || mode == 4;
        trace_exec(gain ? full : 0, -1);
    }
    close(ready_pipe[1]); close(gate[0]);
    char byte;
    CHECK(read(ready_pipe[0], &byte, 1) == 1);
    close(ready_pipe[0]);
    if (attach) CHECK(ptrace(PTRACE_ATTACH, child, NULL, NULL) == 0);
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFSTOPPED(status));
    if (mode == 2 || mode == 3) CHECK(set_effective(mode & 1 ? full : weak) == 0);
    CHECK(ptrace(detach ? PTRACE_DETACH : PTRACE_CONT, child, NULL, NULL) == 0);
    CHECK(write(gate[1], "x", 1) == 1 && close(gate[1]) == 0);
    for (int stops = 0; stops < 8; stops++) {
        CHECK(waitpid(child, &status, 0) == child);
        if (WIFEXITED(status)) return WEXITSTATUS(status);
        CHECK(WIFSTOPPED(status) && WSTOPSIG(status) == SIGTRAP);
        CHECK(ptrace(PTRACE_CONT, child, NULL, NULL) == 0);
    }
    return 1;
}
static int traceme_full(void) { return trace_case(0, 0, 0); }
static int traceme_weak(void) { return trace_case(0, 1, 0); }
static int traceme_drop(void) { return trace_case(0, 2, 0); }
static int traceme_restore(void) { return trace_case(0, 3, 0); }
static int traceme_child_full(void) { return trace_case(0, 4, 0); }
static int traceme_child_weak(void) { return trace_case(0, 5, 0); }
static int attach_full(void) { return trace_case(1, 0, 0); }
static int attach_weak(void) { return trace_case(1, 1, 0); }
static int attach_drop(void) { return trace_case(1, 2, 0); }
static int attach_restore(void) { return trace_case(1, 3, 0); }
static int detach_traceme(void) { return trace_case(0, 1, 1); }
static int detach_attach(void) { return trace_case(1, 1, 1); }
static int traceme_denied(void)
{
    CHECK(root_ids() == 0);
    int gate[2], status;
    CHECK(pipe(gate) == 0);
    pid_t child = fork(); CHECK(child >= 0);
    if (child == 0) {
        char byte; close(gate[1]);
        if (read(gate[0], &byte, 1) != 1) _exit(95);
        errno = 0;
        _exit(ptrace(PTRACE_TRACEME, 0, NULL, NULL) == -1 && errno == EPERM ? 0 : 96);
    }
    close(gate[0]); CHECK(drop_caps() == 0);
    CHECK(write(gate[1], "x", 1) == 1 && close(gate[1]) == 0);
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status));
    return WEXITSTATUS(status);
}
static int tracer_exits(void)
{
    CHECK(root_ids() == 0);
    uint64_t full = capbits(0);
    int report[2], status, result = 99;
    CHECK(pipe(report) == 0);
    pid_t tracer = fork(); CHECK(tracer >= 0);
    if (tracer == 0) {
        close(report[0]);
        if (set_effective(full & ~(UINT64_C(1) << CAP_SYS_PTRACE))) _exit(97);
        pid_t tracee = fork();
        if (tracee < 0) _exit(98);
        if (tracee == 0) {
            if (drop_caps() || ptrace(PTRACE_TRACEME, 0, NULL, NULL)) _exit(99);
            raise(SIGSTOP);
            trace_exec(full, report[1]);
        }
        if (waitpid(tracee, &status, 0) != tracee || !WIFSTOPPED(status)) _exit(100);
        _exit(0);
    }
    close(report[1]);
    CHECK(waitpid(tracer, &status, 0) == tracer && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(read(report[0], &result, sizeof(result)) == sizeof(result));
    CHECK(close(report[0]) == 0 && result == 0);
    return 0;
}
static atomic_int trace_sibling_done;
static void *trace_sibling(void *unused)
{
    (void)unused;
    while (!atomic_load(&trace_sibling_done)) sched_yield();
    return NULL;
}
static int traceme_task_snapshot(void)
{
    CHECK(root_ids() == 0);
    pthread_t sibling;
    CHECK(pthread_create(&sibling, NULL, trace_sibling, NULL) == 0);
    int error = traceme_denied();
    atomic_store(&trace_sibling_done, 1);
    CHECK(pthread_join(sibling, NULL) == 0);
    return error;
}
static int mixed_image(char **argv)
{
    uint64_t expected = strtoull(argv[2], NULL, 16);
    CHECK(ids(0, 0, 1, 1) && ids(1, 0, 1, 1));
    CHECK(getauxval(AT_UID) == 0 && getauxval(AT_EUID) == 1);
    CHECK(getauxval(AT_GID) == 0 && getauxval(AT_EGID) == 1);
    CHECK(capbits(0) == 0 && capbits(1) == expected);
    return 0;
}
static int mixed_nnp(int keep)
{
    CHECK(root_ids() == 0);
    CHECK(prctl(PR_SET_SECUREBITS, SECBIT_NO_SETUID_FIXUP, 0, 0, 0) == 0);
    uint64_t full = capbits(1);
    CHECK(syscall(SYS_setresgid, 0, 1, 1) == 0);
    CHECK(syscall(SYS_setresuid, 0, 1, 1) == 0);
    CHECK(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0);
    if (!keep) {
        CHECK(drop_caps() == 0);
        char *args[] = {(char *)self, "--reset-image", "0", NULL};
        syscall(SYS_execve, self, args, NULL);
        return 1;
    }
    char caps[32];
    snprintf(caps, sizeof(caps), "%llx", (unsigned long long)full);
    char *args[] = {(char *)self, "--mixed-image", caps, NULL};
    syscall(SYS_execve, self, args, NULL);
    return 1;
}
static int mixed_nnp_drop(void) { return mixed_nnp(0); }
static int mixed_nnp_keep(void) { return mixed_nnp(1); }
static pid_t inherit_wait(pid_t pid, int *status, int options)
{
    pid_t got;
    do { got = waitpid(pid, status, options); } while (got < 0 && errno == EINTR);
    return got;
}
static int trace_inherit(void)
{
    int status;
    pid_t child = fork(); CHECK(child >= 0);
    if (child == 0) {
        if (ptrace(PTRACE_TRACEME, 0, NULL, NULL)) _exit(102);
        raise(SIGSTOP);
        pid_t grandchild = fork();
        if (grandchild == 0) _exit(7);
        pid_t waited = grandchild < 0 ? -1 : inherit_wait(grandchild, &status, 0);
        if (grandchild < 0 || waited != grandchild ||
            !WIFEXITED(status) || WEXITSTATUS(status) != 7) {
            fprintf(stderr, "inherit child pid=%d waited=%d status=%#x errno=%d\n",
                grandchild, waited, status, errno);
            _exit(103);
        }
        _exit(0);
    }
    CHECK(inherit_wait(child, &status, 0) == child && WIFSTOPPED(status));
    CHECK(ptrace(PTRACE_SETOPTIONS, child, NULL, (void *)PTRACE_O_TRACEFORK) == 0);
    CHECK(ptrace(PTRACE_CONT, child, NULL, NULL) == 0);
    unsigned long grandchild = 0;
    int child_done = 0, grandchild_detached = 0;
    for (int events = 0; events < 40 && (!child_done || !grandchild_detached); events++) {
        pid_t got = inherit_wait(-1, &status, __WALL);
        CHECK(got > 0);
        if (WIFEXITED(status)) {
            if (got == child) {
                if (WEXITSTATUS(status) != 0) fprintf(stderr, "inherit parent status=%#x\n", status);
                CHECK(WEXITSTATUS(status) == 0); child_done = 1;
            }
            else { fprintf(stderr, "unexpected inherited tracee exit pid=%d status=%#x\n", got, status); return 1; }
            continue;
        }
        CHECK(WIFSTOPPED(status));
        if ((unsigned)status >> 16 == PTRACE_EVENT_FORK)
            CHECK(ptrace(PTRACE_GETEVENTMSG, got, NULL, &grandchild) == 0 && grandchild > 0);
        if (got == (pid_t)grandchild) {
            CHECK(!grandchild_detached && ptrace(PTRACE_DETACH, got, NULL, NULL) == 0);
            grandchild_detached = 1;
        } else {
            int signal = WSTOPSIG(status) == SIGCHLD ? SIGCHLD : 0;
            CHECK(ptrace(PTRACE_CONT, got, NULL, (void *)(uintptr_t)signal) == 0);
        }
    }
    CHECK(child_done && grandchild_detached && grandchild != 0);
    printf("TRACE_INHERIT_DETACH_PASS\n");
    return 0;
}
/* Malformed credentials must fail before any payload access or send. */
static int scm_lengths(void)
{
    for (int dropped = 0; dropped < 2; dropped++) {
        if (dropped) CHECK(drop_caps() == 0);
        for (int kind = 0; kind < 2; kind++) {
            int pair[2];
            CHECK(socketpair(AF_UNIX, kind ? SOCK_STREAM : SOCK_DGRAM, 0, pair) == 0);
            for (unsigned size = 0; size <= 16; size++) {
                if (size == sizeof(struct ucred)) continue;
                union { struct cmsghdr align; unsigned char data[CMSG_SPACE(16)]; } control;
                memset(&control, 0, sizeof(control));
                char byte = 'x';
                struct iovec iov = { &byte, 1 };
                struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1,
                    .msg_control = control.data, .msg_controllen = CMSG_SPACE(size) };
                struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
                cmsg->cmsg_level = SOL_SOCKET;
                cmsg->cmsg_type = SCM_CREDENTIALS;
                cmsg->cmsg_len = CMSG_LEN(size);
                errno = 0;
                CHECK(sendmsg(pair[0], &msg, MSG_NOSIGNAL) == -1 && errno == EINVAL);
            }
            char byte;
            errno = 0;
            CHECK(recv(pair[1], &byte, 1, MSG_DONTWAIT) == -1 && errno == EAGAIN);
            close(pair[0]); close(pair[1]);
        }
    }
    return 0;
}
static ssize_t scm_sendn(int fd, const struct ucred *cred, unsigned count)
{
    char byte = 'x'; struct iovec iov = { &byte, 1 };
    union { struct cmsghdr align; char data[2*CMSG_SPACE(sizeof(struct ucred))]; } buf;
    memset(&buf, 0, sizeof(buf));
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
    if (cred) {
        msg.msg_control = buf.data; msg.msg_controllen = count*CMSG_SPACE(sizeof(*cred));
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        for (unsigned i = 0; i < count; i++, cmsg = CMSG_NXTHDR(&msg,cmsg)) {
            cmsg->cmsg_level = SOL_SOCKET; cmsg->cmsg_type = SCM_CREDENTIALS;
            cmsg->cmsg_len = CMSG_LEN(sizeof(*cred));
            memcpy(CMSG_DATA(cmsg), &cred[i], sizeof(*cred));
        }
    }
    return sendmsg(fd, &msg, MSG_NOSIGNAL);
}
static ssize_t scm_send(int fd, const struct ucred *cred)
{ return scm_sendn(fd, cred, cred != NULL); }
static int scm_roundtrip(int kind, const struct ucred *claim, unsigned count, unsigned uid, unsigned gid)
{
    int pair[2], one = 1;
    CHECK(socketpair(AF_UNIX, kind, 0, pair) == 0);
    CHECK(setsockopt(pair[1], SOL_SOCKET, SO_PASSCRED, &one, sizeof(one)) == 0);
    CHECK(scm_sendn(pair[0], claim, count) == 1);
    char byte; struct iovec iov = { &byte, 1 };
    union { struct cmsghdr align; char data[256]; } buf;
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = buf.data, .msg_controllen = sizeof(buf.data) };
    CHECK(recvmsg(pair[1], &msg, MSG_DONTWAIT) == 1 && !(msg.msg_flags & MSG_CTRUNC));
    unsigned found = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg,c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_CREDENTIALS) continue;
        struct ucred got; CHECK(c->cmsg_len == CMSG_LEN(sizeof(got)));
        memcpy(&got, CMSG_DATA(c), sizeof(got));
        if (got.pid != getpid() || got.uid != uid || got.gid != gid)
            fprintf(stderr, "SCM kind=%d got=%d/%u/%u expected=%d/%u/%u\n", kind,got.pid,got.uid,got.gid,getpid(),uid,gid);
        CHECK(got.pid == getpid() && got.uid == uid && got.gid == gid);
        found++;
    }
    CHECK(found == 1); close(pair[0]); close(pair[1]); return 0;
}
static int scm_explicit(void)
{
    CHECK(drop_caps() == 0);
    struct ucred claim = { getpid(), getuid(), getgid() };
    CHECK(scm_roundtrip(SOCK_DGRAM, &claim, 1, 42, 42) == 0);
    CHECK(scm_roundtrip(SOCK_STREAM, &claim, 1, 42, 42) == 0);
    return 0;
}
static int scm_real(void)
{
    CHECK(root_ids() == 0);
    CHECK(syscall(SYS_setresgid, 1, 2, 0) == 0);
    CHECK(syscall(SYS_setresuid, 1, 2, 0) == 0);
    CHECK(drop_caps() == 0);
    CHECK(scm_roundtrip(SOCK_DGRAM, NULL, 0, 1, 1) == 0);
    CHECK(scm_roundtrip(SOCK_STREAM, NULL, 0, 1, 1) == 0);
    for (unsigned id = 0; id <= 2; id++) {
        struct ucred claim = { getpid(), id, id };
        CHECK(scm_roundtrip(SOCK_DGRAM, &claim, 1, id, id) == 0);
        CHECK(scm_roundtrip(SOCK_STREAM, &claim, 1, id, id) == 0);
    }
    return 0;
}
static int scm_privileged(void)
{
    struct ucred claim = { getpid(), 2, 0 };
    CHECK(scm_roundtrip(SOCK_DGRAM, &claim, 1, 2, 0) == 0);
    CHECK(scm_roundtrip(SOCK_STREAM, &claim, 1, 2, 0) == 0);
    struct ucred claims[2] = {{getpid(),0,1}, {getpid(),2,0}};
    CHECK(scm_roundtrip(SOCK_DGRAM, claims, 2, 2, 0) == 0);
    CHECK(scm_roundtrip(SOCK_STREAM, claims, 2, 2, 0) == 0);
    return 0;
}
static int scm_denied(void)
{
    int pair[2]; CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) == 0);
    /* A user-namespace CAP_SYS_ADMIN is not authority in its parent PID namespace. */
    struct ucred claim = { getppid(), 42, 42 };
    errno = 0; CHECK(scm_send(pair[0], &claim) == -1 && errno == EPERM);
    CHECK(drop_caps() == 0); claim.pid = getpid();
    claim.uid = 0; errno = 0;
    CHECK(scm_send(pair[0], &claim) == -1 && errno == EPERM);
    claim.uid = 42; claim.gid = 0; errno = 0;
    CHECK(scm_send(pair[0], &claim) == -1 && errno == EPERM);
    claim.uid = 999; claim.gid = 999; errno = 0;
    CHECK(scm_send(pair[0], &claim) == -1 && errno == EINVAL);
    claim.uid = (unsigned)-1; claim.gid = 42; errno = 0;
    CHECK(scm_send(pair[0], &claim) == -1 && errno == EINVAL);
    char byte; errno = 0;
    CHECK(recv(pair[1], &byte, 1, MSG_DONTWAIT) == -1 && errno == EAGAIN);
    close(pair[0]); close(pair[1]); return 0;
}
static int stream_receive(int fd, pid_t pid, unsigned id)
{
    char byte; struct iovec iov = { &byte, 1 };
    union { struct cmsghdr align; char data[256]; } buf;
    struct msghdr msg = {.msg_iov=&iov,.msg_iovlen=1,.msg_control=buf.data,.msg_controllen=sizeof(buf.data)};
    CHECK(recvmsg(fd,&msg,0)==1 && !(msg.msg_flags & MSG_CTRUNC));
    unsigned count=0;
    for (struct cmsghdr *c=CMSG_FIRSTHDR(&msg);c;c=CMSG_NXTHDR(&msg,c)) {
        if(c->cmsg_level!=SOL_SOCKET || c->cmsg_type!=SCM_CREDENTIALS) continue;
        struct ucred got; CHECK(c->cmsg_len==CMSG_LEN(sizeof(got))); memcpy(&got,CMSG_DATA(c),sizeof(got));
        if(got.pid!=pid || got.uid!=id || got.gid!=id)
            fprintf(stderr,"stream got=%d/%u/%u expected=%d/%u/%u\n",got.pid,got.uid,got.gid,pid,id,id);
        CHECK(got.pid==pid && got.uid==id && got.gid==id); count++;
    }
    CHECK(count==1); return 0;
}
static int stream_sender(void)
{
    CHECK(root_ids()==0);
    int pair[2],one=1,status; CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0);
    CHECK(setsockopt(pair[1],SOL_SOCKET,SO_PASSCRED,&one,sizeof(one))==0);
    socklen_t len=sizeof(one); one=0;
    CHECK(getsockopt(pair[1],SOL_SOCKET,SO_PASSCRED,&one,&len)==0 && one==1);
    pid_t child=fork(); CHECK(child>=0);
    if(child==0) {
        close(pair[1]); CHECK(write(pair[0],"a",1)==1);
        CHECK(syscall(SYS_setresgid,1,1,1)==0 && syscall(SYS_setresuid,1,1,1)==0);
        struct iovec iov={ (void *)"b",1 };
        CHECK(syscall(SYS_writev,pair[0],&iov,1)==1);
        _exit(0);
    }
    CHECK(stream_receive(pair[1],child,0)==0);
    CHECK(stream_receive(pair[1],child,1)==0);
    CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    CHECK(syscall(SYS_setresgid,2,2,2)==0 && syscall(SYS_setresuid,2,2,2)==0);
    CHECK(write(pair[0],"c",1)==1 && stream_receive(pair[1],getpid(),2)==0);
    close(pair[0]);close(pair[1]);return 0;
}
static int stream_receive_fd(int fd, pid_t pid, unsigned id)
{
    char byte; struct iovec iov={&byte,1};
    union {struct cmsghdr align;char data[256];} control;
    struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control.data,.msg_controllen=sizeof(control.data)};
    CHECK(recvmsg(fd,&msg,MSG_CMSG_CLOEXEC)==1 && !(msg.msg_flags&MSG_CTRUNC));
    unsigned rights=0,creds=0;
    for(struct cmsghdr *c=CMSG_FIRSTHDR(&msg);c;c=CMSG_NXTHDR(&msg,c)) {
        CHECK(c->cmsg_level==SOL_SOCKET);
        if(c->cmsg_type==SCM_RIGHTS) {
            int received;CHECK(c->cmsg_len==CMSG_LEN(sizeof(received)));memcpy(&received,CMSG_DATA(c),sizeof(received));
            CHECK((fcntl(received,F_GETFD)&FD_CLOEXEC)!=0 && read(received,&byte,1)==1 && byte=='Z');close(received);rights++;
        } else if(c->cmsg_type==SCM_CREDENTIALS) {
            struct ucred cr;CHECK(c->cmsg_len==CMSG_LEN(sizeof(cr)));memcpy(&cr,CMSG_DATA(c),sizeof(cr));
            CHECK(cr.pid==pid && cr.uid==id && cr.gid==id);creds++;
        } else CHECK(0);
    }
    CHECK(rights==1 && creds==1);return 0;
}
static int stream_send_fd(int fd)
{
    char path[]="/tmp/lx-stream-fd-XXXXXX";int file=mkstemp(path);
    CHECK(file>=0 && unlink(path)==0 && write(file,"Z",1)==1 && lseek(file,0,SEEK_SET)==0);
    union {struct cmsghdr align;char data[CMSG_SPACE(sizeof(file))];} control;
    memset(&control,0,sizeof(control));char byte='x';struct iovec iov={&byte,1};
    struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control.data,.msg_controllen=sizeof(control.data)};
    struct cmsghdr *c=CMSG_FIRSTHDR(&msg);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(file));memcpy(CMSG_DATA(c),&file,sizeof(file));
    CHECK(sendmsg(fd,&msg,0)==1);close(file);return 0;
}
static int stream_rights(void)
{
    CHECK(root_ids()==0);
    int pair[2],one=1;CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0);
    CHECK(setsockopt(pair[1],SOL_SOCKET,SO_PASSCRED,&one,sizeof(one))==0);
    CHECK(stream_send_fd(pair[0])==0 && stream_receive_fd(pair[1],getpid(),0)==0);
    close(pair[0]);close(pair[1]);return 0;
}
static int stream_accept_case(int exited)
{
    CHECK(root_ids()==0);
    char dir[]="/tmp/lx-accept-XXXXXX";CHECK(mkdtemp(dir)!=NULL);
    struct sockaddr_un addr;memset(&addr,0,sizeof(addr));addr.sun_family=AF_UNIX;
    CHECK(snprintf(addr.sun_path,sizeof(addr.sun_path),"%s/socket",dir)>0);
    int listener=socket(AF_UNIX,SOCK_STREAM,0),one=1,ready_pipe[2],release_pipe[2],status;
    CHECK(listener>=0 && setsockopt(listener,SOL_SOCKET,SO_PASSCRED,&one,sizeof(one))==0);
    CHECK(bind(listener,(struct sockaddr *)&addr,sizeof(addr))==0 && listen(listener,4)==0);
    CHECK(pipe(ready_pipe)==0 && pipe(release_pipe)==0);
    pid_t child=fork();CHECK(child>=0);
    if(child==0) {
        close(ready_pipe[0]);close(release_pipe[1]);close(listener);
        int sender=socket(AF_UNIX,SOCK_STREAM,0);CHECK(sender>=0);
        CHECK(connect(sender,(struct sockaddr *)&addr,sizeof(addr))==0);
        CHECK(write(sender,"a",1)==1);
        CHECK(syscall(SYS_setresgid,1,1,1)==0 && syscall(SYS_setresuid,1,1,1)==0);
        CHECK(write(sender,"b",1)==1);
        if(exited) CHECK(stream_send_fd(sender)==0);
        CHECK(write(ready_pipe[1],"r",1)==1);
        if(!exited) { char byte;CHECK(read(release_pipe[0],&byte,1)==1); }
        close(sender);_exit(0);
    }
    close(ready_pipe[1]);close(release_pipe[0]);char byte;
    CHECK(read(ready_pipe[0],&byte,1)==1);
    if(exited) CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    int accepted=accept(listener,NULL,NULL);CHECK(accepted>=0);
    socklen_t len=sizeof(one);one=0;
    CHECK(getsockopt(accepted,SOL_SOCKET,SO_PASSCRED,&one,&len)==0 && one==1);
    CHECK(stream_receive(accepted,child,0)==0 && stream_receive(accepted,child,1)==0);
    if(exited) CHECK(stream_receive_fd(accepted,child,1)==0);
    if(!exited) {
        CHECK(write(release_pipe[1],"r",1)==1);
        CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    }
    CHECK(read(accepted,&byte,1)==0);
    close(accepted);close(listener);close(ready_pipe[0]);close(release_pipe[1]);
    CHECK(unlink(addr.sun_path)==0 && rmdir(dir)==0);return 0;
}
static int stream_accept_live(void) {return stream_accept_case(0);}
static int stream_accept_exited(void) {return stream_accept_case(1);}
/* Capture native identities before the trusted fixture attaches its map. */
static int prepare_peer_fixtures(void)
{
    pre_peer_pid = getpid();
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pre_root_pair) == 0);
    CHECK(syscall(SYS_setresgid, -1, 2000, -1) == 0);
    CHECK(syscall(SYS_setresuid, -1, 2000, -1) == 0);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pre_unmapped_pair) == 0);
    char path[128];
    snprintf(path, sizeof(path), "/tmp/lx-unmapped-%d", getpid());
    pre_unmapped_fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    CHECK(pre_unmapped_fd >= 0 && unlink(path) == 0);
    CHECK(syscall(SYS_setresuid, -1, 0, -1) == 0);
    CHECK(syscall(SYS_setresuid, 0, 0, 0) == 0);
    CHECK(syscall(SYS_setresgid, 0, 0, 0) == 0);
    return 0;
}
static int peer_is(int fd, pid_t pid, unsigned uid, unsigned gid)
{
    struct ucred peer = { 0 };
    socklen_t size = sizeof(peer);
    CHECK(getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) == 0);
    CHECK(size == sizeof(peer) && peer.pid == pid && peer.uid == uid && peer.gid == gid);
    return 0;
}
static int peer_inherited(void)
{
    CHECK(peer_is(pre_root_pair[0], pre_peer_pid, 42, 42) == 0);
    CHECK(peer_is(pre_unmapped_pair[0], pre_peer_pid, 65534, 65534) == 0);
    return 0;
}
static int peer_mapped(void)
{
    CHECK(root_ids() == 0);
    CHECK(syscall(SYS_setresgid, 2, 2, 2) == 0);
    CHECK(syscall(SYS_setresuid, 1, 1, 1) == 0);
    int pair[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
    CHECK(peer_is(pair[0], getpid(), 1, 2) == 0);
    CHECK(close(pair[0]) == 0 && close(pair[1]) == 0);
    return 0;
}
static int peer_cached(void)
{
    CHECK(root_ids() == 0);
    int pair[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
    CHECK(syscall(SYS_setresgid, 2, 2, 2) == 0);
    CHECK(syscall(SYS_setresuid, 1, 1, 1) == 0);
    CHECK(peer_is(pair[0], getpid(), 0, 0) == 0);
    CHECK(close(pair[0]) == 0 && close(pair[1]) == 0);
    return 0;
}
static int worker_pair[2], worker_pair_error;
static void *peer_creator(void *unused)
{
    (void)unused;
    worker_pair_error = syscall(SYS_setresgid, 2, 2, 2) ||
        syscall(SYS_setresuid, 1, 1, 1) ||
        socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, worker_pair);
    return NULL;
}
static int peer_task(void)
{
    CHECK(root_ids() == 0);
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, peer_creator, NULL) == 0);
    CHECK(pthread_join(thread, NULL) == 0 && worker_pair_error == 0);
    CHECK(ids(0,0,0,0) && ids(1,0,0,0));
    CHECK(peer_is(worker_pair[0], getpid(), 1, 2) == 0);
    CHECK(close(worker_pair[0]) == 0 && close(worker_pair[1]) == 0);
    return 0;
}
static int peer_connection(void)
{
    CHECK(root_ids() == 0);
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    snprintf(address.sun_path, sizeof(address.sun_path), "/tmp/lx-peer-%d", getpid());
    mode_t oldmask = umask(0);
    int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(listener >= 0 && bind(listener, (void *)&address, sizeof(address)) == 0);
    CHECK(listen(listener, 1) == 0);
    pid_t server = getpid(), child = fork(); CHECK(child >= 0);
    if (child == 0) {
        if (syscall(SYS_setresgid, 2, 2, 2) || syscall(SYS_setresuid, 1, 1, 1)) _exit(104);
        int client = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (client < 0 || connect(client, (void *)&address, sizeof(address))) _exit(105);
        if (peer_is(client, server, 0, 0)) _exit(106);
        char byte;
        _exit(read(client, &byte, 1) == 1 ? 0 : 107);
    }
    int accepted = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
    CHECK(accepted >= 0 && peer_is(accepted, child, 1, 2) == 0);
    CHECK(write(accepted, "x", 1) == 1);
    int status;
    CHECK(inherit_wait(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(close(accepted) == 0 && close(listener) == 0 && unlink(address.sun_path) == 0);
    umask(oldmask);
    return 0;
}
static int owned_fd(void)
{
    char path[128];
    snprintf(path, sizeof(path), "/tmp/lx-owned-%d", getpid());
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    if (fd >= 0 && unlink(path)) { close(fd); return -1; }
    return fd;
}
static int owner_is(int fd, unsigned uid, unsigned gid)
{
    struct stat st;
    return fstat(fd, &st) == 0 && st.st_uid == uid && st.st_gid == gid;
}
/* These exercise native owner/group permission, with CAP_CHOWN dropped. */
static int owner_same(void)
{
    CHECK(root_ids() == 0);
    unsigned group = 1;
    CHECK(syscall(SYS_setgroups, 1, &group) == 0);
    CHECK(drop_caps() == 0);
    int fd = owned_fd(); CHECK(fd >= 0);
    CHECK(fchown(fd, 0, 0) == 0 && owner_is(fd, 0, 0));
    CHECK(fchown(fd, -1, 1) == 0 && owner_is(fd, 0, 1));
    CHECK(close(fd) == 0);
    return 0;
}
static int owner_paths(void)
{
    CHECK(root_ids() == 0 && drop_caps() == 0);
    char path[128], link[128];
    snprintf(path, sizeof(path), "/tmp/lx-owner-path-%d", getpid());
    snprintf(link, sizeof(link), "/tmp/lx-owner-link-%d", getpid());
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    CHECK(fd >= 0 && symlink(path, link) == 0);
    CHECK(chown(path, 0, -1) == 0);
    CHECK(lchown(link, 0, 0) == 0);
    CHECK(fchownat(AT_FDCWD, link, 0, 0, AT_SYMLINK_NOFOLLOW) == 0);
    CHECK(fchownat(fd, "", 0, 0, AT_EMPTY_PATH) == 0);
    int pathfd = open(link, O_PATH | O_NOFOLLOW | O_CLOEXEC); CHECK(pathfd >= 0);
    CHECK(fchownat(pathfd, "", 0, 0, AT_EMPTY_PATH) == 0);
    errno = 0;
    CHECK(fchown(pathfd, 0, 0) == -1 && errno == EBADF);
    CHECK(owner_is(fd, 0, 0));
    CHECK(close(pathfd) == 0 && close(fd) == 0 && unlink(link) == 0 && unlink(path) == 0);
    return 0;
}
static int owner_invalid(void)
{
    CHECK(root_ids() == 0 && drop_caps() == 0);
    int fd = owned_fd(); CHECK(fd >= 0);
    errno = 0; CHECK(fchown(fd, 999, 0) == -1 && errno == EINVAL);
    errno = 0; CHECK(fchown(fd, 0, 999) == -1 && errno == EINVAL);
    errno = 0; CHECK(fchown(-1, 999, 999) == -1 && errno == EBADF);
    errno = 0;
    CHECK(fchownat(AT_FDCWD, "/no-such-owner-file", 999, 999, 0) == -1 && errno == ENOENT);
    CHECK(owner_is(fd, 0, 0) && close(fd) == 0);
    return 0;
}
static int owner_denied(void)
{
    CHECK(root_ids() == 0 && drop_caps() == 0);
    int fd = owned_fd(); CHECK(fd >= 0);
    errno = 0; CHECK(fchown(fd, 1, -1) == -1 && errno == EPERM);
    errno = 0; CHECK(fchown(fd, -1, 1) == -1 && errno == EPERM);
    CHECK(owner_is(fd, 0, 0) && close(fd) == 0);
    return 0;
}
static int owner_unmapped(void)
{
    CHECK(root_ids() == 0);
    CHECK(owner_is(pre_unmapped_fd, 65534, 65534));
    errno = 0;
    CHECK(fchown(pre_unmapped_fd, 0, 0) == -1 && errno == EPERM);
    CHECK(owner_is(pre_unmapped_fd, 65534, 65534));
    return 0;
}
static int chown_worker_fd, chown_worker_error;
static void *chown_worker(void *unused)
{
    (void)unused;
    chown_worker_error = syscall(SYS_setresgid, 1, 1, 1) ||
        syscall(SYS_setresuid, 1, 1, 1);
    errno = 0;
    if (fchown(chown_worker_fd, 0, 0) != -1 || errno != EPERM) chown_worker_error = 1;
    return NULL;
}
static int owner_task(void)
{
    CHECK(root_ids() == 0);
    chown_worker_fd = owned_fd(); CHECK(chown_worker_fd >= 0);
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, chown_worker, NULL) == 0);
    CHECK(pthread_join(thread, NULL) == 0 && chown_worker_error == 0);
    CHECK(ids(0,0,0,0) && fchown(chown_worker_fd, 0, 0) == 0);
    CHECK(owner_is(chown_worker_fd, 0, 0) && close(chown_worker_fd) == 0);
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
    if (argc == 2 && !strcmp(argv[1], "--unscoped-trace")) return trace_inherit();
    if (argc == 3 && !strcmp(argv[1], "--mixed-image")) return mixed_image(argv);
    if (argc == 3 && !strcmp(argv[1], "--reset-image")) {
        CHECK(prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) != 1);
        return trace_image(argc, argv);
    }
    if (argc >= 3 && !strcmp(argv[1], "--trace-image")) return trace_image(argc, argv);
    if (argc == 2 && !strcmp(argv[1], "--image")) return image_check();
    self = argv[0];
    if (argc == 2 && !strcmp(argv[1], "--scm-length-only")) return scm_lengths();
    CHECK(prepare_peer_fixtures() == 0);
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
    run("concurrent task identity and procfs snapshots", concurrent_snapshots);
    run("TRACEME full parent cannot bypass child snapshot", traceme_full);
    run("TRACEME weak tracer restricts exec", traceme_weak);
    run("TRACEME parent drop does not replace child snapshot", traceme_drop);
    run("TRACEME parent restore does not replace child snapshot", traceme_restore);
    run("TRACEME retains full child attach credential", traceme_child_full);
    run("TRACEME retains weak child attach credential", traceme_child_weak);
    run("ATTACH privileged tracer exec", attach_full);
    run("ATTACH weak tracer restricts exec", attach_weak);
    run("ATTACH retains pre-drop tracer credential", attach_drop);
    run("ATTACH retains pre-restore tracer credential", attach_restore);
    run("TRACEME detach clears exec restriction", detach_traceme);
    run("ATTACH detach clears exec restriction", detach_attach);
    run("TRACEME rejects weaker parent", traceme_denied);
    run("tracer exit clears exec restriction", tracer_exits);
    run("TRACEME parent permission uses task credential", traceme_task_snapshot);
    run("NNP mixed-ID exec restricts gain and resets effective IDs", mixed_nnp_drop);
    run("NNP mixed-ID exec without gain preserves effective IDs", mixed_nnp_keep);
    run("malformed socket credentials reject short and oversized payloads", scm_lengths);
    run("explicit socket credential namespace roundtrip", scm_explicit);
    run("socket credentials use real IDs and accept owned saved IDs", scm_real);
    run("socket credentials accept scoped SETUID and SETGID claims", scm_privileged);
    run("socket credential forgery and unmapped IDs are rejected", scm_denied);
    run("stream credentials follow fork, writev and sender ID changes", stream_sender);
    run("stream credentials preserve descriptor passing and CLOEXEC", stream_rights);
    run("accept inherits credentials for a live queued sender", stream_accept_live);
    run("accept preserves queued credentials after sender exit", stream_accept_exited);
    run("peer credentials use the reader namespace and overflow IDs", peer_inherited);
    run("peer credentials translate fresh mapped identities", peer_mapped);
    run("peer credentials retain the connection snapshot", peer_cached);
    run("peer credentials retain a departed creator task", peer_task);
    run("Unix connect and accept return mapped peer credentials", peer_connection);
    run("mapped ownership and native supplementary group permission", owner_same);
    run("ownership paths, symlinks, empty paths and O_PATH", owner_paths);
    run("ownership mapping errors preserve native lookup precedence", owner_invalid);
    run("ownership changes still require native authority", owner_denied);
    run("unmapped inode ownership remains protected", owner_unmapped);
    run("ownership permission uses the calling task credential", owner_task);
    if (argc == 1) {
        unsigned group = 1;
        CHECK(root_ids() == 0);
        CHECK(syscall(SYS_setgroups, 1, &group) == 0);
        CHECK(syscall(SYS_setresgid, 1, 1, 1) == 0);
        CHECK(syscall(SYS_setresuid, 1, 1, 1) == 0);
        printf("SID_READY_PID=%d\n", getpid()); fflush(stdout);
        raise(SIGSTOP);
    }
    printf("SCOPE_IDS cases=%d failures=%d\n", cases, failures);
    return failures != 0;
}
