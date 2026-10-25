#include <stdio.h>
#include <mrsh/shell.h>
int builtin_ulimit(struct mrsh_state *state,int argc,char **argv) {
 (void)state;(void)argc;(void)argv;
 fprintf(stderr,"ulimit: resource-limit control is not supported on this target\n");return 2;
}
