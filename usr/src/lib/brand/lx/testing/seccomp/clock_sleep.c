/* CDDL version 1.0, http://www.illumos.org/license/CDDL. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef SYS_rseq
#define SYS_rseq 334
#endif
#ifndef SYS_faccessat2
#define SYS_faccessat2 439
#endif
#ifndef PR_CAPBSET_READ
#define PR_CAPBSET_READ 23
#endif

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s errno=%d\n",__LINE__,#x,errno); return 1; } } while (0)
#define LX_TEST_SA_EXPOSE_TAGBITS 0x00000800UL
struct lx_test_sigaction {
    uintptr_t handler;
    unsigned long flags;
    uintptr_t restorer;
    uint64_t mask;
};
static unsigned cases, failures;
static volatile sig_atomic_t caught;
extern char **environ;
static void handler(int sig) { (void)sig; caught++; }
static int64_t mono(void)
{
    struct timespec t;
    if (syscall(SYS_clock_gettime,CLOCK_MONOTONIC,&t)!=0) return -1;
    return (int64_t)t.tv_sec*1000000000+t.tv_nsec;
}
static int rawsleep(int clock, int flags, const struct timespec *request,
    struct timespec *remaining)
{
    return syscall(SYS_clock_nanosleep,clock,flags,request,remaining);
}
static int validation(void)
{
    struct timespec t={0,0}, rem={123,456};
    CHECK(rawsleep(255,0,(void *)1,NULL)==-1 && errno==EINVAL);
    CHECK(rawsleep(CLOCK_THREAD_CPUTIME_ID,0,&t,NULL)==-1 && errno==EOPNOTSUPP);
    CHECK(rawsleep(CLOCK_MONOTONIC_RAW,0,(void *)1,NULL)==-1 && errno==EOPNOTSUPP);
    CHECK(rawsleep(CLOCK_REALTIME_COARSE,0,&t,NULL)==-1 && errno==EOPNOTSUPP);
    CHECK(rawsleep(CLOCK_MONOTONIC_COARSE,0,&t,NULL)==-1 && errno==EOPNOTSUPP);
    CHECK(rawsleep(CLOCK_MONOTONIC,0,(void *)1,NULL)==-1 && errno==EFAULT);
    CHECK(rawsleep(CLOCK_MONOTONIC,0,NULL,NULL)==-1 && errno==EFAULT);
    t.tv_sec=-1;
    CHECK(rawsleep(CLOCK_MONOTONIC,0,&t,NULL)==-1 && errno==EINVAL);
    t.tv_sec=0;t.tv_nsec=-1;
    CHECK(rawsleep(CLOCK_MONOTONIC,0,&t,NULL)==-1 && errno==EINVAL);
    t.tv_nsec=1000000000;
    CHECK(rawsleep(CLOCK_MONOTONIC,0,&t,NULL)==-1 && errno==EINVAL);
    t.tv_nsec=0;
    CHECK(rawsleep(CLOCK_MONOTONIC,2,&t,&rem)==0);
    CHECK(rem.tv_sec==123 && rem.tv_nsec==456);
    return 0;
}
static int relative(int clock)
{
    struct timespec t={0,20000000}, rem={123,456};
    int64_t start=mono();CHECK(start>=0);
    CHECK(rawsleep(clock,0,&t,&rem)==0);
    CHECK(mono()-start>=20000000);
    CHECK(rem.tv_sec==123 && rem.tv_nsec==456);
    CHECK(rawsleep(clock,0,&t,(void *)1)==0);
    return 0;
}
static int absolute(int clock)
{
    struct timespec t;
    CHECK(syscall(SYS_clock_gettime,clock,&t)==0);
    t.tv_nsec+=20000000;
    if(t.tv_nsec>=1000000000) {t.tv_nsec-=1000000000;t.tv_sec++;}
    int64_t start=mono();CHECK(start>=0);
    CHECK(rawsleep(clock,TIMER_ABSTIME,&t,(void *)1)==0);
    CHECK(mono()-start>=10000000);
    t.tv_sec=0;t.tv_nsec=1;
    CHECK(rawsleep(clock,TIMER_ABSTIME,&t,(void *)1)==0);
    return 0;
}
static pid_t sender(void)
{
    pid_t parent=getpid(), p=fork();
    if(p==0) {
        struct timespec delay={0,30000000};
        if(syscall(SYS_nanosleep,&delay,NULL)!=0) _exit(1);
        _exit(kill(parent,SIGUSR1)!=0);
    }
    return p;
}
static int interruption(int clock, int flags, int bad_rem, int huge, int alias)
{
    struct sigaction sa={0};sa.sa_handler=handler;sa.sa_flags=SA_RESTART;
    CHECK(sigemptyset(&sa.sa_mask)==0 && sigaction(SIGUSR1,&sa,NULL)==0);
    struct timespec t={1,0}, rem={123,456};
    if(flags&TIMER_ABSTIME) {
        CHECK(syscall(SYS_clock_gettime,clock,&t)==0);t.tv_sec++;
    }
    if(huge) {t.tv_sec=sizeof(time_t)==8?LLONG_MAX:INT_MAX;t.tv_nsec=999999999;}
    struct timespec *out=bad_rem?(void *)1:alias?&t:&rem;
    pid_t p=sender();int status;CHECK(p>0);
    CHECK(rawsleep(clock,flags,&t,out)==-1);
    int error=errno;
    CHECK(waitpid(p,&status,0)==p && WIFEXITED(status) && WEXITSTATUS(status)==0);
    CHECK(caught==1);
    CHECK(error==(bad_rem && !(flags&TIMER_ABSTIME)?EFAULT:EINTR));
    if(flags&TIMER_ABSTIME) CHECK(rem.tv_sec==123 && rem.tv_nsec==456);
    else if(!bad_rem) {
        CHECK(out->tv_sec>=0 && out->tv_nsec>=0 && out->tv_nsec<1000000000);
        CHECK(out->tv_sec>0 || out->tv_nsec>0);
        if(!huge) CHECK(out->tv_sec<1 || (out->tv_sec==1 && out->tv_nsec==0));
    }
    return 0;
}
static int rel_rt(void) {return relative(CLOCK_REALTIME);}
static int rel_mono(void) {return relative(CLOCK_MONOTONIC);}
static int rel_boot(void) {return relative(CLOCK_BOOTTIME);}
static int abs_rt(void) {return absolute(CLOCK_REALTIME);}
static int abs_mono(void) {return absolute(CLOCK_MONOTONIC);}
static int abs_boot(void) {return absolute(CLOCK_BOOTTIME);}
static int intr_rel(void) {return interruption(CLOCK_MONOTONIC,0,0,0,0);}
static int intr_rt(void) {return interruption(CLOCK_REALTIME,0,0,0,0);}
static int intr_abs(void) {return interruption(CLOCK_MONOTONIC,TIMER_ABSTIME,0,0,0);}
static int intr_abs_rt(void) {return interruption(CLOCK_REALTIME,TIMER_ABSTIME,0,0,0);}
static int bad_rem(void) {return interruption(CLOCK_MONOTONIC,0,1,0,0);}
static int abs_bad_rem(void) {return interruption(CLOCK_MONOTONIC,TIMER_ABSTIME,1,0,0);}
static int huge_rel(void) {return interruption(CLOCK_MONOTONIC,0,0,1,0);}
static int huge_abs(void) {return interruption(CLOCK_MONOTONIC,TIMER_ABSTIME,1,1,0);}
static int huge_rt(void) {return interruption(CLOCK_REALTIME,TIMER_ABSTIME,1,1,0);}
static int alias_rem(void) {return interruption(CLOCK_MONOTONIC,0,0,0,1);}
static int libc_sleep(void)
{
    int64_t start=mono();CHECK(start>=0);
    CHECK(usleep(1000)==0 && mono()-start>=1000000);
    struct timespec t={0,1000000};
    CHECK(clock_nanosleep(CLOCK_MONOTONIC,0,&t,NULL)==0);
    return 0;
}
static int confined_ftruncate(void)
{
    char path[]="/tmp/lx-ftruncate-XXXXXX";
    char readpath[]="/tmp/lx-ftruncate-read-XXXXXX";
    struct stat st;
    int fd=mkstemp(path);

    CHECK(fd>=0 && unlink(path)==0);
    CHECK(ftruncate(fd,65568)==0);
    CHECK(fstat(fd,&st)==0 && st.st_size==65568);
    CHECK(ftruncate(fd,17)==0);
    CHECK(fstat(fd,&st)==0 && st.st_size==17);
    errno=0;
    CHECK(ftruncate(fd,(off_t)-1)==-1 && errno==EINVAL);
    CHECK(close(fd)==0);

    fd=open("/dev/null",O_WRONLY);
    CHECK(fd>=0);
    errno=0;
    CHECK(ftruncate(fd,1)==-1 && errno==EINVAL);
    CHECK(close(fd)==0);

    fd=mkstemp(readpath);
    CHECK(fd>=0 && close(fd)==0);
    fd=open(readpath,O_RDONLY);
    CHECK(fd>=0 && unlink(readpath)==0);
    errno=0;
    CHECK(ftruncate(fd,1)==-1 && errno==EINVAL);
    CHECK(close(fd)==0);
    return 0;
}
static int confined_fsync(void)
{
    char path[]="/tmp/lx-fsync-XXXXXX";
    int fd=mkstemp(path);

    CHECK(fd>=0 && unlink(path)==0);
    CHECK(write(fd,"data",4)==4);
    CHECK(fsync(fd)==0 && fdatasync(fd)==0);
    CHECK(close(fd)==0);
    fd=open("/dev/null",O_WRONLY);
    CHECK(fd>=0);
    errno=0;
    CHECK(fsync(fd)==-1 && errno==EINVAL);
    errno=0;
    CHECK(fdatasync(fd)==-1 && errno==EINVAL);
    CHECK(close(fd)==0);
    errno=0;
    CHECK(fdatasync(-1)==-1 && errno==EBADF);
    return 0;
}
static int confined_sigaction_expose_tagbits(void)
{
    struct sigaction sa={0};
    struct lx_test_sigaction action, observed;

    sa.sa_handler=handler;
    CHECK(sigemptyset(&sa.sa_mask)==0);
    CHECK(sigaction(SIGUSR2,&sa,NULL)==0);
    CHECK(syscall(SYS_rt_sigaction,SIGUSR2,NULL,&action,8)==0);
    action.flags|=LX_TEST_SA_EXPOSE_TAGBITS;
    CHECK(syscall(SYS_rt_sigaction,SIGUSR2,&action,NULL,8)==0);
    memset(&observed,0,sizeof(observed));
    CHECK(syscall(SYS_rt_sigaction,SIGUSR2,NULL,&observed,8)==0);
    CHECK((observed.flags&LX_TEST_SA_EXPOSE_TAGBITS)!=0);
    return 0;
}
static int confined_sigtimedwait(void)
{
    struct timespec timeout={1,0}, zero={0,0};
    sigset_t set, old;
    siginfo_t info;
    uint64_t bits=1ULL<<(SIGUSR2-1);

    CHECK(sigemptyset(&set)==0 && sigaddset(&set,SIGUSR2)==0);
    CHECK(pthread_sigmask(SIG_BLOCK,&set,&old)==0);
    CHECK(kill(getpid(),SIGUSR2)==0);
    memset(&info,0,sizeof(info));
    CHECK(syscall(SYS_rt_sigtimedwait,&bits,&info,&timeout,8)==SIGUSR2);
    CHECK(info.si_signo==SIGUSR2);
    errno=0;
    CHECK(syscall(SYS_rt_sigtimedwait,&bits,&info,&zero,8)==-1 &&
        errno==EAGAIN);
    errno=0;
    CHECK(syscall(SYS_rt_sigtimedwait,&bits,&info,&zero,7)==-1 &&
        errno==EINVAL);
    CHECK(pthread_sigmask(SIG_SETMASK,&old,NULL)==0);
    return 0;
}
static int confined_tgsigqueueinfo(void)
{
    struct timespec timeout={1,0};
    sigset_t set, old;
    siginfo_t sent, received;
    uint64_t bits=1ULL<<(SIGUSR1-1);

    CHECK(sigemptyset(&set)==0 && sigaddset(&set,SIGUSR1)==0);
    CHECK(pthread_sigmask(SIG_BLOCK,&set,&old)==0);
    memset(&sent,0,sizeof(sent));
    sent.si_signo=SIGUSR1;
    sent.si_code=SI_QUEUE;
    sent.si_pid=getpid();
    sent.si_uid=getuid();
    sent.si_value.sival_int=0x12345678;
    CHECK(syscall(SYS_rt_tgsigqueueinfo,getpid(),syscall(SYS_gettid),
        SIGUSR1,&sent)==0);
    memset(&received,0,sizeof(received));
    CHECK(syscall(SYS_rt_sigtimedwait,&bits,&received,&timeout,8)==SIGUSR1);
    CHECK(received.si_signo==SIGUSR1);
    CHECK(received.si_value.sival_int==0x12345678);
    CHECK(pthread_sigmask(SIG_SETMASK,&old,NULL)==0);
    return 0;
}
static int confined_inotify(void)
{
    char dir[]="/tmp/lx-inotify-XXXXXX", path[128], events[4096];
    struct inotify_event *event;
    ssize_t len;
    int fd, file, wd, flags, found=0;
    size_t pos;

    fd=inotify_init();
    CHECK(fd>=0 && close(fd)==0);
    fd=inotify_init1(IN_NONBLOCK|IN_CLOEXEC);
    CHECK(fd>=0);
    flags=fcntl(fd,F_GETFL);
    CHECK(flags>=0 && (flags&O_NONBLOCK)!=0);
    flags=fcntl(fd,F_GETFD);
    CHECK(flags>=0 && (flags&FD_CLOEXEC)!=0);
    CHECK(mkdtemp(dir)!=NULL);
    wd=inotify_add_watch(fd,dir,IN_CREATE|IN_CLOSE_WRITE|IN_DELETE);
    CHECK(wd>=0);
    CHECK(snprintf(path,sizeof(path),"%s/file",dir)>0);
    file=open(path,O_CREAT|O_WRONLY|O_EXCL,0600);
    CHECK(file>=0 && write(file,"x",1)==1 && close(file)==0);
    len=read(fd,events,sizeof(events));
    CHECK(len>0);
    for(pos=0;pos<(size_t)len;pos+=sizeof(*event)+event->len) {
        event=(struct inotify_event *)(events+pos);
        CHECK(sizeof(*event)+event->len<=(size_t)len-pos);
        if(event->wd==wd && (event->mask&IN_CREATE)!=0 &&
            event->len!=0 && strcmp(event->name,"file")==0) found=1;
    }
    CHECK(found);
    CHECK(inotify_rm_watch(fd,wd)==0);
    CHECK(close(fd)==0 && unlink(path)==0 && rmdir(dir)==0);
    return 0;
}
static int confined_inotify_missing_path(void)
{
    char path[]="/tmp/lx-inotify-missing-XXXXXX";
    int fd, stderr_flags;

    CHECK(mkdtemp(path)!=NULL);
    CHECK(rmdir(path)==0);
    stderr_flags=fcntl(STDERR_FILENO,F_GETFD);
    CHECK(stderr_flags>=0);
    fd=inotify_init1(IN_CLOEXEC);
    CHECK(fd>=0);
    errno=0;
    CHECK(inotify_add_watch(fd,path,IN_CREATE)==-1 && errno==ENOENT);
    CHECK(fcntl(STDERR_FILENO,F_GETFD)==stderr_flags);
    CHECK(close(fd)==0);
    return 0;
}
static pthread_barrier_t task_nlink_barrier;
static void *task_nlink_worker(void *unused)
{
    int error;

    (void)unused;
    error=pthread_barrier_wait(&task_nlink_barrier);
    if(error!=0 && error!=PTHREAD_BARRIER_SERIAL_THREAD)
        return (void *)(uintptr_t)1;
    error=pthread_barrier_wait(&task_nlink_barrier);
    if(error!=0 && error!=PTHREAD_BARRIER_SERIAL_THREAD)
        return (void *)(uintptr_t)1;
    return NULL;
}
static int proc_task_nlink(void)
{
    pthread_t threads[2];
    struct stat st;
    void *result;
    int error;
    size_t i;

    CHECK(pthread_barrier_init(&task_nlink_barrier,NULL,3)==0);
    for(i=0;i<2;i++)
        CHECK(pthread_create(&threads[i],NULL,task_nlink_worker,NULL)==0);
    error=pthread_barrier_wait(&task_nlink_barrier);
    CHECK(error==0 || error==PTHREAD_BARRIER_SERIAL_THREAD);
    CHECK(stat("/proc/self/task",&st)==0);
    CHECK(st.st_nlink==5);
    error=pthread_barrier_wait(&task_nlink_barrier);
    CHECK(error==0 || error==PTHREAD_BARRIER_SERIAL_THREAD);
    for(i=0;i<2;i++) {
        CHECK(pthread_join(threads[i],&result)==0);
        CHECK(result==NULL);
    }
    CHECK(pthread_barrier_destroy(&task_nlink_barrier)==0);
    return 0;
}
static pthread_barrier_t clone_barrier;
static volatile unsigned clone_inotify_stop;
static volatile unsigned long clone_inotify_progress;
static void *clone_inotify_worker(void *unused)
{
    (void)unused;
    while(!__atomic_load_n(&clone_inotify_stop,__ATOMIC_ACQUIRE)) {
        int fd=inotify_init();

        if(fd<0 || close(fd)!=0) return (void *)(uintptr_t)1;
        __atomic_add_fetch(&clone_inotify_progress,1,__ATOMIC_RELEASE);
    }
    return NULL;
}
static void *clone_epoll_worker(void *unused)
{
    int fd, error;

    (void)unused;
    error=pthread_barrier_wait(&clone_barrier);
    if(error!=0 && error!=PTHREAD_BARRIER_SERIAL_THREAD)
        return (void *)(uintptr_t)1;
    fd=epoll_create1(EPOLL_CLOEXEC);
    if(fd<0 || close(fd)!=0) return (void *)(uintptr_t)1;
    return NULL;
}
static int clone_epoll_burst(void)
{
    pthread_t threads[32], inotify_thread;
    void *result;
    int error;
    size_t i;

    clone_inotify_stop=0;
    clone_inotify_progress=0;
    CHECK(pthread_create(&inotify_thread,NULL,clone_inotify_worker,NULL)==0);
    while(__atomic_load_n(&clone_inotify_progress,__ATOMIC_ACQUIRE)<10)
        sched_yield();
    CHECK(pthread_barrier_init(&clone_barrier,NULL,33)==0);
    for(i=0;i<32;i++)
        CHECK(pthread_create(&threads[i],NULL,clone_epoll_worker,NULL)==0);
    error=pthread_barrier_wait(&clone_barrier);
    CHECK(error==0 || error==PTHREAD_BARRIER_SERIAL_THREAD);
    for(i=0;i<32;i++) {
        CHECK(pthread_join(threads[i],&result)==0);
        CHECK(result==NULL);
    }
    __atomic_store_n(&clone_inotify_stop,1,__ATOMIC_RELEASE);
    CHECK(pthread_join(inotify_thread,&result)==0);
    CHECK(result==NULL);
    CHECK(pthread_barrier_destroy(&clone_barrier)==0);
    return 0;
}
static int rseq_fallback(void)
{
    struct timespec t={0,1000000};
    errno=0;
    CHECK(syscall(SYS_rseq,NULL,0,0,0)==-1);
    CHECK(errno==ENOSYS || errno==EINVAL);
    CHECK(rawsleep(CLOCK_MONOTONIC,0,&t,NULL)==0);
    return 0;
}
static int modern_probe_fallbacks(void)
{
    struct timespec t={0,1000000};
    long result;
    errno=0;
    result=prctl(PR_CAPBSET_READ,0,0,0,0);
    CHECK(result==0 || result==1 || (result==-1 && errno==EINVAL));
    errno=0;
    result=syscall(SYS_faccessat2,AT_FDCWD,"/",F_OK,0);
    CHECK(result==0 || (result==-1 && errno==ENOSYS));
    CHECK(rawsleep(CLOCK_MONOTONIC,0,&t,NULL)==0);
    return 0;
}
static int vfork_exec(void)
{
    int status;
    pid_t p=vfork();
    CHECK(p>=0);
    if(p==0) {
        execl("/bin/true","true",(char *)NULL);
        _exit(127);
    }
    CHECK(waitpid(p,&status,0)==p);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status)==0);
    return 0;
}
static long raw_read(int fd, void *buf, size_t len)
{
    long result;
    __asm__ volatile("syscall"
        : "=a" (result)
        : "a" ((long)SYS_read), "D" ((long)fd), "S" (buf), "d" (len)
        : "rcx", "r11", "memory");
    return result;
}
static void __attribute__((noreturn)) raw_exit(int status)
{
    __asm__ volatile("syscall"
        :
        : "a" ((long)SYS_exit), "D" ((long)status)
        : "rcx", "r11", "memory");
    __builtin_unreachable();
}
static long raw_getpid(void)
{
    long result;
    __asm__ volatile("syscall"
        : "=a" (result)
        : "a" ((long)SYS_getpid)
        : "rcx", "r11", "memory");
    return result;
}
static long raw_kill(pid_t pid, int signal)
{
    long result;
    __asm__ volatile("syscall"
        : "=a" (result)
        : "a" ((long)SYS_kill), "D" ((long)pid), "S" ((long)signal)
        : "rcx", "r11", "memory");
    return result;
}
static int vfork_shared_exit(void)
{
    volatile uint32_t *shared;
    struct timespec delay={0,100000000};
    int gate[2], child_status, helper_status;
    int64_t start, elapsed;
    pid_t helper, child;

    shared=mmap(NULL,4096,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(shared!=MAP_FAILED && pipe(gate)==0);
    *shared=0x11111111;
    helper=fork();
    CHECK(helper>=0);
    if(helper==0) {
        char byte='x';
        close(gate[0]);
        if(syscall(SYS_nanosleep,&delay,NULL)!=0 ||
            write(gate[1],&byte,1)!=1) _exit(1);
        _exit(0);
    }
    close(gate[1]);
    start=mono();CHECK(start>=0);
    child=vfork();
    CHECK(child>=0);
    if(child==0) {
        char byte;
        *shared=0x22222222;
        if(raw_read(gate[0],&byte,1)!=1) raw_exit(125);
        raw_exit(42);
    }
    elapsed=mono()-start;
    close(gate[0]);
    CHECK(*shared==0x22222222);
    CHECK(elapsed>=50000000);
    CHECK(waitpid(child,&child_status,0)==child);
    CHECK(WIFEXITED(child_status) && WEXITSTATUS(child_status)==42);
    CHECK(waitpid(helper,&helper_status,0)==helper);
    CHECK(WIFEXITED(helper_status) && WEXITSTATUS(helper_status)==0);
    CHECK(munmap((void *)shared,4096)==0);
    return 0;
}
static int vfork_exec_failure(void)
{
    int status;
    pid_t p=vfork();
    CHECK(p>=0);
    if(p==0) {
        execl("/file-that-does-not-exist","missing",(char *)NULL);
        _exit(errno==ENOENT?127:126);
    }
    CHECK(waitpid(p,&status,0)==p);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status)==127);
    return 0;
}
static volatile unsigned long vfork_thread_progress;
static volatile unsigned vfork_thread_child;
static volatile unsigned vfork_thread_stop;
static volatile unsigned vfork_thread_sent;
static volatile unsigned char vfork_thread_source;
static int vfork_thread_gate;
static void *vfork_sibling(void *unused)
{
    char byte='t';

    (void)unused;
    while(!__atomic_load_n(&vfork_thread_stop,__ATOMIC_ACQUIRE)) {
        __atomic_add_fetch(&vfork_thread_progress,1,__ATOMIC_RELAXED);
        if(!__atomic_load_n(&vfork_thread_sent,__ATOMIC_RELAXED) &&
            __atomic_load_n(&vfork_thread_child,__ATOMIC_ACQUIRE) &&
            write(vfork_thread_gate,&byte,1)==1) {
            __atomic_store_n(&vfork_thread_sent,1,__ATOMIC_RELEASE);
        }
    }
    return NULL;
}
static int vfork_multithreaded(void)
{
    pthread_t thread;
    unsigned long before, after;
    struct timespec delay={0,500000000};
    int gate[2], status, watchdog_status;
    pid_t child, watchdog;

    CHECK(pipe(gate)==0);
    watchdog=fork();
    CHECK(watchdog>=0);
    if(watchdog==0) {
        char byte='w';
        close(gate[0]);
        if(syscall(SYS_nanosleep,&delay,NULL)!=0 ||
            write(gate[1],&byte,1)!=1) _exit(1);
        _exit(0);
    }
    vfork_thread_progress=0;
    vfork_thread_child=0;
    vfork_thread_stop=0;
    vfork_thread_sent=0;
    vfork_thread_source=0;
    vfork_thread_gate=gate[1];
    CHECK(pthread_create(&thread,NULL,vfork_sibling,NULL)==0);
    while(__atomic_load_n(&vfork_thread_progress,__ATOMIC_RELAXED)<1000)
        sched_yield();
    before=__atomic_load_n(&vfork_thread_progress,__ATOMIC_RELAXED);
    child=vfork();
    CHECK(child>=0);
    if(child==0) {
        char byte;
        __atomic_store_n(&vfork_thread_child,1,__ATOMIC_RELEASE);
        if(raw_read(gate[0],&byte,1)!=1) raw_exit(125);
        __atomic_store_n(&vfork_thread_source,(unsigned char)byte,
            __ATOMIC_RELEASE);
        raw_exit(0);
    }
    after=__atomic_load_n(&vfork_thread_progress,__ATOMIC_RELAXED);
    __atomic_store_n(&vfork_thread_stop,1,__ATOMIC_RELEASE);
    CHECK(pthread_join(thread,NULL)==0);
    CHECK(waitpid(child,&status,0)==child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status)==0);
    CHECK(waitpid(watchdog,&watchdog_status,0)==watchdog);
    close(gate[0]);
    close(gate[1]);
    CHECK(WIFEXITED(watchdog_status) && WEXITSTATUS(watchdog_status)==0);
    CHECK(__atomic_load_n(&vfork_thread_source,__ATOMIC_ACQUIRE)=='t');
    CHECK(__atomic_load_n(&vfork_thread_sent,__ATOMIC_ACQUIRE)==1);
    CHECK(after>before);
    return 0;
}
static int vfork_signal_exit(void)
{
    int status;
    pid_t p=vfork();
    CHECK(p>=0);
    if(p==0) {
        if(raw_kill((pid_t)raw_getpid(),SIGKILL)!=0) raw_exit(125);
        raw_exit(126);
    }
    CHECK(waitpid(p,&status,0)==p);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL);
    return 0;
}
static int spawn_exec(void)
{
    char *const argv[]={"true",NULL};
    int error, status;
    pid_t child;

    error=posix_spawn(&child,"/bin/true",NULL,NULL,argv,environ);
    errno=error;
    CHECK(error==0);
    CHECK(waitpid(child,&status,0)==child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status)==0);
    return 0;
}
static int confine(void)
{
    struct sock_filter insn=BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW);
    struct sock_fprog program={1,&insn};

    if(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)!=0 ||
        prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&program)!=0) return 1;
    return 0;
}
static int vfork_stress(unsigned long iterations)
{
    volatile unsigned long i;

    for(i=0;i<iterations;i++) {
        int status, expected;
        pid_t child=vfork();
        if(child<0) return 1;
        if(child==0) {
            switch(i%4) {
            case 0:
                raw_exit(42);
            case 1:
                execl("/bin/true","true",(char *)NULL);
                raw_exit(126);
            case 2:
                execl("/file-that-does-not-exist","missing",
                    (char *)NULL);
                raw_exit(errno==ENOENT?127:126);
            default:
                if(raw_kill((pid_t)raw_getpid(),SIGKILL)!=0)
                    raw_exit(125);
                raw_exit(126);
            }
        }
        if(waitpid(child,&status,0)!=child) return 1;
        expected=i%4;
        if((expected==0 && (!WIFEXITED(status) ||
            WEXITSTATUS(status)!=42)) ||
            (expected==1 && (!WIFEXITED(status) ||
            WEXITSTATUS(status)!=0)) ||
            (expected==2 && (!WIFEXITED(status) ||
            WEXITSTATUS(status)!=127)) ||
            (expected==3 && (!WIFSIGNALED(status) ||
            WTERMSIG(status)!=SIGKILL))) return 1;
    }
    return 0;
}
static void run(const char *name,int (*fn)(void))
{
    pid_t p=fork();int status=-1;
    if(p==0) {
        /* Arm before confinement; alarm's existing libc path is unrelated. */
        alarm(5);
        if(confine()!=0) _exit(2);
        _exit(fn());
    }
    int ok=p>0 && waitpid(p,&status,0)==p && WIFEXITED(status) && WEXITSTATUS(status)==0;
    printf("%s %s status=%#x\n",ok?"PASS":"FAIL",name,status);
    cases++;failures+=!ok;
}
int main(int argc, char **argv)
{
    unsigned long iterations;

    setvbuf(stdout,NULL,_IOLBF,0);
    if(argc==3 && strcmp(argv[1],"--vfork-stress")==0) {
        char *end;
        errno=0;
        iterations=strtoul(argv[2],&end,10);
        if(errno!=0 || *end!='\0' || iterations==0 || iterations>1000000) {
            fprintf(stderr,"invalid vfork stress iteration count\n");
            return 2;
        }
        alarm(300);
        if(confine()!=0 || vfork_stress(iterations)!=0) {
            printf("VFORK_STRESS iterations=%lu failures=1\n",iterations);
            return 1;
        }
        printf("VFORK_STRESS iterations=%lu failures=0\n",iterations);
        return 0;
    }
    if(argc!=1) {
        fprintf(stderr,"usage: %s [--vfork-stress iterations]\n",argv[0]);
        return 2;
    }
    run("clock and timespec validation; unused flags and remainder",validation);
    run("relative realtime uses elapsed time",rel_rt);
    run("relative monotonic sleep",rel_mono);
    run("relative boottime sleep",rel_boot);
    run("absolute realtime deadline and past deadline",abs_rt);
    run("absolute monotonic deadline and past deadline",abs_mono);
    run("absolute boottime deadline and past deadline",abs_boot);
    run("relative interruption ignores SA_RESTART and returns remainder",intr_rel);
    run("relative realtime interruption returns remainder",intr_rt);
    run("absolute interruption preserves remainder",intr_abs);
    run("absolute realtime interruption preserves remainder",intr_abs_rt);
    run("interrupted relative remainder copy fault",bad_rem);
    run("absolute interruption ignores bad remainder pointer",abs_bad_rem);
    run("huge relative duration does not overflow",huge_rel);
    run("huge monotonic absolute deadline does not overflow",huge_abs);
    run("huge realtime absolute deadline does not overflow",huge_rt);
    run("request and remainder may alias",alias_rem);
    run("libc usleep and clock_nanosleep remain confined",libc_sleep);
    run("restricted ftruncate grows and shrinks a regular file",confined_ftruncate);
    run("restricted fsync and fdatasync synchronize regular files",confined_fsync);
    run("restricted rt_sigaction accepts SA_EXPOSE_TAGBITS",confined_sigaction_expose_tagbits);
    run("restricted rt_sigtimedwait consumes blocked signals",
        confined_sigtimedwait);
    run("restricted rt_tgsigqueueinfo queues thread signals",
        confined_tgsigqueueinfo);
    run("restricted inotify creates watches and delivers events",confined_inotify);
    run("failed inotify watch preserves existing descriptors",
        confined_inotify_missing_path);
    run("proc task link count includes visible threads",proc_task_nlink);
    run("restricted concurrent clone and epoll publication",clone_epoll_burst);
    run("rseq fallback does not poison later sleep",rseq_fallback);
    run("cap bounds and faccessat2 probes do not poison later sleep",modern_probe_fallbacks);
    run("restricted vfork shares memory and suspends its parent",vfork_shared_exit);
    run("restricted vfork releases its parent after exec",vfork_exec);
    run("restricted vfork releases its parent after failed exec and exit",vfork_exec_failure);
    run("restricted vfork suspends only the calling thread",vfork_multithreaded);
    run("restricted vfork releases its parent after child signal",vfork_signal_exit);
    run("libc posix_spawn remains confined",spawn_exec);
    printf("CLOCK_SLEEP cases=%u failures=%u\n",cases,failures);
    return failures!=0;
}
