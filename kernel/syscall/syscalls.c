/* The system call handlers -- 88 of them, and not one is architecture-specific.
 * (Seven more live in kernel/process/debug.c, next to the debugger state they
 * manipulate; they take the same struct sysargs.)
 *
 * This file used to be kernel/arch/x86_64/syscall/syscall.c, 2,019 lines under
 * arch/ that had nothing to do with x86 except how they read their arguments:
 * 184 direct reads of r->rdi, r->rsi, r->rdx, r->r10, r->r8 and r->r9. The
 * policy was always portable -- sys_open parses a path and asks the VFS,
 * sys_win_move moves a window -- so the audit in docs/ARM64.md §1 counted the
 * whole file as arch-specific code and it never was.
 *
 * What remains under arch/x86_64/syscall/ is the ~60 lines that genuinely are
 * a machine: struct regs, the IDT gate, and the entry point that fills in a
 * struct sysargs. See kernel/include/syscall_abi.h for the contract.
 *
 * ARGUMENTS ARE arg[0..5]. If you find yourself wanting the trap frame, read
 * the note in syscall_abi.h before adding one.
 */

#include "include/syscall_abi.h"
#include "include/usercopy.h"
#include "mm/vma.h"        /* sys_mmap / sys_munmap */
#include "drivers/char/serial.h"
#include "include/kprintf.h"   /* sys_read's copy_to_user fault-path diagnostic */
#include "ipc/pipe.h"          /* sys_pipe: pipe_create */
#include "drivers/timer/rtc.h"
#include "drivers/timer/hpet.h"
#include "drivers/timer/timer.h"
#include "include/errno.h"
#include "include/types.h"
#include "include/kstring.h"
#include "process/process.h"
#include "process/debug.h"   /* sys_debug_* handlers for the table below */
#include "tty/tty.h"
#include "gfx/surface.h"
#include "gfx/compositor.h"
#include "ipc/clipboard.h"
#include "drivers/video/framebuffer.h"
#include "drivers/input/mouse.h"
#include "drivers/input/keyboard.h"
#include "drivers/audio/audio.h"  /* sys_audio_* handlers below */
#include "include/kmalloc.h"
#include "ipc/channel.h"
#include "ipc/endpoint.h"
#include "fs/fd.h"
#include "fs/vfs.h"
#include "net/net.h"   /* sys_net_resolve -> net_resolve; socket fds via fd.h */

// Heap bounce buffer bounds for sys_read/sys_write. _MAX is the real throughput
// lever: chunk size IS throughput, because storage costs ~2.7 ms PER DEVICE
// REQUEST (ATA setup + a serial busy-wait for the DMA IRQ) rather than per byte.
// 64 KB keeps one allocation per syscall small while cutting fd->vfs->fs trips
// (and the B-tree descents under them) ~32x versus the old 2 KB stack array.
// _MIN is the on-stack fallback used only when kmalloc fails -- correctness must
// not depend on the allocator, so a read under memory pressure still completes,
// just slowly. Keep _MIN small: it is a KERNEL STACK array (KSTACK_SIZE 16 KiB).
#define SYSCALL_IO_CHUNK_MAX (64u * 1024u)
#define SYSCALL_IO_CHUNK_MIN 2048
// Max path length copy_string_from_user() will accept for open()/stat().
#define SYSCALL_PATH_MAX 256


/* --- Individual syscall handlers --- */

/* write(fd, buf, len) -> bytes written. fd==1 (stdout) goes to the serial
 * console directly, since there's no real "console" vnode to open yet; any
 * other fd routes through the fd/VFS layer (vfs_fd_write), which already
 * fully supports EMBKFS/FAT32.
 *
 * `buf` is a USER pointer: copied through copy_from_user() in bounded
 * chunks rather than dereferenced directly, so a ring-3 caller can't hand
 * the kernel a kernel-space (or unmapped) address and get it read back —
 * see docs/TODO.md's "Security — user-pointer validation" entry, now closed. */
static int64_t sys_write(const struct sysargs *a) {
    int fd = (int)a->arg[0];
    const char *buf = (const char *)a->arg[1];
    size_t len = (size_t)a->arg[2];

    /* Heap bounce buffer, one allocation per call -- same reasoning as sys_read:
     * the chunk size is the throughput lever, and the kernel stack could never
     * afford a big one. Falls back to a small stack chunk if kmalloc fails. */
    char fallback[SYSCALL_IO_CHUNK_MIN];
    size_t cap = len < SYSCALL_IO_CHUNK_MAX ? len : SYSCALL_IO_CHUNK_MAX;
    char *chunk = cap ? (char *)kmalloc(cap) : NULL;
    if (!chunk) {
        chunk = fallback;
        cap = sizeof fallback;
    }

    int64_t ret;
    size_t done = 0;
    while (done < len) {
        size_t n = len - done;
        if (n > cap) {
            n = cap;
        }
        if (copy_from_user(chunk, buf + done, n) != EMBK_OK) {
            ret = done > 0 ? (int64_t)done : -EMBK_EFAULT;
            goto out;
        }

        size_t advanced;
        size_t written = 0;
        int rc = vfs_fd_write(fd, chunk, n, &written);
        if (rc != EMBK_OK) {
            ret = done > 0 ? (int64_t)done : rc;
            goto out;
        }
        advanced = written;

        done += advanced;
        if (advanced < n) {
            break;   // short write (fd path only); stop rather than looping forever
        }
    }
    ret = (int64_t)done;
out:
    /* Single exit: no path may leak the buffer. Only the heap one is freed. */
    if (chunk != fallback) {
        kfree(chunk);
    }
    return ret;
}

/* read(fd, buf, len) -> bytes read, via the fd/VFS layer. No fd 0 (stdin)
 * wiring yet — there's no "console input" vnode, only the raw keyboard
 * buffer main.c's loop drains directly. `buf` is a USER pointer: filled
 * through a kernel bounce buffer + copy_to_user(), the mirror image of
 * sys_write's copy_from_user() pattern. */
static int64_t sys_read(const struct sysargs *a) {
    int fd = (int)a->arg[0];
    char *buf = (char *)a->arg[1];
    size_t len = (size_t)a->arg[2];

    if ( fd == 1 || fd == 2) {
        return -EMBK_EINVAL;   // no stdio-fd read support yet
    }

    /* Bounce buffer on the HEAP, one allocation for the whole call.
     *
     * It used to be `char chunk[SYSCALL_IO_CHUNK]` on the kernel stack, which
     * capped the chunk at 2 KB (KSTACK_SIZE is only 16 KiB) -- and the chunk
     * size IS the throughput: each chunk is a full fd->vfs->fs trip, and the
     * storage cost is ~2.7 ms PER DEVICE REQUEST (ATA setup + a serial busy-wait
     * for the DMA IRQ), not per byte. Small chunks meant ~250 KB/s no matter that
     * the transfer itself is DMA. Off the stack, the chunk can be 64 KB: ~32x
     * fewer trips, and ~32x fewer B-tree descents in the fs beneath.
     *
     * kmalloc failure is NOT fatal: fall back to a small stack chunk so reads
     * still work under memory pressure, just slowly. `cap` is sized to the
     * request, so a 100-byte read doesn't allocate 64 KB. */
    char fallback[SYSCALL_IO_CHUNK_MIN];
    size_t cap = len < SYSCALL_IO_CHUNK_MAX ? len : SYSCALL_IO_CHUNK_MAX;
    char *chunk = cap ? (char *)kmalloc(cap) : NULL;
    if (!chunk) {
        chunk = fallback;
        cap = sizeof fallback;
    }

    int64_t ret;
    size_t done = 0;
    while (done < len) {
        size_t n = len - done;
        if (n > cap) {
            n = cap;
        }
        /* Validate the destination BEFORE anything is consumed. The rewind
         * below is only possible on a SEEKABLE fd -- a pipe returns ESPIPE,
         * so bytes taken out of its ring can never be put back, and a
         * copy_to_user failure there destroys them permanently (observed:
         * "corrupt structured output" -- a half-delivered wire frame).
         * Checking first turns that data-loss window into a clean EFAULT
         * that consumes nothing. */
        if (!access_ok(buf + done, n)) {
            ret = done > 0 ? (int64_t)done : -EMBK_EFAULT;
            goto out;
        }

        size_t got = 0;
        int rc = vfs_fd_read(fd, chunk, n, &got);
        if (rc != EMBK_OK) {
            ret = done > 0 ? (int64_t)done : rc;
            goto out;
        }
        if (got > 0 && copy_to_user(buf + done, chunk, got) != EMBK_OK) {
            /* The fs already consumed `got` bytes (vfs_fd_read advanced the
             * position) but the caller never received them. Returning here
             * WITHOUT rewinding silently dropped this chunk -- the next read
             * continued past it (observed: font.ttf arriving k*256 bytes
             * short with varying content under -smp 4, -> textless UI).
             * Rewind so a retrying caller sees every byte exactly once --
             * and if the fd CAN'T rewind (a pipe: ESPIPE), say so instead of
             * ignoring the failure and silently eating the bytes. */
            uint64_t np = 0;
            int srr = vfs_fd_seek(fd, -(int64_t)got, 1 /* SEEK_CUR */, &np);
            kprintf("sys_read: copy_to_user FAULT pid=%u dst=0x%llx off_done=%llu (%s %llu)\n",
                    current_thread ? (unsigned)current_process->pid : 0u,
                    (unsigned long long)(uintptr_t)(buf + done),
                    (unsigned long long)done,
                    srr == EMBK_OK ? "rewound" : "UNSEEKABLE -- LOST",
                    (unsigned long long)got);
            ret = done > 0 ? (int64_t)done : -EMBK_EFAULT;
            goto out;
        }
        done += got;
        if (got < n) {
            break;   // short read: EOF (or similar) -- stop here
        }
    }
    ret = (int64_t)done;
out:
    /* Single exit so no path can leak the buffer. `fallback` is on the stack --
     * only free the heap one. */
    if (chunk != fallback) {
        kfree(chunk);
    }
    return ret;
}

/* open(path, flags, mode) -> fd, or -errno. `path` is copied in through a
 * bounded kernel buffer (copy_string_from_user) rather than resolved
 * directly from the user pointer. */
/* The capability gate (ring 3+): a syscall that INSTALLS a resource handle
 * refuses it to a process lacking the class. Returns 0 to proceed or -EMBK_EPERM
 * to deny. Gate the INSTALL points only (open, surface/window create) -- ops on
 * a handle already held are handle-scoped and need no re-check. A syscall always
 * runs in a ring-3 process, so current_process is non-NULL. */
static inline int cap_gate(uint32_t cap_id) {
    return (current_process->cap_set & EMBK_CAP_BIT(cap_id)) ? 0 : -EMBK_EPERM;
}

static int64_t sys_open(const struct sysargs *a) {
    /* open() installs an fd handle -- the filesystem access point. Gate it here
     * (not read/write, which are scoped to an fd already granted). stdio fds
     * 0/1/2 are installed by fds_init_stdio at spawn, not through open, so a
     * process without FILESYSTEM still has its console -- it just cannot open
     * anything new. */
    int cg = cap_gate(EMBK_CAP_FILESYSTEM);
    if (cg) return cg;
    const char *user_path = (const char *)a->arg[0];
    int flags = (int)a->arg[1];
    uint32_t mode = (uint32_t)a->arg[2];

    char path[SYSCALL_PATH_MAX];
    int len = copy_string_from_user(path, user_path, sizeof(path));
    if (len < 0) {
        return len;   // -EMBK_EFAULT or -EMBK_ENAMETOOLONG
    }

    return vfs_open(path, flags, mode);
}

/* close(fd) -> 0, or -errno. */
static int64_t sys_close(const struct sysargs *a) {
    int fd = (int)a->arg[0];
    return vfs_close(fd);
}

/* pipe(out[2]) -> 0, or -errno. Writes {read_handle, write_handle} -- typed
 * OBJ-HANDLES (the capability table sys_spawn's INSTALL action consumes),
 * NOT fds. The caller turns one into an fd by installing it, or hands it to
 * a child at spawn. On a copy_to_user fault both handles are released via
 * the kind dispatch (which drops the pipe refs), so a bad pointer can't leak
 * a pipe. */
static int64_t sys_pipe(const struct sysargs *a) {
    uint64_t user_out = a->arg[0];    /* int[2]: {read_handle, write_handle} */
    int handles[2];
    int rc = pipe_create(&handles[0], &handles[1]);
    if (rc != EMBK_OK) {
        return rc;
    }
    if (copy_to_user((void *)user_out, handles, sizeof handles) != EMBK_OK) {
        obj_handle_free(current_process, handles[0]);   /* dispatch drops the refs */
        obj_handle_free(current_process, handles[1]);
        return -EMBK_EFAULT;
    }
    return 0;
}

/* handle_close(h) -> 0, or -errno. Release ONE obj-handle through the
 * generic kind dispatch (obj_handle_free): a pipe end is unref'd (the
 * reader sees EOF when the last writer drops), a channel end closed +
 * peer woken, a surface unmapped + unref'd. This is the pipeline dance's
 * missing fourth step: after installing pipe ends into children via
 * INSTALL_OBJ (COPY semantics), the parent MUST drop its own handles or
 * its retained write end keeps n_writers > 0 forever -- the classic
 * "close your copy of the write end or the reader never sees EOF". */
static int64_t sys_handle_close(const struct sysargs *a) {
    int h = (int)a->arg[0];
    if (h < 0 || h >= OBJ_HANDLE_MAX) {
        return -EMBK_EBADF;
    }
    if (!current_process->obj_handles[h].used) {
        return -EMBK_EBADF;
    }
    obj_handle_free(current_process, h);
    return 0;
}

/* fd_install_obj(handle, target_fd) -> target_fd, or -errno. Install an
 * obj-handle the CALLER holds into its OWN fd table -- the self-install
 * half of the INSTALL_OBJ spawn action. The shell needs this for its own
 * end of a pipeline: it holds the READ handle of the last stage's output
 * and must turn it into an fd it can read(). COPY semantics, same as the
 * spawn action: the handle stays alive; close it separately. Only
 * byte-stream kinds are installable (a surface has no read/write meaning). */
static int64_t sys_fd_install_obj(const struct sysargs *a) {
    int h         = (int)a->arg[0];
    int target_fd = (int)a->arg[1];
    if (h < 0 || h >= OBJ_HANDLE_MAX) {
        return -EMBK_EBADF;
    }
    struct obj_handle *oh = &current_process->obj_handles[h];
    if (!oh->used) {
        return -EMBK_EBADF;
    }
    if (oh->kind != HANDLE_KIND_PIPE) {
        return -EMBK_EINVAL;
    }
    struct pipe_end *pe = (struct pipe_end *)oh->obj;
    int rc = fd_install_pipe(current_process, target_fd, pe->p, pe->side);
    if (rc != EMBK_OK) {
        return rc;
    }
    return target_fd;
}

/* fd_avail(fd) -> bytes readable WITHOUT blocking (0 = nothing yet, NOT
 * EOF), or -errno. A pipe answers its buffered count; the console answers
 * whether a key is waiting. What lets the terminal poll the shell's output
 * pipe from its render loop instead of parking a thread in read(). */
static int64_t sys_fd_avail(const struct sysargs *a) {
    int fd = (int)a->arg[0];
    return vfs_fd_avail(fd);
}

/* fcntl(fd, cmd, arg) -- the sliver of POSIX fcntl the kernel actually owns:
 * the O_NONBLOCK fd flag. cmd 1 = get (returns 1 if non-blocking, else 0);
 * cmd 2 = set non-blocking to `arg`. F_DUPFD is served by SYS_dup, which the
 * libc's fcntl() routes to; everything else about fcntl (F_GETFD, ...) stays a
 * libc concern. */
static int64_t sys_fcntl(const struct sysargs *a) {
    int fd  = (int)a->arg[0];
    int cmd = (int)a->arg[1];
    int arg = (int)a->arg[2];
    switch (cmd) {
    case 1: {
        int fl = vfs_fd_get_flags(fd);
        if (fl < 0) return fl;
        return (fl & O_NONBLOCK) ? 1 : 0;
    }
    case 2:
        return vfs_fd_set_nonblock(fd, arg != 0);
    default:
        return -EMBK_EINVAL;
    }
}

/* dup(oldfd, newfd, min_fd) -> the new fd, or -errno.
 *
 * One syscall for three POSIX calls, because they are three spellings of one
 * operation: dup(fd) is (fd, -1, 0), dup2(fd, n) is (fd, n, 0), and
 * fcntl(fd, F_DUPFD, n) is (fd, -1, n). Splitting them into three entry points
 * would be three copies of the same fd-table walk.
 *
 * What comes back is a SECOND NAME for the same open file description, not a
 * copy of it: the cursor is shared, and the underlying object is released once
 * when the last descriptor closes. That is what makes shell redirection work
 * -- `cmd > file` is dup2 onto fd 1 -- and it is why this needed a real
 * refcounted object underneath rather than a struct assignment. */
static int64_t sys_dup(const struct sysargs *a) {
    return vfs_fd_dup((int)a->arg[0], (int)a->arg[1], (int)a->arg[2]);
}

/* fsync(fd) -> 0 or -errno.
 *
 * Before the page cache this could not exist as anything but a lie or a no-op:
 * every write() went straight to the device, so there was nothing to flush and
 * "your writes are committed" was already true. Now a write() returns as soon
 * as the bytes are in the cache, and this is how a caller says the difference
 * matters -- a database's journal, a shell's history, git writing a ref. It
 * does not return until the file's dirty pages are on the device. */
static int64_t sys_fsync(const struct sysargs *a) {
    return vfs_fd_fsync((int)a->arg[0]);
}

/* fd_poll(fd, events) -> ready POLL* bits (or POLLNVAL for a bad fd). One fd at
 * a time; the libc select() loops over the fd_set calling this. What lets a
 * non-blocking socket report connect-completion (POLLOUT) and readability. */
static int64_t sys_fd_poll(const struct sysargs *a) {
    return vfs_fd_poll((int)a->arg[0], (int)a->arg[1]);
}

/* unlink(path) -> 0 or -errno. The shell's `rm`. */
static int64_t sys_unlink(const struct sysargs *a) {
    char path[SYSCALL_PATH_MAX];
    int len = copy_string_from_user(path, (const char *)a->arg[0], sizeof(path));
    if (len < 0) {
        return len;
    }
    return vfs_unlink_path(path);
}

/* mkdir(path) -> 0 or -errno. The shell's `mkdir`. */
/* rename(old, new). STRICT kernel semantics: the destination must not exist
 * (EMBKFS refuses with -EEXIST) -- the POSIX replace-atomically veneer lives in
 * the LIBC (unlink dest, then rename), same split as path_abs. Two path copies,
 * one syscall: git's lockfile commit is the customer. */
static int64_t sys_rename(const struct sysargs *a) {
    char oldp[SYSCALL_PATH_MAX], newp[SYSCALL_PATH_MAX];
    int len = copy_string_from_user(oldp, (const char *)a->arg[0], sizeof(oldp));
    if (len < 0) return len;
    len = copy_string_from_user(newp, (const char *)a->arg[1], sizeof(newp));
    if (len < 0) return len;
    return vfs_rename_path(oldp, newp);
}

/* chmod(path, mode) -- REAL: EMBKFS inodes have always carried a mode, there
 * was simply no road here from userspace. The fs preserves the file-TYPE bits
 * itself, so this cannot turn a file into a directory. git init's core.filemode
 * probe is what exposed the gap. */
static int64_t sys_chmod(const struct sysargs *a) {
    char path[SYSCALL_PATH_MAX];
    int len = copy_string_from_user(path, (const char *)a->arg[0], sizeof(path));
    if (len < 0) return len;
    return vfs_chmod_path(path, (uint32_t)a->arg[1]);
}

/* ftruncate(fd, size) -- REAL, not a stub: the per-fs truncate op already
 * exists (EMBKFS wires it); this only had no road from userspace. */
static int64_t sys_ftruncate(const struct sysargs *a) {
    return vfs_fd_truncate((int)a->arg[0], (uint64_t)a->arg[1]);
}

static int64_t sys_mkdir(const struct sysargs *a) {
    char path[SYSCALL_PATH_MAX];
    int len = copy_string_from_user(path, (const char *)a->arg[0], sizeof(path));
    if (len < 0) {
        return len;
    }
    return vfs_mkdir_path(path);
}

/* rmdir(path) -> 0 or -errno. EMPTY directories only (the fs enforces it --
 * embkfs_rmdir_name refuses a non-empty target); never recursive. */
static int64_t sys_rmdir(const struct sysargs *a) {
    char path[SYSCALL_PATH_MAX];
    int len = copy_string_from_user(path, (const char *)a->arg[0], sizeof(path));
    if (len < 0) {
        return len;
    }
    return vfs_rmdir_path(path);
}

/* set_fs_base(addr) -> 0, or -errno. Installs this thread's TLS thread pointer,
 * i.e. what %fs's base is while it runs. crt0 calls it once, and only for a
 * program that actually has a PT_TLS segment.
 *
 * SECURITY -- why the check is not optional: `addr` goes straight into
 * IA32_FS_BASE via WRMSR, and WRMSR raises #GP on a NON-CANONICAL value while
 * we are in RING 0. Passing this argument through unvalidated would be a
 * one-line user-triggered kernel fault. access_ok() is precisely the right
 * gate: it bounds the range to USER_VA_LIMIT (0x0000800000000000 -- the top of
 * the low canonical half), rejects wraps, and walks the page tables to confirm
 * the address is really mapped and user-accessible. A value that passes can be
 * neither non-canonical nor a kernel address.
 *
 * 8 bytes because that is exactly what the ABI dereferences first: every TLS
 * access opens with `mov %fs:0x0,%reg`, reading the TCB self-pointer AT the
 * base. A base whose first word isn't readable is useless anyway.
 *
 * Aiming %fs at some other mapping in your OWN address space is NOT a privilege
 * boundary -- ring 3 can already read all of it -- so nothing further is
 * restricted here.
 */
static int64_t sys_set_fs_base(const struct sysargs *a) {
    uint64_t base = a->arg[0];

    if (!access_ok((const void *)(uintptr_t)base, 8)) {
        return -EMBK_EFAULT;
    }

    struct thread *t = current_thread_atomic();
    if (!t) {
        return -EMBK_EFAULT;
    }

    /* Record it BEFORE installing it. This is the authoritative copy: if we get
     * preempted between these two lines, schedule() reinstalls from the field on
     * the way back in, and the MSR write below is merely redundant. Doing it the
     * other way round would leave a window where the MSR is set but the field
     * still says 0, and the next switch would silently clear %fs. */
    t->fs_base = base;
    arch_tls_base_set(base);
    return 0;
}

/* proc_list(out, max) -> count, or -errno. Copies up to `max` process_info
 * snapshots into the caller's buffer -- the shell's `ps`. Read-only system
 * introspection; the struct is mirrored FIELD-FOR-FIELD by embk.h's
 * embk_proc_info (grow both together). */
static int64_t sys_proc_list(const struct sysargs *a) {
    struct process_info *user_out = (struct process_info *)a->arg[0];
    int max = (int)a->arg[1];
    if (max <= 0 || !user_out) {
        return -EMBK_EINVAL;
    }
    if (max > MAX_PROCESSES) {
        max = MAX_PROCESSES;
    }
    struct process_info snap[MAX_PROCESSES];
    int n = process_list(snap, max);
    if (n > 0 && copy_to_user(user_out, snap, (size_t)n * sizeof(snap[0])) != EMBK_OK) {
        return -EMBK_EFAULT;
    }
    return n;
}

/* proc_kill(pid) -> 0 or -errno. The shell's `kill <pid>`.
 * DELIBERATE AMBIENT AUTHORITY: unlike SYS_kill (which takes a spawn HANDLE,
 * capability-scoped to your own children), this names any pid -- the shell
 * is this single-user OS's process manager and needs to kill what `ps`
 * shows. If multi-user ever happens, this is the syscall to gate. */
static int64_t sys_proc_kill(const struct sysargs *a) {
    uint32_t pid = (uint32_t)a->arg[0];
    if (!process_alive(pid)) {
        return -EMBK_ENOENT;
    }
    process_kill(pid);
    return 0;
}

/* lseek(fd, offset, whence) -> new absolute offset, or -errno.
 * whence: 0 = SEEK_SET, 1 = SEEK_CUR, 2 = SEEK_END (vfs_fd_seek's existing
 * numeric convention -- no libc yet to define the usual named constants
 * against). */
static int64_t sys_lseek(const struct sysargs *a) {
    int fd = (int)a->arg[0];
    int64_t offset = (int64_t)a->arg[1];
    int whence = (int)a->arg[2];

    uint64_t new_pos = 0;
    int rc = vfs_fd_seek(fd, offset, whence, &new_pos);
    if (rc != EMBK_OK) {
        return rc;
    }
    return (int64_t)new_pos;
}

/* stat(path, out_stat) -> 0, or -errno. `out_stat` must point at
 * sizeof(struct vfs_stat) bytes of user memory; filled via copy_to_user(). */
static int64_t sys_stat(const struct sysargs *a) {
    const char *user_path = (const char *)a->arg[0];
    struct vfs_stat *user_out = (struct vfs_stat *)a->arg[1];

    char path[SYSCALL_PATH_MAX];
    int len = copy_string_from_user(path, user_path, sizeof(path));
    if (len < 0) {
        return len;
    }

    struct vfs_stat st;
    memset(&st, 0, sizeof(st));   /* same mtime-honesty zeroing as sys_fstat */
    int rc = vfs_stat(path, &st);
    if (rc != EMBK_OK) {
        return rc;
    }

    if (copy_to_user(user_out, &st, sizeof(st)) != EMBK_OK) {
        return -EMBK_EFAULT;
    }
    return 0;
}

/* readdir(path, out_entries, max_entries) -> number of entries written, or
 * -errno. Not POSIX's getdents (variable-length, fd-based, resumable) --
 * this kernel has no libc yet to match an ABI against, so it's a single
 * call that walks the whole directory (path-based, like every other vfs_*
 * call here) and packs up to max_entries fixed-size records into the
 * caller's buffer. Revisit once something needs a directory too large to
 * read in one call. */
struct sys_dirent {
    uint64_t ino;
    uint8_t  type;      // VFS_DT_*
    char     name[59];  // NUL-terminated, truncated if longer (sizeof == 64 with ino+type)
};

struct sys_readdir_ctx {
    struct sys_dirent *user_entries;
    uint32_t max_entries;
    uint32_t count;
    int      error;
};

static int sys_readdir_cb(const char *name, uint8_t name_len, uint8_t type,
                          uint64_t ino, void *ctx_) {
    struct sys_readdir_ctx *ctx = (struct sys_readdir_ctx *)ctx_;
    if (ctx->count >= ctx->max_entries) {
        return EMBK_OK;   // caller's buffer is full; stop collecting, not an error
    }

    struct sys_dirent ent;
    memset(&ent, 0, sizeof(ent));
    ent.ino = ino;
    ent.type = type;
    size_t copy_len = name_len < sizeof(ent.name) - 1 ? name_len : sizeof(ent.name) - 1;
    memcpy(ent.name, name, copy_len);
    ent.name[copy_len] = '\0';

    if (copy_to_user(&ctx->user_entries[ctx->count], &ent, sizeof(ent)) != EMBK_OK) {
        ctx->error = -EMBK_EFAULT;
        return -EMBK_EFAULT;   // stop the walk
    }
    ctx->count++;
    return EMBK_OK;
}

static int64_t sys_readdir(const struct sysargs *a) {
    const char *user_path = (const char *)a->arg[0];
    struct sys_dirent *user_entries = (struct sys_dirent *)a->arg[1];
    uint32_t max_entries = (uint32_t)a->arg[2];

    char path[SYSCALL_PATH_MAX];
    int len = copy_string_from_user(path, user_path, sizeof(path));
    if (len < 0) {
        return len;
    }

    struct sys_readdir_ctx ctx = {
        .user_entries = user_entries,
        .max_entries = max_entries,
        .count = 0,
        .error = 0,
    };
    int rc = vfs_readdir(path, sys_readdir_cb, &ctx);
    if (ctx.error != 0) {
        return ctx.error;
    }
    if (rc != EMBK_OK) {
        return rc;
    }
    return (int64_t)ctx.count;
}

/* spawn(path) -> handle, or -errno. docs/ARCHITECTURE.md §3.2's spawn()
 * shape: builds a fresh address space directly from an ELF path, no
 * fork+exec. This is a thin wrapper around process_create() (process.c),
 * which already does exactly that -- the only syscall-specific work is
 * copying the path in from user space. The new process is left
 * PROCESS_READY; it isn't run synchronously here, it starts getting real
 * CPU time the next time schedule() (cooperative or timer-driven) picks it
 * up, same as any other READY process. No file-actions list yet (§3.2's
 * fuller model).
 *
 * Returns an opaque per-caller HANDLE (docs/ARCHITECTURE.md §3.4/§3.5), not
 * the raw pid -- process_create() already recorded current_process as the
 * child's parent, so the handle is really just a capability wrapper around
 * that same relationship: only the parent (or whoever it hands the handle
 * to) can ever name this child via sys_wait/sys_kill, closing the
 * confused-deputy gap a bare pid argument would leave open at this
 * boundary (any ring-3 process could otherwise wait()/kill() any pid it
 * could guess). If the handle table is full, the orphaned child is killed
 * immediately rather than leaked unreferenced. */
static int64_t sys_spawn(const struct sysargs *a) {
    const char *user_path = (const char *)a->arg[0];
    const uint64_t *user_argv = (const uint64_t *)a->arg[1];
    int argc = (int)a->arg[2];
    const struct spawn_file_action *user_actions = (const struct spawn_file_action *)a->arg[3];
    int n_count = (int)a->arg[4];
    /* envp: NULL-TERMINATED, not counted -- there is no seventh argument
     * register, and it is the shape `char **environ` needs anyway. 0 means "give
     * the child NO environment", which is this OS's default: nothing is
     * inherited unless the parent names it (see spawn.h). */
    const uint64_t *user_envp = (const uint64_t *)a->arg[5];

    char path[SYSCALL_PATH_MAX];
    int len = copy_string_from_user(path, user_path, sizeof(path));
    if (len < 0) {
        return len;   // -EMBK_EFAULT or -EMBK_ENAMETOOLONG
    }

    /* SPAWN_ARGV_MAX (spawn.h) is the budget for the argv[] pointer array
     * INCLUDING the NULL terminator process_create() appends itself -- so
     * the real string count `argc` must leave room for it, i.e. reject
     * argc == SPAWN_ARGV_MAX, not just argc > SPAWN_ARGV_MAX. */
    if (argc < 0 || argc >= SPAWN_ARGV_MAX || n_count < 0 || n_count > SPAWN_ACTIONS_MAX) {
        return -EMBK_E2BIG;   // caller's request is too large to handle
    }

    /* Copy the argv POINTER ARRAY in first -- these are still PARENT-space
     * pointer (current_process is the parent here; the child will get its own
     * copy in its own address space) */
    uint64_t user_argv_ptrs[SPAWN_ARGV_MAX];
    if (argc > 0 && copy_from_user(user_argv_ptrs, user_argv, argc * sizeof(uint64_t)) != EMBK_OK) {
        return -EMBK_EFAULT;
    }
    
    /* Copy each string into ONE packed kernel buffer -- address-space agnostic
     * once it's here, which is what lets process_create() set up the child's
     * argv correctly without the child's CR3 ever being loaded. 
     * Deliberately NOT static: this syscall can run concurrently on a different
     * core for a different spawn() invocation. static buffer here would let 
     * two spawn() invocations corrupt each other's buffers. */
    /* kmalloc'd, NOT a stack array: at SPAWN_ARGV_BYTES_MAX (8 KiB) it would eat a
     * big fraction of the kstack before process_create_env() + elf_load() -- the
     * deepest chain in the kernel -- pile on. Freed on EVERY exit below, like
     * envp_buf. (A 96-object kernel link is the customer for the raised caps.) */
    char *argv_buf = kmalloc(SPAWN_ARGV_BYTES_MAX);
    if (!argv_buf) return -EMBK_ENOMEM;
    char *argv_kernel[SPAWN_ARGV_MAX];
    size_t used = 0;
    for (int i = 0; i < argc; i++) {
        int slen = copy_string_from_user(argv_buf + used, (const char *)user_argv_ptrs[i], SPAWN_ARGV_BYTES_MAX - used);
        if (slen < 0) {
            kfree(argv_buf);
            return slen;   // -EMBK_EFAULT or -EMBK_ENAMETOOLONG
        }
        argv_kernel[i] = argv_buf + used;
        used += (size_t)slen + 1;   // includes the null terminator

    }

    struct spawn_file_action actions_kernel[SPAWN_ACTIONS_MAX];
    if (n_count > 0 && copy_from_user(actions_kernel, user_actions,
         n_count * sizeof(struct spawn_file_action)) != EMBK_OK) {
        kfree(argv_buf);
        return -EMBK_EFAULT;
    }

    /* envp. NULL-terminated in USER space, so its length is discovered by
     * walking it -- one pointer at a time, since we cannot know how far it is
     * safe to read ahead. Bounded by SPAWN_ENVP_MAX.
     *
     * The string buffer is kmalloc'd, NOT another 1 KiB array here: this frame
     * already carries path[256] + argv_buf[1024] + actions_kernel[~2.2 KiB], and
     * a second 1 KiB would put it near a THIRD of the 16 KiB kernel stack before
     * process_create_env() and elf_load() add their own frames on top. This
     * codebase has already been bitten once by a big syscall buffer on that
     * stack (see SYSCALL_IO_CHUNK's history in sys_read). The alloc/free window
     * below is deliberately narrow -- two returns, both handled. */
    uint64_t user_envp_ptrs[SPAWN_ENVP_MAX];
    char *envp_kernel[SPAWN_ENVP_MAX];
    char *envp_buf = NULL;
    int envc = 0;
    if (user_envp) {
        for (;;) {
            uint64_t p;
            if (copy_from_user(&p, user_envp + envc, sizeof p) != EMBK_OK) {
                kfree(argv_buf);
                return -EMBK_EFAULT;
            }
            if (!p) break;                                  /* the terminator */
            /* -1 leaves room for the NULL process_create_env() expects. */
            if (envc >= SPAWN_ENVP_MAX - 1) { kfree(argv_buf); return -EMBK_E2BIG; }
            user_envp_ptrs[envc++] = p;
        }

        envp_buf = kmalloc(SPAWN_ENVP_BYTES_MAX);
        if (!envp_buf) { kfree(argv_buf); return -EMBK_ENOMEM; }
        size_t eused = 0;
        for (int i = 0; i < envc; i++) {
            int slen = copy_string_from_user(envp_buf + eused,
                                             (const char *)user_envp_ptrs[i],
                                             SPAWN_ENVP_BYTES_MAX - eused);
            if (slen < 0) { kfree(envp_buf); kfree(argv_buf); return slen; }  /* EFAULT/ENAMETOOLONG */
            envp_kernel[i] = envp_buf + eused;
            eused += (size_t)slen + 1;                       /* includes the NUL */
        }
        envp_kernel[envc] = NULL;
    }

    /* A SPAWN_ACTION_SET_CAPS action, if present, carries the child's requested
     * capability set in `flags`. Absent => EMBK_CAP_INHERIT (the child gets the
     * parent's whole set, the default every existing spawn already gets).
     * process_create_caps enforces the request is a subset of THIS process's
     * own set -- the userspace side of EMBX §6 step 9. */
    uint64_t requested_caps = EMBK_CAP_INHERIT;
    for (int i = 0; i < n_count; i++) {
        if (actions_kernel[i].kind == SPAWN_ACTION_SET_CAPS) {
            requested_caps = (uint64_t)(uint32_t)actions_kernel[i].flags;
            break;
        }
    }

    /* user_envp==0 stays NULL, NOT an empty vector: "no environment" is the
     * default and a distinct, honest answer from "an empty environment". */
    int pid = process_create_caps(path, argv_kernel, argc,
                                 user_envp ? envp_kernel : NULL,
                                 actions_kernel, n_count, requested_caps);
    /* Safe already: process_create_env() COPIES every string into the child's
     * own stack before returning -- that is its documented contract. */
    if (envp_buf) kfree(envp_buf);
    kfree(argv_buf);
    if (pid < 0) {
        return pid;   // -errno from process_create_env()
    }
    
    int handle = process_handle_alloc(current_process, (uint32_t)pid);
    if (handle < 0) {
        /* Table full -- almost always because the caller spawned children and
         * never wait()'d the dead ones, leaking a handle slot each. Reclaim any
         * handles that name already-exited children (and reap those zombies),
         * then retry, so a leak-filled table doesn't force us to KILL the valid
         * child we just created. */
        if (process_handle_reap_dead(current_process) > 0) {
            handle = process_handle_alloc(current_process, (uint32_t)pid);
        }
    }
    if (handle < 0) {
        /* Genuinely at capacity -- every handle names a still-LIVE child, so
         * there's no slot to name this one and (with no init-reaper for
         * orphans) leaving it unnamed would leak it. Kill it and report EMFILE. */
        process_kill((uint32_t)pid);
        return handle;   // -EMBK_EMFILE
    }
    return handle;
}

/* wait(handle) -> exit_code, or -errno (-EMBK_EINVAL for a bad/unknown
 * handle, -EMBK_ECHILD if it doesn't/no-longer names one of our children).
 * Resolves the handle to a pid, then blocks on process_wait() (process.c)
 * -- a REAL block via the target's parent's child_wait queue, woken by the
 * child's own exit/kill, not a busy-poll. Frees the handle on return either
 * way (successful reap or a stale/invalid handle): a handle is single-use
 * for waiting, matching the one-shot nature of an exit status. */
static int64_t sys_wait(const struct sysargs *a) {
    int handle = (int)a->arg[0];

    uint32_t pid;
    int rc = process_handle_resolve(current_process, handle, &pid);
    if (rc != 0) {
        return rc;   // -EMBK_EINVAL
    }

    int code = process_wait(pid);
    process_handle_free(current_process, handle);
    return code;
}

/* kill(handle) -> 0, or -EMBK_EINVAL for a bad/unknown handle. The
 * userspace-reachable edge of the uncatchable kill
 * (docs/ARCHITECTURE.md §3.3, docs/architecture/process-and-scheduling.md
 * §15.2) -- process_kill() itself (process.c) has existed since Phase B,
 * used internally by the scheduler selftests, but was never exposed to
 * ring 3 until now. Does NOT free the handle: the caller still needs it to
 * sys_wait() afterward and collect the exit code (-1, "killed") the same
 * way a normal exit would be collected. */
static int64_t sys_kill(const struct sysargs *a) {
    int handle = (int)a->arg[0];

    uint32_t pid;
    int rc = process_handle_resolve(current_process, handle, &pid);
    if (rc != 0) {
        return rc;   // -EMBK_EINVAL
    }

    process_kill(pid);
    return 0;
}

/* cancel(handle) -> ask the child named by `handle` to stop. The POLITE half of
 * stopping something; see docs/INTERRUPTION.md.
 *
 * HANDLE-SCOPED, exactly like sys_kill and for the same reason: you may only
 * cancel a child YOU spawned and still hold a handle for. There is deliberately
 * no cancel-by-pid -- that would be ambient authority over a process you were
 * never handed, which is the property this OS refuses (and why kill(other_pid)
 * in our libc is an honest ENOSYS).
 *
 * The child's blocking syscalls then fail -EMBK_ECANCELED so it can clean up and
 * exit. It may also ignore this forever: sys_kill remains the uncatchable
 * backstop, and deciding when to escalate is the PARENT's policy, not ours. */
static int64_t sys_cancel(const struct sysargs *a) {
    int handle = (int)a->arg[0];

    uint32_t pid;
    int rc = process_handle_resolve(current_process, handle, &pid);
    if (rc != 0) {
        return rc;   // -EMBK_EINVAL
    }

    return process_cancel(pid);
}

/* cancelled(handle_or_-1) -> 1 if cancelled, else 0.
 *
 *   arg 0 < 0  : THIS process. For a compute loop that makes no syscalls -- with
 *              nothing injected into user control flow, a process that never
 *              blocks would otherwise never learn. Ambient over SELF is fine
 *              (same reasoning as sys_getpid): no confused deputy.
 *   arg 0 >= 0 : a CHILD named by a spawn handle the caller holds. This is the
 *              escalation primitive (docs/INTERRUPTION.md §4.3): a parent that
 *              delegated ^C never sees the keystroke, so watching the child's
 *              cancel state is the only honest way to start a grace-then-kill
 *              clock -- otherwise "cancelled but declining" looks identical to
 *              "healthy but slow". Handle-scoped like sys_cancel/sys_kill, and
 *              strictly weaker than both.
 *
 * Reading does NOT clear it -- the flag is sticky, so cleanup code that itself
 * blocks still sees -EMBK_ECANCELED rather than deadlocking on a cleared flag. */
static int64_t sys_cancelled(const struct sysargs *a) {
    int64_t handle = (int64_t)a->arg[0];
    if (handle < 0) {
        return current_process->cancelled ? 1 : 0;
    }
    uint32_t pid;
    int rc = process_handle_resolve(current_process, (int)handle, &pid);
    if (rc != 0) {
        return rc;   // -EMBK_EINVAL: not a child of yours
    }
    return process_is_cancelled(pid);
}

/* console_interrupt_route(handle) -> route ^C to the child named by `handle`;
 * handle < 0 clears the route. See docs/INTERRUPTION.md.
 *
 * A DELEGATION, not an inference. EmbLink has no "foreground process", no
 * session and no process group: the shell holds the console, and when it runs a
 * command it HANDS OVER the right to be interrupted -- exactly as it hands over
 * fds via file-actions and the environment via envp. Both "obvious" alternatives
 * were considered and fail (see the doc): "whoever holds the console" doesn't
 * discriminate, because stdio is inherited by default and BOTH shell and child
 * hold it; "whoever is blocked reading the console" has no target precisely when
 * you want one, because a compute-bound child isn't reading stdin.
 *
 * Only a HANDLE is accepted, never a pid: you can only point ^C at a child you
 * spawned. The slot itself is global (there is one console) and last-writer-wins
 * -- a single-user concession of the same class as sys_proc_kill's ambient
 * authority, and stated as such rather than dressed up. */
static int64_t sys_console_interrupt_route(const struct sysargs *a) {
    int handle = (int)a->arg[0];

    if (handle == 0) {                       /* route to SELF: the session owner
                                              * claiming ^C at its own prompt */
        keyboard_set_interrupt_target(current_process->pid);
        return 0;
    }

    if (handle < 0) {
        keyboard_set_interrupt_target(0);   /* reclaim: ^C is a byte again */
        return 0;
    }

    uint32_t pid;
    int rc = process_handle_resolve(current_process, handle, &pid);
    if (rc != 0) {
        return rc;   // -EMBK_EINVAL: not a child of yours
    }

    keyboard_set_interrupt_target(pid);
    return 0;
}

/* getpid() -> this process's pid. Trivial, but a real primitive a spawn()
 * caller needs -- e.g. to tell itself apart from a child that's about to
 * run the exact same binary from the same entry point. Deliberately still
 * the real pid, not a handle: a process always has ambient authority over
 * itself, so there's no confused-deputy concern for getpid() the way there
 * is for naming some OTHER process via spawn/wait/kill. */
static int64_t sys_getpid(const struct sysargs *a) {
    (void)a;
    return current_process->pid;
}


/* exit(code): mark the process a zombie and hand off to the scheduler.
 * process_exit_self() (process.c) does the actual work and is shared with
 * the in-kernel scheduler selftests, which need the identical mark-zombie-
 * then-reschedule path without going through a syscall. */
static int64_t sys_exit(const struct sysargs *a) {
    /* The exit code is the syscall's first ARGUMENT, in arg 0 (same slot
     * sys_write's fd comes from) -- NOT rax, which still holds the syscall
     * NUMBER (2, for every SYS_exit call) at this point. Reading rax here
     * used to silently discard every real exit code and replace it with 2. */
    int code = (int)a->arg[0];
    serial_write_string("\n[syscall] exit code=");
    serial_write_hex(code);
    serial_write_string("\n");

    process_exit_self(code);   // noreturn
}

/* yield(): voluntarily give up the CPU to the next READY process. process.c's
 * sys_yield() is void (no args, no return value) since it's also called
 * internally; wrap it to match the syscall_handler_t signature. */
static int64_t sys_yield_syscall(const struct sysargs *a) {
    (void)a;
    sys_yield();
    return 0;
}

/* thread_create(entry, arg) -> tid, or -errno. The ring-3 edge of Phase 5's
 * multi-thread primitive (docs/architecture/process-and-scheduling.md) --
 * an ADDITIONAL thread under the CALLER's own process, sharing its address
 * space, with its own dedicated user stack (thread_create_user(), process.c).
 *
 * `entry` is validated with access_ok() before ever being handed to the
 * scheduler: unlike sys_spawn's path (a fresh ELF's own entry point, chosen
 * by the trusted loader, not by the caller), this entry point is an
 * ARBITRARY ring-3-supplied address, so it gets the same "must be mapped in
 * the CALLER's own address space" check every other user pointer this
 * syscall layer accepts gets (usercopy.h) -- cheap insurance against a
 * caller (accidentally or not) pointing a brand-new thread at unmapped or
 * kernel-half memory before it ever gets to run.
 *
 * Returns a raw tid (thread_table[] slot index), not a capability handle --
 * see thread_create_user()'s doc comment (process.h) for why a thread
 * doesn't need the same confused-deputy protection sys_spawn's handle does:
 * a tid only ever names a thread of the CALLER's OWN process, there's no
 * cross-process thread naming at all. */
static int64_t sys_thread_create(const struct sysargs *a) {
    uint64_t entry_point = a->arg[0];
    uint64_t arg = a->arg[1];

    if (!access_ok((const void *)entry_point, 1)) {
        return -EMBK_EFAULT;
    }

    return thread_create_user(current_process, entry_point, arg);
}

/* thread_join(tid) -> exit_code, or -errno. Blocks (a real block, via
 * proc->join_wait -- not a busy-poll, same shape as sys_wait/process_wait())
 * until thread `tid` of the CALLER's own process exits, then reaps it and
 * returns its exit code. -EMBK_EINVAL for an unknown/wrong-process/
 * non-joinable/already-joined tid (thread_join(), process.c). */
static int64_t sys_thread_join(const struct sysargs *a) {
    int tid = (int)a->arg[0];
    return thread_join(current_process, tid);
}

/* thread_exit(code): mark the CALLING thread a zombie and hand off to the
 * scheduler -- ends only this thread, not necessarily the whole process
 * (thread_exit_self(), process.c). If it happens to be the process's last
 * thread, this completes the process exactly like sys_exit would. */
static int64_t sys_thread_exit(const struct sysargs *a) {
    int code = (int)a->arg[0];
    thread_exit_self(code);   // noreturn
}

/* sbrk(increment) -> new break pointer, or -errno. Adjusts the current
 * process's heap break pointer (current_process->heap_brk) by the signed
 * `increment` amount, growing or shrinking the heap. Returns the new break
 * pointer on success, or -errno on failure (e.g., exceeding USER_HEAP_VA_MAX
 * or underflowing below USER_HEAP_VA_BASE). */
static int64_t sys_sbrk(const struct sysargs *a) {
    int64_t increment = (int64_t)a->arg[0];
    /* _atomic, NOT the bare `current_process` macro: this drives a real
     * address-space mutation. With the plain macro a preempt-and-migrate
     * between this_cpu() and the deref made process_sbrk grow ANOTHER
     * process's heap -- mapping the pages into their pml4 and returning
     * THEIR break to our malloc, which then wrote to memory unmapped in our
     * address space. Observed live: shell.elf faulting in
     * malloc_extend_top, write to heap+1.1MB (the other process's
     * high-water mark), not-present, user-mode. See process.h. */
    struct process *proc = current_process_atomic();
    if (!proc) {
        return -EMBK_ENOMEM;   /* no process context -> no heap to grow;
                                * ENOMEM is what sbrk's contract expects */
    }
    return process_sbrk(proc, increment);
}

/* fstat(fd, out_stat) -> 0, or -errno. The fd-based companion to sys_stat's
 * path-based lookup -- newlib's stdio layer calls fstat() on every open
 * stream (to size its buffer / decide line- vs block-buffering), so a libc
 * port can't get far without it. Fills the kernel's neutral struct vfs_stat
 * (vfs.h); the userland _fstat stub maps that to newlib's struct stat.
 *
 * fds 0/1/2 aren't real vnodes (no console vnode yet -- see sys_write), so
 * vfs_fd_fstat() rejects them; the userland stub synthesizes a character-
 * device stat for those itself rather than calling here (the standard
 * newlib "_fstat on a tty returns S_IFCHR" shortcut), so this only ever
 * sees real fd >= FD_BASE. */
static int64_t sys_fstat(const struct sysargs *a) {
    int fd = (int)a->arg[0];
    struct vfs_stat *user_out = (struct vfs_stat *)a->arg[1];

    struct vfs_stat st;
    memset(&st, 0, sizeof(st));   /* fields an fs doesn't track (mtime on
                                   * FAT32/epfs) must read 0, not stack junk */
    int rc = vfs_fd_fstat(fd, &st);
    if (rc != EMBK_OK) {
        return rc;
    }
    if (copy_to_user(user_out, &st, sizeof(st)) != EMBK_OK) {
        return -EMBK_EFAULT;
    }
    return 0;
}

/* gettimeofday(out) -> 0, or -errno. Writes two uint64s -- {seconds,
 * microseconds} since the Unix epoch -- into the caller's buffer, sourced
 * from the CMOS RTC (rtc_now_unix(), the only wall-clock this kernel has).
 * microseconds is ALWAYS 0: a CMOS RTC's real resolution is one second
 * (rtc.h documents this precision gap), so fabricating sub-second digits
 * here would be dishonest. The userland _gettimeofday stub maps this pair
 * onto newlib's struct timeval; time() is then just the seconds field. */
static int64_t sys_gettimeofday(const struct sysargs *a) {
    uint64_t *user_out = (uint64_t *)a->arg[0];   // [0]=sec, [1]=usec

    uint64_t tv[2];
    tv[0] = rtc_now_unix();
    tv[1] = 0;
    if (copy_to_user(user_out, tv, sizeof(tv)) != EMBK_OK) {
        return -EMBK_EFAULT;
    }
    return 0;
}

/* ---- Surfaces (EmbLink UI Piece 1: shared-memory pixel buffers) ---------
 * Thin wrappers over kernel/gfx/surface.c. current_process is the client for
 * create; the caller for the rest. See surface.h and the design spec. */

static int64_t sys_surface_create(const struct sysargs *a) {
    /* RING 3: the capability GATE on handle installation. A GPU surface is the
     * handle a process needs before it can draw anything; withhold it from a
     * process that does not hold EMBK_CAP_GPU and every downstream surface op
     * has nothing to act on -- so the coarse capability gates WHICH handles get
     * installed, and the existing object-handle model does the actual
     * enforcement (this is the reconciliation in capabilities.h, made real).
     *
     * No app regresses: everything spawned today inherits EMBK_CAP_ALL from
     * init, so it holds GPU. Only a process DELIBERATELY spawned without it --
     * an EMBX binary that did not declare GPU, or a SET_CAPS-attenuated child --
     * is refused, which is the whole point. */
    int cg = cap_gate(EMBK_CAP_GPU);
    if (cg) return cg;

    uint32_t w = (uint32_t)a->arg[0], h = (uint32_t)a->arg[1];
    uint32_t fmt = (uint32_t)a->arg[2], n = (uint32_t)a->arg[3];
    struct surface_info *user_out = (struct surface_info *)a->arg[4];

    struct surface_info info;
    int handle = surface_create(current_process, w, h, fmt, n, &info);
    if (handle < 0) {
        return handle;
    }
    if (user_out && copy_to_user(user_out, &info, sizeof(info)) != EMBK_OK) {
        surface_destroy(current_process, handle);   // don't leak a half-handed-out surface
        return -EMBK_EFAULT;
    }
    return handle;
}

static int64_t sys_surface_map(const struct sysargs *a) {
    int handle = (int)a->arg[0];
    struct surface_info *user_out = (struct surface_info *)a->arg[1];

    struct surface_info info;
    uint64_t base = surface_map(current_process, handle, &info);
    if (!base) {
        return -EMBK_EINVAL;
    }
    if (user_out && copy_to_user(user_out, &info, sizeof(info)) != EMBK_OK) {
        return -EMBK_EFAULT;
    }
    return (int64_t)base;
}

static int64_t sys_surface_acquire(const struct sysargs *a) {
    return surface_acquire(current_process, (int)a->arg[0]);
}
static int64_t sys_surface_commit(const struct sysargs *a) {
    return surface_commit(current_process, (int)a->arg[0], (int)a->arg[1]);
}
static int64_t sys_surface_release(const struct sysargs *a) {
    return surface_release(current_process, (int)a->arg[0], (int)a->arg[1]);
}
static int64_t sys_surface_destroy(const struct sysargs *a) {
    return surface_destroy(current_process, (int)a->arg[0]);
}

/* ---- Direct present: blit a user BGRA8888 buffer to the framebuffer -------
 * A minimal "get pixels on screen" path for a single full-screen UI process,
 * standing in for a full compositor. Copies row-by-row via copy_from_user
 * (safe against a bad/short user pointer) and centres the image, then presents.
 * arg 0=user pixels, arg 1=w, arg 2=h. Source pixels are premultiplied BGRA (== a
 * little-endian 0xAARRGGBB uint32); fb_blit's opaque path copies the RGB. */
static int64_t sys_ui_present(const struct sysargs *a) {
    const uint8_t *upx = (const uint8_t *)a->arg[0];
    uint32_t w = (uint32_t)a->arg[1], h = (uint32_t)a->arg[2];
    const fb_info_t *fi = fb_get_info();
    if (!fi || w == 0 || h == 0 || w > fi->width || h > fi->height) return -EMBK_EINVAL;

    uint32_t *line = (uint32_t *)kmalloc((size_t)w * 4);
    if (!line) return -EMBK_ENOMEM;
    int32_t x = (int32_t)(fi->width  - w) / 2;
    int32_t y = (int32_t)(fi->height - h) / 2;
    for (uint32_t row = 0; row < h; row++) {
        if (copy_from_user(line, upx + (size_t)row * w * 4, (size_t)w * 4) != EMBK_OK) {
            kfree(line);
            return -EMBK_EFAULT;
        }
        fb_blit(x, y + (int32_t)row, (int32_t)w, 1, line, w);
    }
    kfree(line);
    fb_present();
    return 0;
}

/* Present only a sub-rectangle of the user surface to the framebuffer -- the
 * interactive path: an app that only moved its cursor uploads a handful of
 * rows instead of the whole surface (the full blit above dominates a TCG
 * frame). Args are packed to fit the register ABI:
 *   arg 0 = pixel base, arg 1 = (surf_w<<16)|surf_h,
 *   arg 2 = (rx<<48)|(ry<<32)|(rw<<16)|rh  (each 16-bit, surface-local). */
static int64_t sys_ui_present_rect(const struct sysargs *a) {
    const uint8_t *upx = (const uint8_t *)a->arg[0];
    uint32_t sw = ((uint32_t)a->arg[1] >> 16) & 0xFFFF, sh = (uint32_t)a->arg[1] & 0xFFFF;
    uint64_t rp = (uint64_t)a->arg[2];
    uint32_t rx = (uint32_t)((rp >> 48) & 0xFFFF), ry = (uint32_t)((rp >> 32) & 0xFFFF);
    uint32_t rw = (uint32_t)((rp >> 16) & 0xFFFF), rh = (uint32_t)(rp & 0xFFFF);
    const fb_info_t *fi = fb_get_info();
    if (!fi || sw == 0 || sh == 0 || sw > fi->width || sh > fi->height) return -EMBK_EINVAL;
    if (rx >= sw || ry >= sh || rw == 0 || rh == 0) return 0;   /* empty -> nothing to push */
    if (rx + rw > sw) rw = sw - rx;
    if (ry + rh > sh) rh = sh - ry;

    uint32_t *line = (uint32_t *)kmalloc((size_t)rw * 4);
    if (!line) return -EMBK_ENOMEM;
    int32_t x0 = (int32_t)(fi->width  - sw) / 2;
    int32_t y0 = (int32_t)(fi->height - sh) / 2;
    for (uint32_t row = 0; row < rh; row++) {
        const uint8_t *srow = upx + ((size_t)(ry + row) * sw + rx) * 4;
        if (copy_from_user(line, srow, (size_t)rw * 4) != EMBK_OK) {
            kfree(line);
            return -EMBK_EFAULT;
        }
        fb_blit(x0 + (int32_t)rx, y0 + (int32_t)(ry + row), (int32_t)rw, 1, line, rw);
    }
    kfree(line);
    fb_present();
    return 0;
}

/* Poll current pointer state into a user struct {int32 x,y; uint32 buttons}.
 * The ring-3 UI loop calls this each frame to drive hover/click. */
struct ui_input_kbuf { int32_t x, y; uint32_t buttons; int32_t wheel; };
static int64_t sys_ui_input(const struct sysargs *a) {
    struct ui_input_kbuf k;
    int32_t x = 0, y = 0; uint32_t b = 0;
    mouse_get_state(&x, &y, &b);
    k.x = x; k.y = y; k.buttons = b; k.wheel = mouse_take_wheel();
    if (copy_to_user((void *)a->arg[0], &k, sizeof k) != EMBK_OK) return -EMBK_EFAULT;
    return 0;
}

/* Non-blocking keystroke poll: returns the next ASCII byte (incl. '\b' 0x08 and
 * '\n'), or 0 if the keyboard buffer is empty. The ring-3 UI loop drains this
 * each frame and routes chars to the focused text field. */
/* screen_luma(x,y,w,h) -- how bright is what is already on screen there. */
static int64_t sys_screen_luma(const struct sysargs *a) {
    return (int64_t)compositor_backdrop_luma((int)a->arg[0], (int)a->arg[1],
                                             (int)a->arg[2], (int)a->arg[3]);
}

/* win_desktop_front(on) -- the shell lifting its own full-screen surface above
 * the app windows (the Applications launcher) and putting it back after. */
static int64_t sys_win_desktop_front(const struct sysargs *a) {
    return compositor_desktop_front(current_process ? (int)current_process->pid : 0,
                                    (int)a->arg[0]);
}

/* win_minimize(win) -- an app parking its OWN window. */
static int64_t sys_win_minimize(const struct sysargs *a) {
    return compositor_win_minimize(current_process ? (int)current_process->pid : 0,
                                   (uint32_t)a->arg[0]);
}

/* win_restore(handle) -- un-minimize and raise the windows of the process the
 * SPAWN HANDLE names. A handle, not a pid: the launcher is handed handles by
 * spawn and never learns pids (same contract as proc_alive), and resolving
 * through the handle table keeps this authority-scoped -- you can only bring
 * back an app you started. */
static int64_t sys_win_restore(const struct sysargs *a) {
    uint32_t pid;
    if (process_handle_resolve(current_process, (int)a->arg[0], &pid) != 0)
        return -EMBK_EINVAL;
    return (int64_t)compositor_restore_pid((int)pid);
}

static int64_t sys_win_blur_rect(const struct sysargs *a) {
    return compositor_win_blur_rect(current_process ? (int)current_process->pid : 0,
                                    (uint32_t)a->arg[0], (int)a->arg[1], (int)a->arg[2],
                                    (int)a->arg[3], (int)a->arg[4]);
}

/* ---- the system clipboard (ipc/clipboard.c) ---------------------------- */
static int64_t sys_clip_set(const struct sysargs *a) {
    return clipboard_set_user((const void *)a->arg[0], (size_t)a->arg[1]);
}
static int64_t sys_clip_get(const struct sysargs *a) {
    return clipboard_get_user((void *)a->arg[0], (size_t)a->arg[1]);
}

static int64_t sys_key_poll(const struct sysargs *a) {
    (void)a;
    /* Keys belong to the FRONT window's process. The character queue is a
     * single global one, so without this every UI process that polls drains it
     * and they race for each keystroke -- with two app loops running (a top bar
     * and an app) a terminal received roughly one press in ten and typing felt
     * like the keyboard was broken. Anyone who isn't focused reads nothing
     * rather than stealing somebody else's input. */
    uint32_t focus = compositor_focused_pid();
    if (focus && current_process && current_process->pid != focus) return 0;
    if (keyboard_has_char()) return (int64_t)(unsigned char)keyboard_getchar();
    return 0;
}

/* Pop one KEY EVENT (make OR break, with modifiers) into a user buffer.
 * Returns 1 on an event, 0 when the queue is empty, -EFAULT on a bad pointer.
 *
 * A SECOND stream beside sys_key_poll's characters, not a replacement -- see
 * keyboard.h for why they cannot be one thing (C0 is full: Up and Ctrl+S are
 * the same byte, and F-keys / releases have no byte at all). Text readers keep
 * using key_poll and are untouched.
 *
 * Non-blocking on purpose: the callers are frame loops and games, which already
 * have a clock and must not park a thread on a keypress. */
static int64_t sys_key_event_poll(const struct sysargs *a) {
    struct key_event ev;
    if (!keyboard_event_pop(&ev)) return 0;
    if (copy_to_user((void *)a->arg[0], &ev, sizeof ev) != EMBK_OK) return -EMBK_EFAULT;
    return 1;
}

/* The live modifier bitmap (EKM_*). The event stream says what CHANGED; this
 * says what is held RIGHT NOW, which a poller that missed a make cannot infer. */
static int64_t sys_key_mods(const struct sysargs *a) {
    (void)a;
    return (int64_t)keyboard_mods();
}

/* Grab/release the keyboard so the kernel shell stops draining it while a UI app
 * owns keystrokes (arg 0 != 0 grabs). Auto-released when this process is reaped. */
static int64_t sys_key_grab(const struct sysargs *a) {
    keyboard_set_grab(a->arg[0] != 0, current_process ? current_process->pid : 0);
    return 0;
}

/* Monotonic milliseconds since boot -- the clock the ring-3 UI animator ticks
 * on. timer_uptime_ms() is exactly this on both architectures (the HPET behind
 * a TSC on x86, the architectural counter on aarch64), and naming the device
 * here was the only thing that made this function x86-specific. */
static uint64_t uptime_ms_now(void) {
    return timer_uptime_ms();
}

static int64_t sys_uptime_ms(const struct sysargs *a) {
    (void)a;
    return (int64_t)uptime_ms_now();
}

/* Sleep >= arg 0 milliseconds.
 *
 * This WAS a yield loop against an HPET deadline, and the comment here said
 * why: "not a blocking timer wait (no scheduler-side wakeup list --
 * deliberately zero scheduler surgery), but it removes ~all of the idle CPU
 * theft". It removed most of it and left the part that mattered most: a
 * yield loop is RUNNABLE, so a sleeping app kept the scheduler permanently
 * supplied with work and no core ever reached its idle thread. The kernel now
 * has the wakeup list (sched_sleep_ms), so a sleeping process genuinely
 * blocks and a machine with nothing to do genuinely halts. */
static int64_t sys_sleep_ms(const struct sysargs *a) {
    sched_sleep_ms(a->arg[0]);
    return 0;
}

/* 1 if the child named by spawn HANDLE `arg 0` is alive, else 0. Takes the same
 * handle sys_spawn returned (NOT a raw pid -- spawn deliberately never exposes
 * pids), so the check is pinned to the exact child instance: a recycled pid
 * can't alias, and an unknown/freed handle is simply "not alive". The home
 * launcher uses this to not re-spawn an app whose tile is clicked twice. */
static int64_t sys_proc_alive(const struct sysargs *a) {
    uint32_t pid;
    if (process_handle_resolve(current_process, (int)a->arg[0], &pid) != 0)
        return 0;
    return (int64_t)process_alive(pid);
}

/* win_resize(id, w, h, &out_va) -> id, with the window's NEW shared-pixel
 * mapping base written to *out_va (the old mapping is gone on return). */
static int64_t sys_win_resize(const struct sysargs *a) {
    uint64_t cva = 0;
    int64_t rc = compositor_win_resize(current_process, (uint32_t)a->arg[0],
                                       (uint32_t)a->arg[1], (uint32_t)a->arg[2], &cva);
    if (rc < 0) return rc;
    if (a->arg[3] && copy_to_user((void *)a->arg[3], &cva, sizeof(cva)) != EMBK_OK)
        return -EMBK_EFAULT;
    return rc;
}

/* ---- window compositor (EmbLink UI Piece 2) -----------------------------
 * Thin wrappers over kernel/gfx/compositor.c. The client renders into its own
 * buffer and PRESENTS pixels (copy_from_user) into a kernel window content
 * buffer; the compositor draws all windows over a desktop with chrome. */

/* arg 0=content_w, arg 1=content_h, arg 2=x, arg 3=y, arg 4=title(user char*),
 * arg 5=out uint64_t* for the shared client VA (0 => plain copy window).
 * Returns a window id (>0) or -EMBK_*. */
static int64_t sys_win_create(const struct sysargs *a) {
    int cg = cap_gate(EMBK_CAP_GPU);      /* a window is a GPU resource */
    if (cg) return cg;
    /* Window-style flags ride the high bits of arg 0 (cw is <= 16 bits real). */
    uint32_t cw = (uint32_t)(a->arg[0] & 0xFFFFFFFFULL), ch = (uint32_t)a->arg[1];
    int chromeless  = (a->arg[0] >> 32) & 1;   /* EMBK_WINF_CHROMELESS */
    int widget      = (a->arg[0] >> 33) & 1;   /* EMBK_WINF_WIDGET */
    int glass       = (a->arg[0] >> 34) & 1;   /* EMBK_WINF_GLASS */
    int translucent = (a->arg[0] >> 35) & 1;   /* EMBK_WINF_TRANSLUCENT */
    int32_t x = (int32_t)a->arg[2], y = (int32_t)a->arg[3];
    char title[COMP_TITLE_MAX + 1];
    int i = 0;
    if (a->arg[4]) {
        for (; i < COMP_TITLE_MAX; i++) {
            char c;
            if (copy_from_user(&c, (const void *)(a->arg[4] + (uint64_t)i), 1) != EMBK_OK) break;
            if (!c) break;
            title[i] = c;
        }
    }
    title[i] = 0;
    int pid = current_process ? (int)current_process->pid : -1;

    if (a->arg[5]) {   /* zero-copy: map the pixel pages into the client, return VA */
        uint64_t cva = 0;
        int64_t id = widget
            ? compositor_win_create_widget(current_process, cw, ch, x, y, title, glass, &cva)
            : translucent
            ? compositor_win_create_translucent(current_process, cw, ch, x, y, title, &cva)
            : glass
            ? compositor_win_create_glass(current_process, cw, ch, x, y, title, &cva)
            : chromeless
            ? compositor_win_create_chromeless(current_process, cw, ch, x, y, title, &cva)
            : compositor_win_create_shared(current_process, cw, ch, x, y, title, &cva);
        if (id < 0) return id;
        if (copy_to_user((void *)a->arg[5], &cva, sizeof(cva)) != EMBK_OK) {
            compositor_win_destroy(pid, (uint32_t)id);
            return -EMBK_EFAULT;
        }
        return id;
    }
    return compositor_win_create(pid, cw, ch, x, y, title);
}

/* arg 0=win id, arg 1=user pixels (whole cw*ch content), arg 2=(cw<<16)|ch,
 * arg 3=(rx<<48)|(ry<<32)|(rw<<16)|rh  (0 => present whole window). */
static int64_t sys_win_present(const struct sysargs *a) {
    uint32_t id = (uint32_t)a->arg[0];
    const uint8_t *upx = (const uint8_t *)a->arg[1];
    uint32_t uw = ((uint32_t)a->arg[2] >> 16) & 0xFFFF, uh = (uint32_t)a->arg[2] & 0xFFFF;
    int pid = current_process ? (int)current_process->pid : -1;

    uint32_t cw = 0, ch = 0;
    uint32_t *content = compositor_win_content(pid, id, &cw, &ch);
    if (!content) return -EMBK_ENOENT;
    if (uw != cw || uh != ch) return -EMBK_EINVAL;   /* client/kernel disagree */

    uint64_t rp = (uint64_t)a->arg[3];
    uint32_t rx, ry, rw, rh;
    if (rp == 0) { rx = 0; ry = 0; rw = cw; rh = ch; }
    else {
        rx = (uint32_t)((rp >> 48) & 0xFFFF); ry = (uint32_t)((rp >> 32) & 0xFFFF);
        rw = (uint32_t)((rp >> 16) & 0xFFFF); rh = (uint32_t)(rp & 0xFFFF);
    }
    if (rx >= cw || ry >= ch || rw == 0 || rh == 0) return 0;
    if (rx + rw > cw) rw = cw - rx;
    if (ry + rh > ch) rh = ch - ry;

    /* A shared (zero-copy) window's client already rendered straight into the
     * shared pages -- there's nothing to copy, just damage. A plain window
     * uploads the presented rows from the client's private buffer. */
    if (!compositor_win_is_shared(pid, id)) {
        for (uint32_t row = 0; row < rh; row++) {
            size_t off = ((size_t)(ry + row) * cw + rx);
            if (copy_from_user(content + off, upx + off * 4, (size_t)rw * 4) != EMBK_OK)
                return -EMBK_EFAULT;
        }
    }
    return compositor_win_damage(pid, id, rx, ry, rw, rh);
}

/* arg 0=win id, arg 1=x, arg 2=y (screen coords of the window frame's top-left). */
static int64_t sys_win_move(const struct sysargs *a) {
    int pid = current_process ? (int)current_process->pid : -1;
    return compositor_win_move(pid, (uint32_t)a->arg[0], (int32_t)a->arg[1], (int32_t)a->arg[2]);
}

/* arg 0=win id. */
static int64_t sys_win_destroy(const struct sysargs *a) {
    int pid = current_process ? (int)current_process->pid : -1;
    return compositor_win_destroy(pid, (uint32_t)a->arg[0]);
}

/* Report the screen (framebuffer) size so a windowed app can fit itself to it.
 * Writes width to *[arg 0] and height to *[arg 1]. */
static int64_t sys_screen_size(const struct sysargs *a) {
    const fb_info_t *fi = fb_get_info();
    uint32_t w = fi ? fi->width : 0, h = fi ? fi->height : 0;
    if ((a->arg[0] && copy_to_user((void *)a->arg[0], &w, sizeof w) != EMBK_OK) ||
        (a->arg[1] && copy_to_user((void *)a->arg[1], &h, sizeof h) != EMBK_OK))
        return -EMBK_EFAULT;
    return 0;
}

/* Create the full-screen chromeless HOME/desktop window (zero-copy). Returns the
 * window id; writes the client pixel VA to *[arg 0] and the screen size to *[arg 1]
 * (w) and *[arg 2] (h) so the app learns the dimensions. */
static int64_t sys_win_create_desktop(const struct sysargs *a) {
    int cg = cap_gate(EMBK_CAP_GPU);
    if (cg) return cg;
    uint64_t cva = 0; uint32_t w = 0, h = 0;
    int64_t id = compositor_win_create_desktop(current_process, &cva, &w, &h);
    if (id < 0) return id;
    int pid = current_process ? (int)current_process->pid : -1;
    if ((a->arg[0] && copy_to_user((void *)a->arg[0], &cva, sizeof cva) != EMBK_OK) ||
        (a->arg[1] && copy_to_user((void *)a->arg[1], &w, sizeof w) != EMBK_OK) ||
        (a->arg[2] && copy_to_user((void *)a->arg[2], &h, sizeof h) != EMBK_OK)) {
        compositor_win_destroy(pid, (uint32_t)id);
        return -EMBK_EFAULT;
    }
    return id;
}

/* arg 0 = user ptr to struct { int32_t focused; int32_t x, y; uint32_t buttons,
 * win; }. Delivers the content-local pointer if this process owns the window
 * under the cursor (focused=1) else focused=0. */
struct win_input_kbuf { int32_t focused; int32_t x, y; uint32_t buttons; uint32_t win; int32_t wheel; };
static int64_t sys_win_input(const struct sysargs *a) {
    int pid = current_process ? (int)current_process->pid : -1;
    int32_t lx = 0, ly = 0, wheel = 0; uint32_t btn = 0, win = 0;
    int foc = compositor_win_input(pid, &lx, &ly, &btn, &win, &wheel);
    struct win_input_kbuf k = { foc, lx, ly, btn, win, wheel };
    if (copy_to_user((void *)a->arg[0], &k, sizeof k) != EMBK_OK) return -EMBK_EFAULT;
    return 0;
}

/* ---- IPC channels (EmbLink UI Piece 1, Layer A) -------------------------
 * The syscall layer does the user<->kernel copies; kernel/ipc/channel.c does
 * the queueing, blocking, and handle transfer. */

static int64_t sys_chan_pair(const struct sysargs *a) {
    int *user_out = (int *)a->arg[0];   /* -> int[2] */
    int h0 = -1, h1 = -1;
    int rc = channel_create_pair(current_process, &h0, &h1);
    if (rc < 0) return rc;
    int out[2] = { h0, h1 };
    if (copy_to_user(user_out, out, sizeof(out)) != EMBK_OK) {
        channel_close(current_process, h0);
        channel_close(current_process, h1);
        return -EMBK_EFAULT;
    }
    return 0;
}

static int64_t sys_chan_send(const struct sysargs *a) {
    int handle       = (int)a->arg[0];
    const void *ubytes = (const void *)a->arg[1];
    uint32_t len     = (uint32_t)a->arg[2];
    const int *uhnds = (const int *)a->arg[3];
    const int *uflags= (const int *)a->arg[4];
    uint32_t n_hnd   = (uint32_t)a->arg[5];

    if (len > CHAN_MSG_MAX_BYTES || n_hnd > CHAN_MSG_MAX_HANDLES) return -EMBK_EINVAL;

    uint8_t kbytes[CHAN_MSG_MAX_BYTES];
    if (len && copy_from_user(kbytes, ubytes, len) != EMBK_OK) return -EMBK_EFAULT;

    int khnds[CHAN_MSG_MAX_HANDLES];
    int kflags[CHAN_MSG_MAX_HANDLES];
    if (n_hnd) {
        if (copy_from_user(khnds, uhnds, n_hnd * sizeof(int)) != EMBK_OK) return -EMBK_EFAULT;
        if (uflags) {
            if (copy_from_user(kflags, uflags, n_hnd * sizeof(int)) != EMBK_OK) return -EMBK_EFAULT;
        } else {
            for (uint32_t i = 0; i < n_hnd; i++) kflags[i] = CHAN_HANDLE_COPY;
        }
    }
    return channel_send(current_process, handle, len ? kbytes : 0, len,
                        n_hnd ? khnds : 0, n_hnd ? kflags : 0, n_hnd);
}

static int64_t sys_chan_recv(const struct sysargs *a) {
    int handle          = (int)a->arg[0];
    void *ubuf          = (void *)a->arg[1];
    uint32_t buflen     = (uint32_t)a->arg[2];
    uint32_t *u_outlen  = (uint32_t *)a->arg[3];
    int *u_outhnds      = (int *)a->arg[4];
    uint32_t *u_outnhnd = (uint32_t *)a->arg[5];

    /* Effective limit = min(user buflen, kernel buffer). channel_recv returns
     * EMSGSIZE (message left queued) if the message exceeds it, so we never
     * overrun the user buffer. */
    uint32_t eff = buflen > CHAN_MSG_MAX_BYTES ? CHAN_MSG_MAX_BYTES : buflen;
    uint8_t kbuf[CHAN_MSG_MAX_BYTES];
    uint32_t out_len = 0, out_nhnd = 0;
    int out_hnds[CHAN_MSG_MAX_HANDLES];

    int64_t rc = channel_recv(current_process, handle, kbuf, eff, &out_len, out_hnds, &out_nhnd);
    if (rc < 0) return rc;

    if (out_len && ubuf && copy_to_user(ubuf, kbuf, out_len) != EMBK_OK) return -EMBK_EFAULT;
    if (u_outlen && copy_to_user(u_outlen, &out_len, sizeof(out_len)) != EMBK_OK) return -EMBK_EFAULT;
    if (out_nhnd && u_outhnds &&
        copy_to_user(u_outhnds, out_hnds, out_nhnd * sizeof(int)) != EMBK_OK) return -EMBK_EFAULT;
    if (u_outnhnd && copy_to_user(u_outnhnd, &out_nhnd, sizeof(out_nhnd)) != EMBK_OK) return -EMBK_EFAULT;
    return 0;
}

static int64_t sys_chan_close(const struct sysargs *a) {
    return channel_close(current_process, (int)a->arg[0]);
}

/* ---- Rendezvous (EmbLink UI Piece 1, Layer B) ---------------------------
 * VFS named listening endpoints (epfs, mounted at /run). `path` copied in
 * through a bounded kernel buffer, same pattern as sys_open/sys_stat. */

static int64_t sys_chan_listen(const struct sysargs *a) {
    const char *user_path = (const char *)a->arg[0];
    char path[SYSCALL_PATH_MAX];
    int len = copy_string_from_user(path, user_path, sizeof(path));
    if (len < 0) return len;
    return endpoint_listen(current_process, path);
}

static int64_t sys_chan_accept(const struct sysargs *a) {
    return endpoint_accept(current_process, (int)a->arg[0]);
}

static int64_t sys_chan_connect(const struct sysargs *a) {
    const char *user_path = (const char *)a->arg[0];
    char path[SYSCALL_PATH_MAX];
    int len = copy_string_from_user(path, user_path, sizeof(path));
    if (len < 0) return len;
    return endpoint_connect(current_process, path);
}

static int64_t sys_tty_mode(const struct sysargs *a) {
    int mode = (int)a->arg[0];
    if (mode != TTY_COOKED && mode != TTY_RAW) return -EMBK_EINVAL;
    enum tty_mode old = tty_get_mode();
    tty_set_mode((enum tty_mode)mode);
    return (uint64_t)old;
}

/* getcaps() -> this process's coarse resource-class capability set
 * (capabilities.h). The introspection half of the capability model: a program
 * can ask what it was granted rather than probe-by-failing. Also the witness
 * `test spawncaps` uses to prove attenuation crossed the syscall boundary. */
static int64_t sys_getcaps(const struct sysargs *a) {
    (void)a;
    return (int64_t)process_current_caps();
}

/* ---- Networking (M4): the ring-3 socket surface -------------------------
 * A socket is an fd backed by a TCP connection (FD_BACKING_SOCKET, fd.c).
 * socket() claims an unconnected one, connect() runs the active open, and
 * read()/write()/close() on the fd route to net_tcp_recv/send/close. The
 * capability gates socket creation and name resolution -- once a process holds
 * a socket fd, read/write are scoped to it (the same open-vs-read/write rule
 * the filesystem gate uses). IPs cross the boundary in HOST order. */
static int64_t sys_net_socket(const struct sysargs *a) {
    int cg = cap_gate(EMBK_CAP_NETWORK);
    if (cg) return cg;
    /* type: 2 = SOCK_DGRAM (UDP); anything else = SOCK_STREAM (TCP). */
    if ((int)a->arg[0] == 2) return fd_alloc_udp(current_process);
    return fd_alloc_socket(current_process);
}

static int64_t sys_net_connect(const struct sysargs *a) {
    int cg = cap_gate(EMBK_CAP_NETWORK);
    if (cg) return cg;
    return fd_socket_connect(current_process, (int)a->arg[0],
                             (uint32_t)a->arg[1], (uint16_t)a->arg[2]);
}

static int64_t sys_net_resolve(const struct sysargs *a) {
    int cg = cap_gate(EMBK_CAP_NETWORK);
    if (cg) return cg;
    char name[256];
    int len = copy_string_from_user(name, (const char *)a->arg[0], sizeof(name));
    if (len < 0) return len;
    uint32_t ip = 0;
    if (!net_resolve(name, &ip)) return -EMBK_ECONNREFUSED;   /* no A record / no answer */
    if (copy_to_user((void *)a->arg[1], &ip, sizeof(ip)) != EMBK_OK) return -EMBK_EFAULT;
    return 0;
}

/* Server side: bind a local port, listen, and accept (returns a NEW socket fd
 * for the connection). All CAP_NETWORK-gated, same as the client calls. */
static int64_t sys_net_bind(const struct sysargs *a) {
    int cg = cap_gate(EMBK_CAP_NETWORK);
    if (cg) return cg;
    return fd_socket_bind(current_process, (int)a->arg[0], (uint16_t)a->arg[1]);
}
static int64_t sys_net_listen(const struct sysargs *a) {
    int cg = cap_gate(EMBK_CAP_NETWORK);
    if (cg) return cg;
    return fd_socket_listen(current_process, (int)a->arg[0]);   /* a->arg[1] = backlog, ignored */
}
static int64_t sys_net_accept(const struct sysargs *a) {
    int cg = cap_gate(EMBK_CAP_NETWORK);
    if (cg) return cg;
    return fd_socket_accept(current_process, (int)a->arg[0]);
}

/* UDP datagram send/receive over a SOCK_DGRAM fd. Payloads bounce through a
 * kernel buffer (<= one datagram); recvfrom optionally reports the source. */
static int64_t sys_net_sendto(const struct sysargs *a) {
    int cg = cap_gate(EMBK_CAP_NETWORK);
    if (cg) return cg;
    uint32_t len = (uint32_t)a->arg[4];
    if (len > 1472) len = 1472;
    uint8_t kbuf[1472];
    if (len && copy_from_user(kbuf, (const void *)a->arg[3], len) != EMBK_OK) return -EMBK_EFAULT;
    return fd_udp_sendto(current_process, (int)a->arg[0], (uint32_t)a->arg[1], (uint16_t)a->arg[2], kbuf, len);
}
static int64_t sys_net_recvfrom(const struct sysargs *a) {
    int cg = cap_gate(EMBK_CAP_NETWORK);
    if (cg) return cg;
    uint32_t cap = (uint32_t)a->arg[2];
    if (cap > 1472) cap = 1472;
    uint8_t kbuf[1472];
    uint32_t src_ip = 0; uint16_t src_port = 0;
    int n = fd_udp_recvfrom(current_process, (int)a->arg[0], kbuf, cap, &src_ip, &src_port);
    if (n < 0) return n;
    if (n && copy_to_user((void *)a->arg[1], kbuf, (size_t)n) != EMBK_OK) return -EMBK_EFAULT;
    if (a->arg[3] && copy_to_user((void *)a->arg[3], &src_ip, sizeof(src_ip)) != EMBK_OK) return -EMBK_EFAULT;
    if (a->arg[4]  && copy_to_user((void *)a->arg[4], &src_port, sizeof(src_port)) != EMBK_OK) return -EMBK_EFAULT;
    return n;
}

/* --- The table: index = syscall number --- */

#include "include/syscall_nr.h"



/* --- sound -----------------------------------------------------------------
 * Every one of these is refused without EMBK_CAP_AUDIO. The capability is not
 * a formality here: a program that can write to the speaker can be heard by
 * whoever is in the room, and it is the second device class (after the GPU)
 * where "can this process do it" is a question with a physical answer.
 *
 * The device has ONE owner and the kernel remembers which pid it is, so a
 * program cannot write into another's stream by guessing, and a process that
 * dies holding the device has it reclaimed (audio_reap_pid). */
static bool audio_permitted(void)
{
    return (process_current_caps() & EMBK_CAP_BIT(EMBK_CAP_AUDIO)) != 0;
}

/* audio_open() -> 0, or -errno. Also the query: arg 0 non-zero asks for the
 * sample rate instead of claiming the device, so a program can size its
 * buffers before it commits to owning the speaker. */
static int64_t sys_audio_open(const struct sysargs *a) {
    if (!audio_permitted()) return -EMBK_EPERM;
    if (a->arg[0] != 0) return (int64_t)audio_sample_rate();

    struct process *proc = current_process_atomic();
    if (!proc) return -EMBK_ENOMEM;   /* no process context: nothing to own the stream */
    return audio_open(proc->pid);
}

/* audio_write(frames, nframes) -> frames ACCEPTED, or -errno.
 * A short result is normal: the ring is full and the caller comes back. */
static int64_t sys_audio_write(const struct sysargs *a) {
    if (!audio_permitted()) return -EMBK_EPERM;

    const int16_t *user = (const int16_t *)a->arg[0];
    uint32_t nframes = (uint32_t)a->arg[1];

    struct process *proc = current_process_atomic();
    if (!proc) return -EMBK_ENOMEM;   /* no process context: nothing to own the stream */
    if (nframes == 0) return 0;

    /* Copy through a kernel staging buffer rather than letting the DMA engine
     * read user memory: the descriptor holds a PHYSICAL address, and a user
     * page can be unmapped or reused between the write and the moment the
     * hardware reaches it. One buffer's worth at a time, which is also the
     * most audio_write can accept per call. */
    static int16_t stage[8192];
    uint32_t cap_frames = (uint32_t)(sizeof stage / sizeof stage[0]) / 2;
    if (nframes > cap_frames) nframes = cap_frames;

    if (copy_from_user(stage, user, (size_t)nframes * 2 * sizeof(int16_t)) != EMBK_OK)
        return -EMBK_EFAULT;

    uint32_t accepted = 0;
    int rc = audio_write(proc->pid, stage, nframes, &accepted);
    if (rc != EMBK_OK) return rc;
    return (int64_t)accepted;
}

/* audio_close() -> 0. arg 0 non-zero asks "has it all been played yet?" instead,
 * which is what a program needs before it exits: closing while the hardware
 * still has buffers cuts the end off every sound. */
static int64_t sys_audio_close(const struct sysargs *a) {
    if (!audio_permitted()) return -EMBK_EPERM;
    struct process *proc = current_process_atomic();
    if (!proc) return -EMBK_ENOMEM;   /* no process context: nothing to own the stream */

    if (a->arg[0] != 0) return audio_drained(proc->pid) ? 1 : 0;
    audio_close(proc->pid);
    return 0;
}

/* --- memory mappings ------------------------------------------------------
 * mmap(len, prot, flags) -> address, or -errno.
 *
 * NO `addr` ARGUMENT AND NO fd. Both are deliberate omissions rather than
 * unimplemented parameters: MAP_FIXED is refused (an address the caller
 * chooses is an address the caller can collide with, and nothing here needs
 * it), and file-backed mappings need a page cache that does not exist yet.
 * Adding them as arguments that are ignored would be worse than not having
 * them -- a caller passing a file descriptor and getting anonymous zeroes is
 * a bug that surfaces a long way from the call.
 *
 * The one prot combination refused is WRITE|EXEC. See mm/vma.h. */
static int64_t sys_mmap(const struct sysargs *a) {
    struct process *p = current_thread ? current_thread->proc : 0;
    if (!p)
        return -EMBK_EPERM;
    return vma_mmap(p, 0, (uint64_t)a->arg[0],
                    (uint32_t)a->arg[1],
                    (uint32_t)a->arg[2] | MAP_ANONYMOUS | MAP_PRIVATE);
}

/* munmap(addr, len) -> 0, or -errno. Unmapping a range that was never mapped
 * is an ERROR, not a no-op: it is far more often a bug than idempotent
 * cleanup, and a silent success there hides a double free of address space. */
static int64_t sys_munmap(const struct sysargs *a) {
    struct process *p = current_thread ? current_thread->proc : 0;
    if (!p)
        return -EMBK_EPERM;
    return vma_munmap(p, (uint64_t)a->arg[0], (uint64_t)a->arg[1]);
}

/* mprotect(addr, len, prot) -> 0, or -errno.
 *
 * The whole range must already be mapped (-ENOMEM otherwise, POSIX's answer),
 * and PROT_WRITE|PROT_EXEC is refused here exactly as it is in mmap. The
 * frames do not move: their contents are the entire reason anyone calls this.
 */
static int64_t sys_mprotect(const struct sysargs *a) {
    struct process *p = current_thread ? current_thread->proc : 0;
    if (!p)
        return -EMBK_EPERM;
    return vma_mprotect(p, (uint64_t)a->arg[0], (uint64_t)a->arg[1],
                        (uint32_t)a->arg[2]);
}

/* intr(cmd, arg) -> see syscall_nr.h.
 *
 * WHY THIS IS NOT sys_cancelled(). Cancellation is sticky and permanent, and
 * that is the property that makes it trustworthy: a process cannot miss one by
 * being between calls. It is therefore useless for "^C, go back to your
 * prompt" -- a shell that routed ^C at itself would take one keystroke and
 * never read a line again. This channel is counted and cleared by the taker,
 * so a process can be interrupted as many times as a human presses the key and
 * keep living. Both exist; neither is a weakened version of the other. */
static int64_t sys_intr(const struct sysargs *a) {
    switch ((int)a->arg[0]) {
    case 0: return (int64_t)process_take_interrupts();
    case 1: process_set_intr_catch(a->arg[1] != 0); return 0;
    default: return -EMBK_EINVAL;
    }
}

static syscall_handler_t syscall_table[] = {
    [SYS_write]   = sys_write,
    [SYS_exit]    = sys_exit,
    [SYS_yield]   = sys_yield_syscall,
    [SYS_open]    = sys_open,
    [SYS_close]   = sys_close,
    [SYS_read]    = sys_read,
    [SYS_lseek]   = sys_lseek,
    [SYS_stat]    = sys_stat,
    [SYS_readdir] = sys_readdir,
    [SYS_spawn]   = sys_spawn,
    [SYS_wait]    = sys_wait,
    [SYS_getpid]  = sys_getpid,
    [SYS_kill]    = sys_kill,
    [SYS_thread_create] = sys_thread_create,
    [SYS_thread_join]   = sys_thread_join,
    [SYS_thread_exit]   = sys_thread_exit,
    [SYS_sbrk]    = sys_sbrk,
    [SYS_fstat]   = sys_fstat,
    [SYS_gettimeofday] = sys_gettimeofday,
    [SYS_surface_create]  = sys_surface_create,
    [SYS_surface_map]     = sys_surface_map,
    [SYS_surface_acquire] = sys_surface_acquire,
    [SYS_surface_commit]  = sys_surface_commit,
    [SYS_surface_release] = sys_surface_release,
    [SYS_surface_destroy] = sys_surface_destroy,
    [SYS_chan_pair]  = sys_chan_pair,
    [SYS_chan_send]  = sys_chan_send,
    [SYS_chan_recv]  = sys_chan_recv,
    [SYS_chan_close] = sys_chan_close,
    [SYS_chan_listen]  = sys_chan_listen,
    [SYS_chan_accept]  = sys_chan_accept,
    [SYS_chan_connect] = sys_chan_connect,
    [SYS_ui_present]   = sys_ui_present,
    [SYS_ui_input]     = sys_ui_input,
    [SYS_ui_present_rect] = sys_ui_present_rect,
    [SYS_key_poll]     = sys_key_poll,
    [SYS_uptime_ms]    = sys_uptime_ms,
    [SYS_key_grab]     = sys_key_grab,
    [SYS_win_create]   = sys_win_create,
    [SYS_win_present]  = sys_win_present,
    [SYS_win_move]     = sys_win_move,
    [SYS_win_destroy]  = sys_win_destroy,
    [SYS_win_create_desktop] = sys_win_create_desktop,
    [SYS_win_input]    = sys_win_input,
    [SYS_screen_size]  = sys_screen_size,
    [SYS_sleep_ms]     = sys_sleep_ms,
    [SYS_proc_alive]   = sys_proc_alive,
    [SYS_win_resize]   = sys_win_resize,
    [SYS_pipe]         = sys_pipe,
    [SYS_handle_close] = sys_handle_close,
    [SYS_fd_install_obj] = sys_fd_install_obj,
    [SYS_fd_avail]     = sys_fd_avail,
    [SYS_unlink]       = sys_unlink,
    [SYS_mkdir]        = sys_mkdir,
    [SYS_proc_list]    = sys_proc_list,
    [SYS_proc_kill]    = sys_proc_kill,
    [SYS_rmdir]        = sys_rmdir,
    [SYS_set_fs_base]  = sys_set_fs_base,
    [SYS_cancel]       = sys_cancel,
    [SYS_cancelled]    = sys_cancelled,
    [SYS_console_interrupt_route] = sys_console_interrupt_route,
    [SYS_rename]       = sys_rename,
    [SYS_ftruncate]    = sys_ftruncate,
    [SYS_chmod]        = sys_chmod,
    [SYS_key_event_poll] = sys_key_event_poll,
    [SYS_key_mods]       = sys_key_mods,
    [SYS_tty_mode]       = sys_tty_mode,
    [SYS_getcaps]        = sys_getcaps,
    [SYS_net_socket]     = sys_net_socket,
    [SYS_net_connect]    = sys_net_connect,
    [SYS_net_resolve]    = sys_net_resolve,
    [SYS_net_bind]       = sys_net_bind,
    [SYS_net_listen]     = sys_net_listen,
    [SYS_net_accept]     = sys_net_accept,
    [SYS_net_sendto]     = sys_net_sendto,
    [SYS_net_recvfrom]   = sys_net_recvfrom,
    [SYS_fcntl]          = sys_fcntl,
    [SYS_fd_poll]        = sys_fd_poll,
    [SYS_clip_set]       = sys_clip_set,
    [SYS_clip_get]       = sys_clip_get,
    [SYS_win_blur_rect]  = sys_win_blur_rect,
    [SYS_win_restore]    = sys_win_restore,
    [SYS_win_minimize]   = sys_win_minimize,
    [SYS_screen_luma]    = sys_screen_luma,
    [SYS_audio_open]     = sys_audio_open,
    [SYS_audio_write]    = sys_audio_write,
    [SYS_audio_close]    = sys_audio_close,
    [SYS_mmap]           = sys_mmap,
    [SYS_mprotect]       = sys_mprotect,
    [SYS_dup]            = sys_dup,
    [SYS_fsync]          = sys_fsync,
    [SYS_intr]           = sys_intr,
    [SYS_munmap]         = sys_munmap,
    [SYS_win_desktop_front] = sys_win_desktop_front,
    [SYS_debug_attach]   = sys_debug_attach,
    [SYS_debug_wait]     = sys_debug_wait,
    [SYS_debug_cont]     = sys_debug_cont,
    [SYS_debug_regs]     = sys_debug_regs,
    [SYS_debug_mem]      = sys_debug_mem,
    [SYS_debug_hwbp]     = sys_debug_hwbp,
    [SYS_debug_detach]   = sys_debug_detach,
};

#define SYSCALL_TABLE_SIZE (sizeof(syscall_table) / sizeof(syscall_handler_t))

/* The dispatch itself. Each architecture's trap entry fills in `a` and calls
 * this; nothing below the entry point knows how the arguments arrived. */
int64_t syscall_invoke(const struct sysargs *a) {
    if (a->nr < SYSCALL_TABLE_SIZE && syscall_table[a->nr])
        return syscall_table[a->nr](a);
    return -EMBK_EINVAL;
}
