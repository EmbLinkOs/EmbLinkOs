/* kernel/drivers/storage/nvme.c -- NVM Express, over PCIe, on both architectures.
 *
 * WHY THIS IS A PILLAR AND NOT A DRIVER. Until this file the kernel could read
 * a disk through ATA, AHCI or virtio-blk. virtio-blk exists only inside a
 * virtual machine; ATA and AHCI are SATA, which a laptop or desktop bought in
 * the last several years very probably does not boot from. On such a machine
 * the OS had no disk -- which means no filesystem, which means nothing to run.
 * Everything else in the tree was, on that machine, unreachable.
 *
 * SHARED CODE, like virtio-blk: NVMe is a PCIe protocol, not an architecture,
 * so the same file serves x86 and aarch64, and QEMU's `-device nvme` exercises
 * it on both.
 *
 * THE SHAPE, and each choice in it:
 *
 *   One admin queue pair and ONE I/O queue pair per controller. NVMe allows
 *   65,535 I/O queues so every core can submit without a lock; this kernel's
 *   filesystem is synchronous above a single big lock (docs/TODO.md), and the
 *   concurrent-virtio-blk experiment measured that a deeper device path buys
 *   nothing while that lock is there. One queue is the honest depth today.
 *
 *   POLLED, with interrupts masked at the controller (INTMS) AND at PCI
 *   (command-register INTx disable). A device that raises a level-triggered
 *   line nobody services stops the machine, and "the driver polls" does not
 *   stop the device asserting it -- virtio-blk learned that the hard way and
 *   says so. Belt and braces, because the failure is a silent wedge.
 *
 *   ONE COMMAND IN FLIGHT per controller, under a sleeping lock. The command
 *   memory, the bounce buffer and the PRP list are per controller, and a
 *   second thread overwriting them mid-command is exactly the virtio-blk bug
 *   ("request timed out", far from the cause). A sleeping lock because the
 *   wait is a device round trip.
 *
 *   A BOUNCE BUFFER. Callers hand in kernel pointers that are not guaranteed
 *   physically contiguous, and NVMe addresses memory in physical pages (PRPs).
 *   The bounce buffer lives in .bss, so it IS contiguous (the kernel image is
 *   mapped linearly) and KV2P() turns it into bus addresses.
 *
 * WHAT IT DOES NOT DO, stated so nothing downstream assumes it:
 *   - namespaces with per-block metadata (end-to-end protection) are skipped;
 *   - namespaces whose logical block is not 512..4096 bytes are skipped;
 *   - controllers whose minimum memory page is not 4 KiB are refused;
 *   - no MSI-X, no multiple I/O queues, no TRIM (Dataset Management).
 * Each is a real feature and none is needed to boot from, read and write an
 * ordinary consumer NVMe SSD. */
#include "drivers/storage/nvme.h"
#include "drivers/bus/pci.h"
#include "block/block.h"
#include "mm/vmm.h"
#include "mm/pmm.h"                 /* KV2P */
#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/errno.h"
#include "include/arch_irq.h"       /* arch_cpu_relax */
#include "process/ksync.h"

/* --- controller registers (BAR0) ------------------------------------------ */
#define NVME_REG_CAP    0x00        /* capabilities, 64-bit                  */
#define NVME_REG_VS     0x08        /* version                               */
#define NVME_REG_INTMS  0x0C        /* interrupt mask set                    */
#define NVME_REG_CC     0x14        /* controller configuration              */
#define NVME_REG_CSTS   0x1C        /* controller status                     */
#define NVME_REG_AQA    0x24        /* admin queue attributes                */
#define NVME_REG_ASQ    0x28        /* admin submission queue base, 64-bit   */
#define NVME_REG_ACQ    0x30        /* admin completion queue base, 64-bit   */
#define NVME_DOORBELLS  0x1000

#define CC_EN           (1u << 0)
#define CC_CSS_NVM      (0u << 4)
#define CC_MPS_4K       (0u << 7)   /* memory page = 4 KiB << 0              */
#define CC_AMS_RR       (0u << 11)
#define CC_SHN_NORMAL   (1u << 14)
#define CC_SHN_MASK     (3u << 14)
#define CC_IOSQES       (6u << 16)  /* SQ entry = 2^6  = 64 bytes            */
#define CC_IOCQES       (4u << 20)  /* CQ entry = 2^4  = 16 bytes            */

#define CSTS_RDY        (1u << 0)
#define CSTS_CFS        (1u << 1)   /* controller fatal status               */
#define CSTS_SHST_MASK  (3u << 2)
#define CSTS_SHST_DONE  (2u << 2)

/* --- opcodes -------------------------------------------------------------- */
#define ADM_CREATE_SQ   0x01
#define ADM_CREATE_CQ   0x05
#define ADM_IDENTIFY    0x06
#define NVM_FLUSH       0x00
#define NVM_WRITE       0x01
#define NVM_READ        0x02

#define CNS_NAMESPACE   0x00
#define CNS_CONTROLLER  0x01
#define CNS_ACTIVE_NSID 0x02

/* PCI identity: mass storage / non-volatile memory / NVM Express. */
#define PCI_CLASS_STORAGE   0x01
#define PCI_SUBCLASS_NVM    0x08
#define PCI_PROGIF_NVME     0x02

#define PAGE            4096u
#define QDEPTH          64
#define BOUNCE_SZ       (64u * 1024u)
#define NVME_MAX_CTRL   2
#define NVME_MAX_NS     4

/* How long to spin for a device before calling it dead. Counted in polls
 * rather than time for the same reason virtio-blk does: this runs before the
 * TSC is calibrated on x86, when time_get_ns() still returns 0. Generous --
 * CAP.TO allows a controller up to 127.5 s to become ready -- because the
 * bound exists only so a dead device reports an error instead of hanging the
 * boot, and a slow-but-alive one must not be declared dead. */
#define NVME_SPIN_LIMIT 4000000000ULL

/* --- queue entries (NVMe base spec, figures "Common Command Format" and
 *     "Completion Queue Entry") ------------------------------------------- */
struct nvme_sqe {
    uint32_t cdw0;          /* opcode [7:0], fuse [9:8], psdt [15:14], cid [31:16] */
    uint32_t nsid;
    uint64_t rsvd;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
} __attribute__((packed));

struct nvme_cqe {
    uint32_t dw0;           /* command-specific result                           */
    uint32_t dw1;
    uint16_t sqhd;          /* how far the controller has consumed our SQ        */
    uint16_t sqid;
    uint16_t cid;
    uint16_t status;        /* bit 0 = PHASE, bits 15:1 = status field           */
} __attribute__((packed));

_Static_assert(sizeof(struct nvme_sqe) == 64, "NVMe submission entry is 64 bytes");
_Static_assert(sizeof(struct nvme_cqe) == 16, "NVMe completion entry is 16 bytes");

struct nvme_queue {
    volatile struct nvme_sqe *sq;
    volatile struct nvme_cqe *cq;
    volatile uint32_t        *sq_db;    /* submission tail doorbell */
    volatile uint32_t        *cq_db;    /* completion head doorbell */
    uint16_t qid, depth;
    uint16_t sq_tail, cq_head;
    uint16_t phase;                     /* the PHASE bit a NEW entry carries */
    uint16_t next_cid;
};

struct nvme_ctrl;

struct nvme_ns {
    struct nvme_ctrl        *ctrl;
    uint32_t                 nsid;
    uint32_t                 lba_size;
    uint64_t                 nblocks;
    struct embk_block_device blkdev;
};

/* One per controller. The DMA regions come first and are page-aligned: every
 * queue base must be, the identify buffer is a full page by definition, and a
 * PRP entry names a page. .bss, not kmalloc -- kmalloc'd memory is in the heap
 * window, which is mapped but not KV2P-able. */
struct nvme_ctrl {
    struct nvme_sqe asq[QDEPTH]       __attribute__((aligned(PAGE)));
    struct nvme_cqe acq[QDEPTH]       __attribute__((aligned(PAGE)));
    struct nvme_sqe iosq[QDEPTH]      __attribute__((aligned(PAGE)));
    struct nvme_cqe iocq[QDEPTH]      __attribute__((aligned(PAGE)));
    uint8_t         ident[PAGE]       __attribute__((aligned(PAGE)));
    uint64_t        prp_list[PAGE / 8] __attribute__((aligned(PAGE)));
    uint8_t         bounce[BOUNCE_SZ] __attribute__((aligned(PAGE)));

    volatile uint8_t *regs;
    uint32_t          dstrd;            /* doorbell stride, bytes           */
    uint16_t          depth;            /* entries per queue (min(64, MQES)) */
    uint32_t          max_xfer;         /* bytes per command (MDTS, bounce) */
    bool              vwc;              /* has a volatile write cache       */
    bool              up;
    char              model[41];
    struct nvme_queue admin, io;
    struct mutex      lock;
    int               nns;
    struct nvme_ns    ns[NVME_MAX_NS];
};

static struct nvme_ctrl g_ctrl[NVME_MAX_CTRL];
static int              g_nctrl;
static int              g_nns_total;

/* --- register access ------------------------------------------------------
 * 64-bit registers are accessed as two 32-bit halves, low first. Every
 * controller must accept that (it is how a 32-bit host talks to one), and it
 * keeps this file free of any assumption about a 64-bit MMIO store being a
 * single bus transaction on both architectures. */
static inline uint32_t r32(struct nvme_ctrl *c, uint32_t o) {
    return *(volatile uint32_t *)(c->regs + o);
}
static inline void w32(struct nvme_ctrl *c, uint32_t o, uint32_t v) {
    *(volatile uint32_t *)(c->regs + o) = v;
}
static inline uint64_t r64(struct nvme_ctrl *c, uint32_t o) {
    return (uint64_t)r32(c, o) | ((uint64_t)r32(c, o + 4) << 32);
}
static inline void w64(struct nvme_ctrl *c, uint32_t o, uint64_t v) {
    w32(c, o, (uint32_t)v);
    w32(c, o + 4, (uint32_t)(v >> 32));
}
static inline uint64_t dma(const volatile void *p) { return KV2P((uint64_t)(uintptr_t)p); }

static inline uint32_t le32_at(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t le64_at(const uint8_t *p) {
    return (uint64_t)le32_at(p) | ((uint64_t)le32_at(p + 4) << 32);
}

static void queue_init(struct nvme_ctrl *c, struct nvme_queue *q, uint16_t qid, uint16_t depth,
                       struct nvme_sqe *sq, struct nvme_cqe *cq)
{
    memset(sq, 0, sizeof(struct nvme_sqe) * depth);
    memset(cq, 0, sizeof(struct nvme_cqe) * depth);
    q->sq = sq;
    q->cq = cq;
    q->qid = qid;
    q->depth = depth;
    q->sq_tail = 0;
    q->cq_head = 0;
    /* A zeroed completion queue has every phase bit at 0, and the controller
     * writes its first pass with phase 1 -- so 1 is what "new" looks like until
     * the head wraps, at which point the meaning flips. That flip is the whole
     * completion protocol: there is no "valid" bit, only a phase that differs
     * from the stale entry's. */
    q->phase = 1;
    q->next_cid = 0;
    q->sq_db = (volatile uint32_t *)(c->regs + NVME_DOORBELLS + (2u * qid) * c->dstrd);
    q->cq_db = (volatile uint32_t *)(c->regs + NVME_DOORBELLS + (2u * qid + 1u) * c->dstrd);
}

/* Submit one command and spin for ITS completion. Returns the NVMe status
 * field (0 = success), or -1 if the controller never answered. The caller holds
 * the controller lock or is the single-threaded probe. */
static int nvme_cmd(struct nvme_ctrl *c, struct nvme_queue *q, struct nvme_sqe *cmd, uint32_t *dw0_out)
{
    uint16_t cid = q->next_cid++;
    cmd->cdw0 = (cmd->cdw0 & 0xFFFFu) | ((uint32_t)cid << 16);

    volatile struct nvme_sqe *slot = &q->sq[q->sq_tail];
    const uint32_t *src = (const uint32_t *)cmd;
    volatile uint32_t *dst = (volatile uint32_t *)slot;
    for (unsigned i = 0; i < sizeof(*cmd) / 4; i++)
        dst[i] = src[i];

    q->sq_tail = (uint16_t)((q->sq_tail + 1) % q->depth);
    /* The entry is ordinary memory the device will READ by DMA; the doorbell is
     * a device register. Without the barrier the doorbell can overtake the
     * entry, and the controller fetches a half-written command. */
    __sync_synchronize();
    *q->sq_db = q->sq_tail;

    for (uint64_t spins = 0; spins < NVME_SPIN_LIMIT; spins++) {
        volatile struct nvme_cqe *e = &q->cq[q->cq_head];
        uint16_t st = e->status;
        if ((st & 1u) == q->phase) {
            __sync_synchronize();       /* the rest of the entry, and any DMA'd data */
            uint16_t got = e->cid;
            uint32_t dw0 = e->dw0;
            q->cq_head++;
            if (q->cq_head == q->depth) {
                q->cq_head = 0;
                q->phase ^= 1u;
            }
            *q->cq_db = q->cq_head;
            if (got != cid) {
                /* One command in flight means the next completion is ours. A
                 * different ID is a completion we never waited for -- say so
                 * and keep looking rather than hand its status to this caller. */
                kprintf("nvme: stray completion cid %u on queue %u (waiting for %u)\n",
                        (unsigned)got, (unsigned)q->qid, (unsigned)cid);
                continue;
            }
            if (dw0_out)
                *dw0_out = dw0;
            return (int)(st >> 1);
        }
        arch_cpu_relax();
    }
    kprintf("nvme: queue %u: command %u (opcode %02x) never completed; CSTS=%08x\n",
            (unsigned)q->qid, (unsigned)cid, (unsigned)(cmd->cdw0 & 0xFF),
            (unsigned)r32(c, NVME_REG_CSTS));
    return -1;
}

/* Wait for CSTS.RDY to read `want`. False on timeout, on a vanished device
 * (all ones), or -- only while waiting for READY -- on a fatal status. */
static bool wait_ready(struct nvme_ctrl *c, bool want)
{
    for (uint64_t i = 0; i < NVME_SPIN_LIMIT; i++) {
        uint32_t s = r32(c, NVME_REG_CSTS);
        if (s == 0xFFFFFFFFu)
            return false;
        if (want && (s & CSTS_CFS))
            return false;
        if (((s & CSTS_RDY) != 0) == want)
            return true;
        arch_cpu_relax();
    }
    return false;
}

/* --- I/O ------------------------------------------------------------------ */

/* One read or write of `nlb` blocks between the bounce buffer and the medium.
 * Builds the PRPs for however many pages the transfer spans: one page needs
 * PRP1 alone, two put the second page in PRP2, and more make PRP2 point at a
 * LIST of the remaining pages. Getting the two-page case wrong (a list where a
 * page belongs) is the classic NVMe driver bug; it passes every 4 KiB test. */
static int nvme_xfer(struct nvme_ns *ns, bool write, uint64_t lba, uint32_t nlb)
{
    struct nvme_ctrl *c = ns->ctrl;
    uint32_t bytes = nlb * ns->lba_size;
    uint32_t pages = (bytes + PAGE - 1) / PAGE;
    uint64_t base  = dma(c->bounce);

    struct nvme_sqe cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.cdw0  = write ? NVM_WRITE : NVM_READ;
    cmd.nsid  = ns->nsid;
    cmd.prp1  = base;
    if (pages == 2) {
        cmd.prp2 = base + PAGE;
    } else if (pages > 2) {
        for (uint32_t i = 1; i < pages; i++)
            c->prp_list[i - 1] = base + (uint64_t)i * PAGE;
        cmd.prp2 = dma(c->prp_list);
    }
    cmd.cdw10 = (uint32_t)lba;
    cmd.cdw11 = (uint32_t)(lba >> 32);
    cmd.cdw12 = (nlb - 1u) & 0xFFFFu;       /* 0-based count */

    int st = nvme_cmd(c, &c->io, &cmd, 0);
    if (st != 0) {
        kprintf("nvme: %s: %s of %u block(s) at %llu failed, status %04x\n",
                ns->blkdev.name, write ? "write" : "read", (unsigned)nlb,
                (unsigned long long)lba, (unsigned)(st < 0 ? 0xFFFF : st));
        return -EMBK_EIO;
    }
    return EMBK_OK;
}

static int nvme_blk_read(struct embk_block_device *dev, uint64_t lba, uint32_t count, void *buf)
{
    struct nvme_ns   *ns  = (struct nvme_ns *)dev->driver_data;
    struct nvme_ctrl *c   = ns->ctrl;
    uint32_t          per = c->max_xfer / ns->lba_size;
    uint8_t          *out = (uint8_t *)buf;
    int rc = EMBK_OK;

    mutex_lock(&c->lock);
    if (!c->up) {                       /* after nvme_shutdown_all(): the drive */
        mutex_unlock(&c->lock);         /* has been told the power is going     */
        return -EMBK_EIO;
    }
    while (count) {
        uint32_t n = count < per ? count : per;
        rc = nvme_xfer(ns, false, lba, n);
        if (rc != EMBK_OK)
            break;
        memcpy(out, c->bounce, (size_t)n * ns->lba_size);
        out   += (size_t)n * ns->lba_size;
        lba   += n;
        count -= n;
    }
    mutex_unlock(&c->lock);
    return rc;
}

static int nvme_blk_write(struct embk_block_device *dev, uint64_t lba, uint32_t count, const void *buf)
{
    struct nvme_ns   *ns  = (struct nvme_ns *)dev->driver_data;
    struct nvme_ctrl *c   = ns->ctrl;
    uint32_t          per = c->max_xfer / ns->lba_size;
    const uint8_t    *in  = (const uint8_t *)buf;
    int rc = EMBK_OK;

    mutex_lock(&c->lock);
    if (!c->up) {
        mutex_unlock(&c->lock);
        return -EMBK_EIO;
    }
    while (count) {
        uint32_t n = count < per ? count : per;
        memcpy(c->bounce, in, (size_t)n * ns->lba_size);
        rc = nvme_xfer(ns, true, lba, n);
        if (rc != EMBK_OK)
            break;
        in    += (size_t)n * ns->lba_size;
        lba   += n;
        count -= n;
    }
    mutex_unlock(&c->lock);
    return rc;
}

/* The filesystem's write barrier. EMBKFS flushes the new tree before writing
 * the superblock that points at it; on a drive with a volatile write cache,
 * a "completed" write is only in that cache, and without this the superblock
 * could reach flash before the tree it names. Sent even when VWC reads 0: the
 * spec makes Flush a successful no-op then, and asking costs one round trip
 * against the risk of trusting a bit some firmware gets wrong. */
static int nvme_blk_flush(struct embk_block_device *dev)
{
    struct nvme_ns   *ns = (struct nvme_ns *)dev->driver_data;
    struct nvme_ctrl *c  = ns->ctrl;
    struct nvme_sqe   cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.cdw0 = NVM_FLUSH;
    cmd.nsid = ns->nsid;

    mutex_lock(&c->lock);
    int st = c->up ? nvme_cmd(c, &c->io, &cmd, 0) : -1;
    mutex_unlock(&c->lock);
    if (st != 0) {
        kprintf("nvme: %s: flush failed, status %04x\n", dev->name,
                (unsigned)(st < 0 ? 0xFFFF : st));
        return -EMBK_EIO;
    }
    return EMBK_OK;
}

/* --- bring-up ------------------------------------------------------------- */

static int identify(struct nvme_ctrl *c, uint32_t cns, uint32_t nsid)
{
    struct nvme_sqe cmd;
    memset(&cmd, 0, sizeof cmd);
    memset(c->ident, 0, sizeof c->ident);
    cmd.cdw0  = ADM_IDENTIFY;
    cmd.nsid  = nsid;
    cmd.prp1  = dma(c->ident);
    cmd.cdw10 = cns;
    return nvme_cmd(c, &c->admin, &cmd, 0);
}

/* Identify one namespace and register it if this driver can serve it. */
static void attach_namespace(struct nvme_ctrl *c, uint32_t nsid)
{
    if (c->nns >= NVME_MAX_NS)
        return;
    if (identify(c, CNS_NAMESPACE, nsid) != 0) {
        kprintf("nvme: identify namespace %u failed\n", (unsigned)nsid);
        return;
    }
    uint64_t nsze  = le64_at(c->ident + 0);
    uint8_t  flbas = c->ident[26];
    uint32_t lbaf  = le32_at(c->ident + 128 + 4u * (flbas & 0x0Fu));
    uint32_t lbads = (lbaf >> 16) & 0xFFu;
    uint32_t ms    = lbaf & 0xFFFFu;

    if (nsze == 0)
        return;                         /* allocated but inactive */
    if (ms != 0) {
        kprintf("nvme: namespace %u carries %u bytes of metadata per block -- skipped\n",
                (unsigned)nsid, (unsigned)ms);
        return;
    }
    if (lbads < 9 || lbads > 12) {
        kprintf("nvme: namespace %u has %u-byte blocks -- skipped (512..4096 supported)\n",
                (unsigned)nsid, (unsigned)(1u << lbads));
        return;
    }

    struct nvme_ns *ns = &c->ns[c->nns];
    memset(ns, 0, sizeof *ns);
    ns->ctrl     = c;
    ns->nsid     = nsid;
    ns->lba_size = 1u << lbads;
    ns->nblocks  = nsze;

    ns->blkdev.block_count        = nsze;
    ns->blkdev.block_size         = ns->lba_size;
    ns->blkdev.read               = nvme_blk_read;
    ns->blkdev.write              = nvme_blk_write;
    ns->blkdev.flush              = nvme_blk_flush;
    ns->blkdev.driver_data        = ns;
    ns->blkdev.dma_max_phys       = UINT64_MAX;
    ns->blkdev.needs_kernel_range = true;

    if (embk_block_register(&ns->blkdev) != 0) {
        kprintf("nvme: block layer refused namespace %u\n", (unsigned)nsid);
        return;
    }
    c->nns++;
    g_nns_total++;
    kprintf("nvme: %s = %s, namespace %u: %llu blocks of %u bytes (%llu MiB)%s\n",
            ns->blkdev.name, c->model, (unsigned)nsid,
            (unsigned long long)nsze, (unsigned)ns->lba_size,
            (unsigned long long)((nsze * ns->lba_size) >> 20),
            c->vwc ? ", volatile write cache" : "");
}

/* Reset the controller and bring it to "ready, with one I/O queue pair".
 * Separate from probing because it has TWO callers: nvme_attach() on the way
 * up, and nvme_restart_all() when a power-off that already sent the shutdown
 * notification turns out not to have taken the machine. The controller state
 * it builds is entirely ours -- queue addresses, entry sizes, interrupt mask --
 * so running it again from scratch is exactly as valid as the first time. */
static bool nvme_enable(struct nvme_ctrl *c)
{
    /* Reset: an enabled controller -- firmware used it to boot, or it came up
     * enabled -- has queues at addresses we did not choose. */
    uint32_t cc = r32(c, NVME_REG_CC);
    if (cc & CC_EN)
        w32(c, NVME_REG_CC, cc & ~CC_EN);
    if (!wait_ready(c, false)) {
        kprintf("nvme: controller did not stop (CSTS=%08x)\n", (unsigned)r32(c, NVME_REG_CSTS));
        return false;
    }

    queue_init(c, &c->admin, 0, c->depth, c->asq, c->acq);
    w32(c, NVME_REG_AQA, ((uint32_t)(c->depth - 1u) << 16) | (uint32_t)(c->depth - 1u));
    w64(c, NVME_REG_ASQ, dma(c->asq));
    w64(c, NVME_REG_ACQ, dma(c->acq));

    w32(c, NVME_REG_CC, CC_CSS_NVM | CC_MPS_4K | CC_AMS_RR | CC_IOSQES | CC_IOCQES | CC_EN);
    if (!wait_ready(c, true)) {
        kprintf("nvme: controller did not become ready (CSTS=%08x)\n",
                (unsigned)r32(c, NVME_REG_CSTS));
        return false;
    }
    /* Mask every pin-based/MSI vector. After enable, not before: the mask is
     * part of the running controller's state. */
    w32(c, NVME_REG_INTMS, 0xFFFFFFFFu);

    /* --- one I/O queue pair ----------------------------------------------- */
    queue_init(c, &c->io, 1, c->depth, c->iosq, c->iocq);

    struct nvme_sqe cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.cdw0  = ADM_CREATE_CQ;
    cmd.prp1  = dma(c->iocq);
    cmd.cdw10 = ((uint32_t)(c->depth - 1u) << 16) | 1u;   /* size, QID 1 */
    cmd.cdw11 = 1u;                                     /* contiguous; IEN = 0 */
    int st = nvme_cmd(c, &c->admin, &cmd, 0);
    if (st != 0) {
        kprintf("nvme: create I/O completion queue failed, status %04x\n", (unsigned)(st & 0xFFFF));
        return false;
    }
    memset(&cmd, 0, sizeof cmd);
    cmd.cdw0  = ADM_CREATE_SQ;
    cmd.prp1  = dma(c->iosq);
    cmd.cdw10 = ((uint32_t)(c->depth - 1u) << 16) | 1u;   /* size, QID 1 */
    cmd.cdw11 = (1u << 16) | 1u;                        /* CQ 1; contiguous */
    st = nvme_cmd(c, &c->admin, &cmd, 0);
    if (st != 0) {
        kprintf("nvme: create I/O submission queue failed, status %04x\n", (unsigned)(st & 0xFFFF));
        return false;
    }
    return true;
}

static bool nvme_attach(struct nvme_ctrl *c, const struct pci_device *d)
{
    mutex_init(&c->lock);

    /* PCI command register: MEMORY SPACE (bit 1) so BAR0 decodes, BUS MASTER
     * (bit 2) so the controller can DMA, INTx DISABLE (bit 10) because we poll.
     * All three set here, explicitly, rather than trusting firmware to have
     * left memory decoding on: it usually has, on a disk it booted from, and
     * "usually" is not a property of a second drive. */
    uint32_t cmdreg = arch_pci_cfg_read32(d->bus, d->device, d->function, PCI_COMMAND);
    cmdreg |= (1u << 1) | (1u << 2) | (1u << 10);
    arch_pci_cfg_write32(d->bus, d->device, d->function, PCI_COMMAND, cmdreg);

    struct pci_bar bar = pci_read_bar(d->bus, d->device, d->function, 0);
    if (!bar.valid || !bar.is_mmio || bar.size < 0x2000) {
        kprintf("nvme: %02x:%02x.%x: BAR0 unusable (valid %d, mmio %d, size %llu)\n",
                d->bus, d->device, d->function, (int)bar.valid, (int)bar.is_mmio,
                (unsigned long long)bar.size);
        return false;
    }
    c->regs = (volatile uint8_t *)(uintptr_t)vmm_map_mmio(bar.address, bar.size);
    if (!c->regs) {
        kprintf("nvme: could not map BAR0\n");
        return false;
    }

    uint64_t cap    = r64(c, NVME_REG_CAP);
    uint32_t vs     = r32(c, NVME_REG_VS);
    uint32_t mqes   = (uint32_t)(cap & 0xFFFFu) + 1u;
    uint32_t mpsmin = (uint32_t)((cap >> 48) & 0xFu);
    bool     nvmcss = ((cap >> 37) & 1u) != 0;
    c->dstrd = 4u << ((cap >> 32) & 0xFu);

    if (cap == 0xFFFFFFFFFFFFFFFFULL) {
        kprintf("nvme: controller reads all ones -- not responding\n");
        return false;
    }
    if (mpsmin != 0) {
        kprintf("nvme: minimum memory page is %u KiB; this driver uses 4 KiB -- refused\n",
                (unsigned)(4u << mpsmin));
        return false;
    }
    if (!nvmcss) {
        kprintf("nvme: controller does not support the NVM command set -- refused\n");
        return false;
    }
    uint16_t depth = QDEPTH;
    if (depth > mqes)
        depth = (uint16_t)mqes;
    c->depth = depth;
    /* The doorbells for queues 0 and 1 must be inside the BAR we mapped. */
    if (NVME_DOORBELLS + 4u * c->dstrd > bar.size) {
        kprintf("nvme: doorbell stride %u does not fit in a %llu-byte BAR -- refused\n",
                (unsigned)c->dstrd, (unsigned long long)bar.size);
        return false;
    }

    if (!nvme_enable(c))
        return false;

    /* --- who is this ------------------------------------------------------ */
    if (identify(c, CNS_CONTROLLER, 0) != 0) {
        kprintf("nvme: identify controller failed\n");
        return false;
    }
    memcpy(c->model, c->ident + 24, 40);
    c->model[40] = 0;
    for (int i = 39; i >= 0 && (c->model[i] == ' ' || c->model[i] == 0); i--)
        c->model[i] = 0;
    uint8_t  mdts = c->ident[77];
    uint32_t nn   = le32_at(c->ident + 516);
    c->vwc = (c->ident[525] & 1u) != 0;

    /* MDTS: the largest transfer, as a power of two of the minimum page (4 KiB
     * here, enforced above). 0 means no limit. Clamped to the bounce buffer. */
    c->max_xfer = BOUNCE_SZ;
    if (mdts != 0 && mdts < 32) {
        uint64_t lim = (uint64_t)PAGE << mdts;
        if (lim < c->max_xfer)
            c->max_xfer = (uint32_t)lim;
    }

    c->up = true;

    kprintf("nvme: %02x:%02x.%x \"%s\", NVMe %u.%u, queue depth %u, "
            "%u KiB per command, %u namespace(s) declared\n",
            d->bus, d->device, d->function, c->model,
            (unsigned)(vs >> 16), (unsigned)((vs >> 8) & 0xFF),
            (unsigned)depth, (unsigned)(c->max_xfer / 1024), (unsigned)nn);

    /* --- namespaces --------------------------------------------------------
     * The ACTIVE namespace list (CNS 2, NVMe 1.1+) rather than counting 1..NN:
     * NN is the highest ID the controller could have, not the ones it does,
     * and on a controller that declares 1024 the dumb loop is 1024 identify
     * commands to find one namespace. A 1.0 controller rejects CNS 2; it gets
     * the loop, bounded. */
    if (identify(c, CNS_ACTIVE_NSID, 0) == 0) {
        uint32_t ids[NVME_MAX_NS * 4];
        int found = 0;
        for (int i = 0; i < (int)(sizeof ids / sizeof ids[0]); i++) {
            uint32_t id = le32_at(c->ident + 4 * i);
            if (id == 0)
                break;                  /* the list ends at the first zero */
            ids[found++] = id;
        }
        for (int i = 0; i < found; i++)
            attach_namespace(c, ids[i]);
    } else {
        uint32_t top = nn < 16 ? nn : 16;
        for (uint32_t id = 1; id <= top; id++)
            attach_namespace(c, id);
    }
    if (c->nns == 0)
        kprintf("nvme: \"%s\" has no namespace this driver can serve\n", c->model);
    return true;
}

bool nvme_init(void)
{
    for (uint32_t i = 0; i < pci_devices_count() && g_nctrl < NVME_MAX_CTRL; i++) {
        const struct pci_device *d = pci_get_device(i);
        if (!d || d->class_code != PCI_CLASS_STORAGE ||
            d->subclass != PCI_SUBCLASS_NVM || d->prog_if != PCI_PROGIF_NVME)
            continue;
        if (nvme_attach(&g_ctrl[g_nctrl], d))
            g_nctrl++;
    }
    if (g_nctrl == 0) {
        kprintf("nvme: no controller\n");
        return false;
    }
    return g_nns_total > 0;
}

/* The normal shutdown notification for one controller. Caller holds its lock.
 * True when the controller reports shutdown processing complete. */
static bool ctrl_shutdown(struct nvme_ctrl *c)
{
    uint32_t cc = r32(c, NVME_REG_CC);
    w32(c, NVME_REG_CC, (cc & ~CC_SHN_MASK) | CC_SHN_NORMAL);
    for (uint64_t s = 0; s < NVME_SPIN_LIMIT; s++) {
        if ((r32(c, NVME_REG_CSTS) & CSTS_SHST_MASK) == CSTS_SHST_DONE)
            return true;
        arch_cpu_relax();
    }
    return false;
}

void nvme_shutdown_all(void)
{
    for (int i = 0; i < g_nctrl; i++) {
        struct nvme_ctrl *c = &g_ctrl[i];
        if (!c->up)
            continue;
        mutex_lock(&c->lock);
        bool done = ctrl_shutdown(c);
        c->up = false;                  /* no command after a shutdown notification */
        mutex_unlock(&c->lock);
        kprintf("nvme: \"%s\": shutdown %s\n", c->model, done ? "complete" : "NOT acknowledged");
    }
}

/* Undo nvme_shutdown_all(). Only for the case it exists for: a power-off or
 * reboot that sent every drive its shutdown notification and then did NOT take
 * the machine -- which, on real hardware with no ACPI interpreter yet, is what
 * power-off does (kernel/arch/x86_64/power/power_x86.c says so plainly). The
 * machine keeps running, so its disks have to keep working: a controller that
 * has processed a shutdown must be reset before it accepts another command,
 * and that reset is exactly nvme_enable(). */
void nvme_restart_all(void)
{
    for (int i = 0; i < g_nctrl; i++) {
        struct nvme_ctrl *c = &g_ctrl[i];
        if (c->up)
            continue;
        mutex_lock(&c->lock);
        bool ok = nvme_enable(c);
        c->up = ok;
        mutex_unlock(&c->lock);
        kprintf("nvme: \"%s\": %s\n", c->model,
                ok ? "restarted -- the power-off did not happen"
                   : "could NOT be restarted; its disks will fail until reboot");
    }
}

/* The recovery path, exercised without losing the machine: notify shutdown and
 * restart the controller that owns namespace `index`, holding its lock across
 * both, so no other thread's I/O lands in the gap and fails. The two halves are
 * the functions nvme_shutdown_all() and nvme_restart_all() use -- the point is
 * to test THOSE, on a machine where power-off always succeeds and therefore
 * never lets the real restart run. Returns 0 if the controller came back. */
int nvme_cycle_namespace_controller(int index)
{
    for (int i = 0; i < g_nctrl; i++) {
        struct nvme_ctrl *c = &g_ctrl[i];
        if (index >= c->nns) {
            index -= c->nns;
            continue;
        }
        mutex_lock(&c->lock);
        bool acked = ctrl_shutdown(c);
        bool back  = nvme_enable(c);
        c->up = back;
        mutex_unlock(&c->lock);
        return (acked && back) ? 0 : -EMBK_EIO;
    }
    return -EMBK_ENOENT;
}

int nvme_namespace_count(void)
{
    return g_nns_total;
}

struct embk_block_device *nvme_namespace_blkdev(int index)
{
    for (int i = 0; i < g_nctrl; i++) {
        if (index < g_ctrl[i].nns)
            return &g_ctrl[i].ns[index].blkdev;
        index -= g_ctrl[i].nns;
    }
    return 0;
}

uint32_t nvme_namespace_max_xfer(int index)
{
    for (int i = 0; i < g_nctrl; i++) {
        if (index < g_ctrl[i].nns)
            return g_ctrl[i].max_xfer;
        index -= g_ctrl[i].nns;
    }
    return 0;
}
