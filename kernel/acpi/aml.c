/* kernel/acpi/aml.c -- the ACPI bytecode interpreter.
 *
 * See aml.h for why this has to exist at all. This file is the machine: a
 * namespace built from the firmware's tables, and an evaluator for the
 * bytecode those tables carry.
 *
 * TWO PASSES, and the split is the whole structure.
 *
 *   THE LOAD PASS walks a table and CREATES NAMES. Scope, Device, Name,
 *   Method, OperationRegion, Field and friends are declarations; each makes a
 *   namespace node. A Method's body is NOT parsed here -- it is remembered as
 *   a span of bytes and parsed only if someone calls it. That is not laziness:
 *   a DSDT contains methods for hardware that is not present, and parsing them
 *   eagerly means failing on code that would never have run.
 *
 *   THE EXECUTOR runs a method body when it is called. It is a recursive
 *   descent over the same bytecode with locals, arguments, control flow and
 *   the operation regions that reach real hardware.
 *
 * WHAT A NAME MEANS, because it is the part that reads oddly: every ACPI name
 * is EXACTLY four characters, padded with '_'. `_SB` is really `_SB_`. A path
 * is those segments joined, `\` means the root and `^` means the parent, and a
 * bare four-character name is searched from the current scope UPWARD to the
 * root -- so the same text resolves differently depending on where it appears.
 *
 * ON FAILURE: this refuses rather than guesses. An opcode that is not
 * understood ends the method with no value, and the caller -- every one of
 * which already has a non-ACPI fallback, because it had to work before this
 * file existed -- keeps what it had. A wrong answer from ACPI is a machine
 * that powers off at the wrong moment or routes an interrupt into nothing.
 */
#include <stdint.h>
#include <stddef.h>

#include "include/types.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/kmalloc.h"
#include "include/io.h"
#include "acpi/acpi.h"
#include "acpi/aml.h"
#include "drivers/bus/pci.h"
#include "mm/vmm.h"

/* ---- opcodes ------------------------------------------------------------ */
#define OP_ZERO        0x00
#define OP_ONE         0x01
#define OP_ALIAS       0x06
#define OP_NAME        0x08
#define OP_BYTE        0x0A
#define OP_WORD        0x0B
#define OP_DWORD       0x0C
#define OP_STRING      0x0D
#define OP_QWORD       0x0E
#define OP_SCOPE       0x10
#define OP_BUFFER      0x11
#define OP_PACKAGE     0x12
#define OP_VAR_PACKAGE 0x13
#define OP_METHOD      0x14
#define OP_EXTERNAL    0x15
#define OP_EXT         0x5B
#define OP_ROOT_CHAR   0x5C
#define OP_PARENT_CHAR 0x5E
#define OP_DUAL_NAME   0x2E
#define OP_MULTI_NAME  0x2F
#define OP_LOCAL0      0x60
#define OP_ARG0        0x68
#define OP_STORE       0x70
#define OP_REF_OF      0x71
#define OP_ADD         0x72
#define OP_CONCAT      0x73
#define OP_SUBTRACT    0x74
#define OP_INCREMENT   0x75
#define OP_DECREMENT   0x76
#define OP_MULTIPLY    0x77
#define OP_DIVIDE      0x78
#define OP_SHIFT_LEFT  0x79
#define OP_SHIFT_RIGHT 0x7A
#define OP_AND         0x7B
#define OP_NAND        0x7C
#define OP_OR          0x7D
#define OP_NOR         0x7E
#define OP_XOR         0x7F
#define OP_NOT         0x80
#define OP_FIND_SET_L  0x81
#define OP_FIND_SET_R  0x82
#define OP_DEREF_OF    0x83
#define OP_CONCAT_RES  0x84
#define OP_MOD         0x85
#define OP_NOTIFY      0x86
#define OP_SIZE_OF     0x87
#define OP_INDEX       0x88
#define OP_MATCH       0x89
#define OP_CREATE_DW   0x8A
#define OP_CREATE_W    0x8B
#define OP_CREATE_B    0x8C
#define OP_CREATE_BIT  0x8D
#define OP_OBJECT_TYPE 0x8E
#define OP_CREATE_QW   0x8F
#define OP_LAND        0x90
#define OP_LOR         0x91
#define OP_LNOT        0x92
#define OP_LEQUAL      0x93
#define OP_LGREATER    0x94
#define OP_LLESS       0x95
#define OP_TO_BUFFER   0x96
#define OP_TO_DEC_STR  0x97
#define OP_TO_HEX_STR  0x98
#define OP_TO_INTEGER  0x99
#define OP_TO_STRING   0x9C
#define OP_COPY_OBJECT 0x9D
#define OP_MID         0x9E
#define OP_CONTINUE    0x9F
#define OP_IF          0xA0
#define OP_ELSE        0xA1
#define OP_WHILE       0xA2
#define OP_NOOP        0xA3
#define OP_RETURN      0xA4
#define OP_BREAK       0xA5
#define OP_BREAKPOINT  0xCC
#define OP_ONES        0xFF

/* extended, after 0x5B */
#define EXT_MUTEX       0x01
#define EXT_EVENT       0x02
#define EXT_COND_REF_OF 0x12
#define EXT_CREATE_FLD  0x13
#define EXT_LOAD_TABLE  0x1F
#define EXT_LOAD        0x20
#define EXT_STALL       0x21
#define EXT_SLEEP       0x22
#define EXT_ACQUIRE     0x23
#define EXT_SIGNAL      0x24
#define EXT_WAIT        0x25
#define EXT_RESET       0x26
#define EXT_RELEASE     0x27
#define EXT_FROM_BCD    0x28
#define EXT_TO_BCD      0x29
#define EXT_UNLOAD      0x2A
#define EXT_REVISION    0x30
#define EXT_DEBUG       0x31
#define EXT_FATAL       0x32
#define EXT_TIMER       0x33
#define EXT_OP_REGION   0x80
#define EXT_FIELD       0x81
#define EXT_DEVICE      0x82
#define EXT_PROCESSOR   0x83
#define EXT_POWER_RES   0x84
#define EXT_THERMAL_ZN  0x85
#define EXT_INDEX_FIELD 0x86
#define EXT_BANK_FIELD  0x87

/* ---- state -------------------------------------------------------------- */
static struct aml_node  g_root = { "\\", 0, 0, 0, 0 };
static struct aml_stats g_stats;
static bool             g_ready;

/* HOW DEEP A METHOD MAY GO. AML is firmware-supplied and a malformed table can
 * describe unbounded recursion; the kernel stack cannot. 24 is well past what
 * any real DSDT needs and far short of what overflows. */
#define AML_MAX_DEPTH 24

/* Bounded work per method call. A `While` whose predicate never falsifies is a
 * hang in the middle of boot with nothing on screen. */
#define AML_MAX_STEPS 2000000

struct aml_ctx {
    const uint8_t     *p, *end;
    struct aml_node   *scope;
    struct aml_object *local[8];
    struct aml_object *arg[7];
    struct aml_object *ret;
    int                flow;      /* FLOW_*                                 */
    int                depth;
    uint32_t          *steps;     /* shared with nested contexts            */
    bool               failed;
};

#define FLOW_NORMAL   0
#define FLOW_RETURN   1
#define FLOW_BREAK    2
#define FLOW_CONTINUE 3

static struct aml_object *term_arg(struct aml_ctx *c);
static bool termlist_exec(struct aml_ctx *c, const uint8_t *end);
static bool load_term(struct aml_ctx *c);
static bool load_termlist(const uint8_t *p, const uint8_t *end,
                          struct aml_node *scope);

/* ---- objects ------------------------------------------------------------ */
static struct aml_object *obj_new(enum aml_kind k) {
    struct aml_object *o = kmalloc(sizeof *o);
    if (!o) return NULL;
    memset(o, 0, sizeof *o);
    o->kind = k;
    o->refs = 1;
    return o;
}

struct aml_object *aml_integer(uint64_t v) {
    struct aml_object *o = obj_new(AML_INTEGER);
    if (o) o->u.integer = v;
    return o;
}

static struct aml_object *obj_buffer(const uint8_t *src, uint32_t len) {
    struct aml_object *o = obj_new(AML_BUFFER);
    if (!o) return NULL;
    o->u.buf.data = len ? kmalloc(len) : NULL;
    if (len && !o->u.buf.data) { kfree(o); return NULL; }
    o->u.buf.len = len;
    if (src && len) memcpy(o->u.buf.data, src, len);
    else if (len)   memset(o->u.buf.data, 0, len);
    return o;
}

static struct aml_object *obj_string(const char *s, uint32_t len) {
    struct aml_object *o = obj_new(AML_STRING);
    if (!o) return NULL;
    o->u.buf.data = kmalloc(len + 1);
    if (!o->u.buf.data) { kfree(o); return NULL; }
    if (len) memcpy(o->u.buf.data, s, len);
    o->u.buf.data[len] = 0;
    o->u.buf.len = len;
    return o;
}

struct aml_object *aml_string(const char *s) {
    return obj_string(s, (uint32_t)strlen(s));
}

static struct aml_object *obj_package(uint32_t n) {
    struct aml_object *o = obj_new(AML_PACKAGE);
    if (!o) return NULL;
    o->u.pkg.elem = n ? kmalloc(n * sizeof(struct aml_object *)) : NULL;
    if (n && !o->u.pkg.elem) { kfree(o); return NULL; }
    for (uint32_t i = 0; i < n; i++) o->u.pkg.elem[i] = NULL;
    o->u.pkg.count = n;
    return o;
}

struct aml_object *aml_get(struct aml_object *o) {
    if (o) o->refs++;
    return o;
}

void aml_put(struct aml_object *o) {
    if (!o || --o->refs > 0) return;
    switch (o->kind) {
    case AML_STRING:
    case AML_BUFFER:
        kfree(o->u.buf.data);
        break;
    case AML_PACKAGE:
        for (uint32_t i = 0; i < o->u.pkg.count; i++) aml_put(o->u.pkg.elem[i]);
        kfree(o->u.pkg.elem);
        break;
    case AML_BUFFER_FIELD:
        aml_put(o->u.bfield.buffer);
        break;
    case AML_REF:
        aml_put(o->u.ref.obj);
        break;
    case AML_UNRESOLVED:
        kfree(o->u.uname.path);
        break;
    default:
        break;
    }
    kfree(o);
}

/* The integer value of an object, by ACPI's conversion rules as far as anyone
 * relies on them: a buffer's first 8 bytes little-endian, a string's leading
 * hex digits. Anything else is 0. */
static uint64_t obj_as_integer(struct aml_object *o) {
    if (!o) return 0;
    if (o->kind == AML_INTEGER) return o->u.integer;
    if (o->kind == AML_BUFFER) {
        uint64_t v = 0;
        for (uint32_t i = 0; i < o->u.buf.len && i < 8; i++)
            v |= (uint64_t)o->u.buf.data[i] << (i * 8);
        return v;
    }
    if (o->kind == AML_STRING) {
        uint64_t v = 0;
        for (uint32_t i = 0; i < o->u.buf.len; i++) {
            char ch = (char)o->u.buf.data[i];
            int d;
            if      (ch >= '0' && ch <= '9') d = ch - '0';
            else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
            else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
            else break;
            v = v * 16 + (uint64_t)d;
        }
        return v;
    }
    return 0;
}

/* ---- the namespace ------------------------------------------------------ */
static struct aml_node *node_child(struct aml_node *p, const char *seg) {
    for (struct aml_node *n = p->child; n; n = n->sibling)
        if (memcmp(n->name, seg, AML_NAME_LEN) == 0) return n;
    return NULL;
}

static struct aml_node *node_create(struct aml_node *p, const char *seg) {
    struct aml_node *n = node_child(p, seg);
    if (n) return n;
    n = kmalloc(sizeof *n);
    if (!n) return NULL;
    memset(n, 0, sizeof *n);
    memcpy(n->name, seg, AML_NAME_LEN);
    n->name[AML_NAME_LEN] = 0;
    n->parent = p;
    n->sibling = p->child;
    p->child = n;
    g_stats.nodes++;
    return n;
}

static void node_set(struct aml_node *n, struct aml_object *o) {
    if (!n) { aml_put(o); return; }
    aml_put(n->value);
    n->value = o;
}

/* A parsed NameString: where it starts from and the segments after that. */
#define AML_MAX_SEGS 16
struct aml_path {
    int  root;
    int  parents;
    int  nsegs;
    char seg[AML_MAX_SEGS][AML_NAME_LEN];
};

static bool parse_namestring(struct aml_ctx *c, struct aml_path *out) {
    memset(out, 0, sizeof *out);
    if (c->p >= c->end) return false;

    if (*c->p == OP_ROOT_CHAR) { out->root = 1; c->p++; }
    else while (c->p < c->end && *c->p == OP_PARENT_CHAR) { out->parents++; c->p++; }

    if (c->p >= c->end) return false;
    int count;
    if (*c->p == OP_DUAL_NAME)       { c->p++; count = 2; }
    else if (*c->p == OP_MULTI_NAME) { c->p++; if (c->p >= c->end) return false;
                                       count = *c->p++; }
    else if (*c->p == 0x00)          { c->p++; count = 0; }   /* NullName */
    else                             { count = 1; }

    if (count > AML_MAX_SEGS) return false;
    for (int i = 0; i < count; i++) {
        if (c->p + AML_NAME_LEN > c->end) return false;
        memcpy(out->seg[i], c->p, AML_NAME_LEN);
        c->p += AML_NAME_LEN;
    }
    out->nsegs = count;
    return true;
}

/* Resolve a parsed path. `create` makes the final segment if it is absent.
 *
 * THE UPWARD SEARCH is ACPI's own rule and the part that surprises: a bare
 * four-character name with no prefix is looked for in the current scope, then
 * its parent, and so on to the root. Anything with a `\`, a `^`, or more than
 * one segment resolves exactly from where it says. */
static struct aml_node *ns_resolve(struct aml_node *from,
                                   const struct aml_path *path, bool create) {
    struct aml_node *base = path->root ? &g_root : (from ? from : &g_root);
    for (int i = 0; i < path->parents && base->parent; i++) base = base->parent;

    if (path->nsegs == 0) return base;

    if (!path->root && path->parents == 0 && path->nsegs == 1 && !create) {
        for (struct aml_node *s = base; s; s = s->parent) {
            struct aml_node *n = node_child(s, path->seg[0]);
            if (n) return n;
        }
        return NULL;
    }

    struct aml_node *n = base;
    for (int i = 0; i < path->nsegs; i++) {
        bool last = (i == path->nsegs - 1);
        struct aml_node *next = node_child(n, path->seg[i]);
        if (!next) {
            /* Intermediate scopes are created too: firmware writes
             * `Scope(\_SB.PCI0)` in an SSDT loaded before the DSDT that
             * declares PCI0 often enough that refusing would lose the table. */
            if (!create && !last) return NULL;
            if (!create) return NULL;
            next = node_create(n, path->seg[i]);
            if (!next) return NULL;
        }
        n = next;
    }
    return n;
}

struct aml_node *aml_root(void) { return &g_root; }

struct aml_node *aml_name_node(struct aml_object *o) {
    if (!o) return NULL;
    if (o->kind == AML_REF) return o->u.ref.node;
    if (o->kind == AML_UNRESOLVED)
        return aml_lookup(o->u.uname.scope, o->u.uname.path);
    return NULL;
}

/* Text path -> node, for callers outside this file. */
struct aml_node *aml_lookup(struct aml_node *from, const char *path) {
    struct aml_path p;
    memset(&p, 0, sizeof p);
    const char *s = path;
    if (*s == '\\') { p.root = 1; s++; }
    while (*s == '^') { p.parents++; s++; }
    while (*s) {
        if (*s == '.') { s++; continue; }
        if (p.nsegs >= AML_MAX_SEGS) return NULL;
        int i = 0;
        while (i < AML_NAME_LEN && *s && *s != '.') p.seg[p.nsegs][i++] = *s++;
        while (i < AML_NAME_LEN) p.seg[p.nsegs][i++] = '_';
        p.nsegs++;
    }
    return ns_resolve(from, &p, false);
}

static uint32_t node_path(struct aml_node *n, char *out, uint32_t cap) {
    struct aml_node *chain[32];
    int d = 0;
    for (struct aml_node *x = n; x && x != &g_root && d < 32; x = x->parent)
        chain[d++] = x;
    uint32_t i = 0;
    if (i < cap - 1) out[i++] = '\\';
    for (int k = d - 1; k >= 0; k--) {
        if (k != d - 1 && i < cap - 1) out[i++] = '.';
        for (int j = 0; j < AML_NAME_LEN && i < cap - 1; j++)
            out[i++] = chain[k]->name[j];
    }
    out[i] = 0;
    return i;
}

/* ---- PkgLength ---------------------------------------------------------- */
static bool pkg_length(struct aml_ctx *c, uint32_t *out) {
    if (c->p >= c->end) return false;
    uint8_t lead = *c->p++;
    uint32_t follow = (lead >> 6) & 3;
    uint32_t len;
    if (follow == 0) {
        len = lead & 0x3F;
    } else {
        len = lead & 0x0F;
        for (uint32_t i = 0; i < follow; i++) {
            if (c->p >= c->end) return false;
            len |= (uint32_t)(*c->p++) << (4 + 8 * i);
        }
    }
    *out = len;
    return true;
}

/* The end of a package whose PkgLength starts at `start` (the byte AFTER the
 * opcode). The length COUNTS ITS OWN ENCODING, which is the detail that makes
 * an off-by-a-few parser read one field into the next. */
static const uint8_t *pkg_end(struct aml_ctx *c, const uint8_t *start) {
    uint32_t len;
    if (!pkg_length(c, &len)) return NULL;
    const uint8_t *e = start + len;
    return (e > c->end || e < c->p) ? NULL : e;
}

/* ---- operation regions: reaching the hardware ---------------------------- */

/* Where a PCI_Config region's accesses actually go. The region names an
 * OFFSET inside some device's configuration space, and WHICH device is not in
 * the region at all -- it is the _ADR of the enclosing Device and the _BBN of
 * the host bridge above it. So the answer is a walk up the namespace. */
static bool pci_location(struct aml_node *from, uint8_t *bus,
                         uint8_t *dev, uint8_t *fn) {
    uint64_t adr = 0;
    bool have_adr = false;
    struct aml_node *n;
    for (n = from; n; n = n->parent) {
        if (!have_adr && node_child(n, "_ADR") &&
            aml_eval_integer(n, "_ADR", &adr)) { have_adr = true; }
        if (node_child(n, "_BBN")) {
            uint64_t bbn = 0;
            if (aml_eval_integer(n, "_BBN", &bbn)) { *bus = (uint8_t)bbn; goto got_bus; }
        }
        if (node_child(n, "_HID")) {
            /* A host bridge with no _BBN is bus 0 -- the usual single-segment
             * machine, and the only one this kernel's PCI layer knows. */
            *bus = 0;
            goto got_bus;
        }
    }
    *bus = 0;
got_bus:
    if (!have_adr) return false;
    *dev = (uint8_t)((adr >> 16) & 0xFF);
    *fn  = (uint8_t)(adr & 0xFF);
    return true;
}

/* THE EMBEDDED CONTROLLER, which is how a laptop's battery, lid and fan are
 * actually reached. Two I/O ports: 0x66 takes a command and reports status,
 * 0x62 carries data. Read is 0x80, write 0x81.
 *
 * UNVERIFIED, and it has to be said plainly: QEMU emulates no embedded
 * controller, so nothing here has ever executed against one. The protocol is
 * from the ACPI specification and the timeouts are what keep a machine that
 * does not answer from hanging the boot rather than what makes a machine that
 * does answer work. docs/TODO.md records it as needing the target machine. */
#if defined(__x86_64__)
#define EC_DATA 0x62
#define EC_CMD  0x66
#define EC_OBF  (1u << 0)     /* output buffer full: data is waiting for us   */
#define EC_IBF  (1u << 1)     /* input buffer full: it has not read ours yet  */

static bool ec_wait(uint8_t mask, uint8_t want) {
    for (int i = 0; i < 100000; i++) {
        if ((inb(EC_CMD) & mask) == want) return true;
        for (volatile int d = 0; d < 100; d++) { }
    }
    return false;
}

static bool ec_read(uint8_t addr, uint8_t *out) {
    if (!ec_wait(EC_IBF, 0)) return false;
    outb(EC_CMD, 0x80);
    if (!ec_wait(EC_IBF, 0)) return false;
    outb(EC_DATA, addr);
    if (!ec_wait(EC_OBF, EC_OBF)) return false;
    *out = inb(EC_DATA);
    return true;
}

static bool ec_write(uint8_t addr, uint8_t val) {
    if (!ec_wait(EC_IBF, 0)) return false;
    outb(EC_CMD, 0x81);
    if (!ec_wait(EC_IBF, 0)) return false;
    outb(EC_DATA, addr);
    if (!ec_wait(EC_IBF, 0)) return false;
    outb(EC_DATA, val);
    return true;
}
#endif

/* One aligned access of `width` bytes at `offset` within a region. */
static bool region_read(struct aml_object *r, struct aml_node *owner,
                        uint64_t offset, uint32_t width, uint64_t *out) {
    uint64_t addr = r->u.region.offset + offset;
    *out = 0;
    switch (r->u.region.space) {
    case AML_REGION_SYSTEM_MEMORY: {
        if (!r->u.region.mapped) {
            uint64_t len = r->u.region.length ? r->u.region.length : 0x1000;
            r->u.region.mapped = vmm_map_mmio(r->u.region.offset, len);
            if (!r->u.region.mapped) return false;
        }
        volatile uint8_t *v = (volatile uint8_t *)(uintptr_t)
                              (r->u.region.mapped + offset);
        switch (width) {
        case 1: *out = *v; return true;
        case 2: *out = *(volatile uint16_t *)v; return true;
        case 4: *out = *(volatile uint32_t *)v; return true;
        case 8: *out = *(volatile uint64_t *)v; return true;
        }
        return false;
    }
#if defined(__x86_64__)
    case AML_REGION_SYSTEM_IO:
        switch (width) {
        case 1: *out = inb((uint16_t)addr); return true;
        case 2: *out = inw((uint16_t)addr); return true;
        case 4: *out = inl((uint16_t)addr); return true;
        }
        return false;
    case AML_REGION_EMBEDDED_CTRL: {
        uint64_t v = 0;
        for (uint32_t i = 0; i < width; i++) {
            uint8_t b;
            if (!ec_read((uint8_t)(addr + i), &b)) return false;
            v |= (uint64_t)b << (i * 8);
        }
        *out = v;
        return true;
    }
#endif
    case AML_REGION_PCI_CONFIG: {
        uint8_t bus, dev, fn;
        /* WHICH DEVICE'S CONFIG SPACE is a property of where the REGION was
         * declared, not of where the field was. Firmware routinely declares
         * the region inside the device and the fields over it from a
         * different scope -- QEMU puts the PIIX3's interrupt-routing region
         * at \_SB.PCI0.S08.P40C and the PRQ0-3 fields at \_SB -- so walking
         * up from the field finds no _ADR, every read fails, and every PCI
         * interrupt in the machine resolves to line 0. */
        struct aml_node *loc = r->u.region.owner ? r->u.region.owner : owner;
        if (!pci_location(loc, &bus, &dev, &fn)) return false;
        uint32_t v = arch_pci_cfg_read32(bus, dev, fn, (uint8_t)(addr & 0xFC));
        uint32_t sh = (uint32_t)(addr & 3) * 8;
        switch (width) {
        case 1: *out = (v >> sh) & 0xFF; return true;
        case 2: *out = (v >> sh) & 0xFFFF; return true;
        case 4: *out = v; return true;
        }
        return false;
    }
    default:
        return false;
    }
}

static bool region_write(struct aml_object *r, struct aml_node *owner,
                         uint64_t offset, uint32_t width, uint64_t val) {
    uint64_t addr = r->u.region.offset + offset;
    switch (r->u.region.space) {
    case AML_REGION_SYSTEM_MEMORY: {
        if (!r->u.region.mapped) {
            uint64_t len = r->u.region.length ? r->u.region.length : 0x1000;
            r->u.region.mapped = vmm_map_mmio(r->u.region.offset, len);
            if (!r->u.region.mapped) return false;
        }
        volatile uint8_t *v = (volatile uint8_t *)(uintptr_t)
                              (r->u.region.mapped + offset);
        switch (width) {
        case 1: *v = (uint8_t)val; return true;
        case 2: *(volatile uint16_t *)v = (uint16_t)val; return true;
        case 4: *(volatile uint32_t *)v = (uint32_t)val; return true;
        case 8: *(volatile uint64_t *)v = val; return true;
        }
        return false;
    }
#if defined(__x86_64__)
    case AML_REGION_SYSTEM_IO:
        switch (width) {
        case 1: outb((uint16_t)addr, (uint8_t)val); return true;
        case 2: outw((uint16_t)addr, (uint16_t)val); return true;
        case 4: outl((uint16_t)addr, (uint32_t)val); return true;
        }
        return false;
    case AML_REGION_EMBEDDED_CTRL:
        for (uint32_t i = 0; i < width; i++)
            if (!ec_write((uint8_t)(addr + i), (uint8_t)(val >> (i * 8))))
                return false;
        return true;
#endif
    case AML_REGION_PCI_CONFIG: {
        uint8_t bus, dev, fn;
        struct aml_node *loc = r->u.region.owner ? r->u.region.owner : owner;
        if (!pci_location(loc, &bus, &dev, &fn)) return false;
        uint8_t off = (uint8_t)(addr & 0xFC);
        uint32_t sh = (uint32_t)(addr & 3) * 8;
        uint32_t cur = arch_pci_cfg_read32(bus, dev, fn, off);
        uint32_t mask;
        switch (width) {
        case 1: mask = 0xFFu << sh; break;
        case 2: mask = 0xFFFFu << sh; break;
        case 4: mask = 0xFFFFFFFFu; sh = 0; break;
        default: return false;
        }
        arch_pci_cfg_write32(bus, dev, fn, off,
                             (cur & ~mask) | (((uint32_t)val << sh) & mask));
        return true;
    }
    default:
        return false;
    }
}

/* ---- field units: a run of bits inside a region -------------------------
 *
 * A Field declares named bit ranges, and the ranges need not be byte aligned
 * or byte sized -- `Offset(0x10), , 3, FLG1, 1` is ordinary. So a read walks
 * the ACCESS-SIZED units the range touches, takes the bits it wants from each,
 * and assembles them; a write does the same in reverse and, where the range
 * does not fill a unit, READS IT BACK FIRST so the neighbouring bits survive.
 *
 * The access size is not a hint. A field declared DWordAcc on an I/O region
 * must be read 32 bits at a time; reading it as four bytes is four different
 * bus transactions and hardware that latches on a read sees four events. */
static uint32_t access_bits(uint8_t access, uint32_t bit_width) {
    switch (access) {
    case AML_ACCESS_BYTE:  return 8;
    case AML_ACCESS_WORD:  return 16;
    case AML_ACCESS_DWORD: return 32;
    case AML_ACCESS_QWORD: return 64;
    default:
        /* AnyAcc: the smallest unit that can hold the field, which is what
         * firmware means by it and what avoids splitting a 16-bit register. */
        if (bit_width <= 8)  return 8;
        if (bit_width <= 16) return 16;
        if (bit_width <= 32) return 32;
        return 64;
    }
}

static bool field_read(struct aml_object *f, struct aml_node *fnode,
                       uint64_t *out) {
    struct aml_node *rn = f->u.field.region;
    if (!rn || !rn->value || rn->value->kind != AML_REGION) return false;

    uint32_t abits = access_bits(f->u.field.access, f->u.field.bit_width);
    uint32_t abytes = abits / 8;
    uint64_t value = 0;
    uint32_t done = 0;

    while (done < f->u.field.bit_width) {
        uint32_t bit = f->u.field.bit_offset + done;
        uint64_t unit = (bit / abits) * abytes;
        uint32_t within = bit % abits;
        uint32_t take = abits - within;
        if (take > f->u.field.bit_width - done) take = f->u.field.bit_width - done;

        uint64_t raw;
        if (!region_read(rn->value, fnode ? fnode : rn, unit, abytes, &raw))
            return false;
        uint64_t mask = (take >= 64) ? ~0ULL : ((1ULL << take) - 1);
        value |= ((raw >> within) & mask) << done;
        done += take;
    }
    *out = value;
    return true;
}

static bool field_write(struct aml_object *f, struct aml_node *fnode,
                        uint64_t value) {
    struct aml_node *rn = f->u.field.region;
    if (!rn || !rn->value || rn->value->kind != AML_REGION) return false;

    uint32_t abits = access_bits(f->u.field.access, f->u.field.bit_width);
    uint32_t abytes = abits / 8;
    uint32_t done = 0;

    while (done < f->u.field.bit_width) {
        uint32_t bit = f->u.field.bit_offset + done;
        uint64_t unit = (bit / abits) * abytes;
        uint32_t within = bit % abits;
        uint32_t take = abits - within;
        if (take > f->u.field.bit_width - done) take = f->u.field.bit_width - done;

        uint64_t mask = (take >= 64) ? ~0ULL : ((1ULL << take) - 1);
        uint64_t bits = (value >> done) & mask;
        uint64_t raw = 0;

        if (take != abits) {
            /* Only part of this unit belongs to the field. What happens to the
             * rest is the field's UpdateRule, and getting it wrong writes over
             * a neighbouring register that happened to share the word. */
            switch (f->u.field.update) {
            case AML_UPDATE_WRITE_AS_ONES:  raw = ~0ULL; break;
            case AML_UPDATE_WRITE_AS_ZEROS: raw = 0; break;
            default:
                if (!region_read(rn->value, fnode ? fnode : rn, unit, abytes, &raw))
                    return false;
                break;
            }
        }
        raw = (raw & ~(mask << within)) | (bits << within);
        if (!region_write(rn->value, fnode ? fnode : rn, unit, abytes, raw))
            return false;
        done += take;
    }
    return true;
}

/* ---- reading a node's value, running it if it is a method ---------------- */
static struct aml_object *node_value(struct aml_node *n, struct aml_ctx *c);

/* ---- targets: where a Store goes ---------------------------------------- */
struct aml_target {
    enum { T_NONE, T_LOCAL, T_ARG, T_NODE, T_INDEX, T_DEBUG } kind;
    int                idx;
    struct aml_node   *node;
    struct aml_object *base;     /* for T_INDEX: the package or buffer */
};

static void target_release(struct aml_target *t) {
    if (t->kind == T_INDEX) aml_put(t->base);
    t->kind = T_NONE;
}

static bool parse_target(struct aml_ctx *c, struct aml_target *t) {
    memset(t, 0, sizeof *t);
    if (c->p >= c->end) return false;
    uint8_t op = *c->p;

    if (op == OP_ZERO) { c->p++; t->kind = T_NONE; return true; }  /* discard */
    if (op >= OP_LOCAL0 && op < OP_LOCAL0 + 8) {
        c->p++; t->kind = T_LOCAL; t->idx = op - OP_LOCAL0; return true;
    }
    if (op >= OP_ARG0 && op < OP_ARG0 + 7) {
        c->p++; t->kind = T_ARG; t->idx = op - OP_ARG0; return true;
    }
    if (op == OP_EXT && c->p + 1 < c->end && c->p[1] == EXT_DEBUG) {
        c->p += 2; t->kind = T_DEBUG; return true;
    }
    if (op == OP_INDEX) {
        c->p++;
        struct aml_object *base = term_arg(c);
        struct aml_object *idx  = term_arg(c);
        struct aml_target sink;
        parse_target(c, &sink);          /* Index's own target; ignored here */
        target_release(&sink);
        if (!base || !idx) { aml_put(base); aml_put(idx); return false; }
        t->kind = T_INDEX;
        t->base = base;
        t->idx  = (int)obj_as_integer(idx);
        aml_put(idx);
        return true;
    }
    if (op == OP_DEREF_OF) {
        c->p++;
        struct aml_object *r = term_arg(c);
        if (r && r->kind == AML_REF && r->u.ref.node) {
            t->kind = T_NODE; t->node = r->u.ref.node;
            aml_put(r);
            return true;
        }
        aml_put(r);
        return false;
    }

    struct aml_path path;
    if (!parse_namestring(c, &path)) return false;
    struct aml_node *n = ns_resolve(c->scope, &path, false);
    if (!n) n = ns_resolve(c->scope, &path, true);   /* Store creates names */
    if (!n) return false;
    t->kind = T_NODE;
    t->node = n;
    return true;
}

/* Convert `src` for storage into a destination that already has a kind.
 * ACPI's store rules are destination-driven; the cases below are the ones
 * firmware actually relies on. */
static bool store_into(struct aml_target *t, struct aml_object *src,
                       struct aml_ctx *c) {
    switch (t->kind) {
    case T_NONE:
        return true;
    case T_DEBUG: {
        if (src && src->kind == AML_INTEGER)
            kprintf("AML Debug: 0x%llx\n", (unsigned long long)src->u.integer);
        else if (src && src->kind == AML_STRING)
            kprintf("AML Debug: %s\n", (const char *)src->u.buf.data);
        else
            kprintf("AML Debug: object kind %d\n", src ? (int)src->kind : -1);
        return true;
    }
    case T_LOCAL:
        aml_put(c->local[t->idx]);
        c->local[t->idx] = aml_get(src);
        return true;
    case T_ARG:
        aml_put(c->arg[t->idx]);
        c->arg[t->idx] = aml_get(src);
        return true;
    case T_INDEX:
        if (t->base && t->base->kind == AML_PACKAGE &&
            t->idx >= 0 && (uint32_t)t->idx < t->base->u.pkg.count) {
            aml_put(t->base->u.pkg.elem[t->idx]);
            t->base->u.pkg.elem[t->idx] = aml_get(src);
            return true;
        }
        if (t->base && (t->base->kind == AML_BUFFER || t->base->kind == AML_STRING) &&
            t->idx >= 0 && (uint32_t)t->idx < t->base->u.buf.len) {
            t->base->u.buf.data[t->idx] = (uint8_t)obj_as_integer(src);
            return true;
        }
        return false;
    case T_NODE: {
        struct aml_object *cur = t->node->value;
        if (cur && cur->kind == AML_FIELD)
            return field_write(cur, t->node, obj_as_integer(src));
        if (cur && cur->kind == AML_BUFFER_FIELD) {
            struct aml_object *b = cur->u.bfield.buffer;
            uint64_t v = obj_as_integer(src);
            for (uint32_t i = 0; i < cur->u.bfield.bit_width; i++) {
                uint32_t bit = cur->u.bfield.bit_offset + i;
                if (bit / 8 >= b->u.buf.len) break;
                uint8_t m = (uint8_t)(1u << (bit % 8));
                if ((v >> i) & 1) b->u.buf.data[bit / 8] |= m;
                else              b->u.buf.data[bit / 8] &= (uint8_t)~m;
            }
            return true;
        }
        node_set(t->node, aml_get(src));
        return true;
    }
    }
    return false;
}

/* ---- the executor ------------------------------------------------------- */

/* A method's result, or its value if it is not a method. */
static struct aml_object *invoke(struct aml_node *n, struct aml_object **args,
                                 int argc, struct aml_ctx *outer);

static struct aml_object *node_value(struct aml_node *n, struct aml_ctx *c) {
    if (!n) return NULL;
    struct aml_object *v = n->value;
    if (!v) return NULL;
    if (v->kind == AML_METHOD) return invoke(n, NULL, 0, c);
    if (v->kind == AML_FIELD) {
        uint64_t x = 0;
        if (!field_read(v, n, &x)) return NULL;
        return aml_integer(x);
    }
    if (v->kind == AML_BUFFER_FIELD) {
        struct aml_object *b = v->u.bfield.buffer;
        uint64_t x = 0;
        for (uint32_t i = 0; i < v->u.bfield.bit_width && i < 64; i++) {
            uint32_t bit = v->u.bfield.bit_offset + i;
            if (bit / 8 >= b->u.buf.len) break;
            if (b->u.buf.data[bit / 8] & (1u << (bit % 8))) x |= 1ULL << i;
        }
        return aml_integer(x);
    }
    return aml_get(v);
}

/* Two operands and a target: Add, Subtract, And, Or, ... */
static struct aml_object *binop(struct aml_ctx *c, uint8_t op) {
    struct aml_object *a = term_arg(c);
    struct aml_object *b = term_arg(c);
    uint64_t x = obj_as_integer(a), y = obj_as_integer(b), r = 0;
    aml_put(a); aml_put(b);

    switch (op) {
    case OP_ADD:         r = x + y; break;
    case OP_SUBTRACT:    r = x - y; break;
    case OP_MULTIPLY:    r = x * y; break;
    case OP_MOD:         r = y ? x % y : 0; break;
    case OP_SHIFT_LEFT:  r = (y >= 64) ? 0 : (x << y); break;
    case OP_SHIFT_RIGHT: r = (y >= 64) ? 0 : (x >> y); break;
    case OP_AND:         r = x & y; break;
    case OP_NAND:        r = ~(x & y); break;
    case OP_OR:          r = x | y; break;
    case OP_NOR:         r = ~(x | y); break;
    case OP_XOR:         r = x ^ y; break;
    default:             r = 0; break;
    }

    struct aml_target t;
    if (parse_target(c, &t)) {
        struct aml_object *v = aml_integer(r);
        store_into(&t, v, c);
        target_release(&t);
        aml_put(v);
    }
    return aml_integer(r);
}

static struct aml_object *logic(struct aml_ctx *c, uint8_t op) {
    struct aml_object *a = term_arg(c);
    struct aml_object *b = term_arg(c);
    bool r = false;

    /* Strings compare as strings, everything else as integers. A DSDT that
     * does LEqual(_OS, "Windows") depends on this and gets 0 == 0 otherwise. */
    if (a && b && a->kind == AML_STRING && b->kind == AML_STRING) {
        uint32_t n = a->u.buf.len < b->u.buf.len ? a->u.buf.len : b->u.buf.len;
        int cmp = n ? memcmp(a->u.buf.data, b->u.buf.data, n) : 0;
        if (cmp == 0) cmp = (int)a->u.buf.len - (int)b->u.buf.len;
        r = (op == OP_LEQUAL) ? (cmp == 0)
          : (op == OP_LGREATER) ? (cmp > 0) : (cmp < 0);
    } else {
        uint64_t x = obj_as_integer(a), y = obj_as_integer(b);
        r = (op == OP_LEQUAL) ? (x == y)
          : (op == OP_LGREATER) ? (x > y)
          : (op == OP_LLESS) ? (x < y)
          : (op == OP_LAND) ? (x && y) : (x || y);
    }
    aml_put(a); aml_put(b);
    return aml_integer(r ? ~0ULL : 0);
}

/* CreateXxxField(buffer, index, name) -- a named window into a buffer. */
static bool create_bfield(struct aml_ctx *c, uint32_t width_bits, bool bit_index) {
    struct aml_object *buf = term_arg(c);
    struct aml_object *idx = term_arg(c);
    struct aml_path path;
    bool ok = false;

    if (buf && idx && buf->kind == AML_BUFFER && parse_namestring(c, &path)) {
        struct aml_node *n = ns_resolve(c->scope, &path, true);
        struct aml_object *f = obj_new(AML_BUFFER_FIELD);
        if (n && f) {
            f->u.bfield.buffer = aml_get(buf);
            f->u.bfield.bit_offset = (uint32_t)obj_as_integer(idx) *
                                     (bit_index ? 1u : 8u);
            f->u.bfield.bit_width = width_bits;
            node_set(n, f);
            ok = true;
        } else {
            aml_put(f);
        }
    }
    aml_put(buf); aml_put(idx);
    return ok;
}

/* A DECLARATION IS A STATEMENT, and that is not a technicality.
 *
 * A method body may declare names: `Name(PRR0, ResourceTemplate(){...})`,
 * then patch and return it, is how firmware builds a _CRS it has to compute.
 * QEMU's own IQCR does exactly that, and an executor that treats every
 * declarative opcode as "not executable" stops at the first byte of it -- as
 * this one did, on the method that decides where PCI interrupts go.
 *
 * The names go into the METHOD's scope, so two devices calling the same
 * helper do not collide, and a second call overwrites rather than duplicates. */
static bool is_declaration(const struct aml_ctx *c, uint8_t op) {
    switch (op) {
    case OP_NAME: case OP_SCOPE: case OP_METHOD: case OP_ALIAS: case OP_EXTERNAL:
        return true;
    case OP_EXT:
        if (c->p + 1 >= c->end) return false;
        switch (c->p[1]) {
        case EXT_OP_REGION: case EXT_FIELD: case EXT_DEVICE: case EXT_PROCESSOR:
        case EXT_POWER_RES: case EXT_THERMAL_ZN: case EXT_INDEX_FIELD:
        case EXT_BANK_FIELD: case EXT_MUTEX: case EXT_EVENT:
            return true;
        default:
            return false;
        }
    default:
        return false;
    }
}

/* Read one complete term and produce its value. This is the interpreter. */
static struct aml_object *term_arg(struct aml_ctx *c) {
    if (c->failed || c->p >= c->end) return NULL;
    if (++(*c->steps) > AML_MAX_STEPS) { c->failed = true; return NULL; }

    if (is_declaration(c, *c->p)) {
        if (!load_term(c)) { c->failed = true; return NULL; }
        return aml_integer(0);
    }

    uint8_t op = *c->p++;
    switch (op) {
    case OP_ZERO: return aml_integer(0);
    case OP_ONE:  return aml_integer(1);
    case OP_ONES: return aml_integer(~0ULL);
    case OP_BYTE:
        if (c->p + 1 > c->end) break;
        return aml_integer(*c->p++);
    case OP_WORD: {
        if (c->p + 2 > c->end) break;
        uint64_t v = (uint64_t)c->p[0] | ((uint64_t)c->p[1] << 8);
        c->p += 2; return aml_integer(v);
    }
    case OP_DWORD: {
        if (c->p + 4 > c->end) break;
        uint64_t v = 0;
        for (int i = 0; i < 4; i++) v |= (uint64_t)c->p[i] << (i * 8);
        c->p += 4; return aml_integer(v);
    }
    case OP_QWORD: {
        if (c->p + 8 > c->end) break;
        uint64_t v = 0;
        for (int i = 0; i < 8; i++) v |= (uint64_t)c->p[i] << (i * 8);
        c->p += 8; return aml_integer(v);
    }
    case OP_STRING: {
        const uint8_t *s = c->p;
        while (c->p < c->end && *c->p) c->p++;
        uint32_t len = (uint32_t)(c->p - s);
        if (c->p < c->end) c->p++;                 /* the NUL */
        return obj_string((const char *)s, len);
    }
    case OP_BUFFER: {
        const uint8_t *start = c->p;
        const uint8_t *e = pkg_end(c, start);
        if (!e) break;
        struct aml_object *size = term_arg(c);
        uint32_t n = (uint32_t)obj_as_integer(size);
        aml_put(size);
        uint32_t init = (uint32_t)(e - c->p);
        if (n < init) n = init;                     /* declared short: trust data */
        struct aml_object *b = obj_buffer(NULL, n);
        if (b && init) memcpy(b->u.buf.data, c->p, init);
        c->p = e;
        return b;
    }
    case OP_PACKAGE:
    case OP_VAR_PACKAGE: {
        const uint8_t *start = c->p;
        const uint8_t *e = pkg_end(c, start);
        if (!e) break;
        uint32_t n;
        if (op == OP_PACKAGE) {
            if (c->p >= e) break;
            n = *c->p++;
        } else {
            struct aml_object *cnt = term_arg(c);
            n = (uint32_t)obj_as_integer(cnt);
            aml_put(cnt);
        }
        struct aml_object *pkg = obj_package(n);
        if (!pkg) { c->p = e; break; }
        const uint8_t *saved_end = c->end;
        c->end = e;
        for (uint32_t i = 0; i < n && c->p < e; i++) {
            /* A package element may NAME something that is declared later in
             * the table -- legal, and common in _PRT, whose fourth element is
             * often a link device defined further down. An unresolved name is
             * kept as its TEXT rather than dropped, so the consumer can look
             * it up once the whole table is loaded. */
            if (c->p < e && (*c->p == OP_ROOT_CHAR || *c->p == OP_PARENT_CHAR ||
                             *c->p == OP_DUAL_NAME || *c->p == OP_MULTI_NAME ||
                             (*c->p >= 'A' && *c->p <= 'Z') || *c->p == '_')) {
                struct aml_path path;
                const uint8_t *before = c->p;
                if (parse_namestring(c, &path)) {
                    struct aml_node *n2 = ns_resolve(c->scope, &path, false);
                    if (n2) {
                        struct aml_object *r = obj_new(AML_REF);
                        if (r) { r->u.ref.node = n2; r->u.ref.index = -1; }
                        pkg->u.pkg.elem[i] = r;
                    } else {
                        /* NOT AN ERROR, and not a string either: the name may
                         * simply not be declared YET. Keep the text and the
                         * scope so the same search can run once the table is
                         * loaded -- which is when ACPI says names resolve. */
                        char txt[AML_MAX_SEGS * 5 + 2];
                        uint32_t k = 0;
                        if (path.root && k < sizeof txt - 1) txt[k++] = '\\';
                        for (int s = 0; s < path.nsegs; s++) {
                            if (s && k < sizeof txt - 1) txt[k++] = '.';
                            for (int j = 0; j < AML_NAME_LEN && k < sizeof txt - 1; j++)
                                txt[k++] = path.seg[s][j];
                        }
                        txt[k] = 0;
                        struct aml_object *u = obj_new(AML_UNRESOLVED);
                        if (u) {
                            u->u.uname.scope = c->scope;
                            u->u.uname.path = kmalloc(k + 1);
                            if (u->u.uname.path) memcpy(u->u.uname.path, txt, k + 1);
                            else { aml_put(u); u = NULL; }
                        }
                        pkg->u.pkg.elem[i] = u;
                    }
                    continue;
                }
                c->p = before;
            }
            pkg->u.pkg.elem[i] = term_arg(c);
        }
        c->end = saved_end;
        c->p = e;
        return pkg;
    }

    case OP_STORE: {
        struct aml_object *src = term_arg(c);
        struct aml_target t;
        if (parse_target(c, &t)) { store_into(&t, src, c); target_release(&t); }
        return src;
    }
    case OP_COPY_OBJECT: {
        struct aml_object *src = term_arg(c);
        struct aml_target t;
        if (parse_target(c, &t)) { store_into(&t, src, c); target_release(&t); }
        return src;
    }
    case OP_ADD: case OP_SUBTRACT: case OP_MULTIPLY: case OP_MOD:
    case OP_SHIFT_LEFT: case OP_SHIFT_RIGHT: case OP_AND: case OP_NAND:
    case OP_OR: case OP_NOR: case OP_XOR:
        return binop(c, op);
    case OP_DIVIDE: {
        struct aml_object *a = term_arg(c), *b = term_arg(c);
        uint64_t x = obj_as_integer(a), y = obj_as_integer(b);
        aml_put(a); aml_put(b);
        uint64_t q = y ? x / y : 0, rem = y ? x % y : 0;
        struct aml_target tr, tq;
        if (parse_target(c, &tr)) {
            struct aml_object *v = aml_integer(rem);
            store_into(&tr, v, c); target_release(&tr); aml_put(v);
        }
        if (parse_target(c, &tq)) {
            struct aml_object *v = aml_integer(q);
            store_into(&tq, v, c); target_release(&tq); aml_put(v);
        }
        return aml_integer(q);
    }
    case OP_NOT: {
        struct aml_object *a = term_arg(c);
        uint64_t r = ~obj_as_integer(a);
        aml_put(a);
        struct aml_target t;
        if (parse_target(c, &t)) {
            struct aml_object *v = aml_integer(r);
            store_into(&t, v, c); target_release(&t); aml_put(v);
        }
        return aml_integer(r);
    }
    case OP_FIND_SET_L: case OP_FIND_SET_R: {
        struct aml_object *a = term_arg(c);
        uint64_t x = obj_as_integer(a), r = 0;
        aml_put(a);
        if (x) {
            if (op == OP_FIND_SET_L) { for (int i = 63; i >= 0; i--)
                                           if (x & (1ULL << i)) { r = (uint64_t)i + 1; break; } }
            else                     { for (int i = 0; i < 64; i++)
                                           if (x & (1ULL << i)) { r = (uint64_t)i + 1; break; } }
        }
        struct aml_target t;
        if (parse_target(c, &t)) {
            struct aml_object *v = aml_integer(r);
            store_into(&t, v, c); target_release(&t); aml_put(v);
        }
        return aml_integer(r);
    }
    case OP_INCREMENT: case OP_DECREMENT: {
        const uint8_t *save = c->p;
        struct aml_target t;
        if (!parse_target(c, &t)) break;
        c->p = save;
        struct aml_object *cur = term_arg(c);
        uint64_t v = obj_as_integer(cur) + (op == OP_INCREMENT ? 1 : (uint64_t)-1);
        aml_put(cur);
        struct aml_object *nv = aml_integer(v);
        store_into(&t, nv, c);
        target_release(&t);
        return nv;
    }
    case OP_LAND: case OP_LOR: case OP_LEQUAL: case OP_LGREATER: case OP_LLESS:
        return logic(c, op);
    case OP_LNOT: {
        /* 0x92 is also the prefix of the three "not" comparisons: LNotEqual is
         * 0x92 0x93, LLessEqual 0x92 0x94, LGreaterEqual 0x92 0x95. Reading it
         * as a bare LNot leaves the following opcode to be parsed as a term
         * and the method goes off the rails silently. */
        if (c->p < c->end && (*c->p == OP_LEQUAL || *c->p == OP_LGREATER ||
                              *c->p == OP_LLESS)) {
            uint8_t sub = *c->p++;
            struct aml_object *r = logic(c, sub);
            uint64_t v = obj_as_integer(r);
            aml_put(r);
            return aml_integer(v ? 0 : ~0ULL);
        }
        struct aml_object *a = term_arg(c);
        uint64_t x = obj_as_integer(a);
        aml_put(a);
        return aml_integer(x ? 0 : ~0ULL);
    }
    case OP_SIZE_OF: {
        struct aml_object *a = term_arg(c);
        uint64_t r = 0;
        if (a) {
            if (a->kind == AML_PACKAGE) r = a->u.pkg.count;
            else if (a->kind == AML_BUFFER || a->kind == AML_STRING) r = a->u.buf.len;
            else if (a->kind == AML_REF && a->u.ref.node)
                { struct aml_object *v = a->u.ref.node->value;
                  if (v && v->kind == AML_PACKAGE) r = v->u.pkg.count;
                  else if (v && (v->kind == AML_BUFFER || v->kind == AML_STRING))
                      r = v->u.buf.len; }
        }
        aml_put(a);
        return aml_integer(r);
    }
    case OP_INDEX: {
        struct aml_object *base = term_arg(c);
        struct aml_object *idx  = term_arg(c);
        uint32_t i = (uint32_t)obj_as_integer(idx);
        aml_put(idx);
        struct aml_object *r = obj_new(AML_REF);
        if (r) { r->u.ref.obj = aml_get(base); r->u.ref.index = (int)i; }
        struct aml_target t;
        if (parse_target(c, &t)) { store_into(&t, r, c); target_release(&t); }
        aml_put(base);
        return r;
    }
    case OP_DEREF_OF: {
        struct aml_object *r = term_arg(c);
        struct aml_object *v = NULL;
        if (r && r->kind == AML_REF) {
            if (r->u.ref.obj && r->u.ref.index >= 0) {
                struct aml_object *b = r->u.ref.obj;
                if (b->kind == AML_PACKAGE && (uint32_t)r->u.ref.index < b->u.pkg.count)
                    v = aml_get(b->u.pkg.elem[r->u.ref.index]);
                else if ((b->kind == AML_BUFFER || b->kind == AML_STRING) &&
                         (uint32_t)r->u.ref.index < b->u.buf.len)
                    v = aml_integer(b->u.buf.data[r->u.ref.index]);
            } else if (r->u.ref.node) {
                v = node_value(r->u.ref.node, c);
            } else if (r->u.ref.obj) {
                v = aml_get(r->u.ref.obj);
            }
        } else {
            v = aml_get(r);
        }
        aml_put(r);
        return v;
    }
    case OP_REF_OF: {
        struct aml_path path;
        const uint8_t *save = c->p;
        struct aml_object *r = obj_new(AML_REF);
        if (!r) break;
        r->u.ref.index = -1;
        if (parse_namestring(c, &path)) {
            r->u.ref.node = ns_resolve(c->scope, &path, false);
            if (!r->u.ref.node) { c->p = save; }
        }
        return r;
    }
    case OP_TO_INTEGER: {
        struct aml_object *a = term_arg(c);
        uint64_t v = obj_as_integer(a);
        aml_put(a);
        struct aml_target t;
        if (parse_target(c, &t)) {
            struct aml_object *o = aml_integer(v);
            store_into(&t, o, c); target_release(&t); aml_put(o);
        }
        return aml_integer(v);
    }
    case OP_TO_BUFFER: {
        struct aml_object *a = term_arg(c);
        struct aml_object *b;
        if (a && (a->kind == AML_BUFFER || a->kind == AML_STRING))
            b = obj_buffer(a->u.buf.data, a->u.buf.len);
        else { uint64_t v = obj_as_integer(a); b = obj_buffer((const uint8_t *)&v, 8); }
        aml_put(a);
        struct aml_target t;
        if (parse_target(c, &t)) { store_into(&t, b, c); target_release(&t); }
        return b;
    }
    case OP_OBJECT_TYPE: {
        struct aml_object *a = term_arg(c);
        uint64_t k = 0;
        if (a) switch (a->kind) {
            case AML_INTEGER: k = 1; break;
            case AML_STRING:  k = 2; break;
            case AML_BUFFER:  k = 3; break;
            case AML_PACKAGE: k = 4; break;
            case AML_FIELD:   k = 5; break;
            case AML_DEVICE:  k = 6; break;
            case AML_METHOD:  k = 8; break;
            default:          k = 0; break;
        }
        aml_put(a);
        return aml_integer(k);
    }
    case OP_MID: {
        struct aml_object *src = term_arg(c);
        struct aml_object *idx = term_arg(c);
        struct aml_object *len = term_arg(c);
        uint32_t i = (uint32_t)obj_as_integer(idx), n = (uint32_t)obj_as_integer(len);
        aml_put(idx); aml_put(len);
        struct aml_object *r = NULL;
        if (src && (src->kind == AML_BUFFER || src->kind == AML_STRING)) {
            if (i > src->u.buf.len) i = src->u.buf.len;
            if (i + n > src->u.buf.len) n = src->u.buf.len - i;
            r = (src->kind == AML_STRING)
                ? obj_string((const char *)src->u.buf.data + i, n)
                : obj_buffer(src->u.buf.data + i, n);
        }
        aml_put(src);
        if (!r) r = aml_integer(0);
        struct aml_target t;
        if (parse_target(c, &t)) { store_into(&t, r, c); target_release(&t); }
        return r;
    }
    case OP_CREATE_B:   if (!create_bfield(c, 8,  false)) break; return aml_integer(0);
    case OP_CREATE_W:   if (!create_bfield(c, 16, false)) break; return aml_integer(0);
    case OP_CREATE_DW:  if (!create_bfield(c, 32, false)) break; return aml_integer(0);
    case OP_CREATE_QW:  if (!create_bfield(c, 64, false)) break; return aml_integer(0);
    case OP_CREATE_BIT: if (!create_bfield(c, 1,  true))  break; return aml_integer(0);

    case OP_NOTIFY: {
        /* A device asking the OS to look at it again -- a battery changing
         * state, a lid closing. Nothing subscribes yet, so it is logged and
         * dropped rather than pretended about. */
        struct aml_object *dev = term_arg(c);
        struct aml_object *val = term_arg(c);
        aml_put(dev); aml_put(val);
        return aml_integer(0);
    }
    case OP_NOOP: case OP_BREAKPOINT:
        return aml_integer(0);

    case OP_IF: {
        const uint8_t *start = c->p;
        const uint8_t *e = pkg_end(c, start);
        if (!e) break;
        struct aml_object *pred = term_arg(c);
        bool taken = obj_as_integer(pred) != 0;
        aml_put(pred);
        if (taken) {
            if (!termlist_exec(c, e)) { c->failed = true; return NULL; }
            c->p = e;
        } else {
            c->p = e;
        }
        /* The Else, if there is one, is a separate term that follows. */
        if (c->p < c->end && *c->p == OP_ELSE) {
            c->p++;
            const uint8_t *estart = c->p;
            const uint8_t *ee = pkg_end(c, estart);
            if (!ee) break;
            if (!taken) {
                if (!termlist_exec(c, ee)) { c->failed = true; return NULL; }
            }
            c->p = ee;
        }
        return aml_integer(0);
    }
    case OP_ELSE: {
        /* Reached only when the matching If was not executed by us. */
        const uint8_t *start = c->p;
        const uint8_t *e = pkg_end(c, start);
        if (!e) break;
        c->p = e;
        return aml_integer(0);
    }
    case OP_WHILE: {
        const uint8_t *start = c->p;
        const uint8_t *e = pkg_end(c, start);
        if (!e) break;
        const uint8_t *body = c->p;
        for (;;) {
            if (++(*c->steps) > AML_MAX_STEPS) { c->failed = true; return NULL; }
            c->p = body;
            struct aml_object *pred = term_arg(c);
            bool go = obj_as_integer(pred) != 0;
            aml_put(pred);
            if (!go) break;
            if (!termlist_exec(c, e)) { c->failed = true; return NULL; }
            if (c->flow == FLOW_RETURN) return aml_integer(0);
            if (c->flow == FLOW_BREAK)  { c->flow = FLOW_NORMAL; break; }
            if (c->flow == FLOW_CONTINUE) c->flow = FLOW_NORMAL;
        }
        c->p = e;
        return aml_integer(0);
    }
    case OP_RETURN: {
        struct aml_object *v = term_arg(c);
        aml_put(c->ret);
        c->ret = v;
        c->flow = FLOW_RETURN;
        return aml_integer(0);
    }
    case OP_BREAK:    c->flow = FLOW_BREAK;    return aml_integer(0);
    case OP_CONTINUE: c->flow = FLOW_CONTINUE; return aml_integer(0);

    case OP_EXT: {
        if (c->p >= c->end) break;
        uint8_t x = *c->p++;
        switch (x) {
        case EXT_REVISION: return aml_integer(2);
        case EXT_DEBUG: {
            struct aml_object *d = obj_new(AML_DEBUG);
            return d;
        }
        case EXT_TIMER: return aml_integer(0);
        case EXT_STALL: case EXT_SLEEP: {
            struct aml_object *a = term_arg(c);
            uint64_t n = obj_as_integer(a);
            aml_put(a);
            /* Busy-wait. There is no scheduler to yield to during table load,
             * and a method that sleeps for a second is firmware waiting on
             * hardware -- shortening it is how a race becomes intermittent. */
            for (uint64_t i = 0; i < n * (x == EXT_SLEEP ? 100000ULL : 100ULL); i++)
                __asm__ volatile("" ::: "memory");
            return aml_integer(0);
        }
        case EXT_ACQUIRE: {
            struct aml_target t;
            parse_target(c, &t);
            target_release(&t);
            if (c->p + 2 > c->end) break;
            c->p += 2;                      /* the timeout word */
            /* Single-threaded: the mutex is always acquired. Zero means
             * success, which is the inverse of every other predicate in AML. */
            return aml_integer(0);
        }
        case EXT_RELEASE: case EXT_SIGNAL: case EXT_RESET: {
            struct aml_target t;
            parse_target(c, &t);
            target_release(&t);
            return aml_integer(0);
        }
        case EXT_WAIT: {
            struct aml_target t;
            parse_target(c, &t);
            target_release(&t);
            struct aml_object *a = term_arg(c);
            aml_put(a);
            return aml_integer(0);
        }
        case EXT_COND_REF_OF: {
            struct aml_path path;
            struct aml_node *n = NULL;
            if (parse_namestring(c, &path)) n = ns_resolve(c->scope, &path, false);
            struct aml_target t;
            if (parse_target(c, &t)) {
                if (n) {
                    struct aml_object *r = obj_new(AML_REF);
                    if (r) { r->u.ref.node = n; r->u.ref.index = -1; }
                    store_into(&t, r, c);
                    aml_put(r);
                }
                target_release(&t);
            }
            return aml_integer(n ? ~0ULL : 0);
        }
        case EXT_FATAL: {
            if (c->p + 5 > c->end) break;
            c->p += 5;
            struct aml_object *a = term_arg(c);
            aml_put(a);
            kprintf("AML: firmware executed Fatal()\n");
            return aml_integer(0);
        }
        case EXT_TO_BCD: case EXT_FROM_BCD: {
            struct aml_object *a = term_arg(c);
            uint64_t v = obj_as_integer(a), r = 0;
            aml_put(a);
            if (x == EXT_FROM_BCD) {
                uint64_t m = 1;
                while (v) { r += (v & 0xF) * m; m *= 10; v >>= 4; }
            } else {
                int sh = 0;
                while (v && sh < 64) { r |= (v % 10) << sh; v /= 10; sh += 4; }
            }
            struct aml_target t;
            if (parse_target(c, &t)) {
                struct aml_object *o = aml_integer(r);
                store_into(&t, o, c); target_release(&t); aml_put(o);
            }
            return aml_integer(r);
        }
        default:
            break;
        }
        c->failed = true;
        return NULL;
    }

    default:
        break;
    }

    /* Locals, Args, and a bare name: a method call or a value. */
    if (op >= OP_LOCAL0 && op < OP_LOCAL0 + 8)
        return aml_get(c->local[op - OP_LOCAL0]);
    if (op >= OP_ARG0 && op < OP_ARG0 + 7)
        return aml_get(c->arg[op - OP_ARG0]);

    if (op == OP_ROOT_CHAR || op == OP_PARENT_CHAR || op == OP_DUAL_NAME ||
        op == OP_MULTI_NAME || op == '_' || (op >= 'A' && op <= 'Z')) {
        c->p--;
        struct aml_path path;
        if (!parse_namestring(c, &path)) { c->failed = true; return NULL; }
        struct aml_node *n = ns_resolve(c->scope, &path, false);
        if (!n) {
            /* A name that is not there. Firmware does this for hardware that
             * is absent; the honest value is zero, not a failed method. */
            return aml_integer(0);
        }
        if (n->value && n->value->kind == AML_METHOD) {
            int argc = n->value->u.method.argc;
            struct aml_object *args[7] = { 0 };
            for (int i = 0; i < argc; i++) args[i] = term_arg(c);
            struct aml_object *r = invoke(n, args, argc, c);
            for (int i = 0; i < argc; i++) aml_put(args[i]);
            return r;
        }
        return node_value(n, c);
    }

    c->failed = true;
    return NULL;
}

/* Run terms until `end`, stopping early on Return/Break/Continue. */
static bool termlist_exec(struct aml_ctx *c, const uint8_t *end) {
    const uint8_t *saved = c->end;
    c->end = end;
    while (c->p < end && !c->failed && c->flow == FLOW_NORMAL) {
        struct aml_object *v = term_arg(c);
        aml_put(v);
    }
    c->end = saved;
    return !c->failed;
}

/* ---- predefined methods the firmware expects US to provide ---------------
 *
 * _OSI is the one that matters: firmware calls it to ask what the OS is, and
 * gates entire blocks of the DSDT on the answer -- including, on many
 * machines, whether the battery and thermal methods are the modern ones or a
 * compatibility path kept for an OS from 2001.
 *
 * WE CLAIM THE WINDOWS STRINGS, and that deserves a reason rather than a
 * shrug. _OSI is not "which OS is this"; it is "does the OS implement the
 * behaviour that this string names", and the behaviours are ACPI features.
 * Hardware vendors test the Windows paths and nothing else, so those are the
 * paths that work; every non-Windows OS answers this way for the same reason.
 * Answering "no" to all of them does not get a neutral DSDT -- it gets the
 * oldest and least maintained branch in it. */
static const char *const g_osi_yes[] = {
    "Windows 2000", "Windows 2001", "Windows 2001 SP1", "Windows 2001 SP2",
    "Windows 2001.1", "Windows 2006", "Windows 2006 SP1", "Windows 2006.1",
    "Windows 2009", "Windows 2012", "Windows 2013", "Windows 2015",
    "Windows 2016", "Windows 2017", "Windows 2018", "Windows 2019",
    "Windows 2020", "Windows 2021", "Windows 2022",
};

static struct aml_object *native_osi(struct aml_object **args, int argc) {
    if (argc < 1 || !args[0] || args[0]->kind != AML_STRING) return aml_integer(0);
    const char *s = (const char *)args[0]->u.buf.data;
    for (unsigned i = 0; i < sizeof g_osi_yes / sizeof g_osi_yes[0]; i++)
        if (strcmp(s, g_osi_yes[i]) == 0) return aml_integer(~0ULL);
    return aml_integer(0);
}

/* ---- invoking a method -------------------------------------------------- */
static struct aml_object *invoke(struct aml_node *n, struct aml_object **args,
                                 int argc, struct aml_ctx *outer) {
    struct aml_object *m = n->value;
    if (!m || m->kind != AML_METHOD) return node_value(n, outer);

    if (m->u.method.body == NULL)            /* a method we implement */
        return native_osi(args, argc);

    if (outer && outer->depth >= AML_MAX_DEPTH) {
        char path[160];
        node_path(n, path, sizeof path);
        kprintf("AML: %s exceeded the call depth limit -- refusing\n", path);
        return NULL;
    }

    static uint32_t steps_root;
    struct aml_ctx c;
    memset(&c, 0, sizeof c);
    c.p     = m->u.method.body;
    c.end   = m->u.method.body + m->u.method.len;
    c.scope = n;
    c.depth = outer ? outer->depth + 1 : 1;
    if (outer) { c.steps = outer->steps; }
    else       { steps_root = 0; c.steps = &steps_root; }
    for (int i = 0; i < argc && i < 7; i++) c.arg[i] = aml_get(args[i]);

    termlist_exec(&c, c.end);

    struct aml_object *ret = c.ret;
    if (c.failed) {
        char path[160];
        node_path(n, path, sizeof path);
        kprintf("AML: %s failed at byte %u of %u -- no value\n", path,
                (unsigned)(c.p - m->u.method.body), (unsigned)m->u.method.len);
        aml_put(ret);
        ret = NULL;
    }
    for (int i = 0; i < 8; i++) aml_put(c.local[i]);
    for (int i = 0; i < 7; i++) aml_put(c.arg[i]);
    return ret;
}

struct aml_object *aml_evaluate(struct aml_node *node,
                                struct aml_object **args, int argc) {
    if (!node || !g_ready) return NULL;
    if (node->value && node->value->kind == AML_METHOD)
        return invoke(node, args, argc, NULL);
    return node_value(node, NULL);
}

bool aml_eval_integer(struct aml_node *from, const char *path, uint64_t *out) {
    struct aml_node *n = aml_lookup(from, path);
    if (!n) return false;
    struct aml_object *v = aml_evaluate(n, NULL, 0);
    if (!v) return false;
    bool ok = (v->kind == AML_INTEGER);
    if (ok) *out = v->u.integer;
    aml_put(v);
    return ok;
}

/* ---- the load pass ------------------------------------------------------ */

/* Field declarations: a list of (name, bit width) with holes and offsets. The
 * running bit position is the part to get right -- a reserved entry advances
 * it without creating a name, and an Offset restarts it at an absolute BYTE. */
static void load_fieldlist(struct aml_ctx *c, const uint8_t *end,
                           struct aml_node *region, uint8_t flags) {
    uint32_t bit = 0;
    uint8_t access = flags & 0x0F;
    uint8_t lock   = (flags >> 4) & 1;
    uint8_t update = (flags >> 5) & 3;

    while (c->p < end) {
        uint8_t lead = *c->p;
        if (lead == 0x00) {                    /* ReservedField: a hole */
            c->p++;
            uint32_t len;
            if (!pkg_length(c, &len)) return;
            bit += len;
            continue;
        }
        if (lead == 0x01) {                    /* AccessField: changes access */
            c->p++;
            if (c->p + 2 > end) return;
            access = *c->p++ & 0x0F;
            c->p++;                            /* AccessAttrib */
            continue;
        }
        if (lead == 0x02) {                    /* ConnectField */
            c->p++;
            struct aml_path dummy;
            if (!parse_namestring(c, &dummy)) return;
            continue;
        }
        if (lead == 0x03) {                    /* ExtendedAccessField */
            c->p++;
            if (c->p + 3 > end) return;
            access = *c->p++ & 0x0F;
            c->p += 2;
            continue;
        }
        if (c->p + AML_NAME_LEN > end) return;
        char seg[AML_NAME_LEN];
        memcpy(seg, c->p, AML_NAME_LEN);
        c->p += AML_NAME_LEN;
        uint32_t len;
        if (!pkg_length(c, &len)) return;

        struct aml_node *n = node_create(c->scope, seg);
        struct aml_object *f = obj_new(AML_FIELD);
        if (n && f) {
            f->u.field.region     = region;
            f->u.field.bit_offset = bit;
            f->u.field.bit_width  = len;
            f->u.field.access     = access;
            f->u.field.lock       = lock;
            f->u.field.update     = update;
            node_set(n, f);
            g_stats.fields++;
        } else {
            aml_put(f);
        }
        bit += len;
    }
}

static bool load_term(struct aml_ctx *c);

static bool load_termlist(const uint8_t *p, const uint8_t *end,
                          struct aml_node *scope) {
    struct aml_ctx c;
    memset(&c, 0, sizeof c);
    uint32_t steps = 0;
    c.p = p; c.end = end; c.scope = scope; c.steps = &steps;
    while (c.p < end)
        if (!load_term(&c)) return false;
    return true;
}

/* One declaration. Returns false only when the stream can no longer be
 * followed -- at which point the REST OF THE TABLE is lost, so everything that
 * can be skipped is skipped instead. */
static bool load_term(struct aml_ctx *c) {
    if (c->p >= c->end) return true;
    const uint8_t *op_at = c->p;
    uint8_t op = *c->p++;

    switch (op) {
    case OP_NAME: {
        struct aml_path path;
        if (!parse_namestring(c, &path)) return false;
        struct aml_node *n = ns_resolve(c->scope, &path, true);
        struct aml_object *v = term_arg(c);
        if (c->failed) return false;
        node_set(n, v);
        return true;
    }
    case OP_SCOPE: {
        const uint8_t *start = c->p;
        const uint8_t *e = pkg_end(c, start);
        if (!e) return false;
        struct aml_path path;
        if (!parse_namestring(c, &path)) return false;
        struct aml_node *n = ns_resolve(c->scope, &path, true);
        if (!n) { c->p = e; return true; }
        if (!load_termlist(c->p, e, n)) { c->p = e; return true; }
        c->p = e;
        return true;
    }
    case OP_METHOD: {
        const uint8_t *start = c->p;
        const uint8_t *e = pkg_end(c, start);
        if (!e) return false;
        struct aml_path path;
        if (!parse_namestring(c, &path)) return false;
        if (c->p >= e) return false;
        uint8_t flags = *c->p++;
        struct aml_node *n = ns_resolve(c->scope, &path, true);
        struct aml_object *m = obj_new(AML_METHOD);
        if (n && m) {
            /* THE BODY IS NOT PARSED. It is a span of the table, which stays
             * mapped for the life of the machine, and it is walked only if
             * something calls this method. */
            m->u.method.body = c->p;
            m->u.method.len  = (uint32_t)(e - c->p);
            m->u.method.argc = flags & 0x07;
            m->u.method.serialized = (flags >> 3) & 1;
            node_set(n, m);
            g_stats.methods++;
        } else {
            aml_put(m);
        }
        c->p = e;
        return true;
    }
    case OP_ALIAS: {
        struct aml_path src, dst;
        if (!parse_namestring(c, &src)) return false;
        if (!parse_namestring(c, &dst)) return false;
        struct aml_node *s = ns_resolve(c->scope, &src, false);
        struct aml_node *d = ns_resolve(c->scope, &dst, true);
        if (s && d) node_set(d, aml_get(s->value));
        return true;
    }
    case OP_EXTERNAL: {
        struct aml_path path;
        if (!parse_namestring(c, &path)) return false;
        if (c->p + 2 > c->end) return false;
        c->p += 2;                     /* object type, argument count */
        return true;
    }
    case OP_IF: {
        /* A CONDITIONAL DECLARATION. Firmware wraps whole blocks of names in
         * `If (CondRefOf(...))` or a check on _OSI, so skipping the block
         * would lose real devices -- but EXECUTING a predicate this early can
         * touch hardware that is not up. The compromise: evaluate it, and if
         * evaluation fails, load the block anyway rather than lose it. */
        const uint8_t *start = c->p;
        const uint8_t *e = pkg_end(c, start);
        if (!e) return false;
        struct aml_object *pred = term_arg(c);
        bool taken = pred ? (obj_as_integer(pred) != 0) : true;
        aml_put(pred);
        c->failed = false;
        if (taken) load_termlist(c->p, e, c->scope);
        c->p = e;
        if (c->p < c->end && *c->p == OP_ELSE) {
            c->p++;
            const uint8_t *es = c->p;
            const uint8_t *ee = pkg_end(c, es);
            if (!ee) return false;
            if (!taken) load_termlist(c->p, ee, c->scope);
            c->p = ee;
        }
        return true;
    }
    case OP_ELSE: case OP_WHILE: {
        const uint8_t *start = c->p;
        const uint8_t *e = pkg_end(c, start);
        if (!e) return false;
        c->p = e;
        return true;
    }
    case OP_EXT: {
        if (c->p >= c->end) return false;
        uint8_t x = *c->p++;
        switch (x) {
        case EXT_OP_REGION: {
            struct aml_path path;
            if (!parse_namestring(c, &path)) return false;
            if (c->p >= c->end) return false;
            uint8_t space = *c->p++;
            struct aml_object *off = term_arg(c);
            struct aml_object *len = term_arg(c);
            if (c->failed) { aml_put(off); aml_put(len); return false; }
            struct aml_node *n = ns_resolve(c->scope, &path, true);
            struct aml_object *r = obj_new(AML_REGION);
            if (n && r) {
                r->u.region.space  = space;
                r->u.region.offset = obj_as_integer(off);
                r->u.region.length = obj_as_integer(len);
                r->u.region.owner  = n;
                node_set(n, r);
                g_stats.regions++;
            } else {
                aml_put(r);
            }
            aml_put(off); aml_put(len);
            return true;
        }
        case EXT_FIELD: {
            const uint8_t *start = c->p;
            const uint8_t *e = pkg_end(c, start);
            if (!e) return false;
            struct aml_path path;
            if (!parse_namestring(c, &path)) return false;
            if (c->p >= e) return false;
            uint8_t flags = *c->p++;
            struct aml_node *region = ns_resolve(c->scope, &path, false);
            if (region) load_fieldlist(c, e, region, flags);
            c->p = e;
            return true;
        }
        case EXT_INDEX_FIELD: case EXT_BANK_FIELD: {
            /* Indexed and banked fields reach their bits through ANOTHER
             * field rather than directly. Nothing here consumes one, and a
             * half-implemented one would read the wrong register -- so the
             * names are skipped and counted. */
            const uint8_t *start = c->p;
            const uint8_t *e = pkg_end(c, start);
            if (!e) return false;
            c->p = e;
            g_stats.load_errors++;
            return true;
        }
        case EXT_DEVICE: case EXT_POWER_RES: case EXT_THERMAL_ZN:
        case EXT_PROCESSOR: {
            const uint8_t *start = c->p;
            const uint8_t *e = pkg_end(c, start);
            if (!e) return false;
            struct aml_path path;
            if (!parse_namestring(c, &path)) return false;
            /* PowerResource and Processor carry fixed header bytes between the
             * name and the body; Device and ThermalZone do not. */
            if (x == EXT_POWER_RES) { if (c->p + 3 > e) return false; c->p += 3; }
            if (x == EXT_PROCESSOR) { if (c->p + 6 > e) return false; c->p += 6; }
            struct aml_node *n = ns_resolve(c->scope, &path, true);
            if (n) {
                enum aml_kind k = (x == EXT_DEVICE)     ? AML_DEVICE
                                : (x == EXT_POWER_RES)  ? AML_POWER_RES
                                : (x == EXT_PROCESSOR)  ? AML_PROCESSOR
                                                        : AML_THERMAL_ZONE;
                if (!n->value) node_set(n, obj_new(k));
                if (x == EXT_DEVICE) g_stats.devices++;
                load_termlist(c->p, e, n);
            }
            c->p = e;
            return true;
        }
        case EXT_MUTEX: {
            struct aml_path path;
            if (!parse_namestring(c, &path)) return false;
            if (c->p >= c->end) return false;
            uint8_t sync = *c->p++;
            struct aml_node *n = ns_resolve(c->scope, &path, true);
            struct aml_object *m = obj_new(AML_MUTEX);
            if (n && m) { m->u.mutex.sync_level = sync; node_set(n, m); }
            else aml_put(m);
            return true;
        }
        case EXT_EVENT: {
            struct aml_path path;
            if (!parse_namestring(c, &path)) return false;
            struct aml_node *n = ns_resolve(c->scope, &path, true);
            if (n) node_set(n, obj_new(AML_EVENT));
            return true;
        }
        default:
            kprintf("AML: unknown extended opcode 0x5B 0x%02x at +%u -- "
                    "stopping this table\n", x, (unsigned)(op_at - c->p));
            g_stats.load_errors++;
            return false;
        }
    }
    default:
        /* An executable term at declaration level. There is no way to know how
         * long it is without a full grammar, so following the stream past it
         * is guesswork -- and guessing here silently invents names. Stop. */
        kprintf("AML: opcode 0x%02x is not a declaration -- stopping this "
                "table with %u names loaded\n", op, (unsigned)g_stats.nodes);
        g_stats.load_errors++;
        return false;
    }
}

/* ---- bringing it up ----------------------------------------------------- */
static void predefine(void) {
    struct aml_node *n;

    n = node_create(&g_root, "_OSI");
    if (n) {
        struct aml_object *m = obj_new(AML_METHOD);
        if (m) { m->u.method.body = NULL; m->u.method.argc = 1; node_set(n, m); }
    }
    n = node_create(&g_root, "_OS_");
    if (n) node_set(n, obj_string("EmbLinkOS", 9));
    n = node_create(&g_root, "_REV");
    if (n) node_set(n, aml_integer(2));
    /* The scopes every DSDT assumes exist. */
    node_create(&g_root, "_SB_");
    node_create(&g_root, "_SI_");
    node_create(&g_root, "_GPE");
    node_create(&g_root, "_PR_");
    node_create(&g_root, "_TZ_");
}

static bool load_table(const struct acpi_sdt_header *t) {
    const uint8_t *aml = (const uint8_t *)t + sizeof *t;
    const uint8_t *end = (const uint8_t *)t + t->length;
    if (end <= aml) return false;
    g_stats.tables++;
    return load_termlist(aml, end, &g_root);
}

bool aml_init(void) {
    if (g_ready) return true;
    memset(&g_stats, 0, sizeof g_stats);
    predefine();

    const struct acpi_sdt_header *dsdt = acpi_dsdt();
    if (!dsdt) {
        kprintf("AML: no DSDT -- the namespace stays empty\n");
        return false;
    }
    if (!load_table(dsdt))
        kprintf("AML: the DSDT stopped short; keeping what loaded\n");

    for (int i = 0; ; i++) {
        const struct acpi_sdt_header *s = acpi_ssdt(i);
        if (!s) break;
        if (!load_table(s))
            kprintf("AML: SSDT %d stopped short; keeping what loaded\n", i);
    }

    g_ready = true;
    kprintf("AML: %u table(s), %u names -- %u methods, %u devices, "
            "%u regions, %u fields%s\n",
            g_stats.tables, g_stats.nodes, g_stats.methods, g_stats.devices,
            g_stats.regions, g_stats.fields,
            g_stats.load_errors ? " (with refusals -- see above)" : "");
    return true;
}

bool aml_available(void) { return g_ready; }
const struct aml_stats *aml_get_stats(void) { return &g_stats; }

void aml_walk(struct aml_node *from, bool (*fn)(struct aml_node *, void *),
              void *ctx) {
    if (!from) from = &g_root;
    if (!fn(from, ctx)) return;
    for (struct aml_node *n = from->child; n; n = n->sibling)
        aml_walk(n, fn, ctx);
}

struct hid_ctx {
    const char *hid;
    bool (*fn)(struct aml_node *, void *);
    void *ctx;
};

static bool obj_is_hid(struct aml_object *v, const char *hid) {
    if (!v) return false;
    if (v->kind == AML_STRING)
        return strcmp((const char *)v->u.buf.data, hid) == 0;
    if (v->kind == AML_INTEGER) {
        /* An EISA id packed into 32 bits: three 5-bit letters then four hex
         * digits. `PNP0C0A` is 0x0A0CD041 in a DSDT, and a battery is not
         * found at all by anyone who compares the text to the number. */
        uint32_t id = (uint32_t)v->u.integer;
        char s[8];
        s[0] = (char)(0x40 + ((id >> 2) & 0x1F));
        s[1] = (char)(0x40 + (((id & 0x3) << 3) | ((id >> 13) & 0x7)));
        s[2] = (char)(0x40 + ((id >> 8) & 0x1F));
        static const char hx[] = "0123456789ABCDEF";
        s[3] = hx[(id >> 20) & 0xF];
        s[4] = hx[(id >> 16) & 0xF];
        s[5] = hx[(id >> 28) & 0xF];
        s[6] = hx[(id >> 24) & 0xF];
        s[7] = 0;
        return strcmp(s, hid) == 0;
    }
    if (v->kind == AML_PACKAGE) {
        for (uint32_t i = 0; i < v->u.pkg.count; i++)
            if (obj_is_hid(v->u.pkg.elem[i], hid)) return true;
    }
    return false;
}

static bool hid_visit(struct aml_node *n, void *vctx) {
    struct hid_ctx *h = vctx;
    if (n->value && n->value->kind == AML_DEVICE) {
        for (int which = 0; which < 2; which++) {
            struct aml_node *id = node_child(n, which ? "_CID" : "_HID");
            if (!id) continue;
            struct aml_object *v = aml_evaluate(id, NULL, 0);
            bool hit = obj_is_hid(v, h->hid);
            aml_put(v);
            if (hit) return h->fn(n, h->ctx);
        }
    }
    return true;
}

void aml_for_each_hid(const char *hid, bool (*fn)(struct aml_node *, void *),
                      void *ctx) {
    struct hid_ctx h = { hid, fn, ctx };
    aml_walk(&g_root, hid_visit, &h);
}

/* ---- seeing what the firmware declared ---------------------------------- */
static const char *kind_name(enum aml_kind k) {
    switch (k) {
    case AML_INTEGER: return "Integer";
    case AML_STRING:  return "String";
    case AML_BUFFER:  return "Buffer";
    case AML_PACKAGE: return "Package";
    case AML_DEVICE:  return "Device";
    case AML_METHOD:  return "Method";
    case AML_REGION:  return "OperationRegion";
    case AML_FIELD:   return "Field";
    case AML_BUFFER_FIELD: return "BufferField";
    case AML_MUTEX:   return "Mutex";
    case AML_EVENT:   return "Event";
    case AML_POWER_RES: return "PowerResource";
    case AML_PROCESSOR: return "Processor";
    case AML_THERMAL_ZONE: return "ThermalZone";
    case AML_UNRESOLVED: return "UnresolvedName";
    default: return "Scope";
    }
}

static bool dump_visit(struct aml_node *n, void *ctx) {
    (void)ctx;
    if (n == &g_root) return true;
    char path[160];
    node_path(n, path, sizeof path);
    struct aml_object *v = n->value;
    if (!v) { kprintf("  %-40s Scope\n", path); return true; }
    switch (v->kind) {
    case AML_INTEGER:
        kprintf("  %-40s Integer 0x%llx\n", path,
                (unsigned long long)v->u.integer);
        break;
    case AML_STRING:
        kprintf("  %-40s String \"%s\"\n", path, (const char *)v->u.buf.data);
        break;
    case AML_METHOD:
        kprintf("  %-40s Method (%u args, %u bytes)\n", path,
                v->u.method.argc, v->u.method.len);
        break;
    case AML_REGION:
        kprintf("  %-40s OperationRegion space %u at 0x%llx len 0x%llx\n", path,
                v->u.region.space, (unsigned long long)v->u.region.offset,
                (unsigned long long)v->u.region.length);
        break;
    case AML_FIELD:
        kprintf("  %-40s Field bit %u width %u\n", path,
                v->u.field.bit_offset, v->u.field.bit_width);
        break;
    case AML_PACKAGE:
        kprintf("  %-40s Package (%u)\n", path, v->u.pkg.count);
        break;
    default:
        kprintf("  %-40s %s\n", path, kind_name(v->kind));
        break;
    }
    return true;
}

void aml_dump(void) {
    kprintf("=== ACPI namespace ===\n");
    aml_walk(&g_root, dump_visit, NULL);
}
