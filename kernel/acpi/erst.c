/* kernel/acpi/erst.c -- error records that survive the reboot.
 *
 * docs/PILLARS.md phase 3 wants persistent crash reports: a machine that
 * panics should be able to say why after it comes back. Writing that to the
 * disk is the obvious way and the wrong one -- the disk driver may be what
 * panicked, the filesystem may be mid-transaction, and a kernel that is
 * already broken should not be running its most complicated code.
 *
 * ERST is the firmware's answer. The platform provides somewhere durable and
 * a way to write a record into it that does not involve any of the operating
 * system's own storage stack.
 *
 * AND THE WAY IT PROVIDES IT IS UNUSUAL ENOUGH TO BE THE POINT OF THIS FILE:
 * the ACPI table does not describe a device. It contains a PROGRAM. Each
 * "action" -- begin a write, set the record offset, execute, check whether it
 * is busy -- is a list of instructions, and each instruction reads or writes
 * one register with a mask and a shift. The operating system is the
 * interpreter. Two machines can implement ERST with completely different
 * hardware and this code does not change, because the difference is in the
 * table.
 *
 * WHICH MEANS THE INSTRUCTIONS MUST BE READ AS WRITTEN AND NOT SUMMARISED. A
 * mask that is applied before the shift instead of after, or a register width
 * taken from the address instead of the descriptor, produces an interpreter
 * that works on the machine it was written on.
 */
#include <stdint.h>
#include <stddef.h>

#include "include/types.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/errno.h"
#include "include/io.h"
#include "acpi/acpi.h"
#include "acpi/erst.h"
#include "mm/vmm.h"
#include "mm/pmm.h"

/* Serialization actions. */
#define ERST_BEGIN_WRITE        0x00
#define ERST_BEGIN_READ         0x01
#define ERST_BEGIN_CLEAR        0x02
#define ERST_END                0x03
#define ERST_SET_RECORD_OFFSET  0x04
#define ERST_EXECUTE_OPERATION  0x05
#define ERST_CHECK_BUSY_STATUS  0x06
#define ERST_GET_COMMAND_STATUS 0x07
#define ERST_GET_RECORD_ID      0x08
#define ERST_SET_RECORD_ID      0x09
#define ERST_GET_RECORD_COUNT   0x0A
#define ERST_BEGIN_DUMMY_WRITE  0x0B
#define ERST_GET_ERROR_RANGE    0x0D
#define ERST_GET_ERROR_LENGTH   0x0E
#define ERST_GET_ERROR_ATTRS    0x0F

/* Instructions. */
#define INS_READ_REGISTER        0x00
#define INS_READ_REGISTER_VALUE  0x01
#define INS_WRITE_REGISTER       0x02
#define INS_WRITE_REGISTER_VALUE 0x03
#define INS_NOOP                 0x04
#define INS_LOAD_VAR1            0x05
#define INS_LOAD_VAR2            0x06
#define INS_STORE_VAR1           0x07
#define INS_ADD                  0x08
#define INS_SUBTRACT             0x09
#define INS_ADD_VALUE            0x0A
#define INS_SUBTRACT_VALUE       0x0B
#define INS_STALL                0x0C
#define INS_STALL_WHILE_TRUE     0x0D
#define INS_SKIP_NEXT_IF_TRUE    0x0E
#define INS_GOTO                 0x0F
#define INS_SET_SRC_ADDRESS_BASE 0x10
#define INS_SET_DST_ADDRESS_BASE 0x11
#define INS_MOVE_DATA            0x12

#define ERST_FLAG_PRESERVE 0x01

/* The ACPI generic address: what register, how wide, and in which space. */
struct gas {
    uint8_t  space_id;      /* 0 system memory, 1 system I/O */
    uint8_t  bit_width;
    uint8_t  bit_offset;
    uint8_t  access_size;
    uint64_t address;
} __attribute__((packed));

struct erst_entry {
    uint8_t action;
    uint8_t instruction;
    uint8_t flags;
    uint8_t reserved;
    struct gas reg;
    uint64_t value;
    uint64_t mask;
} __attribute__((packed));

#define ERST_MAX_ENTRIES 64

static struct erst_entry g_entries[ERST_MAX_ENTRIES];
static uint32_t g_nentries;
static bool g_up;
static uint64_t g_range_phys, g_range_len;
static volatile uint8_t *g_range;
static uint64_t g_written;

/* The record identifier the firmware uses to mean "there are no more". */
#define ERST_ID_INVALID 0xFFFFFFFFFFFFFFFFULL

/* ---- the registers, mapped ONCE ------------------------------------------
 *
 * These were mapped and unmapped around every single access, which is
 * wasteful in normal use and DANGEROUS in the one that matters: the panic
 * path writes a crash record through this interpreter, and mapping a page
 * means allocating virtual address space and editing page tables -- on a
 * machine that has already gone wrong, possibly while another core holds the
 * lock that protects them.
 *
 * So every distinct page any instruction names is mapped at init and kept.
 * After that a register access is a load or a store and nothing else. */
#define ERST_MAX_PAGES 8
static struct { uint64_t phys; volatile uint8_t *virt; } g_pages[ERST_MAX_PAGES];
static uint32_t g_npages;

static volatile uint8_t *erst_map_page(uint64_t phys) {
    uint64_t page = phys & ~0xFFFULL;
    for (uint32_t i = 0; i < g_npages; i++)
        if (g_pages[i].phys == page) return g_pages[i].virt;
    if (g_npages >= ERST_MAX_PAGES) return NULL;
    volatile uint8_t *v = (volatile uint8_t *)(uintptr_t)vmm_map_mmio(page, 0x1000);
    if (!v) return NULL;
    g_pages[g_npages].phys = page;
    g_pages[g_npages].virt = v;
    g_npages++;
    return v;
}

static volatile uint8_t *erst_reg_ptr(const struct gas *g) {
    for (uint32_t i = 0; i < g_npages; i++)
        if (g_pages[i].phys == (g->address & ~0xFFFULL))
            return g_pages[i].virt + (g->address & 0xFFF);
    return NULL;
}

static uint64_t reg_read(const struct gas *g) {
    uint32_t width = g->bit_width ? g->bit_width : 32;
    if (g->space_id == 1) {                 /* system I/O */
        uint16_t port = (uint16_t)g->address;
        switch (width) {
        case 8:  return inb(port);
        case 16: return inw(port);
        default: return inl(port);
        }
    }
    volatile uint8_t *q = erst_reg_ptr(g);
    if (!q) return 0;
    switch (width) {
    case 8:  return *(volatile uint8_t  *)q;
    case 16: return *(volatile uint16_t *)q;
    case 64: return *(volatile uint64_t *)q;
    default: return *(volatile uint32_t *)q;
    }
}

static void reg_write(const struct gas *g, uint64_t v) {
    uint32_t width = g->bit_width ? g->bit_width : 32;
    if (g->space_id == 1) {
        uint16_t port = (uint16_t)g->address;
        switch (width) {
        case 8:  outb(port, (uint8_t)v); return;
        case 16: outw(port, (uint16_t)v); return;
        default: outl(port, (uint32_t)v); return;
        }
    }
    volatile uint8_t *q = erst_reg_ptr(g);
    if (!q) return;
    switch (width) {
    case 8:  *(volatile uint8_t  *)q = (uint8_t)v; break;
    case 16: *(volatile uint16_t *)q = (uint16_t)v; break;
    case 64: *(volatile uint64_t *)q = v; break;
    default: *(volatile uint32_t *)q = (uint32_t)v; break;
    }
}

/* The interpreter's state for one action. */
struct erst_ctx {
    uint64_t value;    /* the action's input on the way in, its output on the way out */
    uint64_t var1, var2;
    uint64_t src_base, dst_base;
};

/* Run every instruction belonging to `action`, in table order. Returns false
 * only if an instruction this interpreter does not implement is reached --
 * silently skipping one would produce an answer that looks right. */
static bool erst_run(struct erst_ctx *c, uint8_t action, bool optional) {
    bool found = false;
    for (uint32_t i = 0; i < g_nentries; i++) {
        struct erst_entry *e = &g_entries[i];
        if (e->action != action) continue;
        found = true;

        switch (e->instruction) {
        case INS_READ_REGISTER:
            /* SHIFT THEN MASK, in that order. The bit offset says where the
             * field starts inside the register and the mask says how wide it
             * is; masking first would keep the wrong bits. */
            c->value = (reg_read(&e->reg) >> e->reg.bit_offset) & e->mask;
            break;
        case INS_READ_REGISTER_VALUE:
            c->value = (((reg_read(&e->reg) >> e->reg.bit_offset) & e->mask)
                        == e->value) ? 1 : 0;
            break;
        case INS_WRITE_REGISTER:
        case INS_WRITE_REGISTER_VALUE: {
            uint64_t v = (e->instruction == INS_WRITE_REGISTER_VALUE)
                       ? e->value : c->value;
            v = (v & e->mask) << e->reg.bit_offset;
            if (e->flags & ERST_FLAG_PRESERVE) {
                /* THE OTHER BITS OF THE REGISTER BELONG TO SOMEBODY ELSE.
                 * Without this the write clears every field sharing the
                 * register, which on a status register is most of it. */
                uint64_t old = reg_read(&e->reg);
                old &= ~(e->mask << e->reg.bit_offset);
                v |= old;
            }
            reg_write(&e->reg, v);
            break;
        }
        case INS_NOOP: break;
        case INS_LOAD_VAR1: c->var1 = (reg_read(&e->reg) >> e->reg.bit_offset) & e->mask; break;
        case INS_LOAD_VAR2: c->var2 = (reg_read(&e->reg) >> e->reg.bit_offset) & e->mask; break;
        case INS_STORE_VAR1: reg_write(&e->reg, (c->var1 & e->mask) << e->reg.bit_offset); break;
        case INS_ADD:       c->var1 += c->var2; break;
        case INS_SUBTRACT:  c->var1 -= c->var2; break;
        case INS_ADD_VALUE: c->value += e->value; break;
        case INS_SUBTRACT_VALUE: c->value -= e->value; break;
        case INS_STALL:
            for (volatile uint64_t n = 0; n < e->value * 100; n++) { }
            break;
        case INS_SET_SRC_ADDRESS_BASE: c->src_base = e->value; break;
        case INS_SET_DST_ADDRESS_BASE: c->dst_base = e->value; break;
        default:
            kprintf("erst: instruction %u in action %u is not implemented\n",
                    e->instruction, action);
            return false;
        }
    }
    return found || optional;
}

static uint64_t erst_action(uint8_t action, uint64_t input, bool *ok) {
    struct erst_ctx c;
    memset(&c, 0, sizeof c);
    c.value = input;
    bool r = erst_run(&c, action, false);
    if (ok) *ok = r;
    return c.value;
}

/* The firmware is doing something; wait until it is not. Every ERST operation
 * is asynchronous and the busy check is how it ends. */
static bool erst_wait(void) {
    for (int i = 0; i < 2000000; i++) {
        bool ok = false;
        uint64_t busy = erst_action(ERST_CHECK_BUSY_STATUS, 0, &ok);
        if (!ok) return false;
        if (!busy) return true;
        __asm__ volatile("" ::: "memory");
    }
    return false;
}

/* One complete operation: begin, point at the record, name it, execute, wait,
 * ask how it went, end. The shape is the same for write, read and clear --
 * only the action that begins it differs.
 *
 * THE ORDER IS THE SPECIFICATION'S AND IT IS NOT A STYLE. Setting the record
 * identifier BEFORE the operation begins was this driver's first attempt, and
 * it appeared to work: the write went in, the read came back. What it
 * actually did was set an identifier that beginning the operation then
 * discarded, so every read ran against whatever the platform had left there.
 * The identifier belongs between the offset and the execute, which is where
 * the specification puts it and where every other implementation puts it. */
static int erst_operation(uint8_t begin, uint64_t record_id, uint64_t offset) {
    bool ok = false;
    erst_action(begin, 0, &ok);          /* optional on some platforms */
    erst_action(ERST_SET_RECORD_OFFSET, offset, &ok);
    if (!ok) return -EMBK_EIO;
    erst_action(ERST_SET_RECORD_ID, record_id, &ok);
    if (!ok) return -EMBK_EIO;
    erst_action(ERST_EXECUTE_OPERATION, 0, &ok);
    if (!ok) return -EMBK_EIO;
    if (!erst_wait()) return -EMBK_EIO;
    uint64_t status = erst_action(ERST_GET_COMMAND_STATUS, 0, &ok);
    erst_action(ERST_END, 0, NULL);
    if (!ok) return -EMBK_EIO;
    return status == 0 ? EMBK_OK : -EMBK_EIO;
}

uint64_t erst_record_count(void) {
    if (!g_up) return 0;
    bool ok = false;
    uint64_t n = erst_action(ERST_GET_RECORD_COUNT, 0, &ok);
    return ok ? n : 0;
}

int erst_write(uint64_t record_id, const void *data, uint32_t len) {
    if (!g_up || !g_range) return -EMBK_ENODEV;
    if (!len || len > g_range_len) return -EMBK_EINVAL;

    /* THE RECORD GOES IN THE PLATFORM'S BUFFER FIRST. The operation does not
     * take a pointer: it serialises whatever is sitting in the error log
     * address range the table named. */
    for (uint32_t i = 0; i < len; i++) g_range[i] = ((const uint8_t *)data)[i];
    int rc = erst_operation(ERST_BEGIN_WRITE, record_id, 0);
    if (rc == EMBK_OK) g_written++;
    return rc;
}

int erst_read(uint64_t record_id, void *out, uint32_t cap) {
    if (!g_up || !g_range) return -EMBK_ENODEV;
    int rc = erst_operation(ERST_BEGIN_READ, record_id, 0);
    if (rc != EMBK_OK) return rc;
    uint32_t n = cap < g_range_len ? cap : (uint32_t)g_range_len;
    for (uint32_t i = 0; i < n; i++) ((uint8_t *)out)[i] = g_range[i];
    return (int)n;
}

/* ---- walking the records ------------------------------------------------
 *
 * The platform hands them out one at a time, and the identifier it is
 * currently pointing at is what decides which comes next. Setting that to the
 * invalid identifier means "start again from the beginning"; the enumeration
 * ends by handing back the invalid identifier, which conveniently leaves the
 * platform ready to start again for whoever asks next. */
uint64_t erst_first_record(void) {
    if (!g_up) return ERST_ID_INVALID;
    bool ok = false;
    erst_action(ERST_SET_RECORD_ID, ERST_ID_INVALID, &ok);
    return erst_action(ERST_GET_RECORD_ID, 0, &ok);
}

uint64_t erst_next_record(void) {
    if (!g_up) return ERST_ID_INVALID;
    bool ok = false;
    return erst_action(ERST_GET_RECORD_ID, 0, &ok);
}

int erst_clear(uint64_t record_id) {
    if (!g_up) return -EMBK_ENODEV;
    return erst_operation(ERST_BEGIN_CLEAR, record_id, 0);
}

bool erst_init(void) {
    if (g_up) return true;
    const struct acpi_sdt_header *t = acpi_find_table("ERST");
    if (!t) return false;

    /* The header is 48 bytes: the standard ACPI one, then a serialization
     * header size, a reserved word, and the instruction count. */
    const uint8_t *b = (const uint8_t *)t;
    if (t->length < 48) return false;
    uint32_t count = (uint32_t)b[44] | ((uint32_t)b[45] << 8) |
                     ((uint32_t)b[46] << 16) | ((uint32_t)b[47] << 24);
    const struct erst_entry *ents = (const struct erst_entry *)(b + 48);
    uint32_t have = (t->length - 48) / (uint32_t)sizeof(struct erst_entry);
    if (count > have) count = have;
    if (count > ERST_MAX_ENTRIES) count = ERST_MAX_ENTRIES;
    if (!count) return false;

    for (uint32_t i = 0; i < count; i++) g_entries[i] = ents[i];
    g_nentries = count;

    /* MAP EVERY REGISTER PAGE NOW, while there is a working machine to do it
     * on. After this the interpreter never touches the memory manager, which
     * is what lets the panic path use it. */
    for (uint32_t i = 0; i < g_nentries; i++) {
        if (g_entries[i].reg.space_id != 1 && g_entries[i].reg.address) {
            if (!erst_map_page(g_entries[i].reg.address)) {
                kprintf("erst: could not map the register page at %llx\n",
                        (unsigned long long)g_entries[i].reg.address);
                return false;
            }
        }
    }
    g_up = true;

    bool ok = false;
    g_range_phys = erst_action(ERST_GET_ERROR_RANGE, 0, &ok);
    if (!ok) { g_up = false; return false; }
    g_range_len = erst_action(ERST_GET_ERROR_LENGTH, 0, &ok);
    if (!ok || !g_range_len) { g_up = false; return false; }
    if (g_range_len > 0x10000) g_range_len = 0x10000;

    g_range = (volatile uint8_t *)(uintptr_t)vmm_map_mmio(g_range_phys, g_range_len);
    if (!g_range) {
        kprintf("erst: could not map the error log range at %llx\n",
                (unsigned long long)g_range_phys);
        g_up = false;
        return false;
    }

    kprintf("erst: %u instruction(s), %llu-byte record buffer at %llx, "
            "%llu record(s) already stored\n", g_nentries,
            (unsigned long long)g_range_len, (unsigned long long)g_range_phys,
            (unsigned long long)erst_record_count());
    return true;
}

bool     erst_present(void) { return g_up; }
uint64_t erst_buffer_size(void) { return g_range_len; }
uint64_t erst_writes(void) { return g_written; }
