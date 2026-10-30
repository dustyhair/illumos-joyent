/* CDDL version 1.0, http://www.illumos.org/license/CDDL. */
#define __EXTENSIONS__
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/lx_brand.h>
#include <zone.h>
#include <priv.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "line %d: %s errno=%d\n", __LINE__, #x, errno); \
    goto out; } } while (0)
static pid_t wait_child(pid_t pid, int *status)
{
    pid_t got;
    do { got = waitpid(pid, status, 0); } while (got < 0 && errno == EINTR);
    return got;
}
static int wait_ok(pid_t *child)
{
    int status;
    pid_t got = wait_child(*child, &status);
    if (got != *child) return -1;
    *child = -1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}
static pid_t run_test(const char *test)
{
    pid_t pid = fork();
    if (pid == 0) {
        execl("/lab/ktest", "ktest", "run", test, (char *)NULL);
        _exit(127);
    }
    return pid;
}
static int get(zoneid_t zone, int attr, uint32_t *value)
{
    return zone_getattr(zone, attr, value, sizeof(*value)) == (ssize_t)sizeof(*value) ? 0 : -1;
}
static int set(zoneid_t zone, uint32_t value)
{
    return zone_setattr(zone, LX_ATTR_USERNS_MAX, &value, sizeof(value));
}
int main(int argc, char **argv)
{
    zoneid_t zone;
    uint32_t original = 0, maximum, used, value = 9;
    int restore = 0, result = 1, status;
    pid_t child = -1;
    CHECK(argc == 3 && getzoneid() == GLOBAL_ZONEID);
    CHECK(!strcmp(argv[1], "--ready") || !strcmp(argv[1], "--running"));
    zone = getzoneidbyname(argv[2]);
    CHECK(zone > GLOBAL_ZONEID);
    CHECK(get(zone, LX_ATTR_USERNS_MAX, &original) == 0);
    CHECK(get(zone, LX_ATTR_USERNS_USED, &used) == 0 && used == 0);
    if (!strcmp(argv[1], "--running")) {
        CHECK(original == 64);
        errno = 0;
        CHECK(set(zone, 999) == -1 && errno == EINVAL);
        CHECK(get(zone, LX_ATTR_USERNS_MAX, &maximum) == 0 && maximum == original);
        printf("LX_USERNS_BUDGET_RUNNING_PASS max=%u\n", original);
        return 0;
    }
    restore = 1;
    CHECK(original > 0);
    errno = 0;
    CHECK(zone_getattr(zone, LX_ATTR_USERNS_MAX, &value, 1) == -1 && errno == ERANGE);
    errno = 0;
    CHECK(zone_getattr(zone, LX_ATTR_USERNS_USED, NULL, sizeof(value)) == -1 && errno == EFAULT);
    errno = 0;
    CHECK(zone_setattr(zone, LX_ATTR_USERNS_MAX, &value, 1) == -1 && errno == ERANGE);
    errno = 0;
    CHECK(zone_setattr(zone, LX_ATTR_USERNS_MAX, NULL, sizeof(value)) == -1 && errno == EFAULT);
    errno = 0;
    CHECK(zone_setattr(zone, LX_ATTR_USERNS_USED, &value, sizeof(value)) == -1 && errno == EINVAL);
    child = fork(); CHECK(child >= 0);
    if (child == 0) {
        priv_set_t *empty = priv_allocset();
        if (empty == NULL) _exit(2);
        priv_emptyset(empty);
        if (setppriv(PRIV_SET, PRIV_EFFECTIVE, empty) ||
            setppriv(PRIV_SET, PRIV_PERMITTED, empty) ||
            setppriv(PRIV_SET, PRIV_LIMIT, empty)) _exit(3);
        errno = 0;
        _exit(set(zone, 999) == -1 && errno == EPERM ? 0 : 4);
    }
    CHECK(wait_ok(&child) == 0);
    child = -1;
    CHECK(get(zone, LX_ATTR_USERNS_MAX, &maximum) == 0 && maximum == original);
    CHECK(set(zone, 0) == 0);
    child = run_test("lx_namespace:user:zone_budget_disabled"); CHECK(child > 0);
    CHECK(wait_ok(&child) == 0);
    child = -1;
    CHECK(set(zone, 2) == 0);
    child = run_test("lx_namespace:user:zone_budget_hold"); CHECK(child > 0);
    unsigned attempt;
    for (attempt = 0; attempt < 200; attempt++) {
        CHECK(get(zone, LX_ATTR_USERNS_USED, &used) == 0);
        if (used == 2) break;
        usleep(50000);
    }
    CHECK(used == 2 && get(zone, LX_ATTR_USERNS_MAX, &maximum) == 0 && maximum == 2);
    CHECK(set(zone, 1) == 0);
    CHECK(wait_ok(&child) == 0);
    child = -1;
    CHECK(get(zone, LX_ATTR_USERNS_USED, &used) == 0 && used == 0);
    CHECK(get(zone, LX_ATTR_USERNS_MAX, &maximum) == 0 && maximum == 1);
    CHECK(set(zone, original) == 0);
    result = 0;
    printf("LX_USERNS_BUDGET_READY_PASS restored=%u\n", original);
out:
    if (restore) (void)set(zone, original);
    if (child > 0) {
        (void)kill(child, SIGKILL);
        (void)wait_child(child, &status);
    }
    return result;
}
