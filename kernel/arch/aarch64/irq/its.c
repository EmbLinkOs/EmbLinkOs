#include "arch/aarch64/irq/its.h"
#include "arch/aarch64/irq/gicv3.h"
#include "arch/aarch64/boot/fdt.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "mm/pmm.h"

/* The GIC Interrupt Translation Service -- MSI on aarch64.
 *
 * WHY THERE IS A WHOLE SERVICE FOR THIS. On x86 an MSI is a write to a fixed
 * doorbell (0xFEE00000 | apic<<12) whose DATA is the vector: the message
 * carries both the target and the number, and nothing has to be told anything
 * in advance. GICv3 does the opposite. A device writes its EVENT ID to ONE
 * address -- GITS_TRANSLATER -- and the ITS looks up who wrote it (by the
 * device's RID, which the bus supplies, not the device) and translates
 * (DeviceID, EventID) into (LPI INTID, target redistributor). That table has
 * to be BUILT, per device, before the first write.
 *
 * Which is why arch_pci_msi_message() had to learn the PCI address: an MSI
 * message here cannot be computed from a vector and a CPU alone. That is a
 * real difference between the machines, not an inconvenience.
 *
 * THREE THINGS LIVE IN MEMORY THE ITS READS BY DMA, and all three are in .bss
 * for the reason every DMA structure in this tree is: KV2P() works on the
 * kernel image window and not on the heap.
 *
 *   the command queue     what we ask it to do, 32 bytes per command
 *   the device table      DeviceID -> that device's translation table
 *   an ITT per device     EventID  -> (LPI, collection)
 *
 * A "collection" is the ITS's name for a target: MAPC binds one to a
 * redistributor, and MAPTI points an event at a collection rather than at a
 * CPU directly, so retargeting is one command instead of one per interrupt.
 */

#define GITS_CTLR        0x0000
#define GITS_TYPER       0x0008
#define GITS_CBASER      0x0080
#define GITS_CWRITER     0x0088
#define GITS_CREADR      0x0090
#define GITS_BASER0      0x0100
#define GITS_TRANSLATER  0x10040

#define ITS_CMD_MOVI     0x01
#define ITS_CMD_INT      0x03
#define ITS_CMD_SYNC     0x05
#define ITS_CMD_MAPD     0x08
#define ITS_CMD_MAPC     0x09
#define ITS_CMD_MAPTI    0x0A
#define ITS_CMD_INV      0x0C

#define BASER_TYPE_DEVICE      1
#define BASER_TYPE_COLLECTION  4

#define CMDQ_BYTES   (64u * 1024u)      /* CBASER is 64 KiB aligned          */
#define TABLE_BYTES  (64u * 1024u)
#define ITT_BYTES    256u               /* 32 events per device is plenty    */
#define MAX_DEVS     8

/* Command queue and tables. 64 KiB alignment is the register's, not a
 * preference: CBASER/BASER carry the address in bits [51:12] but the ITS
 * requires the region itself to be page aligned and sized. */
static uint8_t cmdq[CMDQ_BYTES]   __attribute__((aligned(65536)));
static uint8_t dev_tab[TABLE_BYTES]  __attribute__((aligned(65536)));
static uint8_t coll_tab[TABLE_BYTES] __attribute__((aligned(65536)));
static uint8_t itt[MAX_DEVS][ITT_BYTES] __attribute__((aligned(256)));

static volatile uint8_t *its;
static uint64_t its_phys;              /* for GITS_TRANSLATER's address     */
static bool     its_up;
static uint32_t cmd_write;             /* our index into cmdq, in commands  */
static uint32_t next_lpi = GIC_LPI_BASE;
static uint32_t next_itt;
static uint32_t mapped_dev[MAX_DEVS];  /* DeviceIDs already MAPD'd           */
static uint32_t mapped_count;
static uint32_t max_devid;             /* what the device table can index    */


static inline uint64_t rd64(uint32_t o) { return *(volatile uint64_t *)(its + o); }
static inline void     wr64(uint32_t o, uint64_t v) { *(volatile uint64_t *)(its + o) = v; }
static inline uint32_t rd32(uint32_t o) { return *(volatile uint32_t *)(its + o); }
static inline void     wr32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(its + o) = v; }

/* How MAPC and SYNC name a redistributor, which is NOT a free choice: it is
 * GITS_TYPER.PTA that decides.
 *
 *   PTA = 1  the target is the redistributor's PHYSICAL ADDRESS, in bits
 *            [51:16] of the command word.
 *   PTA = 0  the target is a PROCESSOR NUMBER -- GICR_TYPER's, not an address
 *            at all -- in bits [50:16].
 *
 * Guessing wrong is silent. The ITS accepts the command, reports the queue
 * drained, and translates every subsequent event to a collection pointing at
 * nothing; the MSI is mapped, enabled, triggered, and never arrives. Which is
 * exactly what happened here before this was read. */
static uint64_t its_target(void) {
    uint64_t typer = rd64(GITS_TYPER);
    if (typer & (1ULL << 19))               /* PTA */
        return gic_redistributor_phys() & ~0xFFFFULL;
    return (uint64_t)gic_processor_number() << 16;
}

/* Push one 32-byte command and wait for the ITS to consume it.
 *
 * Synchronous on purpose. These happen a handful of times at boot, never on an
 * interrupt path, and a queue that is written but never checked is how a
 * MAPTI that the ITS rejected becomes an interrupt that silently never
 * arrives. */
static bool cmd_submit(uint64_t d0, uint64_t d1, uint64_t d2, uint64_t d3) {
    uint64_t *c = (uint64_t *)(cmdq + (uint64_t)cmd_write * 32);
    c[0] = d0; c[1] = d1; c[2] = d2; c[3] = d3;
    __asm__ volatile("dsb sy" ::: "memory");

    cmd_write = (cmd_write + 1) % (CMDQ_BYTES / 32);
    wr64(GITS_CWRITER, (uint64_t)cmd_write * 32);

    for (int spins = 0; spins < 10000000; spins++) {
        if ((rd64(GITS_CREADR) & ~0x1FULL) == (uint64_t)cmd_write * 32)
            return true;
        __asm__ volatile("yield" ::: "memory");
    }
    kprintf("its: command queue never drained\n");
    return false;
}

static bool its_sync(void) {
    return cmd_submit(ITS_CMD_SYNC, 0, its_target(), 0);
}

/* Point one BASER at a table we allocated. The ITS negotiates: it reports what
 * it wants (entry size, page size) and accepts what it can. Everything here
 * uses 64 KiB pages because that is what the tables are aligned and sized to,
 * and the value is READ BACK -- a BASER that did not take the write is a table
 * the ITS is not using, and it would look exactly like a working one. */
static bool set_baser(int index, uint8_t want_type, void *table) {
    uint32_t off = GITS_BASER0 + (uint32_t)index * 8;
    uint64_t b = rd64(off);
    uint8_t  type = (uint8_t)((b >> 56) & 0x7);
    if (type != want_type)
        return false;

    b &= ~((0xFFFFFFFFFFULL << 12) | (0x3ULL << 8) | (0x3ULL << 10) | 0xFFULL);
    b |= KV2P((uint64_t)(uintptr_t)table);
    b |= (1ULL << 63);                 /* Valid                              */
    b |= (2ULL << 8);                  /* Page size = 64 KiB                 */
    b |= (1ULL << 10);                 /* Shareability = Inner Shareable     */
    b |= (TABLE_BYTES / (64 * 1024)) - 1;   /* Size, in pages minus one      */
    wr64(off, b);

    uint64_t back = rd64(off);
    if (!(back & (1ULL << 63))) {
        kprintf("its: BASER%d (type %d) refused the table\n", index, (int)want_type);
        return false;
    }

    /* HOW MANY DEVICES THIS TABLE CAN ACTUALLY INDEX, which is not the same as
     * how many the ITS can address. GITS_TYPER.Devbits said 16 here -- 65536
     * DeviceIDs -- while the table is 64 KiB; at the entry size the ITS
     * reports, that indexes 8192 of them. A DeviceID past the end is not
     * rejected: MAPD is accepted, the queue drains, and every event for that
     * device translates to nothing. The first MSI simply never arrives, which
     * is precisely how this presented. */
    if (want_type == BASER_TYPE_DEVICE) {
        uint32_t entry_size = (uint32_t)(((back >> 48) & 0x1F) + 1);
        max_devid = (uint32_t)(TABLE_BYTES / entry_size);
        kprintf("its: device table %d KiB, %d bytes/entry -> DeviceIDs 0..%d\n",
                (int)(TABLE_BYTES / 1024), (int)entry_size, (int)max_devid - 1);
    }
    return true;
}

bool its_init(void) {
    fdt_node_t n = fdt_find_compatible("arm,gic-v3-its");
    if (n == FDT_NONE) {
        kprintf("its: no arm,gic-v3-its node -- MSI unavailable, INTx still works\n");
        return false;
    }

    uint64_t base = 0, size = 0;
    if (!fdt_reg(n, 0, &base, &size)) {
        kprintf("its: node has no reg\n");
        return false;
    }
    its_phys = base;
    its = (volatile uint8_t *)(uintptr_t)(MMIO_BASE + base);

    /* Disabled while its tables are described: a live ITS reading a BASER
     * mid-write is not a defined state. */
    wr32(GITS_CTLR, 0);
    __asm__ volatile("dsb sy" ::: "memory");

    memset(cmdq, 0, sizeof cmdq);
    memset(dev_tab, 0, sizeof dev_tab);
    memset(coll_tab, 0, sizeof coll_tab);
    memset(itt, 0, sizeof itt);

    bool have_dev = false, have_coll = false;
    for (int i = 0; i < 8; i++) {
        if (!have_dev  && set_baser(i, BASER_TYPE_DEVICE, dev_tab))      have_dev = true;
        else if (!have_coll && set_baser(i, BASER_TYPE_COLLECTION, coll_tab)) have_coll = true;
    }
    if (!have_dev) {
        kprintf("its: no device table BASER\n");
        return false;
    }
    /* A collection table is OPTIONAL: an implementation may keep collections
     * entirely in hardware, and QEMU does. Its absence is not a failure. */

    wr64(GITS_CBASER,
         KV2P((uint64_t)(uintptr_t)cmdq)
         | (1ULL << 63)                        /* Valid                     */
         | (1ULL << 10)                        /* Inner Shareable           */
         | ((CMDQ_BYTES / (4 * 1024)) - 1));   /* Size, 4 KiB pages minus 1 */
    wr64(GITS_CWRITER, 0);
    cmd_write = 0;

    wr32(GITS_CTLR, 1);                        /* Enabled                   */
    __asm__ volatile("dsb sy" ::: "memory");

    /* One collection, bound to THIS core's redistributor. Every MSI targets
     * it. Spreading interrupts across cores is a MOVI command away and is a
     * policy decision nothing here needs yet. */
    if (!cmd_submit(ITS_CMD_MAPC, 0, its_target() | (1ULL << 63), 0)) {
        kprintf("its: MAPC failed\n");
        return false;
    }

    its_up = true;
    kprintf("its: at %p, command queue %d KiB, collection 0 -> this core\n",
            (void *)(uintptr_t)base, (int)(CMDQ_BYTES / 1024));
    return true;
}

bool its_present(void) { return its_up; }

/* Give this device an LPI and return the message that delivers it.
 *
 * `devid` is the PCI RID -- bus<<8 | dev<<3 | fn -- which is what the host
 * bridge puts on the bus and therefore what the ITS sees. QEMU's `virt`
 * declares that identity mapping in the DT's msi-map, and it is the only
 * mapping this machine uses. */
bool its_map_msi(uint32_t devid, uint32_t *out_intid, uint64_t *out_addr,
                 uint32_t *out_data)
{
    if (!its_up || mapped_count >= MAX_DEVS || next_lpi >= GIC_LPI_BASE + 64)
        return false;

    if (max_devid && devid >= max_devid) {
        kprintf("its: DeviceID %d is past the device table (%d entries)\n",
                (int)devid, (int)max_devid);
        return false;
    }

    /* MAPD once per device: it points the ITS at this device's own
     * translation table. Doing it twice for one device would hand it a second
     * ITT and orphan every event already mapped in the first. */
    bool known = false;
    for (uint32_t i = 0; i < mapped_count; i++)
        if (mapped_dev[i] == devid) { known = true; break; }

    if (!known) {
        uint32_t slot = next_itt++;
        /* MAPD's size field is the number of EventID BITS minus one. */
        if (!cmd_submit(ITS_CMD_MAPD | ((uint64_t)devid << 32),
                        5 - 1,
                        KV2P((uint64_t)(uintptr_t)itt[slot]) | (1ULL << 63),
                        0))
            return false;
        mapped_dev[mapped_count++] = devid;
    }

    uint32_t lpi = next_lpi++;

    /* MAPTI: (DeviceID, EventID 0) -> this LPI, on collection 0. */
    if (!cmd_submit(ITS_CMD_MAPTI | ((uint64_t)devid << 32),
                    (uint64_t)lpi << 32,          /* EventID 0, pINTID = lpi */
                    0,                            /* collection 0            */
                    0))
        return false;

    /* The LPI configuration table is memory the redistributor caches, so
     * enabling an LPI is not visible until INV tells it to re-read. */
    if (!cmd_submit(ITS_CMD_INV | ((uint64_t)devid << 32), 0, 0, 0))
        return false;
    its_sync();

    *out_intid = lpi;
    *out_addr  = its_phys + GITS_TRANSLATER;
    *out_data  = 0;                    /* the EventID we mapped              */
    return true;
}

/* --- proving it -----------------------------------------------------------
 *
 * "The ITS initialised" means its tables were accepted and its command queue
 * drains. It does NOT mean a translation works or that anything is delivered,
 * and those are the parts that can be wrong: a MAPTI the ITS quietly declined,
 * an LPI left disabled in a configuration table the redistributor never
 * re-read, a collection pointed at the wrong redistributor. Every one of those
 * looks exactly like a working ITS until a device raises its first MSI, at
 * which point nothing happens.
 *
 * So the ITS is asked to deliver one ITSELF. The INT command takes a
 * (DeviceID, EventID), translates it exactly as a device's write to
 * GITS_TRANSLATER would, and sets the resulting LPI pending. That exercises
 * the whole path -- device table, ITT, collection, redistributor, LPI
 * configuration table, CPU interface -- with no device involved and nothing
 * to destabilise. A real MSI differs only in who does the writing.
 */
static volatile uint64_t its_test_hits;
static void its_test_handler(uint32_t intid) { (void)intid; its_test_hits++; }

bool its_selftest(void) {
    if (!its_up)
        return false;

    /* A DeviceID no real device has, but one the device table can INDEX.
     * `virt` puts every device on bus 0, so PCI RIDs here are below 256; 2048
     * is clear of them and well inside the table. Picking a large "obviously
     * unused" number instead (0x7FFF was the first attempt) puts it past the
     * table's end, where MAPD is accepted and nothing is ever translated. */
    const uint32_t devid = 2048;

    uint32_t intid = 0, data = 0;
    uint64_t addr = 0;
    if (!its_map_msi(devid, &intid, &addr, &data)) {
        kprintf("  [FAIL] its: could not map a test MSI\n");
        return false;
    }
    gic_register_lpi(intid, its_test_handler, "ITS self-test");

    /* The configuration table changed; the redistributor caches it. */
    cmd_submit(ITS_CMD_INV | ((uint64_t)devid << 32), 0, 0, 0);
    its_sync();

    uint64_t before = its_test_hits;
    if (!cmd_submit(ITS_CMD_INT | ((uint64_t)devid << 32), 0, 0, 0)) {
        kprintf("  [FAIL] its: INT command was not accepted\n");
        return false;
    }
    its_sync();

    /* The interrupt is delivered asynchronously; wait briefly with interrupts
     * unmasked rather than assuming it has already arrived. */
    for (int spins = 0; spins < 20000000 && its_test_hits == before; spins++)
        __asm__ volatile("yield" ::: "memory");

    bool ok = its_test_hits > before;
    kprintf("  [%s] its: software-triggered MSI -> LPI %d %s\n",
            ok ? " ok " : "FAIL", (int)intid,
            ok ? "delivered" : "NEVER ARRIVED");
    return ok;
}
