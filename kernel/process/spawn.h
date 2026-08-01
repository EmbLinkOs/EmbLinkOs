// shared between process.c and spawn.c, so they can both see the same struct
#include <stdint.h>


/* Raised 16 -> 32 (and bytes 1024 -> 2048) for EmbBuild v2: a real link line
 * -- compiler, flags, crt0, syscalls, ELEVEN objects, -lc, -o, output -- is
 * 19 argv entries and ~720 string bytes, entirely legitimate. 16 was sized
 * for hand-typed commands; the first machine-generated argv outgrew it
 * (spawn returned E2BIG on the shell's own rebuild). Kernel-stack cost of
 * the bump: +1 KB in sys_spawn's argv_buf + 128 B of pointer arrays --
 * well inside the guarded kernel stacks. */
#define SPAWN_ARGV_MAX             128         // max argv[] entries (incl. the NULL terminator).
                                               // 32 -> 64 for on-OS self-host of the FP math:
                                               // linking the whole libc + fdlibm tree + app is
                                               // ~38 object paths in one embld argv. The real
                                               // limit is the child's 4 KiB argv/envp page, still
                                               // checked at runtime in process_create_env
                                               // (STACK_ROOM_NEEDED); this only sizes the arrays.
#define SPAWN_ARGV_BYTES_MAX       8192        // total bytes of argv[] strings (incl. NUL terminators)

/* ENVIRONMENT -- passed EXPLICITLY at spawn, exactly like argv, and never
 * inherited.
 *
 * This is the deliberate EmbLink shape, not an oversight vs Unix. A Unix
 * environment is ambient state a child receives whether or not anyone chose to
 * give it: fork+exec copies it by default. EmbLink's whole position is that a
 * child gets ONLY what the parent names -- that is why there is no exec, why
 * file-actions list every fd, and why FD_CLOEXEC is vacuously true here. An
 * env that leaked in by default would contradict all of it. So: envp==NULL
 * means the child has NO environment, and getenv() honestly returns NULL.
 *
 * envp is NULL-TERMINATED rather than counted (unlike argc): it is what
 * `char **environ` must be anyway, and sys_spawn has no free argument register
 * left for a count.
 *
 * Budget: argv and envp share the child's TOP 4 KiB stack page with both
 * pointer arrays. 1024 + 16*8 + 1024 + 32*8 = 2432 of 4096 -- process_create_env()
 * enforces it rather than trusting this arithmetic. */
#define SPAWN_ENVP_MAX             32          // max envp[] entries INCLUDING the NULL terminator
#define SPAWN_ENVP_BYTES_MAX       1024        // total bytes of envp[] strings (including NUL terminators)
#define SPAWN_ACTIONS_MAX           8          // max number of spawn actions (file descriptor remaps, etc.)
#define SPAWN_ACTION_PATH_MAX      256         // Deliberately seperate from syscall.c's
                                               // SYSCALL_PATH_MAX -- process.h shouldn't
                                               // depend on a #define private to syscall.c

#define SPAWN_ACTION_OPEN            1         // open path onto target_fd in the child
#define SPAWN_ACTION_INHERIT_SURFACE 2         // dup+map a surface into the child (minimal
                                               // handle_transfer -- EmbLink UI Piece 1). For
                                               // this kind, `target_fd` holds the PARENT's
                                               // surface obj-handle; the child receives it at
                                               // its first free obj_handle slot (0 for a fresh
                                               // child), already mapped into its address space.
#define SPAWN_ACTION_INHERIT_CHANNEL 3         // MOVE a channel end into the child (Layer A):
                                               // `target_fd` holds the PARENT's channel-end
                                               // obj-handle; the parent gives it up and the
                                               // child receives it at its first free slot. The
                                               // bootstrap for two-process channel tests.
#define SPAWN_ACTION_INSTALL_OBJ     4         // install a byte-stream obj-handle (a pipe end)
#define SPAWN_ACTION_DEBUG           6         // born under debug, stopped before _start
                                               // (EMBDBG_Specification.md §6.2). Needs the
                                               // spawner to hold EMBK_CAP_DEBUG; the child is
                                               // created and parked, and the child handle
                                               // sys_spawn returns doubles as the debug handle.
                                               // (id 6, not 5 -- SET_CAPS took 5 below.)
#define SPAWN_ACTION_NS_BIND         7         // narrow the child's NAMESPACE (USERSPACE_v2 UP2b):
                                               // `path` is the prefix to grant, `flags` its mode
                                               // (0 = RW, 1 = RO). The prefix is resolved in the
                                               // PARENT's namespace, so a parent can only hand down
                                               // what it can itself name, at a mode no wider than it
                                               // holds -- attenuation by construction. The child's
                                               // namespace becomes EXACTLY the granted prefixes
                                               // (absent => inherit the parent's whole view). Reuses
                                               // `path`+`flags`; consumed at creation, skipped by the
                                               // file-action processor.
#define SPAWN_ACTION_SET_CAPS        5         // attenuate the child's capability set: `flags`
                                               // carries the requested cap bitmask (capabilities.h
                                               // cap IDs). Enforced <= the spawning process's own
                                               // set; absent => child INHERITs the parent's full
                                               // set. Not a file action -- consumed by sys_spawn to
                                               // pick the process_create_caps() request; the action
                                               // processor skips it. Reuses `flags` so the ABI
                                               // struct does not grow.
                                               // the PARENT holds as a plain FD in the child:
                                               // `src_obj_handle` names the parent's handle,
                                               // `target_fd` the child fd it lands on (0/1 for
                                               // a shell pipeline). COPY semantics: the child's
                                               // fd is a NEW reference; the parent keeps its
                                               // handle and must release it separately for EOF.
                                               // (NOT 2/3 -- those are taken above.)

struct spawn_file_action {
    uint8_t kind;                       // SPAWN_ACTION_OPEN, etc.
    int target_fd;                      // SPAWN_ACTION_OPEN: fd to remap to (e.g. 0 for stdin).
                                        // SPAWN_ACTION_INHERIT_SURFACE: parent's surface handle.
                                        // SPAWN_ACTION_INSTALL_OBJ: fd in the CHILD.
    char path[SPAWN_ACTION_PATH_MAX];   // NUL-terminated path for SPAWN_ACTION_OPEN
    int flags;                          // O_RDONLY, O_WRONLY, O_RDWR, O_CREAT, etc. for SPAWN_ACTION_OPEN
    int mode;                           // 0o644, 0o600, etc. for SPAWN_ACTION_OPEN
    int src_obj_handle;                 // SPAWN_ACTION_INSTALL_OBJ: handle in the PARENT's
                                        // obj_handle table. ABI NOTE: sys_spawn copies this
                                        // struct RAW from user memory, so user/lib/embk.h's
                                        // embk_spawn_file_action mirrors it field-for-field --
                                        // grow BOTH together and rebuild every action-passing app.
};

