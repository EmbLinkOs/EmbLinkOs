/* kernel/drivers/storage/lsi53c895a.c -- the controller that runs a program.
 *
 * Every other controller in this directory is told what to do. This one is
 * PROGRAMMED. It contains a small processor -- LSI called the instruction set
 * SCRIPTS -- which fetches instructions from HOST MEMORY and executes them:
 * select a target, move these bytes in this bus phase, branch on the phase
 * the target went to next, interrupt the driver when finished. The driver's
 * job is to write that program, point the chip's instruction pointer at it,
 * and wait.
 *
 * That is not a curiosity. It is why a 1998 card could run a deep queue with
 * almost no host involvement, and it is why this driver is shaped completely
 * unlike the other four: there is no request structure and no doorbell. There
 * is a program, and the interrupt at the end of it carries a value the program
 * chose.
 *
 * THE PROGRAM HAS TO KNOW THE BUS PHASES IN ADVANCE, and it cannot. A SCSI
 * target decides what happens next -- whether there is data, whether it
 * disconnects to go and fetch it, how many bytes it actually has. A block
 * move whose phase does not match the bus raises a PHASE MISMATCH and halts
 * the processor. So the program here is not a straight line: it is a
 * DISPATCH LOOP that branches on the current phase and comes back to itself
 * after every step, and the driver restarts it at the dispatch label whenever
 * a mismatch halts it. That single structure is what makes this work for
 * commands with data, without data, short reads, and disconnects.
 *
 * DISCONNECTION IS NOT OPTIONAL. A real read takes milliseconds, and the
 * target releases the bus and RESELECTS when the data is ready. A program
 * that does not wait for that hangs on the first command that actually
 * touches a disk -- which is why probing appeared to work long before
 * reading did.
 */
#include <stdint.h>
#include <stddef.h>

#include "include/types.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/errno.h"
#include "drivers/bus/pci.h"
#include "drivers/storage/lsi53c895a.h"
#include "block/block.h"
#include "block/scsi.h"
#include "mm/vmm.h"
#include "mm/pmm.h"

#define LSI_SCNTL0 0x00
#define LSI_SCNTL1 0x01
#define LSI_SCID   0x04
#define LSI_DSTAT  0x0C
#define LSI_SSTAT1 0x0E
#define LSI_ISTAT0 0x14
#define LSI_DSP    0x2C
#define LSI_DSPS   0x30
#define LSI_DMODE  0x38
#define LSI_DIEN   0x39
#define LSI_DCNTL  0x3B
#define LSI_SIEN0  0x40
#define LSI_SIEN1  0x41
#define LSI_SIST0  0x42
#define LSI_SIST1  0x43
#define LSI_STEST3 0x4F

#define ISTAT0_DIP 0x01
#define ISTAT0_SIP 0x02

#define DSTAT_IID  0x01   /* illegal instruction */
#define DSTAT_SIR  0x04   /* the script asked for an interrupt */

#define SIST0_MA   0x10   /* phase mismatch */
#define SIST0_UDC  0x04   /* unexpected disconnect */
#define SIST1_STO  0x04   /* selection timed out: nothing at that address */
#define DSTAT_DFE  0x80   /* the DMA FIFO is empty -- a state, not a fault */

/* Bus phases, which are also the block-move instruction's phase field. */
#define PH_DO  0
#define PH_DI  1
#define PH_CMD 2
#define PH_ST  3
#define PH_MO  6
#define PH_MI  7

/* The interrupt values this program reports. Arbitrary, but distinct, so a
 * failure says which branch of the program it stopped in. */
#define INT_DONE       0x00C0DE00u
#define INT_BAD_PHASE  0x00BAD000u
#define INT_NO_TARGET  0x00F00F00u
#define INT_NO_DATA    0x00D00D00u

#define LSI_MAX_TARGETS 8
#define LSI_XFER        65536
#define LSI_SCRIPT_DWORDS 64

static volatile uint8_t *g_regs;
static bool g_up;

static uint32_t g_script[LSI_SCRIPT_DWORDS] __attribute__((aligned(64)));
static uint8_t  g_cdb[16]     __attribute__((aligned(64)));
static uint8_t  g_msgout[1]   __attribute__((aligned(64)));
static uint8_t  g_msgin[8]    __attribute__((aligned(64)));
static uint8_t  g_status[1]   __attribute__((aligned(64)));
static uint8_t  g_data[LSI_XFER] __attribute__((aligned(64)));

struct lsi_target {
    bool used;
    uint8_t target;
    struct scsi_device sd;
    struct embk_block_device blk;
};
static struct lsi_target g_targets[LSI_MAX_TARGETS];

static inline uint8_t rr(uint32_t o) { return *(volatile uint8_t *)(g_regs + o); }
static inline void    rw(uint32_t o, uint8_t v) { *(volatile uint8_t *)(g_regs + o) = v; }
static inline uint64_t dma(const volatile void *p) { return KV2P((uint64_t)(uintptr_t)p); }

/* Instruction encodings. Each is two 32-bit words; the second is an address
 * or an immediate depending on the first. */
static inline uint32_t insn_select(uint8_t id)  { return 0x41000000u | ((uint32_t)id << 16); }
static inline uint32_t insn_move(uint8_t phase, uint32_t count) {
    return ((uint32_t)phase << 24) | (count & 0x00FFFFFFu);
}
/* Jump when the bus is in this phase: the "test phase" and "jump if true"
 * bits, with the phase in the same field a block move uses. */
static inline uint32_t insn_jump_phase(uint8_t phase) {
    return 0x800A0000u | ((uint32_t)phase << 24);
}
/* Jump when the first byte received equals this value. The mask lives in
 * bits 15:8 INVERTED, so a full-byte comparison writes zeros there. */
static inline uint32_t insn_jump_data(uint8_t value) {
    return 0x800C0000u | value;
}
#define INSN_JUMP        0x80080000u
#define INSN_WAIT_DISC   0x48000000u
#define INSN_WAIT_RESEL  0x50000000u
#define INSN_INT         0x98080000u

/* Where each label sits, in dwords. Fixed offsets rather than a assembler,
 * because the program is twenty-three instructions long and never changes
 * shape -- only the counts and addresses inside it do. */
#define L_START     0
#define L_DISPATCH  6
#define L_DATAIN   16
#define L_DATAOUT  20
#define L_STATUS   24
#define L_MSGIN    28
#define L_DISC     36
#define L_COMPLETE 42
#define L_FAIL     46

static void build_script(uint8_t target, uint8_t cdb_len, uint32_t len, bool to_dev) {
    uint32_t base = (uint32_t)dma(g_script);
    uint32_t *p = g_script;
#define AT(label) (base + (label) * 4)

    /* Select the target. On failure the chip jumps to the address in the
     * second word instead of hanging on a bus that never answered. */
    p[0] = insn_select(target);      p[1] = AT(L_FAIL);
    /* IDENTIFY, so the target knows which logical unit and that it may
     * disconnect. Withholding that permission does not make a slow disk
     * fast; it makes it hold the bus. */
    p[2] = insn_move(PH_MO, 1);      p[3] = (uint32_t)dma(g_msgout);
    p[4] = insn_move(PH_CMD, cdb_len); p[5] = (uint32_t)dma(g_cdb);

    /* The dispatch loop: whatever phase the target moved to, go and do it,
     * then come back here. */
    p[L_DISPATCH + 0] = insn_jump_phase(PH_DI); p[L_DISPATCH + 1] = AT(L_DATAIN);
    p[L_DISPATCH + 2] = insn_jump_phase(PH_DO); p[L_DISPATCH + 3] = AT(L_DATAOUT);
    p[L_DISPATCH + 4] = insn_jump_phase(PH_ST); p[L_DISPATCH + 5] = AT(L_STATUS);
    p[L_DISPATCH + 6] = insn_jump_phase(PH_MI); p[L_DISPATCH + 7] = AT(L_MSGIN);
    p[L_DISPATCH + 8] = INSN_INT;               p[L_DISPATCH + 9] = INT_BAD_PHASE;

    /* The data phases. A command with no data still needs these slots to
     * exist -- the dispatch loop branches to them unconditionally on phase --
     * so when there is nothing to move they report it rather than issuing a
     * zero-length move, which is not a defined instruction. */
    if (len) {
        p[L_DATAIN + 0]  = insn_move(PH_DI, len); p[L_DATAIN + 1]  = (uint32_t)dma(g_data);
        p[L_DATAOUT + 0] = insn_move(PH_DO, len); p[L_DATAOUT + 1] = (uint32_t)dma(g_data);
    } else {
        p[L_DATAIN + 0]  = INSN_INT; p[L_DATAIN + 1]  = INT_NO_DATA;
        p[L_DATAOUT + 0] = INSN_INT; p[L_DATAOUT + 1] = INT_NO_DATA;
    }
    p[L_DATAIN + 2]  = INSN_JUMP; p[L_DATAIN + 3]  = AT(L_DISPATCH);
    p[L_DATAOUT + 2] = INSN_JUMP; p[L_DATAOUT + 3] = AT(L_DISPATCH);
    (void)to_dev;   /* the phase the target chooses decides the direction */

    p[L_STATUS + 0] = insn_move(PH_ST, 1); p[L_STATUS + 1] = (uint32_t)dma(g_status);
    p[L_STATUS + 2] = INSN_JUMP;           p[L_STATUS + 3] = AT(L_DISPATCH);

    /* One message byte at a time, because the messages that matter are one
     * byte and the branch is on the byte just received. */
    p[L_MSGIN + 0] = insn_move(PH_MI, 1);  p[L_MSGIN + 1] = (uint32_t)dma(g_msgin);
    p[L_MSGIN + 2] = insn_jump_data(0x00); p[L_MSGIN + 3] = AT(L_COMPLETE);
    p[L_MSGIN + 4] = insn_jump_data(0x04); p[L_MSGIN + 5] = AT(L_DISC);
    /* Anything else -- save data pointer, restore pointers, an identify on
     * reselection -- is acknowledged by having been read, and the loop
     * continues. */
    p[L_MSGIN + 6] = INSN_JUMP;            p[L_MSGIN + 7] = AT(L_DISPATCH);

    /* The target has gone away to fetch the data. Wait for it to come back
     * and pick up where it left off. */
    p[L_DISC + 0] = INSN_WAIT_DISC;  p[L_DISC + 1] = 0;
    p[L_DISC + 2] = INSN_WAIT_RESEL; p[L_DISC + 3] = AT(L_FAIL);
    p[L_DISC + 4] = INSN_JUMP;       p[L_DISC + 5] = AT(L_DISPATCH);

    p[L_COMPLETE + 0] = INSN_WAIT_DISC; p[L_COMPLETE + 1] = 0;
    p[L_COMPLETE + 2] = INSN_INT;       p[L_COMPLETE + 3] = INT_DONE;

    p[L_FAIL + 0] = INSN_INT; p[L_FAIL + 1] = INT_NO_TARGET;
#undef AT
}

/* Start or restart the processor at `dword_offset`. Writing the top byte of
 * the instruction pointer is what sets it running. */
static void script_run(uint32_t dword_offset) {
    uint32_t a = (uint32_t)dma(g_script) + dword_offset * 4;
    __sync_synchronize();
    rw(LSI_DSP + 0, (uint8_t)a);
    rw(LSI_DSP + 1, (uint8_t)(a >> 8));
    rw(LSI_DSP + 2, (uint8_t)(a >> 16));
    rw(LSI_DSP + 3, (uint8_t)(a >> 24));
}

static bool lsi_exec(void *ctx, const uint8_t *cdb, uint8_t cdb_len,
                     void *data, uint32_t len, bool to_dev) {
    struct lsi_target *t = ctx;
    if (!g_up || len > LSI_XFER || cdb_len > 16) return false;

    memcpy(g_cdb, cdb, cdb_len);
    g_msgout[0] = 0xC0;          /* IDENTIFY, LUN 0, disconnect permitted */
    g_status[0] = 0xFF;
    if (to_dev && data && len) memcpy(g_data, data, len);

    build_script(t->target, cdb_len, len, to_dev);
    script_run(L_START);

    /* The processor runs until it stops. It stops for exactly three reasons:
     * the program asked to interrupt, the bus phase did not match an
     * instruction, or something went wrong. Only the first is an answer. */
    for (int round = 0; round < 64; round++) {
        bool stopped = false;
        for (int spin = 0; spin < 20000000; spin++) {
            if (rr(LSI_ISTAT0) & (ISTAT0_DIP | ISTAT0_SIP)) { stopped = true; break; }
            __asm__ volatile("" ::: "memory");
        }
        if (!stopped) {
            kprintf("lsi: target %u: the script never stopped\n", t->target);
            return false;
        }

        /* READ THE STATUS REGISTERS ONCE. Reading them is what clears them,
         * so a second read reports nothing and the reason is lost. */
        uint8_t dstat = rr(LSI_DSTAT);
        uint8_t sist0 = rr(LSI_SIST0);
        uint8_t sist1 = rr(LSI_SIST1);

        /* SELECTION TIMED OUT: there is nothing at that address. That is the
         * answer to a probe, not a fault, and it does not even stop the
         * processor -- the chip carries on into the next instruction, which
         * is a message move on a bus nobody is holding. So it has to be
         * caught here, before the halt that follows looks like a failure. */
        if (sist1 & SIST1_STO) return false;

        if (sist0 & SIST0_MA) {
            /* PHASE MISMATCH. The target moved on before the instruction
             * finished -- a short transfer, almost always, which is a normal
             * thing for a target to do. The dispatch loop exists precisely so
             * that the answer is "go and look at the phase again". */
            script_run(L_DISPATCH);
            continue;
        }
        if (sist0 & SIST0_UDC) {
            kprintf("lsi: target %u disconnected unexpectedly\n", t->target);
            return false;
        }
        if (dstat & DSTAT_IID) {
            kprintf("lsi: the chip refused an instruction (script is wrong)\n");
            return false;
        }
        if (dstat & DSTAT_SIR) {
            uint32_t v = (uint32_t)rr(LSI_DSPS) |
                         ((uint32_t)rr(LSI_DSPS + 1) << 8) |
                         ((uint32_t)rr(LSI_DSPS + 2) << 16) |
                         ((uint32_t)rr(LSI_DSPS + 3) << 24);
            if (v == INT_DONE) {
                if (g_status[0] != 0) return false;
                if (!to_dev && data && len) memcpy(data, g_data, len);
                return true;
            }
            if (v == INT_NO_TARGET) return false;      /* nothing there: a probe */
            if (v == INT_NO_DATA) {
                /* The target wants to move data for a command we were told
                 * has none. Nothing can be done with it; stop rather than
                 * transfer into a buffer of unknown size. */
                return false;
            }
            kprintf("lsi: target %u: script interrupt %08x\n", t->target, v);
            return false;
        }
        /* Something else stopped it; going round again would spin. DFE just
         * says the chip's DMA FIFO is empty, which it usually is. */
        if (dstat & ~DSTAT_DFE)
            kprintf("lsi: target %u halted: dstat %02x sist0 %02x\n",
                    t->target, dstat, sist0);
        return false;
    }
    kprintf("lsi: target %u: too many phase changes for one command\n", t->target);
    return false;
}

static int lsi_read(struct embk_block_device *b, uint64_t lba,
                    uint32_t count, void *buf) {
    struct lsi_target *t = b->driver_data;
    return scsi_read(&t->sd, lba, count, buf, LSI_XFER / t->sd.block_size);
}
static int lsi_write(struct embk_block_device *b, uint64_t lba,
                     uint32_t count, const void *buf) {
    struct lsi_target *t = b->driver_data;
    return scsi_write(&t->sd, lba, count, buf, LSI_XFER / t->sd.block_size);
}

bool lsi53c895a_init(void) {
    if (g_up) return true;
    const struct pci_device *d = NULL;
    uint32_t n = pci_devices_count();
    for (uint32_t i = 0; i < n; i++) {
        const struct pci_device *c = pci_get_device(i);
        if (!c || c->vendor_id != 0x1000) continue;
        if (c->device_id == 0x0012 || c->device_id == 0x000F) { d = c; break; }
    }
    if (!d) return false;

    /* BAR0 is I/O, BAR1 the same registers in memory, BAR2 the chip's own
     * scratch RAM. The memory window is the one to use. */
    struct pci_bar bar = pci_read_bar(d->bus, d->device, d->function, 1);
    if (!bar.valid || !bar.is_mmio) {
        kprintf("lsi: BAR1 is not a memory window\n");
        return false;
    }
    g_regs = (volatile uint8_t *)(uintptr_t)vmm_map_mmio(bar.address,
                                                        bar.size ? bar.size : 0x400);
    if (!g_regs) return false;
    pci_enable_bus_mastering(d->bus, d->device, d->function);

    /* Reset, then say who we are. SCID is this adapter's own address on the
     * bus; 7 by convention, because the highest id wins arbitration. */
    rw(LSI_ISTAT0, 0x40);                 /* software reset */
    for (volatile int i = 0; i < 100000; i++) { }
    rw(LSI_ISTAT0, 0x00);
    rw(LSI_SCID, 0x07);
    rw(LSI_SCNTL0, 0xC0);                 /* arbitration: full, with priority */
    rw(LSI_SCNTL1, 0x00);
    rw(LSI_DMODE, 0x00);
    /* NO INTERRUPTS ENABLED. The status bits still latch and this driver
     * polls them; enabling the line would assert a level-triggered interrupt
     * with nothing to clear it. */
    rw(LSI_DIEN, 0x00);
    rw(LSI_SIEN0, 0x00);
    rw(LSI_SIEN1, 0x00);
    (void)rr(LSI_DSTAT);
    (void)rr(LSI_SIST0);
    (void)rr(LSI_SIST1);
    g_up = true;

    int found = 0;
    for (uint32_t tn = 0; tn < LSI_MAX_TARGETS; tn++) {
        if (tn == 7) continue;            /* that is us */
        struct lsi_target *t = &g_targets[tn];
        memset(t, 0, sizeof *t);
        t->used = true;
        t->target = (uint8_t)tn;
        t->sd.exec = lsi_exec;
        t->sd.ctx = t;
        if (!scsi_probe(&t->sd)) { t->used = false; continue; }

        t->blk.block_count = t->sd.blocks;
        t->blk.block_size  = t->sd.block_size;
        t->blk.read  = lsi_read;
        t->blk.write = lsi_write;
        t->blk.driver_data = t;
        /* THE SCRIPT'S ADDRESSES ARE THIRTY-TWO BITS WIDE, so everything it
         * can reach has to live below four gigabytes. */
        t->blk.dma_max_phys = 0xFFFFFFFFULL;
        t->blk.needs_kernel_range = true;
        if (embk_block_register(&t->blk) != EMBK_OK) { t->used = false; continue; }
        kprintf("lsi: %s = target %u, %s %s, %llu x %u B\n", t->blk.name, tn,
                t->sd.vendor, t->sd.product,
                (unsigned long long)t->sd.blocks, t->sd.block_size);
        found++;
    }
    if (!found) kprintf("lsi: controller up, no targets answered\n");
    return true;
}

bool lsi53c895a_present(void) { return g_up; }
uint32_t lsi53c895a_target_count(void) {
    uint32_t n = 0;
    for (int i = 0; i < LSI_MAX_TARGETS; i++) if (g_targets[i].used) n++;
    return n;
}
