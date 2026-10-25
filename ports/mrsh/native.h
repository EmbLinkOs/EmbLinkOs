#ifndef EMBLINK_MRSH_NATIVE_H
#define EMBLINK_MRSH_NATIVE_H
#include <mrsh/shell.h>
#include <spawn.h>
#include "shell/task.h"
int sh_anonymous_file(void);
int sh_spawn_command(struct mrsh_state *,const char *,char **,const struct mrsh_simple_command *);
pid_t sh_spawn_worker(struct mrsh_state *,struct mrsh_node *,int,int,bool);
int sh_worker_main(int);
int sh_wait(pid_t);
int sh_interrupted(void);
void sh_interrupt_init(void);
void sh_set_foreground(const pid_t *,unsigned);
void sh_poll_background(struct mrsh_state *);
int sh_builtin_wait(struct mrsh_state *,int,char **);
int sh_extra_builtin(struct mrsh_state *,int,char **);
bool sh_has_extra_builtin(const char *);
extern const char *sh_executable;
#endif
