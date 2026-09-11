#ifndef _VMA_H_
#define _VMA_H_

#include <stdint.h>
#include "include/types.h"

struct vm_object;   /* mm/vm_object.h -- a file's pages */

/* A mapping being UNMAPPED. It stays on the list -- so the range is not handed
 * out again, and a fault there is declined -- while its pages are dealt with
 * OUTSIDE vma_lock: that work talks to the page cache, whose lock sleeps, and
 * vma_lock is a spinlock taken in the fault path with interrupts off. Set in
 * `flags`, above every MAP_* bit; never visible to userland. */
#define VMA_DYING 0x80000000u

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
 * DEMAND-PAGED. mmap() reserves ADDRESS SPACE and allocates nothing; a page
 * appears the first time it is touched, and the fault that puts it there is
 * resolved rather than reported.
 *
 * This used to be eager, and the comment here said why: "demand paging needs a
 * fault handler that can distinguish 'not mapped yet' from 'not yours'". That
 * distinction is exactly what this list is -- a VMA is the record that says an
 * address is legitimately the caller's before any page exists there -- so the
 * fault handler was one lookup away the whole time. See vm_fault() below.
 *
 * The difference is not a micro-optimisation. Eager allocation means the size
 * of a mapping is the cost of a mapping, so a program that maps a gigabyte to
 * use three pages of it pays for a gigabyte, and a machine with 512 MiB simply
 * cannot run it. Demand paging is what makes address space and memory two
 * different resources.
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

    /* THE OBJECT BEHIND THE MAPPING. A file's page object, or -- for an
     * anonymous mapping -- an object with no file that exists so the memory
     * can be paged out (vmo_create_anon). Never NULL once the mapping is made.
     *
     * For a file, the object is the SAME one read()/write() go through (mm/vm_object.h),
     * not a copy of it -- which is the entire reason the cache was built as an
     * object. A process that maps a file and one that reads it are looking at
     * one set of pages, so they cannot disagree about what the file says.
     *
     * `file_off` is the byte offset in the file that `start` corresponds to,
     * so page N of the mapping is page (file_off/PAGE + N) of the object. */
    struct vm_object *obj;
    uint64_t file_off;

    struct vm_area *next;   /* sorted ascending by start */
    struct vm_area *dying_next;  /* munmap's private chain of VMA_DYING nodes */
};

/* Map `len` bytes. Returns the base address, or a negative -EMBK_* code.
 * `addr` is a hint and is honoured only with MAP_FIXED. */
int64_t vma_mmap(struct process *proc, uint64_t addr, uint64_t len,
                 uint32_t prot, uint32_t flags);

/* Map a FILE. `obj` is the file's page object -- the caller holds a reference
 * for the life of the mapping and vma_munmap/vma_destroy_all release it.
 * `file_off` must be page aligned: a mapping is made of pages, and an
 * unaligned offset has no page to correspond to.
 *
 * MAP_SHARED means writes go to the FILE, through the same writeback the rest
 * of the kernel uses. MAP_PRIVATE means writes are the caller's alone: the
 * page is mapped read-only from the cache and COPIED on the first write, so
 * two processes reading one file share its pages and only a writer pays. */
int64_t vma_mmap_file(struct process *proc, uint64_t len, uint32_t prot,
                      uint32_t flags, struct vm_object *obj, uint64_t file_off);

/* Unmap a whole mapping, or a page-aligned prefix/suffix/middle of one.
 * Returns 0 or -EMBK_*. Unmapping a range that was never mapped is an error,
 * not a no-op: it is far more often a bug than an idempotent cleanup. */
int vma_munmap(struct process *proc, uint64_t addr, uint64_t len);

/* RESOLVE A FAULT, or decline it.
 *
 * Called from each architecture's fault handler before it decides the program
 * is broken. Returns true if the fault was HANDLED -- a page has been mapped
 * and the faulting instruction should be retried -- and false if this address
 * is genuinely not the process's, which is when the program dies.
 *
 * `write` is whether the access was a store, and `exec` whether it was an
 * instruction fetch. Both matter because a VMA carries permissions: touching a
 * PROT_READ page for writing is not a missing page, it is a protection
 * violation, and resolving it by mapping a writable page would silently grant
 * what the caller was refused.
 *
 * This is the function that separates "a kernel that reports faults" from "a
 * kernel that uses them". Everything a modern VM does -- demand paging, copy
 * on write, growing a stack, mapping a file, swapping -- is a different answer
 * from this one place. */
bool vm_fault(struct process *proc, uint64_t addr, bool write, bool exec);

/* The top of the user half. An address at or above this belongs to the kernel
 * and can never have a VMA, so a fault there is always a real bug. Defined
 * here because the fault handlers need it and both architectures agree: 47
 * bits of user VA, the same split the page tables already use. */
#define USER_VA_LIMIT 0x0000800000000000ULL

/* How many faults this kernel has resolved, and how many it declined. The
 * numbers `test mmap` asserts against: a demand-paged mapping that reported
 * zero faults never faulted, which means it was not demand-paged. */
struct vm_fault_stats {
    uint64_t handled;      /* a page was mapped and the instruction retried */
    uint64_t declined;     /* not this process's address, or a permission
                            * violation -- the program dies */
    uint64_t cow;          /* a private file page copied because it was
                            * written -- the count that says sharing was
                            * actually happening up to that point */
    uint64_t ns;           /* wall time inside vm_fault, all outcomes -- the
                            * number that says whether a slow paging run is
                            * the disk, the fault path, or the machine */
    uint64_t wire_ns;      /* ... of which, getting the page from its object
                            * (cache lock, fill or swap-in, disk waits) */
    uint64_t map_ns;       /* ... of which, installing the PTE */
};
void vm_fault_stats(struct vm_fault_stats *out);

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

/* TEARDOWN IN TWO HALVES, because the reap path holds the scheduler lock and
 * an object letting go of its pages takes the page cache's sleeping lock.
 * `vma_detach_all` takes the list off the process -- no lock: a process being
 * reaped has no threads, so nothing can be walking it -- and `vma_destroy_list`
 * finishes the job from the kworker, where blocking is fine. `pml4_phys` is
 * passed explicitly because the process slot is gone by then. vma_destroy_all
 * is the two in one, for callers in process context (a process_create that
 * fails halfway). */
struct vm_area *vma_detach_all(struct process *proc);
void vma_destroy_list(struct vm_area *list, uint64_t pml4_phys);

/* --- KERNEL-PLACED ANONYMOUS MAPPINGS: the sbrk heap and the stacks ------
 *
 * Until these existed, the heap and every stack were bare frames mapped
 * eagerly at creation -- 512 KiB per process for the main stack whether or
 * not it was used, and heap pages the fault handler never saw, so none of it
 * could be paged out, and the stack pages were handed over UNZEROED (a frame
 * recycled from another process arrived with that process's data). Now they
 * are mappings like any other: an anonymous object each, demand-paged, zero
 * on first touch, swappable, and returned exactly on teardown. */

/* Place an anonymous private mapping at exactly [start, start+len) -- outside
 * the mmap window, which is why it is not vma_mmap with MAP_FIXED. EEXIST if
 * anything is there. */
int vma_map_anon_at(struct process *proc, uint64_t start, uint64_t len, uint32_t prot);

/* Move the heap's mapped end from `old_end` to `new_end` (both page-aligned,
 * inside the heap window). Growth extends the mapping that ends at old_end or
 * places a new one; shrinking unmaps [new_end, old_end) and frees its pages
 * and swap slots -- the old sbrk never gave memory back. */
int vma_heap_set_end(struct process *proc, uint64_t old_end, uint64_t new_end);

/* Fault a page in NOW and return its frame: for the kernel writing a process's
 * initial stack (argv, envp) before the process has ever run. 0 on failure. */
uint64_t vma_prefault(struct process *proc, uint64_t va);

/* Bytes currently mapped, for diagnostics and the boot self-test. */
uint64_t vma_total_bytes(struct process *proc);

#endif /* _VMA_H_ */
