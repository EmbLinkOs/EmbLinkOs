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

/* Which root translates this address. Bit 63 decides, in hardware. */
static uint64_t *root_for(uint64_t va) {
    return (va >> 63) ? boot_l0_ttbr1 : boot_l0_ttbr0;
}

static uint64_t flags_to_pte(uint32_t flags) {
    uint64_t d = PTE_VALID | PTE_AF;

    if (flags & PT_DEVICE)
        d |= PTE_ATTR(MAIR_DEVICE) | PTE_SH(SH_NONE);
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
static uint64_t *walk(uint64_t va, bool create, int *err) {
    uint64_t *tbl = root_for(va);

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

uint64_t vm_translate(uint64_t va) {
    uint64_t *tbl = root_for(va);

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
