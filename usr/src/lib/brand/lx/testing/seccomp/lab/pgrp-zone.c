/* Reuse the leaderless fixture's direct-child pipe supervisor unchanged. */
#define main leaderless_reference_main
#include "leaderless-pgrp-base.c"
#undef main

static void
membership(const char *name, struct child *c, pid_t expected)
{
    struct result r = command(c, 'G', 0);
    int pass = r.pgid == expected;
    printf("%s pid=%ld pgid=%ld expected=%ld %s\n", name, (long)c->pid,
        (long)r.pgid, (long)expected, pass ? "PASS" : "FAIL");
    if (!pass) failures++;
}

static void
initialize(void)
{
    struct sigaction sa;
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = interrupted;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGALRM, &sa, NULL) || sigaction(SIGTERM, &sa, NULL) ||
        sigaction(SIGINT, &sa, NULL)) die("sigaction");
    alarm(180);
    if (setsid() < 0) die("setsid");
}

static int
peer(void)
{
    struct child *leader = spawn(), *member = spawn();
    pid_t group = leader->pid;
    char op;
    child_set("peer create", leader, 0, 0);
    parent_set("peer populate", member->pid, group, 0);
    if (failures) die("peer setup failed");
    printf("READY live %ld\n", (long)group);
    for (;;) {
        /* Commands are one byte. The global controller supplies no new IDs. */
        struct pollfd input = { STDIN_FILENO, POLLIN, 0 };
        if (poll(&input, 1, 60000) <= 0) {
            errno = ETIMEDOUT; die("peer command deadline");
        }
        transfer(STDIN_FILENO, &op, 1, 0);
        if (op == 'R' && leader->live) {
            reap(leader);
            errno = 0;
            int ret = getpgid(group), error = errno;
            check("peer leader reaped", ret, error, ESRCH);
            membership("peer retained after reap", member, group);
            if (failures) die("peer reap failed");
            printf("READY leaderless %ld\n", (long)group);
        } else if (op == 'C') {
            membership("peer intact", member, group);
            if (leader->live) membership("peer leader intact", leader, group);
            if (failures) die("peer membership failed");
            printf("INTACT %ld\n", (long)group);
        } else if (op == 'X') {
            reap(member);
            if (leader->live) reap(leader);
            cleanup();
            printf("RESULT failures=%d\n", failures);
            return failures != 0;
        } else { errno = EINVAL; die("peer command"); }
    }
}

static int
caller(pid_t foreign)
{
    struct child *own = spawn(), *target = spawn();
    pid_t original = getpgrp();
    alarm(20);
    printf("INPUT foreign_pgid=%ld caller=%ld sid=%ld\n", (long)foreign,
        (long)getpid(), (long)getsid(0));
    child_set("own group create", own, 0, 0);
    parent_set("foreign parent attempt", target->pid, foreign, EPERM);
    membership("target unchanged parent", target, original);
    child_set("foreign self attempt", target, foreign, EPERM);
    membership("target unchanged self", target, original);
    parent_set("own parent join", target->pid, own->pid, 0);
    membership("own parent membership", target, own->pid);
    child_set("own self return", target, original, 0);
    child_set("own self join", target, own->pid, 0);
    parent_set("foreign parent retry", target->pid, foreign, EPERM);
    child_set("foreign self retry", target, foreign, EPERM);
    membership("target retains own group", target, own->pid);
    printf("caller unchanged pgid=%ld expected=%ld %s\n", (long)getpgrp(),
        (long)original, getpgrp() == original ? "PASS" : "FAIL");
    if (getpgrp() != original) failures++;
    membership("own leader intact", own, own->pid);
    reap(target); reap(own); cleanup();
    printf("RESULT failures=%d\n", failures);
    return failures != 0;
}

int
main(int argc, char **argv)
{
    int bootstrap = isolated_child();
    if (bootstrap >= 0) return bootstrap;
    char *end;
    long input;
    if (argc == 2 && strcmp(argv[1], "reference") == 0)
        return leaderless_reference_main();
    initialize();
    if (argc == 2 && strcmp(argv[1], "peer") == 0) return peer();
    if (argc == 3 && strcmp(argv[1], "caller") == 0) {
        errno = 0;
        input = strtol(argv[2], &end, 10);
        if (errno || *end || input <= 1 || input > 2147483647L) {
            errno = EINVAL; die("foreign pgid input");
        }
        return caller((pid_t)input);
    }
    fprintf(stderr, "usage: %s reference | peer | caller PGID\n", argv[0]);
    return 2;
}
