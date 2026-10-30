/* CDDL version 1.0, http://www.illumos.org/license/CDDL. */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s errno=%d\n",__LINE__,#x,errno); return 1; } } while (0)
static unsigned cases, failures;
static volatile sig_atomic_t caught;
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
static void run(const char *name,int (*fn)(void))
{
    pid_t p=fork();int status=-1;
    if(p==0) {
        /* Arm before confinement; alarm's existing libc path is unrelated. */
        alarm(5);
        struct sock_filter insn=BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW);
        struct sock_fprog program={1,&insn};
        if(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)!=0 ||
            prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&program)!=0) _exit(2);
        _exit(fn());
    }
    int ok=p>0 && waitpid(p,&status,0)==p && WIFEXITED(status) && WEXITSTATUS(status)==0;
    printf("%s %s status=%#x\n",ok?"PASS":"FAIL",name,status);
    cases++;failures+=!ok;
}
int main(void)
{
    setvbuf(stdout,NULL,_IOLBF,0);
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
    printf("CLOCK_SLEEP cases=%u failures=%u\n",cases,failures);
    return failures!=0;
}
