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
#include <sys/mman.h>
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
