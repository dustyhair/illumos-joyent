/* CDDL version 1.0, http://www.illumos.org/license/CDDL. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/capability.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s errno=%d\n",__LINE__,#x,errno);return 1;}} while(0)
static const char *self;
static int linux_control,restricted,cases,failures;
static uint64_t caps(void)
{
    struct __user_cap_header_struct h={_LINUX_CAPABILITY_VERSION_3,0};
    struct __user_cap_data_struct d[2];
    if(syscall(SYS_capget,&h,d))return UINT64_MAX;
    return d[0].effective|((uint64_t)d[1].effective<<32);
}
static int put(const char *path,const char *text)
{
    int fd=open(path,O_WRONLY|O_CLOEXEC);if(fd<0)return -1;
    ssize_t n=write(fd,text,strlen(text));int error=errno;close(fd);errno=error;
    return n==(ssize_t)strlen(text)?0:-1;
}
static int map_pid(pid_t pid,unsigned uid,unsigned gid)
{
    char path[96],text[80];
    snprintf(path,sizeof(path),"/proc/%d/setgroups",pid);CHECK(put(path,"deny\n")==0);
    snprintf(path,sizeof(path),"/proc/%d/uid_map",pid);snprintf(text,sizeof(text),"0 %u 1\n",uid);
    CHECK(put(path,text)==0);
    snprintf(path,sizeof(path),"/proc/%d/gid_map",pid);snprintf(text,sizeof(text),"0 %u 1\n",gid);
    CHECK(put(path,text)==0);return 0;
}
struct state {unsigned uid,gid;uint64_t caps;int nnp,filter,tid;};
struct context {int ready[2],release[2],mode,*ctid;};
static int image(int filter)
{
    CHECK(geteuid()==0 && getegid()==0 && prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)==1);
    CHECK(prctl(PR_GET_SECCOMP,0,0,0,0)==filter);return 0;
}
static int child_main(void *arg)
{
    struct context *c=arg;
    struct state record={geteuid(),getegid(),caps(),prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0),
        prctl(PR_GET_SECCOMP,0,0,0,0),c->ctid?*c->ctid:0};
    CHECK(write(c->ready[1],&record,sizeof(record))==sizeof(record));
    char byte;CHECK(read(c->release[0],&byte,1)==1);
    CHECK(geteuid()==0 && getegid()==0);
    if(c->mode==1) {
        char *const args[]={(char *)self,"--image",record.filter?"2":"0",NULL};
        syscall(SYS_execve,self,args,NULL);return 1;
    }
    if(c->mode==2) {
        CHECK(syscall(SYS_unshare,CLONE_NEWUSER)==0);
        CHECK(geteuid()==65534 && map_pid(getpid(),0,0)==0);
    }
    if(c->mode==4) {
        char path[96];snprintf(path,sizeof(path),"/proc/%d/maps",getpid());
        for(int dumpable=1;dumpable>=0;dumpable--) {
            CHECK(prctl(PR_SET_DUMPABLE,dumpable,0,0,0)==0);
            pid_t peer=fork();int status;CHECK(peer>=0);
            if(peer==0) {
                errno=0;int fd=open(path,O_RDONLY|O_CLOEXEC),error=errno;
                if(fd>=0)close(fd);
                _exit(dumpable ? fd<0 : !(fd<0 && (error==EPERM || error==EACCES)));
            }
            CHECK(waitpid(peer,&status,0)==peer && WIFEXITED(status) && WEXITSTATUS(status)==0);
        }
    }
    if(c->mode==3) {
        char *const args[]={"/native/usr/bin/true",NULL};errno=0;
        CHECK(syscall(SYS_execve,args[0],args,NULL)==-1 && errno==ENOEXEC);
        long value=230;__asm__ volatile("int $0x91":"+a"(value)::"cc","memory");return 1;
    }
    return 0;
}
static int native_parent(void)
{
    if(linux_control)return 0;
    pid_t child=fork();int status;CHECK(child>=0);
    if(child==0) {
        char *const args[]={"/native/usr/bin/true",NULL};
        syscall(SYS_execve,args[0],args,NULL);_exit(1);
    }
    CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);return 0;
}
static int create(int mode,int raw,int tids,int ordinary_policy,int check_native)
{
    struct context c={.mode=mode};CHECK(pipe(c.ready)==0 && pipe(c.release)==0);
    struct state parent={geteuid(),getegid(),caps(),prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0),
        prctl(PR_GET_SECCOMP,0,0,0,0),0};
    int ptid=-1,ctid=-1;c.ctid=tids?&ctid:NULL;
    unsigned flags=CLONE_NEWUSER|SIGCHLD|(tids?CLONE_PARENT_SETTID|CLONE_CHILD_SETTID:0);
    char *stack=NULL;pid_t child;
    if(raw) {
        child=syscall(SYS_clone,flags,0,&ptid,&ctid,0);
        if(child==0)_exit(child_main(&c));
    } else {
        stack=malloc(131072);CHECK(stack!=NULL);
        child=clone(child_main,stack+131072,flags,&c,&ptid,NULL,&ctid);
    }
    CHECK(child>0);
    CHECK(geteuid()==parent.uid && getegid()==parent.gid && caps()==parent.caps);
    CHECK(prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)==parent.nnp);
    CHECK(prctl(PR_GET_SECCOMP,0,0,0,0)==parent.filter);
    struct state before;CHECK(read(c.ready[0],&before,sizeof(before))==sizeof(before));
    CHECK(before.uid==65534 && before.gid==65534);
    if(ordinary_policy && linux_control && restricted && before.caps==0)
        puts("CONTROL_POLICY ordinary clone child has zero namespace capabilities");
    else CHECK((before.caps&((1ULL<<41)-1))==((1ULL<<41)-1));
    CHECK(before.nnp==parent.nnp && before.filter==parent.filter);
    if(tids)CHECK(ptid==child && before.tid==child && ctid==-1);
    char path[96],text[64];snprintf(path,sizeof(path),"/proc/%d/uid_map",child);
    int fd=open(path,O_RDONLY|O_CLOEXEC);CHECK(fd>=0);
    CHECK(read(fd,text,sizeof(text))==0);close(fd);
    CHECK(map_pid(child,parent.uid,parent.gid)==0);
    CHECK(write(c.release[1],"x",1)==1);int status;
    CHECK(waitpid(child,&status,0)==child);
    if(mode==3) CHECK(WIFSIGNALED(status) && WTERMSIG(status)==SIGSYS);
    else CHECK(WIFEXITED(status) && WEXITSTATUS(status)==0);
    close(c.ready[0]);close(c.ready[1]);close(c.release[0]);close(c.release[1]);free(stack);
    CHECK(geteuid()==parent.uid && getegid()==parent.gid && caps()==parent.caps);
    if(check_native)CHECK(native_parent()==0);
    return 0;
}
static int basic(void){return create(0,0,0,0,1);}
static int raw_clone(void){return create(0,1,0,0,1);}
static int repeat_clone(void){CHECK(basic()==0);return basic();}
static int tid_clone(void){return create(0,0,1,0,1);}
static int ordinary(void)
{
    CHECK(syscall(SYS_setgroups,0,NULL)==0);
    CHECK(syscall(SYS_setresgid,1000,1000,1000)==0 && syscall(SYS_setresuid,1000,1000,1000)==0);
    CHECK(prctl(PR_SET_DUMPABLE,1,0,0,0)==0 && caps()==0);
    return create(0,0,0,1,1);
}
static int nested_parent(void)
{
    unsigned uid=geteuid(),gid=getegid();
    CHECK(syscall(SYS_unshare,CLONE_NEWUSER)==0 && map_pid(getpid(),uid,gid)==0);
    return create(0,0,0,0,0);
}
static int nested_child(void){return create(2,0,0,0,1);}
static int exec_child(void)
{
    CHECK(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)==0);return create(1,0,0,0,1);
}
static int filtered(void)
{
    struct sock_filter code={BPF_RET|BPF_K,0,0,SECCOMP_RET_ALLOW};struct sock_fprog prog={1,&code};
    CHECK(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)==0);
    CHECK(prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&prog,0,0)==0);
    return create(1,0,0,0,0);
}
static int guard(void){return create(3,0,0,0,1);}
static int memory_owner(void){return create(4,0,0,0,1);}
static int rejected(int expected)
{
    unsigned uid=geteuid();uint64_t before=caps();errno=0;
    CHECK(syscall(SYS_clone,CLONE_NEWUSER|SIGCHLD,0,0,0,0)==-1 && errno==expected);
    CHECK(geteuid()==uid && caps()==before);return native_parent();
}
static int invalid(void)
{
    const uint64_t flags[]={CLONE_NEWUSER|CLONE_FS|SIGCHLD,
        CLONE_NEWUSER|CLONE_THREAD|SIGCHLD,CLONE_NEWUSER|CLONE_NEWNS|SIGCHLD,
        CLONE_NEWUSER|(1ULL<<40)|SIGCHLD};
    unsigned uid=geteuid();uint64_t before=caps();
    for(unsigned i=0;i<sizeof(flags)/sizeof(flags[0]);i++) {
        errno=0;CHECK(syscall(SYS_clone,flags[i],0,0,0,0)==-1 && errno==EINVAL);
        CHECK(geteuid()==uid && caps()==before);
    }
    return native_parent();
}
static void run(const char *name,int (*fn)(void))
{
    fflush(NULL);pid_t child=fork();int status=0;if(child==0)_exit(fn());
    int ok=child>0 && waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0;
    printf("%s %s status=%#x\n",ok?"PASS":"FAIL",name,status);cases++;failures+=!ok;
}
int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IOLBF,0);self=argv[0];if(argc<2)return 2;
    linux_control=!strcmp(argv[1],"--linux");
    if(argc==3 && !strcmp(argv[1],"--image"))return image(atoi(argv[2]));
    if(!strcmp(argv[1],"--disabled"))return rejected(EINVAL);
    if(!strcmp(argv[1],"--budget-zero"))return rejected(ENOSPC);
    if(linux_control) {
        FILE *f=fopen("/proc/sys/kernel/apparmor_restrict_unprivileged_userns","r");
        if(f){if(fscanf(f,"%d",&restricted)!=1)restricted=0;fclose(f);}
    } else if(strcmp(argv[1],"--enabled"))return 2;
    run("namespace child and unchanged native-capable parent",basic);
    run("raw clone preserves the copied Linux stack",raw_clone);
    run("repeated clones release the parent pending credential",repeat_clone);
    run("parent and child TID stores use their own address spaces",tid_clone);
    run("ordinary parent brokers its child mapping",ordinary);
    run("namespace parent creates a fresh descendant",nested_parent);
    run("clone child can enter another namespace",nested_child);
    run("namespace child retains NNP across exec",exec_child);
    run("namespace clone retains seccomp across exec",filtered);
    run("cloned address space retains its older owner namespace",memory_owner);
    if(!linux_control) {
        run("clone child rejects native exec and native syscall entry",guard);
        run("unsupported clone combinations preserve the parent",invalid);
    }
    printf("USERNS_CLONE cases=%d failures=%d\n",cases,failures);return failures!=0;
}
