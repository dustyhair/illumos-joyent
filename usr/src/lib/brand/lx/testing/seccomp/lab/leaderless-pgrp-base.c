#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Every process is a direct child. Only exact, unreaped child PIDs are killed. */
struct command { int op; pid_t pgid; };
struct result { int ret, error; pid_t pgid, sid, tid; };
struct child { pid_t pid; int cmd, result, live; };
static struct child children[16];
static int count, failures;
static volatile sig_atomic_t aborted;

static void interrupted(int sig)
{
    aborted = sig;
}
static int threadpipe[2], threadreport[2];

static void cleanup(void)
{
    int i, status;
    for (i = 0; i < count; i++) {
        if (children[i].live)
            (void)kill(children[i].pid, SIGKILL);
    }
    for (i = 0; i < count; i++) {
        if (children[i].live) {
            while (waitpid(children[i].pid, &status, 0) < 0 && errno == EINTR) {}
            children[i].live = 0;
        }
        close(children[i].cmd);
        close(children[i].result);
    }
}

static void die(const char *message)
{
    perror(message);
    cleanup();
    exit(2);
}

static void transfer(int fd, void *buf, size_t size, int writing)
{
    size_t done = 0;
    while (done < size) {
        if (aborted) { errno = EINTR; die("interrupted"); }
        struct pollfd pfd = { fd, writing ? POLLOUT : POLLIN, 0 };
        ssize_t n;
        int ret;
        do { ret = poll(&pfd, 1, 5000); } while (ret < 0 && errno == EINTR && !aborted);
        if (ret <= 0) { errno = ret == 0 ? ETIMEDOUT : errno; die("pipe poll"); }
        n = writing ? write(fd, (char *)buf + done, size - done) :
            read(fd, (char *)buf + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { errno = n == 0 ? EPIPE : errno; die("pipe transfer"); }
        done += (size_t)n;
    }
}

static void *thread_main(void *unused)
{
    pid_t tid = (pid_t)syscall(SYS_gettid);
    char byte;
    (void)unused;
    if (write(threadreport[1], &tid, sizeof(tid)) != sizeof(tid)) _exit(91);
    /* The thread lives until the whole owned child exits. */
    for (;;) {
        if (read(threadpipe[0], &byte, 1) < 0 && errno == EINTR) continue;
        pause();
    }
    return NULL;
}

static void worker(int input, int output)
{
    struct command c;
    struct result r;
    pthread_t thread;
    pid_t tid;
    /* Do not invoke supervisor cleanup from a child on an I/O failure. */
    count = 0;
    for (;;) {
        transfer(input, &c, sizeof(c), 0);
        if (c.op == 'X') _exit(0);
        memset(&r, 0, sizeof(r));
        errno = 0;
        if (c.op == 'S') r.ret = setsid();
        else if (c.op == 'P') r.ret = setpgid(0, c.pgid);
        else if (c.op == 'T') {
            /* Separate report and lifetime pipes avoid thread/parent races. */
            if (pipe(threadreport) != 0 || pipe(threadpipe) != 0) _exit(92);
            r.ret = pthread_create(&thread, NULL, thread_main, NULL);
            if (r.ret != 0) _exit(93);
            transfer(threadreport[0], &tid, sizeof(tid), 0);
            r.tid = tid;
            close(threadreport[0]); close(threadreport[1]);
        }
        r.error = errno;
        r.pgid = getpgrp();
        r.sid = getsid(0);
        transfer(output, &r, sizeof(r), 1);
    }
}

static struct child *spawn(void)
{
    int cmd[2], result[2], i;
    pid_t pid;
    struct child *c;
    if (count == 16 || pipe(cmd) != 0 || pipe(result) != 0) die("spawn pipes");
    pid = fork();
    if (pid < 0) die("fork");
    if (pid == 0) {
        close(cmd[1]); close(result[0]);
        for (i = 0; i < count; i++) {
            close(children[i].cmd); close(children[i].result);
        }
        worker(cmd[0], result[1]);
        _exit(94);
    }
    close(cmd[0]); close(result[1]);
    c = &children[count++];
    *c = (struct child){pid, cmd[1], result[0], 1};
    if (aborted) { errno = EINTR; die("spawn interrupted"); }
    return c;
}

static struct result command(struct child *c, int op, pid_t pgid)
{
    struct command cmd = { op, pgid };
    struct result r;
    transfer(c->cmd, &cmd, sizeof(cmd), 1);
    transfer(c->result, &r, sizeof(r), 0);
    return r;
}

static void check(const char *name, int ret, int error, int expected)
{
    int pass = expected == 0 ? ret == 0 && error == 0 : ret == -1 && error == expected;
    printf("%s ret=%d errno=%d expected=%d %s\n", name, ret, error, expected,
        pass ? "PASS" : "FAIL");
    if (!pass) failures++;
}

static void parent_set(const char *name, pid_t pid, pid_t pgid, int expected)
{
    int ret, error;
    errno = 0;
    ret = setpgid(pid, pgid);
    error = errno;
    check(name, ret, error, expected);
}

static void child_set(const char *name, struct child *c, pid_t pgid, int expected)
{
    struct result r = command(c, 'P', pgid);
    check(name, r.ret, r.error, expected);
    if (expected == 0 && r.pgid != (pgid == 0 ? c->pid : pgid)) {
        printf("%s group mismatch got=%ld requested=%ld FAIL\n", name,
            (long)r.pgid, (long)pgid);
        failures++;
    }
}

static void reap(struct child *c)
{
    struct command cmd = { 'X', 0 };
    int status;
    pid_t ret;
    transfer(c->cmd, &cmd, sizeof(cmd), 1);
    do { ret = waitpid(c->pid, &status, 0); } while (ret < 0 && errno == EINTR && !aborted);
    if (ret != c->pid) die("waitpid");
    c->live = 0;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "child %ld status=%d\n", (long)c->pid, status);
        failures++;
    }
}

/* zlogin can start us as a group leader. A child may safely setsid(). */
static int isolated_child(void)
{
    pid_t child = fork(), result;
    int status;
    if (child < 0) { perror("bootstrap fork"); return 2; }
    if (child == 0) return -1;
    do { result = waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
    if (result != child || !WIFEXITED(status)) return 2;
    return WEXITSTATUS(status);
}

int main(void)
{
    int bootstrap = isolated_child();
    if (bootstrap >= 0) return bootstrap;
    struct child *leader, *member, *liveparent, *liveself;
    struct child *deadparent, *deadself, *other, *target;
    struct result r;
    pid_t group, tid;
    int ret, error, status;
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = interrupted;
        sigemptyset(&sa.sa_mask);
        if (sigaction(SIGALRM, &sa, NULL) ||
            sigaction(SIGTERM, &sa, NULL) ||
            sigaction(SIGINT, &sa, NULL)) die("sigaction");
    }
    alarm(20);
    /* A dedicated supervisor session removes dependence on the launching shell. */
    if (setsid() < 0) die("supervisor setsid");
    leader = spawn(); member = spawn(); liveparent = spawn(); liveself = spawn();
    group = leader->pid;
    printf("supervisor=%ld sid=%ld leader=%ld\n", (long)getpid(),
        (long)getsid(0), (long)group);
    child_set("live leader create", leader, 0, 0);
    parent_set("populate group", member->pid, group, 0);
    parent_set("live leader parent join", liveparent->pid, group, 0);
    child_set("live leader child self join", liveself, group, 0);
    parent_set("same populated group", member->pid, group, 0);
    r = command(leader, 'T', 0); tid = r.tid;
    if (tid <= 0 || tid == group) die("nonleader tid");
    target = spawn();
    parent_set("nonleader TID destination", target->pid, tid, EPERM);
    reap(leader);
    errno = 0; ret = getpgid(group); error = errno;
    check("reaped leader getpgid", ret, error, ESRCH);
    errno = 0; ret = waitpid(group, &status, WNOHANG); error = errno;
    check("leader already reaped", ret, error, ECHILD);
    r = command(member, 'G', 0);
    printf("surviving member=%ld pgid=%ld expected=%ld %s\n", (long)member->pid,
        (long)r.pgid, (long)group, r.pgid == group ? "PASS" : "FAIL");
    if (r.pgid != group) failures++;
    deadparent = spawn(); deadself = spawn();
    parent_set("reaped leader parent join", deadparent->pid, group, 0);
    child_set("reaped leader child self join", deadself, group, 0);
    parent_set("same leaderless group", member->pid, group, 0);
    other = spawn();
    r = command(other, 'S', 0);
    if (r.ret != other->pid || r.error != 0) die("child setsid");
    parent_set("wrong destination session", target->pid, other->pid, EPERM);
    parent_set("wrong target session same group", other->pid, other->pid, EPERM);
    child_set("session leader same group", other, other->pid, EPERM);
    child_set("session leader zero group", other, 0, EPERM);
    parent_set("init destination other session", target->pid, 1, EPERM);
    parent_set("negative pid", -1, group, ESRCH);
    parent_set("negative pgid", target->pid, -1, EINVAL);
    reap(member); reap(liveparent); reap(liveself); reap(deadparent); reap(deadself);
    parent_set("empty reaped group nonexistent", target->pid, group, EPERM);
    child_set("create own group after refusal", target, 0, 0);
    child_set("same own group", target, target->pid, 0);
    reap(target); reap(other);
    alarm(0);
    cleanup();
    printf("RESULT failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
