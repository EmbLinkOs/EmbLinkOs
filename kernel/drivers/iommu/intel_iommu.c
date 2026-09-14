/* kernel/drivers/iommu/intel_iommu.c -- VT-d, the IOMMU a real x86 machine has.
 *
 * virtio_iommu.c drives the paravirtual one, which is what a VM offers.
 * THIS is the hardware: an Intel chipset's DMA remapping engine, described by
 * the ACPI DMAR table, present on essentially every x86 machine made since
 * about 2010 and switched off until an operating system turns it on.
 *
 * The structure is a three-level lookup on the DEVICE, and then a page table:
 *
 *   the ROOT TABLE is indexed by PCI bus number and points at
 *   a CONTEXT TABLE indexed by device and function, whose entry names
 *   a DOMAIN and points at that domain's SECOND-LEVEL PAGE TABLE,
 *   which is walked with the device's address exactly like a processor
 *   page table -- and an address with no entry is refused.
 *
 * WHAT MAKES THIS DIFFERENT FROM A PROCESSOR PAGE TABLE, and it is the thing
 * to get right: THE CACHES ARE NOT COHERENT WITH MEMORY. The engine caches
 * context entries and translations, and it has no idea that the driver just
 * wrote a table. Every structural change must be followed by an explicit
 * invalidation, or the hardware keeps using what it remembers -- which on a
 * freshly-enabled engine means it keeps using nothing, and every device on
 * the machine stops.
 *
 * AND, LIKE virtio_iommu.c: THIS CAN STOP THE MACHINE BY WORKING. Enabling
 * translation is a single bit, and the instant it is set every DMA on the
 * system goes through these tables. They have to be complete first.
 *
 * The mapping installed is an identity one over the low 4 GiB, for the same
 * reason and with the same honesty as the virtio driver: it protects nothing
 * yet, and what it buys is that the machinery is real.
 */
#include <stdint.h>
#include <stddef.h>

#include "include/types.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/errno.h"
#include "acpi/acpi.h"
#include "drivers/bus/pci.h"
#include "drivers/iommu/intel_iommu.h"
#include "mm/vmm.h"
#include "mm/pmm.h"

/* Remapping hardware registers, from the base the DMAR table gives. */
#define DMAR_VER    0x000
#define DMAR_CAP    0x008
#define DMAR_ECAP   0x010
#define DMAR_GCMD   0x018
#define DMAR_GSTS   0x01C
#define DMAR_RTADDR 0x020
#define DMAR_CCMD   0x028
#define DMAR_FSTS   0x034
#define DMAR_FECTL  0x038

#define GCMD_TE   (1u << 31)   /* translation enable        */
#define GCMD_SRTP (1u << 30)   /* set root table pointer    */
#define GSTS_TES  (1u << 31)
#define GSTS_RTPS (1u << 30)

#define CCMD_ICC    (1ULL << 63)   /* invalidate context cache */
#define CCMD_CIRG_G (1ULL << 61)   /* global granularity       */

#define IOTLB_IVT   (1ULL << 63)
#define IOTLB_IIRG_G (1ULL << 60)

/* Second-level paging entry bits. Note there is no "present": READ or WRITE
 * being set IS presence, which is a small difference from a processor page
 * table and a large one if you assume otherwise. */
#define SL_READ  (1ULL << 0)
#define SL_WRITE (1ULL << 1)
#define SL_LARGE (1ULL << 7)

/* Context entry, low word. */
#define CTX_PRESENT (1ULL << 0)
#define CTX_TT_MULTILEVEL (0ULL << 2)

#define DMAR_MAX_UNITS 4
#define IDENTITY_LIMIT 0x100000000ULL   /* the low 4 GiB */

struct dmar_unit {
    volatile uint8_t *regs;
    uint64_t cap, ecap;
    uint32_t iotlb_off;
};

static struct dmar_unit g_units[DMAR_MAX_UNITS];
static uint32_t g_nunits;
static bool g_up;
static uint32_t g_contexts;

/* THE TABLES THE HARDWARE WALKS live in .bss and must be page-aligned: the
 * engine reads physical addresses out of them and takes only the top bits of
 * each pointer, so a misaligned table is silently rounded down to one that is
 * not there. */
static uint8_t  g_root[4096]        __attribute__((aligned(4096)));
static uint8_t  g_context[4096]     __attribute__((aligned(4096)));
static uint64_t g_pml4[512]         __attribute__((aligned(4096)));
static uint64_t g_pdpt[512]         __attribute__((aligned(4096)));
static uint64_t g_pd[4][512]        __attribute__((aligned(4096)));

static inline uint32_t rr32(struct dmar_unit *u, uint32_t o) {
    return *(volatile uint32_t *)(u->regs + o);
}
static inline void rw32(struct dmar_unit *u, uint32_t o, uint32_t v) {
    *(volatile uint32_t *)(u->regs + o) = v;
}
static inline uint64_t rr64(struct dmar_unit *u, uint32_t o) {
    return *(volatile uint64_t *)(u->regs + o);
}
static inline void rw64(struct dmar_unit *u, uint32_t o, uint64_t v) {
    *(volatile uint64_t *)(u->regs + o) = v;
}
static inline uint64_t dma(const volatile void *p) { return KV2P((uint64_t)(uintptr_t)p); }

/* Build the identity map: 4 GiB of 2 MiB pages. Three levels of table and
 * 2048 leaf entries, which is five pages of memory to describe four
 * gigabytes -- the reason large pages exist. */
static void build_identity(void) {
    memset(g_pml4, 0, sizeof g_pml4);
    memset(g_pdpt, 0, sizeof g_pdpt);
    memset(g_pd, 0, sizeof g_pd);

    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 512; j++) {
            uint64_t phys = ((uint64_t)i << 30) | ((uint64_t)j << 21);
            g_pd[i][j] = phys | SL_READ | SL_WRITE | SL_LARGE;
        }
        g_pdpt[i] = dma(g_pd[i]) | SL_READ | SL_WRITE;
    }
    g_pml4[0] = dma(g_pdpt) | SL_READ | SL_WRITE;
}

/* One context entry per (bus, device, function) that exists. The engine looks
 * the device up by its requester id, so only devices that are actually there
 * need entries -- an absent device cannot ask. */
static void build_tables(uint8_t agaw_field) {
    memset(g_root, 0, sizeof g_root);
    memset(g_context, 0, sizeof g_context);

    uint32_t n = pci_devices_count();
    for (uint32_t i = 0; i < n; i++) {
        const struct pci_device *d = pci_get_device(i);
        if (!d) continue;
        /* ONE CONTEXT TABLE, SHARED BY EVERY BUS. Each root entry may point
         * at its own, and on a machine with devices on many buses that is
         * what you would do; pointing them all at one table means bus 0
         * device 3 and bus 1 device 3 share a context entry. Safe here
         * because every entry names the same domain and the same page
         * table -- and noted, because it stops being safe the moment
         * different devices get different domains. */
        uint8_t devfn = (uint8_t)((d->device << 3) | d->function);
        uint64_t *ctx = (uint64_t *)(g_context + devfn * 16);
        if (ctx[0] & CTX_PRESENT) continue;
        ctx[0] = (dma(g_pml4) & ~0xFFFULL) | CTX_TT_MULTILEVEL | CTX_PRESENT;
        ctx[1] = ((uint64_t)1 << 8) |          /* domain 1 */
                 (uint64_t)(agaw_field & 7);   /* address width */
        g_contexts++;

        uint64_t *root = (uint64_t *)(g_root + d->bus * 16);
        root[0] = (dma(g_context) & ~0xFFFULL) | 1;   /* present */
        root[1] = 0;
    }
}

static void invalidate_all(struct dmar_unit *u) {
    /* CONTEXT CACHE FIRST, THEN THE TRANSLATION CACHE, and in that order --
     * flushing the translations first lets the engine refill them through a
     * context entry it has not re-read. */
    rw64(u, DMAR_CCMD, CCMD_ICC | CCMD_CIRG_G);
    for (int i = 0; i < 1000000; i++) {
        if (!(rr64(u, DMAR_CCMD) & CCMD_ICC)) break;
        __asm__ volatile("" ::: "memory");
    }
    /* THE IOTLB REGISTER IS NOT AT A FIXED ADDRESS. Its offset is in ECAP,
     * in sixteen-byte units, and it differs between chipsets -- which is why
     * a hard-coded offset works on the machine it was written on. */
    uint32_t io = u->iotlb_off;
    rw64(u, io + 8, IOTLB_IVT | IOTLB_IIRG_G);
    for (int i = 0; i < 1000000; i++) {
        if (!(rr64(u, io + 8) & IOTLB_IVT)) break;
        __asm__ volatile("" ::: "memory");
    }
}

bool intel_iommu_init(void) {
    if (g_up) return true;
    const struct acpi_sdt_header *dmar = acpi_find_table("DMAR");
    if (!dmar) return false;

    /* Walk the DMAR's remapping-hardware entries. Each one is a separate
     * engine covering some of the buses; a desktop has one, a server has
     * several. */
    const uint8_t *p = (const uint8_t *)dmar + 48;   /* past the DMAR header */
    const uint8_t *end = (const uint8_t *)dmar + dmar->length;
    while (p + 4 <= end && g_nunits < DMAR_MAX_UNITS) {
        uint16_t type = (uint16_t)(p[0] | (p[1] << 8));
        uint16_t len  = (uint16_t)(p[2] | (p[3] << 8));
        if (len < 4 || p + len > end) break;
        if (type == 0 && len >= 16) {             /* DRHD */
            uint64_t base = 0;
            for (int i = 0; i < 8; i++) base |= (uint64_t)p[8 + i] << (i * 8);
            struct dmar_unit *u = &g_units[g_nunits];
            u->regs = (volatile uint8_t *)(uintptr_t)vmm_map_mmio(base, 0x1000);
            if (u->regs) {
                u->cap = rr64(u, DMAR_CAP);
                u->ecap = rr64(u, DMAR_ECAP);
                u->iotlb_off = (uint32_t)(((u->ecap >> 8) & 0x3FF) * 16);
                g_nunits++;
            }
        }
        p += len;
    }
    if (!g_nunits) {
        kprintf("intel-iommu: DMAR present but no remapping hardware in it\n");
        return false;
    }

    /* WHICH ADDRESS WIDTH THE ENGINE SUPPORTS decides how many levels of page
     * table it will walk, and the context entry has to say the same number.
     * SAGAW bit 2 is 48-bit (four levels), bit 1 is 39-bit (three). This
     * builds four levels, so a chipset offering only three is refused rather
     * than handed a table it will misread. */
    uint32_t sagaw = (uint32_t)((g_units[0].cap >> 8) & 0x1F);
    if (!(sagaw & (1u << 2))) {
        kprintf("intel-iommu: the engine does not support 48-bit addressing "
                "(SAGAW %02x); this driver builds four-level tables only\n",
                sagaw);
        return false;
    }

    build_identity();
    build_tables(2 /* 48-bit, four levels */);

    for (uint32_t i = 0; i < g_nunits; i++) {
        struct dmar_unit *u = &g_units[i];
        /* Mask fault reporting: this driver polls nothing and a fault
         * interrupt with no handler is the interrupt-storm trap. */
        rw32(u, DMAR_FECTL, 1u << 31);
        rw32(u, DMAR_FSTS, rr32(u, DMAR_FSTS));

        rw64(u, DMAR_RTADDR, dma(g_root));
        rw32(u, DMAR_GCMD, GCMD_SRTP);
        bool ok = false;
        for (int spin = 0; spin < 1000000; spin++) {
            if (rr32(u, DMAR_GSTS) & GSTS_RTPS) { ok = true; break; }
            __asm__ volatile("" ::: "memory");
        }
        if (!ok) {
            kprintf("intel-iommu: unit %u never accepted the root table\n", i);
            return false;
        }

        /* THE CACHES REMEMBER NOTHING USEFUL AND MUST BE TOLD SO, before the
         * enable bit and not after: the engine is entitled to have cached
         * whatever was at these addresses beforehand. */
        invalidate_all(u);

        rw32(u, DMAR_GCMD, GCMD_TE);
        ok = false;
        for (int spin = 0; spin < 1000000; spin++) {
            if (rr32(u, DMAR_GSTS) & GSTS_TES) { ok = true; break; }
            __asm__ volatile("" ::: "memory");
        }
        if (!ok) {
            kprintf("intel-iommu: unit %u would not enable translation\n", i);
            return false;
        }
        invalidate_all(u);
    }

    g_up = true;
    kprintf("intel-iommu: %u unit(s), %u device(s) in domain 1, low 4 GiB "
            "identity-mapped, translation ON\n", g_nunits, g_contexts);
    return true;
}

bool     intel_iommu_present(void) { return g_up; }
uint32_t intel_iommu_units(void) { return g_nunits; }
uint32_t intel_iommu_devices(void) { return g_contexts; }
