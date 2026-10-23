/* NVM Express 1.4: controller registers (§3), SQ/CQ entries (§4), Identify
 * (§5.15), Create I/O queues (§5.3/5.4), NVM Read/Write/Flush (§6).
 * Register/Identify layouts cross-checked with EDK II IndustryStandard/Nvme.h.
 * One outstanding command per controller, with static, page-aligned DMA memory.
 * No MSI dependency: polling remains usable during storage bring-up. */
#include "drivers/storage/nvme.h"
#include "drivers/bus/pci.h"
#include "block/block.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "include/kstring.h"
#include "include/kprintf.h"
#include "include/errno.h"
#include "process/ksync.h"
#include "drivers/timer/timer.h"

#define NVME_CONTROLLERS 2
#define NVME_NAMESPACES  8
#define NVME_QDEPTH     16
struct nvme_command {
    uint32_t op_cid, nsid, reserved[2];
    uint64_t metadata, prp1, prp2;
    uint32_t dword[6];
};
struct nvme_completion {
    uint32_t result, reserved;
    uint16_t sq_head, sq_id, cid, status;
};
_Static_assert(sizeof(struct nvme_command) == 64, "NVMe SQ entry");
_Static_assert(sizeof(struct nvme_completion) == 16, "NVMe CQ entry");
struct nvme_queue {
    struct nvme_command sq[NVME_QDEPTH] __attribute__((aligned(4096)));
    volatile struct nvme_completion cq[NVME_QDEPTH] __attribute__((aligned(4096)));
    uint16_t tail, head, cid;
    uint8_t phase;
};
struct nvme_controller;
struct nvme_namespace {
    struct embk_block_device block;
    struct nvme_controller *ctrl;
    uint32_t id, native_size;
};
struct nvme_controller {
    volatile uint8_t *regs;
    uint32_t stride, depth;
    uint64_t ready_timeout;
    bool online;
    struct mutex lock;
    struct nvme_queue admin, io;
    uint8_t bounce[4096] __attribute__((aligned(4096)));
    struct nvme_namespace ns[NVME_NAMESPACES];
};
static struct nvme_controller controllers[NVME_CONTROLLERS];

static uint32_t rd(struct nvme_controller *c, uint32_t off) {
    return *(volatile uint32_t *)(c->regs + off);
}
static void wr(struct nvme_controller *c, uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(c->regs + off) = val;
}
static bool ready(struct nvme_controller *c, bool enabled) {
    uint64_t start = time_get_us();
    do {
        uint32_t s = rd(c, 0x1c);
        if ((s & 1) == (unsigned)enabled) return true;
        if (enabled && (s & 2)) return false;
        __asm__ volatile("pause");
    } while (time_get_us() - start < c->ready_timeout);
    return false;
}
static void fail_controller(struct nvme_controller *c) {
    c->online = false;
    wr(c, 0x14, 0);
    /* Static queue/bounce pages are never freed or reused after a timeout,
     * including if broken hardware fails to stop DMA after CC.EN is cleared. */
    kprintf("NVMe: controller disabled after command timeout/protocol failure\n");
}
static int submit(struct nvme_controller *c, unsigned qid, struct nvme_command *cmd) {
    if (!c->online) return -EMBK_ENODEV;
    struct nvme_queue *q = qid ? &c->io : &c->admin;
    uint16_t cid = ++q->cid;
    cmd->op_cid = (cmd->op_cid & 0xffff) | ((uint32_t)cid << 16);
    q->sq[q->tail] = *cmd;
    q->tail = (q->tail + 1) % c->depth;
    __asm__ volatile("sfence" ::: "memory");
    wr(c, 0x1000 + 2 * qid * c->stride, q->tail);
    uint64_t start = time_get_us();
    volatile struct nvme_completion *entry = &q->cq[q->head];
    while ((entry->status & 1) != q->phase) {
        if ((rd(c, 0x1c) & 2) || time_get_us() - start >= 30000000ULL) {
            fail_controller(c);
            return -EMBK_EIO;
        }
        __asm__ volatile("pause");
    }
    __asm__ volatile("lfence" ::: "memory");
    uint16_t status = entry->status;
    if (entry->cid != cid || entry->sq_id != qid) {
        fail_controller(c);
        return -EMBK_EIO;
    }
    q->head++;
    if (q->head == c->depth) { q->head = 0; q->phase ^= 1; }
    wr(c, 0x1000 + (2 * qid + 1) * c->stride, q->head);
    if ((status >> 1) & 0x7ff) {
        kprintf("NVMe: command %x namespace %u status %x\n",
                cmd->op_cid & 0xff, cmd->nsid, status >> 1);
        return -EMBK_EIO;
    }
    return EMBK_OK;
}
static int identify(struct nvme_controller *c, uint32_t nsid, unsigned cns) {
    memset(c->bounce, 0, sizeof c->bounce);
    struct nvme_command cmd = { .op_cid = 6, .nsid = nsid,
                               .prp1 = KV2P(c->bounce), .dword = { cns } };
    return submit(c, 0, &cmd);
}
static int native_io(struct nvme_namespace *ns, uint64_t lba, uint32_t n, bool write) {
    struct nvme_command cmd = { .op_cid = write ? 1 : 2, .nsid = ns->id,
        .prp1 = KV2P(ns->ctrl->bounce),
        .dword = { (uint32_t)lba, (uint32_t)(lba >> 32), n - 1 } };
    return submit(ns->ctrl, 1, &cmd);
}
static int transfer(struct embk_block_device *dev, uint64_t lba, uint32_t n,
                    void *buffer, bool write) {
    if (!dev || !buffer) return -EMBK_EINVAL;
    if (lba > dev->block_count || n > dev->block_count - lba) return -EMBK_EINVAL;
    struct nvme_namespace *ns = dev->driver_data;
    struct nvme_controller *c = ns->ctrl;
    uint8_t *p = buffer;
    uint32_t ratio = ns->native_size / 512;
    int rc = EMBK_OK;
    mutex_lock(&c->lock);
    while (n) {
        uint32_t off = lba % ratio;
        uint32_t logical, units;
        if (!off && n >= ratio) {
            units = n / ratio;
            if (units > 4096 / ns->native_size) units = 4096 / ns->native_size;
            logical = units * ratio;
        } else {
            units = 1;
            logical = ratio - off;
            if (logical > n) logical = n;
        }
        /* Expose 512-byte sectors even on 4Kn SSDs. Partial native writes
         * preserve neighboring sectors via RMW under the controller lock. */
        if (!write || off || logical != units * ratio) {
            rc = native_io(ns, lba / ratio, units, false);
            if (rc != EMBK_OK) break;
        }
        if (write) {
            memcpy(c->bounce + off * 512, p, logical * 512);
            rc = native_io(ns, lba / ratio, units, true);
            if (rc != EMBK_OK) break;
        } else memcpy(p, c->bounce + off * 512, logical * 512);
        p += logical * 512; lba += logical; n -= logical;
    }
    mutex_unlock(&c->lock);
    return rc;
}
static int read_blocks(struct embk_block_device *d, uint64_t l, uint32_t n, void *b) {
    return transfer(d, l, n, b, false);
}
static int write_blocks(struct embk_block_device *d, uint64_t l, uint32_t n, const void *b) {
    return transfer(d, l, n, (void *)b, true);
}
static int flush_blocks(struct embk_block_device *dev) {
    if (!dev) return -EMBK_EINVAL;
    struct nvme_namespace *ns = dev->driver_data;
    struct nvme_command cmd = { .op_cid = 0, .nsid = ns->id };
    mutex_lock(&ns->ctrl->lock);
    int rc = submit(ns->ctrl, 1, &cmd);
    mutex_unlock(&ns->ctrl->lock);
    return rc;
}
static bool init_controller(struct nvme_controller *c, const struct pci_device *pci) {
    struct pci_bar bar = pci_read_bar(pci->bus, pci->device, pci->function, 0);
    if (!bar.valid || !bar.is_mmio || bar.size < 0x1010) return false;
    pci_write16(pci->bus, pci->device, pci->function, PCI_COMMAND,
        pci_read16(pci->bus, pci->device, pci->function, PCI_COMMAND) | 6);
    c->regs = (volatile uint8_t *)vmm_map_mmio(bar.address, bar.size);
    if (!c->regs) return false;
    uint64_t cap = *(volatile uint64_t *)c->regs;
    c->stride = 4U << ((cap >> 32) & 15);
    unsigned to = (cap >> 24) & 255;
    c->ready_timeout = (uint64_t)(to ? to : 1) * 500000;
    if (!(cap & (1ULL << 37)) || ((cap >> 48) & 15)
        || bar.size < 0x1000ULL + 4 * c->stride) return false;
    c->depth = (cap & 0xffff) + 1;
    if (c->depth > NVME_QDEPTH) c->depth = NVME_QDEPTH;
    if (c->depth < 2) return false;
    wr(c, 0x14, 0);
    if (!ready(c, false)) return false;
    wr(c, 0x0c, 0xffffffff);  /* polling: mask interrupts */
    c->admin.phase = c->io.phase = 1;
    wr(c, 0x24, (c->depth - 1) | ((c->depth - 1) << 16));
    *(volatile uint64_t *)(c->regs + 0x28) = KV2P(c->admin.sq);
    *(volatile uint64_t *)(c->regs + 0x30) = KV2P(c->admin.cq);
    __asm__ volatile("sfence" ::: "memory");
    wr(c, 0x14, 1 | (6 << 16) | (4 << 20));
    if (!ready(c, true)) { wr(c, 0x14, 0); return false; }
    c->online = true;
    if (identify(c, 0, 1) != EMBK_OK) return false;
    char model[41]; memcpy(model, c->bounce + 24, 40); model[40] = 0;
    char serial[21];memcpy(serial,c->bounce+4,20);serial[20]=0;
    for (int i = 39; i >= 0 && model[i] == ' '; i--) model[i] = 0;
    kprintf("NVMe: PCI %x:%x.%x %s, queue depth %u\n",
            pci->bus, pci->device, pci->function, model, c->depth);
    struct nvme_command cmd = { .op_cid = 9, .dword = { 7, 0 } };
    if (submit(c, 0, &cmd) != EMBK_OK) return false;
    memset(&cmd, 0, sizeof cmd);
    cmd.op_cid = 5; cmd.prp1 = KV2P(c->io.cq);
    cmd.dword[0] = 1 | ((c->depth - 1) << 16); cmd.dword[1] = 1;
    if (submit(c, 0, &cmd) != EMBK_OK) return false;
    cmd.op_cid = 1; cmd.prp1 = KV2P(c->io.sq); cmd.dword[1] = (1 << 16) | 1;
    if (submit(c, 0, &cmd) != EMBK_OK) return false;
    if (identify(c, 0, 2) != EMBK_OK) return false;
    uint32_t ids[NVME_NAMESPACES];
    memcpy(ids, c->bounce, sizeof ids);
    for (unsigned i = 0; i < NVME_NAMESPACES && ids[i]; i++) {
        if (identify(c, ids[i], 0) != EMBK_OK) continue;
        uint64_t blocks; memcpy(&blocks, c->bounce, sizeof blocks);
        unsigned format = c->bounce[26] & 15;
        unsigned bits = c->bounce[128 + 4 * format + 2];
        uint16_t meta; memcpy(&meta, c->bounce + 128 + 4 * format, 2);
        if (!blocks || (c->bounce[26] & 0x70) || format > c->bounce[25]
            || meta || bits < 9 || bits > 12 || blocks > (UINT64_MAX >> (bits - 9))) {
            kprintf("NVMe: namespace %u unsupported LBA/metadata format\n", ids[i]);
            continue;
        }
        struct nvme_namespace *ns = &c->ns[i];
        ns->ctrl = c; ns->id = ids[i]; ns->native_size = 1U << bits;
        ns->block.block_count = blocks << (bits - 9);
        ns->block.block_size = 512;
        ns->block.read = read_blocks; ns->block.write = write_blocks;
        ns->block.flush = flush_blocks; ns->block.driver_data = ns;
        /* Our DMA bounce page accepts arbitrary caller buffers. */
        ns->block.dma_max_phys = UINT64_MAX;
        ns->block.needs_kernel_range = false;
        if (embk_block_register(&ns->block) == EMBK_OK) {
            embk_block_set_identity(&ns->block,"NVMe",model,serial);
            kprintf("NVMe: namespace %u -> %s, native sector %u, logical sectors %llu\n",
                ids[i], ns->block.name, ns->native_size,
                (unsigned long long)ns->block.block_count);
        }
    }
    return true;
}
void nvme_init(void) {
    if (!tsc_get_freq_hz()) { kprintf("NVMe: no calibrated timeout clock\n"); return; }
    unsigned used = 0;
    for (uint32_t i = 0; i < pci_devices_count(); i++) {
        const struct pci_device *p = pci_get_device(i);
        if (p->class_code != 1 || p->subclass != 8 || p->prog_if != 2) continue;
        if (used == NVME_CONTROLLERS) { kprintf("NVMe: controller limit reached\n"); break; }
        struct nvme_controller *c = &controllers[used++];
        if (!init_controller(c, p)) {
            if (c->regs) { c->online = false; wr(c, 0x14, 0); }
            kprintf("NVMe: controller initialization failed\n");
        }
    }
}
