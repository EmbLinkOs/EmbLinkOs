#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include "native.h"
int run_pipeline(struct mrsh_context *ctx,struct mrsh_pipeline *pl) {
 if(!pl->commands.len || pl->commands.len>16){fprintf(stderr,"sh: pipeline limit is 16 commands\n");return 2;}
 if(pl->commands.len==1){int ret=run_command(ctx,pl->commands.data[0]);return pl->bang && ret>=0?!ret:ret;}
 pid_t children[16];unsigned count=0;int input=-1,ret=0;
 for(size_t i=0;i<pl->commands.len;i++) {
  int fds[2]={-1,-1};
  if(i+1<pl->commands.len) {
   if(pipe(fds)<0){perror("sh: pipe");ret=TASK_STATUS_ERROR;break;}
   if(fcntl(fds[0],F_SETFD,FD_CLOEXEC) || fcntl(fds[1],F_SETFD,FD_CLOEXEC)){close(fds[0]);close(fds[1]);ret=TASK_STATUS_ERROR;break;}
  }
  struct mrsh_command *cmd=pl->commands.data[i];
  pid_t pid=sh_spawn_worker(ctx->state,&cmd->node,input,fds[1],false);
  if(input>=0)close(input);
  input=-1;
  if(fds[1]>=0)close(fds[1]);
  if(pid<0){if(fds[0]>=0)close(fds[0]);ret=TASK_STATUS_ERROR;break;}
  children[count++]=pid;input=fds[0];
 }
 if(input>=0)close(input);
 sh_set_foreground(children,count);
 int failed=ret<0;
 for(unsigned i=0;i<count;i++){int status=sh_wait(children[i]);if(!failed)ret=status;}
 sh_set_foreground(NULL,0);
 return pl->bang && ret>=0?!ret:ret;
}
