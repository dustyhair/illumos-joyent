/* CDDL version 1.0, http://www.illumos.org/license/CDDL. */
#define _GNU_SOURCE
#include <sys/stat.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
    char home[512], cache[512];
    if (argc < 2 || argc > 3 ||
        strncmp(argv[1], "/var/tmp/lx-npm-install-", 24) != 0)
        return 2;
    if (argc == 3) {
        if (strcmp(argv[2], "--drop") != 0 || setgroups(0, NULL) != 0 ||
            setgid(65534) != 0 || setuid(65534) != 0)
            return 2;
    }
    if (snprintf(home, sizeof(home), "%s/home", argv[1]) >= (int)sizeof(home) ||
        snprintf(cache, sizeof(cache), "%s/cache", argv[1]) >= (int)sizeof(cache))
        return 2;
    if (clearenv() != 0 || chdir(argv[1]) != 0)
        return 2;
    if (setenv("HOME", home, 1) != 0 || setenv("CODEX_HOME", home, 1) != 0 ||
        setenv("npm_config_cache", cache, 1) != 0 ||
        setenv("PATH", "/opt/lx-zone-install/runtime/bin:/usr/bin:/bin", 1) != 0 ||
        setenv("LD_LIBRARY_PATH", "/opt/lx-zone-install/runtime/lib", 1) != 0)
        return 2;
    umask(077);
    printf("NPM_INSTALL_UID=%u GID=%u WORK=%s NO_AUTH_ENV=1\n",
        getuid(), getgid(), argv[1]);
    fflush(stdout);
    alarm(90);
    execl("/bin/sh", "sh", "/opt/lx-zone-install/npm-install-case.sh",
        argv[1], NULL);
    perror("exec npm install case");
    return 1;
}
