#ifndef _ACPI_H_
#define _ACPI_H_

#include "include/types.h"
#include <stdint.h>


// Common headers on every ACPI table (EXCEPT RSDP which has its own header)
struct acpi_sdt_header {
    char signature[4]; // ASCII signature to identify the table type
    uint32_t length;   // total length of the table, including this header
    uint8_t revision;  // ACPI version (1 for ACPI 1.0, 2 for ACPI 2.0+)
    uint8_t checksum;  // entire table must sum to zero
    char oem_id[6];    // OEM identifier string (padded with spaces)
    char oem_table_id[8]; // OEM table identifier (padded with spaces)
    uint32_t oem_revision; // OEM revision number
    uint32_t creator_id;   // Vendor ID of utility that created the table
    uint32_t creator_revision; // Revision of utility that created the table
} __attribute__((packed));

// Root System Description Pointer (RSDP) structure for ACPI 2.0+
struct rsdp {
    char signature[8]; // "RSD PTR " (with trailing space)
    uint8_t checksum; // entire table must sum to zero
    char oem_id[6];   // OEM identifier string (padded with spaces)
    uint8_t revision; // ACPI version (0 for 1.0, 2 for 2.0+)
    uint32_t rsdt_address; // physical address of the RSDT (ACPI 1.0)

    // Fields below are only valid if revision >= 2
    uint32_t length;   // total length of the RSDP structure (36 bytes for ACPI 2.0+)
    uint64_t xsdt_address; // physical address of the XSDT (ACPI 2.0+)
    uint8_t extended_checksum; // entire table must sum to zero
    uint8_t reserved[3]; // reserved, must be zero
} __attribute__((packed));

// MADT entry header (common to all MADT entry types)
// Signature of MADT is "APIC". Follows the acpi_sdt_header.
struct madt {
    struct acpi_sdt_header header; // standard ACPI table header
    uint32_t local_apic_address;   // physical address of the local APIC
    uint32_t flags;                // bit 0 = PC-AT compatibility, other bits reserved
    // Followed by variable-length array of MADT entries (type, length, data...)
} __attribute__((packed));

// MADT entry header (common to all MADT entry types)
struct madt_entry_header {
    uint8_t type;   // entry type (0=processor, 1=ioapic, 2=interrupt source override, etc.)
    uint8_t length; // total length of this entry, including this header
    // Followed by entry-specific data depending on the type
} __attribute__((packed));


// MADT entry types

#define MADT_TYPE_LOCAL_APIC        0
#define MADT_TYPE_IO_APIC           1
#define MADT_TYPE_INT_OVERRIDE      2
#define MADT_TYPE_NMI_SOURCE        3
#define MADT_TYPE_LOCAL_APIC_NMI    4
#define MADT_TYPE_LOCAL_APIC_OVR    5
#define MADT_TYPE_LOCAL_X2APIC      9

// Type 0: Processor Local APIC
struct madt_local_apic {
    struct madt_entry_header header; // type=0, length=8
    uint8_t acpi_processor_id; // ACPI processor ID (matches _PR._PID)
    uint8_t apic_id;     // Local APIC ID (for programming the APIC)
    uint32_t flags;           // bit 0 = enabled, other bits reserved
} __attribute__((packed));


// Type 1: I/O APIC
struct madt_io_apic {
    struct madt_entry_header header; // type=1, length=12
    uint8_t io_apic_id;   // I/O APIC ID (for programming the APIC)
    uint8_t reserved;     // reserved, must be zero
    uint32_t io_apic_address; // physical address of the I/O APIC's registers
    uint32_t global_system_interrupt_base; // GSI base for this I/O APIC (maps GSI numbers to IRQs)
} __attribute__((packed));


// Type 2: Interrupt Source Override
struct madt_int_override {
    struct madt_entry_header header; // type=2, length=10
    uint8_t bus;   // bus type (0=ISA, 1=PCI, etc.)
    uint8_t source;   // IRQ number on the source bus
    uint32_t global_system_interrupt; // GSI number to route this IRQ to
    uint16_t flags;       // bit 0 = active high, bit 1 = active low, bit 2 = level-triggered, bit 3 = edge-triggered, other bits reserved
} __attribute__((packed));


/* ---- HPET ACPI description table (signature "HPET") ---- */
struct acpi_hpet {
    struct acpi_sdt_header header;      /* "HPET" */
    uint32_t event_timer_block_id;      /* bits[15:8]=vendor, [12]=64-bit, [7:0]=HW rev */
    /* Generic Address Structure for the base */
    uint8_t  gas_addr_space;            /* 0 = memory-mapped */
    uint8_t  gas_bit_width;
    uint8_t  gas_bit_offset;
    uint8_t  gas_access_size;
    uint64_t base_address;              /* physical base of the HPET block */
    uint8_t  hpet_number;
    uint16_t minimum_tick;              /* minimum IRQ period (clock ticks) */
    uint8_t  page_protection;
} __attribute__((packed));

// Max CPUs / IO-APICs we'll
#define ACPI_MAX_CPUS     256
#define ACPI_MAX_IO_APICS 8
#define ACPI_MAX_INT_OVERRIDES 24

// A parsed MADT Interrupt Source Override (type 2): the firmware saying "ISA IRQ
// `source` is actually wired to GSI `gsi` with these polarity/trigger flags".
// The canonical example is the PIT: ISA IRQ0 is delivered on GSI2, not GSI0.
struct acpi_int_override {
    uint8_t  source;   // ISA IRQ number (the legacy line a driver asks for)
    uint32_t gsi;      // the Global System Interrupt it is really wired to
    uint16_t flags;    // MPS INTI flags: bits[1:0]=polarity, bits[3:2]=trigger
};

// Parsed ACPI data, filled by acpi_init
struct acpi_info{
    bool found; // true if ACPI tables were found and parsed successfully
    uint64_t local_apic_address; // physical address of the local APIC (from MADT)

    uint32_t cpu_count; // number of CPUs found in MADT
    uint8_t cpu_apic_ids[ACPI_MAX_CPUS]; // APIC IDs

    uint32_t io_apic_count; // number of I/O APICs found in MADT
    uint32_t io_apic_addresses[ACPI_MAX_IO_APICS]; // physical addresses of I/O APICs
    uint32_t io_apic_gsi_bases[ACPI_MAX_IO_APICS]; // GSI base numbers for each I/O APIC

    uint32_t int_override_count; // number of interrupt source overrides found
    struct acpi_int_override int_overrides[ACPI_MAX_INT_OVERRIDES];

    bool     hpet_found;          /* true if HPET table was present and valid */
    uint64_t hpet_address;        /* physical base address of HPET MMIO block */
    uint16_t hpet_minimum_tick;   /* minimum IRQ period in clock ticks */
};


/* ---- Generic Address Structure: how ACPI names a register ---------------- */
#define ACPI_GAS_MEMORY   0
#define ACPI_GAS_IO       1
#define ACPI_GAS_PCI_CFG  2

struct acpi_gas {
    uint8_t  space_id;      /* ACPI_GAS_*                                         */
    uint8_t  bit_width;
    uint8_t  bit_offset;
    uint8_t  access_size;   /* 0 = undefined, 1 = byte, 2 = word, 3 = dword, 4 = qword */
    uint64_t address;
} __attribute__((packed));

/* ---- FADT (signature "FACP"): where the power hardware is ---------------
 * The full ACPI 6 layout, because the fields this kernel needs are spread
 * across all three generations of it: the ACPI 1.0 I/O-port blocks, the 2.0
 * reset register and 64-bit addresses, and the 5.0 hardware-reduced sleep
 * registers. A table is only as long as its header says; every field past an
 * old table's end is ABSENT, and acpi.c checks the length before reading one. */
struct acpi_fadt {
    struct acpi_sdt_header header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t  reserved0;
    uint8_t  preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t  acpi_enable;
    uint8_t  acpi_disable;
    uint8_t  s4bios_req;
    uint8_t  pstate_cnt;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;
    uint32_t pm2_cnt_blk;
    uint32_t pm_tmr_blk;
    uint32_t gpe0_blk;
    uint32_t gpe1_blk;
    uint8_t  pm1_evt_len;
    uint8_t  pm1_cnt_len;
    uint8_t  pm2_cnt_len;
    uint8_t  pm_tmr_len;
    uint8_t  gpe0_blk_len;
    uint8_t  gpe1_blk_len;
    uint8_t  gpe1_base;
    uint8_t  cst_cnt;
    uint16_t p_lvl2_lat;
    uint16_t p_lvl3_lat;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t  duty_offset;
    uint8_t  duty_width;
    uint8_t  day_alrm;
    uint8_t  mon_alrm;
    uint8_t  century;
    uint16_t iapc_boot_arch;
    uint8_t  reserved1;
    uint32_t flags;
    struct acpi_gas reset_reg;          /* ACPI 2.0 -- offset 116 */
    uint8_t  reset_value;
    uint16_t arm_boot_arch;
    uint8_t  fadt_minor_version;
    uint64_t x_firmware_ctrl;
    uint64_t x_dsdt;                    /* offset 140 */
    struct acpi_gas x_pm1a_evt_blk;
    struct acpi_gas x_pm1b_evt_blk;
    struct acpi_gas x_pm1a_cnt_blk;     /* offset 172 */
    struct acpi_gas x_pm1b_cnt_blk;
    struct acpi_gas x_pm2_cnt_blk;
    struct acpi_gas x_pm_tmr_blk;
    struct acpi_gas x_gpe0_blk;
    struct acpi_gas x_gpe1_blk;
    struct acpi_gas sleep_control_reg;  /* ACPI 5.0 -- offset 244 */
    struct acpi_gas sleep_status_reg;
    uint64_t hypervisor_vendor_id;
} __attribute__((packed));

#define ACPI_FADT_RESET_REG_SUP  (1u << 10)
#define ACPI_FADT_HW_REDUCED     (1u << 20)

/* What power-off and reboot need, read out of the FADT and the AML. Filled by
 * acpi_init(); every field is honest about absence (fadt_found, s5_found,
 * reset_supported), so the power code can say WHICH part of ACPI was missing
 * rather than "power-off failed". */
struct acpi_power_info {
    bool     fadt_found;
    uint8_t  fadt_revision;
    bool     hw_reduced;            /* ACPI 5 hardware-reduced: no PM1 blocks  */
    uint32_t smi_cmd;               /* port that switches the chipset to ACPI  */
    uint8_t  acpi_enable;           /* value to write there                    */
    struct acpi_gas pm1a_cnt;       /* address 0 = absent                      */
    struct acpi_gas pm1b_cnt;
    struct acpi_gas sleep_control;  /* hardware-reduced platforms only         */
    bool     reset_supported;
    struct acpi_gas reset_reg;
    uint8_t  reset_value;
    bool     s5_found;              /* \_S5_ decoded from the AML              */
    uint8_t  s5_typa, s5_typb;      /* SLP_TYP for PM1a / PM1b                 */
    char     s5_table[5];           /* which table it came from: DSDT or SSDT  */
};

/* The power part of the ACPI tables, or NULL before acpi_init(). */
const struct acpi_power_info *acpi_power_info(void);

// Discover and parse ACPI tables, fill the acpi_info struct with relevant data for APIC initialization and interrupt routing
const struct acpi_info *acpi_init(void);

// Get the parsed info after acpi_init. Returns NULL if ACPI tables were not found or failed to parse.
const struct acpi_info *acpi_get_info(void);

// Resolve an ISA IRQ (0-15) to the GSI it is really delivered on plus its
// electrical characteristics, applying any MADT interrupt source override.
// With no override the ISA defaults apply: identity GSI, edge-triggered,
// active-high. Any of the out-params may be NULL if the caller doesn't need it.
void acpi_resolve_isa_irq(uint8_t isa_irq, uint32_t *out_gsi,
                          bool *out_active_low, bool *out_level);



#endif /* _ACPI_H_ */