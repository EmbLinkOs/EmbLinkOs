/* EmbLink UEFI loader.
 *
 * Does the same JOB as boot/stage2/stage2.asm (BIOS) in a completely different
 * environment: firmware hands us control already in 64-bit long mode with Boot
 * Services available, so instead of INT 13h / VBE / E820 / a hand-built climb to
 * long mode we use the file-less embedded kernel, GOP, GetMemoryMap, and a page
 * table we build in C. The OUTPUT is identical: the kernel entered at its
 * higher-half entry point with RDI = &boot_protocol.
 *
 * The one hard constraint we inherit from the kernel: KP2V hardwires the
 * higher-half window (0xFFFFFFFF80000000) to physical 0, so the kernel image
 * MUST live at its linked physical address (0x100000) and the page tables must
 * map higher-half -> phys 0, exactly like stage2 does.
 */
#include "uefi.h"
#include "console.h"
#include "menu.h"

/* ---- boot_protocol ABI (mirrors kernel/arch/x86_64/boot/boot_protocol.h;
 *      that header's _Static_asserts are the master copy -- keep in sync). --- */
#define BOOT_PROTOCOL_MAGIC   0x4F52504B4E494C45ULL   /* "ELINKPRO" */
#define BOOT_PROTOCOL_VERSION 1
#define BOOT_FW_UEFI          2

/* our memory-map type numbering (kernel pmm.h) */
#define E820_USABLE       1
#define E820_RESERVED     2
#define E820_ACPI_RECLAIM 3
#define E820_ACPI_NVS     4
#define E820_BAD_MEMORY   5

struct boot_mmap_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t attr;
};

struct boot_protocol {
    uint64_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t firmware;
    uint32_t mmap_count;
    uint64_t mmap_phys;
    uint32_t mmap_stride;
    uint32_t boot_disk_sig;
    uint64_t fb_addr;
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_pitch;
    uint32_t fb_bpp;
    uint32_t fb_format;
    uint8_t  boot_drive;
    uint8_t  _reserved[3];
    uint64_t acpi_rsdp;
};
_Static_assert(sizeof(struct boot_mmap_entry) == 24, "mmap entry 24");
_Static_assert(sizeof(struct boot_protocol)   == 0x50, "boot_protocol 0x50");

/* ---- ELF64 (only what we parse) ----------------------------------------- */
typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Elf64_Ehdr;
typedef struct {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} Elf64_Phdr;
#define PT_LOAD 1

/* The kernel is linked in the higher half; its true physical load address is
 * p_vaddr - KERNEL_VIRTUAL_BASE (which is what KP2V inverts). We must key off
 * p_vaddr, NOT p_paddr: the kernel's linker script leaves .bss with a p_paddr
 * that overlaps .data, so a p_paddr-based load zeroes .data. stage2 keys off
 * p_vaddr for exactly this reason. */
#define KERNEL_VIRTUAL_BASE 0xFFFFFFFF80000000ULL
#define KV_TO_PHYS(v) ((v) - KERNEL_VIRTUAL_BASE)

/* The kernel image, embedded by boot/uefi/kernel_blob.S. Hidden visibility so
 * the reference is a direct RIP-relative lea (position-independent), not an
 * absolute address frozen at the link-time base of 0. */
extern const uint8_t kernel_elf_start[] __attribute__((visibility("hidden")));
extern const uint8_t kernel_elf_end[]   __attribute__((visibility("hidden")));

/* ---- tiny freestanding helpers ------------------------------------------ */
static void *mini_memcpy(void *d, const void *s, uint64_t n) {
    uint8_t *dp = d; const uint8_t *sp = s;
    while (n--) *dp++ = *sp++;
    return d;
}
static void mini_memset(void *d, int c, uint64_t n) {
    uint8_t *dp = d;
    while (n--) *dp++ = (uint8_t)c;
}

/* Console I/O (con_print/con_printhex/con_die) + ST live in console.c now, so
 * the menu and the loader share one implementation. */

/* Map a firmware memory type to our e820-style numbering. Anything the loader
 * itself allocated is EfiLoaderData, which we mark RESERVED so the kernel's PMM
 * never frees the page tables / kernel image / protocol out from under it. */
static uint32_t efi_type_to_ours(uint32_t t) {
    switch (t) {
    case EfiConventionalMemory:
    case EfiBootServicesCode:
    case EfiBootServicesData:  return E820_USABLE;
    case EfiACPIReclaimMemory: return E820_ACPI_RECLAIM;
    case EfiACPIMemoryNVS:     return E820_ACPI_NVS;
    case EfiUnusableMemory:    return E820_BAD_MEMORY;
    default:                   return E820_RESERVED;  /* incl. Loader* + MMIO */
    }
}

static EFI_PHYSICAL_ADDRESS alloc_pages(EFI_ALLOCATE_TYPE how,
                                        EFI_PHYSICAL_ADDRESS at, UINTN pages,
                                        const char *what) {
    EFI_PHYSICAL_ADDRESS m = at;
    EFI_STATUS s = ST->BootServices->AllocatePages(how, EfiLoaderData, pages, &m);
    if (EFI_ERROR(s)) { con_print(what); con_die("AllocatePages failed"); }
    return m;
}

/* ---- page tables: identity map 0..4GB, plus higher-half 0xFFFFFFFF80000000
 *      -> phys 0..1GB, both via shared 2MB-page PDs. Mirrors stage2. -------- */
#define PTE_P   0x1
#define PTE_RW  0x2
#define PTE_PS  0x80   /* 2 MB huge page at PD level */

static uint64_t build_page_tables(void) {
    /* 7 contiguous 4KB pages: PML4, PDPT_low, PDPT_high, PD0..PD3. Kept below
     * 4GB so they sit inside the identity map we build (we touch them only
     * before the CR3 switch, but staying in-map is cheap insurance). */
    EFI_PHYSICAL_ADDRESS blk = alloc_pages(AllocateMaxAddress, 0xFFFFFFFF, 7,
                                           "pagetables: ");
    uint64_t *pml4  = (uint64_t *)(blk + 0x0000);
    uint64_t *pdpl  = (uint64_t *)(blk + 0x1000);   /* PDPT low  */
    uint64_t *pdph  = (uint64_t *)(blk + 0x2000);   /* PDPT high */
    uint64_t *pd[4] = { (uint64_t *)(blk + 0x3000), (uint64_t *)(blk + 0x4000),
                        (uint64_t *)(blk + 0x5000), (uint64_t *)(blk + 0x6000) };
    mini_memset((void *)blk, 0, 7 * 4096);

    /* 4 PDs x 512 huge pages = identity 0..4GB */
    for (int k = 0; k < 4; k++)
        for (int i = 0; i < 512; i++)
            pd[k][i] = ((uint64_t)k * 0x40000000ULL + (uint64_t)i * 0x200000ULL)
                       | PTE_P | PTE_RW | PTE_PS;

    /* identity: PML4[0] -> PDPT_low -> PD0..3 */
    pml4[0] = (uint64_t)pdpl | PTE_P | PTE_RW;
    for (int k = 0; k < 4; k++)
        pdpl[k] = (uint64_t)pd[k] | PTE_P | PTE_RW;

    /* higher half: PML4[511] -> PDPT_high[510] -> PD0 (0xFFFFFFFF80000000 ->
     * phys 0..1GB). Index 510 = (0xFFFFFFFF80000000 >> 30) & 0x1FF. */
    pml4[511] = (uint64_t)pdph | PTE_P | PTE_RW;
    pdph[510] = (uint64_t)pd[0] | PTE_P | PTE_RW;

    return blk;   /* physical address of the PML4 */
}

/* ---- load the embedded kernel ELF to its fixed physical addresses --------- */
static uint64_t load_kernel(void) {
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)kernel_elf_start;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F')
        con_die("embedded kernel is not ELF");

    const Elf64_Phdr *ph = (const Elf64_Phdr *)(kernel_elf_start + eh->e_phoff);

    /* pass 1: physical span of all PT_LOAD segments (physical = vaddr - KVB) */
    uint64_t lo = ~0ULL, hi = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0) continue;
        uint64_t p = KV_TO_PHYS(ph[i].p_vaddr);
        if (p < lo) lo = p;
        if (p + ph[i].p_memsz > hi) hi = p + ph[i].p_memsz;
    }
    if (hi == 0) con_die("kernel has no PT_LOAD segments");

    uint64_t base  = lo & ~0xFFFULL;
    uint64_t pages = (hi - base + 0xFFF) / 0x1000;
    /* Reserve the exact physical range the kernel is linked for. */
    alloc_pages(AllocateAddress, base, pages, "kernel image: ");

    /* pass 2: copy file data, zero bss -- destination keyed off p_vaddr so the
     * .bss segment (whose p_paddr overlaps .data) lands past .data, not on it. */
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0) continue;
        uint8_t *dst = (uint8_t *)KV_TO_PHYS(ph[i].p_vaddr);
        mini_memcpy(dst, kernel_elf_start + ph[i].p_offset, ph[i].p_filesz);
        if (ph[i].p_memsz > ph[i].p_filesz)
            mini_memset(dst + ph[i].p_filesz, 0, ph[i].p_memsz - ph[i].p_filesz);
    }
    return eh->e_entry;   /* higher-half virtual entry point */
}

static int guid_eq(const EFI_GUID *a, const EFI_GUID *b) {
    const uint8_t *pa = (const uint8_t *)a, *pb = (const uint8_t *)b;
    for (int i = 0; i < (int)sizeof(EFI_GUID); i++) if (pa[i] != pb[i]) return 0;
    return 1;
}

/* Find the ACPI RSDP in the EFI configuration table (2.0 GUID preferred -- it
 * carries the XSDT; fall back to 1.0). The kernel's legacy 0xE0000 scan finds
 * nothing under UEFI, so without this it comes up with no ACPI at all -- no
 * HPET, a bogus LAPIC-timer calibration, and a hang later on. */
static uint64_t find_acpi_rsdp(void) {
    EFI_GUID g20 = EFI_ACPI_20_TABLE_GUID, g10 = EFI_ACPI_10_TABLE_GUID;
    uint64_t v10 = 0;
    for (UINTN i = 0; i < ST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *e = &ST->ConfigurationTable[i];
        if (guid_eq(&e->VendorGuid, &g20)) return (uint64_t)e->VendorTable;
        if (guid_eq(&e->VendorGuid, &g10)) v10 = (uint64_t)e->VendorTable;
    }
    return v10;   /* 2.0 not found: 1.0 if present, else 0 */
}

static void fill_framebuffer(struct boot_protocol *bp) {
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
    EFI_STATUS s = ST->BootServices->LocateProtocol(&gop_guid, 0, (void **)&gop);
    if (EFI_ERROR(s) || !gop) { con_print("  (no GOP; fb left 0)\n"); return; }

    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi = gop->Mode->Info;
    bp->fb_addr   = gop->Mode->FrameBufferBase;
    bp->fb_width  = mi->HorizontalResolution;
    bp->fb_height = mi->VerticalResolution;
    bp->fb_bpp    = 32;
    bp->fb_pitch  = mi->PixelsPerScanLine * 4;
    bp->fb_format = (mi->PixelFormat == PixelRedGreenBlueReserved8BitPerColor)
                    ? 0 /* RGB */ : 1 /* BGR (OVMF default, and the safe guess) */;
}

/* Fill the memory map, ExitBootServices, and never touch firmware again. Runs
 * the classic retry loop: any allocation invalidates the map key, so we size,
 * allocate the firmware buffer, then re-read the map immediately before exit. */
static void finalize_and_handoff(struct boot_protocol *bp,
                                 struct boot_mmap_entry *mmap_out, UINTN mmap_cap,
                                 uint64_t pml4_phys, uint64_t entry,
                                 uint64_t bp_phys, EFI_HANDLE image) {
    EFI_BOOT_SERVICES *BS = ST->BootServices;
    UINTN map_size = 0, map_key = 0, desc_size = 0;
    uint32_t desc_ver = 0;
    EFI_MEMORY_DESCRIPTOR *map = 0;

    /* size it (expects BUFFER_TOO_SMALL), then over-allocate for the churn the
     * AllocatePool below itself adds. */
    BS->GetMemoryMap(&map_size, map, &map_key, &desc_size, &desc_ver);
    map_size += 8 * desc_size;
    if (EFI_ERROR(BS->AllocatePool(EfiLoaderData, map_size, (void **)&map)))
        con_die("AllocatePool for memory map");

    for (;;) {
        EFI_STATUS s = BS->GetMemoryMap(&map_size, map, &map_key,
                                        &desc_size, &desc_ver);
        if (EFI_ERROR(s)) con_die("GetMemoryMap");

        /* translate -> boot_mmap_entry (coalescing not needed; kernel handles
         * a sparse map). NO firmware allocation happens past this point. */
        UINTN n = map_size / desc_size, out = 0;
        for (UINTN i = 0; i < n && out < mmap_cap; i++) {
            EFI_MEMORY_DESCRIPTOR *d =
                (EFI_MEMORY_DESCRIPTOR *)((uint8_t *)map + i * desc_size);
            mmap_out[out].base   = d->PhysicalStart;
            mmap_out[out].length = d->NumberOfPages * 0x1000ULL;
            mmap_out[out].type   = efi_type_to_ours(d->Type);
            mmap_out[out].attr   = 0;
            out++;
        }
        bp->mmap_count = (uint32_t)out;

        s = BS->ExitBootServices(image, map_key);
        if (!EFI_ERROR(s)) break;   /* success: firmware services are gone */
        /* map changed under us between GetMemoryMap and here -- retry. */
    }

    /* Firmware is gone. Switch to our page tables and enter the kernel with the
     * protocol pointer in RDI (kentry.asm relays it to kernel_main). */
    __asm__ volatile (
        "cli\n"
        "mov %0, %%cr3\n"
        "mov %1, %%rdi\n"
        "jmp *%2\n"
        : : "r"(pml4_phys), "r"(bp_phys), "r"(entry) : "memory");
    __builtin_unreachable();
}

/* Boot EmbLinkOS: the OS-handoff backend. Loads the (embedded, for M1) kernel,
 * builds the boot_protocol + page tables, ExitBootServices, and jumps. Never
 * returns. This is the machinery the "Boot EmbLinkOS" and (later) "Recovery"
 * menu entries invoke; the menu itself lives in menu.c. */
void boot_emblinkos(EFI_HANDLE image) {
    con_print("\nBooting EmbLinkOS...\n");

    /* Structures the kernel reads via KP2V must live below 1GB (the extent of
     * the higher-half window we map). One page for the protocol, the rest for
     * the translated memory map. */
    EFI_PHYSICAL_ADDRESS lowblk = alloc_pages(AllocateMaxAddress, 0x40000000, 16,
                                              "low structs: ");
    struct boot_protocol   *bp   = (struct boot_protocol *)lowblk;
    struct boot_mmap_entry *mmap = (struct boot_mmap_entry *)(lowblk + 0x1000);
    UINTN mmap_cap = (15 * 4096) / sizeof(struct boot_mmap_entry);

    mini_memset(bp, 0, sizeof *bp);
    bp->version       = BOOT_PROTOCOL_VERSION;
    bp->size          = sizeof(struct boot_protocol);
    bp->firmware      = BOOT_FW_UEFI;
    bp->mmap_phys     = (uint64_t)mmap;
    bp->mmap_stride   = sizeof(struct boot_mmap_entry);
    bp->boot_disk_sig = 0;      /* not resolved yet (kernel probes all disks) */
    bp->boot_drive    = 0xFF;   /* n/a under UEFI */
    bp->acpi_rsdp     = find_acpi_rsdp();

    con_print("locating GOP...\n");
    fill_framebuffer(bp);
    con_print("  fb "); con_printhex(bp->fb_addr);
    con_print(" "); con_printhex(bp->fb_width); con_print("x"); con_printhex(bp->fb_height); con_print("\n");

    con_print("loading kernel...\n");
    uint64_t entry = load_kernel();
    con_print("  entry "); con_printhex(entry); con_print("\n");

    con_print("building page tables...\n");
    uint64_t pml4 = build_page_tables();

    /* magic LAST so a torn record fails the kernel's check instead of half-passing */
    bp->magic = BOOT_PROTOCOL_MAGIC;

    con_print("exiting boot services + jumping to kernel...\n");
    finalize_and_handoff(bp, mmap, mmap_cap, pml4, entry, (uint64_t)bp, image);
    __builtin_unreachable();
}

/* ---- EmbBoot entry point: run the menu, dispatch the choice ---------------- */
EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    con_init(st);

    for (;;) {
        int choice = menu_run();          /* draws the menu, returns an entry id */
        switch (choice) {
        case MENU_BOOT_EMBLINKOS:
            boot_emblinkos(image);        /* never returns */
            break;
        default:
            /* Recovery / Diagnostics / Firmware Update / Boot Manager / Secure
             * Boot Verification are not built yet -- menu.c shows "(soon)" and
             * returns here, so we simply re-draw the menu. */
            break;
        }
    }
}
