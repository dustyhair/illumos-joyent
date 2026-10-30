#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s errno=%d\n",__LINE__,#x,errno);return 1;}}while(0)
static int root_ids(void){return 0;}
static int stream_rights(void)
{
    CHECK(root_ids()==0);
    int pair[2],one=1; CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0);
    CHECK(setsockopt(pair[1],SOL_SOCKET,SO_PASSCRED,&one,sizeof(one))==0);
    char path[]="/tmp/lx-stream-fd-XXXXXX"; int file=mkstemp(path);
    CHECK(file>=0 && unlink(path)==0 && write(file,"Z",1)==1 && lseek(file,0,SEEK_SET)==0);
    union {struct cmsghdr align; char data[CMSG_SPACE(sizeof(int))];} control;
    memset(&control,0,sizeof(control)); char byte='x'; struct iovec iov={&byte,1};
    struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control.data,.msg_controllen=sizeof(control.data)};
    struct cmsghdr *c=CMSG_FIRSTHDR(&msg);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(file));memcpy(CMSG_DATA(c),&file,sizeof(file));
    CHECK(sendmsg(pair[0],&msg,0)==1);close(file);
    union {struct cmsghdr align;char data[256];} recvcontrol;
    msg.msg_control=recvcontrol.data;msg.msg_controllen=sizeof(recvcontrol.data);
    ssize_t received=recvmsg(pair[1],&msg,MSG_CMSG_CLOEXEC);
    fprintf(stderr,"FD_DIAG received=%ld flags=%#x controllen=%zu errno=%d\n",(long)received,msg.msg_flags,msg.msg_controllen,errno);
    CHECK(received==1 && !(msg.msg_flags&MSG_CTRUNC));
    unsigned rights=0,creds=0;
    for(c=CMSG_FIRSTHDR(&msg);c;c=CMSG_NXTHDR(&msg,c)) {
        CHECK(c->cmsg_level==SOL_SOCKET);
        if(c->cmsg_type==SCM_RIGHTS) {
            int fd;CHECK(c->cmsg_len==CMSG_LEN(sizeof(fd)));memcpy(&fd,CMSG_DATA(c),sizeof(fd));
            CHECK((fcntl(fd,F_GETFD)&FD_CLOEXEC)!=0 && read(fd,&byte,1)==1 && byte=='Z');close(fd);rights++;
        } else if(c->cmsg_type==SCM_CREDENTIALS) {
            struct ucred cr;CHECK(c->cmsg_len==CMSG_LEN(sizeof(cr)));memcpy(&cr,CMSG_DATA(c),sizeof(cr));
            CHECK(cr.pid==getpid() && cr.uid==0 && cr.gid==0);creds++;
        } else CHECK(0);
    }
    CHECK(rights==1 && creds==1);close(pair[0]);close(pair[1]);return 0;
}

int main(void){return stream_rights();}
