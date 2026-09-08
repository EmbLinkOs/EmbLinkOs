#ifndef _AARCH64_FDT_H
#define _AARCH64_FDT_H

#include <stdint.h>
#include "include/types.h"

/* A reader for the flattened device tree -- docs/ARM64.md phase A2.
 *
 * This is the aarch64 answer to what ACPI and the BIOS e820 map do on x86: the
 * firmware's description of the machine. On QEMU `virt` it is not optional --
 * there is no other way to learn where RAM is, and unlike x86 there is no
 * legacy layout to fall back on.
 *
 * DELIBERATELY NOT A DRIVER FRAMEWORK (docs/ARM64.md §5). There is no device
 * binding, no probing, no ordering. It answers questions -- "where is RAM",
 * "what address is the GIC at" -- about the four or five nodes this kernel
 * actually cares about, and the rest of `virt`'s well-known layout stays
 * hardcoded. A general DT framework is a project of its own and buys nothing
 * on one board.
 *
 * Everything here is READ-ONLY and every accessor is bounds-checked against
 * the header's own totalsize. The DTB is firmware-supplied input: it is the
 * first thing this kernel parses that it did not write itself, and a malformed
 * one must produce a diagnosis, not a wild pointer.
 *
 * Format: Devicetree Specification v0.4, §5. All integers in the blob are
 * BIG-ENDIAN regardless of the machine -- that is the format, not the CPU, and
 * is not a counter-example to §5's little-endian-only rule. */

/* An opaque handle to a node: an offset into the structure block. -1 = none. */
typedef int32_t fdt_node_t;
#define FDT_NONE ((fdt_node_t)-1)

/* Point the reader at a blob. `phys` is the physical address firmware handed
 * us in x0; it is read through the direct map, so the MMU must be on.
 * Validates magic, version and totalsize. Returns 0 on success. Every other
 * function in this file returns "not found" until this succeeds. */
int fdt_init(uint64_t phys);

/* True once fdt_init() has succeeded. */
bool fdt_ok(void);

/* Total size of the blob, so callers can reserve it from the allocator. */
uint64_t fdt_phys_base(void);
uint32_t fdt_total_size(void);

/* --- navigation ------------------------------------------------------------
 * Nodes are found, not enumerated into memory: the blob stays the only copy.
 */

/* The root node ("/"). */
fdt_node_t fdt_root(void);

/* First child of `node`, then successive siblings. Iterate:
 *     for (fdt_node_t c = fdt_first_child(n); c != FDT_NONE; c = fdt_next_sibling(c)) */
fdt_node_t fdt_first_child(fdt_node_t node);
fdt_node_t fdt_next_sibling(fdt_node_t node);

/* The node's name as it appears in the tree, e.g. "memory@40000000". Points
 * into the blob; valid as long as the blob is. */
const char *fdt_node_name(fdt_node_t node);

/* Find the first node anywhere in the tree whose `compatible` property
 * contains `compat` as one of its NUL-separated strings. This is how a driver
 * finds its device: "arm,pl011", "arm,gic-v3", "pci-host-ecam-generic". */
fdt_node_t fdt_find_compatible(const char *compat);

/* Find the first node whose `device_type` property equals `type` -- "memory"
 * and "cpu" are the two that matter. */
fdt_node_t fdt_find_device_type(const char *type, fdt_node_t after);

/* --- properties ----------------------------------------------------------- */

/* Raw property data, or NULL. *len receives the byte length if not NULL. */
const void *fdt_prop(fdt_node_t node, const char *name, uint32_t *len);

/* A property that is a single big-endian cell / two cells. Returns `dflt` if
 * the property is missing or the wrong size. */
uint32_t fdt_prop_u32(fdt_node_t node, const char *name, uint32_t dflt);

/* Does string-list property `name` contain the string `value`? */
bool fdt_prop_has_string(fdt_node_t node, const char *name, const char *value);

/* #address-cells / #size-cells governing a node's `reg`, which are declared on
 * its PARENT. Defaults per spec: 2 and 1. Getting this wrong silently halves
 * or doubles every address, so it is never assumed. */
void fdt_reg_cells(fdt_node_t node, uint32_t *addr_cells, uint32_t *size_cells);

/* Decode entry `index` of a `reg` property into address/size. Returns false
 * when the index is past the end. */
bool fdt_reg(fdt_node_t node, uint32_t index, uint64_t *addr, uint64_t *size);

/* --- the memory reservation block ------------------------------------------
 * A list, separate from the tree, of physical ranges firmware says must not be
 * touched. Entry `index`; returns false past the end. */
bool fdt_mem_rsv(uint32_t index, uint64_t *addr, uint64_t *size);

/* Print the blob's shape: where it is, its cell counts, firmware reservations
 * and how many top-level nodes it has. Cheap; printed on every boot. */
void fdt_dump_summary(void);

/* Print every top-level node and its device_type. Fifty lines on `virt`, so
 * this is for when a lookup has already failed and the question is what the
 * tree actually contains. */
void fdt_dump_nodes(void);

#endif /* _AARCH64_FDT_H */
