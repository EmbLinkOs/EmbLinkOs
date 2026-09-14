/* kernel/drivers/storage/mptsas.c -- LSI Fusion-MPT (SAS1068 and relatives).
 *
 * The third programming model in this directory and the third completely
 * different one. MegaRAID is a mailbox holding one frame at a time; PVSCSI is
 * a pair of rings; this is MESSAGE PASSING: the driver writes the physical
 * address of a request message into a hardware FIFO, and the controller
 * answers by pushing a value into a second FIFO. Nothing is polled in the
 * message itself and there is no producer index -- the FIFO either has
 * something in it or reads back as all ones.
 *
 * TWO KINDS OF ANSWER COME OUT OF THAT FIFO AND THEY ARE TOLD APART BY ONE
 * BIT. With bit 31 clear the value IS the request's context: the command
 * succeeded and there is nothing more to read. With bit 31 set the value is
 * the address of a REPLY FRAME, shifted right by one, holding the status of a
 * command that did not go perfectly. A driver that assumes one kind works
 * until the first short read and then dereferences a context value as a
 * pointer.
 *
 * REPLY FRAMES MUST BE GIVEN TO THE CONTROLLER BEFORE THEY ARE NEEDED. The
 * driver posts empty ones into the same FIFO it reads answers out of --
 * writing to it donates, reading from it receives. A controller with no free
 * reply frame and something to report drops the report.
 *
 * Initialisation does not go through the FIFO at all: it goes through a
 * DOORBELL, one 32-bit word at a time, because the FIFO does not work until
 * initialisation has told the controller where memory is.
 */
#include <stdint.h>
#include <stddef.h>

#include "include/types.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/errno.h"
#include "drivers/bus/pci.h"
#include "drivers/storage/mptsas.h"
#include "block/block.h"
#include "block/scsi.h"
#include "mm/vmm.h"
#include "mm/pmm.h"

#define MPI_DOORBELL        0x00
#define MPI_WRITE_SEQUENCE  0x04
#define MPI_DIAGNOSTIC      0x08
#define MPI_INTR_STATUS     0x30
#define MPI_INTR_MASK       0x34
#define MPI_REQUEST_QUEUE   0x40
#define MPI_REPLY_QUEUE     0x44

#define MPI_DOORBELL_FUNCTION_SHIFT  24
#define MPI_DOORBELL_ADD_DWORDS_SHIFT 16
#define MPI_IOC_STATE_MASK        0xF0000000u
#define MPI_IOC_STATE_READY       0x10000000u
#define MPI_IOC_STATE_OPERATIONAL 0x20000000u
#define MPI_IOC_STATE_FAULT       0x40000000u

#define MPI_HIS_DOORBELL_INTERRUPT 0x00000001u
#define MPI_HIS_REPLY_INTERRUPT    0x00000008u

#define MPI_FUNCTION_SCSI_IO_REQUEST 0x00
#define MPI_FUNCTION_IOC_INIT        0x02
#define MPI_FUNCTION_PORT_ENABLE     0x06
#define MPI_FUNCTION_HANDSHAKE       0x42

#define MPI_SCSIIO_CONTROL_WRITE 0x01000000u
#define MPI_SCSIIO_CONTROL_READ  0x02000000u

/* Scatter-gather element flags, in the top byte of the length word. */
#define SGE_LAST_ELEMENT  0x80u
#define SGE_END_OF_BUFFER 0x40u
#define SGE_SIMPLE        0x10u
#define SGE_HOST_TO_IOC   0x04u   /* the direction bit: set means writing */
#define SGE_64BIT_ADDRESS 0x02u
#define SGE_END_OF_LIST   0x01u

#define MPI_ADDRESS_REPLY_A_BIT 0x80000000u

#define MPT_MAX_TARGETS 8
#define MPT_XFER        65536
#define MPT_REPLY_FRAMES 4
#define MPT_REPLY_BYTES  128

struct mpt_scsi_io {
    uint8_t  target_id;
    uint8_t  bus;
    uint8_t  chain_offset;
    uint8_t  function;
    uint8_t  cdb_length;
    uint8_t  sense_length;
    uint8_t  reserved;
    uint8_t  msg_flags;
    uint32_t msg_context;
    uint8_t  lun[8];
    uint32_t control;
    uint8_t  cdb[16];
    uint32_t data_length;
    uint32_t sense_low_addr;
    /* One simple 64-bit scatter-gather element, inline. */
    uint32_t sge_flags_length;
    uint32_t sge_addr_low;
    uint32_t sge_addr_high;
} __attribute__((packed));

struct mpt_ioc_init {
    uint8_t  who_init;
    uint8_t  reserved;
    uint8_t  chain_offset;
    uint8_t  function;
    uint8_t  flags;
    uint8_t  max_devices;
    uint8_t  max_buses;
    uint8_t  msg_flags;
    uint32_t msg_context;
    uint16_t reply_frame_size;
    uint16_t reserved1;
    uint32_t host_mfa_high_addr;
    uint32_t sense_high_addr;
    uint32_t reply_fifo_host_signalling_addr;
    uint32_t host_page_sge[3];
    uint16_t msg_version;
    uint16_t header_version;
} __attribute__((packed));

struct mpt_port_enable {
    uint8_t  reserved[2];
    uint8_t  chain_offset;
    uint8_t  function;
    uint8_t  reserved1[3];
    uint8_t  msg_flags;
    uint32_t msg_context;
} __attribute__((packed));

/* The header every reply frame starts with. */
struct mpt_reply {
    uint8_t  target_id;
    uint8_t  bus;
    uint8_t  msg_length;
    uint8_t  function;
    uint8_t  cdb_length;
    uint8_t  sense_length;
    uint8_t  reserved;
    uint8_t  msg_flags;
    uint32_t msg_context;
    uint8_t  scsi_status;
    uint8_t  scsi_state;
    uint16_t ioc_status;
    uint32_t ioc_log_info;
    uint32_t transfer_count;
} __attribute__((packed));

static volatile uint8_t *g_regs;
static bool g_up;

static struct mpt_scsi_io g_io      __attribute__((aligned(64)));
static uint8_t g_sense[96]          __attribute__((aligned(64)));
static uint8_t g_data[MPT_XFER]     __attribute__((aligned(64)));
static uint8_t g_reply_frames[MPT_REPLY_FRAMES][MPT_REPLY_BYTES] __attribute__((aligned(64)));

struct mpt_target {
    bool used;
    uint8_t target;
    struct scsi_device sd;
    struct embk_block_device blk;
};
static struct mpt_target g_targets[MPT_MAX_TARGETS];

static inline uint32_t rr(uint32_t o) { return *(volatile uint32_t *)(g_regs + o); }
static inline void     rw(uint32_t o, uint32_t v) { *(volatile uint32_t *)(g_regs + o) = v; }
static inline uint64_t dma(const volatile void *p) { return KV2P((uint64_t)(uintptr_t)p); }

static bool wait_doorbell_int(void) {
    for (int i = 0; i < 5000000; i++) {
        if (rr(MPI_INTR_STATUS) & MPI_HIS_DOORBELL_INTERRUPT) return true;
        __asm__ volatile("" ::: "memory");
    }
    return false;
}

/* Send one message through the doorbell, a word at a time, and drain the
 * reply the same way. Used only before the FIFOs are usable. */
static bool mpt_handshake(const void *msg, uint32_t bytes) {
    const uint32_t *w = msg;
    uint32_t dwords = bytes / 4;

    rw(MPI_INTR_STATUS, 0);
    rw(MPI_DOORBELL, ((uint32_t)MPI_FUNCTION_HANDSHAKE << MPI_DOORBELL_FUNCTION_SHIFT) |
                     (dwords << MPI_DOORBELL_ADD_DWORDS_SHIFT));
    if (!wait_doorbell_int()) { kprintf("mptsas: doorbell never answered\n"); return false; }
    rw(MPI_INTR_STATUS, 0);

    for (uint32_t i = 0; i < dwords; i++) rw(MPI_DOORBELL, w[i]);

    /* The answer comes back SIXTEEN BITS AT A TIME, and each word is released
     * by clearing the interrupt. Nothing here needs the reply's contents --
     * the controller's state register says whether initialisation worked --
     * but it has to be drained or the doorbell stays in its read state and
     * every later message is refused. */
    for (int i = 0; i < 32; i++) {
        if (!wait_doorbell_int()) break;
        (void)rr(MPI_DOORBELL);
        rw(MPI_INTR_STATUS, 0);
    }
    return true;
}

/* One SCSI command through the request FIFO. */
static bool mpt_exec(void *ctx, const uint8_t *cdb, uint8_t cdb_len,
                     void *data, uint32_t len, bool to_dev) {
    struct mpt_target *t = ctx;
    if (!g_up || len > MPT_XFER) return false;

    if (to_dev && data && len) memcpy(g_data, data, len);
    memset(g_sense, 0, sizeof g_sense);
    memset(&g_io, 0, sizeof g_io);

    g_io.target_id = t->target;
    g_io.bus = 0;
    g_io.function = MPI_FUNCTION_SCSI_IO_REQUEST;
    g_io.cdb_length = cdb_len;
    g_io.sense_length = sizeof g_sense;
    g_io.msg_context = 0x1000u + t->target;
    /* LUN 0 in the usual eight-byte form. */
    g_io.lun[0] = 0;
    g_io.control = len ? (to_dev ? MPI_SCSIIO_CONTROL_WRITE : MPI_SCSIIO_CONTROL_READ) : 0;
    memcpy(g_io.cdb, cdb, cdb_len > 16 ? 16 : cdb_len);
    g_io.data_length = len;
    g_io.sense_low_addr = (uint32_t)dma(g_sense);

    uint32_t flags = SGE_LAST_ELEMENT | SGE_END_OF_BUFFER | SGE_SIMPLE |
                     SGE_64BIT_ADDRESS | SGE_END_OF_LIST |
                     (to_dev ? SGE_HOST_TO_IOC : 0u);
    uint64_t dpa = dma(g_data);
    g_io.sge_flags_length = (flags << 24) | (len & 0x00FFFFFFu);
    g_io.sge_addr_low  = (uint32_t)dpa;
    g_io.sge_addr_high = (uint32_t)(dpa >> 32);

    __sync_synchronize();
    rw(MPI_REQUEST_QUEUE, (uint32_t)dma(&g_io));

    for (int spin = 0; spin < 20000000; spin++) {
        uint32_t v = rr(MPI_REPLY_QUEUE);
        if (v == 0xFFFFFFFFu) { __asm__ volatile("" ::: "memory"); continue; }

        rw(MPI_INTR_STATUS, 0);
        if (!(v & MPI_ADDRESS_REPLY_A_BIT)) {
            /* A context reply: it worked, and there is nothing to read. */
            if (!to_dev && data && len) memcpy(data, g_data, len);
            return true;
        }

        /* AN ADDRESS REPLY. The address was shifted right by one to make room
         * for the flag bit, so it has to be shifted back before it is a
         * pointer to anything. */
        uint64_t rpa = (uint64_t)(v & ~MPI_ADDRESS_REPLY_A_BIT) << 1;
        struct mpt_reply *rep = NULL;
        for (int i = 0; i < MPT_REPLY_FRAMES; i++)
            if (dma(g_reply_frames[i]) == rpa) rep = (struct mpt_reply *)g_reply_frames[i];

        bool ok = false;
        if (!rep) {
            /* A frame the firmware still held from whoever drove this
             * controller before we did. It is the controller's to recycle,
             * so it goes back into the pool; the command it belonged to is
             * not ours to interpret. */
        } else {
            /* IOCStatus 0 with a SCSI status of 0 is success even when a
             * frame was posted -- the controller posts one for a short
             * transfer too, which INQUIRY produces legitimately. */
            uint16_t st = rep->ioc_status & 0x7FFF;
            ok = st == 0 && rep->scsi_status == 0;
            /* 0x43 is "there is no device at that address", which is the
             * answer to a probe and not a fault. Every other status is worth
             * a line. */
            if (!ok && st != 0x0043)
                kprintf("mptsas: target %u command 0x%02x: ioc %04x scsi %02x\n",
                        t->target, cdb[0], rep->ioc_status, rep->scsi_status);
        }
        /* Give the frame back whatever it said. */
        if (rep) rw(MPI_REPLY_QUEUE, (uint32_t)rpa);
        if (ok && !to_dev && data && len) memcpy(data, g_data, len);
        return ok;
    }
    kprintf("mptsas: target %u did not answer command 0x%02x\n", t->target, cdb[0]);
    return false;
}

static int mpt_read(struct embk_block_device *b, uint64_t lba,
                    uint32_t count, void *buf) {
    struct mpt_target *t = b->driver_data;
    return scsi_read(&t->sd, lba, count, buf, MPT_XFER / t->sd.block_size);
}
static int mpt_write(struct embk_block_device *b, uint64_t lba,
                     uint32_t count, const void *buf) {
    struct mpt_target *t = b->driver_data;
    return scsi_write(&t->sd, lba, count, buf, MPT_XFER / t->sd.block_size);
}

bool mptsas_init(void) {
    if (g_up) return true;
    const struct pci_device *d = NULL;
    uint32_t n = pci_devices_count();
    for (uint32_t i = 0; i < n; i++) {
        const struct pci_device *c = pci_get_device(i);
        if (!c || c->vendor_id != 0x1000) continue;
        /* SAS1068 and SAS1068E, which is what QEMU emulates, plus the 1064
         * parts that share the interface. */
        if (c->device_id == 0x0054 || c->device_id == 0x0058 ||
            c->device_id == 0x0050 || c->device_id == 0x0056) { d = c; break; }
    }
    if (!d) return false;

    /* THE MEMORY WINDOW IS BAR1. BAR0 is the same registers in I/O space and
     * BAR3 is a diagnostic window into the controller's own memory -- mapping
     * that one and writing a doorbell into it does nothing at all. */
    struct pci_bar bar = pci_read_bar(d->bus, d->device, d->function, 1);
    if (!bar.valid || !bar.is_mmio) {
        kprintf("mptsas: BAR1 is not a memory window\n");
        return false;
    }
    g_regs = (volatile uint8_t *)(uintptr_t)vmm_map_mmio(bar.address,
                                                        bar.size ? bar.size : 0x4000);
    if (!g_regs) return false;
    pci_enable_bus_mastering(d->bus, d->device, d->function);
    rw(MPI_INTR_MASK, 0xFFFFFFFFu);      /* this driver polls */

    uint32_t state = rr(MPI_DOORBELL) & MPI_IOC_STATE_MASK;
    if (state == MPI_IOC_STATE_FAULT) {
        kprintf("mptsas: controller is in its fault state\n");
        return false;
    }

    /* Tell the controller where memory is. HostMfaHighAddr and
     * SenseBufferHighAddr are the TOP halves of every address this driver
     * will later send as 32 bits; both are zero because the rings and
     * buffers are in the kernel image, which is loaded low. */
    struct mpt_ioc_init init;
    memset(&init, 0, sizeof init);
    init.who_init = 2;                   /* the host driver */
    init.function = MPI_FUNCTION_IOC_INIT;
    init.max_devices = 8;
    init.max_buses = 1;
    init.reply_frame_size = MPT_REPLY_BYTES;
    init.host_mfa_high_addr = 0;
    init.sense_high_addr = 0;
    init.msg_version = 0x0105;
    init.header_version = 0x1e00;
    if (!mpt_handshake(&init, sizeof init)) return false;

    state = rr(MPI_DOORBELL) & MPI_IOC_STATE_MASK;
    if (state != MPI_IOC_STATE_OPERATIONAL) {
        kprintf("mptsas: still in state %08x after initialisation\n", state);
        return false;
    }
    g_up = true;

    /* EMPTY THE FIFO FIRST. The firmware may already have been driven by
     * whatever ran before this kernel -- on a PC that is the BIOS, looking
     * for something to boot -- and its leftover answers are still queued,
     * pointing at frames in memory we do not own. */
    for (int i = 0; i < 64; i++) {
        uint32_t v = rr(MPI_REPLY_QUEUE);
        if (v == 0xFFFFFFFFu) break;
    }
    rw(MPI_INTR_STATUS, 0);

    /* DONATE THE REPLY FRAMES. Writing an address into the reply FIFO gives
     * the controller a frame; reading from it takes an answer out. Without
     * this every reply that needs a frame is silently dropped. */
    for (int i = 0; i < MPT_REPLY_FRAMES; i++)
        rw(MPI_REPLY_QUEUE, (uint32_t)dma(g_reply_frames[i]));

    /* Enable the port, so the controller starts reporting its devices. */
    struct mpt_port_enable pe;
    memset(&pe, 0, sizeof pe);
    pe.function = MPI_FUNCTION_PORT_ENABLE;
    pe.msg_context = 0x2000;
    rw(MPI_REQUEST_QUEUE, (uint32_t)dma(&pe));
    for (int spin = 0; spin < 5000000; spin++) {
        uint32_t v = rr(MPI_REPLY_QUEUE);
        if (v != 0xFFFFFFFFu) {
            rw(MPI_INTR_STATUS, 0);
            if (v & MPI_ADDRESS_REPLY_A_BIT)
                rw(MPI_REPLY_QUEUE, (uint32_t)((uint64_t)(v & ~MPI_ADDRESS_REPLY_A_BIT) << 1));
            break;
        }
        __asm__ volatile("" ::: "memory");
    }

    int found = 0;
    for (uint32_t tn = 0; tn < MPT_MAX_TARGETS; tn++) {
        struct mpt_target *t = &g_targets[tn];
        memset(t, 0, sizeof *t);
        t->used = true;
        t->target = (uint8_t)tn;
        t->sd.exec = mpt_exec;
        t->sd.ctx = t;
        if (!scsi_probe(&t->sd)) { t->used = false; continue; }

        t->blk.block_count = t->sd.blocks;
        t->blk.block_size  = t->sd.block_size;
        t->blk.read  = mpt_read;
        t->blk.write = mpt_write;
        t->blk.driver_data = t;
        t->blk.dma_max_phys = ~0ULL;
        t->blk.needs_kernel_range = true;
        if (embk_block_register(&t->blk) != EMBK_OK) { t->used = false; continue; }
        kprintf("mptsas: %s = target %u, %s %s, %llu x %u B\n", t->blk.name, tn,
                t->sd.vendor, t->sd.product,
                (unsigned long long)t->sd.blocks, t->sd.block_size);
        found++;
    }
    if (!found) kprintf("mptsas: controller up, no targets answered\n");
    return true;
}

bool mptsas_present(void) { return g_up; }
uint32_t mptsas_target_count(void) {
    uint32_t n = 0;
    for (int i = 0; i < MPT_MAX_TARGETS; i++) if (g_targets[i].used) n++;
    return n;
}
