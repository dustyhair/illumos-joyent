#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <limits.h>
#include <stdint.h>

static int ready[2], cmd[2][2], ack[2];
static int sig;
static pid_t owned, owner, sending;
static void cleanup(void) {
 if (getpid() == owner && sending > 0) {
  (void)kill(sending, SIGKILL);
  while (waitpid(sending, NULL, 0) < 0 && errno == EINTR) {}
 }
 if (getpid() == owner && owned > 0) {
  (void)kill(owned, SIGKILL);
  while (waitpid(owned, NULL, 0) < 0 && errno == EINTR) {}
 }
}
static void interrupted(int signo) { (void)signo; exit(124); }
static void die(const char *s) { perror(s); exit(1); }
static void put(int fd, const void *p, size_t n) {
 if (write(fd, p, n) != (ssize_t)n) die("write");
}
static void get(int fd, void *p, size_t n) {
 struct pollfd f = { fd, POLLIN, 0 };
 if (poll(&f, 1, 5000) != 1 || read(fd, p, n) != (ssize_t)n) die("read/timeout");
}
static siginfo_t info(int code, int v) {
 siginfo_t i; memset(&i, 0, sizeof(i)); i.si_signo = SIGUSR1;
 i.si_code = code; i.si_errno = 7; i.si_pid = 12345;
 i.si_uid = 23456; i.si_value.sival_ptr = (void *)(uintptr_t)(0x12340000U + (unsigned)v);
 return i;
}
static int sendq(pid_t p, pid_t t, int s, siginfo_t *i) {
 errno = 0;
 return syscall(SYS_rt_tgsigqueueinfo, p, t, s, i) == 0 ? 0 : errno;
}
static void consume(int count) {
 sigset_t set; siginfo_t got; struct timespec ts = { 2, 0 };
 sigemptyset(&set); sigaddset(&set, sig);
 for (int j = 0; j < count; j++) {
  siginfo_t want = info(SI_QUEUE, j);
  if (sigtimedwait(&set, &got, &ts) != sig || got.si_signo != sig ||
      got.si_code != SI_QUEUE || got.si_errno != 7 || got.si_pid != 12345 ||
      got.si_uid != 23456 || got.si_value.sival_ptr != want.si_value.sival_ptr)
   die("payload/count");
 }
 ts.tv_sec = 0;
 if (sigtimedwait(&set, &got, &ts) != -1 || errno != EAGAIN) die("extra pending");
}
static void *receiver(void *arg) {
 int idx = (int)(intptr_t)arg, n;
 pid_t tid = (pid_t)syscall(SYS_gettid);
 put(ready[1], &tid, sizeof(tid));
 get(cmd[idx][0], &n, sizeof(n)); consume(n);
 put(ack[1], &idx, sizeof(idx)); return NULL;
}
static void expect(const char *name, pid_t p, pid_t t, int s, int code, int want) {
 siginfo_t i = info(code, 0); int e = sendq(p, t, s, &i);
 printf("CASE %s errno=%d expected=%d\n", name, e, want); fflush(stdout);
 if (e != want) exit(1);
}
static void *sibling(void *arg) {
 pid_t main_tid=*(pid_t *)arg;
 expect("sibling-user-zero",getpid(),main_tid,0,SI_USER,EPERM);
 expect("sibling-tkill-zero",getpid(),main_tid,0,SI_TKILL,EPERM);
 return NULL;
}
static void sender(pid_t p, pid_t t, int cross) {
 if (setuid(cross ? 61002 : 61001)) die("sender setuid");
 if (cross) {
  expect("cross-uid-zero", p,t,0,SI_QUEUE,EPERM);
  expect("cross-uid-rt", p,t,sig,SI_QUEUE,EPERM);
 } else {
  expect("zero-tgid",0,t,sig,SI_QUEUE,EINVAL);
  expect("negative-tid",p,-1,sig,SI_QUEUE,EINVAL);
  expect("missing-tid",p,INT_MAX,sig,SI_QUEUE,ESRCH);
  expect("wrong-tgid",getpid(),t,sig,SI_QUEUE,ESRCH);
  expect("foreign-positive",p,t,0,1,EPERM);
  expect("reserved-missing",p,INT_MAX,0,SI_USER,EPERM);
  expect("missing-invalid-signal",p,INT_MAX,65,SI_QUEUE,ESRCH);
  expect("foreign-user",p,t,sig,SI_USER,EPERM);
  expect("foreign-tkill",p,t,sig,SI_TKILL,EPERM);
  for (int j=0;j<4096;j++) {
   siginfo_t i=info(SI_QUEUE,0);
   if (sendq(p,t,0,&i)) die("zero repeat");
  }
  puts("CASE zero-repeat count=4096 errno=0");
  for (int j=0;j<8;j++) {
   siginfo_t i=info(SI_QUEUE,j);
   if (sendq(p,t,sig,&i)) die("queue");
  }
  pid_t me=(pid_t)syscall(SYS_gettid);
  pthread_t helper;
  if (pthread_create(&helper,NULL,sibling,&me) || pthread_join(helper,NULL)) die("sibling join");
  expect("self-positive-zero",getpid(),me,0,1,0);
  expect("self-user-zero",getpid(),me,0,SI_USER,0);
  expect("self-tkill-zero",getpid(),me,0,SI_TKILL,0);
  expect("invalid-signal",p,t,65,SI_QUEUE,EINVAL);
  expect("null-info",p,t,0,SI_QUEUE,0);
  if (sendq(p,t,0,NULL) != EFAULT) die("null info");
 }
}
int main(int argc, char **argv) {
 pthread_t th; pid_t target, tids[2]; int status, n;
 (void)argv;
 sigset_t set; sig=SIGRTMIN+3;
 setvbuf(stdout,NULL,_IONBF,0);
 if (getuid()!=0) { fputs("root required for owned child UID tests\n",stderr); return 1; }
 if (pipe(ready)||pipe(cmd[0])||pipe(cmd[1])||pipe(ack)) die("pipe");
 sigemptyset(&set); sigaddset(&set,sig);
 if (pthread_sigmask(SIG_BLOCK,&set,NULL)) die("mask");
 owner=getpid(); atexit(cleanup);
 signal(SIGTERM,interrupted); signal(SIGINT,interrupted);
 target=fork(); if (target<0) die("fork");
 if (!target) {
  alarm(25);
  if (setuid(61001)) die("target setuid");
  /* Main reports before creating worker, fixing report ordering. */
  pid_t me=(pid_t)syscall(SYS_gettid);
  pthread_t helper;
  if (pthread_create(&helper,NULL,sibling,&me) || pthread_join(helper,NULL)) die("sibling join");
  put(ready[1],&me,sizeof(me));
  if (pthread_create(&th,NULL,receiver,(void *)(intptr_t)1)) die("pthread_create");
  get(cmd[0][0],&n,sizeof(n)); consume(n); n=0; put(ack[1],&n,sizeof(n));
  if (pthread_join(th,NULL)) die("join");
  _exit(0);
 }
 owned=target;
 get(ready[0],&tids[0],sizeof(pid_t)); get(ready[0],&tids[1],sizeof(pid_t));
 if (argc > 1) {
  siginfo_t i=info(1,0);
  printf("REFERENCE foreign-positive=%d\n",sendq(target,tids[0],0,&i));
  i=info(-60,0);
  printf("REFERENCE foreign-asyncnl-zero=%d\n",sendq(target,tids[0],0,&i));
  i=info(SI_QUEUE,0);
  printf("REFERENCE missing-invalid-signal=%d\n",sendq(target,INT_MAX,65,&i));
  i=info(SI_USER,0);
  printf("REFERENCE reserved-missing=%d\n",sendq(target,INT_MAX,0,&i));
  i=info(1,0);
  printf("REFERENCE self-positive-zero=%d\n",sendq(getpid(),(pid_t)syscall(SYS_gettid),0,&i));
 }
 int failed=0;
 for (int idx=0;idx<2;idx++) {
  for (int cross=1;cross>=0;cross--) {
   pid_t child=fork(); if (child<0) die("fork sender");
   if (!child) { sender(target,tids[idx],cross); _exit(0); }
   sending=child;
   if (waitpid(child,&status,0)!=child || !WIFEXITED(status) || WEXITSTATUS(status)) failed=1;
   sending=0;
   if (failed) break;
  }
  if (failed) break;
 }
 if (failed) kill(target,SIGKILL);
 else {
  n=8; put(cmd[0][1],&n,sizeof(n)); put(cmd[1][1],&n,sizeof(n));
  get(ack[0],&n,sizeof(n)); get(ack[0],&n,sizeof(n));
 }
 if (waitpid(target,&status,0)!=target || !WIFEXITED(status) || WEXITSTATUS(status)) failed=1;
 owned=0;
 puts(failed ? "FAIL queued_threads" : "PASS queued_threads main=8 worker=8 zero=8192 reaped=5");
 return failed;
}
