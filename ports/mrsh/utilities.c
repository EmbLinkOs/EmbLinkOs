#define _POSIX_C_SOURCE 200809L
#include "native.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef EMBLINK_TARGET
#include "embk.h"
#endif
static int escape(const char **text,bool *stop) {
 const char *p=*text;int c=(unsigned char)*p++;
 switch(c) {
 case 0:p--;c='\\';break;
 case 'a':c='\a';break;case 'b':c='\b';break;case 'f':c='\f';break;
 case 'n':c='\n';break;case 'r':c='\r';break;case 't':c='\t';break;case 'v':c='\v';break;
 case 'c':*stop=true;break;
 case '\\':break;
 default:
  if(c>='0' && c<='7') {
   unsigned value=c-'0',count=1;
   if(c=='0')count=0;
   while(count<3 && *p>='0' && *p<='7'){value=(value<<3)+(*p++-'0');count++;}
   c=value&255;
  } else {putchar('\\');}
 }
 *text=p;return c;
}
static bool print_escaped(const char *p) {
 while(*p){bool stop=false;int c=(unsigned char)*p++;
 if(c=='\\')c=escape(&p,&stop);
 if(stop){return true;}putchar(c);}return false;
}
static int utility_printf(int argc,char **argv) {
 if(argc<2){fprintf(stderr,"printf: expected a format\n");return 2;}
 int arg=2,error=0;
 do {
  int before=arg;const char *p=argv[1];
  while(*p) {
   if(*p=='\\'){p++;bool stop=false;int c=escape(&p,&stop);
 if(stop){return error;}putchar(c);continue;}
   if(*p!='%'){putchar((unsigned char)*p++);continue;}
   p++;
 if(*p=='%'){putchar('%');p++;continue;}
   char format[64]="%";size_t used=1;
   while(*p && strchr("-+ #0",*p)){if(used+1>=sizeof format){goto invalid;}format[used++]=*p++;}
   unsigned width=0,precision=0;
   while(isdigit((unsigned char)*p)){if(width>4096)goto invalid;width=width*10+(*p-'0');
 if(used+1>=sizeof format){goto invalid;}format[used++]=*p++;}
   if(width>4096)goto invalid;
   if(*p=='.') {
    if(used+1>=sizeof format){goto invalid;}format[used++]=*p++;
    while(isdigit((unsigned char)*p)){if(precision>4096)goto invalid;precision=precision*10+(*p-'0');
 if(used+1>=sizeof format){goto invalid;}format[used++]=*p++;}
    if(precision>4096)goto invalid;
   }
   char type=*p++;
 if(!type)goto invalid;
   const char *value=arg<argc?argv[arg++]:"";
   if(type=='s' || type=='c') {
    if(used+2>sizeof format)goto invalid;
    format[used++]=type;format[used]=0;
    if(type=='s')printf(format,value);else printf(format,(unsigned char)*value);
   } else if(type=='b' && used==1) {if(print_escaped(value))return error;}
   else if(strchr("diuoxX",type)) {
    char *end;errno=0;unsigned long long number=0;
    if(*value=='\'' || *value=='"')number=(unsigned char)value[1];
    else if(*value) {
     if(type=='d' || type=='i')number=(unsigned long long)strtoll(value,&end,0);
     else number=strtoull(value,&end,0);
     if(errno || end==value || *end){fprintf(stderr,"printf: invalid number: %s\n",value);error=1;}
    }
    if(used+4>sizeof format)goto invalid;
    format[used++]='l';format[used++]='l';format[used++]=type;format[used]=0;
    if(type=='d' || type=='i')printf(format,(long long)number);else printf(format,number);
   } else goto invalid;
  }
  if(arg==before)break;
 }while(arg<argc);
 return error || ferror(stdout);
invalid:
 fprintf(stderr,"printf: unsupported or oversized format\n");return 2;
}
static bool binary_op(const char *s){return !strcmp(s,"=") || !strcmp(s,"!=") || !strcmp(s,"-eq") || !strcmp(s,"-ne") || !strcmp(s,"-lt") || !strcmp(s,"-le") || !strcmp(s,"-gt") || !strcmp(s,"-ge");}
static int test_expr(int argc,char **argv,unsigned depth,int *error) {
 if(depth>64){*error=1;return 0;}
 if(!argc)return 0;
 if(argc==1)return *argv[0]!=0;
 if(argc==3 && binary_op(argv[1])) {
  if(!strcmp(argv[1],"="))return !strcmp(argv[0],argv[2]);
  if(!strcmp(argv[1],"!="))return strcmp(argv[0],argv[2])!=0;
  char *end;errno=0;long long a=strtoll(argv[0],&end,10);
  if(errno || end==argv[0] || *end){*error=1;return 0;}
  errno=0;long long b=strtoll(argv[2],&end,10);
  if(errno || end==argv[2] || *end){*error=1;return 0;}
  if(!strcmp(argv[1],"-eq"))return a==b;
 if(!strcmp(argv[1],"-ne"))return a!=b;
  if(!strcmp(argv[1],"-lt"))return a<b;
 if(!strcmp(argv[1],"-le"))return a<=b;
  if(!strcmp(argv[1],"-gt"))return a>b;
  return a>=b;
 }
 int level=0;
 for(int i=0;i<argc;i++) {
  if(!strcmp(argv[i],"("))level++;else if(!strcmp(argv[i],")"))level--;
  if(level<0){*error=1;return 0;}
  if(!level && !strcmp(argv[i],"-o")) {
   int a=test_expr(i,argv,depth+1,error),b=test_expr(argc-i-1,argv+i+1,depth+1,error);return a||b;
  }
 }
 if(level){*error=1;return 0;}
 level=0;
 for(int i=0;i<argc;i++) {
  if(!strcmp(argv[i],"("))level++;else if(!strcmp(argv[i],")"))level--;
  if(!level && !strcmp(argv[i],"-a")) {
   int a=test_expr(i,argv,depth+1,error),b=test_expr(argc-i-1,argv+i+1,depth+1,error);return a&&b;
  }
 }
 if(!strcmp(argv[0],"!"))return !test_expr(argc-1,argv+1,depth+1,error);
 if(argc>=3 && !strcmp(argv[0],"(") && !strcmp(argv[argc-1],")"))return test_expr(argc-2,argv+1,depth+1,error);
 if(argc!=2){*error=1;return 0;}
 const char *op=argv[0],*name=argv[1];
 if(!strcmp(op,"-n"))return *name!=0;
 if(!strcmp(op,"-z"))return *name==0;
 if(!strcmp(op,"-t")){char *end;long fd=strtol(name,&end,10);
 if(!*name || *end || fd<0 || fd>INT_MAX){*error=1;return 0;}return isatty(fd);}
 if(!strcmp(op,"-r"))return access(name,R_OK)==0;
 if(!strcmp(op,"-w"))return access(name,W_OK)==0;
 if(!strcmp(op,"-x"))return access(name,X_OK)==0;
 struct stat st;int rc=(!strcmp(op,"-L") || !strcmp(op,"-h"))?lstat(name,&st):stat(name,&st);
 if(!strcmp(op,"-e"))return rc==0;
 if(strcmp(op,"-f") && strcmp(op,"-d") && strcmp(op,"-s") && strcmp(op,"-L") && strcmp(op,"-h") && strcmp(op,"-p") && strcmp(op,"-c") && strcmp(op,"-b")){*error=1;return 0;}
 if(rc)return 0;
 if(!strcmp(op,"-f"))return S_ISREG(st.st_mode);
 if(!strcmp(op,"-d"))return S_ISDIR(st.st_mode);
 if(!strcmp(op,"-s"))return st.st_size>0;
 if(!strcmp(op,"-L") || !strcmp(op,"-h"))return S_ISLNK(st.st_mode);
 if(!strcmp(op,"-p"))return S_ISFIFO(st.st_mode);
 if(!strcmp(op,"-c"))return S_ISCHR(st.st_mode);
 return S_ISBLK(st.st_mode);
}
static int utility_test(int argc,char **argv) {
 if(!strcmp(argv[0],"[")){if(argc<2 || strcmp(argv[argc-1],"]")){fprintf(stderr,"[: missing ]\n");return 2;}argc--;}
 int error=0,value=test_expr(argc-1,argv+1,0,&error);
 if(error){fprintf(stderr,"test: invalid expression\n");return 2;}return value?0:1;
}
static int copy_fd(int input) {
 char buf[1024];ssize_t n;
 while((n=read(input,buf,sizeof buf))>0) {
  size_t off=0;while(off<(size_t)n){ssize_t k=write(1,buf+off,(size_t)n-off);
 if(k<0 && errno==EINTR)continue;
 if(k<=0){return 1;}off+=(size_t)k;}
 }
 return n<0;
}
static int utility_cat(int argc,char **argv) {
 if(argc==1)return copy_fd(0);
 int ret=0;
 for(int i=1;i<argc;i++){int fd=!strcmp(argv[i],"-")?0:open(argv[i],O_RDONLY|O_CLOEXEC);
 if(fd<0){perror(argv[i]);ret=1;continue;}if(copy_fd(fd)){perror("cat");ret=1;}if(fd!=0)close(fd);}return ret;
}
bool sh_has_extra_builtin(const char *name) {
 static const char *names[]={"echo","printf","test","[","cat","sleep","help"};
 for(size_t i=0;i<sizeof names/sizeof names[0];i++)if(!strcmp(name,names[i]))return true;
 return false;
}
int sh_extra_builtin(struct mrsh_state *state,int argc,char **argv) {
 (void)state;
 if(!strcmp(argv[0],"printf"))return utility_printf(argc,argv);
 if(!strcmp(argv[0],"test") || !strcmp(argv[0],"["))return utility_test(argc,argv);
 if(!strcmp(argv[0],"cat"))return utility_cat(argc,argv);
 if(!strcmp(argv[0],"echo")) {
  int first=1;bool newline=true;
 if(argc>1 && !strcmp(argv[1],"-n")){first=2;newline=false;}
  for(int i=first;i<argc;i++){if(i>first)putchar(' ');fputs(argv[i],stdout);}if(newline)putchar('\n');return ferror(stdout)?1:0;
 }
 if(!strcmp(argv[0],"sleep")) {
  if(argc!=2)return 2;
  char *end;errno=0;double n=strtod(argv[1],&end);
  if(errno || !*argv[1] || *end || !(n>=0 && n<=86400)){fprintf(stderr,"sleep: expected 0..86400 seconds\n");return 2;}
#ifdef EMBLINK_TARGET
  unsigned long long ms=(unsigned long long)(n*1000.0+0.5);
  while(ms){unsigned step=ms>20?20:(unsigned)ms;embk_sleep_ms(step);ms-=step;if(sh_interrupted())return 130;}
  return 0;
#else
  struct timespec delay={.tv_sec=(time_t)n,.tv_nsec=(long)((n-(time_t)n)*1000000000.0)};
  while(nanosleep(&delay,&delay)<0){if(errno!=EINTR)return 1;}return 0;
#endif
 }
 puts("EmbLink sh: a spawn-based mrsh port\nCommands: variables, quoting, functions, loops, case, pipelines, redirection,\nsubshells, command substitution, background execution and wait.\nUtilities: echo, printf (integer/string), test, [, cat, sleep.\nLimits: options -a, -f, -n, -u, -x; no errexit/noclobber yet.\nNo process-group job control, signal traps or in-place exec;\n16 pipeline stages, 32 background statuses and 1 MiB worker context.");return 0;
}
