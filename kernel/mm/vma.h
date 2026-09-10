#ifndef _VMA_H_
#define _VMA_H_

#include <stdint.h>
#include "include/types.h"

/* MEMORY MAPPINGS -- mmap()/munmap() and the per-process record of what is
 * mapped where.
 *
 * WHAT THE KERNEL HAD BEFORE THIS: sbrk(), and only sbrk(). One heap per
 * process that GROWS AND NEVER SHRINKS -- process_sbrk() says so in its own
 * comment -- so a long-running program's footprint is its high-water mark
 * forever, and there was no way for userspace to obtain memory with chosen
 * PERMISSIONS at all. No guard pages, no W^X region for generated code, no way
 * to hand a page back.
 *
 * WHY A VMA LIST AND NOT JUST PAGE TABLES. The page tables know a page is
 * mapped; they do not know it was the third page of a twelve-page mapping the
 * caller will later unmap as a unit, and they cannot be asked "is this range
 * free". A munmap() implemented by walking page tables would happily tear a
 * hole in the middle of somebody else's mapping. So each process keeps a
 * sorted list of the ranges it asked for, and the page tables remain what they
 * are: the hardware's opinion, derived from this.
 *
 * EAGER, NOT DEMAND-PAGED. mmap() allocates and zeroes every page up front.
 * Demand paging needs a fault handler that can distinguish "not mapped yet"
 * from "not yours", plus a page cache to make file-backed mappings worth
 * having; that is the next piece of work and is recorded in docs/TODO.md, not
 * half-built here. The consequence is honest and worth stating: a large
 * mapping costs its full size immediately.
 */

/* prot -- the same bit values POSIX uses, so a userland wrapper is a pass. */
#define PROT_NONE   0x0
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4

/* flags. MAP_PRIVATE and MAP_ANONYMOUS are the only combination implemented;
 * anything else is refused rather than silently ignored. */
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

/* The mmap window. Deliberately its own region, clear of everything else in
 * the user address space:
 *
 *   0x0000_0000_0040_0000  the program image
 *   0x0000_5000_0000_0000  THIS -- mmap, growing up
 *   0x0000_6000_0000_0000  the sbrk heap (1 GiB)
 *   0x0000_7000_0000_0000  the main thread stack, growing down
 *   0x0000_7000_1000_0000  per-thread stacks, 1 MiB apart
 *
 * Separate windows rather than one arena because a collision between two of
 * them is silent: the second allocation simply overwrites the first's pages
 * and the program corrupts itself somewhere else entirely. */
#define USER_MMAP_BASE  0x0000500000000000ULL
#define USER_MMAP_MAX   (USER_MMAP_BASE + 0x0000004000000000ULL)   /* 256 GiB of VA */

struct process;

struct vm_area {
    uint64_t start;         /* inclusive, page aligned */
    uint64_t end;           /* exclusive, page aligned */
    uint32_t prot;
    uint32_t flags;
    struct vm_area *next;   /* sorted ascending by start */
};

/* Map `len` bytes. Returns the base address, or a negative -EMBK_* code.
 * `addr` is a hint and is honoured only with MAP_FIXED. */
int64_t vma_mmap(struct process *proc, uint64_t addr, uint64_t len,
                 uint32_t prot, uint32_t flags);

/* Unmap a whole mapping, or a page-aligned prefix/suffix/middle of one.
 * Returns 0 or -EMBK_*. Unmapping a range that was never mapped is an error,
 * not a no-op: it is far more often a bug than an idempotent cleanup. */
int vma_munmap(struct process *proc, uint64_t addr, uint64_t len);

/* Change the permissions of an already-mapped range, keeping its contents.
 * Returns 0 or -EMBK_*; -EMBK_ENOMEM if any page in the range is not mapped
 * (POSIX's answer), checked before anything changes so a refusal is inert.
 * The range need not line up with a mapping: VMAs are split at both ends.
 *
 * THIS IS WHAT MAKES W^X A RULE INSTEAD OF AN OBSTACLE. mmap refuses
 * PROT_WRITE|PROT_EXEC, and without mprotect that refusal would simply mean
 * generated code is impossible. With it, the sequence a JIT actually wants
 * works and the dangerous state never exists: map PROT_READ|PROT_WRITE, emit
 * the code, mprotect to PROT_READ|PROT_EXEC, execute. Holding both at once is
 * still refused here.
 *
 * PROT_NONE is real: the frame stays allocated and owned, and every access
 * from userspace faults. That is a guard page. */
int vma_mprotect(struct process *proc, uint64_t addr, uint64_t len, uint32_t prot);

/* Release every mapping a process owns. The page TABLES are torn down by
 * vmm_destroy_address_space(); this frees the bookkeeping and is what stops
 * the list itself leaking. */
void vma_destroy_all(struct process *proc);

/* Bytes currently mapped, for diagnostics and the boot self-test. */
uint64_t vma_total_bytes(struct process *proc);

#endif /* _VMA_H_ */
