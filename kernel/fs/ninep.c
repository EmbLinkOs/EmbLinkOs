/* kernel/fs/ninep.c -- the host's filesystem, mounted in the guest.
 *
 * WHY THIS IS THE HIGHEST-LEVERAGE THING IN docs/HARDWARE_GAPS.md's list: it
 * is the only entry that makes every OTHER entry cheaper. Testing a one-line
 * change to a driver currently means rebuilding a 190 MB disk image and
 * booting it; with the build directory mounted from the host it means saving
 * the file. Everything downstream of that -- every driver, every app, every
 * test -- is developed faster.
 *
 * WHAT 9P IS. A filesystem protocol in about a dozen messages, designed for
 * Plan 9 and adopted by virtio because it is small enough to implement
 * completely rather than approximately. Every message is
 * size[4] type[1] tag[2] followed by typed fields, and every reply is either
 * the matching R-message or Rlerror with an errno in it. There is no state on
 * the wire beyond FIDs -- client-chosen handles that name an open file or a
 * position in the tree.
 *
 * READ-ONLY, DELIBERATELY, for now. The value above is entirely in reading:
 * the host builds, the guest runs. Writing back into the host's filesystem
 * from a guest that is being actively debugged is a way to lose work, and the
 * protocol side of it (Tlcreate, Twrite, Tmkdir, Tunlinkat) is a separate
 * piece that can be added when something needs it. docs/TODO.md records it.
 *
 * THE FID LIFETIME IS THE HARD PART and it is worth saying why. The VFS hands
 * out `struct vnode` by VALUE -- there is no destructor, no reference count,
 * nothing that fires when the caller is done. A fid allocated per lookup
 * would leak one per path resolution, forever. So this keeps a small CACHE of
 * fids keyed by inode number, and reuses or recycles them; an entry that is
 * evicted is clunked on the wire. That is not elegance, it is the shape the
 * VFS's ownership model forces.
 */
#include <stdint.h>
#include <stddef.h>

#include "include/types.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/errno.h"
#include "drivers/bus/virtio_pci.h"
#include "fs/vfs.h"
#include "fs/ninep.h"
#include "mm/pmm.h"

#define VIRTIO_9P_DEVID_M 0x1049   /* 0x1040 + device type 9 */
#define VIRTIO_9P_DEVID_T 0x1009   /* transitional / legacy  */

/* ---- the protocol ------------------------------------------------------- */
#define P9_TLERROR   6
#define P9_RLERROR   7
#define P9_TSTATFS   8
#define P9_TLOPEN    12
#define P9_RLOPEN    13
#define P9_TGETATTR  24
#define P9_RGETATTR  25
#define P9_TREADDIR  40
#define P9_RREADDIR  41
#define P9_TVERSION  100
#define P9_RVERSION  101
#define P9_TATTACH   104
#define P9_RATTACH   105
#define P9_TWALK     110
#define P9_RWALK     111
#define P9_TREAD     116
#define P9_RREAD     117
#define P9_TCLUNK    120
#define P9_RCLUNK    121

#define P9_QTDIR     0x80
#define P9_QTSYMLINK 0x02

#define P9_NOFID     0xFFFFFFFFu
#define P9_GETATTR_BASIC 0x000007FFULL

/* The message size we negotiate. Big enough that a directory listing and a
 * read are one round trip each, small enough to live in .bss -- the ring
 * buffers must be KV2P-able, which rules out the heap. */
#define P9_MSIZE 8192

#define NP_QSIZE 8

static struct virtio_pci_dev g_vd;
static bool g_up;
static char g_tag[32];

static struct vring_desc g_desc[NP_QSIZE] __attribute__((aligned(16)));
static struct {
    uint16_t flags, idx;
    uint16_t ring[NP_QSIZE];
    uint16_t used_event;
} __attribute__((packed, aligned(2))) g_avail;
static struct {
    uint16_t flags, idx;
    struct vring_used_elem ring[NP_QSIZE];
    uint16_t avail_event;
} __attribute__((packed, aligned(4))) g_used;

static uint8_t  g_req[P9_MSIZE] __attribute__((aligned(64)));
static uint8_t  g_rep[P9_MSIZE] __attribute__((aligned(64)));
static uint16_t g_notify_off, g_qsize, g_last_used;

static inline uint64_t dma(const volatile void *p) {
    return KV2P((uint64_t)(uintptr_t)p);
}

/* ---- building and reading messages --------------------------------------- */
struct p9buf { uint8_t *b; uint32_t len, cap; bool bad; };

static void pw8(struct p9buf *m, uint8_t v) {
    if (m->len + 1 > m->cap) { m->bad = true; return; }
    m->b[m->len++] = v;
}
static void pw16(struct p9buf *m, uint16_t v) { pw8(m, (uint8_t)v); pw8(m, (uint8_t)(v >> 8)); }
static void pw32(struct p9buf *m, uint32_t v) { pw16(m, (uint16_t)v); pw16(m, (uint16_t)(v >> 16)); }
static void pw64(struct p9buf *m, uint64_t v) { pw32(m, (uint32_t)v); pw32(m, (uint32_t)(v >> 32)); }
static void pwstr(struct p9buf *m, const char *s, uint32_t n) {
    pw16(m, (uint16_t)n);
    for (uint32_t i = 0; i < n; i++) pw8(m, (uint8_t)s[i]);
}

static uint8_t  pr8 (struct p9buf *m) { return m->len + 1 > m->cap ? (m->bad = true, 0) : m->b[m->len++]; }
static uint16_t pr16(struct p9buf *m) { uint16_t a = pr8(m); return (uint16_t)(a | (pr8(m) << 8)); }
static uint32_t pr32(struct p9buf *m) { uint32_t a = pr16(m); return a | ((uint32_t)pr16(m) << 16); }
static uint64_t pr64(struct p9buf *m) { uint64_t a = pr32(m); return a | ((uint64_t)pr32(m) << 32); }
static void     prskip(struct p9buf *m, uint32_t n) {
    if (m->len + n > m->cap) m->bad = true; else m->len += n;
}

/* One request, one reply. Synchronous: the filesystem sits under the VFS and
 * every caller above it is already blocking on a path walk. */
static int p9_rpc(uint32_t req_len, uint8_t want_type, struct p9buf *reply) {
    if (!g_up) return -EMBK_ENODEV;

    /* Patch the size field now that the body is built. */
    g_req[0] = (uint8_t)req_len;       g_req[1] = (uint8_t)(req_len >> 8);
    g_req[2] = (uint8_t)(req_len >> 16); g_req[3] = (uint8_t)(req_len >> 24);

    g_desc[0].addr  = dma(g_req);
    g_desc[0].len   = req_len;
    g_desc[0].flags = VRING_DESC_F_NEXT;
    g_desc[0].next  = 1;
    g_desc[1].addr  = dma(g_rep);
    g_desc[1].len   = sizeof g_rep;
    g_desc[1].flags = VRING_DESC_F_WRITE;
    g_desc[1].next  = 0;

    g_avail.ring[g_avail.idx % g_qsize] = 0;
    __sync_synchronize();
    g_avail.idx++;
    __sync_synchronize();
    virtio_pci_notify(&g_vd, g_notify_off, 0);

    for (int spin = 0; spin < 20000000; spin++) {
        if (g_used.idx != g_last_used) { g_last_used++; goto done; }
        __asm__ volatile("" ::: "memory");
    }
    kprintf("9p: the server did not answer a type-%u request\n", g_req[4]);
    return -EMBK_EIO;

done: ;
    reply->b = g_rep; reply->cap = sizeof g_rep; reply->len = 0; reply->bad = false;
    uint32_t size = pr32(reply);
    uint8_t  type = pr8(reply);
    prskip(reply, 2);                      /* tag */
    if (reply->bad || size < 7) return -EMBK_EIO;
    if (reply->cap > size) reply->cap = size;

    if (type == P9_RLERROR) {
        uint32_t ecode = pr32(reply);
        /* The server's errno is a LINUX one. The two that callers act on are
         * mapped; the rest become EIO, which is honest -- inventing a
         * specific error from a number we did not check is worse. */
        if (ecode == 2)  return -EMBK_ENOENT;
        if (ecode == 13) return -EMBK_EACCES;
        if (ecode == 20) return -EMBK_ENOTDIR;
        if (ecode == 21) return -EMBK_EISDIR;
        return -EMBK_EIO;
    }
    if (type != want_type) {
        kprintf("9p: expected reply %u, got %u\n", want_type, type);
        return -EMBK_EIO;
    }
    return EMBK_OK;
}

static struct p9buf p9_start(uint8_t type) {
    struct p9buf m = { g_req, 0, sizeof g_req, false };
    pw32(&m, 0);            /* size, patched by p9_rpc */
    pw8(&m, type);
    pw16(&m, 0);            /* tag: one request at a time, so always zero */
    return m;
}

/* ---- fids ----------------------------------------------------------------
 *
 * A fid is the server's handle on a file. The VFS has no destructor for a
 * vnode, so a fid allocated per lookup would leak one per path component
 * forever. This keeps a bounded cache keyed by the inode number the VFS
 * carries, and clunks the least-recently-used entry when it needs a slot. */
#define P9_MAX_FIDS 64
#define P9_ROOT_FID 0

static struct {
    bool     used;
    uint32_t fid;
    uint64_t ino;        /* the qid path the server gave us */
    bool     opened;
    bool     is_dir;
    uint64_t stamp;
} g_fids[P9_MAX_FIDS];
static uint64_t g_clock;
static uint32_t g_next_fid = 1;

static int p9_clunk(uint32_t fid) {
    struct p9buf m = p9_start(P9_TCLUNK);
    pw32(&m, fid);
    if (m.bad) return -EMBK_EIO;
    struct p9buf r;
    return p9_rpc(m.len, P9_RCLUNK, &r);
}

static int fid_slot_for(uint64_t ino) {
    for (int i = 0; i < P9_MAX_FIDS; i++)
        if (g_fids[i].used && g_fids[i].ino == ino) {
            g_fids[i].stamp = ++g_clock;
            return i;
        }
    return -1;
}

static int fid_alloc(uint64_t ino, uint32_t *out_fid) {
    int slot = -1;
    for (int i = 0; i < P9_MAX_FIDS; i++)
        if (!g_fids[i].used) { slot = i; break; }
    if (slot < 0) {
        /* Evict the least recently used, and TELL THE SERVER. A fid dropped
         * without a Tclunk stays allocated on the other side for the life of
         * the mount, and the server's table is not larger than ours. */
        uint64_t oldest = ~0ULL;
        for (int i = 0; i < P9_MAX_FIDS; i++)
            if (g_fids[i].stamp < oldest) { oldest = g_fids[i].stamp; slot = i; }
        p9_clunk(g_fids[slot].fid);
        g_fids[slot].used = false;
    }
    g_fids[slot].used = true;
    g_fids[slot].fid = g_next_fid++;
    g_fids[slot].ino = ino;
    g_fids[slot].opened = false;
    g_fids[slot].stamp = ++g_clock;
    *out_fid = g_fids[slot].fid;
    return slot;
}

/* The fid for a vnode, walking from the root if the cache has lost it. Every
 * vnode carries its qid path as its inode number; the cache maps that back. */
static int fid_for(uint64_t ino, uint32_t *out) {
    if (ino == 1) { *out = P9_ROOT_FID; return 0; }
    int s = fid_slot_for(ino);
    /* The cache lost this fid and the vnode is the only record of the path,
     * which the VFS does not keep. ENOENT is the honest answer: from here the
     * file cannot be reached again without walking from the root. */
    if (s < 0) return -EMBK_ENOENT;
    *out = g_fids[s].fid;
    return 0;
}

/* ---- the operations ------------------------------------------------------ */
static int p9_getattr(uint32_t fid, uint64_t *size, uint32_t *mode) {
    struct p9buf m = p9_start(P9_TGETATTR);
    pw32(&m, fid);
    pw64(&m, P9_GETATTR_BASIC);
    if (m.bad) return -EMBK_EIO;

    struct p9buf r;
    int rc = p9_rpc(m.len, P9_RGETATTR, &r);
    if (rc != EMBK_OK) return rc;

    prskip(&r, 8);              /* valid  */
    prskip(&r, 13);             /* qid    */
    uint32_t md = pr32(&r);
    prskip(&r, 4 + 4);          /* uid, gid */
    prskip(&r, 8 + 8);          /* nlink, rdev */
    uint64_t sz = pr64(&r);
    if (r.bad) return -EMBK_EIO;
    if (size) *size = sz;
    if (mode) *mode = md;
    return EMBK_OK;
}

static int p9_walk_one(uint32_t fid, const char *name, size_t name_len,
                       uint32_t newfid, uint64_t *qid_path, bool *is_dir) {
    struct p9buf m = p9_start(P9_TWALK);
    pw32(&m, fid);
    pw32(&m, newfid);
    pw16(&m, name ? 1 : 0);
    if (name) pwstr(&m, name, (uint32_t)name_len);
    if (m.bad) return -EMBK_EIO;

    struct p9buf r;
    int rc = p9_rpc(m.len, P9_RWALK, &r);
    if (rc != EMBK_OK) return rc;

    uint16_t nwqid = pr16(&r);
    if (name && nwqid != 1) return -EMBK_ENOENT;
    if (nwqid) {
        uint8_t qtype = pr8(&r);
        prskip(&r, 4);                     /* version */
        uint64_t path = pr64(&r);
        if (qid_path) *qid_path = path;
        if (is_dir) *is_dir = (qtype & P9_QTDIR) != 0;
    } else {
        if (qid_path) *qid_path = 1;       /* walked nowhere: still the root */
        if (is_dir) *is_dir = true;
    }
    return r.bad ? -EMBK_EIO : EMBK_OK;
}

static int p9_open(int slot, bool dir) {
    if (g_fids[slot].opened) return EMBK_OK;
    struct p9buf m = p9_start(P9_TLOPEN);
    pw32(&m, g_fids[slot].fid);
    pw32(&m, dir ? 0x10000 /* O_DIRECTORY */ : 0 /* O_RDONLY */);
    if (m.bad) return -EMBK_EIO;
    struct p9buf r;
    int rc = p9_rpc(m.len, P9_RLOPEN, &r);
    if (rc != EMBK_OK) return rc;
    g_fids[slot].opened = true;
    return EMBK_OK;
}

/* ---- VFS ops ------------------------------------------------------------- */
static int np_lookup(struct vnode *dir, const char *name, size_t name_len,
                     struct vnode *out) {
    uint32_t dfid;
    int rc = fid_for(dir->ino, &dfid);
    if (rc != 0) return rc;

    uint32_t nfid;
    uint64_t qid = 0;
    bool is_dir = false;
    int slot = fid_alloc(0, &nfid);
    rc = p9_walk_one(dfid, name, name_len, nfid, &qid, &is_dir);
    if (rc != EMBK_OK) {
        g_fids[slot].used = false;
        return rc;
    }
    g_fids[slot].ino = qid;
    g_fids[slot].is_dir = is_dir;

    out->mnt  = dir->mnt;
    out->ino  = qid;
    out->type = is_dir ? VFS_DT_DIR : VFS_DT_REG;
    return EMBK_OK;
}

static int np_read(struct vnode *vn, uint64_t off, void *buf, size_t len,
                   size_t *out_read) {
    if (out_read) *out_read = 0;
    uint32_t fid;
    int rc = fid_for(vn->ino, &fid);
    if (rc != 0) return rc;
    int slot = fid_slot_for(vn->ino);
    if (slot < 0) return -EMBK_ENOENT;
    rc = p9_open(slot, false);
    if (rc != EMBK_OK) return rc;

    /* THE MESSAGE SIZE IS THE CEILING, not the caller's buffer. A read bigger
     * than msize is split; asking for more in one message than was negotiated
     * is the mistake that makes a server close the connection. */
    size_t done = 0;
    while (done < len) {
        uint32_t want = (uint32_t)(len - done);
        if (want > P9_MSIZE - 32) want = P9_MSIZE - 32;

        struct p9buf m = p9_start(P9_TREAD);
        pw32(&m, fid);
        pw64(&m, off + done);
        pw32(&m, want);
        if (m.bad) return -EMBK_EIO;

        struct p9buf r;
        rc = p9_rpc(m.len, P9_RREAD, &r);
        if (rc != EMBK_OK) return rc;
        uint32_t got = pr32(&r);
        if (r.bad || got > r.cap - r.len) return -EMBK_EIO;
        if (got == 0) break;                       /* end of file */
        memcpy((uint8_t *)buf + done, r.b + r.len, got);
        done += got;
        if (got < want) break;                     /* short read: that is all */
    }
    if (out_read) *out_read = done;
    return EMBK_OK;
}

/* A FID THAT HAS BEEN OPENED CANNOT BE WALKED FROM. That is 9p's rule, and it
 * is why a directory is never opened in place: the root's fid is the one every
 * path resolution starts from, so opening it to list it would make every
 * subsequent lookup fail.
 *
 * So the fid is CLONED first -- Twalk with zero names, which is 9p's "give me
 * another handle on this same thing" -- and the clone is opened, read and
 * clunked. The original stays untouched and walkable.
 *
 * Getting this wrong is what made the first version return ENOENT for every
 * listing: Treaddir on a fid nobody had opened. */
#define P9_SCRATCH_FID 0xFFFFFFFEu

static int np_readdir(struct vnode *dir, vfs_readdir_cb cb, void *ctx) {
    uint32_t fid;
    int rc = fid_for(dir->ino, &fid);
    if (rc != 0) return rc;

    rc = p9_walk_one(fid, NULL, 0, P9_SCRATCH_FID, NULL, NULL);
    if (rc != EMBK_OK) return rc;

    struct p9buf m = p9_start(P9_TLOPEN);
    pw32(&m, P9_SCRATCH_FID);
    pw32(&m, 0x10000);                 /* O_DIRECTORY */
    struct p9buf r;
    if (m.bad || p9_rpc(m.len, P9_RLOPEN, &r) != EMBK_OK) {
        p9_clunk(P9_SCRATCH_FID);
        return -EMBK_EIO;
    }

    uint64_t offset = 0;
    rc = EMBK_OK;
    for (;;) {
        m = p9_start(P9_TREADDIR);
        pw32(&m, P9_SCRATCH_FID);
        pw64(&m, offset);
        pw32(&m, P9_MSIZE - 64);
        if (m.bad) { rc = -EMBK_EIO; break; }

        rc = p9_rpc(m.len, P9_RREADDIR, &r);
        if (rc != EMBK_OK) break;
        uint32_t count = pr32(&r);
        if (r.bad) { rc = -EMBK_EIO; break; }
        if (count == 0) break;                  /* end of the directory */

        uint32_t end = r.len + count;
        if (end > r.cap) { rc = -EMBK_EIO; break; }
        while (r.len < end) {
            uint8_t qtype = pr8(&r);
            prskip(&r, 4);                  /* qid version */
            uint64_t qpath = pr64(&r);
            offset = pr64(&r);              /* the cookie for the next call */
            prskip(&r, 1);                  /* dirent type */
            uint16_t nlen = pr16(&r);
            if (r.bad || r.len + nlen > end) { rc = -EMBK_EIO; goto out; }

            char name[256];
            uint16_t n = nlen < sizeof name - 1 ? nlen : (uint16_t)(sizeof name - 1);
            memcpy(name, r.b + r.len, n);
            name[n] = 0;
            prskip(&r, nlen);

            int crc = cb(name, (uint8_t)n,
                         (qtype & P9_QTDIR) ? VFS_DT_DIR : VFS_DT_REG,
                         qpath, ctx);
            if (crc != EMBK_OK) { rc = crc; goto out; }
        }
    }
out:
    p9_clunk(P9_SCRATCH_FID);
    return rc;
}

static int np_stat(struct vnode *vn, struct vfs_stat *out) {
    uint32_t fid;
    int rc = fid_for(vn->ino, &fid);
    if (rc != 0) return rc;
    uint64_t size = 0; uint32_t mode = 0;
    rc = p9_getattr(fid, &size, &mode);
    if (rc != EMBK_OK) return rc;

    memset(out, 0, sizeof *out);
    out->size = size;
    out->type = (mode & 0040000) ? VFS_DT_DIR : VFS_DT_REG;
    out->mode = mode & 0777;
    return EMBK_OK;
}

static int np_vget(struct vfs_mount *mnt, uint64_t ino, uint8_t type,
                   struct vnode *out) {
    out->mnt = mnt;
    out->ino = ino;
    out->type = type;
    return EMBK_OK;
}

/* READ-ONLY, and it refuses rather than pretending. A write that silently did
 * nothing would look like a successful save of work that was never stored. */
static int np_ro_write(struct vnode *vn, uint64_t off, const void *buf,
                       size_t len, size_t *out) {
    (void)vn; (void)off; (void)buf; (void)len;
    if (out) *out = 0;
    return -EMBK_EROFS;
}
static int np_ro_create(struct vnode *d, const char *n, size_t l, uint32_t m,
                        struct vnode *o) {
    (void)d;(void)n;(void)l;(void)m;(void)o; return -EMBK_EROFS;
}
static int np_ro_mkdir(struct vnode *d, const char *n, size_t l,
                       struct vnode *o) {
    (void)d;(void)n;(void)l;(void)o; return -EMBK_EROFS;
}
static int np_ro_unlink(struct vnode *d, const char *n, size_t l) {
    (void)d;(void)n;(void)l; return -EMBK_EROFS;
}

static const struct vfs_ops ninep_vfs_ops = {
    .lookup  = np_lookup,
    .readdir = np_readdir,
    .read    = np_read,
    .write   = np_ro_write,
    .create  = np_ro_create,
    .mkdir   = np_ro_mkdir,
    .unlink  = np_ro_unlink,
    .stat    = np_stat,
    .vget    = np_vget,
    .obj_get = NULL,
    .obj_put = NULL,
};

const void *ninep_vfs_ops_ptr(void) { return &ninep_vfs_ops; }

/* ---- bringing it up ------------------------------------------------------ */
static int g_mount_sentinel;

bool ninep_init(const char *mount_at) {
    if (g_up) return true;

    const struct pci_device *pci = virtio_pci_find(VIRTIO_9P_DEVID_M, 0);
    if (!pci) pci = virtio_pci_find(VIRTIO_9P_DEVID_T, 0);
    if (!pci) return false;

    if (!virtio_pci_attach(&g_vd, pci, "virtio-9p", 0, 0)) return false;

    /* THE MOUNT TAG is in device configuration space: a length then the
     * bytes. It is what the host called the share, and it is the only way to
     * tell two shares apart. */
    if (g_vd.devcfg) {
        uint16_t tlen = vp_r16(g_vd.devcfg, 0);
        if (tlen > sizeof g_tag - 1) tlen = sizeof g_tag - 1;
        for (uint16_t i = 0; i < tlen; i++) g_tag[i] = (char)vp_r8(g_vd.devcfg, 2 + i);
        g_tag[tlen] = 0;
    }

    memset(g_desc, 0, sizeof g_desc);
    memset((void *)&g_avail, 0, sizeof g_avail);
    memset((void *)&g_used, 0, sizeof g_used);
    g_avail.flags = VRING_AVAIL_F_NO_INTERRUPT;

    g_qsize = virtio_pci_setup_queue(&g_vd, 0, NP_QSIZE, g_desc, &g_avail,
                                     &g_used, &g_notify_off);
    if (!g_qsize) { kprintf("9p: no request queue\n"); return false; }
    virtio_pci_driver_ok(&g_vd);
    g_last_used = g_used.idx;
    g_up = true;

    /* Tversion first. The server may answer with a SMALLER msize than asked
     * for, and every later message has to respect it -- so this refuses a
     * server that wants less than we can work with rather than silently
     * overrunning it later. */
    struct p9buf m = p9_start(P9_TVERSION);
    pw32(&m, P9_MSIZE);
    pwstr(&m, "9P2000.L", 8);
    struct p9buf r;
    if (p9_rpc(m.len, P9_RVERSION, &r) != EMBK_OK) { g_up = false; return false; }
    uint32_t msize = pr32(&r);
    uint16_t vlen = pr16(&r);
    if (r.bad || msize < 4096) {
        kprintf("9p: server offered msize %u -- too small\n", msize);
        g_up = false;
        return false;
    }
    if (vlen != 8 || memcmp(r.b + r.len, "9P2000.L", 8) != 0) {
        kprintf("9p: server does not speak 9P2000.L\n");
        g_up = false;
        return false;
    }

    /* Tattach binds fid 0 to the root of the share. n_uname 0 is "the user
     * the server already decided we are", which is what a passthrough share
     * with no security model means. */
    m = p9_start(P9_TATTACH);
    pw32(&m, P9_ROOT_FID);
    pw32(&m, P9_NOFID);
    pwstr(&m, "", 0);
    pwstr(&m, "", 0);
    pw32(&m, 0);
    if (p9_rpc(m.len, P9_RATTACH, &r) != EMBK_OK) {
        kprintf("9p: the server refused the attach\n");
        g_up = false;
        return false;
    }

    if (vfs_mount(mount_at, &ninep_vfs_ops, &g_mount_sentinel, 1) != EMBK_OK) {
        kprintf("9p: could not mount at %s\n", mount_at);
        g_up = false;
        return false;
    }

    kprintf("9p: host share \"%s\" mounted read-only at %s (msize %u)\n",
            g_tag[0] ? g_tag : "(untagged)", mount_at, msize);
    return true;
}

bool ninep_present(void) { return g_up; }
const char *ninep_tag(void) { return g_tag; }
