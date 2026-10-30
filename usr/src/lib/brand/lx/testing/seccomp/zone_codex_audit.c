/* CDDL version 1.0, http://www.illumos.org/license/CDDL. */
#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <grp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static FILE *input, *output;
static pid_t server;
static char line[131072];
static char terminal[4096];
static size_t terminal_used;
static void terminal_output(void)
{
    const char *p=strstr(line,"\"deltaBase64\":\"");
    if(p==NULL || strstr(line,"audit-pty")==NULL)return;
    p+=15;unsigned value=0,bits=0;
    while(*p && *p!='"' && *p!='=') {
        unsigned c=(unsigned char)*p++,d;
        if(c>='A' && c<='Z')d=c-'A';
        else if(c>='a' && c<='z')d=c-'a'+26;
        else if(c>='0' && c<='9')d=c-'0'+52;
        else if(c=='+')d=62;
        else if(c=='/')d=63;
        else return;
        value=(value<<6)|d;bits+=6;
        if(bits>=8) {
            bits-=8;
            if(terminal_used<sizeof(terminal)-1)terminal[terminal_used++]=(value>>bits)&255;
        }
    }
    terminal[terminal_used]=0;
}
static int send_line(const char *text)
{
    printf("RPC_IN %s\n",text);
    return fprintf(input,"%s\n",text)<0 || fflush(input)!=0;
}
static int read_line(void)
{
    struct pollfd p={fileno(output),POLLIN,0};
    if(poll(&p,1,15000)<=0 || fgets(line,sizeof(line),output)==NULL) return 1;
    printf("RPC_OUT %s",line);terminal_output();return 0;
}
static int response(int id,int expected,const char *marker)
{
    char key[32],status[32];snprintf(key,sizeof(key),"\"id\":%d,",id);
    snprintf(status,sizeof(status),"\"exitCode\":%d",expected);
    for(int i=0;i<150;i++) {
        if(read_line()!=0)return 1;
        if(strstr(line,key)!=NULL) {
            if(strstr(line,"\"error\":")!=NULL)return 1;
            if(expected>=0 && strstr(line,status)==NULL)return 1;
            if(marker!=NULL && strstr(line,marker)==NULL)return 1;
            return 0;
        }
    }
    return 1;
}
int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IOLBF,0);
    if(argc==2 && !strcmp(argv[1],"--identity")) {
        struct utsname u;if(uname(&u)!=0)return 1;
        printf("IDENTITY uid=%u gid=%u os=%s machine=%s\n",getuid(),getgid(),u.sysname,u.machine);
        return 0;
    }
    if(argc<2 || argc>3)return 2;
    if(argc==3 && !strcmp(argv[2],"--drop")) {
        if(setgroups(0,NULL)!=0 || setgid(65534)!=0 || setuid(65534)!=0)return 2;
    }
    char work[]="/var/tmp/zone-codex-audit.XXXXXX",home[256];
    if(mkdtemp(work)==NULL)return 2;
    snprintf(home,sizeof(home),"%s/home",work);
    if(mkdir(home,0700)!=0 || chdir(work)!=0)return 2;
    clearenv();setenv("HOME",home,1);setenv("CODEX_HOME",home,1);
    setenv("PATH","/usr/bin:/bin",1);setenv("SHELL","/bin/sh",1);
    printf("AUDIT_UID=%u CWD=%s NO_AUTH_ENV=1\n",getuid(),work);
    int in[2],out[2];if(pipe(in)!=0 || pipe(out)!=0)return 2;
    server=fork();if(server<0)return 2;
    if(server==0) {
        setpgid(0,0);dup2(in[0],0);dup2(out[1],1);
        close(in[0]);close(in[1]);close(out[0]);close(out[1]);
        execl(argv[1],argv[1],"app-server","--stdio","-c","sandbox_mode=\"danger-full-access\"",NULL);
        _exit(127);
    }
    close(in[0]);close(out[1]);input=fdopen(in[1],"w");output=fdopen(out[0],"r");
    setvbuf(output,NULL,_IONBF,0);
    int result=1,status;
    if(send_line("{\"id\":1,\"method\":\"initialize\",\"params\":{\"clientInfo\":{\"name\":\"lx-zone-audit\",\"version\":\"1\"},\"capabilities\":{\"experimentalApi\":true}}}")!=0 || response(1,-1,"userAgent")!=0)goto done;
    if(send_line("{\"method\":\"initialized\",\"params\":{}}")!=0)goto done;
    if(send_line("{\"id\":2,\"method\":\"command/exec\",\"params\":{\"command\":[\"/bin/sh\",\"-c\",\"printf SHELL_OK; printf saved > artifact; IFS= read -r value < artifact; printf FILE_%s \\\"$value\\\"; (printf CHILD_OK); printf PIPE_OK | { read value; printf %s \\\"$value\\\"; }; exit 7\"],\"sandboxPolicy\":{\"type\":\"dangerFullAccess\"},\"timeoutMs\":5000}}")!=0 || response(2,7,"SHELL_OKFILE_savedCHILD_OKPIPE_OK")!=0)goto done;
    puts("BUFFERED_COMMAND_PASS");
    if(send_line("{\"id\":3,\"method\":\"command/exec\",\"params\":{\"command\":[\"/bin/sh\",\"-c\",\"printf EXTERNAL_OK\"],\"sandboxPolicy\":{\"type\":\"externalSandbox\",\"networkAccess\":\"enabled\"},\"timeoutMs\":5000}}")!=0 || response(3,0,"EXTERNAL_OK")!=0)goto done;
    puts("EXTERNAL_POLICY_PASS");
    if(send_line("{\"id\":4,\"method\":\"command/exec\",\"params\":{\"command\":[\"/bin/sh\",\"-c\",\"printf PTY_READY; IFS= read -r value; case $value in input) printf PTY_INPUT_OK;; *) exit 9;; esac; IFS= read -r value; case $value in done) exit 0;; *) exit 9;; esac\"],\"sandboxPolicy\":{\"type\":\"dangerFullAccess\"},\"processId\":\"audit-pty\",\"tty\":true,\"timeoutMs\":5000}}")!=0)goto done;
    for(int i=0;i<150;i++) {
        if(read_line()!=0)goto done;
        if(strstr(line,"\"error\":")!=NULL)goto done;
        if(strstr(line,"command/exec/outputDelta")!=NULL && strstr(line,"audit-pty")!=NULL)break;
        if(i==149)goto done;
    }
    if(send_line("{\"id\":5,\"method\":\"command/exec/write\",\"params\":{\"processId\":\"audit-pty\",\"deltaBase64\":\"aW5wdXQK\"}}")!=0)goto done;
    for(int i=0;i<150 && strstr(terminal,"PTY_INPUT_OK")==NULL;i++) {
        if(read_line()!=0 || strstr(line,"\"error\":")!=NULL || strstr(line,"\"id\":4,")!=NULL)goto done;
    }
    if(strstr(terminal,"PTY_INPUT_OK")==NULL)goto done;
    if(send_line("{\"id\":6,\"method\":\"command/exec/write\",\"params\":{\"processId\":\"audit-pty\",\"deltaBase64\":\"ZG9uZQo=\"}}")!=0 || response(4,0,NULL)!=0)goto done;
    puts("PTY_COMMAND_PASS");result=0;
done:
    fclose(input);kill(-server,SIGTERM);
    if(waitpid(server,&status,0)<0)result=1;
    fclose(output);
    printf("ZONE_CODEX_AUDIT_EXIT=%d\n",result);return result;
}
