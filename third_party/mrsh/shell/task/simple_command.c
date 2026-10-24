#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <mrsh/ast.h>
#include <mrsh/builtin.h>
#include <mrsh/entry.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "shell/shell.h"
#include "shell/path.h"
#include "shell/redir.h"
#include "shell/word.h"
#include "shell/task.h"
#include "native.h"

static int run_process(struct mrsh_context *ctx,struct mrsh_simple_command *sc,char **argv) {
    char *path=expand_path(ctx->state,argv[0],true,false);
    if(!path){fprintf(stderr,"%s: not found\n",argv[0]);return 127;}
    int ret=sh_spawn_command(ctx->state,path,argv,sc);free(path);return ret;
}

struct saved_fd {int target,backup,flags;};
static int run_builtin(struct mrsh_context *ctx,struct mrsh_simple_command *sc,int argc,char **argv) {
    struct saved_fd saved[64];unsigned count=0;int ret=1;
    bool reserved[64]={true,true,true};
    if(sc->io_redirects.len>61){errno=E2BIG;perror("sh: redirections");return ret;}
    for(size_t i=0;i<sc->io_redirects.len;i++) {
        struct mrsh_io_redirect *r=sc->io_redirects.data[i];int fd=r->io_number;
        if(fd<0)fd=(r->op==MRSH_IO_GREAT || r->op==MRSH_IO_DGREAT || r->op==MRSH_IO_CLOBBER || r->op==MRSH_IO_GREATAND)?1:0;
        if(fd<0 || fd>=64){errno=EBADF;goto restore;}
        reserved[fd]=true;
        if(r->op==MRSH_IO_LESSAND || r->op==MRSH_IO_GREATAND) {
            char *text=mrsh_word_str(r->name),*end;long src=strtol(text,&end,10);
            if(*text && !*end && src>=0 && src<64)reserved[src]=true;
            free(text);
        }
    }
    for(size_t i=0;i<sc->io_redirects.len;i++) {
        struct mrsh_io_redirect *r=sc->io_redirects.data[i];int target=r->io_number;
        if(target<0)target=(r->op==MRSH_IO_GREAT || r->op==MRSH_IO_DGREAT || r->op==MRSH_IO_CLOBBER || r->op==MRSH_IO_GREATAND)?1:0;
        bool found=false;for(unsigned k=0;k<count;k++)if(saved[k].target==target)found=true;
        if(!found) {
            int flags=fcntl(target,F_GETFD),backup=-1;
            if(flags<0 && errno!=EBADF)goto restore;
            if(flags>=0) {
                for(int minimum=3;minimum<64;minimum++) {
                    if(reserved[minimum])continue;
                    backup=fcntl(target,F_DUPFD,minimum);if(backup<0)goto restore;
                    if(reserved[backup]){minimum=backup;close(backup);backup=-1;continue;}
                    reserved[backup]=true;break;
                }
                if(backup<0){errno=EMFILE;goto restore;}
                if(fcntl(backup,F_SETFD,FD_CLOEXEC)<0){close(backup);goto restore;}
            }
            saved[count++]=(struct saved_fd){target,backup,flags};
        }
        char *name=mrsh_word_str(r->name);
        bool duplication=r->op==MRSH_IO_LESSAND || r->op==MRSH_IO_GREATAND;
        if(duplication && !strcmp(name,"-")){free(name);if(close(target) && errno!=EBADF)goto restore;continue;}
        free(name);
        int actual,fd=process_redir(r,&actual);if(fd<0)goto restore;
        if(dup2(fd,target)<0){if(!duplication)close(fd);goto restore;}
        /* A newly-opened descriptor can equal a previously closed target.
         * Its CLOEXEC must still clear for explicit redirection. */
        if(fcntl(target,F_SETFD,0)<0){if(!duplication && fd!=target)close(fd);goto restore;}
        if(!duplication && fd!=target)close(fd);
    }
    ret=sh_has_extra_builtin(argv[0])?sh_extra_builtin(ctx->state,argc,argv):mrsh_run_builtin(ctx->state,argc,argv);
    fflush(stdout);fflush(stderr);
restore:
    for(unsigned i=count;i>0;i--) {
        struct saved_fd *s=&saved[i-1];
        if(s->backup>=0){if(dup2(s->backup,s->target)<0)ret=TASK_STATUS_ERROR;else fcntl(s->target,F_SETFD,s->flags);close(s->backup);}
        else (void)close(s->target);
    }
    /* stdio EOF/error bits belong to the redirected stream, not the restored
     * one. A read builtin's EOF must not poison the following prompt. */
    clearerr(stdin);clearerr(stdout);clearerr(stderr);
    return ret;
}

static int run_assignments(struct mrsh_context *ctx, struct mrsh_array *assignments) {
	for (size_t i = 0; i < assignments->len; ++i) {
		struct mrsh_assignment *assign = assignments->data[i];
		char *new_value = mrsh_word_str(assign->value);
		uint32_t attribs = MRSH_VAR_ATTRIB_NONE;
		if ((ctx->state->options & MRSH_OPT_ALLEXPORT)) {
			attribs = MRSH_VAR_ATTRIB_EXPORT;
		}
		uint32_t prev_attribs = 0;
		if (mrsh_env_get(ctx->state, assign->name, &prev_attribs) != NULL
				&& (prev_attribs & MRSH_VAR_ATTRIB_READONLY)) {
			free(new_value);
			fprintf(stderr, "cannot modify readonly variable %s\n",
				assign->name);
			return TASK_STATUS_ERROR;
		}
		mrsh_env_set(ctx->state, assign->name, new_value, attribs);
		free(new_value);
	}

	return 0;
}

static int expand_assignments(struct mrsh_context *ctx,
		struct mrsh_array *assignments) {
	for (size_t i = 0; i < assignments->len; ++i) {
		struct mrsh_assignment *assign = assignments->data[i];
		expand_tilde(ctx->state, &assign->value, true);
		int ret = run_word(ctx, &assign->value);
		if (ret < 0) {
			return ret;
		}
	}
	return 0;
}

static struct mrsh_simple_command *copy_simple_command(
		const struct mrsh_simple_command *sc) {
	struct mrsh_command *cmd = mrsh_command_copy(&sc->command);
	return mrsh_command_get_simple_command(cmd);
}

static int expand_redirects(struct mrsh_context *ctx,struct mrsh_simple_command *sc) {
    struct mrsh_state *state=ctx->state;int ret;
	for (size_t i = 0; i < sc->io_redirects.len; ++i) {
		struct mrsh_io_redirect *redir = sc->io_redirects.data[i];
		expand_tilde(state, &redir->name, false);
		ret = run_word(ctx, &redir->name);
		if (ret < 0) {
			return ret;
		}
		for (size_t j = 0; j < redir->here_document.len; ++j) {
			struct mrsh_word **line_word_ptr =
				(struct mrsh_word **)&redir->here_document.data[j];
			expand_tilde(state, line_word_ptr, false);
			ret = run_word(ctx, line_word_ptr);
			if (ret < 0) {
				return ret;
			}
		}
	}

    return 0;
}

int run_simple_command(struct mrsh_context *ctx, struct mrsh_simple_command *sc) {
	struct mrsh_state *state = ctx->state;
	struct mrsh_state_priv *priv = state_get_priv(state);
    priv->did_substitute=false;

    if(sc->name==NULL) {
        struct mrsh_simple_command *copy=copy_simple_command(sc);
        int status=expand_assignments(ctx,&copy->assignments);
        if(status>=0)status=run_assignments(ctx,&copy->assignments);
        if(status>=0)status=expand_redirects(ctx,copy);
        int substitution=priv->did_substitute?priv->substitution_status:0;
        if(status>=0 && copy->io_redirects.len) {
            char *args[]={":",NULL};status=run_builtin(ctx,copy,1,args);
        }
        mrsh_command_destroy(&copy->command);
        return status?status:substitution;
    }

	// Copy the command from the AST, because during expansion and substitution
	// we'll mutate the tree
	sc = copy_simple_command(sc);

	struct mrsh_array args = {0};
	int ret = expand_word(ctx, sc->name, &args);
	if (ret < 0) {
		return ret;
	}
	for (size_t i = 0; i < sc->arguments.len; ++i) {
		struct mrsh_word *arg = sc->arguments.data[i];
		ret = expand_word(ctx, arg, &args);
		if (ret < 0) {
			return ret;
		}
	}
	assert(args.len > 0);
	mrsh_array_add(&args, NULL);

	ret = expand_assignments(ctx, &sc->assignments);
	if (ret < 0) {
		return ret;
	}

    ret=expand_redirects(ctx,sc);if(ret<0)return ret;

	char **argv = (char **)args.data;
	int argc = args.len - 1; // argv is NULL-terminated
	const char *argv_0 = argv[0];

	if ((state->options & MRSH_OPT_XTRACE)) {
		char *ps4 = mrsh_get_ps4(state);
		fprintf(stderr, "%s", ps4);
		for (int i = 0; i < argc; ++i) {
			fprintf(stderr, "%s%s", i > 0 ? " " : "", argv[i]);
		}
		fprintf(stderr, "\n");
		free(ps4);
	}

	ret = -1;
	const struct mrsh_function *fn_def =
		mrsh_hashtable_get(&priv->functions, argv_0);
	if (fn_def != NULL) {
		push_frame(state, argc, (const char **)argv);
		// fn_def may be free'd during run_command when overwritten with another
		// function, so we need to copy it.
		struct mrsh_command *body = mrsh_command_copy(fn_def->body);
		ret = run_command(ctx, body);
        struct mrsh_call_frame_priv *frame=call_frame_get_priv(state->frame);
        if(ret==TASK_STATUS_INTERRUPTED && frame->branch_control==MRSH_BRANCH_RETURN)ret=state->last_status;
		mrsh_command_destroy(body);
		pop_frame(state);
	} else if ((mrsh_has_builtin(argv_0) || sh_has_extra_builtin(argv_0))) {
		ret = run_builtin(ctx, sc, argc, argv);
	} else {
		ret = run_process(ctx, sc, argv);
	}

	mrsh_command_destroy(&sc->command);
	for (size_t i = 0; i < args.len; ++i) {
		free(args.data[i]);
	}
	mrsh_array_finish(&args);
	return ret;
}
