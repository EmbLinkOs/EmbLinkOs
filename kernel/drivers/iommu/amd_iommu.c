/* kernel/drivers/iommu/amd_iommu.c -- AMD-Vi, the other hardware IOMMU.
 *
 * Same job as VT-d and a different shape. Intel looks a device up through two
 * levels -- a root table by bus, then a context table by device -- and AMD
 * uses ONE FLAT DEVICE TABLE indexed by the whole sixteen-bit requester id.
 * Simpler to walk, and much larger: a table covering every possible id is two
 * megabytes.
 *
 * THE OTHER DIFFERENCE IS THAT NOTHING HERE IS DONE BY WRITING A REGISTER.
 * Intel has an invalidate-and-wait register; AMD has a COMMAND BUFFER in
 * memory with a head and a tail, and invalidations are commands posted into
 * it. Which means the command buffer has to be set up and enabled BEFORE the
 * IOMMU is, because the first thing that needs doing after enabling it is an
 * invalidation -- and there would be no way to issue one.
 *
 * A device table entry here does not have to point at a page table. Setting
 * it valid with translation "not required" leaves addresses alone and still
 * applies the read and write permission bits, which is the identity mapping
 * with none of the tables. Same honesty as the other two drivers: it protects
 * nothing yet, and the value is that the machinery is real and switched on.
 */
#include <stdint.h>
#include <stddef.h>

#include "include/types.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/errno.h"
#include "acpi/acpi.h"
#include "drivers/bus/pci.h"
#include "drivers/iommu/amd_iommu.h"
#include "mm/vmm.h"
#include "mm/pmm.h"

#define AMDV_DEVTAB_BASE 0x0000
#define AMDV_CMDBUF_BASE 0x0008
#define AMDV_EVTLOG_BASE 0x0010
#define AMDV_CONTROL     0x0018
#define AMDV_CMD_HEAD    0x2000
#define AMDV_CMD_TAIL    0x2008
#define AMDV_EVT_HEAD    0x2010
#define AMDV_EVT_TAIL    0x2018
#define AMDV_STATUS      0x2020

#define CTRL_IOMMU_EN  (1ULL << 0)
#define CTRL_EVENT_EN  (1ULL << 2)
#define CTRL_EVENT_INT (1ULL << 3)
#define CTRL_CMDBUF_EN (1ULL << 12)

#define DTE_VALID   (1ULL << 0)
#define DTE_TV      (1ULL << 1)   /* the translation information is valid */
#define DTE_IR      (1ULL << 61)  /* reads allowed  */
#define DTE_IW      (1ULL << 62)  /* writes allowed */

#define CMD_COMPLETION_WAIT   0x01
#define CMD_INVALIDATE_DEVTAB 0x02
#define CMD_INVALIDATE_PAGES  0x03

/* 2048 entries of 32 bytes: requester ids 0..0x7FF, which is buses 0 to 7.
 * A full table is 2 MiB, and a machine with devices above bus 7 needs one --
 * recorded rather than silently truncated, because a device outside the table
 * is refused, not passed through. */
#define AMDV_DEVTAB_ENTRIES 2048
#define AMDV_DEVTAB_BYTES   (AMDV_DEVTAB_ENTRIES * 32)
#define AMDV_CMD_ENTRIES    256
#define AMDV_EVT_ENTRIES    256

static volatile uint8_t *g_regs;
static bool g_up;
static uint32_t g_devices;

static uint64_t g_devtab[AMDV_DEVTAB_BYTES / 8] __attribute__((aligned(4096)));
static uint64_t g_cmdbuf[AMDV_CMD_ENTRIES * 2]  __attribute__((aligned(4096)));
static uint64_t g_evtlog[AMDV_EVT_ENTRIES * 2]  __attribute__((aligned(4096)));
static uint32_t g_cmd_tail;

static inline uint64_t rr(uint32_t o) { return *(volatile uint64_t *)(g_regs + o); }
static inline void     rw(uint32_t o, uint64_t v) { *(volatile uint64_t *)(g_regs + o) = v; }
static inline uint64_t dma(const volatile void *p) { return KV2P((uint64_t)(uintptr_t)p); }

/* Post one 16-byte command and move the tail. The IOMMU reads from head to
 * tail; moving the tail is the notification, and there is no doorbell. */
static void amdv_command(uint32_t opcode, uint32_t arg0, uint32_t arg1) {
    uint32_t idx = g_cmd_tail / 16;
    uint64_t *c = &g_cmdbuf[idx * 2];
    c[0] = (uint64_t)arg0;
    /* THE OPCODE LIVES IN THE TOP FOUR BITS OF THE SECOND QUADWORD, not in
     * the first byte. A command written the obvious way round is read as
     * opcode zero, which is reserved, and the IOMMU stops. */
    c[1] = ((uint64_t)opcode << 60) | (uint64_t)arg1;
    __sync_synchronize();
    g_cmd_tail = (g_cmd_tail + 16) % (AMDV_CMD_ENTRIES * 16);
    rw(AMDV_CMD_TAIL, g_cmd_tail);
}

/* A completion wait with the store bit set: the IOMMU writes a value to an
 * address when it has finished everything queued before it. That is the only
 * way to know an invalidation has actually taken effect. */
static volatile uint64_t g_wait_sem __attribute__((aligned(8)));

static bool amdv_wait(void) {
    g_wait_sem = 0;
    uint64_t pa = dma((const void *)&g_wait_sem);
    uint32_t idx = g_cmd_tail / 16;
    uint64_t *c = &g_cmdbuf[idx * 2];
    /* Store address in bits 3..51 of the first quadword, with bit 0 set to
     * ask for the store. */
    c[0] = (pa & ~0x7ULL) | 1;
    c[1] = ((uint64_t)CMD_COMPLETION_WAIT << 60) | ((pa >> 32) & 0xFFFFF);
    /* The value to store goes in the second half -- for a 32-bit store it is
     * the low half of the upper quadword, which this uses as a marker. */
    __sync_synchronize();
    g_cmd_tail = (g_cmd_tail + 16) % (AMDV_CMD_ENTRIES * 16);
    rw(AMDV_CMD_TAIL, g_cmd_tail);

    for (int i = 0; i < 2000000; i++) {
        if (rr(AMDV_CMD_HEAD) == g_cmd_tail) return true;
        __asm__ volatile("" ::: "memory");
    }
    return false;
}

bool amd_iommu_init(void) {
    if (g_up) return true;
    const struct acpi_sdt_header *ivrs = acpi_find_table("IVRS");
    if (!ivrs) return false;

    /* Walk the IVRS for an IVHD entry; its type is 0x10, 0x11 or 0x40 and the
     * MMIO base is at a fixed offset in all three. */
    const uint8_t *p = (const uint8_t *)ivrs + 48;
    const uint8_t *end = (const uint8_t *)ivrs + ivrs->length;
    uint64_t base = 0;
    while (p + 8 <= end) {
        uint8_t type = p[0];
        uint16_t len = (uint16_t)(p[2] | (p[3] << 8));
        if (len < 8 || p + len > end) break;
        if ((type == 0x10 || type == 0x11 || type == 0x40) && len >= 24) {
            for (int i = 0; i < 8; i++) base |= (uint64_t)p[8 + i] << (i * 8);
            break;
        }
        p += len;
    }
    if (!base) {
        kprintf("amd-iommu: IVRS present but names no remapping hardware\n");
        return false;
    }

    g_regs = (volatile uint8_t *)(uintptr_t)vmm_map_mmio(base, 0x4000);
    if (!g_regs) return false;

    /* Every device that exists gets an entry saying "valid, no translation
     * required, reads and writes allowed". Every device that does not have
     * one is refused -- which is the right default and is why the table is
     * built from the PCI scan rather than filled in wholesale. */
    memset(g_devtab, 0, sizeof g_devtab);
    uint32_t n = pci_devices_count();
    for (uint32_t i = 0; i < n; i++) {
        const struct pci_device *d = pci_get_device(i);
        if (!d) continue;
        uint32_t devid = ((uint32_t)d->bus << 8) |
                         ((uint32_t)d->device << 3) | d->function;
        if (devid >= AMDV_DEVTAB_ENTRIES) {
            kprintf("amd-iommu: device %04x is beyond this driver's table\n", devid);
            continue;
        }
        uint64_t *dte = &g_devtab[devid * 4];
        dte[0] = DTE_VALID | DTE_TV | DTE_IR | DTE_IW;
        dte[1] = 1;                 /* domain 1 */
        dte[2] = 0;
        dte[3] = 0;
        g_devices++;
    }

    memset(g_cmdbuf, 0, sizeof g_cmdbuf);
    memset(g_evtlog, 0, sizeof g_evtlog);
    g_cmd_tail = 0;

    /* THE COMMAND BUFFER BEFORE THE IOMMU. The first thing that needs doing
     * after enabling translation is an invalidation, and an invalidation is a
     * command -- so a driver that enables the IOMMU first has no way to issue
     * one. The size field is the log2 of the entry count. */
    rw(AMDV_DEVTAB_BASE, (dma(g_devtab) & ~0xFFFULL) |
                         ((AMDV_DEVTAB_BYTES / 4096) - 1));
    rw(AMDV_CMDBUF_BASE, (dma(g_cmdbuf) & ~0xFFFULL) | ((uint64_t)8 << 56));
    rw(AMDV_EVTLOG_BASE, (dma(g_evtlog) & ~0xFFFULL) | ((uint64_t)8 << 56));
    rw(AMDV_CMD_HEAD, 0);
    rw(AMDV_CMD_TAIL, 0);
    rw(AMDV_EVT_HEAD, 0);
    rw(AMDV_EVT_TAIL, 0);

    rw(AMDV_CONTROL, CTRL_CMDBUF_EN);
    rw(AMDV_CONTROL, CTRL_CMDBUF_EN | CTRL_EVENT_EN | CTRL_IOMMU_EN);

    /* And tell it that everything it may have cached about these devices is
     * stale, which it is: they were not in a table at all a moment ago. */
    for (uint32_t i = 0; i < n; i++) {
        const struct pci_device *d = pci_get_device(i);
        if (!d) continue;
        uint32_t devid = ((uint32_t)d->bus << 8) |
                         ((uint32_t)d->device << 3) | d->function;
        if (devid < AMDV_DEVTAB_ENTRIES)
            amdv_command(CMD_INVALIDATE_DEVTAB, devid, 0);
    }
    if (!amdv_wait())
        kprintf("amd-iommu: the completion wait never finished; continuing\n");

    g_up = true;
    kprintf("amd-iommu: %u device(s) in domain 1, translation ON\n", g_devices);
    return true;
}

bool     amd_iommu_present(void) { return g_up; }
uint32_t amd_iommu_devices(void) { return g_devices; }
