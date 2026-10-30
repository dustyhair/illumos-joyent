/* CDDL version 1.0, http://www.illumos.org/license/CDDL. */
#define __EXTENSIONS__
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
int main(int argc,char **argv)
{
    if(argc!=3)return 2;
    int fd=open(argv[1],O_RDONLY);if(fd<0){perror("open");return 1;}
    alarm(25);unsigned cycles=0;
    while(access(argv[2],F_OK)!=0) {
        if(fchown(fd,1001,1001)!=0){perror("mapped fchown");return 1;}
        usleep(1000);
        if(fchown(fd,2000,2000)!=0){perror("unmapped fchown");return 1;}
        usleep(3000);cycles++;
    }
    close(fd);printf("NATIVE_CHOWN_RACER_PASS cycles=%u\n",cycles);return 0;
}
