/* kernel/drivers/storage/megasas.c -- LSI MegaRAID SAS.
 *
 * This is the RAID controller in a very large number of servers, and the
 * interface is not like anything else in this tree. AHCI and NVMe give you
 * registers and a command ring. This gives you a FIRMWARE with a mailbox: you
 * build a command FRAME in memory, hand the controller its physical address
 * through a doorbell, and the firmware posts the frame's context value back
 * into a reply queue when it is done.
 *
 * WHAT THE DRIVER SEES IS NOT WHAT IS PLUGGED IN. The whole point of a RAID
 * controller is that the disks behind it are combined into LOGICAL DRIVES by
 * firmware, and those are what the operating system gets. This driver talks
 * to logical drives (MFI_CMD_LD_SCSI_IO) and not to physical ones -- asking
 * for the physical disks behind a configured array is how you corrupt it.
 *
 * The commands inside the frames are ordinary SCSI, built by
 * kernel/block/scsi.c, the same as USB mass storage and virtio-scsi.
 *
 * THREE THINGS THAT MUST HAPPEN IN ORDER AND HAVE NO ERROR IF THEY DO NOT:
 *
 *   1. Wait for the firmware to reach its READY state. It is not ready when
 *      the machine is; it boots.
 *   2. Send MFI_CMD_INIT describing the reply queue BEFORE any other frame.
 *      A frame submitted first is accepted and never answered.
 *   3. Submit by writing the HIGH half of the frame address first. The low
 *      write is what triggers the firmware, so writing them the other way
 *      round submits an address with a zero top half.
 */
#include <stdint.h>
#include <stddef.h>

#include "include/types.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/errno.h"
#include "drivers/bus/pci.h"
#include "drivers/storage/megasas.h"
#include "block/block.h"
#include "block/scsi.h"
#include "mm/vmm.h"
#include "mm/pmm.h"

/* Registers, in the memory-mapped window. */
#define MFI_OMSG0  0x18    /* firmware state, mirrored                */
#define MFI_IDB    0x20    /* inbound doorbell                        */
#define MFI_OSTS   0x30    /* outbound interrupt status               */
#define MFI_OMSK   0x34    /* outbound interrupt mask                 */
#define MFI_IQP    0x40    /* inbound queue port, 32-bit frame address */
/* THE 64-BIT QUEUE PORT IS NOT NEXT TO THE 32-BIT ONE. It is at 0xC0/0xC4,
 * and writing what looks like its high half at 0x44 lands on a register that
 * does not exist -- silently, because an unimplemented register accepts a
 * write and forgets it. */
#define MFI_IQPL   0xC0
#define MFI_IQPH   0xC4
/* THE FIRMWARE STATE IS MIRRORED IN TWO PLACES and the family does not agree
 * on which. The outbound scratch pad at 0xFC is what the vendor's driver uses
 * on some parts; MFI_OMSG0 at 0x18 carries the same value and is implemented
 * everywhere, including on parts where 0xFC reads back as zero -- which is
 * indistinguishable from "the firmware has not booted". Read both and take
 * whichever answers. */
#define MFI_OSP    0xFC

#define MFI_STATE_MASK        0xF0000000u
#define MFI_STATE_READY       0xB0000000u
#define MFI_STATE_OPERATIONAL 0xC0000000u
#define MFI_STATE_FAULT       0xF0000000u

#define MFI_CMD_INIT       0x00
#define MFI_CMD_LD_SCSI_IO 0x03
#define MFI_CMD_DCMD       0x05

/* THE FLAG THAT MAKES A POLLED DRIVER WORK. Bit 0 asks the firmware NOT to
 * post this frame's completion in the reply queue -- it writes the status
 * into the frame instead, and the driver watches that. Which is exactly what
 * a driver with no interrupt handler needs, and is what the vendor's own
 * polled path does. */
#define MFI_FRAME_DONT_POST 0x0001
#define MFI_FRAME_SGL64     0x0002
#define MFI_FRAME_SENSE64   0x0004
#define MFI_FRAME_DIR_WRITE 0x0008
#define MFI_FRAME_DIR_READ  0x0010

#define MFI_STAT_OK       0x00
#define MFI_STAT_INVALID  0xFF   /* "the firmware has not touched this yet" */

#define MEGASAS_MAX_LD 8
#define MEGASAS_XFER   65536
#define MEGASAS_REPLIES 16

/* The frame header every command starts with. Little-endian throughout, which
 * matches both architectures this kernel runs on. */
struct mfi_header {
    uint8_t  cmd;
    uint8_t  sense_len;
    uint8_t  cmd_status;
    uint8_t  scsi_status;
    uint8_t  target_id;
    uint8_t  lun_id;
    uint8_t  cdb_len;
    uint8_t  sge_count;
    uint32_t context;
    uint32_t pad0;
    uint16_t flags;
    uint16_t timeout;
    uint32_t data_len;
} __attribute__((packed));

struct mfi_sge64 { uint64_t addr; uint32_t len; } __attribute__((packed));

/* A SCSI pass-through frame: the header, where to put sense data, the CDB,
 * and one scatter-gather element. */
struct mfi_io_frame {
    struct mfi_header hdr;
    uint32_t sense_addr_lo;
    uint32_t sense_addr_hi;
    uint8_t  cdb[16];
    struct mfi_sge64 sgl;
    uint8_t  pad[8];
} __attribute__((packed));

/* The initialisation frame points at this, which describes the reply queue. */
struct mfi_init_qinfo {
    uint32_t flags;
    uint32_t rq_entries;
    uint64_t rq_addr;
    uint64_t pi_addr;      /* producer index */
    uint64_t ci_addr;      /* consumer index */
} __attribute__((packed));

struct mfi_init_frame {
    struct mfi_header hdr;
    uint32_t qinfo_new_addr_lo;
    uint32_t qinfo_new_addr_hi;
    uint32_t qinfo_old_addr_lo;
    uint32_t qinfo_old_addr_hi;
    uint32_t reserved[6];
} __attribute__((packed));

static volatile uint8_t *g_regs;
static bool g_up;

static struct mfi_io_frame   g_frame  __attribute__((aligned(64)));
static struct mfi_init_frame g_init   __attribute__((aligned(64)));
static struct mfi_init_qinfo g_qinfo  __attribute__((aligned(64)));
static uint8_t  g_sense[96]           __attribute__((aligned(64)));
static uint8_t  g_data[MEGASAS_XFER]  __attribute__((aligned(64)));
static uint32_t g_replies[MEGASAS_REPLIES] __attribute__((aligned(64)));
static volatile uint32_t g_producer   __attribute__((aligned(64)));
static volatile uint32_t g_consumer   __attribute__((aligned(64)));

struct ms_ld {
    bool used;
    uint8_t target;
    struct scsi_device sd;
    struct embk_block_device blk;
};
static struct ms_ld g_lds[MEGASAS_MAX_LD];

static inline uint32_t rr(uint32_t o) { return *(volatile uint32_t *)(g_regs + o); }
static inline void     rw(uint32_t o, uint32_t v) { *(volatile uint32_t *)(g_regs + o) = v; }
static inline uint64_t dma(const volatile void *p) { return KV2P((uint64_t)(uintptr_t)p); }

/* Hand a frame to the firmware and wait for it to come back.
 *
 * COMPLETION IS READ OUT OF THE FRAME, not out of the reply queue. The queue
 * is the interrupt-driven path: the firmware posts the frame's context there
 * and raises an interrupt. A driver that polls asks for the other one -- the
 * DONT_POST flag, with the status byte pre-set to a value the firmware never
 * writes -- and watches that byte. The reply queue is still registered,
 * because initialisation requires describing one, but nothing here depends on
 * it. That is the vendor driver's own polled path and not an approximation of
 * it. */
static bool mfi_submit(void *frame, struct mfi_header *hdr, uint32_t frame_words) {
    uint64_t pa = dma(frame);

    hdr->cmd_status = MFI_STAT_INVALID;
    hdr->flags |= MFI_FRAME_DONT_POST;
    __sync_synchronize();

    if (pa >> 32) {
        /* THE HIGH HALF FIRST: the low write is what triggers the firmware. */
        rw(MFI_IQPH, (uint32_t)(pa >> 32));
        rw(MFI_IQPL, (uint32_t)(pa & ~0x1FULL) | (frame_words << 1));
    } else {
        rw(MFI_IQP, (uint32_t)(pa & ~0x1FULL) | (frame_words << 1));
    }

    for (int spin = 0; spin < 20000000; spin++) {
        if (hdr->cmd_status != MFI_STAT_INVALID) return true;
        __asm__ volatile("" ::: "memory");
    }
    kprintf("megasas: a frame was never answered\n");
    return false;
}

/* One SCSI command to a logical drive. */
static bool ms_exec(void *ctx, const uint8_t *cdb, uint8_t cdb_len,
                    void *data, uint32_t len, bool to_dev) {
    struct ms_ld *ld = ctx;
    if (!g_up || len > MEGASAS_XFER) return false;

    if (to_dev && data && len) memcpy(g_data, data, len);
    memset(&g_frame, 0, sizeof g_frame);
    memset(g_sense, 0, sizeof g_sense);

    g_frame.hdr.cmd = MFI_CMD_LD_SCSI_IO;
    g_frame.hdr.sense_len = sizeof g_sense;

    g_frame.hdr.target_id = ld->target;
    g_frame.hdr.lun_id = 0;
    g_frame.hdr.cdb_len = cdb_len;
    g_frame.hdr.sge_count = len ? 1 : 0;
    g_frame.hdr.context = 1;
    g_frame.hdr.flags = MFI_FRAME_SGL64 | MFI_FRAME_SENSE64 |
                        (len ? (to_dev ? MFI_FRAME_DIR_WRITE : MFI_FRAME_DIR_READ) : 0);
    g_frame.hdr.timeout = 0;
    g_frame.hdr.data_len = len;
    uint64_t spa = dma(g_sense);
    g_frame.sense_addr_lo = (uint32_t)spa;
    g_frame.sense_addr_hi = (uint32_t)(spa >> 32);
    memcpy(g_frame.cdb, cdb, cdb_len > 16 ? 16 : cdb_len);
    if (len) {
        g_frame.sgl.addr = dma(g_data);
        g_frame.sgl.len = len;
    }

    if (!mfi_submit(&g_frame, &g_frame.hdr, sizeof g_frame / 16)) return false;

    /* TWO STATUSES AGAIN, as with virtio-scsi: cmd_status is the firmware's
     * ("I could not do this") and scsi_status is the drive's. */
    if (g_frame.hdr.cmd_status != MFI_STAT_OK) return false;
    if (g_frame.hdr.scsi_status != 0) return false;
    if (!to_dev && data && len) memcpy(data, g_data, len);
    return true;
}

static int ms_read(struct embk_block_device *b, uint64_t lba,
                   uint32_t count, void *buf) {
    struct ms_ld *ld = b->driver_data;
    return scsi_read(&ld->sd, lba, count, buf, MEGASAS_XFER / ld->sd.block_size);
}
static int ms_write(struct embk_block_device *b, uint64_t lba,
                    uint32_t count, const void *buf) {
    struct ms_ld *ld = b->driver_data;
    return scsi_write(&ld->sd, lba, count, buf, MEGASAS_XFER / ld->sd.block_size);
}

static const struct pci_device *megasas_find(void) {
    uint32_t n = pci_devices_count();
    for (uint32_t i = 0; i < n; i++) {
        const struct pci_device *d = pci_get_device(i);
        if (!d || d->vendor_id != 0x1000) continue;
        switch (d->device_id) {
        case 0x0411:   /* SAS1078, QEMU's `megasas`      */
        case 0x0060:   /* SAS1078DE                      */
        case 0x0073:   /* SAS2108, QEMU's `megasas-gen2` */
        case 0x0079:
        case 0x005B: case 0x005D: case 0x005F:
            return d;
        default: break;
        }
    }
    return 0;
}

bool megasas_init(void) {
    if (g_up) return true;
    const struct pci_device *d = megasas_find();
    if (!d) return false;

    /* THE REGISTER WINDOW IS NOT ALWAYS BAR0. This controller family puts its
     * memory window at BAR0 or BAR1 depending on whether MSI-X is configured,
     * and there is an I/O window as well. Taking the first memory BAR that
     * exists is what the vendor's own driver does. */
    struct pci_bar bar = { 0 };
    for (int i = 0; i < 3; i++) {
        struct pci_bar b = pci_read_bar(d->bus, d->device, d->function, (uint8_t)i);
        if (b.valid && b.is_mmio && b.size >= 0x100) { bar = b; break; }
    }
    if (!bar.valid) { kprintf("megasas: no memory window\n"); return false; }

    g_regs = (volatile uint8_t *)(uintptr_t)vmm_map_mmio(bar.address,
                                                         bar.size ? bar.size : 0x4000);
    if (!g_regs) return false;
    pci_enable_bus_mastering(d->bus, d->device, d->function);

    /* WAIT FOR THE FIRMWARE. It is not ready when the machine is. */
    uint32_t state = 0;
    for (int i = 0; i < 200; i++) {
        uint32_t a = rr(MFI_OMSG0) & MFI_STATE_MASK;
        uint32_t b = rr(MFI_OSP) & MFI_STATE_MASK;
        state = a ? a : b;
        if (state == MFI_STATE_READY || state == MFI_STATE_OPERATIONAL) break;
        if (state == MFI_STATE_FAULT) {
            kprintf("megasas: firmware reports a fault\n");
            return false;
        }
        for (volatile int j = 0; j < 200000; j++) { }
    }
    if (state != MFI_STATE_READY && state != MFI_STATE_OPERATIONAL) {
        kprintf("megasas: firmware never became ready (OMSG0 %08x, OSP %08x)\n",
                rr(MFI_OMSG0), rr(MFI_OSP));
        return false;
    }

    rw(MFI_OMSK, 0xFFFFFFFFu);          /* mask interrupts: this driver polls */

    /* Describe the reply queue, then send MFI_CMD_INIT pointing at it. */
    memset(g_replies, 0, sizeof g_replies);
    g_producer = 0;
    g_consumer = 0;
    memset(&g_qinfo, 0, sizeof g_qinfo);
    g_qinfo.rq_entries = MEGASAS_REPLIES;
    g_qinfo.rq_addr = dma(g_replies);
    g_qinfo.pi_addr = dma(&g_producer);
    g_qinfo.ci_addr = dma(&g_consumer);

    memset(&g_init, 0, sizeof g_init);
    g_init.hdr.cmd = MFI_CMD_INIT;
    g_init.hdr.context = 0;
    g_init.hdr.data_len = sizeof g_qinfo;
    uint64_t qpa = dma(&g_qinfo);
    g_init.qinfo_new_addr_lo = (uint32_t)qpa;
    g_init.qinfo_new_addr_hi = (uint32_t)(qpa >> 32);

    if (!mfi_submit(&g_init, &g_init.hdr, sizeof g_init / 16) ||
        g_init.hdr.cmd_status != MFI_STAT_OK) {
        kprintf("megasas: initialisation frame returned %u\n",
                g_init.hdr.cmd_status);
        return false;
    }
    g_up = true;

    /* Each logical drive that answers INQUIRY becomes a block device. */
    int found = 0;
    for (uint32_t t = 0; t < MEGASAS_MAX_LD; t++) {
        struct ms_ld *ld = &g_lds[t];
        memset(ld, 0, sizeof *ld);
        ld->used = true;
        ld->target = (uint8_t)t;
        ld->sd.exec = ms_exec;
        ld->sd.ctx = ld;
        if (!scsi_probe(&ld->sd)) { ld->used = false; continue; }
        if (ld->sd.is_optical) {
            kprintf("megasas: LD %u is an optical drive\n", t);
        }

        ld->blk.block_count = ld->sd.blocks;
        ld->blk.block_size  = ld->sd.block_size;
        ld->blk.read  = ms_read;
        ld->blk.write = ms_write;
        ld->blk.driver_data = ld;
        ld->blk.dma_max_phys = ~0ULL;
        ld->blk.needs_kernel_range = true;
        if (embk_block_register(&ld->blk) != EMBK_OK) { ld->used = false; continue; }
        kprintf("megasas: %s = LD %u, %s %s, %llu x %u B\n", ld->blk.name, t,
                ld->sd.vendor, ld->sd.product,
                (unsigned long long)ld->sd.blocks, ld->sd.block_size);
        found++;
    }
    if (!found) kprintf("megasas: controller up, no logical drives\n");
    return true;
}

bool megasas_present(void) { return g_up; }
uint32_t megasas_ld_count(void) {
    uint32_t n = 0;
    for (int i = 0; i < MEGASAS_MAX_LD; i++) if (g_lds[i].used) n++;
    return n;
}
