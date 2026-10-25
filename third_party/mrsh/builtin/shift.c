#include <errno.h>
#include <mrsh/builtin.h>
#include <mrsh/shell.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "builtin.h"

static const char shift_usage[] = "usage: shift [n]\n";

int builtin_shift(struct mrsh_state *state, int argc, char *argv[]) {
	if (argc > 2) {
		fprintf(stderr, shift_usage);
		return 1;
	}
	int n = 1;
	if (argc == 2) {
		char *endptr;
		errno = 0;
		long n_long = strtol(argv[1], &endptr, 10);
		if (*endptr != '\0' || errno != 0 || n_long>INT_MAX || n_long<0) {
			fprintf(stderr, shift_usage);
			if (!state->interactive) {
				state->exit = 1;
			}
			return 1;
		}
		n = (int)n_long;
	}
	if (n == 0) {
		return 0;
	} else if (n < 1) {
		fprintf(stderr, "shift: [n] must be positive\n");
		if (!state->interactive) {
			state->exit = 1;
		}
		return 1;
	} else if (n > state->frame->argc - 1) {
		fprintf(stderr, "shift: [n] must be less than $#\n");
		if (!state->interactive) {
			state->exit = 1;
		}
		return 1;
	}
    for(int i=1;i<=n;i++)free(state->frame->argv[i]);
    memmove(state->frame->argv+1,state->frame->argv+n+1,
            (size_t)(state->frame->argc-n-1)*sizeof(char *));
    state->frame->argc-=n;state->frame->argv[state->frame->argc]=NULL;
	return 0;
}
