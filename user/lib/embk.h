/* user/embk.h — the EmbLink SDK.
 *
 * newlib gives an EmbLink program the standard C library (printf, malloc,
 * string.h, ...). It CANNOT give it the things POSIX doesn't model but this
 * OS is built around: spawn()-based process creation with file-action
 * redirection, ring-3 threads with join, cooperative yield, capability
 * handles, and the native readdir/stat surface. This header is that layer --
 * the typed, documented EmbLink-native API, sitting directly on the raw
 * int-0x80 ABI (user/embk_syscall.h).
 *
 * HEADER-ONLY (every function is `static inline`): usable from a freestanding
 * program (user/init.c -- its own _start, no libc) AND from a newlib program
 * (link it alongside libc for the best of both: printf/malloc from newlib,
 * spawn/threads from here). No separate .o to link, no libc dependency.
 *
 * Return convention (inherited from the kernel): >= 0 is success (an fd, a
 * handle, a tid, a byte count, a break address); a small negative value is
 * -EMBK_* (see kernel/include/errno.h). Callers test `ret < 0`. This is the
 * RAW kernel convention -- unlike user/syscalls.c's POSIX stubs, these do NOT
 * set errno; they hand the negative code straight back, which is what a
 * freestanding program wants (no libc errno to touch). */

#ifndef __EMBK_H__
#define __EMBK_H__

#include <stdint.h>
#include <stddef.h>
#include "embk_syscall.h"

/* ==================================================================== */
/* open() flags -- the KERNEL's values (kernel/fs/fd.h). A newlib program  */
/* that wants POSIX open() uses <fcntl.h>+libc; these are for freestanding */
/* callers going straight through embk_open() (and match what the kernel   */
/* actually interprets, unlike newlib's differently-numbered O_* macros).   */
/* ==================================================================== */
#define EMBK_O_RDONLY   0x0000
#define EMBK_O_WRONLY   0x0001
#define EMBK_O_RDWR     0x0002
#define EMBK_O_CREAT    0x0040
#define EMBK_O_EXCL     0x0080
#define EMBK_O_TRUNC    0x0200
#define EMBK_O_APPEND   0x0400

/* Directory-entry / stat type tags (kernel VFS_DT_*). */
#define EMBK_DT_UNKNOWN 0
#define EMBK_DT_REG     1
#define EMBK_DT_DIR     2
#define EMBK_DT_LNK     3

/* Seek origins for embk_lseek() (kernel vfs_fd_seek convention). */
#define EMBK_SEEK_SET   0
#define EMBK_SEEK_CUR   1
#define EMBK_SEEK_END   2

#define EMBK_TTY_COOKED 0
#define EMBK_TTY_RAW    1

/* ==================================================================== */
/* Types shared with the kernel by value (hand-synced -- no shared kernel  */
/* header). Same compiler + default layout on both sides, so a copy_to/    */
/* from_user of one of these matches byte-for-byte.                        */
/* ==================================================================== */

/* One spawn() file action: "open PATH onto TARGET_FD in the child before it
 * runs" (EMBK_SPAWN_ACTION_OPEN), or "install the pipe end behind
 * SRC_OBJ_HANDLE as the child's TARGET_FD" (EMBK_SPAWN_ACTION_INSTALL_OBJ --
 * the pipe->stdio bridge for shell pipelines). Mirrors the kernel's
 * struct spawn_file_action (kernel/process/spawn.h) FIELD-FOR-FIELD: the
 * kernel copies this struct raw, so the two must grow together (and every
 * action-passing app rebuilt when they do). */
#define EMBK_SPAWN_ACTION_OPEN        1
#define EMBK_SPAWN_ACTION_INSTALL_OBJ 4   /* 2/3 are surface/channel inherits */
#define EMBK_SPAWN_ACTION_SET_CAPS    5   /* attenuate child caps; `flags` = mask */
#define EMBK_SPAWN_ACTION_NS_BIND     7   /* narrow child namespace; `path`=prefix, `flags`=mode */
#define EMBK_SPAWN_ACTION_NEW_SESSION 8   /* child leads a new session for user `path` */

/* Coarse resource-class capabilities (kernel capabilities.h, EMBX spec §5.6).
 * A child's set is a subset of its parent's; declare a subset via a
 * SET_CAPS spawn action, or read your own with embk_getcaps(). */
#define EMBK_CAP_FILESYSTEM  1
#define EMBK_CAP_NETWORK     2
#define EMBK_CAP_GPU         3
#define EMBK_CAP_AUDIO       4
#define EMBK_CAP_CAMERA      5
#define EMBK_CAP_USB         6
#define EMBK_CAP_SERIAL      7
#define EMBK_CAP_RAWDISK     8
#define EMBK_CAP_KERNEL_EXT  9
#define EMBK_CAP_DEBUG      10
#define EMBK_CAP_SESSION    11   /* open a session for a user; init only -- see embk_action_new_session */
/* TURN THE MACHINE OFF. A class of its own because it is unlike every other
 * one here: the rest gate what a process may TOUCH, and this gates something
 * that ends every other process on the machine at once. The desktop shell
 * holds it; an application has no business with it. */
#define EMBK_CAP_POWER      12
#define EMBK_CAP_BIT(id)     (1u << (id))
struct embk_spawn_file_action {
    unsigned char kind;         /* EMBK_SPAWN_ACTION_* */
    int           target_fd;    /* fd this action populates in the child */
    char          path[256];    /* OPEN: NUL-terminated path */
    int           flags;        /* OPEN: EMBK_O_* */
    unsigned int  mode;         /* OPEN: creation mode if EMBK_O_CREAT */
    int           src_obj_handle; /* INSTALL_OBJ: pipe-end handle in the
                                   * PARENT's table (from embk_pipe). COPY
                                   * semantics: the parent keeps its handle
                                   * and must embk_close_handle() it, or the
                                   * reader never sees EOF. */
};

/* One directory entry from embk_readdir() (kernel struct sys_dirent). */
struct embk_dirent {
    uint64_t ino;               /* fs-private object id */
    uint8_t  type;              /* EMBK_DT_* */
    char     name[59];          /* NUL-terminated (truncated if longer) */
};

/* File metadata from embk_stat()/embk_fstat() (kernel struct vfs_stat,
 * mirrored FIELD-FOR-FIELD -- the kernel copies it raw; keep positions). */
struct embk_stat {
    uint8_t  type;              /* EMBK_DT_* */
    uint32_t mode;              /* POSIX-shaped st_mode */
    uint64_t size;              /* logical size in bytes */
    uint64_t mtime;             /* last-modified, seconds since epoch; 0 = untracked */
    uint64_t nlink;             /* hard-link count */
};

/* ==================================================================== */
/* File descriptors / VFS                                                 */
/* ==================================================================== */

static inline int64_t embk_write(int fd, const void *buf, size_t len) {
    return embk_syscall3(EMBK_SYS_write, fd, (int64_t)(intptr_t)buf, (int64_t)len);
}
static inline int64_t embk_read(int fd, void *buf, size_t len) {
    return embk_syscall3(EMBK_SYS_read, fd, (int64_t)(intptr_t)buf, (int64_t)len);
}
static inline int64_t embk_open(const char *path, int flags, unsigned mode) {
    return embk_syscall3(EMBK_SYS_open, (int64_t)(intptr_t)path, flags, mode);
}
static inline int64_t embk_close(int fd) {
    return embk_syscall1(EMBK_SYS_close, fd);
}
/* Create a pipe. Writes {read_handle, write_handle} -- typed OBJ-HANDLES for
 * spawn's INSTALL_OBJ action, NOT fds (you cannot embk_read/embk_write them
 * directly; install one as a child's fd 0/1, or into your own fd table). */
static inline int embk_pipe(int out_handles[2]) {
    return (int)embk_syscall1(EMBK_SYS_pipe, (int64_t)(intptr_t)out_handles);
}
/* Release an obj-handle (pipe end, channel end, surface) via the kernel's
 * generic kind dispatch. For pipes this is what delivers EOF: after
 * installing ends into children, close YOUR copies or the reader hangs. */
static inline int embk_close_handle(int handle) {
    return (int)embk_syscall1(EMBK_SYS_handle_close, handle);
}
/* Install a pipe-end handle into the CALLER's OWN fd table at target_fd
 * (the self-install half of INSTALL_OBJ -- how the shell turns its read
 * handle of a pipeline's output into an fd it can read()). COPY semantics:
 * the handle stays alive; embk_close_handle it separately. Returns the fd. */
static inline int embk_fd_install_obj(int handle, int target_fd) {
    return (int)embk_syscall2(EMBK_SYS_fd_install_obj, handle, target_fd);
}
/* Bytes readable from fd RIGHT NOW without blocking (0 = nothing yet, NOT
 * EOF), or -EMBK_*. Poll a pipe from a render loop: avail > 0 guarantees
 * the next read() returns immediately. */
static inline int64_t embk_fd_avail(int fd) {
    return embk_syscall1(EMBK_SYS_fd_avail, fd);
}
/* Remove a file (the shell's rm). 0 or -EMBK_*. */
static inline int embk_unlink(const char *path) {
    return (int)embk_syscall1(EMBK_SYS_unlink, (int64_t)(intptr_t)path);
}
/* Create a directory (the shell's mkdir). 0 or -EMBK_*. */
static inline int embk_mkdir(const char *path) {
    return (int)embk_syscall1(EMBK_SYS_mkdir, (int64_t)(intptr_t)path);
}
/* Remove an EMPTY directory (the shell's rmdir; the fs refuses non-empty,
 * never recurses). 0 or -EMBK_*. */
static inline int embk_rmdir(const char *path) {
    return (int)embk_syscall1(EMBK_SYS_rmdir, (int64_t)(intptr_t)path);
}

/* One `ps` row. Mirrors the kernel's struct process_info (process.h)
 * FIELD-FOR-FIELD -- sys_proc_list copies it raw; grow both together.
 * state: 0 UNUSED, 1 READY, 2 RUNNING, 3 BLOCKED, 4 ZOMBIE. */
struct embk_proc_info {
    uint32_t      pid;
    uint32_t      parent_pid;   /* 0 if none */
    int           state;        /* enum process_state's values */
    uint8_t       priority;
    int           exit_code;    /* meaningful only for ZOMBIE */
    unsigned char is_kthread;
    uint64_t      cpu_ns;       /* CPU actually consumed, all threads. Time
                                 * EXECUTING -- a thread halted or blocked is
                                 * charged nothing, which is the difference
                                 * between this and wall time. */
    uint32_t      session_id;   /* whose it is: 0 = the system session */
    char          name[24];     /* the binary's basename; "" for kernel threads.
                                 * A NAME FOR READING, never for deciding: a
                                 * process's rights here are its namespace and
                                 * its capabilities, and this string is not a
                                 * second, weaker way to say who it is. Before
                                 * it, `ps` was a column of bare numbers. */
};
/* Snapshot every live process (the shell's ps). Returns the count written
 * (<= max), or -EMBK_*. */
static inline int embk_proc_list(struct embk_proc_info *out, int max) {
    return (int)embk_syscall2(EMBK_SYS_proc_list, (int64_t)(intptr_t)out, max);
}
/* Kill by PID (the shell's kill). Unlike embk_kill (handle-scoped to your
 * own children), this names any pid -- but only one in YOUR session may be
 * killed this way (-EMBK_EPERM otherwise, and always for a kernel thread).
 * 0, or -EMBK_ENOENT if no such live pid. */
static inline int embk_proc_kill(uint32_t pid) {
    return (int)embk_syscall1(EMBK_SYS_proc_kill, (int64_t)pid);
}
static inline int64_t embk_lseek(int fd, int64_t offset, int whence) {
    return embk_syscall3(EMBK_SYS_lseek, fd, offset, whence);
}
static inline int64_t embk_stat(const char *path, struct embk_stat *out) {
    return embk_syscall2(EMBK_SYS_stat, (int64_t)(intptr_t)path, (int64_t)(intptr_t)out);
}
static inline int64_t embk_fstat(int fd, struct embk_stat *out) {
    return embk_syscall2(EMBK_SYS_fstat, fd, (int64_t)(intptr_t)out);
}
/* Read up to `max` directory entries of `path` into `out`; returns the count
 * actually written, or -EMBK_*. Not resumable -- one call walks the whole
 * directory (see the kernel's sys_readdir comment). */
static inline int64_t embk_readdir(const char *path, struct embk_dirent *out, uint32_t max) {
    return embk_syscall3(EMBK_SYS_readdir, (int64_t)(intptr_t)path,
                         (int64_t)(intptr_t)out, (int64_t)max);
}

/* ==================================================================== */
/* Memory                                                                 */
/* ==================================================================== */

/* Adjust the heap break by `incr` bytes; returns the PREVIOUS break (newly
 * available region starts there) or -EMBK_* on failure. A newlib program
 * gets malloc() on top of this for free; a freestanding one can carve its
 * own allocator out of it. */
static inline int64_t embk_sbrk(int64_t incr) {
    return embk_syscall1(EMBK_SYS_sbrk, incr);
}

/* ==================================================================== */
/* Processes -- spawn()-based, NOT fork()/exec(). This is the model POSIX  */
/* can't express, and the reason this SDK exists.                         */
/* ==================================================================== */

/* Launch `path` as a new process WITH an explicit environment.
 *
 * `argv` is NULL-terminated (argv[0] is conventionally the program name); argc
 * is counted here so callers never touch the raw ABI. `actions`/`n_actions`
 * pre-open files onto specific fds in the child before it runs (may be NULL/0).
 *
 * `envp` is a NULL-terminated array of "KEY=VALUE", or **NULL to give the child
 * NO environment** -- which is the default and an honest one. THE CHILD INHERITS
 * NOTHING: unlike fork()+exec(), where the environment tags along whether or not
 * anyone chose to pass it, here a variable reaches a child only because a parent
 * named it. That is the same rule as file-actions and obj-handles. If you want a
 * child to see your own environment, pass `environ` explicitly -- and that is
 * the point: it is a decision, visible at the call site.
 *
 * Returns an opaque per-caller HANDLE (not a raw pid -- capability-scoped, only
 * this process can name the child via embk_wait/embk_kill), or -EMBK_*.
 * Over the kernel's SPAWN_ENVP_MAX/SPAWN_ENVP_BYTES_MAX budget -> -EMBK_E2BIG
 * (rejected, never silently truncated). */
static inline int64_t embk_spawn_env(const char *path, char *const argv[],
                                     char *const envp[],
                                     const struct embk_spawn_file_action *actions,
                                     int n_actions) {
    int argc = 0;
    if (argv) {
        while (argv[argc]) argc++;
    }
    return embk_syscall6(EMBK_SYS_spawn, (int64_t)(intptr_t)path,
                         (int64_t)(intptr_t)argv, argc,
                         (int64_t)(intptr_t)actions, n_actions,
                         (int64_t)(intptr_t)envp);
}

/* embk_spawn_env() with NO environment for the child. This is the plain spawn:
 * every existing caller keeps its exact behaviour, because "no environment" is
 * what they always got. */
/* This process's own capability set (a bitmask of EMBK_CAP_BIT(id)). */
static inline unsigned long embk_getcaps(void) {
    return (unsigned long)embk_syscall0(EMBK_SYS_getcaps);
}

/* ---- Networking (M4): the native socket surface -------------------------
 * Gated on EMBK_CAP_NETWORK. A socket is a REAL fd, so read()/write()/close()
 * work on it directly (which is what the BSD sockets shim in <embk_socket.h>
 * maps onto for ports). IPs are HOST order: 10.0.2.15 == 0x0A00020F. */
/* select()/poll readiness bits -- MUST match kernel/fs/fd.h POLL*. */
#define EMBK_POLLIN   0x0001
#define EMBK_POLLOUT  0x0004
#define EMBK_POLLERR  0x0008
#define EMBK_POLLHUP  0x0010
#define EMBK_POLLNVAL 0x0020

/* O_NONBLOCK fd flag: sys_fcntl cmd 1 = get (1/0), 2 = set from arg. */
static inline int embk_fcntl_get_nonblock(int fd) {
    return (int)embk_syscall3(EMBK_SYS_fcntl, fd, 1, 0);
}
static inline int embk_fcntl_set_nonblock(int fd, int on) {
    return (int)embk_syscall3(EMBK_SYS_fcntl, fd, 2, on ? 1 : 0);
}
/* Ready POLL* bits for one fd right now, without waiting. */
static inline int embk_fd_poll(int fd, int events) {
    return (int)embk_syscall2(EMBK_SYS_fd_poll, fd, events);
}

static inline int embk_net_socket(int type) {      /* type: 1=stream(TCP) 2=dgram(UDP); -> fd */
    return (int)embk_syscall1(EMBK_SYS_net_socket, type);
}
static inline int embk_net_connect(int fd, unsigned int ip_host, unsigned short port) {
    return (int)embk_syscall3(EMBK_SYS_net_connect, fd, (int64_t)ip_host, (int64_t)port);
}
static inline int embk_net_resolve(const char *name, unsigned int *out_ip) {
    return (int)embk_syscall2(EMBK_SYS_net_resolve,
                              (int64_t)(long)name, (int64_t)(long)out_ip);
}
/* Server side: bind a local port, listen, accept (returns a new socket fd). */
static inline int embk_net_bind(int fd, unsigned short port) {
    return (int)embk_syscall2(EMBK_SYS_net_bind, fd, (int64_t)port);
}
static inline int embk_net_listen(int fd, int backlog) {
    return (int)embk_syscall2(EMBK_SYS_net_listen, fd, backlog);
}
static inline int embk_net_accept(int fd) {
    return (int)embk_syscall1(EMBK_SYS_net_accept, fd);
}
/* UDP datagrams. sendto: ip HOST order. recvfrom: out_ip/out_port may be 0. */
static inline int embk_net_sendto(int fd, unsigned int ip_host, unsigned short port,
                                  const void *data, unsigned long len) {
    return (int)embk_syscall5(EMBK_SYS_net_sendto, fd, (int64_t)ip_host, (int64_t)port,
                              (int64_t)(long)data, (int64_t)len);
}
static inline int embk_net_recvfrom(int fd, void *buf, unsigned long cap,
                                    unsigned int *out_ip, unsigned short *out_port) {
    return (int)embk_syscall5(EMBK_SYS_net_recvfrom, fd, (int64_t)(long)buf, (int64_t)cap,
                              (int64_t)(long)out_ip, (int64_t)(long)out_port);
}

/* Fill `a` as a SET_CAPS action requesting `cap_mask` for the child. Add it to
 * the actions array you pass to embk_spawn; the kernel enforces the mask is a
 * subset of THIS process's caps and refuses (-EMBK_EPERM) otherwise. */
static inline void embk_action_set_caps(struct embk_spawn_file_action *a,
                                        unsigned cap_mask) {
    a->kind = EMBK_SPAWN_ACTION_SET_CAPS;
    a->target_fd = 0; a->path[0] = 0; a->flags = (int)cap_mask;
    a->mode = 0; a->src_obj_handle = 0;
}

/* The kernel's error codes -- a syscall that fails returns one NEGATED. The
 * values are the kernel's own (kernel/include/errno.h); only the ones a
 * program is likely to branch on are mirrored here. Guarded: other EmbLink
 * headers define a few of them too. */
#ifndef EMBK_EPERM
#define EMBK_EPERM    1
#endif
#ifndef EMBK_ENOENT
#define EMBK_ENOENT   2
#endif
#ifndef EMBK_EACCES
#define EMBK_EACCES  13
#endif
#ifndef EMBK_EEXIST
#define EMBK_EEXIST  17
#endif
#ifndef EMBK_EINVAL
#define EMBK_EINVAL  22
#endif
#ifndef EMBK_E2BIG
#define EMBK_E2BIG    7
#endif

/* --- sessions: whose a process is -------------------------------------------
 *
 * A session is opened when init authenticates someone: the desktop is spawned
 * with a NEW_SESSION action naming the user, and every process started from it
 * inherits the session and can never change it. When the leader exits the
 * session ends and whatever it left running is stopped. Session 0 is the
 * system (kernel, init, the login screen). */
#define EMBK_EXIT_LOGOUT 0x4C4F47          /* a session leader's exit code after a logout */

struct embk_session_info {
    uint32_t id;                  /* 0 = the system session */
    uint32_t leader_pid;          /* the process whose exit ends it */
    char     user[32];            /* "" in the system session */
};

/* Who am I -- the kernel's answer, not $USER (which any parent can set). */
static inline int embk_session_info(struct embk_session_info *out) {
    return (int)embk_syscall1(EMBK_SYS_session_info, (int64_t)(intptr_t)out);
}

/* Log out: end session `sid` (0 = your own). Every process in it is stopped,
 * the leader last with EMBK_EXIT_LOGOUT -- and you with them, so for your own
 * session this does not return. Another session needs EMBK_CAP_SESSION. */
static inline int embk_session_end(uint32_t sid) {
    return (int)embk_syscall1(EMBK_SYS_session_end, (int64_t)sid);
}

/* Fill `a` as a NEW_SESSION action: the child leads a new session for `user`
 * ([a-z0-9_-], 1..31). Needs EMBK_CAP_SESSION, which only init holds; the
 * child is born without it. */
static inline void embk_action_new_session(struct embk_spawn_file_action *a, const char *user) {
    a->kind = EMBK_SPAWN_ACTION_NEW_SESSION;
    a->target_fd = 0;
    int i = 0;
    if (user) { while (user[i] && i < 31) { a->path[i] = user[i]; i++; } }
    a->path[i] = 0;
    a->flags = 0; a->mode = 0; a->src_obj_handle = 0;
}

/* Namespace grant modes (mirror kernel enum ns_mode). */
#define EMBK_NS_RW 0
#define EMBK_NS_RO 1

/* Fill `a` as an NS_BIND action: grant the child a namespace binding for the
 * absolute prefix `prefix` at `ns_mode` (EMBK_NS_RW / EMBK_NS_RO). Adding ANY
 * NS_BIND action NARROWS the child to EXACTLY the granted prefixes (absent =>
 * the child inherits your whole view). The kernel resolves `prefix` in YOUR
 * namespace, so you can only grant what you can name, at a mode no wider than
 * you hold; an ungrantable prefix fails the spawn. See docs/USERSPACE_v2.md
 * UP2b. Copy `prefix` into the fixed path buffer (no libc assumed here). */
static inline void embk_action_ns_bind(struct embk_spawn_file_action *a,
                                       const char *prefix, int ns_mode) {
    a->kind = EMBK_SPAWN_ACTION_NS_BIND;
    a->target_fd = 0;
    int i = 0;
    if (prefix) { while (prefix[i] && i < 255) { a->path[i] = prefix[i]; i++; } }
    a->path[i] = 0;
    a->flags = ns_mode ? EMBK_NS_RO : EMBK_NS_RW;
    a->mode = 0; a->src_obj_handle = 0;
}

static inline int64_t embk_spawn(const char *path, char *const argv[],
                                 const struct embk_spawn_file_action *actions,
                                 int n_actions) {
    return embk_spawn_env(path, argv, (char *const *)0, actions, n_actions);
}

/* Block until the child named by `handle` exits; returns its exit code (or
 * -1 if it was killed), and frees the handle. -EMBK_* for a bad handle. */
static inline int64_t embk_wait(int handle) {
    return embk_syscall1(EMBK_SYS_wait, handle);
}

/* Uncatchably terminate the child named by `handle`. The handle stays valid
 * for a following embk_wait() to collect the exit status. */
static inline int64_t embk_kill(int handle) {
    return embk_syscall1(EMBK_SYS_kill, handle);
}

/* Ask the child named by `handle` to STOP -- politely. See docs/INTERRUPTION.md.
 *
 * The child's blocking syscalls start failing -EMBK_ECANCELED, so it wakes out of
 * whatever it was waiting on and can unwind, clean up, and exit through error
 * paths it already has. NO handler runs and nothing is injected into its control
 * flow: it learns at a syscall boundary, or by calling embk_cancelled().
 *
 * Handle-scoped like embk_kill: you may only cancel a child you spawned. There
 * is no cancel-by-pid, deliberately -- that would be ambient authority over
 * something you were never handed.
 *
 * ⚠️ This may be IGNORED FOREVER. A child that never blocks and never polls will
 * not notice. Cancellation is a request; embk_kill() is the answer when the
 * request is declined, and deciding how long to wait is YOUR policy. The pairing
 * is SIGTERM-then-SIGKILL, minus the ambient authority.
 *
 * Cancelling an already-exited child succeeds (its end state already holds). */
static inline int64_t embk_cancel(int handle) {
    return embk_syscall1(EMBK_SYS_cancel, handle);
}

/* The one errno a caller of embk_wait() has to recognise by name. Mirrors
 * kernel/include/errno.h, which is the source of truth; guarded because other
 * headers in this tree define the codes they need the same way. */
#ifndef EMBK_ESTOPPED
#define EMBK_ESTOPPED 129
#endif

/* ---------------------------------------------------------------------------
 * STOP a child, and start it again. The ^Z half of job control.
 *
 * ^C says "do not finish this". ^Z says "finish it later": the child keeps its
 * memory, its file descriptors and its place in your handle table, and simply
 * stops being scheduled. Handle-scoped like embk_cancel and embk_proc_kill --
 * a child you spawned, never an arbitrary pid.
 *
 * WHAT CHANGES FOR THE PARENT: embk_wait() on a stopped child returns
 * -EMBK_ESTOPPED rather than blocking. It has to -- a stopped process is not
 * going to exit, so a wait for its exit code would never return. Treat that
 * return as "it is still there, frozen", not as a failure.
 *
 * Returns how many threads changed state; 0 means it was already in that state
 * (or every thread of it is pinned and cannot be frozen). */
static inline int64_t embk_suspend(int handle) {
    return embk_syscall2(EMBK_SYS_suspend, handle, 0);
}
static inline int64_t embk_resume(int handle) {
    return embk_syscall2(EMBK_SYS_suspend, handle, 1);
}

/* Unpredictable bytes from the kernel's CSPRNG (HMAC_DRBG over SHA-256, seeded
 * from a hardware generator where the processor has one and from clocks and
 * timing jitter regardless). At most 256 per call; returns how many, which is
 * always `len` unless the pointer is bad. The libc getentropy() is built on
 * this where there is no unprivileged hardware instruction to build it on. */
static inline int64_t embk_getrandom(void *buf, size_t len) {
    return embk_syscall2(EMBK_SYS_getrandom, (int64_t)(intptr_t)buf, (int64_t)len);
}

/* 1 if THIS process has been cancelled, 0 if not.
 *
 * For compute loops that make no syscalls -- with nothing injected, a process
 * that never blocks would otherwise never find out. Reading does NOT clear it
 * (the flag is sticky), so cleanup that itself blocks still sees ECANCELED
 * rather than hanging on a flag that cleared itself.
 *
 * The -1 is LOAD-BEARING: the kernel reads the first argument as
 * handle-or-minus-one (see embk_child_cancelled). A syscall0 here would hand it
 * whatever garbage sat in rdi -- occasionally a valid handle. */
static inline int64_t embk_cancelled(void) {
    return embk_syscall1(EMBK_SYS_cancelled, -1);
}

/* 1 if the CHILD named by spawn handle `handle` has been cancelled, 0 if not,
 * -EMBK_EINVAL if the handle names nothing of yours.
 *
 * The escalation primitive (docs/INTERRUPTION.md §4.3). A parent that delegated
 * ^C to a child never sees the keystroke -- the kernel consumed it -- so this is
 * how it starts the grace clock: cancelled + still alive + grace expired =>
 * embk_kill(). Without it, "cancelled but declining" is indistinguishable from
 * "healthy but slow", and a parent would either never escalate or kill innocent
 * long-running children. Handle-scoped, and strictly weaker than embk_cancel. */
static inline int64_t embk_child_cancelled(int handle) {
    return embk_syscall1(EMBK_SYS_cancelled, handle);
}

/* Route console Ctrl-C to the child named by `handle`; pass -1 to reclaim it.
 * See docs/INTERRUPTION.md.
 *
 * While routed, a ^C at the keyboard CANCELS that child (embk_cancel semantics)
 * instead of arriving as a byte. When nothing is routed, ^C is just an ordinary
 * 0x03 input character.
 *
 * This is a DELEGATION -- there is no "foreground process" on EmbLink, no
 * session and no process group. A shell that runs a command hands over the right
 * to be interrupted, and takes it back when the child exits. **Reclaim it**, or
 * ^C keeps pointing at a pid that may already be gone (harmless -- cancelling a
 * corpse succeeds -- but then YOUR ^C does nothing, which looks like a bug).
 *
 * Only a handle you hold is accepted, so you cannot aim ^C at a process you were
 * never given. The slot is global (one console) and last-writer-wins: a
 * single-user concession, same class as embk_proc_kill.
 *
 * Cancellation may be declined -- pair it with embk_kill() as the backstop. */
#define EMBK_INTR_ROUTE_SELF  (-2)   /* ^C means ME (a shell at its own prompt) */
#define EMBK_INTR_ROUTE_NONE  (-1)   /* reclaim: ^C is a byte again */
/* handle >= 0 names a child. SELF used to be 0, which is also a process's first
 * spawn handle -- a shell's first child's ^C went to the shell. */
static inline int64_t embk_console_interrupt_route(int handle) {
    return embk_syscall1(EMBK_SYS_console_interrupt_route, handle);
}

static inline int64_t embk_getpid(void) {
    return embk_syscall0(EMBK_SYS_getpid);
}

/* End the calling PROCESS (all its threads) with `code`. Never returns. */
static inline void embk_exit(int code) {
    embk_syscall1(EMBK_SYS_exit, code);
    for (;;) { }
}

/* ==================================================================== */
/* Threads -- additional ring-3 threads of the CALLING process, sharing    */
/* its address space (the pthread-shaped primitive newlib has no syscall    */
/* for on a bare kernel).                                                  */
/* ==================================================================== */

/* Start `entry` as a new thread of this process; `arg` arrives as entry's
 * first parameter (RDI). Returns a tid (>= 0) or -EMBK_*. The tid names a
 * thread only within THIS process.
 *
 * `entry` MUST END BY CALLING embk_thread_exit(). It is not called like an
 * ordinary function and there is NO RETURN ADDRESS on its stack -- returning
 * jumps to 0 and the thread dies with a page fault at instruction address
 * zero, which looks like a wild function pointer and is nothing of the kind.
 * A trampoline in the libc could make a plain `return` work; until one does,
 * this is the contract. */
static inline int64_t embk_thread_create(void (*entry)(long arg), long arg) {
    return embk_syscall2(EMBK_SYS_thread_create, (int64_t)(intptr_t)entry, arg);
}

/* Block until thread `tid` of this process exits; returns its exit code. */
static inline int64_t embk_thread_join(int tid) {
    return embk_syscall1(EMBK_SYS_thread_join, tid);
}

/* End the calling THREAD (not the whole process, unless it's the last one)
 * with `code`. Never returns. */
static inline void embk_thread_exit(int code) {
    embk_syscall1(EMBK_SYS_thread_exit, code);
    for (;;) { }
}

/* ==================================================================== */
/* Scheduling                                                             */
/* ==================================================================== */

/* Voluntarily give up the rest of this timeslice to the next runnable
 * thread. Cooperative complement to the timer's preemption. */
static inline void embk_yield(void) {
    embk_syscall0(EMBK_SYS_yield);
}

/* ==================================================================== */
/* Surfaces -- shared-memory pixel buffers (EmbLink UI Piece 1). A surface */
/* is a kernel-owned, refcounted run of pages that a UI client and the      */
/* compositor both map (their own VAs, same physical memory) to share       */
/* pixels with zero copies. Named by a typed handle in the process's        */
/* obj_handle table. See kernel/gfx/surface.h.                              */
/* ==================================================================== */

#define EMBK_PIXFMT_BGRA8888_PRE 1   /* premultiplied alpha; only one impl'd */

/* spawn() file-action kind: transfer+map a surface the parent holds into
 * the child (target_fd carries the parent's surface handle). The minimal
 * handle_transfer for Piece 1. */
#define EMBK_SPAWN_ACTION_INHERIT_SURFACE 2

/* Geometry the kernel fills on create/map so both sides agree without
 * re-querying. Mirrors struct surface_info (kernel/gfx/surface.h). */
struct embk_surface_info {
    uint32_t width, height, stride, format, n_buffers;
    uint64_t buffer_size;   /* bytes per buffer; buffer i is at base + i*this */
};

/* Create a surface and map it into THIS process (the client). Returns a
 * surface handle (>= 0) or -EMBK_*. `out` is filled with the geometry. */
static inline int embk_surface_create(uint32_t w, uint32_t h, uint32_t fmt,
                                      uint32_t n_buffers, struct embk_surface_info *out) {
    return (int)embk_syscall5(EMBK_SYS_surface_create, w, h, fmt, n_buffers,
                              (int64_t)(intptr_t)out);
}

/* Map a surface this process holds a handle to (e.g. one inherited at spawn)
 * into this address space. Returns the base VA (buffer 0), or a negative
 * -EMBK_* (test with < 0). Fills `out` with geometry. */
static inline int64_t embk_surface_map(int handle, struct embk_surface_info *out) {
    return embk_syscall2(EMBK_SYS_surface_map, handle, (int64_t)(intptr_t)out);
}

/* Get a buffer index the client may draw into (owner==CLIENT), or
 * -EMBK_EAGAIN if all are held by the compositor (B2). */
static inline int embk_surface_acquire(int handle) {
    return (int)embk_syscall1(EMBK_SYS_surface_acquire, handle);
}
/* Post `idx` as the newest frame (client -> compositor). */
static inline int embk_surface_commit(int handle, int idx) {
    return (int)embk_syscall2(EMBK_SYS_surface_commit, handle, idx);
}
/* Return `idx` to the client (compositor -> client). */
static inline int embk_surface_release(int handle, int idx) {
    return (int)embk_syscall2(EMBK_SYS_surface_release, handle, idx);
}
/* Drop this process's handle + mapping (refcount--; frees at 0). */
static inline int embk_surface_destroy(int handle) {
    return (int)embk_syscall1(EMBK_SYS_surface_destroy, handle);
}

/* Direct present: blit a premultiplied-BGRA8888 pixel buffer (w*h, tight) to
 * the centre of the framebuffer and flush. A minimal single-fullscreen-app
 * path standing in for a compositor. Returns 0 or -EMBK_*. */
static inline int embk_ui_present(const void *pixels, uint32_t w, uint32_t h) {
    return (int)embk_syscall3(EMBK_SYS_ui_present, (int64_t)(intptr_t)pixels, w, h);
}

/* Present only a surface-local sub-rectangle (rx,ry,rw,rh) -- the interactive
 * fast path, so a cursor that moved a few pixels doesn't re-upload the whole
 * surface. Same pixel buffer (w*h tight BGRA8888_PRE) as embk_ui_present. */
static inline int embk_ui_present_rect(const void *pixels, uint32_t w, uint32_t h,
                                       uint32_t rx, uint32_t ry, uint32_t rw, uint32_t rh) {
    int64_t dims = ((int64_t)(w & 0xFFFF) << 16) | (h & 0xFFFF);
    int64_t rect = ((int64_t)(rx & 0xFFFF) << 48) | ((int64_t)(ry & 0xFFFF) << 32)
                 | ((int64_t)(rw & 0xFFFF) << 16) |  (int64_t)(rh & 0xFFFF);
    return (int)embk_syscall3(EMBK_SYS_ui_present_rect, (int64_t)(intptr_t)pixels, dims, rect);
}

/* Pointer (mouse) state for the UI event loop. x/y are screen pixels; buttons
 * is a bitmask of EMBK_MOUSE_*. */
#define EMBK_MOUSE_LEFT   0x01
#define EMBK_MOUSE_RIGHT  0x02
#define EMBK_MOUSE_MIDDLE 0x04

/* Private single-byte key codes the kernel keyboard driver emits for the
 * extended (0xE0-prefixed) navigation keys -- kept in lockstep with
 * kernel/drivers/input/keyboard.c's EK_*. A text widget reads these from the
 * same byte stream as typed characters (ui_input_char) to move its cursor.
 * Backspace is the usual 0x08, Enter 0x0A, Tab 0x09. */
#define EMBK_KEY_LEFT   0x11
#define EMBK_KEY_RIGHT  0x12
#define EMBK_KEY_UP     0x13
#define EMBK_KEY_DOWN   0x14
#define EMBK_KEY_HOME   0x02
#define EMBK_KEY_END    0x05
#define EMBK_KEY_DEL    0x7F
#define EMBK_KEY_PGUP   0x0E
#define EMBK_KEY_PGDN   0x0F

/* The EDITING commands, in the same byte stream as the text. 0xF8..0xFF can
 * never occur in valid UTF-8 at any position, which is what makes it safe to
 * carry them there -- and carrying them there is what keeps them in ORDER with
 * the typed characters, so a paste between two keystrokes lands between them.
 *
 * They come from the GUI/Super key, not Ctrl, and that is a standing rule of
 * this system rather than a preference: Ctrl+C is the console INTERRUPT here
 * (docs/INTERRUPTION.md) and is consumed long before any application sees it.
 * CTRL BELONGS TO THE TERMINAL, THE GUI KEY BELONGS TO THE INTERFACE.
 * Mirrors EK_* in kernel/drivers/input/keyboard.h. */
#define EMBK_KEY_SEL_LEFT   0xF8   /* Shift+Left  */
#define EMBK_KEY_SEL_RIGHT  0xF9   /* Shift+Right */
#define EMBK_KEY_SEL_HOME   0xFA   /* Shift+Home  */
#define EMBK_KEY_SEL_END    0xFB   /* Shift+End   */
#define EMBK_KEY_SEL_ALL    0xFC   /* GUI+A       */
#define EMBK_KEY_COPY       0xFD   /* GUI+C       */
#define EMBK_KEY_CUT        0xFE   /* GUI+X       */
#define EMBK_KEY_PASTE      0xFF   /* GUI+V       */
#define EMBK_KEY_UNDO       0xF6   /* GUI+Z */
#define EMBK_KEY_REDO       0xF7   /* GUI+Shift+Z, GUI+Y */

/* ---- key EVENTS (make/break + modifiers) --------------------------------
 * A SECOND stream beside the characters above, not a replacement. The char
 * stream answers "what did the user type"; it cannot answer "what key is down"
 * because C0 is full -- Ctrl+letter owns 0x01-0x1A, so EMBK_KEY_UP (0x13) and
 * Ctrl+S are literally the same byte, and F-keys/releases have no byte at all.
 * Keep using embk_key_poll() for text; use this for real key state.
 * Mirrors EKC_, EKM_ and struct key_event in kernel/drivers/input/keyboard.h --
 * change one, change both. */
#define EMBK_KC_F1     0x101   /* F2..F12 follow consecutively */
#define EMBK_KC_F12    0x10C
#define EMBK_KC_INS    0x110
#define EMBK_KC_LWIN   0x111
#define EMBK_KC_RWIN   0x112
#define EMBK_KC_MENU   0x113
#define EMBK_KC_LEFT   0x120
#define EMBK_KC_RIGHT  0x121
#define EMBK_KC_UP     0x122
#define EMBK_KC_DOWN   0x123
#define EMBK_KC_HOME   0x124
#define EMBK_KC_END    0x125
#define EMBK_KC_PGUP   0x126
#define EMBK_KC_PGDN   0x127
#define EMBK_KC_DEL    0x128
#define EMBK_KC_LSHIFT 0x130
#define EMBK_KC_LCTRL  0x132
#define EMBK_KC_LALT   0x134
#define EMBK_KC_CAPS   0x140
#define EMBK_KC_NUM    0x141
#define EMBK_KC_SCROLL 0x142

#define EMBK_KM_SHIFT  0x01
#define EMBK_KM_CTRL   0x02
#define EMBK_KM_ALT    0x04
#define EMBK_KM_GUI    0x08
#define EMBK_KM_CAPS   0x10
#define EMBK_KM_NUM    0x20
#define EMBK_KM_SCROLL 0x40

struct embk_key_event {
    unsigned short code;      /* unshifted ASCII, or an EMBK_KC_* */
    unsigned char  mods;      /* EMBK_KM_* at event time */
    unsigned char  pressed;   /* 1 = pressed, 0 = released */
};

/* THE KEYBOARD LAYOUT. Kernel policy, because the driver is what turns a
 * scancode into a character -- so this is an ask, not a setting userspace can
 * keep to itself.
 *
 *   name != NULL : switch to it ("us", "azerty", "dvorak")
 *   out  != NULL : receive the CURRENT layout's name
 *
 * 0, or -EMBK_EINVAL for a name nothing matches -- and on a bad name the
 * layout is left alone, so a typo cannot leave the machine unable to type. */
static inline int embk_kbd_layout(const char *name, char *out, int cap) {
    return (int)embk_syscall3(EMBK_SYS_kbd_layout, (int64_t)(intptr_t)name,
                              (int64_t)(intptr_t)out, cap);
}

/* WHAT WINDOWS EXIST. Mirrors the kernel's struct comp_win_info field for
 * field -- sys_win_list copies it raw; grow both together.
 *
 * The answer is SCOPED TO YOUR SESSION and front-most first, and it leaves out
 * what a person cannot switch to: the wallpaper, widgets, the translucent
 * strips (menu bar, notifier) and anything that never named itself.
 *
 * TWO NAMES, and using the wrong one is the mistake to avoid. `title` is what
 * the app calls this window; SHOW that. `app` is the basename of the binary;
 * COMPARE that. A title changes as the person works -- open a document and the
 * editor's window becomes the document's name -- so anything that tries to
 * recognise an app by its title recognises it only until it is used. */
#define EMBK_WIN_MINIMIZED  0x1
#define EMBK_WIN_FOCUSED    0x2

struct embk_win_info {
    uint32_t pid;
    uint32_t id;
    uint32_t flags;      /* EMBK_WIN_* */
    char     title[32];  /* COMP_TITLE_MAX + 1 -- show this */
    char     app[24];    /* the binary's basename -- match on this */
};

static inline int embk_win_list(struct embk_win_info *out, int max) {
    return (int)embk_syscall2(EMBK_SYS_win_list, (int64_t)(intptr_t)out, max);
}

/* Bring that process's windows forward, un-minimising them. The switcher's
 * other half. -EMBK_EPERM for a pid outside your session -- you may raise what
 * you were allowed to see.
 *
 * Distinct from embk_win_restore, which takes a SPAWN HANDLE and so only works
 * for a child you started yourself (the dock's click-to-restore). */
/* TURN THE MACHINE OFF (0) or RESTART IT (1).
 *
 * Does not return on success. -EMBK_EPERM without EMBK_CAP_POWER, which the
 * desktop shell holds and an application should not: this ends every process
 * on the machine, not just yours. A negative result otherwise means the
 * platform has no mechanism -- reported rather than hung, because "this
 * machine cannot power itself off" is something a person can act on. */
static inline int embk_power(int what) {
    return (int)embk_syscall1(EMBK_SYS_power, what);
}
static inline int embk_power_off(void)     { return embk_power(0); }
static inline int embk_power_restart(void) { return embk_power(1); }

static inline int embk_win_raise(uint32_t pid) {
    return (int)embk_syscall1(EMBK_SYS_win_raise, pid);
}

/* The basename of an exec path: "/system/apps/term.elf" -> "term.elf". What you
 * compare against embk_win_info::app. Returns a pointer INTO `path`. */
static inline const char *embk_basename(const char *path) {
    const char *b = path ? path : "";
    for (const char *c = b; *c; c++) if (*c == '/') b = c + 1;
    return b;
}

/* 1 = got an event, 0 = queue empty. Never blocks. */
static inline int embk_key_event_poll(struct embk_key_event *ev) {
    return (int)embk_syscall1(EMBK_SYS_key_event_poll, (uint64_t)(uintptr_t)ev);
}
/* What is held RIGHT NOW (EMBK_KM_*), which events alone cannot tell you if
 * you missed the press. */
static inline int embk_key_mods(void) {
    return (int)embk_syscall0(EMBK_SYS_key_mods);
}
struct embk_ui_input { int32_t x, y; uint32_t buttons; int32_t wheel; };
static inline int embk_ui_input(struct embk_ui_input *out) {
    return (int)embk_syscall1(EMBK_SYS_ui_input, (int64_t)(intptr_t)out);
}

/* Non-blocking keystroke poll for the UI loop: returns the next ASCII byte
 * ('\b'=0x08 backspace, '\n' enter), or 0 when nothing is pending. Drain it in
 * a loop each frame and feed chars to the UI via ui_input_char(). */
static inline int embk_key_poll(void) {
    return (int)embk_syscall0(EMBK_SYS_key_poll);
}

/* Grab (1) or release (0) exclusive keyboard: while grabbed, the kernel shell
 * stops consuming keystrokes so this app gets them all. Auto-released on exit. */
static inline int embk_key_grab(int on) {
    return (int)embk_syscall1(EMBK_SYS_key_grab, on);
}

/* Monotonic milliseconds since boot -- the clock a UI animator ticks on. */
/* Seconds since the Unix epoch, from the CMOS RTC -- the only wall clock this
 * kernel has. Distinct from embk_uptime_ms, which is monotonic since BOOT and
 * therefore useless for anything that must outlive the machine being on: a
 * cookie's expiry, a cache's freshness, a file's age. */
static inline uint64_t embk_now_unix(void) {
    uint64_t tv[2] = { 0, 0 };
    if (embk_syscall1(EMBK_SYS_gettimeofday, (int64_t)(intptr_t)tv) != 0) return 0;
    return tv[0];
}

static inline uint64_t embk_uptime_ms(void) {
    return (uint64_t)embk_syscall0(EMBK_SYS_uptime_ms);
}

/* ==================================================================== */
/* Window compositor (EmbLink UI Piece 2). A WINDOW is a positioned tile of */
/* client-rendered pixels the kernel composites over a desktop with a title  */
/* bar and z-order. Unlike embk_ui_present (one centred surface), many        */
/* windows from many processes coexist. Pixels are BGRA8888_PRE (premult),    */
/* same as surfaces: a uint32 is 0xAARRGGBB.                                  */
/* ==================================================================== */

/* Create a window: content `cw` x `ch`, window frame's top-left at screen
 * (x,y) (the title bar occupies the first rows of the frame, content below).
 * Returns a window id (>0) or a negative -errno. */
static inline int embk_win_create(uint32_t cw, uint32_t ch, int32_t x, int32_t y,
                                  const char *title) {
    return (int)embk_syscall5(EMBK_SYS_win_create, cw, ch, x, y,
                              (int64_t)(intptr_t)title);
}

/* Zero-copy window: the kernel maps the window's pixel pages into THIS process
 * and returns the base pointer in *out_pixels. Render directly into it, then
 * call embk_win_present/_rect (which becomes a damage-only call -- no copy).
 * Returns a window id (>0) or a negative -errno. */
static inline int embk_win_create_shared(uint32_t cw, uint32_t ch, int32_t x, int32_t y,
                                         const char *title, void **out_pixels) {
    uint64_t va = 0;
    int id = (int)embk_syscall6(EMBK_SYS_win_create, cw, ch, x, y,
                                (int64_t)(intptr_t)title, (int64_t)(intptr_t)&va);
    if (id >= 0 && out_pixels) *out_pixels = (void *)(intptr_t)va;
    return id;
}

/* Window-style flags for embk_win_create_shared_ex (high bits of the cw arg). */
#define EMBK_WINF_CHROMELESS (1ULL << 32)   /* no kernel bar/close/border: the app
                                             * draws its own chrome (EmUI Window/
                                             * WindowBar) and moves itself. */
#define EMBK_WINF_WIDGET     (1ULL << 33)   /* DESKTOP WIDGET: chromeless AND kept
                                             * in a z-band above the desktop but
                                             * below every app window. */
#define EMBK_WINF_GLASS      (1ULL << 34)   /* GLASS: chromeless AND the compositor
                                             * blurs the backdrop behind the window
                                             * and composites its translucent pixels
                                             * over it (frosted acrylic). */
#define EMBK_WINF_TRANSLUCENT (1ULL << 35)  /* TRANSLUCENT: chromeless, per-pixel
                                             * transparent, NO blur -- composited
                                             * over the sharp backdrop. A thin bar
                                             * with a tall invisible dropdown canvas. */

/* Resize a shared window's content to w x h. The window's pixel pages are
 * REPLACED: *out_pixels receives the NEW mapping base and the old pointer is
 * dead the moment this returns. Returns the window id, or -EMBK_*. */
static inline int embk_win_resize(int id, uint32_t w, uint32_t h, void **out_pixels) {
    uint64_t va = 0;
    int rc = (int)embk_syscall4(EMBK_SYS_win_resize, id, w, h, (int64_t)(intptr_t)&va);
    if (rc >= 0 && out_pixels) *out_pixels = (void *)(intptr_t)va;
    return rc;
}

/* Zero-copy window with style flags. Same as embk_win_create_shared plus
 * EMBK_WINF_* or'd into `flags`. */
static inline int embk_win_create_shared_ex(uint32_t cw, uint32_t ch, int32_t x, int32_t y,
                                            const char *title, uint64_t flags,
                                            void **out_pixels) {
    uint64_t va = 0;
    int id = (int)embk_syscall6(EMBK_SYS_win_create, (int64_t)(flags | cw), ch, x, y,
                                (int64_t)(intptr_t)title, (int64_t)(intptr_t)&va);
    if (id >= 0 && out_pixels) *out_pixels = (void *)(intptr_t)va;
    return id;
}

/* Present the whole content buffer (cw*ch tight BGRA) to window `id`. */
static inline int embk_win_present(int id, const void *pixels,
                                   uint32_t cw, uint32_t ch) {
    int64_t dims = ((int64_t)(cw & 0xFFFF) << 16) | (ch & 0xFFFF);
    return (int)embk_syscall4(EMBK_SYS_win_present, id,
                              (int64_t)(intptr_t)pixels, dims, 0);
}

/* Present only a content-local sub-rectangle (rx,ry,rw,rh) -- the fast path,
 * so a small change doesn't re-upload the whole window. Same cw*ch buffer. */
static inline int embk_win_present_rect(int id, const void *pixels,
                                        uint32_t cw, uint32_t ch,
                                        uint32_t rx, uint32_t ry,
                                        uint32_t rw, uint32_t rh) {
    int64_t dims = ((int64_t)(cw & 0xFFFF) << 16) | (ch & 0xFFFF);
    int64_t rect = ((int64_t)(rx & 0xFFFF) << 48) | ((int64_t)(ry & 0xFFFF) << 32)
                 | ((int64_t)(rw & 0xFFFF) << 16) |  (int64_t)(rh & 0xFFFF);
    return (int)embk_syscall4(EMBK_SYS_win_present, id,
                              (int64_t)(intptr_t)pixels, dims, rect);
}

/* Move a window's frame top-left to screen (x,y); also raises it to the front. */
static inline int embk_win_move(int id, int32_t x, int32_t y) {
    return (int)embk_syscall3(EMBK_SYS_win_move, id, x, y);
}

/* Destroy a window and erase it from the desktop. */
static inline int embk_win_destroy(int id) {
    return (int)embk_syscall1(EMBK_SYS_win_destroy, id);
}

/* Create the full-screen chromeless HOME/desktop window (zero-copy): sized to
 * the framebuffer, pinned at the back, no title bar. The pixel base is returned
 * in out_pixels and the screen size in out_w and out_h. Only one app should hold
 * the desktop (the home launcher). Returns a window id (>0) or a negative -errno. */
static inline int embk_win_create_desktop(void **out_pixels, uint32_t *out_w, uint32_t *out_h) {
    uint64_t va = 0; uint32_t w = 0, h = 0;
    int id = (int)embk_syscall3(EMBK_SYS_win_create_desktop,
                                (int64_t)(intptr_t)&va, (int64_t)(intptr_t)&w,
                                (int64_t)(intptr_t)&h);
    if (id >= 0) {
        if (out_pixels) *out_pixels = (void *)(intptr_t)va;
        if (out_w) *out_w = w;
        if (out_h) *out_h = h;
    }
    return id;
}

/* Sleep at least `ms` milliseconds, YIELDING the CPU the whole time. This is
 * how a UI event loop paces itself -- a volatile spin loop burns the app's
 * whole scheduler slice and starves every other process; this costs ~nothing.
 * Granularity is a scheduler round trip, not a hard timer. */
static inline int embk_sleep_ms(uint64_t ms) {
    return (int)embk_syscall1(EMBK_SYS_sleep_ms, (int64_t)ms);
}

/* ---------------------------------------------------------------------------
 * DECLARE THIS THREAD'S RATE: "I must run once every `period_ms`, and I need
 * about `budget_ms` of CPU each time."
 *
 * This is what a compositor and an audio thread have that ordinary code does
 * not: a cadence, and a small, known amount of work per beat. Saying so lets
 * the scheduler run you close to your beat under load, where a plain sleep
 * loop waits its ordinary turn behind whatever else is busy -- which for a
 * 16 ms frame and three busy neighbours is a dropped frame every time.
 *
 * It is a request about TIMING, not importance, and it cannot make you run
 * more than budget_ms per period ahead of anyone. Past that you are simply an
 * ordinary thread again until the next period, so declaring one honestly costs
 * you nothing and declaring one dishonestly gains you nothing.
 *
 * budget_ms == 0 means "half the period" -- generous, and the right answer when
 * you have not measured. period_ms == 0 gives the reservation back.
 *
 * Returns 0, or -EBUSY if the machine has already promised out as much of
 * itself as it is willing to. THAT IS NOT A FAILURE YOU SHOULD DIE ON: it
 * means run the ordinary way, exactly as every program did before this
 * existed. Check it, and carry on either way. */
static inline int embk_sched_period(uint32_t period_ms, uint32_t budget_ms) {
    return (int)embk_syscall2(EMBK_SYS_sched_period, period_ms, budget_ms);
}

/* CPU THIS THREAD HAS ACTUALLY USED, in nanoseconds. Monotonic, includes the
 * slice it is running right now.
 *
 * Use this, not the clock, whenever you want the COST of something. On a busy
 * machine wall time is mostly how long you were not running: the UI toolkit
 * timing a frame in order to declare an honest scheduling budget measured
 * ~40 ms for a frame whose real work was a few milliseconds, and then declined
 * to ask for a cadence it could easily have kept. Subtract two readings around
 * the work and you have what the work cost. */
static inline uint64_t embk_thread_cpu_ns(void) {
    int64_t r = embk_syscall0(EMBK_SYS_thread_cpu_ns);
    return r > 0 ? (uint64_t)r : 0;
}

/* 1 if the child named by spawn HANDLE `handle` (what embk_spawn returned) is
 * still alive, else 0 (unknown/freed handles are "not alive"). Handle-based on
 * purpose: spawn never exposes raw pids, and a handle stays pinned to the
 * exact child instance even after pids recycle. Lets a launcher avoid starting
 * a second instance of an app that is still running. */
static inline int embk_proc_alive(int handle) {
    return (int)embk_syscall1(EMBK_SYS_proc_alive, handle);
}

/* Screen (framebuffer) size, so a windowed app can size itself to fit -- e.g.
 * clamp a tall window so it isn't rejected on a short display. */
static inline int embk_screen_size(uint32_t *w, uint32_t *h) {
    return (int)embk_syscall2(EMBK_SYS_screen_size,
                              (int64_t)(intptr_t)w, (int64_t)(intptr_t)h);
}

/* Content-local pointer for a windowed app. `focused` is 1 when the pointer is
 * over THIS process's window content (then x,y are window-local pixels and
 * buttons is the mouse state -- EMBK_MOUSE_LEFT etc.), 0 otherwise. The home
 * launcher reads this to make its tiles clickable. */
#define EMBK_WIN_ACTION_MAXIMIZE  0x80000001u
/* PLEASE CLOSE. The window's close light, or GUI+W. It is a REQUEST: the app
 * takes itself down, which is what gives it the chance to save. Ignore it and
 * the compositor kills the process a few seconds later, because a window that
 * cannot be closed is worse than an app that loses a moment of state. */
#define EMBK_WIN_ACTION_CLOSE     0x80000002u
/* Mirrors the kernel's struct win_input_kbuf field for field -- sys_win_input
 * copies it raw; grow both together.
 *
 * `when` is the millisecond the reported state was MADE. For a press the
 * compositor had to hold back (the app was mid-render when it happened) that
 * is when the finger actually went down, NOT when this poll ran -- which is
 * the difference between double-click working on a slow machine and not. Held
 * clicks are replayed one per poll, so two real clicks 130 ms apart reach an
 * app rendering at a few frames a second half a second apart. */
struct embk_win_input { int32_t focused; int32_t x, y; uint32_t buttons; uint32_t win;
                        int32_t wheel; uint32_t when; };
static inline int embk_win_input(struct embk_win_input *out) {
    return (int)embk_syscall1(EMBK_SYS_win_input, (int64_t)(intptr_t)out);
}

/* ==================================================================== */
/* IPC channels (EmbLink UI Piece 1, Layer A). A channel is a bidirectional */
/* message-oriented endpoint pair. Messages carry a byte payload + 0..N     */
/* ancillary handles, each passed COPY or MOVE. Bulk data (pixels) goes      */
/* through shared surfaces, not payloads -- channels carry control + handles.*/
/* ==================================================================== */

#define EMBK_CHAN_HANDLE_COPY 0   /* sender keeps its handle; receiver gets one too */
#define EMBK_CHAN_HANDLE_MOVE 1   /* sender's handle consumed; capability moves */

/* spawn() file-action: MOVE a channel end the parent holds into the child
 * (target_fd = parent's channel handle). Bootstraps a parent/child channel. */
#define EMBK_SPAWN_ACTION_INHERIT_CHANNEL 3

/* Create a connected pair in THIS process; out[0]/out[1] receive the two end
 * handles. Returns 0 or -EMBK_*. */
static inline int embk_chan_pair(int out_handles[2]) {
    return (int)embk_syscall1(EMBK_SYS_chan_pair, (int64_t)(intptr_t)out_handles);
}

/* Send one message: `len` payload bytes plus `n_hnd` ancillary handles
 * (`hnds[]`) with per-handle COPY/MOVE (`flags[]`, or NULL for all-COPY).
 * Blocks if the peer's inbox is full; -EMBK_EPIPE if the peer has closed.
 * Returns 0 or -EMBK_*. */
static inline int embk_chan_send(int handle, const void *bytes, unsigned len,
                                 const int *hnds, const int *flags, unsigned n_hnd) {
    return (int)embk_syscall6(EMBK_SYS_chan_send, handle, (int64_t)(intptr_t)bytes,
                              len, (int64_t)(intptr_t)hnds, (int64_t)(intptr_t)flags, n_hnd);
}

/* Receive one message. Blocks until one arrives or the peer closes
 * (-EMBK_EPIPE). Fills up to `buflen` payload bytes (-EMBK_EMSGSIZE, message
 * left queued, if it's bigger); *out_len gets the real length. Ancillary
 * handles are installed in this process's table, their ints written to
 * out_hnds[], count to *out_nhnd. Returns 0 or -EMBK_*. */
static inline int embk_chan_recv(int handle, void *buf, unsigned buflen,
                                 unsigned *out_len, int *out_hnds, unsigned *out_nhnd) {
    return (int)embk_syscall6(EMBK_SYS_chan_recv, handle, (int64_t)(intptr_t)buf,
                              buflen, (int64_t)(intptr_t)out_len,
                              (int64_t)(intptr_t)out_hnds, (int64_t)(intptr_t)out_nhnd);
}

/* Close this process's end (wakes the peer with EPIPE). */
static inline int embk_chan_close(int handle) {
    return (int)embk_syscall1(EMBK_SYS_chan_close, handle);
}

/* ==================================================================== */
/* Rendezvous (EmbLink UI Piece 1, Layer B): find a channel peer by a VFS   */
/* path (e.g. "/run/compositor") instead of needing an already-open        */
/* channel or a spawn-time handoff. Backed by a RAM filesystem mounted at   */
/* /run -- a crashed server's path vanishes automatically (kernel/ipc/     */
/* endpoint.h's B4).                                                        */
/* ==================================================================== */

/* Publish a listening endpoint at `path`. Returns an ENDPOINT handle, or
 * -EMBK_EEXIST if the path is already taken. */
static inline int embk_chan_listen(const char *path) {
    return (int)embk_syscall1(EMBK_SYS_chan_listen, (int64_t)(intptr_t)path);
}

/* Block until a client connects; returns a new CHANNEL handle for that
 * connection, or -EMBK_*. */
static inline int embk_chan_accept(int listen_handle) {
    return (int)embk_syscall1(EMBK_SYS_chan_accept, listen_handle);
}

/* Connect to the listener at `path`. Returns a new CHANNEL handle, or
 * -EMBK_ENOENT (no such path) / -EMBK_ECONNREFUSED (path exists but its
 * owner is dead / mid-teardown). */
static inline int embk_chan_connect(const char *path) {
    return (int)embk_syscall1(EMBK_SYS_chan_connect, (int64_t)(intptr_t)path);
}

/* ==================================================================== */
/* Tiny freestanding helpers (no libc). A newlib program has string.h and  */
/* ignores these; a freestanding one (user/init.c) leans on them.          */
/* ==================================================================== */

static inline size_t embk_strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}
static inline int embk_streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
/* Convenience: write a NUL-terminated string to an fd. */
static inline int64_t embk_puts(int fd, const char *s) {
    return embk_write(fd, s, embk_strlen(s));
}

/* TTY mode: 0 = cooked (line-buffered, echo), 1 = raw (unbuffered, no echo).
 * Returns the previous mode (0/1) or -EMBK_* on error. */
/* Frost the backdrop behind a window-local sub-rect. For TRANSLUCENT windows
 * (a menu bar is a thin strip inside a window tall enough for its dropdowns):
 * full-window glass would blur a slab of desktop the window never paints, so
 * the app declares the part that is actually opaque. w<=0 clears it. */
static inline int embk_win_blur_rect(int win, int x, int y, int w, int h) {
    return (int)embk_syscall5(EMBK_SYS_win_blur_rect, win, x, y, w, h);
}

/* Bring back an app you started: un-minimizes any window it parked and raises
 * its windows to the front. Takes the SPAWN HANDLE (like embk_proc_alive), not
 * a pid -- a launcher never learns pids, and going through the handle keeps
 * this scoped to apps you actually spawned. Returns 1 if anything changed. */
static inline int embk_win_restore(int handle) {
    return (int)embk_syscall1(EMBK_SYS_win_restore, handle);
}

/* How bright is what is already composed under this screen rect (0-255, or
 * -1)? For a window that paints no background of its own and must still stay
 * legible over whatever wallpaper it happens to sit on. */
static inline int embk_screen_luma(int x, int y, int w, int h) {
    return (int)embk_syscall4(EMBK_SYS_screen_luma, x, y, w, h);
}

/* Park my own window. The process keeps running and its dock dot stays lit;
 * clicking that dock icon calls embk_win_restore and brings it back. */
/* --- sound ------------------------------------------------------------------
 * Interleaved stereo, 16-bit signed, at embk_audio_rate(). One process owns
 * the device at a time; a second gets EBUSY rather than a silently shared
 * stream. Every call needs the `audio` capability -- declare it in the app's
 * .caps or these return -EPERM before touching the hardware.
 *
 * The write is SHORT-WRITE by design: it returns how many frames the ring
 * took, and a program feeds the rest next time round. Treating a short write
 * as an error is how a caller ends up dropping the middle of a sound. */
static inline uint32_t embk_audio_rate(void) {
    return (uint32_t)embk_syscall1(EMBK_SYS_audio_open, 1);   /* query, no claim */
}
static inline int embk_audio_open(void) {
    return (int)embk_syscall1(EMBK_SYS_audio_open, 0);
}
/* Returns frames accepted (may be 0 when the ring is full), or -errno. */
static inline int embk_audio_write(const int16_t *frames, uint32_t nframes) {
    return (int)embk_syscall2(EMBK_SYS_audio_write,
                              (int64_t)(intptr_t)frames, (int64_t)nframes);
}
/* 1 once the hardware has played everything queued. Ask before exiting, or
 * the tail of the sound goes with the process. */
static inline int embk_audio_drained(void) {
    return (int)embk_syscall1(EMBK_SYS_audio_close, 1);
}
static inline int embk_audio_close(void) {
    return (int)embk_syscall1(EMBK_SYS_audio_close, 0);
}

/* ---------------------------------------------------------------------------
 * WHERE THE SPEAKER ACTUALLY IS, in frames since this stream started.
 * Monotonic; 0 until the device begins.
 *
 * A streaming writer needs this and cannot substitute the clock. The device
 * does not start when you start writing -- it starts once enough is queued to
 * keep it fed -- so "frames the clock says should have played" runs ahead of
 * the truth by exactly the start-up buffer, and a writer pacing off it silently
 * holds far more audio queued than it meant to. (That mistake is what made the
 * first version of this OS's own latency test measure nothing.)
 *
 * It is also the number A/V sync is made of: picture time is sound time here.
 * Resolution is one buffer, about 21 ms -- the position of the buffer being
 * played, not of the sample inside it. */
static inline uint64_t embk_audio_position(void) {
    int64_t r = embk_syscall0(EMBK_SYS_audio_position);
    return r > 0 ? (uint64_t)r : 0;
}

/* Ask to start playing sooner: the device begins once `ms` of audio is queued
 * instead of the default ~170 ms.
 *
 * THAT NUMBER IS THE LATENCY of everything the stream does afterwards -- the
 * delay between deciding to make a sound and the sound existing. Lower it if
 * your refill loop can be relied on to come back in time (declaring a period
 * with embk_sched_period() is how you make that true), and leave it alone if
 * it cannot: running dry is a hole in the sound, and that is worse than delay.
 *
 * Call it after embk_audio_open() and before the first write. Returns the
 * latency actually granted in ms -- rounded UP to whole buffers, floored at
 * two -- or -EBUSY if the sound has already started. */
static inline int embk_audio_latency(uint32_t ms) {
    return (int)embk_syscall1(EMBK_SYS_audio_latency, ms);
}

static inline int embk_win_minimize(int win) {
    return (int)embk_syscall1(EMBK_SYS_win_minimize, win);
}

/* Lift the DESKTOP layer above every app window, or drop it back to the ground.
 *
 * The desktop is pinned at z=0 because it is the ground: everything is supposed
 * to be in front of it. That is right until the shell itself needs the whole
 * screen -- the Applications launcher is drawn by the desktop process, so with
 * an app window open it opened BEHIND that window and looked like it had not
 * opened at all. Launchpad is full-screen and in front, and this is what lets
 * the same program draw it.
 *
 * Only the desktop layer's owner may call it, and it is a MODE rather than a
 * raise: nothing else re-orders while it is set, and clearing it puts the layer
 * back on the ground rather than leaving it somewhere in the stack. */
static inline int embk_win_desktop_front(int on) {
    return (int)embk_syscall1(EMBK_SYS_win_desktop_front, on);
}

/* --- the clipboard: one text buffer PER SESSION ---------------------------
 * Set replaces it whole; get copies up to cap bytes OUT and returns how many
 * bytes the clipboard HOLDS -- more than cap means the caller saw a prefix.
 * What one session copied, another sees as an empty clipboard, and a session's
 * clipboard is wiped when it ends. */
static inline int embk_clip_set(const void *buf, size_t len) {
    return (int)embk_syscall2(EMBK_SYS_clip_set, (int64_t)(intptr_t)buf, (int64_t)len);
}
static inline int64_t embk_clip_get(void *buf, size_t cap) {
    return embk_syscall2(EMBK_SYS_clip_get, (int64_t)(intptr_t)buf, (int64_t)cap);
}

static inline int embk_tty_mode(int mode) {
    return (int)embk_syscall1(EMBK_SYS_tty_mode, mode);
}

/* --- anonymous memory mappings -------------------------------------------
 * Pages with the permissions you ask for, at an address the kernel picks, and
 * -- unlike the sbrk heap, which only ever grows -- pages you can give back.
 *
 * PRIVATE AND ANONYMOUS ONLY. There is no `fd`: a file mapping needs a page
 * cache the kernel does not have, and MAP_SHARED that quietly behaved as
 * MAP_PRIVATE would be two processes each believing they saw the other's
 * writes. There is no `addr`: an address the caller chooses is an address the
 * caller can collide with.
 *
 * PROT_WRITE|PROT_EXEC is REFUSED (-EMBK_EINVAL). A page that is both is the
 * primitive every code injection needs. A JIT maps it writable, writes, and
 * then flips it to executable -- which is mprotect, and is not built yet.
 *
 * Every page arrives ZEROED, and the whole mapping is allocated up front, so
 * a large mapping costs its full size immediately. */
#define EMBK_PROT_NONE   0x0
#define EMBK_PROT_READ   0x1
#define EMBK_PROT_WRITE  0x2
#define EMBK_PROT_EXEC   0x4

#define EMBK_MAP_SHARED   0x01
#define EMBK_MAP_PRIVATE  0x02

/* fd < 0 = anonymous. A file mapping shares the kernel's page cache object for
 * that file, so a reader and a mapper see one set of pages -- and the mapping
 * holds its own reference, so closing the fd afterwards is fine. */
static inline int64_t embk_mmap_fd(size_t len, int prot, int flags,
                                   int fd, uint64_t off) {
    return embk_syscall5(EMBK_SYS_mmap, (int64_t)len, prot, flags, fd, (int64_t)off);
}
static inline int64_t embk_mmap(size_t len, int prot) {
    return embk_mmap_fd(len, prot, 0, -1, 0);
}

/* Unmaps a whole mapping, or a page-aligned prefix, suffix or middle of one --
 * a middle splits it in two. Unmapping a range that was never mapped is an
 * ERROR, not a no-op: it is far more often a bug than idempotent cleanup, and
 * succeeding silently would hide a double free of address space. */
static inline int embk_munmap(void *addr, size_t len) {
    return (int)embk_syscall2(EMBK_SYS_munmap, (int64_t)(intptr_t)addr, (int64_t)len);
}

/* Change the permissions of memory you already have, keeping its contents.
 *
 * This is the other half of the W^X rule. mmap will not give you a page that
 * is writable AND executable; with mprotect you do not need one:
 *
 *     void *code = (void *)embk_mmap(len, EMBK_PROT_READ | EMBK_PROT_WRITE);
 *     ... emit instructions into it ...
 *     embk_mprotect(code, len, EMBK_PROT_READ | EMBK_PROT_EXEC);
 *
 * The dangerous state never exists, and generated code still runs. Asking for
 * WRITE|EXEC in one call is refused here too.
 *
 * EMBK_PROT_NONE is real, not a rounding: the pages stay yours and every
 * access to them faults, which is how you place a guard page around something.
 *
 * The range must be page-aligned and ENTIRELY mapped -- a range with a hole in
 * it is refused whole (-EMBK_ENOMEM) rather than half-applied, because a
 * caller that believes it protected N pages and got M has a guarantee it does
 * not have. */
static inline int embk_mprotect(void *addr, size_t len, int prot) {
    return (int)embk_syscall3(EMBK_SYS_mprotect, (int64_t)(intptr_t)addr,
                              (int64_t)len, prot);
}

/* --- a second name for an open file ---------------------------------------
 * `newfd` < 0 asks for the lowest free descriptor at or above `min_fd`;
 * otherwise `newfd` is the exact slot and whatever it held is closed first.
 *
 * The two descriptors share ONE open file description -- one cursor, one
 * reference to the underlying object. A write through either advances both,
 * and closing either leaves the other fully working. That shared cursor is the
 * entire promise of dup: re-opening the file instead would give a separate
 * one, which is the opposite of what the caller asked for.
 *
 * embk_dup(fd, fd) returns fd unchanged rather than closing and reopening it,
 * because closing the target first would destroy the description being
 * duplicated. */
static inline int embk_dup(int oldfd, int newfd, int min_fd) {
    return (int)embk_syscall3(EMBK_SYS_dup, oldfd, newfd, min_fd);
}

/* Do not return until this file's outstanding writes are ON THE DEVICE.
 *
 * write() returns as soon as the bytes reach the page cache, which is what
 * makes writing fast and what makes a crash able to lose them. This is the
 * control: a journal, a lock file, a git ref -- anything whose ordering
 * against a later write must survive a power cut -- calls this and waits.
 * Everything else does not, and gets the speed. */
static inline int embk_fsync(int fd) {
    return (int)embk_syscall1(EMBK_SYS_fsync, fd);
}

/* --- being interrupted, and surviving it ----------------------------------
 *
 * ^C routed at a process CANCELS it, and cancellation is sticky on purpose --
 * a process must not be able to miss one by being between calls. That makes it
 * exactly wrong for the other thing ^C means: "stop what you are doing and go
 * back to your prompt". A shell that routed ^C at itself would take one
 * keystroke and never read a line again.
 *
 * So a process can opt IN to catching interrupts instead. Then ^C increments a
 * COUNTER it takes and clears whenever it is ready to act -- as many times as
 * a human presses the key. Counted rather than a flag so a second press during
 * a slow unwind is not lost, and so an impatient double-tap is visible as one.
 *
 *     embk_intr_catch(1);                    // ^C now interrupts, not kills
 *     embk_console_interrupt_route(0);       // ...and it is aimed at me
 *     while (working) {
 *         if (embk_intr_take()) break;       // a human asked us to stop
 *     }
 *     embk_console_interrupt_route(-1);      // hand ^C back
 */
static inline int embk_intr_catch(int on) {
    return (int)embk_syscall2(EMBK_SYS_intr, 1, on);
}
static inline unsigned embk_intr_take(void) {
    int64_t n = embk_syscall2(EMBK_SYS_intr, 0, 0);
    return n > 0 ? (unsigned)n : 0u;
}

/* --- symbolic links --------------------------------------------------------
 * A link holds TEXT, not a reference to an object. It may name something that
 * does not exist (creating one before its target is normal), it is re-resolved
 * on every path walk, and a cycle is reported as -EMBK_ELOOP rather than
 * hung on. `target` is stored verbatim, so a relative target stays relative --
 * which is what lets a tree of links survive being moved.
 *
 * embk_readlink reports the FULL length even when the buffer was too small, so
 * a caller can tell a truncated answer from a complete one. */
/* A HARD link: `newpath` becomes a second name for the object `oldpath`
 * names. Unlike embk_symlink this is a reference to the thing, not a string
 * that names it -- both names are equal, neither is the original, and the
 * data lives until the last name is unlinked. Directories are refused. */
static inline int embk_link(const char *oldpath, const char *newpath) {
    return (int)embk_syscall2(EMBK_SYS_link, (int64_t)(intptr_t)oldpath,
                              (int64_t)(intptr_t)newpath);
}

static inline int embk_symlink(const char *target, const char *linkpath) {
    return (int)embk_syscall2(EMBK_SYS_symlink, (int64_t)(intptr_t)target,
                              (int64_t)(intptr_t)linkpath);
}
static inline int64_t embk_readlink(const char *path, char *buf, size_t cap) {
    return embk_syscall3(EMBK_SYS_readlink, (int64_t)(intptr_t)path,
                         (int64_t)(intptr_t)buf, (int64_t)cap);
}

/* ===========================================================================
 * LOCKS -- the first correct way for two threads in one process to agree
 * about anything.
 *
 * embk_thread_create has existed for a long time; a way to SHARE data between
 * the threads it makes has not. A program had a choice between spinning on an
 * atomic -- burning a whole timeslice per contention, on a machine where the
 * holder may be on another core or may not be scheduled at all -- and being
 * wrong. There was no third option.
 *
 * THE FAST PATH NEVER ENTERS THE KERNEL. An uncontended lock is one atomic
 * compare-and-swap in ring 3 and nothing else; the syscall below is only what
 * happens when a thread must actually wait. That is the whole point of the
 * futex shape, and it is why the kernel call is the odd-looking "sleep if this
 * word still says what I think it says" rather than "lock this".
 *
 * THREE STATES, not two, and the third is what stops a lost wakeup:
 *
 *     0  free
 *     1  held, and NOBODY is waiting        -- unlock needs no syscall
 *     2  held, and somebody MAY be waiting  -- unlock must wake
 *
 * With only free/held, a thread that released the lock could not know whether
 * anyone was asleep on it, so it would have to make the wake syscall every
 * time -- paying the kernel on the uncontended path, which is the one case
 * that must stay free. */
#define EMBK_FUTEX_WAIT 0
#define EMBK_FUTEX_WAKE 1

static inline int64_t embk_futex(volatile uint32_t *addr, int op, uint32_t val) {
    return embk_syscall3(EMBK_SYS_futex, (int64_t)(intptr_t)addr, op, val);
}

typedef struct { volatile uint32_t state; } embk_mutex;

#define EMBK_MUTEX_INIT { 0 }

static inline void embk_mutex_init(embk_mutex *m) { m->state = 0; }

static inline void embk_mutex_lock(embk_mutex *m) {
    uint32_t expected = 0;
    /* THE FAST PATH: one CAS, free -> held-uncontended. No syscall. */
    if (__atomic_compare_exchange_n(&m->state, &expected, 1, false,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return;

    /* Contended. Announce it by moving the lock to state 2 and then sleeping
     * on that value -- so the holder's unlock knows a wake is owed.
     *
     * The exchange is unconditional: whatever the lock was (1 or 2) it is 2
     * now, which is the safe direction. Claiming "somebody may be waiting"
     * when nobody is costs one wasted wake syscall; the opposite mistake
     * loses a wakeup and hangs. */
    while (__atomic_exchange_n(&m->state, 2, __ATOMIC_ACQUIRE) != 0) {
        /* Sleep only while the lock still reads 2. If the holder released
         * between the exchange and here, the kernel's compare fails, WAIT
         * returns EAGAIN, and we loop round to try the exchange again --
         * which is exactly the lost-wakeup window the in-kernel compare
         * exists to close. */
        (void)embk_futex(&m->state, EMBK_FUTEX_WAIT, 2);
    }
}

static inline int embk_mutex_trylock(embk_mutex *m) {
    uint32_t expected = 0;
    return __atomic_compare_exchange_n(&m->state, &expected, 1, false,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED) ? 0 : -1;
}

static inline void embk_mutex_unlock(embk_mutex *m) {
    /* If the lock was 1, nobody ever announced a wait and there is nothing to
     * wake -- the uncontended unlock is one atomic store and no syscall. */
    if (__atomic_exchange_n(&m->state, 0, __ATOMIC_RELEASE) == 2)
        (void)embk_futex(&m->state, EMBK_FUTEX_WAKE, 1);
}

/* ---------------------------------------------------------------------------
 * CONDITION VARIABLES -- waiting for a STATE, not for a lock.
 *
 * A mutex answers "may I touch this yet". It cannot answer "has it become
 * what I need", and that is what almost every piece of concurrent code
 * actually wants: a consumer waiting for an item, a worker waiting for a job,
 * a thread waiting for shutdown. Without one, the only way to express it is to
 * take the lock, look, drop the lock, and try again -- a poll loop that burns
 * a core to discover nothing changed.
 *
 * A SEQUENCE NUMBER IS THE WHOLE IMPLEMENTATION. The counter is not the state
 * anyone cares about; it exists so a waiter can say "sleep unless something
 * has been signalled since I looked". Read it BEFORE dropping the mutex, and
 * the dangerous window closes: if a signal lands between the unlock and the
 * futex call, the counter has already moved, the kernel's compare fails, and
 * the wait returns immediately instead of sleeping through the event it was
 * waiting for. That lost wakeup is the entire difficulty of condition
 * variables, and this is where it is prevented.
 *
 * SPURIOUS WAKEUPS ARE PART OF THE CONTRACT, not a defect to be papered over.
 * A wake means "the state MAY have changed"; it never means it did. Every
 * caller must re-check its own condition in a loop:
 *
 *     embk_mutex_lock(&m);
 *     while (!ready)                     // while, never if
 *         embk_cond_wait(&c, &m);
 *     ... consume ...
 *     embk_mutex_unlock(&m);
 *
 * Writing `if` there is the classic bug, and it is silent: it works until the
 * day two waiters are woken by one signal and the second finds nothing.
 */
typedef struct { volatile uint32_t seq; } embk_cond;

#define EMBK_COND_INIT { 0 }

static inline void embk_cond_init(embk_cond *c) { c->seq = 0; }

/* Atomically release `m` and sleep until signalled; re-acquires `m` before
 * returning. The mutex MUST be held on entry -- that is what makes the
 * caller's check of its own condition and this wait a single indivisible
 * step from any other thread's point of view. */
static inline void embk_cond_wait(embk_cond *c, embk_mutex *m) {
    /* Read the sequence WHILE STILL HOLDING THE MUTEX. Everything depends on
     * this ordering: a signal cannot slip past between here and the sleep,
     * because a signal moves the counter and the kernel refuses to sleep on a
     * value that has changed. */
    uint32_t seq = __atomic_load_n(&c->seq, __ATOMIC_RELAXED);

    embk_mutex_unlock(m);
    (void)embk_futex(&c->seq, EMBK_FUTEX_WAIT, seq);
    embk_mutex_lock(m);
}

/* Wake ONE waiter. Correct only when any single waiter can make progress --
 * which is not true of, say, a bounded queue with producers and consumers on
 * one condition, where the one woken may be the wrong kind. Broadcast when in
 * doubt: waking too many costs time, waking too few hangs. */
static inline void embk_cond_signal(embk_cond *c) {
    __atomic_fetch_add(&c->seq, 1, __ATOMIC_RELEASE);
    (void)embk_futex(&c->seq, EMBK_FUTEX_WAKE, 1);
}

static inline void embk_cond_broadcast(embk_cond *c) {
    __atomic_fetch_add(&c->seq, 1, __ATOMIC_RELEASE);
    (void)embk_futex(&c->seq, EMBK_FUTEX_WAKE, 0x7FFFFFFF);
}

#endif /* __EMBK_H__ */
