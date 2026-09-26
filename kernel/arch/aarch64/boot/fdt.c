#include "arch/aarch64/boot/fdt.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "mm/pmm.h"      /* P2V -- the blob is read through the direct map */

/* Devicetree Specification v0.4 §5. See fdt.h for what this is and is not. */

#define FDT_MAGIC          0xd00dfeedu
#define FDT_COMPAT_VERSION 16          /* we understand v16 and later         */

#define FDT_BEGIN_NODE 0x1u
#define FDT_END_NODE   0x2u
#define FDT_PROP       0x3u
#define FDT_NOP        0x4u
#define FDT_END        0x9u

struct fdt_header {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
};

static const uint8_t *blob;        /* virtual, via the direct map            */
static uint64_t       blob_phys;
static uint32_t       blob_size;
static uint32_t       struct_off, struct_size;
static uint32_t       strings_off, strings_size;
static uint32_t       rsvmap_off;

/* Every integer in the blob is big-endian. Reading them byte-wise rather than
 * with a byte-swap intrinsic also means no alignment assumption -- properties
 * are only 4-byte aligned, and a `reg` with 64-bit cells is routinely read at
 * an offset that is not 8-aligned. On a Device mapping that would fault; even
 * on Normal memory it is undefined in C. */
static uint32_t be32_at(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}
static uint64_t be64_at(const uint8_t *p) {
    return ((uint64_t)be32_at(p) << 32) | be32_at(p + 4);
}

/* Bounds. Every read goes through one of these; the blob is firmware input. */
static bool in_struct(uint32_t off, uint32_t len) {
    return blob && off >= struct_off && len <= struct_size &&
           off <= struct_off + struct_size - len;
}
static const char *string_at(uint32_t off) {
    if (!blob || off >= strings_size)
        return 0;
    /* The strings block must be NUL-terminated within itself. Walk rather than
     * trust: an unterminated final string would otherwise run into the rest of
     * the blob and out the far side of it. */
    const char *s = (const char *)(blob + strings_off + off);
    for (uint32_t i = off; i < strings_size; i++)
        if (blob[strings_off + i] == 0)
            return s;
    return 0;
}

static uint32_t align4(uint32_t v) { return (v + 3u) & ~3u; }

int fdt_init(uint64_t phys) {
    blob = 0;

    if (!phys || (phys & 7)) {
        kprintf("fdt: bad blob address %p (must be non-zero and 8-byte aligned)\n",
                (void *)(uintptr_t)phys);
        return -1;
    }

    const uint8_t *p = (const uint8_t *)(uintptr_t)P2V(phys);
    const struct fdt_header *h = (const struct fdt_header *)p;

    if (be32_at((const uint8_t *)&h->magic) != FDT_MAGIC) {
        kprintf("fdt: bad magic %x at %p\n",
                be32_at((const uint8_t *)&h->magic), (void *)(uintptr_t)phys);
        return -1;
    }

    uint32_t total = be32_at((const uint8_t *)&h->totalsize);
    uint32_t ver   = be32_at((const uint8_t *)&h->version);
    uint32_t comp  = be32_at((const uint8_t *)&h->last_comp_version);

    if (total < sizeof(struct fdt_header) || total > (64u << 20)) {
        kprintf("fdt: implausible totalsize %d\n", (int)total);
        return -1;
    }
    /* last_comp_version is the OLDEST reader that can still parse this blob.
     * If it demands something newer than we implement, refuse rather than
     * misread -- a misread device tree is a wrong memory map. */
    if (comp > FDT_COMPAT_VERSION) {
        kprintf("fdt: blob requires reader v%d, we are v%d\n",
                (int)comp, FDT_COMPAT_VERSION);
        return -1;
    }

    uint32_t so = be32_at((const uint8_t *)&h->off_dt_struct);
    uint32_t ss = be32_at((const uint8_t *)&h->size_dt_struct);
    uint32_t to = be32_at((const uint8_t *)&h->off_dt_strings);
    uint32_t ts = be32_at((const uint8_t *)&h->size_dt_strings);
    uint32_t ro = be32_at((const uint8_t *)&h->off_mem_rsvmap);

    /* Both blocks must lie wholly inside the blob. Checked as
     * `off > total - size` rather than `off + size > total` so an overflowing
     * offset cannot wrap past the comparison. */
    if (ss > total || so > total - ss || ts > total || to > total - ts ||
        ro > total - 16) {
        kprintf("fdt: header offsets escape the blob (v%d, total %d)\n",
                (int)ver, (int)total);
        return -1;
    }

    blob         = p;
    blob_phys    = phys;
    blob_size    = total;
    struct_off   = so;
    struct_size  = ss;
    strings_off  = to;
    strings_size = ts;
    rsvmap_off   = ro;
    return 0;
}

bool     fdt_ok(void)         { return blob != 0; }
uint64_t fdt_phys_base(void)  { return blob_phys; }
uint32_t fdt_total_size(void) { return blob_size; }

/* --- token walking --------------------------------------------------------
 * A node handle is the offset of its FDT_BEGIN_NODE token. Everything else is
 * derived by walking forward from there, which is why no state is cached: the
 * blob is small, the walks are short, and a cache would be a second source of
 * truth about firmware data we do not control. */

/* Offset of the token following the one at `off`, or 0 on malformed input. */
static uint32_t token_next(uint32_t off, uint32_t *out_token) {
    if (!in_struct(off, 4))
        return 0;

    uint32_t tok = be32_at(blob + off);
    if (out_token)
        *out_token = tok;

    switch (tok) {
    case FDT_BEGIN_NODE: {
        /* token + NUL-terminated name, padded to 4 */
        uint32_t n = off + 4;
        while (in_struct(n, 1) && blob[n])
            n++;
        if (!in_struct(n, 1))
            return 0;
        return align4(n + 1);
    }
    case FDT_PROP: {
        if (!in_struct(off, 12))
            return 0;
        uint32_t len = be32_at(blob + off + 4);
        if (len > struct_size)
            return 0;
        uint32_t n = off + 12 + len;
        if (!in_struct(off, 12 + len))
            return 0;
        return align4(n);
    }
    case FDT_END_NODE:
    case FDT_NOP:
        return off + 4;
    case FDT_END:
        return 0;
    default:
        return 0;   /* unknown token: stop, do not guess a length */
    }
}

fdt_node_t fdt_root(void) {
    if (!blob)
        return FDT_NONE;
    /* Skip any leading NOPs, then expect BEGIN_NODE. */
    uint32_t off = struct_off, tok = 0;
    while (in_struct(off, 4)) {
        tok = be32_at(blob + off);
        if (tok == FDT_NOP) { off += 4; continue; }
        return tok == FDT_BEGIN_NODE ? (fdt_node_t)off : FDT_NONE;
    }
    return FDT_NONE;
}

const char *fdt_node_name(fdt_node_t node) {
    if (!blob || node < 0 || !in_struct((uint32_t)node, 4))
        return "";
    return (const char *)(blob + node + 4);
}

/* Offset just past this node's name -- where its properties begin. */
static uint32_t node_body(fdt_node_t node) {
    uint32_t tok = 0;
    return token_next((uint32_t)node, &tok);
}

fdt_node_t fdt_first_child(fdt_node_t node) {
    if (!blob || node < 0)
        return FDT_NONE;

    uint32_t off = node_body(node), tok = 0;
    while (off) {
        if (!in_struct(off, 4))
            return FDT_NONE;
        tok = be32_at(blob + off);
        if (tok == FDT_BEGIN_NODE)
            return (fdt_node_t)off;
        if (tok == FDT_END_NODE || tok == FDT_END)
            return FDT_NONE;
        off = token_next(off, 0);      /* a property or a NOP: skip it */
    }
    return FDT_NONE;
}

fdt_node_t fdt_next_sibling(fdt_node_t node) {
    if (!blob || node < 0)
        return FDT_NONE;

    /* Skip this node's ENTIRE subtree, then take whatever comes next if it is
     * another node rather than the parent's END_NODE.
     *
     * depth starts at 1, not 0: passing this node's own BEGIN_NODE has already
     * put us inside it. Starting at 0 makes the node's FIRST CHILD look like
     * its next sibling -- which is a tree that walks but is wrong, and shows
     * up much later as "the device tree has no memory node" because the walk
     * descended into /chosen and never came back out. */
    uint32_t off = token_next((uint32_t)node, 0);
    int depth = 1;

    while (off && in_struct(off, 4)) {
        uint32_t tok = be32_at(blob + off);

        if (tok == FDT_END)
            return FDT_NONE;

        if (tok == FDT_BEGIN_NODE) {
            if (depth == 0)
                return (fdt_node_t)off;
            depth++;
        } else if (tok == FDT_END_NODE) {
            depth--;
            if (depth < 0)
                return FDT_NONE;          /* parent closed: no more siblings */
        }
        off = token_next(off, 0);
    }
    return FDT_NONE;
}

const void *fdt_prop(fdt_node_t node, const char *name, uint32_t *len) {
    if (!blob || node < 0 || !name)
        return 0;

    uint32_t off = node_body(node), tok = 0;
    while (off && in_struct(off, 4)) {
        tok = be32_at(blob + off);
        if (tok == FDT_BEGIN_NODE || tok == FDT_END_NODE || tok == FDT_END)
            return 0;
        if (tok == FDT_PROP) {
            if (!in_struct(off, 12))
                return 0;
            uint32_t plen = be32_at(blob + off + 4);
            uint32_t noff = be32_at(blob + off + 8);
            const char *pname = string_at(noff);
            if (pname && strcmp(pname, name) == 0) {
                if (!in_struct(off, 12 + plen))
                    return 0;
                if (len)
                    *len = plen;
                return blob + off + 12;
            }
        }
        off = token_next(off, 0);
    }
    return 0;
}

uint32_t fdt_prop_u32(fdt_node_t node, const char *name, uint32_t dflt) {
    uint32_t len = 0;
    const void *p = fdt_prop(node, name, &len);
    return (p && len >= 4) ? be32_at((const uint8_t *)p) : dflt;
}

bool fdt_prop_has_string(fdt_node_t node, const char *name, const char *value) {
    uint32_t len = 0;
    const char *p = (const char *)fdt_prop(node, name, &len);
    if (!p || !value)
        return false;

    /* A string-list property is several NUL-terminated strings back to back.
     * The final NUL is guaranteed by len, which fdt_prop already bounds. */
    uint32_t i = 0;
    while (i < len) {
        const char *s = p + i;
        uint32_t remaining = len - i;
        uint32_t n = 0;
        while (n < remaining && s[n])
            n++;
        if (n == remaining)
            return false;                 /* unterminated: malformed, refuse */
        if (strcmp(s, value) == 0)
            return true;
        i += n + 1;
    }
    return false;
}

fdt_node_t fdt_find_compatible(const char *compat) {
    fdt_node_t root = fdt_root();
    if (root == FDT_NONE)
        return FDT_NONE;

    for (fdt_node_t n = fdt_first_child(root); n != FDT_NONE; n = fdt_next_sibling(n)) {
        if (fdt_prop_has_string(n, "compatible", compat))
            return n;
        for (fdt_node_t c = fdt_first_child(n); c != FDT_NONE; c = fdt_next_sibling(c))
            if (fdt_prop_has_string(c, "compatible", compat))
                return c;
    }
    return FDT_NONE;
}

fdt_node_t fdt_find_device_type(const char *type, fdt_node_t after) {
    fdt_node_t root = fdt_root();
    if (root == FDT_NONE)
        return FDT_NONE;

    bool seen = (after == FDT_NONE);
    for (fdt_node_t n = fdt_first_child(root); n != FDT_NONE; n = fdt_next_sibling(n)) {
        if (!seen) {
            if (n == after)
                seen = true;
            continue;
        }
        uint32_t len = 0;
        const char *dt = (const char *)fdt_prop(n, "device_type", &len);
        if (dt && len && strcmp(dt, type) == 0)
            return n;
    }
    return FDT_NONE;
}

/* The parent of `node`. Nodes are offsets into the blob and carry no parent
 * pointer, so walk down from the root -- cheaply, because a subtree occupies a
 * contiguous span: at each level the node can only be inside the LAST child
 * that starts before it, so exactly one child is descended into per level. */
fdt_node_t fdt_parent(fdt_node_t node) {
    fdt_node_t p = fdt_root();
    if (p == FDT_NONE || node == FDT_NONE || node == p)
        return FDT_NONE;

    for (int depth = 0; depth < 32; depth++) {
        fdt_node_t inside = FDT_NONE;
        for (fdt_node_t c = fdt_first_child(p); c != FDT_NONE; c = fdt_next_sibling(c)) {
            if (c == node)
                return p;
            if (c > node)
                break;
            inside = c;
        }
        if (inside == FDT_NONE)
            return FDT_NONE;
        p = inside;
    }
    return FDT_NONE;
}

/* A bus's own #address-cells / #size-cells: what its CHILDREN use. Spec
 * defaults if absent: 2 and 1. */
static void bus_cells(fdt_node_t bus, uint32_t *a, uint32_t *s) {
    *a = bus == FDT_NONE ? 2 : fdt_prop_u32(bus, "#address-cells", 2);
    *s = bus == FDT_NONE ? 1 : fdt_prop_u32(bus, "#size-cells", 1);
}

void fdt_reg_cells(fdt_node_t node, uint32_t *addr_cells, uint32_t *size_cells) {
    /* #address-cells and #size-cells for a node's `reg` are declared on its
     * PARENT -- not on the root. Until the Raspberry Pi this used the root's
     * unconditionally, which was right only because every node read on `virt`
     * is the root's child (or the ITS, whose parent happens to agree). A Pi
     * puts its peripherals under /soc with ONE address cell, and the root's two
     * would have read each `reg` as garbage. docs/RPI4.md P1. */
    fdt_node_t parent = fdt_parent(node);
    uint32_t a, s;
    bus_cells(parent == FDT_NONE ? fdt_root() : parent, &a, &s);

    /* A cell count above 2 cannot fit in uint64_t; clamp and say so rather
     * than silently truncating an address. */
    if (a > 2 || s > 2) {
        kprintf("fdt: #address-cells=%d #size-cells=%d exceeds 64 bits; clamping\n",
                (int)a, (int)s);
        if (a > 2) a = 2;
        if (s > 2) s = 2;
    }
    if (addr_cells) *addr_cells = a;
    if (size_cells) *size_cells = s;
}

static uint64_t read_cells(const uint8_t *p, uint32_t cells) {
    return cells == 2 ? be64_at(p) : (uint64_t)be32_at(p);
}

/* Carry an address in `bus`'s child space up to a CPU physical address, one
 * `ranges` at a time, until the root. `virt` never needed this -- its devices
 * are the root's children, so the loop does not run -- but a Pi's /soc maps
 * bus address 0x7e201000 to CPU 0xfe201000, and a driver that skipped this
 * would program the VideoCore's view of the world into the ARM's MMU.
 *
 * `ranges;` (empty) is the identity. NO `ranges` means the bus is not mapped
 * into the CPU's address space at all, and the address has no CPU meaning:
 * that is a failure, not an identity. */
static bool translate_to_cpu(fdt_node_t bus, uint64_t *addr) {
    fdt_node_t root = fdt_root();
    while (bus != FDT_NONE && bus != root) {
        uint32_t len = 0;
        const uint8_t *r = (const uint8_t *)fdt_prop(bus, "ranges", &len);
        if (!r)
            return false;

        fdt_node_t up = fdt_parent(bus);
        if (len == 0) {
            bus = up;
            continue;
        }

        uint32_t ca, cs, pa, ps;
        bus_cells(bus, &ca, &cs);
        bus_cells(up == FDT_NONE ? root : up, &pa, &ps);
        if (ca > 2 || cs > 2 || pa > 2)
            return false;       /* a PCI-style bus: its ranges are not MMIO reg */

        uint32_t stride = (ca + pa + cs) * 4;
        bool found = false;
        for (uint32_t off = 0; off + stride <= len; off += stride) {
            uint64_t child  = read_cells(r + off, ca);
            uint64_t parent = read_cells(r + off + ca * 4, pa);
            uint64_t size   = read_cells(r + off + (ca + pa) * 4, cs);
            if (*addr >= child && *addr - child < size) {
                *addr = parent + (*addr - child);
                found = true;
                break;
            }
        }
        if (!found)
            return false;
        bus = up;
    }
    return true;
}

bool fdt_reg(fdt_node_t node, uint32_t index, uint64_t *addr, uint64_t *size) {
    uint32_t ac = 2, sc = 1, len = 0;
    fdt_reg_cells(node, &ac, &sc);

    const uint8_t *p = (const uint8_t *)fdt_prop(node, "reg", &len);
    if (!p)
        return false;

    uint32_t stride = (ac + sc) * 4;
    if (!stride || (uint64_t)(index + 1) * stride > len)
        return false;

    const uint8_t *e = p + (uint64_t)index * stride;
    uint64_t a = read_cells(e, ac);
    if (!translate_to_cpu(fdt_parent(node), &a))
        return false;
    if (addr) *addr = a;
    if (size) *size = read_cells(e + ac * 4, sc);
    return true;
}

bool fdt_dma_translate(fdt_node_t node, bool cpu_to_bus, uint64_t in, uint64_t *out) {
    /* One level: the device's parent bus's dma-ranges, entries of
     * (bus address [bus #address-cells], CPU address [its parent's
     * #address-cells], size [bus #size-cells]). Enough for the Pi, whose /soc
     * maps VideoCore bus 0xC0000000 onto ARM 0 for 1 GiB; a machine that
     * nests DMA translations would need the walk translate_to_cpu() does. */
    fdt_node_t bus = fdt_parent(node);
    if (bus == FDT_NONE)
        return false;
    uint32_t len = 0;
    const uint8_t *r = (const uint8_t *)fdt_prop(bus, "dma-ranges", &len);
    if (!r || len == 0)
        return false;

    fdt_node_t up = fdt_parent(bus);
    uint32_t ca, cs, pa, ps;
    bus_cells(bus, &ca, &cs);
    bus_cells(up == FDT_NONE ? fdt_root() : up, &pa, &ps);
    if (ca > 2 || cs > 2 || pa > 2)
        return false;

    uint32_t stride = (ca + pa + cs) * 4;
    for (uint32_t off = 0; off + stride <= len; off += stride) {
        uint64_t b    = read_cells(r + off, ca);
        uint64_t c    = read_cells(r + off + ca * 4, pa);
        uint64_t size = read_cells(r + off + (ca + pa) * 4, cs);
        uint64_t from = cpu_to_bus ? c : b, to = cpu_to_bus ? b : c;
        if (in >= from && in - from < size) {
            *out = to + (in - from);
            return true;
        }
    }
    return false;
}

fdt_node_t fdt_find_gic(void) {
    /* v3 first, then every GICv2 name a device tree uses -- the same list, in
     * the same order, as irq/gicv2.c probes, so the two cannot disagree about
     * which controller this machine has. */
    static const char *const names[] = {
        "arm,gic-v3", "arm,gic-400", "arm,cortex-a15-gic",
        "arm,cortex-a9-gic", "arm,cortex-a7-gic",
    };
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
        fdt_node_t n = fdt_find_compatible(names[i]);
        if (n != FDT_NONE)
            return n;
    }
    return FDT_NONE;
}

bool fdt_interrupt(fdt_node_t node, uint32_t index,
                   uint32_t *type, uint32_t *num, uint32_t *flags) {
    uint32_t len = 0;
    const uint8_t *p = (const uint8_t *)fdt_prop(node, "interrupts", &len);
    if (!p)
        return false;

    /* #interrupt-cells belongs to the interrupt CONTROLLER, not to this node.
     * Everything on `virt` hangs off the one GIC, so ask it directly rather
     * than following interrupt-parent phandles -- a phandle resolver is a
     * device-tree framework, which docs/ARM64.md §5 rules out. If no GIC is
     * found, 3 is the ARM binding's value and the only one that could apply. */
    uint32_t cells = 3;
    fdt_node_t gic = fdt_find_gic();
    if (gic != FDT_NONE)
        cells = fdt_prop_u32(gic, "#interrupt-cells", 3);

    if (cells < 2 || cells > 4)
        return false;

    uint32_t stride = cells * 4;
    if ((uint64_t)(index + 1) * stride > len)
        return false;

    const uint8_t *e = p + (uint64_t)index * stride;
    if (type)  *type  = be32_at(e);
    if (num)   *num   = be32_at(e + 4);
    if (flags) *flags = cells >= 3 ? be32_at(e + 8) : 0;
    return true;
}

bool fdt_mem_rsv(uint32_t index, uint64_t *addr, uint64_t *size) {
    if (!blob)
        return false;

    /* Sixteen bytes per entry, terminated by an all-zero entry. */
    uint64_t off = (uint64_t)rsvmap_off + (uint64_t)index * 16;
    if (off + 16 > blob_size)
        return false;

    uint64_t a = be64_at(blob + off);
    uint64_t s = be64_at(blob + off + 8);
    if (a == 0 && s == 0)
        return false;

    if (addr) *addr = a;
    if (size) *size = s;
    return true;
}

void fdt_dump_summary(void) {
    if (!blob) {
        kprintf("fdt: not initialised\n");
        return;
    }

    kprintf("fdt: blob at phys %p, %d bytes\n",
            (void *)(uintptr_t)blob_phys, (int)blob_size);

    fdt_node_t root = fdt_root();
    uint32_t ac = 2, sc = 1;
    fdt_reg_cells(root, &ac, &sc);
    kprintf("fdt: root #address-cells=%d #size-cells=%d\n", (int)ac, (int)sc);

    uint64_t a = 0, s = 0;
    for (uint32_t i = 0; fdt_mem_rsv(i, &a, &s); i++)
        kprintf("fdt: firmware reservation %p + %x\n", (void *)(uintptr_t)a, (unsigned)s);

    int n = 0;
    for (fdt_node_t c = fdt_first_child(root); c != FDT_NONE; c = fdt_next_sibling(c))
        n++;
    kprintf("fdt: %d top-level nodes\n", n);
}

void fdt_dump_nodes(void) {
    /* Separate from the summary because it is 49 lines on `virt` and only
     * worth printing when a lookup has already failed -- at which point what
     * the tree ACTUALLY contains is the whole question. */
    if (!blob)
        return;

    kprintf("fdt: top-level nodes:\n");
    for (fdt_node_t c = fdt_first_child(fdt_root()); c != FDT_NONE;
         c = fdt_next_sibling(c)) {
        uint32_t dl = 0;
        const char *dt = (const char *)fdt_prop(c, "device_type", &dl);
        kprintf("fdt:   /%s%s%s\n", fdt_node_name(c),
                dt ? "  device_type=" : "", dt ? dt : "");
    }
}
