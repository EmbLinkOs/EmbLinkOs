#include "acpi/acpi.h"
#include <stddef.h>   /* offsetof: FADT field presence */
#include "drivers/char/serial.h"
#include "mm/pmm.h"
#include "include/kprintf.h"
#include "include/kstring.h"   /* memset, memcpy: the power parser */
#include "boot/boot_protocol.h"   /* boot_acpi_rsdp — UEFI RSDP */
#include <stdint.h>


static struct acpi_info info; // Global variable to hold parsed ACPI info


// Validate checksum of an ACPI table. Returns true if valid, false if invalid.
static bool checksum_ok(const void *ptr, uint64_t length) {
    const uint8_t *bytes = (const uint8_t *)ptr;
    uint8_t sum = 0;
    for (uint64_t i = 0; i < length; i++) {
        sum += bytes[i];
    }
    return sum == 0;
}

// Compare a 4-character signature to a string.
static bool sig_match(const char *a, const char *b) {
    return (a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3]);
}

// Scan a physical memory range for the RSDP signature. Returns pointer to RSDP if found, NULL if not found.
// Returns Virtual pointer to RSDP if found, NULL if not found.
static struct rsdp *scan_for_rsdp(uint64_t phys_start, uint64_t phys_end) {
    const char target[8] = {'R', 'S', 'D', ' ', 'P', 'T', 'R', ' '}; // "RSD PTR "

    for (uint64_t phys = phys_start; phys < phys_end; phys += 16) {
        char *candidate = (char *)P2V(phys); // Convert physical address to virtual address

        bool match = true;
        for (int i = 0; i < 8; i++) {
            if (candidate[i] != target[i]) {
                match = false;
                break;
            }
        }
        if (!match) {
            continue; // Signature does not match, keep scanning
        }
        // validate the ACPI 1.0 checksum (first 20 bytes)
        struct rsdp *r = (struct rsdp *)candidate;
        if (checksum_ok(r, 20)) {
            return r;
        }
    }
    return NULL;
}


static struct rsdp *find_rsdp() {
    // 0. The boot loader may have handed us the RSDP directly. Under UEFI it
    //    lives in the EFI configuration table, NOT the legacy BIOS memory the
    //    scans below cover -- so without this, a UEFI boot finds no ACPI at all
    //    (no HPET/IOAPIC, a bogus timer calibration, and a downstream hang).
    uint64_t rp = boot_acpi_rsdp();
    if (rp) {
        struct rsdp *r = (struct rsdp *)P2V(rp);
        const char target[8] = {'R', 'S', 'D', ' ', 'P', 'T', 'R', ' '};
        bool ok = true;
        for (int i = 0; i < 8; i++) if (((const char *)r)[i] != target[i]) { ok = false; break; }
        if (ok && checksum_ok(r, 20))
            return r;
        kprintf("ACPI: loader RSDP at %p invalid, falling back to scan\n", (void *)rp);
    }

    // 1. First try the EBDA (Extended BIOS Data Area) which is often at 0x80000 or 0x90000 0x40E in the BIOS data area contains the segment base of the EBDA. The EBDA is typically 1KB in size, so we scan that range.
    uint16_t ebda_segment = *((uint16_t *)P2V(0x40E)); // EBDA segment is stored at 0x40E in the BIOS data area
    uint64_t ebda_phys = ((uint64_t)ebda_segment) << 4; // Convert segment to physical address
    if (ebda_phys) {
        struct rsdp *r = scan_for_rsdp(ebda_phys, ebda_phys + 1024); // Scan first 1KB of EBDA
        if (r) {
            return r;
        }
    }

    // 2. If not found in EBDA, scan the upper memory area from 0xE0000 to 0xFFFFF
    return scan_for_rsdp(0xE0000, 0x100000);
}   


// Given the RSDP, find a table by its 4-character signature. Returns pointer to the table if found and valid, NULL if not found or invalid.
// If ACPI 2.0+ is supported, uses the XSDT which has 64-bit pointers. If only ACPI 1.0 is supported, uses the RSDT which has 32-bit pointers.
/* The n-th table (0-based) carrying `signature`. There is ONE of most tables and
 * several of some -- a machine commonly has a handful of SSDTs, and \_S5_ may
 * be defined in any of them rather than in the DSDT. */
static struct acpi_sdt_header *find_table_nth(const struct rsdp *r, const char *signature, int nth);

static struct acpi_sdt_header *find_table(const struct rsdp *r, const char *signature) {
    return find_table_nth(r, signature, 0);
}

static struct acpi_sdt_header *find_table_nth(const struct rsdp *r, const char *signature, int nth) {
    bool use_xsdt = (r->revision >= 2) && (r->xsdt_address != 0);

    if (use_xsdt) {
        struct acpi_sdt_header *xsdt = (struct acpi_sdt_header *)P2V(r->xsdt_address);
        if (!checksum_ok(xsdt, xsdt->length)) {
            serial_write_string("ACPI: XSDT checksum invalid\n");
            return NULL;
        }

        // Number of entries in the XSDT is (length of XSDT - size of header) / 8 (size of each entry)
        uint32_t entries = (xsdt->length - sizeof(struct acpi_sdt_header)) / 8;
        uint64_t *table_ptrs = (uint64_t *)((uint8_t *)xsdt + sizeof(struct acpi_sdt_header));

        for (uint32_t i = 0; i < entries; i++) {
            struct acpi_sdt_header *t = (struct acpi_sdt_header *)P2V(table_ptrs[i]);
            if (checksum_ok(t, t->length) && sig_match(t->signature, signature)) {
                if (nth-- == 0)
                    return t;
            }
        }
    } else {
        struct acpi_sdt_header *rsdt = (struct acpi_sdt_header *)P2V(r->rsdt_address);
        if (!checksum_ok(rsdt, rsdt->length)) {
            serial_write_string("ACPI: RSDT checksum invalid\n");
            return NULL;
        }

        // Number of entries in the RSDT is (length of RSDT - size of header) / 4 (size of each entry)
        uint32_t entries = (rsdt->length - sizeof(struct acpi_sdt_header)) / 4;
        uint32_t *table_ptrs = (uint32_t *)((uint8_t *)rsdt + sizeof(struct acpi_sdt_header));

        for (uint32_t i = 0; i < entries; i++) {
            struct acpi_sdt_header *t = (struct acpi_sdt_header *)P2V(table_ptrs[i]);
            if (checksum_ok(t, t->length) && sig_match(t->signature, signature)) {
                if (nth-- == 0)
                    return t;
            }
        }
    
    }
    return NULL; // Table not found
}


static void parse_madt(struct madt *madt){
    info.local_apic_address = madt->local_apic_address;

    kprintf("MADT: Local APIC at %p, flages=%x\n",
         (void *)(uint64_t)madt->local_apic_address,(unsigned int)madt->flags);
    
    // Walk the variable-length array of MADT entries to find CPUs and I/O APICs
    // They start right after the madt structure's fixed fields.
    uint8_t *ptr = (uint8_t *)madt + sizeof(struct madt);
    uint8_t *end = (uint8_t *)madt + madt->header.length;

    while (ptr < end) {
        struct madt_entry_header *eh = (struct madt_entry_header *)ptr;

        if (eh->length == 0) {
            // Malformed - avoid infinite loop
            kprintf("MADT: zero-length entry, stopping\n");
            break;
        }
        switch (eh->type) {
            case MADT_TYPE_LOCAL_APIC: {
                struct madt_local_apic *la = (struct madt_local_apic *)ptr;
                bool enabled = (la->flags & 0x1) != 0;
                 kprintf(" CPU: acpi_id=%u apic_id=%u %s\n",
                    (unsigned int)la->acpi_processor_id, (unsigned int)la->apic_id,
                    enabled ? "enabled" : "disabled");
                if (enabled && info.cpu_count < ACPI_MAX_CPUS) {
                                    
                    info.cpu_apic_ids[info.cpu_count++] = la->apic_id;
                    } 
                break;
            }
            case MADT_TYPE_IO_APIC: {
                struct madt_io_apic *io = (struct madt_io_apic *)ptr;
                    kprintf(" I/O APIC: id=%u addr=%p gsi_base=%u\n",
                        (unsigned int)io->io_apic_id, (void *)(uint64_t)io->io_apic_address,
                        (unsigned int)io->global_system_interrupt_base);
                if (info.io_apic_count < ACPI_MAX_IO_APICS) {
                    info.io_apic_addresses[info.io_apic_count] = io->io_apic_address;
                    info.io_apic_gsi_bases[info.io_apic_count] = io->global_system_interrupt_base;
                    info.io_apic_count++;
                } 
                break;
            }
            case MADT_TYPE_INT_OVERRIDE: {
                struct madt_int_override *ov = (struct madt_int_override *)ptr;
                kprintf(" IRQ Override: src=%u -> gsi=%u flags=%x\n",
                    (unsigned int)ov->source, (unsigned int)ov->global_system_interrupt,
                    (unsigned int)ov->flags);
                /* STORE it (previously only printed): the routing side consults
                 * these via acpi_resolve_isa_irq() so an ISA IRQ lands on the
                 * GSI the firmware actually wired it to, with the right
                 * polarity/trigger -- rather than assuming GSI == IRQ. */
                if (info.int_override_count < ACPI_MAX_INT_OVERRIDES) {
                    struct acpi_int_override *dst =
                        &info.int_overrides[info.int_override_count++];
                    dst->source = ov->source;
                    dst->gsi    = ov->global_system_interrupt;
                    dst->flags  = ov->flags;
                }
                break;
            }

            default:
                kprintf(" ( entry type %u, length %u)\n",
                     (unsigned int)eh->type, (unsigned int)eh->length);
                break;
        }
        ptr += eh->length; // Move to the next entry
    }
}


/* ======================================================================
 * POWER: the FADT and \_S5_
 *
 * Powering off a PC is two numbers written to one register: SLP_TYP, which
 * says WHICH sleep state, and SLP_EN, which says "now". The register's address
 * is in the FADT. SLP_TYP for S5 (soft off) is NOT in any table -- it is the
 * first element of the \_S5_ object in the DSDT, which is AML bytecode, which
 * is why this kernel could not power off a real machine: it guessed SLP_TYP
 * from constants that are true of QEMU, Bochs and VirtualBox and of nothing
 * else.
 *
 * This is not an AML interpreter, and says so. \_S5_ is, on essentially every
 * machine, a NAMED PACKAGE OF INTEGER CONSTANTS -- Name (_S5, Package () {5, 5,
 * 0, 0}) -- which is data, not code, and can be decoded by recognising its
 * encoding exactly: NameOp, the name, PackageOp, a PkgLength, an element
 * count, and integer constants in any of the six ways AML writes one. What it
 * cannot handle is a \_S5_ that is a METHOD computing the package at run time;
 * that is rare, it is detected (no match), and the power code then says ACPI
 * gave it nothing rather than writing a guess. The full interpreter -- battery,
 * lid, thermal zones, sleep -- is the rest of the ACPI pillar (docs/PILLARS.md).
 * ====================================================================== */

static struct acpi_power_info g_power;
static bool g_power_parsed;

const struct acpi_power_info *acpi_power_info(void) {
    return g_power_parsed ? &g_power : NULL;
}

static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* One AML integer constant at p, bounded by end. Returns the bytes it takes,
 * or 0 if what is there is not an integer constant. */
static int aml_integer(const uint8_t *p, const uint8_t *end, uint64_t *out) {
    if (p >= end) return 0;
    switch (p[0]) {
    case 0x00: *out = 0;          return 1;                    /* ZeroOp      */
    case 0x01: *out = 1;          return 1;                    /* OneOp       */
    case 0xFF: *out = ~0ull;      return 1;                    /* OnesOp      */
    case 0x0A: if (end - p < 2) return 0; *out = p[1]; return 2;              /* BytePrefix  */
    case 0x0B: if (end - p < 3) return 0; *out = (uint64_t)p[1] | ((uint64_t)p[2] << 8); return 3;
    case 0x0C: if (end - p < 5) return 0; *out = rd32le(p + 1); return 5;     /* DWordPrefix */
    case 0x0E: if (end - p < 9) return 0;
               *out = (uint64_t)rd32le(p + 1) | ((uint64_t)rd32le(p + 5) << 32); return 9;
    default:   return 0;
    }
}

/* A PkgLength at p. It counts ITSELF as well as what follows, and is 1 to 4
 * bytes: the top two bits of the first byte say how many follow. */
static int aml_pkglength(const uint8_t *p, const uint8_t *end, uint32_t *len) {
    if (p >= end) return 0;
    int follow = p[0] >> 6;
    if (end - p < 1 + follow) return 0;
    if (follow == 0) { *len = p[0] & 0x3Fu; return 1; }
    uint32_t v = p[0] & 0x0Fu;
    for (int i = 0; i < follow; i++)
        v |= (uint32_t)p[1 + i] << (4 + 8 * i);
    *len = v;
    return 1 + follow;
}

/* Look for Name (\_S5_, Package () { SLP_TYPa, SLP_TYPb, ... }) in one table's
 * AML. Every requirement below is there to refuse something that merely
 * CONTAINS the four bytes "_S5_" -- a reference to it from inside a method, a
 * string, a longer name: it must be the name being DEFINED by a NameOp, the
 * object must be a package, and both values must fit the 3-bit field they go
 * into. */
static bool find_s5(const struct acpi_sdt_header *t, uint8_t *typa, uint8_t *typb) {
    const uint8_t *aml = (const uint8_t *)t + sizeof(struct acpi_sdt_header);
    const uint8_t *end = (const uint8_t *)t + t->length;
    for (const uint8_t *p = aml; p + 4 <= end; p++) {
        if (p[0] != '_' || p[1] != 'S' || p[2] != '5' || p[3] != '_')
            continue;
        bool defined = (p - aml >= 1 && p[-1] == 0x08) ||                   /* NameOp _S5_   */
                       (p - aml >= 2 && p[-1] == '\\' && p[-2] == 0x08);   /* NameOp \_S5_  */
        if (!defined)
            continue;
        const uint8_t *q = p + 4;
        if (q >= end || *q != 0x12)                                         /* PackageOp     */
            continue;
        q++;
        uint32_t pkglen;
        int n = aml_pkglength(q, end, &pkglen);
        if (!n)
            continue;
        const uint8_t *pkg_end = q + pkglen;
        if (pkg_end > end)
            pkg_end = end;
        q += n;
        if (q >= pkg_end)
            continue;
        uint8_t nelem = *q++;
        uint64_t a, b;
        int c = aml_integer(q, pkg_end, &a);
        if (nelem < 1 || !c)
            continue;
        q += c;
        /* One element means "the same for PM1b"; that is how the spec reads a
         * short package, not a malformed one. */
        if (nelem < 2 || !aml_integer(q, pkg_end, &b))
            b = a;
        if (a > 7 || b > 7)
            continue;
        *typa = (uint8_t)a;
        *typb = (uint8_t)b;
        return true;
    }
    return false;
}

static bool fadt_has(const struct acpi_fadt *f, size_t off, size_t size) {
    return f->header.length >= off + size;
}

static void parse_power(const struct rsdp *r) {
    memset(&g_power, 0, sizeof g_power);
    g_power_parsed = true;

    const struct acpi_fadt *f = (const struct acpi_fadt *)find_table(r, "FACP");
    if (!f) {
        kprintf("ACPI: no FADT -- the tables say nothing about power\n");
        return;
    }
    g_power.fadt_found    = true;
    g_power.fadt_revision = f->header.revision;
    g_power.smi_cmd       = f->smi_cmd;
    g_power.acpi_enable   = f->acpi_enable;

    uint32_t flags = fadt_has(f, offsetof(struct acpi_fadt, flags), 4) ? f->flags : 0;
    g_power.hw_reduced = (flags & ACPI_FADT_HW_REDUCED) != 0;

    /* PM1 control: the 64-bit GAS when the table has one and it is filled in,
     * otherwise the ACPI 1.0 I/O port. Both describe the same register. */
    if (fadt_has(f, offsetof(struct acpi_fadt, x_pm1a_cnt_blk), sizeof(struct acpi_gas)) &&
        f->x_pm1a_cnt_blk.address) {
        g_power.pm1a_cnt = f->x_pm1a_cnt_blk;
    } else if (f->pm1a_cnt_blk) {
        g_power.pm1a_cnt.space_id  = ACPI_GAS_IO;
        g_power.pm1a_cnt.bit_width = (uint8_t)(f->pm1_cnt_len * 8);
        g_power.pm1a_cnt.address   = f->pm1a_cnt_blk;
    }
    if (fadt_has(f, offsetof(struct acpi_fadt, x_pm1b_cnt_blk), sizeof(struct acpi_gas)) &&
        f->x_pm1b_cnt_blk.address) {
        g_power.pm1b_cnt = f->x_pm1b_cnt_blk;
    } else if (f->pm1b_cnt_blk) {
        g_power.pm1b_cnt.space_id  = ACPI_GAS_IO;
        g_power.pm1b_cnt.bit_width = (uint8_t)(f->pm1_cnt_len * 8);
        g_power.pm1b_cnt.address   = f->pm1b_cnt_blk;
    }
    if (g_power.hw_reduced &&
        fadt_has(f, offsetof(struct acpi_fadt, sleep_control_reg), sizeof(struct acpi_gas)))
        g_power.sleep_control = f->sleep_control_reg;

    /* Reset: ACPI 2.0 gave the reset register a flag of its own, and the flag
     * is what counts -- plenty of tables carry a register they do not support. */
    if ((flags & ACPI_FADT_RESET_REG_SUP) &&
        fadt_has(f, offsetof(struct acpi_fadt, reset_value), 1) && f->reset_reg.address) {
        g_power.reset_supported = true;
        g_power.reset_reg       = f->reset_reg;
        g_power.reset_value     = f->reset_value;
    }

    /* \_S5_: the DSDT first -- the FADT points at it, it is never in the
     * XSDT -- then every SSDT. */
    uint64_t dsdt_phys = 0;
    if (fadt_has(f, offsetof(struct acpi_fadt, x_dsdt), 8) && f->x_dsdt)
        dsdt_phys = f->x_dsdt;
    else
        dsdt_phys = f->dsdt;
    if (dsdt_phys) {
        const struct acpi_sdt_header *d = (const struct acpi_sdt_header *)P2V(dsdt_phys);
        if (sig_match(d->signature, "DSDT") && checksum_ok((void *)d, d->length) &&
            find_s5(d, &g_power.s5_typa, &g_power.s5_typb)) {
            g_power.s5_found = true;
            memcpy(g_power.s5_table, "DSDT", 5);
        }
    }
    for (int i = 0; !g_power.s5_found && i < 32; i++) {
        const struct acpi_sdt_header *t = find_table_nth(r, "SSDT", i);
        if (!t)
            break;
        if (find_s5(t, &g_power.s5_typa, &g_power.s5_typb)) {
            g_power.s5_found = true;
            memcpy(g_power.s5_table, "SSDT", 5);
        }
    }

    const char *space[] = { "mem", "io", "pci" };
    kprintf("ACPI: FADT rev %u%s: PM1a_CNT %s 0x%llx%s, reset %s",
            (unsigned)g_power.fadt_revision, g_power.hw_reduced ? " (hardware-reduced)" : "",
            g_power.pm1a_cnt.space_id < 3 ? space[g_power.pm1a_cnt.space_id] : "?",
            (unsigned long long)g_power.pm1a_cnt.address,
            g_power.pm1b_cnt.address ? " + PM1b" : "",
            g_power.reset_supported ? "" : "not supported\n");
    if (g_power.reset_supported)
        kprintf("%s 0x%llx <- 0x%x\n",
                g_power.reset_reg.space_id < 3 ? space[g_power.reset_reg.space_id] : "?",
                (unsigned long long)g_power.reset_reg.address, (unsigned)g_power.reset_value);
    if (g_power.s5_found)
        kprintf("ACPI: \\_S5_ found in the %s: SLP_TYPa=%u SLP_TYPb=%u -- power-off is real\n",
                g_power.s5_table, (unsigned)g_power.s5_typa, (unsigned)g_power.s5_typb);
    else
        kprintf("ACPI: no \\_S5_ package in the DSDT or any SSDT -- power-off falls back\n");
}

const struct acpi_info *acpi_init(void) {
    serial_write_string("\n=== ACPI init ===\n");

    // Zero the info struct before filling it in
    info.found = false;
    info.local_apic_address = 0;
    info.cpu_count = 0;
    info.io_apic_count = 0;
    info.int_override_count = 0;
    info.hpet_found = false;
    info.hpet_address = 0;
    info.hpet_minimum_tick = 0;


    struct rsdp *r = find_rsdp();
    if (!r) {
        kprintf("ACPI: RSDP not found!\n");
        return &info;
    }
    kprintf("ACPI: RSDP found at %p, revision %u\n", (void *)r, (unsigned int)r->revision);
    kprintf("ACPI: %s\n", r->revision >= 2 ? "using XSDT (ACPI 2.0+)" : "using RSDT (ACPI 1.0)");

    /* Before the MADT, whose absence returns early: a machine with no MADT
     * still has to be able to switch itself off. */
    parse_power(r);
    

    struct acpi_sdt_header *madt_hdr = find_table(r, "APIC");
    if (!madt_hdr) {
        kprintf("ACPI: MADT not found\n");
        return &info;
    }

    // Validate MADT checksum and length before parsing
    if (!checksum_ok(madt_hdr, madt_hdr->length)) {
        kprintf("ACPI: MADT checksum invalid\n");
        return &info;
    }

    
    parse_madt((struct madt *)madt_hdr);

    /* Look for the HPET table (optional). */
    struct acpi_sdt_header *hpet_hdr = find_table(r, "HPET");
    if (hpet_hdr && checksum_ok(hpet_hdr, hpet_hdr->length) &&
        hpet_hdr->length >= sizeof(struct acpi_hpet)) {
        struct acpi_hpet *ht = (struct acpi_hpet *)hpet_hdr;
        if (ht->base_address && ht->gas_addr_space == 0 /* memory */) {
            info.hpet_found       = true;
            info.hpet_address     = ht->base_address;
            info.hpet_minimum_tick = ht->minimum_tick;
            kprintf("ACPI: HPET at phys %p, minimum_tick=%u\n",
                    (void *)info.hpet_address,
                    (unsigned int)info.hpet_minimum_tick);
        }
    } else {
        kprintf("ACPI: no valid HPET table\n");
    }

    info.found = true;
    kprintf("ACPI: found %u CPU(s), %u IO-APIC(s)\n",
         (unsigned int)info.cpu_count, (unsigned int)info.io_apic_count); 
    return &info;
}


const struct acpi_info *acpi_get_info(void) {
    return &info;
}

void acpi_resolve_isa_irq(uint8_t isa_irq, uint32_t *out_gsi,
                          bool *out_active_low, bool *out_level) {
    /* ISA defaults, used when no override names this IRQ: the line goes to the
     * GSI of the same number, edge-triggered, active-high. */
    uint32_t gsi        = isa_irq;
    bool     active_low = false;
    bool     level      = false;

    for (uint32_t i = 0; i < info.int_override_count; i++) {
        if (info.int_overrides[i].source != isa_irq)
            continue;

        gsi = info.int_overrides[i].gsi;

        /* MPS INTI flags. Polarity in bits[1:0], trigger in bits[3:2]; a value
         * of 00 means "conforms to the bus default", which for ISA is
         * active-high + edge. Only the explicit "active low" (11b) / "level"
         * (11b) encodings change anything. */
        uint16_t flags = info.int_overrides[i].flags;
        uint16_t pol = flags & 0x3;
        uint16_t trg = (flags >> 2) & 0x3;
        active_low = (pol == 0x3);
        level      = (trg == 0x3);
        break;
    }

    if (out_gsi)        *out_gsi = gsi;
    if (out_active_low) *out_active_low = active_low;
    if (out_level)      *out_level = level;
}