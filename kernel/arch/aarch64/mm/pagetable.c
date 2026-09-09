#include "arch/aarch64/mm/pagetable.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "include/kprintf.h"
#include "include/kstring.h"

/* See pagetable.h. Arm ARM D8.3 for the descriptor formats. */

/* boot.S's tables. We do not replace them -- these ARE the kernel's tables. */
extern uint64_t boot_l0_ttbr0[512];
extern uint64_t boot_l0_ttbr1[512];

/* Linker-script section boundaries, all virtual. */
extern char __text_start[],   __text_end[];
extern char __rodata_start[], __rodata_end[];
extern char __data_start[],   __data_end[];
extern char __bss_start[],    __bss_end[];
extern char __kernel_start[], __kernel_end[];

/* boot.S hardcodes these because the assembler cannot include pmm.h. This is
 * the promised cross-check: this file can see both, so the duplication is
 * verified at compile time instead of trusted. */
_Static_assert(((KERNEL_VIRTUAL_BASE >> 39) & 0x1FF) == 511, "boot.S L0_IDX_KERNEL");
_Static_assert(((DIRECT_MAP_BASE     >> 39) & 0x1FF) == 256, "boot.S L0_IDX_DIRECT");
_Static_assert(((MMIO_BASE           >> 39) & 0x1FF) == 384, "boot.S L0_IDX_MMIO");

#define PTE_VALID   (1ULL << 0)
#define PTE_TABLE   (1ULL << 1)   /* at L0-L2: table. At L3: "page".        */
#define PTE_AF      (1ULL << 10)
#define PTE_NG      (1ULL << 11)
#define PTE_PXN     (1ULL << 53)
#define PTE_UXN     (1ULL << 54)

#define PTE_ATTR(i) ((uint64_t)(i) << 2)
#define PTE_AP(v)   ((uint64_t)(v) << 6)
#define PTE_SH(v)   ((uint64_t)(v) << 8)

#define AP_RW_EL1   0
#define AP_RW_ALL   1
#define AP_RO_EL1   2
#define AP_RO_ALL   3

#define SH_NONE     0
#define SH_INNER    3

#define MAIR_DEVICE 0
#define MAIR_NORMAL 1
#define MAIR_NC     2   /* Normal non-cacheable -- see boot.S's MAIR_VALUE */

/* Output address field of any descriptor: bits [47:12]. */
#define PTE_ADDR_MASK 0x0000FFFFFFFFF000ULL

#define ENTRIES 512

/* Bytes one entry maps, per level: 512 GiB, 1 GiB, 2 MiB, 4 KiB. */
static uint64_t level_size(int level) {
    return 1ULL << (39 - 9 * level);
}

static uint32_t level_index(uint64_t va, int level) {
    return (uint32_t)((va >> (39 - 9 * level)) & 0x1FF);
}

/* A descriptor at L0-L2 with bit 1 clear is a BLOCK: it maps memory directly
 * instead of pointing at the next table. At L3 the same bit pattern means
 * "invalid", and bit 1 SET means "page" -- the encoding is reused with an
 * opposite sense, which is the single easiest thing to get wrong here. */
static bool is_table(uint64_t d, int level) {
    return level < 3 && (d & PTE_VALID) && (d & PTE_TABLE);
}
static bool is_block(uint64_t d, int level) {
    return level >= 1 && level <= 2 && (d & PTE_VALID) && !(d & PTE_TABLE);
}

static uint64_t *table_at(uint64_t phys) {
    return (uint64_t *)(uintptr_t)P2V(phys);
}

/* --- TLB maintenance -------------------------------------------------------
 * The barriers are the point, not the tlbi. A table write is an ordinary store
 * that the page-table walker -- a separate observer -- is not guaranteed to
 * see without a dsb, and instruction fetch is not guaranteed to notice the
 * invalidation without an isb. Omitting either produces a fault or a stale
 * translation that reproduces intermittently and looks like anything else. */
static void tlb_flush_page(uint64_t va) {
    __asm__ volatile("dsb ishst" ::: "memory");
    __asm__ volatile("tlbi vaae1is, %0" :: "r"(va >> 12) : "memory");
    __asm__ volatile("dsb ish; isb" ::: "memory");
}

static void tlb_flush_all(void) {
    __asm__ volatile("dsb ishst" ::: "memory");
    __asm__ volatile("tlbi vmalle1is" ::: "memory");
    __asm__ volatile("dsb ish; isb" ::: "memory");
}

/* The kernel's own root -- TTBR1's level-0 table. Never changes. */
static uint64_t kernel_root_phys(void) {
    return KV2P((uint64_t)(uintptr_t)boot_l0_ttbr1);
}

/* Whatever TTBR0_EL1 currently points at: THE address space this CPU is in.
 * Read from the register rather than cached in a variable, so it cannot drift
 * out of step with the hardware -- the one source of truth about which process
 * is mapped is the register the MMU is using. */
static uint64_t user_root_phys(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(v));
    return v & PTE_ADDR_MASK;
}

/* Which root translates this address. Bit 63 decides, in hardware -- so
 * unlike x86, a kernel address and a user address are never candidates for the
 * same table, and a process's tables physically cannot describe kernel memory. */
static uint64_t root_phys_for(uint64_t va) {
    return (va >> 63) ? kernel_root_phys() : user_root_phys();
}

static uint64_t flags_to_pte(uint32_t flags) {
    uint64_t d = PTE_VALID | PTE_AF;

    if (flags & PT_DEVICE)
        d |= PTE_ATTR(MAIR_DEVICE) | PTE_SH(SH_NONE);
    else if (flags & PT_WC)
        d |= PTE_ATTR(MAIR_NC) | PTE_SH(SH_INNER);
    else
        d |= PTE_ATTR(MAIR_NORMAL) | PTE_SH(SH_INNER);

    if (flags & PT_USER)
        d |= PTE_AP((flags & PT_WRITE) ? AP_RW_ALL : AP_RO_ALL) | PTE_NG;
    else
        d |= PTE_AP((flags & PT_WRITE) ? AP_RW_EL1 : AP_RO_EL1);

    /* Execute permission is expressed as two separate NEVER bits, and both
     * have to be considered every time. A kernel page must always be UXN:
     * without it, user code that reaches a kernel address could execute it,
     * and on cores without a hardware defence that is the whole of a
     * privilege escalation. */
    if (flags & PT_USER) {
        d |= PTE_PXN;                          /* EL1 never executes user memory */
        if (!(flags & PT_EXEC))
            d |= PTE_UXN;
    } else {
        d |= PTE_UXN;
        if (!(flags & PT_EXEC))
            d |= PTE_PXN;
    }
    return d;
}

/* Turn a block descriptor into a table of the next level down describing the
 * same memory with the same attributes. Nothing about the mapping changes --
 * only its granularity -- so this is always safe to do to live mappings,
 * including the one containing the code that is running. */
static int split_block(uint64_t *slot, int level) {
    uint64_t old = *slot;
    uint64_t phys = pmm_alloc_page();
    if (!phys)
        return PT_ERR_NOMEM;

    uint64_t *nt = table_at(phys);
    uint64_t base = old & PTE_ADDR_MASK;
    uint64_t attrs = old & ~PTE_ADDR_MASK & ~PTE_TABLE;
    uint64_t step = level_size(level + 1);

    for (int i = 0; i < ENTRIES; i++) {
        uint64_t d = (base + (uint64_t)i * step) | attrs;
        /* At L3 the descriptor must additionally set bit 1 to mean "page". */
        if (level + 1 == 3)
            d |= PTE_TABLE;
        nt[i] = d;
    }

    __asm__ volatile("dsb ishst" ::: "memory");
    *slot = phys | PTE_VALID | PTE_TABLE;
    tlb_flush_all();
    return PT_OK;
}

/* Walk to the level-3 entry for `va`, creating tables (and splitting blocks)
 * when `create` is set. Returns a pointer to the slot, or NULL. */
static uint64_t *walk_in(uint64_t root_phys, uint64_t va, bool create, int *err) {
    uint64_t *tbl = table_at(root_phys);

    for (int level = 0; level < 3; level++) {
        uint64_t *slot = &tbl[level_index(va, level)];

        if (is_block(*slot, level)) {
            if (!create) {
                if (err) *err = PT_ERR_NOTMAPPED;
                return 0;                       /* mapped, but coarser than 4K */
            }
            int rc = split_block(slot, level);
            if (rc != PT_OK) {
                if (err) *err = rc;
                return 0;
            }
        } else if (!is_table(*slot, level)) {
            if (!create) {
                if (err) *err = PT_ERR_NOTMAPPED;
                return 0;
            }
            uint64_t phys = pmm_alloc_page();
            if (!phys) {
                if (err) *err = PT_ERR_NOMEM;
                return 0;
            }
            memset(table_at(phys), 0, PAGE_SIZE);
            __asm__ volatile("dsb ishst" ::: "memory");
            *slot = phys | PTE_VALID | PTE_TABLE;
        }

        tbl = table_at(*slot & PTE_ADDR_MASK);
    }

    return &tbl[level_index(va, 3)];
}

static uint64_t *walk(uint64_t va, bool create, int *err) {
    return walk_in(root_phys_for(va), va, create, err);
}

int vm_map_page(uint64_t va, uint64_t pa, uint32_t flags) {
    if ((va | pa) & (PAGE_SIZE - 1))
        return PT_ERR_ALIGN;

    int err = PT_OK;
    uint64_t *slot = walk(va, true, &err);
    if (!slot)
        return err;

    *slot = (pa & PTE_ADDR_MASK) | flags_to_pte(flags) | PTE_TABLE;
    tlb_flush_page(va);
    return PT_OK;
}

int vm_map_range(uint64_t va, uint64_t pa, uint64_t size, uint32_t flags) {
    if ((va | pa | size) & (PAGE_SIZE - 1))
        return PT_ERR_ALIGN;

    for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
        int rc = vm_map_page(va + off, pa + off, flags);
        if (rc != PT_OK)
            return rc;
    }
    return PT_OK;
}

int vm_unmap_page(uint64_t va) {
    int err = PT_OK;
    uint64_t *slot = walk(va, false, &err);
    if (!slot || !(*slot & PTE_VALID))
        return PT_ERR_NOTMAPPED;

    *slot = 0;
    tlb_flush_page(va);
    return PT_OK;
}

static uint64_t vm_translate_in(uint64_t root_phys, uint64_t va) {
    uint64_t *tbl = table_at(root_phys);

    for (int level = 0; level < 4; level++) {
        uint64_t d = tbl[level_index(va, level)];

        if (!(d & PTE_VALID))
            return 0;

        if (level == 3)
            return (d & PTE_ADDR_MASK) | (va & (PAGE_SIZE - 1));

        if (is_block(d, level)) {
            uint64_t sz = level_size(level);
            return (d & PTE_ADDR_MASK & ~(sz - 1)) | (va & (sz - 1));
        }
        tbl = table_at(d & PTE_ADDR_MASK);
    }
    return 0;
}

uint64_t vm_translate(uint64_t va) {
    return vm_translate_in(root_phys_for(va), va);
}

static int protect_section(const char *name, char *start, char *end, uint32_t flags) {
    uint64_t va = (uint64_t)(uintptr_t)start;
    uint64_t sz = (uint64_t)(uintptr_t)end - va;

    if (sz == 0)
        return PT_OK;

    int rc = vm_map_range(va, KV2P(va), sz, flags);
    if (rc != PT_OK) {
        kprintf("vm: FAILED to protect %s (%d)\n", name, rc);
        return rc;
    }
    kprintf("vm: %-8s %p..%p  %s%s%s\n", name,
            (void *)(uintptr_t)va, (void *)(uintptr_t)(va + sz),
            "r",
            (flags & PT_WRITE) ? "w" : "-",
            (flags & PT_EXEC)  ? "x" : "-");
    return PT_OK;
}

int vm_protect_kernel_sections(void) {
    int rc;

    /* Order matters only in that .text is remapped while executing from it.
     * That is safe: vm_map_page writes one leaf entry and flushes one page, and
     * the mapping it installs still permits execution -- it merely stops
     * permitting writes. */
    if ((rc = protect_section(".text",   __text_start,   __text_end,
                              PT_READ | PT_EXEC)) != PT_OK) return rc;
    if ((rc = protect_section(".rodata", __rodata_start, __rodata_end,
                              PT_READ)) != PT_OK) return rc;
    if ((rc = protect_section(".data",   __data_start,   __data_end,
                              PT_READ | PT_WRITE)) != PT_OK) return rc;
    if ((rc = protect_section(".bss",    __bss_start,    __bss_end,
                              PT_READ | PT_WRITE)) != PT_OK) return rc;

    tlb_flush_all();
    return PT_OK;
}

void vm_drop_identity_map(void) {
    /* Only entry 0 was ever populated: 4 GiB of identity map through one
     * level-0 slot. */
    boot_l0_ttbr0[0] = 0;
    __asm__ volatile("dsb ishst" ::: "memory");
    tlb_flush_all();
}

/* Map one page into a SPECIFIC address space, creating tables as needed. */
static int vm_map_page_in(uint64_t root_phys, uint64_t va, uint64_t pa,
                          uint32_t flags) {
    if ((va | pa) & (PAGE_SIZE - 1))
        return PT_ERR_ALIGN;

    int err = PT_OK;
    uint64_t *slot = walk_in(root_phys, va, true, &err);
    if (!slot)
        return err;

    *slot = (pa & PTE_ADDR_MASK) | flags_to_pte(flags) | PTE_TABLE;
    tlb_flush_page(va);
    return PT_OK;
}

void vm_init(void) {
    kprintf("vm: kernel image %p..%p (%d KiB), phys %p\n",
            (void *)__kernel_start, (void *)__kernel_end,
            (int)(((uint64_t)(uintptr_t)__kernel_end -
                   (uint64_t)(uintptr_t)__kernel_start) / 1024),
            (void *)(uintptr_t)KV2P((uint64_t)(uintptr_t)__kernel_start));
}

void vm_dump_kernel_mapping(void) {
    struct { const char *name; char *p; } probes[] = {
        { ".text",   __text_start   },
        { ".rodata", __rodata_start },
        { ".data",   __data_start   },
        { ".bss",    __bss_start    },
    };

    kprintf("vm: translations resolved by walking the live tables:\n");
    for (unsigned i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        uint64_t va = (uint64_t)(uintptr_t)probes[i].p;
        uint64_t pa = vm_translate(va);
        kprintf("vm:   %-8s %p -> %p %s\n", probes[i].name,
                (void *)(uintptr_t)va, (void *)(uintptr_t)pa,
                pa == KV2P(va) ? "(matches KV2P)" : "*** MISMATCH ***");
    }
}

/* --- kernel/mm/vmm.h, the shared VM interface ------------------------------
 *
 * The x86 implementation is kernel/arch/x86_64/mm/vmm.c (moved there from
 * kernel/mm/ once it was clear it is PML4 code, not portable code). This is
 * the aarch64 half, and today it is exactly as much of that header as shared
 * code actually calls: `kernel/mm/kheap.c` maps one page at a time as the heap
 * grows, and nothing else outside arch/ calls into vmm.h at all.
 *
 * TWO THINGS ABOUT THE FLAGS ARE x86 SHOWING THROUGH, and both are recorded in
 * docs/TODO.md rather than papered over:
 *
 *   VMM_PRESENT / VMM_WRITABLE / VMM_NX are not abstract names -- they ARE the
 *   x86 page-table bit positions (0, 1, and 63). aarch64 reads them as an
 *   ordinary flag word, which works, but the header is describing one
 *   machine's hardware and calling it an interface.
 *
 *   VMM_NX is INVERTED with respect to how a permission should read: absent
 *   means executable. So a caller that asks for a writable page and says
 *   nothing about execution gets a WRITABLE, EXECUTABLE page -- which is what
 *   the kernel heap is on x86 today. This implementation does NOT reproduce
 *   that: a mapping here is non-executable unless something explicitly asks
 *   otherwise, and nothing can yet, because vmm.h has no way to say "I want to
 *   run code here". A W^X heap is the right default and the wrong place to
 *   inherit a bug from.
 */
int vmm_map(uint64_t virt, uint64_t phys, uint64_t flags) {
    uint32_t f = 0;

    if (flags & VMM_WRITABLE)
        f |= PT_WRITE;
    if (flags & VMM_USER)
        f |= PT_USER;
    if (flags & (VMM_NOCACHE | VMM_WRITETHROUGH))
        f |= PT_DEVICE;

    /* No PT_EXEC: see the note above. */
    return vm_map_page(virt, phys, f) == PT_OK ? 0 : -1;
}

void vmm_unmap(uint64_t virt) {
    vm_unmap_page(virt);
}

uint64_t vmm_get_phys(uint64_t virt) {
    return vm_translate(virt);
}

void vmm_flush_tlb(uint64_t virt) {
    tlb_flush_page(virt);
}

/* --- address spaces, kernel/mm/vmm.h --------------------------------------
 *
 * THE SIMPLEST PART OF THIS PORT, and worth saying why. On x86 a process's
 * PML4 must contain the KERNEL's mappings too -- every address space carries a
 * copy of the top half, they have to be kept in step, and switching address
 * spaces means reloading a root that describes both halves at once. Here
 * TTBR1_EL1 holds the kernel and TTBR0_EL1 holds the process, permanently and
 * separately. A process address space is therefore a single level-0 table
 * describing nothing but that process, creating it is one zeroed page, and
 * switching is one register write that cannot possibly disturb the kernel's
 * own mappings.
 *
 * vmm.h calls the handle a `pml4_phys`. It is the physical address of a
 * level-0 table here. The name is x86's; docs/TODO.md has the rename. */

/* A kernel stack window: bump-allocated VA with an unmapped guard page below
 * each stack, so an overflow faults instead of quietly eating its neighbour.
 * Level-0 index 508, chosen because 511 (kernel window), 384 (MMIO) and 256
 * (direct map) are taken and 508 is not. */
#define KSTACK_VA_BASE 0xFFFFFE0000000000ULL
static uint64_t kstack_va_next = KSTACK_VA_BASE;

uint64_t vmm_create_address_space(void) {
    uint64_t root = pmm_alloc_page();
    if (!root)
        return 0;

    /* Zero IS the address space: every entry invalid, nothing mapped. No
     * kernel half to copy in, because there is no kernel half in TTBR0. */
    memset(table_at(root), 0, PAGE_SIZE);
    __asm__ volatile("dsb ishst" ::: "memory");
    return root;
}

/* Recursively free a table and everything under it. Only ever called on a
 * TTBR0 root, so everything it can reach is that process's own -- there is no
 * shared kernel half to accidentally walk into, which is the failure mode the
 * x86 version has to be careful about. */
static void destroy_table(uint64_t table_phys, int level) {
    uint64_t *t = table_at(table_phys);

    for (int i = 0; i < ENTRIES; i++) {
        uint64_t d = t[i];
        if (!(d & PTE_VALID))
            continue;

        if (is_table(d, level))
            destroy_table(d & PTE_ADDR_MASK, level + 1);
        else
            pmm_free_page(d & PTE_ADDR_MASK);   /* a leaf: the frame itself */
    }
    pmm_free_page(table_phys);
}

void vmm_destroy_address_space(uint64_t root_phys) {
    if (!root_phys)
        return;

    /* Never tear down the address space we are standing in: the next
     * instruction fetch would translate through freed tables. */
    if (root_phys == user_root_phys())
        vmm_switch_address_space(vmm_get_kernel_pml4());

    destroy_table(root_phys, 0);
}

void vmm_switch_address_space(uint64_t root_phys) {
    /* One register. The kernel's mappings are in TTBR1 and are not touched,
     * so unlike a CR3 reload this cannot invalidate the code doing the switch.
     *
     * The full TLB invalidate is the price of not having allocated ASIDs yet
     * (docs/TODO.md): with an ASID per address space the hardware would keep
     * both processes' entries and this would need no flush at all. */
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"(root_phys) : "memory");
    __asm__ volatile("isb" ::: "memory");
    tlb_flush_all();
}

uint64_t vmm_get_kernel_pml4(void) {
    /* "The address space with no process in it." On x86 this is the kernel's
     * own PML4; here it is the empty TTBR0 root the boot code left behind once
     * the identity map was dropped. Switching to it has the same meaning --
     * no user mappings are reachable -- which is all the callers want. */
    return KV2P((uint64_t)(uintptr_t)boot_l0_ttbr0);
}

int vmm_map_in(uint64_t root_phys, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint32_t f = 0;
    if (flags & VMM_WRITABLE) f |= PT_WRITE;
    if (flags & VMM_USER)     f |= PT_USER;
    if (flags & (VMM_NOCACHE | VMM_WRITETHROUGH)) f |= PT_DEVICE;
    /* VMM_NX is inverted on x86 (absent means executable) and this deliberately
     * does not reproduce that -- see the note on vmm_map above. A user mapping
     * that must run code has to say so, and vmm.h cannot yet. Until it can,
     * user TEXT is mapped through vm_map_page() with PT_EXEC directly. */
    return vm_map_page_in(root_phys, virt, phys, f) == PT_OK ? 0 : -1;
}

void vmm_unmap_in(uint64_t root_phys, uint64_t virt) {
    int err = PT_OK;
    uint64_t *slot = walk_in(root_phys, virt, false, &err);
    if (slot && (*slot & PTE_VALID)) {
        *slot = 0;
        tlb_flush_page(virt);
    }
}

uint64_t vmm_get_phys_in(uint64_t root_phys, uint64_t virt) {
    return vm_translate_in(root_phys, virt);
}

uint64_t vmm_alloc_kernel_stack(uint64_t size) {
    uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (!pages)
        return 0;

    /* One unmapped page below the stack. It is never mapped, so a thread that
     * runs off the bottom of its stack takes a translation fault at a known
     * address instead of writing into whatever VA happens to precede it -- and
     * A1's decoder names it. Leaving a hole is cheaper than any check. */
    uint64_t guard = kstack_va_next;
    uint64_t base  = guard + PAGE_SIZE;
    kstack_va_next = base + pages * PAGE_SIZE + PAGE_SIZE;
    (void)guard;

    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys)
            return 0;             /* VA is bump-allocated and not reclaimed --
                                   * same simplification vmm.h documents */
        memset((void *)(uintptr_t)P2V(phys), 0, PAGE_SIZE);
        if (vm_map_page(base + i * PAGE_SIZE, phys, PT_WRITE) != PT_OK)
            return 0;
    }
    return base + pages * PAGE_SIZE;    /* the TOP: stacks grow down */
}

void vmm_free_kernel_stack(uint64_t stack_top, uint64_t size) {
    uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t base  = stack_top - pages * PAGE_SIZE;

    for (uint64_t i = 0; i < pages; i++) {
        uint64_t va = base + i * PAGE_SIZE;
        uint64_t phys = vm_translate(va);
        if (phys) {
            vm_unmap_page(va);
            pmm_free_page(phys);
        }
    }
}

/* --- the MMIO and kmap windows, kernel/mm/vmm.h ---------------------------
 *
 * boot.S maps the first gigabyte of physical space as Device memory at
 * MMIO_BASE, which is enough for the PL011, the GIC and the virtio-mmio
 * transports. It is NOT enough for everything: QEMU `virt` puts the PCIe ECAM
 * window at 0x4010000000 and its 64-bit MMIO aperture above 0x8000000000, so
 * a device discovered over PCIe in A7 needs a mapping made at runtime.
 *
 * Both windows bump-allocate virtual addresses and never reclaim them, which
 * is the same simplification vmm.h documents for the x86 side. Physical pages
 * ARE reclaimed; it is only the address space that is spent, and there is
 * 512 GiB of it per level-0 slot. */
#define MMIO_DYN_BASE  (MMIO_BASE + 0x40000000ULL)   /* above boot.S's 1 GiB block */
#define KMAP_VA_BASE   0xFFFFFD0000000000ULL         /* L0 slot 506, unused */

static uint64_t mmio_va_next = MMIO_DYN_BASE;
static uint64_t kmap_va_next = KMAP_VA_BASE;

static uint64_t map_mmio_common(uint64_t phys, uint64_t size, uint32_t flags) {
    if (!size)
        return 0;

    /* Round the range OUT to whole pages at both ends: a device register at
     * offset 0xF04 in a 0x100-byte request still has to be reachable. */
    uint64_t first = phys & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t last  = (phys + size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t pages = (last - first) / PAGE_SIZE;

    uint64_t va = mmio_va_next;
    mmio_va_next += pages * PAGE_SIZE + PAGE_SIZE;   /* +1 page of separation */

    for (uint64_t i = 0; i < pages; i++)
        if (vm_map_page(va + i * PAGE_SIZE, first + i * PAGE_SIZE, flags) != PT_OK)
            return 0;

    return va + (phys - first);      /* hand back the exact address asked for */
}

uint64_t vmm_map_mmio(uint64_t phys, uint64_t size) {
    /* Device-nGnRnE: no gathering, no reordering, no early write
     * acknowledgement. Registers only -- anything weaker and a write to a
     * command register can be split, merged or delayed past the read that
     * checks whether it took effect. */
    return map_mmio_common(phys, size, PT_WRITE | PT_DEVICE);
}

uint64_t vmm_map_mmio_wc(uint64_t phys, uint64_t size) {
    /* Normal non-cacheable, NOT Device: a framebuffer aperture is written with
     * ordinary stores, often unaligned and worth merging, and Device memory
     * forbids exactly that. Never use this for registers. */
    return map_mmio_common(phys, size, PT_WRITE | PT_WC);
}

void vmm_unmap_mmio(uint64_t virt, uint64_t size) {
    uint64_t first = virt & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t last  = (virt + size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

    for (uint64_t va = first; va < last; va += PAGE_SIZE)
        vm_unmap_page(va);
    /* The VA is not returned to a free list -- see the window comment. The
     * PHYSICAL pages were never ours: they are a device's. */
}

uint64_t vmm_kmap_pages(const uint64_t *phys, uint32_t n) {
    if (!phys || !n)
        return 0;

    uint64_t va = kmap_va_next;
    kmap_va_next += (uint64_t)n * PAGE_SIZE + PAGE_SIZE;

    /* Scattered frames, one contiguous kernel view -- what the compositor
     * wants for a surface whose pixel pages came from the allocator one at a
     * time. Cached and never executable: it is pixels. */
    for (uint32_t i = 0; i < n; i++)
        if (vm_map_page(va + (uint64_t)i * PAGE_SIZE, phys[i], PT_WRITE) != PT_OK)
            return 0;

    return va;
}

void vmm_kunmap_pages(uint64_t virt_base, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        vm_unmap_page(virt_base + (uint64_t)i * PAGE_SIZE);
}
