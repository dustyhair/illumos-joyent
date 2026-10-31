#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <stdint.h>

/* All numeric UID changes happen in direct children. The root supervisor
 * retains each child unreaped until its registry slot is cleared. */
static pid_t owner, held[2];
static void cleanup(void)
{
    if (getpid() != owner) return;
    for (int j = 0; j < 2; j++) {
        if (held[j] > 0) {
            (void)kill(held[j], SIGKILL);
            while (waitpid(held[j], NULL, 0) < 0 && errno == EINTR) {}
            held[j] = 0;
        }
    }
}
static void fail(const char *s)
{
    fprintf(stderr, "FAIL %s errno=%d\n", s, errno);
    exit(1);
}
static void interrupted(int s) { (void)s; exit(124); }
static void put(int fd, const void *p, size_t n)
{
    if (write(fd, p, n) != (ssize_t)n) fail("write");
}
static void get(int fd, void *p, size_t n)
{
    struct pollfd f = { fd, POLLIN, 0 };
    if (poll(&f, 1, 3000) != 1 || read(fd, p, n) != (ssize_t)n)
        fail("pipe deadline/read");
}
static void block_cleanup(sigset_t *old)
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM); sigaddset(&set, SIGINT);
    sigaddset(&set, SIGALRM);
    if (sigprocmask(SIG_BLOCK, &set, old)) fail("block cleanup");
}
static pid_t forkheld(int slot)
{
    sigset_t old;
    block_cleanup(&old);
    pid_t p = fork();
    if (p > 0) held[slot] = p;
    if (sigprocmask(SIG_SETMASK, &old, NULL)) fail("restore mask");
    if (p < 0) fail("fork");
    return p;
}
static void reap(int slot)
{
    siginfo_t si;
    pid_t p = held[slot];
    memset(&si, 0, sizeof(si));
    if (waitid(P_PID, p, &si, WEXITED | WNOWAIT) ||
        si.si_pid != p || si.si_code != CLD_EXITED || si.si_status != 0)
        fail("child exact exit status");
    /* No kill can occur after reap: clear while still unreaped. */
    sigset_t old;
    block_cleanup(&old);
    held[slot] = 0;
    int st;
    if (waitpid(p, &st, 0) != p || !WIFEXITED(st) || WEXITSTATUS(st))
        fail("reap");
    if (sigprocmask(SIG_SETMASK, &old, NULL)) fail("restore reap mask");
}
struct identity { pid_t pid, tid, sid; uid_t uid, euid; };
static int sendone(int api, pid_t p, int sig)
{
    siginfo_t i;
    memset(&i, 0, sizeof(i));
    i.si_signo = SIGUSR1; i.si_code = SI_QUEUE; i.si_errno = 7;
    i.si_pid = 12345; i.si_uid = 23456;
    i.si_value.sival_ptr = (void *)(uintptr_t)0x12345678;
    errno = 0;
    long r = api == 0 ? syscall(SYS_tkill, p, sig) :
        api == 1 ? syscall(SYS_tgkill, p, p, sig) :
        syscall(SYS_rt_tgsigqueueinfo, p, p, sig, &i);
    return r == 0 ? 0 : errno;
}
static void expect(int api, pid_t p, int sig, int want)
{
    int e = sendone(api, p, sig);
    printf("SEND api=%d sig=%d errno=%d expected=%d\n", api, sig, e, want);
    if (e != want) fail("send errno");
}
static void receiver(int different_sid, uid_t uid, int api, int allowed,
    int ready, int ack, int release)
{
    if (different_sid && setsid() < 0) fail("receiver setsid");
    if (setuid(uid)) fail("receiver setuid");
    sigset_t set;
    sigemptyset(&set); sigaddset(&set, SIGCONT); sigaddset(&set, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &set, NULL)) fail("receiver mask");
    struct identity id = { getpid(), (pid_t)syscall(SYS_gettid), getsid(0),
        getuid(), geteuid() };
    put(ready, &id, sizeof(id));
    if (raise(SIGSTOP)) fail("stop self");
    /* Blocked SIGCONT must still resume execution. A successful wait and
     * pipe acknowledgment prove progress without handler timing. */
    siginfo_t got;
    struct timespec ts = { 2, 0 };
    if (syscall(SYS_rt_sigtimedwait, &set, &got, &ts, 8) != SIGCONT || got.si_signo != SIGCONT)
        fail("CONT generation/resume");
    if (allowed && api == 2 && (got.si_code != SI_QUEUE ||
        got.si_errno != 7 || got.si_pid != 12345 || got.si_uid != 23456 ||
        (uintptr_t)got.si_value.sival_ptr != 0x12345678)) fail("CONT payload");
    printf("RECEIVE api=%d allowed=%d code=%d errno=%d pid=%d uid=%u value=%lx\n",
        api, allowed, got.si_code, got.si_errno, got.si_pid, got.si_uid,
        (unsigned long)(uintptr_t)got.si_value.sival_ptr);
    if (allowed && api != 2 && got.si_code != SI_TKILL)
        fail("CONT tkill code");
    ts.tv_sec = 0;
    if (syscall(SYS_rt_sigtimedwait, &set, &got, &ts, 8) != -1 || errno != EAGAIN)
        fail("extra pending/ordinary control");
    int token = 1;
    put(ack, &token, sizeof(token));
    get(release, &token, sizeof(token));
    _exit(token == 1 ? 0 : 1);
}
static void run(int api, int scenario)
{
    int ready[2], ack[2], release[2], result[2], token;
    if (pipe(ready) || pipe(ack) || pipe(release) || pipe(result)) fail("pipe");
    int different_sid = scenario != 1, cross = scenario != 0;
    int allowed = scenario != 2;
    uid_t uid = cross ? 61002 : 61001;
    pid_t p = forkheld(0);
    if (!p) receiver(different_sid, uid, api, allowed,
        ready[1], ack[1], release[0]);
    struct identity id;
    get(ready[0], &id, sizeof(id));
    if (id.pid != p || id.tid != p || id.uid != uid || id.euid != uid ||
        id.sid != getsid(p) || ((id.sid != getsid(0)) != different_sid))
        fail("live receiver identity");
    siginfo_t stopped;
    memset(&stopped, 0, sizeof(stopped));
    if (waitid(P_PID, p, &stopped, WSTOPPED | WEXITED | WNOWAIT) ||
        stopped.si_pid != p || stopped.si_code != CLD_STOPPED ||
        stopped.si_status != SIGSTOP) fail("exact stopped status");
    pid_t sender = forkheld(1);
    if (!sender) {
        if (setuid(61001) || getuid() != 61001 || geteuid() != 61001)
            fail("sender uid");
        if ((getsid(0) != id.sid) != different_sid) fail("sender sid");
        if (cross) {
            expect(api, p, SIGUSR1, EPERM);
            expect(api, p, 0, EPERM);
        } else expect(api, p, 0, 0);
        expect(api, p, SIGCONT, allowed ? 0 : EPERM);
        token = 1; put(result[1], &token, sizeof(token));
        _exit(0);
    }
    get(result[0], &token, sizeof(token));
    reap(1);
    if (!allowed) {
        /* Trusted supervisor resumes only its still-unreaped direct child. */
        if (kill(p, SIGCONT)) fail("supervisor rescue CONT");
    }
    get(ack[0], &token, sizeof(token));
    if (token != 1) fail("receiver ack");
    put(release[1], &token, sizeof(token));
    reap(0);
    close(ready[0]); close(ready[1]); close(ack[0]); close(ack[1]);
    close(release[0]); close(release[1]); close(result[0]); close(result[1]);
    printf("PASS case api=%d scenario=%d allowed=%d resumed=ack reaped=2\n",
        api, scenario, allowed);
}
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (getuid() != 0) fail("root required");
    owner = getpid();
    atexit(cleanup);
    signal(SIGTERM, interrupted); signal(SIGINT, interrupted);
    signal(SIGALRM, interrupted); signal(SIGPIPE, SIG_IGN);
    alarm(45);
    for (int api = 0; api < 3; api++)
        for (int scenario = 0; scenario < 3; scenario++) run(api, scenario);
    alarm(0);
    puts("PASS sigcont_matrix cases=9 reaped=18");
    return 0;
}
