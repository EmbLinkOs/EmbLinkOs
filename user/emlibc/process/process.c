/* process.c — emlibc. The capability + handle-based-spawn surface, over the
 * EmbLink syscall ABI. This is the part of the OS newlib has no vocabulary for:
 * a process inspecting its own authority and handing a child a strict subset.
 */

#include <process.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "embk_syscall.h"

extern int embk_errno_from_kernel(int64_t ret);

unsigned long em_getcaps(void)
{
    return (unsigned long)embk_syscall0(EMBK_SYS_getcaps);
}

int em_have_cap(enum em_cap c)
{
    return (int)((em_getcaps() >> (int)c) & 1UL);
}

const char *em_cap_name(enum em_cap c)
{
    switch (c) {
    case EM_CAP_FILESYSTEM: return "filesystem";
    case EM_CAP_NETWORK:    return "network";
    case EM_CAP_GPU:        return "gpu";
    case EM_CAP_AUDIO:      return "audio";
    case EM_CAP_CAMERA:     return "camera";
    case EM_CAP_USB:        return "usb";
    case EM_CAP_SERIAL:     return "serial";
    case EM_CAP_RAWDISK:    return "rawdisk";
    case EM_CAP_KERNEL_EXT: return "kernel-ext";
    case EM_CAP_DEBUG:      return "debug";
    default:                return "?";
    }
}

void em_action_open(em_spawn_action *a, int child_fd, const char *path, int flags)
{
    memset(a, 0, sizeof *a);
    a->kind = EM_ACTION_OPEN;
    a->target_fd = child_fd;
    size_t n = strlen(path);
    if (n >= sizeof a->path) n = sizeof a->path - 1;
    memcpy(a->path, path, n);
    a->path[n] = 0;
    a->flags = flags;
    a->mode = 0644;
}

void em_action_set_caps(em_spawn_action *a, unsigned long cap_mask)
{
    memset(a, 0, sizeof *a);
    a->kind = EM_ACTION_SET_CAPS;
    a->flags = (int)cap_mask;      /* the ABI carries the mask in `flags` */
}

int em_spawn(const char *path, char *const argv[], char *const envp[],
             const em_spawn_action *actions, int n_actions)
{
    int argc = 0;
    if (argv) while (argv[argc]) argc++;
    int64_t r = embk_syscall6(EMBK_SYS_spawn,
                              (int64_t)(intptr_t)path,
                              (int64_t)(intptr_t)argv, argc,
                              (int64_t)(intptr_t)actions, n_actions,
                              (int64_t)(intptr_t)envp);
    if (embk_is_err(r)) { errno = embk_errno_from_kernel(r); return (int)r; }
    return (int)r;                  /* the child handle */
}

int em_wait(int handle)
{
    int64_t r = embk_syscall1(EMBK_SYS_wait, handle);
    /* -1 is a legitimate result (the child was killed); only a small negative
     * errno-range value is a wait failure. */
    if (embk_is_err(r) && r != -1) errno = embk_errno_from_kernel(r);
    return (int)r;
}

int em_kill(int handle)   { return (int)embk_syscall1(EMBK_SYS_kill, handle); }
int em_cancel(int handle) { return (int)embk_syscall1(EMBK_SYS_cancel, handle); }
int em_alive(int handle)  { return (int)embk_syscall1(EMBK_SYS_proc_alive, handle); }
