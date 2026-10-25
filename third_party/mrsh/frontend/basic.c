/* Basic, strictly POSIX interactive line interface */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mrsh/shell.h>
#include <mrsh/parser.h>
#include "frontend.h"
#include <unistd.h>
#include <termios.h>
#ifdef EMBLINK_TARGET
#define getline __getline
#endif

void interactive_init(struct mrsh_state *state) {
	// no-op
}

size_t interactive_next(struct mrsh_state *state,
		char **line, const char *prompt) {
    const char *integration=getenv("EMBLINK_PROMPT");
    bool integrated=integration && !strcmp(integration,"1");
    struct termios terminal;
    bool software_echo=tcgetattr(STDIN_FILENO,&terminal)!=0 || !(terminal.c_lflag&ECHO);
    if(integrated) {
        char cwd[256];if(!getcwd(cwd,sizeof cwd))strcpy(cwd,"/");
        for(char *p=cwd;*p;p++)if((unsigned char)*p<32 || *p==127)*p='?';
        fprintf(stderr,"\033]133;A;cwd=%s\007",cwd);
    } else fprintf(stderr,"%s",prompt);
    fflush(stderr);
	size_t len = 0;
	char *_line = NULL;
	errno = 0;
	ssize_t n_read = getline(&_line, &len, stdin);
	if (n_read < 0) {
		free(_line);
		if (errno != 0) {
			perror("getline");
		}
		return 0;
	}
    if(integrated){if(software_echo)fputs(_line,stderr);fputs("\033]133;C\007",stderr);fflush(stderr);}
	*line = _line;
	return n_read;
}
