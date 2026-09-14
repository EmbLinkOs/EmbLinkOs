/* kernel/drivers/storage/pvscsi.c -- VMware's paravirtual SCSI controller.
 *
 * The SCSI equivalent of virtio: no hardware is being emulated, so there are
 * no FIFOs, no doorbell timing and no chip errata -- just two rings in guest
 * memory and a register to kick. It is what a VMware virtual machine uses for
 * its disks, and the reason to drive it is the same as the reason to drive
 * virtio: an OS that only runs well under one hypervisor is not portable.
 *
 * THE RINGS ARE DESCRIBED IN PAGE FRAME NUMBERS, not addresses -- every
 * pointer handed to this controller is a physical address shifted right by
 * twelve, and each ring must therefore be page-aligned and a whole number of
 * pages long. Passing an address where a frame number is expected points the
 * controller four thousand times too far into memory.
 *
 * The commands inside the requests are ordinary SCSI from kernel/block/scsi.c.
 */
#include <stdint.h>
#include <stddef.h>

#include "include/types.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/errno.h"
#include "drivers/bus/pci.h"
#include "drivers/storage/pvscsi.h"
#include "block/block.h"
#include "block/scsi.h"
#include "mm/vmm.h"
#include "mm/pmm.h"

#define PVSCSI_VENDOR 0x15AD
#define PVSCSI_DEVICE 0x07C0

#define PV_REG_COMMAND        0x0000
#define PV_REG_COMMAND_DATA   0x0004
#define PV_REG_COMMAND_STATUS 0x0008
#define PV_REG_INTR_STATUS    0x100C
#define PV_REG_INTR_MASK      0x2010
#define PV_REG_KICK_NON_RW_IO 0x3014
#define PV_REG_KICK_RW_IO     0x4018

#define PV_CMD_ADAPTER_RESET 1
#define PV_CMD_SETUP_RINGS   3

#define PV_FLAG_DIR_NONE     (1u << 2)
#define PV_FLAG_DIR_TOHOST   (1u << 3)
#define PV_FLAG_DIR_TODEVICE (1u << 4)

#define PV_MAX_TARGETS 8
#define PV_XFER        65536
#define PV_RING_PAGES  1
#define PV_REQ_ENTRIES 32     /* 4096 / 128 */
#define PV_CMP_ENTRIES 128    /* 4096 / 32  */

/* The shared state page: where each side of each ring has got to. The padding
 * is the specification's and must be exact -- the message ring's indices are
 * at a fixed offset and a short struct puts them on top of something else. */
struct pv_rings_state {
    uint32_t req_prod;
    uint32_t req_cons;
    uint32_t req_entries_log2;
    uint32_t cmp_prod;
    uint32_t cmp_cons;
    uint32_t cmp_entries_log2;
    uint8_t  pad[104];
    uint32_t msg_prod;
    uint32_t msg_cons;
    uint32_t msg_entries_log2;
} __attribute__((packed));

struct pv_req_desc {
    uint64_t context;
    uint64_t data_addr;
    uint64_t data_len;
    uint64_t sense_addr;
    uint32_t sense_len;
    uint32_t flags;
    uint8_t  cdb[16];
    uint8_t  cdb_len;
    uint8_t  lun[8];
    uint8_t  tag;
    uint8_t  bus;
    uint8_t  target;
    uint8_t  vcpu_hint;
    uint8_t  unused[59];
} __attribute__((packed));

struct pv_cmp_desc {
    uint64_t context;
    uint64_t data_len;
    uint32_t sense_len;
    uint16_t host_status;
    uint16_t scsi_status;
    uint32_t pad[2];
} __attribute__((packed));

/* The command that sets the rings up. Its fields are page frame numbers. */
struct pv_setup_rings {
    uint32_t req_pages;
    uint32_t cmp_pages;
    uint64_t state_ppn;
    uint64_t req_ppn[32];
    uint64_t cmp_ppn[32];
} __attribute__((packed));

static volatile uint8_t *g_regs;
static bool g_up;

/* PAGE-ALIGNED, because the controller is told about them in page frame
 * numbers and cannot express anything finer. */
static struct pv_rings_state g_state    __attribute__((aligned(4096)));
static struct pv_req_desc    g_req_ring[PV_REQ_ENTRIES] __attribute__((aligned(4096)));
static struct pv_cmp_desc    g_cmp_ring[PV_CMP_ENTRIES] __attribute__((aligned(4096)));
static uint8_t g_sense[96] __attribute__((aligned(64)));
static uint8_t g_data[PV_XFER] __attribute__((aligned(64)));

struct pv_target {
    bool used;
    uint8_t target;
    struct scsi_device sd;
    struct embk_block_device blk;
};
static struct pv_target g_targets[PV_MAX_TARGETS];

static inline uint32_t rr(uint32_t o) { return *(volatile uint32_t *)(g_regs + o); }
static inline void     rw(uint32_t o, uint32_t v) { *(volatile uint32_t *)(g_regs + o) = v; }
static inline uint64_t dma(const volatile void *p) { return KV2P((uint64_t)(uintptr_t)p); }
static inline uint64_t ppn(const volatile void *p) { return dma(p) >> 12; }

/* Send one controller command: the opcode, then its payload a dword at a time
 * through a single data register. */
static void pv_command(uint32_t cmd, const void *data, uint32_t bytes) {
    rw(PV_REG_COMMAND, cmd);
    const uint32_t *w = data;
    for (uint32_t i = 0; i < bytes / 4; i++)
        rw(PV_REG_COMMAND_DATA, w[i]);
}

static bool pv_exec(void *ctx, const uint8_t *cdb, uint8_t cdb_len,
                    void *data, uint32_t len, bool to_dev) {
    struct pv_target *t = ctx;
    if (!g_up || len > PV_XFER) return false;

    if (to_dev && data && len) memcpy(g_data, data, len);
    memset(g_sense, 0, sizeof g_sense);

    uint32_t idx = g_state.req_prod % PV_REQ_ENTRIES;
    struct pv_req_desc *r = &g_req_ring[idx];
    memset(r, 0, sizeof *r);
    r->context = 1;
    r->data_addr = len ? dma(g_data) : 0;
    r->data_len = len;
    r->sense_addr = dma(g_sense);
    r->sense_len = sizeof g_sense;
    r->flags = len ? (to_dev ? PV_FLAG_DIR_TODEVICE : PV_FLAG_DIR_TOHOST)
                   : PV_FLAG_DIR_NONE;
    memcpy(r->cdb, cdb, cdb_len > 16 ? 16 : cdb_len);
    r->cdb_len = cdb_len;
    r->bus = 0;
    r->target = t->target;
    /* LUN 0, in the same eight-byte form every SCSI transport uses. */
    r->lun[1] = 0;

    uint32_t cmp_start = g_state.cmp_prod;
    __sync_synchronize();
    g_state.req_prod = g_state.req_prod + 1;
    __sync_synchronize();
    rw(PV_REG_KICK_RW_IO, 0);

    for (int spin = 0; spin < 20000000; spin++) {
        if (g_state.cmp_prod != cmp_start) {
            struct pv_cmp_desc *c = &g_cmp_ring[cmp_start % PV_CMP_ENTRIES];
            uint16_t host = c->host_status, scsi = c->scsi_status;
            __sync_synchronize();
            g_state.cmp_cons = g_state.cmp_prod;
            rw(PV_REG_INTR_STATUS, rr(PV_REG_INTR_STATUS));   /* acknowledge */
            /* TWO STATUSES, as everywhere else: the controller's view and
             * the target's. Host status 0 is BTSTAT_SUCCESS. */
            if (host != 0 || scsi != 0) return false;
            if (!to_dev && data && len) memcpy(data, g_data, len);
            return true;
        }
        __asm__ volatile("" ::: "memory");
    }
    kprintf("pvscsi: target %u did not answer command 0x%02x\n", t->target, cdb[0]);
    return false;
}

static int pv_read(struct embk_block_device *b, uint64_t lba,
                   uint32_t count, void *buf) {
    struct pv_target *t = b->driver_data;
    return scsi_read(&t->sd, lba, count, buf, PV_XFER / t->sd.block_size);
}
static int pv_write(struct embk_block_device *b, uint64_t lba,
                    uint32_t count, const void *buf) {
    struct pv_target *t = b->driver_data;
    return scsi_write(&t->sd, lba, count, buf, PV_XFER / t->sd.block_size);
}

bool pvscsi_init(void) {
    if (g_up) return true;
    const struct pci_device *d = NULL;
    uint32_t n = pci_devices_count();
    for (uint32_t i = 0; i < n; i++) {
        const struct pci_device *c = pci_get_device(i);
        if (c && c->vendor_id == PVSCSI_VENDOR && c->device_id == PVSCSI_DEVICE) {
            d = c; break;
        }
    }
    if (!d) return false;

    struct pci_bar bar = { 0 };
    for (int i = 0; i < 3; i++) {
        struct pci_bar b = pci_read_bar(d->bus, d->device, d->function, (uint8_t)i);
        if (b.valid && b.is_mmio && b.size >= 0x5000) { bar = b; break; }
    }
    if (!bar.valid) { kprintf("pvscsi: no register window\n"); return false; }
    g_regs = (volatile uint8_t *)(uintptr_t)vmm_map_mmio(bar.address, bar.size);
    if (!g_regs) return false;
    pci_enable_bus_mastering(d->bus, d->device, d->function);

    rw(PV_REG_COMMAND, PV_CMD_ADAPTER_RESET);
    rw(PV_REG_INTR_MASK, 0);            /* this driver polls */

    memset((void *)&g_state, 0, sizeof g_state);
    memset(g_req_ring, 0, sizeof g_req_ring);
    memset(g_cmp_ring, 0, sizeof g_cmp_ring);
    /* THE SIZES ARE GIVEN AS LOGARITHMS, and the controller uses them to mask
     * the producer index. A log that does not match the ring actually handed
     * over wraps writes onto the wrong descriptors. */
    g_state.req_entries_log2 = 5;       /* 32 entries of 128 bytes = one page */
    g_state.cmp_entries_log2 = 7;       /* 128 entries of 32 bytes = one page */

    struct pv_setup_rings setup;
    memset(&setup, 0, sizeof setup);
    setup.req_pages = PV_RING_PAGES;
    setup.cmp_pages = PV_RING_PAGES;
    setup.state_ppn = ppn(&g_state);
    setup.req_ppn[0] = ppn(g_req_ring);
    setup.cmp_ppn[0] = ppn(g_cmp_ring);
    /* THE WHOLE STRUCTURE, ALL 528 BYTES OF IT, including the thirty-one
     * unused page slots in each array. The controller counts the dwords
     * arriving through the data register and runs the command when it has
     * received as many as the structure is long -- so sending only the fields
     * that are filled in means the command never executes at all. No error:
     * the controller is simply still waiting for the rest. */
    pv_command(PV_CMD_SETUP_RINGS, &setup, (uint32_t)sizeof setup);
    g_up = true;

    int found = 0;
    for (uint32_t tn = 0; tn < PV_MAX_TARGETS; tn++) {
        struct pv_target *t = &g_targets[tn];
        memset(t, 0, sizeof *t);
        t->used = true;
        t->target = (uint8_t)tn;
        t->sd.exec = pv_exec;
        t->sd.ctx = t;
        if (!scsi_probe(&t->sd)) { t->used = false; continue; }

        t->blk.block_count = t->sd.blocks;
        t->blk.block_size  = t->sd.block_size;
        t->blk.read  = pv_read;
        t->blk.write = pv_write;
        t->blk.driver_data = t;
        t->blk.dma_max_phys = ~0ULL;
        t->blk.needs_kernel_range = true;
        if (embk_block_register(&t->blk) != EMBK_OK) { t->used = false; continue; }
        kprintf("pvscsi: %s = target %u, %s %s, %llu x %u B\n", t->blk.name, tn,
                t->sd.vendor, t->sd.product,
                (unsigned long long)t->sd.blocks, t->sd.block_size);
        found++;
    }
    if (!found) kprintf("pvscsi: controller up, no targets answered\n");
    return true;
}

bool pvscsi_present(void) { return g_up; }
uint32_t pvscsi_target_count(void) {
    uint32_t n = 0;
    for (int i = 0; i < PV_MAX_TARGETS; i++) if (g_targets[i].used) n++;
    return n;
}
