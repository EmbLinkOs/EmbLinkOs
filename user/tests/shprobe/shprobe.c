#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <spawn.h>
#ifdef EMBLINK_TARGET
#include "embk.h"
#endif
#define SELF "/data/apps/shprobe/shprobe.elf"
#define SHELL "/system/bin/sh.elf"
static int all_write(int fd,const void *data,size_t n) {
 const char *p=data;while(n){int k=write(fd,p,n);if(k<0 && errno==EINTR)continue;if(k<=0)return -1;p+=k;n-=(size_t)k;}return 0;
}
static int helper(int argc,char **argv) {
 if(!strcmp(argv[1],"--hold")) {
  puts("Foreground hold: ready for Ctrl+C");fflush(stdout);
  for(;;)usleep(10000); /* deliberately declines cooperative cancellation */
 }
 if(!strcmp(argv[1],"--status"))return argc>2?atoi(argv[2]):0;
 if(!strcmp(argv[1],"--mark")){const char *s=getenv("MARK");puts(s?s:"unset");return 0;}
 if(!strcmp(argv[1],"--streams"))return all_write(1,"out\n",4) || all_write(2,"err\n",4);
 if(!strcmp(argv[1],"--fds")){int n=0;for(int i=3;i<64;i++)if(fcntl(i,F_GETFD)>=0)n++;printf("%d\n",n);return 0;}
 if(!strcmp(argv[1],"--emit")) {
  char b[257];for(unsigned i=0;i<sizeof b;i++)b[i]=(char)('a'+i%26);
  for(unsigned i=0;i<257;i++)if(all_write(1,b,sizeof b))return 80;
  return 0;
 }
 if(!strcmp(argv[1],"--upper")) {
  char b[333];int n;
  while((n=read(0,b,sizeof b))>0){for(int i=0;i<n;i++)if(b[i]>='a' && b[i]<='z')b[i]-=32;if(all_write(1,b,n))return 81;}
  return n<0?82:0;
 }
 if(!strcmp(argv[1],"--count") || !strcmp(argv[1],"--count-lower")) {
  int base=!strcmp(argv[1],"--count")?'A':'a';char b[431];unsigned total=0;int n;
  while((n=read(0,b,sizeof b))>0){for(int i=0;i<n;i++)if(b[i]!=base+((total+(unsigned)i)%257)%26)return 83;total+=(unsigned)n;}
  if(n<0){return 84;}printf("%u\n",total);return total==66049?0:85;
 }
 return 86;
}
#ifndef SH_PROBE_HELPER
#include "../../../tests/posixsh/cases.h"
static unsigned checks,failures;
#define CHECK(c) do{checks++;if(!(c)){printf("FAIL line %d: %s (errno %d)\n",__LINE__,#c,errno);failures++;}}while(0)
static int write_file(const char *path,const char *data) {
 int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0644);if(fd<0)return -1;int rc=all_write(fd,data,strlen(data));close(fd);return rc;
}
static char *replace(const char *input,const char *token,const char *value) {
 size_t count=0,t=strlen(token),v=strlen(value),size=strlen(input)+1;
 for(const char *p=input;(p=strstr(p,token));p+=t)count++;
 if(v>=t)size+=count*(v-t);else size-=count*(t-v);
 char *out=malloc(size);if(!out)return NULL;char *q=out;const char *p=input,*next;
 while((next=strstr(p,token))){size_t n=next-p;memcpy(q,p,n);q+=n;memcpy(q,value,v);q+=v;p=next+t;}strcpy(q,p);return out;
}
static int run_case(const struct shell_case *c,const char *root) {
 char *script=replace(c->script,"@PROBE@",SELF);if(!script)return -1;
 char *first=replace(script,"@ROOT@",root);free(script);script=first;if(!script)return -1;
 char *expected=replace(c->output,"@ROOT@",root);if(!expected){free(script);return -1;}
 char home[300];snprintf(home,sizeof home,"HOME=%s",root);
 char *env[]={home,"PATH=/system/bin:/bin",NULL};
 char *args[7]={"sh",NULL};int n=1;
 if(c->option && !strcmp(c->option,"--file")){args[n++]=script;args[n++]="argument";}
 else {if(c->option)args[n++]=(char *)c->option;args[n++]="-c";args[n++]=script;args[n++]="case-sh";}
 args[n]=NULL;
 posix_spawn_file_actions_t actions={0};int rc=posix_spawn_file_actions_init(&actions);
 if(!rc)rc=posix_spawn_file_actions_addopen(&actions,1,"out.bin",O_WRONLY|O_CREAT|O_TRUNC,0644);
 if(!rc)rc=posix_spawn_file_actions_addopen(&actions,2,"err.bin",O_WRONLY|O_CREAT|O_TRUNC,0644);
 pid_t pid=0;if(!rc)rc=posix_spawn(&pid,SHELL,&actions,NULL,args,env);
 posix_spawn_file_actions_destroy(&actions);
 CHECK(rc==0);
 int status=0;if(!rc)CHECK(waitpid(pid,&status,0)==pid);
 int code=WIFEXITED(status)?WEXITSTATUS(status):-1;
 CHECK(!rc && code==c->status);
 int fd=open("out.bin",O_RDONLY);char output[8192];int length=fd>=0?read(fd,output,sizeof output-1):-1;if(fd>=0)close(fd);
 if(length>=0)output[length]=0;
 CHECK(length>=0 && (size_t)length==strlen(expected) && !memcmp(output,expected,(size_t)length));
 if(rc || code!=c->status || length<0 || strcmp(output,expected)) {
  printf("Case: %s, status=%d expected=%d\n",c->name,code,c->status);
  printf("Output: [%.*s]\nExpected: [%s]\n",length>0?length:0,output,expected);
  int e=open("err.bin",O_RDONLY);if(e>=0){int k=read(e,output,sizeof output-1);close(e);if(k>0){output[k]=0;printf("Diagnostic: %s\n",output);}}
 }
 free(script);free(expected);return 0;
}
#endif
int main(int argc,char **argv) {
 if(argc>1)return helper(argc,argv);
#ifdef SH_PROBE_HELPER
 return 0;
#else
 const char *home=getenv("HOME");if(!home || home[0]!='/')home="/data";
 char root[256];if(strlen(home)>220)return 1;snprintf(root,sizeof root,"%s/shprobe",home);
 (void)mkdir(root,0755);CHECK(chdir(root)==0);(void)mkdir("nested",0755);
 CHECK(write_file("alpha.txt","A\n")==0 && write_file("beta.txt","B\n")==0);
 CHECK(write_file(".hidden.txt","hidden\n")==0 && write_file("source.sh","sourced=sourced-value\n")==0 && write_file("file-script.sh","printf 'file:%s\\n' \"$1\"\n")==0);
 for(size_t i=0;i<sizeof shell_cases/sizeof shell_cases[0];i++)run_case(&shell_cases[i],root);
 CHECK(access("syntax-only.txt",F_OK)==-1 && errno==ENOENT);
 unsigned temporary=0;DIR *dir=opendir(".");struct dirent *entry;
 if(dir){while((entry=readdir(dir)))if(!strncmp(entry->d_name,".sh-",4))temporary++;closedir(dir);}
 CHECK(dir!=NULL && temporary==0);
 CHECK(waitpid(-1,NULL,WNOHANG)==-1 && errno==ECHILD);
 printf("Shell scripts: %u cases; quoting, functions, loops, isolated workers and redirections.\n",(unsigned)(sizeof shell_cases/sizeof shell_cases[0]));
 puts("Native POSIX pipeline: 66049 bytes verified across three stages.");
 printf("shprobe: %u/%u checks passed\n",checks-failures,checks);
 return failures?1:0;
#endif
}
