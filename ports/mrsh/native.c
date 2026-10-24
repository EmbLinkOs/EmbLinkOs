#define _POSIX_C_SOURCE 200809L
#include "native.h"
#include "ast_codec.h"
#include "shell/redir.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mrsh/parser.h>
#include <mrsh/entry.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef EMBLINK_TARGET
#include "embk.h"
#endif
int sh_interrupted(void) {
#ifdef EMBLINK_TARGET
 return embk_intr_take()!=0 || embk_cancelled()==1;
#else
 return 0;
#endif
}
void sh_interrupt_init(void) {
#ifdef EMBLINK_TARGET
 (void)embk_intr_catch(1);
#endif
}
static pid_t foreground_pids[16];
static unsigned foreground_count;
void sh_set_foreground(const pid_t *pids,unsigned n) {
 foreground_count=n>16?16:n;
 if(foreground_count)memcpy(foreground_pids,pids,foreground_count*sizeof *pids);
}
#ifdef EMBLINK_TARGET
static bool foreground_child(uint32_t child,pid_t waiting) {
 if(!foreground_count)return child==(uint32_t)waiting;
 for(unsigned i=0;i<foreground_count;i++)if(child==(uint32_t)foreground_pids[i])return true;
 return false;
}
#endif
extern char **environ;
const char *sh_executable="/system/bin/sh.elf";
static bool write_all(int fd,const void *data,size_t n) {
 const char *p=data;while(n){ssize_t k=write(fd,p,n);if(k<0 && errno==EINTR)continue;if(k<=0)return false;p+=k;n-=k;}return true;
}
int sh_anonymous_file(void) {
 const char *base=getenv("TMPDIR");if(!base || base[0]!='/')base=getenv("HOME");
 if(!base || base[0]!='/')base=".";
 char path[256];int n=snprintf(path,sizeof path,"%s/.sh-XXXXXX",base);
 if(n<0 || n>=(int)sizeof path){errno=ENAMETOOLONG;return -1;}
 int fd=mkstemp(path);if(fd<0)return -1;
 if(fd<3){int moved=fcntl(fd,F_DUPFD,3);int error=errno;close(fd);fd=moved;if(fd<0){unlink(path);errno=error;return -1;}}
 if(unlink(path) || fcntl(fd,F_SETFD,FD_CLOEXEC)) {int saved=errno;close(fd);errno=saved;return -1;}
 return fd;
}
int sh_wait(pid_t pid) {
 int status;pid_t r;
#ifdef EMBLINK_TARGET
 int handle=-1;
 for(int h=0;h<32;h++){struct embk_child_result info;if(embk_child_control(h,EMBK_CHILD_INFO,0,&info)==1 && info.pid==(uint32_t)pid){handle=h;break;}}
 if(handle>=0)(void)embk_terminal_foreground(0,handle);
 uint64_t cancelled=0;
 for(;;) {
  r=waitpid(pid,&status,WNOHANG);if(r!=0)break;
  if(sh_interrupted()) {
   /* Only children already owned by this shell can be cancelled or killed.
    * This is native interruption, not a POSIX signal or process group. */
   for(int h=0;h<32;h++){struct embk_child_result info;if(embk_child_control(h,EMBK_CHILD_INFO,0,&info)==1 && !(info.flags&EMBK_CHILD_DONE) && foreground_child(info.pid,pid))(void)embk_cancel(h);}
   if(!cancelled)cancelled=embk_uptime_ms();
  }
  if(handle>=0 && embk_child_cancelled(handle)==1 && !cancelled)cancelled=embk_uptime_ms();
  if(cancelled && embk_uptime_ms()-cancelled>=500) {
   for(int h=0;h<32;h++){struct embk_child_result info;if(embk_child_control(h,EMBK_CHILD_INFO,0,&info)==1 && !(info.flags&EMBK_CHILD_DONE) && foreground_child(info.pid,pid))(void)embk_kill(h);}
  }
  embk_sleep_ms(10);
 }
 (void)embk_terminal_foreground(0,EMBK_INTR_ROUTE_SELF);
#else
 do{r=waitpid(pid,&status,0);}while(r<0 && errno==EINTR);
#endif
 if(r<0){perror("sh: waitpid");return TASK_STATUS_ERROR;}
 return WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status);
}
/* Explicit environment, with assignment prefixes taking precedence over
 * exported shell variables. Never modify the parent's global environ. */
struct env_builder {char *items[32];unsigned count;size_t bytes;int error;};
static void env_add(struct env_builder *b,const char *key,const char *value) {
 if(b->error)return;
 size_t keylen=strlen(key),n=keylen+strlen(value)+2;unsigned i;
 for(i=0;i<b->count;i++)if(!strncmp(b->items[i],key,keylen) && b->items[i][keylen]=='=')break;
 size_t previous=i<b->count?strlen(b->items[i])+1:0;
 if((i==b->count && b->count>=31) || b->bytes-previous+n>1024){b->error=E2BIG;return;}
 char *s=malloc(n);if(!s){b->error=ENOMEM;return;}snprintf(s,n,"%s=%s",key,value);
 if(i<b->count)free(b->items[i]);else b->count++;
 b->items[i]=s;b->bytes=b->bytes-previous+n;b->items[b->count]=NULL;
}
static void exported(const char *name,void *value,void *data) {
 struct mrsh_variable *v=value;if(v->attribs&MRSH_VAR_ATTRIB_EXPORT)env_add(data,name,v->value);
}
static void env_finish(struct env_builder *b){for(unsigned i=0;i<b->count;i++)free(b->items[i]);}
static int add_redirect(posix_spawn_file_actions_t *actions,const struct mrsh_io_redirect *r) {
 char *name=mrsh_word_str(r->name);if(!name)return ENOMEM;
 int target=r->io_number;int flags=0,rc=0;
 if(target<0)target=(r->op==MRSH_IO_GREAT || r->op==MRSH_IO_CLOBBER || r->op==MRSH_IO_DGREAT || r->op==MRSH_IO_GREATAND)?1:0;
 if(r->op==MRSH_IO_LESSAND || r->op==MRSH_IO_GREATAND) {
  if(!strcmp(name,"-"))rc=posix_spawn_file_actions_addclose(actions,target);
  else {char *end;errno=0;long source=strtol(name,&end,10);
   rc=errno || !*name || *end || source<0 || source>=64?EBADF:posix_spawn_file_actions_adddup2(actions,(int)source,target);}
 } else if(r->op==MRSH_IO_DLESS || r->op==MRSH_IO_DLESSDASH) {
  /* The caller handles here-documents separately so temporary ownership can
   * outlive action construction, up to the spawn's descriptor snapshot. */
  rc=ENOTSUP;
 } else {
  switch(r->op) {
  case MRSH_IO_LESS:flags=O_RDONLY;break;
  case MRSH_IO_GREAT:case MRSH_IO_CLOBBER:flags=O_WRONLY|O_CREAT|O_TRUNC;break;
  case MRSH_IO_DGREAT:flags=O_WRONLY|O_CREAT|O_APPEND;break;
  case MRSH_IO_LESSGREAT:flags=O_RDWR|O_CREAT;break;
  default:rc=EINVAL;break;
  }
  if(!rc)rc=posix_spawn_file_actions_addopen(actions,target,name,flags,0644);
 }
 free(name);return rc;
}
int sh_spawn_command(struct mrsh_state *state,const char *path,char **argv,const struct mrsh_simple_command *command) {
 struct mrsh_state_priv *priv=state_get_priv(state);
 struct env_builder env={0};mrsh_hashtable_for_each(&priv->variables,exported,&env);
 if(command)for(size_t i=0;i<command->assignments.len;i++) {
  struct mrsh_assignment *a=command->assignments.data[i];uint32_t attributes=0;
  (void)mrsh_env_get(state,a->name,&attributes);
  if(attributes&MRSH_VAR_ATTRIB_READONLY){env.error=EPERM;break;}
  char *value=mrsh_word_str(a->value);if(!value){env.error=ENOMEM;break;}
  env_add(&env,a->name,value);free(value);
 }
 posix_spawn_file_actions_t actions={0};int rc=env.error;bool ready=false;
 int temporary[61],n_temp=0;
 if(!rc){rc=posix_spawn_file_actions_init(&actions);ready=!rc;}
 if(!rc && command)for(size_t i=0;i<command->io_redirects.len;i++) {
  struct mrsh_io_redirect *r=command->io_redirects.data[i];
  if(r->op==MRSH_IO_DLESS || r->op==MRSH_IO_DLESSDASH) {
   if(n_temp==61){rc=E2BIG;break;}
   int target,fd=process_redir(r,&target);
   if(fd<0){rc=errno?errno:EIO;break;}
   temporary[n_temp++]=fd;rc=posix_spawn_file_actions_adddup2(&actions,fd,target);
  } else rc=add_redirect(&actions,r);
  if(rc)break;
 }
 pid_t pid=-1;if(!rc)rc=posix_spawn(&pid,path,&actions,NULL,argv,env.items);
 for(int i=0;i<n_temp;i++)close(temporary[i]);
 if(ready)posix_spawn_file_actions_destroy(&actions);
 env_finish(&env);
 if(rc){fprintf(stderr,"sh: %s: %s\n",argv[0],strerror(rc));return rc==ENOENT?127:126;}
 return sh_wait(pid);
}
/* Private worker stream. AST payloads carry their own version and typed
 * bounds; metadata lengths and aggregate size have independent budgets. */
struct stream {int fd;size_t total;bool ok;};
static void send_bytes(struct stream *s,const void *p,size_t n) {
 if(!s->ok)return;
 if(n>SH_CONTEXT_MAX-s->total || !write_all(s->fd,p,n)){s->ok=false;return;}s->total+=n;
}
static void send_number(struct stream *s,uint32_t n){send_bytes(s,&n,4);}
static void send_text(struct stream *s,const char *p){size_t n=strlen(p);if(n>SH_CONTEXT_MAX){s->ok=false;return;}send_number(s,n);send_bytes(s,p,n);}
static void send_node(struct stream *s,const struct mrsh_node *node) {
 char *data;size_t size;
 if(!sh_ast_encode(node,&data,&size)){s->ok=false;return;}
 send_number(s,size);send_bytes(s,data,size);free(data);
}
static unsigned table_count(struct mrsh_hashtable *table) {
 unsigned n=0;for(unsigned i=0;i<MRSH_HASHTABLE_BUCKETS;i++)for(struct mrsh_hashtable_entry *e=table->buckets[i];e;e=e->next)n++;return n;
}
static void send_variable(const char *key,void *value,void *data) {
 struct stream *s=data;struct mrsh_variable *v=value;send_text(s,key);send_text(s,v->value);send_number(s,v->attribs);
}
static void send_function(const char *key,void *value,void *data) {
 struct stream *s=data;struct mrsh_function *v=value;send_text(s,key);send_node(s,&v->body->node);
}
static void send_alias(const char *key,void *value,void *data){send_text(data,key);send_text(data,value);}
pid_t sh_spawn_worker(struct mrsh_state *state,struct mrsh_node *node,int input,int output,bool null_input) {
 int fd=sh_anonymous_file();if(fd<0){perror("sh: worker context");return -1;}
 struct mrsh_state_priv *priv=state_get_priv(state);
 struct stream s={.fd=fd,.ok=true};send_number(&s,0x53484301);
 send_number(&s,state->options);send_number(&s,state->last_status);send_number(&s,priv->shell_pid);send_number(&s,priv->last_bg_pid);
 send_number(&s,state->frame->argc);
 for(int i=0;i<state->frame->argc;i++)send_text(&s,state->frame->argv[i]);
 send_number(&s,table_count(&priv->variables));mrsh_hashtable_for_each(&priv->variables,send_variable,&s);
 send_number(&s,table_count(&priv->functions));mrsh_hashtable_for_each(&priv->functions,send_function,&s);
 send_number(&s,table_count(&priv->aliases));mrsh_hashtable_for_each(&priv->aliases,send_alias,&s);
 send_node(&s,node);
 if(!s.ok || lseek(fd,0,SEEK_SET)<0){close(fd);errno=E2BIG;perror("sh: worker state");return -1;}
 posix_spawn_file_actions_t actions={0};int rc=posix_spawn_file_actions_init(&actions);bool ready=!rc;
 if(!rc && input>=0)rc=posix_spawn_file_actions_adddup2(&actions,input,0);
 if(!rc && input<0 && null_input)rc=posix_spawn_file_actions_addopen(&actions,0,"/dev/null",O_RDONLY,0);
 if(!rc && output>=0)rc=posix_spawn_file_actions_adddup2(&actions,output,1);
 if(!rc)rc=posix_spawn_file_actions_adddup2(&actions,fd,fd);
 char descriptor[16];snprintf(descriptor,sizeof descriptor,"%d",fd);
 char *argv[]={(char *)sh_executable,"--emblink-worker",descriptor,NULL};
 pid_t pid=-1;
 /* Worker shell state is explicit binary metadata. Only the already-authorized
  * namespace/cwd/fd view is inherited; no parent environment is injected. */
 char *empty[]={NULL};
 if(!rc)rc=posix_spawn(&pid,sh_executable,&actions,NULL,argv,empty);
 if(ready)posix_spawn_file_actions_destroy(&actions);
 close(fd);
 if(rc){fprintf(stderr,"sh: worker spawn: %s\n",strerror(rc));errno=rc;return -1;}
 return pid;
}
struct input {const unsigned char *p;size_t left;bool ok;};
static uint32_t take_number(struct input *r) {
 uint32_t n=0;if(!r->ok)return 0;
 if(r->left<4){r->ok=false;return 0;}memcpy(&n,r->p,4);r->p+=4;r->left-=4;return n;
}
static char *take_text(struct input *r) {
 uint32_t n=take_number(r);if(!r->ok || n>r->left || memchr(r->p,0,n)){r->ok=false;return NULL;}
 char *p=malloc((size_t)n+1);if(!p){r->ok=false;return NULL;}memcpy(p,r->p,n);p[n]=0;r->p+=n;r->left-=n;return p;
}
static struct mrsh_node *take_node(struct input *r) {
 uint32_t n=take_number(r);if(!r->ok || n>r->left){r->ok=false;return NULL;}
 struct mrsh_node *node=sh_ast_decode(r->p,n);if(!node){r->ok=false;return NULL;}r->p+=n;r->left-=n;return node;
}
int sh_worker_main(int fd) {
 sh_interrupt_init();
 struct stat st;
 if(fd<3 || fd>=64 || fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size<4 || st.st_size>SH_CONTEXT_MAX){fprintf(stderr,"sh: invalid worker descriptor\n");return 126;}
 size_t n=(size_t)st.st_size;char *data=malloc(n);if(!data)return 126;
 size_t done=0;while(done<n){ssize_t k=read(fd,data+done,n-done);if(k<0 && errno==EINTR)continue;if(k<=0)break;done+=(size_t)k;}
 close(fd);if(done!=n){free(data);return 126;}
 struct input r={.p=(const unsigned char *)data,.left=n,.ok=true};
 struct mrsh_state *state=mrsh_state_create();if(!state){free(data);return 126;}
 struct mrsh_state_priv *priv=state_get_priv(state);
 if(take_number(&r)!=0x53484301)r.ok=false;
 state->options=take_number(&r);state->last_status=(int32_t)take_number(&r);
 priv->shell_pid=take_number(&r);priv->last_bg_pid=take_number(&r);priv->child=true;
 if(state->options&~(MRSH_OPT_ALLEXPORT|MRSH_OPT_NOGLOB|MRSH_OPT_NOEXEC|MRSH_OPT_NOUNSET|MRSH_OPT_XTRACE))r.ok=false;
 unsigned argc=take_number(&r);if(!argc || argc>4096)r.ok=false;
 if(r.ok){state->frame->argv=calloc((size_t)argc+1,sizeof(char *));if(!state->frame->argv)r.ok=false;}
 for(unsigned i=0;i<argc && r.ok;i++){char *v=take_text(&r);if(r.ok)state->frame->argv[state->frame->argc++]=v;}
 unsigned count=take_number(&r);if(count>4096)r.ok=false;
 for(unsigned i=0;i<count && r.ok;i++) {
  char *key=take_text(&r),*value=take_text(&r);unsigned attr=take_number(&r);
  if(attr&~(MRSH_VAR_ATTRIB_EXPORT|MRSH_VAR_ATTRIB_READONLY))r.ok=false;
  if(r.ok)mrsh_env_set(state,key,value,attr);
  free(key);free(value);
 }
 count=take_number(&r);if(count>1024)r.ok=false;
 for(unsigned i=0;i<count && r.ok;i++) {
  char *key=take_text(&r);struct mrsh_node *node=take_node(&r);
  if(node && node->type!=MRSH_NODE_COMMAND)r.ok=false;
  if(r.ok){struct mrsh_function *f=calloc(1,sizeof *f);if(!f)r.ok=false;else {f->body=(struct mrsh_command *)node;node=NULL;function_destroy(mrsh_hashtable_set(&priv->functions,key,f));}}
  if(node){mrsh_node_destroy(node);}free(key);
 }
 count=take_number(&r);if(count>4096)r.ok=false;
 for(unsigned i=0;i<count && r.ok;i++) {
  char *key=take_text(&r),*value=take_text(&r);
  if(r.ok){free(mrsh_hashtable_set(&priv->aliases,key,value));value=NULL;}
  free(key);free(value);
 }
 struct mrsh_node *node=r.ok?take_node(&r):NULL;
 int status=126;
 if(r.ok && !r.left && node) {
  /* Restore libc selectors used only for temporary files; exported environment
   * construction still uses the exact restored shell variable attributes. */
  const char *home=mrsh_env_get(state,"HOME",NULL);if(home)setenv("HOME",home,1);
  const char *tmp=mrsh_env_get(state,"TMPDIR",NULL);if(tmp)setenv("TMPDIR",tmp,1);
  struct mrsh_context ctx={.state=state};
  switch(node->type) {
  case MRSH_NODE_PROGRAM:status=mrsh_run_program(state,(struct mrsh_program *)node);break;
  case MRSH_NODE_COMMAND:status=run_command(&ctx,(struct mrsh_command *)node);break;
  case MRSH_NODE_AND_OR_LIST:status=run_and_or_list(&ctx,(struct mrsh_and_or_list *)node);break;
  default:fprintf(stderr,"sh: invalid worker root\n");break;
  }
  if(state->exit>=0)status=state->exit;
  mrsh_run_exit_trap(state);
  if(status<0)status=125;
 } else fprintf(stderr,"sh: malformed worker context\n");
 if(node){mrsh_node_destroy(node);}free(data);mrsh_state_destroy(state);return status;
}
void sh_poll_background(struct mrsh_state *state) {
 struct mrsh_state_priv *priv=state_get_priv(state);
 for(size_t i=0;i<priv->processes.len;i++) {
  struct mrsh_process *p=priv->processes.data[i];if(p->terminated)continue;
  int status;pid_t pid=waitpid(p->pid,&status,WNOHANG);
  if(pid>0)update_process(state,pid,status);
 }
}
int sh_builtin_wait(struct mrsh_state *state,int argc,char **argv) {
 struct mrsh_state_priv *priv=state_get_priv(state);int result=0;
 for(int i=argc==1?0:1;argc==1?priv->processes.len>0:i<argc;argc==1?(void)0:(void)i++) {
  struct mrsh_process *p=NULL;
  if(argc==1)p=priv->processes.data[0];
  else {char *end;errno=0;long pid=strtol(argv[i],&end,10);
   if(errno || !*argv[i] || *end || pid<=0){fprintf(stderr,"wait: invalid PID\n");return 2;}
   for(size_t j=0;j<priv->processes.len;j++){struct mrsh_process *item=priv->processes.data[j];if(item->pid==pid){p=item;break;}}
  }
  if(!p){result=127;continue;}
  result=p->terminated?process_poll(p):sh_wait(p->pid);process_destroy(p);
 }
 return argc==1?0:result;
}
