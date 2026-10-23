/* kernel/acpi/acpi_dev.c -- what the interpreter is FOR.
 *
 * aml.c can run the firmware's bytecode. This file is the set of questions
 * worth asking it, and each one has the same shape: ACPI answers if it can,
 * and the caller's existing non-ACPI behaviour stands if it cannot. Nothing
 * here is allowed to make a machine worse than it was before the interpreter
 * existed.
 *
 * WHICH INTERRUPT DOES A PCI CARD ACTUALLY USE is the question that matters
 * most, and it is worth being clear about why it cannot be answered any other
 * way. A PCI device asserts one of four pins, A to D. Where that pin lands on
 * the interrupt controller is a property of the BOARD -- its traces -- and
 * appears nowhere in the device, nowhere in the PCI configuration header
 * (the Interrupt Line register is a scratch byte the firmware writes and
 * nothing enforces), and nowhere in any fixed ACPI table. It is in `_PRT`, a
 * method, and on this very machine QEMU's `_PRT` is 1853 bytes of code that
 * returns a DIFFERENT table depending on whether the OS said it was using the
 * PIC or the APIC. There is nothing to pattern-match.
 */
#include <stdint.h>
#include <stddef.h>

#include "include/types.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/kmalloc.h"
#include "acpi/acpi.h"
#include "acpi/aml.h"

/* ---- telling the firmware which interrupt controller we chose -----------
 *
 * `_PIC` is how the OS declares it: 0 means the 8259 pair, 1 means the I/O
 * APIC. Firmware branches on it -- QEMU's `_PRT` literally returns a
 * different package -- so calling it is not a courtesy, it is what makes the
 * routing that follows correct. A machine with no `_PIC` is fine; it means
 * its routing does not depend on the answer. */
#define ACPI_PIC_APIC 1

bool acpi_pic_method_exists(void) {
    return aml_available() && aml_lookup(NULL, "\\_PIC") != NULL;
}

static void announce_mode(int mode) {
    struct aml_node *pic = aml_lookup(NULL, "\\_PIC");
    if (!pic) return;
    struct aml_object *arg = aml_integer((uint64_t)mode);
    struct aml_object *r = aml_evaluate(pic, &arg, 1);
    aml_put(r);
    aml_put(arg);
    kprintf("ACPI: told the firmware we use the %s (_PIC %d)\n",
            mode ? "I/O APIC" : "8259 PIC", mode);
}

/* ---- the routing table -------------------------------------------------- */
struct prt_entry {
    uint8_t  bus;
    uint8_t  device;
    uint8_t  pin;          /* 0 = INTA, 1 = INTB, 2 = INTC, 3 = INTD */
    uint32_t gsi;
    bool     active_low;
    bool     level;
};

#define PRT_MAX 256
static struct prt_entry g_prt[PRT_MAX];
static uint32_t         g_prt_count;
static bool             g_prt_ready;

/* An IRQ out of a resource template.
 *
 * `_CRS` returns a BUFFER of resource descriptors, not a number: a chain of
 * tag/length/payload records ending in an End Tag. Two of them carry an
 * interrupt -- the small IRQ descriptor (0x22/0x23), whose payload is a
 * 16-bit MASK of the ISA lines it could be on, and the large Extended
 * Interrupt descriptor (0x89), which carries a real 32-bit number and the
 * flags. Everything else is skipped by its length. */
static bool crs_first_irq(struct aml_object *buf, uint32_t *gsi,
                          bool *active_low, bool *level) {
    if (!buf || buf->kind != AML_BUFFER) return false;
    const uint8_t *p = buf->u.buf.data;
    uint32_t n = buf->u.buf.len;
    uint32_t i = 0;

    while (i < n) {
        uint8_t tag = p[i];
        if (tag & 0x80) {                      /* large descriptor */
            if (i + 3 > n) return false;
            uint16_t len = (uint16_t)(p[i + 1] | (p[i + 2] << 8));
            if (tag == 0x89 && i + 3 + 6 <= n && len >= 6) {
                const uint8_t *d = p + i + 3;
                uint8_t flags = d[0];
                /* d[1] is the COUNT of interrupts; the first is the one a
                 * link device is currently set to. */
                if (d[1] >= 1 && i + 3 + 2 + 4 <= n) {
                    *gsi = (uint32_t)d[2] | ((uint32_t)d[3] << 8) |
                           ((uint32_t)d[4] << 16) | ((uint32_t)d[5] << 24);
                    *level      = (flags & 0x02) == 0;   /* bit1: 1 = edge  */
                    *active_low = (flags & 0x04) != 0;
                    return true;
                }
            }
            i += 3 + len;
            continue;
        }
        /* small descriptor: length is in the low three bits of the tag */
        uint8_t len = tag & 0x07;
        uint8_t name = (tag >> 3) & 0x0F;
        if (name == 0x0F) return false;        /* End Tag */
        if (name == 0x04 && i + 1 + 2 <= n) {  /* IRQ descriptor */
            uint16_t mask = (uint16_t)(p[i + 1] | (p[i + 2] << 8));
            for (int b = 0; b < 16; b++) {
                if (!(mask & (1u << b))) continue;
                *gsi = (uint32_t)b;
                /* A three-byte IRQ descriptor carries flags; a two-byte one
                 * means the ISA defaults, which are edge and active high. */
                if (len >= 3 && i + 3 < n) {
                    uint8_t f = p[i + 3];
                    *level      = (f & 0x01) == 0;
                    *active_low = (f & 0x08) != 0;
                } else {
                    *level = false; *active_low = false;
                }
                return true;
            }
            return false;
        }
        i += 1 + len;
    }
    return false;
}

/* One _PRT package: { Address, Pin, Source, SourceIndex }.
 *
 * Source is the part that decides everything. An INTEGER zero means "this is
 * a hard-wired global interrupt and SourceIndex IS it" -- what an APIC machine
 * returns. A NAME means the pin goes through a PCI link device, which is a
 * piece of programmable routing logic, and the actual line is whatever that
 * device's `_CRS` currently reports. */
/* WHY AN ENTRY WAS DROPPED. A routing table that comes out empty is the
 * failure this has to explain, because "0 entries" on a machine nobody can
 * attach a debugger to says nothing about which of the five steps went
 * wrong. Counted here and reported once. */
static struct {
    uint32_t not_package, bad_fields, no_link, link_no_crs, crs_unparsed;
    uint8_t  first_src_kind;
} g_drop;

static void prt_add(struct aml_object *e, uint8_t bus) {
    if (!e || e->kind != AML_PACKAGE || e->u.pkg.count < 4) {
        g_drop.not_package++;
        return;
    }
    if (g_prt_count >= PRT_MAX) return;

    struct aml_object *a_adr = e->u.pkg.elem[0];
    struct aml_object *a_pin = e->u.pkg.elem[1];
    struct aml_object *a_src = e->u.pkg.elem[2];
    struct aml_object *a_idx = e->u.pkg.elem[3];
    if (!a_adr || a_adr->kind != AML_INTEGER ||
        !a_pin || a_pin->kind != AML_INTEGER) { g_drop.bad_fields++; return; }
    if (!g_drop.first_src_kind && a_src) g_drop.first_src_kind = (uint8_t)a_src->kind;

    struct prt_entry pe;
    pe.bus    = bus;
    pe.device = (uint8_t)((a_adr->u.integer >> 16) & 0xFF);
    pe.pin    = (uint8_t)a_pin->u.integer;
    pe.gsi    = 0;
    /* PCI interrupts are level triggered and active low. That is the bus's
     * rule, not a guess, and it is the right default when a link device does
     * not say otherwise. */
    pe.active_low = true;
    pe.level      = true;

    bool have = false;
    if (a_src && a_src->kind == AML_INTEGER && a_src->u.integer == 0) {
        if (a_idx && a_idx->kind == AML_INTEGER) {
            pe.gsi = (uint32_t)a_idx->u.integer;
            have = true;
        }
    } else {
        /* A link device, by reference or by the name text the loader kept
         * when it could not resolve it yet. */
        /* Resolved reference, or a name that was not declared yet when the
         * package was parsed -- aml_name_node() handles both, and resolves
         * the second one from the scope it was written in. */
        struct aml_node *link = aml_name_node(a_src);
        if (!link && a_src && a_src->kind == AML_STRING)
            link = aml_lookup(NULL, (const char *)a_src->u.buf.data);
        if (!link) {
            g_drop.no_link++;
        } else {
            struct aml_node *crs = aml_lookup(link, "_CRS");
            if (!crs || crs->parent != link) {
                g_drop.link_no_crs++;
            } else {
                struct aml_object *b = aml_evaluate(crs, NULL, 0);
                have = crs_first_irq(b, &pe.gsi, &pe.active_low, &pe.level);
                if (!have) g_drop.crs_unparsed++;
                aml_put(b);
            }
        }
    }
    if (have) g_prt[g_prt_count++] = pe;
}

struct bridge_ctx { int bridges; };

static bool bridge_visit(struct aml_node *n, void *vctx) {
    struct bridge_ctx *bc = vctx;
    struct aml_node *prt = aml_lookup(n, "_PRT");
    if (!prt || prt->parent != n) return true;

    uint64_t bus = 0;
    if (!aml_eval_integer(n, "_BBN", &bus)) bus = 0;

    struct aml_object *t = aml_evaluate(prt, NULL, 0);
    if (t && t->kind == AML_PACKAGE) {
        for (uint32_t i = 0; i < t->u.pkg.count; i++)
            prt_add(t->u.pkg.elem[i], (uint8_t)bus);
        bc->bridges++;
    }
    aml_put(t);
    return true;
}

/* Build the routing table. After aml_init() and after the I/O APIC is the
 * chosen controller, because _PIC changes the answer. */
/* Announce a mode and rebuild the table from scratch.
 *
 * REBUILDING IS A REAL OPERATION, not a test fixture: `_PIC` changes firmware
 * state, and on a machine that has it the routing before and after are
 * different tables. That is also the only way to PROVE the call took --
 * switching modes and watching the answer change is evidence; calling a method
 * that returns nothing and hoping is not. */
void acpi_pci_routing_rebuild(int pic_mode) {
    if (!aml_available()) return;
    g_prt_count = 0;
    memset(&g_drop, 0, sizeof g_drop);
    announce_mode(pic_mode);

    struct bridge_ctx bc = { 0 };
    aml_walk(NULL, bridge_visit, &bc);
    g_prt_ready = true;

    if (!g_prt_count) {
        kprintf("ACPI: no _PRT routing found -- PCI interrupts keep their "
                "firmware-programmed lines\n");
        kprintf("ACPI:   dropped: %u not a package, %u bad fields, %u link not "
                "found, %u link has no _CRS, %u _CRS had no interrupt "
                "(first Source kind %u)\n",
                g_drop.not_package, g_drop.bad_fields, g_drop.no_link,
                g_drop.link_no_crs, g_drop.crs_unparsed, g_drop.first_src_kind);
        return;
    }
    kprintf("ACPI: PCI interrupt routing from %d bridge(s): %u entr%s\n",
            bc.bridges, g_prt_count, g_prt_count == 1 ? "y" : "ies");
}

void acpi_pci_routing_init(void) {
    if (g_prt_ready) return;
    acpi_pci_routing_rebuild(ACPI_PIC_APIC);
}

/* Where does (bus, device, pin) land? `pin` is 0..3 for INTA..INTD.
 *
 * False means ACPI did not describe this device, which is normal for a bus
 * behind a bridge whose _PRT this kernel has not walked -- the caller keeps
 * whatever the firmware left in the Interrupt Line register. */
bool acpi_pci_route(uint8_t bus, uint8_t device, uint8_t pin,
                    uint32_t *out_gsi, bool *out_active_low, bool *out_level) {
    if (!g_prt_ready) return false;
    for (uint32_t i = 0; i < g_prt_count; i++) {
        if (g_prt[i].bus != bus || g_prt[i].device != device) continue;
        if (g_prt[i].pin != pin) continue;
        if (out_gsi)        *out_gsi = g_prt[i].gsi;
        if (out_active_low) *out_active_low = g_prt[i].active_low;
        if (out_level)      *out_level = g_prt[i].level;
        return true;
    }
    return false;
}

uint32_t acpi_pci_route_count(void) { return g_prt_count; }

bool acpi_pci_route_at(uint32_t i, uint8_t *bus, uint8_t *device, uint8_t *pin,
                       uint32_t *gsi, bool *active_low, bool *level) {
    if (i >= g_prt_count) return false;
    if (bus)        *bus = g_prt[i].bus;
    if (device)     *device = g_prt[i].device;
    if (pin)        *pin = g_prt[i].pin;
    if (gsi)        *gsi = g_prt[i].gsi;
    if (active_low) *active_low = g_prt[i].active_low;
    if (level)      *level = g_prt[i].level;
    return true;
}

/* ---- switching the machine off ------------------------------------------
 *
 * acpi.c decodes `\_S5_` by recognising the byte pattern of a named package,
 * and documents that it cannot handle a machine where `_S5` is a METHOD. This
 * can: it evaluates the object, whatever it is. The pattern matcher stays as
 * the fallback for a machine whose DSDT this interpreter refuses. */
/* Let the power code use the EVALUATED \_S5_ instead of the pattern-matched
 * one. Called once, after aml_init().
 *
 * Two independent answers to the same question, which is worth more than
 * either: acpi.c recognises the byte encoding of a named package without
 * running anything, this runs the object. Where they DISAGREE the machine is
 * one whose _S5 the pattern matcher misread, and the interpreter is right --
 * it is the one that handles a _S5 that is a method, computed from a package
 * elsewhere, or wrapped in a conditional. Where the matcher found nothing at
 * all, this is the only answer there is. */
void acpi_power_adopt_aml_s5(void) {
    struct acpi_power_info *pw = acpi_power_info_mutable();
    if (!pw || !aml_available()) return;

    uint8_t a = 0, b = 0;
    if (!acpi_aml_sleep_typ(5, &a, &b)) {
        if (pw->s5_found)
            kprintf("ACPI: the interpreter could not evaluate \\_S5_; keeping "
                    "the decoded SLP_TYPa=%u\n", (unsigned)pw->s5_typa);
        return;
    }
    if (pw->s5_found && (pw->s5_typa != a || pw->s5_typb != b)) {
        kprintf("ACPI: \\_S5_ EVALUATES to SLP_TYPa=%u SLP_TYPb=%u, but the "
                "byte decoder read %u/%u -- using the evaluated pair\n",
                (unsigned)a, (unsigned)b,
                (unsigned)pw->s5_typa, (unsigned)pw->s5_typb);
    } else if (!pw->s5_found) {
        kprintf("ACPI: \\_S5_ evaluated to SLP_TYPa=%u SLP_TYPb=%u -- power-off "
                "is real after all\n", (unsigned)a, (unsigned)b);
    }
    pw->s5_typa = a;
    pw->s5_typb = b;
    pw->s5_found = true;
    memcpy(pw->s5_table, "AML_", 5);
}

bool acpi_aml_sleep_typ(int state, uint8_t *typa, uint8_t *typb) {
    if (!aml_available()) return false;
    char path[8] = { '\\', '_', 'S', (char)('0' + state), '_', 0, 0, 0 };
    struct aml_node *n = aml_lookup(NULL, path);
    if (!n) return false;

    struct aml_object *v = aml_evaluate(n, NULL, 0);
    bool ok = false;
    if (v && v->kind == AML_PACKAGE && v->u.pkg.count >= 1) {
        struct aml_object *a = v->u.pkg.elem[0];
        struct aml_object *b = v->u.pkg.count > 1 ? v->u.pkg.elem[1] : NULL;
        if (a && a->kind == AML_INTEGER) {
            *typa = (uint8_t)(a->u.integer & 0x7);
            *typb = (b && b->kind == AML_INTEGER) ? (uint8_t)(b->u.integer & 0x7)
                                                  : *typa;
            ok = true;
        }
    }
    aml_put(v);
    return ok;
}

/* ---- devices a laptop has and this machine does not ---------------------
 *
 * Everything below is written against the ACPI specification and has never
 * run against the hardware it describes: QEMU emulates no battery, no lid and
 * no thermal zone, so there is nothing here to execute them on. They are here
 * because the interpreter makes them a dozen lines each and because the first
 * real machine should find them already written rather than absent -- but
 * "written" is not "working", and docs/TODO.md says so rather than this file
 * quietly implying otherwise. */

static bool first_node(struct aml_node *n, void *ctx) {
    *(struct aml_node **)ctx = n;
    return false;                       /* stop at the first */
}

bool acpi_battery_present(void) {
    struct aml_node *n = NULL;
    aml_for_each_hid("PNP0C0A", first_node, &n);
    return n != NULL;
}

/* Charge now and full charge, in the units the battery reports (mWh or mAh --
 * `_BIF`'s first element says which). False if there is no battery, or its
 * methods did not evaluate. */
bool acpi_battery_state(uint64_t *remaining, uint64_t *full, uint64_t *state) {
    struct aml_node *bat = NULL;
    aml_for_each_hid("PNP0C0A", first_node, &bat);
    if (!bat) return false;

    struct aml_node *bst = aml_lookup(bat, "_BST");
    struct aml_node *bif = aml_lookup(bat, "_BIF");
    if (!bst || bst->parent != bat) return false;

    struct aml_object *s = aml_evaluate(bst, NULL, 0);
    bool ok = false;
    if (s && s->kind == AML_PACKAGE && s->u.pkg.count >= 4) {
        struct aml_object *st = s->u.pkg.elem[0];
        struct aml_object *rc = s->u.pkg.elem[2];
        if (state && st && st->kind == AML_INTEGER) *state = st->u.integer;
        if (remaining && rc && rc->kind == AML_INTEGER) *remaining = rc->u.integer;
        ok = true;
    }
    aml_put(s);

    if (ok && full && bif && bif->parent == bat) {
        struct aml_object *i = aml_evaluate(bif, NULL, 0);
        if (i && i->kind == AML_PACKAGE && i->u.pkg.count >= 3) {
            struct aml_object *fc = i->u.pkg.elem[2];
            if (fc && fc->kind == AML_INTEGER) *full = fc->u.integer;
        }
        aml_put(i);
    }
    return ok;
}

/* Is the lid open? `_LID` returns non-zero for open. */
bool acpi_lid_open(bool *open) {
    struct aml_node *lid = NULL;
    aml_for_each_hid("PNP0C0D", first_node, &lid);
    if (!lid) return false;
    uint64_t v = 0;
    if (!aml_eval_integer(lid, "_LID", &v)) return false;
    *open = v != 0;
    return true;
}

/* The first thermal zone's temperature, in tenths of a kelvin -- which is what
 * `_TMP` returns and what every consumer has to convert. */
bool acpi_thermal_temp(uint64_t *decikelvin) {
    struct aml_node *zone = NULL;
    struct aml_node *tz = aml_lookup(NULL, "\\_TZ_");
    if (tz) {
        for (struct aml_node *n = tz->child; n; n = n->sibling) {
            if (n->value && n->value->kind == AML_THERMAL_ZONE) { zone = n; break; }
        }
    }
    if (!zone) return false;
    return aml_eval_integer(zone, "_TMP", decikelvin);
}
