/* kernel/drivers/storage/ufs.c -- Universal Flash Storage.
 *
 * What a phone and a thin laptop use instead of SATA. The commands are still
 * SCSI -- the same CDBs kernel/block/scsi.c builds for everything else -- but
 * they arrive wrapped in two layers this kernel has not met before:
 *
 *   A UPIU is the packet: a 32-byte header saying what kind of message this
 *   is, who it is for, and how much data follows, with the CDB inside it.
 *
 *   A TRANSFER REQUEST DESCRIPTOR is the envelope the CONTROLLER reads: it
 *   points at a command descriptor holding the request UPIU, the space for
 *   the response UPIU, and a scatter-gather table, and it says where inside
 *   that block each of those three things starts.
 *
 * THOSE OFFSETS ARE IN DWORDS, NOT BYTES. Every one of the four length and
 * offset fields in the descriptor counts 32-bit words. Writing byte offsets
 * points the controller a quarter of the way into the block it wanted, and
 * what it finds there is a plausible-looking mess.
 *
 * AND THE SCATTER-GATHER SIZE IS THE BYTE COUNT MINUS ONE. A zero-length
 * entry cannot be expressed, and an entry that says 512 moves 513 bytes.
 *
 * Before any of that works the LINK has to be started -- UFS is a serial
 * link with its own protocol stack underneath, and the controller will not
 * accept a transfer request until it has negotiated with the device.
 */
#include <stdint.h>
#include <stddef.h>

#include "include/types.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/errno.h"
#include "drivers/bus/pci.h"
#include "drivers/storage/ufs.h"
#include "block/block.h"
#include "block/scsi.h"
#include "mm/vmm.h"
#include "mm/pmm.h"

#define UFS_CAP        0x00
#define UFS_VER        0x08
#define UFS_IS         0x20   /* interrupt status  */
#define UFS_IE         0x24   /* interrupt enable  */
#define UFS_HCS        0x30   /* controller status */
#define UFS_HCE        0x34   /* controller enable */
#define UFS_UTRLBA     0x50   /* transfer request list, low half  */
#define UFS_UTRLBAU    0x54   /* and high half                    */
#define UFS_UTRLDBR    0x58   /* the doorbell: one bit per slot   */
#define UFS_UTRLCLR    0x5C
#define UFS_UTRLRSR    0x60   /* run/stop                         */
#define UFS_UICCMD     0x90
#define UFS_UICCMDARG1 0x94
#define UFS_UICCMDARG2 0x98
#define UFS_UICCMDARG3 0x9C

#define HCS_DP      0x01   /* a device is present   */
#define HCS_UTRLRDY 0x02
#define HCS_UCRDY   0x08   /* ready for a UIC command */

#define IS_UTRCS 0x00000001u   /* a transfer request finished */
#define IS_UCCS  0x00000400u   /* a UIC command finished      */

#define UIC_DME_LINKSTARTUP 0x16

/* Transfer request descriptor, first dword. */
#define UTRD_CT_UFS_STORAGE (1u << 28)
#define UTRD_DD_TO_DEVICE   (1u << 25)
#define UTRD_DD_TO_HOST     (1u << 26)
#define UTRD_INTERRUPT      (1u << 24)

#define UPIU_CMD        0x01
#define UPIU_NOP_OUT    0x00
#define UPIU_FLAG_READ  0x40
#define UPIU_FLAG_WRITE 0x20

#define UFS_MAX_LUNS 4
#define UFS_XFER     65536

/* The controller reads this; every field the hardware touches is little
 * endian EXCEPT the UPIU's own, which are big endian -- a UPIU is a network
 * packet by ancestry. Both appear below and are marked where they do. */
struct utrd {
    uint32_t dword0;
    uint32_t dword1;
    uint32_t dword2;          /* the controller writes the overall status here */
    uint32_t dword3;
    uint32_t ucd_base_lo;
    uint32_t ucd_base_hi;
    uint16_t resp_upiu_length;   /* IN DWORDS */
    uint16_t resp_upiu_offset;   /* IN DWORDS */
    uint16_t prdt_length;        /* entries  */
    uint16_t prdt_offset;        /* IN DWORDS */
} __attribute__((packed));

struct upiu_header {
    uint8_t  transaction;
    uint8_t  flags;
    uint8_t  lun;
    uint8_t  task_tag;
    uint8_t  command_set_type;
    uint8_t  function;
    uint8_t  response;
    uint8_t  status;
    uint8_t  ehs_length;
    uint8_t  device_info;
    uint16_t data_segment_length;   /* big endian */
} __attribute__((packed));

struct upiu_command {
    struct upiu_header hdr;
    uint32_t expected_len;          /* big endian */
    uint8_t  cdb[16];
} __attribute__((packed));

struct prdt_entry {
    uint32_t addr_lo;
    uint32_t addr_hi;
    uint32_t reserved;
    uint32_t size;                  /* BYTE COUNT MINUS ONE */
} __attribute__((packed));

/* One command descriptor: request, response and table, at fixed offsets so
 * the descriptor's dword offsets are constants rather than arithmetic. */
#define UCD_REQ_OFF   0
#define UCD_RESP_OFF  128
#define UCD_PRDT_OFF  256
#define UCD_SIZE      512
#define UFS_PRDT_MAX  ((UCD_SIZE - UCD_PRDT_OFF) / (int)sizeof(struct prdt_entry))

static volatile uint8_t *g_regs;
static bool g_up;

/* THE LIST MUST BE 1024-BYTE ALIGNED and the command descriptor 128-byte
 * aligned; the controller ignores the low bits of the addresses it is given
 * rather than complaining about them. */
static struct utrd g_utrd[1]  __attribute__((aligned(1024)));
static uint8_t     g_ucd[UCD_SIZE] __attribute__((aligned(128)));
static uint8_t     g_data[UFS_XFER] __attribute__((aligned(64)));

struct ufs_lun {
    bool used;
    uint8_t lun;
    struct scsi_device sd;
    struct embk_block_device blk;
};
static struct ufs_lun g_luns[UFS_MAX_LUNS];

static inline uint32_t rr(uint32_t o) { return *(volatile uint32_t *)(g_regs + o); }
static inline void     rw(uint32_t o, uint32_t v) { *(volatile uint32_t *)(g_regs + o) = v; }
static inline uint64_t dma(const volatile void *p) { return KV2P((uint64_t)(uintptr_t)p); }
static inline uint32_t be32(uint32_t v) {
    return ((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8) |
           ((v >> 8) & 0xFF00u) | ((v >> 24) & 0xFFu);
}

static bool wait_bits(uint32_t reg, uint32_t mask, int spins) {
    for (int i = 0; i < spins; i++) {
        if ((rr(reg) & mask) == mask) return true;
        __asm__ volatile("" ::: "memory");
    }
    return false;
}

/* Run one request through slot 0 and wait for the doorbell bit to clear. */
static bool ufs_submit(void) {
    rw(UFS_IS, IS_UTRCS);            /* clear the previous completion */
    __sync_synchronize();
    rw(UFS_UTRLDBR, 1);              /* slot 0 */
    for (int i = 0; i < 20000000; i++) {
        if (!(rr(UFS_UTRLDBR) & 1)) {
            rw(UFS_IS, IS_UTRCS);
            return true;
        }
        __asm__ volatile("" ::: "memory");
    }
    return false;
}

/* Build the descriptor for a command whose UPIU is already in the command
 * descriptor. `prdt_entries` is 0 for a command with no data. */
static void ufs_build_utrd(uint32_t dd, uint16_t prdt_entries) {
    memset(&g_utrd[0], 0, sizeof g_utrd[0]);
    g_utrd[0].dword0 = UTRD_CT_UFS_STORAGE | dd | UTRD_INTERRUPT;
    uint64_t ucd = dma(g_ucd);
    g_utrd[0].ucd_base_lo = (uint32_t)ucd;
    g_utrd[0].ucd_base_hi = (uint32_t)(ucd >> 32);
    /* ALL FOUR OF THESE COUNT DWORDS. */
    g_utrd[0].resp_upiu_offset = UCD_RESP_OFF / 4;
    g_utrd[0].resp_upiu_length = (UCD_PRDT_OFF - UCD_RESP_OFF) / 4;
    g_utrd[0].prdt_offset = UCD_PRDT_OFF / 4;
    g_utrd[0].prdt_length = prdt_entries;
}

static bool ufs_exec(void *ctx, const uint8_t *cdb, uint8_t cdb_len,
                     void *data, uint32_t len, bool to_dev) {
    struct ufs_lun *l = ctx;
    if (!g_up || len > UFS_XFER) return false;

    if (to_dev && data && len) memcpy(g_data, data, len);
    memset(g_ucd, 0, sizeof g_ucd);

    struct upiu_command *c = (struct upiu_command *)(g_ucd + UCD_REQ_OFF);
    c->hdr.transaction = UPIU_CMD;
    c->hdr.flags = len ? (to_dev ? UPIU_FLAG_WRITE : UPIU_FLAG_READ) : 0;
    c->hdr.lun = l->lun;
    c->hdr.task_tag = 1;
    c->hdr.command_set_type = 0;        /* SCSI */
    /* THE UPIU'S OWN NUMBERS ARE BIG ENDIAN. It is a packet format, not a
     * memory structure, and it kept the byte order of the wire it came from. */
    c->expected_len = be32(len);
    memcpy(c->cdb, cdb, cdb_len > 16 ? 16 : cdb_len);

    uint16_t entries = 0;
    if (len) {
        struct prdt_entry *e = (struct prdt_entry *)(g_ucd + UCD_PRDT_OFF);
        uint64_t pa = dma(g_data);
        e[0].addr_lo = (uint32_t)pa;
        e[0].addr_hi = (uint32_t)(pa >> 32);
        /* MINUS ONE. The field is a maximum index, not a count. */
        e[0].size = len - 1;
        entries = 1;
    }
    ufs_build_utrd(len ? (to_dev ? UTRD_DD_TO_DEVICE : UTRD_DD_TO_HOST) : 0u,
                   entries);

    if (!ufs_submit()) {
        kprintf("ufs: LUN %u did not answer command 0x%02x\n", l->lun, cdb[0]);
        return false;
    }

    /* TWO STATUSES. The descriptor's overall command status is the
     * controller's view; the response UPIU's status byte is the device's. */
    uint8_t ocs = (uint8_t)(g_utrd[0].dword2 & 0xFF);
    struct upiu_header *rsp = (struct upiu_header *)(g_ucd + UCD_RESP_OFF);
    if (ocs != 0) return false;
    if (rsp->status != 0) return false;
    if (!to_dev && data && len) memcpy(data, g_data, len);
    return true;
}

static int ufs_read(struct embk_block_device *b, uint64_t lba,
                    uint32_t count, void *buf) {
    struct ufs_lun *l = b->driver_data;
    return scsi_read(&l->sd, lba, count, buf, UFS_XFER / l->sd.block_size);
}
static int ufs_write(struct embk_block_device *b, uint64_t lba,
                     uint32_t count, const void *buf) {
    struct ufs_lun *l = b->driver_data;
    return scsi_write(&l->sd, lba, count, buf, UFS_XFER / l->sd.block_size);
}

/* Send one UIC command -- the link-layer protocol underneath the transfer
 * layer. Only one is needed, and without it nothing else works. */
static bool ufs_uic(uint32_t cmd, uint32_t a1, uint32_t a2, uint32_t a3) {
    if (!wait_bits(UFS_HCS, HCS_UCRDY, 1000000)) {
        kprintf("ufs: controller never became ready for a link command\n");
        return false;
    }
    rw(UFS_IS, IS_UCCS);
    rw(UFS_UICCMDARG1, a1);
    rw(UFS_UICCMDARG2, a2);
    rw(UFS_UICCMDARG3, a3);
    rw(UFS_UICCMD, cmd);
    if (!wait_bits(UFS_IS, IS_UCCS, 5000000)) {
        kprintf("ufs: link command %u never completed\n", cmd);
        return false;
    }
    uint32_t result = rr(UFS_UICCMDARG2) & 0xFF;
    rw(UFS_IS, IS_UCCS);
    if (result != 0) {
        kprintf("ufs: link command %u failed with %u\n", cmd, result);
        return false;
    }
    return true;
}

bool ufs_init(void) {
    if (g_up) return true;
    const struct pci_device *d = NULL;
    uint32_t n = pci_devices_count();
    for (uint32_t i = 0; i < n; i++) {
        const struct pci_device *c = pci_get_device(i);
        /* Class 01 subclass 09 is "UFS controller". Matched on class rather
         * than a device id, because unlike the SCSI cards there is no family
         * of incompatible parts sharing it. */
        if (c && c->class_code == 0x01 && c->subclass == 0x09) { d = c; break; }
    }
    if (!d) return false;

    struct pci_bar bar = pci_read_bar(d->bus, d->device, d->function, 0);
    if (!bar.valid || !bar.is_mmio) { kprintf("ufs: no MMIO BAR0\n"); return false; }
    g_regs = (volatile uint8_t *)(uintptr_t)vmm_map_mmio(bar.address,
                                                        bar.size ? bar.size : 0x1000);
    if (!g_regs) return false;
    pci_enable_bus_mastering(d->bus, d->device, d->function);

    /* Enable, and wait for the controller to say it is enabled -- the bit is
     * a request going in and a report coming back. */
    rw(UFS_HCE, 1);
    if (!wait_bits(UFS_HCE, 1, 5000000)) {
        kprintf("ufs: controller did not enable\n");
        return false;
    }
    rw(UFS_IE, 0);                  /* this driver polls */

    /* START THE LINK. UFS is a serial link with its own protocol stack, and
     * the transfer layer above it does not exist until the two ends have
     * negotiated. Nothing else here works before this. */
    if (!ufs_uic(UIC_DME_LINKSTARTUP, 0, 0, 0)) return false;
    if (!wait_bits(UFS_HCS, HCS_DP, 5000000)) {
        kprintf("ufs: link started but no device is present\n");
        return false;
    }

    uint64_t lb = dma(g_utrd);
    rw(UFS_UTRLBA, (uint32_t)lb);
    rw(UFS_UTRLBAU, (uint32_t)(lb >> 32));
    rw(UFS_UTRLRSR, 1);
    if (!wait_bits(UFS_HCS, HCS_UTRLRDY, 1000000)) {
        kprintf("ufs: the transfer request list never came ready\n");
        return false;
    }
    g_up = true;

    /* A NOP first: it proves the whole path -- descriptor, command
     * descriptor, doorbell, response -- before a real command depends on it,
     * and it is what the device expects as its first transaction. */
    memset(g_ucd, 0, sizeof g_ucd);
    ((struct upiu_header *)g_ucd)->transaction = UPIU_NOP_OUT;
    ((struct upiu_header *)g_ucd)->task_tag = 1;
    ufs_build_utrd(0, 0);
    if (!ufs_submit() || (g_utrd[0].dword2 & 0xFF) != 0)
        kprintf("ufs: the device did not answer a NOP; continuing anyway\n");

    int found = 0;
    for (uint32_t ln = 0; ln < UFS_MAX_LUNS; ln++) {
        struct ufs_lun *l = &g_luns[ln];
        memset(l, 0, sizeof *l);
        l->used = true;
        l->lun = (uint8_t)ln;
        l->sd.exec = ufs_exec;
        l->sd.ctx = l;
        if (!scsi_probe(&l->sd)) { l->used = false; continue; }

        l->blk.block_count = l->sd.blocks;
        l->blk.block_size  = l->sd.block_size;
        l->blk.read  = ufs_read;
        l->blk.write = ufs_write;
        l->blk.driver_data = l;
        l->blk.dma_max_phys = ~0ULL;
        l->blk.needs_kernel_range = true;
        if (embk_block_register(&l->blk) != EMBK_OK) { l->used = false; continue; }
        kprintf("ufs: %s = LUN %u, %s %s, %llu x %u B\n", l->blk.name, ln,
                l->sd.vendor, l->sd.product,
                (unsigned long long)l->sd.blocks, l->sd.block_size);
        found++;
    }
    if (!found) kprintf("ufs: controller up, no logical units answered\n");
    return true;
}

bool ufs_present(void) { return g_up; }
uint32_t ufs_lun_count(void) {
    uint32_t n = 0;
    for (int i = 0; i < UFS_MAX_LUNS; i++) if (g_luns[i].used) n++;
    return n;
}
