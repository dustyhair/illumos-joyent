/* CDDL version 1.0, http://www.illumos.org/license/CDDL. */
#define _XOPEN_SOURCE 600
#define __EXTENSIONS__
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <ucred.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#define CHECK(x) do { if (!(x)) { perror(#x); exit(1); } } while (0)
static void receive(int fd, pid_t pid, uid_t uid)
{
    char byte; struct iovec iov = { &byte, 1 };
    union { struct cmsghdr align; char data[8192]; } buf;
    struct msghdr msg;
    struct cmsghdr *c;
    int found = 0;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov; msg.msg_iovlen = 1;
    msg.msg_control = buf.data; msg.msg_controllen = sizeof(buf.data);
    CHECK(recvmsg(fd, &msg, 0) == 1 && !(msg.msg_flags & MSG_CTRUNC));
    for (c = CMSG_FIRSTHDR(&msg); c != NULL; c = CMSG_NXTHDR(&msg, c)) {
        ucred_t *cr;
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_UCRED) continue;
        cr = (ucred_t *)CMSG_DATA(c);
        CHECK(ucred_getpid(cr) == pid && ucred_getruid(cr) == uid && ucred_geteuid(cr) == uid);
        found++;
    }
    CHECK(found == 1);
}
int main(void)
{
    alarm(20);
    int pair[2], one = 1, status;
    pid_t child;
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    CHECK(setsockopt(pair[1], SOL_SOCKET, SO_RECVUCRED, &one, sizeof(one)) == 0);
    child = fork(); CHECK(child >= 0);
    if (child == 0) {
        close(pair[1]); CHECK(write(pair[0], "a", 1) == 1);
        CHECK(setgid(1001) == 0 && setuid(1001) == 0);
        CHECK(write(pair[0], "b", 1) == 1); _exit(0);
    }
    close(pair[0]); receive(pair[1], child, getuid()); receive(pair[1], child, 1001);
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(pair[1]); puts("NATIVE_STREAM_CRED_PASS");
    char cwd[4096], path[]="/tmp/native-rmdir-XXXXXX";
    CHECK(getcwd(cwd,sizeof(cwd)) != NULL && mkdtemp(path) != NULL);
    CHECK(chdir(path) == 0); errno=0;
    CHECK(rmdir(path) == -1 && errno == EINVAL);
    CHECK(chdir(cwd) == 0 && rmdir(path) == 0);
    puts("NATIVE_RMDIR_CWD_PASS"); return (0);
}
