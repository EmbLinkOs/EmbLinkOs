/* capfs.c -- witness for the FILESYSTEM gate, including native and POSIX
 * spawn OPEN actions. Tries to open a file that exists
 * (/hello.txt) and reports the outcome, exactly like capgpu does for GPU:
 *   2  open granted (this process holds EMBK_CAP_FILESYSTEM)
 *   0  open refused with EPERM (it does not)
 *   3  some other failure (e.g. the file is missing) -- distinct so it can't be
 *      mistaken for the clean gate denial
 * Note it still has stdout: fds 0/1/2 are installed at spawn, not via open, so
 * a process without FILESYSTEM keeps its console -- it just cannot open more. */
#include "embk.h"
#include <spawn.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>

int main(void) {
    int fd = embk_open("/hello.txt", EMBK_O_RDONLY, 0);
    if (fd >= 0) { embk_close(fd); return 2; }   /* granted */
    if(fd==-1) {
        char *argv[]={"hello",NULL};
        const char *path="/data/apps/hello/hello.elf";
        struct embk_spawn_file_action action={0};
        action.kind=EMBK_SPAWN_ACTION_OPEN;action.target_fd=3;
        __builtin_strcpy(action.path,"/dev/null");
        int h=(int)embk_spawn(path,argv,&action,1);
        if(h>=0){(void)embk_wait(h);return 4;}
        if(h!=-1)return 3;
        posix_spawn_file_actions_t actions={0};
        if(posix_spawn_file_actions_init(&actions))return 3;
        int rc=posix_spawn_file_actions_addopen(&actions,3,"/hello.txt",O_RDONLY,0);
        pid_t pid=12345;
        if(!rc)rc=posix_spawn(&pid,path,&actions,NULL,argv,NULL);
        posix_spawn_file_actions_destroy(&actions);
        if(!rc){(void)waitpid(pid,NULL,0);return 4;}
        return rc==EPERM && pid==12345?0:3;
    }
    return 3;                                     /* other (missing file, ...) */
}
