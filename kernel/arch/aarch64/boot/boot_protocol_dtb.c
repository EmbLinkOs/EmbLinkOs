#include "boot/boot_protocol.h"
#include "arch/aarch64/boot/fdt.h"
#include "mm/pmm.h"
#include "include/kprintf.h"
#include "include/kstring.h"

/* The third producer of the boot protocol -- docs/ARM64.md §2.5, phase A2.
 *
 * BIOS stage2 and the UEFI loader FILL this record before the kernel starts.
 * There is no equivalent step here: QEMU hands us a device tree pointer and
 * nothing else, so the record is synthesised in-kernel by decoding the tree.
 * The rest of the kernel cannot tell the difference, which is the entire point
 * of the boot protocol existing -- kernel/mm/pmm.c reads its memory map
 * through boot_mmap_at() and needed no change at all for a second
 * architecture.
 *
 * What a device tree does NOT tell us, and the x86 loaders do:
 *   framebuffer  -- `virt` has no firmware framebuffer at all. virtio-gpu is
 *                   discovered over PCIe in A7 and fills these fields then.
 *   boot disk    -- likewise virtio; there is no BIOS drive number.
 *   ACPI RSDP    -- `virt` can present ACPI, but we read the device tree
 *                   instead. Left zero, which already means "unknown".
 * Zero is the honest value for each, and each already has a documented
 * "unknown" meaning in the protocol. */

#define MAX_MMAP 32

static struct boot_mmap_entry mmap[MAX_MMAP];
static uint32_t mmap_count;
static struct boot_protocol proto;
static bool captured;

static void add_range(uint64_t base, uint64_t len, uint32_t type) {
    if (len == 0)
        return;
    if (mmap_count >= MAX_MMAP) {
        /* Silently dropping a RAM range would produce a kernel that works and
         * is quietly short of memory, which is far worse than a loud one. */
        kprintf("boot: WARNING device tree has more than %d memory ranges; "
                "%p+%p DROPPED\n", MAX_MMAP,
                (void *)(uintptr_t)base, (void *)(uintptr_t)len);
        return;
    }
    mmap[mmap_count].base   = base;
    mmap[mmap_count].length = len;
    mmap[mmap_count].type   = type;
    mmap[mmap_count].attr   = 0;
    mmap_count++;
}

/* /reserved-memory children describe RAM inside /memory that firmware or
 * another agent owns. They are recorded here for pmm_print_map()'s benefit and
 * actually withheld from the allocator by arch_pmm_reserve_fixed(), because
 * pmm_init() frees usable ranges and never un-frees them -- a RESERVED entry
 * overlapping a USABLE one does not subtract. That asymmetry is a property of
 * the existing allocator, not of this producer, so it is handled here rather
 * than by changing an allocator that x86 depends on. */
static void add_reserved_memory_nodes(void) {
    fdt_node_t root = fdt_root();
    if (root == FDT_NONE)
        return;

    for (fdt_node_t n = fdt_first_child(root); n != FDT_NONE; n = fdt_next_sibling(n)) {
        if (strcmp(fdt_node_name(n), "reserved-memory") != 0)
            continue;
        for (fdt_node_t c = fdt_first_child(n); c != FDT_NONE; c = fdt_next_sibling(c)) {
            uint64_t a = 0, s = 0;
            for (uint32_t i = 0; fdt_reg(c, i, &a, &s); i++)
                add_range(a, s, E820_RESERVED);
        }
    }
}

void boot_protocol_capture(uint64_t phys)
{
    /* `phys` is the device tree pointer from x0, not a boot_protocol record.
     * Same entry point, same contract to the rest of the kernel, different
     * thing on the wire -- see the file comment. */
    if (fdt_init(phys) != 0) {
        kprintf("boot: FATAL no usable device tree at %p\n",
                (void *)(uintptr_t)phys);
        kprintf("boot: there is no fallback -- `virt` has no legacy memory map.\n");
        for (;;)
            __asm__ volatile("wfi");
    }

    mmap_count = 0;

    /* Every node with device_type = "memory" is RAM. `virt` emits one; a
     * NUMA machine emits several, and each can carry several reg entries. */
    fdt_node_t m = FDT_NONE;
    while ((m = fdt_find_device_type("memory", m)) != FDT_NONE) {
        uint64_t a = 0, s = 0;
        for (uint32_t i = 0; fdt_reg(m, i, &a, &s); i++) {
            add_range(a, s, E820_USABLE);
            kprintf("boot: dtb memory %p + %p\n",
                    (void *)(uintptr_t)a, (void *)(uintptr_t)s);
        }
    }

    if (mmap_count == 0) {
        kprintf("boot: FATAL device tree declares no memory\n");
        fdt_dump_nodes();     /* what DID it contain? */
        for (;;)
            __asm__ volatile("wfi");
    }

    add_reserved_memory_nodes();

    proto.magic       = BOOT_PROTOCOL_MAGIC;
    proto.version     = BOOT_PROTOCOL_VERSION;
    proto.size        = sizeof(proto);
    proto.firmware    = BOOT_FW_DTB;
    proto.mmap_count  = mmap_count;
    proto.mmap_phys   = KV2P((uint64_t)(uintptr_t)mmap);
    proto.mmap_stride = sizeof(struct boot_mmap_entry);
    proto.boot_drive  = 0xFF;      /* protocol's "unknown" */
    captured = true;
}

const struct boot_protocol *boot_protocol_get(void)
{
    return captured ? &proto : 0;
}

uint32_t boot_mmap_count(void)
{
    return captured ? mmap_count : 0;
}

const struct boot_mmap_entry *boot_mmap_at(uint32_t index)
{
    /* The entries are a kernel-owned array in .bss, not a loader buffer read
     * through KP2V, so unlike the x86 producer this stays valid for the life
     * of the kernel and has no ordering constraint against the PMM. */
    if (!captured || index >= mmap_count)
        return 0;
    return &mmap[index];
}

bool bootinfo_boot_disk_sig(uint32_t *out)
{
    (void)out;
    return false;      /* no BIOS disk signature exists on this machine */
}

uint8_t bootinfo_boot_drive(void)
{
    return 0xFF;
}

uint64_t boot_acpi_rsdp(void)
{
    return 0;          /* we read the device tree, not ACPI */
}

void boot_protocol_dump(void)
{
    if (!captured) {
        kprintf("boot: protocol not captured\n");
        return;
    }
    kprintf("boot: firmware=DTB version=%d entries=%d stride=%d\n",
            (int)proto.version, (int)proto.mmap_count, (int)proto.mmap_stride);
    fdt_dump_summary();
}
