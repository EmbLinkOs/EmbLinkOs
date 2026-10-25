#include <stdio.h>
#include <unistd.h>
#include "builtin.h"
#include "mrsh_getopt.h"
#include "shell/path.h"

static const char exec_usage[] = "usage: exec [command [argument...]]\n";

int builtin_exec(struct mrsh_state *state, int argc, char *argv[]) {
	_mrsh_optind = 0;
	if (_mrsh_getopt(argc, argv, ":") != -1) {
		fprintf(stderr, "exec: unknown option -- %c\n", _mrsh_optopt);
		fprintf(stderr, exec_usage);
		return 1;
	}
	if (_mrsh_optind == argc) {
		return 0;
	}

    fprintf(stderr,"exec: in-place image replacement is not supported\n");
    return 126;
}
