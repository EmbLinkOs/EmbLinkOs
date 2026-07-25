/* process.h — emlibc's process and capability surface: the part of the OS a
 * POSIX libc fundamentally cannot express (docs/EMLIBC_Requirements.md §3).
 *
 * EmbLink has no fork and no exec. A process is created by SPAWN with an
 * explicit list of file-actions — a child receives ONLY what the parent names
 * (argv, env, fds, capabilities), nothing ambient. Children are named by
 * unforgeable HANDLES, not raw pids: wait/kill/cancel operate on the handle the
 * parent was handed by spawn.
 *
 * Capabilities are coarse resource-class authority (capabilities.h /
 * EMBX §5.6). A process can read its OWN set (em_getcaps) and, at spawn, grant
 * a child a SUBSET of its own — never a superset. That monotonic invariant is
 * the kernel's, surfaced here in EmbLink's own vocabulary, not a POSIX costume.
 */
#ifndef _EMLIBC_PROCESS_H
#define _EMLIBC_PROCESS_H

#include <stddef.h>

/* Capability IDs — bit position == cap_id (capabilities.h, EMBX §5.6). */
enum em_cap {
    EM_CAP_FILESYSTEM = 1,
    EM_CAP_NETWORK    = 2,
    EM_CAP_GPU        = 3,
    EM_CAP_AUDIO      = 4,
    EM_CAP_CAMERA     = 5,
    EM_CAP_USB        = 6,
    EM_CAP_SERIAL     = 7,
    EM_CAP_RAWDISK    = 8,
    EM_CAP_KERNEL_EXT = 9,
    EM_CAP_DEBUG      = 10,
};

#define EM_CAP_BIT(id)  (1UL << (id))

/* This process's own capability set (bitmask of EM_CAP_BIT(id)). */
unsigned long em_getcaps(void);
/* 1 if this process holds capability `c`, else 0. */
int  em_have_cap(enum em_cap c);
/* Stable lowercase name ("filesystem", "network", …) for logs. */
const char *em_cap_name(enum em_cap c);

/* A spawn file-action. Binary-compatible with the kernel's
 * struct spawn_file_action (copied raw across the syscall) — do not reorder. */
typedef struct em_spawn_action {
    unsigned char kind;
    int           target_fd;
    char          path[256];
    int           flags;
    unsigned int  mode;
    int           src_obj_handle;
} em_spawn_action;

#define EM_ACTION_OPEN       1   /* open `path` onto target_fd in the child */
#define EM_ACTION_SET_CAPS   5   /* attenuate the child's caps to `flags` (a mask) */

/* Open `path` (flags = emlibc O_* from <unistd.h>) onto `child_fd` in the child. */
void em_action_open(em_spawn_action *a, int child_fd, const char *path, int flags);
/* Request that the child be born holding exactly `cap_mask`. The kernel refuses
 * (spawn fails EPERM) if the mask is not a subset of THIS process's caps. */
void em_action_set_caps(em_spawn_action *a, unsigned long cap_mask);

/* Spawn `path`. argv/envp are explicit (envp may be NULL = no environment).
 * `actions` is applied in order. Returns a child HANDLE (>= 0) — pass it to
 * em_wait/em_kill/em_cancel/em_alive — or a negative value with errno set. */
int em_spawn(const char *path, char *const argv[], char *const envp[],
             const em_spawn_action *actions, int n_actions);

int em_wait(int handle);    /* block; return exit code (or -1 if killed); frees the handle */
int em_kill(int handle);    /* uncatchably terminate; handle stays valid for em_wait */
int em_cancel(int handle);  /* politely ask to stop (blocking syscalls fail ECANCELED) */
int em_alive(int handle);   /* 1 if the child is still running, 0 if it has exited */

#endif /* _EMLIBC_PROCESS_H */
