/* CDDL version 1.0, http://www.illumos.org/license/CDDL. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/capability.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do { if(!(x)) {fprintf(stderr,"line %d: %s errno=%d\n",__LINE__,#x,errno);return 1;} } while(0)
static const char *self;
static int linux_control,linux_restricted,cases,failures;
static atomic_int ready,finish;
static uint64_t capbits(void)
{
    struct __user_cap_header_struct h={_LINUX_CAPABILITY_VERSION_3,0};
    struct __user_cap_data_struct d[2];
    if(syscall(SYS_capget,&h,d)) return UINT64_MAX;
    return d[0].effective|((uint64_t)d[1].effective<<32);
}
static int put(const char *path,const char *text)
{
    int fd=open(path,O_WRONLY|O_CLOEXEC);if(fd<0)return -1;
    ssize_t count=write(fd,text,strlen(text));int error=errno;
    close(fd);errno=error;return count==(ssize_t)strlen(text)?0:-1;
}
static int map_self(unsigned uid,unsigned gid)
{
    char text[80];
    CHECK(put("/proc/self/setgroups","deny\n")==0);
    snprintf(text,sizeof(text),"0 %u 1\n",uid);
    CHECK(put("/proc/self/uid_map",text)==0);
    snprintf(text,sizeof(text),"0 %u 1\n",gid);
    CHECK(put("/proc/self/gid_map",text)==0);
    CHECK(getuid()==0 && geteuid()==0 && getgid()==0 && getegid()==0);
    return 0;
}
static int enter(void)
{
    unsigned uid=geteuid(),gid=getegid();
    CHECK(syscall(SYS_unshare,CLONE_NEWUSER)==0);
    CHECK(getuid()==65534 && geteuid()==65534 && getgid()==65534 && getegid()==65534);
    uint64_t caps=capbits();
    if((caps&((1ULL<<41)-1))!=((1ULL<<41)-1)) fprintf(stderr,"ENTRY_CAPS=%#llx\n",(unsigned long long)caps);
    CHECK((caps&((1ULL<<41)-1))==((1ULL<<41)-1));
    CHECK(prctl(PR_GET_SECUREBITS,0,0,0,0)==0);
    return map_self(uid,gid);
}
static int basic(void)
{
    char path[]="/tmp/lx-userns-entry.XXXXXX";int fd=mkstemp(path);CHECK(fd>=0);
    CHECK(unlink(path)==0);unsigned uid=geteuid(),gid=getegid();
    CHECK(enter()==0);struct stat st;CHECK(fstat(fd,&st)==0 && st.st_uid==0);
    CHECK(write(fd,"ok",2)==2);close(fd);
    errno=0;CHECK(syscall(SYS_setgroups,0,NULL)==-1 && errno==EPERM);
    CHECK(geteuid()==0 && getegid()==0);
    printf("ENTRY_BACKING_UID=%u GID=%u\n",uid,gid);return 0;
}
static int ordinary(void)
{
    CHECK(syscall(SYS_setgroups,0,NULL)==0);
    CHECK(syscall(SYS_setresgid,1000,1000,1000)==0 && syscall(SYS_setresuid,1000,1000,1000)==0);
    CHECK(capbits()==0);
    if(linux_control && linux_restricted) {
        CHECK(syscall(SYS_unshare,CLONE_NEWUSER)==0);
        if(capbits()==0) {
            errno=0;CHECK(put("/proc/self/uid_map","0 1000 1\n")==-1 && (errno==EPERM || errno==EACCES));
            CHECK(geteuid()==65534);
            puts("CONTROL_POLICY ordinary userns has zero capabilities and cannot self-map");
            return 0;
        }
        CHECK(map_self(1000,1000)==0);
    }
    return basic();
}
static int fs_flag(void)
{
    unsigned uid=geteuid(),gid=getegid();
    CHECK(syscall(SYS_unshare,CLONE_NEWUSER|CLONE_FS)==0);
    return map_self(uid,gid);
}
static int empty_parent(void)
{
    unsigned uid=geteuid(),gid=getegid();CHECK(syscall(SYS_unshare,CLONE_NEWUSER)==0);
    uint64_t before=capbits();errno=0;
    CHECK(syscall(SYS_unshare,CLONE_NEWUSER)==-1 && errno==EPERM);
    CHECK(geteuid()==65534 && capbits()==before);
    return map_self(uid,gid);
}
static int nested(void)
{
    CHECK(enter()==0);return enter();
}
static int depth(void)
{
    for(unsigned i=0;i<33;i++) CHECK(enter()==0);
    uint64_t before=capbits();errno=0;
    CHECK(syscall(SYS_unshare,CLONE_NEWUSER)==-1 && errno==ENOSPC);
    CHECK(geteuid()==0 && getegid()==0 && capbits()==before);return 0;
}
static int image(void)
{
    CHECK(geteuid()==0 && getegid()==0);
    CHECK(prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)==1);
    char text[128]={0};int fd=open("/proc/self/uid_map",O_RDONLY|O_CLOEXEC);CHECK(fd>=0);
    CHECK(read(fd,text,sizeof(text)-1)>0);close(fd);
    unsigned inside,outside,count;CHECK(sscanf(text,"%u %u %u",&inside,&outside,&count)==3);
    CHECK(inside==0 && count==1);return 0;
}
static int inheritance(void)
{
    CHECK(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)==0 && enter()==0);
    pid_t child=fork();int status;CHECK(child>=0);
    if(child==0) {
        if(image()!=0) _exit(1);
        char *const args[]={(char *)self,"--image",NULL};
        syscall(SYS_execve,self,args,NULL);_exit(1);
    }
    CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    return image();
}
static int memory_owner(void)
{
    CHECK(enter()==0);
    pid_t parent=getpid();char path[80];snprintf(path,sizeof(path),"/proc/%d/maps",parent);
    for(int dumpable=1;dumpable>=0;dumpable--) {
        CHECK(prctl(PR_SET_DUMPABLE,dumpable,0,0,0)==0);
        int own=open("/proc/self/maps",O_RDONLY|O_CLOEXEC);CHECK(own>=0);close(own);
        pid_t child=fork();int status;CHECK(child>=0);
        if(child==0) {
            if(!(capbits()&(1ULL<<CAP_SYS_PTRACE))) _exit(1);
            errno=0;int fd=open(path,O_RDONLY|O_CLOEXEC),error=errno;
            if(fd>=0) close(fd);
            _exit(dumpable ? fd<0 : !(fd<0 && (error==EPERM || error==EACCES)));
        }
        CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    }
    CHECK(prctl(PR_SET_DUMPABLE,1,0,0,0)==0);return 0;
}
static void *worker(void *unused)
{
    (void)unused;atomic_store(&ready,1);
    while(!atomic_load(&finish)) sched_yield();
    return NULL;
}
static int threads_case(void)
{
    pthread_t thread;CHECK(pthread_create(&thread,NULL,worker,NULL)==0);
    while(!atomic_load(&ready)) sched_yield();
    unsigned uid=geteuid(),gid=getegid();uint64_t caps=capbits();errno=0;
    int result=syscall(SYS_unshare,CLONE_NEWUSER),error=errno;
    atomic_store(&finish,1);CHECK(pthread_join(thread,NULL)==0);
    CHECK(result==-1 && error==EINVAL);
    CHECK(geteuid()==uid && getegid()==gid && capbits()==caps);return 0;
}
static int scoped_threads(void)
{
    CHECK(enter()==0);return threads_case();
}
static int chroot_case(void)
{
    char path[]="/tmp/lx-userns-chroot.XXXXXX";CHECK(mkdtemp(path)!=NULL);
    int root=open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC);CHECK(root>=0);
    CHECK(chroot(path)==0);unsigned uid=geteuid();uint64_t before=capbits();errno=0;
    CHECK(syscall(SYS_unshare,CLONE_NEWUSER)==-1 && errno==EPERM);
    CHECK(geteuid()==uid && capbits()==before);
    CHECK(fchdir(root)==0 && chroot(".")==0);close(root);CHECK(rmdir(path)==0);return 0;
}
static int filtered(void)
{
    struct sock_filter code={BPF_RET|BPF_K,0,0,SECCOMP_RET_ALLOW};
    struct sock_fprog prog={1,&code};
    CHECK(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)==0);
    CHECK(prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&prog,0,0)==0);
    CHECK(enter()==0 && prctl(PR_GET_SECCOMP,0,0,0,0)==2);
    return image();
}
static int native_guard(void)
{
    CHECK(!linux_control && enter()==0);
    char *const args[]={"/native/usr/bin/true",NULL};errno=0;
    CHECK(syscall(SYS_execve,args[0],args,NULL)==-1 && errno==ENOEXEC);
    CHECK(prctl(PR_GET_SECCOMP,0,0,0,0)==0);
    pid_t child=fork();int status;CHECK(child>=0);
    if(child==0) {
        long value=230;__asm__ volatile("int $0x91":"+a"(value)::"cc","memory");_exit(1);
    }
    CHECK(waitpid(child,&status,0)==child && WIFSIGNALED(status) && WTERMSIG(status)==SIGSYS);
    return 0;
}
static int wide_flags(void)
{
    unsigned uid=geteuid();uint64_t caps=capbits();errno=0;
    CHECK(syscall(SYS_unshare,(1ULL<<40)|CLONE_NEWUSER)==-1 && errno==EINVAL);
    CHECK(geteuid()==uid && capbits()==caps);return 0;
}
static int unsupported_flags(void)
{
    unsigned uid=geteuid();uint64_t caps=capbits();
    const unsigned flags[]={CLONE_NEWUSER|CLONE_NEWNS,CLONE_NEWUSER|CLONE_NEWNET,CLONE_NEWUSER|SIGCHLD};
    for(unsigned i=0;i<sizeof(flags)/sizeof(flags[0]);i++) {
        errno=0;CHECK(syscall(SYS_unshare,flags[i])==-1 && errno==EINVAL);
        CHECK(geteuid()==uid && capbits()==caps);
    }
    return 0;
}
static int fs_peer(void *arg)
{
    int fd=*(int *)arg;char byte;
    return read(fd,&byte,1)==1?0:1;
}
static int shared_fs(void)
{
    int fds[2];CHECK(pipe(fds)==0);
    char *stack=malloc(131072);CHECK(stack!=NULL);
    pid_t child=clone(fs_peer,stack+131072,CLONE_FS|SIGCHLD,&fds[0]);CHECK(child>0);
    unsigned uid=geteuid();uint64_t caps=capbits();errno=0;
    int result=syscall(SYS_unshare,CLONE_NEWUSER),error=errno;
    CHECK(write(fds[1],"x",1)==1);int status;
    CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    close(fds[0]);close(fds[1]);free(stack);
    CHECK(result==-1 && error==EOPNOTSUPP && geteuid()==uid && capbits()==caps);return 0;
}
static int denied(int expected)
{
    unsigned uid=geteuid(),gid=getegid();uint64_t caps=capbits();errno=0;
    CHECK(syscall(SYS_unshare,CLONE_NEWUSER)==-1 && errno==expected);
    CHECK(geteuid()==uid && getegid()==gid && capbits()==caps);
    if(access("/native/usr/bin/uname",X_OK)==0) {
        pid_t child=fork();int status;CHECK(child>=0);
        if(child==0) {
            char *const args[]={"/native/usr/bin/true",NULL};
            syscall(SYS_execve,args[0],args,NULL);_exit(1);
        }
        CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    }
    return 0;
}
static void run(const char *name,int (*fn)(void))
{
    fflush(NULL);pid_t child=fork();int status=0;
    if(child==0) _exit(fn());
    int ok=child>0 && waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0;
    printf("%s %s status=%#x\n",ok?"PASS":"FAIL",name,status);cases++;failures+=!ok;
}
int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IOLBF,0);
    self=argv[0];if(argc!=2)return 2;
    if(!strcmp(argv[1],"--image"))return image();
    if(!strcmp(argv[1],"--disabled"))return denied(EPERM);
    if(!strcmp(argv[1],"--budget-zero"))return denied(ENOSPC);
    linux_control=!strcmp(argv[1],"--linux");
    if(!linux_control && strcmp(argv[1],"--enabled"))return 2;
    if(linux_control) {
        FILE *f=fopen("/proc/sys/kernel/apparmor_restrict_unprivileged_userns","r");
        if(f!=NULL) {if(fscanf(f,"%d",&linux_restricted)!=1)linux_restricted=0;fclose(f);}
    }
    run("initial root enters and maps its namespace",basic);
    run(linux_control?"ordinary user entry follows local LSM policy":"ordinary user enters without parent capabilities",ordinary);
    run("explicit filesystem unshare flag",fs_flag);
    run("unmapped parent rejects another entry atomically",empty_parent);
    run("nested namespace maps parent-relative identities",nested);
    run("namespace depth limit preserves current credentials",depth);
    run("fork and exec preserve namespace and NNP",inheritance);
    run("entry retains the older address-space owner namespace",memory_owner);
    run("multithreaded entry rejects without mutation",threads_case);
    run("nested multithreaded entry rejects without mutation",scoped_threads);
    run("chroot entry is denied",chroot_case);
    run("reserved high flag bits are rejected without truncation",wide_flags);
    run("entry retains an existing seccomp filter",filtered);
    if(!linux_control) {
        run("namespace guard denies native entry without seccomp",native_guard);
        run("shared filesystem clone group is rejected without mutation",shared_fs);
        run("unsupported combined namespaces do not mutate credentials",unsupported_flags);
    }
    printf("USERNS_ENTRY cases=%d failures=%d\n",cases,failures);return failures!=0;
}
