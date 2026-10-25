#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <mrsh/buffer.h>
#include <mrsh/shell.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "shell/path.h"

char *expand_path(struct mrsh_state *state,const char *file,bool executable,bool default_path) {
    if(strchr(file,'/'))return strdup(file);
#ifdef EMBLINK_TARGET
    const char *standard="/system/bin:/bin";
#else
    const char *standard="/usr/bin:/bin";
#endif
    const char *path=default_path?standard:mrsh_env_get(state,"PATH",NULL);
    if(!path)path=standard;
    for(const char *part=path;;) {
        const char *end=strchr(part,':');size_t n=end?(size_t)(end-part):strlen(part);
        size_t size=n+(n?1:0)+strlen(file)+1;
        char *candidate=malloc(size);if(!candidate)return NULL;
        memcpy(candidate,part,n);size_t off=n;if(n)candidate[off++]='/';strcpy(candidate+off,file);
        if(access(candidate,executable?X_OK:R_OK)==0)return candidate;
        free(candidate);if(!end)break;part=end+1;
    }
    return NULL;
}

char *current_working_dir(void) {
	// POSIX doesn't provide a way to query the CWD size
	struct mrsh_buffer buf = {0};
	if (mrsh_buffer_reserve(&buf, 256) == NULL) {
		return NULL;
	}
	while (getcwd(buf.data, buf.cap) == NULL) {
		if (errno != ERANGE) {
			return NULL;
		}
		if (mrsh_buffer_reserve(&buf, buf.cap * 2) == NULL) {
			return NULL;
		}
	}
	return mrsh_buffer_steal(&buf);
}
