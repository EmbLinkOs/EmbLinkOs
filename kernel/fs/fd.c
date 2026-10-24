#include "fs/fd.h"
#include "fs/vfs.h"
#include "fs/namespace.h"   /* ns_check_writable -- read-only binding gate (UP2) */
#include "include/errno.h"
#include "include/kmalloc.h"   /* open file descriptions are heap objects */
#include "mm/vm_object.h"        /* the page cache: one object per file */
#include "include/kprintf.h"
#include "include/kstring.h"
#include "process/process.h"
#include "tty/tty.h"
#include "tty/pty.h"
#include "process/ksync.h"            /* per-process fd-table mutex (fdlock/fdunlock) */
#include "drivers/input/keyboard.h"   /* console fd read: keyboard_getchar_blocking/has_char */
#include "drivers/video/console.h"    /* console fd write: console_putchar */
#include "ipc/pipe.h"                 /* pipe fd backing: pipe_read/write + ref/unref */
#include "net/net.h"                  /* socket fd backing: net_tcp_recv/send/close/abort */
#include "kworker/kworker.h"          /* vnode close_locked: defer obj_put off the lock */
#include "include/types.h"
#include <stdint.h>
/* string funcs (memcpy/memset/memcmp/strcmp) come from include/kstring.h above --
 * NOT <string.h>: the kernel is freestanding, so a hosted libc header must never
 * be relied on (it only resolved because some toolchains ship newlib). */



struct process;
/* struct fd_entry + enum fd_backing live in fd.h (struct process embeds the
 * fds[] table, so the layout must be public). struct fd_ops is fd.c-private --
 * fd.h only forward-declares it since fd_entry just holds a pointer. */

/* Boot-time fd table: used only while current_process is NULL (early boot,
 * before any real process exists -- e.g. `test fd`/`test ring3` selftests
 * and enter_user_mode()'s legacy path, all of which run at that point).
 * Once a real process exists, every fd operation uses ITS OWN
 * current_process->fds table instead -- see fd_table() below. This keeps
 * every pre-existing caller working unchanged while giving real processes
 * genuine per-process fd isolation (docs/architecture/process-and-
 * scheduling.md's "no per-process fd table" gap, now closed). */
static struct fd_entry g_boot_fds[FD_MAX_OPEN];

/* The fd table THIS call should operate on: the calling process's own table
 * if one exists, else the shared boot-time table. */
static struct fd_entry *fd_table(void)
{
    return current_thread ? current_process->fds : g_boot_fds;
}

/* Guard a process's fd table across a mutation. Keyed on the process, not on
 * `current`, because the install paths (fd_open_into / fd_install_pipe) mutate a
 * TARGET process's table, which may or may not be self.
 *
 * NULL (== the boot context, which uses g_boot_fds) is a no-op: pre-scheduler is
 * single-threaded, so g_boot_fds has no concurrent writer -- the same
 * by-construction argument the mutex itself rests on. This mirrors fd_table()'s
 * own current_thread check exactly, so lock scope and table selection never
 * disagree.
 *
 * ⚠️ NEVER call an ops->close or obj_get while HOLDING this if that callee takes
 * g_sched_lock (pipe_fd_close does): the mutex is built on g_sched_lock, and
 * though mutex_lock releases it before returning, a close run *inside* the
 * critical section would still be re-entering fd state under a held table.
 * The rule here is simpler and stricter -- do blocking teardown OUTSIDE the
 * lock, on a snapshot. Every site below follows it. */
static inline void fdlock(struct process *p)   { if (p) mutex_lock(&p->fd_lock); }
static inline void fdunlock(struct process *p) { if (p) mutex_unlock(&p->fd_lock); }

/* One status/socket identity per open description, not per descriptor number.
 * References include unpublished spawn copies and in-flight I/O loans. Backing
 * endpoints retain their existing lifetime rules; sockets close only when this
 * last shared reference goes. CLOEXEC deliberately stays in fd_entry. */
struct fd_shared {
    int refs;
    int flags;
    int conn;                   /* TCP TCB or UDP socket index */
    uint16_t bind_port;
    struct mutex io_lock;       /* serializes the regular-file cursor */
};
static unsigned shared_live;
static struct fd_shared *shared_new(int flags) {
    struct fd_shared *s=kmalloc(sizeof *s);
    if (!s) return NULL;
    memset(s,0,sizeof *s);
    s->refs=1;s->flags=flags&(O_ACCMODE|O_APPEND|O_NONBLOCK);s->conn=-1;
    mutex_init(&s->io_lock);
    __atomic_fetch_add(&shared_live,1,__ATOMIC_RELAXED);
    return s;
}
unsigned fd_shared_live_count(void) { return __atomic_load_n(&shared_live,__ATOMIC_RELAXED); }
int fd_entry_flags(const struct fd_entry *e) {
    struct fd_shared *state=e->shared;
    return state?__atomic_load_n(&state->flags,__ATOMIC_ACQUIRE):0;
}
static int shared_copy(struct fd_entry *dst,const struct fd_entry *src) {
    if (!src->shared) return -EMBK_ENOMEM;
    __atomic_fetch_add(&src->shared->refs,1,__ATOMIC_RELAXED);
    *dst=*src;return 0;
}
static bool shared_drop_locked(struct fd_entry *e) {
    struct fd_shared *s=e->shared;
    if (!s || __atomic_sub_fetch(&s->refs,1,__ATOMIC_ACQ_REL)) return false;
    __atomic_fetch_sub(&shared_live,1,__ATOMIC_RELAXED);kfree(s);return true;
}

static bool shared_drop(struct fd_entry *e) {
    /* Process FD diagnostics read shared flags under the scheduler lock. */
    sched_lock();bool last=shared_drop_locked(e);sched_unlock();return last;
}

/* --- open file descriptions (see struct open_file in fd.h) -----------------
 *
 * The refcount is atomic rather than fd-table-locked because the descriptors
 * sharing one description can live in DIFFERENT processes -- a spawn-inherited
 * fd is exactly that -- so no single table lock covers them all. Anything else
 * here is accessed through an owned descriptor or I/O loan. The shared
 * io_lock serializes cursor operations across different processes. */
static struct open_file *of_alloc(struct vnode vn) {
    struct open_file *of = kmalloc(sizeof *of);
    if (!of)
        return NULL;
    of->vn = vn;
    of->pos = 0;
    of->refs = 1;
    /* Join the file's page object, creating it if this is the first open. NULL
     * is not a failure -- it means the cache cannot serve this backing, and
     * every path below falls through to the filesystem. */
    of->obj = vmo_get(vn);
    return of;
}

static void of_get(struct open_file *of) {
    if (of) __atomic_fetch_add(&of->refs, 1, __ATOMIC_ACQ_REL);
}

/* Drop one descriptor's reference. True when that was the LAST one, meaning
 * the caller must release the vnode -- which it does differently depending on
 * whether it may block, hence the two close ops. The struct itself is freed
 * here either way. */
static bool of_put(struct open_file *of) {
    if (!of)
        return false;
    return __atomic_sub_fetch(&of->refs, 1, __ATOMIC_ACQ_REL) == 0;
}

void vfs_fd_init(void)
{
    struct fd_entry *fds = fd_table();
    for (int i = 0; i < FD_MAX_OPEN; i++) {
        fds[i].used = false;
        fds[i].cloexec = false;
        fds[i].backing = FD_BACKING_NONE;
        fds[i].ops = NULL;
        fds[i].shared = NULL;
        fds[i].u.file.of = NULL;
    }
}


/* the per-backing dispatch tables (defined below, near their handlers);
 * forward-declared so the open + stdio-init paths can point new fds at them. */
static const struct fd_ops vnode_fd_ops;
static const struct fd_ops console_fd_ops;
static const struct fd_ops nulldev_fd_ops;

/* Map an fd to its live table entry (in the CALLING process's own table). */
static struct fd_entry *fd_lookup(int fd)
{
    if (fd < FD_BASE || fd >= FD_BASE + FD_MAX_OPEN)
        return NULL;

    struct fd_entry *e = &fd_table()[fd - FD_BASE];
    if (!e->used)
        return NULL;

    return e;
}

static int borrow_from(struct process *p,int fd,struct fd_entry *copy) {
    if(fd<0 || fd>=FD_MAX_OPEN || !copy)return -EMBK_EBADF;
    fdlock(p);
    struct fd_entry *e=p?&p->fds[fd]:&g_boot_fds[fd];
    int rc=-EMBK_EBADF;
    if(e->used && e->ops && e->ops->inherit) {
        if(e->backing==FD_BACKING_PTY) {
            rc=shared_copy(copy,e);
            if(!rc){sched_lock();pty_pin_locked(copy->u.pty.p);sched_unlock();}
        } else rc=e->ops->inherit(copy,e);
    }
    fdunlock(p);return rc;
}
int fd_borrow(int fd,struct fd_entry *copy) { return borrow_from(current_thread?current_process:NULL,fd,copy); }
void fd_return(struct fd_entry *copy) {
    if(copy->backing==FD_BACKING_PTY) {
        sched_lock();pty_unpin_locked(copy->u.pty.p);shared_drop_locked(copy);sched_unlock();
    } else if(copy->ops && copy->ops->close)copy->ops->close(copy);
}

/* Split an absolute path into parent vnode + leaf component. */
static int fd_split_parent(const char *path, struct vnode *parent_out,
                           const char **leaf_out, size_t *leaf_len_out)
{
    if (!path || !parent_out || !leaf_out || !leaf_len_out)
        return -EMBK_EINVAL;

    const char *last_slash = NULL;
    for (const char *s = path; *s != '\0'; s++) {
        if (*s == '/')
            last_slash = s;
    }

    if (!last_slash)
        return -EMBK_EINVAL;

    const char *leaf = last_slash + 1;
    size_t leaf_len = 0;
    while (leaf[leaf_len] != '\0') leaf_len++;

    if (leaf_len == 0 || leaf_len > 255)
        return -EMBK_EINVAL;

    char parent_path[256];
    size_t parent_len = (size_t)(last_slash - path);
    if (parent_len == 0) {
        parent_path[0] = '/';
        parent_path[1] = '\0';
    } else {
        if (parent_len >= sizeof(parent_path))
            return -EMBK_ENAMETOOLONG;

        for (size_t i = 0; i < parent_len; i++) {
            parent_path[i] = path[i];
        }
        parent_path[parent_len] = '\0';
    }

    int err = vfs_resolve(parent_path, parent_out);
    if (err)
        return err;

    *leaf_out = leaf;
    *leaf_len_out = leaf_len;
    return EMBK_OK;
}

static int fd_unlink_path(const char *path)
{
    struct vnode parent;
    const char *leaf = NULL;
    size_t leaf_len = 0;

    int rc = fd_split_parent(path, &parent, &leaf, &leaf_len);
    if (rc != EMBK_OK)
        return rc;

    if (!parent.mnt || !parent.mnt->ops || !parent.mnt->ops->unlink)
        return -EMBK_ENOSYS;

    return parent.mnt->ops->unlink(&parent, leaf, leaf_len);
}

/* Public path-level twins, for sys_unlink/sys_mkdir (the shell's rm/mkdir).
 * Same split-parent + per-fs-op dispatch fd_open_into's O_CREAT path uses. */
int vfs_unlink_path(const char *path)
{
    int wok = ns_check_writable(path);   /* refuse in a read-only binding (UP2) */
    if (wok != EMBK_OK)
        return wok;
    return fd_unlink_path(path);
}

int vfs_mkdir_path(const char *path)
{
    int wok = ns_check_writable(path);   /* refuse in a read-only binding (UP2) */
    if (wok != EMBK_OK)
        return wok;

    struct vnode parent;
    const char *leaf = NULL;
    size_t leaf_len = 0;

    int rc = fd_split_parent(path, &parent, &leaf, &leaf_len);
    if (rc != EMBK_OK)
        return rc;

    if (!parent.mnt || !parent.mnt->ops || !parent.mnt->ops->mkdir)
        return -EMBK_ENOSYS;

    struct vnode made;
    return parent.mnt->ops->mkdir(&parent, leaf, leaf_len, &made);
}

/* symlink(target, linkpath) -- create `linkpath` as a link holding the TEXT
 * `target`.
 *
 * The target is NEVER resolved or validated here. A symlink may name something
 * that does not exist, and creating one before its target is the normal case
 * in every build system that uses them -- checking would break the thing they
 * are for. Only the LINK's own parent has to exist and be writable. */
int vfs_symlink_path(const char *target, const char *linkpath)
{
    if (!target || !linkpath || !*target)
        return -EMBK_EINVAL;

    int wok = ns_check_writable(linkpath);   /* refuse in a read-only binding (UP2) */
    if (wok != EMBK_OK)
        return wok;

    struct vnode parent;
    const char *leaf = NULL;
    size_t leaf_len = 0;

    int rc = fd_split_parent(linkpath, &parent, &leaf, &leaf_len);
    if (rc != EMBK_OK)
        return rc;

    if (!parent.mnt || !parent.mnt->ops || !parent.mnt->ops->symlink)
        return -EMBK_ENOSYS;

    return parent.mnt->ops->symlink(&parent, leaf, leaf_len, target);
}

/* readlink(path) -- the TEXT a link holds, without following it.
 *
 * Resolved with follow_final FALSE, which is the whole point: resolving it the
 * ordinary way would land on the link's TARGET and there would be nothing left
 * to read. Reports the full length even when the buffer is short, so a caller
 * can tell truncation from completion. */
/* A hard link: `newpath` becomes a second name for the object `oldpath`
 * names. The target is resolved WITH symlink following -- a hard link is to
 * the thing, never to the pointer -- and must be on the same filesystem as
 * the new name, because a link is a directory entry naming an object id and
 * an id means nothing on another volume (EXDEV, the same answer rename gives
 * across mounts). Directories are refused here, uniformly, before any
 * filesystem gets to decide: two parents is a cycle, and every filesystem
 * that allowed it has regretted it. */
int vfs_link_path(const char *oldpath, const char *newpath)
{
    if (!oldpath || !newpath || !*oldpath || !*newpath)
        return -EMBK_EINVAL;

    int wok = ns_check_writable(newpath);
    if (wok != EMBK_OK)
        return wok;

    struct vnode target;
    int rc = vfs_resolve(oldpath, &target);
    if (rc != EMBK_OK)
        return rc;                        /* ENOENT: nothing to link to */
    if (target.type == VFS_DT_DIR)
        return -EMBK_EPERM;

    struct vnode parent;
    const char *leaf = NULL;
    size_t leaf_len = 0;
    rc = fd_split_parent(newpath, &parent, &leaf, &leaf_len);
    if (rc != EMBK_OK)
        return rc;

    if (parent.mnt != target.mnt)
        return -EMBK_EXDEV;
    if (!parent.mnt || !parent.mnt->ops || !parent.mnt->ops->link)
        return -EMBK_ENOSYS;

    return parent.mnt->ops->link(&parent, leaf, leaf_len, &target);
}

int vfs_readlink_path(const char *path, char *buf, size_t cap, size_t *out_len)
{
    if (!path || !buf || !out_len)
        return -EMBK_EINVAL;

    struct vnode vn;
    int rc = vfs_resolve_nofollow(path, &vn);
    if (rc != EMBK_OK)
        return rc;

    if (vn.type != VFS_DT_LNK)
        return -EMBK_EINVAL;          /* POSIX: EINVAL, not ENOENT -- it exists */
    if (!vn.mnt || !vn.mnt->ops || !vn.mnt->ops->readlink)
        return -EMBK_ENOSYS;

    return vn.mnt->ops->readlink(&vn, buf, cap, out_len);
}

/* lstat -- metadata of the LINK, not of what it points at. The one place a
 * caller can see that a symlink is a symlink. */
int vfs_lstat_path(const char *path, struct vfs_stat *out)
{
    if (!path || !out)
        return -EMBK_EINVAL;

    struct vnode vn;
    int rc = vfs_resolve_nofollow(path, &vn);
    if (rc != EMBK_OK)
        return rc;
    if (!vn.mnt || !vn.mnt->ops || !vn.mnt->ops->stat)
        return -EMBK_ENOSYS;
    return vn.mnt->ops->stat(&vn, out);
}

static int rename_path(const char *old_path, const char *new_path,int noreplace)
{
    struct vnode old_parent, new_parent;
    const char *old_leaf = NULL, *new_leaf = NULL;
    size_t old_len = 0, new_len = 0;

    /* Both endpoints are modified (source removed, dest created) -- both must be
     * in a writable binding (UP2). */
    int wok = ns_check_writable(old_path);
    if (wok != EMBK_OK)
        return wok;
    wok = ns_check_writable(new_path);
    if (wok != EMBK_OK)
        return wok;

    int rc = fd_split_parent(old_path, &old_parent, &old_leaf, &old_len);
    if (rc != EMBK_OK)
        return rc;
    rc = fd_split_parent(new_path, &new_parent, &new_leaf, &new_len);
    if (rc != EMBK_OK)
        return rc;

    /* One mount today, but say it anyway: a cross-mount rename is a copy, not
     * a rename, and must be refused rather than half-done. */
    if (old_parent.mnt != new_parent.mnt)
        return -EMBK_EXDEV;

    if (!old_parent.mnt || !old_parent.mnt->ops || !old_parent.mnt->ops->rename)
        return -EMBK_ENOSYS;

    if(noreplace) {
        if(!old_parent.mnt->ops->rename_noreplace)return -EMBK_ENOTSUP;
        return old_parent.mnt->ops->rename_noreplace(&old_parent,old_leaf,old_len,
                                                    &new_parent,new_leaf,new_len);
    }
    return old_parent.mnt->ops->rename(&old_parent, old_leaf, old_len,
                                       &new_parent, new_leaf, new_len);
}

int vfs_rename_path(const char *old_path,const char *new_path) {return rename_path(old_path,new_path,0);}
int vfs_rename_noreplace(const char *old_path,const char *new_path) {return rename_path(old_path,new_path,1);}

/* ftruncate: fd -> vnode -> the per-fs truncate op that already exists (EMBKFS
 * wires it). Only a VNODE-backed fd can be truncated -- truncating a pipe or
 * the console has no meaning, and EINVAL is what POSIX says for those. */
static int entry_truncate(struct fd_entry *e, uint64_t size)
{
    if((fd_entry_flags(e)&O_ACCMODE)==O_RDONLY)return -EMBK_EBADF;
    if (e->backing != FD_BACKING_VNODE)
        return -EMBK_EINVAL;
    struct open_file *of = e->u.file.of;
    if (!of || !of->vn.mnt || !of->vn.mnt->ops || !of->vn.mnt->ops->truncate)
        return -EMBK_ENOSYS;

    int rc = of->vn.mnt->ops->truncate(&of->vn, size);
    if (rc == EMBK_OK)
        vmo_truncated(of->obj, size);   /* or the cache serves the old tail */
    return rc;
}

int vfs_fd_truncate(int fd, uint64_t size) {
    struct fd_entry held;int rc=fd_borrow(fd,&held);if(rc)return rc;
    rc=entry_truncate(&held,size);fd_return(&held);return rc;
}

/* fsync(fd) -- push this file's dirty pages to the device and do not return
 * until they are there.
 *
 * This used to be vacuously true: every write() went straight to the device,
 * so "your writes are committed" was already a fact and fsync had nothing to
 * do. user/lib/syscalls.c carried the standing obligation that came with that
 * -- "if a write-back cache is ever added, these must become a real device
 * flush the same day". This is that day. */
/* mmap needs the file's pages, and the fd is what names them. A NEW reference
 * is taken here rather than borrowing the descriptor's: a mapping outlives the
 * close() that made it, which is the one behaviour every caller of mmap
 * relies on and the reason the object is refcounted at all. */
static struct vm_object *entry_object(struct fd_entry *e)
{
    if (e->backing != FD_BACKING_VNODE)
        return NULL;
    struct open_file *of = e->u.file.of;
    if (!of || !of->obj)
        return NULL;
    return vmo_get(of->vn);          /* same object, one more holder */
}

struct vm_object *vfs_fd_object(int fd) {
    struct fd_entry held;if(fd_borrow(fd,&held))return NULL;
    struct vm_object *o=entry_object(&held);fd_return(&held);return o;
}

static int entry_fsync(struct fd_entry *e)
{
    /* A pipe, a socket or the console has no device behind it and nothing
     * buffered on the caller's behalf. Succeeding is the true answer, not a
     * convenient one: there is genuinely nothing outstanding. */
    if (e->backing != FD_BACKING_VNODE)
        return EMBK_OK;
    struct open_file *of = e->u.file.of;
    if (!of) return -EMBK_EBADF;
    int rc=of->obj?vmo_flush(of->obj):EMBK_OK;
    if(rc!=EMBK_OK)return rc;
    /* A clean page cache does not imply a durable device. In particular,
     * directories have no cached data: fsync(parent) must still issue the
     * filesystem's barrier after an atomic rename. */
    if(of->vn.mnt && of->vn.mnt->ops && of->vn.mnt->ops->sync)
        return of->vn.mnt->ops->sync(&of->vn);
    return EMBK_OK;
}

int vfs_chmod_path(const char *path, uint32_t mode)
{
    int wok = ns_check_writable(path);   /* metadata write: refuse if read-only (UP2) */
    if (wok != EMBK_OK)
        return wok;
    struct vnode vn;
    int rc = vfs_resolve(path, &vn);
    if (rc != EMBK_OK)
        return rc;
    if (!vn.mnt || !vn.mnt->ops || !vn.mnt->ops->chmod)
        return -EMBK_ENOSYS;
    return vn.mnt->ops->chmod(&vn, mode);
}

int vfs_rmdir_path(const char *path)
{
    int wok = ns_check_writable(path);   /* refuse in a read-only binding (UP2) */
    if (wok != EMBK_OK)
        return wok;

    struct vnode parent;
    const char *leaf = NULL;
    size_t leaf_len = 0;

    int rc = fd_split_parent(path, &parent, &leaf, &leaf_len);
    if (rc != EMBK_OK)
        return rc;

    if (!parent.mnt || !parent.mnt->ops || !parent.mnt->ops->rmdir)
        return -EMBK_ENOSYS;

    return parent.mnt->ops->rmdir(&parent, leaf, leaf_len);
}

int vfs_fd_fsync(int fd) {
    struct fd_entry held;int rc=fd_borrow(fd,&held);if(rc)return rc;
    rc=entry_fsync(&held);fd_return(&held);return rc;
}

static bool fd_readable(int flags)
{
    int acc = flags & O_ACCMODE;
    return (acc == O_RDONLY || acc == O_RDWR);
}

static bool fd_writable(int flags)
{
    int acc = flags & O_ACCMODE;
    return (acc == O_WRONLY || acc == O_RDWR);
}

static int fd_seek_compute(uint64_t base, int64_t delta, uint64_t *out)
{
    if (!out)
        return -EMBK_EINVAL;

    if (delta >= 0) {
        uint64_t d = (uint64_t)delta;
        if (d > UINT64_MAX - base)
            return -EMBK_ERANGE;
        *out = base + d;
        return EMBK_OK;
    }

    uint64_t d = (uint64_t)(-(delta + 1)) + 1;
    if (d > base)
        return -EMBK_EINVAL;
    *out = base - d;
    return EMBK_OK;
}

/* Populate a new process's stdio (fds 0/1/2): inherit each from the spawning
 * parent via the per-backing inherit op, or default to the console when there's
 * no parent / no inheritable entry. Called from process_create() BEFORE file
 * actions, so a spawn redirect can override an inherited slot. */
void fds_init_stdio(struct process *proc) {
    struct process *parent=current_thread?current_process:NULL;
    struct fd_entry *parent_fds = parent?parent->fds:NULL;
    fdlock(parent);
    for (int i = 0; i < FD_STDIO_MAX; i++) {
        struct fd_entry *dst = &proc->fds[i];
        if (parent_fds && parent_fds[i].used && parent_fds[i].ops
             && parent_fds[i].ops->inherit) {
            if (parent_fds[i].ops->inherit(dst, &parent_fds[i]) == EMBK_OK)
                continue;
            /* Inheritance refused (today: any VNODE --see vnode_fd_inherit).
             * Leaves the slot unset; the child gets EBADF on that fd, which
             * is loud and traceable, rather than silently-shared cursor. */
            memset(dst, 0, sizeof(*dst));
            continue;
        }

        /* No parent (the kernel spawned us -- `home`\init, where
         * current_thread is NULL and the boot context's g_boot_fds has no
         * stdio), or the parent's own slot is empty. Defaults to the console,
         * so the very first userland process has working stdio and the
         * inheritance chain has something to propagate. */
        dst->used = true;
        dst->backing = FD_BACKING_CONSOLE;
        dst->ops = &console_fd_ops;
        dst->shared = shared_new((i == 0) ? O_RDONLY : O_WRONLY);
        if (!dst->shared) { memset(dst,0,sizeof *dst);continue; }  /* stdin vs stdout/stderr */
        memset(&dst->u, 0, sizeof(dst->u));
    }
    fdunlock(parent);
}

int vfs_open(const char *path, int flags, uint32_t mode)
{
    int acc = flags & O_ACCMODE;
    if (!path)
        return -EMBK_EINVAL;
    if (acc != O_RDONLY && acc != O_WRONLY && acc != O_RDWR)
        return -EMBK_EINVAL;
    if (flags & ~(O_ACCMODE | O_CREAT | O_EXCL | O_TRUNC | O_APPEND | O_NONBLOCK | O_CLOEXEC))
        return -EMBK_EINVAL;

    /* /dev/null: the one special name (see nulldev_fd_ops above). Checked
     * BEFORE VFS resolution -- there is no /dev directory to resolve through,
     * and creating one on disk to host a fake file would be the dishonest
     * version of this. (fd_open_into, the spawn file-action twin below, does
     * not special-case it yet -- add it there when a spawn wants to silence a
     * child's stdio.) */
    if (strcmp(path, "/dev/null") == 0) {
        struct process *p = current_process;
        struct fd_entry *fds = fd_table();
        int rfd = -EMBK_EMFILE;
        fdlock(p);              /* same find-free-slot race as vfs_open, no I/O here */
        for (int i = 0; i < FD_MAX_OPEN; i++) {
            if (!fds[i].used) {
                struct fd_entry *e = &fds[i];
                memset(e, 0, sizeof(*e));
                e->used = true;
                e->backing = FD_BACKING_NULLDEV;
                e->ops = &nulldev_fd_ops;
                e->shared = shared_new(flags);
                if (!e->shared) { memset(e,0,sizeof *e);rfd=-EMBK_ENOMEM;break; }
    e->cloexec = (flags & O_CLOEXEC) != 0;
                rfd = i + FD_BASE;
                break;
            }
        }
        fdunlock(p);
        return rfd;
    }

    /* Namespace write-gate: an open that can MODIFY the object (write access,
     * create, or truncate) is refused in a read-only binding -- the sealed
     * /system, or any narrowed read-only view (docs/USERSPACE_v2.md UP2). A pure
     * O_RDONLY open is always allowed. No-op in kernel context. */
    if (fd_writable(flags) || (flags & (O_CREAT | O_TRUNC))) {
        int wok = ns_check_writable(path);
        if (wok != EMBK_OK)
            return wok;
    }

    struct vnode vn;
    int err = vfs_resolve(path, &vn);
    if (err == EMBK_OK) {
        if ((flags & O_EXCL) && (flags & O_CREAT))
            return -EMBK_EEXIST;
    } else if (err == -EMBK_ENOENT && (flags & O_CREAT)) {
        struct vnode parent;
        const char *leaf;
        size_t leaf_len;
        err = fd_split_parent(path, &parent, &leaf, &leaf_len);
        if (err)
            return err;
        if (!parent.mnt || !parent.mnt->ops || !parent.mnt->ops->create)
            return -EMBK_ENOSYS;

        err = parent.mnt->ops->create(&parent, leaf, leaf_len, mode, &vn);
        if (err)
            return err;
    } else {
        return err;
    }

    /* O_TRUNC has teeth now (was reserved): shrink to zero through the
     * per-fs truncate op BEFORE the fd is installed. Requires writable
     * access; a filesystem without the op fails LOUD (-ENOSYS) rather than
     * silently keeping stale bytes past the new writer's data -- the
     * save/cp tail-corruption trap this closes. Truncating a just-created
     * empty file is a cheap no-op, so no created-vs-resolved split. */
    if ((flags & O_TRUNC) && vn.type != VFS_DT_DIR) {
        if (!fd_writable(flags))
            return -EMBK_EINVAL;
        if (!vn.mnt || !vn.mnt->ops || !vn.mnt->ops->truncate)
            return -EMBK_ENOSYS;
        err = vn.mnt->ops->truncate(&vn, 0);
        if (err != EMBK_OK)
            return err;
        /* Somebody ELSE may have this file open and cached. The truncate went
         * to the device; the pages have to follow, or that other descriptor
         * keeps reading a file that no longer has those bytes. */
        vmo_file_truncated(vn, 0);
    }

    /* Find a free slot and claim it ATOMICALLY. Before the lock, the find and
     * the fill were separated by obj_get -- so two threads of one process could
     * both see the same slot free, and both fill it: two fds aliasing one entry,
     * and a leaked obj_get. obj_get can do I/O, so the lock has to be a sleeping
     * one (it is), and it is held across obj_get on purpose: obj_get never takes
     * fd_lock, so there is no cycle, and holding it just serialises this one
     * process's concurrent opens -- opens are not a hot path. */
    struct process *p = current_process;
    struct fd_entry *fds = fd_table();
    fdlock(p);
    int fd = -1;
    for (int i = 0; i < FD_MAX_OPEN; i++) {
        if (!fds[i].used) {
            fd = i + FD_BASE;
            break;
        }
    }
    if (fd < 0) { fdunlock(p); return -EMBK_EMFILE; }

    /* Claim the slot BEFORE the (possibly blocking) obj_get, so a concurrent
     * open scanning for a free slot skips it. `used` with backing==NONE is never
     * observable elsewhere: the fd number is not returned to userland until this
     * function succeeds, so no other call can look it up by number. */
    fds[fd - FD_BASE].used = true;

    if (vn.mnt && vn.mnt->ops && vn.mnt->ops->obj_get) {
        err = vn.mnt->ops->obj_get(vn.mnt, vn.ino);
        if (err) {
            fds[fd - FD_BASE].used = false;   /* release the claim on failure */
            fdunlock(p);
            return err;
        }
    }

    struct open_file *of = of_alloc(vn);
    if (!of) {
        fds[fd - FD_BASE].used = false;
        fdunlock(p);
        if (vn.mnt && vn.mnt->ops && vn.mnt->ops->obj_put)
            (void)vn.mnt->ops->obj_put(vn.mnt, vn.ino);
        return -EMBK_ENOMEM;
    }

    fds[fd - FD_BASE].backing = FD_BACKING_VNODE;
    fds[fd - FD_BASE].ops = &vnode_fd_ops;
    fds[fd - FD_BASE].u.file.of = of;
    fds[fd - FD_BASE].shared = shared_new(flags);
    if (!fds[fd - FD_BASE].shared) {
        memset(&fds[fd-FD_BASE],0,sizeof fds[0]);fdunlock(p);
        vmo_put(of->obj);kfree(of);
        if (vn.mnt && vn.mnt->ops && vn.mnt->ops->obj_put)
            (void)vn.mnt->ops->obj_put(vn.mnt,vn.ino);
        return -EMBK_ENOMEM;
    }
    fds[fd - FD_BASE].cloexec = (flags&O_CLOEXEC)!=0;
    fdunlock(p);


    return fd;
}


int fd_read_entry(struct fd_entry *e,void *buf,size_t len,size_t *out) {
    if(!fd_readable(fd_entry_flags(e)))return -EMBK_EBADF;
    if((!buf && len) || !out)return -EMBK_EINVAL;
    if(!e->ops || !e->ops->read)return -EMBK_ENOSYS;
    return e->ops->read(e,buf,len,out);
}
int fd_write_entry(struct fd_entry *e,const void *buf,size_t len,size_t *out) {
    if(!fd_writable(fd_entry_flags(e)))return -EMBK_EBADF;
    if((!buf && len) || !out)return -EMBK_EINVAL;
    if(!e->ops || !e->ops->write)return -EMBK_ENOSYS;
    return e->ops->write(e,buf,len,out);
}
int vfs_fd_read(int fd,void *buf,size_t len,size_t *out) {
    struct fd_entry held;int rc=fd_borrow(fd,&held);if(rc)return rc;
    rc=fd_read_entry(&held,buf,len,out);fd_return(&held);return rc;
}
int vfs_fd_write(int fd,const void *buf,size_t len,size_t *out) {
    struct fd_entry held;int rc=fd_borrow(fd,&held);if(rc)return rc;
    rc=fd_write_entry(&held,buf,len,out);fd_return(&held);return rc;
}
int vfs_fd_seek(int fd,int64_t delta,int whence,uint64_t *out) {
    struct fd_entry held;int rc=fd_borrow(fd,&held);if(rc)return rc;
    rc=!out?-EMBK_EINVAL:(!held.ops->seek?-EMBK_ENOSYS:held.ops->seek(&held,delta,whence,out));
    fd_return(&held);return rc;
}

int vfs_fd_inherit(int fd, struct fd_entry *dst)
{


        struct fd_entry *src = fd_lookup(fd);
    if (!src)
        return -EMBK_EBADF;

    if (!dst)
        return -EMBK_EINVAL;

    if (!src->ops || !src->ops->inherit)
        return -EMBK_ENOSYS;

    return src->ops->inherit(dst, src);
}

/* Explicit descriptor transfer at spawn. No ambient descriptor inheritance
 * beyond stdio; the source is resolved in the spawning process's own table. */
int fd_inherit_into(struct process *target,int target_fd,int source_fd) {
    if (!current_thread || !target || target==current_process) return -EMBK_EINVAL;
    if (target_fd<0 || target_fd>=FD_MAX_OPEN) return -EMBK_EBADF;
    struct fd_entry copy;
    fdlock(current_process);
    int rc=vfs_fd_inherit(source_fd,&copy);
    fdunlock(current_process);
    if (rc) return rc;
    fdlock(target);
    struct fd_entry old=target->fds[target_fd];
    target->fds[target_fd]=copy;
    fdunlock(target);
    if (old.used && old.ops && old.ops->close) old.ops->close(&old);
    return 0;
}

int vfs_close(int fd)
{
    if (fd < FD_BASE || fd >= FD_BASE + FD_MAX_OPEN)
        return -EMBK_EBADF;

    struct process *p = current_process;

    /* Free the slot UNDER the lock, then tear down OUTSIDE it, working from a
     * snapshot. Two reasons this order is mandatory, not stylistic:
     *   1. ops->close can take g_sched_lock (pipe_fd_close does), and the fd
     *      mutex is itself built on g_sched_lock -- so close must not run while
     *      the table is logically held.
     *   2. It closes the double-close / close-races-open window: the first
     *      closer flips `used` false under the lock, so a racing second close
     *      re-checks and sees EBADF, and a racing open can immediately reuse the
     *      freed slot. A snapshot is safe because no close op depends on the
     *      entry's ADDRESS -- they all read e->u.* by value. */
    fdlock(p);
    struct fd_entry *e = &fd_table()[fd - FD_BASE];
    if (!e->used)                    { fdunlock(p); return -EMBK_EBADF; }
    if (!e->ops || !e->ops->close)   { fdunlock(p); return -EMBK_ENOSYS; }
    struct fd_entry snap = *e;       /* by-value copy for teardown */
    memset(e, 0, sizeof(*e));        /* slot is free the instant we unlock */
    fdunlock(p);

    snap.ops->close(&snap);
    return EMBK_OK;
}

/* dup / dup2 / fcntl(F_DUPFD) -- see fd.h.
 *
 * ONE open file description, TWO descriptors. Not a copy: the cursor is
 * shared, so a write through either advances both, and the vnode reference is
 * released once when the last descriptor closes. Anything else would be the
 * "accidental third thing" vnode_fd_inherit() spent a paragraph refusing to
 * be, and the whole reason this needed a real object rather than a struct
 * assignment.
 *
 * dup2(fd, fd) is a NO-OP that returns fd, which is not an arbitrary special
 * case: closing the target first and then duplicating would destroy the very
 * description being duplicated. POSIX calls it out for the same reason. */
int vfs_fd_dup(int oldfd, int newfd, int min_fd) {
    if (oldfd < FD_BASE || oldfd >= FD_BASE + FD_MAX_OPEN)
        return -EMBK_EBADF;
    if (newfd >= FD_BASE + FD_MAX_OPEN || min_fd < 0 || min_fd >= FD_MAX_OPEN)
        return -EMBK_EBADF;

    struct process *p = current_process;

    fdlock(p);
    struct fd_entry *fds = fd_table();
    struct fd_entry *src = &fds[oldfd - FD_BASE];
    if (!src->used || !src->ops) { fdunlock(p); return -EMBK_EBADF; }

    if (newfd >= 0 && newfd == oldfd) { fdunlock(p); return oldfd; }

    /* A backing that cannot say what inheriting means must not be guessed at:
     * "inherit" is a struct copy for a stateless singleton and a refcount bump
     * for anything owned, and picking wrong is a silent use-after-free. */
    if (!src->ops->inherit) { fdunlock(p); return -EMBK_ENOSYS; }

    int target = newfd;
    if (target < 0) {
        target = -1;
        for (int i = min_fd; i < FD_MAX_OPEN; i++)
            if (!fds[i].used) { target = i + FD_BASE; break; }
        if (target < 0) { fdunlock(p); return -EMBK_EMFILE; }
    }

    struct fd_entry *dst = &fds[target - FD_BASE];

    /* Whatever the target held is closed -- that IS dup2, and it is how every
     * shell redirect works. Snapshot then clear under the lock, tear down
     * after: the same ordering rule vfs_close() states, for the same reason
     * (a close op may take g_sched_lock, which this mutex is built on). */
    struct fd_entry snap;
    bool had = dst->used && dst->ops && dst->ops->close;
    if (had) snap = *dst;
    memset(dst, 0, sizeof(*dst));

    int rc = src->ops->inherit(dst, src);
    if (rc != EMBK_OK) {
        memset(dst, 0, sizeof(*dst));
        fdunlock(p);
        if (had) snap.ops->close(&snap);   /* it is gone either way */
        return rc;
    }
    dst->used = true;
    dst->cloexec = false;
    fdunlock(p);

    if (had) snap.ops->close(&snap);
    return target;
}

int64_t vfs_fd_avail(int fd) {
    struct fd_entry held;int rc=fd_borrow(fd,&held);if(rc)return rc;
    int64_t result=held.ops->avail?held.ops->avail(&held):-EMBK_ENOSYS;
    fd_return(&held);return result;
}
int vfs_fd_poll(int fd,int events) {
    struct fd_entry held;int rc=fd_borrow(fd,&held);if(rc)return POLLNVAL;
    int result=held.ops->poll?held.ops->poll(&held,events):(events&(POLLIN|POLLOUT));
    fd_return(&held);return result;
}

int vfs_fd_get_flags(int fd) {
    struct process *p=current_process;fdlock(p);struct fd_entry *e=fd_lookup(fd);
    int rc=e?fd_entry_flags(e):-EMBK_EBADF;fdunlock(p);return rc;
}

int vfs_fd_set_nonblock(int fd, bool on) {
    struct process *p=current_process;fdlock(p);
    struct fd_entry *e=fd_lookup(fd);
    if (e && e->shared) {
        if (on) __atomic_fetch_or(&e->shared->flags,O_NONBLOCK,__ATOMIC_ACQ_REL);
        else __atomic_fetch_and(&e->shared->flags,~O_NONBLOCK,__ATOMIC_ACQ_REL);
    }
    int rc=e && e->shared?0:-EMBK_EBADF;fdunlock(p);return rc;
}
int vfs_fd_set_status(int fd,int flags) {
    struct process *p=current_process;fdlock(p);
    struct fd_entry *e=fd_lookup(fd);
    if (e && e->shared) {
        int old=fd_entry_flags(e);
        __atomic_store_n(&e->shared->flags,(old&O_ACCMODE)|(flags&(O_NONBLOCK|O_APPEND)),__ATOMIC_RELEASE);
    }
    int rc=e && e->shared?0:-EMBK_EBADF;fdunlock(p);return rc;
}

int vfs_fd_fstat(int fd,struct vfs_stat *out) {
    struct fd_entry held;int rc=fd_borrow(fd,&held);if(rc)return rc;
    rc=!out?-EMBK_EINVAL:(!held.ops->fstat?-EMBK_ENOSYS:held.ops->fstat(&held,out));
    fd_return(&held);return rc;
}

/* Parallel to vfs_open(), but for an explicit, not-yet-running
 * target process. placing the result at a SPECIFIC fd rather than the 
 * lowest free one. Shares vfs_open()'s underlying logic. only the
 * table and placement logic differ from vfs_open(). 
 *
 * No "close what's already there" step: target->fds[] is guaranteed entirely
 * unused here - process_alloc() never populates it, and spawn() POSIX
 * addopen running against a fork()'d tables that might already have entries. */
 int fd_open_into(struct process *target, int target_fd, const char *path, int flags, uint32_t mode){
    /* A spawn action opens a new object on the parent's authority, just like
     * sys_open. It must not bypass the filesystem class gate. The parent may
     * still explicitly transfer an already-held descriptor to a weaker child. */
    if(current_thread && !(current_process->cap_set&EMBK_CAP_BIT(EMBK_CAP_FILESYSTEM)))
        return -EMBK_EPERM;

    if (!target || !path)
        return -EMBK_EINVAL;
    if (target_fd < FD_BASE || target_fd >= FD_BASE + FD_MAX_OPEN)
        return -EMBK_EINVAL;

    int acc = flags & O_ACCMODE;
    if (acc != O_RDONLY && acc != O_WRONLY && acc != O_RDWR)
        return -EMBK_EINVAL;
    if (flags & ~(O_ACCMODE | O_CREAT | O_EXCL | O_TRUNC | O_APPEND | O_NONBLOCK | O_CLOEXEC))
        return -EMBK_EINVAL;

    if(strcmp(path,"/dev/null")==0) {
        struct fd_entry entry={0};entry.used=true;entry.backing=FD_BACKING_NULLDEV;
        entry.ops=&nulldev_fd_ops;entry.shared=shared_new(flags);
        if(!entry.shared)return -EMBK_ENOMEM;
        entry.cloexec=(flags&O_CLOEXEC)!=0;
        (void)fd_close_into(target,target_fd);target->fds[target_fd]=entry;return target_fd;
    }
    if(fd_writable(flags) || (flags&(O_CREAT|O_TRUNC))) {
        int rc=ns_check_writable(path);if(rc)return rc;
    }
    struct vnode vn;
    int err = vfs_resolve(path, &vn);
    if (err == EMBK_OK) {
        if ((flags & O_EXCL) && (flags & O_CREAT))
            return -EMBK_EEXIST;
    } else if (err == -EMBK_ENOENT && (flags & O_CREAT)) {
        struct vnode parent;
        const char *leaf;
        size_t leaf_len;
        err = fd_split_parent(path, &parent, &leaf, &leaf_len);
        if (err)
            return err;
        if (!parent.mnt || !parent.mnt->ops || !parent.mnt->ops->create)
            return -EMBK_ENOSYS;

        err = parent.mnt->ops->create(&parent, leaf, leaf_len, mode, &vn);
        if (err)
            return err;
    } else {
        return err;
    }

    /* Same O_TRUNC hook as vfs_open() -- this is the spawn-file-action /
     * redirect path, where a shell's `> file` shape lands. */
    if ((flags & O_TRUNC) && vn.type != VFS_DT_DIR) {
        if (!fd_writable(flags))
            return -EMBK_EINVAL;
        if (!vn.mnt || !vn.mnt->ops || !vn.mnt->ops->truncate)
            return -EMBK_ENOSYS;
        err = vn.mnt->ops->truncate(&vn, 0);
        if (err != EMBK_OK)
            return err;
        /* Somebody ELSE may have this file open and cached. The truncate went
         * to the device; the pages have to follow, or that other descriptor
         * keeps reading a file that no longer has those bytes. */
        vmo_file_truncated(vn, 0);
    }

    /* This new open-file description owns one vnode reference, just as a
     * normal open does. Dup/inherit only bump the description's own count. */
    if(vn.mnt && vn.mnt->ops && vn.mnt->ops->obj_get) {
        err=vn.mnt->ops->obj_get(vn.mnt,vn.ino);if(err)return err;
    }

    /* Lock the TARGET's table, not current's. At spawn time target is the child,
     * not yet running, so this is uncontended -- but it makes the redirect atomic
     * for the dup2-into-self case, and keeps every fds[] mutation under one rule.
     * ops->close inside is fine: it takes g_sched_lock fresh, not nested under
     * this mutex (mutex_lock released g_sched_lock before returning). */
    fdlock(target);
    struct fd_entry *e = &target->fds[target_fd - FD_BASE];
    if (e->used && e->ops && e->ops->close)
        e->ops->close(e);

    memset(e, 0, sizeof(*e));

    struct open_file *of = of_alloc(vn);
    if (!of) {
        fdunlock(target);
        if (vn.mnt && vn.mnt->ops && vn.mnt->ops->obj_put)
            (void)vn.mnt->ops->obj_put(vn.mnt, vn.ino);
        return -EMBK_ENOMEM;
    }

    e->used = true;
    e->backing = FD_BACKING_VNODE;
    e->ops = &vnode_fd_ops;
    e->u.file.of = of;
    e->shared = shared_new(flags);
    if (!e->shared) {
        memset(e,0,sizeof *e);fdunlock(target);vmo_put(of->obj);kfree(of);
        if (vn.mnt && vn.mnt->ops && vn.mnt->ops->obj_put)
            (void)vn.mnt->ops->obj_put(vn.mnt,vn.ino);
        return -EMBK_ENOMEM;
    }
    e->cloexec = (flags & O_CLOEXEC) != 0;

    fdunlock(target);

    return target_fd;
 }

/* Metadata from the filesystem, with the SIZE corrected to what a reader can
 * actually see.
 *
 * A write that has only reached the cache has already changed the file: any
 * descriptor onto it reads the new bytes back immediately. Reporting the
 * device's idea of the length would make stat() disagree with read(), and the
 * failure is not subtle -- every program that stats a file and then reads that
 * many bytes (which is most of them) would get a truncated copy of anything
 * recently written. */
static int vnode_stat_cached(struct open_file *of, struct vfs_stat *out) {
    int err = of->vn.mnt->ops->stat(&of->vn, out);
    if (err)
        return err;
    if (of->obj) {
        uint64_t cached = vmo_size(of->obj);
        if (cached > out->size)
            out->size = cached;
    }
    return EMBK_OK;
}

static int vnode_fd_read(struct fd_entry *e,void *buf,size_t len,size_t *out) {
    if (!fd_readable(fd_entry_flags(e)))return -EMBK_EBADF;
    struct open_file *of=e->u.file.of;
    if (!of || !of->vn.mnt || !of->vn.mnt->ops || !of->vn.mnt->ops->read)return -EMBK_ENOSYS;
    mutex_lock(&e->shared->io_lock);
    uint64_t done=0;size_t bytes=0;int rc;
    if(of->obj){rc=vmo_read(of->obj,of->pos,buf,len,&done);bytes=(size_t)done;}
    else rc=of->vn.mnt->ops->read(&of->vn,of->pos,buf,len,&bytes);
    if(!rc){of->pos+=bytes;*out=bytes;}
    mutex_unlock(&e->shared->io_lock);return rc;
}
static int vnode_fd_write(struct fd_entry *e,const void *buf,size_t len,size_t *out) {
    if (!fd_writable(fd_entry_flags(e)))return -EMBK_EBADF;
    struct open_file *of=e->u.file.of;
    if (!of || !of->vn.mnt || !of->vn.mnt->ops || !of->vn.mnt->ops->write)return -EMBK_ENOSYS;
    if(!len){*out=0;return 0;}
    mutex_lock(&e->shared->io_lock);
    uint64_t start=of->pos,done=0;size_t bytes=0;int rc;
    bool append=(fd_entry_flags(e)&O_APPEND)!=0;
    if(of->obj){
        rc=append?vmo_append(of->obj,buf,len,&start,&done):vmo_write(of->obj,start,buf,len,&done);
        bytes=(size_t)done;
    } else if(append && current_thread) {
        /* An uncached filesystem has no atomic append primitive. Refuse rather
         * than race a stat+write against another open description. */
        rc=-EMBK_ENOSYS;
    } else {
        struct vfs_stat st;
        rc=append?of->vn.mnt->ops->stat(&of->vn,&st):0;
        if(append && !rc)start=st.size;
        if(!rc && len>UINT64_MAX-start)rc=-EMBK_ERANGE;
        if(!rc)rc=of->vn.mnt->ops->write(&of->vn,start,buf,len,&bytes);
    }
    if(!rc){of->pos=start+bytes;*out=bytes;}
    mutex_unlock(&e->shared->io_lock);return rc;
}
static int vnode_fd_seek(struct fd_entry *e,int64_t delta,int whence,uint64_t *out) {
    struct open_file *of=e->u.file.of;
    if(!of || !of->vn.mnt || !of->vn.mnt->ops || !of->vn.mnt->ops->stat)return -EMBK_ENOSYS;
    if(whence<0 || whence>2)return -EMBK_EINVAL;
    mutex_lock(&e->shared->io_lock);
    struct vfs_stat st;int rc=0;uint64_t base=0,pos;
    if(whence==1)base=of->pos;
    if(whence==2){rc=vnode_stat_cached(of,&st);if(!rc)base=st.size;}
    if(!rc)rc=fd_seek_compute(base,delta,&pos);
    if(!rc){of->pos=pos;*out=pos;}
    mutex_unlock(&e->shared->io_lock);return rc;
}

static int vnode_fd_fstat(struct fd_entry *e, struct vfs_stat *out) {
    struct open_file *of = e->u.file.of;
    if (!of || !of->vn.mnt || !of->vn.mnt->ops || !of->vn.mnt->ops->stat)
        return -EMBK_ENOSYS;

    return vnode_stat_cached(of, out);
}


static int vnode_fd_inherit(struct fd_entry *dst, const struct fd_entry *src) {
    /* This used to be an outright -ENOSYS, and the comment it carried said
     * why: "obj_get + struct copy" would have fixed the LIFETIME bug while
     * leaving the CURSOR bug -- the child getting an INDEPENDENT offset onto
     * the same object, which is neither POSIX nor a fresh open but an
     * accidental third thing that corrupts silently. It also said when to fix
     * it properly: "when something eventually redirects a child's stdout to a
     * FILE". dup2() is that, so the shared open file description now exists
     * and this is a reference bump onto it -- one cursor, two descriptors,
     * exactly as POSIX describes.
     *
     * The vnode's own reference is NOT bumped here: it belongs to the
     * description, which is what is being shared, and is released once when
     * the last descriptor onto it closes. */
    int rc=shared_copy(dst,src);if(rc)return rc;
    of_get(dst->u.file.of);
    return EMBK_OK;
}


static void vnode_fd_close(struct fd_entry *e) {
    struct open_file *of = e->u.file.of;
    shared_drop(e);
    if (!of_put(of))
        return;                 /* another descriptor still holds it */
    /* Before the vnode reference goes: the last descriptor onto a file is the
     * last chance to write its dirty pages while the object is still nameable.
     * vmo_put flushes if this was the final reference. */
    vmo_put(of->obj);
    if (of->vn.mnt && of->vn.mnt->ops && of->vn.mnt->ops->obj_put)
        (void)of->vn.mnt->ops->obj_put(of->vn.mnt, of->vn.ino);
    kfree(of);
}

static void vnode_fd_close_locked(struct fd_entry *e) {
    /* Cannot obj_put here: last-close reads the on-disk link count and may
     * destroy the object (block reclamation + metadata writes) -- real disk
     * I/O, disqualifying under g_sched_lock. Defer to the kworker, which
     * obj_puts from a normal schedulable thread holding nothing. This CLOSES
     * the pre-existing exit-time vnode refcount leak. */
    struct open_file *of = e->u.file.of;
    shared_drop_locked(e);
    if (!of_put(of))
        return;
    /* NOT vmo_put() here: it takes a sleeping lock and may write to the disk,
     * and this runs under g_sched_lock -- the same reason the obj_put below is
     * deferred. The object is handed to the kworker with the vnode, which
     * flushes and releases it from a context that is allowed to block. */
    kworker_defer_vmo_put_locked(of->vn, of->obj);
    kfree(of);
}

static const struct fd_ops vnode_fd_ops = {
    .read  = vnode_fd_read,
    .write = vnode_fd_write,
    .seek  = vnode_fd_seek,
    .fstat = vnode_fd_fstat,
    .inherit = vnode_fd_inherit,
    .close = vnode_fd_close,
    .close_locked = vnode_fd_close_locked,
};





static int console_fd_read(struct fd_entry *e, void *buf, size_t len, size_t *out_read) {
    (void)e; 
    return tty_read(buf, len, out_read);  // line discipline
}

/* ONE write() TO THE CONSOLE IS ONE WRITE. Without this a process preempted
 * mid-loop -- typically while the UART's 16-byte FIFO drains -- lets another
 * process's bytes land in the middle of its line. The aarch64 boot test saw
 * `home: desktop ready` arrive as `home: desktop re` + TopBar's lines +
 * `ady`, and reported a working desktop as failed. A sleeping lock, because
 * the loop is long and this is syscall context; kprintf takes its own path
 * to the serial port. It IS serialised against kprintf now, a line at a time --
 * see below. */
static struct mutex g_console_write_lock = MUTEX_INIT;

static int console_fd_write(struct fd_entry *e, const void *buf, size_t len, size_t *out_written) {
    (void)e; 

    const char *cbuf = (const char *)buf;
    /* The mutex keeps two USER writers apart; kprintf's spinlock keeps the
     * KERNEL out of the middle of a line. Both are needed and they nest in
     * this order only -- kprintf never takes the mutex, so there is no cycle.
     *
     * A LINE AT A TIME, not the whole write. A program dumping a megabyte must
     * not own the serial port for the duration of it, and the line is the unit
     * that has to arrive whole: a kernel line inside a user line is what made
     * the aarch64 acceptance test report `init never spawned the session` on a
     * boot where it had (the log read `init: desktop seEMBKFS: sda: ...`). */
    mutex_lock(&g_console_write_lock);
    size_t i = 0;
    while (i < len) {
        size_t end = i;
        while (end < len && cbuf[end] != '\n') end++;
        if (end < len) end++;                    /* take the newline with it */
        kprintf_line_begin();
        for (size_t k = i; k < end; k++) console_putchar(cbuf[k]);
        kprintf_line_end();
        i = end;
    }
    mutex_unlock(&g_console_write_lock);
    *out_written = len;
    return EMBK_OK;
}

static int console_fd_seek(struct fd_entry *e, int64_t delta, int whence, uint64_t *out_offset) {
    (void)e; (void)delta; (void)whence; (void)out_offset;
    return -EMBK_ESPIPE; /* errno.h literally anticipates this : "illegal seek" */
}

static int console_fd_fstat(struct fd_entry *e, struct vfs_stat *out) {
    (void)e; 
    
    /* A character device. Incidentally this is what finally  makes newlib's 
     * _isatty/_fstat stubs HONEST -- they currently claim chardev with no
     * object backing the claim. */

    out->size = 0; /* size is meaningless for a console */
    out->type = VFS_DT_CHAR;
    return EMBK_OK;
}

static int console_fd_inherit(struct fd_entry *dst, const struct fd_entry *src) {
    return shared_copy(dst,src);
}
static void console_fd_close(struct fd_entry *e) { shared_drop(e); }
static void status_close_locked(struct fd_entry *e) { shared_drop_locked(e); }

static int64_t console_fd_avail(struct fd_entry *e) {
    (void)e;
    return keyboard_has_char() ? 1 : 0;
}

static const struct fd_ops console_fd_ops = {
    .read = console_fd_read,
    .write = console_fd_write,
    .seek = console_fd_seek,
    .fstat = console_fd_fstat,
    .inherit = console_fd_inherit,
    .close = console_fd_close,
    .close_locked = status_close_locked,
    .avail = console_fd_avail,
};

/* -------------------------------------------------------------------------
 * /dev/null -- a REAL null device, not a shim. EmbLink has no /dev tree; this
 * one name is special-cased in vfs_open() because a discard-sink/EOF-source is
 * a genuine OS primitive that portable software assumes exists (git's
 * sanitize_stdfds() opens it before doing anything at all; shell redirects
 * want it too). Stateless, like the console backing: reads are instant EOF,
 * writes vanish and report success -- which here is the TRUTH of the object,
 * not a stub pretending. FD_BACKING_NULLDEV, so nothing mistakes it for a file.
 * ------------------------------------------------------------------------- */
static int nulldev_fd_read(struct fd_entry *e, void *buf, size_t len, size_t *out) {
    (void)e; (void)buf; (void)len;
    *out = 0;                      /* always EOF */
    return EMBK_OK;
}
static int nulldev_fd_write(struct fd_entry *e, const void *buf, size_t len, size_t *out) {
    (void)e; (void)buf;
    *out = len;                    /* swallowed, by definition */
    return EMBK_OK;
}
static int nulldev_fd_seek(struct fd_entry *e, int64_t delta, int whence, uint64_t *out_offset) {
    (void)e; (void)delta; (void)whence;
    *out_offset = 0;
    return EMBK_OK;
}
static int nulldev_fd_fstat(struct fd_entry *e, struct vfs_stat *out) {
    (void)e;
    out->size = 0;
    out->type = VFS_DT_CHAR;
    return EMBK_OK;
}
static int nulldev_fd_inherit(struct fd_entry *dst, const struct fd_entry *src) { return shared_copy(dst,src); }
static void nulldev_fd_close(struct fd_entry *e) { shared_drop(e); }
static int64_t nulldev_fd_avail(struct fd_entry *e) { (void)e; return 0; }

static const struct fd_ops nulldev_fd_ops = {
    .read = nulldev_fd_read,
    .write = nulldev_fd_write,
    .seek = nulldev_fd_seek,
    .fstat = nulldev_fd_fstat,
    .inherit = nulldev_fd_inherit,
    .close = nulldev_fd_close,
    .close_locked = status_close_locked,
    .avail = nulldev_fd_avail,
};

static int pipe_fd_read(struct fd_entry *e, void *buf, size_t len, size_t *out) {
    if (e->u.pipe.side != 0) return -EMBK_EBADF;    /* direction enforced at the
                                                      * op, per the one-kind design */
    return pipe_read_flags(e->u.pipe.p,buf,len,out,(fd_entry_flags(e)&O_NONBLOCK)!=0);
}
static int pipe_fd_write(struct fd_entry *e, const void *buf, size_t len, size_t *out) {
    if (e->u.pipe.side != 1) return -EMBK_EBADF;
    return pipe_write_flags(e->u.pipe.p,buf,len,out,(fd_entry_flags(e)&O_NONBLOCK)!=0,true);
}
static int pipe_fd_seek(struct fd_entry *e, int64_t d, int w, uint64_t *out) {
    (void)e;(void)d;(void)w;(void)out; return -EMBK_ESPIPE;
}
static int pipe_fd_fstat(struct fd_entry *e, struct vfs_stat *out) {
    (void)e; out->size = 0; out->type = VFS_DT_FIFO; return EMBK_OK;
    /* FIFO keeps isatty(fd) correctly FALSE when stdio is a pipe -- the
     * mirror-image favor of the console's VFS_DT_CHAR making it true */
}
static int pipe_fd_inherit(struct fd_entry *dst, const struct fd_entry *src) {
    int rc=shared_copy(dst,src);if(rc)return rc;
    sched_lock();
    pipe_ref_locked(dst->u.pipe.p, dst->u.pipe.side);   /* a copy IS a new
                                                          * reference -- the exact
                                                          * under-ref trap the
                                                          * blanket-memcpy had */
    sched_unlock();
    return EMBK_OK;
}
static void pipe_fd_close(struct fd_entry *e) {
    sched_lock(); pipe_unref_locked(e->u.pipe.p, e->u.pipe.side); sched_unlock();shared_drop(e);
}
static void pipe_fd_close_locked(struct fd_entry *e) {
    pipe_unref_locked(e->u.pipe.p, e->u.pipe.side);shared_drop_locked(e);
}

static int64_t pipe_fd_avail(struct fd_entry *e) {
    /* read end: bytes readable; write end: space writable -- both are the
     * same question ("how much moves without blocking?"). */
    return pipe_avail(e->u.pipe.p, e->u.pipe.side);
}

static const struct fd_ops pipe_fd_ops = {
    .read = pipe_fd_read, .write = pipe_fd_write, .seek = pipe_fd_seek,
    .fstat = pipe_fd_fstat, .inherit = pipe_fd_inherit,
    .close = pipe_fd_close, .close_locked = pipe_fd_close_locked,
    .avail = pipe_fd_avail,
};

/* See fd.h. Mirrors fd_open_into's close-then-install redirect idiom: the
 * child's stdio slot was pre-populated by fds_init_stdio (a console, or an
 * inherited entry) -- installing onto it is a REDIRECT, so the old entry is
 * released through its own ops first (console: no-op; an inherited pipe
 * would be properly unref'd rather than leaked). Runs UNLOCKED (spawn's
 * file-actions loop, ordinary syscall context), hence ops->close, and the
 * self-contained sched_lock around the ref bump. */
static int install_pipe(struct process *target, int target_fd, struct pipe *p, int side)
{
    if (!target || !p || (side != 0 && side != 1))
        return -EMBK_EINVAL;
    if (target_fd < -1 || target_fd >= FD_BASE + FD_MAX_OPEN)
        return -EMBK_EBADF;

    fdlock(target);             /* same rule as fd_open_into: target's table, atomic redirect */
    if (target_fd==-1) {
        for(target_fd=FD_BASE;target_fd<FD_BASE+FD_MAX_OPEN;target_fd++)
            if(!target->fds[target_fd-FD_BASE].used)break;
        if(target_fd==FD_BASE+FD_MAX_OPEN){fdunlock(target);return -EMBK_EMFILE;}
    }
    struct fd_entry *e = &target->fds[target_fd - FD_BASE];
    if (e->used && e->ops && e->ops->close)
        e->ops->close(e);
    memset(e, 0, sizeof(*e));

    e->used = true;
    e->backing = FD_BACKING_PIPE;
    e->ops = &pipe_fd_ops;
    e->shared = shared_new(side==0?O_RDONLY:O_WRONLY);
    if (!e->shared) { memset(e,0,sizeof *e);fdunlock(target);return -EMBK_ENOMEM; }
    e->u.pipe.p = p;
    e->u.pipe.side = side;

    sched_lock();
    pipe_ref_locked(p, side);   /* the child is a NEW reference to this end --
                                 * bump before it ever runs, so its close can't
                                 * drop the count below the parent's own hold.
                                 * Fresh g_sched_lock, not nested under fd_lock. */
    sched_unlock();
    fdunlock(target);
    return target_fd;
}


int fd_install_pipe(struct process *target,int fd,struct pipe *p,int side) {
    if(fd<FD_BASE)return -EMBK_EBADF;
    int rc=install_pipe(target,fd,p,side);return rc<0?rc:EMBK_OK;
}
int fd_install_pipe_any(struct process *target,struct pipe *p,int side) {
    return install_pipe(target,-1,p,side);
}

/* ---- FD_BACKING_SOCKET: a TCP connection as a read/write fd (M4) --------- */
static int sock_fd_read(struct fd_entry *e, void *buf, size_t len, size_t *out) {
    if (e->shared->conn < 0) return -EMBK_EINVAL;      /* not connected */
    if (fd_entry_flags(e) & O_NONBLOCK) {                       /* never wait: -EAGAIN if no data */
        int n = net_tcp_recv_nb(e->shared->conn, buf, (uint32_t)len);
        if (n == -2) return -EMBK_EAGAIN;
        if (n < 0)   return -EMBK_EIO;
        *out = (size_t)n;
        return EMBK_OK;
    }
    int n = net_tcp_recv(e->shared->conn, buf, (uint32_t)len);
    /* Timed out with the connection still open. NOT end-of-stream, and not an
     * I/O error either -- the distinction is the whole point (see
     * net_tcp_recv). A blocking reader gets EAGAIN and decides for itself
     * whether it is mid-message and must wait longer. */
    if (n == -2) return -EMBK_EAGAIN;
    if (n < 0) return -EMBK_EIO;
    *out = (size_t)n;                                 /* 0 = peer FIN / EOF */
    return EMBK_OK;
}
/* Readiness for select()/poll: connect-in-progress becomes writable on
 * ESTABLISHED; buffered data / FIN becomes readable. See net_tcp_ready(). */
static int sock_fd_poll(struct fd_entry *e, int events) {
    if (e->shared->conn < 0) return POLLNVAL;          /* unconnected socket */
    int rdy = net_tcp_ready(e->shared->conn);
    if (rdy < 0) return POLLERR;
    int re = 0;
    if ((events & POLLIN)  && (rdy & TCP_RDY_READ))  re |= POLLIN;
    if ((events & POLLOUT) && (rdy & TCP_RDY_WRITE)) re |= POLLOUT;
    if (rdy & TCP_RDY_ERR) re |= POLLERR;
    return re;
}
static int sock_fd_write(struct fd_entry *e, const void *buf, size_t len, size_t *out) {
    if (e->shared->conn < 0) return -EMBK_EINVAL;
    int n = net_tcp_send(e->shared->conn, buf, (uint32_t)len);
    if (n < 0) return -EMBK_EIO;
    *out = (size_t)n;
    return EMBK_OK;
}
static int sock_fd_seek(struct fd_entry *e, int64_t d, int w, uint64_t *out) {
    (void)e;(void)d;(void)w;(void)out; return -EMBK_ESPIPE;
}
static int sock_fd_fstat(struct fd_entry *e, struct vfs_stat *out) {
    (void)e; out->size = 0; out->type = VFS_DT_FIFO;  /* not a tty; isatty(fd) == false */
    return EMBK_OK;
}
static int sock_fd_inherit(struct fd_entry *dst, const struct fd_entry *src) { return shared_copy(dst,src); }
static void sock_fd_close(struct fd_entry *e) {
    int conn=e->shared->conn;
    if (shared_drop(e) && conn>=0) net_tcp_close(conn);
}
static void sock_fd_close_locked(struct fd_entry *e) {
    int conn=e->shared->conn;
    if (shared_drop_locked(e) && conn>=0) net_tcp_abort(conn);
}

static const struct fd_ops sock_fd_ops = {
    .read = sock_fd_read, .write = sock_fd_write, .seek = sock_fd_seek,
    .fstat = sock_fd_fstat, .inherit = sock_fd_inherit,
    .close = sock_fd_close, .close_locked = sock_fd_close_locked,
    .avail = NULL, .poll = sock_fd_poll,
};

static int alloc_socket(struct process *p,int conn) {
    if (!p) return -EMBK_EINVAL;
    struct fd_entry *fds = p->fds;
    fdlock(p);
    int fd = -1;
    for (int i = 0; i < FD_MAX_OPEN; i++)
        if (!fds[i].used) { fd = i + FD_BASE; break; }
    if (fd < 0) { fdunlock(p); return -EMBK_EMFILE; }
    struct fd_entry *e = &fds[fd - FD_BASE];
    memset(e, 0, sizeof(*e));
    e->used = true;
    e->backing = FD_BACKING_SOCKET;
    e->ops = &sock_fd_ops;
    e->shared = shared_new(O_RDWR);
    if (!e->shared) { memset(e,0,sizeof *e);fdunlock(p);return -EMBK_ENOMEM; }
    e->shared->conn = conn;                              /* unconnected until connect() */
    fdunlock(p);
    return fd;
}

int fd_alloc_socket(struct process *p) { return alloc_socket(p,-1); }
int fd_socket_connect(struct process *p,int fd,uint32_t ip,uint16_t port) {
    struct fd_entry held;int rc=borrow_from(p,fd,&held);if(rc)return rc;
    if(held.backing!=FD_BACKING_SOCKET){fd_return(&held);return -EMBK_EBADF;}
    mutex_lock(&held.shared->io_lock);
    if(held.shared->conn>=0)rc=-EMBK_EINVAL;
    else if(fd_entry_flags(&held)&O_NONBLOCK) {
        int conn=net_tcp_connect_start(ip,port);
        if(conn<0)rc=-EMBK_ECONNREFUSED;
        else {__atomic_store_n(&held.shared->conn,conn,__ATOMIC_RELEASE);rc=-EMBK_EINPROGRESS;}
    } else {
        int conn=net_tcp_connect(ip,port);
        if(conn<0)rc=-EMBK_ECONNREFUSED;
        else {__atomic_store_n(&held.shared->conn,conn,__ATOMIC_RELEASE);rc=0;}
    }
    mutex_unlock(&held.shared->io_lock);fd_return(&held);return rc;
}
int fd_socket_bind(struct process *p,int fd,uint16_t port) {
    struct fd_entry held;int rc=borrow_from(p,fd,&held);if(rc)return rc;
    mutex_lock(&held.shared->io_lock);
    if(held.backing==FD_BACKING_SOCKET) {
        if(held.shared->conn>=0)rc=-EMBK_EINVAL;
        else {held.shared->bind_port=port;rc=0;}
    } else if(held.backing==FD_BACKING_UDP)rc=net_udp_bind(held.shared->conn,port)==0?0:-EMBK_EINVAL;
    else rc=-EMBK_EBADF;
    mutex_unlock(&held.shared->io_lock);fd_return(&held);return rc;
}

/* ---- FD_BACKING_UDP: a UDP datagram socket (M5) ------------------------- */
static int udp_fd_read(struct fd_entry *e, void *buf, size_t len, size_t *out) {
    int n = (fd_entry_flags(e)&O_NONBLOCK)?net_udp_recvfrom_nb(e->shared->conn,buf,(uint32_t)len,0,0):net_udp_recvfrom(e->shared->conn,buf,(uint32_t)len,0,0);
    if(n==-2)return -EMBK_EAGAIN;
    if (n < 0) return -EMBK_EIO;
    *out = (size_t)n;
    return EMBK_OK;
}
static int udp_fd_write(struct fd_entry *e, const void *buf, size_t len, size_t *out) {
    (void)e; (void)buf; (void)len; (void)out;
    return -EMBK_EINVAL;                        /* connectionless: use sendto */
}
static int udp_fd_seek(struct fd_entry *e, int64_t d, int w, uint64_t *o) {
    (void)e;(void)d;(void)w;(void)o; return -EMBK_ESPIPE;
}
static int udp_fd_fstat(struct fd_entry *e, struct vfs_stat *o) {
    (void)e; o->size = 0; o->type = VFS_DT_FIFO; return EMBK_OK;
}
static int udp_fd_inherit(struct fd_entry *dst, const struct fd_entry *src) { return shared_copy(dst,src); }
static void udp_fd_close(struct fd_entry *e) { int us=e->shared->conn;if(shared_drop(e))net_udp_close(us); }
static void udp_fd_close_locked(struct fd_entry *e) { int us=e->shared->conn;if(shared_drop_locked(e))net_udp_abort(us); }

static int udp_fd_poll(struct fd_entry *e,int events) {
    int ready=net_udp_ready(e->shared->conn);
    return ready<0?POLLERR:((events&POLLOUT)|(ready?(events&POLLIN):0));
}
static const struct fd_ops udp_fd_ops = {
    .read = udp_fd_read, .write = udp_fd_write, .seek = udp_fd_seek, .fstat = udp_fd_fstat,
    .inherit = udp_fd_inherit, .close = udp_fd_close, .close_locked = udp_fd_close_locked, .avail = NULL,.poll=udp_fd_poll,
};

int fd_alloc_udp(struct process *p) {
    if (!p) return -EMBK_EINVAL;
    int us = net_udp_open();
    if (us < 0) return -EMBK_EMFILE;
    struct fd_entry *fds = p->fds;
    fdlock(p);
    int fd = -1;
    for (int i = 0; i < FD_MAX_OPEN; i++) if (!fds[i].used) { fd = i + FD_BASE; break; }
    if (fd < 0) { fdunlock(p); net_udp_abort(us); return -EMBK_EMFILE; }
    struct fd_entry *e = &fds[fd - FD_BASE];
    memset(e, 0, sizeof(*e));
    e->used = true; e->backing = FD_BACKING_UDP; e->ops = &udp_fd_ops; e->shared=shared_new(O_RDWR);
    if (!e->shared) { memset(e,0,sizeof *e);fdunlock(p);net_udp_abort(us);return -EMBK_ENOMEM; }
    e->shared->conn = us;
    fdunlock(p);
    return fd;
}

/* Kernel-buffer sendto/recvfrom; the syscall bounces user memory in/out. */
int fd_udp_sendto(struct process *p,int fd,uint32_t ip,uint16_t port,const void *data,uint32_t len) {
    struct fd_entry held;int rc=borrow_from(p,fd,&held);if(rc)return rc;
    if(held.backing!=FD_BACKING_UDP)rc=-EMBK_EBADF;
    else {int n=net_udp_sendto(held.shared->conn,ip,port,data,len);rc=n<0?-EMBK_EIO:n;}
    fd_return(&held);return rc;
}
int fd_udp_recvfrom(struct process *p,int fd,void *buf,uint32_t cap,uint32_t *ip,uint16_t *port) {
    struct fd_entry held;int rc=borrow_from(p,fd,&held);if(rc)return rc;
    if(held.backing!=FD_BACKING_UDP)rc=-EMBK_EBADF;
    else {
        int n=(fd_entry_flags(&held)&O_NONBLOCK)?net_udp_recvfrom_nb(held.shared->conn,buf,cap,ip,port):net_udp_recvfrom(held.shared->conn,buf,cap,ip,port);
        rc=n==-2?-EMBK_EAGAIN:(n<0?-EMBK_EIO:n);
    }
    fd_return(&held);return rc;
}
int fd_socket_listen(struct process *p,int fd) {
    struct fd_entry held;int rc=borrow_from(p,fd,&held);if(rc)return rc;
    if(held.backing!=FD_BACKING_SOCKET){fd_return(&held);return -EMBK_EBADF;}
    mutex_lock(&held.shared->io_lock);
    if(held.shared->conn>=0 || !held.shared->bind_port)rc=-EMBK_EINVAL;
    else {
        int conn=net_tcp_listen(held.shared->bind_port);
        if(conn<0)rc=-EMBK_EMFILE;
        else {__atomic_store_n(&held.shared->conn,conn,__ATOMIC_RELEASE);rc=0;}
    }
    mutex_unlock(&held.shared->io_lock);fd_return(&held);return rc;
}
int fd_socket_accept(struct process *p,int fd) {
    struct fd_entry held;int rc=borrow_from(p,fd,&held);if(rc)return rc;
    int child=-1;
    if(held.backing!=FD_BACKING_SOCKET || held.shared->conn<0)rc=-EMBK_EBADF;
    else {
        child=(fd_entry_flags(&held)&O_NONBLOCK)?net_tcp_accept_nb(held.shared->conn):net_tcp_accept(held.shared->conn);
        rc=child==-2?-EMBK_EAGAIN:(child<0?-EMBK_EIO:0);
    }
    fd_return(&held);if(rc)return rc;
    int nfd=alloc_socket(p,child);
    if(nfd<0)net_tcp_abort(child);
    return nfd;
}

int vfs_fd_run_selftests(void)
{
    const char *path = "/fd_selftest.tmp";
    const char *missing = "/fd_selftest_does_not_exist";
    const char payload[] = "fd-selftest";
    char buf[sizeof(payload)] = {0};
    struct vfs_stat st;
    uint64_t off = 0;
    size_t n = 0;

    kprintf("FD: selftest: begin\n");

    int rc = vfs_open(missing, O_RDONLY, 0);
    if (rc != -EMBK_ENOENT) {
        kprintf("FD: selftest fail: open missing -> %s\n", embk_strerror(rc));
        return rc < 0 ? rc : -EMBK_EINVAL;
    }

    int fd = vfs_open(path, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        kprintf("FD: selftest fail: create/open -> %s\n", embk_strerror(fd));
        return fd;
    }

    rc = vfs_fd_write(fd, payload, sizeof(payload) - 1, &n);
    if (rc != EMBK_OK || n != sizeof(payload) - 1) {
        kprintf("FD: selftest fail: write rc=%s n=%u\n", embk_strerror(rc), (unsigned)n);
        vfs_close(fd);
        return (rc != EMBK_OK) ? rc : -EMBK_EIO;
    }

    rc = vfs_fd_seek(fd, 0, 0, &off);
    if (rc != EMBK_OK || off != 0) {
        kprintf("FD: selftest fail: seek set rc=%s off=%lu\n", embk_strerror(rc), off);
        vfs_close(fd);
        return (rc != EMBK_OK) ? rc : -EMBK_EIO;
    }

    rc = vfs_fd_read(fd, buf, sizeof(payload) - 1, &n);
    if (rc != EMBK_OK || n != sizeof(payload) - 1 || memcmp(buf, payload, sizeof(payload) - 1) != 0) {
        kprintf("FD: selftest fail: read/compare rc=%s n=%u\n", embk_strerror(rc), (unsigned)n);
        vfs_close(fd);
        return (rc != EMBK_OK) ? rc : -EMBK_EIO;
    }

    rc = vfs_fd_fstat(fd, &st);
    if (rc != EMBK_OK || st.size < (sizeof(payload) - 1)) {
        kprintf("FD: selftest fail: fstat rc=%s size=%lu\n", embk_strerror(rc), st.size);
        vfs_close(fd);
        return (rc != EMBK_OK) ? rc : -EMBK_EIO;
    }

    rc = vfs_fd_seek(fd, 0, 2, &off);
    if (rc != EMBK_OK || off != st.size) {
        kprintf("FD: selftest fail: seek end rc=%s off=%lu size=%lu\n", embk_strerror(rc), off, st.size);
        vfs_close(fd);
        return (rc != EMBK_OK) ? rc : -EMBK_EIO;
    }

    rc = vfs_close(fd);
    if (rc != EMBK_OK) {
        kprintf("FD: selftest fail: close rc=%s\n", embk_strerror(rc));
        return rc;
    }

    rc = vfs_close(fd);
    if (rc != -EMBK_EBADF) {
        kprintf("FD: selftest fail: close invalid expected EBADF got %s\n", embk_strerror(rc));
        return (rc < 0) ? rc : -EMBK_EINVAL;
    }

    fd = vfs_open(path, O_WRONLY, 0);
    if (fd < 0)
        return fd;
    rc = vfs_fd_read(fd, buf, 1, &n);
    (void)vfs_close(fd);
    if (rc != -EMBK_EBADF) {
        kprintf("FD: selftest fail: read on O_WRONLY expected EBADF got %s\n", embk_strerror(rc));
        return (rc < 0) ? rc : -EMBK_EINVAL;
    }

    fd = vfs_open(path, O_RDONLY, 0);
    if (fd < 0)
        return fd;
    rc = vfs_fd_write(fd, payload, 1, &n);
    (void)vfs_close(fd);
    if (rc != -EMBK_EBADF) {
        kprintf("FD: selftest fail: write on O_RDONLY expected EBADF got %s\n", embk_strerror(rc));
        return (rc < 0) ? rc : -EMBK_EINVAL;
    }

    /* unlink-while-open: the fd is bound to the object, not the name. */
    fd = vfs_open(path, O_RDONLY, 0);
    if (fd < 0)
        return fd;

    rc = fd_unlink_path(path);
    if (rc != EMBK_OK) {
        kprintf("FD: selftest fail: unlink-while-open rc=%s\n", embk_strerror(rc));
        (void)vfs_close(fd);
        return rc;
    }

    int look = vfs_open(path, O_RDONLY, 0);
    if (look != -EMBK_ENOENT) {
        kprintf("FD: selftest fail: name still resolves after unlink -> %s\n", embk_strerror(look));
        if (look >= 0)
            (void)vfs_close(look);
        (void)vfs_close(fd);
        return -EMBK_EINVAL;
    }

    memset(buf, 0, sizeof buf);
    n = 0;
    rc = vfs_fd_seek(fd, 0, 0, &off);
    if (rc == EMBK_OK)
        rc = vfs_fd_read(fd, buf, sizeof(payload) - 1, &n);
    if (rc != EMBK_OK || n != sizeof(payload) - 1 || memcmp(buf, payload, sizeof(payload) - 1) != 0) {
        kprintf("FD: selftest fail: read-after-unlink rc=%s n=%u (object freed early?)\n", embk_strerror(rc), (unsigned)n);
        (void)vfs_close(fd);
        return (rc != EMBK_OK) ? rc : -EMBK_EIO;
    }

    (void)vfs_close(fd);

    rc = fd_unlink_path(path);
    if (rc != EMBK_OK && rc != -EMBK_ENOSYS && rc != -EMBK_ENOENT) {
        kprintf("FD: selftest fail: cleanup unlink rc=%s\n", embk_strerror(rc));
        return rc;
    }

    /* --- the small-batch trio: O_TRUNC, mtime, rmdir ------------------- */

    /* O_TRUNC: write a LONG payload, reopen with O_TRUNC, write a SHORT
     * one, and assert the file is exactly the short one -- the old tail
     * must be gone (the save/cp corruption this feature closes). */
    const char longp[]  = "AAAAAAAAAAAAAAAA";
    const char shortp[] = "BB";
    fd = vfs_open(path, O_CREAT | O_WRONLY, 0644);
    if (fd < 0) { kprintf("FD: selftest fail: trunc setup open\n"); return fd; }
    rc = vfs_fd_write(fd, longp, sizeof(longp) - 1, &n);
    vfs_close(fd);
    if (rc != EMBK_OK) { kprintf("FD: selftest fail: trunc setup write\n"); return rc; }

    fd = vfs_open(path, O_WRONLY | O_TRUNC, 0);
    if (fd < 0) { kprintf("FD: selftest fail: O_TRUNC open -> %s\n", embk_strerror(fd)); return fd; }
    rc = vfs_fd_write(fd, shortp, sizeof(shortp) - 1, &n);
    vfs_close(fd);
    if (rc != EMBK_OK) { kprintf("FD: selftest fail: post-trunc write\n"); return rc; }

    rc = vfs_stat(path, &st);
    if (rc != EMBK_OK || st.size != sizeof(shortp) - 1) {
        kprintf("FD: selftest fail: O_TRUNC left size=%lu (want %u) -- stale tail!\n",
                (unsigned long)st.size, (unsigned)(sizeof(shortp) - 1));
        return rc != EMBK_OK ? rc : -EMBK_EIO;
    }

    /* mtime: a just-written file must carry a nonzero last-modified. */
    if (st.mtime == 0) {
        kprintf("FD: selftest fail: mtime is 0 on a fresh write\n");
        return -EMBK_EIO;
    }
    (void)vfs_unlink_path(path);

    /* rmdir: mkdir -> rmdir succeeds; rmdir of a NON-EMPTY dir refuses;
     * after cleanup the name resolves to nothing. */
    const char *dpath = "/fd_selftest_dir";
    rc = vfs_mkdir_path(dpath);
    if (rc != EMBK_OK) { kprintf("FD: selftest fail: mkdir -> %s\n", embk_strerror(rc)); return rc; }

    fd = vfs_open("/fd_selftest_dir/inner.tmp", O_CREAT | O_WRONLY, 0644);
    if (fd < 0) { kprintf("FD: selftest fail: create-in-dir\n"); return fd; }
    vfs_close(fd);

    rc = vfs_rmdir_path(dpath);
    if (rc == EMBK_OK) {
        kprintf("FD: selftest fail: rmdir removed a NON-EMPTY dir\n");
        return -EMBK_EIO;
    }
    rc = vfs_unlink_path("/fd_selftest_dir/inner.tmp");
    if (rc != EMBK_OK) { kprintf("FD: selftest fail: unlink inner\n"); return rc; }
    rc = vfs_rmdir_path(dpath);
    if (rc != EMBK_OK) {
        kprintf("FD: selftest fail: rmdir of empty dir -> %s\n", embk_strerror(rc));
        return rc;
    }
    struct vnode gone;
    if (vfs_resolve(dpath, &gone) != -EMBK_ENOENT) {
        kprintf("FD: selftest fail: rmdir'd dir still resolves\n");
        return -EMBK_EIO;
    }

    kprintf("FD: selftest: OK (incl. O_TRUNC + mtime + rmdir)\n");
    return EMBK_OK;
}

/* A PTY operation pins the object independently of descriptors. Closing the
 * last peer fd therefore wakes a blocked operation immediately, while the pin
 * keeps its storage alive until the operation unwinds. */
static int pty_hold(struct fd_entry *e,struct fd_entry *copy,int *flags) {
    if (!current_thread) return -EMBK_ENOTTY;
    fdlock(current_process);
    if (!e || !e->used) { fdunlock(current_process); return -EMBK_EBADF; }
    if (e->backing!=FD_BACKING_PTY) { fdunlock(current_process); return -EMBK_ENOTTY; }
    *copy=*e;if(flags)*flags=fd_entry_flags(e);
    sched_lock();
    pty_pin_locked(copy->u.pty.p); current_thread->io_pty=copy->u.pty.p;
    sched_unlock();
    fdunlock(current_process); return 0;
}
static void pty_drop(struct fd_entry *e) {
    sched_lock(); current_thread->io_pty=NULL; pty_unpin_locked(e->u.pty.p); sched_unlock();
}
static int pty_fd_read(struct fd_entry *e,void *buf,size_t len,size_t *out) {
    struct fd_entry held;int flags; int rc=pty_hold(e,&held,&flags); if (rc) return rc;
    rc=pty_read(held.u.pty.p,held.u.pty.side,buf,len,out,(flags&O_NONBLOCK)!=0);
    pty_drop(&held); return rc;
}
static int pty_fd_write(struct fd_entry *e,const void *buf,size_t len,size_t *out) {
    struct fd_entry held;int flags; int rc=pty_hold(e,&held,&flags); if (rc) return rc;
    rc=pty_write(held.u.pty.p,held.u.pty.side,buf,len,out,(flags&O_NONBLOCK)!=0);
    pty_drop(&held); return rc;
}
static int pty_fd_fstat(struct fd_entry *e,struct vfs_stat *out) {
    (void)e; memset(out,0,sizeof *out); out->type=VFS_DT_CHAR; out->mode=0600; return 0;
}
static int pty_fd_inherit(struct fd_entry *dst,const struct fd_entry *src) {
    int rc=shared_copy(dst,src);if(rc)return rc;
    sched_lock(); pty_ref_locked(src->u.pty.p,src->u.pty.side); sched_unlock();
    return 0;
}
static void pty_fd_close_locked(struct fd_entry *e) { pty_unref_locked(e->u.pty.p,e->u.pty.side);shared_drop_locked(e); }
static void pty_fd_close(struct fd_entry *e) { sched_lock(); pty_fd_close_locked(e); sched_unlock(); }
static int64_t pty_fd_avail(struct fd_entry *e) {
    struct fd_entry held;int flags; int rc=pty_hold(e,&held,&flags); if (rc) return rc;
    int64_t n=pty_avail(held.u.pty.p,held.u.pty.side); pty_drop(&held); return n;
}
static int pty_fd_poll(struct fd_entry *e,int events) {
    struct fd_entry held;int flags; int rc=pty_hold(e,&held,&flags); if (rc) return rc;
    int n=pty_poll(held.u.pty.p,held.u.pty.side,events); pty_drop(&held); return n;
}
static const struct fd_ops pty_fd_ops={
    .read=pty_fd_read,.write=pty_fd_write,.seek=pipe_fd_seek,.fstat=pty_fd_fstat,
    .inherit=pty_fd_inherit,.close=pty_fd_close,.close_locked=pty_fd_close_locked,
    .avail=pty_fd_avail,.poll=pty_fd_poll,
};
int fd_alloc_pty(int pair[2]) {
    if (!current_thread) return -EMBK_EINVAL;
    fdlock(current_process);
    int fds[2],n=0;
    for (int i=FD_STDIO_MAX;i<FD_MAX_OPEN && n<2;i++) if (!current_process->fds[i].used) fds[n++]=i;
    if (n<2) { fdunlock(current_process); return -EMBK_EMFILE; }
    struct fd_shared *status[2]={shared_new(O_RDWR),shared_new(O_RDWR)};
    if (!status[0] || !status[1]) {
        for(int i=0;i<2;i++)if(status[i]){struct fd_entry drop={.shared=status[i]};shared_drop(&drop);}
        fdunlock(current_process);return -EMBK_ENOMEM;
    }
    struct pty *p; int rc=pty_create(&p);
    if (rc)for(int i=0;i<2;i++){struct fd_entry drop={.shared=status[i]};shared_drop(&drop);}
    if (!rc) for (int side=0;side<2;side++) {
        struct fd_entry *e=&current_process->fds[fds[side]];
        memset(e,0,sizeof *e); e->used=true; e->backing=FD_BACKING_PTY;
        e->ops=&pty_fd_ops; e->shared=status[side]; e->u.pty.p=p; e->u.pty.side=side;
        pair[side]=fds[side];
    }
    fdunlock(current_process); return rc;
}
int fd_terminal_control(int fd,unsigned op,void *data) {
    if (!current_thread) return -EMBK_ENOTTY;
    struct fd_entry held;
    if (fd<0 || fd>=FD_MAX_OPEN) return -EMBK_EBADF;
    int rc=pty_hold(&current_process->fds[fd],&held,NULL); if (rc) return rc;
    uint32_t pid=0;
    if (op==EMBK_TERMINAL_FOREGROUND) {
        int handle=*(int *)data;
        if (handle==EMBK_TERMINAL_SELF) pid=current_process->pid;
        else if (handle!=EMBK_TERMINAL_NONE && process_handle_resolve(current_process,handle,&pid)!=0) rc=-EMBK_EINVAL;
        data=&pid;
    }
    if (!rc) rc=pty_control(held.u.pty.p,held.u.pty.side,op,data);
    pty_drop(&held); return rc;
}

/* Descriptor flags are per descriptor. Duplication clears close-on-exec;
 * status flags and socket identity belong to the shared description. */
int vfs_fd_get_cloexec(int fd) {
    struct process *p=current_process;fdlock(p);
    struct fd_entry *e=fd_lookup(fd);
    int rc=e?(e->cloexec?1:0):-EMBK_EBADF;
    fdunlock(p);return rc;
}
int vfs_fd_set_cloexec(int fd,bool on) {
    struct process *p=current_process;fdlock(p);
    struct fd_entry *e=fd_lookup(fd);
    if(e)e->cloexec=on;
    fdunlock(p);return e?0:-EMBK_EBADF;
}
int fd_close_into(struct process *target,int fd) {
    if(!target || fd<0 || fd>=FD_MAX_OPEN)return -EMBK_EBADF;
    struct fd_entry *e=&target->fds[fd];
    if(!e->used)return -EMBK_EBADF;
    struct fd_entry old=*e;memset(e,0,sizeof *e);
    if(old.ops && old.ops->close)old.ops->close(&old);
    return 0;
}
int fd_dup_into(struct process *target,int oldfd,int newfd) {
    if(!target || oldfd<0 || oldfd>=FD_MAX_OPEN || newfd<0 || newfd>=FD_MAX_OPEN)return -EMBK_EBADF;
    struct fd_entry *src=&target->fds[oldfd];
    if(!src->used || !src->ops || !src->ops->inherit)return -EMBK_EBADF;
    /* Unlike ordinary dup2(fd,fd), a spawn dup2 action clears CLOEXEC. */
    if(oldfd==newfd){src->cloexec=false;return 0;}
    struct fd_entry copy={0};int rc=src->ops->inherit(&copy,src);
    if(rc)return rc;
    (void)fd_close_into(target,newfd);copy.cloexec=false;
    target->fds[newfd]=copy;return 0;
}
int fd_snapshot_into(struct process *target) {
    if(!current_thread || !target || target==current_process)return -EMBK_EINVAL;
    for(int i=0;i<FD_MAX_OPEN;i++)(void)fd_close_into(target,i);
    fdlock(current_process);
    int rc=0;
    for(int i=0;i<FD_MAX_OPEN;i++) {
        const struct fd_entry *src=&current_process->fds[i];
        if(!src->used)continue;
        if(!src->ops || !src->ops->inherit){rc=-EMBK_ENOSYS;break;}
        rc=src->ops->inherit(&target->fds[i],src);
        if(rc)break;
    }
    fdunlock(current_process);return rc;
}
void fd_close_exec(struct process *target) {
    for(int i=0;i<FD_MAX_OPEN;i++)
        if(target->fds[i].used && target->fds[i].cloexec)(void)fd_close_into(target,i);
}
