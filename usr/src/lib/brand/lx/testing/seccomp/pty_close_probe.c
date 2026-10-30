/* CDDL version 1.0, http://www.illumos.org/license/CDDL. */
#define _GNU_SOURCE
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static int
one(unsigned index, int drain)
{
    char marker[64], received[256], *slave;
    int master=posix_openpt(O_RDWR|O_NOCTTY|O_NONBLOCK), status, available=-1;
    if(master<0 || grantpt(master)!=0 || unlockpt(master)!=0 ||
        (slave=ptsname(master))==NULL) {perror("open pty");return 2;}
    int length=snprintf(marker,sizeof(marker),"PTY_CLOSE_MARKER_%u",index);
    pid_t child=fork();
    if(child<0) return 2;
    if(child==0) {
        int fd=open(slave,O_RDWR|O_NOCTTY);
        if(fd<0) _exit(2);
        close(master);
        if(write(fd,marker,length)!=length) _exit(3);
        if(drain && tcdrain(fd)!=0) _exit(4);
        close(fd);_exit(0);
    }
    // Deliberately let the slave close completely before reading the master.
    if(waitpid(child,&status,0)!=child || !WIFEXITED(status) || WEXITSTATUS(status)!=0)
        return 2;
    int irc=ioctl(master,FIONREAD,&available), end_error=0;
    struct pollfd p={master,POLLIN,0};
    int prc=poll(&p,1,1000);
    size_t used=0;
    for(unsigned tries=0;tries<20 && used<sizeof(received)-1;tries++) {
        ssize_t n=read(master,received+used,sizeof(received)-1-used);
        if(n>0) {used+=n;continue;}
        if(n==0) break;
        end_error=errno;
        if(errno==EAGAIN) {poll(&p,1,20);continue;}
        if(errno==EIO) break;
        perror("read pty");close(master);return 2;
    }
    received[used]=0;
    int okay=used==(size_t)length && !memcmp(received,marker,length);
    printf("PTY_CLOSE_CASE i=%u drain=%d available=%d ioctl=%d poll=%d events=%#x bytes=%zu end_errno=%d found=%d\n",
        index,drain,available,irc,prc,p.revents,used,end_error,okay);
    close(master);return okay?0:1;
}
int
main(int argc,char **argv)
{
    unsigned count=argc>1?(unsigned)atoi(argv[1]):30, missing=0, errors=0;
    int drain=argc>2 && !strcmp(argv[2],"--drain");
    if(count<1 || count>200)return 2;
    setvbuf(stdout,NULL,_IOLBF,0);alarm(30);
    for(unsigned i=0;i<count;i++) {
        int result=one(i,drain);missing+=result==1;errors+=result==2;
        if(result==2)break;
    }
    printf("PTY_CLOSE_RESULT count=%u drain=%d missing=%u errors=%u\n",count,drain,missing,errors);
    return missing || errors;
}
