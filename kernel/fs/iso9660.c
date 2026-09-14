/* kernel/fs/iso9660.c -- the filesystem on a CD.
 *
 * ISO 9660 is the only filesystem this kernel reads that was designed for a
 * medium that CANNOT BE WRITTEN. Every consequence follows from that: there is
 * no allocation table, no free list, no journal and no fragmentation. A file
 * is a START BLOCK AND A LENGTH, stored in its directory entry, and a
 * directory is a file whose contents are directory entries. Reading it is
 * arithmetic.
 *
 * That also means there is nothing to be careful about on a crash, which is
 * why this file is a fifth the size of embkfs.c and has no write path at all.
 *
 * THREE THINGS THE FORMAT DOES THAT NOTHING ELSE HERE DOES:
 *
 *   1. EVERY MULTI-BYTE FIELD IS STORED TWICE, once little-endian and once
 *      big-endian, back to back. The little-endian copy comes first and is
 *      the one read here; a reader that takes the field's full width gets a
 *      number with the same value's byte-reversal glued to the top of it.
 *   2. FILE NAMES CARRY A VERSION SUFFIX, ";1". It is part of the stored name
 *      and part of no name a person ever types, so it is stripped on the way
 *      out and tolerated on the way in.
 *   3. THE FIRST TWO ENTRIES OF EVERY DIRECTORY have zero-length names and
 *      mean "." and "..". They are not files and are not listed.
 *
 * Names are matched case-insensitively. The standard stores them upper-cased,
 * and a user who types `readme.txt` means the `README.TXT;1` that is there.
 */
#include <stdint.h>
#include <stddef.h>

#include "include/types.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/errno.h"
#include "block/block.h"
#include "fs/vfs.h"
#include "fs/iso9660.h"

#define ISO_BLOCK       2048u
#define ISO_PVD_LBA     16u          /* the volume descriptors start here */
#define ISO_ROOT_RECORD 156u         /* offset of the root record in the PVD */

#define ISO_FLAG_DIR    0x02

/* A directory record's fixed part. Sizes are the standard's. */
#define REC_LEN        0
#define REC_EXTENT     2     /* both-endian LBA; LE copy at +2      */
#define REC_SIZE      10     /* both-endian length; LE copy at +10  */
#define REC_FLAGS     25
#define REC_NAME_LEN  32
#define REC_NAME      33

struct iso_volume {
    struct embk_block_device *dev;
    uint32_t root_lba;
    uint32_t root_size;
    char     label[33];
};

static struct iso_volume g_vols[2];
static uint32_t g_nvols;

/* An inode number here is the directory record's own position: the block it
 * lives in and the byte offset inside it. A record never straddles a block --
 * the standard forbids it -- so 21 bits of block and 11 of offset address
 * every record on a disc up to 4 GiB, and the size is carried alongside. */
static inline uint64_t ino_make(uint32_t lba, uint32_t size) {
    return ((uint64_t)lba << 32) | ((uint64_t)size & 0xFFFFFFFFULL);
}
static inline uint32_t ino_lba(uint64_t ino)  { return (uint32_t)(ino >> 32); }
static inline uint32_t ino_size(uint64_t ino) { return (uint32_t)ino; }

static inline uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* One 2 KiB logical block. The block device may have any sector size; the
 * filesystem's block is 2048 and the conversion happens here and nowhere
 * else. */
static int iso_read_block(struct iso_volume *v, uint32_t lba, void *buf) {
    uint32_t sectors = ISO_BLOCK / v->dev->block_size;
    if (!sectors) return -EMBK_EIO;              /* a >2 KiB sector: not a disc */
    return v->dev->read(v->dev, (uint64_t)lba * sectors, sectors, buf);
}

static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

/* Compare a stored name against a wanted one, ignoring case and the ";1"
 * version suffix. A directory's stored name has no suffix; a file's does. */
static bool name_eq(const uint8_t *stored, uint8_t stored_len,
                    const char *want, size_t want_len) {
    uint8_t n = stored_len;
    for (uint8_t i = 0; i < stored_len; i++)
        if (stored[i] == ';') { n = i; break; }
    /* A trailing '.' with nothing after it is how an extensionless file is
     * stored. It is not part of the name. */
    if (n && stored[n - 1] == '.') n--;
    if (n != want_len) return false;
    for (uint8_t i = 0; i < n; i++)
        if (up((char)stored[i]) != up(want[i])) return false;
    return true;
}

static uint8_t visible_name(const uint8_t *stored, uint8_t stored_len) {
    uint8_t n = stored_len;
    for (uint8_t i = 0; i < stored_len; i++)
        if (stored[i] == ';') { n = i; break; }
    if (n && stored[n - 1] == '.') n--;
    return n;
}

/* Walk every record of a directory, calling `fn`. Returns whatever `fn`
 * returns when it stops early, or EMBK_OK when the directory runs out. */
typedef int (*rec_fn)(const uint8_t *rec, void *ctx);

static int iso_walk_dir(struct iso_volume *v, uint32_t lba, uint32_t size,
                        rec_fn fn, void *ctx) {
    static uint8_t block[ISO_BLOCK] __attribute__((aligned(64)));
    uint32_t blocks = (size + ISO_BLOCK - 1) / ISO_BLOCK;
    for (uint32_t b = 0; b < blocks; b++) {
        int rc = iso_read_block(v, lba + b, block);
        if (rc != EMBK_OK) return rc;
        uint32_t off = 0;
        while (off < ISO_BLOCK) {
            uint8_t len = block[off];
            /* A ZERO LENGTH IS NOT THE END OF THE DIRECTORY -- it is the end
             * of the records in THIS block. The next block may have more.
             * Treating it as the end truncates every directory that spans
             * more than 2 KiB, which is most of them on a real disc. */
            if (len == 0) break;
            if (off + len > ISO_BLOCK) break;
            rc = fn(&block[off], ctx);
            if (rc != EMBK_OK) return rc;
            off += len;
        }
    }
    return EMBK_OK;
}

/* ---- lookup ---------------------------------------------------------- */

struct lookup_ctx {
    const char *name;
    size_t      name_len;
    uint32_t    lba, size;
    uint8_t     type;
    bool        found;
};

static int lookup_rec(const uint8_t *rec, void *ctx) {
    struct lookup_ctx *l = ctx;
    uint8_t nlen = rec[REC_NAME_LEN];
    if (nlen == 0 || (nlen == 1 && (rec[REC_NAME] == 0 || rec[REC_NAME] == 1)))
        return EMBK_OK;                       /* "." and ".." */
    if (!name_eq(&rec[REC_NAME], nlen, l->name, l->name_len)) return EMBK_OK;
    l->lba  = le32(&rec[REC_EXTENT]);
    l->size = le32(&rec[REC_SIZE]);
    l->type = (rec[REC_FLAGS] & ISO_FLAG_DIR) ? VFS_DT_DIR : VFS_DT_REG;
    l->found = true;
    return -EMBK_EEXIST;                      /* any non-OK stops the walk */
}

static int iso_lookup(struct vnode *dir, const char *name, size_t name_len,
                      struct vnode *out) {
    struct iso_volume *v = dir->mnt->fs_data;
    struct lookup_ctx l = { name, name_len, 0, 0, 0, false };
    iso_walk_dir(v, ino_lba(dir->ino), ino_size(dir->ino), lookup_rec, &l);
    if (!l.found) return -EMBK_ENOENT;
    out->mnt = dir->mnt;
    out->ino = ino_make(l.lba, l.size);
    out->type = l.type;
    return EMBK_OK;
}

/* ---- readdir --------------------------------------------------------- */

struct readdir_ctx { vfs_readdir_cb cb; void *ctx; };

static int readdir_rec(const uint8_t *rec, void *ctx) {
    struct readdir_ctx *r = ctx;
    uint8_t nlen = rec[REC_NAME_LEN];
    if (nlen == 0 || (nlen == 1 && (rec[REC_NAME] == 0 || rec[REC_NAME] == 1)))
        return EMBK_OK;
    uint8_t vis = visible_name(&rec[REC_NAME], nlen);
    if (!vis) return EMBK_OK;
    uint32_t lba = le32(&rec[REC_EXTENT]);
    uint32_t size = le32(&rec[REC_SIZE]);
    bool isdir = (rec[REC_FLAGS] & ISO_FLAG_DIR) != 0;
    return r->cb((const char *)&rec[REC_NAME], vis,
                 isdir ? VFS_DT_DIR : VFS_DT_REG, ino_make(lba, size),
                 r->ctx);
}

static int iso_readdir(struct vnode *dir, vfs_readdir_cb cb, void *ctx) {
    struct iso_volume *v = dir->mnt->fs_data;
    struct readdir_ctx r = { cb, ctx };
    return iso_walk_dir(v, ino_lba(dir->ino), ino_size(dir->ino), readdir_rec, &r);
}

/* ---- read ------------------------------------------------------------ */

static int iso_read(struct vnode *vn, uint64_t off, void *buf, size_t len,
                    size_t *out_read) {
    struct iso_volume *v = vn->mnt->fs_data;
    uint32_t size = ino_size(vn->ino);
    *out_read = 0;
    if (off >= size) return EMBK_OK;                 /* clean EOF, not an error */
    if (off + len > size) len = size - (size_t)off;

    static uint8_t block[ISO_BLOCK] __attribute__((aligned(64)));
    uint8_t *dst = buf;
    uint32_t base = ino_lba(vn->ino);
    while (len) {
        uint32_t blk = (uint32_t)(off / ISO_BLOCK);
        uint32_t skip = (uint32_t)(off % ISO_BLOCK);
        uint32_t n = ISO_BLOCK - skip;
        if (n > len) n = (uint32_t)len;
        int rc = iso_read_block(v, base + blk, block);
        if (rc != EMBK_OK) return rc;
        memcpy(dst, block + skip, n);
        dst += n; off += n; len -= n; *out_read += n;
    }
    return EMBK_OK;
}

/* ---- stat / vget ----------------------------------------------------- */

static int iso_stat(struct vnode *vn, struct vfs_stat *out) {
    out->type  = vn->type;
    /* Read-only for everybody, and honestly so: the medium cannot be written
     * whatever the bits say. */
    out->mode  = (vn->type == VFS_DT_DIR ? 0040000u : 0100000u) | 0555u;
    out->size  = ino_size(vn->ino);
    out->mtime = 0;                 /* the record has a date; nothing reads it */
    out->nlink = 1;
    return EMBK_OK;
}

static int iso_vget(struct vfs_mount *mnt, uint64_t ino, uint8_t type,
                    struct vnode *out) {
    out->mnt = mnt;
    out->ino = ino;
    out->type = type;
    return EMBK_OK;
}

/* Every mutating op is absent. The VFS answers -ENOSYS for a NULL op, which
 * is the right answer for a medium that physically cannot take the write --
 * better than EROFS, which suggests a remount would help. */
static const struct vfs_ops g_iso_ops = {
    .lookup  = iso_lookup,
    .readdir = iso_readdir,
    .read    = iso_read,
    .stat    = iso_stat,
    .vget    = iso_vget,
};

const void *iso9660_vfs_ops_ptr(void) { return &g_iso_ops; }

int iso9660_mount(struct embk_block_device *dev, const char *at) {
    if (g_nvols >= 2) return -EMBK_ENOMEM;
    struct iso_volume *v = &g_vols[g_nvols];
    memset(v, 0, sizeof *v);
    v->dev = dev;

    static uint8_t pvd[ISO_BLOCK] __attribute__((aligned(64)));
    if (iso_read_block(v, ISO_PVD_LBA, pvd) != EMBK_OK) return -EMBK_EIO;

    /* THE IDENTIFIER IS THE TEST. Type 1 is the primary volume descriptor and
     * "CD001" is the standard identifier; a disc with anything else at block
     * 16 is not ISO 9660 and must be refused rather than parsed. */
    if (pvd[0] != 1 || pvd[1] != 'C' || pvd[2] != 'D' ||
        pvd[3] != '0' || pvd[4] != '0' || pvd[5] != '1')
        return -EMBK_EINVAL;

    const uint8_t *root = &pvd[ISO_ROOT_RECORD];
    v->root_lba  = le32(&root[REC_EXTENT]);
    v->root_size = le32(&root[REC_SIZE]);
    if (!v->root_size) return -EMBK_EINVAL;

    /* The volume label is 32 bytes of space-padded text at offset 40. */
    memcpy(v->label, &pvd[40], 32);
    v->label[32] = 0;
    for (int i = 31; i >= 0 && v->label[i] == ' '; i--) v->label[i] = 0;

    g_nvols++;
    int rc = vfs_mount(at, &g_iso_ops, v, ino_make(v->root_lba, v->root_size));
    if (rc != EMBK_OK) { g_nvols--; return rc; }

    kprintf("iso9660: %s mounted at %s (volume \"%s\", root %u blocks)\n",
            dev->name, at, v->label[0] ? v->label : "(unlabelled)",
            (v->root_size + ISO_BLOCK - 1) / ISO_BLOCK);
    return EMBK_OK;
}

bool iso9660_probe(struct embk_block_device *dev) {
    struct iso_volume tmp = { .dev = dev };
    static uint8_t pvd[ISO_BLOCK] __attribute__((aligned(64)));
    if (iso_read_block(&tmp, ISO_PVD_LBA, pvd) != EMBK_OK) return false;
    return pvd[0] == 1 && pvd[1] == 'C' && pvd[2] == 'D' &&
           pvd[3] == '0' && pvd[4] == '0' && pvd[5] == '1';
}
